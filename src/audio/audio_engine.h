#pragma once

// Audio passthrough: capture device -> ring buffer -> playback device.
//
// Routing the card's audio through our own ring is what makes the delay
// adjustable at all. A DirectShow audio renderer would hand us whatever
// buffering it feels like, typically over a hundred milliseconds; here the
// target fill of the ring is the latency, and the playback rate is nudged by a
// fraction of a percent to hold it there without ever cutting the stream.
//
// The two devices sit behind AudioInput and AudioOutput. Everything between
// them -- the ring, the target fill, the drift correction -- is plain
// arithmetic and lives here.

#include <atomic>
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

struct AudioStats {
  bool running = false;
  bool exclusive = false;
  int captureRate = 0;
  int captureChannels = 0;
  int renderRate = 0;
  int renderChannels = 0;
  double bufferMs = 0.0;  // what is currently queued
  double targetMs = 0.0;  // what we aim for
  uint64_t underruns = 0;
  uint64_t overruns = 0;
  std::string inputName;
  std::string inputVia;  // how the input is read, for the overlay
  std::string outputName;
};

class AudioEngine {
 public:
  AudioEngine() = default;
  ~AudioEngine();

  AudioEngine(const AudioEngine&) = delete;
  AudioEngine& operator=(const AudioEngine&) = delete;

  // `input` is the recording device (the card's audio), `settings.output` the
  // playback endpoint (empty means system default).
  bool Start(const DeviceRef& input, const AudioSettings& settings, std::string* error);
  void Stop();

  // Volume, mute and the A/V offset can change while running.
  void ApplySettings(const AudioSettings& settings);

  // ---- recording tap ----
  //
  // A second ring that receives the captured audio exactly as it arrived: no
  // resampling, no drift correction, no volume. The playback ring is
  // deliberately not usable for this -- it nudges the playback rate by a
  // fraction of a percent to hold its target fill, which is right for listening
  // and wrong for a file that has to stay in sync over an hour.
  void SetTapEnabled(bool enabled);
  bool tapEnabled() const { return tapEnabled_.load(std::memory_order_relaxed); }

  // Interleaved stereo float at tapSampleRate(). Returns frames actually read.
  size_t ReadTap(float* out, size_t frames);
  size_t TapAvailable() const { return tapRing_.Available(); }
  int tapSampleRate() const { return captureRate_.load(std::memory_order_relaxed); }
  // Counts how often the tap ring overflowed, i.e. the recorder fell behind.
  uint64_t tapOverruns() const { return tapRing_.overruns(); }

  // Loudest sample seen recently on the input, 0..1, with a decay. Measured
  // before volume and mute, so it shows what the card is delivering rather than
  // how loud you have it.
  float inputPeak() const { return inputPeak_.load(std::memory_order_relaxed); }

  bool running() const { return running_.load(std::memory_order_relaxed); }
  // Set when a device disappeared; the app can then offer a restart.
  bool failed() const { return failed_.load(std::memory_order_relaxed); }
  std::string lastError() const;

  AudioStats stats() const;

 private:
  void Fail(const Said& said);

  // Single entry point for captured audio, whichever backend produced it.
  // Feeds the playback ring and, when recording, the tap.
  void OnCapturedAudio(const float* interleaved, size_t frames);

  // The output's side, on its thread: a fresh stream, then one period at a time.
  void OnOutputStart(const AudioOutputFormat& format);
  AudioFill FillOutput(float* out, size_t frames);

  AudioRing ring_;
  AudioRing tapRing_;
  std::atomic<bool> tapEnabled_{false};

  std::unique_ptr<AudioInput> input_;
  std::unique_ptr<AudioOutput> output_;

  std::atomic<bool> running_{false};
  std::atomic<bool> failed_{false};
  mutable std::mutex statsMutex_;
  std::string lastError_;

  std::atomic<float> inputPeak_{0.0f};
  std::atomic<float> volume_{1.0f};
  std::atomic<bool> mute_{false};
  std::atomic<int> targetMs_{30};
  // What the output actually aims for: the configured value raised to clear the
  // playback device's own buffer.
  std::atomic<double> effectiveTargetMs_{0.0};

  std::atomic<int> captureRate_{0};
  std::atomic<int> captureChannels_{0};
  std::atomic<int> renderRate_{0};
  std::atomic<int> renderChannels_{0};
  std::atomic<uint64_t> underruns_{0};
  std::atomic<bool> exclusiveActive_{false};

  std::string inputName_;
  std::string inputVia_;
  std::string outputName_;

  // Playback state, touched only from the output's thread.
  int outputRate_ = 0;
  double minTargetMs_ = 0.0;
  // Resampler state: srcFrac is where we are inside the current source frame,
  // prev holds the frame before it so interpolation always has a left sample.
  // `pending` keeps frames that were read from the ring but not consumed yet --
  // without it every period would quietly throw one frame away.
  double srcFrac_ = 0.0;
  float prev_[2] = {0.0f, 0.0f};
  bool primed_ = false;
  std::vector<float> pending_;
};

}  // namespace cap
