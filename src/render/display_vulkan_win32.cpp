// The Windows answers to display_vulkan_platform.h.

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include "render/display_vulkan_platform.h"

#include <dxgi1_6.h>

#include "common_win32.h"
#include "i18n.h"
#include "render/vulkan_device.h"
#include "window_win32.h"

namespace cap {

bool VulkanLoaderPresent(std::string* error) {
  // The program links vulkan-1.dll late, so a missing loader ends here rather
  // than at start. Only System32, where the drivers put it: a vulkan-1.dll
  // lying next to the program would otherwise be taken first. The later calls
  // find this module by its name and use it.
  HMODULE loader = ::LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!loader) {
    return ReportError(error, CAP_SAID(T("Vulkan ist nicht installiert (vulkan-1.dll fehlt).",
                                         "Vulkan is not installed (vulkan-1.dll is missing).")));
  }
  // A loader from before Vulkan 1.3 lacks these, and the late link would fail
  // on the first of them in the middle of a frame.
  for (const char* name : {"vkCmdBeginRendering", "vkCmdPipelineBarrier2", "vkQueueSubmit2"}) {
    if (!::GetProcAddress(loader, name)) {
      return ReportError(error, CAP_SAID(T("Der Vulkan-Loader ist zu alt (Vulkan 1.3 fehlt).",
                                           "The Vulkan loader is too old (Vulkan 1.3 is missing).")));
    }
  }
  return true;
}

const char* VulkanSurfaceExtension() { return VK_KHR_WIN32_SURFACE_EXTENSION_NAME; }

bool CreateVulkanSurface(VkInstance instance, const Window& window, VkSurfaceKHR* surface,
                         std::string* error) {
  VkWin32SurfaceCreateInfoKHR info = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
  info.hinstance = ::GetModuleHandleW(nullptr);
  info.hwnd = NativeWindow(window);
  const VkResult result = vkCreateWin32SurfaceKHR(instance, &info, nullptr, surface);
  if (result != VK_SUCCESS) {
    *surface = VK_NULL_HANDLE;
    return ReportError(error, CAP_SAID(std::string(T("Vulkan-Fläche fehlgeschlagen: ",
                                                     "Vulkan surface failed: ")) +
                                       VkResultName(result)));
  }
  return true;
}

void VulkanClientSize(const Window& window, int* width, int* height) {
  RECT rect = {};
  ::GetClientRect(NativeWindow(window), &rect);
  *width = (int)(rect.right - rect.left);
  *height = (int)(rect.bottom - rect.top);
}

Display::DisplayCapability VulkanDisplayCapability(const Window& window) {
  Display::DisplayCapability capability;

  // Vulkan says nothing about whether Windows runs the screen in HDR, so DXGI
  // is asked, as the Direct3D 11 display asks it. There is no swapchain of
  // DXGI's to name the output, so the screen the window mostly sits on is
  // looked up among all outputs.
  HMONITOR monitor = ::MonitorFromWindow(NativeWindow(window), MONITOR_DEFAULTTONEAREST);
  if (!monitor) return capability;

  ComPtr<IDXGIFactory1> factory;
  if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return capability;
  for (UINT a = 0;; ++a) {
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) break;
    for (UINT o = 0;; ++o) {
      ComPtr<IDXGIOutput> output;
      if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND) break;
      DXGI_OUTPUT_DESC desc = {};
      if (FAILED(output->GetDesc(&desc)) || desc.Monitor != monitor) continue;

      ComPtr<IDXGIOutput6> output6;
      DXGI_OUTPUT_DESC1 desc1 = {};
      if (FAILED(output.As(&output6)) || !output6 || FAILED(output6->GetDesc1(&desc1))) {
        return capability;
      }
      capability.hdr = desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
      capability.peakNits = desc1.MaxLuminance > 0.0f ? desc1.MaxLuminance : 100.0f;
      capability.minNits = desc1.MinLuminance;
      return capability;
    }
  }
  return capability;
}

bool VulkanTearingSupported() {
  ComPtr<IDXGIFactory5> factory;
  if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
  BOOL allow = FALSE;
  if (FAILED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow,
                                          sizeof(allow)))) {
    return false;
  }
  return allow != FALSE;
}

}  // namespace cap
