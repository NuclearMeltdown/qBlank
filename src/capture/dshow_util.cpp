#include "capture/dshow_util.h"

#include "i18n.h"
#include "text_win32.h"

#include <dvdmedia.h>  // VIDEOINFOHEADER2
#include <ks.h>        // KSPROPERTY_SUPPORT_GET / _SET

#include <algorithm>
#include <cmath>
#include <iterator>
#include <set>

namespace cap {
namespace {

constexpr uint32_t Fourcc(char a, char b, char c, char d) {
  return (uint32_t)(unsigned char)a | ((uint32_t)(unsigned char)b << 8) |
         ((uint32_t)(unsigned char)c << 16) | ((uint32_t)(unsigned char)d << 24);
}

// FOURCC based media subtypes all share the same GUID tail.
constexpr GUID FourccGuid(uint32_t fourcc) {
  return GUID{fourcc, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
}

// How the bytes of one frame are arranged, which is all that is needed to work
// out stride and total size.
enum class Layout {
  Packed,      // one row of bpp bits per pixel, no padding
  Planar420,   // full res luma plane, then half res chroma
  Rgb,         // like Packed, but rows padded to 4 bytes like a DIB
  Compressed,  // size comes from the sample
};

struct SubtypeInfo {
  const char* label;
  GUID guid;
  int bpp;          // 0 for compressed
  Layout layout;
  bool rendererOk;  // can the D3D renderer upload it directly
  PixelLayout pixels;  // what the rest of the program calls it
};

// Order matters only for the reverse lookup; the UI keeps driver order.
// The 10 and 16 bit entries are listed so that a card offering them gets a
// readable label and a correct frame size instead of a hex FOURCC -- the
// renderer cannot draw them yet, so selecting one falls back to a format it can.
// HDYC is laid out as UYVY -- it differs in the matrix, which ParseVideoMediaType
// passes on separately -- and I420 and IYUV are two names for the same layout.
const SubtypeInfo kSubtypes[] = {
    {"YUY2", FourccGuid(Fourcc('Y', 'U', 'Y', '2')), 16, Layout::Packed, true, PixelLayout::Yuyv},
    {"UYVY", FourccGuid(Fourcc('U', 'Y', 'V', 'Y')), 16, Layout::Packed, true, PixelLayout::Uyvy},
    {"YVYU", FourccGuid(Fourcc('Y', 'V', 'Y', 'U')), 16, Layout::Packed, true, PixelLayout::Yvyu},
    {"HDYC", FourccGuid(Fourcc('H', 'D', 'Y', 'C')), 16, Layout::Packed, true, PixelLayout::Uyvy},
    {"NV12", FourccGuid(Fourcc('N', 'V', '1', '2')), 12, Layout::Planar420, true, PixelLayout::Nv12},
    {"YV12", FourccGuid(Fourcc('Y', 'V', '1', '2')), 12, Layout::Planar420, true, PixelLayout::Yv12},
    {"I420", FourccGuid(Fourcc('I', '4', '2', '0')), 12, Layout::Planar420, true, PixelLayout::I420},
    {"IYUV", FourccGuid(Fourcc('I', 'Y', 'U', 'V')), 12, Layout::Planar420, true, PixelLayout::I420},
    // 10 and 16 bit, the formats an HDR capable card delivers.
    {"P010", FourccGuid(Fourcc('P', '0', '1', '0')), 24, Layout::Planar420, true, PixelLayout::P010},
    {"P016", FourccGuid(Fourcc('P', '0', '1', '6')), 24, Layout::Planar420, true, PixelLayout::P016},
    {"Y210", FourccGuid(Fourcc('Y', '2', '1', '0')), 32, Layout::Packed, false, PixelLayout::Other},
    {"Y216", FourccGuid(Fourcc('Y', '2', '1', '6')), 32, Layout::Packed, false, PixelLayout::Other},
    {"Y410", FourccGuid(Fourcc('Y', '4', '1', '0')), 32, Layout::Packed, false, PixelLayout::Other},
    {"v210", FourccGuid(Fourcc('v', '2', '1', '0')), 0, Layout::Compressed, false, PixelLayout::Other},
    {"r210", FourccGuid(Fourcc('r', '2', '1', '0')), 32, Layout::Packed, false, PixelLayout::Other},
    {"MJPG", FourccGuid(Fourcc('M', 'J', 'P', 'G')), 0, Layout::Compressed, false, PixelLayout::Other},
    {"dvsd", FourccGuid(Fourcc('d', 'v', 's', 'd')), 0, Layout::Compressed, false, PixelLayout::Other},
    {"H264", FourccGuid(Fourcc('H', '2', '6', '4')), 0, Layout::Compressed, false, PixelLayout::Other},
    {"HEVC", FourccGuid(Fourcc('H', 'E', 'V', 'C')), 0, Layout::Compressed, false, PixelLayout::Other},
};

// Goes by the label rather than the GUID, the way the renderer always has: a
// GUID the table does not know, but whose first four bytes spell a FOURCC it
// does, is taken for that format.
PixelLayout PixelLayoutOf(const std::string& label) {
  for (const SubtypeInfo& s : kSubtypes) {
    if (label == s.label) return s.pixels;
  }
  // DirectShow's RGB is a Windows bitmap: blue first.
  if (label == "RGB24") return PixelLayout::Bgr24;
  if (label == "RGB32") return PixelLayout::Bgrx32;
  if (label == "ARGB32") return PixelLayout::Bgra32;
  return PixelLayout::Other;
}

const SubtypeInfo* FindSubtype(const GUID& g) {
  for (const SubtypeInfo& s : kSubtypes) {
    if (IsEqualGUID(s.guid, g)) return &s;
  }
  return nullptr;
}

bool IsRgbSubtype(const GUID& g) {
  return IsEqualGUID(g, MEDIASUBTYPE_RGB24) || IsEqualGUID(g, MEDIASUBTYPE_RGB32) ||
         IsEqualGUID(g, MEDIASUBTYPE_ARGB32) || IsEqualGUID(g, MEDIASUBTYPE_RGB565) ||
         IsEqualGUID(g, MEDIASUBTYPE_RGB555);
}

double FpsFromInterval(REFERENCE_TIME interval) {
  if (interval <= 0) return 0.0;
  return 10000000.0 / (double)interval;
}

REFERENCE_TIME IntervalFromFps(double fps) {
  if (fps <= 0.0) return 0;
  return (REFERENCE_TIME)llround(10000000.0 / fps);
}

std::string ReadBagString(IPropertyBag* bag, const wchar_t* key) {
  VARIANT var;
  ::VariantInit(&var);
  std::string out;
  if (SUCCEEDED(bag->Read(key, &var, nullptr)) && var.vt == VT_BSTR && var.bstrVal) {
    out = ToUtf8(var.bstrVal);
  }
  ::VariantClear(&var);
  return out;
}

}  // namespace

// ------------------------------------------------------------- media subtypes

std::string SubtypeLabel(const GUID& subtype) {
  if (const SubtypeInfo* s = FindSubtype(subtype)) return s->label;
  if (IsEqualGUID(subtype, MEDIASUBTYPE_RGB24)) return "RGB24";
  if (IsEqualGUID(subtype, MEDIASUBTYPE_RGB32)) return "RGB32";
  if (IsEqualGUID(subtype, MEDIASUBTYPE_ARGB32)) return "ARGB32";
  if (IsEqualGUID(subtype, MEDIASUBTYPE_RGB565)) return "RGB565";
  if (IsEqualGUID(subtype, MEDIASUBTYPE_RGB555)) return "RGB555";

  // Unknown: if the first four bytes look like a printable FOURCC, show that.
  const unsigned char* p = (const unsigned char*)&subtype.Data1;
  bool printable = true;
  for (int i = 0; i < 4; ++i) {
    if (p[i] < 0x20 || p[i] > 0x7E) printable = false;
  }
  if (printable) return std::string((const char*)p, 4);
  return Format("%08X", (unsigned)subtype.Data1);
}

bool SubtypeFromLabel(const std::string& label, GUID* out) {
  for (const SubtypeInfo& s : kSubtypes) {
    if (label == s.label) {
      *out = s.guid;
      return true;
    }
  }
  if (label == "RGB24") { *out = MEDIASUBTYPE_RGB24; return true; }
  if (label == "RGB32") { *out = MEDIASUBTYPE_RGB32; return true; }
  if (label == "ARGB32") { *out = MEDIASUBTYPE_ARGB32; return true; }
  if (label == "RGB565") { *out = MEDIASUBTYPE_RGB565; return true; }
  if (label == "RGB555") { *out = MEDIASUBTYPE_RGB555; return true; }
  return false;
}

bool IsRendererSubtype(const GUID& subtype) {
  if (const SubtypeInfo* s = FindSubtype(subtype)) return s->rendererOk;
  return IsEqualGUID(subtype, MEDIASUBTYPE_RGB24) || IsEqualGUID(subtype, MEDIASUBTYPE_RGB32) ||
         IsEqualGUID(subtype, MEDIASUBTYPE_ARGB32);
}

int BitsPerPixelForSubtype(const GUID& subtype) {
  if (const SubtypeInfo* s = FindSubtype(subtype)) return s->bpp;
  if (IsEqualGUID(subtype, MEDIASUBTYPE_RGB24)) return 24;
  if (IsEqualGUID(subtype, MEDIASUBTYPE_RGB32) || IsEqualGUID(subtype, MEDIASUBTYPE_ARGB32)) return 32;
  if (IsEqualGUID(subtype, MEDIASUBTYPE_RGB565) || IsEqualGUID(subtype, MEDIASUBTYPE_RGB555)) return 16;
  return 0;
}

size_t ImageSizeForSubtype(const GUID& subtype, int width, int height, int* strideOut) {
  if (width <= 0 || height <= 0) {
    if (strideOut) *strideOut = 0;
    return 0;
  }
  const int bpp = BitsPerPixelForSubtype(subtype);
  Layout layout = Layout::Compressed;
  if (const SubtypeInfo* s = FindSubtype(subtype)) {
    layout = s->layout;
  } else if (IsRgbSubtype(subtype)) {
    layout = Layout::Rgb;
  }

  int stride = 0;
  size_t size = 0;
  switch (layout) {
    case Layout::Rgb:
      // DIB rows are padded to a 4 byte boundary.
      stride = ((width * bpp + 31) / 32) * 4;
      size = (size_t)stride * (size_t)height;
      break;
    case Layout::Packed:
      stride = width * bpp / 8;
      size = (size_t)stride * (size_t)height;
      break;
    case Layout::Planar420:
      // Luma plane stride: one byte per sample at 8 bit, two at 10 or 16.
      stride = width * (bpp > 12 ? 2 : 1);
      size = (size_t)stride * (size_t)height * 3 / 2;
      break;
    case Layout::Compressed:
    default:
      stride = 0;
      size = 0;  // size comes from the sample
      break;
  }
  if (strideOut) *strideOut = stride;
  return size;
}

// -------------------------------------------------------- colour description

namespace {

// The DXVA numbering the media type uses, into the program's own. Everything
// without a name here was "unknown" before it got this far, and stays so.
ColorInfo::Range RangeFromDxva(unsigned value) {
  switch (value) {
    case 1: return ColorInfo::Range::Full;
    case 2: return ColorInfo::Range::Limited;
    case 3: return ColorInfo::Range::From48To208;
    case 4: return ColorInfo::Range::From64To127;
    default: return ColorInfo::Range::Unknown;
  }
}

ColorInfo::Matrix MatrixFromDxva(unsigned value) {
  switch (value) {
    case 1: return ColorInfo::Matrix::BT709;
    case 2: return ColorInfo::Matrix::BT601;
    case 3: return ColorInfo::Matrix::SMPTE240M;
    case 4: return ColorInfo::Matrix::BT2020_10;
    case 5: return ColorInfo::Matrix::BT2020_12;
    default: return ColorInfo::Matrix::Unknown;
  }
}

ColorInfo::Primaries PrimariesFromDxva(unsigned value) {
  switch (value) {
    case 2: return ColorInfo::Primaries::BT709;
    case 3: return ColorInfo::Primaries::BT470M;
    case 4: return ColorInfo::Primaries::BT470BG;
    case 5: return ColorInfo::Primaries::SMPTE170M;
    case 6: return ColorInfo::Primaries::SMPTE240M;
    case 9: return ColorInfo::Primaries::BT2020;
    case 11: return ColorInfo::Primaries::DCIP3;
    default: return ColorInfo::Primaries::Unknown;
  }
}

ColorInfo::Transfer TransferFromDxva(unsigned value) {
  switch (value) {
    case 1: return ColorInfo::Transfer::Linear;
    case 4: return ColorInfo::Transfer::Gamma22;
    case 5: return ColorInfo::Transfer::BT709;
    case 7: return ColorInfo::Transfer::SRGB;
    case 12: return ColorInfo::Transfer::BT2020Constant;
    case 13: return ColorInfo::Transfer::BT2020;
    case 15: return ColorInfo::Transfer::PQ;
    case 16: return ColorInfo::Transfer::HLG;
    default: return ColorInfo::Transfer::Unknown;
  }
}

}  // namespace

// ---------------------------------------------------------------- media types

void FreeMediaType(AM_MEDIA_TYPE& mt) {
  if (mt.cbFormat != 0) {
    ::CoTaskMemFree(mt.pbFormat);
    mt.cbFormat = 0;
    mt.pbFormat = nullptr;
  }
  if (mt.pUnk) {
    mt.pUnk->Release();
    mt.pUnk = nullptr;
  }
}

void DeleteMediaType(AM_MEDIA_TYPE* mt) {
  if (!mt) return;
  FreeMediaType(*mt);
  ::CoTaskMemFree(mt);
}

AM_MEDIA_TYPE* CreateMediaTypeCopy(const AM_MEDIA_TYPE* src) {
  if (!src) return nullptr;
  auto* dst = (AM_MEDIA_TYPE*)::CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
  if (!dst) return nullptr;
  *dst = *src;
  dst->pbFormat = nullptr;
  dst->pUnk = nullptr;
  if (src->cbFormat && src->pbFormat) {
    dst->pbFormat = (BYTE*)::CoTaskMemAlloc(src->cbFormat);
    if (!dst->pbFormat) {
      ::CoTaskMemFree(dst);
      return nullptr;
    }
    memcpy(dst->pbFormat, src->pbFormat, src->cbFormat);
  } else {
    dst->cbFormat = 0;
  }
  if (src->pUnk) {
    dst->pUnk = src->pUnk;
    dst->pUnk->AddRef();
  }
  return dst;
}

bool ParseVideoMediaType(const AM_MEDIA_TYPE* mt, VideoFormatInfo* out) {
  if (!mt || !out || !mt->pbFormat) return false;
  if (!IsEqualGUID(mt->majortype, MEDIATYPE_Video)) return false;

  const BITMAPINFOHEADER* bih = nullptr;
  REFERENCE_TIME avgTime = 0;

  if (IsEqualGUID(mt->formattype, FORMAT_VideoInfo) && mt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
    const auto* vih = (const VIDEOINFOHEADER*)mt->pbFormat;
    bih = &vih->bmiHeader;
    avgTime = vih->AvgTimePerFrame;
    out->aspectX = 0;
    out->aspectY = 0;
    out->interlaced = false;
  } else if (IsEqualGUID(mt->formattype, FORMAT_VideoInfo2) &&
             mt->cbFormat >= sizeof(VIDEOINFOHEADER2)) {
    const auto* vih2 = (const VIDEOINFOHEADER2*)mt->pbFormat;
    bih = &vih2->bmiHeader;
    avgTime = vih2->AvgTimePerFrame;
    out->aspectX = (int)vih2->dwPictAspectRatioX;
    out->aspectY = (int)vih2->dwPictAspectRatioY;
    out->interlaced = (vih2->dwInterlaceFlags & AMINTERLACE_IsInterlaced) != 0;
    out->fieldOneFirst = (vih2->dwInterlaceFlags & AMINTERLACE_Field1First) != 0;

    // When this flag is set, dwControlFlags is really a DXVA_ExtendedFormat
    // bitfield. Decoded by hand rather than by casting to the struct: the bit
    // layout is fixed and documented, and this way there is no dependency on
    // which DXVA header happens to be reachable.
    if (vih2->dwControlFlags & AMCONTROL_COLORINFO_PRESENT) {
      const DWORD f = vih2->dwControlFlags;
      out->color.present = true;
      out->color.range = RangeFromDxva((f >> 12) & 0x7);
      out->color.matrix = MatrixFromDxva((f >> 15) & 0x7);
      out->color.primaries = PrimariesFromDxva((f >> 22) & 0x1F);
      out->color.transfer = TransferFromDxva((f >> 27) & 0x1F);
    }
  } else {
    return false;
  }

  out->subtypeLabel = SubtypeLabel(mt->subtype);
  out->layout = PixelLayoutOf(out->subtypeLabel);
  // HDYC is UYVY that carries BT.709 by definition.
  out->formatMatrix =
      out->subtypeLabel == "HDYC" ? ColorInfo::Matrix::BT709 : ColorInfo::Matrix::Unknown;
  out->width = (int)bih->biWidth;
  out->height = (int)std::abs(bih->biHeight);
  out->bottomUp = IsRgbSubtype(mt->subtype) && bih->biHeight > 0;
  out->fps = FpsFromInterval(avgTime);

  int stride = 0;
  size_t computed = ImageSizeForSubtype(mt->subtype, out->width, out->height, &stride);
  out->stride = stride;
  out->imageSize = bih->biSizeImage ? (size_t)bih->biSizeImage : computed;
  // Some drivers report a bogus biSizeImage; trust the computed size when we
  // know the layout and the reported one is clearly too small.
  if (computed && out->imageSize < computed) out->imageSize = computed;

  return out->width > 0 && out->height > 0;
}

// ----------------------------------------------------------------- device enum

namespace {

std::vector<VideoDeviceInfo> EnumerateDShowCategory(const GUID& category) {
  std::vector<VideoDeviceInfo> devices;

  ComPtr<ICreateDevEnum> devEnum;
  if (FAILED(CAP_HR(::CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&devEnum))))) {
    return devices;
  }

  ComPtr<IEnumMoniker> enumMoniker;
  HRESULT hr = devEnum->CreateClassEnumerator(category, &enumMoniker, 0);
  if (hr != S_OK || !enumMoniker) return devices;  // S_FALSE means no devices

  ComPtr<IMoniker> moniker;
  while (enumMoniker->Next(1, &moniker, nullptr) == S_OK) {
    VideoDeviceInfo info;

    ComPtr<IPropertyBag> bag;
    if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag)))) {
      info.name = ReadBagString(bag.Get(), L"FriendlyName");
      info.id = ReadBagString(bag.Get(), L"DevicePath");
    }

    LPOLESTR display = nullptr;
    if (SUCCEEDED(moniker->GetDisplayName(nullptr, nullptr, &display)) && display) {
      info.monikerName = ToUtf8(display);
      ::CoTaskMemFree(display);
    }

    if (info.name.empty()) info.name = T("Unbenanntes Gerät", "Unnamed device");
    if (info.id.empty()) info.id = info.monikerName;
    devices.push_back(std::move(info));

    moniker.Reset();
  }
  return devices;
}

}  // namespace

std::vector<VideoDeviceInfo> EnumerateVideoDevices() {
  return EnumerateDShowCategory(CLSID_VideoInputDeviceCategory);
}

std::vector<VideoDeviceInfo> EnumerateAudioCaptureDShowDevices() {
  return EnumerateDShowCategory(CLSID_AudioInputDeviceCategory);
}

std::vector<VideoDeviceInfo> EnumerateCrossbarDevices() {
  return EnumerateDShowCategory(AM_KSCATEGORY_CROSSBAR);
}

std::vector<VideoDeviceInfo> EnumerateDeviceCategory(const GUID& category) {
  return EnumerateDShowCategory(category);
}

ComPtr<IBaseFilter> CreateVideoFilter(const DeviceRef& ref, VideoDeviceInfo* resolved) {
  ComPtr<ICreateDevEnum> devEnum;
  if (FAILED(::CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&devEnum)))) {
    return nullptr;
  }
  ComPtr<IEnumMoniker> enumMoniker;
  if (devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enumMoniker, 0) != S_OK) {
    return nullptr;
  }

  // Three passes so an exact id match always wins over a name match.
  struct Candidate {
    ComPtr<IMoniker> moniker;
    VideoDeviceInfo info;
  };
  std::vector<Candidate> candidates;

  ComPtr<IMoniker> moniker;
  while (enumMoniker->Next(1, &moniker, nullptr) == S_OK) {
    Candidate c;
    c.moniker = moniker;
    ComPtr<IPropertyBag> bag;
    if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag)))) {
      c.info.name = ReadBagString(bag.Get(), L"FriendlyName");
      c.info.id = ReadBagString(bag.Get(), L"DevicePath");
    }
    LPOLESTR display = nullptr;
    if (SUCCEEDED(moniker->GetDisplayName(nullptr, nullptr, &display)) && display) {
      c.info.monikerName = ToUtf8(display);
      ::CoTaskMemFree(display);
    }
    if (c.info.id.empty()) c.info.id = c.info.monikerName;
    candidates.push_back(std::move(c));
    moniker.Reset();
  }

  const Candidate* pick = nullptr;
  if (!ref.id.empty()) {
    for (const Candidate& c : candidates) {
      if (c.info.id == ref.id) { pick = &c; break; }
    }
    if (!pick) {
      for (const Candidate& c : candidates) {
        if (!c.info.monikerName.empty() && c.info.monikerName == ref.id) { pick = &c; break; }
      }
    }
  }
  if (!pick && !ref.name.empty()) {
    for (const Candidate& c : candidates) {
      if (c.info.name == ref.name) { pick = &c; break; }
    }
  }
  if (!pick) return nullptr;

  ComPtr<IBaseFilter> filter;
  if (FAILED(CAP_HR(pick->moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&filter))))) {
    return nullptr;
  }
  if (resolved) *resolved = pick->info;
  return filter;
}

ComPtr<IBaseFilter> CreateFilterFromMoniker(const VideoDeviceInfo& info) {
  if (info.monikerName.empty()) return nullptr;

  ComPtr<IBindCtx> bindCtx;
  if (FAILED(CAP_HR(::CreateBindCtx(0, &bindCtx)))) return nullptr;

  ComPtr<IMoniker> moniker;
  ULONG eaten = 0;
  std::wstring display = ToWide(info.monikerName);
  if (FAILED(CAP_HR(::MkParseDisplayName(bindCtx.Get(), display.c_str(), &eaten, &moniker)))) {
    return nullptr;
  }

  ComPtr<IBaseFilter> filter;
  if (FAILED(CAP_HR(moniker->BindToObject(bindCtx.Get(), nullptr, IID_PPV_ARGS(&filter))))) {
    return nullptr;
  }
  return filter;
}

ComPtr<IPin> FindPinByDirection(IBaseFilter* filter, PIN_DIRECTION dir, int skip) {
  if (!filter) return nullptr;
  ComPtr<IEnumPins> pins;
  if (FAILED(filter->EnumPins(&pins))) return nullptr;
  ComPtr<IPin> pin;
  while (pins->Next(1, &pin, nullptr) == S_OK) {
    PIN_DIRECTION d;
    if (SUCCEEDED(pin->QueryDirection(&d)) && d == dir) {
      if (skip == 0) return pin;
      --skip;
    }
    pin.Reset();
  }
  return nullptr;
}

ComPtr<IPin> FindCapturePin(ICaptureGraphBuilder2* builder, IBaseFilter* filter) {
  if (!filter) return nullptr;
  if (builder) {
    ComPtr<IPin> pin;
    if (SUCCEEDED(builder->FindPin(filter, PINDIR_OUTPUT, &PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video,
                                   FALSE, 0, &pin)) &&
        pin) {
      return pin;
    }
  }
  // Fallback: first output pin that exposes IAMStreamConfig.
  ComPtr<IEnumPins> pins;
  if (FAILED(filter->EnumPins(&pins))) return nullptr;
  ComPtr<IPin> pin;
  ComPtr<IPin> firstOutput;
  while (pins->Next(1, &pin, nullptr) == S_OK) {
    PIN_DIRECTION d;
    if (SUCCEEDED(pin->QueryDirection(&d)) && d == PINDIR_OUTPUT) {
      if (!firstOutput) firstOutput = pin;
      ComPtr<IAMStreamConfig> cfg;
      if (SUCCEEDED(pin.As(&cfg))) return pin;
    }
    pin.Reset();
  }
  return firstOutput;
}

// ------------------------------------------------------------------ capability

std::vector<CapsEntry> EnumerateCaps(IPin* capturePin) {
  std::vector<CapsEntry> out;
  if (!capturePin) return out;

  ComPtr<IAMStreamConfig> cfg;
  if (FAILED(capturePin->QueryInterface(IID_PPV_ARGS(&cfg)))) return out;

  int count = 0, size = 0;
  if (FAILED(cfg->GetNumberOfCapabilities(&count, &size))) return out;
  if (size != sizeof(VIDEO_STREAM_CONFIG_CAPS)) {
    CAP_WARN("Unexpected size of VIDEO_STREAM_CONFIG_CAPS (%d), skipping the caps",
             size);
    return out;
  }

  for (int i = 0; i < count; ++i) {
    AM_MEDIA_TYPE* mt = nullptr;
    VIDEO_STREAM_CONFIG_CAPS vscc = {};
    if (FAILED(cfg->GetStreamCaps(i, &mt, (BYTE*)&vscc)) || !mt) continue;

    VideoFormatInfo info;
    if (ParseVideoMediaType(mt, &info)) {
      CapsEntry e;
      e.subtypeLabel = info.subtypeLabel;
      e.rendererOk = IsRendererSubtype(mt->subtype);
      e.width = info.width;
      e.height = info.height;
      e.defaultFps = info.fps;
      e.minFps = FpsFromInterval(vscc.MaxFrameInterval);
      e.maxFps = FpsFromInterval(vscc.MinFrameInterval);
      e.minWidth = (int)vscc.MinOutputSize.cx;
      e.maxWidth = (int)vscc.MaxOutputSize.cx;
      e.minHeight = (int)vscc.MinOutputSize.cy;
      e.maxHeight = (int)vscc.MaxOutputSize.cy;
      e.granularityX = (int)vscc.OutputGranularityX;
      e.granularityY = (int)vscc.OutputGranularityY;

      // Drivers that fill the caps struct with nonsense: fall back to the
      // media type's own resolution as the only supported one.
      if (e.minWidth <= 0 || e.maxWidth < e.minWidth) {
        e.minWidth = e.maxWidth = e.width;
      }
      if (e.minHeight <= 0 || e.maxHeight < e.minHeight) {
        e.minHeight = e.maxHeight = e.height;
      }
      if (e.maxFps <= 0.0) e.maxFps = e.defaultFps;
      if (e.minFps <= 0.0 || e.minFps > e.maxFps) e.minFps = e.maxFps > 0 ? 1.0 : 0.0;
      if (e.defaultFps <= 0.0) e.defaultFps = e.maxFps;

      out.push_back(std::move(e));
    }
    DeleteMediaType(mt);
  }

  CAP_LOG("Capture pin reports %d capabilities, %zu of them readable", count, out.size());
  return out;
}

// --------------------------------------------------------------- apply format

namespace {

// Rewrites width / height / frame interval in a copied media type.
bool PatchMediaType(AM_MEDIA_TYPE* mt, int width, int height, double fps) {
  if (!mt || !mt->pbFormat) return false;

  BITMAPINFOHEADER* bih = nullptr;
  REFERENCE_TIME* avgTime = nullptr;
  RECT* rcSource = nullptr;
  RECT* rcTarget = nullptr;

  if (IsEqualGUID(mt->formattype, FORMAT_VideoInfo) && mt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
    auto* vih = (VIDEOINFOHEADER*)mt->pbFormat;
    bih = &vih->bmiHeader;
    avgTime = &vih->AvgTimePerFrame;
    rcSource = &vih->rcSource;
    rcTarget = &vih->rcTarget;
  } else if (IsEqualGUID(mt->formattype, FORMAT_VideoInfo2) &&
             mt->cbFormat >= sizeof(VIDEOINFOHEADER2)) {
    auto* vih2 = (VIDEOINFOHEADER2*)mt->pbFormat;
    bih = &vih2->bmiHeader;
    avgTime = &vih2->AvgTimePerFrame;
    rcSource = &vih2->rcSource;
    rcTarget = &vih2->rcTarget;
    // Keep the aspect ratio consistent with the new size when it was set.
    if (vih2->dwPictAspectRatioX && vih2->dwPictAspectRatioY) {
      vih2->dwPictAspectRatioX = (DWORD)width;
      vih2->dwPictAspectRatioY = (DWORD)height;
    }
  } else {
    return false;
  }

  const bool wasBottomUp = bih->biHeight > 0;
  bih->biWidth = width;
  bih->biHeight = wasBottomUp ? height : -height;

  int bpp = BitsPerPixelForSubtype(mt->subtype);
  if (bpp > 0) {
    bih->biBitCount = (WORD)bpp;
    bih->biSizeImage = (DWORD)ImageSizeForSubtype(mt->subtype, width, height, nullptr);
  } else if (bih->biSizeImage == 0) {
    bih->biSizeImage = (DWORD)((size_t)width * height * 2);
  }

  if (fps > 0.0) *avgTime = IntervalFromFps(fps);

  // Empty source/target rectangles mean "whole image", which is what we want.
  *rcSource = RECT{};
  *rcTarget = RECT{};

  mt->lSampleSize = bih->biSizeImage;
  mt->bFixedSizeSamples = bpp > 0 ? TRUE : FALSE;
  return true;
}

}  // namespace

HRESULT ApplyFormat(IPin* capturePin, const FormatSel& fmt, VideoFormatInfo* applied) {
  if (!capturePin || !fmt.valid()) return E_INVALIDARG;

  ComPtr<IAMStreamConfig> cfg;
  HRESULT hr = capturePin->QueryInterface(IID_PPV_ARGS(&cfg));
  if (FAILED(hr)) return hr;

  GUID wantSubtype = GUID_NULL;
  if (!SubtypeFromLabel(fmt.subtype, &wantSubtype)) {
    CAP_WARN("Unknown colour format '%s'", fmt.subtype.c_str());
    return E_INVALIDARG;
  }

  int count = 0, size = 0;
  if (FAILED(cfg->GetNumberOfCapabilities(&count, &size)) ||
      size != sizeof(VIDEO_STREAM_CONFIG_CAPS)) {
    return E_FAIL;
  }

  // Pick a template: exact resolution match first, then any entry with the
  // same subtype. Patching an entry of the right subtype keeps driver-specific
  // fields (palette, private data) intact.
  AM_MEDIA_TYPE* templateMt = nullptr;
  int templateScore = -1;
  for (int i = 0; i < count; ++i) {
    AM_MEDIA_TYPE* mt = nullptr;
    VIDEO_STREAM_CONFIG_CAPS vscc = {};
    if (FAILED(cfg->GetStreamCaps(i, &mt, (BYTE*)&vscc)) || !mt) continue;

    int score = -1;
    VideoFormatInfo info;
    if (IsEqualGUID(mt->subtype, wantSubtype) && ParseVideoMediaType(mt, &info)) {
      score = 1;
      if (info.width == fmt.width && info.height == fmt.height) score = 3;
      else if (info.width >= fmt.width && info.height >= fmt.height) score = 2;
    }
    if (score > templateScore) {
      DeleteMediaType(templateMt);
      templateMt = mt;
      templateScore = score;
    } else {
      DeleteMediaType(mt);
    }
  }

  if (!templateMt || templateScore < 0) {
    DeleteMediaType(templateMt);
    CAP_WARN("No matching media type found for %s", fmt.subtype.c_str());
    return VFW_E_INVALIDMEDIATYPE;
  }

  AM_MEDIA_TYPE* patched = CreateMediaTypeCopy(templateMt);
  DeleteMediaType(templateMt);
  if (!patched) return E_OUTOFMEMORY;

  hr = E_FAIL;
  if (PatchMediaType(patched, fmt.width, fmt.height, fmt.fps)) {
    hr = cfg->SetFormat(patched);
    if (FAILED(hr) && fmt.fps > 0.0) {
      // Some drivers reject an unusual frame interval but accept the size.
      // Retry with the template's own interval so at least the resolution
      // takes effect; the actual rate is reported back to the caller.
      CAP_WARN("SetFormat with %.3f fps refused (%s), trying without a frame rate",
               fmt.fps, HrToEnglish(hr).c_str());
      AM_MEDIA_TYPE* retry = CreateMediaTypeCopy(patched);
      if (retry && PatchMediaType(retry, fmt.width, fmt.height, 0.0)) {
        hr = cfg->SetFormat(retry);
      }
      DeleteMediaType(retry);
    }
  }
  DeleteMediaType(patched);

  if (FAILED(hr)) return hr;

  if (applied) {
    AM_MEDIA_TYPE* current = nullptr;
    if (SUCCEEDED(cfg->GetFormat(&current)) && current) {
      ParseVideoMediaType(current, applied);
      DeleteMediaType(current);
    }
  }
  return S_OK;
}

// -------------------------------------------------------------------- crossbar

std::string PhysicalConnectorName(long physicalType) {
  switch (physicalType) {
    case PhysConn_Video_Tuner: return "Tuner";
    case PhysConn_Video_Composite: return "Composite";
    case PhysConn_Video_SVideo: return "S-Video";
    case PhysConn_Video_RGB: return "VGA / RGB";
    case PhysConn_Video_YRYBY: return "Component (YPbPr)";
    case PhysConn_Video_SerialDigital: return "HDMI / SDI";
    case PhysConn_Video_ParallelDigital: return "DVI";
    case PhysConn_Video_SCSI: return "SCSI";
    case PhysConn_Video_AUX: return "AUX";
    case PhysConn_Video_1394: return "FireWire";
    case PhysConn_Video_USB: return "USB";
    case PhysConn_Video_VideoDecoder: return T("Video-Decoder", "Video decoder");
    case PhysConn_Video_VideoEncoder: return T("Video-Encoder", "Video encoder");
    case PhysConn_Video_SCART: return "SCART";
    case PhysConn_Video_Black: return T("Schwarz (kein Eingang)", "Black (no input)");
    case PhysConn_Audio_Tuner: return "Audio Tuner";
    case PhysConn_Audio_Line: return "Audio Line-In";
    case PhysConn_Audio_Mic: return T("Mikrofon", "Microphone");
    case PhysConn_Audio_AESDigital: return "AES/EBU";
    case PhysConn_Audio_SPDIFDigital: return "S/PDIF";
    case PhysConn_Audio_SCSI: return "Audio SCSI";
    case PhysConn_Audio_AUX: return "Audio AUX";
    case PhysConn_Audio_1394: return "Audio FireWire";
    case PhysConn_Audio_USB: return "Audio USB";
    case PhysConn_Audio_AudioDecoder: return T("Audio-Decoder", "Audio decoder");
    default: return Format(T("Eingang Typ %ld", "Input type %ld"), physicalType);
  }
}

namespace {

ComPtr<IAMCrossbar> FindCrossbar(ICaptureGraphBuilder2* builder, IBaseFilter* captureFilter) {
  if (!builder || !captureFilter) return nullptr;
  ComPtr<IAMCrossbar> xbar;
  HRESULT hr = builder->FindInterface(&LOOK_UPSTREAM_ONLY, nullptr, captureFilter,
                                      IID_IAMCrossbar, (void**)xbar.GetAddressOf());
  if (FAILED(hr)) return nullptr;
  return xbar;
}

bool IsVideoConnector(long physicalType) {
  return physicalType < PhysConn_Audio_Tuner;
}

// ------------------------------------------------------ vendor input selectors
//
// Not every card that can switch its inputs says so through IAMCrossbar. The
// SA7160 this program is measured against says nothing at all: its connectors
// hang off a private KS property set that, until now, only the vendor's own
// property page ever wrote to. Having such a set is not what makes the card
// unusual -- having no crossbar beside it is -- so the selectors live in a
// table, and a second card is an entry in it rather than a second code path.
//
// Asking a driver about a set it does not know is safe: IKsPropertySet answers
// ERROR_SET_NOT_FOUND and touches nothing. That is what lets this be tried on
// every card without first having to know which one it is, and it is why no
// other card sees a changed path here.

// One connector, as the vendor numbers it. The names are carried along instead
// of coming out of PhysicalConnectorName, because two of them would collapse:
// HDMI and SDI are separate sockets and both are PhysConn_Video_SerialDigital.
struct VendorInput {
  DWORD value;
  long physicalType;
  const char* name;
};

// The SA7160's own numbering. Read off the radio group in the driver's property
// page, confirmed against the value the driver persists in its class key when
// that group is used, and confirmed again against the label table inside
// AmaRecTV, which writes the same property on the same set.
//
// Seven of the group's entries are named here. It has an eighth, and an AUTO
// beside it, whose values were never pinned down -- a card sitting on one of
// those reads back something this table does not name, which is the honest
// answer: input unknown. The list is what the driver offers, not what is
// soldered onto any one board; the vendor's own page offers exactly the same.
constexpr VendorInput kSa7160Inputs[] = {
    {0, PhysConn_Video_SerialDigital, "HDMI"},
    {1, PhysConn_Video_ParallelDigital, "DVI-D"},
    {2, PhysConn_Video_YRYBY, "Component (YPbPr)"},
    {3, PhysConn_Video_RGB, "VGA / RGB"},  // the vendor calls this one DVI-A
    {4, PhysConn_Video_SerialDigital, "SDI"},
    {5, PhysConn_Video_Composite, "Composite"},
    {6, PhysConn_Video_SVideo, "S-Video"},
};

struct VendorSelector {
  const char* card;  // log lines only
  GUID set;
  DWORD property;
  const VendorInput* inputs;
  size_t count;
};

constexpr VendorSelector kVendorSelectors[] = {
    {"SA7160",
     {0xD1E5209F, 0x68FD, 0x4529, {0xBE, 0xE0, 0x5E, 0x7A, 0x1F, 0x47, 0x92, 0x1C}},
     201,
     kSa7160Inputs,
     std::size(kSa7160Inputs)},
};

struct VendorSelection {
  ComPtr<IKsPropertySet> ks;
  const VendorSelector* sel = nullptr;
};

// Which of the known selectors this filter answers for, if any. One
// QuerySupported call per entry, and a set only counts when the driver will let
// the property be both read and written: a card offering inputs it cannot
// actually switch would be worse than one offering none.
VendorSelection FindVendorSelector(IBaseFilter* captureFilter) {
  VendorSelection found;
  if (!captureFilter) return found;
  ComPtr<IKsPropertySet> ks;
  if (FAILED(captureFilter->QueryInterface(IID_PPV_ARGS(&ks)))) return found;
  for (const VendorSelector& s : kVendorSelectors) {
    DWORD support = 0;
    if (FAILED(ks->QuerySupported(s.set, s.property, &support))) continue;
    if (!(support & KSPROPERTY_SUPPORT_GET)) continue;
    if (!(support & KSPROPERTY_SUPPORT_SET)) continue;
    found.ks = ks;
    found.sel = &s;
    break;
  }
  return found;
}

std::vector<CrossbarInput> VendorCrossbarInputs(IBaseFilter* captureFilter) {
  std::vector<CrossbarInput> inputs;
  const VendorSelection v = FindVendorSelector(captureFilter);
  if (!v.sel) return inputs;
  for (size_t i = 0; i < v.sel->count; ++i) {
    CrossbarInput in;
    in.pinIndex = (int)v.sel->inputs[i].value;
    in.physicalType = v.sel->inputs[i].physicalType;
    in.name = v.sel->inputs[i].name;
    inputs.push_back(std::move(in));
  }
  return inputs;
}

// Index into the list above, or -1 when the card reads back a value it does not
// name -- the eighth entry, AUTO, or anything a future driver adds.
int CurrentVendorInput(IBaseFilter* captureFilter) {
  const VendorSelection v = FindVendorSelector(captureFilter);
  if (!v.sel) return -1;
  DWORD value = 0;
  DWORD returned = 0;
  const HRESULT hr = v.ks->Get(v.sel->set, v.sel->property, nullptr, 0, &value, sizeof(value),
                               &returned);
  if (FAILED(hr) || returned != sizeof(value)) return -1;
  for (size_t i = 0; i < v.sel->count; ++i) {
    if (v.sel->inputs[i].value == value) return (int)i;
  }
  return -1;
}

bool RouteVendorInput(IBaseFilter* captureFilter, int index) {
  const VendorSelection v = FindVendorSelector(captureFilter);
  if (!v.sel) return false;
  if (index < 0 || (size_t)index >= v.sel->count) return false;

  DWORD value = v.sel->inputs[(size_t)index].value;
  const HRESULT hr = v.ks->Set(v.sel->set, v.sel->property, nullptr, 0, &value, sizeof(value));
  if (FAILED(hr)) {
    CAP_WARN("%s: could not set input '%s' (0x%08lX)", v.sel->card,
             v.sel->inputs[(size_t)index].name, (unsigned long)hr);
    return false;
  }
  CAP_LOG("%s: input '%s' set (property %lu = %lu)", v.sel->card,
          v.sel->inputs[(size_t)index].name, (unsigned long)v.sel->property,
          (unsigned long)value);
  return true;
}

}  // namespace

std::vector<CrossbarInput> EnumerateCrossbarInputs(ICaptureGraphBuilder2* builder,
                                                   IBaseFilter* captureFilter) {
  std::vector<CrossbarInput> inputs;
  ComPtr<IAMCrossbar> xbar = FindCrossbar(builder, captureFilter);
  if (!xbar) return VendorCrossbarInputs(captureFilter);

  long outCount = 0, inCount = 0;
  if (FAILED(xbar->get_PinCounts(&outCount, &inCount))) return inputs;

  // Count duplicates so two composite inputs become "Composite 1" / "Composite 2".
  std::vector<std::pair<long, int>> seen;
  for (long i = 0; i < inCount; ++i) {
    long related = 0, physType = 0;
    if (FAILED(xbar->get_CrossbarPinInfo(TRUE, i, &related, &physType))) continue;
    if (!IsVideoConnector(physType)) continue;

    int ordinal = 0;
    for (auto& s : seen) {
      if (s.first == physType) { ordinal = ++s.second; break; }
    }
    if (ordinal == 0) seen.emplace_back(physType, 0);

    CrossbarInput in;
    in.pinIndex = (int)i;
    in.physicalType = physType;
    in.name = PhysicalConnectorName(physType);
    if (ordinal > 0) in.name += Format(" %d", ordinal + 1);
    inputs.push_back(std::move(in));
  }
  return inputs;
}

bool RouteCrossbarInput(ICaptureGraphBuilder2* builder, IBaseFilter* captureFilter, int index) {
  if (index < 0) return true;  // "leave alone"
  ComPtr<IAMCrossbar> xbar = FindCrossbar(builder, captureFilter);
  if (!xbar) return RouteVendorInput(captureFilter, index);

  long outCount = 0, inCount = 0;
  if (FAILED(xbar->get_PinCounts(&outCount, &inCount))) return false;

  std::vector<CrossbarInput> inputs = EnumerateCrossbarInputs(builder, captureFilter);
  if (index >= (int)inputs.size()) return false;
  const long videoIn = inputs[(size_t)index].pinIndex;

  long relatedAudioIn = 0, physType = 0;
  xbar->get_CrossbarPinInfo(TRUE, videoIn, &relatedAudioIn, &physType);

  bool routed = false;
  for (long o = 0; o < outCount; ++o) {
    long relatedOut = 0, outPhys = 0;
    if (FAILED(xbar->get_CrossbarPinInfo(FALSE, o, &relatedOut, &outPhys))) continue;
    if (!IsVideoConnector(outPhys)) continue;
    if (xbar->CanRoute(o, videoIn) != S_OK) continue;
    if (SUCCEEDED(xbar->Route(o, videoIn))) {
      routed = true;
      CAP_LOG("Crossbar: video input '%s' routed to output %ld",
              inputs[(size_t)index].name.c_str(), o);
      // Route the paired audio input to the matching audio output, so picking
      // "Component" also selects the audio jacks that belong to it.
      if (relatedAudioIn >= 0 && relatedOut >= 0) {
        if (xbar->CanRoute(relatedOut, relatedAudioIn) == S_OK) {
          xbar->Route(relatedOut, relatedAudioIn);
        }
      }
      break;
    }
  }
  if (!routed) CAP_WARN("Crossbar: could not route input %d", index);
  return routed;
}

int CurrentCrossbarInput(ICaptureGraphBuilder2* builder, IBaseFilter* captureFilter) {
  ComPtr<IAMCrossbar> xbar = FindCrossbar(builder, captureFilter);
  if (!xbar) return CurrentVendorInput(captureFilter);

  long outCount = 0, inCount = 0;
  if (FAILED(xbar->get_PinCounts(&outCount, &inCount))) return -1;
  const std::vector<CrossbarInput> inputs = EnumerateCrossbarInputs(builder, captureFilter);

  // The first video output that has something routed to it. A crossbar may have
  // several outputs, but only one of them feeds the capture pin, and on every
  // card that has one it is the first video output there is.
  for (long o = 0; o < outCount; ++o) {
    long relatedOut = 0, outPhys = 0;
    if (FAILED(xbar->get_CrossbarPinInfo(FALSE, o, &relatedOut, &outPhys))) continue;
    if (!IsVideoConnector(outPhys)) continue;
    long routedFrom = -1;
    if (FAILED(xbar->get_IsRoutedTo(o, &routedFrom)) || routedFrom < 0) continue;
    for (size_t i = 0; i < inputs.size(); ++i) {
      if (inputs[i].pinIndex == (int)routedFrom) return (int)i;
    }
  }
  return -1;
}

bool ConnectorFollowsVideoStandard(long physicalType) {
  switch (physicalType) {
    case PhysConn_Video_Composite:
    case PhysConn_Video_SVideo:
    case PhysConn_Video_Tuner:
    case PhysConn_Video_SCART:
    case PhysConn_Video_AUX:
      // SCART fuehrt je nach Kabel Composite oder RGB, aber beide in einem
      // Sendersaster: es gibt kein SCART, das 720p traegt.
      return true;
    default:
      // Composite und S-Video sind die einzigen, bei denen die Norm die
      // Zeilenzahl wirklich festlegt. Component und VGA sind analog und tragen
      // trotzdem, was die Quelle will; HDMI, DVI und SDI beantworten die Frage
      // gar nicht erst.
      return false;
  }
}

// ---------------------------------------------------------------------------
// Analogue video standard

// qBlank's own numbers for the standards are DirectShow's, so that a value
// read off the decoder goes on unchanged and a stored setting needs no
// translation. This is where that is held to.
static_assert(kVideoStdNone == AnalogVideo_None, "");
static_assert(kVideoStdNtscM == AnalogVideo_NTSC_M, "");
static_assert(kVideoStdNtscMJ == AnalogVideo_NTSC_M_J, "");
static_assert(kVideoStdNtsc433 == AnalogVideo_NTSC_433, "");
static_assert(kVideoStdPalB == AnalogVideo_PAL_B, "");
static_assert(kVideoStdPalD == AnalogVideo_PAL_D, "");
static_assert(kVideoStdPalG == AnalogVideo_PAL_G, "");
static_assert(kVideoStdPalH == AnalogVideo_PAL_H, "");
static_assert(kVideoStdPalI == AnalogVideo_PAL_I, "");
static_assert(kVideoStdPalM == AnalogVideo_PAL_M, "");
static_assert(kVideoStdPalN == AnalogVideo_PAL_N, "");
static_assert(kVideoStdPal60 == AnalogVideo_PAL_60, "");
static_assert(kVideoStdSecamB == AnalogVideo_SECAM_B, "");
static_assert(kVideoStdSecamD == AnalogVideo_SECAM_D, "");
static_assert(kVideoStdSecamG == AnalogVideo_SECAM_G, "");
static_assert(kVideoStdSecamH == AnalogVideo_SECAM_H, "");
static_assert(kVideoStdSecamK == AnalogVideo_SECAM_K, "");
static_assert(kVideoStdSecamK1 == AnalogVideo_SECAM_K1, "");
static_assert(kVideoStdSecamL == AnalogVideo_SECAM_L, "");
static_assert(kVideoStdSecamL1 == AnalogVideo_SECAM_L1, "");
static_assert(kVideoStdPalNCombo == AnalogVideo_PAL_N_COMBO, "");

namespace {

ComPtr<IAMAnalogVideoDecoder> DecoderOf(IBaseFilter* filter) {
  ComPtr<IAMAnalogVideoDecoder> dec;
  if (filter) filter->QueryInterface(IID_PPV_ARGS(&dec));
  return dec;
}

ComPtr<IAMVideoProcAmp> ProcAmpOf(IBaseFilter* filter) {
  ComPtr<IAMVideoProcAmp> amp;
  if (filter) filter->QueryInterface(IID_PPV_ARGS(&amp));
  return amp;
}

// The ones that describe the picture, and only those.
//
// Gain, white balance and backlight compensation are left alone on purpose:
// they are the card deciding how to read the signal rather than what to do with
// it afterwards, they are usually on automatic, and pinning an analogue
// decoder's AGC to a fixed number is a good way to turn a slightly weak source
// into a dark one.
const struct {
  long property;
  const char* name;
} kProcAmpProps[] = {
    {VideoProcAmp_Brightness, "Helligkeit"}, {VideoProcAmp_Contrast, "Kontrast"},
    {VideoProcAmp_Hue, "Farbton"},           {VideoProcAmp_Saturation, "Sättigung"},
    {VideoProcAmp_Sharpness, "Schärfe"},     {VideoProcAmp_Gamma, "Gamma"},
    {VideoProcAmp_ColorEnable, "Farbe"},
};
const int kProcAmpPropCount = (int)(sizeof(kProcAmpProps) / sizeof(kProcAmpProps[0]));

}  // namespace

long AvailableVideoStandards(IBaseFilter* filter) {
  ComPtr<IAMAnalogVideoDecoder> dec = DecoderOf(filter);
  if (!dec) return 0;
  long available = 0;
  if (FAILED(dec->get_AvailableTVFormats(&available))) return 0;
  return available;
}

long CurrentVideoStandard(IBaseFilter* filter) {
  ComPtr<IAMAnalogVideoDecoder> dec = DecoderOf(filter);
  if (!dec) return 0;
  long current = 0;
  if (FAILED(dec->get_TVFormat(&current))) return 0;
  return current;
}

bool SetVideoStandard(IBaseFilter* filter, long standard) {
  ComPtr<IAMAnalogVideoDecoder> dec = DecoderOf(filter);
  if (!dec || standard == 0) return false;
  const HRESULT hr = dec->put_TVFormat(standard);
  if (FAILED(hr)) {
    CAP_WARN("Could not set video standard %s: %s",
             VideoStandardName(VideoStandardIndexOf(standard)), HrToEnglish(hr).c_str());
    return false;
  }
  CAP_LOG("Video standard set: %s", VideoStandardName(VideoStandardIndexOf(standard)));
  return true;
}

int NeutraliseProcAmp(IBaseFilter* filter) {
  ComPtr<IAMVideoProcAmp> amp = ProcAmpOf(filter);
  // Beide Faelle gehoeren ins Log, und zwar unterschieden. "Keine Zeile" hiesse
  // sonst entweder "die Karte hat keine solchen Regler" oder "sie standen schon
  // richtig", und das ist genau die Frage, die dieser Durchgang beantworten
  // soll: kommt hier ein unveraendertes Bild an oder nicht.
  if (!amp) {
    CAP_LOG("Card controls: none present, the picture arrives unchanged");
    return 0;
  }

  int moved = 0;
  int checked = 0;
  for (int i = 0; i < kProcAmpPropCount; ++i) {
    const long prop = kProcAmpProps[i].property;

    long min = 0, max = 0, step = 0, def = 0, caps = 0;
    if (FAILED(amp->GetRange(prop, &min, &max, &step, &def, &caps))) continue;
    // Nothing to put it back to. A driver that will only run this property on
    // automatic is deciding for itself, and overruling that needs a value it
    // has just said it does not take.
    if ((caps & VideoProcAmp_Flags_Manual) == 0) continue;
    ++checked;

    long value = 0, flags = 0;
    if (FAILED(amp->Get(prop, &value, &flags))) continue;
    if (value == def && (flags & VideoProcAmp_Flags_Manual) != 0) continue;

    if (FAILED(amp->Set(prop, def, VideoProcAmp_Flags_Manual))) {
      CAP_WARN("Card control %s could not be neutralised",
               kProcAmpProps[i].name);
      continue;
    }
    CAP_LOG("Card control %s neutralised: %ld -> %ld", kProcAmpProps[i].name, value, def);
    ++moved;
  }
  // Der dritte Fall, und auf der PEXHDCAP60L der tatsaechliche: die Karte
  // beantwortet die Schnittstelle, stellt aber keinen einzigen Regler von Hand
  // ein. Dann gibt es hier nichts zu neutralisieren -- und ebenso wenig etwas,
  // das uns das Bild verstellt haben koennte. Was der Treiberdialog anbietet,
  // laeuft in dem Fall ueber eine eigene Schnittstelle und ist von hier aus
  // nicht erreichbar.
  if (checked == 0) {
    CAP_LOG("Card controls: interface present, but none adjustable");
  } else if (moved == 0) {
    CAP_LOG("Card controls: %d found, all already neutral", checked);
  }
  return moved;
}

int VideoStandardLocked(IBaseFilter* filter) {
  ComPtr<IAMAnalogVideoDecoder> dec = DecoderOf(filter);
  if (!dec) return -1;
  long locked = 0;
  if (FAILED(dec->get_HorizontalLocked(&locked))) return -1;
  return locked ? 1 : 0;
}

}  // namespace cap
