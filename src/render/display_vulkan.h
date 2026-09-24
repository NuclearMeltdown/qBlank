#pragma once

// The Vulkan half of display.h, for the renderer's passes: they draw with the
// same device into the same swapchain image.

#include "render/display.h"
#include "render/vulkan_device.h"

namespace cap {

// Null unless the display is initialised and runs on Vulkan.
VulkanDevice* NativeVulkan(const Display& display);

}  // namespace cap
