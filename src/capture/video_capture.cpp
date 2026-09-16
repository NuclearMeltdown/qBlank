#include "capture/video_capture.h"

#include <algorithm>
#include <vector>

#include "i18n.h"

namespace cap {
namespace {

// Creates a filter graph plus capture graph builder, wired together.
bool CreateGraph(ComPtr<IGraphBuilder>* graph, ComPtr<ICaptureGraphBuilder2>* builder,
                 std::string* error) {
  HRESULT hr = ::CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(graph->GetAddressOf()));
  if (FAILED(hr)) {
    ReportError(error, CAP_SAID(T("Filtergraph konnte nicht erstellt werden: ",
                                     "The filter graph could not be created: ") + HrToString(hr)));
    return false;
  }
  hr = ::CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(builder->GetAddressOf()));
  if (FAILED(hr)) {
    ReportError(error, CAP_SAID(T("Capture Graph Builder konnte nicht erstellt werden: ",
                                     "The capture graph builder could not be created: ") + HrToString(hr)));
    return false;
  }
  hr = (*builder)->SetFiltergraph(graph->Get());
  if (FAILED(hr)) {
    ReportError(error, CAP_SAID(T("SetFiltergraph fehlgeschlagen: ", "SetFiltergraph failed: ") + HrToString(hr)));
    return false;
  }
  return true;
}

// True for errors that mean "somebody else has this device", as opposed to
// "this device does not like that format". KS drivers report a pin whose single
// instance is already taken as ERROR_NO_SYSTEM_RESOURCES, which reads like a
// memory problem but is not one -- it is the usual answer when a second program
// (another qBlank window, OBS, the vendor tool) is holding the card.
bool IsDeviceBusyError(HRESULT hr) {
  return hr == HRESULT_FROM_WIN32(ERROR_NO_SYSTEM_RESOURCES) ||
         hr == HRESULT_FROM_WIN32(ERROR_BUSY) ||
         hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) ||
         hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) || hr == E_ACCESSDENIED ||
         hr == VFW_E_NO_ALLOCATOR;
}

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

// Cost of one frame in bytes, used to order fallbacks from cheapest upwards.
size_t FrameBytes(const std::string& subtypeLabel, int width, int height) {
  GUID subtype = GUID_NULL;
  if (!SubtypeFromLabel(subtypeLabel, &subtype)) return SIZE_MAX;
  const size_t size = ImageSizeForSubtype(subtype, width, height, nullptr);
  return size ? size : SIZE_MAX;  // compressed formats sort last
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
      !ConnectorFollowsVideoStandard(caps.crossbarInputs[(size_t)index].physicalType)) {
    return 0;
  }
  return caps.currentStandard;
}

std::vector<FormatSel> BuildFormatCandidates(const FormatSel& wanted, const CapsModel& caps) {
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
            [](const FormatSel& a, const FormatSel& b) {
              return FrameBytes(a.subtype, a.width, a.height) <
                     FrameBytes(b.subtype, b.width, b.height);
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

  ComPtr<IGraphBuilder> graph;
  ComPtr<ICaptureGraphBuilder2> builder;
  if (!CreateGraph(&graph, &builder, &out.error)) return out;

  ComPtr<IBaseFilter> filter = CreateVideoFilter(device, &out.device);
  if (!filter) {
    out.error = T("Gerät '", "Device '") + (device.name.empty() ? device.id : device.name) +
                T("' wurde nicht gefunden. Ist die Karte angeschlossen?",
                  "' was not found. Is the card plugged in?");
    return out;
  }

  HRESULT hr = graph->AddFilter(filter.Get(), L"Capture");
  if (FAILED(hr)) {
    out.error = T("Capture-Filter konnte nicht in den Graph eingefügt werden: ",
                  "The capture filter could not be added to the graph: ") + HrToString(hr);
    return out;
  }

  ComPtr<IPin> pin = FindCapturePin(builder.Get(), filter.Get());
  if (!pin) {
    out.error = T("Das Gerät hat keinen brauchbaren Capture-Pin",
                  "The device offers no usable capture pin");
    return out;
  }

  out.availableStandards = AvailableVideoStandards(filter.Get());
  out.currentStandard = CurrentVideoStandard(filter.Get());

  out.caps.Build(EnumerateCaps(pin.Get()));

  // The crossbar is a separate upstream filter that only joins the graph once
  // the capture pin is connected, so build the full chain before asking for it.
  ComPtr<FrameSink> sink = FrameSink::Create();
  if (sink && SUCCEEDED(graph->AddFilter(sink.Get(), L"qBlank Probe Sink"))) {
    // Direct connection only. Falling back to intelligent connect here would
    // make the graph builder try every registered filter against every media
    // type the card offers, which takes seconds and is far more than is needed
    // to make the crossbar show up.
    graph->ConnectDirect(pin.Get(), static_cast<IPin*>(sink->pin()), nullptr);
  }
  out.crossbarInputs = EnumerateCrossbarInputs(builder.Get(), filter.Get());
  out.currentInput = CurrentCrossbarInput(builder.Get(), filter.Get());
  // Erst hier, denn die Antwort haengt am Eingang, und der steht erst, seit der
  // Pin verbunden ist. Ein Wunsch des Nutzers ist an dieser Stelle nicht bekannt
  // -- die Karte wird ja gerade erst befragt --, also entscheidet, worauf sie
  // steht.
  out.caps.SetNativeStandard(NativeStandardOf(out, -1, false));

  ComPtr<IAMStreamConfig> config;
  if (SUCCEEDED(pin->QueryInterface(IID_PPV_ARGS(&config)))) {
    AM_MEDIA_TYPE* current = nullptr;
    if (SUCCEEDED(config->GetFormat(&current)) && current) {
      ParseVideoMediaType(current, &out.colorInfo);
      DeleteMediaType(current);
    }
  }
  out.ok = true;

  // Tear down explicitly and in order so the device is released before we
  // return -- the caller may want to open it for real right afterwards.
  if (sink) {
    sink->pin()->Disconnect();
    graph->RemoveFilter(sink.Get());
  }
  graph->RemoveFilter(filter.Get());
  pin.Reset();
  sink.Reset();
  filter.Reset();
  builder.Reset();
  graph.Reset();
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

  std::string err;
  if (!CreateGraph(&graph_, &builder_, &err)) return fail(Relayed(err));

  captureFilter_ = CreateVideoFilter(settings.video, &capabilities_.device);
  if (!captureFilter_) {
    return fail(CAP_SAID(T("Videogerät '", "Video device '") +
                         (settings.video.name.empty() ? settings.video.id : settings.video.name) +
                         T("' nicht gefunden. Ist die Capture-Karte angeschlossen?",
                           "' not found. Is the capture card plugged in?")));
  }

  HRESULT hr = graph_->AddFilter(captureFilter_.Get(), L"Capture");
  if (FAILED(hr))
    return fail(CAP_SAID(T("Capture-Filter konnte nicht eingefügt werden: ",
                           "The capture filter could not be added: ") +
                         HrToString(hr)));

  capturePin_ = FindCapturePin(builder_.Get(), captureFilter_.Get());
  if (!capturePin_)
    return fail(CAP_SAID(T("Das Gerät hat keinen brauchbaren Capture-Pin",
                           "The device has no usable capture pin")));

  // Bevor irgendetwas anderes eingestellt wird, und bei jedem Start neu: was in
  // diesen Reglern steht, wird angewendet, bevor das Bild ueberhaupt bei uns
  // ankommt. Ein Dekoder, der den Schwarzwert anhebt, laesst im ganzen Programm
  // keine saubere Fassung mehr uebrig -- und der Schaden ist nicht mehr
  // ruecknehmbar, weil er schon geklemmt hat. Dieselben vier Regler gibt es im
  // Bild-Tab, dort im Shader, wo das Original darunter erhalten bleibt.
  //
  // Die Treiberdialoge (DevicePropertyPages) koennen das jederzeit wieder
  // verstellen. Das ist in Ordnung: es gilt dann bis zum naechsten Start.
  NeutraliseProcAmp(captureFilter_.Get());

  // Before the formats are read, not after: the standard decides how many lines
  // the card will produce, and therefore which formats it advertises at all.
  capabilities_.availableStandards = AvailableVideoStandards(captureFilter_.Get());
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
    SetVideoStandard(captureFilter_.Get(), settings.videoStandard);
  }
  capabilities_.currentStandard = CurrentVideoStandard(captureFilter_.Get());

  capabilities_.caps.Build(EnumerateCaps(capturePin_.Get()));

  // Vorlaeufig, fuer die Formatwahl gleich darunter: ein privater Selektor
  // antwortet ohne Graphen, ein Crossbar erst, wenn der Capture-Pin verbunden
  // ist -- und verbunden wird der erst, wenn das Format steht. Wo hier schon
  // etwas zu holen ist, wird es geholt; sonst bleibt stehen, was der vorige Lauf
  // hinterlassen hat, denn umgesteckt wird zwischen zwei Laeufen selten. Die
  // verbindliche Fassung kommt weiter unten, sobald der Graph steht.
  std::vector<CrossbarInput> earlyInputs =
      EnumerateCrossbarInputs(builder_.Get(), captureFilter_.Get());
  if (!earlyInputs.empty()) {
    capabilities_.crossbarInputs = std::move(earlyInputs);
    capabilities_.currentInput = CurrentCrossbarInput(builder_.Get(), captureFilter_.Get());
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

  sink_ = FrameSink::Create();
  if (!sink_)
    return fail(CAP_SAID(T("Sink-Filter konnte nicht erstellt werden",
                           "The sink filter could not be created")));

  hr = graph_->AddFilter(sink_.Get(), L"qBlank Sink");
  if (FAILED(hr))
    return fail(CAP_SAID(T("Sink-Filter konnte nicht eingefügt werden: ",
                           "The sink filter could not be added: ") +
                         HrToString(hr)));

  IPin* sinkPin = static_cast<IPin*>(sink_->pin());

  // A card can say no when the format is set and again when the pins connect.
  // Only a format-shaped refusal is worth retrying with a different format; if
  // the device is simply taken, trying eleven more formats just makes the user
  // wait for the same answer eleven more times.
  // What failed last, kept in parts so the message can be built in either language.
  std::string lastStep;
  HRESULT lastHr = S_OK;
  bool connected = false;
  for (const FormatSel& candidate : BuildFormatCandidates(wanted, capabilities_.caps)) {
    VideoFormatInfo appliedFormat;
    hr = ApplyFormat(capturePin_.Get(), candidate, &appliedFormat);
    if (FAILED(hr)) {
      if (IsDeviceBusyError(hr)) return fail(CAP_SAID(BusyMessage(settings.video)));
      lastStep = "SetFormat " + candidate.Label();
      lastHr = hr;
      continue;
    }

    // Direct connection keeps the graph at two filters. Only fall back to
    // intelligent connect when the capture format needs a decoder (MJPG).
    hr = graph_->ConnectDirect(capturePin_.Get(), sinkPin, nullptr);
    if (FAILED(hr) && !IsDeviceBusyError(hr)) hr = graph_->Connect(capturePin_.Get(), sinkPin);

    if (SUCCEEDED(hr)) {
      if (!candidate.SameFormat(wanted)) {
        CAP_WARN("Format %s failed, using %s instead", wanted.Label().c_str(),
                 candidate.Label().c_str());
      }
      connectedFormat_ = candidate;
      connected = true;
      break;
    }

    if (IsDeviceBusyError(hr)) return fail(CAP_SAID(BusyMessage(settings.video)));

    lastStep = "Connect " + candidate.Label();
    lastHr = hr;
    CAP_WARN("%s: %s", lastStep.c_str(), HrToEnglish(hr).c_str());
    // Leave no half-connected pins behind before the next attempt.
    capturePin_->Disconnect();
    sinkPin->Disconnect();
  }

  if (!connected) {
    return fail(CAP_SAID(T("Die Karte hat kein einziges Format akzeptiert. Letzter Fehler: ",
                           "The card accepted none of the formats. Last error: ") +
                         (lastStep.empty() ? std::string() : lastStep + ": " + HrToString(lastHr))));
  }

  // Only now that the capture pin is connected is the crossbar filter part of
  // the graph, so this is the earliest point at which the input can be routed.
  capabilities_.crossbarInputs = EnumerateCrossbarInputs(builder_.Get(), captureFilter_.Get());
  if (settings.crossbarInput >= 0 && !capabilities_.crossbarInputs.empty()) {
    RouteCrossbarInput(builder_.Get(), captureFilter_.Get(), settings.crossbarInput);
  }
  // Read back afterwards, so this says where the card ended up rather than
  // where it was asked to go -- and says it just as well when it was not asked
  // at all, which is the case that "Automatisch" needs answered.
  capabilities_.currentInput = CurrentCrossbarInput(builder_.Get(), captureFilter_.Get());

  // No reference clock: with a clock the graph would hold each sample until its
  // presentation time, which is pure added latency for a live preview.
  ComPtr<IMediaFilter> mediaFilter;
  if (SUCCEEDED(graph_.As(&mediaFilter))) {
    mediaFilter->SetSyncSource(nullptr);
  }

  if (FAILED(hr = graph_.As(&control_))) {
    return fail(CAP_SAID(T("IMediaControl nicht verfügbar: ", "IMediaControl not available: ") +
                         HrToString(hr)));
  }
  graph_.As(&events_);

  hr = control_->Run();
  if (FAILED(hr)) {
    return fail(CAP_SAID(T("Der Graph konnte nicht gestartet werden (",
                           "The graph could not be started (") +
                         HrToString(hr) + T("). Benutzt ein anderes Programm die Karte gerade?",
                                            "). Is another program using the card right now?")));
  }

  VideoFormatInfo info = sink_->format();
  CAP_LOG("Capture running: %s %dx%d @ %.3f fps%s", info.subtypeLabel.c_str(), info.width,
          info.height, info.fps, info.interlaced ? " (interlaced)" : "");
  return true;
}

void VideoCapture::Stop() {
  if (control_) {
    control_->Stop();
  }
  Teardown();
}

void VideoCapture::Teardown() {
  if (graph_) {
    if (captureFilter_) graph_->RemoveFilter(captureFilter_.Get());
    if (sink_) graph_->RemoveFilter(sink_.Get());
  }
  events_.Reset();
  control_.Reset();
  capturePin_.Reset();
  captureFilter_.Reset();
  sink_.Reset();
  builder_.Reset();
  graph_.Reset();
  connectedFormat_ = FormatSel{};
}

VideoFormatInfo VideoCapture::format() const {
  if (!sink_) return VideoFormatInfo{};
  return sink_->format();
}

bool VideoCapture::PumpEvents(std::string* message) {
  if (!events_) return false;
  bool fatal = false;
  Said said;
  long code = 0;
  LONG_PTR p1 = 0, p2 = 0;
  while (SUCCEEDED(events_->GetEvent(&code, &p1, &p2, 0))) {
    switch (code) {
      case EC_DEVICE_LOST:
        // p2 == 0 means the device went away; == 1 means it came back.
        if (p2 == 0) {
          fatal = true;
          said = CAP_SAID(T("Das Aufnahmegerät wurde entfernt.", "The capture device was removed."));
        }
        break;
      case EC_ERRORABORT:
      case EC_ERRORABORTEX:
        fatal = true;
        said = CAP_SAID(T("Der Capture-Graph wurde mit einem Fehler abgebrochen (",
                          "The capture graph was aborted with an error (") +
                        HrToString((HRESULT)p1) + ").");
        break;
      case EC_COMPLETE:
      case EC_USERABORT:
        fatal = true;
        said = CAP_SAID(T("Der Stream wurde beendet.", "The stream ended."));
        break;
      default: break;
    }
    events_->FreeEventParams(code, p1, p2);
  }
  if (!fatal && sink_ && sink_->ended()) {
    fatal = true;
    said = CAP_SAID(T("Die Karte liefert keine Daten mehr.", "The card stopped sending data."));
  }
  if (fatal) {
    CAP_WARN("Capture interrupted: %s", said.logged.c_str());
    if (message) *message = said.shown;
  }
  return fatal;
}

bool VideoCapture::SetCrossbarInput(int index) {
  if (!builder_ || !captureFilter_) return false;
  const bool ok = RouteCrossbarInput(builder_.Get(), captureFilter_.Get(), index);
  capabilities_.currentInput = CurrentCrossbarInput(builder_.Get(), captureFilter_.Get());
  return ok;
}

}  // namespace cap
