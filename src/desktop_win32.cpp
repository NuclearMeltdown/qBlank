#include "desktop.h"

#include "common_win32.h"

#include <shellapi.h>  // ShellExecuteW
#include <shlobj.h>    // SHOpenFolderAndSelectItems, IShellLinkW, known folders

#include <thread>

#include "app_identity.h"
#include "text_win32.h"

namespace cap {
namespace {

// ShellExecute says more than "it went wrong" -- everything above 32 is a
// success, and the small numbers below it are the old error codes. None of the
// callers here can do anything with the difference.
bool Shell(const wchar_t* verb, const std::wstring& what, const wchar_t* args) {
  const HINSTANCE result =
      ::ShellExecuteW(nullptr, verb, what.c_str(), args, nullptr, SW_SHOWNORMAL);
  return (INT_PTR)result > 32;
}

// The user's own start menu and desktop, not the ones shared by everybody on
// the machine: those need administrator rights, and a portable program has no
// business asking for them.
std::wstring ShortcutFile(ShortcutPlace place) {
  const KNOWNFOLDERID& id =
      place == ShortcutPlace::StartMenu ? FOLDERID_Programs : FOLDERID_Desktop;
  PWSTR raw = nullptr;
  std::wstring folder;
  if (SUCCEEDED(::SHGetKnownFolderPath(id, 0, nullptr, &raw)) && raw) folder.assign(raw);
  if (raw) ::CoTaskMemFree(raw);
  if (folder.empty()) return folder;
  return folder + L"\\" + kAppName + L".lnk";
}

// Whether the shortcut at `lnk` exists and leads to the running executable.
bool StartsThisCopy(const std::wstring& lnk) {
  if (::GetFileAttributesW(lnk.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
  ComPtr<IShellLinkW> link;
  ComPtr<IPersistFile> file;
  if (FAILED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&link))) ||
      FAILED(link.As(&file)) || FAILED(file->Load(lnk.c_str(), STGM_READ))) {
    return false;
  }
  // Raw and unresolved, as in RepointShortcuts: resolving goes looking for a
  // target that has moved, and may rewrite the shortcut while doing it.
  wchar_t raw[MAX_PATH * 2] = {};
  if (FAILED(link->GetPath(raw, (int)std::size(raw), nullptr, SLGP_RAWPATH))) return false;
  wchar_t target[MAX_PATH * 2] = {};
  const DWORD n = ::ExpandEnvironmentStringsW(raw, target, (DWORD)std::size(target));
  if (n == 0 || n > std::size(target)) return false;
  const std::wstring exe = ExePath();
  return ::CompareStringOrdinal(target, -1, exe.c_str(), (int)exe.size(), TRUE) == CSTR_EQUAL;
}

}  // namespace

bool OpenUrl(const std::string& url) { return Shell(L"open", ToWide(url), nullptr); }

bool OpenFolder(const std::filesystem::path& folder) {
  return Shell(L"open", folder.native(), nullptr);
}

bool OpenFile(const std::filesystem::path& file) { return Shell(L"open", file.native(), nullptr); }

// Explorer, mit der Datei markiert. SHOpenFolderAndSelectItems nimmt ein
// Fenster, das den Ordner schon zeigt, statt ein neues danebenzustellen, will
// aber ein STA -- und der rufende Faden ist wegen DirectShow MTA. Also ein
// kurzer eigener. Geht es dort schief, tut es explorer /select auch, nur eben
// mit einem Fenster mehr.
//
// Deswegen ist das Ja hier eines auf die Frage, ob es losgeschickt wurde: was
// der eigene Faden herausfindet, kommt zu spaet, um es noch zu melden.
bool ShowFileInFolder(const std::filesystem::path& file) {
  // Nur wer vorn ist, darf das weitergeben -- und das sind wir gerade, es
  // wurde eben auf uns geklickt. Sonst ginge der Explorer hinter dem Bild auf.
  ::AllowSetForegroundWindow(ASFW_ANY);
  std::wstring native = file.native();
  std::thread([native] {
    ComScope com(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    HRESULT hr = E_FAIL;
    if (PIDLIST_ABSOLUTE item = ::ILCreateFromPathW(native.c_str())) {
      hr = ::SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
      ::ILFree(item);
    }
    if (FAILED(hr)) {
      const std::wstring args = L"/select,\"" + native + L"\"";
      ::ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
  }).detach();
  return true;
}

void ShowFatalMessage(const std::string& text) {
  ::MessageBoxW(nullptr, ToWide(text).c_str(), kAppName, MB_ICONERROR | MB_OK);
}

// The main thread is already in the multithreaded apartment; the scope only
// matters to a thread that is in none, and costs nothing otherwise.
bool HasShortcut(ShortcutPlace place) {
  const std::wstring lnk = ShortcutFile(place);
  if (lnk.empty()) return false;
  ComScope com(COINIT_MULTITHREADED);
  return StartsThisCopy(lnk);
}

bool CreateShortcut(ShortcutPlace place) {
  const std::wstring lnk = ShortcutFile(place);
  if (lnk.empty()) return false;
  ComScope com(COINIT_MULTITHREADED);
  ComPtr<IShellLinkW> link;
  ComPtr<IPersistFile> file;
  if (FAILED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&link))) ||
      FAILED(link.As(&file))) {
    return false;
  }
  const std::wstring exe = ExePath();
  std::wstring folder = ExeDirectory();
  if (folder.size() > 3 && (folder.back() == L'\\' || folder.back() == L'/')) folder.pop_back();
  link->SetPath(exe.c_str());
  link->SetWorkingDirectory(folder.c_str());
  link->SetIconLocation(exe.c_str(), 0);

  const bool replacing = ::GetFileAttributesW(lnk.c_str()) != INVALID_FILE_ATTRIBUTES;
  if (FAILED(CAP_HR(file->Save(lnk.c_str(), TRUE)))) return false;
  // Explorer notices a new file by itself, but only when it next gets round to
  // it; told directly, the icon is on the desktop at once.
  ::SHChangeNotify(replacing ? SHCNE_UPDATEITEM : SHCNE_CREATE, SHCNF_PATHW, lnk.c_str(),
                   nullptr);
  return true;
}

bool RemoveShortcut(ShortcutPlace place) {
  const std::wstring lnk = ShortcutFile(place);
  if (lnk.empty()) return false;
  ComScope com(COINIT_MULTITHREADED);
  if (!StartsThisCopy(lnk) || !::DeleteFileW(lnk.c_str())) return false;
  ::SHChangeNotify(SHCNE_DELETE, SHCNF_PATHW, lnk.c_str(), nullptr);
  return true;
}

}  // namespace cap
