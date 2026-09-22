#pragma once

// Ties everything together: window, message loop, capture graph, audio engine,
// renderer and UI.

#include <windows.h>

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
#include "common_win32.h"
#include "config.h"
#include "record/recorder.h"
#include "render/d3d_context.h"
#include "render/video_renderer.h"
#include "ui/overlay.h"
#include "ui/settings_host.h"
#include "update/updater.h"
#include "vcam/virtual_camera.h"
#include "ui/settings_window.h"
#include "ui/toolbar.h"

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

  bool Initialize(HINSTANCE instance, int showCmd);
  int Run();
  void Shutdown();

  LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

 private:
  enum class CaptureState { Idle, Running, Reconnecting, NeedsSetup };

  bool CreateMainWindow(HINSTANCE instance, int showCmd);
  bool InitImGui();
  void ShutdownImGui();

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

  void SwitchProfile(int index);
  void ToggleFullscreen();
  void SetFullscreen(bool on);
  void ApplyWindowFlags();
  void ApplyTheme();
  void AdjustVolume(float delta);
  void ToggleMute();
  void ShowVolumeOsd();

  // Test encodes take seconds and must never run on the UI thread -- doing so
  // froze the window long enough to drop displayed frames.
  void StartEncoderProbe(bool full);
  // Graphics hardware plus ffmpeg build. A cached encoder test only counts when
  // this still matches.
  std::string EncoderSignature() const;
  void LoadCachedEncoders();
  void SaveCachedEncoders();
  void CollectEncoderProbe();

  void ToggleRecording();
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
  void ToggleBypass();
  // Fragt den freien Platz auf dem Aufnahmelaufwerk ab, hoechstens einmal je
  // Sekunde. Die Einstellungen zeigen ihn an, die laufende Aufnahme haengt
  // daran.
  void UpdateDiskSpace();
  void DrawToolbarStrip();
  void OpenFolderInExplorer(std::string* configured, const std::filesystem::path& fallback);
  // Starts or stops the microphone to match the settings and what is going on.
  // `aboutToRecord` starts it for a recording that has not begun yet -- the
  // sample rate has to be known before ffmpeg is given its command line.
  void SyncMicrophone(bool aboutToRecord = false);
  // What SyncMicrophone last acted on, so a failure is reported once instead of
  // once per frame. Cleared when the settings change.
  DeviceRef micApplied_;
  bool micAttempted_ = false;
  bool micFailed_ = false;
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

  // Finds the analogue video standard by watching whether the decoder locks.
  // Runs only when the source is set to automatic.
  void UpdateVideoStandard();
  // Die zweite Stufe dahinter: ob die eingerastete Norm auch den richtigen
  // Farbtraeger hat. Der Lock kann das nicht sagen, ein graues Bild schon --
  // die Begruendung steht bei der Umsetzung.
  void VerifyStandardColour(int64_t now);
  void ResetStandardColourCheck();
  // Beide Stufen von Hand ausloesen, ohne den Umweg ueber den Normwaehler.
  // Siehe die Umsetzung: was ein Mensch an einer Norm auszusetzen hat, misst
  // die Automatik nicht unbedingt mit.
  void RescanVideoStandard();
  // Dasselbe fuer den Wertebereich: das Urteil steht, bis sich das Bildformat
  // aendert, und eine Option im Treiber der Karte aendert es nicht. Wirft nur
  // diese eine Messung weg -- Verschraenkung und Bildgrenzen bleiben stehen,
  // die neu zu suchen sieht man sofort.
  void RemeasureRange();
  // Das Ergebnis eines von Hand ausgeloesten Suchlaufs festhalten, damit es
  // ein paar Sekunden im Bild stehen kann. Tut nichts, wenn keiner lief.
  void FinishManualStandardSearch(long standard, const std::string& detail);
  // Ob gerade so ein Ergebnis angezeigt wird.
  bool ShowingStandardResult() const;
  // Die zwei Zeilen der Einblendung waehrend der Suche: was gerade laeuft, und
  // warum. Der Grund steht in `colourDoubt_` und den Zaehlern daneben, und
  // beides ist nur hier beisammen.
  void StandardSearchText(std::string* headline, std::string* detail) const;
  // Whether the decoder has a signal, asked of the driver a few times a second
  // rather than once a frame. Each call crosses into the driver, and doing that
  // twice per frame cost over ten milliseconds of every one -- more than the
  // entire video pipeline, and enough to stop the deinterlacer being shown at
  // its proper rate.
  int PollSignalLocked();
  // Was das Einstellungsfenster ueber die Normensuche schreiben soll. Der
  // Dialog kann das nicht selbst wissen: die Suche laeuft hier, stellt die
  // Karte mehrmals um und baut den Graphen dabei nicht neu.
  SettingsWindow::StandardSearch StandardSearchDisplay() const;
  // Asking the driver for the signal state takes about ten milliseconds -- it is
  // a round trip into kernel mode -- which is most of a field period and, on the
  // render thread, a visible hitch in the deinterlacer. So a small thread of its
  // own does the asking and leaves the answer where the render thread can pick
  // it up for free.
  void StartSignalWatch();
  void StopSignalWatch();
  void UpdateSignalWatch();

  // Draws the settings into their own window when that is switched on.
  // Returns true when it took care of them, so the in-picture panel is skipped.
  // True while the settings live in a window of their own, whether or not that
  // window happens to be visible.
  bool settingsAreWindowed() const { return config_.app.settingsSeparateWindow; }
  // The settings window's own frame. Called *after* the main window has been
  // presented, never inside its frame -- see the comment at the call site.
  void DrawSettingsWindowed();
  // Copies the settings window's position into the configuration, so it comes
  // back where it was left rather than wherever Windows decides.
  void RememberSettingsWindow();
  // The one-off notice when the check made at startup finds something. Shown in
  // the picture, because a tab nobody opened is not a notice.
  void DrawUpdatePrompt();
  // Once, when the log's previous session never got its end line.
  void DrawCrashNotice();

  void OpenDeviceConfig();
  void DetectCrop();
  // Wirft den Zuschnitt weg, wenn die Quelle ihre Groesse gewechselt hat --
  // oder holt den fuer die neue Groesse gemerkten hervor.
  void UpdateCropForFormat();
  // Waehlt das Profil, das zur erkannten Videonorm passt.
  void UpdateProfileForStandard();

  void BeginCropPick();
  void EndCropPick(bool apply);
  void DrawCropPicker();
  // Moves the A/B divider when it is dragged on the picture.
  void DragCompareDivider();
  // Fullscreen on a double click on the picture.
  void DoubleClickFullscreen();
 public:
  bool cropPickActive() const { return cropPick_.active; }
 private:
  // Makes sure an output folder exists. Recreates it when it was deleted, and
  // falls back to the default when even that fails.
  std::filesystem::path ResolveOutputFolder(std::string* configured,
                                            const std::filesystem::path& fallback);
  void StartRecording();
  void StopRecording();
  // Hands the readback frame to the recorder, if one is running.
  void FeedRecorder();
  // One place decides whether the GPU has to hand pictures back, and one place
  // hands them out -- the recorder and the camera both want the same frame, and
  // fetching it twice would give each of them every second one.
  void FeedFrameConsumers();
  void UpdateVirtualCamera();
  // Decides what curve the picture is in and what the screen should be given.
  // Runs every frame because both ends can change underneath it: a console
  // switches to HDR, or the window is dragged onto another screen.
  void UpdateHdr();
  // Opens the release in the browser, at the address the server gave for it --
  // which stays right even after the project has been renamed. Falls back to
  // the built-in one when there is no answer to take an address from.
  void OpenReleasePage(const UpdateStatus& status);
  // Same, for the website.
  void OpenWebsite();

  // `file`, if given, makes the toast clickable: a click shows that file in
  // Explorer.
  void Toast(const std::string& text, const std::filesystem::path& file = {});
  void DrawToastStrip();
  void UpdatePowerRequest();
  void SaveWindowPlacement();
  void SaveConfig();

  bool HandleKeyDown(WPARAM key);
  // Every key press from either window comes through here. `busy` is true when
  // the ImGui context of the window it arrived at wants the key for itself.
  bool OnKey(WPARAM key, LPARAM lparam, bool busy);

  HINSTANCE instance_ = nullptr;
  HWND hwnd_ = nullptr;
  bool running_ = false;
  bool minimized_ = false;
  bool fullscreen_ = false;
  WINDOWPLACEMENT windowedPlacement_ = {};
  bool imguiReady_ = false;
  bool darkMode_ = true;
  float uiScale_ = 1.0f;

  Config config_;
  D3DContext d3d_;
  VideoRenderer renderer_;

  // The application icon as a texture, for the empty state. Loaded once; zero
  // when the icon could not be read, which costs the idle screen its picture
  // and nothing else.
  ComPtr<ID3D11ShaderResourceView> idleIcon_;
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
  // Located once at startup; encoder tests run on probeThread_ and are handed
  // over through probeResult_ once probeDone_ flips.
  FfmpegInfo ffmpeg_;
  FfmpegInfo probeResult_;
  std::thread probeThread_;
  std::atomic<bool> probing_{false};
  std::atomic<bool> probeDone_{false};

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
  bool bypass_ = false;
  // Whether the bar was drawn this frame; the picture layout follows it.
  bool toolbarVisible_ = false;

  // Dragging the crop edges on the picture instead of typing four numbers.
  struct CropPick {
    bool active = false;
    ImageSettings saved;              // restored on cancel
    int left = 0, right = 0, top = 0, bottom = 0;  // source pixels
    int drag = -1;                    // 0 left, 1 right, 2 top, 3 bottom
  } cropPick_;
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
  DevicePropertyPages devicePages_;
  SettingsHost settingsHost_;
  Updater updater_;
  VirtualCamera virtualCamera_;
  // Refilled once a frame rather than allocated once a frame.
  std::vector<VirtualCamera::Consumer> virtualCameraConsumers_;
  // What woke the main loop last time round, and when it last drew. Together
  // they keep the preview paced by the picture rather than by the message
  // queue -- see the comment at the call to RenderFrame.
  unsigned long lastWait_ = 0x00000102ul;  // WAIT_TIMEOUT
  bool lastWaitHadEvent_ = false;
  int64_t lastRenderQpc_ = 0;
  int hdrDisplayPoll_ = 0;
  bool updatePromptQueued_ = false;   // waiting to be opened
  bool updatePromptRaised_ = false;   // already shown once this session
  UnfinishedSession unfinishedSession_;  // what the crash notice reports
  bool crashNoticeQueued_ = false;
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

  // Automatic video standard. Only the timestamps live here; what the card can
  // do is asked of the card each time, because it is the card that knows.
  int standardCandidate_ = -1;      // index into the candidate list, -1 = not searching
  // Wie lang die Liste beim letzten Durchgang war. Nur fuer die Einblendung --
  // "3 von 8" statt drei laufender Punkte --, deshalb hier mitgeschrieben
  // statt neu berechnet: die Liste haengt an den Faehigkeiten der Karte und
  // der Wohnregion, und die je Bild noch einmal zusammenzustellen waere Arbeit
  // fuer eine Zahl.
  int standardCandidateCount_ = 0;
  int64_t standardLostQpc_ = 0;     // when the lock was first missing
  int64_t standardNextTryQpc_ = 0;  // not before this
  // Wann die aktuell probierte Norm gesetzt wurde. Nur zum Messen: daraus wird
  // im Log die Zeit, die der Decoder zum Einfangen gebraucht hat, und die ist
  // der einzige Weg, die Wartefrist zu begruenden statt zu raten.
  int64_t standardSetQpc_ = 0;
  int standardSweeps_ = 0;          // completed passes through the list without a lock
  // Welcher der beiden Durchgaenge gerade laeuft. Falsch heisst: der schnelle,
  // in dem jeder Kandidat nur kStandardFastSeconds bekommt. Kippt am Ende eines
  // erfolglosen schnellen Durchgangs auf wahr und mit der Pause danach zurueck,
  // so dass jeder Anlauf mit dem billigen Durchgang beginnt. Siehe
  // kStandardFastSeconds fuer die Begruendung.
  bool standardPatientPass_ = false;
  // Seit wann gar kein Bild mehr ankommt, 0 = es kommt eines. Trennt die
  // ausgeschaltete Quelle (leere Bilder kommen weiter an) von der Norm, die
  // das anliegende Signal nicht dekodieren kann (es kommt nichts mehr) --
  // siehe UpdateVideoStandard. Das Logbuch dazu nur einmal je Aussetzer.
  int64_t standardStarvedSinceQpc_ = 0;
  bool standardStarvedLogged_ = false;

  // Die Quellgroesse, fuer die der gespeicherte Zuschnitt gemessen wurde.
  // 0x0 heisst: in dieser Sitzung noch kein Bild gesehen, das erste zaehlt
  // dann als der Stand, auf den sich die Zahlen beziehen. Siehe
  // UpdateCropForFormat.
  int cropFormatWidth_ = 0;
  int cropFormatHeight_ = 0;
  long standardLastGood_ = 0;       // die zuletzt eingerastete Norm, 0 = noch keine
  // Die Zeilenzahl, unter der das laufende Format ausgesucht wurde. Aus der
  // Karte beim Graphenbau, nicht aus dem Profil -- dort kann "automatisch"
  // stehen. Siehe ReleaseStandardBoundFormat.
  int appliedStandardLines_ = 0;

  // Die Norm, auf die die Profilautomatik zuletzt reagiert hat. Sie macht aus
  // einem Zustand ("es ist PAL 60") ein Ereignis ("es ist gerade PAL 60
  // geworden") -- ohne das waere jede von Hand getroffene Profilwahl im
  // naechsten Bild wieder ueberschrieben. Siehe App::UpdateProfileForStandard.
  long profileMatchedStandard_ = 0;

  // Farbpruefung. Siehe App::VerifyStandardColour.
  long colourCheckedStandard_ = 0;  // fuer diese Norm ist die Sache entschieden
  // Woran der Rundgang haengt. Steht nur fuer die Einblendung hier: sie soll
  // den gemessenen Grund nennen, und der ist an der Stelle bekannt, an der der
  // Rundgang beginnt, nicht mehr an der, an der gezeichnet wird.
  enum class ColourDoubt {
    None,    // kein Rundgang
    Pale,    // zu wenig Farbe fuer eine Norm, die stimmen koennte
    Tinted,  // Farbe da, aber sie steht auch im Schwarzen
    Manual,  // von Hand ausgeloest, ohne dass etwas dagegen sprach
  };
  ColourDoubt colourDoubt_ = ColourDoubt::None;
  // Bis zu diesem Zeitpunkt wird der Rundgang gegangen, auch wenn die
  // anliegende Norm kraeftig Farbe zeigt. Setzt RescanVideoStandard.
  //
  // Eine Frist und kein Schalter, weil der Wunsch verfallen koennen muss. Wer
  // die Taste auf einem schwarzen Bild drueckt, bekommt keinen Rundgang -- auf
  // Schwarz ist keine Norm zu erkennen, es wird gewartet --, und ein Schalter
  // stuende dann fuer immer. Zwei Minuten spaeter faengt das Bild ploetzlich an
  // durchzuschalten, und niemand weiss mehr warum.
  int64_t standardForceColourUntilQpc_ = 0;
  // Ob gerade ein von Hand ausgeloester Suchlauf laeuft, und was er ergeben
  // hat.
  //
  // Eine Suche, die von selbst anspringt, schuldet niemandem eine Antwort --
  // sie beantwortet eine Frage, die keiner gestellt hat, und wenn sie fertig
  // ist, steht das Ergebnis im Bild. Ein Tastendruck ist eine Frage, und der
  // gehoert eine Antwort: die Einblendung sagt, was gesucht wird, und
  // verschwand bisher wortlos, sobald es gefunden war. Wer die Taste drueckt,
  // weil ihm die Farben nicht gefallen, erfaehrt so nicht einmal, ob sich
  // etwas geaendert hat.
  //
  // Die Fahne haelt nur bis zur ersten Auskunft. Danach ist die Frage
  // beantwortet, und was die Automatik spaeter noch entscheidet, gehoert
  // wieder ihr -- inklusive des Toasts, den die Antwort hier so lange
  // vertritt.
  bool standardManualSearch_ = false;
  int64_t standardResultUntilQpc_ = 0;
  std::string standardResultHeadline_;
  std::string standardResultDetail_;
  // Der Rundgang durch die Normen derselben Zeilenzahl. Leer, solange keiner
  // laeuft; sonst die Kandidaten, der Zeiger auf den gerade gemessenen und die
  // Messreihen dazu (-1 = keine Messung zustande gekommen).
  std::vector<long> colourCandidates_;
  std::vector<float> colourEnergies_;
  std::vector<float> colourDarks_;
  // Und wie stark die Farbe von Zeile zu Zeile umklappt, getrennt nach Achse:
  // das Kennzeichen einer Norm, deren Dekoder die Phasenumkehr des Signals
  // nicht trifft. Siehe kAltFlipping in App::VerifyStandardColour.
  std::vector<float> colourAltV_;
  std::vector<float> colourAltU_;
  int colourIndex_ = 0;
  int64_t colourSettleUntilQpc_ = 0;    // bis dahin gehoeren die Bilder noch der alten
  int64_t colourStartedQpc_ = 0;        // seit wann auf eine Messung gewartet wird
  float colourWindowEnergy_ = -1.0f;    // das vorige Messfenster desselben Kandidaten,
  float colourWindowDark_ = -1.0f;      // zum Vergleich; -1 heisst "noch keins"
  int64_t colourRetryQpc_ = 0;          // vor diesem Zeitpunkt nicht noch einmal
  int colourAttempts_ = 0;              // unentschiedene Anlaeufe fuer diese Norm
  // Ob gerade auf ein Bild gewartet wird, das hell genug zum Vergleichen ist.
  // Nur damit die Zeile darueber einmal im Log steht statt sechzig Mal je
  // Sekunde; entschieden wird jedes Bild neu.
  bool colourWaitingForPicture_ = false;
  std::thread signalWatch_;
  std::atomic<bool> signalWatchRun_{false};
  std::atomic<int> signalLocked_{-1};
  // Die Norm, die der Wachthread im selben Atemzug wie den Lock von der Karte
  // gelesen hat. Sie steht hier und nicht in `capabilities_`, weil sie sich
  // waehrend der Suche mehrmals aendert, ohne dass der Graph neu gebaut wird --
  // und weil die beiden Angaben, aus derselben Abfrage genommen, gar nicht mehr
  // auseinanderlaufen koennen.
  std::atomic<long> signalStandard_{0};
  // Counts up every time the watcher stores a reading. The automatic search
  // notes it down when it changes the standard and then ignores anything older:
  // otherwise it judges the new standard by a measurement taken before it was
  // set, and settles on whichever one happened to be tried when a stale "locked"
  // came through.
  std::atomic<uint32_t> signalSeq_{0};
  uint32_t standardSeqAtSet_ = 0;
  std::string toastText_;
  std::filesystem::path toastFile_;  // what a click on the toast shows, empty if nothing
  bool toastHint_ = false;     // this one says that it can be clicked
  bool toastTouched_ = false;  // the mouse has moved over it since it appeared
  double toastStart_ = 0.0;
  double volumeOsdStart_ = -1000.0;
  double lastSplitCheck_ = 0.0;
  // Die Bildrate der Quelle, wie sie beim Start der Aufnahme gemessen wurde --
  // ungedoppelt, also ohne das Bobbing eingerechnet. Verglichen wird die rohe
  // Rate, damit ein Deinterlacer, den jemand mitten in der Aufnahme umstellt,
  // die Datei nicht schneidet: das ist eine Einstellung, keine andere Quelle.
  // Die Bildgroesse steht im Recorder selbst (`frameWidth`, `frameHeight`).
  double recordSourceFps_ = 0.0;
  // Was die Quelle gerade zeigt, seit wann sie es unveraendert zeigt, und ob
  // das ueberhaupt von der laufenden Datei abweicht. Ein Normsuchlauf geht
  // durch mehrere Zeilenzahlen; geschnitten wird erst, wenn eine davon stehen
  // bleibt. `pendingSince_ < 0` heisst: passt zusammen, nichts zu tun.
  int pendingWidth_ = 0;
  int pendingHeight_ = 0;
  double pendingFps_ = 0.0;
  double pendingSince_ = -1.0;
  // Platz auf dem Ziellaufwerk. Nachgesehen wird im Sekundentakt und nicht je
  // Bild: die Zahl aendert sich langsam, der Systemaufruf geht auf die Platte.
  // Siehe UpdateDiskSpace, und den Wachdienst in FeedRecorder.
  double lastDiskCheck_ = -1000.0;
  uint64_t diskFreeBytes_ = 0;
  bool diskFreeKnown_ = false;
  // Was eine Sekunde Aufnahme nach den Einstellungen kostet. Geschaetzt, solange
  // nichts laeuft; waehrend einer Aufnahme gewinnt die gemessene Schreibrate.
  double diskBytesPerSecond_ = 0.0;
  // Ob vor dem knappen Platz schon gewarnt wurde. Je Aufnahme einmal -- eine
  // Meldung je Sekunde waere keine Warnung mehr, sondern ein Dauerzustand.
  bool diskWarned_ = false;
  // lParam of the key message being handled, for telling a real key press apart
  // from a synthesised one in the log.
  uint64_t lastKeyLParam_ = 0;

  DWORD lastPowerPokeTick_ = 0;
  bool cursorHidden_ = false;
  // Es gab noch keine Konfigurationsdatei, als dieser Lauf begann. Entscheidet
  // ueber die Begruessung statt der Einstellungen.
  bool firstRun_ = false;
  int64_t lastMouseMoveQpc_ = 0;
  POINT lastMousePos_ = {};
};

}  // namespace cap
