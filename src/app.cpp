#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "desktop.h"
#include "files.h"
#include "i18n.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "platform.h"
#include "record/ffmpeg_locator.h"
#include "render/display.h"
#include "record/screenshot.h"
#include "ui/theme.h"

namespace cap {
namespace {

// How long without a frame before we call it "no signal".
const double kNoSignalSeconds = 1.5;
// How long a flat picture is given before it counts as no signal rather than as
// a black screen. Generous, because being wrong here means covering a game that
// was only fading out; a title screen that stays black for eight seconds is rare
// and a card with nothing on it stays black forever.
const double kFlatSeconds = 8.0;
// Unless the decoder also says it has no lock, in which case there is no reason
// to keep waiting on a second opinion.
const double kFlatUnlockedSeconds = 2.0;
// Snow is the confident verdict, but not by as much as a synthetic test
// suggested. Measured against a real racing demo on this card, steady fast
// motion reads a change of 37 against a threshold of 48, and a fade-and-cut
// between scenes produced a burst of Snow, Picture, Flat, Picture, Snow inside
// 1.2 seconds. Two seconds clears that comfortably and costs nothing where it
// matters: an unterminated input does not stop being snow after two seconds.
const double kSnowSeconds = 2.0;
// Delay between reconnect attempts, and the slower pace once it is clear the
// device is not coming back on its own.
const double kRetrySeconds = 2.0;
const double kRetrySlowSeconds = 10.0;
const int kRetryBackoffAfter = 5;
// Hide the pointer after this much stillness in fullscreen.
const double kCursorIdleSeconds = 2.0;
// How long the volume readout stays on screen after a change.
const double kVolumeOsdSeconds = 1.6;
// One wheel notch.
const float kVolumeStep = 0.05f;

// Wie SetItemTooltip, aber mit Umbruch -- dieselbe Breite wie im
// Einstellungsfenster, damit beide gleich aussehen. Der eingebaute bricht nicht
// um: ein ganzer Satz laeuft dann als eine einzige Zeile quer ueber den
// Bildschirm und steht mit dem Ende davon ausserhalb.
void WrappedTooltip(const char* text) {
  if (!text || !*text) return;
  if (ImGui::BeginItemTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

// BeginMenu mit Tastenkuerzel. Dear ImGui zeichnet bei einem Untermenue keins,
// die Spalte dafuer hat das Menue aber trotzdem: hier wird sie angemeldet und
// das Kuerzel an dieselbe Stelle und in derselben Farbe gesetzt wie bei einem
// MenuItem, links vom Pfeil.
bool BeginMenuWithShortcut(const char* label, const char* shortcut) {
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  const bool draw = shortcut && *shortcut && !window->SkipItems &&
                    window->DC.LayoutType == ImGuiLayoutType_Vertical;
  const ImVec2 pos = window->DC.CursorPos;
  float stretch = 0.0f;
  if (draw) {
    const float mark = IM_TRUNC(ImGui::GetFontSize() * 1.20f);
    const float width = window->DC.MenuColumns.DeclColumns(
        0.0f, ImGui::CalcTextSize(label, nullptr, true).x, ImGui::CalcTextSize(shortcut).x, mark);
    stretch = ImMax(0.0f, ImGui::GetContentRegionAvail().x - width);
  }
  const bool open = ImGui::BeginMenu(label);
  // Nach einem geoeffneten BeginMenu ist das Untermenue das aktuelle Fenster,
  // das Kuerzel gehoert aber in die Zeile darueber.
  if (draw) {
    const ImVec2 at(pos.x + window->DC.MenuColumns.OffsetShortcut + stretch, pos.y);
    window->DrawList->AddText(at, ImGui::GetColorU32(ImGuiCol_TextDisabled), shortcut);
  }
  return open;
}

}  // namespace

// ------------------------------------------------------------ FrameDelayLine

void FrameDelayLine::Configure(double delayMs) {
  if (delayMs <= 0.0) {
    delayMs_ = 0.0;
    Clear();
    return;
  }
  if (std::abs(delayMs - delayMs_) > 0.5) Clear();
  delayMs_ = delayMs;
}

void FrameDelayLine::Clear() {
  queue_.clear();
  pool_.clear();
  current_ = Entry{};
}

void FrameDelayLine::Push(const FrameView& frame, int64_t qpc) {
  if (!active() || !frame.valid()) return;
  Entry e;
  if (!pool_.empty()) {
    e = std::move(pool_.back());
    pool_.pop_back();
  }
  e.data.assign(frame.data, frame.data + frame.size);
  e.qpc = qpc;
  e.sequence = frame.sequence;
  queue_.push_back(std::move(e));

  // A pathological setting must not eat all memory.
  while (queue_.size() > 64) queue_.pop_front();
}

bool FrameDelayLine::Pop(FrameView* out, int64_t qpc) {
  if (!active()) return false;
  const int64_t threshold = qpc - SecondsToTicks(delayMs_ / 1000.0);
  bool got = false;
  while (!queue_.empty() && queue_.front().qpc <= threshold) {
    if (!current_.data.empty()) pool_.push_back(std::move(current_));
    current_ = std::move(queue_.front());
    queue_.pop_front();
    got = true;
  }
  if (!got || current_.data.empty()) return false;
  if (out) {
    out->data = current_.data.data();
    out->size = current_.data.size();
    out->sequence = current_.sequence;
  }
  return true;
}

// ----------------------------------------------------------------------- App

App::~App() {
  Shutdown();
}

bool App::Initialize() {
  std::string configError;
  // Load meldet dasselbe False fuer "keine Datei" und fuer "Datei kaputt". Der
  // Unterschied steht in error: beim allerersten Start ist es leer.
  firstRun_ = !config_.Load(&configError) && configError.empty();
  unfinishedSession_ = LogInit(config_.app.logToFile, config_.app.logRetention);
  if (unfinishedSession_.found) {
    const LocalTime& s = unfinishedSession_.started;
    CAP_WARN("Previous session (%s, started %04d-%02d-%02d %02d:%02d:%02d) has no end line",
             unfinishedSession_.version.c_str(), s.year, s.month, s.day, s.hour, s.minute,
             s.second);
    crashNoticeQueued_ = true;
  }
  if (!configError.empty()) CAP_WARN("%s", configError.c_str());

  // Settings inherited from a name this program no longer uses need one thing
  // done to them: the folders they leave empty have to be written down.
  //
  // An empty folder means "the one named after the program", and that name has
  // just changed -- so leaving them empty would quietly send new recordings to a
  // new folder while every recording made so far stayed behind in the old one.
  // Pinning them to what they resolved to yesterday keeps everything in one
  // place. Anyone who wants the new folder can pick it; nobody has to go looking
  // for files that moved on their own.
  if (!AdoptedFrom().empty()) {
    const std::string& was = AdoptedFrom();
    if (config_.record.outputFolder.empty()) {
      config_.record.outputFolder = PathToUtf8(DefaultRecordFolder(was));
    }
    if (config_.record.screenshotFolder.empty()) {
      config_.record.screenshotFolder = PathToUtf8(DefaultScreenshotFolder(was));
    }
    config_.Save();
    CAP_LOG("Settings adopted from %s; folders pinned",
            AdoptedFrom().c_str());
  }

  SetLanguage(config_.app.language);
  darkMode_ = ResolveDark(config_.app.theme);
  lastSerialized_ = config_.Serialize();

  if (!CreateMainWindow()) return false;

  std::string error;
  if (!display_.Initialize(window_, &error)) {
    ShowErrorMessage(error);
    return false;
  }
  if (!InitImGui()) return false;
  if (!renderer_.Initialize(&display_, &error)) {
    ShowErrorMessage(error);
    return false;
  }

  LoadIdleIcon();

  if (config_.app.startFullscreen) SetFullscreen(true);

  // Straight into the picture if we can; into the settings if we cannot.
  //
  // Beim allerersten Start aber nicht: zehn Reiter voller Optionen sind das
  // Erste, was jemand von diesem Programm sehen sollte, am allerwenigsten. Da
  // steht stattdessen der Willkommensbildschirm, der eine einzige Sache sagt --
  // welche Taste die Einstellungen oeffnet. Ab dem zweiten Start ist es wieder
  // die Abkuerzung, denn dann ist "kein Geraet" keine Begruessung mehr, sondern
  // ein Problem.
  if (firstRun_) {
    CAP_LOG("First start: welcome screen instead of settings");
    // Asked only for what is not there yet: a copy unpacked a second time, with
    // its settings file left behind, may still have its shortcuts.
    welcomeStartMenu_ = !HasShortcut(ShortcutPlace::StartMenu);
    welcomeDesktop_ = !HasShortcut(ShortcutPlace::Desktop);
    welcomeQueued_ = welcomeStartMenu_ || welcomeDesktop_;
  } else if (config_.active().capture.video.empty()) {
    OpenSettings(T("Noch kein Aufnahmegerät ausgewählt. Wähle unten die Capture-Karte aus.",
                   "No capture device selected yet. Pick your capture card below."));
  } else if (!StartCapture(&error)) {
    OpenSettings(error);
  }
  CaptureAppliedState();
  recording_.ffmpeg() = LocateFfmpeg(config_.record.ffmpegPath);
  // No test on startup. Which encoders work does not change from one run to the
  // next, so the answer is loaded from the config; testing is something the user
  // asks for once, or when the machine changed underneath it.
  recording_.LoadCachedEncoders();

  // The leftover of a previous update is cleared in wWinMain, before the name
  // is sorted out and the settings are read -- both of which would otherwise
  // work on the wrong file.
  CameraSink::CleanUpOldInstalls();
  if (config_.app.checkUpdatesOnStart) updater_.CheckAsync(true);

  running_ = true;
  return true;
}

void App::Shutdown() {
  // First, so nothing can be picked from its menu while the rest goes down.
  trayPopup_.Destroy();
  tray_.Hide();
  if (window_.created()) SaveWindowPlacement();
  // Before anything else: closing the pipes is what makes ffmpeg finalise the
  // container, and that has to happen while the app is still alive.
  recording_.Shutdown();
  standardSearch_.StopSignalWatch();
  settingsHost_.Destroy();
  StopCapture();
  mic_.Stop();
  audio_.Stop();
  renderer_.Shutdown();
  ShutdownImGui();
  display_.Shutdown();
  window_.Destroy();
  KeepDisplayAwake(false);
}

// -------------------------------------------------------------------- window

bool App::CreateMainWindow() {
  // First, so nothing the window reports while it is being created goes unheard.
  window_.SetListener([this](const WindowEvent& e) { return OnWindowEvent(e); });

  WindowSpec spec;
  spec.role = WindowRole::Main;
  spec.id = "MainWindow";
  spec.title = AppNameUtf8();
  spec.width = config_.app.windowW;
  spec.height = config_.app.windowH;
  // Zwei Gruende, die Stelle nicht zu benutzen, und beide sind echte Fragen.
  // Erstens: es wurde noch nie eine gespeichert -- das ist diese Abfrage.
  // Zweitens: die gespeicherte liegt heute auf keinem Bildschirm mehr; das
  // prueft das Fenster selbst.
  spec.hasPosition = config_.app.windowX != AppSettings::kWindowPosUnset &&
                     config_.app.windowY != AppSettings::kWindowPosUnset;
  spec.x = config_.app.windowX;
  spec.y = config_.app.windowY;
  spec.borderless = config_.app.borderless;

  switch (window_.Create(spec)) {
    case CreateResult::Ok:
      break;
    case CreateResult::RegistrationFailed:
      ShowErrorMessage(T("Fensterklasse konnte nicht registriert werden.",
                         "The window class could not be registered."));
      return false;
    case CreateResult::WindowFailed:
      ShowErrorMessage(
          T("Fenster konnte nicht erstellt werden.", "The window could not be created."));
      return false;
  }

  window_.SetDarkFrame(darkMode_);
  window_.ShowFirstTime(config_.app.maximized);
  ApplyWindowFlags();
  return true;
}

void App::SaveWindowPlacement() {
  if (!window_.created() || fullscreen_) return;
  Window::Placement placement;
  if (!window_.GetPlacement(&placement)) return;
  config_.app.maximized = placement.maximized;

  config_.app.windowX = placement.x;
  config_.app.windowY = placement.y;
  config_.app.windowW = std::max(160, placement.clientWidth);
  config_.app.windowH = std::max(120, placement.clientHeight);
}

void App::ApplyWindowFlags() {
  if (!window_.created()) return;
  // The driver's dialog is a top-level window of its own with no owner, which is
  // what keeps it out of our message loop -- and also means nothing lifts it
  // above a window that insists on staying on top. So while it is up, we do not.
  const bool top = config_.app.alwaysOnTop && !devicePages_.busy();
  window_.SetTopmost(top);
  // Second, so that it ends up in front of the preview rather than behind it.
  settingsHost_.SetTopmost(top);
}

void App::SetFullscreen(bool on) {
  if (!window_.created() || on == fullscreen_) return;

  if (on) {
    SaveWindowPlacement();

    // Which monitor: the configured one, otherwise the one the window is on.
    Rect target;
    bool haveTarget = false;
    if (config_.app.fullscreenMonitor >= 0) {
      std::vector<MonitorInfoEntry> monitors = EnumerateMonitors();
      if (config_.app.fullscreenMonitor < (int)monitors.size()) {
        target = monitors[(size_t)config_.app.fullscreenMonitor].rect;
        haveTarget = true;
      }
    }
    if (!haveTarget) haveTarget = window_.CurrentDisplayRect(&target);
    if (!haveTarget) return;

    // Straight under the settings window if that is where the keyboard is --
    // the shortcut was pressed there, and the window it was pressed in should
    // not vanish behind the picture it just made bigger.
    const Window* under = nullptr;
    if (settingsHost_.visible() && settingsHost_.window().IsForeground()) {
      under = &settingsHost_.window();
    }
    window_.EnterFullscreen(target, under, config_.app.alwaysOnTop);
    fullscreen_ = true;
  } else {
    window_.LeaveFullscreen();
    fullscreen_ = false;
    window_.SetCursorHidden(false);
    ApplyWindowFlags();
  }
  display_.Resize();
}

void App::ToggleFullscreen() {
  SetFullscreen(!fullscreen_);
}

// --------------------------------------------------------------------- ImGui

bool App::InitImGui() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();

  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;  // no imgui.ini next to the exe
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  uiScale_ = window_.DpiScale();
  LoadUiFont();
  ApplyImGuiTheme(darkMode_, config_.app.accentColor, uiScale_);

  if (!window_.AttachUi(nullptr)) return false;
  if (!display_.InitUi()) {
    // Messages went to ImGui only once both halves were up; the window must not
    // go on feeding them to half of it.
    window_.DetachUi();
    return false;
  }
  imguiReady_ = true;
  return true;
}

void App::ShutdownImGui() {
  if (!imguiReady_) return;
  display_.ShutdownUi();
  window_.DetachUi();
  ImGui::DestroyContext();
  imguiReady_ = false;
}

void App::ApplyTheme() {
  const bool dark = ResolveDark(config_.app.theme);
  darkMode_ = dark;
  ApplyImGuiTheme(dark, config_.app.accentColor, uiScale_);
  window_.SetDarkFrame(dark);
  if (settingsHost_.created()) settingsHost_.ApplyTheme(dark, config_.app.accentColor);
}

// ---------------------------------------------------------------------- tray

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

// ------------------------------------------------------------------- capture

bool App::StartCapture(std::string* error) {
  StopCapture();

  const Profile& p = config_.active();
  std::string err;
  if (!capture_.Start(p.capture, &err)) {
    captureState_ = CaptureState::Reconnecting;
    captureError_ = err;
    nextRetryQpc_ = ClockTicks() + SecondsToTicks(kRetrySeconds);
    if (error) *error = err;
    return false;
  }

  // Write back the device identity we actually opened, so a card that moved
  // slots keeps working on the next start.
  const VideoDeviceInfo& resolved = capture_.resolvedDevice();
  if (!resolved.id.empty()) {
    config_.active().capture.video.id = resolved.id;
    config_.active().capture.video.name = resolved.name;
  }

  // Dieselbe Ueberlegung fuer die Aufloesung: stand im Profil keine, hat die
  // Karte sie gerade ausgesucht, und niemand sonst weiss welche.
  //
  // Wird sie nicht aufgeschrieben, schreibt sie spaeter jemand anders auf, und
  // zwar teuer. Das Profil bleibt leer, der Abgleich in ApplyProfile sieht im
  // naechsten Bild einen Unterschied zu dem, was gerade gebaut wurde, und baut
  // den Graphen ein zweites Mal auf; wenn danach irgendwann die Einstellungen
  // gezeichnet werden, traegt EnsureValidFormat dieselbe Zahl nach, und es wird
  // ein drittes Mal aufgebaut -- dann mitten im Betrieb, mit schwarzem Bild und
  // Tonabriss. Beim Wechsel des Wuerfels auf 60 Hz stand genau das im Log:
  // 01:34:07,795 und 01:34:08,055 direkt hintereinander, 01:34:34,748
  // sechsundzwanzig Sekunden spaeter.
  //
  // Nur wenn nichts dastand. Eine von Hand erzwungene Aufloesung bleibt stehen,
  // auch wenn die Karte sie gerade nicht hergibt und ersatzweise etwas anderes
  // verbunden hat: sie ist ein Wunsch fuer jeden Start, kein Messwert, und
  // wegzuschreiben waere sie fuer immer weg.
  //
  // Und nur Pixelformat und Groesse. Die Bildrate bleibt, wie sie gewuenscht
  // war -- eine 0 heisst "hoechste verfuegbare" und muss eine 0 bleiben. Wuerde
  // hier die ausgehandelte Zahl hineingeschrieben, waere aus dem Wunsch eine
  // festgenagelte Rate geworden, und zwar die einer einzigen Norm: unter PAL B
  // steht dann 59,94 im Profil, die Karte lehnt jeden Kandidaten damit ab, und
  // im Log stapeln sich "SetFormat mit 59.940 fps abgelehnt".
  if (!config_.active().capture.format.valid() && capture_.connectedFormat().valid()) {
    FormatSel& stored = config_.active().capture.format;
    const double wishedFps = stored.fps;
    stored = capture_.connectedFormat();
    stored.fps = wishedFps;
  }

  // Was hier gebaut wurde, gilt ab jetzt als angewandt -- unabhaengig davon, ob
  // der Aufrufer gleich noch CaptureAppliedState() ruft. Die meisten Wege
  // hierher tun das naemlich nicht (UpdateVideoStandard, ReinitialiseCard, der
  // Wiederverbindungsversuch), und dann steht in applied_ noch das Format von
  // vorhin, obwohl es das schon nicht mehr gibt.
  applied_.format = config_.active().capture.format;

  std::string rendererError;
  renderer_.SetSourceFormat(capture_.format(), &rendererError);

  captureState_ = CaptureState::Running;
  captureError_.clear();
  retryCount_ = 0;
  secondFieldPending_ = false;
  sawFirstFrame_ = false;
  // Sonst misst der erste Durchgang nach einem Neustart gegen die Ankunft von
  // vor dem Neustart und meldet Sekunden.
  displayedArrivalQpc_ = 0;
  captureStartQpc_ = ClockTicks();

  // The dot crawl filter works on the colour subcarrier, so it has to be told
  // which one this signal carries. The card knows, when it has an analogue
  // decoder at all; without one the default covers the common case.
  const DeviceProbeResult& caps = capture_.capabilities();
  const long standard = caps.availableStandards != 0 ? caps.currentStandard : 0;
  renderer_.SetCarrierSamples(VideoStandardSubcarrierSamples(standard));
  // Unter welcher Zeilenzahl dieses Format ausgesucht wurde. Aus der Karte, weil
  // sie beim Graphenbau gefragt wird und die Einstellung im Profil "automatisch"
  // heissen kann; siehe ReleaseStandardBoundFormat.
  appliedStandardLines_ = VideoStandardLines(standard);

  StartAudio();
  UpdatePowerRequest();
  standardSearch_.StartSignalWatch();
  return true;
}

void App::StopCapture() {
  standardSearch_.StopSignalWatch();
  capture_.Stop();
  renderer_.DropFrame();
  delayLine_.Clear();
  captureState_ = CaptureState::Idle;
}

void App::StartAudio() {
  audio_.Stop();
  const Profile& p = config_.active();

  DeviceRef input;
  switch (p.capture.audioSource) {
    case AudioSource::None: return;
    case AudioSource::Manual: input = p.capture.audio; break;
    case AudioSource::Embedded:
    default: {
      AudioDeviceInfo found;
      if (FindEmbeddedAudioDevice(capture_.resolvedDevice(), &found)) {
        input = found.ToRef();
      } else {
        CAP_WARN("No embedded audio device found, sound stays off");
        return;
      }
      break;
    }
  }

  std::string err;
  if (!audio_.Start(input, p.audio, &err)) {
    CAP_WARN("Sound stays off");
    Toast(T("Ton konnte nicht gestartet werden: ", "Sound could not be started: ") + err);
  }
  delayLine_.Configure(std::max(0, -p.audio.avOffsetMs));
}

// Aufloesung und Bildrate loslassen, wenn die neue Norm eine andere Zeilenzahl
// hat als die, unter der sie ausgesucht wurden.
//
// 625/50 liefert 720x576, 525/60 liefert 720x480 -- eine Groesse, die unter der
// einen Norm gewaehlt wurde, ist unter der anderen falsch. Im Profil steht sie
// trotzdem, und der naechste Graphenbau fordert genau sie wieder an. Am
// GameCube heisst das: zwischen 50 und 60 Hz umschalten, und die Anzeige bleibt
// auf der alten Groesse stehen, bis jemand von Hand ins Aufklappmenue geht.
// Die Liste dort ist da laengst richtig -- sie kommt aus der laufenden Karte --,
// nur wird nichts daraus genommen, solange die alte Wahl noch dasteht. Genau
// dieses Haengenbleiben am Nutzer soll weg.
//
// Zurueck auf 0 heisst "such es dir aus", nicht "nimm irgendwas": die Auswahl
// beim Graphenbau geht ueber das, was die Karte unter der jetzigen Norm
// tatsaechlich anbietet.
//
// Das Pixelformat bleibt stehen, aus demselben Grund wie in ReinitialiseCard:
// es haengt an der Karte, nicht an der Norm. Wer RGB32 ausgesucht hat, will es
// nach dem Umschalten immer noch.
bool App::ReleaseStandardBoundFormat(int newLines) {
  if (appliedStandardLines_ <= 0 || newLines <= 0) return false;
  if (appliedStandardLines_ == newLines) return false;
  FormatSel& f = config_.active().capture.format;
  if (f.width <= 0 && f.height <= 0 && f.fps <= 0.0) return false;
  CAP_LOG("Video standard: %d lines instead of %d -- releasing %s", newLines, appliedStandardLines_,
          f.Label().c_str());
  f.width = 0;
  f.height = 0;
  // Eine Zahl faellt weg, eine Betriebsart nicht. "Hoechste verfuegbare" und
  // "die des Signals" sind keine Werte, die an der Norm haengen -- sie sind die
  // Anweisung, nach dem Umschalten neu zu antworten, und genau die soll das
  // Umschalten nicht loeschen. Eine feste Zahl dagegen wird zur Rate des
  // Signals: sie war unter der alten Norm gewaehlt, und die neue sagt selbst,
  // welche richtig ist.
  if (f.fps > 0.0) f.fps = kFpsNative;
  f.forced = false;
  return true;
}

// Die Karte von vorn aufmachen, nicht nur den Graphen neu bauen.
//
// Der Unterschied ist, was verworfen wird. Ein Neustart nimmt die Einstellungen
// mit, die gerade dastehen -- und genau die sind das Problem, wenn zwischen dem
// analogen und dem digitalen Eingang umgesteckt wurde: Videonorm und Format
// beschreiben, was an der *vorherigen* Buchse hing. Eine auf PAL festgehaltene
// Karte liefert dann 720x576 bei 50 Hz an einem Eingang, an dem etwas voellig
// anderes anliegt.
//
// Geraet und Eingang bleiben stehen. Die hat jemand ausgesucht; Norm und Format
// hat qBlank gemessen oder geraten, und Gemessenes darf weg.
void App::ReinitialiseCard() {
  StopCapture();

  CaptureSettings& c = config_.active().capture;
  const bool hadStandard = c.videoStandard > 0;
  const std::string keptSubtype = c.format.subtype;
  // Dieselbe Unterscheidung wie in ReleaseStandardBoundFormat: eine Zahl haengt
  // an dem Format, das gerade weggeworfen wird, eine Betriebsart nicht. "Die des
  // Signals" ist keine Messung, die neu gemacht werden muesste, sondern die
  // Anweisung, wie nach dem Neueinlesen zu antworten ist -- und die soll das
  // Neueinlesen nicht loeschen. Sonst steht hinterher wieder "hoechste
  // verfuegbare" da, obwohl niemand das ausgesucht hat.
  const double keptFps = c.format.fps <= 0.0 ? c.format.fps : kFpsNative;
  c.videoStandard = -1;  // wieder suchen lassen

  // Aufloesung wieder suchen lassen, das Pixelformat nicht. Eine Karte, die eben
  // noch RGB32 konnte, kann es nach dem Neueinlesen immer noch, und wer es
  // ausgewaehlt hat, will es nicht jedes Mal neu auswaehlen. Kann sie es
  // wirklich nicht mehr, faellt die Auswahl beim Start zurueck.
  c.format = FormatSel{};
  c.format.subtype = keptSubtype;
  c.format.fps = keptFps;

  // Die Geraeteliste ebenfalls, denn eine umgesteckte Karte kann unter einem
  // anderen Pfad auftauchen als der, den wir uns gemerkt haben.
  settings_.InvalidateDeviceLists();

  CAP_LOG("Re-reading the card: video standard %s, resolution dropped, pixel format %s",
          hadStandard ? "dropped" : "already automatic",
          keptSubtype.empty() ? "already automatic" : keptSubtype.c_str());

  std::string error;
  if (StartCapture(&error)) {
    Toast(keptSubtype.empty()
              ? T("Karte neu eingelesen. Videonorm und Format stehen wieder auf automatisch.",
                  "Card reinitialised. Video standard and format are back to automatic.")
              : T("Karte neu eingelesen. Videonorm und Auflösung wieder automatisch, "
                  "Pixelformat beibehalten.",
                  "Card reinitialised. Video standard and resolution back to automatic, "
                  "pixel format kept."));
  } else {
    Toast(error);
  }
}

// Wie lange die Anzeige stehen darf, wenn kein Bild ankommt.
//
// Das Video braucht keinen Boden: kommt nichts, gibt es nichts Neues zu zeigen,
// und 200 ms halten den Ruhebildschirm am Leben, ohne Rechenzeit fuer ein
// unveraendertes Bild zu verbrennen.
//
// Die Bedienoberflaeche ist etwas anderes. Sie bewegt sich aus eigener Kraft --
// und das eingebettete Einstellungsfeld wird von genau dieser Schleife
// gezeichnet. Mit dem Boden fuer das Video lief es ohne Aufnahmegeraet mit
// gemessenen 4,7 Bildern in der Sekunde, was sich anfuehlt wie zwei.
//
// Es geht dabei nicht darum, ob ein Signal anliegt, sondern ob etwas auf dem
// Schirm ist, das sich bewegen koennen muss. Liegt ein Signal an, gibt dessen
// Takt ohnehin alles vor und dieser Boden kommt nie zum Tragen.
double App::IdleFloorMs() const {
  const bool embeddedPanel = settings_.isOpen() && !config_.app.settingsSeparateWindow;
  const double now = ImGui::GetTime();
  const bool toastUp = !toastText_.empty() && now - toastStart_ <= 2.5;
  const bool osdUp = now - volumeOsdStart_ <= kVolumeOsdSeconds;
  if (embeddedPanel || cropTool_.active() || toastUp || osdUp) return 16.0;
  return 200.0;
}

void App::RestartAll(bool userRequested) {
  std::string error;
  if (StartCapture(&error)) {
    if (userRequested) Toast(T("Aufnahme neu gestartet", "Capture restarted"));
  } else if (userRequested) {
    Toast(error);
  }
}

// ------------------------------------------------------------------ settings

void App::OpenSettings(const std::string& reason) {
  settings_.Open(&config_, reason);
  // Already open in its own window, but possibly behind the preview since the
  // two no longer stack together: asking for the settings means wanting to see
  // them.
  settingsHost_.Raise();
}

void App::CaptureAppliedState() {
  const Profile& p = config_.active();
  applied_.activeProfile = config_.activeProfile;
  applied_.video = p.capture.video;
  applied_.format = p.capture.format;
  applied_.crossbarInput = p.capture.crossbarInput;
  applied_.videoStandard = p.capture.videoStandard;
  applied_.audioSource = p.capture.audioSource;
  applied_.audioIn = p.capture.audio;
  applied_.audioOut = p.audio.output;
  applied_.exclusive = p.audio.exclusive;
  applied_.bufferMs = p.audio.bufferMs;
  applied_.avOffsetMs = p.audio.avOffsetMs;
  applied_.volume = p.audio.volume;
  applied_.mute = p.audio.mute;
  applied_.theme = config_.app.theme;
  applied_.accent = config_.app.accentColor;
  applied_.alwaysOnTop = config_.app.alwaysOnTop;
  applied_.borderless = config_.app.borderless;
  applied_.language = config_.app.language;
}

void App::SyncConfigChanges() {
  const Profile& p = config_.active();

  if (config_.app.language != applied_.language) {
    SetLanguage(config_.app.language);
  }
  if (config_.app.theme != applied_.theme || config_.app.accentColor != applied_.accent) {
    ApplyTheme();
  }
  if (config_.app.alwaysOnTop != applied_.alwaysOnTop ||
      devicePages_.busy() != devicePagesWereBusy_) {
    devicePagesWereBusy_ = devicePages_.busy();
    ApplyWindowFlags();
  }
  if (config_.app.borderless != applied_.borderless) {
    window_.SetBorderless(config_.app.borderless);
  }

  // Anything that changes what the card is asked to produce needs the graph
  // rebuilt. Everything else is applied without interrupting the picture.
  const bool profileChanged = config_.activeProfile != applied_.activeProfile;
  const bool deviceChanged = !(p.capture.video == applied_.video);
  const bool formatChanged = !p.capture.format.SameFormat(applied_.format) ||
                             std::abs(p.capture.format.fps - applied_.format.fps) > 0.01;
  // A different standard usually means a different number of lines, so the graph
  // has to come up again around it. Automatic is the exception: it has nothing
  // to apply until it has found something, and rebuilds by itself when it does.
  const bool standardChanged = p.capture.videoStandard != applied_.videoStandard &&
                               p.capture.videoStandard > 0;
  if (standardChanged) {
    CAP_LOG("Video standard changed: %s -> %s, rebuilding the graph",
            VideoStandardSettingName(applied_.videoStandard).c_str(),
            VideoStandardSettingName(p.capture.videoStandard).c_str());
    // Vor dem Neubau, denn der liest die Groesse aus dem Profil.
    ReleaseStandardBoundFormat(VideoStandardLines(p.capture.videoStandard));
  }
  // Zurueck auf Automatisch: die Suche faengt von vorne an. Das ist keine
  // Selbstverstaendlichkeit, sondern eine Entscheidung -- ohne diesen Block
  // ueberlebt `standardCandidate_` das Umschalten und die Suche setzt dort
  // fort, wo sie beim Verlassen stand. Das waere zweimal falsch: die
  // Kandidatenliste wird bei jedem Aufruf neu sortiert, ein alter Index zeigt
  // darin auf eine andere Norm als damals, und die vorderen Plaetze -- die
  // Geschwisternorm, die zuletzt gute, die Region -- sind gerade die besten
  // Vermutungen und wuerden uebersprungen. Wer von Hand auf Automatisch
  // zurueckstellt, will die beste Vermutung, nicht die naechste.
  //
  // `standardLastGood_` bleibt absichtlich stehen: dass hier eine Norm schon
  // einmal gehalten hat, ist auch nach dem Umschalten noch wahr.
  //
  // `standardLostQpc_` wird hier *nicht* angefasst. Es steht fuer den Moment,
  // in dem der Lock verloren ging, und daran hat das Umschalten nichts
  // geaendert. Genullt hiesse: die Schonfrist laeuft neu, und die Zeile "Lock
  // auf ... verloren" wird ein zweites Mal geschrieben -- am 30.08. um 08:53:15
  // genau so im Log gestanden, zweimal im Abstand von 148 ms.
  if (p.capture.videoStandard == -1 && applied_.videoStandard != -1) {
    standardSearch_.StartOver();
  }

  if (profileChanged || deviceChanged || formatChanged || standardChanged) {
    std::string error;
    if (!StartCapture(&error)) {
      // Keep the dialog open on the failing setting instead of closing over it.
      if (!settings_.isOpen()) OpenSettings(error);
      Toast(error);
    }
    CaptureAppliedState();
    UpdatePowerRequest();
    return;
  }

  if (p.capture.crossbarInput != applied_.crossbarInput) {
    capture_.SetCrossbarInput(p.capture.crossbarInput);
    // A different input is a different signal on the same format: levels, field
    // structure and where the picture sits all have to be measured again.
    renderer_.ResetAnalysis();
  }

  const bool audioRouteChanged = p.capture.audioSource != applied_.audioSource ||
                                 !(p.capture.audio == applied_.audioIn) ||
                                 !(p.audio.output == applied_.audioOut) ||
                                 p.audio.exclusive != applied_.exclusive;
  if (audioRouteChanged) {
    StartAudio();
  } else if (p.audio.bufferMs != applied_.bufferMs || p.audio.avOffsetMs != applied_.avOffsetMs ||
             p.audio.volume != applied_.volume || p.audio.mute != applied_.mute) {
    audio_.ApplySettings(p.audio);
    delayLine_.Configure(std::max(0, -p.audio.avOffsetMs));
  }

  CaptureAppliedState();
}

void App::MaybeSaveConfig() {
  const double now = ImGui::GetTime();

  // Serialising four times a second is cheap and means no field can be
  // forgotten here when one gets added to the config.
  if (now - lastSerializeCheck_ >= 0.25) {
    lastSerializeCheck_ = now;
    std::string current = config_.Serialize();
    if (current != lastSerialized_) {
      lastSerialized_ = std::move(current);
      configDirty_ = true;
      lastConfigChange_ = now;
    }
  }

  // Wait for the user to stop fiddling before touching the disk.
  if (configDirty_ && now - lastConfigChange_ >= 1.0) {
    configDirty_ = false;
    std::string error;
    config_.Save(&error);
  }
}

void App::SaveConfig() {
  // Wherever the settings window stands right now goes into what is about to be
  // written -- otherwise it is only remembered when the mode is toggled, and a
  // window moved and then left alone would come back somewhere else.
  RememberSettingsWindow();
  SaveWindowPlacement();
  std::string error;
  if (!config_.Save(&error)) Toast(error);
}

void App::SwitchProfile(int index) {
  if (index < 0 || index >= (int)config_.profiles.size()) return;
  if (index == config_.activeProfile) return;
  config_.SetActiveProfile(index);
  std::string error;
  if (StartCapture(&error)) {
    Toast(Format(T("Profil %d: %s", "Profile %d: %s"), index + 1, config_.active().name.c_str()));
  } else {
    Toast(error);
  }
  CaptureAppliedState();
  SaveConfig();
}

// --------------------------------------------------------------------- misc

namespace {

// So lange steht ein Toast. Einer zu einer Datei etwas laenger: den will man
// vielleicht noch anklicken, und bis die Maus unten in der Mitte ist, waere
// der kurze schon halb verblasst.
constexpr double kToastSeconds = 2.5;
constexpr double kFileToastSeconds = 4.0;
// Wie viele Toasts zu einer Datei dazusagen, dass ein Klick sie zeigt.
constexpr int kFileToastHints = 3;

}  // namespace

void App::Toast(const std::string& text, const std::filesystem::path& file) {
  toastText_ = text;
  toastFile_ = file;
  toastTouched_ = false;
  toastStart_ = ImGui::GetTime();
  // Beim Entstehen entschieden, nicht je Bild: sonst verschwaende die Zeile
  // beim letzten Mal mitten im Toast, sobald der Zaehler oben ankommt.
  toastHint_ = !file.empty() && config_.app.fileToastHints < kFileToastHints;
  if (toastHint_) ++config_.app.fileToastHints;
}

// Ein Toast zu einer Datei -- Aufnahme gespeichert, Screenshot -- zeigt sie auf
// Klick im Explorer. Solange die Maus darauf liegt, bleibt er stehen: wer
// hinzeigt, will ihn noch lesen oder gleich anklicken. Aber nur, wenn sie sich
// dort auch bewegt hat. Eine Maus, die zufaellig unten in der Mitte parkt,
// hielte ihn sonst fuer immer ueber dem Bild.
void App::DrawToastStrip() {
  if (toastText_.empty()) return;
  const bool clickable = !toastFile_.empty();
  const double duration = clickable ? kFileToastSeconds : kToastSeconds;
  const double age = ImGui::GetTime() - toastStart_;
  if (age > duration) {
    toastText_.clear();
    toastFile_.clear();
    return;
  }

  const ToastResult result =
      DrawToast(toastText_, age, duration, clickable,
                toastHint_ ? T("Klicken zeigt die Datei im Ordner", "Click to show the file in its folder")
                           : nullptr);
  if (!result.hovered) return;

  const ImVec2 delta = ImGui::GetIO().MouseDelta;
  if (delta.x != 0.0f || delta.y != 0.0f) toastTouched_ = true;
  if (toastTouched_) toastStart_ = ImGui::GetTime();

  if (result.clicked) {
    // Wer einmal geklickt hat, braucht den Hinweis nicht mehr.
    config_.app.fileToastHints = kFileToastHints;
    if (!PathExists(toastFile_)) {
      Toast(T("Die Datei ist nicht mehr da.", "The file is no longer there."));
      return;
    }
    ShowFileInFolder(toastFile_);
    toastText_.clear();
    toastFile_.clear();
  }
}

void App::UpdatePowerRequest() {
  const uint32_t now = TickMilliseconds();
  if (now - lastPowerPokeTick_ < 30000) return;
  lastPowerPokeTick_ = now;
  KeepDisplayAwake(config_.app.preventSleep && captureState_ == CaptureState::Running);
}

void App::AdjustVolume(float delta) {
  AudioSettings& a = config_.active().audio;
  a.volume = Clamp(a.volume + delta, 0.0f, 1.0f);
  a.mute = false;
  audio_.ApplySettings(a);
  applied_.volume = a.volume;
  applied_.mute = a.mute;
  ShowVolumeOsd();
}

void App::ToggleMute() {
  AudioSettings& a = config_.active().audio;
  a.mute = !a.mute;
  audio_.ApplySettings(a);
  applied_.mute = a.mute;
  ShowVolumeOsd();
}

// ------------------------------------------------------------------ recording

void App::ToggleFreeze() {
  // Waehrend einer Aufnahme gibt es kein Standbild: die Aufnahme und die
  // virtuelle Kamera holen ihr Bild aus derselben Textur wie die Anzeige, ein
  // Standbild landete also in der Datei.
  if (recorder_.recording()) {
    Toast(T("Während der Aufnahme geht kein Standbild.",
            "No freeze while recording."));
    return;
  }
  frozen_ = !frozen_;
  if (!frozen_) {
    // Was waehrend des Standbilds in der Verzoegerungsleitung stehen geblieben
    // ist, ist inzwischen so alt wie das Standbild lang war. Es auszugeben
    // hiesse, beim Fortsetzen erst einmal die Vergangenheit abzuspielen.
    delayLine_.Clear();
  }
  Toast(frozen_ ? T("Standbild", "Frozen") : T("Standbild aus", "Running again"));
}

void App::ToggleCompare() {
  // Aus demselben Grund wie beim Standbild: der Schnitt liegt im ersten
  // Durchgang, und die Aufnahme holt ihr Bild hinter diesem Durchgang ab. Eine
  // halb gefilterte Datei will niemand.
  if (recorder_.recording()) {
    Toast(T("Während der Aufnahme geht kein Vergleich.", "No compare while recording."));
    return;
  }
  compare_ = !compare_;
  // Neben einem Bild ganz ohne Filter gibt es nichts zu vergleichen.
  if (compare_) bypass_ = false;
  if (!compare_) {
    Toast(T("Vergleich aus", "Compare off"));
  } else {
    ToastCompareSide();
  }
}

void App::ToastCompareSide() {
  if (config_.active().image.compareHorizontal) {
    Toast(T("Vergleich: über der Linie ohne Filter", "Compare: filters off above the line"));
  } else {
    Toast(T("Vergleich: links ohne Filter", "Compare: filters off on the left"));
  }
}

// Aus dem Kontextmenue: eine Richtung waehlen zeigt den Vergleich in dieser
// Richtung, die schon gezeigte noch einmal waehlen schaltet ihn aus.
void App::ChooseCompare(bool horizontal) {
  bool& across = config_.active().image.compareHorizontal;
  if (compare_ && across == horizontal) {
    ToggleCompare();
    return;
  }
  across = horizontal;
  if (compare_) {
    ToastCompareSide();
  } else {
    ToggleCompare();
  }
}

// Das ganze Bild ohne Filter, fuer die Frage, ob ueberhaupt einer etwas taugt.
//
// Weg ist alles, was am Signal etwas veraendert: die Composite-Kette,
// Schaerfen, die vier Bildregler, das native Raster und die Bildroehre. Stehen
// bleibt, was das Bild erst richtig hinstellt -- Deinterlacing, Zuschnitt,
// Seitenverhaeltnis, Drehung, Skalierung, Wertebereich und Matrix. Ohne die
// waere das Bild nicht ungefiltert, sondern falsch.
//
// Das Deinterlacing stand frueher auf der anderen Seite und schaltete auf
// Weave. Aber Kammlinien sind nicht das Signal, wie es ankommt, sondern zwei
// Halbbilder, die nie zusammen gezeigt werden sollten; neben ihnen war jeder
// Filter schlecht zu beurteilen.
//
// Der Vergleich (F12) ist dasselbe auf einer Haelfte: links fehlt genau, was
// hier fehlt, und das Deinterlacing laeuft auf beiden Seiten. Die Kette faellt
// im ersten Durchgang weg, der Rest im Skalierdurchgang (CompareRaw).
//
// Aus denselben Gruenden wie das Standbild nicht waehrend einer Aufnahme, und
// ebenso wenig im Profil: siehe EffectiveImage.
void App::ToggleBypass() {
  if (recorder_.recording()) {
    Toast(T("Während der Aufnahme bleiben die Filter an.", "Filters stay on while recording."));
    return;
  }
  bypass_ = !bypass_;
  if (bypass_) compare_ = false;
  Toast(bypass_ ? T("Alle Filter aus", "All filters off") : T("Filter wieder an", "Filters back on"));
}

void App::CropHost::Toast(const std::string& text) { app_.Toast(text); }
void App::CropHost::OpenSettings() { app_.OpenSettings({}); }
void App::CropHost::CloseSettings() { app_.settings_.Close(); }

bool App::RecordingHost::CaptureRunning() const {
  return app_.captureState_ == CaptureState::Running;
}
std::filesystem::path App::RecordingHost::ResolveOutputFolder(
    std::string* configured, const std::filesystem::path& fallback) {
  return app_.ResolveOutputFolder(configured, fallback);
}
void App::RecordingHost::DropViewAids() {
  if (app_.frozen_) {
    app_.frozen_ = false;
    app_.delayLine_.Clear();
  }
  app_.compare_ = false;
  app_.bypass_ = false;
}
void App::RecordingHost::Toast(const std::string& text, const std::filesystem::path& file) {
  app_.Toast(text, file);
}

std::filesystem::path App::ResolveOutputFolder(std::string* configured,
                                               const std::filesystem::path& fallback) {
  std::filesystem::path::string_type folder =
      configured->empty() ? fallback.native() : Utf8ToPath(*configured).native();
  while (!folder.empty() && (folder.back() == '\\' || folder.back() == '/')) folder.pop_back();

  // Deleting the folder between two recordings is normal housekeeping, so the
  // first answer is simply to make it again.
  if (EnsureFolder(folder)) return folder;

  // It cannot be created either -- an unplugged drive, a path that is no longer
  // writable. Rather than refuse, fall back to the default and clear the custom
  // path so the settings show where the files are actually going now.
  CAP_WARN("Folder not available: %s", PathToUtf8(folder).c_str());
  if (!configured->empty() && EnsureFolder(fallback)) {
    Toast(T("Ordner nicht verfügbar, Standardordner wird benutzt.",
            "Folder not available, using the default folder."));
    configured->clear();
    settings_.InvalidateFolderFields();
    return fallback;
  }
  return {};
}

void App::ShowOutputFolder(std::string* configured, const std::filesystem::path& fallback) {
  const std::filesystem::path folder = ResolveOutputFolder(configured, fallback);
  if (folder.empty()) {
    Toast(T("Zielordner nicht verfügbar.", "Folder not available."));
    return;
  }
  OpenFolder(folder);
}

void App::DrawToolbarStrip() {
  ToolbarState state;
  state.recording = recorder_.recording();
  state.recordSeconds = state.recording ? recorder_.stats().seconds : 0.0;
  state.muted = config_.active().audio.mute;
  state.volume = config_.active().audio.volume;
  state.canRecord = captureState_ == CaptureState::Running && renderer_.hasFrame();

  const ToolbarResult result = DrawToolbar(state, config_.app.accentColor);
  if (result.volume >= 0.0f) {
    config_.active().audio.volume = result.volume;
    if (config_.active().audio.mute) config_.active().audio.mute = false;
    audio_.ApplySettings(config_.active().audio);
    ShowVolumeOsd();
  }

  switch (result.action) {
    case ToolbarAction::ToggleRecording: recording_.ToggleRecording(); break;
    case ToolbarAction::Screenshot: RequestScreenshot(); break;
    case ToolbarAction::Settings: OpenSettings({}); break;
    case ToolbarAction::OpenRecordFolder:
      ShowOutputFolder(&config_.record.outputFolder, DefaultRecordFolder());
      break;
    case ToolbarAction::OpenScreenshotFolder:
      ShowOutputFolder(&config_.record.screenshotFolder, DefaultScreenshotFolder());
      break;
    case ToolbarAction::ToggleMute: ToggleMute(); break;
    case ToolbarAction::Hide: config_.app.showToolbar = false; break;
    default: break;
  }
}

void App::WriteScreenshot(bool includeUi, bool toClipboard) {
  RecordSettings& rec = config_.record;

  std::vector<uint8_t> pixels;
  std::vector<uint16_t> halfPixels;
  int width = 0, height = 0, halfStride = 0;
  bool wide = false;
  std::string note;

  if (includeUi) {
    // The whole window as it stands, which is what "with the interface" has to
    // mean: the picture scaled into the window, the bar, the overlays. Only
    // possible in eight bit -- an HDR back buffer is scRGB float, and the
    // conversion out of it is not something to guess at. There, the picture
    // itself is saved instead and the message says so.
    if (!display_.GrabBackBuffer(&pixels, &width, &height)) {
      includeUi = false;
      note = T(" (ohne Oberfläche, HDR-Ausgabe)", " (without interface, HDR output)");
    }
  }

  if (!includeUi) {
    // Keeping the range only means anything when there is a range to keep, so
    // the setting and the source both have to say so. Otherwise this is an
    // ordinary screenshot and takes the ordinary path. The clipboard is always
    // the ordinary path: there is no way to hand a wide range over it.
    wide = !toClipboard && config_.app.screenshotHdr &&
           renderer_.hdrTransfer() != VideoRenderer::Transfer::Sdr;
    if (wide) {
      if (!renderer_.GrabStillHalf(&halfPixels, &width, &height, &halfStride)) {
        Toast(T("Kein Bild zum Speichern.", "No picture to save."));
        return;
      }
    } else if (!renderer_.GrabStill(&pixels, &width, &height)) {
      Toast(T("Kein Bild zum Speichern.", "No picture to save."));
      return;
    }
  }

  if (toClipboard) {
    std::string clipError;
    if (!CopyScreenshotToClipboard(&window_, pixels.data(), width, height, &clipError)) {
      Toast(T("Kopieren fehlgeschlagen: ", "Copy failed: ") + clipError);
      return;
    }
    Toast(T("In der Zwischenablage", "On the clipboard") +
          Format(" (%dx%d)", width, height) + note);
    CAP_LOG("Screenshot copied to the clipboard (%dx%d)", width, height);
    return;
  }

  const std::filesystem::path folder =
      ResolveOutputFolder(&rec.screenshotFolder, DefaultScreenshotFolder());
  const std::filesystem::path path =
      folder.empty() ? std::filesystem::path()
      : wide          ? MakeHdrScreenshotPath(folder, config_.app.hdrShotFormat)
                      : MakeScreenshotPath(folder, rec.screenshotFormat);
  if (path.empty()) {
    Toast(T("Zielordner nicht verfügbar.", "Folder not available."));
    CAP_ERR("Screenshot: folder not available: %s", PathToUtf8(folder).c_str());
    return;
  }

  std::string error;
  const bool ok =
      !wide ? SaveScreenshot(path, pixels.data(), width, height, rec.screenshotFormat,
                             rec.jpegQuality, &error)
      : config_.app.hdrShotFormat == HdrShotFormat::Avif
          ? SaveScreenshotAvif(path, Utf8ToPath(recording_.ffmpeg().path), halfPixels.data(),
                               width, height, halfStride, config_.app.paperWhiteNits, &error)
          : SaveScreenshotHdr(path, halfPixels.data(), width, height, halfStride,
                              config_.app.paperWhiteNits, &error);
  if (!ok) {
    Toast(T("Screenshot fehlgeschlagen: ", "Screenshot failed: ") + error);
    return;
  }

  // The file name, not the whole path: the path is long, and the point of the
  // message is "it worked and it is called this".
  Toast(T("Screenshot: ", "Screenshot: ") + PathToUtf8(path.filename()) + note, path);
  CAP_LOG("Screenshot saved: %s (%dx%d)", PathToUtf8(path).c_str(), width, height);
}

// ------------------------------------------------------------ video standard

bool App::StandardSearchHost::CaptureRunning() const {
  return app_.captureState_ == CaptureState::Running;
}
bool App::StandardSearchHost::SourceIsAnalogue() const { return app_.SourceIsAnalogue(); }
AnalogConnector App::StandardSearchHost::ResolvedConnector() const {
  return app_.ResolvedConnector();
}
bool App::StandardSearchHost::ConnectorHasColourCarrier() const {
  return app_.ConnectorHasColourCarrier();
}
long App::StandardSearchHost::AppliedVideoStandard() const {
  return app_.applied_.videoStandard;
}
bool App::StandardSearchHost::ReleaseStandardBoundFormat(int newLines) {
  return app_.ReleaseStandardBoundFormat(newLines);
}
bool App::StandardSearchHost::StartCapture(std::string* error) {
  return app_.StartCapture(error);
}
void App::StandardSearchHost::Toast(const std::string& text) { app_.Toast(text); }

void App::RemeasureRange() {
  if (captureState_ != CaptureState::Running) {
    Toast(T("Keine laufende Quelle.", "No source is running."));
    return;
  }
  renderer_.ResetRangeAnalysis();
  CAP_LOG("Signal range: measurement restarted by hand");
  // Gesagt werden muss es, weil sonst nichts passiert, was man sehen koennte:
  // die Messung braucht vierzig Bilder, und bis dahin steht in der Anzeige
  // dasselbe wie vorher.
  Toast(config_.active().image.range == ColorRange::Auto
            ? T("Wertebereich wird neu gemessen.", "Measuring the range again.")
            : T("Wertebereich wird neu gemessen — verwendet wird die feste Einstellung.",
                "Measuring the range again — the fixed setting is what gets used."));
}

// The program's own icon, as a texture for the empty state.
void App::LoadIdleIcon() {
  if (idleIcon_ || !display_.initialized()) return;

  int w = 0, h = 0;
  std::vector<uint8_t> pixels = AppIconRgba(256, &w, &h);
  if (pixels.empty()) return;

  // Premultiplying by alpha is the one thing still to do here, and it belongs
  // here: ImGui's blend state is premultiplied, and handing it straight alpha
  // draws a dark halo around every edge of the icon.
  for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
    const unsigned a = pixels[i + 3];
    pixels[i + 0] = (uint8_t)(pixels[i + 0] * a / 255u);
    pixels[i + 1] = (uint8_t)(pixels[i + 1] * a / 255u);
    pixels[i + 2] = (uint8_t)(pixels[i + 2] * a / 255u);
  }

  UiImage image = display_.CreateUiImage(pixels.data(), w, h);
  if (!image) return;
  idleIcon_ = image;
  idleIconSize_ = w;
}

// Frames arriving is the whole answer on a digital input and no answer at all on
// an analogue one, where the card keeps delivering whatever an open wire decodes
// to. So the pixels are asked as well -- and the decoder's own lock is used only
// to say "no" faster, never to say "yes": this card reports a lock on some
// standards with nothing connected, which is measured and written down in the
// wiki.
bool App::HaveLiveSignal() const {
  const FrameBuffer* sink = capture_.sink();
  if (!sink || !sink->HasRecentFrame(kNoSignalSeconds) || !renderer_.hasFrame()) return false;

  // Solange die automatische Normensuche laeuft, sind die Pixel kein Zeuge.
  //
  // Jeder Normwechsel wirft fuer einen Moment breite gruene Streifen durch das
  // blaue Bild, waehrend der Decoder neu synchronisiert. Ein einziger solcher
  // Blitz liest sich als "Picture", nimmt unten die Abkuerzung und setzt die
  // Geduld wieder auf null -- bei einer Suche, die alle paar hundert
  // Millisekunden umschaltet, kommt die Kein-Signal-Anzeige deshalb nie.
  //
  // Nach einer vollen Runde ohne Lock ist die Sache aber entschieden: es wurde
  // jede Norm durchprobiert, die die Karte kann, und keine hat gegriffen. Was
  // das Bild dann noch zeigt, haben wir selbst verursacht.
  //
  // Eine volle Runde ist seit dem zweiten Durchgang beides zusammen. Der
  // schnelle allein entscheidet nichts -- er darf zu ungeduldig gewesen sein,
  // das ist sein Zweck --, und "kein Signal" auf eine Auskunft zu stuetzen,
  // die gerade nachgeprueft wird, waere voreilig. `!standardPatientPass_`
  // trifft genau den Zustand nach einem beendeten geduldigen Durchgang: die
  // Fahne steht waehrend des zweiten Durchgangs und faellt mit seinem Ende.
  if (standardSearch_.sweptWithoutLock() && standardSearch_.signalLocked() == 0) {
    return false;
  }

  const VideoRenderer::SignalVerdict verdict = renderer_.detectedSignal();
  if (verdict == VideoRenderer::SignalVerdict::Picture ||
      verdict == VideoRenderer::SignalVerdict::Unknown) {
    return true;
  }

  // Neither of the two failure verdicts is acted on the instant it appears.
  //
  // Snow is the confident one, but a hard cut between two busy scenes can clear
  // both its thresholds for a single measurement, and a viewer that blinks the
  // idle screen mid-game is worse than one that takes a moment to notice a
  // pulled cable. A second is far longer than any cut and far shorter than
  // anyone's patience with a dead input.
  //
  // Flat is the ambiguous one: a black loading screen measures exactly like a
  // muted input, so only time separates them at all. A decoder that says it has
  // no lock is reason enough to stop waiting for a second opinion.
  //
  // An einem digitalen Eingang laeuft der Wachthread gar nicht, `locked` ist
  // dann -1 und es bleibt bei der langen Geduld. Das ist genau richtig: dort
  // sitzt der Decoder nicht im Signalweg, seine Meinung waere geraten.
  double patience = kSnowSeconds;
  if (verdict == VideoRenderer::SignalVerdict::Flat) {
    const int locked = standardSearch_.signalLocked();
    patience = locked == 0 ? kFlatUnlockedSeconds : kFlatSeconds;
  }
  return renderer_.signalHeldSeconds() < patience;
}

void App::RememberSettingsWindow() {
  if (!settingsHost_.created()) return;
  const SettingsHost::Placement where = settingsHost_.placement();
  if (where.width <= 0 || where.height <= 0) return;
  config_.app.settingsWindowX = where.x;
  config_.app.settingsWindowY = where.y;
  config_.app.settingsWindowW = where.width;
  config_.app.settingsWindowH = where.height;
}

void App::DrawSettingsWindowed() {
  const bool wanted = config_.app.settingsSeparateWindow;

  // Nothing to do, and nothing built: the common case, and it costs one branch.
  if (!wanted && !settingsHost_.created()) return;

  if (wanted && !settingsHost_.created()) {
    std::string error;
    // Auspoppen: das Fenster geht dort auf, wo das eingebettete Feld gerade
    // stand. Der Weg fuehrt ueber Bildschirmkoordinaten, weil die beiden in
    // verschiedenen Bezugssystemen leben -- das Feld im Client des
    // Hauptfensters, das Fenster auf dem Desktop.
    //
    // Verglichen werden die *Aussenkanten* beider, nicht ihre Inhalte. Das ist
    // die einzige Zuordnung, die sich nicht um ein paar Pixel verzieht: das
    // eingebettete Feld zaehlt seine eigene Titelleiste zur Flaeche dazu, das
    // freigestellte faengt beim Client unterhalb der Windows-Titelleiste an.
    // Wer Client auf Client abbildet, verschiebt beim Umschalten jedes Mal um
    // die Differenz der beiden Leisten -- einmal nach unten, einmal nach oben.
    SettingsHost::Placement where;
    where.x = config_.app.settingsWindowX;
    where.y = config_.app.settingsWindowY;
    where.width = config_.app.settingsWindowW;
    where.height = config_.app.settingsWindowH;
    if (config_.app.settingsPanelW > 200 && config_.app.settingsPanelH > 200) {
      const Point topLeft =
          window_.ClientToScreen({config_.app.settingsPanelX, config_.app.settingsPanelY});
      where.x = topLeft.x;
      where.y = topLeft.y;
      where.width = config_.app.settingsPanelW;
      where.height = config_.app.settingsPanelH;
    }
    if (!settingsHost_.Create(uiScale_, display_.tearingSupported(), where, &error)) {
      config_.app.settingsSeparateWindow = false;
      Toast(error);
      return;
    }
    settingsHost_.ApplyTheme(darkMode_, config_.app.accentColor);
    ApplyWindowFlags();
    settingsHost_.SetKeyCallback([this](Key key, bool ctrl, bool shift, bool alt, bool busy) {
      return OnKey(key, ctrl, shift, alt, busy);
    });
    // While its window is being dragged, Windows keeps the loop to itself. The
    // timer inside that loop is what still lets the picture run.
    // Dragging a window puts Windows into a modal loop of its own that does not
    // return until the mouse is let go, so the main loop stops running and the
    // preview stops with it. A timer inside that loop is the only way back in.
    // It has to do what one turn of the main loop does -- which since the two
    // were separated means the settings window as well as the preview.
    settingsHost_.SetFrameCallback([this]() {
      if (inModalFrame_) return;
      inModalFrame_ = true;
      Tick();
      // The preview only. The dialog's *content* does not change while its
      // frame is being dragged, and redrawing it here means a second present
      // between every mouse movement and the window catching up with it --
      // which turns a frozen preview into a window that lags the cursor.
      //
      // Genau dieselbe Frage wie in der Hauptschleife, und aus demselben Grund:
      // ist ein neues Bild da, ist ein zweites Halbbild faellig, oder ist es zu
      // lange her? Frueher stand hier stattdessen eine Zeitschranke, und jede
      // Zahl, die dort stand, war neben der Kadenz der Quelle -- mal zu
      // langsam, mal gegen sie schwebend.
      //
      // Das Ereignis der Karte laesst sich mit einer Wartezeit von null
      // abfragen; es setzt sich selbst zurueck, also ist das dieselbe
      // Entnahme, die die Hauptschleife sonst macht. Sie laeuft in diesem
      // Moment nicht, also nimmt ihr das nichts weg.
      bool newPicture = false;
      if (FrameBuffer* sink = capture_.sink()) {
        newPicture = sink->frameReady().Wait(0);
      }
      const int64_t nowQpc = ClockTicks();
      const double sinceRenderMs =
          lastRenderQpc_ == 0 ? 1e9 : TicksToSeconds(nowQpc - lastRenderQpc_) * 1000.0;
      const bool fieldDue = secondFieldPending_ && nowQpc >= secondFieldQpc_;
      if (newPicture || fieldDue || sinceRenderMs >= 200.0) {
        lastRenderQpc_ = nowQpc;
        RenderFrame();
      }
      inModalFrame_ = false;
    });
  }

  if (!wanted) {
    // Switched off again: put the panel back inside the picture, and give the
    // preview its shortest queue back. Where it stood is remembered first --
    // this is the same object that will be built again if it is switched back
    // on, and it should come up where it was left.
    RememberSettingsWindow();
    // Einbetten: das Feld geht dort auf, wo das Fenster gerade stand. Umgekehrt
    // derselbe Weg -- Client des Fensters auf den Bildschirm, von dort in den
    // Client des Hauptfensters.
    //
    // Liegt das Fenster ganz oder ueberwiegend neben dem Hauptfenster, kommt
    // dabei eine Lage heraus, die das Feld unerreichbar machen wuerde. Das faengt
    // die Wiederherstellung selbst ab und setzt in die Mitte.
    Rect outer;
    if (settingsHost_.window().FrameRect(&outer)) {
      const Point inMain = window_.ScreenToClient({outer.left, outer.top});
      config_.app.settingsPanelX = inMain.x;
      config_.app.settingsPanelY = inMain.y;
      config_.app.settingsPanelW = outer.width();
      config_.app.settingsPanelH = outer.height();
    }
    settings_.RestorePosition();
    settingsHost_.Destroy();
    return;
  }

  // Closing the window is closing the settings, the same as the button is.
  if (settingsHost_.takeCloseRequest()) settings_.Close();

  if (settings_.isOpen() != settingsHost_.visible()) {
    if (settings_.isOpen()) {
      settingsHost_.Show(AppNameUtf8() + T(" – Einstellungen", " – Settings"));
    } else {
      settingsHost_.Hide();
    }
  }

  // Die Vorschau behaelt ihre kurze Warteschlange, immer. Frueher musste sie
  // hier auf drei hoch, weil beide Fenster an einem Geraet hingen und sich
  // gegenseitig auf das Present warten liessen; seit der Dialog sein eigenes
  // Geraet hat, geht ihn das nichts mehr an.

  if (!settingsHost_.BeginFrame(darkMode_, config_.app.accentColor)) return;

  settings_.SetFillsWindow(true);
  const SettingsWindow::Result result =
      settings_.Draw(capture_.running() ? &capture_.capabilities() : nullptr,
                     &recording_.ffmpeg());
  settingsHost_.EndFrame();

  // Nothing to put back. This runs after the main window has presented, so the
  // targets it wants are set again by the next frame's first pass.

  // Closing is closing, whether it was the footer button or the window's own.
  if (result == SettingsWindow::Result::Close) settings_.Close();
}

void App::OpenReleasePage(const UpdateStatus& status) { OpenUrl(ReleasePageUrl(status)); }

void App::OpenWebsite() { OpenUrl(WebsiteUrl()); }

void App::UpdateHdr() {
  const VideoFormatInfo& src = renderer_.sourceFormat();

  VideoRenderer::Transfer transfer = VideoRenderer::Transfer::Sdr;
  bool wideGamut = false;
  switch (config_.app.hdrInput) {
    case HdrInput::Pq:
      transfer = VideoRenderer::Transfer::Pq;
      wideGamut = true;
      break;
    case HdrInput::Hlg:
      transfer = VideoRenderer::Transfer::Hlg;
      wideGamut = true;
      break;
    case HdrInput::Sdr:
      break;
    case HdrInput::Auto:
    default:
      if (src.color.transfer == ColorInfo::Transfer::PQ) transfer = VideoRenderer::Transfer::Pq;
      if (src.color.transfer == ColorInfo::Transfer::HLG) transfer = VideoRenderer::Transfer::Hlg;
      // BT.2020 primaries; the matrix says the same thing a second way.
      wideGamut = src.color.primaries == ColorInfo::Primaries::BT2020 ||
                  src.color.matrix == ColorInfo::Matrix::BT2020_10 ||
                  src.color.matrix == ColorInfo::Matrix::BT2020_12;
      break;
  }
  renderer_.SetHdrInput(transfer, wideGamut);

  // The screen can change without anything else doing so -- dragging the window
  // to another monitor, or turning HDR on in Windows while this runs. Asking
  // costs a little, so not every frame.
  if (++hdrDisplayPoll_ >= 120) {
    hdrDisplayPoll_ = 0;
    display_.RefreshDisplayCapability();
  }

  const Display::DisplayCapability display = display_.displayCapability();
  bool want = false;
  switch (config_.app.hdrOutput) {
    case HdrOutput::Always:
      want = display.hdr;
      break;
    case HdrOutput::Auto:
      // Only when there is something to gain. An ordinary picture on an HDR
      // screen goes through one more conversion for no benefit.
      want = display.hdr && transfer != VideoRenderer::Transfer::Sdr;
      break;
    case HdrOutput::Off:
    default:
      break;
  }

  if (want != display_.hdrOutput()) {
    std::string error;
    if (!display_.SetHdrOutput(want, &error)) {
      // Said once and then left alone, rather than every frame from here on.
      if (!error.empty() && want) {
        config_.app.hdrOutput = HdrOutput::Off;
        Toast(error);
      }
    }
  }

  renderer_.SetHdrOutput(display_.hdrOutput(), config_.app.paperWhiteNits,
                         config_.app.sourcePeakNits,
                         display_.hdrOutput() ? display.peakNits : 100.0f);

  settings_.SetHdrState(display.hdr, display_.hdrOutput(), display.peakNits, (int)transfer);
  settings_.SetCarrierPeriod(renderer_.effectiveCarrierPeriod());
}

void App::DrawUpdatePrompt() {
  // The startup check runs on its own thread, so the result turns up a second or
  // two in. Raised once per session and never again, whatever the user does with
  // it -- a notice that keeps coming back is an advertisement.
  if (!updatePromptRaised_ && updater_.status().announce &&
      updater_.status().state == UpdateStatus::State::Available) {
    updatePromptRaised_ = true;
    updatePromptQueued_ = true;
  }

  const char* id = T("Update verfügbar###app_update", "Update available###app_update");
  // Not over the crash notice or the welcome: opened at the same level, it
  // would replace them.
  if (updatePromptQueued_ && !ImGui::IsPopupOpen("###crash_notice") &&
      !ImGui::IsPopupOpen("###welcome")) {
    ImGui::OpenPopup(id);
    updatePromptQueued_ = false;
  }

  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                 viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                          ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

  const UpdateStatus st = updater_.status();
  ImGui::Text(T("%s %s ist verfügbar.", "%s %s is available."), AppNameUtf8().c_str(),
              st.latestVersion.c_str());
  ImGui::TextDisabled(T("Installiert ist %s.", "This build is %s."), Updater::currentVersion());
  ImGui::Spacing();

  switch (st.state) {
    case UpdateStatus::State::Downloading:
      ImGui::TextDisabled("%s", T("wird geladen ...", "downloading ..."));
      break;
    case UpdateStatus::State::Ready:
      ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "%s",
                         T("Eingesetzt. Ein Neustart übernimmt sie.",
                           "Installed. A restart picks it up."));
      break;
    case UpdateStatus::State::Failed:
      ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.35f, 1.0f), "%s", UpdateErrorText(st).c_str());
      break;
    default:
      break;
  }

  ImGui::Spacing();
  const float buttonWidth = 130.0f * uiScale_;

  if (st.state == UpdateStatus::State::Ready) {
    if (ImGui::Button(T("Jetzt neu starten", "Restart now"), ImVec2(buttonWidth, 0))) {
      if (updater_.RestartIntoNewBuild()) {
        running_ = false;
      } else {
        Toast(T("Neustart fehlgeschlagen.", "Restart failed."));
      }
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Später", "Later"), ImVec2(buttonWidth, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }

  ImGui::BeginDisabled(updater_.busy());
  if (ImGui::Button(T("Installieren", "Install"), ImVec2(buttonWidth, 0))) {
    updater_.InstallAsync();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button(T("Später", "Later"), ImVec2(buttonWidth, 0))) ImGui::CloseCurrentPopup();
  ImGui::SameLine();
  if (ImGui::Button(T("Was ist neu", "What is new"), ImVec2(buttonWidth, 0))) {
    // The release page, not the Updates tab. The tab shows the notes trimmed to
    // something that fits; the page has the whole of them, the file, and the
    // history above it.
    OpenReleasePage(st);
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void App::DrawCrashNotice() {
  const char* id = T("Nicht normal beendet###crash_notice", "Did not close normally###crash_notice");
  if (crashNoticeQueued_) {
    ImGui::OpenPopup(id);
    crashNoticeQueued_ = false;
  }

  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                 viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                          ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

  const std::string when = ShortDateAndTime(unfinishedSession_.started);

  ImGui::Text(T("%s wurde beim letzten Mal nicht normal beendet.",
                "%s did not close normally last time."),
              AppNameUtf8().c_str());
  if (!when.empty()) {
    ImGui::TextDisabled(T("Sitzung vom %s, Version %s", "Session from %s, version %s"),
                        when.c_str(), unfinishedSession_.version.c_str());
  }
  ImGui::Spacing();
  ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
  ImGui::TextUnformatted(T("Die Sitzung hat im Protokoll keine Endzeile: meist ein Absturz, "
                           "sonst Task-Manager oder Stromausfall. Was zuletzt passiert ist, "
                           "steht dort über der nachgetragenen Zeile \"ended abnormally\".",
                           "The session has no end line in the log: usually a crash, otherwise "
                           "the task manager or a power cut. What happened last is right above "
                           "the \"ended abnormally\" line added there now."));
  ImGui::PopTextWrapPos();
  ImGui::Spacing();

  const float buttonWidth = 130.0f * uiScale_;
  if (ImGui::Button(T("Protokoll öffnen", "Open log"), ImVec2(buttonWidth, 0))) {
    OpenFile(OwnFile("log"));
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (ImGui::Button("OK", ImVec2(buttonWidth, 0))) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

void App::DrawWelcome() {
  const char* id = T("Erster Start###welcome", "First start###welcome");
  if (welcomeQueued_) {
    ImGui::OpenPopup(id);
    welcomeQueued_ = false;
  }

  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                 viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                          ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

  ImGui::Text(T("Sieht so aus, als wäre das dein erster Start mit %s.",
                "Looks like this is your first time using %s."),
              AppNameUtf8().c_str());
  ImGui::TextUnformatted(T("Verknüpfungen anlegen?", "Create shortcuts?"));
  ImGui::Spacing();
  if (welcomeStartMenu_) {
    ImGui::Checkbox(T("Im Startmenü", "In the Start menu"), &welcomeWantStartMenu_);
  }
  if (welcomeDesktop_) {
    ImGui::Checkbox(T("Auf dem Desktop", "On the desktop"), &welcomeWantDesktop_);
  }
  ImGui::Spacing();
  ImGui::TextDisabled("%s", T("Geht auch später, in den Einstellungen unter Anzeige.",
                              "Also possible later, in the settings under Display."));
  ImGui::Spacing();

  const bool startMenu = welcomeStartMenu_ && welcomeWantStartMenu_;
  const bool desktop = welcomeDesktop_ && welcomeWantDesktop_;
  const float buttonWidth = 130.0f * uiScale_;
  ImGui::BeginDisabled(!startMenu && !desktop);
  if (ImGui::Button(T("Anlegen", "Create"), ImVec2(buttonWidth, 0))) {
    bool ok = true;
    if (startMenu) ok = CreateShortcut(ShortcutPlace::StartMenu) && ok;
    if (desktop) ok = CreateShortcut(ShortcutPlace::Desktop) && ok;
    if (!ok) {
      Toast(T("Die Verknüpfung ließ sich nicht anlegen.", "The shortcut could not be created."));
    }
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button(T("Nein danke", "No thanks"), ImVec2(buttonWidth, 0))) {
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void App::OpenDeviceConfig() {
  if (!capture_.running()) {
    Toast(T("Die Karte läuft nicht.", "The card is not running."));
    return;
  }
  const std::string& title = config_.active().capture.video.name;
  std::string error;
  if (!devicePages_.Open(capture_, title, &error)) {
    Toast(error.empty() ? T("Der Konfigurationsdialog ließ sich nicht öffnen.",
                            "The configuration dialog could not be opened.")
                        : error);
  }
}

// Das Profil zur erkannten Videonorm.
//
// Die Suche beantwortet eine Frage ueber das Signal; welches Profil dazu
// gehoert, ist eine Frage ueber den Nutzer, und die beantwortet er einmal in
// den Einstellungen. Erkannt wird PAL 60, und wer dafuer ein Profil angelegt
// hat -- anderer Zuschnitt, andere Filter, anderer Ton --, bekommt es, statt es
// von Hand zu waehlen.
//
// Verglichen wird ueber die Gruppe, siehe Profile::autoSelectStandard. Und
// ausgeloest wird am *Wechsel* des Ergebnisses, nicht an seinem Bestand:
// `profileMatchedStandard_` haelt fest, worauf zuletzt reagiert wurde. Damit
// wirkt die Regel einmal je erkannter Norm, und wer danach von Hand ein anderes
// Profil waehlt, behaelt es -- bis sich die Quelle wirklich aendert. Ohne das
// waere jeder Handgriff nach einer Sekunde wieder rueckgaengig gemacht, und
// zwar von etwas, das der Nutzer nicht angefasst hat.
//
// Die Schleife ist die eigentliche Gefahr: schaltet Profil A nach B und B nach
// A, wechselt qBlank im Sekundentakt zwischen zwei Graphen. Drei Bedingungen
// schliessen sie aus. Passt die Regel des *aktiven* Profils schon, geschieht
// nichts -- dann ist das Ziel erreicht, ganz gleich, wie viele andere Profile
// dieselbe Norm beanspruchen. Ein Ziel mit fester Norm scheidet aus, weil dort
// nie wieder etwas erkannt wuerde. Und ein Ziel auf einer anderen Karte oder
// einem anderen Eingang scheidet aus, weil die erkannte Norm dann gar nichts
// ueber das aussagt, was nach dem Wechsel anliegt.
void App::UpdateProfileForStandard() {
  const long standard = standardSearch_.colourCheckedStandard();
  if (standard == 0) {
    // Die Suche laeuft wieder. Was als naechstes herauskommt, ist ein neues
    // Ergebnis und darf wieder wirken -- auch wenn dieselbe Norm herauskommt.
    profileMatchedStandard_ = 0;
    return;
  }
  if (standard == profileMatchedStandard_) return;
  // Gemerkt wird auch dann, wenn nichts passt: die Norm ist abgehandelt.
  profileMatchedStandard_ = standard;

  // Mitten in einer Aufnahme nicht. Ein Profilwechsel baut den Graphen neu auf,
  // und was dabei aus der laufenden Datei wird, ist keine Frage, die ungefragt
  // beantwortet werden darf.
  if (recorder_.recording()) return;
  if (cropTool_.active()) return;

  const int group = VideoStandardGroupOf(standard);
  if (group < 0) return;

  const Profile& active = config_.active();
  if (active.autoSelectStandard != 0 &&
      VideoStandardGroupOf(active.autoSelectStandard) == group) {
    return;
  }

  for (int i = 0; i < (int)config_.profiles.size(); ++i) {
    if (i == config_.activeProfile) continue;
    const Profile& p = config_.profiles[i];
    if (p.autoSelectStandard == 0) continue;
    if (VideoStandardGroupOf(p.autoSelectStandard) != group) continue;
    if (p.capture.videoStandard != -1) continue;
    if (!(p.capture.video == active.capture.video)) continue;
    if (p.capture.crossbarInput != active.capture.crossbarInput) continue;

    CAP_LOG("Profile %d (%s) takes over: detected %s", i + 1, p.name.c_str(),
            VideoStandardName(VideoStandardIndexOf(standard)));
    const std::string name = p.name;
    SwitchProfile(i);
    Toast(Format(T("%s erkannt — Profil „%s“", "%s detected — profile \"%s\""),
                 VideoStandardGroupName(group), name.c_str()));
    return;
  }
}

// Ob das Bild aus dem Analogdekoder kommt. Dieselbe Frage, die entscheidet, was
// in den Einstellungen erscheint -- und sie muss dieselbe Antwort geben, sonst
// wirkt etwas, das nirgends mehr zu sehen ist.
//
// Einen Dekoder zu haben heisst nicht, ihn zu benutzen: auf einer Karte, die
// beides kann, wird er weiterhin gemeldet, waehrend das Bild vom digitalen
// Eingang kommt. Keine Videonorm liefert mehr als 576 Zeilen, also beantwortet
// das Bild die Frage selbst.
bool App::SourceIsAnalogue() const {
  const SignalKind kind = config_.active().capture.signalKind;
  if (kind == SignalKind::Analog) return true;
  if (kind == SignalKind::Digital) return false;
  const bool hasDecoder = capture_.running() && capture_.capabilities().availableStandards != 0;
  const VideoFormatInfo fmt = renderer_.sourceFormat();
  return hasDecoder && (!fmt.valid() || fmt.height <= 576);
}

// Welcher Anschluss, wenn die Einstellung auf Automatisch steht.
//
// Die Karte weiss es, wo sie ihre Eingaenge ueberhaupt offenlegt:
// `EnumerateCrossbarInputs` liest den physischen Typ jedes Eingangs mit aus --
// aus dem Crossbar, wo es einen gibt, sonst aus dem privaten Selektor des
// Herstellers, wenn qBlank ihn kennt (die SA7160 hat so einen).
//
// Gefragt wird zuerst nach dem eingestellten Eingang und, wo keiner eingestellt
// ist, nach dem, auf dem die Karte tatsaechlich steht. Das ist nicht dasselbe:
// "Nicht aendern" heisst, dass qBlank den Selektor in Ruhe laesst, nicht dass
// niemand wuesste, wo er steht. Nur wenn beides nichts hergibt, bleibt die
// Annahme.
//
// Die Annahme ist Composite, und zwar in die sichere Richtung. Ein zu viel
// angebotener Filter steht auf null und tut nichts, bis jemand ihn anfasst;
// ein zu wenig angebotener fehlt dort, wo er gebraucht wird, und der Nutzer
// sieht das Kriechen und findet den Regler nicht mehr.
AnalogConnector App::ResolvedConnector() const {
  const AnalogConnector chosen = config_.active().capture.connector;
  if (chosen != AnalogConnector::Auto) return chosen;

  const std::vector<CrossbarInput>& inputs = capture_.capabilities().crossbarInputs;
  int index = config_.active().capture.crossbarInput;
  if (index < 0) index = capture_.capabilities().currentInput;
  if (index >= 0 && index < (int)inputs.size()) {
    switch (inputs[(size_t)index].kind) {
      case ConnectorKind::SVideo:
        return AnalogConnector::SVideo;
      case ConnectorKind::Component:
      case ConnectorKind::Rgb:
        // RGB kommt wie Component ohne Farbtraeger an -- drei Leitungen, jede
        // ihr eigenes Signal. Fuer alles, was hier davon abhaengt, ist das
        // derselbe Fall.
        return AnalogConnector::Component;
      // SCART absichtlich nicht: derselbe Stecker fuehrt je nach Kabel
      // Composite oder RGB, und welches davon steckt, sagt der Typ nicht. Also
      // die sichere Annahme, und wer es besser weiss, stellt es ein.
      default:
        break;
    }
  }
  return AnalogConnector::Composite;
}

bool App::ConnectorMixesLumaAndChroma() const {
  return SourceIsAnalogue() && ResolvedConnector() == AnalogConnector::Composite;
}

bool App::ConnectorHasColourCarrier() const {
  return SourceIsAnalogue() && ResolvedConnector() != AnalogConnector::Component;
}

// Die Einstellungen, wie sie fuer *diese* Quelle gelten.
//
// Ausgeblendet muss auch abgeschaltet heissen. Wer am SNES die Kriechfilter und
// das native Raster anhatte und dann eine HD-Konsole ansteckt, saehe sonst ein
// Bild, das er nicht will, und faende den Regler dafuer nirgends mehr -- die
// Einstellung ist ja gerade verschwunden, weil sie nicht passt.
//
// Neutralisiert wird nur die Kopie, die gezeichnet wird. Das Profil behaelt
// seine Werte, denn es beschreibt eine Konsole, und die kommt wieder. Was es
// nicht tut, ist sich zu merken und wiederherzustellen: die naechste analoge
// Quelle ist vielleicht eine andere Konsole mit anderen Werten. Genau dafuer
// gibt es Profile.
ImageSettings App::EffectiveImage(const Profile& profile) const {
  ImageSettings img = profile.image;
  const VideoFormatInfo fmt = renderer_.sourceFormat();
  img.compare = compare_;

  if (!SourceIsAnalogue()) {
    // Composite bringt diese Stoerungen mit, ein digitaler Eingang nicht. Der
    // Demodulator wuerde einen Traeger herausrechnen, den es nicht gibt.
    img.chromaSoft = 0;
    img.temporalDenoise = 0.0f;
    img.dotNotch = 0.0f;
    // Und die Bandanhebung ebenso: ihre Fensterbreite kommt aus der
    // Traegerfrequenz, und ohne Traeger hebt sie ein Band an, das niemand
    // gedaempft hat. Stand vorher nicht hier -- ausgeblendet war sie schon,
    // abgeschaltet nicht, und das ist genau der Fall, den der Kommentar
    // ueber dieser Funktion verbietet.
    img.bandwidthRestore = 0.0f;
    // Das native Raster rechnet das Abtasten einer analogen Zeile zurueck.
    img.nativeWidth = 0;
    // Der Vergleich bleibt: er zeigt jetzt auch Schaerfen, Bildregler und
    // Bildroehre, und die gibt es an jedem Eingang.
  } else if (!ConnectorMixesLumaAndChroma()) {
    // Analog, aber Helligkeit und Farbe kommen getrennt an -- S-Video auf zwei
    // Leitungen, Component auf drei. Damit faellt alles weg, was Uebersprechen
    // zwischen den beiden behandelt, und das ist kein Feintuning: diese Filter
    // suchen ein Muster auf der Traegerfrequenz und finden dort bei einer
    // sauberen Quelle Bilddetail. Sie wuerden es wegrechnen.
    //
    //   chromaSoft       gegen Regenbogen, also gegen Helligkeitsdetail, das
    //                    der Dekoder als Farbe gelesen hat. Ohne gemeinsame
    //                    Leitung liest er nichts falsch. Bei Component ist die
    //                    Farbbandbreite ausserdem breit genug, dass seitliches
    //                    Weichzeichnen echtes Detail kostet.
    //   adaptiveChroma   ist die Bedingung auf chromaSoft und faellt mit ihm.
    //   dotNotch         rechnet den Farbtraeger aus der Helligkeit heraus. Da
    //                    ist keiner drin.
    //   bandwidthRestore hebt die Daempfung zum Traeger hin wieder an. Ohne
    //                    Traeger gibt es diese Daempfung nicht.
    //
    // Was bleibt, bleibt mit Absicht: temporalDenoise mittelt Rauschen weg --
    // die Traegerausloeschung ist nur die zweite Haelfte seiner Arbeit --, und
    // motionCompensate ist ohnehin um den Traeger herum gesperrt und entfernt
    // Rauschen, "the same defect in every colour system", wie es im Shader
    // steht. Rauschen bringt jede analoge Leitung mit.
    img.chromaSoft = 0;
    img.adaptiveChroma = false;
    img.dotNotch = 0.0f;
    img.bandwidthRestore = 0.0f;
  }

  // Verdoppeln nur, wo wirklich die halbe Bildhoehe ankommt.
  //
  // Das Kaestchen wird sonst gar nicht erst gezeigt, aber ein gespeichertes
  // Haekchen aus einer Sitzung mit 240p-Quelle wuerde hier weiterwirken -- und
  // zwar unsichtbar: das Fenster passt das Bild auf das eingestellte
  // Seitenverhaeltnis, die Aufnahme dagegen kaeme doppelt so hoch heraus.
  if (!SourceIsAnalogue() || !fmt.valid() || fmt.height > kHalfHeightLines) {
    img.lineDouble = false;
  }

  // Das native Raster faellt ueber dem Standardraster weg. Es rechnet in Proben
  // einer analogen Zeile, und oberhalb von 576 Zeilen hat etwas dazwischen
  // hochgerechnet -- ein Dongle, ein Skalierer, die Karte selbst. Die Kanten
  // liegen dann nicht mehr dort, wo diese Zahl sie sucht, und das Zuordnen nimmt
  // Detail weg statt welches zurueckzugeben. Der Regler ist dabei schon
  // ausgeblendet; ein gespeicherter Wert wirkte ohne diese Zeile weiter.
  //
  // Die Zeilenzahl der Quelle hilft hier ausdruecklich *nicht* nach. Sie sagt
  // etwas ueber senkrecht, und dieses Raster liegt waagerecht: dass jemand weiss,
  // wie viele Zeilen seine Konsole zeichnet, macht die 720 Proben je Zeile nicht
  // wieder zu denen, die die Karte einmal genommen hat.
  if (fmt.valid() && fmt.height > kStandardLines) {
    img.nativeWidth = 0;
  }

  // Bildroehreneffekte werden hier nicht mehr abgeschaltet, und das ist der
  // Unterschied zu vorher. Sie versteckten sich frueher ueber 576 Zeilen, also
  // mussten sie hier mit weg; jetzt bleiben sie stehen und sagen selbst, wenn
  // kein Platz ist. Die Maske braucht ohnehin nur das Fenster, und die
  // Zeilenluecken lehnen im Shader von allein ab, wo sie nur aliasen wuerden.

  // Halbbilder: hat die Quelle keine, darf ein von Hand gewaehlter Deinterlacer
  // nicht trotzdem laufen. Nicht abschalten, sondern auf "nur bei interlaced"
  // stellen -- das ist dieselbe Aussage und ueberlebt den Wechsel zurueck.
  const bool hasFields =
      fmt.interlaced || renderer_.detectedInterlace() == VideoRenderer::InterlaceVerdict::Interlaced;
  if (!SourceIsAnalogue() && !hasFields) img.deinterlaceAuto = true;

  // Zuletzt, damit nichts darueber es wieder einschaltet. Siehe ToggleBypass.
  if (bypass_) {
    img.chromaSoft = 0;
    img.adaptiveChroma = false;
    img.temporalDenoise = 0.0f;
    img.dotNotch = 0.0f;
    img.motionCompensate = false;
    img.bandwidthRestore = 0.0f;
    img.compare = false;
    img.sharpen = 0.0f;
    img.brightness = 0.0f;
    img.contrast = 1.0f;
    img.saturation = 1.0f;
    img.hue = 0.0f;
    img.nativeWidth = 0;
    img.scanlines = 0.0f;
    img.mask = 0;
  }

  return img;
}

// Ob das gemessene "interlaced" bei dieser Quelle nach einem Irrtum aussieht.
//
// Kein Veto, sondern eine Nachfrage, und das mit Absicht: die Messung kann hier
// nicht widerlegt werden. Ein bildschirmfuellendes Foto von Kammlinien ist
// raeumlich von Kammlinien nicht zu unterscheiden, und genau das steht auf
// einem erfassten Desktop schnell einmal im Browser. Also entscheidet weiter
// die Messung, und danebengestellt wird der Satz, den ein Mensch braucht, um
// den Fehler in zwei Sekunden selbst zu beheben.
//
// Vier Bedingungen, jede mit einem eigenen Grund:
// * Die Karte hat *nicht* selbst interlaced gemeldet. Sagt sie es, stimmt es,
//   und dann gibt es nichts zu bezweifeln.
// * Die Quelle ist digital. An einem Analogeingang ist interlaced der
//   Normalfall und die Messung ohnehin die einzige Auskunft.
// * Mindestens 720 Zeilen. Bei 720 ist die Sache klar -- ein 720i hat es nie
//   gegeben. Bei 1080 ist sie es nicht, 1080i gab es im Fernsehen wirklich,
//   und deshalb steht hier eine Frage und keine Behauptung. Darunter, etwa bei
//   480i von einem DVD-Spieler ueber HDMI, ist interlaced schlicht richtig.
//
// Die Haelfte dieser Faelle beantwortet inzwischen die Bildrate von selbst:
// kommen bei 720 Zeilen oder mehr fuenfzig oder sechzig Bilder in der Sekunde
// an, laesst die Erkennung "interlaced" gar nicht mehr zu (siehe
// VideoRenderer::SetFrameRateHint). Uebrig bleibt hier also die Quelle, die mit
// 25 oder 30 Bildern ankommt und kaemmt -- und die kann eben beides sein, ein
// echtes 1080i und ein erfasster Bildschirm, auf dem ein Video davon laeuft.
// * Die automatische Erkennung ist eingeschaltet. Ist sie es nicht, laeuft der
//   Deinterlacer sowieso und die Messung aendert am Bild nichts -- vor etwas zu
//   warnen, das gar nicht wirkt, zeigt in die falsche Richtung.
bool App::InterlaceVerdictDoubtful(const Profile& profile) const {
  if (!profile.image.deinterlaceAuto) return false;
  if (renderer_.detectedInterlace() != VideoRenderer::InterlaceVerdict::Interlaced) return false;
  const VideoFormatInfo fmt = renderer_.sourceFormat();
  if (!fmt.valid() || fmt.interlaced) return false;
  if (SourceIsAnalogue()) return false;
  return fmt.height >= 720;
}

// Ein analoger Eingang, an dem der volle Wertebereich ankommt. Ein Dekoder fuer
// Composite, S-Video oder Tuner liefert BT.601, und das heisst 16-235: Schwarz
// liegt auf 16, weil die Norm es dort hinlegt, nicht weil eine Quelle sich so
// entschieden hat. Liegt es stattdessen unten am Anschlag, hat irgendwer davor
// gestreckt -- ueblicherweise die Karte, auf deren Wertebereichsschalter. An
// einem digitalen Eingang sagt dasselbe Bild nichts: ein PC-Desktop hat
// reichlich echte Nullen und echte 255er, und die sind keine Fehlfunktion.
//
// Mehr steht hier nicht drin, und das ist Absicht. Ob beim Strecken etwas
// hinausgefallen ist, laesst sich von hier aus *nicht* feststellen: die
// Verstaerkung sitzt vor dem Wandler, also fehlt der Kamm, an dem man eine
// digitale Streckung erkennen wuerde, und die Stapel an den Enden hat der
// Dekoder ohnehin -- BT.601 verbietet 0 und 255, alles Dunklere kommt auf 1 an
// und alles Hellere auf 254. Ein Versuch, daraus ein Urteil zu bauen, hat
// zuverlaessig auch dann angeschlagen, wenn nichts anlag.
//
// Deshalb ist das ein Hinweis im Reiter Bild und keine Warnung: er sagt, was
// gemessen wurde und wo man nachsieht, und behauptet keinen Schaden.
bool App::AnalogueRangeIsFull() const {
  if (captureState_ != CaptureState::Running) return false;
  if (!SourceIsAnalogue()) return false;
  return renderer_.detectedRange() == VideoRenderer::RangeVerdict::Full;
}

// Eine Aufloesung, die nicht zum Raster der anliegenden Norm passt: 720x480
// an einer PAL-Quelle, 720x576 an NTSC, oder 1080p an einem analogen Eingang.
// Die Karte skaliert dann die Zeilen, und das Bild sieht weich aus oder hat
// Balken, ohne dass irgendwo steht, warum. Beim Wechsel der Norm laesst
// ReleaseStandardBoundFormat die alte Groesse von selbst los; das hier faengt
// den Rest ein -- eine von Hand gewaehlte oder aus einem alten Profil
// mitgebrachte.
//
// Voll (576, 480 oder 486) und halb (288, 240) passen beide. Gemeldet wird nur,
// wenn die Norm feststeht -- eingerastet und keine Suche unterwegs -- und die
// Karte fuer das Pixelformat eine passende Groesse wirklich anbietet; sonst
// gaebe es nichts, worauf der Hinweis zeigen koennte.
int App::ResolutionMismatchLines() const {
  if (captureState_ != CaptureState::Running || !SourceIsAnalogue()) return 0;
  if (standardSearch_.signalLocked() != 1) return 0;
  const SettingsWindow::StandardSearch search = standardSearch_.StandardSearchDisplay();
  if (search != SettingsWindow::StandardSearch::Off &&
      search != SettingsWindow::StandardSearch::Result) {
    return 0;
  }
  const int lines = VideoStandardLines(standardSearch_.signalStandard());
  if (lines <= 0) return 0;
  const int active = lines >= 600 ? 576 : 480;
  const VideoFormatInfo fmt = renderer_.sourceFormat();
  if (!fmt.valid()) return 0;
  if (std::abs(fmt.height - active) <= 16 || std::abs(fmt.height - active / 2) <= 8) return 0;
  const ResolutionOption fit =
      capture_.capabilities().caps.FittingResolution(capture_.connectedFormat().subtype, active);
  if (fit.width <= 0) return 0;
  return active;
}

bool App::SourceLooksInterlaced(const Profile& profile) const {
  if (!profile.image.deinterlaceAuto) return true;
  // The media type is believed when it claims interlaced -- a card that bothers
  // to say so is right. It is not believed when it stays quiet, which is the
  // usual case on an analogue input and is why the picture is measured as well.
  if (renderer_.sourceFormat().interlaced) return true;
  return renderer_.detectedInterlace() == VideoRenderer::InterlaceVerdict::Interlaced;
}

// Die Trennlinie des Vergleichs mit der Maus verschieben, so wie die Raender
// beim Zuschnitt. Gezeichnet wird sie weiterhin im Shader; hier wird nur
// ausgerechnet, wo sie auf dem Schirm liegt, und die Maus zurueck in einen
// Anteil verwandelt.
void App::DragCompareDivider() {
  if (!compare_ || !ImGui::IsMousePosValid()) {
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) compareDrag_ = false;
    return;
  }
  const ImageSettings img = EffectiveImage(config_.active());
  const Rect& r = renderer_.videoRect();
  const float rw = (float)(r.right - r.left);
  const float rh = (float)(r.bottom - r.top);
  if (!img.compare || !renderer_.hasFrame() || rw < 8.0f || rh < 8.0f) {
    compareDrag_ = false;
    return;
  }

  // Der Schnitt liegt im Raster der Quelle, gedreht wird erst danach. Eine
  // Vierteldrehung legt die Linie also quer, und bei zwei der vier Drehungen
  // zaehlt ihr Anteil vom anderen Rand her -- dieselbe Zuordnung wie im
  // zweiten Durchgang.
  const int rot = (int)img.rotation & 3;
  const bool upright = img.compareHorizontal == ((rot & 1) != 0);
  const bool reversed = img.compareHorizontal ? (rot == 1 || rot == 2) : rot >= 2;
  const float lo = upright ? (float)r.left : (float)r.top;
  const float len = upright ? rw : rh;
  const float split = Clamp(img.compareSplit, 0.0f, 1.0f);
  const float at = lo + (reversed ? 1.0f - split : split) * len;

  const ImVec2 mouse = ImGui::GetMousePos();
  const float across = upright ? mouse.x : mouse.y;
  const float along = upright ? mouse.y : mouse.x;
  const bool alongPicture = upright ? along >= (float)r.top && along <= (float)r.bottom
                                    : along >= (float)r.left && along <= (float)r.right;
  const bool hot = compareDrag_ || (!ImGui::GetIO().WantCaptureMouse && alongPicture &&
                                    std::abs(across - at) <= 8.0f * uiScale_);
  if (!hot) return;

  ImGui::SetMouseCursor(upright ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
  if (!compareDrag_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) compareDrag_ = true;
  if (compareDrag_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) compareDrag_ = false;
  if (!compareDrag_) return;

  float share = Clamp((across - lo) / len, 0.0f, 1.0f);
  if (reversed) share = 1.0f - share;
  config_.active().image.compareSplit = share;
}

// Doppelklick aufs Bild schaltet das Vollbild um, wie in jedem Videoplayer.
//
// Beide Klicks muessen auf dem blossen Bild landen. Der erste koennte sonst
// ein Menue geschlossen oder die Trennlinie gegriffen haben, und der zweite
// machte daraus einen Wechsel, den niemand wollte. Laeuft nach
// DragCompareDivider, damit ein Griff an die Linie schon zaehlt.
void App::DoubleClickFullscreen() {
  if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
  const bool onPicture = !ImGui::GetIO().WantCaptureMouse && !compareDrag_;
  if (onPicture && clickOnPicture_ && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    ToggleFullscreen();
    // Ein dritter Klick zaehlt als neuer erster.
    clickOnPicture_ = false;
    return;
  }
  clickOnPicture_ = onPicture;
}

// Ohne Titelleiste greift man das Fenster am Bild. Erst ein Ziehen ueber
// ImGuis Schwelle macht daraus ein Verschieben, ein blosser Klick bleibt ein
// Klick und zaehlt weiter fuer den Doppelklick. Der Druck muss wie dort auf dem
// blossen Bild begonnen haben, nicht an der Trennlinie oder in einem Fenster.
// Der zweite Klick eines Doppelklicks zaehlt nicht: verlaesst er das Vollbild,
// springt das Fenster unter dem Zeiger weg, und der Sprung saehe aus wie Ziehen.
void App::DragBorderlessWindow() {
  if (!config_.app.borderless || fullscreen_ || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    moveDragArmed_ = false;
    return;
  }
  if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    moveDragArmed_ = !ImGui::GetIO().WantCaptureMouse && !compareDrag_ &&
                     !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
  }
  if (!moveDragArmed_ || !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) return;
  moveDragArmed_ = false;
  clickOnPicture_ = false;
  const ImVec2 at = ImGui::GetIO().MouseClickedPos[ImGuiMouseButton_Left];
  window_.BeginMoveDrag(window_.ClientToScreen({(int)at.x, (int)at.y}));
}

void App::FeedFrameConsumers() {
  const bool wantRecorder = recorder_.recording();
  // Only while something is actually watching. An idle camera costs a flag.
  const bool wantCamera = virtualCamera_.running() && virtualCamera_.consumed();

  renderer_.SetHdrWideWanted(wantRecorder && config_.app.recordHdr,
                             wantCamera && virtualCamera_.wantsWide());
  renderer_.SetReadbackEnabled(wantRecorder || wantCamera);
  if (!wantRecorder && !wantCamera) return;

  // Either of the two can be on the wide path while the other is not, so both
  // rings can be live at once. They are read in one place so a frame is never
  // fetched twice and never half handed out.
  const bool wide = renderer_.hdrWideActive();
  const bool recordWide = wantRecorder && wide && config_.app.recordHdr;
  const bool cameraWide = wantCamera && wide && virtualCamera_.wantsWide();

  if (recordWide || cameraWide) {
    VideoRenderer::ReadbackFrame w;
    if (renderer_.FetchHdrReadback(&w)) {
      if (recordWide) recorder_.PushVideo(w.data, w.stride, w.width, w.height);
      if (cameraWide) virtualCamera_.PushFrameWide(w.data, w.stride, w.width, w.height);
      renderer_.ReleaseHdrReadback();
    }
  }

  if (!(wantCamera && !cameraWide) && !(wantRecorder && !recordWide)) return;

  VideoRenderer::ReadbackFrame frame;
  if (!renderer_.FetchReadback(&frame)) return;
  if (wantRecorder && !recordWide) {
    recorder_.PushVideo(frame.data, frame.stride, frame.width, frame.height);
  }
  if (wantCamera && !cameraWide) {
    virtualCamera_.PushFrame(frame.data, frame.stride, frame.width, frame.height);
  }
  renderer_.ReleaseReadback();
}

void App::UpdateVirtualCamera() {
  const int request = settings_.takeVirtualCameraRequest();
  if (request != 0) {
    // Installing while it runs would pull the source out from under a reader,
    // so it goes off first either way.
    const bool wasOn = config_.app.virtualCamera;
    if (virtualCamera_.running()) virtualCamera_.Stop();

    std::string error;
    const bool ok = request == 1 ? CameraSink::InstallSystemWide(&error)
                                 : CameraSink::RemoveSystemWide(&error);
    if (ok) {
      Toast(request == 1 ? T("Kamera installiert.", "Camera installed.")
                         : T("Kamera deinstalliert.", "Camera uninstalled."));
      if (request == 2) config_.app.virtualCamera = false;
    } else {
      Toast(error);
      config_.app.virtualCamera = wasOn;
    }
  }

  virtualCamera_.SetWideOffered(config_.app.cameraHdr);

  const bool want = config_.app.virtualCamera;
  if (want && !virtualCamera_.running() && !virtualCamera_.starting()) {
    virtualCamera_.StartAsync();
  } else if (!want && (virtualCamera_.running() || virtualCamera_.starting())) {
    virtualCamera_.Stop();
  }

  std::string startError;
  if (virtualCamera_.takeError(&startError)) {
    // Turning the switch back off rather than leaving it on and doing nothing,
    // so the tab does not claim a camera that is not there.
    config_.app.virtualCamera = false;
    Toast(startError);
  }

  // What the camera would hand out right now, written whether or not anybody is
  // listening. A consumer reads all of this the moment it connects -- before it
  // has asked for a frame -- so it has to be there beforehand rather than be
  // discovered from the first picture.
  const VideoFormatInfo& format = renderer_.sourceFormat();
  const bool wide = config_.app.cameraHdr &&
                    renderer_.hdrTransfer() != VideoRenderer::Transfer::Sdr;
  virtualCamera_.SetSourceShape(renderer_.outputWidth(), renderer_.outputHeight(), format.fps,
                                wide);

  virtualCamera_.consumers(&virtualCameraConsumers_);
  settings_.SetVirtualCameraState(virtualCamera_.running(), virtualCameraConsumers_);
}

void App::ShowVolumeOsd() {
  // The readout replaces a toast here: a number plus a bar says more than a
  // line of text, and it is what you want to see while a game is running.
  if (config_.app.showVolumeOsd) {
    volumeOsdStart_ = ImGui::GetTime();
  } else {
    const AudioSettings& a = config_.active().audio;
    Toast(a.mute ? T("Stumm", "Muted")
                 : Format(T("Lautstärke %.0f %%", "Volume %.0f %%"), a.volume * 100.0f));
  }
}

// ------------------------------------------------------------------ main loop

int App::Run() {
  while (running_) {
    if (!PumpEvents()) running_ = false;
    if (!running_) break;

    Tick();

    if (minimized_) {
      // Nothing to draw; block on messages so we use no CPU at all.
      WaitForEvents();
      continue;
    }

    // The preview is drawn when there is a new picture to draw, and at no
    // other time.
    //
    // This is the second attempt at pacing it and the first was wrong in a way
    // worth recording. Waking on any message and redrawing was clearly wrong --
    // measured, the whole pipeline ran 235 times a second for a source
    // delivering 25. But replacing it with a clock was no better: at a fixed
    // 33 ms against a 25 fps source the two beat against each other, which is
    // judder of exactly the kind the change was meant to remove. A source has a
    // cadence; anything that is not that cadence is wrong.
    //
    // So: the frame event, a second field falling due, and a slow floor that
    // only matters when no pictures are arriving at all -- with a source
    // running, everything on screen animates at the source's rate anyway.
    const int64_t nowQpc = ClockTicks();
    const double sinceRenderMs =
        lastRenderQpc_ == 0 ? 1e9 : TicksToSeconds(nowQpc - lastRenderQpc_) * 1000.0;
    const bool wokeOnPicture = lastWake_ == WaitResult::Signal;
    // Due by the clock, not by *how* the wait ended.
    //
    // This used to read `lastWait_ == WAIT_TIMEOUT`, which is only ever true
    // when nothing else woke the loop at all. Open the settings and there is a
    // steady stream of messages, so the wait returns "input available" every
    // time and the timeout branch never runs -- on an interlaced source that
    // silently dropped every second field for as long as the dialog was open,
    // because the next arriving picture takes the other branch and resets the
    // field index before the second one was ever drawn.
    //
    // The same shape of mistake as the WM_TIMER that could not compete with the
    // drag loop's message flood: a schedule must not be conditional on the
    // queue being quiet.
    const bool fieldDue = secondFieldPending_ && nowQpc >= secondFieldQpc_;
    const double idleFloor = IdleFloorMs();
    if (wokeOnPicture || fieldDue || sinceRenderMs >= idleFloor) {
      lastRenderQpc_ = nowQpc;
      RenderFrame();
    }

    // And the settings window is drawn on its own account, every time round,
    // with its own throttle inside. It wants to follow the mouse; the preview
    // wants to follow the capture card. Tying them together made one of them
    // wrong whichever rate was chosen.
    DrawSettingsWindowed();
    // The icon's menu likewise, while it is open.
    DrawTrayMenu();

    // Wait for the next captured frame, a pending second field, or input.
    //
    // This bounds how long the loop *sleeps*, not how often it draws. It used
    // to be both, back when any wake-up redrew the preview -- 16 rather than 8
    // was the ceiling that stopped the settings dialog from driving the whole
    // video pipeline at a hundred and twenty-five times a second for the sake
    // of feeling responsive. Since the redraw became conditional on a picture
    // having arrived, the ceiling is gone: a source delivering two hundred and
    // forty frames signals the event two hundred and forty times and gets two
    // hundred and forty redraws, and this timeout never comes into it.
    // Short while the dialog is open, because that is what keeps *it* smooth;
    // it no longer costs the preview anything, since a wake-up without a
    // picture no longer redraws the preview.
    // Ein Boden nuetzt nichts, wenn die Schleife laenger schlaeft als er lang
    // ist. Solange die Einstellungen offen sind kurz, weil das freigestellte
    // Fenster jede Runde gezeichnet wird; sonst so kurz, wie der Boden es
    // verlangt, und hoechstens 100 ms.
    int timeout =
        settings_.isOpen() || trayPopup_.isOpen() ? 16 : Clamp((int)idleFloor, 16, 100);
    if (secondFieldPending_) {
      // Rounded up, not truncated. Truncating asks to be woken a fraction of a
      // millisecond before the field is due, at which point the loop finds it is
      // not due yet, redraws the same field for nothing and then spins on a zero
      // timeout until it is. Waiting the extra millisecond costs a millisecond
      // and saves all of that.
      const double waitMs = TicksToSeconds(secondFieldQpc_ - ClockTicks()) * 1000.0;
      timeout = Clamp((int)std::ceil(waitMs), 0, timeout);
    }
    FrameBuffer* sink = capture_.sink();
    lastWake_ = WaitForEventsOr(sink ? &sink->frameReady() : nullptr, timeout);
  }
  return 0;
}

void App::Tick() {
  UpdatePowerRequest();
  recording_.CollectEncoderProbe();
  // Erst entscheiden, ob der Decoder ueberhaupt befragt wird, dann das
  // Ergebnis benutzen. Beim Start der Aufnahme steht das Format noch nicht
  // fest, der Wachthread laeuft also zunaechst an und wird hier ein Bild
  // spaeter wieder angehalten, sobald sich die Quelle als digital erweist.
  standardSearch_.UpdateSignalWatch();
  standardSearch_.UpdateVideoStandard();
  UpdateProfileForStandard();
  cropTool_.UpdateCropForFormat();

  if (captureState_ == CaptureState::Running) {
    std::string message;
    if (capture_.PumpEvents(&message)) {
      captureError_ = message;
      captureState_ = CaptureState::Reconnecting;
      capture_.Stop();
      audio_.Stop();
      nextRetryQpc_ = ClockTicks() + SecondsToTicks(kRetrySeconds);
      retryCount_ = 0;
    }
  }

  // While the settings dialog is open the user is presumably fixing exactly
  // this, so retrying behind their back only produces noise and blocks the
  // device they are about to pick.
  if (captureState_ == CaptureState::Reconnecting && !settings_.isOpen() &&
      ClockTicks() >= nextRetryQpc_) {
    ++retryCount_;
    std::string error;
    if (StartCapture(&error)) {
      CAP_LOG("Reconnected after %d attempts", retryCount_);
      Toast(T("Wieder verbunden", "Reconnected"));
    } else {
      captureError_ = error;
      // Back off once it is clear this is not a brief hiccup.
      const double wait = retryCount_ >= kRetryBackoffAfter ? kRetrySlowSeconds : kRetrySeconds;
      nextRetryQpc_ = ClockTicks() + SecondsToTicks(wait);
    }
  }

  if (audio_.failed()) {
    CAP_WARN("Audio failed, restarting");
    StartAudio();
  }

  // Der rote Punkt in der Taskleiste folgt der Aufnahme, auch wenn sie von
  // selbst endet.
  if (recorder_.recording() != recordingBadge_) {
    recordingBadge_ = !recordingBadge_;
    window_.SetTaskbarBadge(recordingBadge_, T("Aufnahme läuft", "Recording"));
  }
  UpdateTray();

  // Hide the pointer once it has been still for a while in fullscreen.
  if (fullscreen_ && config_.app.hideCursorFullscreen && !settings_.isOpen()) {
    if (!window_.cursorHidden() &&
        TicksToSeconds(ClockTicks() - lastMouseMoveQpc_) > kCursorIdleSeconds) {
      window_.SetCursorHidden(true);
    }
  } else if (window_.cursorHidden()) {
    window_.SetCursorHidden(false);
  }
}

void App::RenderFrame() {
  const int64_t now = ClockTicks();
  const Profile& profile = config_.active();

  // ---- pull the newest frame ----
  FrameBuffer* sink = capture_.sink();
  bool haveNewFrame = false;
  if (sink) {
    FrameView view;
    if (sink->AcquireFrame(&view) && view.valid()) {
      if (!sawFirstFrame_) {
        sawFirstFrame_ = true;
        CAP_LOG("First frame after %.0f ms (%zu bytes)",
                TicksToSeconds(now - captureStartQpc_) * 1000.0, view.size);
      }
      renderer_.SetSourceFormat(sink->format(), nullptr);
      // Das Standbild haelt hier an und nirgends sonst: die Bilder werden
      // weiter abgeholt, damit der Zulauf nicht auflaeuft und die Statistik
      // stimmt, nur in die Textur geht keines mehr. Alles dahinter -- Filter,
      // Skalierung, Regler -- laeuft am stehenden Bild weiter.
      if (frozen_) {
        // nichts hochladen
      } else if (delayLine_.active()) {
        // Mit der Ankunftszeit und nicht mit `now`: die Verzoegerungsleitung
        // soll das Bild um die eingestellte Zeit nach seinem Eintreffen
        // herausgeben, nicht nach dem Zeichendurchgang, der es aufgegriffen hat.
        delayLine_.Push(view, sink->lastArrivalTicks());
      } else {
        renderer_.UploadFrame(view);
        haveNewFrame = true;
        displayedArrivalQpc_ = sink->lastArrivalTicks();
      }
    }
    if (!frozen_ && delayLine_.active()) {
      FrameView delayed;
      if (delayLine_.Pop(&delayed, now)) {
        renderer_.UploadFrame(delayed);
        haveNewFrame = true;
        displayedArrivalQpc_ = delayLine_.lastPoppedQpc();
      }
    }
  }

  // ---- bob deinterlacing: second field of the previous frame ----
  const VideoFormatInfo format = renderer_.sourceFormat();
  const bool deinterlacing =
      profile.image.deinterlace != Deinterlace::Off && SourceLooksInterlaced(profile);

  // Which field is the earlier one. The media type is asked first and is usually
  // silent on an analogue card, in which case top-field-first is the convention
  // for standard definition -- but a wrong guess here does not soften the
  // picture, it makes it jump: the two fields are shown in the wrong order, so
  // every frame steps back half a frame and then forward again. Hence the
  // override in the settings, which is the only reliable fix when the card says
  // nothing.
  int firstField = format.fieldOneFirst ? 0 : 1;
  if (profile.image.fieldOrder == FieldOrder::TopFirst) firstField = 0;
  else if (profile.image.fieldOrder == FieldOrder::BottomFirst) firstField = 1;

  if (haveNewFrame) {
    fieldIndex_ = firstField;
    if (deinterlacing) ++fieldsShown_[0];
    if (deinterlacing) {
      // The announced rate and the delivered rate are not always the same
      // number. This card announces 50 fps and hands over 25 woven frames a
      // second -- half the rate, twice the content per frame. Splitting the
      // announced interval would put the second field on screen 10 ms in and
      // then leave it there for 30, which judders harder than not deinterlacing
      // at all, so the measured arrival rate wins whenever there is one.
      double frameSeconds = format.fps > 1.0 ? 1.0 / format.fps : 1.0 / 60.0;
      const double measured = sink ? sink->stats().sourceFps : 0.0;
      if (measured > 1.0) frameSeconds = 1.0 / measured;

      // Counted from when the frame arrived, not from when we got round to
      // drawing it. Those are not the same instant: the log has shown the
      // displayed frame to be twenty milliseconds old, and half a frame after
      // *that* falls past the arrival of the next one -- at which point the
      // second field is never shown at all. Half the frames then get both
      // fields and half get one, which is exactly what a juddering picture is.
      const int64_t arrival = sink && sink->lastArrivalTicks() != 0 ? sink->lastArrivalTicks() : now;
      const int64_t period = SecondsToTicks(frameSeconds);

      // The card delivers when the driver gets round to it -- the graph runs
      // without a reference clock on purpose -- and measured here the arrivals
      // wander by ten milliseconds either way. Half a frame after each raw
      // arrival therefore lands anywhere, and bob ends up showing one field for
      // five milliseconds and the next for twenty-eight. The average is exactly
      // right and the picture stutters anyway.
      //
      // So the next arrival is predicted from a phase that is nudged towards the
      // arrivals rather than following each one, and the second field is put
      // halfway between the frame we have and the frame we expect. A late frame
      // then shortens both of its fields equally instead of crushing one of them.
      if (framePhaseQpc_ == 0 || std::llabs(arrival - framePhaseQpc_) > period) {
        framePhaseQpc_ = arrival;  // first frame, or the stream jumped
      } else {
        framePhaseQpc_ += (arrival - framePhaseQpc_) / 8;
      }
      const int64_t expectedNext = framePhaseQpc_ + period;
      framePhaseQpc_ = expectedNext;

      int64_t gap = (expectedNext - arrival) / 2;
      const int64_t minGap = period / 4;
      const int64_t maxGap = period * 3 / 4;
      if (gap < minGap) gap = minGap;
      if (gap > maxGap) gap = maxGap;
      secondFieldQpc_ = arrival + gap;
      secondFieldPending_ = true;
    } else {
      secondFieldPending_ = false;
    }
  } else if (secondFieldPending_ && now >= secondFieldQpc_) {
    fieldIndex_ = 1 - firstField;
    secondFieldPending_ = false;
    ++fieldsShown_[1];
  }

  // ---- draw ----
  float clear[4];
  GetBackgroundColor(darkMode_, config_.app.accentColor, clear);
  if (!display_.BeginFrame(clear)) return;

  // The window is on a monitor with other scaling now. Sizes, spacing and font
  // follow it from this frame on.
  if (pendingUiScale_ > 0.0f) {
    if (pendingUiScale_ != uiScale_) {
      uiScale_ = pendingUiScale_;
      ApplyTheme();
    }
    pendingUiScale_ = 0.0f;
  }

  // Decided before drawing, so the picture is laid out around the bar in the
  // same frame the bar appears in.
  const int topInset = toolbarVisible_ ? (int)std::lround(ToolbarHeight()) : 0;
  renderer_.SetTopInset(topInset);
  UpdateHdr();
  renderer_.Draw(EffectiveImage(profile), fieldIndex_);
  // The shape Shift holds while the window is resized.
  window_.SetSizingAspect(renderer_.hasFrame() ? renderer_.pictureAspect() : 0.0, topInset);

  // Right after the first pass, so the still is the picture that was just put on
  // screen -- and before the UI is drawn, so the overlay never lands in it. The
  // other setting takes the same shot one step later; see below.
  if (screenshotPending_ && !config_.record.screenshotIncludeUi) {
    screenshotPending_ = false;
    WriteScreenshot(false, screenshotToClipboard_);
  }

  display_.NewUiFrame();
  window_.BeginUiFrame();
  ImGui::NewFrame();
  DrawUi();
  ImGui::Render();
  // In HDR the interface goes to a buffer of its own first: it is drawn in sRGB
  // and the screen is being fed linear light, so it needs converting rather than
  // copying. In SDR both calls do nothing and it draws straight to the screen.
  const bool uiLayer = renderer_.BeginUiLayer();
  display_.RenderUi(ImGui::GetDrawData());
  if (uiLayer) renderer_.CompositeUiLayer();

  // The other grab point. Everything has been drawn and nothing has been
  // presented yet, which is the only moment the back buffer holds the finished
  // window: under the flip model its contents are undefined after the present.
  if (screenshotPending_) {
    screenshotPending_ = false;
    WriteScreenshot(true, screenshotToClipboard_);
  }

  // Beide Messwerte jedes Bild, unabhaengig davon, ob das Panel offen ist: das
  // Log schreibt seine Statuszeile auch dann, und ein Ruckler waehrend einer
  // geschlossenen Anzeige ist genau der, den man spaeter sucht.
  if (captureState_ == CaptureState::Running) {
    const AudioStats audioNow = audio_.stats();
    if (audioNow.running) audioBufferMeter_.Sample(audioNow.bufferMs, TicksToSeconds(now));
  }
  recording_.SyncMicrophone();
  recording_.FeedRecorder();
  UpdateVirtualCamera();
  FeedFrameConsumers();
  display_.EndFrame(config_.app.vsync);

  // ---- Durchlaufzeit ----
  // Erst hier, weil erst hier feststeht, wann das Bild qBlank verlaesst.
  // Gemessen wird die Strecke, fuer die dieses Programm geradesteht: von der
  // Ankunft in der Senke bis zu dem Augenblick, in dem Present zurueckkehrt und
  // das Bild dem Compositor gehoert. Alles dazwischen zaehlt mit -- das
  // Hochladen, Deinterlacing, Filter, Skalierung, das Zeichnen, die
  // Present-Warteschlange und bei eingeschaltetem VSync das Warten auf den
  // Bildwechsel.
  //
  // Was davor liegt (Halbbildaufnahme, Karte, Treiber, Transport) und was
  // danach kommt (Compositor, Kabel, die Elektronik des Schirms), ist von hier
  // aus nicht messbar und deshalb auch nicht enthalten. Die Zahl ist der
  // Beitrag von qBlank, nicht das Alter des Lichts.
  //
  // Vorher stand hier die Ankunftszeit gegen den Zeichenbeginn, und weil die
  // Schleife auf das Bildereignis wartet, war das fast immer dieselbe Zehntel
  // Millisekunde -- eine Zahl, die nur sagte, dass der Renderthread wach
  // geworden ist.
  if (captureState_ == CaptureState::Running && displayedArrivalQpc_ != 0) {
    const int64_t leaving = ClockTicks();
    const double ageMs = TicksToSeconds(leaving - displayedArrivalQpc_) * 1000.0;
    if (ageMs >= 0.0) frameAgeMeter_.Sample(ageMs, TicksToSeconds(leaving));
  }

  // ---- present rate ----
  ++presentCount_;
  if (fpsWindowQpc_ == 0) fpsWindowQpc_ = now;
  const double elapsed = TicksToSeconds(now - fpsWindowQpc_);
  if (elapsed >= 1.0) {
    presentFps_ = presentCount_ / elapsed;
    presentCount_ = 0;
    fpsWindowQpc_ = now;


    // With logging on, write a line every few seconds. This is what makes a
    // "it stutters" report actionable without having to reproduce it here.
    if (config_.app.logToFile && captureState_ == CaptureState::Running) {
      if (++statsLogCounter_ >= 5) {
        statsLogCounter_ = 0;
        const SinkStats sinkStats = sink ? sink->stats() : SinkStats{};
        const AudioStats audioStats = audio_.stats();
        // Mittel und Spitze ueber die ganzen fuenf Sekunden statt eines
        // Augenblickswerts. Ein Ruckler ist ein einzelnes Bild -- die Chance,
        // ihn mit einer Stichprobe alle fuenf Sekunden zu erwischen, ist etwa
        // eins zu dreihundert, und genau danach wird in diesen Zeilen gesucht.
        double ageHigh = 0.0;
        frameAgeMeter_.TakeRange(nullptr, &ageHigh);
        double bufLow = 0.0, bufHigh = 0.0;
        audioBufferMeter_.TakeRange(&bufLow, &bufHigh);
        CAP_LOG("Status: source %.2f fps, output %.1f fps, %llu shown, %llu dropped, frame age "
                "%.1f ms (peak %.1f) | fields %llu/%llu | audio %.1f/%.0f ms (%.1f-%.1f), %llu "
                "underruns, %llu overruns",
                sinkStats.sourceFps, presentFps_, (unsigned long long)sinkStats.displayed,
                (unsigned long long)sinkStats.dropped, frameAgeMeter_.average, ageHigh,
                (unsigned long long)fieldsShown_[0], (unsigned long long)fieldsShown_[1],
                audioBufferMeter_.average, audioStats.targetMs, bufLow, bufHigh,
                (unsigned long long)audioStats.underruns,
                (unsigned long long)audioStats.overruns);
      }
    }
  }
}

void App::DrawUi() {
  const Profile& profile = config_.active();
  FrameBuffer* sink = capture_.sink();

  // ---- toolbar ----
  // Windowed it is simply there; in fullscreen it follows the pointer, which is
  // already hidden after a couple of seconds of play.
  toolbarVisible_ = config_.app.showToolbar && !cropTool_.active() &&
                    (!fullscreen_ || !window_.cursorHidden());
  if (toolbarVisible_) {
    DrawToolbarStrip();
    // Everything else positions itself against the viewport work area, which is
    // exactly what it is for: the part of the window not taken by a bar. Moving
    // it here means the statistics, the toasts, the status card and the settings
    // dialog all keep clear of the toolbar without knowing it exists. ImGui
    // resets this at the start of every frame.
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float reserved = ToolbarHeight();
    vp->WorkPos.y += reserved;
    vp->WorkSize.y -= reserved;
  }

  // ---- status card, or the empty state ----
  if (!HaveLiveSignal()) {
    if (captureState_ == CaptureState::Reconnecting) {
      // This one keeps the card: it interrupts a picture that was there a moment
      // ago and is expected back, and the spinner says so.
      DrawStatusCard(T("Verbindung unterbrochen", "Connection lost"),
                     captureError_.empty() ? std::string() : captureError_, true);
    } else if (captureState_ == CaptureState::Running) {
      const VideoRenderer::SignalVerdict verdict = renderer_.detectedSignal();
      DrawIdleScreen(
          idleIcon_.id(), idleIconSize_,
          verdict == VideoRenderer::SignalVerdict::Snow
              ? T("Kein Signal — die Karte empfängt nur Rauschen. Kabel und Eingang prüfen.",
                  "No signal — the card is receiving noise only. Check the cable and input.")
              : T("Kein Signal. Quelle eingeschaltet? Richtiger Eingang gewählt?",
                  "No signal. Is the source on? Is the right input selected?"));
    } else if (!settings_.isOpen()) {
      // Beim ersten Start eine Begruessung statt einer Fehlermeldung. Es ist
      // derselbe Bildschirm -- Zeichen, Schriftzug, eine Zeile darunter --, nur
      // sagt die Zeile hier, was als Naechstes zu tun ist, statt zu melden, dass
      // etwas fehlt. Beim ersten Mal fehlt naemlich noch nichts.
      //
      // Und sie nennt beide Wege hinein. Das Rechtsklickmenue steht sonst
      // nirgends: es ist der schnellere von beiden, weil das Naheliegende darin
      // gleich anklickbar ist statt hinter einem Reiter, aber wer nicht auf die
      // Idee kommt, ins Bild zu klicken, findet es nie.
      DrawIdleScreen(
          idleIcon_.id(), idleIconSize_,
          firstRun_ ? T("Willkommen, bitte zuerst die Capture-Karte auswählen: F2 öffnet die "
                        "Einstellungen,\n"
                        "oder Rechtsklick für das Kontextmenü.",
                        "Welcome, please select your capture device first by pressing F2 to open "
                        "settings,\n"
                        "or right click for the context menu.")
                    : T("Kein Gerät aktiv — Rechtsklick oder F2 öffnet die Einstellungen.",
                        "No device active — right-click or press F2 for the settings."));
    }
  }

  // ---- Normensuche ----
  //
  // Nur solange etwas laeuft: "Eingestellt: PAL B" gehoert in den Dialog, nicht
  // dauerhaft ins Bild. Und nicht, waehrend der Dialog offen ist -- dort steht
  // dieselbe Auskunft schon, ausfuehrlicher.
  if (!settings_.isOpen()) {
    const SettingsWindow::StandardSearch search = standardSearch_.StandardSearchDisplay();
    switch (search) {
      case SettingsWindow::StandardSearch::Trying:
      case SettingsWindow::StandardSearch::Colour: {
        // Kopfzeile und Grund kommen aus einer Hand, weil sie zusammengehoeren:
        // welcher Schritt von wie vielen laeuft, haengt daran, welche der
        // beiden Stufen gerade sucht.
        std::string headline, detail;
        standardSearch_.StandardSearchText(&headline, &detail);
        DrawSearchIndicator(headline, detail);
        break;
      }
      // Und was daraus geworden ist, an derselben Stelle: die Einblendung wird
      // nicht ersetzt, sie hoert auf zu laufen. Nur nach einem Suchlauf von
      // Hand -- die Automatik sucht bei jedem Quellenwechsel, und ein Ergebnis
      // nach jedem waere kein Ergebnis mehr, sondern ein Bildschirmelement.
      case SettingsWindow::StandardSearch::Result: {
        std::string headline, detail;
        standardSearch_.StandardSearchText(&headline, &detail);
        DrawSearchResult(headline, detail, standardSearch_.ResultAgeSeconds(),
                         VideoStandardSearch::ResultSeconds());
        break;
      }
      // Die Pause ist kein Vorgang, sondern deren Abwesenheit -- dafuer laufende
      // Punkte ins Bild zu setzen, waere gelogen. Der Dialog sagt es weiterhin.
      case SettingsWindow::StandardSearch::Paused:
      case SettingsWindow::StandardSearch::Off:
        break;
    }
  }

  // ---- stats ----
  if (config_.app.showStats) {
    OverlayStats stats;
    stats.profileName = profile.name;
    stats.deviceName = capture_.resolvedDevice().name;
    const auto& inputs = capture_.capabilities().crossbarInputs;
    if (profile.capture.crossbarInput >= 0 &&
        profile.capture.crossbarInput < (int)inputs.size()) {
      stats.inputName = inputs[(size_t)profile.capture.crossbarInput].name;
    }
    stats.format = renderer_.sourceFormat();
    if (sink) stats.sink = sink->stats();
    stats.audio = audio_.stats();
    stats.presentFps = presentFps_;
    stats.frameAge = frameAgeMeter_;
    stats.audioBuffer = audioBufferMeter_;
    stats.vsync = config_.app.vsync;
    stats.tearing = display_.tearingSupported();
    // Vier Zustaende, und der erste ist derjenige, der sonst wie ein Fehler
    // aussieht: solange gemessen wird, steht das auch da. Die Erkennung braucht
    // rund eine Sekunde bewegtes Bild, und wer in dieser Sekunde hinsieht, soll
    // "wird gemessen" lesen und nicht ein "progressiv", das gleich widerrufen
    // wird.
    if (!renderer_.sourceFormat().valid()) {
      stats.scanLabel = "—";
    } else if (profile.image.deinterlaceAuto && !renderer_.sourceFormat().interlaced &&
               renderer_.detectedInterlace() == VideoRenderer::InterlaceVerdict::Pending) {
      stats.scanLabel = T("wird gemessen", "measuring");
    } else if (!SourceLooksInterlaced(profile)) {
      stats.scanLabel = T("progressiv", "progressive");
    } else if (renderer_.sourceCoSitedFields()) {
      stats.scanLabel = T("deckungsgleich (240p/288p)", "aligned (240p/288p)");
    } else if (profile.image.deinterlace == Deinterlace::Off) {
      stats.scanLabel = T("interlaced, kein Deinterlacer", "interlaced, no deinterlacer");
    } else {
      stats.scanLabel = std::string(T("interlaced, ", "interlaced, ")) +
                        DeinterlaceName((int)profile.image.deinterlace);
    }
    // Der Toast ist weg, sobald man kurz weggesehen hat, und die Einstellungen
    // sind zu. Diese Zeile ist die eine Flaeche, die dauerhaft sichtbar ist --
    // wer sich fragt, warum das Bild weicher geworden ist, findet die Antwort
    // dort, wo er ohnehin nachsieht. Ein Fragezeichen und nicht mehr: der
    // Zustand steht davor, dies ist nur der Zweifel daran.
    if (InterlaceVerdictDoubtful(profile)) stats.scanLabel += T(" (?)", " (?)");
    const Rect& r = renderer_.videoRect();
    stats.displayWidth = (int)(r.right - r.left);
    stats.displayHeight = (int)(r.bottom - r.top);
    stats.filterName = ScaleFilterName((int)profile.image.filter);
    stats.videoDelayMs = (int)delayLine_.delayMs();
    stats.detail = config_.app.statsDetail;
    // Spell out what "automatic" resolved to, since that is the setting people
    // second-guess when a picture looks wrong.
    {
      std::string range = profile.image.range == ColorRange::Auto
                              ? (detectedRangeText_ ? detectedRangeText_
                                                    : T("wird gemessen", "measuring"))
                              : ColorRangeName((int)profile.image.range);
      std::string matrix = ColorMatrixName((int)profile.image.matrix);
      stats.colorInfo = range + "  /  " + matrix;
    }
    DrawStatsPanel(stats);
  }

  // ---- crop picker ----
  if (cropTool_.active()) {
    clickOnPicture_ = false;
    cropTool_.DrawCropPicker();
    if (!cropTool_.active()) return;  // Apply or Cancel closed it this frame
  } else {
    DragCompareDivider();
    DoubleClickFullscreen();
    DragBorderlessWindow();
  }

  // ---- recording indicator ----
  if (recorder_.recording()) {
    DrawRecordIndicator(recorder_.stats().seconds, config_.app.osdCorner);
  }

  // ---- volume readout ----
  {
    const double age = ImGui::GetTime() - volumeOsdStart_;
    if (age < kVolumeOsdSeconds) {
      DrawVolumeOsd(profile.audio.volume, profile.audio.mute, config_.app.osdCorner, age,
                    kVolumeOsdSeconds);
    }
  }

  // ---- toast ----
  DrawToastStrip();

  DrawContextMenu();

  // ---- settings ----
  // The banner explains why the dialog opened by itself. Once the card is
  // actually running the reason is gone, whichever route got it there --
  // picking a device, F5, or the automatic retry. Leaving it up until the
  // dialog is closed and reopened reads like the selection did not take.
  if (captureState_ == CaptureState::Running) settings_.ClearReason();

  switch (renderer_.detectedRange()) {
    case VideoRenderer::RangeVerdict::Limited:
      detectedRangeText_ = T("begrenzt (16-235)", "limited (16-235)");
      break;
    case VideoRenderer::RangeVerdict::Full:
      detectedRangeText_ = T("voll (0-255)", "full (0-255)");
      break;
    default:
      detectedRangeText_ = nullptr;
      break;
  }
  settings_.SetDetectedRange(&detectedRangeText_);

  // Die Zahlen hinter dem Urteil. Der Anteil unter 16 ist der, an dem es
  // haengt (die Schwelle steht bei 0,2 %), der ueber 235 steht daneben, weil
  // Superweiss in einem begrenzten Signal erlaubt ist und beim Ablesen sonst
  // wie ein Widerspruch aussieht.
  {
    const VideoRenderer::RangeNumbers n = renderer_.rangeNumbers();
    if (n.samples > 0) {
      rangeNumbersText_ = Format(T("min %d, max %d, %.2f %% unter 16, %.2f %% über 235, "
                                   "%llu Proben",
                                   "min %d, max %d, %.2f %% below 16, %.2f %% above 235, "
                                   "%llu samples"),
                                 n.min, n.max, 100.0 * (double)n.below16 / (double)n.samples,
                                 100.0 * (double)n.above235 / (double)n.samples,
                                 (unsigned long long)n.samples);
    } else {
      rangeNumbersText_.clear();
    }
  }
  settings_.SetRangeNumbers(&rangeNumbersText_);

  switch (renderer_.detectedInterlace()) {
    case VideoRenderer::InterlaceVerdict::Interlaced:
      detectedInterlaceText_ = renderer_.sourceCoSitedFields()
                                   ? T("interlaced, 240p/288p-Quelle",
                                       "interlaced, 240p/288p source")
                                   : T("interlaced", "interlaced");
      break;
    case VideoRenderer::InterlaceVerdict::Progressive:
      detectedInterlaceText_ = T("progressiv", "progressive");
      break;
    default:
      detectedInterlaceText_ = nullptr;
      break;
  }
  settings_.SetDetectedInterlace(&detectedInterlaceText_);
  settings_.SetInterlaceDoubtful(InterlaceVerdictDoubtful(profile));

  // Ein 1080p- oder 720p-Bild, das die Karte als progressiv meldet und das hier
  // trotzdem als interlaced gemessen wurde: das ist kaum je richtig, und wer
  // gerade zusieht, merkt sonst nur, dass das Bild ploetzlich weicher wird,
  // ohne den Grund zu finden. Die Meldung nennt deshalb gleich den Reiter, in
  // dem es abzustellen ist.
  {
    const bool doubtful = InterlaceVerdictDoubtful(profile);
    if (doubtful && !interlaceDoubtToasted_) {
      interlaceDoubtToasted_ = true;
      const VideoFormatInfo fmt = renderer_.sourceFormat();
      CAP_LOG("Interlacing detected at %dx%d although the card reports progressive -- hint shown",
              fmt.width, fmt.height);
      Toast(Format(T("Halbbilder bei %dx%d erkannt — bitte prüfen (Reiter Bild)",
                     "Fields detected at %dx%d — please check (Image tab)"),
                   fmt.width, fmt.height));
    } else if (!doubtful) {
      interlaceDoubtToasted_ = false;
    }
  }

  // Und der Hinweis auf einen analogen Eingang, an dem trotzdem der volle
  // Wertebereich ankommt. Anders als das Interlacing darueber gibt es dafuer
  // *keinen* Toast: es ist nichts kaputt, es gibt nichts zu bestaetigen, und die
  // Lage ist an dieser Karte der Normalfall. Wer wissen will, warum Schwarz
  // unten am Anschlag liegt, findet den Hinweis im Reiter Bild, gleich neben dem
  // Regler, um den es geht.
  {
    const bool full = AnalogueRangeIsFull();
    settings_.SetAnalogueFullRange(full);
    if (full && !analogueFullRangeLogged_) {
      analogueFullRangeLogged_ = true;
      const VideoRenderer::RangeNumbers n = renderer_.rangeNumbers();
      CAP_LOG("Analogue input delivers the full range (min %d, max %d) -- hint in the Picture tab",
              n.min, n.max);
    } else if (!full) {
      analogueFullRangeLogged_ = false;
    }
  }

  // Eine Aufloesung neben dem Raster der Norm, siehe ResolutionMismatchLines.
  // Toast einmal beim Eintreten, der Hinweis im Reiter Quelle, solange es so
  // bleibt -- mit dem Knopf, der die passende Groesse einstellt.
  {
    const int active = ResolutionMismatchLines();
    int shown = 0;
    if (active <= 0) {
      resolutionMismatchSince_ = -1.0;
      resolutionMismatchToasted_ = false;
    } else if (resolutionMismatchSince_ < 0.0) {
      resolutionMismatchSince_ = ImGui::GetTime();
    } else if (ImGui::GetTime() - resolutionMismatchSince_ >= 3.0) {
      shown = active;
      if (!resolutionMismatchToasted_) {
        resolutionMismatchToasted_ = true;
        const VideoFormatInfo fmt = renderer_.sourceFormat();
        CAP_LOG("Resolution %dx%d does not fit the video standard (%d active lines) -- hint shown",
                fmt.width, fmt.height, active);
        Toast(Format(T("%dx%d passt nicht zur Videonorm (%d Zeilen) — bitte prüfen (Reiter Quelle)",
                       "%dx%d does not fit the video standard (%d lines) — please check (Source "
                       "tab)"),
                     fmt.width, fmt.height, active));
      }
    }
    settings_.SetResolutionMismatch(shown);
  }
  settings_.SetCoSitedFields(renderer_.sourceCoSitedFields());
  settings_.SetSignalLocked(standardSearch_.PollSignalLocked());
  settings_.SetStandardSearch(standardSearch_.StandardSearchDisplay());
  settings_.SetLiveStandard(capture_.running() ? standardSearch_.signalStandard()
                                               : 0);
  settings_.SetProbeAllowed(captureState_ != CaptureState::Reconnecting);
  settings_.SetUpdater(&updater_);
  // Auto means: analogue when the card has a decoder for it. A card that only
  // does one of the two therefore needs nobody to say which.
  // Dieselbe Quelle der Wahrheit, die auch entscheidet, was ueberhaupt noch
  // gezeichnet wird. Liefen die beiden auseinander, wirkte etwas, das nirgends
  // mehr einstellbar ist.
  settings_.SetAnalogueSource(SourceIsAnalogue());
  // Und derselbe Weg fuer den Anschluss, aufgeloest statt roh: was der Dialog
  // im Reiter Bild noch zeigen darf, haengt an dem, was "Automatisch" ergibt,
  // und nicht an dem, was dort ausgewaehlt ist.
  settings_.SetConnector(ResolvedConnector());
  // Und dieselbe Wahrheit noch einmal an den Renderer, der daran entscheidet,
  // ob die kachelweise Interlacing-Erkennung mitreden darf.
  renderer_.SetAnalogueSource(SourceIsAnalogue());
  // Und die gemessene Ankunftsrate dazu, mit der die Erkennung ein Kammurteil
  // verwerfen kann, das der Formatraum nicht hergibt -- siehe SetFrameRateHint.
  // Gemessen, nicht angekuendigt: die Karte kuendigt bei einer Halbbildquelle
  // 50 an und liefert 25 gewebte Bilder, und mit der angekuendigten Zahl haette
  // das Veto genau die Quellen erwischt, vor denen es schuetzen soll. Ohne
  // Messung 0, und 0 heisst "noch nicht gemessen", nicht "steht still".
  double measuredFps = 0.0;
  if (const FrameBuffer* sink = capture_.sink()) {
    const double measured = sink->stats().sourceFps;
    if (measured > 1.0) measuredFps = measured;
  }
  renderer_.SetFrameRateHint(measuredFps);
  settings_.SetSourceFps(renderer_.sourceFormat().fps);
  // Woraus sich entscheidet, welche Abschnitte im Reiter Bild erscheinen: die
  // Zeilenzahl fuer die Bildroehreneffekte, die Halbbilder fuer das
  // Deinterlacing. Beides aus der Quelle, nicht daraus, ob sie analog ist --
  // 1080i gibt es ueber HDMI, und 480p gibt es von einem RetroTINK.
  settings_.SetSourceHeight(renderer_.sourceFormat().valid() ? renderer_.sourceFormat().height : 0);
  settings_.SetSourceInterlaced(
      renderer_.sourceFormat().interlaced ||
      renderer_.detectedInterlace() == VideoRenderer::InterlaceVerdict::Interlaced);
  settings_.SetScanlineRoom(renderer_.scanlineRoom());
  settings_.SetLevels(audio_.inputPeak(), mic_.peak(), mic_.running());
  settings_.SetViewAids(compare_, bypass_);
  recording_.UpdateDiskSpace();
  if (settings_.takeCompareToggle()) ToggleCompare();
  if (settings_.takeBypassToggle()) ToggleBypass();
  if (settings_.takeCropPickRequest()) cropTool_.BeginCropPick();
  if (settings_.takeDeviceConfigRequest()) OpenDeviceConfig();
  if (settings_.takeCropDetectRequest()) cropTool_.DetectCrop();
  if (settings_.takeCardResetRequest()) ReinitialiseCard();
  if (settings_.takeRangeRemeasureRequest()) RemeasureRange();
  settings_.setProbeBusy(recording_.probing());
  // Drawn here only when the settings live inside the picture. The separate
  // window is deliberately not touched from in here: this runs between the main
  // context's NewFrame and Render, and presenting a second swapchain in the
  // middle of another window's frame flushes every bit of GPU work already
  // queued for it -- sixty times a second, while the preview runs at twice that
  // or more. That was not merely qBlank stuttering; it was enough to make the
  // desktop's own cursor stutter. It happens after the present instead.
  if (!settingsAreWindowed()) {
    settings_.SetFillsWindow(false);
    if (settings_.Draw(capture_.running() ? &capture_.capabilities() : nullptr,
                       &recording_.ffmpeg()) == SettingsWindow::Result::Close) {
      settings_.Close();
    }
  }
  if (settings_.takeProbeRequest()) recording_.StartEncoderProbe(true);
  DrawWelcome();
  DrawCrashNotice();
  DrawUpdatePrompt();
  if (settings_.takeRestartRequest()) {
    if (updater_.RestartIntoNewBuild()) {
      // The new build is coming up; this one gets out of its way so the window
      // position and the configuration are written before it reads them.
      running_ = false;
    } else {
      Toast(T("Neustart fehlgeschlagen.", "Restart failed."));
    }
  }

  // Everything above may have edited the configuration in place, so act on it
  // here in one spot rather than sprinkling apply calls through the UI code.
  SyncConfigChanges();
  MaybeSaveConfig();
}

void App::DrawContextMenu() {
  if (!ImGui::BeginPopupContextVoid("qblank_context", ImGuiPopupFlags_MouseButtonRight)) return;

  // Shortcut labels come from the live bindings, so rebinding a key is visible
  // here immediately instead of leaving the menu quietly lying about it. The
  // strings have to outlive the frame, hence the static buffer per action.
  auto sc = [this](HotkeyAction action) -> const char* {
    static std::string text[(int)HotkeyAction::Count];
    const int i = (int)action;
    text[i] = config_.hotkeys[action].bound() ? HotkeyText(config_.hotkeys[action]) : std::string();
    return text[i].empty() ? nullptr : text[i].c_str();
  };

  if (ImGui::MenuItem(T("Einstellungen...", "Settings..."), sc(HotkeyAction::Settings))) OpenSettings({});
  ImGui::Separator();

  bool fs = fullscreen_;
  if (ImGui::MenuItem(T("Vollbild", "Fullscreen"), sc(HotkeyAction::Fullscreen), &fs)) SetFullscreen(fs);

  // Whole multiples of the lines, so integer scaling does not mean hunting for
  // the size by hand. The width follows the aspect, as in the Integer mode, and
  // the toolbar is added on top so the picture itself gets the size.
  if (ImGui::BeginMenu(T("Fenstergröße", "Window size"), !fullscreen_ && renderer_.hasFrame())) {
    const int inset = toolbarVisible_ ? (int)std::lround(ToolbarHeight()) : 0;
    for (int factor = 1; factor <= 3; ++factor) {
      int w = 0, h = 0;
      if (!renderer_.PictureSizeAt(factor, &w, &h)) continue;
      const bool current = !window_.maximized() && display_.width() == w &&
                           display_.height() == h + inset;
      const std::string label = Format("%d× (%d × %d)", factor, w, h);
      if (ImGui::MenuItem(label.c_str(), nullptr, current, window_.ClientSizeFits(w, h + inset))) {
        window_.SetClientSize(w, h + inset);
      }
    }
    ImGui::EndMenu();
  }

  bool top = config_.app.alwaysOnTop;
  if (ImGui::MenuItem(T("Immer im Vordergrund", "Always on top"), nullptr, &top)) {
    config_.app.alwaysOnTop = top;
    ApplyWindowFlags();
  }

  bool borderless = config_.app.borderless;
  if (ImGui::MenuItem(T("Rahmenlos", "Borderless"), nullptr, &borderless)) config_.app.borderless = borderless;

  bool stats = config_.app.showStats;
  if (ImGui::MenuItem(T("Statistik", "Statistics"), sc(HotkeyAction::Stats), &stats)) config_.app.showStats = stats;

  bool toolbar = config_.app.showToolbar;
  if (ImGui::MenuItem(T("Werkzeugleiste", "Toolbar"), nullptr, &toolbar)) {
    config_.app.showToolbar = toolbar;
  }

  // Same reasoning as the colour menu below, only more so: whether the standard
  // is right is something you see instantly, and on a console that switches
  // between 50 and 60 Hz it is the setting you reach for most.
  //
  // Both conditions, exactly as the settings window has them. A card with an
  // analogue decoder still reports its whole standard list while it is showing
  // HDMI, and on that input every entry in it is dead: the decoder is not in the
  // path at all. Offering them anyway is a menu that does nothing, which is
  // worse than one that is not there.
  const long availableStandards =
      capture_.running() ? capture_.capabilities().availableStandards : 0;
  if (availableStandards != 0 && SourceIsAnalogue() &&
      ImGui::BeginMenu(T("Videonorm", "Video standard"))) {
    CaptureSettings& cap = config_.active().capture;
    const long before = cap.videoStandard;

    if (ImGui::MenuItem(T("Automatisch", "Automatic"), nullptr, cap.videoStandard == -1)) {
      cap.videoStandard = -1;
    }
    if (ImGui::MenuItem(T("Nicht ändern", "Leave alone"), nullptr, cap.videoStandard == 0)) {
      cap.videoStandard = 0;
    }
    ImGui::Separator();
    const int chosen = VideoStandardGroupOf(cap.videoStandard);
    for (int i = 0; i < VideoStandardGroupCount(); ++i) {
      const long value = VideoStandardGroupPick(i, availableStandards);
      if (value == 0) continue;
      if (ImGui::MenuItem(VideoStandardGroupName(i), nullptr, chosen == i)) {
        cap.videoStandard = value;
      }
      WrappedTooltip(VideoStandardGroupHint(i));
    }

    if (cap.videoStandard != before) {
      // A different standard usually means a different number of lines, so the
      // graph has to come up again around the new format. Automatic is the one
      // case that does not restart here: it has nothing to apply yet and will
      // rebuild by itself once it has found something that locks.
      if (cap.videoStandard > 0) {
        std::string error;
        if (StartCapture(&error)) {
          Toast(Format(T("Videonorm: %s", "Video standard: %s"),
                       VideoStandardPickerName(cap.videoStandard).c_str()));
        } else {
          Toast(error);
        }
      }
      SaveConfig();
    }
    ImGui::EndMenu();
  }

  // Right here rather than buried in the dialog: wrong levels or a wrong matrix
  // are things you spot by looking at the picture, and both take effect on the
  // very next frame, so switching them while watching is the fastest way to
  // land on the right one.
  if (ImGui::BeginMenu(T("Farbe", "Colour"))) {
    ImageSettings& img = config_.active().image;

    ImGui::SeparatorText(T("Wertebereich", "Range"));
    for (int i = 0; i < 3; ++i) {
      // "##range" keeps the id unique: the first entry of both lists is called
      // "Automatic", and two menu items with the same label in one menu share an
      // id, which Dear ImGui reports as a programmer error.
      const std::string label = std::string(ColorRangeName(i)) + "##range";
      if (ImGui::MenuItem(label.c_str(), nullptr, (int)img.range == i)) {
        img.range = (ColorRange)i;
        Toast(std::string(T("Wertebereich: ", "Range: ")) + ColorRangeName(i));
      }
    }

    ImGui::SeparatorText(T("Farbmatrix", "Colour matrix"));
    for (int i = 0; i < 3; ++i) {
      const std::string label = std::string(ColorMatrixName(i)) + "##matrix";
      if (ImGui::MenuItem(label.c_str(), nullptr, (int)img.matrix == i)) {
        img.matrix = (ColorMatrix)i;
        Toast(std::string(T("Farbmatrix: ", "Colour matrix: ")) + ColorMatrixName(i));
      }
    }
    ImGui::EndMenu();
  }

  // Volume lives in the menu as well as on the wheel: the menu is where you
  // look when you cannot remember the shortcut.
  AudioSettings& audio = config_.active().audio;
  bool muted = audio.mute;
  if (ImGui::MenuItem(T("Stumm", "Muted"), sc(HotkeyAction::Mute), &muted)) ToggleMute();

  ImGui::SetNextItemWidth(180.0f * uiScale_);
  float percent = audio.volume * 100.0f;
  if (ImGui::SliderFloat(T("Lautstärke", "Volume"), &percent, 0.0f, 100.0f, "%.0f %%")) {
    audio.volume = Clamp(percent / 100.0f, 0.0f, 1.0f);
    audio.mute = false;
    audio_.ApplySettings(audio);
    applied_.volume = audio.volume;
    applied_.mute = audio.mute;
  }

  if (config_.profiles.size() > 1 && ImGui::BeginMenu(T("Profil", "Profile"))) {
    for (int i = 0; i < (int)config_.profiles.size(); ++i) {
      const bool selected = (i == config_.activeProfile);
      std::string shortcut = i < 9 ? Format(T("Strg+%d", "Ctrl+%d"), i + 1) : std::string();
      if (ImGui::MenuItem(config_.profiles[(size_t)i].name.c_str(),
                          shortcut.empty() ? nullptr : shortcut.c_str(), selected)) {
        SwitchProfile(i);
      }
    }
    ImGui::EndMenu();
  }

  ImGui::Separator();
  {
    const bool rec = recorder_.recording();
    if (ImGui::MenuItem(rec ? T("Aufnahme stoppen", "Stop recording")
                            : T("Aufnahme starten", "Start recording"),
                        sc(HotkeyAction::Record))) {
      recording_.ToggleRecording();
    }
  }
  if (ImGui::MenuItem(T("Screenshot", "Screenshot"), sc(HotkeyAction::Screenshot))) RequestScreenshot();
  if (ImGui::MenuItem(T("Screenshot in die Zwischenablage", "Screenshot to clipboard"),
                      sc(HotkeyAction::ScreenshotClipboard))) {
    RequestScreenshot(true);
  }

  // Beides hilft beim Einstellen und aendert nur die Anzeige, deshalb stehen
  // sie hier und nicht in den Einstellungen: man greift danach, waehrend man
  // auf das Bild sieht.
  if (ImGui::MenuItem(T("Standbild", "Freeze"), sc(HotkeyAction::Freeze), frozen_)) {
    ToggleFreeze();
  }
  WrappedTooltip(T("Hält das Bild an, damit man Filter in Ruhe einstellen kann. Die Filter "
                   "laufen weiter, nur die Quelle steht.",
                   "Holds the picture so filters can be set in peace. The filters keep "
                   "running; only the source stands still."));
  // Die Richtung gleich mit, statt sie im Einstellungsfenster zu suchen. Die
  // Taste schaltet den Vergleich in der zuletzt gewaehlten Richtung, deshalb
  // steht sie am Menue und nicht an einer der beiden.
  if (BeginMenuWithShortcut(T("Filter vergleichen", "Compare filters"),
                            sc(HotkeyAction::Compare))) {
    const bool across = config_.active().image.compareHorizontal;
    const char* hint = T("Teilt das Bild: auf einer Seite alle Filter aus, auf der anderen an. "
                         "Das Deinterlacing läuft auf beiden Seiten, die Trennlinie lässt sich "
                         "im Bild ziehen. Noch einmal wählen schaltet den Vergleich aus.",
                         "Splits the picture: every filter off on one side, on on the other. "
                         "Deinterlacing runs on both sides, and the divider can be dragged in "
                         "the picture. Choose it again to switch off.");
    if (ImGui::MenuItem(T("Senkrecht – links ungefiltert", "Vertical – unfiltered on the left"),
                        nullptr, compare_ && !across)) {
      ChooseCompare(false);
    }
    WrappedTooltip(hint);
    if (ImGui::MenuItem(T("Waagerecht – oben ungefiltert", "Horizontal – unfiltered on top"),
                        nullptr, compare_ && across)) {
      ChooseCompare(true);
    }
    WrappedTooltip(hint);
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(T("Alle Filter aus", "All filters off"), sc(HotkeyAction::BypassFilters),
                      bypass_)) {
    ToggleBypass();
  }
  WrappedTooltip(T("Zeigt das Bild ohne Composite-Filter, Schärfen, Bildregler, natives Raster "
                   "und Bildröhre. Deinterlacing, Zuschnitt, Seitenverhältnis und Skalierung "
                   "bleiben. Wird nicht gespeichert.",
                   "Shows the picture without composite filters, sharpening, picture controls, "
                   "native pixel grid and CRT effects. Deinterlacing, crop, aspect and scaling "
                   "stay. Not saved."));

  // Der schwarze Rand ist etwas, das man sieht, und das Suchen danach gehoert
  // deshalb dorthin, wo man hinsieht, statt in einen Reiter des
  // Einstellungsfensters. Zumal die Messung nur so gut ist wie das Bild, das
  // gerade anliegt: sie sucht die Grenzen dessen, was nicht schwarz ist, und
  // auf einem Ladebildschirm sind das die Grenzen des Ladebildschirms. Wer den
  // Knopf im Vorbeigehen erreicht, drueckt ihn im richtigen Moment noch einmal.
  if (ImGui::MenuItem(T("Rand suchen", "Detect border"), sc(HotkeyAction::DetectCrop))) {
    cropTool_.DetectCrop();
  }
  WrappedTooltip(T("Schneidet den schwarzen Rand weg, den die Karte mitliefert. Braucht ein "
                   "richtiges Bild — auf Schwarz gemessen kommt Unsinn heraus.",
                   "Crops the black border the card delivers. Needs a real picture — measured "
                   "on black it produces nonsense."));

  // Und daneben dasselbe fuer die Norm. Der Grund, es hier anzubieten, ist
  // derselbe wie beim Rand: ausgeloest wird es, weil man etwas *sieht* -- ein
  // Bild, dessen Farben nicht stimmen --, und was man sieht, sieht man nicht im
  // Einstellungsfenster.
  if (ImGui::MenuItem(T("Videonorm suchen", "Detect video standard"),
                      sc(HotkeyAction::DetectStandard))) {
    standardSearch_.RescanVideoStandard();
  }
  WrappedTooltip(T("Sucht die Videonorm neu, auch wenn die eingestellte hält. Für den Fall, "
                   "dass die Farben falsch sind, ohne dass es sich messen ließe.",
                   "Searches for the video standard again, even when the current one holds. "
                   "For colours that are wrong in a way no measurement catches."));

  // Und die dritte Messung derselben Art. Sie steht hier vor allem deshalb,
  // weil sie sonst nur im Einstellungsfenster zu erreichen waere -- und wer im
  // Treiber der Karte etwas umstellt, hat qBlank im Ruecken, nicht offen.
  if (ImGui::MenuItem(T("Wertebereich neu messen", "Measure range again"),
                      sc(HotkeyAction::RemeasureRange))) {
    RemeasureRange();
  }
  WrappedTooltip(T("Das Urteil über voll oder begrenzt steht, bis sich das Bildformat ändert. "
                   "Nach einer Umstellung im Treiber der Karte hiermit neu messen.",
                   "The verdict on full or limited holds until the picture format changes. "
                   "After changing something in the card's driver, measure again here."));

  if (ImGui::MenuItem(T("Aufnahme neu starten", "Restart capture"), sc(HotkeyAction::RestartCapture))) RestartAll(true);
  if (ImGui::MenuItem(T("Karte neu einlesen", "Reinitialise card"), sc(HotkeyAction::ReinitCard))) ReinitialiseCard();
  if (ImGui::MenuItem(T("Beenden", "Quit"), "Alt+F4")) window_.RequestClose();

  ImGui::EndPopup();
}

// ------------------------------------------------------------------ messages

bool App::HandleKeyDown(Key key, bool ctrl, bool shift, bool alt) {
  // Fixed on purpose, see hotkeys.h: a rebindable Escape is a way to lock
  // yourself into fullscreen, and the profile digits are a block, not a key.
  if (ctrl && key >= Key::Digit1 && key <= Key::Digit9) {
    SwitchProfile((int)key - (int)Key::Digit1);
    return true;
  }
  if (cropTool_.active()) {
    if (key == Key::Escape) {
      cropTool_.EndCropPick(false);
      return true;
    }
    if (key == Key::Enter) {
      cropTool_.EndCropPick(true);
      return true;
    }
    return true;  // swallow everything else: one job at a time
  }

  if (key == Key::Escape) {
    if (settings_.isOpen()) {
      settings_.Close();
    } else if (fullscreen_) {
      SetFullscreen(false);
    }
    return true;
  }

  switch (config_.hotkeys.Find(key, ctrl, shift, alt)) {
    case HotkeyAction::Fullscreen:
      ToggleFullscreen();
      return true;
    case HotkeyAction::Settings:
      // A settings window lost behind the preview is fetched, not closed: the
      // key was pressed to see it, and closing what cannot be seen is the one
      // thing that cannot be what was meant.
      if (!settings_.isOpen()) {
        OpenSettings({});
      } else if (!settingsHost_.RaiseIfCoveredBy(window_)) {
        settings_.Close();
      }
      return true;
    case HotkeyAction::Stats:
      config_.app.showStats = !config_.app.showStats;
      return true;
    case HotkeyAction::RestartCapture:
      RestartAll(true);
      return true;
    case HotkeyAction::ReinitCard:
      ReinitialiseCard();
      return true;
    case HotkeyAction::Record:
      recording_.ToggleRecording();
      return true;
    case HotkeyAction::Screenshot:
      RequestScreenshot();
      return true;
    case HotkeyAction::ScreenshotClipboard:
      RequestScreenshot(true);
      return true;
    case HotkeyAction::Freeze:
      ToggleFreeze();
      return true;
    case HotkeyAction::Compare:
      ToggleCompare();
      return true;
    case HotkeyAction::BypassFilters:
      ToggleBypass();
      return true;
    case HotkeyAction::DetectCrop:
      cropTool_.DetectCrop();
      return true;
    case HotkeyAction::DetectStandard:
      standardSearch_.RescanVideoStandard();
      return true;
    case HotkeyAction::RemeasureRange:
      RemeasureRange();
      return true;
    case HotkeyAction::Mute:
      ToggleMute();
      return true;
    case HotkeyAction::VolumeUp:
      AdjustVolume(kVolumeStep);
      return true;
    case HotkeyAction::VolumeDown:
      AdjustVolume(-kVolumeStep);
      return true;
    default:
      break;
  }

  // The numeric keypad follows the main volume keys without needing its own
  // binding -- nobody expects to have to bind both.
  if (key == Key::NumAdd && config_.hotkeys[HotkeyAction::VolumeUp].bound()) {
    AdjustVolume(kVolumeStep);
    return true;
  }
  if (key == Key::NumSubtract && config_.hotkeys[HotkeyAction::VolumeDown].bound()) {
    AdjustVolume(-kVolumeStep);
    return true;
  }
  return false;
}

bool App::OnKey(Key key, bool ctrl, bool shift, bool alt, bool busy) {
  // The binding editor wants the raw key, before anyone acts on it -- that is
  // the whole point of it being open. From either window: in the separate one
  // it used to wait for a key that never reached it.
  if (settings_.waitingForKey()) {
    settings_.OfferKey(key, ctrl, shift, alt);
    return true;
  }
  // Typing, or a list is open: the key is ImGui's, Esc included -- it cancels
  // the edit or closes the list before it closes anything bigger.
  if (busy) return false;
  return HandleKeyDown(key, ctrl, shift, alt);
}

bool App::OnWindowEvent(const WindowEvent& e) {
  switch (e.kind) {
    case WindowEvent::Kind::ModalFrame:
      // Same as for the settings window: the platform takes the loop away while
      // a window is being dragged, and this is what still gets through.
      if (!inModalFrame_) {
        inModalFrame_ = true;
        Tick();
        RenderFrame();
        inModalFrame_ = false;
      }
      return true;
    case WindowEvent::Kind::Resized:
      minimized_ = e.minimized;
      if (!minimized_) display_.Resize();
      return true;
    case WindowEvent::Kind::DpiChanged:
      // Can arrive in the middle of a frame -- a SetWindowPos from the UI that
      // lands the window on another monitor sends it right away -- so the
      // interface is rescaled before the next frame, not here.
      pendingUiScale_ = e.dpiScale;
      return false;

    case WindowEvent::Kind::MouseMoved:
      if (e.mouse.x != lastMousePos_.x || e.mouse.y != lastMousePos_.y) {
        lastMousePos_ = e.mouse;
        lastMouseMoveQpc_ = ClockTicks();
        window_.SetCursorHidden(false);
      }
      return true;

    case WindowEvent::Kind::MouseWheel:
      // Only over the picture: inside the settings window the wheel scrolls.
      if (config_.app.wheelVolume && imguiReady_ && !ImGui::GetIO().WantCaptureMouse) {
        if (e.wheelNotches != 0) {
          AdjustVolume(kVolumeStep * (float)e.wheelNotches);
          return true;
        }
      }
      return false;

    case WindowEvent::Kind::KeyDown: {
      // Not WantCaptureKeyboard, which keyboard navigation holds true whenever
      // the embedded panel has focus -- see SettingsHost::OnWindowEvent.
      const bool busy = imguiReady_ &&
                        (ImGui::GetIO().WantTextInput ||
                         ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId |
                                                    ImGuiPopupFlags_AnyPopupLevel));
      return OnKey(e.key, e.ctrl, e.shift, e.alt, busy);
    }

    case WindowEvent::Kind::ScreenSaverStarting:
      // Block the screensaver from starting over our window.
      return config_.app.preventSleep;

    case WindowEvent::Kind::DisplaysChanged:
    case WindowEvent::Kind::DevicesChanged:
      settings_.InvalidateDeviceLists();
      return false;

    case WindowEvent::Kind::ThemeChanged:
      if (config_.app.theme == Theme::System) ApplyTheme();
      return false;

    case WindowEvent::Kind::CloseRequested:
      SaveConfig();
      running_ = false;
      RequestQuit();
      return true;

    // Windows shutting down or logging off ends the process as soon as this
    // returns, without the way out through main -- the log would take that for
    // a crash at the next start.
    case WindowEvent::Kind::SessionEnd:
      if (e.ending) {
        SaveConfig();
        CAP_LOG("Windows session ending");
        LogEnd();
      }
      return true;

    case WindowEvent::Kind::Destroyed:
      running_ = false;
      RequestQuit();
      return true;

    default:
      return false;
  }
}

}  // namespace cap
