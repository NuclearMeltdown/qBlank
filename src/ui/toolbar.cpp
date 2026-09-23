#include "ui/toolbar.h"

#include <cmath>

#include "i18n.h"
#include "imgui.h"

namespace cap {
namespace {

// One button is a square of this many text-line heights, so the bar scales with
// the interface font instead of being pinned to a pixel size. Kept close to the
// text height: this is a convenience strip over a picture, and every pixel it
// takes is a pixel the picture does not get.
float ButtonSide() {
  return ImGui::GetFontSize() * 1.25f;
}

// Vertical breathing room above and below the buttons.
float BarPadding() {
  return ImGui::GetFontSize() * 0.2f;
}

ImU32 Rgb(unsigned rgb, float alpha) {
  return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF,
                  (int)(alpha * 255.0f));
}

// ---- icons -----------------------------------------------------------------
//
// Each takes the centre of its button and a radius, so they all end up the same
// visual weight. Drawn rather than typeset: see the header.

void IconRecord(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
  dl->AddCircleFilled(c, r * 0.62f, col, 24);
}

void IconStop(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
  const float h = r * 0.52f;
  dl->AddRectFilled(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), col, r * 0.12f);
}

void IconCamera(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
  const float w = r * 0.85f;
  const float h = r * 0.62f;
  // Body, with the little raised section on top of a camera.
  dl->AddRect(ImVec2(c.x - w, c.y - h * 0.55f), ImVec2(c.x + w, c.y + h),
              col, r * 0.18f, r * 0.16f);
  dl->AddRectFilled(ImVec2(c.x - w * 0.42f, c.y - h * 0.95f),
                    ImVec2(c.x - w * 0.02f, c.y - h * 0.5f), col, r * 0.08f);
  dl->AddCircle(ImVec2(c.x, c.y + h * 0.18f), r * 0.3f, col, 20, r * 0.16f);
}

// Three sliders. A gear at this size turns into a blob; sliders stay readable
// down to sixteen pixels and mean the same thing.
void IconSettings(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
  const float w = r * 0.8f;
  const float t = r * 0.15f;
  const float rows[3] = {c.y - r * 0.48f, c.y, c.y + r * 0.48f};
  const float knobs[3] = {c.x - w * 0.35f, c.x + w * 0.3f, c.x - w * 0.1f};
  for (int i = 0; i < 3; ++i) {
    dl->AddLine(ImVec2(c.x - w, rows[i]), ImVec2(c.x + w, rows[i]), col, t);
    dl->AddCircleFilled(ImVec2(knobs[i], rows[i]), r * 0.24f, col, 16);
  }
}

void IconFolder(ImDrawList* dl, ImVec2 c, float r, ImU32 col, bool film) {
  const float w = r * 0.85f;
  const float h = r * 0.6f;
  // Tab along the top edge, then the body.
  dl->AddRectFilled(ImVec2(c.x - w, c.y - h), ImVec2(c.x - w * 0.15f, c.y - h * 0.62f),
                    col, r * 0.1f);
  dl->AddRect(ImVec2(c.x - w, c.y - h * 0.72f), ImVec2(c.x + w, c.y + h), col, r * 0.16f,
              r * 0.16f);
  // The two folders are otherwise the same shape, so the mark inside has to do
  // the distinguishing: a play triangle for recordings, an aperture for stills.
  if (film) {
    dl->AddTriangleFilled(ImVec2(c.x - r * 0.14f, c.y - r * 0.14f),
                          ImVec2(c.x - r * 0.14f, c.y + r * 0.26f),
                          ImVec2(c.x + r * 0.22f, c.y + r * 0.06f), col);
  } else {
    dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.06f), r * 0.2f, col, 16);
  }
}

void IconSpeaker(ImDrawList* dl, ImVec2 c, float r, ImU32 col, bool muted) {
  const float w = r * 0.75f;
  const float h = r * 0.55f;
  // Cone: a small box and a triangle opening to the right.
  dl->AddRectFilled(ImVec2(c.x - w, c.y - h * 0.42f), ImVec2(c.x - w * 0.38f, c.y + h * 0.42f),
                    col, 0.0f);
  dl->AddTriangleFilled(ImVec2(c.x - w * 0.42f, c.y - h),
                        ImVec2(c.x - w * 0.42f, c.y + h),
                        ImVec2(c.x + w * 0.05f, c.y), col);
  if (muted) {
    const float d = r * 0.42f;
    const ImVec2 m(c.x + r * 0.5f, c.y);
    dl->AddLine(ImVec2(m.x - d * 0.5f, m.y - d * 0.5f), ImVec2(m.x + d * 0.5f, m.y + d * 0.5f),
                col, r * 0.16f);
    dl->AddLine(ImVec2(m.x - d * 0.5f, m.y + d * 0.5f), ImVec2(m.x + d * 0.5f, m.y - d * 0.5f),
                col, r * 0.16f);
  } else {
    for (int i = 1; i <= 2; ++i) {
      dl->PathArcTo(ImVec2(c.x - w * 0.42f, c.y), r * (0.42f + 0.28f * i), -0.9f, 0.9f, 14);
      dl->PathStroke(col, r * 0.13f);
    }
  }
}

std::string Elapsed(double seconds) {
  const int total = (int)seconds;
  return Format("%d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
}

}  // namespace

float ToolbarHeight() {
  return ButtonSide() + BarPadding() * 2.0f;
}

ToolbarResult DrawToolbar(const ToolbarState& state, unsigned accentRgb) {
  ToolbarResult result;

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float height = ToolbarHeight();
  ImGui::SetNextWindowPos(vp->WorkPos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, height), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.88f);

  // Only the three the window itself reads may be popped straight after Begin.
  // ItemSpacing and FramePadding are read by the widgets, so they have to stay
  // pushed until the contents are done -- popping them here leaves the slider at
  // the default frame padding, which is nearly the full height of the bar.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                      ImVec2(ImGui::GetFontSize() * 0.5f, BarPadding()));
  const bool open = ImGui::Begin("##toolbar", nullptr,
                                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                     ImGuiWindowFlags_NoSavedSettings |
                                     ImGuiWindowFlags_NoBringToFrontOnFocus |
                                     // Without NoNavInputs this window takes
                                     // keyboard navigation focus, which makes
                                     // ImGui claim the keyboard, which makes the
                                     // application drop every hotkey. The bar is
                                     // driven with the mouse; it has no business
                                     // holding the keyboard.
                                     ImGuiWindowFlags_NoNavInputs |
                                     ImGuiWindowFlags_NoNavFocus);
  ImGui::PopStyleVar(3);
  if (!open) {
    ImGui::End();
    return result;
  }

  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetFontSize() * 0.28f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 1.0f));

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float side = ButtonSide();
  // Top of the button row. Anything shorter than a button is centred against
  // this rather than left to ImGui's line alignment, which puts a slider hard
  // against the bottom edge of the bar.
  const float rowTop = ImGui::GetCursorPosY();
  auto centreInRow = [&](float itemHeight) {
    ImGui::SetCursorPosY(rowTop + (side - itemHeight) * 0.5f);
  };
  const float radius = side * 0.5f;
  const ImU32 fg = ImGui::GetColorU32(ImGuiCol_Text);
  const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);

  // One button: an invisible hit area with an icon drawn into it, so every entry
  // is the same size whatever it depicts.
  auto button = [&](const char* id, const char* tip, bool enabled,
                    void (*draw)(ImDrawList*, ImVec2, float, ImU32), ImU32 colour) {
    ImGui::BeginDisabled(!enabled);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(side, side));
    const bool hovered = ImGui::IsItemHovered();
    if (hovered && enabled) {
      dl->AddRectFilled(origin, ImVec2(origin.x + side, origin.y + side),
                        ImGui::GetColorU32(ImGuiCol_ButtonHovered), side * 0.18f);
    }
    draw(dl, ImVec2(origin.x + radius, origin.y + radius), radius, enabled ? colour : dim);
    ImGui::EndDisabled();
    if (hovered && tip && *tip) ImGui::SetTooltip("%s", tip);
    ImGui::SameLine();
    return pressed && enabled;
  };

  // ---- record ----
  {
    const ImU32 red = IM_COL32(230, 70, 70, 255);
    ImGui::BeginDisabled(!state.canRecord);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("##rec", ImVec2(side, side));
    const bool hovered = ImGui::IsItemHovered();
    if (hovered && state.canRecord) {
      dl->AddRectFilled(origin, ImVec2(origin.x + side, origin.y + side),
                        ImGui::GetColorU32(ImGuiCol_ButtonHovered), side * 0.18f);
    }
    const ImVec2 centre(origin.x + radius, origin.y + radius);
    if (state.recording) {
      IconStop(dl, centre, radius, fg);
    } else {
      IconRecord(dl, centre, radius, state.canRecord ? red : dim);
    }
    ImGui::EndDisabled();
    if (hovered) {
      ImGui::SetTooltip("%s", state.recording ? T("Aufnahme stoppen", "Stop recording")
                                              : T("Aufnahme starten", "Start recording"));
    }
    if (pressed && state.canRecord) result.action = ToolbarAction::ToggleRecording;
    ImGui::SameLine();
  }

  if (state.recording) {
    // The running time sits next to the button rather than in a corner, so the
    // one thing that is time critical is where the eye already is.
    centreInRow(ImGui::GetTextLineHeight());
    ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.4f, 1.0f), "%s", Elapsed(state.recordSeconds).c_str());
    ImGui::SameLine();
  }

  if (button("##shot", T("Screenshot", "Screenshot"), true, IconCamera, fg)) {
    result.action = ToolbarAction::Screenshot;
  }
  if (button("##settings", T("Einstellungen", "Settings"), true, IconSettings, fg)) {
    result.action = ToolbarAction::Settings;
  }

  ImGui::Dummy(ImVec2(side * 0.3f, 0.0f));
  ImGui::SameLine();

  {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("##recfolder", ImVec2(side, side));
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
      dl->AddRectFilled(origin, ImVec2(origin.x + side, origin.y + side),
                        ImGui::GetColorU32(ImGuiCol_ButtonHovered), side * 0.18f);
      ImGui::SetTooltip("%s", T("Aufnahmeordner öffnen", "Open the recordings folder"));
    }
    IconFolder(dl, ImVec2(origin.x + radius, origin.y + radius), radius, fg, true);
    if (pressed) result.action = ToolbarAction::OpenRecordFolder;
    ImGui::SameLine();
  }
  {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("##shotfolder", ImVec2(side, side));
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
      dl->AddRectFilled(origin, ImVec2(origin.x + side, origin.y + side),
                        ImGui::GetColorU32(ImGuiCol_ButtonHovered), side * 0.18f);
      ImGui::SetTooltip("%s", T("Screenshot-Ordner öffnen", "Open the screenshots folder"));
    }
    IconFolder(dl, ImVec2(origin.x + radius, origin.y + radius), radius, fg, false);
    if (pressed) result.action = ToolbarAction::OpenScreenshotFolder;
    ImGui::SameLine();
  }

  ImGui::Dummy(ImVec2(side * 0.3f, 0.0f));
  ImGui::SameLine();

  // ---- volume ----
  {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("##mute", ImVec2(side, side));
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
      dl->AddRectFilled(origin, ImVec2(origin.x + side, origin.y + side),
                        ImGui::GetColorU32(ImGuiCol_ButtonHovered), side * 0.18f);
      ImGui::SetTooltip("%s", state.muted ? T("Ton an", "Unmute") : T("Stumm", "Mute"));
    }
    IconSpeaker(dl, ImVec2(origin.x + radius, origin.y + radius), radius,
                state.muted ? dim : fg, state.muted);
    if (pressed) result.action = ToolbarAction::ToggleMute;
    ImGui::SameLine();
  }

  ImGui::BeginDisabled(state.muted);
  centreInRow(ImGui::GetFrameHeight());
  ImGui::SetNextItemWidth(side * 4.5f);
  float percent = state.volume * 100.0f;
  if (ImGui::SliderFloat("##vol", &percent, 0.0f, 100.0f, "%.0f %%")) {
    result.volume = Clamp(percent / 100.0f, 0.0f, 1.0f);
  }
  ImGui::EndDisabled();

  // ---- hide, pushed to the right edge ----
  {
    const float x = vp->WorkSize.x - side - ImGui::GetStyle().WindowPadding.x;
    if (x > ImGui::GetCursorPosX()) ImGui::SameLine(x);
    ImGui::SetCursorPosY(rowTop);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("##hide", ImVec2(side, side));
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
      dl->AddRectFilled(origin, ImVec2(origin.x + side, origin.y + side),
                        ImGui::GetColorU32(ImGuiCol_ButtonHovered), side * 0.18f);
      ImGui::SetTooltip("%s", T("Leiste ausblenden (Rechtsklick bringt sie zurück)",
                                "Hide the bar (the right-click menu brings it back)"));
    }
    const ImVec2 c(origin.x + radius, origin.y + radius);
    const float d = radius * 0.42f;
    dl->AddLine(ImVec2(c.x - d, c.y - d), ImVec2(c.x + d, c.y + d), dim, radius * 0.16f);
    dl->AddLine(ImVec2(c.x - d, c.y + d), ImVec2(c.x + d, c.y - d), dim, radius * 0.16f);
    if (pressed) result.action = ToolbarAction::Hide;
  }

  // A hairline in the accent colour, so the bar reads as part of the program
  // rather than something floating on top of the picture.
  const ImVec2 pos = ImGui::GetWindowPos();
  const ImVec2 size = ImGui::GetWindowSize();
  dl->AddLine(ImVec2(pos.x, pos.y + size.y - 1.0f), ImVec2(pos.x + size.x, pos.y + size.y - 1.0f),
              Rgb(accentRgb, 0.55f), 1.0f);

  ImGui::PopStyleVar(2);
  ImGui::End();
  return result;
}

}  // namespace cap
