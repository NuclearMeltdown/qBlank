#pragma once

// Thin helpers over DirectShow: device enumeration, media type juggling,
// capability enumeration and crossbar routing.

#include <dshow.h>

#include <string>
#include <vector>

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

// Reads the parts of a video media type the renderer cares about.
struct VideoFormatInfo {
  GUID subtype = GUID_NULL;
  std::string subtypeLabel;
  int width = 0;
  int height = 0;       // always positive
  int stride = 0;       // bytes per row of the top plane
  bool bottomUp = false;  // RGB DIBs are stored upside down
  bool interlaced = false;
  bool fieldOneFirst = true;
  double fps = 0.0;
  int aspectX = 0;  // from VIDEOINFOHEADER2, 0 when unknown
  int aspectY = 0;
  size_t imageSize = 0;

  // Colour description the driver put in DXVA_ExtendedFormat. Most cards leave
  // this empty, but when it is there it beats guessing the range and matrix from
  // the picture height -- and it is where an HDR source announces PQ or HLG.
  bool colorInfoPresent = false;
  int nominalRange = 0;      // 1 = full 0-255, 2 = limited 16-235
  int transferMatrix = 0;    // 1 = BT.709, 2 = BT.601, 4/5 = BT.2020
  int primaries = 0;         // 2 = BT.709, 9 = BT.2020
  int transferFunction = 0;  // 5 = BT.709, 15 = PQ (ST.2084), 16 = HLG

  bool isHdrTransfer() const { return transferFunction == 15 || transferFunction == 16; }

  bool valid() const { return width > 0 && height > 0 && imageSize > 0; }
};

// Human readable names for the DXVA colour fields above, for the diagnostics.
const char* NominalRangeName(int value);
const char* TransferMatrixName(int value);
const char* PrimariesName(int value);
const char* TransferFunctionName(int value);

bool ParseVideoMediaType(const AM_MEDIA_TYPE* mt, VideoFormatInfo* out);

// ----------------------------------------------------------------- device enum

struct VideoDeviceInfo {
  std::string name;         // friendly name shown in the UI
  std::string id;           // DevicePath -- stable across restarts
  std::string monikerName;  // display name, used to re-bind quickly
};

std::vector<VideoDeviceInfo> EnumerateVideoDevices();

// DirectShow's own audio input category. Some capture cards expose their
// embedded audio here even when Windows hides the matching sound endpoint, so
// this is worth looking at when the audio pairing comes up empty.
std::vector<VideoDeviceInfo> EnumerateAudioCaptureDShowDevices();

// Resolves a saved reference to a live filter. Matching order: exact id, then
// moniker display name, then friendly name. `resolved` receives what was
// actually opened so the caller can write the fresh id back to the config.
ComPtr<IBaseFilter> CreateVideoFilter(const DeviceRef& ref, VideoDeviceInfo* resolved);

// Instantiates any enumerated device from its moniker display name. Works for
// every category, unlike CreateVideoFilter which searches the video category.
ComPtr<IBaseFilter> CreateFilterFromMoniker(const VideoDeviceInfo& info);

ComPtr<IPin> FindPinByDirection(IBaseFilter* filter, PIN_DIRECTION dir, int skip = 0);

// The pin that carries the live stream. Uses the capture category when a graph
// builder is available, otherwise falls back to the first output pin that
// exposes IAMStreamConfig.
ComPtr<IPin> FindCapturePin(ICaptureGraphBuilder2* builder, IBaseFilter* filter);

// ------------------------------------------------------------------ capability

// One capability as reported by IAMStreamConfig::GetStreamCaps. Drivers vary:
// some return a row per (format, resolution, fps), others a row per
// (format, resolution) with a frame rate range.
struct CapsEntry {
  GUID subtype = GUID_NULL;
  std::string subtypeLabel;
  int width = 0;
  int height = 0;
  double defaultFps = 0.0;
  double minFps = 0.0;
  double maxFps = 0.0;
  // Output size range for this entry, used to offer resolutions the driver
  // does not list explicitly.
  int minWidth = 0, maxWidth = 0, granularityX = 0;
  int minHeight = 0, maxHeight = 0, granularityY = 0;
};

std::vector<CapsEntry> EnumerateCaps(IPin* capturePin);

// A resolution or frame rate offered in the settings UI. `forced` marks values
// outside what the driver advertises -- they often work anyway (the card that
// claims 1080p30 but does 1080p60), but may fail.
struct ResolutionOption {
  int width = 0;
  int height = 0;
  bool forced = false;
};

// `highest` marks the entry that does not name a rate at all: it stands for
// whatever the card turns out to top out at, and is resolved when the card is
// opened rather than when it is picked. A number written down today is wrong
// the moment the console changes mode; this one is not.
// `native` is the second one of those: the rate the incoming video standard
// prescribes -- 50 Hz under PAL and SECAM, 59,94 under NTSC, 60 under PAL 60 --
// rather than the fastest the card will go. The difference only exists on a
// card that is not pinned to one mode: such a card offers 720x576 at 59,94 as
// readily as at 50, and taking the top means asking for a rate the signal does
// not have. Only offered where the standard is known, which is the same
// condition as CapsModel::SetNativeStandard.
struct FpsOption {
  double fps = 0.0;  // 0 together with `highest` or `native`: resolved on open
  bool forced = false;
  bool highest = false;
  bool native = false;
};

// Derives the three dropdown lists (format / resolution / frame rate) from a
// capability list, mixing advertised and forced values.
class CapsModel {
 public:
  void Build(std::vector<CapsEntry> entries);

  // Welche Videonorm vorne anliegt, als DirectShow-Bitmaske, oder 0, wenn das
  // niemand sagen kann. Keine Eigenschaft der Karte: die zaehlt dieselbe Liste
  // auf, was auch immer angeschlossen ist. Es ist der Zusammenhang, der
  // entscheidet, welcher Eintrag dieser Liste der richtige ist, und ohne ihn
  // gewinnt die groesste Flaeche zur hoechsten Rate. Siehe PickDefault.
  //
  // Die Norm und nicht die Zeilenzahl, weil an ihr zwei Dinge haengen, die sich
  // nicht auseinander ableiten lassen: das Raster (525/625) und die Halbbild-
  // rate. PAL 60 und NTSC haben dasselbe Raster und verschiedene Raten.
  void SetNativeStandard(long standard);
  int nativeLines() const { return nativeLines_; }
  double nativeFieldRate() const { return nativeFieldRate_; }

  bool empty() const { return entries_.empty(); }
  const std::vector<CapsEntry>& entries() const { return entries_; }

  std::vector<std::string> Subtypes() const;
  std::vector<ResolutionOption> Resolutions(const std::string& subtype) const;
  std::vector<FpsOption> FpsList(const std::string& subtype, int width, int height) const;

  // The highest rate the driver claims for this combination, or 0 when it
  // claims none. This is what a stored fps of 0 turns into on open.
  double HighestFps(const std::string& subtype, int width, int height) const;

  // Die Rate, die die Norm des ankommenden Signals vorgibt, auf das gerundet,
  // was die Karte fuer diese Kombination wirklich anbietet -- oder 0, wenn die
  // Norm unbekannt ist oder nichts in ihre Naehe kommt. Das ist es, was ein
  // gespeichertes fps von kFpsNative beim Oeffnen wird.
  double NativeFps(const std::string& subtype, int width, int height) const;

  // True when the combination is advertised by the driver as-is.
  bool IsAdvertised(const std::string& subtype, int width, int height, double fps) const;

  // Picks a sensible starting format: the largest resolution the incoming
  // signal actually has, at the highest advertised frame rate, preferring
  // uncompressed formats.
  //
  // "The largest the signal has" rather than "the largest on offer", and the
  // difference only shows on a card that is not pinned to one resolution: such
  // a card advertises everything up to 1080p whatever is plugged in, and taking
  // the biggest means recording a GameCube as 1080p -- scaled up by the card,
  // without a single detail gained. SetNativeStandard is what draws that line;
  // without it, or when nothing on offer fits underneath, the biggest wins as
  // it always did.
  //
  // `preferSubtype`, when given, outranks all of that as long as the card still
  // offers it. Re-reading a card is not a reason to lose the pixel format
  // somebody chose: the resolution may well have changed underneath, which is
  // the point of re-reading, but a card that could do RGB32 a moment ago can
  // still do it now. If it cannot, the ordinary choice applies.
  FormatSel PickDefault(const std::string& preferSubtype = std::string()) const;

  // Die Groesse, die ein Raster mit `activeLines` sichtbaren Zeilen (576 oder
  // 480) ohne Skalieren traegt, unter dem, was der Treiber fuer dieses
  // Pixelformat wirklich meldet -- mit derselben Vorliebe fuer die echte
  // Zeilenlaenge wie PickDefault. Breite 0, wenn nichts passt.
  ResolutionOption FittingResolution(const std::string& subtype, int activeLines) const;

 private:
  std::vector<CapsEntry> entries_;
  int nativeLines_ = 0;
  double nativeFieldRate_ = 0.0;
};

// Applies a format to the capture pin. Returns the media type that was actually
// negotiated in `applied` (optional). Failure means the driver rejected it.
HRESULT ApplyFormat(IPin* capturePin, const FormatSel& fmt, VideoFormatInfo* applied);

// -------------------------------------------------------------------- crossbar

struct CrossbarInput {
  // Whatever identifies this input to the card: the index of the input pin on
  // the crossbar, or, on a card whose inputs are switched through a private
  // property set instead, the vendor's own number for the connector.
  int pinIndex = -1;
  long physicalType = 0;
  std::string name;  // "HDMI", "Component (YPbPr)", "Composite", ...
};

// True for the connectors whose raster the analogue video standard settles
// completely: 625 lines carry 576 visible ones, 525 carry 480, and there is no
// third possibility on a composite or S-Video cable.
//
// Component and VGA are analogue as well and are deliberately not among them.
// They carry whatever the source feels like -- 480p, 720p, 1080i -- and no
// video standard describes any of it, so the line count says nothing there.
bool ConnectorFollowsVideoStandard(long physicalType);

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
std::vector<VideoDeviceInfo> EnumerateCrossbarDevices();

// Everything registered in one device category, by its GUID. EnumerateVideoDevices
// asks for CLSID_VideoInputDeviceCategory, which is what every capture program
// shows; a driver may register KS filters that the category does not surface.
// Diagnostic only -- the device list still comes from EnumerateVideoDevices.
std::vector<VideoDeviceInfo> EnumerateDeviceCategory(const GUID& category);

std::string PhysicalConnectorName(long physicalType);

// ---------------------------------------------------------------------------
// Analogue video standard
//
// The setting behind the video standard list in a capture driver's own dialog.
// It matters more than it looks: PAL is 625 lines at 50 Hz and PAL-60 is 525 at
// 60, so a console that can do both needs the card told which one it is sending.
// Set the wrong one and either nothing locks at all or the picture arrives with
// the wrong number of lines in it.
//
// Values are the AnalogVideo_* bitmask from the DirectShow headers.

// Everything a card might offer, common variants first. The config stores the
// bitmask value rather than the index, so the order is a presentation choice.
int VideoStandardCount();
long VideoStandardValue(int index);
const char* VideoStandardName(int index);
// Index of a bitmask value, or -1 when it is not one we know.
int VideoStandardIndexOf(long value);
// 525 or 625, or 0 when unknown. Derived from the standard, not measured.
int VideoStandardLines(long value);
// Halbbilder je Sekunde, oder 0 bei unbekannter Norm. 50 auf 625 Zeilen,
// 59,94 auf 525 -- ausser bei PAL 60, das glatte 60 hat. Ebenfalls abgeleitet
// und nicht gemessen: es ist die Rate, die das Signal haben *soll*.
double VideoStandardFieldRate(long value);

// The same list again, collapsed to the eight that actually look different on a
// baseband input. The letters behind PAL and SECAM describe the RF channel, not
// the picture, so offering them separately asks the user a question that has no
// answer. Groups are for the pickers only: what gets stored is still a concrete
// AnalogVideo_* value.
int VideoStandardGroupCount();
const char* VideoStandardGroupName(int index);
// One sentence on line count, subcarrier and where it is used. Follows the
// selected language.
const char* VideoStandardGroupHint(int index);
// The group a concrete value belongs to, or -1 for 0, -1 and anything unknown.
int VideoStandardGroupOf(long value);
// The value to apply for a group, chosen from what the card offers. Zero when
// the card offers none of the group's members.
long VideoStandardGroupPick(int index, long available);

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

// The order automatic selection tries, filtered to what `available` offers.
// One variant per line count and colour system: the PAL letters differ only in
// sound carrier, which is nothing to do with the picture.
//
// `region` sagt, welche Normen ueberhaupt in der Naehe des Nutzers vorkommen;
// sie stehen vorn. Das ist die einzige Auskunft, die die Reihenfolge innerhalb
// einer Zeilenzahl begruenden kann -- der Lock kann PAL 60 und NTSC M nicht
// trennen, also entscheidet die Reihenfolge, und ohne diese Angabe entschiede
// sie fuer alle gleich. Dahinter stehen trotzdem alle uebrigen: eine falsch
// eingestellte Region soll langsamer sein, nicht aussichtslos.
//
// `lastGood` ist die Norm, die zuletzt eingerastet war, oder 0. Ist sie
// gesetzt, stehen ihr Partner (dieselbe Farbe, andere Bildfrequenz) und dann
// sie selbst vor allem anderen -- die zwei Faelle, in denen ein Lock verloren
// geht, ohne dass jemand das Kabel gezogen hat. `preferred`, wenn angegeben,
// bekommt die Anzahl dieser vorgezogenen Eintraege; der Aufrufer gibt ihnen
// mehr Zeit als einem gewoehnlichen Kandidaten.
std::vector<long> AutoStandardCandidates(long available, VideoRegion region,
                                         long lastGood = 0, int* preferred = nullptr);

// Alle Normen derselben Zeilenzahl, `standard` selbst zuerst, dahinter die
// uebrigen in der Reihenfolge der Region. Leer, wenn die Zeilenzahl unbekannt
// ist.
//
// Genau hier sitzt die Luecke, die der Lock offenlaesst: er meldet nur einen
// waagerechten Lock, und 625/50 zerfaellt in PAL B, SECAM B und PAL N, 525/60
// in PAL 60, NTSC M, NTSC M (Japan), PAL M und NTSC 4.43. Waagerecht sehen die
// jeweils gleich aus. Rastet die Karte ein, ist damit die Zeilenzahl geklaert
// und sonst nichts -- und was von den uebrigen stimmt, sagt allein das Bild.
//
// Hier stand vorher `VideoStandardColourAlternative`, die *eine* Gegennorm mit
// dem anderen Farbtraeger. Das war zu wenig, und zwar aus einem Grund, der in
// der Tabelle steht: `VideoStandardSubcarrierSamples` kennt nur zwei Werte,
// 3,58 und 4,43 MHz, und SECAM faellt darin mit PAL B zusammen. Wer von SECAM B
// aus nach dem anderen Traeger fragte, bekam PAL N -- die richtige Antwort
// PAL B wurde uebersprungen, weil sie denselben Traeger hat. Die falsche
// SECAM-Dekodierung eines PAL-Signals war so nicht zu reparieren.
std::vector<long> VideoStandardColourCandidates(long standard, long available,
                                                VideoRegion region);

// Label for a stored setting: 0 "leave alone", -1 "automatic", otherwise the
// standard's name. Follows the selected language for the first two. This is the
// exact one, for logs and for the readout that says what the card really took.
std::string VideoStandardSettingName(long setting);
// The same, but naming the group instead of the variant. This is what the
// pickers show, so that what they echo back is something they also offered.
std::string VideoStandardPickerName(long setting);

// Wie die Farbe getragen wird. Zusammen mit dem Traeger unten und der
// Zeilenzahl beschreibt das ein Bild vollstaendig -- was darueber hinaus
// verschieden heisst, ist im Kabel nicht mehr verschieden. Der Farbrundgang
// misst danach, ob es sich lohnt, eine Norm ueberhaupt zu probieren.
enum class VideoColourSystem { Pal, Ntsc, Secam };
VideoColourSystem VideoStandardColourSystem(long standard);

// How many samples of a 720 pixel line one cycle of the colour subcarrier
// occupies. This is what the dot crawl filter needs to know: the crawl *is* the
// subcarrier leaking into brightness, so removing it means knowing its
// frequency. Both numbers follow from the standard and BT.601 sampling, and
// measuring the picture agrees with them to within two parts in a thousand.
double VideoStandardSubcarrierSamples(long standard);

}  // namespace cap
