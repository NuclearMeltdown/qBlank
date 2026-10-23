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

// What a cached encoder test holds for. The ffmpeg build belongs in it as well
// as the hardware: a different build can be missing an encoder the last one
// had, and would then be judged by a result it never produced. The number at
// the end goes up when the test itself learns new encoders, as it did the
// lossless ones.
std::string ProbeSignature(const std::string& version) {
  return GraphicsAdapterSignature() + " | " + version + " | 2";
}

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
  recordWhenLocated_ = false;
  CancelStart();
  StopSession();
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
    probeSignature_ = ProbeSignature(copy.version);
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
      recorder_.active() || startPending_) {
    return false;
  }
  if (probeThread_.joinable()) probeThread_.join();
  probing_.store(true, std::memory_order_relaxed);
  locating_ = true;
  probeThread_ = std::thread([this]() {
    SystemThreadScope onThisThread;
    probeResult_ = LocateFfmpeg();
    probeSignature_ = ProbeSignature(probeResult_.version);
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
  if (recorder_.recording() || (startPending_ && pendingWithFile_) || recordWhenLocated_) {
    StopRecording(true);
  } else {
    StartRecording();
  }
}

void RecordingControl::StartRecording() {
  if (recorder_.recording() || (startPending_ && pendingWithFile_)) return;
  // A replay still on its way is overtaken: it comes along with the file.
  if (startPending_) CancelStart();
  replaySuspended_ = false;

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

  // On the encoders the replay already runs, when they suit: the file begins
  // at their next keyframe, and nothing is encoded twice. When they do not,
  // they start again for the file, and what the replay held is lost.
  const bool onSession = recorder_.active() && !recorder_.failed() &&
                         SameSession(config_.record) && !SourcesChanged();
  if (recorder_.active() && !onSession) StopSession();
  BeginStart(WantedSettings(*usable),
             IsLosslessEncoder(usable->id) ? RecordingKind::Lossless : RecordingKind::Normal,
             true, onSession);
}

RecordSettings RecordingControl::WantedSettings(const EncoderInfo& encoder) const {
  RecordSettings wanted = config_.record;
  // A lossless encoder brings its own container, and the file is named for it.
  wanted.container = ContainerFor(encoder.id, wanted.container);
  return wanted;
}

bool RecordingControl::SameSession(const RecordSettings& wanted) const {
  const RecordSettings& s = sessionSettings_;
  return wanted.encoder == s.encoder &&
         ContainerFor(wanted.encoder, wanted.container) == s.container &&
         wanted.speed == s.speed && wanted.bitrateKbps == s.bitrateKbps &&
         wanted.rateControl == s.rateControl && wanted.qualityLevel == s.qualityLevel &&
         wanted.preset == s.preset && wanted.tune == s.tune && wanted.multipass == s.multipass &&
         wanted.lookAhead == s.lookAhead && wanted.adaptiveQuant == s.adaptiveQuant &&
         wanted.allEncoderChips == s.allEncoderChips && wanted.fullChroma == s.fullChroma &&
         wanted.unfiltered == s.unfiltered && wanted.fps == s.fps &&
         wanted.maxLines == s.maxLines &&
         config_.app.recordHdr == sessionHdrAsked_;
}

bool RecordingControl::SourcesChanged() const {
  // The tap's rate is the main track's; 0 is a session without it.
  const int tapRate = audio_.running() ? audio_.tapSampleRate() : 0;
  if (tapRate != sessionTapRate_) return true;
  // By index, as sources_ goes: an entry put in front moves every capture
  // behind it onto another device.
  const std::vector<RecordSource>& list = config_.active().audio.sources;
  if (list.size() != sessionSources_.size()) return true;
  for (size_t i = 0; i < list.size(); ++i) {
    const RecordSource& now = list[i];
    const RecordSource& then = sessionSources_[i];
    if (now.enabled != then.enabled) return true;
    if (now.enabled && (now.kind != then.kind || now.tracks != then.tracks ||
                        !(now.device == then.device))) {
      return true;
    }
  }
  return false;
}

void RecordingControl::BeginStart(const RecordSettings& settings, RecordingKind kind,
                                  bool withFile, bool onSession) {
  // The tap has to exist before the recorder pulls from it. Encoders that run
  // have theirs.
  const bool wantAudio = audio_.running() && audio_.tapSampleRate() > 0;
  if (wantAudio && !onSession) audio_.SetTapEnabled(true);

  // Every source decides its own rate, and the recorder opens its tracks and
  // resamplers for them. Opening one takes from a few milliseconds to, for a
  // device that does not answer, three seconds; the picture goes on meanwhile,
  // and FeedRecorder launches once they and the disk have answered.
  pendingSettings_ = settings;
  pendingAudio_ = wantAudio;
  pendingFile_.clear();
  startPending_ = true;
  pendingWithFile_ = withFile;
  pendingOnSession_ = onSession;
  const uint32_t serial = ++startSerial_;
  SyncSources();
  if (!withFile) return;

  // What the disk has to say, on its own thread: a drive that has gone to
  // sleep takes a second or more to answer.
  //
  // The folder is made again if it was deleted, or answered with the default,
  // instead of failing the start.
  disk_.Ask([this, serial, asked = pendingSettings_.outputFolder,
             container = pendingSettings_.container, kind]() -> std::function<void()> {
    StartCheck check;
    check.folder = ChooseFolder(asked, DefaultRecordFolder());
    if (!check.folder.folder.empty()) {
      check.freeKnown = DiskFreeBytes(check.folder.folder, &check.freeBytes);
      check.file = NextRecordingFile(check.folder.folder, container, kind);
    }
    return [this, serial, asked, check] { StartChecked(serial, asked, check); };
  });
}

void RecordingControl::StartChecked(uint32_t serial, const std::string& askedFolder,
                                    const StartCheck& check) {
  if (!startPending_ || serial != startSerial_) return;  // called off meanwhile

  // A split that cannot have its next file ends the recording, as a stop
  // would: asking again every frame would say so every frame.
  auto refuse = [this] {
    CancelStart();
    if (recorder_.recording()) StopRecording();
  };
  if (check.folder.fellBack) host_.UseDefaultFolder(&config_.record.outputFolder, askedFolder);
  if (check.folder.folder.empty()) {
    refuse();
    host_.Toast(T("Zielordner nicht verfügbar.", "Folder not available."));
    return;
  }

  // Gar nicht erst anfangen, wenn der Platz schon jetzt nicht reicht: die
  // Aufnahme wuerde ihren Kopf schreiben und abbrechen, und liegen bliebe eine
  // Datei von ein paar Kilobyte, die aussieht wie eine Aufnahme.
  if (check.freeKnown && check.freeBytes < kDiskFloorBytes) {
    refuse();
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
  // Encoders that run keep their tap.
  if (!recorder_.active()) audio_.SetTapEnabled(false);
}

void RecordingControl::LaunchRecording() {
  const bool withFile = pendingWithFile_;
  // The source can have gone in the frames this waited.
  if (!host_.CaptureRunning() || !renderer_.hasFrame()) {
    CancelStart();
    if (withFile) host_.Toast(T("Kein Bild zum Aufnehmen.", "No picture to record."));
    return;
  }
  startPending_ = false;
  // The encoders the file was meant for still run: it goes onto them. If they
  // ended meanwhile, it gets encoders of its own.
  if (withFile && pendingOnSession_ && recorder_.active() && !recorder_.failed()) {
    AttachFile();
    return;
  }
  const bool wantAudio = pendingAudio_;
  // Again: encoders ended meanwhile let go of the tap.
  if (wantAudio) audio_.SetTapEnabled(true);

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
  double recordFps = SourceFps();
  // Vor dem Doppeln festgehalten: `FeedRecorder` vergleicht damit, ob die
  // Quelle waehrend der Aufnahme eine andere geworden ist, und das soll ein
  // umgestellter Deinterlacer nicht ausloesen.
  recordSourceFps_ = recordFps;
  pendingSince_ = -1.0;
  // Ohne Filter die Bilder der Karte, wie sie ankommen: mit den Farben, mit
  // denen der Renderer sie liest, und mit der Rate der Karte -- gewebte
  // Halbbilder bleiben ein Bild. Nimmt der Encoder das Format so nicht, wird
  // mit Filter aufgenommen, und das wird gesagt.
  const Profile& profile = config_.active();
  Recorder::CardFrames card;
  bool unfiltered = !host_.OnScreen() && pendingSettings_.unfiltered &&
                    IsLosslessEncoder(pendingSettings_.encoder);
  bool cardRefused = false;
  if (unfiltered) {
    card.format = renderer_.sourceFormat();
    card.fullRange = renderer_.cardFullRange();
    card.bt709 = renderer_.cardBt709();
    card.transfer = (int)renderer_.hdrTransfer();
    const bool interlaced =
        card.format.interlaced ||
        (profile.image.deinterlace != Deinterlace::Off &&
         (!profile.image.deinterlaceAuto ||
          renderer_.detectedInterlace() == VideoRenderer::InterlaceVerdict::Interlaced));
    bool topFirst = card.format.fieldOneFirst;
    if (profile.image.fieldOrder != FieldOrder::Auto) {
      topFirst = profile.image.fieldOrder == FieldOrder::TopFirst;
    }
    card.fieldOrder = interlaced ? (topFirst ? 1 : 2) : 0;
    cardRefused = Recorder::CardPixelBytes(pendingSettings_.encoder, card.format) <= 0.0;
    unfiltered = !cardRefused;
  }
  recorder_.SetCard(unfiltered ? &card : nullptr);
  if (!unfiltered && !host_.OnScreen() && profile.image.deinterlace != Deinterlace::Off) {
    recordFps *= 2.0;
  }

  // Alle drei Sehhilfen wuerden in die Datei wandern, weil Aufnahme und virtuelle
  // Kamera hinter denselben Durchgaengen abgreifen wie die Anzeige. Also fallen
  // sie hier weg, statt die Aufnahme zu verweigern: wer aufnimmt, meint das
  // ganze Bild von jetzt. Erst hier, weil alles davor noch abbrechen kann. Ohne
  // Filter kommen sie nicht hinein. Das Replay allein laesst sie stehen: es
  // laeuft im Hintergrund, ohne dass jemand gerade aufnehmen wollte.
  if (!unfiltered && withFile) host_.DropViewAids();

  // Die Platzwarnung gilt je Aufnahme. Die Zahl dazu ist die der Pruefung vor
  // dem Start, nicht eine von vor einer Sekunde.
  diskWarned_ = false;
  encoderWarned_ = false;
  bufferWarned_ = false;
  leftOutSeen_ = 0;
  recordingFile_ = pendingFile_;
  recordingBytes_ = 0;

  recorder_.SetGpuAdapter(renderer_.sharedAdapter());
  renderer_.SetRecordLines(unfiltered ? 0 : pendingSettings_.maxLines);
  const bool ok = recorder_.Start(pendingSettings_, pendingFile_, ffmpeg_,
                                  unfiltered ? card.format.width : renderer_.recordWidth(),
                                  unfiltered ? card.format.height : renderer_.recordHeight(),
                                  recordFps, mainTrack, std::move(extras), &error);

  if (!ok) {
    audio_.SetTapEnabled(false);
    // Once: the replay would otherwise try again every frame.
    if (config_.record.replay) replaySuspended_ = true;
    host_.Toast(error);
    return;
  }
  sessionSettings_ = pendingSettings_;
  sessionHdrAsked_ = config_.app.recordHdr;
  sessionSources_ = config_.active().audio.sources;
  sessionTapRate_ = wantAudio ? mainTrack.sampleRate : 0;
  ++sessionSerial_;
  // The replay goes on from EnsureReplay, still in this frame.
  replaySecondsSet_ = 0;
  replayGbSet_ = 0;
  if (!withFile) return;
  if (cardRefused) {
    host_.Toast(T("Aufnahme läuft, mit Filter: dieser Encoder nimmt das Kartenformat nicht roh.",
                  "Recording, filtered: this encoder does not take the card's format raw."));
    return;
  }
  // HDR asked for and not given: only NVENC keeps ten bits losslessly.
  if (IsLosslessEncoder(pendingSettings_.encoder) && config_.app.recordHdr &&
      renderer_.sourceHdr() && !recorder_.hdr()) {
    host_.Toast(T("Aufnahme läuft, in SDR: verlustfrei geht HDR nur mit NVENC.",
                  "Recording, in SDR: lossless HDR takes NVENC."));
    return;
  }
  // 4:4:4 asked for and not given: the encoder did not pass it at this depth,
  // or was never tested for it. Recording anyway, but not silently.
  const YuvLayout layout = recorder_.format().layout;
  if (pendingSettings_.fullChroma && !unfiltered && layout != YuvLayout::I444 &&
      layout != YuvLayout::Bgr) {
    host_.Toast(T("Aufnahme läuft, in 4:2:0: der Encoder kann hier kein 4:4:4.",
                  "Recording, in 4:2:0: the encoder cannot do 4:4:4 here."));
    return;
  }
  host_.Toast(T("Aufnahme läuft", "Recording"));
}

void RecordingControl::StopRecording(bool announce) {
  recordWhenLocated_ = false;
  // A file still waiting is called off; one being split goes with it.
  if (startPending_ && pendingWithFile_) CancelStart();
  if (!recorder_.recording()) return;
  if (announce) announceFile_ = recorder_.stats().file;
  // The replay keeps its encoders, and only the file ends.
  if (recorder_.replaying()) {
    recorder_.StopFile();
  } else {
    StopSession();
  }
}

void RecordingControl::StopSession() {
  recorder_.Stop();
  audio_.SetTapEnabled(false);
}

void RecordingControl::AttachFile() {
  // Freeze, compare and bypass would end up in the file from here on, as in
  // LaunchRecording.
  if (!recorder_.unfiltered()) host_.DropViewAids();
  diskWarned_ = false;
  encoderWarned_ = false;
  bufferWarned_ = false;
  const bool split = recorder_.recording();
  recordingFile_ = pendingFile_;
  recordingBytes_ = 0;
  recorder_.StartFile(pendingFile_);
  if (!split) host_.Toast(T("Aufnahme läuft", "Recording"));
}

void RecordingControl::SplitFile() {
  if (startPending_) return;  // the next file is on its way
  // Automatic never picks a lossless encoder, so the setting tells.
  BeginStart(sessionSettings_,
             IsLosslessEncoder(sessionSettings_.encoder) ? RecordingKind::Lossless
                                                         : RecordingKind::Normal,
             true, true);
}

void RecordingControl::ToggleReplay() {
  RecordSettings& rec = config_.record;
  rec.replay = !rec.replay;
  if (!rec.replay) {
    EnsureReplay();  // lets it go at once, not a frame later
    host_.Toast(T("Replay aus", "Replay off"));
    return;
  }
  if (!ffmpeg_.found && !locating_) {
    host_.Toast(T("Replay an — es fehlt noch ffmpeg, in den Einstellungen unter Aufnahme holen.",
                  "Replay on — ffmpeg is still missing, fetch it under Recording in the settings."));
    return;
  }
  EnsureReplay();
  host_.Toast(Format(T("Replay an (%d s)", "Replay on (%d s)"), rec.replaySeconds));
}

void RecordingControl::SaveReplay() {
  if (!recorder_.replaying()) {
    host_.Toast(!config_.record.replay ? T("Das Replay ist aus.", "The replay is off.")
                : replaySuspended_     ? T("Das Replay ist ausgefallen.", "The replay has failed.")
                                       : T("Das Replay startet noch.", "The replay is still starting."));
    return;
  }
  // The clip's name comes from the disk, which may have to wake up first; the
  // encoders the answer is for are counted, so it does not land in the ring
  // of a restart.
  disk_.Ask([this, asked = config_.record.outputFolder, container = sessionSettings_.container,
             serial = sessionSerial_]() -> std::function<void()> {
    const FolderChoice folder = ChooseFolder(asked, DefaultRecordFolder());
    std::filesystem::path file;
    if (!folder.folder.empty()) file = NextRecordingFile(folder.folder, container, RecordingKind::Clip);
    return [this, asked, folder, file, serial] {
      if (folder.fellBack) host_.UseDefaultFolder(&config_.record.outputFolder, asked);
      if (serial != sessionSerial_) {
        host_.Toast(T("Das Replay hat neu angefangen, der Clip fehlt.",
                      "The replay started over, there is no clip."));
        return;
      }
      if (file.empty()) {
        host_.Toast(T("Zielordner nicht verfügbar.", "Folder not available."));
        return;
      }
      double seconds = 0.0;
      if (!recorder_.SaveReplay(file, &seconds)) {
        host_.Toast(T("Noch nichts im Replay.", "Nothing in the replay yet."));
      }
    };
  });
}

void RecordingControl::EnsureReplay() {
  const RecordSettings& rec = config_.record;
  // Switched on again, by key or in the settings: a failure is tried again.
  if (rec.replay != replaySeen_) {
    replaySeen_ = rec.replay;
    replaySuspended_ = false;
    replayProbeAsked_ = false;
  }

  if (!replayWanted()) {
    if (startPending_ && !pendingWithFile_) CancelStart();
    if (recorder_.replaying()) recorder_.SetReplay(0.0, 0);
    // A file on its way still wants the encoders.
    if (recorder_.active() && !recorder_.recording() && !startPending_) StopSession();
    return;
  }

  if (recorder_.active()) {
    if (recorder_.failed()) return;  // FeedRecorder says so and ends them
    // On its own, the replay follows the settings by starting again -- what it
    // held is lost. Not while they are open: every step of a slider would.
    if (!recorder_.recording() && !startPending_ &&
        (SourcesChanged() || (!settings_.isOpen() && !SameSession(rec)))) {
      CAP_LOG("Recording: the replay's settings changed, starting again");
      StopSession();
      return;  // and again from below, the next frame
    }
    // Another folder only once they are closed: typed into, every letter would
    // be one.
    if (!recorder_.replaying() || rec.replaySeconds != replaySecondsSet_ ||
        rec.replayGb != replayGbSet_ || rec.replayOnDisk != replayOnDiskSet_ ||
        (rec.replayOnDisk && !settings_.isOpen() && rec.outputFolder != replayFolderSet_)) {
      replaySecondsSet_ = rec.replaySeconds;
      replayGbSet_ = rec.replayGb;
      replayOnDiskSet_ = rec.replayOnDisk;
      replayFolderSet_ = rec.outputFolder;
      // Beside the clips, on the disk meant to have the room.
      std::filesystem::path disk;
      if (rec.replayOnDisk) {
        disk = (rec.outputFolder.empty() ? DefaultRecordFolder() : Utf8ToPath(rec.outputFolder)) /
               "clips" / ".replay";
      }
      recorder_.SetReplay((double)rec.replaySeconds, (uint64_t)rec.replayGb << 30, disk);
    }
    return;
  }

  // Started on its own once it can, and quietly: no picture yet, or ffmpeg
  // still loading, is waiting rather than failing.
  if (startPending_ || locating_ || !ffmpeg_.found || probing_.load(std::memory_order_relaxed) ||
      !host_.CaptureRunning() || !renderer_.hasFrame()) {
    return;
  }
  const EncoderInfo* usable = ffmpeg_.Resolve(rec.encoder);
  if (!usable || !usable->available) {
    // Nothing tested yet: one test, not one a frame.
    if (!replayProbeAsked_) {
      replayProbeAsked_ = true;
      StartEncoderProbe(true);
    }
    return;
  }
  BeginStart(WantedSettings(*usable), RecordingKind::Normal, false, false);
}

void RecordingControl::CollectFinished() {
  // "Saved" only once the file is really closed, and only for a stop someone
  // asked for: a split goes straight on into the next file, and a full disk or
  // a failed encoder says so itself.
  Recorder::Finished done;
  while (recorder_.TakeFinished(&done)) {
    // A clip is always one someone asked for. One that came to nothing is
    // already gone, so there is no file to point at.
    if (done.clip) {
      if (done.complete) {
        host_.Toast(Format(T("Clip gespeichert (%.0f s)", "Clip saved (%.0f s)"), done.seconds),
                    Utf8ToPath(done.file));
      } else {
        host_.Warn(T("Clip konnte nicht gespeichert werden.", "Could not save the clip."));
      }
      continue;
    }
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
  // The replay depends on the memory.
  if (!settings_.isOpen() && !recorder_.active()) return;

  // Wie viele Tonspuren in der Datei landen wuerden -- jede kostet ihre
  // 192 kbit/s. Dieselbe Rechnung wie im Recorder (TrackPlan).
  TrackPlan plan(audio_.running() && audio_.tapSampleRate() > 0);
  const std::vector<RecordSource>& list = config_.active().audio.sources;
  for (size_t i = 0; i < list.size() && i < sources_.size(); ++i) {
    const SourceCapture& source = *sources_[i].capture;
    if (list[i].enabled && source.running() && source.sampleRate() > 0) plan.Add(list[i].tracks);
  }
  // Lossless goes by the picture: the one there is, at the rate LaunchRecording
  // would choose, or 1080p60 while there is none.
  int width = 0;
  int height = 0;
  CappedRecordSize(renderer_.outputWidth(), renderer_.outputHeight(), config_.record.maxLines,
                   &width, &height);
  if (width <= 0 || height <= 0) {
    width = 1920;
    height = 1080;
  }
  double fps = config_.record.fps;
  if (fps <= 0.0) {
    fps = SourceFps();
    if (!host_.OnScreen() && config_.active().image.deinterlace != Deinterlace::Off) fps *= 2.0;
    if (fps <= 1.0) fps = 60.0;
  }
  // Ohne Filter die Karte, wie sie kommt, wenn der Encoder sie so nimmt.
  double pixelBytes = 3.0;
  const VideoFormatInfo& card = renderer_.sourceFormat();
  if (!host_.OnScreen() && config_.record.unfiltered && IsLosslessEncoder(config_.record.encoder) &&
      card.width > 0 && card.height > 0) {
    const double cardBytes = Recorder::CardPixelBytes(config_.record.encoder, card);
    if (cardBytes > 0.0) {
      pixelBytes = cardBytes;
      width = card.width & ~1;
      height = card.height & ~1;
      fps = SourceFps() > 1.0 ? SourceFps() : 60.0;
    }
  }
  diskBytesPerSecond_ =
      EstimatedBytesPerSecond(config_.record, plan.count(), width, height, fps, pixelBytes);

  const double now = TicksToSeconds(ClockTicks());
  // Der Arbeitsspeicher im selben Takt, aber ohne den Umweg ueber den Job: die
  // Abfrage kostet Mikrosekunden und wartet auf keine Platte. Was der Puffer
  // selbst haelt, zaehlt fuer die Einstellungen als frei -- er gibt es ja her.
  // Ebenso das Replay.
  if (now - lastMemoryCheck_ > 1.0) {
    lastMemoryCheck_ = now;
    uint64_t total = 0, freeBytes = 0;
    if (MemoryBytes(&total, &freeBytes)) {
      recorder_.LimitBuffer(config_.record, freeBytes);
      bool onDisk = false;
      const uint64_t replay = recorder_.replayBytes(nullptr, &onDisk);
      replayDiskBytes_ = onDisk ? replay : 0;
      settings_.SetMemory(total, freeBytes + recorder_.queuedBytes() + (onDisk ? 0 : replay));
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
  settings_.SetDiskFree(diskFreeBytes_, diskFreeKnown_, diskBytesPerSecond_, replayDiskBytes_);
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
  // indicator for no reason and tell the user something untrue. The replay
  // records all the time it is on, and holds them open as long.
  //
  // The replay alone does not lock the list: it starts again with the new one.
  if (recorder_.active() && !recorder_.recording() && !startPending_ && SourcesChanged()) {
    CAP_LOG("Recording: the replay's sources changed, starting again");
    StopSession();
  }
  const bool recording = recorder_.recording();
  const bool used = startPending_ || recorder_.active() || settings_.isOpen();
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

double RecordingControl::SourceFps() const {
  double fps = renderer_.sourceFormat().fps;
  // Ein Bildschirm liefert nur, wenn sich etwas aendert -- gemessen haette ein
  // stehender Desktop zwei Bilder je Sekunde. Er hat seine Wiederholrate.
  if (host_.OnScreen()) return fps;
  if (const FrameBuffer* sink = capture_.sink()) {
    const double measured = sink->stats().sourceFps;
    if (measured > 1.0) fps = measured;
  }
  return fps;
}

bool RecordingControl::leftOutLately() const {
  return leftOutSeen_ > 0 && TicksToSeconds(ClockTicks()) - leftOutAt_ < 10.0;
}

void RecordingControl::FeedRecorder() {
  CollectFinished();
  disk_.Collect();
  // The replay alone has no file to wait for.
  if (startPending_ && (!pendingWithFile_ || !pendingFile_.empty()) && !SourcesOpening()) {
    LaunchRecording();
  }
  EnsureReplay();
  if (!recorder_.active()) return;

  // The encoder did not open, or encoding or writing failed: stop cleanly.
  // First, because nothing below means anything for it. The replay goes with
  // them, and stays off until it is switched on again: started again on its
  // own, it would fail the same way every frame.
  if (recorder_.failed()) {
    const RecordStats stats = recorder_.stats();
    const bool wasRecording = recorder_.recording();
    StopSession();
    if (config_.record.replay) replaySuspended_ = true;
    host_.Warn(!stats.error.empty() ? stats.error
               : wasRecording       ? T("Aufnahme abgebrochen.", "Recording aborted.")
                                    : T("Replay abgebrochen.", "Replay aborted."));
    return;
  }
  // Only the replay on the disk gave up; a file goes on, and EnsureReplay ends
  // the encoders when there is none.
  std::string replayTrouble;
  if (recorder_.TakeReplayFailure(&replayTrouble)) {
    replaySuspended_ = true;
    host_.Warn(replayTrouble);
  }

  const double now = TicksToSeconds(ClockTicks());

  // Der Encoder schafft die Bilder nicht so schnell, wie sie kommen. Einmal je
  // Aufnahme, wie die Platzwarnung; solange es anhaelt, sagt es die Anzeige im
  // Bild.
  if (!encoderWarned_ && recorder_.encoderBehind()) {
    encoderWarned_ = true;
    host_.Warn(T("Encoder kommt nicht hinterher — schnellere Voreinstellung, kleinere Auflösung "
                 "oder Bildrate wählen.",
                 "Encoder overloaded — try a faster preset, or a lower resolution or frame rate."));
  }

  // Bilder, fuer die der Encoder keine Zeit hatte: in der Datei ein Ruckler.
  // Gesagt beim ersten, und wieder, wenn nach einer halben Minute Ruhe neue
  // dazukommen -- ein Encoder an der Grenze laesst laufend welche aus, und
  // dafuer steht die Zahl im Bild.
  const uint64_t leftOut = recorder_.leftOut();
  if (leftOut > leftOutSeen_) {
    if (leftOutSeen_ == 0 || now - leftOutAt_ > 30.0) {
      host_.Warn(T("Bilder ausgelassen — der Encoder kam nicht hinterher.",
                   "Frames dropped — the encoder fell behind."));
    }
    leftOutSeen_ = leftOut;
    leftOutAt_ = now;
  }

  // Die Platte faellt zurueck, und was sie nicht nimmt, wartet im Speicher. Bei
  // drei Vierteln einmal sagen, ein zweites Mal erst, wenn sie zwischendurch
  // wieder unter die Haelfte aufgeholt hat. Voll beendet der Recorder selbst.
  // Nur fuer die laufende Datei: ein Clip schreibt aus dem Replay und hat
  // keinen Puffer, der volllaufen koennte.
  const double fill = recorder_.recording() ? recorder_.bufferFill() : 0.0;
  if (!bufferWarned_ && fill >= 0.75) {
    bufferWarned_ = true;
    host_.Warn(Format(T("Platte kommt nicht hinterher — Puffer zu %.0f %% voll.",
                        "The disk is falling behind — the buffer is %.0f %% full."),
                      fill * 100.0));
  } else if (fill < 0.5) {
    bufferWarned_ = false;
  }

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
    // Ohne Filter zaehlt das Kartenbild selbst: dieselbe Groesse reicht dort
    // nicht, das Format der Bytes steht mit dem Encoder ebenso fest.
    const bool unfiltered = recorder_.unfiltered();
    const VideoFormatInfo& card = renderer_.sourceFormat();
    const int liveWidth = unfiltered ? card.width : renderer_.recordWidth();
    const int liveHeight = unfiltered ? card.height : renderer_.recordHeight();
    double liveFps = recordSourceFps_;
    if (host_.OnScreen()) {
      if (card.fps > 1.0) liveFps = card.fps;
    } else if (const FrameBuffer* sink = capture_.sink()) {
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
    const bool formatDiffers =
        unfiltered ? !SameCardFormat(card, recorder_.card()) || renderer_.sourceHdr() != recorder_.hdr()
                   : liveWide != recorder_.wideAsked();

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
      // Das Replay faengt mit der neuen Form von vorne an, was es hielt, ist
      // weg: es passt in keine Datei mit der neuen.
      CAP_LOG("Recording: source now %dx%d @ %.2f fps%s instead of %dx%d @ %.2f%s, new file",
              liveWidth, liveHeight, liveFps, liveWide ? " HDR" : "",
              recorder_.frameWidth(), recorder_.frameHeight(), recordSourceFps_,
              recorder_.hdr() ? " HDR" : "");
      const bool wasRecording = recorder_.recording();
      StopSession();
      if (wasRecording) StartRecording();
      return;
    }
  }

  // The rest is about the file.
  if (!recorder_.recording()) return;

  // Splitting is seamless: the same encoders go on, and the next file begins
  // at their next keyframe, where the last one ends.
  // The size is the one UpdateDiskSpace last had looked at, once a second.
  if (config_.record.splitFiles &&
      recordingBytes_ >= (uint64_t)config_.record.splitSizeMb * 1024ull * 1024ull) {
    if (!startPending_) CAP_LOG("Recording: size limit reached, new file");
    SplitFile();
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
      host_.Warn(T("Aufnahme beendet — Speicherplatz fast aufgebraucht.",
                   "Recording stopped — the disk is nearly full."));
      return;
    }
    // Einmal je Aufnahme: eine Warnung, die im Sekundentakt wiederkommt, ist
    // keine Warnung mehr.
    if (!diskWarned_ && rate > 1.0 && left < 120.0) {
      diskWarned_ = true;
      host_.Warn(Format(T("Nur noch %s frei — etwa %s Aufnahme.",
                          "Only %s free — about %s of recording."),
                        FormatBytes(room).c_str(), FormatDuration(left).c_str()));
    }
  }
}

}  // namespace cap
