#include "record/remuxer.h"

#include <algorithm>
#include <cerrno>
#include <functional>

#include "files.h"
#include "i18n.h"
#include "record/libav.h"

namespace cap {
namespace {

// Same name, .mp4 instead. Nothing is ever overwritten: a batch job that eats
// the original the user meant to keep is not a job anyone runs twice.
std::filesystem::path MakeOutputName(const std::filesystem::path& input) {
  std::filesystem::path candidate = input;
  candidate.replace_extension(".mp4");

  const std::filesystem::path folder = input.parent_path();
  const std::string stem = PathToUtf8(input.stem());
  for (int n = 1; IsFile(candidate) && n < 1000; ++n) {
    candidate = folder / Utf8ToPath(stem + " (" + std::to_string(n) + ").mp4");
  }
  return candidate;
}

// Checked by ffmpeg while it reads and writes, so a cancel does not have to sit
// out the rest of a long file.
int Interrupted(void* cancel) {
  return ((std::atomic<bool>*)cancel)->load(std::memory_order_relaxed) ? 1 : 0;
}

// One file, input to output. Empty when it worked; otherwise why not.
// `fraction` follows the read position through the input.
Said RemuxOne(const Libav& av, const std::filesystem::path& input,
              const std::filesystem::path& output, std::atomic<bool>& cancel,
              const std::function<void(float)>& fraction) {
  LibavClearError();
  LibavQuiet quiet;
  AVFormatContext* in = av.avformat_alloc_context();
  AVFormatContext* out = nullptr;
  AVPacket* packet = av.av_packet_alloc();
  bool outOpen = false;
  auto finish = [&](Said said) {
    av.av_packet_free(&packet);
    if (out) {
      if (outOpen) av.avio_closep(&out->pb);
      av.avformat_free_context(out);
    }
    av.avformat_close_input(&in);
    return said;
  };
  auto failed = [&](int code) { return finish(CAP_SAID(LibavError(code))); };
  if (!in || !packet) return finish(CAP_SAID(T("Kein Speicher.", "Out of memory.")));
  in->interrupt_callback.callback = &Interrupted;
  in->interrupt_callback.opaque = &cancel;

  const std::string inputName = PathToUtf8(input);
  int result = av.avformat_open_input(&in, inputName.c_str(), nullptr, nullptr);
  if (result < 0) {
    in = nullptr;  // freed by the failed open
    if (result == AVERROR(ENOENT)) {
      return finish(CAP_SAID(T("Datei nicht gefunden.", "File not found.")));
    }
    if (result == AVERROR_INVALIDDATA) {
      return finish(CAP_SAID(T("Die Datei ist beschädigt oder keine Videodatei.",
                               "The file is damaged or not a video file.")));
    }
    return failed(result);
  }
  if ((result = av.avformat_find_stream_info(in, nullptr)) < 0) return failed(result);

  const std::string outputName = PathToUtf8(output);
  if ((result = av.avformat_alloc_output_context2(&out, nullptr, "mp4", outputName.c_str())) < 0) {
    return failed(result);
  }
  out->interrupt_callback = in->interrupt_callback;
  av.av_dict_copy(&out->metadata, in->metadata, 0);

  // Every stream, copied as it is. A codec MP4 has no tag for is said up front
  // in a sentence of our own: ffmpeg's would only name the codec.
  for (unsigned i = 0; i < in->nb_streams; ++i) {
    const AVStream* source = in->streams[i];
    const AVCodecID codec = source->codecpar->codec_id;
    if (av.avformat_query_codec(out->oformat, codec, FF_COMPLIANCE_NORMAL) != 1) {
      // qBlank's own lossless AVIs, which are where they belong.
      if (codec == AV_CODEC_ID_UTVIDEO || codec == AV_CODEC_ID_RAWVIDEO) {
        return finish(CAP_SAID(T("UT Video und unkomprimierte Aufnahmen passen nicht in eine "
                                 "MP4. Schnittprogramme lesen das AVI direkt.",
                                 "UT Video and uncompressed recordings do not fit in an MP4. "
                                 "Editors read the AVI as it is.")));
      }
      return finish(CAP_SAID(T("Dieses Format passt nicht in eine MP4. Nur Umpacken geht hier "
                               "nicht, das müsste neu kodiert werden.",
                               "This format does not fit in an MP4. Rewrapping cannot help; it "
                               "would have to be re-encoded.")));
    }
    AVStream* target = av.avformat_new_stream(out, nullptr);
    if (!target) return finish(CAP_SAID(T("Kein Speicher.", "Out of memory.")));
    if ((result = av.avcodec_parameters_copy(target->codecpar, source->codecpar)) < 0) {
      return failed(result);
    }
    // The tag is the input container's; MP4 picks its own.
    target->codecpar->codec_tag = 0;
    target->time_base = source->time_base;
    target->avg_frame_rate = source->avg_frame_rate;
    target->disposition = source->disposition;
    av.av_dict_copy(&target->metadata, source->metadata, 0);
  }

  if ((result = av.avio_open2(&out->pb, outputName.c_str(), AVIO_FLAG_WRITE,
                              &out->interrupt_callback, nullptr)) < 0) {
    return failed(result);
  }
  outOpen = true;
  // faststart pulls the index to the front of the file once it is written,
  // which is what a player needs to start before the whole file has arrived.
  AVDictionary* options = nullptr;
  av.av_dict_set(&options, "movflags", "+faststart", 0);
  result = av.avformat_write_header(out, &options);
  av.av_dict_free(&options);
  if (result < 0) return failed(result);

  const int64_t size = av.avio_size(in->pb);
  while ((result = av.av_read_frame(in, packet)) >= 0) {
    const AVStream* source = in->streams[packet->stream_index];
    const AVStream* target = out->streams[packet->stream_index];
    if (size > 0 && packet->pos > 0) fraction((float)((double)packet->pos / (double)size));
    av.av_packet_rescale_ts(packet, source->time_base, target->time_base);
    packet->pos = -1;
    // Takes the packet over, ours or not.
    result = av.av_interleaved_write_frame(out, packet);
    if (result < 0) return failed(result);
  }
  if (result != AVERROR_EOF) return failed(result);
  if ((result = av.av_write_trailer(out)) < 0) return failed(result);
  return finish(Said());
}

}  // namespace

Remuxer::~Remuxer() {
  Cancel();
  if (thread_.joinable()) thread_.join();
}

bool Remuxer::Start(const std::vector<std::filesystem::path>& inputs) {
  if (busy() || inputs.empty() || !Av()) return false;
  if (thread_.joinable()) thread_.join();

  {
    std::lock_guard<std::mutex> lock(mutex_);
    items_.clear();
    for (const std::filesystem::path& input : inputs) {
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

  thread_ = std::thread(&Remuxer::Run, this);
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

void Remuxer::Run() {
  const Libav& av = *Av();
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

    std::filesystem::path input;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      input = items_[i].input;
    }
    const std::filesystem::path output = MakeOutputName(input);

    SetMessage(Format(T("%zu von %zu: %s", "%zu of %zu: %s"), i + 1, count,
                      PathToUtf8(input.filename()).c_str()));

    // Nothing is decoded: the packets are moved across as they are.
    const Said failure =
        RemuxOne(av, input, output, cancel_, [this, i, count](float fraction) {
          progress_.store(((float)i + std::min(fraction, 1.0f)) / (float)count,
                          std::memory_order_relaxed);
        });
    const bool good = failure.shown.empty();
    // A file that was done before the cancel came is kept.
    if (!good && cancel_.load(std::memory_order_relaxed)) {
      RemoveFile(output);
      SetMessage(T("Abgebrochen.", "Cancelled."));
      state_.store(State::Failed, std::memory_order_relaxed);
      return;
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
      CAP_LOG("Remuxed: %s", PathToUtf8(output.filename()).c_str());
    } else {
      // A half written MP4 is worse than none: it looks like a result.
      RemoveFile(output);
      LogWrite("ERR ", "Remux failed: %s -- %s", PathToUtf8(input.filename()).c_str(),
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
