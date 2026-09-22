#pragma once

// D3D11 device and swap chain, set up for the lowest presentation latency the
// API allows: flip model, two buffers, a maximum frame latency of one, and
// tearing permitted so a frame can reach the panel without waiting for vblank.

#include <d3d11.h>
#include <dxgi1_6.h>

#include <string>
#include <vector>

#include "common_win32.h"

namespace cap {

// Names of the installed graphics adapters, joined. Used to notice that the
// machine's hardware changed since the encoder test was cached -- a saved
// result from someone's old card is worse than no result at all.
std::string GraphicsAdapterSignature();

class D3DContext {
 public:
  D3DContext() = default;
  ~D3DContext();

  D3DContext(const D3DContext&) = delete;
  D3DContext& operator=(const D3DContext&) = delete;

  bool Initialize(HWND hwnd, std::string* error);
  void Shutdown();

  // Recreates the back buffer for the window's current client size. Safe to
  // call on every resize message.
  void Resize();

  // What the screen this window is on can actually do. Asked of DXGI rather
  // than of a setting, and re-asked when the window moves: dragging a window
  // from an HDR screen to an ordinary one has to change what is sent to it.
  struct DisplayCapability {
    bool hdr = false;      // the output is in HDR10 mode right now
    float peakNits = 100.0f;
    float minNits = 0.0f;
  };
  DisplayCapability displayCapability() const { return display_; }
  void RefreshDisplayCapability();

  // Switches the swapchain between eight bit sRGB and half float scRGB. scRGB
  // is linear with 1.0 fixed at eighty nits, so highlights simply carry on past
  // one -- which is why it suits a pipeline that already works in float.
  // Returns false and stays where it was if the switch could not be made.
  bool SetHdrOutput(bool enabled, std::string* error);
  bool hdrOutput() const { return hdrOutput_; }

  // Binds the back buffer and clears it. Returns false when the window is
  // occluded or has no area, in which case the frame should be skipped.
  bool BeginFrame(const float clearColor[4]);

  // Presents. `vsync` false uses immediate presentation with tearing allowed
  // where the system supports it.
  void EndFrame(bool vsync);

  // Liest den Rueckpuffer als RGBA aus, also das Bild samt allem, was darauf
  // gezeichnet wurde. Muss vor dem Present passieren -- danach ist der Inhalt
  // unter FLIP_DISCARD undefiniert.
  //
  // Nur im Acht-Bit-Modus. Ist die HDR-Ausgabe aktiv, ist der Puffer scRGB in
  // halben Gleitkommazahlen, und ihn ohne Pruefung auf echter Hardware nach
  // SDR umzurechnen hiesse, ungetestete Farbmathematik auszuliefern.
  bool GrabBackBuffer(std::vector<uint8_t>* rgba, int* width, int* height);

  ID3D11Device* device() const { return device_.Get(); }
  ID3D11DeviceContext* context() const { return context_.Get(); }
  ID3D11RenderTargetView* rtv() const { return rtv_.Get(); }
  IDXGISwapChain1* swapchain() const { return swapchain_.Get(); }

  int width() const { return width_; }
  int height() const { return height_; }
  bool tearingSupported() const { return tearingSupported_; }

  // How many frames the CPU may run ahead of the GPU. One is what the preview
  // wants on its own -- it is the cheapest latency there is. It stops being
  // right the moment a second window shares the device: the queue is per
  // device, so with a depth of one the two swapchains take turns waiting for
  // each other, and a present that should cost a fraction of a millisecond
  // costs most of a refresh. Raised while the settings have a window, put back
  // when they do not.
  void SetFrameLatency(UINT frames);

 private:
  bool CreateRenderTarget();
  void ReleaseRenderTarget();

  HWND hwnd_ = nullptr;
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<IDXGISwapChain1> swapchain_;
  DisplayCapability display_;
  bool hdrOutput_ = false;
  ComPtr<ID3D11RenderTargetView> rtv_;

  int width_ = 0;
  int height_ = 0;
  bool tearingSupported_ = false;
  UINT frameLatency_ = 1;
  bool occluded_ = false;
  UINT swapchainFlags_ = 0;
};

}  // namespace cap
