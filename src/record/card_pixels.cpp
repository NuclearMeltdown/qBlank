#include "record/card_pixels.h"

#include <cstring>

namespace cap {
namespace {

bool Packed(PixelLayout l) {
  return l == PixelLayout::Yuyv || l == PixelLayout::Uyvy || l == PixelLayout::Yvyu;
}
bool Rgb(PixelLayout l) {
  return l == PixelLayout::Bgr24 || l == PixelLayout::Bgrx32 || l == PixelLayout::Bgra32;
}

// Where Y0, Y1, U and V lie in each four bytes of packed 4:2:2.
struct Order {
  int y0, y1, u, v;
};
Order PackedOrder(PixelLayout l) {
  if (l == PixelLayout::Uyvy) return {1, 3, 0, 2};
  if (l == PixelLayout::Yvyu) return {0, 2, 3, 1};
  return {0, 2, 1, 3};
}

int BgrRow(int width) { return (width * 3 + 3) & ~3; }

void CopyRows(uint8_t* dst, size_t dstPitch, const uint8_t* src, size_t srcPitch, size_t bytes,
              int rows) {
  for (int y = 0; y < rows; ++y) {
    memcpy(dst + (size_t)y * dstPitch, src + (size_t)y * srcPitch, bytes);
  }
}

// The card's planes: Y (or the packed picture), then U and V; NV12's
// interleaved chroma is the second.
void CardPlanes(const VideoFormatInfo& card, const uint8_t* src, const uint8_t* plane[3],
                size_t pitch[3]) {
  const size_t stride = (size_t)CardStride(card);
  const size_t h = (size_t)card.height;
  plane[0] = src;
  pitch[0] = stride;
  switch (card.layout) {
    case PixelLayout::Nv12:
    case PixelLayout::P010:
    case PixelLayout::P016:
      plane[1] = src + stride * h;
      pitch[1] = stride;
      break;
    case PixelLayout::I420:
    case PixelLayout::Yv12: {
      // YV12 lays V before U.
      const size_t c = stride / 2;
      const uint8_t* first = src + stride * h;
      const uint8_t* second = first + c * ((h + 1) / 2);
      const bool swap = card.layout == PixelLayout::Yv12;
      plane[1] = swap ? second : first;
      plane[2] = swap ? first : second;
      pitch[1] = pitch[2] = c;
      break;
    }
    default:
      break;
  }
}

void OutPlanes(CardOut out, int w, int h, uint8_t* base, uint8_t* data[4], int linesize[4]) {
  const size_t px = (size_t)w * (size_t)h;
  data[0] = base;
  switch (out) {
    case CardOut::Packed422:
      linesize[0] = w * 2;
      break;
    case CardOut::Bgr24:
      linesize[0] = BgrRow(w);
      break;
    case CardOut::Nv12:
      linesize[0] = linesize[1] = w;
      data[1] = base + px;
      break;
    case CardOut::P010:
      linesize[0] = linesize[1] = w * 2;
      data[1] = base + px * 2;
      break;
    case CardOut::Yuv420p:
    case CardOut::Yuv422p:
      linesize[0] = w;
      linesize[1] = linesize[2] = w / 2;
      data[1] = base + px;
      data[2] = data[1] + (size_t)(w / 2) * (size_t)(out == CardOut::Yuv420p ? h / 2 : h);
      break;
    case CardOut::Yuv444p:
    case CardOut::Gbrp:
      linesize[0] = linesize[1] = linesize[2] = w;
      data[1] = base + px;
      data[2] = base + px * 2;
      break;
  }
}

}  // namespace

bool SameCardFormat(const VideoFormatInfo& a, const VideoFormatInfo& b) {
  return a.layout == b.layout && a.width == b.width && a.height == b.height &&
         CardStride(a) == CardStride(b) && a.bottomUp == b.bottomUp;
}

int CardStride(const VideoFormatInfo& card) {
  if (card.stride > 0) return card.stride;
  switch (card.layout) {
    case PixelLayout::Yuyv:
    case PixelLayout::Uyvy:
    case PixelLayout::Yvyu:
    case PixelLayout::P010:
    case PixelLayout::P016:
      return card.width * 2;
    case PixelLayout::Bgr24:
      return BgrRow(card.width);
    case PixelLayout::Bgrx32:
    case PixelLayout::Bgra32:
      return card.width * 4;
    default:
      return card.width;
  }
}

size_t CardFrameBytes(const VideoFormatInfo& card) {
  const size_t stride = (size_t)CardStride(card);
  const size_t h = (size_t)card.height;
  switch (card.layout) {
    case PixelLayout::Nv12:
    case PixelLayout::P010:
    case PixelLayout::P016:
      return stride * h + stride * ((h + 1) / 2);
    case PixelLayout::I420:
    case PixelLayout::Yv12:
      return stride * h + 2 * (stride / 2) * ((h + 1) / 2);
    default:
      return stride * h;
  }
}

size_t CardOutBytes(CardOut out, int width, int height) {
  const size_t px = (size_t)width * (size_t)height;
  switch (out) {
    case CardOut::Packed422:
    case CardOut::Yuv422p:
      return px * 2;
    case CardOut::Nv12:
    case CardOut::Yuv420p:
      return px + 2 * (size_t)(width / 2) * (size_t)(height / 2);
    case CardOut::P010:
    case CardOut::Yuv444p:
    case CardOut::Gbrp:
      return px * 3;
    case CardOut::Bgr24:
      return (size_t)BgrRow(width) * (size_t)height;
  }
  return 0;
}

CardPlan PlanCard(RecordEncoder encoder, const VideoFormatInfo& card) {
  CardPlan p;
  const PixelLayout l = card.layout;
  const bool packed = Packed(l);
  const int w = card.width & ~1;
  const int h = card.height & ~1;
  const int stride = CardStride(card);
  // Already as the file keeps it, where nothing is converted at all.
  const bool even = w == card.width && h == card.height;
  auto set = [&](CardOut out, const char* name, CardRepack repack, double bytes) {
    p.pixelFormat = name;
    p.out = out;
    p.repack = repack;
    p.rgb = out == CardOut::Gbrp || out == CardOut::Bgr24;
    p.pixelBytes = bytes;
  };
  auto tight = [&](int row) { return even && stride == row ? CardRepack::None : CardRepack::Rows; };
  const char* packedName = l == PixelLayout::Uyvy   ? "uyvy422"
                           : l == PixelLayout::Yvyu ? "yvyu422"
                                                    : "yuyv422";
  switch (encoder) {
    // An AVI holds YUV rows tight and RGB rows on four bytes.
    case RecordEncoder::Uncompressed:
      if (packed) set(CardOut::Packed422, packedName, tight(w * 2), 2.0);
      else if (l == PixelLayout::Nv12) set(CardOut::Nv12, "nv12", tight(w), 1.5);
      else if (l == PixelLayout::I420) set(CardOut::Yuv420p, "yuv420p", tight(w), 1.5);
      else if (l == PixelLayout::Yv12) set(CardOut::Yuv420p, "yuv420p", CardRepack::Rows, 1.5);
      else if (l == PixelLayout::Bgr24) set(CardOut::Bgr24, "bgr24", tight(BgrRow(w)), 3.0);
      else if (Rgb(l)) set(CardOut::Bgr24, "bgr24", CardRepack::Bgr24, 3.0);
      break;
    case RecordEncoder::UtVideo:
      if (packed) set(CardOut::Yuv422p, "yuv422p", CardRepack::Planar422, 2.0);
      else if (l == PixelLayout::Nv12) set(CardOut::Yuv420p, "yuv420p", CardRepack::Planar420, 1.5);
      else if (l == PixelLayout::I420 || l == PixelLayout::Yv12) {
        set(CardOut::Yuv420p, "yuv420p", CardRepack::None, 1.5);
      } else if (Rgb(l)) set(CardOut::Gbrp, "gbrp", CardRepack::Gbr, 3.0);
      break;
    // No 4:2:2 here: packed goes in as 4:4:4, every chroma sample twice. P016
    // may use more than the ten bits Main 10 keeps.
    case RecordEncoder::NvencLossless:
      if (l == PixelLayout::Nv12) set(CardOut::Nv12, "nv12", CardRepack::None, 1.5);
      else if (l == PixelLayout::P010) set(CardOut::P010, "p010le", CardRepack::None, 3.0);
      else if (l == PixelLayout::I420 || l == PixelLayout::Yv12) {
        set(CardOut::Yuv420p, "yuv420p", CardRepack::None, 1.5);
      } else if (packed) set(CardOut::Yuv444p, "yuv444p", CardRepack::Full444, 3.0);
      else if (Rgb(l)) set(CardOut::Gbrp, "gbrp", CardRepack::Gbr, 3.0);
      break;
    default:
      break;
  }
  return p;
}

void RepackCard(const CardPlan& plan, const VideoFormatInfo& format, int w, int h,
                const uint8_t* card, uint8_t* out, uint8_t* data[4], int linesize[4]) {
  const uint8_t* in[3] = {};
  size_t pitch[3] = {};
  CardPlanes(format, card, in, pitch);
  if (plan.repack == CardRepack::None) {
    for (int i = 0; i < 3; ++i) {
      data[i] = const_cast<uint8_t*>(in[i]);
      linesize[i] = (int)pitch[i];
    }
    return;
  }
  OutPlanes(plan.out, w, h, out, data, linesize);

  switch (plan.repack) {
    case CardRepack::None:
      break;
    case CardRepack::Rows:
      switch (plan.out) {
        case CardOut::Packed422:
          CopyRows(data[0], (size_t)linesize[0], in[0], pitch[0], (size_t)w * 2, h);
          break;
        case CardOut::Bgr24:
          CopyRows(data[0], (size_t)linesize[0], in[0], pitch[0], (size_t)w * 3, h);
          // The padding as well, so a frame says the same every time.
          if (linesize[0] > w * 3) {
            for (int y = 0; y < h; ++y) {
              memset(data[0] + (size_t)y * (size_t)linesize[0] + (size_t)w * 3, 0,
                     (size_t)(linesize[0] - w * 3));
            }
          }
          break;
        case CardOut::Nv12:
          CopyRows(data[0], (size_t)w, in[0], pitch[0], (size_t)w, h);
          CopyRows(data[1], (size_t)w, in[1], pitch[1], (size_t)w, h / 2);
          break;
        default:  // yuv420p
          CopyRows(data[0], (size_t)w, in[0], pitch[0], (size_t)w, h);
          CopyRows(data[1], (size_t)(w / 2), in[1], pitch[1], (size_t)(w / 2), h / 2);
          CopyRows(data[2], (size_t)(w / 2), in[2], pitch[2], (size_t)(w / 2), h / 2);
          break;
      }
      break;
    case CardRepack::Planar422:
    case CardRepack::Full444: {
      const Order o = PackedOrder(format.layout);
      const bool full = plan.repack == CardRepack::Full444;
      for (int y = 0; y < h; ++y) {
        const uint8_t* s = in[0] + (size_t)y * pitch[0];
        uint8_t* Y = data[0] + (size_t)y * (size_t)linesize[0];
        uint8_t* U = data[1] + (size_t)y * (size_t)linesize[1];
        uint8_t* V = data[2] + (size_t)y * (size_t)linesize[2];
        for (int x = 0; x < w / 2; ++x, s += 4) {
          Y[2 * x] = s[o.y0];
          Y[2 * x + 1] = s[o.y1];
          if (full) {
            U[2 * x] = U[2 * x + 1] = s[o.u];
            V[2 * x] = V[2 * x + 1] = s[o.v];
          } else {
            U[x] = s[o.u];
            V[x] = s[o.v];
          }
        }
      }
      break;
    }
    case CardRepack::Planar420:
      CopyRows(data[0], (size_t)w, in[0], pitch[0], (size_t)w, h);
      for (int y = 0; y < h / 2; ++y) {
        const uint8_t* s = in[1] + (size_t)y * pitch[1];
        uint8_t* U = data[1] + (size_t)y * (size_t)linesize[1];
        uint8_t* V = data[2] + (size_t)y * (size_t)linesize[2];
        for (int x = 0; x < w / 2; ++x) {
          U[x] = s[2 * x];
          V[x] = s[2 * x + 1];
        }
      }
      break;
    case CardRepack::Gbr: {
      // ffmpeg's planar RGB starts with green, and its first row is the top.
      const int step = format.layout == PixelLayout::Bgr24 ? 3 : 4;
      for (int y = 0; y < h; ++y) {
        const int row = format.bottomUp ? format.height - 1 - y : y;
        const uint8_t* s = in[0] + (size_t)row * pitch[0];
        uint8_t* G = data[0] + (size_t)y * (size_t)linesize[0];
        uint8_t* B = data[1] + (size_t)y * (size_t)linesize[1];
        uint8_t* R = data[2] + (size_t)y * (size_t)linesize[2];
        for (int x = 0; x < w; ++x, s += step) {
          B[x] = s[0];
          G[x] = s[1];
          R[x] = s[2];
        }
      }
      break;
    }
    case CardRepack::Bgr24: {
      // The rows stay in the card's order; the file's header says which.
      const size_t pad = (size_t)(linesize[0] - w * 3);
      for (int y = 0; y < h; ++y) {
        const uint8_t* s = in[0] + (size_t)y * pitch[0];
        uint8_t* d = data[0] + (size_t)y * (size_t)linesize[0];
        for (int x = 0; x < w; ++x, s += 4, d += 3) {
          d[0] = s[0];
          d[1] = s[1];
          d[2] = s[2];
        }
        if (pad) memset(d, 0, pad);
      }
      break;
    }
  }
}

}  // namespace cap
