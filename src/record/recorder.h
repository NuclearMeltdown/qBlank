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
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "child_process.h"
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

// Where the next recording in `folder` goes: named to the second, with a number
// added when that name is taken -- a file cut within the same second as the
// last one started would otherwise land on it, with "-y", while the ffmpeg
// before may still be writing it. Looks at the disk, so never on the thread
// that draws.
std::filesystem::path NextRecordingFile(const std::filesystem::path& folder,
                                        RecordContainer container);

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

  // The picture the writer will be handed: which ffmpeg pixel format it is in,
  // and whether it carries the full range on the PQ curve. Set before Start.
  void SetPixelFormat(const char* ffmpegName, bool hdr) {
    pixelFormat_ = ffmpegName;
    hdr_ = hdr;
  }

  // Which family an encoder belongs to. The settings are one set of names; what
  // each vendor actually calls them is another, and this is the seam.
  enum class Family { Software, Nvenc, Amf, Qsv };
  static Family FamilyOf(const std::string& ffmpegName);

  // `main` is the sound you hear, and its sample count is the master clock; a
  // rate of 0 there hands the clock to the wall instead. `mic` is optional;
  // `micTrackMode` says whether it is mixed in, kept as its own track, or both.
  // ffmpeg itself is started by the writer thread: an ffmpeg that cannot start
  // shows up as failed() a moment later, not as false here. `outFile` is from
  // NextRecordingFile, in a folder that exists.
  bool Start(const RecordSettings& settings, const std::filesystem::path& outFile,
             const FfmpegInfo& ffmpeg, int width, int height, double sourceFps,
             const AudioSource& main, const AudioSource& mic, MicTrackMode micTrackMode,
             std::string* error);
  // Ends the recording and returns at once. ffmpeg still has to write the
  // container's index, which can take seconds with a slow preset, and neither
  // the picture nor the next file of a split waits for that: it finishes on a
  // thread of its own, and TakeFinished says when.
  void Stop();

  // A file whose ffmpeg has exited since the last call. `complete` is false
  // when it had to be killed, and the file may then not play.
  struct Finished {
    std::string file;
    double seconds = 0.0;
    bool complete = true;
  };
  bool TakeFinished(Finished* out);

  bool recording() const { return running_.load(std::memory_order_relaxed); }
  // Set when ffmpeg could not start or died on its own; the app should stop and
  // report. recording() stays true until it does.
  bool failed() const { return failed_.load(std::memory_order_relaxed); }
  // The encoder has not kept up for seconds on end, and the file repeats frames
  // to stay in step with the sound. Clears once it catches up again.
  bool encoderBehind() const { return encoderBehind_.load(std::memory_order_relaxed); }

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
  // The two numbers asked for every frame, without copying the strings in
  // stats() under their lock.
  double seconds() const;
  uint64_t bytesWritten() const { return bytesWritten_.load(std::memory_order_relaxed); }

 private:
  // Starts ffmpeg, then writes the frames to it.
  void VideoThread(ProcessSpec spec, uint32_t session);
  // `countsAsClock` is true only for the main track, whose written frame count
  // drives the video timeline.
  void AudioThread(ChildStream* stream, AudioPullFn pull, bool countsAsClock);
  // One line of whatever ffmpeg wrote to its error stream, from the reader
  // thread inside ChildProcess. `session` is the Start it belongs to: an ffmpeg
  // still closing the previous file must not put its words on the next one.
  void OnFfmpegSaid(const std::string& line, uint32_t session);
  void Fail(const Said& said);
  bool WriteAll(ChildStream* stream, const uint8_t* data, size_t size);
  int PickWriteSlotLocked() const;

  // Everything the settings say, in the words this particular encoder uses.
  std::vector<std::string> EncoderOptions(const RecordSettings& settings,
                                          const EncoderInfo& encoder) const;

  // The whole encode, one argument per entry. Nothing here is quoted: the
  // quoting belongs to whoever starts the process, and a file name with a space
  // in it is one argument either way.
  std::vector<std::string> BuildArguments(const RecordSettings& settings,
                                          const EncoderInfo& encoder,
                                          const std::string& audioPipe,
                                          const std::string& micPipe,
                                          const std::filesystem::path& outFile, int audioRate,
                                          int micRate) const;

  // Overwritten by SetPixelFormat before every start; the literal is only what
  // an unconfigured recorder would fall back to.
  std::string pixelFormat_ = "rgba";
  bool hdr_ = false;
  std::atomic<bool> running_{false};
  std::atomic<bool> failed_{false};
  std::atomic<bool> encoderBehind_{false};

  // Handed to closer_ by Stop together with the streams, so the next Start
  // gets a fresh one while the old ffmpeg is still finishing.
  std::unique_ptr<ChildProcess> process_;
  std::unique_ptr<ChildStream> videoStream_;  // ffmpeg's standard input
  std::unique_ptr<ChildStream> audioStream_;  // opened by name, one per track
  std::unique_ptr<ChildStream> micStream_;
  std::atomic<uint32_t> session_{0};

  // The newest closing ffmpeg. Each one joins the one before it, so this is
  // the only thread the destructor has to wait for.
  std::thread closer_;
  std::mutex finishedMutex_;
  std::vector<Finished> finished_;
  std::atomic<bool> anyFinished_{false};

  std::thread videoThread_;
  std::thread audioThread_;
  std::thread micThread_;
  AudioPullFn pullAudio_;
  AudioPullFn pullMic_;
  int micRate_ = 0;
  // Which tracks end up in the file when both sources are in use.
  MicTrackMode micTrackMode_ = MicTrackMode::Both;

  int width_ = 0;
  int height_ = 0;
  double fps_ = 60.0;
  int audioRate_ = 0;
  size_t frameBytes_ = 0;

  // Triple buffer, same shape as the capture sink: the writer holds one slot
  // while it works, so the render thread always has somewhere to write.
  mutable std::mutex frameMutex_;
  // Wakes the video writer: a new frame, audio that moved the timeline on, or
  // Stop.
  std::condition_variable frameReady_;
  std::vector<uint8_t> slots_[3];
  int readyIdx_ = -1;
  int heldIdx_ = -1;

  std::atomic<uint64_t> audioFramesWritten_{0};
  std::atomic<uint64_t> videoFramesWritten_{0};
  std::atomic<uint64_t> duplicated_{0};
  std::atomic<uint64_t> dropped_{0};
  std::atomic<uint64_t> bytesWritten_{0};
  int64_t startTicks_ = 0;

  // Video thread only: notices when the audio stops advancing so the frame
  // timeline can fall back to the system clock instead of freezing.
  uint64_t lastAudioSeen_ = 0;
  int64_t lastAudioProgressTicks_ = 0;
  bool audioStallLogged_ = false;

  mutable std::mutex infoMutex_;
  std::string file_;
  std::string encoderLabel_;
  std::string error_;
};

}  // namespace cap
