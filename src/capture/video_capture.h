#pragma once

// Owns the DirectShow graph: capture filter -> FrameSink. Nothing else is in
// the graph, and the graph runs without a reference clock so samples are
// delivered the moment the driver produces them instead of being scheduled.

#include <dshow.h>

#include <string>
#include <vector>

#include "capture/dshow_util.h"
#include "capture/frame_sink.h"
#include "common_win32.h"
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

  // Builds and runs the graph. On failure `error` holds a message meant for the
  // user and the object stays stopped.
  bool Start(const CaptureSettings& settings, std::string* error);
  void Stop();

  bool running() const { return control_ != nullptr; }

  FrameSink* sink() { return sink_.Get(); }
  const FrameSink* sink() const { return sink_.Get(); }

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

  // Drains the graph event queue. Returns true when the graph reported a fatal
  // condition (device lost, abort); `message` then describes it.
  bool PumpEvents(std::string* message);

  // Switches the crossbar input on a running graph, no restart needed.
  bool SetCrossbarInput(int index);

  // The live filter and its capture pin, for the driver's own property pages.
  // Null while stopped.
  IBaseFilter* filter() const { return captureFilter_.Get(); }
  IPin* capturePin() const { return capturePin_.Get(); }

  // 1 locked, 0 not, -1 when the card cannot say. Polled by the automatic
  // standard selection; cheap enough to ask a few times a second.
  int signalLocked() const { return VideoStandardLocked(captureFilter_.Get()); }
  long currentStandard() const { return CurrentVideoStandard(captureFilter_.Get()); }
  bool SetStandard(long standard) { return SetVideoStandard(captureFilter_.Get(), standard); }

 private:
  void Teardown();

  ComPtr<IGraphBuilder> graph_;
  ComPtr<ICaptureGraphBuilder2> builder_;
  ComPtr<IMediaControl> control_;
  ComPtr<IMediaEventEx> events_;
  ComPtr<IBaseFilter> captureFilter_;
  ComPtr<IPin> capturePin_;
  ComPtr<FrameSink> sink_;

  DeviceProbeResult capabilities_;
  FormatSel connectedFormat_;
};

}  // namespace cap
