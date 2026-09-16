#include "record/remuxer.h"

#include <algorithm>

#include "i18n.h"
#include "record/ffmpeg_locator.h"

namespace cap {
namespace {

bool FileExists(const std::wstring& path) {
  const DWORD attrs = ::GetFileAttributesW(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring FileNameOf(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// Same name, .mp4 instead. Nothing is ever overwritten: a batch job that eats
// the original the user meant to keep is not a job anyone runs twice.
std::wstring MakeOutputName(const std::wstring& input) {
  const size_t dot = input.find_last_of(L'.');
  const size_t slash = input.find_last_of(L"\\/");
  const bool hasExt = dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash);
  const std::wstring stem = hasExt ? input.substr(0, dot) : input;

  std::wstring candidate = stem + L".mp4";
  for (int n = 1; FileExists(candidate) && n < 1000; ++n) {
    candidate = stem + L" (" + std::to_wstring(n) + L").mp4";
  }
  return candidate;
}

// Digs the useful line out of ffmpeg's stderr. The interesting failure is a
// codec MP4 cannot carry, and ffmpeg says so in a sentence worth passing on
// rather than burying under an exit code.
std::string ExplainFailure(const std::string& stderrText, DWORD exitCode) {
  if (stderrText.find("Could not find tag for codec") != std::string::npos ||
      stderrText.find("codec not currently supported in container") != std::string::npos) {
    return T("Dieses Format passt nicht in eine MP4. Nur Umpacken geht hier nicht, das müsste "
             "neu kodiert werden.",
             "This format does not fit in an MP4. Rewrapping cannot help; it would have to be "
             "re-encoded.");
  }
  if (stderrText.find("Invalid data found") != std::string::npos) {
    return T("Die Datei ist beschädigt oder keine Videodatei.",
             "The file is damaged or not a video file.");
  }
  if (stderrText.find("No such file") != std::string::npos) {
    return T("Datei nicht gefunden.", "File not found.");
  }

  // Otherwise hand back ffmpeg's last line, which is usually the actual reason.
  std::string last;
  size_t pos = 0;
  while (pos < stderrText.size()) {
    const size_t end = stderrText.find_first_of("\r\n", pos);
    const std::string line =
        stderrText.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    if (!line.empty()) last = line;
    if (end == std::string::npos) break;
    pos = end + 1;
  }
  if (!last.empty()) return last;
  return Format(T("ffmpeg brach ab (Code %lu).", "ffmpeg failed (code %lu)."),
                (unsigned long)exitCode);
}

}  // namespace

Remuxer::~Remuxer() {
  Cancel();
  if (thread_.joinable()) thread_.join();
}

bool Remuxer::Start(const std::wstring& ffmpegPath, const std::vector<std::wstring>& inputs) {
  if (busy() || inputs.empty() || ffmpegPath.empty()) return false;
  if (thread_.joinable()) thread_.join();

  {
    std::lock_guard<std::mutex> lock(mutex_);
    items_.clear();
    for (const std::wstring& input : inputs) {
      Item item;
      item.input = input;
      items_.push_back(std::move(item));
    }
    message_.clear();
  }
  cancel_.store(false, std::memory_order_relaxed);
  progress_.store(0.0f, std::memory_order_relaxed);
  done_.store(0, std::memory_order_relaxed);
  ok_.store(0, std::memory_order_relaxed);
  state_.store(State::Running, std::memory_order_relaxed);

  thread_ = std::thread(&Remuxer::Run, this, ffmpegPath);
  return true;
}

void Remuxer::Cancel() { cancel_.store(true, std::memory_order_relaxed); }

std::string Remuxer::message() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return message_;
}

std::vector<Remuxer::Item> Remuxer::items() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return items_;
}

void Remuxer::SetMessage(const std::string& text) {
  std::lock_guard<std::mutex> lock(mutex_);
  message_ = text;
}

void Remuxer::Run(std::wstring ffmpegPath) {
  size_t count = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    count = items_.size();
  }

  for (size_t i = 0; i < count; ++i) {
    if (cancel_.load(std::memory_order_relaxed)) {
      SetMessage(T("Abgebrochen.", "Cancelled."));
      state_.store(State::Failed, std::memory_order_relaxed);
      return;
    }

    std::wstring input;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      input = items_[i].input;
    }
    const std::wstring output = MakeOutputName(input);

    SetMessage(Format(T("%zu von %zu: %s", "%zu of %zu: %s"), i + 1, count,
                      ToUtf8(FileNameOf(input)).c_str()));

    // -c copy is the whole point: the streams are moved, not re read. faststart
    // then pulls the index to the front of the file, which is what a player
    // needs to seek before the download finished.
    const std::wstring args = L"-hide_banner -loglevel error -y -i \"" + input +
                              L"\" -map 0 -c copy -movflags +faststart \"" + output + L"\"";

    std::string captured;
    DWORD exitCode = 0;
    const bool started =
        RunFfmpeg(ToUtf8(ffmpegPath), args, &captured, &exitCode, 30 * 60 * 1000);

    const bool good = started && exitCode == 0 && FileExists(output);
    Said failure;
    if (!good) {
      failure = started ? CAP_SAID(ExplainFailure(captured, exitCode))
                        : CAP_SAID(T("ffmpeg konnte nicht gestartet werden.",
                                     "ffmpeg could not be started."));
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      items_[i].output = output;
      items_[i].done = true;
      items_[i].ok = good;
      if (!good) items_[i].error = failure.shown;
    }
    if (good) {
      ok_.fetch_add(1, std::memory_order_relaxed);
      CAP_LOG("Remuxed: %s", ToUtf8(FileNameOf(output)).c_str());
    } else {
      // A half written MP4 is worse than none: it looks like a result.
      ::DeleteFileW(output.c_str());
      LogWrite("ERR ", "Remux failed: %s -- %s", ToUtf8(FileNameOf(input)).c_str(),
               failure.logged.c_str());
    }

    done_.fetch_add(1, std::memory_order_relaxed);
    progress_.store((float)(i + 1) / (float)count, std::memory_order_relaxed);
  }

  const int good = ok_.load(std::memory_order_relaxed);
  if (good == (int)count) {
    SetMessage(count == 1 ? T("Fertig.", "Done.")
                          : Format(T("%d Dateien umgepackt.", "%d files rewrapped."), good));
    state_.store(State::Done, std::memory_order_relaxed);
  } else {
    SetMessage(Format(T("%d von %zu umgepackt, der Rest ist fehlgeschlagen.",
                        "%d of %zu rewrapped, the rest failed."),
                      good, count));
    state_.store(good > 0 ? State::Done : State::Failed, std::memory_order_relaxed);
  }
}

}  // namespace cap
