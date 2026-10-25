#pragma once

// Rebindable keyboard shortcuts.
//
// Two things are deliberately not in here. Escape always leaves fullscreen and
// closes the dialog, and Ctrl+1..9 always picks a profile: both are conventions
// people expect to hold, and a binding editor that lets you break your way out
// of fullscreen is a trap rather than a feature.

#include <string>

#include "common.h"
#include "keys.h"

namespace cap {

enum class HotkeyAction {
  Fullscreen,
  Borderless,
  Settings,
  Stats,
  Toolbar,
  RestartCapture,
  ReinitCard,
  Record,
  ReplaySave,
  ReplayToggle,
  Marker,
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
  Key key = Key::None;  // None = unbound
  bool ctrl = false;
  bool shift = false;
  bool alt = false;

  bool bound() const { return key != Key::None; }
  bool Matches(Key pressed, bool ctrlDown, bool shiftDown, bool altDown) const {
    return bound() && key == pressed && ctrl == ctrlDown && shift == shiftDown && alt == altDown;
  }
  bool operator==(const HotkeyBinding& o) const {
    return key == o.key && ctrl == o.ctrl && shift == o.shift && alt == o.alt;
  }
};

// All bindings, indexed by HotkeyAction.
struct Hotkeys {
  HotkeyBinding items[(int)HotkeyAction::Count];

  Hotkeys();  // factory defaults
  HotkeyBinding& operator[](HotkeyAction a) { return items[(int)a]; }
  const HotkeyBinding& operator[](HotkeyAction a) const { return items[(int)a]; }

  // The action bound to this combination, or Count when there is none.
  HotkeyAction Find(Key key, bool ctrl, bool shift, bool alt) const;
};

// The same actions while another program has the keyboard, any of them. All
// unbound out of the box: a combination held system-wide is taken from every
// other program, and that is not something to do unasked.
struct GlobalHotkeys {
  bool on = true;  // off keeps the bindings but holds none of them
  HotkeyBinding items[(int)HotkeyAction::Count];
};

// Label shown in the settings and the context menu, in the current language.
const char* HotkeyActionName(HotkeyAction action);
// Stable key used in the config file, independent of language.
const char* HotkeyActionKey(HotkeyAction action);

// "Strg+Umschalt+F9", or an empty string when unbound. Key names come from the
// keyboard layout, so a French keyboard reads its own labels.
std::string HotkeyText(const HotkeyBinding& binding);

// True for keys that must not be swallowed as a shortcut.
bool IsReservedKey(Key key);

}  // namespace cap
