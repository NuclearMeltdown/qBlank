#include "audio/audio_win32.h"

#include <cstring>

namespace cap {

bool ParseWaveFormat(const WAVEFORMATEX* wf, StreamFormat* out) {
  if (!wf || !out) return false;
  out->sampleRate = (int)wf->nSamplesPerSec;
  out->channels = (int)wf->nChannels;
  out->bitsPerSample = (int)wf->wBitsPerSample;
  out->validBits = (int)wf->wBitsPerSample;
  out->blockAlign = (int)wf->nBlockAlign;

  if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
    out->isFloat = true;
  } else if (wf->wFormatTag == WAVE_FORMAT_PCM) {
    out->isFloat = false;
  } else if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
    const auto* ext = (const WAVEFORMATEXTENSIBLE*)wf;
    out->validBits = (int)ext->Samples.wValidBitsPerSample;
    if (out->validBits <= 0) out->validBits = out->bitsPerSample;
    // The KSDATAFORMAT_SUBTYPE_* GUIDs are a wave format tag wrapped in a fixed
    // GUID tail, so compare that directly and skip the ksmedia headers.
    static const uint8_t kWaveTail[8] = {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    const GUID& sub = ext->SubFormat;
    if (sub.Data2 != 0 || sub.Data3 != 0x0010 || memcmp(sub.Data4, kWaveTail, 8) != 0) {
      return false;
    }
    if (sub.Data1 == WAVE_FORMAT_IEEE_FLOAT) {
      out->isFloat = true;
    } else if (sub.Data1 == WAVE_FORMAT_PCM) {
      out->isFloat = false;
    } else {
      return false;
    }
  } else {
    return false;
  }
  return out->valid();
}

}  // namespace cap
