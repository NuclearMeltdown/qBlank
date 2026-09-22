#include "ui/ui_surface.h"

#include <d3d11.h>
#include <dxgi1_2.h>

#include "backends/imgui_impl_dx11.h"
#include "common_win32.h"
#include "i18n.h"
#include "imgui.h"
#include "window_win32.h"

namespace cap {

struct UiSurface::Impl {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> ctx;
  ComPtr<IDXGISwapChain1> swapchain;
  ComPtr<ID3D11RenderTargetView> rtv;
  int width = 0;
  int height = 0;
  bool occluded = false;
  // The flags have to match in three places -- creation, resize and present --
  // or the resize fails and the present blocks anyway.
  UINT swapchainFlags = 0;
  UINT presentFlags = 0;

  bool CreateRenderTarget();
};

UiSurface::UiSurface() : impl_(new Impl()) {}

UiSurface::~UiSurface() {
  ReleaseSwapchain();
  ReleaseDevice();
}

bool UiSurface::CreateDevice(std::string* error) {
  Impl& s = *impl_;
  if (s.device) return true;

  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                      D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
  HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                   (UINT)(sizeof(levels) / sizeof(levels[0])), D3D11_SDK_VERSION,
                                   &s.device, &got, &s.ctx);
  if (FAILED(hr)) {
    // Ohne BGRA nochmal: aeltere Treiber melden das Flag nicht, brauchen es
    // hier aber auch nicht.
    hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
                             (UINT)(sizeof(levels) / sizeof(levels[0])), D3D11_SDK_VERSION,
                             &s.device, &got, &s.ctx);
  }
  if (FAILED(CAP_HR(hr)) || !s.device || !s.ctx) {
    return ReportError(error,
                       CAP_SAID(T("Eigenes Grafikgerät für das Einstellungsfenster fehlgeschlagen",
                                  "Could not create a graphics device for the settings window")));
  }
  // Der Dialog darf ruhig eine Warteschlange haben: er will fluessig dem
  // Mauszeiger folgen, nicht in einer Millisekunde auf dem Schirm sein.
  ComPtr<IDXGIDevice1> own;
  if (SUCCEEDED(s.device.As(&own)) && own) own->SetMaximumFrameLatency(3);
  return true;
}

bool UiSurface::Attach(const Window& window, bool allowTearing, std::string* error) {
  Impl& s = *impl_;
  if (!s.device) {
    return ReportError(error, CAP_SAID(T("Kein Grafikgerät für das Einstellungsfenster",
                                         "The settings window has no graphics device")));
  }

  ComPtr<IDXGIDevice> dxgiDevice;
  ComPtr<IDXGIAdapter> adapter;
  ComPtr<IDXGIFactory2> factory;
  if (FAILED(CAP_HR(s.device.As(&dxgiDevice))) ||
      FAILED(CAP_HR(dxgiDevice->GetAdapter(&adapter))) ||
      FAILED(CAP_HR(adapter->GetParent(IID_PPV_ARGS(&factory))))) {
    return ReportError(error, CAP_SAID(T("DXGI-Factory für das Einstellungsfenster fehlt",
                                         "The settings window has no DXGI factory")));
  }

  DXGI_SWAP_CHAIN_DESC1 desc = {};
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  // Three, not two. The device asks for a short queue, and with only two
  // buffers a present that arrives before the previous one has been retired
  // waits for it. The third buffer is a few megabytes against several
  // milliseconds a frame.
  desc.BufferCount = 3;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  // Measured before this was here: the present cost 12 to 17 milliseconds on
  // average and up to 41 at worst, roughly thirty-five times a second. A flip
  // model swapchain with a sync interval of zero and *no* tearing flag is not
  // "not vsynced" -- it still hands the frame to the compositor at a vertical
  // blank, and with a short frame latency the call blocks until the previous
  // one has been retired. The thread that owns both windows was therefore
  // parked for better than half of every second, not pumping messages, which
  // is why the *desktop's* cursor stuttered and not only the preview.
  s.swapchainFlags = allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;
  s.presentFlags = allowTearing ? DXGI_PRESENT_ALLOW_TEARING : 0u;
  desc.Flags = s.swapchainFlags;
  if (FAILED(CAP_HR(factory->CreateSwapChainForHwnd(s.device.Get(), NativeWindow(window), &desc,
                                                    nullptr, nullptr, &s.swapchain)))) {
    return ReportError(error, CAP_SAID(T("Swapchain für das Einstellungsfenster fehlgeschlagen",
                                         "The settings window swapchain failed")));
  }
  // Deliberately not calling MakeWindowAssociation here. It is a property of the
  // *factory*, not of a window: calling it again would replace the association
  // the main window made, and with it the NO_WINDOW_CHANGES that stops DXGI
  // resizing the preview behind our back. Leaving it alone means Alt+Enter keeps
  // meaning the preview, which is what it should mean anyway.

  s.CreateRenderTarget();
  return true;
}

bool UiSurface::InitUi() {
  Impl& s = *impl_;
  return s.device && s.ctx && ImGui_ImplDX11_Init(s.device.Get(), s.ctx.Get());
}

void UiSurface::ShutdownUi() {
  // Guarded, because the window's creation can fail between the two backends:
  // the platform one is initialised first and this one only if it succeeded,
  // and the failure path comes straight here. Shutting down a backend that was
  // never started walks a null.
  if (ImGui::GetIO().BackendRendererUserData) ImGui_ImplDX11_Shutdown();
}

bool UiSurface::Impl::CreateRenderTarget() {
  if (!swapchain) return false;
  ComPtr<ID3D11Texture2D> back;
  if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
  if (FAILED(CAP_HR(device->CreateRenderTargetView(back.Get(), nullptr, &rtv)))) return false;

  D3D11_TEXTURE2D_DESC td = {};
  back->GetDesc(&td);
  width = (int)td.Width;
  height = (int)td.Height;
  return true;
}

void UiSurface::ReleaseSwapchain() {
  Impl& s = *impl_;
  s.rtv.Reset();
  s.swapchain.Reset();
}

void UiSurface::ReleaseDevice() {
  Impl& s = *impl_;
  s.device.Reset();
  s.ctx.Reset();
}

void UiSurface::Resize() {
  Impl& s = *impl_;
  if (!s.swapchain) return;
  s.rtv.Reset();
  s.swapchain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, s.swapchainFlags);
  s.CreateRenderTarget();
}

bool UiSurface::hasTarget() const { return impl_->rtv != nullptr; }

bool UiSurface::StillOccluded() {
  Impl& s = *impl_;
  if (!s.occluded) return false;
  if (!s.swapchain) return true;
  if (s.swapchain->Present(0, DXGI_PRESENT_TEST) != S_OK) return true;
  s.occluded = false;
  return false;
}

void UiSurface::NewUiFrame() { ImGui_ImplDX11_NewFrame(); }

void UiSurface::Present(const float clear[4]) {
  Impl& s = *impl_;
  if (!s.ctx || !s.rtv) return;

  ID3D11RenderTargetView* rtvs[] = {s.rtv.Get()};
  s.ctx->OMSetRenderTargets(1, rtvs, nullptr);
  s.ctx->ClearRenderTargetView(s.rtv.Get(), clear);

  D3D11_VIEWPORT vp = {};
  vp.Width = (float)s.width;
  vp.Height = (float)s.height;
  vp.MaxDepth = 1.0f;
  s.ctx->RSSetViewports(1, &vp);

  ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
  // Sixty a second, after the preview has already gone to the screen. Both
  // halves of that matter: this used to run in the middle of the preview's own
  // frame, where presenting a second swapchain flushes everything queued for
  // the first one.
  //
  // The result is kept rather than discarded: a fully covered window gets the
  // compositor's occluded-present throttle, and every present then takes far
  // longer than it looks like it should. The main window handles that the same
  // way.
  const HRESULT hr = s.swapchain->Present(0, s.presentFlags);
  s.occluded = hr == DXGI_STATUS_OCCLUDED;

  // And unbind. Defensive now rather than necessary -- the preview sets its own
  // targets at the start of every pass -- but leaving a presented back buffer
  // bound is the sort of thing that only shows up later, in something
  // unrelated.
  ID3D11RenderTargetView* none[] = {nullptr};
  s.ctx->OMSetRenderTargets(1, none, nullptr);
}

int UiSurface::width() const { return impl_->width; }
int UiSurface::height() const { return impl_->height; }

}  // namespace cap
