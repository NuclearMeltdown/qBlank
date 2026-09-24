#include "vcam/virtual_camera.h"

#include <shellapi.h>

#include <algorithm>
#include <vector>

#include "common.h"
#include "app_identity.h"
#include "i18n.h"
#include "inflate.h"
#include "text_win32.h"
#include "vcam/vcam_shared.h"
#include "vcam_packed.h"

#pragma comment(lib, "shell32.lib")

namespace cap {
namespace {

// The filter's file name carries a hash of its own contents.
//
// This is not tidiness, it is the whole mechanism. Windows identifies a loaded
// module inside a process by its *file name*, not by its path: an application
// that has the camera open holds qblank_vcam.dll mapped, and a later
// LoadLibrary of a different file with that same name hands back the module
// already loaded rather than reading the new one. Renaming the old file out of
// the way does not help -- the module keeps its pages either way, which is
// exactly what made this look unfixable from the outside: installing appeared
// to work, and the old code kept answering.
//
// A name that changes with the contents cannot be confused with anything. It
// also means an install is idempotent: the file for this build either is
// already there or is not.
//
// The program's name in front, lower-cased, so the prefix follows a rename --
// and so the former names stay recognisable, which is what lets the sweep below
// clear out what a build under an older name left in the folder.
std::wstring DllPrefix(const wchar_t* appName) {
  std::wstring prefix = appName;
  for (wchar_t& c : prefix) {
    if (c >= L'A' && c <= L'Z') c = (wchar_t)(c + (L'a' - L'A'));
  }
  return prefix + L"_vcam_";
}

const wchar_t kDllSuffix[] = L".dll";

// Hashes the packed bytes: packing is deterministic, so they change exactly
// when the filter does, and naming the file needs no unpacking.
std::wstring DllFileName() {
  static const std::wstring name = [] {
    uint64_t hash = 1469598103934665603ull;  // FNV-1a, as the shader cache uses
    for (size_t i = 0; i < kVcamFilter.size; ++i) {
      hash = (hash ^ kVcamFilter.data[i]) * 1099511628211ull;
    }
    wchar_t buffer[64] = {};
    ::swprintf(buffer, 64, L"%s%016llx%s", DllPrefix(kAppName).c_str(),
               (unsigned long long)hash, kDllSuffix);
    return std::wstring(buffer);
  }();
  return name;
}

std::wstring DllPath() { return ExeDirectory() + DllFileName(); }

bool FileThere(const std::wstring& path) {
  const DWORD attr = ::GetFileAttributesW(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool ReadInprocServer(HKEY root, const std::wstring& key, std::wstring* out) {
  HKEY handle = nullptr;
  if (::RegOpenKeyExW(root, key.c_str(), 0, KEY_READ, &handle) != ERROR_SUCCESS) return false;
  wchar_t value[MAX_PATH] = {};
  DWORD bytes = sizeof(value) - sizeof(wchar_t);
  DWORD type = 0;
  const LONG status = ::RegQueryValueExW(handle, nullptr, nullptr, &type, (BYTE*)value, &bytes);
  ::RegCloseKey(handle);
  if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return false;
  *out = value;
  return true;
}

// Reads back what the registration points at, so "installed" can mean the file
// is actually there rather than merely that a key exists. A build that was
// moved leaves exactly that trap behind.
//
// The per-user hive is asked first because that is the order COM itself uses:
// a class registered under HKEY_CURRENT_USER shadows the machine-wide one, so
// the per-user entry -- if there is one -- names the DLL that would really be
// loaded. Reporting the machine entry while COM quietly loads another file is
// the kind of disagreement that costs an evening to find.
bool RegisteredPath(std::wstring* out) {
  std::wstring key = L"Software\\Classes\\CLSID\\";
  key += vcam::kFilterClsidString;
  key += L"\\InprocServer32";
  if (ReadInprocServer(HKEY_CURRENT_USER, key, out)) return true;
  return ReadInprocServer(HKEY_LOCAL_MACHINE, key, out);
}

// Writes the filter that this executable carries next to it. The file it would
// replace is very likely in use -- every application with the camera open holds
// it -- but since the name follows the contents, a file that is already there
// is already the right one and nothing needs replacing.
bool WriteFilterDll(std::string* error) {
  const std::wstring target = DllPath();
  if (FileThere(target)) return true;

  std::vector<uint8_t> data(kVcamFilter.unpackedSize);
  if (!Inflate(kVcamFilter, data.data())) {
    ReportError(error, CAP_SAID(T("Die Kameraquelle in dieser qBlank-Fassung ist beschädigt.",
                                  "The camera source in this build of qBlank is damaged.")));
    return false;
  }
  const DWORD bytes = (DWORD)data.size();

  HANDLE file = ::CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    ReportError(error, CAP_SAID(T("Konnte die Kameraquelle nicht in den Programmordner schreiben.",
                                  "Could not write the camera source into the program folder.")));
    return false;
  }
  DWORD written = 0;
  const bool ok = ::WriteFile(file, data.data(), bytes, &written, nullptr) && written == bytes;
  ::CloseHandle(file);
  if (!ok) {
    ReportError(error, CAP_SAID(T("Die Kameraquelle wurde nicht vollständig geschrieben.",
                                  "The camera source was not written in full.")));
    return false;
  }
  CAP_LOG("Camera source written: %lu bytes", (unsigned long)bytes);
  return true;
}

// regsvr32 under UAC. Waiting for it matters: without that the settings tab
// would report the old state right after the user clicked.
bool RunRegsvr(bool remove, std::string* error) {
  const std::wstring dll = DllPath();
  if (!FileThere(dll)) {
    ReportError(error, CAP_SAID(T("qblank_vcam.dll fehlt neben qBlank.exe.",
                                  "qblank_vcam.dll is missing next to qBlank.exe.")));
    return false;
  }

  std::wstring args = remove ? L"/s /u \"" : L"/s \"";
  args += dll;
  args += L"\"";

  SHELLEXECUTEINFOW info = {};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
  info.lpVerb = L"runas";  // this is the UAC prompt
  info.lpFile = L"regsvr32.exe";
  info.lpParameters = args.c_str();
  info.nShow = SW_HIDE;

  if (!::ShellExecuteExW(&info) || !info.hProcess) {
    const DWORD err = ::GetLastError();
    CAP_ERR("Camera: ShellExecuteEx(runas, regsvr32) failed, error %lu", (unsigned long)err);
    ReportError(error,
                CAP_SAID(err == ERROR_CANCELLED
                             ? T("Abgebrochen -- ohne Administratorrechte geht es nicht.",
                                 "Cancelled -- this cannot be done without administrator rights.")
                             : T("regsvr32 ließ sich nicht starten.", "regsvr32 could not be started.")));
    return false;
  }

  ::WaitForSingleObject(info.hProcess, 30000);
  DWORD code = 1;
  ::GetExitCodeProcess(info.hProcess, &code);
  ::CloseHandle(info.hProcess);

  CAP_LOG("Camera: regsvr32 %s exited with %lu", remove ? "/u" : "", (unsigned long)code);

  if (code != 0) {
    // regsvr32's own numbering, said plainly. "It failed" is what made this
    // take three rounds to understand, so the number goes in the message.
    auto why = [code]() {
      return code == 3   ? T("die DLL ließ sich nicht laden", "the DLL would not load")
             : code == 4 ? T("der Einstiegspunkt fehlt", "the entry point is missing")
             : code == 5 ? T("abgelehnt -- vermutlich fehlen Administratorrechte",
                             "refused -- probably missing administrator rights")
                         : T("unbekannter Grund", "reason unknown");
    };
    return ReportError(
        error, CAP_SAID(std::string(remove ? T("Entfernen fehlgeschlagen (", "Removing failed (")
                                           : T("Registrierung fehlgeschlagen (",
                                               "Registration failed (")) +
                        why() + ", regsvr32 " + std::to_string((int)code) + ")."));
  }
  return true;
}

// --------------------------------------------------------------------------
// RGBA to NV12, at the size it arrives.
//
// Nothing is scaled and nothing is letterboxed here any more: that is the
// filter's job now, once per consumer, in the consumer's own process. What
// qBlank publishes is what qBlank is showing.
//
// BT.601 with limited range, which is what a consumer assumes of NV12 when it
// is not told otherwise, and what the filter's media type declares.
// --------------------------------------------------------------------------
void RgbaToNv12(const uint8_t* rgba, int srcStride, int width, int height, uint8_t* dst,
                int dstStride) {
  uint8_t* luma = dst;
  uint8_t* chroma = dst + (size_t)dstStride * height;

  // Two rows at a time: the chroma plane is half height, so a pair of luma rows
  // produces one row of it and the source is only walked once.
  for (int y = 0; y < height; y += 2) {
    const uint8_t* row0 = rgba + (size_t)y * srcStride;
    const uint8_t* row1 = row0 + srcStride;
    uint8_t* y0 = luma + (size_t)y * dstStride;
    uint8_t* y1 = y0 + dstStride;
    uint8_t* uv = chroma + (size_t)(y / 2) * dstStride;

    for (int x = 0; x < width; x += 2) {
      const uint8_t* px[4] = {row0 + (size_t)x * 4, row0 + (size_t)(x + 1) * 4,
                              row1 + (size_t)x * 4, row1 + (size_t)(x + 1) * 4};
      uint8_t* at[4] = {y0 + x, y0 + x + 1, y1 + x, y1 + x + 1};

      int rSum = 0, gSum = 0, bSum = 0;
      for (int i = 0; i < 4; ++i) {
        const int r = px[i][0], g = px[i][1], b = px[i][2];
        rSum += r;
        gSum += g;
        bSum += b;
        *at[i] = (uint8_t)std::clamp((66 * r + 129 * g + 25 * b + 128) / 256 + 16, 16, 235);
      }

      // Chroma from the average of the four, which is what 4:2:0 means.
      const int r = rSum / 4, g = gSum / 4, b = bSum / 4;
      uv[x] = (uint8_t)std::clamp((-38 * r - 74 * g + 112 * b + 128) / 256 + 128, 16, 240);
      uv[x + 1] = (uint8_t)std::clamp((112 * r - 94 * g - 18 * b + 128) / 256 + 128, 16, 240);
    }
  }
}

// The same into P010. What arrives is already on the PQ curve and already
// BT.2020 -- the shader that feeds the recorder had to do both anyway -- so
// nothing here touches a curve. It is a colour matrix and some packing.
void PackedToP010(const uint8_t* packed, int srcStride, int width, int height, uint8_t* dst,
                  int dstStride) {
  uint8_t* luma = dst;
  uint8_t* chroma = dst + (size_t)dstStride * height;

  for (int y = 0; y < height; y += 2) {
    // DXGI packs R10G10B10A2 with red in the low bits.
    const uint32_t* row0 = (const uint32_t*)(packed + (size_t)y * srcStride);
    const uint32_t* row1 = (const uint32_t*)(packed + (size_t)(y + 1) * srcStride);
    uint16_t* y0 = (uint16_t*)(luma + (size_t)y * dstStride);
    uint16_t* y1 = (uint16_t*)(luma + (size_t)(y + 1) * dstStride);
    uint16_t* uv = (uint16_t*)(chroma + (size_t)(y / 2) * dstStride);

    for (int x = 0; x < width; x += 2) {
      const uint32_t src[4] = {row0[x], row0[x + 1], row1[x], row1[x + 1]};
      uint16_t* at[4] = {y0 + x, y0 + x + 1, y1 + x, y1 + x + 1};

      float mean[3] = {0.0f, 0.0f, 0.0f};
      for (int i = 0; i < 4; ++i) {
        const float rgb[3] = {(float)(src[i] & 0x3FFu) / 1023.0f,
                              (float)((src[i] >> 10) & 0x3FFu) / 1023.0f,
                              (float)((src[i] >> 20) & 0x3FFu) / 1023.0f};
        for (int c = 0; c < 3; ++c) mean[c] += rgb[c] * 0.25f;
        // BT.2020 non-constant luminance, limited range, ten bits.
        const float v = 0.2627f * rgb[0] + 0.6780f * rgb[1] + 0.0593f * rgb[2];
        *at[i] = (uint16_t)((uint32_t)std::clamp((int)(v * 876.0f + 64.5f), 64, 940) << 6);
      }

      const float v = 0.2627f * mean[0] + 0.6780f * mean[1] + 0.0593f * mean[2];
      const int cb = (int)((mean[2] - v) / 1.8814f * 896.0f + 512.5f);
      const int cr = (int)((mean[0] - v) / 1.4746f * 896.0f + 512.5f);
      uv[x] = (uint16_t)((uint32_t)std::clamp(cb, 64, 960) << 6);
      uv[x + 1] = (uint16_t)((uint32_t)std::clamp(cr, 64, 960) << 6);
    }
  }
}

// 100 ns since this process started, which is what the slot header carries.
int64_t NowIn100ns() {
  static LARGE_INTEGER frequency = [] {
    LARGE_INTEGER f = {};
    ::QueryPerformanceFrequency(&f);
    return f;
  }();
  static const int64_t origin = [] {
    LARGE_INTEGER t = {};
    ::QueryPerformanceCounter(&t);
    return t.QuadPart;
  }();
  LARGE_INTEGER now = {};
  ::QueryPerformanceCounter(&now);
  if (frequency.QuadPart <= 0) return 0;
  return (now.QuadPart - origin) * 10000000 / frequency.QuadPart;
}

}  // namespace

// ------------------------------------------------------------- installation

VirtualCamera::~VirtualCamera() { Stop(); }

VirtualCamera::Install VirtualCamera::Status() {
  std::wstring registered;
  if (!RegisteredPath(&registered)) return Install::Missing;
  // Registered is not enough: it has to be *this* build's filter. Anything else
  // is a leftover from an earlier one, and saying so is the difference between
  // an install that helps and one that changes nothing.
  if (_wcsicmp(registered.c_str(), DllPath().c_str()) != 0) return Install::Stale;
  return FileThere(registered) ? Install::Installed : Install::Stale;
}

bool VirtualCamera::InstallSource(std::string* error) {
  // Always lay down a fresh copy first. Registering only writes registry
  // entries; pointing them at a stale file again would change nothing, which is
  // exactly the loop this used to sit in.
  if (!WriteFilterDll(error)) return false;
  if (!RunRegsvr(false, error)) return false;
  CleanUpOldSources();
  return true;
}

bool VirtualCamera::UninstallSource(std::string* error) { return RunRegsvr(true, error); }

void VirtualCamera::CleanUpOldSources() {
  // Everything that looks like a filter except the one this build uses. Files
  // from older builds keep their own names, so they accumulate unless somebody
  // sweeps them; whatever is still mapped by a running consumer simply refuses
  // and gets another chance at some later start.
  //
  // Under the former names too, because a rename does not move those files: the
  // program that wrote them is this one, and nothing else will ever come back
  // for them.
  std::vector<std::wstring> prefixes = {DllPrefix(kAppName)};
  for (size_t i = 0; i < kFormerAppNameCount; ++i) {
    prefixes.push_back(DllPrefix(kFormerAppNames[i]));
  }

  const std::wstring folder = ExeDirectory();
  const std::wstring keep = DllFileName();
  int removed = 0;
  for (const std::wstring& prefix : prefixes) {
    WIN32_FIND_DATAW found = {};
    HANDLE search = ::FindFirstFileW((folder + prefix.substr(0, prefix.size() - 1) + L"*").c_str(),
                                     &found);
    if (search == INVALID_HANDLE_VALUE) continue;
    do {
      if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      if (_wcsicmp(found.cFileName, keep.c_str()) == 0) continue;
      if (::DeleteFileW((folder + found.cFileName).c_str())) ++removed;
    } while (::FindNextFileW(search, &found));
    ::FindClose(search);
  }
  if (removed > 0) CAP_LOG("Camera source: %d old file(s) removed", removed);
}

// ---------------------------------------------------------------- lifetime

void VirtualCamera::SetWideOffered(bool offered) {
  wantsWide_.store(offered, std::memory_order_relaxed);
}

void VirtualCamera::SetSourceShape(int width, int height, double fps, bool wide) {
  // Even edges, because 4:2:0 has no other kind, and the filter refuses odd
  // ones rather than guessing what half a chroma sample would mean.
  const uint32_t w = width > 1 ? (uint32_t)(width & ~1) : 0u;
  const uint32_t h = height > 1 ? (uint32_t)(height & ~1) : 0u;
  const uint32_t pixel = wide ? vcam::kPixelP010 : vcam::kPixelNv12;

  // Anything outside this is not a frame rate, it is a mistake, and writing it
  // would have every consumer negotiate against nonsense. An unknown rate keeps
  // whatever was there, which at worst is the thirty the camera starts on.
  int64_t interval = sourceInterval_.load(std::memory_order_relaxed);
  if (fps > 0.1 && fps < 10000.0) interval = (int64_t)(10000000.0 / fps + 0.5);

  sourceWidth_.store(w, std::memory_order_relaxed);
  sourceHeight_.store(h, std::memory_order_relaxed);
  sourcePixel_.store(pixel, std::memory_order_relaxed);
  sourceInterval_.store(interval, std::memory_order_relaxed);

  auto* cb = static_cast<vcam::ControlBlock*>(controlView_);
  if (!cb) return;
  if (cb->sourceWidth != w) cb->sourceWidth = w;
  if (cb->sourceHeight != h) cb->sourceHeight = h;
  if (cb->sourcePixel != pixel) cb->sourcePixel = pixel;
  if (cb->sourceInterval100ns != interval) cb->sourceInterval100ns = interval;
}

void VirtualCamera::StartAsync() {
  // Kept asynchronous in name only. Starting used to mean waking the frame
  // server and creating a Media Foundation camera, which was slow enough to
  // need a thread; now it is a shared section and a thread of our own, and
  // doing that on a background thread would only make the failure arrive late.
  if (running()) return;
  starting_ = true;
  std::string error;
  if (!StartBlocking(&error)) {
    std::lock_guard<std::mutex> lock(errorMutex_);
    startError_ = error;
  }
  starting_ = false;
}

bool VirtualCamera::takeError(std::string* out) {
  std::lock_guard<std::mutex> lock(errorMutex_);
  if (startError_.empty()) return false;
  if (out) *out = startError_;
  startError_.clear();
  return true;
}

bool VirtualCamera::StartBlocking(std::string* error) {
  if (running()) return true;

  if (Status() != Install::Installed) {
    ReportError(error, CAP_SAID(T("Die Kameraquelle ist nicht installiert.",
                                  "The camera source is not installed.")));
    return false;
  }

  if (!CreateControl(error)) return false;

  quit_ = false;
  wake_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
  running_ = true;
  worker_ = std::thread(&VirtualCamera::WorkerLoop, this);
  CAP_LOG("Virtual camera started");
  return true;
}

void VirtualCamera::Stop() {
  starting_ = false;
  if (running_.exchange(false)) {
    quit_ = true;
    if (wake_) ::SetEvent(wake_);
    if (worker_.joinable()) worker_.join();
  }
  if (wake_) {
    ::CloseHandle(wake_);
    wake_ = nullptr;
  }
  DestroyControl();
  consumed_ = false;
}

bool VirtualCamera::CreateControl(std::string* error) {
  controlSection_ = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                         sizeof(vcam::ControlBlock), vcam::kControlSectionName);
  const DWORD created = ::GetLastError();
  if (!controlSection_) {
    CAP_ERR("Virtual camera: CreateFileMapping failed, error %lu",
            (unsigned long)created);
    ReportError(error, CAP_SAID(T("Der geteilte Speicher für die Kamera ließ sich nicht anlegen.",
                                  "The camera's shared memory could not be created.")));
    return false;
  }
  controlView_ = ::MapViewOfFile(controlSection_, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0,
                                 sizeof(vcam::ControlBlock));
  if (!controlView_) {
    ::CloseHandle(controlSection_);
    controlSection_ = nullptr;
    ReportError(error, CAP_SAID(T("Der geteilte Speicher für die Kamera ließ sich nicht abbilden.",
                                  "The camera's shared memory could not be mapped.")));
    return false;
  }

  auto* cb = static_cast<vcam::ControlBlock*>(controlView_);
  const bool reused = created == ERROR_ALREADY_EXISTS && cb->magic == vcam::kMagic &&
                      cb->version == vcam::kVersion &&
                      cb->stateBytes == sizeof(vcam::ControlBlock);
  if (reused) {
    // Somebody was here before -- almost always this qBlank being restarted
    // while a consumer kept the section alive. Those consumers are still valid
    // and still in the table, so it is left alone; only the producer's half is
    // taken over. Carrying the generation forward matters too: reusing a number
    // a consumer already has mapped would leave it looking at a dead section.
    generation_ = cb->frameGeneration;
    CAP_LOG("Virtual camera: took over the existing control block (generation %u)",
            (unsigned)generation_);
  } else {
    ::memset(cb, 0, sizeof(*cb));
    cb->magic = vcam::kMagic;
    cb->version = vcam::kVersion;
    cb->stateBytes = sizeof(vcam::ControlBlock);
    // Not from zero: a consumer that survived a qBlank which did not get to
    // clean up would otherwise be handed a generation it already has mapped.
    generation_ = ::GetTickCount() | 1u;
  }

  cb->sourceWidth = sourceWidth_.load(std::memory_order_relaxed);
  cb->sourceHeight = sourceHeight_.load(std::memory_order_relaxed);
  cb->sourcePixel = sourcePixel_.load(std::memory_order_relaxed);
  cb->sourceInterval100ns = sourceInterval_.load(std::memory_order_relaxed);
  cb->framesPublished = 0;
  cb->frameGeneration = 0;  // no pictures yet; the first push makes the section
  cb->producerAlive = 1;

  // All eight wake events, up front and held open for as long as the camera
  // runs. A consumer creates its own too, but only while it streams; holding
  // them here means the name stays valid and the two never race over it.
  for (uint32_t i = 0; i < vcam::kConsumerSlots; ++i) {
    wchar_t name[128];
    vcam::FormatName(name, 128, vcam::kWakeEventPrefix, i);
    consumerWake_[i] = ::CreateEventW(nullptr, FALSE, FALSE, name);
  }

  writeIndex_ = 0;
  published_ = 0;
  return true;
}

void VirtualCamera::DestroyControl() {
  if (auto* cb = static_cast<vcam::ControlBlock*>(controlView_)) {
    // Consumers watch this. Without it they would keep showing the last picture
    // for as long as their mapping lives, which looks like a frozen camera
    // rather than a stopped one.
    cb->producerAlive = 0;
    cb->frameGeneration = 0;
  }
  for (uint32_t i = 0; i < vcam::kConsumerSlots; ++i) {
    if (consumerWake_[i]) {
      ::SetEvent(consumerWake_[i]);  // one last wake, so nobody sleeps on a corpse
      ::CloseHandle(consumerWake_[i]);
      consumerWake_[i] = nullptr;
    }
  }
  if (frameView_) {
    ::UnmapViewOfFile(frameView_);
    frameView_ = nullptr;
  }
  if (frameSection_) {
    ::CloseHandle(frameSection_);
    frameSection_ = nullptr;
  }
  frameWidth_ = frameHeight_ = framePixel_ = 0;
  if (controlView_) {
    ::UnmapViewOfFile(controlView_);
    controlView_ = nullptr;
  }
  if (controlSection_) {
    ::CloseHandle(controlSection_);
    controlSection_ = nullptr;
  }
}

// ------------------------------------------------------------- the handover

void VirtualCamera::PushFrame(const uint8_t* rgba, int stride, int width, int height) {
  if (!running() || !rgba || width <= 0 || height <= 0) return;
  // Nobody watching costs nothing. The flag is set by the worker from the
  // consumer table, so this is one relaxed load per displayed frame.
  if (!consumed()) return;

  {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    pending_.resize((size_t)width * height * 4);
    for (int row = 0; row < height; ++row) {
      ::memcpy(pending_.data() + (size_t)row * width * 4, rgba + (size_t)row * stride,
               (size_t)width * 4);
    }
    pendingWidth_ = width;
    pendingHeight_ = height;
    pendingWide_ = false;
    pendingFull_ = true;
  }
  if (wake_) ::SetEvent(wake_);
}

void VirtualCamera::PushFrameWide(const uint8_t* packed, int stride, int width, int height) {
  if (!running() || !packed || width <= 0 || height <= 0) return;
  if (!wantsWide() || !consumed()) return;

  {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    pending_.resize((size_t)width * height * 4);
    for (int row = 0; row < height; ++row) {
      ::memcpy(pending_.data() + (size_t)row * width * 4, packed + (size_t)row * stride,
               (size_t)width * 4);
    }
    pendingWidth_ = width;
    pendingHeight_ = height;
    pendingWide_ = true;
    pendingFull_ = true;
  }
  if (wake_) ::SetEvent(wake_);
}

// --------------------------------------------------------------- publishing

bool VirtualCamera::EnsureFrameSection(uint32_t width, uint32_t height, uint32_t pixel) {
  if (frameView_ && frameWidth_ == width && frameHeight_ == height && framePixel_ == pixel) {
    return true;
  }

  auto* cb = static_cast<vcam::ControlBlock*>(controlView_);
  if (!cb) return false;

  // Let go of the old one first. Consumers still reading it keep it alive
  // through their own handles until they notice the generation moved, so this
  // is a release rather than a demolition.
  if (frameView_) {
    ::UnmapViewOfFile(frameView_);
    frameView_ = nullptr;
  }
  if (frameSection_) {
    ::CloseHandle(frameSection_);
    frameSection_ = nullptr;
  }
  frameWidth_ = frameHeight_ = framePixel_ = 0;

  const uint32_t strideY = width * vcam::BytesPerSample(pixel);
  uint64_t slotBytes = vcam::PictureBytes(strideY, height);
  slotBytes = (slotBytes + 4095ull) & ~4095ull;  // a slot to itself, page for page
  const uint64_t bytes = vcam::FrameSectionBytes(slotBytes);

  // A generation number nobody else holds. Colliding is close to impossible and
  // entirely survivable: take the next one.
  HANDLE section = nullptr;
  uint32_t generation = generation_;
  for (int attempt = 0; attempt < 64 && !section; ++attempt) {
    if (++generation == 0) generation = 1;
    wchar_t name[128];
    vcam::FormatName(name, 128, vcam::kFrameSectionPrefix, generation);
    HANDLE tried = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                        (DWORD)(bytes >> 32), (DWORD)(bytes & 0xFFFFFFFFull),
                                        name);
    if (tried && ::GetLastError() == ERROR_ALREADY_EXISTS) {
      ::CloseHandle(tried);
      continue;
    }
    if (!tried) {
      CAP_ERR("Virtual camera: frame memory (%llu bytes) failed, error %lu",
              (unsigned long long)bytes, (unsigned long)::GetLastError());
      return false;
    }
    section = tried;
  }
  if (!section) return false;

  void* view = ::MapViewOfFile(section, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, 0);
  if (!view) {
    ::CloseHandle(section);
    return false;
  }

  auto* fs = static_cast<vcam::FrameSectionHeader*>(view);
  ::memset(fs, 0, sizeof(*fs));
  fs->magic = vcam::kMagic;
  fs->version = vcam::kVersion;
  fs->generation = generation;
  fs->slotCount = vcam::kSlotCount;
  fs->width = width;
  fs->height = height;
  fs->strideY = strideY;
  fs->pixel = pixel;
  fs->slotBytes = slotBytes;

  frameSection_ = section;
  frameView_ = view;
  frameWidth_ = width;
  frameHeight_ = height;
  framePixel_ = pixel;
  generation_ = generation;
  writeIndex_ = 0;

  // Last, and only now: the header is whole, so a consumer that follows this
  // pointer finds something it can read.
  ::MemoryBarrier();
  cb->frameGeneration = generation;

  CAP_LOG("Virtual camera: %ux%u %s, generation %u, %llu bytes", (unsigned)width,
          (unsigned)height, pixel == vcam::kPixelP010 ? "P010" : "NV12", (unsigned)generation,
          (unsigned long long)bytes);
  return true;
}

void VirtualCamera::Publish(const uint8_t* pixels, int stride, int width, int height,
                            uint32_t pixel) {
  auto* cb = static_cast<vcam::ControlBlock*>(controlView_);
  if (!cb) return;

  // Even edges, because 4:2:0 has no other kind. A source with an odd dimension
  // loses its last row or column rather than its chroma plane.
  const uint32_t w = (uint32_t)(width & ~1);
  const uint32_t h = (uint32_t)(height & ~1);
  if (w < 2 || h < 2) return;
  if (!EnsureFrameSection(w, h, pixel)) return;

  auto* fs = static_cast<vcam::FrameSectionHeader*>(frameView_);
  const uint32_t index = writeIndex_ % vcam::kSlotCount;
  vcam::SlotHeader& head = fs->slots[index];
  uint8_t* dst = vcam::SlotData(frameView_, fs->slotBytes, index);

  // Odd while writing, even when whole. The reader checks the number either
  // side of its copy, so it can tell a torn read from a good one without
  // either side ever waiting for the other.
  head.sequence = head.sequence + 1;
  ::MemoryBarrier();

  if (pixel == vcam::kPixelP010) {
    PackedToP010(pixels, stride, (int)w, (int)h, dst, (int)fs->strideY);
  } else {
    RgbaToNv12(pixels, stride, (int)w, (int)h, dst, (int)fs->strideY);
  }

  head.width = w;
  head.height = h;
  head.strideY = fs->strideY;
  head.pixel = pixel;
  head.frameIndex = ++published_;
  head.timestamp100ns = NowIn100ns();

  ::MemoryBarrier();
  head.sequence = head.sequence + 1;
  ::MemoryBarrier();

  ++writeIndex_;
  cb->framesPublished = published_;
  WakeConsumers();
}

void VirtualCamera::WakeConsumers() {
  auto* cb = static_cast<vcam::ControlBlock*>(controlView_);
  if (!cb) return;
  for (uint32_t i = 0; i < vcam::kConsumerSlots; ++i) {
    if (cb->consumers[i].inUse && consumerWake_[i]) ::SetEvent(consumerWake_[i]);
  }
}

// ---------------------------------------------------------------- consumers

void VirtualCamera::PruneConsumers() {
  auto* cb = static_cast<vcam::ControlBlock*>(controlView_);
  if (!cb) return;

  const DWORD now = ::GetTickCount();
  bool anyLive = false;

  for (uint32_t i = 0; i < vcam::kConsumerSlots; ++i) {
    vcam::ConsumerSlot& s = cb->consumers[i];
    if (!s.inUse) continue;

    const bool quiet = (uint32_t)(now - s.lastSeenMs) > vcam::kConsumerStaleMs;
    if (quiet) {
      // Quiet is not gone: a filter negotiating a format in a preview dialog can
      // be quiet for a while. Only a quiet slot whose process has actually died
      // is reclaimed, and asking the operating system is the one answer that
      // cannot be wrong.
      bool alive = true;
      if (HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, s.pid)) {
        alive = ::WaitForSingleObject(process, 0) != WAIT_OBJECT_0;
        ::CloseHandle(process);
      } else if (::GetLastError() == ERROR_INVALID_PARAMETER) {
        alive = false;  // no such process
      }
      if (!alive) {
        CAP_LOG("Virtual camera: reader %u (PID %lu) gone", (unsigned)i,
                (unsigned long)s.pid);
        s.streaming = 0;
        s.framesServed = 0;
        ::InterlockedExchange((volatile LONG*)&s.inUse, 0);
        continue;
      }
    }
    if (s.streaming && !quiet) anyLive = true;
  }

  if (consumed_.exchange(anyLive) != anyLive) {
    CAP_LOG("Virtual camera: readers %s", anyLive ? "present" : "gone");
  }
}

void VirtualCamera::consumers(std::vector<Consumer>* out) const {
  if (!out) return;
  out->clear();
  auto* cb = static_cast<vcam::ControlBlock*>(controlView_);
  if (!cb) return;

  const DWORD now = ::GetTickCount();
  for (uint32_t i = 0; i < vcam::kConsumerSlots; ++i) {
    const vcam::ConsumerSlot& s = cb->consumers[i];
    if (!s.inUse) continue;
    if (s.streaming && (uint32_t)(now - s.lastSeenMs) > vcam::kConsumerStaleMs) continue;

    Consumer c;
    // Bounded rather than trusted: the name is written by another process, and
    // a missing terminator would otherwise be read as far as the page allows.
    c.name = ToUtf8(std::wstring(s.name, ::wcsnlen(s.name, 63)));
    c.pid = s.pid;
    c.width = (int)s.width;
    c.height = (int)s.height;
    c.fps = s.frameInterval100ns > 0 ? 10000000.0 / (double)s.frameInterval100ns : 0.0;
    c.wide = s.pixel == vcam::kPixelP010;
    c.streaming = s.streaming != 0;
    out->push_back(std::move(c));
  }
}

// ------------------------------------------------------------------ worker

void VirtualCamera::WorkerLoop() {
  ::SetThreadDescription(::GetCurrentThread(), L"qBlank vcam");

  while (!quit_.load(std::memory_order_relaxed)) {
    ::WaitForSingleObject(wake_, 100);
    if (quit_.load(std::memory_order_relaxed)) break;

    PruneConsumers();

    std::vector<uint8_t> frame;
    int width = 0, height = 0;
    bool wide = false;
    {
      std::lock_guard<std::mutex> lock(pendingMutex_);
      if (!pendingFull_) continue;
      frame.swap(pending_);
      width = pendingWidth_;
      height = pendingHeight_;
      wide = pendingWide_;
      pendingFull_ = false;
    }
    if (width <= 0 || height <= 0) continue;

    Publish(frame.data(), width * 4, width, height,
            wide ? vcam::kPixelP010 : vcam::kPixelNv12);

    // Once a second, what both ends think is happening. All of this crosses a
    // process boundary, so without it the only symptom available is "the
    // picture is black".
    const DWORD now = ::GetTickCount();
    if (now - lastReport_ > 1000) {
      lastReport_ = now;
      auto* cb = static_cast<vcam::ControlBlock*>(controlView_);
      if (cb) {
        unsigned live = 0;
        for (uint32_t i = 0; i < vcam::kConsumerSlots; ++i) {
          if (cb->consumers[i].inUse) ++live;
        }
        CAP_LOG("Virtual camera: %ux%u %s, readers %u, published %u", (unsigned)frameWidth_,
                (unsigned)frameHeight_, framePixel_ == vcam::kPixelP010 ? "P010" : "NV12", live,
                (unsigned)published_);
      }
    }
  }
}

}  // namespace cap
