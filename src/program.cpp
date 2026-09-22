#include "program.h"

#include <cstdio>

#include "app.h"
#include "app_files.h"
#include "common.h"
#include "config.h"
#include "files.h"
#include "i18n.h"
#include "platform.h"
#include "record/ffmpeg_download.h"
#include "record/ffmpeg_locator.h"
#include "ui/startup_dialog.h"

namespace {

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

// Settings under a name this build does not use: leftovers of the rename, or a
// folder someone copied. Which of the two files holds the profiles is not
// something the program can work out, so it asks once and then never again --
// whichever file loses ends up as ".bak" and is not offered a second time.
cap::SettingsAnswer AskAboutSettings(const cap::ForeignSettings& found, bool haveOwn) {
  SpeakLanguageOf(found);
  const std::string file = found.stem;
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
  other.detail =
      label + "  ·  " + cap::ShortDateAndTime(cap::LocalTimeFrom(found.modified));
  other.highlight = true;
  q.rows.push_back(other);
  if (haveOwn) {
    cap::StartupQuestion::Row own;
    own.label = mine + ".json";
    own.detail = cap::Format(cap::T("%s, diese Version", "%s, this build"), mine.c_str()) + "  ·  " +
                 cap::ShortDateAndTime(
                     cap::LocalTimeFrom(cap::FileWriteTime(cap::Config::FilePath())));
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

  switch (cap::AskAtStartup(q)) {
    case cap::StartupAnswer::Accept:
      return cap::SettingsAnswer::TakeOver;
    case cap::StartupAnswer::Reject:
      return cap::SettingsAnswer::Discard;
    default:
      return cap::SettingsAnswer::Postpone;
  }
}

// Runs the same download the settings button does, without the window. Useful
// for provisioning a machine from a script -- and the only way to exercise this
// code path without a human clicking.
int FetchFfmpeg() {
  cap::UseParentConsole();
  // A windowed program handed a console it did not create cannot rely on stdout
  // surviving, so the log file is the dependable record of what happened.
  // Same file as the program's, so the same rules decide what stays in it.
  cap::Config config;
  config.Load(nullptr);
  cap::LogInit(true, config.app.logRetention);
  cap::LogWrite("INFO", "--fetch-ffmpeg started");
  std::printf("\nqBlank: fetching ffmpeg ...\n");

  cap::FfmpegDownloader downloader;
  if (!downloader.Start(cap::ExeFolder() / "ffmpeg")) {
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
    cap::SleepMilliseconds(200);
  }
  std::printf("  %s\n", downloader.message().c_str());

  const bool ok = downloader.state() == cap::FfmpegDownloader::State::Done;
  if (ok && !downloader.resultPath().empty()) {
    std::printf("  %s\n", downloader.resultPath().c_str());
  }
  // A failure has been logged where it happened.
  if (ok) cap::LogWrite("INFO", "--fetch-ffmpeg: %s", downloader.resultPath().c_str());
  cap::LogEnd();
  return ok ? 0 : 1;
}

// Prints what the encoder probe finds, so a recording problem can be diagnosed
// without opening the settings.
int ListEncoders() {
  cap::UseParentConsole();
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

// A word that contains the flag counts, which is how the whole command line was
// searched before it arrived here as words.
bool Asked(const std::vector<std::string>& arguments, const char* flag) {
  for (const std::string& word : arguments) {
    if (word.find(flag) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

namespace cap {

int RunProgram(const std::vector<std::string>& arguments) {
  // Before anything reads the settings: settle which file they are. The name the
  // program wears has already been corrected by this point, so the question here
  // is only about the contents -- see FindForeignSettings in config.h.
  AdoptSettings(&AskAboutSettings);

  if (Asked(arguments, "--fetch-ffmpeg")) return FetchFfmpeg();
  if (Asked(arguments, "--list-encoders")) return ListEncoders();

  BeginPreciseTiming();

  int result = 1;
  {
    App app;
    if (app.Initialize()) {
      result = app.Run();
    }
    app.Shutdown();
  }

  EndPreciseTiming();
  LogEnd();
  return result;
}

}  // namespace cap
