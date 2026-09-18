#include "ui/settings_host.h"

#include "resource.h"

#include <dwmapi.h>

#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "imgui.h"
#include "ui/theme.h"
#include "i18n.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace cap {
namespace {

const std::wstring kClassName = WindowClassName(L"SettingsWindow");

}  // namespace

SettingsHost::~SettingsHost() { Destroy(); }

LRESULT CALLBACK SettingsHost::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  SettingsHost* self = reinterpret_cast<SettingsHost*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
  }
  if (!self || !self->imgui_) return ::DefWindowProcW(hwnd, msg, wParam, lParam);

  // Input has to be fed to *this* window's context, not the main one, so the
  // current context is swapped for the duration of the handler.
  ImGuiContext* previous = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(self->imgui_);
  const bool handled = ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam) != 0;
  // Not WantCaptureKeyboard: with keyboard navigation on, that is true whenever
  // this window has focus at all, which is exactly when the shortcuts should
  // work. What the dialog really needs the keys for is typing and open lists.
  const bool busy = ImGui::GetIO().WantTextInput ||
                    ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId |
                                               ImGuiPopupFlags_AnyPopupLevel);
  ImGui::SetCurrentContext(previous);
  if (handled) return 1;

  switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
      if (self->onKey_ && self->onKey_(wParam, lParam, busy)) return 0;
      break;
    case WM_CLOSE:
      self->closeRequested_ = true;
      self->Hide();
      return 0;
    case WM_SIZE:
      if (wParam != SIZE_MINIMIZED) self->Resize();
      return 0;
    case WM_ENTERSIZEMOVE:
      // A timer as well, but only as a floor for when the mouse is held still:
      // WM_TIMER is the lowest priority message there is and Windows only
      // generates one when the queue is otherwise empty. During a drag the queue
      // never is. Measured, at an interval of 8 ms it fired seven times in a
      // second, and the preview stood still for over a second at a stretch.
      //
      // 10 ms, weil das die kleinste Zahl ist, die Windows fuer einen Timer
      // ueberhaupt annimmt (USER_TIMER_MINIMUM). Ein Weckruf ohne neues Bild
      // kostet seit dem Wegfall der Zeitschranke nichts mehr als die Frage, ob
      // eines da ist -- also darf oefter gefragt werden, als 60 Hz brauchen.
      ::SetTimer(hwnd, 1, 10, nullptr);
      return 0;
    case WM_EXITSIZEMOVE:
      ::KillTimer(hwnd, 1);
      return 0;
    case WM_MOVING:
    case WM_SIZING:
      // This is the hook that actually works. Windows sends these continuously
      // while the window is being dragged or resized -- once per mouse movement
      // -- and unlike WM_TIMER they are real messages that cannot be starved by
      // the flood of mouse input that causes the problem in the first place.
      self->PumpModalFrame();
      break;  // and on to DefWindowProc, which does the actual moving
    case WM_TIMER:
      if (wParam == 1) self->PumpModalFrame();
      return 0;
    case WM_DESTROY:
      self->hwnd_ = nullptr;
      return 0;
    default:
      break;
  }
  return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

// One frame from inside the modal loop.
//
// Dragging a window puts Windows into a loop of its own that does not return
// until the mouse is released, so qBlank's own loop stops running and the
// preview stops with it. The only way back in is from a message this window
// receives while that loop is running.
// Kein Takt mehr an dieser Stelle -- und das ist der dritte Anlauf.
//
// Erst stand hier fest 30 ms. Sichtbar: die Vorschau fiel beim Ziehen auf 33
// Bilder, nicht weil etwas ueberlastet war, sondern weil es hier so beschlossen
// wurde. Dann stand hier die gemessene Bildrate der Quelle, und das war
// schlechter: bei PAL sind das 25 Bilder, waehrend die Vorschau nach dem
// Deinterlacing 50 Halbbilder zeigt -- die Haelfte davon fiel weg. Dazu kommt,
// dass ::GetTickCount alle 15,6 ms weiterzaehlt, also jede Schranke auf das
// naechste Vielfache davon aufrundet: 40 ms werden zu 46,8 und damit 21 Bilder
// in der Sekunde. Genau das war zu sehen.
//
// Eine Zahl ist hier immer falsch, egal welche. Ob es etwas Neues zu zeigen
// gibt, weiss nur die Schleife oben, und die entscheidet es fuer den Normalfall
// laengst -- neues Bild, faelliges zweites Halbbild, oder ein langsamer Boden.
// Also wird hier nichts mehr entschieden: der Rueckruf wird angeboten, und was
// daran haengt, prueft dieselbe Bedingung wie sonst auch.
void SettingsHost::PumpModalFrame() {
  if (!onFrame_ || inFrameCallback_) return;
  inFrameCallback_ = true;
  onFrame_();
  inFrameCallback_ = false;
}

bool SettingsHost::Create(HINSTANCE instance, ID3D11Device* device, ID3D11DeviceContext* context,
                          ImFontAtlas* atlas, float uiScale, bool allowTearing,
                          const Placement& where, std::string* error) {
  if (hwnd_) return true;
  // Ein eigenes Direct3D-Geraet, nicht das der Vorschau.
  //
  // Zwei Swapchains auf einem Geraet teilen sich zwangslaeufig zwei Dinge, und
  // beide waren teuer. Erstens gilt SetMaximumFrameLatency fuer das *Geraet*:
  // mit der kurzen Warteschlange, von der die Vorschau lebt, wartete jedes
  // Present auf das Bild der jeweils anderen. Zweitens teilen sie den Immediate
  // Context -- jeder Zeichenbefehl des Dialogs laeuft dann durch genau den
  // Strang, den die Vorschau braucht, und serialisiert sich dagegen.
  //
  // Ein zweites Geraet loest beides an der Wurzel statt es auszubalancieren.
  // Es kostet eine eigene Schriftatlas-Textur und etwas Speicher; dafuer darf
  // die Vorschau ihre Warteschlange dauerhaft auf eins lassen, was der groesste
  // einzelne Hebel auf ihre Verzoegerung ist.
  (void)device;
  (void)context;
  {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                     (UINT)(sizeof(levels) / sizeof(levels[0])), D3D11_SDK_VERSION,
                                     &device_, &got, &ctx_);
    if (FAILED(hr)) {
      // Ohne BGRA nochmal: aeltere Treiber melden das Flag nicht, brauchen es
      // hier aber auch nicht.
      hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
                               (UINT)(sizeof(levels) / sizeof(levels[0])), D3D11_SDK_VERSION,
                               &device_, &got, &ctx_);
    }
    if (FAILED(CAP_HR(hr)) || !device_ || !ctx_) {
      ReportError(error, CAP_SAID(T("Eigenes Grafikgerät für das Einstellungsfenster fehlgeschlagen",
                                    "Could not create a graphics device for the settings window")));
      return false;
    }
    // Der Dialog darf ruhig eine Warteschlange haben: er will fluessig dem
    // Mauszeiger folgen, nicht in einer Millisekunde auf dem Schirm sein.
    ComPtr<IDXGIDevice1> own;
    if (SUCCEEDED(device_.As(&own)) && own) own->SetMaximumFrameLatency(3);
  }
  uiScale_ = uiScale > 0.1f ? uiScale : 1.0f;

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = &SettingsHost::WndProc;
  wc.hInstance = instance;
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  // The same icon as the preview. Without it the taskbar button this window now
  // has -- and its Alt+Tab entry -- would show the generic placeholder.
  wc.hIcon = (HICON)::LoadImageW(instance, MAKEINTRESOURCEW(IDI_QBLANK), IMAGE_ICON,
                                 ::GetSystemMetrics(SM_CXICON), ::GetSystemMetrics(SM_CYICON),
                                 LR_DEFAULTCOLOR);
  wc.hIconSm = (HICON)::LoadImageW(instance, MAKEINTRESOURCEW(IDI_QBLANK), IMAGE_ICON,
                                   ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON),
                                   LR_DEFAULTCOLOR);
  wc.lpszClassName = kClassName.c_str();
  // Registering twice is not an error worth failing over; the second call just
  // tells us it is already there.
  ::RegisterClassExW(&wc);

  // What it was last time, or a sensible default the first time. Checked
  // against the virtual screen, so a window remembered on a monitor that is no
  // longer there does not come up somewhere nobody can reach it.
  int w = where.width > 200 ? where.width : (int)(720 * uiScale_);
  int h = where.height > 200 ? where.height : (int)(640 * uiScale_);
  int x = CW_USEDEFAULT;
  int y = CW_USEDEFAULT;
  if (where.x != -1 || where.y != -1) {
    RECT desk = {::GetSystemMetrics(SM_XVIRTUALSCREEN), ::GetSystemMetrics(SM_YVIRTUALSCREEN), 0,
                 0};
    desk.right = desk.left + ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    desk.bottom = desk.top + ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
    // A hundred pixels of title bar has to remain reachable.
    if (where.x + 100 > desk.left && where.x < desk.right - 100 && where.y >= desk.top &&
        where.y < desk.bottom - 40) {
      x = where.x;
      y = where.y;
    }
  }
  // WS_EX_APPWINDOW, so it has a taskbar button and somewhere to go when
  // minimised -- without one it lands as a stub in the bottom left corner of the
  // screen.
  //
  // No owner. An owned window is lifted above its owner every time the owner is
  // activated, so a click into the preview dragged the settings in front of it
  // as well, from wherever they had been left. Unowned, the two stack like any
  // other two windows. What the owner did besides -- close with the preview,
  // stay above it while it is topmost -- App does by hand.
  hwnd_ = ::CreateWindowExW(WS_EX_APPWINDOW, kClassName.c_str(), kAppName, WS_OVERLAPPEDWINDOW,
                            x, y, w, h, nullptr, nullptr, instance, this);
  if (!hwnd_) {
    ReportError(error, CAP_SAID(T("Einstellungsfenster konnte nicht erstellt werden",
                                     "The settings window could not be created")));
    return false;
  }

  ComPtr<IDXGIDevice> dxgiDevice;
  ComPtr<IDXGIAdapter> adapter;
  ComPtr<IDXGIFactory2> factory;
  if (FAILED(CAP_HR(device_.As(&dxgiDevice))) ||
      FAILED(CAP_HR(dxgiDevice->GetAdapter(&adapter))) ||
      FAILED(CAP_HR(adapter->GetParent(IID_PPV_ARGS(&factory))))) {
    ReportError(error, CAP_SAID(T("DXGI-Factory für das Einstellungsfenster fehlt",
                                     "The settings window has no DXGI factory")));
    Destroy();
    return false;
  }

  DXGI_SWAP_CHAIN_DESC1 desc = {};
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  // Three, not two. The device asks for one frame of latency
  // (SetMaximumFrameLatency in D3DContext, which is device-wide and binds this
  // chain too), and with only two buffers a present that arrives before the
  // previous one has been retired waits for it. The third buffer is a few
  // megabytes against several milliseconds a frame.
  desc.BufferCount = 3;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  // Measured before this was here: the present below cost 12 to 17 milliseconds
  // on average and up to 41 at worst, roughly thirty-five times a second. A
  // flip model swapchain with a sync interval of zero and *no* tearing flag is
  // not "not vsynced" -- it still hands the frame to the compositor at a
  // vertical blank, and with the device-wide SetMaximumFrameLatency(1) the call
  // blocks until the previous one has been retired. The thread that owns both
  // windows was therefore parked for better than half of every second, not
  // pumping messages, which is why the *desktop's* cursor stuttered and not
  // only the preview.
  //
  // The flags have to match in three places -- creation, resize and present --
  // or the resize fails and the present blocks anyway.
  swapchainFlags_ = allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;
  presentFlags_ = allowTearing ? DXGI_PRESENT_ALLOW_TEARING : 0u;
  desc.Flags = swapchainFlags_;
  if (FAILED(CAP_HR(factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &desc, nullptr, nullptr,
                                                    &swapchain_)))) {
    ReportError(error, CAP_SAID(T("Swapchain für das Einstellungsfenster fehlgeschlagen",
                                     "The settings window swapchain failed")));
    Destroy();
    return false;
  }
  // Deliberately not calling MakeWindowAssociation here. It is a property of the
  // *factory*, not of a window: calling it again would replace the association
  // the main window made, and with it the NO_WINDOW_CHANGES that stops DXGI
  // resizing the preview behind our back. Leaving it alone means Alt+Enter keeps
  // meaning the preview, which is what it should mean anyway.

  // A context of its own, sharing the main one's fonts: the glyphs are the same
  // and one atlas on the GPU is enough for both.
  ImGuiContext* previous = ImGui::GetCurrentContext();
  // Eigener Atlas, weil eine Textur nicht ueber zwei Geraete hinweg gilt. Das
  // nimmt dem Ganzen zugleich die Falle, die der geteilte Atlas mitbrachte: der
  // DX11-Rueckenteil legt die Textur-Id *im Atlas* ab, also riss das Schliessen
  // dieses Fensters dem Hauptfenster die Schrift unter den Fuessen weg.
  (void)atlas;
  imgui_ = ImGui::CreateContext();
  ImGui::SetCurrentContext(imgui_);
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  // Dieselbe Schrift wie im Hauptfenster, und in derselben Groesse. Der eigene
  // Atlas faengt sonst mit ImGuis eingebauter an -- dann sieht ein Fenster des
  // Programms aus wie aus einem anderen Programm.
  //
  // Muss nach SetCurrentContext stehen: LoadUiFont haengt die Schrift in den
  // Atlas des gerade aktuellen Kontexts.
  LoadUiFont(17.0f * uiScale_);

  const bool ok =
      ImGui_ImplWin32_Init(hwnd_) && ImGui_ImplDX11_Init(device_.Get(), ctx_.Get());
  ImGui::SetCurrentContext(previous);
  if (!ok) {
    ReportError(error, CAP_SAID(T("ImGui für das Einstellungsfenster fehlgeschlagen",
                                     "ImGui failed for the settings window")));
    Destroy();
    return false;
  }

  CreateRenderTarget();
  CAP_LOG("Settings window created");
  return true;
}

void SettingsHost::Destroy() {
  if (imgui_) {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    // If this context is the current one, it is about to stop existing -- so
    // there is nothing to go back to and nothing to repair.
    if (previous == imgui_) previous = nullptr;
    ImGui::SetCurrentContext(imgui_);
    // Guarded, because Create can fail between the two backends: the platform
    // one is initialised first and the renderer one only if it succeeded, and
    // the failure path comes straight here. Shutting down a backend that was
    // never started walks a null.
    if (ImGui::GetIO().BackendRendererUserData) ImGui_ImplDX11_Shutdown();
    if (ImGui::GetIO().BackendPlatformUserData) ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(imgui_);
    imgui_ = nullptr;
    ImGui::SetCurrentContext(previous == nullptr ? nullptr : previous);

  }
  ReleaseRenderTarget();
  swapchain_.Reset();
  if (hwnd_) {
    HWND h = hwnd_;
    hwnd_ = nullptr;
    ::DestroyWindow(h);
  }
  device_.Reset();
  ctx_.Reset();
  visible_ = false;
}

bool SettingsHost::CreateRenderTarget() {
  if (!swapchain_) return false;
  ComPtr<ID3D11Texture2D> back;
  if (FAILED(swapchain_->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
  if (FAILED(CAP_HR(device_->CreateRenderTargetView(back.Get(), nullptr, &rtv_)))) return false;

  D3D11_TEXTURE2D_DESC td = {};
  back->GetDesc(&td);
  width_ = (int)td.Width;
  height_ = (int)td.Height;
  return true;
}

void SettingsHost::ReleaseRenderTarget() { rtv_.Reset(); }

void SettingsHost::Resize() {
  if (!swapchain_) return;
  ReleaseRenderTarget();
  swapchain_->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, swapchainFlags_);
  CreateRenderTarget();
}

void SettingsHost::Show(const std::wstring& title) {
  if (!hwnd_) return;
  ::SetWindowTextW(hwnd_, title.c_str());
  // SW_SHOW displays a minimised window *still minimised*, and BeginFrame then
  // refuses to draw it -- so reopening the settings after minimising them did
  // nothing at all. Only reachable since this window gained a taskbar button
  // and could be minimised properly in the first place.
  ::ShowWindow(hwnd_, ::IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
  ::SetForegroundWindow(hwnd_);
  visible_ = true;
  closeRequested_ = false;
}

void SettingsHost::Hide() {
  if (!hwnd_) return;
  ::ShowWindow(hwnd_, SW_HIDE);
  visible_ = false;
}

void SettingsHost::Raise() {
  if (!hwnd_ || !visible_) return;
  if (::IsIconic(hwnd_)) ::ShowWindow(hwnd_, SW_RESTORE);
  ::SetForegroundWindow(hwnd_);
}

bool SettingsHost::RaiseIfCoveredBy(HWND other) {
  if (!hwnd_ || !visible_) return false;
  if (::IsIconic(hwnd_)) {
    Raise();
    return true;
  }
  RECT mine = {}, theirs = {}, overlap = {};
  if (!other || !::IsWindowVisible(other) || ::IsIconic(other) ||
      !::GetWindowRect(hwnd_, &mine) || !::GetWindowRect(other, &theirs) ||
      !::IntersectRect(&overlap, &mine, &theirs)) {
    return false;
  }
  // Overlapping is not covering: `other` has to be above this window as well.
  // Everything before this window in the z-order is above it.
  for (HWND above = ::GetWindow(hwnd_, GW_HWNDPREV); above;
       above = ::GetWindow(above, GW_HWNDPREV)) {
    if (above == other) {
      Raise();
      return true;
    }
  }
  return false;
}

void SettingsHost::SetTopmost(bool top) {
  if (!hwnd_) return;
  // Only when it changes. HWND_TOPMOST also moves the window to the front of
  // the topmost windows, and doing that every time would put the settings back
  // over a preview that had just been clicked into.
  const bool now = (::GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
  if (now == top) return;
  ::SetWindowPos(hwnd_, top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

bool SettingsHost::takeCloseRequest() {
  const bool requested = closeRequested_;
  closeRequested_ = false;
  return requested;
}

void SettingsHost::ApplyTheme(bool darkMode, unsigned accentColor) {
  if (!imgui_) return;
  ImGuiContext* previous = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(imgui_);
  ApplyImGuiTheme(darkMode, accentColor);
  ImGui::GetStyle().ScaleAllSizes(uiScale_);
  ImGui::SetCurrentContext(previous);
  ApplyWindowDarkMode(hwnd_, darkMode);
  themeApplied_ = true;
}

bool SettingsHost::BeginFrame(bool darkMode, unsigned accentColor) {
  if (!hwnd_ || !visible_ || !imgui_ || !rtv_) return false;
  if (::IsIconic(hwnd_)) return false;
  // Covered by something else: stop drawing and ask cheaply whether that is
  // still true, rather than paying for a present nobody can see.
  if (occluded_) {
    if (swapchain_->Present(0, DXGI_PRESENT_TEST) != S_OK) return false;
    occluded_ = false;
  }
  if (width_ <= 0 || height_ <= 0) return false;

  // Ein Deckel bleibt, aber weit oberhalb dessen, was ein Monitor zeigt: ohne
  // ihn zeichnet der Dialog den kompletten Einstellungsbaum so oft neu, wie die
  // Schleife durchlaeuft, und verbrennt dafuer Rechenzeit an Bilder, die
  // niemand sieht. Bei vier Millisekunden sind das 250 in der Sekunde -- ueber
  // jeder Bildwiederholrate, die an diesem Rechner haengt, und trotzdem
  // begrenzt.
  const DWORD now = ::GetTickCount();
  if (lastDrawTick_ != 0 && now - lastDrawTick_ < 4) return false;
  lastDrawTick_ = now;

  if (!themeApplied_) ApplyTheme(darkMode, accentColor);

  previous_ = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(imgui_);
  ImGui_ImplDX11_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();
  return true;
}

void SettingsHost::EndFrame() {
  ImGui::Render();

  float clear[4];
  GetBackgroundColor(true, 0x000000, clear);
  const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
  clear[0] = bg.x;
  clear[1] = bg.y;
  clear[2] = bg.z;
  clear[3] = 1.0f;

  ID3D11RenderTargetView* rtvs[] = {rtv_.Get()};
  ctx_->OMSetRenderTargets(1, rtvs, nullptr);
  ctx_->ClearRenderTargetView(rtv_.Get(), clear);

  D3D11_VIEWPORT vp = {};
  vp.Width = (float)width_;
  vp.Height = (float)height_;
  vp.MaxDepth = 1.0f;
  ctx_->RSSetViewports(1, &vp);

  ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
  // Sixty a second, after the preview has already gone to the screen. Both
  // halves of that matter: this used to run in the middle of the preview's own
  // frame, where presenting a second swapchain flushes everything queued for
  // the first one.
  //
  // The result is kept rather than discarded: a fully covered window gets DXGI's
  // occluded-present throttle, and every present then takes far longer than it
  // looks like it should. The main window handles that the same way.
  const HRESULT hr = swapchain_->Present(0, presentFlags_);
  occluded_ = hr == DXGI_STATUS_OCCLUDED;

  // And unbind. Defensive now rather than necessary -- the preview sets its own
  // targets at the start of every pass -- but leaving a presented back buffer
  // bound to the shared immediate context is the sort of thing that only shows
  // up later, in something unrelated.
  ID3D11RenderTargetView* none[] = {nullptr};
  ctx_->OMSetRenderTargets(1, none, nullptr);

  ImGui::SetCurrentContext(previous_);
  previous_ = nullptr;
}

SettingsHost::Placement SettingsHost::placement() const {
  Placement out;
  if (!hwnd_) return out;
  // The restored rectangle, not the current one: a window read while it is
  // minimised or maximised would be remembered at the wrong size.
  WINDOWPLACEMENT wp = {};
  wp.length = sizeof(wp);
  if (!::GetWindowPlacement(hwnd_, &wp)) return out;
  out.x = wp.rcNormalPosition.left;
  out.y = wp.rcNormalPosition.top;
  out.width = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
  out.height = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
  return out;
}

}  // namespace cap
