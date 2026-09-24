#pragma once

// Ties everything together: window, message loop, capture graph, audio engine,
// renderer and UI.
//
// The class is spread over several files by topic: app.cpp (start, window,
// capture, settings), app_frame.cpp (main loop and frame), app_source.cpp
// (what the source is), app_ui.cpp (overlay and menus), app_input.cpp (keys and
// window messages), app_tray.cpp and app_hosts.cpp. The parts with their own
// state are classes of their own: VideoStandardSearch, RecordingControl,
// CropTool and StartupNotices.

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "audio/audio_engine.h"
#include "audio/mic_capture.h"
#include "capture/device_config.h"
#include "capture/video_capture.h"
#include "common.h"
#include "config.h"
#include "crop_tool.h"
#include "record/recorder.h"
#include "recording_control.h"
#include "render/display.h"
#include "render/video_renderer.h"
#include "ui/overlay.h"
#include "ui/settings_host.h"
#include "update/updater.h"
#include "camera_sink.h"
#include "ui/settings_window.h"
#include "ui/startup_notices.h"
#include "ui/toolbar.h"
#include "ui/tray_menu.h"
#include "tray.h"
#include "video_standard_search.h"
#include "window.h"

namespace cap {

// Holds captured frames back by a fixed time, used when the A/V offset asks for
// the picture to wait for the sound. Only allocated while that is the case,
// because it costs both memory and exactly the latency it introduces.
class FrameDelayLine {
 public:
  void Configure(double delayMs);
  void Clear();
  bool active() const { return delayMs_ > 0.0; }
  double delayMs() const { return delayMs_; }

  void Push(const FrameView& frame, int64_t qpc);
  // Returns the newest frame that is already old enough, dropping older ones.
  bool Pop(FrameView* out, int64_t qpc);
  // Wann das zuletzt herausgegebene Bild hineingereicht wurde. Die Wartezeit
  // hier drin ist eingestellte Verzoegerung und keine Panne, sie gehoert aber
  // in die Durchlaufzeit: das Bild ist wirklich so alt, wenn es hinausgeht.
  int64_t lastPoppedQpc() const { return current_.qpc; }

 private:
  struct Entry {
    std::vector<uint8_t> data;
    int64_t qpc = 0;
    uint64_t sequence = 0;
  };
  std::deque<Entry> queue_;
  std::vector<Entry> pool_;
  Entry current_;
  double delayMs_ = 0.0;
};

class App {
 public:
  App() = default;
  ~App();

  bool Initialize();
  int Run();
  void Shutdown();
  // Asked for from inside: start the program again once this one is gone.
  bool restartAfterExit() const { return restartAfterExit_; }

 private:
  enum class CaptureState { Idle, Running, Reconnecting, NeedsSetup };

  bool CreateMainWindow();
  bool OnWindowEvent(const WindowEvent& e);
  bool InitImGui();
  void ShutdownImGui();
  bool StartGraphics();

  void Tick();
  void RenderFrame();
  void DrawUi();
  void DrawContextMenu();

  bool StartCapture(std::string* error);
  void StopCapture();
  void StartAudio();
  void RestartAll(bool userRequested);
  // Wie lange die Anzeige hoechstens stehen darf, wenn kein Bild ankommt.
  // Haengt davon ab, ob etwas auf dem Schirm ist, das sich bewegen muss.
  double IdleFloorMs() const;
  // Ob das Bild aus dem Analogdekoder kommt, und die Bildeinstellungen so, wie
  // sie fuer diese Quelle gelten. Was in den Einstellungen ausgeblendet ist,
  // darf auch nicht mehr wirken -- siehe die Umsetzung.
  bool SourceIsAnalogue() const;
  // Welcher analoge Anschluss es ist, ohne Auto: die Einstellung, wo eine
  // steht, sonst der Crossbar, sonst Composite. Siehe die Umsetzung.
  AnalogConnector ResolvedConnector() const;
  // Ob Helligkeit und Farbe sich eine Leitung teilen. Das ist die Frage hinter
  // Punktkriechen und Regenbogen -- beide sind Uebersprechen zwischen den
  // beiden, und ohne gemeinsame Leitung gibt es keines. Nur Composite.
  bool ConnectorMixesLumaAndChroma() const;
  // Ob die Farbe ueberhaupt auf einem Traeger sitzt. Bei S-Video tut sie das,
  // nur eben auf einer eigenen Leitung; bei Component gibt es keinen Traeger.
  // Daran haengt, ob die Farbrunde der Normsuche etwas messen kann.
  bool ConnectorHasColourCarrier() const;
  ImageSettings EffectiveImage(const Profile& profile) const;
  // Wie RestartAll, verwirft aber vorher, was fuer den vorherigen Eingang
  // gemessen wurde -- Videonorm und Format. Siehe die Umsetzung.
  void ReinitialiseCard();
  // Die gemerkte Aufloesung loslassen, wenn die neue Norm eine andere
  // Zeilenzahl hat -- 720x576 und 720x480 lassen sich nicht gegeneinander
  // austauschen. Wahr, wenn wirklich etwas verworfen wurde. Siehe die
  // Umsetzung; vor jedem Graphenbau nach einem Normwechsel zu rufen.
  bool ReleaseStandardBoundFormat(int newLines);

  void OpenSettings(const std::string& reason);

  // Live settings: the dialog edits config_ directly, and this notices what
  // changed and does whatever that change requires -- restart the graph, re-route
  // the crossbar, retheme, or nothing at all.
  void SyncConfigChanges();
  void CaptureAppliedState();
  void MaybeSaveConfig();
  // Saves, ends the main loop and has RunProgram start the program again after
  // the shutdown -- after, so the new one finds the card and the files free.
  void QuitAndRestart();

  void SwitchProfile(int index);
  void ToggleFullscreen();
  void SetFullscreen(bool on);
  void ApplyWindowFlags();
  void ApplyTheme();
  // The icon in the notification area: shown or not, its menu kept current,
  // and what happened to it answered on this thread.
  void UpdateTray();
  std::vector<TrayMenuItem> BuildTrayMenu();
  // The icon's menu, when it is open, and whatever is picked there.
  void DrawTrayMenu();
  void RunTrayCommand(int id);
  void AdjustVolume(float delta);
  void ToggleMute();
  void ShowVolumeOsd();

  // Grabs the next rendered frame and writes it to disk, or puts it on the
  // clipboard instead. Both take the same shot at the same moment; only the
  // destination differs.
  void RequestScreenshot(bool toClipboard = false) {
    screenshotPending_ = true;
    screenshotToClipboard_ = toClipboard;
  }
  // `includeUi` false grabs the picture itself at source resolution; true grabs
  // the finished window, overlay and all, at window resolution.
  void WriteScreenshot(bool includeUi, bool toClipboard);
  // Haelt an, was in die Textur geht -- der Zulauf laeuft weiter, die Filter
  // auch. Ein Standbild ist zum Hinsehen da: an einem stehenden Bild laesst
  // sich ein Regler beurteilen, an einem laufenden nicht.
  //
  // Waehrend einer Aufnahme geht das nicht. Aufnahme und virtuelle Kamera holen
  // ihr Bild aus derselben Textur wie die Anzeige, ein Standbild wuerde also
  // auch in die Datei und in die Kamera gefrieren -- was niemand meint, wenn er
  // die Anzeige anhaelt.
  void ToggleFreeze();
  void ToggleCompare();
  void ToastCompareSide();
  void ChooseCompare(bool horizontal);
  void ToggleBypass();
  void DrawToolbarStrip();
  void ShowOutputFolder(std::string* configured, const std::filesystem::path& fallback);
  // Whether the deinterlacer should run at all. Only interesting when
  // "interlaced sources only" is ticked, and then it is the measurement that
  // decides, not the media type.
  bool SourceLooksInterlaced(const Profile& profile) const;
  // Ob das gemessene "interlaced" bei dieser Quelle nach einem Irrtum aussieht
  // und deshalb nachgefragt statt behauptet wird. Siehe die Umsetzung.
  bool InterlaceVerdictDoubtful(const Profile& profile) const;
  // Ob an einem analogen Eingang der volle Wertebereich ankommt, wo die Norm
  // 16-235 vorsieht -- ein Hinweis, keine Warnung. Siehe die Umsetzung.
  bool AnalogueRangeIsFull() const;
  // Die sichtbaren Zeilen der anliegenden Norm, wenn die laufende Aufloesung
  // nicht dazu passt und die Karte eine passende anbietet; sonst 0. Siehe die
  // Umsetzung.
  int ResolutionMismatchLines() const;
  // Stellt die Groesse ein, die zu `activeLines` passt, und sagt es per Toast.
  // False, wenn es keine gibt oder sie schon eingestellt ist.
  bool ApplyFittingResolution(int activeLines);
  // Der Hinweis mit "Anpassen" und "Ignorieren", solange er offen ist. Gibt
  // seine Hoehe zurueck, damit ein Toast darueber steht, sonst 0.
  float DrawResolutionNotice();
  // Der Hinweis der Nachkontrolle, dass die Videonorm nicht passen koennte.
  // Steht an derselben Stelle, also nur, wenn der zur Aufloesung nicht steht.
  float DrawColourNotice();

  // Dasselbe fuer den Wertebereich: das Urteil steht, bis sich das Bildformat
  // aendert, und eine Option im Treiber der Karte aendert es nicht. Wirft nur
  // diese eine Messung weg -- Verschraenkung und Bildgrenzen bleiben stehen,
  // die neu zu suchen sieht man sofort.
  void RemeasureRange();

  // Draws the settings into their own window when that is switched on.
  // Returns true when it took care of them, so the in-picture panel is skipped.
  // True while the settings live in a window of their own, whether or not that
  // window happens to be visible.
  bool settingsAreWindowed() const { return config_.app.settingsSeparateWindow; }
  // The settings window's own frame. Called *after* the main window has been
  // presented, never inside its frame -- see the comment at the call site.
  void DrawSettingsWindowed();
  void DrawSettingsFrame();  // its part that draws, for the resize loop as well
  // Copies the settings window's position into the configuration, so it comes
  // back where it was left rather than wherever Windows decides.
  void RememberSettingsWindow();

  void OpenDeviceConfig();
  // Waehlt das Profil, das zur erkannten Videonorm passt.
  void UpdateProfileForStandard();

  // Moves the A/B divider when it is dragged on the picture.
  void DragCompareDivider();
  // Fullscreen on a double click on the picture.
  void DoubleClickFullscreen();
  // Borderless window: dragging the picture moves the window.
  void DragBorderlessWindow();
 public:
  bool cropPickActive() const { return cropTool_.active(); }
 private:
  // Makes sure an output folder exists. Recreates it when it was deleted, and
  // falls back to the default when even that fails.
  std::filesystem::path ResolveOutputFolder(std::string* configured,
                                            const std::filesystem::path& fallback);
  // One place decides whether the GPU has to hand pictures back, and one place
  // hands them out -- the recorder and the camera both want the same frame, and
  // fetching it twice would give each of them every second one.
  void FeedFrameConsumers();
  void UpdateVirtualCamera();
  // Decides what curve the picture is in and what the screen should be given.
  // Runs every frame because both ends can change underneath it: a console
  // switches to HDR, or the window is dragged onto another screen.
  void UpdateHdr();
  // Opens the website.
  void OpenWebsite();

  // `file`, if given, makes the toast clickable: a click shows that file in
  // Explorer.
  void Toast(const std::string& text, const std::filesystem::path& file = {});
  // `lift`: so viel hoeher, damit er ueber einem offenen Hinweis steht.
  void DrawToastStrip(float lift = 0.0f);
  void UpdatePowerRequest();
  void SaveWindowPlacement();
  void SaveConfig();

  bool HandleKeyDown(Key key, bool ctrl, bool shift, bool alt);
  // Every key press from either window comes through here. `busy` is true when
  // the ImGui context of the window it arrived at wants the key for itself.
  bool OnKey(Key key, bool ctrl, bool shift, bool alt, bool busy);

  Window window_;
  bool running_ = false;
  bool restartAfterExit_ = false;
  bool minimized_ = false;
  bool fullscreen_ = false;
  bool imguiReady_ = false;
  bool darkMode_ = true;
  float uiScale_ = 1.0f;
  float pendingUiScale_ = 0.0f;  // from a DPI change, applied before the next frame

  Config config_;
  Display display_;
  VideoRenderer renderer_;

  // The application icon as a texture, for the empty state. Loaded once; empty
  // when the icon could not be read, which costs the idle screen its picture
  // and nothing else.
  UiImage idleIcon_;
  int idleIconSize_ = 0;
  void LoadIdleIcon();
  // Whether the picture arriving is worth showing. Combines what the pixels say
  // with what the analogue decoder says, because neither is sufficient alone.
  bool HaveLiveSignal() const;
  VideoCapture capture_;
  AudioEngine audio_;
  MicCapture mic_;
  SettingsWindow settings_;
  FrameDelayLine delayLine_;
  Recorder recorder_;

  CaptureState captureState_ = CaptureState::Idle;
  std::string captureError_;
  int64_t nextRetryQpc_ = 0;
  int retryCount_ = 0;

  // Bob deinterlacing presents each captured frame twice, once per field.
  bool secondFieldPending_ = false;
  int64_t secondFieldQpc_ = 0;
  int fieldIndex_ = 0;

  // Present rate measurement.
  int64_t fpsWindowQpc_ = 0;
  int presentCount_ = 0;
  double presentFps_ = 0.0;
  // Die beiden Zahlen, die sich mit jedem Bild aendern. Jedes Bild gefuettert,
  // gelesen im Anzeigetakt vom Panel und im Fuenfsekundentakt vom Log.
  StatMeter frameAgeMeter_;
  // Wann das Bild ankam, das gerade gezeichnet wird -- der Anfang der Strecke,
  // die frameAgeMeter_ misst. Beim Bob-Deinterlacing zeigen zwei Durchgaenge
  // dasselbe angekommene Bild, und der zweite ist absichtlich ein halbes
  // Vollbild spaeter dran: genau das soll die Zahl auch sagen.
  int64_t displayedArrivalQpc_ = 0;
  StatMeter audioBufferMeter_;
  int statsLogCounter_ = 0;

  // Diagnostics for "the card starts but nothing shows up".
  bool sawFirstFrame_ = false;
  int64_t captureStartQpc_ = 0;

  // Snapshot of the settings that actually drive something, so a live edit can
  // be told apart from a harmless one.
  struct AppliedState {
    int activeProfile = -1;
    long videoStandard = 0;
    DeviceRef video;
    FormatSel format;
    int crossbarInput = -1;
    AudioSource audioSource = AudioSource::Embedded;
    DeviceRef audioIn;
    DeviceRef audioOut;
    bool exclusive = false;
    int bufferMs = 0;
    int avOffsetMs = 0;
    float volume = 1.0f;
    bool mute = false;
    Theme theme = Theme::Dark;
    unsigned accent = 0;
    bool alwaysOnTop = false;
    bool borderless = false;
    Language language = Language::German;
  };
  AppliedState applied_;

  // Saving is throttled: dragging a slider must not write the file every frame.
  std::string lastSerialized_;
  bool configDirty_ = false;
  double lastConfigChange_ = 0.0;
  double lastSerializeCheck_ = 0.0;

  bool screenshotPending_ = false;
  bool screenshotToClipboard_ = false;
  // Standbild: siehe ToggleFreeze. Kein Profilwert, sondern ein Zustand -- ein
  // angehaltenes Bild soll einen Neustart nicht ueberleben.
  bool frozen_ = false;
  // Die beiden anderen Sehhilfen, aus demselben Grund ebenfalls hier und nicht
  // im Profil: siehe ToggleCompare und ToggleBypass.
  bool compare_ = false;
  bool compareDrag_ = false;  // the divider is being dragged with the mouse
  bool clickOnPicture_ = false;  // the last left click landed on the bare picture
  bool moveDragArmed_ = false;   // the left button went down on the bare picture
  bool recordingBadge_ = false;  // the red dot is on the taskbar button
  TrayIcon tray_;
  TrayMenu trayPopup_;
  std::vector<TrayMenuItem> trayMenu_;  // what the icon's menu holds right now
  std::string trayTooltip_;
  bool trayFailed_ = false;  // not retried until the setting goes off and on
  bool bypass_ = false;
  // Whether the bar was drawn this frame; the picture layout follows it.
  bool toolbarVisible_ = false;

  // Filled from the renderer each frame; the settings dialog reads it.
  const char* detectedRangeText_ = nullptr;
  // Die Zahlen unter dem Urteil, schon gesetzt. Leer, solange nichts gemessen
  // ist. Siehe SettingsWindow::SetRangeNumbers.
  std::string rangeNumbersText_;
  const char* detectedInterlaceText_ = nullptr;
  // Ob der Hinweis auf ein fragwuerdiges "interlaced" schon einmal aufgeploppt
  // ist. Die Meldung gehoert zum Umschlagen, nicht zum Zustand -- ohne das hier
  // stuende sie alle paar Sekunden wieder da, solange die Quelle anliegt.
  // Zurueckgesetzt, sobald der Zweifel weg ist, damit das naechste Umschlagen
  // wieder eine Meldung wert ist.
  bool interlaceDoubtToasted_ = false;
  // Dasselbe fuer den Wertebereich, nur fuers Protokoll: der Zustand haelt,
  // solange die Quelle anliegt -- das Urteil friert ein und taut nur auf F6
  // oder einen Formatwechsel wieder auf -- also gehoert die Zeile an den
  // Augenblick, in dem er eintritt, und nicht an jedes Bild danach.
  bool analogueFullRangeLogged_ = false;
  // Seit wann die Aufloesung nicht zur Norm passt (-1: sie passt), und ob das
  // schon gemeldet ist. Die Wartezeit, weil Norm und Format nach einem
  // Umschalten nicht im selben Bild ankommen.
  double resolutionMismatchSince_ = -1.0;
  bool resolutionMismatchToasted_ = false;
  // Der offene Hinweis mit Knoepfen, wenn nicht automatisch angepasst wird:
  // sein Text und die Zeilen, zu denen er passen soll (0: keiner offen).
  std::string resolutionNoticeText_;
  int resolutionNoticeLines_ = 0;
  // Was mit "Ignorieren" weggeklickt wurde, bis zum Ende der Sitzung. Dieselbe
  // Groesse an derselben Norm fragt dann nicht noch einmal -- ein GameCube
  // wechselt zwischen 50 und 60 Hz, und jeder Wechsel waere sonst eine neue
  // Frage. Der Hinweis im Reiter Quelle bleibt.
  int resolutionIgnoredWidth_ = 0;
  int resolutionIgnoredHeight_ = 0;
  int resolutionIgnoredLines_ = 0;
  DevicePropertyPages devicePages_;
  SettingsHost settingsHost_;
  Updater updater_;
  CameraSink virtualCamera_;
  // Refilled once a frame rather than allocated once a frame.
  std::vector<CameraSink::Consumer> virtualCameraConsumers_;

  // Recording, and what it asks of the rest of the program.
  class RecordingHost final : public RecordingControl::Host {
   public:
    explicit RecordingHost(App& app) : app_(app) {}
    bool CaptureRunning() const override;
    std::filesystem::path ResolveOutputFolder(std::string* configured,
                                              const std::filesystem::path& fallback) override;
    void DropViewAids() override;
    void Toast(const std::string& text, const std::filesystem::path& file) override;

   private:
    App& app_;
  };
  RecordingHost recordingHost_{*this};
  RecordingControl recording_{config_, recorder_, renderer_, capture_, audio_,
                              mic_, settings_, virtualCamera_, recordingHost_};

  // Cropping, and what it asks of the rest of the program.
  class CropHost final : public CropTool::Host {
   public:
    explicit CropHost(App& app) : app_(app) {}
    void Toast(const std::string& text) override;
    void OpenSettings() override;
    void CloseSettings() override;

   private:
    App& app_;
  };
  CropHost cropHost_{*this};
  CropTool cropTool_{config_, renderer_, cropHost_};

  // The notices around a start, and what they ask of the rest of the program.
  class NoticesHost final : public StartupNotices::Host {
   public:
    explicit NoticesHost(App& app) : app_(app) {}
    float UiScale() const override;
    void Toast(const std::string& text) override;
    void Restart() override;

   private:
    App& app_;
  };
  NoticesHost noticesHost_{*this};
  StartupNotices notices_{updater_, noticesHost_};

  // What woke the main loop last time round, and when it last drew. Together
  // they keep the preview paced by the picture rather than by the message
  // queue -- see the comment at the call to RenderFrame.
  WaitResult lastWake_ = WaitResult::Timeout;
  int64_t lastRenderQpc_ = 0;
  int hdrDisplayPoll_ = 0;
  bool devicePagesWereBusy_ = false;
  // Guards the frame drawn from inside a window drag against re-entering itself.
  bool inModalFrame_ = false;
  // How often each field actually reached the screen. Equal counts mean the
  // deinterlacer is being shown at the rate it is designed for; a shortfall on
  // the second one is the picture juddering.
  uint64_t fieldsShown_[2] = {0, 0};
  // Smoothed estimate of when the next frame is due. The card does not deliver
  // on a metronome, and pacing the fields off each raw arrival hands that
  // wobble straight to the viewer.
  int64_t framePhaseQpc_ = 0;

  // Automatic video standard, and what it asks of the rest of the program.
  class StandardSearchHost final : public VideoStandardSearch::Host {
   public:
    explicit StandardSearchHost(App& app) : app_(app) {}
    bool CaptureRunning() const override;
    bool SourceIsAnalogue() const override;
    AnalogConnector ResolvedConnector() const override;
    bool ConnectorHasColourCarrier() const override;
    long AppliedVideoStandard() const override;
    bool ReleaseStandardBoundFormat(int newLines) override;
    bool StartCapture(std::string* error) override;
    void Toast(const std::string& text) override;

   private:
    App& app_;
  };
  StandardSearchHost standardSearchHost_{*this};
  VideoStandardSearch standardSearch_{config_, capture_, renderer_, standardSearchHost_};
  // Die Zeilenzahl, unter der das laufende Format ausgesucht wurde. Aus der
  // Karte beim Graphenbau, nicht aus dem Profil -- dort kann "automatisch"
  // stehen. Siehe ReleaseStandardBoundFormat.
  int appliedStandardLines_ = 0;

  // Die Norm, auf die die Profilautomatik zuletzt reagiert hat. Sie macht aus
  // einem Zustand ("es ist PAL 60") ein Ereignis ("es ist gerade PAL 60
  // geworden") -- ohne das waere jede von Hand getroffene Profilwahl im
  // naechsten Bild wieder ueberschrieben. Siehe App::UpdateProfileForStandard.
  long profileMatchedStandard_ = 0;

  std::string toastText_;
  std::filesystem::path toastFile_;  // what a click on the toast shows, empty if nothing
  bool toastHint_ = false;     // this one says that it can be clicked
  bool toastTouched_ = false;  // the mouse has moved over it since it appeared
  double toastStart_ = 0.0;
  double volumeOsdStart_ = -1000.0;

  uint32_t lastPowerPokeTick_ = 0;
  // Es gab noch keine Konfigurationsdatei, als dieser Lauf begann. Entscheidet
  // ueber die Begruessung statt der Einstellungen.
  bool firstRun_ = false;
  int64_t lastMouseMoveQpc_ = 0;
  Point lastMousePos_;
};

}  // namespace cap
