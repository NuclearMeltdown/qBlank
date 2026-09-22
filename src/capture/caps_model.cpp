#include "capture/caps_model.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "capture/video_standard.h"

namespace cap {
namespace {

// Resolutions offered on top of what the driver lists.
struct StdRes {
  int w, h;
};
const StdRes kStandardResolutions[] = {
    {320, 240},   {640, 360},   {640, 480},   {720, 480},   {720, 576},   {800, 600},
    {960, 540},   {1024, 768},  {1280, 720},  {1280, 800},  {1280, 1024}, {1360, 768},
    {1440, 900},  {1600, 900},  {1680, 1050}, {1920, 1080}, {1920, 1200}, {2560, 1440},
    {3840, 2160},
};

// DirectShow itself has no frame rate ceiling worth speaking of -- it stores the
// interval in 100 ns units -- so this list only has to cover what hardware and
// monitors actually run at, including the high refresh rates.
const double kStandardFps[] = {23.976, 24.0,  25.0,  29.97, 30.0,  48.0,  50.0,
                               59.94,  60.0,  72.0,  75.0,  90.0,  100.0, 119.88,
                               120.0,  144.0, 165.0, 180.0, 200.0, 240.0};

bool FpsNear(double a, double b) {
  return std::fabs(a - b) < 0.05;
}

}  // namespace

// ------------------------------------------------------------------ CapsModel

void CapsModel::Build(std::vector<CapsEntry> entries) {
  // Deduplicate identical (format, resolution, fps) rows -- some drivers list
  // the same combination several times.
  std::vector<CapsEntry> unique;
  for (CapsEntry& e : entries) {
    bool dup = false;
    for (const CapsEntry& u : unique) {
      if (u.subtypeLabel == e.subtypeLabel && u.width == e.width && u.height == e.height &&
          FpsNear(u.defaultFps, e.defaultFps)) {
        dup = true;
        break;
      }
    }
    if (!dup) unique.push_back(std::move(e));
  }
  entries_ = std::move(unique);
}

std::vector<std::string> CapsModel::Subtypes() const {
  std::vector<std::string> out;
  for (const CapsEntry& e : entries_) {
    if (std::find(out.begin(), out.end(), e.subtypeLabel) == out.end()) {
      out.push_back(e.subtypeLabel);
    }
  }
  return out;
}

std::vector<ResolutionOption> CapsModel::Resolutions(const std::string& subtype) const {
  std::vector<ResolutionOption> out;
  int minW = INT32_MAX, maxW = 0, minH = INT32_MAX, maxH = 0;
  bool any = false;
  // Only meaningful when the driver reports a size range rather than a list of
  // fixed sizes. Cards that report min == max per entry support exactly those
  // sizes, and offering anything in between would just be wrong.
  bool scalable = false;

  for (const CapsEntry& e : entries_) {
    if (e.subtypeLabel != subtype) continue;
    any = true;
    minW = std::min(minW, e.minWidth);
    maxW = std::max(maxW, e.maxWidth);
    minH = std::min(minH, e.minHeight);
    maxH = std::max(maxH, e.maxHeight);
    if (e.minWidth < e.maxWidth || e.minHeight < e.maxHeight) scalable = true;

    bool dup = false;
    for (const ResolutionOption& r : out) {
      if (r.width == e.width && r.height == e.height) { dup = true; break; }
    }
    if (!dup) out.push_back({e.width, e.height, false});
  }
  if (!any) return out;

  // Standard resolutions the driver did not list but that fit inside a range it
  // says it can scale to. Flagged so the UI can mark them as not guaranteed.
  if (scalable) {
    for (const StdRes& r : kStandardResolutions) {
      if (r.w < minW || r.w > maxW || r.h < minH || r.h > maxH) continue;
      bool dup = false;
      for (const ResolutionOption& o : out) {
        if (o.width == r.w && o.height == r.h) { dup = true; break; }
      }
      if (!dup) out.push_back({r.w, r.h, true});
    }
  }

  std::sort(out.begin(), out.end(), [](const ResolutionOption& a, const ResolutionOption& b) {
    if (a.width * a.height != b.width * b.height) {
      return a.width * a.height > b.width * b.height;
    }
    return a.width > b.width;
  });
  return out;
}

namespace {

// The union of the frame-rate ranges the driver reports for one format at one
// resolution. `borrowed` says the numbers came from a different resolution
// because none covered the one asked about -- they are then an educated guess
// rather than a promise, and the UI says so.
struct FpsRange {
  double min = 0.0;
  double max = 0.0;
  bool have = false;
  bool borrowed = false;
};

// Widens `r` to include one entry's range.
void Widen(FpsRange* r, const CapsEntry& e) {
  if (!r->have) {
    r->min = e.minFps;
    r->max = e.maxFps;
    r->have = true;
  } else {
    r->min = std::min(r->min, e.minFps);
    r->max = std::max(r->max, e.maxFps);
  }
}

// What the driver says about this format at this resolution.
FpsRange ReportedRange(const std::vector<CapsEntry>& entries, const std::string& subtype,
                       int width, int height) {
  FpsRange r;
  for (const CapsEntry& e : entries) {
    if (e.subtypeLabel != subtype) continue;
    const bool sameRes = (e.width == width && e.height == height);
    const bool coversRes = width >= e.minWidth && width <= e.maxWidth && height >= e.minHeight &&
                           height <= e.maxHeight;
    if (sameRes || coversRes) Widen(&r, e);
  }
  if (r.have) return r;

  // Nothing covers this resolution, so it is one the user forced. Borrow the
  // union of everything reported for the format: an educated guess beats an
  // empty dropdown, as long as it is labelled as one.
  for (const CapsEntry& e : entries) {
    if (e.subtypeLabel == subtype) Widen(&r, e);
  }
  r.borrowed = r.have;
  return r;
}

}  // namespace

std::vector<FpsOption> CapsModel::FpsList(const std::string& subtype, int width, int height) const {
  std::vector<FpsOption> out;
  const FpsRange range = ReportedRange(entries_, subtype, width, height);

  auto known = [&out](double fps) {
    for (const FpsOption& o : out) {
      if (FpsNear(o.fps, fps)) return true;
    }
    return false;
  };

  // Rates named outright for exactly this resolution.
  for (const CapsEntry& e : entries_) {
    if (e.subtypeLabel != subtype) continue;
    if (e.width != width || e.height != height) continue;
    if (e.defaultFps > 0.0 && !known(e.defaultFps)) out.push_back({e.defaultFps, false, false});
  }

  // Standard rates that fall inside the advertised range. These are not
  // inventions: a DirectShow range is the driver's own claim that it accepts
  // anything between the two ends, and 48 inside 25..59.94 is as much a promise
  // as 25 is. What used to sit here as well -- everything up to twice the
  // advertised maximum, on the theory that a card claiming 30 might secretly do
  // 60 -- is gone. It filled the list with rates no card had ever mentioned,
  // and on an input that reports no range at all it marked every single entry
  // from 25 to 119.88 "not reported", which is a dropdown that tells you
  // nothing. Anyone who wants to gamble on an unlisted rate can still type it
  // in by hand, and then it is their guess rather than ours.
  if (range.have) {
    for (double f : kStandardFps) {
      if (f < range.min - 0.05 || f > range.max + 0.05) continue;
      if (!known(f)) out.push_back({f, range.borrowed, false});
    }
    // The top of the range itself, when no standard rate happened to land on
    // it. A card topping out at 47.5 should still offer 47.5.
    if (range.max > 0.0 && !known(range.max)) out.push_back({range.max, range.borrowed, false});
  }

  std::sort(out.begin(), out.end(),
            [](const FpsOption& a, const FpsOption& b) { return a.fps > b.fps; });

  // Above the numbers, the entries that are not numbers. They lead because they
  // are the right answer for almost everyone: neither can go stale when the
  // console switches from 576i50 to 480p60.
  out.insert(out.begin(), FpsOption{0.0, false, true, false});
  // Und ueber der hoechsten die richtige, wo bekannt ist, welche das ist. Sie
  // steht nur da, wenn die Norm es sagt -- eine Auswahl anzubieten, die sich
  // beim Oeffnen als "geht nicht" herausstellt, waere schlechter als keine.
  if (nativeFieldRate_ > 0.0) out.insert(out.begin(), FpsOption{0.0, false, false, true});
  return out;
}

void CapsModel::SetNativeStandard(long standard) {
  nativeLines_ = VideoStandardLines(standard);
  nativeFieldRate_ = VideoStandardFieldRate(standard);
}

double CapsModel::NativeFps(const std::string& subtype, int width, int height) const {
  // Die Rate steht in der Normtabelle: 50 auf 625 Zeilen, 59,94 auf 525, 60 bei
  // PAL 60. Gefragt ist die Halbbildrate, nicht die halbe -- qBlank zeigt
  // Halbbilder einzeln, und eine Karte, die 720x576 anbietet, nennt dieselbe 50
  // dazu.
  const double want = nativeFieldRate_;
  if (want <= 0.0) return 0.0;

  // Ueber denselben Bereich wie "hoechste verfuegbare", damit beide Betriebs-
  // arten dieselbe Auskunft benutzen: der schliesst Eintraege ein, die diese
  // Groesse nur ueberdecken statt sie zu nennen, und leiht sich bei einer von
  // Hand erzwungenen Groesse die Raten des Formats. Sonst haette die Rate des
  // Signals nur dort funktioniert, wo die Karte genau diese Zeile auffuehrt --
  // also fuer 720x576 in jedem Pixelformat, aber fuer nichts Erzwungenes.
  const FpsRange range = ReportedRange(entries_, subtype, width, height);
  if (range.have && want >= range.min - 0.05 && want <= range.max + 0.05) return want;

  // Nennt die Karte sie nirgends, das Naechstgelegene von dem, was sie nennt --
  // eine, die nur 60,00 kennt, soll 60,00 bekommen und nicht 0. Erst aus den
  // Eintraegen zu dieser Groesse, und nur wenn es keine gibt, aus dem ganzen
  // Format.
  double best = 0.0;
  double bestErr = 1e9;
  auto consider = [&](double c) {
    if (c <= 0.0) return;
    const double err = std::fabs(c - want);
    if (err < bestErr) {
      bestErr = err;
      best = c;
    }
  };
  for (int pass = 0; pass < 2 && best <= 0.0; ++pass) {
    for (const CapsEntry& e : entries_) {
      if (e.subtypeLabel != subtype) continue;
      if (pass == 0) {
        const bool sameRes = (e.width == width && e.height == height);
        const bool coversRes = width >= e.minWidth && width <= e.maxWidth &&
                               height >= e.minHeight && height <= e.maxHeight;
        if (!sameRes && !coversRes) continue;
      }
      consider(e.defaultFps);
      consider(e.minFps);
      consider(e.maxFps);
    }
  }
  return best;
}

double CapsModel::HighestFps(const std::string& subtype, int width, int height) const {
  const FpsRange range = ReportedRange(entries_, subtype, width, height);
  double best = range.have ? range.max : 0.0;
  // A discrete entry can name a rate above its own range when a driver fills
  // the two fields inconsistently. Taking the larger keeps "highest" honest.
  for (const CapsEntry& e : entries_) {
    if (e.subtypeLabel != subtype) continue;
    if (e.width != width || e.height != height) continue;
    best = std::max(best, e.defaultFps);
  }
  return best;
}

bool CapsModel::IsAdvertised(const std::string& subtype, int width, int height, double fps) const {
  for (const CapsEntry& e : entries_) {
    if (e.subtypeLabel != subtype) continue;
    if (e.width != width || e.height != height) continue;
    // "Highest available" names no rate of its own, so it is advertised exactly
    // when the resolution is -- whatever comes back is the driver's own number.
    if (fps <= 0.0) return true;
    if (FpsNear(e.defaultFps, fps)) return true;
    if (fps >= e.minFps - 0.05 && fps <= e.maxFps + 0.05) return true;
  }
  return false;
}

FormatSel CapsModel::PickDefault(const std::string& preferSubtype) const {
  // Wieviele Zeilen das Bild hoechstens tragen kann. Ein 625-Zeilen-Raster hat
  // 576 sichtbare, ein 525er 480; ein paar Treiber bieten fuer 525 auch 486 an,
  // dafuer der Schlupf. Der laesst 576 unter 625 durch und 720 nirgends.
  const int activeLines = nativeLines_ >= 600 ? 576 : (nativeLines_ > 0 ? 480 : 0);

  FormatSel best;
  long long bestScore = -1;
  for (const CapsEntry& e : entries_) {
    // A named subtype beats everything below it, and everything below it still
    // decides between the entries that carry it. The area term tops out around
    // 2^33 at 4K, the line bonus sits at 2^39, the renderer bonus at 2^40 and
    // the raster bonus at 2^41, so 2^42 clears all four.
    const long long wishBonus =
        (!preferSubtype.empty() && e.subtypeLabel == preferSubtype) ? 1LL << 42 : 0;
    // Die richtige Groesse vor der bequemen. Wo das Raster bekannt ist, gewinnt
    // jeder Eintrag, der hineinpasst, gegen jeden, der darueber liegt -- auch
    // gegen einen, den der Renderer lieber haette. Ein Dekoder im Graphen
    // kostet Rechenzeit, eine hochskalierte Aufnahme kostet das Bild, und das
    // eine ist ruecknehmbar, das andere nicht. Passt gar nichts darunter,
    // bekommt jeder Eintrag dieselbe Null und es bleibt beim Groessten.
    const long long fitBonus =
        (activeLines > 0 && e.height > 0 && e.height <= activeLines + 16) ? 1LL << 41 : 0;
    // Prefer formats the renderer can take without a decoder in the graph.
    const long long formatBonus = e.rendererOk ? 1LL << 40 : 0;
    // Unter dem Raster entscheidet sonst wieder die Flaeche, und die groesste
    // ist dort 768x576 -- eine Zeile, die dieselben 720 Abtastwerte auf 768
    // breitgerechnet hat, damit die Pixel quadratisch werden. Genau das soll
    // die Aufnahme nicht: eine Norm-Zeile hat 13,5 MHz und damit 720 Werte,
    // egal ob 525 oder 625 Zeilen (704 bei den Karten, die den Rand weglassen).
    // Also der echten Zeilenlaenge den Vorzug, der Umrechnung nicht. Bietet
    // niemand eine an, bleibt es bei Null und die Flaeche entscheidet weiter.
    const long long lineBonus =
        (activeLines > 0 && e.width >= 704 && e.width <= 720) ? 1LL << 39 : 0;
    const double fps = e.maxFps > 0.0 ? e.maxFps : e.defaultFps;
    const long long score = wishBonus + fitBonus + formatBonus + lineBonus +
                            (long long)e.width * e.height * 1000 + (long long)(fps * 10);
    if (score > bestScore) {
      bestScore = score;
      best.subtype = e.subtypeLabel;
      best.width = e.width;
      best.height = e.height;
      best.fps = fps;
      best.forced = false;
    }
  }
  return best;
}

ResolutionOption CapsModel::FittingResolution(const std::string& subtype, int activeLines) const {
  ResolutionOption best;
  if (activeLines <= 0) return best;
  long long bestScore = -1;
  for (const CapsEntry& e : entries_) {
    if (e.subtypeLabel != subtype) continue;
    // Derselbe Schlupf wie in PickDefault: 486 unter 525 zaehlt mit.
    if (e.height < activeLines || e.height > activeLines + 16) continue;
    const long long lineBonus = (e.width >= 704 && e.width <= 720) ? 1LL << 40 : 0;
    const long long score = lineBonus + (long long)e.width * e.height;
    if (score > bestScore) {
      bestScore = score;
      best.width = e.width;
      best.height = e.height;
    }
  }
  return best;
}

}  // namespace cap
