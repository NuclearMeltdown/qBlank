#include "record/ffmpeg_download.h"

#include <cctype>
#include <vector>

#include "app_files.h"
#include "archive.h"
#include "files.h"
#include "http.h"
#include "i18n.h"
#include "json.h"
#include "record/ffmpeg_locator.h"
#include "record/libav.h"
#include "update/release_source.h"

namespace cap {
namespace {

// The release line the headers in third_party/ffmpeg belong to. A new major
// means new headers, and this number moves with them.
constexpr int kFfmpegMajor = 9;
static_assert(LIBAVCODEC_VERSION_MAJOR == 63, "kFfmpegMajor goes with the headers");

const char kRepository[] = "https://api.github.com/repos/GyanD/codexffmpeg";

// What a download unpacks into, and the file that says it finished.
const char kStaging[] = "new";
const char kComplete[] = "complete";

struct Package {
  std::string version;  // the tag, "9.0.2"
  std::string url;
  std::string sha256;   // lowercase hex, empty when the release names none
  uint64_t size = 0;
};

std::string FetchText(FetchError error, int status) {
  switch (error) {
    case FetchError::NoNetwork: return T("keine Netzwerkverbindung", "no network connection");
    case FetchError::NoServer: return T("GitHub nicht erreichbar", "GitHub could not be reached");
    case FetchError::HttpStatus: return Format(T("GitHub antwortete mit %d", "GitHub returned %d"), status);
    case FetchError::Unreadable: return T("Antwort nicht lesbar", "the answer could not be read");
    default: return T("keine Antwort", "no answer");
  }
}

bool IsReleaseNumber(const std::string& text) {
  if (text.empty() || !isdigit((unsigned char)text[0])) return false;
  for (char c : text) {
    if (!isdigit((unsigned char)c) && c != '.') return false;
  }
  return true;
}

// The newest release of kFfmpegMajor: its tags are plain numbers, "9.0.2".
bool NewestTag(std::string* tag, FetchError* error, int* status, const std::atomic<bool>* cancel) {
  std::string body;
  if (!FetchUrl(Format("%s/git/matching-refs/tags/%d.", kRepository, kFfmpegMajor), true, &body,
                error, status, cancel)) {
    return false;
  }
  const json::Value refs = json::Parse(body);
  const char kPrefix[] = "refs/tags/";
  for (size_t i = 0; i < refs.Size(); ++i) {
    const std::string ref = refs.At(i)["ref"].AsString();
    if (ref.compare(0, sizeof(kPrefix) - 1, kPrefix) != 0) continue;
    const std::string name = ref.substr(sizeof(kPrefix) - 1);
    if (IsReleaseNumber(name) && (tag->empty() || IsNewerFfmpeg(name, *tag))) *tag = name;
  }
  if (tag->empty()) *error = FetchError::Unreadable;
  return !tag->empty();
}

// The shared build in that release, as 7z or zip. GitHub publishes a SHA-256
// for every file it holds, as "sha256:<hex>".
bool FindPackage(const std::string& tag, bool sevenZip, Package* out, FetchError* error,
                 int* status, const std::atomic<bool>* cancel) {
  std::string body;
  if (!FetchUrl(Format("%s/releases/tags/%s", kRepository, tag.c_str()), true, &body, error, status,
                cancel)) {
    return false;
  }
  const json::Value release = json::Parse(body);
  const std::string wanted = "ffmpeg-" + tag + "-full_build-shared" + (sevenZip ? ".7z" : ".zip");
  const json::Value& assets = release["assets"];
  for (size_t i = 0; i < assets.Size(); ++i) {
    const json::Value& asset = assets.At(i);
    if (asset["name"].AsString() != wanted) continue;
    out->version = tag;
    out->url = asset["browser_download_url"].AsString();
    out->size = (uint64_t)asset["size"].AsNumber();
    std::string digest = asset["digest"].AsString();
    if (digest.compare(0, 7, "sha256:") == 0) {
      digest = digest.substr(7);
      for (char& c : digest) c = (char)tolower((unsigned char)c);
      out->sha256 = digest;
    }
    return !out->url.empty();
  }
  *error = FetchError::Unreadable;
  return false;
}

HttpRequest Ask(const std::string& url) {
  HttpRequest request;
  request.url = url;
  request.userAgent = AppNameUtf8();
  // A download of this size needs longer than the default to get going.
  request.timeoutMs = 30000;
  return request;
}

void RemoveStaging(const std::filesystem::path& folder) {
  const std::filesystem::path staging = folder / kStaging;
  if (!PathExists(staging)) return;
  for (const std::string& name : LibavFileNames()) RemoveFile(staging / name);
  RemoveFile(staging / "ffmpeg.exe");  // what the downloader of earlier versions left
  RemoveFile(staging / kComplete);
  RemoveFile(staging);  // takes an empty folder as well
}

// The staged set over the installed one, all or nothing. A library in use can
// be renamed, not written over: the old one steps aside first, and goes once
// nothing has it open.
bool MoveIn(const std::filesystem::path& folder) {
  const std::filesystem::path staging = folder / kStaging;
  const std::vector<std::string> names = LibavFileNames();
  for (const std::string& name : names) {
    if (!IsFile(staging / name)) return false;
  }
  size_t moved = 0;
  for (; moved < names.size(); ++moved) {
    const std::filesystem::path target = folder / names[moved];
    const std::filesystem::path old = folder / (names[moved] + ".old");
    const bool had = IsFile(target);
    if (had && !RenameOver(target, old)) break;
    if (!RenameOver(staging / names[moved], target)) {
      if (had) RenameOver(old, target);
      break;
    }
  }
  if (moved == names.size()) return true;
  while (moved-- > 0) {
    const std::filesystem::path target = folder / names[moved];
    const std::filesystem::path old = folder / (names[moved] + ".old");
    RenameOver(target, staging / names[moved]);
    if (IsFile(old)) RenameOver(old, target);
  }
  return false;
}

void RemoveLeftovers(const std::filesystem::path& folder) {
  // Fails while another instance still has one open; the next start takes it.
  for (const std::string& name : LibavFileNames()) RemoveFile(folder / (name + ".old"));
  RemoveFile(folder / "ffmpeg.exe.old");
  // The program earlier versions ran, once the libraries have taken its place.
  if (LibavFilesPresent(folder)) RemoveFile(folder / "ffmpeg.exe");
}

}  // namespace

void FfmpegDownloader::InstallPending(const std::filesystem::path& targetFolder) {
  const std::filesystem::path staging = targetFolder / kStaging;
  if (IsFile(staging / kComplete)) {
    std::string version;
    ReadWholeFile(staging / kComplete, &version, 64);
    if (MoveIn(targetFolder)) {
      CAP_LOG("ffmpeg %s moved in", Trim(version).c_str());
    } else {
      CAP_WARN("ffmpeg %s could not be moved in, the download is discarded", Trim(version).c_str());
    }
  }
  RemoveStaging(targetFolder);
  RemoveLeftovers(targetFolder);
}

FfmpegDownloader::~FfmpegDownloader() {
  Cancel();
  if (thread_.joinable()) thread_.join();
}

void FfmpegDownloader::Cancel() {
  cancel_.store(true, std::memory_order_relaxed);
}

std::string FfmpegDownloader::message() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return message_;
}

std::string FfmpegDownloader::remoteVersion() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return remoteVersion_;
}

std::string FfmpegDownloader::installedVersion() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return installedVersion_;
}

void FfmpegDownloader::SetMessage(const std::string& text) {
  std::lock_guard<std::mutex> lock(mutex_);
  message_ = text;
}

bool FfmpegDownloader::Start(const std::filesystem::path& targetFolder) {
  if (busy()) return false;
  if (thread_.joinable()) thread_.join();
  cancel_.store(false, std::memory_order_relaxed);
  progress_.store(-1.0f, std::memory_order_relaxed);
  state_.store(State::Running, std::memory_order_relaxed);
  thread_ = std::thread(&FfmpegDownloader::Run, this, targetFolder, false);
  return true;
}

bool FfmpegDownloader::StartVersionCheck(const std::string& installed, bool announce) {
  if (busy()) return false;
  if (thread_.joinable()) thread_.join();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    installedVersion_ = installed;
  }
  announce_.store(announce, std::memory_order_relaxed);
  cancel_.store(false, std::memory_order_relaxed);
  state_.store(State::Running, std::memory_order_relaxed);
  thread_ = std::thread(&FfmpegDownloader::Run, this, std::filesystem::path(), true);
  return true;
}

void FfmpegDownloader::Run(std::filesystem::path targetFolder, bool versionOnly) {
  auto finish = [&](bool ok, const Said& said) {
    if (!ok && !said.logged.empty()) CAP_ERR("ffmpeg download: %s", said.logged.c_str());
    SetMessage(said.shown);
    state_.store(ok ? State::Done : State::Failed, std::memory_order_relaxed);
  };

  // --- which release, and which file of it ---
  SetMessage(T("Version wird abgefragt ...", "Checking the version ..."));
  FetchError fetchError = FetchError::None;
  int status = 0;
  std::string tag;
  Package package;
  // The 7z is 57 MB against 95, where the system can unpack it.
  const bool sevenZip = !versionOnly && ArchiveReads7z();
  if (!NewestTag(&tag, &fetchError, &status, &cancel_) ||
      !FindPackage(tag, sevenZip, &package, &fetchError, &status, &cancel_)) {
    finish(false, CAP_SAID(std::string(T("Version konnte nicht ermittelt werden: ",
                                         "Could not determine the version: ")) +
                           FetchText(fetchError, status) + "."));
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    remoteVersion_ = package.version;
  }
  const std::string& version = package.version;
  if (versionOnly) {
    const std::string installed = installedVersion();
    const bool newer = IsNewerFfmpeg(version, installed);
    updateAvailable_.store(newer, std::memory_order_relaxed);
    if (newer) {
      CAP_LOG("ffmpeg update available: %s (installed %s)", version.c_str(), installed.c_str());
      finish(true, CAP_SAID(Format(T("ffmpeg %s ist verfügbar, installiert ist %s.",
                                     "ffmpeg %s is available, %s is installed."),
                                   version.c_str(), installed.c_str())));
    } else if (!installed.empty()) {
      finish(true, CAP_SAID(Format(T("Aktuell (neueste ist %s).", "Up to date (newest is %s)."),
                                   version.c_str())));
    } else {
      finish(true, CAP_SAID(T("Neueste Version: ", "Latest version: ") + version));
    }
    return;
  }

  // --- download ---
  SetMessage(T("Lade ffmpeg herunter ...", "Downloading ffmpeg ..."));
  const std::filesystem::path archive =
      TempFolder() / (sevenZip ? "qblank_ffmpeg.7z" : "qblank_ffmpeg.zip");
  {
    FileWriter out;
    if (!out.Open(archive)) {
      finish(false, CAP_SAID(T("Temporäre Datei konnte nicht angelegt werden.",
                               "Could not create the temporary file.")));
      return;
    }

    uint64_t received = 0;
    HttpResponse response;
    // Straight to the file as it arrives: a hundred megabytes has no business
    // sitting in memory first. Answering false is how Cancel gets out of here.
    const bool ok = HttpGet(Ask(package.url), &response,
                            [&](const void* data, size_t size) {
                              if (cancel_.load(std::memory_order_relaxed)) return false;
                              if (!out.Write(data, size)) return false;
                              received += size;
                              const uint64_t total =
                                  response.contentLength > 0 ? response.contentLength : package.size;
                              if (total > 0) {
                                progress_.store((float)((double)received / (double)total));
                              }
                              SetMessage(
                                  Format(T("Lade ffmpeg ... %.1f MB", "Downloading ffmpeg ... %.1f MB"),
                                         (double)received / (1024.0 * 1024.0)));
                              return true;
                            },
                            nullptr);
    const bool written = out.Close();

    if (!ok || !written || received == 0) {
      RemoveFile(archive);
      finish(false, CAP_SAID(cancel_.load(std::memory_order_relaxed)
                                 ? T("Abgebrochen.", "Cancelled.")
                                 : T("Download fehlgeschlagen.", "The download failed.")));
      return;
    }
  }

  // --- verify ---
  if (!package.sha256.empty()) {
    SetMessage(T("Prüfe SHA-256 ...", "Verifying SHA-256 ..."));
    const std::string actual = Sha256HexOfFile(archive);
    if (actual.empty() || actual != package.sha256) {
      RemoveFile(archive);
      finish(false, CAP_SAID(T("Prüfsumme stimmt nicht — Download verworfen.",
                               "Checksum mismatch — the download was discarded.")));
      return;
    }
  } else {
    CAP_WARN("ffmpeg download: the release names no checksum, extracting unchecked");
  }

  // --- extract ---
  // Into a folder of its own first. Only the four libraries, two folders deep
  // in every one of these archives: ffmpeg-<version>-full_build-shared/bin/.
  SetMessage(T("Entpacke ...", "Extracting ..."));
  RemoveStaging(targetFolder);
  const std::filesystem::path staging = targetFolder / kStaging;
  EnsureFolder(staging);
  const std::vector<std::string> names = LibavFileNames();
  std::vector<std::string> members;
  for (const std::string& name : names) members.push_back("*/bin/" + name);
  std::string extractError;
  const bool extracted = ExtractFromArchive(archive, members, 2, staging, &extractError);
  RemoveFile(archive);
  if (!extracted) {
    RemoveStaging(targetFolder);
    finish(false, Relayed(extractError));
    return;
  }
  for (const std::string& name : names) {
    if (!IsFile(staging / name)) {
      RemoveStaging(targetFolder);
      finish(false, CAP_SAID(Format(T("%s war nicht im Archiv.", "%s was not in the archive."),
                                    name.c_str())));
      return;
    }
  }
  WriteWholeFile(staging / kComplete, version.data(), version.size());

  // --- move in ---
  // At once, unless this process already has the old ones open: then at the
  // next start.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    installedVersion_ = version;
  }
  updateAvailable_.store(false, std::memory_order_relaxed);
  progress_.store(1.0f);
  if (!Av() && MoveIn(targetFolder)) {
    RemoveStaging(targetFolder);
    RemoveLeftovers(targetFolder);
    installed_.store(true, std::memory_order_relaxed);
    CAP_LOG("ffmpeg %s downloaded to %s", version.c_str(), PathToUtf8(targetFolder).c_str());
    finish(true, CAP_SAID(Format(T("ffmpeg %s ist bereit.", "ffmpeg %s is ready."), version.c_str())));
    return;
  }
  pending_.store(true, std::memory_order_relaxed);
  CAP_LOG("ffmpeg %s downloaded, moves in at the next start", version.c_str());
  finish(true, CAP_SAID(Format(T("ffmpeg %s ist geladen und gilt ab dem nächsten Start.",
                                 "ffmpeg %s is downloaded and takes over at the next start."),
                               version.c_str())));
}

}  // namespace cap
