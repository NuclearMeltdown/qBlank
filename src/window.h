#pragma once

// A top-level window and what happens to it, without saying whose windows they
// are. The Windows half is window_win32.cpp; window_win32.h hands the native
// handle to the few places that pass it on to a Windows API themselves.
//
// The window keeps what is the platform's business: registering it, its frame,
// the loop Windows runs of its own while a window is dragged, the pointer. What
// the program decides -- what a key does, when to draw, what closing means --
// arrives as a WindowEvent and is answered through the listener.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "keys.h"

struct ImGuiContext;

namespace cap {

class WakeSignal;

struct Point {
  int x = 0;
  int y = 0;
};

// Screen coordinates; right and bottom lie just outside.
struct Rect {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  int width() const { return right - left; }
  int height() const { return bottom - top; }
};

enum class WindowRole {
  // The picture. Its size is the inside of the window, frame not counted; a
  // remembered position is only used while the window would still be on a
  // screen. It cannot be made smaller than 320 x 240.
  Main,
  // A window of the program's own next to the picture -- the settings -- with
  // its own taskbar entry. Its size is the whole window; a remembered position
  // is only used while a hundred pixels of its title bar stay reachable.
  Tool,
  // A question on its own, before there is a main window. Fixed size; centred
  // on the primary screen's work area unless a position is given.
  Dialog,
  // A menu of the program's own, drawn by it: no frame, no taskbar button,
  // above everything, with the shadow and corners the platform gives its own
  // menus. Placed with SetFrameRect.
  Popup,
};

struct WindowSpec {
  WindowRole role = WindowRole::Main;
  // Tells the program's windows apart from each other and from every other
  // program's: "MainWindow", "SettingsWindow".
  std::string id;
  std::string title;
  int width = 0;
  int height = 0;
  bool hasPosition = false;
  int x = 0;
  int y = 0;
  // Main only: see Window::SetBorderless.
  bool borderless = false;
};

enum class CreateResult {
  Ok,
  RegistrationFailed,  // the platform refused the kind of window, before there was one
  WindowFailed,
};

struct WindowEvent {
  enum class Kind {
    Resized,          // minimized
    Moved,
    DpiChanged,       // dpiScale
    FocusChanged,     // focused
    // The platform holds the thread in a loop of its own -- Windows does while
    // a window is dragged or resized -- so the program's loop is not running.
    // Whatever has to keep moving meanwhile is done from here.
    ModalFrame,
    MouseMoved,       // mouse, inside the window
    MouseWheel,       // wheelNotches, away from the user positive
    KeyDown,          // key, ctrl, shift, alt; also for keys Key does not list
    CloseRequested,
    Destroyed,
    SessionEnd,       // ending: the session is over and the process with it
    DevicesChanged,
    DisplaysChanged,
    // The system's settings may have changed the theme. Also arrives when they
    // changed something else.
    ThemeChanged,
    ScreenSaverStarting,
  };

  Kind kind = Kind::Resized;
  bool minimized = false;
  bool focused = false;
  Point mouse;
  int wheelNotches = 0;
  float dpiScale = 1.0f;  // 1.0 at 96 DPI
  Key key = Key::None;
  bool ctrl = false;
  bool shift = false;
  bool alt = false;
  bool ending = false;
};

class Window {
 public:
  // True when the program dealt with the event. That matters for some:
  //   CloseRequested       -- otherwise the platform closes the window itself
  //   KeyDown, MouseWheel  -- otherwise the platform's default for the key
  //   ScreenSaverStarting  -- true holds the screen saver off
  // and is merely noted for the rest.
  using Listener = std::function<bool(const WindowEvent&)>;

  Window();
  ~Window();

  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  // Before Create, to hear what creating the window already sends -- the
  // first size, for one.
  void SetListener(Listener listener);

  CreateResult Create(const WindowSpec& spec);
  bool created() const;
  void Destroy();

  // The first time the main window appears: maximised, or however the program
  // was asked to start.
  void ShowFirstTime(bool maximized);
  // Visible and in front; restored first if it was minimised.
  void Show();
  void Hide();
  // In front; restored first if it was minimised.
  void Raise();
  void SetTitle(const std::string& title);

  bool minimized() const;
  bool maximized() const;
  bool IsVisible() const;
  bool IsForeground() const;
  // True when `other` is visible, overlaps this window and lies above it.
  bool IsCoveredBy(const Window& other) const;

  // Acts only on a change.
  void SetTopmost(bool topmost);
  void SetDarkFrame(bool dark);
  // How much larger than at 96 dpi the screen the window is on draws.
  float DpiScale() const;

  // Where the window stands when it is neither minimised nor maximised, which is
  // what is worth remembering.
  struct Placement {
    int x = 0;
    int y = 0;
    int width = 0;         // the whole window
    int height = 0;
    int clientWidth = 0;   // the same without the frame
    int clientHeight = 0;
    bool maximized = false;
  };
  bool GetPlacement(Placement* out) const;

  // The whole window as it stands now, in screen coordinates.
  bool FrameRect(Rect* out) const;
  Point ClientToScreen(Point p) const;
  Point ScreenToClient(Point p) const;
  // Gives the inside of the window this height, keeps the width, and centres it
  // on the primary screen's work area.
  void ResizeClientHeightCentred(int clientHeight);
  // Gives the inside of the window this size, restoring it first if it is
  // maximised. The top left corner stays unless that would push the window off
  // the work area of its screen.
  void SetClientSize(int width, int height);
  // Whether the window, frame and all, would fit that screen's work area with
  // the inside at this size.
  bool ClientSizeFits(int width, int height) const;
  // The whole window at `rect`, in screen coordinates, as it is: nothing is
  // kept on a screen or restored.
  void SetFrameRect(const Rect& rect);
  // Popup only: the colour, 0xRRGGBB, of the thin line the platform draws
  // around the window. False when it draws none, and the window has to draw
  // its own.
  bool SetOutlineColor(unsigned rgb);
  // Main window only: while Shift is held, resizing keeps the inside below the
  // top `inset` pixels at this width over height. 0 turns it off.
  void SetSizingAspect(double aspect, int inset);

  // Main window only: no title bar and no frame, the inside is the whole
  // window. It stays a window in every other respect -- taskbar button, Alt+Tab,
  // snapping, minimising -- and is sized at its edges. The inside keeps its
  // place on screen when this is switched.
  void SetBorderless(bool borderless);
  // Moves the window with the pointer for as long as the left button stays
  // down, as dragging a title bar would. For the borderless window, which has
  // none; call it while the button is held. `grabbed` is where it went down, in
  // screen coordinates: that point of the window follows the pointer.
  void BeginMoveDrag(Point grabbed);
  // Main window only: a red dot on the taskbar button. `description` is what a
  // screen reader says about it.
  void SetTaskbarBadge(bool shown, const std::string& description);

  // Borderless over `target`. `under`, when given, is the window it goes
  // directly beneath; otherwise it goes to the front, above everything if
  // `topmost`.
  void EnterFullscreen(const Rect& target, const Window* under, bool topmost);
  void LeaveFullscreen();
  // The screen the window is on, or most of it.
  bool CurrentDisplayRect(Rect* out) const;

  // Over this window only.
  void SetCursorHidden(bool hidden);
  bool cursorHidden() const;

  // As if the user had closed it: CloseRequested follows.
  void RequestClose();

  // Dear ImGui's platform half for this window. With `context` its input goes
  // there, whatever context is current; without, to the current one.
  bool AttachUi(ImGuiContext* context);
  void DetachUi();
  void BeginUiFrame();

  // The platform half behind it, for platform code that hands the window to an
  // API of its own. Defined there.
  struct Impl;
  const Impl* impl() const { return impl_.get(); }

 private:
  std::unique_ptr<Impl> impl_;
};

// The window the current Dear ImGui context draws into, for a dialog that wants
// an owner from code that does not know which window it is in. Null when it is
// none of ours.
const Window* UiWindow();

struct DisplayInfo {
  Rect rect;
  Rect work;  // without the taskbar and whatever else is docked to the edges
  bool primary = false;
};
// In the platform's order, which is the order the settings number them in.
std::vector<DisplayInfo> EnumerateDisplays();

// The scaling of the primary screen, before there is a window to ask.
float SystemUiScale();

// Delivers everything waiting for this thread's windows. False when the program
// has been asked to quit; that request is used up.
bool PumpEvents();
// Sleeps until there is something to deliver.
void WaitForEvents();

enum class WaitResult { Signal, Input, Timeout, Failed };
// Sleeps until there is something to deliver, `signal` is given, or the time
// runs out. Null or unusable signals are simply not waited for.
WaitResult WaitForEventsOr(const WakeSignal* signal, int timeoutMs);

// Makes the next PumpEvents return false.
void RequestQuit();
// Drops a quit request nobody is meant to see.
void DiscardPendingQuit();

// A message nobody can miss, for failures before or instead of a window.
void ShowErrorMessage(const std::string& text);

}  // namespace cap
