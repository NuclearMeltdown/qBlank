#pragma once

// Persistent settings model. Everything the user can change lives here; the
// UI edits a Config, the capture/render/audio subsystems read one.
//
// Layout: a Config holds app-wide settings (window, theme, vsync) plus a list
// of profiles. A profile bundles everything that changes when you swap the
// source -- device, crossbar input, capture format, image and audio settings --
// so "SNES 4:3 nearest" and "PS2 Component 480i" are one hotkey apart.

#include <filesystem>
#include <string>
#include <vector>

#include "hotkeys.h"
#include "i18n.h"

namespace cap {

enum class Theme { Dark, Light, System };

enum class ScaleFilter { Nearest, Bilinear, Bicubic, Lanczos3, SharpBilinear };

// Which field of an interlaced frame is the earlier one. Analogue capture cards
// routinely describe their format with a plain VIDEOINFOHEADER, which has no
// field to put this in, so guessing is all that is left -- and guessing wrong
// makes every deinterlacer play the two fields in the wrong order, which looks
// like the picture juddering up and down.
enum class FieldOrder { Auto, TopFirst, BottomFirst };

// Appended rather than reordered: these are written to the config as numbers.
enum class Deinterlace { Off, Bob, BobLinear, MotionAdaptive, EdgeDirected, Yadif };
const int kDeinterlaceCount = 6;

// YADIF is the only mode that needs the frame before the current one, which is
// why it is the only one that costs anything to have switched on.
inline bool DeinterlaceNeedsHistory(Deinterlace d) { return d == Deinterlace::Yadif; }

// Quarter turns, applied after deinterlacing and before scaling, so what is
// recorded and screenshotted is rotated too.
enum class Rotation { None, Cw90, Half, Ccw90 };
const int kRotationCount = 4;

// Angehaengt wird hier, nie eingefuegt: der Wert steht als Zahl in der
// qBlank.json, und ein Einschub verschoebe stillschweigend jede gespeicherte
// Einstellung um eins.
enum class AspectMode { Source, Force16x9, Force4x3, Stretch, Integer, SquarePixels };
const int kAspectModeCount = 6;

enum class ColorRange { Auto, Limited, Full };

enum class ColorMatrix { Auto, BT601, BT709 };

// Which half of a hybrid card is in use. Cards like the PEXHDCAP60L carry a
// digital input and an analogue decoder on the same board, and the settings that
// matter differ completely between the two: a video standard, composite
// artefacts and half-height sources are analogue problems and simply do not
// arise over DVI. Auto means "analogue if the card has a decoder at all", which
// is right for every card that only does one of the two.
enum class SignalKind { Auto, Analog, Digital };
const int kSignalKindCount = 3;

// Which analogue connector the picture arrives on -- and with it, whether the
// colour rides on a subcarrier inside the brightness or comes in on wires of
// its own. Half the composite section depends on the answer: dot crawl and
// rainbow shimmer are not noise, they are the subcarrier itself, and where
// there is none the filters work on something that is not there.
//
// This is asked rather than measured, because it frequently cannot be
// measured. IAMCrossbar is the documented way and plenty of cards do offer it,
// but the SA7160 this program was built against does not: its input selector
// lives in the vendor's own property page, visible on screen and unreadable
// from outside. So the crossbar answers where there is one, and the person who
// plugged the cable in answers where there is not.
//
// Auto therefore means composite wherever the card stays quiet. That is the
// connector the filters exist for, and guessing it wrong on a cleaner input
// costs nothing worse than a few sliders too many.
enum class AnalogConnector { Auto, Composite, SVideo, Component };
const int kAnalogConnectorCount = 4;

// Beides zusammen ist eine einzige Frage, und sie wird auch als eine gestellt.
//
// Getrennt waren es zwei Auswahllisten uebereinander, die zweite nur manchmal
// da, und die erste beantwortete der Nutzer schon mit dem Kabel in der Hand:
// wer "Composite" sagt, hat damit auch "analog" gesagt. Die Kette ist
// Automatisch - Composite - S-Video - Component - Digital, und das ist die
// ganze Liste dessen, was hinten am Rechner steckt.
//
// Gespeichert bleiben trotzdem zwei Schluessel. Sie tragen zwei verschiedene
// Aussagen -- "welche Haelfte der Karte" und "welches Kabel daran" --, und ein
// dritter Schluessel, der beide zugleich meint, waere eine dritte Wahrheit, die
// mit den anderen auseinanderlaufen kann.
const int kSignalInputCount = 5;

// Wo der Nutzer wohnt -- und damit, welche Videonormen ueberhaupt in Frage
// kommen.
//
// Das ist keine Bequemlichkeit, sondern die einzige verlaessliche Auskunft, die
// es an dieser Stelle gibt. Der Lock der Karte misst nur die Zeilenfrequenz;
// PAL 60 und NTSC M sind beide 525/60 und unterscheiden sich allein im
// Farbtraeger, den er gar nicht anfasst. Wer zuerst gefragt wird, gewinnt also,
// und ohne diese Angabe entscheidet die Reihenfolge einer festen Liste, in
// welchem Land jemand zu wohnen hat.
//
// Deshalb steht das hier und nicht im Profil: ein Profil beschreibt ein Geraet,
// das hier beschreibt den Menschen davor. Wer eine importierte Konsole hat,
// stellt deren Norm im Profil von Hand ein -- dafuer ist die Auswahl da.
// `None` ist keine Region, sondern die Weigerung, eine zu nennen: dann wird
// nichts vorgezogen und die Suche laeuft in der Reihenfolge, in der die Normen
// auf der Welt vorkommen (siehe kCommon und kRare in dshow_util.cpp). Das ist
// die richtige Antwort fuer jemanden, an dessen Karte gemischtes Material
// haengt -- eine Sammlung importierter Konsolen hat keinen Wohnort --, und es
// ist zugleich die Einstellung, mit der sich das Verhalten *ohne* Region
// nachstellen laesst, wenn die Region einmal verdaechtigt wird.
enum class VideoRegion {
  Auto,          // aus der Laendereinstellung von Windows
  None,          // keine Bevorzugung, nur die allgemeine Reihenfolge
  PalEurope,     // PAL B/G/I/D, 625/50 mit 4,43 MHz
  NtscAmerica,   // NTSC M, 525/60 mit 3,58 MHz
  NtscJapan,     // dasselbe mit anderem Schwarzpegel
  Secam,         // SECAM, 625/50
  PalBrazil,     // PAL M, 525/60 mit 3,58 MHz
  PalArgentina,  // PAL N, 625/50 mit 3,58 MHz
};
const int kVideoRegionCount = 8;

enum class AudioSource { Embedded, Manual, None };

enum class OsdCorner { TopLeft, TopRight, BottomLeft, BottomRight };

// How the microphone ends up in the file. "Both" costs one more AAC track --
// about a megabyte a minute against a recording of well over a hundred -- and
// means the file plays correctly in anything while still being separable in an
// editor.
enum class MicTrackMode { Both, Mixed, Separate };

// How much of the statistics panel is shown. The full panel is a wall of text
// to read while playing, so the useful numbers come first and the rest is opt in.
enum class StatsDetail { Compact, Normal, Full };

// What curve the source is encoded against. Automatic believes what the driver
// put in the media type, which is right when it is there at all and absent on
// most cards -- hence the override.
enum class HdrInput { Auto, Sdr, Pq, Hlg };
inline constexpr int kHdrInputCount = 4;

// Whether to hand the screen high dynamic range. Automatic does it only when
// the screen is in HDR mode *and* the source actually is HDR, which avoids
// putting an ordinary picture through a conversion it does not need.
enum class HdrOutput { Off, Auto, Always };
inline constexpr int kHdrOutputCount = 3;

enum class RecordContainer { Mkv, Mp4 };

// How the bitrate is spent. The three every encoder here understands, under one
// set of names -- what each vendor calls them differs, and translating that is
// the recorder's job rather than the user's.
enum class RateControl {
  Cbr,      // the same every second. What streaming wants.
  Vbr,      // spends more where the picture needs it, up to the ceiling.
  Quality,  // no bitrate at all: a quality target, and the file is what it is.
};
inline constexpr int kRateControlCount = 3;

// Speed against quality, seven steps because that is what NVENC offers and the
// others map onto it. Auto leaves it to the encoder's own default.
enum class EncoderPreset { Auto, Fastest, Faster, Fast, Medium, Slow, Slower, Slowest };
inline constexpr int kEncoderPresetCount = 8;

// What the encoder should optimise for. Only NVENC and AMF have an opinion.
enum class EncoderTune { Auto, Quality, LowLatency };
inline constexpr int kEncoderTuneCount = 3;

// Two passes cost time and buy accuracy near the bitrate ceiling. NVENC only.
enum class Multipass { Auto, Off, Quarter, Full };
inline constexpr int kMultipassCount = 4;

// PNG is lossless and the sane default for a capture card: the point of a still
// is usually to look at it closely. JPEG is there for whoever takes hundreds.
enum class ScreenshotFormat { Png, Jpeg };

// What an HDR screenshot comes out as. JPEG XR needs nothing: Windows ships the
// encoder, and the Photos app reads it. AVIF is read by every browser and by
// most things that are not Windows -- but it goes through ffmpeg, and
// screenshots are otherwise the one part of qBlank that never needs it.
enum class HdrShotFormat { Jxr, Avif };
inline constexpr int kHdrShotFormatCount = 2;

// Which encoder ffmpeg is asked for. Auto picks the best one that survived the
// test encode, preferring hardware. The AV1 entries need recent hardware
// (NVIDIA RTX 40, Intel Arc, AMD RDNA 3); on anything older the probe simply
// marks them unavailable.
// AutoEfficient is appended rather than inserted: these values are written to
// the config file as plain numbers, and renumbering them would silently change
// what an existing configuration means.
enum class RecordEncoder {
  Auto, X264, Nvenc, QuickSync, Amf, X265, NvencHevc, Av1Nvenc, Av1QuickSync, Av1Amf,
  AutoEfficient
};
const int kRecordEncoderCount = 11;

// Both automatic modes pick an encoder for you; the difference is what they
// optimise for. Neither refers to a specific encoder, so anything that looks one
// up by id has to check this first.
inline bool IsAutoEncoder(RecordEncoder e) {
  return e == RecordEncoder::Auto || e == RecordEncoder::AutoEfficient;
}

// Only the software encoder gets a speed knob -- for the hardware encoders the
// vendor defaults are better than anything guessed here.
enum class RecordSpeed { UltraFast, VeryFast, Faster, Fast, Medium };

// UI labels in the language currently selected, taken by enum value. Passing an
// out of range index is safe and yields the first entry.
const char* ThemeName(int index);
const char* LanguageName(int index);
const char* ScaleFilterName(int index);
const char* DeinterlaceName(int index);
const char* FieldOrderName(int index);
const char* RotationName(int index);
const char* SignalKindName(int index);
const char* AnalogConnectorName(int index);
// Was in der Auswahlliste unter dem Namen steht: woran man den Stecker in der
// Hand erkennt. Leer fuer Auto, das ist kein Anschluss.
const char* AnalogConnectorLook(int index);
// Die zusammengelegte Liste: Automatisch, Composite, S-Video, Component,
// Digital. `SignalInputLook` beschreibt wieder den Stecker, fuer Automatisch
// stattdessen, was Automatisch heisst.
const char* SignalInputName(int index);
const char* SignalInputLook(int index);
// Und die Umrechnung in beide Richtungen. Kombinationen, die die Liste nicht
// hergibt, faellt `SignalInputIndex` auf den naechstliegenden Eintrag zurueck --
// gespeicherte Profile von vor der Zusammenlegung sind genau das.
int SignalInputIndex(SignalKind kind, AnalogConnector connector);
void SignalInputApply(int index, SignalKind* kind, AnalogConnector* connector);
const char* VideoRegionName(int index);
// Was `VideoRegion::Auto` bedeutet: die Region aus der Laendereinstellung von
// Windows, oder PalEurope, wenn sich daraus nichts machen laesst. Gibt nie
// wieder Auto zurueck.
VideoRegion ResolveVideoRegion(VideoRegion region);
const char* AspectName(int index);
const char* ColorRangeName(int index);
const char* ColorMatrixName(int index);
const char* OsdCornerName(int index);
const char* StatsDetailName(int index);
const char* MicTrackModeName(int index);
const char* RecordContainerName(int index);
const char* ScreenshotFormatName(int index);
const char* RecordEncoderName(int index);
const char* RecordSpeedName(int index);

// Short explanations shown as tooltips. Empty string when there is nothing
// worth saying beyond the label.
const char* ScaleFilterHelp(int index);
const char* DeinterlaceHelp(int index);
const char* MaskName(int index);
const char* MaskHelp(int index);
const char* AspectHelp(int index);

// Accent colour presets offered in the settings, as 0xRRGGBB.
struct AccentPreset {
  unsigned rgb;
  const char* (*name)();
};
int AccentPresetCount();
unsigned AccentPresetColor(int index);
const char* AccentPresetName(int index);

// Identifies a device across restarts. `id` is the stable one, whatever the
// platform uses to name a device for good; `name` is what the user sees and is
// used as a fallback when the id no longer resolves (e.g. card moved slots).
struct DeviceRef {
  std::string name;
  std::string id;
  // Audio inputs only: which way of reading a device found this one. Opaque
  // everywhere but in the platform's audio code, which set it and reads it.
  std::string backend;

  bool empty() const { return name.empty() && id.empty(); }
  bool operator==(const DeviceRef& o) const {
    return name == o.name && id == o.id && backend == o.backend;
  }
};

// Zwei Betriebsarten anstelle einer Zahl, beide erst aufgeloest, wenn die Karte
// offen ist. Eine Zahl, die heute stimmt, ist falsch, sobald die Konsole den
// Modus wechselt -- deshalb steht sie gar nicht erst in der Konfiguration.
//
// Ein aelteres qBlank liest beide als "hoechste verfuegbare", weil es nur auf
// `fps <= 0` prueft. Das ist genau das alte Verhalten und damit in Ordnung.
inline constexpr double kFpsHighest = 0.0;  // was die Karte hergibt
inline constexpr double kFpsNative = -1.0;  // was die Norm des Signals vorgibt

// One concrete capture format. `subtype` is a short label such as "YUY2",
// "UYVY", "NV12", "RGB24" or "MJPG".
struct FormatSel {
  std::string subtype;
  int width = 0;
  int height = 0;
  // Eine Rate in Hz, oder kFpsHighest / kFpsNative.
  //
  // Von Haus aus die des Signals. "Hoechste verfuegbare" fordert an einer
  // PAL-Konsole von einer Karte, die 60 kann, mehr Bilder an, als das Signal
  // hat. Wo keine Norm bekannt ist, am Digitaleingang, laeuft die Rate des
  // Signals ohnehin auf die hoechste hinaus; siehe VideoCapture::Start.
  double fps = kFpsNative;
  // True when this combination is not advertised by the driver but lies inside
  // the ranges it reports -- the 1080p60 case.
  bool forced = false;

  bool valid() const { return width > 0 && height > 0 && !subtype.empty(); }
  bool SameFormat(const FormatSel& o) const {
    return subtype == o.subtype && width == o.width && height == o.height;
  }
  // "1920x1080 @ 60,00 Hz  YUY2"
  std::string Label() const;
};

struct CaptureSettings {
  DeviceRef video;
  AudioSource audioSource = AudioSource::Embedded;
  DeviceRef audio;         // only used when audioSource == Manual
  int crossbarInput = -1;  // index into the enumerated crossbar inputs, -1 = leave alone
  // Analogue video standard. 0 leaves whatever the card was set to alone, which
  // is what it did before this existed; -1 lets qBlank find it by watching
  // whether the decoder locks; anything else is an AnalogVideo_* bitmask.
  long videoStandard = 0;
  SignalKind signalKind = SignalKind::Auto;
  // Only asked once the source counts as analogue. See AnalogConnector.
  AnalogConnector connector = AnalogConnector::Auto;
  FormatSel format;
};

// Bis zu welcher Zeilenzahl eine Quelle als halbe Bildhoehe gilt -- siehe
// lineDouble weiter unten. 288 ist PAL 288p, 240 ist NTSC 240p; alles darueber
// hat seine Zeilen schon.
inline constexpr int kHalfHeightLines = 288;

// Die groesste Zeilenzahl, die noch aus einem Standardraster stammen kann: 576
// ist PAL, 480 ist NTSC. Darueber hat entweder die Quelle wirklich mehr Zeilen,
// oder die Karte hat hochgerechnet -- und alles, was in Bildpunkten der Quelle
// rechnet, findet dann ein Raster vor, das es so nie gab.
inline constexpr int kStandardLines = 576;

// Ein Zuschnitt, der zu einer bestimmten Quellgroesse gehoert.
//
// Die Groesse ist der Schluessel und nicht die Videonorm: gemessen werden
// Bildpunkte, und was sie beschreiben, haengt an der Groesse des Bildes, in
// dem sie gezaehlt wurden. Dieselbe Norm kann in verschiedenen Formaten
// ankommen, und zwei Normen koennen dasselbe Format liefern -- die Groesse ist
// die Auskunft, die zu den Zahlen passt.
struct CropForFormat {
  int width = 0;
  int height = 0;
  int left = 0;
  int right = 0;
  int top = 0;
  int bottom = 0;
};

struct ImageSettings {
  ScaleFilter filter = ScaleFilter::Bilinear;
  float sharpen = 0.0f;  // 0..1, contrast-adaptive sharpening applied after scaling

  // The four knobs every capture program has. Ours are in the shader rather
  // than on the card, and that is the whole point: the card's own set is
  // neutralised when the graph is built, so what arrives here is what the
  // console sent. See NeutraliseProcAmp -- a decoder quietly lifting the black
  // level would otherwise mean there is no clean signal anywhere to go back to.
  float brightness = 0.0f;  // -1..1, added; 0 neutral
  float contrast = 1.0f;    // 0..2, around a pivot; 1 neutral
  float saturation = 1.0f;  // 0..2, toward luma; 1 neutral
  float hue = 0.0f;         // -180..180 degrees; 0 neutral
  // Whether the four above reach past the window, exactly like
  // squarePixelOutput below -- but off by default, and that asymmetry is
  // deliberate. Recording clean and grading later costs nothing: the same curve
  // goes on in the editor. Recording with contrast baked in clips highlights to
  // full scale and crushes shadows to zero, and no amount of editing brings
  // those back. Of the two settings only one can destroy, so the one that
  // cannot is the default.
  bool procAmpToOutput = false;

  // What a cathode ray tube did to the picture. Off by default, and deliberately
  // so: the rest of this program exists to hand over the signal as clean as it
  // arrived, and these put something back that was never in it. They are here
  // because 240p artwork was drawn for a display that had line gaps and a
  // coloured mask, and some people want that back.
  //
  // Display only. Recordings, screenshots and the virtual camera take the
  // intermediate, which is upstream of the pass these run in.
  // What the source really has across, when the card is sampling it at some
  // other rate. 0 leaves it alone. Display only, like the two below it.
  int nativeWidth = 0;
  // Wie viele Bildzeilen die Quelle wirklich hat, wenn mehr ankommen als sie
  // gezeichnet hat. 0 heisst: selbst herausfinden, so wie es immer war.
  //
  // Die Zahl gibt es, weil sie sich nicht immer messen laesst. Eine Karte, die
  // erst bei 720p anfaengt, oder ein Dongle mit eigenem Skalierer liefert 1080
  // Zeilen von einer Konsole, die 480 gezeichnet hat -- und nichts im Bild sagt
  // zuverlaessig, welche der beiden Zahlen die echte ist. Der Mensch davor weiss
  // es, weil er die Konsole angesteckt hat.
  //
  // Waagerecht gilt sie ausdruecklich nicht; dafuer gibt es nativeWidth, und die
  // beiden haben verschiedene Voraussetzungen. Siehe dort.
  int sourceLines = 0;
  float scanlines = 0.0f;   // 0..1, how dark the gaps between source lines go
  int mask = 0;             // 0 off, 1 aperture grille, 2 shadow mask
  float maskStrength = 0.35f;
  Deinterlace deinterlace = Deinterlace::Bob;
  bool deinterlaceAuto = true;  // only kick in when the source looks interlaced
  FieldOrder fieldOrder = FieldOrder::Auto;
  // Every source line filled into two. For a 240p or 288p console that the card
  // hands over at its true line count, which otherwise arrives half as tall as
  // it should be.
  //
  // Nur dort. Eine Quelle mit voller Zeilenzahl hat nichts zu verdoppeln, und
  // das Ergebnis waere im Fenster unsichtbar (dort entscheidet das
  // Seitenverhaeltnis, nicht die Zeilenzahl) und in der Aufnahme falsch.
  // kHalfHeightLines zieht die Grenze: 288 ist die groesste Hoehe, die noch als
  // halbe Bildhoehe durchgeht -- PAL 288p, darunter NTSC 240p.
  bool lineDouble = false;
  Rotation rotation = Rotation::None;

  // Composite clean-up. Two separate defects, two separate knobs -- see the
  // shader for why one cannot fix the other.
  //
  // chromaSoft blurs the colour horizontally, in source pixels. Composite
  // carries colour at about a quarter of the bandwidth of brightness, so there
  // is no fine colour detail in the signal to lose; what goes away is the
  // rainbow shimmer over dense patterns.
  int chromaSoft = 0;  // 0 off, up to 8 source pixels either side
  // temporalDenoise averages with the previous frame wherever nothing moved.
  // The colour subcarrier flips phase from frame to frame, so that average is
  // what cancels dot crawl -- and analogue noise goes with it.
  float temporalDenoise = 0.0f;  // 0..1
  // Wo der Mittelwert wieder losgelassen wird. Er kann nur mitteln, und Mitteln
  // ueber Bewegung ist Schmieren -- also entscheidet ein Gatter pro Bildpunkt,
  // und dieses Flag verschiebt dessen Arbeitspunkt. Aus: das Gatter haelt lange
  // fest, weil Punktkriechen sich selbst bewegt und ein empfindliches Gatter
  // den Filter genau dort abschaltet, wo das Artefakt sitzt. An: es laesst
  // frueh los, was bewegte Kanten sauber macht und dafuer etwas Kriechen an
  // langsamen Stellen stehen laesst.
  bool avoidGhosting = false;
  // The same defect where the temporal filter cannot reach it, which in a
  // running game is most of the screen. Costs a little horizontal sharpness,
  // which is why it is a knob of its own rather than part of the one above.
  float dotNotch = 0.0f;  // 0..1
  // Averaging along the movement instead of across it, for the noise the gate
  // above leaves behind everywhere something is moving -- which in a game is
  // most of the screen. It does not touch the dot crawl and cannot be made to:
  // the pattern is fixed to the raster rather than to the picture, so shifting
  // a frame shifts its phase, and the four frame cancellation only survives at
  // speeds that are a multiple of a quarter of the carrier period. See the
  // shader, which does the algebra. So this is band limited away from the
  // carrier and removes noise, which is the same defect in every colour system.
  bool motionCompensate = false;
  // The top of the brightness band, put back. Composite rolls off towards the
  // subcarrier at both ends of the chain, and that rolloff is what a composite
  // picture is soft with -- known rather than guessed, because its shape
  // follows the carrier. Not the same thing as the sharpening on the scaling
  // section: that one raises contrast at edges after scaling, this one lifts a
  // band the transmission attenuated, in the source's own samples.
  float bandwidthRestore = 0.0f;  // 0..1
  // Whether the colour softening picks its spots. Off it blurs colour sideways
  // over the whole picture; on it does so only where the brightness carries
  // enough at the carrier frequency for the decoder to have invented colour out
  // of it, and leaves genuine colour detail alone everywhere else.
  bool adaptiveChroma = false;
  // Die halbe Anzeige ohne die Filter oben, damit sich beurteilen laesst, was
  // sie tun. Der Schnitt liegt im Quellraster und nicht im Fenster: so folgt er
  // dem Bild, wenn es gedreht oder beschnitten ist, statt quer darueber zu
  // liegen. Links das Rohbild, rechts das gefilterte -- und nur die Kette aus
  // diesem Abschnitt ist betroffen, Schaerfen, Skalierung und Roehrenmaske
  // laufen ueber beide Haelften, sonst waere der Vergleich keiner.
  //
  // Wird nicht gespeichert und im Profil nicht gefuehrt: ein Profil kommt immer
  // mit false an, und was der Renderer sieht, setzt App::EffectiveImage aus
  // App::compare_. Ein Vergleich ist eine Sehhilfe wie das Standbild und soll
  // einen Neustart ebenso wenig ueberleben. Die Lage der Trennlinie dagegen ist
  // eine Vorliebe und bleibt, ebenso ihre Richtung.
  bool compare = false;
  float compareSplit = 0.5f;  // 0..1, Anteil der Breite links (Hoehe oben) vom Schnitt
  bool compareHorizontal = false;  // Trennlinie waagerecht, ungefiltert oben
  AspectMode aspect = AspectMode::Source;
  // Whether the aspect above reaches past the window. The display gets it for
  // free -- it simply draws into a rectangle of the right shape -- but a
  // recording, a screenshot and the virtual camera are all just a pixel grid,
  // and a grid with no room for "these pixels are not square" in it. So the
  // picture is resampled to square pixels on its way out, with the scaling
  // filter chosen above, and 720x576 leaves as 768x576.
  //
  // On HDMI this changes nothing at all: those pixels are already square, the
  // sizes come out equal and the pass is skipped. It exists for the analogue
  // standards, where leaving it off means a 4:3 console arriving in OBS as 5:4
  // -- seven percent too narrow, and no way for anything downstream to know.
  bool squarePixelOutput = true;
  int cropLeft = 0;
  int cropRight = 0;
  int cropTop = 0;
  int cropBottom = 0;
  // Ob der Zuschnitt je Quellgroesse gemerkt wird.
  //
  // Aus heisst: die vier Zahlen gelten fuer die Groesse, an der sie gemessen
  // wurden, und werden zurueckgesetzt, sobald eine andere kommt (siehe
  // App::UpdateCropForFormat -- sie sind Bildpunkte, keine Anteile, und "oben
  // 17" beschreibt bei 480 Zeilen einen anderen Streifen als bei 576).
  //
  // An heisst: sie werden unter der alten Groesse abgelegt und beim naechsten
  // Mal von dort wieder geholt. Das ist der GameCube, der zwischen 50 und 60
  // Hz wechselt: dieselbe Konsole, dasselbe Kabel, dieselben Filter -- nur der
  // schwarze Rand sitzt anders. Dafuer zwei Profile zu fuehren, hiesse jede
  // andere Einstellung doppelt zu pflegen.
  //
  // Aus, weil ein Zuschnitt, der sich beim Formatwechsel von selbst aendert,
  // erklaert werden muss, bevor er hilft.
  bool cropPerFormat = false;
  ColorRange range = ColorRange::Auto;
  ColorMatrix matrix = ColorMatrix::Auto;
  // Die abgelegten Zuschnitte, einer je Quellgroesse. Nur gefuellt, wenn
  // `cropPerFormat` an ist oder einmal an war; die vier Zahlen oben bleiben
  // dabei der geltende Stand, dies hier ist das Gedaechtnis dahinter.
  std::vector<CropForFormat> cropVariants;
};

struct AudioSettings {
  DeviceRef output;        // empty = system default output
  int bufferMs = 30;       // target buffer between capture and playback
  bool exclusive = false;  // WASAPI exclusive mode on the output endpoint
  int avOffsetMs = 0;      // >0 delays audio, <0 delays video
  float volume = 1.0f;     // 0..1 linear
  bool mute = false;

  // A second input that only goes into the recording, as its own track. Never
  // played back -- monitoring your own microphone through the same speakers the
  // game comes out of is a feedback loop, not a feature.
  bool micEnabled = false;
  DeviceRef micDevice;   // empty = system default recording device
  float micGain = 1.0f;  // linear, 0..4 (up to +12 dB), independent of Windows
  MicTrackMode micTrackMode = MicTrackMode::Both;
};

// Recording is deliberately thin: it writes what the viewer shows, at the
// source resolution, and the only knobs are the ones that change the result in
// a way a preset cannot guess.
struct RecordSettings {
  std::string outputFolder;  // empty = Videos\qBlank
  RecordContainer container = RecordContainer::Mkv;
  RecordEncoder encoder = RecordEncoder::Auto;
  RecordSpeed speed = RecordSpeed::VeryFast;  // software encoder only
  int bitrateKbps = 20000;


  // ---- what the encoder is told ----
  //
  // All of these are "leave it alone" by default, so a recording made without
  // touching any of them is exactly what it was before these existed.
  RateControl rateControl = RateControl::Cbr;
  // For RateControl::Quality. Lower is better; the scale differs a little
  // between encoders, which is why it is a number and not a word.
  int qualityLevel = 23;
  EncoderPreset preset = EncoderPreset::Auto;
  EncoderTune tune = EncoderTune::Auto;
  Multipass multipass = Multipass::Auto;
  // Looking ahead costs latency and buys better bit distribution; adaptive
  // quantisation spends bits where the eye looks. Both cost a little speed.
  bool lookAhead = false;
  bool adaptiveQuant = false;
  // 0 = whatever the source delivers. Otherwise frames are dropped to hit this.
  double fps = 0.0;
  // Off by default: only FAT32 needs it, NTFS does not.
  bool splitFiles = false;
  int splitSizeMb = 4000;
  // Path to ffmpeg.exe. Empty means: look next to qBlank, then on PATH.
  std::string ffmpegPath;

  // Result of the encoder test, kept so the list is filled on the next start
  // instead of making everyone test again. The signature is the graphics
  // hardware plus the ffmpeg build it was measured with; when either changes
  // the result is thrown away, because it is no longer about this machine.
  std::string encoderProbeSignature;
  std::vector<int> encodersAvailable;  // RecordEncoder values that worked

  // Stills. Empty folder = Pictures\qBlank. Screenshots go somewhere separate
  // from the recordings on purpose: a folder holding both a handful of videos
  // and four hundred stills is a folder nobody can find anything in.
  std::string screenshotFolder;
  ScreenshotFormat screenshotFormat = ScreenshotFormat::Png;
  int jpegQuality = 92;
  // Ob die Bedienoberflaeche mit ins Bild kommt. Aus heisst: abgegriffen wird
  // das Zwischenbild, also die Quelle in ihrer eigenen Aufloesung, ohne
  // Werkzeugleiste und Anzeigen. An heisst: der Rueckpuffer nach dem Zeichnen
  // der Oberflaeche -- das ist dann ein Abbild des Fensters und hat dessen
  // Groesse, nicht die der Quelle.
  bool screenshotIncludeUi = false;
};

struct Profile {
  std::string name = "Standard";
  // Bei welcher erkannten Videonorm dieses Profil von selbst gewaehlt wird.
  // 0 heisst: nie, und das ist die Voreinstellung -- ein Profil zu wechseln
  // ist ein sichtbarer Eingriff, und ungefragt geschieht er nicht.
  //
  // Gespeichert wird eine Norm wie in `capture.videoStandard`, verglichen wird
  // aber ueber die Gruppe (VideoStandardGroupOf): PAL B und PAL D sehen am
  // Kabel gleich aus, und ein Profil, das nur bei genau einem der beiden
  // greift, waere eine Falle. Die Gruppe ist zugleich die Unterscheidung, um
  // die es hier ueberhaupt geht -- PAL gegen PAL 60 ist der GameCube, der
  // zwischen 50 und 60 Hz wechselt.
  //
  // Wirkt nur, solange `capture.videoStandard` auf Automatisch steht. Eine
  // feste Norm wird nicht gesucht, also wird auch nichts erkannt, und ein
  // Profil, das nach dem Umschalten nichts mehr erkennen kann, waere eine
  // Einbahnstrasse. Die Einstellungen sagen das an der Stelle, an der es
  // auffaellt.
  long autoSelectStandard = 0;
  CaptureSettings capture;
  ImageSettings image;
  AudioSettings audio;
};

struct AppSettings {
  // Follows Windows unless told otherwise. Only a first run reaches this --
  // anyone with a configuration file has their own answer written down -- and on
  // a first run the program has nothing better to go on than what the rest of
  // the desktop is already doing.
  Theme theme = Theme::System;
  Language language = Language::English;
  // In welchem Land der Nutzer wohnt. Bestimmt, in welcher Reihenfolge die
  // automatische Normerkennung die analogen Videonormen durchprobiert -- siehe
  // VideoRegion und AutoStandardCandidates. Steht hier und nicht im Profil,
  // weil man nicht je Geraet in einem anderen Land wohnt.
  VideoRegion videoRegion = VideoRegion::Auto;
  // Drives the whole palette: buttons, sliders, borders and the shade of the
  // window background are all derived from this one colour. 0xRRGGBB.
  unsigned accentColor = 0x8B5CF6;  // violet
  bool vsync = false;  // off means "allow tearing" -- the biggest latency lever
  bool alwaysOnTop = false;
  bool hideCursorFullscreen = true;
  bool preventSleep = true;
  bool showStats = false;
  // The button strip along the top. On by default: everything it offers is also
  // on a key, but a key you have to know about first.
  bool showToolbar = true;
  // The settings as a window of the operating system's own rather than one
  // drawn inside the preview, so it can be put on another monitor or beside the
  // picture instead of on top of it.
  // Standardmaessig ein eigenes Fenster. Es hat ein eigenes Direct3D-Geraet und
  // ist damit vollstaendig von der Vorschau entkoppelt -- die Groesse und die
  // Lage der Vorschau schraenken es nicht ein, und es kann auf einen zweiten
  // Bildschirm. Das eingebettete Feld bleibt, weil es Faelle gibt, in denen ein
  // zweites Fenster stoert: eine Fensteraufnahme in OBS sieht es nicht.
  bool settingsSeparateWindow = true;
  // Where that window was last left. Kept because it is destroyed and rebuilt
  // every time the setting is switched, and a window that jumps back to the
  // middle of the screen each time is a window you have to keep putting back.
  // -1 means "let Windows place it", which is right the first time only.
  // Which tab the settings were last on. Reopening on the tab somebody was
  // working in is worth the four bytes; reopening on the first one every time
  // means finding the same thing again on every visit.
  int settingsTab = 0;
  // Wie oft der Toast zu einer Datei schon dazugesagt hat, dass ein Klick sie
  // im Ordner zeigt. Nach ein paar Malen weiss man es, und wer einmal
  // geklickt hat, weiss es sofort -- dann steht hier gleich die Hoechstzahl
  // (kFileToastHints in app.cpp).
  int fileToastHints = 0;
  // Wo das *eingebettete* Feld zuletzt stand, im Hauptfenster. Getrennt von der
  // Lage des freigestellten Fensters gefuehrt: das sind zwei verschiedene Orte
  // in zwei verschiedenen Bezugssystemen, und der eine soll den anderen nicht
  // verschieben.
  int settingsPanelX = -1;
  int settingsPanelY = -1;
  int settingsPanelW = 0;
  int settingsPanelH = 0;
  int settingsWindowX = -1;
  int settingsWindowY = -1;
  int settingsWindowW = 0;
  int settingsWindowH = 0;
  // Ask GitHub for the newest release once at startup. Only ever asks -- the
  // download is a separate, explicit action.
  bool checkUpdatesOnStart = true;
  // Offer the picture to other programs as a webcam. Remembered, so it comes
  // back with qBlank -- the camera itself only exists while qBlank runs.
  bool virtualCamera = false;

  // ---- high dynamic range ----
  HdrInput hdrInput = HdrInput::Auto;
  HdrOutput hdrOutput = HdrOutput::Auto;
  // Diffuse white -- the brightness of a sheet of paper in the picture, and the
  // level ordinary content is anchored to. 203 nits is what BT.2408 recommends
  // and what most HDR material is graded against.
  float paperWhiteNits = 203.0f;
  // How bright the source is assumed to get. DirectShow carries no mastering
  // metadata at all, so there is nothing to read this from -- and it matters:
  // assuming ten thousand where the content only reaches a thousand drags the
  // whole picture down by a third when tone mapping to an ordinary screen.
  float sourcePeakNits = 1000.0f;
  // Keep the range when recording, rather than writing what an ordinary screen
  // would have shown. Costs a ten bit encoder and a player that understands PQ.
  bool recordHdr = false;
  // Screenshots that keep the range, written as JPEG XR rather than PNG.
  bool screenshotHdr = false;
  HdrShotFormat hdrShotFormat = HdrShotFormat::Jxr;
  // Offer the virtual camera in ten bit PQ as well as the ordinary eight bit.
  // Off by default, and deliberately: almost nothing on the other end knows
  // what to do with an HDR webcam yet.
  bool cameraHdr = false;
  StatsDetail statsDetail = StatsDetail::Compact;
  bool logToFile = false;
  LogRetention logRetention;

  // Where the volume readout appears when it changes.
  OsdCorner osdCorner = OsdCorner::TopRight;
  bool showVolumeOsd = true;
  // Mouse wheel over the picture changes the volume.
  bool wheelVolume = true;

  // Where the window was, and a sentinel for "never saved". That sentinel must
  // be a number no monitor can produce, which -1 is not: a display placed to the
  // left of the primary one has nothing *but* negative coordinates, so treating
  // negative as unset means such a monitor can never be restored to.
  static const int kWindowPosUnset = (-2147483647 - 1);
  int windowX = kWindowPosUnset;
  int windowY = kWindowPosUnset;
  int windowW = 1280;
  int windowH = 720;
  bool maximized = false;
  bool startFullscreen = false;
  // -1 = whichever monitor the window is currently on.
  int fullscreenMonitor = -1;
};

struct Config {
  AppSettings app;
  RecordSettings record;
  Hotkeys hotkeys;
  std::vector<Profile> profiles;
  int activeProfile = 0;

  Config();

  Profile& active();
  const Profile& active() const;
  void SetActiveProfile(int index);

  // qBlank.json next to the executable -- portable, no registry.
  static std::filesystem::path FilePath();

  // Returns false when the file is missing or unreadable; defaults are kept in
  // that case and `error` describes the problem (empty if simply absent). On
  // success `error` can still carry a note for the log.
  bool Load(std::string* error = nullptr);
  bool Save(std::string* error = nullptr) const;

  // The exact text Save would write. Comparing two of these is how the app
  // notices that something changed without having to hand-write an equality
  // operator that would go stale the moment a field is added.
  std::string Serialize() const;
};

// A settings file lying next to the program under a name this build does not
// use -- the leftovers of a rename, or of a copy someone kept.
struct ForeignSettings {
  std::filesystem::path path;  // the full path
  std::string stem;            // the file name without ".json"
  std::string program;         // the name the file gives the program; older files say nothing

  // The language those settings are in, as the Language enum, -1 when the file
  // does not say. The question about this file has to be asked in it: whoever
  // set the program to German has not agreed to read English to get their
  // profiles back.
  int language = -1;

  // Last written, in seconds since 1970-01-01 UTC, 0 when unknown. Shown in
  // the question, because when each file was last touched is the one fact that
  // actually tells the two apart.
  int64_t modified = 0;
};

// Every settings file next to the executable that is ours but is not the one
// this build writes, newest first. Recognised by content, so no list of former
// names has to be kept correct for this to work.
std::vector<ForeignSettings> FindForeignSettings();

// The language stored in one settings file, as the Language enum, -1 when the
// file cannot be read or does not say. For the one moment before the settings
// are loaded, when something already has to be said in the right language.
int SettingsLanguage(const std::filesystem::path& path);

// Asked once per foreign file: take those settings over, or start fresh?
// `haveOwn` says whether there are settings under the current name as well --
// with them, Discard means keeping the ones already there; without, it means
// starting from defaults.
enum class SettingsAnswer {
  Postpone,  // no answer given -- change nothing, ask again next time
  TakeOver,  // use the foreign file from now on
  Discard,   // set the foreign file aside
};

using SettingsChoice = SettingsAnswer (*)(const ForeignSettings& found, bool haveOwn);

// Settles which settings file this build uses. Call before anything reads one;
// the answer costs nothing then, because nothing has been loaded yet that would
// have to be thrown away.
//
// Returns the name the adopted settings were written under, empty when nothing
// was adopted. Whatever loses is set aside as ".bak", never deleted.
std::string AdoptSettings(SettingsChoice ask);

}  // namespace cap
