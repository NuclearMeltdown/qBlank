#include "hotkeys.h"

#include "i18n.h"

namespace cap {

Hotkeys::Hotkeys() {
  (*this)[HotkeyAction::Fullscreen].vk = VK_RETURN;
  (*this)[HotkeyAction::Settings].vk = VK_F2;
  (*this)[HotkeyAction::Stats].vk = VK_F1;
  (*this)[HotkeyAction::RestartCapture].vk = VK_F5;
  // Shift+F5 next to F5, the way a browser puts the hard reload next to the
  // ordinary one: same key, more thrown away.
  (*this)[HotkeyAction::ReinitCard].vk = VK_F5;
  (*this)[HotkeyAction::ReinitCard].shift = true;
  (*this)[HotkeyAction::Record].vk = VK_F9;
  (*this)[HotkeyAction::Screenshot].vk = VK_F10;
  // Dieselbe Taste mit Strg, wie das Kopieren ueberall sonst: gleiches Bild,
  // anderes Ziel.
  (*this)[HotkeyAction::ScreenshotClipboard].vk = VK_F10;
  (*this)[HotkeyAction::ScreenshotClipboard].ctrl = true;
  // Die beiden Vergleichshilfen liegen nebeneinander auf den zwei Tasten, die
  // in der Reihe noch frei sind. Beide aendern nur, was zu sehen ist.
  (*this)[HotkeyAction::Freeze].vk = VK_F11;
  (*this)[HotkeyAction::Compare].vk = VK_F12;
  // Mit Umschalt dasselbe, nur ganz: nicht die halbe Anzeige ohne die
  // Composite-Filter, sondern die ganze ohne jeden Filter. Wie bei F5 und
  // Umschalt+F5 -- gleiche Taste, mehr weggenommen.
  (*this)[HotkeyAction::BypassFilters].vk = VK_F12;
  (*this)[HotkeyAction::BypassFilters].shift = true;
  // Der Zuschnitt wird im Zweifel mehrmals hintereinander gesucht -- die
  // Messung braucht ein richtiges Bild, und wann eines anliegt, weiss nur der,
  // der hinsieht. F8 liegt frei und in derselben Reihe wie das uebrige, was
  // man im Vorbeigehen ausloest.
  (*this)[HotkeyAction::DetectCrop].vk = VK_F8;
  // Neben dem Rand, weil es dieselbe Art von Befehl ist: eine Messung am
  // gerade anliegenden Bild, die man ausloest, weil man etwas sieht, das die
  // Automatik nicht misst. Und wie dort gilt, dass der Moment zaehlt -- ein
  // Rundgang auf einem schwarzen Bild entscheidet nichts.
  (*this)[HotkeyAction::DetectStandard].vk = VK_F7;
  // Die dritte derselben Sorte, und deshalb die dritte Taste in derselben
  // Reihe. Sie ist die einzige, die eine *stehende* Messung wegwirft: das
  // Urteil ueber den Wertebereich haelt, bis sich das Bildformat aendert, und
  // eine im Treiber umgestellte Option aendert es nicht.
  (*this)[HotkeyAction::RemeasureRange].vk = VK_F6;
  (*this)[HotkeyAction::Mute].vk = 'M';
  (*this)[HotkeyAction::VolumeUp].vk = VK_OEM_PLUS;
  (*this)[HotkeyAction::VolumeDown].vk = VK_OEM_MINUS;
}

HotkeyAction Hotkeys::Find(int vk, bool ctrl, bool shift, bool alt) const {
  for (int i = 0; i < (int)HotkeyAction::Count; ++i) {
    if (items[i].Matches(vk, ctrl, shift, alt)) return (HotkeyAction)i;
  }
  return HotkeyAction::Count;
}

const char* HotkeyActionName(HotkeyAction action) {
  switch (action) {
    case HotkeyAction::Fullscreen: return T("Vollbild", "Fullscreen");
    case HotkeyAction::Settings: return T("Einstellungen", "Settings");
    case HotkeyAction::Stats: return T("Statistik", "Statistics");
    case HotkeyAction::RestartCapture: return T("Aufnahme neu starten", "Restart capture");
    case HotkeyAction::ReinitCard: return T("Karte neu einlesen", "Reinitialise card");
    case HotkeyAction::Record: return T("Aufnahme starten/stoppen", "Start/stop recording");
    case HotkeyAction::Screenshot: return T("Screenshot", "Screenshot");
    case HotkeyAction::ScreenshotClipboard:
      return T("Screenshot in die Zwischenablage", "Screenshot to clipboard");
    case HotkeyAction::Freeze: return T("Standbild", "Freeze");
    case HotkeyAction::Compare: return T("Filter vergleichen", "Compare filters");
    case HotkeyAction::BypassFilters: return T("Alle Filter aus", "All filters off");
    case HotkeyAction::DetectCrop: return T("Rand suchen", "Detect border");
    case HotkeyAction::DetectStandard: return T("Videonorm suchen", "Detect video standard");
    case HotkeyAction::RemeasureRange: return T("Wertebereich neu messen", "Measure range again");
    case HotkeyAction::Mute: return T("Stumm", "Mute");
    case HotkeyAction::VolumeUp: return T("Lauter", "Volume up");
    case HotkeyAction::VolumeDown: return T("Leiser", "Volume down");
    default: return "";
  }
}

const char* HotkeyActionKey(HotkeyAction action) {
  switch (action) {
    case HotkeyAction::Fullscreen: return "fullscreen";
    case HotkeyAction::Settings: return "settings";
    case HotkeyAction::Stats: return "stats";
    case HotkeyAction::RestartCapture: return "restartCapture";
    case HotkeyAction::ReinitCard: return "reinitCard";
    case HotkeyAction::Record: return "record";
    case HotkeyAction::Screenshot: return "screenshot";
    case HotkeyAction::ScreenshotClipboard: return "screenshotClipboard";
    case HotkeyAction::Freeze: return "freeze";
    case HotkeyAction::Compare: return "compare";
    case HotkeyAction::BypassFilters: return "bypassFilters";
    case HotkeyAction::DetectCrop: return "detectCrop";
    case HotkeyAction::DetectStandard: return "detectStandard";
    case HotkeyAction::RemeasureRange: return "remeasureRange";
    case HotkeyAction::Mute: return "mute";
    case HotkeyAction::VolumeUp: return "volumeUp";
    case HotkeyAction::VolumeDown: return "volumeDown";
    default: return "";
  }
}

namespace {

// Keys GetKeyNameText either names badly or not at all.
const char* SpecialKeyName(int vk) {
  switch (vk) {
    case VK_RETURN: return T("Eingabe", "Enter");
    case VK_SPACE: return T("Leertaste", "Space");
    case VK_OEM_PLUS: return "+";
    case VK_OEM_MINUS: return "-";
    case VK_ADD: return T("Num +", "Num +");
    case VK_SUBTRACT: return T("Num -", "Num -");
    case VK_LEFT: return T("Links", "Left");
    case VK_RIGHT: return T("Rechts", "Right");
    case VK_UP: return T("Hoch", "Up");
    case VK_DOWN: return T("Runter", "Down");
    case VK_PRIOR: return T("Bild auf", "Page Up");
    case VK_NEXT: return T("Bild ab", "Page Down");
    case VK_HOME: return T("Pos1", "Home");
    case VK_END: return T("Ende", "End");
    case VK_INSERT: return T("Einfg", "Insert");
    case VK_DELETE: return T("Entf", "Delete");
    case VK_TAB: return T("Tab", "Tab");
    case VK_BACK: return T("Rücktaste", "Backspace");
    default: return nullptr;
  }
}

}  // namespace

std::string HotkeyText(const HotkeyBinding& binding) {
  if (!binding.bound()) return T("—", "—");

  std::string text;
  if (binding.ctrl) text += T("Strg+", "Ctrl+");
  if (binding.alt) text += "Alt+";
  if (binding.shift) text += T("Umschalt+", "Shift+");

  if (const char* special = SpecialKeyName(binding.vk)) {
    text += special;
    return text;
  }

  // Everything else comes from the layout, so the label matches the key cap in
  // front of the user rather than a US keyboard.
  const UINT scan = ::MapVirtualKeyW((UINT)binding.vk, MAPVK_VK_TO_VSC);
  wchar_t name[64] = {};
  if (scan != 0 && ::GetKeyNameTextW((LONG)(scan << 16), name, 64) > 0) {
    text += ToUtf8(name);
    return text;
  }

  if (binding.vk >= 'A' && binding.vk <= 'Z') {
    text += (char)binding.vk;
  } else if (binding.vk >= '0' && binding.vk <= '9') {
    text += (char)binding.vk;
  } else {
    text += Format("VK 0x%02X", binding.vk);
  }
  return text;
}

bool IsReservedKey(int vk) {
  switch (vk) {
    // Escape and the profile digits are fixed; the modifiers alone are not a
    // shortcut; F4 would collide with Alt+F4.
    case VK_ESCAPE:
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
    case VK_LWIN:
    case VK_RWIN:
    case VK_F4:
      return true;
    default:
      return false;
  }
}

}  // namespace cap
