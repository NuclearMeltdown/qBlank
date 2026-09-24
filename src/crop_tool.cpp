#include "crop_tool.h"

#include <cmath>
#include <cstdio>

#include "i18n.h"
#include "imgui.h"
#include "render/video_renderer.h"

namespace cap {

CropTool::CropTool(Config& config, VideoRenderer& renderer, Host& host)
    : config_(config), renderer_(renderer), host_(host) {}

void CropTool::DetectCrop() {
  int left = 0, top = 0, right = 0, bottom = 0;
  if (!renderer_.contentBounds(&left, &top, &right, &bottom)) {
    host_.Toast(T("Noch nichts gemessen. Einen Moment warten.",
            "Nothing measured yet. Give it a moment."));
    return;
  }
  const VideoFormatInfo format = renderer_.sourceFormat();
  if (!format.valid()) return;

  // Wieviel vom Bild ueberhaupt uebrig bliebe -- und das ist die Frage, die vor
  // dem Zuschneiden zu stellen ist.
  //
  // Die Messung sucht die Grenzen dessen, was nicht schwarz ist, und kann
  // zwischen einem Rand und einer dunklen Stelle nicht unterscheiden. Zeigt
  // eine Konsole gerade nur ihr Startlogo auf Schwarz, ist das gemessene
  // Rechteck das Logo, und alles darum wuerde weggeschnitten: Bildflaeche, die
  // in diesem Moment nur nicht beleuchtet ist.
  //
  // Gemessen wird die Flaeche, nicht die einzelne Kante, und das ist der
  // Unterschied, auf den es ankommt: ein echter Rand frisst eine Richtung, ein
  // Logo auf Schwarz frisst beide.
  //
  // Der breiteste Rand, der noch einer ist, ist ein Kinoformat in 4:3 --
  // 2,35:1 laesst 57 Prozent der Hoehe und damit auch 57 Prozent der Flaeche
  // stehen. Ein 4:3-Bild in einem 16:9-Signal laesst 75 Prozent der Breite.
  // Der Startbildschirm des GameCube dagegen, an einem 720x576-Signal
  // nachgerechnet: 45 Prozent der Breite, 73 Prozent der Hoehe, zusammen 33
  // Prozent der Flaeche -- ueber die Kanten allein waeren das nur fuenf Punkte
  // Abstand zur Haelfte, ueber die Flaeche sind es siebzehn.
  //
  // Die zweite Schranke ist nur gegen den entarteten Fall: ein schmaler
  // Streifen kann die halbe Flaeche halten und trotzdem kein Rand sein.
  const int keptW = right - left + 1;
  const int keptH = bottom - top + 1;
  const double partW = format.width > 0 ? (double)keptW / format.width : 1.0;
  const double partH = format.height > 0 ? (double)keptH / format.height : 1.0;
  if (partW * partH < 0.5 || partW < 0.4 || partH < 0.4) {
    char text[240];
    std::snprintf(text, sizeof(text),
                  T("Da blieben nur %.0f Prozent des Bildes stehen (%.0f x %.0f). Das sieht "
                    "nach einem Logo auf Schwarz aus, nicht nach einem Rand -- erst ein "
                    "richtiges Bild der Konsole abwarten.",
                    "That would leave only %.0f per cent of the picture (%.0f x %.0f). It "
                    "looks like a logo on black rather than a border -- wait for a real "
                    "picture from the console first."),
                  partW * partH * 100.0, partW * 100.0, partH * 100.0);
    host_.Toast(text);
    CAP_LOG("Crop discarded: only %dx%d of %dx%d left (%.0f %% of the area)", keptW, keptH,
            format.width, format.height, partW * partH * 100.0);
    return;
  }

  ImageSettings& img = config_.active().image;
  const int cl = left;
  const int ct = top;
  const int cr = format.width - 1 - right;
  const int cb = format.height - 1 - bottom;
  // A border of a pixel or two is measurement noise on an analogue input, not a
  // border, and cropping it would only cost resolution.
  const int floorPx = 3;
  img.cropLeft = cl >= floorPx ? cl : 0;
  img.cropTop = ct >= floorPx ? ct : 0;
  img.cropRight = cr >= floorPx ? cr : 0;
  img.cropBottom = cb >= floorPx ? cb : 0;

  if (img.cropLeft || img.cropRight || img.cropTop || img.cropBottom) {
    char text[160];
    std::snprintf(text, sizeof(text),
                  T("Rand erkannt: links %d, rechts %d, oben %d, unten %d",
                    "Border found: left %d, right %d, top %d, bottom %d"),
                  img.cropLeft, img.cropRight, img.cropTop, img.cropBottom);
    host_.Toast(text);
  } else {
    host_.Toast(T("Kein schwarzer Rand gefunden.", "No black border found."));
  }
}

// Ein Zuschnitt gilt fuer die Groesse, an der er gemessen wurde.
//
// Die vier Zahlen sind Bildpunkte der Quelle, nicht Anteile: "links 20" heisst
// zwanzig Punkte von 720. Wechselt die Norm von 525 auf 625 Zeilen, wechselt
// mit ihr die Bildhoehe, und "oben 17" beschreibt dann einen anderen Streifen
// als den gemessenen. Im besten Fall steht ein schmaler schwarzer Rand wieder
// im Bild, im schlechteren wird echter Bildinhalt weggeschnitten -- und beides
// sieht nicht nach einer Einstellung aus, die noch von vorhin steht, sondern
// nach einem kaputten Bild.
//
// Zurueckgesetzt statt neu gemessen, und das ist eine Entscheidung. Ein
// automatischer zweiter Anlauf laege nahe -- die Norm hat gerade gewechselt,
// gleich einmal nachmessen --, aber genau in diesem Moment ist das Bild am
// wenigsten dazu geeignet: der Graph ist eben erst wieder aufgebaut, die
// Konsole schaltet gerade um oder faehrt hoch, und was anliegt, ist ein
// Startlogo auf Schwarz oder noch gar nichts. Die Messung hat gegen genau
// diesen Fall bereits eine Schranke (siehe DetectCrop), aber sie noch dazu
// ungefragt in ihn hineinzuschicken hiesse, sie gegen ihre eigene Schranke
// laufen zu lassen. Also: sauber aufraeumen, es sagen, und die Entscheidung
// dem ueberlassen, der das Bild sieht -- der Knopf dafuer liegt jetzt im
// Rechtsklickmenue.
//
// Wer will, kann es sich stattdessen merken lassen (`cropPerFormat`). Ein
// Profil ist die Beschreibung einer Quelle, und eine Quelle kann zwei Groessen
// haben: derselbe GameCube liefert 576 Zeilen im PAL-Modus und 480 im
// 60-Hz-Modus, und beide Male haengt ein anderer schwarzer Rand daran. Das sind
// nicht zwei Quellen, also sollen es nicht zwei Profile sein muessen. Gemessen
// wird weiterhin von Hand -- gespeichert wird nur, was gemessen wurde, und zwar
// unter der Groesse, bei der es gemessen wurde.
// Den Zuschnitt, wie er gerade steht, unter einer Bildgroesse ablegen.
//
// Ein Zuschnitt aus lauter Nullen ist kein Zuschnitt, sondern seine Abwesenheit
// -- und die ist auch das, was ohne Eintrag geschieht. Er wird deshalb nicht
// gespeichert, sondern loescht einen vorhandenen Eintrag: sonst fuellt sich die
// Liste mit Groessen, unter denen nichts steht, und die Zeile in den
// Einstellungen, die sie aufzaehlt, zaehlt Nichts auf.
static void StoreCropVariant(ImageSettings& img, int w, int h) {
  if (w <= 0 || h <= 0) return;
  const bool empty = !img.cropLeft && !img.cropRight && !img.cropTop && !img.cropBottom;
  for (auto it = img.cropVariants.begin(); it != img.cropVariants.end(); ++it) {
    if (it->width != w || it->height != h) continue;
    if (empty) {
      img.cropVariants.erase(it);
      return;
    }
    it->left = img.cropLeft;
    it->right = img.cropRight;
    it->top = img.cropTop;
    it->bottom = img.cropBottom;
    return;
  }
  if (empty) return;
  CropForFormat v;
  v.width = w;
  v.height = h;
  v.left = img.cropLeft;
  v.right = img.cropRight;
  v.top = img.cropTop;
  v.bottom = img.cropBottom;
  img.cropVariants.push_back(v);
}

static const CropForFormat* FindCropVariant(const ImageSettings& img, int w, int h) {
  for (const CropForFormat& v : img.cropVariants) {
    if (v.width == w && v.height == h) return &v;
  }
  return nullptr;
}

void CropTool::UpdateCropForFormat() {
  const VideoFormatInfo fmt = renderer_.sourceFormat();
  if (!fmt.valid()) return;
  // Waehrend des Ziehens sind die Zahlen ohnehin auf null gesetzt und das
  // Format zu merken waere verfrueht.
  if (cropPick_.active) return;

  ImageSettings& img = config_.active().image;
  const int w = fmt.width;
  const int h = fmt.height;

  if (cropFormatWidth_ == w && cropFormatHeight_ == h) {
    // Nichts gewechselt -- aber vielleicht wurde am Zuschnitt geschraubt.
    //
    // Jeden Weg dorthin einzeln zu benachrichtigen hiesse, vier Regler, den
    // Rahmen zum Ziehen, die Messung und den Menuepunkt zum Zuruecksetzen an
    // dieselbe Buchhaltung zu haengen und beim naechsten Weg daran zu denken.
    // Hier steht ohnehin jedes Bild ein Vergleich an; er kostet vier Zahlen.
    if (img.cropPerFormat) StoreCropVariant(img, w, h);
    return;
  }

  // Das erste Format einer Sitzung hat nichts geaendert; es ist das, wofuer
  // die gespeicherten Zahlen gelten sollen -- es sei denn, fuer genau diese
  // Groesse steht etwas Eigenes in der Liste. Dann ist das die juengere
  // Auskunft: die vier Zahlen oben gehoeren zu der Groesse, bei der zuletzt
  // aufgehoert wurde, und das muss nicht die sein, mit der es weitergeht.
  const bool first = cropFormatWidth_ == 0 && cropFormatHeight_ == 0;
  const int wasW = cropFormatWidth_;
  const int wasH = cropFormatHeight_;
  cropFormatWidth_ = w;
  cropFormatHeight_ = h;

  if (img.cropPerFormat) {
    if (!first) StoreCropVariant(img, wasW, wasH);
    const CropForFormat* v = FindCropVariant(img, w, h);
    if (v) {
      const bool same = img.cropLeft == v->left && img.cropRight == v->right &&
                        img.cropTop == v->top && img.cropBottom == v->bottom;
      img.cropLeft = v->left;
      img.cropRight = v->right;
      img.cropTop = v->top;
      img.cropBottom = v->bottom;
      if (first || same) return;
      CAP_LOG("Crop for %dx%d applied (left %d, right %d, top %d, bottom %d)", w, h,
              v->left, v->right, v->top, v->bottom);
      host_.Toast(Format(T("Videoformat geändert (%dx%d) — gespeicherter Zuschnitt eingesetzt.",
                     "Video format changed (%dx%d) — stored crop applied."),
                   w, h));
      return;
    }
    if (first) return;
  } else if (first) {
    return;
  }

  if (!img.cropLeft && !img.cropRight && !img.cropTop && !img.cropBottom) return;

  CAP_LOG("Crop reset: source now %dx%d (border was left %d, right %d, top %d, bottom %d)",
          w, h, img.cropLeft, img.cropRight, img.cropTop, img.cropBottom);
  img.cropLeft = 0;
  img.cropRight = 0;
  img.cropTop = 0;
  img.cropBottom = 0;
  host_.Toast(Format(T("Videoformat geändert (%dx%d) — Zuschnitt zurückgesetzt.",
                 "Video format changed (%dx%d) — crop reset."),
               w, h));
}

// ------------------------------------------------------------- crop picker
//
// Typing four numbers and checking the result is a loop nobody enjoys, so the
// edges can be dragged on the picture instead. While picking, the crop is set
// to zero so the whole frame is visible -- otherwise you would be cropping an
// already cropped image and the numbers would compound.

void CropTool::BeginCropPick() {
  if (cropPick_.active) return;
  const VideoFormatInfo format = renderer_.sourceFormat();
  if (!format.valid() || !renderer_.hasFrame()) {
    host_.Toast(T("Kein Bild zum Zuschneiden.", "No picture to crop."));
    return;
  }

  ImageSettings& img = config_.active().image;
  cropPick_.saved = img;
  cropPick_.left = img.cropLeft;
  cropPick_.right = img.cropRight;
  cropPick_.top = img.cropTop;
  cropPick_.bottom = img.cropBottom;
  cropPick_.drag = -1;
  cropPick_.active = true;

  // Show the full frame underneath, so screen position maps straight to source
  // pixels and the handles start where the current crop is.
  img.cropLeft = img.cropRight = img.cropTop = img.cropBottom = 0;
  host_.CloseSettings();
}

void CropTool::EndCropPick(bool apply) {
  if (!cropPick_.active) return;
  ImageSettings& img = config_.active().image;
  img = cropPick_.saved;
  if (apply) {
    img.cropLeft = cropPick_.left;
    img.cropRight = cropPick_.right;
    img.cropTop = cropPick_.top;
    img.cropBottom = cropPick_.bottom;
    CAP_LOG("Crop set: left %d, right %d, top %d, bottom %d", img.cropLeft,
            img.cropRight, img.cropTop, img.cropBottom);
  }
  cropPick_.active = false;
  host_.OpenSettings();
}

void CropTool::DrawCropPicker() {
  const VideoFormatInfo format = renderer_.sourceFormat();
  const Rect& r = renderer_.videoRect();
  const float rw = (float)(r.right - r.left);
  const float rh = (float)(r.bottom - r.top);
  if (!format.valid() || rw < 8.0f || rh < 8.0f) {
    EndCropPick(false);
    return;
  }

  const float srcW = (float)format.width;
  const float srcH = (float)format.height;
  const float scaleX = rw / srcW;
  const float scaleY = rh / srcH;

  // Source pixels <-> client pixels.
  auto toScreenX = [&](int src) { return (float)r.left + (float)src * scaleX; };
  auto toScreenY = [&](int src) { return (float)r.top + (float)src * scaleY; };
  auto toSrcX = [&](float screen) { return (int)std::lround((screen - (float)r.left) / scaleX); };
  auto toSrcY = [&](float screen) { return (int)std::lround((screen - (float)r.top) / scaleY); };

  // Hintergrundliste, nicht Vordergrundliste: ImGui zeichnet die Vordergrundliste
  // nach allen Fenstern, die Hintergrundliste davor. Beide liegen ueber dem
  // Video, denn das Bild kommt gar nicht aus ImGui -- es steht schon im
  // Rueckpuffer, bevor hier irgendetwas gezeichnet wird.
  //
  // Im Vordergrund lag die Abdunklung ueber der eigenen Werkzeugleiste: zieht
  // man eine Kante ueber sie hinweg, waechst das abgedunkelte Feld darueber und
  // Übernehmen und Abbrechen werden unlesbar -- genau in dem Moment, in dem man
  // sie braucht. Dieselbe Ordnung, die der Ruhebildschirm in overlay.cpp
  // benutzt, und aus demselben Grund.
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  const ImVec2 mouse = ImGui::GetMousePos();

  float xL = toScreenX(cropPick_.left);
  float xR = toScreenX(format.width - cropPick_.right);
  float yT = toScreenY(cropPick_.top);
  float yB = toScreenY(format.height - cropPick_.bottom);

  // ---- grab handling ----
  // Everything outside the picture is ignored, so dragging the window or using
  // the buttons above still works.
  const float grab = 10.0f;
  const bool overVideo = mouse.x >= r.left - grab && mouse.x <= r.right + grab &&
                         mouse.y >= r.top - grab && mouse.y <= r.bottom + grab;

  int hot = -1;
  if (cropPick_.drag >= 0) {
    hot = cropPick_.drag;
  } else if (overVideo && !ImGui::GetIO().WantCaptureMouse) {
    float best = grab;
    if (std::abs(mouse.x - xL) < best) { best = std::abs(mouse.x - xL); hot = 0; }
    if (std::abs(mouse.x - xR) < best) { best = std::abs(mouse.x - xR); hot = 1; }
    if (std::abs(mouse.y - yT) < best) { best = std::abs(mouse.y - yT); hot = 2; }
    if (std::abs(mouse.y - yB) < best) { best = std::abs(mouse.y - yB); hot = 3; }
  }

  if (hot >= 0) {
    ImGui::SetMouseCursor(hot < 2 ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
  }
  if (hot >= 0 && cropPick_.drag < 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    cropPick_.drag = hot;
  }
  if (cropPick_.drag >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    cropPick_.drag = -1;
  }

  if (cropPick_.drag >= 0) {
    // At least sixteen source pixels have to survive in each direction --
    // a zero sized picture is not a crop, it is a crash waiting to happen.
    const int minKeep = 16;
    switch (cropPick_.drag) {
      case 0:
        cropPick_.left = Clamp(toSrcX(mouse.x), 0, format.width - cropPick_.right - minKeep);
        break;
      case 1:
        cropPick_.right =
            Clamp(format.width - toSrcX(mouse.x), 0, format.width - cropPick_.left - minKeep);
        break;
      case 2:
        cropPick_.top = Clamp(toSrcY(mouse.y), 0, format.height - cropPick_.bottom - minKeep);
        break;
      case 3:
        cropPick_.bottom =
            Clamp(format.height - toSrcY(mouse.y), 0, format.height - cropPick_.top - minKeep);
        break;
      default: break;
    }
    xL = toScreenX(cropPick_.left);
    xR = toScreenX(format.width - cropPick_.right);
    yT = toScreenY(cropPick_.top);
    yB = toScreenY(format.height - cropPick_.bottom);
  }

  // ---- painting ----
  const ImU32 dim = IM_COL32(0, 0, 0, 150);
  dl->AddRectFilled(ImVec2((float)r.left, (float)r.top), ImVec2(xL, (float)r.bottom), dim);
  dl->AddRectFilled(ImVec2(xR, (float)r.top), ImVec2((float)r.right, (float)r.bottom), dim);
  dl->AddRectFilled(ImVec2(xL, (float)r.top), ImVec2(xR, yT), dim);
  dl->AddRectFilled(ImVec2(xL, yB), ImVec2(xR, (float)r.bottom), dim);

  const ImU32 line = IM_COL32(255, 255, 255, 230);
  const ImU32 lineHot = IM_COL32(255, 200, 80, 255);
  dl->AddRect(ImVec2(xL, yT), ImVec2(xR, yB), line, 0.0f, 1.5f);

  // A thicker bar on each edge, so there is something obvious to aim at.
  const float bar = 4.0f;
  dl->AddRectFilled(ImVec2(xL - bar * 0.5f, yT), ImVec2(xL + bar * 0.5f, yB),
                    hot == 0 ? lineHot : line);
  dl->AddRectFilled(ImVec2(xR - bar * 0.5f, yT), ImVec2(xR + bar * 0.5f, yB),
                    hot == 1 ? lineHot : line);
  dl->AddRectFilled(ImVec2(xL, yT - bar * 0.5f), ImVec2(xR, yT + bar * 0.5f),
                    hot == 2 ? lineHot : line);
  dl->AddRectFilled(ImVec2(xL, yB - bar * 0.5f), ImVec2(xR, yB + bar * 0.5f),
                    hot == 3 ? lineHot : line);

  // ---- toolbar ----
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 18.0f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.92f);
  if (ImGui::Begin("##croptools", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
    ImGui::TextUnformatted(T("Ränder ziehen", "Drag the edges"));
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Text(T("links %d  rechts %d  oben %d  unten %d", "left %d  right %d  top %d  bottom %d"),
                cropPick_.left, cropPick_.right, cropPick_.top, cropPick_.bottom);
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Text(T("Ergebnis %dx%d", "Result %dx%d"),
                format.width - cropPick_.left - cropPick_.right,
                format.height - cropPick_.top - cropPick_.bottom);

    ImGui::SameLine();
    if (ImGui::Button(T("Übernehmen", "Apply"))) {
      EndCropPick(true);
      ImGui::End();
      return;
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Abbrechen", "Cancel"))) {
      EndCropPick(false);
      ImGui::End();
      return;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Nichts##crop", "None##crop"))) {
      cropPick_.left = cropPick_.right = cropPick_.top = cropPick_.bottom = 0;
    }
  }
  ImGui::End();
}

}  // namespace cap
