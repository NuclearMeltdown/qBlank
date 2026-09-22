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

namespace cap {

class MicCapture {
 public:
  MicCapture() = default;
  ~MicCapture();

  MicCapture(const MicCapture&) = delete;
  MicCapture& operator=(const MicCapture&) = delete;

  bool Start(const DeviceRef& device, std::string* error);
  void Stop();

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
  void OnAudio(const float* interleaved, size_t frames);
  void Fail(const Said& said);
  // Lets Start() stop waiting, whether the device came up or gave up.
  void SignalReady();

  AudioRing ring_;
  std::unique_ptr<AudioInput> input_;
  std::vector<float> scratch_;  // the input's thread only

  // Set once the device is open and the sample rate is published, or once it
  // has given up. Start() waits for it, because the caller needs the rate
  // before it can ask ffmpeg for a second input.
  std::mutex readyMutex_;
  std::condition_variable readyChanged_;
  bool ready_ = false;
  std::atomic<bool> startFailed_{false};

  std::atomic<bool> running_{false};
  std::atomic<int> sampleRate_{0};
  std::atomic<float> gain_{1.0f};
  std::atomic<float> peak_{0.0f};
  std::atomic<bool> clipped_{false};

  mutable std::mutex mutex_;
  std::string deviceName_;
  std::string lastError_;
};

}  // namespace cap
