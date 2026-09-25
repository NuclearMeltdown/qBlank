#pragma once

// The automatic video standard: which standard the analogue decoder locks to,
// and whether the one it locked to also decodes the colour. Two stages, both
// driven from the main loop; a small thread of its own asks the driver for the
// lock so the render thread never has to.

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "ui/settings_window.h"

namespace cap {

class VideoCapture;
class VideoRenderer;

class VideoStandardSearch {
 public:
  // What the search needs from the application around it.
  class Host {
   public:
    virtual ~Host() = default;
    virtual bool CaptureRunning() const = 0;
    virtual bool SourceIsAnalogue() const = 0;
    virtual AnalogConnector ResolvedConnector() const = 0;
    virtual bool ConnectorHasColourCarrier() const = 0;
    // The standard the running graph was built with, as the settings name it.
    virtual long AppliedVideoStandard() const = 0;
    virtual bool ReleaseStandardBoundFormat(int newLines) = 0;
    virtual bool StartCapture(std::string* error) = 0;
    virtual void Toast(const std::string& text) = 0;
  };

  VideoStandardSearch(const Config& config, VideoCapture& capture, VideoRenderer& renderer,
                      Host& host);

  // Finds the analogue video standard by watching whether the decoder locks.
  // Runs only when the source is set to automatic.
  void UpdateVideoStandard();
  // Beide Stufen von Hand ausloesen, ohne den Umweg ueber den Normwaehler.
  // Siehe die Umsetzung: was ein Mensch an einer Norm auszusetzen hat, misst
  // die Automatik nicht unbedingt mit.
  void RescanVideoStandard();
  // Die zwei Zeilen der Einblendung waehrend der Suche: was gerade laeuft, und
  // warum. Der Grund steht in `colourDoubt_` und den Zaehlern daneben, und
  // beides ist nur hier beisammen.
  void StandardSearchText(std::string* headline, std::string* detail) const;
  // Whether the decoder has a signal, asked of the driver a few times a second
  // rather than once a frame. Each call crosses into the driver, and doing that
  // twice per frame cost over ten milliseconds of every one -- more than the
  // entire video pipeline, and enough to stop the deinterlacer being shown at
  // its proper rate.
  int PollSignalLocked();
  // Was das Einstellungsfenster ueber die Normensuche schreiben soll. Der
  // Dialog kann das nicht selbst wissen: die Suche laeuft hier, stellt die
  // Karte mehrmals um und baut den Graphen dabei nicht neu.
  SettingsWindow::StandardSearch StandardSearchDisplay() const;
  // Asking the driver for the signal state takes about ten milliseconds -- it is
  // a round trip into kernel mode -- which is most of a field period and, on the
  // render thread, a visible hitch in the deinterlacer. So a small thread of its
  // own does the asking and leaves the answer where the render thread can pick
  // it up for free.
  void StartSignalWatch();
  void StopSignalWatch();
  void UpdateSignalWatch();

  // The watcher's last reading, as it stands: -1 when nothing is measured.
  int signalLocked() const { return signalLocked_.load(std::memory_order_relaxed); }
  long signalStandard() const { return signalStandard_.load(std::memory_order_relaxed); }
  // A full round without a lock is over and the search is resting.
  bool sweptWithoutLock() const { return standardSweeps_ >= 1 && !standardPatientPass_; }
  // The standard whose colour check is decided, 0 while it is not.
  long colourCheckedStandard() const { return colourCheckedStandard_; }
  // What the passive re-check says about the settled standard, empty while it
  // has nothing to say. The app shows it as a notice with two answers: search
  // (true) or leave it be (false).
  const std::string& colourNotice() const { return colourNotice_; }
  void AnswerColourNotice(bool search);
  // How long a manual search's result stays up, and how long it has been up.
  static double ResultSeconds();
  double ResultAgeSeconds() const;
  // Back to the first candidate, for a switch from a fixed standard back to
  // automatic.
  void StartOver();
  // Anderer Eingang oder neu initialisierte Karte: was die Suche ueber die
  // alte Quelle wusste, gilt nicht mehr. Die Uhr des Signalwaechters bleibt.
  void SourceChanged();

 private:
  void ResetStandardColourCheck();
  // Die zweite Stufe dahinter: ob die eingerastete Norm auch den richtigen
  // Farbtraeger hat. Der Lock kann das nicht sagen, ein graues Bild schon --
  // die Begruendung steht bei der Umsetzung.
  void VerifyStandardColour(int64_t now);
  // Das Ergebnis eines von Hand ausgeloesten Suchlaufs festhalten, damit es
  // ein paar Sekunden im Bild stehen kann. Tut nichts, wenn keiner lief.
  void FinishManualStandardSearch(long standard, const std::string& detail);
  // Ob gerade so ein Ergebnis angezeigt wird.
  bool ShowingStandardResult() const;

  const Config& config_;
  VideoCapture& capture_;
  VideoRenderer& renderer_;
  Host& host_;

  // Automatic video standard. Only the timestamps live here; what the card can
  // do is asked of the card each time, because it is the card that knows.
  int standardCandidate_ = -1;      // index into the candidate list, -1 = not searching
  // Wie lang die Liste beim letzten Durchgang war. Nur fuer die Einblendung --
  // "3 von 8" statt drei laufender Punkte --, deshalb hier mitgeschrieben
  // statt neu berechnet: die Liste haengt an den Faehigkeiten der Karte und
  // der Wohnregion, und die je Bild noch einmal zusammenzustellen waere Arbeit
  // fuer eine Zahl.
  int standardCandidateCount_ = 0;
  int64_t standardLostQpc_ = 0;     // when the lock was first missing
  int64_t standardNextTryQpc_ = 0;  // not before this
  // Wann die aktuell probierte Norm gesetzt wurde. Nur zum Messen: daraus wird
  // im Log die Zeit, die der Decoder zum Einfangen gebraucht hat, und die ist
  // der einzige Weg, die Wartefrist zu begruenden statt zu raten.
  int64_t standardSetQpc_ = 0;
  int standardSweeps_ = 0;          // completed passes through the list without a lock
  // Welcher der beiden Durchgaenge gerade laeuft. Falsch heisst: der schnelle,
  // in dem jeder Kandidat nur kStandardFastSeconds bekommt. Kippt am Ende eines
  // erfolglosen schnellen Durchgangs auf wahr und mit der Pause danach zurueck,
  // so dass jeder Anlauf mit dem billigen Durchgang beginnt. Siehe
  // kStandardFastSeconds fuer die Begruendung.
  bool standardPatientPass_ = false;
  // Seit wann gar kein Bild mehr ankommt, 0 = es kommt eines. Trennt die
  // ausgeschaltete Quelle (leere Bilder kommen weiter an) von der Norm, die
  // das anliegende Signal nicht dekodieren kann (es kommt nichts mehr) --
  // siehe UpdateVideoStandard. Das Logbuch dazu nur einmal je Aussetzer.
  int64_t standardStarvedSinceQpc_ = 0;
  bool standardStarvedLogged_ = false;
  long standardLastGood_ = 0;       // die zuletzt eingerastete Norm, 0 = noch keine

  // Farbpruefung. Siehe VerifyStandardColour.
  long colourCheckedStandard_ = 0;  // fuer diese Norm ist die Sache entschieden
  // Woran der Rundgang haengt. Steht nur fuer die Einblendung hier: sie soll
  // den gemessenen Grund nennen, und der ist an der Stelle bekannt, an der der
  // Rundgang beginnt, nicht mehr an der, an der gezeichnet wird.
  enum class ColourDoubt {
    None,    // kein Rundgang
    Pale,    // zu wenig Farbe fuer eine Norm, die stimmen koennte
    Tinted,    // Farbe da, aber sie steht auch im Schwarzen
    Flipping,  // Farbe da, aber sie kippt von Zeile zu Zeile
    Manual,    // von Hand ausgeloest, ohne dass etwas dagegen sprach
  };
  ColourDoubt colourDoubt_ = ColourDoubt::None;
  // Die Nachkontrolle einer entschiedenen Norm. Siehe kChromaPale.
  //
  // Bewiesen heisst: kraeftige Farbe, neutrale Tiefen, kein Kippen -- einmal
  // gemessen. Nur eine unbewiesene Norm darf fuer blasses Bild verdaechtigt
  // werden; eine bewiesene zeigt auf einem blassen Bild einfach ein blasses
  // Bild.
  bool colourProven_ = false;
  enum class ColourWatch {
    Watching,  // liest mit, darf die Pruefung einmal neu eroeffnen
    Reopened,  // hat das getan, darf nur noch hinweisen
    Notice,    // der Hinweis steht
    Done,      // nichts mehr zu tun, bis zum naechsten Lock
  };
  ColourWatch colourWatch_ = ColourWatch::Watching;
  long colourWatchStandard_ = 0;  // die Norm, deren Fenster gerade gelesen wird
  int colourWatchStrikes_ = 0;    // verdaechtige Fenster in Folge
  std::string colourNotice_;
  // Bis zu diesem Zeitpunkt wird der Rundgang gegangen, auch wenn die
  // anliegende Norm kraeftig Farbe zeigt. Setzt RescanVideoStandard.
  //
  // Eine Frist und kein Schalter, weil der Wunsch verfallen koennen muss. Wer
  // die Taste auf einem schwarzen Bild drueckt, bekommt keinen Rundgang -- auf
  // Schwarz ist keine Norm zu erkennen, es wird gewartet --, und ein Schalter
  // stuende dann fuer immer. Zwei Minuten spaeter faengt das Bild ploetzlich an
  // durchzuschalten, und niemand weiss mehr warum.
  int64_t standardForceColourUntilQpc_ = 0;
  // Ob gerade ein von Hand ausgeloester Suchlauf laeuft, und was er ergeben
  // hat.
  //
  // Eine Suche, die von selbst anspringt, schuldet niemandem eine Antwort --
  // sie beantwortet eine Frage, die keiner gestellt hat, und wenn sie fertig
  // ist, steht das Ergebnis im Bild. Ein Tastendruck ist eine Frage, und der
  // gehoert eine Antwort: die Einblendung sagt, was gesucht wird, und
  // verschwand bisher wortlos, sobald es gefunden war. Wer die Taste drueckt,
  // weil ihm die Farben nicht gefallen, erfaehrt so nicht einmal, ob sich
  // etwas geaendert hat.
  //
  // Die Fahne haelt nur bis zur ersten Auskunft. Danach ist die Frage
  // beantwortet, und was die Automatik spaeter noch entscheidet, gehoert
  // wieder ihr -- inklusive des Toasts, den die Antwort hier so lange
  // vertritt.
  bool standardManualSearch_ = false;
  int64_t standardResultUntilQpc_ = 0;
  std::string standardResultHeadline_;
  std::string standardResultDetail_;
  // Der Rundgang durch die Normen derselben Zeilenzahl. Leer, solange keiner
  // laeuft; sonst die Kandidaten, der Zeiger auf den gerade gemessenen und die
  // Messreihen dazu (-1 = keine Messung zustande gekommen).
  std::vector<long> colourCandidates_;
  std::vector<float> colourEnergies_;
  std::vector<float> colourDarks_;
  // Und wie stark die Farbe von Zeile zu Zeile umklappt, getrennt nach Achse:
  // das Kennzeichen einer Norm, deren Dekoder die Phasenumkehr des Signals
  // nicht trifft. Siehe kAltFlipping in App::VerifyStandardColour.
  std::vector<float> colourAltV_;
  std::vector<float> colourAltU_;
  int colourIndex_ = 0;
  int64_t colourSettleUntilQpc_ = 0;    // bis dahin gehoeren die Bilder noch der alten
  int64_t colourStartedQpc_ = 0;        // seit wann auf eine Messung gewartet wird
  float colourWindowEnergy_ = -1.0f;    // das vorige Messfenster desselben Kandidaten,
  float colourWindowDark_ = -1.0f;      // zum Vergleich; -1 heisst "noch keins"
  int64_t colourRetryQpc_ = 0;          // vor diesem Zeitpunkt nicht noch einmal
  int colourAttempts_ = 0;              // unentschiedene Anlaeufe fuer diese Norm
  // Ob gerade auf ein Bild gewartet wird, das hell genug zum Vergleichen ist.
  // Nur damit die Zeile darueber einmal im Log steht statt sechzig Mal je
  // Sekunde; entschieden wird jedes Bild neu.
  bool colourWaitingForPicture_ = false;
  std::thread signalWatch_;
  std::atomic<bool> signalWatchRun_{false};
  std::atomic<int> signalLocked_{-1};
  // Die Norm, die der Wachthread im selben Atemzug wie den Lock von der Karte
  // gelesen hat. Sie steht hier und nicht in `capabilities_`, weil sie sich
  // waehrend der Suche mehrmals aendert, ohne dass der Graph neu gebaut wird --
  // und weil die beiden Angaben, aus derselben Abfrage genommen, gar nicht mehr
  // auseinanderlaufen koennen.
  std::atomic<long> signalStandard_{0};
  // Counts up every time the watcher stores a reading. The automatic search
  // notes it down when it changes the standard and then ignores anything older:
  // otherwise it judges the new standard by a measurement taken before it was
  // set, and settles on whichever one happened to be tried when a stale "locked"
  // came through.
  std::atomic<uint32_t> signalSeq_{0};
  uint32_t standardSeqAtSet_ = 0;
};

}  // namespace cap
