// The Windows half of screen_capture.h: DXGI Desktop Duplication, on a device
// of its own on the monitor's adapter.
//
// The duplication hands out the desktop's own texture whenever something on
// it changed, and the pointer apart from it. Each new picture is copied into
// the slot the frame buffer gives, the pointer drawn over it, and the slot
// published: one copy on the GPU and, for the pointer, two tiny draws. Only a
// renderer elsewhere costs a read back.

#include "capture/screen_capture.h"

#include "common_win32.h"

// avrt.h has no includes of its own and needs windows.h and SAL first.
#include <avrt.h>
#include <d3d11.h>
#include <dxgi1_5.h>
#include <shellapi.h>  // SHQueryUserNotificationState

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "i18n.h"
#include "inflate.h"
#include "render/shared_frames.h"
#include "text_win32.h"

// The same bytecode the renderer draws with (render_passes_win32.cpp).
#ifdef QBLANK_SHADERS_PACKED
#include "shaders_dxbc.h"
#else
namespace cap {
extern const Packed kFullscreenVS, kCursorPS;
}
#endif

namespace cap {
namespace {

bool Failed(HRESULT hr, const char* what) {
  if (SUCCEEDED(hr)) return false;
  CAP_WARN("Screen capture: %s failed: %s", what, HrToEnglish(hr).c_str());
  return true;
}

// The textures stay open as long as either side holds them.
class OwnedFrames : public SharedFrames {
 public:
  ~OwnedFrames() override {
    for (void* h : handle) {
      if (h) CloseHandle(h);
    }
  }
};

BOOL CALLBACK CollectScreen(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
  MONITORINFOEXW mi = {};
  mi.cbSize = sizeof(mi);
  if (::GetMonitorInfoW(monitor, &mi)) {
    ScreenInfo s;
    s.id = ToUtf8(mi.szDevice);
    s.width = mi.rcMonitor.right - mi.rcMonitor.left;
    s.height = mi.rcMonitor.bottom - mi.rcMonitor.top;
    s.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    ((std::vector<ScreenInfo>*)param)->push_back(std::move(s));
  }
  return TRUE;
}

uint64_t LuidOf(IDXGIAdapter1* adapter) {
  DXGI_ADAPTER_DESC1 desc = {};
  uint64_t luid = 0;
  if (SUCCEEDED(adapter->GetDesc1(&desc))) memcpy(&luid, &desc.AdapterLuid, sizeof(luid));
  return luid;
}

// The output a monitor of EnumDisplayMonitors is on DXGI's side, and its
// adapter: the one whose device may duplicate it.
bool FindOutput(const std::wstring& device, ComPtr<IDXGIAdapter1>* adapter,
                ComPtr<IDXGIOutput1>* output) {
  ComPtr<IDXGIFactory1> factory;
  if (Failed(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1")) return false;
  ComPtr<IDXGIAdapter1> a;
  for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
    ComPtr<IDXGIOutput> o;
    for (UINT j = 0; a->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; ++j) {
      DXGI_OUTPUT_DESC desc = {};
      if (SUCCEEDED(o->GetDesc(&desc)) && device == desc.DeviceName && SUCCEEDED(o.As(output))) {
        *adapter = a;
        return true;
      }
    }
  }
  return false;
}

// The window in front while Windows says a Direct3D program holds a screen in
// exclusive fullscreen, else null; never one of qBlank's.
HWND ExclusiveFullscreenWindow() {
  QUERY_USER_NOTIFICATION_STATE state;
  if (FAILED(::SHQueryUserNotificationState(&state)) || state != QUNS_RUNNING_D3D_FULL_SCREEN) {
    return nullptr;
  }
  HWND front = ::GetForegroundWindow();
  DWORD pid = 0;
  if (!front || !::GetWindowThreadProcessId(front, &pid) || pid == ::GetCurrentProcessId()) {
    return nullptr;
  }
  return front;
}

// Why a duplication could not be had, for the user.
Said DuplicationRefused(HRESULT hr) {
  switch (hr) {
    case E_ACCESSDENIED:
      return CAP_SAID(T("Windows gibt den Bildschirm gerade nicht frei, etwa bei einer "
                        "UAC-Abfrage oder am Sperrbildschirm.",
                        "Windows is not sharing the screen right now, for example during a UAC "
                        "prompt or on the lock screen."));
    case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE:
      return CAP_SAID(T("Zu viele Programme nehmen diesen Bildschirm schon auf.",
                        "Too many programs are capturing this screen already."));
    case DXGI_ERROR_UNSUPPORTED:
      return CAP_SAID(T("Der Grafiktreiber lässt diesen Bildschirm nicht aufnehmen.",
                        "The graphics driver does not allow capturing this screen."));
    default:
      return CAP_SAID(Format(T("Die Bildschirmaufnahme lässt sich nicht starten: %s",
                               "The screen capture could not be started: %s"),
                             HrToString(hr).c_str()));
  }
}

class Win32ScreenCapture : public ScreenCapture {
 public:
  ~Win32ScreenCapture() override { Stop(); }

  bool Start(const CaptureTarget& target, bool cursor, uint64_t adapter,
             std::string* error) override;
  void Stop() override;
  bool running() const override { return thread_.joinable(); }

  void SetCursor(bool on) override {
    if (cursor_.exchange(on) != on) redraw_ = true;
  }

  const std::string& screen() const override { return screen_; }
  uint64_t adapter() const override { return adapter_; }

  FrameBuffer* frames() override { return running() ? &frames_ : nullptr; }
  std::shared_ptr<SharedFrames> sharedFrames() const override { return shared_; }

  bool needsRestart(std::string* error) const override {
    if (!restart_.load()) return false;
    if (error) {
      std::lock_guard<std::mutex> lock(errorMutex_);
      *error = error_;
    }
    return true;
  }

  bool exclusiveFullscreen() const override {
    HWND front = ExclusiveFullscreenWindow();
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    return front && ::GetMonitorInfoW(::MonitorFromWindow(front, MONITOR_DEFAULTTONULL), &mi) &&
           ToUtf8(mi.szDevice) == screen_;
  }

 private:
  // The pointer's pictures: one blended over, or two that mask and then
  // invert, as Windows draws a monochrome or masked one.
  enum class PointerKind { None, Color, Masked };

  bool Duplicate(Said* refused);
  bool CreateTargets();
  bool CreatePipeline();
  bool CreateShader(const Packed& packed, bool pixel);
  bool MakeTexture(const uint32_t* pixels, int width, int height,
                   ComPtr<ID3D11ShaderResourceView>* srv);

  void Run();
  // What the frame says about the pointer taken over; true when the picture
  // should be drawn again for it.
  bool TakePointer(const DXGI_OUTDUPL_FRAME_INFO& info);
  bool ReadPointerShape(UINT size);
  // `from` into the next slot, the pointer over it with `pointer`, published.
  void Compose(ID3D11Texture2D* from, bool pointer);
  void DrawPointer(ID3D11RenderTargetView* target);
  void Restart(const Said* why);

  std::string screen_;
  uint64_t adapter_ = 0;
  bool share_ = false;
  int width_ = 0;
  int height_ = 0;

  ComPtr<IDXGIOutput1> output_;
  ComPtr<ID3D11Device> dev_;
  ComPtr<ID3D11DeviceContext> dc_;
  ComPtr<IDXGIOutputDuplication> dupl_;

  FrameBuffer frames_;
  std::shared_ptr<OwnedFrames> shared_;
  ComPtr<ID3D11Texture2D> tex_[SharedFrames::kSlots];
  ComPtr<IDXGIKeyedMutex> mutex_[SharedFrames::kSlots];
  ComPtr<ID3D11RenderTargetView> rtv_[SharedFrames::kSlots];
  // Without sharing: the picture composed, and read back.
  ComPtr<ID3D11Texture2D> compose_;
  ComPtr<ID3D11RenderTargetView> composeRtv_;
  ComPtr<ID3D11Texture2D> staging_;
  // The last desktop picture, kept while the pointer is drawn: a frame in which
  // only the pointer moved brings no picture of its own.
  ComPtr<ID3D11Texture2D> desktop_;
  bool desktopValid_ = false;

  ComPtr<ID3D11VertexShader> vs_;
  ComPtr<ID3D11PixelShader> ps_;
  ComPtr<ID3D11RasterizerState> raster_;
  ComPtr<ID3D11BlendState> blendOver_;
  ComPtr<ID3D11BlendState> blendAnd_;
  ComPtr<ID3D11BlendState> blendXor_;

  PointerKind pointerKind_ = PointerKind::None;
  ComPtr<ID3D11ShaderResourceView> pointerA_;  // the colour, or the AND mask
  ComPtr<ID3D11ShaderResourceView> pointerB_;  // the XOR mask
  int pointerW_ = 0;
  int pointerH_ = 0;
  int pointerX_ = 0;
  int pointerY_ = 0;
  bool pointerVisible_ = false;
  std::vector<uint8_t> shapeBuffer_;
  std::vector<uint32_t> shapeA_, shapeB_;

  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> cursor_{true};
  std::atomic<bool> redraw_{false};
  std::atomic<bool> restart_{false};
  mutable std::mutex errorMutex_;
  std::string error_;
};

bool Win32ScreenCapture::Start(const CaptureTarget& target, bool cursor, uint64_t adapter,
                               std::string* error) {
  const std::string& id = target.screen;
  Stop();
  auto fail = [&](const Said& said) {
    if (error) *error = said.shown;
    if (!said.logged.empty()) CAP_ERR("Screen capture could not be started: %s", said.logged.c_str());
    Stop();
    return false;
  };

  // The screen asked for, or the primary one.
  const std::vector<ScreenInfo> screens = ListScreens();
  const ScreenInfo* chosen = nullptr;
  for (const ScreenInfo& s : screens) {
    if (s.id == id) chosen = &s;
  }
  for (const ScreenInfo& s : screens) {
    if (!chosen && s.primary) chosen = &s;
  }
  if (!chosen && !screens.empty()) chosen = &screens[0];
  if (!chosen) return fail(CAP_SAID(T("Kein Bildschirm gefunden.", "No screen found.")));
  screen_ = chosen->id;

  ComPtr<IDXGIAdapter1> outputAdapter;
  if (!FindOutput(ToWide(screen_), &outputAdapter, &output_)) {
    return fail(CAP_SAID(T("Windows bietet diesen Bildschirm nicht zur Aufnahme an.",
                           "Windows does not offer this screen for capture.")));
  }
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  if (Failed(D3D11CreateDevice(outputAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2,
                               D3D11_SDK_VERSION, &dev_, nullptr, &dc_),
             "D3D11CreateDevice")) {
    return fail(CAP_SAID(T("Die Grafikkarte des Bildschirms lässt sich nicht öffnen.",
                           "The screen's graphics card could not be opened.")));
  }
  Said refused;
  if (!Duplicate(&refused)) return fail(refused);

  DXGI_OUTDUPL_DESC desc = {};
  dupl_->GetDesc(&desc);
  width_ = (int)desc.ModeDesc.Width;
  height_ = (int)desc.ModeDesc.Height;
  if (desc.Rotation != DXGI_MODE_ROTATION_IDENTITY &&
      desc.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
    CAP_WARN("Screen capture: %s is rotated (%d); the picture comes unrotated", screen_.c_str(),
             (int)desc.Rotation);
  }
  // Shared within one adapter only; across two the picture goes through the
  // CPU.
  adapter_ = adapter;
  share_ = adapter != 0 && LuidOf(outputAdapter.Get()) == adapter;
  if (!CreateTargets() || !CreatePipeline()) {
    return fail(CAP_SAID(T("Die Bildschirmaufnahme lässt sich nicht einrichten.",
                           "The screen capture could not be set up.")));
  }

  VideoFormatInfo info;
  info.layout = PixelLayout::Bgrx32;
  info.subtypeLabel = "RGB32";
  info.width = width_;
  info.height = height_;
  info.stride = width_ * 4;
  info.fps = desc.ModeDesc.RefreshRate.Denominator
                 ? (double)desc.ModeDesc.RefreshRate.Numerator / desc.ModeDesc.RefreshRate.Denominator
                 : 60.0;
  info.imageSize = (size_t)width_ * height_ * 4;
  info.color.present = true;
  info.color.range = ColorInfo::Range::Full;
  info.color.matrix = ColorInfo::Matrix::BT709;
  info.color.primaries = ColorInfo::Primaries::BT709;
  info.color.transfer = ColorInfo::Transfer::SRGB;
  frames_.Configure(info, share_);

  CAP_LOG("Screen capture: %s, %dx%d at %.2f Hz, %s", screen_.c_str(), width_, height_, info.fps,
          share_ ? "on the GPU" : "read back");
  cursor_ = cursor;
  stop_ = false;
  restart_ = false;
  thread_ = std::thread(&Win32ScreenCapture::Run, this);
  return true;
}

void Win32ScreenCapture::Stop() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
  frames_.Clear();
  dupl_.Reset();
  shared_.reset();
  for (int i = 0; i < SharedFrames::kSlots; ++i) {
    tex_[i].Reset();
    mutex_[i].Reset();
    rtv_[i].Reset();
  }
  compose_.Reset();
  composeRtv_.Reset();
  staging_.Reset();
  desktop_.Reset();
  desktopValid_ = false;
  pointerKind_ = PointerKind::None;
  pointerA_.Reset();
  pointerB_.Reset();
  pointerVisible_ = false;
  vs_.Reset();
  ps_.Reset();
  raster_.Reset();
  blendOver_.Reset();
  blendAnd_.Reset();
  blendXor_.Reset();
  dc_.Reset();
  dev_.Reset();
  output_.Reset();
  restart_ = false;
}

bool Win32ScreenCapture::Duplicate(Said* refused) {
  // In the format asked for, which makes Windows bring an HDR desktop down to
  // eight bits; and in the monitor's own pixels, as qBlank is DPI aware. The
  // older call where the newer one is missing.
  ComPtr<IDXGIOutput5> output5;
  HRESULT hr = E_NOINTERFACE;
  if (SUCCEEDED(output_.As(&output5))) {
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
    hr = output5->DuplicateOutput1(dev_.Get(), 0, 1, formats, &dupl_);
  }
  if (FAILED(hr)) hr = output_->DuplicateOutput(dev_.Get(), &dupl_);
  if (SUCCEEDED(hr)) return true;
  CAP_LOG("Screen capture: DuplicateOutput gave %s", HrToEnglish(hr).c_str());
  if (refused) *refused = DuplicationRefused(hr);
  return false;
}

bool Win32ScreenCapture::CreateTargets() {
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)width_;
  td.Height = (UINT)height_;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  if (!share_) {
    if (Failed(dev_->CreateTexture2D(&td, nullptr, &compose_), "CreateTexture2D") ||
        Failed(dev_->CreateRenderTargetView(compose_.Get(), nullptr, &composeRtv_),
               "CreateRenderTargetView")) {
      return false;
    }
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return !Failed(dev_->CreateTexture2D(&td, nullptr, &staging_), "CreateTexture2D");
  }

  td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
  shared_ = std::make_shared<OwnedFrames>();
  shared_->width = width_;
  shared_->height = height_;
  shared_->rows = height_;
  shared_->bgra = true;
  for (int i = 0; i < SharedFrames::kSlots; ++i) {
    ComPtr<IDXGIResource1> resource;
    HANDLE handle = nullptr;
    if (Failed(dev_->CreateTexture2D(&td, nullptr, &tex_[i]), "CreateTexture2D") ||
        Failed(tex_[i].As(&mutex_[i]), "IDXGIKeyedMutex") ||
        Failed(tex_[i].As(&resource), "IDXGIResource1") ||
        Failed(resource->CreateSharedHandle(
                   nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                   &handle),
               "CreateSharedHandle") ||
        Failed(dev_->CreateRenderTargetView(tex_[i].Get(), nullptr, &rtv_[i]),
               "CreateRenderTargetView")) {
      return false;
    }
    shared_->handle[i] = handle;
    // The renderer copies with key 1 and gives back 0; a fresh texture opens
    // with 0.
    shared_->key[i] = 0;
  }
  return true;
}

bool Win32ScreenCapture::CreateShader(const Packed& packed, bool pixel) {
  std::vector<uint8_t> code(packed.unpackedSize);
  if (!Inflate(packed, code.data())) {
    CAP_ERR("Screen capture: a shader is damaged");
    return false;
  }
  return pixel ? !Failed(dev_->CreatePixelShader(code.data(), code.size(), nullptr, &ps_),
                         "CreatePixelShader")
               : !Failed(dev_->CreateVertexShader(code.data(), code.size(), nullptr, &vs_),
                         "CreateVertexShader");
}

bool Win32ScreenCapture::CreatePipeline() {
  if (!CreateShader(kFullscreenVS, false) || !CreateShader(kCursorPS, true)) return false;
  D3D11_RASTERIZER_DESC rd = {};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  if (Failed(dev_->CreateRasterizerState(&rd, &raster_), "CreateRasterizerState")) return false;

  // Colour only; what the desktop has in alpha stays.
  D3D11_BLEND_DESC bd = {};
  D3D11_RENDER_TARGET_BLEND_DESC& rt = bd.RenderTarget[0];
  rt.BlendEnable = TRUE;
  rt.BlendOp = D3D11_BLEND_OP_ADD;
  rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
  rt.DestBlendAlpha = D3D11_BLEND_ONE;
  rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
  rt.RenderTargetWriteMask =
      D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
  // Over.
  rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
  rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  if (Failed(dev_->CreateBlendState(&bd, &blendOver_), "CreateBlendState")) return false;
  // The screen times the mask: white keeps it, black clears it.
  rt.SrcBlend = D3D11_BLEND_ZERO;
  rt.DestBlend = D3D11_BLEND_SRC_COLOR;
  if (Failed(dev_->CreateBlendState(&bd, &blendAnd_), "CreateBlendState")) return false;
  // s(1 - d) + d(1 - s): exactly XOR where the mask is black or white.
  rt.SrcBlend = D3D11_BLEND_INV_DEST_COLOR;
  rt.DestBlend = D3D11_BLEND_INV_SRC_COLOR;
  return !Failed(dev_->CreateBlendState(&bd, &blendXor_), "CreateBlendState");
}

bool Win32ScreenCapture::MakeTexture(const uint32_t* pixels, int width, int height,
                                     ComPtr<ID3D11ShaderResourceView>* srv) {
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)width;
  td.Height = (UINT)height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_IMMUTABLE;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA init = {pixels, (UINT)width * 4, 0};
  ComPtr<ID3D11Texture2D> tex;
  return !Failed(dev_->CreateTexture2D(&td, &init, &tex), "CreateTexture2D") &&
         !Failed(dev_->CreateShaderResourceView(tex.Get(), nullptr, srv->ReleaseAndGetAddressOf()),
                 "CreateShaderResourceView");
}

void Win32ScreenCapture::Run() {
  DWORD taskIndex = 0;
  HANDLE task = ::AvSetMmThreadCharacteristicsW(L"Capture", &taskIndex);
  bool holding = false;
  while (!stop_) {
    if (!dupl_) {
      // Lost, for the secure desktop or a mode change: tried again until it is
      // back, the last picture standing meanwhile.
      if (!Duplicate(nullptr)) {
        ::Sleep(100);
        continue;
      }
      DXGI_OUTDUPL_DESC desc = {};
      dupl_->GetDesc(&desc);
      if ((int)desc.ModeDesc.Width != width_ || (int)desc.ModeDesc.Height != height_) {
        CAP_LOG("Screen capture: %s is %ux%u now", screen_.c_str(), desc.ModeDesc.Width,
                desc.ModeDesc.Height);
        Restart(nullptr);
        break;
      }
      desktopValid_ = false;
    }

    // Held until the next one is asked for, as the documentation advises:
    // while it is held Windows does not copy into it.
    if (holding) {
      dupl_->ReleaseFrame();
      holding = false;
    }
    DXGI_OUTDUPL_FRAME_INFO info = {};
    ComPtr<IDXGIResource> resource;
    const HRESULT hr = dupl_->AcquireNextFrame(100, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
      // The pointer switched on or off over a picture that stands still.
      if (redraw_.exchange(false) && desktopValid_) Compose(desktop_.Get(), cursor_);
      continue;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST) {
      dupl_.Reset();
      continue;
    }
    if (FAILED(hr)) {
      CAP_WARN("Screen capture: AcquireNextFrame gave %s", HrToEnglish(hr).c_str());
      const Said why = CAP_SAID(T("Die Bildschirmaufnahme ist abgerissen.",
                                  "The screen capture broke off."));
      Restart(&why);
      break;
    }
    holding = true;

    const bool cursor = cursor_;
    const bool picture = info.LastPresentTime.QuadPart != 0;
    const bool pointer = TakePointer(info) && cursor;
    redraw_ = false;
    if (!picture && !pointer) continue;

    if (picture) {
      ComPtr<ID3D11Texture2D> desktop;
      if (Failed(resource.As(&desktop), "ID3D11Texture2D")) continue;
      if (!cursor) {
        desktopValid_ = false;
        Compose(desktop.Get(), false);
        continue;
      }
      if (!desktop_) {
        D3D11_TEXTURE2D_DESC td = {};
        desktop->GetDesc(&td);
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = 0;
        td.CPUAccessFlags = 0;
        td.MiscFlags = 0;
        if (Failed(dev_->CreateTexture2D(&td, nullptr, &desktop_), "CreateTexture2D")) continue;
      }
      dc_->CopyResource(desktop_.Get(), desktop.Get());
      desktopValid_ = true;
    }
    if (desktopValid_) Compose(desktop_.Get(), true);
  }
  if (holding && dupl_) dupl_->ReleaseFrame();
  if (task) ::AvRevertMmThreadCharacteristics(task);
}

bool Win32ScreenCapture::TakePointer(const DXGI_OUTDUPL_FRAME_INFO& info) {
  bool changed = false;
  if (info.PointerShapeBufferSize && ReadPointerShape(info.PointerShapeBufferSize)) changed = true;
  if (info.LastMouseUpdateTime.QuadPart != 0) {
    // On another screen it is not visible on this one.
    const bool visible = info.PointerPosition.Visible != FALSE;
    const int x = info.PointerPosition.Position.x;
    const int y = info.PointerPosition.Position.y;
    changed = changed || visible != pointerVisible_ ||
              (visible && (x != pointerX_ || y != pointerY_));
    pointerVisible_ = visible;
    pointerX_ = x;
    pointerY_ = y;
  }
  return changed;
}

bool Win32ScreenCapture::ReadPointerShape(UINT size) {
  shapeBuffer_.resize(size);
  UINT required = 0;
  DXGI_OUTDUPL_POINTER_SHAPE_INFO shape = {};
  if (Failed(dupl_->GetFramePointerShape(size, shapeBuffer_.data(), &required, &shape),
             "GetFramePointerShape")) {
    return false;
  }
  const int w = (int)shape.Width;
  int h = (int)shape.Height;
  if (shape.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) h /= 2;  // AND rows, then XOR
  if (w <= 0 || h <= 0) return false;
  const size_t n = (size_t)w * h;
  shapeA_.resize(n);
  shapeB_.resize(n);
  const uint8_t* buf = shapeBuffer_.data();
  const UINT pitch = shape.Pitch;

  // White and black as the masks hold them; the passes only read the colour.
  const uint32_t white = 0xFFFFFFFFu;
  const uint32_t black = 0xFF000000u;
  PointerKind kind = PointerKind::Masked;
  switch (shape.Type) {
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
      kind = PointerKind::Color;
      for (int y = 0; y < h; ++y) memcpy(&shapeA_[(size_t)y * w], buf + (size_t)y * pitch, (size_t)w * 4);
      break;
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
      for (int y = 0; y < h; ++y) {
        const uint8_t* andRow = buf + (size_t)y * pitch;
        const uint8_t* xorRow = buf + (size_t)(y + h) * pitch;
        for (int x = 0; x < w; ++x) {
          const uint8_t bit = (uint8_t)(0x80 >> (x & 7));
          shapeA_[(size_t)y * w + x] = (andRow[x >> 3] & bit) ? white : black;
          shapeB_[(size_t)y * w + x] = (xorRow[x >> 3] & bit) ? white : black;
        }
      }
      break;
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
      // Alpha 0: the colour replaces the screen. 0xFF: it is XORed with it.
      for (int y = 0; y < h; ++y) {
        const uint32_t* row = (const uint32_t*)(buf + (size_t)y * pitch);
        for (int x = 0; x < w; ++x) {
          shapeA_[(size_t)y * w + x] = (row[x] >> 24) ? white : black;
          shapeB_[(size_t)y * w + x] = row[x] | black;
        }
      }
      break;
    default:
      return false;
  }

  pointerA_.Reset();
  pointerB_.Reset();
  pointerKind_ = PointerKind::None;
  if (!MakeTexture(shapeA_.data(), w, h, &pointerA_)) return false;
  if (kind == PointerKind::Masked && !MakeTexture(shapeB_.data(), w, h, &pointerB_)) return false;
  pointerKind_ = kind;
  pointerW_ = w;
  pointerH_ = h;
  return true;
}

void Win32ScreenCapture::DrawPointer(ID3D11RenderTargetView* target) {
  if (!pointerVisible_ || pointerKind_ == PointerKind::None) return;
  ID3D11DeviceContext* dc = dc_.Get();
  dc->IASetInputLayout(nullptr);
  dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  dc->VSSetShader(vs_.Get(), nullptr, 0);
  dc->PSSetShader(ps_.Get(), nullptr, 0);
  dc->RSSetState(raster_.Get());
  dc->OMSetDepthStencilState(nullptr, 0);
  dc->OMSetRenderTargets(1, &target, nullptr);
  // The pointer's rectangle; the part off the screen is clipped.
  D3D11_VIEWPORT vp = {};
  vp.TopLeftX = (float)pointerX_;
  vp.TopLeftY = (float)pointerY_;
  vp.Width = (float)pointerW_;
  vp.Height = (float)pointerH_;
  vp.MaxDepth = 1.0f;
  dc->RSSetViewports(1, &vp);

  ID3D11ShaderResourceView* srv[] = {pointerA_.Get()};
  dc->PSSetShaderResources(0, 1, srv);
  dc->OMSetBlendState(pointerKind_ == PointerKind::Color ? blendOver_.Get() : blendAnd_.Get(),
                      nullptr, 0xFFFFFFFF);
  dc->Draw(3, 0);
  if (pointerKind_ == PointerKind::Masked) {
    srv[0] = pointerB_.Get();
    dc->PSSetShaderResources(0, 1, srv);
    dc->OMSetBlendState(blendXor_.Get(), nullptr, 0xFFFFFFFF);
    dc->Draw(3, 0);
  }
  srv[0] = nullptr;
  dc->PSSetShaderResources(0, 1, srv);
  dc->OMSetRenderTargets(0, nullptr, nullptr);
}

void Win32ScreenCapture::Compose(ID3D11Texture2D* from, bool pointer) {
  pointer = pointer && pointerVisible_ && pointerKind_ != PointerKind::None;
  const size_t bytes = (size_t)width_ * height_ * 4;
  int slot = 0;
  uint8_t* out = frames_.BeginWrite(share_ ? 0 : bytes, &slot);

  if (share_) {
    // Key 0 once the renderer copied it out, still 1 when it never got to it.
    // Its copy waits on the GPU, not here: the renderer gives the key back as
    // soon as the copy is queued.
    const HRESULT hr = mutex_[slot]->AcquireSync(shared_->key[slot], 100);
    if (hr != S_OK) {
      CAP_WARN("Screen capture: AcquireSync gave 0x%08lx", hr);
      return;
    }
    dc_->CopyResource(tex_[slot].Get(), from);
    if (pointer) DrawPointer(rtv_[slot].Get());
    mutex_[slot]->ReleaseSync(1);
    shared_->key[slot] = 1;
    frames_.EndWrite(slot, 0);
    return;
  }

  if (pointer) {
    dc_->CopyResource(compose_.Get(), from);
    DrawPointer(composeRtv_.Get());
    from = compose_.Get();
  }
  dc_->CopyResource(staging_.Get(), from);
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (Failed(dc_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map")) return;
  const size_t row = (size_t)width_ * 4;
  const uint8_t* src = (const uint8_t*)mapped.pData;
  if (mapped.RowPitch == row) {
    memcpy(out, src, bytes);
  } else {
    for (int y = 0; y < height_; ++y) memcpy(out + y * row, src + (size_t)y * mapped.RowPitch, row);
  }
  dc_->Unmap(staging_.Get(), 0);
  frames_.EndWrite(slot, bytes);
}

void Win32ScreenCapture::Restart(const Said* why) {
  {
    std::lock_guard<std::mutex> lock(errorMutex_);
    error_ = why ? why->shown : std::string();
  }
  restart_ = true;
  // Wakes the loop that waits for frames, so it finds out now.
  frames_.frameReady().Signal();
}

}  // namespace

std::vector<ScreenInfo> ListScreens() {
  std::vector<ScreenInfo> list;
  ::EnumDisplayMonitors(nullptr, nullptr, CollectScreen, (LPARAM)&list);
  return list;
}

std::unique_ptr<ScreenCapture> CreateScreenCapture() {
  return std::make_unique<Win32ScreenCapture>();
}

}  // namespace cap
