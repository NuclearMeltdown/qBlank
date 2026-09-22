#include "tools/mf_probe.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <cstdio>
#include <string>
#include <vector>

#include "common_win32.h"
#include "text_win32.h"

namespace cap {
namespace {

// MF reports a subtype GUID whose first four bytes are the FOURCC for the
// formats we care about, exactly like DirectShow does.
std::string MfSubtypeLabel(const GUID& subtype) {
  const unsigned char* p = (const unsigned char*)&subtype.Data1;
  bool printable = true;
  for (int i = 0; i < 4; ++i) {
    if (p[i] < 0x20 || p[i] > 0x7E) printable = false;
  }
  if (printable) return std::string((const char*)p, 4);
  if (IsEqualGUID(subtype, MFVideoFormat_RGB24)) return "RGB24";
  if (IsEqualGUID(subtype, MFVideoFormat_RGB32)) return "RGB32";
  if (IsEqualGUID(subtype, MFVideoFormat_ARGB32)) return "ARGB32";
  return Format("%08X", (unsigned)subtype.Data1);
}

std::string ReadStringAttribute(IMFAttributes* attrs, const GUID& key) {
  if (!attrs) return {};
  LPWSTR value = nullptr;
  UINT32 length = 0;
  if (FAILED(attrs->GetAllocatedString(key, &value, &length)) || !value) return {};
  std::string out = ToUtf8(value);
  ::CoTaskMemFree(value);
  return out;
}

void PrintDeviceFormats(IMFActivate* activate) {
  ComPtr<IMFMediaSource> source;
  HRESULT hr = activate->ActivateObject(IID_PPV_ARGS(&source));
  if (FAILED(hr)) {
    std::printf("        (kann nicht geöffnet werden: %s)\n", HrToString(hr).c_str());
    return;
  }

  ComPtr<IMFSourceReader> reader;
  hr = ::MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader);
  if (FAILED(hr)) {
    std::printf("        (kein Source Reader: %s)\n", HrToString(hr).c_str());
    source->Shutdown();
    return;
  }

  // Deduplicate: MF lists a row per (format, size, rate) and cards repeat a lot.
  std::vector<std::string> seen;
  for (DWORD i = 0;; ++i) {
    ComPtr<IMFMediaType> type;
    hr = reader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &type);
    if (hr == MF_E_NO_MORE_TYPES || FAILED(hr)) break;

    GUID subtype = GUID_NULL;
    type->GetGUID(MF_MT_SUBTYPE, &subtype);

    UINT32 width = 0, height = 0;
    ::MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height);

    UINT32 num = 0, den = 0;
    ::MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &num, &den);
    const double fps = den ? (double)num / (double)den : 0.0;

    std::string line =
        Format("        %-6s %5ux%-5u  %6.2f fps", MfSubtypeLabel(subtype).c_str(), width, height,
               fps);
    bool dup = false;
    for (const std::string& s : seen) {
      if (s == line) { dup = true; break; }
    }
    if (!dup) {
      seen.push_back(line);
      std::printf("%s\n", line.c_str());
    }
  }
  if (seen.empty()) std::printf("        (keine Formate gemeldet)\n");

  reader.Reset();
  source->Shutdown();
  activate->ShutdownObject();
}

void PrintCategory(const GUID& category, const char* title, bool withFormats) {
  std::printf("================ Media Foundation: %s ================\n", title);

  ComPtr<IMFAttributes> attrs;
  if (FAILED(::MFCreateAttributes(&attrs, 1))) {
    std::printf("  ! MFCreateAttributes fehlgeschlagen\n\n");
    return;
  }
  attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, category);

  IMFActivate** devices = nullptr;
  UINT32 count = 0;
  HRESULT hr = ::MFEnumDeviceSources(attrs.Get(), &devices, &count);
  if (FAILED(hr)) {
    std::printf("  ! MFEnumDeviceSources: %s\n\n", HrToString(hr).c_str());
    return;
  }
  if (count == 0) std::printf("  (keine gefunden)\n");

  for (UINT32 i = 0; i < count; ++i) {
    const std::string name =
        ReadStringAttribute(devices[i], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
    std::printf("  [%u] %s\n", i, name.c_str());

    const std::string link =
        ReadStringAttribute(devices[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
    if (!link.empty()) std::printf("       SymbolicLink: %s\n", link.c_str());

    if (withFormats) PrintDeviceFormats(devices[i]);
    devices[i]->Release();
  }
  ::CoTaskMemFree(devices);
  std::printf("\n");
}

}  // namespace

void PrintMediaFoundationDevices() {
  HRESULT hr = ::MFStartup(MF_VERSION, MFSTARTUP_LITE);
  if (FAILED(hr)) {
    std::printf("Media Foundation nicht verfügbar: %s\n\n", HrToString(hr).c_str());
    return;
  }
  PrintCategory(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID, "Videoquellen", true);
  PrintCategory(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_GUID, "Audioquellen", false);
  ::MFShutdown();
}

}  // namespace cap
