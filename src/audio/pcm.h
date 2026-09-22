#pragma once

// Sample format description and conversion to and from the interleaved stereo
// float that everything between capture and playback works in.

// common.h pulls in windows.h, which mmreg.h needs to have seen first.
#include "common_win32.h"

#include <mmreg.h>

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

// Handles WAVE_FORMAT_PCM, WAVE_FORMAT_IEEE_FLOAT and WAVE_FORMAT_EXTENSIBLE.
// Returns false for anything compressed.
bool ParseWaveFormat(const WAVEFORMATEX* wf, StreamFormat* out);

// Any channel layout and bit depth -> interleaved stereo float.
void ToStereoFloat(const uint8_t* src, size_t frames, const StreamFormat& fmt, float* dst);

// Interleaved stereo float -> the endpoint's format, applying gain.
void FromStereoFloat(const float* src, size_t frames, const StreamFormat& fmt, uint8_t* dst,
                     float gain);

}  // namespace cap
