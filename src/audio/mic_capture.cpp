#include "audio/mic_capture.h"

#include <audioclient.h>
#include <avrt.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "audio/pcm.h"
#include "i18n.h"

namespace cap {
namespace {

const REFERENCE_TIME kMsToRefTime = 10000;

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
    // Nothing chosen means the system default, which OpenAudioEndpoint gives us
    // when the id is empty. Resolving would fail here, since there is nothing to
    // resolve.
    info.name = T("Systemstandard", "System default");
  } else if (!ResolveAudioDevice(device, true, &info)) {
    const Said said = CAP_SAID(T("Mikrofon nicht gefunden.", "Microphone not found."));
    if (error) *error = said.shown;
    Fail(said);
    return false;
  }
  // A DirectShow-only input is a capture card's embedded audio, not a
  // microphone; it belongs on the main path, not here.
  if (info.directShow) {
    const Said said =
        CAP_SAID(T("Dieses Gerät ist ein DirectShow-Eingang und kommt hier nicht in Frage.",
                   "That device is a DirectShow input and does not belong here."));
    if (error) *error = said.shown;
    Fail(said);
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    deviceName_ = info.name;
    lastError_.clear();
  }

  stopEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  readyEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!stopEvent_ || !readyEvent_) {
    ReportError(error,
                CAP_SAID(T("Ereignis konnte nicht erstellt werden.", "Could not create the event.")));
    Stop();
    return false;
  }

  // Four seconds. The recorder drains this continuously; the headroom is for
  // the moment ffmpeg takes to open its end of the pipe.
  ring_.Reset(4 * 48000);
  startFailed_.store(false, std::memory_order_relaxed);
  running_.store(true, std::memory_order_relaxed);
  thread_ = std::thread(&MicCapture::CaptureThread, this, info);

  // Opening a WASAPI endpoint takes a few milliseconds, and until it is done
  // there is no sample rate to report. Returning early would let a recording
  // start without the microphone track and never notice.
  if (::WaitForSingleObject(readyEvent_, 3000) != WAIT_OBJECT_0 ||
      startFailed_.load(std::memory_order_relaxed)) {
    // What the thread reported is in the log already; only silence is new.
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
  if (stopEvent_) ::SetEvent(stopEvent_);
  if (thread_.joinable()) thread_.join();
  if (stopEvent_) {
    ::CloseHandle(stopEvent_);
    stopEvent_ = nullptr;
  }
  if (readyEvent_) {
    ::CloseHandle(readyEvent_);
    readyEvent_ = nullptr;
  }
  sampleRate_.store(0, std::memory_order_relaxed);
  peak_.store(0.0f, std::memory_order_relaxed);
}

void MicCapture::CaptureThread(AudioDeviceInfo device) {
  ComScope com(COINIT_MULTITHREADED);
  HANDLE mmcss = JoinProAudio();
  HANDLE dataEvent = nullptr;

  // Whatever happens, Start() must stop waiting. Failing to signal here would
  // cost three seconds of frozen interface on every attempt.
  auto cleanup = [&]() {
    if (dataEvent) ::CloseHandle(dataEvent);
    if (mmcss) ::AvRevertMmThreadCharacteristics(mmcss);
    running_.store(false, std::memory_order_relaxed);
    startFailed_.store(true, std::memory_order_relaxed);
    if (readyEvent_) ::SetEvent(readyEvent_);
  };

  ComPtr<IMMDevice> endpoint = OpenAudioEndpoint(device, true);
  if (!endpoint) {
    Fail(CAP_SAID(T("Mikrofon konnte nicht geöffnet werden.", "Could not open the microphone.")));
    cleanup();
    return;
  }

  ComPtr<IAudioClient> client;
  if (FAILED(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client))) {
    Fail(CAP_SAID(T("Mikrofon konnte nicht aktiviert werden.", "Could not activate the microphone.")));
    cleanup();
    return;
  }

  WAVEFORMATEX* mixFormat = nullptr;
  StreamFormat fmt;
  if (FAILED(client->GetMixFormat(&mixFormat)) || !mixFormat ||
      !ParseWaveFormat(mixFormat, &fmt)) {
    if (mixFormat) ::CoTaskMemFree(mixFormat);
    Fail(CAP_SAID(T("Unbekanntes Mikrofonformat.", "Unknown microphone format.")));
    cleanup();
    return;
  }

  dataEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
  HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                  40 * kMsToRefTime, 0, mixFormat, nullptr);
  ::CoTaskMemFree(mixFormat);
  if (FAILED(hr)) {
    Fail(CAP_SAID(T("Mikrofon konnte nicht initialisiert werden: ",
                    "Could not initialise the microphone: ") + HrToString(hr)));
    cleanup();
    return;
  }
  client->SetEventHandle(dataEvent);

  ComPtr<IAudioCaptureClient> capture;
  if (FAILED(client->GetService(IID_PPV_ARGS(&capture)))) {
    Fail(CAP_SAID(T("IAudioCaptureClient nicht verfügbar.", "IAudioCaptureClient is not available.")));
    cleanup();
    return;
  }

  ring_.Reset((size_t)fmt.sampleRate * 4);
  sampleRate_.store(fmt.sampleRate, std::memory_order_relaxed);
  client->Start();
  if (readyEvent_) ::SetEvent(readyEvent_);  // Start() may return now
  CAP_LOG("Microphone running: '%s', %d Hz, %d channels", device.name.c_str(), fmt.sampleRate,
          fmt.channels);

  std::vector<float> scratch;
  HANDLE waits[2] = {stopEvent_, dataEvent};

  while (running_.load(std::memory_order_relaxed)) {
    const DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, 200);
    if (w == WAIT_OBJECT_0) break;
    if (w == WAIT_TIMEOUT) {
      // Silence still has to decay, or the bar would hang at the last peak.
      peak_.store(peak_.load(std::memory_order_relaxed) * kPeakDecay, std::memory_order_relaxed);
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

        // Gain first, then measure: the meter should show what goes into the
        // file, not what the device handed over.
        const float gain = gain_.load(std::memory_order_relaxed);
        float blockPeak = 0.0f;
        for (float& sample : scratch) {
          sample *= gain;
          const float magnitude = std::abs(sample);
          if (magnitude > blockPeak) blockPeak = magnitude;
        }
        if (blockPeak >= 1.0f) clipped_.store(true, std::memory_order_relaxed);

        const float decayed = peak_.load(std::memory_order_relaxed) * kPeakDecay;
        peak_.store(std::max(blockPeak, decayed), std::memory_order_relaxed);

        ring_.Write(scratch.data(), frames);
      }
      capture->ReleaseBuffer(frames);
    }

    if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
      Fail(CAP_SAID(T("Das Mikrofon wurde entfernt.", "The microphone was removed.")));
      break;
    }
  }

  client->Stop();
  if (dataEvent) ::CloseHandle(dataEvent);
  if (mmcss) ::AvRevertMmThreadCharacteristics(mmcss);
  running_.store(false, std::memory_order_relaxed);
}

}  // namespace cap
