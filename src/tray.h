#pragma once

// The icon in the notification area and the menu behind it: a second place for
// a handful of commands, because the context menu has grown too long to find
// them in quickly. Which commands is the user's choice (AppSettings::trayItems);
// this file only shows what it is handed.
//
// The icon lives on a thread of its own. A menu opened from it runs a modal
// loop until it closes, and on the thread that draws the picture that loop
// would hold the picture still for as long as the menu is open. So the menu is
// built on the main thread, handed over whole, and what was picked comes back
// through TakeCommands().
//
// Nothing here hides the main window. The taskbar button stays whether the icon
// is shown or not; minimising into the tray is deliberately not a feature.
//
// The Windows half is tray_win32.cpp.

#include <memory>
#include <string>
#include <vector>

namespace cap {

struct TrayMenuItem {
  int id = 0;  // 0 is a separator; commands are positive
  std::string label;
  bool checked = false;
  bool enabled = true;
  bool isDefault = false;              // drawn bold: what a left click does too
  std::vector<TrayMenuItem> children;  // a submenu when not empty

  bool operator==(const TrayMenuItem& o) const {
    return id == o.id && label == o.label && checked == o.checked && enabled == o.enabled &&
           isDefault == o.isDefault && children == o.children;
  }
  bool operator!=(const TrayMenuItem& o) const { return !(*this == o); }
};

class TrayIcon {
 public:
  // What a left click on the icon, or Enter on it, reports.
  static constexpr int kActivate = -1;

  TrayIcon();
  ~TrayIcon();
  TrayIcon(const TrayIcon&) = delete;
  TrayIcon& operator=(const TrayIcon&) = delete;

  // Starts the thread and puts the icon up. The calling thread is the one woken
  // when something is picked, so call it from the main thread. False when the
  // icon could not be shown; the program runs on without one.
  bool Show(const std::string& tooltip);
  // Takes the icon down and ends its thread. Closes an open menu first. Safe to
  // call when nothing is shown.
  void Hide();
  bool shown() const;

  void SetTooltip(const std::string& tooltip);
  // The whole menu, replacing the last one. Takes effect the next time it opens.
  void SetMenu(std::vector<TrayMenuItem> items);
  // Menus drawn dark or light. Process-wide on Windows, so the main window's
  // system menu follows along -- which is what it should do anyway.
  void SetDarkMenus(bool dark);

  // What was picked since the last call, oldest first.
  std::vector<int> TakeCommands();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cap
