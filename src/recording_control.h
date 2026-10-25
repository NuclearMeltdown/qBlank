#pragma once

// Recording: ffmpeg and its encoder test, starting and stopping, cutting a new
// file when the source changes shape or a size limit is reached, the free disk
// space, and the sound sources that go with a recording.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "files.h"
#include "job_thread.h"
#include "record/ffmpeg_locator.h"

namespace cap {

class AudioEngine;
class CameraSink;
class Recorder;
enum class RecordingKind;
class SettingsWindow;
class SourceCapture;
class VideoCapture;
class VideoRenderer;

class RecordingControl {
 public:
  // What recording needs from the application around it.
  class Host {
   public:
    virtual ~Host() = default;
    virtual bool CaptureRunning() const = 0;
    // The screen is the source: no card frames to record unfiltered, no fields,
    // and a rate that is its refresh rate rather than what arrives.
    virtual bool OnScreen() const = 0;
    // The folder `asked` for could not be made and the default is used instead:
    // says so, and empties `configured` if it still names `asked`.
    virtual void UseDefaultFolder(std::string* configured, const std::string& asked) = 0;
    // Freeze, compare and bypass would all end up in the file, so a recording
    // switches them off as it starts.
    virtual void DropViewAids() = 0;
    virtual void Toast(const std::string& text, const std::filesystem::path& file = {}) = 0;
    // Something going wrong with the recording: stands longer and stands out.
    virtual void Warn(const std::string& text) = 0;
  };

  RecordingControl(Config& config, Recorder& recorder, VideoRenderer& renderer,
                   VideoCapture& capture, AudioEngine& audio, SettingsWindow& settings,
                   CameraSink& camera, Host& host);
  ~RecordingControl();

  // Stops a running recording and waits for an encoder test still under way.
  void Shutdown();

  // Test encodes take seconds and must never run on the UI thread -- doing so
  // froze the window long enough to drop displayed frames.
  void StartEncoderProbe(bool full);
  // Takes over what the worker thread finished. True when that was loading
  // ffmpeg rather than an encoder test.
  bool CollectEncoderProbe();
  // Loads the ffmpeg libraries -- at startup, after a download put them in
  // place -- so the version shown is the one in use and another build's encoder
  // test no longer counts for it. On the worker thread: loading takes a moment,
  // and the picture does not wait for it. False while that has to wait -- an
  // encoder test would bring the old result back when it lands, and a
  // recording gets the machine to itself.
  bool RelocateFfmpeg();

  void ToggleRecording();
  // Checks what can be checked at once and then hands over to LaunchRecording,
  // which waits for the things that take a moment -- the disk, the sound
  // sources being opened, ffmpeg being loaded -- without holding up a single
  // frame. With the replay running on the same settings, the file goes onto
  // its encoders instead of starting a second set.
  void StartRecording();
  // A recording or the replay asked for and not running yet. Frames have to go
  // on arriving for it, minimised or not: it launches from FeedRecorder.
  bool starting() const { return startPending_; }
  // Returns before the file is finished. `announce` toasts it as saved
  // once that happened, see CollectFinished. A start still waiting is called
  // off. The replay runs on.
  void StopRecording(bool announce = false);

  // The replay (RecordSettings::replay): switched on and off for good, and
  // kept running from FeedRecorder while it is on, beside a recording or
  // without one.
  void ToggleReplay();
  // Writes what the replay holds into clips\, on the encoders already
  // running: nothing is encoded twice.
  void SaveReplay();
  // A chapter at this moment in the file being written, and in any clip
  // that holds it (Recorder::AddMarker).
  void AddMarker();
  // The replay is on and has not failed: it needs the pictures, minimised or
  // not, to start and to fill.
  bool replayWanted() const { return config_.record.replay && !replaySuspended_; }
  // Hands the readback frame to the recorder, if one is running.
  void FeedRecorder();
  // The running recording left frames out (Recorder::leftOut) in the last few
  // seconds, as FeedRecorder last saw it.
  bool leftOutLately() const;
  // Fragt den freien Platz auf dem Aufnahmelaufwerk ab, hoechstens einmal je
  // Sekunde, und die Groesse der laufenden Datei, wenn nach Groesse geteilt
  // wird. Die Einstellungen zeigen den Platz an, die laufende Aufnahme haengt
  // an beidem. Die Antwort kommt vom Platten-Worker, ein Bild spaeter.
  void UpdateDiskSpace();
  // Starts or stops every recording source (AudioSettings::sources) to match
  // the settings and what is going on, a recording that is about to begin
  // included: their sample rates have to be known before the recorder opens
  // its tracks. Hands their levels to the settings.
  void SyncSources();

  // The ffmpeg in use. Loaded at startup, or once a download brought it.
  FfmpegInfo& ffmpeg() { return ffmpeg_; }
  bool probing() const { return probing_.load(std::memory_order_relaxed); }

 private:
  // What the disk said about a start, on disk_.
  struct StartCheck {
    FolderChoice folder;
    bool freeKnown = false;
    uint64_t freeBytes = 0;
    std::filesystem::path file;
  };

  // A cached encoder test only counts while probeSignature_ still matches.
  void LoadCachedEncoders();
  void SaveCachedEncoders();
  void CollectFinished();
  // The record settings a start goes by: the encoder resolved, and the
  // container a lossless one brings along.
  RecordSettings WantedSettings(const EncoderInfo& encoder) const;
  // Whether a running session's encoders can take a file for `wanted` as they
  // are. The buffer and the replay's own numbers do not count: those change
  // live. `wanted` may be config_.record itself, the container not yet chosen.
  bool SameSession(const RecordSettings& wanted) const;
  // The sound sources are no longer the ones the session opened its tracks and
  // resamplers for, in the settings or at the tap; it cannot follow that.
  bool SourcesChanged() const;
  // The part every start shares: what to wait for, and the disk asked for the
  // file when there is to be one. `onSession` puts the file on the encoders
  // already running.
  void BeginStart(const RecordSettings& settings, RecordingKind kind, bool withFile,
                  bool onSession);
  // StartRecording's checks on the disk came back.
  void StartChecked(uint32_t serial, const std::string& askedFolder, const StartCheck& check);
  // A start called off before it launched.
  void CancelStart();
  // The second half of StartRecording, once the disk and the sources have
  // answered.
  void LaunchRecording();
  // The waiting file onto the running encoders; one being written ends where
  // it begins.
  void AttachFile();
  // A new file for the size limit, on the same encoders.
  void SplitFile();
  // Ends the encoders: the file, the replay, all of it.
  void StopSession();
  // Brings the replay in line with the settings, once a frame: on the
  // encoders that run, or on its own when none do and it can start.
  void EnsureReplay();
  bool SourcesOpening() const;
  // The source's frame rate: measured where there is a measurement, which
  // beats what the format says (LaunchRecording).
  double SourceFps() const;

  Config& config_;
  Recorder& recorder_;
  VideoRenderer& renderer_;
  VideoCapture& capture_;
  AudioEngine& audio_;
  SettingsWindow& settings_;
  CameraSink& virtualCamera_;
  Host& host_;

  // The search and the encoder tests run on probeThread_, one at a time, and
  // are handed over through probeResult_ once probeDone_ flips.
  FfmpegInfo ffmpeg_;
  FfmpegInfo probeResult_;
  std::thread probeThread_;
  std::atomic<bool> probing_{false};
  std::atomic<bool> probeDone_{false};
  // Graphics hardware plus ffmpeg build, worked out on probeThread_ along with
  // its result: creating a DXGI factory to enumerate the adapters is not for
  // the thread that draws.
  std::string probeSignature_;
  // What probeThread_ is doing: searching rather than testing. UI thread only.
  bool locating_ = false;
  // A recording that has passed the checks that need nothing but memory, with
  // the settings it passed them with, and waits for the disk and the
  // recording sources. The file is empty until the disk has answered. Counted, so an
  // answer meant for a start that was called off is not taken for the next.
  // UI thread only.
  // `pendingWithFile_` is false for the replay alone, which waits for no disk;
  // `pendingOnSession_` puts the file on the encoders that run.
  bool startPending_ = false;
  bool pendingWithFile_ = false;
  bool pendingOnSession_ = false;
  RecordSettings pendingSettings_;
  bool pendingAudio_ = false;
  std::filesystem::path pendingFile_;
  uint32_t startSerial_ = 0;
  // What the running encoders were opened with (LaunchRecording), and a
  // count of them, so a clip asked for before a restart is not saved from the
  // one after.
  RecordSettings sessionSettings_;
  bool sessionHdrAsked_ = false;
  std::vector<RecordSource> sessionSources_;
  int sessionTapRate_ = 0;
  uint32_t sessionSerial_ = 0;
  // The replay failed and stays off until it is switched on again, rather
  // than failing once a frame. `replaySeen_` is RecordSettings::replay as
  // EnsureReplay last saw it, so the settings' checkbox counts as that too.
  bool replaySuspended_ = false;
  bool replaySeen_ = false;
  // What SetReplay was last given.
  int replaySecondsSet_ = 0;
  int replayGbSet_ = 0;
  bool replayOnDiskSet_ = false;
  std::string replayFolderSet_;  // RecordSettings::outputFolder, then
  // No usable encoder for the replay: one test asked for, not one a frame.
  bool replayProbeAsked_ = false;
  // F9 found no ffmpeg and loading it is still under way; it starts once that
  // came back with it.
  bool recordWhenLocated_ = false;
  // One capture per entry of AudioSettings::sources, by index, with what
  // SyncSources last opened it for: a failure is reported once instead of once
  // per frame, and cleared when the entry changes. Never shrinks -- letting a
  // capture go waits for its thread, and the recorder may still read from it.
  struct Source {
    std::unique_ptr<SourceCapture> capture;
    SourceKind kind = SourceKind::Input;
    DeviceRef device;
    bool failed = false;
  };
  std::vector<Source> sources_;
  // The file being written and how big it was when last looked at, for
  // splitting by size.
  std::filesystem::path recordingFile_;
  uint64_t recordingBytes_ = 0;
  // The file a stop by hand is waiting to call saved.
  std::string announceFile_;
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
  // Siehe UpdateDiskSpace, und den Wachdienst in FeedRecorder. Eine Abfrage
  // auf einmal: eine Platte, die erst anlaufen muss, soll keine Schlange
  // bekommen.
  double lastDiskCheck_ = -1000.0;
  double lastMemoryCheck_ = -1000.0;
  bool diskAsked_ = false;
  uint64_t diskFreeBytes_ = 0;
  bool diskFreeKnown_ = false;
  // What the replay holds on that disk, and would give up.
  uint64_t replayDiskBytes_ = 0;
  // Was eine Sekunde Aufnahme nach den Einstellungen kostet. Geschaetzt, solange
  // nichts laeuft; waehrend einer Aufnahme gewinnt die gemessene Schreibrate.
  double diskBytesPerSecond_ = 0.0;
  // Ob vor dem knappen Platz schon gewarnt wurde. Je Aufnahme einmal -- eine
  // Meldung je Sekunde waere keine Warnung mehr, sondern ein Dauerzustand.
  bool diskWarned_ = false;
  bool encoderWarned_ = false;
  bool bufferWarned_ = false;
  // Recorder::leftOut as last seen, and when it last grew.
  uint64_t leftOutSeen_ = 0;
  double leftOutAt_ = 0.0;

  // Everything that asks the disk the recording goes to: the checks before a
  // start, the free space, the size of the file. One thread, because all of it
  // waits on the same drive; nothing else waits on it. Last, because what it
  // hands back uses the members above.
  JobThread disk_;
};

}  // namespace cap
