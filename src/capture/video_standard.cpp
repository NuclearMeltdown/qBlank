#include "capture/video_standard.h"

#include "i18n.h"

namespace cap {
namespace {

struct StandardEntry {
  long value;
  const char* name;
  int lines;
  // Halbbilder je Sekunde. Nicht aus der Zeilenzahl ableitbar: die 525-Zeiler
  // teilen sich in die, deren Takt am NTSC-Farbtraeger haengt -- 60000/1001,
  // die krumme Rate --, und PAL 60, das es nur gibt, weil eine umgebaute
  // Konsole oder ein Player ein PAL-Bild mit *glatten* 60 Hz erzeugt. Beide
  // bietet dieselbe Karte an, und wer bei PAL 60 die 59,94 nimmt, hat sich um
  // ein Bild in tausend vertan.
  double fieldRate;
};

// Ordered so the common ones come first. The config stores the DirectShow value
// and not the index, so this list may be reordered and regrouped freely.
const StandardEntry kStandards[] = {
    {kVideoStdPalB, "PAL B", 625, 50.0},
    {kVideoStdPalG, "PAL G", 625, 50.0},
    {kVideoStdPalI, "PAL I", 625, 50.0},
    {kVideoStdPalD, "PAL D", 625, 50.0},
    {kVideoStdPalH, "PAL H", 625, 50.0},
    {kVideoStdPalN, "PAL N", 625, 50.0},
    {kVideoStdPal60, "PAL 60", 525, 60.0},
    {kVideoStdPalM, "PAL M", 525, 59.94},
    {kVideoStdNtscM, "NTSC M", 525, 59.94},
    {kVideoStdNtscMJ, "NTSC M (Japan)", 525, 59.94},
    {kVideoStdNtsc433, "NTSC 4.43", 525, 59.94},
    {kVideoStdSecamB, "SECAM B", 625, 50.0},
    {kVideoStdSecamD, "SECAM D", 625, 50.0},
    {kVideoStdSecamG, "SECAM G", 625, 50.0},
    {kVideoStdSecamH, "SECAM H", 625, 50.0},
    {kVideoStdSecamK, "SECAM K", 625, 50.0},
    {kVideoStdSecamK1, "SECAM K1", 625, 50.0},
    {kVideoStdSecamL, "SECAM L", 625, 50.0},
    {kVideoStdSecamL1, "SECAM L1", 625, 50.0},
    {kVideoStdPalNCombo, "PAL N combo", 625, 50.0},
};
const int kStandardCount = (int)(sizeof(kStandards) / sizeof(kStandards[0]));

// The letters behind PAL and SECAM name RF properties -- sound carrier spacing
// and channel bandwidth -- and none of that survives the trip down a composite
// or S-Video cable. What reaches the decoder is a line count, a field rate and a
// colour subcarrier, and by those there are only the eight distinct pictures
// below. A list of twenty entries where eight of them differ is a list that
// makes the reader guess which one is theirs.
//
// The group is a display device only. What goes into the config is still the
// concrete DirectShow value the card accepted, so existing configs keep working
// and the readout below the picker can still name the exact standard.
//
// NTSC M and its Japanese variant stay apart on purpose: they differ in setup,
// the 7.5 IRE pedestal that lifts black off the blanking level, and that is a
// visible difference rather than a broadcast one.
struct StandardGroup {
  const char* name;
  long members;  // every value that decodes to the same picture
  const char* hintDe;
  const char* hintEn;
};

const StandardGroup kStandardGroups[] = {
    {"PAL",
     kVideoStdPalB | kVideoStdPalG | kVideoStdPalI | kVideoStdPalD |
         kVideoStdPalH,
     "625 Zeilen, 50 Hz, Träger 4,43 MHz. Europa, Australien, weite Teile Asiens und "
     "Afrikas. Umfasst PAL B, G, I, D und H, die sich nur im Tonträger unterscheiden.",
     "625 lines, 50 Hz, 4.43 MHz subcarrier. Europe, Australia, much of Asia and Africa. "
     "Covers PAL B, G, I, D and H, which differ only in their sound carrier."},
    {"PAL 60", kVideoStdPal60,
     "525 Zeilen, 60 Hz, Träger 4,43 MHz. Keine Sendenorm, sondern das, was umgebaute "
     "Konsolen und Player an einen PAL-Fernseher ausgeben.",
     "525 lines, 60 Hz, 4.43 MHz subcarrier. Not a broadcast standard, but what modified "
     "consoles and players send to a PAL television."},
    {"PAL M", kVideoStdPalM,
     "525 Zeilen, 60 Hz, Träger 3,58 MHz. Brasilien.",
     "525 lines, 60 Hz, 3.58 MHz subcarrier. Brazil."},
    {"PAL N", kVideoStdPalN | kVideoStdPalNCombo,
     "625 Zeilen, 50 Hz, Träger 3,58 MHz. Argentinien, Uruguay, Paraguay.",
     "625 lines, 50 Hz, 3.58 MHz subcarrier. Argentina, Uruguay, Paraguay."},
    {"NTSC", kVideoStdNtscM,
     "525 Zeilen, 60 Hz, Träger 3,58 MHz. Nordamerika. Schwarz liegt 7,5 IRE über "
     "Austastung.",
     "525 lines, 60 Hz, 3.58 MHz subcarrier. North America. Black sits 7.5 IRE above "
     "blanking."},
    {"NTSC Japan", kVideoStdNtscMJ,
     "Wie NTSC, aber Schwarz liegt auf Austastpegel. Zu hell wirkendes Schwarz an einer "
     "japanischen Konsole ist meistens dieser Unterschied.",
     "Like NTSC, but black sits at blanking level. Washed-out black on a Japanese console "
     "is usually this difference."},
    {"NTSC 4.43", kVideoStdNtsc433,
     "525 Zeilen, 60 Hz, aber Träger 4,43 MHz. Keine Sendenorm, sondern was manche "
     "PAL-Player aus einer NTSC-Quelle machen.",
     "525 lines, 60 Hz, but a 4.43 MHz subcarrier. Not a broadcast standard, but what some "
     "PAL players make of an NTSC source."},
    {"SECAM",
     kVideoStdSecamB | kVideoStdSecamD | kVideoStdSecamG | kVideoStdSecamH |
         kVideoStdSecamK | kVideoStdSecamK1 | kVideoStdSecamL |
         kVideoStdSecamL1,
     "625 Zeilen, 50 Hz, Farbe frequenzmoduliert statt in der Phase. Frankreich, "
     "Osteuropa. Umfasst SECAM B bis L1.",
     "625 lines, 50 Hz, colour frequency modulated rather than carried in the phase. "
     "France, eastern Europe. Covers SECAM B through L1."},
};
const int kStandardGroupCount = (int)(sizeof(kStandardGroups) / sizeof(kStandardGroups[0]));

}  // namespace

int VideoStandardCount() { return kStandardCount; }

long VideoStandardValue(int index) {
  if (index < 0 || index >= kStandardCount) return 0;
  return kStandards[index].value;
}

const char* VideoStandardName(int index) {
  if (index < 0 || index >= kStandardCount) return "";
  return kStandards[index].name;
}

int VideoStandardIndexOf(long value) {
  for (int i = 0; i < kStandardCount; ++i) {
    if (kStandards[i].value == value) return i;
  }
  return -1;
}

int VideoStandardLines(long value) {
  const int index = VideoStandardIndexOf(value);
  return index < 0 ? 0 : kStandards[index].lines;
}

double VideoStandardFieldRate(long value) {
  const int index = VideoStandardIndexOf(value);
  return index < 0 ? 0.0 : kStandards[index].fieldRate;
}

int VideoStandardGroupCount() { return kStandardGroupCount; }

const char* VideoStandardGroupName(int index) {
  if (index < 0 || index >= kStandardGroupCount) return "";
  return kStandardGroups[index].name;
}

const char* VideoStandardGroupHint(int index) {
  if (index < 0 || index >= kStandardGroupCount) return "";
  return T(kStandardGroups[index].hintDe, kStandardGroups[index].hintEn);
}

int VideoStandardGroupOf(long value) {
  if (value <= 0) return -1;
  for (int i = 0; i < kStandardGroupCount; ++i) {
    if (kStandardGroups[i].members & value) return i;
  }
  return -1;
}

// The value to hand the decoder when a group is picked. Its members produce the
// same picture, so any one the card admits to will do; kStandards decides the
// order, which is the one that puts the common variants first.
long VideoStandardGroupPick(int index, long available) {
  if (index < 0 || index >= kStandardGroupCount) return 0;
  const long members = kStandardGroups[index].members & available;
  if (members == 0) return 0;
  for (int i = 0; i < kStandardCount; ++i) {
    if (members & kStandards[i].value) return kStandards[i].value;
  }
  return 0;
}

std::string VideoStandardSettingName(long setting) {
  if (setting == 0) return T("Nicht ändern", "Leave alone");
  if (setting == -1) return T("Automatisch", "Automatic");
  const int index = VideoStandardIndexOf(setting);
  return index < 0 ? T("Unbekannt", "Unknown") : VideoStandardName(index);
}

// What the pickers show. They offer groups, so echoing back the exact variant
// the card took would name something the list never offered. The readout under
// the picker stays exact -- that one exists to say what really happened.
std::string VideoStandardPickerName(long setting) {
  const int group = VideoStandardGroupOf(setting);
  return group < 0 ? VideoStandardSettingName(setting) : VideoStandardGroupName(group);
}

VideoColourSystem VideoStandardColourSystem(long standard) {
  // Nach dem, was der Decoder mit der Farbe macht, nicht nach dem Namen. Die
  // Buchstaben hinter PAL und SECAM sind Rundfunkeigenschaften, und PAL 60,
  // PAL M und PAL N heissen PAL, weil sie die Phase zeilenweise wenden -- was
  // sie voneinander trennt, ist Zeilenzahl und Traeger, und beides wird
  // anderswo gefragt.
  switch (standard) {
    case kVideoStdNtscM:
    case kVideoStdNtscMJ:
    case kVideoStdNtsc433:
      return VideoColourSystem::Ntsc;
    case kVideoStdSecamB:
    case kVideoStdSecamD:
    case kVideoStdSecamG:
    case kVideoStdSecamH:
    case kVideoStdSecamK:
    case kVideoStdSecamK1:
    case kVideoStdSecamL:
    case kVideoStdSecamL1:
      return VideoColourSystem::Secam;
    default:
      return VideoColourSystem::Pal;
  }
}

double VideoStandardSubcarrierSamples(long standard) {
  // 13.5 MHz of sampling over the active line, from BT.601.
  const double kSampleRate = 13500000.0;
  // NTSC M and its Japanese variant carry 3.579545 MHz. PAL M at 3.575611 and
  // PAL N at 3.582056 are close enough to count with them: 0.11 % and 0.07 %
  // off, which over the nine periods the demodulator looks at comes to a
  // fiftieth of a sample. Everything else in practice -- PAL, PAL-60, SECAM, and
  // the 4.43 MHz NTSC variant -- sits at 4.43361875 MHz.
  const bool ntsc = standard == kVideoStdNtscM || standard == kVideoStdNtscMJ ||
                    standard == kVideoStdPalM || standard == kVideoStdPalN ||
                    standard == kVideoStdPalNCombo;
  const double carrier = ntsc ? 3579545.0 : 4433618.75;
  return kSampleRate / carrier;
}


// Der Partner einer Norm: dieselbe Farbe, die andere Bildfrequenz.
//
// PAL und PAL 60 tragen beide den 4,43-MHz-Traeger und unterscheiden sich nur
// in Zeilenzahl und Bildrate. Genau dazwischen springt eine Konsole, die von 50
// auf 60 Hz umschaltet, und genau dort geht der Lock verloren.
//
// Das ist nicht nur eine Wahrscheinlichkeit, sondern noetig: der Decoder meldet
// einen *horizontalen* Lock, und PAL 60 und NTSC M haben dieselben 525 Zeilen
// bei 60 Hz. Sie unterscheiden sich allein im Farbtraeger, den diese Meldung
// gar nicht anfasst. Wer zuerst gefragt wird, gewinnt also -- deshalb muss der
// Partner vor dem allgemeinen Durchlauf drankommen.
long VideoStandardSibling(long standard, long available) {
  const int group = VideoStandardGroupOf(standard);
  if (group < 0) return 0;
  const int pal = VideoStandardGroupOf(kVideoStdPalB);
  const int pal60 = VideoStandardGroupOf(kVideoStdPal60);
  if (group == pal) return VideoStandardGroupPick(pal60, available);
  if (group == pal60) return VideoStandardGroupPick(pal, available);
  return 0;
}

namespace {

// Was in einer Region ueberhaupt vorkommt, in der Reihenfolge, in der es dort
// vorkommt. Endet je Zeile mit 0.
//
// Entscheidend ist nicht, was in der Liste steht, sondern was *zuerst* steht:
// innerhalb einer Zeilenzahl gewinnt der erste Eintrag immer, weil der Lock die
// Farbe nicht misst. Fuer Europa heisst das PAL 60 vor NTSC M -- eine 525/60-
// Quelle an einem europaeischen Anschluss ist fast immer eine umgebaute Konsole
// im 60-Hz-Modus und fast nie ein amerikanisches Geraet. In Nordamerika steht
// es genau andersherum, und das ist derselbe Satz mit vertauschten Rollen.
//
// Der Rest kommt danach aus kCommon und kRare dazu; hier steht nur der Vorlauf.
//
// `VideoRegion::None` hat hier absichtlich keine Zeile. Wer keine Region nennt,
// bekommt keinen Vorlauf -- die Schleife unten findet dann nichts und faellt
// direkt in kCommon. Das ist kein Sonderfall im Code, sondern einer, den die
// Tabelle durch Schweigen erledigt.
struct RegionLead {
  VideoRegion region;
  long order[4];
};
const RegionLead kRegionLeads[] = {
    {VideoRegion::PalEurope, {kVideoStdPalB, kVideoStdPal60, 0, 0}},
    {VideoRegion::NtscAmerica, {kVideoStdNtscM, kVideoStdNtscMJ, 0, 0}},
    {VideoRegion::NtscJapan, {kVideoStdNtscMJ, kVideoStdNtscM, 0, 0}},
    {VideoRegion::Secam, {kVideoStdSecamB, kVideoStdPalB, kVideoStdPal60, 0}},
    // Brasilien sendete 525/60 mit dem NTSC-Traeger; die Geraete von jenseits
    // der Grenze sind NTSC, nicht PAL.
    {VideoRegion::PalBrazil, {kVideoStdPalM, kVideoStdNtscM, 0, 0}},
    // PAL N ist 625/50 und steht damit vor PAL B, das dieselben Zeilen hat.
    {VideoRegion::PalArgentina, {kVideoStdPalN, kVideoStdPalB, 0, 0}},
};

}  // namespace

std::vector<long> AutoStandardCandidates(long available, VideoRegion region, long lastGood,
                                         int* preferred) {
  // Je ein Vertreter pro Zeilenzahl und Farbsystem, ueber die Gruppen gewaehlt.
  // PAL B und PAL G zu probieren hiesse dieselbe Frage zweimal stellen -- sie
  // unterscheiden sich im Tonträger, der kein Teil des Bildes ist. Ueber die
  // Gruppe statt ueber ein festes Bit, damit eine Karte, die PAL I meldet aber
  // kein PAL B, trotzdem einen PAL-Kandidaten bekommt.
  //
  // Zuerst die gaengigen, dann die seltenen. Jeder Kandidat kostet eine
  // Wartezeit, und die haeufigen sollen nicht hinter Normen stehen, die
  // ausserhalb einer Handvoll Laender niemand hat. Karten melden PAL M und
  // PAL N naemlich unabhaengig davon, ob im Umkreis von tausend Kilometern
  // jemand so sendet.
  static const long kCommon[] = {
      kVideoStdPalB,    // PAL, 625/50 -- Europa, Australien, halb Asien
      kVideoStdPal60,   // PAL 60, 525/60 -- umgebaute Konsolen und Player
      kVideoStdNtscM,   // NTSC, 525/60 -- Nordamerika
      kVideoStdNtscMJ,  // NTSC Japan, dasselbe mit anderem Schwarzpegel
  };
  static const long kRare[] = {
      kVideoStdSecamB,   // SECAM, 625/50 -- Frankreich, Osteuropa
      kVideoStdPalM,     // 525/60 mit 3,58 MHz -- Brasilien
      kVideoStdPalN,     // 625/50 mit 3,58 MHz -- Argentinien und Nachbarn
      kVideoStdNtsc433,  // 525/60 mit 4,43 MHz -- was PAL-Player daraus machen
  };

  std::vector<long> out;
  auto add = [&out](long value) {
    if (value <= 0) return;
    for (long have : out) {
      if (have == value) return;
    }
    out.push_back(value);
  };
  auto addFamily = [&](long representative) {
    add(VideoStandardGroupPick(VideoStandardGroupOf(representative), available));
  };

  // Ganz vorne der Partner der zuletzt eingerasteten Norm, dahinter sie selbst.
  // Das deckt die zwei Faelle ab, in denen ein Lock ueberhaupt verloren geht,
  // ohne dass das Kabel gezogen wurde: die Konsole schaltet zwischen 50 und
  // 60 Hz um, oder sie wurde neu gestartet und kommt gleich wieder.
  const long sibling = VideoStandardSibling(lastGood, available);
  add(sibling);
  if (lastGood > 0 && (available & lastGood) != 0) add(lastGood);
  if (preferred) *preferred = (int)out.size();

  // Dahinter das, was in der Gegend des Nutzers ueberhaupt vorkommt. Erst
  // danach der allgemeine Durchlauf -- der bleibt vollstaendig, damit eine
  // falsch eingestellte Region die richtige Norm verzoegert und nicht
  // verhindert.
  const VideoRegion resolved = ResolveVideoRegion(region);
  for (const RegionLead& lead : kRegionLeads) {
    if (lead.region != resolved) continue;
    for (long family : lead.order) {
      if (family == 0) break;
      addFamily(family);
    }
    break;
  }

  for (long family : kCommon) addFamily(family);
  for (long family : kRare) addFamily(family);
  return out;
}

std::vector<long> VideoStandardColourCandidates(long standard, long available,
                                                VideoRegion region) {
  std::vector<long> out;
  const int lines = VideoStandardLines(standard);
  if (lines <= 0) return out;

  // Ueber dieselbe Kandidatenliste wie die Normensuche selbst. Die hat die
  // Arbeit schon getan: je ein Vertreter pro Farbsystem, die Tonträgervarianten
  // zusammengefasst, und nach Region sortiert. Hier bleibt nur, sie auf die
  // Zeilenzahl einzuengen -- die steht ja fest, der Lock hat sie bestaetigt.
  //
  // Ohne lastGood: gesucht wird, was zur *jetzigen* Norm passt, nicht was
  // zuletzt gut war.
  const std::vector<long> ordered = AutoStandardCandidates(available, region, 0, nullptr);

  // Und was zweimal dasselbe misst, wird einmal gemessen.
  //
  // Der Rundgang urteilt ueber die Farbe und ueber nichts sonst: wieviel
  // Chroma im Bild steckt und wie eingefaerbt die Tiefen sind. Zwei Normen mit
  // demselben Farbsystem auf demselben Traeger erzeugen daraus dasselbe Bild,
  // und die zweite Messung kann deshalb nichts sagen, was die erste nicht
  // schon gesagt hat -- sie kostet nur ihre Einschwingzeit und stellt ihr
  // Ergebnis als Konkurrenten neben das Original.
  //
  // Bei 525 Zeilen trifft das genau ein Paar: NTSC M und NTSC M (Japan), beide
  // NTSC auf 3,58 MHz. Sie unterscheiden sich im Schwarzabhebungswert -- 7,5
  // IRE gegen 0 -- und das ist eine Sache der Helligkeit, die dieser Rundgang
  // gar nicht misst. Am 31.08.2026 um 02:42 standen sie im Log denn auch
  // brav nebeneinander, 0,9 Sekunden fuer eine Auskunft, die schon vorlag.
  //
  // Bei 625 Zeilen faellt nichts weg: PAL B (4,43), PAL N (3,58) und SECAM
  // sind drei verschiedene Bilder. Der Rundgang verliert dadurch also nichts
  // ausser der Wiederholung.
  //
  // Welche der beiden stehen bleibt, entscheidet die Reihenfolge und damit die
  // Region -- dieselbe Regel, die schon ueberall sonst den Gleichstand
  // aufloest. In Japan kommt die japanische zuerst, sonst die amerikanische.
  auto sameColour = [](long a, long b) {
    return VideoStandardColourSystem(a) == VideoStandardColourSystem(b) &&
           VideoStandardSubcarrierSamples(a) == VideoStandardSubcarrierSamples(b);
  };

  // Die jetzige zuerst, denn sie ist bereits gemessen; sie noch einmal
  // einzustellen waere ein Wechsel und kostete eine Einschwingzeit umsonst.
  if ((available & standard) != 0) out.push_back(standard);
  for (long candidate : ordered) {
    if (candidate == standard) continue;
    if (VideoStandardLines(candidate) != lines) continue;
    bool duplicate = false;
    for (long have : out) {
      if (sameColour(have, candidate)) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;
    out.push_back(candidate);
  }
  return out;
}

bool ConnectorFollowsVideoStandard(ConnectorKind kind) {
  switch (kind) {
    case ConnectorKind::Composite:
    case ConnectorKind::SVideo:
    case ConnectorKind::Tuner:
    case ConnectorKind::Scart:
    case ConnectorKind::Aux:
      // SCART fuehrt je nach Kabel Composite oder RGB, aber beide in einem
      // Sendersaster: es gibt kein SCART, das 720p traegt.
      return true;
    default:
      // Composite und S-Video sind die einzigen, bei denen die Norm die
      // Zeilenzahl wirklich festlegt. Component und VGA sind analog und tragen
      // trotzdem, was die Quelle will; HDMI, DVI und SDI beantworten die Frage
      // gar nicht erst.
      return false;
  }
}

}  // namespace cap
