#pragma once

// Rebindable keyboard shortcuts.
//
// Two things are deliberately not in here. Escape always leaves fullscreen and
// closes the dialog, and Ctrl+1..9 always picks a profile: both are conventions
// people expect to hold, and a binding editor that lets you break your way out
// of fullscreen is a trap rather than a feature.

#include <string>

#include "common.h"

namespace cap {

enum class HotkeyAction {
  Fullscreen,
  Settings,
  Stats,
  RestartCapture,
  ReinitCard,
  Record,
  Screenshot,
  ScreenshotClipboard,
  Freeze,
  Compare,
  BypassFilters,
  DetectCrop,
  DetectStandard,
  RemeasureRange,
  Mute,
  VolumeUp,
  VolumeDown,
  Count
};

struct HotkeyBinding {
  int vk = 0;  // 0 = unbound
  bool ctrl = false;
  bool shift = false;
  bool alt = false;

  bool bound() const { return vk != 0; }
  bool Matches(int key, bool ctrlDown, bool shiftDown, bool altDown) const {
    return bound() && vk == key && ctrl == ctrlDown && shift == shiftDown && alt == altDown;
  }
  bool operator==(const HotkeyBinding& o) const {
    return vk == o.vk && ctrl == o.ctrl && shift == o.shift && alt == o.alt;
  }
};

// All bindings, indexed by HotkeyAction.
struct Hotkeys {
  HotkeyBinding items[(int)HotkeyAction::Count];

  Hotkeys();  // factory defaults
  HotkeyBinding& operator[](HotkeyAction a) { return items[(int)a]; }
  const HotkeyBinding& operator[](HotkeyAction a) const { return items[(int)a]; }

  // The action bound to this combination, or Count when there is none.
  HotkeyAction Find(int vk, bool ctrl, bool shift, bool alt) const;
};

// Label shown in the settings and the context menu, in the current language.
const char* HotkeyActionName(HotkeyAction action);
// Stable key used in the config file, independent of language.
const char* HotkeyActionKey(HotkeyAction action);

// "Strg+Umschalt+F9", or an empty string when unbound. Key names come from the
// keyboard layout, so a French keyboard reads its own labels.
std::string HotkeyText(const HotkeyBinding& binding);

// True for keys that must not be swallowed as a shortcut.
bool IsReservedKey(int vk);

}  // namespace cap
