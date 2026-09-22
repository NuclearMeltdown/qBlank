#pragma once

// Thin helpers over DirectShow: device enumeration, media type juggling,
// capability enumeration and crossbar routing.

#include <dshow.h>

#include <string>
#include <vector>

#include "capture/caps_model.h"
#include "capture/capture_device.h"
#include "capture/video_format.h"
#include "capture/video_standard.h"
#include "common_win32.h"
#include "config.h"

namespace cap {

// ------------------------------------------------------------- media subtypes

// Short label for a subtype GUID ("YUY2", "RGB24", "MJPG", ... ). Unknown GUIDs
// come back as their FOURCC if printable, otherwise as a hex string.
std::string SubtypeLabel(const GUID& subtype);

// Reverse lookup for the labels above. Returns false for unknown labels.
bool SubtypeFromLabel(const std::string& label, GUID* out);

// True for the uncompressed formats the renderer can upload directly.
// Compressed capture formats (MJPG) are still selectable -- the graph builder
// inserts a decoder whose output is one of these.
bool IsRendererSubtype(const GUID& subtype);

// Bytes per row and total image size for an uncompressed subtype.
// Returns 0 for formats we cannot size (compressed).
size_t ImageSizeForSubtype(const GUID& subtype, int width, int height, int* strideOut);

int BitsPerPixelForSubtype(const GUID& subtype);

// ---------------------------------------------------------------- media types

void FreeMediaType(AM_MEDIA_TYPE& mt);
void DeleteMediaType(AM_MEDIA_TYPE* mt);
AM_MEDIA_TYPE* CreateMediaTypeCopy(const AM_MEDIA_TYPE* src);

// Reads the parts of a video media type the renderer cares about, translating
// the subtype into a PixelLayout and the DXVA colour codes into a ColorInfo.
bool ParseVideoMediaType(const AM_MEDIA_TYPE* mt, VideoFormatInfo* out);

// ----------------------------------------------------------------- device enum

// A device as DirectShow lists it. The id is the DevicePath.
struct DShowDeviceInfo : VideoDeviceInfo {
  std::string monikerName;  // display name, used to re-bind quickly
};

// CLSID_VideoInputDeviceCategory, the list every capture program shows.
// EnumerateVideoDevices is this list without the monikers.
std::vector<DShowDeviceInfo> EnumerateVideoCaptureDShowDevices();

// DirectShow's own audio input category. Some capture cards expose their
// embedded audio here even when Windows hides the matching sound endpoint, so
// this is worth looking at when the audio pairing comes up empty.
std::vector<DShowDeviceInfo> EnumerateAudioCaptureDShowDevices();

// Strips a DirectShow DevicePath down to the hardware it names, so paths from
// different device interfaces on the same card compare equal.
//   \\?\pci#ven_1131&dev_7160&subsys_x&rev_y#6&846d6d9&0&000800e2#{iface}\{...}
//   ->  PCI\VEN_1131&DEV_7160&SUBSYS_X&REV_Y\6&846D6D9&0&000800E2
std::string NormalizeDevicePath(const std::string& devicePath);

// A device instance path in upper case -- what NormalizeDevicePath returns, or
// what a sound device reports as its instance id -- as a HardwarePath: split on
// the backslashes, with VEN_xxxx&DEV_xxxx taken from the hardware id.
HardwarePath HardwareFromInstancePath(const std::string& instancePath);

// Resolves a saved reference to a live filter. Matching order: exact id, then
// moniker display name, then friendly name. `resolved` receives what was
// actually opened so the caller can write the fresh id back to the config.
ComPtr<IBaseFilter> CreateVideoFilter(const DeviceRef& ref, VideoDeviceInfo* resolved);

// Instantiates any enumerated device from its moniker display name. Works for
// every category, unlike CreateVideoFilter which searches the video category.
ComPtr<IBaseFilter> CreateFilterFromMoniker(const DShowDeviceInfo& info);

ComPtr<IPin> FindPinByDirection(IBaseFilter* filter, PIN_DIRECTION dir, int skip = 0);

// The pin that carries the live stream. Uses the capture category when a graph
// builder is available, otherwise falls back to the first output pin that
// exposes IAMStreamConfig.
ComPtr<IPin> FindCapturePin(ICaptureGraphBuilder2* builder, IBaseFilter* filter);

// ------------------------------------------------------------------ capability

std::vector<CapsEntry> EnumerateCaps(IPin* capturePin);

// Applies a format to the capture pin. Returns the media type that was actually
// negotiated in `applied` (optional). Failure means the driver rejected it.
HRESULT ApplyFormat(IPin* capturePin, const FormatSel& fmt, VideoFormatInfo* applied);

// -------------------------------------------------------------------- crossbar

// A PhysConn_* value in the terms the rest of the program asks about.
ConnectorKind ConnectorKindOf(long physicalType);

// Enumerates the video inputs of the crossbar upstream of `captureFilter`, and
// where there is no crossbar, the inputs of a vendor selector the card is known
// to answer for. Empty when the card offers neither, which is the honest answer
// for a pure HDMI card and for any card whose selector is not known here.
std::vector<CrossbarInput> EnumerateCrossbarInputs(ICaptureGraphBuilder2* builder,
                                                   IBaseFilter* captureFilter);

// Routes the given input (index into the list above) to the crossbar output.
// Also routes the matching audio input when the crossbar has one. On a card
// switched through a vendor selector, sets that selector instead.
bool RouteCrossbarInput(ICaptureGraphBuilder2* builder, IBaseFilter* captureFilter, int index);

// Which input the card is on right now, as an index into the list above, or -1
// when it cannot be read. Not the same question as which input qBlank chose:
// the card keeps its own setting, and until something writes to it that setting
// is whatever the vendor's property page or the last program left behind.
int CurrentCrossbarInput(ICaptureGraphBuilder2* builder, IBaseFilter* captureFilter);

// Every crossbar the system registers, as its own device rather than as
// something hanging off a capture filter. A WDM crossbar is a filter in its own
// right, in its own category, and a driver is free to register one without the
// graph builder ever finding it from the capture filter. Diagnostic only --
// nothing in qBlank routes through this yet.
std::vector<DShowDeviceInfo> EnumerateCrossbarDevices();

// Everything registered in one device category, by its GUID. EnumerateVideoDevices
// asks for CLSID_VideoInputDeviceCategory, which is what every capture program
// shows; a driver may register KS filters that the category does not surface.
// Diagnostic only -- the device list still comes from EnumerateVideoDevices.
std::vector<DShowDeviceInfo> EnumerateDeviceCategory(const GUID& category);

std::string PhysicalConnectorName(long physicalType);

// ---------------------------------------------------------------------------
// Analogue video standard, as the card's decoder sees it. The table and
// everything that only reads it are in video_standard.h.

// What the card says it can do, as a bitmask. Zero when it has no analogue
// decoder -- a pure HDMI input does not.
long AvailableVideoStandards(IBaseFilter* filter);
long CurrentVideoStandard(IBaseFilter* filter);
bool SetVideoStandard(IBaseFilter* filter, long standard);

// Put the card's own brightness, contrast, saturation, hue and the rest back to
// the neutral values the driver itself declares, and leave them there.
//
// Not a nicety. Whatever those are set to is applied before the frame reaches
// us, so a decoder sitting on a lifted black level or a contrast boost means
// there is no clean picture anywhere in the program to go back to -- and the
// damage is the kind that cannot be undone afterwards, because it has already
// clipped. qBlank does these four in the shader instead, where the original is
// still there underneath and a recording can be made without them.
//
// Silent when the card has no such controls, which a pure HDMI input usually
// does not. Returns how many properties were actually moved.
int NeutraliseProcAmp(IBaseFilter* filter);

// Whether the decoder has locked onto a signal: 1 yes, 0 no, -1 when the card
// cannot say. This one is genuinely measured -- the decoder loses the lock when
// it is set to the wrong line count -- which is what makes automatic selection
// possible at all. Its sibling get_NumberOfLines is not: that merely repeats
// the standard it was given.
int VideoStandardLocked(IBaseFilter* filter);

}  // namespace cap
