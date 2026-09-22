// Where Windows starts this program, and the handful of things that have to be
// settled while the process is still nothing but a process. The program itself
// is program.cpp.

#include <windows.h>
// CommandLineToArgvW. shellapi.h is a wall of errors without windows.h having
// been seen first.
#include <shellapi.h>

#include <string>
#include <vector>

#include "app_identity.h"
#include "common_win32.h"
#include "platform.h"
#include "program.h"
#include "text_win32.h"
#include "update/updater.h"
#include "window_win32.h"

namespace {

// The command line as words. Not the string wWinMain is handed: that one leaves
// out the program's own name, and CommandLineToArgvW reads its first word by the
// rules for a program name -- quoting works differently there. So the whole line
// is asked for and the first word dropped.
std::vector<std::string> Arguments() {
  std::vector<std::string> words;
  int count = 0;
  LPWSTR* parts = ::CommandLineToArgvW(::GetCommandLineW(), &count);
  if (!parts) return words;
  for (int i = 1; i < count; ++i) words.push_back(cap::ToUtf8(parts[i]));
  ::LocalFree(parts);
  return words;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int showCmd) {
  cap::SetStartupShowCommand(showCmd);

  cap::MakeProcessDpiAware();

  // Multi threaded apartment: DirectShow filters and WASAPI both push from
  // their own threads, and MTA keeps those calls free of apartment marshalling.
  cap::ComScope com(COINIT_MULTITHREADED);
  if (!com.ok()) {
    ::MessageBoxW(nullptr, L"COM could not be initialised.", cap::kAppName,
                  MB_ICONERROR | MB_OK);
    return 1;
  }

  // Before anything reads a file: sort out what this program is called. A build
  // that arrived through the updater while the program was being renamed is
  // still sitting under the old file name, and the settings next to it still
  // carry the old name too. Both are corrected here, once, and everything after
  // this point can just ask for "the settings file" and get the right one.
  //
  // Renaming a running executable is allowed on Windows -- only overwriting is
  // not -- which is the same thing the updater relies on to replace itself.
  cap::Updater::CleanUpPreviousBuild();
  cap::AdoptOwnName();
  cap::AdoptFormerLog();

  return cap::RunProgram(Arguments());
}
