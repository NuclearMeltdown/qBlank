#include "hotkeys.h"

#include "i18n.h"

namespace cap {

Hotkeys::Hotkeys() {
  (*this)[HotkeyAction::Fullscreen].key = Key::Enter;
  (*this)[HotkeyAction::Settings].key = Key::F2;
  (*this)[HotkeyAction::Stats].key = Key::F1;
  (*this)[HotkeyAction::RestartCapture].key = Key::F5;
  // Shift+F5 next to F5, the way a browser puts the hard reload next to the
  // ordinary one: same key, more thrown away.
  (*this)[HotkeyAction::ReinitCard].key = Key::F5;
  (*this)[HotkeyAction::ReinitCard].shift = true;
  (*this)[HotkeyAction::Record].key = Key::F9;
  (*this)[HotkeyAction::Screenshot].key = Key::F10;
  // Dieselbe Taste mit Strg, wie das Kopieren ueberall sonst: gleiches Bild,
  // anderes Ziel.
  (*this)[HotkeyAction::ScreenshotClipboard].key = Key::F10;
  (*this)[HotkeyAction::ScreenshotClipboard].ctrl = true;
  // Die beiden Vergleichshilfen liegen nebeneinander auf den zwei Tasten, die
  // in der Reihe noch frei sind. Beide aendern nur, was zu sehen ist.
  (*this)[HotkeyAction::Freeze].key = Key::F11;
  (*this)[HotkeyAction::Compare].key = Key::F12;
  // Mit Umschalt dasselbe, nur ganz: nicht die halbe Anzeige ohne die
  // Composite-Filter, sondern die ganze ohne jeden Filter. Wie bei F5 und
  // Umschalt+F5 -- gleiche Taste, mehr weggenommen.
  (*this)[HotkeyAction::BypassFilters].key = Key::F12;
  (*this)[HotkeyAction::BypassFilters].shift = true;
  // Der Zuschnitt wird im Zweifel mehrmals hintereinander gesucht -- die
  // Messung braucht ein richtiges Bild, und wann eines anliegt, weiss nur der,
  // der hinsieht. F8 liegt frei und in derselben Reihe wie das uebrige, was
  // man im Vorbeigehen ausloest.
  (*this)[HotkeyAction::DetectCrop].key = Key::F8;
  // Neben dem Rand, weil es dieselbe Art von Befehl ist: eine Messung am
  // gerade anliegenden Bild, die man ausloest, weil man etwas sieht, das die
  // Automatik nicht misst. Und wie dort gilt, dass der Moment zaehlt -- ein
  // Rundgang auf einem schwarzen Bild entscheidet nichts.
  (*this)[HotkeyAction::DetectStandard].key = Key::F7;
  // Die dritte derselben Sorte, und deshalb die dritte Taste in derselben
  // Reihe. Sie ist die einzige, die eine *stehende* Messung wegwirft: das
  // Urteil ueber den Wertebereich haelt, bis sich das Bildformat aendert, und
  // eine im Treiber umgestellte Option aendert es nicht.
  (*this)[HotkeyAction::RemeasureRange].key = Key::F6;
  (*this)[HotkeyAction::Mute].key = Key::M;
  (*this)[HotkeyAction::VolumeUp].key = Key::OemPlus;
  (*this)[HotkeyAction::VolumeDown].key = Key::OemMinus;
}

HotkeyAction Hotkeys::Find(Key key, bool ctrl, bool shift, bool alt) const {
  for (int i = 0; i < (int)HotkeyAction::Count; ++i) {
    if (items[i].Matches(key, ctrl, shift, alt)) return (HotkeyAction)i;
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

// Keys the keyboard layout either names badly or not at all.
const char* SpecialKeyName(Key key) {
  switch (key) {
    case Key::Enter: return T("Eingabe", "Enter");
    case Key::Space: return T("Leertaste", "Space");
    case Key::OemPlus: return "+";
    case Key::OemMinus: return "-";
    case Key::NumAdd: return T("Num +", "Num +");
    case Key::NumSubtract: return T("Num -", "Num -");
    case Key::Left: return T("Links", "Left");
    case Key::Right: return T("Rechts", "Right");
    case Key::Up: return T("Hoch", "Up");
    case Key::Down: return T("Runter", "Down");
    case Key::PageUp: return T("Bild auf", "Page Up");
    case Key::PageDown: return T("Bild ab", "Page Down");
    case Key::Home: return T("Pos1", "Home");
    case Key::End: return T("Ende", "End");
    case Key::Insert: return T("Einfg", "Insert");
    case Key::Delete: return T("Entf", "Delete");
    case Key::Tab: return T("Tab", "Tab");
    case Key::Backspace: return T("Rücktaste", "Backspace");
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

  if (const char* special = SpecialKeyName(binding.key)) {
    text += special;
    return text;
  }

  // Everything else goes by the layout, see KeyLabel.
  text += KeyLabel(binding.key);
  return text;
}

bool IsReservedKey(Key key) {
  switch (key) {
    // Escape and the profile digits are fixed; the modifiers alone are not a
    // shortcut; F4 would collide with Alt+F4.
    case Key::Escape:
    case Key::Ctrl:
    case Key::LeftCtrl:
    case Key::RightCtrl:
    case Key::Shift:
    case Key::LeftShift:
    case Key::RightShift:
    case Key::Alt:
    case Key::LeftAlt:
    case Key::RightAlt:
    case Key::LeftWin:
    case Key::RightWin:
    case Key::F4:
      return true;
    default:
      return false;
  }
}

}  // namespace cap
