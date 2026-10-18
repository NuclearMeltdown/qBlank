#pragma once

// A minimal DirectShow renderer filter.
//
// The stock renderers (VMR9 / EVR) schedule samples against the graph clock and
// keep a queue, which is exactly the latency we are trying to avoid. This one
// does the opposite: it copies each incoming sample into the FrameBuffer and
// returns immediately, and the render thread picks the newest frame up from
// there.

#include <dshow.h>

#include <atomic>

#include "capture/dshow_util.h"
#include "capture/frame_buffer.h"
#include "common_win32.h"

namespace cap {

class FrameSink;

// The single input pin. Lifetime is tied to the filter, so its IUnknown
// delegates to the owner.
class SinkPin final : public IPin, public IMemInputPin {
 public:
  explicit SinkPin(FrameSink* owner) : owner_(owner) {}
  ~SinkPin();

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;

  // IPin
  HRESULT STDMETHODCALLTYPE Connect(IPin* receive, const AM_MEDIA_TYPE* mt) override;
  HRESULT STDMETHODCALLTYPE ReceiveConnection(IPin* connector, const AM_MEDIA_TYPE* mt) override;
  HRESULT STDMETHODCALLTYPE Disconnect() override;
  HRESULT STDMETHODCALLTYPE ConnectedTo(IPin** pin) override;
  HRESULT STDMETHODCALLTYPE ConnectionMediaType(AM_MEDIA_TYPE* mt) override;
  HRESULT STDMETHODCALLTYPE QueryPinInfo(PIN_INFO* info) override;
  HRESULT STDMETHODCALLTYPE QueryDirection(PIN_DIRECTION* dir) override;
  HRESULT STDMETHODCALLTYPE QueryId(LPWSTR* id) override;
  HRESULT STDMETHODCALLTYPE QueryAccept(const AM_MEDIA_TYPE* mt) override;
  HRESULT STDMETHODCALLTYPE EnumMediaTypes(IEnumMediaTypes** enumerator) override;
  HRESULT STDMETHODCALLTYPE QueryInternalConnections(IPin** pins, ULONG* count) override;
  HRESULT STDMETHODCALLTYPE EndOfStream() override;
  HRESULT STDMETHODCALLTYPE BeginFlush() override;
  HRESULT STDMETHODCALLTYPE EndFlush() override;
  HRESULT STDMETHODCALLTYPE NewSegment(REFERENCE_TIME start, REFERENCE_TIME stop,
                                       double rate) override;

  // IMemInputPin
  HRESULT STDMETHODCALLTYPE GetAllocator(IMemAllocator** allocator) override;
  HRESULT STDMETHODCALLTYPE NotifyAllocator(IMemAllocator* allocator, BOOL readOnly) override;
  HRESULT STDMETHODCALLTYPE GetAllocatorRequirements(ALLOCATOR_PROPERTIES* props) override;
  HRESULT STDMETHODCALLTYPE Receive(IMediaSample* sample) override;
  HRESULT STDMETHODCALLTYPE ReceiveMultiple(IMediaSample** samples, long count,
                                            long* processed) override;
  HRESULT STDMETHODCALLTYPE ReceiveCanBlock() override;

  bool connected() const { return connected_ != nullptr; }
  const AM_MEDIA_TYPE& media_type() const { return mediaType_; }

 private:
  HRESULT CheckMediaType(const AM_MEDIA_TYPE* mt) const;

  FrameSink* owner_;
  ComPtr<IPin> connected_;
  ComPtr<IMemAllocator> allocator_;
  AM_MEDIA_TYPE mediaType_ = {};
  bool flushing_ = false;
};

class FrameSink final : public IBaseFilter, public IAMFilterMiscFlags {
 public:
  FrameSink();
  ~FrameSink();

  static ComPtr<FrameSink> Create();

  // Where the frames go, and where the render thread takes them from.
  FrameBuffer& buffer() { return buffer_; }
  const FrameBuffer& buffer() const { return buffer_; }

  // Set when the upstream filter signalled end of stream (device unplugged,
  // driver gave up).
  bool ended() const { return ended_.load(std::memory_order_relaxed); }

  // ---- IUnknown ----
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;

  // ---- IPersist / IMediaFilter / IBaseFilter ----
  HRESULT STDMETHODCALLTYPE GetClassID(CLSID* clsid) override;
  HRESULT STDMETHODCALLTYPE Stop() override;
  HRESULT STDMETHODCALLTYPE Pause() override;
  HRESULT STDMETHODCALLTYPE Run(REFERENCE_TIME start) override;
  HRESULT STDMETHODCALLTYPE GetState(DWORD msTimeout, FILTER_STATE* state) override;
  HRESULT STDMETHODCALLTYPE SetSyncSource(IReferenceClock* clock) override;
  HRESULT STDMETHODCALLTYPE GetSyncSource(IReferenceClock** clock) override;
  HRESULT STDMETHODCALLTYPE EnumPins(IEnumPins** enumerator) override;
  HRESULT STDMETHODCALLTYPE FindPin(LPCWSTR id, IPin** pin) override;
  HRESULT STDMETHODCALLTYPE QueryFilterInfo(FILTER_INFO* info) override;
  HRESULT STDMETHODCALLTYPE JoinFilterGraph(IFilterGraph* graph, LPCWSTR name) override;
  HRESULT STDMETHODCALLTYPE QueryVendorInfo(LPWSTR* vendorInfo) override;

  // ---- IAMFilterMiscFlags ----
  ULONG STDMETHODCALLTYPE GetMiscFlags() override;

  // Called by the pin.
  void OnConnected(const AM_MEDIA_TYPE* mt);
  void OnDisconnected();
  HRESULT OnSample(IMediaSample* sample);
  void OnEndOfStream();
  void OnFlush();
  FILTER_STATE state() const { return state_.load(std::memory_order_relaxed); }
  SinkPin* pin() { return &pin_; }

 private:
  std::atomic<LONG> ref_{1};
  SinkPin pin_{this};
  FrameBuffer buffer_;

  std::atomic<FILTER_STATE> state_{State_Stopped};
  std::atomic<bool> ended_{false};

  IFilterGraph* graph_ = nullptr;  // weak, per DirectShow rules
  std::wstring name_ = L"qBlank Frame Sink";
  ComPtr<IReferenceClock> clock_;

  // The buffer's format by DirectShow's name for it. Only the pin callbacks
  // touch it, and DirectShow never runs those at the same time: pins connect
  // and disconnect while the graph is stopped, and samples come only while it
  // is not.
  GUID subtype_ = GUID_NULL;
};

}  // namespace cap
