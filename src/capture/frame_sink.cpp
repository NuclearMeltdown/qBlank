#include "capture/frame_sink.h"

#include <cstring>
#include <new>

namespace cap {
namespace {

// {8E1C4A22-6B3F-4C1E-9E77-2F5B0A4D9C10}
const GUID CLSID_qBlankFrameSink = {
    0x8e1c4a22, 0x6b3f, 0x4c1e, {0x9e, 0x77, 0x2f, 0x5b, 0x0a, 0x4d, 0x9c, 0x10}};

const wchar_t kPinId[] = L"In";

int64_t QpcNow() {
  LARGE_INTEGER v;
  ::QueryPerformanceCounter(&v);
  return v.QuadPart;
}

double QpcFrequency() {
  static const double freq = [] {
    LARGE_INTEGER f;
    ::QueryPerformanceFrequency(&f);
    return (double)f.QuadPart;
  }();
  return freq;
}

double QpcSeconds(int64_t ticks) {
  return (double)ticks / QpcFrequency();
}

LPWSTR AllocTaskString(const wchar_t* s) {
  size_t bytes = (wcslen(s) + 1) * sizeof(wchar_t);
  auto* out = (LPWSTR)::CoTaskMemAlloc(bytes);
  if (out) memcpy(out, s, bytes);
  return out;
}

// ------------------------------------------------------------------ enumerators

class EnumPinsImpl final : public IEnumPins {
 public:
  explicit EnumPinsImpl(IPin* pin, ULONG index = 0) : pin_(pin), index_(index) {}

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
    *out = new (std::nothrow) EnumPinsImpl(pin_.Get(), index_);
    return *out ? S_OK : E_OUTOFMEMORY;
  }

 private:
  LONG ref_ = 1;
  ComPtr<IPin> pin_;
  ULONG index_ = 0;
};

// We advertise no media types of our own. Intelligent connect drives the
// negotiation from the upstream pin's types and our QueryAccept, which is
// enough to get a decoder inserted for compressed formats.
class EmptyEnumMediaTypes final : public IEnumMediaTypes {
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
    *out = new (std::nothrow) EmptyEnumMediaTypes();
    return *out ? S_OK : E_OUTOFMEMORY;
  }

 private:
  LONG ref_ = 1;
};

}  // namespace

// ================================================================== SinkPin

SinkPin::~SinkPin() {
  FreeMediaType(mediaType_);
}

HRESULT SinkPin::QueryInterface(REFIID riid, void** ppv) {
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

ULONG SinkPin::AddRef() {
  return owner_->AddRef();
}

ULONG SinkPin::Release() {
  return owner_->Release();
}

HRESULT SinkPin::CheckMediaType(const AM_MEDIA_TYPE* mt) const {
  if (!mt) return E_POINTER;
  if (!IsEqualGUID(mt->majortype, MEDIATYPE_Video)) return VFW_E_TYPE_NOT_ACCEPTED;
  if (!IsRendererSubtype(mt->subtype)) return VFW_E_TYPE_NOT_ACCEPTED;

  VideoFormatInfo info;
  if (!ParseVideoMediaType(mt, &info)) return VFW_E_TYPE_NOT_ACCEPTED;
  if (!info.valid()) return VFW_E_TYPE_NOT_ACCEPTED;
  return S_OK;
}

HRESULT SinkPin::Connect(IPin*, const AM_MEDIA_TYPE*) {
  return E_UNEXPECTED;  // input pins are connected to, not from
}

HRESULT SinkPin::ReceiveConnection(IPin* connector, const AM_MEDIA_TYPE* mt) {
  if (!connector || !mt) return E_POINTER;
  if (connected_) return VFW_E_ALREADY_CONNECTED;
  if (owner_->state() != State_Stopped) return VFW_E_NOT_STOPPED;

  HRESULT hr = CheckMediaType(mt);
  if (FAILED(hr)) return hr;

  FreeMediaType(mediaType_);
  AM_MEDIA_TYPE* copy = CreateMediaTypeCopy(mt);
  if (!copy) return E_OUTOFMEMORY;
  mediaType_ = *copy;
  ::CoTaskMemFree(copy);  // the format block ownership moved into mediaType_

  connected_ = connector;
  owner_->OnConnected(&mediaType_);
  return S_OK;
}

HRESULT SinkPin::Disconnect() {
  if (owner_->state() != State_Stopped) return VFW_E_NOT_STOPPED;
  if (!connected_) return S_FALSE;
  connected_.Reset();
  allocator_.Reset();
  FreeMediaType(mediaType_);
  owner_->OnDisconnected();
  return S_OK;
}

HRESULT SinkPin::ConnectedTo(IPin** pin) {
  if (!pin) return E_POINTER;
  if (!connected_) {
    *pin = nullptr;
    return VFW_E_NOT_CONNECTED;
  }
  *pin = connected_.Get();
  (*pin)->AddRef();
  return S_OK;
}

HRESULT SinkPin::ConnectionMediaType(AM_MEDIA_TYPE* mt) {
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

HRESULT SinkPin::QueryPinInfo(PIN_INFO* info) {
  if (!info) return E_POINTER;
  info->pFilter = static_cast<IBaseFilter*>(owner_);
  info->pFilter->AddRef();
  info->dir = PINDIR_INPUT;
  wcscpy_s(info->achName, kPinId);
  return S_OK;
}

HRESULT SinkPin::QueryDirection(PIN_DIRECTION* dir) {
  if (!dir) return E_POINTER;
  *dir = PINDIR_INPUT;
  return S_OK;
}

HRESULT SinkPin::QueryId(LPWSTR* id) {
  if (!id) return E_POINTER;
  *id = AllocTaskString(kPinId);
  return *id ? S_OK : E_OUTOFMEMORY;
}

HRESULT SinkPin::QueryAccept(const AM_MEDIA_TYPE* mt) {
  return CheckMediaType(mt) == S_OK ? S_OK : S_FALSE;
}

HRESULT SinkPin::EnumMediaTypes(IEnumMediaTypes** enumerator) {
  if (!enumerator) return E_POINTER;
  *enumerator = new (std::nothrow) EmptyEnumMediaTypes();
  return *enumerator ? S_OK : E_OUTOFMEMORY;
}

HRESULT SinkPin::QueryInternalConnections(IPin**, ULONG* count) {
  if (count) *count = 0;
  return E_NOTIMPL;
}

HRESULT SinkPin::EndOfStream() {
  owner_->OnEndOfStream();
  return S_OK;
}

HRESULT SinkPin::BeginFlush() {
  flushing_ = true;
  owner_->OnFlush();
  return S_OK;
}

HRESULT SinkPin::EndFlush() {
  flushing_ = false;
  return S_OK;
}

HRESULT SinkPin::NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) {
  return S_OK;
}

HRESULT SinkPin::GetAllocator(IMemAllocator** allocator) {
  if (!allocator) return E_POINTER;
  *allocator = nullptr;
  // We never hold on to upstream buffers, so we have no opinion -- let the
  // capture filter supply its own allocator.
  return VFW_E_NO_ALLOCATOR;
}

HRESULT SinkPin::NotifyAllocator(IMemAllocator* allocator, BOOL) {
  allocator_ = allocator;
  return S_OK;
}

HRESULT SinkPin::GetAllocatorRequirements(ALLOCATOR_PROPERTIES*) {
  // No requirements. Buffer count does not affect our latency because Receive
  // copies and returns immediately, so the upstream queue never fills up.
  return E_NOTIMPL;
}

HRESULT SinkPin::Receive(IMediaSample* sample) {
  if (!sample) return E_POINTER;
  if (flushing_) return S_FALSE;
  const FILTER_STATE state = owner_->state();
  if (state == State_Stopped) return VFW_E_WRONG_STATE;
  return owner_->OnSample(sample);
}

HRESULT SinkPin::ReceiveMultiple(IMediaSample** samples, long count, long* processed) {
  if (!samples || !processed) return E_POINTER;
  *processed = 0;
  for (long i = 0; i < count; ++i) {
    HRESULT hr = Receive(samples[i]);
    if (FAILED(hr)) return hr;
    ++(*processed);
  }
  return S_OK;
}

HRESULT SinkPin::ReceiveCanBlock() {
  return S_FALSE;  // Receive never blocks
}

// ================================================================= FrameSink

FrameSink::FrameSink() {
  frameEvent_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

FrameSink::~FrameSink() {
  if (frameEvent_) ::CloseHandle(frameEvent_);
}

ComPtr<FrameSink> FrameSink::Create() {
  ComPtr<FrameSink> sink;
  sink.Attach(new (std::nothrow) FrameSink());  // constructed with refcount 1
  return sink;
}

HRESULT FrameSink::QueryInterface(REFIID riid, void** ppv) {
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

ULONG FrameSink::AddRef() {
  return (ULONG)ref_.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG FrameSink::Release() {
  LONG n = ref_.fetch_sub(1, std::memory_order_acq_rel) - 1;
  if (n == 0) delete this;
  return (ULONG)n;
}

HRESULT FrameSink::GetClassID(CLSID* clsid) {
  if (!clsid) return E_POINTER;
  *clsid = CLSID_qBlankFrameSink;
  return S_OK;
}

HRESULT FrameSink::Stop() {
  state_.store(State_Stopped, std::memory_order_relaxed);
  OnFlush();
  return S_OK;
}

HRESULT FrameSink::Pause() {
  state_.store(State_Paused, std::memory_order_relaxed);
  return S_OK;
}

HRESULT FrameSink::Run(REFERENCE_TIME) {
  ended_.store(false, std::memory_order_relaxed);
  state_.store(State_Running, std::memory_order_relaxed);
  return S_OK;
}

HRESULT FrameSink::GetState(DWORD, FILTER_STATE* state) {
  if (!state) return E_POINTER;
  // Always report the transition as complete: we never buffer waiting for data,
  // so making the graph wait for us would only add startup delay.
  *state = state_.load(std::memory_order_relaxed);
  return S_OK;
}

HRESULT FrameSink::SetSyncSource(IReferenceClock* clock) {
  clock_ = clock;
  return S_OK;
}

HRESULT FrameSink::GetSyncSource(IReferenceClock** clock) {
  if (!clock) return E_POINTER;
  *clock = clock_.Get();
  if (*clock) (*clock)->AddRef();
  return S_OK;
}

HRESULT FrameSink::EnumPins(IEnumPins** enumerator) {
  if (!enumerator) return E_POINTER;
  *enumerator = new (std::nothrow) EnumPinsImpl(static_cast<IPin*>(&pin_));
  return *enumerator ? S_OK : E_OUTOFMEMORY;
}

HRESULT FrameSink::FindPin(LPCWSTR id, IPin** pin) {
  if (!id || !pin) return E_POINTER;
  if (wcscmp(id, kPinId) != 0) {
    *pin = nullptr;
    return VFW_E_NOT_FOUND;
  }
  *pin = static_cast<IPin*>(&pin_);
  (*pin)->AddRef();
  return S_OK;
}

HRESULT FrameSink::QueryFilterInfo(FILTER_INFO* info) {
  if (!info) return E_POINTER;
  wcsncpy_s(info->achName, name_.c_str(), _TRUNCATE);
  info->pGraph = graph_;
  if (info->pGraph) info->pGraph->AddRef();
  return S_OK;
}

HRESULT FrameSink::JoinFilterGraph(IFilterGraph* graph, LPCWSTR name) {
  // Weak reference by design -- the graph owns us, not the other way round.
  graph_ = graph;
  if (name) name_ = name;
  return S_OK;
}

HRESULT FrameSink::QueryVendorInfo(LPWSTR* vendorInfo) {
  if (!vendorInfo) return E_POINTER;
  *vendorInfo = nullptr;
  return E_NOTIMPL;
}

ULONG FrameSink::GetMiscFlags() {
  return AM_FILTER_MISC_FLAGS_IS_RENDERER;
}

// ------------------------------------------------------------- pin callbacks

void FrameSink::OnConnected(const AM_MEDIA_TYPE* mt) {
  VideoFormatInfo info;
  if (!ParseVideoMediaType(mt, &info)) return;

  std::lock_guard<std::mutex> lock(mutex_);
  format_ = info;
  subtype_ = mt->subtype;
  for (int i = 0; i < 3; ++i) {
    slots_[i].assign(info.imageSize, 0);
    slotSize_[i] = 0;
  }
  readyIdx_ = -1;
  readIdx_ = -1;
  sequence_ = 0;
  readSequence_ = 0;
  received_ = dropped_ = displayed_ = 0;
  lastArrivalQpc_ = 0;
  measuredFps_ = 0.0;
  fpsWindowStartQpc_ = 0;
  fpsWindowFrames_ = 0;

  // Deliberately not logged here: intelligent connect calls this for every type
  // it tries, so a line per attempt would bury the one that stuck. The graph
  // logs the format it settled on once it is running.
}

void FrameSink::OnDisconnected() {
  std::lock_guard<std::mutex> lock(mutex_);
  format_ = VideoFormatInfo{};
  subtype_ = GUID_NULL;
  readyIdx_ = -1;
  readIdx_ = -1;
}

void FrameSink::OnEndOfStream() {
  CAP_WARN("Sink: end of stream from the capture filter");
  ended_.store(true, std::memory_order_relaxed);
}

void FrameSink::OnFlush() {
  std::lock_guard<std::mutex> lock(mutex_);
  readyIdx_ = -1;
}

int FrameSink::PickWriteSlotLocked() const {
  for (int i = 0; i < 3; ++i) {
    if (i != readyIdx_ && i != readIdx_) return i;
  }
  return 0;  // unreachable with three slots and two reserved indices
}

HRESULT FrameSink::OnSample(IMediaSample* sample) {
  // A mid-stream format change arrives as a media type attached to the sample.
  AM_MEDIA_TYPE* changed = nullptr;
  if (sample->GetMediaType(&changed) == S_OK && changed) {
    VideoFormatInfo info;
    if (ParseVideoMediaType(changed, &info)) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (info.width != format_.width || info.height != format_.height ||
          !IsEqualGUID(changed->subtype, subtype_)) {
        CAP_LOG("Format change mid-stream: %s %dx%d", info.subtypeLabel.c_str(), info.width,
                info.height);
        for (int i = 0; i < 3; ++i) {
          slots_[i].assign(info.imageSize, 0);
          slotSize_[i] = 0;
        }
        readyIdx_ = -1;
        readIdx_ = -1;
      }
      format_ = info;
      subtype_ = changed->subtype;
    }
    DeleteMediaType(changed);
  }

  BYTE* src = nullptr;
  if (FAILED(sample->GetPointer(&src)) || !src) return S_OK;
  const long length = sample->GetActualDataLength();
  if (length <= 0) return S_OK;

  int slot;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    slot = PickWriteSlotLocked();
    if ((size_t)length > slots_[slot].size()) slots_[slot].resize((size_t)length);
  }

  // Copy outside the lock. The chosen slot is neither the one the reader holds
  // nor the one waiting to be picked up, so nobody else can touch it. Only the
  // upstream streaming thread calls Receive, so there is no second writer.
  memcpy(slots_[slot].data(), src, (size_t)length);

  const int64_t now = QpcNow();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (readyIdx_ >= 0) ++dropped_;  // previous frame never made it to the screen
    slotSize_[slot] = (size_t)length;
    readyIdx_ = slot;
    ++sequence_;
    ++received_;
    lastArrivalQpc_ = now;

    // Arrival rate over a rolling one second window.
    if (fpsWindowStartQpc_ == 0) {
      fpsWindowStartQpc_ = now;
      fpsWindowFrames_ = 0;
    }
    ++fpsWindowFrames_;
    const double elapsed = QpcSeconds(now - fpsWindowStartQpc_);
    if (elapsed >= 1.0) {
      measuredFps_ = (double)fpsWindowFrames_ / elapsed;
      fpsWindowStartQpc_ = now;
      fpsWindowFrames_ = 0;
    }
  }
  if (frameEvent_) ::SetEvent(frameEvent_);
  return S_OK;
}

// ------------------------------------------------------------- reader side

int64_t FrameSink::lastArrivalQpc() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return lastArrivalQpc_;
}

bool FrameSink::AcquireFrame(FrameView* out) {
  std::lock_guard<std::mutex> lock(mutex_);
  bool isNew = false;
  if (readyIdx_ >= 0) {
    readIdx_ = readyIdx_;
    readyIdx_ = -1;
    readSequence_ = sequence_;
    ++displayed_;
    isNew = true;
  }
  if (out) {
    if (readIdx_ >= 0) {
      out->data = slots_[readIdx_].data();
      out->size = slotSize_[readIdx_];
      out->sequence = readSequence_;
    } else {
      *out = FrameView{};
    }
  }
  return isNew;
}

VideoFormatInfo FrameSink::format() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return format_;
}

SinkStats FrameSink::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  SinkStats s;
  s.received = received_;
  s.dropped = dropped_;
  s.displayed = displayed_;
  s.sourceFps = measuredFps_;
  s.lastArrivalAgeMs =
      lastArrivalQpc_ ? QpcSeconds(QpcNow() - lastArrivalQpc_) * 1000.0 : -1.0;
  return s;
}

void FrameSink::ResetStats() {
  std::lock_guard<std::mutex> lock(mutex_);
  received_ = dropped_ = displayed_ = 0;
  measuredFps_ = 0.0;
  fpsWindowStartQpc_ = 0;
  fpsWindowFrames_ = 0;
}

bool FrameSink::HasRecentFrame(double withinSeconds) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (lastArrivalQpc_ == 0) return false;
  return QpcSeconds(QpcNow() - lastArrivalQpc_) <= withinSeconds;
}

}  // namespace cap
