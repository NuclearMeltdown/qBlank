#pragma once

// What a capture device says it can do, and the dropdown lists derived from it.

#include <string>
#include <vector>

#include "config.h"

namespace cap {

// One capability as the capture backend reports it. Drivers vary: some return
// a row per (format, resolution, fps), others a row per (format, resolution)
// with a frame rate range.
struct CapsEntry {
  std::string subtypeLabel;
  // Whether the renderer can take this format without a decoder in front of
  // it. Filled in by the backend, which is the one that knows the format.
  bool rendererOk = false;
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

  // Welche Videonorm vorne anliegt, als Bitmaske aus video_standard.h, oder 0, wenn das
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

}  // namespace cap
