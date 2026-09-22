#include "audio/audio_win32.h"

// avrt.h has no includes of its own and needs windows.h and SAL first.
#include <avrt.h>

#include <mmdeviceapi.h>
// Must come after mmdeviceapi.h: it needs the PROPERTYKEY macros pulled in there.
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>

#include "capture/dshow_util.h"
#include "i18n.h"
#include "text_win32.h"

namespace cap {

// Recording devices come from two places. Most sit behind WASAPI like any
// microphone. Some capture cards, though -- the StarTech PEXHDCAP60L among
// them -- never publish their embedded audio as a Windows sound device at all
// and expose it only as a DirectShow audio input; those are listed too and
// are read through a DirectShow graph instead.
const char kWasapiBackend[] = "wasapi";
const char kDShowBackend[] = "dshow";

namespace {

std::string ReadStringProperty(IPropertyStore* store, const PROPERTYKEY& key) {
  if (!store) return {};
  PROPVARIANT var;
  ::PropVariantInit(&var);
  std::string out;
  if (SUCCEEDED(store->GetValue(key, &var)) && var.vt == VT_LPWSTR && var.pwszVal) {
    out = ToUtf8(var.pwszVal);
  }
  ::PropVariantClear(&var);
  return out;
}

std::vector<AudioDeviceInfo> EnumerateWasapi(bool capture) {
  std::vector<AudioDeviceInfo> devices;

  ComPtr<IMMDeviceEnumerator> enumerator;
  if (FAILED(CAP_HR(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       IID_PPV_ARGS(&enumerator))))) {
    return devices;
  }

  const EDataFlow flow = capture ? eCapture : eRender;

  std::string defaultId;
  {
    ComPtr<IMMDevice> defaultDevice;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &defaultDevice))) {
      LPWSTR id = nullptr;
      if (SUCCEEDED(defaultDevice->GetId(&id)) && id) {
        defaultId = ToUtf8(id);
        ::CoTaskMemFree(id);
      }
    }
  }

  ComPtr<IMMDeviceCollection> collection;
  if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection))) {
    return devices;
  }

  UINT count = 0;
  collection->GetCount(&count);
  for (UINT i = 0; i < count; ++i) {
    ComPtr<IMMDevice> device;
    if (FAILED(collection->Item(i, &device))) continue;

    AudioDeviceInfo info;
    info.backend = kWasapiBackend;
    info.via = "WASAPI";
    LPWSTR id = nullptr;
    if (SUCCEEDED(device->GetId(&id)) && id) {
      info.id = ToUtf8(id);
      ::CoTaskMemFree(id);
    }

    ComPtr<IPropertyStore> store;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store))) {
      info.name = ReadStringProperty(store.Get(), PKEY_Device_FriendlyName);
      // Not every driver fills this in; the matcher copes with an empty value.
      info.hardware = HardwareFromInstancePath(
          ToUpper(ReadStringProperty(store.Get(), PKEY_Device_InstanceId)));
    }
    if (info.name.empty()) info.name = T("Unbenanntes Audiogerät", "Unnamed audio device");
    info.isDefault = !defaultId.empty() && info.id == defaultId;
    devices.push_back(std::move(info));
  }
  return devices;
}

std::vector<AudioDeviceInfo> EnumerateDShowAudio() {
  std::vector<AudioDeviceInfo> devices;
  for (const VideoDeviceInfo& d : EnumerateAudioCaptureDShowDevices()) {
    AudioDeviceInfo info;
    info.name = d.name;
    info.id = d.id;
    info.backend = kDShowBackend;
    info.via = "DirectShow";
    info.cardAudio = true;
    // The id is a DevicePath of the same kind as the video device's, so an
    // equal hardware part means it is literally the same card.
    info.hardware = HardwareFromInstancePath(NormalizeDevicePath(d.id));
    devices.push_back(std::move(info));
  }
  return devices;
}

}  // namespace

std::vector<AudioDeviceInfo> ListAudioDevices(bool capture) {
  std::vector<AudioDeviceInfo> devices = EnumerateWasapi(capture);

  if (capture) {
    // Add DirectShow-only inputs. Anything already present as a sound device is
    // skipped: WASAPI is the better path when both exist.
    for (AudioDeviceInfo& ds : EnumerateDShowAudio()) {
      const bool duplicate = std::any_of(devices.begin(), devices.end(),
                                         [&](const AudioDeviceInfo& w) { return w.name == ds.name; });
      if (!duplicate) devices.push_back(std::move(ds));
    }
  }
  return devices;
}

ComPtr<IMMDevice> OpenAudioEndpoint(const AudioDeviceInfo& info, bool capture) {
  ComPtr<IMMDeviceEnumerator> enumerator;
  if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator)))) {
    return nullptr;
  }
  ComPtr<IMMDevice> device;
  if (!info.id.empty()) {
    if (SUCCEEDED(enumerator->GetDevice(ToWide(info.id).c_str(), &device))) return device;
  }
  if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(capture ? eCapture : eRender, eConsole,
                                                    &device))) {
    return device;
  }
  return nullptr;
}

HANDLE JoinProAudio() {
  DWORD taskIndex = 0;
  return ::AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
}

}  // namespace cap
