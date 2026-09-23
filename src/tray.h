#pragma once

// The icon in the notification area. It only reports what happened to it: a
// left click, or a right click and where. The menu behind it is the program's
// own (ui/tray_menu.h), drawn like every other menu it has, and what it holds
// is the user's choice (AppSettings::trayItems).
//
// The icon lives on a thread of its own. Every call to the notification area
// is a message to Explorer that waits for the answer, and on the thread that
// draws the picture a busy Explorer -- starting up, restarting -- would hold
// the picture still.
//
// Nothing here hides the main window. The taskbar button stays whether the icon
// is shown or not; minimising into the tray is deliberately not a feature.
//
// The Windows half is tray_win32.cpp.

#include <memory>
#include <string>
#include <vector>

#include "window.h"

namespace cap {

struct TrayEvent {
  enum class Kind {
    Activate,  // a left click on the icon, or Enter on it
    Menu,      // a right click, or the menu key: open the menu at `at`
  };
  Kind kind = Kind::Activate;
  Point at;  // screen coordinates
};

class TrayIcon {
 public:
  TrayIcon();
  ~TrayIcon();
  TrayIcon(const TrayIcon&) = delete;
  TrayIcon& operator=(const TrayIcon&) = delete;

  // Starts the thread and puts the icon up. The calling thread is the one woken
  // when something happens, so call it from the main thread. False when the
  // icon could not be shown; the program runs on without one.
  bool Show(const std::string& tooltip);
  // Takes the icon down and ends its thread. Safe to call when nothing is shown.
  void Hide();
  bool shown() const;

  void SetTooltip(const std::string& tooltip);

  // What happened since the last call, oldest first. A Menu event comes with
  // the right to bring a window of the program to the front, for a moment: open
  // the menu straight away.
  std::vector<TrayEvent> TakeEvents();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cap
