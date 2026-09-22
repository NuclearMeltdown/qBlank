#include "common_win32.h"

#include <shlwapi.h>

#include <cstring>
#include <mutex>

#include "text_win32.h"

namespace cap {

namespace {

std::mutex g_log_mutex;
FILE* g_log_file = nullptr;
ULONGLONG g_log_opened = 0;

}  // namespace

// ---------------------------------------------------------------------- logging

namespace {

const char kUtf8Bom[] = "\xEF\xBB\xBF";

// "2026-09-16 14:03:22", local time -- read back by ParseSessionStart.
std::string Stamp(const SYSTEMTIME& st) {
  return Format("%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour,
                st.wMinute, st.wSecond);
}

ULONGLONG Seconds(const SYSTEMTIME& st) {
  FILETIME ft;
  if (!::SystemTimeToFileTime(&st, &ft)) return 0;
  return (((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000000ULL;
}

struct LogSession {
  size_t begin = 0;     // offset of its first line in the file
  std::string version;  // empty for text from before sessions were marked
  ULONGLONG started = 0;  // local time in seconds, 0 when unknown
};

// "===== qBlank 4.3.0 | started 2026-09-16 14:03:22 ====="
bool ParseSessionStart(const std::string& line, LogSession* session) {
  if (line.rfind("===== ", 0) != 0) return false;
  const size_t bar = line.find(" | started ");
  if (bar == std::string::npos) return false;
  const std::string who = line.substr(6, bar - 6);
  const size_t space = who.find_last_of(' ');
  if (space != std::string::npos) session->version = who.substr(space + 1);

  unsigned y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
  if (std::sscanf(line.c_str() + bar + 11, "%u-%u-%u %u:%u:%u", &y, &mo, &d, &h, &mi, &s) == 6) {
    SYSTEMTIME st = {};
    st.wYear = (WORD)y;
    st.wMonth = (WORD)mo;
    st.wDay = (WORD)d;
    st.wHour = (WORD)h;
    st.wMinute = (WORD)mi;
    st.wSecond = (WORD)s;
    session->started = Seconds(st);
  }
  return true;
}

// Numerically, part by part, so 4.10 comes after 4.9. No version at all is
// older than any.
bool OlderThanThisBuild(const std::string& version) {
  const auto parts = [](const std::string& v) {
    std::vector<int> out;
    int value = -1;
    for (char c : v) {
      if (c >= '0' && c <= '9') {
        value = (value < 0 ? 0 : value * 10) + (c - '0');
      } else if (c == '.' && value >= 0) {
        out.push_back(value);
        value = -1;
      } else {
        break;
      }
    }
    if (value >= 0) out.push_back(value);
    return out;
  };
  const std::vector<int> a = parts(version);
  const std::vector<int> b = parts(kAppVersion);
  if (a.empty()) return true;
  for (size_t i = 0; i < a.size() || i < b.size(); ++i) {
    const int x = i < a.size() ? a[i] : 0;
    const int y = i < b.size() ? b[i] : 0;
    if (x != y) return x < y;
  }
  return false;
}

// The whole file without its byte order mark, empty when there is none.
std::string ReadLog(const std::wstring& path) {
  std::string text;
  FILE* in = _wfopen(path.c_str(), L"rb");
  if (!in) return text;
  char buffer[1 << 16];
  size_t n = 0;
  while ((n = std::fread(buffer, 1, sizeof(buffer), in)) > 0) text.append(buffer, n);
  std::fclose(in);
  if (text.rfind(kUtf8Bom, 0) == 0) text.erase(0, 3);
  return text;
}

// The last session in the log, if no end line follows its start line.
bool FindUnfinished(const std::string& text, LogSession* last) {
  bool open = false;
  for (size_t pos = 0; pos < text.size();) {
    const size_t newline = text.find('\n', pos);
    const size_t end = newline == std::string::npos ? text.size() : newline;
    const std::string line = text.substr(pos, end - pos);
    LogSession session;
    if (ParseSessionStart(line, &session)) {
      *last = session;
      open = true;
    } else if (open && line.rfind("===== ", 0) == 0 && line.find(" | ended ") != std::string::npos) {
      open = false;
    }
    pos = end + 1;
  }
  return open;
}

// Another instance still writing to the log: one started from the same folder,
// or the build an update just replaced, still on its way out. Its session has
// no end line *yet*, and that is not a crash. A writer keeps the file open up
// to its end line, and while it does, an open that will not share writing is
// refused -- which also answers for builds from before this check.
bool LogInUseElsewhere(const std::wstring& path) {
  const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return ::GetLastError() == ERROR_SHARING_VIOLATION;
  ::CloseHandle(file);
  return false;
}

// The end line a session never got to write, put in at the next start: from
// the file's last change, which is about when its last line went in.
std::string LateEndLine(const std::wstring& path, const LogSession& session) {
  std::string line = Format("===== %s %s | ended abnormally", AppNameUtf8().c_str(),
                            session.version.c_str());
  WIN32_FILE_ATTRIBUTE_DATA info;
  SYSTEMTIME utc, local;
  if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) &&
      ::FileTimeToSystemTime(&info.ftLastWriteTime, &utc) &&
      ::SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) {
    line += " " + Stamp(local);
    const ULONGLONG last = Seconds(local);
    if (session.started != 0 && last >= session.started) {
      line += " after " + FormatDuration((double)(last - session.started));
    }
  }
  return line + " =====\r\n";
}

// Takes out the earlier sessions the rules are done with. The file is only
// rewritten when something goes, and through a second file, so a crash halfway
// cannot cost the part that was meant to stay.
void TrimLog(const std::wstring& path, const std::string& text, const LogRetention& keep) {
  if (!keep.byAge && !keep.byCount && !keep.olderVersions) return;
  if (text.empty()) return;

  // Text ahead of the first start line was written by a build that overwrote
  // the log on every start. It has neither date nor version, so every rule that
  // asks counts it as old.
  std::vector<LogSession> sessions;
  for (size_t pos = 0; pos < text.size();) {
    const size_t newline = text.find('\n', pos);
    const size_t end = newline == std::string::npos ? text.size() : newline;
    std::string line = text.substr(pos, end - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    LogSession session;
    session.begin = pos;
    if (ParseSessionStart(line, &session) || pos == 0) sessions.push_back(session);
    pos = end + 1;
  }

  SYSTEMTIME now;
  ::GetLocalTime(&now);
  const ULONGLONG nowSeconds = Seconds(now);
  const ULONGLONG maxAge = (ULONGLONG)std::max(keep.days, 1) * 24 * 60 * 60;
  const size_t maxSessions = (size_t)std::max(keep.sessions, 1);

  std::string kept;
  bool dropped = false;
  for (size_t i = 0; i < sessions.size(); ++i) {
    const LogSession& s = sessions[i];
    const size_t end = i + 1 < sessions.size() ? sessions[i + 1].begin : text.size();
    const bool tooOld = keep.byAge && (s.started == 0 || s.started + maxAge < nowSeconds);
    // The session about to start is one of the ones kept.
    const bool tooMany = keep.byCount && sessions.size() - i >= maxSessions;
    const bool outdated = keep.olderVersions && OlderThanThisBuild(s.version);
    if (tooOld || tooMany || outdated) {
      dropped = true;
    } else {
      kept.append(text, s.begin, end - s.begin);
    }
  }
  if (!dropped) return;

  // The blank line in front of a start line belongs to the session before it.
  // If that start line went, one is left over at the end.
  while (kept.size() >= 4 && kept.compare(kept.size() - 4, 4, "\r\n\r\n") == 0) {
    kept.resize(kept.size() - 2);
  }

  const std::wstring temp = path + L".tmp";
  FILE* out = _wfopen(temp.c_str(), L"wb");
  if (!out) return;
  bool ok = std::fwrite(kUtf8Bom, 1, 3, out) == 3;
  if (ok && !kept.empty()) ok = std::fwrite(kept.data(), 1, kept.size(), out) == kept.size();
  ok = std::fclose(out) == 0 && ok;
  if (!ok || !::MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    ::DeleteFileW(temp.c_str());
  }
}

// Caller holds g_log_mutex.
void WriteToLogFile(const std::string& text) {
  if (!g_log_file) return;
  std::fwrite(text.data(), 1, text.size(), g_log_file);
  std::fflush(g_log_file);
}

}  // namespace

UnfinishedSession LogInit(bool toFile, const LogRetention& keep) {
  UnfinishedSession unfinished;
  std::lock_guard<std::mutex> lock(g_log_mutex);
  if (g_log_file) {
    std::fclose(g_log_file);
    g_log_file = nullptr;
  }
  if (!toFile) return unfinished;
  const std::wstring path = OwnFile("log").native();
  std::string text = ReadLog(path);

  // Before the clean-up, which may well be about to take that very session out.
  LogSession last;
  if (!LogInUseElsewhere(path) && FindUnfinished(text, &last)) {
    unfinished.found = true;
    unfinished.version = last.version;
    // Back from the seconds ParseSessionStart made of the local time.
    const ULONGLONG ticks = last.started * 10000000ULL;
    const FILETIME ft = {(DWORD)ticks, (DWORD)(ticks >> 32)};
    SYSTEMTIME st;
    if (last.started != 0 && ::FileTimeToSystemTime(&ft, &st)) {
      unfinished.started = {st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond};
    }
    // A power cut can leave the last line half written.
    std::string end = text.empty() || text.back() == '\n' ? "" : "\r\n";
    end += LateEndLine(path, last);
    if (FILE* out = _wfopen(path.c_str(), L"ab")) {
      if (std::fwrite(end.data(), 1, end.size(), out) == end.size()) text += end;
      std::fclose(out);
    }
  }
  TrimLog(path, text, keep);

  // Binary, and the line ends written out: the bytes already in the file stay
  // exactly as they are.
  g_log_file = _wfopen(path.c_str(), L"ab");
  if (!g_log_file) return unfinished;
  g_log_opened = ::GetTickCount64();

  std::fseek(g_log_file, 0, SEEK_END);
  const __int64 size = _ftelli64(g_log_file);
  SYSTEMTIME st;
  ::GetLocalTime(&st);
  // A byte order mark on a new file, so no editor takes the text for the local
  // code page, and a blank line between sessions.
  std::string head = size <= 0 ? std::string(kUtf8Bom) : size > 3 ? "\r\n" : "";
  head += Format("===== %s %s | started %s =====\r\n", AppNameUtf8().c_str(), kAppVersion,
                 Stamp(st).c_str());
  WriteToLogFile(head);
  return unfinished;
}

void LogEnd() {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  if (!g_log_file) return;
  SYSTEMTIME st;
  ::GetLocalTime(&st);
  const double seconds = (double)(::GetTickCount64() - g_log_opened) / 1000.0;
  WriteToLogFile(Format("===== %s %s | ended %s after %s =====\r\n", AppNameUtf8().c_str(),
                        kAppVersion, Stamp(st).c_str(), FormatDuration(seconds).c_str()));
  std::fclose(g_log_file);
  g_log_file = nullptr;
}

void LogWrite(const char* level, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  va_list copy;
  va_copy(copy, args);
  int n = std::vsnprintf(nullptr, 0, fmt, copy);
  va_end(copy);
  std::string msg;
  if (n > 0) {
    msg.resize((size_t)n);
    std::vsnprintf(msg.data(), (size_t)n + 1, fmt, args);
  }
  va_end(args);

  SYSTEMTIME st;
  ::GetLocalTime(&st);
  const std::string line = Format("[%02u:%02u:%02u.%03u] %s %s", st.wHour, st.wMinute,
                                  st.wSecond, st.wMilliseconds, level, msg.c_str());

  std::lock_guard<std::mutex> lock(g_log_mutex);
  ::OutputDebugStringW(ToWide(line + "\n").c_str());
  WriteToLogFile(line + "\r\n");
}

namespace {

std::string SystemMessage(HRESULT hr, LANGID language) {
  LPWSTR buf = nullptr;
  DWORD n = ::FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, (DWORD)hr, language, (LPWSTR)&buf, 0, nullptr);
  std::string text;
  if (n && buf) {
    text = Trim(ToUtf8(std::wstring(buf, n)));
  }
  if (buf) ::LocalFree(buf);
  return text;
}

}  // namespace

std::string HrToString(HRESULT hr) {
  // A German Windows usually carries no English message table, and then the
  // English request fails outright -- so the system's own language follows.
  // The code in front stays either way, and that is what a search finds.
  std::string text;
  if (SpeakingEnglish()) text = SystemMessage(hr, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
  if (text.empty()) text = SystemMessage(hr, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT));
  if (text.empty()) return Format("0x%08X", (unsigned)hr);
  return Format("0x%08X (%s)", (unsigned)hr, text.c_str());
}

std::string HrToEnglish(HRESULT hr) {
  EnglishScope english;
  return HrToString(hr);
}

HRESULT LogHrFailure(HRESULT hr, const char* expr, const char* file, int line) {
  if (FAILED(hr)) {
    const char* base = file;
    for (const char* p = file; *p; ++p) {
      if (*p == '\\' || *p == '/') base = p + 1;
    }
    LogWrite("ERR ", "%s:%d  %s -> %s", base, line, expr, HrToEnglish(hr).c_str());
  }
  return hr;
}

// ----------------------------------------------------------------------- clock

int64_t ClockTicks() {
  LARGE_INTEGER v;
  ::QueryPerformanceCounter(&v);
  return v.QuadPart;
}

double TicksToSeconds(int64_t ticks) {
  static const double freq = [] {
    LARGE_INTEGER f;
    ::QueryPerformanceFrequency(&f);
    return (double)f.QuadPart;
  }();
  return (double)ticks / freq;
}

// ------------------------------------------------------------------ misc utils

namespace {

bool EnsureFolderW(const std::wstring& path) {
  if (path.empty()) return false;
  if (::CreateDirectoryW(path.c_str(), nullptr)) return true;
  const DWORD err = ::GetLastError();
  if (err == ERROR_ALREADY_EXISTS) return true;
  if (err != ERROR_PATH_NOT_FOUND) return false;

  const size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return false;
  if (!EnsureFolderW(path.substr(0, slash))) return false;
  return ::CreateDirectoryW(path.c_str(), nullptr) || ::GetLastError() == ERROR_ALREADY_EXISTS;
}

}  // namespace

bool EnsureFolder(const std::filesystem::path& path) { return EnsureFolderW(path.native()); }

bool DiskFreeBytes(const std::filesystem::path& path, uint64_t* freeBytes) {
  if (freeBytes) *freeBytes = 0;
  if (path.empty()) return false;

  std::wstring probe = path.native();
  while (!probe.empty()) {
    ULARGE_INTEGER avail = {};
    // Der erste der drei Werte und nicht der zweite: er ist das, was diesem
    // Benutzer zur Verfuegung steht, und gegen den laeuft eine Aufnahme. Wo ein
    // Kontingent gesetzt ist, sind die beiden verschieden.
    if (::GetDiskFreeSpaceExW(probe.c_str(), &avail, nullptr, nullptr)) {
      if (freeBytes) *freeBytes = (uint64_t)avail.QuadPart;
      return true;
    }

    // Nach oben, bis ein Verzeichnis antwortet: der Aufnahmeordner muss noch
    // nicht angelegt sein, wenn jemand in den Einstellungen nachsieht.
    const size_t slash = probe.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    if (slash < 3) {
      // An der Wurzel angekommen. "C:" allein meint das aktuelle Verzeichnis
      // dieses Laufwerks und nicht die Wurzel, deshalb der Schraegstrich -- und
      // antwortet die Wurzel nicht, gibt es nichts mehr zu fragen.
      probe = probe.substr(0, slash) + L"\\";
      ULARGE_INTEGER root = {};
      if (!::GetDiskFreeSpaceExW(probe.c_str(), &root, nullptr, nullptr)) return false;
      if (freeBytes) *freeBytes = (uint64_t)root.QuadPart;
      return true;
    }
    probe.resize(slash);
  }
  return false;
}

ComScope::ComScope(DWORD model) {
  HRESULT hr = ::CoInitializeEx(nullptr, model);
  // RPC_E_CHANGED_MODE means the thread is already initialised in a different
  // apartment; we must not call CoUninitialize in that case.
  initialized_ = SUCCEEDED(hr);
}

ComScope::~ComScope() {
  if (initialized_) ::CoUninitialize();
}

}  // namespace cap
