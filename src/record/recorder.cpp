#include "record/recorder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <system_error>
#include <utility>

#include "record/gpu_frames.h"
#include "record/libav.h"
#include "record/replay_disk.h"
#include "record/track_plan.h"
#include "render/shared_frames.h"

namespace cap {
namespace {

std::string TimestampedName(RecordContainer container, RecordingKind kind) {
  const LocalTime t = NowLocal();
  return Format("%s%s_%04d-%02d-%02d_%02d-%02d-%02d.%s", AppNameUtf8().c_str(),
                kind == RecordingKind::Lossless ? "-raw"
                : kind == RecordingKind::Clip   ? "-clip"
                                                : "",
                t.year, t.month, t.day, t.hour, t.minute, t.second,
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

// What the replay may hold while `held` is in it and `freeBytes` of memory are
// free besides: what was asked, by the same rule as the queue, and never below
// a quarter gigabyte.
int64_t RingLimit(uint64_t asked, int64_t held, uint64_t freeBytes) {
  const int64_t room = held + (int64_t)freeBytes - (int64_t)kRecordBufferReserveBytes;
  return std::max<int64_t>(256ll << 20, std::min((int64_t)asked, room));
}

// The same on the disk, by its free space, with room left for a clip of all of
// it. Anything while that is not known yet.
int64_t DiskRingLimit(int64_t held, uint64_t freeBytes) {
  if (freeBytes == UINT64_MAX) return INT64_MAX;
  const int64_t room = held + (int64_t)freeBytes - (int64_t)kRecordBufferReserveBytes;
  return std::max<int64_t>(256ll << 20, room / 2);
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

// ------------------------------------------------------------------- output

// One file on the session's encoders. The packets arrive in their codec's time
// base and keep it up to the muxer, which rescales them to what its header
// chose; the window below decides which of them belong to the file.
struct Recorder::Output {
  const Libav* av = nullptr;
  std::filesystem::path file;
  bool clip = false;
  AVFormatContext* ctx = nullptr;
  std::vector<AVRational> from;  // the session's bases, per stream
  int videoStream = -1;
  bool flipped = false;  // an AVI's bottom-up RGB (flipped_raw_rgb)
  // The recorder's count of bytes written while this is the file being
  // written, null after. Under the session's routeMutex, as is everything
  // from the window on.
  std::atomic<uint64_t>* counter = nullptr;
  // The session's, raised when this fails or overflows. Null for a clip,
  // which only reports.
  std::atomic<bool>* trouble = nullptr;

  // The muxer's own thread: the disk may pause, the encoders must not.
  std::mutex muxMutex;
  std::condition_variable muxWake;
  std::deque<AVPacket*> queue;
  bool closing = false;
  std::atomic<int64_t> queuedBytes{0};
  std::atomic<int64_t> queueLimit{kSmallQueueBytes};
  std::atomic<bool> writeFailed{false};
  Said error;  // under muxMutex
  // The muxer thread's until it is joined.
  bool created = false;
  bool fileOpen = false;
  bool headerWritten = false;
  std::thread muxer;

  // The window, in the picture's pts. A file from the session's start takes
  // `everything`; one begun later starts at the first keyframe at or after
  // `startAsked`, and a file ends before the first one at or after
  // `endAsked`. The same keyframe ends one file and starts the next, so a
  // split loses nothing and repeats nothing.
  bool everything = false;
  bool started = false;
  int64_t startAsked = 0;
  int64_t start = 0;
  int64_t endAsked = INT64_MAX;
  bool ended = false;
  int64_t end = 0;
  int64_t endedTicks = 0;
  std::vector<int64_t> offset;  // the start in each stream's time base
  // Sound that may belong to the file once the next keyframe says where it
  // starts or ends.
  std::vector<AVPacket*> held;
  // The sound streams that went past the end. Sound leaves its encoder ahead
  // of the picture or behind it, and the file waits for the rest.
  std::vector<char> pastEnd;
  int64_t lastVideo = INT64_MIN;  // the newest picture queued, before the offset
  bool overflow = false;

  // A clip of the replay on the disk: the files its pictures are in, and the
  // muxer's way into them, closed first.
  std::vector<ReplayDisk::Pin> pins;
  ReplayDisk::Reader reader;

  enum class Fate { Drop, Queue, Hold };

  ~Output();
  void MuxLoop();
  void StopMuxer();
  // Keeps a failure for whoever asks and raises `trouble`. Muxer thread.
  void Failed(Said said);
  // Where a packet goes, moving the window on as keyframes pass.
  Fate Take(const AVPacket* packet);
  Fate SoundFate(const AVPacket* packet);
  // Decides on what was held, once the window moved.
  void Settle();
  // Carries out `fate` on a packet of its own; null is out of memory.
  void Keep(AVPacket* packet, Fate fate);
  // Hands one packet to the muxer, timed from the file's start.
  void Queue(AVPacket* packet);
  // Ended, with nothing more to wait for.
  bool Done(int64_t now) const;
  double seconds() const;
  // Ends the muxer and writes the container's index. True when the whole file
  // made it to the disk.
  bool Close();
};

Recorder::Output::~Output() {
  StopMuxer();
  for (AVPacket* packet : queue) av->av_packet_free(&packet);
  for (AVPacket* packet : held) av->av_packet_free(&packet);
  if (ctx) {
    if (fileOpen) av->avio_closep(&ctx->pb);
    av->avformat_free_context(ctx);
  }
}

void Recorder::Output::MuxLoop() {
  // The file is made here, so a slow disk holds up nothing but this thread.
  LibavClearError();
  int result = av->avio_open(&ctx->pb, PathToUtf8(file).c_str(), AVIO_FLAG_WRITE);
  if (result >= 0) {
    created = fileOpen = true;
    // Uncompressed RGB comes bottom-up from the GPU, as a bitmap keeps it, and
    // the header has to say so. Without filters in the card's own row order.
    AVDictionary* header = nullptr;
    if (flipped) av->av_dict_set(&header, "flipped_raw_rgb", "1", 0);
    result = av->avformat_write_header(ctx, &header);
    av->av_dict_free(&header);
    headerWritten = result >= 0;
  }
  if (result < 0) {
    const std::string why = LibavError(result);
    Failed(CAP_SAID(Format(T("Die Datei konnte nicht angelegt werden: %s",
                             "Could not create the file: %s"),
                           why.c_str())));
  }

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
      // A picture of the replay on the disk comes without its data.
      if (!packet->buf && packet->opaque && !writeFailed.load(std::memory_order_relaxed) &&
          !ReplayDisk::Load(*av, packet, &reader)) {
        Failed(CAP_SAID(T("Das Replay auf der Platte ließ sich nicht lesen.",
                          "Could not read the replay from the disk.")));
      }
      if (!writeFailed.load(std::memory_order_relaxed)) {
        const int stream = packet->stream_index;
        av->av_packet_rescale_ts(packet, from[stream], ctx->streams[stream]->time_base);
        LibavClearError();
        result = av->av_interleaved_write_frame(ctx, packet);
        if (result < 0) {
          const std::string why = LibavError(result);
          Failed(CAP_SAID(
              Format(T("Schreiben fehlgeschlagen: %s", "Writing failed: %s"), why.c_str())));
        }
      }
      av->av_packet_free(&packet);
      queuedBytes.fetch_sub(size, std::memory_order_relaxed);
    }
    batch.clear();
  }
}

void Recorder::Output::StopMuxer() {
  if (!muxer.joinable()) return;
  {
    std::lock_guard<std::mutex> lock(muxMutex);
    closing = true;
  }
  muxWake.notify_all();
  muxer.join();
}

void Recorder::Output::Failed(Said said) {
  {
    std::lock_guard<std::mutex> lock(muxMutex);
    error = std::move(said);
  }
  writeFailed.store(true, std::memory_order_release);
  if (trouble) trouble->store(true, std::memory_order_release);
}

Recorder::Output::Fate Recorder::Output::Take(const AVPacket* packet) {
  if (packet->stream_index != videoStream) return SoundFate(packet);
  const bool key = (packet->flags & AV_PKT_FLAG_KEY) != 0;
  if (ended) return Fate::Drop;
  if (!started) {
    if (!key || packet->pts < startAsked) return Fate::Drop;
    if (packet->pts >= endAsked) {
      // Ended before it began: nothing belongs to it.
      ended = true;
      endedTicks = ClockTicks();
      for (AVPacket*& waiting : held) av->av_packet_free(&waiting);
      held.clear();
      return Fate::Drop;
    }
    started = true;
    start = packet->pts;
    for (size_t i = 0; i < from.size(); ++i) {
      offset[i] = av->av_rescale_q(start, from[videoStream], from[i]);
    }
    Settle();
    return Fate::Queue;
  }
  if (key && packet->pts >= endAsked) {
    ended = true;
    end = packet->pts;
    endedTicks = ClockTicks();
    Settle();
    return Fate::Drop;
  }
  // Behind the start in decode order but before it in time: a picture that
  // looks back past the keyframe, at one the file does not have.
  return packet->pts < start ? Fate::Drop : Fate::Queue;
}

Recorder::Output::Fate Recorder::Output::SoundFate(const AVPacket* packet) {
  const int stream = packet->stream_index;
  auto at = [&](int64_t pts) { return av->av_rescale_q(pts, from[videoStream], from[stream]); };
  if (!started) return !ended && packet->pts >= at(startAsked) ? Fate::Hold : Fate::Drop;
  if (!everything && packet->pts < offset[stream]) return Fate::Drop;
  if (ended) {
    if (packet->pts < at(end)) return Fate::Queue;
    pastEnd[stream] = 1;
    return Fate::Drop;
  }
  return endAsked == INT64_MAX || packet->pts < at(endAsked) ? Fate::Queue : Fate::Hold;
}

void Recorder::Output::Settle() {
  std::vector<AVPacket*> waiting;
  waiting.swap(held);
  for (AVPacket* packet : waiting) Keep(packet, SoundFate(packet));
}

void Recorder::Output::Keep(AVPacket* packet, Fate fate) {
  if (!packet) return;
  if (fate == Fate::Queue) {
    Queue(packet);
  } else if (fate == Fate::Hold) {
    held.push_back(packet);
  } else {
    av->av_packet_free(&packet);
  }
}

void Recorder::Output::Queue(AVPacket* packet) {
  const int stream = packet->stream_index;
  if (stream == videoStream) lastVideo = std::max(lastVideo, packet->pts);
  if (packet->pts != AV_NOPTS_VALUE) packet->pts -= offset[stream];
  if (packet->dts != AV_NOPTS_VALUE) packet->dts -= offset[stream];
  const int size = packet->size;
  if (counter) counter->fetch_add((uint64_t)size, std::memory_order_relaxed);
  const int64_t queued = queuedBytes.fetch_add(size, std::memory_order_relaxed) + size;
  {
    std::lock_guard<std::mutex> lock(muxMutex);
    queue.push_back(packet);
  }
  muxWake.notify_one();
  // What is in it still goes to the disk, as the file closes.
  if (trouble && !overflow && queued > queueLimit.load(std::memory_order_relaxed)) {
    overflow = true;
    trouble->store(true, std::memory_order_release);
  }
}

bool Recorder::Output::Done(int64_t now) const {
  if (!ended) return false;
  if (!started) return true;
  for (size_t i = 0; i < pastEnd.size(); ++i) {
    if ((int)i != videoStream && !pastEnd[i]) return TicksToSeconds(now - endedTicks) > 5.0;
  }
  return true;
}

double Recorder::Output::seconds() const {
  if (lastVideo == INT64_MIN) return 0.0;
  return (double)(lastVideo - start + 1) * av_q2d(from[videoStream]);
}

bool Recorder::Output::Close() {
  StopMuxer();
  bool complete = headerWritten && !writeFailed.load(std::memory_order_acquire);
  // A file that never got a picture has nothing to index, and goes.
  if (headerWritten && lastVideo != INT64_MIN) {
    LibavClearError();
    const int trailer = av->av_write_trailer(ctx);
    if (trailer < 0) {
      CAP_ERR("Recording: could not finish the file: %s", LibavError(trailer).c_str());
      complete = false;
    }
  }
  if (fileOpen) {
    fileOpen = false;
    if (av->avio_closep(&ctx->pb) < 0) complete = false;
  }
  return complete;
}

// ------------------------------------------------------------------ session

struct Recorder::Session {
  const Libav* av = nullptr;
  RecordSettings settings;
  EncoderInfo encoder;
  AVRational rate = {60, 1};

  // The streams every file copies, itself never written, and the time base
  // each stream's packets come in.
  AVFormatContext* proto = nullptr;
  std::vector<AVRational> bases;
  bool flipped = false;  // Output::flipped

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

  // Where the packets go, for the encoders' threads and the control thread
  // alike: the files, the replay, the last second of sound.
  std::mutex routeMutex;
  bool opened = false;  // the encoders are; until then `file` waits here
  std::filesystem::path file;
  int64_t liveLimit = kSmallQueueBytes;  // what the file being written may queue
  std::vector<std::unique_ptr<Output>> outputs;
  Output* live = nullptr;  // the file being written, among them
  // Ended ones, for RetireDone to take to the closing thread.
  std::vector<std::unique_ptr<Output>> done;
  std::atomic<bool> anyDone{false};
  // The next picture sent goes out as a keyframe: a file begins or ends.
  std::atomic<bool> keyWanted{false};
  std::atomic<bool> trouble{false};  // Output::trouble
  int64_t latestVideo = INT64_MIN;   // the newest pts out of the encoder
  // Sound leaves its encoder ahead of the picture. A file begun now still
  // wants it: the last second, by reference.
  std::deque<AVPacket*> recentSound;
  std::vector<Output::Fate> fates;  // Route's, kept for the allocation

  // The replay: whole keyframe intervals, the oldest let go while the next
  // one still leaves `keep` (in the picture's pts) or the bytes are over
  // `limit`. The front is always a keyframe.
  struct Ring {
    bool on = false;
    int64_t keep = 0;
    uint64_t asked = 0;
    int64_t limit = 0;
    int64_t bytes = 0;
    int64_t newest = INT64_MIN;
    std::deque<AVPacket*> video;
    std::deque<uint64_t> keys;  // keyframes, counted from the first ever kept
    uint64_t popped = 0;        // let go from the front so far
    std::deque<AVPacket*> audio;
    // The pictures on the disk instead, in `folder`; made the first time and
    // gone with the session, on the closing thread.
    bool onDisk = false;
    std::filesystem::path folder;
    std::unique_ptr<ReplayDisk> disk;
    // Gave up, and why; under routeMutex. For TakeReplayFailure.
    std::atomic<bool> failed{false};
    Said failure;
  } ring;

  // What Stop saw, for the closing thread's line in the log.
  bool wasRunning = false;
  unsigned long long stoppedFrames = 0;
  unsigned long long stoppedLeftOut = 0;

  ~Session();
  // Takes what `codec` has finished and routes it.
  int Drain(AVCodecContext* codec, int stream, AVPacket** spare);
  // Hands one packet to every output that takes it, the replay and the last
  // second of sound; whatever ended moves to `done`.
  void Route(AVPacket* packet, int stream);
  // Appends interleaved stereo to a track, sending each frame as it fills.
  int Encode(Track& track, const float* interleaved, size_t frames);
  // Lets the encoders finish into the outputs. Closing thread.
  void Flush();
  // A file with the session's streams and its muxer running; null when out of
  // memory.
  std::unique_ptr<Output> NewOutput(const std::filesystem::path& path, bool clip);
  // The file being written from the next keyframe on, ending the one before
  // there. Under routeMutex, once opened.
  bool AddFile(const std::filesystem::path& path, std::atomic<uint64_t>* counter);
  // The replay's, under routeMutex.
  void Remember(AVPacket* packet);
  void DropInterval();
  void ClearRing();
  // Off for good, its disk failed or fell behind.
  void GiveUpRing(Said why);
};

Recorder::Session::~Session() {
  // First: their muxers may still write to `trouble`.
  outputs.clear();
  done.clear();
  if (!av) return;
  ClearRing();
  for (AVPacket* packet : recentSound) av->av_packet_free(&packet);
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
  if (proto) av->avformat_free_context(proto);
}

int Recorder::Session::Drain(AVCodecContext* codec, int stream, AVPacket** spare) {
  for (;;) {
    if (!*spare && !(*spare = av->av_packet_alloc())) return AVERROR(ENOMEM);
    const int result = av->avcodec_receive_packet(codec, *spare);
    if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return 0;
    if (result < 0) return result;
    AVPacket* packet = *spare;
    *spare = nullptr;
    Route(packet, stream);
  }
}

void Recorder::Session::Route(AVPacket* packet, int stream) {
  packet->stream_index = stream;
  const bool isVideo = stream == videoStream;
  std::lock_guard<std::mutex> lock(routeMutex);
  if (isVideo) latestVideo = std::max(latestVideo, packet->pts);
  const bool toRing =
      ring.on && (!isVideo || !ring.video.empty() || (packet->flags & AV_PKT_FLAG_KEY) != 0);
  const bool toRecent = !isVideo;
  size_t takers = (toRing ? 1 : 0) + (toRecent ? 1 : 0);
  fates.resize(outputs.size());
  for (size_t i = 0; i < outputs.size(); ++i) {
    fates[i] = outputs[i]->Take(packet);
    if (fates[i] != Output::Fate::Drop) ++takers;
  }
  if (takers == 0) av->av_packet_free(&packet);
  // Every taker but the last gets a reference of its own, made before the
  // packet's times change in an output.
  auto share = [&]() { return --takers == 0 ? packet : av->av_packet_clone(packet); };
  bool anyEnded = false;
  for (size_t i = 0; i < outputs.size(); ++i) {
    if (fates[i] != Output::Fate::Drop) outputs[i]->Keep(share(), fates[i]);
    anyEnded |= outputs[i]->ended;
  }
  if (toRing) Remember(share());
  if (toRecent) {
    const int64_t oldest = packet->pts - av->av_rescale_q(1, AVRational{1, 1}, bases[stream]);
    while (!recentSound.empty() && recentSound.front()->pts < oldest) {
      AVPacket* old = recentSound.front();
      recentSound.pop_front();
      av->av_packet_free(&old);
    }
    if (AVPacket* kept = share()) recentSound.push_back(kept);
  }

  if (!anyEnded) return;
  const int64_t now = ClockTicks();
  for (size_t i = 0; i < outputs.size();) {
    if (!outputs[i]->Done(now)) {
      ++i;
      continue;
    }
    if (outputs[i].get() == live) live = nullptr;
    done.push_back(std::move(outputs[i]));
    outputs.erase(outputs.begin() + (ptrdiff_t)i);
    anyDone.store(true, std::memory_order_release);
  }
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

void Recorder::Session::Flush() {
  {
    std::lock_guard<std::mutex> lock(routeMutex);
    ring.on = false;
    ClearRing();
  }
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
  // Sound held for an end that never came: the file runs to the last
  // picture, and the sound to it belongs in it.
  std::lock_guard<std::mutex> lock(routeMutex);
  for (const std::unique_ptr<Output>& out : outputs) {
    std::vector<AVPacket*> waiting;
    waiting.swap(out->held);
    for (AVPacket* packet : waiting) {
      out->Keep(packet, out->started ? Output::Fate::Queue : Output::Fate::Drop);
    }
  }
}

std::unique_ptr<Recorder::Output> Recorder::Session::NewOutput(const std::filesystem::path& path,
                                                               bool clip) {
  auto out = std::make_unique<Output>();
  out->av = av;
  out->file = path;
  out->clip = clip;
  out->videoStream = videoStream;
  out->flipped = flipped;
  out->trouble = clip ? nullptr : &trouble;
  out->from = bases;
  out->offset.assign(bases.size(), 0);
  out->pastEnd.assign(bases.size(), 0);
  const std::string name = PathToUtf8(path);
  LibavClearError();
  if (av->avformat_alloc_output_context2(&out->ctx, proto->oformat, nullptr, name.c_str()) < 0 ||
      !out->ctx) {
    return nullptr;
  }
  for (unsigned i = 0; i < proto->nb_streams; ++i) {
    const AVStream* src = proto->streams[i];
    AVStream* to = av->avformat_new_stream(out->ctx, nullptr);
    if (!to || av->avcodec_parameters_copy(to->codecpar, src->codecpar) < 0) return nullptr;
    to->time_base = src->time_base;
    to->avg_frame_rate = src->avg_frame_rate;
    to->disposition = src->disposition;
    av->av_dict_copy(&to->metadata, src->metadata, 0);
  }
  out->muxer = std::thread(&Output::MuxLoop, out.get());
  return out;
}

bool Recorder::Session::AddFile(const std::filesystem::path& path,
                                std::atomic<uint64_t>* counter) {
  std::unique_ptr<Output> out = NewOutput(path, false);
  if (!out) return false;
  if (latestVideo == INT64_MIN) {
    out->everything = out->started = true;
  } else {
    out->startAsked = latestVideo + 1;
  }
  if (live) {
    live->endAsked = out->startAsked;
    live->counter = nullptr;
  }
  out->queueLimit.store(liveLimit, std::memory_order_relaxed);
  out->counter = counter;
  for (AVPacket* packet : recentSound) {
    const Output::Fate fate = out->SoundFate(packet);
    if (fate != Output::Fate::Drop) out->Keep(av->av_packet_clone(packet), fate);
  }
  live = out.get();
  outputs.push_back(std::move(out));
  return true;
}

void Recorder::Session::Remember(AVPacket* packet) {
  if (!packet) return;
  ring.bytes += packet->size;
  if (packet->stream_index != videoStream) {
    ring.audio.push_back(packet);
    // Before the first keyframe only the last second, for the one to come.
    if (ring.video.empty()) {
      const int64_t oldest =
          packet->pts - av->av_rescale_q(1, AVRational{1, 1}, bases[packet->stream_index]);
      while (ring.audio.front()->pts < oldest) {
        AVPacket* old = ring.audio.front();
        ring.audio.pop_front();
        ring.bytes -= old->size;
        av->av_packet_free(&old);
      }
    }
    return;
  }
  if (ring.onDisk) {
    // Its data goes to the disk's thread. One that falls behind by as much as
    // a file may queue would only run out of memory.
    ReplayDisk& disk = *ring.disk;
    Said why;
    if (disk.failed(&why) || disk.pending() > liveLimit) {
      if (why.shown.empty()) {
        why = CAP_SAID(T("Die Platte kommt mit dem Replay nicht hinterher.",
                         "The disk cannot keep up with the replay."));
      }
      av->av_packet_free(&packet);
      GiveUpRing(std::move(why));
      return;
    }
    packet = disk.Put(packet);
    if (!packet) return;
    ring.limit = DiskRingLimit(ring.bytes, disk.freeBytes());
  }
  if (packet->flags & AV_PKT_FLAG_KEY) ring.keys.push_back(ring.popped + ring.video.size());
  ring.video.push_back(packet);
  ring.newest = std::max(ring.newest, packet->pts);
  bool dropped = false;
  while (ring.keys.size() >= 2) {
    const int64_t next = ring.video[(size_t)(ring.keys[1] - ring.popped)]->pts;
    if (next > ring.newest - ring.keep && ring.bytes <= ring.limit) break;
    DropInterval();
    dropped = true;
  }
  if (!dropped) return;
  if (ring.onDisk) ring.disk->Trim(ring.video.front());
  // The sound from before the picture that is now the first.
  const int64_t first = ring.video.front()->pts;
  while (!ring.audio.empty()) {
    AVPacket* old = ring.audio.front();
    if (old->pts >= av->av_rescale_q(first, bases[videoStream], bases[old->stream_index])) break;
    ring.audio.pop_front();
    ring.bytes -= old->size;
    av->av_packet_free(&old);
  }
}

void Recorder::Session::DropInterval() {
  const uint64_t until = ring.keys[1];
  while (ring.popped < until) {
    AVPacket* old = ring.video.front();
    ring.video.pop_front();
    ring.bytes -= old->size;
    av->av_packet_free(&old);
    ++ring.popped;
  }
  ring.keys.pop_front();
}

void Recorder::Session::ClearRing() {
  for (AVPacket* packet : ring.video) av->av_packet_free(&packet);
  for (AVPacket* packet : ring.audio) av->av_packet_free(&packet);
  ring.video.clear();
  ring.audio.clear();
  ring.keys.clear();
  ring.popped = 0;
  ring.bytes = 0;
  ring.newest = INT64_MIN;
  if (ring.disk) ring.disk->Clear();
}

void Recorder::Session::GiveUpRing(Said why) {
  ring.on = false;
  ClearRing();
  ring.disk->Stop();
  ring.onDisk = false;  // switched on again, it starts afresh
  ring.failure = std::move(why);
  ring.failed.store(true, std::memory_order_release);
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

  // ---- every encoder chip on the card ----
  // H.264 cannot be split. For the others the driver makes one strip per chip.
  if (family == Family::Nvenc && settings.allEncoderChips &&
      encoder.ffmpegName.compare(0, 4, "h264") != 0) {
    pair("split_encode_mode", "forced");
  }

  return out;
}

// -------------------------------------------------------------------- start

std::filesystem::path NextRecordingFile(const std::filesystem::path& folder,
                                        RecordContainer container, RecordingKind kind) {
  std::filesystem::path dir = folder;
  if (kind != RecordingKind::Normal) {
    dir /= kind == RecordingKind::Lossless ? "raw" : "clips";
    std::error_code made;
    std::filesystem::create_directories(dir, made);
    if (made) dir = folder;
  }
  const std::filesystem::path named = dir / Utf8ToPath(TimestampedName(container, kind));
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
  // Nothing is reserved: the queue only grows while the disk is behind.
  if (settings.largeBuffer) {
    uint64_t freeBytes = 0;
    const int64_t limit = MemoryBytes(nullptr, &freeBytes)
                              ? QueueLimit(settings, 0, freeBytes)
                              : (int64_t)settings.bufferGb << 30;
    session->liveLimit = limit;
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
  recording_.store(!outFile.empty(), std::memory_order_relaxed);
  replaying_.store(false, std::memory_order_relaxed);
  if (audioRate_ > 0 || !extras_.empty()) {
    audioThread_ = std::thread(&Recorder::AudioThread, this, session_.get());
  }
  // The encoder opens there and not here: NVENC takes a tenth of a second and
  // more, and the picture would wait for it.
  videoThread_ = std::thread(&Recorder::VideoThread, this, session_.get());

  CAP_LOG("Recording started: %s, %dx%d @ %.3f fps, %s",
          outFile.empty() ? "replay only" : PathToUtf8(outFile).c_str(), width_, height_, fps_,
          encoder->label.c_str());
  if (unfiltered_) {
    CAP_LOG("Recording: unfiltered, card %s %dx%d as %s%s", card_.format.subtypeLabel.c_str(),
            card_.format.width, card_.format.height, plan_.pixelFormat,
            plan_.repack == CardRepack::None ? "" : ", repacked");
  }
  return true;
}

bool Recorder::Open(Session* s) {
  const Libav& av = *s->av;
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
      &s->proto, nullptr,
      container == RecordContainer::Mp4 ? "mp4" : avi ? "avi" : "matroska", nullptr);
  if (result < 0 || !s->proto) {
    return failed("Die Datei konnte nicht angelegt werden: %s", "Could not create the file: %s",
                  result);
  }
  const bool globalHeader = (s->proto->oformat->flags & AVFMT_GLOBALHEADER) != 0;

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
    // A file begins on a keyframe asked for (Session::keyWanted), and a player
    // has to be able to start there: NVENC and AMF make it one that nothing
    // behind it looks past only when told.
    for (const char* idr : {"forced-idr", "forced_idr"}) {
      if (v->priv_data && av.av_opt_find(v->priv_data, idr, nullptr, 0, 0)) {
        av.av_dict_set(&options, idr, "1", 0);
      }
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

  AVStream* stream = av.avformat_new_stream(s->proto, nullptr);
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
    AVStream* st = av.avformat_new_stream(s->proto, nullptr);
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

  // What each stream's packets are timed in as they leave their encoder.
  s->bases.assign(s->proto->nb_streams, AVRational{1, 1});
  s->bases[(size_t)s->videoStream] = v->time_base;
  for (const Session::Track& track : s->tracks) {
    s->bases[(size_t)track.stream] = track.codec->time_base;
  }
  s->flipped = avi && (!unfiltered_ || card_.format.bottomUp);

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

  // ---- the file ----
  // Asked for at Start, unless StartFile came in the meantime or StopFile
  // took it back. It gets everything from the first frame.
  bool added = true;
  {
    std::lock_guard<std::mutex> lock(s->routeMutex);
    s->opened = true;
    if (!s->file.empty()) added = s->AddFile(s->file, &bytesWritten_);
  }
  if (!added) {
    return failed("Die Datei konnte nicht angelegt werden: %s", "Could not create the file: %s",
                  AVERROR(ENOMEM));
  }

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

  recording_.store(false, std::memory_order_relaxed);
  replaying_.store(false, std::memory_order_relaxed);
  if (session_) {
    std::unique_ptr<Session> session = std::move(session_);
    {
      // What the encoders still let out from here does not count towards the
      // next file.
      std::lock_guard<std::mutex> lock(session->routeMutex);
      for (const std::unique_ptr<Output>& out : session->outputs) out->counter = nullptr;
      session->live = nullptr;
    }
    // Freed once the encoder lets go of the last frame it holds.
    if (session->pool) av->av_buffer_pool_uninit(&session->pool);
    if (session->repacked) av->av_buffer_pool_uninit(&session->repacked);
    session->wasRunning = wasRunning;
    session->stoppedFrames = videoFramesWritten_.load(std::memory_order_relaxed);
    session->stoppedLeftOut = leftOut_.load(std::memory_order_relaxed);
    CloseLater(std::move(session), {});
  }
  pullAudio_ = nullptr;
  extras_.clear();
}

void Recorder::CloseLater(std::unique_ptr<Session> session,
                          std::vector<std::unique_ptr<Output>> outputs) {
  // The encoders give up what they still hold and the containers write their
  // index -- an MP4 that never gets that far has no moov atom and does not
  // play at all.
  std::thread previous = std::move(closer_);
  closer_ = std::thread([this, previous = std::move(previous), session = std::move(session),
                         outputs = std::move(outputs)]() mutable {
    if (previous.joinable()) previous.join();
    if (session) {
      if (session->opened) session->Flush();
      std::lock_guard<std::mutex> lock(session->routeMutex);
      for (auto* from : {&session->done, &session->outputs}) {
        for (std::unique_ptr<Output>& out : *from) outputs.push_back(std::move(out));
        from->clear();
      }
    }
    for (std::unique_ptr<Output>& out : outputs) {
      const bool wrote = out->lastVideo != INT64_MIN;
      Finished done;
      done.file = PathToUtf8(out->file);
      done.seconds = out->seconds();
      done.clip = out->clip;
      done.complete = out->Close();
      if (!done.complete) {
        CAP_WARN("Recording: %s was not finished cleanly%s%s", done.file.c_str(),
                 out->error.logged.empty() ? "" : ": ", out->error.logged.c_str());
      }
      // Opened and never begun: nothing in it worth keeping. A clip says so,
      // as it was asked for.
      if (!wrote && out->created) {
        std::error_code ignored;
        std::filesystem::remove(out->file, ignored);
      }
      out.reset();
      if (!wrote && !done.clip) continue;
      if (!wrote) done.complete = false;
      std::lock_guard<std::mutex> lock(finishedMutex_);
      finished_.push_back(std::move(done));
      anyFinished_.store(true, std::memory_order_release);
    }
    if (session && session->wasRunning) {
      CAP_LOG("Recording stopped, %llu frames, %llu left out", session->stoppedFrames,
              session->stoppedLeftOut);
    }
  });
}

void Recorder::RetireDone() {
  if (!session_ || !session_->anyDone.load(std::memory_order_acquire)) return;
  std::vector<std::unique_ptr<Output>> ended;
  {
    std::lock_guard<std::mutex> lock(session_->routeMutex);
    ended.swap(session_->done);
    session_->anyDone.store(false, std::memory_order_relaxed);
  }
  if (!ended.empty()) CloseLater(nullptr, std::move(ended));
}

void Recorder::StartFile(const std::filesystem::path& file) {
  Session* s = session_.get();
  if (!s) return;
  bool added = true;
  {
    std::lock_guard<std::mutex> lock(s->routeMutex);
    bytesWritten_.store(0, std::memory_order_relaxed);
    if (s->opened) {
      added = s->AddFile(file, &bytesWritten_);
    } else {
      s->file = file;
    }
  }
  if (!added) {
    Fail(CAP_SAID(T("Kein Speicher für die Aufnahme.", "Out of memory for the recording.")));
    return;
  }
  s->keyWanted.store(true, std::memory_order_relaxed);
  startTicks_ = ClockTicks();
  {
    std::lock_guard<std::mutex> lock(infoMutex_);
    file_ = PathToUtf8(file);
  }
  recording_.store(true, std::memory_order_relaxed);
  CAP_LOG("Recording: file %s", PathToUtf8(file).c_str());
}

void Recorder::StopFile() {
  Session* s = session_.get();
  if (!s) return;
  {
    std::lock_guard<std::mutex> lock(s->routeMutex);
    if (!s->opened) s->file.clear();
    if (Output* live = s->live) {
      // Up to the newest picture out of the encoder, and what it still holds
      // up to the keyframe asked for.
      live->endAsked = s->latestVideo == INT64_MIN ? 0 : s->latestVideo + 1;
      live->counter = nullptr;
      s->live = nullptr;
    }
  }
  s->keyWanted.store(true, std::memory_order_relaxed);
  recording_.store(false, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(infoMutex_);
  file_.clear();
}

void Recorder::SetReplay(double seconds, uint64_t bytes, const std::filesystem::path& disk) {
  Session* s = session_.get();
  if (!s) return;
  uint64_t freeBytes = 0;
  const bool measured = MemoryBytes(nullptr, &freeBytes);
  std::lock_guard<std::mutex> lock(s->routeMutex);
  Session::Ring& ring = s->ring;
  if (seconds <= 0.0) {
    ring.on = false;
    s->ClearRing();
    if (ring.onDisk) ring.disk->Stop();
    ring.onDisk = false;
    replaying_.store(false, std::memory_order_relaxed);
    return;
  }
  // Somewhere else now: what it held there goes.
  const bool onDisk = !disk.empty();
  if (onDisk != ring.onDisk || (onDisk && disk != ring.folder)) {
    if (ring.on) {
      ring.on = false;
      s->ClearRing();
    }
    if (ring.onDisk) ring.disk->Stop();
    ring.onDisk = onDisk;
    ring.folder = disk;
    if (onDisk) {
      if (!ring.disk) ring.disk = std::make_unique<ReplayDisk>(s->av);
      // Files of a second or so, whatever the keyframes.
      ring.disk->Start(disk, std::llround((double)s->rate.num / s->rate.den));
    }
  }
  ring.asked = bytes;
  if (onDisk) {
    ring.limit = DiskRingLimit(ring.bytes, ring.disk->freeBytes());
  } else {
    ring.limit = measured ? RingLimit(bytes, ring.bytes, freeBytes) : (int64_t)bytes;
  }
  ring.keep = std::llround(seconds * s->rate.num / s->rate.den);
  if (!ring.on) {
    // The sound ahead of the next keyframe, as a file begun now gets it.
    for (AVPacket* packet : s->recentSound) {
      if (AVPacket* kept = s->av->av_packet_clone(packet)) {
        ring.bytes += kept->size;
        ring.audio.push_back(kept);
      }
    }
    ring.on = true;
    // It begins on a keyframe; on encoders already running for a file, the
    // next one is asked for rather than waited for.
    s->keyWanted.store(true, std::memory_order_relaxed);
    if (onDisk) {
      CAP_LOG("Recording: replay of %.0f s on the disk, in %s", seconds,
              PathToUtf8(disk).c_str());
    } else {
      CAP_LOG("Recording: replay of %.0f s, up to %s", seconds,
              FormatBytes((uint64_t)ring.limit).c_str());
    }
  }
  replaying_.store(true, std::memory_order_relaxed);
}

bool Recorder::SaveReplay(const std::filesystem::path& file, double* seconds) {
  Session* s = session_.get();
  if (!s) return false;
  const Libav* av = s->av;
  std::lock_guard<std::mutex> lock(s->routeMutex);
  Session::Ring& ring = s->ring;
  if (!ring.on || ring.video.empty()) return false;
  const AVRational vtb = s->bases[(size_t)s->videoStream];

  // The pictures leave the encoder in decode order. The clip ends where every
  // picture up to the newest one has come: behind it, a B-frame may still be
  // on its way.
  const int64_t first = ring.video.front()->pts;
  size_t cut = 0;
  int64_t end = first;
  int64_t top = INT64_MIN;
  int64_t count = 0;
  for (size_t i = 0; i < ring.video.size(); ++i) {
    const int64_t pts = ring.video[i]->pts;
    if (pts < first) continue;
    top = std::max(top, pts);
    if (++count == top - first + 1) {
      cut = i + 1;
      end = top + 1;
    }
  }
  if (cut == 0) return false;

  std::unique_ptr<Output> out = s->NewOutput(file, true);
  if (!out) return false;
  out->started = out->ended = true;
  out->start = out->startAsked = first;
  out->endAsked = out->end = end;
  out->endedTicks = ClockTicks();
  for (size_t i = 0; i < out->from.size(); ++i) {
    out->offset[i] = av->av_rescale_q(first, vtb, out->from[i]);
  }
  // Before the first picture reaches its muxer.
  if (ring.onDisk) out->pins = ring.disk->PinAll();

  // Sound and picture in the order of their time, as a container interleaves
  // them best: before each picture the sound up to it.
  std::vector<AVPacket*> sound;
  for (AVPacket* packet : ring.audio) {
    if (out->SoundFate(packet) == Output::Fate::Drop) continue;
    if (AVPacket* kept = av->av_packet_clone(packet)) sound.push_back(kept);
  }
  size_t next = 0;
  for (size_t i = 0; i < cut; ++i) {
    const AVPacket* picture = ring.video[i];
    if (picture->pts < first) continue;
    const int64_t at = picture->dts != AV_NOPTS_VALUE ? picture->dts : picture->pts;
    while (next < sound.size() &&
           av->av_rescale_q(sound[next]->pts, out->from[(size_t)sound[next]->stream_index], vtb) <=
               at) {
      out->Queue(sound[next++]);
    }
    AVPacket* kept = ring.onDisk ? ReplayDisk::Copy(*av, picture) : av->av_packet_clone(picture);
    if (kept) out->Queue(kept);
  }
  while (next < sound.size()) out->Queue(sound[next++]);

  const double length = (double)(end - first) * av_q2d(vtb);
  if (seconds) *seconds = length;
  CAP_LOG("Recording: clip %s, %.1f s", PathToUtf8(file).c_str(), length);
  // The sound still to come up to its end arrives through Route, which also
  // lets it go once that is in.
  s->outputs.push_back(std::move(out));
  return true;
}

bool Recorder::TakeFinished(Finished* out) {
  RetireDone();
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
      bool converted = false;
      if (result >= 0) {
        converted = s->gpu->Convert(slot, next, running_);  // gives the slot back
      } else {
        shared_->state[slot].store(SharedFrames::Free, std::memory_order_release);
      }
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
    // Only the file being written stops the encoders. One that already ended
    // says so when it is closed (Finished::complete), and the replay runs on.
    if (s->trouble.exchange(false, std::memory_order_acquire)) {
      Said said;
      {
        std::lock_guard<std::mutex> lock(s->routeMutex);
        if (Output* live = s->live) {
          if (live->writeFailed.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> muxLock(live->muxMutex);
            said = live->error;
          } else if (live->overflow) {
            said = s->settings.largeBuffer
                       ? CAP_SAID(T("Puffer voll: die Platte kommt mit dem Schreiben nicht hinterher.",
                                    "Buffer full: the disk cannot keep up with the writing."))
                       : CAP_SAID(T("Die Platte kommt mit dem Schreiben nicht hinterher. Ein "
                                    "größerer Puffer lässt sich unter Aufnahme einstellen.",
                                    "The disk cannot keep up with the writing. A larger buffer "
                                    "can be set under Recording."));
          }
        }
      }
      if (!said.shown.empty()) {
        Fail(said);
        break;
      }
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
        s->Route(packet, s->videoStream);
      } else {
        frame->pts = (int64_t)i;
        frame->duration = 1;
        // A file begins or ends here (StartFile, StopFile).
        const bool key = s->keyWanted.load(std::memory_order_relaxed) &&
                         s->keyWanted.exchange(false, std::memory_order_relaxed);
        frame->pict_type = key ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        LibavClearError();
        result = av.avcodec_send_frame(s->video, frame);
        frame->pict_type = AV_PICTURE_TYPE_NONE;
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
  if (!session_) return 0;
  int64_t queued = 0;
  std::lock_guard<std::mutex> lock(session_->routeMutex);
  for (auto* list : {&session_->outputs, &session_->done}) {
    for (const std::unique_ptr<Output>& out : *list) {
      queued += out->queuedBytes.load(std::memory_order_relaxed);
    }
  }
  return queued > 0 ? (uint64_t)queued : 0;
}

uint64_t Recorder::replayBytes(double* seconds, bool* onDisk) const {
  if (seconds) *seconds = 0.0;
  if (onDisk) *onDisk = false;
  if (!session_) return 0;
  Session& s = *session_;
  std::lock_guard<std::mutex> lock(s.routeMutex);
  if (onDisk) *onDisk = s.ring.onDisk;
  if (seconds && !s.ring.video.empty()) {
    *seconds = (double)(s.ring.newest + 1 - s.ring.video.front()->pts) *
               av_q2d(s.bases[(size_t)s.videoStream]);
  }
  return s.ring.bytes > 0 ? (uint64_t)s.ring.bytes : 0;
}

double Recorder::bufferFill() const {
  if (!session_) return 0.0;
  std::lock_guard<std::mutex> lock(session_->routeMutex);
  const Output* live = session_->live;
  return live ? (double)live->queuedBytes.load(std::memory_order_relaxed) /
                    (double)live->queueLimit.load(std::memory_order_relaxed)
              : 0.0;
}

void Recorder::LimitBuffer(const RecordSettings& settings, uint64_t freeBytes) {
  if (!session_) return;
  Session& s = *session_;
  std::lock_guard<std::mutex> lock(s.routeMutex);
  // Both are only read under this lock once the session runs.
  const bool wasLarge = s.settings.largeBuffer;
  s.settings.largeBuffer = settings.largeBuffer;
  s.settings.bufferGb = settings.bufferGb;
  if (s.settings.largeBuffer || wasLarge) {
    const int64_t queued = s.live ? s.live->queuedBytes.load(std::memory_order_relaxed) : 0;
    s.liveLimit = QueueLimit(s.settings, queued, freeBytes);
    if (s.live) s.live->queueLimit.store(s.liveLimit, std::memory_order_relaxed);
  }
  // On the disk, Remember goes by the disk's free space.
  if (s.ring.on && !s.ring.onDisk) s.ring.limit = RingLimit(s.ring.asked, s.ring.bytes, freeBytes);
}

bool Recorder::TakeReplayFailure(std::string* why) {
  Session* s = session_.get();
  if (!s || !s->ring.failed.load(std::memory_order_acquire)) return false;
  Said said;
  {
    std::lock_guard<std::mutex> lock(s->routeMutex);
    said = std::move(s->ring.failure);
    s->ring.failed.store(false, std::memory_order_relaxed);
  }
  replaying_.store(false, std::memory_order_relaxed);
  CAP_WARN("Recording: the replay stopped: %s", said.logged.c_str());
  *why = std::move(said.shown);
  return true;
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
