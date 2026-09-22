#pragma once

// What a frame from the capture device looks like: size, rate, pixel layout and
// whatever the device said about its colour. The capture backend fills this in
// from its own description of the stream; everything past it -- renderer,
// overlay, recorder -- reads only this.

#include <cstddef>
#include <string>

namespace cap {

// How the bytes of an uncompressed frame are arranged. Only the layouts the
// renderer can take have a name here; anything else -- compressed formats, the
// 10 and 16 bit packed ones -- is Other, and the capture backend inserts a
// decoder or picks something else before a frame of it gets this far.
//
// The names say what lies in memory, in order. An RGB frame that stores blue
// first is Bgr24, whatever the platform happens to call it.
enum class PixelLayout {
  Other,
  Yuyv,    // packed 4:2:2, Y0 U Y1 V
  Uyvy,    // packed 4:2:2, U Y0 V Y1
  Yvyu,    // packed 4:2:2, Y0 V Y1 U
  Nv12,    // 4:2:0, a luma plane, then one plane of U and V taking turns
  P010,    // as Nv12 with 16 bit samples, ten bits used at the top of each
  P016,    // as Nv12 with 16 bit samples, all of them used
  I420,    // 4:2:0, three planes: Y, then U, then V
  Yv12,    // 4:2:0, three planes: Y, then V, then U
  Bgr24,   // B G R, three bytes per pixel
  Bgrx32,  // B G R and a byte of padding
  Bgra32,  // B G R A
};

// The colour description a device can attach to a stream. Most capture cards
// leave it out, but when it is there it beats guessing the range and matrix from
// the picture height -- and it is where an HDR source announces PQ or HLG.
//
// Each list holds what a device has been seen to announce and qBlank can name;
// anything else a backend reads is Unknown.
struct ColorInfo {
  enum class Range { Unknown, Full, Limited, From48To208, From64To127 };
  enum class Matrix { Unknown, BT709, BT601, SMPTE240M, BT2020_10, BT2020_12 };
  enum class Primaries { Unknown, BT709, BT470M, BT470BG, SMPTE170M, SMPTE240M, BT2020, DCIP3 };
  enum class Transfer {
    Unknown, Linear, Gamma22, BT709, SRGB, BT2020Constant, BT2020, PQ, HLG
  };

  bool present = false;  // the device said anything about its colour at all
  Range range = Range::Unknown;
  Matrix matrix = Matrix::Unknown;
  Primaries primaries = Primaries::Unknown;
  Transfer transfer = Transfer::Unknown;

  bool isHdr() const { return transfer == Transfer::PQ || transfer == Transfer::HLG; }
};

// Human readable names for the values above, for the diagnostics.
const char* RangeName(ColorInfo::Range value);
const char* MatrixName(ColorInfo::Matrix value);
const char* PrimariesName(ColorInfo::Primaries value);
const char* TransferName(ColorInfo::Transfer value);

struct VideoFormatInfo {
  PixelLayout layout = PixelLayout::Other;
  std::string subtypeLabel;  // "YUY2", "RGB24", ... -- what the settings store and show
  int width = 0;
  int height = 0;       // always positive
  int stride = 0;       // bytes per row of the top plane
  bool bottomUp = false;  // rows stored bottom first, as RGB bitmaps on Windows are
  bool interlaced = false;
  bool fieldOneFirst = true;
  double fps = 0.0;
  int aspectX = 0;  // picture aspect ratio, 0 when the device does not say
  int aspectY = 0;
  size_t imageSize = 0;

  ColorInfo color;

  // The matrix the pixel format stands for by its very name, apart from any
  // colour description. HDYC is UYVY that carries BT.709 by definition; nearly
  // every other format leaves this Unknown.
  ColorInfo::Matrix formatMatrix = ColorInfo::Matrix::Unknown;

  bool valid() const { return width > 0 && height > 0 && imageSize > 0; }
};

}  // namespace cap
