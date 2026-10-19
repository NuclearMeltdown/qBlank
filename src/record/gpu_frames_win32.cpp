#include "record/gpu_frames.h"

#include <d3d11_4.h>
#include <dxgi1_2.h>

#include <cstring>
#include <vector>

#include "common_win32.h"
#include "inflate.h"
#include "record/libav.h"
#include "render/shared_frames.h"

extern "C" {
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
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

class D3D11GpuFrames : public GpuFrames {
 public:
  D3D11GpuFrames(const Libav& av) : av_(av) {}
  ~D3D11GpuFrames() override {
    targets_.clear();
    av_.av_buffer_unref(&frames_);
    av_.av_buffer_unref(&device_);
  }

  bool Create(uint64_t adapter, int width, int height, bool sixteenBit);

  std::shared_ptr<SharedFrames> frames() const override { return shared_; }
  AVBufferRef* framesContext() const override { return frames_; }
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

  const Libav& av_;
  ComPtr<ID3D11Device> dev_;
  ComPtr<ID3D11DeviceContext> dc_;
  AVBufferRef* device_ = nullptr;
  AVBufferRef* frames_ = nullptr;
  AVD3D11VADeviceContext* hw_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  bool sixteenBit_ = false;

  std::shared_ptr<OwnedFrames> shared_;
  ComPtr<ID3D11Texture2D> tex_[SharedFrames::kSlots];
  ComPtr<IDXGIKeyedMutex> mutex_[SharedFrames::kSlots];
  ComPtr<ID3D11ShaderResourceView> srv_[SharedFrames::kSlots];

  ComPtr<ID3D11VertexShader> vs_;
  ComPtr<ID3D11PixelShader> ps_;
  ComPtr<ID3D11Buffer> cbLuma_;
  ComPtr<ID3D11Buffer> cbChroma_;
  ComPtr<ID3D11RasterizerState> raster_;
  std::vector<Targets> targets_;
};

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
  width_ = width;
  height_ = height;
  sixteenBit_ = sixteenBit;

  // The device, on the renderer's adapter: a texture is shared within one.
  ComPtr<IDXGIFactory1> factory;
  if (Failed(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1")) return false;
  ComPtr<IDXGIAdapter1> found;
  ComPtr<IDXGIAdapter1> a;
  for (UINT i = 0; !found && factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 desc = {};
    uint64_t luid = 0;
    if (SUCCEEDED(a->GetDesc1(&desc))) memcpy(&luid, &desc.AdapterLuid, sizeof(luid));
    if (luid == adapter) found = a;
  }
  if (!found) {
    CAP_WARN("Recording on the GPU: the renderer's adapter is not there");
    return false;
  }
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  if (Failed(D3D11CreateDevice(found.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                               D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, 2, D3D11_SDK_VERSION,
                               &dev_, nullptr, &dc_),
             "D3D11CreateDevice")) {
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

  // The three textures the yuv pass draws into.
  const int rows = height + height / 2;
  const DXGI_FORMAT format = sixteenBit ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R8_UINT;
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)width;
  td.Height = (UINT)rows;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = format;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
  D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
  sd.Format = format;
  sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  sd.Texture2D.MipLevels = 1;
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
  // Key 1 once the yuv pass has drawn it. That is released before the slot is
  // handed over, so this hardly ever waits; a renderer that went away does not
  // hold the recording up for more than a second.
  IDXGIKeyedMutex* mutex = mutex_[slot].Get();
  HRESULT hr = (HRESULT)WAIT_TIMEOUT;
  for (int i = 0; i < 10 && hr == (HRESULT)WAIT_TIMEOUT &&
                  keepWaiting.load(std::memory_order_relaxed);
       ++i) {
    hr = mutex->AcquireSync(1, 100);
  }
  if (hr != S_OK) {
    if (hr != (HRESULT)WAIT_TIMEOUT) CAP_WARN("Recording on the GPU: AcquireSync gave 0x%08lx", hr);
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
    dc->Flush();
    hw_->unlock(hw_->lock_ctx);
  }

  mutex->ReleaseSync(0);
  shared_->key[slot] = 0;
  return t != nullptr;
}

}  // namespace

std::unique_ptr<GpuFrames> CreateGpuFrames(const Libav& av, uint64_t adapter, int width,
                                           int height, bool sixteenBit) {
  if (!adapter || width < 2 || height < 2 || (width | height) & 1) return nullptr;
  auto frames = std::make_unique<D3D11GpuFrames>(av);
  if (!frames->Create(adapter, width, height, sixteenBit)) return nullptr;
  return frames;
}

}  // namespace cap
