#pragma once

// How a picture leaves the yuv pass (kYuvPS) for the encoder or the virtual
// camera: one image `width` samples wide, the luma rows first and the chroma
// rows after them. Which layout and depth is chosen per consumer, so that the
// bytes are what it takes and nobody converts them again on the CPU.

#include <cstdint>

namespace cap {

enum class YuvMatrix : uint8_t {
  Bt709,     // the recording
  Bt601,     // the camera: what a consumer assumes of NV12 when not told
  Bt2020Pq,  // either of them keeping the range: ten bits on the PQ curve
};

enum class YuvLayout : uint8_t {
  Nv12,  // 4:2:0, Cb and Cr side by side in half as many rows again
  I420,  // 4:2:0, a plane of Cb and then one of Cr
  I444,  // 4:4:4, a plane of Cb and then one of Cr, both full size
  // Not yuv at all: the picture as it is shown, eight bits each of B, G and R
  // side by side and the rows bottom-up, as an AVI keeps uncompressed RGB.
  Bgr,
};

struct YuvFormat {
  YuvMatrix matrix = YuvMatrix::Bt709;
  YuvLayout layout = YuvLayout::Nv12;
  // For the ten bits: at the top of sixteen, as P010 has them, or at the
  // bottom, as the software encoders want them.
  bool msb = true;

  bool pq() const { return matrix == YuvMatrix::Bt2020Pq; }
  int sampleBytes() const { return pq() ? 2 : 1; }
  bool operator==(const YuvFormat& o) const {
    return matrix == o.matrix && layout == o.layout && (!pq() || msb == o.msb);
  }
  bool operator!=(const YuvFormat& o) const { return !(*this == o); }
};

// The image's rows for `height` luma rows (even), and its samples across for
// `width` pixels. A BGR row ends on four bytes, as a bitmap's must and as
// ffmpeg writes one into an AVI.
inline int YuvRows(YuvLayout layout, int height) {
  if (layout == YuvLayout::Bgr) return height;
  return layout == YuvLayout::I444 ? height * 3 : height + height / 2;
}
inline int YuvColumns(YuvLayout layout, int width) {
  return layout == YuvLayout::Bgr ? (width * 3 + 3) & ~3 : width;
}

// ffmpeg's name for the same bytes.
inline const char* FfmpegPixelFormat(const YuvFormat& f) {
  switch (f.layout) {
    case YuvLayout::Nv12: return f.pq() ? "p010le" : "nv12";
    case YuvLayout::I420: return f.pq() ? (f.msb ? "p010le" : "yuv420p10le") : "yuv420p";
    case YuvLayout::I444: return f.pq() ? (f.msb ? "yuv444p16le" : "yuv444p10le") : "yuv444p";
    case YuvLayout::Bgr: return "bgr24";
  }
  return "nv12";
}

}  // namespace cap
