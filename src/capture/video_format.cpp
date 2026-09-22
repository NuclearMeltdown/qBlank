#include "capture/video_format.h"

namespace cap {

const char* RangeName(ColorInfo::Range value) {
  switch (value) {
    case ColorInfo::Range::Full: return "Full (0-255)";
    case ColorInfo::Range::Limited: return "Limited (16-235)";
    case ColorInfo::Range::From48To208: return "48-208";
    case ColorInfo::Range::From64To127: return "64-127";
    default: return "unbekannt";
  }
}

const char* MatrixName(ColorInfo::Matrix value) {
  switch (value) {
    case ColorInfo::Matrix::BT709: return "BT.709";
    case ColorInfo::Matrix::BT601: return "BT.601";
    case ColorInfo::Matrix::SMPTE240M: return "SMPTE 240M";
    case ColorInfo::Matrix::BT2020_10: return "BT.2020 (10 Bit)";
    case ColorInfo::Matrix::BT2020_12: return "BT.2020 (12 Bit)";
    default: return "unbekannt";
  }
}

const char* PrimariesName(ColorInfo::Primaries value) {
  switch (value) {
    case ColorInfo::Primaries::BT709: return "BT.709";
    case ColorInfo::Primaries::BT470M: return "BT.470-2 System M";
    case ColorInfo::Primaries::BT470BG: return "BT.470-2 System B/G";
    case ColorInfo::Primaries::SMPTE170M: return "SMPTE 170M";
    case ColorInfo::Primaries::SMPTE240M: return "SMPTE 240M";
    case ColorInfo::Primaries::BT2020: return "BT.2020";
    case ColorInfo::Primaries::DCIP3: return "DCI-P3";
    default: return "unbekannt";
  }
}

const char* TransferName(ColorInfo::Transfer value) {
  switch (value) {
    case ColorInfo::Transfer::Linear: return "linear";
    case ColorInfo::Transfer::Gamma22: return "Gamma 2.2";
    case ColorInfo::Transfer::BT709: return "BT.709";
    case ColorInfo::Transfer::SRGB: return "sRGB";
    case ColorInfo::Transfer::BT2020Constant: return "BT.2020 (konstante Luminanz)";
    case ColorInfo::Transfer::BT2020: return "BT.2020";
    case ColorInfo::Transfer::PQ: return "PQ / ST.2084 (HDR10)";
    case ColorInfo::Transfer::HLG: return "HLG (HDR)";
    default: return "unbekannt";
  }
}

}  // namespace cap
