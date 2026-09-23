#include "config.h"

#include <algorithm>
#include <cmath>

#include "app_files.h"
#include "files.h"
#include "json.h"
#include "platform.h"

namespace cap {

namespace {

// Clamps an index into range so a stale config value cannot read past an array.
int Pick(int index, int count) {
  return (index < 0 || index >= count) ? 0 : index;
}

}  // namespace

// ------------------------------------------------------------------ UI labels

const char* ThemeName(int i) {
  static const char* de[3] = {"Dunkel", "Hell", "Wie Windows"};
  static const char* en[3] = {"Dark", "Light", "Follow Windows"};
  i = Pick(i, 3);
  return T(de[i], en[i]);
}

const char* LanguageName(int i) {
  static const char* names[2] = {"Deutsch", "English"};
  return names[Pick(i, 2)];
}

const char* ScaleFilterName(int i) {
  static const char* names[5] = {"Nearest Neighbor", "Bilinear", "Bicubic", "Lanczos3",
                                 "Sharp Bilinear"};
  return names[Pick(i, 5)];
}

const char* DeinterlaceName(int i) {
  static const char* de[kDeinterlaceCount] = {"Aus (Weave)",      "Bob",
                                              "Bob (interpoliert)", "Bewegungsadaptiv",
                                              "Kantenorientiert",   "YADIF"};
  static const char* en[kDeinterlaceCount] = {"Off (weave)",       "Bob",
                                              "Bob (interpolated)", "Motion adaptive",
                                              "Edge directed",      "YADIF"};
  i = Pick(i, kDeinterlaceCount);
  return T(de[i], en[i]);
}

const char* FieldOrderName(int i) {
  static const char* de[3] = {"Automatisch", "Oberes Halbbild zuerst", "Unteres Halbbild zuerst"};
  static const char* en[3] = {"Automatic", "Top field first", "Bottom field first"};
  i = Pick(i, 3);
  return T(de[i], en[i]);
}

const char* RotationName(int i) {
  static const char* de[kRotationCount] = {"Keine", "90 Grad rechts", "180 Grad",
                                           "90 Grad links"};
  static const char* en[kRotationCount] = {"None", "90 degrees right", "180 degrees",
                                           "90 degrees left"};
  i = Pick(i, kRotationCount);
  return T(de[i], en[i]);
}

const char* SignalKindName(int i) {
  static const char* de[kSignalKindCount] = {"Automatisch", "Analog", "Digital"};
  static const char* en[kSignalKindCount] = {"Automatic", "Analogue", "Digital"};
  i = Pick(i, kSignalKindCount);
  return T(de[i], en[i]);
}

const char* AnalogConnectorName(int i) {
  // Der landlaeufige Name zuerst, die Abkuerzung dahinter: wer "FBAS" auf einem
  // alten Geraet gelesen hat, findet sich sonst nicht wieder, und wer nur "der
  // gelbe Stecker" kennt, braucht die Beschreibung darunter ohnehin.
  static const char* de[kAnalogConnectorCount] = {"Automatisch", "Composite (FBAS)", "S-Video",
                                                  "Component / RGB"};
  static const char* en[kAnalogConnectorCount] = {"Automatic", "Composite", "S-Video",
                                                  "Component / RGB"};
  i = Pick(i, kAnalogConnectorCount);
  return T(de[i], en[i]);
}

// Woran der Stecker zu erkennen ist, in einem Satz.
//
// Das steht hier und nicht im Hilfetext, weil es die Auswahl selbst ist, die
// die Frage aufwirft: wer nicht weiss, was er angesteckt hat, kann den Namen
// nicht waehlen, und ein Hilfezeichen daneben liest er erst, wenn er schon
// geraten hat. Beschrieben wird deshalb der Stecker in der Hand, nicht das
// Signal darin.
const char* AnalogConnectorLook(int i) {
  static const char* de[kAnalogConnectorCount] = {
      "",
      "Ein einzelner gelber Cinch-Stecker, meist neben Rot und Weiß für den Ton.",
      "Runder schwarzer Stecker mit vier Stiften, etwa fingerdick — wie ein alter "
      "PS/2-Mausstecker.",
      "Drei Cinch-Stecker in Grün, Blau und Rot, oft mit Y, Pb, Pr beschriftet — dazu Rot "
      "und Weiß für den Ton, also fünf. Der blaue VGA-Stecker gehört auch hierher."};
  static const char* en[kAnalogConnectorCount] = {
      "",
      "A single yellow RCA plug, usually beside a red and a white one for audio.",
      "A round black plug with four pins, about as thick as a finger — like an old PS/2 "
      "mouse plug.",
      "Three RCA plugs in green, blue and red, often labelled Y, Pb, Pr — plus red and "
      "white for audio, so five in all. The blue VGA plug belongs here too."};
  i = Pick(i, kAnalogConnectorCount);
  return T(de[i], en[i]);
}

// Die zusammengelegte Liste. Vier analoge Eintraege plus Digital, in der
// Reihenfolge, in der jemand sie sucht: erst "entscheide du", dann das Kabel.
const char* SignalInputName(int i) {
  static const char* de[kSignalInputCount] = {"Automatisch", "Composite (FBAS)", "S-Video",
                                              "Component / RGB", "Digital (HDMI, DVI, SDI)"};
  static const char* en[kSignalInputCount] = {"Automatic", "Composite", "S-Video",
                                              "Component / RGB", "Digital (HDMI, DVI, SDI)"};
  i = Pick(i, kSignalInputCount);
  return T(de[i], en[i]);
}

const char* SignalInputLook(int i) {
  // Automatisch beschreibt keinen Stecker, sondern was ohne Antwort geschieht;
  // Digital beschreibt einen, der keine Wahl mehr laesst.
  static const char* de[kSignalInputCount] = {
      "Analog, sobald die Karte einen Analogdecoder hat. Welches Kabel, kommt aus dem "
      "Eingangswähler der Karte — und wo es den nicht gibt, wird Composite angenommen.",
      "Ein einzelner gelber Cinch-Stecker, meist neben Rot und Weiß für den Ton.",
      "Runder schwarzer Stecker mit vier Stiften, etwa fingerdick — wie ein alter "
      "PS/2-Mausstecker.",
      "Drei Cinch-Stecker in Grün, Blau und Rot, oft mit Y, Pb, Pr beschriftet — dazu Rot "
      "und Weiß für den Ton, also fünf. Der blaue VGA-Stecker gehört auch hierher.",
      "Ein einzelner digitaler Stecker: HDMI, DVI oder SDI. Kein Farbträger, keine "
      "Videonorm, keine Analogfilter."};
  static const char* en[kSignalInputCount] = {
      "Analogue as soon as the card has an analogue decoder. Which cable comes from the "
      "card's input selector — and where there is none, composite is assumed.",
      "A single yellow RCA plug, usually beside a red and a white one for audio.",
      "A round black plug with four pins, about as thick as a finger — like an old PS/2 "
      "mouse plug.",
      "Three RCA plugs in green, blue and red, often labelled Y, Pb, Pr — plus red and "
      "white for audio, so five in all. The blue VGA plug belongs here too.",
      "A single digital plug: HDMI, DVI or SDI. No colour subcarrier, no video standard, no "
      "analogue filters."};
  i = Pick(i, kSignalInputCount);
  return T(de[i], en[i]);
}

int SignalInputIndex(SignalKind kind, AnalogConnector connector) {
  if (kind == SignalKind::Digital) return 4;
  switch (connector) {
    case AnalogConnector::Composite:
      return 1;
    case AnalogConnector::SVideo:
      return 2;
    case AnalogConnector::Component:
      return 3;
    default:
      break;
  }
  // Bleibt: kein Kabel genannt. Das ist Automatisch -- auch dann, wenn daneben
  // ausdruecklich "Analog" steht, wie in jedem Profil von vor der
  // Zusammenlegung. Die beiden sind dort dasselbe: Automatisch heisst analog,
  // sobald die Karte einen Decoder hat, und ohne Decoder gibt es nichts zu
  // erzwingen. Der Eintrag verschwindet damit, ohne dass eine Einstellung
  // umkippt.
  return 0;
}

void SignalInputApply(int index, SignalKind* kind, AnalogConnector* connector) {
  index = Pick(index, kSignalInputCount);
  if (index == 4) {
    *kind = SignalKind::Digital;
    // Das Kabel bleibt stehen, statt zurueckgesetzt zu werden: wer kurz auf
    // Digital schaut und zurueckgeht, findet seine Antwort wieder.
    return;
  }
  *kind = index == 0 ? SignalKind::Auto : SignalKind::Analog;
  static const AnalogConnector map[4] = {AnalogConnector::Auto, AnalogConnector::Composite,
                                         AnalogConnector::SVideo, AnalogConnector::Component};
  *connector = map[index];
}

const char* VideoRegionName(int i) {
  // Die Norm steht in Klammern dahinter, weil das die Angabe ist, die der Code
  // verwendet -- wer weiss, was er hat, findet sich daran wieder; wer es nicht
  // weiss, sucht sein Land und muss es nicht wissen.
  static const char* de[kVideoRegionCount] = {
      "Automatisch (Windows)",         "Keine (Reihenfolge nach Häufigkeit)",
      "Europa, Australien, Asien (PAL)", "Nordamerika (NTSC)",
      "Japan (NTSC)",                  "Frankreich, Osteuropa (SECAM)",
      "Brasilien (PAL M)",             "Argentinien, Uruguay (PAL N)"};
  static const char* en[kVideoRegionCount] = {
      "Automatic (Windows)",           "None (order by prevalence)",
      "Europe, Australia, Asia (PAL)", "North America (NTSC)",
      "Japan (NTSC)",                  "France, Eastern Europe (SECAM)",
      "Brazil (PAL M)",                "Argentina, Uruguay (PAL N)"};
  i = Pick(i, kVideoRegionCount);
  return T(de[i], en[i]);
}

VideoRegion ResolveVideoRegion(VideoRegion region) {
  if (region != VideoRegion::Auto) return region;

  // Das System kennt das Land des Nutzers, und es ist dieselbe Angabe, nach der
  // hier sonst gefragt wuerde. Danach zu fragen, was das System schon weiss,
  // ist eine Frage zu viel -- die Auswahl bleibt daneben stehen, fuer alle, bei
  // denen die Annahme nicht stimmt.
  //
  // Gefragt wird nach dem *Wohnort*, nicht nach der Sprache: zwei
  // Einstellungen, die auseinandergehen, sobald jemand sein System auf Englisch
  // stellt und in Deutschland wohnen bleibt.
  const std::string country = UserCountryCode();
  if (country.empty()) return VideoRegion::PalEurope;

  struct Entry {
    const char* code;
    VideoRegion region;
  };
  // Nur die Ausnahmen, alles andere ist PAL. Das ist keine Nachlaessigkeit,
  // sondern das Kraefteverhaeltnis: PAL war der Normalfall auf drei
  // Kontinenten, und eine Liste, die jedes PAL-Land einzeln auffuehrt, hat
  // vierzig Zeilen mehr und sagt dasselbe.
  //
  // Die Zuordnung meint das analoge Fernsehen des Landes, auch wo es
  // abgeschaltet ist -- die Geraete, die hier angeschlossen werden, stammen aus
  // genau der Zeit.
  static const Entry kByCountry[] = {
      // NTSC M, 525/60 mit 3,58 MHz.
      {"US", VideoRegion::NtscAmerica}, {"CA", VideoRegion::NtscAmerica},
      {"MX", VideoRegion::NtscAmerica}, {"GT", VideoRegion::NtscAmerica},
      {"BZ", VideoRegion::NtscAmerica}, {"SV", VideoRegion::NtscAmerica},
      {"HN", VideoRegion::NtscAmerica}, {"NI", VideoRegion::NtscAmerica},
      {"CR", VideoRegion::NtscAmerica}, {"PA", VideoRegion::NtscAmerica},
      {"CU", VideoRegion::NtscAmerica}, {"DO", VideoRegion::NtscAmerica},
      {"PR", VideoRegion::NtscAmerica}, {"JM", VideoRegion::NtscAmerica},
      {"HT", VideoRegion::NtscAmerica}, {"TT", VideoRegion::NtscAmerica},
      {"BS", VideoRegion::NtscAmerica}, {"BB", VideoRegion::NtscAmerica},
      {"CO", VideoRegion::NtscAmerica}, {"VE", VideoRegion::NtscAmerica},
      {"EC", VideoRegion::NtscAmerica}, {"PE", VideoRegion::NtscAmerica},
      {"BO", VideoRegion::NtscAmerica}, {"CL", VideoRegion::NtscAmerica},
      {"SR", VideoRegion::NtscAmerica}, {"PH", VideoRegion::NtscAmerica},
      {"KR", VideoRegion::NtscAmerica}, {"TW", VideoRegion::NtscAmerica},
      {"MM", VideoRegion::NtscAmerica},
      // Dasselbe, aber ohne den 7,5-IRE-Sockel unter Schwarz.
      {"JP", VideoRegion::NtscJapan},
      // PAL M und PAL N -- 525/60 und 625/50, beide mit dem NTSC-Farbtraeger.
      {"BR", VideoRegion::PalBrazil},
      {"AR", VideoRegion::PalArgentina}, {"UY", VideoRegion::PalArgentina},
      {"PY", VideoRegion::PalArgentina},
      // SECAM. Osteuropa sendet laengst in PAL oder digital, aber die
      // Videorecorder und Konsolen aus der SECAM-Zeit stehen noch da.
      {"FR", VideoRegion::Secam},       {"RU", VideoRegion::Secam},
      {"UA", VideoRegion::Secam},       {"BY", VideoRegion::Secam},
      {"KZ", VideoRegion::Secam},       {"MD", VideoRegion::Secam},
      {"AM", VideoRegion::Secam},       {"AZ", VideoRegion::Secam},
      {"GE", VideoRegion::Secam},       {"MN", VideoRegion::Secam},
  };

  for (const Entry& e : kByCountry) {
    if (country == e.code) return e.region;
  }
  return VideoRegion::PalEurope;
}

const char* AspectName(int i) {
  static const char* de[kAspectModeCount] = {"Quelle",   "16:9 erzwingen",
                                             "4:3 erzwingen", "Strecken",
                                             "Integer-Skalierung", "Quadratische Pixel"};
  static const char* en[kAspectModeCount] = {"Source",          "Force 16:9",
                                             "Force 4:3",       "Stretch",
                                             "Integer scaling", "Square pixels"};
  i = Pick(i, kAspectModeCount);
  return T(de[i], en[i]);
}

const char* ColorRangeName(int i) {
  static const char* de[3] = {"Automatisch", "Limited (16-235)", "Full (0-255)"};
  static const char* en[3] = {"Automatic", "Limited (16-235)", "Full (0-255)"};
  i = Pick(i, 3);
  return T(de[i], en[i]);
}

const char* ColorMatrixName(int i) {
  static const char* de[3] = {"Automatisch", "BT.601 (SD)", "BT.709 (HD)"};
  static const char* en[3] = {"Automatic", "BT.601 (SD)", "BT.709 (HD)"};
  i = Pick(i, 3);
  return T(de[i], en[i]);
}

const char* StatsDetailName(int i) {
  static const char* de[3] = {"Kompakt", "Normal", "Vollständig"};
  static const char* en[3] = {"Compact", "Normal", "Full"};
  i = Pick(i, 3);
  return T(de[i], en[i]);
}

const char* MicTrackModeName(int i) {
  static const char* de[3] = {"Gemischt und getrennt", "Nur gemischt", "Nur getrennt"};
  static const char* en[3] = {"Mixed and separate", "Mixed only", "Separate only"};
  i = Pick(i, 3);
  return T(de[i], en[i]);
}

const char* RecordContainerName(int i) {
  static const char* names[2] = {"MKV (Matroska)", "MP4"};
  return names[Pick(i, 2)];
}

const char* ScreenshotFormatName(int i) {
  static const char* names[2] = {"PNG", "JPEG"};
  return names[Pick(i, 2)];
}

const char* RecordEncoderName(int i) {
  // Only the first entry differs between languages; the rest are product names.
  static const char* names[kRecordEncoderCount] = {
      "",
      "H.264 (CPU, x264)",
      "H.264 (NVIDIA NVENC)",
      "H.264 (Intel QuickSync / Arc)",
      "H.264 (AMD AMF)",
      "H.265 (CPU, x265)",
      "H.265 (NVIDIA NVENC)",
      "AV1 (NVIDIA NVENC, RTX 40+)",
      "AV1 (Intel Arc)",
      "AV1 (AMD RDNA 3+)",
      "",
  };
  i = Pick(i, kRecordEncoderCount);
  if (i == (int)RecordEncoder::Auto) {
    return T("Automatisch — überall abspielbar", "Automatic — plays anywhere");
  }
  if (i == (int)RecordEncoder::AutoEfficient) {
    return T("Automatisch — kleinere Dateien", "Automatic — smaller files");
  }
  return names[i];
}

const char* RecordSpeedName(int i) {
  // x264 preset names, left in English because that is what they are called.
  static const char* names[5] = {"ultrafast", "veryfast", "faster", "fast", "medium"};
  return names[Pick(i, 5)];
}

const char* OsdCornerName(int i) {
  static const char* de[4] = {"Oben links", "Oben rechts", "Unten links", "Unten rechts"};
  static const char* en[4] = {"Top left", "Top right", "Bottom left", "Bottom right"};
  i = Pick(i, 4);
  return T(de[i], en[i]);
}

// Kept to a line or two on purpose: the label already says what it is, the
// tooltip only has to say when you would want it.
const char* ScaleFilterHelp(int i) {
  static const char* de[5] = {
      "Harte Pixelkanten. Retro-Konsolen, zusammen mit Integer-Skalierung.",
      "Weich und neutral. Standard für moderne Quellen.",
      "Catmull-Rom. Schärfer als bilinear, leichte Überschwinger an Kanten.",
      "Schärfste Wahl. Gut für HD, erzeugt bei Pixelgrafik Halos.",
      "Wie Nearest, aber ohne ungleich breite Pixel bei krummen Faktoren. Mit "
      "Quelle die Integer-Optik, bis zum Fensterrand.",
  };
  static const char* en[5] = {
      "Hard pixel edges. For retro consoles, paired with integer scaling.",
      "Soft and neutral. The default for modern sources.",
      "Catmull-Rom. Sharper than bilinear, slight overshoot at edges.",
      "Sharpest option. Good for HD, produces halos on pixel art.",
      "Like nearest, but without unevenly wide pixels at odd scale factors. "
      "With Source, the integer look all the way to the window edge.",
  };
  i = Pick(i, 5);
  return T(de[i], en[i]);
}

const char* DeinterlaceHelp(int i) {
  static const char* de[kDeinterlaceCount] = {
      "Halbbilder bleiben verwoben. Bei Bewegung Kammartefakte.",
      "Volle Bildrate, keine Latenz. Zeigt je ein Halbbild doppelt, wodurch die Zeilenpaare im Takt der Halbbilder um eine Zeile wandern. Für 240p/288p ideal, für echtes Interlacing die unruhigste Wahl.",
      "Wie Bob, fehlende Zeilen interpoliert. Steht deutlich ruhiger als Bob, weil die Zeilen an ihrer wahren Position landen.",
      "Ruhige Bildteile behalten die volle Zeilenzahl, bewegte werden interpoliert. "
      "Keine Latenz.",
      "Für Pixelgrafik gedacht: interpoliert entlang der Kantenrichtung, glatte "
      "Diagonalen, keine Latenz. Auf Composite-Signalen schlechter als die anderen.",
      "Beste Qualität. Vergleicht mit dem vorherigen Bild, braucht ein Bild Speicher "
      "und etwas mehr GPU-Last.",
  };
  static const char* en[kDeinterlaceCount] = {
      "Fields stay woven. Combing artefacts on motion.",
      "Full frame rate, no added latency. Shows one field twice over, so the line pairs move by one line at field rate. Ideal for 240p/288p, the least steady choice for genuine interlacing.",
      "Like bob with interpolated lines. Considerably steadier than bob, because each line lands where it belongs.",
      "Still parts of the picture keep every line, moving parts are interpolated. "
      "No added latency.",
      "Meant for pixel art: interpolates along the direction of edges, smooth diagonals, "
      "no added latency. Worse than the others on a composite signal.",
      "Best quality. Compares against the previous frame, so it keeps one frame in "
      "memory and costs a little more GPU time.",
  };
  i = Pick(i, kDeinterlaceCount);
  return T(de[i], en[i]);
}

const char* MaskName(int i) {
  static const char* de[3] = {"Aus", "Streifenmaske", "Lochmaske"};
  static const char* en[3] = {"Off", "Aperture grille", "Shadow mask"};
  i = Pick(i, 3);
  return T(de[i], en[i]);
}

const char* MaskHelp(int i) {
  static const char* de[3] = {
      "Keine Maske.",
      "Senkrechte Streifen, wie bei einer Trinitron-Röhre.",
      "Versetzte Tripel, wie bei einer gewöhnlichen Lochmaskenröhre.",
  };
  static const char* en[3] = {
      "No mask.",
      "Vertical stripes, the way a Trinitron tube worked.",
      "Staggered triads, the way an ordinary shadow mask tube worked.",
  };
  i = Pick(i, 3);
  return T(de[i], en[i]);
}

const char* AspectHelp(int i) {
  static const char* de[kAspectModeCount] = {
      "So wie die Karte es liefert.",
      "Unabhängig von der Auflösung der Quelle.",
      "Für alte Konsolen, die 4:3 in einem 16:9-Signal liefern.",
      "Füllt das Fenster, verzerrt das Bild.",
      "Ganzzahliger Faktor auf die Zeilen, Breite aus dem Seitenverhältnis. "
      "Alle Zeilen gleich hoch, dafür bleibt Rand. Randfüllend: Quelle mit "
      "Sharp-Bilinear.",
      "Jeder Konsolenpixel so breit wie hoch, aus der Breite der Quelle weiter "
      "unten. Ohne Angabe zählen die Proben der Karte.",
  };
  static const char* en[kAspectModeCount] = {
      "Whatever the card reports.",
      "Regardless of the source resolution.",
      "For old consoles that put 4:3 inside a 16:9 signal.",
      "Fills the window, distorts the picture.",
      "Whole-number factor on the lines, width from the aspect. Every line the "
      "same height, at the cost of a border. Edge to edge: Source with "
      "sharp-bilinear.",
      "Every console pixel as wide as it is tall, from the source width below. "
      "With none given, the card's samples count instead.",
  };
  i = Pick(i, kAspectModeCount);
  return T(de[i], en[i]);
}

// ------------------------------------------------------------ accent presets

namespace {

struct AccentEntry {
  unsigned rgb;
  const char* de;
  const char* en;
};

const AccentEntry kAccents[] = {
    {0x8B5CF6, "Violett", "Violet"},   {0xA855F7, "Purpur", "Purple"},
    {0xEC4899, "Pink", "Pink"},        {0xEF4444, "Rot", "Red"},
    {0xF97316, "Orange", "Orange"},    {0xF59E0B, "Bernstein", "Amber"},
    {0x22C55E, "Grün", "Green"},       {0x14B8A6, "Türkis", "Teal"},
    {0x06B6D4, "Cyan", "Cyan"},        {0x4C8DFF, "Blau", "Blue"},
    {0x6366F1, "Indigo", "Indigo"},    {0x94A3B8, "Grau", "Grey"},
};

}  // namespace

int AccentPresetCount() {
  return (int)(sizeof(kAccents) / sizeof(kAccents[0]));
}

unsigned AccentPresetColor(int index) {
  return kAccents[Pick(index, AccentPresetCount())].rgb;
}

const char* AccentPresetName(int index) {
  const AccentEntry& e = kAccents[Pick(index, AccentPresetCount())];
  return T(e.de, e.en);
}

namespace {

// --------------------------------------------------------------- enum helpers

template <typename E>
E EnumFromInt(int v, int count, E fallback) {
  if (v < 0 || v >= count) return fallback;
  return (E)v;
}

template <typename E>
E ReadEnum(const json::Value& v, const char* key, int count, E fallback) {
  if (!v.Has(key)) return fallback;
  return EnumFromInt<E>(v[key].AsInt((int)fallback), count, fallback);
}

// ---------------------------------------------------------- struct <-> json

json::Value WriteDevice(const DeviceRef& d) {
  json::Value o = json::Value::Object();
  o["name"] = d.name;
  o["id"] = d.id;
  if (!d.backend.empty()) o["backend"] = d.backend;
  return o;
}

DeviceRef ReadDevice(const json::Value& v) {
  DeviceRef d;
  d.name = v["name"].AsString();
  d.id = v["id"].AsString();
  d.backend = v["backend"].AsString();
  return d;
}

json::Value WriteFormat(const FormatSel& f) {
  json::Value o = json::Value::Object();
  o["subtype"] = f.subtype;
  o["width"] = f.width;
  o["height"] = f.height;
  o["fps"] = f.fps;
  o["forced"] = f.forced;
  return o;
}

FormatSel ReadFormat(const json::Value& v) {
  FormatSel f;
  f.subtype = v["subtype"].AsString();
  f.width = v["width"].AsInt(0);
  f.height = v["height"].AsInt(0);
  f.fps = v["fps"].AsNumber(kFpsNative);
  f.forced = v["forced"].AsBool(false);
  return f;
}

json::Value WriteProfile(const Profile& p) {
  json::Value o = json::Value::Object();
  o["name"] = p.name;
  o["autoSelectStandard"] = (int)p.autoSelectStandard;

  json::Value cap = json::Value::Object();
  cap["video"] = WriteDevice(p.capture.video);
  cap["audioSource"] = (int)p.capture.audioSource;
  cap["audio"] = WriteDevice(p.capture.audio);
  cap["crossbarInput"] = p.capture.crossbarInput;
  cap["videoStandard"] = (int)p.capture.videoStandard;
  cap["signalKind"] = (int)p.capture.signalKind;
  cap["connector"] = (int)p.capture.connector;
  cap["format"] = WriteFormat(p.capture.format);
  o["capture"] = cap;

  json::Value img = json::Value::Object();
  img["filter"] = (int)p.image.filter;
  img["sharpen"] = p.image.sharpen;
  img["brightness"] = p.image.brightness;
  img["contrast"] = p.image.contrast;
  img["saturation"] = p.image.saturation;
  img["hue"] = p.image.hue;
  img["procAmpToOutput"] = p.image.procAmpToOutput;
  img["nativeWidth"] = p.image.nativeWidth;
  img["sourceLines"] = p.image.sourceLines;
  img["scanlines"] = p.image.scanlines;
  img["mask"] = p.image.mask;
  img["maskStrength"] = p.image.maskStrength;
  img["deinterlace"] = (int)p.image.deinterlace;
  img["deinterlaceAuto"] = p.image.deinterlaceAuto;
  img["fieldOrder"] = (int)p.image.fieldOrder;
  img["lineDouble"] = p.image.lineDouble;
  img["rotation"] = (int)p.image.rotation;
  img["chromaSoft"] = p.image.chromaSoft;
  img["temporalDenoise"] = p.image.temporalDenoise;
  img["avoidGhosting"] = p.image.avoidGhosting;
  img["dotNotch"] = p.image.dotNotch;
  img["motionCompensate"] = p.image.motionCompensate;
  img["bandwidthRestore"] = p.image.bandwidthRestore;
  img["adaptiveChroma"] = p.image.adaptiveChroma;
  img["compareSplit"] = p.image.compareSplit;
  img["compareHorizontal"] = p.image.compareHorizontal;
  img["aspect"] = (int)p.image.aspect;
  img["squarePixelOutput"] = p.image.squarePixelOutput;
  img["cropLeft"] = p.image.cropLeft;
  img["cropRight"] = p.image.cropRight;
  img["cropTop"] = p.image.cropTop;
  img["cropBottom"] = p.image.cropBottom;
  img["cropPerFormat"] = p.image.cropPerFormat;
  // Die abgelegten Zuschnitte nur, wenn es welche gibt: eine leere Liste in
  // jedem Profil waere Rauschen in einer Datei, die man von Hand lesen koennen
  // soll.
  if (!p.image.cropVariants.empty()) {
    json::Value list = json::Value::Array();
    for (const CropForFormat& v : p.image.cropVariants) {
      json::Value e = json::Value::Object();
      e["width"] = v.width;
      e["height"] = v.height;
      e["left"] = v.left;
      e["right"] = v.right;
      e["top"] = v.top;
      e["bottom"] = v.bottom;
      list.Push(e);
    }
    img["cropVariants"] = list;
  }
  img["range"] = (int)p.image.range;
  img["matrix"] = (int)p.image.matrix;
  o["image"] = img;

  json::Value aud = json::Value::Object();
  aud["output"] = WriteDevice(p.audio.output);
  aud["bufferMs"] = p.audio.bufferMs;
  aud["exclusive"] = p.audio.exclusive;
  aud["avOffsetMs"] = p.audio.avOffsetMs;
  aud["volume"] = p.audio.volume;
  aud["mute"] = p.audio.mute;
  aud["micEnabled"] = p.audio.micEnabled;
  aud["micDevice"] = WriteDevice(p.audio.micDevice);
  aud["micGain"] = p.audio.micGain;
  aud["micTrackMode"] = (int)p.audio.micTrackMode;
  o["audio"] = aud;

  return o;
}

Profile ReadProfile(const json::Value& v) {
  Profile p;
  p.name = v["name"].AsString("Standard");
  p.autoSelectStandard = (long)v["autoSelectStandard"].AsInt(0);

  const json::Value& c = v["capture"];
  p.capture.video = ReadDevice(c["video"]);
  p.capture.audioSource = ReadEnum<AudioSource>(c, "audioSource", 3, AudioSource::Embedded);
  p.capture.audio = ReadDevice(c["audio"]);
  p.capture.crossbarInput = c["crossbarInput"].AsInt(-1);
  p.capture.videoStandard = (long)c["videoStandard"].AsInt(0);
  p.capture.signalKind = ReadEnum<SignalKind>(c, "signalKind", kSignalKindCount, SignalKind::Auto);
  // Neu in 3.5. Ein Profil ohne den Schluessel bekommt Auto und verhaelt sich
  // damit wie vorher -- ein eigener Schluessel statt zusaetzlicher Werte in
  // signalKind ist genau deshalb gewaehlt: dort haetten sie die gespeicherten
  // Zahlen verschoben und aus "Digital" stillschweigend "S-Video" gemacht.
  p.capture.connector =
      ReadEnum<AnalogConnector>(c, "connector", kAnalogConnectorCount, AnalogConnector::Auto);
  p.capture.format = ReadFormat(c["format"]);

  const json::Value& i = v["image"];
  p.image.filter = ReadEnum<ScaleFilter>(i, "filter", 5, ScaleFilter::Bilinear);
  p.image.sharpen = (float)Clamp(i["sharpen"].AsNumber(0.0), 0.0, 1.0);
  p.image.brightness = (float)Clamp(i["brightness"].AsNumber(0.0), -1.0, 1.0);
  p.image.contrast = (float)Clamp(i["contrast"].AsNumber(1.0), 0.0, 2.0);
  p.image.saturation = (float)Clamp(i["saturation"].AsNumber(1.0), 0.0, 2.0);
  p.image.hue = (float)Clamp(i["hue"].AsNumber(0.0), -180.0, 180.0);
  p.image.procAmpToOutput = i["procAmpToOutput"].AsBool(false);
  // 16384 rather than a round number: it is the longest texture edge D3D11
  // can address, so nothing this program could draw fits above it anyway.
  p.image.nativeWidth = Clamp(i["nativeWidth"].AsInt(0), 0, 16384);
  p.image.sourceLines = Clamp(i["sourceLines"].AsInt(0), 0, 16384);
  p.image.scanlines = (float)Clamp(i["scanlines"].AsNumber(0.0), 0.0, 0.5);
  p.image.mask = Clamp(i["mask"].AsInt(0), 0, 2);
  p.image.maskStrength = (float)Clamp(i["maskStrength"].AsNumber(0.35), 0.0, 0.5);
  p.image.deinterlace =
      ReadEnum<Deinterlace>(i, "deinterlace", kDeinterlaceCount, Deinterlace::Bob);
  p.image.deinterlaceAuto = i["deinterlaceAuto"].AsBool(true);
  p.image.fieldOrder = ReadEnum<FieldOrder>(i, "fieldOrder", 3, FieldOrder::Auto);
  p.image.lineDouble = i["lineDouble"].AsBool(false);
  p.image.rotation = ReadEnum<Rotation>(i, "rotation", kRotationCount, Rotation::None);
  p.image.chromaSoft = i["chromaSoft"].AsInt(0);
  p.image.temporalDenoise = (float)i["temporalDenoise"].AsNumber(0.0);
  p.image.avoidGhosting = i["avoidGhosting"].AsBool(false);
  p.image.dotNotch = (float)i["dotNotch"].AsNumber(0.0);
  p.image.motionCompensate = i["motionCompensate"].AsBool(false);
  p.image.bandwidthRestore = (float)Clamp(i["bandwidthRestore"].AsNumber(0.0), 0.0, 1.0);
  p.image.adaptiveChroma = i["adaptiveChroma"].AsBool(false);
  p.image.compareSplit = (float)Clamp(i["compareSplit"].AsNumber(0.5), 0.0, 1.0);
  p.image.compareHorizontal = i["compareHorizontal"].AsBool(false);
  p.image.aspect = ReadEnum<AspectMode>(i, "aspect", kAspectModeCount, AspectMode::Source);
  p.image.squarePixelOutput = i["squarePixelOutput"].AsBool(true);
  p.image.cropLeft = Clamp(i["cropLeft"].AsInt(0), 0, 16384);
  p.image.cropRight = Clamp(i["cropRight"].AsInt(0), 0, 16384);
  p.image.cropTop = Clamp(i["cropTop"].AsInt(0), 0, 16384);
  p.image.cropBottom = Clamp(i["cropBottom"].AsInt(0), 0, 16384);
  p.image.cropPerFormat = i["cropPerFormat"].AsBool(false);
  {
    const json::Value& list = i["cropVariants"];
    for (size_t n = 0; n < list.Size(); ++n) {
      const json::Value& e = list.At(n);
      CropForFormat cv;
      cv.width = Clamp(e["width"].AsInt(0), 0, 16384);
      cv.height = Clamp(e["height"].AsInt(0), 0, 16384);
      // Ein Eintrag ohne Groesse hat keinen Schluessel und passt deshalb nie.
      if (cv.width <= 0 || cv.height <= 0) continue;
      cv.left = Clamp(e["left"].AsInt(0), 0, 16384);
      cv.right = Clamp(e["right"].AsInt(0), 0, 16384);
      cv.top = Clamp(e["top"].AsInt(0), 0, 16384);
      cv.bottom = Clamp(e["bottom"].AsInt(0), 0, 16384);
      p.image.cropVariants.push_back(cv);
    }
  }
  p.image.range = ReadEnum<ColorRange>(i, "range", 3, ColorRange::Auto);
  p.image.matrix = ReadEnum<ColorMatrix>(i, "matrix", 3, ColorMatrix::Auto);

  const json::Value& a = v["audio"];
  p.audio.output = ReadDevice(a["output"]);
  p.audio.bufferMs = Clamp(a["bufferMs"].AsInt(30), 2, 500);
  p.audio.exclusive = a["exclusive"].AsBool(false);
  p.audio.avOffsetMs = Clamp(a["avOffsetMs"].AsInt(0), -500, 500);
  p.audio.volume = (float)Clamp(a["volume"].AsNumber(1.0), 0.0, 1.0);
  p.audio.mute = a["mute"].AsBool(false);
  p.audio.micEnabled = a["micEnabled"].AsBool(false);
  p.audio.micDevice = ReadDevice(a["micDevice"]);
  p.audio.micGain = (float)Clamp(a["micGain"].AsNumber(1.0), 0.0, 4.0);
  p.audio.micTrackMode = ReadEnum<MicTrackMode>(a, "micTrackMode", 3, MicTrackMode::Both);

  return p;
}

// A settings file is a few kilobytes. Sixteen megabytes is already absurd, and
// something that size next to the executable is not a settings file.
const uint64_t kJsonLimit = 16 * 1024 * 1024;

bool ReadJsonFile(const std::filesystem::path& path, std::string& out) {
  if (!ReadWholeFile(path, &out, kJsonLimit)) return false;
  // Strip a UTF-8 BOM if an editor added one.
  if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB &&
      (unsigned char)out[2] == 0xBF) {
    out.erase(0, 3);
  }
  return true;
}

// True when this document is one of ours, whatever the file is called.
//
// Recognising it by name would mean keeping a list of every name the program
// has ever had, in a place that has to be right years later. Recognising it by
// content does not: the file says what it is. Newer ones say it outright, in
// "program"; older ones -- everything written before the program started
// stamping its name in -- say it by shape, and the shape is specific enough. A
// version number plus these two objects is not something a stray .json from
// another program happens to have.
bool LooksLikeSettings(const json::Value& root, ForeignSettings* out) {
  if (!root.IsObject() || !root["version"].IsNumber()) return false;
  if (!root["app"].IsObject() || !root["record"].IsObject()) return false;
  if (out) {
    out->program = root["program"].AsString();
    const json::Value& language = root["app"]["language"];
    out->language = language.IsNumber() ? language.AsInt() : -1;
  }
  return true;
}

}  // namespace

// ------------------------------------------------------------------ FormatSel

std::string FormatSel::Label() const {
  if (!valid()) return T("(nicht gesetzt)", "(not set)");
  std::string fpsText;
  if (fps > 0.0) {
    fpsText = Format("%.2f", fps);
    // German decimal comma; the rest of the UI is German too.
    for (char& c : fpsText) {
      if (c == '.') c = ',';
    }
    // Trim a trailing ",00" so 60 fps reads as "60" and 59.94 stays "59,94".
    if (fpsText.size() > 3 && fpsText.compare(fpsText.size() - 3, 3, ",00") == 0) {
      fpsText.resize(fpsText.size() - 3);
    }
  }
  std::string out = Format("%dx%d", width, height);
  if (!fpsText.empty()) {
    out += " @ " + fpsText + " Hz";
  } else if (width > 0) {
    // No rate stored is not "unknown", it is the request to work one out on
    // open. Saying so beats a label that quietly omits the rate.
    out += fps < 0.0 ? T(" @ Rate des Signals", " @ the signal's rate")
                     : T(" @ höchste verfügbare", " @ highest available");
  }
  if (!subtype.empty()) out += "  " + subtype;
  if (forced) out += T("  [erzwungen]", "  [forced]");
  return out;
}

// --------------------------------------------------------------------- Config

Config::Config() {
  profiles.emplace_back();  // one default profile so `active()` is always valid
}

Profile& Config::active() {
  if (profiles.empty()) profiles.emplace_back();
  if (activeProfile < 0 || activeProfile >= (int)profiles.size()) activeProfile = 0;
  return profiles[(size_t)activeProfile];
}

const Profile& Config::active() const {
  return const_cast<Config*>(this)->active();
}

void Config::SetActiveProfile(int index) {
  if (index >= 0 && index < (int)profiles.size()) activeProfile = index;
}

std::filesystem::path Config::FilePath() {
  return OwnFile("json");
}

std::vector<ForeignSettings> FindForeignSettings() {
  const std::string mine = ToUpper(PathToUtf8(Config::FilePath().stem()));

  // Sorted by when it was last written, newest first: if somebody has three of
  // these lying around, the one they were using last is the interesting one.
  // Files set aside earlier end in ".bak" and are not in this list.
  std::vector<std::pair<int64_t, ForeignSettings>> dated;
  for (const std::filesystem::path& file : FilesWithExtension(ExeFolder(), ".json")) {
    const std::string stem = PathToUtf8(file.stem());
    if (ToUpper(stem) == mine) continue;
    ForeignSettings entry;
    entry.stem = stem;
    entry.path = file;

    std::string text;
    if (!ReadJsonFile(entry.path, text)) continue;
    if (!LooksLikeSettings(json::Parse(text), &entry)) continue;

    const int64_t when = FileWriteTime(file);
    entry.modified = when;
    dated.emplace_back(when, std::move(entry));
  }

  std::sort(dated.begin(), dated.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });
  std::vector<ForeignSettings> out;
  out.reserve(dated.size());
  for (auto& d : dated) out.push_back(std::move(d.second));
  return out;
}

int SettingsLanguage(const std::filesystem::path& path) {
  std::string text;
  if (!ReadJsonFile(path, text)) return -1;
  ForeignSettings probe;
  if (!LooksLikeSettings(json::Parse(text), &probe)) return -1;
  return probe.language;
}

std::string AdoptSettings(SettingsChoice ask) {
  const std::vector<ForeignSettings> others = FindForeignSettings();
  if (others.empty()) return std::string();

  const ForeignSettings& other = others.front();
  const std::filesystem::path mine = Config::FilePath();
  const bool haveOwn = PathExists(mine);

  // Nobody but the user knows which of the two files has the evening's work in
  // it, so nobody but the user answers this. The loser is set aside rather than
  // removed, which makes the wrong answer survivable.
  const SettingsAnswer answer = ask ? ask(other, haveOwn) : SettingsAnswer::Discard;
  if (answer == SettingsAnswer::Postpone) return std::string();
  if (answer == SettingsAnswer::Discard) {
    SetFileAside(other.path);
    return std::string();
  }
  if (haveOwn) SetFileAside(mine);
  if (!RenameOver(other.path, mine)) return std::string();

  // What the file calls the program if it says so, otherwise what it is called
  // -- either way the name those settings were written under.
  const std::string from = other.program.empty() ? other.stem : other.program;
  SetAdoptedFrom(from);
  return from;
}

bool Config::Load(std::string* error) {
  if (error) error->clear();

  std::string text;
  if (!ReadJsonFile(FilePath(), text)) {
    return false;  // no file yet -- first run, defaults apply
  }

  std::string parseError;
  json::Value root = json::Parse(text, &parseError);
  if (!parseError.empty() || !root.IsObject()) {
    // Only ever logged, and before the log is open -- so English, and handed
    // back rather than written.
    if (error) {
      *error = parseError.empty() ? "The settings file is not valid JSON"
                                  : ("The settings file is damaged: " + parseError);
    }
    return false;
  }

  const json::Value& a = root["app"];
  app.theme = ReadEnum<Theme>(a, "theme", 3, Theme::System);
  // Englisch, wie in AppSettings. Stand hier auf Deutsch und war damit ein
  // stiller Widerspruch: eine frische Installation kam auf Englisch hoch, eine
  // Konfiguration ohne diesen Schlüssel auf Deutsch.
  app.language = ReadEnum<Language>(a, "language", 2, Language::English);
  app.videoRegion = ReadEnum<VideoRegion>(a, "videoRegion", kVideoRegionCount, VideoRegion::Auto);
  app.accentColor = (unsigned)Clamp(a["accentColor"].AsInt(0x8B5CF6), 0, 0xFFFFFF);
  app.osdCorner = ReadEnum<OsdCorner>(a, "osdCorner", 4, OsdCorner::TopRight);
  app.showVolumeOsd = a["showVolumeOsd"].AsBool(true);
  app.wheelVolume = a["wheelVolume"].AsBool(true);
  app.vsync = a["vsync"].AsBool(false);
  app.alwaysOnTop = a["alwaysOnTop"].AsBool(false);
  app.borderless = a["borderless"].AsBool(false);
  app.hideCursorFullscreen = a["hideCursorFullscreen"].AsBool(true);
  app.preventSleep = a["preventSleep"].AsBool(true);
  app.showStats = a["showStats"].AsBool(false);
  app.showToolbar = a["showToolbar"].AsBool(true);
  app.settingsSeparateWindow = a["settingsSeparateWindow"].AsBool(false);
  app.settingsTab = a["settingsTab"].AsInt(0);
  app.fileToastHints = Clamp(a["fileToastHints"].AsInt(0), 0, 1000);
  app.settingsPanelX = a["settingsPanelX"].AsInt(-1);
  app.settingsPanelY = a["settingsPanelY"].AsInt(-1);
  app.settingsPanelW = a["settingsPanelW"].AsInt(0);
  app.settingsPanelH = a["settingsPanelH"].AsInt(0);
  app.settingsWindowX = a["settingsWindowX"].AsInt(-1);
  app.settingsWindowY = a["settingsWindowY"].AsInt(-1);
  app.settingsWindowW = a["settingsWindowW"].AsInt(0);
  app.settingsWindowH = a["settingsWindowH"].AsInt(0);
  app.checkUpdatesOnStart = a["checkUpdatesOnStart"].AsBool(true);
  app.virtualCamera = a["virtualCamera"].AsBool(false);
  app.hdrInput = ReadEnum<HdrInput>(a, "hdrInput", kHdrInputCount, HdrInput::Auto);
  app.hdrOutput = ReadEnum<HdrOutput>(a, "hdrOutput", kHdrOutputCount, HdrOutput::Auto);
  app.paperWhiteNits = (float)a["paperWhiteNits"].AsNumber(203.0);
  app.sourcePeakNits = (float)a["sourcePeakNits"].AsNumber(1000.0);
  app.recordHdr = a["recordHdr"].AsBool(false);
  app.screenshotHdr = a["screenshotHdr"].AsBool(false);
  app.hdrShotFormat = ReadEnum<HdrShotFormat>(a, "hdrShotFormat", kHdrShotFormatCount,
                                              HdrShotFormat::Jxr);
  app.cameraHdr = a["cameraHdr"].AsBool(false);
  app.statsDetail = ReadEnum<StatsDetail>(a, "statsDetail", 3, StatsDetail::Compact);
  app.logToFile = a["logToFile"].AsBool(false);
  app.logRetention.byAge = a["logByAge"].AsBool(true);
  app.logRetention.days = Clamp(a["logDays"].AsInt(14), 1, 3650);
  app.logRetention.byCount = a["logByCount"].AsBool(true);
  app.logRetention.sessions = Clamp(a["logSessions"].AsInt(10), 1, 1000);
  app.logRetention.olderVersions = a["logOlderVersions"].AsBool(false);
  app.windowX = a["windowX"].AsInt(AppSettings::kWindowPosUnset);
  app.windowY = a["windowY"].AsInt(AppSettings::kWindowPosUnset);
  app.windowW = Clamp(a["windowW"].AsInt(1280), 160, 16384);
  app.windowH = Clamp(a["windowH"].AsInt(720), 120, 16384);
  app.maximized = a["maximized"].AsBool(false);
  app.startFullscreen = a["startFullscreen"].AsBool(false);
  app.fullscreenMonitor = a["fullscreenMonitor"].AsInt(-1);

  const json::Value& r = root["record"];
  record.outputFolder = r["outputFolder"].AsString();
  record.container = ReadEnum<RecordContainer>(r, "container", 2, RecordContainer::Mkv);
  record.encoder =
      ReadEnum<RecordEncoder>(r, "encoder", kRecordEncoderCount, RecordEncoder::Auto);
  record.speed = ReadEnum<RecordSpeed>(r, "speed", 5, RecordSpeed::VeryFast);
  record.bitrateKbps = Clamp(r["bitrateKbps"].AsInt(20000), 500, 500000);
  record.rateControl = ReadEnum<RateControl>(r, "rateControl", kRateControlCount,
                                          RateControl::Cbr);
  record.qualityLevel = (int)r["qualityLevel"].AsNumber(23);
  record.preset = ReadEnum<EncoderPreset>(r, "preset", kEncoderPresetCount, EncoderPreset::Auto);
  record.tune = ReadEnum<EncoderTune>(r, "tune", kEncoderTuneCount, EncoderTune::Auto);
  record.multipass = ReadEnum<Multipass>(r, "multipass", kMultipassCount, Multipass::Auto);
  record.lookAhead = r["lookAhead"].AsBool(false);
  record.adaptiveQuant = r["adaptiveQuant"].AsBool(false);
  record.fps = Clamp(r["fps"].AsNumber(0.0), 0.0, 480.0);
  record.splitFiles = r["splitFiles"].AsBool(false);
  record.splitSizeMb = Clamp(r["splitSizeMb"].AsInt(4000), 100, 100000);
  record.ffmpegPath = r["ffmpegPath"].AsString();
  record.encoderProbeSignature = r["encoderProbeSignature"].AsString();
  record.encodersAvailable.clear();
  const json::Value& enc = r["encodersAvailable"];
  for (size_t i = 0; i < enc.Size(); ++i) {
    const int id = enc.At(i).AsInt(-1);
    if (id >= 0 && id < 10) record.encodersAvailable.push_back(id);
  }
  record.screenshotFolder = r["screenshotFolder"].AsString();
  record.screenshotFormat = ReadEnum<ScreenshotFormat>(r, "screenshotFormat", 2,
                                                       ScreenshotFormat::Png);
  record.jpegQuality = Clamp(r["jpegQuality"].AsInt(92), 1, 100);
  record.screenshotIncludeUi = r["screenshotIncludeUi"].AsBool(false);

  const json::Value& hk = root["hotkeys"];
  std::string dropped;
  for (int i = 0; i < (int)HotkeyAction::Count; ++i) {
    const json::Value& entry = hk[HotkeyActionKey((HotkeyAction)i)];
    if (!entry.IsObject()) continue;  // missing entry keeps the factory default
    HotkeyBinding& b = hotkeys.items[i];
    // The key goes by name since 4.5. Older files carry only the number qBlank
    // used to store, and that is still honoured: a user who updates keeps
    // every binding.
    Key named;
    if (KeyFromName(entry["key"].AsString(), &named)) {
      b.key = named;
    } else if (entry["vk"].IsNumber()) {
      const int code = Clamp(entry["vk"].AsInt(0), 0, 255);
      b.key = KeyFromLegacyCode(code);
      if (code != 0 && b.key == Key::None) {
        if (!dropped.empty()) dropped += ", ";
        dropped += Format("%s (0x%02X)", HotkeyActionKey((HotkeyAction)i), code);
      }
    }
    b.ctrl = entry["ctrl"].AsBool(false);
    b.shift = entry["shift"].AsBool(false);
    b.alt = entry["alt"].AsBool(false);
  }

  profiles.clear();
  const json::Value& list = root["profiles"];
  for (size_t i = 0; i < list.Size(); ++i) {
    profiles.push_back(ReadProfile(list.At(i)));
  }
  if (profiles.empty()) profiles.emplace_back();

  activeProfile = Clamp(root["activeProfile"].AsInt(0), 0, (int)profiles.size() - 1);
  // Before the log is open as well, like the parse error above: handed back.
  if (error && !dropped.empty()) {
    *error = "Hotkeys with a code that is no keyboard key, left unbound: " + dropped;
  }
  return true;
}

bool Config::Save(std::string* error) const {
  if (error) error->clear();
  const std::string text = Serialize();
  if (!ReplaceWholeFile(FilePath(), text.data(), text.size())) {
    ReportError(error,
                CAP_SAID(T("Konfiguration konnte nicht geschrieben werden (Schreibrechte im Programmordner?)",
                           "Could not write the configuration (write access to the program folder?)")));
    return false;
  }
  return true;
}

std::string Config::Serialize() const {
  json::Value root = json::Value::Object();
  root["version"] = 1;
  // So a later build can tell at a glance whose file this is and what the
  // program was called when it was written, without matching on the file name.
  root["program"] = AppNameUtf8();

  json::Value a = json::Value::Object();
  a["theme"] = (int)app.theme;
  a["language"] = (int)app.language;
  a["videoRegion"] = (int)app.videoRegion;
  a["accentColor"] = (int)app.accentColor;
  a["osdCorner"] = (int)app.osdCorner;
  a["showVolumeOsd"] = app.showVolumeOsd;
  a["wheelVolume"] = app.wheelVolume;
  a["vsync"] = app.vsync;
  a["alwaysOnTop"] = app.alwaysOnTop;
  a["borderless"] = app.borderless;
  a["hideCursorFullscreen"] = app.hideCursorFullscreen;
  a["preventSleep"] = app.preventSleep;
  a["showStats"] = app.showStats;
  a["showToolbar"] = app.showToolbar;
  a["settingsSeparateWindow"] = app.settingsSeparateWindow;
  a["settingsTab"] = app.settingsTab;
  a["fileToastHints"] = app.fileToastHints;
  a["settingsPanelX"] = app.settingsPanelX;
  a["settingsPanelY"] = app.settingsPanelY;
  a["settingsPanelW"] = app.settingsPanelW;
  a["settingsPanelH"] = app.settingsPanelH;
  a["settingsWindowX"] = app.settingsWindowX;
  a["settingsWindowY"] = app.settingsWindowY;
  a["settingsWindowW"] = app.settingsWindowW;
  a["settingsWindowH"] = app.settingsWindowH;
  a["checkUpdatesOnStart"] = app.checkUpdatesOnStart;
  a["virtualCamera"] = app.virtualCamera;
  a["hdrInput"] = (int)app.hdrInput;
  a["hdrOutput"] = (int)app.hdrOutput;
  a["paperWhiteNits"] = app.paperWhiteNits;
  a["sourcePeakNits"] = app.sourcePeakNits;
  a["recordHdr"] = app.recordHdr;
  a["screenshotHdr"] = app.screenshotHdr;
  a["hdrShotFormat"] = (int)app.hdrShotFormat;
  a["cameraHdr"] = app.cameraHdr;
  a["statsDetail"] = (int)app.statsDetail;
  a["logToFile"] = app.logToFile;
  a["logByAge"] = app.logRetention.byAge;
  a["logDays"] = app.logRetention.days;
  a["logByCount"] = app.logRetention.byCount;
  a["logSessions"] = app.logRetention.sessions;
  a["logOlderVersions"] = app.logRetention.olderVersions;
  a["windowX"] = app.windowX;
  a["windowY"] = app.windowY;
  a["windowW"] = app.windowW;
  a["windowH"] = app.windowH;
  a["maximized"] = app.maximized;
  a["startFullscreen"] = app.startFullscreen;
  a["fullscreenMonitor"] = app.fullscreenMonitor;
  root["app"] = a;

  json::Value r = json::Value::Object();
  r["outputFolder"] = record.outputFolder;
  r["container"] = (int)record.container;
  r["encoder"] = (int)record.encoder;
  r["speed"] = (int)record.speed;
  r["bitrateKbps"] = record.bitrateKbps;
  r["rateControl"] = (int)record.rateControl;
  r["qualityLevel"] = record.qualityLevel;
  r["preset"] = (int)record.preset;
  r["tune"] = (int)record.tune;
  r["multipass"] = (int)record.multipass;
  r["lookAhead"] = record.lookAhead;
  r["adaptiveQuant"] = record.adaptiveQuant;
  r["fps"] = record.fps;
  r["splitFiles"] = record.splitFiles;
  r["splitSizeMb"] = record.splitSizeMb;
  r["ffmpegPath"] = record.ffmpegPath;
  r["encoderProbeSignature"] = record.encoderProbeSignature;
  json::Value enc = json::Value::Array();
  for (int id : record.encodersAvailable) enc.Push(json::Value(id));
  r["encodersAvailable"] = enc;
  r["screenshotFolder"] = record.screenshotFolder;
  r["screenshotFormat"] = (int)record.screenshotFormat;
  r["jpegQuality"] = record.jpegQuality;
  r["screenshotIncludeUi"] = record.screenshotIncludeUi;
  root["record"] = r;

  json::Value hk = json::Value::Object();
  for (int i = 0; i < (int)HotkeyAction::Count; ++i) {
    const HotkeyBinding& b = hotkeys.items[i];
    json::Value entry = json::Value::Object();
    entry["key"] = KeyName(b.key);
    // For an older qBlank reading the same file after a downgrade.
    entry["vk"] = LegacyCode(b.key);
    entry["ctrl"] = b.ctrl;
    entry["shift"] = b.shift;
    entry["alt"] = b.alt;
    hk[HotkeyActionKey((HotkeyAction)i)] = entry;
  }
  root["hotkeys"] = hk;

  json::Value list = json::Value::Array();
  for (const Profile& p : profiles) list.Push(WriteProfile(p));
  root["profiles"] = list;
  root["activeProfile"] = activeProfile;

  return json::Dump(root, 2);
}

}  // namespace cap
