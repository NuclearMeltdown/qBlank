// The Windows half of screen_capture.h for a single window:
// Windows.Graphics.Capture through its ABI headers, on a device of its own on
// the renderer's adapter.
//
// The compositor hands each new picture of the window to a frame pool, whose
// handler only wakes the capture thread. That copies the part the program draws
// in out of the newest picture into the slot the frame buffer gives and
// publishes it: one copy on the GPU, like the screen's. Only a renderer
// elsewhere costs a read back.

#include "capture/screen_capture.h"

#include "common_win32.h"

// avrt.h has no includes of its own and needs windows.h and SAL first.
#include <avrt.h>
#include <d3d11_4.h>
#include <dwmapi.h>
#include <roapi.h>
#include <shellapi.h>  // SHQueryUserNotificationState
#include <winstring.h>

#include <Windows.Graphics.Capture.Interop.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "capture/monitor_hdr_win32.h"
#include "i18n.h"
#include "render/shared_frames.h"
#include "text_win32.h"

namespace cap {
namespace {

namespace wgc = ABI::Windows::Graphics::Capture;
namespace wgd3d = ABI::Windows::Graphics::DirectX::Direct3D11;
using FrameHandler =
    ABI::Windows::Foundation::ITypedEventHandler<wgc::Direct3D11CaptureFramePool*, IInspectable*>;
using DxgiAccess = ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;

// A size change counts once the window has held it this long: dragging its
// edge would otherwise start the capture anew for every step.
const double kSettleSeconds = 0.2;

bool Failed(HRESULT hr, const char* what) {
  if (SUCCEEDED(hr)) return false;
  CAP_WARN("Window capture: %s failed: %s", what, HrToEnglish(hr).c_str());
  return true;
}

template <typename T>
HRESULT Factory(const wchar_t* runtimeClass, T** out) {
  HSTRING_HEADER header;
  HSTRING name = nullptr;
  const HRESULT hr =
      ::WindowsCreateStringReference(runtimeClass, (UINT32)wcslen(runtimeClass), &header, &name);
  if (FAILED(hr)) return hr;
  return ::RoGetActivationFactory(name, __uuidof(T), (void**)out);
}

void CloseObject(IUnknown* object) {
  ComPtr<ABI::Windows::Foundation::IClosable> closable;
  if (object && SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&closable)))) closable->Close();
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

// Called on a thread of the pool's; it only wakes the capture thread.
class FrameArrived : public FrameHandler {
 public:
  FrameArrived() : event_(::CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
  ~FrameArrived() {
    if (event_) ::CloseHandle(event_);
  }
  HANDLE event() const { return event_; }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IAgileObject) ||
        iid == __uuidof(FrameHandler)) {
      *out = static_cast<FrameHandler*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG left = --refs_;
    if (left == 0) delete this;
    return left;
  }
  HRESULT STDMETHODCALLTYPE Invoke(wgc::IDirect3D11CaptureFramePool*, IInspectable*) override {
    ::SetEvent(event_);
    return S_OK;
  }

 private:
  HANDLE event_;
  std::atomic<ULONG> refs_{1};
};

uint64_t LuidOf(IDXGIAdapter1* adapter) {
  DXGI_ADAPTER_DESC1 desc = {};
  uint64_t luid = 0;
  if (SUCCEEDED(adapter->GetDesc1(&desc))) memcpy(&luid, &desc.AdapterLuid, sizeof(luid));
  return luid;
}

std::string ProcessName(DWORD pid) {
  HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return std::string();
  wchar_t path[MAX_PATH];
  DWORD n = MAX_PATH;
  const bool ok = ::QueryFullProcessImageNameW(process, 0, path, &n) != FALSE;
  ::CloseHandle(process);
  if (!ok) return std::string();
  const wchar_t* name = wcsrchr(path, L'\\');
  return ToUtf8(name ? name + 1 : path);
}

struct Candidate {
  HWND hwnd = nullptr;
  WindowInfo info;
};

// What the taskbar would show: visible, not cloaked, not a tool window, with a
// title; no window of qBlank's and not the desktop.
BOOL CALLBACK CollectWindow(HWND hwnd, LPARAM param) {
  if (!::IsWindowVisible(hwnd)) return TRUE;
  const LONG_PTR ex = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  if (ex & WS_EX_TOOLWINDOW) return TRUE;
  if (::GetWindow(hwnd, GW_OWNER) && !(ex & WS_EX_APPWINDOW)) return TRUE;
  DWORD cloaked = 0;
  if (SUCCEEDED(::DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
      cloaked) {
    return TRUE;
  }
  DWORD pid = 0;
  ::GetWindowThreadProcessId(hwnd, &pid);
  if (pid == ::GetCurrentProcessId()) return TRUE;
  wchar_t cls[16];
  if (::GetClassNameW(hwnd, cls, 16) && wcscmp(cls, L"Progman") == 0) return TRUE;
  const int length = ::GetWindowTextLengthW(hwnd);
  if (length <= 0) return TRUE;
  std::wstring title((size_t)length + 1, L'\0');
  title.resize((size_t)::GetWindowTextW(hwnd, title.data(), length + 1));
  if (title.empty()) return TRUE;

  Candidate c;
  c.hwnd = hwnd;
  c.info.app = ProcessName(pid);
  if (c.info.app.empty()) return TRUE;
  c.info.title = ToUtf8(title);
  RECT client = {};
  if (::GetClientRect(hwnd, &client)) {
    c.info.width = client.right;
    c.info.height = client.bottom;
  }
  ((std::vector<Candidate>*)param)->push_back(std::move(c));
  return TRUE;
}

std::vector<Candidate> Candidates() {
  std::vector<Candidate> list;
  ::EnumWindows(CollectWindow, (LPARAM)&list);
  return list;
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

// The monitor's refresh rate as the display path has it, fractions and all.
double RefreshOf(HMONITOR monitor) {
  MONITORINFOEXW mi = {};
  mi.cbSize = sizeof(mi);
  DISPLAYCONFIG_PATH_INFO path = {};
  if (!::GetMonitorInfoW(monitor, &mi) || !DisplayPathOf(mi.szDevice, &path)) return 60.0;
  const DISPLAYCONFIG_RATIONAL& rate = path.targetInfo.refreshRate;
  return rate.Denominator && rate.Numerator ? (double)rate.Numerator / rate.Denominator : 60.0;
}

class Win32WindowCapture : public ScreenCapture {
 public:
  ~Win32WindowCapture() override { Stop(); }

  bool Start(const CaptureTarget& target, bool cursor, uint64_t adapter,
             std::string* error) override;
  void Stop() override;
  bool running() const override { return thread_.joinable(); }

  void SetCursor(bool on) override { cursor_ = on; }

  const std::string& screen() const override { return title_; }
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

  // By program: a game in exclusive fullscreen may well put up a window of its
  // own for it.
  bool exclusiveFullscreen() const override {
    HWND front = ExclusiveFullscreenWindow();
    DWORD frontPid = 0, ownPid = 0;
    return front && hwnd_ && ::GetWindowThreadProcessId(front, &frontPid) &&
           ::GetWindowThreadProcessId(hwnd_, &ownPid) && frontPid == ownPid;
  }

 private:
  bool CreateDevice(uint64_t adapter);
  bool CreateTargets();
  bool OpenSession(bool cursor);

  void Run();
  void Publish(ID3D11Texture2D* from);
  bool HdrChanged() const;
  void Restart(const Said* why);

  HWND hwnd_ = nullptr;
  std::string title_;
  uint64_t adapter_ = 0;
  bool share_ = false;
  // The HDR of the window's screen, and whether the picture comes in its half
  // floats.
  MonitorHdr hdr_;
  bool half_ = false;
  ULONGLONG hdrChecked_ = 0;
  // The window's picture as the pool was made for it, and in it the part the
  // program draws in.
  int frameW_ = 0;
  int frameH_ = 0;
  int cropX_ = 0;
  int cropY_ = 0;
  int width_ = 0;
  int height_ = 0;

  ComPtr<ID3D11Device> dev_;
  ComPtr<ID3D11DeviceContext> dc_;
  ComPtr<wgd3d::IDirect3DDevice> device_;
  ComPtr<wgc::IGraphicsCaptureItem> item_;
  ComPtr<wgc::IDirect3D11CaptureFramePool> pool_;
  ComPtr<wgc::IGraphicsCaptureSession> session_;
  ComPtr<FrameArrived> handler_;
  EventRegistrationToken token_ = {};
  bool subscribed_ = false;

  FrameBuffer frames_;
  std::shared_ptr<OwnedFrames> shared_;
  ComPtr<ID3D11Texture2D> tex_[SharedFrames::kSlots];
  ComPtr<IDXGIKeyedMutex> mutex_[SharedFrames::kSlots];
  ComPtr<ID3D11Texture2D> staging_;

  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> cursor_{true};
  std::atomic<bool> restart_{false};
  mutable std::mutex errorMutex_;
  std::string error_;
};

bool Win32WindowCapture::Start(const CaptureTarget& target, bool cursor, uint64_t adapter,
                               std::string* error) {
  Stop();
  auto fail = [&](const Said& said) {
    if (error) *error = said.shown;
    if (!said.logged.empty()) CAP_ERR("Window capture could not be started: %s", said.logged.c_str());
    Stop();
    return false;
  };

  // The window asked for, else the program's frontmost one: its title may
  // have moved on since it was picked.
  // A window still to be picked is no error: shown, but kept out of the log,
  // as it is asked again every few seconds.
  if (target.app.empty()) {
    if (error) *error = T("Kein Fenster gewählt.", "No window chosen.");
    return false;
  }
  const std::vector<Candidate> found = Candidates();
  const Candidate* chosen = nullptr;
  for (const Candidate& c : found) {
    if (!chosen && _stricmp(c.info.app.c_str(), target.app.c_str()) == 0 &&
        c.info.title == target.title) {
      chosen = &c;
    }
  }
  for (const Candidate& c : found) {
    if (!chosen && _stricmp(c.info.app.c_str(), target.app.c_str()) == 0) chosen = &c;
  }
  // Waiting for a program is no error; it goes in no log, as it is asked again
  // every few seconds.
  if (!chosen) {
    if (error) {
      *error = Format(T("%s zeigt gerade kein Fenster.", "%s is showing no window right now."),
                      target.app.c_str());
    }
    return false;
  }
  if (::IsIconic(chosen->hwnd)) {
    if (error) {
      *error = T("Das Fenster ist minimiert und liefert so kein Bild.",
                 "The window is minimized and sends no picture that way.");
    }
    return false;
  }
  hwnd_ = chosen->hwnd;
  title_ = chosen->info.title;

  if (!CreateDevice(adapter)) {
    return fail(CAP_SAID(T("Die Grafikkarte lässt sich für die Fensteraufnahme nicht öffnen.",
                           "The graphics card could not be opened for the window capture.")));
  }
  adapter_ = adapter;
  // HDR where it stays on the GPU; read back, Windows brings it down to eight
  // bits.
  const HMONITOR monitor = ::MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
  if (share_) hdr_ = HdrOf(monitor);
  half_ = share_ && hdr_.on;

  ComPtr<IGraphicsCaptureItemInterop> interop;
  if (Failed(Factory(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem, interop.GetAddressOf()),
             "GraphicsCaptureItem") ||
      Failed(interop->CreateForWindow(hwnd_, __uuidof(wgc::IGraphicsCaptureItem),
                                      (void**)item_.GetAddressOf()),
             "CreateForWindow")) {
    return fail(CAP_SAID(T("Windows gibt dieses Fenster nicht zur Aufnahme frei.",
                           "Windows does not offer this window for capture.")));
  }
  ABI::Windows::Graphics::SizeInt32 size = {};
  item_->get_Size(&size);
  frameW_ = size.Width;
  frameH_ = size.Height;

  // The client area within the picture, which runs from the visible frame's
  // corner. Both corners mapped to the screen: for a program Windows scales,
  // its own client size is in its pixels, the screen's are in ours.
  RECT client = {};
  RECT bounds = {};
  ::GetClientRect(hwnd_, &client);
  ::MapWindowPoints(hwnd_, nullptr, (POINT*)&client, 2);
  if (FAILED(::DwmGetWindowAttribute(hwnd_, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))) {
    ::GetWindowRect(hwnd_, &bounds);
  }
  cropX_ = std::min(std::max((int)(client.left - bounds.left), 0), frameW_);
  cropY_ = std::min(std::max((int)(client.top - bounds.top), 0), frameH_);
  width_ = std::min((int)(client.right - client.left), frameW_ - cropX_);
  height_ = std::min((int)(client.bottom - client.top), frameH_ - cropY_);
  CAP_LOG("Window capture: %s, picture %dx%d, frame at %ld,%ld, client %ldx%ld at %ld,%ld",
          chosen->info.app.c_str(), frameW_, frameH_, bounds.left, bounds.top,
          client.right - client.left, client.bottom - client.top, client.left, client.top);
  if (width_ <= 0 || height_ <= 0) {
    return fail(CAP_SAID(T("Das Fenster hat keine Fläche, die sich aufnehmen ließe.",
                           "The window has no area to capture.")));
  }

  if (!CreateTargets() || !OpenSession(cursor)) {
    return fail(CAP_SAID(T("Die Fensteraufnahme lässt sich nicht einrichten.",
                           "The window capture could not be set up.")));
  }

  VideoFormatInfo info;
  info.layout = half_ ? PixelLayout::Rgba16F : PixelLayout::Bgrx32;
  info.subtypeLabel = half_ ? "RGBA16F" : "RGB32";
  info.width = width_;
  info.height = height_;
  info.stride = width_ * (half_ ? 8 : 4);
  info.fps = RefreshOf(monitor);
  info.imageSize = (size_t)info.stride * height_;
  info.color.present = true;
  info.color.range = ColorInfo::Range::Full;
  info.color.matrix = ColorInfo::Matrix::BT709;
  info.color.primaries = ColorInfo::Primaries::BT709;
  info.color.transfer = half_ ? ColorInfo::Transfer::Linear : ColorInfo::Transfer::SRGB;
  info.whiteNits = hdr_.whiteNits;
  info.peakNits = hdr_.peakNits;
  frames_.Configure(info, share_);

  CAP_LOG("Window capture: %dx%d at %d,%d, %.2f Hz, %s%s", width_, height_, cropX_, cropY_, info.fps,
          share_ ? "on the GPU" : "read back",
          half_ ? Format(", HDR, white at %.0f nits", hdr_.whiteNits).c_str() : "");
  cursor_ = cursor;
  stop_ = false;
  restart_ = false;
  thread_ = std::thread(&Win32WindowCapture::Run, this);
  if (Failed(session_->StartCapture(), "StartCapture")) {
    return fail(CAP_SAID(T("Die Fensteraufnahme lässt sich nicht starten.",
                           "The window capture could not be started.")));
  }
  return true;
}

bool Win32WindowCapture::CreateDevice(uint64_t adapter) {
  // On the renderer's adapter, so the textures can be shared; with none, or
  // where it is not found, on the system's first and read back.
  ComPtr<IDXGIAdapter1> chosen;
  ComPtr<IDXGIFactory1> factory;
  if (adapter != 0 && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; !chosen && factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
      if (LuidOf(a.Get()) == adapter) chosen = a;
    }
  }
  share_ = chosen != nullptr;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  if (Failed(D3D11CreateDevice(chosen.Get(), chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                               nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                               D3D11_SDK_VERSION, &dev_, nullptr, &dc_),
             "D3D11CreateDevice")) {
    return false;
  }
  // The pool works on it from threads of its own.
  ComPtr<ID3D11Multithread> multithread;
  if (SUCCEEDED(dc_.As(&multithread))) multithread->SetMultithreadProtected(TRUE);
  ComPtr<IDXGIDevice> dxgi;
  ComPtr<IInspectable> inspectable;
  return !Failed(dev_.As(&dxgi), "IDXGIDevice") &&
         !Failed(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), &inspectable),
                 "CreateDirect3D11DeviceFromDXGIDevice") &&
         !Failed(inspectable.As(&device_), "IDirect3DDevice");
}

bool Win32WindowCapture::CreateTargets() {
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)width_;
  td.Height = (UINT)height_;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = half_ ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  if (!share_) {
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return !Failed(dev_->CreateTexture2D(&td, nullptr, &staging_), "CreateTexture2D");
  }

  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
  shared_ = std::make_shared<OwnedFrames>();
  shared_->width = width_;
  shared_->height = height_;
  shared_->rows = height_;
  shared_->bgra = true;
  shared_->halfFloat = half_;
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
    // The renderer copies with key 1 and gives back 0; a fresh texture opens
    // with 0.
    shared_->key[i] = 0;
  }
  return true;
}

bool Win32WindowCapture::OpenSession(bool cursor) {
  // Free-threaded: the frames are handed out on the pool's own threads, with
  // no dispatcher queue to run.
  ComPtr<wgc::IDirect3D11CaptureFramePoolStatics2> statics;
  if (Failed(Factory(RuntimeClass_Windows_Graphics_Capture_Direct3D11CaptureFramePool,
                     statics.GetAddressOf()),
             "Direct3D11CaptureFramePool") ||
      Failed(statics->CreateFreeThreaded(
                 device_.Get(),
                 half_ ? ABI::Windows::Graphics::DirectX::DirectXPixelFormat_R16G16B16A16Float
                       : ABI::Windows::Graphics::DirectX::DirectXPixelFormat_B8G8R8A8UIntNormalized,
                 2, {frameW_, frameH_}, &pool_),
             "CreateFreeThreaded")) {
    return false;
  }
  handler_.Attach(new FrameArrived());
  if (!handler_->event() ||
      Failed(pool_->add_FrameArrived(handler_.Get(), &token_), "add_FrameArrived")) {
    return false;
  }
  subscribed_ = true;
  if (Failed(pool_->CreateCaptureSession(item_.Get(), &session_), "CreateCaptureSession")) {
    return false;
  }

  // What newer systems offer on top: the pointer left out, no yellow frame
  // around the window, and every picture rather than a few a second.
  ComPtr<wgc::IGraphicsCaptureSession2> session2;
  if (SUCCEEDED(session_.As(&session2))) session2->put_IsCursorCaptureEnabled(cursor);
  ComPtr<wgc::IGraphicsCaptureSession3> session3;
  if (SUCCEEDED(session_.As(&session3))) {
    const HRESULT hr = session3->put_IsBorderRequired(false);
    if (FAILED(hr)) CAP_LOG("Window capture: no capture without border: %s", HrToEnglish(hr).c_str());
  }
  ComPtr<wgc::IGraphicsCaptureSession5> session5;
  if (SUCCEEDED(session_.As(&session5))) {
    ABI::Windows::Foundation::TimeSpan interval = {};
    interval.Duration = 10000;  // 1 ms in 100 ns ticks
    session5->put_MinUpdateInterval(interval);
  }
  return true;
}

void Win32WindowCapture::Stop() {
  stop_ = true;
  if (handler_) ::SetEvent(handler_->event());
  if (thread_.joinable()) thread_.join();
  if (pool_ && subscribed_) pool_->remove_FrameArrived(token_);
  subscribed_ = false;
  CloseObject(session_.Get());
  CloseObject(pool_.Get());
  session_.Reset();
  pool_.Reset();
  item_.Reset();
  handler_.Reset();
  frames_.Clear();
  shared_.reset();
  for (int i = 0; i < SharedFrames::kSlots; ++i) {
    tex_[i].Reset();
    mutex_[i].Reset();
  }
  staging_.Reset();
  device_.Reset();
  dc_.Reset();
  dev_.Reset();
  hwnd_ = nullptr;
  hdr_ = MonitorHdr();
  half_ = false;
  restart_ = false;
}

void Win32WindowCapture::Run() {
  const HRESULT ro = ::RoInitialize(RO_INIT_MULTITHREADED);
  DWORD taskIndex = 0;
  HANDLE task = ::AvSetMmThreadCharacteristicsW(L"Capture", &taskIndex);
  bool cursor = cursor_;
  // A size the window went to, and since when.
  int pendingW = 0;
  int pendingH = 0;
  int64_t pendingSince = 0;
  while (!stop_) {
    const DWORD waited = ::WaitForSingleObject(handler_->event(), 100);
    if (stop_) break;
    if (cursor_ != cursor) {
      cursor = cursor_;
      ComPtr<wgc::IGraphicsCaptureSession2> session2;
      if (SUCCEEDED(session_.As(&session2))) session2->put_IsCursorCaptureEnabled(cursor);
    }

    if (waited == WAIT_OBJECT_0) {
      // The newest only; what came before it is out of date already.
      ComPtr<wgc::IDirect3D11CaptureFrame> frame;
      ComPtr<wgc::IDirect3D11CaptureFrame> next;
      while (SUCCEEDED(pool_->TryGetNextFrame(next.ReleaseAndGetAddressOf())) && next) {
        CloseObject(frame.Get());
        frame = next;
      }
      if (frame) {
        ABI::Windows::Graphics::SizeInt32 size = {};
        frame->get_ContentSize(&size);
        if (size.Width == frameW_ && size.Height == frameH_) {
          pendingSince = 0;
          ComPtr<wgd3d::IDirect3DSurface> surface;
          ComPtr<DxgiAccess> access;
          ComPtr<ID3D11Texture2D> texture;
          if (SUCCEEDED(frame->get_Surface(&surface)) && SUCCEEDED(surface.As(&access)) &&
              !Failed(access->GetInterface(IID_PPV_ARGS(&texture)), "GetInterface")) {
            Publish(texture.Get());
          }
        } else if (size.Width > 0 && size.Height > 0 &&
                   (pendingSince == 0 || size.Width != pendingW || size.Height != pendingH)) {
          pendingW = size.Width;
          pendingH = size.Height;
          pendingSince = ClockTicks();
        }
        CloseObject(frame.Get());
      }
    } else if (!::IsWindow(hwnd_)) {
      CAP_LOG("Window capture: the window is gone");
      Restart(nullptr);
      break;
    }

    // The new size is set up anew: the pool, the textures and the renderer
    // all have the old one.
    if (pendingSince != 0 && ClockTicks() - pendingSince >= SecondsToTicks(kSettleSeconds)) {
      CAP_LOG("Window capture: the window is %dx%d now", pendingW, pendingH);
      Restart(nullptr);
      break;
    }

    // Unlike the screen's, the window's capture hears nothing of HDR switched,
    // its white moved or the window gone to another screen: asked once a
    // second, after a picture.
    if (share_) {
      const ULONGLONG now = ::GetTickCount64();
      if (now - hdrChecked_ >= 1000) {
        hdrChecked_ = now;
        if (HdrChanged()) {
          CAP_LOG("Window capture: the window's screen changed its HDR");
          Restart(nullptr);
          break;
        }
      }
    }
  }
  if (task) ::AvRevertMmThreadCharacteristics(task);
  if (SUCCEEDED(ro)) ::RoUninitialize();
}

void Win32WindowCapture::Publish(ID3D11Texture2D* from) {
  const D3D11_BOX box = {(UINT)cropX_, (UINT)cropY_, 0, (UINT)(cropX_ + width_),
                         (UINT)(cropY_ + height_), 1};
  const size_t bytes = (size_t)width_ * height_ * 4;
  int slot = 0;
  uint8_t* out = frames_.BeginWrite(share_ ? 0 : bytes, &slot);

  if (share_) {
    // As the screen's: key 0 once the renderer copied it out.
    const HRESULT hr = mutex_[slot]->AcquireSync(shared_->key[slot], 100);
    if (hr != S_OK) {
      CAP_WARN("Window capture: AcquireSync gave 0x%08lx", hr);
      return;
    }
    dc_->CopySubresourceRegion(tex_[slot].Get(), 0, 0, 0, 0, from, 0, &box);
    mutex_[slot]->ReleaseSync(1);
    shared_->key[slot] = 1;
    frames_.EndWrite(slot, 0);
    return;
  }

  dc_->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, from, 0, &box);
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

bool Win32WindowCapture::HdrChanged() const {
  MONITORINFOEXW mi = {};
  mi.cbSize = sizeof(mi);
  return ::GetMonitorInfoW(::MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi) &&
         HdrMoved(hdr_, DisplayHdr(mi.szDevice));
}

void Win32WindowCapture::Restart(const Said* why) {
  {
    std::lock_guard<std::mutex> lock(errorMutex_);
    error_ = why ? why->shown : std::string();
  }
  restart_ = true;
  // Wakes the loop that waits for frames, so it finds out now.
  frames_.frameReady().Signal();
}

}  // namespace

std::vector<WindowInfo> ListWindows() {
  std::vector<WindowInfo> list;
  for (Candidate& c : Candidates()) list.push_back(std::move(c.info));
  return list;
}

bool CanCaptureWindows() {
  static const bool supported = [] {
    // Asked from whichever thread, with the runtime up or not.
    const HRESULT ro = ::RoInitialize(RO_INIT_MULTITHREADED);
    ComPtr<wgc::IGraphicsCaptureSessionStatics> statics;
    boolean yes = false;
    const bool ok = SUCCEEDED(Factory(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureSession,
                                      statics.GetAddressOf())) &&
                    SUCCEEDED(statics->IsSupported(&yes)) && yes;
    statics.Reset();
    if (SUCCEEDED(ro)) ::RoUninitialize();
    CAP_LOG("Window capture %s", ok ? "available" : "not available on this system");
    return ok;
  }();
  return supported;
}

std::unique_ptr<ScreenCapture> CreateWindowCapture() {
  return std::make_unique<Win32WindowCapture>();
}

}  // namespace cap
