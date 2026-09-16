#include "common.h"

#include <shlwapi.h>

#include <cstring>
#include <mutex>

namespace cap {

namespace {

std::mutex g_log_mutex;
FILE* g_log_file = nullptr;

}  // namespace

// ---------------------------------------------------------------- string utils

std::string ToUtf8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string out((size_t)n, '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(), n, nullptr, nullptr);
  return out;
}

std::wstring ToWide(const std::string& s) {
  if (s.empty()) return {};
  int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  if (n <= 0) return {};
  std::wstring out((size_t)n, L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
  return out;
}

std::string ToUpper(std::string s) {
  for (char& c : s) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
  }
  return s;
}

std::string Trim(const std::string& s) {
  const char* kWhitespace = " \t\r\n";
  size_t b = s.find_first_not_of(kWhitespace);
  if (b == std::string::npos) return {};
  size_t e = s.find_last_not_of(kWhitespace);
  return s.substr(b, e - b + 1);
}

std::string Format(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  va_list copy;
  va_copy(copy, args);
  int n = std::vsnprintf(nullptr, 0, fmt, copy);
  va_end(copy);
  std::string out;
  if (n > 0) {
    out.resize((size_t)n);
    std::vsnprintf(out.data(), (size_t)n + 1, fmt, args);
  }
  va_end(args);
  return out;
}

// ---------------------------------------------------------------------- logging

void LogInit(bool toFile) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  if (g_log_file) {
    std::fclose(g_log_file);
    g_log_file = nullptr;
  }
  if (!toFile) return;
  const std::wstring path = AppFile(L"log");
  g_log_file = _wfopen(path.c_str(), L"w, ccs=UTF-8");
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
  std::string line = Format("[%02u:%02u:%02u.%03u] %s %s\n", st.wHour, st.wMinute, st.wSecond,
                            st.wMilliseconds, level, msg.c_str());

  std::lock_guard<std::mutex> lock(g_log_mutex);
  ::OutputDebugStringW(ToWide(line).c_str());
  if (g_log_file) {
    std::fputws(ToWide(line).c_str(), g_log_file);
    std::fflush(g_log_file);
  }
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

bool ReportError(std::string* error, const Said& said) {
  LogWrite("ERR ", "%s", said.logged.c_str());
  if (error) *error = said.shown;
  return false;
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

// ------------------------------------------------------------------ misc utils

bool EnsureFolder(const std::wstring& path) {
  if (path.empty()) return false;
  if (::CreateDirectoryW(path.c_str(), nullptr)) return true;
  const DWORD err = ::GetLastError();
  if (err == ERROR_ALREADY_EXISTS) return true;
  if (err != ERROR_PATH_NOT_FOUND) return false;

  const size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return false;
  if (!EnsureFolder(path.substr(0, slash))) return false;
  return ::CreateDirectoryW(path.c_str(), nullptr) || ::GetLastError() == ERROR_ALREADY_EXISTS;
}

bool DiskFreeBytes(const std::wstring& path, uint64_t* freeBytes) {
  if (freeBytes) *freeBytes = 0;
  if (path.empty()) return false;

  std::wstring probe = path;
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

std::string FormatBytes(uint64_t bytes) {
  static const char* kUnits[] = {"B", "KB", "MB", "GB", "TB", "PB"};
  double value = (double)bytes;
  int unit = 0;
  while (value >= 1024.0 && unit + 1 < (int)(sizeof(kUnits) / sizeof(kUnits[0]))) {
    value /= 1024.0;
    ++unit;
  }
  if (unit == 0) return Format("%llu B", (unsigned long long)bytes);
  // Eine Nachkommastelle nur, solange sie etwas aussagt: bei 412 GB ist die
  // dritte Stelle Rauschen, bei 3,7 TB ist sie die Antwort.
  return Format(value < 10.0 ? "%.1f %s" : "%.0f %s", value, kUnits[unit]);
}

std::string FormatDuration(double seconds) {
  if (!(seconds > 0.0)) return "0 s";
  if (seconds < 60.0) return Format("%.0f s", seconds);
  const long long total = (long long)(seconds + 0.5);
  const long long hours = total / 3600;
  const long long minutes = (total % 3600) / 60;
  if (hours <= 0) return Format("%lld min", minutes);
  return Format("%lld h %lld min", hours, minutes);
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
