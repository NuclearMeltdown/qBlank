#include "record/recorder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <system_error>

#include "record/gpu_frames.h"
#include "record/libav.h"
#include "record/track_plan.h"
#include "render/shared_frames.h"

namespace cap {
namespace {

std::string TimestampedName(RecordContainer container) {
  const LocalTime t = NowLocal();
  return Format("%s_%04d-%02d-%02d_%02d-%02d-%02d.%s", AppNameUtf8().c_str(), t.year, t.month,
                t.day, t.hour, t.minute, t.second,
                container == RecordContainer::Mp4   ? "mp4"
                : container == RecordContainer::Avi ? "avi"
                                                    : "mkv");
}

constexpr int kAudioBitrate = 192000;
// What a second of sound costs in an AVI, which takes it as it is: two
// channels of sixteen bits.
constexpr int kPcmBytesPerFrame = 4;
// What the audio thread pulls at a time, in frames.
constexpr size_t kAudioChunk = 4096;
// The tracks' rate when there is no card sound to set it: the recording
// sources are then brought onto the wall clock at this.
constexpr int kClockRate = 48000;
// Packets the disk has not taken yet, unless RecordSettings::largeBuffer says
// otherwise. A stall of seconds fits in this many; past it the disk is simply
// slower than the encoder, and waiting longer only ends in running out of
// memory.
constexpr int64_t kSmallQueueBytes = 512ll << 20;

// What the queue may grow to while `queued` waits in it and `freeBytes` of
// memory are free besides: the size set, but never into the reserve left to
// everything else, and never below the small buffer.
int64_t QueueLimit(const RecordSettings& settings, int64_t queued, uint64_t freeBytes) {
  if (!settings.largeBuffer) return kSmallQueueBytes;
  const int64_t set = (int64_t)settings.bufferGb << 30;
  const int64_t room = queued + (int64_t)freeBytes - (int64_t)kRecordBufferReserveBytes;
  return std::max(kSmallQueueBytes, std::min(set, room));
}

// The frame rate as a fraction, which is how the encoder and the container
// want it. Measured rates jitter, so what is close to a whole number or to its
// NTSC sibling becomes exactly that: 59.9401 is 60000/1001, not some fraction
// that only happens to land near it.
AVRational RecordingRate(const Libav& av, double fps) {
  const double whole = std::round(fps);
  if (whole >= 1.0 && std::fabs(fps - whole) <= fps * 0.0003) return AVRational{(int)whole, 1};
  const double ntsc = std::round(fps * 1.001);
  if (ntsc >= 1.0 && std::fabs(fps - ntsc / 1.001) <= fps * 0.0003) {
    return AVRational{(int)ntsc * 1000, 1001};
  }
  return av.av_d2q(fps, 1001000);
}

}  // namespace

double EstimatedBytesPerSecond(const RecordSettings& settings, int audioTracks, int width,
                               int height, double fps, double pixelBytes) {
  const bool pcm = ContainerFor(settings.encoder, settings.container) == RecordContainer::Avi;
  const double audio = pcm ? (double)kClockRate * kPcmBytesPerFrame : kAudioBitrate / 8.0;
  // Container overhead is left out: a few bytes of index per frame against
  // twenty megabit of picture is below the error of the bitrate itself.
  double video = (double)settings.bitrateKbps * 1000.0 / 8.0;
  if (IsLosslessEncoder(settings.encoder)) {
    // The bytes a pixel as it is, three for RGB. Both codecs came to 0.3 of
    // that on real composite captures and to 0.5 on a noisy 1080p picture.
    // The estimate takes the worse.
    video = (double)width * (double)height * pixelBytes * fps;
    if (settings.encoder != RecordEncoder::Uncompressed) video *= 0.5;
  }
  return video + audio * (double)std::max(audioTracks, 0);
}

// ------------------------------------------------------------------ session

struct Recorder::Session {
  const Libav* av = nullptr;
  RecordSettings settings;
  EncoderInfo encoder;
  std::filesystem::path file;
  AVRational rate = {60, 1};
  // The recorder's count of bytes written, while the recording runs. Cleared
  // before closing, so what the encoders still let out then does not count
  // towards the next file.
  std::atomic<uint64_t>* counter = nullptr;

  AVFormatContext* output = nullptr;
  bool fileOpen = false;
  bool headerWritten = false;

  AVCodecContext* video = nullptr;
  int videoStream = -1;
  AVPacket* videoPacket = nullptr;  // the next one to receive into
  // The frames PushVideo fills. A buffer comes back once nobody refers to it
  // any more, the encoder included, so none is ever written into while read.
  AVBufferPool* pool = nullptr;
  // Without filters, the card's frames as the encoder takes them, where they
  // had to be repacked.
  AVBufferPool* repacked = nullptr;
  // Where the encoder takes its frames on the GPU instead, when it does.
  std::unique_ptr<GpuFrames> gpu;

  struct Track {
    AVCodecContext* codec = nullptr;
    int stream = -1;
    AVFrame* frame = nullptr;  // filled up to `fill`, sent when full
    int fill = 0;
    int64_t samples = 0;
  };
  std::vector<Track> tracks;
  int mix = -1;      // indices into tracks
  int capture = -1;
  AVPacket* audioPacket = nullptr;

  // One recording source on its way onto the master clock: resampled, then
  // waiting in `fifo` (interleaved stereo, from `start` to `end`, in frames)
  // to be taken in step with it. The audio thread only, past the resampler.
  struct Feed {
    SwrContext* resampler = nullptr;
    int track = -1;  // its own, -1 for none
    bool mixed = false;
    std::vector<float> fifo;
    size_t start = 0;
    size_t end = 0;
    size_t lowest = SIZE_MAX;
    uint64_t measured = 0;
    uint64_t underruns = 0;
    uint64_t overflows = 0;
  };
  std::vector<Feed> feeds;  // in the order of Recorder::extras_

  // The muxer's own thread: the disk may pause, the encoders must not.
  std::mutex muxMutex;
  std::condition_variable muxWake;
  std::deque<AVPacket*> queue;
  bool closing = false;
  std::atomic<int64_t> queuedBytes{0};
  std::atomic<int64_t> queueLimit{kSmallQueueBytes};
  std::atomic<bool> writeFailed{false};
  std::string writeError;  // under muxMutex
  std::thread muxer;

  ~Session();
  void MuxLoop();
  void StopMuxer();
  // Takes what `codec` has finished and queues it for the muxer.
  int Drain(AVCodecContext* codec, int stream, AVPacket** spare);
  // Hands one packet, timed in `timeBase`, to the muxer.
  void Queue(AVPacket* packet, int stream, AVRational timeBase);
  // Appends interleaved stereo to a track, sending each frame as it fills.
  int Encode(Track& track, const float* interleaved, size_t frames);
  // Lets the encoders finish and writes the container's index. True when the
  // whole file made it to the disk.
  bool Close();
};

Recorder::Session::~Session() {
  StopMuxer();
  if (!av) return;
  for (AVPacket* packet : queue) av->av_packet_free(&packet);
  av->av_packet_free(&videoPacket);
  av->av_packet_free(&audioPacket);
  av->avcodec_free_context(&video);
  for (Track& track : tracks) {
    av->av_frame_free(&track.frame);
    av->avcodec_free_context(&track.codec);
  }
  for (Feed& feed : feeds) av->swr_free(&feed.resampler);
  if (pool) av->av_buffer_pool_uninit(&pool);
  if (repacked) av->av_buffer_pool_uninit(&repacked);
  if (output) {
    if (fileOpen) av->avio_closep(&output->pb);
    av->avformat_free_context(output);
  }
}

void Recorder::Session::MuxLoop() {
  std::deque<AVPacket*> batch;
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(muxMutex);
      muxWake.wait(lock, [this] { return !queue.empty() || closing; });
      if (queue.empty()) return;
      batch.swap(queue);
    }
    for (AVPacket* packet : batch) {
      const int size = packet->size;
      if (!writeFailed.load(std::memory_order_relaxed)) {
        LibavClearError();
        const int result = av->av_interleaved_write_frame(output, packet);
        if (result < 0) {
          const std::string why = LibavError(result);
          {
            std::lock_guard<std::mutex> lock(muxMutex);
            writeError = why;
          }
          writeFailed.store(true, std::memory_order_release);
        }
      }
      av->av_packet_free(&packet);
      queuedBytes.fetch_sub(size, std::memory_order_relaxed);
    }
    batch.clear();
  }
}

void Recorder::Session::StopMuxer() {
  if (!muxer.joinable()) return;
  {
    std::lock_guard<std::mutex> lock(muxMutex);
    closing = true;
  }
  muxWake.notify_all();
  muxer.join();
}

int Recorder::Session::Drain(AVCodecContext* codec, int stream, AVPacket** spare) {
  for (;;) {
    if (!*spare && !(*spare = av->av_packet_alloc())) return AVERROR(ENOMEM);
    const int result = av->avcodec_receive_packet(codec, *spare);
    if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return 0;
    if (result < 0) return result;
    AVPacket* packet = *spare;
    *spare = nullptr;
    Queue(packet, stream, codec->time_base);
  }
}

void Recorder::Session::Queue(AVPacket* packet, int stream, AVRational timeBase) {
  packet->stream_index = stream;
  av->av_packet_rescale_ts(packet, timeBase, output->streams[stream]->time_base);
  if (counter) counter->fetch_add((uint64_t)packet->size, std::memory_order_relaxed);
  queuedBytes.fetch_add(packet->size, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(muxMutex);
    queue.push_back(packet);
  }
  muxWake.notify_one();
}

int Recorder::Session::Encode(Track& track, const float* in, size_t frames) {
  const int frameSize = track.frame->nb_samples;
  while (frames > 0) {
    if (track.fill == 0) {
      // The encoder may still hold the last one.
      const int result = av->av_frame_make_writable(track.frame);
      if (result < 0) return result;
    }
    const int n = (int)std::min(frames, (size_t)(frameSize - track.fill));
    if (track.frame->format == AV_SAMPLE_FMT_S16) {
      // Times 32768, the inverse of what made the floats of sixteen bit
      // sound, so a card's samples come out as they went in.
      int16_t* out = (int16_t*)track.frame->data[0] + 2 * (size_t)track.fill;
      for (int i = 0; i < 2 * n; ++i) {
        out[i] = (int16_t)std::clamp(std::lrintf(in[i] * 32768.0f), -32768L, 32767L);
      }
    } else {
      float* left = (float*)track.frame->data[0] + track.fill;
      float* right = (float*)track.frame->data[1] + track.fill;
      for (int i = 0; i < n; ++i) {
        left[i] = in[2 * i];
        right[i] = in[2 * i + 1];
      }
    }
    in += 2 * (size_t)n;
    frames -= (size_t)n;
    track.fill += n;
    if (track.fill == frameSize) {
      track.frame->pts = track.samples;
      track.samples += frameSize;
      track.fill = 0;
      int result = av->avcodec_send_frame(track.codec, track.frame);
      if (result >= 0) result = Drain(track.codec, track.stream, &audioPacket);
      if (result < 0) return result;
    }
  }
  return 0;
}

bool Recorder::Session::Close() {
  if (!headerWritten) return false;
  LibavClearError();
  // What the encoders still hold: lookahead and B-frames for the picture, the
  // last part frame for the sound.
  if (video && av->avcodec_send_frame(video, nullptr) >= 0) {
    Drain(video, videoStream, &videoPacket);
  }
  for (Track& track : tracks) {
    if (!track.codec) continue;
    if (track.fill > 0 && av->av_frame_make_writable(track.frame) >= 0) {
      track.frame->nb_samples = track.fill;
      track.frame->pts = track.samples;
      track.samples += track.fill;
      track.fill = 0;
      if (av->avcodec_send_frame(track.codec, track.frame) >= 0) {
        Drain(track.codec, track.stream, &audioPacket);
      }
    }
    if (av->avcodec_send_frame(track.codec, nullptr) >= 0) {
      Drain(track.codec, track.stream, &audioPacket);
    }
  }
  StopMuxer();

  bool complete = !writeFailed.load(std::memory_order_acquire);
  if (!complete) CAP_ERR("Recording: writing failed: %s", writeError.c_str());
  LibavClearError();
  const int trailer = av->av_write_trailer(output);
  if (trailer < 0) {
    CAP_ERR("Recording: could not finish the file: %s", LibavError(trailer).c_str());
    complete = false;
  }
  if (fileOpen) {
    fileOpen = false;
    if (av->avio_closep(&output->pb) < 0) complete = false;
  }
  return complete;
}

// -------------------------------------------------------------- the recorder

Recorder::Recorder() = default;

Recorder::~Recorder() {
  Stop();
  if (closer_.joinable()) closer_.join();
}

void Recorder::Fail(const Said& said) {
  {
    std::lock_guard<std::mutex> lock(infoMutex_);
    if (error_.empty()) error_ = said.shown;
  }
  failed_.store(true, std::memory_order_relaxed);
  CAP_ERR("Recording: %s", said.logged.c_str());
}

Recorder::Family Recorder::FamilyOf(const std::string& name) {
  if (name.find("_nvenc") != std::string::npos) return Family::Nvenc;
  if (name.find("_amf") != std::string::npos) return Family::Amf;
  if (name.find("_qsv") != std::string::npos) return Family::Qsv;
  return Family::Software;
}

YuvFormat Recorder::FormatFor(const EncoderInfo& encoder, bool wide, bool fullChroma) {
  YuvFormat f;
  // Lossless is 4:4:4 whatever the setting, and ten bits only where NVENC
  // passed them; uncompressed is the RGB an AVI holds, in eight.
  if (encoder.id == RecordEncoder::Uncompressed) {
    f.layout = YuvLayout::Bgr;
    return f;
  }
  if (IsLosslessEncoder(encoder.id)) {
    wide = wide && encoder.full10;
    fullChroma = true;
  }
  f.matrix = wide ? YuvMatrix::Bt2020Pq : YuvMatrix::Bt709;
  // Ten bits at the top of sixteen for the hardware, as P010 has them; x264
  // and x265 count from the bottom.
  const bool software = FamilyOf(encoder.ffmpegName) == Family::Software;
  f.msb = !software;
  if (fullChroma && (wide ? encoder.full10 : encoder.full8)) {
    f.layout = YuvLayout::I444;
  } else if (software && (wide || encoder.ffmpegName == "libx265")) {
    // x265 takes no NV12 at all, x264 none in ten bits: planar.
    f.layout = YuvLayout::I420;
  } else {
    f.layout = YuvLayout::Nv12;
  }
  return f;
}

double Recorder::CardPixelBytes(RecordEncoder encoder, const VideoFormatInfo& card) {
  const CardPlan plan = PlanCard(encoder, card);
  return plan.pixelFormat ? plan.pixelBytes : 0.0;
}

namespace {

// The seven speed steps, in each vendor's own words. Empty means "this one has
// nothing to say here", and then nothing is passed at all -- which is not the
// same as passing its default, and is the only honest thing to do.
const char* PresetName(Recorder::Family family, EncoderPreset preset) {
  if (preset == EncoderPreset::Auto) return nullptr;
  const int step = (int)preset - 1;  // 0 = fastest .. 6 = slowest
  switch (family) {
    case Recorder::Family::Nvenc: {
      // p1 is fastest, p7 slowest. Straight across.
      static const char* kNames[] = {"p1", "p2", "p3", "p4", "p5", "p6", "p7"};
      return kNames[step];
    }
    case Recorder::Family::Qsv: {
      static const char* kNames[] = {"veryfast", "faster", "fast",
                                        "medium",   "slow",   "slower", "veryslow"};
      return kNames[step];
    }
    case Recorder::Family::Amf: {
      // AMF has three, so the seven steps fold onto them.
      static const char* kNames[] = {"speed",   "speed",  "balanced", "balanced",
                                        "quality", "quality", "quality"};
      return kNames[step];
    }
    default: {
      static const char* kNames[] = {"ultrafast", "veryfast", "faster", "medium",
                                        "slow",      "slower",   "veryslow"};
      return kNames[step];
    }
  }
}

}  // namespace

std::vector<std::string> Recorder::EncoderOptions(const RecordSettings& settings,
                                                  const EncoderInfo& encoder) const {
  const Family family = FamilyOf(encoder.ffmpegName);
  std::vector<std::string> out;
  auto pair = [&out](std::string key, std::string value) {
    out.push_back(std::move(key));
    out.push_back(std::move(value));
  };
  if (IsLosslessEncoder(encoder.id)) {
    for (const auto& [key, value] : LosslessOptions(encoder.id)) pair(key, value);
    return out;
  }

  // ---- how the bitrate is spent ----
  //
  // The names differ per vendor and so does the way a quality target is asked
  // for. Quicksync has no rc at all: what it does follows from which of
  // bitrate or quality it is given.
  //
  // A fixed quantiser runs 0..255 for AV1 in hardware, against 0..63 for the
  // software AV1 encoders' crf. Four steps of one are one of the other, so the
  // level means the same whichever AV1 encoder it goes to.
  const int qp = settings.qualityLevel *
                 (encoder.ffmpegName.compare(0, 4, "av1_") == 0 ? 4 : 1);
  switch (family) {
    case Family::Nvenc:
      pair("rc", settings.rateControl == RateControl::Cbr   ? "cbr"
                 : settings.rateControl == RateControl::Vbr ? "vbr"
                                                           : "constqp");
      // cq belongs to vbr; constqp only listens to qp.
      if (settings.rateControl == RateControl::Quality) pair("qp", std::to_string(qp));
      break;
    case Family::Amf:
      pair("rc", settings.rateControl == RateControl::Cbr   ? "cbr"
                 : settings.rateControl == RateControl::Vbr ? "vbr_peak"
                                                           : "cqp");
      if (settings.rateControl == RateControl::Quality) {
        pair("qp_i", std::to_string(qp));
        pair("qp_p", std::to_string(qp));
      }
      break;
    case Family::Qsv:
      if (settings.rateControl == RateControl::Quality) {
        pair("global_quality", std::to_string(settings.qualityLevel));
      }
      break;
    default:
      if (settings.rateControl == RateControl::Quality) {
        pair("crf", std::to_string(settings.qualityLevel));
      }
      break;
  }

  // ---- speed against quality ----
  if (const char* preset = PresetName(family, settings.preset)) {
    pair(family == Family::Amf ? "quality" : "preset", preset);
  } else if (family == Family::Software) {
    // The old speed setting still applies when the new preset is left on
    // automatic, so nobody's existing configuration changes meaning.
    pair("preset", RecordSpeedName((int)settings.speed));
  }

  // ---- what to optimise for ----
  if (settings.tune != EncoderTune::Auto) {
    if (family == Family::Nvenc) {
      pair("tune", settings.tune == EncoderTune::Quality ? "hq" : "ll");
    } else if (family == Family::Amf) {
      pair("usage", settings.tune == EncoderTune::Quality ? "transcoding" : "lowlatency");
    }
  }

  // ---- looking ahead ----
  if (settings.lookAhead) {
    if (family == Family::Nvenc) {
      pair("rc-lookahead", "32");
    } else if (family == Family::Qsv) {
      pair("look_ahead", "1");
    } else if (family == Family::Amf) {
      pair("preanalysis", "1");
    }
  }

  // ---- spending bits where the eye looks ----
  if (settings.adaptiveQuant) {
    if (family == Family::Nvenc) {
      pair("spatial-aq", "1");
      pair("temporal-aq", "1");
    } else if (family == Family::Amf) {
      pair("vbaq", "1");
    } else if (family == Family::Qsv) {
      pair("mbbrc", "1");
    }
  }

  // ---- two passes ----
  if (family == Family::Nvenc && settings.multipass != Multipass::Auto) {
    pair("multipass", settings.multipass == Multipass::Off       ? "disabled"
                      : settings.multipass == Multipass::Quarter ? "qres"
                                                                : "fullres");
  }

  return out;
}

// -------------------------------------------------------------------- start

std::filesystem::path NextRecordingFile(const std::filesystem::path& folder,
                                        RecordContainer container) {
  const std::filesystem::path named = folder / Utf8ToPath(TimestampedName(container));
  std::filesystem::path outFile = named;
  std::error_code exists;
  for (int n = 2; std::filesystem::exists(outFile, exists); ++n) {
    outFile = named;
    outFile.replace_filename(Utf8ToPath(Format("%s_%d", PathToUtf8(named.stem()).c_str(), n)) +=
                             named.extension());
  }
  return outFile;
}

bool Recorder::Start(const RecordSettings& settings, const std::filesystem::path& outFile,
                     const FfmpegInfo& ffmpeg, int width, int height, double sourceFps,
                     const AudioSource& main, std::vector<ExtraAudio> extras, std::string* error) {
  Stop();

  auto fail = [&](const Said& said) {
    if (error) *error = said.shown;
    CAP_ERR("Recording could not be started: %s", said.logged.c_str());
    Stop();
    return false;
  };

  const Libav* av = Av();
  if (!ffmpeg.found || !av) {
    return fail(CAP_SAID(T("ffmpeg wurde nicht gefunden.", "ffmpeg was not found.")));
  }
  if (width <= 0 || height <= 0) {
    return fail(CAP_SAID(T("Kein Bild zum Aufnehmen.", "No picture to record.")));
  }

  const EncoderInfo* encoder = ffmpeg.Resolve(settings.encoder);
  if (!encoder || !encoder->available) {
    return fail(CAP_SAID(T("Kein verwendbarer Encoder. Encoder-Test in den Einstellungen ausführen.",
                           "No usable encoder. Run the encoder test in the settings.")));
  }

  // Odd sizes break 4:2:0 chroma. Rather than refuse, round down by a pixel --
  // the alternative is telling someone their 1439 pixel capture cannot be
  // recorded, which helps nobody.
  width_ = width & ~1;
  height_ = height & ~1;
  // Without filters at the card's rate: dropping frames would be a filter too.
  const double fps =
      settings.fps > 0.0 && !unfiltered_ ? settings.fps : (sourceFps > 1.0 ? sourceFps : 60.0);
  const AVRational rate = RecordingRate(*av, fps);
  fps_ = av_q2d(rate);
  audioRate_ = main.active() ? main.sampleRate : 0;
  format_ = FormatFor(*encoder, wide_, settings.fullChroma);
  frameBytes_ = (size_t)YuvColumns(format_.layout, width_) *
                (size_t)YuvRows(format_.layout, height_) * (size_t)format_.sampleBytes();
  if (unfiltered_) {
    plan_ = PlanCard(encoder->id, card_.format);
    if (!plan_.pixelFormat) {
      return fail(CAP_SAID(T("Dieser Encoder nimmt das Kartenformat nicht ohne Filter.",
                             "This encoder does not take the card's format unfiltered.")));
    }
    cardBytes_ = CardFrameBytes(card_.format);
    frameBytes_ = CardOutBytes(plan_.out, width_, height_);
  }
  pullAudio_ = audioRate_ > 0 ? main.pull : nullptr;
  extras.erase(std::remove_if(extras.begin(), extras.end(),
                              [](const ExtraAudio& e) { return !e.source.active(); }),
               extras.end());
  extras_ = std::move(extras);

  auto session = std::make_unique<Session>();
  session->av = av;
  session->settings = settings;
  session->encoder = *encoder;
  session->file = outFile;
  session->rate = rate;
  session->counter = &bytesWritten_;
  // Nothing is reserved: the queue only grows while the disk is behind.
  if (settings.largeBuffer) {
    uint64_t freeBytes = 0;
    const int64_t limit = MemoryBytes(nullptr, &freeBytes)
                              ? QueueLimit(settings, 0, freeBytes)
                              : (int64_t)settings.bufferGb << 30;
    session->queueLimit.store(limit, std::memory_order_relaxed);
    CAP_LOG("Recording: buffer up to %s of %d GB set, %s of memory free",
            FormatBytes((uint64_t)limit).c_str(), settings.bufferGb, FormatBytes(freeBytes).c_str());
  }

  audioFramesWritten_.store(0, std::memory_order_relaxed);
  videoFramesWritten_.store(0, std::memory_order_relaxed);
  duplicated_.store(0, std::memory_order_relaxed);
  leftOut_.store(0, std::memory_order_relaxed);
  dropped_.store(0, std::memory_order_relaxed);
  bytesWritten_.store(0, std::memory_order_relaxed);
  startTicks_ = ClockTicks();
  lastAudioSeen_ = 0;
  lastAudioProgressTicks_ = 0;
  audioStallLogged_ = false;
  failed_.store(false, std::memory_order_relaxed);
  encoderBehind_.store(false, std::memory_order_relaxed);
  ready_.store(false, std::memory_order_relaxed);
  sharedRefused_.store(false, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(infoMutex_);
    file_ = PathToUtf8(outFile);
    encoderLabel_ = encoder->label;
    error_.clear();
  }

  session_ = std::move(session);
  running_.store(true, std::memory_order_relaxed);
  if (audioRate_ > 0 || !extras_.empty()) {
    audioThread_ = std::thread(&Recorder::AudioThread, this, session_.get());
  }
  // The encoder opens there and not here: NVENC takes a tenth of a second and
  // more, and the picture would wait for it.
  videoThread_ = std::thread(&Recorder::VideoThread, this, session_.get());

  CAP_LOG("Recording started: %s, %dx%d @ %.3f fps, %s", PathToUtf8(outFile).c_str(), width_,
          height_, fps_, encoder->label.c_str());
  if (unfiltered_) {
    CAP_LOG("Recording: unfiltered, card %s %dx%d as %s%s", card_.format.subtypeLabel.c_str(),
            card_.format.width, card_.format.height, plan_.pixelFormat,
            plan_.repack == CardRepack::None ? "" : ", repacked");
  }
  return true;
}

bool Recorder::Open(Session* s) {
  const Libav& av = *s->av;
  const std::string file = PathToUtf8(s->file);
  auto failed = [&](const char* de, const char* en, int code) {
    const std::string why = LibavError(code);
    Fail(CAP_SAID(Format(T(de, en), why.c_str())));
    return false;
  };

  // An AVI takes its sound as PCM, as an editor reading one expects it.
  const RecordContainer container = ContainerFor(s->encoder.id, s->settings.container);
  const bool avi = container == RecordContainer::Avi;
  LibavClearError();
  int result = av.avformat_alloc_output_context2(
      &s->output, nullptr,
      container == RecordContainer::Mp4 ? "mp4" : avi ? "avi" : "matroska", file.c_str());
  if (result < 0 || !s->output) {
    return failed("Die Datei konnte nicht angelegt werden: %s", "Could not create the file: %s",
                  result);
  }
  const bool globalHeader = (s->output->oformat->flags & AVFMT_GLOBALHEADER) != 0;

  // ---- the picture ----
  const AVCodec* codec = av.avcodec_find_encoder_by_name(s->encoder.ffmpegName.c_str());
  if (!codec) {
    Fail(CAP_SAID(Format(T("Encoder %s fehlt in diesem ffmpeg.", "This ffmpeg lacks encoder %s."),
                         s->encoder.ffmpegName.c_str())));
    return false;
  }

  // A hardware encoder takes the picture on the GPU, where the renderer draws
  // it, when it takes D3D11 frames and the renderer can share them. 4:4:4 goes
  // as CUDA frames: no D3D11 format holds it, and NVENC, the one encoder that
  // does, takes those as well.
  const bool fullChroma = format_.layout == YuvLayout::I444;
  if (gpuAdapter_ != 0 && !unfiltered_ && (fullChroma || format_.layout == YuvLayout::Nv12) &&
      FamilyOf(s->encoder.ffmpegName) != Family::Software) {
    const AVPixelFormat wanted = fullChroma ? AV_PIX_FMT_CUDA : AV_PIX_FMT_D3D11;
    bool takes = false;
    for (int i = 0; const AVCodecHWConfig* c = av.avcodec_get_hw_config(codec, i); ++i) {
      takes |= c->pix_fmt == wanted && (c->methods & AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX) != 0;
    }
    if (takes) {
      s->gpu = CreateGpuFrames(av, gpuAdapter_, width_, height_, fullChroma,
                               format_.sampleBytes() == 2);
    }
  }

  // Everything the encoder is told, then opened. Once more when it did not
  // take the frames on the GPU, for the ones read back.
  std::string told;
  std::string unknown;
  auto open = [&]() {
    av.avcodec_free_context(&s->video);
    AVCodecContext* v = s->video = av.avcodec_alloc_context3(codec);
    if (!v) return AVERROR(ENOMEM);
    v->width = width_;
    v->height = height_;
    if (s->gpu) {
      v->pix_fmt = ((const AVHWFramesContext*)s->gpu->framesContext()->data)->format;
      if (!(v->hw_frames_ctx = av.av_buffer_ref(s->gpu->framesContext()))) return AVERROR(ENOMEM);
    } else {
      v->pix_fmt = av.av_get_pix_fmt(unfiltered_ ? plan_.pixelFormat : FfmpegPixelFormat(format_));
    }
    v->time_base = AVRational{s->rate.den, s->rate.num};
    v->framerate = s->rate;
    // As many threads as it likes, which is what the command line did by
    // default; the library's own default is one.
    v->thread_count = 0;
    // The frames arrive as Y'CbCr already, converted on the GPU with the
    // matrix named here. Without these the file says nothing about its
    // colours, and a player reads an untagged HD file as whatever it likes and
    // an untagged PQ one as anything but HDR.
    if (format_.pq()) {
      v->color_primaries = AVCOL_PRI_BT2020;
      v->color_trc = AVCOL_TRC_SMPTE2084;
      v->colorspace = AVCOL_SPC_BT2020_NCL;
    } else {
      v->color_primaries = AVCOL_PRI_BT709;
      v->color_trc = AVCOL_TRC_BT709;
      v->colorspace = AVCOL_SPC_BT709;
    }
    v->color_range = AVCOL_RANGE_MPEG;
    if (format_.layout == YuvLayout::Bgr) {
      v->colorspace = AVCOL_SPC_RGB;
      v->color_range = AVCOL_RANGE_JPEG;
    }
    // Without filters the card's own colours, as the renderer reads them.
    if (unfiltered_) {
      const bool hd = card_.bt709 || plan_.rgb;  // RGB is sRGB, on BT.709's primaries
      const AVColorPrimaries sd = height_ > 480 ? AVCOL_PRI_BT470BG : AVCOL_PRI_SMPTE170M;
      v->color_primaries = card_.transfer != 0 ? AVCOL_PRI_BT2020 : hd ? AVCOL_PRI_BT709 : sd;
      v->color_trc = card_.transfer == 1   ? AVCOL_TRC_SMPTE2084
                     : card_.transfer == 2 ? AVCOL_TRC_ARIB_STD_B67
                                           : AVCOL_TRC_BT709;
      v->colorspace = plan_.rgb               ? AVCOL_SPC_RGB
                      : card_.transfer != 0   ? AVCOL_SPC_BT2020_NCL
                      : hd                    ? AVCOL_SPC_BT709
                      : height_ > 480         ? AVCOL_SPC_BT470BG
                                              : AVCOL_SPC_SMPTE170M;
      v->color_range = card_.fullRange ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
      if (card_.fieldOrder != 0) v->field_order = card_.fieldOrder == 1 ? AV_FIELD_TT : AV_FIELD_BB;
    }
    if (globalHeader) v->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (s->settings.rateControl == RateControl::Quality || IsLosslessEncoder(s->encoder.id)) {
      // A quality target and a bitrate are contradictory instructions, and the
      // encoders resolve them by quietly ignoring one. Better to give only one.
      // Lossless has nothing to aim for.
      v->bit_rate = 0;
    } else {
      // Constant means constant: the ceiling is the bitrate. Variable is
      // allowed to peak at half again, which is what makes it worth choosing.
      const int64_t kbps = s->settings.bitrateKbps;
      const int64_t ceiling = s->settings.rateControl == RateControl::Cbr ? kbps : kbps * 3 / 2;
      v->bit_rate = kbps * 1000;
      v->rc_max_rate = ceiling * 1000;
      v->rc_buffer_size = (int)std::min<int64_t>(ceiling * 2000, INT32_MAX);
    }

    AVDictionary* options = nullptr;
    told.clear();
    const std::vector<std::string> pairs = EncoderOptions(s->settings, s->encoder);
    for (size_t i = 0; i + 1 < pairs.size(); i += 2) {
      av.av_dict_set(&options, pairs[i].c_str(), pairs[i + 1].c_str(), 0);
      told += Format(" %s=%s", pairs[i].c_str(), pairs[i + 1].c_str());
    }
    LibavClearError();
    const int opened = av.avcodec_open2(v, codec, &options);
    // What is left is what the encoder did not know. Not a failure, but worth
    // a line: it is a setting that did nothing.
    unknown.clear();
    for (const AVDictionaryEntry* e = nullptr;
         (e = av.av_dict_get(options, "", e, AV_DICT_IGNORE_SUFFIX)) != nullptr;) {
      unknown += Format(" %s", e->key);
    }
    av.av_dict_free(&options);
    return opened;
  };
  result = AVERROR(ENOSYS);
  if (s->gpu && (result = open()) < 0) {
    CAP_WARN("Recording: %s took no frames on the GPU (%s); reading them back",
             s->encoder.ffmpegName.c_str(), LibavError(result).c_str());
    s->gpu.reset();
  }
  if (!s->gpu) result = open();
  if (result < 0) {
    return failed("Der Encoder ließ sich nicht öffnen: %s", "Could not open the encoder: %s",
                  result);
  }
  if (!unknown.empty()) {
    CAP_WARN("Recording: %s ignored%s", s->encoder.ffmpegName.c_str(), unknown.c_str());
  }
  const AVCodecContext* v = s->video;

  AVStream* stream = av.avformat_new_stream(s->output, nullptr);
  if (!stream) return failed("Die Datei konnte nicht angelegt werden: %s",
                             "Could not create the file: %s", AVERROR(ENOMEM));
  stream->time_base = v->time_base;
  stream->avg_frame_rate = s->rate;
  stream->disposition = AV_DISPOSITION_DEFAULT;
  av.avcodec_parameters_from_context(stream->codecpar, v);
  s->videoStream = stream->index;

  // ---- the sound ----
  //
  // With more than one sound there is a choice of what the file should
  // contain (TrackPlan). The separate tracks stay what each device delivered;
  // the mix is a plain sum, so the game is as loud in it as on its own track.
  // The sum can clip if several are hot, which is what the level meters are
  // for.
  TrackPlan plan(audioRate_ > 0);
  for (const ExtraAudio& extra : extras_) plan.Add(extra.tracks);
  // Everything at the capture's rate when there is a capture: the sources are
  // brought onto it, which is what keeps them in step for an hour.
  const int trackRate = audioRate_ > 0 ? audioRate_ : kClockRate;
  s->tracks.reserve((size_t)plan.count());
  auto addTrack = [&](const char* title) -> int {
    const int trackCount = (int)s->tracks.size();
    Session::Track& track = s->tracks.emplace_back();
    const AVCodec* encoder = av.avcodec_find_encoder_by_name(avi ? "pcm_s16le" : "aac");
    if (!encoder || !(track.codec = av.avcodec_alloc_context3(encoder))) {
      failed("Der Audio-Encoder ließ sich nicht öffnen: %s", "Could not open the audio encoder: %s",
             AVERROR_ENCODER_NOT_FOUND);
      return -1;
    }
    AVCodecContext* a = track.codec;
    a->sample_fmt = avi ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_FLTP;
    a->sample_rate = trackRate;
    av.av_channel_layout_default(&a->ch_layout, 2);
    if (!avi) a->bit_rate = kAudioBitrate;
    a->time_base = AVRational{1, trackRate};
    if (globalHeader) a->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    LibavClearError();
    int r = av.avcodec_open2(a, encoder, nullptr);
    if (r < 0) {
      failed("Der Audio-Encoder ließ sich nicht öffnen: %s", "Could not open the audio encoder: %s",
             r);
      return -1;
    }
    AVStream* st = av.avformat_new_stream(s->output, nullptr);
    track.frame = av.av_frame_alloc();
    if (!st || !track.frame) {
      failed("Der Audio-Encoder ließ sich nicht öffnen: %s", "Could not open the audio encoder: %s",
             AVERROR(ENOMEM));
      return -1;
    }
    st->time_base = a->time_base;
    if (trackCount == 0) st->disposition = AV_DISPOSITION_DEFAULT;
    // Named tracks, so a player and an editor both show which is which instead
    // of "Audio 1" and "Audio 2".
    av.av_dict_set(&st->metadata, "title", title, 0);
    av.avcodec_parameters_from_context(st->codecpar, a);
    track.stream = st->index;

    track.frame->format = a->sample_fmt;
    track.frame->sample_rate = trackRate;
    track.frame->nb_samples = a->frame_size > 0 ? a->frame_size : 1024;
    av.av_channel_layout_default(&track.frame->ch_layout, 2);
    r = av.av_frame_get_buffer(track.frame, 0);
    if (r < 0) {
      failed("Der Audio-Encoder ließ sich nicht öffnen: %s", "Could not open the audio encoder: %s",
             r);
      return -1;
    }
    return trackCount;
  };
  if (plan.mix() && (s->mix = addTrack("Mix")) < 0) return false;
  if (plan.main() && (s->capture = addTrack("Capture")) < 0) return false;

  s->feeds.resize(extras_.size());
  AVChannelLayout stereo = {};
  av.av_channel_layout_default(&stereo, 2);
  for (size_t i = 0; i < extras_.size(); ++i) {
    const ExtraAudio& extra = extras_[i];
    Session::Feed& feed = s->feeds[i];
    feed.mixed = plan.mixed(extra.tracks);
    if (plan.own(extra.tracks) && (feed.track = addTrack(extra.title.c_str())) < 0) {
      av.av_channel_layout_uninit(&stereo);
      return false;
    }
    LibavClearError();
    result = av.swr_alloc_set_opts2(&feed.resampler, &stereo, AV_SAMPLE_FMT_FLT, trackRate,
                                    &stereo, AV_SAMPLE_FMT_FLT, extra.source.sampleRate, 0,
                                    nullptr);
    // Resampling even at the same rate: the two clocks are never quite the
    // same, and only a resampler can make up the difference smoothly.
    if (result >= 0) result = av.av_opt_set_int(feed.resampler, "flags", SWR_FLAG_RESAMPLE, 0);
    if (result >= 0) result = av.swr_init(feed.resampler);
    if (result < 0) {
      av.av_channel_layout_uninit(&stereo);
      return failed("Eine Tonquelle ließ sich nicht umrechnen: %s",
                    "Could not resample a sound source: %s", result);
    }
  }
  av.av_channel_layout_uninit(&stereo);
  const int trackCount = (int)s->tracks.size();

  // ---- the file ----
  LibavClearError();
  result = av.avio_open(&s->output->pb, file.c_str(), AVIO_FLAG_WRITE);
  if (result < 0) {
    return failed("Die Datei konnte nicht angelegt werden: %s", "Could not create the file: %s",
                  result);
  }
  s->fileOpen = true;
  // Uncompressed RGB comes bottom-up from the GPU, as a bitmap keeps it, and
  // the header has to say so. Without filters in the card's own row order.
  AVDictionary* header = nullptr;
  if (avi && (!unfiltered_ || card_.format.bottomUp)) {
    av.av_dict_set(&header, "flipped_raw_rgb", "1", 0);
  }
  result = av.avformat_write_header(s->output, &header);
  av.av_dict_free(&header);
  if (result < 0) {
    return failed("Die Datei konnte nicht angelegt werden: %s", "Could not create the file: %s",
                  result);
  }
  s->headerWritten = true;
  s->muxer = std::thread(&Session::MuxLoop, s);

  // Room for a frame, plus what SIMD may read past the end. A few are touched
  // now, so the first copies on the render thread do not fault their pages in.
  // On the GPU they are only there for when the renderer cannot share after
  // all, and stay untouched until then.
  // Without filters the card's frames as they come, and where the encoder
  // takes them otherwise, a second pool for the repacked ones.
  const size_t bufferBytes = (unfiltered_ ? cardBytes_ : frameBytes_) + AV_INPUT_BUFFER_PADDING_SIZE;
  const size_t repackedBytes = frameBytes_ + AV_INPUT_BUFFER_PADDING_SIZE;
  s->pool = av.av_buffer_pool_init(bufferBytes, nullptr);
  if (unfiltered_ && plan_.repack != CardRepack::None) {
    s->repacked = av.av_buffer_pool_init(repackedBytes, nullptr);
  }
  if (!s->pool || (unfiltered_ && plan_.repack != CardRepack::None && !s->repacked)) {
    Fail(CAP_SAID(T("Kein Speicher für die Aufnahme.", "Out of memory for the recording.")));
    return false;
  }
  auto warm = [&](AVBufferPool* pool, size_t bytes) {
    AVBufferRef* buffers[4] = {};
    for (AVBufferRef*& buffer : buffers) {
      buffer = av.av_buffer_pool_get(pool);
      if (buffer) memset(buffer->data, 0, bytes);
    }
    for (AVBufferRef*& buffer : buffers) av.av_buffer_unref(&buffer);
  };
  if (!s->gpu) warm(s->pool, bufferBytes);
  if (s->repacked) warm(s->repacked, repackedBytes);

  CAP_LOG("Recording: %s %dx%d %s%s at %d/%d fps,%s %d audio track%s at %d Hz",
          s->encoder.ffmpegName.c_str(), width_, height_,
          unfiltered_ ? plan_.pixelFormat : FfmpegPixelFormat(format_),
          s->gpu ? " on the GPU" : "", s->rate.num, s->rate.den,
          told.empty() ? " defaults," : told.c_str(), trackCount, trackCount == 1 ? "" : "s",
          trackCount > 0 ? trackRate : 0);
  return true;
}

void Recorder::Stop() {
  const bool wasRunning = running_.exchange(false, std::memory_order_relaxed);

  { std::lock_guard<std::mutex> lock(frameMutex_); }
  frameReady_.notify_all();
  if (videoThread_.joinable()) videoThread_.join();
  if (audioThread_.joinable()) audioThread_.join();
  ready_.store(false, std::memory_order_relaxed);

  AVBufferRef* stale = nullptr;
  {
    std::lock_guard<std::mutex> lock(frameMutex_);
    std::swap(stale, readyBuffer_);
    if (readySlot_ >= 0) {
      shared_->state[readySlot_].store(SharedFrames::Free, std::memory_order_release);
      readySlot_ = -1;
    }
  }
  shared_.reset();
  const Libav* av = Av();
  if (stale && av) av->av_buffer_unref(&stale);

  if (session_) {
    std::unique_ptr<Session> session = std::move(session_);
    session->counter = nullptr;
    // Freed once the encoder lets go of the last frame it holds.
    if (session->pool) av->av_buffer_pool_uninit(&session->pool);
    if (session->repacked) av->av_buffer_pool_uninit(&session->repacked);

    Finished done;
    {
      std::lock_guard<std::mutex> lock(infoMutex_);
      done.file = file_;
    }
    done.seconds = seconds();
    const unsigned long long frames = videoFramesWritten_.load(std::memory_order_relaxed);
    const unsigned long long leftOut = leftOut_.load(std::memory_order_relaxed);

    // The encoders give up what they still hold and the container writes its
    // index -- an MP4 that never gets that far has no moov atom and does not
    // play at all.
    std::thread previous = std::move(closer_);
    closer_ = std::thread([this, previous = std::move(previous), session = std::move(session),
                           done = std::move(done), frames, leftOut, wasRunning]() mutable {
      if (previous.joinable()) previous.join();
      const bool written = session->headerWritten;
      const bool created = session->fileOpen;
      done.complete = session->Close();
      const std::filesystem::path file = session->file;
      session.reset();
      if (!written) {
        // Opened and never begun: nothing in it worth keeping.
        if (created) {
          std::error_code ignored;
          std::filesystem::remove(file, ignored);
        }
        return;
      }
      if (wasRunning) {
        CAP_LOG("Recording stopped, %llu frames, %llu left out%s", frames, leftOut,
                done.complete ? "" : ", the file was not finished cleanly");
      }
      std::lock_guard<std::mutex> lock(finishedMutex_);
      finished_.push_back(std::move(done));
      anyFinished_.store(true, std::memory_order_release);
    });
  }
  pullAudio_ = nullptr;
  extras_.clear();
}

bool Recorder::TakeFinished(Finished* out) {
  // Asked every frame, and almost always the answer is no.
  if (!anyFinished_.load(std::memory_order_acquire)) return false;
  std::lock_guard<std::mutex> lock(finishedMutex_);
  if (finished_.empty()) return false;
  *out = std::move(finished_.front());
  finished_.erase(finished_.begin());
  anyFinished_.store(!finished_.empty(), std::memory_order_relaxed);
  return true;
}

// --------------------------------------------------------------- frame feed

void Recorder::PushVideo(const uint8_t* pixels, int stride, int width, int height,
                         const YuvFormat& format) {
  if (!running_.load(std::memory_order_relaxed) || !ready_.load(std::memory_order_acquire) ||
      !pixels || format != format_) {
    return;
  }

  // A frame of a different shape than the encoder was opened for. This happens
  // for the fraction of a second between the source changing its video
  // standard and the app noticing and cutting the file. Dropping it is not a
  // nicety: the copy below reads the rows the encoder was opened for, and a
  // source that just went from 576 to 480 lines reads a fifth of a frame past
  // the end of the staging buffer.
  //
  // Compared with the same rounding that Start applied, because an odd source
  // is a legitimate one -- a crop can leave 721 columns -- and the file is then
  // one column narrower on purpose. Rounding down can only ever ask for less
  // than the frame holds, so it stays safe.
  if ((width & ~1) != width_ || (height & ~1) != height_) return;

  // A buffer nobody else holds, so the copy needs no lock. The rows are
  // repacked because a staging texture's pitch is rarely the row's width; the
  // planes then lie one after another, as the video thread points into them.
  AVBufferRef* buffer = Av()->av_buffer_pool_get(session_->pool);
  if (!buffer) return;
  const size_t rowBytes =
      (size_t)YuvColumns(format_.layout, width_) * (size_t)format_.sampleBytes();
  const int rows = YuvRows(format_.layout, height_);
  uint8_t* dst = buffer->data;
  if ((size_t)stride == rowBytes) {
    memcpy(dst, pixels, rowBytes * (size_t)rows);
  } else {
    for (int y = 0; y < rows; ++y) {
      memcpy(dst + (size_t)y * rowBytes, pixels + (size_t)y * (size_t)stride, rowBytes);
    }
  }
  Offer(buffer);
}

void Recorder::PushCardFrame(const uint8_t* data, size_t size) {
  if (!running_.load(std::memory_order_relaxed) || !ready_.load(std::memory_order_acquire) ||
      !unfiltered_ || !data || size < cardBytes_) {
    return;
  }
  // One copy as it is: the repacking waits for the video thread.
  AVBufferRef* buffer = Av()->av_buffer_pool_get(session_->pool);
  if (!buffer) return;
  memcpy(buffer->data, data, cardBytes_);
  Offer(buffer);
}

void Recorder::Offer(AVBufferRef* buffer) {
  AVBufferRef* stale = nullptr;
  {
    std::lock_guard<std::mutex> lock(frameMutex_);
    stale = readyBuffer_;
    readyBuffer_ = buffer;
  }
  frameReady_.notify_one();
  if (stale) {
    // A frame still waiting here never made it into the file: the timeline did
    // not need it, because the source runs faster than the recording rate.
    dropped_.fetch_add(1, std::memory_order_relaxed);
    Av()->av_buffer_unref(&stale);
  }
}

const std::shared_ptr<SharedFrames>& Recorder::sharedFrames() const {
  static const std::shared_ptr<SharedFrames> none;
  return ready_.load(std::memory_order_acquire) && !sharedRefused_.load(std::memory_order_relaxed)
             ? shared_
             : none;
}

void Recorder::PushGpuFrame(int slot) {
  SharedFrames* frames = shared_.get();
  if (!frames || slot < 0 || slot >= SharedFrames::kSlots) return;
  int stale = -1;
  {
    std::lock_guard<std::mutex> lock(frameMutex_);
    stale = readySlot_;
    frames->state[slot].store(SharedFrames::Ready, std::memory_order_release);
    readySlot_ = slot;
  }
  frameReady_.notify_one();
  if (stale >= 0) {
    // As in PushVideo: the timeline did not need it.
    dropped_.fetch_add(1, std::memory_order_relaxed);
    frames->state[stale].store(SharedFrames::Free, std::memory_order_release);
  }
}

// ------------------------------------------------------------ writer threads

void Recorder::VideoThread(Session* s) {
  if (!Open(s)) return;
  const Libav& av = *s->av;
  AVFrame* frame = av.av_frame_alloc();
  // On the GPU: the pool's frame being filled, and a read back frame on its
  // way up.
  AVFrame* next = s->gpu ? av.av_frame_alloc() : nullptr;
  AVFrame* soft = s->gpu ? av.av_frame_alloc() : nullptr;
  if (!frame || (s->gpu && (!next || !soft))) {
    Fail(CAP_SAID(T("Kein Speicher für die Aufnahme.", "Out of memory for the recording.")));
    av.av_frame_free(&frame);
    av.av_frame_free(&next);
    av.av_frame_free(&soft);
    return;
  }

  // A read back frame points into the buffer it holds: the luma plane, then
  // the chroma as YuvLayout lays it out, no padding anywhere. The encoder takes
  // a reference, so the same frame goes out again with the next number when
  // the timeline needs a repeat.
  const AVCodecContext* v = s->video;
  const bool raw = s->encoder.id == RecordEncoder::Uncompressed && !s->gpu;
  const AVPixelFormat softFormat =
      av.av_get_pix_fmt(unfiltered_ ? plan_.pixelFormat : FfmpegPixelFormat(format_));
  const int rowBytes = YuvColumns(format_.layout, width_) * format_.sampleBytes();
  auto tag = [&](AVFrame* f) {
    f->color_primaries = v->color_primaries;
    f->color_trc = v->color_trc;
    f->colorspace = v->colorspace;
    f->color_range = v->color_range;
  };
  auto point = [&](AVFrame* f, AVBufferRef* buffer) {
    av.av_frame_unref(f);
    f->buf[0] = buffer;
    f->format = softFormat;
    f->width = width_;
    f->height = height_;
    tag(f);
    uint8_t* chroma = buffer->data + (size_t)rowBytes * (size_t)height_;
    f->data[0] = buffer->data;
    f->linesize[0] = rowBytes;
    switch (format_.layout) {
      case YuvLayout::Nv12:
        f->data[1] = chroma;
        f->linesize[1] = rowBytes;
        break;
      case YuvLayout::I420:
        f->data[1] = chroma;
        f->data[2] = chroma + (size_t)(rowBytes / 2) * (size_t)(height_ / 2);
        f->linesize[1] = f->linesize[2] = rowBytes / 2;
        break;
      case YuvLayout::I444:
        f->data[1] = chroma;
        f->data[2] = chroma + (size_t)rowBytes * (size_t)height_;
        f->linesize[1] = f->linesize[2] = rowBytes;
        break;
      case YuvLayout::Bgr:
        break;  // one plane
    }
  };
  // Without filters the card's frame as it is, or repacked into a buffer of
  // its own, and the card's goes back to the pool.
  auto pointCard = [&](AVFrame* f, AVBufferRef* buffer) {
    AVBufferRef* out = nullptr;
    if (plan_.repack != CardRepack::None && !(out = av.av_buffer_pool_get(s->repacked))) {
      av.av_buffer_unref(&buffer);
      return false;
    }
    av.av_frame_unref(f);
    f->format = softFormat;
    f->width = width_;
    f->height = height_;
    tag(f);
    RepackCard(plan_, card_.format, width_, height_, buffer->data, out ? out->data : nullptr,
               f->data, f->linesize);
    if (out) av.av_buffer_unref(&buffer);
    f->buf[0] = out ? out : buffer;
    return true;
  };
  // On the GPU the frame is one of the encoder's pool: the picture of a
  // shared slot split into it, or a read back one uploaded. Slots that fail
  // three times in a row send the renderer back to reading back.
  int slotFailures = 0;
  auto fill = [&](int slot, AVBufferRef* buffer) {
    LibavClearError();
    int result = av.av_hwframe_get_buffer(s->gpu->framesContext(), next, 0);
    if (slot >= 0) {
      const bool converted = result >= 0 && s->gpu->Convert(slot, next, running_);
      shared_->state[slot].store(SharedFrames::Free, std::memory_order_release);
      if (!converted) {
        av.av_frame_unref(next);
        if (++slotFailures == 3) {
          CAP_WARN("Recording on the GPU: the frames do not come through; reading them back");
          sharedRefused_.store(true, std::memory_order_relaxed);
        }
        return 0;
      }
      slotFailures = 0;
    } else {
      point(soft, buffer);
      if (result >= 0) result = av.av_hwframe_transfer_data(next, soft, 0);
      av.av_frame_unref(soft);
      if (result < 0) {
        av.av_frame_unref(next);
        return result;
      }
    }
    tag(next);
    av.av_frame_unref(frame);
    av.av_frame_move_ref(frame, next);
    return 1;
  };
  bool holding = false;

  shared_ = s->gpu ? s->gpu->frames() : nullptr;
  originTicks_ = ClockTicks();
  ready_.store(true, std::memory_order_release);

  // When the writer last had nothing left to write. An encoder that keeps up
  // gets there after every frame; one that does not gets there only between
  // the slots it leaves out (below), so a gap within the last half second
  // still counts as behind. A slow source looks nothing like that: the held
  // frame goes out again at once. The start gets longer, for the encoder
  // settling in while the first frames pile up.
  int64_t caughtUpTicks = originTicks_;
  int64_t gapTicks = 0;
  bool everCaughtUp = false;
  // Whether the writer waited since it last encoded. Slots left out after a
  // wait were the clock's doing -- the sound moves it on in steps of its
  // packets, a sleep can run long -- and the source's frames went missing
  // for those just the same before any encoder saw them. Only the ones after
  // an encode with no wait in between are the encoder's (leftOut).
  bool waited = true;

  while (running_.load(std::memory_order_relaxed)) {
    if (s->writeFailed.load(std::memory_order_acquire)) {
      std::string why;
      {
        std::lock_guard<std::mutex> lock(s->muxMutex);
        why = s->writeError;
      }
      Fail(CAP_SAID(Format(T("Schreiben fehlgeschlagen: %s", "Writing failed: %s"), why.c_str())));
      break;
    }
    if (s->queuedBytes.load(std::memory_order_relaxed) >
        s->queueLimit.load(std::memory_order_relaxed)) {
      // What is in it still goes to the disk, as the file closes.
      Fail(s->settings.largeBuffer
               ? CAP_SAID(T("Puffer voll: die Platte kommt mit dem Schreiben nicht hinterher.",
                            "Buffer full: the disk cannot keep up with the writing."))
               : CAP_SAID(T("Die Platte kommt mit dem Schreiben nicht hinterher. Ein größerer "
                            "Puffer lässt sich unter Aufnahme einstellen.",
                            "The disk cannot keep up with the writing. A larger buffer can be "
                            "set under Recording.")));
      break;
    }

    // Take the newest frame and keep holding it: when the timeline needs more
    // frames than actually arrived, this is what gets repeated.
    bool fresh = false;
    int slot = -1;
    AVBufferRef* buffer = nullptr;
    {
      std::unique_lock<std::mutex> lock(frameMutex_);
      // Nothing to repeat before the first frame.
      frameReady_.wait(lock, [&] {
        return readyBuffer_ != nullptr || readySlot_ >= 0 || holding ||
               !running_.load(std::memory_order_relaxed);
      });
      std::swap(slot, readySlot_);
      std::swap(buffer, readyBuffer_);
      if (slot >= 0) shared_->state[slot].store(SharedFrames::Converting, std::memory_order_relaxed);
    }
    if (slot >= 0 && buffer) {
      // Both, for the frame the renderer went back to reading back: the slot
      // is the older one only if it is still waiting, and either does.
      dropped_.fetch_add(1, std::memory_order_relaxed);
      av.av_buffer_unref(&buffer);
    }
    if (unfiltered_ && buffer) {
      if (pointCard(frame, buffer)) {
        holding = fresh = true;
      } else {
        dropped_.fetch_add(1, std::memory_order_relaxed);
      }
    } else if (!s->gpu && buffer) {
      point(frame, buffer);
      holding = fresh = true;
    } else if (slot >= 0 || buffer) {
      const int filled = fill(slot, buffer);
      if (filled < 0) {
        Fail(CAP_SAID(Format(T("Encoder-Fehler: %s", "Encoder error: %s"),
                             LibavError(filled).c_str())));
        break;
      }
      if (filled > 0) holding = fresh = true;
    }
    if (!holding) continue;

    // How many frames the timeline should contain by now.
    //
    // The audio is the better master once it flows, because the card's sample
    // clock is steadier than the PC's. Until audio appears -- and again if it
    // stops appearing -- the wall clock stands in.
    const uint64_t audioFrames = audioFramesWritten_.load(std::memory_order_relaxed);
    const int64_t now = ClockTicks();
    if (audioFrames != lastAudioSeen_) {
      lastAudioSeen_ = audioFrames;
      lastAudioProgressTicks_ = now;
    }
    const bool audioStalled =
        lastAudioProgressTicks_ != 0 && TicksToSeconds(now - lastAudioProgressTicks_) > 2.0;
    const bool audioIsMaster = audioRate_ > 0 && audioFrames > 0 && !audioStalled;

    if (audioStalled && !audioStallLogged_) {
      audioStallLogged_ = true;
      CAP_WARN("Recording: audio stopped, frame timing continues on the system clock");
    }

    uint64_t target = 0;
    if (audioIsMaster) {
      target = (uint64_t)((double)audioFrames / (double)audioRate_ * fps_);
    } else {
      target = (uint64_t)(TicksToSeconds(now - originTicks_) * fps_);
    }

    const uint64_t written = videoFramesWritten_.load(std::memory_order_relaxed);
    if (written >= target) {
      if (TicksToSeconds(now - gapTicks) > 0.5) {
        caughtUpTicks = now;
        everCaughtUp = true;
      }
      if (encoderBehind_.load(std::memory_order_relaxed) && caughtUpTicks == now) {
        encoderBehind_.store(false, std::memory_order_relaxed);
        CAP_LOG("Recording: the encoder caught up again");
      }
      // On the wall clock the next frame falls due at a known time. On the
      // audio clock the audio thread says when it moved; the timeout is then
      // only there to notice the audio stopping.
      double wait = 0.05;
      if (!audioIsMaster) {
        wait = (double)(written + 1) / fps_ - TicksToSeconds(now - originTicks_);
        wait = std::clamp(wait, 0.001, 0.05);
      }
      waited = true;
      std::unique_lock<std::mutex> lock(frameMutex_);
      if (!readyBuffer_ && readySlot_ < 0 && running_.load(std::memory_order_relaxed)) {
        frameReady_.wait_for(lock, std::chrono::microseconds((int64_t)(wait * 1e6)));
      }
      continue;
    }

    if (!encoderBehind_.load(std::memory_order_relaxed) &&
        TicksToSeconds(now - caughtUpTicks) > (everCaughtUp ? 3.0 : 8.0)) {
      encoderBehind_.store(true, std::memory_order_relaxed);
      CAP_WARN("Recording: the encoder is not keeping up, frames are left out");
    }

    // Every frame goes out at its slot's time -- one placed a slot early would
    // make the video shorter than the audio and desync the file for good. If
    // the encoder stalls, it stalls here, which is exactly where the waiting
    // belongs.
    //
    // Slots it fell behind on are not encoded one by one, though. The held
    // frame repeated into each would cost a whole frame apiece and keep an
    // encoder near its limit behind for good, with the one picture over and
    // over in the file. It goes out once, at the newest slot due; the slots
    // before it are a gap the previous frame stands through, which plays the
    // same as a repeat, and the encoder's time goes to the next fresh frame.
    //
    // Uncompressed goes past the encoder, which would only copy it: the frame's
    // buffer is the packet, and a repeat one more reference to it, so it keeps
    // every slot.
    uint64_t first = written;
    if (!raw && target - written > 1) {
      first = target - 1;
      gapTicks = now;
      videoFramesWritten_.fetch_add(first - written, std::memory_order_relaxed);
      duplicated_.fetch_add(first - written, std::memory_order_relaxed);
      if (!waited && leftOut_.fetch_add(first - written, std::memory_order_relaxed) == 0) {
        CAP_LOG("Recording: %llu slots left out at %.1f s, the encoder was still busy",
                (unsigned long long)(first - written), TicksToSeconds(now - originTicks_));
      }
    }
    waited = false;
    int result = 0;
    for (uint64_t i = first; i < target && running_.load(std::memory_order_relaxed); ++i) {
      if (raw) {
        AVPacket* packet = av.av_packet_alloc();
        if (!packet || !(packet->buf = av.av_buffer_ref(frame->buf[0]))) {
          av.av_packet_free(&packet);
          result = AVERROR(ENOMEM);
          break;
        }
        packet->data = packet->buf->data;
        packet->size = (int)frameBytes_;
        packet->pts = packet->dts = (int64_t)i;
        packet->duration = 1;
        packet->flags = AV_PKT_FLAG_KEY;
        s->Queue(packet, s->videoStream, v->time_base);
      } else {
        frame->pts = (int64_t)i;
        frame->duration = 1;
        LibavClearError();
        result = av.avcodec_send_frame(s->video, frame);
        if (result >= 0) result = s->Drain(s->video, s->videoStream, &s->videoPacket);
        if (result < 0) break;
      }
      videoFramesWritten_.fetch_add(1, std::memory_order_relaxed);
      if (!fresh || i > first) duplicated_.fetch_add(1, std::memory_order_relaxed);
    }
    if (result < 0) {
      Fail(CAP_SAID(Format(T("Encoder-Fehler: %s", "Encoder error: %s"), LibavError(result).c_str())));
      break;
    }
  }
  av.av_frame_free(&frame);
  av.av_frame_free(&next);
  av.av_frame_free(&soft);
}

void Recorder::AudioThread(Session* s) {
  // Nothing to write into before the encoders are open. In short steps: one
  // that never opens must not leave this thread stuck and hang the shutdown.
  while (!ready_.load(std::memory_order_acquire)) {
    if (!running_.load(std::memory_order_relaxed) || failed_.load(std::memory_order_relaxed)) {
      return;
    }
    SleepMilliseconds(2);
  }
  const Libav& av = *s->av;

  std::vector<float> capture(kAudioChunk * 2);
  std::vector<float> in(kAudioChunk * 2);
  std::vector<float> mixed(kAudioChunk * 2);
  // What came before the first frame belongs to no frame.
  if (pullAudio_) {
    while (pullAudio_(capture.data(), kAudioChunk) == kAudioChunk) {
    }
  }
  for (const ExtraAudio& extra : extras_) {
    while (extra.source.pull(in.data(), kAudioChunk) == kAudioChunk) {
    }
  }

  // Every source waits in its feed on the master clock: the capture's when
  // there is a capture, the wall clock otherwise.
  //
  // The devices deliver in bursts of their own size, so the level swings by a
  // burst or two. Its lowest point over each second is held near `floor`, by
  // asking the resampler for a few samples more or fewer: deep enough that a
  // late burst does not run it dry, shallow enough that the source stays
  // within a frame or two of the game.
  const int outRate = audioRate_ > 0 ? audioRate_ : kClockRate;
  const size_t floor = (size_t)outRate * 15 / 1000;
  for (Session::Feed& feed : s->feeds) {
    feed.fifo.assign((size_t)outRate * 2, 0.0f);
    feed.end = floor;  // starts with that much silence
  }
  // Without a capture: when the clock started, and how much of it is written.
  const int64_t clockOrigin = ClockTicks();
  uint64_t clockFrames = 0;

  auto reserve = [](Session::Feed& feed, size_t frames) {
    if (feed.end + frames <= feed.fifo.size() / 2) return;
    std::memmove(feed.fifo.data(), feed.fifo.data() + feed.start * 2,
                 (feed.end - feed.start) * 2 * sizeof(float));
    feed.end -= feed.start;
    feed.start = 0;
    if (feed.end + frames > feed.fifo.size() / 2) feed.fifo.resize((feed.end + frames) * 2);
  };

  // `frames` of the master clock, `master` null on the wall clock: the
  // capture's track, as much of every source, then the mix.
  auto step = [&](const float* master, size_t frames) -> int {
    int result = 0;
    if (master && s->capture >= 0) result = s->Encode(s->tracks[s->capture], master, frames);
    if (s->mix >= 0) {
      if (master) {
        std::memcpy(mixed.data(), master, frames * 2 * sizeof(float));
      } else {
        std::fill(mixed.begin(), mixed.begin() + (ptrdiff_t)(frames * 2), 0.0f);
      }
    }
    for (Session::Feed& feed : s->feeds) {
      if (result < 0) return result;
      // As much as the clock moved, padded with silence when it has not
      // arrived.
      const size_t have = std::min(frames, feed.end - feed.start);
      if (have < frames) {
        reserve(feed, frames - have);
        std::fill(feed.fifo.begin() + (ptrdiff_t)(feed.end * 2),
                  feed.fifo.begin() + (ptrdiff_t)((feed.start + frames) * 2), 0.0f);
        ++feed.underruns;
      }
      const float* data = feed.fifo.data() + feed.start * 2;
      if (feed.mixed) {
        for (size_t i = 0; i < frames * 2; ++i) mixed[i] += data[i];
      }
      if (feed.track >= 0) result = s->Encode(s->tracks[feed.track], data, frames);
      feed.start += have;

      feed.lowest = std::min(feed.lowest, feed.end - feed.start);
      feed.measured += frames;
      if (feed.measured >= (uint64_t)outRate) {
        // A tenth of the distance per second, and never more than 0.2 %:
        // devices drift by hundredths of that, so it settles within seconds
        // and the change of pitch is far below hearing.
        const int limit = outRate / 500;
        const double off = (double)feed.lowest - (double)floor;
        const int delta = std::clamp((int)std::lround(-off * 0.1), -limit, limit);
        av.swr_set_compensation(feed.resampler, delta, outRate);
        feed.lowest = SIZE_MAX;
        feed.measured = 0;
      }
    }
    if (result >= 0 && s->mix >= 0) result = s->Encode(s->tracks[s->mix], mixed.data(), frames);
    return result;
  };

  auto failEncode = [&](int code) {
    Fail(CAP_SAID(Format(T("Audio-Fehler: %s", "Audio error: %s"), LibavError(code).c_str())));
  };

  while (running_.load(std::memory_order_relaxed) && !failed_.load(std::memory_order_relaxed)) {
    bool any = false;

    // The sources first, so that what the clock is about to take is already
    // there.
    for (size_t i = 0; i < extras_.size(); ++i) {
      Session::Feed& feed = s->feeds[i];
      for (;;) {
        const size_t got = extras_[i].source.pull(in.data(), kAudioChunk);
        if (got == 0) break;
        any = true;
        LibavClearError();
        const int room = av.swr_get_out_samples(feed.resampler, (int)got);
        reserve(feed, (size_t)std::max(room, 0));
        uint8_t* out[1] = {(uint8_t*)(feed.fifo.data() + feed.end * 2)};
        const uint8_t* from[1] = {(const uint8_t*)in.data()};
        const int made = av.swr_convert(feed.resampler, out, room, from, (int)got);
        if (made < 0) {
          failEncode(made);
          return;
        }
        feed.end += (size_t)made;
        // The clock has stopped taking: rather than grow without end, keep the
        // newest.
        if (feed.end - feed.start > (size_t)outRate / 2) {
          feed.start = feed.end - floor;
          ++feed.overflows;
        }
        if (got < kAudioChunk) break;
      }
    }

    if (pullAudio_) {
      for (;;) {
        const size_t got = pullAudio_(capture.data(), kAudioChunk);
        if (got == 0) break;
        any = true;
        LibavClearError();
        const int result = step(capture.data(), got);
        if (result < 0) {
          failEncode(result);
          return;
        }
        audioFramesWritten_.fetch_add(got, std::memory_order_relaxed);
        { std::lock_guard<std::mutex> lock(frameMutex_); }
        frameReady_.notify_one();
        if (got < kAudioChunk) break;
      }
    } else {
      // The wall clock: as much silence as time has passed, for the sources
      // to be laid onto.
      const uint64_t due = (uint64_t)(TicksToSeconds(ClockTicks() - clockOrigin) * outRate);
      while (clockFrames < due) {
        const size_t frames = (size_t)std::min<uint64_t>(due - clockFrames, kAudioChunk);
        LibavClearError();
        const int result = step(nullptr, frames);
        if (result < 0) {
          failEncode(result);
          return;
        }
        clockFrames += frames;
      }
    }

    if (!any) SleepMilliseconds(2);
  }

  for (size_t i = 0; i < s->feeds.size(); ++i) {
    const Session::Feed& feed = s->feeds[i];
    if (feed.underruns || feed.overflows) {
      CAP_LOG("Recording: '%s' ran dry %llu times, overflowed %llu times",
              extras_[i].title.c_str(), (unsigned long long)feed.underruns,
              (unsigned long long)feed.overflows);
    }
  }
}

double Recorder::seconds() const {
  return startTicks_ ? TicksToSeconds(ClockTicks() - startTicks_) : 0.0;
}

uint64_t Recorder::queuedBytes() const {
  const int64_t queued = session_ ? session_->queuedBytes.load(std::memory_order_relaxed) : 0;
  return queued > 0 ? (uint64_t)queued : 0;
}

double Recorder::bufferFill() const {
  return session_ ? (double)queuedBytes() /
                        (double)session_->queueLimit.load(std::memory_order_relaxed)
                  : 0.0;
}

void Recorder::LimitBuffer(uint64_t freeBytes) {
  if (!session_ || !session_->settings.largeBuffer) return;
  session_->queueLimit.store(QueueLimit(session_->settings, (int64_t)queuedBytes(), freeBytes),
                             std::memory_order_relaxed);
}

RecordStats Recorder::stats() const {
  RecordStats s;
  s.running = running_.load(std::memory_order_relaxed);
  s.seconds = seconds();
  s.videoFrames = videoFramesWritten_.load(std::memory_order_relaxed);
  s.duplicated = duplicated_.load(std::memory_order_relaxed);
  s.dropped = dropped_.load(std::memory_order_relaxed);
  s.bytesWritten = bytesWritten_.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(infoMutex_);
    s.file = file_;
    s.encoder = encoderLabel_;
    s.error = error_;
  }
  return s;
}

}  // namespace cap
