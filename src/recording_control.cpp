#include "recording_control.h"

#include <cmath>

#include "audio/audio_engine.h"
#include "audio/mic_capture.h"
#include "camera_sink.h"
#include "capture/video_capture.h"
#include "files.h"
#include "i18n.h"
#include "imgui.h"
#include "platform.h"
#include "record/recorder.h"
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
                                   VideoCapture& capture, AudioEngine& audio, MicCapture& mic,
                                   SettingsWindow& settings, CameraSink& camera, Host& host)
    : config_(config),
      recorder_(recorder),
      renderer_(renderer),
      capture_(capture),
      audio_(audio),
      mic_(mic),
      settings_(settings),
      virtualCamera_(camera),
      host_(host) {}

void RecordingControl::Shutdown() {
  StopRecording();
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
    probeResult_ = std::move(copy);
    probing_.store(false, std::memory_order_relaxed);
    probeDone_.store(true, std::memory_order_release);
  });
}

void RecordingControl::CollectEncoderProbe() {
  if (!probeDone_.load(std::memory_order_acquire)) return;
  probeDone_.store(false, std::memory_order_relaxed);
  if (probeThread_.joinable()) probeThread_.join();
  ffmpeg_ = std::move(probeResult_);
  SaveCachedEncoders();
}

std::string RecordingControl::EncoderSignature() const {
  // The ffmpeg build belongs in here as well as the hardware: a different build
  // can be missing an encoder the last one had, and would then be judged by a
  // result it never produced.
  return GraphicsAdapterSignature() + " | " + ffmpeg_.version;
}

void RecordingControl::LoadCachedEncoders() {
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

void RecordingControl::SaveCachedEncoders() {
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
    host_.Toast(T("Der gewählte Encoder funktioniert hier nicht, zurück auf Automatisch.",
            "The selected encoder does not work here, back to Automatic."));
  }
}

void RecordingControl::ToggleRecording() {
  if (recorder_.recording()) {
    StopRecording();
  } else {
    StartRecording();
  }
}

void RecordingControl::StartRecording() {
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
    host_.Toast(T("ffmpeg fehlt — in den Einstellungen unter Aufnahme holen.",
            "ffmpeg is missing — fetch it under Recording in the settings."));
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
      host_.ResolveOutputFolder(&config_.record.outputFolder, DefaultRecordFolder());
  if (folder.empty()) {
    renderer_.SetReadbackEnabled(false);
    audio_.SetTapEnabled(false);
    host_.Toast(T("Zielordner nicht verfügbar.", "Folder not available."));
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
    host_.Toast(Format(T("Zu wenig Speicherplatz — nur noch %s frei.",
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
  host_.DropViewAids();

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
    host_.Toast(error);
    return;
  }
  host_.Toast(T("Aufnahme läuft", "Recording"));
}

void RecordingControl::StopRecording() {
  if (!recorder_.recording()) return;
  const RecordStats stats = recorder_.stats();
  recorder_.Stop();
  renderer_.SetReadbackEnabled(false);
  audio_.SetTapEnabled(false);
  host_.Toast(Format(T("Aufnahme gespeichert (%.0f s)", "Recording saved (%.0f s)"), stats.seconds),
        Utf8ToPath(stats.file));
}

void RecordingControl::UpdateDiskSpace() {
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

void RecordingControl::SyncMicrophone(bool aboutToRecord) {
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
      if (!error.empty()) host_.Toast(error);
    }
  } else if (!wanted && mic_.running()) {
    mic_.Stop();
    micAttempted_ = false;
  }
  mic_.SetGain(a.micGain);
}

void RecordingControl::FeedRecorder() {
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
    } else if (now - pendingSince_ > 1.5 && host_.CaptureRunning() &&
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
                   FormatBytes(diskFreeBytes_).c_str(), FormatDuration(left).c_str()));
    }
  }

  // ffmpeg died on its own: stop cleanly rather than filling a dead pipe.
  if (recorder_.failed()) {
    const RecordStats stats = recorder_.stats();
    StopRecording();
    host_.Toast(stats.error.empty() ? T("Aufnahme abgebrochen.", "Recording aborted.") : stats.error);
    return;
  }

}

}  // namespace cap
