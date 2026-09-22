#pragma once

// Opens the configured capture device and keeps it streaming. Which format to
// ask for, what to fall back to and what the input says about the video
// standard is decided here, once; talking to the device is the backend's, in
// capture_device.h.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "capture/caps_model.h"
#include "capture/capture_device.h"
#include "capture/frame_buffer.h"
#include "capture/video_format.h"
#include "config.h"

namespace cap {

// What a device can do, gathered without committing to it. Used by the settings
// window to fill the format dropdowns and the input list.
struct DeviceProbeResult {
  bool ok = false;
  std::string error;
  VideoDeviceInfo device;
  CapsModel caps;
  std::vector<CrossbarInput> crossbarInputs;
  // Which of them the card was found on, or -1 when that cannot be read. This
  // is the card's own state, not qBlank's setting: it holds whatever the last
  // program to touch it left behind, which is what makes it worth reading.
  int currentInput = -1;
  // What the analogue decoder offers, and what it is set to. Both zero when the
  // card has no analogue decoder at all.
  long availableStandards = 0;
  long currentStandard = 0;
  // Colour description read off the pin's current media type, so the diagnostics
  // can show whether the driver describes its output or leaves it to guesswork.
  VideoFormatInfo colorInfo;
};

class VideoCapture {
 public:
  VideoCapture() = default;
  ~VideoCapture();

  VideoCapture(const VideoCapture&) = delete;
  VideoCapture& operator=(const VideoCapture&) = delete;

  // Opens the device and starts it. On failure `error` holds a message meant for
  // the user and the object stays stopped.
  bool Start(const CaptureSettings& settings, std::string* error);
  void Stop();

  bool running() const { return device_ && device_->running(); }

  // Where the frames arrive. Null while stopped.
  FrameBuffer* sink() { return device_ ? device_->frames() : nullptr; }
  const FrameBuffer* sink() const { return device_ ? device_->frames() : nullptr; }

  VideoFormatInfo format() const;

  // Filled during Start, so the settings window can show the live device's
  // capabilities without probing it a second time (the card is busy then).
  const DeviceProbeResult& capabilities() const { return capabilities_; }

  // Actual device identity that got opened, for writing a refreshed id back to
  // the config after a match by name.
  const VideoDeviceInfo& resolvedDevice() const { return capabilities_.device; }

  // The format the pins actually connected with -- empty while stopped, and
  // empty after a failed Start. Same purpose as resolvedDevice above: when the
  // profile asked for no particular resolution, the card picked one, and the
  // caller has no other way to learn which.
  const FormatSel& connectedFormat() const { return connectedFormat_; }

  // Opens a device briefly to read its capabilities. Do not call this for the
  // device that is currently running -- use capabilities() instead.
  static DeviceProbeResult Probe(const DeviceRef& device);

  // Drains the device's event queue. Returns true when it reported a fatal
  // condition (device lost, abort); `message` then describes it.
  bool PumpEvents(std::string* message);

  // Switches the crossbar input on a running device, no restart needed.
  bool SetCrossbarInput(int index);

  // 1 locked, 0 not, -1 when the card cannot say. Polled by the automatic
  // standard selection; cheap enough to ask a few times a second.
  int signalLocked() const { return device_ ? device_->SignalLocked() : -1; }
  long currentStandard() const { return device_ ? device_->CurrentStandard() : 0; }
  bool SetStandard(long standard) { return device_ ? device_->SetStandard(standard) : false; }

  // The same two questions on a handle of their own, for a watcher thread. Null
  // while stopped.
  std::shared_ptr<SignalProbe> signalProbe() const {
    return device_ ? device_->OpenSignalProbe() : nullptr;
  }

  // The driver's own settings dialog, see CaptureDevice::OwnDialog. False and
  // empty while stopped.
  bool hasOwnDialog() const { return device_ && device_->HasOwnDialog(); }
  std::function<void()> ownDialog(const std::string& title) const {
    return device_ ? device_->OwnDialog(title) : std::function<void()>();
  }

 private:
  void Teardown();

  std::unique_ptr<CaptureDevice> device_;

  DeviceProbeResult capabilities_;
  FormatSel connectedFormat_;
};

}  // namespace cap
