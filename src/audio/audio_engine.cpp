#include "audio/audio_engine.h"

#include <algorithm>
#include <cmath>

#include "common.h"
#include "i18n.h"

namespace cap {
namespace {

// One second at 96 kHz, so the ring never has to be resized once the rate is
// known and the output can read it without synchronising on that.
const size_t kRingCapacityFrames = 96000;

}  // namespace

// ---------------------------------------------------------------- AudioEngine

AudioEngine::~AudioEngine() {
  Stop();
}

void AudioEngine::Fail(const Said& said) {
  {
    std::lock_guard<std::mutex> lock(statsMutex_);
    lastError_ = said.shown;
  }
  failed_.store(true, std::memory_order_relaxed);
  CAP_ERR("Audio: %s", said.logged.c_str());
}

std::string AudioEngine::lastError() const {
  std::lock_guard<std::mutex> lock(statsMutex_);
  return lastError_;
}

bool AudioEngine::Start(const DeviceRef& input, const AudioSettings& settings, std::string* error) {
  Stop();

  AudioDeviceInfo inputInfo;
  if (!ResolveAudioDevice(input, true, &inputInfo)) {
    const std::string name = input.name.empty() ? input.id : input.name;
    ReportError(error, CAP_SAID(input.empty()
                                    ? std::string(T("Kein Audiogerät ausgewählt", "No audio device selected"))
                                    : (T("Audioeingang '", "Audio input '") + name +
                                       T("' wurde nicht gefunden", "' was not found"))));
    return false;
  }

  AudioDeviceInfo outputInfo;
  if (!ResolveAudioDevice(settings.output, false, &outputInfo)) {
    // Empty or stale reference: fall back to the system default output.
    std::vector<AudioDeviceInfo> outputs = EnumerateAudioDevices(false);
    if (outputs.empty()) {
      ReportError(error, CAP_SAID(T("Kein Wiedergabegerät verfügbar", "No playback device available")));
      return false;
    }
    outputInfo = outputs.front();
    if (!settings.output.empty()) {
      CAP_WARN("Playback device '%s' not found, using '%s'", settings.output.name.c_str(),
               outputInfo.name.c_str());
    }
  }

  volume_.store(Clamp(settings.volume, 0.0f, 1.0f), std::memory_order_relaxed);
  mute_.store(settings.mute, std::memory_order_relaxed);
  // Only a positive offset is handled here (delay the audio). A negative offset
  // means the video is held back instead, which the app does.
  targetMs_.store(Clamp(settings.bufferMs + std::max(0, settings.avOffsetMs), 2, 1000),
                  std::memory_order_relaxed);
  underruns_.store(0, std::memory_order_relaxed);
  failed_.store(false, std::memory_order_relaxed);
  captureRate_.store(0, std::memory_order_relaxed);

  {
    std::lock_guard<std::mutex> lock(statsMutex_);
    inputName_ = inputInfo.name;
    inputVia_ = inputInfo.via;
    outputName_ = outputInfo.name;
    lastError_.clear();
  }

  ring_.Reset(kRingCapacityFrames);
  running_.store(true, std::memory_order_relaxed);

  AudioInputEvents in;
  in.onFormat = [this](int sampleRate, int channels) {
    captureChannels_.store(channels, std::memory_order_relaxed);
    captureRate_.store(sampleRate, std::memory_order_relaxed);
  };
  in.onAudio = [this](const float* data, size_t frames) { OnCapturedAudio(data, frames); };
  in.onFailure = [this](const Said& said) { Fail(said); };

  std::string err;
  input_ = CreateAudioInput(inputInfo);
  if (!input_->Start(inputInfo, AudioRole::Passthrough, std::move(in), &err)) {
    input_.reset();
    running_.store(false, std::memory_order_relaxed);
    if (error) *error = err;
    return false;
  }

  AudioOutputEvents out;
  out.onStart = [this](const AudioOutputFormat& format) { OnOutputStart(format); };
  out.fill = [this](float* samples, size_t frames) { return FillOutput(samples, frames); };
  out.onFailure = [this](const Said& said) { Fail(said); };

  output_ = CreateAudioOutput(outputInfo);
  if (!output_->Start(outputInfo, settings.exclusive, std::move(out), &err)) {
    output_.reset();
    input_->Stop();
    input_.reset();
    running_.store(false, std::memory_order_relaxed);
    if (error) *error = err;
    return false;
  }

  CAP_LOG("Audio started: '%s' (%s) -> '%s', target %d ms%s", inputInfo.name.c_str(),
          inputInfo.via.c_str(), outputInfo.name.c_str(), targetMs_.load(),
          settings.exclusive ? ", Exclusive" : "");
  return true;
}

void AudioEngine::Stop() {
  const bool wasRunning = running_.exchange(false, std::memory_order_relaxed);
  if (!wasRunning && !input_ && !output_) return;

  // The output first: once it is gone nothing reads the ring any more, and
  // the speakers go quiet at once rather than playing out what is left.
  if (output_) {
    output_->Stop();
    output_.reset();
  }
  if (input_) {
    input_->Stop();
    input_.reset();
  }
  ring_.Reset(0);
  captureRate_.store(0, std::memory_order_relaxed);
}

void AudioEngine::OnCapturedAudio(const float* interleaved, size_t frames) {
  // Peak with decay, for the level meter in the settings.
  if (interleaved && frames > 0) {
    float blockPeak = 0.0f;
    for (size_t i = 0; i < frames * 2; ++i) {
      const float magnitude = std::abs(interleaved[i]);
      if (magnitude > blockPeak) blockPeak = magnitude;
    }
    const float decayed = inputPeak_.load(std::memory_order_relaxed) * 0.80f;
    inputPeak_.store(std::max(blockPeak, decayed), std::memory_order_relaxed);
  }

  ring_.Write(interleaved, frames);
  if (tapEnabled_.load(std::memory_order_relaxed)) {
    tapRing_.Write(interleaved, frames);
  }
}

void AudioEngine::SetTapEnabled(bool enabled) {
  if (enabled == tapEnabled_.load(std::memory_order_relaxed)) return;
  if (enabled) {
    // Four seconds is enough to ride out an encoder hiccup without letting a
    // stalled recorder grow the buffer forever; past that the ring drops the
    // oldest audio and counts it, which the recorder surfaces.
    const int rate = captureRate_.load(std::memory_order_relaxed);
    tapRing_.Reset((size_t)(rate > 0 ? rate : 48000) * 4);
  }
  tapEnabled_.store(enabled, std::memory_order_relaxed);
  if (!enabled) tapRing_.Reset(0);
}

size_t AudioEngine::ReadTap(float* out, size_t frames) {
  if (!tapEnabled_.load(std::memory_order_relaxed)) return 0;
  return tapRing_.Read(out, frames);
}

void AudioEngine::ApplySettings(const AudioSettings& settings) {
  volume_.store(Clamp(settings.volume, 0.0f, 1.0f), std::memory_order_relaxed);
  mute_.store(settings.mute, std::memory_order_relaxed);
  targetMs_.store(Clamp(settings.bufferMs + std::max(0, settings.avOffsetMs), 2, 1000),
                  std::memory_order_relaxed);
}

AudioStats AudioEngine::stats() const {
  AudioStats s;
  s.running = running_.load(std::memory_order_relaxed);
  s.exclusive = exclusiveActive_.load(std::memory_order_relaxed);
  s.captureRate = captureRate_.load(std::memory_order_relaxed);
  s.captureChannels = captureChannels_.load(std::memory_order_relaxed);
  s.renderRate = renderRate_.load(std::memory_order_relaxed);
  s.renderChannels = renderChannels_.load(std::memory_order_relaxed);
  const double effective = effectiveTargetMs_.load(std::memory_order_relaxed);
  s.targetMs = effective > 0.0 ? effective : (double)targetMs_.load(std::memory_order_relaxed);
  s.underruns = underruns_.load(std::memory_order_relaxed);
  s.overruns = ring_.overruns();
  if (s.captureRate > 0) {
    s.bufferMs = (double)ring_.Available() * 1000.0 / (double)s.captureRate;
  }
  {
    std::lock_guard<std::mutex> lock(statsMutex_);
    s.inputName = inputName_;
    s.inputVia = inputVia_;
    s.outputName = outputName_;
  }
  return s;
}

// ------------------------------------------------------------------- playback

void AudioEngine::OnOutputStart(const AudioOutputFormat& format) {
  renderRate_.store(format.sampleRate, std::memory_order_relaxed);
  renderChannels_.store(format.channels, std::memory_order_relaxed);
  exclusiveActive_.store(format.exclusive, std::memory_order_relaxed);
  outputRate_ = format.sampleRate;

  // The ring has to hold more than one device buffer, otherwise every bit of
  // jitter on the capture side empties it before the next period. Whatever the
  // user asked for, this is the floor.
  const double devicePeriodMs =
      format.sampleRate > 0 ? 1000.0 * (double)format.bufferFrames / (double)format.sampleRate
                            : 10.0;
  minTargetMs_ = devicePeriodMs + 8.0;

  srcFrac_ = 0.0;
  prev_[0] = 0.0f;
  prev_[1] = 0.0f;
  primed_ = false;
  pending_.clear();
}

AudioFill AudioEngine::FillOutput(float* out, size_t frames) {
  AudioFill silence;
  silence.silent = true;

  const int captureRate = captureRate_.load(std::memory_order_relaxed);
  if (captureRate <= 0) {
    // Capture side is not up yet: keep the output running on silence.
    return silence;
  }

  // Wait until the ring holds roughly the target before starting, otherwise
  // the first second is nothing but underruns.
  const double targetMs =
      std::max((double)targetMs_.load(std::memory_order_relaxed), minTargetMs_);
  effectiveTargetMs_.store(targetMs, std::memory_order_relaxed);
  const size_t targetFrames = (size_t)(targetMs * captureRate / 1000.0);
  const size_t available = ring_.Available() + pending_.size() / 2;
  if (!primed_) {
    if (available < targetFrames) return silence;
    primed_ = true;
    srcFrac_ = 0.0;
  } else if (available * 4 < targetFrames) {
    // Persistently starved -- the source is delivering less than real time,
    // which is what a card with no signal locked does. One clean gap of
    // silence while the ring refills beats grinding along on held samples.
    primed_ = false;
    return silence;
  }

  // Nudge the playback rate by a fraction of a percent to hold the target
  // fill. Inaudible, and it absorbs the clock drift between the card and the
  // sound device without ever cutting the stream.
  double ratio = (double)captureRate / (double)outputRate_;
  if (targetFrames > 0) {
    const double err = ((double)available - (double)targetFrames) / (double)targetFrames;
    ratio *= Clamp(1.0 + 0.05 * err, 0.997, 1.003);
  }

  // Index 0 of the virtual source is `prev`, index k+1 is pending[k]. The
  // interpolator may reach one frame past the last output sample, and the
  // consumed count can exceed that when downsampling, so take the larger.
  const double endPos = srcFrac_ + ratio * frames;
  const size_t shift = (size_t)std::floor(endPos);
  const double lastPos = srcFrac_ + ratio * (double)(frames - 1);
  const size_t maxIndex = (size_t)std::floor(lastPos) + 1;
  const size_t needed = std::max(shift, maxIndex);

  size_t have = pending_.size() / 2;
  if (have < needed) {
    const size_t want = needed - have;
    pending_.resize(needed * 2, 0.0f);
    const size_t got = ring_.Read(pending_.data() + have * 2, want);
    if (got < want) {
      underruns_.fetch_add(1, std::memory_order_relaxed);
      if (got == 0) primed_ = false;  // re-prime instead of grinding along empty
      // Hold the last known value rather than dropping to silence: far less
      // clicky, and a short hold is nearly inaudible.
      const size_t filled = have + got;
      const float holdL = filled > 0 ? pending_[(filled - 1) * 2 + 0] : prev_[0];
      const float holdR = filled > 0 ? pending_[(filled - 1) * 2 + 1] : prev_[1];
      for (size_t i = filled; i < needed; ++i) {
        pending_[i * 2 + 0] = holdL;
        pending_[i * 2 + 1] = holdR;
      }
    }
  }

  for (size_t i = 0; i < frames; ++i) {
    const double pos = srcFrac_ + ratio * i;
    const size_t i0 = (size_t)pos;
    const float f = (float)(pos - (double)i0);
    const float a0 = i0 == 0 ? prev_[0] : pending_[(i0 - 1) * 2 + 0];
    const float a1 = i0 == 0 ? prev_[1] : pending_[(i0 - 1) * 2 + 1];
    const float b0 = pending_[i0 * 2 + 0];
    const float b1 = pending_[i0 * 2 + 1];
    out[i * 2 + 0] = a0 + (b0 - a0) * f;
    out[i * 2 + 1] = a1 + (b1 - a1) * f;
  }

  // Advance: everything up to `shift` is used up, the rest stays for the next
  // period so no sample is ever silently dropped.
  if (shift > 0) {
    prev_[0] = pending_[(shift - 1) * 2 + 0];
    prev_[1] = pending_[(shift - 1) * 2 + 1];
    pending_.erase(pending_.begin(), pending_.begin() + (ptrdiff_t)(shift * 2));
  }
  srcFrac_ = endPos - (double)shift;

  AudioFill fill;
  fill.gain =
      mute_.load(std::memory_order_relaxed) ? 0.0f : volume_.load(std::memory_order_relaxed);
  return fill;
}

}  // namespace cap
