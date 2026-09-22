// The bridge out of the old name.
//
// This program ships in every release, forever, under the file name the program
// had before the rename -- CapView.exe. The updater in CapView 3.7 looks for an
// asset with exactly that name, downloads it over itself and restarts it, which
// means the last build under the old name walks into this one on its own. That
// is the entire trick: no installer, no helper service, no second channel.
//
// Because it is dragged along from release to release without ever being
// rebuilt, it must not know what the program is called now. It does not. It asks
// the release which file is the program, takes the name from there, and says
// that name back to the user. A second rename years from now needs nothing from
// this file -- the same binary keeps working, because the only name it spells
// out is the one it was born with, and that one cannot change.
//
// What it does, in order:
//   * asks GitHub for the newest release, by repository number
//   * shows what is about to happen, with the real name and version in it
//   * downloads the program asset, under whatever name the release gives it
//   * unregisters a virtual camera registered from this folder, if there is one
//   * points desktop and start menu shortcuts at the new executable
//   * renames itself to "<new name>.exe.old", which the new build deletes at its
//     first start, and starts that build
//
// Settings are not touched. The new build asks about those itself -- see
// FindForeignSettings in config.h -- because only the person in front of the
// screen knows which of two files has the work in it.

#include <windows.h>

#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <cstdarg>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Only the name-independent half of this header is used here: where the running
// file is, what it is called, how to repoint a shortcut. Everything derived from
// kAppName belongs to the program, not to this bridge -- kAppName in a build
// made today already says the new name, and this file must never assume it.
#include "app_identity.h"
#include "i18n.h"
#include "json.h"
#include "resource.h"
#include "update/release_source.h"
#include "vcam/vcam_shared.h"

namespace cap {
namespace {

// The DirectShow category every video capture device is listed under. Asking it
// which filters are installed is how a camera left behind by an older build is
// found even when its class is not one this build knows.
const wchar_t kVideoInputCategory[] = L"{860BB310-5D01-11D0-BD3B-00A0C911CE86}";

enum class Phase {
  Checking,  // asking the server what there is
  Ask,       // waiting for the button
  Working,   // downloading and moving things
  Failed,
};

enum : int {
  IDC_PRIMARY = 1001,
  IDC_SECONDARY = 1002,
};

enum : UINT {
  WM_PHASE = WM_APP + 1,
  WM_FINISHED = WM_APP + 2,  // the work is done and the new program is running
};

struct State {
  std::mutex mutex;
  Phase phase = Phase::Checking;
  std::string heading;
  std::string body;
  std::vector<std::string> bullets;
  std::string status;
  std::string primary;
  std::string secondary;

  // What the release said. Filled by the check, used by the install.
  std::string version;
  std::string assetUrl;
  std::wstring assetName;  // the file name to save it under
  std::wstring newStem;    // that name without ".exe" -- the program's new name
  std::string pageUrl;
};

State g_state;
HWND g_window = nullptr;
HFONT g_font = nullptr;
HFONT g_headingFont = nullptr;
bool g_dark = false;
float g_scale = 1.0f;
int g_dots = 0;

// ----------------------------------------------------------------- small tools

std::wstring Widen(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring out((size_t)n, L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
  return out;
}

std::string Narrow(const std::wstring& s) {
  if (s.empty()) return std::string();
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr,
                                      nullptr);
  std::string out((size_t)n, '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n, nullptr, nullptr);
  return out;
}

std::string Format(const char* fmt, ...) {
  char buffer[1024] = {};
  va_list args;
  va_start(args, fmt);
  ::vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  return buffer;
}

bool Exists(const std::wstring& path) {
  return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool SameText(const std::wstring& a, const std::wstring& b) {
  return ::CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) ==
         CSTR_EQUAL;
}

bool StartsWith(const std::wstring& text, const std::wstring& prefix) {
  return text.size() >= prefix.size() && SameText(text.substr(0, prefix.size()), prefix);
}

std::string Megabytes(long long bytes) {
  if (bytes <= 0) return std::string();
  return Format(" (%.1f MB)", (double)bytes / (1024.0 * 1024.0));
}

// ------------------------------------------------------------------- language

// The question has to be asked in the language the settings are in, and the
// settings are the one thing here that predates this program. Recognised by
// content rather than by file name, for the same reason everything else is: the
// file may already have been renamed by a previous run.
bool LooksLikeSettings(const json::Value& root, int* language) {
  if (!root.IsObject() || !root["version"].IsNumber()) return false;
  if (!root["app"].IsObject() || !root["record"].IsObject()) return false;
  const json::Value& value = root["app"]["language"];
  *language = value.IsNumber() ? value.AsInt() : -1;
  return true;
}

void SpeakTheirLanguage() {
  WIN32_FIND_DATAW found = {};
  const std::wstring folder = ExeDirectory();
  const HANDLE search = ::FindFirstFileW((folder + L"*.json").c_str(), &found);
  if (search == INVALID_HANDLE_VALUE) return;
  int language = -1;
  do {
    if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    HANDLE file = ::CreateFileW((folder + found.cFileName).c_str(), GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) continue;
    LARGE_INTEGER size = {};
    ::GetFileSizeEx(file, &size);
    std::string text;
    if (size.QuadPart > 0 && size.QuadPart < 4 * 1024 * 1024) {
      text.resize((size_t)size.QuadPart);
      DWORD read = 0;
      if (!::ReadFile(file, text.data(), (DWORD)text.size(), &read, nullptr)) text.clear();
      text.resize(read);
    }
    ::CloseHandle(file);
    if (text.empty()) continue;
    int candidate = -1;
    if (LooksLikeSettings(json::Parse(text), &candidate) && candidate >= 0) {
      language = candidate;
      break;
    }
  } while (::FindNextFileW(search, &found));
  ::FindClose(search);
  if (language >= 0) SetLanguage(language == 1 ? Language::English : Language::German);
}

// --------------------------------------------------------------------- camera

// A DLL registered from this folder, and the class it is registered under. The
// class matters because removing the registration means handing regsvr32 the
// DLL, and the DLL is what the class points at.
struct RegisteredFilter {
  std::wstring clsid;
  std::wstring dll;
};

bool ReadDefaultString(HKEY root, const std::wstring& key, std::wstring* out) {
  HKEY handle = nullptr;
  if (::RegOpenKeyExW(root, key.c_str(), 0, KEY_READ, &handle) != ERROR_SUCCESS) return false;
  wchar_t value[MAX_PATH * 2] = {};
  DWORD bytes = sizeof(value) - sizeof(wchar_t);
  DWORD type = 0;
  const LONG status = ::RegQueryValueExW(handle, nullptr, nullptr, &type, (BYTE*)value, &bytes);
  ::RegCloseKey(handle);
  if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return false;
  *out = value;
  return true;
}

bool InprocServerOf(const std::wstring& clsid, std::wstring* dll) {
  const std::wstring key = L"Software\\Classes\\CLSID\\" + clsid + L"\\InprocServer32";
  if (ReadDefaultString(HKEY_CURRENT_USER, key, dll)) return true;
  return ReadDefaultString(HKEY_LOCAL_MACHINE, key, dll);
}

void AddIfOurs(const std::wstring& clsid, std::vector<RegisteredFilter>* out) {
  std::wstring dll;
  if (!InprocServerOf(clsid, &dll) || dll.empty()) return;
  // Only files inside the folder this program is being replaced in. Somebody
  // else's camera is somebody else's business, and a registration pointing
  // somewhere else was not put there by any build of this program.
  if (!StartsWith(dll, ExeDirectory())) return;
  for (const RegisteredFilter& seen : *out) {
    if (SameText(seen.dll, dll)) return;
  }
  out->push_back({clsid, dll});
}

// Every camera filter registered out of this folder. The two classes this
// program has used are checked outright; the category is then walked for
// anything else, so a filter from a build older than this one is still found.
std::vector<RegisteredFilter> RegisteredCameras() {
  std::vector<RegisteredFilter> out;
  AddIfOurs(vcam::kFilterClsidString, &out);
  AddIfOurs(vcam::kLegacySourceClsidString, &out);

  const std::wstring instances =
      std::wstring(L"Software\\Classes\\CLSID\\") + kVideoInputCategory + L"\\Instance";
  const HKEY roots[] = {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE};
  for (HKEY root : roots) {
    HKEY handle = nullptr;
    if (::RegOpenKeyExW(root, instances.c_str(), 0, KEY_READ, &handle) != ERROR_SUCCESS) continue;
    for (DWORD i = 0;; ++i) {
      wchar_t name[256] = {};
      DWORD length = (DWORD)std::size(name);
      if (::RegEnumKeyExW(handle, i, name, &length, nullptr, nullptr, nullptr, nullptr) !=
          ERROR_SUCCESS) {
        break;
      }
      // The instance key names the filter's own class in its "CLSID" value; the
      // key name itself is only an identifier for the listing.
      HKEY entry = nullptr;
      if (::RegOpenKeyExW(handle, name, 0, KEY_READ, &entry) != ERROR_SUCCESS) continue;
      wchar_t clsid[64] = {};
      DWORD bytes = sizeof(clsid) - sizeof(wchar_t);
      DWORD type = 0;
      const LONG status = ::RegQueryValueExW(entry, L"CLSID", nullptr, &type, (BYTE*)clsid, &bytes);
      ::RegCloseKey(entry);
      if (status == ERROR_SUCCESS && type == REG_SZ) AddIfOurs(clsid, &out);
    }
    ::RegCloseKey(handle);
  }
  return out;
}

// One elevated pass over all of them. Several registrations would otherwise mean
// several consent prompts in a row, which looks exactly like something going
// wrong.
bool UnregisterCameras(const std::vector<RegisteredFilter>& filters) {
  if (filters.empty()) return true;

  std::wstring file = L"regsvr32.exe";
  std::wstring args = L"/s /u \"" + filters.front().dll + L"\"";
  if (filters.size() > 1) {
    file = L"cmd.exe";
    args = L"/c ";
    for (size_t i = 0; i < filters.size(); ++i) {
      if (i) args += L" & ";
      args += L"regsvr32.exe /s /u \"" + filters[i].dll + L"\"";
    }
  }

  SHELLEXECUTEINFOW info = {};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
  info.lpVerb = L"runas";  // this is the consent prompt
  info.lpFile = file.c_str();
  info.lpParameters = args.c_str();
  info.nShow = SW_HIDE;
  if (!::ShellExecuteExW(&info) || !info.hProcess) return false;
  ::WaitForSingleObject(info.hProcess, 60000);
  DWORD code = 1;
  ::GetExitCodeProcess(info.hProcess, &code);
  ::CloseHandle(info.hProcess);
  return code == 0;
}

// ----------------------------------------------------------------- the window

void SetStatus(const std::string& text) {
  {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    g_state.status = text;
  }
  if (g_window) ::InvalidateRect(g_window, nullptr, TRUE);
}

void SetPhase(Phase phase) {
  {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    g_state.phase = phase;
  }
  if (g_window) ::PostMessageW(g_window, WM_PHASE, 0, 0);
}

bool SystemUsesDarkMode() {
  HKEY key = nullptr;
  if (::RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                      KEY_READ, &key) != ERROR_SUCCESS) {
    return false;
  }
  DWORD light = 1, bytes = sizeof(light), type = 0;
  const LONG status =
      ::RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type, (BYTE*)&light, &bytes);
  ::RegCloseKey(key);
  return status == ERROR_SUCCESS && type == REG_DWORD && light == 0;
}

COLORREF Background() { return g_dark ? RGB(24, 22, 29) : RGB(247, 246, 250); }
COLORREF Foreground() { return g_dark ? RGB(228, 226, 234) : RGB(28, 27, 34); }
COLORREF Dim() { return g_dark ? RGB(150, 146, 162) : RGB(104, 100, 118); }
COLORREF Accent() { return RGB(0x8B, 0x5C, 0xF6); }

int Scaled(int value) { return (int)(value * g_scale + 0.5f); }

// Breaking text into lines by hand, because DrawText's DT_WORDBREAK cannot be
// trusted with German. Its word logic still runs through the system's ANSI code
// page, and where that page has no letter at the umlaut's position it treats the
// character as a word boundary: "übernehmen" comes out as "ü" at the end of one
// line and "bernehmen" at the start of the next, with room to spare. Measured,
// not guessed -- the same sentence with a plain "u" wraps correctly.
std::vector<std::wstring> WrapLines(HDC dc, const std::wstring& text, int width) {
  std::vector<std::wstring> lines;
  size_t start = 0;
  while (start < text.size()) {
    const int rest = (int)(text.size() - start);
    INT fit = 0;
    SIZE size = {};
    if (!::GetTextExtentExPointW(dc, text.c_str() + start, rest, width, &fit, nullptr, &size) ||
        fit >= rest) {
      lines.push_back(text.substr(start));
      break;
    }
    size_t take = fit > 0 ? (size_t)fit : 1;
    // Back off to the last space that still fits. A word longer than the whole
    // line has nowhere to go and is broken where it ran out.
    const size_t space = text.rfind(L' ', start + take);
    if (space != std::wstring::npos && space > start) {
      lines.push_back(text.substr(start, space - start));
      start = space + 1;
    } else {
      lines.push_back(text.substr(start, take));
      start += take;
    }
  }
  if (lines.empty()) lines.emplace_back();
  return lines;
}

// Lays the text out and reports how tall it came out. Called once with a device
// context that draws nowhere, to size the window, and again for every repaint.
int PaintOrMeasure(HDC dc, int width, bool draw) {
  State copy;
  {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    copy.phase = g_state.phase;
    copy.heading = g_state.heading;
    copy.body = g_state.body;
    copy.bullets = g_state.bullets;
    copy.status = g_state.status;
  }

  const int margin = Scaled(24);
  int y = margin;

  // The indent is the whole reason a bullet is not just text with a dot in
  // front: a bullet that wraps has to wrap under its own first word, not back
  // to the margin.
  auto block = [&](const std::string& text, HFONT font, COLORREF colour, int gap, int indent) {
    if (text.empty()) return;
    ::SelectObject(dc, font);
    ::SetTextColor(dc, colour);
    TEXTMETRICW metrics = {};
    ::GetTextMetricsW(dc, &metrics);
    const int lineHeight = metrics.tmHeight + metrics.tmExternalLeading;
    const int left = margin + indent;
    const std::vector<std::wstring> lines = WrapLines(dc, Widen(text), width - margin - left);
    if (draw) {
      for (size_t i = 0; i < lines.size(); ++i) {
        ::TextOutW(dc, left, y + (int)i * lineHeight, lines[i].c_str(), (int)lines[i].size());
      }
      if (indent > 0) ::TextOutW(dc, margin, y, L"•", 1);
    }
    y += (int)lines.size() * lineHeight + gap;
  };

  block(copy.heading, g_headingFont, Accent(), Scaled(14), 0);
  block(copy.body, g_font, Foreground(), Scaled(12), 0);
  for (const std::string& bullet : copy.bullets) {
    block(bullet, g_font, Foreground(), Scaled(9), Scaled(18));
  }
  if (!copy.status.empty()) {
    y += Scaled(6);
    const bool waiting = copy.phase == Phase::Checking || copy.phase == Phase::Working;
    block(copy.status + (waiting ? std::string((size_t)g_dots, '.') : std::string()), g_font, Dim(),
          0, 0);
  }
  return y + margin;
}

void LayOutButtons(HWND hwnd) {
  RECT client = {};
  ::GetClientRect(hwnd, &client);
  const int margin = Scaled(24);
  const int width = Scaled(190);
  const int height = Scaled(36);
  const int gap = Scaled(10);
  const int y = client.bottom - margin - height;
  ::SetWindowPos(::GetDlgItem(hwnd, IDC_PRIMARY), nullptr, client.right - margin - width, y, width,
                 height, SWP_NOZORDER);
  ::SetWindowPos(::GetDlgItem(hwnd, IDC_SECONDARY), nullptr,
                 client.right - margin - width * 2 - gap, y, width, height, SWP_NOZORDER);
}

void ApplyPhase(HWND hwnd) {
  State copy;
  {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    copy.phase = g_state.phase;
    copy.primary = g_state.primary;
    copy.secondary = g_state.secondary;
  }
  const HWND primary = ::GetDlgItem(hwnd, IDC_PRIMARY);
  const HWND secondary = ::GetDlgItem(hwnd, IDC_SECONDARY);
  ::SetWindowTextW(primary, Widen(copy.primary).c_str());
  ::SetWindowTextW(secondary, Widen(copy.secondary).c_str());
  ::ShowWindow(primary, copy.primary.empty() ? SW_HIDE : SW_SHOW);
  ::ShowWindow(secondary, copy.secondary.empty() ? SW_HIDE : SW_SHOW);
  ::EnableWindow(primary, copy.phase != Phase::Working);
  ::EnableWindow(secondary, copy.phase != Phase::Working);
  if (!copy.primary.empty() && copy.phase != Phase::Working) ::SetFocus(primary);
  ::InvalidateRect(hwnd, nullptr, TRUE);
}

// Trims the window to the height its text needs, plus room for the buttons.
void FitToContent(HWND hwnd) {
  RECT window = {}, client = {};
  ::GetWindowRect(hwnd, &window);
  ::GetClientRect(hwnd, &client);
  const HDC dc = ::GetDC(hwnd);
  const int text = PaintOrMeasure(dc, client.right, false);
  ::ReleaseDC(hwnd, dc);

  const int wanted = text + Scaled(36) + Scaled(24);
  const int chrome = (window.bottom - window.top) - (client.bottom - client.top);
  const int height = wanted + chrome;
  const int width = window.right - window.left;
  RECT work = {0, 0, 0, 0};
  ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  ::SetWindowPos(hwnd, nullptr, work.left + ((work.right - work.left) - width) / 2,
                 work.top + ((work.bottom - work.top) - height) / 2, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
  LayOutButtons(hwnd);
}

void StartWork();

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_ERASEBKGND: {
      RECT client = {};
      ::GetClientRect(hwnd, &client);
      const HBRUSH brush = ::CreateSolidBrush(Background());
      ::FillRect((HDC)wparam, &client, brush);
      ::DeleteObject(brush);
      return 1;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps = {};
      const HDC dc = ::BeginPaint(hwnd, &ps);
      RECT client = {};
      ::GetClientRect(hwnd, &client);
      ::SetBkMode(dc, TRANSPARENT);
      PaintOrMeasure(dc, client.right, true);
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_PHASE:
      ApplyPhase(hwnd);
      FitToContent(hwnd);
      if (!::IsWindowVisible(hwnd)) {
        ::ShowWindow(hwnd, SW_SHOW);
        ::SetForegroundWindow(hwnd);
      }
      return 0;
    case WM_FINISHED:
      ::DestroyWindow(hwnd);
      return 0;
    case WM_TIMER: {
      // Something has to move while a few megabytes come down a wire.
      Phase phase = Phase::Ask;
      {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        phase = g_state.phase;
      }
      if (phase == Phase::Checking || phase == Phase::Working) {
        g_dots = (g_dots + 1) % 4;
        ::InvalidateRect(hwnd, nullptr, TRUE);
      }
      return 0;
    }
    case WM_COMMAND: {
      const int id = LOWORD(wparam);
      Phase phase = Phase::Ask;
      std::string page;
      {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        phase = g_state.phase;
        page = g_state.pageUrl;
      }
      if (phase == Phase::Ask && id == IDC_PRIMARY) {
        StartWork();
      } else if (phase == Phase::Failed && id == IDC_PRIMARY) {
        ::ShellExecuteW(nullptr, L"open", Widen(page).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      } else if (id == IDC_SECONDARY || id == IDCANCEL) {
        // IDCANCEL is Escape, which IsDialogMessage turns into this.
        ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
      }
      return 0;
    }
    case WM_CLOSE: {
      Phase phase = Phase::Ask;
      {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        phase = g_state.phase;
      }
      // While files are being moved is the one moment where stopping halfway
      // would leave a folder nobody can explain.
      if (phase == Phase::Working) return 0;
      ::DestroyWindow(hwnd);
      return 0;
    }
    case WM_DESTROY:
      ::PostQuitMessage(0);
      return 0;
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

// ------------------------------------------------------------------- the work

void Fail(const std::string& why) {
  std::string page;
  {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    g_state.heading = T("Das hat nicht geklappt", "That did not work");
    g_state.body = why;
    g_state.bullets.clear();
    g_state.status.clear();
    g_state.primary = T("Release-Seite öffnen", "Open the release page");
    g_state.secondary = T("Schließen", "Close");
    if (g_state.pageUrl.empty()) g_state.pageUrl = Releases().releasePage;
  }
  SetPhase(Phase::Failed);
}

std::string WhyFetchFailed(FetchError error, int status) {
  switch (error) {
    case FetchError::NoNetwork:
      return T("Keine Netzwerkverbindung möglich.", "No network connection available.");
    case FetchError::NoServer:
      return T("Der Server war nicht erreichbar.", "The server could not be reached.");
    case FetchError::NoAnswer:
      return T("Keine Antwort vom Server.", "No answer from the server.");
    case FetchError::HttpStatus:
      return Format(T("Der Server antwortete mit %d.", "The server answered with %d."), status);
    case FetchError::Transfer:
      return T("Die Übertragung ist abgebrochen.", "The transfer broke off.");
    case FetchError::Unreadable:
      return T("Die Antwort war nicht lesbar.", "The answer could not be read.");
    default:
      return T("Die Anfrage ist fehlgeschlagen.", "The request failed.");
  }
}

bool WriteFileWhole(const std::wstring& path, const std::string& data) {
  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const bool ok =
      ::WriteFile(file, data.data(), (DWORD)data.size(), &written, nullptr) != FALSE &&
      written == data.size();
  ::CloseHandle(file);
  if (!ok) ::DeleteFileW(path.c_str());
  return ok;
}

void Install() {
  std::string url, version;
  std::wstring assetName, newStem;
  {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    url = g_state.assetUrl;
    version = g_state.version;
    assetName = g_state.assetName;
    newStem = g_state.newStem;
  }

  SetStatus(Format(T("%s wird geladen", "Downloading %s"), Narrow(newStem).c_str()));

  std::string data;
  FetchError error = FetchError::None;
  int status = 0;
  if (!FetchUrl(url, false, &data, &error, &status)) {
    Fail(WhyFetchFailed(error, status));
    return;
  }
  if (data.size() < 256 * 1024 || data[0] != 'M' || data[1] != 'Z') {
    Fail(T("Was heruntergeladen wurde, ist kein Programm.",
           "What came down is not a program."));
    return;
  }

  const std::wstring folder = ExeDirectory();
  const std::wstring self = ExePath();
  const std::wstring target = folder + assetName;

  SetStatus(T("Wird eingesetzt", "Putting it in place"));

  if (SameText(target, self)) {
    // The release still carries the old name: this is an update, not a rename.
    // The running file cannot be overwritten, but it can be moved aside.
    const std::wstring fresh = self + L".new";
    const std::wstring old = self + L".old";
    ::DeleteFileW(old.c_str());
    if (!WriteFileWhole(fresh, data) ||
        !::MoveFileExW(self.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
      ::DeleteFileW(fresh.c_str());
      Fail(T("Die neue Fassung ließ sich nicht einsetzen.",
             "The new build could not be put in place."));
      return;
    }
    if (!::MoveFileExW(fresh.c_str(), self.c_str(), MOVEFILE_REPLACE_EXISTING)) {
      ::MoveFileExW(old.c_str(), self.c_str(), MOVEFILE_REPLACE_EXISTING);
      Fail(T("Die neue Fassung ließ sich nicht einsetzen.",
             "The new build could not be put in place."));
      return;
    }
  } else {
    if (Exists(target)) SetAside(target);
    if (!WriteFileWhole(target, data)) {
      Fail(T("In den Programmordner ließ sich nicht schreiben.",
             "Could not write into the program folder."));
      return;
    }
  }

  // The camera, if one was installed from this folder. It is registered under
  // the old name and would keep announcing it to every application on the
  // machine; the new build offers to install it again in its settings.
  const std::vector<RegisteredFilter> cameras = RegisteredCameras();
  if (!cameras.empty()) {
    SetStatus(T("Die virtuelle Kamera wird abgemeldet", "Removing the virtual camera"));
    UnregisterCameras(cameras);
  }

  if (!SameText(target, self)) {
    SetStatus(T("Verknüpfungen werden umgehängt", "Pointing the shortcuts at it"));
    RepointShortcuts(self, target);

    // The build that started all this. The 3.7 updater moved it aside before it
    // put this file in its place, and nothing else is ever going to come back
    // for it -- so it goes now, with the new program in place and working, and
    // not one step earlier: until this line it is the only executable in the
    // folder that still runs.
    ::DeleteFileW((self + L".old").c_str());

    // Out of the way under a name the new build already knows how to clean up:
    // it deletes "<its own name>.exe.old" at every start, because that is where
    // its own updater leaves the previous version.
    const std::wstring old = target + L".old";
    ::DeleteFileW(old.c_str());
    ::MoveFileExW(self.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
  }

  ::ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, folder.c_str(), SW_SHOWNORMAL);

  // Not WM_CLOSE: that one refuses to do anything while the work is running,
  // which is the whole point of it. And not PostQuitMessage either -- this is
  // the worker thread, and a quit message posted here would sit in this
  // thread's queue while the window kept waiting in the other one.
  if (g_window) ::PostMessageW(g_window, WM_FINISHED, 0, 0);
}

void StartWork() {
  SetPhase(Phase::Working);
  std::thread(Install).detach();
}

// The check that runs before anything is shown. Everything the window says
// afterwards -- the new name, the version, whether the camera is mentioned at
// all -- comes from what this finds.
void Check() {
  Release release;
  FetchError error = FetchError::None;
  int status = 0;
  if (!FetchLatestRelease(&release, &error, &status)) {
    Fail(WhyFetchFailed(error, status));
    return;
  }
  const ReleaseAsset* program = PickProgram(release);
  if (!program) {
    Fail(T("Die neueste Version enthält kein Programm zum Herunterladen.",
           "The newest release carries no program to download."));
    return;
  }

  std::wstring assetName = Widen(program->name);
  const std::wstring stem = FileStem(assetName);
  if (assetName.size() < 5 || !SameText(assetName.substr(assetName.size() - 4), L".exe")) {
    assetName = stem + L".exe";
  }
  const std::string newName = Narrow(stem);
  const std::string oldName = Narrow(FileStem(ExePath()));
  const bool renamed = !SameText(stem, FileStem(ExePath()));
  const bool camera = !RegisteredCameras().empty();

  {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    g_state.version = release.tag;
    g_state.assetUrl = program->url;
    g_state.assetName = assetName;
    g_state.newStem = stem;
    if (!release.pageUrl.empty()) g_state.pageUrl = release.pageUrl;

    g_state.heading =
        renamed ? Format(T("Aus %s wird %s", "%s is now called %s"), oldName.c_str(),
                         newName.c_str())
                : Format(T("%s %s ist da", "%s %s is out"), newName.c_str(), release.tag.c_str());
    g_state.body =
        renamed
            ? Format(T("Gleiches Programm, neuer Name. Diese Datei holt %s %s und richtet alles "
                       "so ein, dass nichts verloren geht.",
                       "Same program, new name. This file fetches %s %s and sets things up so "
                       "nothing is lost."),
                     newName.c_str(), release.tag.c_str())
            : Format(T("Diese Datei holt %s %s und setzt es ein.",
                       "This file fetches %s %s and puts it in place."),
                     newName.c_str(), release.tag.c_str());

    g_state.bullets.clear();
    g_state.bullets.push_back(
        Format(T("%s%s wird heruntergeladen und in diesen Ordner gelegt.",
                 "%s%s is downloaded into this folder."),
               Narrow(assetName).c_str(), Megabytes(program->size).c_str()));
    if (renamed) {
      g_state.bullets.push_back(
          T("Verknüpfungen auf dem Desktop und im Startmenü zeigen danach auf den neuen Namen.",
            "Shortcuts on the desktop and in the start menu will point at the new name."));
    }
    g_state.bullets.push_back(
        T("Einstellungen und Profile bleiben liegen. Das neue Programm fragt beim ersten Start, "
          "ob es sie übernehmen soll.",
          "Settings and profiles stay where they are. The new program asks at its first start "
          "whether to take them over."));
    if (camera) {
      g_state.bullets.push_back(
          T("Die virtuelle Kamera wird abgemeldet -- dafür fragt Windows nach Administratorrechten. "
            "In den Einstellungen lässt sie sich mit einem Klick wieder anmelden.",
            "The virtual camera is unregistered -- Windows will ask for administrator rights for "
            "that. One click in the settings puts it back."));
    }
    g_state.bullets.push_back(
        Format(T("Diese Datei bleibt nicht liegen: %s räumt sie beim ersten Start weg.",
                 "This file does not stay behind: %s clears it away at its first start."),
               newName.c_str()));

    g_state.status.clear();
    g_state.primary = T("Weiter", "Continue");
    g_state.secondary = T("Abbrechen", "Cancel");
  }
  SetPhase(Phase::Ask);
}

}  // namespace
}  // namespace cap

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
  using namespace cap;

  ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  // Shortcuts are COM objects; nothing else here needs an apartment.
  ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  SpeakTheirLanguage();

  const UINT dpi = ::GetDpiForSystem();
  g_scale = dpi > 0 ? (float)dpi / 96.0f : 1.0f;
  g_dark = SystemUsesDarkMode();

  NONCLIENTMETRICSW metrics = {sizeof(metrics)};
  ::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi);
  g_font = ::CreateFontIndirectW(&metrics.lfMessageFont);
  LOGFONTW heading = metrics.lfMessageFont;
  heading.lfHeight = (LONG)(heading.lfHeight * 1.35);
  heading.lfWeight = FW_SEMIBOLD;
  g_headingFont = ::CreateFontIndirectW(&heading);

  {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    g_state.heading = T("Einen Moment", "One moment");
    g_state.body = T("Es wird nachgesehen, welche Fassung es inzwischen gibt.",
                     "Asking which build there is by now.");
    g_state.status = T("Verbindung wird aufgebaut", "Connecting");
    g_state.secondary = T("Abbrechen", "Cancel");
    g_state.pageUrl = Releases().releasePage;
  }

  const std::wstring className = std::wstring(L"CapViewMigrator");
  WNDCLASSEXW wc = {sizeof(wc)};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = instance;
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = className.c_str();
  wc.hIcon = ::LoadIconW(instance, MAKEINTRESOURCEW(IDI_QBLANK));
  wc.hIconSm = wc.hIcon;
  ::RegisterClassExW(&wc);

  const int width = (int)(620 * g_scale);
  const HWND hwnd = ::CreateWindowExW(0, className.c_str(), FileStem(ExePath()).c_str(),
                                      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT,
                                      CW_USEDEFAULT, width, (int)(300 * g_scale), nullptr, nullptr,
                                      instance, nullptr);
  if (!hwnd) return 1;
  g_window = hwnd;

  const BOOL dark = g_dark ? TRUE : FALSE;
  ::DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));

  for (int id : {IDC_PRIMARY, IDC_SECONDARY}) {
    const DWORD style = WS_CHILD | WS_TABSTOP |
                        (id == IDC_PRIMARY ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON);
    const HWND button = ::CreateWindowExW(0, L"BUTTON", L"", style, 0, 0, 0, 0, hwnd,
                                          (HMENU)(INT_PTR)id, instance, nullptr);
    ::SendMessageW(button, WM_SETFONT, (WPARAM)g_font, TRUE);
    if (g_dark) ::SetWindowTheme(button, L"DarkMode_Explorer", nullptr);
  }

  ApplyPhase(hwnd);
  FitToContent(hwnd);
  ::ShowWindow(hwnd, SW_SHOW);
  ::SetForegroundWindow(hwnd);
  ::SetTimer(hwnd, 1, 400, nullptr);

  std::thread(Check).detach();

  MSG msg;
  while (::GetMessageW(&msg, nullptr, 0, 0)) {
    if (!::IsDialogMessageW(hwnd, &msg)) {
      ::TranslateMessage(&msg);
      ::DispatchMessageW(&msg);
    }
  }

  ::DeleteObject(g_font);
  ::DeleteObject(g_headingFont);
  ::CoUninitialize();
  return 0;
}
