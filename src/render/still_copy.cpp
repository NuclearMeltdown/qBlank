#include "render/still_copy.h"

#include <cstring>

namespace cap {

void StillPixels::Read() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (read_.load(std::memory_order_relaxed)) return;

  const size_t w = (size_t)width;
  const size_t h = (size_t)height;
  if (layout == StillLayout::Half4) {
    half_.resize(w * 4 * h);
    for (size_t y = 0; y < h; ++y) {
      std::memcpy(half_.data() + y * w * 4, mapped_ + y * rowPitch_, w * 8);
    }
  } else {
    // The pipeline carries an alpha channel it has no use for. Whatever ended
    // up in it, a screenshot of opaque video is opaque.
    rgba_.resize(w * 4 * h);
    const bool swap = layout == StillLayout::Bgra8;
    for (size_t y = 0; y < h; ++y) {
      const uint8_t* s = mapped_ + y * rowPitch_;
      uint8_t* d = rgba_.data() + y * w * 4;
      if (!swap) {
        std::memcpy(d, s, w * 4);
        for (size_t x = 3; x < w * 4; x += 4) d[x] = 0xFF;
        continue;
      }
      for (size_t x = 0; x < w * 4; x += 4) {
        d[x + 0] = s[x + 2];
        d[x + 1] = s[x + 1];
        d[x + 2] = s[x + 0];
        d[x + 3] = 0xFF;
      }
    }
  }
  read_.store(true, std::memory_order_release);
}

}  // namespace cap
