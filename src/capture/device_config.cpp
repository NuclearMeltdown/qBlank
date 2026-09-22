#include "capture/device_config.h"

#include "capture/video_capture.h"
#include "common.h"
#include "i18n.h"

namespace cap {

DevicePropertyPages::~DevicePropertyPages() {
  // The dialog is modal to the user, not to us. There is no supported way to
  // close it from here, so the only correct thing on shutdown is to wait.
  if (thread_.joinable()) thread_.join();
}

bool DevicePropertyPages::Open(const VideoCapture& capture, const std::string& title,
                               std::string* error) {
  if (running_.load(std::memory_order_relaxed)) {
    ReportError(error, CAP_SAID(T("Der Konfigurationsdialog ist bereits offen.",
                       "The configuration dialog is already open.")));
    return false;
  }
  if (thread_.joinable()) thread_.join();  // reap the previous one
  if (!capture.running()) {
    ReportError(error, CAP_SAID(T("Die Karte läuft nicht.", "The card is not running.")));
    return false;
  }
  if (!capture.hasOwnDialog()) {
    ReportError(error, CAP_SAID(T("Diese Karte bringt keinen eigenen Konfigurationsdialog mit.",
                       "This card brings no configuration dialog of its own.")));
    return false;
  }

  show_ = capture.ownDialog(title);

  running_.store(true, std::memory_order_relaxed);
  thread_ = std::thread(&DevicePropertyPages::Run, this);
  return true;
}

void DevicePropertyPages::Run() {
  show_();
  running_.store(false, std::memory_order_release);
}

}  // namespace cap
