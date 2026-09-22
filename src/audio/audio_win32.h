#pragma once

// The Windows half of the audio code: what the WASAPI and DirectShow backends
// share among themselves.

// common_win32.h pulls in windows.h, which mmreg.h needs to have seen first.
#include "common_win32.h"

#include <mmdeviceapi.h>
#include <mmreg.h>

#include "audio/audio_devices.h"
#include "audio/pcm.h"

namespace cap {

// The `backend` of an AudioDeviceInfo, as the configuration stores it.
extern const char kWasapiBackend[];   // "wasapi"
extern const char kDShowBackend[];    // "dshow"

// Opens the endpoint a device names, falling back to the system default when
// its id is empty or no longer resolves.
ComPtr<IMMDevice> OpenAudioEndpoint(const AudioDeviceInfo& info, bool capture);

// Raises the calling thread to the "Pro Audio" MMCSS class so the scheduler
// stops treating it like ordinary work. The handle reverts it on exit.
HANDLE JoinProAudio();

// Handles WAVE_FORMAT_PCM, WAVE_FORMAT_IEEE_FLOAT and WAVE_FORMAT_EXTENSIBLE.
// Returns false for anything compressed.
bool ParseWaveFormat(const WAVEFORMATEX* wf, StreamFormat* out);

}  // namespace cap
