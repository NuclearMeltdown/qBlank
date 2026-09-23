#include "ui/theme.h"

#include <algorithm>
#include <cmath>

#include "common.h"
#include "imgui.h"
#include "platform.h"

namespace cap {
namespace {

struct Hsv {
  float h;  // 0..1
  float s;  // 0..1
  float v;  // 0..1
};

Hsv RgbToHsv(unsigned rgb) {
  const float r = ((rgb >> 16) & 0xFF) / 255.0f;
  const float g = ((rgb >> 8) & 0xFF) / 255.0f;
  const float b = (rgb & 0xFF) / 255.0f;
  Hsv out{};
  ImGui::ColorConvertRGBtoHSV(r, g, b, out.h, out.s, out.v);
  return out;
}

// Builds a colour from the accent's hue, with saturation and value given
// outright. Keeping the hue and scaling the rest is what makes every surface
// read as part of the same family.
ImVec4 Tint(const Hsv& accent, float saturation, float value, float alpha = 1.0f) {
  float r = 0.0f, g = 0.0f, b = 0.0f;
  ImGui::ColorConvertHSVtoRGB(accent.h, Clamp(saturation, 0.0f, 1.0f),
                              Clamp(value, 0.0f, 1.0f), r, g, b);
  return ImVec4(r, g, b, alpha);
}

ImVec4 WithAlpha(ImVec4 c, float alpha) {
  c.w = alpha;
  return c;
}

}  // namespace

bool ResolveDark(Theme theme) {
  switch (theme) {
    case Theme::Dark: return true;
    case Theme::Light: return false;
    case Theme::System:
    default: return SystemPrefersDark();
  }
}

void GetBackgroundColor(bool dark, unsigned accentRgb, float out[4]) {
  const Hsv a = RgbToHsv(accentRgb);
  // Deliberately near-black in both themes: this sits behind the video, and a
  // bright surround washes the picture out. Just enough hue to belong.
  const ImVec4 c = dark ? Tint(a, a.s * 0.30f, 0.055f) : Tint(a, a.s * 0.22f, 0.115f);
  out[0] = c.x;
  out[1] = c.y;
  out[2] = c.z;
  out[3] = 1.0f;
}

void ApplyImGuiTheme(bool dark, unsigned accentRgb) {
  const Hsv a = RgbToHsv(accentRgb);
  ImGuiStyle& s = ImGui::GetStyle();

  s.WindowRounding = 10.0f;
  s.ChildRounding = 8.0f;
  s.FrameRounding = 6.0f;
  s.PopupRounding = 8.0f;
  s.GrabRounding = 6.0f;
  s.TabRounding = 7.0f;
  s.ScrollbarRounding = 8.0f;

  s.WindowBorderSize = 1.0f;
  s.ChildBorderSize = 1.0f;
  s.FrameBorderSize = dark ? 0.0f : 1.0f;
  s.PopupBorderSize = 1.0f;
  s.TabBorderSize = 0.0f;

  s.WindowPadding = ImVec2(18, 16);
  s.FramePadding = ImVec2(11, 7);
  s.CellPadding = ImVec2(8, 6);
  s.ItemSpacing = ImVec2(11, 9);
  s.ItemInnerSpacing = ImVec2(8, 6);
  s.IndentSpacing = 20.0f;
  s.ScrollbarSize = 13.0f;
  s.GrabMinSize = 12.0f;
  s.SeparatorTextBorderSize = 1.0f;
  s.SeparatorTextPadding = ImVec2(18, 8);

  s.WindowTitleAlign = ImVec2(0.0f, 0.5f);
  s.ButtonTextAlign = ImVec2(0.5f, 0.5f);

  // Accent at full strength, plus a lighter and a darker step for hover and
  // press states.
  const ImVec4 accent = Tint(a, a.s, a.v);
  const ImVec4 accentHover = Tint(a, a.s * 0.92f, std::min(a.v * 1.14f, 1.0f));
  const ImVec4 accentActive = Tint(a, std::min(a.s * 1.05f, 1.0f), a.v * 0.86f);

  ImVec4 windowBg, panelBg, frameBg, frameHover, frameActive, border, titleBg, headerBg;
  ImVec4 text, textDim, scrollGrab;

  if (dark) {
    // Saturation is pulled right down on surfaces: a fully saturated background
    // at any hue is unusable to look at for hours.
    windowBg = Tint(a, a.s * 0.22f, 0.105f);
    panelBg = Tint(a, a.s * 0.20f, 0.135f);
    frameBg = Tint(a, a.s * 0.18f, 0.175f);
    frameHover = Tint(a, a.s * 0.20f, 0.235f);
    frameActive = Tint(a, a.s * 0.22f, 0.285f);
    border = Tint(a, a.s * 0.18f, 0.265f);
    titleBg = Tint(a, a.s * 0.25f, 0.085f);
    headerBg = Tint(a, a.s * 0.18f, 0.175f);
    text = Tint(a, a.s * 0.05f, 0.925f);
    textDim = Tint(a, a.s * 0.10f, 0.615f);
    scrollGrab = Tint(a, a.s * 0.20f, 0.33f);
  } else {
    windowBg = Tint(a, a.s * 0.06f, 0.965f);
    panelBg = Tint(a, a.s * 0.03f, 1.0f);
    frameBg = Tint(a, a.s * 0.03f, 1.0f);
    frameHover = Tint(a, a.s * 0.10f, 0.945f);
    frameActive = Tint(a, a.s * 0.14f, 0.900f);
    border = Tint(a, a.s * 0.14f, 0.850f);
    titleBg = Tint(a, a.s * 0.10f, 0.915f);
    headerBg = Tint(a, a.s * 0.10f, 0.925f);
    text = Tint(a, a.s * 0.30f, 0.130f);
    textDim = Tint(a, a.s * 0.18f, 0.420f);
    scrollGrab = Tint(a, a.s * 0.16f, 0.760f);
  }

  ImVec4* c = s.Colors;
  c[ImGuiCol_Text] = text;
  c[ImGuiCol_TextDisabled] = textDim;
  c[ImGuiCol_WindowBg] = windowBg;
  c[ImGuiCol_ChildBg] = WithAlpha(panelBg, dark ? 0.5f : 1.0f);
  c[ImGuiCol_PopupBg] = panelBg;
  c[ImGuiCol_Border] = border;
  c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);

  c[ImGuiCol_FrameBg] = frameBg;
  c[ImGuiCol_FrameBgHovered] = frameHover;
  c[ImGuiCol_FrameBgActive] = frameActive;

  c[ImGuiCol_TitleBg] = titleBg;
  c[ImGuiCol_TitleBgActive] = titleBg;
  c[ImGuiCol_TitleBgCollapsed] = WithAlpha(titleBg, 0.75f);
  c[ImGuiCol_MenuBarBg] = panelBg;

  c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ScrollbarGrab] = WithAlpha(scrollGrab, 0.65f);
  c[ImGuiCol_ScrollbarGrabHovered] = WithAlpha(scrollGrab, 0.85f);
  c[ImGuiCol_ScrollbarGrabActive] = scrollGrab;

  c[ImGuiCol_CheckMark] = accent;
  // A ticked box keeps the plain frame: the accent tick already says it is on.
  // Left unset, ImGui's own default blue shows through.
  c[ImGuiCol_CheckboxSelectedBg] = frameBg;
  c[ImGuiCol_SliderGrab] = accent;
  c[ImGuiCol_SliderGrabActive] = accentActive;

  c[ImGuiCol_Button] = frameBg;
  c[ImGuiCol_ButtonHovered] = frameHover;
  c[ImGuiCol_ButtonActive] = frameActive;

  c[ImGuiCol_Header] = headerBg;
  c[ImGuiCol_HeaderHovered] = frameHover;
  c[ImGuiCol_HeaderActive] = WithAlpha(accent, 0.35f);

  c[ImGuiCol_Separator] = border;
  c[ImGuiCol_SeparatorHovered] = WithAlpha(accent, 0.6f);
  c[ImGuiCol_SeparatorActive] = accent;

  c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ResizeGripHovered] = WithAlpha(accent, 0.5f);
  c[ImGuiCol_ResizeGripActive] = WithAlpha(accent, 0.8f);

  c[ImGuiCol_Tab] = frameBg;
  c[ImGuiCol_TabHovered] = WithAlpha(accent, 0.5f);
  c[ImGuiCol_TabSelected] = WithAlpha(accent, 0.85f);
  c[ImGuiCol_TabSelectedOverline] = accent;
  c[ImGuiCol_TabDimmed] = frameBg;
  c[ImGuiCol_TabDimmedSelected] = frameActive;

  c[ImGuiCol_PlotLines] = accent;
  c[ImGuiCol_PlotLinesHovered] = accentHover;
  c[ImGuiCol_PlotHistogram] = accent;
  c[ImGuiCol_PlotHistogramHovered] = accentHover;
  c[ImGuiCol_TextSelectedBg] = WithAlpha(accent, 0.35f);
  // ImGui's default is white, which would vanish in the light theme.
  c[ImGuiCol_InputTextCursor] = text;
  c[ImGuiCol_TreeLines] = border;
  c[ImGuiCol_NavCursor] = accent;
  c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, dark ? 0.55f : 0.35f);
  c[ImGuiCol_TableHeaderBg] = headerBg;
  c[ImGuiCol_TableBorderStrong] = border;
  c[ImGuiCol_TableBorderLight] = WithAlpha(border, 0.5f);
}

void LoadUiFont(float sizePixels) {
  ImGuiIO& io = ImGui::GetIO();

  ImFontConfig cfg;
  cfg.OversampleH = 2;
  cfg.OversampleV = 1;
  cfg.PixelSnapH = false;

  // ImGui's default range stops at the end of Latin-1, which covers the umlauts
  // but not the punctuation the interface actually uses. An em dash outside the
  // baked range does not fall back to anything -- it renders as "?", which is
  // how "— nothing selected —" ended up looking like an error message.
  static const ImWchar ranges[] = {
      0x0020, 0x00FF,  // Basic Latin + Latin-1 Supplement
      0x2010, 0x203A,  // dashes, quotation marks, ellipsis, angle quotes
      0x20AC, 0x20AC,  // euro sign
      0,
  };

  // ImGui takes UTF-8 paths, which is what the interface deals in anyway.
  for (const std::string& path : UiFontFiles()) {
    if (io.Fonts->AddFontFromFileTTF(path.c_str(), sizePixels, &cfg, ranges)) return;
  }
  CAP_WARN("Segoe UI not found, using the built-in font");
}

}  // namespace cap
