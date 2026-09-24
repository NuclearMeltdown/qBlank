#pragma once

// What the backends behind display.h share. Display itself only forwards: it
// holds one of these, made by the factory for the backend it was asked for.
//
// The fields below are the answers every backend gives the same way, so
// Display can hand them out without asking which backend it is.

#include <memory>
#include <string>
#include <vector>

#include "render/display.h"

namespace cap {

struct Display::Impl {
  virtual ~Impl() = default;

  virtual GraphicsApi api() const = 0;

  virtual bool Initialize(const Window& window, std::string* error) = 0;
  // Safe after a failed Initialize, and more than once.
  virtual void Shutdown() = 0;
  virtual bool initialized() const = 0;
  virtual void Resize() = 0;
  virtual void RefreshDisplayCapability() = 0;
  virtual bool SetHdrOutput(bool enabled, std::string* error) = 0;
  virtual bool BeginFrame(const float clearColor[4]) = 0;
  virtual void EndFrame(bool vsync) = 0;
  virtual bool GrabBackBuffer(std::vector<uint8_t>* rgba, int* width, int* height) = 0;

  virtual bool InitUi() = 0;
  virtual void ShutdownUi() = 0;
  virtual void NewUiFrame() = 0;
  virtual void RenderUi(ImDrawData* data) = 0;
  virtual UiImage CreateUiImage(const uint8_t* rgba, int width, int height) = 0;

  DisplayCapability display_;
  bool hdrOutput_ = false;
  int width_ = 0;
  int height_ = 0;
  // Asked of DXGI in both backends: the settings window and the tray menu
  // build their own Direct3D 11 chains from this answer.
  bool tearingSupported_ = false;
};

// display_win32.cpp.
std::unique_ptr<Display::Impl> CreateD3D11Display();
// display_vulkan.cpp. Only built where the build found the Vulkan SDK
// (QBLANK_VULKAN); GraphicsApiBuilt says whether it is there.
std::unique_ptr<Display::Impl> CreateVulkanDisplay();

}  // namespace cap
