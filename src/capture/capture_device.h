#pragma once

// A capture device as a backend opens it. Only the primitives live here: open
// it, ask what it offers, try a format, run it, switch its input. None of the
// choosing does -- which format to ask for first, what to fall back to, what
// the input says about the video standard. That is VideoCapture's, written
// once for every backend.
//
// Checked on paper against DirectShow and V4L2, so that it is not simply the
// first one with the names changed:
//   - a format attempt is SetFormat plus a pin connection on one, S_FMT plus
//     buffer setup on the other, and both can tell a device somebody else holds
//     (EBUSY, a pin whose only instance is taken) from a format it refuses;
//   - inputs are a crossbar or a vendor selector on one, ENUMINPUT/S_INPUT on
//     the other;
//   - the video standard is IAMAnalogVideoDecoder on one, G_STD/S_STD with
//     QUERYSTD for the lock on the other.

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "capture/caps_model.h"
#include "capture/video_format.h"
#include "capture/video_standard.h"
#include "config.h"
#include "i18n.h"

namespace cap {

class FrameBuffer;

struct VideoDeviceInfo {
  std::string name;  // friendly name shown in the UI
  std::string id;    // stable across restarts; what it means is the backend's business
};

// The video capture devices there are right now, in the order the system lists
// them.
std::vector<VideoDeviceInfo> EnumerateVideoDevices();

// Where a device sits in the machine, as the platform describes it: the bus,
// the hardware id, the instance and whatever the platform adds after that, most
// general first. Two devices that agree in the first two parts are functions of
// the same kind of card on the same bus; the third tells copies of it apart.
// The parts are only ever compared, never read.
struct HardwarePath {
  std::vector<std::string> parts;  // empty when the platform does not say
  std::string vendorDevice;        // vendor and device id of the hardware, or empty
};

// Where a video device sits. Worked out from its id, so it answers for a device
// that is only remembered as well.
HardwarePath VideoDeviceHardware(const VideoDeviceInfo& device);

struct CrossbarInput {
  // Whatever identifies this input to the card: the index of the input pin on
  // the crossbar, or, on a card whose inputs are switched through a private
  // property set instead, the vendor's own number for the connector.
  int pinIndex = -1;
  ConnectorKind kind = ConnectorKind::Other;
  long backendType = 0;  // the backend's own code for the connector, diagnostics only
  std::string name;  // "HDMI", "Component (YPbPr)", "Composite", ...
};

// A thread that may talk to capture devices, running `body`. Whatever the
// backend needs set up on a thread before that works is done around it.
std::thread StartCaptureThread(std::function<void()> body);

// The two questions the automatic standard search keeps asking, on a handle of
// their own so a watcher thread can ask them without holding the device. The
// handle keeps the card object alive when the stream is torn down underneath
// it; the worst that happens then is that it asks a card that is no longer
// running, and gets told so.
class SignalProbe {
 public:
  virtual ~SignalProbe() = default;
  // Same meaning as CaptureDevice::CurrentStandard and SignalLocked.
  virtual long CurrentStandard() const = 0;
  virtual int Locked() const = 0;
};

class CaptureDevice {
 public:
  // A probe opens the device briefly to read it and lets go again; a stream
  // keeps it. The backend says its failures a little differently for each, and
  // may tear down differently.
  enum class Purpose { Probe, Stream };

  // What trying one format came to. Busy means somebody else holds the device,
  // and no other format will change that.
  enum class FormatAttempt { Accepted, Busy, Refused };

  // A device object of the backend this build carries, not yet open.
  static std::unique_ptr<CaptureDevice> Create();

  virtual ~CaptureDevice() = default;

  // Finds the device a saved reference names and opens it. `resolved` receives
  // what was actually opened, so the caller can write a fresh id back; it is
  // left alone when nothing was. On failure `failure` says why -- logged
  // already where the backend logs it, otherwise not.
  virtual bool Open(const DeviceRef& ref, Purpose purpose, VideoDeviceInfo* resolved,
                    Said* failure) = 0;

  // Releases the device, in whatever order the backend needs, so it can be
  // opened again right away. Safe to call twice. The destructor only lets go of
  // what is still held.
  virtual void Close() = 0;

  // Puts the card's own brightness, contrast, saturation, hue and the rest back
  // to the neutral values the driver itself declares. Returns how many moved.
  virtual int NeutraliseImageControls() = 0;

  // The analogue video standard, as the bitmask in video_standard.h. Available
  // is zero on a card without an analogue decoder.
  virtual long AvailableStandards() const = 0;
  virtual long CurrentStandard() const = 0;
  virtual bool SetStandard(long standard) = 0;
  // Whether the decoder has locked onto a signal: 1 yes, 0 no, -1 when the card
  // cannot say.
  virtual int SignalLocked() const = 0;

  // What the device says it can deliver.
  virtual std::vector<CapsEntry> Caps() const = 0;
  // Bytes one frame of this format takes, or 0 when that cannot be said
  // beforehand -- compressed formats, and labels the backend does not know.
  virtual size_t FrameBytes(const std::string& subtype, int width, int height) const = 0;
  // The format the device is set to right now, for its colour description.
  virtual void ReadCurrentFormat(VideoFormatInfo* out) const = 0;

  // Some backends only find the input switch once a stream path is wired up. A
  // probe calls this before Inputs(); a backend that knows its inputs from the
  // start has nothing to do here.
  virtual void RevealInputs() = 0;
  virtual std::vector<CrossbarInput> Inputs() const = 0;
  // Which input the card is on right now, as an index into Inputs(), or -1
  // when it cannot be read.
  virtual int CurrentInput() const = 0;
  // Switches to an index into Inputs(); a negative one means leave it alone.
  virtual bool RouteInput(int index) = 0;

  // Where the frames will go. Before the first TryFormat.
  virtual bool AttachSink(Said* failure) = 0;
  // Sets one format and wires the stream up with it. On Refused, the device is
  // left ready for the next attempt and `refusal` says what was refused.
  virtual FormatAttempt TryFormat(const FormatSel& format, Said* refusal) = 0;
  // Starts the frames flowing, with nothing between the driver and the sink
  // that would hold them back.
  virtual bool Run(Said* failure) = 0;
  virtual bool running() const = 0;
  virtual void Stop() = 0;

  // Where the frames arrive. Null until AttachSink.
  virtual FrameBuffer* frames() = 0;
  virtual const FrameBuffer* frames() const = 0;

  // Drains what the device reported since the last call. True when it reported
  // the end of the stream; `failure` then says what happened.
  virtual bool PumpEvents(Said* failure) = 0;

  // Null when the device is not open.
  virtual std::shared_ptr<SignalProbe> OpenSignalProbe() const = 0;

  // The driver's own settings dialog, if it brings one. The job shows it and
  // returns when it is closed; it is meant for a thread of its own, and keeps
  // what it needs of the device alive until then.
  virtual bool HasOwnDialog() const = 0;
  virtual std::function<void()> OwnDialog(const std::string& title) const = 0;
};

}  // namespace cap
