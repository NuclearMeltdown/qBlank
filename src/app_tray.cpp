// The icon in the notification area and its menu.

#include "app.h"

#include "i18n.h"
#include "record/screenshot.h"

namespace cap {

namespace {

// Commands in the icon's menu. The quick actions are numbered after TrayItem so
// the menu and the list in the settings cannot drift apart.
constexpr int kTrayShow = 1;
constexpr int kTrayQuit = 2;
constexpr int kTrayIconOff = 3;
constexpr int kTrayItemBase = 100;
constexpr int kTrayProfileBase = 200;

bool IsTraySeparator(const TrayMenuItem& item) {
  return item.id == 0 && item.children.empty();
}

}  // namespace

void App::UpdateTray() {
  if (!config_.app.trayIcon) {
    trayFailed_ = false;
    if (tray_.shown()) {
      trayPopup_.Destroy();
      tray_.Hide();
      trayMenu_.clear();
      trayTooltip_.clear();
    }
    return;
  }
  if (trayFailed_) return;

  const std::string tooltip =
      recorder_.recording() ? AppNameUtf8() + T(" – Aufnahme läuft", " – Recording") : AppNameUtf8();
  if (!tray_.shown()) {
    if (!tray_.Show(tooltip)) {
      // Not retried every frame; switching the setting off and on tries again.
      CAP_WARN("Tray icon could not be created");
      trayFailed_ = true;
      return;
    }
    trayTooltip_ = tooltip;
    trayMenu_.clear();
  } else if (tooltip != trayTooltip_) {
    tray_.SetTooltip(tooltip);
    trayTooltip_ = tooltip;
  }

  std::vector<TrayMenuItem> menu = BuildTrayMenu();
  if (menu != trayMenu_) {
    trayMenu_ = menu;
    trayPopup_.SetItems(std::move(menu));
  }

  // Not from inside a move or resize: they wait there until it is over.
  if (inModalFrame_) return;
  for (const TrayEvent& event : tray_.TakeEvents()) {
    if (event.kind == TrayEvent::Kind::Activate) {
      window_.Raise();
      continue;
    }
    std::string error;
    if (!trayPopup_.Open(event.at, display_.tearingSupported(), &error)) {
      CAP_WARN("Tray menu could not be opened: %s", error.c_str());
    }
  }
}

void App::DrawTrayMenu() {
  if (inModalFrame_) return;
  const int id = trayPopup_.Draw(darkMode_, config_.app.accentColor);
  if (id == 0) return;
  // Before the menu goes: while it is in front, the command may bring a window
  // of the program forward.
  RunTrayCommand(id);
  trayPopup_.Close();
}

std::vector<TrayMenuItem> App::BuildTrayMenu() {
  const TrayItems& on = config_.app.trayItems;
  std::vector<TrayMenuItem> menu;
  const auto add = [&](int id, const char* label, bool checked = false) {
    TrayMenuItem item;
    item.id = id;
    item.label = label;
    item.checked = checked;
    menu.push_back(std::move(item));
  };
  const auto quick = [&](TrayItem which, const char* label, bool checked = false) {
    if (on[(size_t)which]) add(kTrayItemBase + (int)which, label, checked);
  };
  // Between two groups only when both hold something.
  const auto group = [&] {
    if (!menu.empty() && !IsTraySeparator(menu.back())) menu.emplace_back();
  };

  add(kTrayShow, Format(T("%s anzeigen", "Show %s"), AppNameUtf8().c_str()).c_str());

  group();
  quick(TrayItem::Record, recorder_.recording() ? T("Aufnahme stoppen", "Stop recording")
                                                : T("Aufnahme starten", "Start recording"));
  quick(TrayItem::Screenshot, T("Screenshot", "Screenshot"));
  quick(TrayItem::ScreenshotClipboard,
        T("Screenshot in die Zwischenablage", "Screenshot to clipboard"));

  group();
  quick(TrayItem::RecordFolder, T("Aufnahmeordner öffnen", "Open the recordings folder"));
  quick(TrayItem::ScreenshotFolder, T("Screenshot-Ordner öffnen", "Open the screenshots folder"));

  group();
  quick(TrayItem::Fullscreen, T("Vollbild", "Fullscreen"), fullscreen_);
  quick(TrayItem::AlwaysOnTop, T("Immer im Vordergrund", "Always on top"), config_.app.alwaysOnTop);
  quick(TrayItem::Borderless, T("Rahmenlos", "Borderless"), config_.app.borderless);
  quick(TrayItem::Mute, T("Stumm", "Muted"), config_.active().audio.mute);
  quick(TrayItem::Freeze, T("Standbild", "Freeze"), frozen_);
  quick(TrayItem::VirtualCamera, T("Virtuelle Kamera", "Virtual camera"), config_.app.virtualCamera);

  group();
  if (on[(size_t)TrayItem::Profiles] && config_.profiles.size() > 1) {
    TrayMenuItem profiles;
    profiles.id = kTrayItemBase + (int)TrayItem::Profiles;
    profiles.label = T("Profil", "Profile");
    for (int i = 0; i < (int)config_.profiles.size(); ++i) {
      TrayMenuItem item;
      item.id = kTrayProfileBase + i;
      item.label = config_.profiles[(size_t)i].name;
      item.checked = i == config_.activeProfile;
      profiles.children.push_back(std::move(item));
    }
    menu.push_back(std::move(profiles));
  }
  quick(TrayItem::RestartCapture, T("Aufnahme neu starten", "Restart capture"));

  group();
  quick(TrayItem::Settings, T("Einstellungen...", "Settings..."));
  // Always there, and always ticked: the icon's own way out, so switching it
  // off does not mean hunting for it in the settings. Back on from there.
  add(kTrayIconOff, T("Symbol im Infobereich", "Icon in the notification area"), true);

  group();
  add(kTrayQuit, T("Beenden", "Quit"));
  return menu;
}

void App::RunTrayCommand(int id) {
  if (id == kTrayShow) {
    window_.Raise();
    return;
  }
  if (id == kTrayQuit) {
    window_.RequestClose();
    return;
  }
  if (id == kTrayIconOff) {
    config_.app.trayIcon = false;  // UpdateTray takes it away next frame
    return;
  }
  if (id >= kTrayProfileBase) {
    SwitchProfile(id - kTrayProfileBase);
    return;
  }
  switch ((TrayItem)(id - kTrayItemBase)) {
    case TrayItem::Record:
      recording_.ToggleRecording();
      break;
    case TrayItem::Screenshot:
      RequestScreenshot();
      break;
    case TrayItem::ScreenshotClipboard:
      RequestScreenshot(true);
      break;
    case TrayItem::RecordFolder:
      ShowOutputFolder(&config_.record.outputFolder, DefaultRecordFolder());
      break;
    case TrayItem::ScreenshotFolder:
      ShowOutputFolder(&config_.record.screenshotFolder, DefaultScreenshotFolder());
      break;
    case TrayItem::Fullscreen:
      ToggleFullscreen();
      if (fullscreen_) window_.Raise();
      break;
    case TrayItem::AlwaysOnTop:
      config_.app.alwaysOnTop = !config_.app.alwaysOnTop;
      ApplyWindowFlags();
      break;
    case TrayItem::Borderless:
      config_.app.borderless = !config_.app.borderless;
      break;
    case TrayItem::Mute:
      ToggleMute();
      break;
    case TrayItem::Freeze:
      ToggleFreeze();
      break;
    case TrayItem::VirtualCamera:
      // Switching on needs the camera installed, which is a step with
      // administrator rights that belongs in the settings, not in a menu.
      if (config_.app.virtualCamera || CameraSink::Status() == CameraSink::Setup::Installed) {
        config_.app.virtualCamera = !config_.app.virtualCamera;
      } else {
        // The settings may be drawn inside the main window, which is then the
        // one that has to come forward.
        window_.Raise();
        OpenSettings(T("Die virtuelle Kamera muss erst einmal installiert werden, unter Aufnahme.",
                       "The virtual camera has to be installed once first, under Recording."));
      }
      break;
    case TrayItem::RestartCapture:
      RestartAll(true);
      break;
    case TrayItem::Settings:
      window_.Raise();
      OpenSettings({});
      break;
    default:
      break;
  }
}

}  // namespace cap
