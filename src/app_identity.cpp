#include "app_identity.h"

#include <windows.h>

#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

namespace cap {
namespace {

// Set once AdoptOwnName has renamed the running image. Windows keeps handing
// out the name the module was loaded under, which by then is a file that no
// longer exists -- and the updater would try to move it aside.
std::wstring g_exe_override;
std::wstring g_adopted_from;

bool SameText(const std::wstring& a, const std::wstring& b) {
  return ::CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) ==
         CSTR_EQUAL;
}

bool Exists(const std::wstring& path) {
  return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Takes a file over from an older name. setAside says what to do when the
// current name already has one: set it aside, or leave both alone.
bool TakeOver(const std::wstring& from, const std::wstring& to, bool setAside) {
  if (!Exists(from)) return false;
  if (Exists(to)) {
    if (!setAside) return false;
    SetAside(to);
  }
  return ::MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR raw = nullptr;
  std::wstring out;
  if (SUCCEEDED(::SHGetKnownFolderPath(id, 0, nullptr, &raw)) && raw) out.assign(raw);
  if (raw) ::CoTaskMemFree(raw);
  return out;
}

std::wstring Expand(const std::wstring& text) {
  if (text.find(L'%') == std::wstring::npos) return text;
  wchar_t buffer[MAX_PATH * 2] = {};
  const DWORD n = ::ExpandEnvironmentStringsW(text.c_str(), buffer, (DWORD)std::size(buffer));
  if (n == 0 || n > std::size(buffer)) return text;
  return std::wstring(buffer);
}

// True when this one shortcut was pointing at `from` and now points at `to`.
bool RepointOne(const std::wstring& lnk, const std::wstring& from, const std::wstring& to) {
  IShellLinkW* link = nullptr;
  if (FAILED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                (void**)&link))) {
    return false;
  }
  IPersistFile* file = nullptr;
  bool changed = false;
  if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&file))) {
    if (SUCCEEDED(file->Load(lnk.c_str(), STGM_READWRITE))) {
      wchar_t target[MAX_PATH * 2] = {};
      // Raw, and without resolving: resolving sends Windows hunting for a file
      // that has just been renamed, which is slow and can rewrite the shortcut
      // to something else entirely before we get a look at it.
      if (SUCCEEDED(link->GetPath(target, (int)std::size(target), nullptr, SLGP_RAWPATH)) &&
          SameText(Expand(target), from)) {
        link->SetPath(to.c_str());
        const size_t slash = to.find_last_of(L"\\/");
        if (slash != std::wstring::npos) link->SetWorkingDirectory(to.substr(0, slash).c_str());

        // The icon usually lives in the executable itself, so it has to follow.
        wchar_t icon[MAX_PATH * 2] = {};
        int index = 0;
        if (SUCCEEDED(link->GetIconLocation(icon, (int)std::size(icon), &index)) &&
            SameText(Expand(icon), from)) {
          link->SetIconLocation(to.c_str(), index);
        }
        changed = SUCCEEDED(file->Save(nullptr, TRUE));
      }
    }
    file->Release();
  }
  link->Release();
  return changed;
}

int RepointIn(const std::wstring& folder, const std::wstring& from, const std::wstring& to,
              int depth) {
  if (folder.empty() || depth > 4) return 0;
  WIN32_FIND_DATAW found = {};
  const HANDLE search = ::FindFirstFileW((folder + L"\\*").c_str(), &found);
  if (search == INVALID_HANDLE_VALUE) return 0;
  int count = 0;
  do {
    const std::wstring name = found.cFileName;
    if (name == L"." || name == L"..") continue;
    const std::wstring path = folder + L"\\" + name;
    if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      count += RepointIn(path, from, to, depth + 1);
    } else if (name.size() > 4 && SameText(name.substr(name.size() - 4), L".lnk")) {
      if (RepointOne(path, from, to)) ++count;
    }
  } while (::FindNextFileW(search, &found));
  ::FindClose(search);
  return count;
}

}  // namespace

const std::string& AppNameUtf8() {
  static const std::string name = [] {
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, kAppName, -1, nullptr, 0, nullptr, nullptr);
    std::string out((size_t)(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) ::WideCharToMultiByte(CP_UTF8, 0, kAppName, -1, out.data(), n, nullptr, nullptr);
    return out;
  }();
  return name;
}

std::wstring WindowClassName(const wchar_t* suffix) {
  return std::wstring(kAppName) + suffix;
}

std::wstring ExePath() {
  if (!g_exe_override.empty()) return g_exe_override;
  wchar_t path[MAX_PATH * 2] = {};
  const DWORD n = ::GetModuleFileNameW(nullptr, path, (DWORD)std::size(path));
  const std::wstring started(path, n);

  // winget starts a portable app through a symlink in its Links folder, and
  // Windows reports the name the program was started under. Settings, log,
  // ffmpeg and the updater all belong next to the real file, not the link.
  const DWORD attr = ::GetFileAttributesW(started.c_str());
  if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_REPARSE_POINT)) return started;
  const HANDLE file =
      ::CreateFileW(started.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return started;
  wchar_t real[MAX_PATH * 2] = {};
  const DWORD m = ::GetFinalPathNameByHandleW(file, real, (DWORD)std::size(real),
                                              FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  ::CloseHandle(file);
  if (m == 0 || m >= std::size(real)) return started;
  const std::wstring resolved(real, m);
  if (resolved.rfind(L"\\\\?\\UNC\\", 0) == 0) return L"\\\\" + resolved.substr(8);
  if (resolved.rfind(L"\\\\?\\", 0) == 0) return resolved.substr(4);
  return resolved;
}

std::wstring ExeDirectory() {
  const std::wstring path = ExePath();
  const size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return L".\\";
  return path.substr(0, slash + 1);
}

std::wstring FileStem(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
  const size_t dot = name.find_last_of(L'.');
  return dot == std::wstring::npos ? name : name.substr(0, dot);
}

std::wstring AppFile(const wchar_t* extension) {
  return ExeDirectory() + kAppName + L"." + extension;
}

std::wstring FormerAppFile(size_t index, const wchar_t* extension) {
  if (index >= kFormerAppNameCount) return std::wstring();
  return ExeDirectory() + kFormerAppNames[index] + L"." + extension;
}

const std::wstring& AdoptedFrom() { return g_adopted_from; }

bool SetAside(const std::wstring& path) {
  if (!Exists(path)) return true;
  return ::MoveFileExW(path.c_str(), (path + L".bak").c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

void SetAdoptedFrom(const std::wstring& name) { g_adopted_from = name; }

void AdoptFormerLog() {
  // Newest predecessor first, and only into a gap: a log is a record of what
  // happened, not something to set another file aside for.
  for (size_t i = kFormerAppNameCount; i-- > 0;) {
    if (TakeOver(FormerAppFile(i, L"log"), AppFile(L"log"), false)) return;
  }
}

bool AdoptOwnName() {
  const std::wstring exe = ExePath();
  const std::wstring stem = FileStem(exe);
  if (SameText(stem, kAppName)) return false;

  // Only a name this program used to answer to gets corrected. Somebody who
  // renamed the executable on purpose keeps their name.
  bool ours = false;
  for (size_t i = 0; i < kFormerAppNameCount; ++i) {
    if (SameText(stem, kFormerAppNames[i])) {
      ours = true;
      break;
    }
  }
  if (!ours) return false;

  const std::wstring fresh = ExeDirectory() + kAppName + L".exe";
  if (Exists(fresh)) return false;  // two copies; leave both alone
  if (!::MoveFileExW(exe.c_str(), fresh.c_str(), 0)) return false;

  g_exe_override = fresh;
  RepointShortcuts(exe, fresh);
  return true;
}

int RepointShortcuts(const std::wstring& from, const std::wstring& to) {
  const std::wstring appdata = KnownFolder(FOLDERID_RoamingAppData);
  const std::wstring places[] = {
      KnownFolder(FOLDERID_Desktop),
      KnownFolder(FOLDERID_PublicDesktop),
      KnownFolder(FOLDERID_Programs),
      KnownFolder(FOLDERID_CommonPrograms),
      appdata.empty() ? std::wstring()
                      : appdata + L"\\Microsoft\\Internet Explorer\\Quick Launch",
  };
  int count = 0;
  for (const std::wstring& place : places) count += RepointIn(place, from, to, 0);
  return count;
}

}  // namespace cap
