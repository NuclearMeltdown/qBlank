#include "ui/overlay.h"

#include <cmath>

#include "i18n.h"
#include "imgui.h"

namespace cap {
namespace {

const ImGuiWindowFlags kOverlayFlags = ImGuiWindowFlags_NoDecoration |
                                       ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoInputs;

void Row(const char* label, const std::string& value) {
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::TextDisabled("%s", label);
  ImGui::TableNextColumn();
  ImGui::TextUnformatted(value.c_str());
}

std::string Decimal(double v, int digits) {
  std::string s = Format("%.*f", digits, v);
  if (CurrentLanguage() == Language::German) {
    for (char& c : s) {
      if (c == '.') c = ',';
    }
  }
  return s;
}

// Vier Anzeigen in der Sekunde. Schnell genug, dass die Zahl lebt und man eine
// Aenderung sofort bemerkt, langsam genug, dass man sie lesen kann.
const double kStatWindowSeconds = 0.25;

// Wie lange ein Ausschlag stehen bleibt, nachdem er vorbei ist. Ohne das waere
// eine Spitze fuer eine Viertelsekunde zu sehen und danach fort -- messbar,
// aber nicht ablesbar, was fuer eine Anzeige dasselbe ist wie gar nicht da.
//
// Fuenf Sekunden, weil man beim Spielen nicht auf das Panel schaut, sondern
// erst hinsieht, *nachdem* etwas geruckelt hat. Bis der Blick von der Mitte des
// Bildes nach links oben gewandert ist, vergehen ein, zwei Sekunden; ein Wert,
// der schon weg ist, wenn man ankommt, beantwortet die Frage nicht, wegen der
// man hingesehen hat.
const double kStatHoldSeconds = 5.0;

// Ob ein Extremwert eine Erwaehnung wert ist. Er muss beides sein: deutlich vom
// Mittel entfernt und absolut weit genug weg, um etwas zu bedeuten. Jeder Test
// allein geht daneben -- eine Durchlaufzeit, die von 2 auf 4 ms steigt, hat
// sich verdoppelt und heisst nichts, und 3 ms auf einem 60-ms-Tonpuffer sind
// normales Atmen.
bool StandsOut(double value, double average) {
  const double d = value > average ? value - average : average - value;
  if (d < 1.0) return false;
  return average <= 0.0 || d / average >= 0.15;
}

// Places an overlay window in one of the four corners, inset from the edge.
void PositionInCorner(OsdCorner corner, float inset) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const bool right = (corner == OsdCorner::TopRight || corner == OsdCorner::BottomRight);
  const bool bottom = (corner == OsdCorner::BottomLeft || corner == OsdCorner::BottomRight);

  const ImVec2 pos(vp->WorkPos.x + (right ? vp->WorkSize.x - inset : inset),
                   vp->WorkPos.y + (bottom ? vp->WorkSize.y - inset : inset));
  ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
}

}  // namespace

void StatMeter::Sample(double value, double now) {
  if (count_ == 0) {
    windowStart_ = now;
    windowLow_ = value;
    windowHigh_ = value;
  } else {
    if (value < windowLow_) windowLow_ = value;
    if (value > windowHigh_) windowHigh_ = value;
  }
  sum_ += value;
  ++count_;

  if (!sinceValid_) {
    sinceLow_ = value;
    sinceHigh_ = value;
    sinceValid_ = true;
  } else {
    if (value < sinceLow_) sinceLow_ = value;
    if (value > sinceHigh_) sinceHigh_ = value;
  }

  if (now - windowStart_ < kStatWindowSeconds) return;

  average = sum_ / (double)count_;

  // Gezeigt wird der **juengste** Ausschlag, nicht der groesste. Jeder frische
  // ersetzt den vorigen und stellt die Uhr zurueck, auch wenn er kleiner ist.
  //
  // Das ist die Entscheidung, die man beim Zusehen braucht. Wer hinsieht, weil
  // es gerade geruckelt hat, will wissen, was gerade passiert ist -- eine
  // groessere Zahl von vor vier Sekunden beantwortet eine andere Frage und
  // sieht dabei aus, als sei sie die Antwort. Und mit dem Groessten stuende bei
  // einer Serie von Rucklern nur der erste da, waehrend es weiterruckelt.
  //
  // Was "frisch" heisst, ist dieselbe Bedingung, an der das Panel entscheidet,
  // ob es den Wert ueberhaupt nennt: die Uhr laeuft genau so lange, wie es
  // etwas zu sagen gaebe. Laeuft sie ab, faellt der Wert auf das zurueck, was
  // das laufende Fenster hergibt -- also auf den Normalzustand.
  const bool freshHigh = StandsOut(windowHigh_, average);
  const bool freshLow = StandsOut(windowLow_, average);
  if (!valid || freshHigh || now - highAt_ >= kStatHoldSeconds) {
    high = windowHigh_;
    highAt_ = now;
  }
  if (!valid || freshLow || now - lowAt_ >= kStatHoldSeconds) {
    low = windowLow_;
    lowAt_ = now;
  }
  valid = true;

  sum_ = 0.0;
  count_ = 0;
}

void StatMeter::TakeRange(double* lowOut, double* highOut) {
  if (lowOut) *lowOut = sinceValid_ ? sinceLow_ : average;
  if (highOut) *highOut = sinceValid_ ? sinceHigh_ : average;
  sinceValid_ = false;
}

void DrawStatsPanel(const OverlayStats& s) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 16.0f, vp->WorkPos.y + 16.0f), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.78f);

  if (!ImGui::Begin("##stats", nullptr, kOverlayFlags)) {
    ImGui::End();
    return;
  }

  // Compact is what you want while playing: is it running at rate, and how old
  // is the picture. Everything else is diagnosis, and diagnosis can wait until
  // you go looking for it.
  const bool normal = s.detail != StatsDetail::Compact;
  const bool full = s.detail == StatsDetail::Full;

  if (ImGui::BeginTable("statstable", 2, ImGuiTableFlags_SizingFixedFit)) {
    if (full) {
      Row(T("Profil", "Profile"), s.profileName);
      if (!s.deviceName.empty()) Row(T("Gerät", "Device"), s.deviceName);
      if (!s.inputName.empty()) Row(T("Eingang", "Input"), s.inputName);
    }

    if (s.format.valid()) {
      if (normal) {
        std::string src = Format("%dx%d  %s", s.format.width, s.format.height,
                                 s.format.subtypeLabel.c_str());
        if (s.format.interlaced) src += "  interlaced";
        Row(T("Quelle", "Source"), src);
        if (!s.colorInfo.empty()) Row(T("Farbe", "Colour"), s.colorInfo);
      }
      Row(T("Quellrate", "Source rate"),
          Decimal(s.sink.sourceFps, 2) + T(" fps  (gemeldet ", " fps  (reported ") +
              Decimal(s.format.fps, 2) + ")");
    } else {
      Row(T("Quelle", "Source"), "—");
    }

    if (normal) {
      Row(T("Anzeige", "Display"),
          Format("%dx%d  %s", s.displayWidth, s.displayHeight, s.filterName));
    }
    Row(T("Ausgabe", "Output"),
        Decimal(s.presentFps, 1) + (s.vsync ? T(" fps  VSync an", " fps  VSync on")
                                            : T(" fps  VSync aus", " fps  VSync off")) +
            ((!s.vsync && s.tearing) ? T(", Tearing erlaubt", ", tearing allowed") : ""));
    if (normal) Row(T("Abtastung", "Scan"), s.scanLabel);

    if (full) {
      Row(T("Bilder", "Frames"),
          Format(T("%llu angezeigt, %llu verworfen", "%llu shown, %llu dropped"),
                 (unsigned long long)s.sink.displayed, (unsigned long long)s.sink.dropped));
    }
    // Die Durchlaufzeit ist eine Latenz: nach unten ist alles gut, nur der
    // Ausschlag nach oben ist eine Nachricht. Deshalb hier nur die Spitze und
    // keine Spanne.
    //
    // Sie heisst nicht mehr Bildalter, weil sie es nie war. Sie misst die
    // Strecke von der Ankunft eines Bildes bis zu dem Augenblick, in dem es
    // qBlank verlaesst -- und nur die. Was die Karte davor gebraucht hat und
    // was der Schirm danach noch tut, kann von hier aus niemand messen.
    if (s.frameAge.valid) {
      std::string age = Decimal(s.frameAge.average, 1) + " ms";
      if (StandsOut(s.frameAge.high, s.frameAge.average)) {
        age += T("  (Spitze ", "  (peak ") + Decimal(s.frameAge.high, 1) + ")";
      }
      Row(T("Durchlauf", "Pipeline"), age);
    } else {
      Row(T("Durchlauf", "Pipeline"), "—");
    }
    if (full && s.videoDelayMs > 0) {
      Row(T("Bildverzögerung", "Video delay"),
          Format(T("%d ms (A/V-Versatz)", "%d ms (A/V offset)"), s.videoDelayMs));
    }

    if (s.audio.running) {
      if (full) {
        Row(T("Ton ein", "Audio in"),
            s.audio.inputName + (s.audio.directShowInput ? "  [DirectShow]" : "  [WASAPI]"));
        Row(T("Ton aus", "Audio out"),
            s.audio.outputName + (s.audio.exclusive ? "  [Exclusive]" : "  [Shared]"));
      }
      if (normal) {
        // Beim Tonpuffer zaehlen beide Richtungen und sie bedeuten Verschiedenes:
        // ein Puffer, der leerlaeuft, knackt gleich, und einer, der sich
        // aufstaut, kostet Verzoegerung. Deshalb die Spanne, wenn es nach beiden
        // Seiten ausschlug, sonst die eine Seite, die es tat.
        std::string swing;
        const bool hi = s.audioBuffer.valid && StandsOut(s.audioBuffer.high, s.audioBuffer.average);
        const bool lo = s.audioBuffer.valid && StandsOut(s.audioBuffer.low, s.audioBuffer.average);
        if (hi && lo) {
          swing = Decimal(s.audioBuffer.low, 1) + " - " + Decimal(s.audioBuffer.high, 1) + ", ";
        } else if (hi) {
          swing = T("Spitze ", "peak ") + Decimal(s.audioBuffer.high, 1) + ", ";
        } else if (lo) {
          swing = T("Einbruch ", "dip ") + Decimal(s.audioBuffer.low, 1) + ", ";
        }
        const double shown = s.audioBuffer.valid ? s.audioBuffer.average : s.audio.bufferMs;
        Row(T("Tonpuffer", "Audio buffer"),
            Decimal(shown, 1) + " ms  (" + swing + T("Ziel ", "target ") +
                Decimal(s.audio.targetMs, 0) + " ms)");
      }
      if (full) {
        Row(T("Tonformat", "Audio format"),
            Format("%d Hz -> %d Hz", s.audio.captureRate, s.audio.renderRate));
      }
      if (s.audio.underruns || s.audio.overruns) {
        Row(T("Tonaussetzer", "Audio glitches"),
            Format(T("%llu leer, %llu übergelaufen", "%llu underrun, %llu overrun"),
                   (unsigned long long)s.audio.underruns,
                   (unsigned long long)s.audio.overruns));
      }
    } else if (normal) {
      Row(T("Ton", "Audio"), T("aus", "off"));
    }
    ImGui::EndTable();
  }

  ImGui::End();
}

void DrawIdleScreen(unsigned long long icon, int iconPixels, const std::string& detail) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();

  // A scrim over whatever the card is putting out, because that is not always
  // something you can read text on. A muted input is a flat blue and would be
  // fine; an unterminated analogue one is snow, and a wall of moving noise
  // behind the wordmark is unreadable. Drawn into the background list, which
  // sits over the video and under every window.
  //
  // Not opaque: what the card sends is still worth seeing, and the difference
  // between blue, black and snow is exactly what tells you which cable to go
  // and look at.
  ImVec4 scrim = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
  scrim.w = 0.88f;
  ImGui::GetBackgroundDrawList()->AddRectFilled(
      vp->WorkPos, ImVec2(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y),
      ImGui::GetColorU32(scrim));

  // Scaled against the window rather than fixed: the icon should read as the
  // subject of the window at any size, and 256 pixels in a 3840 wide one is a
  // stamp in the middle of a field.
  float size = (float)iconPixels;
  const float wide = vp->WorkSize.x * 0.18f;
  const float tall = vp->WorkSize.y * 0.30f;
  const float want = wide < tall ? wide : tall;
  if (want < size) size = want;
  if (size < 48.0f) size = 48.0f;

  ImGui::SetNextWindowPos(
      ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
      ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  // No panel behind it, and no border either -- the border is a style variable
  // and survives a transparent background, which leaves a rectangle drawn around
  // nothing. The card has both because it sits over a picture and has to stay
  // readable; here there is nothing underneath to separate it from.
  ImGui::SetNextWindowBgAlpha(0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

  // Explicitly at the back. ImGui orders windows by focus history rather than
  // by the order they were declared in, so being drawn first is no guarantee of
  // staying behind -- and this one covered the settings panel, which is both
  // the thing you opened deliberately and the place you would go to fix
  // whatever it is complaining about.
  if (!ImGui::Begin("##idle", nullptr, kOverlayFlags | ImGuiWindowFlags_NoBringToFrontOnFocus)) {
    ImGui::End();
    ImGui::PopStyleVar();
    return;
  }

  // Centring has to be done by hand against the window's own width, and the
  // window is auto-sized -- so the widest item sets the width and everything
  // narrower is offset into it. The detail line is normally the widest.
  auto centreFor = [](float itemWidth) {
    const float width = ImGui::GetContentRegionAvail().x;
    if (itemWidth < width) {
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (width - itemWidth) * 0.5f);
    }
  };
  auto centred = [&](const char* text, bool dim) {
    centreFor(ImGui::CalcTextSize(text).x);
    if (dim) {
      ImGui::TextDisabled("%s", text);
    } else {
      ImGui::TextUnformatted(text);
    }
  };

  if (icon != 0) {
    centreFor(size);
    ImGui::Image((ImTextureID)icon, ImVec2(size, size));
    ImGui::Spacing();
  }

  // The wordmark, at whatever the interface scale makes of it. Not a second
  // font: one is enough, and a name set in the same face as everything else
  // looks deliberate rather than like a logo dropped in.
  ImGui::SetWindowFontScale(1.6f);
  centred(AppNameUtf8().c_str(), false);
  ImGui::SetWindowFontScale(1.0f);

  // Zeilenweise, und jede Zeile fuer sich zentriert. Ein \n in einem einzelnen
  // Textelement setzt die Zeilen zwar untereinander, aber linksbuendig zum
  // Block -- und zwei verschieden lange Zeilen sehen dann verrutscht aus statt
  // gesetzt.
  if (!detail.empty()) {
    ImGui::Spacing();
    for (size_t at = 0; at <= detail.size();) {
      const size_t brk = detail.find('\n', at);
      centred(detail.substr(at, brk == std::string::npos ? brk : brk - at).c_str(), true);
      if (brk == std::string::npos) break;
      at = brk + 1;
    }
  }

  ImGui::End();
  ImGui::PopStyleVar();
}

void DrawStatusCard(const std::string& title, const std::string& detail, bool spinner) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(
      ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
      ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowBgAlpha(0.92f);
  ImGui::SetNextWindowSizeConstraints(ImVec2(340.0f, 0.0f), ImVec2(560.0f, FLT_MAX));

  if (!ImGui::Begin("##status", nullptr, kOverlayFlags)) {
    ImGui::End();
    return;
  }

  ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
  ImGui::TextUnformatted(title.c_str());
  if (!detail.empty()) {
    ImGui::Spacing();
    ImGui::TextDisabled("%s", detail.c_str());
  }
  ImGui::PopTextWrapPos();

  if (spinner) {
    ImGui::Spacing();
    // Three dots cycling, so a long reconnect does not look frozen.
    const int phase = (int)(ImGui::GetTime() * 3.0) % 4;
    std::string dots(3, '.');
    for (int i = 0; i < 3; ++i) {
      if (i >= phase) dots[(size_t)i] = ' ';
    }
    ImGui::TextDisabled(T("Neuer Versuch%s", "Retrying%s"), dots.c_str());
  }

  ImGui::End();
}

ToastResult DrawToast(const std::string& text, double age, double duration, bool clickable,
                      const char* hint) {
  ToastResult result;
  if (age >= duration) return result;
  const float fade = (float)Clamp((duration - age) / 0.4, 0.0, 1.0);

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(
      ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y - 48.0f),
      ImGuiCond_Always, ImVec2(0.5f, 1.0f));
  ImGui::SetNextWindowBgAlpha(0.82f * fade);
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fade);

  // Klickbar heisst: die Maus gehoert dem Toast. Sonst ginge ein Klick darauf
  // durch aufs Bild, und zwei davon machten Vollbild statt Explorer.
  const ImGuiWindowFlags flags =
      clickable ? kOverlayFlags & ~ImGuiWindowFlags_NoInputs : kOverlayFlags;
  if (ImGui::Begin("##toast", nullptr, flags)) {
    ImGui::TextUnformatted(text.c_str());
    if (clickable && hint && *hint) ImGui::TextDisabled("%s", hint);
    if (clickable && ImGui::IsWindowHovered()) {
      result.hovered = true;
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      result.clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    }
  }
  ImGui::End();
  ImGui::PopStyleVar();
  return result;
}

void DrawSearchIndicator(const std::string& text, const std::string& detail) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 24.0f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.78f);

  if (ImGui::Begin("##standardsearch", nullptr, kOverlayFlags)) {
    // Dieselben drei Punkte wie auf der Statuskarte. Die Suche kann eine halbe
    // Minute laufen, und ohne Bewegung sieht ein stehender Text nach einem
    // Ergebnis aus statt nach einem Vorgang.
    const int phase = (int)(ImGui::GetTime() * 3.0) % 4;
    std::string dots(3, '.');
    for (int i = 0; i < 3; ++i) {
      if (i >= phase) dots[(size_t)i] = ' ';
    }
    ImGui::Text("%s%s", text.c_str(), dots.c_str());
    // Der Grund darunter, klein: die Kopfzeile sagt, was gerade auf der Karte
    // steht, und die will man im Vorbeigehen lesen koennen. Warum ueberhaupt
    // gesucht wird, liest man einmal.
    if (!detail.empty()) ImGui::TextDisabled("%s", detail.c_str());
  }
  ImGui::End();
}

void DrawSearchResult(const std::string& text, const std::string& detail, double age,
                      double duration) {
  if (age >= duration) return;
  const float fade = (float)Clamp((duration - age) / 0.5, 0.0, 1.0);

  // Dieselbe Stelle wie die Suche selbst, und das mit Absicht: die Einblendung
  // wird nicht ersetzt, sie hoert auf, sich zu bewegen. Wer hinsieht, muss den
  // Blick nicht umsetzen, um zu erfahren, was aus dem geworden ist, das er
  // gerade gelesen hat.
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 24.0f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.78f * fade);
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fade);

  if (ImGui::Begin("##standardresult", nullptr, kOverlayFlags)) {
    // Und hier keine laufenden Punkte. Sie sind der ganze Unterschied: drei
    // Punkte heissen "es geht weiter", und genau das tut es nicht mehr.
    ImGui::TextUnformatted(text.c_str());
    if (!detail.empty()) ImGui::TextDisabled("%s", detail.c_str());
  }
  ImGui::End();
  ImGui::PopStyleVar();
}

void DrawRecordIndicator(double seconds, OsdCorner volumeCorner) {
  // Opposite corner to the volume readout: both can appear at once, and a
  // number sliding out from under a dot looks like a bug.
  OsdCorner corner = OsdCorner::TopRight;
  switch (volumeCorner) {
    case OsdCorner::TopRight: corner = OsdCorner::TopLeft; break;
    case OsdCorner::TopLeft: corner = OsdCorner::TopRight; break;
    case OsdCorner::BottomRight: corner = OsdCorner::BottomLeft; break;
    case OsdCorner::BottomLeft: corner = OsdCorner::BottomRight; break;
  }
  // Except when the statistics panel already owns the top left.
  if (corner == OsdCorner::TopLeft) corner = OsdCorner::BottomLeft;

  PositionInCorner(corner, 24.0f);
  ImGui::SetNextWindowBgAlpha(0.80f);

  if (ImGui::Begin("##recording", nullptr, kOverlayFlags)) {
    // One slow pulse per second, so it reads as "running" without flickering.
    const float pulse = 0.55f + 0.45f * (float)std::abs(std::sin(ImGui::GetTime() * 3.14159));
    const float radius = ImGui::GetFontSize() * 0.32f;
    const ImVec2 centre = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(centre.x + radius, centre.y + ImGui::GetFontSize() * 0.5f), radius,
        ImGui::GetColorU32(ImVec4(0.95f, 0.25f, 0.25f, pulse)));
    ImGui::Dummy(ImVec2(radius * 2.0f + 6.0f, ImGui::GetFontSize()));
    ImGui::SameLine();

    const int total = (int)seconds;
    ImGui::Text("%s  %02d:%02d:%02d", T("Aufnahme", "REC"), total / 3600, (total / 60) % 60,
                total % 60);
  }
  ImGui::End();
}

void DrawVolumeOsd(float volume, bool muted, OsdCorner corner, double age, double duration) {
  if (age >= duration) return;
  const float fade = (float)Clamp((duration - age) / 0.35, 0.0, 1.0);

  PositionInCorner(corner, 24.0f);
  ImGui::SetNextWindowBgAlpha(0.85f * fade);
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fade);

  if (ImGui::Begin("##volume", nullptr, kOverlayFlags)) {
    const float percent = volume * 100.0f;
    if (muted) {
      ImGui::TextUnformatted(T("Stumm", "Muted"));
    } else {
      ImGui::Text(T("Lautstärke  %.0f %%", "Volume  %.0f %%"), percent);
    }

    // A plain bar rather than a slider: this is a readout, not a control.
    const ImVec2 size(190.0f, 6.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 trackColor = ImGui::GetColorU32(ImGuiCol_FrameBg, fade);
    const ImU32 fillColor =
        ImGui::GetColorU32(muted ? ImGuiCol_TextDisabled : ImGuiCol_SliderGrab, fade);

    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), trackColor, 3.0f);
    const float filled = muted ? 0.0f : size.x * Clamp(volume, 0.0f, 1.0f);
    if (filled > 0.5f) {
      draw->AddRectFilled(origin, ImVec2(origin.x + filled, origin.y + size.y), fillColor, 3.0f);
    }
    ImGui::Dummy(size);
  }
  ImGui::End();
  ImGui::PopStyleVar();
}

}  // namespace cap
