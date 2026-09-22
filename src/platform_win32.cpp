#include "platform.h"

#include "common_win32.h"
// timeBeginPeriod. Came along with the audio headers once; they no longer bring
// in anything of Windows.
#include <timeapi.h>

#include <cstdio>

#include "resource.h"
#include "text_win32.h"

namespace cap {

bool SystemPrefersDark() {
  HKEY key = nullptr;
  if (::RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                      KEY_READ, &key) != ERROR_SUCCESS) {
    return true;  // assume dark; this program is meant for a darkened room
  }
  DWORD value = 1;
  DWORD size = sizeof(value);
  DWORD type = 0;
  const bool ok = ::RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type, (LPBYTE)&value,
                                     &size) == ERROR_SUCCESS &&
                  type == REG_DWORD;
  ::RegCloseKey(key);
  return ok ? value == 0 : true;
}

std::vector<std::string> UiFontFiles() {
  std::vector<std::string> found;

  wchar_t windir[MAX_PATH] = {};
  if (::GetWindowsDirectoryW(windir, MAX_PATH) == 0) return found;

  // Segoe UI Variable on Windows 11, plain Segoe UI everywhere else.
  const wchar_t* candidates[] = {L"\\Fonts\\SegUIVar.ttf", L"\\Fonts\\segoeui.ttf"};
  for (const wchar_t* rel : candidates) {
    const std::wstring path = std::wstring(windir) + rel;
    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
    found.push_back(ToUtf8(path));
  }
  return found;
}

// Not through WIC, although WIC is already linked: the resource compiler splits
// an .ico into RT_GROUP_ICON plus one RT_ICON per size, so there is no .ico file
// in the binary for WIC to decode. LoadImage understands that split and picks
// the size asked for, which is the whole reason to go the GDI way here.
std::vector<uint8_t> AppIconRgba(int size, int* width, int* height) {
  if (width) *width = 0;
  if (height) *height = 0;

  HICON icon = (HICON)::LoadImageW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_QBLANK),
                                   IMAGE_ICON, size, size, LR_DEFAULTCOLOR);
  if (!icon) return {};

  ICONINFO info = {};
  BITMAP bm = {};
  std::vector<uint8_t> pixels;
  int w = 0, h = 0;
  if (::GetIconInfo(icon, &info) && info.hbmColor &&
      ::GetObjectW(info.hbmColor, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 0) {
    w = bm.bmWidth;
    h = bm.bmHeight;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // negative: top down, so no row flip afterwards
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    pixels.resize((size_t)w * (size_t)h * 4);
    HDC screen = ::GetDC(nullptr);
    if (!::GetDIBits(screen, info.hbmColor, 0, (UINT)h, pixels.data(), &bi, DIB_RGB_COLORS)) {
      pixels.clear();
    }
    ::ReleaseDC(nullptr, screen);
  }
  if (info.hbmColor) ::DeleteObject(info.hbmColor);
  if (info.hbmMask) ::DeleteObject(info.hbmMask);
  ::DestroyIcon(icon);
  if (pixels.empty()) return {};

  // GetDIBits hands back BGRA, so the channels swap on the way out.
  //
  // An icon with no alpha at all is an old-style one whose transparency lives in
  // the mask instead. Rather than decode the mask, such an icon is handed over
  // opaque: the rectangle is square and the background behind it is flat, so the
  // result is plain rather than wrong.
  bool anyAlpha = false;
  for (size_t i = 3; i < pixels.size(); i += 4) {
    if (pixels[i] != 0) { anyAlpha = true; break; }
  }
  for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
    const uint8_t b = pixels[i];
    pixels[i + 0] = pixels[i + 2];
    pixels[i + 2] = b;
    if (!anyAlpha) pixels[i + 3] = 255;
  }

  if (width) *width = w;
  if (height) *height = h;
  return pixels;
}

// Den Wohnort gibt GetUserDefaultGeoName -- erst ab Windows 10 1709, deshalb
// spaet gebunden. Faellt es aus, bleibt die Locale, die schlechtere Auskunft
// (sie beschreibt Zahlen- und Datumsformate), aber besser als gar keine.
std::string UserCountryCode() {
  wchar_t country[16] = {};

  typedef int(WINAPI * GeoNameFn)(PWSTR, int);
  static const GeoNameFn geoName = [] {
    HMODULE kernel = ::GetModuleHandleW(L"kernel32.dll");
    return kernel ? (GeoNameFn)::GetProcAddress(kernel, "GetUserDefaultGeoName") : nullptr;
  }();
  if (geoName && geoName(country, 16) > 0) return ToUpper(ToUtf8(country));

  // Nicht LOCALE_NAME_USER_DEFAULT (also NULL) durchreichen: das wird je nach
  // Wirt anders aufgeloest und liefert dann die invariante Locale ("IV"), die in
  // keiner Tabelle steht und still im PAL-Zweig landet. Am 29.08.2026 auf diesem
  // Rechner nachgemessen: mit NULL kam "IV", mit dem ausgeschriebenen Namen
  // "DE". Also erst den Namen holen, dann damit fragen.
  wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
  if (::GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) > 0 &&
      ::GetLocaleInfoEx(locale, LOCALE_SISO3166CTRYNAME, country, 16) > 0) {
    return ToUpper(ToUtf8(country));
  }
  return std::string();
}

std::string ShortDateAndTime(const LocalTime& when) {
  if (when.year == 0) return std::string();
  SYSTEMTIME s = {};
  s.wYear = (WORD)when.year;
  s.wMonth = (WORD)when.month;
  s.wDay = (WORD)when.day;
  s.wHour = (WORD)when.hour;
  s.wMinute = (WORD)when.minute;
  s.wSecond = (WORD)when.second;
  wchar_t date[64] = {};
  wchar_t time[64] = {};
  ::GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &s, nullptr, date, 64, nullptr);
  ::GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &s, nullptr, time, 64);
  return ToUtf8(std::wstring(date) + L" " + time);
}

// Multi threaded apartment, the same model the process is initialised in:
// DirectShow filters and WASAPI both push from their own threads, and MTA keeps
// those calls free of apartment marshalling.
SystemThreadScope::SystemThreadScope() {
  // RPC_E_CHANGED_MODE means the thread is already initialised in a different
  // apartment; we must not call CoUninitialize in that case.
  held_ = SUCCEEDED(::CoInitializeEx(nullptr, COINIT_MULTITHREADED));
}

SystemThreadScope::~SystemThreadScope() {
  if (held_) ::CoUninitialize();
}

void KeepDisplayAwake(bool keep) {
  ::SetThreadExecutionState(keep ? (ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED)
                                 : ES_CONTINUOUS);
}

void BeginPreciseTiming() { ::timeBeginPeriod(1); }

void EndPreciseTiming() { ::timeEndPeriod(1); }

void MakeProcessDpiAware() {
  // Per monitor, so the picture is not stretched by the compositor on a scaled
  // display.
  ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

void UseParentConsole() {
  if (!::AttachConsole(ATTACH_PARENT_PROCESS)) return;
  FILE* dummy = nullptr;
  freopen_s(&dummy, "CONOUT$", "w", stdout);
  freopen_s(&dummy, "CONOUT$", "w", stderr);
}

}  // namespace cap
