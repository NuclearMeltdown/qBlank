#include "ui/startup_dialog.h"

#include "common.h"
#include "config.h"
#include "render/display.h"
#include "ui/theme.h"
#include "window.h"

#include "imgui.h"

namespace cap {
namespace {

// The accent the program starts life with. There are no settings to read one
// from yet -- that is the whole reason this window exists.
constexpr unsigned kDefaultAccent = 0x8B5CF6;

Display* g_display = nullptr;
StartupAnswer g_answer = StartupAnswer::Postpone;
bool g_done = false;

// How tall the content turned out to be. The window is created too big and
// trimmed to this after the first frame: a question with one file in it and one
// with three are not the same size, and guessing at the difference leaves either
// a hole under the buttons or a scrollbar.
float g_contentHeight = 0.0f;

bool OnEvent(const WindowEvent& e) {
  switch (e.kind) {
    case WindowEvent::Kind::Resized:
      if (g_display && !e.minimized) g_display->Resize();
      return true;
    case WindowEvent::Kind::CloseRequested:
      // Closing decides nothing. Both files stay where they are and the
      // question comes back at the next start, which is the only answer that
      // cannot be given by accident.
      g_answer = StartupAnswer::Postpone;
      g_done = true;
      return true;
    // Nothing for Destroyed, and deliberately nothing. The usual quit request
    // belongs to a window that *is* the program; this one is asked before the
    // program exists and hands control back to a caller that still has work to
    // do. A quit requested here outlives the window -- the event queue is the
    // thread's, not the window's -- and the main loop would find it waiting on
    // its very first pass and shut down without ever drawing a frame. The loop
    // below ends on g_done, which is what the buttons and closing set.
    default:
      return false;
  }
}

void Draw(const StartupQuestion& q, float scale) {
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(viewport->WorkSize);
  ImGui::Begin("##startup", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
  ImGui::TextUnformatted(q.heading.c_str());
  ImGui::PopStyleColor();
  ImGui::Spacing();

  ImGui::TextWrapped("%s", q.body.c_str());

  if (!q.rows.empty()) {
    ImGui::Spacing();
    if (ImGui::BeginTable("##files", 2,
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
      for (const StartupQuestion::Row& row : q.rows) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (row.highlight) {
          ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), "%s",
                             row.label.c_str());
        } else {
          ImGui::TextUnformatted(row.label.c_str());
        }
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", row.detail.c_str());
      }
      ImGui::EndTable();
    }
  }

  // Buttons after the facts, footnote under them: the last thing read before
  // clicking should be what the click cannot break.
  ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight()));

  const float buttonHeight = ImGui::GetFrameHeight() * 1.4f;
  const float spacing = ImGui::GetStyle().ItemSpacing.x;
  const float width = (ImGui::GetContentRegionAvail().x - spacing) * 0.5f;
  if (ImGui::Button(q.acceptLabel.c_str(), ImVec2(width, buttonHeight))) {
    g_answer = StartupAnswer::Accept;
    g_done = true;
  }
  ImGui::SameLine();
  if (ImGui::Button(q.rejectLabel.c_str(), ImVec2(width, buttonHeight))) {
    g_answer = StartupAnswer::Reject;
    g_done = true;
  }

  if (!q.footnote.empty()) {
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", q.footnote.c_str());
    ImGui::PopTextWrapPos();
  }

  g_contentHeight = ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y;
  ImGui::End();
  (void)scale;
}

}  // namespace

StartupAnswer AskAtStartup(const StartupQuestion& question) {
  g_answer = StartupAnswer::Postpone;
  g_done = false;

  // Centred on the primary screen's work area, at that screen's scaling. There
  // is no saved position to restore -- this window is seen once.
  const float scale = SystemUiScale();
  // Created generously tall and hidden; the first frame measures the content and
  // the window is trimmed to it before it is shown.
  WindowSpec spec;
  spec.role = WindowRole::Dialog;
  spec.id = "StartupDialog";
  spec.title = AppNameUtf8();
  spec.width = (int)(620 * scale);
  spec.height = (int)(760 * scale);

  Window window;
  window.SetListener(OnEvent);
  if (window.Create(spec) != CreateResult::Ok) return StartupAnswer::Postpone;

  Display display;
  std::string error;
  if (!display.Initialize(window, &error)) {
    // No device, no window worth showing. The caller falls back to deciding
    // nothing, which leaves both files untouched.
    window.Destroy();
    return StartupAnswer::Postpone;
  }
  g_display = &display;

  const bool dark = ResolveDark(Theme::System);
  IMGUI_CHECKVERSION();
  ImGuiContext* previous = ImGui::GetCurrentContext();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  LoadUiFont(17.0f * scale);
  ApplyImGuiTheme(dark, kDefaultAccent);
  ImGui::GetStyle().ScaleAllSizes(scale);

  const bool attached = window.AttachUi(nullptr);
  const bool ready = attached && display.InitUi();
  if (ready) {
    window.SetDarkFrame(dark);

    float clear[4];
    GetBackgroundColor(dark, kDefaultAccent, clear);
    // Vsync on: this window has all the time in the world and no reason to
    // spin a core while somebody reads it.
    bool shown = false;
    while (!g_done) {
      if (!PumpEvents()) break;

      display.NewUiFrame();
      window.BeginUiFrame();
      ImGui::NewFrame();
      Draw(question, scale);
      ImGui::Render();

      // The first frame is only measured, never shown: it exists to find out how
      // tall the window has to be.
      if (!shown) {
        shown = true;
        // Trimmed to the height the content actually needed and put back in the
        // middle of the screen.
        window.ResizeClientHeightCentred((int)(g_contentHeight + 0.5f));
        window.Show();
        continue;
      }

      if (display.BeginFrame(clear)) {
        display.RenderUi(ImGui::GetDrawData());
        display.EndFrame(true);
      } else {
        SleepMilliseconds(16);
      }
    }

    display.ShutdownUi();
  }
  // Also when the drawing side failed to start: the window's half was already
  // attached, and a context must not be destroyed with a backend still in it.
  if (attached) window.DetachUi();

  ImGui::DestroyContext();
  ImGui::SetCurrentContext(previous);
  g_display = nullptr;
  display.Shutdown();
  window.Destroy();

  // Nothing of this window may be left in the queue when the program starts.
  // The handler above no longer asks to quit, but ImGui's backend and the
  // shutdown of a swap chain both dispatch through the same queue, and the
  // caller's loop treats any quit request it finds as its own.
  DiscardPendingQuit();
  return ready ? g_answer : StartupAnswer::Postpone;
}

}  // namespace cap
