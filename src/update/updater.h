#pragma once

// Checks the GitHub releases page for a newer build, and can fetch and install
// it. Everything that touches the network runs on a thread of its own; the UI
// only ever reads a snapshot.
//
// Installing replaces the running executable. Windows will not let a running
// image be overwritten, but it will let it be *renamed* -- so the old build is
// moved aside, the new one takes its name, and the next start deletes the
// leftover. Nothing has to be running for that to work, and there is no helper
// process to go missing.

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace cap {

// What went wrong, rather than a sentence saying so. The work happens on a
// thread of its own and the language can be changed at any time, so the wording
// is picked where it is shown rather than where it is raised.
enum class UpdateError {
  None,
  NoNetwork,
  NoServer,
  NoRequest,
  NoAnswer,
  HttpStatus,     // see UpdateStatus::httpStatus
  Transfer,
  Unreadable,
  NoAsset,
  NoUrl,
  NotAProgram,
  WriteFailed,
  MoveAsideFailed,
  InsertFailed,
};

struct UpdateStatus {
  enum class State {
    Idle,         // nothing asked for yet
    Checking,     // querying the releases API
    UpToDate,     // asked, and this is the newest
    Available,    // a newer release exists
    Downloading,  // fetching it
    Ready,        // swapped in; a restart picks it up
    Failed,
  };

  State state = State::Idle;
  std::string latestVersion;  // as the tag names it, e.g. "v1.1"
  std::string notes;          // the release text, trimmed to something readable
  std::string pageUrl;        // where that release is on the web, as it said itself
  UpdateError error = UpdateError::None;
  int httpStatus = 0;  // only with UpdateError::HttpStatus
  int percent = 0;     // download progress
  // Whether finding something is worth interrupting anybody for. True only for
  // the check made at startup: somebody who pressed "check now" is already
  // looking at the answer, and a dialog on top of it is just in the way.
  bool announce = false;
};

// What to put on the screen for a failed status, in the language selected right
// now -- which is why it is worked out here and not where the failure happened.
std::string UpdateErrorText(const UpdateStatus& status);

// Where to send somebody who has to fetch a build by hand. The release page
// comes from the answer the server gave, so it stays right across a rename; the
// built-in address is only used when there was no answer at all.
std::string ReleasePageUrl(const UpdateStatus& status);
std::string WebsiteUrl();

class Updater {
 public:
  Updater() = default;
  ~Updater();

  Updater(const Updater&) = delete;
  Updater& operator=(const Updater&) = delete;

  // The version this build reports. Compared against the release tag.
  static const char* currentVersion();

  // Removes the build a previous update moved aside. Call once at startup; it
  // costs nothing when there is nothing to remove.
  static void CleanUpPreviousBuild();

  // Both return immediately; watch status().
  // `announce` marks the result as worth a popup. The check at startup sets it;
  // the button in the settings does not.
  void CheckAsync(bool announce = false);
  void InstallAsync();

  UpdateStatus status() const;
  bool busy() const { return busy_.load(std::memory_order_relaxed); }

 private:
  void SetStatus(const UpdateStatus& s);
  void Run(bool install);

  mutable std::mutex mutex_;
  UpdateStatus status_;
  // Set by CheckAsync, read by the thread it starts.
  bool announceNext_ = false;
  std::string downloadUrl_;  // filled by the check, used by the install
  std::thread thread_;
  std::atomic<bool> busy_{false};
};

}  // namespace cap
