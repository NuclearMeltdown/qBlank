#include "capture/dshow_util.h"

#include "i18n.h"

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
};

// Order matters only for the reverse lookup; the UI keeps driver order.
// The 10 and 16 bit entries are listed so that a card offering them gets a
// readable label and a correct frame size instead of a hex FOURCC -- the
// renderer cannot draw them yet, so selecting one falls back to a format it can.
const SubtypeInfo kSubtypes[] = {
    {"YUY2", FourccGuid(Fourcc('Y', 'U', 'Y', '2')), 16, Layout::Packed, true},
    {"UYVY", FourccGuid(Fourcc('U', 'Y', 'V', 'Y')), 16, Layout::Packed, true},
    {"YVYU", FourccGuid(Fourcc('Y', 'V', 'Y', 'U')), 16, Layout::Packed, true},
    {"HDYC", FourccGuid(Fourcc('H', 'D', 'Y', 'C')), 16, Layout::Packed, true},
    {"NV12", FourccGuid(Fourcc('N', 'V', '1', '2')), 12, Layout::Planar420, true},
    {"YV12", FourccGuid(Fourcc('Y', 'V', '1', '2')), 12, Layout::Planar420, true},
    {"I420", FourccGuid(Fourcc('I', '4', '2', '0')), 12, Layout::Planar420, true},
    {"IYUV", FourccGuid(Fourcc('I', 'Y', 'U', 'V')), 12, Layout::Planar420, true},
    // 10 and 16 bit, the formats an HDR capable card delivers.
    {"P010", FourccGuid(Fourcc('P', '0', '1', '0')), 24, Layout::Planar420, true},
    {"P016", FourccGuid(Fourcc('P', '0', '1', '6')), 24, Layout::Planar420, true},
    {"Y210", FourccGuid(Fourcc('Y', '2', '1', '0')), 32, Layout::Packed, false},
    {"Y216", FourccGuid(Fourcc('Y', '2', '1', '6')), 32, Layout::Packed, false},
    {"Y410", FourccGuid(Fourcc('Y', '4', '1', '0')), 32, Layout::Packed, false},
    {"v210", FourccGuid(Fourcc('v', '2', '1', '0')), 0, Layout::Compressed, false},
    {"r210", FourccGuid(Fourcc('r', '2', '1', '0')), 32, Layout::Packed, false},
    {"MJPG", FourccGuid(Fourcc('M', 'J', 'P', 'G')), 0, Layout::Compressed, false},
    {"dvsd", FourccGuid(Fourcc('d', 'v', 's', 'd')), 0, Layout::Compressed, false},
    {"H264", FourccGuid(Fourcc('H', '2', '6', '4')), 0, Layout::Compressed, false},
    {"HEVC", FourccGuid(Fourcc('H', 'E', 'V', 'C')), 0, Layout::Compressed, false},
};

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

// Resolutions offered on top of what the driver lists.
struct StdRes {
  int w, h;
};
const StdRes kStandardResolutions[] = {
    {320, 240},   {640, 360},   {640, 480},   {720, 480},   {720, 576},   {800, 600},
    {960, 540},   {1024, 768},  {1280, 720},  {1280, 800},  {1280, 1024}, {1360, 768},
    {1440, 900},  {1600, 900},  {1680, 1050}, {1920, 1080}, {1920, 1200}, {2560, 1440},
    {3840, 2160},
};

// DirectShow itself has no frame rate ceiling worth speaking of -- it stores the
// interval in 100 ns units -- so this list only has to cover what hardware and
// monitors actually run at, including the high refresh rates.
const double kStandardFps[] = {23.976, 24.0,  25.0,  29.97, 30.0,  48.0,  50.0,
                               59.94,  60.0,  72.0,  75.0,  90.0,  100.0, 119.88,
                               120.0,  144.0, 165.0, 180.0, 200.0, 240.0};

bool FpsNear(double a, double b) {
  return std::fabs(a - b) < 0.05;
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

const char* NominalRangeName(int value) {
  switch (value) {
    case 1: return "Full (0-255)";
    case 2: return "Limited (16-235)";
    case 3: return "48-208";
    case 4: return "64-127";
    default: return "unbekannt";
  }
}

const char* TransferMatrixName(int value) {
  switch (value) {
    case 1: return "BT.709";
    case 2: return "BT.601";
    case 3: return "SMPTE 240M";
    case 4: return "BT.2020 (10 Bit)";
    case 5: return "BT.2020 (12 Bit)";
    default: return "unbekannt";
  }
}

const char* PrimariesName(int value) {
  switch (value) {
    case 2: return "BT.709";
    case 3: return "BT.470-2 System M";
    case 4: return "BT.470-2 System B/G";
    case 5: return "SMPTE 170M";
    case 6: return "SMPTE 240M";
    case 9: return "BT.2020";
    case 11: return "DCI-P3";
    default: return "unbekannt";
  }
}

const char* TransferFunctionName(int value) {
  switch (value) {
    case 1: return "linear";
    case 4: return "Gamma 2.2";
    case 5: return "BT.709";
    case 7: return "sRGB";
    case 12: return "BT.2020 (konstante Luminanz)";
    case 13: return "BT.2020";
    case 15: return "PQ / ST.2084 (HDR10)";
    case 16: return "HLG (HDR)";
    default: return "unbekannt";
  }
}

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
      out->colorInfoPresent = true;
      out->nominalRange = (int)((f >> 12) & 0x7);
      out->transferMatrix = (int)((f >> 15) & 0x7);
      out->primaries = (int)((f >> 22) & 0x1F);
      out->transferFunction = (int)((f >> 27) & 0x1F);
    }
  } else {
    return false;
  }

  out->subtype = mt->subtype;
  out->subtypeLabel = SubtypeLabel(mt->subtype);
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
    CAP_WARN("Unerwartete Größe von VIDEO_STREAM_CONFIG_CAPS (%d), Caps werden übersprungen",
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
      e.subtype = info.subtype;
      e.subtypeLabel = info.subtypeLabel;
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

  CAP_LOG("Capture-Pin meldet %d Capability-Einträge, %zu davon lesbar", count, out.size());
  return out;
}

// ------------------------------------------------------------------ CapsModel

void CapsModel::Build(std::vector<CapsEntry> entries) {
  // Deduplicate identical (format, resolution, fps) rows -- some drivers list
  // the same combination several times.
  std::vector<CapsEntry> unique;
  for (CapsEntry& e : entries) {
    bool dup = false;
    for (const CapsEntry& u : unique) {
      if (u.subtypeLabel == e.subtypeLabel && u.width == e.width && u.height == e.height &&
          FpsNear(u.defaultFps, e.defaultFps)) {
        dup = true;
        break;
      }
    }
    if (!dup) unique.push_back(std::move(e));
  }
  entries_ = std::move(unique);
}

std::vector<std::string> CapsModel::Subtypes() const {
  std::vector<std::string> out;
  for (const CapsEntry& e : entries_) {
    if (std::find(out.begin(), out.end(), e.subtypeLabel) == out.end()) {
      out.push_back(e.subtypeLabel);
    }
  }
  return out;
}

std::vector<ResolutionOption> CapsModel::Resolutions(const std::string& subtype) const {
  std::vector<ResolutionOption> out;
  int minW = INT32_MAX, maxW = 0, minH = INT32_MAX, maxH = 0;
  bool any = false;
  // Only meaningful when the driver reports a size range rather than a list of
  // fixed sizes. Cards that report min == max per entry support exactly those
  // sizes, and offering anything in between would just be wrong.
  bool scalable = false;

  for (const CapsEntry& e : entries_) {
    if (e.subtypeLabel != subtype) continue;
    any = true;
    minW = std::min(minW, e.minWidth);
    maxW = std::max(maxW, e.maxWidth);
    minH = std::min(minH, e.minHeight);
    maxH = std::max(maxH, e.maxHeight);
    if (e.minWidth < e.maxWidth || e.minHeight < e.maxHeight) scalable = true;

    bool dup = false;
    for (const ResolutionOption& r : out) {
      if (r.width == e.width && r.height == e.height) { dup = true; break; }
    }
    if (!dup) out.push_back({e.width, e.height, false});
  }
  if (!any) return out;

  // Standard resolutions the driver did not list but that fit inside a range it
  // says it can scale to. Flagged so the UI can mark them as not guaranteed.
  if (scalable) {
    for (const StdRes& r : kStandardResolutions) {
      if (r.w < minW || r.w > maxW || r.h < minH || r.h > maxH) continue;
      bool dup = false;
      for (const ResolutionOption& o : out) {
        if (o.width == r.w && o.height == r.h) { dup = true; break; }
      }
      if (!dup) out.push_back({r.w, r.h, true});
    }
  }

  std::sort(out.begin(), out.end(), [](const ResolutionOption& a, const ResolutionOption& b) {
    if (a.width * a.height != b.width * b.height) {
      return a.width * a.height > b.width * b.height;
    }
    return a.width > b.width;
  });
  return out;
}

namespace {

// The union of the frame-rate ranges the driver reports for one format at one
// resolution. `borrowed` says the numbers came from a different resolution
// because none covered the one asked about -- they are then an educated guess
// rather than a promise, and the UI says so.
struct FpsRange {
  double min = 0.0;
  double max = 0.0;
  bool have = false;
  bool borrowed = false;
};

// Widens `r` to include one entry's range.
void Widen(FpsRange* r, const CapsEntry& e) {
  if (!r->have) {
    r->min = e.minFps;
    r->max = e.maxFps;
    r->have = true;
  } else {
    r->min = std::min(r->min, e.minFps);
    r->max = std::max(r->max, e.maxFps);
  }
}

// What the driver says about this format at this resolution.
FpsRange ReportedRange(const std::vector<CapsEntry>& entries, const std::string& subtype,
                       int width, int height) {
  FpsRange r;
  for (const CapsEntry& e : entries) {
    if (e.subtypeLabel != subtype) continue;
    const bool sameRes = (e.width == width && e.height == height);
    const bool coversRes = width >= e.minWidth && width <= e.maxWidth && height >= e.minHeight &&
                           height <= e.maxHeight;
    if (sameRes || coversRes) Widen(&r, e);
  }
  if (r.have) return r;

  // Nothing covers this resolution, so it is one the user forced. Borrow the
  // union of everything reported for the format: an educated guess beats an
  // empty dropdown, as long as it is labelled as one.
  for (const CapsEntry& e : entries) {
    if (e.subtypeLabel == subtype) Widen(&r, e);
  }
  r.borrowed = r.have;
  return r;
}

}  // namespace

std::vector<FpsOption> CapsModel::FpsList(const std::string& subtype, int width, int height) const {
  std::vector<FpsOption> out;
  const FpsRange range = ReportedRange(entries_, subtype, width, height);

  auto known = [&out](double fps) {
    for (const FpsOption& o : out) {
      if (FpsNear(o.fps, fps)) return true;
    }
    return false;
  };

  // Rates named outright for exactly this resolution.
  for (const CapsEntry& e : entries_) {
    if (e.subtypeLabel != subtype) continue;
    if (e.width != width || e.height != height) continue;
    if (e.defaultFps > 0.0 && !known(e.defaultFps)) out.push_back({e.defaultFps, false, false});
  }

  // Standard rates that fall inside the advertised range. These are not
  // inventions: a DirectShow range is the driver's own claim that it accepts
  // anything between the two ends, and 48 inside 25..59.94 is as much a promise
  // as 25 is. What used to sit here as well -- everything up to twice the
  // advertised maximum, on the theory that a card claiming 30 might secretly do
  // 60 -- is gone. It filled the list with rates no card had ever mentioned,
  // and on an input that reports no range at all it marked every single entry
  // from 25 to 119.88 "not reported", which is a dropdown that tells you
  // nothing. Anyone who wants to gamble on an unlisted rate can still type it
  // in by hand, and then it is their guess rather than ours.
  if (range.have) {
    for (double f : kStandardFps) {
      if (f < range.min - 0.05 || f > range.max + 0.05) continue;
      if (!known(f)) out.push_back({f, range.borrowed, false});
    }
    // The top of the range itself, when no standard rate happened to land on
    // it. A card topping out at 47.5 should still offer 47.5.
    if (range.max > 0.0 && !known(range.max)) out.push_back({range.max, range.borrowed, false});
  }

  std::sort(out.begin(), out.end(),
            [](const FpsOption& a, const FpsOption& b) { return a.fps > b.fps; });

  // Above the numbers, the entries that are not numbers. They lead because they
  // are the right answer for almost everyone: neither can go stale when the
  // console switches from 576i50 to 480p60.
  out.insert(out.begin(), FpsOption{0.0, false, true, false});
  // Und ueber der hoechsten die richtige, wo bekannt ist, welche das ist. Sie
  // steht nur da, wenn die Norm es sagt -- eine Auswahl anzubieten, die sich
  // beim Oeffnen als "geht nicht" herausstellt, waere schlechter als keine.
  if (nativeFieldRate_ > 0.0) out.insert(out.begin(), FpsOption{0.0, false, false, true});
  return out;
}

void CapsModel::SetNativeStandard(long standard) {
  nativeLines_ = VideoStandardLines(standard);
  nativeFieldRate_ = VideoStandardFieldRate(standard);
}

double CapsModel::NativeFps(const std::string& subtype, int width, int height) const {
  // Die Rate steht in der Normtabelle: 50 auf 625 Zeilen, 59,94 auf 525, 60 bei
  // PAL 60. Gefragt ist die Halbbildrate, nicht die halbe -- qBlank zeigt
  // Halbbilder einzeln, und eine Karte, die 720x576 anbietet, nennt dieselbe 50
  // dazu.
  const double want = nativeFieldRate_;
  if (want <= 0.0) return 0.0;

  // Ueber denselben Bereich wie "hoechste verfuegbare", damit beide Betriebs-
  // arten dieselbe Auskunft benutzen: der schliesst Eintraege ein, die diese
  // Groesse nur ueberdecken statt sie zu nennen, und leiht sich bei einer von
  // Hand erzwungenen Groesse die Raten des Formats. Sonst haette die Rate des
  // Signals nur dort funktioniert, wo die Karte genau diese Zeile auffuehrt --
  // also fuer 720x576 in jedem Pixelformat, aber fuer nichts Erzwungenes.
  const FpsRange range = ReportedRange(entries_, subtype, width, height);
  if (range.have && want >= range.min - 0.05 && want <= range.max + 0.05) return want;

  // Nennt die Karte sie nirgends, das Naechstgelegene von dem, was sie nennt --
  // eine, die nur 60,00 kennt, soll 60,00 bekommen und nicht 0. Erst aus den
  // Eintraegen zu dieser Groesse, und nur wenn es keine gibt, aus dem ganzen
  // Format.
  double best = 0.0;
  double bestErr = 1e9;
  auto consider = [&](double c) {
    if (c <= 0.0) return;
    const double err = std::fabs(c - want);
    if (err < bestErr) {
      bestErr = err;
      best = c;
    }
  };
  for (int pass = 0; pass < 2 && best <= 0.0; ++pass) {
    for (const CapsEntry& e : entries_) {
      if (e.subtypeLabel != subtype) continue;
      if (pass == 0) {
        const bool sameRes = (e.width == width && e.height == height);
        const bool coversRes = width >= e.minWidth && width <= e.maxWidth &&
                               height >= e.minHeight && height <= e.maxHeight;
        if (!sameRes && !coversRes) continue;
      }
      consider(e.defaultFps);
      consider(e.minFps);
      consider(e.maxFps);
    }
  }
  return best;
}

double CapsModel::HighestFps(const std::string& subtype, int width, int height) const {
  const FpsRange range = ReportedRange(entries_, subtype, width, height);
  double best = range.have ? range.max : 0.0;
  // A discrete entry can name a rate above its own range when a driver fills
  // the two fields inconsistently. Taking the larger keeps "highest" honest.
  for (const CapsEntry& e : entries_) {
    if (e.subtypeLabel != subtype) continue;
    if (e.width != width || e.height != height) continue;
    best = std::max(best, e.defaultFps);
  }
  return best;
}

bool CapsModel::IsAdvertised(const std::string& subtype, int width, int height, double fps) const {
  for (const CapsEntry& e : entries_) {
    if (e.subtypeLabel != subtype) continue;
    if (e.width != width || e.height != height) continue;
    // "Highest available" names no rate of its own, so it is advertised exactly
    // when the resolution is -- whatever comes back is the driver's own number.
    if (fps <= 0.0) return true;
    if (FpsNear(e.defaultFps, fps)) return true;
    if (fps >= e.minFps - 0.05 && fps <= e.maxFps + 0.05) return true;
  }
  return false;
}

FormatSel CapsModel::PickDefault(const std::string& preferSubtype) const {
  // Wieviele Zeilen das Bild hoechstens tragen kann. Ein 625-Zeilen-Raster hat
  // 576 sichtbare, ein 525er 480; ein paar Treiber bieten fuer 525 auch 486 an,
  // dafuer der Schlupf. Der laesst 576 unter 625 durch und 720 nirgends.
  const int activeLines = nativeLines_ >= 600 ? 576 : (nativeLines_ > 0 ? 480 : 0);

  FormatSel best;
  long long bestScore = -1;
  for (const CapsEntry& e : entries_) {
    // A named subtype beats everything below it, and everything below it still
    // decides between the entries that carry it. The area term tops out around
    // 2^33 at 4K, the line bonus sits at 2^39, the renderer bonus at 2^40 and
    // the raster bonus at 2^41, so 2^42 clears all four.
    const long long wishBonus =
        (!preferSubtype.empty() && e.subtypeLabel == preferSubtype) ? 1LL << 42 : 0;
    // Die richtige Groesse vor der bequemen. Wo das Raster bekannt ist, gewinnt
    // jeder Eintrag, der hineinpasst, gegen jeden, der darueber liegt -- auch
    // gegen einen, den der Renderer lieber haette. Ein Dekoder im Graphen
    // kostet Rechenzeit, eine hochskalierte Aufnahme kostet das Bild, und das
    // eine ist ruecknehmbar, das andere nicht. Passt gar nichts darunter,
    // bekommt jeder Eintrag dieselbe Null und es bleibt beim Groessten.
    const long long fitBonus =
        (activeLines > 0 && e.height > 0 && e.height <= activeLines + 16) ? 1LL << 41 : 0;
    // Prefer formats the renderer can take without a decoder in the graph.
    const long long formatBonus = IsRendererSubtype(e.subtype) ? 1LL << 40 : 0;
    // Unter dem Raster entscheidet sonst wieder die Flaeche, und die groesste
    // ist dort 768x576 -- eine Zeile, die dieselben 720 Abtastwerte auf 768
    // breitgerechnet hat, damit die Pixel quadratisch werden. Genau das soll
    // die Aufnahme nicht: eine Norm-Zeile hat 13,5 MHz und damit 720 Werte,
    // egal ob 525 oder 625 Zeilen (704 bei den Karten, die den Rand weglassen).
    // Also der echten Zeilenlaenge den Vorzug, der Umrechnung nicht. Bietet
    // niemand eine an, bleibt es bei Null und die Flaeche entscheidet weiter.
    const long long lineBonus =
        (activeLines > 0 && e.width >= 704 && e.width <= 720) ? 1LL << 39 : 0;
    const double fps = e.maxFps > 0.0 ? e.maxFps : e.defaultFps;
    const long long score = wishBonus + fitBonus + formatBonus + lineBonus +
                            (long long)e.width * e.height * 1000 + (long long)(fps * 10);
    if (score > bestScore) {
      bestScore = score;
      best.subtype = e.subtypeLabel;
      best.width = e.width;
      best.height = e.height;
      best.fps = fps;
      best.forced = false;
    }
  }
  return best;
}

ResolutionOption CapsModel::FittingResolution(const std::string& subtype, int activeLines) const {
  ResolutionOption best;
  if (activeLines <= 0) return best;
  long long bestScore = -1;
  for (const CapsEntry& e : entries_) {
    if (e.subtypeLabel != subtype) continue;
    // Derselbe Schlupf wie in PickDefault: 486 unter 525 zaehlt mit.
    if (e.height < activeLines || e.height > activeLines + 16) continue;
    const long long lineBonus = (e.width >= 704 && e.width <= 720) ? 1LL << 40 : 0;
    const long long score = lineBonus + (long long)e.width * e.height;
    if (score > bestScore) {
      bestScore = score;
      best.width = e.width;
      best.height = e.height;
    }
  }
  return best;
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
    CAP_WARN("Unbekanntes Farbformat '%s'", fmt.subtype.c_str());
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
    CAP_WARN("Kein passender Media-Type für %s gefunden", fmt.subtype.c_str());
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
      CAP_WARN("SetFormat mit %.3f fps abgelehnt (%s), versuche ohne Bildrate",
               fmt.fps, HrToString(hr).c_str());
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
    CAP_WARN("%s: Eingang '%s' konnte nicht gesetzt werden (0x%08lX)", v.sel->card,
             v.sel->inputs[(size_t)index].name, (unsigned long)hr);
    return false;
  }
  CAP_LOG("%s: Eingang '%s' gesetzt (Property %lu = %lu)", v.sel->card,
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
      CAP_LOG("Crossbar: Videoeingang '%s' auf Ausgang %ld geroutet",
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
  if (!routed) CAP_WARN("Crossbar: Eingang %d konnte nicht geroutet werden", index);
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

namespace {

struct StandardEntry {
  long value;
  const char* name;
  int lines;
  // Halbbilder je Sekunde. Nicht aus der Zeilenzahl ableitbar: die 525-Zeiler
  // teilen sich in die, deren Takt am NTSC-Farbtraeger haengt -- 60000/1001,
  // die krumme Rate --, und PAL 60, das es nur gibt, weil eine umgebaute
  // Konsole oder ein Player ein PAL-Bild mit *glatten* 60 Hz erzeugt. Beide
  // bietet dieselbe Karte an, und wer bei PAL 60 die 59,94 nimmt, hat sich um
  // ein Bild in tausend vertan.
  double fieldRate;
};

// Ordered so the common ones come first. The config stores the DirectShow value
// and not the index, so this list may be reordered and regrouped freely.
const StandardEntry kStandards[] = {
    {AnalogVideo_PAL_B, "PAL B", 625, 50.0},
    {AnalogVideo_PAL_G, "PAL G", 625, 50.0},
    {AnalogVideo_PAL_I, "PAL I", 625, 50.0},
    {AnalogVideo_PAL_D, "PAL D", 625, 50.0},
    {AnalogVideo_PAL_H, "PAL H", 625, 50.0},
    {AnalogVideo_PAL_N, "PAL N", 625, 50.0},
    {AnalogVideo_PAL_60, "PAL 60", 525, 60.0},
    {AnalogVideo_PAL_M, "PAL M", 525, 59.94},
    {AnalogVideo_NTSC_M, "NTSC M", 525, 59.94},
    {AnalogVideo_NTSC_M_J, "NTSC M (Japan)", 525, 59.94},
    {AnalogVideo_NTSC_433, "NTSC 4.43", 525, 59.94},
    {AnalogVideo_SECAM_B, "SECAM B", 625, 50.0},
    {AnalogVideo_SECAM_D, "SECAM D", 625, 50.0},
    {AnalogVideo_SECAM_G, "SECAM G", 625, 50.0},
    {AnalogVideo_SECAM_H, "SECAM H", 625, 50.0},
    {AnalogVideo_SECAM_K, "SECAM K", 625, 50.0},
    {AnalogVideo_SECAM_K1, "SECAM K1", 625, 50.0},
    {AnalogVideo_SECAM_L, "SECAM L", 625, 50.0},
    {AnalogVideo_SECAM_L1, "SECAM L1", 625, 50.0},
    {AnalogVideo_PAL_N_COMBO, "PAL N combo", 625, 50.0},
};
const int kStandardCount = (int)(sizeof(kStandards) / sizeof(kStandards[0]));

// The letters behind PAL and SECAM name RF properties -- sound carrier spacing
// and channel bandwidth -- and none of that survives the trip down a composite
// or S-Video cable. What reaches the decoder is a line count, a field rate and a
// colour subcarrier, and by those there are only the eight distinct pictures
// below. A list of twenty entries where eight of them differ is a list that
// makes the reader guess which one is theirs.
//
// The group is a display device only. What goes into the config is still the
// concrete DirectShow value the card accepted, so existing configs keep working
// and the readout below the picker can still name the exact standard.
//
// NTSC M and its Japanese variant stay apart on purpose: they differ in setup,
// the 7.5 IRE pedestal that lifts black off the blanking level, and that is a
// visible difference rather than a broadcast one.
struct StandardGroup {
  const char* name;
  long members;  // every value that decodes to the same picture
  const char* hintDe;
  const char* hintEn;
};

const StandardGroup kStandardGroups[] = {
    {"PAL",
     AnalogVideo_PAL_B | AnalogVideo_PAL_G | AnalogVideo_PAL_I | AnalogVideo_PAL_D |
         AnalogVideo_PAL_H,
     "625 Zeilen, 50 Hz, Träger 4,43 MHz. Europa, Australien, weite Teile Asiens und "
     "Afrikas. Umfasst PAL B, G, I, D und H, die sich nur im Tonträger unterscheiden.",
     "625 lines, 50 Hz, 4.43 MHz subcarrier. Europe, Australia, much of Asia and Africa. "
     "Covers PAL B, G, I, D and H, which differ only in their sound carrier."},
    {"PAL 60", AnalogVideo_PAL_60,
     "525 Zeilen, 60 Hz, Träger 4,43 MHz. Keine Sendenorm, sondern das, was umgebaute "
     "Konsolen und Player an einen PAL-Fernseher ausgeben.",
     "525 lines, 60 Hz, 4.43 MHz subcarrier. Not a broadcast standard, but what modified "
     "consoles and players send to a PAL television."},
    {"PAL M", AnalogVideo_PAL_M,
     "525 Zeilen, 60 Hz, Träger 3,58 MHz. Brasilien.",
     "525 lines, 60 Hz, 3.58 MHz subcarrier. Brazil."},
    {"PAL N", AnalogVideo_PAL_N | AnalogVideo_PAL_N_COMBO,
     "625 Zeilen, 50 Hz, Träger 3,58 MHz. Argentinien, Uruguay, Paraguay.",
     "625 lines, 50 Hz, 3.58 MHz subcarrier. Argentina, Uruguay, Paraguay."},
    {"NTSC", AnalogVideo_NTSC_M,
     "525 Zeilen, 60 Hz, Träger 3,58 MHz. Nordamerika. Schwarz liegt 7,5 IRE über "
     "Austastung.",
     "525 lines, 60 Hz, 3.58 MHz subcarrier. North America. Black sits 7.5 IRE above "
     "blanking."},
    {"NTSC Japan", AnalogVideo_NTSC_M_J,
     "Wie NTSC, aber Schwarz liegt auf Austastpegel. Zu hell wirkendes Schwarz an einer "
     "japanischen Konsole ist meistens dieser Unterschied.",
     "Like NTSC, but black sits at blanking level. Washed-out black on a Japanese console "
     "is usually this difference."},
    {"NTSC 4.43", AnalogVideo_NTSC_433,
     "525 Zeilen, 60 Hz, aber Träger 4,43 MHz. Keine Sendenorm, sondern was manche "
     "PAL-Player aus einer NTSC-Quelle machen.",
     "525 lines, 60 Hz, but a 4.43 MHz subcarrier. Not a broadcast standard, but what some "
     "PAL players make of an NTSC source."},
    {"SECAM",
     AnalogVideo_SECAM_B | AnalogVideo_SECAM_D | AnalogVideo_SECAM_G | AnalogVideo_SECAM_H |
         AnalogVideo_SECAM_K | AnalogVideo_SECAM_K1 | AnalogVideo_SECAM_L |
         AnalogVideo_SECAM_L1,
     "625 Zeilen, 50 Hz, Farbe frequenzmoduliert statt in der Phase. Frankreich, "
     "Osteuropa. Umfasst SECAM B bis L1.",
     "625 lines, 50 Hz, colour frequency modulated rather than carried in the phase. "
     "France, eastern Europe. Covers SECAM B through L1."},
};
const int kStandardGroupCount = (int)(sizeof(kStandardGroups) / sizeof(kStandardGroups[0]));

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

int VideoStandardCount() { return kStandardCount; }

long VideoStandardValue(int index) {
  if (index < 0 || index >= kStandardCount) return 0;
  return kStandards[index].value;
}

const char* VideoStandardName(int index) {
  if (index < 0 || index >= kStandardCount) return "";
  return kStandards[index].name;
}

int VideoStandardIndexOf(long value) {
  for (int i = 0; i < kStandardCount; ++i) {
    if (kStandards[i].value == value) return i;
  }
  return -1;
}

int VideoStandardLines(long value) {
  const int index = VideoStandardIndexOf(value);
  return index < 0 ? 0 : kStandards[index].lines;
}

double VideoStandardFieldRate(long value) {
  const int index = VideoStandardIndexOf(value);
  return index < 0 ? 0.0 : kStandards[index].fieldRate;
}

int VideoStandardGroupCount() { return kStandardGroupCount; }

const char* VideoStandardGroupName(int index) {
  if (index < 0 || index >= kStandardGroupCount) return "";
  return kStandardGroups[index].name;
}

const char* VideoStandardGroupHint(int index) {
  if (index < 0 || index >= kStandardGroupCount) return "";
  return T(kStandardGroups[index].hintDe, kStandardGroups[index].hintEn);
}

int VideoStandardGroupOf(long value) {
  if (value <= 0) return -1;
  for (int i = 0; i < kStandardGroupCount; ++i) {
    if (kStandardGroups[i].members & value) return i;
  }
  return -1;
}

// The value to hand the decoder when a group is picked. Its members produce the
// same picture, so any one the card admits to will do; kStandards decides the
// order, which is the one that puts the common variants first.
long VideoStandardGroupPick(int index, long available) {
  if (index < 0 || index >= kStandardGroupCount) return 0;
  const long members = kStandardGroups[index].members & available;
  if (members == 0) return 0;
  for (int i = 0; i < kStandardCount; ++i) {
    if (members & kStandards[i].value) return kStandards[i].value;
  }
  return 0;
}

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
    CAP_WARN("Videonorm %s konnte nicht gesetzt werden: %s",
             VideoStandardName(VideoStandardIndexOf(standard)), HrToString(hr).c_str());
    return false;
  }
  CAP_LOG("Videonorm gesetzt: %s", VideoStandardName(VideoStandardIndexOf(standard)));
  return true;
}

int NeutraliseProcAmp(IBaseFilter* filter) {
  ComPtr<IAMVideoProcAmp> amp = ProcAmpOf(filter);
  // Beide Faelle gehoeren ins Log, und zwar unterschieden. "Keine Zeile" hiesse
  // sonst entweder "die Karte hat keine solchen Regler" oder "sie standen schon
  // richtig", und das ist genau die Frage, die dieser Durchgang beantworten
  // soll: kommt hier ein unveraendertes Bild an oder nicht.
  if (!amp) {
    CAP_LOG("Kartenregler: keine vorhanden, Bild kommt unveraendert an");
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
      CAP_WARN("Kartenregler %s liess sich nicht neutralisieren",
               kProcAmpProps[i].name);
      continue;
    }
    CAP_LOG("Kartenregler %s neutralisiert: %ld -> %ld", kProcAmpProps[i].name, value, def);
    ++moved;
  }
  // Der dritte Fall, und auf der PEXHDCAP60L der tatsaechliche: die Karte
  // beantwortet die Schnittstelle, stellt aber keinen einzigen Regler von Hand
  // ein. Dann gibt es hier nichts zu neutralisieren -- und ebenso wenig etwas,
  // das uns das Bild verstellt haben koennte. Was der Treiberdialog anbietet,
  // laeuft in dem Fall ueber eine eigene Schnittstelle und ist von hier aus
  // nicht erreichbar.
  if (checked == 0) {
    CAP_LOG("Kartenregler: Schnittstelle da, aber keine von Hand einstellbar");
  } else if (moved == 0) {
    CAP_LOG("Kartenregler: %d gefunden, alle bereits neutral", checked);
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

std::string VideoStandardSettingName(long setting) {
  if (setting == 0) return T("Nicht ändern", "Leave alone");
  if (setting == -1) return T("Automatisch", "Automatic");
  const int index = VideoStandardIndexOf(setting);
  return index < 0 ? T("Unbekannt", "Unknown") : VideoStandardName(index);
}

// What the pickers show. They offer groups, so echoing back the exact variant
// the card took would name something the list never offered. The readout under
// the picker stays exact -- that one exists to say what really happened.
std::string VideoStandardPickerName(long setting) {
  const int group = VideoStandardGroupOf(setting);
  return group < 0 ? VideoStandardSettingName(setting) : VideoStandardGroupName(group);
}

VideoColourSystem VideoStandardColourSystem(long standard) {
  // Nach dem, was der Decoder mit der Farbe macht, nicht nach dem Namen. Die
  // Buchstaben hinter PAL und SECAM sind Rundfunkeigenschaften, und PAL 60,
  // PAL M und PAL N heissen PAL, weil sie die Phase zeilenweise wenden -- was
  // sie voneinander trennt, ist Zeilenzahl und Traeger, und beides wird
  // anderswo gefragt.
  switch (standard) {
    case AnalogVideo_NTSC_M:
    case AnalogVideo_NTSC_M_J:
    case AnalogVideo_NTSC_433:
      return VideoColourSystem::Ntsc;
    case AnalogVideo_SECAM_B:
    case AnalogVideo_SECAM_D:
    case AnalogVideo_SECAM_G:
    case AnalogVideo_SECAM_H:
    case AnalogVideo_SECAM_K:
    case AnalogVideo_SECAM_K1:
    case AnalogVideo_SECAM_L:
    case AnalogVideo_SECAM_L1:
      return VideoColourSystem::Secam;
    default:
      return VideoColourSystem::Pal;
  }
}

double VideoStandardSubcarrierSamples(long standard) {
  // 13.5 MHz of sampling over the active line, from BT.601.
  const double kSampleRate = 13500000.0;
  // NTSC M and its Japanese variant carry 3.579545 MHz. PAL M at 3.575611 and
  // PAL N at 3.582056 are close enough to count with them: 0.11 % and 0.07 %
  // off, which over the nine periods the demodulator looks at comes to a
  // fiftieth of a sample. Everything else in practice -- PAL, PAL-60, SECAM, and
  // the 4.43 MHz NTSC variant -- sits at 4.43361875 MHz.
  const bool ntsc = standard == AnalogVideo_NTSC_M || standard == AnalogVideo_NTSC_M_J ||
                    standard == AnalogVideo_PAL_M || standard == AnalogVideo_PAL_N ||
                    standard == AnalogVideo_PAL_N_COMBO;
  const double carrier = ntsc ? 3579545.0 : 4433618.75;
  return kSampleRate / carrier;
}


// Der Partner einer Norm: dieselbe Farbe, die andere Bildfrequenz.
//
// PAL und PAL 60 tragen beide den 4,43-MHz-Traeger und unterscheiden sich nur
// in Zeilenzahl und Bildrate. Genau dazwischen springt eine Konsole, die von 50
// auf 60 Hz umschaltet, und genau dort geht der Lock verloren.
//
// Das ist nicht nur eine Wahrscheinlichkeit, sondern noetig: der Decoder meldet
// einen *horizontalen* Lock, und PAL 60 und NTSC M haben dieselben 525 Zeilen
// bei 60 Hz. Sie unterscheiden sich allein im Farbtraeger, den diese Meldung
// gar nicht anfasst. Wer zuerst gefragt wird, gewinnt also -- deshalb muss der
// Partner vor dem allgemeinen Durchlauf drankommen.
long VideoStandardSibling(long standard, long available) {
  const int group = VideoStandardGroupOf(standard);
  if (group < 0) return 0;
  const int pal = VideoStandardGroupOf(AnalogVideo_PAL_B);
  const int pal60 = VideoStandardGroupOf(AnalogVideo_PAL_60);
  if (group == pal) return VideoStandardGroupPick(pal60, available);
  if (group == pal60) return VideoStandardGroupPick(pal, available);
  return 0;
}

namespace {

// Was in einer Region ueberhaupt vorkommt, in der Reihenfolge, in der es dort
// vorkommt. Endet je Zeile mit 0.
//
// Entscheidend ist nicht, was in der Liste steht, sondern was *zuerst* steht:
// innerhalb einer Zeilenzahl gewinnt der erste Eintrag immer, weil der Lock die
// Farbe nicht misst. Fuer Europa heisst das PAL 60 vor NTSC M -- eine 525/60-
// Quelle an einem europaeischen Anschluss ist fast immer eine umgebaute Konsole
// im 60-Hz-Modus und fast nie ein amerikanisches Geraet. In Nordamerika steht
// es genau andersherum, und das ist derselbe Satz mit vertauschten Rollen.
//
// Der Rest kommt danach aus kCommon und kRare dazu; hier steht nur der Vorlauf.
//
// `VideoRegion::None` hat hier absichtlich keine Zeile. Wer keine Region nennt,
// bekommt keinen Vorlauf -- die Schleife unten findet dann nichts und faellt
// direkt in kCommon. Das ist kein Sonderfall im Code, sondern einer, den die
// Tabelle durch Schweigen erledigt.
struct RegionLead {
  VideoRegion region;
  long order[4];
};
const RegionLead kRegionLeads[] = {
    {VideoRegion::PalEurope, {AnalogVideo_PAL_B, AnalogVideo_PAL_60, 0, 0}},
    {VideoRegion::NtscAmerica, {AnalogVideo_NTSC_M, AnalogVideo_NTSC_M_J, 0, 0}},
    {VideoRegion::NtscJapan, {AnalogVideo_NTSC_M_J, AnalogVideo_NTSC_M, 0, 0}},
    {VideoRegion::Secam, {AnalogVideo_SECAM_B, AnalogVideo_PAL_B, AnalogVideo_PAL_60, 0}},
    // Brasilien sendete 525/60 mit dem NTSC-Traeger; die Geraete von jenseits
    // der Grenze sind NTSC, nicht PAL.
    {VideoRegion::PalBrazil, {AnalogVideo_PAL_M, AnalogVideo_NTSC_M, 0, 0}},
    // PAL N ist 625/50 und steht damit vor PAL B, das dieselben Zeilen hat.
    {VideoRegion::PalArgentina, {AnalogVideo_PAL_N, AnalogVideo_PAL_B, 0, 0}},
};

}  // namespace

std::vector<long> AutoStandardCandidates(long available, VideoRegion region, long lastGood,
                                         int* preferred) {
  // Je ein Vertreter pro Zeilenzahl und Farbsystem, ueber die Gruppen gewaehlt.
  // PAL B und PAL G zu probieren hiesse dieselbe Frage zweimal stellen -- sie
  // unterscheiden sich im Tonträger, der kein Teil des Bildes ist. Ueber die
  // Gruppe statt ueber ein festes Bit, damit eine Karte, die PAL I meldet aber
  // kein PAL B, trotzdem einen PAL-Kandidaten bekommt.
  //
  // Zuerst die gaengigen, dann die seltenen. Jeder Kandidat kostet eine
  // Wartezeit, und die haeufigen sollen nicht hinter Normen stehen, die
  // ausserhalb einer Handvoll Laender niemand hat. Karten melden PAL M und
  // PAL N naemlich unabhaengig davon, ob im Umkreis von tausend Kilometern
  // jemand so sendet.
  static const long kCommon[] = {
      AnalogVideo_PAL_B,     // PAL, 625/50 -- Europa, Australien, halb Asien
      AnalogVideo_PAL_60,    // PAL 60, 525/60 -- umgebaute Konsolen und Player
      AnalogVideo_NTSC_M,    // NTSC, 525/60 -- Nordamerika
      AnalogVideo_NTSC_M_J,  // NTSC Japan, dasselbe mit anderem Schwarzpegel
  };
  static const long kRare[] = {
      AnalogVideo_SECAM_B,  // SECAM, 625/50 -- Frankreich, Osteuropa
      AnalogVideo_PAL_M,    // 525/60 mit 3,58 MHz -- Brasilien
      AnalogVideo_PAL_N,    // 625/50 mit 3,58 MHz -- Argentinien und Nachbarn
      AnalogVideo_NTSC_433,  // 525/60 mit 4,43 MHz -- was PAL-Player daraus machen
  };

  std::vector<long> out;
  auto add = [&out](long value) {
    if (value <= 0) return;
    for (long have : out) {
      if (have == value) return;
    }
    out.push_back(value);
  };
  auto addFamily = [&](long representative) {
    add(VideoStandardGroupPick(VideoStandardGroupOf(representative), available));
  };

  // Ganz vorne der Partner der zuletzt eingerasteten Norm, dahinter sie selbst.
  // Das deckt die zwei Faelle ab, in denen ein Lock ueberhaupt verloren geht,
  // ohne dass das Kabel gezogen wurde: die Konsole schaltet zwischen 50 und
  // 60 Hz um, oder sie wurde neu gestartet und kommt gleich wieder.
  const long sibling = VideoStandardSibling(lastGood, available);
  add(sibling);
  if (lastGood > 0 && (available & lastGood) != 0) add(lastGood);
  if (preferred) *preferred = (int)out.size();

  // Dahinter das, was in der Gegend des Nutzers ueberhaupt vorkommt. Erst
  // danach der allgemeine Durchlauf -- der bleibt vollstaendig, damit eine
  // falsch eingestellte Region die richtige Norm verzoegert und nicht
  // verhindert.
  const VideoRegion resolved = ResolveVideoRegion(region);
  for (const RegionLead& lead : kRegionLeads) {
    if (lead.region != resolved) continue;
    for (long family : lead.order) {
      if (family == 0) break;
      addFamily(family);
    }
    break;
  }

  for (long family : kCommon) addFamily(family);
  for (long family : kRare) addFamily(family);
  return out;
}

std::vector<long> VideoStandardColourCandidates(long standard, long available,
                                                VideoRegion region) {
  std::vector<long> out;
  const int lines = VideoStandardLines(standard);
  if (lines <= 0) return out;

  // Ueber dieselbe Kandidatenliste wie die Normensuche selbst. Die hat die
  // Arbeit schon getan: je ein Vertreter pro Farbsystem, die Tonträgervarianten
  // zusammengefasst, und nach Region sortiert. Hier bleibt nur, sie auf die
  // Zeilenzahl einzuengen -- die steht ja fest, der Lock hat sie bestaetigt.
  //
  // Ohne lastGood: gesucht wird, was zur *jetzigen* Norm passt, nicht was
  // zuletzt gut war.
  const std::vector<long> ordered = AutoStandardCandidates(available, region, 0, nullptr);

  // Und was zweimal dasselbe misst, wird einmal gemessen.
  //
  // Der Rundgang urteilt ueber die Farbe und ueber nichts sonst: wieviel
  // Chroma im Bild steckt und wie eingefaerbt die Tiefen sind. Zwei Normen mit
  // demselben Farbsystem auf demselben Traeger erzeugen daraus dasselbe Bild,
  // und die zweite Messung kann deshalb nichts sagen, was die erste nicht
  // schon gesagt hat -- sie kostet nur ihre Einschwingzeit und stellt ihr
  // Ergebnis als Konkurrenten neben das Original.
  //
  // Bei 525 Zeilen trifft das genau ein Paar: NTSC M und NTSC M (Japan), beide
  // NTSC auf 3,58 MHz. Sie unterscheiden sich im Schwarzabhebungswert -- 7,5
  // IRE gegen 0 -- und das ist eine Sache der Helligkeit, die dieser Rundgang
  // gar nicht misst. Am 31.08.2026 um 02:42 standen sie im Log denn auch
  // brav nebeneinander, 0,9 Sekunden fuer eine Auskunft, die schon vorlag.
  //
  // Bei 625 Zeilen faellt nichts weg: PAL B (4,43), PAL N (3,58) und SECAM
  // sind drei verschiedene Bilder. Der Rundgang verliert dadurch also nichts
  // ausser der Wiederholung.
  //
  // Welche der beiden stehen bleibt, entscheidet die Reihenfolge und damit die
  // Region -- dieselbe Regel, die schon ueberall sonst den Gleichstand
  // aufloest. In Japan kommt die japanische zuerst, sonst die amerikanische.
  auto sameColour = [](long a, long b) {
    return VideoStandardColourSystem(a) == VideoStandardColourSystem(b) &&
           VideoStandardSubcarrierSamples(a) == VideoStandardSubcarrierSamples(b);
  };

  // Die jetzige zuerst, denn sie ist bereits gemessen; sie noch einmal
  // einzustellen waere ein Wechsel und kostete eine Einschwingzeit umsonst.
  if ((available & standard) != 0) out.push_back(standard);
  for (long candidate : ordered) {
    if (candidate == standard) continue;
    if (VideoStandardLines(candidate) != lines) continue;
    bool duplicate = false;
    for (long have : out) {
      if (sameColour(have, candidate)) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;
    out.push_back(candidate);
  }
  return out;
}

}  // namespace cap
