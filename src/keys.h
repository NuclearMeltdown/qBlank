#pragma once

// Keyboard keys, named by what they are rather than by any platform's number
// for them. The list is a physical keyboard's worth: mouse buttons and gamepad
// controls are not keys here, even where a platform reports them as such.

#include <string>

namespace cap {

enum class Key {
  None,  // no key, or one this list does not know

  Cancel,
  Backspace,
  Tab,
  Clear,
  Enter,
  Shift,
  Ctrl,
  Alt,
  Pause,
  CapsLock,
  Kana,  // also Hangul
  ImeOn,
  Junja,
  Final,
  Kanji,  // also Hanja
  ImeOff,
  Escape,
  Convert,
  NonConvert,
  Accept,
  ModeChange,
  Space,
  PageUp,
  PageDown,
  End,
  Home,
  Left,
  Up,
  Right,
  Down,
  Select,
  Print,
  Execute,
  PrintScreen,
  Insert,
  Delete,
  Help,

  Digit0, Digit1, Digit2, Digit3, Digit4, Digit5, Digit6, Digit7, Digit8, Digit9,

  A, B, C, D, E, F, G, H, I, J, K, L, M,
  N, O, P, Q, R, S, T, U, V, W, X, Y, Z,

  LeftWin,
  RightWin,
  ContextMenu,
  Sleep,

  Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
  NumMultiply,
  NumAdd,
  NumSeparator,
  NumSubtract,
  NumDecimal,
  NumDivide,

  F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
  F13, F14, F15, F16, F17, F18, F19, F20, F21, F22, F23, F24,

  NumLock,
  ScrollLock,
  OemJisho,
  OemMasshou,
  OemTouroku,
  OemLoya,
  OemRoya,

  LeftShift,
  RightShift,
  LeftCtrl,
  RightCtrl,
  LeftAlt,
  RightAlt,

  BrowserBack,
  BrowserForward,
  BrowserRefresh,
  BrowserStop,
  BrowserSearch,
  BrowserFavorites,
  BrowserHome,
  VolumeMute,
  VolumeDown,
  VolumeUp,
  MediaNext,
  MediaPrevious,
  MediaStop,
  MediaPlayPause,
  LaunchMail,
  LaunchMediaSelect,
  LaunchApp1,
  LaunchApp2,

  // The punctuation keys. Which character each one carries depends on the
  // layout; the names follow the US one.
  Oem1,
  OemPlus,
  OemComma,
  OemMinus,
  OemPeriod,
  Oem2,
  Oem3,
  AbntC1,
  AbntC2,
  Oem4,
  Oem5,
  Oem6,
  Oem7,
  Oem8,
  OemAx,
  Oem102,

  IcoHelp,
  Ico00,
  ProcessKey,
  IcoClear,
  Packet,
  OemReset,
  OemJump,
  OemPa1,
  OemPa2,
  OemPa3,
  OemWsCtrl,
  OemCuSel,
  OemAttn,
  OemFinish,
  OemCopy,
  OemAuto,
  OemEnlw,
  OemBackTab,
  Attn,
  CrSel,
  ExSel,
  EraseEof,
  Play,
  Zoom,
  NoName,
  Pa1,
  OemClear,

  Count
};

// Stable name for the config file, independent of language and platform:
// "Enter", "F5", "M", "OemPlus". Key::None is "None".
const char* KeyName(Key key);
// The reverse. False when the name is not one of ours.
bool KeyFromName(const std::string& name, Key* key);

// The number qBlank 4.4 and earlier stored for a key, and still writes next to
// the name so an older version keeps reading the file. By origin these are the
// Windows virtual-key codes, but here they are only a file format. Zero is
// Key::None; so is any number that stands for no key on the list.
Key KeyFromLegacyCode(int code);
int LegacyCode(Key key);

// What the key is called on the user's keyboard, in the user's layout: "Ö"
// rather than Oem3 on a German one. Always something, if only a code.
std::string KeyLabel(Key key);

}  // namespace cap
