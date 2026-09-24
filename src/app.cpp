#include "app.h"

#include <algorithm>
#include <cmath>

#include "desktop.h"
#include "i18n.h"
#include "imgui_internal.h"
#include "platform.h"
#include "record/screenshot.h"
#include "ui/theme.h"

namespace cap {
namespace {

// Delay between reconnect attempts, and the slower pace once it is clear the
// device is not coming back on its own.
const double kRetrySeconds = 2.0;
const double kRetrySlowSeconds = 10.0;
const int kRetryBackoffAfter = 5;
// Hide the pointer after this much stillness in fullscreen.
const double kCursorIdleSeconds = 2.0;

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
  const UnfinishedSession unfinished =
      LogInit(config_.app.logToFile, config_.app.logRetention);
  if (unfinished.found) {
    const LocalTime& s = unfinished.started;
    CAP_WARN("Previous session (%s, started %04d-%02d-%02d %02d:%02d:%02d) has no end line",
             unfinished.version.c_str(), s.year, s.month, s.day, s.hour, s.minute, s.second);
    notices_.QueueCrashNotice(unfinished);
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

  if (!StartGraphics()) return false;

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
  std::string error;
  if (firstRun_) {
    CAP_LOG("First start: welcome screen instead of settings");
    // Asked only for what is not there yet: a copy unpacked a second time, with
    // its settings file left behind, may still have its shortcuts.
    notices_.QueueWelcome(!HasShortcut(ShortcutPlace::StartMenu),
                          !HasShortcut(ShortcutPlace::Desktop));
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

// ------------------------------------------------------------------ graphics

// Display, ImGui's renderer and the video passes, on Vulkan when the settings
// ask for it and the build has it. Anything on the way that does not come up
// sends the whole stack back to Direct3D 11: a driver without a usable Vulkan
// costs the setting, never the picture.
bool App::StartGraphics() {
  std::string error;
  if (config_.app.vulkan && GraphicsApiBuilt(GraphicsApi::Vulkan)) {
    if (display_.Initialize(window_, &error, GraphicsApi::Vulkan) && InitImGui() &&
        renderer_.Initialize(&display_, &error)) {
      CAP_LOG("Graphics: Vulkan");
      return true;
    }
    CAP_WARN("Vulkan did not start (%s), using Direct3D 11", error.c_str());
    renderer_.Shutdown();
    ShutdownImGui();
    display_.Shutdown();
    error.clear();
  }

  if (!display_.Initialize(window_, &error)) {
    ShowErrorMessage(error);
    return false;
  }
  if (!InitImGui()) return false;
  if (!renderer_.Initialize(&display_, &error)) {
    ShowErrorMessage(error);
    return false;
  }
  CAP_LOG("Graphics: Direct3D 11");
  return true;
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

  // On failure the context goes as well, so StartGraphics can begin again on
  // another backend.
  if (!window_.AttachUi(nullptr)) {
    ImGui::DestroyContext();
    return false;
  }
  if (!display_.InitUi()) {
    // Messages went to ImGui only once both halves were up; the window must not
    // go on feeding them to half of it.
    window_.DetachUi();
    ImGui::DestroyContext();
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

void App::QuitAndRestart() {
  SaveConfig();
  restartAfterExit_ = true;
  running_ = false;
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

void App::OpenWebsite() { OpenUrl(WebsiteUrl()); }

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

// ------------------------------------------------------------------ main loop

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

}  // namespace cap
