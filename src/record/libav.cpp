#include "record/libav.h"

#include <atomic>
#include <cstdarg>
#include <memory>
#include <mutex>

#include "common.h"
#include "files.h"
#include "shared_library.h"

namespace cap {
namespace {

struct Library {
  const char* base;
  int major;
};

const Library kLibraries[] = {
    {"avutil", LIBAVUTIL_VERSION_MAJOR},
    {"swresample", LIBSWRESAMPLE_VERSION_MAJOR},
    {"avcodec", LIBAVCODEC_VERSION_MAJOR},
    {"avformat", LIBAVFORMAT_VERSION_MAJOR},
};
constexpr int kLibraryCount = (int)(sizeof(kLibraries) / sizeof(kLibraries[0]));

std::mutex g_loadMutex;
std::atomic<const Libav*> g_av{nullptr};

thread_local std::string t_firstError;
thread_local bool t_quiet = false;

// ffmpeg's errors, to the log and to LibavError. Called on whatever thread
// ffmpeg logs from, an encoder's own included. Everything below an error is
// dropped here: the defaults would write to a console nobody sees.
void LogCallback(void* context, int level, const char* format, va_list args) {
  if (level > AV_LOG_ERROR) return;
  const Libav* av = g_av.load(std::memory_order_acquire);
  if (!av) return;
  char line[1024];
  int printPrefix = 0;
  av->av_log_format_line2(context, level, format, args, line, sizeof(line), &printPrefix);
  std::string text = Trim(line);
  if (text.empty()) return;
  // The name of whatever logged it, without the address the default prefix
  // carries: "h264_nvenc: No capable devices found".
  const AVClass* cls = context ? *(const AVClass* const*)context : nullptr;
  if (cls && cls->item_name) text = std::string(cls->item_name(context)) + ": " + text;
  // The first since LibavClearError: what follows it is mostly the same failure
  // passed up, "Error while opening encoder" after the reason.
  if (t_firstError.empty()) t_firstError = text;
  if (!t_quiet) CAP_WARN("ffmpeg: %s", text.c_str());
}

}  // namespace

std::vector<std::string> LibavFileNames() {
  std::vector<std::string> names;
  for (const Library& l : kLibraries) names.push_back(SharedLibraryFileName(l.base, l.major));
  return names;
}

bool LibavFilesPresent(const std::filesystem::path& folder) {
  for (const std::string& name : LibavFileNames()) {
    if (!IsFile(folder / name)) return false;
  }
  return true;
}

const Libav* LoadLibav(const std::filesystem::path& folder, std::string* error) {
  if (const Libav* av = g_av.load(std::memory_order_acquire)) return av;
  std::lock_guard<std::mutex> lock(g_loadMutex);
  if (const Libav* av = g_av.load(std::memory_order_acquire)) return av;

  // All four there before any is opened: a set that is half there is a
  // download that has not finished, and opening part of it would keep those
  // files from being replaced.
  const std::vector<std::string> names = LibavFileNames();
  for (const std::string& name : names) {
    if (!IsFile(folder / name)) {
      ReportError(error, CAP_SAID(Format(T("%s fehlt in %s.", "%s is missing from %s."),
                                         name.c_str(), PathToUtf8(folder).c_str())));
      return nullptr;
    }
  }

  void* modules[kLibraryCount] = {};
  auto fail = [&]() -> const Libav* {
    for (int i = kLibraryCount - 1; i >= 0; --i) CloseSharedLibrary(modules[i]);
    return nullptr;
  };
  for (int i = 0; i < kLibraryCount; ++i) {
    modules[i] = OpenSharedLibrary(folder / names[i], error);
    if (!modules[i]) return fail();
  }

  auto av = std::make_unique<Libav>();
  const char* missing = nullptr;
  int missingIn = 0;
  auto find = [&](int library, const char* name) -> void* {
    void* symbol = SharedLibrarySymbol(modules[library], name);
    if (!symbol && !missing) {
      missing = name;
      missingIn = library;
    }
    return symbol;
  };
#define QB_AV_FIND(name) av->name = (decltype(av->name))find(library, #name);
  int library = 0;
  QB_AVUTIL_FUNCTIONS(QB_AV_FIND)
  library = 1;
  QB_SWRESAMPLE_FUNCTIONS(QB_AV_FIND)
  library = 2;
  QB_AVCODEC_FUNCTIONS(QB_AV_FIND)
  library = 3;
  QB_AVFORMAT_FUNCTIONS(QB_AV_FIND)
#undef QB_AV_FIND
  if (missing) {
    const std::string file = names[missingIn];
    ReportError(error, CAP_SAID(Format(T("%s fehlt die Funktion %s.", "%s lacks the function %s."),
                                       file.c_str(), missing)));
    return fail();
  }

  // The file names carry the major already, but a file can be named anything.
  const unsigned versions[kLibraryCount] = {av->avutil_version(), av->swresample_version(),
                                            av->avcodec_version(), av->avformat_version()};
  for (int i = 0; i < kLibraryCount; ++i) {
    const int major = (int)(versions[i] >> 16);
    if (major != kLibraries[i].major) {
      const std::string file = names[i];
      ReportError(error, CAP_SAID(Format(T("%s ist Version %d, gebraucht wird %d.",
                                           "%s is version %d, %d is needed."),
                                         file.c_str(), major, kLibraries[i].major)));
      return fail();
    }
  }

  av->version = av->av_version_info();
  av->folder = folder;
  CAP_LOG("ffmpeg %s loaded from %s", av->version.c_str(), PathToUtf8(folder).c_str());
  const Libav* loaded = av.release();
  g_av.store(loaded, std::memory_order_release);
  loaded->av_log_set_callback(&LogCallback);
  return loaded;
}

const Libav* Av() { return g_av.load(std::memory_order_acquire); }

std::string LibavError(int code) {
  if (!t_firstError.empty()) return t_firstError;
  const Libav* av = Av();
  char text[256] = {};
  if (av && av->av_strerror(code, text, sizeof(text)) == 0) return text;
  return Format("error %d", code);
}

void LibavClearError() { t_firstError.clear(); }

LibavQuiet::LibavQuiet() : was_(t_quiet) { t_quiet = true; }
LibavQuiet::~LibavQuiet() { t_quiet = was_; }

}  // namespace cap
