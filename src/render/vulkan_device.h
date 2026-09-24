#pragma once

// The Vulkan device the display and the passes share, and the little they need
// on top of the raw API: one command buffer that is always the one being
// recorded, one fence behind it, and destruction that waits for that fence.
//
// One frame in flight, as display.h asks. Commands() waits for the previous
// submission before it hands out the buffer again, so drawing never runs more
// than one frame in front of the GPU, and nothing here needs a second set of
// anything.
//
// Neutral: the platform only comes in through the surface, which the caller
// makes (display_vulkan_win32.cpp).

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cap {

// One image with its view and memory. `layout` is what the last recorded use
// left it in; Transition keeps it.
struct VulkanImage {
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;  // null for a swapchain image
  VkFormat format = VK_FORMAT_UNDEFINED;
  int width = 0;
  int height = 0;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;

  explicit operator bool() const { return image != VK_NULL_HANDLE; }
};

struct VulkanBuffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize size = 0;
  uint8_t* mapped = nullptr;  // host visible buffers stay mapped for their life
  bool coherent = true;

  explicit operator bool() const { return buffer != VK_NULL_HANDLE; }
};

// What a buffer is for, which decides the memory it lives in.
enum class VulkanMemory {
  Device,    // the GPU's own
  Upload,    // written by the CPU, read by the GPU: host visible and coherent
  Readback,  // written by the GPU, read by the CPU: host cached where there is such
};

const char* VkResultName(VkResult result);

class VulkanDevice {
 public:
  VulkanDevice() = default;
  ~VulkanDevice() { Destroy(); }

  VulkanDevice(const VulkanDevice&) = delete;
  VulkanDevice& operator=(const VulkanDevice&) = delete;

  // Vulkan 1.3 with the given instance extensions. Those in `optional` are
  // enabled where the loader has them.
  bool CreateInstance(const std::vector<const char*>& extensions,
                      const std::vector<const char*>& optional, std::string* error);
  // Picks a device that can draw and present to `surface` and opens it with
  // dynamic rendering, synchronization2 and robust image access. Prefers a
  // discrete GPU. Takes the surface over, also when it fails: it has to go
  // after the device and before the instance, which only this knows.
  bool CreateDevice(VkSurfaceKHR surface, std::string* error);
  // Waits for the GPU, runs what was deferred and destroys everything, the
  // surface included. What was made from the surface (the swapchain) is the
  // caller's and goes before this.
  void Destroy();

  VkInstance instance() const { return instance_; }
  VkSurfaceKHR surface() const { return surface_; }
  VkPhysicalDevice physical() const { return physical_; }
  VkDevice device() const { return device_; }
  VkQueue queue() const { return queue_; }
  uint32_t queueFamily() const { return queueFamily_; }
  const VkPhysicalDeviceProperties& properties() const { return properties_; }
  bool instanceHas(const char* extension) const;
  // VK_KHR_present_id and VK_KHR_present_wait, both enabled.
  bool presentWait() const { return presentWait_; }
  // vkWaitForPresentKHR, which the loader does not export.
  VkResult WaitForPresent(VkSwapchainKHR swapchain, uint64_t presentId, uint64_t timeoutNs);

  // ---- recording ----

  // The command buffer being recorded, begun if it was not. Beginning it waits
  // for the previous submission and runs what waited on it.
  VkCommandBuffer Commands();
  bool recording() const { return recording_; }

  // Ends and submits what was recorded, waiting on the acquire set by
  // SetAcquireWait and signalling `signal` if given. False when the queue
  // refused it; the device is then as good as lost.
  bool Submit(VkSemaphore signal = VK_NULL_HANDLE);
  // Submits and waits for the GPU.
  bool Flush();
  void WaitIdle();

  // The next submission waits on this before it writes colour, as the
  // swapchain image just acquired asks.
  void SetAcquireWait(VkSemaphore semaphore) { acquireWait_ = semaphore; }

  // Submissions are counted. The one being recorded is RecordingSerial().
  uint64_t RecordingSerial() const { return submittedSerial_ + 1; }
  // Whether that one has finished on the GPU. Never waits.
  bool Completed(uint64_t serial);
  // Waits until it has, submitting first if it is the one being recorded.
  bool Wait(uint64_t serial);

  // Runs `destroy` once the GPU is done with everything recorded so far.
  void Defer(std::function<void(VkDevice)> destroy);

  // Makes what the transfers of this recording wrote readable by the CPU once
  // it has finished.
  void HostReadBarrier(VkCommandBuffer cmd);

  // ---- resources ----

  // Device local, one mip, one layer, with a view.
  bool CreateImage(int width, int height, VkFormat format, VkImageUsageFlags usage,
                   VulkanImage* out);
  // Deferred until the GPU is done with it; `image` is empty afterwards.
  void DestroyImage(VulkanImage* image);
  bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VulkanMemory memory,
                    VulkanBuffer* out);
  void DestroyBuffer(VulkanBuffer* buffer);
  // Before the CPU reads a readback buffer the GPU wrote.
  void Invalidate(const VulkanBuffer& buffer);

  // Records the barrier that takes `image` from its layout to `layout`, with
  // the stages and accesses each layout implies. Also between two writes in
  // the same layout.
  void Transition(VkCommandBuffer cmd, VulkanImage* image, VkImageLayout layout);
  // Fills a whole image with one colour through a rendering of its own.
  void Clear(VkCommandBuffer cmd, VulkanImage* image, const float color[4]);

  // ---- the frame ----

  // The swapchain image between BeginFrame and EndFrame; null outside.
  VulkanImage* backBuffer() const { return backBuffer_; }
  void SetBackBuffer(VulkanImage* image) { backBuffer_ = image; }
  // Where the interface draws: the passes' own layer while they have one open
  // (render_passes.h, BeginUiLayer), else the back buffer.
  VulkanImage* uiTarget() const { return uiTarget_ ? uiTarget_ : backBuffer_; }
  void SetUiTarget(VulkanImage* image) { uiTarget_ = image; }
  // The format the interface is drawn in: the swapchain's eight bit one. The
  // passes make their layer in it too, so the interface's pipeline stays the
  // same whether it draws into the swapchain or into the layer.
  VkFormat uiFormat() const { return uiFormat_; }
  void SetUiFormat(VkFormat format) { uiFormat_ = format; }

 private:
  bool FindMemoryType(uint32_t bits, VkMemoryPropertyFlags want, uint32_t* index) const;
  bool Allocate(const VkMemoryRequirements& req, VulkanMemory memory, VkDeviceMemory* out,
                bool* coherent);
  void RunDeferred();

  VkInstance instance_ = VK_NULL_HANDLE;
  std::vector<std::string> instanceExtensions_;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties properties_ = {};
  VkPhysicalDeviceMemoryProperties memory_ = {};
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t queueFamily_ = 0;
  bool presentWait_ = false;
  PFN_vkWaitForPresentKHR waitForPresent_ = nullptr;

  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  bool recording_ = false;
  VkSemaphore acquireWait_ = VK_NULL_HANDLE;
  uint64_t submittedSerial_ = 0;
  uint64_t completedSerial_ = 0;

  struct Deferred {
    uint64_t serial;
    std::function<void(VkDevice)> destroy;
  };
  std::vector<Deferred> deferred_;

  VulkanImage* backBuffer_ = nullptr;
  VulkanImage* uiTarget_ = nullptr;
  VkFormat uiFormat_ = VK_FORMAT_B8G8R8A8_UNORM;
};

}  // namespace cap
