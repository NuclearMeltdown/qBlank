#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "app.h"
#include "common.h"
#include "config.h"
#include "i18n.h"
#include "record/ffmpeg_download.h"
#include "record/ffmpeg_locator.h"
#include "ui/startup_dialog.h"
#include "update/updater.h"

namespace {

// The question about the settings is asked before there is a window, so it is
// asked before there is an instance handle to hand around. Every caller of the
// callback below is inside this process anyway.
HINSTANCE g_instance = nullptr;

// The language sits in the settings, and the settings are exactly what is being
// asked about -- so the question is put in the language of the file that has the
// history. The file this build wrote is the second choice, English the third,
// because that is what a fresh install starts in.
void SpeakLanguageOf(const cap::ForeignSettings& found) {
  int language = found.language;
  if (language < 0) language = cap::SettingsLanguage(cap::Config::FilePath());
  if (language < 0) return;
  cap::SetLanguage(language == 1 ? cap::Language::English : cap::Language::German);
}

// "08.09.2026 21:14" in whatever order and separators Windows is set to.
std::string WhenWritten(unsigned long long fileTime) {
  if (fileTime == 0) return std::string();
  ULARGE_INTEGER raw;
  raw.QuadPart = fileTime;
  FILETIME utc = {raw.LowPart, raw.HighPart};
  FILETIME local = {};
  SYSTEMTIME when = {};
  if (!::FileTimeToLocalFileTime(&utc, &local) || !::FileTimeToSystemTime(&local, &when)) {
    return std::string();
  }
  wchar_t date[64] = {};
  wchar_t time[64] = {};
  ::GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &when, nullptr, date, 64, nullptr);
  ::GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &when, nullptr, time, 64);
  return cap::ToUtf8(std::wstring(date) + L" " + time);
}

unsigned long long LastWritten(const std::wstring& path) {
  WIN32_FILE_ATTRIBUTE_DATA info = {};
  if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return 0;
  ULARGE_INTEGER when = {};
  when.LowPart = info.ftLastWriteTime.dwLowDateTime;
  when.HighPart = info.ftLastWriteTime.dwHighDateTime;
  return when.QuadPart;
}

// Settings under a name this build does not use: leftovers of the rename, or a
// folder someone copied. Which of the two files holds the profiles is not
// something the program can work out, so it asks once and then never again --
// whichever file loses ends up as ".bak" and is not offered a second time.
cap::SettingsAnswer AskAboutSettings(const cap::ForeignSettings& found, bool haveOwn) {
  SpeakLanguageOf(found);
  const std::string file = cap::ToUtf8(found.stem);
  const std::string label = found.program.empty() ? file : found.program;
  const std::string mine = cap::AppNameUtf8();

  cap::StartupQuestion q;
  q.heading = haveOwn ? std::string(cap::T("Zwei Einstellungsdateien", "Two settings files"))
                      : cap::Format(cap::T("Einstellungen von %s gefunden",
                                           "Found settings from %s"),
                                    label.c_str());
  q.body = haveOwn
               ? cap::Format(cap::T("Neben dem Programm liegen die Einstellungen von %s und die "
                                    "dieser Version. Beide enthalten Profile, und nur du weisst, "
                                    "in welcher die Arbeit steckt.",
                                    "Next to the program are the settings from %s and the ones "
                                    "this build wrote. Both hold profiles, and only you know "
                                    "which of them has the work in it."),
                             label.c_str())
               : cap::Format(cap::T("Neben dem Programm liegen Einstellungen von %s. Sie lassen "
                                    "sich mitsamt allen Profilen weiterverwenden.",
                                    "Next to the program are settings from %s. They can be "
                                    "carried over, profiles and all."),
                             label.c_str());

  cap::StartupQuestion::Row other;
  other.label = file + ".json";
  other.detail = label + "  ·  " + WhenWritten(found.modified);
  other.highlight = true;
  q.rows.push_back(other);
  if (haveOwn) {
    cap::StartupQuestion::Row own;
    own.label = mine + ".json";
    own.detail = cap::Format(cap::T("%s, diese Version", "%s, this build"), mine.c_str()) + "  ·  " +
                 WhenWritten(LastWritten(cap::Config::FilePath()));
    q.rows.push_back(own);
  }

  q.acceptLabel = cap::Format(cap::T("%s.json übernehmen", "Take over %s.json"), file.c_str());
  q.rejectLabel = haveOwn ? cap::Format(cap::T("Bei %s.json bleiben", "Stay with %s.json"),
                                        mine.c_str())
                          : std::string(cap::T("Neu anfangen", "Start fresh"));
  q.footnote = cap::T("Die andere Datei wird als .bak beiseitegelegt, gelöscht wird nichts. "
                      "Dieses Fenster zu schließen entscheidet nichts – die Frage kommt beim "
                      "nächsten Start wieder.",
                      "The other file is set aside as .bak, nothing is deleted. Closing this "
                      "window decides nothing – the question comes back at the next start.");

  switch (cap::AskAtStartup(g_instance, q)) {
    case cap::StartupAnswer::Accept:
      return cap::SettingsAnswer::TakeOver;
    case cap::StartupAnswer::Reject:
      return cap::SettingsAnswer::Discard;
    default:
      return cap::SettingsAnswer::Postpone;
  }
}

// Attaches to the parent console if there is one, so a command line run can
// report progress. A windowed program has no console of its own.
void AttachConsoleIfAny() {
  if (!::AttachConsole(ATTACH_PARENT_PROCESS)) return;
  FILE* dummy = nullptr;
  freopen_s(&dummy, "CONOUT$", "w", stdout);
  freopen_s(&dummy, "CONOUT$", "w", stderr);
}

// Runs the same download the settings button does, without the window. Useful
// for provisioning a machine from a script -- and the only way to exercise this
// code path without a human clicking.
int FetchFfmpeg() {
  AttachConsoleIfAny();
  // A windowed program handed a console it did not create cannot rely on stdout
  // surviving, so the log file is the dependable record of what happened.
  cap::LogInit(true);
  cap::LogWrite("INFO", "--fetch-ffmpeg started");
  std::printf("\nqBlank: fetching ffmpeg ...\n");

  cap::FfmpegDownloader downloader;
  if (!downloader.Start(cap::ExeDirectory() + L"ffmpeg")) {
    std::printf("Could not start the download.\n");
    return 1;
  }

  std::string last;
  while (downloader.busy()) {
    const std::string message = downloader.message();
    if (message != last) {
      last = message;
      std::printf("  %s\n", message.c_str());
    }
    ::Sleep(200);
  }
  std::printf("  %s\n", downloader.message().c_str());

  const bool ok = downloader.state() == cap::FfmpegDownloader::State::Done;
  if (ok && !downloader.resultPath().empty()) {
    std::printf("  %s\n", downloader.resultPath().c_str());
  }
  // A failure has been logged where it happened.
  if (ok) cap::LogWrite("INFO", "--fetch-ffmpeg: %s", downloader.resultPath().c_str());
  return ok ? 0 : 1;
}

// Prints what the encoder probe finds, so a recording problem can be diagnosed
// without opening the settings.
int ListEncoders() {
  AttachConsoleIfAny();
  cap::FfmpegInfo info = cap::LocateFfmpeg({});
  if (!info.found) {
    std::printf("\nffmpeg not found.\n");
    return 1;
  }
  std::printf("\n%s\n%s\n\n", info.path.c_str(), info.version.c_str());
  cap::ProbeEncoders(&info);
  for (const cap::EncoderInfo& e : info.encoders) {
    std::printf("  %-30s %-9s %s\n", e.label.c_str(), e.hardware ? "Hardware" : "CPU",
                e.available ? "available" : ("not available  " + e.error).c_str());
  }
  return 0;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR commandLine, int showCmd) {
  g_instance = instance;

  // Per monitor DPI so the picture is not stretched by the compositor on a
  // scaled display.
  ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

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
  cap::AdoptSettings(&AskAboutSettings);

  const std::wstring args = commandLine ? commandLine : L"";
  if (args.find(L"--fetch-ffmpeg") != std::wstring::npos) return FetchFfmpeg();
  if (args.find(L"--list-encoders") != std::wstring::npos) return ListEncoders();

  // 1 ms timer resolution: the wait in the render loop and the second field of
  // a bob deinterlaced frame both need better than the default 15.6 ms.
  ::timeBeginPeriod(1);

  int result = 1;
  {
    cap::App app;
    if (app.Initialize(instance, showCmd)) {
      result = app.Run();
    }
    app.Shutdown();
  }

  ::timeEndPeriod(1);
  cap::LogWrite("INFO", "%s exited", cap::AppNameUtf8().c_str());
  return result;
}
