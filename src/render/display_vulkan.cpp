#include "render/display_vulkan.h"

#include "render/display_backend.h"
#include "render/display_vulkan_platform.h"

#include "backends/imgui_impl_vulkan.h"
#include "common.h"
#include "i18n.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace cap {

namespace {

// Eight bit and stored as written, as DXGI's B8G8R8A8_UNORM: the picture and
// the interface already are sRGB values. An _SRGB format would encode them a
// second time.
const VkFormat kSdrFormats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
// Half float scRGB: linear, BT.709 primaries, 1.0 = 80 nits, as DXGI's
// R16G16B16A16_FLOAT with G10_NONE_P709.
const VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
const VkColorSpaceKHR kHdrColorSpace = VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;

// How long an acquire may block before the frame is skipped instead. Only
// reached when the system holds the images back, as for a covered window.
const uint64_t kAcquireTimeoutNs = 250000000ull;
// How long the wait for the previous present gives it to reach the screen.
const uint64_t kPresentWaitNs = 100000000ull;

const char* PresentModeName(VkPresentModeKHR mode) {
  switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
    case VK_PRESENT_MODE_FIFO_KHR: return "fifo";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo relaxed";
    default: return "other";
  }
}

void LogUiResult(VkResult result) {
  if (result < 0) CAP_ERR("Vulkan interface: %s", VkResultName(result));
}

// One picture made by CreateUiImage. The UiImage holds it; the display keeps a
// weak list of them, because they have to let go of the device before it goes
// and may well outlive it in whoever asked for them.
struct UiTexture {
  VulkanDevice* device = nullptr;  // null once released
  VulkanImage image;
  VkDescriptorSet set = VK_NULL_HANDLE;

  ~UiTexture() { Release(); }

  void Release() {
    VulkanDevice* d = device;
    if (!d) return;
    device = nullptr;
    // The set may be in the frame being drawn; freed with the image, once the
    // GPU is past it.
    const VkDescriptorSet doomed = set;
    set = VK_NULL_HANDLE;
    if (doomed) d->Defer([doomed](VkDevice) { ImGui_ImplVulkan_RemoveTexture(doomed); });
    d->DestroyImage(&image);
  }
};

}  // namespace

struct VulkanDisplay final : Display::Impl {
  GraphicsApi api() const override { return GraphicsApi::Vulkan; }
  bool Initialize(const Window& window, std::string* error) override;
  void Shutdown() override;
  bool initialized() const override { return device_.device() != VK_NULL_HANDLE; }
  void Resize() override;
  void RefreshDisplayCapability() override;
  bool SetHdrOutput(bool enabled, std::string* error) override;
  bool BeginFrame(const float clearColor[4]) override;
  void EndFrame(bool vsync) override;
  bool GrabBackBuffer(std::vector<uint8_t>* rgba, int* width, int* height) override;

  bool InitUi() override;
  void ShutdownUi() override;
  void NewUiFrame() override;
  void RenderUi(ImDrawData* data) override;
  UiImage CreateUiImage(const uint8_t* rgba, int width, int height) override;

  enum class Outcome { Ready, Later, Failed };

  std::vector<VkSurfaceFormatKHR> SurfaceFormats() const;
  bool SurfaceHas(VkFormat format, VkColorSpaceKHR space) const;
  // The size the swapchain should have now; zero while the window has none.
  bool SurfaceExtent(VkSurfaceCapabilitiesKHR* caps, int* width, int* height) const;
  // A new swapchain for the window's size, the output mode and the vsync
  // setting. Waits for the GPU. Later: the window has no area right now.
  Outcome Recreate(std::string* error);
  // The same in the middle of a frame: what was recorded goes first, and the
  // frame carries on with an image of the new swapchain, cleared again.
  Outcome RecreateNow(std::string* error);
  // The next image, with one retry for a swapchain that went out of date.
  bool Acquire();
  // Makes the acquired image the frame's back buffer and clears it.
  void StartImage();
  void DestroySwapchain();

  const Window* window_ = nullptr;
  VulkanDevice device_;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkFormat sdrFormat_ = VK_FORMAT_UNDEFINED;
  VkFormat format_ = VK_FORMAT_UNDEFINED;
  VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
  VkPresentModeKHR immediateMode_ = VK_PRESENT_MODE_IMMEDIATE_KHR;
  bool transferSrc_ = false;
  std::vector<VulkanImage> images_;
  // One per image, signalled by the frame's submission and waited on by its
  // present. Only ever added to: a present of the old swapchain may still hold
  // one when the chain is recreated.
  std::vector<VkSemaphore> renderDone_;
  // The acquire's. One is enough: the frame before has finished on the GPU,
  // and with it the wait on this, before the next acquire signals it again.
  VkSemaphore acquired_ = VK_NULL_HANDLE;
  uint32_t imageIndex_ = 0;
  bool haveImage_ = false;

  bool vsync_ = false;           // what the next swapchain is made for
  bool swapchainVsync_ = false;  // what the current one was made for
  bool inFrame_ = false;
  bool recreatePending_ = false;
  uint64_t presentId_ = 0;
  uint64_t lastPresentId_ = 0;
  float clear_[4] = {};

  // What Recreate logged last, so a window being dragged larger does not
  // write a line for every size it passes.
  VkFormat loggedFormat_ = VK_FORMAT_UNDEFINED;
  VkPresentModeKHR loggedMode_ = VK_PRESENT_MODE_MAX_ENUM_KHR;
  size_t loggedCount_ = 0;

  bool uiReady_ = false;
  VkFormat uiPipelineFormat_ = VK_FORMAT_UNDEFINED;
  std::vector<std::weak_ptr<UiTexture>> uiImages_;
};

// ------------------------------------------------------------------- creation

bool VulkanDisplay::Initialize(const Window& window, std::string* error) {
  window_ = &window;
  if (!VulkanLoaderPresent(error)) return false;

  // The colour space extension is what makes scRGB possible at all; without it
  // the display simply stays eight bit, as one without HDR does.
  if (!device_.CreateInstance({VK_KHR_SURFACE_EXTENSION_NAME, VulkanSurfaceExtension()},
                              {VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME}, error)) {
    return false;
  }
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  if (!CreateVulkanSurface(device_.instance(), window, &surface, error)) return false;
  if (!device_.CreateDevice(surface, error)) return false;

  for (VkFormat want : kSdrFormats) {
    if (SurfaceHas(want, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)) {
      sdrFormat_ = want;
      break;
    }
  }
  if (sdrFormat_ == VK_FORMAT_UNDEFINED) {
    return ReportError(error, CAP_SAID(T("Vulkan bietet für dieses Fenster kein 8-Bit-Format an.",
                                         "Vulkan offers no 8-bit format for this window.")));
  }
  device_.SetUiFormat(sdrFormat_);

  // Without vsync the picture goes out at once, tearing where it has to. Where
  // the driver cannot do that, mailbox still never waits for a vertical blank;
  // with neither this backend cannot keep what display.h promises.
  uint32_t count = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(device_.physical(), device_.surface(), &count,
                                            nullptr);
  std::vector<VkPresentModeKHR> modes(count);
  if (count) {
    vkGetPhysicalDeviceSurfacePresentModesKHR(device_.physical(), device_.surface(), &count,
                                              modes.data());
  }
  modes.resize(count);
  const auto has = [&](VkPresentModeKHR mode) {
    return std::find(modes.begin(), modes.end(), mode) != modes.end();
  };
  if (has(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
    immediateMode_ = VK_PRESENT_MODE_IMMEDIATE_KHR;
  } else if (has(VK_PRESENT_MODE_MAILBOX_KHR)) {
    immediateMode_ = VK_PRESENT_MODE_MAILBOX_KHR;
    CAP_WARN("Vulkan: no immediate present mode, using mailbox");
  } else {
    return ReportError(error, CAP_SAID(T("Vulkan kann in diesem Fenster nicht ohne VSync "
                                         "darstellen.",
                                         "Vulkan cannot present without vsync in this window.")));
  }

  VkSemaphoreCreateInfo semaphoreInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  const VkResult result = vkCreateSemaphore(device_.device(), &semaphoreInfo, nullptr, &acquired_);
  if (result != VK_SUCCESS) {
    acquired_ = VK_NULL_HANDLE;
    return ReportError(error, CAP_SAID(std::string(T("Vulkan-Semaphore fehlgeschlagen: ",
                                                     "Vulkan semaphore failed: ")) +
                                       VkResultName(result)));
  }

  tearingSupported_ = VulkanTearingSupported();
  RefreshDisplayCapability();

  if (Recreate(error) == Outcome::Failed) return false;

  CAP_LOG("Vulkan ready: tearing %s, %s", tearingSupported_ ? "supported" : "not supported",
          device_.presentWait() ? "present wait" : "no present wait");
  return true;
}

void VulkanDisplay::Shutdown() {
  if (device_.device()) {
    // A frame left open is dropped rather than presented.
    device_.Flush();
    device_.WaitIdle();
  }
  ShutdownUi();
  inFrame_ = false;
  haveImage_ = false;
  device_.SetBackBuffer(nullptr);
  device_.SetUiTarget(nullptr);
  DestroySwapchain();
  if (device_.device()) {
    for (VkSemaphore s : renderDone_) vkDestroySemaphore(device_.device(), s, nullptr);
    if (acquired_) vkDestroySemaphore(device_.device(), acquired_, nullptr);
  }
  renderDone_.clear();
  acquired_ = VK_NULL_HANDLE;
  // The device takes the surface and the instance with it.
  device_.Destroy();
  window_ = nullptr;
  width_ = 0;
  height_ = 0;
  recreatePending_ = false;
  lastPresentId_ = 0;
}

void VulkanDisplay::DestroySwapchain() {
  if (device_.device()) {
    for (VulkanImage& image : images_) {
      if (image.view) vkDestroyImageView(device_.device(), image.view, nullptr);
    }
    if (swapchain_) vkDestroySwapchainKHR(device_.device(), swapchain_, nullptr);
  }
  images_.clear();
  swapchain_ = VK_NULL_HANDLE;
}

// ------------------------------------------------------------------ swapchain

std::vector<VkSurfaceFormatKHR> VulkanDisplay::SurfaceFormats() const {
  uint32_t count = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(device_.physical(), device_.surface(), &count, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(count);
  if (count) {
    vkGetPhysicalDeviceSurfaceFormatsKHR(device_.physical(), device_.surface(), &count,
                                         formats.data());
  }
  formats.resize(count);
  return formats;
}

bool VulkanDisplay::SurfaceHas(VkFormat format, VkColorSpaceKHR space) const {
  for (const VkSurfaceFormatKHR& f : SurfaceFormats()) {
    if (f.format == format && f.colorSpace == space) return true;
  }
  return false;
}

bool VulkanDisplay::SurfaceExtent(VkSurfaceCapabilitiesKHR* caps, int* width, int* height) const {
  *width = 0;
  *height = 0;
  if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device_.physical(), device_.surface(), caps) !=
      VK_SUCCESS) {
    return false;
  }
  if (caps->currentExtent.width == 0xFFFFFFFFu) {
    // The surface leaves the size to the swapchain: the window's, then.
    int w = 0, h = 0;
    if (window_) VulkanClientSize(*window_, &w, &h);
    if (w <= 0 || h <= 0) return true;
    *width = std::clamp<int>(w, (int)caps->minImageExtent.width, (int)caps->maxImageExtent.width);
    *height =
        std::clamp<int>(h, (int)caps->minImageExtent.height, (int)caps->maxImageExtent.height);
  } else {
    *width = (int)caps->currentExtent.width;
    *height = (int)caps->currentExtent.height;
  }
  return true;
}

VulkanDisplay::Outcome VulkanDisplay::Recreate(std::string* error) {
  recreatePending_ = true;
  if (!device_.device()) return Outcome::Failed;

  VkSurfaceCapabilitiesKHR caps = {};
  int w = 0, h = 0;
  if (!SurfaceExtent(&caps, &w, &h)) {
    ReportError(error, CAP_SAID(T("Vulkan kennt die Fläche des Fensters nicht mehr.",
                                  "Vulkan lost track of the window's surface.")));
    return Outcome::Failed;
  }
  if (w <= 0 || h <= 0) return Outcome::Later;  // minimised

  // Nothing may still use the old images, nor the pipelines built for them.
  device_.Flush();
  device_.WaitIdle();

  const VkFormat format = hdrOutput_ ? kHdrFormat : sdrFormat_;
  const VkColorSpaceKHR space = hdrOutput_ ? kHdrColorSpace : VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  const VkPresentModeKHR mode = vsync_ ? VK_PRESENT_MODE_FIFO_KHR : immediateMode_;

  // Two, as the DXGI chain has, unless the driver will not go that low. More
  // than two keep to one frame ahead only through the present wait in Acquire.
  uint32_t count = std::max(2u, caps.minImageCount);
  if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);

  VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  transferSrc_ = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
  if (transferSrc_) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

  VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  if (!(caps.supportedCompositeAlpha & alpha)) {
    for (uint32_t bit = 1; bit <= VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR; bit <<= 1) {
      if (caps.supportedCompositeAlpha & bit) {
        alpha = (VkCompositeAlphaFlagBitsKHR)bit;
        break;
      }
    }
  }

  VkSwapchainCreateInfoKHR info = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  info.surface = device_.surface();
  info.minImageCount = count;
  info.imageFormat = format;
  info.imageColorSpace = space;
  info.imageExtent = {(uint32_t)w, (uint32_t)h};
  info.imageArrayLayers = 1;
  info.imageUsage = usage;
  info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.preTransform = caps.currentTransform;
  info.compositeAlpha = alpha;
  info.presentMode = mode;
  info.clipped = VK_TRUE;
  info.oldSwapchain = swapchain_;

  VkSwapchainKHR created = VK_NULL_HANDLE;
  const VkResult result = vkCreateSwapchainKHR(device_.device(), &info, nullptr, &created);
  // The old chain is retired either way, so it goes either way.
  device_.SetBackBuffer(nullptr);
  haveImage_ = false;
  DestroySwapchain();
  lastPresentId_ = 0;
  if (result != VK_SUCCESS) {
    ReportError(error, CAP_SAID(std::string(T("Vulkan-Swapchain fehlgeschlagen: ",
                                              "Vulkan swapchain failed: ")) +
                                VkResultName(result)));
    return Outcome::Failed;
  }
  swapchain_ = created;

  uint32_t imageCount = 0;
  vkGetSwapchainImagesKHR(device_.device(), swapchain_, &imageCount, nullptr);
  std::vector<VkImage> handles(imageCount);
  if (imageCount) vkGetSwapchainImagesKHR(device_.device(), swapchain_, &imageCount, handles.data());
  images_.resize(imageCount);
  for (uint32_t i = 0; i < imageCount; ++i) {
    VulkanImage& image = images_[i];
    image.image = handles[i];
    image.format = format;
    image.width = w;
    image.height = h;
    image.layout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = handles[i];
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    const VkResult viewResult = vkCreateImageView(device_.device(), &view, nullptr, &image.view);
    if (viewResult != VK_SUCCESS) {
      image.view = VK_NULL_HANDLE;
      DestroySwapchain();
      ReportError(error, CAP_SAID(std::string(T("Vulkan-Bildansicht fehlgeschlagen: ",
                                                "Vulkan image view failed: ")) +
                                  VkResultName(viewResult)));
      return Outcome::Failed;
    }
  }
  while (renderDone_.size() < images_.size()) {
    VkSemaphoreCreateInfo semaphoreInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore semaphore = VK_NULL_HANDLE;
    const VkResult semaphoreResult =
        vkCreateSemaphore(device_.device(), &semaphoreInfo, nullptr, &semaphore);
    if (semaphoreResult != VK_SUCCESS) {
      DestroySwapchain();
      ReportError(error, CAP_SAID(std::string(T("Vulkan-Semaphore fehlgeschlagen: ",
                                                "Vulkan semaphore failed: ")) +
                                  VkResultName(semaphoreResult)));
      return Outcome::Failed;
    }
    renderDone_.push_back(semaphore);
  }

  width_ = w;
  height_ = h;
  swapchainVsync_ = vsync_;
  presentMode_ = mode;
  format_ = format;
  recreatePending_ = false;

  if (format != loggedFormat_ || mode != loggedMode_ || images_.size() != loggedCount_) {
    loggedFormat_ = format;
    loggedMode_ = mode;
    loggedCount_ = images_.size();
    CAP_LOG("Vulkan swapchain: %dx%d, %u images, %s, %s", w, h, (unsigned)images_.size(),
            PresentModeName(mode), hdrOutput_ ? "scRGB" : "8 bit");
    if (images_.size() > 2 && !device_.presentWait()) {
      CAP_WARN("Vulkan: %u images without present wait, the picture may run a frame late",
               (unsigned)images_.size());
    }
  }
  return Outcome::Ready;
}

VulkanDisplay::Outcome VulkanDisplay::RecreateNow(std::string* error) {
  // The acquire's semaphore is only waited on by a submission; this one takes
  // it, together with what the frame recorded so far.
  if (inFrame_) device_.Submit();
  const Outcome outcome = Recreate(error);
  if (inFrame_ && outcome == Outcome::Ready && device_.Commands() && Acquire()) StartImage();
  return outcome;
}

bool VulkanDisplay::Acquire() {
  // With more than two images the driver would let drawing run ahead of the
  // screen; waiting for the last present to arrive keeps it to one frame, as
  // two images do by themselves.
  if (lastPresentId_ && images_.size() > 2 && device_.presentWait()) {
    device_.WaitForPresent(swapchain_, lastPresentId_, kPresentWaitNs);
  }
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!swapchain_) return false;
    const VkResult result = vkAcquireNextImageKHR(device_.device(), swapchain_, kAcquireTimeoutNs,
                                                  acquired_, VK_NULL_HANDLE, &imageIndex_);
    if (result == VK_SUCCESS) return true;
    if (result == VK_SUBOPTIMAL_KHR) {
      // Usable for this frame; the next one gets a chain that fits.
      recreatePending_ = true;
      return true;
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR && attempt == 0) {
      if (Recreate(nullptr) != Outcome::Ready) return false;
      // Recreate submitted what was recorded; the frame needs a buffer again.
      if (!device_.Commands()) return false;
      continue;
    }
    if (result != VK_TIMEOUT && result != VK_NOT_READY) {
      CAP_ERR("Vulkan acquire failed: %s", VkResultName(result));
      recreatePending_ = true;
    }
    return false;
  }
  return false;
}

void VulkanDisplay::StartImage() {
  VulkanImage* image = &images_[imageIndex_];
  // Whatever the image held before is gone, and it is not needed.
  image->layout = VK_IMAGE_LAYOUT_UNDEFINED;
  device_.SetAcquireWait(acquired_);
  device_.SetBackBuffer(image);
  device_.Clear(device_.Commands(), image, clear_);
  haveImage_ = true;
}

void VulkanDisplay::Resize() {
  if (!device_.device()) return;
  VkSurfaceCapabilitiesKHR caps = {};
  int w = 0, h = 0;
  if (!SurfaceExtent(&caps, &w, &h)) return;
  if (w <= 0 || h <= 0) return;                               // minimised
  if (swapchain_ && w == width_ && h == height_) return;      // nothing changed
  // In the middle of a frame the one being drawn is finished on the old chain.
  if (inFrame_) {
    recreatePending_ = true;
    return;
  }
  Recreate(nullptr);
}

void VulkanDisplay::RefreshDisplayCapability() {
  display_ = window_ ? VulkanDisplayCapability(*window_) : Display::DisplayCapability();
}

bool VulkanDisplay::SetHdrOutput(bool enabled, std::string* error) {
  if (enabled == hdrOutput_) return true;
  if (!device_.device()) return false;

  // Asked before anything is torn down: a display that cannot take it keeps
  // the chain it has.
  if (enabled && !SurfaceHas(kHdrFormat, kHdrColorSpace)) {
    return ReportError(error, CAP_SAID(T("Diese Anzeige nimmt kein scRGB entgegen.",
                                         "This display will not take scRGB.")));
  }

  hdrOutput_ = enabled;
  if (RecreateNow(error) == Outcome::Failed) {
    hdrOutput_ = !enabled;
    RecreateNow(nullptr);
    return false;
  }
  CAP_LOG("Display switched to %s", enabled ? "scRGB (HDR)" : "sRGB");
  return true;
}

// ---------------------------------------------------------------------- frame

bool VulkanDisplay::BeginFrame(const float clearColor[4]) {
  if (!device_.device() || inFrame_) return false;
  memcpy(clear_, clearColor, sizeof(clear_));

  if ((recreatePending_ || !swapchain_) && Recreate(nullptr) != Outcome::Ready) return false;
  // Waits for the frame before to finish on the GPU: the one frame in flight.
  if (!device_.Commands()) return false;
  if (!Acquire()) return false;

  inFrame_ = true;
  StartImage();
  return true;
}

void VulkanDisplay::EndFrame(bool vsync) {
  // A change of vsync is a new present mode, which takes a new chain. The frame
  // going out now still goes out in the old one.
  vsync_ = vsync;
  if (vsync_ != swapchainVsync_) recreatePending_ = true;

  if (!inFrame_) return;
  inFrame_ = false;
  device_.SetUiTarget(nullptr);
  VulkanImage* image = device_.backBuffer();
  device_.SetBackBuffer(nullptr);
  if (!haveImage_ || !image) {
    device_.Submit();
    return;
  }
  haveImage_ = false;

  const uint32_t index = imageIndex_;
  device_.Transition(device_.Commands(), image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  // A submission the queue refused signals nothing, and a present waiting for
  // it would wait forever.
  if (!device_.Submit(renderDone_[index])) {
    recreatePending_ = true;
    return;
  }

  VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  present.waitSemaphoreCount = 1;
  present.pWaitSemaphores = &renderDone_[index];
  present.swapchainCount = 1;
  present.pSwapchains = &swapchain_;
  present.pImageIndices = &index;

  VkPresentIdKHR ids = {VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
  uint64_t id = 0;
  if (device_.presentWait()) {
    id = ++presentId_;
    ids.swapchainCount = 1;
    ids.pPresentIds = &id;
    present.pNext = &ids;
  }

  const VkResult result = vkQueuePresentKHR(device_.queue(), &present);
  if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
    if (id) lastPresentId_ = id;
    if (result == VK_SUBOPTIMAL_KHR) recreatePending_ = true;
  } else if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    recreatePending_ = true;
  } else {
    CAP_ERR("Vulkan present failed: %s", VkResultName(result));
    recreatePending_ = true;
  }
}

bool VulkanDisplay::GrabBackBuffer(std::vector<uint8_t>* rgba, int* width, int* height) {
  if (!rgba) return false;
  if (hdrOutput_) return false;  // scRGB-Float, siehe Kommentar in display.h
  VulkanImage* image = device_.backBuffer();
  if (!image || !transferSrc_) return false;

  const int w = image->width;
  const int h = image->height;
  const size_t rowBytes = (size_t)w * 4;
  VulkanBuffer buffer;
  if (!device_.CreateBuffer((VkDeviceSize)rowBytes * (VkDeviceSize)h,
                            VK_BUFFER_USAGE_TRANSFER_DST_BIT, VulkanMemory::Readback, &buffer)) {
    return false;
  }

  VkCommandBuffer cmd = device_.Commands();
  device_.Transition(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  VkBufferImageCopy region = {};
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.layerCount = 1;
  region.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
  vkCmdCopyImageToBuffer(cmd, image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.buffer, 1,
                         &region);
  device_.HostReadBarrier(cmd);
  const bool ok = device_.Flush();
  if (ok) {
    device_.Invalidate(buffer);
    rgba->resize(rowBytes * (size_t)h);
    const uint8_t* src = buffer.mapped;
    uint8_t* dst = rgba->data();
    // Das Alpha wird gesetzt statt uebernommen: was auf dem Bildschirm deckend
    // aussieht, muss in der Datei deckend sein.
    const bool bgra = image->format == VK_FORMAT_B8G8R8A8_UNORM;
    for (size_t i = 0; i < (size_t)w * (size_t)h; ++i) {
      dst[i * 4 + 0] = src[i * 4 + (bgra ? 2 : 0)];
      dst[i * 4 + 1] = src[i * 4 + 1];
      dst[i * 4 + 2] = src[i * 4 + (bgra ? 0 : 2)];
      dst[i * 4 + 3] = 0xFF;
    }
    if (width) *width = w;
    if (height) *height = h;
  }
  device_.DestroyBuffer(&buffer);
  return ok;
}

// ------------------------------------------------------------------ interface

bool VulkanDisplay::InitUi() {
  if (!device_.device() || uiReady_) return uiReady_;

  uiPipelineFormat_ = device_.uiFormat();
  ImGui_ImplVulkan_InitInfo info = {};
  info.ApiVersion = VK_API_VERSION_1_3;
  info.Instance = device_.instance();
  info.PhysicalDevice = device_.physical();
  info.Device = device_.device();
  info.QueueFamily = device_.queueFamily();
  info.Queue = device_.queue();
  // The font atlas and the handful of pictures CreateUiImage makes.
  info.DescriptorPoolSize = 64;
  // What ImGui keeps per frame in flight: one being drawn, one on the GPU.
  info.MinImageCount = 2;
  info.ImageCount = 2;
  info.UseDynamicRendering = true;
  info.PipelineInfoMain.PipelineRenderingCreateInfo = {
      VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
  info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &uiPipelineFormat_;
  info.CheckVkResultFn = LogUiResult;
  uiReady_ = ImGui_ImplVulkan_Init(&info);
  if (!uiReady_) CAP_ERR("ImGui_ImplVulkan_Init failed");
  return uiReady_;
}

void VulkanDisplay::ShutdownUi() {
  if (!uiReady_) return;
  device_.Flush();
  device_.WaitIdle();
  // Whoever still holds a picture keeps an empty handle; the device and the
  // interface's pool it came from are about to go.
  for (std::weak_ptr<UiTexture>& weak : uiImages_) {
    if (std::shared_ptr<UiTexture> texture = weak.lock()) texture->Release();
  }
  uiImages_.clear();
  device_.WaitIdle();
  ImGui_ImplVulkan_Shutdown();
  uiReady_ = false;
}

void VulkanDisplay::NewUiFrame() {
  if (uiReady_) ImGui_ImplVulkan_NewFrame();
}

void VulkanDisplay::RenderUi(ImDrawData* data) {
  VulkanImage* target = device_.uiTarget();
  if (!uiReady_ || !data || !target) return;

  if (target->format != uiPipelineFormat_) {
    // Only when the chain came back in the other eight bit format; the
    // interface's pipeline is made for one. It is replaced at once, so the GPU
    // must be done with it.
    CAP_LOG("Vulkan interface pipeline rebuilt for format %d", (int)target->format);
    device_.WaitIdle();
    uiPipelineFormat_ = target->format;
    ImGui_ImplVulkan_PipelineInfo pipeline = {};
    pipeline.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    pipeline.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    pipeline.PipelineRenderingCreateInfo.pColorAttachmentFormats = &uiPipelineFormat_;
    ImGui_ImplVulkan_CreateMainPipeline(&pipeline);
  }

  VkCommandBuffer cmd = device_.Commands();
  if (!cmd) return;
  device_.Transition(cmd, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

  VkRenderingAttachmentInfo attachment = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  attachment.imageView = target->view;
  attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo rendering = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea.extent = {(uint32_t)target->width, (uint32_t)target->height};
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachments = &attachment;
  vkCmdBeginRendering(cmd, &rendering);
  ImGui_ImplVulkan_RenderDrawData(data, cmd);
  vkCmdEndRendering(cmd);
}

UiImage VulkanDisplay::CreateUiImage(const uint8_t* rgba, int width, int height) {
  UiImage result;
  if (!uiReady_ || !rgba || width <= 0 || height <= 0) return result;

  auto texture = std::make_shared<UiTexture>();
  if (!device_.CreateImage(width, height, VK_FORMAT_R8G8B8A8_UNORM,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           &texture->image)) {
    return result;
  }
  texture->device = &device_;

  const VkDeviceSize size = (VkDeviceSize)width * (VkDeviceSize)height * 4;
  VulkanBuffer staging;
  if (!device_.CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VulkanMemory::Upload,
                            &staging)) {
    return result;
  }
  memcpy(staging.mapped, rgba, (size_t)size);

  // Recorded into the frame's commands, ahead of anything that draws it.
  VkCommandBuffer cmd = device_.Commands();
  if (!cmd) {
    device_.DestroyBuffer(&staging);
    return result;
  }
  device_.Transition(cmd, &texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkBufferImageCopy region = {};
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.layerCount = 1;
  region.imageExtent = {(uint32_t)width, (uint32_t)height, 1};
  vkCmdCopyBufferToImage(cmd, staging.buffer, texture->image.image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  device_.Transition(cmd, &texture->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  device_.DestroyBuffer(&staging);

  texture->set =
      ImGui_ImplVulkan_AddTexture(texture->image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  if (!texture->set) return result;

  uiImages_.erase(std::remove_if(uiImages_.begin(), uiImages_.end(),
                                 [](const std::weak_ptr<UiTexture>& w) { return w.expired(); }),
                  uiImages_.end());
  uiImages_.push_back(texture);
  // What ImGui::Image is handed is the descriptor set itself; the handle keeps
  // the rest alive.
  result.texture = std::shared_ptr<void>(texture, (void*)texture->set);
  return result;
}

std::unique_ptr<Display::Impl> CreateVulkanDisplay() {
  return std::make_unique<VulkanDisplay>();
}

VulkanDevice* NativeVulkan(const Display& display) {
  const Display::Impl* impl = display.impl();
  if (!impl || impl->api() != GraphicsApi::Vulkan || !impl->initialized()) return nullptr;
  // The passes draw with it, which is not a change to the display.
  return &const_cast<VulkanDisplay*>(static_cast<const VulkanDisplay*>(impl))->device_;
}

}  // namespace cap
