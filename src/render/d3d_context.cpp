#include "render/d3d_context.h"
#include "i18n.h"
#include "text_win32.h"

#include <algorithm>
#include <vector>

namespace cap {

std::string GraphicsAdapterSignature() {
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return {};

  // Deduplicated and sorted. DXGI happily reports the same card several times --
  // this machine lists one RTX 4080 four times -- and the count can change when
  // a monitor is plugged in. A signature that moves for that reason would throw
  // away a perfectly good encoder test.
  std::vector<std::string> names;
  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 desc = {};
    if (FAILED(adapter->GetDesc1(&desc))) continue;
    // Skip the software renderer: it is always there and says nothing about
    // what encoders exist.
    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
    names.push_back(ToUtf8(desc.Description));
  }
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());

  std::string result;
  for (const std::string& name : names) {
    if (!result.empty()) result += " + ";
    result += name;
  }
  return result;
}
namespace {

const DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
// Half float, linear, BT.709 primaries, 1.0 = 80 nits. Values above one are
// what makes it HDR; values below zero are legal too and hold colours outside
// BT.709.
const DXGI_FORMAT kHdrBackBufferFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

bool QueryTearingSupport(IDXGIFactory2* factory) {
  ComPtr<IDXGIFactory5> factory5;
  if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&factory5)))) return false;
  BOOL allow = FALSE;
  if (FAILED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow,
                                           sizeof(allow)))) {
    return false;
  }
  return allow != FALSE;
}

}  // namespace

D3DContext::~D3DContext() {
  Shutdown();
}

bool D3DContext::Initialize(HWND hwnd, std::string* error) {
  hwnd_ = hwnd;

  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
  flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                      D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_11_0;

  HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                   (UINT)std::size(levels), D3D11_SDK_VERSION, &device_, &obtained,
                                   &context_);
#ifdef _DEBUG
  if (FAILED(hr)) {
    // The debug layer is not installed on every machine.
    flags &= ~(UINT)D3D11_CREATE_DEVICE_DEBUG;
    hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                             (UINT)std::size(levels), D3D11_SDK_VERSION, &device_, &obtained,
                             &context_);
  }
#endif
  if (FAILED(hr)) {
    CAP_WARN("Hardware device not available (%s), trying WARP", HrToEnglish(hr).c_str());
    hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels,
                             (UINT)std::size(levels), D3D11_SDK_VERSION, &device_, &obtained,
                             &context_);
  }
  if (FAILED(hr)) {
    ReportError(error, CAP_SAID(T("Direct3D 11 konnte nicht initialisiert werden: ",
                                     "Direct3D 11 could not be initialised: ") + HrToString(hr)));
    return false;
  }

  ComPtr<IDXGIDevice1> dxgiDevice;
  if (FAILED(hr = device_.As(&dxgiDevice))) {
    ReportError(error, CAP_SAID(T("IDXGIDevice1 nicht verfügbar: ",
                                     "IDXGIDevice1 is not available: ") + HrToString(hr)));
    return false;
  }
  // One frame of latency: the CPU never runs more than a frame ahead of the GPU.
  dxgiDevice->SetMaximumFrameLatency(1);

  ComPtr<IDXGIAdapter> adapter;
  if (FAILED(hr = dxgiDevice->GetAdapter(&adapter))) {
    ReportError(error, CAP_SAID(T("DXGI-Adapter nicht verfügbar: ",
                                     "The DXGI adapter is not available: ") + HrToString(hr)));
    return false;
  }
  ComPtr<IDXGIFactory2> factory;
  if (FAILED(hr = adapter->GetParent(IID_PPV_ARGS(&factory)))) {
    ReportError(error, CAP_SAID(T("DXGI-Factory nicht verfügbar: ",
                                     "The DXGI factory is not available: ") + HrToString(hr)));
    return false;
  }

  tearingSupported_ = QueryTearingSupport(factory.Get());
  swapchainFlags_ = tearingSupported_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;

  RECT rc{};
  ::GetClientRect(hwnd_, &rc);
  width_ = std::max<int>(1, rc.right - rc.left);
  height_ = std::max<int>(1, rc.bottom - rc.top);

  DXGI_SWAP_CHAIN_DESC1 desc = {};
  desc.Width = (UINT)width_;
  desc.Height = (UINT)height_;
  desc.Format = hdrOutput_ ? kHdrBackBufferFormat : kBackBufferFormat;
  desc.Stereo = FALSE;
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  desc.Flags = swapchainFlags_;

  hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &desc, nullptr, nullptr, &swapchain_);
  if (FAILED(hr)) {
    ReportError(error, CAP_SAID(T("Swapchain konnte nicht erstellt werden: ",
                                     "The swapchain could not be created: ") + HrToString(hr)));
    return false;
  }

  // We handle fullscreen ourselves with a borderless window, so DXGI must keep
  // its hands off Alt+Enter and off resizing the window.
  factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);

  RefreshDisplayCapability();

  if (!CreateRenderTarget()) {
    ReportError(error, CAP_SAID(T("Rendertarget konnte nicht erstellt werden",
                                     "The render target could not be created")));
    return false;
  }

  CAP_LOG("D3D11 ready: feature level 0x%X, tearing %s", (unsigned)obtained,
          tearingSupported_ ? "supported" : "not supported");
  return true;
}

void D3DContext::Shutdown() {
  ReleaseRenderTarget();
  swapchain_.Reset();
  if (context_) {
    context_->ClearState();
    context_->Flush();
  }
  context_.Reset();
  device_.Reset();
  hwnd_ = nullptr;
}

bool D3DContext::CreateRenderTarget() {
  ComPtr<ID3D11Texture2D> backbuffer;
  if (FAILED(CAP_HR(swapchain_->GetBuffer(0, IID_PPV_ARGS(&backbuffer))))) return false;
  if (FAILED(CAP_HR(device_->CreateRenderTargetView(backbuffer.Get(), nullptr, &rtv_)))) {
    return false;
  }
  D3D11_TEXTURE2D_DESC desc = {};
  backbuffer->GetDesc(&desc);
  width_ = (int)desc.Width;
  height_ = (int)desc.Height;
  return true;
}

void D3DContext::ReleaseRenderTarget() {
  rtv_.Reset();
}

void D3DContext::Resize() {
  if (!swapchain_) return;

  RECT rc{};
  ::GetClientRect(hwnd_, &rc);
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;
  if (w <= 0 || h <= 0) return;             // minimised
  if (w == width_ && h == height_) return;  // nothing changed

  ReleaseRenderTarget();
  HRESULT hr = swapchain_->ResizeBuffers(0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, swapchainFlags_);
  if (FAILED(hr)) {
    CAP_ERR("ResizeBuffers failed: %s", HrToEnglish(hr).c_str());
  }
  CreateRenderTarget();
}

bool D3DContext::BeginFrame(const float clearColor[4]) {
  if (!rtv_ || width_ <= 0 || height_ <= 0) return false;

  if (occluded_) {
    // Cheap test present; nothing is drawn while the window is fully covered.
    if (swapchain_->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) return false;
    occluded_ = false;
  }

  ID3D11RenderTargetView* targets[] = {rtv_.Get()};
  context_->OMSetRenderTargets(1, targets, nullptr);
  context_->ClearRenderTargetView(rtv_.Get(), clearColor);

  D3D11_VIEWPORT vp = {};
  vp.Width = (float)width_;
  vp.Height = (float)height_;
  vp.MaxDepth = 1.0f;
  context_->RSSetViewports(1, &vp);
  return true;
}

bool D3DContext::GrabBackBuffer(std::vector<uint8_t>* rgba, int* width, int* height) {
  if (!rgba || !swapchain_ || !context_ || !device_) return false;
  if (hdrOutput_) return false;  // scRGB-Float, siehe Kommentar in der Kopfdatei
  if (width_ <= 0 || height_ <= 0) return false;

  ComPtr<ID3D11Texture2D> back;
  if (FAILED(CAP_HR(swapchain_->GetBuffer(0, IID_PPV_ARGS(&back)))) || !back) return false;

  D3D11_TEXTURE2D_DESC td = {};
  back->GetDesc(&td);
  td.Usage = D3D11_USAGE_STAGING;
  td.BindFlags = 0;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  td.MiscFlags = 0;

  ComPtr<ID3D11Texture2D> staging;
  if (FAILED(CAP_HR(device_->CreateTexture2D(&td, nullptr, &staging)))) return false;
  context_->CopyResource(staging.Get(), back.Get());

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (FAILED(CAP_HR(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))) return false;

  const int w = (int)td.Width;
  const int h = (int)td.Height;
  const size_t rowBytes = (size_t)w * 4;
  rgba->resize(rowBytes * (size_t)h);
  const uint8_t* src = (const uint8_t*)mapped.pData;
  for (int y = 0; y < h; ++y) {
    const uint8_t* s = src + (size_t)y * (size_t)mapped.RowPitch;
    uint8_t* d = rgba->data() + (size_t)y * rowBytes;
    // Der Rueckpuffer ist BGRA, die Speicherroutinen erwarten RGBA. Und das
    // Alpha wird gesetzt statt uebernommen: was auf dem Bildschirm deckend
    // aussieht, muss in der Datei deckend sein.
    for (int x = 0; x < w; ++x) {
      d[x * 4 + 0] = s[x * 4 + 2];
      d[x * 4 + 1] = s[x * 4 + 1];
      d[x * 4 + 2] = s[x * 4 + 0];
      d[x * 4 + 3] = 0xFF;
    }
  }
  context_->Unmap(staging.Get(), 0);

  if (width) *width = w;
  if (height) *height = h;
  return true;
}

void D3DContext::EndFrame(bool vsync) {
  if (!swapchain_) return;

  UINT interval = vsync ? 1u : 0u;
  UINT flags = (!vsync && tearingSupported_) ? DXGI_PRESENT_ALLOW_TEARING : 0u;

  HRESULT hr = swapchain_->Present(interval, flags);
  if (hr == DXGI_STATUS_OCCLUDED) {
    occluded_ = true;
  } else if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
    CAP_ERR("Graphics device lost: %s",
            HrToEnglish(device_ ? device_->GetDeviceRemovedReason() : hr).c_str());
  }
}


void D3DContext::RefreshDisplayCapability() {
  display_ = DisplayCapability();
  if (!swapchain_) return;

  // The output the window mostly sits on. Asking the swapchain rather than
  // enumerating outputs is what makes this follow the window across screens.
  ComPtr<IDXGIOutput> output;
  if (FAILED(swapchain_->GetContainingOutput(&output)) || !output) return;

  ComPtr<IDXGIOutput6> output6;
  if (FAILED(output.As(&output6)) || !output6) return;

  DXGI_OUTPUT_DESC1 desc = {};
  if (FAILED(output6->GetDesc1(&desc))) return;

  display_.hdr = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
  display_.peakNits = desc.MaxLuminance > 0.0f ? desc.MaxLuminance : 100.0f;
  display_.minNits = desc.MinLuminance;
}

bool D3DContext::SetHdrOutput(bool enabled, std::string* error) {
  if (enabled == hdrOutput_) return true;
  if (!swapchain_ || !device_) return false;

  // Everything pointing at the old buffers has to let go first; ResizeBuffers
  // refuses while a view is outstanding.
  rtv_.Reset();
  context_->OMSetRenderTargets(0, nullptr, nullptr);
  context_->Flush();

  const DXGI_FORMAT format = enabled ? kHdrBackBufferFormat : kBackBufferFormat;
  HRESULT hr = swapchain_->ResizeBuffers(0, (UINT)width_, (UINT)height_, format, swapchainFlags_);
  if (FAILED(hr)) {
    ReportError(error, CAP_SAID(T("Die Anzeige ließ sich nicht auf HDR umstellen: ",
                                  "The display could not be switched to HDR: ") + HrToString(hr)));
    swapchain_->ResizeBuffers(0, (UINT)width_, (UINT)height_,
                              hdrOutput_ ? kHdrBackBufferFormat : kBackBufferFormat,
                              swapchainFlags_);
    CreateRenderTarget();
    return false;
  }

  // Telling Windows what the numbers in that buffer mean. Without this the
  // compositor treats them as ordinary sRGB and the picture comes out wrong in
  // a way that looks like a bug in the tone mapping.
  ComPtr<IDXGISwapChain3> swapchain3;
  if (SUCCEEDED(swapchain_.As(&swapchain3)) && swapchain3) {
    const DXGI_COLOR_SPACE_TYPE space = enabled
        ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709      // scRGB
        : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;     // ordinary sRGB
    UINT support = 0;
    if (SUCCEEDED(swapchain3->CheckColorSpaceSupport(space, &support)) &&
        (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
      swapchain3->SetColorSpace1(space);
    } else if (enabled) {
      ReportError(error, CAP_SAID(T("Diese Anzeige nimmt kein scRGB entgegen.",
                                    "This display will not take scRGB.")));
      swapchain_->ResizeBuffers(0, (UINT)width_, (UINT)height_, kBackBufferFormat,
                                swapchainFlags_);
      CreateRenderTarget();
      return false;
    }
  }

  hdrOutput_ = enabled;
  CreateRenderTarget();
  CAP_LOG("Display switched to %s", enabled ? "scRGB (HDR)" : "sRGB");
  return true;
}

void D3DContext::SetFrameLatency(UINT frames) {
  if (frames == frameLatency_ || !device_) return;
  ComPtr<IDXGIDevice1> dxgiDevice;
  if (FAILED(device_.As(&dxgiDevice)) || !dxgiDevice) return;
  if (SUCCEEDED(dxgiDevice->SetMaximumFrameLatency(frames))) {
    frameLatency_ = frames;
    CAP_LOG("Frame queue set to %u", frames);
  }
}

}  // namespace cap
