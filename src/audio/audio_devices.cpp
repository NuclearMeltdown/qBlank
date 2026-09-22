#include "audio/audio_devices.h"

#include <algorithm>

#include "common.h"

namespace cap {
namespace {

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

}  // namespace

std::vector<AudioDeviceInfo> EnumerateAudioDevices(bool capture) {
  std::vector<AudioDeviceInfo> devices = ListAudioDevices(capture);

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

  const HardwarePath videoHardware = VideoDeviceHardware(video);
  const std::vector<std::string>& videoParts = videoHardware.parts;

  const AudioDeviceInfo* best = nullptr;
  int bestScore = 0;

  for (const AudioDeviceInfo& audio : devices) {
    int score = 0;

    // An input that names the same hardware path as the video device is
    // literally the same card.
    const std::vector<std::string>& audioParts = audio.hardware.parts;

    if (!videoParts.empty() && !audioParts.empty()) {
      if (audioParts == videoParts) {
        score += 100;
      } else {
        // Same bus and same hardware id: the audio function of the same card.
        if (videoParts.size() >= 2 && audioParts.size() >= 2 && videoParts[0] == audioParts[0] &&
            videoParts[1] == audioParts[1]) {
          score += 80;
          if (videoParts.size() >= 3 && audioParts.size() >= 3 && videoParts[2] == audioParts[2]) {
            score += 15;
          }
        } else if (!videoHardware.vendorDevice.empty() &&
                   audio.hardware.vendorDevice == videoHardware.vendorDevice) {
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
          best->name.c_str(), best->via.c_str(), bestScore);
  if (out) *out = *best;
  return true;
}

}  // namespace cap
