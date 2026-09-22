#pragma once

// The settings dialog as a window of its own, so it can be moved off the
// preview -- onto a second monitor, next to the picture, wherever.
//
// This is a second Win32 window with its own swap chain and its own Dear ImGui
// context, sharing the D3D device and the font atlas with the main window. The
// tidier route would be ImGui's multi-viewport support, which turns any ImGui
// window into a real one when it is dragged out; that lives on the docking
// branch, and swapping the vendored library over for this one feature is a
// larger and riskier change than writing the window.

#include <d3d11.h>
#include <dxgi1_2.h>

#include <functional>
#include <string>

#include "common_win32.h"

struct ImGuiContext;
struct ImFontAtlas;

namespace cap {

class SettingsHost {
 public:
  ~SettingsHost();

  SettingsHost(const SettingsHost&) = delete;
  SettingsHost& operator=(const SettingsHost&) = delete;
  SettingsHost() = default;

  // Creates the window hidden. `atlas` is the main context's font atlas, shared
  // rather than rebuilt: the fonts are identical and one copy is enough.
  // `allowTearing` comes from the main context, which has already asked DXGI
  // whether the adapter supports it.
  // Where the window should come up, and where it ended up. Zero or negative
  // means "wherever Windows likes", which is only right the very first time.
  struct Placement {
    int x = -1;
    int y = -1;
    int width = 0;
    int height = 0;
  };
  Placement placement() const;

  bool Create(HINSTANCE instance, ID3D11Device* device, ID3D11DeviceContext* context,
              ImFontAtlas* atlas, float uiScale, bool allowTearing, const Placement& where,
              std::string* error);
  void Destroy();

  bool created() const { return hwnd_ != nullptr; }
  bool visible() const { return visible_; }
  HWND hwnd() const { return hwnd_; }

  ImGuiContext* context() const { return imgui_; }

  // Called while the window is being dragged or resized. Windows runs a modal
  // loop of its own for that and does not return to ours until the mouse comes
  // up, so without this the picture stands still for as long as the window is
  // being moved. A timer keeps ticking inside that loop, which is the one thing
  // that still gets through.
  void SetFrameCallback(std::function<void()> callback) { onFrame_ = std::move(callback); }

  // Offered every key press this window receives, so the shortcuts work with
  // the settings in front as well. `busy` says this window's ImGui wants the key
  // for itself -- a text field is being typed into, or a list is open. True
  // from the callback means the key was used and goes no further.
  void SetKeyCallback(std::function<bool(WPARAM key, LPARAM lparam, bool busy)> callback) {
    onKey_ = std::move(callback);
  }

  void Show(const std::string& title);
  void Hide();
  // Brings the window back in front of everything, from minimised as well.
  void Raise();
  // Raise, but only if `other` covers part of it or it is minimised -- the
  // settings shortcut fetches a window that was lost behind the preview instead
  // of closing it. False when nothing was in the way.
  bool RaiseIfCoveredBy(HWND other);
  // Mirrors the preview's "always on top". Without an owner nothing else keeps
  // the settings above a topmost preview.
  void SetTopmost(bool top);

  // True when the user clicked the window's close button since the last call.
  bool takeCloseRequest();

  // Makes this window's ImGui context current and starts its frame. False when
  // there is nothing to draw -- hidden, minimised, or no client area.
  bool BeginFrame(bool darkMode, unsigned accentColor);
  // Ends the frame, presents, and restores the previous ImGui context.
  void EndFrame();

  // Client size in pixels, valid between BeginFrame and EndFrame.
  int width() const { return width_; }
  int height() const { return height_; }

  // Re-applies colours after a theme change.
  void ApplyTheme(bool darkMode, unsigned accentColor);

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  bool CreateRenderTarget();
  void ReleaseRenderTarget();
  void Resize();

  HWND hwnd_ = nullptr;
  ImGuiContext* imgui_ = nullptr;
  ImGuiContext* previous_ = nullptr;  // restored by EndFrame
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> ctx_;
  ComPtr<IDXGISwapChain1> swapchain_;
  ComPtr<ID3D11RenderTargetView> rtv_;
  int width_ = 0;
  int height_ = 0;
  bool visible_ = false;
  bool closeRequested_ = false;
  bool occluded_ = false;
  // Offers a frame from inside Windows' modal move loop. Whether one is
  // actually drawn is the callback's decision, not this class's -- see the
  // comment on the implementation.
  void PumpModalFrame();
  // Whether this swapchain may hand a frame straight to the screen instead of
  // waiting to be composited. Measured to be the difference between a present
  // that costs 0.2 ms and one that costs 14.
  UINT swapchainFlags_ = 0;
  UINT presentFlags_ = 0;
  bool themeApplied_ = false;
  bool inFrameCallback_ = false;
  unsigned long lastDrawTick_ = 0;
  std::function<void()> onFrame_;
  std::function<bool(WPARAM, LPARAM, bool)> onKey_;
  float uiScale_ = 1.0f;
};

}  // namespace cap
