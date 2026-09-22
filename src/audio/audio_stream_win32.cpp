#include "audio/audio_stream.h"

#include "audio/audio_win32.h"

#include <audioclient.h>
#include <avrt.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

#include "audio/dshow_audio_capture.h"

namespace cap {
namespace {

const REFERENCE_TIME kMsToRefTime = 10000;

// ------------------------------------------------------------------ WASAPI in

// What differs between the two uses of a capture client: how big its device
// buffer is and how it words its failures.
struct InputWording {
  REFERENCE_TIME buffer;
  Said (*notOpened)();
  Said (*notActivated)();
  Said (*noFormat)();
  Said (*unknownFormat)();
  Said (*notInitialised)(HRESULT hr);
  Said (*noCaptureClient)();
  Said (*removed)();
  Said (*noEvent)();
};

// A short device buffer: the ring is where the latency is meant to live.
const InputWording kPassthroughWording = {
    20 * kMsToRefTime,
    [] {
      return CAP_SAID(T("Audioeingang konnte nicht geöffnet werden",
                        "The audio input could not be opened"));
    },
    [] {
      return CAP_SAID(T("Audioeingang konnte nicht aktiviert werden",
                        "The audio input could not be activated"));
    },
    [] {
      return CAP_SAID(T("Audioformat des Eingangs konnte nicht ermittelt werden",
                        "The audio input's format could not be determined"));
    },
    [] {
      return CAP_SAID(T("Der Audioeingang liefert ein unbekanntes Format",
                        "The audio input delivers an unknown format"));
    },
    [](HRESULT hr) {
      return CAP_SAID(T("Audioeingang konnte nicht initialisiert werden: ",
                        "The audio input could not be initialised: ") +
                      HrToString(hr));
    },
    [] {
      return CAP_SAID(T("IAudioCaptureClient nicht verfügbar", "IAudioCaptureClient not available"));
    },
    [] { return CAP_SAID(T("Der Audioeingang wurde entfernt", "The audio input was removed")); },
    [] {
      return CAP_SAID(T("Audio-Stopp-Event konnte nicht erstellt werden",
                        "The audio stop event could not be created"));
    },
};

const InputWording kMicrophoneWording = {
    40 * kMsToRefTime,
    [] {
      return CAP_SAID(T("Mikrofon konnte nicht geöffnet werden.", "Could not open the microphone."));
    },
    [] {
      return CAP_SAID(T("Mikrofon konnte nicht aktiviert werden.",
                        "Could not activate the microphone."));
    },
    [] { return CAP_SAID(T("Unbekanntes Mikrofonformat.", "Unknown microphone format.")); },
    [] { return CAP_SAID(T("Unbekanntes Mikrofonformat.", "Unknown microphone format.")); },
    [](HRESULT hr) {
      return CAP_SAID(T("Mikrofon konnte nicht initialisiert werden: ",
                        "Could not initialise the microphone: ") + HrToString(hr));
    },
    [] {
      return CAP_SAID(T("IAudioCaptureClient nicht verfügbar.",
                        "IAudioCaptureClient is not available."));
    },
    [] { return CAP_SAID(T("Das Mikrofon wurde entfernt.", "The microphone was removed.")); },
    [] {
      return CAP_SAID(T("Ereignis konnte nicht erstellt werden.", "Could not create the event."));
    },
};

class WasapiInput : public AudioInput {
 public:
  ~WasapiInput() override { Stop(); }

  bool Start(const AudioDeviceInfo& device, AudioRole role, AudioInputEvents events,
             std::string* error) override {
    Stop();
    const InputWording& wording =
        role == AudioRole::Microphone ? kMicrophoneWording : kPassthroughWording;
    stopEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) return ReportError(error, wording.noEvent());
    events_ = std::move(events);
    running_.store(true, std::memory_order_relaxed);
    thread_ = std::thread(&WasapiInput::Run, this, device, role, &wording);
    return true;
  }

  void Stop() override {
    running_.store(false, std::memory_order_relaxed);
    if (stopEvent_) ::SetEvent(stopEvent_);
    if (thread_.joinable()) thread_.join();
    if (stopEvent_) {
      ::CloseHandle(stopEvent_);
      stopEvent_ = nullptr;
    }
  }

 private:
  void Run(AudioDeviceInfo device, AudioRole role, const InputWording* wording) {
    ComScope com(COINIT_MULTITHREADED);
    HANDLE mmcss = JoinProAudio();
    HANDLE dataEvent = nullptr;

    auto cleanup = [&]() {
      if (dataEvent) ::CloseHandle(dataEvent);
      if (mmcss) ::AvRevertMmThreadCharacteristics(mmcss);
    };
    // Whatever happens, whoever waits for the format must hear of it.
    auto report = [&](const Said& said) {
      if (events_.onFailure) events_.onFailure(said);
    };
    auto fail = [&](const Said& said) {
      report(said);
      cleanup();
    };

    ComPtr<IMMDevice> endpoint = OpenAudioEndpoint(device, true);
    if (!endpoint) return fail(wording->notOpened());

    ComPtr<IAudioClient> client;
    if (FAILED(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client))) {
      return fail(wording->notActivated());
    }

    WAVEFORMATEX* mixFormat = nullptr;
    if (FAILED(client->GetMixFormat(&mixFormat)) || !mixFormat) {
      if (mixFormat) ::CoTaskMemFree(mixFormat);
      return fail(wording->noFormat());
    }

    StreamFormat fmt;
    if (!ParseWaveFormat(mixFormat, &fmt)) {
      ::CoTaskMemFree(mixFormat);
      return fail(wording->unknownFormat());
    }

    dataEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                    wording->buffer, 0, mixFormat, nullptr);
    ::CoTaskMemFree(mixFormat);
    if (FAILED(hr)) return fail(wording->notInitialised(hr));
    client->SetEventHandle(dataEvent);

    ComPtr<IAudioCaptureClient> capture;
    if (FAILED(client->GetService(IID_PPV_ARGS(&capture)))) return fail(wording->noCaptureClient());

    if (events_.onFormat) events_.onFormat(fmt.sampleRate, fmt.channels);

    client->Start();
    if (role == AudioRole::Microphone) {
      CAP_LOG("Microphone running: '%s', %d Hz, %d channels", device.name.c_str(), fmt.sampleRate,
              fmt.channels);
    } else {
      CAP_LOG("WASAPI capture: %d Hz, %d channels, %d bit%s", fmt.sampleRate, fmt.channels,
              fmt.bitsPerSample, fmt.isFloat ? " float" : "");
    }

    std::vector<float> scratch;
    HANDLE waits[2] = {stopEvent_, dataEvent};

    while (running_.load(std::memory_order_relaxed)) {
      const DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, 200);
      if (w == WAIT_OBJECT_0) break;  // stop requested
      if (w == WAIT_TIMEOUT) {
        if (events_.onQuiet) events_.onQuiet();
        continue;
      }

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
          events_.onAudio(scratch.data(), frames);
        }
        capture->ReleaseBuffer(frames);
      }

      if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
        report(wording->removed());
        break;
      }
    }

    client->Stop();
    cleanup();
  }

  std::thread thread_;
  HANDLE stopEvent_ = nullptr;
  std::atomic<bool> running_{false};
  AudioInputEvents events_;
};

// ------------------------------------------------------------- DirectShow in

// Cards whose embedded audio Windows never publishes as a sound device. The
// graph delivers on a streaming thread of its own; the format is known once
// the pins have connected, which is before Start returns.
class DShowInput : public AudioInput {
 public:
  ~DShowInput() override { Stop(); }

  bool Start(const AudioDeviceInfo& device, AudioRole /*role*/, AudioInputEvents events,
             std::string* error) override {
    Stop();
    events_ = std::move(events);
    if (!capture_.Start(device.ToRef(), events_.onAudio, error)) return false;
    const StreamFormat fmt = capture_.format();
    if (events_.onFormat) events_.onFormat(fmt.sampleRate, fmt.channels);
    return true;
  }

  void Stop() override { capture_.Stop(); }

 private:
  DShowAudioCapture capture_;
  AudioInputEvents events_;
};

// ----------------------------------------------------------------- WASAPI out

class WasapiOutput : public AudioOutput {
 public:
  ~WasapiOutput() override { Stop(); }

  bool Start(const AudioDeviceInfo& device, bool exclusive, AudioOutputEvents events,
             std::string* error) override {
    Stop();
    stopEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) {
      return ReportError(error, CAP_SAID(T("Audio-Stopp-Event konnte nicht erstellt werden",
                                           "The audio stop event could not be created")));
    }
    events_ = std::move(events);
    running_.store(true, std::memory_order_relaxed);
    thread_ = std::thread(&WasapiOutput::Run, this, device, exclusive);
    return true;
  }

  void Stop() override {
    running_.store(false, std::memory_order_relaxed);
    if (stopEvent_) ::SetEvent(stopEvent_);
    if (thread_.joinable()) thread_.join();
    if (stopEvent_) {
      ::CloseHandle(stopEvent_);
      stopEvent_ = nullptr;
    }
  }

 private:
  void Run(AudioDeviceInfo device, bool exclusive) {
    ComScope com(COINIT_MULTITHREADED);
    HANDLE mmcss = JoinProAudio();
    HANDLE dataEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);

    auto cleanup = [&]() {
      if (dataEvent) ::CloseHandle(dataEvent);
      if (mmcss) ::AvRevertMmThreadCharacteristics(mmcss);
    };
    auto report = [&](const Said& said) {
      if (events_.onFailure) events_.onFailure(said);
    };
    auto fail = [&](const Said& said) {
      report(said);
      cleanup();
    };

    ComPtr<IMMDevice> endpoint = OpenAudioEndpoint(device, false);
    if (!endpoint) {
      return fail(CAP_SAID(T("Wiedergabegerät konnte nicht geöffnet werden",
                             "The playback device could not be opened")));
    }

    ComPtr<IAudioClient> client;
    if (FAILED(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client))) {
      return fail(CAP_SAID(T("Wiedergabegerät konnte nicht aktiviert werden",
                             "The playback device could not be activated")));
    }

    WAVEFORMATEX* mixFormat = nullptr;
    if (FAILED(client->GetMixFormat(&mixFormat)) || !mixFormat) {
      return fail(CAP_SAID(T("Audioformat der Wiedergabe konnte nicht ermittelt werden",
                             "The playback format could not be determined")));
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
          return fail(CAP_SAID(T("Wiedergabegerät konnte nicht aktiviert werden",
                                 "The playback device could not be activated")));
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
      return fail(CAP_SAID(T("Wiedergabe konnte nicht initialisiert werden: ",
                             "Playback could not be initialised: ") +
                           HrToString(hr)));
    }

    client->SetEventHandle(dataEvent);

    ComPtr<IAudioRenderClient> render;
    if (FAILED(client->GetService(IID_PPV_ARGS(&render)))) {
      return fail(
          CAP_SAID(T("IAudioRenderClient nicht verfügbar", "IAudioRenderClient not available")));
    }

    UINT32 bufferFrames = 0;
    client->GetBufferSize(&bufferFrames);

    AudioOutputFormat format;
    format.sampleRate = fmt.sampleRate;
    format.channels = fmt.channels;
    format.bufferFrames = (int)bufferFrames;
    format.exclusive = usingExclusive;
    if (events_.onStart) events_.onStart(format);

    CAP_LOG("Audio playback: %d Hz, %d channels, %d bit%s, buffer %u frames, %s", fmt.sampleRate,
            fmt.channels, fmt.bitsPerSample, fmt.isFloat ? " float" : "", bufferFrames,
            usingExclusive ? "Exclusive" : "Shared");

    client->Start();

    std::vector<float> outBuffer;
    HANDLE waits[2] = {stopEvent_, dataEvent};

    auto writeSilence = [&](UINT32 frames) {
      BYTE* data = nullptr;
      if (SUCCEEDED(render->GetBuffer(frames, &data))) {
        render->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
      }
    };

    while (running_.load(std::memory_order_relaxed)) {
      const DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, 200);
      if (w == WAIT_OBJECT_0) break;
      if (w == WAIT_TIMEOUT) continue;

      UINT32 padding = 0;
      if (!usingExclusive && FAILED(client->GetCurrentPadding(&padding))) break;
      const UINT32 frames = usingExclusive ? bufferFrames : (bufferFrames - padding);
      if (frames == 0) continue;

      outBuffer.resize((size_t)frames * 2);
      const AudioFill fill = events_.fill(outBuffer.data(), frames);
      if (fill.silent) {
        writeSilence(frames);
        continue;
      }

      BYTE* data = nullptr;
      hr = render->GetBuffer(frames, &data);
      if (FAILED(hr)) {
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
          report(CAP_SAID(T("Das Wiedergabegerät wurde entfernt", "The playback device was removed")));
          break;
        }
        continue;
      }
      FromStereoFloat(outBuffer.data(), frames, fmt, data, fill.gain);
      render->ReleaseBuffer(frames, 0);
    }

    client->Stop();
    cleanup();
  }

  std::thread thread_;
  HANDLE stopEvent_ = nullptr;
  std::atomic<bool> running_{false};
  AudioOutputEvents events_;
};

}  // namespace

std::unique_ptr<AudioInput> CreateAudioInput(const AudioDeviceInfo& device) {
  if (device.backend == kDShowBackend) return std::make_unique<DShowInput>();
  // WASAPI, and also a device with no backend at all: the system default.
  return std::make_unique<WasapiInput>();
}

std::unique_ptr<AudioOutput> CreateAudioOutput(const AudioDeviceInfo& /*device*/) {
  return std::make_unique<WasapiOutput>();
}

}  // namespace cap
