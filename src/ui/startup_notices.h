#pragma once

// The three notices that can come up by themselves around a start: an update
// is available, the previous session did not end normally, and the welcome on
// the very first start.

#include <string>

#include "common.h"
#include "update/updater.h"

namespace cap {

class StartupNotices {
 public:
  // What the notices need from the application around them.
  class Host {
   public:
    virtual ~Host() = default;
    virtual float UiScale() const = 0;
    virtual void Toast(const std::string& text) = 0;
    // Ends the main loop, after the update has started the new build.
    virtual void Quit() = 0;
  };

  StartupNotices(Updater& updater, Host& host);

  void QueueCrashNotice(const UnfinishedSession& session);
  // Only the shortcuts that are missing are offered.
  void QueueWelcome(bool startMenu, bool desktop);

  // The one-off notice when the check made at startup finds something. Shown in
  // the picture, because a tab nobody opened is not a notice.
  void DrawUpdatePrompt();
  // Once, when the log's previous session never got its end line.
  void DrawCrashNotice();
  // On the very first start: shortcuts in the start menu and on the desktop?
  void DrawWelcome();

 private:
  // Opens the release in the browser, at the address the server gave for it --
  // which stays right even after the project has been renamed. Falls back to
  // the built-in one when there is no answer to take an address from.
  void OpenReleasePage(const UpdateStatus& status);

  Updater& updater_;
  Host& host_;

  bool updatePromptQueued_ = false;   // waiting to be opened
  bool updatePromptRaised_ = false;   // already shown once this session
  UnfinishedSession unfinishedSession_;  // what the crash notice reports
  bool crashNoticeQueued_ = false;
  // The first start's question: which shortcuts are missing, and which of
  // those are ticked.
  bool welcomeQueued_ = false;
  bool welcomeStartMenu_ = false;
  bool welcomeDesktop_ = false;
  bool welcomeWantStartMenu_ = true;
  bool welcomeWantDesktop_ = true;
};

}  // namespace cap
