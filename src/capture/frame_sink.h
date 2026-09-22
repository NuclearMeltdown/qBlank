#pragma once

// A minimal DirectShow renderer filter.
//
// The stock renderers (VMR9 / EVR) schedule samples against the graph clock and
// keep a queue, which is exactly the latency we are trying to avoid. This one
// does the opposite: it copies each incoming sample into a free slot of a
// triple buffer and returns immediately, and the render thread always picks up
// the newest slot. Frames that arrive faster than we display are dropped rather
// than queued, so the picture is always as fresh as the card can make it.

#include <dshow.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "capture/dshow_util.h"
#include "common_win32.h"

namespace cap {

// Points at the slot the reader currently holds. Stays valid until the next
// AcquireFrame call on the same thread.
struct FrameView {
  const uint8_t* data = nullptr;
  size_t size = 0;
  uint64_t sequence = 0;

  bool valid() const { return data != nullptr && size > 0; }
};

struct SinkStats {
  uint64_t received = 0;
  uint64_t dropped = 0;    // arrived but overwritten before being displayed
  uint64_t displayed = 0;  // picked up by the render thread
  double sourceFps = 0.0;  // measured arrival rate
  double lastArrivalAgeMs = 0.0;
};

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

  // ---- reader side (render thread) ----

  // Moves the newest completed frame into the reader's hands. Returns true when
  // that frame is one the reader has not seen yet. `out` always describes the
  // currently held frame, even when it is the previous one.
  bool AcquireFrame(FrameView* out);

  VideoFormatInfo format() const;
  SinkStats stats() const;

  // When the newest frame was handed over by the driver, on the QPC clock.
  // Zero before the first one. Scheduling anything from the moment we get round
  // to drawing instead of from this is how a second field ends up due after the
  // next frame has already arrived.
  int64_t lastArrivalQpc() const;
  void ResetStats();

  // True when a frame arrived within the given window -- used to show the
  // "no signal" state without tearing the graph down.
  bool HasRecentFrame(double withinSeconds) const;

  // Set when the upstream filter signalled end of stream (device unplugged,
  // driver gave up).
  bool ended() const { return ended_.load(std::memory_order_relaxed); }

  // Signalled whenever a frame is published, so the render loop can sleep
  // instead of polling. Auto-reset; owned by the sink.
  HANDLE frameEvent() const { return frameEvent_; }

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
  int PickWriteSlotLocked() const;

  std::atomic<LONG> ref_{1};
  SinkPin pin_{this};
  HANDLE frameEvent_ = nullptr;

  std::atomic<FILTER_STATE> state_{State_Stopped};
  std::atomic<bool> ended_{false};

  IFilterGraph* graph_ = nullptr;  // weak, per DirectShow rules
  std::wstring name_ = L"qBlank Frame Sink";
  ComPtr<IReferenceClock> clock_;

  mutable std::mutex mutex_;
  std::vector<uint8_t> slots_[3];
  size_t slotSize_[3] = {0, 0, 0};
  int readyIdx_ = -1;  // newest completed frame, not yet taken by the reader
  int readIdx_ = -1;   // slot the reader currently holds
  uint64_t sequence_ = 0;
  uint64_t readSequence_ = 0;
  VideoFormatInfo format_;

  // Stats.
  uint64_t received_ = 0;
  uint64_t dropped_ = 0;
  uint64_t displayed_ = 0;
  int64_t lastArrivalQpc_ = 0;
  double measuredFps_ = 0.0;
  int64_t fpsWindowStartQpc_ = 0;
  uint64_t fpsWindowFrames_ = 0;
};

}  // namespace cap
