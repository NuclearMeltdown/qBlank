#pragma once

// On-screen text drawn over the video: the statistics panel, the status card
// shown when there is nothing to display, the volume readout and short-lived
// toasts.

#include <string>

#include "audio/audio_engine.h"
#include "capture/frame_buffer.h"
#include "capture/video_format.h"
#include "common.h"
#include "config.h"

namespace cap {

// Ein Messwert, der jedes Bild anfaellt, aber viermal in der Sekunde angezeigt
// wird.
//
// Durchlaufzeit und Tonpuffer aendern sich mit jedem Bild, und eine Zahl, die
// sechzig Mal in der Sekunde eine andere ist, kann man nicht lesen -- bis das
// Auge eine Ziffer aufgeloest hat, steht dort laengst die naechste. Gesammelt
// wird deshalb weiter jedes Bild, angezeigt aber, was die letzte Viertelsekunde
// enthielt.
//
// Der Mittelwert ist die Zahl, die man eigentlich sehen will; die Extreme sind
// die, an denen man einen Ruckler erkennt. Die bleiben laenger stehen als der
// Mittelwert, denn ein Aussetzer dauert ein einziges Bild -- wuerde er nur sein
// eigenes Fenster lang angezeigt, waere er wieder weg, bevor man hinsieht.
struct StatMeter {
  // Ein Messwert. `now` sind Sekunden aus einer monotonen Uhr.
  void Sample(double value, double now);

  // Die Extreme seit dem letzten Abruf, den sie zuruecksetzt. Fuer die
  // Statuszeile im Log, die alle paar Sekunden geschrieben wird und sonst einen
  // einzelnen Augenblickswert meldete -- also genau das, was ein Ruckler nicht
  // ist. Beide Zeiger duerfen null sein.
  void TakeRange(double* low, double* high);

  bool valid = false;    // false, bis das erste Fenster geschlossen hat
  double average = 0.0;  // ueber das letzte geschlossene Fenster
  double low = 0.0;      // die gehaltenen Extreme
  double high = 0.0;

 private:
  double windowStart_ = 0.0;
  double sum_ = 0.0;
  int count_ = 0;
  double windowLow_ = 0.0;
  double windowHigh_ = 0.0;
  double lowAt_ = 0.0;
  double highAt_ = 0.0;
  // Zweites, unabhaengiges Paar fuer TakeRange: das Log fragt in einem ganz
  // anderen Takt als das Panel zeichnet, und beide sollen sich nicht in die
  // Quere kommen.
  bool sinceValid_ = false;
  double sinceLow_ = 0.0;
  double sinceHigh_ = 0.0;
};

struct OverlayStats {
  std::string profileName;
  std::string deviceName;
  std::string inputName;
  VideoFormatInfo format;
  SinkStats sink;
  AudioStats audio;
  double presentFps = 0.0;
  // How old the displayed frame was when it went out, and how full the audio
  // ring was -- both as meters rather than as numbers, see StatMeter.
  StatMeter frameAge;
  StatMeter audioBuffer;
  bool vsync = false;
  bool tearing = false;
  // How the picture is scanned and, when that means work, what is doing it.
  //
  // One row that is always there rather than a deinterlacer row that appears out
  // of nowhere once something latches: the interesting moment is the *change*,
  // and a row that only exists after the change has no before to compare
  // against. While it reads "progressiv" you know the detector has looked and
  // said no -- which is a different statement from a row not being there, and it
  // is the one you want while wondering whether the thing is working.
  //
  // Not simply the setting: on a source whose fields are co-sited every mode
  // does the same thing, and saying "YADIF" there would name a code path nobody
  // took.
  std::string scanLabel;
  int displayWidth = 0;
  int displayHeight = 0;
  const char* filterName = "";
  int videoDelayMs = 0;
  StatsDetail detail = StatsDetail::Full;
  // What the automatic colour handling settled on; empty when it is still
  // measuring or the user picked the values by hand.
  std::string colorInfo;
};

void DrawStatsPanel(const OverlayStats& stats);

// The empty state: the icon at size, the name under it, and one line saying why
// there is nothing to show. `icon` is an ImTextureID -- passed as the underlying
// integer so this header does not have to pull in ImGui -- and may be zero, in
// which case only the text is drawn.
//
// Separate from DrawStatusCard because it means something different. The card
// interrupts a picture; this replaces one that was never there.
void DrawIdleScreen(unsigned long long icon, int iconPixels, const std::string& detail);

// Big centred card, used for "no device", "no signal" and error states.
// `spinner` adds an animated dot row to show that a retry is running.
void DrawStatusCard(const std::string& title, const std::string& detail, bool spinner);

// Fades out over its lifetime; `age` and `duration` are in seconds.
//
// A toast about a file is clickable: it takes the mouse, shows the hand, and
// `hint`, if not empty, as a second, quieter line. The other kind lets every
// click through to the picture underneath.
struct ToastResult {
  bool hovered = false;
  bool clicked = false;
};
ToastResult DrawToast(const std::string& text, double age, double duration,
                      bool clickable = false, const char* hint = nullptr);

// Standard search, shown on the picture itself while it runs.
//
// It used to live only in the settings dialog, and that is the one place where
// nobody is looking: the search runs right after the source is switched, when
// the eye is on the picture and the dialog is closed. What is visible there
// without this is a picture that changes standard every few seconds for no
// stated reason. Top centre, clear of the statistics panel (top left), the
// recording dot (the corners) and the toasts (bottom centre).
// `detail` steht klein darunter und sagt, *warum* gesucht wird. Ohne das ist
// die Einblendung eine Behauptung: das Bild schaltet durch mehrere Normen, und
// die einzige Auskunft dazu sind drei laufende Punkte. Der Grund ist gemessen
// -- zu blasse Farbe, eingefaerbte Tiefen, kein Lock -- und gehoert deshalb
// dorthin, wo der Vorgang zu sehen ist. Leer heisst: nur die Kopfzeile.
void DrawSearchIndicator(const std::string& text, const std::string& detail = {});

// Und was dabei herauskam, an derselben Stelle und fuer ein paar Sekunden.
//
// Nur nach einer Suche, die jemand ausgeloest hat. Ein Tastendruck ist eine
// Frage; die Einblendung verschwinden zu lassen, sobald die Antwort feststeht,
// beantwortet sie nicht. Ohne laufende Punkte und mit Ausblenden, damit der
// Unterschied zwischen "laeuft noch" und "fertig" nicht am Text haengt.
void DrawSearchResult(const std::string& text, const std::string& detail, double age,
                      double duration);

// Volume readout with a bar, parked in the chosen corner. Shown for a moment
// after every change so the level is visible without opening anything.
void DrawVolumeOsd(float volume, bool muted, OsdCorner corner, double age, double duration);

// Recording indicator: a pulsing dot and the elapsed time, in the corner
// opposite the volume readout so the two never overlap.
void DrawRecordIndicator(double seconds, OsdCorner volumeCorner);

}  // namespace cap
