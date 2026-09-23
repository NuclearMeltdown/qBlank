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

// "&" marks a mnemonic in a menu label. These labels are plain text, and a
// profile may well be called "Mario & Luigi".
std::wstring MenuText(const std::string& label) {
  std::wstring out;
  for (wchar_t c : ToWide(label)) {
    out += c;
    if (c == L'&') out += L'&';
  }
  return out;
}

HMENU BuildMenu(const std::vector<TrayMenuItem>& items) {
  HMENU menu = ::CreatePopupMenu();
  if (!menu) return nullptr;
  for (const TrayMenuItem& item : items) {
    if (item.id == 0 && item.children.empty()) {
      ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      continue;
    }
    UINT flags = MF_STRING;
    if (item.checked) flags |= MF_CHECKED;
    if (!item.enabled) flags |= MF_GRAYED;
    if (!item.children.empty()) {
      // Belongs to the parent once appended, and is destroyed with it.
      if (HMENU sub = BuildMenu(item.children)) {
        ::AppendMenuW(menu, flags | MF_POPUP, (UINT_PTR)sub, MenuText(item.label).c_str());
      }
      continue;
    }
    ::AppendMenuW(menu, flags, (UINT_PTR)item.id, MenuText(item.label).c_str());
    if (item.isDefault) ::SetMenuDefaultItem(menu, (UINT)item.id, FALSE);
  }
  return menu;
}

DWORD WindowsBuild() {
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
  const auto get =
      ntdll ? reinterpret_cast<RtlGetVersionFn>(::GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
  OSVERSIONINFOW info = {};
  info.dwOSVersionInfoSize = sizeof(info);
  return get && get(&info) == 0 ? info.dwBuildNumber : 0;
}

// There is no documented way to ask for dark menus. uxtheme exports the switch
// by ordinal only, and every program with a dark tray menu goes through these
// two. Ordinal 135 has been SetPreferredAppMode since Windows 10 1903 (build
// 18362); before that it was a different function with a different signature,
// so older builds keep light menus rather than guess.
void ApplyMenuTheme(bool dark) {
  using SetPreferredAppModeFn = int(WINAPI*)(int);
  using FlushMenuThemesFn = void(WINAPI*)();
  static const HMODULE uxtheme =
      WindowsBuild() >= 18362
          ? ::LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)
          : nullptr;
  if (!uxtheme) return;
  static const auto setMode =
      reinterpret_cast<SetPreferredAppModeFn>(::GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
  static const auto flush =
      reinterpret_cast<FlushMenuThemesFn>(::GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));
  if (!setMode) return;
  constexpr int kDefault = 0;
  constexpr int kForceDark = 2;
  setMode(dark ? kForceDark : kDefault);
  if (flush) flush();
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
  std::vector<TrayMenuItem> menu;
  std::vector<int> commands;
  bool dark = false;

  // The tray thread's own.
  HICON icon = nullptr;
  bool menuOpen = false;
  bool closing = false;
  int appliedDark = -1;

  void Run();
  bool AddIcon();
  void UpdateTooltip();
  void OpenMenu(int x, int y);
  void Report(int command);
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
  // reasons: only top-level windows hear TaskbarCreated, and the menu needs a
  // window it can bring to the foreground.
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

void TrayIcon::Impl::OpenMenu(int x, int y) {
  std::vector<TrayMenuItem> items;
  bool wantDark = false;
  {
    std::lock_guard<std::mutex> lock(mutex);
    items = menu;
    wantDark = dark;
  }
  if (items.empty()) return;
  if ((int)wantDark != appliedDark) {
    ApplyMenuTheme(wantDark);
    appliedDark = (int)wantDark;
  }
  const HMENU popup = BuildMenu(items);
  if (!popup) return;

  // Opens away from the taskbar: upwards from a taskbar at the bottom,
  // downwards from one at the top.
  UINT flags = TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY;
  flags |= ::GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
  MONITORINFO monitor = {};
  monitor.cbSize = sizeof(monitor);
  const POINT at = {x, y};
  const bool upperHalf =
      ::GetMonitorInfoW(::MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &monitor) &&
      y < (monitor.rcMonitor.top + monitor.rcMonitor.bottom) / 2;
  flags |= upperHalf ? TPM_TOPALIGN : TPM_BOTTOMALIGN;

  // Both halves of this are documented under TrackPopupMenu: without the
  // window in the foreground a click elsewhere does not close the menu, and
  // without a message afterwards the next one can open and vanish at once.
  ::SetForegroundWindow(hwnd);
  menuOpen = true;
  const int picked = (int)::TrackPopupMenuEx(popup, flags, x, y, hwnd, nullptr);
  menuOpen = false;
  ::PostMessageW(hwnd, WM_NULL, 0, 0);
  ::DestroyMenu(popup);

  if (closing) {
    ::DestroyWindow(hwnd);
    return;
  }
  if (picked > 0) Report(picked);
}

void TrayIcon::Impl::Report(int command) {
  {
    std::lock_guard<std::mutex> lock(mutex);
    commands.push_back(command);
  }
  // Wakes the main loop if it is waiting for input. The command itself waits in
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
          Report(TrayIcon::kActivate);
          break;
        case WM_CONTEXTMENU:
          OpenMenu(GET_X_LPARAM(wp), GET_Y_LPARAM(wp));
          break;
      }
      return 0;
    case kMsgTooltip:
      UpdateTooltip();
      return 0;
    case kMsgClose:
      // Arrives inside the menu's own loop when the menu is open; the window
      // goes once that loop has returned.
      closing = true;
      if (menuOpen) {
        ::EndMenu();
      } else {
        ::DestroyWindow(hwnd);
      }
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
  t.closing = false;
  t.appliedDark = -1;
  {
    std::lock_guard<std::mutex> lock(t.mutex);
    t.tooltip = ToWide(tooltip);
    t.commands.clear();  // picked from the last icon, after it was already gone
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

void TrayIcon::SetMenu(std::vector<TrayMenuItem> items) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->menu = std::move(items);
}

void TrayIcon::SetDarkMenus(bool dark) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->dark = dark;
}

std::vector<int> TrayIcon::TakeCommands() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<int> out;
  out.swap(impl_->commands);
  return out;
}

}  // namespace cap
