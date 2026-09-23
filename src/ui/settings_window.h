#pragma once

// The settings dialog.
//
// It edits the live configuration in place, so every change takes effect on the
// spot -- there is no Apply button and nothing to confirm. A snapshot is taken
// when the dialog opens, which is what "Discard" restores.

#include <filesystem>
#include <string>
#include <vector>

struct ImGuiContext;

#include "audio/audio_devices.h"
#include "capture/video_capture.h"
#include "update/updater.h"
#include "config.h"
#include "record/ffmpeg_download.h"
#include "record/ffmpeg_locator.h"
#include "record/remuxer.h"
#include "ui/file_dialog.h"
#include "ui/settings_search.h"
#include "camera_sink.h"
#include "window.h"

namespace cap {

struct MonitorInfoEntry {
  int index = 0;
  std::string name;
  Rect rect;
  bool primary = false;
};

std::vector<MonitorInfoEntry> EnumerateMonitors();

class SettingsWindow {
 public:
  enum class Result { None, Close };

  // `live` must outlive the dialog; it is edited directly. `reason` is shown as
  // a banner, e.g. why the dialog opened by itself.
  void Open(Config* live, const std::string& reason = {});
  void Close();
  // Drops the banner; called once the condition that raised it is resolved.
  void ClearReason() { reason_.clear(); }
  // Reloads the folder text fields from the config, after something outside the
  // dialog changed them.
  void InvalidateFolderFields() { recordBuffersLoaded_ = false; }

  // What the automatic level detection settled on, shown next to the range
  // selector so "Automatic" is not a black box. Points at a static string owned
  // by the caller, or null while nothing has been measured.
  void SetDetectedRange(const char* const* text) { detectedRange_ = text; }
  // Die Rohzahlen dahinter, schon fertig gesetzt, oder ein leerer Text. Sie
  // stehen klein unter dem Urteil, weil das Urteil zwei Faelle zusammenfasst,
  // die beim Einstellen der Karte gerade auseinandergehalten werden muessen:
  // Schwarz bei 0 und Schwarz bei 16 heissen beide "erkannt", meinen aber
  // Verschiedenes. Zeigt auf einen Text, der dem Aufrufer gehoert.
  void SetRangeNumbers(const std::string* text) { rangeNumbers_ = text; }
  // Gesetzt, wenn der Benutzer die Bereichsmessung von vorn haben will. Das
  // Urteil friert sonst ein, bis sich das Format aendert -- und eine
  // umgestellte Treiberoption aendert das Format nicht.
  bool takeRangeRemeasureRequest();
  void SetDetectedInterlace(const char* const* text) { detectedInterlace_ = text; }
  // Ob neben der Messung der Satz stehen soll, dass sie hier vermutlich irrt.
  // Die Bedingungen dafuer stehen bei App::InterlaceVerdictDoubtful.
  void SetInterlaceDoubtful(bool doubtful) { interlaceDoubtful_ = doubtful; }
  // Ob unter dem Wertebereich der Hinweis stehen soll, dass an einem analogen
  // Eingang der volle Bereich ankommt. Die Bedingungen dafuer stehen bei
  // App::AnalogueRangeIsFull.
  void SetAnalogueFullRange(bool full) { analogueFullRange_ = full; }
  // Die sichtbaren Zeilen der anliegenden Norm, wenn die laufende Aufloesung
  // nicht dazu passt; 0 sonst. Die Bedingungen stehen bei
  // App::ResolutionMismatchLines.
  void SetResolutionMismatch(int activeLines) { resolutionMismatch_ = activeLines; }
  void SetCoSitedFields(bool on) { coSitedFields_ = on; }
  // False while the app is itself trying to open the card. Probing means
  // building a second graph on the same device, and doing that while the first
  // one is being retried puts two of them on one card -- which the driver is
  // under no obligation to survive.
  void SetProbeAllowed(bool on) { probeAllowed_ = on; }

  // The updater lives in the app; the dialog only drives it and shows what it
  // reports.
  void SetUpdater(Updater* updater) { updater_ = updater; }
  // Set when the user asked to restart into a freshly installed build.
  bool takeRestartRequest();
  // 1 locked, 0 not, -1 unknown. Shown next to the video standard, because that
  // is the one number that says whether the setting is the right one.
  void SetSignalLocked(int locked) { signalLocked_ = locked; }

  // Was die automatische Normensuche gerade tut. Der Dialog wuerde es sonst
  // nicht erfahren: die Suche laeuft im Hintergrund und stellt die Karte
  // waehrenddessen mehrmals um.
  enum class StandardSearch {
    Off,      // nichts zu suchen -- eingerastet, oder die Norm steht fest
    Trying,   // ein Kandidat steht auf der Karte und hat noch nicht eingerastet
    Paused,   // eine Runde ohne Lock; die Karte ist geparkt, es wird gewartet
    Colour,   // eingerastet, aber gerade wird die Farbe gegengeprueft
    Result,   // fertig -- und weil von Hand gesucht wurde, steht das Ergebnis noch
  };
  void SetStandardSearch(StandardSearch state) { standardSearch_ = state; }
  // Die Norm, die *jetzt* auf der Karte steht, aus derselben Abfrage wie der
  // Lock. Nicht dasselbe wie `caps.currentStandard`: das ist der Stand des
  // letzten Graphenbaus, und die Suche baut den Graphen nicht neu, wenn sie
  // eine Norm nur ausprobiert. Genau daran zeigte der Dialog waehrend der
  // Suche die vorletzte Antwort und konnte nach einer Farbberichtigung sogar
  // dauerhaft eine andere Norm nennen als die, die wirklich eingerastet war.
  // 0 heisst: nichts laeuft, dann bleibt `caps` die beste Auskunft.
  void SetLiveStandard(long standard) { liveStandard_ = standard; }
  // Whether the running source is being treated as analogue. Decides which of
  // the picture settings are worth showing at all.
  void SetAnalogueSource(bool on) { analogueSource_ = on; }
  // Welcher Anschluss dabei wirklich gilt -- also mit "Automatisch" schon
  // aufgeloest. Die Auswahl steht im Reiter Quelle, aber gebraucht wird sie im
  // Reiter Bild: die halbe Filterkette dort gibt es nur, weil Composite Luma
  // und Chroma auf einer Leitung fuehrt. Siehe App::ResolvedConnector.
  void SetConnector(AnalogConnector c) { connector_ = c; }

  // Frame rate the card is currently delivering; caps the recording rate.
  void SetSourceFps(double fps) { sourceFps_ = fps; }
  // Wie gross das Quellbild ist und ob es Halbbilder hat. Entscheidet, welche
  // Abschnitte im Reiter Bild ueberhaupt erscheinen -- siehe DrawImageTab.
  void SetSourceHeight(int lines) { sourceHeight_ = lines; }
  void SetSourceInterlaced(bool on) { sourceInterlaced_ = on; }
  // Ausgabezeilen je Bildzeile, siehe VideoRenderer::scanlineRoom. Unter zwei
  // sagt der Regler fuer die Zeilenluecken selbst, dass nichts zu holen ist.
  void SetScanlineRoom(float rows) { scanlineRoom_ = rows; }

  // Current input levels, 0..1, for the meters on the audio tab.
  void SetLevels(float input, float mic, bool micRunning) {
    inputPeak_ = input;
    micPeak_ = mic;
    micRunning_ = micRunning;
  }
  // Vergleich und "alle Filter aus" gehoeren der App und nicht dem Profil, also
  // zeigt der Dialog sie nur und bittet ums Umschalten -- dann gelten auch
  // dieselben Sperren wie bei der Taste.
  void SetViewAids(bool compare, bool bypass) {
    compareOn_ = compare;
    bypassOn_ = bypass;
  }
  bool takeCompareToggle();
  bool takeBypassToggle();
  bool isOpen() const { return open_; }

  // `liveCaps` are the capabilities of the device that is currently running, so
  // the dialog does not have to reopen a busy card. May be null. `ffmpeg` is the
  // app's shared state and is updated in place when the user tests encoders or
  // downloads a build.
  Result Draw(const DeviceProbeResult* liveCaps, FfmpegInfo* ffmpeg);

  // Draws the same contents filling the whole viewport, with no frame of its
  // own -- for when the dialog *is* the window rather than a panel inside one.
  void SetFillsWindow(bool on) { fillsWindow_ = on; }

  // Beim naechsten Zeichnen die gemerkte Lage wieder einnehmen. Wird beim
  // Wechsel zwischen eingebettet und freigestellt gerufen, damit das Feld dort
  // aufgeht, wo das Fenster gerade stand -- und nicht dort, wo es selbst
  // zuletzt lag.
  void RestorePosition() { restorePos_ = true; }

  // Set by the dialog when the user asks for a full encoder test; the app runs
  // it in the background and clears the flag.
  bool takeProbeRequest();
  // Set when the user wants to drag the crop on the picture; the app closes the
  // dialog and takes over.
  bool takeCropPickRequest();
  // Set when the user asks for the capture driver's own settings dialog.
  bool takeDeviceConfigRequest();
  // Set when the user wants the black border measured and cropped away.
  bool takeCropDetectRequest();
  // Set when the user wants the card opened from scratch, forgetting what was
  // measured and chosen for the input that was plugged in before.
  bool takeCardResetRequest();

  // The virtual camera. 0 = nothing wanted, 1 = install the source, 2 = remove
  // it. Both raise a UAC prompt, so the app does it rather than the UI thread.
  int takeVirtualCameraRequest();
  // What the app knows and the settings cannot see for themselves. One entry
  // per application reading the camera, each with the format it negotiated for
  // itself -- which is the shape of the thing now rather than a detail: there
  // is one filter instance per consumer and they routinely disagree.
  void SetVirtualCameraState(bool running, const std::vector<CameraSink::Consumer>& consumers);
  void SetCarrierPeriod(float samples) { carrierPeriod_ = samples > 1.5f ? samples : 3.045f; }

  // Platz auf dem Ziellaufwerk, von der App gemessen statt hier: sie sieht
  // ohnehin waehrend der Aufnahme hin, und ein Systemaufruf je gezeichnetem
  // Bild waere fuer eine Zahl, die sich langsam aendert, reine Verschwendung.
  // `bytes` 0 bei unbekanntem Ziel. `bytesPerSecond` kommt ebenfalls von dort,
  // weil nur die App weiss, wie viele Tonspuren gerade zusammenkaemen.
  void SetDiskFree(uint64_t bytes, bool known, double bytesPerSecond) {
    diskFree_ = bytes;
    diskKnown_ = known;
    diskBytesPerSecond_ = bytesPerSecond;
  }

  void SetHdrState(bool displayCapable, bool outputActive, float displayPeak, int sourceTransfer) {
    hdrDisplayCapable_ = displayCapable;
    hdrOutputActive_ = outputActive;
    hdrDisplayPeak_ = displayPeak;
    hdrSourceTransfer_ = sourceTransfer;
  }

  // True while the binding editor is waiting for a key press. The app routes
  // key messages here instead of acting on them.
  bool waitingForKey() const { return captureAction_ >= 0; }
  void OfferKey(Key key, bool ctrl, bool shift, bool alt);

  // Forces the device and format lists to be read again on the next frame.
  void InvalidateDeviceLists();

 private:
  void RefreshDeviceLists();
  const DeviceProbeResult& CapsFor(const DeviceRef& device, const DeviceProbeResult* liveCaps);
  void DrawSourceTab(const DeviceProbeResult& caps);
  void DrawImageTab();
  void DrawAudioTab();
  void DrawDisplayTab();
  void DrawHdrBlock();
  void DrawRecordTab(FfmpegInfo* ffmpeg);
  void DrawFfmpegBlock(FfmpegInfo* ffmpeg);
  void DrawEncoderTab(FfmpegInfo* ffmpeg);
  void DrawHdrTab();
  // The three text buffers the recording and encoder tabs share. Filled once
  // from the configuration; both tabs need them and either may be first.
  void LoadRecordBuffers();
  void DrawVirtualCameraBlock();
  void DrawEncoderBlock(const EncoderInfo* encoder);
  void DrawHotkeysTab();
  void DrawUpdatesTab();
  // Text field plus Browse / Default / Open, shared by both output folders.
  void FolderRow(const char* id, int pickTag, char* buffer, size_t bufferSize,
                 std::string* value, const std::filesystem::path& defaultFolder);
  // Applies a finished file dialog to whichever field opened it.
  void PollFileDialog(FfmpegInfo* ffmpeg);
 public:
  void setProbeBusy(bool busy) { probeBusy_ = busy; }
 private:
  void DrawProfilesTab(const DeviceProbeResult& caps);
  void EnsureValidFormat(const DeviceProbeResult& caps);

  // Search. The field sits above the tabs; while it holds text the results take
  // the tabs' place, and picking one opens its tab and scrolls to it.
  void DrawSearchField();
  bool DrawSearchResults(float footer);  // false when the query is empty
  void PickSearchHit(int entry);
  // Placed right after a control the search can find. Scrolls to it and
  // flashes it when it is the one that was picked.
  void Anchor(const char* key);

  Config& cfg() { return *live_; }

  bool open_ = false;
  bool restorePos_ = true;
  float panelScale_ = 0.0f;  // DPI scale the embedded panel was last laid out at
  std::string reason_;
  Config* live_ = nullptr;
  Config snapshot_;  // state when the dialog opened, for Discard

  bool listsValid_ = false;
  std::vector<VideoDeviceInfo> videoDevices_;
  std::vector<AudioDeviceInfo> audioInputs_;
  std::vector<AudioDeviceInfo> audioOutputs_;
  std::vector<MonitorInfoEntry> monitors_;

  // Capabilities of the device the dialog last probed, keyed by its id.
  std::string probedId_;
  DeviceProbeResult probed_;

  // Resolved embedded audio device for the current video device, for display.
  std::string embeddedAudioName_;
  std::string embeddedAudioForDevice_;

  // Custom format entry.
  bool customFormat_ = false;
  int customWidth_ = 1920;
  int customHeight_ = 1080;
  double customFps_ = 60.0;

  char renameBuffer_[64] = {};
  int renameTarget_ = -1;

  // Recording tab.
  FfmpegDownloader downloader_;
  Remuxer remuxer_;

  // One dialog at a time, tagged so the result finds its way back. The tags are
  // PickTarget values.
  AsyncFileDialog picker_;
  enum PickTarget { kPickNone = 0, kPickRecordFolder, kPickShotFolder, kPickFfmpeg, kPickRemux };
  bool probeRequested_ = false;
  bool cropPickRequested_ = false;
  bool probeBusy_ = false;
  char folderBuffer_[512] = {};
  char ffmpegPathBuffer_[512] = {};
  char shotFolderBuffer_[512] = {};
  bool recordBuffersLoaded_ = false;

  // Index into Hotkeys::items currently being rebound, -1 when idle.
  int captureAction_ = -1;
  const char* const* detectedRange_ = nullptr;
  const std::string* rangeNumbers_ = nullptr;
  bool rangeRemeasureRequested_ = false;
  const char* const* detectedInterlace_ = nullptr;
  bool interlaceDoubtful_ = false;
  bool analogueFullRange_ = false;
  int resolutionMismatch_ = 0;
  bool fillsWindow_ = false;
  bool probeAllowed_ = true;
  Updater* updater_ = nullptr;
  bool restartRequested_ = false;
  bool coSitedFields_ = false;
  int signalLocked_ = -1;
  StandardSearch standardSearch_ = StandardSearch::Off;
  long liveStandard_ = 0;
  bool analogueSource_ = true;
  AnalogConnector connector_ = AnalogConnector::Composite;
  // Kurz fuer "analog *und* auf einer Leitung": nur dann sind die
  // Composite-Filter ueberhaupt eine Frage.
  bool CompositeSource() const {
    return analogueSource_ && connector_ == AnalogConnector::Composite;
  }
  bool captureRunning_ = false;
  bool deviceConfigRequested_ = false;
  bool cropDetectRequested_ = false;
  bool cardResetRequested_ = false;
  bool compareOn_ = false;
  bool bypassOn_ = false;
  bool compareToggleRequested_ = false;
  bool bypassToggleRequested_ = false;
  // Setzt den Eingabecursor beim Oeffnen des Namensdialogs ins Textfeld, aber
  // nur einmal -- sonst finge es jedes Bild die Eingabe neu ein.
  bool namePopupFocus_ = false;
  int virtualCameraRequest_ = 0;
  int vcamStatus_ = 0;
  double vcamStatusChecked_ = -10.0;
  bool vcamRunning_ = false;
  std::vector<CameraSink::Consumer> vcamConsumers_;
  // What the dot crawl slider currently amounts to. Passed in because it
  // depends on the video standard and the captured width, neither of which the
  // settings know.
  float carrierPeriod_ = 3.045f;
  // Siehe SetDiskFree.
  uint64_t diskFree_ = 0;
  bool diskKnown_ = false;
  double diskBytesPerSecond_ = 0.0;
  // Which tab is open, carried across the two ImGui contexts by hand.
  ImGuiContext* tabContext_ = nullptr;
  int activeTab_ = 0;
  bool tabRestored_ = false;
  int wantTab_ = -1;
  char searchBuf_[96] = {};
  std::string searchQuery_;  // what searchRows_ was computed for
  std::vector<int> searchRows_;  // entries in the order they are listed
  int searchDirectRows_ = 0;     // how many of them matched the label itself
  int searchSel_ = 0;
  bool focusSearch_ = false;
  const char* jumpKey_ = nullptr;  // waiting to be drawn
  const char* jumpLabel_ = nullptr;
  int jumpWait_ = 0;
  const char* flashKey_ = nullptr;  // drawn and being flashed
  double flashStart_ = 0.0;
  std::string searchNote_;
  double searchNoteTime_ = -10.0;
  // What the app knows about the screen and the source; the settings cannot
  // ask DXGI themselves.
  bool hdrDisplayCapable_ = false;
  bool hdrOutputActive_ = false;
  float hdrDisplayPeak_ = 100.0f;
  int hdrSourceTransfer_ = 0;  // 0 SDR, 1 PQ, 2 HLG
  double sourceFps_ = 0.0;
  int sourceHeight_ = 0;  // 0 = noch nichts gemessen
  bool sourceInterlaced_ = false;
  float scanlineRoom_ = 0.0f;
  float inputPeak_ = 0.0f;
  float micPeak_ = 0.0f;
  bool micRunning_ = false;
};

}  // namespace cap
