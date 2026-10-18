#pragma once

// Recording: ffmpeg and its encoder test, starting and stopping, cutting a new
// file when the source changes shape or a size limit is reached, the free disk
// space, and the microphone that goes with a recording.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>

#include "config.h"
#include "files.h"
#include "job_thread.h"
#include "record/ffmpeg_locator.h"

namespace cap {

class AudioEngine;
class CameraSink;
class MicCapture;
class Recorder;
class SettingsWindow;
class VideoCapture;
class VideoRenderer;

class RecordingControl {
 public:
  // What recording needs from the application around it.
  class Host {
   public:
    virtual ~Host() = default;
    virtual bool CaptureRunning() const = 0;
    // The folder `asked` for could not be made and the default is used instead:
    // says so, and empties `configured` if it still names `asked`.
    virtual void UseDefaultFolder(std::string* configured, const std::string& asked) = 0;
    // Freeze, compare and bypass would all end up in the file, so a recording
    // switches them off as it starts.
    virtual void DropViewAids() = 0;
    virtual void Toast(const std::string& text, const std::filesystem::path& file = {}) = 0;
  };

  RecordingControl(Config& config, Recorder& recorder, VideoRenderer& renderer,
                   VideoCapture& capture, AudioEngine& audio, MicCapture& mic,
                   SettingsWindow& settings, CameraSink& camera, Host& host);

  // Stops a running recording and waits for an encoder test still under way.
  void Shutdown();

  // Test encodes take seconds and must never run on the UI thread -- doing so
  // froze the window long enough to drop displayed frames.
  void StartEncoderProbe(bool full);
  // Takes over what the worker thread finished. True when that was a search
  // for ffmpeg rather than an encoder test.
  bool CollectEncoderProbe();
  // Finds ffmpeg again -- at startup, after a download put a new one in place,
  // after the path was changed -- so the version shown is the one on disk and
  // the old build's encoder test no longer counts for it. On the worker
  // thread: "ffmpeg -version" takes a moment, and on the UI thread that held
  // up the picture. False while that has to wait -- an encoder test would
  // bring the old one back when it lands, and a recording gets the machine to
  // itself.
  bool RelocateFfmpeg();

  void ToggleRecording();
  // Checks what can be checked at once and then hands over to LaunchRecording,
  // which waits for the things that take a moment -- the disk, a microphone
  // being opened, ffmpeg being searched for -- without holding up a single
  // frame.
  void StartRecording();
  // A recording asked for and not running yet. Frames have to go on arriving
  // for it, minimised or not: it launches from FeedRecorder.
  bool starting() const { return startPending_; }
  // Returns before ffmpeg has finished the file. `announce` toasts it as saved
  // once that happened, see CollectFinished. A start still waiting is called
  // off.
  void StopRecording(bool announce = false);
  // Hands the readback frame to the recorder, if one is running.
  void FeedRecorder();
  // Fragt den freien Platz auf dem Aufnahmelaufwerk ab, hoechstens einmal je
  // Sekunde, und die Groesse der laufenden Datei, wenn nach Groesse geteilt
  // wird. Die Einstellungen zeigen den Platz an, die laufende Aufnahme haengt
  // an beidem. Die Antwort kommt vom Platten-Worker, ein Bild spaeter.
  void UpdateDiskSpace();
  // Starts or stops the microphone to match the settings and what is going on,
  // a recording that is about to begin included: its sample rate has to be
  // known before ffmpeg is given its command line.
  void SyncMicrophone();

  // The ffmpeg in use. Located at startup, replaced when it has gone missing
  // or a download brought a new one.
  FfmpegInfo& ffmpeg() { return ffmpeg_; }
  bool probing() const { return probing_.load(std::memory_order_relaxed); }

 private:
  // What the disk said about a start, on disk_.
  struct StartCheck {
    bool ffmpegThere = false;
    FolderChoice folder;
    bool freeKnown = false;
    uint64_t freeBytes = 0;
    std::filesystem::path file;
  };

  // A cached encoder test only counts while probeSignature_ still matches.
  void LoadCachedEncoders();
  void SaveCachedEncoders();
  void CollectFinished();
  // StartRecording's checks on the disk came back.
  void StartChecked(uint32_t serial, const std::string& askedFolder, const StartCheck& check);
  // A start called off before it launched.
  void CancelStart();
  // The second half of StartRecording, once the disk and the microphone have
  // answered.
  void LaunchRecording();

  Config& config_;
  Recorder& recorder_;
  VideoRenderer& renderer_;
  VideoCapture& capture_;
  AudioEngine& audio_;
  MicCapture& mic_;
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
  // microphone. The file is empty until the disk has answered. Counted, so an
  // answer meant for a start that was called off is not taken for the next.
  // UI thread only.
  bool startPending_ = false;
  RecordSettings pendingSettings_;
  bool pendingAudio_ = false;
  std::filesystem::path pendingFile_;
  uint32_t startSerial_ = 0;
  // F9 found no ffmpeg and the search for it is still out; it starts once that
  // came back with one.
  bool recordWhenLocated_ = false;
  // What SyncMicrophone last acted on, so a failure is reported once instead of
  // once per frame. Cleared when the settings change.
  DeviceRef micApplied_;
  bool micFailed_ = false;
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
  bool diskAsked_ = false;
  uint64_t diskFreeBytes_ = 0;
  bool diskFreeKnown_ = false;
  // Was eine Sekunde Aufnahme nach den Einstellungen kostet. Geschaetzt, solange
  // nichts laeuft; waehrend einer Aufnahme gewinnt die gemessene Schreibrate.
  double diskBytesPerSecond_ = 0.0;
  // Ob vor dem knappen Platz schon gewarnt wurde. Je Aufnahme einmal -- eine
  // Meldung je Sekunde waere keine Warnung mehr, sondern ein Dauerzustand.
  bool diskWarned_ = false;

  // Everything that asks the disk the recording goes to: the checks before a
  // start, the free space, the size of the file. One thread, because all of it
  // waits on the same drive; nothing else waits on it. Last, because what it
  // hands back uses the members above.
  JobThread disk_;
};

}  // namespace cap
