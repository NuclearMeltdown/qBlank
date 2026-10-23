// The Windows answers to vulkan_shared.h: the recorder's Direct3D 11 textures,
// opened by their NT handles, and their keyed mutexes taken with a submission.

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include "render/vulkan_shared.h"

#include "common_win32.h"
#include "render/shared_frames.h"
#include "render/vulkan_device.h"

namespace cap {

namespace {

const VkExternalMemoryHandleTypeFlagBits kHandleType =
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;

class Win32SharedImages : public VulkanSharedImages {
 public:
  explicit Win32SharedImages(VulkanDevice& vk) : vk_(&vk) {}
  ~Win32SharedImages() override {
    if (!vk_) return;
    for (const Slot& slot : slots_) {
      if (vk_->chained() == &slot.mutex) vk_->ChainSubmit(nullptr);
    }
    for (Slot& slot : slots_) vk_->DestroyImage(&slot.image);
  }

  bool Open(const SharedFrames& frames);

  VulkanImage* Begin(VkCommandBuffer cmd, int slot) override {
    VulkanImage* image = &slots_[slot].image;
    // What the other side drew is kept only from the layout it is in.
    const VkImageLayout to =
        read_ ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    Barrier(cmd, image, VK_QUEUE_FAMILY_EXTERNAL, vk_->queueFamily(),
            read_ ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED, to);
    image->layout = to;
    return image;
  }

  void End(VkCommandBuffer cmd, int slot) override {
    VulkanImage* image = &slots_[slot].image;
    Barrier(cmd, image, vk_->queueFamily(), VK_QUEUE_FAMILY_EXTERNAL, image->layout,
            VK_IMAGE_LAYOUT_GENERAL);
    // Whatever the other side leaves; the next Begin drops it anyway.
    image->layout = VK_IMAGE_LAYOUT_UNDEFINED;
  }

  const void* SubmitChain(int slot, uint64_t acquireKey, uint64_t releaseKey,
                          uint32_t timeoutMs) override {
    Slot& s = slots_[slot];
    s.acquireKey = acquireKey;
    s.releaseKey = releaseKey;
    s.timeout = timeoutMs;
    s.mutex = {VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR};
    s.mutex.acquireCount = 1;
    s.mutex.pAcquireSyncs = &s.image.memory;
    s.mutex.pAcquireKeys = &s.acquireKey;
    s.mutex.pAcquireTimeouts = &s.timeout;
    s.mutex.releaseCount = 1;
    s.mutex.pReleaseSyncs = &s.image.memory;
    s.mutex.pReleaseKeys = &s.releaseKey;
    return &s.mutex;
  }

  void Forget() override {
    vk_ = nullptr;
    for (Slot& slot : slots_) slot.image = VulkanImage();
  }

 private:
  struct Slot {
    VulkanImage image;
    VkWin32KeyedMutexAcquireReleaseInfoKHR mutex = {};
    uint64_t acquireKey = 0;
    uint64_t releaseKey = 0;
    uint32_t timeout = 0;
  };

  // The queue family transfer an external image needs on the way in and out.
  // The colour output is the only stage that touches it here, or the copy out
  // of a screen's.
  void Barrier(VkCommandBuffer cmd, VulkanImage* image, uint32_t fromFamily, uint32_t toFamily,
               VkImageLayout from, VkImageLayout to) const {
    VkImageMemoryBarrier2 barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    const bool in = fromFamily == VK_QUEUE_FAMILY_EXTERNAL;
    const VkPipelineStageFlags2 stage =
        read_ ? VK_PIPELINE_STAGE_2_COPY_BIT : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkAccessFlags2 access =
        read_ ? VK_ACCESS_2_TRANSFER_READ_BIT : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.srcStageMask = in ? VK_PIPELINE_STAGE_2_NONE : stage;
    barrier.srcAccessMask = in ? VK_ACCESS_2_NONE : access;
    barrier.dstStageMask = in ? stage : VK_PIPELINE_STAGE_2_NONE;
    barrier.dstAccessMask = in ? access : VK_ACCESS_2_NONE;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = fromFamily;
    barrier.dstQueueFamilyIndex = toFamily;
    barrier.image = image->image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
  }

  bool Import(HANDLE handle, VkFormat format, VkImageUsageFlags usage, int width, int height,
              VulkanImage* out);

  VulkanDevice* vk_;
  bool read_ = false;  // the screen's, copied from rather than drawn into
  Slot slots_[SharedFrames::kSlots];
};

bool Win32SharedImages::Import(HANDLE handle, VkFormat format, VkImageUsageFlags usage, int width,
                               int height, VulkanImage* out) {
  const VkDevice device = vk_->device();
  VkExternalMemoryImageCreateInfo external = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
  external.handleTypes = kHandleType;
  VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.pNext = &external;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {(uint32_t)width, (uint32_t)height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VulkanImage image;
  image.format = format;
  image.width = width;
  image.height = height;
  VkResult result = vkCreateImage(device, &info, nullptr, &image.image);
  if (result != VK_SUCCESS) {
    CAP_WARN("Vulkan: shared image failed: %s", VkResultName(result));
    return false;
  }

  VkMemoryRequirements req = {};
  vkGetImageMemoryRequirements(device, image.image, &req);
  // Not exported by the loader. Where it answers, only the types it names can
  // take the handle.
  uint32_t bits = req.memoryTypeBits;
  auto handleProperties = (PFN_vkGetMemoryWin32HandlePropertiesKHR)vkGetDeviceProcAddr(
      device, "vkGetMemoryWin32HandlePropertiesKHR");
  VkMemoryWin32HandlePropertiesKHR props = {VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
  if (handleProperties && handleProperties(device, kHandleType, handle, &props) == VK_SUCCESS &&
      (bits & props.memoryTypeBits)) {
    bits &= props.memoryTypeBits;
  }
  uint32_t type = 0;
  if (!vk_->FindMemoryType(bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
      !vk_->FindMemoryType(bits, 0, &type)) {
    vkDestroyImage(device, image.image, nullptr);
    CAP_WARN("Vulkan: no memory type for the shared image");
    return false;
  }

  // The handle stays the recorder's: importing an NT handle does not take it.
  VkMemoryDedicatedAllocateInfo dedicated = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  dedicated.image = image.image;
  VkImportMemoryWin32HandleInfoKHR import = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
  import.pNext = &dedicated;
  import.handleType = kHandleType;
  import.handle = handle;
  VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.pNext = &import;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = type;
  if ((result = vkAllocateMemory(device, &alloc, nullptr, &image.memory)) != VK_SUCCESS ||
      (result = vkBindImageMemory(device, image.image, image.memory, 0)) != VK_SUCCESS) {
    if (image.memory) vkFreeMemory(device, image.memory, nullptr);
    vkDestroyImage(device, image.image, nullptr);
    CAP_WARN("Vulkan: importing the shared image failed: %s", VkResultName(result));
    return false;
  }

  VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = image.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = format;
  view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  view.subresourceRange.levelCount = 1;
  view.subresourceRange.layerCount = 1;
  if (vkCreateImageView(device, &view, nullptr, &image.view) != VK_SUCCESS) {
    vkFreeMemory(device, image.memory, nullptr);
    vkDestroyImage(device, image.image, nullptr);
    CAP_WARN("Vulkan: shared image view failed");
    return false;
  }
  *out = image;
  return true;
}

bool Win32SharedImages::Open(const SharedFrames& frames) {
  read_ = frames.bgra;
  const VkFormat format = read_ && frames.halfFloat ? VK_FORMAT_R16G16B16A16_SFLOAT
                          : read_                  ? VK_FORMAT_B8G8R8A8_UNORM
                          : frames.sixteenBit     ? VK_FORMAT_R16_UINT
                                                  : VK_FORMAT_R8_UINT;
  const VkImageUsageFlags usage =
      read_ ? VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
            : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

  // Asked first, so a driver that cannot is a line in the log and not an error
  // halfway through.
  VkPhysicalDeviceExternalImageFormatInfo externalInfo = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
  externalInfo.handleType = kHandleType;
  VkPhysicalDeviceImageFormatInfo2 formatInfo = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
  formatInfo.pNext = &externalInfo;
  formatInfo.format = format;
  formatInfo.type = VK_IMAGE_TYPE_2D;
  formatInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  formatInfo.usage = usage;
  VkExternalImageFormatProperties externalProps = {
      VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
  VkImageFormatProperties2 formatProps = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
  formatProps.pNext = &externalProps;
  const VkResult result =
      vkGetPhysicalDeviceImageFormatProperties2(vk_->physical(), &formatInfo, &formatProps);
  if (result != VK_SUCCESS || !(externalProps.externalMemoryProperties.externalMemoryFeatures &
                                VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
    CAP_WARN("Vulkan: this device cannot import Direct3D 11 textures (%s)", VkResultName(result));
    return false;
  }

  for (int i = 0; i < SharedFrames::kSlots; ++i) {
    if (!Import((HANDLE)frames.handle[i], format, usage, frames.width, frames.rows,
                &slots_[i].image)) {
      return false;
    }
  }
  return true;
}

}  // namespace

std::vector<const char*> VulkanSharedExtensions() {
  return {VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_WIN32_KEYED_MUTEX_EXTENSION_NAME};
}

bool VulkanCanShare(const VulkanDevice& vk) {
  for (const char* name : VulkanSharedExtensions()) {
    if (!vk.deviceHas(name)) return false;
  }
  return true;
}

std::unique_ptr<VulkanSharedImages> OpenVulkanShared(VulkanDevice& vk, const SharedFrames& frames) {
  if (!vk.device() || !VulkanCanShare(vk)) return nullptr;
  auto images = std::make_unique<Win32SharedImages>(vk);
  if (!images->Open(frames)) return nullptr;
  return images;
}

}  // namespace cap
