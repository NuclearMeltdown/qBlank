#pragma once

// The settings dialog as a window of its own, so it can be moved off the
// preview -- onto a second monitor, next to the picture, wherever.
//
// This is a second window with a drawing surface of its own (ui/ui_surface.h)
// and its own Dear ImGui context. The tidier route would be ImGui's
// multi-viewport support, which turns any ImGui window into a real one when it
// is dragged out; that lives on the docking branch, and swapping the vendored
// library over for this one feature is a larger and riskier change than writing
// the window.

#include <cstdint>
#include <functional>
#include <string>

#include "ui/ui_surface.h"
#include "window.h"

struct ImGuiContext;

namespace cap {

class SettingsHost {
 public:
  ~SettingsHost();

  SettingsHost(const SettingsHost&) = delete;
  SettingsHost& operator=(const SettingsHost&) = delete;
  SettingsHost() = default;

  // Creates the window hidden. `allowTearing` comes from the preview, which
  // has already asked whether the adapter supports it.
  // Where the window should come up, and where it ended up. Zero or negative
  // means "wherever Windows likes", which is only right the very first time.
  struct Placement {
    int x = -1;
    int y = -1;
    int width = 0;
    int height = 0;
  };
  Placement placement() const;

  bool Create(float uiScale, bool allowTearing, const Placement& where, std::string* error);
  void Destroy();

  bool created() const { return window_.created(); }
  bool visible() const { return visible_; }
  const Window& window() const { return window_; }

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
  void SetKeyCallback(
      std::function<bool(Key key, bool ctrl, bool shift, bool alt, bool busy)> callback) {
    onKey_ = std::move(callback);
  }

  void Show(const std::string& title);
  void Hide();
  // Brings the window back in front of everything, from minimised as well.
  void Raise();
  // Raise, but only if `other` covers part of it or it is minimised -- the
  // settings shortcut fetches a window that was lost behind the preview instead
  // of closing it. False when nothing was in the way.
  bool RaiseIfCoveredBy(const Window& other);
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
  int width() const { return surface_.width(); }
  int height() const { return surface_.height(); }

  // Re-applies colours after a theme change.
  void ApplyTheme(bool darkMode, unsigned accentColor);

 private:
  bool OnWindowEvent(const WindowEvent& e);
  void Resize();

  Window window_;
  ImGuiContext* imgui_ = nullptr;
  ImGuiContext* previous_ = nullptr;  // restored by EndFrame
  UiSurface surface_;
  bool visible_ = false;
  bool closeRequested_ = false;
  // Offers a frame from inside Windows' modal move loop. Whether one is
  // actually drawn is the callback's decision, not this class's -- see the
  // comment on the implementation.
  void PumpModalFrame();
  bool themeApplied_ = false;
  bool inFrameCallback_ = false;
  uint32_t lastDrawTick_ = 0;
  std::function<void()> onFrame_;
  std::function<bool(Key, bool, bool, bool, bool)> onKey_;
  float uiScale_ = 1.0f;
};

}  // namespace cap
