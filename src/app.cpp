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
// Wo eine Aufnahme nicht mehr anfaengt und eine laufende aufhoert. Ein fester
// Boden neben der Restzeit: unter ein paar hundert Megabyte wird Windows selbst
// unruhig, und die letzten Bytes eines Dateisystems sind die langsamsten.
const uint64_t kDiskFloorBytes = 256ull * 1024 * 1024;
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
  } else if (config_.active().capture.video.empty()) {
    OpenSettings(T("Noch kein Aufnahmegerät ausgewählt. Wähle unten die Capture-Karte aus.",
                   "No capture device selected yet. Pick your capture card below."));
  } else if (!StartCapture(&error)) {
    OpenSettings(error);
  }
  CaptureAppliedState();
  ffmpeg_ = LocateFfmpeg(config_.record.ffmpegPath);
  // No test on startup. Which encoders work does not change from one run to the
  // next, so the answer is loaded from the config; testing is something the user
  // asks for once, or when the machine changed underneath it.
  LoadCachedEncoders();

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
  tray_.Hide();
  if (window_.created()) SaveWindowPlacement();
  // Before anything else: closing the pipes is what makes ffmpeg finalise the
  // container, and that has to happen while the app is still alive.
  StopRecording();
  if (probeThread_.joinable()) probeThread_.join();
  StopSignalWatch();
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
  tray_.SetDarkMenus(dark);
}

// ---------------------------------------------------------------------- tray

namespace {

// Commands in the icon's menu. The quick actions are numbered after TrayItem so
// the menu and the list in the settings cannot drift apart.
constexpr int kTrayShow = 1;
constexpr int kTrayQuit = 2;
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
    tray_.SetDarkMenus(darkMode_);
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
    tray_.SetMenu(std::move(menu));
  }

  // Not from inside a move or resize: they wait there until it is over.
  if (inModalFrame_) return;
  for (int id : tray_.TakeCommands()) RunTrayCommand(id);
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
  menu.back().isDefault = true;

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

  group();
  add(kTrayQuit, T("Beenden", "Quit"));
  return menu;
}

void App::RunTrayCommand(int id) {
  if (id == TrayIcon::kActivate || id == kTrayShow) {
    window_.Raise();
    return;
  }
  if (id == kTrayQuit) {
    window_.RequestClose();
    return;
  }
  if (id >= kTrayProfileBase) {
    SwitchProfile(id - kTrayProfileBase);
    return;
  }
  switch ((TrayItem)(id - kTrayItemBase)) {
    case TrayItem::Record:
      ToggleRecording();
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
  StartSignalWatch();
  return true;
}

void App::StopCapture() {
  StopSignalWatch();
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
  if (embeddedPanel || cropPick_.active || toastUp || osdUp) return 16.0;
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
    standardCandidate_ = -1;
    standardSweeps_ = 0;
    standardNextTryQpc_ = 0;
    standardPatientPass_ = false;
    ResetStandardColourCheck();
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

void App::StartEncoderProbe(bool full) {
  if (probing_.load(std::memory_order_relaxed) || !ffmpeg_.found) return;
  if (probeThread_.joinable()) probeThread_.join();

  probeDone_.store(false, std::memory_order_relaxed);
  probing_.store(true, std::memory_order_relaxed);
  // The thread works on its own copy and publishes it in one go, so the UI
  // thread never sees a half filled structure.
  FfmpegInfo copy = ffmpeg_;
  probeThread_ = std::thread([this, copy, full]() mutable {
    SystemThreadScope onThisThread;
    if (full) {
      ProbeEncoders(&copy);
    } else {
      EnsureUsableEncoder(&copy, config_.record.encoder);
    }
    probeResult_ = std::move(copy);
    probing_.store(false, std::memory_order_relaxed);
    probeDone_.store(true, std::memory_order_release);
  });
}

void App::CollectEncoderProbe() {
  if (!probeDone_.load(std::memory_order_acquire)) return;
  probeDone_.store(false, std::memory_order_relaxed);
  if (probeThread_.joinable()) probeThread_.join();
  ffmpeg_ = std::move(probeResult_);
  SaveCachedEncoders();
}

std::string App::EncoderSignature() const {
  // The ffmpeg build belongs in here as well as the hardware: a different build
  // can be missing an encoder the last one had, and would then be judged by a
  // result it never produced.
  return GraphicsAdapterSignature() + " | " + ffmpeg_.version;
}

void App::LoadCachedEncoders() {
  if (!ffmpeg_.found) return;
  RecordSettings& rec = config_.record;
  if (rec.encodersAvailable.empty() && rec.encoderProbeSignature.empty()) return;

  if (rec.encoderProbeSignature != EncoderSignature()) {
    CAP_LOG("Encoder probe discarded: hardware or ffmpeg has changed");
    rec.encoderProbeSignature.clear();
    rec.encodersAvailable.clear();
    return;
  }

  ApplyCachedProbe(&ffmpeg_, rec.encodersAvailable);
  CAP_LOG("Encoders taken from the configuration: %zu usable",
          rec.encodersAvailable.size());
}

void App::SaveCachedEncoders() {
  if (!ffmpeg_.found || !ffmpeg_.tested) return;
  RecordSettings& rec = config_.record;
  rec.encodersAvailable.clear();
  for (const EncoderInfo& e : ffmpeg_.encoders) {
    if (e.tested && e.available) rec.encodersAvailable.push_back((int)e.id);
  }
  rec.encoderProbeSignature = EncoderSignature();

  // A selection that did not survive the test would silently fail at F9, so it
  // goes back to Auto rather than staying as a trap.
  const EncoderInfo* chosen = ffmpeg_.Find(rec.encoder);
  if (!IsAutoEncoder(rec.encoder) && (!chosen || !chosen->available)) {
    CAP_WARN("Chosen encoder is not available, back to automatic");
    rec.encoder = RecordEncoder::Auto;
    Toast(T("Der gewählte Encoder funktioniert hier nicht, zurück auf Automatisch.",
            "The selected encoder does not work here, back to Automatic."));
  }
}

void App::ToggleRecording() {
  if (recorder_.recording()) {
    StopRecording();
  } else {
    StartRecording();
  }
}

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

void App::StartRecording() {
  if (recorder_.recording()) return;

  // The path was resolved at startup and could have gone stale since -- a
  // portable build on a stick that got unplugged, an antivirus quarantine, or
  // simply someone tidying up. Checking costs one file attribute lookup and
  // turns a confusing "ffmpeg exited with code 1" into an honest answer.
  if (ffmpeg_.found && !IsFile(Utf8ToPath(ffmpeg_.path))) {
    CAP_WARN("ffmpeg has disappeared: %s", ffmpeg_.path.c_str());
    ffmpeg_ = FfmpegInfo{};
  }
  if (!ffmpeg_.found) {
    ffmpeg_ = LocateFfmpeg(config_.record.ffmpegPath);
    settings_.InvalidateFolderFields();
  }
  if (!ffmpeg_.found) {
    Toast(T("ffmpeg fehlt — in den Einstellungen unter Aufnahme holen.",
            "ffmpeg is missing — fetch it under Recording in the settings."));
    return;
  }
  if (captureState_ != CaptureState::Running || !renderer_.hasFrame()) {
    Toast(T("Kein Bild zum Aufnehmen.", "No picture to record."));
    return;
  }

  if (probing_.load(std::memory_order_relaxed)) {
    Toast(T("Encoder werden noch geprüft, gleich nochmal.",
            "Still testing encoders, try again in a moment."));
    return;
  }
  const EncoderInfo* usable = ffmpeg_.Resolve(config_.record.encoder);
  if (!usable || !usable->available) {
    // Nothing tested yet: kick a full probe off in the background rather than
    // freezing the window here, and let the user press again. Full, so the
    // settings end up with the complete list rather than the first answer that
    // happened to work.
    StartEncoderProbe(true);
    Toast(T("Encoder werden geprüft, gleich nochmal.",
            "Testing encoders, try again in a moment."));
    return;
  }

  // The tap has to exist before the recorder pulls from it, and the readback
  // before the first frame is fetched.
  const bool wantAudio = audio_.running() && audio_.tapSampleRate() > 0;
  if (wantAudio) audio_.SetTapEnabled(true);
  renderer_.SetReadbackEnabled(true);

  // Start the microphone before asking it for its rate: the device decides that,
  // and ffmpeg has to be told it on the command line.
  SyncMicrophone(true);
  const bool wantMic = mic_.running() && mic_.sampleRate() > 0;
  if (wantMic) mic_.ResetBuffer();

  // Resolve the folder before handing it over, so a deleted directory is
  // recreated -- or answered with the default -- instead of failing the start.
  RecordSettings settings = config_.record;
  const std::filesystem::path folder =
      ResolveOutputFolder(&config_.record.outputFolder, DefaultRecordFolder());
  if (folder.empty()) {
    renderer_.SetReadbackEnabled(false);
    audio_.SetTapEnabled(false);
    Toast(T("Zielordner nicht verfügbar.", "Folder not available."));
    return;
  }
  settings.outputFolder = PathToUtf8(folder);

  // Gar nicht erst anfangen, wenn der Platz schon jetzt nicht reicht: ffmpeg
  // wuerde starten, seinen Kopf schreiben und sterben, und liegen bliebe eine
  // Datei von ein paar Kilobyte, die aussieht wie eine Aufnahme.
  uint64_t freeNow = 0;
  if (DiskFreeBytes(folder, &freeNow) && freeNow < kDiskFloorBytes) {
    renderer_.SetReadbackEnabled(false);
    audio_.SetTapEnabled(false);
    Toast(Format(T("Zu wenig Speicherplatz — nur noch %s frei.",
                   "Not enough disk space — only %s free."),
                 FormatBytes(freeNow).c_str()));
    return;
  }

  const VideoFormatInfo format = renderer_.sourceFormat();
  std::string error;
  Recorder::AudioSource mainTrack;
  if (wantAudio) {
    mainTrack.sampleRate = audio_.tapSampleRate();
    mainTrack.pull = [this](float* out, size_t frames) { return audio_.ReadTap(out, frames); };
  }
  Recorder::AudioSource micTrack;
  if (wantMic) {
    micTrack.sampleRate = mic_.sampleRate();
    micTrack.pull = [this](float* out, size_t frames) { return mic_.Read(out, frames); };
  }

  // Which picture the writer will be fed, decided before it starts: the wide
  // path only exists while the source is HDR and the setting asks for it.
  renderer_.SetHdrWideWanted(config_.app.recordHdr, virtualCamera_.wantsWide());
  const bool wideRecording = config_.app.recordHdr && renderer_.hdrWideActive();
  recorder_.SetPixelFormat(wideRecording ? VideoRenderer::kHdrReadbackPixelFormat
                                         : VideoRenderer::kReadbackPixelFormat,
                           wideRecording);

  // Mit welcher Bildrate aufgenommen wird -- und die gemeldete ist es nicht.
  //
  // `format.fps` kommt aus `AvgTimePerFrame` des Medientyps, und das Feld haelt
  // nicht, was sein Name verspricht: es steht da, was zuletzt jemand
  // hineingeschrieben hat. Am 30.08. um 14:00 Uhr erzwang das Profil noch ein
  // `720x480 @ 59,94 RGB32` aus den NTSC-Testlaeufen, die Karte lehnte es ab,
  // fiel auf `720x576 YUY2` zurueck -- und liess die 59,94 im Kopf stehen. Die
  // echte Quelle war PAL mit gemessenen 25,00 Bildern.
  //
  // Der Recorder haelt seine Rate gegen die Tonuhr durch und verdoppelt
  // notfalls Bilder, um sie zu erreichen. Die Aufnahme um 14:02 lief deshalb
  // mit 59,94 statt der 50,0, die im Status danebenstanden: rund hundert
  // Bilder wurden gedoppelt, ohne dass eines davon neu war. Falsch abgespielt
  // wird nichts -- die Dauer stimmte auf 10,41 s genau -- aber die Datei nennt
  // eine Bildrate, die die Quelle nie hatte, und traegt sie mit.
  //
  // Der Deinterlacer weiter unten in `Tick` steht vor derselben Frage und
  // beantwortet sie seit jeher so: die gemessene Ankunftsrate gewinnt, wenn es
  // eine gibt. Hier gilt dasselbe -- nur muss das Bobbing dazu, weil dabei aus
  // jedem Halbbild ein Vollbild wird und wirklich doppelt so viele verschiedene
  // Bilder den Renderer verlassen. Gemessene 25,00 und Bobbing ergeben also die
  // 50,0, die die Statuszeile die ganze Aufnahme ueber gezeigt hat; die
  // gemeldeten 59,94 waren an keiner Stelle die Rate von irgendetwas. An einer
  // NTSC-Quelle rechnet dieselbe Zeile 29,97 x 2 = 59,94 -- die native Rate
  // dieser Karte, die 60 gar nicht kann.
  //
  // Verdoppelt wird nach der *Einstellung*, nicht nach dem gerade gemessenen
  // Zustand, und das ist wichtig. `SourceLooksInterlaced` misst bei
  // eingeschalteter Automatik am Bildinhalt, und ein stehendes Menuebild hat
  // keine Kammlinien: die Erkennung sagt dort "progressiv" und kippt erst,
  // wenn sich etwas bewegt. Wer auf dem Menue aufnimmt und dann losfaehrt,
  // haette die Aufnahme sonst auf 25 fps festgenagelt, und der Recorder wirft
  // gegen die Tonuhr jedes zweite Bild weg -- also genau die zweiten
  // Halbbilder, um derentwillen ueberhaupt gebobbt wird. Nach dem Start laesst
  // sich nichts mehr richten, `-r` steht in der ffmpeg-Zeile fest.
  //
  // Die Obergrenze kostet dafuer bei einer wirklich progressiven Quelle mit
  // eingeschalteter Automatik doppelte Bilder in der Datei. Das ist der
  // billigere der beiden Fehler: Platz laesst sich nachtraeglich sparen,
  // weggeworfene Halbbilder nicht.
  double recordFps = format.fps;
  if (const FrameBuffer* sink = capture_.sink()) {
    const double measured = sink->stats().sourceFps;
    if (measured > 1.0) recordFps = measured;
  }
  // Vor dem Doppeln festgehalten: `FeedRecorder` vergleicht damit, ob die
  // Quelle waehrend der Aufnahme eine andere geworden ist, und das soll ein
  // umgestellter Deinterlacer nicht ausloesen.
  recordSourceFps_ = recordFps;
  pendingSince_ = -1.0;
  if (config_.active().image.deinterlace != Deinterlace::Off) recordFps *= 2.0;

  // Alle drei Sehhilfen wuerden in die Datei wandern, weil Aufnahme und virtuelle
  // Kamera hinter denselben Durchgaengen abgreifen wie die Anzeige. Also fallen
  // sie hier weg, statt die Aufnahme zu verweigern: wer aufnimmt, meint das
  // ganze Bild von jetzt. Erst hier, weil alles davor noch abbrechen kann.
  if (frozen_) {
    frozen_ = false;
    delayLine_.Clear();
  }
  compare_ = false;
  bypass_ = false;

  // Die Platzwarnung gilt je Aufnahme, und die naechste Abfrage soll nicht die
  // Zahl von vor einer Sekunde benutzen, um sofort zu warnen oder zu schweigen.
  diskWarned_ = false;
  lastDiskCheck_ = -1000.0;

  const bool ok = recorder_.Start(settings, ffmpeg_, renderer_.outputWidth(),
                                  renderer_.outputHeight(), recordFps, mainTrack, micTrack,
                                  config_.active().audio.micTrackMode, &error);

  if (!ok) {
    renderer_.SetReadbackEnabled(false);
    audio_.SetTapEnabled(false);
    Toast(error);
    return;
  }
  Toast(T("Aufnahme läuft", "Recording"));
}

void App::StopRecording() {
  if (!recorder_.recording()) return;
  const RecordStats stats = recorder_.stats();
  recorder_.Stop();
  renderer_.SetReadbackEnabled(false);
  audio_.SetTapEnabled(false);
  Toast(Format(T("Aufnahme gespeichert (%.0f s)", "Recording saved (%.0f s)"), stats.seconds),
        Utf8ToPath(stats.file));
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

void App::UpdateDiskSpace() {
  // Nur wenn jemand hinsieht oder etwas davon abhaengt. Ein Laufwerk im
  // Sekundentakt zu befragen, waehrend niemand die Zahl braucht, weckt eine
  // schlafende Platte fuer nichts.
  if (!settings_.isOpen() && !recorder_.recording()) return;

  // Wie viele Tonspuren in der Datei landen wuerden -- jede kostet ihre
  // 192 kbit/s. Dieselbe Rechnung wie in BuildCommandLine: die Mischung ist
  // eine eigene Spur, und "beide" heisst Mischung *und* die zwei einzelnen.
  const bool haveMain = audio_.running() && audio_.tapSampleRate() > 0;
  const bool haveMic = mic_.running() && mic_.sampleRate() > 0;
  int tracks = (haveMain ? 1 : 0) + (haveMic ? 1 : 0);
  if (haveMain && haveMic) {
    const MicTrackMode mode = config_.active().audio.micTrackMode;
    if (mode == MicTrackMode::Mixed) tracks = 1;
    if (mode == MicTrackMode::Both) tracks = 3;
  }
  diskBytesPerSecond_ = EstimatedBytesPerSecond(config_.record, tracks);

  const double now = ImGui::GetTime();
  if (now - lastDiskCheck_ > 1.0) {
    lastDiskCheck_ = now;
    // Absichtlich nicht ueber ResolveOutputFolder: das legt Ordner an und meldet
    // sich mit Einblendungen, und beides hat eine Abfrage im Sekundentakt nicht
    // zu tun. DiskFreeBytes geht selbst bis zum naechsten vorhandenen Elternteil
    // hoch, der Ordner muss also noch gar nicht existieren.
    const std::filesystem::path folder = config_.record.outputFolder.empty()
                                             ? DefaultRecordFolder()
                                             : Utf8ToPath(config_.record.outputFolder);
    diskFreeKnown_ = DiskFreeBytes(folder, &diskFreeBytes_);
  }
  settings_.SetDiskFree(diskFreeBytes_, diskFreeKnown_, diskBytesPerSecond_);
}

void App::SyncMicrophone(bool aboutToRecord) {
  const AudioSettings& a = config_.active().audio;

  // Held open only while it is actually being used: during a recording, and
  // while the settings are open so the level meter means something. Keeping a
  // microphone open the rest of the time would light up the Windows privacy
  // indicator for no reason and tell the user something untrue.
  const bool wanted =
      a.micEnabled && (aboutToRecord || recorder_.recording() || settings_.isOpen());

  // Changing the device is a fresh start, and clears a previous failure.
  if (!(micApplied_ == a.micDevice)) {
    micApplied_ = a.micDevice;
    micAttempted_ = false;
    micFailed_ = false;
    if (mic_.running()) mic_.Stop();
  }

  if (wanted && !mic_.running() && !micFailed_) {
    std::string error;
    micAttempted_ = true;
    if (!mic_.Start(a.micDevice, &error)) {
      // Not fatal: the recording still happens, just without the second track.
      // Latched, because retrying every frame would fill the log and the screen
      // with the same message sixty times a second.
      micFailed_ = true;
      if (!error.empty()) Toast(error);
    }
  } else if (!wanted && mic_.running()) {
    mic_.Stop();
    micAttempted_ = false;
  }
  mic_.SetGain(a.micGain);
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
    case ToolbarAction::ToggleRecording: ToggleRecording(); break;
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
          ? SaveScreenshotAvif(path, Utf8ToPath(ffmpeg_.path), halfPixels.data(), width, height,
                               halfStride, config_.app.paperWhiteNits, &error)
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

// ------------------------------------------------------------- crop picker
//
// Typing four numbers and checking the result is a loop nobody enjoys, so the
// edges can be dragged on the picture instead. While picking, the crop is set
// to zero so the whole frame is visible -- otherwise you would be cropping an
// already cropped image and the numbers would compound.

// How long the lock has to be missing before anything is changed, and how long
// a freshly set standard is given to prove itself. Both err on the generous
// side: a console being switched between 50 and 60 Hz drops out for a moment on
// its own, and reacting to that would mean changing the card's setting every
// time somebody opens a menu.
static const double kStandardLostSeconds = 1.5;
// Gemessen am 30.08.2026 an einer SA7160, sechs echte Locks: 0,78 / 0,72 /
// 0,76 s bei frisch eingeschalteter Quelle und 0,30 / 0,39 / 0,55 s bei bereits
// anliegendem Signal. 1,5 s ist gut das Doppelte des schlechtesten Wertes --
// eng genug, dass ein voller Durchlauf von rund zwanzig auf zwoelf Sekunden
// faellt, und weit genug, dass keiner dieser Locks knapp geworden waere.
//
// Der eine Ausreisser von 2,00 s aus einer frueheren Messung ist bewusst nicht
// abgedeckt: dort lief die Konsole selbst noch hoch. Dieser Fall verliert den
// Durchlauf ohnehin, weil zu dem Zeitpunkt auch die *richtige* Norm nicht
// einrastet -- gerettet wird er nicht von einem laengeren Fenster, sondern vom
// naechsten Durchlauf.
//
// Von 1,5 auf 1,25 s heruntergesetzt, und zwar nicht, weil enger gerechnet
// wird, sondern weil das Gemessene kleiner geworden ist. In jedem der zehn
// Werte oben steckt das Warten auf zwei frische Messwerte des Wachtthreads
// mit drin, und das waren bei 250 ms Takt bis zu 0,5 s davon; bei 100 ms sind
// es bis zu 0,2 s.
//
// Hier stand zuerst 1,0 s, hergeleitet aus genau dieser Rechnung: der
// schlechteste alte Wert von 0,88 s muesste auf gut 0,6 s fallen, und 1,0 s
// stuende dazu wie vorher 1,5 s zu 0,88 s. Die erste Messung mit dem neuen
// Takt hat das nicht bestaetigt -- am 31.08. um 03:09:16, ein verlorener Lock
// auf PAL B und der Wechsel zurueck auf PAL 60: **0,73 s**. Gegen 1,0 s ist
// das Faktor 1,37, wo vorher 1,7 stand, und da der Poll-Anteil jetzt bis zu
// 0,2 s betraegt, liegt der schlechteste Fall dieser Art bei rund 0,83 s.
//
// 1,25 s stellt das Verhaeltnis wieder her (Faktor 1,5 auf 0,83 s) und behaelt
// den groesseren Teil des Gewinns: die Frist faellt um ein Sechstel, das
// Warten davor um bis zu 0,3 s, und die wirklich teuren Faelle nimmt ohnehin
// die Kuerzung bei Bildmangel weiter unten weg. Eine Vorhersage durch eine
// Messung ersetzt, nicht durch eine zweite Vorhersage.
//
// Nachpruefbar bleibt es an derselben Zeile: "Video standard found
// automatically: ... (lock after %.2f s)". Bleiben diese Werte unter 0,85 s, stimmt die
// Rechnung; kommen sie in die Naehe von 1,25, gehoert die Frist zurueck auf
// 1,5.
static const double kStandardSettleSeconds = 1.25;
// Und die kurze Frist des ersten Durchgangs.
//
// Die Frist darueber deckt jeden gemessenen Lock ab, auch den einer Quelle,
// die selbst noch hochfaehrt. Das ist richtig fuer den Kandidaten, der die
// Antwort ist, und Verschwendung fuer die uebrigen: eine Norm mit der falschen
// Zeilenzahl rastet nie ein, sie sitzt die Frist nur ab. In einer Liste von
// acht Kandidaten sind das im schlechtesten Fall zehn Sekunden, von denen neun
// auf Normen entfallen, die von der ersten Zehntelsekunde an widerlegt sind.
//
// Also zwei Durchgaenge. Der erste geht die Liste schnell ab und nimmt in Kauf,
// einen langsamen Lock zu verpassen; findet er nichts, geht der zweite dieselbe
// Liste mit der vollen Frist. Verworfen wird dabei nie etwas endgueltig -- was
// der schnelle Durchgang liegen laesst, bekommt der geduldige --, und der
// schlechteste Fall waechst nur um die Laenge des schnellen Durchgangs.
//
// Der Wert kommt aus derselben Messreihe wie kStandardSettleSeconds. Sechs
// echte Locks: 0,78 / 0,72 / 0,76 s bei frisch eingeschalteter Quelle, 0,30 /
// 0,39 / 0,55 s bei bereits anliegendem Signal, dazu 0,73 s bei einem
// verlorenen Lock. Die Trennlinie liegt nicht zufaellig zwischen diesen beiden
// Gruppen: was den Unterschied macht, ist nicht die Karte, sondern ob die
// Quelle selbst schon stabil ist.
//
// 0,6 s liegt ueber der langsamsten Messung an einem anliegenden Signal (0,55)
// und unter der schnellsten an einer hochfahrenden Quelle (0,72). Genau das
// ist die richtige Stelle: eine Quelle, die noch nicht liefert, verliert den
// ersten Durchgang ohnehin -- zu dem Zeitpunkt rastet auch die *richtige* Norm
// nicht ein, ob man ihr nun 0,6 oder 1,25 s gibt --, und sie wird vom zweiten
// eingefangen. Was der schnelle Durchgang gewinnen soll, ist der haeufigste
// Fall ueberhaupt: eine laufende Konsole, die zwischen 50 und 60 Hz umgestellt
// wurde. Dort liegt ein Signal an, und dort liegt jeder gemessene Lock unter
// 0,6 s.
//
// Nachpruefbar an "Video standard found automatically: ... (lock after %.2f s)"
// -- dieselbe Zeile wie oben. Steht davor "fast pass" und liegt der Wert
// dicht unter 0,6, ist die Grenze zu eng; landen umgekehrt Faelle mit
// anliegendem Signal regelmaessig erst im zweiten Durchgang, ebenso.
static const double kStandardFastSeconds = 0.6;
// Die vorgezogenen Kandidaten nach einem verlorenen Lock -- der Partner der
// zuletzt guten Norm und sie selbst -- bekommen deutlich mehr. Eine Konsole,
// die gerade neu startet oder von 50 auf 60 Hz umschaltet, braucht ein paar
// Sekunden, bis ueberhaupt wieder etwas Stabiles aus ihr herauskommt; mit
// 0,6 Sekunden waere man laengst weitergezogen, wenn sie so weit ist.
//
// Von 3,0 auf 2,0 s, und das ist der doppelte Sprung: die Frist selbst faellt
// um eine Sekunde, und sie faellt ganz weg, sobald gar kein Bild mehr ankommt
// -- siehe die Kuerzung in UpdateVideoStandard. Was bleibt, ist der Fall, fuer
// den sie gedacht war: es kommen Bilder, sie rasten nur noch nicht ein. Zwei
// Sekunden sind auch dann noch das Doppelte einer gewoehnlichen Frist, und die
// Konsole, die laenger braucht, wird ohnehin erst von der naechsten Runde
// eingefangen.
static const double kStandardPreferredSeconds = 2.0;
// After a full pass with nothing locking, there is probably no signal at all --
// the console is off. Stop poking the card and look again in a while.
static const double kStandardBackoffSeconds = 6.0;
// Ab wann "es kommt kein Bild mehr" heisst, dass die eingestellte Norm das
// anliegende Signal nicht dekodieren kann. Grosszuegig gegen den Neuaufbau
// des Graphen gewaehlt: waehrenddessen kommt naturgemaess nichts, und ein
// frisch gestarteter Graph liefert an der SA7160 nach rund 0,3 s wieder --
// gemessen am 31.08. um 02:29:37, Neubau in 118 ms. Zwei Sekunden sind das
// Sechsfache davon und trotzdem kurz genug, dass ein wieder angestecktes Kabel
// nicht sekundenlang ins Leere laeuft.
static const double kStandardStarvedSeconds = 2.0;

// Wie lange das Ergebnis eines von Hand ausgeloesten Suchlaufs stehen bleibt.
//
// Es sind zwei Zeilen, und die zweite ist die, um derentwillen es angezeigt
// wird -- fuenf Sekunden reichen, sie in Ruhe zu lesen, ohne dass ein Ergebnis
// laenger im Bild steht als die Suche gedauert hat, die es hervorgebracht hat.
static const double kStandardResultSeconds = 5.0;

// Bis die Karte nach einem Normwechsel wieder saubere Bilder liefert. Die
// Zeilenzahl bleibt gleich, es geht nur um den Farb-PLL, deshalb kurz.
//
// Dass die Frist ueberhaupt etwas tut, ist am 30.08. nachgestellt worden,
// indem sie auf null gesetzt wurde: dann misst jeder Kandidat den
// Umschaltmoment mit und wird zu seinem Vorgaenger hin verschmiert -- PAL N
// 0,047 und 0,064 statt 0,012 bis 0,021, SECAM B in den Tiefen 0,246 statt
// 0,271 bis 0,410. Wie weit sie darueber hinaus Reserve hat, ist nicht
// gemessen.
//
// Sie war frueher gratis: der Rundgang wartet nach einem Normwechsel ohnehin
// auf zwei frische Messwerte des Wachtthreads, und bei 250 ms Takt dauerte das
// ungefaehr ebenso lange. Seit der Takt bei 100 ms liegt -- siehe
// kSignalPollNaps -- ist sie es nicht mehr, sondern war der laengste
// Einzelposten eines Kandidaten.
//
// Sie stand deshalb bei 0,5 s und ist am 31.08. auf 0,10 s gekuerzt worden,
// zusammen mit einer Aenderung, die ihr die eigentliche Arbeit abnimmt: der
// Rundgang wartet jetzt nicht mehr auf die Uhr, sondern darauf, dass die
// Messung nicht mehr steigt (siehe kColourStableGiveUpSeconds und
// VerifyStandardColour). Was hier stehenbleibt, ist nur noch der Boden -- die
// paar Bilder, die der Treiber unmittelbar nach put_TVFormat noch aus der
// alten Einstellung liefert. Wie lange der Farb-PLL darueber hinaus braucht,
// wird nicht mehr geschaetzt, sondern je Kandidat gemessen; PAL 60 braucht
// dreimal so lange wie NTSC 4.43, und das ist genau die Sorte Unterschied, die
// eine gemeinsame Frist nicht abbilden kann.
//
// Sie steht hier statt in VerifyStandardColour, weil sie inzwischen von zwei
// Seiten gebraucht wird: auch der Neubau des Graphen nach einem gefundenen
// Lock legt sie an, damit die erste Messung der neuen Norm hinter dem Umbau
// beginnt statt darueber hinweg zu mitteln. Siehe UpdateVideoStandard.
static const double kColourSettleSeconds = 0.10;

// Wie lange ein Kandidat hoechstens gemessen wird, wenn seine Zahl nicht zur
// Ruhe kommt. Siehe VerifyStandardColour.
//
// PAL 60 -- die langsamste gemessene Norm auf dieser Karte -- ist nach rund
// 0,8 s so weit. Die Obergrenze liegt darueber mit Luft, aber unter dem, was
// vier Kandidaten in Serie ertraeglich machen. Sie ist keine Frist, auf die
// gewartet wird, sondern eine, die nur bei einem bewegten Bild ueberhaupt
// erreicht wird -- und dann steht im Log, dass sie es war.
static const double kColourStableGiveUpSeconds = 1.2;

// Ob eine Messung gegenueber der vorigen noch deutlich gestiegen ist.
//
// Der absolute Anteil faengt die Werte nahe null, wo ein Verhaeltnis nichts
// mehr aussagt: von 0,002 auf 0,004 ist eine Verdopplung und trotzdem grau.
// Der relative faengt die grossen, wo ein fester Abstand zu streng waere.
// Nachgerechnet an der Einschwingkurve von PAL 60: 0,095 auf 0,119 gilt als
// gestiegen (Schwelle 0,014), 0,119 auf 0,119 nicht.
static bool ColourStillRising(float previous, float current) {
  if (previous < 0.0f || current < 0.0f) return false;
  const float tol = 0.008f > previous * 0.15f ? 0.008f : previous * 0.15f;
  return current > previous + tol;
}

// Wie oft der Wachtthread den Decoder nach seinem Lock fragt, in Zehnteln
// einer Zehntelsekunde -- er schlaeft in 10-ms-Haeppchen, damit das Beenden
// nicht darauf warten muss.
//
// Das ist der Boden unter allen Fristen dieser Datei, und er war lange
// unsichtbar. Bevor ueber eine frisch gesetzte Norm geurteilt werden darf,
// wartet `UpdateVideoStandard` auf zwei frische Messwerte -- bei 250 ms Takt
// sind das 0,25 bis 0,5 s, in denen nichts gemessen wird, sondern nur gewartet.
// Genau diese Spanne steckt in jedem gemessenen "lock after"-Wert mit drin:
// zehn davon aus drei Sitzungen liegen zwischen 0,32 und 0,88 s, und ein
// gutes Drittel davon ist dieses Warten.
//
// Bei 100 ms schrumpft es auf 0,1 bis 0,2 s. Das ist die Voraussetzung dafuer,
// dass die Fristen darunter kuerzer werden koennen, ohne enger zu werden --
// gekuerzt wird das Warten, nicht die Messung. Der Preis sind zwei
// Property-Gets auf dem Decoder zehnmal statt viermal je Sekunde, auf einem
// eigenen Thread, weit weg vom Bildweg.
//
// Der Rundgang profitiert genauso: auch er wartet je Kandidat auf zwei frische
// Messwerte, fuenfmal je Runde.
static const int kSignalPollNaps = 10;

void App::StartSignalWatch() {
  StopSignalWatch();
  // The thread keeps its own handle, so tearing the capture down does not pull
  // the card out from under it; the worst that happens is that it asks a card
  // that is no longer running, and gets told so.
  std::shared_ptr<SignalProbe> probe = capture_.signalProbe();
  if (!probe) return;

  signalWatchRun_.store(true, std::memory_order_relaxed);
  signalWatch_ = StartCaptureThread([this, probe]() {
    while (signalWatchRun_.load(std::memory_order_relaxed)) {
      // Beides aus demselben Durchgang: was eingestellt ist und ob es haelt.
      // Getrennt gefragt koennten die zwei aus verschiedenen Momenten stammen,
      // und genau daraus entsteht die Anzeige, die eine Norm als eingerastet
      // meldet, waehrend laengst eine andere auf der Karte steht.
      signalStandard_.store(probe->CurrentStandard(), std::memory_order_relaxed);
      signalLocked_.store(probe->Locked(), std::memory_order_relaxed);
      signalSeq_.fetch_add(1, std::memory_order_release);
      // Split into short naps so shutdown does not have to wait for the interval.
      for (int i = 0; i < kSignalPollNaps && signalWatchRun_.load(std::memory_order_relaxed);
           ++i) {
        SleepMilliseconds(10);
      }
    }
  });
}

void App::StopSignalWatch() {
  signalWatchRun_.store(false, std::memory_order_relaxed);
  if (signalWatch_.joinable()) signalWatch_.join();
  signalLocked_.store(-1, std::memory_order_relaxed);
  signalStandard_.store(0, std::memory_order_relaxed);
}

int App::PollSignalLocked() {
  if (!capture_.running()) return -1;
  return signalLocked_.load(std::memory_order_relaxed);
}

SettingsWindow::StandardSearch App::StandardSearchDisplay() const {
  using S = SettingsWindow::StandardSearch;
  // Ohne laufende Aufnahme oder an einer digitalen Quelle gibt es keine Suche,
  // und der Wachthread laeuft dann auch gar nicht.
  if (captureState_ != CaptureState::Running || !SourceIsAnalogue()) return S::Off;
  // Eine von Hand gesetzte Norm wird nicht gesucht. Sie ist eingestellt, auch
  // wenn nichts einrastet -- dass sie nicht haelt, sagt die Zeile darunter.
  if (config_.active().capture.videoStandard != -1) return S::Off;

  const int locked = signalLocked_.load(std::memory_order_relaxed);
  if (locked < 0) return S::Off;  // noch nichts gemessen
  if (locked == 1) {
    // Eingerastet -- aber die Sache ist erst entschieden, wenn auch die Farbe
    // stimmt. Waehrend des Gegenversuchs steht die *andere* Norm auf der Karte,
    // und ohne diesen Zustand saehe das aus wie ein Ergebnis, das sich von
    // selbst wieder aendert.
    if (!colourCandidates_.empty()) return S::Colour;
    // Von Hand ausgeloest und noch kein Rundgang: gemessen wird trotzdem
    // schon. Der Rundgang beginnt erst, wenn eine Messung vorliegt, und auf
    // einem dunklen Bild kann das dauern -- ohne diese Zeile waere zwischen
    // Tastendruck und Ergebnis eine halbe Minute, in der nichts zu sehen ist
    // und der Tastendruck wie verschluckt aussieht.
    if (standardManualSearch_) return S::Colour;
    if (ShowingStandardResult()) return S::Result;
    return S::Off;
  }
  // Kein Lock: gesucht wird -- es sei denn, gerade laeuft die Pause zwischen
  // zwei Runden.
  //
  // Hier stand nur `standardSweeps_ >= 1`, und das war die falsche Frage. Nach
  // der ersten erfolglosen Runde geht der Zaehler nie wieder auf null, die
  // Zeile blieb also fuer immer bei "Suche pausiert" -- auch waehrend der
  // zweiten, dritten, zehnten Runde, in der sehr wohl gesucht wird. Sichtbar
  // wurde es daran, dass die genannte Norm munter weiterlief, waehrend
  // danebenstand, es sei pausiert: am 30.08. um 13:31 Uhr zeigte der Dialog
  // "Scanning paused, card set to PAL M", und PAL M war Platz 6 einer gerade
  // laufenden Runde. Die Pause ist der Zustand *zwischen* den Runden, und den
  // erkennt man nicht am Zaehler, sondern daran, dass gerade kein Kandidat
  // gesetzt ist.
  //
  // Seit es zwei Durchgaenge gibt, reicht auch das nicht mehr ganz: zwischen
  // dem schnellen und dem geduldigen steht ebenfalls kein Kandidat, und dort
  // wird nicht pausiert, sondern sofort weitergemacht. Der Unterschied ist
  // `standardPatientPass_` -- die Fahne steht in dem Moment schon auf wahr, in
  // dem der geduldige Durchgang beginnt, und faellt erst mit seinem Ende
  // zusammen mit dem Beginn der Pause zurueck. Ohne das haette die Zeile fuer
  // ein Bild lang "Suche pausiert" gezeigt, mitten in der Suche.
  return standardSweeps_ >= 1 && standardCandidate_ < 0 && !standardPatientPass_ ? S::Paused
                                                                                 : S::Trying;
}

// Die beiden Zeilen der Einblendung: was laeuft, und warum es laeuft.
//
// Bis hierher standen dort drei laufende Punkte hinter einem Normnamen, und
// das ist genau die Auskunft, die schon zu sehen war -- das Bild schaltet
// sichtbar durch mehrere Normen. Was nicht zu sehen ist und den Vorgang erst
// erklaert, ist der Grund: dass die Farbe zu blass war fuer eine Norm, die
// stimmen koennte, oder dass Farbe bis ins Schwarze steht, oder dass die Karte
// gar nicht erst einrastet. Der Grund ist gemessen, er liegt vor, und er
// gehoert dorthin, wo der Vorgang zu sehen ist.
//
// Dazu der Zaehler. Eine Suche ohne Zaehler ist eine Suche ohne Ende -- man
// kann ihr nicht ansehen, ob sie in einer Sekunde fertig ist oder in zehn --,
// und der Zaehler ist der Unterschied zwischen Warten und Zusehen.
void App::StandardSearchText(std::string* headline, std::string* detail) const {
  const long shown = capture_.running() ? signalStandard_.load(std::memory_order_relaxed) : 0;
  const int idx = shown != 0 ? VideoStandardIndexOf(shown) : -1;
  const char* name = idx >= 0 ? VideoStandardName(idx) : "?";

  // Fertig, und jemand wartet auf die Antwort. Sie steht fertig da: geschrieben
  // wurde sie an der Stelle, an der die Suche geendet hat, weil nur die weiss,
  // woran sie geendet ist.
  if (ShowingStandardResult()) {
    *headline = standardResultHeadline_;
    *detail = standardResultDetail_;
    return;
  }

  if (!colourCandidates_.empty()) {
    // Der Rundgang misst einen Schritt mehr, als er Normen hat: die
    // Ausgangsnorm kommt am Ende ein zweites Mal dran, siehe
    // VerifyStandardColour. Gezaehlt wird deshalb in Schritten und nicht in
    // Normen -- sonst stuende "5 von 4" im Bild --, und die Klammer in der
    // Zeile darunter sagt, woher der zusaetzliche Schritt kommt.
    const int steps = (int)colourCandidates_.size();
    const int done = colourIndex_ + 1 < steps ? colourIndex_ + 1 : steps;
    *headline = Format(T("Farbe wird geprüft: %s (%d/%d)", "Checking colour: %s (%d/%d)"), name,
                       done, steps);

    const int lines = shown != 0 ? VideoStandardLines(shown) : 0;
    const int norms = steps - 1;
    switch (colourDoubt_) {
      case ColourDoubt::Tinted:
        *detail = Format(
            T("Farbe steht bis ins Schwarze — die %d Normen mit %d Zeilen werden verglichen "
              "(die erste zweimal)",
              "Colour reaches into the blacks — comparing the %d standards with %d lines "
              "(the first one twice)"),
            norms, lines);
        break;
      case ColourDoubt::Manual:
        *detail = Format(
            T("Von Hand ausgelöst — die %d Normen mit %d Zeilen werden verglichen "
              "(die erste zweimal)",
              "Triggered by hand — comparing the %d standards with %d lines "
              "(the first one twice)"),
            norms, lines);
        break;
      default:
        *detail = Format(
            T("Farbe unklar — die %d Normen mit %d Zeilen werden verglichen "
              "(die erste zweimal)",
              "Colour unclear — comparing the %d standards with %d lines "
              "(the first one twice)"),
            norms, lines);
        break;
    }
    return;
  }

  // Von Hand ausgeloest, eingerastet, und der Rundgang hat noch nicht
  // angefangen. Dazwischen liegt eine Messung, und die braucht ein Bild, an
  // dem etwas zu messen ist -- auf Schwarz wartet sie, und das Warten ist hier
  // die ganze Auskunft.
  if (standardManualSearch_ && signalLocked_.load(std::memory_order_relaxed) == 1) {
    *headline = Format(T("Videonorm wird geprüft: %s", "Checking video standard: %s"), name);
    *detail = colourWaitingForPicture_
                  ? T("Das Bild ist zu dunkel für einen Vergleich — es wird gewartet",
                      "The picture is too dark to compare — waiting")
                  : T("Die Farbe wird gemessen", "Measuring the colour");
    return;
  }

  // Stufe eins. Hier steht kein gemessener Grund zur Auswahl -- es gibt genau
  // einen, naemlich dass der Decoder die Zeilenfrequenz nicht findet --, und
  // der Zaehler ist die eigentliche Auskunft.
  if (standardCandidate_ >= 0 && standardCandidateCount_ > 0) {
    *headline = Format(T("Videonorm wird gesucht: %s (%d/%d)", "Scanning video standard: %s (%d/%d)"),
                       name, standardCandidate_ + 1, standardCandidateCount_);
  } else {
    *headline = Format(T("Videonorm wird gesucht: %s", "Scanning video standard: %s"), name);
  }
  // Dazu, welcher der beiden Durchgaenge laeuft. Das ist keine Kleinigkeit
  // fuer den, der zusieht: derselbe Normname taucht ein zweites Mal auf, und
  // ohne diese Zeile sieht das aus, als drehe sich die Suche im Kreis. Die
  // Frist steht mit dabei, weil sie der ganze Unterschied ist.
  *detail = standardPatientPass_
                ? Format(T("Karte rastet nicht ein — zweiter Durchgang, %.2f s je Norm",
                           "Card does not lock — second pass, %.2f s per standard"),
                         kStandardSettleSeconds)
                : Format(T("Karte rastet nicht ein — erster Durchgang, %.1f s je Norm",
                           "Card does not lock — first pass, %.1f s per standard"),
                         kStandardFastSeconds);
}

// Die Normensuche von Hand ausloesen.
//
// Die Automatik hat einen blinden Fleck, und es ist kein kleiner: sie greift,
// wenn der Lock verloren geht oder wenn die Farbe messbar nicht stimmt. Eine
// Norm, die haelt und kraeftig Farbe zeigt, zweifelt sie nicht an -- auch dann
// nicht, wenn die Farben schlicht falsch sind. Genau das sieht aber ein Mensch
// und keine der beiden Messungen: Gras in der falschen Farbe ist Farbe.
//
// Der Umweg dafuer war bisher, im Normwaehler irgendeine feste Norm zu setzen
// und wieder auf Automatisch zu stellen. Das funktioniert und findet niemand,
// der es nicht schon weiss.
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

void App::RescanVideoStandard() {
  if (captureState_ != CaptureState::Running) {
    Toast(T("Keine laufende Quelle.", "No source is running."));
    return;
  }
  if (!SourceIsAnalogue()) {
    Toast(T("Nur analoge Eingänge haben eine Videonorm.",
            "Only analogue inputs have a video standard."));
    return;
  }
  if (config_.active().capture.videoStandard != -1) {
    // Die feste Norm bleibt fest. Sie ist eine Entscheidung, die jemand
    // getroffen hat, und eine Taste, die sie im Vorbeigehen umwirft, waere
    // eine Ueberraschung -- gesagt wird es trotzdem, sonst sieht der
    // Tastendruck wie ein Fehler aus.
    Toast(T("Die Videonorm steht fest — für die Suche auf Automatisch stellen.",
            "The video standard is fixed — set it to Automatic to search."));
    return;
  }

  CAP_LOG("Video standard: search started by hand");
  // Stufe eins von vorn. Auch die Runden zurueck auf null: wer von Hand sucht,
  // will keine Pause, in der nichts geschieht. Und von vorn heisst mit dem
  // schnellen Durchgang: von Hand gesucht wird an einer Quelle, die laeuft und
  // ein Bild liefert -- genau der Fall, fuer den die kurze Frist gemessen ist.
  standardCandidate_ = -1;
  standardLostQpc_ = 0;
  standardSweeps_ = 0;
  standardNextTryQpc_ = 0;
  standardPatientPass_ = false;
  // Und Stufe zwei von vorn, mitsamt dem Rundgang, den die Abkuerzung sonst
  // ueberspringt. Die Frist ist grosszuegig: auf einem gerade schwarzen Bild
  // wartet der Rundgang, und dieses Warten soll er noch tun duerfen.
  ResetStandardColourCheck();
  standardForceColourUntilQpc_ = ClockTicks() + SecondsToTicks(30.0);
  // Ab hier schuldet die Suche eine Antwort, und ein Ergebnis von vorhin ist
  // keine: wer ein zweites Mal drueckt, fragt ein zweites Mal.
  standardManualSearch_ = true;
  standardResultUntilQpc_ = 0;
  Toast(T("Videonorm wird gesucht", "Scanning for the video standard"));
}

// Die Antwort auf den Tastendruck festhalten.
//
// Aufgerufen wird das an jedem Ausgang, den ein Suchlauf nehmen kann -- der
// berichtigten Norm, der bestaetigten, der unentschiedenen Runde, dem zu
// dunklen Bild, der Quelle ohne Signal. Es sind fuenf, und dass es fuenf sind,
// ist der Grund fuer die Fahne: welcher Ausgang genommen wird, entscheidet
// sich weit weg von der Taste, und keiner von ihnen weiss, ob ihn jemand
// abwartet.
//
// Die erste Auskunft gewinnt. Danach ist die Frage beantwortet, und alles
// Weitere -- ein Wiederholer, der zehn Sekunden spaeter doch noch entscheidet
// -- gehoert wieder der Automatik samt ihrem Toast.
void App::FinishManualStandardSearch(long standard, const std::string& detail) {
  if (!standardManualSearch_) return;
  standardManualSearch_ = false;
  const int idx = standard != 0 ? VideoStandardIndexOf(standard) : -1;
  standardResultHeadline_ = Format(T("Videonorm: %s", "Video standard: %s"),
                                   idx >= 0 ? VideoStandardName(idx) : "?");
  standardResultDetail_ = detail;
  standardResultUntilQpc_ = ClockTicks() + SecondsToTicks(kStandardResultSeconds);
  CAP_LOG("Video standard: manual search finished -- %s",
          idx >= 0 ? VideoStandardName(idx) : "?");
}

// Ob das Ergebnis gerade im Bild steht.
//
// Und nur, solange sonst nichts laeuft. Geht der Lock in diesen fuenf Sekunden
// verloren, faengt die Automatik von vorn an -- dann ist das Ergebnis nicht
// mehr wahr, und es tritt hinter die laufende Suche zurueck, statt sie zu
// verdecken.
bool App::ShowingStandardResult() const {
  return standardResultUntilQpc_ != 0 && ClockTicks() < standardResultUntilQpc_ &&
         colourCandidates_.empty() && standardCandidate_ < 0;
}

// Ob der Wachthread ueberhaupt etwas zu beobachten hat.
//
// Er fragt den Analogdecoder zehnmal in der Sekunde nach seinem Lock. An einem
// digitalen Eingang haengt der Decoder gar nicht im Signalweg -- seine Antwort
// ist dort bedeutungslos, und alles, was auf ihr aufbaut, soll sie deshalb
// auch nicht bekommen: weder die automatische Normensuche noch die kuerzere
// Geduld bei flachem Bild in `HoldingSignal`.
//
// Es ist dieselbe Frage, die den Videonorm-Picker aus den Einstellungen und
// aus dem Rechtsklickmenue nimmt. Was nirgends einstellbar ist, darf auch
// nicht im Hintergrund an der Karte drehen.
void App::UpdateSignalWatch() {
  const bool want = captureState_ == CaptureState::Running && SourceIsAnalogue();
  const bool have = signalWatch_.joinable();
  if (want == have) return;

  if (want) {
    StartSignalWatch();
    return;
  }
  StopSignalWatch();
  // Eine halb gelaufene Suche darf nicht liegen bleiben. Kommt spaeter doch
  // eine analoge Quelle, faengt sie von vorne an statt in der Mitte der Liste.
  standardCandidate_ = -1;
  standardLostQpc_ = 0;
  standardSweeps_ = 0;
  standardNextTryQpc_ = 0;
  standardPatientPass_ = false;
  // Und die Frage von Hand verfaellt mit der Quelle, an die sie gestellt war.
  standardManualSearch_ = false;
  standardResultUntilQpc_ = 0;
  // Auch die Erinnerung. Wer von digital zurueck auf analog wechselt, haengt
  // etwas anderes an -- die alte Norm zu bevorzugen waere dann ein Rat aus
  // einem anderen Leben.
  standardLastGood_ = 0;
  ResetStandardColourCheck();
}

void App::UpdateVideoStandard() {
  const Profile& profile = config_.active();
  if (profile.capture.videoStandard != -1) return;  // not our job
  // Und auch dann nicht, solange die Umstellung noch nicht angekommen ist.
  //
  // Das Einstellungsfenster schreibt beim Klick sofort in `config_`, aber
  // `SyncConfigChanges` -- und damit der Ruecksetzer der Suchposition -- laeuft
  // erst am Ende des naechsten Bildes des *Hauptfensters*. In `Tick` steht
  // diese Funktion davor. Wer also von einer festen Norm auf Automatisch
  // zurueckstellt, kaeme genau einmal hier durch, waehrend `standardCandidate_`
  // noch auf dem alten Stand steht: die Suche schriebe der Karte den naechsten
  // Kandidaten der *alten* Position, und erst danach faengt sie richtig von
  // vorne an. Am 30.08. um 13:32:56 so im Log gestanden -- `PAL N` (Platz 6 der
  // abgebrochenen Runde), 679 ms spaeter dann `PAL B` (Platz 0).
  if (applied_.videoStandard != profile.capture.videoStandard) return;
  if (captureState_ != CaptureState::Running) return;
  // Ein digitaler Eingang hat keine Videonorm, die man suchen koennte. Ohne
  // Wachthread stuende unten ohnehin -1 und die Funktion kehrte um; das hier
  // sagt es aber an der Stelle, an der es gemeint ist.
  if (!SourceIsAnalogue()) return;

  // Einmal je Sitzung festhalten, womit die Suche anfaengt. Das ist die erste
  // Frage, wenn die Automatik danebenliegt -- und die Antwort sagt zugleich, ob
  // die Region stimmt, ohne dass die (uebersetzte) Regionsbezeichnung in dieses
  // unuebersetzte Log muesste. Der Normname ist in jeder Sprache derselbe.
  static bool searchStartLogged = false;
  if (!searchStartLogged) {
    const std::vector<long> plan = AutoStandardCandidates(
        capture_.capabilities().availableStandards, config_.app.videoRegion, 0, nullptr);
    if (!plan.empty()) {
      searchStartLogged = true;
      const char* how = "set by hand";
      if (config_.app.videoRegion == VideoRegion::Auto) how = "automatic, from Windows";
      if (config_.app.videoRegion == VideoRegion::None) how = "none, the general order only";
      CAP_LOG("Video standard search: region %s, trying %s first", how,
              VideoStandardName(VideoStandardIndexOf(plan.front())));
    }
  }

  const int64_t now = ClockTicks();

  // Kommt ueberhaupt noch ein Bild an? Vor allen Ausstiegen weiter unten, damit
  // diese Uhr auch dann laeuft, wenn die Funktion an anderer Stelle umkehrt.
  //
  // Das ist der Unterschied zwischen den beiden Arten von "kein Signal", die
  // sich sonst gleich anfuehlen. Ist die Quelle aus, liefert die Karte weiter
  // Bilder, nur eben leere -- daraus wird das Urteil Flat, und dann ist Parken
  // richtig. Steht die Karte dagegen auf einer Norm mit der falschen
  // Zeilenzahl, kommt gar nichts mehr; das letzte Urteil bleibt als Flat
  // stehen, obwohl es ein Bild von vorhin beschreibt.
  //
  // Genau das hat sich am 31.08.2026 aufgehaengt: um 02:02:46 stand die Karte
  // auf NTSC 4.43 (525/60), am Eingang lag wieder ein 625/50-Signal, und der
  // Graph lieferte kein einziges Bild mehr. Der Parkzweig war schon auf seinem
  // Ziel angekommen, tat also nichts, und kehrte um -- alle 1,5 Sekunden, 112
  // Sekunden lang, bis von Hand neu gestartet wurde. Der Abstand zur letzten
  // Ankunft stieg dabei von 633 ms auf 112352 ms, und das ist der Messwert, an
  // dem es haengt. Nicht zu verwechseln mit der Durchlaufzeit im Panel: die
  // misst eine Strecke, diese hier misst eine Stille.
  //
  // Wenn nichts mehr ankommt, ist Weitersuchen keine Stoerung, sondern das
  // Einzige, was das Bild zurueckholen kann.
  // Gemessen wird am Alter des letzten Bildes selbst, nicht daran, wann uns
  // zum ersten Mal aufgefallen ist, dass keines mehr kommt. Der Unterschied
  // sind volle kNoSignalSeconds, die sonst vor der eigentlichen Frist noch
  // einmal verstreichen -- am 31.08. um 02:29:44 standen dadurch 4,3 s im Log,
  // wo 3,0 gemeint waren. Nur solange die Senke ueberhaupt schon einmal ein
  // Bild gesehen hat; danach ist ihr Alter die Messung. Vorher -- frischer
  // Graph, noch nichts angekommen -- bleibt nur, ab dem ersten Hinsehen zu
  // zaehlen.
  const FrameBuffer* frameSink = capture_.sink();
  const double frameAgeMs = frameSink ? frameSink->stats().lastArrivalAgeMs : -1.0;
  bool starved = false;
  if (frameAgeMs >= 0.0) {
    standardStarvedSinceQpc_ = 0;
    starved = frameAgeMs >= kStandardStarvedSeconds * 1000.0;
  } else {
    if (standardStarvedSinceQpc_ == 0) standardStarvedSinceQpc_ = now;
    starved = TicksToSeconds(now - standardStarvedSinceQpc_) >= kStandardStarvedSeconds;
  }
  if (!starved) standardStarvedLogged_ = false;

  // Two fresh readings since the standard was last changed, so what is being
  // judged is the standard that is actually set.
  if (signalSeq_.load(std::memory_order_acquire) - standardSeqAtSet_ < 2) return;

  const int locked = signalLocked_.load(std::memory_order_relaxed);
  if (locked < 0) return;  // no analogue decoder, or nothing measured yet

  if (locked == 1) {
    // Settled. Whatever is set is right, and the search starts from scratch if
    // it is ever needed again.
    standardLostQpc_ = 0;
    standardSweeps_ = 0;
    standardPatientPass_ = false;
    // Woran nach dem naechsten Aussetzer zuerst gedacht wird. Auch dann
    // gemerkt, wenn gar nicht gesucht wurde: eine Norm, die von selbst
    // eingerastet ist, ist genauso ein guter Hinweis wie eine gefundene.
    standardLastGood_ = capture_.currentStandard();
    // Und der Farbtraeger dazu, weil er allein an der Norm haengt und die genau
    // hier neu ist. Der Neubau darunter setzt ihn auch, laeuft aber nur, wenn
    // eine Suche lief; rastet die Karte von sich aus auf etwas anderes ein,
    // rechneten die Composite-Filter sonst weiter mit dem Traeger der Norm
    // davor -- zwischen den beiden Gruppen sind das 24 % daneben, und der
    // Demodulator sucht dann eine Frequenz, die gar nicht da ist.
    renderer_.SetCarrierSamples(VideoStandardSubcarrierSamples(standardLastGood_));
    if (standardCandidate_ >= 0) {
      // A candidate just proved itself. The line count may have changed with
      // it, so the graph has to be rebuilt around the new format.
      standardCandidate_ = -1;
      const long settled = standardLastGood_;
      // Mit der Einfangzeit daneben. Sie sagt, ob die Wartefrist gereicht hat
      // oder ob sie nur knapp gereicht hat -- und was eine Norm braucht, die
      // die Zeilenfrequenz wirklich neu einfangen musste.
      CAP_LOG("Video standard found automatically: %s (lock after %.2f s)",
              VideoStandardName(VideoStandardIndexOf(settled)),
              standardSetQpc_ != 0 ? TicksToSeconds(now - standardSetQpc_) : 0.0);
      standardSetQpc_ = 0;
      // Die Zeilenzahl kann sich mit der Norm geaendert haben, und dann passt
      // die gemerkte Groesse nicht mehr. Vor dem Neubau, der sie sonst wieder
      // anfordert.
      ReleaseStandardBoundFormat(VideoStandardLines(settled));
      std::string error;
      if (StartCapture(&error)) {
        // The toast speaks the same vocabulary as the picker, so it names the
        // group. The exact variant stays in the log and under the picker.
        Toast(Format(T("Videonorm: %s", "Video standard: %s"),
                     VideoStandardPickerName(settled).c_str()));
      } else {
        Toast(error);
      }
      // Und die Farbmessung von vorn, hinter dem Umbau.
      //
      // Das hat gefehlt, und es war teuer. Die laufende Messung mittelt ueber
      // rund drei Sekunden; wird der Graph mittendrin neu gebaut, steht in ihr
      // die alte Norm, die alte Groesse und der Umbau selbst. Genau das ist am
      // 31.08. um 03:09 passiert: PAL 60 mass unmittelbar nach dem Wechsel von
      // 720x576 auf 720x480 eine Farbe von 0,005 -- gemessen wurde in
      // Wahrheit das Nichts davor. Damit galt die frisch gefundene, richtige
      // Norm als zweifelhaft, und es lief ein voller Rundgang ueber vier
      // Normen los, der zu allem Ueberfluss verworfen wurde, weil die zweite
      // Messung derselben Norm dann 0,258 ergab. Drei Sekunden spaeter stand
      // ohne einen einzigen Normwechsel "PAL 60 hat deutlich Farbe (0,149)"
      // im Log -- die Antwort war die ganze Zeit da, sie wurde nur zu frueh
      // gefragt. Rund sechseinhalb der elfeinhalb Sekunden zwischen
      // Signalverlust und Urteil gingen dafuer drauf.
      //
      // Die Frist darunter ist dieselbe wie im Rundgang und aus demselben
      // Grund da: sie setzt die Messung an ihrem Ende noch einmal zurueck, so
      // dass das Fenster sicher hinter dem Umbau beginnt und nicht davor.
      ResetStandardColourCheck();
      colourSettleUntilQpc_ = now + SecondsToTicks(kColourSettleSeconds);
    }
    VerifyStandardColour(now);
    return;
  }

  // No lock.
  const long available = capture_.capabilities().availableStandards;
  if (standardLostQpc_ == 0) {
    standardLostQpc_ = now;
    if (standardLastGood_ > 0) {
      int planned = 0;
      const std::vector<long> plan =
          AutoStandardCandidates(available, config_.app.videoRegion, standardLastGood_, &planned);
      if (planned > 0) {
        CAP_LOG("Video standard: lost the lock on %s, trying %s first",
                VideoStandardName(VideoStandardIndexOf(standardLastGood_)),
                VideoStandardName(VideoStandardIndexOf(plan.front())));
      }
    }
    return;
  }
  if (TicksToSeconds(now - standardLostQpc_) < kStandardLostSeconds) return;

  // Eine Frist, die auf ein Bild wartet, das nicht kommt, ist kein Zuhoeren.
  //
  // Beim Setzen des Kandidaten wird entschieden, wieviel Zeit er bekommt, und
  // an dem Punkt ist `starved` fast immer noch falsch: das letzte Bild ist
  // erst ein, zwei Sekunden alt, die Schwelle noch nicht erreicht. Die
  // Unterscheidung stand also da, wo sie nichts entscheiden konnte -- am
  // 31.08. um 02:43:39 bekam PAL B seine vollen kStandardPreferredSeconds und
  // lief sie voll aus, obwohl schon beim Setzen seit sieben Sekunden kein Bild
  // mehr angekommen war. 02:43:42,6 "nach 3,00 s ohne Lock verworfen",
  // 02:43:43,2 wieder Bild: die halbe Wartezeit war diese eine Frist.
  //
  // Also wird die Frist nachtraeglich gekuerzt, sobald das Aushungern
  // feststeht. Gekuerzt und nicht gestrichen: der Kandidat braucht seine
  // kStandardSettleSeconds, um seinen Lock ueberhaupt zeigen zu koennen, und
  // dass gerade kein Bild ankommt, sagt darueber nichts -- eine Norm mit der
  // anderen Zeilenzahl liefert grundsaetzlich nichts in einen Graphen der
  // alten Groesse, und trotzdem kann genau sie die richtige sein. Was hier
  // wegfaellt, ist allein die zusaetzliche Geduld der vorgezogenen Plaetze.
  // Die ist fuer eine Konsole gedacht, die noch hochfaehrt, und die liefert
  // dabei Bilder -- nur noch keine stabilen.
  if (starved && standardCandidate_ >= 0 && standardSetQpc_ != 0) {
    const int64_t shortened = standardSetQpc_ + SecondsToTicks(kStandardSettleSeconds);
    if (shortened < standardNextTryQpc_) standardNextTryQpc_ = shortened;
  }
  if (now < standardNextTryQpc_) return;

  int preferred = 0;
  const std::vector<long> candidates =
      AutoStandardCandidates(available, config_.app.videoRegion, standardLastGood_, &preferred);
  if (candidates.empty()) return;
  standardCandidateCount_ = (int)candidates.size();

  // Bei totem Eingang wird nicht weitergeschaltet -- und vor allem gilt nichts
  // als geprueft, was hier gemessen wurde.
  //
  // Das ist ein gemessener Fehler, kein gedachter: am 30.08.2026 wurde der
  // GameCube ausgeschaltet und wieder eingeschaltet, und in dem Fenster ohne
  // Signal lief die Suche weiter. Als das Bild zurueckkam, war PAL B gerade
  // durch, und die Suche stand bei den 525/60-Normen -- sie lief 9,6 s lang
  // vier Kandidaten gegen ein anliegendes 625/50-Signal, die richtige Antwort
  // hatte sie kurz vorher schon in der Hand gehabt und weggeworfen. Eine Norm
  // gegen kein Signal zu pruefen ist keine Pruefung; sie faellt zwangslaeufig
  // durch, und was durchfaellt, wird eine ganze Runde lang nicht wieder gefragt.
  //
  // Also: Karte auf die beste Vermutung parken -- aus demselben Grund wie bei
  // der Pause unten, ein auftauchendes Signal bestaetigt sich sonst auf der
  // zuletzt zufaellig eingestellten Norm selbst -- und die Runde von vorn
  // beginnen lassen, sobald wieder etwas anliegt.
  if (starved && !standardStarvedLogged_) {
    standardStarvedLogged_ = true;
    CAP_LOG("Video standard: no frame for %.1f s -- %s does not match the incoming signal, the "
            "search goes on",
            frameAgeMs >= 0.0 ? frameAgeMs / 1000.0
                              : TicksToSeconds(now - standardStarvedSinceQpc_),
            VideoStandardName(VideoStandardIndexOf(capture_.currentStandard())));
  }
  if (!starved && renderer_.detectedSignal() == VideoRenderer::SignalVerdict::Flat) {
    if (standardCandidate_ >= 0 || capture_.currentStandard() != candidates.front()) {
      bool switched = false;
      if (capture_.currentStandard() != candidates.front()) {
        capture_.SetStandard(candidates.front());
        standardSeqAtSet_ = signalSeq_.load(std::memory_order_acquire);
        ResetStandardColourCheck();
        switched = true;
      }
      CAP_LOG("Video standard: no signal at the input, search paused and parked on %s",
              VideoStandardName(VideoStandardIndexOf(candidates.front())));
      standardCandidate_ = -1;

      // Parken heisst die Karte umstellen, und eine Norm bringt ihre Zeilenzahl
      // mit. Der Graph muss also mit, genau wie beim Einrasten weiter oben --
      // sonst laeuft er auf der alten Groesse weiter, und die Karte schiebt ein
      // 625-Zeilen-Bild in einen 480 Zeilen hohen Graphen.
      //
      // Auch das ist gemessen: am 31.08.2026 wurde der GameCube von 60 auf 50 Hz
      // zurueckgestellt. 01:48:03 wurde auf PAL B geparkt, 01:48:04 kamen 25,03
      // Bilder/s an -- und der Graph stand noch auf 720x480. Neunundvierzig
      // Sekunden lang war das Bild gestaucht, die Kammpruefung urteilte auf
      // einer Groesse, die es nicht gab, und die Aufloesungsliste zeigte die
      // Formate der alten Norm. Erst ein Neustart von Hand hat es geradegezogen.
      //
      // Ohne Signal neu aufzubauen ist dabei kein Nachteil, sondern der Sinn:
      // wenn das Bild wiederkommt, steht der Graph schon richtig. Und oefter
      // als noetig geschieht es nicht -- geparkt wird nur, wenn die Karte noch
      // nicht auf der Vermutung steht, und losgelassen nur, wenn sich die
      // Zeilenzahl wirklich geaendert hat.
      if (switched && ReleaseStandardBoundFormat(VideoStandardLines(candidates.front()))) {
        std::string error;
        if (!StartCapture(&error)) Toast(error);
      }
    }
    // Die Frist des laufenden Kandidaten immer wieder von vorn, damit sie erst
    // zu laufen beginnt, wenn es etwas zu messen gibt.
    standardSetQpc_ = now;
    standardNextTryQpc_ = now + SecondsToTicks(kStandardSettleSeconds);
    return;
  }

  // Eine ganze Runde durch und nichts ist eingerastet: dann liegt vermutlich
  // gar kein Signal an. Pause, dann von vorn.
  //
  // Hier stand `standardSweeps_ >= 1 && standardCandidate_ < 0`, und das konnte
  // nie beides zugleich gelten. Der Index geht ausserhalb der Initialisierung
  // nur in dem Zweig auf -1, in dem der Lock geklappt hat, und der kehrt sofort
  // zurueck; beim allerersten Durchlauf ist er zwar -1, dann sind aber noch
  // null Runden gelaufen. Die Pause trat also nie ein, und die Suche schrieb
  // der Karte fuer immer alle `kStandardSettleSeconds` eine neue Norm.
  //
  // Jetzt wird das Ende einer Runde erkannt, bevor weitergeschaltet wird, und
  // `standardSweeps_` zaehlt wirklich abgeschlossene Runden, so wie es in
  // app.h beschrieben ist.
  if (standardCandidate_ + 1 >= (int)candidates.size()) {
    ++standardSweeps_;
    standardCandidate_ = -1;

    // Der schnelle Durchgang ist durch und hat nichts gefunden. Dann war er
    // entweder zu ungeduldig, oder es liegt wirklich nichts an -- und welches
    // von beidem, sagt genau ein weiterer Durchgang mit der vollen Frist.
    //
    // Ohne Pause dazwischen und ohne Parken. Beides gehoert ans Ende der
    // *Suche* und nicht in ihre Mitte: die Pause ist die Antwort auf "hier ist
    // nichts", und die steht nach einem schnellen Durchgang noch gar nicht
    // fest. Auch die Karte bleibt stehen, wo sie steht -- der naechste
    // Kandidat ist ohnehin wieder der erste der Liste.
    if (!standardPatientPass_) {
      standardPatientPass_ = true;
      standardNextTryQpc_ = now;
      if (standardSweeps_ == 1) {
        CAP_LOG("Video standard: fast pass without a lock (%d standards at %.2f s each), second "
                "pass at %.2f s",
                (int)candidates.size(), kStandardFastSeconds, kStandardSettleSeconds);
      }
      return;
    }

    standardPatientPass_ = false;
    standardNextTryQpc_ = now + SecondsToTicks(kStandardBackoffSeconds);
    // Und dabei nicht stehen lassen, was zuletzt probiert wurde.
    //
    // Waehrend der Pause steht irgendeine Norm auf der Karte, und wenn in
    // dieser Zeit ein Signal auftaucht -- die Konsole wird eingeschaltet,
    // jemand steckt endlich das Kabel an -- dann rastet der Lock darauf ein und
    // oben gilt "was gesetzt ist, stimmt". Gesucht wird dann gar nicht mehr.
    //
    // Das war ein echter Fehler: die Liste endet mit den seltensten Normen, die
    // Runde hinterliess also NTSC 4.43 auf der Karte, und weil der Lock nur
    // waagerecht misst, bestaetigt sich NTSC 4.43 an jeder 525/60-Quelle
    // selbst. Ein PAL-60-GameCube wurde so zuverlaessig als NTSC 4.43 erkannt.
    // Die Pause dauert laenger als eine Runde, das traf also die Mehrzahl der
    // Faelle -- und zwar genau den haeufigsten Ablauf ueberhaupt, naemlich
    // qBlank zuerst starten und die Konsole danach.
    //
    // Der erste Kandidat ist bauartbedingt die beste Vermutung: der Partner der
    // zuletzt eingerasteten Norm, sonst die haeufigste ueberhaupt. Ein Irrtum
    // dieser Art ist damit der wahrscheinlichste statt der unwahrscheinlichste,
    // und wo der erste Kandidat 625/50 ist, kann eine 525/60-Quelle sich gar
    // nicht mehr selbst bestaetigen: der Lock scheitert und es wird richtig
    // gesucht.
    if (capture_.currentStandard() != candidates.front()) {
      capture_.SetStandard(candidates.front());
      standardSeqAtSet_ = signalSeq_.load(std::memory_order_acquire);
    }
    // Nur beim ersten Mal, sonst laeuft das Log voll: dass pausiert wird, ist
    // einmal eine Nachricht und danach der Normalzustand.
    if (standardSweeps_ == 1) {
      CAP_LOG("Video standard: a full round without a lock, search paused (%.0f s), card set to %s",
              kStandardBackoffSeconds,
              VideoStandardName(VideoStandardIndexOf(candidates.front())));
    }
    // Beide Durchgaenge durch und nichts rastet ein: dann ist auch das die
    // Antwort auf den Tastendruck. Die Suche laeuft nach der Pause weiter --
    // was sie dann findet, ist nicht mehr die Antwort auf diese Frage.
    FinishManualStandardSearch(candidates.front(),
                               T("Keine Norm rastet ein — es liegt wohl kein Signal an",
                                 "No standard locks — there is probably no signal"));
    return;
  }

  // Verworfen -- und in der ersten Runde steht im Log, nach wie langer Frist.
  // Genau hier entsteht der Fehler, wenn die Frist zu kurz ist: eine Norm, die
  // nur noch nicht fertig eingefangen hat, sieht genauso aus wie eine falsche.
  // Nur die ersten beiden Runden, sonst schreibt eine Quelle ohne Signal das
  // Log voll -- und die ersten beiden sind es deshalb, weil das jetzt der
  // schnelle und der geduldige Durchgang sind. Genau ihr Vergleich ist die
  // Auskunft: eine Norm, die im schnellen Durchgang durchfaellt und im
  // geduldigen einrastet, sagt, dass kStandardFastSeconds zu knapp bemessen
  // ist. Darum steht auch dabei, welcher Durchgang gerade verwirft.
  if (standardSweeps_ <= 1 && standardCandidate_ >= 0 && standardSetQpc_ != 0) {
    CAP_LOG("Video standard: %s dropped after %.2f s without a lock (%s pass)",
            VideoStandardName(VideoStandardIndexOf(candidates[(size_t)standardCandidate_])),
            TicksToSeconds(now - standardSetQpc_),
            standardPatientPass_ ? "patient" : "fast");
  }

  ++standardCandidate_;

  // Wer uns gerade aushungert, ist schon widerlegt und braucht keine Frist.
  //
  // Nach dem Parken steht die Karte bereits auf candidates.front(), und genau
  // die ist der naechste Kandidat. Ohne diesen Schritt wird sie noch einmal
  // gesetzt -- ein Nichts -- und bekommt dann als vorgezogene Norm ihre vollen
  // kStandardPreferredSeconds, obwohl seit Sekunden kein Bild kommt. Am
  // 31.08. um 02:29 waren das drei geschenkte Sekunden von elf: 02:29:44,7
  // PAL B gesetzt, 02:29:47,6 "nach 3,03 s ohne Lock verworfen", 02:29:48,3
  // Lock auf PAL 60.
  if (starved && standardCandidate_ + 1 < (int)candidates.size() &&
      candidates[(size_t)standardCandidate_] == capture_.currentStandard()) {
    ++standardCandidate_;
  }

  const long next = candidates[(size_t)standardCandidate_];
  capture_.SetStandard(next);
  standardSetQpc_ = now;
  standardSeqAtSet_ = signalSeq_.load(std::memory_order_acquire);
  // Setting it is not the same as it working. Give the decoder a moment, then
  // this function will look at the lock again and either keep it or move on.
  // Den vorgezogenen Kandidaten wird laenger zugehoert, siehe oben.
  //
  // Ausser es kommt gar kein Bild. Die laengere Frist ist fuer eine Konsole
  // gedacht, die noch hochfaehrt -- die liefert dabei Bilder, nur noch keine
  // stabilen. Kommt nichts, ist Warten nur Warten, und der Rueckweg zu einem
  // Bild fuehrt ausschliesslich ueber den naechsten Kandidaten.
  //
  // Steht das Aushungern hier noch nicht fest, faellt es weiter oben nach --
  // die Frist wird dann nachtraeglich gekuerzt statt vorher verweigert.
  //
  // Im schnellen Durchgang bekommt jeder Kandidat dieselbe kurze Frist, auch
  // die vorgezogenen. Die zusaetzliche Geduld der vorderen Plaetze ist fuer
  // eine Quelle gedacht, die noch nicht stabil ist -- und die gewinnt der
  // schnelle Durchgang ohnehin nicht, sie gehoert dem zweiten. Wer hier
  // vorgezogen wird, wird es dadurch, dass er als erster drankommt.
  standardNextTryQpc_ =
      now + SecondsToTicks(!standardPatientPass_ ? kStandardFastSeconds
                         : !starved && standardCandidate_ < preferred
                             ? kStandardPreferredSeconds
                             : kStandardSettleSeconds);
  // Die Norm hat gewechselt, also gehoert die bisherige Farbmessung zu einer
  // anderen Einstellung.
  ResetStandardColourCheck();
}

// Ob die eingerastete Norm auch farblich stimmt.
//
// Der Lock allein kann das nicht sagen, und das ist keine Schwaeche der
// Umsetzung, sondern der Auskunft: er meldet, dass die Zeilenfrequenz gefunden
// wurde. 525/60 zerfaellt aber in fuenf Normen, die sich allein im Farbtraeger
// unterscheiden -- PAL 60 und NTSC 4.43 bei 4,43 MHz, NTSC M, NTSC M (Japan)
// und PAL M bei 3,58 MHz -- und waagerecht sehen die fuenf identisch aus. Steht
// die falsche, rastet die Karte trotzdem ein und meldet Erfolg.
//
// Was dann herauskommt, ist aber nicht unsichtbar: der Burst sitzt auf der
// falschen Frequenz, der Farbkiller des Decoders greift, und das Bild wird grau
// mit etwas Regenbogengries darin. Genau das laesst sich messen, und es ist die
// einzige Auskunft ueber den Farbtraeger, die es ueberhaupt gibt.
//
// Deshalb wird nicht behauptet, sondern verglichen: unter einem Wert, bei dem
// von Farbe keine Rede mehr sein kann, wird die Norm mit dem anderen Traeger
// probiert und nachgemessen. Nur wenn die deutlich farbiger ist, wird
// gewechselt. Bei einer wirklich schwarzweissen Quelle -- einem alten Film,
// einer S/W-Kamera -- sind beide gleich blass, es gewinnt keiner, und es bleibt
// bei dem, was die Region vorgeschlagen hat. Das ist die Rolle der Region an
// dieser Stelle: sie ist der Gleichstandssieger, den eine Messung allein nicht
// hat.
//
// Einmal je Norm. Danach ist die Sache entschieden, und eine Messung, die sich
// jede Minute neu meldet, waere ein Schalter, der von selbst umspringt.
void App::VerifyStandardColour(int64_t now) {
  // Unterhalb davon ist das Bild grau. Bewusst etwas ueber Null: an einem
  // Composite-Eingang rauscht auch ein totgeschalteter Farbkanal noch ein
  // wenig, und ein Verdacht, der am Rauschen scheitert, meldet sich nie.
  //
  // Am 29.08.2026 an einem GameCube im PAL-60-Modus gemessen, je einmal mit der
  // richtigen und der falschen Norm auf derselben Szene:
  //
  //   Szene                     NTSC M (Japan)   PAL 60
  //   Sherbet Land (Schnee)              0,024    0,037
  //   Dry Dry Desert (bunt)              0,013    0,207
  //
  // Der Schwellwert liegt ueber beiden falschen Messungen und unter der
  // richtigen der bunten Szene. Die farbarme Szene ist der enge Fall -- 0,037
  // gegen 0,035 --, und genau fuer den gibt es die Wiederholung weiter unten:
  // dort entscheidet nicht der Schwellwert, sondern die naechste Kurve.
  static const float kChromaSuspect = 0.035f;
  // Und so viel Farbe muss dastehen, damit der erste Durchgang die anliegende
  // Norm ohne jeden Vergleich durchwinkt.
  //
  // kChromaSuspect beantwortet "ist ueberhaupt Farbe da". Das ist die richtige
  // Frage fuer den Vergleich und die falsche fuer die Abkuerzung, und der
  // Unterschied hat heute eine Fehlerkennung gekostet: die Karte stand noch auf
  // PAL N, mass an einem PAL-B-Signal 0,042 -- knapp ueber der Sichtbarkeit --
  // und hatte dazu saubere Tiefen, weil ein falscher Traeger in den Tiefen eben
  // wenig anrichtet, wenn er ueberhaupt wenig anrichtet. Damit war die falsche
  // Norm bestaetigt, bevor irgendetwas verglichen wurde.
  //
  // Eine richtige Dekodierung liegt nicht knapp ueber der Sichtbarkeit, sondern
  // deutlich darueber: dieselbe Szene mass mit PAL B 0,152, mit PAL N 0,017 bis
  // 0,042. Die Schwelle liegt geometrisch dazwischen. Wer sie verfehlt, ist
  // deswegen nicht falsch -- er wird nur verglichen statt geglaubt, und das
  // kostet ein paar Sekunden, keine Fehlerkennung.
  static const float kChromaConfident = 0.080f;
  // So viel farbiger muss der Gewinner sein. Ein knapper Vorsprung ist kein
  // Befund, sondern Rauschen -- und im Zweifel bleibt es bei der Norm, die zur
  // Region passt.
  //
  // Zwei Werte, weil es zwei Faelle sind. Hat der Gewinner sichtbar Farbe, ist
  // die Messung schon von allein aus dem Rauschen heraus und ein deutlicher
  // Vorsprung genuegt. Liegen beide unter der Sichtbarkeitsschwelle, zaehlt
  // allein der Abstand, und der muss dann groesser sein: zwei Rauschwerte
  // derselben Szene liegen dicht beieinander.
  //
  // Hier stand vorher, der Gewinner muesse ausserdem selbst ueber
  // kChromaSuspect liegen -- sonst ergaeben zwei Rauschwerte allein durch ihr
  // Verhaeltnis einen Sieger, den niemand sieht. Der Gedanke stimmt, die
  // Umsetzung war zu grob: sie warf einen Sieg um Faktor 12 (0,024 gegen
  // 0,002) mit einem um Faktor 1,5 in denselben Topf. Genau das trennen die
  // zwei Werte jetzt, und die Messungen vom 29.08. ziehen die Grenze von
  // selbst -- entschieden waren Faktor 12 und 16, unentschieden 1,54.
  // Dazwischen ist viel Platz.
  static const float kChromaBetterBy = 1.6f;
  static const float kChromaBetterByFaint = 3.0f;
  // Und darunter zaehlt gar nichts mehr. Ein Verhaeltnis braucht einen Nenner:
  // misst der Verlierer glatt null, gewinnt jede noch so kleine Zahl mit
  // unendlichem Vorsprung. Der Boden liegt weit unter der kleinsten Messung,
  // die je etwas entschieden hat (0,024), und weit ueber dem Nichts.
  static const float kChromaFloor = 0.005f;
  // Wie oft der Gegenversuch wiederholt wird, wenn er nichts entscheidet.
  //
  // Das ist der Fall, den es wirklich gibt: eine Szene, die auch richtig
  // dekodiert fast grau ist. Am 29.08. an Sherbet Land gemessen -- eine
  // Schneepiste -- kamen 0,024 gegen 0,037 heraus, und daraus laesst sich
  // nichts schliessen. Einmal zu fragen und dann fuer immer zu schweigen waere
  // hier das Schlechteste: das Bild bliebe grau, obwohl die naechste Kurve die
  // Antwort liefert.
  //
  // Also wird spaeter noch einmal gemessen, mit wachsendem Abstand, und dann
  // ist Schluss. Jeder Versuch kostet ein paar Sekunden falsche Farbe, das darf
  // nicht endlos sein -- und eine wirklich schwarzweisse Quelle waere sonst
  // genau das.
  //
  // Der Abstand war einmal 30 s, weil "die naechste Kurve" bei Mario Kart
  // ungefaehr so lange braucht. Das war zu vorsichtig gedacht: solange nichts
  // entschieden ist, laeuft das Bild in der Ausgangsnorm weiter und der
  // Gegenversuch kostet zwei Sekunden. Drei Sekunden, verdoppelt, ergeben
  // 3 + 6 + 12 -- nach gut zwanzig Sekunden ist das Urteil gefaellt.
  //
  // Vorher standen hier 10 + 20 + 40. Das war zu vorsichtig gerechnet: der
  // Wiederholungsfall ist nicht teuer, weil er meistens gar keine Runde ist.
  // Steht ueber die Farbe der eingestellten Norm inzwischen genug fest, kostet
  // er nur den Blick darauf -- so am 31.08. um 02:17:47 und 02:25:41, beide
  // Male "PAL 60 hat deutlich Farbe", ohne einen einzigen Normwechsel. Nur
  // wenn es wieder nicht reicht, laeuft eine volle Runde, und die kostet die
  // vier Sekunden, gegen die die Pause gedacht war.
  static const int kColourRetries = 3;
  static const double kColourRetryBaseSeconds = 3.0;
  // Und was ein verworfener Rundgang wartet, naemlich fast nichts.
  //
  // Die Pause oben ist gegen eine graue Szene gedacht, und dort ist das Warten
  // der Zweck: die Szene soll erst farbig werden. Ein Rundgang, der verworfen
  // wurde, weil sich das Bild waehrenddessen geaendert hat, ist der
  // entgegengesetzte Fall -- gefehlt hat nicht die Farbe, sondern die Ruhe,
  // und ein Rennspiel wird durch Zuwarten nicht ruhiger. Sechs Sekunden
  // spaeter ist die Szene genauso in Bewegung, nur ist die Antwort dann sechs
  // Sekunden aelter.
  //
  // Die Zahl der Anlaeufe bleibt bei kColourRetries, also bleibt auch die
  // sichtbare Stoerung dieselbe: gleich viele Normwechsel, nur ohne die
  // Totzeit dazwischen. Aus 3 + 6 s werden 1 + 1 s.
  static const double kColourMotionRetrySeconds = 1.0;
  // Kommt in dieser Zeit keine Messung zustande, kommt keine. Das Format hat
  // dann keine lesbare Farbe -- siehe VideoRenderer::AnalyzeChroma -- oder es
  // laufen zu wenige Bilder durch. Beides ist kein Fehler, nur ein Nein.
  // Grosszuegig gegen die knapp drei Sekunden, die das Messfenster braucht.
  static const double kColourGiveUpSeconds = 10.0;

  // Um wie viel sauberer das Schwarz des Siegers sein muss.
  //
  // Es gibt hier bewusst *keinen* festen Schwellwert dafuer, ab wann Schwarz
  // als eingefaerbt gilt, und das ist eine Messung, keine Vorsicht. Am
  // 30.08.2026 an einem PAL-GameCube, dieselbe Szene dreimal dekodiert:
  //
  //   Norm                Farbe   dunkle Bereiche
  //   PAL B  (richtig)    0,152   0,101
  //   SECAM B (falsch)    0,461   0,353
  //   PAL N  (falsch)     0,017   0,014
  //
  // Ein fester Wert muesste zwischen 0,101 und 0,353 liegen, und der erste
  // Versuch mit 0,060 warf prompt die *richtige* Norm hinaus. Der Grund ist,
  // dass "dunkel" nicht "schwarz" heisst: unterhalb der Lumaschwelle liegen
  // auch dunkelrote und dunkelblaue Flaechen, die dort mit Recht Farbe haben,
  // und wie viel davon im Bild ist, haengt an der Szene. Ein absoluter Wert
  // misst also mit, was er nicht messen soll.
  //
  // Der Vergleich untereinander tut das nicht: alle drei Messungen sehen
  // dieselbe Szene, der Szenenanteil ist in allen dreien derselbe, und was sie
  // trennt, ist allein der Farbtraeger. 0,353 gegen 0,101 ist Faktor 3,5;
  // gefordert wird 1,6, also gut das Doppelte an Luft.
  static const float kDarkCleanerBy = 1.6f;

  // Um wie viel mehr Farbe der Kandidat mit den schmutzigeren Tiefen haben
  // muss, damit die Tiefenregel darueber nicht mehr entscheidet.
  //
  // Die Messreihe oben hat einen blinden Fleck, und der hat qBlank zwei
  // Rundgaenge lang die falsche Norm eingestellt. Sie besteht ganz aus
  // SECAM B gegen PAL B -- ein falscher Traeger, der Farbe *erfindet*: mehr
  // Farbe als die richtige Norm und dazu eingefaerbte Tiefen. Gegen den ist
  // "das sauberste Schwarz gewinnt" genau richtig.
  //
  // Es gibt den umgekehrten Fehler, und der GameCube zeigt ihn. NTSC 4.43 und
  // PAL 60 teilen sich den Farbtraeger bei 4,43 MHz und unterscheiden sich
  // nur in der zeilenweisen Phasenumkehr. NTSC 4.43 auf einem PAL-60-Signal
  // dekodiert deshalb nicht Unsinn, sondern *weniger*: weniger Farbe im
  // ganzen Bild und damit auch weniger davon im Schwarzen. Am 31.08.2026,
  // zweimal aus NTSC M heraus erzwungen:
  //
  //   Norm                 Farbe   Tiefen
  //   PAL 60  (richtig)    0,159   0,109      0,206   0,134
  //   NTSC 4.43 (falsch)   0,099   0,031      0,116   0,081
  //
  // Beide bestehen die Tinted-Pruefung, beide treten in Stufe eins an, und
  // dort gewinnt zweimal der falsche -- weil er weniger arbeitet. Die
  // Begruendung der Tiefenregel gilt fuer ihn nicht: wer seine Schwebung
  // gleichmaessig ueber alles legt, hat *mehr* Farbe, nicht weniger.
  //
  // Also tritt sie zurueck, sobald der mit den schmutzigeren Tiefen deutlich
  // mehr Farbe hat. Dann ist die Farbe in seinen Tiefen die, die er auch im
  // Rest des Bildes dekodiert, und nicht die eines fremden Traegers.
  //
  // Zuruecktreten kostet dabei fast nichts, und daran haengt die Hoehe des
  // Werts. Die Tiefenregel gibt die Entscheidung nicht an den anderen ab,
  // sondern an Stufe zwei -- und die verlangt ihrerseits kChromaBetterBy.
  // Reicht es dort nicht, stellt die Runde gar nichts ein, bleibt bei der
  // Ausgangsnorm und versucht es spaeter noch einmal. Ein zu frueher Rueckzug
  // kostet also eine Runde, ein zu spaeter eine falsche Norm.
  //
  // Deshalb 1,25 und nicht 1,5. Der erste Versuch stand bei 1,5, gemessen an
  // den 1,61 und 1,78 zweier Rundgaenge -- und liess prompt den naechsten
  // durch: am 31.08. um 15:38:01 stand PAL 60 mit 0,156/0,115 gegen NTSC 4.43
  // mit 0,114/0,067, ein Vorsprung von 1,37, und die Tiefen stellten wieder
  // die falsche Norm ein. Mit 1,25 tritt die Regel dort zurueck, Stufe zwei
  // entscheidet auf diesen Zahlen ebenfalls nichts (1,37 < 1,6), und genau das
  // ist das richtige Ergebnis: es bleibt bei PAL 60.
  //
  // Nach unten haelt die Gegenprobe. Am selben Abend um 15:35:34 hatte
  // NTSC 4.43 mit 0,161 gegen 0,160 einen Vorsprung von 1,006, waehrend die
  // Tiefen mit 0,039 gegen 0,129 klar fuer PAL 60 sprachen. Bei 1,25 kommt die
  // Regel dort nicht zum Zug und die Tiefen entscheiden richtig.
  //
  // Was das kostet: ein falscher Traeger, der es unter kDarkTinted schafft
  // *und* ein Viertel mehr Farbe zeigt, kann jetzt bis in Stufe zwei kommen.
  // In allen Messungen des 30.08. kommt SECAM B nie unter 0,204 und faellt
  // schon vorher aus -- der Fall steht nicht in der Reihe. Und Stufe zwei
  // schliesst eingefaerbte Kandidaten selbst noch einmal aus.
  static const float kDarkYieldsToColourBy = 1.25f;

  // Ab wann die Tiefen eines Kandidaten fuer sich allein als eingefaerbt
  // gelten -- ohne Vergleich, ohne zweite Norm.
  //
  // Der Absatz darueber sagt, ein fester Wert sei untauglich, weil "dunkel"
  // nicht "schwarz" heisst. Das gilt weiter, und trotzdem steht hier einer.
  // Der Grund ist der Versuch mit dem Anteil der Tiefen an der Gesamtfarbe,
  // der genau diesen festen Wert vermeiden sollte und daran gescheitert ist.
  // Alle Messungen an einem PAL-GameCube, 30.08.2026:
  //
  //   Norm                Farbe   Tiefen   Anteil
  //   PAL B  (richtig)    0,152    0,101    0,66
  //   PAL B  (richtig)    0,192    0,110    0,57
  //   PAL B  (richtig)    0,075    0,057    0,76
  //   SECAM B (falsch)    0,461    0,353    0,77
  //   SECAM B (falsch)    0,358    0,335    0,94
  //   SECAM B (falsch)    0,424    0,363    0,86
  //   SECAM B (falsch)    0,421    0,289    0,69
  //   PAL N  (falsch)     0,017    0,014    0,82
  //   PAL N  (falsch)     0,042    0,037    0,88
  //   PAL N  (falsch)     0,017    0,022    1,29
  //
  // Im Anteil ueberlappen richtig (0,57 bis 0,76) und falsch (0,69 bis 1,29).
  // In den Tiefen selbst nicht: richtig bleibt unter 0,110, falsches SECAM
  // faengt bei 0,289 an, Faktor 2,6 dazwischen.
  //
  // Am selben Abend noch einmal, nachdem das Messfenster von 3,2 s auf 0,4 s
  // verkuerzt war (siehe SetChromaCadence), vier Rundgaenge:
  //
  //   PAL B  (richtig)  0,190/0,112  0,259/0,044  0,135/0,053  0,163/0,069
  //   SECAM B (falsch)  0,357/0,334  0,477/0,273  0,438/0,393  0,425/0,204
  //   PAL N  (falsch)   0,042/0,038  0,012/0,011  0,019/0,015  0,018/0,027
  //
  // Die Tiefen des richtigen PAL B bleiben, wo sie waren -- unter 0,112 --,
  // die des falschen SECAM B reichen jetzt bis 0,204 herunter. Der Abstand
  // schrumpft damit von Faktor 2,6 auf 1,8, und der Wert steht nicht mehr in
  // der Mitte, sondern naeher am falschen Rand.
  //
  // Er bleibt trotzdem, wo er ist, weil die beiden Irrtuemer verschieden viel
  // kosten. Zu hoch heisst: ein falscher Kandidat gilt als plausibel und tritt
  // in Stufe eins an -- wo er gegen richtige Tiefen von 0,069 mit 0,204 immer
  // noch um Faktor 3 verliert. Zu niedrig heisst: der *richtige* Kandidat
  // fliegt aus beiden Stufen, der Rundgang entscheidet nichts, und der Nutzer
  // sieht bis zum naechsten Versuch weiter falsche Farben. Der Wert lehnt sich
  // deshalb an die Seite, auf der ein Fehler noch aufgefangen wird.
  //
  // Das Argument fuer den Anteil war, beide Zahlen kaemen aus denselben
  // Bildern, die Szene kuerze sich also heraus. Das stimmt fuer einen
  // *Vergleich zweier Kandidaten* -- und dort wird weiter verglichen, siehe
  // kDarkCleanerBy. Fuer einen festen Wert je Kandidat stimmt es nicht: der
  // Anteil haengt daran, wie viel des Bildes ueberhaupt dunkel ist, und das
  // ist selbst eine Szeneneigenschaft. Auf einer Szene mit wenig dunkler
  // Flaeche faellt der Anteil des falschen Traegers (0,69), auf einer flauen
  // steigt der des richtigen (0,76), und die beiden tauschen die Plaetze.
  //
  // Was den festen Wert hier tragfaehig macht und ihn beim ersten Versuch mit
  // 0,060 zu Fall brachte, ist die Bedingung davor: geprueft wird nur, wer
  // schon kraeftig Farbe hat (kChromaConfident). Wer wenig Farbe zeigt, hat
  // auch wenig davon in den Tiefen und wird gar nicht erst beurteilt.
  //
  // Faellt der Wert einmal falsch, kostet das keine falsche Norm: sind alle
  // Kandidaten eingefaerbt, bleibt die Entscheidung aus und es gilt die
  // Reihenfolge der Wohnregion.
  static const float kDarkTinted = 0.18f;

  // Woran ein NTSC-Dekoder auf einem PAL-Signal zu erkennen ist.
  //
  // Farbmenge und Tiefen trennen PAL 60 und NTSC 4.43 nicht. Beide haben
  // denselben Traeger bei 4,43 MHz, und was sie unterscheidet -- die
  // zeilenweise Umkehr der V-Phase --, geht in beiden Zahlen unter: der
  // NTSC-Dekoder dreht sie nicht zurueck, die Farbe klappt von Zeile zu Zeile
  // um, und im Mittel ueber einen Block hebt sie sich teilweise auf. Deshalb
  // misst die falsche Norm *weniger* Farbe und auch weniger davon im
  // Schwarzen, und deshalb hat sie ueber die Tiefen sogar gewonnen.
  //
  // Also wird das Umklappen selbst gemessen, statt seinen Schatten in den
  // gemittelten Zahlen zu deuten -- siehe VideoRenderer::chromaAltV. Am
  // 31.08.2026, zwoelf Rundgaenge an einem GameCube in PAL 60:
  //
  //   Norm                Zeilenwechsel V     V/U
  //   NTSC 4.43 (falsch)  0,0070 - 0,0515   7,7 - 14,6
  //   PAL 60  (richtig)   0,0002 - 0,0026   0,5 - 2,0
  //   NTSC M              0,0001 - 0,0005   0,4 - 0,8
  //   PAL M               0,0001 - 0,0008   0,1 - 1,0
  //
  // Beide Bedingungen muessen zusammen erfuellt sein, und beide haben Luft.
  // Der Betrag allein wuerde auf einem Bild mit feinem waagerechtem Muster
  // ansprechen -- aber ein solches Muster klappt *beide* Achsen um, und genau
  // das faengt die zweite Bedingung: umgekehrte Phase steht nur auf V.
  // Umgekehrt wuerde das Verhaeltnis allein auf Rauschen ansprechen, wo beide
  // Zahlen nahe null sind; dagegen steht der Betrag.
  //
  // Wer beides erfuellt, ist damit nicht "schlechter", sondern die falsche
  // Familie: ein NTSC-Dekoder auf einem zeilenweise wechselnden Traeger. Das
  // ist ein Ausschlussgrund wie eingefaerbtes Schwarz, kein Nachteil im
  // Vergleich.
  static const float kAltFlipping = 0.005f;
  static const float kAltAxisRatio = 4.0f;

  // Wie viel des Bildes beleuchtet sein muss, damit ein Rundgang ueberhaupt
  // etwas entscheiden kann.
  //
  // Schwarz ist in jeder Norm schwarz. Auf einem fast schwarzen Bild messen
  // alle Kandidaten dieselbe Null, und aus lauter gleichen Zahlen laesst sich
  // keine Norm auswaehlen -- der Rundgang laeuft, schaltet sichtbar durch drei
  // falsche Normen und kommt mit nichts zurueck.
  //
  // Genau so geschehen am 31.08. um 05:30:03. Der Wertebereichsmesser hatte
  // eine Sekunde vorher "95,135 % unter 16" notiert, das Bild war bis auf einen
  // hellen Rest schwarz. Gemessen wurde dann PAL 60 mit 0,005, NTSC M mit
  // 0,002, PAL M mit 0,003, NTSC 4.43 mit 0,003. Zweieinhalb Sekunden
  // Durchschalten fuer vier Mal nichts; als das Bild zurueck war, stand PAL 60
  // bei 0,194 und die Sache war in einer Messung erledigt.
  //
  // Der vorhandene Waechter greift hier nicht und soll es auch nicht: er fragt
  // SignalVerdict::Flat, also ob ueberhaupt etwas anliegt, und es lag etwas an
  // -- die Spanne war 154. "Es ist etwas zu sehen" und "es ist genug zu sehen,
  // um Farbe daran zu messen" sind zwei Fragen.
  //
  // Ein Zehntel, und die Wahl ist bewusst weit weg von dem, was ein Spielbild
  // trifft: gemeint ist nicht "dunkle Szene", sondern "praktisch nichts da".
  //
  // Nachgemessen am 31.08. um 05:43, mit kuenstlich hochgesetztem
  // kChromaConfident, damit ein Rundgang auf laufendem Bild erzwungen wird:
  // **54 % beleuchtet**. Das Fuenffache der Schwelle -- auf gewoehnlichem
  // Spielinhalt kann sie nicht danebengreifen. Der Fall, um den es geht, lag
  // mit "95 % unter Luma 16" auf der anderen Seite.
  //
  // Und falsch herum kostet sie nichts: eingefaerbte Tiefen laufen unten an ihr
  // vorbei, und wer wartet, verbraucht keinen Anlauf.
  static const float kChromaLitWanted = 0.10f;

  auto darkText = [&](float d) {
    return d < 0.0f ? std::string("no dark areas") : Format("%.3f", d);
  };

  // Waehrend eines Vergleichs wird dicht abgetastet, sonst duenn.
  //
  // Hier oben, vor jedem Ruecksprung, damit der dichte Takt nicht in den
  // Normalbetrieb durchsickern kann: colourCandidates_ *ist* der Suchzustand,
  // und wer ihn leert, stellt damit auch den Takt zurueck. Die Bedingung wird
  // je Bild neu gestellt, ausgefuehrt wird nur der Wechsel.
  //
  // Und ebenso, sobald ein Suchlauf von Hand ansteht. Das ist die teuerste
  // einzelne Wartezeit im ganzen Tastendruck, und sie war unsichtbar, weil sie
  // vor dem ersten Normwechsel liegt: der Rundgang kann erst beginnen, wenn
  // die Ausgangsnorm gemessen ist, und diese erste Messung lief noch im duennen
  // Takt. Zehn Bilder bei jedem achten und 29,97 Bildern je Sekunde sind
  // 2,67 s, in denen nichts geschieht und nichts zu sehen ist.
  //
  // Gemessen am 31.08. um 11:26:52,430 (Taste) bis 11:26:55,093 (Beginn des
  // Rundgangs): 2,66 s, gefolgt von 3,34 s Rundgang -- die Vorbereitung kostete
  // vier Zehntel des Ganzen. Drei Laeufe vorher, 11:21 und 11:23, lagen bei
  // 2,66 / 2,68 / 2,67 s: es ist eine Konstante und keine Schwankung.
  //
  // Der dichte Takt macht daraus drei Bilder in Folge, also ein Zehntel
  // Sekunde -- wie viele es sind, haengt am Takt selbst, siehe
  // kChromaFramesWantedDense. Bezahlt wird er mit derselben Rechenarbeit, die
  // der Rundgang ohnehin gleich verlangt, nur ein paar Sekunden frueher. Wer
  // von Hand sucht, bekommt den Rundgang ohnehin: die Abkuerzung ist fuer ihn
  // abgeschaltet, siehe `forced` weiter unten.
  const bool denseWanted = !colourCandidates_.empty() ||
                           (standardForceColourUntilQpc_ != 0 && now < standardForceColourUntilQpc_);
  renderer_.SetChromaCadence(denseWanted ? 1 : 8);

  const long current = capture_.currentStandard();
  if (current == 0) return;

  // Component und RGB haben keinen Farbtraeger, also auch nichts zu vergleichen.
  //
  // Die ganze Runde hier fragt eine einzige Frage: welcher Farbtraeger passt zu
  // dem, was anliegt. Sie stellt vier Normen ein und schaut, unter welcher das
  // Bild Farbe bekommt. Auf getrennten Leitungen kommt die Farbe aber gar nicht
  // aus einem Traeger, sondern liegt schon fertig da -- jede der vier Normen
  // zeigt dieselbe Farbe, und aus vier gleichen Zahlen laesst sich nichts
  // waehlen. Der Rundgang schaltete also sichtbar durch drei falsche Normen und
  // kaeme mit nichts zurueck. Derselbe Ausgang wie bei kChromaLitWanted weiter
  // oben, nur folgt er hier aus der Bauart und nicht aus dem Bildinhalt.
  //
  // Was bleibt, ist Stufe eins: 50 oder 60 Hz, 576 oder 480 Zeilen. Das
  // entscheidet der Lock, und der arbeitet auf Component genauso.
  //
  // Die anliegende Norm gilt damit als geprueft, sonst fragt jede Kurve von
  // vorn. Und der Tastendruck bekommt seine Antwort hier, weil er sie sonst
  // nirgends mehr bekaeme: die vier Ausgaenge unten sind alle hinter diesem
  // Ruecksprung.
  if (colourCandidates_.empty() && !ConnectorHasColourCarrier()) {
    if (colourCheckedStandard_ != current) {
      CAP_LOG("Video standard: no colour round -- %s carries no colour subcarrier",
              AnalogConnectorName((int)ResolvedConnector()));
    }
    colourCheckedStandard_ = current;
    standardForceColourUntilQpc_ = 0;
    FinishManualStandardSearch(
        current, T("Dieser Eingang führt keinen Farbträger — der Lock entscheidet allein",
                   "This input carries no colour subcarrier — the lock decides on its own"));
    return;
  }

  const bool walking = !colourCandidates_.empty();
  if (!walking && current == colourCheckedStandard_) return;
  // Ein unentschiedener Versuch wartet, bevor er sich wiederholt.
  if (colourRetryQpc_ != 0 && now < colourRetryQpc_) return;
  colourRetryQpc_ = 0;

  // Bei totem Eingang wird nicht gemessen. Ein Eingang ohne Signal ist grau,
  // und grau ist hier die Aussage "der Farbtraeger stimmt nicht" -- die Messung
  // saehe also nicht etwa nichts, sie saehe zuverlaessig das Falsche. Dasselbe
  // Argument wie beim Weiterschalten der Normensuche, siehe oben.
  if (renderer_.detectedSignal() == VideoRenderer::SignalVerdict::Flat) {
    renderer_.ResetChroma();
    colourStartedQpc_ = 0;
    return;
  }

  // Frisch gewechselt: die ersten Bilder gehoeren noch der alten Einstellung.
  //
  // Zurueckgesetzt wird genau einmal, naemlich wenn die Frist abgelaufen ist.
  // Hier stand vorher ein Ruecksetzer je Bild, solange gewartet wird, und der
  // sah richtig aus und war es nicht: diese Funktion laeuft nach einem
  // Normwechsel eine knappe halbe Sekunde ueberhaupt nicht: die Suche wartet
  // auf zwei frische Messwerte des Wachtthreads, siehe UpdateVideoStandard.
  // Faellt das Ende der Frist in dieses Loch, ist der erste Aufruf danach
  // schon zu spaet -- es hat nie jemand zurueckgesetzt, und gemessen wird ab
  // dem Wechsel statt ab dem Fristende, mitsamt dem Umschaltmoment der Karte.
  //
  // Am 30.08. mit Frist null nachgestellt, weil dort dasselbe Loch immer
  // klafft: PAL N mass 0,047 und 0,064 statt 0,012 bis 0,021, SECAM B in den
  // Tiefen 0,246 statt 0,271 bis 0,410 -- jeder Kandidat zum Nachbarn hin
  // verschmiert. Mit einer Frist von 0,5 s traf es nur die Laeufe, in denen
  // das Loch etwas laenger war als die Frist, und das war nicht zu sehen.
  //
  // Ein Ruecksetzer am Fristende macht den Anfang des Messfensters unabhaengig
  // davon, wann der naechste Aufruf kommt, und kostet nichts: das Fenster lag
  // ohnehin dahinter.
  if (colourSettleUntilQpc_ != 0) {
    if (now < colourSettleUntilQpc_) return;
    colourSettleUntilQpc_ = 0;
    colourStartedQpc_ = now;
    // Hier faengt der Kandidat an, also hat er noch kein voriges Fenster.
    colourWindowEnergy_ = -1.0f;
    colourWindowDark_ = -1.0f;
    renderer_.ResetChroma();
    return;
  }
  if (colourStartedQpc_ == 0) {
    colourStartedQpc_ = now;
    colourWindowEnergy_ = -1.0f;
    colourWindowDark_ = -1.0f;
  }

  const float energy = renderer_.chromaEnergy();
  if (energy < 0.0f) {
    if (TicksToSeconds(now - colourStartedQpc_) <= kColourGiveUpSeconds) return;
    // Keine Messung zustande gekommen. Ohne laufenden Rundgang ist die Sache
    // damit erledigt -- es gibt nichts zu vergleichen. Im Rundgang zaehlt es
    // als "weiss nicht", wird als solches eingetragen und der naechste
    // Kandidat ist dran.
    if (!walking) {
      colourCheckedStandard_ = current;
      colourStartedQpc_ = 0;
      return;
    }
  }
  const float dark = energy < 0.0f ? -1.0f : renderer_.darkChromaEnergy();

  // Im Rundgang gilt eine Messung erst, wenn sie nicht mehr steigt.
  //
  // Das ersetzt das blosse Abwarten einer Frist, und der Grund dafuer ist am
  // 31.08. gemessen worden. Mit 0,15 s Frist lieferten drei der vier Normen
  // ihren Wert auf den Tausendstel genau wie mit 0,5 s -- NTSC M 0,017,
  // PAL M 0,023, NTSC 4.43 0,065, dreimal hintereinander --, die vierte nicht:
  // PAL 60 mass bei der Rueckkehr 0,000 / 0,047 / 0,095 statt 0,119, und zwar
  // umso hoeher, je spaeter das Fenster lag (0,276 / 0,327 / 0,393 s nach dem
  // Wechsel). Das ist keine Streuung, das ist eine Einschwingkurve, und sie
  // hat einen Grund: der PAL-Burst wechselt zeilenweise die Phase, und seine
  // Kennung braucht mehr Zeilen als NTSCs feste Phase.
  //
  // Eine feste Frist muss deshalb immer die langsamste Norm bezahlen, und alle
  // anderen zahlen mit. Zweimal von drei Laeufen verwarf der Rundgang sich
  // daraufhin selbst, weil die zu frueh gemessene Rueckkehr nicht mehr zur
  // ersten Messung passte -- also nicht ein bisschen ungenauer, sondern
  // unbrauchbar.
  //
  // Der Dekoder sagt aber selbst, wann er so weit ist: solange er einrastet,
  // steigt die Messung, danach steht sie. Gefragt wird deshalb nicht "ist die
  // Frist um", sondern "ist der neue Wert noch hoeher als der vorige" -- und
  // gerichtet, nicht als blosse Aehnlichkeit. Ein bewegtes Bild schwankt in
  // beide Richtungen und ist beim ersten nicht gestiegenen Fenster fertig; ein
  // einrastender Dekoder kann das nicht, er kommt nur von unten.
  //
  // Zwei Fenster braucht es dadurch immer, auch bei der schnellsten Norm. Das
  // ist der Preis und er ist der Sache angemessen: ein einzelnes Fenster hat
  // nichts, woran es sich pruefen koennte.
  if (walking && energy >= 0.0f) {
    const bool rising = colourWindowEnergy_ < 0.0f ||
                        ColourStillRising(colourWindowEnergy_, energy) ||
                        ColourStillRising(colourWindowDark_, dark);
    const double waited = TicksToSeconds(now - colourStartedQpc_);
    if (rising && waited < kColourStableGiveUpSeconds) {
      colourWindowEnergy_ = energy;
      colourWindowDark_ = dark;
      renderer_.ResetChroma();
      return;
    }
    if (rising) {
      // Nicht zur Ruhe gekommen. Der zuletzt gemessene Wert ist trotzdem der
      // beste, den es gibt -- er liegt am weitesten hinter dem Wechsel --,
      // also zaehlt er. Aber er gehoert ins Log, denn er heisst entweder, dass
      // diese Karte laenger braucht als die Obergrenze, oder dass sich das
      // Bild waehrenddessen bewegt hat.
      CAP_LOG("Video standard: %s does not settle within %.2f s (%.3f, before that %.3f) -- the "
              "last value counts",
              VideoStandardName(VideoStandardIndexOf(current)), waited, energy,
              colourWindowEnergy_);
    }
  }

  if (!walking) {
    // Erster Durchgang, und hier werden zwei Fragen gestellt statt einer: hat
    // das Bild Farbe, und bleibt sein Schwarz schwarz?
    //
    // Die zweite fehlte, und das war ein echter Fehler mit sichtbarer Folge.
    // Am 30.08.2026 rastete ein PAL-Signal auf SECAM ein, und SECAM auf PAL
    // liefert kein graues Bild, sondern ein kraeftig eingefaerbtes -- der
    // Nutzer sah einen leuchtend roten Hintergrund, wo Schwarz sein sollte.
    // Der Test "hat Farbe" ging glatt durch, die Fehldekodierung galt als
    // bestaetigt, und danach wurde sie nie wieder in Frage gestellt.
    // Der Schwellwert hier ist ein *Verdacht*, kein Urteil. Er entscheidet nur,
    // ob ueberhaupt verglichen wird; welche Norm gewinnt, entscheiden danach
    // die Kandidaten untereinander. Ein zu hoher Wert kostet eine
    // Fehldekodierung, ein zu niedriger ein paar Sekunden Vergleich. Es ist
    // dieselbe Frage wie beim Vergleich -- sind diese Tiefen eingefaerbt --
    // und deshalb derselbe Wert; die Messreihe steht bei kDarkTinted.
    //
    // Ausser jemand hat die Suche von Hand ausgeloest. Dann ist der Rundgang
    // der Zweck des Tastendrucks und nicht das Mittel gegen einen Verdacht:
    // wer ihn drueckt, sieht etwas, das keine dieser beiden Zahlen misst --
    // einen Farbstich, Gesichter in der falschen Farbe --, und die Abkuerzung
    // waere hier die eine Antwort, die er schon hat.
    const bool forced =
        standardForceColourUntilQpc_ != 0 && now < standardForceColourUntilQpc_;
    if (!forced && energy >= kChromaConfident && dark < kDarkTinted) {
      CAP_LOG("Video standard: %s has clear colour (%.3f), dark areas neutral (%s)",
              VideoStandardName(VideoStandardIndexOf(current)), energy, darkText(dark).c_str());
      colourCheckedStandard_ = current;
      colourStartedQpc_ = 0;
      colourAttempts_ = 0;
      colourWaitingForPicture_ = false;
      // Hierher kommt ein Suchlauf von Hand nur, wenn seine Frist abgelaufen
      // ist, bevor ein Bild zum Vergleichen da war -- und dann ist das hier
      // die Antwort: gemessen wurde, es sprach nichts dagegen.
      FinishManualStandardSearch(current, T("Die Farbe stimmt — es bleibt dabei",
                                            "The colour checks out — no change"));
      return;
    }

    // Zweifel ja -- aber ist genug Bild da, um ihn auszuraeumen?
    //
    // Der Zweifel hat zwei ganz verschiedene Gruende, und nur einer davon ist
    // eine Frage, die ein Rundgang beantworten kann. Zeigt ein *helles* Bild
    // keine Farbe, dann hat der Farbtraeger nicht gestimmt, und welcher es
    // stattdessen ist, entscheiden die Kandidaten untereinander. Zeigt ein
    // schwarzes Bild keine Farbe, dann ist es schwarz. Da ist nichts zu
    // entscheiden, und die vier gleichen Nullen von oben sind die Antwort
    // darauf.
    //
    // Eingefaerbte Tiefen fuehren *nicht* hierher, sondern in den Rundgang, und
    // zwar auch auf einem dunklen Bild: eine Farbe, die im Schwarzen steht, ist
    // ein Beleg fuer einen falschen Traeger und kein fehlender Messwert. Sie
    // ist auf einem dunklen Bild sogar am deutlichsten -- dort sind die
    // Bloecke, um die es geht.
    //
    // Gewartet wird, ohne etwas zu verbrauchen: kein Anlauf wird gezaehlt,
    // keine Norm als geprueft vermerkt. Ein Rundgang, der nie stattgefunden
    // hat, darf weder als unentschieden zaehlen noch die drei Anlaeufe
    // aufbrauchen, bevor das Bild ueberhaupt da ist -- eine Konsole, die
    // hochfaehrt, ist ein paar Sekunden lang schwarz, und danach soll die
    // Pruefung noch alle Anlaeufe haben. Zurueckgesetzt wird dabei wie beim
    // toten Eingang, damit die erste Messung nach dem Warten nicht durch das
    // Schwarze davor verduennt wird.
    const float lit = renderer_.chromaLitFraction();
    if (lit >= 0.0f && lit < kChromaLitWanted && dark >= 0.0f && dark < kDarkTinted) {
      if (!colourWaitingForPicture_) {
        colourWaitingForPicture_ = true;
        CAP_LOG("Video standard: %s is doubtful (colour %.3f), but only %.0f %% of the picture is "
                "lit -- no standard can be told on black, waiting",
                VideoStandardName(VideoStandardIndexOf(current)), energy, lit * 100.0f);
      }
      renderer_.ResetChroma();
      colourStartedQpc_ = 0;
      // Gewartet wird weiter -- aber nicht mehr im Namen des Tastendrucks.
      // Dessen Frist ist abgelaufen, und eine Einblendung, die eine halbe
      // Minute lang "es wird gewartet" sagt und dann verstummt, ist genau die
      // Antwort, die hier gefehlt hat.
      if (!forced) {
        FinishManualStandardSearch(
            current, T("Das Bild blieb zu dunkel für einen Vergleich — es bleibt dabei",
                       "The picture stayed too dark to compare — no change"));
      }
      return;
    }
    colourWaitingForPicture_ = false;

    colourCandidates_ = VideoStandardColourCandidates(
        current, capture_.capabilities().availableStandards, config_.app.videoRegion);
    // Der Rundgang beginnt bei der jetzigen Norm, die ja gerade gemessen wurde.
    // Steht sie nicht vorn, hat die Karte etwas gemeldet, das sie laut eigener
    // Auskunft gar nicht kann -- dann lieber nichts tun als raten.
    if (colourCandidates_.size() < 2 || colourCandidates_.front() != current) {
      colourCandidates_.clear();
      colourCheckedStandard_ = current;
      colourStartedQpc_ = 0;
      standardForceColourUntilQpc_ = 0;
      FinishManualStandardSearch(
          current, Format(T("Keine andere Norm mit %d Zeilen — es bleibt dabei",
                            "No other standard with %d lines — no change"),
                          VideoStandardLines(current)));
      return;
    }
    const int count = (int)colourCandidates_.size();
    // Die Ausgangsnorm kommt am Ende ein zweites Mal dran.
    //
    // Der Vergleich unterstellt, alle Kandidaten saehen dieselbe Szene. Am
    // 30.08. um 15:24 Uhr, als jede Messung noch gut drei Sekunden brauchte
    // und der ganze Rundgang vierzehn, dieselbe Norm im Abstand von zwanzig
    // Sekunden: PAL B 0,181/0,217 und PAL B 0,191/0,107. Die Tiefen
    // halbierten sich, ohne dass sich am Signal etwas geaendert haette.
    //
    // Der Rundgang dauert seither knapp drei Sekunden (siehe
    // SetChromaCadence), und damit ist der Grund kleiner geworden, aber nicht
    // weg -- und er hat einen zweiten bekommen: die erste Messung der
    // Ausgangsnorm ist die mitlaufende aus dem Normalbetrieb, ueber drei
    // Sekunden gemittelt, die der Herausforderer sind kurze Aufnahmen. Die
    // Wiederholung stellt die Ausgangsnorm auf dieselbe Grundlage wie die
    // anderen.
    //
    // Der Schaden daraus ist einseitig. Ueber alle Messungen des 30.08.
    // streuen die Tiefen des falschen SECAM B um Faktor zwei (0,204 bis
    // 0,410), die des richtigen PAL B um Faktor fuenf (0,041 bis 0,217): eine
    // Schwebung aus dem falschen Traeger liegt gleichmaessig ueber jedem Bild
    // und haengt nur wenig an der Szene, echte Farbe in dunklen Flaechen
    // dagegen ganz und gar. Eine flaue Szene laesst also vor allem den
    // *richtigen* Kandidaten schlecht aussehen.
    //
    // Deshalb wird die Ausgangsnorm zweimal gemessen und tritt mit der
    // guenstigeren der beiden Messungen an. Das ist mit Absicht ungleich
    // verteilt: sie ist der Kandidat, der schon laeuft, und ein Wechsel weg
    // von einer richtigen Norm ist der teure Fehler.
    colourCandidates_.push_back(colourCandidates_.front());
    colourEnergies_.assign(colourCandidates_.size(), -1.0f);
    colourDarks_.assign(colourCandidates_.size(), -1.0f);
    colourAltV_.assign(colourCandidates_.size(), -1.0f);
    colourAltU_.assign(colourCandidates_.size(), -1.0f);
    colourIndex_ = 0;
    // Der beleuchtete Anteil steht mit in der Zeile, obwohl er die Runde nicht
    // ausloest. Er ist die Grundlage, auf der sie ueberhaupt etwas entscheiden
    // kann, und wenn ein Rundgang spaeter einmal unerklaerlich lauter Nullen
    // misst, steht die Erklaerung schon in der Zeile davor.
    // Und woran es lag, fuer die Einblendung festgehalten. Die beiden Gruende
    // sind nicht dasselbe und sehen auch nicht gleich aus: zu blass heisst ein
    // graues Bild mit Regenbogengries, eingefaerbt heisst ein Bild mit
    // kraeftig falschen Farben bis ins Schwarze hinein. Wer davorsitzt, sieht
    // genau einen der beiden Faelle und erkennt seinen wieder.
    colourDoubt_ = forced                ? ColourDoubt::Manual
                   : dark >= kDarkTinted ? ColourDoubt::Tinted
                                         : ColourDoubt::Pale;
    standardForceColourUntilQpc_ = 0;
    CAP_LOG("Video standard: %s is doubtful (colour %.3f, dark areas %s, %.0f %% lit) -- "
            "comparing the %d standards with %d lines%s",
            VideoStandardName(VideoStandardIndexOf(current)), energy, darkText(dark).c_str(),
            lit < 0.0f ? 0.0f : lit * 100.0f, count, VideoStandardLines(current),
            colourDoubt_ == ColourDoubt::Manual ? " (started by hand)" : "");
  }

  // Eintragen, was dieser Kandidat gemessen hat, und zum naechsten.
  colourEnergies_[(size_t)colourIndex_] = energy;
  colourDarks_[(size_t)colourIndex_] = dark;
  colourAltV_[(size_t)colourIndex_] = renderer_.chromaAltV();
  colourAltU_[(size_t)colourIndex_] = renderer_.chromaAltU();
  CAP_LOG("Video standard: %s measured -- colour %s, dark areas %s, line alternation V %.4f U %.4f",
          VideoStandardName(VideoStandardIndexOf(current)),
          energy < 0.0f ? "no measurement" : Format("%.3f", energy).c_str(),
          darkText(dark).c_str(), colourAltV_[(size_t)colourIndex_],
          colourAltU_[(size_t)colourIndex_]);

  ++colourIndex_;
  if (colourIndex_ < (int)colourCandidates_.size()) {
    capture_.SetStandard(colourCandidates_[(size_t)colourIndex_]);
    standardSeqAtSet_ = signalSeq_.load(std::memory_order_acquire);
    colourSettleUntilQpc_ = now + SecondsToTicks(kColourSettleSeconds);
    colourStartedQpc_ = 0;
    renderer_.ResetChroma();
    return;
  }

  // Alle durch, die Ausgangsnorm zweimal. Von ihren beiden Messungen zaehlt
  // die mit den saubereren Tiefen, und zwar als Paar: die Farbmenge kommt aus
  // derselben Messung wie die Tiefen, sonst stuenden Zahlen aus zwei Szenen
  // nebeneinander. Die Begruendung steht oben beim zweiten Anlauf.
  // Um wie viel die beiden Messungen derselben Norm auseinanderliegen duerfen,
  // bevor die Runde als ungueltig gilt. Siehe sceneChanged gleich darunter.
  static const float kSceneAgreeBy = 2.0f;
  bool sceneChanged = false;

  if (colourCandidates_.size() > 1 && colourCandidates_.back() == colourCandidates_.front()) {
    const size_t last = colourCandidates_.size() - 1;
    // Guenstiger heisst: so, wie die Runde unten urteilt -- und dort ist
    // "keine dunklen Stellen" kein schlechter Tiefenwert, sondern gar keiner.
    //
    // Die erste Fassung las guenstig als "sauberere Tiefen" und stellte -1
    // dabei ganz nach hinten. Das ging am 31.08.2026 zweimal schief, und zwar
    // in beide Richtungen. Um 15:45:07 mass PAL 60 zuerst 0,084/0,121 und dann
    // 0,155 ohne eine einzige dunkle Stelle; die zweite wurde verworfen, PAL 60
    // trat mit 0,084 an und verlor gegen NTSC 4.43 mit 0,099. Um 15:46:12 war
    // es umgekehrt: zuerst 0,217 ohne dunkle Stellen, dann 0,205/0,295 -- die
    // zweite zaehlte, 0,295 gilt als eingefaerbt, und damit war die richtige
    // Norm aus beiden Stufen draussen.
    //
    // Also der Reihe nach: nicht eingefaerbt schlaegt eingefaerbt. Sind beide
    // gleich weit, entscheiden die Tiefen -- aber nur, wenn es welche gibt.
    // Fehlen sie in einer der beiden Messungen, gibt es nichts zu vergleichen,
    // und dann zaehlt die Farbmenge. Genau die soll die Wiederholung ja
    // retten.
    const bool tinted0 = colourDarks_[0] >= kDarkTinted;
    const bool tintedLast = colourDarks_[last] >= kDarkTinted;
    const bool better =
        tinted0 != tintedLast
            ? !tintedLast
            : (colourDarks_[0] < 0.0f || colourDarks_[last] < 0.0f
                   ? colourEnergies_[last] > colourEnergies_[0]
                   : colourDarks_[last] < colourDarks_[0]);

    // Und hier wird die Zweitmessung das, wofuer sie eigentlich da ist: die
    // Probe darauf, ob die Runde ueberhaupt eine Szene gesehen hat.
    //
    // Die Kandidaten werden nacheinander gemessen, eine ganze Runde dauert
    // ein paar Sekunden, und das Bild wartet nicht. Aendert sich die Szene
    // dazwischen -- ein Bootbildschirm wird zum Spiel --, dann sahen die
    // frueheren Kandidaten etwas anderes als die spaeteren, und der Vergleich
    // vergleicht nichts. Wer zufaellig drankam, als es bunt wurde, gewinnt.
    //
    // Genau das ist am 31.08.2026 um 02:02 passiert. Die Runde ueber die fuenf
    // 525-Zeilen-Normen fing auf einem dunklen Bild an und endete auf einem
    // farbigen: PAL 60 zuerst 0.005, am Ende 0.122, und dazwischen bekam
    // NTSC 4.43 mit 0.157 den Zuschlag -- gegen einen GameCube, der PAL 60
    // ausgibt. Danach stand die Karte auf einer Norm, die das Signal nicht
    // dekodieren kann.
    //
    // Gemessen wird die Abweichung an den beiden Schwellen, die in dieser
    // Runde ueberhaupt etwas entscheiden: kChromaConfident trennt "beurteilt"
    // von "unbeurteilt", kDarkTinted "plausibel" von "eingefaerbt". Springt
    // eine der beiden Messungen ueber eine dieser Schwellen, hat sich der
    // Massstab selbst bewegt. Der Faktor daneben faengt die Faelle, die
    // innerhalb einer Schwelle bleiben und trotzdem eine andere Szene sind.
    const float e0 = colourEnergies_[0], e1 = colourEnergies_[last];
    const float d0 = colourDarks_[0], d1 = colourDarks_[last];
    if (e0 >= 0.0f && e1 >= 0.0f) {
      if ((e0 < kChromaConfident) != (e1 < kChromaConfident)) sceneChanged = true;
      const float lo = e0 < e1 ? e0 : e1, hi = e0 < e1 ? e1 : e0;
      if (hi > lo * kSceneAgreeBy && hi >= kChromaConfident) sceneChanged = true;
    }
    if (d0 >= 0.0f && d1 >= 0.0f && (d0 < kDarkTinted) != (d1 < kDarkTinted)) sceneChanged = true;

    CAP_LOG("Video standard: %s measured a second time -- colour %s, dark areas %s (first %s and "
            "%s)%s",
            VideoStandardName(VideoStandardIndexOf(colourCandidates_.front())),
            colourEnergies_[last] < 0.0f ? "no measurement"
                                         : Format("%.3f", colourEnergies_[last]).c_str(),
            darkText(colourDarks_[last]).c_str(),
            colourEnergies_[0] < 0.0f ? "no measurement" : Format("%.3f", colourEnergies_[0]).c_str(),
            darkText(colourDarks_[0]).c_str(),
            sceneChanged ? " -- the scene changed during the round, the comparison "
                           "does not count"
                         : (better ? " -- the second one counts" : ""));
    if (better) {
      colourEnergies_[0] = colourEnergies_[last];
      colourDarks_[0] = colourDarks_[last];
    }
    // Der Zeilenwechsel folgt dieser Wahl *nicht*, sondern nimmt den groesseren
    // der beiden Werte. Farbmenge und Tiefen haengen an der Szene, und deshalb
    // ist es dort eine Frage, welche der beiden Messungen die aussagekraeftige
    // ist. Das Umklappen haengt an der Norm: ein Dekoder, der die Phasenumkehr
    // richtig aufhebt, kann sie nicht in einer zweiten Messung ploetzlich
    // zeigen. Rauschen und flaue Szenen druecken den Wert nur nach unten. Wer
    // ihn also in einer der beiden Messungen hat, hat ihn.
    if (colourAltV_[last] > colourAltV_[0]) {
      colourAltV_[0] = colourAltV_[last];
      colourAltU_[0] = colourAltU_[last];
    }
    colourCandidates_.pop_back();
    colourEnergies_.pop_back();
    colourDarks_.pop_back();
    colourAltV_.pop_back();
    colourAltU_.pop_back();
  }

  // Erst aussortieren, dann vergleichen -- und die Reihenfolge ist der ganze
  // Punkt.
  //
  // "Wer hat am meisten Farbe" waere das Naheliegende und ist falsch. Ein
  // falscher Farbtraeger toetet die Farbe nicht nur, er kann sie auch
  // erfinden -- und zwar gerade dann, wenn die richtige Norm nichts anzeigt.
  // Am 30.08. um 14:57 Uhr, an einem fast grauen Bild: PAL B (richtig) 0,006,
  // SECAM B (falsch) 0,424. Nach Farbmenge gewaenne die Fehldekodierung um
  // Faktor siebzig.
  //
  // Was die beiden trennt, ist nicht die Menge, sondern der Ort. Ein richtig
  // dekodiertes Bild hat farbige Mitten und neutrale Tiefen -- Schwarz ist
  // schwarz, weil dort nichts zu modulieren ist. Ein falscher Traeger legt
  // seine Schwebung gleichmaessig ueber alles, Mitten wie Tiefen.
  //
  // Daraus wird ein Test, den ein einzelner Kandidat fuer sich besteht oder
  // nicht: wie viel Farbe in den dunklen Stellen steht. Die Messreihe dazu und
  // der Grund, warum es der absolute Wert ist und nicht sein Anteil an der
  // Gesamtfarbe, stehen bei kDarkTinted.
  //
  // Der Test greift nur, wo er etwas messen kann: unterhalb kChromaConfident
  // sind die Tiefen ein Rauschwert und sagen nichts, und ohne dunkle Stellen
  // im Bild gibt es sie gar nicht. In beiden Faellen gilt der Kandidat als
  // unbeurteilt -- nicht als bestaetigt und nicht als widerlegt.
  //
  // Der zweite Ausschlussgrund braucht die Tiefen gar nicht: die Farbe, die
  // von Zeile zu Zeile umklappt. Er faengt den Fall, den der erste
  // strukturell nicht sehen kann -- zwei Normen auf demselben Traeger, die
  // sich nur in der Phasenlage unterscheiden. Der steht bei kAltFlipping.
  //
  // Beide sind Ausschlussgruende und keine Nachteile im Vergleich: wer einen
  // von ihnen erfuellt, ist nicht schlechter dekodiert, sondern falsch.
  enum class Verdict { Unjudged, Plausible, Tinted, Flipping };
  std::vector<Verdict> verdicts(colourCandidates_.size(), Verdict::Unjudged);
  for (size_t i = 0; i < colourCandidates_.size(); ++i) {
    if (colourAltV_[i] >= kAltFlipping && colourAltV_[i] > colourAltU_[i] * kAltAxisRatio) {
      verdicts[i] = Verdict::Flipping;
      CAP_LOG("Video standard: %s flips the colour from line to line (V %.4f, U %.4f) -- the "
              "decoder does not undo the phase alternation, the standard is out",
              VideoStandardName(VideoStandardIndexOf(colourCandidates_[i])), colourAltV_[i],
              colourAltU_[i]);
      continue;
    }
    if (colourEnergies_[i] < kChromaConfident || colourDarks_[i] < 0.0f) continue;
    verdicts[i] = colourDarks_[i] < kDarkTinted ? Verdict::Plausible : Verdict::Tinted;
  }
  const auto ruledOut = [&](size_t i) {
    return verdicts[i] == Verdict::Tinted || verdicts[i] == Verdict::Flipping;
  };

  int best = -1, runnerUp = -1;
  bool byDarks = false;

  // Stufe eins: unter den plausiblen gewinnt das sauberste Schwarz.
  //
  // Hier ist der absolute Vergleich richtig, denn jetzt stehen sich nur noch
  // Kandidaten gegenueber, die denselben Test bestanden haben und dieselbe
  // Szene sehen. Steht einer allein da, ist das ein Befund und kein Zufall:
  // er hat kraeftige Farbe und dazu neutrale Tiefen, und keiner der anderen
  // hat das.
  int dark1 = -1, dark2 = -1;
  for (size_t i = 0; i < colourCandidates_.size(); ++i) {
    if (verdicts[i] != Verdict::Plausible) continue;
    if (dark1 < 0 || colourDarks_[i] < colourDarks_[(size_t)dark1]) {
      dark2 = dark1;
      dark1 = (int)i;
    } else if (dark2 < 0 || colourDarks_[i] < colourDarks_[(size_t)dark2]) {
      dark2 = (int)i;
    }
  }
  // Sauberes Schwarz gewinnt aber nicht gegen deutlich mehr Farbe.
  //
  // Verglichen wird mit dem farbigsten Kandidaten und nicht mit dem
  // zweitsaubersten: welcher die zweitsaubersten Tiefen hat, sagt nichts
  // darueber, wer am meisten dekodiert, und bei mehr als zwei Kandidaten sind
  // das nicht dieselben. Die Begruendung steht bei kDarkYieldsToColourBy.
  //
  // Mitgezaehlt wird dabei auch, wer *unbeurteilt* geblieben ist -- nur die
  // eingefaerbten sind draussen. Unbeurteilt heisst nicht widerlegt, und die
  // Frage hier ist nicht "wer hat den Tiefentest bestanden", sondern "gibt es
  // jemanden, der deutlich mehr dekodiert". Am 31.08.2026 um 15:38:48 hing
  // genau daran die falsche Norm: PAL 60 stand mit Farbe 0,136 da, hatte in
  // dieser Szene aber keinen einzigen dunklen Block und damit keine Tiefen,
  // fiel deshalb aus Stufe eins heraus -- und NTSC 4.43 gewann mit 0,081 als
  // vermeintlich "einzige kraeftig farbige mit neutralen Tiefen". Sie war
  // nicht die einzige, sie war nur die einzige mit dunklen Stellen im Bild.
  int colour1 = -1;
  for (size_t i = 0; i < colourCandidates_.size(); ++i) {
    if (ruledOut(i) || colourEnergies_[i] < 0.0f) continue;
    if (colour1 < 0 || colourEnergies_[i] > colourEnergies_[(size_t)colour1]) colour1 = (int)i;
  }
  const bool darksYield =
      dark1 >= 0 && colour1 >= 0 && colour1 != dark1 &&
      colourEnergies_[(size_t)colour1] > colourEnergies_[(size_t)dark1] * kDarkYieldsToColourBy;
  if (darksYield) {
    CAP_LOG("Video standard: %s has the cleaner shadows (%s against %s), but %s has %.1f times "
            "the colour (%.3f against %.3f) -- the amount of colour decides",
            VideoStandardName(VideoStandardIndexOf(colourCandidates_[(size_t)dark1])),
            darkText(colourDarks_[(size_t)dark1]).c_str(),
            darkText(colourDarks_[(size_t)colour1]).c_str(),
            VideoStandardName(VideoStandardIndexOf(colourCandidates_[(size_t)colour1])),
            colourEnergies_[(size_t)colour1] / colourEnergies_[(size_t)dark1],
            colourEnergies_[(size_t)colour1], colourEnergies_[(size_t)dark1]);
  }

  if (dark1 >= 0 && !darksYield &&
      (dark2 < 0 || colourDarks_[(size_t)dark2] >
                        colourDarks_[(size_t)dark1] * kDarkCleanerBy)) {
    best = dark1;
    runnerUp = dark2;
    byDarks = true;
  }

  // Stufe zwei: geben die Tiefen nichts her -- weil niemand kraeftig genug
  // Farbe hatte oder weil zwei gleich sauber sind --, bleibt es beim alten
  // Verfahren, wer am meisten Farbe hat. Fuer eine wirklich schwarzweisse
  // Quelle ist das nach wie vor die einzige sinnvolle Frage.
  //
  // Wer den Test aber *nicht* bestanden hat, ist hier raus und nicht bloss
  // hinten. Genau daran haengt der graue Fall von 14:57 Uhr: SECAM B haette
  // ihn mit Faktor siebzig gewonnen. Ein eingefaerbtes Schwarz ist ein
  // Ausschlussgrund, kein Nachteil.
  if (best < 0) {
    for (size_t i = 0; i < colourCandidates_.size(); ++i) {
      if (colourEnergies_[i] < 0.0f || ruledOut(i)) continue;
      if (best < 0 || colourEnergies_[i] > colourEnergies_[(size_t)best]) {
        runnerUp = best;
        best = (int)i;
      } else if (runnerUp < 0 || colourEnergies_[i] > colourEnergies_[(size_t)runnerUp]) {
        runnerUp = (int)i;
      }
    }
  }

  const long origin = colourCandidates_.front();
  const float winner = best >= 0 ? colourEnergies_[(size_t)best] : -1.0f;
  const float second = runnerUp >= 0 ? colourEnergies_[(size_t)runnerUp] : -1.0f;
  const float needed = winner >= kChromaSuspect ? kChromaBetterBy : kChromaBetterByFaint;
  // Ueber die Tiefen entscheidet auch ein einzelner Kandidat, denn dort hat er
  // etwas bestanden. Ueber die blosse Farbmenge dagegen ist ein einzelner nur
  // eine gelungene Messung ohne Vergleich, und die entscheidet nichts.
  const bool alone = best >= 0 && runnerUp < 0;
  // sceneChanged sticht alles: hat die Runde zwei Szenen gesehen, sind die
  // Zahlen nicht falsch, sie gehoeren nur nicht zusammen. Dann entscheidet
  // hier nichts, und der Weg unten -- zurueck zum Ausgangspunkt, spaeter noch
  // einmal -- ist derselbe wie bei einer zu farbarmen Szene.
  const bool decided =
      !sceneChanged &&
      (byDarks || (best >= 0 && !alone && winner >= kChromaFloor && winner > second * needed));

  const long chosen = best >= 0 ? colourCandidates_[(size_t)best] : origin;
  const long runnerUpStandard = runnerUp >= 0 ? colourCandidates_[(size_t)runnerUp] : 0;
  const float winnerDark = best >= 0 ? colourDarks_[(size_t)best] : -1.0f;
  const float secondDark = runnerUp >= 0 ? colourDarks_[(size_t)runnerUp] : -1.0f;
  const float originEnergy = colourEnergies_.front();
  const float originDark = colourDarks_.front();

  colourCandidates_.clear();
  colourEnergies_.clear();
  colourDarks_.clear();
  colourAltV_.clear();
  colourAltU_.clear();
  colourIndex_ = 0;
  colourStartedQpc_ = 0;

  // Die Karte steht jetzt auf dem zuletzt gemessenen Kandidaten. In jedem Fall
  // muss sie da weg -- entweder auf den Sieger oder zurueck auf den Anfang.
  const long target = decided ? chosen : origin;
  if (current != target) {
    capture_.SetStandard(target);
    standardSeqAtSet_ = signalSeq_.load(std::memory_order_acquire);
    renderer_.ResetChroma();
  }

  if (decided) {
    if (alone) {
      CAP_LOG("Video standard: %s is the only one strongly coloured with neutral shadows (colour "
              "%.3f, shadows %s) -- set",
              VideoStandardName(VideoStandardIndexOf(chosen)), winner, darkText(winnerDark).c_str());
    } else if (byDarks) {
      CAP_LOG("Video standard: %s has more neutral shadows than %s (%s against %s, both coloured) "
              "-- set",
              VideoStandardName(VideoStandardIndexOf(chosen)),
              VideoStandardName(VideoStandardIndexOf(runnerUpStandard)),
              darkText(winnerDark).c_str(), darkText(secondDark).c_str());
    } else {
      CAP_LOG("Video standard: %s (colour %.3f) has more colour than %s (%.3f) -- set",
              VideoStandardName(VideoStandardIndexOf(chosen)), winner,
              VideoStandardName(VideoStandardIndexOf(runnerUpStandard)), second);
    }
    colourCheckedStandard_ = chosen;
    standardLastGood_ = chosen;
    colourAttempts_ = 0;
    // Wer gefragt hat, bekommt die Antwort dort, wo er die Frage gestellt hat.
    // Der Toast bleibt der Automatik: er ist die Nachricht ueber etwas, das
    // von selbst geschehen ist, und beides zugleich zu zeigen, hiesse
    // dieselbe Sache zweimal an zwei Stellen zu sagen.
    const bool answered = standardManualSearch_;
    FinishManualStandardSearch(
        chosen, chosen != origin
                    ? Format(T("Nach Farbe berichtigt — vorher %s", "Corrected by colour — was %s"),
                             VideoStandardName(VideoStandardIndexOf(origin)))
                    : std::string(T("Die Farbe bestätigt sie — es bleibt dabei",
                                    "The colour confirms it — no change")));
    if (!answered && chosen != origin) {
      Toast(Format(T("Videonorm nach Farbe berichtigt: %s", "Video standard corrected by colour: %s"),
                   VideoStandardPickerName(chosen).c_str()));
    }
    return;
  }

  // Der Vergleich ist hin -- die Ausgangsnorm ist es deshalb nicht.
  //
  // "Verworfen" heisst: die Kandidaten haben verschiedene Szenen gesehen, also
  // sagt ihr Verhaeltnis zueinander nichts. Ueber die Ausgangsnorm allein sagt
  // das nichts aus. Ihre Messung ist eine vollstaendige Messung einer
  // einzelnen Norm, und fuer die gibt es oben laengst ein Urteil: kraeftig
  // Farbe und neutrale Tiefen heisst richtig dekodiert. Es ist woertlich
  // dieselbe Pruefung wie die im Normalbetrieb, nur spaeter im Ablauf -- also
  // wird sie hier gestellt statt eine Wiederholung dafuer zu bezahlen.
  //
  // Am 31.08.2026 um 05:24 hat genau das drei Sekunden gekostet. Die Runde
  // wurde um 39.908 verworfen, und in derselben Zeile stand PAL 60 mit Farbe
  // 0,194 und Tiefen 0,107 -- beides klar innerhalb der Schwellen. Der
  // Wiederholer stellte um 42.573 fest, was schon dagestanden hatte:
  // "PAL 60 hat deutlich Farbe (0.192), dunkle Bereiche neutral (0.107)".
  //
  // Neu ist daran kein Massstab. Damit eine falsche Norm hier durchkaeme,
  // muesste sie kraeftig Farbe *und* neutrale Tiefen zeigen, und das ist die
  // Beschreibung einer richtigen -- ein falsch dekodiertes SECAM lag in den
  // Tiefen bei 0,353 gegen eine Schwelle von 0,18.
  if (sceneChanged && originEnergy >= kChromaConfident && originDark >= 0.0f &&
      originDark < kDarkTinted) {
    CAP_LOG("Video standard: comparison discarded, but %s stands on its own (colour %.3f, dark "
            "areas %s) -- no change",
            VideoStandardName(VideoStandardIndexOf(origin)), originEnergy,
            darkText(originDark).c_str());
    colourCheckedStandard_ = origin;
    standardLastGood_ = origin;
    colourAttempts_ = 0;
    FinishManualStandardSearch(
        origin, T("Der Vergleich war unbrauchbar, die Farbe stimmt für sich — es bleibt dabei",
                  "The comparison was unusable, but the colour stands on its own — no change"));
    return;
  }

  // Kein klarer Sieger. Zurueck zum Ausgangspunkt -- und nicht abgehakt,
  // sondern spaeter noch einmal, denn eine graue Szene sagt nichts ueber den
  // Farbtraeger. Erst nach einigen Anlaeufen ist die Quelle wohl wirklich
  // schwarzweiss.
  ++colourAttempts_;
  if (colourAttempts_ >= kColourRetries) {
    // Auch hier zaehlt der Grund. "Die Quelle ist schwarzweiss" ist eine
    // Aussage ueber das Signal, und die darf nicht fallen, wenn gar nicht die
    // Farbe gefehlt hat, sondern die Ruhe. Aufgegeben wird trotzdem: nach drei
    // Anlaeufen ueber gut siebzig Sekunden bewegt sich das Bild eben staendig,
    // und ohne verlaesslichen Vergleich bleibt der Ausgangspunkt das Beste,
    // was wir haben.
    if (sceneChanged) {
      CAP_LOG("Video standard: the picture moved in all %d attempts -- no reliable comparison "
              "possible, staying on %s",
              colourAttempts_, VideoStandardName(VideoStandardIndexOf(origin)));
    } else {
      CAP_LOG("Video standard: nothing decides after %d attempts -- the source is probably black "
              "and white, staying on %s (colour %.3f, dark areas %s)",
              colourAttempts_, VideoStandardName(VideoStandardIndexOf(origin)), originEnergy,
              darkText(originDark).c_str());
    }
    colourCheckedStandard_ = origin;
    FinishManualStandardSearch(
        origin, sceneChanged
                    ? T("Das Bild war jedesmal in Bewegung — kein verlässlicher Vergleich",
                        "The picture moved every time — no reliable comparison")
                    : T("Nichts entscheidet — die Quelle ist wohl schwarzweiß",
                        "Nothing decides — the source is probably black and white"));
    return;
  }
  const double wait = sceneChanged ? kColourMotionRetrySeconds
                                   : kColourRetryBaseSeconds * (double)(1 << (colourAttempts_ - 1));
  CAP_LOG("Video standard: comparison %s -- again in %.0f s, %s until then",
          sceneChanged ? "discarded, the picture changed meanwhile"
                       : "undecided, the scene has too little colour",
          wait, VideoStandardName(VideoStandardIndexOf(origin)));
  colourRetryQpc_ = now + SecondsToTicks(wait);
  // Auch das ist eine Antwort, und zwar die letzte, die der Tastendruck noch
  // bekommt. Der Wiederholer laeuft weiter -- aber er laeuft in wachsenden
  // Abstaenden bis zu gut einer Minute, und so lange auf eine Einblendung zu
  // warten, die vielleicht nie kommt, ist keine Auskunft. Was der Wiederholer
  // spaeter entscheidet, meldet wieder der Toast.
  FinishManualStandardSearch(
      origin, Format(sceneChanged
                         ? T("Das Bild hat sich während des Vergleichs geändert — in %.0f s noch "
                             "einmal",
                             "The picture changed during the comparison — trying again in %.0f s")
                         : T("Die Szene ist zu farbarm — in %.0f s noch einmal",
                             "The scene has too little colour — trying again in %.0f s"),
                     wait));
}

void App::ResetStandardColourCheck() {
  colourCheckedStandard_ = 0;
  colourCandidates_.clear();
  colourEnergies_.clear();
  colourDarks_.clear();
  colourAltV_.clear();
  colourAltU_.clear();
  colourIndex_ = 0;
  colourSettleUntilQpc_ = 0;
  colourStartedQpc_ = 0;
  colourRetryQpc_ = 0;
  colourAttempts_ = 0;
  colourWaitingForPicture_ = false;
  colourDoubt_ = ColourDoubt::None;
  // Der Wunsch nach einem Rundgang von Hand bleibt dagegen stehen. Ein
  // Graphenumbau setzt hier alles zurueck, und genau einer laeuft haeufig
  // gerade dann, wenn die Taste gedrueckt wird -- der Wunsch waere weg, bevor
  // er einmal drankam. Seine eigene Frist beendet ihn.
  //
  // Auch der Takt zurueck: der Abbruch kann von aussen kommen -- Signal weg,
  // Graph neu -- und dann laeuft VerifyStandardColour nicht mehr, das den Takt
  // sonst selbst zuruecknimmt.
  renderer_.SetChromaCadence(8);
  renderer_.ResetChroma();
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
  if (standardSweeps_ >= 1 && !standardPatientPass_ &&
      signalLocked_.load(std::memory_order_relaxed) == 0) {
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
    const int locked = signalLocked_.load(std::memory_order_relaxed);
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
      settings_.Draw(capture_.running() ? &capture_.capabilities() : nullptr, &ffmpeg_);
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
  // Not over the crash notice: opened at the same level, it would replace it.
  if (updatePromptQueued_ && !ImGui::IsPopupOpen("###crash_notice")) {
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

void App::DetectCrop() {
  int left = 0, top = 0, right = 0, bottom = 0;
  if (!renderer_.contentBounds(&left, &top, &right, &bottom)) {
    Toast(T("Noch nichts gemessen. Einen Moment warten.",
            "Nothing measured yet. Give it a moment."));
    return;
  }
  const VideoFormatInfo format = renderer_.sourceFormat();
  if (!format.valid()) return;

  // Wieviel vom Bild ueberhaupt uebrig bliebe -- und das ist die Frage, die vor
  // dem Zuschneiden zu stellen ist.
  //
  // Die Messung sucht die Grenzen dessen, was nicht schwarz ist, und kann
  // zwischen einem Rand und einer dunklen Stelle nicht unterscheiden. Zeigt
  // eine Konsole gerade nur ihr Startlogo auf Schwarz, ist das gemessene
  // Rechteck das Logo, und alles darum wuerde weggeschnitten: Bildflaeche, die
  // in diesem Moment nur nicht beleuchtet ist.
  //
  // Gemessen wird die Flaeche, nicht die einzelne Kante, und das ist der
  // Unterschied, auf den es ankommt: ein echter Rand frisst eine Richtung, ein
  // Logo auf Schwarz frisst beide.
  //
  // Der breiteste Rand, der noch einer ist, ist ein Kinoformat in 4:3 --
  // 2,35:1 laesst 57 Prozent der Hoehe und damit auch 57 Prozent der Flaeche
  // stehen. Ein 4:3-Bild in einem 16:9-Signal laesst 75 Prozent der Breite.
  // Der Startbildschirm des GameCube dagegen, an einem 720x576-Signal
  // nachgerechnet: 45 Prozent der Breite, 73 Prozent der Hoehe, zusammen 33
  // Prozent der Flaeche -- ueber die Kanten allein waeren das nur fuenf Punkte
  // Abstand zur Haelfte, ueber die Flaeche sind es siebzehn.
  //
  // Die zweite Schranke ist nur gegen den entarteten Fall: ein schmaler
  // Streifen kann die halbe Flaeche halten und trotzdem kein Rand sein.
  const int keptW = right - left + 1;
  const int keptH = bottom - top + 1;
  const double partW = format.width > 0 ? (double)keptW / format.width : 1.0;
  const double partH = format.height > 0 ? (double)keptH / format.height : 1.0;
  if (partW * partH < 0.5 || partW < 0.4 || partH < 0.4) {
    char text[240];
    std::snprintf(text, sizeof(text),
                  T("Da blieben nur %.0f Prozent des Bildes stehen (%.0f x %.0f). Das sieht "
                    "nach einem Logo auf Schwarz aus, nicht nach einem Rand -- erst ein "
                    "richtiges Bild der Konsole abwarten.",
                    "That would leave only %.0f per cent of the picture (%.0f x %.0f). It "
                    "looks like a logo on black rather than a border -- wait for a real "
                    "picture from the console first."),
                  partW * partH * 100.0, partW * 100.0, partH * 100.0);
    Toast(text);
    CAP_LOG("Crop discarded: only %dx%d of %dx%d left (%.0f %% of the area)", keptW, keptH,
            format.width, format.height, partW * partH * 100.0);
    return;
  }

  ImageSettings& img = config_.active().image;
  const int cl = left;
  const int ct = top;
  const int cr = format.width - 1 - right;
  const int cb = format.height - 1 - bottom;
  // A border of a pixel or two is measurement noise on an analogue input, not a
  // border, and cropping it would only cost resolution.
  const int floorPx = 3;
  img.cropLeft = cl >= floorPx ? cl : 0;
  img.cropTop = ct >= floorPx ? ct : 0;
  img.cropRight = cr >= floorPx ? cr : 0;
  img.cropBottom = cb >= floorPx ? cb : 0;

  if (img.cropLeft || img.cropRight || img.cropTop || img.cropBottom) {
    char text[160];
    std::snprintf(text, sizeof(text),
                  T("Rand erkannt: links %d, rechts %d, oben %d, unten %d",
                    "Border found: left %d, right %d, top %d, bottom %d"),
                  img.cropLeft, img.cropRight, img.cropTop, img.cropBottom);
    Toast(text);
  } else {
    Toast(T("Kein schwarzer Rand gefunden.", "No black border found."));
  }
}

// Ein Zuschnitt gilt fuer die Groesse, an der er gemessen wurde.
//
// Die vier Zahlen sind Bildpunkte der Quelle, nicht Anteile: "links 20" heisst
// zwanzig Punkte von 720. Wechselt die Norm von 525 auf 625 Zeilen, wechselt
// mit ihr die Bildhoehe, und "oben 17" beschreibt dann einen anderen Streifen
// als den gemessenen. Im besten Fall steht ein schmaler schwarzer Rand wieder
// im Bild, im schlechteren wird echter Bildinhalt weggeschnitten -- und beides
// sieht nicht nach einer Einstellung aus, die noch von vorhin steht, sondern
// nach einem kaputten Bild.
//
// Zurueckgesetzt statt neu gemessen, und das ist eine Entscheidung. Ein
// automatischer zweiter Anlauf laege nahe -- die Norm hat gerade gewechselt,
// gleich einmal nachmessen --, aber genau in diesem Moment ist das Bild am
// wenigsten dazu geeignet: der Graph ist eben erst wieder aufgebaut, die
// Konsole schaltet gerade um oder faehrt hoch, und was anliegt, ist ein
// Startlogo auf Schwarz oder noch gar nichts. Die Messung hat gegen genau
// diesen Fall bereits eine Schranke (siehe DetectCrop), aber sie noch dazu
// ungefragt in ihn hineinzuschicken hiesse, sie gegen ihre eigene Schranke
// laufen zu lassen. Also: sauber aufraeumen, es sagen, und die Entscheidung
// dem ueberlassen, der das Bild sieht -- der Knopf dafuer liegt jetzt im
// Rechtsklickmenue.
//
// Wer will, kann es sich stattdessen merken lassen (`cropPerFormat`). Ein
// Profil ist die Beschreibung einer Quelle, und eine Quelle kann zwei Groessen
// haben: derselbe GameCube liefert 576 Zeilen im PAL-Modus und 480 im
// 60-Hz-Modus, und beide Male haengt ein anderer schwarzer Rand daran. Das sind
// nicht zwei Quellen, also sollen es nicht zwei Profile sein muessen. Gemessen
// wird weiterhin von Hand -- gespeichert wird nur, was gemessen wurde, und zwar
// unter der Groesse, bei der es gemessen wurde.
// Den Zuschnitt, wie er gerade steht, unter einer Bildgroesse ablegen.
//
// Ein Zuschnitt aus lauter Nullen ist kein Zuschnitt, sondern seine Abwesenheit
// -- und die ist auch das, was ohne Eintrag geschieht. Er wird deshalb nicht
// gespeichert, sondern loescht einen vorhandenen Eintrag: sonst fuellt sich die
// Liste mit Groessen, unter denen nichts steht, und die Zeile in den
// Einstellungen, die sie aufzaehlt, zaehlt Nichts auf.
static void StoreCropVariant(ImageSettings& img, int w, int h) {
  if (w <= 0 || h <= 0) return;
  const bool empty = !img.cropLeft && !img.cropRight && !img.cropTop && !img.cropBottom;
  for (auto it = img.cropVariants.begin(); it != img.cropVariants.end(); ++it) {
    if (it->width != w || it->height != h) continue;
    if (empty) {
      img.cropVariants.erase(it);
      return;
    }
    it->left = img.cropLeft;
    it->right = img.cropRight;
    it->top = img.cropTop;
    it->bottom = img.cropBottom;
    return;
  }
  if (empty) return;
  CropForFormat v;
  v.width = w;
  v.height = h;
  v.left = img.cropLeft;
  v.right = img.cropRight;
  v.top = img.cropTop;
  v.bottom = img.cropBottom;
  img.cropVariants.push_back(v);
}

static const CropForFormat* FindCropVariant(const ImageSettings& img, int w, int h) {
  for (const CropForFormat& v : img.cropVariants) {
    if (v.width == w && v.height == h) return &v;
  }
  return nullptr;
}

void App::UpdateCropForFormat() {
  const VideoFormatInfo fmt = renderer_.sourceFormat();
  if (!fmt.valid()) return;
  // Waehrend des Ziehens sind die Zahlen ohnehin auf null gesetzt und das
  // Format zu merken waere verfrueht.
  if (cropPick_.active) return;

  ImageSettings& img = config_.active().image;
  const int w = fmt.width;
  const int h = fmt.height;

  if (cropFormatWidth_ == w && cropFormatHeight_ == h) {
    // Nichts gewechselt -- aber vielleicht wurde am Zuschnitt geschraubt.
    //
    // Jeden Weg dorthin einzeln zu benachrichtigen hiesse, vier Regler, den
    // Rahmen zum Ziehen, die Messung und den Menuepunkt zum Zuruecksetzen an
    // dieselbe Buchhaltung zu haengen und beim naechsten Weg daran zu denken.
    // Hier steht ohnehin jedes Bild ein Vergleich an; er kostet vier Zahlen.
    if (img.cropPerFormat) StoreCropVariant(img, w, h);
    return;
  }

  // Das erste Format einer Sitzung hat nichts geaendert; es ist das, wofuer
  // die gespeicherten Zahlen gelten sollen -- es sei denn, fuer genau diese
  // Groesse steht etwas Eigenes in der Liste. Dann ist das die juengere
  // Auskunft: die vier Zahlen oben gehoeren zu der Groesse, bei der zuletzt
  // aufgehoert wurde, und das muss nicht die sein, mit der es weitergeht.
  const bool first = cropFormatWidth_ == 0 && cropFormatHeight_ == 0;
  const int wasW = cropFormatWidth_;
  const int wasH = cropFormatHeight_;
  cropFormatWidth_ = w;
  cropFormatHeight_ = h;

  if (img.cropPerFormat) {
    if (!first) StoreCropVariant(img, wasW, wasH);
    const CropForFormat* v = FindCropVariant(img, w, h);
    if (v) {
      const bool same = img.cropLeft == v->left && img.cropRight == v->right &&
                        img.cropTop == v->top && img.cropBottom == v->bottom;
      img.cropLeft = v->left;
      img.cropRight = v->right;
      img.cropTop = v->top;
      img.cropBottom = v->bottom;
      if (first || same) return;
      CAP_LOG("Crop for %dx%d applied (left %d, right %d, top %d, bottom %d)", w, h,
              v->left, v->right, v->top, v->bottom);
      Toast(Format(T("Videoformat geändert (%dx%d) — gespeicherter Zuschnitt eingesetzt.",
                     "Video format changed (%dx%d) — stored crop applied."),
                   w, h));
      return;
    }
    if (first) return;
  } else if (first) {
    return;
  }

  if (!img.cropLeft && !img.cropRight && !img.cropTop && !img.cropBottom) return;

  CAP_LOG("Crop reset: source now %dx%d (border was left %d, right %d, top %d, bottom %d)",
          w, h, img.cropLeft, img.cropRight, img.cropTop, img.cropBottom);
  img.cropLeft = 0;
  img.cropRight = 0;
  img.cropTop = 0;
  img.cropBottom = 0;
  Toast(Format(T("Videoformat geändert (%dx%d) — Zuschnitt zurückgesetzt.",
                 "Video format changed (%dx%d) — crop reset."),
               w, h));
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
  const long standard = colourCheckedStandard_;
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
  if (cropPick_.active) return;

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
  if (signalLocked_.load(std::memory_order_relaxed) != 1) return 0;
  const SettingsWindow::StandardSearch search = StandardSearchDisplay();
  if (search != SettingsWindow::StandardSearch::Off &&
      search != SettingsWindow::StandardSearch::Result) {
    return 0;
  }
  const int lines = VideoStandardLines(signalStandard_.load(std::memory_order_relaxed));
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

void App::BeginCropPick() {
  if (cropPick_.active) return;
  const VideoFormatInfo format = renderer_.sourceFormat();
  if (!format.valid() || !renderer_.hasFrame()) {
    Toast(T("Kein Bild zum Zuschneiden.", "No picture to crop."));
    return;
  }

  ImageSettings& img = config_.active().image;
  cropPick_.saved = img;
  cropPick_.left = img.cropLeft;
  cropPick_.right = img.cropRight;
  cropPick_.top = img.cropTop;
  cropPick_.bottom = img.cropBottom;
  cropPick_.drag = -1;
  cropPick_.active = true;

  // Show the full frame underneath, so screen position maps straight to source
  // pixels and the handles start where the current crop is.
  img.cropLeft = img.cropRight = img.cropTop = img.cropBottom = 0;
  settings_.Close();
}

void App::EndCropPick(bool apply) {
  if (!cropPick_.active) return;
  ImageSettings& img = config_.active().image;
  img = cropPick_.saved;
  if (apply) {
    img.cropLeft = cropPick_.left;
    img.cropRight = cropPick_.right;
    img.cropTop = cropPick_.top;
    img.cropBottom = cropPick_.bottom;
    CAP_LOG("Crop set: left %d, right %d, top %d, bottom %d", img.cropLeft,
            img.cropRight, img.cropTop, img.cropBottom);
  }
  cropPick_.active = false;
  OpenSettings({});
}

void App::DrawCropPicker() {
  const VideoFormatInfo format = renderer_.sourceFormat();
  const Rect& r = renderer_.videoRect();
  const float rw = (float)(r.right - r.left);
  const float rh = (float)(r.bottom - r.top);
  if (!format.valid() || rw < 8.0f || rh < 8.0f) {
    EndCropPick(false);
    return;
  }

  const float srcW = (float)format.width;
  const float srcH = (float)format.height;
  const float scaleX = rw / srcW;
  const float scaleY = rh / srcH;

  // Source pixels <-> client pixels.
  auto toScreenX = [&](int src) { return (float)r.left + (float)src * scaleX; };
  auto toScreenY = [&](int src) { return (float)r.top + (float)src * scaleY; };
  auto toSrcX = [&](float screen) { return (int)std::lround((screen - (float)r.left) / scaleX); };
  auto toSrcY = [&](float screen) { return (int)std::lround((screen - (float)r.top) / scaleY); };

  // Hintergrundliste, nicht Vordergrundliste: ImGui zeichnet die Vordergrundliste
  // nach allen Fenstern, die Hintergrundliste davor. Beide liegen ueber dem
  // Video, denn das Bild kommt gar nicht aus ImGui -- es steht schon im
  // Rueckpuffer, bevor hier irgendetwas gezeichnet wird.
  //
  // Im Vordergrund lag die Abdunklung ueber der eigenen Werkzeugleiste: zieht
  // man eine Kante ueber sie hinweg, waechst das abgedunkelte Feld darueber und
  // Übernehmen und Abbrechen werden unlesbar -- genau in dem Moment, in dem man
  // sie braucht. Dieselbe Ordnung, die der Ruhebildschirm in overlay.cpp
  // benutzt, und aus demselben Grund.
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  const ImVec2 mouse = ImGui::GetMousePos();

  float xL = toScreenX(cropPick_.left);
  float xR = toScreenX(format.width - cropPick_.right);
  float yT = toScreenY(cropPick_.top);
  float yB = toScreenY(format.height - cropPick_.bottom);

  // ---- grab handling ----
  // Everything outside the picture is ignored, so dragging the window or using
  // the buttons above still works.
  const float grab = 10.0f;
  const bool overVideo = mouse.x >= r.left - grab && mouse.x <= r.right + grab &&
                         mouse.y >= r.top - grab && mouse.y <= r.bottom + grab;

  int hot = -1;
  if (cropPick_.drag >= 0) {
    hot = cropPick_.drag;
  } else if (overVideo && !ImGui::GetIO().WantCaptureMouse) {
    float best = grab;
    if (std::abs(mouse.x - xL) < best) { best = std::abs(mouse.x - xL); hot = 0; }
    if (std::abs(mouse.x - xR) < best) { best = std::abs(mouse.x - xR); hot = 1; }
    if (std::abs(mouse.y - yT) < best) { best = std::abs(mouse.y - yT); hot = 2; }
    if (std::abs(mouse.y - yB) < best) { best = std::abs(mouse.y - yB); hot = 3; }
  }

  if (hot >= 0) {
    ImGui::SetMouseCursor(hot < 2 ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
  }
  if (hot >= 0 && cropPick_.drag < 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    cropPick_.drag = hot;
  }
  if (cropPick_.drag >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    cropPick_.drag = -1;
  }

  if (cropPick_.drag >= 0) {
    // At least sixteen source pixels have to survive in each direction --
    // a zero sized picture is not a crop, it is a crash waiting to happen.
    const int minKeep = 16;
    switch (cropPick_.drag) {
      case 0:
        cropPick_.left = Clamp(toSrcX(mouse.x), 0, format.width - cropPick_.right - minKeep);
        break;
      case 1:
        cropPick_.right =
            Clamp(format.width - toSrcX(mouse.x), 0, format.width - cropPick_.left - minKeep);
        break;
      case 2:
        cropPick_.top = Clamp(toSrcY(mouse.y), 0, format.height - cropPick_.bottom - minKeep);
        break;
      case 3:
        cropPick_.bottom =
            Clamp(format.height - toSrcY(mouse.y), 0, format.height - cropPick_.top - minKeep);
        break;
      default: break;
    }
    xL = toScreenX(cropPick_.left);
    xR = toScreenX(format.width - cropPick_.right);
    yT = toScreenY(cropPick_.top);
    yB = toScreenY(format.height - cropPick_.bottom);
  }

  // ---- painting ----
  const ImU32 dim = IM_COL32(0, 0, 0, 150);
  dl->AddRectFilled(ImVec2((float)r.left, (float)r.top), ImVec2(xL, (float)r.bottom), dim);
  dl->AddRectFilled(ImVec2(xR, (float)r.top), ImVec2((float)r.right, (float)r.bottom), dim);
  dl->AddRectFilled(ImVec2(xL, (float)r.top), ImVec2(xR, yT), dim);
  dl->AddRectFilled(ImVec2(xL, yB), ImVec2(xR, (float)r.bottom), dim);

  const ImU32 line = IM_COL32(255, 255, 255, 230);
  const ImU32 lineHot = IM_COL32(255, 200, 80, 255);
  dl->AddRect(ImVec2(xL, yT), ImVec2(xR, yB), line, 0.0f, 1.5f);

  // A thicker bar on each edge, so there is something obvious to aim at.
  const float bar = 4.0f;
  dl->AddRectFilled(ImVec2(xL - bar * 0.5f, yT), ImVec2(xL + bar * 0.5f, yB),
                    hot == 0 ? lineHot : line);
  dl->AddRectFilled(ImVec2(xR - bar * 0.5f, yT), ImVec2(xR + bar * 0.5f, yB),
                    hot == 1 ? lineHot : line);
  dl->AddRectFilled(ImVec2(xL, yT - bar * 0.5f), ImVec2(xR, yT + bar * 0.5f),
                    hot == 2 ? lineHot : line);
  dl->AddRectFilled(ImVec2(xL, yB - bar * 0.5f), ImVec2(xR, yB + bar * 0.5f),
                    hot == 3 ? lineHot : line);

  // ---- toolbar ----
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 18.0f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.92f);
  if (ImGui::Begin("##croptools", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
    ImGui::TextUnformatted(T("Ränder ziehen", "Drag the edges"));
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Text(T("links %d  rechts %d  oben %d  unten %d", "left %d  right %d  top %d  bottom %d"),
                cropPick_.left, cropPick_.right, cropPick_.top, cropPick_.bottom);
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Text(T("Ergebnis %dx%d", "Result %dx%d"),
                format.width - cropPick_.left - cropPick_.right,
                format.height - cropPick_.top - cropPick_.bottom);

    ImGui::SameLine();
    if (ImGui::Button(T("Übernehmen", "Apply"))) {
      EndCropPick(true);
      ImGui::End();
      return;
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Abbrechen", "Cancel"))) {
      EndCropPick(false);
      ImGui::End();
      return;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Nichts##crop", "None##crop"))) {
      cropPick_.left = cropPick_.right = cropPick_.top = cropPick_.bottom = 0;
    }
  }
  ImGui::End();
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

void App::FeedRecorder() {
  if (!recorder_.recording()) return;

  const double now = ImGui::GetTime();

  // Die Quelle hat mitten in der Aufnahme die Norm gewechselt. Bildgroesse und
  // Rate stehen in der ffmpeg-Zeile fest -- rawvideo hat keinen Kopf, in dem
  // etwas anderes stehen koennte --, also passt ab dem Wechsel kein Bild mehr
  // in die laufende Datei. Der Recorder wirft die falsch geformten weg, sonst
  // laese er ueber den Puffer hinaus; die Datei bliebe damit aber ab dem
  // Wechsel einfach stehen und die Aufnahme waere weg. Also: schneiden und mit
  // der neuen Form weiterschreiben.
  //
  // Nicht sofort, sondern erst wenn die neue Form eine Weile steht. Ein
  // Normsuchlauf geht durch mehrere Zeilenzahlen, und jede davon sofort zu
  // schneiden gaebe eine Handvoll Dateien von je einer Sekunde. Der Zaehler
  // faengt deshalb bei jeder *Aenderung* von vorne an und laeuft nur, solange
  // die Abweichung besteht -- kehrt die Quelle zurueck, wird gar nicht
  // geschnitten und die Datei laeuft durch.
  {
    const int liveWidth = renderer_.outputWidth();
    const int liveHeight = renderer_.outputHeight();
    double liveFps = recordSourceFps_;
    if (const FrameBuffer* sink = capture_.sink()) {
      const double measured = sink->stats().sourceFps;
      if (measured > 1.0) liveFps = measured;
    }

    // Die Rate wird grosszuegig verglichen, weil sie gemessen ist und gemessene
    // Werte zittern. Was hier auseinandergehalten werden muss, sind 25 und
    // 29,97 oder 50 und 59,94 -- ein Fuenftel auseinander, weit jenseits von
    // allem, was ein Ruckler in der Messung bewegt.
    //
    // Das faengt nebenbei einen zweiten Fall, und der ist kein Schaden: die
    // Messung zaehlt Bilder in Fenstern von einer Sekunde, und wer eine
    // Aufnahme unmittelbar nach einem Graphenumbau startet, bekommt das
    // angebrochene Fenster davor als Rate in die Datei geschrieben. Dann steht
    // die falsche Zahl in `-r`, anderthalb Sekunden spaeter faellt es hier auf,
    // und die Aufnahme laeuft ab da mit der richtigen weiter.
    // Gerade gerechnet, weil der Recorder die Kantenlaengen selbst abrundet --
    // die meisten Encoder koennen keine ungerade Bildgroesse. Ohne das Abrunden
    // hier waere eine ungerade Quelle dauerhaft "anders" als die eigene Datei
    // und wuerde alle anderthalb Sekunden geschnitten.
    const bool shapeDiffers = liveWidth > 0 && liveHeight > 0 &&
                              ((liveWidth & ~1) != recorder_.frameWidth() ||
                               (liveHeight & ~1) != recorder_.frameHeight());
    const bool rateDiffers = recordSourceFps_ > 1.0 && liveFps > 1.0 &&
                             std::fabs(liveFps - recordSourceFps_) > recordSourceFps_ * 0.08;

    if (!shapeDiffers && !rateDiffers) {
      pendingSince_ = -1.0;
    } else if (liveWidth != pendingWidth_ || liveHeight != pendingHeight_ ||
               std::fabs(liveFps - pendingFps_) > 0.5 || pendingSince_ < 0.0) {
      pendingWidth_ = liveWidth;
      pendingHeight_ = liveHeight;
      pendingFps_ = liveFps;
      pendingSince_ = now;
    } else if (now - pendingSince_ > 1.5 && captureState_ == CaptureState::Running &&
               renderer_.hasFrame()) {
      // Der Neustart braucht ein Bild, sonst bricht `StartRecording` ab und die
      // Aufnahme waere nach dem Stop zu Ende statt geteilt. Waehrend ein Graph
      // neu gebaut wird, gibt es keins -- dann wird eben weiter gewartet.
      CAP_LOG("Recording: source now %dx%d @ %.2f fps instead of %dx%d @ %.2f, new file",
              liveWidth, liveHeight, liveFps, recorder_.frameWidth(), recorder_.frameHeight(),
              recordSourceFps_);
      StopRecording();
      StartRecording();
      return;
    }
  }

  // Splitting is a restart, not a seamless cut: ffmpeg has to close the
  // container it is writing. A fraction of a second is lost at the boundary,
  // which is why this is off unless someone really is on FAT32.
  if (config_.record.splitFiles) {
    if (now - lastSplitCheck_ > 1.0) {
      lastSplitCheck_ = now;
      const uint64_t limit = (uint64_t)config_.record.splitSizeMb * 1024ull * 1024ull;
      if (recorder_.outputFileSize() >= limit) {
        CAP_LOG("Recording: size limit reached, new file");
        StopRecording();
        StartRecording();
        return;
      }
    }
  }

  // Der Platz auf dem Laufwerk. Eine volllaufende Platte bricht ffmpeg mitten
  // im Schreiben ab, und was dann liegen bleibt, haengt vom Format ab: MKV
  // uebersteht das meistens, MP4 ohne seinen Index am Ende selten. Also lieber
  // eine Minute zu frueh sauber beendet als eine Datei, die keiner aufmacht.
  if (diskFreeKnown_) {
    // Gemessen schlaegt geschaetzt, sobald es etwas zu messen gibt: was dieser
    // Encoder auf dieser Quelle wirklich schreibt, kann von der eingestellten
    // Bitrate weit weg sein -- im Qualitaetsmodus ist es das immer.
    const RecordStats stats = recorder_.stats();
    double rate = diskBytesPerSecond_;
    if (stats.seconds > 3.0 && stats.bytesWritten > 0) {
      rate = (double)stats.bytesWritten / stats.seconds;
    }
    const double left = rate > 1.0 ? (double)diskFreeBytes_ / rate : 0.0;

    // Ein fester Boden zusaetzlich zur Zeit: bei kleiner Bitrate reichen 30 s
    // rechnerisch noch lange, waehrend Windows schon keinen Platz mehr fuer
    // seine eigenen Schreibpuffer hat.
    if (diskFreeBytes_ < kDiskFloorBytes || (rate > 1.0 && left < 20.0)) {
      CAP_WARN("Recording: disk almost full (%s free), stopped",
               FormatBytes(diskFreeBytes_).c_str());
      StopRecording();
      Toast(T("Aufnahme beendet — Speicherplatz fast aufgebraucht.",
              "Recording stopped — the disk is nearly full."));
      return;
    }
    // Einmal je Aufnahme: eine Warnung, die im Sekundentakt wiederkommt, ist
    // keine Warnung mehr.
    if (!diskWarned_ && rate > 1.0 && left < 120.0) {
      diskWarned_ = true;
      Toast(Format(T("Nur noch %s frei — etwa %s Aufnahme.",
                     "Only %s free — about %s of recording."),
                   FormatBytes(diskFreeBytes_).c_str(), FormatDuration(left).c_str()));
    }
  }

  // ffmpeg died on its own: stop cleanly rather than filling a dead pipe.
  if (recorder_.failed()) {
    const RecordStats stats = recorder_.stats();
    StopRecording();
    Toast(stats.error.empty() ? T("Aufnahme abgebrochen.", "Recording aborted.") : stats.error);
    return;
  }

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
    int timeout = settings_.isOpen() ? 16 : Clamp((int)idleFloor, 16, 100);
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
  CollectEncoderProbe();
  // Erst entscheiden, ob der Decoder ueberhaupt befragt wird, dann das
  // Ergebnis benutzen. Beim Start der Aufnahme steht das Format noch nicht
  // fest, der Wachthread laeuft also zunaechst an und wird hier ein Bild
  // spaeter wieder angehalten, sobald sich die Quelle als digital erweist.
  UpdateSignalWatch();
  UpdateVideoStandard();
  UpdateProfileForStandard();
  UpdateCropForFormat();

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
  SyncMicrophone();
  FeedRecorder();
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
  toolbarVisible_ = config_.app.showToolbar && !cropPick_.active &&
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
    const SettingsWindow::StandardSearch search = StandardSearchDisplay();
    switch (search) {
      case SettingsWindow::StandardSearch::Trying:
      case SettingsWindow::StandardSearch::Colour: {
        // Kopfzeile und Grund kommen aus einer Hand, weil sie zusammengehoeren:
        // welcher Schritt von wie vielen laeuft, haengt daran, welche der
        // beiden Stufen gerade sucht.
        std::string headline, detail;
        StandardSearchText(&headline, &detail);
        DrawSearchIndicator(headline, detail);
        break;
      }
      // Und was daraus geworden ist, an derselben Stelle: die Einblendung wird
      // nicht ersetzt, sie hoert auf zu laufen. Nur nach einem Suchlauf von
      // Hand -- die Automatik sucht bei jedem Quellenwechsel, und ein Ergebnis
      // nach jedem waere kein Ergebnis mehr, sondern ein Bildschirmelement.
      case SettingsWindow::StandardSearch::Result: {
        std::string headline, detail;
        StandardSearchText(&headline, &detail);
        const double left = TicksToSeconds(standardResultUntilQpc_ - ClockTicks());
        DrawSearchResult(headline, detail, kStandardResultSeconds - left, kStandardResultSeconds);
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
  if (cropPick_.active) {
    clickOnPicture_ = false;
    DrawCropPicker();
    if (!cropPick_.active) return;  // Apply or Cancel closed it this frame
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
  settings_.SetSignalLocked(PollSignalLocked());
  settings_.SetStandardSearch(StandardSearchDisplay());
  settings_.SetLiveStandard(capture_.running() ? signalStandard_.load(std::memory_order_relaxed)
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
  UpdateDiskSpace();
  if (settings_.takeCompareToggle()) ToggleCompare();
  if (settings_.takeBypassToggle()) ToggleBypass();
  if (settings_.takeCropPickRequest()) BeginCropPick();
  if (settings_.takeDeviceConfigRequest()) OpenDeviceConfig();
  if (settings_.takeCropDetectRequest()) DetectCrop();
  if (settings_.takeCardResetRequest()) ReinitialiseCard();
  if (settings_.takeRangeRemeasureRequest()) RemeasureRange();
  settings_.setProbeBusy(probing_.load(std::memory_order_relaxed));
  // Drawn here only when the settings live inside the picture. The separate
  // window is deliberately not touched from in here: this runs between the main
  // context's NewFrame and Render, and presenting a second swapchain in the
  // middle of another window's frame flushes every bit of GPU work already
  // queued for it -- sixty times a second, while the preview runs at twice that
  // or more. That was not merely qBlank stuttering; it was enough to make the
  // desktop's own cursor stutter. It happens after the present instead.
  if (!settingsAreWindowed()) {
    settings_.SetFillsWindow(false);
    if (settings_.Draw(capture_.running() ? &capture_.capabilities() : nullptr, &ffmpeg_) ==
        SettingsWindow::Result::Close) {
      settings_.Close();
    }
  }
  if (settings_.takeProbeRequest()) StartEncoderProbe(true);
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
      ToggleRecording();
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
    DetectCrop();
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
    RescanVideoStandard();
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
  if (cropPick_.active) {
    if (key == Key::Escape) {
      EndCropPick(false);
      return true;
    }
    if (key == Key::Enter) {
      EndCropPick(true);
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
      ToggleRecording();
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
      DetectCrop();
      return true;
    case HotkeyAction::DetectStandard:
      RescanVideoStandard();
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
