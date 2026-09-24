#include "render/display_backend.h"

#include "common.h"
#include "i18n.h"

namespace cap {

const char* GraphicsApiName(GraphicsApi api) {
  switch (api) {
    case GraphicsApi::D3D11: return "Direct3D 11";
    case GraphicsApi::Vulkan: return "Vulkan";
  }
  return "?";
}

bool GraphicsApiBuilt(GraphicsApi api) {
  switch (api) {
    case GraphicsApi::D3D11: return true;
#ifdef QBLANK_VULKAN
    case GraphicsApi::Vulkan: return true;
#else
    case GraphicsApi::Vulkan: return false;
#endif
  }
  return false;
}

Display::Display() = default;

Display::~Display() {
  Shutdown();
}

bool Display::Initialize(const Window& window, std::string* error, GraphicsApi api) {
  Shutdown();
  impl_.reset();

  switch (api) {
    case GraphicsApi::D3D11: impl_ = CreateD3D11Display(); break;
    case GraphicsApi::Vulkan:
#ifdef QBLANK_VULKAN
      impl_ = CreateVulkanDisplay();
#endif
      break;
  }
  if (!impl_) {
    return ReportError(error, CAP_SAID(std::string(T("Dieser Build enthält kein ",
                                                     "This build does not include ")) +
                                       GraphicsApiName(api)));
  }
  if (!impl_->Initialize(window, error)) {
    impl_->Shutdown();
    impl_.reset();
    return false;
  }
  return true;
}

void Display::Shutdown() {
  if (impl_) impl_->Shutdown();
}

bool Display::initialized() const { return impl_ && impl_->initialized(); }

GraphicsApi Display::api() const { return impl_ ? impl_->api() : GraphicsApi::D3D11; }

void Display::Resize() {
  if (impl_) impl_->Resize();
}

Display::DisplayCapability Display::displayCapability() const {
  return impl_ ? impl_->display_ : DisplayCapability();
}

void Display::RefreshDisplayCapability() {
  if (impl_) impl_->RefreshDisplayCapability();
}

bool Display::SetHdrOutput(bool enabled, std::string* error) {
  return impl_ && impl_->SetHdrOutput(enabled, error);
}

bool Display::hdrOutput() const { return impl_ && impl_->hdrOutput_; }

bool Display::BeginFrame(const float clearColor[4]) {
  return impl_ && impl_->BeginFrame(clearColor);
}

void Display::EndFrame(bool vsync) {
  if (impl_) impl_->EndFrame(vsync);
}

bool Display::GrabBackBuffer(std::vector<uint8_t>* rgba, int* width, int* height) {
  return impl_ && impl_->GrabBackBuffer(rgba, width, height);
}

int Display::width() const { return impl_ ? impl_->width_ : 0; }
int Display::height() const { return impl_ ? impl_->height_ : 0; }
bool Display::tearingSupported() const { return impl_ && impl_->tearingSupported_; }

bool Display::InitUi() { return impl_ && impl_->InitUi(); }

void Display::ShutdownUi() {
  if (impl_) impl_->ShutdownUi();
}

void Display::NewUiFrame() {
  if (impl_) impl_->NewUiFrame();
}

void Display::RenderUi(ImDrawData* data) {
  if (impl_) impl_->RenderUi(data);
}

UiImage Display::CreateUiImage(const uint8_t* rgba, int width, int height) {
  return impl_ ? impl_->CreateUiImage(rgba, width, height) : UiImage();
}

}  // namespace cap
