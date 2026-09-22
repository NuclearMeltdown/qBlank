#include "capture/video_capture.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "common.h"
#include "i18n.h"

namespace cap {
namespace {

std::string BusyMessage(const DeviceRef& device) {
  const std::string name = device.name.empty() ? device.id : device.name;
  return T("Das Gerät '", "Device '") + name +
         T("' wird bereits von einem anderen Programm benutzt. Die meisten Karten geben "
           "ihren Videopin nur einmal her — schließe die andere qBlank-Instanz, OBS oder "
           "das Hersteller-Tool. qBlank versucht es von allein weiter.",
           "' is already in use by another program. Most cards hand out their video pin "
           "only once, so close the other qBlank window, OBS or the vendor tool. qBlank "
           "keeps retrying on its own.");
}

// The requested format first, then the same resolution in every other format
// the card offers, cheapest first, and finally the driver's own default.
// Welche Videonorm bei dieser Quelle ankommt, oder 0, wenn es sich nicht sagen
// laesst. Aus ihr kommen zwei Dinge: das Raster, an dem die Aufloesung haengt,
// und die Halbbildrate.
//
// Die Frage ist nicht, ob die Karte einen Analogdekoder hat: eine Karte, die
// beides kann, meldet ihn auch dann, wenn das Bild ueber HDMI hereinkommt. Die
// Frage ist, ob das Bild durch ihn hindurchgeht, und beantwortet wird sie am
// Eingang -- ausdruecklich nicht an der Bildhoehe, denn die ist gerade das, was
// hier herauskommen soll.
//
// Gefragt wird zuerst nach dem eingestellten Eingang, dann nach dem, auf dem die
// Karte steht, und wo beides nichts hergibt, bleibt die Annahme: eine Karte, die
// Videonormen meldet und ihre Eingaenge nicht offenlegt, ist eine SD-Karte, und
// an der haengt Composite. Dieselbe Reihenfolge und dieselbe Annahme wie in
// App::ResolvedConnector -- zwei Stellen, die verschieden raten, waeren schlimmer
// als eine, die falsch raet.
long NativeStandardOf(const DeviceProbeResult& caps, int chosenInput, bool digital) {
  if (digital) return 0;
  if (caps.availableStandards == 0) return 0;  // kein Analogdekoder, keine Norm

  int index = chosenInput;
  if (index < 0) index = caps.currentInput;
  if (index >= 0 && index < (int)caps.crossbarInputs.size() &&
      !ConnectorFollowsVideoStandard(caps.crossbarInputs[(size_t)index].kind)) {
    return 0;
  }
  return caps.currentStandard;
}

std::vector<FormatSel> BuildFormatCandidates(const FormatSel& wanted, const CapsModel& caps,
                                             const CaptureDevice& device) {
  // Cost of one frame in bytes, used to order fallbacks from cheapest upwards.
  auto frameBytes = [&device](const FormatSel& f) {
    const size_t size = device.FrameBytes(f.subtype, f.width, f.height);
    return size ? size : SIZE_MAX;  // compressed formats sort last
  };

  std::vector<FormatSel> out;
  if (wanted.valid()) out.push_back(wanted);

  std::vector<FormatSel> alternatives;
  for (const std::string& subtype : caps.Subtypes()) {
    if (wanted.valid() && subtype == wanted.subtype) continue;
    FormatSel alt = wanted;
    alt.subtype = subtype;
    alt.forced = false;
    if (!alt.valid()) continue;
    alternatives.push_back(alt);
  }
  std::sort(alternatives.begin(), alternatives.end(),
            [&frameBytes](const FormatSel& a, const FormatSel& b) {
              return frameBytes(a) < frameBytes(b);
            });
  out.insert(out.end(), alternatives.begin(), alternatives.end());

  // Zum Schluss, was die Karte selbst vorschlaegt -- und zwar zweimal: erst mit
  // dem gewuenschten Pixelformat, dann ohne.
  //
  // Jeder Kandidat oben traegt die *gewuenschte* Groesse und dreht nur am
  // Pixelformat. Stimmt die Groesse nicht mehr, weil die Karte inzwischen auf
  // einer anderen Zeilenzahl steht, faellt die ganze Liste durch, und uebrig
  // bleibt der Vorschlag der Karte -- der vom Wunsch nichts weiss. Damit war es
  // einer zu wenig: am 31.08.2026 wurde aus einem gewuenschten RGB32 720x480
  // unter PAL B ein YUY2 720x576, obwohl die Karte RGB32 720x576 anbietet. Zu
  // berichtigen war die Groesse, nicht das Pixelformat.
  auto append = [&out](const FormatSel& f) {
    if (!f.valid()) return;
    const bool known = std::any_of(out.begin(), out.end(),
                                   [&](const FormatSel& o) { return o.SameFormat(f); });
    if (!known) out.push_back(f);
  };
  if (!wanted.subtype.empty()) append(caps.PickDefault(wanted.subtype));
  append(caps.PickDefault());
  return out;
}

}  // namespace

VideoCapture::~VideoCapture() {
  Stop();
}

// ------------------------------------------------------------------- probing

DeviceProbeResult VideoCapture::Probe(const DeviceRef& device) {
  DeviceProbeResult out;
  if (device.empty()) {
    out.error = T("Kein Videogerät ausgewählt", "No video device selected");
    return out;
  }

  std::unique_ptr<CaptureDevice> dev = CaptureDevice::Create();
  Said failure;
  if (!dev->Open(device, CaptureDevice::Purpose::Probe, &out.device, &failure)) {
    out.error = failure.shown;
    return out;
  }

  out.availableStandards = dev->AvailableStandards();
  out.currentStandard = dev->CurrentStandard();

  out.caps.Build(dev->Caps());

  dev->RevealInputs();
  out.crossbarInputs = dev->Inputs();
  out.currentInput = dev->CurrentInput();
  // Erst hier, denn die Antwort haengt am Eingang, und der steht erst, seit der
  // Pin verbunden ist. Ein Wunsch des Nutzers ist an dieser Stelle nicht bekannt
  // -- die Karte wird ja gerade erst befragt --, also entscheidet, worauf sie
  // steht.
  out.caps.SetNativeStandard(NativeStandardOf(out, -1, false));

  dev->ReadCurrentFormat(&out.colorInfo);
  out.ok = true;

  // Let go before returning -- the caller may want to open the device for real
  // right afterwards.
  dev->Close();
  return out;
}

// -------------------------------------------------------------------- start

bool VideoCapture::Start(const CaptureSettings& settings, std::string* error) {
  Stop();

  auto fail = [&](const Said& said) {
    if (error) *error = said.shown;
    if (!said.logged.empty()) CAP_ERR("Capture could not be started: %s", said.logged.c_str());
    Teardown();
    return false;
  };

  if (settings.video.empty())
    return fail(CAP_SAID(T("Kein Videogerät ausgewählt", "No video device selected")));

  device_ = CaptureDevice::Create();
  Said failure;
  if (!device_->Open(settings.video, CaptureDevice::Purpose::Stream, &capabilities_.device,
                     &failure)) {
    return fail(failure);
  }

  // Bevor irgendetwas anderes eingestellt wird, und bei jedem Start neu: was in
  // diesen Reglern steht, wird angewendet, bevor das Bild ueberhaupt bei uns
  // ankommt. Ein Dekoder, der den Schwarzwert anhebt, laesst im ganzen Programm
  // keine saubere Fassung mehr uebrig -- und der Schaden ist nicht mehr
  // ruecknehmbar, weil er schon geklemmt hat. Dieselben vier Regler gibt es im
  // Bild-Tab, dort im Shader, wo das Original darunter erhalten bleibt.
  //
  // Die Treiberdialoge (DevicePropertyPages) koennen das jederzeit wieder
  // verstellen. Das ist in Ordnung: es gilt dann bis zum naechsten Start.
  device_->NeutraliseImageControls();

  // Before the formats are read, not after: the standard decides how many lines
  // the card will produce, and therefore which formats it advertises at all.
  capabilities_.availableStandards = device_->AvailableStandards();
  // Eine Videonorm gehoert dem Analogdekoder. Eine Karte, die beides kann, hat
  // ihn trotzdem und meldet die Normen weiterhin -- auf einem HDMI-Eingang sagt
  // das aber nichts ueber das Signal, und PAL dort zu setzen legt die Karte auf
  // 720x576 bei 50 Hz fest, obwohl an der Buchse etwas ganz anderes anliegt.
  //
  // Wer die Quelle ausdruecklich als digital angegeben hat, meint genau das.
  const bool digital = settings.signalKind == SignalKind::Digital;
  if (digital && settings.videoStandard > 0) {
    CAP_LOG("Source is marked digital, video standard not set");
  }
  if (!digital && settings.videoStandard > 0 &&
      (capabilities_.availableStandards & settings.videoStandard) != 0) {
    device_->SetStandard(settings.videoStandard);
  }
  capabilities_.currentStandard = device_->CurrentStandard();

  capabilities_.caps.Build(device_->Caps());

  // Vorlaeufig, fuer die Formatwahl gleich darunter: ein privater Selektor
  // antwortet ohne Graphen, ein Crossbar erst, wenn der Capture-Pin verbunden
  // ist -- und verbunden wird der erst, wenn das Format steht. Wo hier schon
  // etwas zu holen ist, wird es geholt; sonst bleibt stehen, was der vorige Lauf
  // hinterlassen hat, denn umgesteckt wird zwischen zwei Laeufen selten. Die
  // verbindliche Fassung kommt weiter unten, sobald der Graph steht.
  std::vector<CrossbarInput> earlyInputs = device_->Inputs();
  if (!earlyInputs.empty()) {
    capabilities_.crossbarInputs = std::move(earlyInputs);
    capabilities_.currentInput = device_->CurrentInput();
  }
  capabilities_.caps.SetNativeStandard(
      NativeStandardOf(capabilities_, settings.crossbarInput, digital));
  capabilities_.ok = true;

  FormatSel wanted = settings.format;
  if (!wanted.valid()) {
    // A subtype without a size is what re-reading the card leaves behind: the
    // resolution is to be found again, the pixel format is not up for grabs.
    const std::string wish = wanted.subtype;
    // Und die Rate erst recht nicht. PickDefault antwortet mit einem ganzen
    // Format, Rate inbegriffen, und das ist der Vorschlag der Karte -- gefragt
    // war er nicht. Ohne diese Zeile faellt "die des Signals" genau dort unter
    // den Tisch, wo die Aufloesung neu gesucht wird, und die Karte laeuft
    // stillschweigend auf ihrer hoechsten.
    const double wishFps = wanted.fps;
    if (capabilities_.caps.nativeLines() > 0) {
      CAP_LOG("Resolution search: %d lines arrive, upscaled formats are out",
              capabilities_.caps.nativeLines());
    }
    wanted = capabilities_.caps.PickDefault(wish);
    wanted.fps = wishFps;
    if (wish.empty()) {
      CAP_LOG("No format configured, using the default: %s", wanted.Label().c_str());
    } else if (wanted.subtype == wish) {
      CAP_LOG("Resolution searched again, pixel format %s kept: %s", wish.c_str(),
              wanted.Label().c_str());
    } else {
      CAP_WARN("The card no longer offers pixel format %s, instead: %s", wish.c_str(),
               wanted.Label().c_str());
    }
  }

  // Weder "hoechste verfuegbare" noch "die des Signals" ist eine Zahl, und hier
  // wird eine daraus. Spaet mit Absicht: die Zahl kommt dann von der Karte, die
  // wirklich davorsteht, und die Einstellung uebersteht einen Wechsel von
  // 576i50 auf 480p60, ohne dass jemand sie anfasst.
  //
  // Die Rate des Signals zuerst, weil sie die genauere Auskunft ist. Kennt die
  // Karte die Norm nicht -- Digitaleingang, kein Dekoder --, gibt es nichts
  // abzuleiten, und es bleibt bei der hoechsten. Das ist kein Fehler, sondern
  // dieselbe Antwort wie vorher.
  if (wanted.fps < 0.0) {
    const double nat = capabilities_.caps.NativeFps(wanted.subtype, wanted.width, wanted.height);
    if (nat > 0.0) {
      wanted.fps = nat;
      CAP_LOG("Signal frame rate for %dx%d %s: %.3f fps (%s, %d lines, standard %.3f Hz)",
              wanted.width, wanted.height, wanted.subtype.c_str(), nat,
              VideoStandardName(VideoStandardIndexOf(capabilities_.currentStandard)),
              capabilities_.caps.nativeLines(), capabilities_.caps.nativeFieldRate());
    } else {
      wanted.fps = 0.0;
      CAP_LOG("Signal frame rate: no standard known, falling back to the highest");
    }
  }
  if (wanted.fps <= 0.0) {
    const double top = capabilities_.caps.HighestFps(wanted.subtype, wanted.width, wanted.height);
    if (top > 0.0) {
      wanted.fps = top;
      CAP_LOG("Highest available frame rate for %dx%d %s: %.3f fps", wanted.width, wanted.height,
              wanted.subtype.c_str(), top);
    } else {
      CAP_LOG("Highest available frame rate: the card reports none, keeping the driver default");
    }
  }

  if (!device_->AttachSink(&failure)) return fail(failure);

  // A card can say no when the format is set and again when the pins connect.
  // Only a format-shaped refusal is worth retrying with a different format; if
  // the device is simply taken, trying eleven more formats just makes the user
  // wait for the same answer eleven more times.
  // What failed last, kept in both languages so the message can be built in either.
  Said lastRefusal;
  bool connected = false;
  for (const FormatSel& candidate :
       BuildFormatCandidates(wanted, capabilities_.caps, *device_)) {
    Said refusal;
    const CaptureDevice::FormatAttempt attempt = device_->TryFormat(candidate, &refusal);
    if (attempt == CaptureDevice::FormatAttempt::Busy)
      return fail(CAP_SAID(BusyMessage(settings.video)));
    if (attempt == CaptureDevice::FormatAttempt::Accepted) {
      if (!candidate.SameFormat(wanted)) {
        CAP_WARN("Format %s failed, using %s instead", wanted.Label().c_str(),
                 candidate.Label().c_str());
      }
      connectedFormat_ = candidate;
      connected = true;
      break;
    }
    lastRefusal = refusal;
  }

  if (!connected) {
    return fail(CAP_SAID(T("Die Karte hat kein einziges Format akzeptiert. Letzter Fehler: ",
                           "The card accepted none of the formats. Last error: ") +
                         Spoken(lastRefusal)));
  }

  // Only now that the capture pin is connected is the crossbar filter part of
  // the graph, so this is the earliest point at which the input can be routed.
  capabilities_.crossbarInputs = device_->Inputs();
  if (settings.crossbarInput >= 0 && !capabilities_.crossbarInputs.empty()) {
    device_->RouteInput(settings.crossbarInput);
  }
  // Read back afterwards, so this says where the card ended up rather than
  // where it was asked to go -- and says it just as well when it was not asked
  // at all, which is the case that "Automatisch" needs answered.
  capabilities_.currentInput = device_->CurrentInput();

  if (!device_->Run(&failure)) return fail(failure);

  VideoFormatInfo info = format();
  CAP_LOG("Capture running: %s %dx%d @ %.3f fps%s", info.subtypeLabel.c_str(), info.width,
          info.height, info.fps, info.interlaced ? " (interlaced)" : "");
  return true;
}

void VideoCapture::Stop() {
  if (device_) device_->Stop();
  Teardown();
}

void VideoCapture::Teardown() {
  if (device_) device_->Close();
  device_.reset();
  connectedFormat_ = FormatSel{};
}

VideoFormatInfo VideoCapture::format() const {
  const FrameBuffer* frames = sink();
  if (!frames) return VideoFormatInfo{};
  return frames->format();
}

bool VideoCapture::PumpEvents(std::string* message) {
  if (!device_) return false;
  Said said;
  const bool fatal = device_->PumpEvents(&said);
  if (fatal) {
    CAP_WARN("Capture interrupted: %s", said.logged.c_str());
    if (message) *message = said.shown;
  }
  return fatal;
}

bool VideoCapture::SetCrossbarInput(int index) {
  if (!device_) return false;
  const bool ok = device_->RouteInput(index);
  capabilities_.currentInput = device_->CurrentInput();
  return ok;
}

}  // namespace cap
