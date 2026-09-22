#pragma once

// ---------------------------------------------------------------------------
// Analogue video standard
//
// The setting behind the video standard list in a capture driver's own dialog.
// It matters more than it looks: PAL is 625 lines at 50 Hz and PAL-60 is 525 at
// 60, so a console that can do both needs the card told which one it is sending.
// Set the wrong one and either nothing locks at all or the picture arrives with
// the wrong number of lines in it.
//
// Values are the bitmask below. Its numbers are those of DirectShow's
// AnalogVideo_* on purpose: the config has always stored them, and keeping them
// means no stored setting has to be translated.

#include <string>
#include <vector>

#include "config.h"

namespace cap {

constexpr long kVideoStdNone = 0;
constexpr long kVideoStdNtscM = 0x1;
constexpr long kVideoStdNtscMJ = 0x2;
constexpr long kVideoStdNtsc433 = 0x4;
constexpr long kVideoStdPalB = 0x10;
constexpr long kVideoStdPalD = 0x20;
constexpr long kVideoStdPalG = 0x40;
constexpr long kVideoStdPalH = 0x80;
constexpr long kVideoStdPalI = 0x100;
constexpr long kVideoStdPalM = 0x200;
constexpr long kVideoStdPalN = 0x400;
constexpr long kVideoStdPal60 = 0x800;
constexpr long kVideoStdSecamB = 0x1000;
constexpr long kVideoStdSecamD = 0x2000;
constexpr long kVideoStdSecamG = 0x4000;
constexpr long kVideoStdSecamH = 0x8000;
constexpr long kVideoStdSecamK = 0x10000;
constexpr long kVideoStdSecamK1 = 0x20000;
constexpr long kVideoStdSecamL = 0x40000;
constexpr long kVideoStdSecamL1 = 0x80000;
constexpr long kVideoStdPalNCombo = 0x100000;

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
