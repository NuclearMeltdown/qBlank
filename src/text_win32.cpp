#include "text_win32.h"

#include <windows.h>

#include "common.h"

namespace cap {

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

// Not path::u8string() and u8path(): those throw on what does not convert --
// an unpaired surrogate in a file name, a settings file saved in another
// encoding -- where the program has always carried on with a replacement
// character.
std::string PathToUtf8(const std::filesystem::path& path) { return ToUtf8(path.native()); }

std::filesystem::path Utf8ToPath(const std::string& text) {
  return std::filesystem::path(ToWide(text));
}

}  // namespace cap
