#include "audio/audio_engine.h"
#include "i18n.h"

#include <avrt.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cap {
namespace {

const REFERENCE_TIME kMsToRefTime = 10000;

// One second at 96 kHz, so the ring never has to be resized once the rate is
// known and the render thread can read it without synchronising on that.
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
  directShowInput_.store(inputInfo.directShow, std::memory_order_relaxed);

  {
    std::lock_guard<std::mutex> lock(statsMutex_);
    inputName_ = inputInfo.name;
    outputName_ = outputInfo.name;
    lastError_.clear();
  }

  stopEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!stopEvent_) {
    ReportError(error, CAP_SAID(T("Audio-Stopp-Event konnte nicht erstellt werden",
                                     "The audio stop event could not be created")));
    return false;
  }
  ring_.Reset(kRingCapacityFrames);
  running_.store(true, std::memory_order_relaxed);

  if (inputInfo.directShow) {
    std::string err;
    auto sink = [this](const float* data, size_t frames) { OnCapturedAudio(data, frames); };
    if (!dshowCapture_.Start(inputInfo.ToRef(), sink, &err)) {
      running_.store(false, std::memory_order_relaxed);
      ::CloseHandle(stopEvent_);
      stopEvent_ = nullptr;
      if (error) *error = err;
      return false;
    }
    const StreamFormat fmt = dshowCapture_.format();
    captureRate_.store(fmt.sampleRate, std::memory_order_relaxed);
    captureChannels_.store(fmt.channels, std::memory_order_relaxed);
  } else {
    captureThread_ = std::thread(&AudioEngine::CaptureThread, this, inputInfo);
  }

  renderThread_ = std::thread(&AudioEngine::RenderThread, this, outputInfo, settings.exclusive);

  CAP_LOG("Audio started: '%s' (%s) -> '%s', target %d ms%s", inputInfo.name.c_str(),
          inputInfo.directShow ? "DirectShow" : "WASAPI", outputInfo.name.c_str(),
          targetMs_.load(), settings.exclusive ? ", Exclusive" : "");
  return true;
}

void AudioEngine::Stop() {
  const bool wasRunning = running_.exchange(false, std::memory_order_relaxed);
  if (!wasRunning && !captureThread_.joinable() && !renderThread_.joinable()) {
    if (stopEvent_) {
      ::CloseHandle(stopEvent_);
      stopEvent_ = nullptr;
    }
    return;
  }

  if (stopEvent_) ::SetEvent(stopEvent_);
  dshowCapture_.Stop();
  if (captureThread_.joinable()) captureThread_.join();
  if (renderThread_.joinable()) renderThread_.join();
  if (stopEvent_) {
    ::CloseHandle(stopEvent_);
    stopEvent_ = nullptr;
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
  s.directShowInput = directShowInput_.load(std::memory_order_relaxed);
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
    s.outputName = outputName_;
  }
  return s;
}

// ------------------------------------------------------- WASAPI capture thread

void AudioEngine::CaptureThread(AudioDeviceInfo device) {
  ComScope com(COINIT_MULTITHREADED);
  HANDLE mmcss = JoinProAudio();
  HANDLE dataEvent = nullptr;

  auto cleanup = [&]() {
    if (dataEvent) ::CloseHandle(dataEvent);
    if (mmcss) ::AvRevertMmThreadCharacteristics(mmcss);
  };

  ComPtr<IMMDevice> endpoint = OpenAudioEndpoint(device, true);
  if (!endpoint) {
    Fail(CAP_SAID(T("Audioeingang konnte nicht geöffnet werden", "The audio input could not be opened")));
    cleanup();
    return;
  }

  ComPtr<IAudioClient> client;
  if (FAILED(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client))) {
    Fail(CAP_SAID(T("Audioeingang konnte nicht aktiviert werden",
                    "The audio input could not be activated")));
    cleanup();
    return;
  }

  WAVEFORMATEX* mixFormat = nullptr;
  if (FAILED(client->GetMixFormat(&mixFormat)) || !mixFormat) {
    Fail(CAP_SAID(T("Audioformat des Eingangs konnte nicht ermittelt werden",
                    "The audio input's format could not be determined")));
    cleanup();
    return;
  }

  StreamFormat fmt;
  if (!ParseWaveFormat(mixFormat, &fmt)) {
    ::CoTaskMemFree(mixFormat);
    Fail(CAP_SAID(T("Der Audioeingang liefert ein unbekanntes Format",
                    "The audio input delivers an unknown format")));
    cleanup();
    return;
  }

  dataEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
  // A short device buffer: the ring is where the latency is meant to live.
  HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                  20 * kMsToRefTime, 0, mixFormat, nullptr);
  ::CoTaskMemFree(mixFormat);
  if (FAILED(hr)) {
    Fail(CAP_SAID(T("Audioeingang konnte nicht initialisiert werden: ",
                    "The audio input could not be initialised: ") +
                  HrToString(hr)));
    cleanup();
    return;
  }
  client->SetEventHandle(dataEvent);

  ComPtr<IAudioCaptureClient> capture;
  if (FAILED(client->GetService(IID_PPV_ARGS(&capture)))) {
    Fail(CAP_SAID(T("IAudioCaptureClient nicht verfügbar", "IAudioCaptureClient not available")));
    cleanup();
    return;
  }

  captureChannels_.store(fmt.channels, std::memory_order_relaxed);
  captureRate_.store(fmt.sampleRate, std::memory_order_relaxed);

  client->Start();
  CAP_LOG("WASAPI capture: %d Hz, %d channels, %d bit%s", fmt.sampleRate, fmt.channels,
          fmt.bitsPerSample, fmt.isFloat ? " float" : "");

  std::vector<float> scratch;
  HANDLE waits[2] = {stopEvent_, dataEvent};

  while (running_.load(std::memory_order_relaxed)) {
    DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, 200);
    if (w == WAIT_OBJECT_0) break;  // stop requested
    if (w == WAIT_TIMEOUT) continue;

    UINT32 packet = 0;
    while (SUCCEEDED(hr = capture->GetNextPacketSize(&packet)) && packet > 0) {
      BYTE* data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
      if (hr == AUDCLNT_S_BUFFER_EMPTY) break;
      if (FAILED(hr)) break;

      if (frames > 0) {
        scratch.resize((size_t)frames * 2);
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
          std::fill(scratch.begin(), scratch.end(), 0.0f);
        } else {
          ToStereoFloat(data, frames, fmt, scratch.data());
        }
        OnCapturedAudio(scratch.data(), frames);
      }
      capture->ReleaseBuffer(frames);
    }

    if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
      Fail(CAP_SAID(T("Der Audioeingang wurde entfernt", "The audio input was removed")));
      break;
    }
  }

  client->Stop();
  cleanup();
}

// -------------------------------------------------------------- render thread

void AudioEngine::RenderThread(AudioDeviceInfo device, bool exclusive) {
  ComScope com(COINIT_MULTITHREADED);
  HANDLE mmcss = JoinProAudio();
  HANDLE dataEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);

  auto cleanup = [&]() {
    if (dataEvent) ::CloseHandle(dataEvent);
    if (mmcss) ::AvRevertMmThreadCharacteristics(mmcss);
  };

  ComPtr<IMMDevice> endpoint = OpenAudioEndpoint(device, false);
  if (!endpoint) {
    Fail(CAP_SAID(T("Wiedergabegerät konnte nicht geöffnet werden",
                    "The playback device could not be opened")));
    cleanup();
    return;
  }

  ComPtr<IAudioClient> client;
  if (FAILED(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client))) {
    Fail(CAP_SAID(T("Wiedergabegerät konnte nicht aktiviert werden",
                    "The playback device could not be activated")));
    cleanup();
    return;
  }

  WAVEFORMATEX* mixFormat = nullptr;
  if (FAILED(client->GetMixFormat(&mixFormat)) || !mixFormat) {
    Fail(CAP_SAID(T("Audioformat der Wiedergabe konnte nicht ermittelt werden",
                    "The playback format could not be determined")));
    cleanup();
    return;
  }

  REFERENCE_TIME defaultPeriod = 0, minPeriod = 0;
  client->GetDevicePeriod(&defaultPeriod, &minPeriod);

  HRESULT hr = E_FAIL;
  bool usingExclusive = false;
  StreamFormat fmt;
  WAVEFORMATEX pcm = {};  // must outlive `candidate`

  if (exclusive) {
    // Exclusive mode wants a format the hardware takes as-is. Try the mix
    // format first, then plain 16 bit PCM at the same rate.
    WAVEFORMATEX* candidate = mixFormat;
    if (client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, candidate, nullptr) != S_OK) {
      pcm.wFormatTag = WAVE_FORMAT_PCM;
      pcm.nChannels = mixFormat->nChannels > 2 ? 2 : mixFormat->nChannels;
      pcm.nSamplesPerSec = mixFormat->nSamplesPerSec;
      pcm.wBitsPerSample = 16;
      pcm.nBlockAlign = (WORD)(pcm.nChannels * pcm.wBitsPerSample / 8);
      pcm.nAvgBytesPerSec = pcm.nSamplesPerSec * pcm.nBlockAlign;
      pcm.cbSize = 0;
      candidate = (client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &pcm, nullptr) == S_OK)
                      ? &pcm
                      : nullptr;
    }
    if (candidate) {
      hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                              minPeriod, minPeriod, candidate, nullptr);
      if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        // Retry with the buffer size the driver actually wants.
        UINT32 aligned = 0;
        client->GetBufferSize(&aligned);
        client.Reset();
        if (SUCCEEDED(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                         (void**)&client)) &&
            candidate->nSamplesPerSec > 0) {
          const REFERENCE_TIME period =
              (REFERENCE_TIME)(10000.0 * 1000.0 * aligned / candidate->nSamplesPerSec + 0.5);
          hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                  period, period, candidate, nullptr);
        }
      }
      if (SUCCEEDED(hr)) {
        usingExclusive = ParseWaveFormat(candidate, &fmt);
        if (!usingExclusive) hr = E_FAIL;
      } else {
        CAP_WARN("Exclusive mode not possible (%s), using shared mode", HrToEnglish(hr).c_str());
      }
    } else {
      CAP_WARN("Exclusive mode: no suitable format, using shared mode");
    }
  }

  if (!usingExclusive) {
    if (!client) {
      if (FAILED(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client))) {
        ::CoTaskMemFree(mixFormat);
        Fail(CAP_SAID(T("Wiedergabegerät konnte nicht aktiviert werden",
                        "The playback device could not be activated")));
        cleanup();
        return;
      }
    }
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                            defaultPeriod > 0 ? defaultPeriod : 10 * kMsToRefTime, 0, mixFormat,
                            nullptr);
    if (!ParseWaveFormat(mixFormat, &fmt)) hr = E_FAIL;
  }
  ::CoTaskMemFree(mixFormat);
  mixFormat = nullptr;

  if (FAILED(hr) || !fmt.valid()) {
    Fail(CAP_SAID(T("Wiedergabe konnte nicht initialisiert werden: ",
                    "Playback could not be initialised: ") +
                  HrToString(hr)));
    cleanup();
    return;
  }

  client->SetEventHandle(dataEvent);

  ComPtr<IAudioRenderClient> render;
  if (FAILED(client->GetService(IID_PPV_ARGS(&render)))) {
    Fail(CAP_SAID(T("IAudioRenderClient nicht verfügbar", "IAudioRenderClient not available")));
    cleanup();
    return;
  }

  UINT32 bufferFrames = 0;
  client->GetBufferSize(&bufferFrames);

  renderRate_.store(fmt.sampleRate, std::memory_order_relaxed);
  renderChannels_.store(fmt.channels, std::memory_order_relaxed);
  exclusiveActive_.store(usingExclusive, std::memory_order_relaxed);

  CAP_LOG("Audio playback: %d Hz, %d channels, %d bit%s, buffer %u frames, %s", fmt.sampleRate,
          fmt.channels, fmt.bitsPerSample, fmt.isFloat ? " float" : "", bufferFrames,
          usingExclusive ? "Exclusive" : "Shared");

  client->Start();

  // The ring has to hold more than one device buffer, otherwise every bit of
  // jitter on the capture side empties it before the next callback. Whatever
  // the user asked for, this is the floor.
  const double devicePeriodMs =
      fmt.sampleRate > 0 ? 1000.0 * (double)bufferFrames / (double)fmt.sampleRate : 10.0;
  const double minTargetMs = devicePeriodMs + 8.0;

  // Resampler state: srcFrac is where we are inside the current source frame,
  // prev holds the frame before it so interpolation always has a left sample.
  // `pending` keeps frames that were read from the ring but not consumed yet --
  // without it every callback would quietly throw one frame away.
  double srcFrac = 0.0;
  float prev[2] = {0.0f, 0.0f};
  bool primed = false;

  std::vector<float> pending;
  std::vector<float> outBuffer;
  HANDLE waits[2] = {stopEvent_, dataEvent};

  auto writeSilence = [&](UINT32 frames) {
    BYTE* data = nullptr;
    if (SUCCEEDED(render->GetBuffer(frames, &data))) {
      render->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
    }
  };

  while (running_.load(std::memory_order_relaxed)) {
    DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, 200);
    if (w == WAIT_OBJECT_0) break;
    if (w == WAIT_TIMEOUT) continue;

    UINT32 padding = 0;
    if (!usingExclusive && FAILED(client->GetCurrentPadding(&padding))) break;
    const UINT32 frames = usingExclusive ? bufferFrames : (bufferFrames - padding);
    if (frames == 0) continue;

    const int captureRate = captureRate_.load(std::memory_order_relaxed);
    if (captureRate <= 0) {
      // Capture side is not up yet: keep the endpoint running on silence.
      writeSilence(frames);
      continue;
    }

    // Wait until the ring holds roughly the target before starting, otherwise
    // the first second is nothing but underruns.
    const double targetMs =
        std::max((double)targetMs_.load(std::memory_order_relaxed), minTargetMs);
    effectiveTargetMs_.store(targetMs, std::memory_order_relaxed);
    const size_t targetFrames = (size_t)(targetMs * captureRate / 1000.0);
    const size_t available = ring_.Available() + pending.size() / 2;
    if (!primed) {
      if (available < targetFrames) {
        writeSilence(frames);
        continue;
      }
      primed = true;
      srcFrac = 0.0;
    } else if (available * 4 < targetFrames) {
      // Persistently starved -- the source is delivering less than real time,
      // which is what a card with no signal locked does. One clean gap of
      // silence while the ring refills beats grinding along on held samples.
      primed = false;
      writeSilence(frames);
      continue;
    }

    // Nudge the playback rate by a fraction of a percent to hold the target
    // fill. Inaudible, and it absorbs the clock drift between the card and the
    // sound device without ever cutting the stream.
    double ratio = (double)captureRate / (double)fmt.sampleRate;
    if (targetFrames > 0) {
      const double err = ((double)available - (double)targetFrames) / (double)targetFrames;
      ratio *= Clamp(1.0 + 0.05 * err, 0.997, 1.003);
    }

    // Index 0 of the virtual source is `prev`, index k+1 is pending[k]. The
    // interpolator may reach one frame past the last output sample, and the
    // consumed count can exceed that when downsampling, so take the larger.
    const double endPos = srcFrac + ratio * frames;
    const size_t shift = (size_t)std::floor(endPos);
    const double lastPos = srcFrac + ratio * (double)(frames - 1);
    const size_t maxIndex = (size_t)std::floor(lastPos) + 1;
    const size_t needed = std::max(shift, maxIndex);

    size_t have = pending.size() / 2;
    if (have < needed) {
      const size_t want = needed - have;
      pending.resize(needed * 2, 0.0f);
      const size_t got = ring_.Read(pending.data() + have * 2, want);
      if (got < want) {
        underruns_.fetch_add(1, std::memory_order_relaxed);
        if (got == 0) primed = false;  // re-prime instead of grinding along empty
        // Hold the last known value rather than dropping to silence: far less
        // clicky, and a short hold is nearly inaudible.
        const size_t filled = have + got;
        const float holdL = filled > 0 ? pending[(filled - 1) * 2 + 0] : prev[0];
        const float holdR = filled > 0 ? pending[(filled - 1) * 2 + 1] : prev[1];
        for (size_t i = filled; i < needed; ++i) {
          pending[i * 2 + 0] = holdL;
          pending[i * 2 + 1] = holdR;
        }
      }
    }

    outBuffer.assign((size_t)frames * 2, 0.0f);
    for (UINT32 i = 0; i < frames; ++i) {
      const double pos = srcFrac + ratio * i;
      const size_t i0 = (size_t)pos;
      const float f = (float)(pos - (double)i0);
      const float a0 = i0 == 0 ? prev[0] : pending[(i0 - 1) * 2 + 0];
      const float a1 = i0 == 0 ? prev[1] : pending[(i0 - 1) * 2 + 1];
      const float b0 = pending[i0 * 2 + 0];
      const float b1 = pending[i0 * 2 + 1];
      outBuffer[i * 2 + 0] = a0 + (b0 - a0) * f;
      outBuffer[i * 2 + 1] = a1 + (b1 - a1) * f;
    }

    // Advance: everything up to `shift` is used up, the rest stays for the next
    // callback so no sample is ever silently dropped.
    if (shift > 0) {
      prev[0] = pending[(shift - 1) * 2 + 0];
      prev[1] = pending[(shift - 1) * 2 + 1];
      pending.erase(pending.begin(), pending.begin() + (ptrdiff_t)(shift * 2));
    }
    srcFrac = endPos - (double)shift;

    const float gain =
        mute_.load(std::memory_order_relaxed) ? 0.0f : volume_.load(std::memory_order_relaxed);

    BYTE* data = nullptr;
    hr = render->GetBuffer(frames, &data);
    if (FAILED(hr)) {
      if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
        Fail(CAP_SAID(T("Das Wiedergabegerät wurde entfernt", "The playback device was removed")));
        break;
      }
      continue;
    }
    FromStereoFloat(outBuffer.data(), frames, fmt, data, gain);
    render->ReleaseBuffer(frames, 0);
  }

  client->Stop();
  cleanup();
}

}  // namespace cap
