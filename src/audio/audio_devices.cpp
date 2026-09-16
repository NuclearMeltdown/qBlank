#include "audio/audio_devices.h"

// avrt.h has no includes of its own and needs windows.h and SAL first.
#include <avrt.h>

#include <mmdeviceapi.h>
// Must come after mmdeviceapi.h: it needs the PROPERTYKEY macros pulled in there.
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>

#include "i18n.h"

namespace cap {
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

// Splits an instance id on backslashes.
std::vector<std::string> SplitInstance(const std::string& id) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= id.size()) {
    size_t sep = id.find('\\', start);
    if (sep == std::string::npos) {
      out.push_back(id.substr(start));
      break;
    }
    out.push_back(id.substr(start, sep - start));
    start = sep + 1;
  }
  return out;
}

// Pulls VEN_xxxx&DEV_xxxx out of an instance component, ignoring the rest.
std::string VendorDeviceKey(const std::string& component) {
  std::string up = ToUpper(component);
  size_t ven = up.find("VEN_");
  size_t dev = up.find("DEV_");
  if (ven == std::string::npos || dev == std::string::npos) return {};
  auto take = [&](size_t pos) {
    size_t end = up.find('&', pos);
    return up.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
  };
  return take(ven) + "&" + take(dev);
}

// Words worth comparing between two friendly names.
std::vector<std::string> NameTokens(const std::string& name) {
  static const char* kNoise[] = {"AUDIO", "VIDEO",  "CAPTURE", "DEVICE", "GERAET", "DIGITAL",
                                 "INPUT", "SOURCE", "LINE",    "WAVEIN", "THE",    "AND",
                                 "PCIE",  "PCI",    "USB"};
  std::vector<std::string> out;
  std::string cur;
  for (char c : ToUpper(name)) {
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
      cur += c;
    } else {
      if (cur.size() >= 3) out.push_back(cur);
      cur.clear();
    }
  }
  if (cur.size() >= 3) out.push_back(cur);

  out.erase(std::remove_if(out.begin(), out.end(),
                           [&](const std::string& t) {
                             for (const char* n : kNoise) {
                               if (t == n) return true;
                             }
                             return false;
                           }),
            out.end());
  return out;
}

int NameOverlapScore(const std::string& a, const std::string& b) {
  std::vector<std::string> ta = NameTokens(a);
  std::vector<std::string> tb = NameTokens(b);
  if (ta.empty() || tb.empty()) return 0;
  int hits = 0;
  for (const std::string& t : ta) {
    if (std::find(tb.begin(), tb.end(), t) != tb.end()) ++hits;
  }
  if (hits == 0) return 0;
  // Up to 60 points, weighted by how much of the shorter name matched. Three of
  // four words in common is enough on its own; a single shared word is not.
  const double ratio = (double)hits / (double)std::min(ta.size(), tb.size());
  return (int)(ratio * 60.0);
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
    LPWSTR id = nullptr;
    if (SUCCEEDED(device->GetId(&id)) && id) {
      info.id = ToUtf8(id);
      ::CoTaskMemFree(id);
    }

    ComPtr<IPropertyStore> store;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store))) {
      info.name = ReadStringProperty(store.Get(), PKEY_Device_FriendlyName);
      // Not every driver fills this in; the matcher copes with an empty value.
      info.instanceId = ToUpper(ReadStringProperty(store.Get(), PKEY_Device_InstanceId));
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
    info.directShow = true;
    devices.push_back(std::move(info));
  }
  return devices;
}

}  // namespace

std::string NormalizeDevicePath(const std::string& devicePath) {
  if (devicePath.empty()) return {};
  std::string s = devicePath;
  if (s.rfind("\\\\?\\", 0) == 0) s = s.substr(4);
  if (s.rfind("\\\\.\\", 0) == 0) s = s.substr(4);

  // Everything up to the interface class GUID identifies the hardware; what
  // follows only says which interface of it we are looking at.
  std::vector<std::string> parts;
  size_t start = 0;
  while (start <= s.size()) {
    size_t hash = s.find('#', start);
    if (hash == std::string::npos) {
      parts.push_back(s.substr(start));
      break;
    }
    parts.push_back(s.substr(start, hash - start));
    start = hash + 1;
  }
  while (!parts.empty() && (parts.back().empty() || parts.back()[0] == '{')) parts.pop_back();
  if (parts.empty()) return {};

  std::string joined;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) joined += '\\';
    joined += parts[i];
  }
  return ToUpper(joined);
}

std::vector<AudioDeviceInfo> EnumerateAudioDevices(bool capture) {
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

  // Default endpoint first, then alphabetical.
  std::stable_sort(devices.begin(), devices.end(),
                   [](const AudioDeviceInfo& a, const AudioDeviceInfo& b) {
                     if (a.isDefault != b.isDefault) return a.isDefault;
                     return a.name < b.name;
                   });
  return devices;
}

bool ResolveAudioDevice(const DeviceRef& ref, bool capture, AudioDeviceInfo* out) {
  if (ref.empty()) return false;
  std::vector<AudioDeviceInfo> devices = EnumerateAudioDevices(capture);
  for (const AudioDeviceInfo& d : devices) {
    if (!ref.id.empty() && d.id == ref.id) {
      if (out) *out = d;
      return true;
    }
  }
  for (const AudioDeviceInfo& d : devices) {
    if (!ref.name.empty() && d.name == ref.name) {
      if (out) *out = d;
      return true;
    }
  }
  return false;
}

bool FindEmbeddedAudioDevice(const VideoDeviceInfo& video, AudioDeviceInfo* out) {
  std::vector<AudioDeviceInfo> devices = EnumerateAudioDevices(true);
  if (devices.empty()) return false;

  const std::string videoKey = NormalizeDevicePath(video.id);
  const std::vector<std::string> videoParts = SplitInstance(videoKey);
  const std::string videoVenDev =
      videoParts.size() > 1 ? VendorDeviceKey(videoParts[1]) : std::string();

  const AudioDeviceInfo* best = nullptr;
  int bestScore = 0;

  for (const AudioDeviceInfo& audio : devices) {
    int score = 0;

    // A DirectShow audio input carries the same kind of DevicePath as the video
    // device, so an equal hardware part means it is literally the same card.
    const std::string audioKey =
        audio.directShow ? NormalizeDevicePath(audio.id) : audio.instanceId;

    if (!videoKey.empty() && !audioKey.empty()) {
      if (audioKey == videoKey) {
        score += 100;
      } else {
        const std::vector<std::string> audioParts = SplitInstance(audioKey);
        // Same bus and same hardware id: the audio function of the same card.
        if (videoParts.size() >= 2 && audioParts.size() >= 2 && videoParts[0] == audioParts[0] &&
            videoParts[1] == audioParts[1]) {
          score += 80;
          if (videoParts.size() >= 3 && audioParts.size() >= 3 && videoParts[2] == audioParts[2]) {
            score += 15;
          }
        } else if (!videoVenDev.empty() && audioParts.size() >= 2 &&
                   VendorDeviceKey(audioParts[1]) == videoVenDev) {
          score += 60;
        }
      }
    }

    score += NameOverlapScore(video.name, audio.name);

    if (score > bestScore) {
      bestScore = score;
      best = &audio;
    }
  }

  // 45 is above what a single shared word can produce on its own, so a random
  // "USB Audio" does not get picked for an unrelated card.
  if (!best || bestScore < 45) {
    CAP_WARN("No embedded audio device found for '%s' (best score %d)",
             video.name.c_str(), bestScore);
    return false;
  }

  CAP_LOG("Embedded audio for '%s': '%s' (%s, score %d)", video.name.c_str(),
          best->name.c_str(), best->directShow ? "DirectShow" : "WASAPI", bestScore);
  if (out) *out = *best;
  return true;
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
