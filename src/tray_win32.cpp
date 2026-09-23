#include "tray.h"

#include <windows.h>

#include <shellapi.h>
#include <windowsx.h>  // GET_X_LPARAM

#include <condition_variable>
#include <mutex>
#include <thread>

#include "app_identity.h"
#include "resource.h"
#include "text_win32.h"

namespace cap {
namespace {

constexpr UINT kIconId = 1;

// The icon's callback, and the two requests the main thread posts.
constexpr UINT kMsgIcon = WM_APP + 1;
constexpr UINT kMsgTooltip = WM_APP + 2;
constexpr UINT kMsgClose = WM_APP + 3;

HINSTANCE Instance() {
  return ::GetModuleHandleW(nullptr);
}

// Sent to every top-level window when Explorer has started again. Every
// notification icon went with the old one and has to be put back.
UINT TaskbarCreated() {
  static const UINT message = ::RegisterWindowMessageW(L"TaskbarCreated");
  return message;
}

}  // namespace

struct TrayIcon::Impl {
  std::thread thread;
  DWORD wakeThread = 0;  // the thread Show() was called from

  // Written by the tray thread before it reports back, then left alone until
  // the thread has been joined.
  HWND hwnd = nullptr;

  std::mutex mutex;
  std::condition_variable started;
  bool startDone = false;
  bool startOk = false;
  std::wstring tooltip;
  std::vector<TrayEvent> events;

  // The tray thread's own.
  HICON icon = nullptr;

  void Run();
  bool AddIcon();
  void UpdateTooltip();
  void Report(const TrayEvent& event);
  LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
  static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
};

LRESULT CALLBACK TrayIcon::Impl::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE) {
    auto* impl = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    impl->hwnd = hwnd;
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)impl);
  }
  auto* impl = reinterpret_cast<Impl*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  return impl ? impl->Handle(msg, wp, lp) : ::DefWindowProcW(hwnd, msg, wp, lp);
}

void TrayIcon::Impl::Run() {
  ::SetThreadDescription(::GetCurrentThread(), L"qBlank tray");

  const std::wstring className = WindowClassName(L"Tray");
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &Impl::Proc;
  wc.hInstance = Instance();
  wc.lpszClassName = className.c_str();
  // A second Show after a Hide finds the class already there, which is fine.
  ::RegisterClassExW(&wc);

  icon = (HICON)::LoadImageW(Instance(), MAKEINTRESOURCEW(IDI_QBLANK), IMAGE_ICON,
                             ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON),
                             LR_DEFAULTCOLOR);

  // Never shown. A top-level window rather than a message-only one, for two
  // reasons: only top-level windows hear TaskbarCreated, and a right click has
  // to bring a window of the program to the front, see WM_CONTEXTMENU.
  const HWND created = ::CreateWindowExW(WS_EX_TOOLWINDOW, className.c_str(), L"", WS_POPUP, 0, 0,
                                         0, 0, nullptr, nullptr, Instance(), this);
  // A failure to add the icon is not a failure to start: Explorer may not be up
  // yet this early after logging on, and TaskbarCreated will bring it later.
  if (created) AddIcon();
  {
    std::lock_guard<std::mutex> lock(mutex);
    startDone = true;
    startOk = created != nullptr;
  }
  started.notify_all();

  if (created) {
    MSG msg = {};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
      ::TranslateMessage(&msg);
      ::DispatchMessageW(&msg);
    }
  }
  if (icon) ::DestroyIcon(icon);
  icon = nullptr;
}

bool TrayIcon::Impl::AddIcon() {
  NOTIFYICONDATAW data = {};
  data.cbSize = sizeof(data);
  data.hWnd = hwnd;
  data.uID = kIconId;
  data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
  data.uCallbackMessage = kMsgIcon;
  data.hIcon = icon ? icon : ::LoadIconW(nullptr, IDI_APPLICATION);
  {
    std::lock_guard<std::mutex> lock(mutex);
    wcsncpy_s(data.szTip, tooltip.c_str(), _TRUNCATE);
  }
  if (!::Shell_NotifyIconW(NIM_ADD, &data)) return false;
  // Version 4 is what reports a left click as NIN_SELECT and the menu key or a
  // right click as WM_CONTEXTMENU, with the point to open at in wParam.
  data.uVersion = NOTIFYICON_VERSION_4;
  ::Shell_NotifyIconW(NIM_SETVERSION, &data);
  return true;
}

void TrayIcon::Impl::UpdateTooltip() {
  NOTIFYICONDATAW data = {};
  data.cbSize = sizeof(data);
  data.hWnd = hwnd;
  data.uID = kIconId;
  data.uFlags = NIF_TIP | NIF_SHOWTIP;
  {
    std::lock_guard<std::mutex> lock(mutex);
    wcsncpy_s(data.szTip, tooltip.c_str(), _TRUNCATE);
  }
  ::Shell_NotifyIconW(NIM_MODIFY, &data);
}

void TrayIcon::Impl::Report(const TrayEvent& event) {
  {
    std::lock_guard<std::mutex> lock(mutex);
    events.push_back(event);
  }
  // Wakes the main loop if it is waiting for input. The event itself waits in
  // the list; a wake-up lost to some modal loop only delays it to the next one.
  ::PostThreadMessageW(wakeThread, WM_NULL, 0, 0);
}

LRESULT TrayIcon::Impl::Handle(UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == TaskbarCreated() && msg != 0) {
    AddIcon();
    return 0;
  }
  switch (msg) {
    case kMsgIcon:
      switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
          Report({TrayEvent::Kind::Activate, {}});
          break;
        case WM_CONTEXTMENU:
          // Explorer lets the program that owns the icon come to the front now,
          // and only now. Taken here, by this window: once a window of the
          // program is in front, the menu the main thread opens a moment later
          // may take over from it. Without the front, a click elsewhere would
          // not close the menu.
          ::SetForegroundWindow(hwnd);
          Report({TrayEvent::Kind::Menu, {GET_X_LPARAM(wp), GET_Y_LPARAM(wp)}});
          break;
      }
      return 0;
    case kMsgTooltip:
      UpdateTooltip();
      return 0;
    case kMsgClose:
      ::DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY: {
      NOTIFYICONDATAW data = {};
      data.cbSize = sizeof(data);
      data.hWnd = hwnd;
      data.uID = kIconId;
      ::Shell_NotifyIconW(NIM_DELETE, &data);
      ::PostQuitMessage(0);
      return 0;
    }
  }
  return ::DefWindowProcW(hwnd, msg, wp, lp);
}

TrayIcon::TrayIcon() : impl_(std::make_unique<Impl>()) {}

TrayIcon::~TrayIcon() {
  Hide();
}

bool TrayIcon::Show(const std::string& tooltip) {
  Impl& t = *impl_;
  if (t.thread.joinable()) {
    SetTooltip(tooltip);
    return true;
  }
  t.wakeThread = ::GetCurrentThreadId();
  t.hwnd = nullptr;
  {
    std::lock_guard<std::mutex> lock(t.mutex);
    t.tooltip = ToWide(tooltip);
    t.events.clear();  // from the last icon, after it was already gone
    t.startDone = false;
    t.startOk = false;
  }
  t.thread = std::thread([&t] { t.Run(); });

  std::unique_lock<std::mutex> lock(t.mutex);
  t.started.wait(lock, [&t] { return t.startDone; });
  if (t.startOk) return true;
  lock.unlock();
  t.thread.join();
  return false;
}

void TrayIcon::Hide() {
  Impl& t = *impl_;
  if (!t.thread.joinable()) return;
  if (t.hwnd) ::PostMessageW(t.hwnd, kMsgClose, 0, 0);
  t.thread.join();
  t.hwnd = nullptr;
}

bool TrayIcon::shown() const {
  return impl_->thread.joinable();
}

void TrayIcon::SetTooltip(const std::string& tooltip) {
  Impl& t = *impl_;
  const std::wstring wide = ToWide(tooltip);
  {
    std::lock_guard<std::mutex> lock(t.mutex);
    if (wide == t.tooltip) return;
    t.tooltip = wide;
  }
  if (t.hwnd) ::PostMessageW(t.hwnd, kMsgTooltip, 0, 0);
}

std::vector<TrayEvent> TrayIcon::TakeEvents() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<TrayEvent> out;
  out.swap(impl_->events);
  return out;
}

}  // namespace cap
