#pragma once

// Shortcuts that work while another program has the keyboard: start a
// recording or take a screenshot from inside the game. The system hands the
// combination to qBlank alone, so the game never sees it, and a combination
// another program already holds is refused -- the settings say which.
//
// A thread of its own, started only while something is bound: a press wakes the
// main thread the way the tray icon does, and nobody pays for a thread that
// has nothing to listen for.
//
// The Windows half is global_hotkeys_win32.cpp (RegisterHotKey). On Wayland
// it would be the GlobalShortcuts portal, where the desktop asks the user
// instead of refusing.

#include <memory>
#include <vector>

#include "hotkeys.h"

namespace cap {

class SystemHotkeys {
 public:
  SystemHotkeys();
  ~SystemHotkeys();
  SystemHotkeys(const SystemHotkeys&) = delete;
  SystemHotkeys& operator=(const SystemHotkeys&) = delete;

  // Holds exactly these from now on, by action; an unbound entry holds
  // nothing. Costs a comparison when nothing changed, so it can be called
  // every frame -- from the main thread, the one a press wakes.
  void Hold(const HotkeyBinding (&bindings)[(int)HotkeyAction::Count]);
  // The system said no: another program has this combination.
  bool Refused(HotkeyAction action) const;
  // Pressed since the last call, oldest first.
  std::vector<HotkeyAction> TakePressed();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cap
