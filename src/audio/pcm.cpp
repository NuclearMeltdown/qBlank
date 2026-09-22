#include "audio/pcm.h"

#include <cmath>
#include <cstring>

#include "common.h"

namespace cap {
namespace {

float ReadSample(const uint8_t* p, const StreamFormat& fmt) {
  if (fmt.isFloat) {
    if (fmt.bitsPerSample == 32) {
      float v;
      memcpy(&v, p, 4);
      return v;
    }
    if (fmt.bitsPerSample == 64) {
      double v;
      memcpy(&v, p, 8);
      return (float)v;
    }
    return 0.0f;
  }
  switch (fmt.bitsPerSample) {
    case 8:
      // 8 bit PCM is unsigned.
      return ((float)p[0] - 128.0f) / 128.0f;
    case 16: {
      int16_t v;
      memcpy(&v, p, 2);
      return (float)v / 32768.0f;
    }
    case 24: {
      // Sign extend by loading into the top three bytes and shifting back down.
      int32_t v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24);
      return (float)(v >> 8) / 8388608.0f;
    }
    case 32: {
      int32_t v;
      memcpy(&v, p, 4);
      return (float)v / 2147483648.0f;
    }
    default: return 0.0f;
  }
}

void WriteSample(uint8_t* p, const StreamFormat& fmt, float v) {
  v = Clamp(v, -1.0f, 1.0f);
  if (fmt.isFloat) {
    if (fmt.bitsPerSample == 32) {
      memcpy(p, &v, 4);
    } else if (fmt.bitsPerSample == 64) {
      double d = v;
      memcpy(p, &d, 8);
    }
    return;
  }
  switch (fmt.bitsPerSample) {
    case 8:
      p[0] = (uint8_t)Clamp((int)std::lround(v * 127.0f) + 128, 0, 255);
      break;
    case 16: {
      int16_t s = (int16_t)std::lround(v * 32767.0f);
      memcpy(p, &s, 2);
      break;
    }
    case 24: {
      int32_t s = (int32_t)std::lround(v * 8388607.0f);
      p[0] = (uint8_t)(s & 0xFF);
      p[1] = (uint8_t)((s >> 8) & 0xFF);
      p[2] = (uint8_t)((s >> 16) & 0xFF);
      break;
    }
    case 32: {
      int32_t s = (int32_t)std::lround((double)v * 2147483647.0);
      memcpy(p, &s, 4);
      break;
    }
    default: break;
  }
}

}  // namespace

void ToStereoFloat(const uint8_t* src, size_t frames, const StreamFormat& fmt, float* dst) {
  const int bytes = fmt.bitsPerSample / 8;
  for (size_t f = 0; f < frames; ++f) {
    const uint8_t* base = src + f * (size_t)fmt.blockAlign;
    if (fmt.channels == 1) {
      const float v = ReadSample(base, fmt);
      dst[f * 2 + 0] = v;
      dst[f * 2 + 1] = v;
    } else {
      // Anything above stereo: take the front pair, which is where capture
      // cards put the embedded audio.
      dst[f * 2 + 0] = ReadSample(base, fmt);
      dst[f * 2 + 1] = ReadSample(base + bytes, fmt);
    }
  }
}

void FromStereoFloat(const float* src, size_t frames, const StreamFormat& fmt, uint8_t* dst,
                     float gain) {
  const int bytes = fmt.bitsPerSample / 8;
  for (size_t f = 0; f < frames; ++f) {
    uint8_t* base = dst + f * (size_t)fmt.blockAlign;
    const float l = src[f * 2 + 0] * gain;
    const float r = src[f * 2 + 1] * gain;
    if (fmt.channels == 1) {
      WriteSample(base, fmt, (l + r) * 0.5f);
    } else {
      WriteSample(base, fmt, l);
      WriteSample(base + bytes, fmt, r);
      for (int c = 2; c < fmt.channels; ++c) {
        WriteSample(base + (size_t)c * bytes, fmt, 0.0f);
      }
    }
  }
}

}  // namespace cap
