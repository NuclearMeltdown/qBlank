#include "platform.h"

#include "common_win32.h"
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

}  // namespace cap
