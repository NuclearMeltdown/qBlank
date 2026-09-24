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
    virtual std::filesystem::path ResolveOutputFolder(std::string* configured,
                                                      const std::filesystem::path& fallback) = 0;
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
  // Graphics hardware plus ffmpeg build. A cached encoder test only counts when
  // this still matches.
  std::string EncoderSignature() const;
  void LoadCachedEncoders();
  void SaveCachedEncoders();
  void CollectEncoderProbe();

  void ToggleRecording();
  void StartRecording();
  void StopRecording();
  // Hands the readback frame to the recorder, if one is running.
  void FeedRecorder();
  // Fragt den freien Platz auf dem Aufnahmelaufwerk ab, hoechstens einmal je
  // Sekunde. Die Einstellungen zeigen ihn an, die laufende Aufnahme haengt
  // daran.
  void UpdateDiskSpace();
  // Starts or stops the microphone to match the settings and what is going on.
  // `aboutToRecord` starts it for a recording that has not begun yet -- the
  // sample rate has to be known before ffmpeg is given its command line.
  void SyncMicrophone(bool aboutToRecord = false);

  // The ffmpeg in use. Located at startup, replaced when it has gone missing
  // or a download brought a new one.
  FfmpegInfo& ffmpeg() { return ffmpeg_; }
  bool probing() const { return probing_.load(std::memory_order_relaxed); }

 private:
  Config& config_;
  Recorder& recorder_;
  VideoRenderer& renderer_;
  VideoCapture& capture_;
  AudioEngine& audio_;
  MicCapture& mic_;
  SettingsWindow& settings_;
  CameraSink& virtualCamera_;
  Host& host_;

  // Located once at startup; encoder tests run on probeThread_ and are handed
  // over through probeResult_ once probeDone_ flips.
  FfmpegInfo ffmpeg_;
  FfmpegInfo probeResult_;
  std::thread probeThread_;
  std::atomic<bool> probing_{false};
  std::atomic<bool> probeDone_{false};
  // What SyncMicrophone last acted on, so a failure is reported once instead of
  // once per frame. Cleared when the settings change.
  DeviceRef micApplied_;
  bool micAttempted_ = false;
  bool micFailed_ = false;
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
};

}  // namespace cap
