#pragma once

// The menu behind the icon in the notification area (tray.h), drawn by the
// program like every other menu it has rather than by the system.
//
// It has to be able to stand anywhere on the screen -- above the taskbar, far
// from the preview -- so it is a window of its own, with its own drawing surface
// and Dear ImGui context, like the settings (ui/settings_host.h). Made the first
// time the menu opens, and kept until the icon goes.
//
// Nothing can reach out of that window, so a submenu does not fly out to the
// side: it opens below its row, inside the menu.

#include <cstdint>
#include <string>
#include <vector>

#include "ui/ui_surface.h"
#include "window.h"

struct ImGuiContext;

namespace cap {

struct TrayMenuItem {
  int id = 0;  // 0 is a separator; commands are positive
  std::string label;
  bool checked = false;
  bool enabled = true;
  std::vector<TrayMenuItem> children;  // a submenu when not empty

  bool operator==(const TrayMenuItem& o) const {
    return id == o.id && label == o.label && checked == o.checked && enabled == o.enabled &&
           children == o.children;
  }
  bool operator!=(const TrayMenuItem& o) const { return !(*this == o); }
};

class TrayMenu {
 public:
  TrayMenu() = default;
  ~TrayMenu();

  TrayMenu(const TrayMenu&) = delete;
  TrayMenu& operator=(const TrayMenu&) = delete;

  // The whole menu, replacing the last one. An open menu follows along.
  void SetItems(std::vector<TrayMenuItem> items);

  // Opens the menu at `at`, in screen coordinates: below it in the upper half of
  // the screen, above it in the lower. `allowTearing` comes from the preview,
  // which has already asked the adapter. The menu appears with the next Draw.
  bool Open(Point at, bool allowTearing, std::string* error);
  void Close();
  // Closes it and lets go of the window, the surface and the context.
  void Destroy();
  bool isOpen() const { return open_; }

  // Draws the open menu. What was picked, or 0. The menu stays up until
  // Close(): while it is in front, the command may still bring a window of the
  // program forward.
  int Draw(bool dark, unsigned accent);

 private:
  bool Create(bool allowTearing, std::string* error);
  bool OnWindowEvent(const WindowEvent& e);
  void ApplyTheme(bool dark, unsigned accent, float scale);
  // One Dear ImGui frame. What was picked, and how large the menu wants to be.
  int BuildFrame(int* width, int* height);
  int DrawItems(const std::vector<TrayMenuItem>& items);
  void Place(int width, int height);

  Window window_;
  UiSurface surface_;
  ImGuiContext* imgui_ = nullptr;

  std::vector<TrayMenuItem> items_;
  Point anchor_;
  Point origin_;  // where the window stands
  Rect screen_;   // the screen the anchor is on
  Rect work_;    // and the part of it the taskbar leaves
  bool open_ = false;
  bool shown_ = false;    // on screen since it was opened
  bool changed_ = true;   // what it holds, and so perhaps its size
  int expandedId_ = 0;    // the item whose submenu is open below it
  // After a row unfolded, a click picks nothing until the pointer has moved
  // away from where it was, on the screen.
  bool guardPick_ = false;
  float guardX_ = 0.0f, guardY_ = 0.0f;

  bool themeApplied_ = false;
  bool themeDark_ = false;
  unsigned themeAccent_ = 0;
  float themeScale_ = 0.0f;
  bool outlined_ = false;  // the platform draws the outline, not ImGui
  uint32_t lastDrawTick_ = 0;
};

}  // namespace cap
