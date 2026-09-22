#include "update/updater.h"

#include "app_files.h"
#include "child_process.h"
#include "common.h"
#include "files.h"
#include "i18n.h"
#include "update/release_source.h"

namespace cap {
namespace {

// Where a release lives and which file in it is the program are questions with
// answers that outlive any name -- see src/update/release_source.h. Nothing in
// this file spells out a repository, a file name or a version scheme.

// The three names the update dance works with, all derived from the file that is
// running: itself, the one that arrived, and the one it used to be.
std::filesystem::path Beside(const char* suffix) {
  std::filesystem::path path = OwnProgramFile();
  path += suffix;
  return path;
}

UpdateError Translate(FetchError error) {
  switch (error) {
    case FetchError::NoNetwork:
      return UpdateError::NoNetwork;
    case FetchError::NoServer:
      return UpdateError::NoServer;
    case FetchError::NoRequest:
      return UpdateError::NoRequest;
    case FetchError::NoAnswer:
      return UpdateError::NoAnswer;
    case FetchError::HttpStatus:
      return UpdateError::HttpStatus;
    case FetchError::Transfer:
      return UpdateError::Transfer;
    case FetchError::Unreadable:
      return UpdateError::Unreadable;
    default:
      return UpdateError::None;
  }
}

}  // namespace

std::string UpdateErrorText(const UpdateStatus& status) {
  switch (status.error) {
    case UpdateError::NoNetwork:
      return T("Keine Netzwerkverbindung möglich.", "No network connection available.");
    case UpdateError::NoServer:
      return T("Server nicht erreichbar.", "Could not reach the server.");
    case UpdateError::NoRequest:
      return T("Anfrage konnte nicht gestellt werden.", "The request could not be made.");
    case UpdateError::NoAnswer:
      return T("Keine Antwort vom Server.", "No answer from the server.");
    case UpdateError::HttpStatus:
      return std::string(T("Der Server antwortete mit ", "The server answered with ")) +
             std::to_string(status.httpStatus) + ".";
    case UpdateError::Transfer:
      return T("Übertragung abgebrochen.", "The transfer broke off.");
    case UpdateError::Unreadable:
      return T("Die Antwort war nicht lesbar.", "The answer could not be read.");
    case UpdateError::NoAsset:
      return T("Die neue Version enthält kein Programm zum Herunterladen.",
               "That release carries no program to download.");
    case UpdateError::NoUrl:
      return T("Keine Download-Adresse.", "No download address.");
    case UpdateError::NotAProgram:
      return T("Die heruntergeladene Datei ist kein Programm.",
               "What came back is not a program.");
    case UpdateError::WriteFailed:
      return T("Konnte nicht in den Programmordner schreiben.",
               "Could not write to the program folder.");
    case UpdateError::MoveAsideFailed:
      return T("Die alte Version ließ sich nicht beiseite legen.",
               "The old build could not be moved aside.");
    case UpdateError::InsertFailed:
      return T("Die neue Version ließ sich nicht einsetzen.",
               "The new build could not be put in place.");
    default:
      return "";
  }
}

std::string ReleasePageUrl(const UpdateStatus& status) {
  // The address the API handed back is right even after the project has been
  // renamed; the built-in one is for when there was no answer to hand one back.
  return status.pageUrl.empty() ? Releases().releasePage : status.pageUrl;
}

std::string WebsiteUrl() { return Releases().website; }

const char* Updater::currentVersion() { return kAppVersion; }

void Updater::CleanUpPreviousBuild() {
  const std::filesystem::path old = Beside(".old");
  if (PathExists(old) && RemoveFile(old)) {
    CAP_LOG("Previous program version removed");
  }
}

Updater::~Updater() {
  if (thread_.joinable()) thread_.join();
}

UpdateStatus Updater::status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return status_;
}

void Updater::SetStatus(const UpdateStatus& s) {
  std::lock_guard<std::mutex> lock(mutex_);
  status_ = s;
}

void Updater::CheckAsync(bool announce) {
  if (busy_.exchange(true, std::memory_order_acq_rel)) return;
  announceNext_ = announce;
  if (thread_.joinable()) thread_.join();
  thread_ = std::thread(&Updater::Run, this, false);
}

void Updater::InstallAsync() {
  if (busy_.exchange(true, std::memory_order_acq_rel)) return;
  if (thread_.joinable()) thread_.join();
  thread_ = std::thread(&Updater::Run, this, true);
}

void Updater::Run(bool install) {
  UpdateStatus s = status();

  if (!install) {
    s.state = UpdateStatus::State::Checking;
    s.error = UpdateError::None;
    s.announce = announceNext_;
    SetStatus(s);

    Release release;
    FetchError error = FetchError::None;
    if (!FetchLatestRelease(&release, &error, &s.httpStatus)) {
      s.state = UpdateStatus::State::Failed;
      s.error = Translate(error);
      SetStatus(s);
      busy_.store(false, std::memory_order_release);
      return;
    }

    s.latestVersion = release.tag;
    s.pageUrl = release.pageUrl;
    s.notes = release.notes;
    if (s.notes.size() > 1200) s.notes = s.notes.substr(0, 1200) + " ...";

    // The asset carrying the program itself. A release without one is a release
    // this cannot install, and saying so beats pretending otherwise.
    downloadUrl_.clear();
    if (const ReleaseAsset* program = PickProgram(release)) {
      downloadUrl_ = program->url;
      CAP_LOG("Update asset: %s (%s)", program->name.c_str(),
              program->label.empty() ? "no label" : program->label.c_str());
    }

    const bool newer = IsNewerRelease(release, currentVersion());
    s.state = newer ? UpdateStatus::State::Available : UpdateStatus::State::UpToDate;
    if (newer && downloadUrl_.empty()) {
      s.state = UpdateStatus::State::Failed;
      s.error = UpdateError::NoAsset;
    }
    CAP_LOG("Update check: installed %s, latest %s -> %s", currentVersion(),
            s.latestVersion.c_str(), newer ? "newer available" : "up to date");
    SetStatus(s);
    busy_.store(false, std::memory_order_release);
    return;
  }

  // ---- install ----
  s.state = UpdateStatus::State::Downloading;
  s.percent = 0;
  s.error = UpdateError::None;
  SetStatus(s);

  if (downloadUrl_.empty()) {
    s.state = UpdateStatus::State::Failed;
    s.error = UpdateError::NoUrl;
    SetStatus(s);
    busy_.store(false, std::memory_order_release);
    return;
  }

  std::string data;
  FetchError error = FetchError::None;
  if (!FetchUrl(downloadUrl_, false, &data, &error, &s.httpStatus)) {
    s.state = UpdateStatus::State::Failed;
    s.error = Translate(error);
    SetStatus(s);
    busy_.store(false, std::memory_order_release);
    return;
  }

  // Before anything is moved: is this actually a program? Every Windows
  // executable starts with these two bytes, and a redirect page or an error
  // document does not. Overwriting the program with an HTML page would be a
  // remarkably annoying way to find that out later.
  if (data.size() < 256 * 1024 || data[0] != 'M' || data[1] != 'Z') {
    s.state = UpdateStatus::State::Failed;
    s.error = UpdateError::NotAProgram;
    SetStatus(s);
    busy_.store(false, std::memory_order_release);
    return;
  }

  const std::filesystem::path exe = OwnProgramFile();
  const std::filesystem::path fresh = Beside(".new");
  const std::filesystem::path old = Beside(".old");

  // Not ReplaceWholeFile: this is a new file next to the program, not a file
  // being replaced, and what is at stake is caught by the two renames below.
  if (!WriteWholeFile(fresh, data.data(), data.size())) {
    RemoveFile(fresh);
    s.state = UpdateStatus::State::Failed;
    s.error = UpdateError::WriteFailed;
    SetStatus(s);
    busy_.store(false, std::memory_order_release);
    return;
  }

  // The running image cannot be overwritten, but it can be renamed out of the
  // way -- and if the second step fails, the first is put back, so a failed
  // update leaves the program exactly as it was rather than gone.
  //
  // The new build takes the old file's name, whatever that name is. If the
  // program has been renamed since, the build that just arrived notices at its
  // next start and corrects its own name -- see AdoptOwnName in app_identity.h.
  RemoveFile(old);
  if (!RenameOver(exe, old)) {
    RemoveFile(fresh);
    s.state = UpdateStatus::State::Failed;
    s.error = UpdateError::MoveAsideFailed;
    SetStatus(s);
    busy_.store(false, std::memory_order_release);
    return;
  }
  if (!RenameOver(fresh, exe)) {
    RenameOver(old, exe);
    RemoveFile(fresh);
    s.state = UpdateStatus::State::Failed;
    s.error = UpdateError::InsertFailed;
    SetStatus(s);
    busy_.store(false, std::memory_order_release);
    return;
  }

  CAP_LOG("Update to %s installed, restart pending", s.latestVersion.c_str());
  s.state = UpdateStatus::State::Ready;
  s.percent = 100;
  SetStatus(s);
  busy_.store(false, std::memory_order_release);
}

bool Updater::RestartIntoNewBuild() const {
  if (status().state != UpdateStatus::State::Ready) return false;
  const std::filesystem::path exe = OwnProgramFile();
  return StartAndLetGo(exe, exe.parent_path());
}

}  // namespace cap
