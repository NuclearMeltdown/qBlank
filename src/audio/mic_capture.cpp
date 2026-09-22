#include "audio/mic_capture.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

#include "common.h"
#include "i18n.h"

namespace cap {
namespace {

// How fast the meter falls. One capture block is around ten milliseconds, so
// this reaches the floor in roughly a third of a second -- fast enough to read
// as a level, slow enough that a peak is visible at all.
const float kPeakDecay = 0.80f;

}  // namespace

MicCapture::~MicCapture() {
  Stop();
}

void MicCapture::Fail(const Said& said) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (lastError_.empty()) lastError_ = said.shown;
  }
  CAP_ERR("Microphone: %s", said.logged.c_str());
}

void MicCapture::SignalReady() {
  {
    std::lock_guard<std::mutex> lock(readyMutex_);
    ready_ = true;
  }
  readyChanged_.notify_all();
}

std::string MicCapture::lastError() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return lastError_;
}

std::string MicCapture::deviceName() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return deviceName_;
}

bool MicCapture::TakeClipped() {
  return clipped_.exchange(false, std::memory_order_relaxed);
}

void MicCapture::ResetBuffer() {
  ring_.Reset(ring_.Capacity());
}

size_t MicCapture::Read(float* out, size_t frames) {
  return ring_.Read(out, frames);
}

bool MicCapture::Start(const DeviceRef& device, std::string* error) {
  Stop();

  AudioDeviceInfo info;
  if (device.empty()) {
    // Nothing chosen means the system default, which the input opens when the
    // id is empty. Resolving would fail here, since there is nothing to
    // resolve.
    info.name = T("Systemstandard", "System default");
  } else if (!ResolveAudioDevice(device, true, &info)) {
    const Said said = CAP_SAID(T("Mikrofon nicht gefunden.", "Microphone not found."));
    if (error) *error = said.shown;
    Fail(said);
    return false;
  }
  // A capture card's embedded audio is not a microphone; it belongs on the
  // main path, not here.
  if (info.cardAudio) {
    const Said said = CAP_SAID(T("Dieses Gerät ist ein ", "That device is a ") + info.via +
                               T("-Eingang und kommt hier nicht in Frage.",
                                 " input and does not belong here."));
    if (error) *error = said.shown;
    Fail(said);
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    deviceName_ = info.name;
    lastError_.clear();
  }

  // Four seconds. The recorder drains this continuously; the headroom is for
  // the moment ffmpeg takes to open its end of the pipe.
  ring_.Reset(4 * 48000);
  startFailed_.store(false, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(readyMutex_);
    ready_ = false;
  }
  running_.store(true, std::memory_order_relaxed);

  AudioInputEvents events;
  events.onFormat = [this](int sampleRate, int /*channels*/) {
    ring_.Reset((size_t)sampleRate * 4);
    sampleRate_.store(sampleRate, std::memory_order_relaxed);
    SignalReady();  // Start() may return now
  };
  events.onAudio = [this](const float* data, size_t frames) { OnAudio(data, frames); };
  events.onQuiet = [this]() {
    // Silence still has to decay, or the bar would hang at the last peak.
    peak_.store(peak_.load(std::memory_order_relaxed) * kPeakDecay, std::memory_order_relaxed);
  };
  // Whatever happens, Start() must stop waiting. Failing to signal here would
  // cost three seconds of frozen interface on every attempt.
  events.onFailure = [this](const Said& said) {
    Fail(said);
    running_.store(false, std::memory_order_relaxed);
    startFailed_.store(true, std::memory_order_relaxed);
    SignalReady();
  };

  std::string err;
  input_ = CreateAudioInput(info);
  if (!input_->Start(info, AudioRole::Microphone, std::move(events), &err)) {
    if (error) *error = err;
    Stop();
    return false;
  }

  // Opening the device takes a few milliseconds, and until it is done there is
  // no sample rate to report. Returning early would let a recording start
  // without the microphone track and never notice.
  bool ready = false;
  {
    std::unique_lock<std::mutex> lock(readyMutex_);
    ready = readyChanged_.wait_for(lock, std::chrono::milliseconds(3000), [this] { return ready_; });
  }
  if (!ready || startFailed_.load(std::memory_order_relaxed)) {
    // What the input reported is in the log already; only silence is new.
    const std::string reported = lastError();
    if (reported.empty()) {
      ReportError(error, CAP_SAID(T("Mikrofon antwortet nicht.", "The microphone is not responding.")));
    } else if (error) {
      *error = reported;
    }
    Stop();
    return false;
  }
  return true;
}

void MicCapture::Stop() {
  running_.store(false, std::memory_order_relaxed);
  if (input_) {
    input_->Stop();
    input_.reset();
  }
  sampleRate_.store(0, std::memory_order_relaxed);
  peak_.store(0.0f, std::memory_order_relaxed);
}

void MicCapture::OnAudio(const float* interleaved, size_t frames) {
  scratch_.assign(interleaved, interleaved + frames * 2);

  // Gain first, then measure: the meter should show what goes into the file,
  // not what the device handed over.
  const float gain = gain_.load(std::memory_order_relaxed);
  float blockPeak = 0.0f;
  for (float& sample : scratch_) {
    sample *= gain;
    const float magnitude = std::abs(sample);
    if (magnitude > blockPeak) blockPeak = magnitude;
  }
  if (blockPeak >= 1.0f) clipped_.store(true, std::memory_order_relaxed);

  const float decayed = peak_.load(std::memory_order_relaxed) * kPeakDecay;
  peak_.store(std::max(blockPeak, decayed), std::memory_order_relaxed);

  ring_.Write(scratch_.data(), frames);
}

}  // namespace cap
