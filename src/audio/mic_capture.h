#pragma once

// A second capture device -- normally a microphone -- that goes into the
// recording and nowhere else.
//
// Deliberately separate from AudioEngine, which exists to get the card's sound
// to your speakers with as little delay as possible. A microphone has none of
// those concerns: it is never played back (that would be an echo), it does not
// take part in drift correction, and it lands in the file as its own track
// rather than mixed in. Keeping the two apart means neither grows a flag for
// the other's special case.
//
// The stream stays at the device's own sample rate. ffmpeg is told what that
// rate is and resamples on its way into AAC, so there is no resampler here to
// get wrong.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "audio/audio_devices.h"
#include "audio/audio_ring.h"
#include "audio/audio_stream.h"
#include "config.h"
#include "job_thread.h"

namespace cap {

class MicCapture {
 public:
  MicCapture() = default;
  ~MicCapture();

  MicCapture(const MicCapture&) = delete;
  MicCapture& operator=(const MicCapture&) = delete;

  // Both return at once. Finding the device, opening it and letting it go again
  // happen on the microphone's own thread: the device list alone is a round
  // of system calls, a DirectShow device builds a whole graph, and stopping
  // waits for the capture thread. Until the device is open sampleRate() is 0,
  // and one that cannot be found or opened is reported by Opened().
  void Start(const DeviceRef& device);
  void Stop();

  // Whether what Start began is still undecided.
  bool opening() const { return opening_; }
  enum class Open { Pending, Ready, Failed };
  // Waits up to `waitMs` for the device Start is opening. A device that has not
  // answered three seconds after Start counts as failed; a failed one is
  // stopped, with the reason in `error`.
  Open Opened(uint32_t waitMs, std::string* error);

  bool running() const { return running_.load(std::memory_order_relaxed); }
  int sampleRate() const { return sampleRate_.load(std::memory_order_relaxed); }
  std::string deviceName() const;
  std::string lastError() const;

  // Linear, applied before the level meter and before the ring -- so the meter
  // shows what actually lands in the file.
  void SetGain(float gain) { gain_.store(gain, std::memory_order_relaxed); }

  // Loudest sample seen recently, 0..1, with a decay so the bar falls back
  // instead of sticking at the last peak.
  float peak() const { return peak_.load(std::memory_order_relaxed); }
  // True when a sample hit full scale since the last read; the meter shows this
  // as a clip marker.
  bool TakeClipped();

  // Throws away what has queued up. Called when a recording starts so it does
  // not open with several seconds of stale audio.
  void ResetBuffer();

  // Interleaved stereo float at sampleRate(). Returns frames actually read.
  size_t Read(float* out, size_t frames);

 private:
  // Runs on worker_. `generation` is the Start it belongs to; a Stop since then
  // makes it a no-op.
  void OpenOnWorker(const DeviceRef& device, uint32_t generation);
  void OnAudio(const float* interleaved, size_t frames);
  void Fail(const Said& said);
  // The device came up or gave up: either way Opened() may stop waiting.
  void SignalReady();
  void GiveUp(uint32_t generation);

  AudioRing ring_;
  std::unique_ptr<AudioInput> input_;  // worker_ only
  std::vector<float> scratch_;  // the input's thread only

  // A Start not yet followed by a Stop; the caller's thread only. The settings
  // ask for a stop every frame the microphone is not wanted, and only the first
  // has anything to hand to the worker.
  bool started_ = false;
  // Counted up by every Stop. A device being let go of on the worker can still
  // deliver for a moment, and none of it may reach the next session.
  std::atomic<uint32_t> generation_{0};

  // Set once the device is open and the sample rate is published, or once it
  // has given up. Opened() waits for it, because a recording needs the rate
  // before it can ask ffmpeg for a second input.
  std::mutex readyMutex_;
  std::condition_variable readyChanged_;
  bool ready_ = false;
  std::atomic<bool> startFailed_{false};
  bool opening_ = false;  // the caller's thread only
  std::chrono::steady_clock::time_point openStarted_;

  std::atomic<bool> running_{false};
  std::atomic<int> sampleRate_{0};
  std::atomic<float> gain_{1.0f};
  std::atomic<float> peak_{0.0f};
  std::atomic<bool> clipped_{false};

  mutable std::mutex mutex_;
  std::string deviceName_;
  std::string lastError_;

  // One thread, so an open and the stop after it happen in the order asked for.
  // Last, because its jobs use everything above.
  JobThread worker_;
};

}  // namespace cap
