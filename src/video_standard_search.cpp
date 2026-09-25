#include "video_standard_search.h"

#include "capture/video_capture.h"
#include "capture/video_standard.h"
#include "i18n.h"
#include "render/video_renderer.h"

namespace cap {

VideoStandardSearch::VideoStandardSearch(const Config& config, VideoCapture& capture,
                                         VideoRenderer& renderer, Host& host)
    : config_(config), capture_(capture), renderer_(renderer), host_(host) {}

// How long the lock has to be missing before anything is changed, and how long
// a freshly set standard is given to prove itself. Both err on the generous
// side: a console being switched between 50 and 60 Hz drops out for a moment on
// its own, and reacting to that would mean changing the card's setting every
// time somebody opens a menu.
static const double kStandardLostSeconds = 1.5;
// Gemessen am 30.08.2026 an einer SA7160, sechs echte Locks: 0,78 / 0,72 /
// 0,76 s bei frisch eingeschalteter Quelle und 0,30 / 0,39 / 0,55 s bei bereits
// anliegendem Signal. 1,5 s ist gut das Doppelte des schlechtesten Wertes --
// eng genug, dass ein voller Durchlauf von rund zwanzig auf zwoelf Sekunden
// faellt, und weit genug, dass keiner dieser Locks knapp geworden waere.
//
// Der eine Ausreisser von 2,00 s aus einer frueheren Messung ist bewusst nicht
// abgedeckt: dort lief die Konsole selbst noch hoch. Dieser Fall verliert den
// Durchlauf ohnehin, weil zu dem Zeitpunkt auch die *richtige* Norm nicht
// einrastet -- gerettet wird er nicht von einem laengeren Fenster, sondern vom
// naechsten Durchlauf.
//
// Von 1,5 auf 1,25 s heruntergesetzt, und zwar nicht, weil enger gerechnet
// wird, sondern weil das Gemessene kleiner geworden ist. In jedem der zehn
// Werte oben steckt das Warten auf zwei frische Messwerte des Wachtthreads
// mit drin, und das waren bei 250 ms Takt bis zu 0,5 s davon; bei 100 ms sind
// es bis zu 0,2 s.
//
// Hier stand zuerst 1,0 s, hergeleitet aus genau dieser Rechnung: der
// schlechteste alte Wert von 0,88 s muesste auf gut 0,6 s fallen, und 1,0 s
// stuende dazu wie vorher 1,5 s zu 0,88 s. Die erste Messung mit dem neuen
// Takt hat das nicht bestaetigt -- am 31.08. um 03:09:16, ein verlorener Lock
// auf PAL B und der Wechsel zurueck auf PAL 60: **0,73 s**. Gegen 1,0 s ist
// das Faktor 1,37, wo vorher 1,7 stand, und da der Poll-Anteil jetzt bis zu
// 0,2 s betraegt, liegt der schlechteste Fall dieser Art bei rund 0,83 s.
//
// 1,25 s stellt das Verhaeltnis wieder her (Faktor 1,5 auf 0,83 s) und behaelt
// den groesseren Teil des Gewinns: die Frist faellt um ein Sechstel, das
// Warten davor um bis zu 0,3 s, und die wirklich teuren Faelle nimmt ohnehin
// die Kuerzung bei Bildmangel weiter unten weg. Eine Vorhersage durch eine
// Messung ersetzt, nicht durch eine zweite Vorhersage.
//
// Nachpruefbar bleibt es an derselben Zeile: "Video standard found
// automatically: ... (lock after %.2f s)". Bleiben diese Werte unter 0,85 s, stimmt die
// Rechnung; kommen sie in die Naehe von 1,25, gehoert die Frist zurueck auf
// 1,5.
static const double kStandardSettleSeconds = 1.25;
// Und die kurze Frist des ersten Durchgangs.
//
// Die Frist darueber deckt jeden gemessenen Lock ab, auch den einer Quelle,
// die selbst noch hochfaehrt. Das ist richtig fuer den Kandidaten, der die
// Antwort ist, und Verschwendung fuer die uebrigen: eine Norm mit der falschen
// Zeilenzahl rastet nie ein, sie sitzt die Frist nur ab. In einer Liste von
// acht Kandidaten sind das im schlechtesten Fall zehn Sekunden, von denen neun
// auf Normen entfallen, die von der ersten Zehntelsekunde an widerlegt sind.
//
// Also zwei Durchgaenge. Der erste geht die Liste schnell ab und nimmt in Kauf,
// einen langsamen Lock zu verpassen; findet er nichts, geht der zweite dieselbe
// Liste mit der vollen Frist. Verworfen wird dabei nie etwas endgueltig -- was
// der schnelle Durchgang liegen laesst, bekommt der geduldige --, und der
// schlechteste Fall waechst nur um die Laenge des schnellen Durchgangs.
//
// Der Wert kommt aus derselben Messreihe wie kStandardSettleSeconds. Sechs
// echte Locks: 0,78 / 0,72 / 0,76 s bei frisch eingeschalteter Quelle, 0,30 /
// 0,39 / 0,55 s bei bereits anliegendem Signal, dazu 0,73 s bei einem
// verlorenen Lock. Die Trennlinie liegt nicht zufaellig zwischen diesen beiden
// Gruppen: was den Unterschied macht, ist nicht die Karte, sondern ob die
// Quelle selbst schon stabil ist.
//
// 0,6 s liegt ueber der langsamsten Messung an einem anliegenden Signal (0,55)
// und unter der schnellsten an einer hochfahrenden Quelle (0,72). Genau das
// ist die richtige Stelle: eine Quelle, die noch nicht liefert, verliert den
// ersten Durchgang ohnehin -- zu dem Zeitpunkt rastet auch die *richtige* Norm
// nicht ein, ob man ihr nun 0,6 oder 1,25 s gibt --, und sie wird vom zweiten
// eingefangen. Was der schnelle Durchgang gewinnen soll, ist der haeufigste
// Fall ueberhaupt: eine laufende Konsole, die zwischen 50 und 60 Hz umgestellt
// wurde. Dort liegt ein Signal an, und dort liegt jeder gemessene Lock unter
// 0,6 s.
//
// Nachpruefbar an "Video standard found automatically: ... (lock after %.2f s)"
// -- dieselbe Zeile wie oben. Steht davor "fast pass" und liegt der Wert
// dicht unter 0,6, ist die Grenze zu eng; landen umgekehrt Faelle mit
// anliegendem Signal regelmaessig erst im zweiten Durchgang, ebenso.
static const double kStandardFastSeconds = 0.6;
// Die vorgezogenen Kandidaten nach einem verlorenen Lock -- der Partner der
// zuletzt guten Norm und sie selbst -- bekommen deutlich mehr. Eine Konsole,
// die gerade neu startet oder von 50 auf 60 Hz umschaltet, braucht ein paar
// Sekunden, bis ueberhaupt wieder etwas Stabiles aus ihr herauskommt; mit
// 0,6 Sekunden waere man laengst weitergezogen, wenn sie so weit ist.
//
// Von 3,0 auf 2,0 s, und das ist der doppelte Sprung: die Frist selbst faellt
// um eine Sekunde, und sie faellt ganz weg, sobald gar kein Bild mehr ankommt
// -- siehe die Kuerzung in UpdateVideoStandard. Was bleibt, ist der Fall, fuer
// den sie gedacht war: es kommen Bilder, sie rasten nur noch nicht ein. Zwei
// Sekunden sind auch dann noch das Doppelte einer gewoehnlichen Frist, und die
// Konsole, die laenger braucht, wird ohnehin erst von der naechsten Runde
// eingefangen.
static const double kStandardPreferredSeconds = 2.0;
// After a full pass with nothing locking, there is probably no signal at all --
// the console is off. Stop poking the card and look again in a while.
static const double kStandardBackoffSeconds = 6.0;
// Ab wann "es kommt kein Bild mehr" heisst, dass die eingestellte Norm das
// anliegende Signal nicht dekodieren kann. Grosszuegig gegen den Neuaufbau
// des Graphen gewaehlt: waehrenddessen kommt naturgemaess nichts, und ein
// frisch gestarteter Graph liefert an der SA7160 nach rund 0,3 s wieder --
// gemessen am 31.08. um 02:29:37, Neubau in 118 ms. Zwei Sekunden sind das
// Sechsfache davon und trotzdem kurz genug, dass ein wieder angestecktes Kabel
// nicht sekundenlang ins Leere laeuft.
static const double kStandardStarvedSeconds = 2.0;

// Wie lange das Ergebnis eines von Hand ausgeloesten Suchlaufs stehen bleibt.
//
// Es sind zwei Zeilen, und die zweite ist die, um derentwillen es angezeigt
// wird -- fuenf Sekunden reichen, sie in Ruhe zu lesen, ohne dass ein Ergebnis
// laenger im Bild steht als die Suche gedauert hat, die es hervorgebracht hat.
static const double kStandardResultSeconds = 5.0;

// Bis die Karte nach einem Normwechsel wieder saubere Bilder liefert. Die
// Zeilenzahl bleibt gleich, es geht nur um den Farb-PLL, deshalb kurz.
//
// Dass die Frist ueberhaupt etwas tut, ist am 30.08. nachgestellt worden,
// indem sie auf null gesetzt wurde: dann misst jeder Kandidat den
// Umschaltmoment mit und wird zu seinem Vorgaenger hin verschmiert -- PAL N
// 0,047 und 0,064 statt 0,012 bis 0,021, SECAM B in den Tiefen 0,246 statt
// 0,271 bis 0,410. Wie weit sie darueber hinaus Reserve hat, ist nicht
// gemessen.
//
// Sie war frueher gratis: der Rundgang wartet nach einem Normwechsel ohnehin
// auf zwei frische Messwerte des Wachtthreads, und bei 250 ms Takt dauerte das
// ungefaehr ebenso lange. Seit der Takt bei 100 ms liegt -- siehe
// kSignalPollNaps -- ist sie es nicht mehr, sondern war der laengste
// Einzelposten eines Kandidaten.
//
// Sie stand deshalb bei 0,5 s und ist am 31.08. auf 0,10 s gekuerzt worden,
// zusammen mit einer Aenderung, die ihr die eigentliche Arbeit abnimmt: der
// Rundgang wartet jetzt nicht mehr auf die Uhr, sondern darauf, dass die
// Messung nicht mehr steigt (siehe kColourStableGiveUpSeconds und
// VerifyStandardColour). Was hier stehenbleibt, ist nur noch der Boden -- die
// paar Bilder, die der Treiber unmittelbar nach put_TVFormat noch aus der
// alten Einstellung liefert. Wie lange der Farb-PLL darueber hinaus braucht,
// wird nicht mehr geschaetzt, sondern je Kandidat gemessen; PAL 60 braucht
// dreimal so lange wie NTSC 4.43, und das ist genau die Sorte Unterschied, die
// eine gemeinsame Frist nicht abbilden kann.
//
// Sie steht hier statt in VerifyStandardColour, weil sie inzwischen von zwei
// Seiten gebraucht wird: auch der Neubau des Graphen nach einem gefundenen
// Lock legt sie an, damit die erste Messung der neuen Norm hinter dem Umbau
// beginnt statt darueber hinweg zu mitteln. Siehe UpdateVideoStandard.
static const double kColourSettleSeconds = 0.10;

// Wie lange ein Kandidat hoechstens gemessen wird, wenn seine Zahl nicht zur
// Ruhe kommt. Siehe VerifyStandardColour.
//
// PAL 60 -- die langsamste gemessene Norm auf dieser Karte -- ist nach rund
// 0,8 s so weit. Die Obergrenze liegt darueber mit Luft, aber unter dem, was
// vier Kandidaten in Serie ertraeglich machen. Sie ist keine Frist, auf die
// gewartet wird, sondern eine, die nur bei einem bewegten Bild ueberhaupt
// erreicht wird -- und dann steht im Log, dass sie es war.
static const double kColourStableGiveUpSeconds = 1.2;

// Ob eine Messung gegenueber der vorigen noch deutlich gestiegen ist.
//
// Der absolute Anteil faengt die Werte nahe null, wo ein Verhaeltnis nichts
// mehr aussagt: von 0,002 auf 0,004 ist eine Verdopplung und trotzdem grau.
// Der relative faengt die grossen, wo ein fester Abstand zu streng waere.
// Nachgerechnet an der Einschwingkurve von PAL 60: 0,095 auf 0,119 gilt als
// gestiegen (Schwelle 0,014), 0,119 auf 0,119 nicht.
static bool ColourStillRising(float previous, float current) {
  if (previous < 0.0f || current < 0.0f) return false;
  const float tol = 0.008f > previous * 0.15f ? 0.008f : previous * 0.15f;
  return current > previous + tol;
}

// Wie oft der Wachtthread den Decoder nach seinem Lock fragt, in Zehnteln
// einer Zehntelsekunde -- er schlaeft in 10-ms-Haeppchen, damit das Beenden
// nicht darauf warten muss.
//
// Das ist der Boden unter allen Fristen dieser Datei, und er war lange
// unsichtbar. Bevor ueber eine frisch gesetzte Norm geurteilt werden darf,
// wartet `UpdateVideoStandard` auf zwei frische Messwerte -- bei 250 ms Takt
// sind das 0,25 bis 0,5 s, in denen nichts gemessen wird, sondern nur gewartet.
// Genau diese Spanne steckt in jedem gemessenen "lock after"-Wert mit drin:
// zehn davon aus drei Sitzungen liegen zwischen 0,32 und 0,88 s, und ein
// gutes Drittel davon ist dieses Warten.
//
// Bei 100 ms schrumpft es auf 0,1 bis 0,2 s. Das ist die Voraussetzung dafuer,
// dass die Fristen darunter kuerzer werden koennen, ohne enger zu werden --
// gekuerzt wird das Warten, nicht die Messung. Der Preis sind zwei
// Property-Gets auf dem Decoder zehnmal statt viermal je Sekunde, auf einem
// eigenen Thread, weit weg vom Bildweg.
//
// Der Rundgang profitiert genauso: auch er wartet je Kandidat auf zwei frische
// Messwerte, fuenfmal je Runde.
static const int kSignalPollNaps = 10;

void VideoStandardSearch::StartSignalWatch() {
  StopSignalWatch();
  // The thread keeps its own handle, so tearing the capture down does not pull
  // the card out from under it; the worst that happens is that it asks a card
  // that is no longer running, and gets told so.
  std::shared_ptr<SignalProbe> probe = capture_.signalProbe();
  if (!probe) return;

  signalWatchRun_.store(true, std::memory_order_relaxed);
  signalWatch_ = StartCaptureThread([this, probe]() {
    while (signalWatchRun_.load(std::memory_order_relaxed)) {
      // Beides aus demselben Durchgang: was eingestellt ist und ob es haelt.
      // Getrennt gefragt koennten die zwei aus verschiedenen Momenten stammen,
      // und genau daraus entsteht die Anzeige, die eine Norm als eingerastet
      // meldet, waehrend laengst eine andere auf der Karte steht.
      signalStandard_.store(probe->CurrentStandard(), std::memory_order_relaxed);
      signalLocked_.store(probe->Locked(), std::memory_order_relaxed);
      signalSeq_.fetch_add(1, std::memory_order_release);
      // Split into short naps so shutdown does not have to wait for the interval.
      for (int i = 0; i < kSignalPollNaps && signalWatchRun_.load(std::memory_order_relaxed);
           ++i) {
        SleepMilliseconds(10);
      }
    }
  });
}

void VideoStandardSearch::StopSignalWatch() {
  signalWatchRun_.store(false, std::memory_order_relaxed);
  if (signalWatch_.joinable()) signalWatch_.join();
  signalLocked_.store(-1, std::memory_order_relaxed);
  signalStandard_.store(0, std::memory_order_relaxed);
}

int VideoStandardSearch::PollSignalLocked() {
  if (!capture_.running()) return -1;
  return signalLocked_.load(std::memory_order_relaxed);
}

SettingsWindow::StandardSearch VideoStandardSearch::StandardSearchDisplay() const {
  using S = SettingsWindow::StandardSearch;
  // Ohne laufende Aufnahme oder an einer digitalen Quelle gibt es keine Suche,
  // und der Wachthread laeuft dann auch gar nicht.
  if (!host_.CaptureRunning() || !host_.SourceIsAnalogue()) return S::Off;
  // Eine von Hand gesetzte Norm wird nicht gesucht. Sie ist eingestellt, auch
  // wenn nichts einrastet -- dass sie nicht haelt, sagt die Zeile darunter.
  if (config_.active().capture.videoStandard != -1) return S::Off;

  const int locked = signalLocked_.load(std::memory_order_relaxed);
  if (locked < 0) return S::Off;  // noch nichts gemessen
  if (locked == 1) {
    // Eingerastet -- aber die Sache ist erst entschieden, wenn auch die Farbe
    // stimmt. Waehrend des Gegenversuchs steht die *andere* Norm auf der Karte,
    // und ohne diesen Zustand saehe das aus wie ein Ergebnis, das sich von
    // selbst wieder aendert.
    if (!colourCandidates_.empty()) return S::Colour;
    // Von Hand ausgeloest und noch kein Rundgang: gemessen wird trotzdem
    // schon. Der Rundgang beginnt erst, wenn eine Messung vorliegt, und auf
    // einem dunklen Bild kann das dauern -- ohne diese Zeile waere zwischen
    // Tastendruck und Ergebnis eine halbe Minute, in der nichts zu sehen ist
    // und der Tastendruck wie verschluckt aussieht.
    if (standardManualSearch_) return S::Colour;
    if (ShowingStandardResult()) return S::Result;
    return S::Off;
  }
  // Kein Lock: gesucht wird -- es sei denn, gerade laeuft die Pause zwischen
  // zwei Runden.
  //
  // Hier stand nur `standardSweeps_ >= 1`, und das war die falsche Frage. Nach
  // der ersten erfolglosen Runde geht der Zaehler nie wieder auf null, die
  // Zeile blieb also fuer immer bei "Suche pausiert" -- auch waehrend der
  // zweiten, dritten, zehnten Runde, in der sehr wohl gesucht wird. Sichtbar
  // wurde es daran, dass die genannte Norm munter weiterlief, waehrend
  // danebenstand, es sei pausiert: am 30.08. um 13:31 Uhr zeigte der Dialog
  // "Scanning paused, card set to PAL M", und PAL M war Platz 6 einer gerade
  // laufenden Runde. Die Pause ist der Zustand *zwischen* den Runden, und den
  // erkennt man nicht am Zaehler, sondern daran, dass gerade kein Kandidat
  // gesetzt ist.
  //
  // Seit es zwei Durchgaenge gibt, reicht auch das nicht mehr ganz: zwischen
  // dem schnellen und dem geduldigen steht ebenfalls kein Kandidat, und dort
  // wird nicht pausiert, sondern sofort weitergemacht. Der Unterschied ist
  // `standardPatientPass_` -- die Fahne steht in dem Moment schon auf wahr, in
  // dem der geduldige Durchgang beginnt, und faellt erst mit seinem Ende
  // zusammen mit dem Beginn der Pause zurueck. Ohne das haette die Zeile fuer
  // ein Bild lang "Suche pausiert" gezeigt, mitten in der Suche.
  return standardSweeps_ >= 1 && standardCandidate_ < 0 && !standardPatientPass_ ? S::Paused
                                                                                 : S::Trying;
}

// Die beiden Zeilen der Einblendung: was laeuft, und warum es laeuft.
//
// Bis hierher standen dort drei laufende Punkte hinter einem Normnamen, und
// das ist genau die Auskunft, die schon zu sehen war -- das Bild schaltet
// sichtbar durch mehrere Normen. Was nicht zu sehen ist und den Vorgang erst
// erklaert, ist der Grund: dass die Farbe zu blass war fuer eine Norm, die
// stimmen koennte, oder dass Farbe bis ins Schwarze steht, oder dass die Karte
// gar nicht erst einrastet. Der Grund ist gemessen, er liegt vor, und er
// gehoert dorthin, wo der Vorgang zu sehen ist.
//
// Dazu der Zaehler. Eine Suche ohne Zaehler ist eine Suche ohne Ende -- man
// kann ihr nicht ansehen, ob sie in einer Sekunde fertig ist oder in zehn --,
// und der Zaehler ist der Unterschied zwischen Warten und Zusehen.
void VideoStandardSearch::StandardSearchText(std::string* headline, std::string* detail) const {
  const long shown = capture_.running() ? signalStandard_.load(std::memory_order_relaxed) : 0;
  const int idx = shown != 0 ? VideoStandardIndexOf(shown) : -1;
  const char* name = idx >= 0 ? VideoStandardName(idx) : "?";

  // Fertig, und jemand wartet auf die Antwort. Sie steht fertig da: geschrieben
  // wurde sie an der Stelle, an der die Suche geendet hat, weil nur die weiss,
  // woran sie geendet ist.
  if (ShowingStandardResult()) {
    *headline = standardResultHeadline_;
    *detail = standardResultDetail_;
    return;
  }

  if (!colourCandidates_.empty()) {
    // Der Rundgang misst einen Schritt mehr, als er Normen hat: die
    // Ausgangsnorm kommt am Ende ein zweites Mal dran, siehe
    // VerifyStandardColour. Gezaehlt wird deshalb in Schritten und nicht in
    // Normen -- sonst stuende "5 von 4" im Bild --, und die Klammer in der
    // Zeile darunter sagt, woher der zusaetzliche Schritt kommt.
    const int steps = (int)colourCandidates_.size();
    const int done = colourIndex_ + 1 < steps ? colourIndex_ + 1 : steps;
    *headline = Format(T("Farbe wird geprüft: %s (%d/%d)", "Checking colour: %s (%d/%d)"), name,
                       done, steps);

    const int lines = shown != 0 ? VideoStandardLines(shown) : 0;
    const int norms = steps - 1;
    switch (colourDoubt_) {
      case ColourDoubt::Tinted:
        *detail = Format(
            T("Farbe steht bis ins Schwarze — die %d Normen mit %d Zeilen werden verglichen "
              "(die erste zweimal)",
              "Colour reaches into the blacks — comparing the %d standards with %d lines "
              "(the first one twice)"),
            norms, lines);
        break;
      case ColourDoubt::Flipping:
        *detail = Format(
            T("Farbe kippt von Zeile zu Zeile — die %d Normen mit %d Zeilen werden verglichen "
              "(die erste zweimal)",
              "Colour flips from line to line — comparing the %d standards with %d lines "
              "(the first one twice)"),
            norms, lines);
        break;
      case ColourDoubt::Manual:
        *detail = Format(
            T("Von Hand ausgelöst — die %d Normen mit %d Zeilen werden verglichen "
              "(die erste zweimal)",
              "Triggered by hand — comparing the %d standards with %d lines "
              "(the first one twice)"),
            norms, lines);
        break;
      default:
        *detail = Format(
            T("Farbe unklar — die %d Normen mit %d Zeilen werden verglichen "
              "(die erste zweimal)",
              "Colour unclear — comparing the %d standards with %d lines "
              "(the first one twice)"),
            norms, lines);
        break;
    }
    return;
  }

  // Von Hand ausgeloest, eingerastet, und der Rundgang hat noch nicht
  // angefangen. Dazwischen liegt eine Messung, und die braucht ein Bild, an
  // dem etwas zu messen ist -- auf Schwarz wartet sie, und das Warten ist hier
  // die ganze Auskunft.
  if (standardManualSearch_ && signalLocked_.load(std::memory_order_relaxed) == 1) {
    *headline = Format(T("Videonorm wird geprüft: %s", "Checking video standard: %s"), name);
    *detail = colourWaitingForPicture_
                  ? T("Das Bild ist zu dunkel für einen Vergleich — es wird gewartet",
                      "The picture is too dark to compare — waiting")
                  : T("Die Farbe wird gemessen", "Measuring the colour");
    return;
  }

  // Stufe eins. Hier steht kein gemessener Grund zur Auswahl -- es gibt genau
  // einen, naemlich dass der Decoder die Zeilenfrequenz nicht findet --, und
  // der Zaehler ist die eigentliche Auskunft.
  if (standardCandidate_ >= 0 && standardCandidateCount_ > 0) {
    *headline = Format(T("Videonorm wird gesucht: %s (%d/%d)", "Scanning video standard: %s (%d/%d)"),
                       name, standardCandidate_ + 1, standardCandidateCount_);
  } else {
    *headline = Format(T("Videonorm wird gesucht: %s", "Scanning video standard: %s"), name);
  }
  // Dazu, welcher der beiden Durchgaenge laeuft. Das ist keine Kleinigkeit
  // fuer den, der zusieht: derselbe Normname taucht ein zweites Mal auf, und
  // ohne diese Zeile sieht das aus, als drehe sich die Suche im Kreis. Die
  // Frist steht mit dabei, weil sie der ganze Unterschied ist.
  *detail = standardPatientPass_
                ? Format(T("Karte rastet nicht ein — zweiter Durchgang, %.2f s je Norm",
                           "Card does not lock — second pass, %.2f s per standard"),
                         kStandardSettleSeconds)
                : Format(T("Karte rastet nicht ein — erster Durchgang, %.1f s je Norm",
                           "Card does not lock — first pass, %.1f s per standard"),
                         kStandardFastSeconds);
}

// Die Normensuche von Hand ausloesen.
//
// Die Automatik hat einen blinden Fleck, und es ist kein kleiner: sie greift,
// wenn der Lock verloren geht oder wenn die Farbe messbar nicht stimmt. Eine
// Norm, die haelt und kraeftig Farbe zeigt, zweifelt sie nicht an -- auch dann
// nicht, wenn die Farben schlicht falsch sind. Genau das sieht aber ein Mensch
// und keine der beiden Messungen: Gras in der falschen Farbe ist Farbe.
//
// Der Umweg dafuer war bisher, im Normwaehler irgendeine feste Norm zu setzen
// und wieder auf Automatisch zu stellen. Das funktioniert und findet niemand,
// der es nicht schon weiss.
void VideoStandardSearch::RescanVideoStandard() {
  if (!host_.CaptureRunning()) {
    host_.Toast(T("Keine laufende Quelle.", "No source is running."));
    return;
  }
  if (!host_.SourceIsAnalogue()) {
    host_.Toast(T("Nur analoge Eingänge haben eine Videonorm.",
            "Only analogue inputs have a video standard."));
    return;
  }
  if (config_.active().capture.videoStandard != -1) {
    // Die feste Norm bleibt fest. Sie ist eine Entscheidung, die jemand
    // getroffen hat, und eine Taste, die sie im Vorbeigehen umwirft, waere
    // eine Ueberraschung -- gesagt wird es trotzdem, sonst sieht der
    // Tastendruck wie ein Fehler aus.
    host_.Toast(T("Die Videonorm steht fest — für die Suche auf Automatisch stellen.",
            "The video standard is fixed — set it to Automatic to search."));
    return;
  }

  CAP_LOG("Video standard: search started by hand");
  // Stufe eins von vorn. Auch die Runden zurueck auf null: wer von Hand sucht,
  // will keine Pause, in der nichts geschieht. Und von vorn heisst mit dem
  // schnellen Durchgang: von Hand gesucht wird an einer Quelle, die laeuft und
  // ein Bild liefert -- genau der Fall, fuer den die kurze Frist gemessen ist.
  standardCandidate_ = -1;
  standardLostQpc_ = 0;
  standardSweeps_ = 0;
  standardNextTryQpc_ = 0;
  standardPatientPass_ = false;
  // Und Stufe zwei von vorn, mitsamt dem Rundgang, den die Abkuerzung sonst
  // ueberspringt. Die Frist ist grosszuegig: auf einem gerade schwarzen Bild
  // wartet der Rundgang, und dieses Warten soll er noch tun duerfen.
  ResetStandardColourCheck();
  standardForceColourUntilQpc_ = ClockTicks() + SecondsToTicks(30.0);
  // Ab hier schuldet die Suche eine Antwort, und ein Ergebnis von vorhin ist
  // keine: wer ein zweites Mal drueckt, fragt ein zweites Mal.
  standardManualSearch_ = true;
  standardResultUntilQpc_ = 0;
  host_.Toast(T("Videonorm wird gesucht", "Scanning for the video standard"));
}

// Die Antwort auf den Hinweis der Nachkontrolle. Suchen ist dieselbe Suche wie
// von Hand -- sie setzt dabei auch die Nachkontrolle zurueck, und der Hinweis
// verschwindet mit. Ignorieren gilt bis zum naechsten Lock.
void VideoStandardSearch::AnswerColourNotice(bool search) {
  if (colourNotice_.empty()) return;
  colourNotice_.clear();
  colourWatch_ = ColourWatch::Done;
  if (search) {
    RescanVideoStandard();
    return;
  }
  CAP_LOG("Video standard: colour hint dismissed");
}

// Die Antwort auf den Tastendruck festhalten.
//
// Aufgerufen wird das an jedem Ausgang, den ein Suchlauf nehmen kann -- der
// berichtigten Norm, der bestaetigten, der unentschiedenen Runde, dem zu
// dunklen Bild, der Quelle ohne Signal. Es sind fuenf, und dass es fuenf sind,
// ist der Grund fuer die Fahne: welcher Ausgang genommen wird, entscheidet
// sich weit weg von der Taste, und keiner von ihnen weiss, ob ihn jemand
// abwartet.
//
// Die erste Auskunft gewinnt. Danach ist die Frage beantwortet, und alles
// Weitere -- ein Wiederholer, der zehn Sekunden spaeter doch noch entscheidet
// -- gehoert wieder der Automatik samt ihrem Toast.
void VideoStandardSearch::FinishManualStandardSearch(long standard, const std::string& detail) {
  if (!standardManualSearch_) return;
  standardManualSearch_ = false;
  const int idx = standard != 0 ? VideoStandardIndexOf(standard) : -1;
  standardResultHeadline_ = Format(T("Videonorm: %s", "Video standard: %s"),
                                   idx >= 0 ? VideoStandardName(idx) : "?");
  standardResultDetail_ = detail;
  standardResultUntilQpc_ = ClockTicks() + SecondsToTicks(kStandardResultSeconds);
  CAP_LOG("Video standard: manual search finished -- %s",
          idx >= 0 ? VideoStandardName(idx) : "?");
}

// Ob das Ergebnis gerade im Bild steht.
//
// Und nur, solange sonst nichts laeuft. Geht der Lock in diesen fuenf Sekunden
// verloren, faengt die Automatik von vorn an -- dann ist das Ergebnis nicht
// mehr wahr, und es tritt hinter die laufende Suche zurueck, statt sie zu
// verdecken.
bool VideoStandardSearch::ShowingStandardResult() const {
  return standardResultUntilQpc_ != 0 && ClockTicks() < standardResultUntilQpc_ &&
         colourCandidates_.empty() && standardCandidate_ < 0;
}

// Ob der Wachthread ueberhaupt etwas zu beobachten hat.
//
// Er fragt den Analogdecoder zehnmal in der Sekunde nach seinem Lock. An einem
// digitalen Eingang haengt der Decoder gar nicht im Signalweg -- seine Antwort
// ist dort bedeutungslos, und alles, was auf ihr aufbaut, soll sie deshalb
// auch nicht bekommen: weder die automatische Normensuche noch die kuerzere
// Geduld bei flachem Bild in `HoldingSignal`.
//
// Es ist dieselbe Frage, die den Videonorm-Picker aus den Einstellungen und
// aus dem Rechtsklickmenue nimmt. Was nirgends einstellbar ist, darf auch
// nicht im Hintergrund an der Karte drehen.
void VideoStandardSearch::UpdateSignalWatch() {
  const bool want = host_.CaptureRunning() && host_.SourceIsAnalogue();
  const bool have = signalWatch_.joinable();
  if (want == have) return;

  if (want) {
    StartSignalWatch();
    return;
  }
  StopSignalWatch();
  SourceChanged();
}

void VideoStandardSearch::SourceChanged() {
  // Eine halb gelaufene Suche darf nicht liegen bleiben. Kommt spaeter doch
  // eine analoge Quelle, faengt sie von vorne an statt in der Mitte der Liste.
  standardCandidate_ = -1;
  standardLostQpc_ = 0;
  standardSweeps_ = 0;
  standardNextTryQpc_ = 0;
  standardPatientPass_ = false;
  // Und die Frage von Hand verfaellt mit der Quelle, an die sie gestellt war.
  standardManualSearch_ = false;
  standardResultUntilQpc_ = 0;
  // Auch die Erinnerung. Wer von digital zurueck auf analog wechselt, haengt
  // etwas anderes an -- die alte Norm zu bevorzugen waere dann ein Rat aus
  // einem anderen Leben.
  standardLastGood_ = 0;
  ResetStandardColourCheck();
}

void VideoStandardSearch::UpdateVideoStandard() {
  const Profile& profile = config_.active();
  if (profile.capture.videoStandard != -1) return;  // not our job
  // Und auch dann nicht, solange die Umstellung noch nicht angekommen ist.
  //
  // Das Einstellungsfenster schreibt beim Klick sofort in `config_`, aber
  // `SyncConfigChanges` -- und damit der Ruecksetzer der Suchposition -- laeuft
  // erst am Ende des naechsten Bildes des *Hauptfensters*. In `Tick` steht
  // diese Funktion davor. Wer also von einer festen Norm auf Automatisch
  // zurueckstellt, kaeme genau einmal hier durch, waehrend `standardCandidate_`
  // noch auf dem alten Stand steht: die Suche schriebe der Karte den naechsten
  // Kandidaten der *alten* Position, und erst danach faengt sie richtig von
  // vorne an. Am 30.08. um 13:32:56 so im Log gestanden -- `PAL N` (Platz 6 der
  // abgebrochenen Runde), 679 ms spaeter dann `PAL B` (Platz 0).
  if (host_.AppliedVideoStandard() != profile.capture.videoStandard) return;
  if (!host_.CaptureRunning()) return;
  // Ein digitaler Eingang hat keine Videonorm, die man suchen koennte. Ohne
  // Wachthread stuende unten ohnehin -1 und die Funktion kehrte um; das hier
  // sagt es aber an der Stelle, an der es gemeint ist.
  if (!host_.SourceIsAnalogue()) return;

  // Einmal je Sitzung festhalten, womit die Suche anfaengt. Das ist die erste
  // Frage, wenn die Automatik danebenliegt -- und die Antwort sagt zugleich, ob
  // die Region stimmt, ohne dass die (uebersetzte) Regionsbezeichnung in dieses
  // unuebersetzte Log muesste. Der Normname ist in jeder Sprache derselbe.
  static bool searchStartLogged = false;
  if (!searchStartLogged) {
    const std::vector<long> plan = AutoStandardCandidates(
        capture_.capabilities().availableStandards, config_.app.videoRegion, 0, nullptr);
    if (!plan.empty()) {
      searchStartLogged = true;
      const char* how = "set by hand";
      if (config_.app.videoRegion == VideoRegion::Auto) how = "automatic, from Windows";
      if (config_.app.videoRegion == VideoRegion::None) how = "none, the general order only";
      CAP_LOG("Video standard search: region %s, trying %s first", how,
              VideoStandardName(VideoStandardIndexOf(plan.front())));
    }
  }

  const int64_t now = ClockTicks();

  // Kommt ueberhaupt noch ein Bild an? Vor allen Ausstiegen weiter unten, damit
  // diese Uhr auch dann laeuft, wenn die Funktion an anderer Stelle umkehrt.
  //
  // Das ist der Unterschied zwischen den beiden Arten von "kein Signal", die
  // sich sonst gleich anfuehlen. Ist die Quelle aus, liefert die Karte weiter
  // Bilder, nur eben leere -- daraus wird das Urteil Flat, und dann ist Parken
  // richtig. Steht die Karte dagegen auf einer Norm mit der falschen
  // Zeilenzahl, kommt gar nichts mehr; das letzte Urteil bleibt als Flat
  // stehen, obwohl es ein Bild von vorhin beschreibt.
  //
  // Genau das hat sich am 31.08.2026 aufgehaengt: um 02:02:46 stand die Karte
  // auf NTSC 4.43 (525/60), am Eingang lag wieder ein 625/50-Signal, und der
  // Graph lieferte kein einziges Bild mehr. Der Parkzweig war schon auf seinem
  // Ziel angekommen, tat also nichts, und kehrte um -- alle 1,5 Sekunden, 112
  // Sekunden lang, bis von Hand neu gestartet wurde. Der Abstand zur letzten
  // Ankunft stieg dabei von 633 ms auf 112352 ms, und das ist der Messwert, an
  // dem es haengt. Nicht zu verwechseln mit der Durchlaufzeit im Panel: die
  // misst eine Strecke, diese hier misst eine Stille.
  //
  // Wenn nichts mehr ankommt, ist Weitersuchen keine Stoerung, sondern das
  // Einzige, was das Bild zurueckholen kann.
  // Gemessen wird am Alter des letzten Bildes selbst, nicht daran, wann uns
  // zum ersten Mal aufgefallen ist, dass keines mehr kommt. Der Unterschied
  // sind volle kNoSignalSeconds, die sonst vor der eigentlichen Frist noch
  // einmal verstreichen -- am 31.08. um 02:29:44 standen dadurch 4,3 s im Log,
  // wo 3,0 gemeint waren. Nur solange die Senke ueberhaupt schon einmal ein
  // Bild gesehen hat; danach ist ihr Alter die Messung. Vorher -- frischer
  // Graph, noch nichts angekommen -- bleibt nur, ab dem ersten Hinsehen zu
  // zaehlen.
  const FrameBuffer* frameSink = capture_.sink();
  const double frameAgeMs = frameSink ? frameSink->stats().lastArrivalAgeMs : -1.0;
  bool starved = false;
  if (frameAgeMs >= 0.0) {
    standardStarvedSinceQpc_ = 0;
    starved = frameAgeMs >= kStandardStarvedSeconds * 1000.0;
  } else {
    if (standardStarvedSinceQpc_ == 0) standardStarvedSinceQpc_ = now;
    starved = TicksToSeconds(now - standardStarvedSinceQpc_) >= kStandardStarvedSeconds;
  }
  if (!starved) standardStarvedLogged_ = false;

  // Two fresh readings since the standard was last changed, so what is being
  // judged is the standard that is actually set.
  if (signalSeq_.load(std::memory_order_acquire) - standardSeqAtSet_ < 2) return;

  const int locked = signalLocked_.load(std::memory_order_relaxed);
  if (locked < 0) return;  // no analogue decoder, or nothing measured yet

  if (locked == 1) {
    // Settled. Whatever is set is right, and the search starts from scratch if
    // it is ever needed again.
    standardLostQpc_ = 0;
    standardSweeps_ = 0;
    standardPatientPass_ = false;
    // Woran nach dem naechsten Aussetzer zuerst gedacht wird. Auch dann
    // gemerkt, wenn gar nicht gesucht wurde: eine Norm, die von selbst
    // eingerastet ist, ist genauso ein guter Hinweis wie eine gefundene.
    standardLastGood_ = capture_.currentStandard();
    // Und der Farbtraeger dazu, weil er allein an der Norm haengt und die genau
    // hier neu ist. Der Neubau darunter setzt ihn auch, laeuft aber nur, wenn
    // eine Suche lief; rastet die Karte von sich aus auf etwas anderes ein,
    // rechneten die Composite-Filter sonst weiter mit dem Traeger der Norm
    // davor -- zwischen den beiden Gruppen sind das 24 % daneben, und der
    // Demodulator sucht dann eine Frequenz, die gar nicht da ist.
    renderer_.SetCarrierSamples(VideoStandardSubcarrierSamples(standardLastGood_));
    if (standardCandidate_ >= 0) {
      // A candidate just proved itself. The line count may have changed with
      // it, so the graph has to be rebuilt around the new format.
      standardCandidate_ = -1;
      const long settled = standardLastGood_;
      // Mit der Einfangzeit daneben. Sie sagt, ob die Wartefrist gereicht hat
      // oder ob sie nur knapp gereicht hat -- und was eine Norm braucht, die
      // die Zeilenfrequenz wirklich neu einfangen musste.
      CAP_LOG("Video standard found automatically: %s (lock after %.2f s)",
              VideoStandardName(VideoStandardIndexOf(settled)),
              standardSetQpc_ != 0 ? TicksToSeconds(now - standardSetQpc_) : 0.0);
      standardSetQpc_ = 0;
      // Die Zeilenzahl kann sich mit der Norm geaendert haben, und dann passt
      // die gemerkte Groesse nicht mehr. Vor dem Neubau, der sie sonst wieder
      // anfordert.
      host_.ReleaseStandardBoundFormat(VideoStandardLines(settled));
      std::string error;
      if (host_.StartCapture(&error)) {
        // The toast speaks the same vocabulary as the picker, so it names the
        // group. The exact variant stays in the log and under the picker.
        host_.Toast(Format(T("Videonorm: %s", "Video standard: %s"),
                     VideoStandardPickerName(settled).c_str()));
      } else {
        host_.Toast(error);
      }
      // Und die Farbmessung von vorn, hinter dem Umbau.
      //
      // Das hat gefehlt, und es war teuer. Die laufende Messung mittelt ueber
      // rund drei Sekunden; wird der Graph mittendrin neu gebaut, steht in ihr
      // die alte Norm, die alte Groesse und der Umbau selbst. Genau das ist am
      // 31.08. um 03:09 passiert: PAL 60 mass unmittelbar nach dem Wechsel von
      // 720x576 auf 720x480 eine Farbe von 0,005 -- gemessen wurde in
      // Wahrheit das Nichts davor. Damit galt die frisch gefundene, richtige
      // Norm als zweifelhaft, und es lief ein voller Rundgang ueber vier
      // Normen los, der zu allem Ueberfluss verworfen wurde, weil die zweite
      // Messung derselben Norm dann 0,258 ergab. Drei Sekunden spaeter stand
      // ohne einen einzigen Normwechsel "PAL 60 hat deutlich Farbe (0,149)"
      // im Log -- die Antwort war die ganze Zeit da, sie wurde nur zu frueh
      // gefragt. Rund sechseinhalb der elfeinhalb Sekunden zwischen
      // Signalverlust und Urteil gingen dafuer drauf.
      //
      // Die Frist darunter ist dieselbe wie im Rundgang und aus demselben
      // Grund da: sie setzt die Messung an ihrem Ende noch einmal zurueck, so
      // dass das Fenster sicher hinter dem Umbau beginnt und nicht davor.
      ResetStandardColourCheck();
      colourSettleUntilQpc_ = now + SecondsToTicks(kColourSettleSeconds);
    }
    VerifyStandardColour(now);
    return;
  }

  // No lock.
  const long available = capture_.capabilities().availableStandards;
  if (standardLostQpc_ == 0) {
    standardLostQpc_ = now;
    if (standardLastGood_ > 0) {
      int planned = 0;
      const std::vector<long> plan =
          AutoStandardCandidates(available, config_.app.videoRegion, standardLastGood_, &planned);
      if (planned > 0) {
        CAP_LOG("Video standard: lost the lock on %s, trying %s first",
                VideoStandardName(VideoStandardIndexOf(standardLastGood_)),
                VideoStandardName(VideoStandardIndexOf(plan.front())));
      }
    }
    return;
  }
  if (TicksToSeconds(now - standardLostQpc_) < kStandardLostSeconds) return;

  // Eine Frist, die auf ein Bild wartet, das nicht kommt, ist kein Zuhoeren.
  //
  // Beim Setzen des Kandidaten wird entschieden, wieviel Zeit er bekommt, und
  // an dem Punkt ist `starved` fast immer noch falsch: das letzte Bild ist
  // erst ein, zwei Sekunden alt, die Schwelle noch nicht erreicht. Die
  // Unterscheidung stand also da, wo sie nichts entscheiden konnte -- am
  // 31.08. um 02:43:39 bekam PAL B seine vollen kStandardPreferredSeconds und
  // lief sie voll aus, obwohl schon beim Setzen seit sieben Sekunden kein Bild
  // mehr angekommen war. 02:43:42,6 "nach 3,00 s ohne Lock verworfen",
  // 02:43:43,2 wieder Bild: die halbe Wartezeit war diese eine Frist.
  //
  // Also wird die Frist nachtraeglich gekuerzt, sobald das Aushungern
  // feststeht. Gekuerzt und nicht gestrichen: der Kandidat braucht seine
  // kStandardSettleSeconds, um seinen Lock ueberhaupt zeigen zu koennen, und
  // dass gerade kein Bild ankommt, sagt darueber nichts -- eine Norm mit der
  // anderen Zeilenzahl liefert grundsaetzlich nichts in einen Graphen der
  // alten Groesse, und trotzdem kann genau sie die richtige sein. Was hier
  // wegfaellt, ist allein die zusaetzliche Geduld der vorgezogenen Plaetze.
  // Die ist fuer eine Konsole gedacht, die noch hochfaehrt, und die liefert
  // dabei Bilder -- nur noch keine stabilen.
  if (starved && standardCandidate_ >= 0 && standardSetQpc_ != 0) {
    const int64_t shortened = standardSetQpc_ + SecondsToTicks(kStandardSettleSeconds);
    if (shortened < standardNextTryQpc_) standardNextTryQpc_ = shortened;
  }
  if (now < standardNextTryQpc_) return;

  int preferred = 0;
  const std::vector<long> candidates =
      AutoStandardCandidates(available, config_.app.videoRegion, standardLastGood_, &preferred);
  if (candidates.empty()) return;
  standardCandidateCount_ = (int)candidates.size();

  // Bei totem Eingang wird nicht weitergeschaltet -- und vor allem gilt nichts
  // als geprueft, was hier gemessen wurde.
  //
  // Das ist ein gemessener Fehler, kein gedachter: am 30.08.2026 wurde der
  // GameCube ausgeschaltet und wieder eingeschaltet, und in dem Fenster ohne
  // Signal lief die Suche weiter. Als das Bild zurueckkam, war PAL B gerade
  // durch, und die Suche stand bei den 525/60-Normen -- sie lief 9,6 s lang
  // vier Kandidaten gegen ein anliegendes 625/50-Signal, die richtige Antwort
  // hatte sie kurz vorher schon in der Hand gehabt und weggeworfen. Eine Norm
  // gegen kein Signal zu pruefen ist keine Pruefung; sie faellt zwangslaeufig
  // durch, und was durchfaellt, wird eine ganze Runde lang nicht wieder gefragt.
  //
  // Also: Karte auf die beste Vermutung parken -- aus demselben Grund wie bei
  // der Pause unten, ein auftauchendes Signal bestaetigt sich sonst auf der
  // zuletzt zufaellig eingestellten Norm selbst -- und die Runde von vorn
  // beginnen lassen, sobald wieder etwas anliegt.
  if (starved && !standardStarvedLogged_) {
    standardStarvedLogged_ = true;
    CAP_LOG("Video standard: no frame for %.1f s -- %s does not match the incoming signal, the "
            "search goes on",
            frameAgeMs >= 0.0 ? frameAgeMs / 1000.0
                              : TicksToSeconds(now - standardStarvedSinceQpc_),
            VideoStandardName(VideoStandardIndexOf(capture_.currentStandard())));
  }
  if (!starved && renderer_.detectedSignal() == VideoRenderer::SignalVerdict::Flat) {
    if (standardCandidate_ >= 0 || capture_.currentStandard() != candidates.front()) {
      bool switched = false;
      if (capture_.currentStandard() != candidates.front()) {
        capture_.SetStandard(candidates.front());
        standardSeqAtSet_ = signalSeq_.load(std::memory_order_acquire);
        ResetStandardColourCheck();
        switched = true;
      }
      CAP_LOG("Video standard: no signal at the input, search paused and parked on %s",
              VideoStandardName(VideoStandardIndexOf(candidates.front())));
      standardCandidate_ = -1;

      // Parken heisst die Karte umstellen, und eine Norm bringt ihre Zeilenzahl
      // mit. Der Graph muss also mit, genau wie beim Einrasten weiter oben --
      // sonst laeuft er auf der alten Groesse weiter, und die Karte schiebt ein
      // 625-Zeilen-Bild in einen 480 Zeilen hohen Graphen.
      //
      // Auch das ist gemessen: am 31.08.2026 wurde der GameCube von 60 auf 50 Hz
      // zurueckgestellt. 01:48:03 wurde auf PAL B geparkt, 01:48:04 kamen 25,03
      // Bilder/s an -- und der Graph stand noch auf 720x480. Neunundvierzig
      // Sekunden lang war das Bild gestaucht, die Kammpruefung urteilte auf
      // einer Groesse, die es nicht gab, und die Aufloesungsliste zeigte die
      // Formate der alten Norm. Erst ein Neustart von Hand hat es geradegezogen.
      //
      // Ohne Signal neu aufzubauen ist dabei kein Nachteil, sondern der Sinn:
      // wenn das Bild wiederkommt, steht der Graph schon richtig. Und oefter
      // als noetig geschieht es nicht -- geparkt wird nur, wenn die Karte noch
      // nicht auf der Vermutung steht, und losgelassen nur, wenn sich die
      // Zeilenzahl wirklich geaendert hat.
      if (switched && host_.ReleaseStandardBoundFormat(VideoStandardLines(candidates.front()))) {
        std::string error;
        if (!host_.StartCapture(&error)) host_.Toast(error);
      }
    }
    // Die Frist des laufenden Kandidaten immer wieder von vorn, damit sie erst
    // zu laufen beginnt, wenn es etwas zu messen gibt.
    standardSetQpc_ = now;
    standardNextTryQpc_ = now + SecondsToTicks(kStandardSettleSeconds);
    return;
  }

  // Eine ganze Runde durch und nichts ist eingerastet: dann liegt vermutlich
  // gar kein Signal an. Pause, dann von vorn.
  //
  // Hier stand `standardSweeps_ >= 1 && standardCandidate_ < 0`, und das konnte
  // nie beides zugleich gelten. Der Index geht ausserhalb der Initialisierung
  // nur in dem Zweig auf -1, in dem der Lock geklappt hat, und der kehrt sofort
  // zurueck; beim allerersten Durchlauf ist er zwar -1, dann sind aber noch
  // null Runden gelaufen. Die Pause trat also nie ein, und die Suche schrieb
  // der Karte fuer immer alle `kStandardSettleSeconds` eine neue Norm.
  //
  // Jetzt wird das Ende einer Runde erkannt, bevor weitergeschaltet wird, und
  // `standardSweeps_` zaehlt wirklich abgeschlossene Runden, so wie es in
  // app.h beschrieben ist.
  if (standardCandidate_ + 1 >= (int)candidates.size()) {
    ++standardSweeps_;
    standardCandidate_ = -1;

    // Der schnelle Durchgang ist durch und hat nichts gefunden. Dann war er
    // entweder zu ungeduldig, oder es liegt wirklich nichts an -- und welches
    // von beidem, sagt genau ein weiterer Durchgang mit der vollen Frist.
    //
    // Ohne Pause dazwischen und ohne Parken. Beides gehoert ans Ende der
    // *Suche* und nicht in ihre Mitte: die Pause ist die Antwort auf "hier ist
    // nichts", und die steht nach einem schnellen Durchgang noch gar nicht
    // fest. Auch die Karte bleibt stehen, wo sie steht -- der naechste
    // Kandidat ist ohnehin wieder der erste der Liste.
    if (!standardPatientPass_) {
      standardPatientPass_ = true;
      standardNextTryQpc_ = now;
      if (standardSweeps_ == 1) {
        CAP_LOG("Video standard: fast pass without a lock (%d standards at %.2f s each), second "
                "pass at %.2f s",
                (int)candidates.size(), kStandardFastSeconds, kStandardSettleSeconds);
      }
      return;
    }

    standardPatientPass_ = false;
    standardNextTryQpc_ = now + SecondsToTicks(kStandardBackoffSeconds);
    // Und dabei nicht stehen lassen, was zuletzt probiert wurde.
    //
    // Waehrend der Pause steht irgendeine Norm auf der Karte, und wenn in
    // dieser Zeit ein Signal auftaucht -- die Konsole wird eingeschaltet,
    // jemand steckt endlich das Kabel an -- dann rastet der Lock darauf ein und
    // oben gilt "was gesetzt ist, stimmt". Gesucht wird dann gar nicht mehr.
    //
    // Das war ein echter Fehler: die Liste endet mit den seltensten Normen, die
    // Runde hinterliess also NTSC 4.43 auf der Karte, und weil der Lock nur
    // waagerecht misst, bestaetigt sich NTSC 4.43 an jeder 525/60-Quelle
    // selbst. Ein PAL-60-GameCube wurde so zuverlaessig als NTSC 4.43 erkannt.
    // Die Pause dauert laenger als eine Runde, das traf also die Mehrzahl der
    // Faelle -- und zwar genau den haeufigsten Ablauf ueberhaupt, naemlich
    // qBlank zuerst starten und die Konsole danach.
    //
    // Der erste Kandidat ist bauartbedingt die beste Vermutung: der Partner der
    // zuletzt eingerasteten Norm, sonst die haeufigste ueberhaupt. Ein Irrtum
    // dieser Art ist damit der wahrscheinlichste statt der unwahrscheinlichste,
    // und wo der erste Kandidat 625/50 ist, kann eine 525/60-Quelle sich gar
    // nicht mehr selbst bestaetigen: der Lock scheitert und es wird richtig
    // gesucht.
    if (capture_.currentStandard() != candidates.front()) {
      capture_.SetStandard(candidates.front());
      standardSeqAtSet_ = signalSeq_.load(std::memory_order_acquire);
    }
    // Nur beim ersten Mal, sonst laeuft das Log voll: dass pausiert wird, ist
    // einmal eine Nachricht und danach der Normalzustand.
    if (standardSweeps_ == 1) {
      CAP_LOG("Video standard: a full round without a lock, search paused (%.0f s), card set to %s",
              kStandardBackoffSeconds,
              VideoStandardName(VideoStandardIndexOf(candidates.front())));
    }
    // Beide Durchgaenge durch und nichts rastet ein: dann ist auch das die
    // Antwort auf den Tastendruck. Die Suche laeuft nach der Pause weiter --
    // was sie dann findet, ist nicht mehr die Antwort auf diese Frage.
    FinishManualStandardSearch(candidates.front(),
                               T("Keine Norm rastet ein — es liegt wohl kein Signal an",
                                 "No standard locks — there is probably no signal"));
    return;
  }

  // Verworfen -- und in der ersten Runde steht im Log, nach wie langer Frist.
  // Genau hier entsteht der Fehler, wenn die Frist zu kurz ist: eine Norm, die
  // nur noch nicht fertig eingefangen hat, sieht genauso aus wie eine falsche.
  // Nur die ersten beiden Runden, sonst schreibt eine Quelle ohne Signal das
  // Log voll -- und die ersten beiden sind es deshalb, weil das jetzt der
  // schnelle und der geduldige Durchgang sind. Genau ihr Vergleich ist die
  // Auskunft: eine Norm, die im schnellen Durchgang durchfaellt und im
  // geduldigen einrastet, sagt, dass kStandardFastSeconds zu knapp bemessen
  // ist. Darum steht auch dabei, welcher Durchgang gerade verwirft.
  if (standardSweeps_ <= 1 && standardCandidate_ >= 0 && standardSetQpc_ != 0) {
    CAP_LOG("Video standard: %s dropped after %.2f s without a lock (%s pass)",
            VideoStandardName(VideoStandardIndexOf(candidates[(size_t)standardCandidate_])),
            TicksToSeconds(now - standardSetQpc_),
            standardPatientPass_ ? "patient" : "fast");
  }

  ++standardCandidate_;

  // Wer uns gerade aushungert, ist schon widerlegt und braucht keine Frist.
  //
  // Nach dem Parken steht die Karte bereits auf candidates.front(), und genau
  // die ist der naechste Kandidat. Ohne diesen Schritt wird sie noch einmal
  // gesetzt -- ein Nichts -- und bekommt dann als vorgezogene Norm ihre vollen
  // kStandardPreferredSeconds, obwohl seit Sekunden kein Bild kommt. Am
  // 31.08. um 02:29 waren das drei geschenkte Sekunden von elf: 02:29:44,7
  // PAL B gesetzt, 02:29:47,6 "nach 3,03 s ohne Lock verworfen", 02:29:48,3
  // Lock auf PAL 60.
  if (starved && standardCandidate_ + 1 < (int)candidates.size() &&
      candidates[(size_t)standardCandidate_] == capture_.currentStandard()) {
    ++standardCandidate_;
  }

  const long next = candidates[(size_t)standardCandidate_];
  capture_.SetStandard(next);
  standardSetQpc_ = now;
  standardSeqAtSet_ = signalSeq_.load(std::memory_order_acquire);
  // Setting it is not the same as it working. Give the decoder a moment, then
  // this function will look at the lock again and either keep it or move on.
  // Den vorgezogenen Kandidaten wird laenger zugehoert, siehe oben.
  //
  // Ausser es kommt gar kein Bild. Die laengere Frist ist fuer eine Konsole
  // gedacht, die noch hochfaehrt -- die liefert dabei Bilder, nur noch keine
  // stabilen. Kommt nichts, ist Warten nur Warten, und der Rueckweg zu einem
  // Bild fuehrt ausschliesslich ueber den naechsten Kandidaten.
  //
  // Steht das Aushungern hier noch nicht fest, faellt es weiter oben nach --
  // die Frist wird dann nachtraeglich gekuerzt statt vorher verweigert.
  //
  // Im schnellen Durchgang bekommt jeder Kandidat dieselbe kurze Frist, auch
  // die vorgezogenen. Die zusaetzliche Geduld der vorderen Plaetze ist fuer
  // eine Quelle gedacht, die noch nicht stabil ist -- und die gewinnt der
  // schnelle Durchgang ohnehin nicht, sie gehoert dem zweiten. Wer hier
  // vorgezogen wird, wird es dadurch, dass er als erster drankommt.
  standardNextTryQpc_ =
      now + SecondsToTicks(!standardPatientPass_ ? kStandardFastSeconds
                         : !starved && standardCandidate_ < preferred
                             ? kStandardPreferredSeconds
                             : kStandardSettleSeconds);
  // Die Norm hat gewechselt, also gehoert die bisherige Farbmessung zu einer
  // anderen Einstellung.
  ResetStandardColourCheck();
}

// Ob die eingerastete Norm auch farblich stimmt.
//
// Der Lock allein kann das nicht sagen, und das ist keine Schwaeche der
// Umsetzung, sondern der Auskunft: er meldet, dass die Zeilenfrequenz gefunden
// wurde. 525/60 zerfaellt aber in fuenf Normen, die sich allein im Farbtraeger
// unterscheiden -- PAL 60 und NTSC 4.43 bei 4,43 MHz, NTSC M, NTSC M (Japan)
// und PAL M bei 3,58 MHz -- und waagerecht sehen die fuenf identisch aus. Steht
// die falsche, rastet die Karte trotzdem ein und meldet Erfolg.
//
// Was dann herauskommt, ist aber nicht unsichtbar: der Burst sitzt auf der
// falschen Frequenz, der Farbkiller des Decoders greift, und das Bild wird grau
// mit etwas Regenbogengries darin. Genau das laesst sich messen, und es ist die
// einzige Auskunft ueber den Farbtraeger, die es ueberhaupt gibt.
//
// Deshalb wird nicht behauptet, sondern verglichen: unter einem Wert, bei dem
// von Farbe keine Rede mehr sein kann, wird die Norm mit dem anderen Traeger
// probiert und nachgemessen. Nur wenn die deutlich farbiger ist, wird
// gewechselt. Bei einer wirklich schwarzweissen Quelle -- einem alten Film,
// einer S/W-Kamera -- sind beide gleich blass, es gewinnt keiner, und es bleibt
// bei dem, was die Region vorgeschlagen hat. Das ist die Rolle der Region an
// dieser Stelle: sie ist der Gleichstandssieger, den eine Messung allein nicht
// hat.
//
// Einmal je Norm. Danach ist die Sache entschieden, und eine Messung, die sich
// jede Minute neu meldet, waere ein Schalter, der von selbst umspringt.
//
// Entschieden heisst aber nicht blind. Die Messung laeuft ohnehin weiter, und
// die Nachkontrolle liest sie mit, ohne die Norm anzufassen -- siehe
// kChromaPale. Einmal je Lock darf sie die Pruefung neu eroeffnen, danach
// bleibt ihr nur noch der Hinweis.
void VideoStandardSearch::VerifyStandardColour(int64_t now) {
  // Unterhalb davon ist das Bild grau. Bewusst etwas ueber Null: an einem
  // Composite-Eingang rauscht auch ein totgeschalteter Farbkanal noch ein
  // wenig, und ein Verdacht, der am Rauschen scheitert, meldet sich nie.
  //
  // Am 29.08.2026 an einem GameCube im PAL-60-Modus gemessen, je einmal mit der
  // richtigen und der falschen Norm auf derselben Szene:
  //
  //   Szene                     NTSC M (Japan)   PAL 60
  //   Sherbet Land (Schnee)              0,024    0,037
  //   Dry Dry Desert (bunt)              0,013    0,207
  //
  // Der Schwellwert liegt ueber beiden falschen Messungen und unter der
  // richtigen der bunten Szene. Die farbarme Szene ist der enge Fall -- 0,037
  // gegen 0,035 --, und genau fuer den gibt es die Wiederholung weiter unten:
  // dort entscheidet nicht der Schwellwert, sondern die naechste Kurve.
  static const float kChromaSuspect = 0.035f;
  // Und so viel Farbe muss dastehen, damit der erste Durchgang die anliegende
  // Norm ohne jeden Vergleich durchwinkt.
  //
  // kChromaSuspect beantwortet "ist ueberhaupt Farbe da". Das ist die richtige
  // Frage fuer den Vergleich und die falsche fuer die Abkuerzung, und der
  // Unterschied hat heute eine Fehlerkennung gekostet: die Karte stand noch auf
  // PAL N, mass an einem PAL-B-Signal 0,042 -- knapp ueber der Sichtbarkeit --
  // und hatte dazu saubere Tiefen, weil ein falscher Traeger in den Tiefen eben
  // wenig anrichtet, wenn er ueberhaupt wenig anrichtet. Damit war die falsche
  // Norm bestaetigt, bevor irgendetwas verglichen wurde.
  //
  // Eine richtige Dekodierung liegt nicht knapp ueber der Sichtbarkeit, sondern
  // deutlich darueber: dieselbe Szene mass mit PAL B 0,152, mit PAL N 0,017 bis
  // 0,042. Die Schwelle liegt geometrisch dazwischen. Wer sie verfehlt, ist
  // deswegen nicht falsch -- er wird nur verglichen statt geglaubt, und das
  // kostet ein paar Sekunden, keine Fehlerkennung.
  static const float kChromaConfident = 0.080f;
  // So viel farbiger muss der Gewinner sein. Ein knapper Vorsprung ist kein
  // Befund, sondern Rauschen -- und im Zweifel bleibt es bei der Norm, die zur
  // Region passt.
  //
  // Zwei Werte, weil es zwei Faelle sind. Hat der Gewinner sichtbar Farbe, ist
  // die Messung schon von allein aus dem Rauschen heraus und ein deutlicher
  // Vorsprung genuegt. Liegen beide unter der Sichtbarkeitsschwelle, zaehlt
  // allein der Abstand, und der muss dann groesser sein: zwei Rauschwerte
  // derselben Szene liegen dicht beieinander.
  //
  // Hier stand vorher, der Gewinner muesse ausserdem selbst ueber
  // kChromaSuspect liegen -- sonst ergaeben zwei Rauschwerte allein durch ihr
  // Verhaeltnis einen Sieger, den niemand sieht. Der Gedanke stimmt, die
  // Umsetzung war zu grob: sie warf einen Sieg um Faktor 12 (0,024 gegen
  // 0,002) mit einem um Faktor 1,5 in denselben Topf. Genau das trennen die
  // zwei Werte jetzt, und die Messungen vom 29.08. ziehen die Grenze von
  // selbst -- entschieden waren Faktor 12 und 16, unentschieden 1,54.
  // Dazwischen ist viel Platz.
  static const float kChromaBetterBy = 1.6f;
  static const float kChromaBetterByFaint = 3.0f;
  // Und darunter zaehlt gar nichts mehr. Ein Verhaeltnis braucht einen Nenner:
  // misst der Verlierer glatt null, gewinnt jede noch so kleine Zahl mit
  // unendlichem Vorsprung. Der Boden liegt weit unter der kleinsten Messung,
  // die je etwas entschieden hat (0,024), und weit ueber dem Nichts.
  static const float kChromaFloor = 0.005f;
  // Wie oft der Gegenversuch wiederholt wird, wenn er nichts entscheidet.
  //
  // Das ist der Fall, den es wirklich gibt: eine Szene, die auch richtig
  // dekodiert fast grau ist. Am 29.08. an Sherbet Land gemessen -- eine
  // Schneepiste -- kamen 0,024 gegen 0,037 heraus, und daraus laesst sich
  // nichts schliessen. Einmal zu fragen und dann fuer immer zu schweigen waere
  // hier das Schlechteste: das Bild bliebe grau, obwohl die naechste Kurve die
  // Antwort liefert.
  //
  // Also wird spaeter noch einmal gemessen, mit wachsendem Abstand, und dann
  // ist Schluss. Jeder Versuch kostet ein paar Sekunden falsche Farbe, das darf
  // nicht endlos sein -- und eine wirklich schwarzweisse Quelle waere sonst
  // genau das.
  //
  // Der Abstand war einmal 30 s, weil "die naechste Kurve" bei Mario Kart
  // ungefaehr so lange braucht. Das war zu vorsichtig gedacht: solange nichts
  // entschieden ist, laeuft das Bild in der Ausgangsnorm weiter und der
  // Gegenversuch kostet zwei Sekunden. Drei Sekunden, verdoppelt, ergeben
  // 3 + 6 + 12 -- nach gut zwanzig Sekunden ist das Urteil gefaellt.
  //
  // Vorher standen hier 10 + 20 + 40. Das war zu vorsichtig gerechnet: der
  // Wiederholungsfall ist nicht teuer, weil er meistens gar keine Runde ist.
  // Steht ueber die Farbe der eingestellten Norm inzwischen genug fest, kostet
  // er nur den Blick darauf -- so am 31.08. um 02:17:47 und 02:25:41, beide
  // Male "PAL 60 hat deutlich Farbe", ohne einen einzigen Normwechsel. Nur
  // wenn es wieder nicht reicht, laeuft eine volle Runde, und die kostet die
  // vier Sekunden, gegen die die Pause gedacht war.
  static const int kColourRetries = 3;
  static const double kColourRetryBaseSeconds = 3.0;
  // Und was ein verworfener Rundgang wartet, naemlich fast nichts.
  //
  // Die Pause oben ist gegen eine graue Szene gedacht, und dort ist das Warten
  // der Zweck: die Szene soll erst farbig werden. Ein Rundgang, der verworfen
  // wurde, weil sich das Bild waehrenddessen geaendert hat, ist der
  // entgegengesetzte Fall -- gefehlt hat nicht die Farbe, sondern die Ruhe,
  // und ein Rennspiel wird durch Zuwarten nicht ruhiger. Sechs Sekunden
  // spaeter ist die Szene genauso in Bewegung, nur ist die Antwort dann sechs
  // Sekunden aelter.
  //
  // Die Zahl der Anlaeufe bleibt bei kColourRetries, also bleibt auch die
  // sichtbare Stoerung dieselbe: gleich viele Normwechsel, nur ohne die
  // Totzeit dazwischen. Aus 3 + 6 s werden 1 + 1 s.
  static const double kColourMotionRetrySeconds = 1.0;
  // Kommt in dieser Zeit keine Messung zustande, kommt keine. Das Format hat
  // dann keine lesbare Farbe -- siehe VideoRenderer::AnalyzeChroma -- oder es
  // laufen zu wenige Bilder durch. Beides ist kein Fehler, nur ein Nein.
  // Grosszuegig gegen die knapp drei Sekunden, die das Messfenster braucht.
  static const double kColourGiveUpSeconds = 10.0;

  // Um wie viel sauberer das Schwarz des Siegers sein muss.
  //
  // Es gibt hier bewusst *keinen* festen Schwellwert dafuer, ab wann Schwarz
  // als eingefaerbt gilt, und das ist eine Messung, keine Vorsicht. Am
  // 30.08.2026 an einem PAL-GameCube, dieselbe Szene dreimal dekodiert:
  //
  //   Norm                Farbe   dunkle Bereiche
  //   PAL B  (richtig)    0,152   0,101
  //   SECAM B (falsch)    0,461   0,353
  //   PAL N  (falsch)     0,017   0,014
  //
  // Ein fester Wert muesste zwischen 0,101 und 0,353 liegen, und der erste
  // Versuch mit 0,060 warf prompt die *richtige* Norm hinaus. Der Grund ist,
  // dass "dunkel" nicht "schwarz" heisst: unterhalb der Lumaschwelle liegen
  // auch dunkelrote und dunkelblaue Flaechen, die dort mit Recht Farbe haben,
  // und wie viel davon im Bild ist, haengt an der Szene. Ein absoluter Wert
  // misst also mit, was er nicht messen soll.
  //
  // Der Vergleich untereinander tut das nicht: alle drei Messungen sehen
  // dieselbe Szene, der Szenenanteil ist in allen dreien derselbe, und was sie
  // trennt, ist allein der Farbtraeger. 0,353 gegen 0,101 ist Faktor 3,5;
  // gefordert wird 1,6, also gut das Doppelte an Luft.
  static const float kDarkCleanerBy = 1.6f;

  // Um wie viel mehr Farbe der Kandidat mit den schmutzigeren Tiefen haben
  // muss, damit die Tiefenregel darueber nicht mehr entscheidet.
  //
  // Die Messreihe oben hat einen blinden Fleck, und der hat qBlank zwei
  // Rundgaenge lang die falsche Norm eingestellt. Sie besteht ganz aus
  // SECAM B gegen PAL B -- ein falscher Traeger, der Farbe *erfindet*: mehr
  // Farbe als die richtige Norm und dazu eingefaerbte Tiefen. Gegen den ist
  // "das sauberste Schwarz gewinnt" genau richtig.
  //
  // Es gibt den umgekehrten Fehler, und der GameCube zeigt ihn. NTSC 4.43 und
  // PAL 60 teilen sich den Farbtraeger bei 4,43 MHz und unterscheiden sich
  // nur in der zeilenweisen Phasenumkehr. NTSC 4.43 auf einem PAL-60-Signal
  // dekodiert deshalb nicht Unsinn, sondern *weniger*: weniger Farbe im
  // ganzen Bild und damit auch weniger davon im Schwarzen. Am 31.08.2026,
  // zweimal aus NTSC M heraus erzwungen:
  //
  //   Norm                 Farbe   Tiefen
  //   PAL 60  (richtig)    0,159   0,109      0,206   0,134
  //   NTSC 4.43 (falsch)   0,099   0,031      0,116   0,081
  //
  // Beide bestehen die Tinted-Pruefung, beide treten in Stufe eins an, und
  // dort gewinnt zweimal der falsche -- weil er weniger arbeitet. Die
  // Begruendung der Tiefenregel gilt fuer ihn nicht: wer seine Schwebung
  // gleichmaessig ueber alles legt, hat *mehr* Farbe, nicht weniger.
  //
  // Also tritt sie zurueck, sobald der mit den schmutzigeren Tiefen deutlich
  // mehr Farbe hat. Dann ist die Farbe in seinen Tiefen die, die er auch im
  // Rest des Bildes dekodiert, und nicht die eines fremden Traegers.
  //
  // Zuruecktreten kostet dabei fast nichts, und daran haengt die Hoehe des
  // Werts. Die Tiefenregel gibt die Entscheidung nicht an den anderen ab,
  // sondern an Stufe zwei -- und die verlangt ihrerseits kChromaBetterBy.
  // Reicht es dort nicht, stellt die Runde gar nichts ein, bleibt bei der
  // Ausgangsnorm und versucht es spaeter noch einmal. Ein zu frueher Rueckzug
  // kostet also eine Runde, ein zu spaeter eine falsche Norm.
  //
  // Deshalb 1,25 und nicht 1,5. Der erste Versuch stand bei 1,5, gemessen an
  // den 1,61 und 1,78 zweier Rundgaenge -- und liess prompt den naechsten
  // durch: am 31.08. um 15:38:01 stand PAL 60 mit 0,156/0,115 gegen NTSC 4.43
  // mit 0,114/0,067, ein Vorsprung von 1,37, und die Tiefen stellten wieder
  // die falsche Norm ein. Mit 1,25 tritt die Regel dort zurueck, Stufe zwei
  // entscheidet auf diesen Zahlen ebenfalls nichts (1,37 < 1,6), und genau das
  // ist das richtige Ergebnis: es bleibt bei PAL 60.
  //
  // Nach unten haelt die Gegenprobe. Am selben Abend um 15:35:34 hatte
  // NTSC 4.43 mit 0,161 gegen 0,160 einen Vorsprung von 1,006, waehrend die
  // Tiefen mit 0,039 gegen 0,129 klar fuer PAL 60 sprachen. Bei 1,25 kommt die
  // Regel dort nicht zum Zug und die Tiefen entscheiden richtig.
  //
  // Was das kostet: ein falscher Traeger, der es unter kDarkTinted schafft
  // *und* ein Viertel mehr Farbe zeigt, kann jetzt bis in Stufe zwei kommen.
  // In allen Messungen des 30.08. kommt SECAM B nie unter 0,204 und faellt
  // schon vorher aus -- der Fall steht nicht in der Reihe. Und Stufe zwei
  // schliesst eingefaerbte Kandidaten selbst noch einmal aus.
  static const float kDarkYieldsToColourBy = 1.25f;

  // Ab wann die Tiefen eines Kandidaten fuer sich allein als eingefaerbt
  // gelten -- ohne Vergleich, ohne zweite Norm.
  //
  // Der Absatz darueber sagt, ein fester Wert sei untauglich, weil "dunkel"
  // nicht "schwarz" heisst. Das gilt weiter, und trotzdem steht hier einer.
  // Der Grund ist der Versuch mit dem Anteil der Tiefen an der Gesamtfarbe,
  // der genau diesen festen Wert vermeiden sollte und daran gescheitert ist.
  // Alle Messungen an einem PAL-GameCube, 30.08.2026:
  //
  //   Norm                Farbe   Tiefen   Anteil
  //   PAL B  (richtig)    0,152    0,101    0,66
  //   PAL B  (richtig)    0,192    0,110    0,57
  //   PAL B  (richtig)    0,075    0,057    0,76
  //   SECAM B (falsch)    0,461    0,353    0,77
  //   SECAM B (falsch)    0,358    0,335    0,94
  //   SECAM B (falsch)    0,424    0,363    0,86
  //   SECAM B (falsch)    0,421    0,289    0,69
  //   PAL N  (falsch)     0,017    0,014    0,82
  //   PAL N  (falsch)     0,042    0,037    0,88
  //   PAL N  (falsch)     0,017    0,022    1,29
  //
  // Im Anteil ueberlappen richtig (0,57 bis 0,76) und falsch (0,69 bis 1,29).
  // In den Tiefen selbst nicht: richtig bleibt unter 0,110, falsches SECAM
  // faengt bei 0,289 an, Faktor 2,6 dazwischen.
  //
  // Am selben Abend noch einmal, nachdem das Messfenster von 3,2 s auf 0,4 s
  // verkuerzt war (siehe SetChromaCadence), vier Rundgaenge:
  //
  //   PAL B  (richtig)  0,190/0,112  0,259/0,044  0,135/0,053  0,163/0,069
  //   SECAM B (falsch)  0,357/0,334  0,477/0,273  0,438/0,393  0,425/0,204
  //   PAL N  (falsch)   0,042/0,038  0,012/0,011  0,019/0,015  0,018/0,027
  //
  // Die Tiefen des richtigen PAL B bleiben, wo sie waren -- unter 0,112 --,
  // die des falschen SECAM B reichen jetzt bis 0,204 herunter. Der Abstand
  // schrumpft damit von Faktor 2,6 auf 1,8, und der Wert steht nicht mehr in
  // der Mitte, sondern naeher am falschen Rand.
  //
  // Er bleibt trotzdem, wo er ist, weil die beiden Irrtuemer verschieden viel
  // kosten. Zu hoch heisst: ein falscher Kandidat gilt als plausibel und tritt
  // in Stufe eins an -- wo er gegen richtige Tiefen von 0,069 mit 0,204 immer
  // noch um Faktor 3 verliert. Zu niedrig heisst: der *richtige* Kandidat
  // fliegt aus beiden Stufen, der Rundgang entscheidet nichts, und der Nutzer
  // sieht bis zum naechsten Versuch weiter falsche Farben. Der Wert lehnt sich
  // deshalb an die Seite, auf der ein Fehler noch aufgefangen wird.
  //
  // Das Argument fuer den Anteil war, beide Zahlen kaemen aus denselben
  // Bildern, die Szene kuerze sich also heraus. Das stimmt fuer einen
  // *Vergleich zweier Kandidaten* -- und dort wird weiter verglichen, siehe
  // kDarkCleanerBy. Fuer einen festen Wert je Kandidat stimmt es nicht: der
  // Anteil haengt daran, wie viel des Bildes ueberhaupt dunkel ist, und das
  // ist selbst eine Szeneneigenschaft. Auf einer Szene mit wenig dunkler
  // Flaeche faellt der Anteil des falschen Traegers (0,69), auf einer flauen
  // steigt der des richtigen (0,76), und die beiden tauschen die Plaetze.
  //
  // Was den festen Wert hier tragfaehig macht und ihn beim ersten Versuch mit
  // 0,060 zu Fall brachte, ist die Bedingung davor: geprueft wird nur, wer
  // schon kraeftig Farbe hat (kChromaConfident). Wer wenig Farbe zeigt, hat
  // auch wenig davon in den Tiefen und wird gar nicht erst beurteilt.
  //
  // Faellt der Wert einmal falsch, kostet das keine falsche Norm: sind alle
  // Kandidaten eingefaerbt, bleibt die Entscheidung aus und es gilt die
  // Reihenfolge der Wohnregion.
  static const float kDarkTinted = 0.18f;

  // Woran ein NTSC-Dekoder auf einem PAL-Signal zu erkennen ist.
  //
  // Farbmenge und Tiefen trennen PAL 60 und NTSC 4.43 nicht. Beide haben
  // denselben Traeger bei 4,43 MHz, und was sie unterscheidet -- die
  // zeilenweise Umkehr der V-Phase --, geht in beiden Zahlen unter: der
  // NTSC-Dekoder dreht sie nicht zurueck, die Farbe klappt von Zeile zu Zeile
  // um, und im Mittel ueber einen Block hebt sie sich teilweise auf. Deshalb
  // misst die falsche Norm *weniger* Farbe und auch weniger davon im
  // Schwarzen, und deshalb hat sie ueber die Tiefen sogar gewonnen.
  //
  // Also wird das Umklappen selbst gemessen, statt seinen Schatten in den
  // gemittelten Zahlen zu deuten -- siehe VideoRenderer::chromaAltV. Am
  // 31.08.2026, zwoelf Rundgaenge an einem GameCube in PAL 60:
  //
  //   Norm                Zeilenwechsel V     V/U
  //   NTSC 4.43 (falsch)  0,0070 - 0,0515   7,7 - 14,6
  //   PAL 60  (richtig)   0,0002 - 0,0026   0,5 - 2,0
  //   NTSC M              0,0001 - 0,0005   0,4 - 0,8
  //   PAL M               0,0001 - 0,0008   0,1 - 1,0
  //
  // Beide Bedingungen muessen zusammen erfuellt sein, und beide haben Luft.
  // Der Betrag allein wuerde auf einem Bild mit feinem waagerechtem Muster
  // ansprechen -- aber ein solches Muster klappt *beide* Achsen um, und genau
  // das faengt die zweite Bedingung: umgekehrte Phase steht nur auf V.
  // Umgekehrt wuerde das Verhaeltnis allein auf Rauschen ansprechen, wo beide
  // Zahlen nahe null sind; dagegen steht der Betrag.
  //
  // Wer beides erfuellt, ist damit nicht "schlechter", sondern die falsche
  // Familie: ein NTSC-Dekoder auf einem zeilenweise wechselnden Traeger. Das
  // ist ein Ausschlussgrund wie eingefaerbtes Schwarz, kein Nachteil im
  // Vergleich.
  static const float kAltFlipping = 0.005f;
  static const float kAltAxisRatio = 4.0f;

  // Wie viel des Bildes beleuchtet sein muss, damit ein Rundgang ueberhaupt
  // etwas entscheiden kann.
  //
  // Schwarz ist in jeder Norm schwarz. Auf einem fast schwarzen Bild messen
  // alle Kandidaten dieselbe Null, und aus lauter gleichen Zahlen laesst sich
  // keine Norm auswaehlen -- der Rundgang laeuft, schaltet sichtbar durch drei
  // falsche Normen und kommt mit nichts zurueck.
  //
  // Genau so geschehen am 31.08. um 05:30:03. Der Wertebereichsmesser hatte
  // eine Sekunde vorher "95,135 % unter 16" notiert, das Bild war bis auf einen
  // hellen Rest schwarz. Gemessen wurde dann PAL 60 mit 0,005, NTSC M mit
  // 0,002, PAL M mit 0,003, NTSC 4.43 mit 0,003. Zweieinhalb Sekunden
  // Durchschalten fuer vier Mal nichts; als das Bild zurueck war, stand PAL 60
  // bei 0,194 und die Sache war in einer Messung erledigt.
  //
  // Der vorhandene Waechter greift hier nicht und soll es auch nicht: er fragt
  // SignalVerdict::Flat, also ob ueberhaupt etwas anliegt, und es lag etwas an
  // -- die Spanne war 154. "Es ist etwas zu sehen" und "es ist genug zu sehen,
  // um Farbe daran zu messen" sind zwei Fragen.
  //
  // Ein Zehntel, und die Wahl ist bewusst weit weg von dem, was ein Spielbild
  // trifft: gemeint ist nicht "dunkle Szene", sondern "praktisch nichts da".
  //
  // Nachgemessen am 31.08. um 05:43, mit kuenstlich hochgesetztem
  // kChromaConfident, damit ein Rundgang auf laufendem Bild erzwungen wird:
  // **54 % beleuchtet**. Das Fuenffache der Schwelle -- auf gewoehnlichem
  // Spielinhalt kann sie nicht danebengreifen. Der Fall, um den es geht, lag
  // mit "95 % unter Luma 16" auf der anderen Seite.
  //
  // Und falsch herum kostet sie nichts: eingefaerbte Tiefen laufen unten an ihr
  // vorbei, und wer wartet, verbraucht keinen Anlauf.
  static const float kChromaLitWanted = 0.10f;

  // Die Nachkontrolle, und warum es sie gibt.
  //
  // Alles oben entscheidet einmal je Norm, und das war an genau einer Stelle
  // zu wenig: wenn diese eine Gelegenheit auf ein Bild faellt, an dem nichts
  // zu entscheiden ist. Double Dash nach dem Wechsel von 50 auf 60 Hz zeigt
  // lange fast nur Schwarz um einen Lakitu herum -- genug beleuchtet, um
  // kChromaLitWanted zu passieren, zu wenig Farbe, um irgendetwas zu trennen.
  // Drei Anlaeufe gehen ins Leere, die Pruefung gibt auf ("wohl schwarzweiss"),
  // und die Karte bleibt auf NTSC stehen, auch wenn zehn Sekunden spaeter das
  // bunteste Rennen laeuft.
  //
  // Die Messung, die das zeigen wuerde, laeuft aber ohnehin weiter, jedes
  // achte Bild, fuer die Statistik und fuer genau diese Pruefung. Sie
  // mitzulesen kostet nichts -- kein Normwechsel, kein dichter Takt, ein paar
  // Vergleiche je drei Sekunden. Gelesen wird deshalb weiter, und zwar nach
  // drei Befunden, von denen jeder fuer sich einer falschen Norm gehoert:
  //
  //   - die Farbe klappt von Zeile zu Zeile um (kAltFlipping),
  //   - kraeftige Farbe steht bis ins Schwarze (kDarkTinted),
  //   - ein gut beleuchtetes Bild bleibt blass, und die Norm hat noch nie
  //     bewiesen, dass sie Farbe dekodieren kann.
  //
  // Die dritte Bedingung ist die heikle, und deshalb gilt sie nur fuer eine
  // Norm, die nie kraeftige, plausible Farbe gezeigt hat. Wer einmal richtig
  // dekodiert hat, ist auf einer Schneepiste nicht falsch.
  //
  // Die Schwelle liegt unter kChromaSuspect, weil die Nachkontrolle nicht
  // fragt "ist das verdaechtig", sondern "ist das eindeutig". Am 29.08. mass
  // die falsche Norm 0,024 (Schnee) und 0,013 (Wueste), die richtige auf dem
  // Schnee 0,037. Das Band zwischen 0,025 und 0,035 bleibt der Pruefung selbst.
  static const float kChromaPale = 0.025f;
  // Und "gut beleuchtet" heisst hier ein gewoehnliches Spielbild, nicht der
  // Rest um einen Lakitu. Gemessen wurden 54 % auf laufendem Spielinhalt,
  // siehe kChromaLitWanted.
  static const float kWatchLitWanted = 0.30f;
  // So viele Fenster in Folge, knapp sechs Sekunden. Eines allein ist ein
  // Bild, zwei sind ein Zustand.
  static const int kWatchStrikes = 2;

  auto darkText = [&](float d) {
    return d < 0.0f ? std::string("no dark areas") : Format("%.3f", d);
  };
  // Siehe kAltFlipping. Ohne Messung (-1) klappt nichts.
  auto flips = [&](float v, float u) { return v >= kAltFlipping && v > u * kAltAxisRatio; };

  // Waehrend eines Vergleichs wird dicht abgetastet, sonst duenn.
  //
  // Hier oben, vor jedem Ruecksprung, damit der dichte Takt nicht in den
  // Normalbetrieb durchsickern kann: colourCandidates_ *ist* der Suchzustand,
  // und wer ihn leert, stellt damit auch den Takt zurueck. Die Bedingung wird
  // je Bild neu gestellt, ausgefuehrt wird nur der Wechsel.
  //
  // Und ebenso, sobald ein Suchlauf von Hand ansteht. Das ist die teuerste
  // einzelne Wartezeit im ganzen Tastendruck, und sie war unsichtbar, weil sie
  // vor dem ersten Normwechsel liegt: der Rundgang kann erst beginnen, wenn
  // die Ausgangsnorm gemessen ist, und diese erste Messung lief noch im duennen
  // Takt. Zehn Bilder bei jedem achten und 29,97 Bildern je Sekunde sind
  // 2,67 s, in denen nichts geschieht und nichts zu sehen ist.
  //
  // Gemessen am 31.08. um 11:26:52,430 (Taste) bis 11:26:55,093 (Beginn des
  // Rundgangs): 2,66 s, gefolgt von 3,34 s Rundgang -- die Vorbereitung kostete
  // vier Zehntel des Ganzen. Drei Laeufe vorher, 11:21 und 11:23, lagen bei
  // 2,66 / 2,68 / 2,67 s: es ist eine Konstante und keine Schwankung.
  //
  // Der dichte Takt macht daraus drei Bilder in Folge, also ein Zehntel
  // Sekunde -- wie viele es sind, haengt am Takt selbst, siehe
  // kChromaFramesWantedDense. Bezahlt wird er mit derselben Rechenarbeit, die
  // der Rundgang ohnehin gleich verlangt, nur ein paar Sekunden frueher. Wer
  // von Hand sucht, bekommt den Rundgang ohnehin: die Abkuerzung ist fuer ihn
  // abgeschaltet, siehe `forced` weiter unten.
  const bool denseWanted = !colourCandidates_.empty() ||
                           (standardForceColourUntilQpc_ != 0 && now < standardForceColourUntilQpc_);
  renderer_.SetChromaCadence(denseWanted ? 1 : 8);

  const long current = capture_.currentStandard();
  if (current == 0) return;

  // Component und RGB haben keinen Farbtraeger, also auch nichts zu vergleichen.
  //
  // Die ganze Runde hier fragt eine einzige Frage: welcher Farbtraeger passt zu
  // dem, was anliegt. Sie stellt vier Normen ein und schaut, unter welcher das
  // Bild Farbe bekommt. Auf getrennten Leitungen kommt die Farbe aber gar nicht
  // aus einem Traeger, sondern liegt schon fertig da -- jede der vier Normen
  // zeigt dieselbe Farbe, und aus vier gleichen Zahlen laesst sich nichts
  // waehlen. Der Rundgang schaltete also sichtbar durch drei falsche Normen und
  // kaeme mit nichts zurueck. Derselbe Ausgang wie bei kChromaLitWanted weiter
  // oben, nur folgt er hier aus der Bauart und nicht aus dem Bildinhalt.
  //
  // Was bleibt, ist Stufe eins: 50 oder 60 Hz, 576 oder 480 Zeilen. Das
  // entscheidet der Lock, und der arbeitet auf Component genauso.
  //
  // Die anliegende Norm gilt damit als geprueft, sonst fragt jede Kurve von
  // vorn. Und der Tastendruck bekommt seine Antwort hier, weil er sie sonst
  // nirgends mehr bekaeme: die vier Ausgaenge unten sind alle hinter diesem
  // Ruecksprung.
  if (colourCandidates_.empty() && !host_.ConnectorHasColourCarrier()) {
    if (colourCheckedStandard_ != current) {
      CAP_LOG("Video standard: no colour round -- %s carries no colour subcarrier",
              AnalogConnectorName((int)host_.ResolvedConnector()));
    }
    colourCheckedStandard_ = current;
    standardForceColourUntilQpc_ = 0;
    FinishManualStandardSearch(
        current, T("Dieser Eingang führt keinen Farbträger — der Lock entscheidet allein",
                   "This input carries no colour subcarrier — the lock decides on its own"));
    return;
  }

  const bool walking = !colourCandidates_.empty();
  if (!walking && current == colourCheckedStandard_) {
    // Die Nachkontrolle, siehe kChromaPale. Sie liest nur mit: jedes volle
    // Messfenster wird einmal angesehen und dann verworfen, damit das naechste
    // nur neue Bilder enthaelt.
    if (colourWatch_ == ColourWatch::Done) return;
    if (colourWatchStandard_ != current) {
      // Neu hier -- und was bisher im Fenster steht, stammt womoeglich noch
      // aus der Pruefung davor. Ein Hinweis galt der alten Norm.
      colourWatchStandard_ = current;
      colourWatchStrikes_ = 0;
      if (colourWatch_ == ColourWatch::Notice) {
        colourNotice_.clear();
        colourWatch_ = ColourWatch::Reopened;
      }
      renderer_.ResetChroma();
      return;
    }
    if (renderer_.detectedSignal() == VideoRenderer::SignalVerdict::Flat) {
      colourWatchStrikes_ = 0;
      renderer_.ResetChroma();
      return;
    }
    const float e = renderer_.chromaEnergy();
    if (e < 0.0f) return;
    const float d = renderer_.darkChromaEnergy();
    const float lit = renderer_.chromaLitFraction();
    const float v = renderer_.chromaAltV();
    const float u = renderer_.chromaAltU();
    const bool flipping = flips(v, u);
    const char* name = VideoStandardName(VideoStandardIndexOf(current));

    if (e >= kChromaConfident && d < kDarkTinted && !flipping) {
      // Kraeftige, plausible Farbe: die Norm hat sich bewiesen. Auch nach
      // einem Aufgeben -- dann stand die Antwort nur noch aus.
      if (!colourProven_) {
        CAP_LOG("Video standard: %s shows clear colour afterwards (%.3f, dark areas %s) -- "
                "confirmed",
                name, e, darkText(d).c_str());
        colourProven_ = true;
      }
      if (colourWatch_ == ColourWatch::Notice) {
        colourNotice_.clear();
        colourWatch_ = ColourWatch::Done;
      }
      colourWatchStrikes_ = 0;
      renderer_.ResetChroma();
      return;
    }
    const bool tinted = e >= kChromaConfident && d >= kDarkTinted;
    const bool pale = !colourProven_ && lit >= kWatchLitWanted && e < kChromaPale;
    if (!flipping && !tinted && !pale) {
      colourWatchStrikes_ = 0;
      renderer_.ResetChroma();
      return;
    }
    if (++colourWatchStrikes_ < kWatchStrikes || colourWatch_ == ColourWatch::Notice) {
      renderer_.ResetChroma();
      return;
    }
    colourWatchStrikes_ = 0;
    const char* why = flipping ? "flips the colour from line to line"
                      : tinted ? "shows colour in the dark areas"
                               : "stays pale on a lit picture";

    if (colourWatch_ == ColourWatch::Watching) {
      // Einmal je Lock die Pruefung von vorn, und zwar mit genau diesem
      // Fenster: es wird nicht verworfen, der erste Durchgang liest es sofort.
      // Nicht ResetStandardColourCheck -- das setzte auch diese Nachkontrolle
      // zurueck, und aus dem einen Mal wuerde ein Kreislauf.
      CAP_LOG("Video standard: %s %s (colour %.3f, dark areas %s, %.0f %% lit, line alternation "
              "V %.4f U %.4f) -- checking the colour again",
              name, why, e, darkText(d).c_str(), lit * 100.0f, v, u);
      colourWatch_ = ColourWatch::Reopened;
      colourCheckedStandard_ = 0;
      colourWatchStandard_ = 0;
      colourProven_ = false;
      colourAttempts_ = 0;
      colourStartedQpc_ = 0;
      colourRetryQpc_ = 0;
      colourWaitingForPicture_ = false;
      return;
    }

    // Die Wiederholung hat es nicht geklaert. Umschalten darf die Automatik
    // jetzt nicht mehr -- also sagen, was sie sieht, und die Suche anbieten.
    // Eingefaerbte Tiefen allein reichen dafuer nicht: sie sind der weichste
    // der drei Befunde, und eine Runde, die danach dieselbe Norm bestaetigt,
    // hat ihn gerade widerlegt.
    renderer_.ResetChroma();
    if (!flipping && !pale) return;
    CAP_LOG("Video standard: %s %s even after the second check (colour %.3f, dark areas %s, "
            "%.0f %% lit, line alternation V %.4f U %.4f) -- hint shown",
            name, why, e, darkText(d).c_str(), lit * 100.0f, v, u);
    colourWatch_ = ColourWatch::Notice;
    const std::string picker = VideoStandardPickerName(current);
    colourNotice_ =
        flipping ? Format(T("Die Farbe kippt von Zeile zu Zeile — %s passt wohl nicht",
                            "The colour flips from line to line — %s is probably wrong"),
                          picker.c_str())
                 : Format(T("Kaum Farbe unter %s — die Videonorm passt womöglich nicht",
                            "Hardly any colour under %s — the video standard may be wrong"),
                          picker.c_str());
    return;
  }
  // Ein unentschiedener Versuch wartet, bevor er sich wiederholt.
  if (colourRetryQpc_ != 0 && now < colourRetryQpc_) return;
  colourRetryQpc_ = 0;

  // Bei totem Eingang wird nicht gemessen. Ein Eingang ohne Signal ist grau,
  // und grau ist hier die Aussage "der Farbtraeger stimmt nicht" -- die Messung
  // saehe also nicht etwa nichts, sie saehe zuverlaessig das Falsche. Dasselbe
  // Argument wie beim Weiterschalten der Normensuche, siehe oben.
  if (renderer_.detectedSignal() == VideoRenderer::SignalVerdict::Flat) {
    renderer_.ResetChroma();
    colourStartedQpc_ = 0;
    return;
  }

  // Frisch gewechselt: die ersten Bilder gehoeren noch der alten Einstellung.
  //
  // Zurueckgesetzt wird genau einmal, naemlich wenn die Frist abgelaufen ist.
  // Hier stand vorher ein Ruecksetzer je Bild, solange gewartet wird, und der
  // sah richtig aus und war es nicht: diese Funktion laeuft nach einem
  // Normwechsel eine knappe halbe Sekunde ueberhaupt nicht: die Suche wartet
  // auf zwei frische Messwerte des Wachtthreads, siehe UpdateVideoStandard.
  // Faellt das Ende der Frist in dieses Loch, ist der erste Aufruf danach
  // schon zu spaet -- es hat nie jemand zurueckgesetzt, und gemessen wird ab
  // dem Wechsel statt ab dem Fristende, mitsamt dem Umschaltmoment der Karte.
  //
  // Am 30.08. mit Frist null nachgestellt, weil dort dasselbe Loch immer
  // klafft: PAL N mass 0,047 und 0,064 statt 0,012 bis 0,021, SECAM B in den
  // Tiefen 0,246 statt 0,271 bis 0,410 -- jeder Kandidat zum Nachbarn hin
  // verschmiert. Mit einer Frist von 0,5 s traf es nur die Laeufe, in denen
  // das Loch etwas laenger war als die Frist, und das war nicht zu sehen.
  //
  // Ein Ruecksetzer am Fristende macht den Anfang des Messfensters unabhaengig
  // davon, wann der naechste Aufruf kommt, und kostet nichts: das Fenster lag
  // ohnehin dahinter.
  if (colourSettleUntilQpc_ != 0) {
    if (now < colourSettleUntilQpc_) return;
    colourSettleUntilQpc_ = 0;
    colourStartedQpc_ = now;
    // Hier faengt der Kandidat an, also hat er noch kein voriges Fenster.
    colourWindowEnergy_ = -1.0f;
    colourWindowDark_ = -1.0f;
    renderer_.ResetChroma();
    return;
  }
  if (colourStartedQpc_ == 0) {
    colourStartedQpc_ = now;
    colourWindowEnergy_ = -1.0f;
    colourWindowDark_ = -1.0f;
  }

  const float energy = renderer_.chromaEnergy();
  if (energy < 0.0f) {
    if (TicksToSeconds(now - colourStartedQpc_) <= kColourGiveUpSeconds) return;
    // Keine Messung zustande gekommen. Ohne laufenden Rundgang ist die Sache
    // damit erledigt -- es gibt nichts zu vergleichen. Im Rundgang zaehlt es
    // als "weiss nicht", wird als solches eingetragen und der naechste
    // Kandidat ist dran.
    // Die Nachkontrolle haette dieselbe Messung, also auch nichts.
    if (!walking) {
      colourCheckedStandard_ = current;
      colourStartedQpc_ = 0;
      colourWatch_ = ColourWatch::Done;
      return;
    }
  }
  const float dark = energy < 0.0f ? -1.0f : renderer_.darkChromaEnergy();

  // Im Rundgang gilt eine Messung erst, wenn sie nicht mehr steigt.
  //
  // Das ersetzt das blosse Abwarten einer Frist, und der Grund dafuer ist am
  // 31.08. gemessen worden. Mit 0,15 s Frist lieferten drei der vier Normen
  // ihren Wert auf den Tausendstel genau wie mit 0,5 s -- NTSC M 0,017,
  // PAL M 0,023, NTSC 4.43 0,065, dreimal hintereinander --, die vierte nicht:
  // PAL 60 mass bei der Rueckkehr 0,000 / 0,047 / 0,095 statt 0,119, und zwar
  // umso hoeher, je spaeter das Fenster lag (0,276 / 0,327 / 0,393 s nach dem
  // Wechsel). Das ist keine Streuung, das ist eine Einschwingkurve, und sie
  // hat einen Grund: der PAL-Burst wechselt zeilenweise die Phase, und seine
  // Kennung braucht mehr Zeilen als NTSCs feste Phase.
  //
  // Eine feste Frist muss deshalb immer die langsamste Norm bezahlen, und alle
  // anderen zahlen mit. Zweimal von drei Laeufen verwarf der Rundgang sich
  // daraufhin selbst, weil die zu frueh gemessene Rueckkehr nicht mehr zur
  // ersten Messung passte -- also nicht ein bisschen ungenauer, sondern
  // unbrauchbar.
  //
  // Der Dekoder sagt aber selbst, wann er so weit ist: solange er einrastet,
  // steigt die Messung, danach steht sie. Gefragt wird deshalb nicht "ist die
  // Frist um", sondern "ist der neue Wert noch hoeher als der vorige" -- und
  // gerichtet, nicht als blosse Aehnlichkeit. Ein bewegtes Bild schwankt in
  // beide Richtungen und ist beim ersten nicht gestiegenen Fenster fertig; ein
  // einrastender Dekoder kann das nicht, er kommt nur von unten.
  //
  // Zwei Fenster braucht es dadurch immer, auch bei der schnellsten Norm. Das
  // ist der Preis und er ist der Sache angemessen: ein einzelnes Fenster hat
  // nichts, woran es sich pruefen koennte.
  if (walking && energy >= 0.0f) {
    const bool rising = colourWindowEnergy_ < 0.0f ||
                        ColourStillRising(colourWindowEnergy_, energy) ||
                        ColourStillRising(colourWindowDark_, dark);
    const double waited = TicksToSeconds(now - colourStartedQpc_);
    if (rising && waited < kColourStableGiveUpSeconds) {
      colourWindowEnergy_ = energy;
      colourWindowDark_ = dark;
      renderer_.ResetChroma();
      return;
    }
    if (rising) {
      // Nicht zur Ruhe gekommen. Der zuletzt gemessene Wert ist trotzdem der
      // beste, den es gibt -- er liegt am weitesten hinter dem Wechsel --,
      // also zaehlt er. Aber er gehoert ins Log, denn er heisst entweder, dass
      // diese Karte laenger braucht als die Obergrenze, oder dass sich das
      // Bild waehrenddessen bewegt hat.
      CAP_LOG("Video standard: %s does not settle within %.2f s (%.3f, before that %.3f) -- the "
              "last value counts",
              VideoStandardName(VideoStandardIndexOf(current)), waited, energy,
              colourWindowEnergy_);
    }
  }

  if (!walking) {
    // Erster Durchgang, und hier werden zwei Fragen gestellt statt einer: hat
    // das Bild Farbe, und bleibt sein Schwarz schwarz?
    //
    // Die zweite fehlte, und das war ein echter Fehler mit sichtbarer Folge.
    // Am 30.08.2026 rastete ein PAL-Signal auf SECAM ein, und SECAM auf PAL
    // liefert kein graues Bild, sondern ein kraeftig eingefaerbtes -- der
    // Nutzer sah einen leuchtend roten Hintergrund, wo Schwarz sein sollte.
    // Der Test "hat Farbe" ging glatt durch, die Fehldekodierung galt als
    // bestaetigt, und danach wurde sie nie wieder in Frage gestellt.
    // Der Schwellwert hier ist ein *Verdacht*, kein Urteil. Er entscheidet nur,
    // ob ueberhaupt verglichen wird; welche Norm gewinnt, entscheiden danach
    // die Kandidaten untereinander. Ein zu hoher Wert kostet eine
    // Fehldekodierung, ein zu niedriger ein paar Sekunden Vergleich. Es ist
    // dieselbe Frage wie beim Vergleich -- sind diese Tiefen eingefaerbt --
    // und deshalb derselbe Wert; die Messreihe steht bei kDarkTinted.
    //
    // Ausser jemand hat die Suche von Hand ausgeloest. Dann ist der Rundgang
    // der Zweck des Tastendrucks und nicht das Mittel gegen einen Verdacht:
    // wer ihn drueckt, sieht etwas, das keine dieser beiden Zahlen misst --
    // einen Farbstich, Gesichter in der falschen Farbe --, und die Abkuerzung
    // waere hier die eine Antwort, die er schon hat.
    //
    // Und eine dritte, die beide Zahlen nicht sehen: kippt die Farbe von Zeile
    // zu Zeile? Am 31.08. mass NTSC 4.43 auf einem PAL-60-Signal 0,099 Farbe
    // bei 0,031 in den Tiefen -- beide Tests bestanden, und das Bild hatte die
    // Jalousie, die ein fehlender PAL-Phasenwechsel macht. Der Rundgang kennt
    // das Merkmal laengst (siehe kAltFlipping); der erste Durchgang las es
    // bisher nur nicht.
    const bool forced =
        standardForceColourUntilQpc_ != 0 && now < standardForceColourUntilQpc_;
    const bool flipping = flips(renderer_.chromaAltV(), renderer_.chromaAltU());
    if (!forced && !flipping && energy >= kChromaConfident && dark < kDarkTinted) {
      CAP_LOG("Video standard: %s has clear colour (%.3f), dark areas neutral (%s)",
              VideoStandardName(VideoStandardIndexOf(current)), energy, darkText(dark).c_str());
      colourCheckedStandard_ = current;
      colourStartedQpc_ = 0;
      colourAttempts_ = 0;
      colourWaitingForPicture_ = false;
      colourProven_ = true;
      // Hierher kommt ein Suchlauf von Hand nur, wenn seine Frist abgelaufen
      // ist, bevor ein Bild zum Vergleichen da war -- und dann ist das hier
      // die Antwort: gemessen wurde, es sprach nichts dagegen.
      FinishManualStandardSearch(current, T("Die Farbe stimmt — es bleibt dabei",
                                            "The colour checks out — no change"));
      return;
    }

    // Zweifel ja -- aber ist genug Bild da, um ihn auszuraeumen?
    //
    // Der Zweifel hat zwei ganz verschiedene Gruende, und nur einer davon ist
    // eine Frage, die ein Rundgang beantworten kann. Zeigt ein *helles* Bild
    // keine Farbe, dann hat der Farbtraeger nicht gestimmt, und welcher es
    // stattdessen ist, entscheiden die Kandidaten untereinander. Zeigt ein
    // schwarzes Bild keine Farbe, dann ist es schwarz. Da ist nichts zu
    // entscheiden, und die vier gleichen Nullen von oben sind die Antwort
    // darauf.
    //
    // Eingefaerbte Tiefen fuehren *nicht* hierher, sondern in den Rundgang, und
    // zwar auch auf einem dunklen Bild: eine Farbe, die im Schwarzen steht, ist
    // ein Beleg fuer einen falschen Traeger und kein fehlender Messwert. Sie
    // ist auf einem dunklen Bild sogar am deutlichsten -- dort sind die
    // Bloecke, um die es geht.
    //
    // Gewartet wird, ohne etwas zu verbrauchen: kein Anlauf wird gezaehlt,
    // keine Norm als geprueft vermerkt. Ein Rundgang, der nie stattgefunden
    // hat, darf weder als unentschieden zaehlen noch die drei Anlaeufe
    // aufbrauchen, bevor das Bild ueberhaupt da ist -- eine Konsole, die
    // hochfaehrt, ist ein paar Sekunden lang schwarz, und danach soll die
    // Pruefung noch alle Anlaeufe haben. Zurueckgesetzt wird dabei wie beim
    // toten Eingang, damit die erste Messung nach dem Warten nicht durch das
    // Schwarze davor verduennt wird.
    const float lit = renderer_.chromaLitFraction();
    if (!flipping && lit >= 0.0f && lit < kChromaLitWanted && dark >= 0.0f &&
        dark < kDarkTinted) {
      if (!colourWaitingForPicture_) {
        colourWaitingForPicture_ = true;
        CAP_LOG("Video standard: %s is doubtful (colour %.3f), but only %.0f %% of the picture is "
                "lit -- no standard can be told on black, waiting",
                VideoStandardName(VideoStandardIndexOf(current)), energy, lit * 100.0f);
      }
      renderer_.ResetChroma();
      colourStartedQpc_ = 0;
      // Gewartet wird weiter -- aber nicht mehr im Namen des Tastendrucks.
      // Dessen Frist ist abgelaufen, und eine Einblendung, die eine halbe
      // Minute lang "es wird gewartet" sagt und dann verstummt, ist genau die
      // Antwort, die hier gefehlt hat.
      if (!forced) {
        FinishManualStandardSearch(
            current, T("Das Bild blieb zu dunkel für einen Vergleich — es bleibt dabei",
                       "The picture stayed too dark to compare — no change"));
      }
      return;
    }
    colourWaitingForPicture_ = false;

    colourCandidates_ = VideoStandardColourCandidates(
        current, capture_.capabilities().availableStandards, config_.app.videoRegion);
    // Der Rundgang beginnt bei der jetzigen Norm, die ja gerade gemessen wurde.
    // Steht sie nicht vorn, hat die Karte etwas gemeldet, das sie laut eigener
    // Auskunft gar nicht kann -- dann lieber nichts tun als raten.
    // Einen Hinweis gaebe es hier auch nicht: "Norm suchen" faende nichts.
    if (colourCandidates_.size() < 2 || colourCandidates_.front() != current) {
      colourCandidates_.clear();
      colourCheckedStandard_ = current;
      colourStartedQpc_ = 0;
      colourWatch_ = ColourWatch::Done;
      standardForceColourUntilQpc_ = 0;
      FinishManualStandardSearch(
          current, Format(T("Keine andere Norm mit %d Zeilen — es bleibt dabei",
                            "No other standard with %d lines — no change"),
                          VideoStandardLines(current)));
      return;
    }
    const int count = (int)colourCandidates_.size();
    // Die Ausgangsnorm kommt am Ende ein zweites Mal dran.
    //
    // Der Vergleich unterstellt, alle Kandidaten saehen dieselbe Szene. Am
    // 30.08. um 15:24 Uhr, als jede Messung noch gut drei Sekunden brauchte
    // und der ganze Rundgang vierzehn, dieselbe Norm im Abstand von zwanzig
    // Sekunden: PAL B 0,181/0,217 und PAL B 0,191/0,107. Die Tiefen
    // halbierten sich, ohne dass sich am Signal etwas geaendert haette.
    //
    // Der Rundgang dauert seither knapp drei Sekunden (siehe
    // SetChromaCadence), und damit ist der Grund kleiner geworden, aber nicht
    // weg -- und er hat einen zweiten bekommen: die erste Messung der
    // Ausgangsnorm ist die mitlaufende aus dem Normalbetrieb, ueber drei
    // Sekunden gemittelt, die der Herausforderer sind kurze Aufnahmen. Die
    // Wiederholung stellt die Ausgangsnorm auf dieselbe Grundlage wie die
    // anderen.
    //
    // Der Schaden daraus ist einseitig. Ueber alle Messungen des 30.08.
    // streuen die Tiefen des falschen SECAM B um Faktor zwei (0,204 bis
    // 0,410), die des richtigen PAL B um Faktor fuenf (0,041 bis 0,217): eine
    // Schwebung aus dem falschen Traeger liegt gleichmaessig ueber jedem Bild
    // und haengt nur wenig an der Szene, echte Farbe in dunklen Flaechen
    // dagegen ganz und gar. Eine flaue Szene laesst also vor allem den
    // *richtigen* Kandidaten schlecht aussehen.
    //
    // Deshalb wird die Ausgangsnorm zweimal gemessen und tritt mit der
    // guenstigeren der beiden Messungen an. Das ist mit Absicht ungleich
    // verteilt: sie ist der Kandidat, der schon laeuft, und ein Wechsel weg
    // von einer richtigen Norm ist der teure Fehler.
    colourCandidates_.push_back(colourCandidates_.front());
    colourEnergies_.assign(colourCandidates_.size(), -1.0f);
    colourDarks_.assign(colourCandidates_.size(), -1.0f);
    colourAltV_.assign(colourCandidates_.size(), -1.0f);
    colourAltU_.assign(colourCandidates_.size(), -1.0f);
    colourIndex_ = 0;
    // Der beleuchtete Anteil steht mit in der Zeile, obwohl er die Runde nicht
    // ausloest. Er ist die Grundlage, auf der sie ueberhaupt etwas entscheiden
    // kann, und wenn ein Rundgang spaeter einmal unerklaerlich lauter Nullen
    // misst, steht die Erklaerung schon in der Zeile davor.
    // Und woran es lag, fuer die Einblendung festgehalten. Die beiden Gruende
    // sind nicht dasselbe und sehen auch nicht gleich aus: zu blass heisst ein
    // graues Bild mit Regenbogengries, eingefaerbt heisst ein Bild mit
    // kraeftig falschen Farben bis ins Schwarze hinein. Wer davorsitzt, sieht
    // genau einen der beiden Faelle und erkennt seinen wieder.
    colourDoubt_ = forced                ? ColourDoubt::Manual
                   : flipping            ? ColourDoubt::Flipping
                   : dark >= kDarkTinted ? ColourDoubt::Tinted
                                         : ColourDoubt::Pale;
    standardForceColourUntilQpc_ = 0;
    CAP_LOG("Video standard: %s is doubtful (colour %.3f, dark areas %s, %.0f %% lit, line "
            "alternation V %.4f U %.4f) -- comparing the %d standards with %d lines%s",
            VideoStandardName(VideoStandardIndexOf(current)), energy, darkText(dark).c_str(),
            lit < 0.0f ? 0.0f : lit * 100.0f, renderer_.chromaAltV(), renderer_.chromaAltU(),
            count, VideoStandardLines(current),
            colourDoubt_ == ColourDoubt::Manual ? " (started by hand)" : "");
  }

  // Eintragen, was dieser Kandidat gemessen hat, und zum naechsten.
  colourEnergies_[(size_t)colourIndex_] = energy;
  colourDarks_[(size_t)colourIndex_] = dark;
  colourAltV_[(size_t)colourIndex_] = renderer_.chromaAltV();
  colourAltU_[(size_t)colourIndex_] = renderer_.chromaAltU();
  CAP_LOG("Video standard: %s measured -- colour %s, dark areas %s, line alternation V %.4f U %.4f",
          VideoStandardName(VideoStandardIndexOf(current)),
          energy < 0.0f ? "no measurement" : Format("%.3f", energy).c_str(),
          darkText(dark).c_str(), colourAltV_[(size_t)colourIndex_],
          colourAltU_[(size_t)colourIndex_]);

  ++colourIndex_;
  if (colourIndex_ < (int)colourCandidates_.size()) {
    capture_.SetStandard(colourCandidates_[(size_t)colourIndex_]);
    standardSeqAtSet_ = signalSeq_.load(std::memory_order_acquire);
    colourSettleUntilQpc_ = now + SecondsToTicks(kColourSettleSeconds);
    colourStartedQpc_ = 0;
    renderer_.ResetChroma();
    return;
  }

  // Alle durch, die Ausgangsnorm zweimal. Von ihren beiden Messungen zaehlt
  // die mit den saubereren Tiefen, und zwar als Paar: die Farbmenge kommt aus
  // derselben Messung wie die Tiefen, sonst stuenden Zahlen aus zwei Szenen
  // nebeneinander. Die Begruendung steht oben beim zweiten Anlauf.
  // Um wie viel die beiden Messungen derselben Norm auseinanderliegen duerfen,
  // bevor die Runde als ungueltig gilt. Siehe sceneChanged gleich darunter.
  static const float kSceneAgreeBy = 2.0f;
  bool sceneChanged = false;

  if (colourCandidates_.size() > 1 && colourCandidates_.back() == colourCandidates_.front()) {
    const size_t last = colourCandidates_.size() - 1;
    // Guenstiger heisst: so, wie die Runde unten urteilt -- und dort ist
    // "keine dunklen Stellen" kein schlechter Tiefenwert, sondern gar keiner.
    //
    // Die erste Fassung las guenstig als "sauberere Tiefen" und stellte -1
    // dabei ganz nach hinten. Das ging am 31.08.2026 zweimal schief, und zwar
    // in beide Richtungen. Um 15:45:07 mass PAL 60 zuerst 0,084/0,121 und dann
    // 0,155 ohne eine einzige dunkle Stelle; die zweite wurde verworfen, PAL 60
    // trat mit 0,084 an und verlor gegen NTSC 4.43 mit 0,099. Um 15:46:12 war
    // es umgekehrt: zuerst 0,217 ohne dunkle Stellen, dann 0,205/0,295 -- die
    // zweite zaehlte, 0,295 gilt als eingefaerbt, und damit war die richtige
    // Norm aus beiden Stufen draussen.
    //
    // Also der Reihe nach: nicht eingefaerbt schlaegt eingefaerbt. Sind beide
    // gleich weit, entscheiden die Tiefen -- aber nur, wenn es welche gibt.
    // Fehlen sie in einer der beiden Messungen, gibt es nichts zu vergleichen,
    // und dann zaehlt die Farbmenge. Genau die soll die Wiederholung ja
    // retten.
    const bool tinted0 = colourDarks_[0] >= kDarkTinted;
    const bool tintedLast = colourDarks_[last] >= kDarkTinted;
    const bool better =
        tinted0 != tintedLast
            ? !tintedLast
            : (colourDarks_[0] < 0.0f || colourDarks_[last] < 0.0f
                   ? colourEnergies_[last] > colourEnergies_[0]
                   : colourDarks_[last] < colourDarks_[0]);

    // Und hier wird die Zweitmessung das, wofuer sie eigentlich da ist: die
    // Probe darauf, ob die Runde ueberhaupt eine Szene gesehen hat.
    //
    // Die Kandidaten werden nacheinander gemessen, eine ganze Runde dauert
    // ein paar Sekunden, und das Bild wartet nicht. Aendert sich die Szene
    // dazwischen -- ein Bootbildschirm wird zum Spiel --, dann sahen die
    // frueheren Kandidaten etwas anderes als die spaeteren, und der Vergleich
    // vergleicht nichts. Wer zufaellig drankam, als es bunt wurde, gewinnt.
    //
    // Genau das ist am 31.08.2026 um 02:02 passiert. Die Runde ueber die fuenf
    // 525-Zeilen-Normen fing auf einem dunklen Bild an und endete auf einem
    // farbigen: PAL 60 zuerst 0.005, am Ende 0.122, und dazwischen bekam
    // NTSC 4.43 mit 0.157 den Zuschlag -- gegen einen GameCube, der PAL 60
    // ausgibt. Danach stand die Karte auf einer Norm, die das Signal nicht
    // dekodieren kann.
    //
    // Gemessen wird die Abweichung an den beiden Schwellen, die in dieser
    // Runde ueberhaupt etwas entscheiden: kChromaConfident trennt "beurteilt"
    // von "unbeurteilt", kDarkTinted "plausibel" von "eingefaerbt". Springt
    // eine der beiden Messungen ueber eine dieser Schwellen, hat sich der
    // Massstab selbst bewegt. Der Faktor daneben faengt die Faelle, die
    // innerhalb einer Schwelle bleiben und trotzdem eine andere Szene sind.
    const float e0 = colourEnergies_[0], e1 = colourEnergies_[last];
    const float d0 = colourDarks_[0], d1 = colourDarks_[last];
    if (e0 >= 0.0f && e1 >= 0.0f) {
      if ((e0 < kChromaConfident) != (e1 < kChromaConfident)) sceneChanged = true;
      const float lo = e0 < e1 ? e0 : e1, hi = e0 < e1 ? e1 : e0;
      if (hi > lo * kSceneAgreeBy && hi >= kChromaConfident) sceneChanged = true;
    }
    if (d0 >= 0.0f && d1 >= 0.0f && (d0 < kDarkTinted) != (d1 < kDarkTinted)) sceneChanged = true;

    CAP_LOG("Video standard: %s measured a second time -- colour %s, dark areas %s (first %s and "
            "%s)%s",
            VideoStandardName(VideoStandardIndexOf(colourCandidates_.front())),
            colourEnergies_[last] < 0.0f ? "no measurement"
                                         : Format("%.3f", colourEnergies_[last]).c_str(),
            darkText(colourDarks_[last]).c_str(),
            colourEnergies_[0] < 0.0f ? "no measurement" : Format("%.3f", colourEnergies_[0]).c_str(),
            darkText(colourDarks_[0]).c_str(),
            sceneChanged ? " -- the scene changed during the round, the comparison "
                           "does not count"
                         : (better ? " -- the second one counts" : ""));
    if (better) {
      colourEnergies_[0] = colourEnergies_[last];
      colourDarks_[0] = colourDarks_[last];
    }
    // Der Zeilenwechsel folgt dieser Wahl *nicht*, sondern nimmt den groesseren
    // der beiden Werte. Farbmenge und Tiefen haengen an der Szene, und deshalb
    // ist es dort eine Frage, welche der beiden Messungen die aussagekraeftige
    // ist. Das Umklappen haengt an der Norm: ein Dekoder, der die Phasenumkehr
    // richtig aufhebt, kann sie nicht in einer zweiten Messung ploetzlich
    // zeigen. Rauschen und flaue Szenen druecken den Wert nur nach unten. Wer
    // ihn also in einer der beiden Messungen hat, hat ihn.
    if (colourAltV_[last] > colourAltV_[0]) {
      colourAltV_[0] = colourAltV_[last];
      colourAltU_[0] = colourAltU_[last];
    }
    colourCandidates_.pop_back();
    colourEnergies_.pop_back();
    colourDarks_.pop_back();
    colourAltV_.pop_back();
    colourAltU_.pop_back();
  }

  // Erst aussortieren, dann vergleichen -- und die Reihenfolge ist der ganze
  // Punkt.
  //
  // "Wer hat am meisten Farbe" waere das Naheliegende und ist falsch. Ein
  // falscher Farbtraeger toetet die Farbe nicht nur, er kann sie auch
  // erfinden -- und zwar gerade dann, wenn die richtige Norm nichts anzeigt.
  // Am 30.08. um 14:57 Uhr, an einem fast grauen Bild: PAL B (richtig) 0,006,
  // SECAM B (falsch) 0,424. Nach Farbmenge gewaenne die Fehldekodierung um
  // Faktor siebzig.
  //
  // Was die beiden trennt, ist nicht die Menge, sondern der Ort. Ein richtig
  // dekodiertes Bild hat farbige Mitten und neutrale Tiefen -- Schwarz ist
  // schwarz, weil dort nichts zu modulieren ist. Ein falscher Traeger legt
  // seine Schwebung gleichmaessig ueber alles, Mitten wie Tiefen.
  //
  // Daraus wird ein Test, den ein einzelner Kandidat fuer sich besteht oder
  // nicht: wie viel Farbe in den dunklen Stellen steht. Die Messreihe dazu und
  // der Grund, warum es der absolute Wert ist und nicht sein Anteil an der
  // Gesamtfarbe, stehen bei kDarkTinted.
  //
  // Der Test greift nur, wo er etwas messen kann: unterhalb kChromaConfident
  // sind die Tiefen ein Rauschwert und sagen nichts, und ohne dunkle Stellen
  // im Bild gibt es sie gar nicht. In beiden Faellen gilt der Kandidat als
  // unbeurteilt -- nicht als bestaetigt und nicht als widerlegt.
  //
  // Der zweite Ausschlussgrund braucht die Tiefen gar nicht: die Farbe, die
  // von Zeile zu Zeile umklappt. Er faengt den Fall, den der erste
  // strukturell nicht sehen kann -- zwei Normen auf demselben Traeger, die
  // sich nur in der Phasenlage unterscheiden. Der steht bei kAltFlipping.
  //
  // Beide sind Ausschlussgruende und keine Nachteile im Vergleich: wer einen
  // von ihnen erfuellt, ist nicht schlechter dekodiert, sondern falsch.
  enum class Verdict { Unjudged, Plausible, Tinted, Flipping };
  std::vector<Verdict> verdicts(colourCandidates_.size(), Verdict::Unjudged);
  for (size_t i = 0; i < colourCandidates_.size(); ++i) {
    if (flips(colourAltV_[i], colourAltU_[i])) {
      verdicts[i] = Verdict::Flipping;
      CAP_LOG("Video standard: %s flips the colour from line to line (V %.4f, U %.4f) -- the "
              "decoder does not undo the phase alternation, the standard is out",
              VideoStandardName(VideoStandardIndexOf(colourCandidates_[i])), colourAltV_[i],
              colourAltU_[i]);
      continue;
    }
    if (colourEnergies_[i] < kChromaConfident || colourDarks_[i] < 0.0f) continue;
    verdicts[i] = colourDarks_[i] < kDarkTinted ? Verdict::Plausible : Verdict::Tinted;
  }
  const auto ruledOut = [&](size_t i) {
    return verdicts[i] == Verdict::Tinted || verdicts[i] == Verdict::Flipping;
  };

  int best = -1, runnerUp = -1;
  bool byDarks = false;

  // Stufe eins: unter den plausiblen gewinnt das sauberste Schwarz.
  //
  // Hier ist der absolute Vergleich richtig, denn jetzt stehen sich nur noch
  // Kandidaten gegenueber, die denselben Test bestanden haben und dieselbe
  // Szene sehen. Steht einer allein da, ist das ein Befund und kein Zufall:
  // er hat kraeftige Farbe und dazu neutrale Tiefen, und keiner der anderen
  // hat das.
  int dark1 = -1, dark2 = -1;
  for (size_t i = 0; i < colourCandidates_.size(); ++i) {
    if (verdicts[i] != Verdict::Plausible) continue;
    if (dark1 < 0 || colourDarks_[i] < colourDarks_[(size_t)dark1]) {
      dark2 = dark1;
      dark1 = (int)i;
    } else if (dark2 < 0 || colourDarks_[i] < colourDarks_[(size_t)dark2]) {
      dark2 = (int)i;
    }
  }
  // Sauberes Schwarz gewinnt aber nicht gegen deutlich mehr Farbe.
  //
  // Verglichen wird mit dem farbigsten Kandidaten und nicht mit dem
  // zweitsaubersten: welcher die zweitsaubersten Tiefen hat, sagt nichts
  // darueber, wer am meisten dekodiert, und bei mehr als zwei Kandidaten sind
  // das nicht dieselben. Die Begruendung steht bei kDarkYieldsToColourBy.
  //
  // Mitgezaehlt wird dabei auch, wer *unbeurteilt* geblieben ist -- nur die
  // eingefaerbten sind draussen. Unbeurteilt heisst nicht widerlegt, und die
  // Frage hier ist nicht "wer hat den Tiefentest bestanden", sondern "gibt es
  // jemanden, der deutlich mehr dekodiert". Am 31.08.2026 um 15:38:48 hing
  // genau daran die falsche Norm: PAL 60 stand mit Farbe 0,136 da, hatte in
  // dieser Szene aber keinen einzigen dunklen Block und damit keine Tiefen,
  // fiel deshalb aus Stufe eins heraus -- und NTSC 4.43 gewann mit 0,081 als
  // vermeintlich "einzige kraeftig farbige mit neutralen Tiefen". Sie war
  // nicht die einzige, sie war nur die einzige mit dunklen Stellen im Bild.
  int colour1 = -1;
  for (size_t i = 0; i < colourCandidates_.size(); ++i) {
    if (ruledOut(i) || colourEnergies_[i] < 0.0f) continue;
    if (colour1 < 0 || colourEnergies_[i] > colourEnergies_[(size_t)colour1]) colour1 = (int)i;
  }
  const bool darksYield =
      dark1 >= 0 && colour1 >= 0 && colour1 != dark1 &&
      colourEnergies_[(size_t)colour1] > colourEnergies_[(size_t)dark1] * kDarkYieldsToColourBy;
  if (darksYield) {
    CAP_LOG("Video standard: %s has the cleaner shadows (%s against %s), but %s has %.1f times "
            "the colour (%.3f against %.3f) -- the amount of colour decides",
            VideoStandardName(VideoStandardIndexOf(colourCandidates_[(size_t)dark1])),
            darkText(colourDarks_[(size_t)dark1]).c_str(),
            darkText(colourDarks_[(size_t)colour1]).c_str(),
            VideoStandardName(VideoStandardIndexOf(colourCandidates_[(size_t)colour1])),
            colourEnergies_[(size_t)colour1] / colourEnergies_[(size_t)dark1],
            colourEnergies_[(size_t)colour1], colourEnergies_[(size_t)dark1]);
  }

  if (dark1 >= 0 && !darksYield &&
      (dark2 < 0 || colourDarks_[(size_t)dark2] >
                        colourDarks_[(size_t)dark1] * kDarkCleanerBy)) {
    best = dark1;
    runnerUp = dark2;
    byDarks = true;
  }

  // Stufe zwei: geben die Tiefen nichts her -- weil niemand kraeftig genug
  // Farbe hatte oder weil zwei gleich sauber sind --, bleibt es beim alten
  // Verfahren, wer am meisten Farbe hat. Fuer eine wirklich schwarzweisse
  // Quelle ist das nach wie vor die einzige sinnvolle Frage.
  //
  // Wer den Test aber *nicht* bestanden hat, ist hier raus und nicht bloss
  // hinten. Genau daran haengt der graue Fall von 14:57 Uhr: SECAM B haette
  // ihn mit Faktor siebzig gewonnen. Ein eingefaerbtes Schwarz ist ein
  // Ausschlussgrund, kein Nachteil.
  if (best < 0) {
    for (size_t i = 0; i < colourCandidates_.size(); ++i) {
      if (colourEnergies_[i] < 0.0f || ruledOut(i)) continue;
      if (best < 0 || colourEnergies_[i] > colourEnergies_[(size_t)best]) {
        runnerUp = best;
        best = (int)i;
      } else if (runnerUp < 0 || colourEnergies_[i] > colourEnergies_[(size_t)runnerUp]) {
        runnerUp = (int)i;
      }
    }
  }

  const long origin = colourCandidates_.front();
  const float winner = best >= 0 ? colourEnergies_[(size_t)best] : -1.0f;
  const float second = runnerUp >= 0 ? colourEnergies_[(size_t)runnerUp] : -1.0f;
  const float needed = winner >= kChromaSuspect ? kChromaBetterBy : kChromaBetterByFaint;
  // Ueber die Tiefen entscheidet auch ein einzelner Kandidat, denn dort hat er
  // etwas bestanden. Ueber die blosse Farbmenge dagegen ist ein einzelner nur
  // eine gelungene Messung ohne Vergleich, und die entscheidet nichts.
  const bool alone = best >= 0 && runnerUp < 0;
  // sceneChanged sticht alles: hat die Runde zwei Szenen gesehen, sind die
  // Zahlen nicht falsch, sie gehoeren nur nicht zusammen. Dann entscheidet
  // hier nichts, und der Weg unten -- zurueck zum Ausgangspunkt, spaeter noch
  // einmal -- ist derselbe wie bei einer zu farbarmen Szene.
  const bool decided =
      !sceneChanged &&
      (byDarks || (best >= 0 && !alone && winner >= kChromaFloor && winner > second * needed));

  const long chosen = best >= 0 ? colourCandidates_[(size_t)best] : origin;
  const long runnerUpStandard = runnerUp >= 0 ? colourCandidates_[(size_t)runnerUp] : 0;
  const float winnerDark = best >= 0 ? colourDarks_[(size_t)best] : -1.0f;
  const float secondDark = runnerUp >= 0 ? colourDarks_[(size_t)runnerUp] : -1.0f;
  const float originEnergy = colourEnergies_.front();
  const float originDark = colourDarks_.front();
  const bool originFlips = verdicts.front() == Verdict::Flipping;

  colourCandidates_.clear();
  colourEnergies_.clear();
  colourDarks_.clear();
  colourAltV_.clear();
  colourAltU_.clear();
  colourIndex_ = 0;
  colourStartedQpc_ = 0;

  // Die Karte steht jetzt auf dem zuletzt gemessenen Kandidaten. In jedem Fall
  // muss sie da weg -- entweder auf den Sieger oder zurueck auf den Anfang.
  const long target = decided ? chosen : origin;
  if (current != target) {
    capture_.SetStandard(target);
    standardSeqAtSet_ = signalSeq_.load(std::memory_order_acquire);
    renderer_.ResetChroma();
  }

  if (decided) {
    if (alone) {
      CAP_LOG("Video standard: %s is the only one strongly coloured with neutral shadows (colour "
              "%.3f, shadows %s) -- set",
              VideoStandardName(VideoStandardIndexOf(chosen)), winner, darkText(winnerDark).c_str());
    } else if (byDarks) {
      CAP_LOG("Video standard: %s has more neutral shadows than %s (%s against %s, both coloured) "
              "-- set",
              VideoStandardName(VideoStandardIndexOf(chosen)),
              VideoStandardName(VideoStandardIndexOf(runnerUpStandard)),
              darkText(winnerDark).c_str(), darkText(secondDark).c_str());
    } else {
      CAP_LOG("Video standard: %s (colour %.3f) has more colour than %s (%.3f) -- set",
              VideoStandardName(VideoStandardIndexOf(chosen)), winner,
              VideoStandardName(VideoStandardIndexOf(runnerUpStandard)), second);
    }
    colourCheckedStandard_ = chosen;
    standardLastGood_ = chosen;
    colourAttempts_ = 0;
    // Bewiesen hat sich der Sieger nur mit einem Befund: sauberen Tiefen oder
    // kraeftiger Farbe. Ein knapper Sieg auf einem flauen Bild ist bloss der
    // beste Kandidat, und den behaelt die Nachkontrolle im Auge.
    colourProven_ = byDarks || winner >= kChromaConfident;
    // Wer gefragt hat, bekommt die Antwort dort, wo er die Frage gestellt hat.
    // Der Toast bleibt der Automatik: er ist die Nachricht ueber etwas, das
    // von selbst geschehen ist, und beides zugleich zu zeigen, hiesse
    // dieselbe Sache zweimal an zwei Stellen zu sagen.
    const bool answered = standardManualSearch_;
    FinishManualStandardSearch(
        chosen, chosen != origin
                    ? Format(T("Nach Farbe berichtigt — vorher %s", "Corrected by colour — was %s"),
                             VideoStandardName(VideoStandardIndexOf(origin)))
                    : std::string(T("Die Farbe bestätigt sie — es bleibt dabei",
                                    "The colour confirms it — no change")));
    if (!answered && chosen != origin) {
      host_.Toast(Format(T("Videonorm nach Farbe berichtigt: %s", "Video standard corrected by colour: %s"),
                   VideoStandardPickerName(chosen).c_str()));
    }
    return;
  }

  // Der Vergleich ist hin -- die Ausgangsnorm ist es deshalb nicht.
  //
  // "Verworfen" heisst: die Kandidaten haben verschiedene Szenen gesehen, also
  // sagt ihr Verhaeltnis zueinander nichts. Ueber die Ausgangsnorm allein sagt
  // das nichts aus. Ihre Messung ist eine vollstaendige Messung einer
  // einzelnen Norm, und fuer die gibt es oben laengst ein Urteil: kraeftig
  // Farbe und neutrale Tiefen heisst richtig dekodiert. Es ist woertlich
  // dieselbe Pruefung wie die im Normalbetrieb, nur spaeter im Ablauf -- also
  // wird sie hier gestellt statt eine Wiederholung dafuer zu bezahlen.
  //
  // Am 31.08.2026 um 05:24 hat genau das drei Sekunden gekostet. Die Runde
  // wurde um 39.908 verworfen, und in derselben Zeile stand PAL 60 mit Farbe
  // 0,194 und Tiefen 0,107 -- beides klar innerhalb der Schwellen. Der
  // Wiederholer stellte um 42.573 fest, was schon dagestanden hatte:
  // "PAL 60 hat deutlich Farbe (0.192), dunkle Bereiche neutral (0.107)".
  //
  // Neu ist daran kein Massstab. Damit eine falsche Norm hier durchkaeme,
  // muesste sie kraeftig Farbe *und* neutrale Tiefen zeigen, und das ist die
  // Beschreibung einer richtigen -- ein falsch dekodiertes SECAM lag in den
  // Tiefen bei 0,353 gegen eine Schwelle von 0,18.
  //
  // Dazu gehoert inzwischen auch, dass ihre Farbe nicht von Zeile zu Zeile
  // kippt -- dieselbe Luecke wie im ersten Durchgang, siehe dort.
  if (sceneChanged && !originFlips && originEnergy >= kChromaConfident && originDark >= 0.0f &&
      originDark < kDarkTinted) {
    CAP_LOG("Video standard: comparison discarded, but %s stands on its own (colour %.3f, dark "
            "areas %s) -- no change",
            VideoStandardName(VideoStandardIndexOf(origin)), originEnergy,
            darkText(originDark).c_str());
    colourCheckedStandard_ = origin;
    standardLastGood_ = origin;
    colourAttempts_ = 0;
    colourProven_ = true;
    FinishManualStandardSearch(
        origin, T("Der Vergleich war unbrauchbar, die Farbe stimmt für sich — es bleibt dabei",
                  "The comparison was unusable, but the colour stands on its own — no change"));
    return;
  }

  // Kein klarer Sieger. Zurueck zum Ausgangspunkt -- und nicht abgehakt,
  // sondern spaeter noch einmal, denn eine graue Szene sagt nichts ueber den
  // Farbtraeger. Erst nach einigen Anlaeufen ist die Quelle wohl wirklich
  // schwarzweiss.
  ++colourAttempts_;
  if (colourAttempts_ >= kColourRetries) {
    // Auch hier zaehlt der Grund. "Die Quelle ist schwarzweiss" ist eine
    // Aussage ueber das Signal, und die darf nicht fallen, wenn gar nicht die
    // Farbe gefehlt hat, sondern die Ruhe. Aufgegeben wird trotzdem: nach drei
    // Anlaeufen ueber gut siebzig Sekunden bewegt sich das Bild eben staendig,
    // und ohne verlaesslichen Vergleich bleibt der Ausgangspunkt das Beste,
    // was wir haben.
    if (sceneChanged) {
      CAP_LOG("Video standard: the picture moved in all %d attempts -- no reliable comparison "
              "possible, staying on %s",
              colourAttempts_, VideoStandardName(VideoStandardIndexOf(origin)));
    } else {
      CAP_LOG("Video standard: nothing decides after %d attempts -- the source is probably black "
              "and white, staying on %s (colour %.3f, dark areas %s)",
              colourAttempts_, VideoStandardName(VideoStandardIndexOf(origin)), originEnergy,
              darkText(originDark).c_str());
    }
    // Aufgegeben ist nicht bestaetigt: die Nachkontrolle schaut weiter hin,
    // und ein spaeteres buntes Bild kann die Frage noch beantworten.
    colourCheckedStandard_ = origin;
    colourProven_ = false;
    FinishManualStandardSearch(
        origin, sceneChanged
                    ? T("Das Bild war jedesmal in Bewegung — kein verlässlicher Vergleich",
                        "The picture moved every time — no reliable comparison")
                    : T("Nichts entscheidet — die Quelle ist wohl schwarzweiß",
                        "Nothing decides — the source is probably black and white"));
    return;
  }
  const double wait = sceneChanged ? kColourMotionRetrySeconds
                                   : kColourRetryBaseSeconds * (double)(1 << (colourAttempts_ - 1));
  CAP_LOG("Video standard: comparison %s -- again in %.0f s, %s until then",
          sceneChanged ? "discarded, the picture changed meanwhile"
                       : "undecided, the scene has too little colour",
          wait, VideoStandardName(VideoStandardIndexOf(origin)));
  colourRetryQpc_ = now + SecondsToTicks(wait);
  // Auch das ist eine Antwort, und zwar die letzte, die der Tastendruck noch
  // bekommt. Der Wiederholer laeuft weiter -- aber er laeuft in wachsenden
  // Abstaenden bis zu gut einer Minute, und so lange auf eine Einblendung zu
  // warten, die vielleicht nie kommt, ist keine Auskunft. Was der Wiederholer
  // spaeter entscheidet, meldet wieder der Toast.
  FinishManualStandardSearch(
      origin, Format(sceneChanged
                         ? T("Das Bild hat sich während des Vergleichs geändert — in %.0f s noch "
                             "einmal",
                             "The picture changed during the comparison — trying again in %.0f s")
                         : T("Die Szene ist zu farbarm — in %.0f s noch einmal",
                             "The scene has too little colour — trying again in %.0f s"),
                     wait));
}

void VideoStandardSearch::ResetStandardColourCheck() {
  colourCheckedStandard_ = 0;
  colourCandidates_.clear();
  colourEnergies_.clear();
  colourDarks_.clear();
  colourAltV_.clear();
  colourAltU_.clear();
  colourIndex_ = 0;
  colourSettleUntilQpc_ = 0;
  colourStartedQpc_ = 0;
  colourRetryQpc_ = 0;
  colourAttempts_ = 0;
  colourWaitingForPicture_ = false;
  colourDoubt_ = ColourDoubt::None;
  colourProven_ = false;
  colourWatch_ = ColourWatch::Watching;
  colourWatchStandard_ = 0;
  colourWatchStrikes_ = 0;
  colourNotice_.clear();
  // Der Wunsch nach einem Rundgang von Hand bleibt dagegen stehen. Ein
  // Graphenumbau setzt hier alles zurueck, und genau einer laeuft haeufig
  // gerade dann, wenn die Taste gedrueckt wird -- der Wunsch waere weg, bevor
  // er einmal drankam. Seine eigene Frist beendet ihn.
  //
  // Auch der Takt zurueck: der Abbruch kann von aussen kommen -- Signal weg,
  // Graph neu -- und dann laeuft VerifyStandardColour nicht mehr, das den Takt
  // sonst selbst zuruecknimmt.
  renderer_.SetChromaCadence(8);
  renderer_.ResetChroma();
}

double VideoStandardSearch::ResultSeconds() { return kStandardResultSeconds; }

double VideoStandardSearch::ResultAgeSeconds() const {
  const double left = TicksToSeconds(standardResultUntilQpc_ - ClockTicks());
  return kStandardResultSeconds - left;
}

void VideoStandardSearch::StartOver() {
  standardCandidate_ = -1;
  standardSweeps_ = 0;
  standardNextTryQpc_ = 0;
  standardPatientPass_ = false;
  ResetStandardColourCheck();
}

}  // namespace cap
