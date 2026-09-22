#pragma once

#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

// What the program is called, and where its own files are. Everything that
// spells out a name gets it from there -- see src/app_files.h.
#include "app_files.h"
#include "i18n.h"

// Short alias -- ComPtr shows up on almost every line of the DirectShow / D3D code.
template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

namespace cap {

// ---------------------------------------------------------------- string utils

// A path as UTF-8, for the log, the interface and the settings file -- and
// back. Text is UTF-8 everywhere in the program; a path stays a path until it
// has to be shown or stored. Neither direction throws: what does not convert
// cleanly comes out as U+FFFD.
std::string PathToUtf8(const std::filesystem::path& path);
std::filesystem::path Utf8ToPath(const std::string& text);

// Uppercase ASCII copy, used for case-insensitive id/name matching.
std::string ToUpper(std::string s);

// Trims ASCII whitespace from both ends.
std::string Trim(const std::string& s);

std::string Format(const char* fmt, ...);

// ---------------------------------------------------------------------- logging

// Which earlier sessions LogInit removes from the log before it adds a new one.
// Each rule stands on its own, and a session goes as soon as one of them says
// so. Always whole sessions, so what is left still starts with its first line.
struct LogRetention {
  bool byAge = true;
  int days = 14;
  bool byCount = true;
  int sessions = 10;  // including the one about to start
  bool olderVersions = false;
};

// The session before this one, if its end line is missing: the program went
// down without reaching LogEnd -- a crash, the task manager, the power.
struct UnfinishedSession {
  bool found = false;
  std::string version;
  SYSTEMTIME started = {};  // local time, all zero when the start line had none
};

// Writes to the debugger and, if enabled, to qBlank.log next to the exe. The
// file is kept across starts: every session opens with a line naming version
// and time, and LogEnd closes it with another. One without its closing line
// did not end normally, and LogInit says so.
UnfinishedSession LogInit(bool toFile, const LogRetention& keep);
void LogEnd();
void LogWrite(const char* level, const char* fmt, ...);

#define CAP_LOG(...)  ::cap::LogWrite("INFO", __VA_ARGS__)
#define CAP_WARN(...) ::cap::LogWrite("WARN", __VA_ARGS__)
#define CAP_ERR(...)  ::cap::LogWrite("ERR ", __VA_ARGS__)

// Logs the call site and returns hr, so it can be used inline in a condition.
HRESULT LogHrFailure(HRESULT hr, const char* expr, const char* file, int line);

#define CAP_HR(expr) ::cap::LogHrFailure((expr), #expr, __FILE__, __LINE__)

// Human readable HRESULT, e.g. "0x80070002 (The system cannot find the file
// specified)". The text comes from Windows: in English where T() speaks English
// and Windows has the English text, otherwise in the language of the system.
std::string HrToString(HRESULT hr);

// The same, always asking for English first -- for log lines written directly
// rather than through CAP_SAID.
std::string HrToEnglish(HRESULT hr);

// Hands a failure message to the caller in the interface language and writes
// it to the log in English, at the place it arises. `error` may be null. False,
// so `return ReportError(error, CAP_SAID(...));` works.
bool ReportError(std::string* error, const Said& said);

// ------------------------------------------------------------------ misc utils

// ExeFolder, OwnFile and the rest live in app_files.h, included above.

// What this build calls itself. Compared against the newest release tag on
// GitHub, so it has to line up with how those are named -- "v1.1" there against
// "1.1" here.
//
// Set by CMake from project(qBlank VERSION ...), which is also where the
// version resource in the executable comes from. Deliberately no fallback: a
// default here would be a second place the number can live, and the point is
// that there is only one.
#ifndef QBLANK_VERSION
#error "QBLANK_VERSION comes from CMake -- configure the build rather than compiling by hand."
#endif
inline const char* kAppVersion = QBLANK_VERSION;

// Creates a directory and every missing parent. True when it exists afterwards.
bool EnsureFolder(const std::filesystem::path& path);

// Bytes still free on the volume `path` lies on, for the user the program runs
// as -- a quota counts, which is what a recording actually runs into. False
// when the path cannot be reached at all; the folder itself need not exist yet,
// the nearest existing parent answers for it.
bool DiskFreeBytes(const std::filesystem::path& path, uint64_t* freeBytes);

// "412 GB", "3,7 TB". Binary prefixes, because that is what Explorer shows and
// two different numbers for the same disk is worse than either convention.
std::string FormatBytes(uint64_t bytes);

// "4 h 32 min", "18 min", "44 s". Rounded the way someone reads a clock rather
// than the way a stopwatch runs.
std::string FormatDuration(double seconds);

template <typename T>
T Clamp(T v, T lo, T hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// RAII wrapper for CoInitializeEx on the calling thread.
class ComScope {
 public:
  explicit ComScope(DWORD model = COINIT_APARTMENTTHREADED);
  ~ComScope();
  ComScope(const ComScope&) = delete;
  ComScope& operator=(const ComScope&) = delete;
  bool ok() const { return initialized_; }

 private:
  bool initialized_ = false;
};

}  // namespace cap
