#include "record/recorder.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <system_error>

#include "i18n.h"
#include "render/video_renderer.h"  // for the readback pixel format

namespace cap {
namespace {

std::string TimestampedName(RecordContainer container) {
  const LocalTime t = NowLocal();
  return Format("%s_%04d-%02d-%02d_%02d-%02d-%02d.%s", AppNameUtf8().c_str(), t.year, t.month,
                t.day, t.hour, t.minute, t.second,
                container == RecordContainer::Mp4 ? "mp4" : "mkv");
}

// The arguments as one line, for the log alone. What ffmpeg is handed is the
// vector; this is only so a log reader can paste it into a shell and see for
// themselves.
std::string Joined(const std::vector<std::string>& args) {
  std::string out;
  for (const std::string& arg : args) {
    if (!out.empty()) out += ' ';
    out += arg;
  }
  return out;
}

}  // namespace

double EstimatedBytesPerSecond(const RecordSettings& settings, int audioTracks) {
  const double kbps = (double)settings.bitrateKbps + 192.0 * (double)std::max(audioTracks, 0);
  // Container overhead is left out: a few bytes of index per frame against
  // twenty megabit of picture is below the error of the bitrate itself.
  return kbps * 1000.0 / 8.0;
}

Recorder::~Recorder() {
  Stop();
}

void Recorder::Fail(const Said& said) {
  {
    std::lock_guard<std::mutex> lock(infoMutex_);
    if (error_.empty()) error_ = said.shown;
  }
  failed_.store(true, std::memory_order_relaxed);
  CAP_ERR("Recording: %s", said.logged.c_str());
}

// ------------------------------------------------------------- command line

Recorder::Family Recorder::FamilyOf(const std::string& name) {
  if (name.find("_nvenc") != std::string::npos) return Family::Nvenc;
  if (name.find("_amf") != std::string::npos) return Family::Amf;
  if (name.find("_qsv") != std::string::npos) return Family::Qsv;
  return Family::Software;
}

namespace {

// The seven speed steps, in each vendor's own words. Empty means "this one has
// nothing to say here", and then nothing is passed at all -- which is not the
// same as passing its default, and is the only honest thing to do.
const char* PresetName(Recorder::Family family, EncoderPreset preset) {
  if (preset == EncoderPreset::Auto) return nullptr;
  const int step = (int)preset - 1;  // 0 = fastest .. 6 = slowest
  switch (family) {
    case Recorder::Family::Nvenc: {
      // p1 is fastest, p7 slowest. Straight across.
      static const char* kNames[] = {"p1", "p2", "p3", "p4", "p5", "p6", "p7"};
      return kNames[step];
    }
    case Recorder::Family::Qsv: {
      static const char* kNames[] = {"veryfast", "faster", "fast",
                                        "medium",   "slow",   "slower", "veryslow"};
      return kNames[step];
    }
    case Recorder::Family::Amf: {
      // AMF has three, so the seven steps fold onto them.
      static const char* kNames[] = {"speed",   "speed",  "balanced", "balanced",
                                        "quality", "quality", "quality"};
      return kNames[step];
    }
    default: {
      static const char* kNames[] = {"ultrafast", "veryfast", "faster", "medium",
                                        "slow",      "slower",   "veryslow"};
      return kNames[step];
    }
  }
}

}  // namespace

std::vector<std::string> Recorder::EncoderOptions(const RecordSettings& settings,
                                                  const EncoderInfo& encoder) const {
  const Family family = FamilyOf(encoder.ffmpegName);
  std::vector<std::string> out;
  auto pair = [&out](std::string key, std::string value) {
    out.push_back(std::move(key));
    out.push_back(std::move(value));
  };

  // ---- how the bitrate is spent ----
  //
  // The names differ per vendor and so does the way a quality target is asked
  // for. Quicksync has no -rc at all: what it does follows from which of
  // bitrate or quality it is given.
  switch (family) {
    case Family::Nvenc:
      pair("-rc", settings.rateControl == RateControl::Cbr   ? "cbr"
                  : settings.rateControl == RateControl::Vbr ? "vbr"
                                                            : "constqp");
      if (settings.rateControl == RateControl::Quality) {
        pair("-cq", std::to_string(settings.qualityLevel));
      }
      break;
    case Family::Amf:
      pair("-rc", settings.rateControl == RateControl::Cbr   ? "cbr"
                  : settings.rateControl == RateControl::Vbr ? "vbr_peak"
                                                            : "cqp");
      if (settings.rateControl == RateControl::Quality) {
        const std::string qp = std::to_string(settings.qualityLevel);
        pair("-qp_i", qp);
        pair("-qp_p", qp);
      }
      break;
    case Family::Qsv:
      if (settings.rateControl == RateControl::Quality) {
        pair("-global_quality", std::to_string(settings.qualityLevel));
      }
      break;
    default:
      if (settings.rateControl == RateControl::Quality) {
        pair("-crf", std::to_string(settings.qualityLevel));
      }
      break;
  }

  // ---- speed against quality ----
  if (const char* preset = PresetName(family, settings.preset)) {
    pair(family == Family::Amf ? "-quality" : "-preset", preset);
  }

  // ---- what to optimise for ----
  if (settings.tune != EncoderTune::Auto) {
    if (family == Family::Nvenc) {
      pair("-tune", settings.tune == EncoderTune::Quality ? "hq" : "ll");
    } else if (family == Family::Amf) {
      pair("-usage", settings.tune == EncoderTune::Quality ? "transcoding" : "lowlatency");
    }
  }

  // ---- looking ahead ----
  if (settings.lookAhead) {
    if (family == Family::Nvenc) {
      pair("-rc-lookahead", "32");
    } else if (family == Family::Qsv) {
      pair("-look_ahead", "1");
    } else if (family == Family::Amf) {
      pair("-preanalysis", "1");
    }
  }

  // ---- spending bits where the eye looks ----
  if (settings.adaptiveQuant) {
    if (family == Family::Nvenc) {
      pair("-spatial-aq", "1");
      pair("-temporal-aq", "1");
    } else if (family == Family::Amf) {
      pair("-vbaq", "1");
    } else if (family == Family::Qsv) {
      pair("-mbbrc", "1");
    }
  }

  // ---- two passes ----
  if (family == Family::Nvenc && settings.multipass != Multipass::Auto) {
    pair("-multipass", settings.multipass == Multipass::Off       ? "disabled"
                       : settings.multipass == Multipass::Quarter ? "qres"
                                                                 : "fullres");
  }

  return out;
}

std::vector<std::string> Recorder::BuildArguments(const RecordSettings& settings,
                                                  const EncoderInfo& encoder,
                                                  const std::string& audioPipe,
                                                  const std::string& micPipe,
                                                  const std::filesystem::path& outFile,
                                                  int audioRate, int micRate) const {
  std::vector<std::string> a;
  auto add = [&a](std::string one) { a.push_back(std::move(one)); };
  auto pair = [&a](std::string key, std::string value) {
    a.push_back(std::move(key));
    a.push_back(std::move(value));
  };

  add("-hide_banner");
  pair("-loglevel", "error");
  add("-y");

  // Input 0: raw frames exactly as the readback hands them over. Declaring the
  // rate here is what makes the output constant frame rate; the writer thread
  // guarantees that many frames per second actually arrive.
  // The pixel format comes from the renderer rather than being spelled out
  // here: these two have to agree byte for byte, and a literal in this file is
  // exactly how they came apart once already.
  pair("-f", "rawvideo");
  pair("-pix_fmt", pixelFormat_);
  pair("-s", std::to_string(width_) + "x" + std::to_string(height_));
  pair("-r", Format("%.6f", fps_));
  pair("-i", "pipe:0");

  // Input 1: the untouched captured audio.
  if (audioRate > 0) {
    pair("-f", "f32le");
    pair("-ar", std::to_string(audioRate));
    pair("-ac", "2");
    pair("-i", audioPipe);
  }

  // Input 2: the microphone, at whatever rate its device runs at. No resampling
  // here -- ffmpeg is told the rate and does it on the way into AAC.
  if (micRate > 0) {
    pair("-f", "f32le");
    pair("-ar", std::to_string(micRate));
    pair("-ac", "2");
    pair("-i", micPipe);
  }

  // With both sources there is a choice of what the file should contain. The
  // mix is made by ffmpeg rather than here, so the separate tracks stay exactly
  // what each device delivered.
  //
  // normalize=0 matters: amix otherwise divides every input by the number of
  // inputs, which would make the game quieter in the mix than on its own track
  // and leave people wondering what happened. The sum can clip if both are hot,
  // which is what the level meters are for.
  const bool bothSources = audioRate > 0 && micRate > 0;
  const bool wantMix = bothSources && micTrackMode_ != MicTrackMode::Separate;
  const bool wantSeparate = !bothSources || micTrackMode_ != MicTrackMode::Mixed;

  if (wantMix) {
    pair("-filter_complex", "[1:a][2:a]amix=inputs=2:duration=first:normalize=0[mix]");
  }

  pair("-map", "0:v");
  int audioIndex = 0;
  std::vector<std::string> titles;
  if (wantMix) {
    pair("-map", "[mix]");
    titles.push_back("Mix");
  }
  if (wantSeparate) {
    if (audioRate > 0) {
      pair("-map", "1:a");
      titles.push_back("Capture");
    }
    if (micRate > 0) {
      pair("-map", std::to_string(audioRate > 0 ? 2 : 1) + ":a");
      titles.push_back("Microphone");
    }
  }

  pair("-c:v", encoder.ffmpegName);
  for (std::string& option : EncoderOptions(settings, encoder)) add(std::move(option));
  if (hdr_) {
    // Ten bit 4:2:0 and the three pieces of colour description that make a file
    // playable as HDR rather than as a washed out mess. They are not optional:
    // nothing else in the file says the picture is on the PQ curve, and a player
    // that is not told will assume it is not.
    pair("-pix_fmt", "p010le");
    pair("-color_primaries", "bt2020");
    pair("-color_trc", "smpte2084");
    pair("-colorspace", "bt2020nc");
    pair("-color_range", "tv");
  } else {
    // 4:2:0 eight bit: the one format every hardware encoder and every player
    // agrees on. Anything wider would exclude the very cards this is meant for.
    pair("-pix_fmt", "nv12");
  }
  if (settings.rateControl == RateControl::Quality) {
    // A quality target and a bitrate are contradictory instructions, and ffmpeg
    // resolves them by quietly ignoring one. Better to send only one.
    pair("-b:v", "0");
  } else {
    pair("-b:v", std::to_string(settings.bitrateKbps) + "k");
    // Constant means constant: the ceiling is the bitrate. Variable is allowed
    // to peak at half again, which is what makes it worth choosing.
    const int ceiling = settings.rateControl == RateControl::Cbr ? settings.bitrateKbps
                                                                : settings.bitrateKbps * 3 / 2;
    pair("-maxrate", std::to_string(ceiling) + "k");
    pair("-bufsize", std::to_string(ceiling * 2) + "k");
  }

  // The old speed setting still applies when the new preset is left on
  // automatic, so nobody's existing configuration changes meaning.
  if (!encoder.hardware && settings.preset == EncoderPreset::Auto) {
    pair("-preset", RecordSpeedName((int)settings.speed));
  }

  if (!titles.empty()) {
    pair("-c:a", "aac");
    pair("-b:a", "192k");
    // Named tracks, so a player and an editor both show which is which instead
    // of "Audio 1" and "Audio 2".
    for (const std::string& title : titles) {
      pair("-metadata:s:a:" + std::to_string(audioIndex++), "title=" + title);
    }
  }

  add(PathToUtf8(outFile));
  return a;
}

// -------------------------------------------------------------------- start

bool Recorder::Start(const RecordSettings& settings, const FfmpegInfo& ffmpeg, int width,
                     int height, double sourceFps, const AudioSource& main,
                     const AudioSource& mic, MicTrackMode micTrackMode, std::string* error) {
  Stop();

  auto fail = [&](const Said& said) {
    if (error) *error = said.shown;
    CAP_ERR("Recording could not be started: %s", said.logged.c_str());
    Stop();
    return false;
  };

  if (!ffmpeg.found) {
    return fail(CAP_SAID(T("ffmpeg wurde nicht gefunden.", "ffmpeg was not found.")));
  }
  if (width <= 0 || height <= 0) {
    return fail(CAP_SAID(T("Kein Bild zum Aufnehmen.", "No picture to record.")));
  }

  const EncoderInfo* encoder = ffmpeg.Resolve(settings.encoder);
  if (!encoder || !encoder->available) {
    return fail(CAP_SAID(T("Kein verwendbarer Encoder. Encoder-Test in den Einstellungen ausführen.",
                           "No usable encoder. Run the encoder test in the settings.")));
  }

  // Odd sizes break 4:2:0 chroma. Rather than refuse, round down by a pixel --
  // the alternative is telling someone their 1439 pixel capture cannot be
  // recorded, which helps nobody.
  width_ = width & ~1;
  height_ = height & ~1;
  fps_ = settings.fps > 0.0 ? settings.fps : (sourceFps > 1.0 ? sourceFps : 60.0);
  audioRate_ = main.active() ? main.sampleRate : 0;
  micRate_ = mic.active() ? mic.sampleRate : 0;
  micTrackMode_ = micTrackMode;
  frameBytes_ = (size_t)width_ * (size_t)height_ * 4;
  pullAudio_ = main.pull;
  pullMic_ = mic.pull;

  std::string folderText = settings.outputFolder.empty() ? PathToUtf8(DefaultRecordFolder())
                                                         : settings.outputFolder;
  if (!folderText.empty() && (folderText.back() == '\\' || folderText.back() == '/')) {
    folderText.pop_back();
  }
  const std::filesystem::path folder = Utf8ToPath(folderText);
  if (!EnsureFolder(folder)) {
    return fail(CAP_SAID(T("Zielordner konnte nicht angelegt werden: ", "Could not create the folder: ") +
                         folderText));
  }
  const std::filesystem::path outFile = folder / Utf8ToPath(TimestampedName(settings.container));

  // --- streams -----------------------------------------------------------
  // A generous buffer keeps a brief encoder stall from reaching back to the
  // writer thread.
  videoStream_ = MakeInputStream(1 << 22);
  if (!videoStream_) {
    return fail(CAP_SAID(T("Videopipe konnte nicht erstellt werden.", "Could not create the video pipe.")));
  }

  // Only one stream can arrive as ffmpeg's standard input, so every audio track
  // goes through one it opens by name instead.
  if (audioRate_ > 0) {
    audioStream_ = MakeNamedStream("qblank_audio", 1 << 20);
    if (!audioStream_) {
      return fail(CAP_SAID(T("Audiopipe konnte nicht erstellt werden.", "Could not create the audio pipe.")));
    }
  }
  if (micRate_ > 0) {
    micStream_ = MakeNamedStream("qblank_mic", 1 << 20);
    if (!micStream_) {
      return fail(CAP_SAID(T("Mikrofonpipe konnte nicht erstellt werden.",
                             "Could not create the microphone pipe.")));
    }
  }

  // --- process -----------------------------------------------------------
  ProcessSpec spec;
  spec.program = ffmpeg.path;
  spec.args = BuildArguments(settings, *encoder,
                             audioStream_ ? audioStream_->name() : std::string(),
                             micStream_ ? micStream_->name() : std::string(), outFile, audioRate_,
                             micRate_);
  CAP_LOG("ffmpeg %s", Joined(spec.args).c_str());

  // Whatever ffmpeg says about the encode arrives through OnFfmpegSaid. It says
  // it on its error stream, and a windowed program has nowhere for that to go --
  // which is how the first version of this managed to say nothing at all about
  // why it produced no file.
  if (!process_.Start(spec, videoStream_.get(),
                      [this](const std::string& line) { OnFfmpegSaid(line); }, nullptr)) {
    return fail(CAP_SAID(T("ffmpeg konnte nicht gestartet werden.", "Could not start ffmpeg.")));
  }

  // --- state -------------------------------------------------------------
  for (int i = 0; i < 3; ++i) slots_[i].assign(frameBytes_, 0);
  readyIdx_ = -1;
  heldIdx_ = -1;
  audioFramesWritten_.store(0, std::memory_order_relaxed);
  videoFramesWritten_.store(0, std::memory_order_relaxed);
  duplicated_.store(0, std::memory_order_relaxed);
  dropped_.store(0, std::memory_order_relaxed);
  bytesWritten_.store(0, std::memory_order_relaxed);
  startTicks_ = ClockTicks();
  lastAudioSeen_ = 0;
  lastAudioProgressTicks_ = 0;
  audioStallLogged_ = false;
  failed_.store(false, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(infoMutex_);
    file_ = PathToUtf8(outFile);
    encoderLabel_ = encoder->label;
    error_.clear();
  }

  running_.store(true, std::memory_order_relaxed);
  if (audioRate_ > 0) {
    audioThread_ = std::thread(&Recorder::AudioThread, this, audioStream_.get(), pullAudio_, true);
  }
  if (micRate_ > 0) {
    micThread_ = std::thread(&Recorder::AudioThread, this, micStream_.get(), pullMic_, false);
  }
  videoThread_ = std::thread(&Recorder::VideoThread, this);

  CAP_LOG("Recording started: %s, %dx%d @ %.3f fps, %s", PathToUtf8(outFile).c_str(), width_,
          height_, fps_, encoder->label.c_str());
  return true;
}

void Recorder::Stop() {
  const bool wasRunning = running_.exchange(false, std::memory_order_relaxed);

  if (videoThread_.joinable()) videoThread_.join();
  if (audioThread_.joinable()) audioThread_.join();
  if (micThread_.joinable()) micThread_.join();

  // Closing the streams is what tells ffmpeg they ended. It then writes the
  // container index and exits -- an MP4 killed before that point has no moov
  // atom and will not play at all.
  videoStream_.reset();
  audioStream_.reset();
  micStream_.reset();

  if (process_.started()) {
    const int code = process_.WaitOrKill(15000);
    if (code < 0) {
      CAP_WARN("ffmpeg did not exit and was terminated - the file may be incomplete");
    }
    if (wasRunning) {
      CAP_LOG("Recording stopped, ffmpeg exit code %d, %llu frames", code,
              (unsigned long long)videoFramesWritten_.load());
    }
  }

  {
    std::lock_guard<std::mutex> lock(frameMutex_);
    for (int i = 0; i < 3; ++i) slots_[i].clear();
    readyIdx_ = -1;
    heldIdx_ = -1;
  }
  pullAudio_ = nullptr;
}

// --------------------------------------------------------------- frame feed

int Recorder::PickWriteSlotLocked() const {
  for (int i = 0; i < 3; ++i) {
    if (i != readyIdx_ && i != heldIdx_) return i;
  }
  return 0;  // unreachable with three slots and two reserved indices
}

void Recorder::PushVideo(const uint8_t* pixels, int stride, int width, int height) {
  if (!running_.load(std::memory_order_relaxed) || !pixels) return;

  // A frame of a different shape than ffmpeg was told about. This happens for
  // the fraction of a second between the source changing its video standard and
  // the app noticing and cutting the file. Dropping it is not a nicety: the
  // copy below reads `height_` rows of `width_ * 4` bytes, and a source that
  // just went from 576 to 480 lines reads a fifth of a frame past the end of
  // the staging buffer.
  //
  // Compared with the same rounding that Start applied, because an odd source
  // is a legitimate one -- a crop can leave 721 columns -- and the file is then
  // one column narrower on purpose. Rounding down can only ever ask for less
  // than the frame holds, so it stays safe.
  if ((width & ~1) != width_ || (height & ~1) != height_) return;

  int slot;
  {
    std::lock_guard<std::mutex> lock(frameMutex_);
    if (slots_[0].size() != frameBytes_) return;  // not started, or size changed
    slot = PickWriteSlotLocked();
  }

  // Copy outside the lock: the chosen slot is neither the one waiting to be
  // written nor the one the writer holds, so nobody else touches it. The rows
  // are repacked because a staging texture's pitch is rarely width * 4, and
  // rawvideo has no concept of padding.
  const size_t rowBytes = (size_t)width_ * 4;
  uint8_t* dst = slots_[slot].data();
  for (int y = 0; y < height_; ++y) {
    memcpy(dst + (size_t)y * rowBytes, pixels + (size_t)y * (size_t)stride, rowBytes);
  }

  {
    std::lock_guard<std::mutex> lock(frameMutex_);
    // A frame still waiting here never made it into the file: the timeline did
    // not need it, because the source runs faster than the recording rate.
    if (readyIdx_ >= 0) dropped_.fetch_add(1, std::memory_order_relaxed);
    readyIdx_ = slot;
  }
}

bool Recorder::WriteAll(ChildStream* stream, const uint8_t* data, size_t size) {
  if (!stream || !stream->Write(data, size)) return false;
  bytesWritten_.fetch_add(size, std::memory_order_relaxed);
  return true;
}

// ------------------------------------------------------------ writer threads

void Recorder::VideoThread() {
  while (running_.load(std::memory_order_relaxed)) {
    // Take the newest frame and keep holding it: when the timeline needs more
    // frames than actually arrived, this is what gets repeated.
    const uint8_t* held = nullptr;
    bool fresh = false;
    {
      std::lock_guard<std::mutex> lock(frameMutex_);
      if (readyIdx_ >= 0) {
        heldIdx_ = readyIdx_;
        readyIdx_ = -1;
        fresh = true;
      }
      if (heldIdx_ >= 0 && slots_[heldIdx_].size() == frameBytes_) {
        held = slots_[heldIdx_].data();
      }
    }
    if (!held) {
      ::Sleep(2);
      continue;
    }

    // How many frames the timeline should contain by now.
    //
    // The audio is the better master once it flows, because the card's sample
    // clock is steadier than the PC's. But it cannot be the master from the
    // start: ffmpeg opens its inputs in order and blocks reading video before it
    // ever opens the audio pipe, so waiting for audio samples before writing the
    // first frame deadlocks both sides. Until audio appears -- and again if it
    // stops appearing -- the wall clock stands in.
    const uint64_t audioFrames = audioFramesWritten_.load(std::memory_order_relaxed);
    const int64_t now = ClockTicks();
    if (audioFrames != lastAudioSeen_) {
      lastAudioSeen_ = audioFrames;
      lastAudioProgressTicks_ = now;
    }
    const bool audioStalled =
        lastAudioProgressTicks_ != 0 && TicksToSeconds(now - lastAudioProgressTicks_) > 2.0;
    const bool audioIsMaster = audioRate_ > 0 && audioFrames > 0 && !audioStalled;

    if (audioStalled && !audioStallLogged_) {
      audioStallLogged_ = true;
      CAP_WARN("Recording: audio stopped, frame timing continues on the system clock");
    }

    uint64_t target = 0;
    if (audioIsMaster) {
      target = (uint64_t)((double)audioFrames / (double)audioRate_ * fps_);
    } else {
      target = (uint64_t)(TicksToSeconds(now - startTicks_) * fps_);
    }

    uint64_t written = videoFramesWritten_.load(std::memory_order_relaxed);
    if (written >= target) {
      SleepMilliseconds(1);
      continue;
    }

    // Every frame in the timeline has to be written -- skipping one would make
    // the video shorter than the audio and desync the file for good. If the
    // encoder stalls, the pipe blocks here, which is exactly where the waiting
    // belongs.
    for (uint64_t i = written; i < target && running_.load(std::memory_order_relaxed); ++i) {
      if (!WriteAll(videoStream_.get(), held, frameBytes_)) {
        Fail(CAP_SAID(T("Verbindung zu ffmpeg abgebrochen.", "Lost the connection to ffmpeg.")));
        running_.store(false, std::memory_order_relaxed);
        return;
      }
      videoFramesWritten_.fetch_add(1, std::memory_order_relaxed);
      if (!fresh || i > written) duplicated_.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void Recorder::AudioThread(ChildStream* stream, AudioPullFn pull, bool countsAsClock) {
  // Wait for ffmpeg to open its end, in short steps rather than one long one: an
  // ffmpeg that never starts must not be able to leave this thread stuck and
  // hang the whole shutdown.
  ChildStream::Connect state = ChildStream::Connect::Waiting;
  while (state == ChildStream::Connect::Waiting && running_.load(std::memory_order_relaxed)) {
    state = stream->WaitForChild(100);
  }
  if (state != ChildStream::Connect::Connected) return;

  std::vector<float> buffer(4096 * 2);
  while (running_.load(std::memory_order_relaxed)) {
    const size_t got = pull ? pull(buffer.data(), 4096) : 0;
    if (got == 0) {
      SleepMilliseconds(2);
      continue;
    }

    const size_t bytes = got * 2 * sizeof(float);
    if (!WriteAll(stream, (const uint8_t*)buffer.data(), bytes)) {
      Fail(CAP_SAID(T("Audiopipe abgebrochen.", "The audio pipe broke.")));
      running_.store(false, std::memory_order_relaxed);
      break;
    }
    if (countsAsClock) audioFramesWritten_.fetch_add(got, std::memory_order_relaxed);
  }
}

void Recorder::OnFfmpegSaid(const std::string& line) {
  CAP_WARN("ffmpeg: %s", line.c_str());
  std::lock_guard<std::mutex> lock(infoMutex_);
  if (error_.empty()) error_ = line;
}

uint64_t Recorder::outputFileSize() const {
  std::filesystem::path path;
  {
    std::lock_guard<std::mutex> lock(infoMutex_);
    if (file_.empty()) return 0;
    path = Utf8ToPath(file_);
  }
  std::error_code ec;
  const uintmax_t size = std::filesystem::file_size(path, ec);
  return ec ? 0 : (uint64_t)size;
}

RecordStats Recorder::stats() const {
  RecordStats s;
  s.running = running_.load(std::memory_order_relaxed);
  s.seconds = startTicks_ ? TicksToSeconds(ClockTicks() - startTicks_) : 0.0;
  s.videoFrames = videoFramesWritten_.load(std::memory_order_relaxed);
  s.duplicated = duplicated_.load(std::memory_order_relaxed);
  s.dropped = dropped_.load(std::memory_order_relaxed);
  s.bytesWritten = bytesWritten_.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(infoMutex_);
    s.file = file_;
    s.encoder = encoderLabel_;
    s.error = error_;
  }
  return s;
}

}  // namespace cap
