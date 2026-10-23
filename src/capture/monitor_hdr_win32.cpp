#include "capture/monitor_hdr_win32.h"

#include <dxgi1_6.h>

#include <vector>

namespace cap {

bool DisplayPathOf(const wchar_t* device, DISPLAYCONFIG_PATH_INFO* path) {
  UINT32 pathCount = 0;
  UINT32 modeCount = 0;
  if (::GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) {
    return false;
  }
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
  if (::QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount,
                           modes.data(), nullptr) != ERROR_SUCCESS) {
    return false;
  }
  for (UINT32 i = 0; i < pathCount; ++i) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof(source);
    source.header.adapterId = paths[i].sourceInfo.adapterId;
    source.header.id = paths[i].sourceInfo.id;
    if (::DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS &&
        wcscmp(source.viewGdiDeviceName, device) == 0) {
      *path = paths[i];
      return true;
    }
  }
  return false;
}

MonitorHdr DisplayHdr(const wchar_t* device) {
  MonitorHdr hdr;
  DISPLAYCONFIG_PATH_INFO path = {};
  if (!DisplayPathOf(device, &path)) return hdr;
  // HDR, not the wide colour Windows may put on an SDR screen.
  DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color = {};
  color.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
  color.header.size = sizeof(color);
  color.header.adapterId = path.targetInfo.adapterId;
  color.header.id = path.targetInfo.id;
  if (::DisplayConfigGetDeviceInfo(&color.header) != ERROR_SUCCESS ||
      !color.advancedColorEnabled || color.wideColorEnforced) {
    return hdr;
  }
  hdr.on = true;
  // In thousandths of eighty nits.
  DISPLAYCONFIG_SDR_WHITE_LEVEL white = {};
  white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
  white.header.size = sizeof(white);
  white.header.adapterId = path.targetInfo.adapterId;
  white.header.id = path.targetInfo.id;
  if (::DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel) {
    hdr.whiteNits = (float)white.SDRWhiteLevel / 1000.0f * 80.0f;
  }
  return hdr;
}

MonitorHdr HdrOf(HMONITOR monitor) {
  MONITORINFOEXW mi = {};
  mi.cbSize = sizeof(mi);
  if (!::GetMonitorInfoW(monitor, &mi)) return MonitorHdr();
  MonitorHdr hdr = DisplayHdr(mi.szDevice);
  if (!hdr.on) return hdr;
  // The peak only DXGI knows.
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return hdr;
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    ComPtr<IDXGIOutput> output;
    for (UINT j = 0; adapter->EnumOutputs(j, &output) != DXGI_ERROR_NOT_FOUND; ++j) {
      ComPtr<IDXGIOutput6> output6;
      DXGI_OUTPUT_DESC1 desc = {};
      if (SUCCEEDED(output.As(&output6)) && SUCCEEDED(output6->GetDesc1(&desc)) &&
          desc.Monitor == monitor) {
        hdr.peakNits = desc.MaxLuminance;
        return hdr;
      }
    }
  }
  return hdr;
}

}  // namespace cap
