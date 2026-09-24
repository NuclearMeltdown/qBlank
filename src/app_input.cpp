// Keys and window messages.

#include "app.h"

#include "imgui_internal.h"

namespace cap {
namespace {

// One wheel notch.
const float kVolumeStep = 0.05f;

}  // namespace

bool App::HandleKeyDown(Key key, bool ctrl, bool shift, bool alt) {
  // Fixed on purpose, see hotkeys.h: a rebindable Escape is a way to lock
  // yourself into fullscreen, and the profile digits are a block, not a key.
  if (ctrl && key >= Key::Digit1 && key <= Key::Digit9) {
    SwitchProfile((int)key - (int)Key::Digit1);
    return true;
  }
  if (cropTool_.active()) {
    if (key == Key::Escape) {
      cropTool_.EndCropPick(false);
      return true;
    }
    if (key == Key::Enter) {
      cropTool_.EndCropPick(true);
      return true;
    }
    return true;  // swallow everything else: one job at a time
  }

  if (key == Key::Escape) {
    if (settings_.isOpen()) {
      settings_.Close();
    } else if (fullscreen_) {
      SetFullscreen(false);
    }
    return true;
  }

  switch (config_.hotkeys.Find(key, ctrl, shift, alt)) {
    case HotkeyAction::Fullscreen:
      ToggleFullscreen();
      return true;
    case HotkeyAction::Settings:
      // A settings window lost behind the preview is fetched, not closed: the
      // key was pressed to see it, and closing what cannot be seen is the one
      // thing that cannot be what was meant.
      if (!settings_.isOpen()) {
        OpenSettings({});
      } else if (!settingsHost_.RaiseIfCoveredBy(window_)) {
        settings_.Close();
      }
      return true;
    case HotkeyAction::Stats:
      config_.app.showStats = !config_.app.showStats;
      return true;
    case HotkeyAction::RestartCapture:
      RestartAll(true);
      return true;
    case HotkeyAction::ReinitCard:
      ReinitialiseCard();
      return true;
    case HotkeyAction::Record:
      recording_.ToggleRecording();
      return true;
    case HotkeyAction::Screenshot:
      RequestScreenshot();
      return true;
    case HotkeyAction::ScreenshotClipboard:
      RequestScreenshot(true);
      return true;
    case HotkeyAction::Freeze:
      ToggleFreeze();
      return true;
    case HotkeyAction::Compare:
      ToggleCompare();
      return true;
    case HotkeyAction::BypassFilters:
      ToggleBypass();
      return true;
    case HotkeyAction::DetectCrop:
      cropTool_.DetectCrop();
      return true;
    case HotkeyAction::DetectStandard:
      standardSearch_.RescanVideoStandard();
      return true;
    case HotkeyAction::RemeasureRange:
      RemeasureRange();
      return true;
    case HotkeyAction::Mute:
      ToggleMute();
      return true;
    case HotkeyAction::VolumeUp:
      AdjustVolume(kVolumeStep);
      return true;
    case HotkeyAction::VolumeDown:
      AdjustVolume(-kVolumeStep);
      return true;
    default:
      break;
  }

  // The numeric keypad follows the main volume keys without needing its own
  // binding -- nobody expects to have to bind both.
  if (key == Key::NumAdd && config_.hotkeys[HotkeyAction::VolumeUp].bound()) {
    AdjustVolume(kVolumeStep);
    return true;
  }
  if (key == Key::NumSubtract && config_.hotkeys[HotkeyAction::VolumeDown].bound()) {
    AdjustVolume(-kVolumeStep);
    return true;
  }
  return false;
}

bool App::OnKey(Key key, bool ctrl, bool shift, bool alt, bool busy) {
  // The binding editor wants the raw key, before anyone acts on it -- that is
  // the whole point of it being open. From either window: in the separate one
  // it used to wait for a key that never reached it.
  if (settings_.waitingForKey()) {
    settings_.OfferKey(key, ctrl, shift, alt);
    return true;
  }
  // Typing, or a list is open: the key is ImGui's, Esc included -- it cancels
  // the edit or closes the list before it closes anything bigger.
  if (busy) return false;
  return HandleKeyDown(key, ctrl, shift, alt);
}

bool App::OnWindowEvent(const WindowEvent& e) {
  switch (e.kind) {
    case WindowEvent::Kind::ModalFrame:
      // Same as for the settings window: the platform takes the loop away while
      // a window is being dragged, and this is what still gets through.
      if (!inModalFrame_) {
        inModalFrame_ = true;
        Tick();
        RenderFrame();
        inModalFrame_ = false;
      }
      return true;
    case WindowEvent::Kind::Resized:
      minimized_ = e.minimized;
      if (!minimized_) display_.Resize();
      return true;
    case WindowEvent::Kind::DpiChanged:
      // Can arrive in the middle of a frame -- a SetWindowPos from the UI that
      // lands the window on another monitor sends it right away -- so the
      // interface is rescaled before the next frame, not here.
      pendingUiScale_ = e.dpiScale;
      return false;

    case WindowEvent::Kind::MouseMoved:
      if (e.mouse.x != lastMousePos_.x || e.mouse.y != lastMousePos_.y) {
        lastMousePos_ = e.mouse;
        lastMouseMoveQpc_ = ClockTicks();
        window_.SetCursorHidden(false);
      }
      return true;

    case WindowEvent::Kind::MouseWheel:
      // Only over the picture: inside the settings window the wheel scrolls.
      if (config_.app.wheelVolume && imguiReady_ && !ImGui::GetIO().WantCaptureMouse) {
        if (e.wheelNotches != 0) {
          AdjustVolume(kVolumeStep * (float)e.wheelNotches);
          return true;
        }
      }
      return false;

    case WindowEvent::Kind::KeyDown: {
      // Not WantCaptureKeyboard, which keyboard navigation holds true whenever
      // the embedded panel has focus -- see SettingsHost::OnWindowEvent.
      const bool busy = imguiReady_ &&
                        (ImGui::GetIO().WantTextInput ||
                         ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId |
                                                    ImGuiPopupFlags_AnyPopupLevel));
      return OnKey(e.key, e.ctrl, e.shift, e.alt, busy);
    }

    case WindowEvent::Kind::ScreenSaverStarting:
      // Block the screensaver from starting over our window.
      return config_.app.preventSleep;

    case WindowEvent::Kind::DisplaysChanged:
    case WindowEvent::Kind::DevicesChanged:
      settings_.InvalidateDeviceLists();
      return false;

    case WindowEvent::Kind::ThemeChanged:
      if (config_.app.theme == Theme::System) ApplyTheme();
      return false;

    case WindowEvent::Kind::CloseRequested:
      SaveConfig();
      running_ = false;
      RequestQuit();
      return true;

    // Windows shutting down or logging off ends the process as soon as this
    // returns, without the way out through main -- the log would take that for
    // a crash at the next start.
    case WindowEvent::Kind::SessionEnd:
      if (e.ending) {
        SaveConfig();
        CAP_LOG("Windows session ending");
        LogEnd();
      }
      return true;

    case WindowEvent::Kind::Destroyed:
      running_ = false;
      RequestQuit();
      return true;

    default:
      return false;
  }
}

}  // namespace cap
