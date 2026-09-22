#pragma once

// Sample format description and conversion to and from the interleaved stereo
// float that everything between capture and playback works in.

#include <cstddef>
#include <cstdint>

namespace cap {

struct StreamFormat {
  int sampleRate = 0;
  int channels = 0;
  int bitsPerSample = 0;
  int validBits = 0;
  bool isFloat = false;
  int blockAlign = 0;

  bool valid() const { return sampleRate > 0 && channels > 0 && blockAlign > 0; }
};

// Any channel layout and bit depth -> interleaved stereo float.
void ToStereoFloat(const uint8_t* src, size_t frames, const StreamFormat& fmt, float* dst);

// Interleaved stereo float -> the endpoint's format, applying gain.
void FromStereoFloat(const float* src, size_t frames, const StreamFormat& fmt, uint8_t* dst,
                     float gain);

}  // namespace cap
