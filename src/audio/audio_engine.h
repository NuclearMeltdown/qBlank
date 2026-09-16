#pragma once

// Audio passthrough: capture device -> ring buffer -> WASAPI render endpoint.
//
// Routing the card's audio through our own ring is what makes the delay
// adjustable at all. A DirectShow audio renderer would hand us whatever
// buffering it feels like, typically over a hundred milliseconds; here the
// target fill of the ring is the latency, and the playback rate is nudged by a
// fraction of a percent to hold it there without ever cutting the stream.
//
// The capture side is either WASAPI or, for cards whose embedded audio Windows
// does not expose as a sound device, a DirectShow graph. Playback is always
// WASAPI so the output device and the exclusive mode option work either way.

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/audio_devices.h"
#include "audio/audio_ring.h"
#include "audio/dshow_audio_capture.h"
#include "audio/pcm.h"
#include "common.h"
#include "config.h"

namespace cap {

struct AudioStats {
  bool running = false;
  bool exclusive = false;
  bool directShowInput = false;
  int captureRate = 0;
  int captureChannels = 0;
  int renderRate = 0;
  int renderChannels = 0;
  double bufferMs = 0.0;  // what is currently queued
  double targetMs = 0.0;  // what we aim for
  uint64_t underruns = 0;
  uint64_t overruns = 0;
  std::string inputName;
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
  void CaptureThread(AudioDeviceInfo device);
  void RenderThread(AudioDeviceInfo device, bool exclusive);
  void Fail(const Said& said);

  // Single entry point for captured audio, whichever backend produced it.
  // Feeds the playback ring and, when recording, the tap.
  void OnCapturedAudio(const float* interleaved, size_t frames);

  AudioRing ring_;
  AudioRing tapRing_;
  std::atomic<bool> tapEnabled_{false};
  DShowAudioCapture dshowCapture_;

  std::thread captureThread_;
  std::thread renderThread_;
  HANDLE stopEvent_ = nullptr;

  std::atomic<bool> running_{false};
  std::atomic<bool> failed_{false};
  mutable std::mutex statsMutex_;
  std::string lastError_;

  std::atomic<float> inputPeak_{0.0f};
  std::atomic<float> volume_{1.0f};
  std::atomic<bool> mute_{false};
  std::atomic<int> targetMs_{30};
  // What the render thread actually aims for: the configured value raised to
  // clear the playback device's own buffer.
  std::atomic<double> effectiveTargetMs_{0.0};

  std::atomic<int> captureRate_{0};
  std::atomic<int> captureChannels_{0};
  std::atomic<int> renderRate_{0};
  std::atomic<int> renderChannels_{0};
  std::atomic<uint64_t> underruns_{0};
  std::atomic<bool> exclusiveActive_{false};
  std::atomic<bool> directShowInput_{false};

  std::string inputName_;
  std::string outputName_;
};

}  // namespace cap
