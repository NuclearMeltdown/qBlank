#pragma once

// Audio device enumeration, plus the logic that works out which audio input
// belongs to a given capture card ("embedded audio").
//
// A platform can reach recording devices more than one way. On Windows most sit
// behind WASAPI like any microphone, but some capture cards never publish their
// embedded audio as a sound device at all and expose it only as a DirectShow
// audio input. Both kinds are listed here; which way leads to a device is the
// backend's business, carried in `backend` without anyone else reading it.

#include <string>
#include <vector>

#include "capture/capture_device.h"
#include "config.h"

namespace cap {

struct AudioDeviceInfo {
  std::string name;     // friendly name shown in the UI
  std::string id;       // stable across restarts; what it means is the backend's business
  std::string backend;  // opaque, read only by the platform half; stored in the DeviceRef
  std::string via;      // how it is reached, for the log and the overlay: "WASAPI", ...
  // Reachable only as a capture card's audio, not as a sound device. Such an
  // input is never a microphone, and the device list marks it with `via`.
  bool cardAudio = false;
  bool isDefault = false;
  // Where it sits in the machine, for pairing it with a video device. Empty
  // when the platform does not say.
  HardwarePath hardware;

  DeviceRef ToRef() const { return DeviceRef{name, id, backend}; }
};

// `capture` selects recording devices, otherwise playback devices. The system
// default comes first, the rest alphabetically.
std::vector<AudioDeviceInfo> EnumerateAudioDevices(bool capture);

// The devices as the platform lists them, in any order. Defined by the platform
// half; EnumerateAudioDevices sorts them.
std::vector<AudioDeviceInfo> ListAudioDevices(bool capture);

// Resolves a saved reference to a live device: exact id first, then name.
bool ResolveAudioDevice(const DeviceRef& ref, bool capture, AudioDeviceInfo* out);

// Picks the recording device that sits on the same piece of hardware as the
// given video capture device. The same hardware path is the strongest signal
// there is; failing that, the same bus and hardware id, then the same vendor
// and device, and the friendly names on top. Returns false when nothing is
// convincing enough.
bool FindEmbeddedAudioDevice(const VideoDeviceInfo& video, AudioDeviceInfo* out);

}  // namespace cap
