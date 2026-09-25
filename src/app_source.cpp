// What the source is: analogue or not, which connector, whether a signal is
// there, and the picture settings that apply to it.

#include "app.h"

#include <cmath>

namespace cap {
namespace {

// How long without a frame before we call it "no signal".
const double kNoSignalSeconds = 1.5;
// How long a flat picture is given before it counts as no signal rather than as
// a black screen. Generous, because being wrong here means covering a game that
// was only fading out; a title screen that stays black for eight seconds is rare
// and a card with nothing on it stays black forever.
const double kFlatSeconds = 8.0;
// Unless the decoder also says it has no lock, in which case there is no reason
// to keep waiting on a second opinion.
const double kFlatUnlockedSeconds = 2.0;
// Snow is the confident verdict, but not by as much as a synthetic test
// suggested. Measured against a real racing demo on this card, steady fast
// motion reads a change of 37 against a threshold of 48, and a fade-and-cut
// between scenes produced a burst of Snow, Picture, Flat, Picture, Snow inside
// 1.2 seconds. Two seconds clears that comfortably and costs nothing where it
// matters: an unterminated input does not stop being snow after two seconds.
const double kSnowSeconds = 2.0;

}  // namespace

// Frames arriving is the whole answer on a digital input and no answer at all on
// an analogue one, where the card keeps delivering whatever an open wire decodes
// to. So the pixels are asked as well -- and the decoder's own lock is used only
// to say "no" faster, never to say "yes": this card reports a lock on some
// standards with nothing connected, which is measured and written down in the
// wiki.
bool App::HaveLiveSignal() const {
  const FrameBuffer* sink = capture_.sink();
  if (!sink || !sink->HasRecentFrame(kNoSignalSeconds) || !renderer_.hasFrame()) return false;

  // Solange die automatische Normensuche laeuft, sind die Pixel kein Zeuge.
  //
  // Jeder Normwechsel wirft fuer einen Moment breite gruene Streifen durch das
  // blaue Bild, waehrend der Decoder neu synchronisiert. Ein einziger solcher
  // Blitz liest sich als "Picture", nimmt unten die Abkuerzung und setzt die
  // Geduld wieder auf null -- bei einer Suche, die alle paar hundert
  // Millisekunden umschaltet, kommt die Kein-Signal-Anzeige deshalb nie.
  //
  // Nach einer vollen Runde ohne Lock ist die Sache aber entschieden: es wurde
  // jede Norm durchprobiert, die die Karte kann, und keine hat gegriffen. Was
  // das Bild dann noch zeigt, haben wir selbst verursacht.
  //
  // Eine volle Runde ist seit dem zweiten Durchgang beides zusammen. Der
  // schnelle allein entscheidet nichts -- er darf zu ungeduldig gewesen sein,
  // das ist sein Zweck --, und "kein Signal" auf eine Auskunft zu stuetzen,
  // die gerade nachgeprueft wird, waere voreilig. `!standardPatientPass_`
  // trifft genau den Zustand nach einem beendeten geduldigen Durchgang: die
  // Fahne steht waehrend des zweiten Durchgangs und faellt mit seinem Ende.
  if (standardSearch_.sweptWithoutLock() && standardSearch_.signalLocked() == 0) {
    return false;
  }

  const VideoRenderer::SignalVerdict verdict = renderer_.detectedSignal();
  if (verdict == VideoRenderer::SignalVerdict::Picture ||
      verdict == VideoRenderer::SignalVerdict::Unknown) {
    return true;
  }

  // Neither of the two failure verdicts is acted on the instant it appears.
  //
  // Snow is the confident one, but a hard cut between two busy scenes can clear
  // both its thresholds for a single measurement, and a viewer that blinks the
  // idle screen mid-game is worse than one that takes a moment to notice a
  // pulled cable. A second is far longer than any cut and far shorter than
  // anyone's patience with a dead input.
  //
  // Flat is the ambiguous one: a black loading screen measures exactly like a
  // muted input, so only time separates them at all. A decoder that says it has
  // no lock is reason enough to stop waiting for a second opinion.
  //
  // An einem digitalen Eingang laeuft der Wachthread gar nicht, `locked` ist
  // dann -1 und es bleibt bei der langen Geduld. Das ist genau richtig: dort
  // sitzt der Decoder nicht im Signalweg, seine Meinung waere geraten.
  double patience = kSnowSeconds;
  if (verdict == VideoRenderer::SignalVerdict::Flat) {
    const int locked = standardSearch_.signalLocked();
    patience = locked == 0 ? kFlatUnlockedSeconds : kFlatSeconds;
  }
  return renderer_.signalHeldSeconds() < patience;
}

// Ob das Bild aus dem Analogdekoder kommt. Dieselbe Frage, die entscheidet, was
// in den Einstellungen erscheint -- und sie muss dieselbe Antwort geben, sonst
// wirkt etwas, das nirgends mehr zu sehen ist.
//
// Einen Dekoder zu haben heisst nicht, ihn zu benutzen: auf einer Karte, die
// beides kann, wird er weiterhin gemeldet, waehrend das Bild vom digitalen
// Eingang kommt. Keine Videonorm liefert mehr als 576 Zeilen, also beantwortet
// das Bild die Frage selbst.
bool App::SourceIsAnalogue() const {
  const SignalKind kind = config_.active().capture.signalKind;
  if (kind == SignalKind::Analog) return true;
  if (kind == SignalKind::Digital) return false;
  const DeviceProbeResult& caps = capture_.capabilities();
  if (!capture_.running() || caps.availableStandards == 0) return false;
  // Der Eingang weiss es besser als die Bildhoehe: HDMI in 720x576 ist
  // digital, und genau so lief die Normsuche am 25.09. auf dem HDMI-Eingang
  // der SA7160 los.
  int index = config_.active().capture.crossbarInput;
  if (index < 0) index = caps.currentInput;
  if (index >= 0 && index < (int)caps.crossbarInputs.size() &&
      caps.crossbarInputs[(size_t)index].kind == ConnectorKind::Digital)
    return false;
  const VideoFormatInfo fmt = renderer_.sourceFormat();
  return !fmt.valid() || fmt.height <= 576;
}

// Welcher Anschluss, wenn die Einstellung auf Automatisch steht.
//
// Die Karte weiss es, wo sie ihre Eingaenge ueberhaupt offenlegt:
// `EnumerateCrossbarInputs` liest den physischen Typ jedes Eingangs mit aus --
// aus dem Crossbar, wo es einen gibt, sonst aus dem privaten Selektor des
// Herstellers, wenn qBlank ihn kennt (die SA7160 hat so einen).
//
// Gefragt wird zuerst nach dem eingestellten Eingang und, wo keiner eingestellt
// ist, nach dem, auf dem die Karte tatsaechlich steht. Das ist nicht dasselbe:
// "Nicht aendern" heisst, dass qBlank den Selektor in Ruhe laesst, nicht dass
// niemand wuesste, wo er steht. Nur wenn beides nichts hergibt, bleibt die
// Annahme.
//
// Die Annahme ist Composite, und zwar in die sichere Richtung. Ein zu viel
// angebotener Filter steht auf null und tut nichts, bis jemand ihn anfasst;
// ein zu wenig angebotener fehlt dort, wo er gebraucht wird, und der Nutzer
// sieht das Kriechen und findet den Regler nicht mehr.
AnalogConnector App::ResolvedConnector() const {
  const AnalogConnector chosen = config_.active().capture.connector;
  if (chosen != AnalogConnector::Auto) return chosen;

  const std::vector<CrossbarInput>& inputs = capture_.capabilities().crossbarInputs;
  int index = config_.active().capture.crossbarInput;
  if (index < 0) index = capture_.capabilities().currentInput;
  if (index >= 0 && index < (int)inputs.size()) {
    switch (inputs[(size_t)index].kind) {
      case ConnectorKind::SVideo:
        return AnalogConnector::SVideo;
      case ConnectorKind::Component:
      case ConnectorKind::Rgb:
        // RGB kommt wie Component ohne Farbtraeger an -- drei Leitungen, jede
        // ihr eigenes Signal. Fuer alles, was hier davon abhaengt, ist das
        // derselbe Fall.
        return AnalogConnector::Component;
      // SCART absichtlich nicht: derselbe Stecker fuehrt je nach Kabel
      // Composite oder RGB, und welches davon steckt, sagt der Typ nicht. Also
      // die sichere Annahme, und wer es besser weiss, stellt es ein.
      default:
        break;
    }
  }
  return AnalogConnector::Composite;
}

bool App::ConnectorMixesLumaAndChroma() const {
  return SourceIsAnalogue() && ResolvedConnector() == AnalogConnector::Composite;
}

bool App::ConnectorHasColourCarrier() const {
  return SourceIsAnalogue() && ResolvedConnector() != AnalogConnector::Component;
}

// Die Einstellungen, wie sie fuer *diese* Quelle gelten.
//
// Ausgeblendet muss auch abgeschaltet heissen. Wer am SNES die Kriechfilter und
// das native Raster anhatte und dann eine HD-Konsole ansteckt, saehe sonst ein
// Bild, das er nicht will, und faende den Regler dafuer nirgends mehr -- die
// Einstellung ist ja gerade verschwunden, weil sie nicht passt.
//
// Neutralisiert wird nur die Kopie, die gezeichnet wird. Das Profil behaelt
// seine Werte, denn es beschreibt eine Konsole, und die kommt wieder. Was es
// nicht tut, ist sich zu merken und wiederherzustellen: die naechste analoge
// Quelle ist vielleicht eine andere Konsole mit anderen Werten. Genau dafuer
// gibt es Profile.
ImageSettings App::EffectiveImage(const Profile& profile) const {
  ImageSettings img = profile.image;
  const VideoFormatInfo fmt = renderer_.sourceFormat();
  img.compare = compare_;

  if (!SourceIsAnalogue()) {
    // Composite bringt diese Stoerungen mit, ein digitaler Eingang nicht. Der
    // Demodulator wuerde einen Traeger herausrechnen, den es nicht gibt.
    img.chromaSoft = 0;
    img.temporalDenoise = 0.0f;
    img.dotNotch = 0.0f;
    // Und die Bandanhebung ebenso: ihre Fensterbreite kommt aus der
    // Traegerfrequenz, und ohne Traeger hebt sie ein Band an, das niemand
    // gedaempft hat. Stand vorher nicht hier -- ausgeblendet war sie schon,
    // abgeschaltet nicht, und das ist genau der Fall, den der Kommentar
    // ueber dieser Funktion verbietet.
    img.bandwidthRestore = 0.0f;
    // Das native Raster rechnet das Abtasten einer analogen Zeile zurueck.
    img.nativeWidth = 0;
    // Der Vergleich bleibt: er zeigt jetzt auch Schaerfen, Bildregler und
    // Bildroehre, und die gibt es an jedem Eingang.
  } else if (!ConnectorMixesLumaAndChroma()) {
    // Analog, aber Helligkeit und Farbe kommen getrennt an -- S-Video auf zwei
    // Leitungen, Component auf drei. Damit faellt alles weg, was Uebersprechen
    // zwischen den beiden behandelt, und das ist kein Feintuning: diese Filter
    // suchen ein Muster auf der Traegerfrequenz und finden dort bei einer
    // sauberen Quelle Bilddetail. Sie wuerden es wegrechnen.
    //
    //   chromaSoft       gegen Regenbogen, also gegen Helligkeitsdetail, das
    //                    der Dekoder als Farbe gelesen hat. Ohne gemeinsame
    //                    Leitung liest er nichts falsch. Bei Component ist die
    //                    Farbbandbreite ausserdem breit genug, dass seitliches
    //                    Weichzeichnen echtes Detail kostet.
    //   adaptiveChroma   ist die Bedingung auf chromaSoft und faellt mit ihm.
    //   dotNotch         rechnet den Farbtraeger aus der Helligkeit heraus. Da
    //                    ist keiner drin.
    //   bandwidthRestore hebt die Daempfung zum Traeger hin wieder an. Ohne
    //                    Traeger gibt es diese Daempfung nicht.
    //
    // Was bleibt, bleibt mit Absicht: temporalDenoise mittelt Rauschen weg --
    // die Traegerausloeschung ist nur die zweite Haelfte seiner Arbeit --, und
    // motionCompensate ist ohnehin um den Traeger herum gesperrt und entfernt
    // Rauschen, "the same defect in every colour system", wie es im Shader
    // steht. Rauschen bringt jede analoge Leitung mit.
    img.chromaSoft = 0;
    img.adaptiveChroma = false;
    img.dotNotch = 0.0f;
    img.bandwidthRestore = 0.0f;
  }

  // Verdoppeln nur, wo wirklich die halbe Bildhoehe ankommt.
  //
  // Das Kaestchen wird sonst gar nicht erst gezeigt, aber ein gespeichertes
  // Haekchen aus einer Sitzung mit 240p-Quelle wuerde hier weiterwirken -- und
  // zwar unsichtbar: das Fenster passt das Bild auf das eingestellte
  // Seitenverhaeltnis, die Aufnahme dagegen kaeme doppelt so hoch heraus.
  if (!SourceIsAnalogue() || !fmt.valid() || fmt.height > kHalfHeightLines) {
    img.lineDouble = false;
  }

  // Das native Raster faellt ueber dem Standardraster weg. Es rechnet in Proben
  // einer analogen Zeile, und oberhalb von 576 Zeilen hat etwas dazwischen
  // hochgerechnet -- ein Dongle, ein Skalierer, die Karte selbst. Die Kanten
  // liegen dann nicht mehr dort, wo diese Zahl sie sucht, und das Zuordnen nimmt
  // Detail weg statt welches zurueckzugeben. Der Regler ist dabei schon
  // ausgeblendet; ein gespeicherter Wert wirkte ohne diese Zeile weiter.
  //
  // Die Zeilenzahl der Quelle hilft hier ausdruecklich *nicht* nach. Sie sagt
  // etwas ueber senkrecht, und dieses Raster liegt waagerecht: dass jemand weiss,
  // wie viele Zeilen seine Konsole zeichnet, macht die 720 Proben je Zeile nicht
  // wieder zu denen, die die Karte einmal genommen hat.
  if (fmt.valid() && fmt.height > kStandardLines) {
    img.nativeWidth = 0;
  }

  // Bildroehreneffekte werden hier nicht mehr abgeschaltet, und das ist der
  // Unterschied zu vorher. Sie versteckten sich frueher ueber 576 Zeilen, also
  // mussten sie hier mit weg; jetzt bleiben sie stehen und sagen selbst, wenn
  // kein Platz ist. Die Maske braucht ohnehin nur das Fenster, und die
  // Zeilenluecken lehnen im Shader von allein ab, wo sie nur aliasen wuerden.

  // Halbbilder: hat die Quelle keine, darf ein von Hand gewaehlter Deinterlacer
  // nicht trotzdem laufen. Nicht abschalten, sondern auf "nur bei interlaced"
  // stellen -- das ist dieselbe Aussage und ueberlebt den Wechsel zurueck.
  const bool hasFields =
      fmt.interlaced || renderer_.detectedInterlace() == VideoRenderer::InterlaceVerdict::Interlaced;
  if (!SourceIsAnalogue() && !hasFields) img.deinterlaceAuto = true;

  // Zuletzt, damit nichts darueber es wieder einschaltet. Siehe ToggleBypass.
  if (bypass_) {
    img.chromaSoft = 0;
    img.adaptiveChroma = false;
    img.temporalDenoise = 0.0f;
    img.dotNotch = 0.0f;
    img.motionCompensate = false;
    img.bandwidthRestore = 0.0f;
    img.compare = false;
    img.sharpen = 0.0f;
    img.brightness = 0.0f;
    img.contrast = 1.0f;
    img.saturation = 1.0f;
    img.hue = 0.0f;
    img.nativeWidth = 0;
    img.scanlines = 0.0f;
    img.mask = 0;
  }

  return img;
}

// Ob das gemessene "interlaced" bei dieser Quelle nach einem Irrtum aussieht.
//
// Kein Veto, sondern eine Nachfrage, und das mit Absicht: die Messung kann hier
// nicht widerlegt werden. Ein bildschirmfuellendes Foto von Kammlinien ist
// raeumlich von Kammlinien nicht zu unterscheiden, und genau das steht auf
// einem erfassten Desktop schnell einmal im Browser. Also entscheidet weiter
// die Messung, und danebengestellt wird der Satz, den ein Mensch braucht, um
// den Fehler in zwei Sekunden selbst zu beheben.
//
// Vier Bedingungen, jede mit einem eigenen Grund:
// * Die Karte hat *nicht* selbst interlaced gemeldet. Sagt sie es, stimmt es,
//   und dann gibt es nichts zu bezweifeln.
// * Die Quelle ist digital. An einem Analogeingang ist interlaced der
//   Normalfall und die Messung ohnehin die einzige Auskunft.
// * Mindestens 720 Zeilen. Bei 720 ist die Sache klar -- ein 720i hat es nie
//   gegeben. Bei 1080 ist sie es nicht, 1080i gab es im Fernsehen wirklich,
//   und deshalb steht hier eine Frage und keine Behauptung. Darunter, etwa bei
//   480i von einem DVD-Spieler ueber HDMI, ist interlaced schlicht richtig.
//
// Die Haelfte dieser Faelle beantwortet inzwischen die Bildrate von selbst:
// kommen bei 720 Zeilen oder mehr fuenfzig oder sechzig Bilder in der Sekunde
// an, laesst die Erkennung "interlaced" gar nicht mehr zu (siehe
// VideoRenderer::SetFrameRateHint). Uebrig bleibt hier also die Quelle, die mit
// 25 oder 30 Bildern ankommt und kaemmt -- und die kann eben beides sein, ein
// echtes 1080i und ein erfasster Bildschirm, auf dem ein Video davon laeuft.
// * Die automatische Erkennung ist eingeschaltet. Ist sie es nicht, laeuft der
//   Deinterlacer sowieso und die Messung aendert am Bild nichts -- vor etwas zu
//   warnen, das gar nicht wirkt, zeigt in die falsche Richtung.
bool App::InterlaceVerdictDoubtful(const Profile& profile) const {
  if (!profile.image.deinterlaceAuto) return false;
  if (renderer_.detectedInterlace() != VideoRenderer::InterlaceVerdict::Interlaced) return false;
  const VideoFormatInfo fmt = renderer_.sourceFormat();
  if (!fmt.valid() || fmt.interlaced) return false;
  if (SourceIsAnalogue()) return false;
  return fmt.height >= 720;
}

// Ein analoger Eingang, an dem der volle Wertebereich ankommt. Ein Dekoder fuer
// Composite, S-Video oder Tuner liefert BT.601, und das heisst 16-235: Schwarz
// liegt auf 16, weil die Norm es dort hinlegt, nicht weil eine Quelle sich so
// entschieden hat. Liegt es stattdessen unten am Anschlag, hat irgendwer davor
// gestreckt -- ueblicherweise die Karte, auf deren Wertebereichsschalter. An
// einem digitalen Eingang sagt dasselbe Bild nichts: ein PC-Desktop hat
// reichlich echte Nullen und echte 255er, und die sind keine Fehlfunktion.
//
// Mehr steht hier nicht drin, und das ist Absicht. Ob beim Strecken etwas
// hinausgefallen ist, laesst sich von hier aus *nicht* feststellen: die
// Verstaerkung sitzt vor dem Wandler, also fehlt der Kamm, an dem man eine
// digitale Streckung erkennen wuerde, und die Stapel an den Enden hat der
// Dekoder ohnehin -- BT.601 verbietet 0 und 255, alles Dunklere kommt auf 1 an
// und alles Hellere auf 254. Ein Versuch, daraus ein Urteil zu bauen, hat
// zuverlaessig auch dann angeschlagen, wenn nichts anlag.
//
// Deshalb ist das ein Hinweis im Reiter Bild und keine Warnung: er sagt, was
// gemessen wurde und wo man nachsieht, und behauptet keinen Schaden.
bool App::AnalogueRangeIsFull() const {
  if (captureState_ != CaptureState::Running) return false;
  if (!SourceIsAnalogue()) return false;
  return renderer_.detectedRange() == VideoRenderer::RangeVerdict::Full;
}

// Eine Aufloesung, die nicht zum Raster der anliegenden Norm passt: 720x480
// an einer PAL-Quelle, 720x576 an NTSC, oder 1080p an einem analogen Eingang.
// Die Karte skaliert dann die Zeilen, und das Bild sieht weich aus oder hat
// Balken, ohne dass irgendwo steht, warum. Beim Wechsel der Norm laesst
// ReleaseStandardBoundFormat die alte Groesse von selbst los; das hier faengt
// den Rest ein -- eine von Hand gewaehlte oder aus einem alten Profil
// mitgebrachte.
//
// Voll (576, 480 oder 486) und halb (288, 240) passen beide. Gemeldet wird nur,
// wenn die Norm feststeht -- eingerastet und keine Suche unterwegs -- und die
// Karte fuer das Pixelformat eine passende Groesse wirklich anbietet; sonst
// gaebe es nichts, worauf der Hinweis zeigen koennte.
int App::ResolutionMismatchLines() const {
  if (captureState_ != CaptureState::Running || !SourceIsAnalogue()) return 0;
  if (standardSearch_.signalLocked() != 1) return 0;
  const SettingsWindow::StandardSearch search = standardSearch_.StandardSearchDisplay();
  if (search != SettingsWindow::StandardSearch::Off &&
      search != SettingsWindow::StandardSearch::Result) {
    return 0;
  }
  const int lines = VideoStandardLines(standardSearch_.signalStandard());
  if (lines <= 0) return 0;
  const int active = lines >= 600 ? 576 : 480;
  const VideoFormatInfo fmt = renderer_.sourceFormat();
  if (!fmt.valid()) return 0;
  if (std::abs(fmt.height - active) <= 16 || std::abs(fmt.height - active / 2) <= 8) return 0;
  const ResolutionOption fit =
      capture_.capabilities().caps.FittingResolution(capture_.connectedFormat().subtype, active);
  if (fit.width <= 0) return 0;
  return active;
}

// Dasselbe wie der Knopf im Reiter Quelle, nur von hier aus. Der Neubau folgt
// von selbst: SyncConfigChanges sieht das andere Format.
//
// Nichts zu tun, wenn die passende Groesse schon eingestellt ist. Dann liefert
// die Karte trotzdem etwas anderes, und ein zweiter Versuch mit denselben
// Zahlen braechte nur denselben Neubau noch einmal.
bool App::ApplyFittingResolution(int activeLines) {
  const ResolutionOption fit = capture_.capabilities().caps.FittingResolution(
      capture_.connectedFormat().subtype, activeLines);
  FormatSel& f = config_.active().capture.format;
  if (fit.width <= 0 || (f.width == fit.width && f.height == fit.height)) return false;
  CAP_LOG("Resolution %s -> %dx%d to fit %d active lines", f.Label().c_str(), fit.width,
          fit.height, activeLines);
  f.width = fit.width;
  f.height = fit.height;
  // Wie in ReleaseStandardBoundFormat: eine feste Rate war unter der falschen
  // Groesse gewaehlt, die Norm sagt selbst, welche richtig ist.
  if (f.fps > 0.0) f.fps = kFpsNative;
  f.forced = false;
  Toast(Format(T("Auflösung an die Videonorm angepasst: %dx%d",
                 "Resolution matched to the video standard: %dx%d"),
               fit.width, fit.height));
  return true;
}

bool App::SourceLooksInterlaced(const Profile& profile) const {
  if (!profile.image.deinterlaceAuto) return true;
  // The media type is believed when it claims interlaced -- a card that bothers
  // to say so is right. It is not believed when it stays quiet, which is the
  // usual case on an analogue input and is why the picture is measured as well.
  if (renderer_.sourceFormat().interlaced) return true;
  return renderer_.detectedInterlace() == VideoRenderer::InterlaceVerdict::Interlaced;
}

}  // namespace cap
