#include "audio/audio_stream.h"

#include "audio/audio_win32.h"

#include <audioclient.h>
#include <audioclientactivationparams.h>
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

const InputWording kLoopbackWording = {
    40 * kMsToRefTime,
    [] {
      return CAP_SAID(T("Ausgabegerät konnte nicht geöffnet werden.",
                        "Could not open the output device."));
    },
    [] {
      return CAP_SAID(T("Ausgabegerät konnte nicht aktiviert werden.",
                        "Could not activate the output device."));
    },
    [] {
      return CAP_SAID(T("Unbekanntes Format des Ausgabegeräts.", "Unknown output device format."));
    },
    [] {
      return CAP_SAID(T("Unbekanntes Format des Ausgabegeräts.", "Unknown output device format."));
    },
    [](HRESULT hr) {
      return CAP_SAID(T("Ausgabegerät konnte nicht mitgeschnitten werden: ",
                        "Could not capture the output device: ") + HrToString(hr));
    },
    [] {
      return CAP_SAID(T("IAudioCaptureClient nicht verfügbar.",
                        "IAudioCaptureClient is not available."));
    },
    [] {
      return CAP_SAID(T("Das Ausgabegerät wurde entfernt.", "The output device was removed."));
    },
    [] {
      return CAP_SAID(T("Ereignis konnte nicht erstellt werden.", "Could not create the event."));
    },
};

const InputWording kAllButSelfWording = {
    40 * kMsToRefTime,
    [] {
      return CAP_SAID(T("Systemton konnte nicht geöffnet werden.",
                        "Could not open the system sound."));
    },
    [] {
      return CAP_SAID(T("Systemton nicht verfügbar: braucht Windows 10 2004 oder neuer.",
                        "System sound not available: needs Windows 10 2004 or later."));
    },
    [] { return CAP_SAID(T("Unbekanntes Systemtonformat.", "Unknown system sound format.")); },
    [] { return CAP_SAID(T("Unbekanntes Systemtonformat.", "Unknown system sound format.")); },
    [](HRESULT hr) {
      return CAP_SAID(T("Systemton konnte nicht initialisiert werden: ",
                        "Could not initialise the system sound: ") + HrToString(hr));
    },
    [] {
      return CAP_SAID(T("IAudioCaptureClient nicht verfügbar.",
                        "IAudioCaptureClient is not available."));
    },
    [] { return CAP_SAID(T("Der Systemton ist abgebrochen.", "The system sound stopped.")); },
    [] {
      return CAP_SAID(T("Ereignis konnte nicht erstellt werden.", "Could not create the event."));
    },
};

const InputWording& WordingFor(AudioRole role) {
  switch (role) {
    case AudioRole::Microphone: return kMicrophoneWording;
    case AudioRole::Loopback: return kLoopbackWording;
    case AudioRole::AllButSelf: return kAllButSelfWording;
    case AudioRole::Passthrough: break;
  }
  return kPassthroughWording;
}

// An output nothing plays on hands its loopback no packets at all -- not
// silence, nothing (measured) -- and the recording would wait for them. A
// stream of our own on it keeps it running: filled with silence once, started,
// and never written again. Running dry plays silence, so it stays inaudible.
ComPtr<IAudioClient> StartSilentRender(IMMDevice* endpoint) {
  ComPtr<IAudioClient> client;
  if (FAILED(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client))) {
    return nullptr;
  }
  WAVEFORMATEX* mix = nullptr;
  if (FAILED(client->GetMixFormat(&mix)) || !mix) return nullptr;
  HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 100 * kMsToRefTime, 0, mix, nullptr);
  ::CoTaskMemFree(mix);
  ComPtr<IAudioRenderClient> render;
  UINT32 frames = 0;
  if (SUCCEEDED(hr)) hr = client->GetBufferSize(&frames);
  if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&render));
  BYTE* data = nullptr;
  if (SUCCEEDED(hr)) hr = render->GetBuffer(frames, &data);
  if (SUCCEEDED(hr)) hr = render->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
  if (SUCCEEDED(hr)) hr = client->Start();
  if (FAILED(hr)) {
    CAP_WARN("Loopback: no silent stream to keep the output running (%s)", HrToEnglish(hr).c_str());
    return nullptr;
  }
  return client;
}

// ActivateAudioInterfaceAsync answers on a thread of its own; this hands the
// client over. Agile, as the call requires. Reference counted, because the
// answer can come after the waiting gave up.
class ActivationWait : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
 public:
  ActivationWait() : done_(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

  HANDLE done() const { return done_; }
  HRESULT result() const { return result_; }
  ComPtr<IAudioClient> client() const { return client_; }

  STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
      *out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
    } else if (riid == __uuidof(IAgileObject)) {
      *out = static_cast<IAgileObject*>(this);
    } else {
      *out = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)::InterlockedIncrement(&refs_); }
  STDMETHODIMP_(ULONG) Release() override {
    const LONG left = ::InterlockedDecrement(&refs_);
    if (left == 0) delete this;
    return (ULONG)left;
  }

  STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
    HRESULT activated = E_FAIL;
    ComPtr<IUnknown> unknown;
    result_ = operation->GetActivateResult(&activated, &unknown);
    if (SUCCEEDED(result_)) result_ = activated;
    if (SUCCEEDED(result_) && unknown) result_ = unknown.As(&client_);
    ::SetEvent(done_);
    return S_OK;
  }

 private:
  ~ActivationWait() {
    if (done_) ::CloseHandle(done_);
  }

  LONG refs_ = 1;
  HANDLE done_;
  HRESULT result_ = E_FAIL;
  ComPtr<IAudioClient> client_;
};

// A capture client for everything played anywhere except by this process and
// what it starts. Null when Windows cannot (before 10 2004), or `stop` fired.
ComPtr<IAudioClient> ActivateAllButSelf(HANDLE stop) {
  AUDIOCLIENT_ACTIVATION_PARAMS params = {};
  params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
  params.ProcessLoopbackParams.TargetProcessId = ::GetCurrentProcessId();
  params.ProcessLoopbackParams.ProcessLoopbackMode =
      PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
  PROPVARIANT blob = {};
  blob.vt = VT_BLOB;
  blob.blob.cbSize = sizeof(params);
  blob.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

  ActivationWait* wait = new ActivationWait;
  ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
  HRESULT hr = ::ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                             __uuidof(IAudioClient), &blob, wait, &operation);
  ComPtr<IAudioClient> client;
  bool stopped = false;
  if (SUCCEEDED(hr) && wait->done()) {
    HANDLE waits[2] = {stop, wait->done()};
    const DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, 5000);
    if (w == WAIT_OBJECT_0 + 1) {
      hr = wait->result();
      if (SUCCEEDED(hr)) client = wait->client();
    } else {
      stopped = w == WAIT_OBJECT_0;
      hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
  }
  if (!client && !stopped) CAP_WARN("System sound: activation failed (%s)", HrToEnglish(hr).c_str());
  wait->Release();
  return client;
}

class WasapiInput : public AudioInput {
 public:
  ~WasapiInput() override { Stop(); }

  bool Start(const AudioDeviceInfo& device, AudioRole role, AudioInputEvents events,
             std::string* error) override {
    Stop();
    const InputWording& wording = WordingFor(role);
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

    ComPtr<IAudioClient> client;
    ComPtr<IAudioClient> keeper;  // Loopback only, see StartSilentRender
    StreamFormat fmt;
    HRESULT hr = E_FAIL;
    dataEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (role == AudioRole::AllButSelf) {
      // No device and no mix format to ask for: the format is ours to choose,
      // and Windows converts to it.
      client = ActivateAllButSelf(stopEvent_);
      if (!client) return fail(wording->notActivated());
      WAVEFORMATEX wanted = {};
      wanted.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
      wanted.nChannels = 2;
      wanted.nSamplesPerSec = 48000;
      wanted.wBitsPerSample = 32;
      wanted.nBlockAlign = 8;
      wanted.nAvgBytesPerSec = 48000 * 8;
      ParseWaveFormat(&wanted, &fmt);
      hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                              AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                  AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                  AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                              wording->buffer, 0, &wanted, nullptr);
    } else {
      const bool loopback = role == AudioRole::Loopback;
      ComPtr<IMMDevice> endpoint = OpenAudioEndpoint(device, !loopback);
      if (!endpoint) return fail(wording->notOpened());

      if (FAILED(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                    (void**)&client))) {
        return fail(wording->notActivated());
      }

      WAVEFORMATEX* mixFormat = nullptr;
      if (FAILED(client->GetMixFormat(&mixFormat)) || !mixFormat) {
        if (mixFormat) ::CoTaskMemFree(mixFormat);
        return fail(wording->noFormat());
      }
      if (!ParseWaveFormat(mixFormat, &fmt)) {
        ::CoTaskMemFree(mixFormat);
        return fail(wording->unknownFormat());
      }
      hr = client->Initialize(
          AUDCLNT_SHAREMODE_SHARED,
          AUDCLNT_STREAMFLAGS_EVENTCALLBACK | (loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0),
          wording->buffer, 0, mixFormat, nullptr);
      ::CoTaskMemFree(mixFormat);
      if (SUCCEEDED(hr) && loopback) keeper = StartSilentRender(endpoint.Get());
    }
    if (FAILED(hr)) return fail(wording->notInitialised(hr));
    client->SetEventHandle(dataEvent);

    ComPtr<IAudioCaptureClient> capture;
    if (FAILED(client->GetService(IID_PPV_ARGS(&capture)))) return fail(wording->noCaptureClient());

    if (events_.onFormat) events_.onFormat(fmt.sampleRate, fmt.channels);

    client->Start();
    if (role == AudioRole::Passthrough) {
      CAP_LOG("WASAPI capture: %d Hz, %d channels, %d bit%s", fmt.sampleRate, fmt.channels,
              fmt.bitsPerSample, fmt.isFloat ? " float" : "");
    } else {
      CAP_LOG("Recording source running: '%s' (%s), %d Hz, %d channels", device.name.c_str(),
              role == AudioRole::Microphone ? "input"
              : role == AudioRole::Loopback ? "output"
                                            : "all but qBlank",
              fmt.sampleRate, fmt.channels);
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
    if (keeper) keeper->Stop();
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
    bool clientInitialised = false;  // tried exclusive, successfully or not
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
        clientInitialised = true;
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
      // A client goes through Initialize once. After a refused exclusive try
      // it has to be activated afresh before shared mode can have it.
      if (clientInitialised) client.Reset();
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
