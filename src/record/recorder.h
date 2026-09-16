#pragma once

// Writes what the viewer shows to a file, by feeding raw frames and raw audio
// to an ffmpeg child process.
//
// Two rules shape the whole design:
//
//   1. The display path is never blocked. PushVideo copies into a triple buffer
//      and returns; a writer thread owns the pipes. If the encoder cannot keep
//      up, frames are dropped from the *recording*, never from the screen.
//
//   2. Audio is the master clock. The card's clock and the PC's clock drift
//      apart over an hour, so the video timeline is derived from the number of
//      audio samples written, duplicating or dropping frames to match. That
//      makes sync arithmetic instead of hope, and produces constant frame rate
//      output, which both MKV and MP4 prefer.

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common.h"
#include "config.h"
#include "record/ffmpeg_locator.h"

namespace cap {

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
// track is the 192 kbit/s the command line fixes. A running recording has a
// measured rate and does not need this -- this is for the number shown before
// anything is running, where there is nothing to measure.
//
// Under RateControl::Quality the bitrate is a ceiling rather than a target, so
// the estimate is a worst case there and the file usually turns out smaller.
double EstimatedBytesPerSecond(const RecordSettings& settings, int audioTracks);

class Recorder {
 public:
  Recorder() = default;
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

  // Which tracks end up in the file when both sources are in use.
  MicTrackMode micTrackMode_ = MicTrackMode::Both;

  // `main` is the sound you hear, and its sample count is the master clock; a
  // rate of 0 there hands the clock to the wall instead. `mic` is optional and
  // becomes a second track -- never mixed in, so it stays separable later.
  // The picture the writer will be handed: which ffmpeg pixel format it is in,
  // and whether it carries the full range on the PQ curve. Set before Start.
  void SetPixelFormat(const char* ffmpegName, bool hdr) {
    pixelFormat_ = ffmpegName;
    hdr_ = hdr;
  }


 private:
  // Everything the settings say, in the words this particular encoder uses.
  std::wstring EncoderOptions(const RecordSettings& settings, const EncoderInfo& encoder) const;

 public:

  // Which family an encoder belongs to. The settings are one set of names; what
  // each vendor actually calls them is another, and this is the seam.
  enum class Family { Software, Nvenc, Amf, Qsv };
  static Family FamilyOf(const std::string& ffmpegName);

  bool Start(const RecordSettings& settings, const FfmpegInfo& ffmpeg, int width, int height,
             double sourceFps, const AudioSource& main, const AudioSource& mic,
             MicTrackMode micTrackMode, std::string* error);
  void Stop();

  bool recording() const { return running_.load(std::memory_order_relaxed); }
  // Set when ffmpeg died on its own; the app should stop and report.
  bool failed() const { return failed_.load(std::memory_order_relaxed); }

  // Called from the render thread with a freshly read back frame. Its size is
  // passed along and checked: a frame of a different shape than the one the
  // running ffmpeg was told about is dropped rather than written.
  void PushVideo(const uint8_t* pixels, int stride, int width, int height);

  // What was handed to ffmpeg at the start. `-s` and `-r` stand fixed in its
  // command line -- rawvideo carries no header that could say anything else --
  // so if the source moves away from these, the file has to be cut.
  int frameWidth() const { return width_; }
  int frameHeight() const { return height_; }

  RecordStats stats() const;

  // Size of the file being written, for the optional size based split.
  uint64_t outputFileSize() const;

 private:
  void VideoThread();
  // `countsAsClock` is true only for the main track, whose written frame count
  // drives the video timeline.
  void AudioThread(HANDLE pipe, AudioPullFn pull, bool countsAsClock);
  void StderrThread();
  void Fail(const Said& said);
  bool WriteAll(HANDLE pipe, const uint8_t* data, size_t size);
  int PickWriteSlotLocked() const;

  std::wstring BuildCommandLine(const RecordSettings& settings, const EncoderInfo& encoder,
                                const std::wstring& audioPipe, const std::wstring& micPipe,
                                const std::wstring& outFile, int audioRate, int micRate) const;

  // Overwritten by SetPixelFormat before every start; the literal is only what
  // an unconfigured recorder would fall back to.
  std::string pixelFormat_ = "rgba";
  bool hdr_ = false;
  std::atomic<bool> running_{false};
  std::atomic<bool> failed_{false};

  HANDLE process_ = nullptr;
  HANDLE videoPipe_ = nullptr;  // our write end of the child's stdin
  HANDLE audioPipe_ = nullptr;  // named pipe, our end
  std::wstring audioPipeName_;
  HANDLE micPipe_ = nullptr;
  std::wstring micPipeName_;

  std::thread videoThread_;
  std::thread audioThread_;
  std::thread micThread_;
  std::thread stderrThread_;
  HANDLE stderrPipe_ = nullptr;  // our read end of the child's stderr
  AudioPullFn pullAudio_;
  AudioPullFn pullMic_;
  int micRate_ = 0;

  int width_ = 0;
  int height_ = 0;
  double fps_ = 60.0;
  int audioRate_ = 0;
  size_t frameBytes_ = 0;

  // Triple buffer, same shape as the capture sink: the writer holds one slot
  // while it works, so the render thread always has somewhere to write.
  mutable std::mutex frameMutex_;
  std::vector<uint8_t> slots_[3];
  int readyIdx_ = -1;
  int heldIdx_ = -1;

  std::atomic<uint64_t> audioFramesWritten_{0};
  std::atomic<uint64_t> videoFramesWritten_{0};
  std::atomic<uint64_t> duplicated_{0};
  std::atomic<uint64_t> dropped_{0};
  std::atomic<uint64_t> bytesWritten_{0};
  int64_t startQpc_ = 0;

  // Video thread only: notices when the audio stops advancing so the frame
  // timeline can fall back to the system clock instead of freezing.
  uint64_t lastAudioSeen_ = 0;
  int64_t lastAudioProgressQpc_ = 0;
  bool audioStallLogged_ = false;

  mutable std::mutex infoMutex_;
  std::string file_;
  std::string encoderLabel_;
  std::string error_;
};

}  // namespace cap
