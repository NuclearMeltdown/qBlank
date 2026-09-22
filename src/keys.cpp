#include "keys.h"

namespace cap {

namespace {

struct KeyEntry {
  Key key;
  const char* name;
  int legacy;
};

// In enum order; checked below.
constexpr KeyEntry kKeys[] = {
    {Key::None, "None", 0x00},

    {Key::Cancel, "Cancel", 0x03},
    {Key::Backspace, "Backspace", 0x08},
    {Key::Tab, "Tab", 0x09},
    {Key::Clear, "Clear", 0x0C},
    {Key::Enter, "Enter", 0x0D},
    {Key::Shift, "Shift", 0x10},
    {Key::Ctrl, "Ctrl", 0x11},
    {Key::Alt, "Alt", 0x12},
    {Key::Pause, "Pause", 0x13},
    {Key::CapsLock, "CapsLock", 0x14},
    {Key::Kana, "Kana", 0x15},
    {Key::ImeOn, "ImeOn", 0x16},
    {Key::Junja, "Junja", 0x17},
    {Key::Final, "Final", 0x18},
    {Key::Kanji, "Kanji", 0x19},
    {Key::ImeOff, "ImeOff", 0x1A},
    {Key::Escape, "Escape", 0x1B},
    {Key::Convert, "Convert", 0x1C},
    {Key::NonConvert, "NonConvert", 0x1D},
    {Key::Accept, "Accept", 0x1E},
    {Key::ModeChange, "ModeChange", 0x1F},
    {Key::Space, "Space", 0x20},
    {Key::PageUp, "PageUp", 0x21},
    {Key::PageDown, "PageDown", 0x22},
    {Key::End, "End", 0x23},
    {Key::Home, "Home", 0x24},
    {Key::Left, "Left", 0x25},
    {Key::Up, "Up", 0x26},
    {Key::Right, "Right", 0x27},
    {Key::Down, "Down", 0x28},
    {Key::Select, "Select", 0x29},
    {Key::Print, "Print", 0x2A},
    {Key::Execute, "Execute", 0x2B},
    {Key::PrintScreen, "PrintScreen", 0x2C},
    {Key::Insert, "Insert", 0x2D},
    {Key::Delete, "Delete", 0x2E},
    {Key::Help, "Help", 0x2F},

    {Key::Digit0, "0", 0x30},
    {Key::Digit1, "1", 0x31},
    {Key::Digit2, "2", 0x32},
    {Key::Digit3, "3", 0x33},
    {Key::Digit4, "4", 0x34},
    {Key::Digit5, "5", 0x35},
    {Key::Digit6, "6", 0x36},
    {Key::Digit7, "7", 0x37},
    {Key::Digit8, "8", 0x38},
    {Key::Digit9, "9", 0x39},

    {Key::A, "A", 0x41},
    {Key::B, "B", 0x42},
    {Key::C, "C", 0x43},
    {Key::D, "D", 0x44},
    {Key::E, "E", 0x45},
    {Key::F, "F", 0x46},
    {Key::G, "G", 0x47},
    {Key::H, "H", 0x48},
    {Key::I, "I", 0x49},
    {Key::J, "J", 0x4A},
    {Key::K, "K", 0x4B},
    {Key::L, "L", 0x4C},
    {Key::M, "M", 0x4D},
    {Key::N, "N", 0x4E},
    {Key::O, "O", 0x4F},
    {Key::P, "P", 0x50},
    {Key::Q, "Q", 0x51},
    {Key::R, "R", 0x52},
    {Key::S, "S", 0x53},
    {Key::T, "T", 0x54},
    {Key::U, "U", 0x55},
    {Key::V, "V", 0x56},
    {Key::W, "W", 0x57},
    {Key::X, "X", 0x58},
    {Key::Y, "Y", 0x59},
    {Key::Z, "Z", 0x5A},

    {Key::LeftWin, "LeftWin", 0x5B},
    {Key::RightWin, "RightWin", 0x5C},
    {Key::ContextMenu, "ContextMenu", 0x5D},
    {Key::Sleep, "Sleep", 0x5F},

    {Key::Num0, "Num0", 0x60},
    {Key::Num1, "Num1", 0x61},
    {Key::Num2, "Num2", 0x62},
    {Key::Num3, "Num3", 0x63},
    {Key::Num4, "Num4", 0x64},
    {Key::Num5, "Num5", 0x65},
    {Key::Num6, "Num6", 0x66},
    {Key::Num7, "Num7", 0x67},
    {Key::Num8, "Num8", 0x68},
    {Key::Num9, "Num9", 0x69},
    {Key::NumMultiply, "NumMultiply", 0x6A},
    {Key::NumAdd, "NumAdd", 0x6B},
    {Key::NumSeparator, "NumSeparator", 0x6C},
    {Key::NumSubtract, "NumSubtract", 0x6D},
    {Key::NumDecimal, "NumDecimal", 0x6E},
    {Key::NumDivide, "NumDivide", 0x6F},

    {Key::F1, "F1", 0x70},
    {Key::F2, "F2", 0x71},
    {Key::F3, "F3", 0x72},
    {Key::F4, "F4", 0x73},
    {Key::F5, "F5", 0x74},
    {Key::F6, "F6", 0x75},
    {Key::F7, "F7", 0x76},
    {Key::F8, "F8", 0x77},
    {Key::F9, "F9", 0x78},
    {Key::F10, "F10", 0x79},
    {Key::F11, "F11", 0x7A},
    {Key::F12, "F12", 0x7B},
    {Key::F13, "F13", 0x7C},
    {Key::F14, "F14", 0x7D},
    {Key::F15, "F15", 0x7E},
    {Key::F16, "F16", 0x7F},
    {Key::F17, "F17", 0x80},
    {Key::F18, "F18", 0x81},
    {Key::F19, "F19", 0x82},
    {Key::F20, "F20", 0x83},
    {Key::F21, "F21", 0x84},
    {Key::F22, "F22", 0x85},
    {Key::F23, "F23", 0x86},
    {Key::F24, "F24", 0x87},

    {Key::NumLock, "NumLock", 0x90},
    {Key::ScrollLock, "ScrollLock", 0x91},
    {Key::OemJisho, "OemJisho", 0x92},
    {Key::OemMasshou, "OemMasshou", 0x93},
    {Key::OemTouroku, "OemTouroku", 0x94},
    {Key::OemLoya, "OemLoya", 0x95},
    {Key::OemRoya, "OemRoya", 0x96},

    {Key::LeftShift, "LeftShift", 0xA0},
    {Key::RightShift, "RightShift", 0xA1},
    {Key::LeftCtrl, "LeftCtrl", 0xA2},
    {Key::RightCtrl, "RightCtrl", 0xA3},
    {Key::LeftAlt, "LeftAlt", 0xA4},
    {Key::RightAlt, "RightAlt", 0xA5},

    {Key::BrowserBack, "BrowserBack", 0xA6},
    {Key::BrowserForward, "BrowserForward", 0xA7},
    {Key::BrowserRefresh, "BrowserRefresh", 0xA8},
    {Key::BrowserStop, "BrowserStop", 0xA9},
    {Key::BrowserSearch, "BrowserSearch", 0xAA},
    {Key::BrowserFavorites, "BrowserFavorites", 0xAB},
    {Key::BrowserHome, "BrowserHome", 0xAC},
    {Key::VolumeMute, "VolumeMute", 0xAD},
    {Key::VolumeDown, "VolumeDown", 0xAE},
    {Key::VolumeUp, "VolumeUp", 0xAF},
    {Key::MediaNext, "MediaNext", 0xB0},
    {Key::MediaPrevious, "MediaPrevious", 0xB1},
    {Key::MediaStop, "MediaStop", 0xB2},
    {Key::MediaPlayPause, "MediaPlayPause", 0xB3},
    {Key::LaunchMail, "LaunchMail", 0xB4},
    {Key::LaunchMediaSelect, "LaunchMediaSelect", 0xB5},
    {Key::LaunchApp1, "LaunchApp1", 0xB6},
    {Key::LaunchApp2, "LaunchApp2", 0xB7},

    {Key::Oem1, "Oem1", 0xBA},
    {Key::OemPlus, "OemPlus", 0xBB},
    {Key::OemComma, "OemComma", 0xBC},
    {Key::OemMinus, "OemMinus", 0xBD},
    {Key::OemPeriod, "OemPeriod", 0xBE},
    {Key::Oem2, "Oem2", 0xBF},
    {Key::Oem3, "Oem3", 0xC0},
    {Key::AbntC1, "AbntC1", 0xC1},
    {Key::AbntC2, "AbntC2", 0xC2},
    {Key::Oem4, "Oem4", 0xDB},
    {Key::Oem5, "Oem5", 0xDC},
    {Key::Oem6, "Oem6", 0xDD},
    {Key::Oem7, "Oem7", 0xDE},
    {Key::Oem8, "Oem8", 0xDF},
    {Key::OemAx, "OemAx", 0xE1},
    {Key::Oem102, "Oem102", 0xE2},

    {Key::IcoHelp, "IcoHelp", 0xE3},
    {Key::Ico00, "Ico00", 0xE4},
    {Key::ProcessKey, "ProcessKey", 0xE5},
    {Key::IcoClear, "IcoClear", 0xE6},
    {Key::Packet, "Packet", 0xE7},
    {Key::OemReset, "OemReset", 0xE9},
    {Key::OemJump, "OemJump", 0xEA},
    {Key::OemPa1, "OemPa1", 0xEB},
    {Key::OemPa2, "OemPa2", 0xEC},
    {Key::OemPa3, "OemPa3", 0xED},
    {Key::OemWsCtrl, "OemWsCtrl", 0xEE},
    {Key::OemCuSel, "OemCuSel", 0xEF},
    {Key::OemAttn, "OemAttn", 0xF0},
    {Key::OemFinish, "OemFinish", 0xF1},
    {Key::OemCopy, "OemCopy", 0xF2},
    {Key::OemAuto, "OemAuto", 0xF3},
    {Key::OemEnlw, "OemEnlw", 0xF4},
    {Key::OemBackTab, "OemBackTab", 0xF5},
    {Key::Attn, "Attn", 0xF6},
    {Key::CrSel, "CrSel", 0xF7},
    {Key::ExSel, "ExSel", 0xF8},
    {Key::EraseEof, "EraseEof", 0xF9},
    {Key::Play, "Play", 0xFA},
    {Key::Zoom, "Zoom", 0xFB},
    {Key::NoName, "NoName", 0xFC},
    {Key::Pa1, "Pa1", 0xFD},
    {Key::OemClear, "OemClear", 0xFE},
};

constexpr int kKeyCount = (int)(sizeof(kKeys) / sizeof(kKeys[0]));
static_assert(kKeyCount == (int)Key::Count, "every key needs exactly one entry");

constexpr bool InEnumOrder() {
  for (int i = 0; i < kKeyCount; ++i) {
    if ((int)kKeys[i].key != i) return false;
  }
  return true;
}
static_assert(InEnumOrder(), "entries must follow the enum");

// The reverse of the legacy column, so a key code from the file or the
// platform finds its key without a search.
struct LegacyIndex {
  Key byCode[256];
};

constexpr LegacyIndex BuildLegacyIndex() {
  LegacyIndex index{};
  for (int i = 0; i < kKeyCount; ++i) index.byCode[kKeys[i].legacy] = kKeys[i].key;
  return index;
}

constexpr LegacyIndex kByLegacy = BuildLegacyIndex();

}  // namespace

const char* KeyName(Key key) {
  const int i = (int)key;
  return i >= 0 && i < kKeyCount ? kKeys[i].name : kKeys[0].name;
}

bool KeyFromName(const std::string& name, Key* key) {
  for (const KeyEntry& entry : kKeys) {
    if (name == entry.name) {
      *key = entry.key;
      return true;
    }
  }
  return false;
}

Key KeyFromLegacyCode(int code) {
  return code >= 0 && code < 256 ? kByLegacy.byCode[code] : Key::None;
}

int LegacyCode(Key key) {
  const int i = (int)key;
  return i >= 0 && i < kKeyCount ? kKeys[i].legacy : 0;
}

}  // namespace cap
