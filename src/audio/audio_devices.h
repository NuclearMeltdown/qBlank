#pragma once

// Audio device enumeration, plus the logic that works out which audio input
// belongs to a given capture card ("embedded audio").
//
// Recording devices come from two places. Most sit behind WASAPI like any
// microphone. Some capture cards, though, never publish their embedded audio as
// a Windows sound device at all and expose it only as a DirectShow audio input;
// those are listed here too and are read through a DirectShow graph instead.

#include <mmdeviceapi.h>

#include <string>
#include <vector>

#include "capture/dshow_util.h"
#include "common_win32.h"
#include "config.h"

namespace cap {

struct AudioDeviceInfo {
  std::string name;        // friendly name shown in the UI
  std::string id;          // WASAPI endpoint id, or the DirectShow DevicePath
  std::string instanceId;  // device instance path; WASAPI only, often empty
  bool directShow = false;
  bool isDefault = false;

  DeviceRef ToRef() const {
    return DeviceRef{name, id, directShow ? "dshow" : "wasapi"};
  }
};

// Opens the endpoint a reference names, falling back to the system default.
// Shared by the passthrough engine and the microphone capture.
ComPtr<IMMDevice> OpenAudioEndpoint(const AudioDeviceInfo& info, bool capture);

// Raises the calling thread to the "Pro Audio" MMCSS class so the scheduler
// stops treating it like ordinary work. The handle reverts it on exit.
HANDLE JoinProAudio();

// `capture` selects recording devices (WASAPI plus DirectShow-only ones),
// otherwise WASAPI playback endpoints.
std::vector<AudioDeviceInfo> EnumerateAudioDevices(bool capture);

// Resolves a saved reference to a live device: exact id first, then name.
bool ResolveAudioDevice(const DeviceRef& ref, bool capture, AudioDeviceInfo* out);

// Picks the recording device that sits on the same piece of hardware as the
// given video capture device. A DirectShow audio input whose DevicePath points
// at the same hardware is the strongest signal there is; failing that the WASAPI
// device instance path is compared, and failing that the friendly names.
// Returns false when nothing is convincing enough.
bool FindEmbeddedAudioDevice(const VideoDeviceInfo& video, AudioDeviceInfo* out);

// Strips a DirectShow DevicePath down to the hardware it names, so paths from
// different device interfaces on the same card compare equal.
//   \\?\pci#ven_1131&dev_7160&subsys_x&rev_y#6&846d6d9&0&000800e2#{iface}\{...}
//   ->  PCI\VEN_1131&DEV_7160&SUBSYS_X&REV_Y\6&846D6D9&0&000800E2
std::string NormalizeDevicePath(const std::string& devicePath);

}  // namespace cap
