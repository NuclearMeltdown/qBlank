#include "desktop.h"

#include "common_win32.h"

#include <shellapi.h>  // ShellExecuteW
#include <shlobj.h>    // SHOpenFolderAndSelectItems

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

}  // namespace cap
