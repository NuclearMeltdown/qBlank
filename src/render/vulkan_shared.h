#pragma once

// The yuv pass's half of the pictures handed over on the GPU (shared_frames.h)
// under Vulkan: the recorder's textures imported as images, and what a
// submission carries to take and give back each one's keyed mutex.
//
// Neutral; the import is the platform's (vulkan_shared_win32.cpp).

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace cap {

class SharedFrames;
class VulkanDevice;
struct VulkanImage;

// The device extensions the import needs, enabled where the device has them.
std::vector<const char*> VulkanSharedExtensions();
// Whether `vk` was opened with all of them.
bool VulkanCanShare(const VulkanDevice& vk);

class VulkanSharedImages {
 public:
  virtual ~VulkanSharedImages() = default;

  // Records taking `slot`'s image over from the other API, its old contents
  // dropped, ready to be drawn into whole. The screen's (SharedFrames::bgra)
  // the other way: its contents kept, ready to be copied from.
  virtual VulkanImage* Begin(VkCommandBuffer cmd, int slot) = 0;
  // Records handing it back once drawn, or copied from.
  virtual void End(VkCommandBuffer cmd, int slot) = 0;
  // For VkSubmitInfo2::pNext: the submission takes `slot`'s mutex with
  // `acquireKey`, waiting up to `timeoutMs`, and gives it back with
  // `releaseKey`. Valid until the next call for the same slot.
  virtual const void* SubmitChain(int slot, uint64_t acquireKey, uint64_t releaseKey,
                                  uint32_t timeoutMs) = 0;
  // Drops every handle without destroying it, for when the device went first.
  virtual void Forget() = 0;
};

// Null, with a line in the log, when the device cannot take these textures.
std::unique_ptr<VulkanSharedImages> OpenVulkanShared(VulkanDevice& vk, const SharedFrames& frames);

}  // namespace cap
