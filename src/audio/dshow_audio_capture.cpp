#include "audio/dshow_audio_capture.h"

#include <new>
#include <vector>

#include "capture/dshow_util.h"
#include "i18n.h"

namespace cap {
namespace {

// {C1F0B8A4-2E77-4B3D-9C51-7A6E0D42F118}
const GUID CLSID_qBlankAudioSink = {
    0xc1f0b8a4, 0x2e77, 0x4b3d, {0x9c, 0x51, 0x7a, 0x6e, 0x0d, 0x42, 0xf1, 0x18}};

const wchar_t kPinId[] = L"In";

LPWSTR AllocTaskString(const wchar_t* s) {
  size_t bytes = (wcslen(s) + 1) * sizeof(wchar_t);
  auto* out = (LPWSTR)::CoTaskMemAlloc(bytes);
  if (out) memcpy(out, s, bytes);
  return out;
}

class AudioSinkFilter;

// Single input pin; its IUnknown delegates to the owning filter.
class AudioSinkPin final : public IPin, public IMemInputPin {
 public:
  explicit AudioSinkPin(AudioSinkFilter* owner) : owner_(owner) {}
  ~AudioSinkPin() { FreeMediaType(mediaType_); }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;

  HRESULT STDMETHODCALLTYPE Connect(IPin*, const AM_MEDIA_TYPE*) override {
    return E_UNEXPECTED;
  }
  HRESULT STDMETHODCALLTYPE ReceiveConnection(IPin* connector, const AM_MEDIA_TYPE* mt) override;
  HRESULT STDMETHODCALLTYPE Disconnect() override;
  HRESULT STDMETHODCALLTYPE ConnectedTo(IPin** pin) override;
  HRESULT STDMETHODCALLTYPE ConnectionMediaType(AM_MEDIA_TYPE* mt) override;
  HRESULT STDMETHODCALLTYPE QueryPinInfo(PIN_INFO* info) override;
  HRESULT STDMETHODCALLTYPE QueryDirection(PIN_DIRECTION* dir) override {
    if (!dir) return E_POINTER;
    *dir = PINDIR_INPUT;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE QueryId(LPWSTR* id) override {
    if (!id) return E_POINTER;
    *id = AllocTaskString(kPinId);
    return *id ? S_OK : E_OUTOFMEMORY;
  }
  HRESULT STDMETHODCALLTYPE QueryAccept(const AM_MEDIA_TYPE* mt) override {
    return CheckMediaType(mt) == S_OK ? S_OK : S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE EnumMediaTypes(IEnumMediaTypes** enumerator) override;
  HRESULT STDMETHODCALLTYPE QueryInternalConnections(IPin**, ULONG* count) override {
    if (count) *count = 0;
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE EndOfStream() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE BeginFlush() override {
    flushing_ = true;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE EndFlush() override {
    flushing_ = false;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override {
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetAllocator(IMemAllocator** allocator) override {
    if (!allocator) return E_POINTER;
    *allocator = nullptr;
    return VFW_E_NO_ALLOCATOR;
  }
  HRESULT STDMETHODCALLTYPE NotifyAllocator(IMemAllocator*, BOOL) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE GetAllocatorRequirements(ALLOCATOR_PROPERTIES*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE Receive(IMediaSample* sample) override;
  HRESULT STDMETHODCALLTYPE ReceiveMultiple(IMediaSample** samples, long count,
                                            long* processed) override {
    if (!samples || !processed) return E_POINTER;
    *processed = 0;
    for (long i = 0; i < count; ++i) {
      HRESULT hr = Receive(samples[i]);
      if (FAILED(hr)) return hr;
      ++(*processed);
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE ReceiveCanBlock() override { return S_FALSE; }

 private:
  HRESULT CheckMediaType(const AM_MEDIA_TYPE* mt) const;

  AudioSinkFilter* owner_;
  ComPtr<IPin> connected_;
  AM_MEDIA_TYPE mediaType_ = {};
  bool flushing_ = false;
};

class AudioSinkFilter final : public IBaseFilter, public IAMFilterMiscFlags {
 public:
  explicit AudioSinkFilter(AudioSinkFn sink) : sink_(std::move(sink)) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IPersist || riid == IID_IMediaFilter ||
        riid == IID_IBaseFilter) {
      *ppv = static_cast<IBaseFilter*>(this);
    } else if (riid == IID_IAMFilterMiscFlags) {
      *ppv = static_cast<IAMFilterMiscFlags*>(this);
    } else {
      *ppv = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return (ULONG)ref_.fetch_add(1, std::memory_order_relaxed) + 1;
  }
  ULONG STDMETHODCALLTYPE Release() override {
    LONG n = ref_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (n == 0) delete this;
    return (ULONG)n;
  }

  HRESULT STDMETHODCALLTYPE GetClassID(CLSID* clsid) override {
    if (!clsid) return E_POINTER;
    *clsid = CLSID_qBlankAudioSink;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Stop() override {
    state_.store(State_Stopped, std::memory_order_relaxed);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Pause() override {
    state_.store(State_Paused, std::memory_order_relaxed);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Run(REFERENCE_TIME) override {
    state_.store(State_Running, std::memory_order_relaxed);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetState(DWORD, FILTER_STATE* state) override {
    if (!state) return E_POINTER;
    *state = state_.load(std::memory_order_relaxed);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetSyncSource(IReferenceClock* clock) override {
    clock_ = clock;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetSyncSource(IReferenceClock** clock) override {
    if (!clock) return E_POINTER;
    *clock = clock_.Get();
    if (*clock) (*clock)->AddRef();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE EnumPins(IEnumPins** enumerator) override;
  HRESULT STDMETHODCALLTYPE FindPin(LPCWSTR id, IPin** pin) override {
    if (!id || !pin) return E_POINTER;
    if (wcscmp(id, kPinId) != 0) {
      *pin = nullptr;
      return VFW_E_NOT_FOUND;
    }
    *pin = static_cast<IPin*>(&pin_);
    (*pin)->AddRef();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE QueryFilterInfo(FILTER_INFO* info) override {
    if (!info) return E_POINTER;
    wcscpy_s(info->achName, L"qBlank Audio Sink");
    info->pGraph = graph_;
    if (info->pGraph) info->pGraph->AddRef();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE JoinFilterGraph(IFilterGraph* graph, LPCWSTR) override {
    graph_ = graph;  // weak by DirectShow convention
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE QueryVendorInfo(LPWSTR* vendorInfo) override {
    if (!vendorInfo) return E_POINTER;
    *vendorInfo = nullptr;
    return E_NOTIMPL;
  }
  ULONG STDMETHODCALLTYPE GetMiscFlags() override { return AM_FILTER_MISC_FLAGS_IS_RENDERER; }

  FILTER_STATE state() const { return state_.load(std::memory_order_relaxed); }
  AudioSinkPin* pin() { return &pin_; }
  const AudioSinkFn& sink() const { return sink_; }

  void SetFormat(const StreamFormat& fmt) { format_ = fmt; }
  StreamFormat format() const { return format_; }

  std::vector<float>& scratch() { return scratch_; }

 private:
  std::atomic<LONG> ref_{1};
  AudioSinkPin pin_{this};
  std::atomic<FILTER_STATE> state_{State_Stopped};
  IFilterGraph* graph_ = nullptr;
  ComPtr<IReferenceClock> clock_;
  AudioSinkFn sink_;
  StreamFormat format_;
  std::vector<float> scratch_;
};

// ---------------------------------------------------------------- enumerators

class AudioEnumPins final : public IEnumPins {
 public:
  explicit AudioEnumPins(IPin* pin, ULONG index = 0) : pin_(pin), index_(index) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IEnumPins) {
      *ppv = static_cast<IEnumPins*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)::InterlockedIncrement(&ref_); }
  ULONG STDMETHODCALLTYPE Release() override {
    LONG n = ::InterlockedDecrement(&ref_);
    if (n == 0) delete this;
    return (ULONG)n;
  }
  HRESULT STDMETHODCALLTYPE Next(ULONG count, IPin** pins, ULONG* fetched) override {
    if (!pins) return E_POINTER;
    ULONG n = 0;
    while (n < count && index_ < 1) {
      pins[n] = pin_.Get();
      pins[n]->AddRef();
      ++n;
      ++index_;
    }
    if (fetched) *fetched = n;
    return n == count ? S_OK : S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE Skip(ULONG count) override {
    index_ += count;
    return index_ > 1 ? S_FALSE : S_OK;
  }
  HRESULT STDMETHODCALLTYPE Reset() override {
    index_ = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Clone(IEnumPins** out) override {
    if (!out) return E_POINTER;
    *out = new (std::nothrow) AudioEnumPins(pin_.Get(), index_);
    return *out ? S_OK : E_OUTOFMEMORY;
  }

 private:
  LONG ref_ = 1;
  ComPtr<IPin> pin_;
  ULONG index_ = 0;
};

class AudioEmptyEnumMediaTypes final : public IEnumMediaTypes {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IEnumMediaTypes) {
      *ppv = static_cast<IEnumMediaTypes*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)::InterlockedIncrement(&ref_); }
  ULONG STDMETHODCALLTYPE Release() override {
    LONG n = ::InterlockedDecrement(&ref_);
    if (n == 0) delete this;
    return (ULONG)n;
  }
  HRESULT STDMETHODCALLTYPE Next(ULONG, AM_MEDIA_TYPE**, ULONG* fetched) override {
    if (fetched) *fetched = 0;
    return S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE Skip(ULONG) override { return S_FALSE; }
  HRESULT STDMETHODCALLTYPE Reset() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE Clone(IEnumMediaTypes** out) override {
    if (!out) return E_POINTER;
    *out = new (std::nothrow) AudioEmptyEnumMediaTypes();
    return *out ? S_OK : E_OUTOFMEMORY;
  }

 private:
  LONG ref_ = 1;
};

// -------------------------------------------------------------- pin methods

HRESULT AudioSinkPin::QueryInterface(REFIID riid, void** ppv) {
  if (!ppv) return E_POINTER;
  if (riid == IID_IUnknown || riid == IID_IPin) {
    *ppv = static_cast<IPin*>(this);
  } else if (riid == IID_IMemInputPin) {
    *ppv = static_cast<IMemInputPin*>(this);
  } else {
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

ULONG AudioSinkPin::AddRef() {
  return owner_->AddRef();
}

ULONG AudioSinkPin::Release() {
  return owner_->Release();
}

HRESULT AudioSinkPin::CheckMediaType(const AM_MEDIA_TYPE* mt) const {
  if (!mt) return E_POINTER;
  if (!IsEqualGUID(mt->majortype, MEDIATYPE_Audio)) return VFW_E_TYPE_NOT_ACCEPTED;
  if (!IsEqualGUID(mt->formattype, FORMAT_WaveFormatEx)) return VFW_E_TYPE_NOT_ACCEPTED;
  if (!mt->pbFormat || mt->cbFormat < sizeof(WAVEFORMATEX)) return VFW_E_TYPE_NOT_ACCEPTED;

  StreamFormat fmt;
  if (!ParseWaveFormat((const WAVEFORMATEX*)mt->pbFormat, &fmt)) return VFW_E_TYPE_NOT_ACCEPTED;
  return S_OK;
}

HRESULT AudioSinkPin::ReceiveConnection(IPin* connector, const AM_MEDIA_TYPE* mt) {
  if (!connector || !mt) return E_POINTER;
  if (connected_) return VFW_E_ALREADY_CONNECTED;
  if (owner_->state() != State_Stopped) return VFW_E_NOT_STOPPED;

  HRESULT hr = CheckMediaType(mt);
  if (FAILED(hr)) return hr;

  FreeMediaType(mediaType_);
  AM_MEDIA_TYPE* copy = CreateMediaTypeCopy(mt);
  if (!copy) return E_OUTOFMEMORY;
  mediaType_ = *copy;
  ::CoTaskMemFree(copy);

  StreamFormat fmt;
  ParseWaveFormat((const WAVEFORMATEX*)mediaType_.pbFormat, &fmt);
  owner_->SetFormat(fmt);
  connected_ = connector;

  CAP_LOG("DirectShow audio connected: %d Hz, %d channels, %d bit%s", fmt.sampleRate, fmt.channels,
          fmt.bitsPerSample, fmt.isFloat ? " float" : "");
  return S_OK;
}

HRESULT AudioSinkPin::Disconnect() {
  if (owner_->state() != State_Stopped) return VFW_E_NOT_STOPPED;
  if (!connected_) return S_FALSE;
  connected_.Reset();
  FreeMediaType(mediaType_);
  return S_OK;
}

HRESULT AudioSinkPin::ConnectedTo(IPin** pin) {
  if (!pin) return E_POINTER;
  if (!connected_) {
    *pin = nullptr;
    return VFW_E_NOT_CONNECTED;
  }
  *pin = connected_.Get();
  (*pin)->AddRef();
  return S_OK;
}

HRESULT AudioSinkPin::ConnectionMediaType(AM_MEDIA_TYPE* mt) {
  if (!mt) return E_POINTER;
  if (!connected_) {
    ZeroMemory(mt, sizeof(*mt));
    return VFW_E_NOT_CONNECTED;
  }
  AM_MEDIA_TYPE* copy = CreateMediaTypeCopy(&mediaType_);
  if (!copy) return E_OUTOFMEMORY;
  *mt = *copy;
  ::CoTaskMemFree(copy);
  return S_OK;
}

HRESULT AudioSinkPin::QueryPinInfo(PIN_INFO* info) {
  if (!info) return E_POINTER;
  info->pFilter = static_cast<IBaseFilter*>(owner_);
  info->pFilter->AddRef();
  info->dir = PINDIR_INPUT;
  wcscpy_s(info->achName, kPinId);
  return S_OK;
}

HRESULT AudioSinkPin::EnumMediaTypes(IEnumMediaTypes** enumerator) {
  if (!enumerator) return E_POINTER;
  *enumerator = new (std::nothrow) AudioEmptyEnumMediaTypes();
  return *enumerator ? S_OK : E_OUTOFMEMORY;
}

HRESULT AudioSinkPin::Receive(IMediaSample* sample) {
  if (!sample) return E_POINTER;
  if (flushing_ || owner_->state() == State_Stopped) return S_FALSE;

  const StreamFormat fmt = owner_->format();
  const AudioSinkFn& sink = owner_->sink();
  if (!sink || !fmt.valid()) return S_OK;

  BYTE* data = nullptr;
  if (FAILED(sample->GetPointer(&data)) || !data) return S_OK;
  const long length = sample->GetActualDataLength();
  if (length <= 0) return S_OK;

  const size_t frames = (size_t)length / (size_t)fmt.blockAlign;
  if (frames == 0) return S_OK;

  std::vector<float>& scratch = owner_->scratch();
  scratch.resize(frames * 2);
  ToStereoFloat(data, frames, fmt, scratch.data());
  sink(scratch.data(), frames);
  return S_OK;
}

HRESULT AudioSinkFilter::EnumPins(IEnumPins** enumerator) {
  if (!enumerator) return E_POINTER;
  *enumerator = new (std::nothrow) AudioEnumPins(static_cast<IPin*>(&pin_));
  return *enumerator ? S_OK : E_OUTOFMEMORY;
}

// ----------------------------------------------------------- format selection

// Picks the most useful format the input offers: highest sample rate, then
// stereo over mono, then 16 bit over 8. Left alone, many WDM audio inputs
// default to something like 8 kHz mono.
void ApplyBestAudioFormat(IPin* pin) {
  ComPtr<IAMStreamConfig> cfg;
  if (!pin || FAILED(pin->QueryInterface(IID_PPV_ARGS(&cfg)))) return;

  int count = 0, size = 0;
  if (FAILED(cfg->GetNumberOfCapabilities(&count, &size)) || count <= 0) return;
  if (size != sizeof(AUDIO_STREAM_CONFIG_CAPS)) return;

  AM_MEDIA_TYPE* best = nullptr;
  long bestScore = -1;
  for (int i = 0; i < count; ++i) {
    AM_MEDIA_TYPE* mt = nullptr;
    AUDIO_STREAM_CONFIG_CAPS caps = {};
    if (FAILED(cfg->GetStreamCaps(i, &mt, (BYTE*)&caps)) || !mt) continue;

    StreamFormat fmt;
    long score = -1;
    if (mt->pbFormat && IsEqualGUID(mt->formattype, FORMAT_WaveFormatEx) &&
        ParseWaveFormat((const WAVEFORMATEX*)mt->pbFormat, &fmt)) {
      score = fmt.sampleRate + (fmt.channels >= 2 ? 100000 : 0) +
              (fmt.bitsPerSample >= 16 ? 10000 : 0);
    }
    if (score > bestScore) {
      DeleteMediaType(best);
      best = mt;
      bestScore = score;
    } else {
      DeleteMediaType(mt);
    }
  }

  if (best) {
    if (SUCCEEDED(cfg->SetFormat(best))) {
      StreamFormat fmt;
      ParseWaveFormat((const WAVEFORMATEX*)best->pbFormat, &fmt);
      CAP_LOG("DirectShow audio format set: %d Hz, %d channels, %d bit", fmt.sampleRate,
              fmt.channels, fmt.bitsPerSample);
    }
    DeleteMediaType(best);
  }
}

}  // namespace

// -------------------------------------------------------- DShowAudioCapture

DShowAudioCapture::~DShowAudioCapture() {
  Stop();
}

bool DShowAudioCapture::Start(const DeviceRef& device, AudioSinkFn sink, std::string* error) {
  Stop();

  auto fail = [&](const Said& said) {
    if (error) *error = said.shown;
    CAP_ERR("DirectShow audio: %s", said.logged.c_str());
    Teardown();
    return false;
  };

  if (!sink) return fail(CAP_SAID(T("Kein Ziel für die Audiodaten", "No destination for the audio data")));

  HRESULT hr = ::CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&graph_));
  if (FAILED(hr))
    return fail(CAP_SAID(T("Audiograph konnte nicht erstellt werden: ",
                           "The audio graph could not be created: ") +
                         HrToString(hr)));

  // The device reference points at a DirectShow audio input; look it up by the
  // same rules as a video device.
  ComPtr<IBaseFilter> source;
  for (const DShowDeviceInfo& d : EnumerateAudioCaptureDShowDevices()) {
    if (d.id == device.id || (!device.name.empty() && d.name == device.name)) {
      source = CreateFilterFromMoniker(d);
      if (source) break;
    }
  }
  if (!source) {
    return fail(CAP_SAID(T("Audioeingang '", "Audio input '") +
                         (device.name.empty() ? device.id : device.name) +
                         T("' wurde nicht gefunden", "' was not found")));
  }
  sourceFilter_ = source;

  hr = graph_->AddFilter(sourceFilter_.Get(), L"Audio Capture");
  if (FAILED(hr))
    return fail(CAP_SAID(T("Audiofilter konnte nicht eingefügt werden: ",
                           "The audio filter could not be added: ") +
                         HrToString(hr)));

  ComPtr<IPin> outPin = FindPinByDirection(sourceFilter_.Get(), PINDIR_OUTPUT);
  if (!outPin)
    return fail(CAP_SAID(T("Der Audioeingang hat keinen Ausgangs-Pin",
                           "The audio input has no output pin")));
  ApplyBestAudioFormat(outPin.Get());

  auto* sinkFilter = new (std::nothrow) AudioSinkFilter(std::move(sink));
  if (!sinkFilter)
    return fail(CAP_SAID(T("Audio-Sink konnte nicht erstellt werden",
                           "The audio sink could not be created")));
  sinkFilter_.Attach(static_cast<IBaseFilter*>(sinkFilter));  // constructed with refcount 1

  hr = graph_->AddFilter(sinkFilter_.Get(), L"qBlank Audio Sink");
  if (FAILED(hr))
    return fail(CAP_SAID(T("Audio-Sink konnte nicht eingefügt werden: ",
                           "The audio sink could not be added: ") +
                         HrToString(hr)));

  IPin* sinkPin = static_cast<IPin*>(sinkFilter->pin());
  hr = graph_->ConnectDirect(outPin.Get(), sinkPin, nullptr);
  if (FAILED(hr)) hr = graph_->Connect(outPin.Get(), sinkPin);
  if (FAILED(hr)) {
    return fail(CAP_SAID(T("Der Audioeingang liefert kein verwertbares Format (",
                           "The audio input offers no usable format (") +
                         HrToString(hr) + ")"));
  }

  // Same reasoning as the video graph: without a clock nothing gets scheduled,
  // samples arrive as soon as the driver has them.
  ComPtr<IMediaFilter> mediaFilter;
  if (SUCCEEDED(graph_.As(&mediaFilter))) mediaFilter->SetSyncSource(nullptr);

  if (FAILED(hr = graph_.As(&control_))) {
    return fail(CAP_SAID(T("IMediaControl nicht verfügbar: ", "IMediaControl not available: ") +
                         HrToString(hr)));
  }
  hr = control_->Run();
  if (FAILED(hr))
    return fail(CAP_SAID(T("Der Audiograph konnte nicht gestartet werden: ",
                           "The audio graph could not be started: ") +
                         HrToString(hr)));

  format_ = sinkFilter->format();
  return true;
}

void DShowAudioCapture::Stop() {
  if (control_) control_->Stop();
  Teardown();
}

void DShowAudioCapture::Teardown() {
  if (graph_) {
    if (sourceFilter_) graph_->RemoveFilter(sourceFilter_.Get());
    if (sinkFilter_) graph_->RemoveFilter(sinkFilter_.Get());
  }
  control_.Reset();
  sinkFilter_.Reset();
  sourceFilter_.Reset();
  graph_.Reset();
  format_ = StreamFormat{};
}

}  // namespace cap
