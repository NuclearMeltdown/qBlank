#pragma once

// Writes what the viewer shows to a file, with the ffmpeg libraries inside the
// process (libav.h): the encoders take the frames and samples as they are, and
// the muxer writes them out.
//
// Two rules shape the whole design:
//
//   1. The display path is never blocked. PushVideo copies the frame into a
//      buffer of its own and returns; encoding and writing happen on threads
//      of their own. If the encoder cannot keep up, frames are dropped from the
//      *recording*, never from the screen.
//
//   2. Audio is the master clock. The card's clock and the PC's clock drift
//      apart over an hour, so the video timeline is derived from the number of
//      audio samples written, duplicating or dropping frames to match. That
//      makes sync arithmetic instead of hope, and produces constant frame rate
//      output, which both MKV and MP4 prefer.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "i18n.h"
#include "capture/video_format.h"
#include "record/card_pixels.h"
#include "record/ffmpeg_locator.h"
#include "render/yuv_format.h"

struct AVBufferRef;

namespace cap {

class SharedFrames;

// Pulls up to `frames` interleaved stereo float frames; returns how many it
// actually delivered. Called from the recorder's own thread.
using AudioPullFn = std::function<size_t(float* out, size_t frames)>;

struct RecordStats {
  bool running = false;
  double seconds = 0.0;
  uint64_t videoFrames = 0;
  uint64_t duplicated = 0;  // written twice to hold the audio timeline
  uint64_t dropped = 0;     // arrived faster than the timeline needed
  uint64_t bytesWritten = 0;
  std::string file;
  std::string encoder;
  std::string error;
};

// Roughly what a second of recording costs on disk, for the remaining time
// estimate. The video bitrate is what the encoder is asked for; every audio
// track is the 192 kbit/s the recorder fixes, or PCM in an AVI. A running
// recording has a measured rate and does not need this -- this is for the
// number shown before anything is running, where there is nothing to measure.
//
// Under RateControl::Quality the bitrate is a ceiling rather than a target, so
// the estimate is a worst case there and the file usually turns out smaller.
// A lossless encoder has no bitrate, and goes by the picture's size and rate,
// at `pixelBytes` a pixel before the codec (Recorder::CardPixelBytes).
double EstimatedBytesPerSecond(const RecordSettings& settings, int audioTracks, int width,
                               int height, double fps, double pixelBytes = 3.0);

// Where the next recording in `folder` goes: named to the second, with a number
// added when that name is taken -- a file cut within the same second as the
// last one started would otherwise land on it while the one before may still
// be finishing it. Looks at the disk, so never on the thread that draws.
std::filesystem::path NextRecordingFile(const std::filesystem::path& folder,
                                        RecordContainer container);

class Recorder {
 public:
  Recorder();
  ~Recorder();

  Recorder(const Recorder&) = delete;
  Recorder& operator=(const Recorder&) = delete;

  // One audio source: the sample rate it delivers, and where to pull from. A
  // rate of 0 means the source is not in use.
  struct AudioSource {
    int sampleRate = 0;
    AudioPullFn pull;
    bool active() const { return sampleRate > 0 && pull != nullptr; }
  };
  // A sound that goes only into the recording (RecordSource), under `title`.
  struct ExtraAudio {
    AudioSource source;
    std::string title;
    TrackMode tracks = TrackMode::Both;
  };

  // Whether the picture keeps the full range on the PQ curve. Set before
  // Start, which works out the rest of the format from the encoder.
  void SetWide(bool wide) { wide_ = wide; }
  // What was asked, which hdr() need not be: lossless keeps the range only on
  // NVENC.
  bool wideAsked() const { return wide_; }
  // The renderer's adapter (VideoRenderer::sharedAdapter), 0 for none. A
  // hardware encoder then takes the picture there, on the GPU, where it can.
  // Set before Start.
  void SetGpuAdapter(uint64_t adapter) { gpuAdapter_ = adapter; }
  // What the writer takes, from Start on: the renderer delivers exactly this.
  const YuvFormat& format() const { return format_; }
  bool hdr() const { return unfiltered_ ? card_.transfer != 0 : format_.pq(); }

  // The card's frames as they arrive, for a lossless recording without
  // filters (RecordSettings::unfiltered): their format, and how the renderer
  // reads their colour, which is what the file is tagged with.
  struct CardFrames {
    VideoFormatInfo format;
    bool fullRange = false;
    bool bt709 = false;
    int transfer = 0;    // VideoRenderer::Transfer: 0 SDR, 1 PQ, 2 HLG
    int fieldOrder = 0;  // 0 not known, 1 top field first, 2 bottom field first
  };
  // Set before Start; null for the picture as it is shown.
  void SetCard(const CardFrames* card) {
    unfiltered_ = card != nullptr;
    card_ = card ? *card : CardFrames{};
  }
  bool unfiltered() const { return unfiltered_; }
  const VideoFormatInfo& card() const { return card_.format; }
  // What a pixel of `card` costs in the file before `encoder`; 0 when it
  // cannot take the card's frames as they are.
  static double CardPixelBytes(RecordEncoder encoder, const VideoFormatInfo& card);

  // Which family an encoder belongs to. The settings are one set of names; what
  // each vendor actually calls them is another, and this is the seam.
  enum class Family { Software, Nvenc, Amf, Qsv };
  static Family FamilyOf(const std::string& ffmpegName);

  // The bytes `encoder` takes without anything converting them: 4:4:4 when
  // asked for and the encoder passed it at that depth, 4:2:0 otherwise.
  static YuvFormat FormatFor(const EncoderInfo& encoder, bool wide, bool fullChroma);

  // `main` is the sound you hear, and its sample count is the master clock; a
  // rate of 0 there hands the clock to the wall instead. `extras` are brought
  // onto that clock and laid out by TrackPlan; the inactive ones are left out.
  // The encoder is opened by the writer thread: one that cannot open shows up
  // as failed() a moment later, not as false here. `outFile` is from
  // NextRecordingFile, in a folder that exists.
  bool Start(const RecordSettings& settings, const std::filesystem::path& outFile,
             const FfmpegInfo& ffmpeg, int width, int height, double sourceFps,
             const AudioSource& main, std::vector<ExtraAudio> extras, std::string* error);
  // Ends the recording and returns at once. The encoders still hold frames and
  // the container its index, which can take seconds with a slow preset, and
  // neither the picture nor the next file of a split waits for that: it
  // finishes on a thread of its own, and TakeFinished says when.
  void Stop();

  // A file that has been closed since the last call. `complete` is false when
  // writing it failed somewhere, and it may then not play.
  struct Finished {
    std::string file;
    double seconds = 0.0;
    bool complete = true;
  };
  bool TakeFinished(Finished* out);

  bool recording() const { return running_.load(std::memory_order_relaxed); }
  // Set when the encoder could not open or writing failed; the app should stop
  // and report. recording() stays true until it does.
  bool failed() const { return failed_.load(std::memory_order_relaxed); }
  // The encoder has not kept up for seconds on end, and the file repeats frames
  // to stay in step with the sound. Clears once it catches up again.
  bool encoderBehind() const { return encoderBehind_.load(std::memory_order_relaxed); }

  // Called from the render thread with a freshly read back frame. Its size and
  // format are passed along and checked: a frame of a different shape than the
  // one the encoder was opened for is dropped rather than written.
  void PushVideo(const uint8_t* pixels, int stride, int width, int height,
                 const YuvFormat& format);
  // Without filters instead: a frame as the card delivered it, `size` bytes in
  // the format of SetCard. One copy here; the rest happens on the writer.
  void PushCardFrame(const uint8_t* data, size_t size);

  // On the GPU instead (shared_frames.h): the frames to draw into while the
  // encoder takes them there, null while it does not. PushVideo still works
  // all along, for when the renderer cannot draw into them after all.
  const std::shared_ptr<SharedFrames>& sharedFrames() const;
  // The renderer could not use them; it reads back from now on.
  void RefuseShared() { sharedRefused_.store(true, std::memory_order_relaxed); }
  // A slot of sharedFrames() just drawn into, which the caller held as Writing.
  void PushGpuFrame(int slot);

  // What the encoder was opened for. It takes no other size, so if the source
  // moves away from this, the file has to be cut.
  int frameWidth() const { return width_; }
  int frameHeight() const { return height_; }

  RecordStats stats() const;
  // The two numbers asked for every frame, without copying the strings in
  // stats() under their lock.
  double seconds() const;
  uint64_t bytesWritten() const { return bytesWritten_.load(std::memory_order_relaxed); }
  // What is encoded and waits in memory for the disk, and that against what
  // may wait (RecordSettings::largeBuffer): at 1 the recording stops. From the
  // thread that starts and stops.
  uint64_t queuedBytes() const;
  double bufferFill() const;
  // Measures the large buffer against the memory free right now, besides what
  // it holds itself: what other programs took since the start is no longer
  // its to take. Never below the small buffer. From the same thread.
  void LimitBuffer(uint64_t freeBytes);

 private:
  // The encoders, the muxer and what feeds them, for one file. Handed to the
  // closing thread by Stop, so the next Start gets a fresh one while the old
  // file is still finishing.
  struct Session;

  // Opens the encoders and the file, then encodes the frames.
  void VideoThread(Session* session);
  // Every audio source, every track.
  void AudioThread(Session* session);
  bool Open(Session* session);
  void Fail(const Said& said);
  // Hands a filled buffer of the session's pool to the writer.
  void Offer(AVBufferRef* buffer);

  // Everything the settings say, in the words this particular encoder uses:
  // option name and value, by turns.
  std::vector<std::string> EncoderOptions(const RecordSettings& settings,
                                          const EncoderInfo& encoder) const;

  bool wide_ = false;
  uint64_t gpuAdapter_ = 0;
  YuvFormat format_;
  bool unfiltered_ = false;
  CardFrames card_;
  CardPlan plan_;      // from Start on, while unfiltered_
  size_t cardBytes_ = 0;
  // The session's, set by the writer before ready_ and let go of in Stop.
  std::shared_ptr<SharedFrames> shared_;
  std::atomic<bool> sharedRefused_{false};
  std::atomic<bool> running_{false};
  // The encoder is open and takes frames; until then PushVideo drops them.
  std::atomic<bool> ready_{false};
  std::atomic<bool> failed_{false};
  std::atomic<bool> encoderBehind_{false};

  std::unique_ptr<Session> session_;

  // The newest closing file. Each one joins the one before it, so this is the
  // only thread the destructor has to wait for.
  std::thread closer_;
  std::mutex finishedMutex_;
  std::vector<Finished> finished_;
  std::atomic<bool> anyFinished_{false};

  std::thread videoThread_;
  std::thread audioThread_;
  AudioPullFn pullAudio_;
  std::vector<ExtraAudio> extras_;  // the active ones

  int width_ = 0;
  int height_ = 0;
  double fps_ = 60.0;  // as a fraction in the session; this is that, divided out
  int audioRate_ = 0;
  // One frame: the luma rows and the chroma rows after them, without padding;
  // without filters, what the encoder takes (CardOutBytes).
  size_t frameBytes_ = 0;

  // The newest frame, waiting for the writer: one buffer of the session's pool.
  // The writer takes it and keeps it as long as it has nothing newer, which is
  // what a repeated frame repeats.
  mutable std::mutex frameMutex_;
  // Wakes the video writer: a new frame, audio that moved the timeline on, or
  // Stop.
  std::condition_variable frameReady_;
  AVBufferRef* readyBuffer_ = nullptr;
  int readySlot_ = -1;  // the same on the GPU: a Ready slot of shared_

  std::atomic<uint64_t> audioFramesWritten_{0};
  std::atomic<uint64_t> videoFramesWritten_{0};
  std::atomic<uint64_t> duplicated_{0};
  std::atomic<uint64_t> dropped_{0};
  std::atomic<uint64_t> bytesWritten_{0};
  int64_t startTicks_ = 0;

  // Video thread only. When the encoder had opened: the wall clock timeline
  // starts there, not at Start, or the first frames would all be late.
  int64_t originTicks_ = 0;
  // Notices when the audio stops advancing so the frame timeline can fall back
  // to the system clock instead of freezing.
  uint64_t lastAudioSeen_ = 0;
  int64_t lastAudioProgressTicks_ = 0;
  bool audioStallLogged_ = false;

  mutable std::mutex infoMutex_;
  std::string file_;
  std::string encoderLabel_;
  std::string error_;
};

}  // namespace cap
