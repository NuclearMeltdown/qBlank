#include "record/recorder.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "app_identity.h"
#include "i18n.h"
#include "render/video_renderer.h"  // for the readback pixel format
#include "text_win32.h"

namespace cap {
namespace {

int64_t QpcNow() {
  LARGE_INTEGER v;
  ::QueryPerformanceCounter(&v);
  return v.QuadPart;
}

double QpcSeconds(int64_t ticks) {
  static const double freq = [] {
    LARGE_INTEGER f;
    ::QueryPerformanceFrequency(&f);
    return (double)f.QuadPart;
  }();
  return (double)ticks / freq;
}

std::wstring TimestampedName(RecordContainer container) {
  SYSTEMTIME st;
  ::GetLocalTime(&st);
  wchar_t buf[64];
  swprintf_s(buf, L"%s_%04u-%02u-%02u_%02u-%02u-%02u.%s", kAppName, st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond,
             container == RecordContainer::Mp4 ? L"mp4" : L"mkv");
  return buf;
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
const wchar_t* PresetName(Recorder::Family family, EncoderPreset preset) {
  if (preset == EncoderPreset::Auto) return nullptr;
  const int step = (int)preset - 1;  // 0 = fastest .. 6 = slowest
  switch (family) {
    case Recorder::Family::Nvenc: {
      // p1 is fastest, p7 slowest. Straight across.
      static const wchar_t* kNames[] = {L"p1", L"p2", L"p3", L"p4", L"p5", L"p6", L"p7"};
      return kNames[step];
    }
    case Recorder::Family::Qsv: {
      static const wchar_t* kNames[] = {L"veryfast", L"faster", L"fast",
                                        L"medium",   L"slow",   L"slower", L"veryslow"};
      return kNames[step];
    }
    case Recorder::Family::Amf: {
      // AMF has three, so the seven steps fold onto them.
      static const wchar_t* kNames[] = {L"speed",   L"speed",  L"balanced", L"balanced",
                                        L"quality", L"quality", L"quality"};
      return kNames[step];
    }
    default: {
      static const wchar_t* kNames[] = {L"ultrafast", L"veryfast", L"faster", L"medium",
                                        L"slow",      L"slower",   L"veryslow"};
      return kNames[step];
    }
  }
}

}  // namespace

std::wstring Recorder::EncoderOptions(const RecordSettings& settings,
                                      const EncoderInfo& encoder) const {
  const Family family = FamilyOf(encoder.ffmpegName);
  std::wstring out;

  // ---- how the bitrate is spent ----
  //
  // The names differ per vendor and so does the way a quality target is asked
  // for. Quicksync has no -rc at all: what it does follows from which of
  // bitrate or quality it is given.
  switch (family) {
    case Family::Nvenc:
      out += settings.rateControl == RateControl::Cbr       ? L" -rc cbr"
             : settings.rateControl == RateControl::Vbr     ? L" -rc vbr"
                                                            : L" -rc constqp";
      if (settings.rateControl == RateControl::Quality) {
        out += L" -cq " + std::to_wstring(settings.qualityLevel);
      }
      break;
    case Family::Amf:
      out += settings.rateControl == RateControl::Cbr       ? L" -rc cbr"
             : settings.rateControl == RateControl::Vbr     ? L" -rc vbr_peak"
                                                            : L" -rc cqp";
      if (settings.rateControl == RateControl::Quality) {
        const std::wstring qp = std::to_wstring(settings.qualityLevel);
        out += L" -qp_i " + qp + L" -qp_p " + qp;
      }
      break;
    case Family::Qsv:
      if (settings.rateControl == RateControl::Quality) {
        out += L" -global_quality " + std::to_wstring(settings.qualityLevel);
      }
      break;
    default:
      if (settings.rateControl == RateControl::Quality) {
        out += L" -crf " + std::to_wstring(settings.qualityLevel);
      }
      break;
  }

  // ---- speed against quality ----
  if (const wchar_t* preset = PresetName(family, settings.preset)) {
    out += family == Family::Amf ? L" -quality " : L" -preset ";
    out += preset;
  }

  // ---- what to optimise for ----
  if (settings.tune != EncoderTune::Auto) {
    if (family == Family::Nvenc) {
      out += settings.tune == EncoderTune::Quality ? L" -tune hq" : L" -tune ll";
    } else if (family == Family::Amf) {
      out += settings.tune == EncoderTune::Quality ? L" -usage transcoding"
                                                   : L" -usage lowlatency";
    }
  }

  // ---- looking ahead ----
  if (settings.lookAhead) {
    if (family == Family::Nvenc) {
      out += L" -rc-lookahead 32";
    } else if (family == Family::Qsv) {
      out += L" -look_ahead 1";
    } else if (family == Family::Amf) {
      out += L" -preanalysis 1";
    }
  }

  // ---- spending bits where the eye looks ----
  if (settings.adaptiveQuant) {
    if (family == Family::Nvenc) {
      out += L" -spatial-aq 1 -temporal-aq 1";
    } else if (family == Family::Amf) {
      out += L" -vbaq 1";
    } else if (family == Family::Qsv) {
      out += L" -mbbrc 1";
    }
  }

  // ---- two passes ----
  if (family == Family::Nvenc && settings.multipass != Multipass::Auto) {
    out += settings.multipass == Multipass::Off       ? L" -multipass disabled"
           : settings.multipass == Multipass::Quarter ? L" -multipass qres"
                                                      : L" -multipass fullres";
  }

  return out;
}

std::wstring Recorder::BuildCommandLine(const RecordSettings& settings,
                                        const EncoderInfo& encoder,
                                        const std::wstring& audioPipe,
                                        const std::wstring& micPipe,
                                        const std::wstring& outFile, int audioRate,
                                        int micRate) const {
  std::wstring cmd;
  cmd += L"-hide_banner -loglevel error -y";

  // Input 0: raw frames exactly as the readback hands them over. Declaring the
  // rate here is what makes the output constant frame rate; the writer thread
  // guarantees that many frames per second actually arrive.
  // The pixel format comes from the renderer rather than being spelled out
  // here: these two have to agree byte for byte, and a literal in this file is
  // exactly how they came apart once already.
  cmd += L" -f rawvideo -pix_fmt ";
  cmd += ToWide(pixelFormat_);
  cmd += L" -s " + std::to_wstring(width_) + L"x" + std::to_wstring(height_);
  cmd += L" -r " + ToWide(Format("%.6f", fps_));
  cmd += L" -i pipe:0";

  // Input 1: the untouched captured audio.
  if (audioRate > 0) {
    cmd += L" -f f32le -ar " + std::to_wstring(audioRate) + L" -ac 2";
    cmd += L" -i \"" + audioPipe + L"\"";
  }

  // Input 2: the microphone, at whatever rate its device runs at. No resampling
  // here -- ffmpeg is told the rate and does it on the way into AAC.
  if (micRate > 0) {
    cmd += L" -f f32le -ar " + std::to_wstring(micRate) + L" -ac 2";
    cmd += L" -i \"" + micPipe + L"\"";
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
    cmd += L" -filter_complex \"[1:a][2:a]amix=inputs=2:duration=first:normalize=0[mix]\"";
  }

  cmd += L" -map 0:v";
  int audioIndex = 0;
  std::vector<std::wstring> titles;
  if (wantMix) {
    cmd += L" -map \"[mix]\"";
    titles.push_back(L"Mix");
  }
  if (wantSeparate) {
    if (audioRate > 0) {
      cmd += L" -map 1:a";
      titles.push_back(L"Capture");
    }
    if (micRate > 0) {
      cmd += L" -map " + std::to_wstring(audioRate > 0 ? 2 : 1) + L":a";
      titles.push_back(L"Microphone");
    }
  }

  cmd += L" -c:v " + ToWide(encoder.ffmpegName) + EncoderOptions(settings, encoder);
  if (hdr_) {
    // Ten bit 4:2:0 and the three pieces of colour description that make a file
    // playable as HDR rather than as a washed out mess. They are not optional:
    // nothing else in the file says the picture is on the PQ curve, and a player
    // that is not told will assume it is not.
    cmd += L" -pix_fmt p010le";
    cmd += L" -color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc";
    cmd += L" -color_range tv";
  } else {
    // 4:2:0 eight bit: the one format every hardware encoder and every player
    // agrees on. Anything wider would exclude the very cards this is meant for.
    cmd += L" -pix_fmt nv12";
  }
  if (settings.rateControl == RateControl::Quality) {
    // A quality target and a bitrate are contradictory instructions, and ffmpeg
    // resolves them by quietly ignoring one. Better to send only one.
    cmd += L" -b:v 0";
  } else {
    cmd += L" -b:v " + std::to_wstring(settings.bitrateKbps) + L"k";
    // Constant means constant: the ceiling is the bitrate. Variable is allowed
    // to peak at half again, which is what makes it worth choosing.
    const int ceiling = settings.rateControl == RateControl::Cbr ? settings.bitrateKbps
                                                                 : settings.bitrateKbps * 3 / 2;
    cmd += L" -maxrate " + std::to_wstring(ceiling) + L"k";
    cmd += L" -bufsize " + std::to_wstring(ceiling * 2) + L"k";
  }

  // The old speed setting still applies when the new preset is left on
  // automatic, so nobody's existing configuration changes meaning.
  if (!encoder.hardware && settings.preset == EncoderPreset::Auto) {
    cmd += L" -preset " + ToWide(RecordSpeedName((int)settings.speed));
  }

  if (!titles.empty()) {
    cmd += L" -c:a aac -b:a 192k";
    // Named tracks, so a player and an editor both show which is which instead
    // of "Audio 1" and "Audio 2".
    for (const std::wstring& title : titles) {
      cmd += L" -metadata:s:a:" + std::to_wstring(audioIndex++) + L" title=\"" + title + L"\"";
    }
  }

  cmd += L" \"" + outFile + L"\"";
  return cmd;
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

  std::wstring folder = settings.outputFolder.empty() ? DefaultRecordFolder().native()
                                                      : ToWide(settings.outputFolder);
  if (!folder.empty() && (folder.back() == L'\\' || folder.back() == L'/')) folder.pop_back();
  if (!EnsureFolder(folder)) {
    return fail(CAP_SAID(T("Zielordner konnte nicht angelegt werden: ", "Could not create the folder: ") +
                         ToUtf8(folder)));
  }
  const std::wstring outFile = folder + L"\\" + TimestampedName(settings.container);

  // --- pipes -------------------------------------------------------------
  SECURITY_ATTRIBUTES sa = {};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE videoRead = nullptr;
  // A generous pipe buffer keeps a brief encoder stall from reaching back to
  // the writer thread.
  if (!::CreatePipe(&videoRead, &videoPipe_, &sa, 1 << 22)) {
    return fail(CAP_SAID(T("Videopipe konnte nicht erstellt werden.", "Could not create the video pipe.")));
  }
  ::SetHandleInformation(videoPipe_, HANDLE_FLAG_INHERIT, 0);

  // ffmpeg cannot take a second anonymous pipe -- only stdin is inheritable as a
  // standard handle -- so every audio track goes through a named pipe it opens
  // by path.
  auto makeAudioPipe = [&](const wchar_t* prefix, std::wstring* name) -> HANDLE {
    *name = std::wstring(L"\\\\.\\pipe\\") + prefix + std::to_wstring(::GetCurrentProcessId());
    HANDLE h = ::CreateNamedPipeW(name->c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
                                  PIPE_TYPE_BYTE | PIPE_WAIT, 1, 1 << 20, 1 << 20, 0, nullptr);
    return h == INVALID_HANDLE_VALUE ? nullptr : h;
  };

  if (audioRate_ > 0) {
    audioPipe_ = makeAudioPipe(L"qblank_audio_", &audioPipeName_);
    if (!audioPipe_) {
      ::CloseHandle(videoRead);
      return fail(CAP_SAID(T("Audiopipe konnte nicht erstellt werden.", "Could not create the audio pipe.")));
    }
  }
  if (micRate_ > 0) {
    micPipe_ = makeAudioPipe(L"qblank_mic_", &micPipeName_);
    if (!micPipe_) {
      ::CloseHandle(videoRead);
      return fail(CAP_SAID(T("Mikrofonpipe konnte nicht erstellt werden.",
                             "Could not create the microphone pipe.")));
    }
  }

  // --- process -----------------------------------------------------------
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  si.hStdInput = videoRead;
  si.hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);

  // ffmpeg reports everything on stderr. A windowed app has no usable standard
  // error, so without this pipe a failed encode leaves nothing but an exit
  // code -- which is how the first version of this managed to say nothing at
  // all about why it produced no file.
  HANDLE stderrWrite = nullptr;
  if (::CreatePipe(&stderrPipe_, &stderrWrite, &sa, 1 << 16)) {
    ::SetHandleInformation(stderrPipe_, HANDLE_FLAG_INHERIT, 0);
    si.hStdError = stderrWrite;
  } else {
    si.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
  }

  const std::wstring args = BuildCommandLine(settings, *encoder, audioPipeName_, micPipeName_,
                                            outFile, audioRate_, micRate_);
  std::wstring commandLine = L"\"" + ToWide(ffmpeg.path) + L"\" " + args;
  CAP_LOG("ffmpeg %s", ToUtf8(args).c_str());

  std::vector<wchar_t> mutableCmd(commandLine.begin(), commandLine.end());
  mutableCmd.push_back(L'\0');

  PROCESS_INFORMATION pi = {};
  const BOOL ok = ::CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  ::CloseHandle(videoRead);  // the child owns it now
  if (stderrWrite) ::CloseHandle(stderrWrite);
  if (!ok) {
    return fail(CAP_SAID(T("ffmpeg konnte nicht gestartet werden.", "Could not start ffmpeg.")));
  }
  ::CloseHandle(pi.hThread);
  process_ = pi.hProcess;

  // --- state -------------------------------------------------------------
  for (int i = 0; i < 3; ++i) slots_[i].assign(frameBytes_, 0);
  readyIdx_ = -1;
  heldIdx_ = -1;
  audioFramesWritten_.store(0, std::memory_order_relaxed);
  videoFramesWritten_.store(0, std::memory_order_relaxed);
  duplicated_.store(0, std::memory_order_relaxed);
  dropped_.store(0, std::memory_order_relaxed);
  bytesWritten_.store(0, std::memory_order_relaxed);
  startQpc_ = QpcNow();
  lastAudioSeen_ = 0;
  lastAudioProgressQpc_ = 0;
  audioStallLogged_ = false;
  failed_.store(false, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(infoMutex_);
    file_ = ToUtf8(outFile);
    encoderLabel_ = encoder->label;
    error_.clear();
  }

  running_.store(true, std::memory_order_relaxed);
  if (stderrPipe_) stderrThread_ = std::thread(&Recorder::StderrThread, this);
  if (audioRate_ > 0) {
    audioThread_ = std::thread(&Recorder::AudioThread, this, audioPipe_, pullAudio_, true);
  }
  if (micRate_ > 0) {
    micThread_ = std::thread(&Recorder::AudioThread, this, micPipe_, pullMic_, false);
  }
  videoThread_ = std::thread(&Recorder::VideoThread, this);

  CAP_LOG("Recording started: %s, %dx%d @ %.3f fps, %s", ToUtf8(outFile).c_str(), width_,
          height_, fps_, encoder->label.c_str());
  return true;
}

void Recorder::Stop() {
  const bool wasRunning = running_.exchange(false, std::memory_order_relaxed);

  if (videoThread_.joinable()) videoThread_.join();
  if (audioThread_.joinable()) audioThread_.join();
  if (micThread_.joinable()) micThread_.join();

  // Closing the pipes is what tells ffmpeg the streams ended. It then writes
  // the container index and exits -- an MP4 killed before that point has no
  // moov atom and will not play at all.
  if (videoPipe_) {
    ::CloseHandle(videoPipe_);
    videoPipe_ = nullptr;
  }
  if (audioPipe_) {
    ::CloseHandle(audioPipe_);
    audioPipe_ = nullptr;
  }
  if (micPipe_) {
    ::CloseHandle(micPipe_);
    micPipe_ = nullptr;
  }

  // The stderr reader ends by itself once the child closes its end, which
  // happens when ffmpeg exits -- so it is joined after the wait below.
  if (process_) {
    if (::WaitForSingleObject(process_, 15000) == WAIT_TIMEOUT) {
      CAP_WARN("ffmpeg does not exit, terminating it - the file may be incomplete");
      ::TerminateProcess(process_, 1);
    }
    DWORD code = 0;
    ::GetExitCodeProcess(process_, &code);
    if (wasRunning) {
      CAP_LOG("Recording stopped, ffmpeg exit code %lu, %llu frames", code,
              (unsigned long long)videoFramesWritten_.load());
    }
    ::CloseHandle(process_);
    process_ = nullptr;
  }

  if (stderrThread_.joinable()) stderrThread_.join();
  if (stderrPipe_) {
    ::CloseHandle(stderrPipe_);
    stderrPipe_ = nullptr;
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

bool Recorder::WriteAll(HANDLE pipe, const uint8_t* data, size_t size) {
  size_t written = 0;
  while (written < size) {
    DWORD chunk = 0;
    const DWORD want = (DWORD)std::min<size_t>(size - written, 1u << 20);
    if (!::WriteFile(pipe, data + written, want, &chunk, nullptr) || chunk == 0) return false;
    written += chunk;
  }
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
    const int64_t now = QpcNow();
    if (audioFrames != lastAudioSeen_) {
      lastAudioSeen_ = audioFrames;
      lastAudioProgressQpc_ = now;
    }
    const bool audioStalled =
        lastAudioProgressQpc_ != 0 && QpcSeconds(now - lastAudioProgressQpc_) > 2.0;
    const bool audioIsMaster = audioRate_ > 0 && audioFrames > 0 && !audioStalled;

    if (audioStalled && !audioStallLogged_) {
      audioStallLogged_ = true;
      CAP_WARN("Recording: audio stopped, frame timing continues on the system clock");
    }

    uint64_t target = 0;
    if (audioIsMaster) {
      target = (uint64_t)((double)audioFrames / (double)audioRate_ * fps_);
    } else {
      target = (uint64_t)(QpcSeconds(now - startQpc_) * fps_);
    }

    uint64_t written = videoFramesWritten_.load(std::memory_order_relaxed);
    if (written >= target) {
      ::Sleep(1);
      continue;
    }

    // Every frame in the timeline has to be written -- skipping one would make
    // the video shorter than the audio and desync the file for good. If the
    // encoder stalls, the pipe blocks here, which is exactly where the waiting
    // belongs.
    for (uint64_t i = written; i < target && running_.load(std::memory_order_relaxed); ++i) {
      if (!WriteAll(videoPipe_, held, frameBytes_)) {
        Fail(CAP_SAID(T("Verbindung zu ffmpeg abgebrochen.", "Lost the connection to ffmpeg.")));
        running_.store(false, std::memory_order_relaxed);
        return;
      }
      videoFramesWritten_.fetch_add(1, std::memory_order_relaxed);
      if (!fresh || i > written) duplicated_.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void Recorder::AudioThread(HANDLE pipe, AudioPullFn pull, bool countsAsClock) {
  // Wait for ffmpeg to open its end. Overlapped, so a ffmpeg that never starts
  // cannot leave this thread stuck and hang the whole shutdown.
  HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!event) return;

  OVERLAPPED ov = {};
  ov.hEvent = event;
  bool connected = false;
  if (::ConnectNamedPipe(pipe, &ov)) {
    connected = true;
  } else if (::GetLastError() == ERROR_PIPE_CONNECTED) {
    connected = true;
  } else if (::GetLastError() == ERROR_IO_PENDING) {
    while (running_.load(std::memory_order_relaxed)) {
      if (::WaitForSingleObject(event, 100) == WAIT_OBJECT_0) {
        connected = true;
        break;
      }
    }
  }
  if (!connected) {
    ::CancelIo(pipe);
    ::CloseHandle(event);
    return;
  }

  std::vector<float> buffer(4096 * 2);
  while (running_.load(std::memory_order_relaxed)) {
    const size_t got = pull ? pull(buffer.data(), 4096) : 0;
    if (got == 0) {
      ::Sleep(2);
      continue;
    }

    const size_t bytes = got * 2 * sizeof(float);
    ::ResetEvent(event);
    OVERLAPPED wov = {};
    wov.hEvent = event;
    DWORD wrote = 0;
    if (!::WriteFile(pipe, buffer.data(), (DWORD)bytes, &wrote, &wov)) {
      if (::GetLastError() != ERROR_IO_PENDING) {
        Fail(CAP_SAID(T("Audiopipe abgebrochen.", "The audio pipe broke.")));
        running_.store(false, std::memory_order_relaxed);
        break;
      }
      if (!::GetOverlappedResult(pipe, &wov, &wrote, TRUE)) {
        Fail(CAP_SAID(T("Audiopipe abgebrochen.", "The audio pipe broke.")));
        running_.store(false, std::memory_order_relaxed);
        break;
      }
    }
    bytesWritten_.fetch_add(wrote, std::memory_order_relaxed);
    if (countsAsClock) audioFramesWritten_.fetch_add(got, std::memory_order_relaxed);
  }

  ::CloseHandle(event);
}

void Recorder::StderrThread() {
  std::string pending;
  char buffer[1024];
  DWORD read = 0;
  while (::ReadFile(stderrPipe_, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
    pending.append(buffer, read);
    size_t nl;
    while ((nl = pending.find_first_of("\r\n")) != std::string::npos) {
      const std::string line = Trim(pending.substr(0, nl));
      pending.erase(0, nl + 1);
      if (!line.empty()) {
        CAP_WARN("ffmpeg: %s", line.c_str());
        std::lock_guard<std::mutex> lock(infoMutex_);
        if (error_.empty()) error_ = line;
      }
    }
    if (pending.size() > 4096) pending.clear();
  }
}

uint64_t Recorder::outputFileSize() const {
  std::wstring path;
  {
    std::lock_guard<std::mutex> lock(infoMutex_);
    if (file_.empty()) return 0;
    path = ToWide(file_);
  }
  WIN32_FILE_ATTRIBUTE_DATA data = {};
  if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
  return ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
}

RecordStats Recorder::stats() const {
  RecordStats s;
  s.running = running_.load(std::memory_order_relaxed);
  s.seconds = startQpc_ ? QpcSeconds(QpcNow() - startQpc_) : 0.0;
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
