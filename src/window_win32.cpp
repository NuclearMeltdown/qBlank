#include "window_win32.h"

#include <dwmapi.h>
#include <windowsx.h>  // GET_X_LPARAM

#include "app_identity.h"
#include "backends/imgui_impl_win32.h"
#include "imgui.h"
#include "keys_win32.h"
#include "resource.h"
#include "text_win32.h"
#include "wake_signal_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace cap {

struct Window::Impl {
  Window* owner = nullptr;
  WindowRole role = WindowRole::Main;
  HWND hwnd = nullptr;
  std::wstring className;
  Window::Listener listener;
  // Dear ImGui's platform half is attached, so every message is offered to it
  // first -- in `ui` when there is one, in the current context otherwise.
  bool forwardUi = false;
  ImGuiContext* ui = nullptr;
  bool cursorHidden = false;
  WINDOWPLACEMENT windowed = {};  // what leaving fullscreen goes back to
};

namespace {

int g_startupShowCmd = SW_SHOWNORMAL;

// Windows 10 1809 used 19 for this attribute, 20 from 20H1 onwards.
constexpr DWORD kDwmUseImmersiveDarkModeOld = 19;
constexpr DWORD kDwmUseImmersiveDarkMode = 20;

// The one timer a window sets, while it is being dragged.
constexpr UINT_PTR kModalTimer = 1;

HINSTANCE Instance() {
  return ::GetModuleHandleW(nullptr);
}

bool Emit(Window::Impl* w, const WindowEvent& event) {
  return w->listener && w->listener(event);
}

bool Emit(Window::Impl* w, WindowEvent::Kind kind) {
  WindowEvent event;
  event.kind = kind;
  return Emit(w, event);
}

// A window's own Dear ImGui context for the length of a call, when it has one.
class UiScope {
 public:
  explicit UiScope(ImGuiContext* context)
      : previous_(ImGui::GetCurrentContext()), swapped_(context != nullptr) {
    if (swapped_) ImGui::SetCurrentContext(context);
  }
  ~UiScope() {
    if (swapped_) ImGui::SetCurrentContext(previous_);
  }

  UiScope(const UiScope&) = delete;
  UiScope& operator=(const UiScope&) = delete;

 private:
  ImGuiContext* previous_;
  bool swapped_;
};

LRESULT HandleMessage(Window::Impl* w, HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (w->forwardUi) {
    UiScope scope(w->ui);
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam) != 0) return 1;
  }

  const WindowRole role = w->role;
  switch (msg) {
    case WM_ENTERSIZEMOVE:
      if (role == WindowRole::Dialog) break;
      // Windows takes the loop away while a window is being dragged, and only a
      // timer still gets through.
      //
      // For the settings, only as a floor for when the mouse is held still:
      // WM_TIMER is the lowest priority message there is and Windows only
      // generates one when the queue is otherwise empty. During a drag the queue
      // never is. Measured, at an interval of 8 ms it fired seven times in a
      // second, and the preview stood still for over a second at a stretch.
      //
      // 10 ms, weil das die kleinste Zahl ist, die Windows fuer einen Timer
      // ueberhaupt annimmt (USER_TIMER_MINIMUM). Ein Weckruf ohne neues Bild
      // kostet seit dem Wegfall der Zeitschranke nichts mehr als die Frage, ob
      // eines da ist -- also darf oefter gefragt werden, als 60 Hz brauchen.
      // Both windows: the main window asked for 8, which Windows quietly raised
      // to the same 10.
      ::SetTimer(hwnd, kModalTimer, USER_TIMER_MINIMUM, nullptr);
      return 0;
    case WM_EXITSIZEMOVE:
      if (role == WindowRole::Dialog) break;
      ::KillTimer(hwnd, kModalTimer);
      return 0;
    case WM_TIMER:
      if (role == WindowRole::Dialog) break;
      if (wparam != kModalTimer) return 0;
      if (Emit(w, WindowEvent::Kind::ModalFrame)) return 0;
      break;
    case WM_MOVING:
    case WM_SIZING:
      // The settings only. This is the hook that actually works. Windows sends
      // these continuously while the window is being dragged or resized -- once
      // per mouse movement -- and unlike WM_TIMER they are real messages that
      // cannot be starved by the flood of mouse input that causes the problem
      // in the first place.
      if (role == WindowRole::Tool) Emit(w, WindowEvent::Kind::ModalFrame);
      break;  // and on to DefWindowProc, which does the actual moving

    case WM_SIZE: {
      WindowEvent event;
      event.kind = WindowEvent::Kind::Resized;
      event.minimized = wparam == SIZE_MINIMIZED;
      if (Emit(w, event)) return 0;
      break;
    }
    case WM_MOVE:
      if (Emit(w, WindowEvent::Kind::Moved)) return 0;
      break;
    case WM_DPICHANGED:
      Emit(w, WindowEvent::Kind::DpiChanged);
      if (role == WindowRole::Main) {
        const auto* rc = reinterpret_cast<const RECT*>(lparam);
        ::SetWindowPos(hwnd, nullptr, rc->left, rc->top, rc->right - rc->left,
                       rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
      }
      break;
    case WM_GETMINMAXINFO:
      if (role != WindowRole::Main) break;
      {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize.x = 320;
        info->ptMinTrackSize.y = 240;
      }
      return 0;

    case WM_SETFOCUS:
    case WM_KILLFOCUS: {
      WindowEvent event;
      event.kind = WindowEvent::Kind::FocusChanged;
      event.focused = msg == WM_SETFOCUS;
      Emit(w, event);
      break;
    }

    case WM_MOUSEMOVE: {
      WindowEvent event;
      event.kind = WindowEvent::Kind::MouseMoved;
      event.mouse = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (Emit(w, event)) return 0;
      break;
    }
    case WM_MOUSEWHEEL: {
      WindowEvent event;
      event.kind = WindowEvent::Kind::MouseWheel;
      event.wheelNotches = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
      if (Emit(w, event)) return 0;
      break;
    }
    case WM_SETCURSOR:
      if (w->cursorHidden && LOWORD(lparam) == HTCLIENT) {
        ::SetCursor(nullptr);
        return 1;
      }
      break;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
      WindowEvent event;
      event.kind = WindowEvent::Kind::KeyDown;
      event.key = KeyFromVirtualKey((unsigned)wparam);
      event.ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
      event.shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
      event.alt = (::GetKeyState(VK_MENU) & 0x8000) != 0;
      if (Emit(w, event)) return 0;
      break;
    }

    case WM_SYSCOMMAND:
      if ((wparam & 0xFFF0) == SC_SCREENSAVE || (wparam & 0xFFF0) == SC_MONITORPOWER) {
        if (Emit(w, WindowEvent::Kind::ScreenSaverStarting)) return 0;
      }
      break;

    case WM_DISPLAYCHANGE:
      Emit(w, WindowEvent::Kind::DisplaysChanged);
      break;
    case WM_DEVICECHANGE:
      Emit(w, WindowEvent::Kind::DevicesChanged);
      break;
    case WM_SETTINGCHANGE:
      Emit(w, WindowEvent::Kind::ThemeChanged);
      break;

    case WM_CLOSE:
      if (Emit(w, WindowEvent::Kind::CloseRequested)) return 0;
      break;
    case WM_ENDSESSION: {
      WindowEvent event;
      event.kind = WindowEvent::Kind::SessionEnd;
      event.ending = wparam != 0;
      if (Emit(w, event)) return 0;
      break;
    }
    case WM_DESTROY: {
      const bool handled = Emit(w, WindowEvent::Kind::Destroyed);
      w->hwnd = nullptr;
      if (handled) return 0;
      break;
    }

    default:
      break;
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    auto* w = static_cast<Window::Impl*>(cs->lpCreateParams);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)w);
    if (w) w->hwnd = hwnd;
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  auto* w = reinterpret_cast<Window::Impl*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (!w) return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  if (msg == WM_NCDESTROY) {
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return HandleMessage(w, hwnd, msg, wparam, lparam);
}

HICON LoadAppIcon(int cxMetric, int cyMetric) {
  return (HICON)::LoadImageW(Instance(), MAKEINTRESOURCEW(IDI_QBLANK), IMAGE_ICON,
                             ::GetSystemMetrics(cxMetric), ::GetSystemMetrics(cyMetric),
                             LR_DEFAULTCOLOR);
}

BOOL CALLBACK CollectDisplay(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
  auto* list = reinterpret_cast<std::vector<DisplayInfo>*>(data);
  MONITORINFO info = {};
  info.cbSize = sizeof(info);
  if (!::GetMonitorInfoW(monitor, &info)) return TRUE;
  DisplayInfo display;
  display.rect = {info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right,
                  info.rcMonitor.bottom};
  display.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
  list->push_back(display);
  return TRUE;
}

}  // namespace

// ------------------------------------------------------------------- Window

Window::Window() : impl_(std::make_unique<Impl>()) {
  impl_->owner = this;
}

Window::~Window() {
  // Nothing of the owner is left to hear about its own end.
  impl_->listener = nullptr;
  Destroy();
}

void Window::SetListener(Listener listener) {
  impl_->listener = std::move(listener);
}

CreateResult Window::Create(const WindowSpec& spec) {
  Impl& w = *impl_;
  if (w.hwnd) return CreateResult::Ok;
  w.role = spec.role;
  // From the one name in app_identity.h, so a rename does not leave a window
  // class behind that still says what the program used to be called.
  w.className = WindowClassName(ToWide(spec.id).c_str());
  const HINSTANCE instance = Instance();

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = instance;
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = w.className.c_str();

  DWORD style = WS_OVERLAPPEDWINDOW;
  DWORD exStyle = 0;
  int x = CW_USEDEFAULT;
  int y = CW_USEDEFAULT;
  int width = spec.width;
  int height = spec.height;

  switch (spec.role) {
    case WindowRole::Main: {
      wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
      wc.hbrBackground = nullptr;  // we paint every pixel ourselves
      // Large icon for Alt+Tab, small one for the title bar and taskbar.
      wc.hIcon = LoadAppIcon(SM_CXICON, SM_CYICON);
      wc.hIconSm = LoadAppIcon(SM_CXSMICON, SM_CYSMICON);
      if (!wc.hIcon) wc.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
      if (!::RegisterClassExW(&wc)) return CreateResult::RegistrationFailed;

      RECT rc = {0, 0, spec.width, spec.height};
      ::AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
      width = rc.right - rc.left;
      height = rc.bottom - rc.top;
      // Zwei Gruende, die Stelle nicht zu benutzen, und beide sind echte Fragen.
      // Erstens: es wurde noch nie eine gespeichert. Zweitens: die gespeicherte
      // liegt heute auf keinem Bildschirm mehr -- ein Monitor kann abgezogen worden
      // sein, oder die Anordnung hat sich geaendert, und ein Fenster ausserhalb
      // jeder Arbeitsflaeche waere unerreichbar. Alles andere wird uebernommen,
      // ausdruecklich auch negative Werte: ein Bildschirm links vom Hauptbildschirm
      // hat gar keine anderen.
      if (spec.hasPosition) {
        const RECT want = {spec.x, spec.y, spec.x + width, spec.y + height};
        if (::MonitorFromRect(&want, MONITOR_DEFAULTTONULL) != nullptr) {
          x = spec.x;
          y = spec.y;
        }
      }
      break;
    }

    case WindowRole::Tool: {
      wc.style = CS_HREDRAW | CS_VREDRAW;
      // The same icon as the preview. Without it the taskbar button this window
      // now has -- and its Alt+Tab entry -- would show the generic placeholder.
      wc.hIcon = LoadAppIcon(SM_CXICON, SM_CYICON);
      wc.hIconSm = LoadAppIcon(SM_CXSMICON, SM_CYSMICON);
      // Registering twice is not an error worth failing over; the second call
      // just tells us it is already there.
      ::RegisterClassExW(&wc);

      // Checked against the virtual screen, so a window remembered on a monitor
      // that is no longer there does not come up somewhere nobody can reach it.
      if (spec.hasPosition) {
        RECT desk = {::GetSystemMetrics(SM_XVIRTUALSCREEN), ::GetSystemMetrics(SM_YVIRTUALSCREEN),
                     0, 0};
        desk.right = desk.left + ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
        desk.bottom = desk.top + ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
        // A hundred pixels of title bar has to remain reachable.
        if (spec.x + 100 > desk.left && spec.x < desk.right - 100 && spec.y >= desk.top &&
            spec.y < desk.bottom - 40) {
          x = spec.x;
          y = spec.y;
        }
      }
      // WS_EX_APPWINDOW, so it has a taskbar button and somewhere to go when
      // minimised -- without one it lands as a stub in the bottom left corner of
      // the screen.
      //
      // No owner. An owned window is lifted above its owner every time the owner
      // is activated, so a click into the preview dragged the settings in front
      // of it as well, from wherever they had been left. Unowned, the two stack
      // like any other two windows. What the owner did besides -- close with the
      // preview, stay above it while it is topmost -- App does by hand.
      exStyle = WS_EX_APPWINDOW;
      break;
    }

    case WindowRole::Dialog: {
      wc.hIcon = ::LoadIconW(instance, MAKEINTRESOURCEW(IDI_QBLANK));
      wc.hIconSm = wc.hIcon;
      if (!::RegisterClassExW(&wc)) return CreateResult::RegistrationFailed;
      style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
      if (spec.hasPosition) {
        x = spec.x;
        y = spec.y;
      } else {
        RECT work = {0, 0, 0, 0};
        ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        x = work.left + ((work.right - work.left) - width) / 2;
        y = work.top + ((work.bottom - work.top) - height) / 2;
      }
      break;
    }
  }

  const HWND hwnd = ::CreateWindowExW(exStyle, w.className.c_str(), ToWide(spec.title).c_str(),
                                      style, x, y, width, height, nullptr, nullptr, instance, &w);
  w.hwnd = hwnd;
  if (!hwnd) {
    if (spec.role == WindowRole::Dialog) ::UnregisterClassW(w.className.c_str(), instance);
    return CreateResult::WindowFailed;
  }
  return CreateResult::Ok;
}

bool Window::created() const {
  return impl_->hwnd != nullptr;
}

void Window::Destroy() {
  Impl& w = *impl_;
  if (!w.hwnd) return;
  const HWND hwnd = w.hwnd;
  w.hwnd = nullptr;
  ::DestroyWindow(hwnd);
  if (w.role == WindowRole::Dialog) ::UnregisterClassW(w.className.c_str(), Instance());
}

void Window::ShowFirstTime(bool maximized) {
  const HWND hwnd = impl_->hwnd;
  if (!hwnd) return;
  ::ShowWindow(hwnd, maximized ? SW_SHOWMAXIMIZED : g_startupShowCmd);
  ::UpdateWindow(hwnd);
}

void Window::Show() {
  const HWND hwnd = impl_->hwnd;
  if (!hwnd) return;
  // SW_SHOW displays a minimised window *still minimised*, and the settings then
  // refused to draw it -- so reopening them after minimising them did nothing at
  // all.
  ::ShowWindow(hwnd, ::IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
  ::SetForegroundWindow(hwnd);
}

void Window::Hide() {
  if (impl_->hwnd) ::ShowWindow(impl_->hwnd, SW_HIDE);
}

void Window::Raise() {
  const HWND hwnd = impl_->hwnd;
  if (!hwnd) return;
  if (::IsIconic(hwnd)) ::ShowWindow(hwnd, SW_RESTORE);
  ::SetForegroundWindow(hwnd);
}

void Window::SetTitle(const std::string& title) {
  if (impl_->hwnd) ::SetWindowTextW(impl_->hwnd, ToWide(title).c_str());
}

bool Window::minimized() const {
  return impl_->hwnd && ::IsIconic(impl_->hwnd);
}

bool Window::IsVisible() const {
  return impl_->hwnd && ::IsWindowVisible(impl_->hwnd);
}

bool Window::IsForeground() const {
  return impl_->hwnd && ::GetForegroundWindow() == impl_->hwnd;
}

bool Window::IsCoveredBy(const Window& other) const {
  const HWND mine = impl_->hwnd;
  const HWND theirs = NativeWindow(other);
  RECT a = {}, b = {}, overlap = {};
  if (!mine || !theirs || !::IsWindowVisible(theirs) || ::IsIconic(theirs) ||
      !::GetWindowRect(mine, &a) || !::GetWindowRect(theirs, &b) ||
      !::IntersectRect(&overlap, &a, &b)) {
    return false;
  }
  // Overlapping is not covering: `other` has to be above this window as well.
  // Everything before this window in the z-order is above it.
  for (HWND above = ::GetWindow(mine, GW_HWNDPREV); above;
       above = ::GetWindow(above, GW_HWNDPREV)) {
    if (above == theirs) return true;
  }
  return false;
}

void Window::SetTopmost(bool topmost) {
  const HWND hwnd = impl_->hwnd;
  if (!hwnd) return;
  // Only when it changes. Asserting HWND_TOPMOST again also brings the window to
  // the front of the topmost windows. For the preview: leaving fullscreen from a
  // shortcut pressed in the settings window would then bury that window under
  // the preview. For the settings: doing that every time would put them back
  // over a preview that had just been clicked into.
  const bool now = (::GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
  if (now == topmost) return;
  ::SetWindowPos(hwnd, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void Window::SetDarkFrame(bool dark) {
  const HWND hwnd = impl_->hwnd;
  if (!hwnd) return;
  BOOL value = dark ? TRUE : FALSE;
  if (FAILED(::DwmSetWindowAttribute(hwnd, kDwmUseImmersiveDarkMode, &value, sizeof(value)))) {
    ::DwmSetWindowAttribute(hwnd, kDwmUseImmersiveDarkModeOld, &value, sizeof(value));
  }
}

float Window::DpiScale() const {
  const UINT dpi = impl_->hwnd ? ::GetDpiForWindow(impl_->hwnd) : 0;
  return dpi > 0 ? (float)dpi / 96.0f : 1.0f;
}

bool Window::GetPlacement(Placement* out) const {
  const HWND hwnd = impl_->hwnd;
  if (!hwnd || !out) return false;
  WINDOWPLACEMENT wp = {};
  wp.length = sizeof(wp);
  if (!::GetWindowPlacement(hwnd, &wp)) return false;
  // The restored rectangle, not the current one: a window read while it is
  // minimised or maximised would be remembered at the wrong size.
  const RECT& rc = wp.rcNormalPosition;
  RECT frame = {0, 0, 0, 0};
  ::AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
  out->x = rc.left;
  out->y = rc.top;
  out->width = rc.right - rc.left;
  out->height = rc.bottom - rc.top;
  out->clientWidth = out->width - (frame.right - frame.left);
  out->clientHeight = out->height - (frame.bottom - frame.top);
  out->maximized = wp.showCmd == SW_SHOWMAXIMIZED;
  return true;
}

bool Window::FrameRect(Rect* out) const {
  RECT rc = {};
  if (!impl_->hwnd || !out || !::GetWindowRect(impl_->hwnd, &rc)) return false;
  *out = {rc.left, rc.top, rc.right, rc.bottom};
  return true;
}

Point Window::ClientToScreen(Point p) const {
  POINT pt = {p.x, p.y};
  ::ClientToScreen(impl_->hwnd, &pt);
  return {pt.x, pt.y};
}

Point Window::ScreenToClient(Point p) const {
  POINT pt = {p.x, p.y};
  ::ScreenToClient(impl_->hwnd, &pt);
  return {pt.x, pt.y};
}

void Window::ResizeClientHeightCentred(int clientHeight) {
  const HWND hwnd = impl_->hwnd;
  if (!hwnd) return;
  RECT window = {}, client = {};
  ::GetWindowRect(hwnd, &window);
  ::GetClientRect(hwnd, &client);
  const int chrome = (window.bottom - window.top) - (client.bottom - client.top);
  const int height = clientHeight + chrome;
  const int width = window.right - window.left;

  RECT work = {0, 0, 0, 0};
  ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  const int x = work.left + ((work.right - work.left) - width) / 2;
  const int y = work.top + ((work.bottom - work.top) - height) / 2;
  ::SetWindowPos(hwnd, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

void Window::EnterFullscreen(const Rect& target, const Window* under, bool topmost) {
  Impl& w = *impl_;
  if (!w.hwnd) return;
  w.windowed.length = sizeof(w.windowed);
  ::GetWindowPlacement(w.hwnd, &w.windowed);

  ::SetWindowLongPtrW(w.hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
  HWND after = topmost ? HWND_TOPMOST : HWND_TOP;
  if (under) after = NativeWindow(*under);
  ::SetWindowPos(w.hwnd, after, target.left, target.top, target.width(), target.height(),
                 SWP_FRAMECHANGED | SWP_NOACTIVATE);
}

void Window::LeaveFullscreen() {
  Impl& w = *impl_;
  if (!w.hwnd) return;
  ::SetWindowLongPtrW(w.hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
  ::SetWindowPlacement(w.hwnd, &w.windowed);
  ::SetWindowPos(w.hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
}

bool Window::CurrentDisplayRect(Rect* out) const {
  if (!impl_->hwnd || !out) return false;
  HMONITOR monitor = ::MonitorFromWindow(impl_->hwnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFO info = {};
  info.cbSize = sizeof(info);
  if (!::GetMonitorInfoW(monitor, &info)) return false;
  *out = {info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right, info.rcMonitor.bottom};
  return true;
}

void Window::SetCursorHidden(bool hidden) {
  if (impl_->cursorHidden == hidden) return;
  ::ShowCursor(hidden ? FALSE : TRUE);
  impl_->cursorHidden = hidden;
}

bool Window::cursorHidden() const {
  return impl_->cursorHidden;
}

void Window::RequestClose() {
  if (impl_->hwnd) ::PostMessageW(impl_->hwnd, WM_CLOSE, 0, 0);
}

bool Window::AttachUi(ImGuiContext* context) {
  Impl& w = *impl_;
  if (!w.hwnd) return false;
  bool ok;
  {
    UiScope scope(context);
    ok = ImGui_ImplWin32_Init(w.hwnd);
  }
  if (!ok) return false;
  w.ui = context;
  w.forwardUi = true;
  return true;
}

void Window::DetachUi() {
  Impl& w = *impl_;
  {
    UiScope scope(w.ui);
    // Guarded: a failed attach leaves nothing to shut down, and shutting down a
    // backend that was never started walks a null.
    if (ImGui::GetCurrentContext() && ImGui::GetIO().BackendPlatformUserData) {
      ImGui_ImplWin32_Shutdown();
    }
  }
  w.forwardUi = false;
  w.ui = nullptr;
}

void Window::BeginUiFrame() {
  UiScope scope(impl_->ui);
  ImGui_ImplWin32_NewFrame();
}

// ------------------------------------------------------------ free functions

HWND NativeWindow(const Window& window) {
  return window.impl() ? window.impl()->hwnd : nullptr;
}

void SetStartupShowCommand(int showCmd) {
  g_startupShowCmd = showCmd;
}

const Window* UiWindow() {
  if (!ImGui::GetCurrentContext()) return nullptr;
  const HWND hwnd = (HWND)ImGui::GetMainViewport()->PlatformHandleRaw;
  if (!hwnd) return nullptr;
  // Only a window of ours carries a Window behind it.
  if ((WNDPROC)::GetWindowLongPtrW(hwnd, GWLP_WNDPROC) != WindowProc) return nullptr;
  const auto* w = reinterpret_cast<const Window::Impl*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  return w ? w->owner : nullptr;
}

std::vector<DisplayInfo> EnumerateDisplays() {
  std::vector<DisplayInfo> list;
  ::EnumDisplayMonitors(nullptr, nullptr, CollectDisplay, (LPARAM)&list);
  return list;
}

float SystemUiScale() {
  const UINT dpi = ::GetDpiForSystem();
  return dpi > 0 ? (float)dpi / 96.0f : 1.0f;
}

bool PumpEvents() {
  MSG msg = {};
  while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
    if (msg.message == WM_QUIT) return false;
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
  }
  return true;
}

void WaitForEvents() {
  ::WaitMessage();
}

WaitResult WaitForEventsOr(const WakeSignal* signal, int timeoutMs) {
  HANDLE handles[1] = {nullptr};
  DWORD count = 0;
  if (signal) {
    if (HANDLE event = NativeHandle(*signal)) {
      handles[0] = event;
      count = 1;
    }
  }
  const DWORD result =
      ::MsgWaitForMultipleObjectsEx(count, count ? handles : nullptr,
                                    (DWORD)(timeoutMs > 0 ? timeoutMs : 0), QS_ALLINPUT,
                                    MWMO_INPUTAVAILABLE);
  if (count == 1 && result == WAIT_OBJECT_0) return WaitResult::Signal;
  if (result == WAIT_OBJECT_0 + count) return WaitResult::Input;
  if (result == WAIT_TIMEOUT) return WaitResult::Timeout;
  return WaitResult::Failed;
}

void RequestQuit() {
  ::PostQuitMessage(0);
}

void DiscardPendingQuit() {
  MSG stray;
  while (::PeekMessageW(&stray, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {
  }
}

void ShowErrorMessage(const std::string& text) {
  ::MessageBoxW(nullptr, ToWide(text).c_str(), kAppName, MB_ICONERROR | MB_OK);
}

}  // namespace cap
