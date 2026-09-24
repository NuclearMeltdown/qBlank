#include "render/vulkan_device.h"

#include <algorithm>
#include <cstring>

#include "common.h"
#include "i18n.h"

namespace cap {

const char* VkResultName(VkResult result) {
  switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    default: break;
  }
  static thread_local char text[32];
  snprintf(text, sizeof(text), "VkResult %d", (int)result);
  return text;
}

namespace {

bool HasExtension(const std::vector<VkExtensionProperties>& list, const char* name) {
  for (const VkExtensionProperties& e : list) {
    if (strcmp(e.extensionName, name) == 0) return true;
  }
  return false;
}

std::vector<VkExtensionProperties> DeviceExtensions(VkPhysicalDevice physical) {
  uint32_t count = 0;
  vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> list(count);
  if (count) vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, list.data());
  list.resize(count);
  return list;
}

// Which stages touch an image in this layout, and how. What a barrier out of
// the layout waits for, and what a barrier into it makes wait.
void LayoutUse(VkImageLayout layout, VkPipelineStageFlags2* stage, VkAccessFlags2* access) {
  switch (layout) {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
      *stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
      *access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
      break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
      *stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      *access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
      break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
      *stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
      *access = VK_ACCESS_2_TRANSFER_READ_BIT;
      break;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
      *stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
      *access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      break;
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
      // Handed to the presentation engine, which the semaphore orders. The
      // stage is the one the semaphore is signalled at: with none, the layout
      // change would not be chained to the signal and the present could read
      // the image before it.
      *stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      *access = VK_ACCESS_2_NONE;
      break;
    case VK_IMAGE_LAYOUT_UNDEFINED:
    default:
      // Nothing to keep. The stage is still the colour output, because a
      // swapchain image comes out of the acquire in this layout and the wait on
      // its semaphore is at that stage: the barrier has to come after it.
      *stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
      *access = VK_ACCESS_2_NONE;
      break;
  }
}

}  // namespace

// ------------------------------------------------------------------- creation

bool VulkanDevice::CreateInstance(const std::vector<const char*>& extensions,
                                  const std::vector<const char*>& optional,
                                  std::string* error) {
  // Asked through the loader rather than called: a loader from before 1.1 does
  // not export it, and a missing import would take the program down.
  uint32_t loaderVersion = VK_API_VERSION_1_0;
  auto enumerateVersion = (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(
      VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
  if (enumerateVersion) enumerateVersion(&loaderVersion);
  if (loaderVersion < VK_API_VERSION_1_3) {
    char version[32];
    snprintf(version, sizeof(version), "%u.%u", VK_API_VERSION_MAJOR(loaderVersion),
             VK_API_VERSION_MINOR(loaderVersion));
    return ReportError(error, CAP_SAID(std::string(T("Vulkan 1.3 fehlt, installiert ist ",
                                                     "Vulkan 1.3 is missing, installed is ")) +
                                       version));
  }

  uint32_t count = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> available(count);
  if (count) vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());
  available.resize(count);

  std::vector<const char*> enabled;
  for (const char* name : extensions) {
    if (!HasExtension(available, name)) {
      return ReportError(error, CAP_SAID(std::string(T("Vulkan-Erweiterung fehlt: ",
                                                       "Vulkan extension missing: ")) +
                                         name));
    }
    enabled.push_back(name);
  }
  for (const char* name : optional) {
    if (HasExtension(available, name)) enabled.push_back(name);
  }

  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "qBlank";
  app.applicationVersion = 1;
  app.pEngineName = "qBlank";
  app.apiVersion = VK_API_VERSION_1_3;

  VkInstanceCreateInfo info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  info.pApplicationInfo = &app;
  info.enabledExtensionCount = (uint32_t)enabled.size();
  info.ppEnabledExtensionNames = enabled.data();

  const VkResult result = vkCreateInstance(&info, nullptr, &instance_);
  if (result != VK_SUCCESS) {
    instance_ = VK_NULL_HANDLE;
    return ReportError(error, CAP_SAID(std::string(T("Vulkan-Instanz fehlgeschlagen: ",
                                                     "Vulkan instance failed: ")) +
                                       VkResultName(result)));
  }
  instanceExtensions_.assign(enabled.begin(), enabled.end());
  return true;
}

bool VulkanDevice::instanceHas(const char* extension) const {
  for (const std::string& name : instanceExtensions_) {
    if (name == extension) return true;
  }
  return false;
}

VkResult VulkanDevice::WaitForPresent(VkSwapchainKHR swapchain, uint64_t presentId,
                                      uint64_t timeoutNs) {
  if (!waitForPresent_) return VK_ERROR_EXTENSION_NOT_PRESENT;
  return waitForPresent_(device_, swapchain, presentId, timeoutNs);
}

bool VulkanDevice::CreateDevice(VkSurfaceKHR surface, std::string* error) {
  surface_ = surface;
  uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance_, &count, nullptr);
  std::vector<VkPhysicalDevice> devices(count);
  if (count) vkEnumeratePhysicalDevices(instance_, &count, devices.data());
  devices.resize(count);

  // The best one that can do all of it. Anything that cannot is left out
  // rather than opened with less: display.h would rather fail than be slower.
  int bestScore = -1;
  std::string rejected;
  for (VkPhysicalDevice candidate : devices) {
    VkPhysicalDeviceProperties props = {};
    vkGetPhysicalDeviceProperties(candidate, &props);
    const auto reject = [&](const char* why) {
      if (!rejected.empty()) rejected += "; ";
      rejected += std::string(props.deviceName) + ": " + why;
    };
    if (props.apiVersion < VK_API_VERSION_1_3) {
      reject("no Vulkan 1.3");
      continue;
    }
    if (!HasExtension(DeviceExtensions(candidate), VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
      reject("no swapchain");
      continue;
    }

    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &f13;
    vkGetPhysicalDeviceFeatures2(candidate, &features);
    if (!f13.dynamicRendering || !f13.synchronization2 || !f13.robustImageAccess) {
      reject("missing dynamic rendering, synchronization2 or robust image access");
      continue;
    }

    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    if (familyCount) {
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
    }
    int family = -1;
    for (uint32_t i = 0; i < familyCount; ++i) {
      if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
      VkBool32 present = VK_FALSE;
      if (surface) vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &present);
      if (present) {
        family = (int)i;
        break;
      }
    }
    if (family < 0) {
      reject("cannot present to this window");
      continue;
    }

    int score = 0;
    switch (props.deviceType) {
      case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: score = 4; break;
      case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score = 3; break;
      case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: score = 2; break;
      case VK_PHYSICAL_DEVICE_TYPE_CPU: score = 1; break;
      default: score = 0; break;
    }
    if (score > bestScore) {
      bestScore = score;
      physical_ = candidate;
      properties_ = props;
      queueFamily_ = (uint32_t)family;
    }
  }
  if (!physical_) {
    if (rejected.empty()) rejected = "no Vulkan device";
    CAP_WARN("Vulkan: %s", rejected.c_str());
    return ReportError(error, CAP_SAID(std::string(T("Kein passendes Vulkan-Gerät: ",
                                                     "No suitable Vulkan device: ")) +
                                       rejected));
  }
  vkGetPhysicalDeviceMemoryProperties(physical_, &memory_);

  // What is only nice to have. Present wait lets a swapchain of more than two
  // images still keep to one frame ahead (display_vulkan.cpp); the second
  // robustness makes a read outside an image zero in every component, which is
  // exactly what Direct3D 11 answers and what the shaders were written for.
  const std::vector<VkExtensionProperties> available = DeviceExtensions(physical_);
  std::vector<const char*> enabled = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

  VkPhysicalDevicePresentIdFeaturesKHR presentId = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
  VkPhysicalDevicePresentWaitFeaturesKHR presentWait = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
  VkPhysicalDeviceRobustness2FeaturesEXT robustness2 = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
  const bool havePresentWait = HasExtension(available, VK_KHR_PRESENT_ID_EXTENSION_NAME) &&
                               HasExtension(available, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
  const bool haveRobustness2 = HasExtension(available, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
  {
    VkPhysicalDeviceFeatures2 query = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    void** tail = &query.pNext;
    if (havePresentWait) {
      *tail = &presentId;
      presentId.pNext = &presentWait;
      tail = &presentWait.pNext;
    }
    if (haveRobustness2) {
      *tail = &robustness2;
      tail = &robustness2.pNext;
    }
    vkGetPhysicalDeviceFeatures2(physical_, &query);
  }
  presentWait_ = havePresentWait && presentId.presentId && presentWait.presentWait;
  const bool robust2 = haveRobustness2 && robustness2.robustImageAccess2;

  VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  f13.dynamicRendering = VK_TRUE;
  f13.synchronization2 = VK_TRUE;
  f13.robustImageAccess = VK_TRUE;
  VkPhysicalDeviceFeatures2 features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  features.pNext = &f13;
  void** tail = &f13.pNext;
  VkPhysicalDevicePresentIdFeaturesKHR wantId = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
  VkPhysicalDevicePresentWaitFeaturesKHR wantWait = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
  VkPhysicalDeviceRobustness2FeaturesEXT wantRobust = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
  if (presentWait_) {
    enabled.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
    enabled.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
    wantId.presentId = VK_TRUE;
    wantWait.presentWait = VK_TRUE;
    *tail = &wantId;
    wantId.pNext = &wantWait;
    tail = &wantWait.pNext;
  }
  if (robust2) {
    enabled.push_back(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
    wantRobust.robustImageAccess2 = VK_TRUE;
    *tail = &wantRobust;
    tail = &wantRobust.pNext;
  }

  const float priority = 1.0f;
  VkDeviceQueueCreateInfo queueInfo = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queueInfo.queueFamilyIndex = queueFamily_;
  queueInfo.queueCount = 1;
  queueInfo.pQueuePriorities = &priority;

  VkDeviceCreateInfo info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  info.pNext = &features;
  info.queueCreateInfoCount = 1;
  info.pQueueCreateInfos = &queueInfo;
  info.enabledExtensionCount = (uint32_t)enabled.size();
  info.ppEnabledExtensionNames = enabled.data();

  VkResult result = vkCreateDevice(physical_, &info, nullptr, &device_);
  if (result != VK_SUCCESS) {
    device_ = VK_NULL_HANDLE;
    return ReportError(error, CAP_SAID(std::string(T("Vulkan-Gerät fehlgeschlagen: ",
                                                     "Vulkan device failed: ")) +
                                       VkResultName(result)));
  }
  vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
  if (presentWait_) {
    // Not among what the loader exports, so asked of the device.
    waitForPresent_ =
        (PFN_vkWaitForPresentKHR)vkGetDeviceProcAddr(device_, "vkWaitForPresentKHR");
    presentWait_ = waitForPresent_ != nullptr;
  }

  VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  poolInfo.queueFamilyIndex = queueFamily_;
  VkCommandBufferAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandBufferCount = 1;
  VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if ((result = vkCreateCommandPool(device_, &poolInfo, nullptr, &pool_)) != VK_SUCCESS ||
      ((allocInfo.commandPool = pool_),
       (result = vkAllocateCommandBuffers(device_, &allocInfo, &cmd_)) != VK_SUCCESS) ||
      (result = vkCreateFence(device_, &fenceInfo, nullptr, &fence_)) != VK_SUCCESS) {
    return ReportError(error, CAP_SAID(std::string(T("Vulkan-Kommandopuffer fehlgeschlagen: ",
                                                     "Vulkan command buffer failed: ")) +
                                       VkResultName(result)));
  }

  CAP_LOG("Vulkan device: %s, API %u.%u.%u, driver 0x%X, present wait %s, robust image access %s",
          properties_.deviceName, VK_API_VERSION_MAJOR(properties_.apiVersion),
          VK_API_VERSION_MINOR(properties_.apiVersion), VK_API_VERSION_PATCH(properties_.apiVersion),
          properties_.driverVersion, presentWait_ ? "yes" : "no", robust2 ? "2" : "1");
  return true;
}

void VulkanDevice::Destroy() {
  if (device_) {
    if (recording_) {
      vkEndCommandBuffer(cmd_);
      recording_ = false;
    }
    vkDeviceWaitIdle(device_);
    completedSerial_ = submittedSerial_;
    for (Deferred& d : deferred_) d.destroy(device_);
    deferred_.clear();
    if (fence_) vkDestroyFence(device_, fence_, nullptr);
    if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
    vkDestroyDevice(device_, nullptr);
  }
  deferred_.clear();
  fence_ = VK_NULL_HANDLE;
  pool_ = VK_NULL_HANDLE;
  cmd_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
  queue_ = VK_NULL_HANDLE;
  physical_ = VK_NULL_HANDLE;
  presentWait_ = false;
  waitForPresent_ = nullptr;
  acquireWait_ = VK_NULL_HANDLE;
  backBuffer_ = nullptr;
  uiTarget_ = nullptr;
  if (surface_ && instance_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
  surface_ = VK_NULL_HANDLE;
  if (instance_) vkDestroyInstance(instance_, nullptr);
  instance_ = VK_NULL_HANDLE;
  instanceExtensions_.clear();
}

// ------------------------------------------------------------------ recording

VkCommandBuffer VulkanDevice::Commands() {
  if (!device_) return VK_NULL_HANDLE;
  if (recording_) return cmd_;

  // The one frame in flight: whatever went before has to be finished before its
  // buffer, and everything it read, can be used again.
  if (completedSerial_ < submittedSerial_) {
    vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
    completedSerial_ = submittedSerial_;
  }
  RunDeferred();

  vkResetCommandBuffer(cmd_, 0);
  VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(cmd_, &begin) != VK_SUCCESS) return VK_NULL_HANDLE;
  recording_ = true;
  return cmd_;
}

bool VulkanDevice::Submit(VkSemaphore signal) {
  if (!device_) return false;
  if (!recording_) {
    if (!acquireWait_ && !signal) return true;
    // Nothing recorded, but a semaphore still has to be waited on or signalled.
    if (!Commands()) return false;
  }
  vkEndCommandBuffer(cmd_);
  recording_ = false;
  vkResetFences(device_, 1, &fence_);

  VkSemaphoreSubmitInfo wait = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  wait.semaphore = acquireWait_;
  wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSemaphoreSubmitInfo done = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  done.semaphore = signal;
  done.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  VkCommandBufferSubmitInfo commands = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
  commands.commandBuffer = cmd_;

  VkSubmitInfo2 submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  submit.waitSemaphoreInfoCount = acquireWait_ ? 1u : 0u;
  submit.pWaitSemaphoreInfos = &wait;
  submit.commandBufferInfoCount = 1;
  submit.pCommandBufferInfos = &commands;
  submit.signalSemaphoreInfoCount = signal ? 1u : 0u;
  submit.pSignalSemaphoreInfos = &done;

  const VkResult result = vkQueueSubmit2(queue_, 1, &submit, fence_);
  acquireWait_ = VK_NULL_HANDLE;
  ++submittedSerial_;
  if (result != VK_SUCCESS) {
    // The fence will never be signalled for this one; waiting on it would hang.
    completedSerial_ = submittedSerial_;
    CAP_ERR("Vulkan submit failed: %s", VkResultName(result));
    return false;
  }
  return true;
}

bool VulkanDevice::Flush() {
  const bool ok = Submit();
  if (device_ && completedSerial_ < submittedSerial_) {
    vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
    completedSerial_ = submittedSerial_;
  }
  RunDeferred();
  return ok;
}

void VulkanDevice::WaitIdle() {
  if (!device_) return;
  vkDeviceWaitIdle(device_);
  completedSerial_ = submittedSerial_;
  // What the buffer being recorded uses stays: it has not been submitted yet.
  RunDeferred();
}

bool VulkanDevice::Completed(uint64_t serial) {
  if (serial <= completedSerial_) return true;
  if (serial > submittedSerial_ || !device_) return false;
  if (vkGetFenceStatus(device_, fence_) != VK_SUCCESS) return false;
  completedSerial_ = submittedSerial_;
  return true;
}

bool VulkanDevice::Wait(uint64_t serial) {
  if (!device_) return false;
  if (serial > submittedSerial_) return Flush();
  if (serial > completedSerial_) {
    // One submission in flight, so the fence is the one that serial signals.
    vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
    completedSerial_ = submittedSerial_;
    RunDeferred();
  }
  return true;
}

void VulkanDevice::Defer(std::function<void(VkDevice)> destroy) {
  if (!device_) return;
  // While nothing is being recorded the last submission is the last thing that
  // can have used it.
  const uint64_t serial = recording_ ? RecordingSerial() : submittedSerial_;
  if (serial <= completedSerial_ && !recording_) {
    destroy(device_);
    return;
  }
  deferred_.push_back({serial, std::move(destroy)});
}

void VulkanDevice::RunDeferred() {
  if (deferred_.empty()) return;
  std::vector<Deferred> keep;
  // Moved out first: a destroy is allowed to defer another one.
  std::vector<Deferred> list;
  list.swap(deferred_);
  for (Deferred& d : list) {
    if (d.serial <= completedSerial_) {
      d.destroy(device_);
    } else {
      keep.push_back(std::move(d));
    }
  }
  for (Deferred& d : deferred_) keep.push_back(std::move(d));
  deferred_.swap(keep);
}

void VulkanDevice::HostReadBarrier(VkCommandBuffer cmd) {
  VkMemoryBarrier2 barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
  VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.memoryBarrierCount = 1;
  dep.pMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(cmd, &dep);
}

void VulkanDevice::Transition(VkCommandBuffer cmd, VulkanImage* image, VkImageLayout layout) {
  if (!image || !image->image || !cmd) return;
  // Reads after reads in the same layout need nothing between them.
  if (image->layout == layout && (layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
                                  layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)) {
    return;
  }

  VkImageMemoryBarrier2 barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  LayoutUse(image->layout, &barrier.srcStageMask, &barrier.srcAccessMask);
  LayoutUse(layout, &barrier.dstStageMask, &barrier.dstAccessMask);
  // What was only read has nothing to make visible.
  barrier.srcAccessMask &= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
  barrier.oldLayout = image->layout;
  barrier.newLayout = layout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image->image;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.layerCount = 1;

  VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(cmd, &dep);
  image->layout = layout;
}

void VulkanDevice::Clear(VkCommandBuffer cmd, VulkanImage* image, const float color[4]) {
  if (!image || !image->image || !cmd) return;
  Transition(cmd, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

  VkRenderingAttachmentInfo attachment = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  attachment.imageView = image->view;
  attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  memcpy(attachment.clearValue.color.float32, color, sizeof(float) * 4);

  VkRenderingInfo rendering = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea.extent = {(uint32_t)image->width, (uint32_t)image->height};
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachments = &attachment;
  vkCmdBeginRendering(cmd, &rendering);
  vkCmdEndRendering(cmd);
}

// ------------------------------------------------------------------ resources

bool VulkanDevice::FindMemoryType(uint32_t bits, VkMemoryPropertyFlags want,
                                  uint32_t* index) const {
  for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i) {
    if ((bits & (1u << i)) && (memory_.memoryTypes[i].propertyFlags & want) == want) {
      *index = i;
      return true;
    }
  }
  return false;
}

bool VulkanDevice::Allocate(const VkMemoryRequirements& req, VulkanMemory memory,
                            VkDeviceMemory* out, bool* coherent) {
  // In order of preference.
  std::vector<VkMemoryPropertyFlags> wants;
  switch (memory) {
    case VulkanMemory::Device:
      wants = {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0};
      break;
    case VulkanMemory::Upload:
      wants = {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
      break;
    case VulkanMemory::Readback:
      wants = {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT |
                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
      break;
  }
  uint32_t type = 0;
  bool found = false;
  for (VkMemoryPropertyFlags want : wants) {
    if (FindMemoryType(req.memoryTypeBits, want, &type)) {
      found = true;
      break;
    }
  }
  if (!found) return false;

  VkMemoryAllocateInfo info = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  info.allocationSize = req.size;
  info.memoryTypeIndex = type;
  if (vkAllocateMemory(device_, &info, nullptr, out) != VK_SUCCESS) {
    *out = VK_NULL_HANDLE;
    return false;
  }
  if (coherent) {
    *coherent = (memory_.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
  }
  return true;
}

bool VulkanDevice::CreateImage(int width, int height, VkFormat format, VkImageUsageFlags usage,
                               VulkanImage* out) {
  *out = VulkanImage();
  if (!device_) return false;

  VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {(uint32_t)std::max(1, width), (uint32_t)std::max(1, height), 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VulkanImage image;
  image.format = format;
  image.width = (int)info.extent.width;
  image.height = (int)info.extent.height;
  if (vkCreateImage(device_, &info, nullptr, &image.image) != VK_SUCCESS) return false;

  VkMemoryRequirements req = {};
  vkGetImageMemoryRequirements(device_, image.image, &req);
  if (!Allocate(req, VulkanMemory::Device, &image.memory, nullptr) ||
      vkBindImageMemory(device_, image.image, image.memory, 0) != VK_SUCCESS) {
    if (image.memory) vkFreeMemory(device_, image.memory, nullptr);
    vkDestroyImage(device_, image.image, nullptr);
    return false;
  }

  VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = image.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = format;
  view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  view.subresourceRange.levelCount = 1;
  view.subresourceRange.layerCount = 1;
  if (vkCreateImageView(device_, &view, nullptr, &image.view) != VK_SUCCESS) {
    vkFreeMemory(device_, image.memory, nullptr);
    vkDestroyImage(device_, image.image, nullptr);
    return false;
  }
  *out = image;
  return true;
}

void VulkanDevice::DestroyImage(VulkanImage* image) {
  if (!image || !image->image) return;
  const VulkanImage doomed = *image;
  *image = VulkanImage();
  if (backBuffer_ == image) backBuffer_ = nullptr;
  if (uiTarget_ == image) uiTarget_ = nullptr;
  Defer([doomed](VkDevice device) {
    if (doomed.view) vkDestroyImageView(device, doomed.view, nullptr);
    vkDestroyImage(device, doomed.image, nullptr);
    if (doomed.memory) vkFreeMemory(device, doomed.memory, nullptr);
  });
}

bool VulkanDevice::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VulkanMemory memory,
                                VulkanBuffer* out) {
  *out = VulkanBuffer();
  if (!device_) return false;

  VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = std::max<VkDeviceSize>(size, 4);
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VulkanBuffer buffer;
  buffer.size = info.size;
  if (vkCreateBuffer(device_, &info, nullptr, &buffer.buffer) != VK_SUCCESS) return false;

  VkMemoryRequirements req = {};
  vkGetBufferMemoryRequirements(device_, buffer.buffer, &req);
  if (!Allocate(req, memory, &buffer.memory, &buffer.coherent) ||
      vkBindBufferMemory(device_, buffer.buffer, buffer.memory, 0) != VK_SUCCESS) {
    if (buffer.memory) vkFreeMemory(device_, buffer.memory, nullptr);
    vkDestroyBuffer(device_, buffer.buffer, nullptr);
    return false;
  }
  if (memory != VulkanMemory::Device) {
    void* mapped = nullptr;
    if (vkMapMemory(device_, buffer.memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
      vkFreeMemory(device_, buffer.memory, nullptr);
      vkDestroyBuffer(device_, buffer.buffer, nullptr);
      return false;
    }
    buffer.mapped = (uint8_t*)mapped;
  }
  *out = buffer;
  return true;
}

void VulkanDevice::DestroyBuffer(VulkanBuffer* buffer) {
  if (!buffer || !buffer->buffer) return;
  const VulkanBuffer doomed = *buffer;
  *buffer = VulkanBuffer();
  Defer([doomed](VkDevice device) {
    vkDestroyBuffer(device, doomed.buffer, nullptr);
    // Unmapped with the memory.
    vkFreeMemory(device, doomed.memory, nullptr);
  });
}

void VulkanDevice::Invalidate(const VulkanBuffer& buffer) {
  if (!device_ || !buffer.memory || buffer.coherent) return;
  VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
  range.memory = buffer.memory;
  range.size = VK_WHOLE_SIZE;
  vkInvalidateMappedMemoryRanges(device_, 1, &range);
}

}  // namespace cap
