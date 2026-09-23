#include "ui/tray_menu.h"

#include <algorithm>
#include <cmath>

#include "app_files.h"
#include "common.h"
#include "i18n.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "ui/theme.h"

namespace cap {
namespace {

// Frames built for one that is shown. Dear ImGui sizes a window to what it held
// the frame before, and a menu's columns follow what they held the frame before
// that, so a menu that has just changed takes a few to settle. Building one
// without drawing it costs next to nothing.
constexpr int kMaxRounds = 6;

// `top` laid over `below`, which is opaque.
ImVec4 Over(const ImVec4& top, const ImVec4& below) {
  const float a = top.w;
  return ImVec4(top.x * a + below.x * (1.0f - a), top.y * a + below.y * (1.0f - a),
                top.z * a + below.z * (1.0f - a), 1.0f);
}

unsigned ToRgb(const ImVec4& c) {
  const auto channel = [](float v) {
    return (unsigned)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f);
  };
  return channel(c.x) << 16 | channel(c.y) << 8 | channel(c.z);
}

}  // namespace

TrayMenu::~TrayMenu() { Destroy(); }

void TrayMenu::SetItems(std::vector<TrayMenuItem> items) {
  if (items == items_) return;
  items_ = std::move(items);
  changed_ = true;
}

bool TrayMenu::Create(bool allowTearing, std::string* error) {
  // A device of its own, for the reasons the settings window has one
  // (ui/ui_surface.h): the preview's short queue stays the preview's.
  if (!surface_.CreateDevice(error)) return false;

  WindowSpec spec;
  spec.role = WindowRole::Popup;
  spec.id = "TrayMenu";
  spec.title = AppNameUtf8();
  // Sized by what it holds, once it knows.
  spec.width = 1;
  spec.height = 1;
  window_.SetListener([this](const WindowEvent& e) { return OnWindowEvent(e); });
  if (window_.Create(spec) != CreateResult::Ok) {
    surface_.ReleaseDevice();
    return ReportError(error, CAP_SAID(T("Das Menü des Symbols konnte nicht erstellt werden",
                                         "The icon's menu could not be created")));
  }
  if (!surface_.Attach(window_, allowTearing, error)) {
    Destroy();
    return false;
  }

  // Its own context and atlas, as with the settings window: a texture does not
  // carry over to another device.
  ImGuiContext* previous = ImGui::GetCurrentContext();
  imgui_ = ImGui::CreateContext();
  ImGui::SetCurrentContext(imgui_);
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  LoadUiFont();  // into the atlas of the current context, so after switching to it
  const bool ok = window_.AttachUi(imgui_) && surface_.InitUi();
  ImGui::SetCurrentContext(previous);
  if (!ok) {
    Destroy();
    return ReportError(error, CAP_SAID(T("ImGui für das Menü des Symbols fehlgeschlagen",
                                         "ImGui failed for the icon's menu")));
  }
  themeApplied_ = false;
  CAP_LOG("Tray menu created");
  return true;
}

void TrayMenu::Destroy() {
  Close();
  if (imgui_) {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    if (previous == imgui_) previous = nullptr;
    ImGui::SetCurrentContext(imgui_);
    surface_.ShutdownUi();
    window_.DetachUi();
    ImGui::DestroyContext(imgui_);
    imgui_ = nullptr;
    ImGui::SetCurrentContext(previous);
  }
  surface_.ReleaseSwapchain();
  window_.Destroy();
  surface_.ReleaseDevice();
  themeApplied_ = false;
}

bool TrayMenu::Open(Point at, bool allowTearing, std::string* error) {
  Close();
  if (!imgui_ && !Create(allowTearing, error)) return false;

  bool found = false;
  for (const DisplayInfo& d : EnumerateDisplays()) {
    const bool inside =
        at.x >= d.rect.left && at.x < d.rect.right && at.y >= d.rect.top && at.y < d.rect.bottom;
    if (inside || (!found && d.primary)) {
      screen_ = d.rect;
      work_ = d.work.width() > 0 && d.work.height() > 0 ? d.work : d.rect;
      found = true;
      if (inside) break;
    }
  }
  if (!found) {
    screen_ = {at.x - 1000, at.y - 1000, at.x + 1000, at.y + 1000};
    work_ = screen_;
  }

  anchor_ = at;
  origin_ = at;
  guardPick_ = false;
  expandedId_ = 0;
  changed_ = true;
  shown_ = false;
  open_ = true;
  lastDrawTick_ = 0;
  // Onto that screen while still hidden, so the menu is measured at its scaling.
  window_.SetFrameRect({at.x, at.y, at.x + 1, at.y + 1});
  return true;
}

void TrayMenu::Close() {
  // Hiding takes the focus away, which closes the menu: open_ goes first.
  if (!open_) return;
  open_ = false;
  shown_ = false;
  window_.Hide();
}

bool TrayMenu::OnWindowEvent(const WindowEvent& e) {
  if (!imgui_) return false;
  switch (e.kind) {
    case WindowEvent::Kind::FocusChanged:
      // A click anywhere else, as with any menu.
      if (!e.focused) Close();
      return false;
    case WindowEvent::Kind::KeyDown:
      if (e.key != Key::Escape) return false;
      Close();
      return true;
    case WindowEvent::Kind::CloseRequested:
      Close();
      return true;
    case WindowEvent::Kind::Resized:
      if (!e.minimized) surface_.Resize();
      return true;
    case WindowEvent::Kind::DpiChanged:
      // Draw compares the scaling every time; this only makes sure it draws.
      changed_ = true;
      return false;
    default:
      return false;
  }
}

void TrayMenu::ApplyTheme(bool dark, unsigned accent, float scale) {
  ApplyImGuiTheme(dark, accent, scale);
  ImGuiStyle& s = ImGui::GetStyle();
  // The window is the menu. Its corners are the window's, and so is the outline
  // where the platform draws one; then it is given the colour of the one Dear
  // ImGui draws around the main window's menus.
  const ImVec4 bg = Over(s.Colors[ImGuiCol_PopupBg], ImVec4(0, 0, 0, 1));
  outlined_ = window_.SetOutlineColor(ToRgb(Over(s.Colors[ImGuiCol_Border], bg)));
  s.Colors[ImGuiCol_WindowBg] = bg;
  s.WindowRounding = 0.0f;
  s.WindowBorderSize = outlined_ ? 0.0f : s.PopupBorderSize;
  themeApplied_ = true;
  themeDark_ = dark;
  themeAccent_ = accent;
  themeScale_ = scale;
}

int TrayMenu::DrawItems(const std::vector<TrayMenuItem>& items) {
  int picked = 0;
  for (size_t i = 0; i < items.size(); ++i) {
    const TrayMenuItem& item = items[i];
    ImGui::PushID((int)i);
    if (item.id == 0 && item.children.empty()) {
      ImGui::Separator();
    } else if (item.children.empty()) {
      if (ImGui::MenuItem(item.label.c_str(), nullptr, item.checked, item.enabled) &&
          !(guardPick_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left))) {
        picked = item.id;
      }
    } else {
      // A row like BeginMenu's, arrow and all, that unfolds below itself.
      ImGuiWindow* window = ImGui::GetCurrentWindow();
      const float x = window->DC.CursorPos.x;
      const float textY = window->DC.CursorPos.y + window->DC.CurrLineTextBaseOffset;
      const float width = window->DC.MenuColumns.DeclColumns(
          0.0f, ImGui::CalcTextSize(item.label.c_str(), nullptr, true).x, 0.0f,
          IM_TRUNC(ImGui::GetFontSize() * 1.20f));
      const float stretch = ImMax(0.0f, ImGui::GetContentRegionAvail().x - width);
      const bool expanded = expandedId_ == item.id;
      if (ImGui::MenuItem(item.label.c_str(), nullptr, false, item.enabled)) {
        expandedId_ = expanded ? 0 : item.id;
        changed_ = true;
        // The menu grows or shrinks away from the taskbar, and another row
        // comes to lie under the pointer. The second click of a double click
        // is not meant for it.
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        guardX_ = origin_.x + mouse.x;
        guardY_ = origin_.y + mouse.y;
        guardPick_ = true;
      }
      ImGui::RenderArrow(
          window->DrawList,
          ImVec2(x + window->DC.MenuColumns.OffsetMark + stretch + ImGui::GetFontSize() * 0.30f,
                 textY),
          ImGui::GetColorU32(item.enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled),
          expanded ? ImGuiDir_Down : ImGuiDir_Right);
      if (expanded) {
        ImGui::Indent();
        if (const int child = DrawItems(item.children)) picked = child;
        ImGui::Unindent();
      }
    }
    ImGui::PopID();
  }
  return picked;
}

int TrayMenu::BuildFrame(int* width, int* height) {
  surface_.NewUiFrame();
  window_.BeginUiFrame();
  ImGuiIO& io = ImGui::GetIO();
  // Dear ImGui never lets a window grow past what it draws into, and this one
  // only grows after the menu has said how large it wants to be. So the menu is
  // measured against the screen.
  io.DisplaySize.x = std::max(io.DisplaySize.x, (float)work_.width());
  io.DisplaySize.y = std::max(io.DisplaySize.y, (float)work_.height());
  // Several frames in one go, microseconds apart.
  if (io.DeltaTime <= 0.0f) io.DeltaTime = 1.0f / 10000.0f;
  ImGui::NewFrame();

  // A pointer that moved on the screen, not only against a window that moved
  // under it, lets a click pick again.
  if (guardPick_ && ImGui::IsMousePosValid() &&
      (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
    const float dx = origin_.x + io.MousePos.x - guardX_;
    const float dy = origin_.y + io.MousePos.y - guardY_;
    if (dx * dx + dy * dy > 4.0f) guardPick_ = false;
  }

  ImGui::SetNextWindowPos(ImVec2(0, 0));
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoSavedSettings;
  int picked = 0;
  if (ImGui::Begin("##traymenu", nullptr, flags)) picked = DrawItems(items_);
  const ImVec2 size = ImGui::GetWindowSize();
  ImGui::End();
  ImGui::Render();

  *width = std::max(1, (int)std::ceil(size.x));
  *height = std::max(1, (int)std::ceil(size.y));
  return picked;
}

void TrayMenu::Place(int width, int height) {
  // Away from the edge the icon sits at: downwards from a taskbar at the top,
  // upwards from one at the bottom, and to the left where the right runs out.
  int x = anchor_.x;
  int y = anchor_.y < (screen_.top + screen_.bottom) / 2 ? anchor_.y : anchor_.y - height;
  if (x + width > work_.right) x = anchor_.x - width;
  x = std::clamp(x, work_.left, std::max(work_.left, work_.right - width));
  y = std::clamp(y, work_.top, std::max(work_.top, work_.bottom - height));
  window_.SetFrameRect({x, y, x + width, y + height});
  origin_ = {x, y};
}

int TrayMenu::Draw(bool dark, unsigned accent) {
  if (!open_ || !imgui_ || !surface_.hasTarget()) return 0;
  // The same ceiling as the settings window: far above any refresh rate, and
  // still a ceiling.
  const uint32_t now = TickMilliseconds();
  if (lastDrawTick_ != 0 && now - lastDrawTick_ < 4) return 0;
  lastDrawTick_ = now;

  ImGuiContext* previous = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(imgui_);

  const float scale = window_.DpiScale();
  if (!themeApplied_ || dark != themeDark_ || accent != themeAccent_ || scale != themeScale_) {
    ApplyTheme(dark, accent, scale);
    changed_ = true;
  }

  // Frames that are only built, until the size has settled; then one is shown.
  int picked = 0;
  int settle = changed_ || !shown_ ? 2 : 0;
  changed_ = false;
  for (int round = 0; round < kMaxRounds; ++round) {
    int width = 0, height = 0;
    picked = BuildFrame(&width, &height);
    if (picked) break;
    if (changed_) {  // a row unfolded
      settle = 2;
      changed_ = false;
    }
    const bool last = round + 1 == kMaxRounds;
    if (width != surface_.width() || height != surface_.height()) {
      Place(width, height);  // the surface follows from the Resized event
      if (!last) continue;
    }
    if (settle > 0 && !last) {
      --settle;
      continue;
    }

    const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    const float clear[4] = {bg.x, bg.y, bg.z, 1.0f};
    surface_.Present(clear);
    if (!shown_) {
      // Only now, with the frame already in place: shown any earlier, it would
      // come up with whatever it showed last time.
      window_.Show();
      shown_ = true;
    }
    break;
  }

  ImGui::SetCurrentContext(previous);
  return picked;
}

}  // namespace cap
