#include "ui/settings_host.h"

#include "common.h"
#include "imgui.h"
#include "ui/theme.h"
#include "i18n.h"

namespace cap {

SettingsHost::~SettingsHost() { Destroy(); }

bool SettingsHost::OnWindowEvent(const WindowEvent& e) {
  // Nothing of the settings exists yet, or any more: the window is left to
  // itself.
  if (!imgui_) return false;

  switch (e.kind) {
    case WindowEvent::Kind::KeyDown: {
      // Asked in *this* window's context, not the main one.
      ImGuiContext* previous = ImGui::GetCurrentContext();
      ImGui::SetCurrentContext(imgui_);
      // Not WantCaptureKeyboard: with keyboard navigation on, that is true whenever
      // this window has focus at all, which is exactly when the shortcuts should
      // work. What the dialog really needs the keys for is typing and open lists.
      const bool busy = ImGui::GetIO().WantTextInput ||
                        ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId |
                                                   ImGuiPopupFlags_AnyPopupLevel);
      ImGui::SetCurrentContext(previous);
      return onKey_ && onKey_(e.key, e.ctrl, e.shift, e.alt, busy);
    }
    case WindowEvent::Kind::CloseRequested:
      closeRequested_ = true;
      Hide();
      return true;
    case WindowEvent::Kind::Resized:
      if (!e.minimized) Resize();
      return true;
    case WindowEvent::Kind::ModalFrame:
      PumpModalFrame();
      return true;
    default:
      return false;
  }
}

// One frame from inside the modal loop.
//
// Dragging a window puts Windows into a loop of its own that does not return
// until the mouse is released, so qBlank's own loop stops running and the
// preview stops with it. The only way back in is from a message this window
// receives while that loop is running.
// Kein Takt mehr an dieser Stelle -- und das ist der dritte Anlauf.
//
// Erst stand hier fest 30 ms. Sichtbar: die Vorschau fiel beim Ziehen auf 33
// Bilder, nicht weil etwas ueberlastet war, sondern weil es hier so beschlossen
// wurde. Dann stand hier die gemessene Bildrate der Quelle, und das war
// schlechter: bei PAL sind das 25 Bilder, waehrend die Vorschau nach dem
// Deinterlacing 50 Halbbilder zeigt -- die Haelfte davon fiel weg. Dazu kommt,
// dass der grobe Millisekundenzaehler alle 15,6 ms weiterzaehlt, also jede Schranke auf das
// naechste Vielfache davon aufrundet: 40 ms werden zu 46,8 und damit 21 Bilder
// in der Sekunde. Genau das war zu sehen.
//
// Eine Zahl ist hier immer falsch, egal welche. Ob es etwas Neues zu zeigen
// gibt, weiss nur die Schleife oben, und die entscheidet es fuer den Normalfall
// laengst -- neues Bild, faelliges zweites Halbbild, oder ein langsamer Boden.
// Also wird hier nichts mehr entschieden: der Rueckruf wird angeboten, und was
// daran haengt, prueft dieselbe Bedingung wie sonst auch.
void SettingsHost::PumpModalFrame() {
  if (!onFrame_ || inFrameCallback_) return;
  inFrameCallback_ = true;
  onFrame_();
  inFrameCallback_ = false;
}

bool SettingsHost::Create(float uiScale, bool allowTearing, const Placement& where,
                          std::string* error) {
  if (created()) return true;
  // Eine eigene Zeichenflaeche mit eigenem Geraet, nicht die der Vorschau.
  //
  // Zwei Swapchains auf einem Geraet teilen sich zwangslaeufig zwei Dinge, und
  // beide waren teuer. Erstens gilt die Bildwarteschlange fuer das *Geraet*:
  // mit der kurzen, von der die Vorschau lebt, wartete jedes Present auf das
  // Bild der jeweils anderen. Zweitens teilen sie den Befehlsstrang -- jeder
  // Zeichenbefehl des Dialogs laeuft dann durch genau den Strang, den die
  // Vorschau braucht, und serialisiert sich dagegen.
  //
  // Ein zweites Geraet loest beides an der Wurzel statt es auszubalancieren.
  // Es kostet eine eigene Schriftatlas-Textur und etwas Speicher; dafuer darf
  // die Vorschau ihre Warteschlange dauerhaft auf eins lassen, was der groesste
  // einzelne Hebel auf ihre Verzoegerung ist.
  if (!surface_.CreateDevice(error)) return false;
  uiScale_ = uiScale > 0.1f ? uiScale : 1.0f;

  // What it was last time, or a sensible default the first time. The window
  // itself checks that a remembered position is still reachable.
  WindowSpec spec;
  spec.role = WindowRole::Tool;
  spec.id = "SettingsWindow";
  spec.title = AppNameUtf8();
  spec.width = where.width > 200 ? where.width : (int)(720 * uiScale_);
  spec.height = where.height > 200 ? where.height : (int)(640 * uiScale_);
  spec.hasPosition = where.x != -1 || where.y != -1;
  spec.x = where.x;
  spec.y = where.y;
  window_.SetListener([this](const WindowEvent& e) { return OnWindowEvent(e); });
  if (window_.Create(spec) != CreateResult::Ok) {
    ReportError(error, CAP_SAID(T("Einstellungsfenster konnte nicht erstellt werden",
                                     "The settings window could not be created")));
    return false;
  }

  if (!surface_.Attach(window_, allowTearing, error)) {
    Destroy();
    return false;
  }

  ImGuiContext* previous = ImGui::GetCurrentContext();
  // Eigener Atlas, weil eine Textur nicht ueber zwei Geraete hinweg gilt. Das
  // nimmt dem Ganzen zugleich die Falle, die der geteilte Atlas mitbrachte: der
  // DX11-Rueckenteil legt die Textur-Id *im Atlas* ab, also riss das Schliessen
  // dieses Fensters dem Hauptfenster die Schrift unter den Fuessen weg.
  imgui_ = ImGui::CreateContext();
  ImGui::SetCurrentContext(imgui_);
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  // Dieselbe Schrift wie im Hauptfenster, und in derselben Groesse. Der eigene
  // Atlas faengt sonst mit ImGuis eingebauter an -- dann sieht ein Fenster des
  // Programms aus wie aus einem anderen Programm.
  //
  // Muss nach SetCurrentContext stehen: LoadUiFont haengt die Schrift in den
  // Atlas des gerade aktuellen Kontexts.
  LoadUiFont(17.0f * uiScale_);

  const bool ok = window_.AttachUi(imgui_) && surface_.InitUi();
  ImGui::SetCurrentContext(previous);
  if (!ok) {
    ReportError(error, CAP_SAID(T("ImGui für das Einstellungsfenster fehlgeschlagen",
                                     "ImGui failed for the settings window")));
    Destroy();
    return false;
  }

  CAP_LOG("Settings window created");
  return true;
}

void SettingsHost::Destroy() {
  if (imgui_) {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    // If this context is the current one, it is about to stop existing -- so
    // there is nothing to go back to and nothing to repair.
    if (previous == imgui_) previous = nullptr;
    ImGui::SetCurrentContext(imgui_);
    surface_.ShutdownUi();
    window_.DetachUi();
    ImGui::DestroyContext(imgui_);
    imgui_ = nullptr;
    ImGui::SetCurrentContext(previous == nullptr ? nullptr : previous);

  }
  surface_.ReleaseSwapchain();
  window_.Destroy();
  surface_.ReleaseDevice();
  visible_ = false;
}

void SettingsHost::Resize() { surface_.Resize(); }

void SettingsHost::Show(const std::string& title) {
  if (!created()) return;
  window_.SetTitle(title);
  // Restored if it was minimised: BeginFrame refuses to draw a minimised window,
  // so reopening the settings after minimising them used to do nothing at all.
  // Only reachable since this window gained a taskbar button and could be
  // minimised properly in the first place.
  window_.Show();
  visible_ = true;
  closeRequested_ = false;
}

void SettingsHost::Hide() {
  if (!created()) return;
  window_.Hide();
  visible_ = false;
}

void SettingsHost::Raise() {
  if (!created() || !visible_) return;
  window_.Raise();
}

bool SettingsHost::RaiseIfCoveredBy(const Window& other) {
  if (!created() || !visible_) return false;
  if (window_.minimized() || window_.IsCoveredBy(other)) {
    Raise();
    return true;
  }
  return false;
}

void SettingsHost::SetTopmost(bool top) {
  // Only when it changes, which the window sees to. Moving to the top of the
  // topmost windows every time would put the settings back over a preview
  // that had just been clicked into.
  window_.SetTopmost(top);
}

bool SettingsHost::takeCloseRequest() {
  const bool requested = closeRequested_;
  closeRequested_ = false;
  return requested;
}

void SettingsHost::ApplyTheme(bool darkMode, unsigned accentColor) {
  if (!imgui_) return;
  ImGuiContext* previous = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(imgui_);
  ApplyImGuiTheme(darkMode, accentColor);
  ImGui::GetStyle().ScaleAllSizes(uiScale_);
  ImGui::SetCurrentContext(previous);
  window_.SetDarkFrame(darkMode);
  themeApplied_ = true;
}

bool SettingsHost::BeginFrame(bool darkMode, unsigned accentColor) {
  if (!created() || !visible_ || !imgui_ || !surface_.hasTarget()) return false;
  if (window_.minimized()) return false;
  // Covered by something else: stop drawing and ask cheaply whether that is
  // still true, rather than paying for a present nobody can see.
  if (surface_.StillOccluded()) return false;
  if (surface_.width() <= 0 || surface_.height() <= 0) return false;

  // Ein Deckel bleibt, aber weit oberhalb dessen, was ein Monitor zeigt: ohne
  // ihn zeichnet der Dialog den kompletten Einstellungsbaum so oft neu, wie die
  // Schleife durchlaeuft, und verbrennt dafuer Rechenzeit an Bilder, die
  // niemand sieht. Bei vier Millisekunden sind das 250 in der Sekunde -- ueber
  // jeder Bildwiederholrate, die an diesem Rechner haengt, und trotzdem
  // begrenzt.
  const uint32_t now = TickMilliseconds();
  if (lastDrawTick_ != 0 && now - lastDrawTick_ < 4) return false;
  lastDrawTick_ = now;

  if (!themeApplied_) ApplyTheme(darkMode, accentColor);

  previous_ = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(imgui_);
  surface_.NewUiFrame();
  window_.BeginUiFrame();
  ImGui::NewFrame();
  return true;
}

void SettingsHost::EndFrame() {
  ImGui::Render();

  float clear[4];
  GetBackgroundColor(true, 0x000000, clear);
  const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
  clear[0] = bg.x;
  clear[1] = bg.y;
  clear[2] = bg.z;
  clear[3] = 1.0f;

  surface_.Present(clear);

  ImGui::SetCurrentContext(previous_);
  previous_ = nullptr;
}

SettingsHost::Placement SettingsHost::placement() const {
  Placement out;
  // The restored rectangle, not the current one: a window read while it is
  // minimised or maximised would be remembered at the wrong size.
  Window::Placement wp;
  if (!window_.GetPlacement(&wp)) return out;
  out.x = wp.x;
  out.y = wp.y;
  out.width = wp.width;
  out.height = wp.height;
  return out;
}

}  // namespace cap
