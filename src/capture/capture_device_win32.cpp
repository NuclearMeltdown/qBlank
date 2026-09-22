// Owns the DirectShow graph: capture filter -> FrameSink. Nothing else is in
// the graph, and the graph runs without a reference clock so samples are
// delivered the moment the driver produces them instead of being scheduled.

#include "capture/capture_device.h"

#include <olectl.h>

#include <vector>

#include "capture/dshow_util.h"
#include "capture/frame_sink.h"
#include "common_win32.h"
#include "i18n.h"
#include "text_win32.h"

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

// The pages the filter offers. Asking the capture pin as well is tempting --
// it has a stream format page -- but handing a frame two objects means every
// page gets both, and this card's vendor page faults on being given a pin.
// qBlank picks the capture format itself anyway, so that page has nothing to
// add that is not already in the Source tab.
void CollectPages(IUnknown* object, std::vector<GUID>* pages) {
  if (!object) return;
  ComPtr<ISpecifyPropertyPages> spec;
  if (FAILED(object->QueryInterface(IID_PPV_ARGS(&spec)))) return;

  CAUUID ca = {};
  if (FAILED(spec->GetPages(&ca))) return;
  for (ULONG i = 0; i < ca.cElems; ++i) pages->push_back(ca.pElems[i]);
  if (ca.pElems) ::CoTaskMemFree(ca.pElems);
}

class DShowSignalProbe final : public SignalProbe {
 public:
  explicit DShowSignalProbe(ComPtr<IBaseFilter> filter) : filter_(std::move(filter)) {}
  long CurrentStandard() const override { return CurrentVideoStandard(filter_.Get()); }
  int Locked() const override { return VideoStandardLocked(filter_.Get()); }

 private:
  ComPtr<IBaseFilter> filter_;
};

class DShowCaptureDevice final : public CaptureDevice {
 public:
  bool Open(const DeviceRef& ref, Purpose purpose, VideoDeviceInfo* resolved,
            Said* failure) override {
    purpose_ = purpose;
    const bool probe = purpose == Purpose::Probe;

    std::string err;
    if (!CreateGraph(&graph_, &builder_, &err)) {
      *failure = Relayed(err);
      return false;
    }

    filter_ = CreateVideoFilter(ref, resolved);
    if (!filter_) {
      const std::string name = ref.name.empty() ? ref.id : ref.name;
      *failure = probe ? CAP_SAID(T("Gerät '", "Device '") + name +
                                  T("' wurde nicht gefunden. Ist die Karte angeschlossen?",
                                    "' was not found. Is the card plugged in?"))
                       : CAP_SAID(T("Videogerät '", "Video device '") + name +
                                  T("' nicht gefunden. Ist die Capture-Karte angeschlossen?",
                                    "' not found. Is the capture card plugged in?"));
      return false;
    }

    const HRESULT hr = graph_->AddFilter(filter_.Get(), L"Capture");
    if (FAILED(hr)) {
      *failure = probe ? CAP_SAID(T("Capture-Filter konnte nicht in den Graph eingefügt werden: ",
                                    "The capture filter could not be added to the graph: ") +
                                  HrToString(hr))
                       : CAP_SAID(T("Capture-Filter konnte nicht eingefügt werden: ",
                                    "The capture filter could not be added: ") +
                                  HrToString(hr));
      return false;
    }

    pin_ = FindCapturePin(builder_.Get(), filter_.Get());
    if (!pin_) {
      *failure = probe ? CAP_SAID(T("Das Gerät hat keinen brauchbaren Capture-Pin",
                                    "The device offers no usable capture pin"))
                       : CAP_SAID(T("Das Gerät hat keinen brauchbaren Capture-Pin",
                                    "The device has no usable capture pin"));
      return false;
    }
    return true;
  }

  void Close() override {
    if (graph_) {
      if (purpose_ == Purpose::Probe) {
        // Tear down explicitly and in order so the device is released before
        // the probe returns -- the caller may want to open it for real right
        // afterwards.
        if (sink_) {
          sink_->pin()->Disconnect();
          graph_->RemoveFilter(sink_.Get());
        }
        if (filter_) graph_->RemoveFilter(filter_.Get());
      } else {
        if (filter_) graph_->RemoveFilter(filter_.Get());
        if (sink_) graph_->RemoveFilter(sink_.Get());
      }
    }
    events_.Reset();
    control_.Reset();
    pin_.Reset();
    if (purpose_ == Purpose::Probe) sink_.Reset();  // the probe has always let go of it first
    filter_.Reset();
    sink_.Reset();
    builder_.Reset();
    graph_.Reset();
  }

  int NeutraliseImageControls() override { return NeutraliseProcAmp(filter_.Get()); }

  long AvailableStandards() const override { return AvailableVideoStandards(filter_.Get()); }
  long CurrentStandard() const override { return CurrentVideoStandard(filter_.Get()); }
  bool SetStandard(long standard) override { return SetVideoStandard(filter_.Get(), standard); }
  int SignalLocked() const override { return VideoStandardLocked(filter_.Get()); }

  std::vector<CapsEntry> Caps() const override { return EnumerateCaps(pin_.Get()); }

  size_t FrameBytes(const std::string& subtype, int width, int height) const override {
    GUID guid = GUID_NULL;
    if (!SubtypeFromLabel(subtype, &guid)) return 0;
    return ImageSizeForSubtype(guid, width, height, nullptr);
  }

  void ReadCurrentFormat(VideoFormatInfo* out) const override {
    if (!pin_) return;
    ComPtr<IAMStreamConfig> config;
    if (SUCCEEDED(pin_->QueryInterface(IID_PPV_ARGS(&config)))) {
      AM_MEDIA_TYPE* current = nullptr;
      if (SUCCEEDED(config->GetFormat(&current)) && current) {
        ParseVideoMediaType(current, out);
        DeleteMediaType(current);
      }
    }
  }

  void RevealInputs() override {
    if (!graph_ || !pin_) return;
    // The crossbar is a separate upstream filter that only joins the graph once
    // the capture pin is connected, so build the full chain before asking for it.
    sink_ = FrameSink::Create();
    if (sink_ && SUCCEEDED(graph_->AddFilter(sink_.Get(), L"qBlank Probe Sink"))) {
      // Direct connection only. Falling back to intelligent connect here would
      // make the graph builder try every registered filter against every media
      // type the card offers, which takes seconds and is far more than is needed
      // to make the crossbar show up.
      graph_->ConnectDirect(pin_.Get(), static_cast<IPin*>(sink_->pin()), nullptr);
    }
  }

  std::vector<CrossbarInput> Inputs() const override {
    return EnumerateCrossbarInputs(builder_.Get(), filter_.Get());
  }
  int CurrentInput() const override { return CurrentCrossbarInput(builder_.Get(), filter_.Get()); }
  bool RouteInput(int index) override {
    if (!builder_ || !filter_) return false;
    return RouteCrossbarInput(builder_.Get(), filter_.Get(), index);
  }

  bool AttachSink(Said* failure) override {
    sink_ = FrameSink::Create();
    if (!sink_) {
      *failure = CAP_SAID(T("Sink-Filter konnte nicht erstellt werden",
                            "The sink filter could not be created"));
      return false;
    }
    const HRESULT hr = graph_->AddFilter(sink_.Get(), L"qBlank Sink");
    if (FAILED(hr)) {
      *failure = CAP_SAID(T("Sink-Filter konnte nicht eingefügt werden: ",
                            "The sink filter could not be added: ") +
                          HrToString(hr));
      return false;
    }
    return true;
  }

  FormatAttempt TryFormat(const FormatSel& format, Said* refusal) override {
    IPin* sinkPin = static_cast<IPin*>(sink_->pin());

    VideoFormatInfo appliedFormat;
    HRESULT hr = ApplyFormat(pin_.Get(), format, &appliedFormat);
    if (FAILED(hr)) {
      if (IsDeviceBusyError(hr)) return FormatAttempt::Busy;
      const std::string step = "SetFormat " + format.Label();
      *refusal = CAP_SAID(step + ": " + HrToString(hr));
      return FormatAttempt::Refused;
    }

    // Direct connection keeps the graph at two filters. Only fall back to
    // intelligent connect when the capture format needs a decoder (MJPG).
    hr = graph_->ConnectDirect(pin_.Get(), sinkPin, nullptr);
    if (FAILED(hr) && !IsDeviceBusyError(hr)) hr = graph_->Connect(pin_.Get(), sinkPin);

    if (SUCCEEDED(hr)) return FormatAttempt::Accepted;
    if (IsDeviceBusyError(hr)) return FormatAttempt::Busy;

    const std::string step = "Connect " + format.Label();
    *refusal = CAP_SAID(step + ": " + HrToString(hr));
    CAP_WARN("%s: %s", step.c_str(), HrToEnglish(hr).c_str());
    // Leave no half-connected pins behind before the next attempt.
    pin_->Disconnect();
    sinkPin->Disconnect();
    return FormatAttempt::Refused;
  }

  bool Run(Said* failure) override {
    // No reference clock: with a clock the graph would hold each sample until its
    // presentation time, which is pure added latency for a live preview.
    ComPtr<IMediaFilter> mediaFilter;
    if (SUCCEEDED(graph_.As(&mediaFilter))) {
      mediaFilter->SetSyncSource(nullptr);
    }

    HRESULT hr;
    if (FAILED(hr = graph_.As(&control_))) {
      *failure = CAP_SAID(T("IMediaControl nicht verfügbar: ", "IMediaControl not available: ") +
                          HrToString(hr));
      return false;
    }
    graph_.As(&events_);

    hr = control_->Run();
    if (FAILED(hr)) {
      *failure = CAP_SAID(T("Der Graph konnte nicht gestartet werden (",
                            "The graph could not be started (") +
                          HrToString(hr) + T("). Benutzt ein anderes Programm die Karte gerade?",
                                             "). Is another program using the card right now?"));
      return false;
    }
    return true;
  }

  bool running() const override { return control_ != nullptr; }

  void Stop() override {
    if (control_) {
      control_->Stop();
    }
  }

  FrameBuffer* frames() override { return sink_ ? &sink_->buffer() : nullptr; }
  const FrameBuffer* frames() const override { return sink_ ? &sink_->buffer() : nullptr; }

  bool PumpEvents(Said* failure) override {
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
    if (fatal) *failure = said;
    return fatal;
  }

  std::shared_ptr<SignalProbe> OpenSignalProbe() const override {
    if (!filter_) return nullptr;
    return std::make_shared<DShowSignalProbe>(filter_);
  }

  bool HasOwnDialog() const override {
    std::vector<GUID> pages;
    CollectPages(filter_.Get(), &pages);
    return !pages.empty();
  }

  std::function<void()> OwnDialog(const std::string& title) const override {
    if (!filter_) return {};

    // The graph lives in the multithreaded apartment. The pages need an apartment
    // threaded one, because that is what a dialog with a message pump wants, so
    // they run on a thread of their own -- and the pointer goes over as it is,
    // not through the marshaller.
    //
    // That is measured rather than assumed:
    // CoMarshalInterThreadInterfaceInStream fails outright on IID_IBaseFilter,
    // because DirectShow registers no proxy/stub for its interfaces. Handed the
    // raw pointer instead, every page this card offers -- including the vendor's
    // own -- accepts the filter and the dialog comes up populated. The reference
    // below keeps the filter alive for as long as the dialog is up and is dropped
    // on the thread that asked for it, so nothing crosses an apartment boundary
    // that COM would have had to arrange.
    ComPtr<IBaseFilter> filter = filter_;
    std::wstring wide = ToWide(title);
    return [filter, wide]() {
      const HRESULT init =
          ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

      std::vector<GUID> pages;
      CollectPages(filter.Get(), &pages);

      if (!pages.empty()) {
        CAP_LOG("Card configuration dialog: %d pages", (int)pages.size());
        // No owner window. The obvious thing is to pass the main window, and it is
        // wrong: a modal dialog owned across a thread boundary ties the two input
        // queues together, and the frame comes up blank and takes no input but the
        // close button. Unowned, it works -- at the cost of not floating above the
        // main window, which the caller compensates for by dropping topmost while
        // this is open.
        IUnknown* object = filter.Get();
        const HRESULT hr = ::OleCreatePropertyFrame(nullptr, 0, 0, wide.c_str(), 1, &object,
                                                    (ULONG)pages.size(), pages.data(), 0, 0,
                                                    nullptr);
        if (FAILED(hr)) CAP_LOG("OleCreatePropertyFrame failed: 0x%08lX", (unsigned long)hr);
      } else {
        CAP_LOG("Card configuration dialog: no pages found");
      }

      if (SUCCEEDED(init)) ::CoUninitialize();
    };
  }

 private:
  Purpose purpose_ = Purpose::Stream;
  ComPtr<IGraphBuilder> graph_;
  ComPtr<ICaptureGraphBuilder2> builder_;
  ComPtr<IMediaControl> control_;
  ComPtr<IMediaEventEx> events_;
  ComPtr<IBaseFilter> filter_;
  ComPtr<IPin> pin_;
  ComPtr<FrameSink> sink_;
};

}  // namespace

std::unique_ptr<CaptureDevice> CaptureDevice::Create() {
  return std::make_unique<DShowCaptureDevice>();
}

std::vector<VideoDeviceInfo> EnumerateVideoDevices() {
  std::vector<VideoDeviceInfo> out;
  for (const DShowDeviceInfo& d : EnumerateVideoCaptureDShowDevices()) out.push_back(d);
  return out;
}

HardwarePath VideoDeviceHardware(const VideoDeviceInfo& device) {
  // The id is the DevicePath.
  return HardwareFromInstancePath(NormalizeDevicePath(device.id));
}

std::thread StartCaptureThread(std::function<void()> body) {
  return std::thread([body = std::move(body)]() {
    // The same apartment the graph lives in, so the pointer is usable as it is.
    ComScope com(COINIT_MULTITHREADED);
    body();
  });
}

}  // namespace cap
