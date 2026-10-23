#include "record/gpu_frames.h"

#include <d3d11_4.h>
#include <dxgi1_2.h>

#include <cstring>
#include <vector>

#include "common_win32.h"
#include "inflate.h"
#include "record/libav.h"
#include "render/shared_frames.h"

// The two handles hwcontext_cuda.h takes from cuda.h, declared as cuda.h does.
#define CUDA_VERSION 12000
typedef struct CUctx_st* CUcontext;
typedef struct CUstream_st* CUstream;

extern "C" {
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/hwcontext_d3d11va.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
}

// The same bytecode the renderer draws with (render_passes_win32.cpp).
#ifdef QBLANK_SHADERS_PACKED
#include "shaders_dxbc.h"
#else
namespace cap {
extern const Packed kFullscreenVS, kYuvSplitPS;
}
#endif

namespace cap {
namespace {

// kYuvSplitPS's constants.
struct SplitCB {
  uint32_t chroma;  // 0 = the luma plane, 1 = the chroma plane
  int32_t height;   // the luma rows, where the chroma rows start
  int32_t pad[2];
};
static_assert(sizeof(SplitCB) == 16, "constant buffer must be 16 byte aligned");

// The textures stay open as long as either side holds them.
class OwnedFrames : public SharedFrames {
 public:
  ~OwnedFrames() override {
    for (void* h : handle) {
      if (h) CloseHandle(h);
    }
  }
};

bool Failed(HRESULT hr, const char* what) {
  if (SUCCEEDED(hr)) return false;
  CAP_WARN("Recording on the GPU: %s failed: %s", what, HrToEnglish(hr).c_str());
  return true;
}

bool Failed(int code, const char* what) {
  if (code >= 0) return false;
  CAP_WARN("Recording on the GPU: %s failed: %s", what, LibavError(code).c_str());
  return true;
}

// The device on the renderer's adapter and the three textures the yuv pass
// draws into, whichever way the picture goes on from there.
class SharedGpuFrames : public GpuFrames {
 public:
  ~SharedGpuFrames() override {
    av_.av_buffer_unref(&frames_);
    av_.av_buffer_unref(&device_);
  }

  std::shared_ptr<SharedFrames> frames() const override { return shared_; }
  AVBufferRef* framesContext() const override { return frames_; }

 protected:
  explicit SharedGpuFrames(const Libav& av) : av_(av) {}

  // The textures hold `rows` of the yuv pass's image.
  bool CreateShared(uint64_t adapter, UINT deviceFlags, int width, int height, int rows,
                    bool sixteenBit);
  // Key 1 once the yuv pass has drawn it. That is released before the slot is
  // handed over, so this hardly ever waits; a renderer that went away does not
  // hold the recording up for more than a second.
  bool Acquire(int slot, const std::atomic<bool>& keepWaiting);
  void Release(int slot);
  // To the renderer.
  void GiveBack(int slot) {
    shared_->state[slot].store(SharedFrames::Free, std::memory_order_release);
  }

  const Libav& av_;
  ComPtr<IDXGIAdapter1> adapter_;
  ComPtr<ID3D11Device> dev_;
  ComPtr<ID3D11DeviceContext> dc_;
  AVBufferRef* device_ = nullptr;
  AVBufferRef* frames_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  bool sixteenBit_ = false;

  std::shared_ptr<OwnedFrames> shared_;
  ComPtr<ID3D11Texture2D> tex_[SharedFrames::kSlots];
  ComPtr<IDXGIKeyedMutex> mutex_[SharedFrames::kSlots];
};

bool SharedGpuFrames::CreateShared(uint64_t adapter, UINT deviceFlags, int width, int height,
                                   int rows, bool sixteenBit) {
  width_ = width;
  height_ = height;
  sixteenBit_ = sixteenBit;

  // The device, on the renderer's adapter: a texture is shared within one.
  ComPtr<IDXGIFactory1> factory;
  if (Failed(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1")) return false;
  ComPtr<IDXGIAdapter1> a;
  for (UINT i = 0; !adapter_ && factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 desc = {};
    uint64_t luid = 0;
    if (SUCCEEDED(a->GetDesc1(&desc))) memcpy(&luid, &desc.AdapterLuid, sizeof(luid));
    if (luid == adapter) adapter_ = a;
  }
  if (!adapter_) {
    CAP_WARN("Recording on the GPU: the renderer's adapter is not there");
    return false;
  }
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  if (Failed(D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, deviceFlags,
                               levels, 2, D3D11_SDK_VERSION, &dev_, nullptr, &dc_),
             "D3D11CreateDevice")) {
    return false;
  }

  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)width;
  td.Height = (UINT)rows;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = sixteenBit ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R8_UINT;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
  shared_ = std::make_shared<OwnedFrames>();
  shared_->width = width;
  shared_->height = height;
  shared_->rows = rows;
  shared_->sixteenBit = sixteenBit;
  for (int i = 0; i < SharedFrames::kSlots; ++i) {
    ComPtr<IDXGIResource1> resource;
    HANDLE handle = nullptr;
    if (Failed(dev_->CreateTexture2D(&td, nullptr, &tex_[i]), "CreateTexture2D") ||
        Failed(tex_[i].As(&mutex_[i]), "IDXGIKeyedMutex") ||
        Failed(tex_[i].As(&resource), "IDXGIResource1") ||
        Failed(resource->CreateSharedHandle(
                   nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                   &handle),
               "CreateSharedHandle")) {
      return false;
    }
    shared_->handle[i] = handle;
  }
  return true;
}

bool SharedGpuFrames::Acquire(int slot, const std::atomic<bool>& keepWaiting) {
  IDXGIKeyedMutex* mutex = mutex_[slot].Get();
  HRESULT hr = (HRESULT)WAIT_TIMEOUT;
  for (int i = 0; i < 10 && hr == (HRESULT)WAIT_TIMEOUT &&
                  keepWaiting.load(std::memory_order_relaxed);
       ++i) {
    hr = mutex->AcquireSync(1, 100);
  }
  if (hr == S_OK) return true;
  if (hr != (HRESULT)WAIT_TIMEOUT) CAP_WARN("Recording on the GPU: AcquireSync gave 0x%08lx", hr);
  return false;
}

void SharedGpuFrames::Release(int slot) {
  mutex_[slot]->ReleaseSync(0);
  shared_->key[slot] = 0;
}

// ---- NV12 and P010: D3D11 frames ----
//
// One small pass splits a texture's luma and chroma rows into a frame of the
// encoder's D3D11 pool.

class D3D11GpuFrames : public SharedGpuFrames {
 public:
  explicit D3D11GpuFrames(const Libav& av) : SharedGpuFrames(av) {}
  ~D3D11GpuFrames() override;

  bool Create(uint64_t adapter, int width, int height, bool sixteenBit);
  bool Convert(int slot, AVFrame* out, const std::atomic<bool>& keepWaiting) override;

 private:
  // The views into one frame of the pool, made the first time it comes by.
  // Each holds the texture, so no other one can turn up at the same address.
  struct Targets {
    ID3D11Texture2D* texture;
    intptr_t index;
    ComPtr<ID3D11RenderTargetView> luma;
    ComPtr<ID3D11RenderTargetView> chroma;
  };
  Targets* TargetsFor(const AVFrame* frame);
  bool CreateShader(const Packed& packed, bool pixel);

  AVD3D11VADeviceContext* hw_ = nullptr;
  ComPtr<ID3D11ShaderResourceView> srv_[SharedFrames::kSlots];

  ComPtr<ID3D11VertexShader> vs_;
  ComPtr<ID3D11PixelShader> ps_;
  ComPtr<ID3D11Buffer> cbLuma_;
  ComPtr<ID3D11Buffer> cbChroma_;
  ComPtr<ID3D11RasterizerState> raster_;
  std::vector<Targets> targets_;

  // A slot goes back once its split has run, so that the renderer's next
  // picture into it does not wait for that on the GPU, and the recorder does
  // not wait either. Without a fence, or after a second, it goes back at once;
  // the keyed mutex keeps the order then.
  struct Pending {
    D3D11GpuFrames* self;
    int slot;
    HANDLE done;
    PTP_WAIT wait;
  };
  static void CALLBACK SplitDone(PTP_CALLBACK_INSTANCE, void* context, PTP_WAIT, TP_WAIT_RESULT);
  void GiveBackAfterSplit(int slot);

  ComPtr<ID3D11DeviceContext4> dc4_;
  ComPtr<ID3D11Fence> fence_;
  uint64_t splits_ = 0;
  Pending pending_[SharedFrames::kSlots] = {};
};

D3D11GpuFrames::~D3D11GpuFrames() {
  for (Pending& p : pending_) {
    if (p.wait) {
      SetThreadpoolWait(p.wait, nullptr, nullptr);
      WaitForThreadpoolWaitCallbacks(p.wait, TRUE);
      CloseThreadpoolWait(p.wait);
    }
    if (p.done) CloseHandle(p.done);
  }
  // What still waited goes back now.
  for (int i = 0; shared_ && i < SharedFrames::kSlots; ++i) {
    uint8_t converting = SharedFrames::Converting;
    shared_->state[i].compare_exchange_strong(converting, SharedFrames::Free);
  }
  targets_.clear();
}

void CALLBACK D3D11GpuFrames::SplitDone(PTP_CALLBACK_INSTANCE, void* context, PTP_WAIT,
                                        TP_WAIT_RESULT) {
  const Pending* p = (const Pending*)context;
  p->self->GiveBack(p->slot);
}

void D3D11GpuFrames::GiveBackAfterSplit(int slot) {
  Pending& p = pending_[slot];
  if (p.wait && fence_->GetCompletedValue() < splits_ &&
      SUCCEEDED(fence_->SetEventOnCompletion(splits_, p.done))) {
    const int64_t second = -10000000;  // relative, in 100 ns
    FILETIME timeout = {(DWORD)second, (DWORD)(second >> 32)};
    SetThreadpoolWait(p.wait, p.done, &timeout);
    return;
  }
  GiveBack(slot);
}

bool D3D11GpuFrames::CreateShader(const Packed& packed, bool pixel) {
  std::vector<uint8_t> code(packed.unpackedSize);
  if (!Inflate(packed, code.data())) {
    CAP_ERR("Recording on the GPU: a shader is damaged");
    return false;
  }
  return pixel ? !Failed(dev_->CreatePixelShader(code.data(), code.size(), nullptr, &ps_),
                         "CreatePixelShader")
               : !Failed(dev_->CreateVertexShader(code.data(), code.size(), nullptr, &vs_),
                         "CreateVertexShader");
}

bool D3D11GpuFrames::Create(uint64_t adapter, int width, int height, bool sixteenBit) {
  if (!CreateShared(adapter, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, width, height,
                    height + height / 2, sixteenBit)) {
    return false;
  }
  // The encoder may use the device from threads of its own, as ffmpeg's own
  // devices allow.
  ComPtr<ID3D11Multithread> mt;
  if (SUCCEEDED(dc_.As(&mt))) mt->SetMultithreadProtected(TRUE);

  // ffmpeg's device on it, and the pool. Without a pool size ffmpeg makes the
  // frames one texture each, as many as the encoder holds at a time.
  device_ = av_.av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
  if (!device_) return false;
  hw_ = (AVD3D11VADeviceContext*)((AVHWDeviceContext*)device_->data)->hwctx;
  hw_->device = dev_.Get();
  dev_->AddRef();  // ffmpeg releases its own
  if (Failed(av_.av_hwdevice_ctx_init(device_), "av_hwdevice_ctx_init")) return false;

  frames_ = av_.av_hwframe_ctx_alloc(device_);
  if (!frames_) return false;
  AVHWFramesContext* fc = (AVHWFramesContext*)frames_->data;
  fc->format = AV_PIX_FMT_D3D11;
  fc->sw_format = sixteenBit ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12;
  fc->width = width;
  fc->height = height;
  fc->initial_pool_size = 0;
  ((AVD3D11VAFramesContext*)fc->hwctx)->BindFlags |= D3D11_BIND_RENDER_TARGET;
  if (Failed(av_.av_hwframe_ctx_init(frames_), "av_hwframe_ctx_init")) return false;

  D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
  sd.Format = sixteenBit ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R8_UINT;
  sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  sd.Texture2D.MipLevels = 1;
  for (int i = 0; i < SharedFrames::kSlots; ++i) {
    if (Failed(dev_->CreateShaderResourceView(tex_[i].Get(), &sd, &srv_[i]),
               "CreateShaderResourceView")) {
      return false;
    }
  }

  // The split.
  if (!CreateShader(kFullscreenVS, false) || !CreateShader(kYuvSplitPS, true)) return false;
  D3D11_BUFFER_DESC bd = {};
  bd.ByteWidth = sizeof(SplitCB);
  bd.Usage = D3D11_USAGE_IMMUTABLE;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  SplitCB cb = {0, height, {0, 0}};
  D3D11_SUBRESOURCE_DATA init = {&cb, 0, 0};
  if (Failed(dev_->CreateBuffer(&bd, &init, &cbLuma_), "CreateBuffer")) return false;
  cb.chroma = 1;
  if (Failed(dev_->CreateBuffer(&bd, &init, &cbChroma_), "CreateBuffer")) return false;
  D3D11_RASTERIZER_DESC rd = {};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  if (Failed(dev_->CreateRasterizerState(&rd, &raster_), "CreateRasterizerState")) return false;

  // The fence the slots go back by.
  ComPtr<ID3D11Device5> dev5;
  if (SUCCEEDED(dev_.As(&dev5)) && SUCCEEDED(dc_.As(&dc4_)) &&
      SUCCEEDED(dev5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
    for (int i = 0; i < SharedFrames::kSlots; ++i) {
      Pending& p = pending_[i];
      p.self = this;
      p.slot = i;
      p.done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      if (p.done) p.wait = CreateThreadpoolWait(SplitDone, &p, nullptr);
    }
  }

  // One frame of the pool and its views, now: drawing into NV12 is up to the
  // hardware, and better found out before the encoder opens than after.
  AVFrame* probe = av_.av_frame_alloc();
  bool ok = probe &&
            !Failed(av_.av_hwframe_get_buffer(frames_, probe, 0), "av_hwframe_get_buffer") &&
            TargetsFor(probe);
  av_.av_frame_free(&probe);
  return ok;
}

D3D11GpuFrames::Targets* D3D11GpuFrames::TargetsFor(const AVFrame* frame) {
  ID3D11Texture2D* texture = (ID3D11Texture2D*)frame->data[0];
  const intptr_t index = (intptr_t)frame->data[1];
  for (Targets& t : targets_) {
    if (t.texture == texture && t.index == index) return &t;
  }
  if (!texture) return nullptr;

  // The plane follows from the view's format: one sample for luma, two for
  // the chroma at half the size.
  D3D11_TEXTURE2D_DESC td = {};
  texture->GetDesc(&td);
  D3D11_RENDER_TARGET_VIEW_DESC rd = {};
  if (td.ArraySize > 1) {
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
    rd.Texture2DArray.FirstArraySlice = (UINT)index;
    rd.Texture2DArray.ArraySize = 1;
  } else {
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
  }
  Targets t = {texture, index, nullptr, nullptr};
  rd.Format = sixteenBit_ ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R8_UINT;
  if (Failed(dev_->CreateRenderTargetView(texture, &rd, &t.luma), "luma view")) return nullptr;
  rd.Format = sixteenBit_ ? DXGI_FORMAT_R16G16_UINT : DXGI_FORMAT_R8G8_UINT;
  if (Failed(dev_->CreateRenderTargetView(texture, &rd, &t.chroma), "chroma view")) return nullptr;
  targets_.push_back(std::move(t));
  return &targets_.back();
}

bool D3D11GpuFrames::Convert(int slot, AVFrame* out, const std::atomic<bool>& keepWaiting) {
  if (!Acquire(slot, keepWaiting)) {
    GiveBack(slot);
    return false;
  }

  const Targets* t = TargetsFor(out);
  if (t) {
    hw_->lock(hw_->lock_ctx);
    ID3D11DeviceContext* dc = dc_.Get();
    dc->IASetInputLayout(nullptr);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->VSSetShader(vs_.Get(), nullptr, 0);
    dc->PSSetShader(ps_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srv[] = {srv_[slot].Get()};
    dc->PSSetShaderResources(0, 1, srv);
    dc->RSSetState(raster_.Get());
    dc->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    dc->OMSetDepthStencilState(nullptr, 0);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)width_;
    vp.Height = (float)height_;
    vp.MaxDepth = 1.0f;
    ID3D11Buffer* cb[] = {cbLuma_.Get()};
    ID3D11RenderTargetView* rtv[] = {t->luma.Get()};
    dc->OMSetRenderTargets(1, rtv, nullptr);
    dc->RSSetViewports(1, &vp);
    dc->PSSetConstantBuffers(0, 1, cb);
    dc->Draw(3, 0);

    vp.Width = (float)(width_ / 2);
    vp.Height = (float)(height_ / 2);
    cb[0] = cbChroma_.Get();
    rtv[0] = t->chroma.Get();
    dc->OMSetRenderTargets(1, rtv, nullptr);
    dc->RSSetViewports(1, &vp);
    dc->PSSetConstantBuffers(0, 1, cb);
    dc->Draw(3, 0);

    // Nothing stays bound to what the encoder and the renderer take next.
    dc->OMSetRenderTargets(0, nullptr, nullptr);
    srv[0] = nullptr;
    dc->PSSetShaderResources(0, 1, srv);
    if (fence_) dc4_->Signal(fence_.Get(), ++splits_);
    dc->Flush();
    hw_->unlock(hw_->lock_ctx);
  }

  Release(slot);
  if (t) {
    GiveBackAfterSplit(slot);
  } else {
    GiveBack(slot);
  }
  return t != nullptr;
}

// ---- 4:4:4: CUDA frames ----
//
// No D3D11 texture holds planar 4:4:4, and NVENC, the one hardware encoder
// that encodes it, also takes CUDA frames. The yuv pass's image already is the
// three planes one under the other, so three copies on the GPU move it into a
// frame of the pool. CUDA does not register a texture shared by NT handle, so
// the slot is first copied into one of this device's own, which it does. The
// driver's library is opened at run time; what is declared here is the little
// of cuda.h that is called.

using CUresult = int;
using CUdevice = int;
using CUdeviceptr = unsigned long long;
typedef struct CUarray_st* CUarray;
typedef struct CUgraphicsResource_st* CUgraphicsResource;

enum CUmemorytype : int { kCuMemoryDevice = 2, kCuMemoryArray = 3 };

struct CudaCopy2D {  // CUDA_MEMCPY2D
  size_t srcXInBytes, srcY;
  CUmemorytype srcMemoryType;
  const void* srcHost;
  CUdeviceptr srcDevice;
  CUarray srcArray;
  size_t srcPitch;
  size_t dstXInBytes, dstY;
  CUmemorytype dstMemoryType;
  void* dstHost;
  CUdeviceptr dstDevice;
  CUarray dstArray;
  size_t dstPitch;
  size_t WidthInBytes, Height;
};

struct Cuda {
  CUresult(__stdcall* cuInit)(unsigned flags);
  CUresult(__stdcall* cuDeviceGetCount)(int* count);
  CUresult(__stdcall* cuDeviceGet)(CUdevice* device, int ordinal);
  CUresult(__stdcall* cuD3D11GetDevice)(CUdevice* device, IDXGIAdapter* adapter);
  CUresult(__stdcall* cuCtxPushCurrent_v2)(CUcontext ctx);
  CUresult(__stdcall* cuCtxPopCurrent_v2)(CUcontext* ctx);
  CUresult(__stdcall* cuGraphicsD3D11RegisterResource)(CUgraphicsResource* resource,
                                                       ID3D11Resource* d3d, unsigned flags);
  CUresult(__stdcall* cuGraphicsUnregisterResource)(CUgraphicsResource resource);
  CUresult(__stdcall* cuGraphicsMapResources)(unsigned count, CUgraphicsResource* resources,
                                              CUstream stream);
  CUresult(__stdcall* cuGraphicsUnmapResources)(unsigned count, CUgraphicsResource* resources,
                                                CUstream stream);
  CUresult(__stdcall* cuGraphicsSubResourceGetMappedArray)(CUarray* array,
                                                           CUgraphicsResource resource,
                                                           unsigned index, unsigned level);
  CUresult(__stdcall* cuMemcpy2DAsync_v2)(const CudaCopy2D* copy, CUstream stream);
  CUresult(__stdcall* cuStreamSynchronize)(CUstream stream);
  CUresult(__stdcall* cuGetErrorName)(CUresult error, const char** name);
};

#define QB_CUDA_FUNCTIONS(X)                                                                    \
  X(cuInit) X(cuDeviceGetCount) X(cuDeviceGet) X(cuD3D11GetDevice) X(cuCtxPushCurrent_v2)       \
  X(cuCtxPopCurrent_v2) X(cuGraphicsD3D11RegisterResource) X(cuGraphicsUnregisterResource)      \
  X(cuGraphicsMapResources) X(cuGraphicsUnmapResources) X(cuGraphicsSubResourceGetMappedArray) \
  X(cuMemcpy2DAsync_v2) X(cuStreamSynchronize) X(cuGetErrorName)

class CudaGpuFrames : public SharedGpuFrames {
 public:
  explicit CudaGpuFrames(const Libav& av) : SharedGpuFrames(av) {}
  ~CudaGpuFrames() override;

  bool Create(uint64_t adapter, int width, int height, bool sixteenBit);
  bool Convert(int slot, AVFrame* out, const std::atomic<bool>& keepWaiting) override;

 private:
  bool CuFailed(CUresult result, const char* what) const;

  HMODULE dll_ = nullptr;
  Cuda cu_ = {};
  // ffmpeg's: the pool's memory is the context's, and frames the encoder
  // still holds may outlive this.
  CUcontext ctx_ = nullptr;
  ComPtr<ID3D11Texture2D> copy_;
  CUgraphicsResource resource_ = nullptr;
};

bool CudaGpuFrames::CuFailed(CUresult result, const char* what) const {
  if (result == 0) return false;
  const char* name = nullptr;
  if (cu_.cuGetErrorName(result, &name) != 0 || !name) name = "?";
  CAP_WARN("Recording on the GPU: %s failed: %s (%d)", what, name, result);
  return true;
}

CudaGpuFrames::~CudaGpuFrames() {
  if (resource_ && cu_.cuCtxPushCurrent_v2(ctx_) == 0) {
    cu_.cuGraphicsUnregisterResource(resource_);
    CUcontext popped = nullptr;
    cu_.cuCtxPopCurrent_v2(&popped);
  }
  av_.av_buffer_unref(&frames_);
  av_.av_buffer_unref(&device_);
  if (dll_) FreeLibrary(dll_);  // ffmpeg holds its own as long as it needs it
}

bool CudaGpuFrames::Create(uint64_t adapter, int width, int height, bool sixteenBit) {
  dll_ = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!dll_) {
    CAP_LOG("Recording on the GPU: no CUDA driver; 4:4:4 is read back");
    return false;
  }
#define QB_CUDA_LOAD(name)                                                    \
  if (!(cu_.name = (decltype(cu_.name))(void*)GetProcAddress(dll_, #name))) { \
    CAP_WARN("Recording on the GPU: nvcuda.dll has no " #name);              \
    return false;                                                             \
  }
  QB_CUDA_FUNCTIONS(QB_CUDA_LOAD)
#undef QB_CUDA_LOAD

  if (!CreateShared(adapter, 0, width, height, height * 3, sixteenBit)) return false;

  // The renderer's adapter as CUDA counts them, for ffmpeg's device on it.
  CUdevice device = 0;
  int count = 0;
  int ordinal = -1;
  if (CuFailed(cu_.cuInit(0), "cuInit") ||
      CuFailed(cu_.cuD3D11GetDevice(&device, adapter_.Get()), "cuD3D11GetDevice") ||
      CuFailed(cu_.cuDeviceGetCount(&count), "cuDeviceGetCount")) {
    return false;
  }
  for (int i = 0; i < count && ordinal < 0; ++i) {
    CUdevice d = 0;
    if (cu_.cuDeviceGet(&d, i) == 0 && d == device) ordinal = i;
  }
  if (ordinal < 0) {
    CAP_WARN("Recording on the GPU: CUDA does not list the renderer's adapter");
    return false;
  }
  if (Failed(av_.av_hwdevice_ctx_create(&device_, AV_HWDEVICE_TYPE_CUDA,
                                        Format("%d", ordinal).c_str(), nullptr, 0),
             "av_hwdevice_ctx_create")) {
    return false;
  }
  ctx_ = ((AVCUDADeviceContext*)((AVHWDeviceContext*)device_->data)->hwctx)->cuda_ctx;

  frames_ = av_.av_hwframe_ctx_alloc(device_);
  if (!frames_) return false;
  AVHWFramesContext* fc = (AVHWFramesContext*)frames_->data;
  fc->format = AV_PIX_FMT_CUDA;
  // Ten bits at the top of sixteen, as the yuv pass writes them for NVENC.
  fc->sw_format = sixteenBit ? AV_PIX_FMT_YUV444P16 : AV_PIX_FMT_YUV444P;
  fc->width = width;
  fc->height = height;
  fc->initial_pool_size = 0;
  if (Failed(av_.av_hwframe_ctx_init(frames_), "av_hwframe_ctx_init")) return false;

  // The texture CUDA copies from: a slot's own, unshared.
  D3D11_TEXTURE2D_DESC td = {};
  tex_[0]->GetDesc(&td);
  td.MiscFlags = 0;
  if (Failed(dev_->CreateTexture2D(&td, nullptr, &copy_), "CreateTexture2D")) return false;
  if (CuFailed(cu_.cuCtxPushCurrent_v2(ctx_), "cuCtxPushCurrent")) return false;
  const bool ok = !CuFailed(cu_.cuGraphicsD3D11RegisterResource(&resource_, copy_.Get(), 0),
                            "cuGraphicsD3D11RegisterResource");
  CUcontext popped = nullptr;
  cu_.cuCtxPopCurrent_v2(&popped);
  return ok;
}

bool CudaGpuFrames::Convert(int slot, AVFrame* out, const std::atomic<bool>& keepWaiting) {
  // The copy waits for the yuv pass on the keyed mutex. The slot goes back
  // once the map below has waited for the copy.
  if (!Acquire(slot, keepWaiting)) {
    GiveBack(slot);
    return false;
  }
  dc_->CopyResource(copy_.Get(), tex_[slot].Get());
  Release(slot);

  bool ok = !CuFailed(cu_.cuCtxPushCurrent_v2(ctx_), "cuCtxPushCurrent");
  if (ok) {
    // The map waits for the D3D11 copy, the unmap has D3D11 wait for CUDA's.
    // Waiting for them here as well means the encoder never sees a frame half
    // filled.
    CUgraphicsResource resource = resource_;
    ok = !CuFailed(cu_.cuGraphicsMapResources(1, &resource, nullptr), "cuGraphicsMapResources");
    if (ok) {
      CUarray array = nullptr;
      ok = !CuFailed(cu_.cuGraphicsSubResourceGetMappedArray(&array, resource, 0, 0),
                   "cuGraphicsSubResourceGetMappedArray");
      for (int p = 0; ok && p < 3; ++p) {
        CudaCopy2D copy = {};
        copy.srcMemoryType = kCuMemoryArray;
        copy.srcArray = array;
        copy.srcY = (size_t)p * (size_t)height_;
        copy.dstMemoryType = kCuMemoryDevice;
        copy.dstDevice = (CUdeviceptr)out->data[p];
        copy.dstPitch = (size_t)out->linesize[p];
        copy.WidthInBytes = (size_t)width_ * (sixteenBit_ ? 2 : 1);
        copy.Height = (size_t)height_;
        ok = !CuFailed(cu_.cuMemcpy2DAsync_v2(&copy, nullptr), "cuMemcpy2DAsync");
      }
      ok = !CuFailed(cu_.cuStreamSynchronize(nullptr), "cuStreamSynchronize") && ok;
      cu_.cuGraphicsUnmapResources(1, &resource, nullptr);
    }
    CUcontext popped = nullptr;
    cu_.cuCtxPopCurrent_v2(&popped);
  }
  GiveBack(slot);
  return ok;
}

}  // namespace

std::unique_ptr<GpuFrames> CreateGpuFrames(const Libav& av, uint64_t adapter, int width,
                                           int height, bool fullChroma, bool sixteenBit) {
  if (!adapter || width < 2 || height < 2 || (width | height) & 1) return nullptr;
  if (fullChroma) {
    auto frames = std::make_unique<CudaGpuFrames>(av);
    if (!frames->Create(adapter, width, height, sixteenBit)) return nullptr;
    return frames;
  }
  auto frames = std::make_unique<D3D11GpuFrames>(av);
  if (!frames->Create(adapter, width, height, sixteenBit)) return nullptr;
  return frames;
}

}  // namespace cap
