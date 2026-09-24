#pragma once

// What the Vulkan display needs from the system it runs on. display_vulkan.cpp
// is neutral and asks these; display_vulkan_win32.cpp answers them.

#include <vulkan/vulkan.h>

#include <string>

#include "render/display.h"

namespace cap {

class Window;

// False, with the reason, when there is no Vulkan loader to talk to. Asked
// before the first Vulkan call: the program links the loader late, and a
// missing one must end in the fallback rather than in a crash.
bool VulkanLoaderPresent(std::string* error);

// The instance extension the surface for a window comes from.
const char* VulkanSurfaceExtension();
bool CreateVulkanSurface(VkInstance instance, const Window& window, VkSurfaceKHR* surface,
                         std::string* error);

// The window's client area in pixels, for a surface that does not say.
void VulkanClientSize(const Window& window, int* width, int* height);

// What the screen the window is on can do, asked of the system as the Direct3D
// 11 display asks it.
Display::DisplayCapability VulkanDisplayCapability(const Window& window);

// Whether the system allows tearing. The settings window and the tray menu
// build their own Direct3D 11 chains from this answer, whichever backend the
// main window draws with.
bool VulkanTearingSupported();

}  // namespace cap
