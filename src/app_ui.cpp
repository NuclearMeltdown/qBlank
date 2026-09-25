// Everything drawn over the picture: the overlay, the context menu, the
// toolbar, toasts, the settings window and the mouse on the picture.

#include "app.h"

#include <cmath>

#include "desktop.h"
#include "files.h"
#include "i18n.h"
#include "imgui_internal.h"
#include "platform.h"
#include "record/screenshot.h"

namespace cap {
namespace {

// How long the volume readout stays on screen after a change.
const double kVolumeOsdSeconds = 1.6;
// Wie SetItemTooltip, aber mit Umbruch -- dieselbe Breite wie im
// Einstellungsfenster, damit beide gleich aussehen. Der eingebaute bricht nicht
// um: ein ganzer Satz laeuft dann als eine einzige Zeile quer ueber den
// Bildschirm und steht mit dem Ende davon ausserhalb.
void WrappedTooltip(const char* text) {
  if (!text || !*text) return;
  if (ImGui::BeginItemTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

// BeginMenu mit Tastenkuerzel. Dear ImGui zeichnet bei einem Untermenue keins,
// die Spalte dafuer hat das Menue aber trotzdem: hier wird sie angemeldet und
// das Kuerzel an dieselbe Stelle und in derselben Farbe gesetzt wie bei einem
// MenuItem, links vom Pfeil.
bool BeginMenuWithShortcut(const char* label, const char* shortcut) {
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  const bool draw = shortcut && *shortcut && !window->SkipItems &&
                    window->DC.LayoutType == ImGuiLayoutType_Vertical;
  const ImVec2 pos = window->DC.CursorPos;
  float stretch = 0.0f;
  if (draw) {
    const float mark = IM_TRUNC(ImGui::GetFontSize() * 1.20f);
    const float width = window->DC.MenuColumns.DeclColumns(
        0.0f, ImGui::CalcTextSize(label, nullptr, true).x, ImGui::CalcTextSize(shortcut).x, mark);
    stretch = ImMax(0.0f, ImGui::GetContentRegionAvail().x - width);
  }
  const bool open = ImGui::BeginMenu(label);
  // Nach einem geoeffneten BeginMenu ist das Untermenue das aktuelle Fenster,
  // das Kuerzel gehoert aber in die Zeile darueber.
  if (draw) {
    const ImVec2 at(pos.x + window->DC.MenuColumns.OffsetShortcut + stretch, pos.y);
    window->DrawList->AddText(at, ImGui::GetColorU32(ImGuiCol_TextDisabled), shortcut);
  }
  return open;
}

}  // namespace

// Wie lange die Anzeige stehen darf, wenn kein Bild ankommt.
//
// Das Video braucht keinen Boden: kommt nichts, gibt es nichts Neues zu zeigen,
// und 200 ms halten den Ruhebildschirm am Leben, ohne Rechenzeit fuer ein
// unveraendertes Bild zu verbrennen.
//
// Die Bedienoberflaeche ist etwas anderes. Sie bewegt sich aus eigener Kraft --
// und das eingebettete Einstellungsfeld wird von genau dieser Schleife
// gezeichnet. Mit dem Boden fuer das Video lief es ohne Aufnahmegeraet mit
// gemessenen 4,7 Bildern in der Sekunde, was sich anfuehlt wie zwei.
//
// Es geht dabei nicht darum, ob ein Signal anliegt, sondern ob etwas auf dem
// Schirm ist, das sich bewegen koennen muss. Liegt ein Signal an, gibt dessen
// Takt ohnehin alles vor und dieser Boden kommt nie zum Tragen.
double App::IdleFloorMs() const {
  const bool embeddedPanel = settings_.isOpen() && !config_.app.settingsSeparateWindow;
  const double now = ImGui::GetTime();
  const bool toastUp = !toastText_.empty() && now - toastStart_ <= 2.5;
  const bool osdUp = now - volumeOsdStart_ <= kVolumeOsdSeconds;
  // Der Hinweis mit Knoepfen will auf die Maus antworten wie jeder andere.
  const bool noticeUp = resolutionNoticeLines_ > 0 || !standardSearch_.colourNotice().empty();
  if (embeddedPanel || cropTool_.active() || toastUp || osdUp || noticeUp) return 16.0;
  return 200.0;
}

namespace {

// So lange steht ein Toast. Einer zu einer Datei etwas laenger: den will man
// vielleicht noch anklicken, und bis die Maus unten in der Mitte ist, waere
// der kurze schon halb verblasst.
constexpr double kToastSeconds = 2.5;
constexpr double kFileToastSeconds = 4.0;
// Wie viele Toasts zu einer Datei dazusagen, dass ein Klick sie zeigt.
constexpr int kFileToastHints = 3;

}  // namespace

void App::Toast(const std::string& text, const std::filesystem::path& file) {
  // Zwei Meldungen kurz nacheinander stehen untereinander, statt dass die
  // zweite die erste verdraengt, bevor jemand sie lesen konnte. So kommt nach
  // dem Anpassen der Aufloesung der Neubau mit seiner eigenen Meldung zum
  // Zuschnitt. Nur zwei Zeilen, und nicht bei einem Toast zu einer Datei:
  // dessen Klick gehoert zu genau einer Meldung.
  const double now = ImGui::GetTime();
  if (file.empty() && toastFile_.empty() && !toastText_.empty() && toastText_ != text &&
      toastText_.find('\n') == std::string::npos && now - toastStart_ < 1.5) {
    toastText_ += "\n" + text;
    toastStart_ = now;
    return;
  }
  toastText_ = text;
  toastFile_ = file;
  toastTouched_ = false;
  toastStart_ = ImGui::GetTime();
  // Beim Entstehen entschieden, nicht je Bild: sonst verschwaende die Zeile
  // beim letzten Mal mitten im Toast, sobald der Zaehler oben ankommt.
  toastHint_ = !file.empty() && config_.app.fileToastHints < kFileToastHints;
  if (toastHint_) ++config_.app.fileToastHints;
}

// Ein Toast zu einer Datei -- Aufnahme gespeichert, Screenshot -- zeigt sie auf
// Klick im Explorer. Solange die Maus darauf liegt, bleibt er stehen: wer
// hinzeigt, will ihn noch lesen oder gleich anklicken. Aber nur, wenn sie sich
// dort auch bewegt hat. Eine Maus, die zufaellig unten in der Mitte parkt,
// hielte ihn sonst fuer immer ueber dem Bild.
void App::DrawToastStrip(float lift) {
  if (toastText_.empty()) return;
  const bool clickable = !toastFile_.empty();
  const double duration = clickable ? kFileToastSeconds : kToastSeconds;
  const double age = ImGui::GetTime() - toastStart_;
  if (age > duration) {
    toastText_.clear();
    toastFile_.clear();
    return;
  }

  const ToastResult result =
      DrawToast(toastText_, age, duration, clickable,
                toastHint_ ? T("Klicken zeigt die Datei im Ordner", "Click to show the file in its folder")
                           : nullptr,
                lift);
  if (!result.hovered) return;

  const ImVec2 delta = ImGui::GetIO().MouseDelta;
  if (delta.x != 0.0f || delta.y != 0.0f) toastTouched_ = true;
  if (toastTouched_) toastStart_ = ImGui::GetTime();

  if (result.clicked) {
    // Wer einmal geklickt hat, braucht den Hinweis nicht mehr.
    config_.app.fileToastHints = kFileToastHints;
    if (!PathExists(toastFile_)) {
      Toast(T("Die Datei ist nicht mehr da.", "The file is no longer there."));
      return;
    }
    ShowFileInFolder(toastFile_);
    toastText_.clear();
    toastFile_.clear();
  }
}

// Die Frage zur Aufloesung, wenn nicht automatisch angepasst wird. Sie steht,
// bis sie beantwortet ist: "Anpassen" tut dasselbe wie der Automatismus und
// sagt es genauso, "Ignorieren" nimmt sie vom Bild. Der Hinweis im Reiter
// Quelle bleibt in beiden Faellen, solange es nicht passt.
float App::DrawResolutionNotice() {
  if (resolutionNoticeLines_ <= 0) return 0.0f;
  float height = 0.0f;
  const NoticeAnswer answer = DrawNotice(resolutionNoticeText_, T("Anpassen", "Fix"),
                                         T("Ignorieren", "Ignore"), &height);
  if (answer == NoticeAnswer::Primary) {
    const int lines = resolutionNoticeLines_;
    resolutionNoticeLines_ = 0;
    ApplyFittingResolution(lines);
  } else if (answer == NoticeAnswer::Dismiss) {
    const VideoFormatInfo fmt = renderer_.sourceFormat();
    CAP_LOG("Resolution %dx%d ignored for %d active lines", fmt.width, fmt.height,
            resolutionNoticeLines_);
    resolutionIgnoredWidth_ = fmt.width;
    resolutionIgnoredHeight_ = fmt.height;
    resolutionIgnoredLines_ = resolutionNoticeLines_;
    resolutionNoticeLines_ = 0;
  }
  return height > 0.0f ? height + 8.0f : 0.0f;
}

// "Norm suchen" ist dieselbe Suche wie von Hand, "Ignorieren" gilt bis zum
// naechsten Lock. Beides entscheidet die Suche selbst.
float App::DrawColourNotice() {
  const std::string& text = standardSearch_.colourNotice();
  if (text.empty()) return 0.0f;
  float height = 0.0f;
  const NoticeAnswer answer = DrawNotice(text, T("Norm suchen", "Find standard"),
                                         T("Ignorieren", "Ignore"), &height);
  if (answer != NoticeAnswer::None) standardSearch_.AnswerColourNotice(answer == NoticeAnswer::Primary);
  return height > 0.0f ? height + 8.0f : 0.0f;
}

void App::DrawToolbarStrip() {
  ToolbarState state;
  state.recording = recorder_.recording();
  state.recordSeconds = state.recording ? recorder_.stats().seconds : 0.0;
  state.muted = config_.active().audio.mute;
  state.volume = config_.active().audio.volume;
  state.canRecord = captureState_ == CaptureState::Running && renderer_.hasFrame();

  const ToolbarResult result = DrawToolbar(state, config_.app.accentColor);
  if (result.volume >= 0.0f) {
    config_.active().audio.volume = result.volume;
    if (config_.active().audio.mute) config_.active().audio.mute = false;
    audio_.ApplySettings(config_.active().audio);
    ShowVolumeOsd();
  }

  switch (result.action) {
    case ToolbarAction::ToggleRecording: recording_.ToggleRecording(); break;
    case ToolbarAction::Screenshot: RequestScreenshot(); break;
    case ToolbarAction::Settings: OpenSettings({}); break;
    case ToolbarAction::OpenRecordFolder:
      ShowOutputFolder(&config_.record.outputFolder, DefaultRecordFolder());
      break;
    case ToolbarAction::OpenScreenshotFolder:
      ShowOutputFolder(&config_.record.screenshotFolder, DefaultScreenshotFolder());
      break;
    case ToolbarAction::ToggleMute: ToggleMute(); break;
    case ToolbarAction::Hide: config_.app.showToolbar = false; break;
    default: break;
  }
}

// The program's own icon, as a texture for the empty state.
void App::LoadIdleIcon() {
  if (idleIcon_ || !display_.initialized()) return;

  int w = 0, h = 0;
  std::vector<uint8_t> pixels = AppIconRgba(256, &w, &h);
  if (pixels.empty()) return;

  // Premultiplying by alpha is the one thing still to do here, and it belongs
  // here: ImGui's blend state is premultiplied, and handing it straight alpha
  // draws a dark halo around every edge of the icon.
  for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
    const unsigned a = pixels[i + 3];
    pixels[i + 0] = (uint8_t)(pixels[i + 0] * a / 255u);
    pixels[i + 1] = (uint8_t)(pixels[i + 1] * a / 255u);
    pixels[i + 2] = (uint8_t)(pixels[i + 2] * a / 255u);
  }

  UiImage image = display_.CreateUiImage(pixels.data(), w, h);
  if (!image) return;
  idleIcon_ = image;
  idleIconSize_ = w;
}

void App::RememberSettingsWindow() {
  if (!settingsHost_.created()) return;
  const SettingsHost::Placement where = settingsHost_.placement();
  if (where.width <= 0 || where.height <= 0) return;
  config_.app.settingsWindowX = where.x;
  config_.app.settingsWindowY = where.y;
  config_.app.settingsWindowW = where.width;
  config_.app.settingsWindowH = where.height;
}

void App::DrawSettingsWindowed() {
  const bool wanted = config_.app.settingsSeparateWindow;

  // Nothing to do, and nothing built: the common case, and it costs one branch.
  if (!wanted && !settingsHost_.created()) return;

  if (wanted && !settingsHost_.created()) {
    std::string error;
    // Auspoppen: das Fenster geht dort auf, wo das eingebettete Feld gerade
    // stand. Der Weg fuehrt ueber Bildschirmkoordinaten, weil die beiden in
    // verschiedenen Bezugssystemen leben -- das Feld im Client des
    // Hauptfensters, das Fenster auf dem Desktop.
    //
    // Verglichen werden die *Aussenkanten* beider, nicht ihre Inhalte. Das ist
    // die einzige Zuordnung, die sich nicht um ein paar Pixel verzieht: das
    // eingebettete Feld zaehlt seine eigene Titelleiste zur Flaeche dazu, das
    // freigestellte faengt beim Client unterhalb der Windows-Titelleiste an.
    // Wer Client auf Client abbildet, verschiebt beim Umschalten jedes Mal um
    // die Differenz der beiden Leisten -- einmal nach unten, einmal nach oben.
    SettingsHost::Placement where;
    where.x = config_.app.settingsWindowX;
    where.y = config_.app.settingsWindowY;
    where.width = config_.app.settingsWindowW;
    where.height = config_.app.settingsWindowH;
    if (config_.app.settingsPanelW > 200 && config_.app.settingsPanelH > 200) {
      const Point topLeft =
          window_.ClientToScreen({config_.app.settingsPanelX, config_.app.settingsPanelY});
      where.x = topLeft.x;
      where.y = topLeft.y;
      where.width = config_.app.settingsPanelW;
      where.height = config_.app.settingsPanelH;
    }
    if (!settingsHost_.Create(uiScale_, display_.tearingSupported(), where, &error)) {
      config_.app.settingsSeparateWindow = false;
      Toast(error);
      return;
    }
    settingsHost_.ApplyTheme(darkMode_, config_.app.accentColor);
    ApplyWindowFlags();
    settingsHost_.SetKeyCallback([this](Key key, bool ctrl, bool shift, bool alt, bool busy) {
      return OnKey(key, ctrl, shift, alt, busy);
    });
    // While its window is being dragged, Windows keeps the loop to itself. The
    // timer inside that loop is what still lets the picture run.
    // Dragging a window puts Windows into a modal loop of its own that does not
    // return until the mouse is let go, so the main loop stops running and the
    // preview stops with it. A timer inside that loop is the only way back in.
    // It has to do what one turn of the main loop does -- which since the two
    // were separated means the settings window as well as the preview.
    settingsHost_.SetFrameCallback([this]() {
      if (inModalFrame_) return;
      inModalFrame_ = true;
      Tick();
      // The preview only. The dialog's *content* does not change while its
      // frame is being dragged, and redrawing it here means a second present
      // between every mouse movement and the window catching up with it --
      // which turns a frozen preview into a window that lags the cursor.
      //
      // Genau dieselbe Frage wie in der Hauptschleife, und aus demselben Grund:
      // ist ein neues Bild da, ist ein zweites Halbbild faellig, oder ist es zu
      // lange her? Frueher stand hier stattdessen eine Zeitschranke, und jede
      // Zahl, die dort stand, war neben der Kadenz der Quelle -- mal zu
      // langsam, mal gegen sie schwebend.
      //
      // Das Ereignis der Karte laesst sich mit einer Wartezeit von null
      // abfragen; es setzt sich selbst zurueck, also ist das dieselbe
      // Entnahme, die die Hauptschleife sonst macht. Sie laeuft in diesem
      // Moment nicht, also nimmt ihr das nichts weg.
      bool newPicture = false;
      if (FrameBuffer* sink = capture_.sink()) {
        newPicture = sink->frameReady().Wait(0);
      }
      const int64_t nowQpc = ClockTicks();
      const double sinceRenderMs =
          lastRenderQpc_ == 0 ? 1e9 : TicksToSeconds(nowQpc - lastRenderQpc_) * 1000.0;
      const bool fieldDue = secondFieldPending_ && nowQpc >= secondFieldQpc_;
      if (newPicture || fieldDue || sinceRenderMs >= 200.0) {
        lastRenderQpc_ = nowQpc;
        RenderFrame();
      }
      // Resizing is the exception: then the content has to be laid out anew,
      // or the old frame sits stretched across the new size until the mouse
      // comes up. Only once the size has actually changed, so a move still
      // costs nothing.
      if (settingsHost_.resized()) DrawSettingsFrame();
      inModalFrame_ = false;
    });
  }

  if (!wanted) {
    // Switched off again: put the panel back inside the picture, and give the
    // preview its shortest queue back. Where it stood is remembered first --
    // this is the same object that will be built again if it is switched back
    // on, and it should come up where it was left.
    RememberSettingsWindow();
    // Einbetten: das Feld geht dort auf, wo das Fenster gerade stand. Umgekehrt
    // derselbe Weg -- Client des Fensters auf den Bildschirm, von dort in den
    // Client des Hauptfensters.
    //
    // Liegt das Fenster ganz oder ueberwiegend neben dem Hauptfenster, kommt
    // dabei eine Lage heraus, die das Feld unerreichbar machen wuerde. Das faengt
    // die Wiederherstellung selbst ab und setzt in die Mitte.
    Rect outer;
    if (settingsHost_.window().FrameRect(&outer)) {
      const Point inMain = window_.ScreenToClient({outer.left, outer.top});
      config_.app.settingsPanelX = inMain.x;
      config_.app.settingsPanelY = inMain.y;
      config_.app.settingsPanelW = outer.width();
      config_.app.settingsPanelH = outer.height();
    }
    settings_.RestorePosition();
    settingsHost_.Destroy();
    return;
  }

  // Closing the window is closing the settings, the same as the button is.
  if (settingsHost_.takeCloseRequest()) settings_.Close();

  if (settings_.isOpen() != settingsHost_.visible()) {
    if (settings_.isOpen()) {
      settingsHost_.Show(AppNameUtf8() + T(" – Einstellungen", " – Settings"));
    } else {
      settingsHost_.Hide();
    }
  }

  // Die Vorschau behaelt ihre kurze Warteschlange, immer. Frueher musste sie
  // hier auf drei hoch, weil beide Fenster an einem Geraet hingen und sich
  // gegenseitig auf das Present warten liessen; seit der Dialog sein eigenes
  // Geraet hat, geht ihn das nichts mehr an.

  DrawSettingsFrame();
}

void App::DrawSettingsFrame() {
  if (!settingsHost_.BeginFrame(darkMode_, config_.app.accentColor)) return;

  settings_.SetFillsWindow(true);
  const SettingsWindow::Result result =
      settings_.Draw(capture_.running() ? &capture_.capabilities() : nullptr,
                     &recording_.ffmpeg());
  settingsHost_.EndFrame();
  settingsHost_.WidenOnce((int)settings_.oneRowWidth());

  // Nothing to put back. This runs after the main window has presented, so the
  // targets it wants are set again by the next frame's first pass.

  // Closing is closing, whether it was the footer button or the window's own.
  if (result == SettingsWindow::Result::Close) settings_.Close();
}

// Die Trennlinie des Vergleichs mit der Maus verschieben, so wie die Raender
// beim Zuschnitt. Gezeichnet wird sie weiterhin im Shader; hier wird nur
// ausgerechnet, wo sie auf dem Schirm liegt, und die Maus zurueck in einen
// Anteil verwandelt.
void App::DragCompareDivider() {
  if (!compare_ || !ImGui::IsMousePosValid()) {
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) compareDrag_ = false;
    return;
  }
  const ImageSettings img = EffectiveImage(config_.active());
  const Rect& r = renderer_.videoRect();
  const float rw = (float)(r.right - r.left);
  const float rh = (float)(r.bottom - r.top);
  if (!img.compare || !renderer_.hasFrame() || rw < 8.0f || rh < 8.0f) {
    compareDrag_ = false;
    return;
  }

  // Der Schnitt liegt im Raster der Quelle, gedreht wird erst danach. Eine
  // Vierteldrehung legt die Linie also quer, und bei zwei der vier Drehungen
  // zaehlt ihr Anteil vom anderen Rand her -- dieselbe Zuordnung wie im
  // zweiten Durchgang.
  const int rot = (int)img.rotation & 3;
  const bool upright = img.compareHorizontal == ((rot & 1) != 0);
  const bool reversed = img.compareHorizontal ? (rot == 1 || rot == 2) : rot >= 2;
  const float lo = upright ? (float)r.left : (float)r.top;
  const float len = upright ? rw : rh;
  const float split = Clamp(img.compareSplit, 0.0f, 1.0f);
  const float at = lo + (reversed ? 1.0f - split : split) * len;

  const ImVec2 mouse = ImGui::GetMousePos();
  const float across = upright ? mouse.x : mouse.y;
  const float along = upright ? mouse.y : mouse.x;
  const bool alongPicture = upright ? along >= (float)r.top && along <= (float)r.bottom
                                    : along >= (float)r.left && along <= (float)r.right;
  const bool hot = compareDrag_ || (!ImGui::GetIO().WantCaptureMouse && alongPicture &&
                                    std::abs(across - at) <= 8.0f * uiScale_);
  if (!hot) return;

  ImGui::SetMouseCursor(upright ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
  if (!compareDrag_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) compareDrag_ = true;
  if (compareDrag_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) compareDrag_ = false;
  if (!compareDrag_) return;

  float share = Clamp((across - lo) / len, 0.0f, 1.0f);
  if (reversed) share = 1.0f - share;
  config_.active().image.compareSplit = share;
}

// Doppelklick aufs Bild schaltet das Vollbild um, wie in jedem Videoplayer.
//
// Beide Klicks muessen auf dem blossen Bild landen. Der erste koennte sonst
// ein Menue geschlossen oder die Trennlinie gegriffen haben, und der zweite
// machte daraus einen Wechsel, den niemand wollte. Laeuft nach
// DragCompareDivider, damit ein Griff an die Linie schon zaehlt.
void App::DoubleClickFullscreen() {
  if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
  const bool onPicture = !ImGui::GetIO().WantCaptureMouse && !compareDrag_;
  if (onPicture && clickOnPicture_ && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    ToggleFullscreen();
    // Ein dritter Klick zaehlt als neuer erster.
    clickOnPicture_ = false;
    return;
  }
  clickOnPicture_ = onPicture;
}

// Ohne Titelleiste greift man das Fenster am Bild. Erst ein Ziehen ueber
// ImGuis Schwelle macht daraus ein Verschieben, ein blosser Klick bleibt ein
// Klick und zaehlt weiter fuer den Doppelklick. Der Druck muss wie dort auf dem
// blossen Bild begonnen haben, nicht an der Trennlinie oder in einem Fenster.
// Der zweite Klick eines Doppelklicks zaehlt nicht: verlaesst er das Vollbild,
// springt das Fenster unter dem Zeiger weg, und der Sprung saehe aus wie Ziehen.
void App::DragBorderlessWindow() {
  if (!config_.app.borderless || fullscreen_ || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    moveDragArmed_ = false;
    return;
  }
  if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    moveDragArmed_ = !ImGui::GetIO().WantCaptureMouse && !compareDrag_ &&
                     !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
  }
  if (!moveDragArmed_ || !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) return;
  moveDragArmed_ = false;
  clickOnPicture_ = false;
  const ImVec2 at = ImGui::GetIO().MouseClickedPos[ImGuiMouseButton_Left];
  window_.BeginMoveDrag(window_.ClientToScreen({(int)at.x, (int)at.y}));
}

void App::ShowVolumeOsd() {
  // The readout replaces a toast here: a number plus a bar says more than a
  // line of text, and it is what you want to see while a game is running.
  if (config_.app.showVolumeOsd) {
    volumeOsdStart_ = ImGui::GetTime();
  } else {
    const AudioSettings& a = config_.active().audio;
    Toast(a.mute ? T("Stumm", "Muted")
                 : Format(T("Lautstärke %.0f %%", "Volume %.0f %%"), a.volume * 100.0f));
  }
}

void App::DrawUi() {
  const Profile& profile = config_.active();
  FrameBuffer* sink = capture_.sink();

  // ---- toolbar ----
  // Windowed it is simply there; in fullscreen it follows the pointer, which is
  // already hidden after a couple of seconds of play.
  toolbarVisible_ = config_.app.showToolbar && !cropTool_.active() &&
                    (!fullscreen_ || !window_.cursorHidden());
  if (toolbarVisible_) {
    DrawToolbarStrip();
    // Everything else positions itself against the viewport work area, which is
    // exactly what it is for: the part of the window not taken by a bar. Moving
    // it here means the statistics, the toasts, the status card and the settings
    // dialog all keep clear of the toolbar without knowing it exists. ImGui
    // resets this at the start of every frame.
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float reserved = ToolbarHeight();
    vp->WorkPos.y += reserved;
    vp->WorkSize.y -= reserved;
  }

  // ---- status card, or the empty state ----
  if (!HaveLiveSignal()) {
    if (captureState_ == CaptureState::Reconnecting) {
      // This one keeps the card: it interrupts a picture that was there a moment
      // ago and is expected back, and the spinner says so.
      DrawStatusCard(T("Verbindung unterbrochen", "Connection lost"),
                     captureError_.empty() ? std::string() : captureError_, true);
    } else if (captureState_ == CaptureState::Running) {
      const VideoRenderer::SignalVerdict verdict = renderer_.detectedSignal();
      DrawIdleScreen(
          idleIcon_.id(), idleIconSize_,
          verdict == VideoRenderer::SignalVerdict::Snow
              ? T("Kein Signal — die Karte empfängt nur Rauschen. Kabel und Eingang prüfen.",
                  "No signal — the card is receiving noise only. Check the cable and input.")
              : T("Kein Signal. Quelle eingeschaltet? Richtiger Eingang gewählt?",
                  "No signal. Is the source on? Is the right input selected?"));
    } else if (!settings_.isOpen()) {
      // Beim ersten Start eine Begruessung statt einer Fehlermeldung. Es ist
      // derselbe Bildschirm -- Zeichen, Schriftzug, eine Zeile darunter --, nur
      // sagt die Zeile hier, was als Naechstes zu tun ist, statt zu melden, dass
      // etwas fehlt. Beim ersten Mal fehlt naemlich noch nichts.
      //
      // Und sie nennt beide Wege hinein. Das Rechtsklickmenue steht sonst
      // nirgends: es ist der schnellere von beiden, weil das Naheliegende darin
      // gleich anklickbar ist statt hinter einem Reiter, aber wer nicht auf die
      // Idee kommt, ins Bild zu klicken, findet es nie.
      DrawIdleScreen(
          idleIcon_.id(), idleIconSize_,
          firstRun_ ? T("Willkommen, bitte zuerst die Capture-Karte auswählen: F2 öffnet die "
                        "Einstellungen,\n"
                        "oder Rechtsklick für das Kontextmenü.",
                        "Welcome, please select your capture device first by pressing F2 to open "
                        "settings,\n"
                        "or right click for the context menu.")
                    : T("Kein Gerät aktiv — Rechtsklick oder F2 öffnet die Einstellungen.",
                        "No device active — right-click or press F2 for the settings."));
    }
  }

  // ---- Normensuche ----
  //
  // Nur solange etwas laeuft: "Eingestellt: PAL B" gehoert in den Dialog, nicht
  // dauerhaft ins Bild. Und nicht, waehrend der Dialog offen ist -- dort steht
  // dieselbe Auskunft schon, ausfuehrlicher.
  if (!settings_.isOpen()) {
    const SettingsWindow::StandardSearch search = standardSearch_.StandardSearchDisplay();
    switch (search) {
      case SettingsWindow::StandardSearch::Trying:
      case SettingsWindow::StandardSearch::Colour: {
        // Kopfzeile und Grund kommen aus einer Hand, weil sie zusammengehoeren:
        // welcher Schritt von wie vielen laeuft, haengt daran, welche der
        // beiden Stufen gerade sucht.
        std::string headline, detail;
        standardSearch_.StandardSearchText(&headline, &detail);
        DrawSearchIndicator(headline, detail);
        break;
      }
      // Und was daraus geworden ist, an derselben Stelle: die Einblendung wird
      // nicht ersetzt, sie hoert auf zu laufen. Nur nach einem Suchlauf von
      // Hand -- die Automatik sucht bei jedem Quellenwechsel, und ein Ergebnis
      // nach jedem waere kein Ergebnis mehr, sondern ein Bildschirmelement.
      case SettingsWindow::StandardSearch::Result: {
        std::string headline, detail;
        standardSearch_.StandardSearchText(&headline, &detail);
        DrawSearchResult(headline, detail, standardSearch_.ResultAgeSeconds(),
                         VideoStandardSearch::ResultSeconds());
        break;
      }
      // Die Pause ist kein Vorgang, sondern deren Abwesenheit -- dafuer laufende
      // Punkte ins Bild zu setzen, waere gelogen. Der Dialog sagt es weiterhin.
      case SettingsWindow::StandardSearch::Paused:
      case SettingsWindow::StandardSearch::Off:
        break;
    }
  }

  // ---- stats ----
  if (config_.app.showStats) {
    OverlayStats stats;
    stats.profileName = profile.name;
    stats.deviceName = capture_.resolvedDevice().name;
    const auto& inputs = capture_.capabilities().crossbarInputs;
    if (profile.capture.crossbarInput >= 0 &&
        profile.capture.crossbarInput < (int)inputs.size()) {
      stats.inputName = inputs[(size_t)profile.capture.crossbarInput].name;
    }
    stats.format = renderer_.sourceFormat();
    if (sink) stats.sink = sink->stats();
    stats.audio = audio_.stats();
    stats.presentFps = presentFps_;
    stats.frameAge = frameAgeMeter_;
    stats.audioBuffer = audioBufferMeter_;
    stats.vsync = config_.app.vsync;
    stats.tearing = display_.tearingSupported();
    // Vier Zustaende, und der erste ist derjenige, der sonst wie ein Fehler
    // aussieht: solange gemessen wird, steht das auch da. Die Erkennung braucht
    // rund eine Sekunde bewegtes Bild, und wer in dieser Sekunde hinsieht, soll
    // "wird gemessen" lesen und nicht ein "progressiv", das gleich widerrufen
    // wird.
    if (!renderer_.sourceFormat().valid()) {
      stats.scanLabel = "—";
    } else if (profile.image.deinterlaceAuto && !renderer_.sourceFormat().interlaced &&
               renderer_.detectedInterlace() == VideoRenderer::InterlaceVerdict::Pending) {
      stats.scanLabel = T("wird gemessen", "measuring");
    } else if (!SourceLooksInterlaced(profile)) {
      stats.scanLabel = T("progressiv", "progressive");
    } else if (renderer_.sourceCoSitedFields()) {
      stats.scanLabel = T("deckungsgleich (240p/288p)", "aligned (240p/288p)");
    } else if (profile.image.deinterlace == Deinterlace::Off) {
      stats.scanLabel = T("interlaced, kein Deinterlacer", "interlaced, no deinterlacer");
    } else {
      stats.scanLabel = std::string(T("interlaced, ", "interlaced, ")) +
                        DeinterlaceName((int)profile.image.deinterlace);
    }
    // Der Toast ist weg, sobald man kurz weggesehen hat, und die Einstellungen
    // sind zu. Diese Zeile ist die eine Flaeche, die dauerhaft sichtbar ist --
    // wer sich fragt, warum das Bild weicher geworden ist, findet die Antwort
    // dort, wo er ohnehin nachsieht. Ein Fragezeichen und nicht mehr: der
    // Zustand steht davor, dies ist nur der Zweifel daran.
    if (InterlaceVerdictDoubtful(profile)) stats.scanLabel += T(" (?)", " (?)");
    const Rect& r = renderer_.videoRect();
    stats.displayWidth = (int)(r.right - r.left);
    stats.displayHeight = (int)(r.bottom - r.top);
    stats.filterName = ScaleFilterName((int)profile.image.filter);
    stats.videoDelayMs = (int)delayLine_.delayMs();
    stats.detail = config_.app.statsDetail;
    // Spell out what "automatic" resolved to, since that is the setting people
    // second-guess when a picture looks wrong.
    {
      std::string range = profile.image.range == ColorRange::Auto
                              ? (detectedRangeText_ ? detectedRangeText_
                                                    : T("wird gemessen", "measuring"))
                              : ColorRangeName((int)profile.image.range);
      std::string matrix = ColorMatrixName((int)profile.image.matrix);
      stats.colorInfo = range + "  /  " + matrix;
    }
    DrawStatsPanel(stats);
  }

  // ---- crop picker ----
  if (cropTool_.active()) {
    clickOnPicture_ = false;
    cropTool_.DrawCropPicker();
    if (!cropTool_.active()) return;  // Apply or Cancel closed it this frame
  } else {
    DragCompareDivider();
    DoubleClickFullscreen();
    DragBorderlessWindow();
  }

  // ---- recording indicator ----
  if (recorder_.recording()) {
    DrawRecordIndicator(recorder_.stats().seconds, config_.app.osdCorner);
  }

  // ---- volume readout ----
  {
    const double age = ImGui::GetTime() - volumeOsdStart_;
    if (age < kVolumeOsdSeconds) {
      DrawVolumeOsd(profile.audio.volume, profile.audio.mute, config_.app.osdCorner, age,
                    kVolumeOsdSeconds);
    }
  }

  // ---- toast ----
  const float noticeLift = DrawResolutionNotice();
  DrawToastStrip(noticeLift > 0.0f ? noticeLift : DrawColourNotice());

  DrawContextMenu();

  // ---- settings ----
  // The banner explains why the dialog opened by itself. Once the card is
  // actually running the reason is gone, whichever route got it there --
  // picking a device, F5, or the automatic retry. Leaving it up until the
  // dialog is closed and reopened reads like the selection did not take.
  if (captureState_ == CaptureState::Running) settings_.ClearReason();

  switch (renderer_.detectedRange()) {
    case VideoRenderer::RangeVerdict::Limited:
      detectedRangeText_ = T("begrenzt (16-235)", "limited (16-235)");
      break;
    case VideoRenderer::RangeVerdict::Full:
      detectedRangeText_ = T("voll (0-255)", "full (0-255)");
      break;
    default:
      detectedRangeText_ = nullptr;
      break;
  }
  settings_.SetDetectedRange(&detectedRangeText_);

  // Die Zahlen hinter dem Urteil. Der Anteil unter 16 ist der, an dem es
  // haengt (die Schwelle steht bei 0,2 %), der ueber 235 steht daneben, weil
  // Superweiss in einem begrenzten Signal erlaubt ist und beim Ablesen sonst
  // wie ein Widerspruch aussieht.
  {
    const VideoRenderer::RangeNumbers n = renderer_.rangeNumbers();
    if (n.samples > 0) {
      rangeNumbersText_ = Format(T("min %d, max %d, %.2f %% unter 16, %.2f %% über 235, "
                                   "%llu Proben",
                                   "min %d, max %d, %.2f %% below 16, %.2f %% above 235, "
                                   "%llu samples"),
                                 n.min, n.max, 100.0 * (double)n.below16 / (double)n.samples,
                                 100.0 * (double)n.above235 / (double)n.samples,
                                 (unsigned long long)n.samples);
    } else {
      rangeNumbersText_.clear();
    }
  }
  settings_.SetRangeNumbers(&rangeNumbersText_);

  switch (renderer_.detectedInterlace()) {
    case VideoRenderer::InterlaceVerdict::Interlaced:
      detectedInterlaceText_ = renderer_.sourceCoSitedFields()
                                   ? T("interlaced, 240p/288p-Quelle",
                                       "interlaced, 240p/288p source")
                                   : T("interlaced", "interlaced");
      break;
    case VideoRenderer::InterlaceVerdict::Progressive:
      detectedInterlaceText_ = T("progressiv", "progressive");
      break;
    default:
      detectedInterlaceText_ = nullptr;
      break;
  }
  settings_.SetDetectedInterlace(&detectedInterlaceText_);
  settings_.SetInterlaceDoubtful(InterlaceVerdictDoubtful(profile));

  // Ein 1080p- oder 720p-Bild, das die Karte als progressiv meldet und das hier
  // trotzdem als interlaced gemessen wurde: das ist kaum je richtig, und wer
  // gerade zusieht, merkt sonst nur, dass das Bild ploetzlich weicher wird,
  // ohne den Grund zu finden. Die Meldung nennt deshalb gleich den Reiter, in
  // dem es abzustellen ist.
  {
    const bool doubtful = InterlaceVerdictDoubtful(profile);
    if (doubtful && !interlaceDoubtToasted_) {
      interlaceDoubtToasted_ = true;
      const VideoFormatInfo fmt = renderer_.sourceFormat();
      CAP_LOG("Interlacing detected at %dx%d although the card reports progressive -- hint shown",
              fmt.width, fmt.height);
      Toast(Format(T("Halbbilder bei %dx%d erkannt — bitte prüfen (Reiter Bild)",
                     "Fields detected at %dx%d — please check (Image tab)"),
                   fmt.width, fmt.height));
    } else if (!doubtful) {
      interlaceDoubtToasted_ = false;
    }
  }

  // Und der Hinweis auf einen analogen Eingang, an dem trotzdem der volle
  // Wertebereich ankommt. Anders als das Interlacing darueber gibt es dafuer
  // *keinen* Toast: es ist nichts kaputt, es gibt nichts zu bestaetigen, und die
  // Lage ist an dieser Karte der Normalfall. Wer wissen will, warum Schwarz
  // unten am Anschlag liegt, findet den Hinweis im Reiter Bild, gleich neben dem
  // Regler, um den es geht.
  {
    const bool full = AnalogueRangeIsFull();
    settings_.SetAnalogueFullRange(full);
    if (full && !analogueFullRangeLogged_) {
      analogueFullRangeLogged_ = true;
      const VideoRenderer::RangeNumbers n = renderer_.rangeNumbers();
      CAP_LOG("Analogue input delivers the full range (min %d, max %d) -- hint in the Picture tab",
              n.min, n.max);
    } else if (!full) {
      analogueFullRangeLogged_ = false;
    }
  }

  // Eine Aufloesung neben dem Raster der Norm, siehe ResolutionMismatchLines.
  // Einmal beim Eintreten: angepasst und gesagt, oder -- mit dem Haken aus --
  // gefragt, auf dem Bild und bis jemand antwortet. Der Hinweis im Reiter
  // Quelle steht, solange es so bleibt, auch nach "Ignorieren".
  {
    const int active = ResolutionMismatchLines();
    int shown = 0;
    if (active <= 0) {
      resolutionMismatchSince_ = -1.0;
      resolutionMismatchToasted_ = false;
      resolutionNoticeLines_ = 0;
    } else if (resolutionMismatchSince_ < 0.0) {
      resolutionMismatchSince_ = ImGui::GetTime();
    } else if (ImGui::GetTime() - resolutionMismatchSince_ >= 3.0) {
      shown = active;
      // Der Haken, waehrend die Frage offen ist: dann gilt er als Antwort.
      if (resolutionNoticeLines_ > 0 && config_.app.matchResolution) {
        resolutionNoticeLines_ = 0;
        ApplyFittingResolution(active);
      }
      if (!resolutionMismatchToasted_) {
        resolutionMismatchToasted_ = true;
        const VideoFormatInfo fmt = renderer_.sourceFormat();
        const bool ignored = fmt.width == resolutionIgnoredWidth_ &&
                             fmt.height == resolutionIgnoredHeight_ &&
                             active == resolutionIgnoredLines_;
        const ResolutionOption fit = capture_.capabilities().caps.FittingResolution(
            capture_.connectedFormat().subtype, active);
        const FormatSel& want = config_.active().capture.format;
        const bool canFix =
            fit.width > 0 && (want.width != fit.width || want.height != fit.height);
        if (canFix && config_.app.matchResolution) {
          ApplyFittingResolution(active);
        } else if (canFix && !ignored) {
          CAP_LOG("Resolution %dx%d does not fit the video standard (%d active lines) -- asking",
                  fmt.width, fmt.height, active);
          resolutionNoticeLines_ = active;
          resolutionNoticeText_ =
              Format(T("%dx%d passt nicht zur Videonorm (%d Zeilen)",
                       "%dx%d does not fit the video standard (%d lines)"),
                     fmt.width, fmt.height, active);
        } else if (!ignored) {
          // Die passende Groesse steht schon drin, und die Karte liefert
          // trotzdem etwas anderes: nichts, was ein Knopf beheben koennte.
          CAP_LOG("Resolution %dx%d does not fit the video standard (%d active lines) -- hint shown",
                  fmt.width, fmt.height, active);
          Toast(Format(T("%dx%d passt nicht zur Videonorm (%d Zeilen) — bitte prüfen (Reiter Quelle)",
                         "%dx%d does not fit the video standard (%d lines) — please check (Source "
                         "tab)"),
                       fmt.width, fmt.height, active));
        }
      }
    }
    settings_.SetResolutionMismatch(shown);
  }
  settings_.SetCoSitedFields(renderer_.sourceCoSitedFields());
  settings_.SetSignalLocked(standardSearch_.PollSignalLocked());
  settings_.SetStandardSearch(standardSearch_.StandardSearchDisplay());
  settings_.SetLiveStandard(capture_.running() ? standardSearch_.signalStandard()
                                               : 0);
  settings_.SetProbeAllowed(captureState_ != CaptureState::Reconnecting);
  settings_.SetUpdater(&updater_);
  settings_.SetGraphicsBackend(GraphicsApiBuilt(GraphicsApi::Vulkan),
                               display_.api() == GraphicsApi::Vulkan);
  // Auto means: analogue when the card has a decoder for it. A card that only
  // does one of the two therefore needs nobody to say which.
  // Dieselbe Quelle der Wahrheit, die auch entscheidet, was ueberhaupt noch
  // gezeichnet wird. Liefen die beiden auseinander, wirkte etwas, das nirgends
  // mehr einstellbar ist.
  settings_.SetAnalogueSource(SourceIsAnalogue());
  // Und derselbe Weg fuer den Anschluss, aufgeloest statt roh: was der Dialog
  // im Reiter Bild noch zeigen darf, haengt an dem, was "Automatisch" ergibt,
  // und nicht an dem, was dort ausgewaehlt ist.
  settings_.SetConnector(ResolvedConnector());
  // Und dieselbe Wahrheit noch einmal an den Renderer, der daran entscheidet,
  // ob die kachelweise Interlacing-Erkennung mitreden darf.
  renderer_.SetAnalogueSource(SourceIsAnalogue());
  // Und die gemessene Ankunftsrate dazu, mit der die Erkennung ein Kammurteil
  // verwerfen kann, das der Formatraum nicht hergibt -- siehe SetFrameRateHint.
  // Gemessen, nicht angekuendigt: die Karte kuendigt bei einer Halbbildquelle
  // 50 an und liefert 25 gewebte Bilder, und mit der angekuendigten Zahl haette
  // das Veto genau die Quellen erwischt, vor denen es schuetzen soll. Ohne
  // Messung 0, und 0 heisst "noch nicht gemessen", nicht "steht still".
  double measuredFps = 0.0;
  if (const FrameBuffer* sink = capture_.sink()) {
    const double measured = sink->stats().sourceFps;
    if (measured > 1.0) measuredFps = measured;
  }
  renderer_.SetFrameRateHint(measuredFps);
  settings_.SetSourceFps(renderer_.sourceFormat().fps);
  // Woraus sich entscheidet, welche Abschnitte im Reiter Bild erscheinen: die
  // Zeilenzahl fuer die Bildroehreneffekte, die Halbbilder fuer das
  // Deinterlacing. Beides aus der Quelle, nicht daraus, ob sie analog ist --
  // 1080i gibt es ueber HDMI, und 480p gibt es von einem RetroTINK.
  settings_.SetSourceHeight(renderer_.sourceFormat().valid() ? renderer_.sourceFormat().height : 0);
  settings_.SetSourceInterlaced(
      renderer_.sourceFormat().interlaced ||
      renderer_.detectedInterlace() == VideoRenderer::InterlaceVerdict::Interlaced);
  settings_.SetScanlineRoom(renderer_.scanlineRoom());
  settings_.SetLevels(audio_.inputPeak(), mic_.peak(), mic_.running());
  settings_.SetViewAids(compare_, bypass_);
  recording_.UpdateDiskSpace();
  if (settings_.takeCompareToggle()) ToggleCompare();
  if (settings_.takeBypassToggle()) ToggleBypass();
  if (settings_.takeCropPickRequest()) cropTool_.BeginCropPick();
  if (settings_.takeDeviceConfigRequest()) OpenDeviceConfig();
  if (settings_.takeCropDetectRequest()) cropTool_.DetectCrop();
  if (settings_.takeCardResetRequest()) ReinitialiseCard();
  if (settings_.takeRangeRemeasureRequest()) RemeasureRange();
  settings_.setProbeBusy(recording_.probing());
  // Drawn here only when the settings live inside the picture. The separate
  // window is deliberately not touched from in here: this runs between the main
  // context's NewFrame and Render, and presenting a second swapchain in the
  // middle of another window's frame flushes every bit of GPU work already
  // queued for it -- sixty times a second, while the preview runs at twice that
  // or more. That was not merely qBlank stuttering; it was enough to make the
  // desktop's own cursor stutter. It happens after the present instead.
  if (!settingsAreWindowed()) {
    settings_.SetFillsWindow(false);
    if (settings_.Draw(capture_.running() ? &capture_.capabilities() : nullptr,
                       &recording_.ffmpeg()) == SettingsWindow::Result::Close) {
      settings_.Close();
    }
  }
  if (settings_.takeProbeRequest()) recording_.StartEncoderProbe(true);
  notices_.DrawWelcome();
  notices_.DrawCrashNotice();
  notices_.DrawUpdatePrompt();
  if (settings_.takeRestartRequest()) QuitAndRestart();

  // Everything above may have edited the configuration in place, so act on it
  // here in one spot rather than sprinkling apply calls through the UI code.
  // Saving it waits until after the present: see RenderFrame.
  SyncConfigChanges();
}

void App::DrawContextMenu() {
  if (!ImGui::BeginPopupContextVoid("qblank_context", ImGuiPopupFlags_MouseButtonRight)) return;

  // Shortcut labels come from the live bindings, so rebinding a key is visible
  // here immediately instead of leaving the menu quietly lying about it. The
  // strings have to outlive the frame, hence the static buffer per action.
  auto sc = [this](HotkeyAction action) -> const char* {
    static std::string text[(int)HotkeyAction::Count];
    const int i = (int)action;
    text[i] = config_.hotkeys[action].bound() ? HotkeyText(config_.hotkeys[action]) : std::string();
    return text[i].empty() ? nullptr : text[i].c_str();
  };

  if (ImGui::MenuItem(T("Einstellungen...", "Settings..."), sc(HotkeyAction::Settings))) OpenSettings({});
  ImGui::Separator();

  bool fs = fullscreen_;
  if (ImGui::MenuItem(T("Vollbild", "Fullscreen"), sc(HotkeyAction::Fullscreen), &fs)) SetFullscreen(fs);

  // Whole multiples of the lines, so integer scaling does not mean hunting for
  // the size by hand. The width follows the aspect, as in the Integer mode, and
  // the toolbar is added on top so the picture itself gets the size.
  if (ImGui::BeginMenu(T("Fenstergröße", "Window size"), !fullscreen_ && renderer_.hasFrame())) {
    const int inset = toolbarVisible_ ? (int)std::lround(ToolbarHeight()) : 0;
    for (int factor = 1; factor <= 3; ++factor) {
      int w = 0, h = 0;
      if (!renderer_.PictureSizeAt(factor, &w, &h)) continue;
      const bool current = !window_.maximized() && display_.width() == w &&
                           display_.height() == h + inset;
      const std::string label = Format("%d× (%d × %d)", factor, w, h);
      if (ImGui::MenuItem(label.c_str(), nullptr, current, window_.ClientSizeFits(w, h + inset))) {
        window_.SetClientSize(w, h + inset);
      }
    }
    ImGui::EndMenu();
  }

  bool top = config_.app.alwaysOnTop;
  if (ImGui::MenuItem(T("Immer im Vordergrund", "Always on top"), nullptr, &top)) {
    config_.app.alwaysOnTop = top;
    ApplyWindowFlags();
  }

  bool borderless = config_.app.borderless;
  if (ImGui::MenuItem(T("Rahmenlos", "Borderless"), nullptr, &borderless)) config_.app.borderless = borderless;

  // Der Umfang gleich mit, wie beim Filtervergleich: eine Stufe waehlen blendet
  // die Statistik in ihr ein, dieselbe noch einmal blendet sie aus.
  if (BeginMenuWithShortcut(T("Statistik", "Statistics"), sc(HotkeyAction::Stats))) {
    const char* hint = T("Kompakt: Bildraten und Durchlaufzeit. Normal: zusätzlich Format und "
                         "Ton. Vollständig: alles. Noch einmal wählen blendet sie aus.",
                         "Compact: frame rates and pipeline delay. Normal: adds format and "
                         "audio. Full: everything. Choose it again to hide.");
    for (int i = 0; i < 3; ++i) {
      const bool on = config_.app.showStats && (int)config_.app.statsDetail == i;
      if (ImGui::MenuItem(StatsDetailName(i), nullptr, on)) {
        config_.app.showStats = !on;
        config_.app.statsDetail = (StatsDetail)i;
      }
      WrappedTooltip(hint);
    }
    ImGui::EndMenu();
  }

  bool toolbar = config_.app.showToolbar;
  if (ImGui::MenuItem(T("Werkzeugleiste", "Toolbar"), nullptr, &toolbar)) {
    config_.app.showToolbar = toolbar;
  }

  // Same reasoning as the colour menu below, only more so: whether the standard
  // is right is something you see instantly, and on a console that switches
  // between 50 and 60 Hz it is the setting you reach for most.
  //
  // Both conditions, exactly as the settings window has them. A card with an
  // analogue decoder still reports its whole standard list while it is showing
  // HDMI, and on that input every entry in it is dead: the decoder is not in the
  // path at all. Offering them anyway is a menu that does nothing, which is
  // worse than one that is not there.
  const long availableStandards =
      capture_.running() ? capture_.capabilities().availableStandards : 0;
  if (availableStandards != 0 && SourceIsAnalogue() &&
      ImGui::BeginMenu(T("Videonorm", "Video standard"))) {
    CaptureSettings& cap = config_.active().capture;
    const long before = cap.videoStandard;

    if (ImGui::MenuItem(T("Automatisch", "Automatic"), nullptr, cap.videoStandard == -1)) {
      cap.videoStandard = -1;
    }
    if (ImGui::MenuItem(T("Nicht ändern", "Leave alone"), nullptr, cap.videoStandard == 0)) {
      cap.videoStandard = 0;
    }
    ImGui::Separator();
    const int chosen = VideoStandardGroupOf(cap.videoStandard);
    for (int i = 0; i < VideoStandardGroupCount(); ++i) {
      const long value = VideoStandardGroupPick(i, availableStandards);
      if (value == 0) continue;
      if (ImGui::MenuItem(VideoStandardGroupName(i), nullptr, chosen == i)) {
        cap.videoStandard = value;
      }
      WrappedTooltip(VideoStandardGroupHint(i));
    }

    if (cap.videoStandard != before) {
      // A different standard usually means a different number of lines, so the
      // graph has to come up again around the new format. Automatic is the one
      // case that does not restart here: it has nothing to apply yet and will
      // rebuild by itself once it has found something that locks.
      if (cap.videoStandard > 0) {
        std::string error;
        if (StartCapture(&error)) {
          Toast(Format(T("Videonorm: %s", "Video standard: %s"),
                       VideoStandardPickerName(cap.videoStandard).c_str()));
        } else {
          Toast(error);
        }
      }
      SaveConfig();
    }
    ImGui::EndMenu();
  }

  // Right here rather than buried in the dialog: wrong levels or a wrong matrix
  // are things you spot by looking at the picture, and both take effect on the
  // very next frame, so switching them while watching is the fastest way to
  // land on the right one.
  if (ImGui::BeginMenu(T("Farbe", "Colour"))) {
    ImageSettings& img = config_.active().image;

    ImGui::SeparatorText(T("Wertebereich", "Range"));
    for (int i = 0; i < 3; ++i) {
      // "##range" keeps the id unique: the first entry of both lists is called
      // "Automatic", and two menu items with the same label in one menu share an
      // id, which Dear ImGui reports as a programmer error.
      const std::string label = std::string(ColorRangeName(i)) + "##range";
      if (ImGui::MenuItem(label.c_str(), nullptr, (int)img.range == i)) {
        img.range = (ColorRange)i;
        Toast(std::string(T("Wertebereich: ", "Range: ")) + ColorRangeName(i));
      }
    }

    ImGui::SeparatorText(T("Farbmatrix", "Colour matrix"));
    for (int i = 0; i < 3; ++i) {
      const std::string label = std::string(ColorMatrixName(i)) + "##matrix";
      if (ImGui::MenuItem(label.c_str(), nullptr, (int)img.matrix == i)) {
        img.matrix = (ColorMatrix)i;
        Toast(std::string(T("Farbmatrix: ", "Colour matrix: ")) + ColorMatrixName(i));
      }
    }
    ImGui::EndMenu();
  }

  // Volume lives in the menu as well as on the wheel: the menu is where you
  // look when you cannot remember the shortcut.
  AudioSettings& audio = config_.active().audio;
  bool muted = audio.mute;
  if (ImGui::MenuItem(T("Stumm", "Muted"), sc(HotkeyAction::Mute), &muted)) ToggleMute();

  ImGui::SetNextItemWidth(180.0f * uiScale_);
  float percent = audio.volume * 100.0f;
  if (ImGui::SliderFloat(T("Lautstärke", "Volume"), &percent, 0.0f, 100.0f, "%.0f %%")) {
    audio.volume = Clamp(percent / 100.0f, 0.0f, 1.0f);
    audio.mute = false;
    audio_.ApplySettings(audio);
    applied_.volume = audio.volume;
    applied_.mute = audio.mute;
  }

  if (config_.profiles.size() > 1 && ImGui::BeginMenu(T("Profil", "Profile"))) {
    for (int i = 0; i < (int)config_.profiles.size(); ++i) {
      const bool selected = (i == config_.activeProfile);
      std::string shortcut = i < 9 ? Format(T("Strg+%d", "Ctrl+%d"), i + 1) : std::string();
      if (ImGui::MenuItem(config_.profiles[(size_t)i].name.c_str(),
                          shortcut.empty() ? nullptr : shortcut.c_str(), selected)) {
        SwitchProfile(i);
      }
    }
    ImGui::EndMenu();
  }

  ImGui::Separator();
  {
    const bool rec = recorder_.recording();
    if (ImGui::MenuItem(rec ? T("Aufnahme stoppen", "Stop recording")
                            : T("Aufnahme starten", "Start recording"),
                        sc(HotkeyAction::Record))) {
      recording_.ToggleRecording();
    }
  }
  if (ImGui::MenuItem(T("Screenshot", "Screenshot"), sc(HotkeyAction::Screenshot))) RequestScreenshot();
  if (ImGui::MenuItem(T("Screenshot in die Zwischenablage", "Screenshot to clipboard"),
                      sc(HotkeyAction::ScreenshotClipboard))) {
    RequestScreenshot(true);
  }

  // Beides hilft beim Einstellen und aendert nur die Anzeige, deshalb stehen
  // sie hier und nicht in den Einstellungen: man greift danach, waehrend man
  // auf das Bild sieht.
  if (ImGui::MenuItem(T("Standbild", "Freeze"), sc(HotkeyAction::Freeze), frozen_)) {
    ToggleFreeze();
  }
  WrappedTooltip(T("Hält das Bild an, damit man Filter in Ruhe einstellen kann. Die Filter "
                   "laufen weiter, nur die Quelle steht.",
                   "Holds the picture so filters can be set in peace. The filters keep "
                   "running; only the source stands still."));
  // Die Richtung gleich mit, statt sie im Einstellungsfenster zu suchen. Die
  // Taste schaltet den Vergleich in der zuletzt gewaehlten Richtung, deshalb
  // steht sie am Menue und nicht an einer der beiden.
  if (BeginMenuWithShortcut(T("Filter vergleichen", "Compare filters"),
                            sc(HotkeyAction::Compare))) {
    const bool across = config_.active().image.compareHorizontal;
    const char* hint = T("Teilt das Bild: auf einer Seite alle Filter aus, auf der anderen an. "
                         "Das Deinterlacing läuft auf beiden Seiten, die Trennlinie lässt sich "
                         "im Bild ziehen. Noch einmal wählen schaltet den Vergleich aus.",
                         "Splits the picture: every filter off on one side, on on the other. "
                         "Deinterlacing runs on both sides, and the divider can be dragged in "
                         "the picture. Choose it again to switch off.");
    if (ImGui::MenuItem(T("Senkrecht – links ungefiltert", "Vertical – unfiltered on the left"),
                        nullptr, compare_ && !across)) {
      ChooseCompare(false);
    }
    WrappedTooltip(hint);
    if (ImGui::MenuItem(T("Waagerecht – oben ungefiltert", "Horizontal – unfiltered on top"),
                        nullptr, compare_ && across)) {
      ChooseCompare(true);
    }
    WrappedTooltip(hint);
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(T("Alle Filter aus", "All filters off"), sc(HotkeyAction::BypassFilters),
                      bypass_)) {
    ToggleBypass();
  }
  WrappedTooltip(T("Zeigt das Bild ohne Composite-Filter, Schärfen, Bildregler, natives Raster "
                   "und Bildröhre. Deinterlacing, Zuschnitt, Seitenverhältnis und Skalierung "
                   "bleiben. Wird nicht gespeichert.",
                   "Shows the picture without composite filters, sharpening, picture controls, "
                   "native pixel grid and CRT effects. Deinterlacing, crop, aspect and scaling "
                   "stay. Not saved."));

  // Der schwarze Rand ist etwas, das man sieht, und das Suchen danach gehoert
  // deshalb dorthin, wo man hinsieht, statt in einen Reiter des
  // Einstellungsfensters. Zumal die Messung nur so gut ist wie das Bild, das
  // gerade anliegt: sie sucht die Grenzen dessen, was nicht schwarz ist, und
  // auf einem Ladebildschirm sind das die Grenzen des Ladebildschirms. Wer den
  // Knopf im Vorbeigehen erreicht, drueckt ihn im richtigen Moment noch einmal.
  if (ImGui::MenuItem(T("Rand suchen", "Detect border"), sc(HotkeyAction::DetectCrop))) {
    cropTool_.DetectCrop();
  }
  WrappedTooltip(T("Schneidet den schwarzen Rand weg, den die Karte mitliefert. Braucht ein "
                   "richtiges Bild — auf Schwarz gemessen kommt Unsinn heraus.",
                   "Crops the black border the card delivers. Needs a real picture — measured "
                   "on black it produces nonsense."));

  // Und daneben dasselbe fuer die Norm. Der Grund, es hier anzubieten, ist
  // derselbe wie beim Rand: ausgeloest wird es, weil man etwas *sieht* -- ein
  // Bild, dessen Farben nicht stimmen --, und was man sieht, sieht man nicht im
  // Einstellungsfenster.
  if (ImGui::MenuItem(T("Videonorm suchen", "Detect video standard"),
                      sc(HotkeyAction::DetectStandard))) {
    standardSearch_.RescanVideoStandard();
  }
  WrappedTooltip(T("Sucht die Videonorm neu, auch wenn die eingestellte hält. Für den Fall, "
                   "dass die Farben falsch sind, ohne dass es sich messen ließe.",
                   "Searches for the video standard again, even when the current one holds. "
                   "For colours that are wrong in a way no measurement catches."));

  // Und die dritte Messung derselben Art. Sie steht hier vor allem deshalb,
  // weil sie sonst nur im Einstellungsfenster zu erreichen waere -- und wer im
  // Treiber der Karte etwas umstellt, hat qBlank im Ruecken, nicht offen.
  if (ImGui::MenuItem(T("Wertebereich neu messen", "Measure range again"),
                      sc(HotkeyAction::RemeasureRange))) {
    RemeasureRange();
  }
  WrappedTooltip(T("Das Urteil über voll oder begrenzt steht, bis sich das Bildformat ändert. "
                   "Nach einer Umstellung im Treiber der Karte hiermit neu messen.",
                   "The verdict on full or limited holds until the picture format changes. "
                   "After changing something in the card's driver, measure again here."));

  if (ImGui::MenuItem(T("Aufnahme neu starten", "Restart capture"), sc(HotkeyAction::RestartCapture))) RestartAll(true);
  if (ImGui::MenuItem(T("Karte neu einlesen", "Reinitialise card"), sc(HotkeyAction::ReinitCard))) ReinitialiseCard();
  if (ImGui::MenuItem(T("Beenden", "Quit"), "Alt+F4")) window_.RequestClose();

  ImGui::EndPopup();
}

}  // namespace cap
