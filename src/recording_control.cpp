#include "recording_control.h"

#include <cmath>

#include "audio/audio_engine.h"
#include "audio/source_capture.h"
#include "camera_sink.h"
#include "capture/video_capture.h"
#include "common.h"
#include "files.h"
#include "i18n.h"
#include "platform.h"
#include "record/recorder.h"
#include "record/track_plan.h"
#include "render/display.h"
#include "render/video_renderer.h"
#include "ui/settings_window.h"

namespace cap {
namespace {

// Wo eine Aufnahme nicht mehr anfaengt und eine laufende aufhoert. Ein fester
// Boden neben der Restzeit: unter ein paar hundert Megabyte wird Windows selbst
// unruhig, und die letzten Bytes eines Dateisystems sind die langsamsten.
const uint64_t kDiskFloorBytes = 256ull * 1024 * 1024;

}  // namespace

RecordingControl::RecordingControl(Config& config, Recorder& recorder, VideoRenderer& renderer,
                                   VideoCapture& capture, AudioEngine& audio,
                                   SettingsWindow& settings, CameraSink& camera, Host& host)
    : config_(config),
      recorder_(recorder),
      renderer_(renderer),
      capture_(capture),
      audio_(audio),
      settings_(settings),
      virtualCamera_(camera),
      host_(host) {}

RecordingControl::~RecordingControl() = default;

void RecordingControl::Shutdown() {
  StopRecording();
  for (Source& source : sources_) source.capture->Stop();
  if (probeThread_.joinable()) probeThread_.join();
}

void RecordingControl::StartEncoderProbe(bool full) {
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
    // The ffmpeg build belongs in here as well as the hardware: a different
    // build can be missing an encoder the last one had, and would then be
    // judged by a result it never produced.
    probeSignature_ = GraphicsAdapterSignature() + " | " + copy.version;
    probeResult_ = std::move(copy);
    probing_.store(false, std::memory_order_relaxed);
    probeDone_.store(true, std::memory_order_release);
  });
}

bool RecordingControl::CollectEncoderProbe() {
  if (!probeDone_.load(std::memory_order_acquire)) return false;
  probeDone_.store(false, std::memory_order_relaxed);
  if (probeThread_.joinable()) probeThread_.join();
  ffmpeg_ = std::move(probeResult_);
  if (!locating_) {
    SaveCachedEncoders();
    return false;
  }
  locating_ = false;
  LoadCachedEncoders();
  settings_.InvalidateFolderFields();
  if (recordWhenLocated_) {
    recordWhenLocated_ = false;
    if (ffmpeg_.found) {
      StartRecording();
    } else {
      host_.Toast(T("ffmpeg fehlt — in den Einstellungen unter Aufnahme holen.",
                    "ffmpeg is missing — fetch it under Recording in the settings."));
    }
  }
  return true;
}

bool RecordingControl::RelocateFfmpeg() {
  if (probing_.load(std::memory_order_relaxed) || probeDone_.load(std::memory_order_acquire) ||
      recorder_.recording() || startPending_) {
    return false;
  }
  if (probeThread_.joinable()) probeThread_.join();
  probing_.store(true, std::memory_order_relaxed);
  locating_ = true;
  probeThread_ = std::thread([this]() {
    SystemThreadScope onThisThread;
    probeResult_ = LocateFfmpeg();
    probeSignature_ = GraphicsAdapterSignature() + " | " + probeResult_.version;
    probing_.store(false, std::memory_order_relaxed);
    probeDone_.store(true, std::memory_order_release);
  });
  return true;
}

void RecordingControl::LoadCachedEncoders() {
  if (!ffmpeg_.found) return;
  RecordSettings& rec = config_.record;
  if (rec.encodersAvailable.empty() && rec.encoderProbeSignature.empty()) return;

  if (rec.encoderProbeSignature != probeSignature_) {
    CAP_LOG("Encoder probe discarded: hardware or ffmpeg has changed");
    rec.encoderProbeSignature.clear();
    rec.encodersAvailable.clear();
    rec.fullChromaProbed = false;
    rec.encodersFullChroma.clear();
    rec.encodersFullChroma10.clear();
    return;
  }

  ApplyCachedProbe(&ffmpeg_, rec.encodersAvailable, rec.fullChromaProbed,
                   rec.encodersFullChroma, rec.encodersFullChroma10);
  CAP_LOG("Encoders taken from the configuration: %zu usable",
          rec.encodersAvailable.size());
}

void RecordingControl::SaveCachedEncoders() {
  if (!ffmpeg_.found || !ffmpeg_.tested) return;
  RecordSettings& rec = config_.record;
  rec.encodersAvailable.clear();
  rec.encodersFullChroma.clear();
  rec.encodersFullChroma10.clear();
  rec.fullChromaProbed = true;
  for (const EncoderInfo& e : ffmpeg_.encoders) {
    if (!e.tested || !e.available) continue;
    rec.encodersAvailable.push_back((int)e.id);
    if (!e.fullTested) rec.fullChromaProbed = false;
    if (e.full8) rec.encodersFullChroma.push_back((int)e.id);
    if (e.full10) rec.encodersFullChroma10.push_back((int)e.id);
  }
  rec.encoderProbeSignature = probeSignature_;

  // A selection that did not survive the test would silently fail at F9, so it
  // goes back to Auto rather than staying as a trap.
  const EncoderInfo* chosen = ffmpeg_.Find(rec.encoder);
  if (!IsAutoEncoder(rec.encoder) && (!chosen || !chosen->available)) {
    CAP_WARN("Chosen encoder is not available, back to automatic");
    rec.encoder = RecordEncoder::Auto;
    host_.Toast(T("Der gewählte Encoder funktioniert hier nicht, zurück auf Automatisch.",
            "The selected encoder does not work here, back to Automatic."));
  }
}

void RecordingControl::ToggleRecording() {
  if (recorder_.recording() || startPending_ || recordWhenLocated_) {
    StopRecording(true);
  } else {
    StartRecording();
  }
}

void RecordingControl::StartRecording() {
  if (recorder_.recording() || startPending_) return;

  // Loading the ffmpeg libraries takes a moment, so it happens on the probe
  // thread, and the recording starts when it comes back with them.
  if (locating_) {
    recordWhenLocated_ = true;
    return;
  }
  if (!ffmpeg_.found) {
    if (RelocateFfmpeg()) {
      recordWhenLocated_ = true;
    } else {
      host_.Toast(T("Encoder werden noch geprüft, gleich nochmal.",
                    "Still testing encoders, try again in a moment."));
    }
    return;
  }
  if (!host_.CaptureRunning() || !renderer_.hasFrame()) {
    host_.Toast(T("Kein Bild zum Aufnehmen.", "No picture to record."));
    return;
  }

  if (probing_.load(std::memory_order_relaxed)) {
    host_.Toast(T("Encoder werden noch geprüft, gleich nochmal.",
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
    host_.Toast(T("Encoder werden geprüft, gleich nochmal.",
            "Testing encoders, try again in a moment."));
    return;
  }

  // The tap has to exist before the recorder pulls from it.
  const bool wantAudio = audio_.running() && audio_.tapSampleRate() > 0;
  if (wantAudio) audio_.SetTapEnabled(true);

  // Every source decides its own rate, and the recorder opens its tracks and
  // resamplers for them. Opening one takes from a few milliseconds to, for a
  // device that does not answer, three seconds; the picture goes on meanwhile,
  // and FeedRecorder launches once they and the disk have answered.
  pendingSettings_ = config_.record;
  pendingAudio_ = wantAudio;
  pendingFile_.clear();
  startPending_ = true;
  const uint32_t serial = ++startSerial_;
  SyncSources();

  // What the disk has to say, on its own thread: a drive that has gone to
  // sleep takes a second or more to answer.
  //
  // The folder is made again if it was deleted, or answered with the default,
  // instead of failing the start.
  disk_.Ask([this, serial, asked = pendingSettings_.outputFolder,
             container = pendingSettings_.container]() -> std::function<void()> {
    StartCheck check;
    check.folder = ChooseFolder(asked, DefaultRecordFolder());
    if (!check.folder.folder.empty()) {
      check.freeKnown = DiskFreeBytes(check.folder.folder, &check.freeBytes);
      check.file = NextRecordingFile(check.folder.folder, container);
    }
    return [this, serial, asked, check] { StartChecked(serial, asked, check); };
  });
}

void RecordingControl::StartChecked(uint32_t serial, const std::string& askedFolder,
                                    const StartCheck& check) {
  if (!startPending_ || serial != startSerial_) return;  // called off meanwhile

  if (check.folder.fellBack) host_.UseDefaultFolder(&config_.record.outputFolder, askedFolder);
  if (check.folder.folder.empty()) {
    CancelStart();
    host_.Toast(T("Zielordner nicht verfügbar.", "Folder not available."));
    return;
  }

  // Gar nicht erst anfangen, wenn der Platz schon jetzt nicht reicht: die
  // Aufnahme wuerde ihren Kopf schreiben und abbrechen, und liegen bliebe eine
  // Datei von ein paar Kilobyte, die aussieht wie eine Aufnahme.
  if (check.freeKnown && check.freeBytes < kDiskFloorBytes) {
    CancelStart();
    host_.Toast(Format(T("Zu wenig Speicherplatz — nur noch %s frei.",
                         "Not enough disk space — only %s free."),
                       FormatBytes(check.freeBytes).c_str()));
    return;
  }
  // Fresh, so the watch in FeedRecorder does not start on a number from
  // before.
  diskFreeKnown_ = check.freeKnown;
  diskFreeBytes_ = check.freeBytes;
  pendingFile_ = check.file;
}

void RecordingControl::CancelStart() {
  startPending_ = false;
  audio_.SetTapEnabled(false);
}

void RecordingControl::LaunchRecording() {
  // The source can have gone in the frames this waited.
  if (!host_.CaptureRunning() || !renderer_.hasFrame()) {
    CancelStart();
    host_.Toast(T("Kein Bild zum Aufnehmen.", "No picture to record."));
    return;
  }
  startPending_ = false;
  const bool wantAudio = pendingAudio_;

  const VideoFormatInfo format = renderer_.sourceFormat();
  std::string error;
  Recorder::AudioSource mainTrack;
  if (wantAudio) {
    mainTrack.sampleRate = audio_.tapSampleRate();
    mainTrack.pull = [this](float* out, size_t frames) { return audio_.ReadTap(out, frames); };
  }
  // Named after the device, so an editor shows which is which; the default
  // device after what it is.
  std::vector<Recorder::ExtraAudio> extras;
  const std::vector<RecordSource>& list = config_.active().audio.sources;
  for (size_t i = 0; i < list.size() && i < sources_.size(); ++i) {
    SourceCapture* source = sources_[i].capture.get();
    if (!list[i].enabled || !source->running() || source->sampleRate() <= 0) continue;
    source->ResetBuffer();
    Recorder::ExtraAudio extra;
    extra.source.sampleRate = source->sampleRate();
    extra.source.pull = [source](float* out, size_t frames) { return source->Read(out, frames); };
    extra.title = list[i].kind == SourceKind::System ? "System sound"
                  : !list[i].device.name.empty()     ? list[i].device.name
                  : list[i].kind == SourceKind::Input ? "Microphone"
                                                      : "Output";
    extra.tracks = list[i].tracks;
    extras.push_back(std::move(extra));
  }

  // Which picture the writer will be fed, decided before it starts: the wide
  // path only exists while the source is HDR and the setting asks for it.
  recorder_.SetWide(config_.app.recordHdr && renderer_.sourceHdr());

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
  // Der Deinterlacer in `RenderFrame` steht vor derselben Frage und
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
  // sich nichts mehr richten, die Rate steht mit dem Encoder fest.
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
  host_.DropViewAids();

  // Die Platzwarnung gilt je Aufnahme. Die Zahl dazu ist die der Pruefung vor
  // dem Start, nicht eine von vor einer Sekunde.
  diskWarned_ = false;
  encoderWarned_ = false;
  bufferWarned_ = false;
  recordingFile_ = pendingFile_;
  recordingBytes_ = 0;

  recorder_.SetGpuAdapter(renderer_.sharedAdapter());
  const bool ok = recorder_.Start(pendingSettings_, pendingFile_, ffmpeg_,
                                  renderer_.outputWidth(), renderer_.outputHeight(), recordFps,
                                  mainTrack, std::move(extras), &error);

  if (!ok) {
    audio_.SetTapEnabled(false);
    host_.Toast(error);
    return;
  }
  // 4:4:4 asked for and not given: the encoder did not pass it at this depth,
  // or was never tested for it. Recording anyway, but not silently.
  if (pendingSettings_.fullChroma && recorder_.format().layout != YuvLayout::I444) {
    host_.Toast(T("Aufnahme läuft, in 4:2:0: der Encoder kann hier kein 4:4:4.",
                  "Recording, in 4:2:0: the encoder cannot do 4:4:4 here."));
    return;
  }
  host_.Toast(T("Aufnahme läuft", "Recording"));
}

void RecordingControl::StopRecording(bool announce) {
  recordWhenLocated_ = false;
  if (startPending_) {
    CancelStart();
    return;
  }
  if (!recorder_.recording()) return;
  if (announce) announceFile_ = recorder_.stats().file;
  recorder_.Stop();
  audio_.SetTapEnabled(false);
}

void RecordingControl::CollectFinished() {
  // "Saved" only once the file is really closed, and only for a stop someone
  // asked for: a split goes straight on into the next file, and a full disk or
  // a failed encoder says so itself.
  Recorder::Finished done;
  while (recorder_.TakeFinished(&done)) {
    if (!done.complete) {
      host_.Toast(T("Die Datei wurde nicht sauber abgeschlossen und spielt vielleicht nicht.",
                    "The file was not closed cleanly and may not play."),
                  Utf8ToPath(done.file));
    } else if (done.file == announceFile_) {
      host_.Toast(Format(T("Aufnahme gespeichert (%.0f s)", "Recording saved (%.0f s)"),
                         done.seconds),
                  Utf8ToPath(done.file));
    }
    if (done.file == announceFile_) announceFile_.clear();
  }
}

void RecordingControl::UpdateDiskSpace() {
  disk_.Collect();

  // Nur wenn jemand hinsieht oder etwas davon abhaengt. Ein Laufwerk im
  // Sekundentakt zu befragen, waehrend niemand die Zahl braucht, weckt eine
  // schlafende Platte fuer nichts.
  if (!settings_.isOpen() && !recorder_.recording()) return;

  // Wie viele Tonspuren in der Datei landen wuerden -- jede kostet ihre
  // 192 kbit/s. Dieselbe Rechnung wie im Recorder (TrackPlan).
  TrackPlan plan(audio_.running() && audio_.tapSampleRate() > 0);
  const std::vector<RecordSource>& list = config_.active().audio.sources;
  for (size_t i = 0; i < list.size() && i < sources_.size(); ++i) {
    const SourceCapture& source = *sources_[i].capture;
    if (list[i].enabled && source.running() && source.sampleRate() > 0) plan.Add(list[i].tracks);
  }
  diskBytesPerSecond_ = EstimatedBytesPerSecond(config_.record, plan.count());

  const double now = TicksToSeconds(ClockTicks());
  // Der Arbeitsspeicher im selben Takt, aber ohne den Umweg ueber den Job: die
  // Abfrage kostet Mikrosekunden und wartet auf keine Platte. Was der Puffer
  // selbst haelt, zaehlt fuer die Einstellungen als frei -- er gibt es ja her.
  if (now - lastMemoryCheck_ > 1.0) {
    lastMemoryCheck_ = now;
    uint64_t total = 0, freeBytes = 0;
    if (MemoryBytes(&total, &freeBytes)) {
      recorder_.LimitBuffer(total, freeBytes);
      settings_.SetMemory(total, freeBytes + recorder_.queuedBytes());
    }
  }
  if (!diskAsked_ && now - lastDiskCheck_ > 1.0) {
    lastDiskCheck_ = now;
    diskAsked_ = true;
    // Absichtlich nicht ueber ChooseFolder: das legt Ordner an, und das hat
    // eine Abfrage im Sekundentakt nicht zu tun. DiskFreeBytes geht selbst bis
    // zum naechsten vorhandenen Elternteil hoch, der Ordner muss also noch gar
    // nicht existieren.
    const bool splitting = recorder_.recording() && config_.record.splitFiles;
    disk_.Ask([this, folderText = config_.record.outputFolder, serial = startSerial_,
               file = splitting ? recordingFile_ : std::filesystem::path()]()
                  -> std::function<void()> {
      const std::filesystem::path folder =
          folderText.empty() ? DefaultRecordFolder() : Utf8ToPath(folderText);
      uint64_t freeBytes = 0;
      const bool known = DiskFreeBytes(folder, &freeBytes);
      uint64_t fileBytes = 0;
      if (!file.empty()) {
        std::error_code ec;
        const uintmax_t size = std::filesystem::file_size(file, ec);
        if (!ec) fileBytes = (uint64_t)size;
      }
      return [this, known, freeBytes, fileBytes, serial] {
        diskAsked_ = false;
        diskFreeKnown_ = known;
        diskFreeBytes_ = freeBytes;
        // Only for the file it was asked about: a split since started another.
        if (serial == startSerial_) recordingBytes_ = fileBytes;
      };
    });
  }
  settings_.SetDiskFree(diskFreeBytes_, diskFreeKnown_, diskBytesPerSecond_);
}

void RecordingControl::SyncSources() {
  const std::vector<RecordSource>& list = config_.active().audio.sources;
  while (sources_.size() < list.size()) {
    sources_.emplace_back();
    sources_.back().capture = std::make_unique<SourceCapture>();
  }

  // Held open only while actually used: during a recording, and while the
  // settings are open so the level meter means something. Keeping a
  // microphone open the rest of the time would light up the Windows privacy
  // indicator for no reason and tell the user something untrue.
  const bool recording = recorder_.recording();
  const bool used = startPending_ || recording || settings_.isOpen();
  settings_.SetSourcesLocked(recording);

  for (size_t i = 0; i < sources_.size(); ++i) {
    Source& source = sources_[i];
    SourceCapture& capture = *source.capture;
    const RecordSource* entry = i < list.size() ? &list[i] : nullptr;

    // Another device is a fresh start, and clears a previous failure; so does
    // switching the entry off and on again. Not while recording: the recorder
    // has opened its resampler for the rate this one runs at. The settings
    // keep the list as it is meanwhile; only another profile could change it.
    if (entry && !recording) {
      if (source.kind != entry->kind || !(source.device == entry->device)) {
        source.kind = entry->kind;
        source.device = entry->device;
        source.failed = false;
        if (capture.running() || capture.opening()) capture.Stop();
      }
      if (!entry->enabled) source.failed = false;
    }

    // Not fatal: the recording still happens, just without this source.
    // Latched, because retrying every frame would fill the log and the screen
    // with the same message sixty times a second.
    const bool wanted = entry && entry->enabled && used;
    if (wanted && !capture.running() && !capture.opening() && !source.failed) {
      capture.Start(source.kind, source.device);
    } else if (!wanted && (capture.running() || capture.opening())) {
      capture.Stop();
    }
    // Opening happens on the source's thread; this only looks whether it is
    // done, once a frame, and a recording waiting for it starts the frame
    // after.
    std::string error;
    if (capture.opening() && capture.Opened(0, &error) == SourceCapture::Open::Failed) {
      source.failed = true;
      if (!error.empty()) host_.Toast(error);
    }
    if (entry) {
      capture.SetGain(entry->gain);
      settings_.SetSourceLevel(i, capture.peak(), capture.running(), source.failed);
    }
  }
}

bool RecordingControl::SourcesOpening() const {
  for (const Source& source : sources_) {
    if (source.capture->opening()) return true;
  }
  return false;
}

void RecordingControl::FeedRecorder() {
  CollectFinished();
  disk_.Collect();
  if (startPending_ && !pendingFile_.empty() && !SourcesOpening()) LaunchRecording();
  if (!recorder_.recording()) return;

  // The encoder did not open, or encoding or writing failed: stop cleanly.
  // First, because nothing below means anything for it.
  if (recorder_.failed()) {
    const RecordStats stats = recorder_.stats();
    StopRecording();
    host_.Toast(stats.error.empty() ? T("Aufnahme abgebrochen.", "Recording aborted.") : stats.error);
    return;
  }

  // Der Encoder schafft die Bilder nicht so schnell, wie sie kommen. Einmal je
  // Aufnahme, wie die Platzwarnung; solange es anhaelt, sagt es die Anzeige im
  // Bild.
  if (!encoderWarned_ && recorder_.encoderBehind()) {
    encoderWarned_ = true;
    host_.Toast(T("Encoder kommt nicht hinterher — schnellere Voreinstellung, kleinere Auflösung "
                  "oder Bildrate wählen.",
                  "Encoder overloaded — try a faster preset, or a lower resolution or frame rate."));
  }

  // Die Platte faellt zurueck, und was sie nicht nimmt, wartet im Speicher. Bei
  // drei Vierteln einmal sagen, ein zweites Mal erst, wenn sie zwischendurch
  // wieder unter die Haelfte aufgeholt hat. Voll beendet der Recorder selbst.
  const double fill = recorder_.bufferFill();
  if (!bufferWarned_ && fill >= 0.75) {
    bufferWarned_ = true;
    host_.Toast(Format(T("Platte kommt nicht hinterher — Puffer zu %.0f %% voll.",
                         "The disk is falling behind — the buffer is %.0f %% full."),
                       fill * 100.0));
  } else if (fill < 0.5) {
    bufferWarned_ = false;
  }

  const double now = TicksToSeconds(ClockTicks());

  // Die Quelle hat mitten in der Aufnahme die Norm gewechselt. Bildgroesse und
  // Rate stehen fest, seit der Encoder offen ist, also passt ab dem Wechsel
  // kein Bild mehr in die laufende Datei. Der Recorder wirft die falsch geformten weg, sonst
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
    // die falsche Zahl in der Datei, anderthalb Sekunden spaeter faellt es hier auf,
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
    // Ebenso, wenn die Quelle HDR wird oder es nicht mehr ist: das Pixelformat
    // steht genauso mit dem Encoder fest, und der breite Pfad liefert ohne HDR
    // gar kein Bild.
    const bool liveWide = config_.app.recordHdr && renderer_.sourceHdr();
    const bool formatDiffers = liveWide != recorder_.hdr();

    if (!shapeDiffers && !rateDiffers && !formatDiffers) {
      pendingSince_ = -1.0;
    } else if (liveWidth != pendingWidth_ || liveHeight != pendingHeight_ ||
               std::fabs(liveFps - pendingFps_) > 0.5 || pendingSince_ < 0.0) {
      pendingWidth_ = liveWidth;
      pendingHeight_ = liveHeight;
      pendingFps_ = liveFps;
      pendingSince_ = now;
    } else if (now - pendingSince_ > 1.5 && host_.CaptureRunning() &&
               renderer_.hasFrame()) {
      // Der Neustart braucht ein Bild, sonst bricht `StartRecording` ab und die
      // Aufnahme waere nach dem Stop zu Ende statt geteilt. Waehrend ein Graph
      // neu gebaut wird, gibt es keins -- dann wird eben weiter gewartet.
      CAP_LOG("Recording: source now %dx%d @ %.2f fps%s instead of %dx%d @ %.2f%s, new file",
              liveWidth, liveHeight, liveFps, liveWide ? " HDR" : "",
              recorder_.frameWidth(), recorder_.frameHeight(), recordSourceFps_,
              recorder_.hdr() ? " HDR" : "");
      StopRecording();
      StartRecording();
      return;
    }
  }

  // Splitting is a restart, not a seamless cut: the encoder and the container
  // are closed and new ones opened. A fraction of a second is lost at the boundary,
  // which is why this is off unless someone really is on FAT32.
  // The size is the one UpdateDiskSpace last had looked at, once a second.
  if (config_.record.splitFiles &&
      recordingBytes_ >= (uint64_t)config_.record.splitSizeMb * 1024ull * 1024ull) {
    CAP_LOG("Recording: size limit reached, new file");
    StopRecording();
    StartRecording();
    return;
  }

  // Der Platz auf dem Laufwerk. Eine volllaufende Platte bricht das Schreiben
  // mitten in der Datei ab, und was dann liegen bleibt, haengt vom Format ab: MKV
  // uebersteht das meistens, MP4 ohne seinen Index am Ende selten. Also lieber
  // eine Minute zu frueh sauber beendet als eine Datei, die keiner aufmacht.
  if (diskFreeKnown_) {
    // Gemessen schlaegt geschaetzt, sobald es etwas zu messen gibt: was dieser
    // Encoder auf dieser Quelle wirklich schreibt, kann von der eingestellten
    // Bitrate weit weg sein -- im Qualitaetsmodus ist es das immer.
    const double seconds = recorder_.seconds();
    const uint64_t written = recorder_.bytesWritten();
    double rate = diskBytesPerSecond_;
    if (seconds > 3.0 && written > 0) rate = (double)written / seconds;
    // Was im Puffer wartet, kommt noch auf die Platte.
    const uint64_t pending = recorder_.queuedBytes();
    const uint64_t room = diskFreeBytes_ > pending ? diskFreeBytes_ - pending : 0;
    const double left = rate > 1.0 ? (double)room / rate : 0.0;

    // Ein fester Boden zusaetzlich zur Zeit: bei kleiner Bitrate reichen 30 s
    // rechnerisch noch lange, waehrend Windows schon keinen Platz mehr fuer
    // seine eigenen Schreibpuffer hat.
    if (room < kDiskFloorBytes || (rate > 1.0 && left < 20.0)) {
      CAP_WARN("Recording: disk almost full (%s free), stopped", FormatBytes(room).c_str());
      StopRecording();
      host_.Toast(T("Aufnahme beendet — Speicherplatz fast aufgebraucht.",
              "Recording stopped — the disk is nearly full."));
      return;
    }
    // Einmal je Aufnahme: eine Warnung, die im Sekundentakt wiederkommt, ist
    // keine Warnung mehr.
    if (!diskWarned_ && rate > 1.0 && left < 120.0) {
      diskWarned_ = true;
      host_.Toast(Format(T("Nur noch %s frei — etwa %s Aufnahme.",
                     "Only %s free — about %s of recording."),
                   FormatBytes(room).c_str(), FormatDuration(left).c_str()));
    }
  }
}

}  // namespace cap
