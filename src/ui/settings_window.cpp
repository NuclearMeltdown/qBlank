#include "ui/settings_window.h"

#include "record/recorder.h"
#include "camera_sink.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string_view>

#include "app_files.h"
#include "desktop.h"
#include "i18n.h"
#include "record/screenshot.h"
#include "ui/file_dialog.h"
#include "ui/settings_host.h"
#include "imgui.h"

namespace cap {
namespace {

using NameFn = const char* (*)(int);

// A small "?" that shows an explanation on hover.
// Wie SetItemTooltip, aber mit Umbruch. Der eingebaute bricht nicht um, was bei
// den kurzen Einzeilern hier nirgends auffaellt und bei einem laengeren Text
// sofort: er laeuft dann quer ueber den Bildschirm aus dem Fenster heraus.
// Dieselbe Breite wie HelpMarker, damit beide gleich aussehen.
void WrappedTooltip(const char* text) {
  if (!text || !*text) return;
  if (ImGui::BeginItemTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

void HelpMarker(const char* text) {
  if (!text || !*text) return;
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::BeginItemTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

// Wie TextDisabled, aber mit Umbruch. ImGui hat beides nur einzeln: TextDisabled
// faerbt und bricht nicht um, TextWrapped bricht um und faerbt nicht. Ein kurzer
// Halbsatz merkt den Unterschied nie, ein ganzer Erklaersatz sofort -- er
// schiebt das Fenster in die Breite, bis man waagerecht scrollen muss, um das
// Ende zu lesen.
void TextDisabledWrapped(const char* text) {
  if (!text || !*text) return;
  const ImVec4 grey = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
  ImGui::PushStyleColor(ImGuiCol_Text, grey);
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
}

// Dasselbe in der Farbe, die im Rest des Fensters schon "sieh hier hin" heisst
// -- dieselben Werte wie bei "Erzwungen: ...". Grau waere hier falsch: grau
// sagt "Nebensache", und ein Hinweis, den man lesen soll, ist keine.
void TextWarningWrapped(const char* text) {
  if (!text || !*text) return;
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.72f, 0.35f, 1.0f));
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
}

// Rechtsklick auf den Regler davor stellt ihn auf den Wert, mit dem er
// ausgeliefert wird. Die Werte kommen aus den Strukturen selbst -- kImage,
// kAudio und so weiter unten --, damit hier keine zweite Liste veraltet.
//
// Der Hinweis darauf kommt nur, wenn der Regler nicht schon dort steht, und erst
// nach kurzem Stillhalten. Sonst hinge er an jedem Regler, den die Maus auf dem
// Weg nach unten ueberfaehrt.
//
// Die Lautstaerke hat das mit Absicht nicht: ihr Standard ist 100 %, und ein
// verrutschter Klick soll niemandem die Ohren wegblasen.
template <typename V>
void ResetOnRightClick(V& value, V def) {
  if (value == def) return;
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
    value = def;
    return;
  }
  if (!ImGui::IsItemActive() &&
      ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_Stationary)) {
    ImGui::SetTooltip("%s", T("Rechtsklick: Standardwert", "Right click: default"));
  }
}

const ImageSettings kImage{};
const AudioSettings kAudio{};
const RecordSettings kRecord{};
const AppSettings kApp{};
constexpr double kGb = 1024.0 * 1024.0 * 1024.0;

// Combo over an enum whose labels come from a lookup function, so the list
// follows the selected language without any table to keep in sync.
bool ComboEnum(const char* label, int* value, int count, NameFn name, NameFn help = nullptr) {
  if (*value < 0 || *value >= count) *value = 0;
  bool changed = false;
  if (ImGui::BeginCombo(label, name(*value))) {
    for (int i = 0; i < count; ++i) {
      const bool selected = (*value == i);
      if (ImGui::Selectable(name(i), selected)) {
        *value = i;
        changed = true;
      }
      if (help && ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
        ImGui::TextUnformatted(help(i));
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  return changed;
}

// A level bar. Linear sample values crowd everything useful into the top of the
// bar, so the scale is decibels: -60 dB at the left, 0 dB at the right, with the
// last sixth coloured because that is where clipping lives.
void LevelMeter(const char* id, float peak, bool active) {
  const float height = ImGui::GetFrameHeight() * 0.55f;
  const ImVec2 size(ImGui::CalcItemWidth(), height);
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImGuiStyle& style = ImGui::GetStyle();

  dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                    ImGui::GetColorU32(ImGuiCol_FrameBg), style.FrameRounding);

  if (active && peak > 0.0f) {
    const float db = 20.0f * std::log10(std::max(peak, 1e-5f));
    const float norm = Clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
    const bool hot = db > -6.0f;
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x * norm, pos.y + size.y),
                      hot ? IM_COL32(230, 150, 60, 255) : IM_COL32(90, 200, 120, 255),
                      style.FrameRounding);
  }

  // -6 dB mark, the usual "do not go past this" line.
  const float markX = pos.x + size.x * (54.0f / 60.0f);
  dl->AddLine(ImVec2(markX, pos.y), ImVec2(markX, pos.y + size.y), IM_COL32(255, 255, 255, 70));

  ImGui::Dummy(size);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(active && peak > 0.0f ? "%.1f dB" : T("kein Signal", "no signal"),
                      20.0f * std::log10(std::max(peak, 1e-5f)));
  }
  ImGui::SameLine();
  ImGui::TextUnformatted(id);
}

// Over the buffer slider just drawn, which runs over all of the memory: from
// its right end what other programs hold, and left of that the reserve kept
// for them. Where the two begin is where the grab stops. In gigabytes, placed
// with the same arithmetic ImGui uses for an integer slider's grab.
void MemoryOverlay(int maxGb, double roomGb, double reserveGb, ImVec4 usedColor,
                   ImVec4 reserveColor) {
  const ImVec2 min = ImGui::GetItemRectMin();
  const ImVec2 max = ImGui::GetItemRectMax();
  const float pad = 2.0f;
  const float size = max.x - min.x - 2.0f * pad;
  const float grab = std::min(std::max(size / (float)maxGb, ImGui::GetStyle().GrabMinSize), size);
  // The right edge of the grab at `gb`.
  auto x = [&](double gb) {
    const double t = maxGb > 1 ? (gb - 1.0) / (double)(maxGb - 1) : 1.0;
    return Clamp(min.x + pad + grab + (float)t * (size - grab), min.x, max.x);
  };
  const float reserveX = x(roomGb);
  const float usedX = x(roomGb + reserveGb);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(ImVec2(reserveX, min.y), ImVec2(usedX, max.y),
                    ImGui::GetColorU32(reserveColor));
  dl->AddRectFilled(ImVec2(usedX, min.y), max, ImGui::GetColorU32(usedColor),
                    ImGui::GetStyle().FrameRounding, ImDrawFlags_RoundCornersRight);
}

std::string FpsLabel(double fps) {
  // Zero is the stored form of "highest available" and -1 of "the signal's
  // own" -- modes, not rates.
  if (fps < 0.0) return T("Rate des Signals", "The signal's rate");
  if (fps <= 0.0) return T("Höchste verfügbare", "Highest available");
  std::string s = Format("%.2f", fps);
  if (CurrentLanguage() == Language::German) {
    for (char& c : s) {
      if (c == '.') c = ',';
    }
    if (s.size() > 3 && s.compare(s.size() - 3, 3, ",00") == 0) s.resize(s.size() - 3);
  } else if (s.size() > 3 && s.compare(s.size() - 3, 3, ".00") == 0) {
    s.resize(s.size() - 3);
  }
  return s + " Hz";
}

// A width written for 100 %, at the scale of the monitor the window is on.
float Px(float pixels) { return pixels * ImGui::GetStyle().FontScaleDpi; }

// The release text is GitHub markdown, written one way: "**Heading**" lines
// over "- Fixed: ..." bullets. Drawn as what it means rather than as
// asterisks; any other line is plain text.
void DrawReleaseNotes(std::string_view notes) {
  ImGui::PushTextWrapPos(0.0f);
  bool first = true;
  while (!notes.empty()) {
    const size_t end = notes.find('\n');
    std::string_view line = notes.substr(0, end);
    notes.remove_prefix(end == std::string_view::npos ? notes.size() : end + 1);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
    if (line.empty()) continue;
    if (line.size() > 4 && line.compare(0, 2, "**") == 0 &&
        line.compare(line.size() - 2, 2, "**") == 0) {
      if (!first) ImGui::Spacing();
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
      ImGui::TextUnformatted(line.data() + 2, line.data() + line.size() - 2);
      ImGui::PopStyleColor();
    } else if (line.size() > 2 && (line[0] == '-' || line[0] == '*') && line[1] == ' ') {
      ImGui::Bullet();
      ImGui::TextUnformatted(line.data() + 2, line.data() + line.size());
    } else {
      ImGui::TextUnformatted(line.data(), line.data() + line.size());
    }
    first = false;
  }
  ImGui::PopTextWrapPos();
}

}  // namespace

std::vector<MonitorInfoEntry> EnumerateMonitors() {
  std::vector<MonitorInfoEntry> list;
  for (const DisplayInfo& display : EnumerateDisplays()) {
    MonitorInfoEntry entry;
    entry.index = (int)list.size();
    entry.rect = display.rect;
    entry.primary = display.primary;
    entry.name = Format(T("Monitor %d  (%ldx%ld)%s", "Monitor %d  (%ldx%ld)%s"), entry.index + 1,
                        (long)display.rect.width(), (long)display.rect.height(),
                        entry.primary ? T("  [Hauptmonitor]", "  [primary]") : "");
    list.push_back(std::move(entry));
  }
  return list;
}

// ------------------------------------------------------------------ lifecycle

void SettingsWindow::Open(Config* live, const std::string& reason) {
  if (!live) return;
  live_ = live;
  snapshot_ = *live;
  reason_ = reason;
  open_ = true;
  restorePos_ = true;
  listsStale_ = true;
  shortcutsRead_ = false;
  shortcutFailed_ = false;
  renameTarget_ = -1;
  recordBuffersLoaded_ = false;
}

bool SettingsWindow::takeProbeRequest() {
  const bool requested = probeRequested_;
  probeRequested_ = false;
  return requested;
}

bool SettingsWindow::takeCropPickRequest() {
  const bool requested = cropPickRequested_;
  cropPickRequested_ = false;
  return requested;
}

bool SettingsWindow::takeDeviceConfigRequest() {
  const bool requested = deviceConfigRequested_;
  deviceConfigRequested_ = false;
  return requested;
}

bool SettingsWindow::takeRestartRequest() {
  const bool requested = restartRequested_;
  restartRequested_ = false;
  return requested;
}

bool SettingsWindow::takeCropDetectRequest() {
  const bool requested = cropDetectRequested_;
  cropDetectRequested_ = false;
  return requested;
}

bool SettingsWindow::takeCardResetRequest() {
  const bool requested = cardResetRequested_;
  cardResetRequested_ = false;
  return requested;
}

bool SettingsWindow::takeCompareToggle() {
  const bool requested = compareToggleRequested_;
  compareToggleRequested_ = false;
  return requested;
}

bool SettingsWindow::takeBypassToggle() {
  const bool requested = bypassToggleRequested_;
  bypassToggleRequested_ = false;
  return requested;
}

bool SettingsWindow::takeRangeRemeasureRequest() {
  const bool requested = rangeRemeasureRequested_;
  rangeRemeasureRequested_ = false;
  return requested;
}

int SettingsWindow::takeVirtualCameraRequest() {
  const int request = virtualCameraRequest_;
  virtualCameraRequest_ = 0;
  return request;
}

void SettingsWindow::SetVirtualCameraState(bool running,
                                           const std::vector<CameraSink::Consumer>& consumers) {
  vcamRunning_ = running;
  vcamConsumers_ = consumers;
}

void SettingsWindow::Close() {
  open_ = false;
  captureAction_ = -1;
  searchBuf_[0] = 0;
  searchQuery_.clear();
  searchRows_.clear();
  jumpKey_ = nullptr;
  flashKey_ = nullptr;
  searchNoteTime_ = -10.0;
}

void SettingsWindow::InvalidateDeviceLists() {
  ForgetAudioDevices();
  listsStale_ = true;
  ++probeRound_;
  probedId_.clear();
  probingId_.clear();
  probed_ = DeviceProbeResult{};
  embeddedAudioForDevice_.clear();
  embeddedLooking_ = false;
}

// DirectShow binds every device to name it, and the sound devices are asked
// one by one: nothing for the thread that draws.
void SettingsWindow::RefreshDeviceLists() {
  listsStale_ = false;
  listsBusy_ = true;
  deviceJobs_.Ask([this] {
    struct Lists {
      std::vector<VideoDeviceInfo> video;
      std::vector<AudioDeviceInfo> inputs;
      std::vector<AudioDeviceInfo> outputs;
      std::vector<MonitorInfoEntry> monitors;
      std::vector<ScreenInfo> screens;
      std::vector<WindowInfo> windows;
    };
    auto lists = std::make_shared<Lists>();
    lists->video = EnumerateVideoDevices();
    lists->inputs = EnumerateAudioDevices(true);
    lists->outputs = EnumerateAudioDevices(false);
    lists->monitors = EnumerateMonitors();
    lists->screens = ListScreens();
    lists->windows = ListWindows();
    return [this, lists] {
      videoDevices_ = std::move(lists->video);
      audioInputs_ = std::move(lists->inputs);
      audioOutputs_ = std::move(lists->outputs);
      monitors_ = std::move(lists->monitors);
      screens_ = std::move(lists->screens);
      windows_ = std::move(lists->windows);
      listsBusy_ = false;
    };
  });
}

const DeviceProbeResult& SettingsWindow::CapsFor(const DeviceRef& device,
                                                 const DeviceProbeResult* liveCaps) {
  static const DeviceProbeResult kEmpty;
  if (device.empty()) return kEmpty;

  // The running device is already open; probing it again would fail.
  if (liveCaps && liveCaps->ok && !liveCaps->device.id.empty() &&
      (liveCaps->device.id == device.id || liveCaps->device.name == device.name)) {
    return *liveCaps;
  }

  const std::string key = device.id.empty() ? device.name : device.id;
  if (probedId_ == key) return probed_;
  if (probingId_ != key && probeAllowed_) {
    CAP_LOG("Querying the capabilities of '%s'", device.name.c_str());
    probingId_ = key;
    deviceJobs_.Ask([this, device, key, round = probeRound_] {
      auto result = std::make_shared<DeviceProbeResult>(VideoCapture::Probe(device));
      return [this, key, round, result] {
        if (round != probeRound_) return;
        if (probingId_ == key) probingId_.clear();
        probed_ = std::move(*result);
        probedId_ = key;
      };
    });
  }
  // Nothing until the answer is in: the formats of the device before would be
  // taken for this one's.
  return kEmpty;
}

void SettingsWindow::EnsureValidFormat(const DeviceProbeResult& caps) {
  Profile& p = cfg().active();
  if (!caps.ok || caps.caps.empty()) return;

  const std::vector<std::string> subtypes = caps.caps.Subtypes();
  if (subtypes.empty()) return;

  // Format from another card, or nothing set yet: pick something sensible.
  // A subtype without a size counts as not set -- that is what re-reading the
  // card leaves behind -- but the subtype in it is still a wish, so it is
  // handed on rather than thrown away. Without that this would put the
  // ordinary choice back the moment the settings page was drawn.
  if (!p.capture.format.valid() ||
      std::find(subtypes.begin(), subtypes.end(), p.capture.format.subtype) == subtypes.end()) {
    // Die Betriebsart der Bildrate ueberlebt die Neuwahl. PickDefault liefert
    // immer eine Zahl -- die schreibt hier aber gerade nicht ins Profil, wer
    // "hoechste verfuegbare" oder "Rate des Signals" ausgesucht hat: das ist
    // eine Anweisung fuers Oeffnen der Karte und keine Eigenschaft des
    // gefundenen Formats. Sonst wuerde ein Normwechsel, der die Aufloesung
    // loslaesst, beim naechsten Zeichnen der Seite eine feste Zahl hinterlassen.
    const double mode = p.capture.format.fps;
    const bool autoSubtype = p.capture.format.autoSubtype;
    p.capture.format = caps.caps.PickDefault(p.capture.format.subtype);
    if (mode <= 0.0) p.capture.format.fps = mode;
    p.capture.format.autoSubtype = autoSubtype;
  }
  p.capture.format.forced =
      !caps.caps.IsAdvertised(p.capture.format.subtype, p.capture.format.width,
                              p.capture.format.height, p.capture.format.fps);
}

// ----------------------------------------------------------------------- draw

void SettingsWindow::PollFileDialog(FfmpegInfo* ffmpeg) {
  std::vector<std::filesystem::path> picked;
  int tag = kPickNone;
  if (!picker_.TakeResult(&picked, &tag)) return;
  if (picked.empty()) return;  // cancelled

  RecordSettings& rec = cfg().record;
  switch (tag) {
    case kPickRecordFolder:
      rec.outputFolder = PathToUtf8(picked.front());
      std::snprintf(folderBuffer_, sizeof(folderBuffer_), "%s", rec.outputFolder.c_str());
      break;
    case kPickShotFolder:
      rec.screenshotFolder = PathToUtf8(picked.front());
      std::snprintf(shotFolderBuffer_, sizeof(shotFolderBuffer_), "%s",
                    rec.screenshotFolder.c_str());
      break;
    case kPickRemux:
      if (ffmpeg && ffmpeg->found) remuxer_.Start(picked);
      break;
    default:
      break;
  }
}

namespace {

struct TabName {
  int tab;
  const char* de;
  const char* en;
};

// In screen order.
constexpr TabName kTabNames[] = {
    {kTabGeneral, "Allgemein", "General"}, {kTabSource, "Quelle", "Source"},
    {kTabPicture, "Bild", "Picture"},      {kTabHdr, "HDR", "HDR"},
    {kTabAudio, "Ton", "Audio"},           {kTabDisplay, "Anzeige", "Display"},
    {kTabRecord, "Aufnahme", "Recording"}, {kTabReplay, "Replay", "Replay"},
    {kTabEncoder, "Encoder", "Encoder"},   {kTabKeys, "Tasten", "Keys"},
    {kTabProfiles, "Profile", "Profiles"}, {kTabAbout, "Über", "About"},
};
static_assert(std::size(kTabNames) == kTabCount, "every tab needs a name");

}  // namespace

void SettingsWindow::MeasureTabs() {
  const ImGuiStyle& style = ImGui::GetStyle();
  // Ohne Gerät gibt es nur eine sinnvolle Handlung, und das ist eines
  // auszuwählen. Acht weitere Reiter voller Regler, die alle auf ein Bild
  // wirken sollen, das es nicht gibt, sind an dieser Stelle kein Angebot
  // sondern eine Hürde. Sie kommen zurück, sobald die Karte läuft -- und dann
  // gleich passend zu dem, was sie tatsächlich liefert.
  const CaptureSettings& capture = cfg().active().capture;
  const bool screen = capture.source != VideoSource::Card;
  const bool haveDevice = screen || !capture.video.empty();
  float row = -style.ItemInnerSpacing.x;
  bool activeShown = false;
  shownTabCount_ = 0;
  for (int i = 0; i < (int)std::size(kTabNames); ++i) {
    const TabName& t = kTabNames[i];
    if (!haveDevice && SettingsTabNeedsDevice(t.tab)) continue;
    const float width = ImGui::CalcTextSize(T(t.de, t.en)).x + style.FramePadding.x * 2.0f;
    shownTabs_[shownTabCount_++] = {i, width};
    row += width + style.ItemInnerSpacing.x;
    activeShown |= t.tab == activeTab_;
  }
  if (!activeShown) activeTab_ = kTabNames[shownTabs_[0].name].tab;
  oneRowWidth_ = std::ceil(row + style.WindowPadding.x * 2.0f);
}

// ImGui's own tab bar squeezes the labels down to "..." once they stop fitting
// side by side. This one breaks them into as few rows as will do, spreads the
// tabs evenly over them and stretches each row to the full width -- the way the
// Windows tab control does it. One row keeps the labels' own widths.
void SettingsWindow::DrawTabStrip() {
  const ImGuiStyle& style = ImGui::GetStyle();
  const float gap = style.ItemInnerSpacing.x;
  const float avail = ImGui::GetContentRegionAvail().x;
  const int n = shownTabCount_;
  auto rowWidth = [&](int first, int last) {
    float w = -gap;
    for (int i = first; i < last; ++i) w += shownTabs_[i].width + gap;
    return w;
  };
  auto fits = [&](int per) {
    for (int first = 0; first < n; first += per) {
      if (rowWidth(first, std::min(n, first + per)) > avail) return false;
    }
    return true;
  };
  int per = n;
  for (int rows = 2; per > 1 && !fits(per); ++rows) per = (n + rows - 1) / rows;

  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float height = ImGui::GetFrameHeight();
  const float rowStep = height + style.ItemInnerSpacing.y;
  const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
  float y = origin.y;
  for (int first = 0; first < n; first += per, y += rowStep) {
    const int last = std::min(n, first + per);
    const float extra = per < n ? (avail - rowWidth(first, last)) / (last - first) : 0.0f;
    float x = origin.x;
    for (int i = first; i < last; ++i) {
      const TabName& t = kTabNames[shownTabs_[i].name];
      const float left = std::floor(x);
      x += shownTabs_[i].width + extra;
      const float right = std::floor(x);
      x += gap;
      ImGui::SetCursorScreenPos(ImVec2(left, y));
      ImGui::PushID(t.tab);
      // Taken on the press, as ImGui's tabs and Windows' are.
      ImGui::InvisibleButton("##tab", ImVec2(right - left, height), ImGuiButtonFlags_EnableNav);
      ImGui::PopID();
      if (ImGui::IsItemActivated()) activeTab_ = t.tab;
      const ImGuiCol fill = t.tab == activeTab_     ? ImGuiCol_TabSelected
                            : ImGui::IsItemHovered() ? ImGuiCol_TabHovered
                                                     : ImGuiCol_Tab;
      draw->AddRectFilled(ImVec2(left, y), ImVec2(right, y + height - style.TabBarBorderSize),
                          ImGui::GetColorU32(fill), style.TabRounding, ImDrawFlags_RoundCornersTop);
      const float label = shownTabs_[i].width - style.FramePadding.x * 2.0f;
      draw->AddText(ImVec2(std::floor((left + right - label) * 0.5f), y + style.FramePadding.y),
                    text, T(t.de, t.en));
    }
  }
  // The line under the tabs, in the colour ImGui's tab bar gives it.
  const float bottom = y - style.ItemInnerSpacing.y;
  if (style.TabBarBorderSize > 0.0f) {
    const float pad = std::floor(style.WindowPadding.x * 0.5f);
    const ImGuiCol line = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
                              ? ImGuiCol_TabSelected
                              : ImGuiCol_TabDimmedSelected;
    draw->AddRectFilled(ImVec2(origin.x - pad, bottom - style.TabBarBorderSize),
                        ImVec2(origin.x + avail + pad, bottom), ImGui::GetColorU32(line));
  }
  ImGui::SetCursorScreenPos(ImVec2(origin.x, bottom + style.ItemSpacing.y));
}

SettingsWindow::Result SettingsWindow::Draw(const DeviceProbeResult* liveCaps,
                                            FfmpegInfo* ffmpeg) {
  if (!open_ || !live_) return Result::None;
  deviceJobs_.Collect();
  setupJobs_.Collect();
  PollFileDialog(ffmpeg);
  if (listsStale_ && !listsBusy_) RefreshDeviceLists();

  Result result = Result::None;

  // Once per run, the tab from the configuration; after that, whatever was last
  // opened.
  if (!tabRestored_) {
    tabRestored_ = true;
    activeTab_ = cfg().app.settingsTab;
  }
  MeasureTabs();

  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  bool stayOpen = true;
  bool opened;
  if (fillsWindow_) {
    // The dialog *is* the window here: the host already provides a title bar, a
    // border and a close button, so the panel inside it gets none of those and
    // simply covers the whole thing. The footer buttons still work -- the caller
    // acts on the result either way.
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    opened = ImGui::Begin("###qblank_settings_host", nullptr,
                          ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                              ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                              ImGuiWindowFlags_NoBringToFrontOnFocus |
                              ImGuiWindowFlags_NoSavedSettings);
  } else {
    // Wo das Feld zuletzt stand -- und der Rückfall in die Mitte, wenn das
    // nicht mehr geht.
    //
    // ImGui führt seine Fensterlagen im Kontext, also überlebt sie das
    // Umschalten zwischen eingebettet und freigestellt von selbst. Was sie
    // nicht überlebt, ist ein Neustart: die imgui.ini ist abgeschaltet, damit
    // neben der exe nichts liegt, das dort niemand erwartet. Also wird sie hier
    // mitgeschrieben.
    //
    // Der Rückfall greift, wenn das Hauptfenster inzwischen kleiner ist als
    // damals -- dann läge das Feld ganz oder überwiegend draußen, und ohne
    // Titelleiste in Reichweite bekäme man es nicht mehr zurück.
    //
    // The sizes below are for 100 %; the style carries the monitor's scale.
    //
    // The default goes first: ImGui keeps only the last size it is handed, so
    // after the restore below it used to replace the remembered size.
    // Wide enough for the tabs in one row, if the picture has the room.
    const float scale = ImGui::GetStyle().FontScaleDpi;
    ImGui::SetNextWindowSize(
        ImVec2(std::min(std::max(780.0f * scale, oneRowWidth_), viewport->WorkSize.x),
               660.0f * scale),
        ImGuiCond_FirstUseEver);
    if (restorePos_) {
      restorePos_ = false;
      const ImVec2 work = viewport->WorkPos;
      const ImVec2 area = viewport->WorkSize;
      const AppSettings& app = cfg().app;

      const float x = (float)app.settingsPanelX;
      const float y = (float)app.settingsPanelY;
      const float w = (float)app.settingsPanelW;
      const float h = (float)app.settingsPanelH;

      // Hundert Pixel Titelleiste müssen greifbar bleiben, sonst ist es
      // unerreichbar und die Mitte ist die bessere Antwort.
      const bool haveSaved = w > 200.0f && h > 200.0f;
      const bool reachable = haveSaved && x + 100.0f > work.x && x < work.x + area.x - 100.0f &&
                             y >= work.y - 4.0f && y < work.y + area.y - 40.0f;

      if (reachable) {
        ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
        ImGui::SetNextWindowSize(
            ImVec2(std::min(w, area.x), std::min(h, area.y)), ImGuiCond_Always);
      } else {
        ImGui::SetNextWindowPos(
            ImVec2(work.x + area.x * 0.5f, work.y + area.y * 0.5f), ImGuiCond_Always,
            ImVec2(0.5f, 0.5f));
      }
    } else if (panelScale_ > 0.0f && scale != panelScale_) {
      // The main window went to a monitor with other scaling and grew or shrank
      // with it. The panel does the same and keeps its place in it.
      const float k = scale / panelScale_;
      const AppSettings& app = cfg().app;
      ImGui::SetNextWindowPos(ImVec2(app.settingsPanelX * k, app.settingsPanelY * k),
                              ImGuiCond_Always);
      ImGui::SetNextWindowSize(ImVec2(app.settingsPanelW * k, app.settingsPanelH * k),
                               ImGuiCond_Always);
    }
    panelScale_ = scale;
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(kSettingsMinWidth * scale, kSettingsMinHeight * scale), ImVec2(FLT_MAX, FLT_MAX));

    // Everything after "###" is the id, everything before it is the label.
    // Without this the window is a different window in each language: ImGui keys
    // position, size and the selected tab off the name, so switching language
    // moved the dialog, resized it and threw you back to the first tab.
    opened = ImGui::Begin(T("Einstellungen###qblank_settings", "Settings###qblank_settings"),
                          &stayOpen, ImGuiWindowFlags_NoCollapse);
  }
  if (!fillsWindow_ && opened) {
    // Mitschreiben, solange es sichtbar ist. Billig, und es erspart einen
    // eigenen Weg fuer "der Nutzer hat gerade losgelassen".
    const ImVec2 pos = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    AppSettings& app = cfg().app;
    app.settingsPanelX = (int)pos.x;
    app.settingsPanelY = (int)pos.y;
    app.settingsPanelW = (int)size.x;
    app.settingsPanelH = (int)size.y;
  }
  if (!opened) {
    ImGui::End();
    return Result::None;
  }
  if (!stayOpen) {
    ImGui::End();
    return Result::Close;
  }

  if (!reason_.empty()) {
    // AutoResizeY makes the box follow the text. It used to be a fixed 1.6 rows,
    // which turned a longer message -- "the device is already in use by another
    // program", say -- into a scrollbar inside a banner.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.85f, 0.55f, 0.15f, 0.18f));
    ImGui::BeginChild("banner", ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    ImGui::TextWrapped("%s", reason_.c_str());
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Spacing();
  }

  const DeviceProbeResult& caps = CapsFor(cfg().active().capture.video, liveCaps);
  captureRunning_ = liveCaps != nullptr;
  EnsureValidFormat(caps);

  // Height to leave free for the separator and the button row. Each tab scrolls
  // inside its own child, which is what keeps the tab strip pinned to the top.
  const ImGuiStyle& style = ImGui::GetStyle();
  const float footer =
      ImGui::GetFrameHeightWithSpacing() + style.ItemSpacing.y * 2.0f + style.SeparatorSize;

  DrawSearchField();
  const bool searching = DrawSearchResults(footer);

  if (!searching) {
    DrawTabStrip();
    // One child per tab, so each keeps its own scroll position.
    ImGui::PushID(activeTab_);
    ImGui::BeginChild("scroll", ImVec2(0, -footer));
    switch (activeTab_) {
      case kTabGeneral: DrawGeneralTab(); break;
      case kTabSource: DrawSourceTab(caps); break;
      case kTabPicture: DrawImageTab(); break;
      case kTabHdr: DrawHdrTab(); break;
      case kTabAudio: DrawAudioTab(); break;
      case kTabDisplay: DrawDisplayTab(); break;
      case kTabRecord: DrawRecordTab(ffmpeg); break;
      case kTabReplay: DrawReplayTab(ffmpeg); break;
      case kTabEncoder: DrawEncoderTab(ffmpeg); break;
      case kTabKeys: DrawHotkeysTab(); break;
      case kTabProfiles: DrawProfilesTab(caps); break;
      case kTabAbout: DrawAboutTab(); break;
    }
    ImGui::EndChild();
    ImGui::PopID();
    // Hidden controls never draw their anchor -- whatever they hang on, the
    // source or another setting, says no right now. Better to say so than to
    // land on the tab and leave the reader searching it by eye.
    if (jumpKey_ && ++jumpWait_ > 8) {
      searchNote_ = Format(T("„%s“ ist gerade ausgeblendet — hängt an der Quelle "
                             "oder einer anderen Einstellung.",
                             "“%s” is hidden right now — it depends on the source "
                             "or another setting."),
                           jumpLabel_);
      searchNoteTime_ = ImGui::GetTime();
      jumpKey_ = nullptr;
    }
  }
  cfg().app.settingsTab = activeTab_;

  ImGui::Separator();
  const float buttonWidth = Px(120.0f);
  const float spacing = style.ItemSpacing.x;
  const float buttonsX =
      ImGui::GetWindowWidth() - style.WindowPadding.x - buttonWidth * 2.0f - spacing;
  // The hint goes when the window is too narrow for it, rather than running
  // under the buttons.
  const char* hint = T("Änderungen wirken sofort.", "Changes take effect immediately.");
  if (ImGui::GetCursorPosX() + ImGui::CalcTextSize(hint).x + spacing <= buttonsX) {
    ImGui::TextDisabled("%s", hint);
    ImGui::SameLine();
  }
  ImGui::SetCursorPosX(buttonsX);
  if (ImGui::Button(T("Verwerfen", "Discard"), ImVec2(buttonWidth, 0))) {
    *live_ = snapshot_;
    result = Result::Close;
  }
  ImGui::SetItemTooltip(T("Alles auf den Stand beim Öffnen zurücksetzen und schließen.",
                          "Restore everything to how it was when this opened, and close."));
  ImGui::SameLine();
  if (ImGui::Button(T("Schließen", "Close"), ImVec2(buttonWidth, 0))) result = Result::Close;

  ImGui::End();
  return result;
}

// -------------------------------------------------------------------- search

void SettingsWindow::DrawSearchField() {
  // Strg+F from anywhere in the dialog. Embedded, only while it has the focus:
  // over the picture the keys belong to the app, and someone may have bound
  // them. In its own window the keys only get here when that window has the
  // focus anyway -- and asking ImGui as well would fail whenever nothing inside
  // has been clicked yet.
  if ((fillsWindow_ || ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) &&
      ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
    focusSearch_ = true;
  }
  if (focusSearch_) {
    ImGui::SetKeyboardFocusHere();
    focusSearch_ = false;
  }

  // A note after a jump takes the hint's place for a few seconds: it is about
  // the search, it needs no room of its own, and nothing below it moves.
  const bool note = ImGui::GetTime() - searchNoteTime_ < 5.0;
  if (note) ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(0.95f, 0.72f, 0.35f, 1.0f));
  ImGui::SetNextItemWidth(-1.0f);
  const bool entered = ImGui::InputTextWithHint(
      "##search", note ? searchNote_.c_str() : T("Suchen … (Strg+F)", "Search … (Ctrl+F)"),
      searchBuf_, sizeof(searchBuf_),
      ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_EscapeClearsAll |
          ImGuiInputTextFlags_CallbackHistory,
      [](ImGuiInputTextCallbackData* data) {
        // Up and down walk the results without leaving the field.
        auto* self = static_cast<SettingsWindow*>(data->UserData);
        self->searchSel_ += data->EventKey == ImGuiKey_UpArrow ? -1 : 1;
        return 0;
      },
      this);
  if (note) ImGui::PopStyleColor();

  if (searchQuery_ != searchBuf_) {
    searchQuery_ = searchBuf_;
    searchRows_.clear();
    searchSel_ = 0;
    // What matches the label as it reads comes first; the guesses -- a
    // synonym, the other language, a typo -- after it, and fewer of them.
    const std::vector<SettingsSearchHit> hits = SearchSettings(searchBuf_);
    for (const SettingsSearchHit& hit : hits) {
      if (hit.direct && searchRows_.size() < 12) searchRows_.push_back(hit.entry);
    }
    searchDirectRows_ = (int)searchRows_.size();
    int guesses = 0;
    for (const SettingsSearchHit& hit : hits) {
      if (!hit.direct && guesses++ < 6) searchRows_.push_back(hit.entry);
    }
  }
  const int rows = (int)searchRows_.size();
  searchSel_ = rows > 0 ? std::clamp(searchSel_, 0, rows - 1) : 0;

  if (entered && rows > 0) PickSearchHit(searchRows_[searchSel_]);
}

bool SettingsWindow::DrawSearchResults(float footer) {
  if (searchBuf_[0] == 0) return false;

  int picked = -1;
  ImGui::BeginChild("search_results", ImVec2(0, -footer));
  ImGui::Spacing();
  if (searchRows_.empty()) {
    ImGui::TextDisabled("%s", T("Nichts gefunden.", "Nothing found."));
  }
  for (int row = 0; row < (int)searchRows_.size(); ++row) {
    if (row == searchDirectRows_) {
      if (row > 0) ImGui::Spacing();
      ImGui::TextDisabled("%s", searchDirectRows_ == 0 ? T("Meintest du:", "Did you mean:")
                                                       : T("Ähnlich:", "Related:"));
    }
    const int entry = searchRows_[row];
    const bool selected = row == searchSel_;
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    ImGui::PushID(row);
    if (ImGui::Selectable(SearchLabel(entry), selected, ImGuiSelectableFlags_AllowOverlap)) {
      picked = entry;
    }
    if (selected && !ImGui::IsItemVisible()) ImGui::SetScrollHereY(0.5f);
    // Where it lives, right-aligned. Two controls share a name now and then;
    // this is what tells them apart.
    const std::string place = SearchPlace(entry);
    ImGui::SameLine(right - ImGui::CalcTextSize(place.c_str()).x);
    ImGui::TextDisabled("%s", place.c_str());
    ImGui::PopID();
  }
  ImGui::EndChild();

  if (picked >= 0) PickSearchHit(picked);
  return true;
}

void SettingsWindow::JumpTo(const char* key) {
  const int entry = SearchEntryForKey(key);
  if (entry >= 0) PickSearchHit(entry);
}

void SettingsWindow::PickSearchHit(int entry) {
  int tab = SearchTab(entry);
  const char* key = SearchKey(entry);
  jumpLabel_ = SearchLabel(entry);
  // Without a device only General, Source and Updates exist. Pointing at the
  // device list is the useful answer: that is the step between here and there.
  const CaptureSettings& capture = cfg().active().capture;
  if (capture.source == VideoSource::Card && SettingsTabNeedsDevice(tab) && capture.video.empty()) {
    tab = kTabSource;
    key = "videodev";
    searchNote_ = T("Erst ein Videogerät wählen.", "Select a video device first.");
    searchNoteTime_ = ImGui::GetTime();
  }
  activeTab_ = tab;
  jumpKey_ = key;
  jumpWait_ = 0;
  flashKey_ = nullptr;
  searchBuf_[0] = 0;
  searchQuery_.clear();
  searchRows_.clear();
  searchSel_ = 0;
}

void SettingsWindow::Anchor(const char* key) {
  const ImVec2 min = ImGui::GetItemRectMin();
  const ImVec2 max = ImGui::GetItemRectMax();
  if (jumpKey_ && std::strcmp(jumpKey_, key) == 0) {
    // A third of the way down rather than at the top edge: what belongs to it
    // above -- the heading, the line it depends on -- stays in view.
    ImGui::SetScrollFromPosY((min.y + max.y) * 0.5f - ImGui::GetWindowPos().y, 0.3f);
    flashKey_ = jumpKey_;
    flashStart_ = ImGui::GetTime();
    jumpKey_ = nullptr;
  }
  if (!flashKey_ || std::strcmp(flashKey_, key) != 0) return;

  const double t = (ImGui::GetTime() - flashStart_) / 1.5;
  if (t >= 1.0) {
    flashKey_ = nullptr;
    return;
  }
  const float fade = (float)(1.0 - t);
  const ImVec4 c = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
  const ImVec2 a(min.x - 4.0f, min.y - 2.0f);
  const ImVec2 b(max.x + 4.0f, max.y + 2.0f);
  const float rounding = ImGui::GetStyle().FrameRounding;
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(a, b, ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, 0.22f * fade)), rounding);
  draw->AddRect(a, b, ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, 0.9f * fade)), rounding, 1.5f);
}

// ------------------------------------------------------------- about › updates

void SettingsWindow::DrawUpdateSection() {
  AppSettings& app = cfg().app;
  ImGui::Spacing();
  ImGui::SeparatorText(T("Updates", "Updates"));
  Anchor("updatesec");

  ImGui::Checkbox(T("Beim Start nach Updates suchen", "Check for updates at startup"),
                  &app.checkUpdatesOnStart);
  Anchor("updatestartup");
  ImGui::SameLine();
  HelpMarker(T("Fragt GitHub nach der neuesten qBlank-Version und, wenn qBlank das ffmpeg "
               "selbst geladen hat, nach dem neuesten ffmpeg. Heruntergeladen wird nur auf "
               "Anfrage.",
               "Checks GitHub for the latest qBlank release and, if qBlank downloaded the "
               "ffmpeg in use, for the latest ffmpeg. Nothing is downloaded without asking."));

  if (!updater_) return;
  const UpdateStatus st = updater_->status();
  const bool busy = updater_->busy();

  ImGui::BeginDisabled(busy);
  if (ImGui::Button(T("Jetzt suchen", "Check now"))) updater_->CheckAsync();
  Anchor("updatecheck");
  ImGui::EndDisabled();

  ImGui::SameLine();
  switch (st.state) {
    case UpdateStatus::State::Idle:
      ImGui::TextDisabled("%s", T("noch nicht gesucht", "not checked yet"));
      break;
    case UpdateStatus::State::Checking:
      ImGui::TextDisabled("%s", T("wird gesucht ...", "checking ..."));
      break;
    case UpdateStatus::State::UpToDate:
      ImGui::TextDisabled(T("aktuell (neueste ist %s)", "up to date (newest is %s)"),
                          st.latestVersion.c_str());
      break;
    case UpdateStatus::State::Available:
      ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f),
                         T("%s ist verfügbar", "%s is available"), st.latestVersion.c_str());
      break;
    case UpdateStatus::State::Downloading:
      ImGui::TextDisabled("%s", T("wird geladen ...", "downloading ..."));
      break;
    case UpdateStatus::State::Ready:
      ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f),
                         T("%s ist eingesetzt", "%s is installed"), st.latestVersion.c_str());
      break;
    case UpdateStatus::State::Failed:
      ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.35f, 1.0f), "%s", UpdateErrorText(st).c_str());
      break;
  }

  // Whatever went wrong -- no network, no answer, a release with nothing in it
  // to install -- the way out is the same one, and it is worth putting on the
  // screen rather than describing. The addresses come from the answer where
  // there was one, so they are right even after the project has been renamed;
  // the built-in ones are for the case where there was no answer at all.
  if (st.state == UpdateStatus::State::Failed) {
    ImGui::Spacing();
    ImGui::TextWrapped("%s",
                       T("Manuell: Die aktuelle Version liegt auf der Release-Seite und auf "
                         "der Website.",
                         "Manual download: the current version is on the release page and the "
                         "website."));
    if (ImGui::Button(T("Releases öffnen", "Open releases"))) OpenUrl(ReleasePageUrl(st));
    ImGui::SameLine();
    if (ImGui::Button(T("Website öffnen", "Open website"))) OpenUrl(WebsiteUrl());
  }

  if (st.state == UpdateStatus::State::Available) {
    ImGui::Spacing();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(T("Herunterladen und installieren", "Download and install"))) {
      updater_->InstallAsync();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    HelpMarker(Format(T("Ersetzt %s.exe. Die bisherige Version wird beiseitegelegt und beim "
                        "nächsten Start entfernt, damit ein misslungenes Update nichts "
                        "beschädigt.",
                        "Replaces %s.exe. The previous version is moved aside and removed on "
                        "the next start, so a failed update breaks nothing."),
                      AppNameUtf8().c_str())
                   .c_str());
  }

  if (st.state == UpdateStatus::State::Ready) {
    ImGui::Spacing();
    if (ImGui::Button(T("Jetzt neu starten", "Restart now"))) restartRequested_ = true;
    ImGui::SameLine();
    ImGui::TextDisabled("%s", T("oder beim nächsten Start", "or on the next start"));
  }

  if (!st.notes.empty() && (st.state == UpdateStatus::State::Available ||
                            st.state == UpdateStatus::State::Ready)) {
    ImGui::Spacing();
    ImGui::SeparatorText(T("Änderungen", "Changelog"));
    DrawReleaseNotes(st.notes);
    ImGui::Spacing();
    if (ImGui::Button(T("Alle Änderungen", "Full changelog"))) OpenUrl(ChangelogUrl());
    ImGui::SetItemTooltip("%s", ChangelogUrl().c_str());
  }
}

// ----------------------------------------------------------------- about tab

void SettingsWindow::DrawAboutTab() {
  ImGui::Spacing();
  ImGui::SeparatorText(AppNameUtf8().c_str());
  ImGui::Text("Version %s", Updater::currentVersion());
  // The year off the compiler's date, "Sep 30 2026", so the range grows with
  // the first build of a new year without anyone remembering to.
  const char* year = __DATE__ + 7;
  if (std::strcmp(year, "2026") == 0) {
    ImGui::TextUnformatted("© 2026 NuclearMeltdown");
  } else {
    ImGui::Text("© 2026–%s NuclearMeltdown", year);
  }
  ImGui::Spacing();
  ImGui::TextWrapped("%s", T("Freie Software unter der GNU General Public License, Version 3. "
                             "Ohne jede Gewährleistung.",
                             "Free software under the GNU General Public License, version 3. "
                             "It comes with no warranty."));

  // The German pages sit under de/ on the website, so the links follow the
  // language the program speaks.
  ImGui::Spacing();
  ImGui::SeparatorText(T("Links", "Links"));
  const std::string site = WebsiteUrl() + T("de/", "");
  const std::string repo = RepositoryUrl();
  const struct {
    const char* label;
    std::string url;
  } links[] = {
      {T("Website", "Website"), site},
      {T("Handbuch", "Manual"), site + "manual.html"},
      {"GitHub", repo},
      {T("Fehler melden", "Report a bug"), repo + "/issues"},
  };
  for (const auto& link : links) {
    if (&link != links) ImGui::SameLine();
    if (ImGui::Button(link.label)) OpenUrl(link.url);
    ImGui::SetItemTooltip("%s", link.url.c_str());
  }

  // Last, because the release notes of a waiting update can run long.
  DrawUpdateSection();
}

// ---------------------------------------------------------------- source tab

void SettingsWindow::DrawScreenSource() {
  CaptureSettings& c = cfg().active().capture;
  ImGui::Spacing();
  ImGui::SeparatorText(T("Bildschirm", "Screen"));

  // Numbered like the monitors on the Display tab, from the same order.
  auto name = [&](size_t i) {
    const ScreenInfo& s = screens_[i];
    return Format("Monitor %d  (%dx%d)%s", (int)i + 1, s.width, s.height,
                  s.primary ? T("  [Hauptmonitor]", "  [primary]") : "");
  };
  const char* primary = T("Hauptmonitor", "Primary monitor");
  std::string preview = primary;
  if (!c.screen.empty() && !screens_.empty()) {
    // Unplugged, the capture falls back to the primary monitor.
    preview = T("Nicht angeschlossen — es läuft der Hauptmonitor",
                "Not connected — the primary monitor runs instead");
    for (size_t i = 0; i < screens_.size(); ++i) {
      if (screens_[i].id == c.screen) preview = name(i);
    }
  }
  ImGui::SetNextItemWidth(-150.0f);
  if (ImGui::BeginCombo("##screen", preview.c_str())) {
    if (ImGui::Selectable(primary, c.screen.empty())) c.screen.clear();
    ImGui::Separator();
    for (size_t i = 0; i < screens_.size(); ++i) {
      const bool selected = screens_[i].id == c.screen;
      if (ImGui::Selectable(name(i).c_str(), selected)) c.screen = screens_[i].id;
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  Anchor("screen");
  ImGui::SameLine();
  if (ImGui::Button(T("Aktualisieren", "Refresh"), ImVec2(-1, 0))) InvalidateDeviceLists();
  Anchor("refresh");

  ImGui::Checkbox(T("Mauszeiger mit aufnehmen", "Include the mouse pointer"), &c.screenCursor);
  Anchor("screencursor");
  ImGui::Checkbox(T("Vorschau", "Preview"), &c.screenPreview);
  Anchor("screenpreview");
  HelpMarker(T("Zeigt das aufgenommene Bild im Fenster, um zu prüfen, ob der richtige "
               "Bildschirm ankommt. Aus: nur Auflösung, Bildrate und Aufnahmezeit; das "
               "Fenster zeichnet seltener.",
               "Shows the captured picture in the window, to check it is the right screen. "
               "Off: only resolution, frame rate and recording time; the window redraws less "
               "often."));
  ImGui::Checkbox(T("qBlank auf der Aufnahme verbergen", "Hide qBlank from the capture"),
                  &c.screenHideSelf);
  Anchor("screenhideself");
  HelpMarker(T("Verbirgt qBlank vor jeder Bildschirmaufnahme, auch vor Remote Play und "
               "Remotedesktop. Aus: Liegt qBlank auf dem aufgenommenen Bildschirm, nimmt es "
               "sich selbst mit auf.",
               "Hides qBlank from all screen captures, including Remote Play and Remote "
               "Desktop. Off: if qBlank is on the captured screen, it records itself."));

  ImGui::Spacing();
  TextDisabledWrapped(T("Für Spiele im exklusiven Vollbild müssen die Vollbildoptimierungen von "
                        "Windows an sein.",
                        "To capture games in exclusive fullscreen, make sure Windows' fullscreen "
                        "optimizations are on."));
  TextDisabledWrapped(T("Ton: im Reiter Ton „Systemton ohne qBlank“ hinzufügen.",
                        "Sound: add “System sound without qBlank” on the Audio tab."));
}

void SettingsWindow::DrawWindowSource() {
  CaptureSettings& c = cfg().active().capture;
  ImGui::Spacing();
  ImGui::SeparatorText(T("Fenster", "Window"));

  std::string preview = T("Fenster wählen", "Choose a window");
  if (!c.windowApp.empty()) {
    preview = Format("%s  —  %s", c.windowTitle.c_str(), c.windowApp.c_str());
    // Without the title, the same program's other window stands in.
    bool open = windows_.empty();
    for (const WindowInfo& w : windows_) open = open || w.app == c.windowApp;
    if (!open) preview += T("  [nicht offen]", "  [not open]");
  }
  ImGui::SetNextItemWidth(-150.0f);
  if (ImGui::BeginCombo("##window", preview.c_str())) {
    for (size_t i = 0; i < windows_.size(); ++i) {
      const WindowInfo& w = windows_[i];
      const bool selected = w.app == c.windowApp && w.title == c.windowTitle;
      ImGui::PushID((int)i);
      const std::string name =
          Format("%s  —  %s  (%dx%d)", w.title.c_str(), w.app.c_str(), w.width, w.height);
      if (ImGui::Selectable(name.c_str(), selected)) {
        c.windowApp = w.app;
        c.windowTitle = w.title;
      }
      if (selected) ImGui::SetItemDefaultFocus();
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  Anchor("window");
  ImGui::SameLine();
  if (ImGui::Button(T("Aktualisieren", "Refresh"), ImVec2(-1, 0))) InvalidateDeviceLists();
  Anchor("refresh");

  ImGui::Checkbox(T("Mauszeiger mit aufnehmen", "Include the mouse pointer"), &c.screenCursor);
  Anchor("screencursor");
  ImGui::Checkbox(T("Vorschau", "Preview"), &c.screenPreview);
  Anchor("screenpreview");
  HelpMarker(T("Zeigt das aufgenommene Bild im Fenster, um zu prüfen, ob das richtige Fenster "
               "ankommt. Aus: nur Auflösung, Bildrate und Aufnahmezeit; das Fenster zeichnet "
               "seltener.",
               "Shows the captured picture in the window, to check it is the right window. "
               "Off: only resolution, frame rate and recording time; the window redraws less "
               "often."));

  ImGui::Spacing();
  TextDisabledWrapped(T("Das Fenster darf verdeckt sein, aber nicht minimiert.",
                        "The window may be covered, but not minimized."));
  TextDisabledWrapped(T("Ist der Titel weg, nimmt qBlank ein anderes Fenster desselben Programms.",
                        "If the title is gone, qBlank takes another window of the same program."));
  TextDisabledWrapped(T("Für Spiele im exklusiven Vollbild müssen die Vollbildoptimierungen von "
                        "Windows an sein.",
                        "To capture games in exclusive fullscreen, make sure Windows' fullscreen "
                        "optimizations are on."));
  TextDisabledWrapped(T("Ton: im Reiter Ton „Systemton ohne qBlank“ hinzufügen.",
                        "Sound: add “System sound without qBlank” on the Audio tab."));
}

void SettingsWindow::DrawSourceTab(const DeviceProbeResult& caps) {
  Profile& p = cfg().active();
  ImGui::Spacing();

  // Karte oder Bildschirm vor allem anderen: davon haengt ab, was darunter
  // ueberhaupt eine Frage ist.
  ImGui::SeparatorText(T("Bildquelle", "Picture source"));
  {
    int source = (int)p.capture.source;
    ImGui::RadioButton(T("Capture-Karte", "Capture card"), &source, (int)VideoSource::Card);
    Anchor("source");
    ImGui::SameLine();
    ImGui::RadioButton(T("Bildschirm", "Screen"), &source, (int)VideoSource::Screen);
    ImGui::SameLine();
    ImGui::BeginDisabled(!CanCaptureWindows());
    ImGui::RadioButton(T("Fenster", "Window"), &source, (int)VideoSource::Window);
    ImGui::EndDisabled();
    HelpMarker(T("Bildschirm nimmt einen ganzen Monitor auf, etwa für PC-Spiele, Fenster ein "
                 "einzelnes Programmfenster. Die Capture-Karte ist solange freigegeben.",
                 "Screen captures a whole monitor, e.g. for PC games; Window captures a "
                 "single program window. The capture card is released meanwhile."));
    p.capture.source = (VideoSource)source;
  }
  if (p.capture.source == VideoSource::Screen) {
    DrawScreenSource();
    return;
  }
  if (p.capture.source == VideoSource::Window) {
    DrawWindowSource();
    return;
  }

  ImGui::Spacing();
  ImGui::SeparatorText(T("Videogerät", "Video device"));

  // A hybrid card carries both halves on one board and cannot say which one has
  // something plugged into it -- this one exposes no input selector at all, so
  // there is nothing to read. Saying so is left to the person who plugged the
  // cable in, and everything that only makes sense for one of the two follows
  // from it.
  //
  // Eine Liste und nicht zwei. Analog-oder-digital und welches Kabel sind
  // technisch zwei Fragen, aber niemand hat sie je getrennt beantwortet: wer
  // "Composite" sagt, hat "analog" schon gesagt. Zwei Auswahllisten
  // uebereinander, von denen die zweite nur manchmal erscheint, waren dieselbe
  // Auskunft in zwei Zuegen. Die Umrechnung auf die zwei gespeicherten
  // Schluessel steht in SignalInputIndex / SignalInputApply.
  {
    int input = SignalInputIndex(p.capture.signalKind, p.capture.connector);
    ImGui::SetNextItemWidth(-260.0f);
    if (ComboEnum(T("Eingang", "Input"), &input, kSignalInputCount, SignalInputName,
                  SignalInputLook)) {
      SignalInputApply(input, &p.capture.signalKind, &p.capture.connector);
    }
    Anchor("input");
    ImGui::SameLine();
    HelpMarker(
        T("Was an der Karte steckt. Lässt sich nicht auslesen und bestimmt, welche "
          "Einstellungen erscheinen: die Videonorm nur bei analogen Eingängen, die "
          "Composite-Filter nur, wo Helligkeit und Farbe eine Leitung teilen.\n\n"
          "Jeder Eintrag beschreibt beim Darüberfahren seinen Stecker.",
          "What is plugged into the card. It cannot be detected and decides which settings "
          "appear: the video standard only on analogue inputs, the composite filters only "
          "where brightness and colour share one wire.\n\n"
          "Hover an entry to see which plug it is."));
    // Und dieselbe Beschreibung noch einmal fest darunter. Der Tooltip hilft
    // dem, der schon sucht; hier steht sie fuer den, der die Liste gar nicht
    // erst aufklappt, weil er nicht weiss, dass die Frage ihn betrifft.
    TextDisabledWrapped(SignalInputLook(input));
  }

  const char* preview = p.capture.video.name.empty()
                            ? T("— nichts ausgewählt —", "— nothing selected —")
                            : p.capture.video.name.c_str();
  ImGui::SetNextItemWidth(-150.0f);
  if (ImGui::BeginCombo("##videodev", preview)) {
    if (videoDevices_.empty()) {
      ImGui::TextDisabled(listsBusy_ ? T("Suche Geräte …", "Looking for devices …")
                                     : T("Keine Videogeräte gefunden", "No video devices found"));
    }
    // Nichts auszuwählen ist auch eine Auswahl. Niemand braucht das im Betrieb;
    // es ist der einzige Weg, den leeren Zustand zu sehen, ohne die
    // Konfigurationsdatei von Hand anzufassen.
    {
      const bool none = p.capture.video.empty();
      if (ImGui::Selectable(T("— kein Gerät —", "— no device —"), none) && !none) {
        p.capture.video = DeviceRef{};
        p.capture.format = FormatSel{};
        p.capture.videoStandard = -1;
        p.capture.crossbarInput = -1;
        embeddedAudioForDevice_.clear();
      }
      ImGui::Separator();
    }
    for (const VideoDeviceInfo& d : videoDevices_) {
      const bool selected = (d.id == p.capture.video.id);
      if (ImGui::Selectable(d.name.c_str(), selected) && !selected) {
        p.capture.video = DeviceRef{d.name, d.id, ""};
        p.capture.format = FormatSel{};  // format belongs to the old card
        // Und die Videonorm ebenso, aus genau demselben Grund. Auf dieser Karte
        // sind der analoge und der digitale Eingang zwei getrennte Geraete, das
        // Umstecken ist also ein Geraetewechsel -- und eine mitgeschleppte
        // PAL-Einstellung nagelt den digitalen Eingang auf 720x576 bei 50 Hz
        // fest, obwohl sie dort ueberhaupt nichts beschreibt.
        p.capture.videoStandard = -1;
        p.capture.crossbarInput = -1;
        embeddedAudioForDevice_.clear();
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  Anchor("videodev");
  ImGui::SameLine();
  if (ImGui::Button(T("Aktualisieren", "Refresh"), ImVec2(-1, 0))) InvalidateDeviceLists();
  Anchor("refresh");

  // Hier ist Schluss, solange nichts ausgewählt ist. Alles Weitere -- Norm,
  // Eingang, Ton, Format -- beschreibt eine Karte, und es gibt noch keine.
  // Sobald eine läuft, steht ohnehin fest, was sie kann, und der Rest erscheint
  // passend dazu.
  if (p.capture.video.empty()) {
    ImGui::Spacing();
    TextDisabledWrapped(
        T("Wähle oben eine Capture-Karte. Alles Weitere richtet sich danach, was sie "
          "liefert.",
          "Pick a capture card above. Everything else follows from what it delivers."));
    return;
  }

  // The driver's own dialog. Everything in it belongs to the card -- video
  // standard, decoder settings, whatever the vendor put there -- and none of it
  // has an equivalent here.
  ImGui::BeginDisabled(!captureRunning_);
  if (ImGui::Button(T("Karte konfigurieren ...", "Configure card ..."))) {
    deviceConfigRequested_ = true;
  }
  Anchor("cardcfg");
  ImGui::EndDisabled();
  WrappedTooltip(
      captureRunning_
          ? T("Eigener Dialog des Treibers. Das Bild läuft dabei weiter.",
              "The driver's own dialog. The picture keeps running while it is open.")
          : T("Nur bei laufender Karte.", "Only while the card is running."));

  // Mehr als ein Neustart des Graphen, und deshalb ein eigener Knopf: die Karte
  // wird vollstaendig losgelassen und alles, was fuer den vorherigen Eingang
  // gemessen oder gewaehlt wurde, wird verworfen.
  ImGui::SameLine();
  if (ImGui::Button(T("Karte neu einlesen", "Reinitialise card"))) {
    cardResetRequested_ = true;
  }
  Anchor("cardreinit");
  WrappedTooltip(
      T("Gibt die Karte frei, öffnet sie neu und beginnt von vorn.\n\n"
        "Videonorm und Format gehen auf automatisch zurück, da beide zum vorherigen Eingang "
        "gehören. Gerät und Eingang bleiben.",
        "Releases the card, reopens it and starts over.\n\n"
        "Video standard and format return to automatic, since both belong to the previous "
        "input. Device and input are kept."));

  if (!caps.error.empty()) {
    ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.35f, 1.0f), "%s", caps.error.c_str());
  }

  // ---- analogue video standard ----
  //
  // Only shown for cards that have an analogue decoder. On a pure HDMI input
  // there is no such thing and the list would be a row of dead entries.
  if (caps.availableStandards != 0 && analogueSource_) {
    ImGui::Spacing();
    ImGui::SeparatorText(T("Videonorm", "Video standard"));

    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##vstandard",
                          VideoStandardPickerName(p.capture.videoStandard).c_str())) {
      if (ImGui::Selectable(T("Nicht ändern", "Don't change"), p.capture.videoStandard == 0)) {
        p.capture.videoStandard = 0;
      }
      if (ImGui::Selectable(T("Automatisch", "Automatic"), p.capture.videoStandard == -1)) {
        p.capture.videoStandard = -1;
      }
      ImGui::Separator();
      const int chosen = VideoStandardGroupOf(p.capture.videoStandard);
      for (int i = 0; i < VideoStandardGroupCount(); ++i) {
        const long value = VideoStandardGroupPick(i, caps.availableStandards);
        if (value == 0) continue;
        if (ImGui::Selectable(VideoStandardGroupName(i), chosen == i)) {
          p.capture.videoStandard = value;
        }
        WrappedTooltip(VideoStandardGroupHint(i));
      }
      ImGui::EndCombo();
    }
    Anchor("vstandard");
    WrappedTooltip(
        T("Zusammengefasst nach dem, was sich am Kabel unterscheidet; die Buchstaben hinter "
          "PAL und SECAM betreffen nur die Fernsehübertragung. Automatisch probiert sie "
          "durch, bis der Decoder einrastet.",
          "Grouped by what differs over a cable; the letters after PAL and SECAM only matter "
          "for TV broadcasting. Automatic tries each until the decoder locks."));

    // Nur bei "Automatisch": die Region ordnet die Suche, und wo nicht gesucht
    // wird, ordnet sie nichts. Ein Regler, der gerade nichts tut, ist ein
    // Regler, an dem jemand dreht und sich wundert.
    if (p.capture.videoStandard == -1) {
      int region = (int)cfg().app.videoRegion;
      ImGui::SetNextItemWidth(-1.0f);
      if (ComboEnum("##videoregion", &region, kVideoRegionCount, VideoRegionName)) {
        cfg().app.videoRegion = (VideoRegion)region;
      }
      ImGui::SameLine();
      HelpMarker(
          T("Dein Land. Es legt fest, welche Norm zuerst probiert wird: Die Karte meldet nur "
            "die Zeilenfrequenz, und PAL 60 und NTSC M haben beide 525 Zeilen bei 60 Hz. "
            "Andere Normen werden trotzdem gefunden, nur langsamer.",
            "Your country. It sets which standard is tried first: the card only reports the "
            "line frequency, and PAL 60 and NTSC M both have 525 lines at 60 Hz. Other "
            "standards are still found, just more slowly."));
      // Was "Automatisch" hier gerade heisst. Sonst ist die haeufigste
      // Einstellung die einzige, die nichts ueber sich sagt.
      if (cfg().app.videoRegion == VideoRegion::Auto) {
        ImGui::TextDisabled(T("Laut Windows: %s", "According to Windows: %s"),
                            VideoRegionName((int)ResolveVideoRegion(VideoRegion::Auto)));
      }
    }

    // What the card is actually set to. On automatic this is the only way to see
    // what the search settled on, and "it looks right" is not the same as
    // knowing.
    //
    // "Eingestellt" ist dabei erst dann das richtige Wort, wenn die Norm auch
    // gilt. Waehrend der Suche steht auf der Karte eine Vermutung, die im
    // naechsten Moment verworfen wird -- sie so zu nennen, als waere sie eine
    // Entscheidung, ist die Auskunft, die vorher hier stand und die niemandem
    // half. Also sagt die Zeile jetzt, was gerade passiert, und nennt dabei
    // trotzdem die Norm: an ihr sieht man, dass die Suche laeuft und wie weit.
    const long shown = liveStandard_ != 0 ? liveStandard_ : caps.currentStandard;
    if (shown != 0) {
      const int idx = VideoStandardIndexOf(shown);
      const char* name = idx >= 0 ? VideoStandardName(idx) : "?";
      switch (standardSearch_) {
        case StandardSearch::Trying:
          ImGui::TextDisabled(T("Suche läuft: %s …", "Scanning: %s …"), name);
          break;
        case StandardSearch::Paused:
          // Nach einer erfolglosen Runde wird die Karte auf die beste Vermutung
          // gestellt und es wird gewartet. Das ist etwas anderes als Suchen, und
          // wer zusieht, soll nicht auf einen Fortschritt warten, den es gerade
          // nicht gibt.
          ImGui::TextDisabled(T("Suche pausiert, Karte auf %s", "Scanning paused, card set to %s"),
                              name);
          break;
        case StandardSearch::Colour:
          ImGui::TextDisabled(T("Farbe wird geprüft: %s", "Checking colour: %s"), name);
          break;
        // Das Ergebnis eines Suchlaufs von Hand ist hier keine eigene Zeile
        // wert. Ueber dem Bild vertritt es die Einblendung, die gerade
        // verschwindet -- hier steht ohnehin dauerhaft, worauf es hinauslief,
        // und das ist dieselbe Auskunft ohne Verfallsdatum.
        case StandardSearch::Result:
        case StandardSearch::Off:
          ImGui::TextDisabled(T("Eingestellt: %s", "In use: %s"), name);
          break;
      }
    }

    if (signalLocked_ >= 0) {
      if (signalLocked_ == 1) {
        ImGui::TextDisabled(T("Signal: eingerastet", "Signal: locked"));
      } else {
        ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.35f, 1.0f), "%s",
                           T("Signal: kein Lock — falsche Norm oder nichts angeschlossen",
                             "Signal: no lock — wrong standard, or nothing connected"));
      }
    }
  }

  // ---- crossbar ----
  ImGui::Spacing();
  ImGui::SeparatorText(T("Eingang der Karte", "Card input"));
  Anchor("crossbar");
  if (cfg().active().capture.video.empty()) {
    // Nothing has been probed yet, so there is nothing true to say about inputs.
    ImGui::TextDisabled(T("Zuerst ein Videogerät wählen.", "Select a video device first."));
  } else if (caps.crossbarInputs.empty()) {
    ImGui::TextDisabled(T("Diese Karte hat keine umschaltbaren Eingänge.",
                          "This card has no switchable inputs."));
  } else {
    std::string current = T("Nicht ändern", "Don't change");
    if (p.capture.crossbarInput >= 0 &&
        p.capture.crossbarInput < (int)caps.crossbarInputs.size()) {
      current = caps.crossbarInputs[(size_t)p.capture.crossbarInput].name;
    } else if (caps.currentInput >= 0 && caps.currentInput < (int)caps.crossbarInputs.size()) {
      // "Nicht ändern" heisst nicht "unbekannt". Wo die Karte sagt, worauf sie
      // steht, gehoert das dazu -- sonst liest sich die Zeile so, als waere der
      // Eingang offen, und der Nutzer sucht ihn woanders.
      current += Format(T(" (Karte steht auf %s)", " (card is on %s)"),
                        caps.crossbarInputs[(size_t)caps.currentInput].name.c_str());
    }
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##crossbar", current.c_str())) {
      if (ImGui::Selectable(T("Nicht ändern", "Don't change"), p.capture.crossbarInput < 0)) {
        p.capture.crossbarInput = -1;
      }
      for (size_t i = 0; i < caps.crossbarInputs.size(); ++i) {
        const bool selected = ((int)i == p.capture.crossbarInput);
        if (ImGui::Selectable(caps.crossbarInputs[i].name.c_str(), selected)) {
          p.capture.crossbarInput = (int)i;
        }
        if (selected) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    HelpMarker(T("Welcher physische Anschluss aufgenommen wird.",
                 "Which physical connector is captured."));
  }

  // Und was "Automatisch" oben dabei herausbekommen hat. Die Liste oben kann
  // das nicht sagen: sie steht vor der Geraetewahl, und aufgeloest wird erst an
  // der laufenden Karte -- aus dem gewaehlten Eingang, sonst aus dem, auf dem
  // die Karte steht, sonst Composite.
  if (analogueSource_ && p.capture.connector == AnalogConnector::Auto) {
    ImGui::Spacing();
    TextDisabledWrapped(Format(T("Automatisch bedeutet hier: %s.", "Automatic here means: %s."),
                               AnalogConnectorName((int)connector_))
                            .c_str());
  }

  // ---- audio source ----
  ImGui::Spacing();
  ImGui::SeparatorText(T("Ton der Quelle", "Source audio"));
  Anchor("srcaudio");

  bool useEmbedded = (p.capture.audioSource == AudioSource::Embedded);
  if (ImGui::Checkbox(T("Eingebettetes Audio des Videogeräts verwenden",
                        "Use the video device's embedded audio"),
                      &useEmbedded)) {
    p.capture.audioSource = useEmbedded ? AudioSource::Embedded : AudioSource::Manual;
  }
  Anchor("embedded");
  ImGui::SameLine();
  HelpMarker(T("Sucht das Aufnahmegerät auf derselben Karte.",
               "Finds the recording device on the same card."));

  if (useEmbedded) {
    // Resolve once per selected video device, purely to show what was found.
    if (embeddedAudioForDevice_ != p.capture.video.id) {
      embeddedAudioForDevice_ = p.capture.video.id;
      embeddedAudioName_.clear();
      embeddedLooking_ = !p.capture.video.empty();
      if (embeddedLooking_) {
        VideoDeviceInfo vinfo;
        vinfo.name = p.capture.video.name;
        vinfo.id = p.capture.video.id;
        deviceJobs_.Ask([this, vinfo, round = probeRound_] {
          AudioDeviceInfo found;
          std::string name = FindEmbeddedAudioDevice(vinfo, &found) ? found.name : std::string();
          return [this, id = vinfo.id, round, name] {
            if (round != probeRound_ || id != embeddedAudioForDevice_) return;
            embeddedAudioName_ = name;
            embeddedLooking_ = false;
          };
        });
      }
    }
    char shown[256];
    std::snprintf(shown, sizeof(shown), "%s",
                  embeddedLooking_ ? T("Suche …", "Looking …")
                  : embeddedAudioName_.empty()
                      ? T("— nichts Passendes gefunden —", "— nothing matching found —")
                      : embeddedAudioName_.c_str());
    ImGui::BeginDisabled(true);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##embedded", shown, sizeof(shown), ImGuiInputTextFlags_ReadOnly);
    ImGui::EndDisabled();
    if (!embeddedLooking_ && embeddedAudioName_.empty() && !p.capture.video.empty()) {
      ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.35f, 1.0f),
                         T("Nichts gefunden — Haken entfernen und manuell wählen.",
                           "Nothing found — untick and choose manually."));
    }
  } else {
    ImGui::SetNextItemWidth(-1.0f);
    const char* audioPreview =
        p.capture.audioSource == AudioSource::None
            ? T("— kein Ton —", "— no audio —")
            : (p.capture.audio.name.empty() ? T("— nichts ausgewählt —", "— nothing selected —")
                                            : p.capture.audio.name.c_str());
    if (ImGui::BeginCombo("##audiodev", audioPreview)) {
      if (ImGui::Selectable(T("— kein Ton —", "— no audio —"),
                            p.capture.audioSource == AudioSource::None)) {
        p.capture.audioSource = AudioSource::None;
      }
      for (const AudioDeviceInfo& d : audioInputs_) {
        const bool selected =
            (p.capture.audioSource == AudioSource::Manual && d.id == p.capture.audio.id);
        std::string label = d.name + (d.cardAudio ? "   [" + d.via + "]" : "");
        if (ImGui::Selectable(label.c_str(), selected)) {
          p.capture.audioSource = AudioSource::Manual;
          p.capture.audio = d.ToRef();
        }
        if (selected) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    Anchor("audiodev");
  }

  // ---- format ----
  ImGui::Spacing();
  ImGui::SeparatorText("Format");
  Anchor("format");

  if (!caps.ok || caps.caps.empty()) {
    ImGui::TextDisabled(!probingId_.empty()
                            ? T("Frage das Gerät ab …", "Asking the device …")
                            : T("Erst ein Videogerät auswählen.", "Select a video device first."));
    return;
  }

  const std::vector<std::string> subtypes = caps.caps.Subtypes();
  FormatSel& fmt = p.capture.format;

  const char* autoLabel = T("Automatisch", "Automatic");
  const std::string subtypePreview =
      fmt.autoSubtype ? Format("%s (%s)", autoLabel, fmt.subtype.c_str()) : fmt.subtype;
  ImGui::SetNextItemWidth(-260.0f);
  if (ImGui::BeginCombo(T("Farbformat", "Colour format"), subtypePreview.c_str())) {
    // Picked the way a new card is, and again after every change of input. A
    // fixed rate goes back to that of the signal, as a picked size wants it.
    if (ImGui::Selectable(autoLabel, fmt.autoSubtype) && !fmt.autoSubtype) {
      const double mode = fmt.fps <= 0.0 ? fmt.fps : kFpsNative;
      fmt = caps.caps.PickDefault();
      fmt.fps = mode;
    }
    if (fmt.autoSubtype) ImGui::SetItemDefaultFocus();
    for (const std::string& s : subtypes) {
      const bool selected = !fmt.autoSubtype && s == fmt.subtype;
      if (ImGui::Selectable(s.c_str(), selected) && !selected) {
        fmt.autoSubtype = false;
        // The one automatic had picked only becomes a choice; nothing to re-pick.
        if (s != fmt.subtype) {
          fmt.subtype = s;
          // Resolution and rate lists depend on the format, so re-pick.
          std::vector<ResolutionOption> res = caps.caps.Resolutions(s);
          if (!res.empty()) {
            fmt.width = res.front().width;
            fmt.height = res.front().height;
          }
          // The rate goes back to a mode rather than to a number carried over
          // from a format that may not offer it. Those entries lead the list, so
          // taking the first unforced one lands on one of them.
          for (const FpsOption& f : caps.caps.FpsList(s, fmt.width, fmt.height)) {
            if (f.forced) continue;
            fmt.fps = f.native ? kFpsNative : (f.highest ? kFpsHighest : f.fps);
            break;
          }
        }
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  Anchor("subtype");
  ImGui::SameLine();
  HelpMarker(T("Automatisch: qBlank wählt bei jedem Eingangswechsel neu. "
               "YUY2, UYVY, NV12: unkomprimiert. MJPG: braucht einen Decoder, kostet Latenz.",
               "Automatic: qBlank picks again whenever the input changes. "
               "YUY2, UYVY, NV12: uncompressed. MJPG: needs a decoder, costs latency."));

  ImGui::BeginDisabled(customFormat_);
  const std::vector<ResolutionOption> resolutions = caps.caps.Resolutions(fmt.subtype);
  std::string resPreview = Format("%dx%d", fmt.width, fmt.height);
  ImGui::SetNextItemWidth(-260.0f);
  if (ImGui::BeginCombo(T("Auflösung", "Resolution"), resPreview.c_str())) {
    bool forcedSection = false;
    for (const ResolutionOption& r : resolutions) {
      if (r.forced && !forcedSection) {
        forcedSection = true;
        ImGui::SeparatorText(T("Nicht gemeldet", "Not reported"));
      }
      const bool selected = (r.width == fmt.width && r.height == fmt.height);
      std::string label = Format("%dx%d", r.width, r.height);
      if (ImGui::Selectable(label.c_str(), selected) && !selected) {
        fmt.width = r.width;
        fmt.height = r.height;
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  Anchor("resolution");
  // Gleich unter der Auswahl, um die es geht. Steht die passende Groesse schon
  // drin und wartet nur auf den Neustart, gibt es nichts mehr zu sagen.
  if (resolutionMismatch_ > 0) {
    const ResolutionOption fit = caps.caps.FittingResolution(fmt.subtype, resolutionMismatch_);
    if (fit.width > 0 && (fit.width != fmt.width || fit.height != fmt.height)) {
      TextWarningWrapped(
          Format(T("Die Auflösung passt nicht zur Videonorm (%d sichtbare Zeilen). Die Karte "
                   "muss das Bild dafür strecken oder auffüllen.",
                   "The resolution does not fit the video standard (%d visible lines). The "
                   "card has to stretch or pad the picture for it."),
                 resolutionMismatch_)
              .c_str());
      if (ImGui::Button(Format(T("%dx%d einstellen", "Use %dx%d"), fit.width, fit.height).c_str())) {
        fmt.width = fit.width;
        fmt.height = fit.height;
        // Eine feste Rate war unter der falschen Groesse gewaehlt; die Norm
        // sagt selbst, welche richtig ist. Wie in App::ReleaseStandardBoundFormat.
        if (fmt.fps > 0.0) fmt.fps = kFpsNative;
      }
    }
  }
  // Nur an einem analogen Eingang: nur dort gibt es eine Norm, an der die
  // Groesse zu messen waere.
  if (analogueSource_) {
    ImGui::Checkbox(T("Auflösung an die Videonorm anpassen",
                      "Match the resolution to the video standard"),
                    &cfg().app.matchResolution);
    Anchor("matchresolution");
    ImGui::SameLine();
    HelpMarker(T("Passt die Auflösung nicht zur Norm (etwa 720x480 an PAL), stellt qBlank die "
                 "passende selbst ein. Aus: es fragt auf dem Bild.",
                 "If the resolution does not fit the standard (say 720x480 on PAL), qBlank sets "
                 "the right one itself. Off: it asks on the picture."));
  }

  const std::vector<FpsOption> fpsOptions = caps.caps.FpsList(fmt.subtype, fmt.width, fmt.height);
  // Was die beiden Betriebsarten gerade bedeuten. Aufgeloest wird erst beim
  // Oeffnen der Karte, aber die Karte steht ja da -- und "Rate des Signals"
  // ohne die Zahl daneben ist ein Versprechen, das man erst nach einem Neustart
  // nachpruefen koennte.
  const double nativeNow = caps.caps.NativeFps(fmt.subtype, fmt.width, fmt.height);
  const double highestNow = caps.caps.HighestFps(fmt.subtype, fmt.width, fmt.height);
  auto fpsEntryLabel = [&](double stored) {
    std::string text = FpsLabel(stored);
    const double resolved = stored < 0.0 ? nativeNow : (stored <= 0.0 ? highestNow : 0.0);
    if (resolved > 0.0) text += "  (" + FpsLabel(resolved) + ")";
    return text;
  };
  // Die Rate des Signals ist die Voreinstellung, angeboten wird sie aber nur,
  // wo eine Norm bekannt ist. Am Digitaleingang laeuft sie auf die hoechste
  // hinaus, also steht dort auch das da -- ohne den gespeicherten Wert
  // anzufassen, damit dasselbe Profil an einer analogen Quelle wieder die Rate
  // des Signals nimmt.
  const bool nativeOffered = std::any_of(fpsOptions.begin(), fpsOptions.end(),
                                         [](const FpsOption& f) { return f.native; });
  const double shownFps = (fmt.fps < 0.0 && !nativeOffered) ? kFpsHighest : fmt.fps;
  ImGui::SetNextItemWidth(-260.0f);
  if (ImGui::BeginCombo(T("Bildrate", "Frame rate"), fpsEntryLabel(shownFps).c_str())) {
    bool forcedSection = false;
    bool namedSection = false;
    for (const FpsOption& f : fpsOptions) {
      if (!f.highest && !f.native && !namedSection) {
        namedSection = true;
        ImGui::SeparatorText(T("Gemeldet", "Reported"));
      }
      if (f.forced && !forcedSection) {
        forcedSection = true;
        ImGui::SeparatorText(T("Für diese Auflösung nicht gemeldet",
                               "Not reported for this resolution"));
      }
      const double stored = f.native ? kFpsNative : (f.highest ? kFpsHighest : f.fps);
      const bool selected = f.native    ? (shownFps < 0.0)
                            : f.highest ? (std::fabs(shownFps) < 0.05)
                                        : (shownFps > 0.0 && std::fabs(f.fps - shownFps) < 0.05);
      if (ImGui::Selectable(fpsEntryLabel(stored).c_str(), selected) && !selected) {
        fmt.fps = stored;
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  Anchor("fps");
  ImGui::SameLine();
  HelpMarker(T("\"Rate des Signals\" nimmt die Rate der Videonorm: 50 Hz bei PAL und SECAM, "
               "59,94 bei NTSC, 60 bei PAL 60. Nur verfügbar, wenn die Norm bekannt ist. "
               "\"Höchste verfügbare\" nimmt das Maximum der Karte, oft mehr, als die Quelle "
               "liefert.\n\n"
               "Beide folgen Moduswechseln der Quelle. In Klammern steht der aktuelle Wert, "
               "darunter, was der Treiber meldet.",
               "\"The signal's rate\" uses the video standard's rate: 50 Hz for PAL and "
               "SECAM, 59.94 for NTSC, 60 for PAL 60. Only offered when the standard is "
               "known. \"Highest available\" uses the card's maximum, often more than the "
               "source delivers.\n\n"
               "Both follow mode changes of the source. The brackets show the current value, "
               "the line below what the driver reports."));
  ImGui::EndDisabled();

  ImGui::Checkbox(T("Werte von Hand eingeben", "Enter values manually"), &customFormat_);
  Anchor("manualfmt");
  if (customFormat_) {
    ImGui::Indent();
    ImGui::SetNextItemWidth(Px(120.0f));
    ImGui::InputInt(T("Breite", "Width"), &customWidth_, 0, 0);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Px(120.0f));
    ImGui::InputInt(T("Höhe", "Height"), &customHeight_, 0, 0);
    float fpsInput = (float)customFps_;
    ImGui::SetNextItemWidth(Px(120.0f));
    if (ImGui::InputFloat(T("Bilder/s", "Frames/s"), &fpsInput, 0.0f, 0.0f, "%.3f")) {
      customFps_ = fpsInput;
    }
    if (ImGui::Button(T("Anwenden##customfmt", "Apply##customfmt"))) {
      // The ends are the longest edge D3D11 can address and a rate no display
      // hardware reaches, not a judgement about what is sensible. This is the
      // box for forcing something the driver never mentioned; the point of it
      // is that qBlank does not argue.
      fmt.width = Clamp(customWidth_, 16, 16384);
      fmt.height = Clamp(customHeight_, 16, 16384);
      fmt.fps = Clamp(customFps_, 1.0, 1000.0);
    }
    ImGui::Unindent();
  }

  fmt.forced = !caps.caps.IsAdvertised(fmt.subtype, fmt.width, fmt.height, fmt.fps);
  ImGui::Spacing();
  if (fmt.forced) {
    ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1.0f), T("Erzwungen: %s", "Forced: %s"),
                       fmt.Label().c_str());
  } else {
    ImGui::TextDisabled(T("Gewählt: %s", "Selected: %s"), fmt.Label().c_str());
  }
}

// ----------------------------------------------------------------- image tab

// The demodulation window the shader will use for this slider position. Kept
// in step with kCleanPS by hand -- there is no way to ask a compiled shader what
// it decided, and a readout that quietly drifts from the truth is worse than no
// readout at all.
int DotCrawlWindow(float slider, float carrierPeriod) {
  const float cycles = 8.2f - slider * 5.9f;
  int r = (int)std::floor(cycles * carrierPeriod * 0.5f + 0.5f);
  r = r < 2 ? 2 : (r > 12 ? 12 : r);
  return 2 * r + 1;
}

// The slider positions that actually do something different.
//
// The shader rounds the window to a whole number of samples and clamps it, so
// between two positions that land on the same one nothing happens at all: the
// control moves and the picture does not. How many distinct stops there are is
// not fixed either -- it falls out of the carrier period, which comes from the
// video standard and the capture width, so NTSC and PAL do not have the same
// ones.
//
// Rather than invert the shader's arithmetic and risk drifting from it, the
// range is simply walked and the distinct windows collected. One canonical
// position per window, in the middle of the span that produces it, so a stop is
// as far as possible from the rounding boundaries on either side.
struct DotCrawlStep {
  float slider;
  int window;
};

int BuildDotCrawlSteps(float carrierPeriod, DotCrawlStep* out, int max) {
  int count = 0;
  int lastWindow = -1;
  float spanStart = 0.0f;
  const int kProbe = 400;
  for (int i = 0; i <= kProbe; ++i) {
    const float s = (float)i / (float)kProbe;
    const int w = DotCrawlWindow(s, carrierPeriod);
    if (w != lastWindow) {
      if (lastWindow >= 0 && count < max) {
        out[count].slider = (spanStart + (float)(i - 1) / (float)kProbe) * 0.5f;
        out[count].window = lastWindow;
        ++count;
      }
      lastWindow = w;
      spanStart = s;
    }
  }
  if (lastWindow >= 0 && count < max) {
    out[count].slider = (spanStart + 1.0f) * 0.5f;
    out[count].window = lastWindow;
    ++count;
  }
  return count;
}

// What that window does, from the measurements on the capture card rather than
// from theory: how much of the pattern goes, and what it costs in horizontal
// sharpness. Between the measured points it is a straight line, which is honest
// enough for a readout meant to give a sense of the trade.
void DotCrawlEffect(int window, float* removed, float* softer) {
  struct Point {
    int window;
    float removed;
    float softer;
  };
  static const Point kMeasured[] = {
      {25, 42.0f, 7.0f}, {15, 61.0f, 11.0f}, {11, 74.0f, 14.0f}, {9, 81.0f, 17.0f},
  };
  const int count = (int)(sizeof(kMeasured) / sizeof(kMeasured[0]));
  if (window >= kMeasured[0].window) {
    *removed = kMeasured[0].removed;
    *softer = kMeasured[0].softer;
    return;
  }
  for (int i = 1; i < count; ++i) {
    if (window >= kMeasured[i].window) {
      const Point& a = kMeasured[i - 1];
      const Point& b = kMeasured[i];
      const float t = (float)(a.window - window) / (float)(a.window - b.window);
      *removed = a.removed + (b.removed - a.removed) * t;
      *softer = a.softer + (b.softer - a.softer) * t;
      return;
    }
  }
  *removed = kMeasured[count - 1].removed;
  *softer = kMeasured[count - 1].softer;
}

void SettingsWindow::DrawImageTab() {
  ImageSettings& img = cfg().active().image;

  // Der volleste Reiter, also mit einer Reihenfolge und nicht mit einer
  // Reihung: oben, was man an einer frisch angeschlossenen Quelle als Erstes
  // anfasst, unten, was von allein richtig steht.
  //
  //   Skalierung samt Seitenverhältnis, Drehung und Zeilenverdopplung
  //   Bildregler
  //   Halbbilder
  //   Zuschnitt, und direkt darunter das native Raster
  //   Composite-Filter
  //   Bildröhre
  //   Vergleich, hinter allem, was er abschaltet
  //   Farbe
  //
  // Zuschnitt und natives Raster stehen beieinander, weil beide dasselbe
  // zählen: Pixel der Quelle. Farbe steht ganz unten, weil sie sich selbst
  // misst -- wer sie anfasst, tut es, weil die Messung danebenlag, und das ist
  // der seltene Fall.
  //
  // Seitenverhältnis, Drehung und Zeilen verdoppeln standen früher ohne eigene
  // Überschrift hinter der Bildröhre und sahen dadurch nach Nostalgie aus,
  // obwohl sie zur Skalierung gehören und in jede Aufnahme durchschlagen.
  //
  // Was diese Reihenfolge schuldig bleibt: Composite-Filter und Bildröhre sind
  // Nachbarn geworden. Der eine holt heraus, was die Übertragung verdorben
  // hat, der andere legt mit Absicht wieder etwas darüber, und nebeneinander
  // sehen sie aus wie zwei Geschmacksfragen. Trennen kann sie nur ein eigener
  // Reiter, nicht eine andere Sortierung innerhalb dieses hier.
  ImGui::Spacing();

  // Am Bildschirm gibt es nichts zu entflechten, zu entstoeren oder
  // zuzuschneiden (App::EffectiveImage). Was bleibt, ist die Skalierung.
  const bool screen = cfg().active().capture.source != VideoSource::Card;

  // Ganz oben, weil sonst jeder Regler darunter scheinbar nichts tut.
  if (bypassOn_ && !screen) {
    TextWarningWrapped(Format(T("Alle Filter sind aus (%s). Schärfen, Bildregler, natives Pixelraster, "
                                "Composite-Filter und Bildröhre wirken erst danach wieder.",
                                "All filters are off (%s). Sharpen, picture controls, native "
                                "pixel grid, composite filter and cathode ray tube take effect "
                                "again once they are back on."),
                              HotkeyText(cfg().hotkeys[HotkeyAction::BypassFilters]).c_str())
                           .c_str());
    if (ImGui::Button(T("Filter wieder an", "Filters back on"))) bypassToggleRequested_ = true;
    ImGui::Spacing();
  }

  ImGui::SeparatorText(T("Skalierung", "Scaling"));
  Anchor("scaling");
  int filter = (int)img.filter;
  ImGui::SetNextItemWidth(-260.0f);
  if (ComboEnum(T("Filter", "Filter"), &filter, 5, ScaleFilterName, ScaleFilterHelp)) {
    img.filter = (ScaleFilter)filter;
  }
  Anchor("filter");
  ImGui::SameLine();
  HelpMarker(ScaleFilterHelp(filter));
  if (screen) {
    TextDisabledWrapped(T("Am Bildschirm wirkt nur das, und nur auf die Vorschau im Fenster.",
                          "On the screen only this applies, and only to the preview in the "
                          "window."));
    return;
  }

  ImGui::SetNextItemWidth(-260.0f);
  ImGui::SliderFloat(T("Schärfen", "Sharpen"), &img.sharpen, 0.0f, 1.0f, "%.2f");
  Anchor("sharpen");
  ResetOnRightClick(img.sharpen, kImage.sharpen);
  ImGui::SameLine();
  HelpMarker(T("Hebt Kanten nach der Skalierung an. 0 schaltet es ab.",
               "Lifts edges after scaling. 0 turns it off."));

  int aspect = (int)img.aspect;
  ImGui::SetNextItemWidth(-260.0f);
  if (ComboEnum(T("Seitenverhältnis", "Aspect ratio"), &aspect, kAspectModeCount, AspectName,
                AspectHelp)) {
    img.aspect = (AspectMode)aspect;
  }
  Anchor("aspect");
  ImGui::SameLine();
  HelpMarker(AspectHelp(aspect));

  // Quadratische Pixel ohne Breite der Quelle nehmen die Proben der Karte als
  // Konsolenpixel, und die sind analog nie quadratisch: 720 auf 576 Zeilen ist
  // schmaler als 4:3, 720 auf 480 breiter. Das sieht aus wie ein Fehler im
  // Bild, also steht es hier, wo es eingestellt wird. Nur wo es die Breite
  // auch zu setzen gibt.
  if (img.aspect == AspectMode::SquarePixels && img.nativeWidth == 0 && analogueSource_ &&
      (sourceHeight_ == 0 || sourceHeight_ <= kStandardLines)) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.72f, 0.35f, 1.0f));
    ImGui::TextWrapped("%s",
                       T("Ohne Breite der Quelle zählen die Proben der Karte als Pixel: PAL wird "
                         "zu schmal, 60 Hz zu breit. Für 4:3 „Quelle“ nehmen oder unten die "
                         "Breite der Quelle angeben.",
                         "Without a source width the card's samples count as pixels: PAL comes "
                         "out too narrow, 60 Hz too wide. For 4:3 pick \"Source\" or set the "
                         "source width below."));
    ImGui::PopStyleColor();
  }

  // Nur beim Strecken gibt es nichts umzurechnen: dieser Modus hat ueberhaupt
  // keine eigene Form, er nimmt die des Fensters.
  if (img.aspect != AspectMode::Stretch) {
    // Eigene ID: derselbe Satz steht auf dieser Seite ein zweites Mal, weiter
    // unten bei den Bildreglern. ImGui bildet die ID aus der Beschriftung, also
    // waeren es sonst zwei Bedienelemente mit einer ID -- und ImGui sagt das
    // auch, mit einem Fenster mitten im Bild. Umbenennen waere die schlechtere
    // Antwort: der Satz sagt an beiden Stellen dasselbe und soll auch gleich
    // lauten.
    ImGui::PushID("square-pixel-to-output");
    ImGui::Checkbox(T("Auch für Aufnahme, Screenshot und Kamera",
                      "Apply to recording, screenshots and camera"),
                    &img.squarePixelOutput);
                    Anchor("aspectout");
    ImGui::PopID();
    ImGui::SameLine();
    HelpMarker(
        T("Das Fenster zeichnet nicht-quadratische Pixel direkt, Dateien brauchen "
          "quadratische. Aufnahme, Screenshot und Kamera werden deshalb mit dem Filter oben "
          "umgerechnet: Aus 720x576 wird 768x576.\n\n"
          "Über HDMI entfällt das, dort sind die Pixel schon quadratisch.",
          "The window draws non-square pixels directly; files need square ones. Recordings, "
          "screenshots and the camera are therefore resampled with the filter above: 720x576 "
          "becomes 768x576.\n\n"
          "Skipped over HDMI, where pixels are already square."));
  }

  int rotation = (int)img.rotation;
  ImGui::SetNextItemWidth(-260.0f);
  if (ComboEnum(T("Drehung", "Rotation"), &rotation, kRotationCount, RotationName)) {
    img.rotation = (Rotation)rotation;
  }
  Anchor("rotation");
  ImGui::SameLine();
  HelpMarker(T("Gilt auch für Aufnahme und Screenshots.",
               "Applies to recordings and screenshots as well."));

  // Nur wenn die Quelle wirklich halb so hoch ankommt.
  //
  // Frueher stand hier bloss `analogueSource_`, und damit war das Kaestchen an
  // jedem Composite-Eingang anklickbar -- auch an einem GameCube mit 576i.
  // Sichtbar passiert dort nichts, denn das Bild wird ohnehin auf das
  // eingestellte Seitenverhaeltnis gepasst und nicht auf die Zeilenzahl; die
  // Aufnahme dagegen kam mit 720x1152 heraus. Eine Einstellung, die dort nichts
  // tut, wo man hinsieht, und etwas dort, wo man nicht hinsieht, ist die
  // schlechteste Sorte.
  if (analogueSource_ && sourceHeight_ > 0 && sourceHeight_ <= kHalfHeightLines) {
    ImGui::Checkbox(T("Zeilen verdoppeln", "Double lines"), &img.lineDouble);
    Anchor("linedouble");
    ImGui::SameLine();
    HelpMarker(T("Für 240p/288p-Quellen, die halb so hoch ankommen wie sie sollen.",
                 "For 240p/288p sources that arrive half as tall as they should."));
  }

  // Die vier Regler, die sonst jedes Aufnahmeprogramm hat -- hier aber im
  // Shader statt auf der Karte. Die Regler der Karte werden beim Start
  // neutralisiert, damit das, was ankommt, das ist, was die Konsole geschickt
  // hat; siehe NeutraliseProcAmp.
  ImGui::Spacing();
  ImGui::SeparatorText(T("Bildregler", "Picture controls"));
  Anchor("procamp");

  ImGui::SetNextItemWidth(-260.0f);
  ImGui::SliderFloat(T("Helligkeit", "Brightness"), &img.brightness, -1.0f, 1.0f, "%+.2f");
  Anchor("brightness");
  ResetOnRightClick(img.brightness, kImage.brightness);
  ImGui::SameLine();
  HelpMarker(T("Hebt oder senkt das ganze Bild. 0 ist neutral.",
               "Lifts or lowers the whole picture. 0 is neutral."));

  ImGui::SetNextItemWidth(-260.0f);
  ImGui::SliderFloat(T("Kontrast", "Contrast"), &img.contrast, 0.0f, 2.0f, "%.2f");
  Anchor("contrast");
  ResetOnRightClick(img.contrast, kImage.contrast);
  ImGui::SameLine();
  HelpMarker(T("Spreizt um das mittlere Grau. 1 ist neutral.",
               "Spreads around mid grey. 1 is neutral."));

  ImGui::SetNextItemWidth(-260.0f);
  ImGui::SliderFloat(T("Sättigung", "Saturation"), &img.saturation, 0.0f, 2.0f, "%.2f");
  Anchor("saturation");
  ResetOnRightClick(img.saturation, kImage.saturation);
  ImGui::SameLine();
  HelpMarker(T("0 macht das Bild grau, 1 ist neutral, 2 doppelt so bunt.",
               "0 makes the picture grey, 1 is neutral, 2 twice as colourful."));

  ImGui::SetNextItemWidth(-260.0f);
  ImGui::SliderFloat(T("Farbton", "Hue"), &img.hue, -180.0f, 180.0f, "%+.0f°");
  Anchor("hue");
  ResetOnRightClick(img.hue, kImage.hue);
  ImGui::SameLine();
  HelpMarker(
      T("Dreht alle Farben um denselben Winkel. 0 ist neutral. Bei NTSC über "
        "Composite das, was am Fernseher der Farbton-Regler war.",
        "Turns every colour by the same angle. 0 is neutral. On NTSC over composite this "
        "is what the tint knob on a television did."));

  const bool neutral = img.brightness == 0.0f && img.contrast == 1.0f &&
                       img.saturation == 1.0f && img.hue == 0.0f;
  ImGui::BeginDisabled(neutral);
  if (ImGui::Button(T("Zurücksetzen", "Reset"))) {
    img.brightness = 0.0f;
    img.contrast = 1.0f;
    img.saturation = 1.0f;
    img.hue = 0.0f;
  }
  Anchor("procampreset");
  ImGui::EndDisabled();

  ImGui::SameLine();
  // Eigene ID, siehe oben beim gleichlautenden Kaestchen der quadratischen
  // Pixel.
  ImGui::PushID("procamp-to-output");
  ImGui::Checkbox(T("Auch für Aufnahme, Screenshot und Kamera",
                    "Apply to recording, screenshots and camera"),
                  &img.procAmpToOutput);
                  Anchor("procampout");
  ImGui::PopID();
  ImGui::SameLine();
  HelpMarker(
      T("Standardmäßig aus: Sauber aufnehmen und später anpassen verliert nichts, mit "
        "angehobenem Kontrast aufnehmen schneidet Lichter und Tiefen dauerhaft ab.\n\n"
        "An, wenn Aufnahmen unbearbeitet hochgeladen werden. Die Anzeige nutzt die Regler "
        "immer.",
        "Off by default: recording clean and adjusting later loses nothing, while recording "
        "with raised contrast clips highlights and shadows for good.\n\n"
        "Turn on if recordings are uploaded unedited. The display always uses the controls."));

  // Halbbilder. Nicht danach gezeigt, ob die Quelle analog ist, sondern danach,
  // ob sie ueberhaupt Halbbilder hat -- 1080i und 480i gibt es auch ueber HDMI,
  // und wer so etwas anschliesst, braucht diese Regler genauso.
  //
  // Solange noch nichts gemessen ist, wird gezeigt. Etwas zu verstecken, bevor
  // man weiss, ob es gebraucht wird, ist der schlechtere Fehler von beiden.
  if (analogueSource_ || sourceInterlaced_ || sourceHeight_ == 0) {
  ImGui::Spacing();
  ImGui::SeparatorText(T("Halbbilder", "Fields"));
  Anchor("fields");
  ImGui::Checkbox(T("Nur bei interlaced Quellen anwenden", "Only apply to interlaced sources"),
                  &img.deinterlaceAuto);
  Anchor("interlacedonly");
  ImGui::SameLine();
  HelpMarker(T("Erkennung aus dem Bild, nicht aus der Formatmeldung der Karte.",
               "Detected from the picture, not from what the card reports."));
  // Shown regardless of the checkbox. What the source is remains worth knowing
  // when the automatic handling is switched off -- that is exactly the situation
  // in which somebody wants to check whether it was right.
  if (detectedInterlace_) {
    ImGui::TextDisabled(T("Gemessen: %s", "Measured: %s"),
                        *detectedInterlace_ ? *detectedInterlace_
                                            : T("wird gemessen", "measuring"));
  }
  // Direkt unter der Messung und direkt ueber dem Schalter, mit dem man sie
  // uebergeht: das ist die Reihenfolge, in der jemand die Sache liest und
  // erledigt. Warum die Messung hier irren kann, steht bei
  // App::InterlaceVerdictDoubtful -- kurz: ein Bild von Kammlinien ist von
  // Kammlinien nicht zu unterscheiden.
  if (interlaceDoubtful_) {
    TextWarningWrapped(
        T("Ungewöhnlich bei dieser Auflösung. Feine Linienmuster im Bild können die Erkennung "
          "täuschen. Sieht das Bild falsch aus, Deinterlacing hier abschalten.",
          "Unusual at this resolution. Fine line patterns in the picture can fool the "
          "detection. If the picture looks wrong, turn deinterlacing off here."));
  }

  int deint = (int)img.deinterlace;
  ImGui::SetNextItemWidth(-260.0f);
  if (ComboEnum(T("Deinterlacing", "Deinterlacing"), &deint, kDeinterlaceCount, DeinterlaceName,
                DeinterlaceHelp)) {
    img.deinterlace = (Deinterlace)deint;
  }
  Anchor("deinterlace");
  ImGui::SameLine();
  HelpMarker(DeinterlaceHelp(deint));
  if (coSitedFields_) {
    TextDisabledWrapped(T("Diese Quelle braucht keine Rekonstruktion: alle Modi liefern "
                          "dasselbe Bild.",
                          "This source needs no reconstruction: every mode gives the same "
                          "picture."));
  }

  int order = (int)img.fieldOrder;
  ImGui::SetNextItemWidth(-260.0f);
  if (ComboEnum(T("Halbbildreihenfolge", "Field order"), &order, 3, FieldOrderName)) {
    img.fieldOrder = (FieldOrder)order;
  }
  Anchor("fieldorder");
  ImGui::SameLine();
  HelpMarker(T("Springt das Bild auf und ab, die andere Reihenfolge wählen.",
               "If the picture jumps up and down, pick the other order."));
  }  // Halbbilder, nur wenn die Quelle welche hat

  ImGui::Spacing();
  ImGui::SeparatorText(T("Bildrand abschneiden", "Crop"));
  Anchor("crop");
  ImGui::TextDisabled(T("Pixel, die vom Quellbild wegfallen.",
                        "Pixels removed from the source picture."));
  const float quarter =
      (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 3) / 4.0f;
  ImGui::SetNextItemWidth(quarter);
  ImGui::DragInt("##cropl", &img.cropLeft, 0.5f, 0, 2048, T("links %d", "left %d"));
  ResetOnRightClick(img.cropLeft, kImage.cropLeft);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(quarter);
  ImGui::DragInt("##cropr", &img.cropRight, 0.5f, 0, 2048, T("rechts %d", "right %d"));
  ResetOnRightClick(img.cropRight, kImage.cropRight);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(quarter);
  ImGui::DragInt("##cropt", &img.cropTop, 0.5f, 0, 2048, T("oben %d", "top %d"));
  ResetOnRightClick(img.cropTop, kImage.cropTop);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(quarter);
  ImGui::DragInt("##cropb", &img.cropBottom, 0.5f, 0, 2048, T("unten %d", "bottom %d"));
  ResetOnRightClick(img.cropBottom, kImage.cropBottom);
  // The crop is measured on the source, before the picture is turned. Dragging
  // an edge on a rotated image would move a different edge than the one under
  // the cursor, so that combination is simply not offered.
  const bool rotated = img.rotation != Rotation::None;
  ImGui::BeginDisabled(rotated);
  if (ImGui::Button(T("Konfigurieren ...", "Configure ..."))) {
    cropPickRequested_ = true;
  }
  ImGui::EndDisabled();
  ImGui::SetItemTooltip(
      rotated ? T("Bei gedrehtem Bild nicht verfügbar.", "Not available on a rotated picture.")
              : T("Schließt die Einstellungen und lässt die Ränder im Bild ziehen.",
                  "Closes the settings and lets you drag the edges on the picture."));
  ImGui::SameLine();
  ImGui::BeginDisabled(!captureRunning_);
  if (ImGui::Button(T("Erkennen", "Detect"))) cropDetectRequested_ = true;
  Anchor("cropdetect");
  ImGui::EndDisabled();
  ImGui::SetItemTooltip(T("Misst den schwarzen Rand aus und schneidet ihn weg.",
                          "Measures the black border and crops it away."));
  ImGui::SameLine();
  if (ImGui::Button(T("Zurücksetzen##crop", "Reset##crop"))) {
    img.cropLeft = img.cropRight = img.cropTop = img.cropBottom = 0;
  }

  ImGui::Checkbox(T("Je Bildgröße getrennt merken", "Remember per picture size"),
                  &img.cropPerFormat);
  Anchor("cropperres");
  ImGui::SameLine();
  HelpMarker(T("Eine Quelle kann zwei Bildgrößen liefern, etwa ein PAL-GameCube 576 Zeilen "
               "und im 60-Hz-Modus 480, jede mit eigenem Rand. Angehakt behält jede Größe "
               "ihren Zuschnitt, statt beim Wechsel zurückgesetzt zu werden.",
               "One source can deliver two picture sizes, e.g. a PAL GameCube 576 lines and "
               "480 in 60 Hz mode, each with its own border. Ticked, each size keeps its crop "
               "instead of being reset on a change."));
  if (img.cropPerFormat && !img.cropVariants.empty()) {
    std::string sizes;
    for (const CropForFormat& v : img.cropVariants) {
      if (!sizes.empty()) sizes += ", ";
      sizes += std::to_string(v.width) + "x" + std::to_string(v.height);
    }
    ImGui::TextDisabled(T("Gespeichert für: %s", "Stored for: %s"), sizes.c_str());
  }

  // Natives Raster. Steht beim Zuschnitt, weil beide in Pixeln der Quelle
  // rechnen. Und es gehört nicht zur Bildröhre: es macht das Bild sauberer,
  // nicht nostalgischer.
  //
  // Und es ist rein analog. Es rechnet zurück, was das Abtasten einer analogen
  // Zeile mit fester Rate angerichtet hat -- ein digitaler Eingang überträgt
  // die Bildpunkte bereits einzeln, da gibt es kein Raster wiederzufinden.
  //
  // Die Zeilenzahl steht daneben, weil "analog" allein nicht genügt. Hängt ein
  // Dongle davor, das ein 480i-Signal auf 1080 hochrechnet, ist die Quelle
  // weiterhin analog, aber die 720 Proben je Zeile, auf die diese Zahl
  // zurückrechnet, sind längst durch einen Skalierer gegangen. Die Zuordnung
  // greift dann ins Leere und kostet genau das Detail, das sie retten soll --
  // gemeldet in Ausgabe 1.
  if (analogueSource_ && (sourceHeight_ == 0 || sourceHeight_ <= kStandardLines)) {
  ImGui::Spacing();
  ImGui::SeparatorText(T("Natives Pixelraster", "Native pixel grid"));
  Anchor("native");

  // Die Konsole steht in der Liste selbst, nicht nur im Tooltip: wer hier
  // vorbeikommt, weiß was er angeschlossen hat und sucht die Zahl dazu -- nicht
  // umgekehrt.
  //
  // Der erste Eintrag hiess bis 3.6.2 "aus", und das stimmt seit dem
  // Seitenverhaeltnis "Quadratische Pixel" nicht mehr: die Zahl schaltet dort
  // nichts ab, sie fehlt nur, und das Bild bekommt ersatzweise die Proben der
  // Karte. "Keine Angabe" sagt beides auf einmal.
  static const int kNativePresets[] = {0, 256, 320, 384, 512, 640};
  const char* kNativeWho[] = {
      T("keine Angabe", "not specified"),
      "256  ·  SNES, NES, PS1",
      T("320  ·  Mega Drive, PS1", "320  ·  Mega Drive, PS1"),
      T("384  ·  Amiga, PS1 breit", "384  ·  Amiga, PS1 wide"),
      T("512  ·  SNES hochauflösend", "512  ·  SNES hi-res"),
      T("640  ·  GameCube, PS2, Dreamcast", "640  ·  GameCube, PS2, Dreamcast"),
  };
  int nativeIdx = 0;
  for (int k = 0; k < 6; ++k) {
    if (img.nativeWidth == kNativePresets[k]) nativeIdx = k;
  }
  ImGui::SetNextItemWidth(-260.0f);
  if (ImGui::BeginCombo(T("Breite der Quelle", "Source width"), kNativeWho[nativeIdx])) {
    for (int k = 0; k < 6; ++k) {
      const bool chosen = nativeIdx == k;
      if (ImGui::Selectable(kNativeWho[k], chosen)) img.nativeWidth = kNativePresets[k];
      if (chosen) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  Anchor("nativewidth");
  ImGui::SameLine();
  HelpMarker(T("Die echte waagerechte Auflösung der Konsole. Die Karte tastet mit fester Rate "
               "ab, meist 720, ein SNES-Pixel liegt also auf etwa 2,8 Proben. Mit der echten "
               "Breite fällt jedes Ausgabepixel auf genau ein Konsolenpixel statt zwischen "
               "zwei.\n\n"
               "Ersetzt den Filter oben. Am besten mit Integer-Skalierung.\n\n"
               "Das Seitenverhältnis „Quadratische Pixel“ nutzt denselben Wert für die "
               "Bildform.",
               "The console's real horizontal resolution. The card samples at a fixed rate, "
               "usually 720, so one SNES pixel spans about 2.8 samples. With the real width, "
               "each output pixel lands on exactly one console pixel instead of between "
               "two.\n\n"
               "Replaces the filter above. Best with integer scaling.\n\n"
               "The \"Square pixels\" aspect ratio uses the same value for the picture "
               "shape."));

  // Die Zahl hat zwei Aufgaben, und nicht jeder will beide: wer nur die Form
  // fuer "Quadratische Pixel" braucht, will die Kanten nicht mitgezogen haben.
  // Ausgegraut statt ausgeblendet, damit darunter nichts springt.
  ImGui::BeginDisabled(img.nativeWidth == 0);
  ImGui::Checkbox(T("Kanten aufs Raster einrasten", "Snap edges to the grid"), &img.nativeSnap);
  Anchor("nativesnap");
  ImGui::EndDisabled();
  ImGui::SameLine();
  HelpMarker(T("Aus: die Breite gilt nur für die Form bei „Quadratische Pixel“, und das Bild "
               "geht durch den Filter oben wie ohne Angabe.",
               "Off: the width only sets the shape for \"Square pixels\", and the picture "
               "goes through the filter above as if none were given."));

  }  // natives Pixelraster, nur analog und nur im Standardraster

  if (analogueSource_) {
    // Vier der sechs Regler hier gibt es nur, weil Composite Helligkeit und
    // Farbe durch eine Leitung schickt. Auf getrennten Leitungen haben sie
    // nichts zu tun -- nicht "wenig", sondern nichts: das Muster, das sie
    // wegrechnen, entsteht beim Mischen, und es wird nicht gemischt. Sichtbar
    // blieben sie ein Angebot, Schaerfe gegen nichts zu tauschen.
    //
    // Was uebrig bleibt, sind die beiden Entrauscher. Rauschen hat jede analoge
    // Leitung, unabhaengig davon, wie viele es sind.
    //
    // Und dieselbe Trennung noch einmal in App::EffectiveImage, wo die Werte
    // wirklich abgeschaltet werden -- ausblenden allein genuegt nicht, ein
    // gespeicherter Wert wirkte sonst weiter, wo es keinen Regler mehr gibt.
    const bool composite = CompositeSource();

    ImGui::Spacing();
    ImGui::SeparatorText(composite ? T("Composite-Filter", "Composite filter")
                                   : T("Rauschen", "Noise"));
    Anchor("composite");
    if (composite) {
      TextDisabledWrapped(T("Gegen das, was Composite immer mitbringt: falsche Farbe, "
                            "Punktkriechen, Rauschen und einen weichen Bildrand.",
                            "Against what composite always brings with it: false colour, "
                            "dot crawl, noise, and a soft top end."));
    } else {
      TextDisabledWrapped(
          Format(T("%s führt Helligkeit und Farbe getrennt: Punktkriechen, Regenbogenmuster "
                   "und weiche Kanten entstehen nicht, die Filter dagegen entfallen. Es "
                   "bleibt nur Rauschen, wie bei jeder analogen Verbindung.",
                   "%s carries brightness and colour separately, so dot crawl, rainbow "
                   "patterns and soft edges do not occur and their filters are hidden. Only "
                   "noise remains, as on any analogue connection."),
                 AnalogConnectorName((int)connector_))
              .c_str());
    }

    if (composite) {
      ImGui::SetNextItemWidth(-260.0f);
      ImGui::SliderInt(T("Farbschimmern", "Colour shimmer"), &img.chromaSoft, 0, 8,
                       img.chromaSoft == 0 ? T("aus", "off") : "%d");
      Anchor("chromasoft");
      ResetOnRightClick(img.chromaSoft, kImage.chromaSoft);
      ImGui::SameLine();
      HelpMarker(T("Regenbogenmuster über feinen Strukturen. Zeichnet die Farbe waagerecht "
                   "weich; die Schärfe bleibt, da Composite ohnehin keine feinen Farbdetails "
                   "überträgt.",
                   "Rainbow patterns over fine detail. Blurs colour horizontally; sharpness "
                   "is kept, as composite carries no fine colour detail anyway."));

      // Kein zweiter Filter, sondern eine Bedingung auf den darüber: derselbe
      // Weichzeichner, nur nicht mehr überall. Deshalb eingerückt und deshalb
      // gesperrt, solange der Regler auf null steht -- ohne ihn gibt es nichts
      // zu bedingen.
      ImGui::BeginDisabled(img.chromaSoft == 0);
      ImGui::Indent();
      bool adaptive = img.adaptiveChroma;
      if (ImGui::Checkbox(T("Nur wo nötig", "Only where needed"), &adaptive)) {
        img.adaptiveChroma = adaptive;
      }
      Anchor("chromaadaptive");
      ImGui::Unindent();
      ImGui::EndDisabled();
      ImGui::SameLine();
      HelpMarker(T("Zeichnet die Farbe nur dort weich, wo feine Helligkeitsdetails "
                   "Falschfarben erzeugen können, etwa bei Dithering, feinen Rastern und "
                   "Mauerwerk. Aus: Das ganze Bild wird weichgezeichnet.",
                   "Softens colour only where fine brightness detail can produce false "
                   "colour, e.g. dithering, fine patterns and brickwork. Off: the whole "
                   "picture is softened."));
    }

    // Vor dem Mitteln, weil es vor ihm laeuft: das Mitteln und alles danach
    // lesen die Bilder, aus denen dieser Filter das Kriechen schon genommen
    // hat. Nur auf Composite -- ohne gemeinsame Leitung gibt es nichts zu
    // entfernen, siehe EffectiveImage.
    if (composite) {
      bool extended = img.extendedHistory;
      if (ImGui::Checkbox(T("Erweiterte Historie", "Extended history"), &extended)) {
        img.extendedHistory = extended;
      }
      Anchor("extendedhistory");
      ImGui::SameLine();
      HelpMarker(T("Nutzt die letzten zwölf Bilder, um Punktkriechen vor den Filtern darunter "
                   "zu entfernen, auch bei Bewegung und Überblendungen. Wirkt, wo sich das "
                   "Kriechen alle zwei oder vier Bilder wiederholt: PAL, PAL 60, PAL-M, "
                   "PAL-N, NTSC. Kostet GPU-Zeit, keine Verzögerung.",
                   "Uses the last twelve frames to remove dot crawl before the filters below, "
                   "including in motion and fades. Works where the crawl repeats every two or "
                   "four frames: PAL, PAL 60, PAL-M, PAL-N, NTSC. Costs GPU time, adds no "
                   "latency."));
    }

    // Two controls, because they are two different bargains and pretending
    // otherwise hid the more useful one.
    //
    // Averaging over four frames removes the pattern completely wherever the
    // picture is standing still, and costs nothing at all. Working the carrier
    // back out of the brightness is the only thing that helps where something is
    // moving, and it costs sharpness. Behind one slider the first was switched
    // on by the very first step and everything after it was the second getting
    // gradually softer -- which is exactly what it felt like to use, and gave no
    // way to have the free half on its own.
    //
    // They are not independent, though, which is why the checkbox locks: with
    // the averaging off, the demodulator runs over still parts of the picture
    // too, paying sharpness for something the averaging does for free.
    //
    // `composite` davor, weil der Regler unten dann gar nicht dasteht: ein
    // gespeicherter Wert aus einem Composite-Profil wuerde die Mittelung sonst
    // an einem Component-Eingang festhalten, mit dem Verweis auf einen Regler,
    // den dort niemand sieht. Abgeschaltet ist er ohnehin, siehe EffectiveImage.
    const bool demodOn = composite && img.dotNotch > 0.0f;
    if (demodOn) img.temporalDenoise = 1.0f;

    bool average = img.temporalDenoise > 0.0f;
    ImGui::BeginDisabled(demodOn);
    if (ImGui::Checkbox(T("Stillstehendes mitteln", "Average what stands still"), &average)) {
      img.temporalDenoise = average ? 1.0f : 0.0f;
    }
    Anchor("average");
    ImGui::EndDisabled();
    ImGui::SameLine();
    // Derselbe Filter, zwei Beschreibungen, und das ist keine Schoenfaerberei.
    // Er mittelt einen Umlauf des Farbtraegers (je Quelle gemessen, siehe
    // VideoRenderer::crawlCycle); auf Composite loescht das den Traeger, und
    // nebenbei das Rauschen.
    // Ohne Traeger bleibt die zweite Haelfte uebrig, und nur die. Ein Hilfetext,
    // der hier weiter von Punktkriechen redet, beschriebe einen Fehler, den
    // dieser Eingang nicht hat -- und genau daran sah die Liste aus, als sei der
    // Dot-Crawl-Filter bloss halb verschwunden.
    if (!composite) {
      HelpMarker(T("Mittelt über vier Bilder und entfernt das Rauschen in ruhenden Bereichen "
                   "vollständig, ohne Schärfe zu kosten. Bewegte Bereiche bleiben "
                   "ausgenommen; dafür sind die Optionen darunter.",
                   "Averages four frames and removes noise completely in still areas, without "
                   "losing sharpness. Moving areas are left out; the options below handle "
                   "them."));
    } else {
      HelpMarker(T("Mittelt über einen vollen Umlauf des Farbträgers und entfernt "
                   "Punktkriechen in ruhenden Bereichen vollständig, ohne Schärfe zu kosten. "
                   "Der Umlauf wird je Quelle gemessen: vier Bilder bei PAL, zwei bei NTSC "
                   "nach Norm, drei bei Konsolen mit eigenem Takt.\n\n"
                   "Immer an, solange der Regler darunter über null steht, damit der "
                   "Demodulator in ruhenden Bereichen keine Schärfe kostet.",
                   "Averages over one full cycle of the colour subcarrier and removes dot "
                   "crawl completely in still areas, without losing sharpness. The cycle is "
                   "measured per source: four frames for PAL, two for standard NTSC, three on "
                   "consoles with their own clock.\n\n"
                   "Always on while the slider below is above zero, so the demodulator costs "
                   "no sharpness in still areas."));
    }

    // Derselbe Filter an einem anderen Arbeitspunkt, nicht ein zweiter Filter.
    // Mitteln ueber Bewegung ist Schmieren -- das laesst sich nicht wegrechnen,
    // nur verschieben, und wohin es verschoben wird, weiss die Quelle besser
    // als der Code.
    ImGui::BeginDisabled(!average);
    ImGui::Indent();
    bool quick = img.avoidGhosting;
    if (ImGui::Checkbox(T("Ghosting vermeiden", "Avoid ghosting"), &quick)) {
      img.avoidGhosting = quick;
    }
    Anchor("ghosting");
    ImGui::Unindent();
    ImGui::EndDisabled();
    ImGui::SameLine();
    // Derselbe Tausch, nur ohne den Grund, spaet loszulassen. Auf Composite
    // haelt das Gatter lange fest, weil Punktkriechen sich selbst bewegt und
    // ein empfindliches Gatter den Filter genau dort abschaltet, wo das
    // Artefakt sitzt. Rauschen bewegt sich nicht, es steht ueberall, und die
    // Frage schrumpft auf die eine Haelfte, die uebrig bleibt.
    if (!composite) {
      HelpMarker(T("Wo die Mittelung bei Bewegung aufhört. Mitteln über Bewegung verschmiert; "
                   "der Haken verschiebt nur den Kompromiss.\n\n"
                   "Aus: Hält länger fest und entrauscht auch langsame Bewegung, zieht aber "
                   "Fahnen hinter bewegten Kanten.\n\n"
                   "An: Lässt ab vier von 255 Stufen Unterschied los. Bewegte Kanten bleiben "
                   "sauber, behalten aber ihr Rauschen; das übernimmt die Option darunter.",
                   "Where averaging stops in motion. Averaging moving areas smears them; this "
                   "only shifts the trade-off.\n\n"
                   "Off: holds longer and also denoises slow motion, but leaves trails behind "
                   "moving edges.\n\n"
                   "On: lets go from a difference of four levels out of 255. Moving edges "
                   "stay clean but keep their noise; the option below handles that."));
    } else {
      HelpMarker(T("Wo die Mittelung bei Bewegung aufhört. Mitteln über Bewegung verschmiert; "
                   "der Haken verschiebt nur den Kompromiss.\n\n"
                   "Aus: Hält länger fest. Punktkriechen bewegt sich selbst, ein "
                   "empfindliches Gatter schaltete den Filter also genau dort ab. Preis: "
                   "Fahnen hinter langsam bewegten Kanten.\n\n"
                   "An: Lässt ab vier von 255 Stufen Unterschied los. Bewegte Kanten bleiben "
                   "sauber, langsame Bereiche behalten etwas Kriechen; das übernimmt der "
                   "Regler darunter.",
                   "Where averaging stops in motion. Averaging moving areas smears them; this "
                   "only shifts the trade-off.\n\n"
                   "Off: holds longer. Dot crawl moves by itself, so a sensitive gate would "
                   "switch the filter off exactly there. The cost is trails behind slowly "
                   "moving edges.\n\n"
                   "On: lets go from a difference of four levels out of 255. Moving edges "
                   "stay clean, slow areas keep some crawl; the slider below handles that."));
    }

    // Die andere Hälfte des Bildes. Der Haken darüber entscheidet, *wo* die
    // Mittelung loslässt; dieser hier nimmt auf, was sie fallen lässt.
    //
    // Bewusst kein Unterpunkt der Mittelung, obwohl er dieselben drei Bilder
    // liest: er läuft auch ohne sie, weil er etwas anderes tut. Sie löscht das
    // Kriechen und kann nur stillstehen; er entfernt Rauschen und kann nur
    // laufen.
    bool follow = img.motionCompensate;
    if (ImGui::Checkbox(T("Bewegung folgen", "Follow the movement"), &follow)) {
      img.motionCompensate = follow;
    }
    Anchor("follow");
    ImGui::SameLine();
    // Ohne Traeger faellt die halbe Erklaerung weg: kein Kriechen, an dem er
    // nichts aendert, und kein Regler darunter, dem er zuarbeitet. Was bleibt,
    // ist genau das, wofuer er gebaut wurde.
    if (!composite) {
      HelpMarker(T("Verfolgt jeden Punkt über die letzten drei Bilder und mittelt entlang "
                   "seiner Bewegung; so verschwindet Rauschen auch in bewegten Bereichen.\n\n"
                   "Bilder, die nicht zur Bewegung passen, bleiben außen vor, daher ziehen "
                   "drehende oder anhaltende Objekte keine Spur.\n\n"
                   "Kostet GPU-Zeit; auf diesem Eingang der einzige Filter mit nennenswerten "
                   "Kosten.",
                   "Tracks each point over the last three frames and averages along its "
                   "motion, so noise is also removed in moving areas.\n\n"
                   "Frames that do not match the motion are left out, so objects that turn or "
                   "stop leave no trail.\n\n"
                   "Costs GPU time; the only filter on this input with a notable cost."));
    } else {
      HelpMarker(T("Verfolgt jeden Punkt über die letzten drei Bilder und mittelt entlang "
                   "seiner Bewegung; so verschwindet Rauschen auch in bewegten Bereichen.\n\n"
                   "Gegen Punktkriechen hilft es nicht, das Muster hängt am Raster, nicht am "
                   "Bild. Es hilft aber dem Regler darunter, dessen Schätzung unter Rauschen "
                   "leidet.\n\n"
                   "Bilder, die nicht zur Bewegung passen, bleiben außen vor, daher ziehen "
                   "drehende oder anhaltende Objekte keine Spur.\n\n"
                   "Kostet GPU-Zeit, etwa so viel wie der Regler darunter.",
                   "Tracks each point over the last three frames and averages along its "
                   "motion, so noise is also removed in moving areas.\n\n"
                   "Does not remove dot crawl, which is tied to the raster, not the picture. "
                   "It helps the slider below, whose estimate suffers from noise.\n\n"
                   "Frames that do not match the motion are left out, so objects that turn or "
                   "stop leave no trail.\n\n"
                   "Costs GPU time, about as much as the slider below."));
    }

    if (composite) {
      // Stufen statt freiem Lauf: die Zwischenwerte sind ohne Wirkung, und ein
      // Regler, der sich bewegt ohne etwas zu ändern, behauptet etwas Falsches.
      DotCrawlStep steps[16];
      const int stepCount = BuildDotCrawlSteps(carrierPeriod_, steps, 16);
      int stepIndex = 0;  // 0 heißt aus
      if (img.dotNotch > 0.0f) {
        const int nowWindow = DotCrawlWindow(img.dotNotch, carrierPeriod_);
        stepIndex = 1;
        for (int k = 0; k < stepCount; ++k) {
          if (steps[k].window == nowWindow) stepIndex = k + 1;
        }
      }

      char label[64];
      if (stepIndex == 0) {
        snprintf(label, sizeof(label), "%s", T("aus", "off"));
      } else {
        snprintf(label, sizeof(label), T("Stufe %d von %d", "step %d of %d"), stepIndex, stepCount);
      }

      ImGui::SetNextItemWidth(-260.0f);
      if (ImGui::SliderInt(T("Bewegtes entstören", "Clean up what moves"), &stepIndex, 0, stepCount,
                           label)) {
        img.dotNotch = stepIndex <= 0 ? 0.0f : steps[stepIndex - 1].slider;
      }
      Anchor("cleanmove");
      ResetOnRightClick(img.dotNotch, kImage.dotNotch);
      ImGui::SameLine();
      HelpMarker(T("Entfernt den Farbträger aus der Helligkeit. Der einzige Filter gegen "
                   "Punktkriechen in bewegten Bereichen; kostet Schärfe. Weiter rechts ist "
                   "gründlicher und weicher, die Zeile darunter zeigt die aktuelle Stufe.\n\n"
                   "Braucht die richtige Trägerfrequenz aus der Videonorm im Reiter Quelle. "
                   "Steht sie falsch, sinkt die Wirkung von rund 70 auf 34 Prozent.",
                   "Removes the colour subcarrier from the brightness. The only filter "
                   "against dot crawl in moving areas; costs sharpness. Further right is "
                   "stronger and softer, the line below shows the current level.\n\n"
                   "Needs the right carrier frequency from the video standard on the Source "
                   "tab. Set wrong, the effect drops from about 70 % to 34 %."));

      // What the number amounts to, because a number on its own says nothing and
      // this is a trade the user should be able to see rather than infer.
      if (demodOn) {
        const int window = DotCrawlWindow(img.dotNotch, carrierPeriod_);
        float removed = 0.0f, softer = 0.0f;
        DotCrawlEffect(window, &removed, &softer);
        ImGui::Indent();
        ImGui::TextDisabled(
            T("In Bewegung: %.0f %% weg, dafür %.0f %% weicher (Fenster %d Punkte).",
              "Moving: %.0f %% gone, %.0f %% softer for it (window %d samples)."),
            removed, softer, window);
        ImGui::Unindent();
      }

      // Steht unter den drei Entstörern, weil es die Gegenrichtung ist: die
      // nehmen etwas weg, dieser holt etwas zurück. Und es ist die Reihenfolge,
      // in der der Shader rechnet -- erst das Muster raus, dann das Band hoch,
      // denn andersherum würde die Anhebung das Muster mit anheben.
      ImGui::SetNextItemWidth(-260.0f);
      ImGui::SliderFloat(T("Bandbreite zurückholen", "Restore bandwidth"), &img.bandwidthRestore,
                         0.0f, 1.0f, "%.2f");
      Anchor("bandwidth");
      ResetOnRightClick(img.bandwidthRestore, kImage.bandwidthRestore);
      ImGui::SameLine();
      HelpMarker(T("Composite dämpft Helligkeitsdetails zum Farbträger hin. Der Regler hebt "
                   "dieses Band wieder an: Er stellt Details wieder her, statt Kanten "
                   "nachzuschärfen.\n\n"
                   "Anders als \"Schärfen\" unter Skalierung, das bei Fenstergröße den "
                   "Kantenkontrast erhöht, arbeitet er auf den Bildpunkten der Quelle.\n\n"
                   "Um den Träger herum und wo die Zeile echte Energie auf der Trägerfrequenz "
                   "führt, hält er sich zurück: Dort sind Detail und Kriechen nicht zu "
                   "unterscheiden. Nur waagerecht.",
                   "Composite attenuates brightness detail towards the colour subcarrier. "
                   "This lifts that band back up, restoring detail rather than sharpening "
                   "edges.\n\n"
                   "Unlike \"Sharpen\" under Scaling, which adds edge contrast at window "
                   "size, this works on the source's own samples.\n\n"
                   "It holds back around the carrier and wherever the line carries real "
                   "energy at the carrier frequency, where detail and crawl cannot be told "
                   "apart. Horizontal only."));
    }  // Composite-only
  }

  // Bildröhre. Anzeigeeffekte und keine Signalbearbeitung: sie landen weder in
  // einer Aufnahme noch in einem Screenshot, und anders als der Filter direkt
  // darüber stellen sie nichts wieder her, sondern legen etwas obendrauf.
  //
  // Ohne Bedingung sichtbar, und das ist seit 3.7 so.
  //
  // Vorher verschwand der ganze Abschnitt oberhalb von 576 Zeilen, wortlos. Das
  // hat in Ausgabe 1 einen Nutzer zweimal nachfragen lassen, ob es die Funktion
  // ueberhaupt gibt -- seine Karte lief auf 720p, und damit war sie weg. Die
  // Begruendung dafuer stimmte ausserdem nur halb: die Maske rechnet in
  // Ausgabepixeln und ist von der Quelle voellig unabhaengig, und die
  // Zeilenluecken haengen nicht an 576, sondern daran, ob das Fenster zwei
  // Zeilen je Bildzeile hergibt. Auf einem 4K-Schirm tut es das auch bei 720p.
  //
  // Also stehen lassen und sagen, wenn nichts zu holen ist. Ein Regler, der
  // erklaert, warum er gerade nichts tut, ist besser als einer, der fehlt.
  ImGui::Spacing();
  ImGui::SeparatorText(T("Bildröhre", "Cathode ray tube"));
  Anchor("crt");
  ImGui::TextDisabled(
      "%s", T("Setzt zurück, was ein Röhrenmonitor hinzugefügt hat. Nur für die Anzeige.",
              "Puts back what a CRT added. Display only."));

  // Die Zeilenzahl steht vor den Reglern, weil sie darueber entscheidet, ob sie
  // wirken. Sie ist eine Angabe ueber die Quelle und keine Einstellung am Bild:
  // wer sie braucht, hat eine Karte oder ein Dongle, das mehr Zeilen liefert als
  // die Konsole gezeichnet hat, und nur er selbst weiss, welche Zahl stimmt.
  static const int kLinePresets[] = {0, 240, 288, 480, 576, 720, 1080};
  const char* kLineWho[] = {
      T("automatisch", "automatic"),
      T("240  ·  NTSC 240p — SNES, Mega Drive, PS1, N64",
        "240  ·  NTSC 240p — SNES, Mega Drive, PS1, N64"),
      T("288  ·  PAL 288p — dieselben in 50 Hz", "288  ·  PAL 288p — the same in 50 Hz"),
      T("480  ·  NTSC 480i/p — GameCube, PS2, Dreamcast, Wii",
        "480  ·  NTSC 480i/p — GameCube, PS2, Dreamcast, Wii"),
      T("576  ·  PAL 576i — dieselben in 50 Hz", "576  ·  PAL 576i — the same in 50 Hz"),
      T("720  ·  720p", "720  ·  720p"),
      T("1080  ·  1080i/p", "1080  ·  1080i/p"),
  };
  constexpr int kLineCount = (int)(sizeof(kLinePresets) / sizeof(kLinePresets[0]));
  int lineIdx = 0;
  for (int k = 0; k < kLineCount; ++k) {
    if (img.sourceLines == kLinePresets[k]) lineIdx = k;
  }
  ImGui::SetNextItemWidth(-260.0f);
  if (ImGui::BeginCombo(T("Zeilen der Quelle", "Source lines"), kLineWho[lineIdx])) {
    for (int k = 0; k < kLineCount; ++k) {
      const bool chosen = lineIdx == k;
      if (ImGui::Selectable(kLineWho[k], chosen)) img.sourceLines = kLinePresets[k];
      if (chosen) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  Anchor("crtlines");
  ImGui::SameLine();
  HelpMarker(Format(T("Wie viele Zeilen die Konsole wirklich zeichnet. Nötig, wenn mehr "
               "ankommen, etwa von einer Karte ab 720p oder einem Dongle mit eigenem "
               "Skalierer: 1080 Zeilen von einer Konsole, die 480 gezeichnet hat. Ohne die "
               "Angabe nimmt %s die empfangene Zeilenzahl, und die Lücken liegen falsch.\n\n"
               "PAL: 576 oder 288, NTSC: 480 oder 240. PAL 60 nutzt das NTSC-Raster.\n\n"
               "Automatisch nimmt, was die Karte meldet; das stimmt, wenn sie die echte "
               "Auflösung liefert.\n\n"
               "Nur senkrecht; waagerecht ist es „Breite der Quelle“ beim Zuschnitt.",
               "The number of lines the console really draws. Needed when more arrive, e.g. "
               "from a card that starts at 720p or a dongle with its own scaler: 1080 lines "
               "from a console that drew 480. Without it %s uses the received line count and "
               "the gaps land in the wrong place.\n\n"
               "PAL: 576 or 288, NTSC: 480 or 240. PAL 60 uses the NTSC raster.\n\n"
               "Automatic uses what the card reports, which is correct when it delivers the "
               "real resolution.\n\n"
               "Vertical only; horizontally it is \"Source width\" in the crop section."),
                    AppNameUtf8().c_str())
                 .c_str());

  ImGui::SetNextItemWidth(-260.0f);
  ImGui::SliderFloat(T("Zeilenlücken", "Scanlines"), &img.scanlines, 0.0f, 0.5f, "%.2f");
  Anchor("scanlines");
  ResetOnRightClick(img.scanlines, kImage.scanlines);
  ImGui::SameLine();
  HelpMarker(T("Dunkelt die Lücken zwischen den Zeilen der Quelle ab. Unter doppelter "
               "Quellhöhe im Fenster aus, um Moiré zu vermeiden. Die Helligkeit wird "
               "ausgeglichen.",
               "Darkens the gaps between the source's lines. Off below twice the source "
               "height in the window, to avoid moiré. Brightness is compensated."));

  // Der Satz, der frueher gefehlt hat. Er steht auch bei Reglerstellung null da:
  // wer sich fragt, ob es sich lohnt, den Schieber anzufassen, bekommt die
  // Antwort, bevor er es tut.
  if (img.rotation == Rotation::Cw90 || img.rotation == Rotation::Ccw90) {
    ImGui::TextDisabled("%s", T("Gedreht — Zeilenlücken bleiben aus, sie lägen quer.",
                                "Rotated — scanlines stay off, they would run sideways."));
  } else if (scanlineRoom_ > 0.0f && scanlineRoom_ < 2.0f) {
    TextDisabledWrapped(
        Format(T("Zu wenig Platz: %.1f Fensterzeilen je Bildzeile, nötig sind 2. Fenster "
                 "vergrößern oder die Zeilenzahl oben prüfen.",
                 "Not enough room: %.1f window rows per picture line, 2 are needed. Enlarge "
                 "the window or check the line count above."),
               scanlineRoom_)
            .c_str());
  }

  int mask = Clamp(img.mask, 0, 2);
  ImGui::SetNextItemWidth(-260.0f);
  if (ComboEnum(T("Maske", "Mask"), &mask, 3, MaskName, MaskHelp)) img.mask = mask;
  Anchor("mask");
  ImGui::SameLine();
  HelpMarker(MaskHelp(mask));

  if (img.mask != 0) {
    ImGui::SetNextItemWidth(-260.0f);
    ImGui::SliderFloat(T("Maskenstärke", "Mask strength"), &img.maskStrength, 0.0f, 0.5f, "%.2f");
    Anchor("maskstrength");
    ResetOnRightClick(img.maskStrength, kImage.maskStrength);
    ImGui::SameLine();
    HelpMarker(T("Braucht eine hohe Ausgabeauflösung, um als Maske statt als Farbstich zu "
                 "wirken.",
                 "Needs a high output resolution to read as a mask rather than as a tint."));
  }

  // Der A/B-Vergleich steht hinter allem, was er vergleicht, und vor der
  // Farbe, die er nicht anfasst. Links fehlt dasselbe wie unter "Alle Filter
  // aus": Composite-Filter, Schaerfen, Bildregler, natives Raster, Bildroehre.
  // Deinterlacing, Zuschnitt und Farbe laufen auf beiden Seiten -- ohne sie
  // waere die linke Haelfte nicht ungefiltert, sondern falsch.
  //
  // Stand bis 4.4 am Ende des Composite-Abschnitts und verglich nur ihn. Damit
  // war er an jedem digitalen Eingang verschwunden.
  ImGui::Spacing();
  ImGui::SeparatorText(T("Vergleich", "Compare"));
  bool compare = compareOn_;
  if (ImGui::Checkbox(T("Mit und ohne Filter vergleichen", "Compare with and without filters"),
                      &compare)) {
    compareToggleRequested_ = true;
  }
  Anchor("compare");
  ImGui::SameLine();
  HelpMarker(T("Teilt das Bild: links (bei waagerechter Linie oben) ohne Composite-Filter, "
               "Schärfen, Bildregler, natives Raster und Bildröhre, rechts (unten) mit. "
               "Deinterlacing, Zuschnitt und Farbe gelten für beide Seiten.\n\n"
               "Die Trennlinie lässt sich im Bild mit der Maus ziehen. Sie liegt im Raster "
               "der Quelle und dreht sich mit dem Bild.\n\n"
               "Nicht während einer Aufnahme verfügbar und beim Aufnahmestart abgeschaltet, "
               "damit Aufnahme und virtuelle Kamera das voll gefilterte Bild bekommen. Wird "
               "nicht gespeichert.",
               "Splits the picture: left (top with a horizontal divider) without composite "
               "filters, sharpening, picture controls, native pixel grid and CRT effects, "
               "right (bottom) with them. Deinterlacing, crop and colour apply to both "
               "sides.\n\n"
               "Drag the divider in the picture with the mouse. It sits in the source's grid "
               "and rotates with the picture.\n\n"
               "Unavailable while recording and switched off when a recording starts, so "
               "recordings and the virtual camera get the fully filtered picture. Not "
               "saved."));

  ImGui::BeginDisabled(!compareOn_);
  ImGui::Indent();
  const char* axisNames[] = {
      T("Senkrecht — links ungefiltert", "Vertical — unfiltered on the left"),
      T("Waagerecht — oben ungefiltert", "Horizontal — unfiltered on top")};
  int axis = img.compareHorizontal ? 1 : 0;
  ImGui::SetNextItemWidth(-260.0f);
  if (ImGui::Combo(T("Richtung", "Direction"), &axis, axisNames, 2)) {
    img.compareHorizontal = axis == 1;
  }
  Anchor("comparedir");
  float split = img.compareSplit * 100.0f;
  ImGui::SetNextItemWidth(-260.0f);
  if (ImGui::SliderFloat(T("Trennlinie", "Divider"), &split, 0.0f, 100.0f, "%.0f %%")) {
    img.compareSplit = Clamp(split / 100.0f, 0.0f, 1.0f);
  }
  Anchor("comparesplit");
  ResetOnRightClick(img.compareSplit, kImage.compareSplit);
  ImGui::Unindent();
  ImGui::EndDisabled();

  ImGui::Spacing();
  ImGui::SeparatorText(T("Farbe", "Colour"));
  int range = (int)img.range;
  ImGui::SetNextItemWidth(-260.0f);
  if (ComboEnum(T("Wertebereich", "Range"), &range, 3, ColorRangeName)) {
    img.range = (ColorRange)range;
  }
  Anchor("range");
  ImGui::SameLine();
  HelpMarker(T("Falsch gewählt: Schwarz wirkt grau, oder Zeichnung geht verloren. "
               "Automatisch misst den Wertebereich am Bild.",
               "Set wrong: black looks grey, or detail is lost. Automatic measures the range "
               "from the picture."));

  int matrix = (int)img.matrix;
  ImGui::SetNextItemWidth(-260.0f);
  if (ComboEnum(T("Farbmatrix", "Colour matrix"), &matrix, 3, ColorMatrixName)) {
    img.matrix = (ColorMatrix)matrix;
  }
  Anchor("matrix");
  ImGui::SameLine();
  HelpMarker(T("BT.601 für SD, BT.709 für HD. Falsch gewählt kippen Hauttöne.",
               "BT.601 for SD, BT.709 for HD. Set wrong, skin tones shift."));
  if (img.range == ColorRange::Auto) {
    if (detectedRange_ && *detectedRange_) {
      ImGui::TextDisabled(T("Gemessen: %s", "Measured: %s"), *detectedRange_);
    } else {
      ImGui::TextDisabled(T("Gemessen: läuft noch", "Measured: still running"));
    }
    // Der Knopf daneben und nicht anderswo: wer im Treiber etwas umstellt,
    // kommt hierher, um nachzusehen, was es gebracht hat -- und das Urteil
    // steht bis zum naechsten Formatwechsel, den eine Treiberoption nicht
    // ausloest. Ohne den Knopf zeigt die Zeile vier Sekunden alte Zahlen und
    // laesst sich nicht dazu bewegen, neue zu holen.
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Neu messen", "Measure again"))) rangeRemeasureRequested_ = true;
    ImGui::SameLine();
    HelpMarker(T("Die Messung gilt, bis sich das Bildformat ändert. Nach einer Änderung im "
                 "Treiber der Karte hier neu messen.",
                 "The measurement holds until the picture format changes. After changing the "
                 "card's driver settings, measure again here."));
    if (rangeNumbers_ && !rangeNumbers_->empty()) {
      ImGui::TextDisabled("%s", rangeNumbers_->c_str());
    }
  }
  // Ausserhalb des Automatik-Zweigs, weil der Hinweis nichts ueber qBlanks
  // Einstellung sagt, sondern ueber das, was ankommt: er gilt genauso, wenn der
  // Wertebereich hier von Hand steht. Und in einer Zeile plus Fragezeichen,
  // nicht als Absatz: an einer analogen Karte, die streckt, steht er dauerhaft
  // da, und was dauerhaft dasteht, darf nicht schreien.
  if (analogueFullRange_) {
    TextDisabledWrapped(T("Analoger Eingang, trotzdem voller Bereich — Schwarz liegt nicht auf 16.",
                          "Analogue input, yet full range — black is not sitting on 16."));
    ImGui::SameLine();
    HelpMarker(Format(
        T("Composite, S-Video und Tuner nutzen BT.601 mit Schwarz auf 16. Liegt Schwarz auf "
          "0, hat etwas davor den Bereich gestreckt, meist die Karte selbst.\n\n"
          "%s misst den Bereich und stellt sich darauf ein. Was beim Strecken abgeschnitten "
          "wurde, ist verloren, bevor %s das Bild sieht, auch in Aufnahmen. Ob etwas "
          "abgeschnitten wurde, lässt sich nicht feststellen.\n\n"
          "Ändern lässt sich der Bereich nur im Treiber der Karte: Reiter Quelle, „Karte "
          "konfigurieren …“, meist „Colour Range“ oder „Video Range“. Bei manchen Karten "
          "bezeichnet das den erwarteten Eingangsbereich, und „Full“ reicht das Signal "
          "unverändert durch.",
          "Composite, S-Video and tuner inputs use BT.601 with black at 16. Black at 0 means "
          "something upstream stretched the range, usually the card itself.\n\n"
          "%s measures the range and adapts. Anything clipped by the stretch is lost before "
          "%s sees the frame, also in recordings. Whether anything was clipped cannot be "
          "detected.\n\n"
          "Only the card's driver can change the range: Source tab, \"Configure card ...\", "
          "usually \"Colour Range\" or \"Video Range\". On some cards this names the expected "
          "input range, and \"Full\" passes the signal through unchanged."),
        AppNameUtf8().c_str(), AppNameUtf8().c_str())
                   .c_str());
  }
}

// ----------------------------------------------------------------- audio tab

void SettingsWindow::DrawAudioTab() {
  AudioSettings& audio = cfg().active().audio;
  ImGui::Spacing();

  // The screen brings no sound to play, delay or turn down: what is left is
  // what the recording takes.
  if (cfg().active().capture.source != VideoSource::Card) {
    DrawRecordSources(true);
    return;
  }

  ImGui::SeparatorText(T("Wiedergabe", "Playback"));
  const char* outPreview =
      audio.output.name.empty() ? T("Systemstandard", "System default") : audio.output.name.c_str();
  ImGui::SetNextItemWidth(-1.0f);
  if (ImGui::BeginCombo("##audioout", outPreview)) {
    if (ImGui::Selectable(T("Systemstandard", "System default"), audio.output.empty())) {
      audio.output = DeviceRef{};
    }
    for (const AudioDeviceInfo& d : audioOutputs_) {
      const bool selected = (d.id == audio.output.id);
      if (ImGui::Selectable(d.name.c_str(), selected)) audio.output = d.ToRef();
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  Anchor("audioout");

  ImGui::Checkbox("Exclusive Mode", &audio.exclusive);
  Anchor("exclusive");
  ImGui::SameLine();
  HelpMarker(T("Umgeht den Windows-Mixer, geringere Latenz. Sperrt das Gerät für andere "
               "Programme.",
               "Bypasses the Windows mixer, lower latency. Locks the device against other "
               "programs."));

  ImGui::Spacing();
  ImGui::SeparatorText(T("Verzögerung", "Delay"));
  Anchor("delay");
  ImGui::SetNextItemWidth(-260.0f);
  ImGui::SliderInt(T("Tonpuffer", "Audio buffer"), &audio.bufferMs, 5, 200, "%d ms");
  Anchor("audiobuffer");
  ResetOnRightClick(audio.bufferMs, kAudio.bufferMs);
  ImGui::SameLine();
  HelpMarker(T("Kleiner = weniger Verzögerung, ab einem Punkt Aussetzer. 20-40 ms üblich.",
               "Smaller = less delay, dropouts below a point. 20-40 ms is typical."));

  ImGui::SetNextItemWidth(-260.0f);
  ImGui::SliderInt(T("A/V-Versatz", "A/V offset"), &audio.avOffsetMs, -200, 200, "%d ms");
  Anchor("avoffset");
  ResetOnRightClick(audio.avOffsetMs, kAudio.avOffsetMs);
  ImGui::SameLine();
  HelpMarker(T("Positiv verzögert den Ton, negativ das Bild.",
               "Positive delays the audio, negative delays the picture."));
  if (audio.avOffsetMs < 0) {
    ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1.0f),
                       T("Bild wird um %d ms verzögert — das erhöht die Eingabelatenz.",
                         "Picture delayed by %d ms — this adds input latency."),
                       -audio.avOffsetMs);
  }

  ImGui::Spacing();
  ImGui::SeparatorText(T("Lautstärke", "Volume"));
  ImGui::SetNextItemWidth(-260.0f);
  // Reads straight from the live config, so the wheel and the hotkeys move this
  // slider while the dialog is open.
  float percent = audio.volume * 100.0f;
  if (ImGui::SliderFloat(T("Lautstärke", "Volume"), &percent, 0.0f, 100.0f, "%.0f %%")) {
    audio.volume = Clamp(percent / 100.0f, 0.0f, 1.0f);
  }
  Anchor("volume");
  ImGui::Checkbox(T("Stumm", "Muted"), &audio.mute);
  Anchor("mute");

  ImGui::Spacing();
  DrawRecordSources(false);
}

void SettingsWindow::DrawRecordSources(bool screen) {
  AudioSettings& audio = cfg().active().audio;
  ImGui::SeparatorText(T("Tonquellen der Aufnahme", "Recording sources"));
  Anchor("sources");
  if (sourcesLocked_) {
    ImGui::TextDisabled(T("Während der Aufnahme bleibt die Liste, wie sie ist.",
                          "The list stays as it is while recording."));
  }
  ImGui::Spacing();

  if (screen) {
    // Nothing comes with the picture; the game is in what Windows plays.
    TextDisabledWrapped(T("Der Bildschirm bringt keinen Ton mit. Für das Spiel „Systemton ohne "
                          "qBlank“ hinzufügen.",
                          "The screen brings no sound. For the game, add “System sound "
                          "without qBlank”."));
  } else {
    // The card's sound is always the first one; the list adds to it. Which
    // device that is gets picked under Source.
    const CaptureSettings& capture = cfg().active().capture;
    ImGui::TextUnformatted(T("Ton der Karte", "Card audio"));
    Anchor("cardaudio");
    if (capture.audioSource == AudioSource::None) {
      ImGui::TextDisabled(T("Keiner, im Reiter Quelle wählbar.", "None, chosen under Source."));
    } else {
      const bool found =
          capture.audioSource == AudioSource::Manual ||
          (embeddedAudioForDevice_ == capture.video.id && !embeddedAudioName_.empty());
      ImGui::TextDisabled("%s", !found ? T("Eingebettetes Audio", "Embedded audio")
                                : capture.audioSource == AudioSource::Manual
                                    ? capture.audio.name.c_str()
                                    : embeddedAudioName_.c_str());
      ImGui::SetNextItemWidth(-260.0f);
      LevelMeter(T("Pegel", "Level"), inputPeak_, true);
      ImGui::SameLine();
      HelpMarker(T("Vor Lautstärke und Stumm, so wie er aufgenommen wird.",
                   "Before volume and mute, as it is recorded."));
    }
  }
  ImGui::Spacing();

  const ImVec4 warn(0.95f, 0.72f, 0.35f, 1.0f);
  // "System default" is a device too, and the one the card plays on unless
  // something else is picked.
  auto outputOf = [&](const DeviceRef& ref) -> const AudioDeviceInfo* {
    for (const AudioDeviceInfo& d : audioOutputs_) {
      if (ref.empty() ? d.isDefault : d.id == ref.id) return &d;
    }
    return nullptr;
  };
  const AudioDeviceInfo* playback = outputOf(audio.output);
  int remove = -1;
  bool haveSystem = false;
  for (size_t i = 0; i < audio.sources.size(); ++i) {
    RecordSource& source = audio.sources[i];
    const SourceLevel level = i < sourceLevels_.size() ? sourceLevels_[i] : SourceLevel{};
    const bool system = source.kind == SourceKind::System;
    haveSystem |= system;
    ImGui::PushID((int)i);

    ImGui::BeginDisabled(sourcesLocked_);
    ImGui::Checkbox(system ? T("Systemton", "System sound") : T("Eingang", "Input"),
                    &source.enabled);
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         std::max(0.0f, ImGui::GetContentRegionAvail().x -
                                            ImGui::CalcTextSize(T("Entfernen", "Remove")).x -
                                            ImGui::GetStyle().FramePadding.x * 2.0f));
    if (ImGui::SmallButton(T("Entfernen", "Remove"))) remove = (int)i;
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!source.enabled);
    if (system) {
      ImGui::TextDisabled(T("Alles, was Windows abspielt, außer qBlank selbst.",
                            "Everything Windows plays, except qBlank itself."));
    } else {
      // One list for both: a virtual cable is a microphone on one side and an
      // output on the other, and which side carries the sound is the user's
      // setup. Capture card audio is no microphone; it heads the list above.
      const bool input = source.kind == SourceKind::Input;
      const char* preview = !source.device.name.empty() ? source.device.name.c_str()
                            : input ? T("Standard-Aufnahmegerät", "Default recording device")
                                    : T("Standard-Wiedergabegerät", "Default playback device");
      ImGui::BeginDisabled(sourcesLocked_);
      ImGui::SetNextItemWidth(-1.0f);
      if (ImGui::BeginCombo("##device", preview)) {
        auto section = [&](SourceKind kind, const char* title, const char* fallback,
                           const std::vector<AudioDeviceInfo>& devices) {
          const bool here = source.kind == kind;
          ImGui::PushID((int)kind);
          ImGui::SeparatorText(title);
          if (ImGui::Selectable(fallback, here && source.device.empty())) {
            source.kind = kind;
            source.device = DeviceRef{};
          }
          for (const AudioDeviceInfo& d : devices) {
            if (d.cardAudio) continue;
            const bool selected = here && d.id == source.device.id;
            if (ImGui::Selectable(d.name.c_str(), selected)) {
              source.kind = kind;
              source.device = d.ToRef();
            }
            if (selected) ImGui::SetItemDefaultFocus();
          }
          ImGui::PopID();
        };
        section(SourceKind::Input, T("Mikrofone und Eingänge", "Microphones and inputs"),
                T("Standard-Aufnahmegerät", "Default recording device"), audioInputs_);
        section(SourceKind::Output,
                T("Ausgabegeräte, aufgenommen wird, was dort läuft",
                  "Output devices, records what plays there"),
                T("Standard-Wiedergabegerät", "Default playback device"), audioOutputs_);
        ImGui::EndCombo();
      }
      ImGui::EndDisabled();
      if (!input) {
        ImGui::TextDisabled(T("Was dort abgespielt wird.", "What plays there."));
      }
      // The game is in that file once already, through the card.
      const AudioDeviceInfo* at = input || screen ? nullptr : outputOf(source.device);
      if (!input && !screen && (at ? at == playback : source.device == audio.output)) {
        ImGui::TextColored(warn, "%s",
                           T("Dort läuft auch der Ton der Karte: er wäre doppelt in der Aufnahme.",
                             "The card's sound plays there too: it would be recorded twice."));
      }
    }

    ImGui::SetNextItemWidth(-260.0f);
    float db = 20.0f * std::log10(std::max(source.gain, 0.01f));
    if (ImGui::SliderFloat(T("Verstärkung", "Gain"), &db, -20.0f, 12.0f, "%+.1f dB")) {
      source.gain = std::pow(10.0f, db / 20.0f);
    }
    Anchor("sourcegain");
    ResetOnRightClick(source.gain, 1.0f);
    ImGui::SameLine();
    HelpMarker(T("Zusätzlich zur Windows-Einstellung, wirkt nur auf die Aufnahme.",
                 "On top of the Windows setting, affects the recording only."));

    int trackMode = (int)source.tracks;
    ImGui::BeginDisabled(sourcesLocked_);
    ImGui::SetNextItemWidth(-260.0f);
    if (ComboEnum(T("Spuren", "Tracks"), &trackMode, 3, TrackModeName)) {
      source.tracks = (TrackMode)trackMode;
    }
    ImGui::EndDisabled();
    Anchor("sourcetracks");
    ImGui::SameLine();
    HelpMarker(T("Gemischt spielt überall ab. Getrennt lässt sich im Schnitt noch auseinander "
                 "nehmen.",
                 "Mixed plays anywhere. Separate can still be pulled apart in an editor."));

    ImGui::SetNextItemWidth(-260.0f);
    LevelMeter(T("Pegel", "Level"), level.peak, level.running);
    if (source.enabled && level.failed) {
      ImGui::TextColored(warn, "%s", T("Nicht verfügbar.", "Not available."));
    } else if (source.enabled && !level.running) {
      ImGui::TextDisabled(T("Wird geöffnet ...", "Opening ..."));
    }
    ImGui::EndDisabled();
    ImGui::PopID();
    ImGui::Spacing();
  }
  if (remove >= 0) audio.sources.erase(audio.sources.begin() + remove);

  ImGui::BeginDisabled(sourcesLocked_);
  if (ImGui::Button(T("Hinzufügen ...", "Add ..."))) ImGui::OpenPopup("addsource");
  ImGui::EndDisabled();
  Anchor("sourceadd");
  if (ImGui::BeginPopup("addsource")) {
    auto add = [&](SourceKind kind) {
      RecordSource source;
      source.kind = kind;
      source.enabled = true;
      audio.sources.push_back(source);
    };
    if (ImGui::Selectable(T("Eingang (Mikrofon oder Ausgabegerät)",
                            "Input (microphone or output device)"))) {
      add(SourceKind::Input);
    }
    // One is everything already.
    if (ImGui::Selectable(T("Systemton ohne qBlank", "System sound without qBlank"), false,
                          haveSystem ? ImGuiSelectableFlags_Disabled : 0)) {
      add(SourceKind::System);
    }
    ImGui::EndPopup();
  }
}

// --------------------------------------------------------------- display tab

void SettingsWindow::DrawHdrBlock() {
  AppSettings& app = cfg().app;
  ImGui::Spacing();
  ImGui::SeparatorText(T("Hoher Kontrastumfang (HDR)", "High dynamic range"));

  // What is actually the case, before any of the choices below. Two independent
  // facts, and most confusion about HDR comes from mixing them up.
  const bool screen = cfg().active().capture.source != VideoSource::Card;
  const bool linear = hdrSourceTransfer_ == 3;
  if (linear) {
    ImGui::TextDisabled(T("Quelle: HDR, Weiß bei %.0f nits", "Source: HDR, white at %.0f nits"),
                        hdrSourceWhite_);
  } else {
    ImGui::TextDisabled("%s", hdrSourceTransfer_ == 1   ? T("Quelle: HDR (PQ)", "Source: HDR (PQ)")
                              : hdrSourceTransfer_ == 2 ? T("Quelle: HDR (HLG)", "Source: HDR (HLG)")
                                                        : T("Quelle: SDR", "Source: SDR"));
  }
  ImGui::SameLine();
  ImGui::TextDisabled("   |   ");
  ImGui::SameLine();
  if (!hdrDisplayCapable_) {
    ImGui::TextDisabled("%s", T("Anzeige: kein HDR eingeschaltet",
                                "Display: HDR not switched on"));
  } else if (hdrOutputActive_) {
    ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f),
                       T("Anzeige: HDR aktiv, %.0f nits", "Display: HDR active, %.0f nits"),
                       hdrDisplayPeak_);
  } else {
    ImGui::TextDisabled(T("Anzeige: HDR möglich, %.0f nits",
                          "Display: HDR available, %.0f nits"), hdrDisplayPeak_);
  }
  ImGui::Spacing();

  // Was die Quelle liefert -- und ein Analogdekoder liefert kein HDR. PQ und HLG
  // sind digitale Uebertragungskurven; über Composite, S-Video oder Component
  // kommt so etwas nicht an, und eine Kurve zu wählen, die es nicht gibt, kann
  // nur schaden.
  //
  // Was darunter steht, bleibt trotzdem stehen: das ist die Anzeige, nicht die
  // Quelle. Läuft der Desktop im HDR-Modus, muss auch ein SDR-Bild mit einem
  // Weißwert in den HDR-Behälter geschrieben werden, sonst kommt es zu dunkel
  // oder zu hell heraus. Das gilt für ein SNES genauso wie für alles andere.
  //
  // Bildschirm und Fenster sagen selbst, was sie sind: lineares Licht mit dem
  // Weiß von Windows, sobald es den Bildschirm in HDR zeigt, sonst SDR.
  if (screen) {
    if (!linear) {
      TextDisabledWrapped(T("HDR kommt, sobald Windows den Bildschirm in HDR zeigt.",
                            "HDR comes once Windows runs the screen in HDR."));
    }
  } else if (!analogueSource_) {
    const char* inputNames[] = {
        T("Automatisch", "Automatic"), T("SDR", "SDR"), T("HDR10 (PQ)", "HDR10 (PQ)"),
        T("HLG", "HLG")};
    int input = (int)app.hdrInput;
    ImGui::SetNextItemWidth(-260.0f);
    if (ImGui::Combo(T("Quellkurve", "Source curve"), &input, inputNames, kHdrInputCount)) {
      app.hdrInput = (HdrInput)input;
    }
    Anchor("hdrin");
    ImGui::SameLine();
    HelpMarker(T("Automatisch folgt dem Medientyp des Treibers. Die meisten Karten melden "
                 "dort nichts; dann bleibt es bei SDR und muss hier von Hand gesetzt werden.",
                 "Automatic follows the driver's media type. Most cards report nothing there; "
                 "then it stays SDR and must be set here by hand."));
  } else {
    TextDisabledWrapped(
        T("Analoge Quelle: HDR gibt es dort nicht. Was bleibt, betrifft die Anzeige.",
          "Analogue source: there is no HDR there. What remains concerns the display."));
  }

  const char* outputNames[] = {T("Aus", "Off"), T("Automatisch", "Automatic"),
                               T("Immer wenn möglich", "Whenever possible")};
  int output = (int)app.hdrOutput;
  ImGui::SetNextItemWidth(-260.0f);
  if (ImGui::Combo(T("An die Anzeige", "To the display"), &output, outputNames,
                   kHdrOutputCount)) {
    app.hdrOutput = (HdrOutput)output;
  }
  Anchor("hdrout");
  ImGui::SameLine();
  HelpMarker(T("Automatisch schaltet nur um, wenn die Anzeige im HDR-Modus läuft und die "
               "Quelle HDR ist. Sonst wird das Bild auf die Anzeige heruntergerechnet.",
               "Automatic switches only when the display is in HDR mode and the source is "
               "HDR. Otherwise the picture is tone mapped to the display."));

  // Weiß und Spitze eines HDR-Bildschirms kennt Windows.
  if (!linear) {
    ImGui::SetNextItemWidth(-260.0f);
    ImGui::SliderFloat(T("Papierweiß", "Paper white"), &app.paperWhiteNits, 80.0f, 400.0f,
                       "%.0f nits");
    Anchor("paperwhite");
    ResetOnRightClick(app.paperWhiteNits, kApp.paperWhiteNits);
    ImGui::SameLine();
    HelpMarker(T("Helligkeit von gewöhnlichem Weiß, etwa Papier, nicht der hellsten Stelle. "
                 "203 ist der übliche HDR-Referenzwert.",
                 "Brightness of ordinary white, such as paper, not of the brightest "
                 "highlight. 203 is the usual HDR reference."));
  }

  // Auch das gehört der Quelle, also weg bei einer analogen: was ein SNES an
  // seiner hellsten Stelle in nits erreicht, ist keine sinnvolle Frage.
  if (analogueSource_) return;

  if (!screen) {
    ImGui::SetNextItemWidth(-260.0f);
    ImGui::SliderFloat(T("Spitze der Quelle", "Source peak"), &app.sourcePeakNits, 200.0f,
                       10000.0f, "%.0f nits", ImGuiSliderFlags_Logarithmic);
    Anchor("sourcepeak");
    ResetOnRightClick(app.sourcePeakNits, kApp.sourcePeakNits);
    ImGui::SameLine();
    HelpMarker(T("Spitzenhelligkeit der Quelle. DirectShow meldet sie nicht. Zu hoch macht "
                 "das Bild dunkler: 10000 bei einer 1000-Nits-Quelle setzt Papierweiß auf "
                 "einem SDR-Monitor auf 63 statt 88 Nits. 1000 passt für Konsolen.",
                 "Peak brightness of the source. DirectShow does not report it. Too high "
                 "darkens the picture: 10000 for a 1000-nit source puts paper white at 63 "
                 "instead of 88 nits on an SDR monitor. 1000 suits consoles."));
  }

  ImGui::Spacing();
  ImGui::SeparatorText(T("Was den Umfang behält", "What keeps the range"));

  // Only meaningful when the source has a range to keep. Shown either way, so
  // it is clear the settings exist and why they are doing nothing.
  const bool sourceIsHdr = hdrSourceTransfer_ != 0;
  ImGui::BeginDisabled(!sourceIsHdr);

  ImGui::Checkbox(T("Aufnahme", "Recording"), &app.recordHdr);
  Anchor("hdrrec");
  ImGui::SameLine();
  HelpMarker(T("Nimmt in 10 Bit mit PQ und BT.2020 auf, mit den Farbangaben, die die Datei "
               "als HDR kennzeichnen. Braucht einen 10-Bit-Encoder (bei HEVC und AV1 üblich, "
               "bei H.264 selten) und einen Player mit PQ. Aus: das heruntergerechnete Bild, "
               "das überall läuft.",
               "Records 10-bit PQ with BT.2020 and the colour tags that mark the file as HDR. "
               "Needs a 10-bit encoder (usual for HEVC and AV1, rare for H.264) and a player "
               "that supports PQ. Off: records the tone mapped picture, which plays "
               "anywhere."));

  ImGui::Checkbox(T("Screenshots", "Screenshots"), &app.screenshotHdr);
  Anchor("hdrshot");
  if (app.screenshotHdr) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Px(150.0f));
    const char* shotNames[] = {T("JPEG XR (.jxr)", "JPEG XR (.jxr)"), T("AVIF", "AVIF")};
    int shot = (int)app.hdrShotFormat;
    if (ImGui::Combo("##hdrshot", &shot, shotNames, kHdrShotFormatCount)) {
      app.hdrShotFormat = (HdrShotFormat)shot;
    }
  }
  ImGui::SameLine();
  HelpMarker(T("Das eingestellte Bildformat wird übergangen, da PNG und JPEG den Umfang nicht "
               "halten.\n\n"
               "JPEG XR braucht nichts: Windows bringt den Encoder mit, die Fotos-App öffnet "
               "es. AVIF öffnen alle Browser und das meiste außerhalb von Windows, es braucht "
               "aber ffmpeg.",
               "Overrides the chosen picture format, since PNG and JPEG cannot hold the "
               "range.\n\n"
               "JPEG XR needs nothing extra: Windows includes the encoder and the Photos app "
               "opens it. AVIF opens in all browsers and most software outside Windows, but "
               "needs ffmpeg."));

  ImGui::Checkbox(T("Virtuelle Kamera", "Virtual camera"), &app.cameraHdr);
  Anchor("hdrcam");
  ImGui::SameLine();
  HelpMarker(T("Bietet die Kamera zusätzlich in 10 Bit an; das empfangende Programm wählt. "
               "Standardmäßig aus: Kaum ein Programm kann mit einer HDR-Webcam umgehen, und "
               "eines, das 10 Bit ohne HDR-Unterstützung nimmt, zeigt ein falsches Bild. "
               "Wirkt beim nächsten Öffnen der Kamera.",
               "Also offers the camera in 10 bit; the receiving program chooses. Off by "
               "default: few programs handle HDR webcams, and one that takes 10 bit without "
               "HDR support shows a wrong picture. Applies the next time the camera is "
               "opened."));

  ImGui::EndDisabled();
  if (!sourceIsHdr) {
    ImGui::TextDisabled("%s", T("Die Quelle ist SDR — es gibt nichts zu behalten.",
                                "The source is SDR — there is nothing to keep."));
  }
}

// -------------------------------------------------------------- general tab

void SettingsWindow::DrawGeneralTab() {
  AppSettings& app = cfg().app;
  ImGui::Spacing();

  ImGui::SeparatorText(T("Sprache", "Language"));
  // Englisch zuerst, obwohl es in `Language` an zweiter Stelle steht: die Zahl
  // wandert in die Konfigurationsdatei, die Aufzählung darf also nicht nach der
  // Liste sortiert werden. Deshalb hier eine eigene Reihenfolge statt ComboEnum.
  // Oben steht, womit das Programm ohne Einstellung hochkommt.
  static const Language kLanguageOrder[] = {Language::English, Language::German};
  ImGui::SetNextItemWidth(-260.0f);
  if (ImGui::BeginCombo(T("Sprache", "Language"), LanguageName((int)app.language))) {
    for (Language option : kLanguageOrder) {
      const bool selected = app.language == option;
      if (ImGui::Selectable(LanguageName((int)option), selected)) {
        app.language = option;
        SetLanguage(app.language);
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  Anchor("language");

  ImGui::Spacing();
  ImGui::SeparatorText(T("Fenster", "Window"));
  ImGui::Checkbox(T("Einstellungen in eigenem Fenster", "Settings in their own window"),
                  &app.settingsSeparateWindow);
  Anchor("ownwindow");
  ImGui::SameLine();
  HelpMarker(T("Ein eigenes Fenster statt einer Fläche über dem Bild, verschiebbar auf einen "
               "zweiten Monitor oder neben das Bild.",
               "A separate window instead of a panel over the picture, movable to a second "
               "monitor or beside the picture."));

  ImGui::Spacing();
  ImGui::SeparatorText(T("Darstellung", "Appearance"));
  // Eigene Reihenfolge wie bei der Sprache: oben steht, womit das Programm ohne
  // Einstellung hochkommt. Die Aufzaehlung selbst darf nicht umsortiert werden,
  // sie wandert als Zahl in die Konfigurationsdatei.
  static const Theme kThemeOrder[] = {Theme::System, Theme::Dark, Theme::Light};
  ImGui::SetNextItemWidth(-260.0f);
  if (ImGui::BeginCombo(T("Design", "Theme"), ThemeName((int)app.theme))) {
    for (Theme option : kThemeOrder) {
      const bool selected = app.theme == option;
      if (ImGui::Selectable(ThemeName((int)option), selected)) app.theme = option;
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  Anchor("theme");

  // Accent presets as a row of swatches, plus a free colour picker. The whole
  // palette including the window background is derived from this.
  ImGui::Text("%s", T("Akzentfarbe", "Accent colour"));
  Anchor("accent");
  ImGui::SameLine();
  HelpMarker(T("Färbt auch den Fensterhintergrund in einen dunklen Ton davon.",
               "Also tints the window background with a dark shade of it."));

  const int presetCount = AccentPresetCount();
  const float swatch = ImGui::GetFrameHeight();
  for (int i = 0; i < presetCount; ++i) {
    const unsigned rgb = AccentPresetColor(i);
    const ImVec4 color(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f,
                       (rgb & 0xFF) / 255.0f, 1.0f);
    ImGui::PushID(i);
    if (ImGui::ColorButton("##accent", color,
                           ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                           ImVec2(swatch, swatch))) {
      app.accentColor = rgb;
    }
    ImGui::SetItemTooltip("%s", AccentPresetName(i));
    ImGui::PopID();
    if (i % 6 != 5 && i != presetCount - 1) ImGui::SameLine();
  }

  float custom[3] = {((app.accentColor >> 16) & 0xFF) / 255.0f,
                     ((app.accentColor >> 8) & 0xFF) / 255.0f,
                     (app.accentColor & 0xFF) / 255.0f};
  ImGui::SetNextItemWidth(-260.0f);
  if (ImGui::ColorEdit3(T("Eigene Farbe", "Custom colour"), custom,
                        ImGuiColorEditFlags_DisplayHex)) {
    app.accentColor = ((unsigned)(Clamp(custom[0], 0.0f, 1.0f) * 255.0f + 0.5f) << 16) |
                      ((unsigned)(Clamp(custom[1], 0.0f, 1.0f) * 255.0f + 0.5f) << 8) |
                      (unsigned)(Clamp(custom[2], 0.0f, 1.0f) * 255.0f + 0.5f);
  }
  Anchor("customcolour");

  ImGui::Spacing();
  ImGui::SeparatorText(T("Infobereich", "Notification area"));
  Anchor("traysec");
  ImGui::Checkbox(T("Symbol im Infobereich", "Icon in the notification area"), &app.trayIcon);
  Anchor("tray");
  ImGui::SameLine();
  HelpMarker(T("Ein Symbol im Infobereich der Taskleiste. Linksklick holt das Fenster nach "
               "vorn, Rechtsklick öffnet ein Menü mit den unten angehakten Befehlen. Der "
               "Knopf in der Taskleiste bleibt.",
               "An icon in the notification area. Left click brings the window to the front, "
               "right click opens a menu with the commands ticked below. The taskbar button "
               "stays."));
  ImGui::BeginDisabled(!app.trayIcon);
  ImGui::Indent();
  ImGui::TextDisabled("%s", T("Im Menü, außer Anzeigen und Beenden:",
                              "In its menu, besides Show and Quit:"));
  Anchor("trayitems");
  if (ImGui::BeginTable("##trayitems", 2, ImGuiTableFlags_SizingStretchSame)) {
    for (int i = 0; i < kTrayItemCount; ++i) {
      ImGui::TableNextColumn();
      bool on = app.trayItems[(size_t)i];
      ImGui::PushID(i);
      if (ImGui::Checkbox(TrayItemName(i), &on)) app.trayItems[(size_t)i] = on;
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::Unindent();
  ImGui::EndDisabled();

  ImGui::Spacing();
  ImGui::SeparatorText(T("Verknüpfungen", "Shortcuts"));
  Anchor("shortcutsec");
  if (!shortcutsRead_ && !shortcutsBusy_) {
    shortcutsBusy_ = true;
    setupJobs_.Ask([this] {
      const bool startMenu = HasShortcut(ShortcutPlace::StartMenu);
      const bool desktop = HasShortcut(ShortcutPlace::Desktop);
      return [this, startMenu, desktop] {
        shortcutStartMenu_ = startMenu;
        shortcutDesktop_ = desktop;
        shortcutsRead_ = true;
        shortcutsBusy_ = false;
      };
    });
  }
  // Buttons, not checkboxes: they act at once, and Discard could not take a
  // file back off the desktop. Each says what it would do, which is also what
  // tells whether the shortcut is there -- so they wait while that is asked.
  const auto shortcutButton = [&](ShortcutPlace place, bool* has, const char* add,
                                  const char* remove, const char* anchor) {
    ImGui::BeginDisabled(!shortcutsRead_ || shortcutsBusy_);
    if (ImGui::Button(*has ? remove : add)) {
      const bool adding = !*has;
      shortcutsBusy_ = true;
      setupJobs_.Ask([this, place, has, adding] {
        const bool ok = adding ? CreateShortcut(place) : RemoveShortcut(place);
        const bool now = HasShortcut(place);
        return [this, has, ok, now] {
          shortcutFailed_ = !ok;
          *has = now;
          shortcutsBusy_ = false;
        };
      });
    }
    ImGui::EndDisabled();
    Anchor(anchor);
  };
  shortcutButton(ShortcutPlace::StartMenu, &shortcutStartMenu_,
                 T("Ins Startmenü", "Add to the Start menu"),
                 T("Aus dem Startmenü entfernen", "Remove from the Start menu"), "shortcutstart");
  ImGui::SameLine();
  shortcutButton(ShortcutPlace::Desktop, &shortcutDesktop_, T("Auf den Desktop", "Add to the desktop"),
                 T("Vom Desktop entfernen", "Remove from the desktop"), "shortcutdesktop");
  ImGui::SameLine();
  HelpMarker(Format(T("Verknüpfungen zu %s am aktuellen Ort. Nach dem Verschieben neu "
                      "anlegen.",
                      "Shortcuts to %s at its current location. Add them again after "
                      "moving it."),
                    AppNameUtf8().c_str())
                 .c_str());
  if (shortcutFailed_) {
    ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.35f, 1.0f), "%s",
                       T("Die Verknüpfung ließ sich nicht ändern.",
                         "The shortcut could not be changed."));
  }

  ImGui::Spacing();
  ImGui::SeparatorText(T("Sonstiges", "Other"));
  ImGui::Checkbox(
      Format(T("Protokoll in %s.log schreiben", "Write a log to %s.log"), AppNameUtf8().c_str())
          .c_str(),
      &app.logToFile);
      Anchor("log");
  ImGui::SameLine();
  HelpMarker(T("Zur Fehlersuche. Schreibt eine Datei neben das Programm; jede Sitzung steht "
               "zwischen einer Start- und einer Endzeile mit Version und Uhrzeit. Fehlt die "
               "Endzeile, wurde das Programm nicht normal beendet; der nächste Start meldet "
               "das und trägt sie nach.\n\n"
               "Gekürzt wird beim Start, immer um ganze Sitzungen. Wirkt beim nächsten Start.",
               "For troubleshooting. Writes one file next to the program; each session sits "
               "between a start and an end line with version and time. A missing end line "
               "means the program did not exit normally; the next start reports it and adds "
               "the line.\n\n"
               "Trimmed at start, by whole sessions only. Applies on the next start."));

  LogRetention& keep = app.logRetention;
  ImGui::BeginDisabled(!app.logToFile);
  ImGui::Indent();
  ImGui::TextDisabled("%s", T("Beim Start entfernen:", "Remove at start:"));
  Anchor("logretention");
  ImGui::Checkbox(T("Sitzungen älter als##logAge", "Sessions older than##logAge"), &keep.byAge);
  ImGui::SameLine();
  ImGui::BeginDisabled(!keep.byAge);
  ImGui::SetNextItemWidth(Px(64.0f));
  if (ImGui::InputInt("##logDays", &keep.days, 0, 0, ImGuiInputTextFlags_CharsDecimal)) {
    keep.days = Clamp(keep.days, 1, 3650);
  }
  ImGui::SameLine();
  ImGui::TextUnformatted(T("Tage", "days"));
  ImGui::EndDisabled();
  ImGui::Checkbox(T("Alles vor den letzten##logCount", "Everything before the last##logCount"),
                  &keep.byCount);
  ImGui::SameLine();
  ImGui::BeginDisabled(!keep.byCount);
  ImGui::SetNextItemWidth(Px(64.0f));
  if (ImGui::InputInt("##logSessions", &keep.sessions, 0, 0, ImGuiInputTextFlags_CharsDecimal)) {
    keep.sessions = Clamp(keep.sessions, 1, 1000);
  }
  ImGui::SameLine();
  ImGui::TextUnformatted(T("Sitzungen", "sessions"));
  ImGui::EndDisabled();
  ImGui::Checkbox(T("Sitzungen älterer Versionen", "Sessions from older versions"),
                  &keep.olderVersions);
  ImGui::Unindent();
  ImGui::EndDisabled();
}

// -------------------------------------------------------------- display tab

void SettingsWindow::DrawDisplayTab() {
  AppSettings& app = cfg().app;
  ImGui::Spacing();

  ImGui::SeparatorText(T("Grafik", "Graphics"));
  if (vulkanBuilt_) {
    // Die Zahl ist kein Konfigurationswert, nur die Stelle in dieser Liste;
    // gespeichert wird weiter der Schalter `vulkan`.
    static const char* const kRenderers[] = {"Direct3D 11", "Vulkan"};
    int renderer = app.vulkan ? 1 : 0;
    ImGui::SetNextItemWidth(-260.0f);
    if (ImGui::Combo(T("Grafikschnittstelle", "Graphics API"), &renderer, kRenderers, 2)) {
      app.vulkan = renderer == 1;
    }
    Anchor("vulkan");
    ImGui::SameLine();
    HelpMarker(T("Gilt ab dem nächsten Start. Kommt Vulkan nicht hoch, läuft qBlank mit "
                 "Direct3D 11 weiter.",
                 "Takes effect on the next start. If Vulkan does not come up, qBlank carries on "
                 "with Direct3D 11."));
    if (app.vulkan != vulkanRunning_) {
      if (ImGui::Button(T("Jetzt neu starten", "Restart now"))) restartRequested_ = true;
      ImGui::SameLine();
      ImGui::TextDisabled(T("läuft gerade mit %s", "running on %s now"),
                          vulkanRunning_ ? "Vulkan" : "Direct3D 11");
    }
  }
  ImGui::Checkbox("VSync", &app.vsync);
  Anchor("vsync");
  ImGui::SameLine();
  HelpMarker(T("Aus ist der größte Latenzgewinn, kann aber Tearing zeigen.",
               "Off is the biggest latency win, but can show tearing."));

  ImGui::Spacing();
  ImGui::SeparatorText(T("Verhalten", "Behaviour"));
  ImGui::Checkbox(T("Immer im Vordergrund", "Always on top"), &app.alwaysOnTop);
  Anchor("ontop");
  ImGui::Checkbox(T("Rahmenlos", "Borderless"), &app.borderless);
  Anchor("borderless");
  ImGui::SameLine();
  ImGui::TextDisabled("(%s)", HotkeyText(cfg().hotkeys[HotkeyAction::Borderless]).c_str());
  ImGui::SameLine();
  HelpMarker(T("Ohne Titelleiste und Rahmen. Verschieben durch Ziehen am Bild, Größe an den Rändern.",
               "No title bar or frame. Drag the picture to move, the edges to resize."));
  ImGui::Checkbox(T("Mauszeiger im Vollbild ausblenden", "Hide cursor in fullscreen"),
                  &app.hideCursorFullscreen);
  Anchor("hidecursor");
  ImGui::SameLine();
  HelpMarker(T("Zeiger nach kurzer Ruhe ausblenden.", "Hides the pointer after a short idle."));

  ImGui::Checkbox(T("Bildschirmschoner und Standby verhindern", "Prevent screensaver and sleep"),
                  &app.preventSleep);
  Anchor("nosleep");
  ImGui::SameLine();
  HelpMarker(Format(T("Verhindert, dass sich der Bildschirm abschaltet, solange %s läuft.",
                      "Keeps the screen from turning off while %s is running."),
                    AppNameUtf8().c_str())
                 .c_str());

  // The toolbar's own Hide button was the only way to turn it off, and the
  // right-click menu the only way back. A setting that can be reached from one
  // place and undone from another is a setting people lose.
  ImGui::Checkbox(T("Werkzeugleiste anzeigen", "Show toolbar"), &app.showToolbar);
  Anchor("toolbar");
  ImGui::SameLine();
  ImGui::TextDisabled("(%s)", HotkeyText(cfg().hotkeys[HotkeyAction::Toolbar]).c_str());

  ImGui::Checkbox(T("Statistik einblenden", "Show statistics"), &app.showStats);
  Anchor("stats");
  ImGui::SameLine();
  // Read from the binding rather than written out, so rebinding the key is
  // visible here instead of leaving a stale "(F1)" behind.
  ImGui::TextDisabled("(%s)", HotkeyText(cfg().hotkeys[HotkeyAction::Stats]).c_str());

  ImGui::BeginDisabled(!app.showStats);
  int detail = (int)app.statsDetail;
  ImGui::SetNextItemWidth(-260.0f);
  if (ComboEnum(T("Umfang", "Detail"), &detail, 3, StatsDetailName)) {
    app.statsDetail = (StatsDetail)detail;
  }
  Anchor("statsdetail");
  ImGui::EndDisabled();
  ImGui::SameLine();
  HelpMarker(T("Kompakt: Bildraten und Durchlaufzeit. Normal: zusätzlich Format und Ton. "
               "Vollständig: alles.",
               "Compact: frame rates and pipeline delay. Normal: adds format and audio. "
               "Full: everything."));

  ImGui::Spacing();
  ImGui::SeparatorText(T("Lautstärke-Anzeige", "Volume readout"));
  Anchor("volosdsec");
  ImGui::Checkbox(T("Bei Änderung einblenden", "Show on change"), &app.showVolumeOsd);
  Anchor("volosd");
  ImGui::BeginDisabled(!app.showVolumeOsd);
  int corner = (int)app.osdCorner;
  ImGui::SetNextItemWidth(-260.0f);
  if (ComboEnum(T("Ecke", "Corner"), &corner, 4, OsdCornerName)) {
    app.osdCorner = (OsdCorner)corner;
  }
  Anchor("osdcorner");
  ImGui::EndDisabled();
  ImGui::Checkbox(T("Mausrad über dem Bild ändert die Lautstärke",
                    "Mouse wheel over the picture changes the volume"),
                  &app.wheelVolume);
  Anchor("wheelvolume");

  ImGui::Spacing();
  ImGui::SeparatorText(T("Vollbild", "Fullscreen"));
  std::string monitorPreview = T("Monitor des Fensters", "Whichever monitor the window is on");
  for (const MonitorInfoEntry& m : monitors_) {
    if (m.index == app.fullscreenMonitor) monitorPreview = m.name;
  }
  ImGui::SetNextItemWidth(-1.0f);
  if (ImGui::BeginCombo("##fsmonitor", monitorPreview.c_str())) {
    if (ImGui::Selectable(T("Monitor des Fensters", "Whichever monitor the window is on"),
                          app.fullscreenMonitor < 0)) {
      app.fullscreenMonitor = -1;
    }
    for (const MonitorInfoEntry& m : monitors_) {
      const bool selected = (m.index == app.fullscreenMonitor);
      if (ImGui::Selectable(m.name.c_str(), selected)) app.fullscreenMonitor = m.index;
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  Anchor("fsmonitor");
  ImGui::Checkbox(T("Beim Start im Vollbild öffnen", "Start in fullscreen"), &app.startFullscreen);
  Anchor("startfs");
}

// --------------------------------------------------------------- record tab

void SettingsWindow::FolderRow(const char* id, int pickTag, char* buffer, size_t bufferSize,
                              std::string* value, const std::filesystem::path& defaultFolder) {
  const ImGuiStyle& style = ImGui::GetStyle();
  const float browse = ImGui::CalcTextSize(T("Durchsuchen", "Browse")).x + style.FramePadding.x * 2;
  const float reset = ImGui::CalcTextSize(T("Standard", "Default")).x + style.FramePadding.x * 2;
  const float open = ImGui::CalcTextSize(T("Öffnen", "Open")).x + style.FramePadding.x * 2;

  ImGui::SetNextItemWidth(-(browse + reset + open + style.ItemSpacing.x * 3.0f));
  const std::string field = std::string("##") + id;
  if (ImGui::InputTextWithHint(field.c_str(), PathToUtf8(defaultFolder).c_str(), buffer,
                               bufferSize)) {
    *value = buffer;
  }
  Anchor(id);

  ImGui::SameLine();
  ImGui::BeginDisabled(picker_.busy());
  if (ImGui::Button((std::string(T("Durchsuchen", "Browse")) + "##" + id).c_str())) {
    FileDialogRequest request;
    request.mode = FileDialogRequest::Mode::Folder;
    request.startPath = value->empty() ? defaultFolder : Utf8ToPath(*value);
    picker_.Start(request, UiWindow(), pickTag);
  }
  ImGui::EndDisabled();

  ImGui::SameLine();
  // An empty string is what "default" means in the config, so resetting clears
  // the field rather than writing the resolved path into it -- that way the
  // folder keeps following the user profile if it ever moves.
  ImGui::BeginDisabled(value->empty());
  if (ImGui::Button((std::string(T("Standard", "Default")) + "##" + id).c_str())) {
    value->clear();
    buffer[0] = 0;
  }
  ImGui::EndDisabled();

  ImGui::SameLine();
  if (ImGui::Button((std::string(T("Öffnen", "Open")) + "##" + id).c_str())) {
    OpenFolder(value->empty() ? defaultFolder : Utf8ToPath(*value));
  }
}

void SettingsWindow::DrawFfmpegBlock(FfmpegInfo* ffmpeg) {
  const bool found = ffmpeg && ffmpeg->found;

  ImGui::SeparatorText("ffmpeg");
  Anchor("ffmpeg");
  if (found) {
    ImGui::TextWrapped("%s", ffmpeg->version.c_str());
    ImGui::TextDisabled("%s", ffmpeg->folder.c_str());
  } else {
    ImGui::TextWrapped(
        T("Ohne ffmpeg keine Aufnahme und kein AVIF. PNG-Screenshots funktionieren trotzdem.",
          "No recording and no AVIF without ffmpeg. PNG screenshots work regardless."));
    if (ffmpeg && !ffmpeg->error.empty()) {
      ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.35f, 1.0f), "%s", ffmpeg->error.c_str());
    }
  }

  if (downloader_) {
    FfmpegDownloader& dl = *downloader_;
    const bool busy = dl.busy();
    const FfmpegDownloader::State state = dl.state();
    // Only what the check said about the ffmpeg shown above: after a download
    // the comparison is with something that is no longer there.
    const bool update = dl.updateAvailable() && found &&
                        dl.installedVersion() == ffmpeg->number;
    const std::string remote = dl.remoteVersion();

    ImGui::BeginDisabled(busy);
    std::string label = found ? T("Neu herunterladen", "Download again")
                              : T("ffmpeg herunterladen", "Download ffmpeg");
    if (update) label = Format(T("Aktualisieren auf %s", "Update to %s"), remote.c_str());
    if (ImGui::Button((label + "##ffmpegget").c_str())) dl.Start(OwnFfmpegFolder());
    ImGui::SetItemTooltip(T("Shared-Build von gyan.dev, rund 60 MB (7z) oder 100 MB (zip), "
                            "SHA-256 wird geprüft.",
                            "Shared build from gyan.dev, about 60 MB (7z) or 100 MB (zip), "
                            "SHA-256 is verified."));
    ImGui::SameLine();
    if (ImGui::Button(T("Auf Updates prüfen", "Check for updates"))) {
      dl.StartVersionCheck(found ? ffmpeg->number : std::string());
    }
    ImGui::EndDisabled();

    if (busy) {
      const float p = dl.progress();
      if (p >= 0.0f) ImGui::ProgressBar(p, ImVec2(-1.0f, 0.0f));
      ImGui::TextDisabled("%s", dl.message().c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton(T("Abbrechen", "Cancel"))) dl.Cancel();
    } else if (update) {
      // Amber rather than green: something to do, not something done.
      const ImVec4 amber(0.95f, 0.72f, 0.35f, 1.0f);
      ImGui::TextColored(amber, T("ffmpeg %s ist verfügbar, installiert ist %s.",
                                  "ffmpeg %s is available, %s is installed."),
                         remote.c_str(), ffmpeg->number.c_str());
      if (state == FfmpegDownloader::State::Failed) {
        ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.35f, 1.0f), "%s", dl.message().c_str());
      }
    } else if (state != FfmpegDownloader::State::Idle &&
               !(state == FfmpegDownloader::State::Done && dl.updateAvailable())) {
      // The second half: an offer for an ffmpeg that is no longer the one shown.
      const bool failed = state == FfmpegDownloader::State::Failed;
      ImGui::TextColored(failed ? ImVec4(0.95f, 0.5f, 0.35f, 1.0f) : ImVec4(0.5f, 0.85f, 0.5f, 1.0f),
                         "%s", dl.message().c_str());
    }
  }
}

void SettingsWindow::LoadRecordBuffers() {
  if (recordBuffersLoaded_) return;
  const RecordSettings& rec = cfg().record;
  std::snprintf(folderBuffer_, sizeof(folderBuffer_), "%s", rec.outputFolder.c_str());
  std::snprintf(shotFolderBuffer_, sizeof(shotFolderBuffer_), "%s", rec.screenshotFolder.c_str());
  recordFolderDefault_ = DefaultRecordFolder();
  shotFolderDefault_ = DefaultScreenshotFolder();
  recordBuffersLoaded_ = true;
}

// HDR has a tab of its own rather than a block inside Display. It is not a
// display setting: three of its switches decide what the recorder, the
// screenshots and the virtual camera write, and those live in other tabs
// entirely. A subject that reaches across four tabs is a subject, not a section.
void SettingsWindow::DrawHdrTab() { DrawHdrBlock(); }

// All of the memory as the bar, with what others hold, and the reserve, coming
// in from the right; it cannot be dragged there.
void SettingsWindow::MemorySlider(const char* id, int* gb, int standard, const char* field,
                                  const char* freeFor) {
  const bool memKnown = memTotal_ > 0;
  const uint64_t reserve = std::min(kRecordBufferReserveBytes, memTotal_);
  const uint64_t room = memFree_ > reserve ? memFree_ - reserve : 0;
  const int maxGb =
      memKnown ? std::max(2, (int)std::lround((double)memTotal_ / kGb)) : kMaxRecordBufferGb;
  const int roomGb = memKnown ? Clamp((int)((double)room / kGb), 1, maxGb) : kMaxRecordBufferGb;
  const ImVec4 usedColor(0.95f, 0.42f, 0.36f, 1.0f);
  const ImVec4 reserveColor(1.0f, 0.72f, 0.25f, 1.0f);
  ImGui::SetNextItemWidth(-360.0f);
  if (ImGui::SliderInt(id, gb, 1, maxGb, "%d GB", ImGuiSliderFlags_AlwaysClamp)) {
    *gb = std::min(*gb, roomGb);
  }
  if (memKnown) {
    MemoryOverlay(maxGb, (double)room / kGb, (double)reserve / kGb,
                  ImVec4(usedColor.x, usedColor.y, usedColor.z, 0.40f),
                  ImVec4(reserveColor.x, reserveColor.y, reserveColor.z, 0.28f));
  }
  Anchor(id + 2);  // without the "##"
  ResetOnRightClick(*gb, standard);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(Px(96.0f));
  if (ImGui::InputInt(field, gb, 0, 0, ImGuiInputTextFlags_CharsDecimal)) {
    *gb = Clamp(*gb, 1, roomGb);
  }

  // Die Legende zum Balken, in seinen Farben.
  if (memKnown) {
    const uint64_t used = memTotal_ > memFree_ ? memTotal_ - memFree_ : 0;
    ImGui::TextDisabled(freeFor, FormatBytes(room).c_str());
    ImGui::SameLine();
    ImGui::TextColored(usedColor, T("%s belegt,", "%s in use,"), FormatBytes(used).c_str());
    ImGui::SameLine();
    ImGui::TextColored(reserveColor, T("%s Reserve", "%s reserve"), FormatBytes(reserve).c_str());
  }
}

void SettingsWindow::DrawRecordTab(FfmpegInfo* ffmpeg) {
  RecordSettings& rec = cfg().record;

  LoadRecordBuffers();

  const bool ready = ffmpeg && ffmpeg->found;
  if (!ready) {
    ImGui::Spacing();
    ImGui::TextWrapped(T("Ohne ffmpeg lässt sich nicht aufnehmen. Im Reiter Encoder steht, "
                         "wie es dazu kommt.",
                         "Recording needs ffmpeg. The Encoder tab says how to get it."));
  }

  ImGui::BeginDisabled(!ready);

  ImGui::TextWrapped(
      T("Aufgenommen wird in Quellauflösung, nach Zuschnitt und Deinterlacing, vor der "
        "Fensterskalierung. Die Fenstergröße hat keinen Einfluss.",
        "Records at source resolution, after crop and deinterlacing, before window scaling. "
        "The window size has no effect."));
  ImGui::Spacing();

  // ---- output ----
  ImGui::SeparatorText(T("Ausgabe", "Output"));

  // A lossless encoder brings its own container, shown here and not changed:
  // the choice stays for when the encoder goes back.
  const bool lossless = IsLosslessEncoder(rec.encoder);
  int container = (int)ContainerFor(rec.encoder, rec.container);
  ImGui::BeginDisabled(lossless);
  ImGui::SetNextItemWidth(-260.0f);
  if (ComboEnum(T("Container", "Container"), &container, lossless ? 3 : 2, RecordContainerName) &&
      !lossless) {
    rec.container = (RecordContainer)container;
  }
  Anchor("container");
  ImGui::EndDisabled();
  ImGui::SameLine();
  HelpMarker(lossless ? T("Verlustfrei legt den Container fest: H.265 in MKV, UT Video und "
                          "Unkomprimiert in AVI.",
                          "Lossless sets the container: H.265 in MKV, UT Video and uncompressed "
                          "in AVI.")
                      : T("MKV übersteht einen Absturz. Eine nicht sauber geschlossene MP4 lässt "
                          "sich nicht öffnen.",
                          "MKV survives a crash. An MP4 that was not closed properly will not "
                          "open."));

  // AAC per track. An AVI takes the sound as it is, and the choice waits for
  // the container to come back.
  static const int kAudioKbps[] = {96, 128, 160, 192, 256, 320};
  const bool pcm = ContainerFor(rec.encoder, rec.container) == RecordContainer::Avi;
  ImGui::BeginDisabled(pcm);
  ImGui::SetNextItemWidth(-260.0f);
  if (ImGui::BeginCombo(T("Tonbitrate", "Audio bitrate"),
                        pcm ? "PCM" : Format("%d kbit/s", rec.audioKbps).c_str())) {
    for (int kbps : kAudioKbps) {
      if (ImGui::Selectable(Format("%d kbit/s", kbps).c_str(), rec.audioKbps == kbps)) {
        rec.audioKbps = kbps;
      }
    }
    ImGui::EndCombo();
  }
  Anchor("audiokbps");
  ResetOnRightClick(rec.audioKbps, kRecord.audioKbps);
  ImGui::EndDisabled();
  ImGui::SameLine();
  HelpMarker(pcm ? T("AVI nimmt den Ton unverändert auf (PCM).",
                     "AVI keeps the sound as it is (PCM).")
                 : T("AAC, je Tonspur. Fällt bei der Dateigröße kaum ins Gewicht.",
                     "AAC, per audio track. Barely affects the file size."));

  // Die Bitrate stand frueher hier und steht jetzt im Reiter Encoder, direkt
  // unter der Ratensteuerung. Getrennt waren die beiden nicht zu verstehen: bei
  // "Feste Qualitaet" ignoriert der Encoder die Bitrate vollstaendig, und der
  // Regler dafuer sass einen Reiter weiter, voll bedienbar und wirkungslos.

  // Recording faster than the card delivers would only duplicate frames, so the
  // ceiling is the source rate. The number below it is only reached when the
  // rate is not known yet, and it is a slider end rather than a limit -- the
  // unbounded setting is 0, "same as source", which takes whatever arrives.
  // Each source has its own, and a higher one stays stored: the recorder caps
  // it at the source, so PAL's 50 does not overwrite 60 for when NTSC returns.
  double& recFps = rec.FpsFor(cfg().active().capture.source);
  const int maxFps = sourceFps_ > 1.0 ? (int)std::lround(sourceFps_) : 1000;
  int fps = std::min((int)std::lround(recFps), maxFps);
  ImGui::SetNextItemWidth(-260.0f);
  if (ImGui::SliderInt(T("Bildrate", "Frame rate"), &fps, 0, maxFps,
                       fps == 0 ? T("wie die Quelle", "same as source") : "%d fps")) {
    recFps = (double)fps;
  }
  Anchor("recfps");
  ResetOnRightClick(recFps, kRecord.fps);
  ImGui::SameLine();
  HelpMarker(T("0 = Bildrate der Quelle. Niedriger verwirft Bilder, Auflösung bleibt. Karte, "
               "Bildschirm und Fenster merken sich je ihre eigene.",
               "0 = source frame rate. Lower drops frames, resolution unchanged. Card, screen "
               "and window each keep their own."));

  // Only ever down: the list holds what is smaller than the picture, and a cap
  // the picture is already under shows as the source. The value stays, for a
  // larger source later.
  static const int kLines[] = {2160, 1440, 1080, 900, 720, 540, 480, 360};
  const int shortSide = std::min(pictureWidth_, pictureHeight_);
  auto linesLabel = [&](int lines) {
    if (lines <= 0 || (shortSide > 0 && lines >= shortSide)) {
      if (shortSide <= 0) return std::string(T("wie die Quelle", "same as source"));
      return Format(T("wie die Quelle (%dx%d)", "same as source (%dx%d)"), pictureWidth_ & ~1,
                    pictureHeight_ & ~1);
    }
    if (shortSide <= 0) return Format("%dp", lines);
    int w = 0, h = 0;
    CappedRecordSize(pictureWidth_, pictureHeight_, lines, &w, &h);
    return Format("%dp (%dx%d)", lines, w, h);
  };
  const bool cardBytes =
      cfg().active().capture.source == VideoSource::Card && rec.unfiltered && lossless;
  ImGui::BeginDisabled(cardBytes);
  ImGui::SetNextItemWidth(-260.0f);
  if (ImGui::BeginCombo(T("Auflösung", "Resolution"), linesLabel(rec.maxLines).c_str())) {
    const bool asSource = rec.maxLines <= 0 || (shortSide > 0 && rec.maxLines >= shortSide);
    if (ImGui::Selectable(linesLabel(0).c_str(), asSource)) {
      rec.maxLines = 0;
    }
    for (int lines : kLines) {
      if (shortSide > 0 && lines >= shortSide) continue;
      if (ImGui::Selectable(linesLabel(lines).c_str(), rec.maxLines == lines)) rec.maxLines = lines;
    }
    ImGui::EndCombo();
  }
  Anchor("recsize");
  ResetOnRightClick(rec.maxLines, kRecord.maxLines);
  ImGui::EndDisabled();
  ImGui::SameLine();
  HelpMarker(cardBytes ? T("Ohne Filter nimmt die Karte in ihrer eigenen Auflösung auf.",
                           "Unfiltered records the card at its own resolution.")
                       : T("Kleiner als die Quelle, im selben Seitenverhältnis. Entlastet den "
                           "Encoder; Anzeige, Kamera und Screenshots bleiben in voller Größe.",
                           "Smaller than the source, same aspect ratio. Eases the encoder; the "
                           "display, camera and screenshots keep full size."));

  FolderRow("recfolder", kPickRecordFolder, folderBuffer_, sizeof(folderBuffer_),
            &rec.outputFolder, recordFolderDefault_);

  // Was auf das Laufwerk noch draufgeht. Die Zeit ist die eigentliche Auskunft:
  // "412 GB frei" beantwortet die Frage nicht, die jemand vor einer Aufnahme
  // hat, "noch 4 h 32 min" schon.
  if (diskKnown_) {
    const double seconds = diskBytesPerSecond_ > 1.0 ? (double)diskFree_ / diskBytesPerSecond_ : 0.0;
    std::string line = Format(T("%s frei", "%s free"), FormatBytes(diskFree_).c_str());
    if (seconds > 0.0) {
      line += Format(T(" — reicht für etwa %s Aufnahme", " — about %s of recording"),
                     FormatDuration(seconds).c_str());
    }
    ImGui::TextDisabled("%s", line.c_str());
    ImGui::SameLine();
    HelpMarker(lossless ? T("Geschätzt aus Bildgröße und Bildrate.",
                            "Estimated from picture size and frame rate.")
                        : T("Geschätzt aus der eingestellten Bitrate. Bei Qualitätsmodus ist die "
                            "Bitrate nur die Obergrenze, es reicht also meist länger.",
                            "Estimated from the configured bitrate. In quality mode the bitrate "
                            "is only a ceiling, so it usually lasts longer."));
  }

  ImGui::Checkbox(T("Bei Größe aufteilen", "Split at size"), &rec.splitFiles);
  Anchor("split");
  ImGui::SameLine();
  HelpMarker(T("Nur für FAT32 nötig. NTFS und exFAT haben kein 4-GB-Limit. Geteilt wird nahtlos.",
               "Only needed on FAT32. NTFS and exFAT have no 4 GB limit. The split is seamless."));
  ImGui::BeginDisabled(!rec.splitFiles);
  ImGui::SetNextItemWidth(-260.0f);
  ImGui::SliderInt(T("Teilgröße", "Split size"), &rec.splitSizeMb, 100, 20000, "%d MB");
  Anchor("splitsize");
  ResetOnRightClick(rec.splitSizeMb, kRecord.splitSizeMb);
  ImGui::EndDisabled();

  // Wie viel, sagt der Nutzer selbst -- wer 64 GB hat, will vielleicht mehr
  // davon hergeben --, aber nur aus dem, was gerade frei ist. Der Regler zeigt
  // den ganzen Speicher, und was andere Programme halten, samt einer Reserve
  // fuer sie, schiebt sich von rechts hinein; dorthin laesst er sich nicht
  // ziehen. Der Recorder prueft dasselbe beim Start und waehrend der Aufnahme.
  ImGui::Checkbox(T("Größerer Puffer im Arbeitsspeicher", "Larger buffer in memory"),
                  &rec.largeBuffer);
  Anchor("buffer");
  ImGui::SameLine();
  HelpMarker(T("Was die Platte noch nicht geschrieben hat, wartet im Arbeitsspeicher, ohne "
               "den Haken bis 512 MB. Belegt nur, solange die Platte hinterherhängt, nie mehr "
               "als der freie Speicher abzüglich 2 GB. Ist er voll, endet die Aufnahme, und "
               "der Inhalt wird noch geschrieben.",
               "Data the disk has not written yet waits in memory, up to 512 MB without this. "
               "Used only while the disk falls behind, never more than free memory minus 2 "
               "GB. When full, the recording stops and the buffer is still written."));
  ImGui::BeginDisabled(!rec.largeBuffer);
  MemorySlider("##buffersize", &rec.bufferGb, kRecord.bufferGb,
               T("Puffergröße (GB)", "Buffer size (GB)"),
               T("%s frei für den Puffer,", "%s free for the buffer,"));
  ImGui::EndDisabled();

  ImGui::EndDisabled();

  // ---- stills ----
  // Outside the disabled block on purpose: screenshots go through Windows' own
  // imaging stack and have nothing to do with ffmpeg.
  ImGui::Spacing();
  ImGui::SeparatorText(T("Screenshots", "Screenshots"));
  Anchor("shots");
  ImGui::TextWrapped(T("Einzelbild aus der Quelle, auf Wunsch mit der Bedienoberfläche. Benötigt "
                       "kein ffmpeg.",
                       "Single frame from the source, with the interface if you like. Does not "
                       "need ffmpeg."));
  ImGui::Spacing();

  ImGui::Checkbox(T("Bedienoberfläche mit aufnehmen", "Include the interface"),
                  &rec.screenshotIncludeUi);
  Anchor("shotui");
  HelpMarker(T("Speichert das ganze Fenster statt des Bildes, mit Leiste und Meldungen, in "
               "Fenstergröße. Immer SDR; bei HDR-Ausgabe wird nur das Bild gespeichert.",
               "Saves the whole window instead of the picture, including the bar and "
               "messages, at window size. Always SDR; with HDR output only the picture is "
               "saved."));
  ImGui::Spacing();

  int shot = (int)rec.screenshotFormat;
  ImGui::SetNextItemWidth(-260.0f);
  if (ComboEnum(T("Format", "Format"), &shot, 2, ScreenshotFormatName)) {
    rec.screenshotFormat = (ScreenshotFormat)shot;
  }
  Anchor("shotformat");
  ImGui::SameLine();
  HelpMarker(T("PNG verlustfrei, JPEG kleiner.", "PNG lossless, JPEG smaller."));

  ImGui::BeginDisabled(rec.screenshotFormat != ScreenshotFormat::Jpeg);
  ImGui::SetNextItemWidth(-260.0f);
  ImGui::SliderInt(T("JPEG-Qualität", "JPEG quality"), &rec.jpegQuality, 1, 100, "%d %%");
  Anchor("jpegq");
  ResetOnRightClick(rec.jpegQuality, kRecord.jpegQuality);
  ImGui::EndDisabled();

  FolderRow("shotfolder", kPickShotFolder, shotFolderBuffer_, sizeof(shotFolderBuffer_),
            &rec.screenshotFolder, shotFolderDefault_);
  ImGui::Spacing();
  DrawVirtualCameraBlock();
  ImGui::Spacing();
  ImGui::SeparatorText(T("Nach MP4 umpacken", "Rewrap to MP4"));
  Anchor("remux");

  ImGui::TextWrapped(T("Packt MKV-Dateien ohne Neukodierung in MP4 um, auch verlustfreie "
                       "H.265-Aufnahmen. Dauert Sekunden und kostet keine Qualität; das "
                       "Original bleibt. UT Video und Unkomprimiert passen nicht in MP4.",
                       "Rewraps MKV files as MP4 without re-encoding, lossless H.265 "
                       "included. Takes seconds and costs no quality; the original is kept. "
                       "UT Video and uncompressed do not fit in MP4."));
  ImGui::Spacing();

  const bool remuxBusy = remuxer_.busy();
  ImGui::BeginDisabled(remuxBusy || picker_.busy() || !ffmpeg || !ffmpeg->found);
  if (ImGui::Button(T("Dateien wählen ...", "Choose files ..."))) {
    FileDialogRequest request;
    request.mode = FileDialogRequest::Mode::OpenFiles;
    request.title = T("Aufnahmen auswählen", "Select recordings");
    request.startPath =
        rec.outputFolder.empty() ? recordFolderDefault_ : Utf8ToPath(rec.outputFolder);
    request.filters = {{T("Aufnahmen", "Recordings"), "*.mkv;*.mp4;*.mov;*.avi;*.ts"},
                       {T("Alle Dateien", "All files"), "*.*"}};
    picker_.Start(request, UiWindow(), kPickRemux);
  }
  ImGui::EndDisabled();

  if (remuxBusy) {
    ImGui::SameLine();
    if (ImGui::Button(T("Abbrechen##remux", "Cancel##remux"))) remuxer_.Cancel();
  }
  if (ffmpeg && !ffmpeg->found) {
    ImGui::SameLine();
    ImGui::TextDisabled(T("(benötigt ffmpeg)", "(needs ffmpeg)"));
  }

  if (remuxer_.state() != Remuxer::State::Idle) {
    if (remuxBusy) ImGui::ProgressBar(remuxer_.progress(), ImVec2(-1.0f, 0.0f));
    const std::string status = remuxer_.message();
    if (!status.empty()) {
      const bool failed = remuxer_.state() == Remuxer::State::Failed;
      ImGui::TextColored(
          failed ? ImVec4(0.9f, 0.5f, 0.4f, 1.0f) : ImGui::GetStyle().Colors[ImGuiCol_Text], "%s",
          status.c_str());
    }
    // Only failures are listed; a success is its own file on disk.
    for (const Remuxer::Item& item : remuxer_.items()) {
      if (!item.done || item.ok) continue;
      ImGui::TextDisabled("- %s", item.error.c_str());
    }
  }
}

// The replay rides on a running recording's encoders, on its own otherwise;
// nothing reaches the disk before the key is pressed.
void SettingsWindow::DrawReplayTab(FfmpegInfo* ffmpeg) {
  RecordSettings& rec = cfg().record;

  const bool ready = ffmpeg && ffmpeg->found;
  if (!ready) {
    ImGui::Spacing();
    ImGui::TextWrapped(T("Ohne ffmpeg gibt es kein Replay. Im Reiter Encoder steht, wie es dazu "
                         "kommt.",
                         "The replay needs ffmpeg. The Encoder tab says how to get it."));
  }
  ImGui::BeginDisabled(!ready);

  // An action by its name in the Keys tab, with its key when it has one.
  const auto action = [this](HotkeyAction a) {
    std::string text = Format(T("„%s“", "\"%s\""), HotkeyActionName(a));
    const std::string key = HotkeyText(cfg().hotkeys[a]);
    if (!key.empty()) text += " (" + key + ")";
    return text;
  };
  ImGui::TextWrapped(
      "%s", Format(T("Hält die letzten Sekunden mit dem Encoder aus dem Reiter Encoder "
                     "bereit. %s speichert sie als Clip im Unterordner clips des "
                     "Aufnahmeordners. Läuft neben einer Aufnahme ohne zweiten Encoder mit.",
                     "Keeps the last seconds, using the encoder from the Encoder tab. %s "
                     "saves them as a clip in the clips subfolder of the recording folder. "
                     "Runs alongside a recording without a second encoder."),
                   action(HotkeyAction::ReplaySave).c_str())
                .c_str());
  ImGui::Spacing();

  ImGui::Checkbox(T("Replay im Hintergrund", "Replay in the background"), &rec.replay);
  Anchor("replay");
  ImGui::SameLine();
  HelpMarker(Format(T("%s schaltet es auch um.", "%s switches it too."),
                    action(HotkeyAction::ReplayToggle).c_str())
                 .c_str());

  ImGui::BeginDisabled(!rec.replay);
  ImGui::SetNextItemWidth(-260.0f);
  ImGui::SliderInt(T("Länge", "Length"), &rec.replaySeconds, kMinReplaySeconds, kMaxReplaySeconds,
                   "%d s", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
  Anchor("replaylength");
  ResetOnRightClick(rec.replaySeconds, kRecord.replaySeconds);

  ImGui::Spacing();
  ImGui::SeparatorText(T("Ablage", "Storage"));
  if (ImGui::RadioButton(T("Arbeitsspeicher", "Memory"), !rec.replayOnDisk)) rec.replayOnDisk = false;
  Anchor("replaystore");
  ImGui::SameLine();
  if (ImGui::RadioButton(T("Platte", "Disk"), rec.replayOnDisk)) rec.replayOnDisk = true;
  HelpMarker(T("Liegt im Aufnahmeordner unter clips\\.replay und braucht keinen "
               "Arbeitsspeicher. Für eine schnelle SSD, da der Encoder ständig schreibt. Hält "
               "Platz für einen Clip in voller Länge frei.",
               "Stored in the recording folder under clips\\.replay, using no memory. For a "
               "fast SSD, as the encoder writes continuously. Keeps space free for a clip of "
               "the full length."));

  // What the length costs, by the same estimate as the time left on the
  // recording tab. Should the room run out first, the replay holds less.
  const double rate = diskBytesPerSecond_;
  const double need = rate * rec.replaySeconds;
  if (!rec.replayOnDisk) {
    MemorySlider("##replaymemory", &rec.replayGb, kRecord.replayGb,
                 T("Höchstens (GB)", "At most (GB)"),
                 T("%s frei für das Replay,", "%s free for the replay,"));
    if (rate > 1.0) {
      const double cap = (double)rec.replayGb * kGb;
      std::string line = Format(T("%d s brauchen etwa %s", "%d s take about %s"),
                                rec.replaySeconds, FormatBytes((uint64_t)need).c_str());
      if (need > cap) {
        line += Format(T(" — mehr als erlaubt, es bleiben etwa %s",
                         " — more than allowed, about %s fit"),
                       FormatDuration(cap / rate).c_str());
      }
      ImGui::TextDisabled("%s", line.c_str());
    }
  } else if (rate > 1.0) {
    std::string line = Format(T("%d s brauchen etwa %s auf der Platte", "%d s take about %s of disk"),
                              rec.replaySeconds, FormatBytes((uint64_t)need).c_str());
    // The recorder's rule: half of what is free past the reserve, the other
    // half for the clip.
    if (diskKnown_) {
      const double room = ((double)diskFree_ + (double)replayDiskHeld_ -
                           (double)kRecordBufferReserveBytes) / 2.0;
      if (need > room) {
        line += Format(T(" — zu wenig frei, es bleiben etwa %s", " — too little free, about %s fit"),
                       FormatDuration(std::max(room, 0.0) / rate).c_str());
      }
    }
    ImGui::TextDisabled("%s", line.c_str());
  }
  ImGui::EndDisabled();

  ImGui::EndDisabled();
}

void SettingsWindow::DrawEncoderTab(FfmpegInfo* ffmpeg) {
  RecordSettings& rec = cfg().record;
  LoadRecordBuffers();

  // Without ffmpeg none of the recording settings mean anything, so it leads and
  // the rest is greyed out rather than inviting people to configure a bitrate
  // for an encoder that cannot run.
  const bool ready = ffmpeg && ffmpeg->found;
  // Always here, whether ffmpeg was found or not. It used to move to the bottom
  // of the tab once it was working, so where you last saw it was no guide to
  // where it is -- and a thing that moves is a thing you hunt for.
  ImGui::Spacing();
  DrawFfmpegBlock(ffmpeg);
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  ImGui::BeginDisabled(!ready);

  // ---- encoder ----
  ImGui::Spacing();
  ImGui::SeparatorText(T("Encoder", "Encoder"));

  // Only what survived the test is offered. Listing every hardware encoder on a
  // machine that has one vendor's card invites people to pick something that
  // cannot run, and then wonder why.
  const bool tested = ffmpeg && ffmpeg->tested;
  const EncoderInfo* chosen = ffmpeg ? ffmpeg->Find(rec.encoder) : nullptr;
  const bool chosenOk = IsAutoEncoder(rec.encoder) || (chosen && chosen->available);

  const char* preview = !tested ? T("noch nicht geprüft", "not tested yet")
                                : RecordEncoderName((int)rec.encoder);

  ImGui::BeginDisabled(!tested);
  ImGui::SetNextItemWidth(-260.0f);
  if (ImGui::BeginCombo(T("Encoder", "Encoder"), preview)) {
    for (RecordEncoder mode : {RecordEncoder::Auto, RecordEncoder::AutoEfficient}) {
      if (ImGui::Selectable(RecordEncoderName((int)mode), rec.encoder == mode)) {
        rec.encoder = mode;
      }
    }
    ImGui::Separator();
    // The lossless ones last, under a line of their own (kCandidates).
    bool lossyListed = false, losslessListed = false;
    for (const EncoderInfo& e : ffmpeg->encoders) {
      if (!e.available) continue;
      if (IsLosslessEncoder(e.id)) {
        if (lossyListed && !losslessListed) ImGui::Separator();
        losslessListed = true;
      } else {
        lossyListed = true;
      }
      const bool selected = (rec.encoder == e.id);
      if (ImGui::Selectable(RecordEncoderName((int)e.id), selected)) rec.encoder = e.id;
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  Anchor("encoder");
  ImGui::EndDisabled();
  ImGui::SameLine();
  HelpMarker(T("Automatisch: der verträglichste, der hier läuft — H.264 vor H.265 vor AV1, Hardware vor CPU.",
               "Automatic: the most compatible one that works here — H.264 before H.265 before AV1, hardware before CPU."));

  // What the two automatic modes actually do, in the terms that matter: where
  // the file will play, and how big it is. Naming the codecs alone would only
  // help people who already know the answer.
  if (tested && IsAutoEncoder(rec.encoder)) {
    const EncoderInfo* picked = ffmpeg->Resolve(rec.encoder);
    TextDisabledWrapped(
        rec.encoder == RecordEncoder::Auto
            ? T("H.264 zuerst: läuft auf allem, auch auf älteren Fernsehern und Handys.",
                "H.264 first: plays on anything, including older TVs and phones.")
            : T("AV1 und H.265 zuerst: bei gleicher Qualität deutlich kleinere Dateien, "
                "aber ältere Geräte können sie nicht abspielen.",
                "AV1 and H.265 first: much smaller files at the same quality, but older "
                "devices cannot play them."));
    if (picked) {
      ImGui::TextDisabled(T("Hier heißt das: %s", "Here that means: %s"),
                          RecordEncoderName((int)picked->id));
    }
  }
  if (tested && rec.encoder == RecordEncoder::SvtAv1) {
    TextDisabledWrapped(T("Braucht viel Prozessorleistung. Kommt sie nicht mit, eine "
                          "schnellere Voreinstellung oder eine Grafikkarte mit AV1 nutzen.",
                          "CPU-heavy. If it cannot keep up, choose a faster preset or a "
                          "graphics card with AV1."));
  }

  if (!tested) {
    ImGui::TextWrapped(
        T("Welche Encoder gehen, hängt von der Grafikkarte ab. Einmal prüfen; das Ergebnis "
          "bleibt bis zu einem Hardwarewechsel gespeichert.",
          "Available encoders depend on the graphics card. Test once; the result is kept "
          "until the hardware changes."));
  } else if (!chosenOk) {
    ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.35f, 1.0f), "%s",
                       T("Der gespeicherte Encoder ist hier nicht verfügbar.",
                         "The saved encoder is not available here."));
  }

  const bool busy = downloader_ && downloader_->busy();
  ImGui::BeginDisabled(!ffmpeg || !ffmpeg->found || busy || probeBusy_);
  const char* testLabel = probeBusy_ ? T("Wird geprüft ...", "Testing ...")
                          : tested   ? T("Erneut testen", "Test again")
                                     : T("Encoder testen", "Test encoders");
  if (ImGui::Button(testLabel)) probeRequested_ = true;
  Anchor("encodertest");
  ImGui::EndDisabled();
  ImGui::SameLine();
  HelpMarker(T("Kodiert je zwei Testbilder. Die Encoder-Liste des Builds nennt nur, was "
               "einkompiliert ist, nicht was die Hardware kann.",
               "Encodes two test frames each. The build's encoder list only names what was "
               "compiled in, not what the hardware can do."));

  // Two wrapped lines rather than ten. A list of every candidate ran off the
  // bottom of the dialog, which made the result of pressing Test nearly
  // invisible -- and the result is the entire point of pressing it.
  if (ffmpeg && ffmpeg->found && tested) {
    std::string works, fails;
    for (const EncoderInfo& e : ffmpeg->encoders) {
      if (!e.tested) continue;
      std::string& target = e.available ? works : fails;
      if (!target.empty()) target += ", ";
      target += RecordEncoderName((int)e.id);
    }
    if (!works.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.85f, 0.5f, 1.0f));
      ImGui::TextWrapped("%s%s", T("Verwendbar: ", "Usable: "), works.c_str());
      ImGui::PopStyleColor();
    }
    if (!fails.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
      ImGui::TextWrapped("%s%s", T("Geht hier nicht: ", "Not available here: "), fails.c_str());
      ImGui::PopStyleColor();
    }
  }

  const EncoderInfo* selected =
      ffmpeg ? (IsAutoEncoder(rec.encoder) ? nullptr : ffmpeg->Find(rec.encoder)) : nullptr;
  const bool lossless = IsLosslessEncoder(rec.encoder);
  const bool softwareSelected = !lossless && (rec.encoder == RecordEncoder::X264 ||
                                              rec.encoder == RecordEncoder::X265 ||
                                              rec.encoder == RecordEncoder::SvtAv1 ||
                                              (selected && !selected->hardware));
  ImGui::BeginDisabled(!softwareSelected);
  int speed = (int)rec.speed;
  ImGui::SetNextItemWidth(-260.0f);
  if (ComboEnum(T("Geschwindigkeit", "Speed"), &speed, 5, RecordSpeedName)) {
    rec.speed = (RecordSpeed)speed;
  }
  Anchor("speed");
  ImGui::EndDisabled();
  ImGui::SameLine();
  HelpMarker(T("Nur für CPU-Encoder, und nur solange die Voreinstellung unten auf "
               "automatisch steht.",
               "Software encoders only, and only while the preset below is on automatic."));

  // Greyed out where the encoder that would run cannot take it, like the
  // encoder settings below; the line under it says why.
  const EncoderInfo* active =
      selected ? selected : (ffmpeg ? ffmpeg->Resolve(rec.encoder) : nullptr);
  // Lossless is 4:4:4 whatever it says; the setting stays for the others.
  bool fullChroma = rec.fullChroma || lossless;
  ImGui::BeginDisabled(lossless || !active || !active->full8);
  if (ImGui::Checkbox(T("Volle Farbauflösung (4:4:4)", "Full colour resolution (4:4:4)"),
                      &fullChroma)) {
    rec.fullChroma = fullChroma;
  }
  Anchor("fullchroma");
  ImGui::EndDisabled();
  ImGui::SameLine();
  HelpMarker(T("Behält die Farbe jedes Bildpunkts, statt sie über vier zu mitteln. Lohnt "
               "sich bei HDMI- und RGB-Quellen. Größere Dateien; Browser, Handys und viele "
               "Schnittprogramme spielen das nicht ab, VLC und mpv schon.",
               "Keeps the colour of every pixel instead of averaging it over four. Worth it "
               "for HDMI and RGB sources. Larger files; browsers, phones and many editors "
               "cannot play it, VLC and mpv can."));
  // Only where nothing is lost on the way: the card's own bytes are the point.
  // The screen has no other bytes: it is recorded as it comes either way.
  if (cfg().active().capture.source == VideoSource::Card) {
    bool unfiltered = rec.unfiltered && lossless;
    ImGui::BeginDisabled(!lossless);
    if (ImGui::Checkbox(T("Ohne Filter", "Unfiltered"), &unfiltered)) rec.unfiltered = unfiltered;
    Anchor("unfiltered");
    ImGui::EndDisabled();
    ImGui::SameLine();
    HelpMarker(T("Nur verlustfrei: die Bilder der Karte, wie sie ankommen, ohne Deinterlacing, "
                 "Zuschnitt, Skalierung und Filter, mit der Bildrate der Karte. Aus: das Bild, "
                 "wie man es sieht.",
                 "Lossless only: the card's frames as they arrive, without deinterlacing, crop, "
                 "scaling or filters, at the card's frame rate. Off: the picture as you see it."));
  }
  if (lossless) {
    // What lossless costs, in the number that decides whether the disk keeps
    // up -- see the free space on the Recording tab for how long it lasts.
    if (diskBytesPerSecond_ > 1.0) {
      TextDisabledWrapped(
          Format(T("Etwa %s pro Sekunde, %s pro Minute. Die Platte muss das schreiben können.",
                   "About %s a second, %s a minute. The disk has to keep up with that."),
                 FormatBytes((uint64_t)diskBytesPerSecond_).c_str(),
                 FormatBytes((uint64_t)(diskBytesPerSecond_ * 60.0)).c_str())
              .c_str());
    }
    if (cfg().app.recordHdr && hdrSourceTransfer_ != 0 && !(active && active->full10)) {
      TextDisabledWrapped(T("HDR wird hier als SDR aufgenommen: verlustfrei in HDR geht nur mit "
                            "NVENC.",
                            "HDR is recorded as SDR here: lossless HDR takes NVENC."));
    }
  } else if (active && rec.fullChroma) {
    const char* why = nullptr;
    if (!active->fullTested) {
      why = T("Noch nicht auf 4:4:4 geprüft: Encoder erneut testen.",
              "Not tested for 4:4:4 yet: test the encoders again.");
    } else if (!active->full8) {
      why = T("Dieser Encoder kann kein 4:4:4, aufgenommen wird 4:2:0.",
              "This encoder cannot do 4:4:4, it records 4:2:0.");
    } else if (!active->full10 && cfg().app.recordHdr && hdrSourceTransfer_ != 0) {
      why = T("HDR bleibt bei diesem Encoder 4:2:0.", "HDR stays 4:2:0 with this encoder.");
    }
    if (why) TextDisabledWrapped(why);
  }

  // Lossless has nothing to set: no rate to aim for, and NVENC's own preset.
  ImGui::BeginDisabled(lossless);
  DrawEncoderBlock(active);
  ImGui::EndDisabled();

  ImGui::EndDisabled();

}

// ----------------------------------------------------------------- tools tab

void SettingsWindow::DrawVirtualCameraBlock() {
  ImGui::Spacing();
  ImGui::SeparatorText(T("Virtuelle Kamera", "Virtual camera"));
  Anchor("vcam");

  // The registration is a registry read and a look at the file, on setupJobs_
  // every two seconds; the two buttons below ask again at once.
  const double now = ImGui::GetTime();
  if (now - vcamStatusChecked_ > 2.0 && !vcamAsking_) {
    vcamStatusChecked_ = now;
    vcamAsking_ = true;
    setupJobs_.Ask([this] {
      const int status = (int)CameraSink::Status();
      return [this, status] {
        vcamStatus_ = status;
        vcamAsking_ = false;
      };
    });
  }
  const auto status = (CameraSink::Setup)vcamStatus_;

  ImGui::TextWrapped(
      "%s", Format(T("Stellt das Bild als Webcam für andere Programme bereit, etwa OBS, "
                     "Discord, Teams oder den Browser. Die Kamera heißt \"%s\" und bleibt "
                     "nach der Installation in der Geräteliste. Läuft %s nicht, zeigt sie "
                     "einen Hinweis.",
                     "Provides the picture as a webcam to other programs, such as OBS, "
                     "Discord, Teams or a browser. The camera is named \"%s\" and stays in "
                     "the device list once installed. While %s is not running, it shows a "
                     "notice."),
                   CameraSink::DeviceName().c_str(), AppNameUtf8().c_str())
                .c_str());
  ImGui::Spacing();
  if (vcamStatus_ < 0) return;  // not known yet

  if (status != CameraSink::Setup::Installed) {
    ImGui::TextWrapped(
        "%s",
        status == CameraSink::Setup::Stale
            ? Format(T("Die Kameraquelle ist registriert, ihre Datei fehlt aber; vermutlich "
                       "wurde %s verschoben. Erneut installieren behebt das.",
                       "The camera source is registered, but its file is missing; %s was "
                       "probably moved. Install again to fix this."),
                     AppNameUtf8().c_str())
                  .c_str()
            : T("Einmal zu installieren. Jedes Programm, das die Kamera öffnet, lädt ihre "
                "Quelle, daher wird sie systemweit registriert. Dafür braucht es "
                "Administratorrechte, für die Nutzung nicht. Die Quelle steckt im Programm "
                "und wird beim Installieren daneben abgelegt.",
                "Install once. Every program that opens the camera loads its source, so it is "
                "registered system-wide. This needs administrator rights; using it does not. "
                "The source is built into the program and placed next to it on install."));
    ImGui::Spacing();
    if (ImGui::Button(T("Kamera installieren", "Install camera"), ImVec2(Px(200.0f), 0.0f))) {
      virtualCameraRequest_ = 1;
      vcamStatusChecked_ = 0.0;
    }
    Anchor("vcaminstall");
    return;
  }

  bool on = cfg().app.virtualCamera;
  if (ImGui::Checkbox(T("Virtuelle Kamera einschalten", "Turn the virtual camera on"), &on)) {
    cfg().app.virtualCamera = on;
  }
  Anchor("vcamon");

  if (vcamRunning_) {
    if (vcamConsumers_.empty()) {
      ImGui::TextDisabled("%s", T("Läuft, bisher liest niemand mit.",
                                  "Running; nothing is reading it yet."));
    } else {
      ImGui::Spacing();
      // Every reader gets its own row, because every reader gets its own
      // format. OBS taking the source untouched while Discord takes 720p30 is
      // the normal case, not an oddity, and one line could only lie about it.
      if (ImGui::BeginTable("vcamconsumers", 3,
                            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn(T("Programm", "Program"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(T("Format", "Format"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(T("Status", "State"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const CameraSink::Consumer& c : vcamConsumers_) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(c.name.empty() ? "?" : c.name.c_str());
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("PID %u", c.pid);
          ImGui::TableNextColumn();
          if (c.width > 0 && c.height > 0) {
            ImGui::Text("%d x %d @ %.4g%s", c.width, c.height, c.fps, c.wide ? " HDR" : "");
          } else {
            ImGui::TextDisabled("%s", T("noch offen", "not settled"));
          }
          ImGui::TableNextColumn();
          if (c.streaming) {
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "%s",
                               T("liest mit", "reading"));
          } else {
            ImGui::TextDisabled("%s", T("verbunden", "connected"));
          }
        }
        ImGui::EndTable();
      }
    }
  }

  ImGui::Spacing();
  TextDisabledWrapped(T("Ausgegeben wird das Bild wie hier: gleiche Auflösung und Bildrate. "
                        "Programme, die weniger verlangen, bekommen es seitenrichtig "
                        "eingepasst, mit schwarzen Balken. Ein Programm wählt das Format beim "
                        "Öffnen der Kamera; nach einem Quellwechsel die Kamera dort neu "
                        "öffnen.",
                        "The output matches the picture here: same resolution and frame rate. "
                        "Programs requesting less get it fitted with black bars. A program "
                        "picks the format when it opens the camera; after a source change, "
                        "reopen the camera there."));

  ImGui::Spacing();
  if (ImGui::Button(T("Kamera deinstallieren", "Uninstall camera"), ImVec2(Px(200.0f), 0.0f))) {
    virtualCameraRequest_ = 2;
    vcamStatusChecked_ = 0.0;
  }
  Anchor("vcamuninstall");
  ImGui::SameLine();
  ImGui::TextDisabled("%s", T("(fragt wieder nach Administratorrechten)",
                              "(asks for administrator rights again)"));
}

void SettingsWindow::DrawEncoderBlock(const EncoderInfo* encoder) {
  RecordSettings& rec = cfg().record;
  ImGui::Spacing();
  ImGui::SeparatorText(T("Encoder-Einstellungen", "Encoder settings"));
  Anchor("encsettings");

  // Which of these mean anything depends on the encoder. Showing the rest
  // greyed out says more than hiding them: it is the difference between "your
  // card cannot" and "qBlank cannot".
  const Recorder::Family family =
      encoder ? Recorder::FamilyOf(encoder->ffmpegName) : Recorder::Family::Software;
  const bool nvenc = family == Recorder::Family::Nvenc;
  const bool amf = family == Recorder::Family::Amf;
  const bool qsv = family == Recorder::Family::Qsv;

  const char* rateNames[] = {T("Konstant (CBR)", "Constant (CBR)"),
                             T("Variabel (VBR)", "Variable (VBR)"),
                             T("Feste Qualität", "Constant quality")};
  int rate = (int)rec.rateControl;
  ImGui::SetNextItemWidth(-260.0f);
  if (ImGui::Combo(T("Ratensteuerung", "Rate control"), &rate, rateNames, kRateControlCount)) {
    rec.rateControl = (RateControl)rate;
  }
  Anchor("ratecontrol");
  ImGui::SameLine();
  HelpMarker(T("Konstant hält die Datenrate stabil, fürs Streamen. Variabel gibt bewegten "
               "Szenen mehr, bis zum Anderthalbfachen. Feste Qualität ignoriert die "
               "Datenrate; die Dateigröße folgt dem Bild.",
               "Constant keeps the data rate steady, for streaming. Variable gives busy "
               "scenes more, up to 1.5 times the rate. Constant quality ignores the data "
               "rate; the file size follows the picture."));
  if (encoder && encoder->id == RecordEncoder::SvtAv1 &&
      rec.rateControl != RateControl::Quality) {
    TextDisabledWrapped(rec.rateControl == RateControl::Cbr
                            ? T("SVT-AV1 hält die Rate nur in seinem Echtzeitmodus konstant. "
                                "Der kostet bei gleicher Datenrate etwas Qualität.",
                                "SVT-AV1 holds a constant rate only in its real-time mode, "
                                "which costs a little quality at the same data rate.")
                            : T("SVT-AV1 nimmt Variabel nur ohne Obergrenze.",
                                "SVT-AV1 takes Variable only without a ceiling."));
  }

  // Bitrate und Qualitaet sind die beiden Zahlen, die an der Ratensteuerung
  // haengen -- je nach Stellung zaehlt genau eine davon. Beide stehen deshalb
  // hier, direkt darunter, und die gerade nicht zaehlende wird ausgegraut statt
  // versteckt: dasselbe Argument wie oben bei den Encoder-Merkmalen. Ausgegraut
  // sagt "gilt hier nicht", versteckt sagt gar nichts und laesst die Seite
  // ausserdem springen.
  const bool byQuality = rec.rateControl == RateControl::Quality;

  ImGui::BeginDisabled(byQuality);
  // Logarithmisch, weil die Skala sonst am falschen Ende genau ist. 1000 bis
  // 100000 linear auf die Bahnbreite sind rund 330 kbit je Bildpunkt -- man
  // findet einen Wert, treffen kann man ihn nicht. Schlimmer ist die
  // Verteilung: der Bereich, in dem eine Quelle dieser Karte tatsaechlich
  // landet, 1 bis 10 Mbit, belegt neun Prozent der Bahn, die restlichen
  // neunzig sind fuer eine Karte gedacht, die es hier nicht gibt.
  // Logarithmisch bekommt der benutzte Bereich die halbe Bahn.
  ImGui::SetNextItemWidth(-360.0f);
  ImGui::SliderInt("##bitrate", &rec.bitrateKbps, 1000, 100000, "%d kbit/s",
                   ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
  Anchor("bitrate");
  ResetOnRightClick(rec.bitrateKbps, kRecord.bitrateKbps);
  ImGui::SameLine();
  // Und ein Feld daneben, weil ein Regler eine Zahl nur ungefaehr trifft und
  // man hier oft eine bestimmte will. Strg+Klick auf den Regler tut dasselbe,
  // aber das weiss niemand, dem es niemand sagt -- ein sichtbares Feld schon.
  ImGui::SetNextItemWidth(Px(96.0f));
  if (ImGui::InputInt(T("Bitrate", "Bitrate"), &rec.bitrateKbps, 0, 0,
                      ImGuiInputTextFlags_CharsDecimal)) {
    rec.bitrateKbps = Clamp(rec.bitrateKbps, 1000, 100000);
  }
  ImGui::SameLine();
  HelpMarker(byQuality
                 ? T("Bei fester Qualität ohne Wirkung — dort entscheidet der Regler "
                     "darunter, wie groß die Datei wird.",
                     "Ignored at constant quality — there the setting below decides how "
                     "large the file gets.")
                 : T("In kbit/s. Für 720x576 sind 6000 bis 12000 der übliche Bereich. Die "
                     "Zahl lässt sich rechts auch eintippen.",
                     "In kbit/s. For 720x576 the usual range is 6000 to 12000. The number "
                     "can also be typed on the right."));
  ImGui::EndDisabled();

  ImGui::BeginDisabled(!byQuality);
  ImGui::SetNextItemWidth(-260.0f);
  ImGui::SliderInt(T("Qualität", "Quality"), &rec.qualityLevel, 1, 51,
                   T("%d (kleiner = besser)", "%d (lower is better)"),
                   ImGuiSliderFlags_AlwaysClamp);
  Anchor("quality");
  ResetOnRightClick(rec.qualityLevel, kRecord.qualityLevel);
  ImGui::SameLine();
  HelpMarker(byQuality
                 ? T("Die Skala unterscheidet sich je Encoder leicht. Üblich sind 18 bis 24; "
                     "darunter wächst die Datei schnell bei kaum sichtbarem Gewinn.",
                     "The scale differs slightly between encoders. 18 to 24 is typical; lower "
                     "values grow the file quickly for little visible gain.")
                 : T("Nur bei fester Qualität. Sonst entscheidet die Bitrate darüber.",
                     "Constant quality only. Otherwise the bitrate decides."));
  ImGui::EndDisabled();

  const char* presetNames[] = {T("Automatisch", "Automatic"),
                               T("Am schnellsten", "Fastest"),
                               T("Schneller", "Faster"),
                               T("Schnell", "Fast"),
                               T("Mittel", "Medium"),
                               T("Langsam", "Slow"),
                               T("Langsamer", "Slower"),
                               T("Am langsamsten", "Slowest")};
  int preset = (int)rec.preset;
  ImGui::SetNextItemWidth(-260.0f);
  if (ImGui::Combo(T("Voreinstellung", "Preset"), &preset, presetNames, kEncoderPresetCount)) {
    rec.preset = (EncoderPreset)preset;
  }
  Anchor("preset");
  ImGui::SameLine();
  HelpMarker(amf ? T("AMF kennt nur drei Stufen; die sieben hier fallen darauf zusammen.",
                     "AMF has three steps; these seven fold onto them.")
                 : T("Langsamer heißt besseres Bild bei gleicher Datenrate — und mehr Last. "
                     "Automatisch überlässt es dem Encoder.",
                     "Slower means a better picture at the same data rate, and more load. "
                     "Automatic leaves it to the encoder."));

  ImGui::BeginDisabled(!nvenc && !amf);
  const char* tuneNames[] = {T("Automatisch", "Automatic"), T("Qualität", "Quality"),
                             T("Geringe Latenz", "Low latency")};
  int tune = (int)rec.tune;
  ImGui::SetNextItemWidth(-260.0f);
  if (ImGui::Combo(T("Abstimmung", "Tuning"), &tune, tuneNames, kEncoderTuneCount)) {
    rec.tune = (EncoderTune)tune;
  }
  Anchor("tune");
  ImGui::EndDisabled();

  ImGui::BeginDisabled(!nvenc);
  const char* passNames[] = {T("Automatisch", "Automatic"), T("Aus", "Off"),
                             T("Zwei (Viertelauflösung)", "Two (quarter resolution)"),
                             T("Zwei (volle Auflösung)", "Two (full resolution)")};
  int pass = (int)rec.multipass;
  ImGui::SetNextItemWidth(-260.0f);
  if (ImGui::Combo(T("Durchläufe", "Multipass"), &pass, passNames, kMultipassCount)) {
    rec.multipass = (Multipass)pass;
  }
  Anchor("multipass");
  ImGui::SameLine();
  HelpMarker(T("Ein zweiter Durchlauf trifft die Datenrate genauer, vor allem nah am Limit. "
               "Nur NVENC kann das.",
               "A second pass hits the data rate more accurately, especially near the "
               "ceiling. NVENC only."));
  ImGui::EndDisabled();

  ImGui::BeginDisabled(!nvenc || encoder->ffmpegName.compare(0, 4, "h264") == 0);
  ImGui::Checkbox(T("Alle Encoder-Chips", "All encoder chips"), &rec.allEncoderChips);
  Anchor("encoderchips");
  ImGui::SameLine();
  HelpMarker(T("Teilt jedes Bild auf die NVENC-Chips der Grafikkarte auf. Mit zwei (etwa RTX "
               "4080) über 60 % mehr Bilder je Sekunde, bei gleicher Datenrate etwas weniger "
               "Qualität. Mit einem Chip ohne Wirkung. Nur H.265 und AV1.",
               "Splits each frame across the graphics card's NVENC chips. With two (e.g. RTX "
               "4080), over 60 % more frames per second, with slightly lower quality at the "
               "same data rate. No effect with one chip. H.265 and AV1 only."));
  ImGui::EndDisabled();

  ImGui::BeginDisabled(!nvenc && !amf && !qsv);
  ImGui::Checkbox(T("Vorausschau", "Look-ahead"), &rec.lookAhead);
  Anchor("lookahead");
  ImGui::SameLine();
  HelpMarker(T("Der Encoder schaut voraus und verteilt die Bits besser. Kostet Verzögerung: "
               "Beim Aufnehmen egal, beim Streamen nicht.",
               "The encoder looks ahead and distributes bits better. Adds delay, which "
               "matters for streaming, not for recording."));
  // QuickSync takes a depth for H.265 and AV1; its H.264 look-ahead has none.
  const bool depthKnown = nvenc || (qsv && encoder->ffmpegName.compare(0, 4, "h264") != 0);
  ImGui::BeginDisabled(!depthKnown || !rec.lookAhead);
  const char* depthNames[] = {"8", "16", "24", "32"};
  int depth = rec.lookAheadFrames / 8 - 1;
  ImGui::SetNextItemWidth(-260.0f);
  if (ImGui::Combo(T("Bilder voraus", "Frames ahead"), &depth, depthNames, 4)) {
    rec.lookAheadFrames = (depth + 1) * 8;
  }
  Anchor("lookaheadframes");
  ImGui::SameLine();
  HelpMarker(T("Wie weit der Encoder vorausschaut. Mehr Bilder verteilen die Bits besser und "
               "kosten Arbeitsspeicher, bei NVENC und 1440p rund 15 MB je Bild über 8.",
               "How far the encoder looks ahead. More frames spread the bits better and cost "
               "memory, with NVENC at 1440p about 15 MB per frame over 8."));
  ImGui::EndDisabled();
  ImGui::Checkbox(T("Adaptive Quantisierung", "Adaptive quantisation"), &rec.adaptiveQuant);
  Anchor("aq");
  ImGui::SameLine();
  HelpMarker(T("Gibt Flächen und Verläufen, wo Artefakte auffallen, mehr Bits und unruhigen "
               "Bereichen weniger.",
               "Gives more bits to flat areas and gradients, where artefacts show, and fewer "
               "to busy areas."));
  ImGui::EndDisabled();

  if (!nvenc && !amf && !qsv) {
    TextDisabledWrapped(T("Der Softwareencoder nimmt nur Ratensteuerung und "
                          "Voreinstellung an.",
                          "The software encoder takes only rate control and preset."));
  }
}


// --------------------------------------------------------------- hotkeys tab

void SettingsWindow::DrawHotkeysTab() {
  ImGui::Spacing();
  ImGui::TextWrapped(T("Zum Ändern die Taste anklicken und die neue Kombination drücken. "
                       "Esc bricht ab, Entf löscht die Belegung.",
                       "Click a shortcut and press the new combination. Esc cancels, Delete "
                       "clears the binding."));
  ImGui::Spacing();

  Hotkeys& keys = cfg().hotkeys;
  GlobalHotkeys& global = cfg().globalHotkeys;

  // One button: the window's key for an action, or its system-wide one.
  const auto keyButton = [this, &keys, &global](int i, bool system) {
    HotkeyBinding* set = system ? global.items : keys.items;
    const bool capturing = (captureAction_ == i && captureGlobal_ == system);
    const std::string label = (capturing ? std::string(T("Taste drücken ...", "Press a key ..."))
                                         : HotkeyText(set[i])) +
                              (system ? "##ghk" : "##hk") + std::to_string(i);

    // A duplicate is flagged rather than refused: which of the two the user
    // meant to keep is not something this dialog can know. A system-wide key
    // also clashes with the window's key for another action: it wins there.
    bool clash = false;
    if (set[i].bound()) {
      for (int j = 0; j < (int)HotkeyAction::Count && !clash; ++j) {
        if (j != i && (set[j] == set[i] || (system && keys.items[j] == set[i]))) clash = true;
      }
    }
    const bool refused = system && !clash && global.on && set[i].bound() && globalRefused_ &&
                         globalRefused_[i];
    if (clash || refused) {
      ImGui::PushStyleColor(ImGuiCol_Text, refused ? ImVec4(1.0f, 0.42f, 0.38f, 1.0f)
                                                   : ImVec4(0.95f, 0.6f, 0.35f, 1.0f));
    }
    if (ImGui::Button(label.c_str(), ImVec2(-1.0f, 0.0f))) {
      captureAction_ = capturing ? -1 : i;
      captureGlobal_ = system;
    }
    if (clash || refused) {
      ImGui::PopStyleColor();
      ImGui::SetItemTooltip("%s", clash ? T("Mehrfach belegt.", "Bound more than once.")
                                        : T("Von einem anderen Programm belegt.",
                                            "Taken by another program."));
    }
  };

  // The system-wide keys sit beside the window's.
  if (ImGui::BeginTable("hotkeys", 3,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn(T("Aktion", "Action"), ImGuiTableColumnFlags_WidthStretch, 0.5f);
    ImGui::TableSetupColumn(T("Taste", "Shortcut"), ImGuiTableColumnFlags_WidthStretch, 0.25f);
    ImGui::TableSetupColumn(T("Systemweit", "System-wide"), ImGuiTableColumnFlags_WidthStretch,
                            0.25f);
    ImGui::TableHeadersRow();
    for (int i = 0; i < (int)HotkeyAction::Count; ++i) {
      const HotkeyAction action = (HotkeyAction)i;
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(HotkeyActionName(action));
      ImGui::TableSetColumnIndex(1);
      keyButton(i, false);
      ImGui::TableSetColumnIndex(2);
      ImGui::BeginDisabled(!global.on);
      keyButton(i, true);
      ImGui::EndDisabled();
    }
    ImGui::EndTable();
  }

  ImGui::Spacing();
  ImGui::Checkbox(T("Systemweite Tasten", "System-wide shortcuts"), &global.on);
  Anchor("globalkeys");
  ImGui::SameLine();
  HelpMarker(T("Die Tasten der Spalte Systemweit wirken überall, auch im Spiel. Solange "
               "qBlank läuft, bekommt kein anderes Programm die Kombination. Eine bereits "
               "vergebene steht rot da.",
               "Keys in the System-wide column work everywhere, including in games. While "
               "qBlank runs, no other program receives the combination. One already in use is "
               "shown in red."));

  // Both columns; whether the system-wide keys are held stays as it is.
  if (ImGui::Button(T("Alle zurücksetzen", "Reset all"))) {
    cfg().hotkeys = Hotkeys();
    const GlobalHotkeys fresh;
    std::copy(std::begin(fresh.items), std::end(fresh.items), global.items);
    captureAction_ = -1;
  }
  Anchor("keysreset");

  ImGui::Spacing();
  TextDisabledWrapped(T("Fest belegt: Esc verlässt das Vollbild und schließt Dialoge, "
                        "Strg+1 bis Strg+9 wählen ein Profil, Alt+F4 beendet.",
                        "Fixed: Esc leaves fullscreen and closes dialogs, Ctrl+1 to Ctrl+9 pick "
                        "a profile, Alt+F4 quits."));
}

void SettingsWindow::OfferKey(Key key, bool ctrl, bool shift, bool alt) {
  if (captureAction_ < 0 || captureAction_ >= (int)HotkeyAction::Count) return;

  if (key == Key::Escape) {  // cancel, change nothing
    captureAction_ = -1;
    return;
  }
  HotkeyBinding* set = captureGlobal_ ? cfg().globalHotkeys.items : cfg().hotkeys.items;
  if (key == Key::Delete || key == Key::Backspace) {  // unbind
    set[captureAction_] = HotkeyBinding{};
    captureAction_ = -1;
    return;
  }
  // A modifier on its own is not a shortcut yet -- keep waiting so the user can
  // hold Ctrl and then reach for the actual key.
  if (IsReservedKey(key)) return;
  // Nor is a key that has no name to be stored under.
  if (key == Key::None) return;

  HotkeyBinding binding;
  binding.key = key;
  binding.ctrl = ctrl;
  binding.shift = shift;
  binding.alt = alt;
  set[captureAction_] = binding;
  captureAction_ = -1;
}

// -------------------------------------------------------------- profiles tab

void SettingsWindow::DrawProfilesTab(const DeviceProbeResult& caps) {
  Config& c = cfg();
  ImGui::Spacing();
  ImGui::TextWrapped(
      T("Ein Profil speichert Gerät, Eingang, Videonorm, Format und alle Bild- und "
        "Toneinstellungen. Am besten eines pro Konsole, da ein SNES über Composite andere "
        "Filter braucht als eine Switch über HDMI.\n\n"
        "Umschalten mit Strg+Zahl.",
        "A profile stores device, input, video standard, format and all picture and audio "
        "settings. Ideally one per console, as a SNES over composite needs different filters "
        "than a Switch over HDMI.\n\n"
        "Switch with Ctrl+number."));
  ImGui::Spacing();

  // Nur was der Decoder wirklich kann, und nur wenn ueberhaupt einer im Spiel
  // ist. An einem HDMI-Eingang gibt es keine Videonorm, nach der sich etwas
  // richten koennte.
  const bool canAutoSelect = caps.availableStandards != 0 && analogueSource_;
  const float listHeight = ImGui::GetContentRegionAvail().y -
                           ImGui::GetFrameHeightWithSpacing() * (canAutoSelect ? 4.4f : 1.6f);
  if (ImGui::BeginListBox("##profiles", ImVec2(-1.0f, std::max(120.0f, listHeight)))) {
    for (int i = 0; i < (int)c.profiles.size(); ++i) {
      const bool selected = (i == c.activeProfile);
      std::string label = Format("%d.  %s", i + 1, c.profiles[(size_t)i].name.c_str());
      if (ImGui::Selectable(label.c_str(), selected)) c.activeProfile = i;
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndListBox();
  }

  if (ImGui::Button(T("Neu", "New"))) {
    Profile p;
    p.name = Format(T("Profil %d", "Profile %d"), (int)c.profiles.size() + 1);
    c.profiles.push_back(p);
    c.activeProfile = (int)c.profiles.size() - 1;
  }
  Anchor("profnew");
  ImGui::SameLine();
  // Der uebliche Weg, und deshalb steht er hier statt eines "Duplizieren".
  //
  // Eine neue Konsole anzuschliessen heisst fast nie, von vorn anzufangen: man
  // aendert das, was anders ist, und will das Ergebnis behalten. Ein leeres
  // Profil zwingt dagegen dazu, alles noch einmal einzustellen, was ohnehin
  // schon richtig stand.
  if (ImGui::Button(T("Aktuelles sichern als ...", "Save current as ..."))) {
    renameTarget_ = -1;  // -1 heißt: ein neues anlegen statt umbenennen
    renameBuffer_[0] = '\0';
    namePopupFocus_ = true;
    ImGui::OpenPopup("rename_profile");
  }
  Anchor("profsave");
  WrappedTooltip(T("Legt ein Profil aus den aktuellen Einstellungen an: Gerät, Eingang, "
                   "Format, Bild und Ton. Danach einen Namen eintippen, etwa den der "
                   "Konsole.\n\n"
                   "Das bisherige Profil bleibt unverändert.",
                   "Creates a profile from the current settings: device, input, format, "
                   "picture and audio. Then type a name, e.g. the console's.\n\n"
                   "The current profile is not changed."));
  ImGui::SameLine();
  if (ImGui::Button(T("Umbenennen", "Rename"))) {
    renameTarget_ = c.activeProfile;
    std::snprintf(renameBuffer_, sizeof(renameBuffer_), "%s", c.active().name.c_str());
    namePopupFocus_ = true;
    ImGui::OpenPopup("rename_profile");
  }
  Anchor("profrename");
  ImGui::SameLine();
  ImGui::BeginDisabled(c.profiles.size() <= 1);
  if (ImGui::Button(T("Löschen", "Delete"))) {
    c.profiles.erase(c.profiles.begin() + c.activeProfile);
    c.activeProfile = Clamp(c.activeProfile, 0, (int)c.profiles.size() - 1);
  }
  Anchor("profdelete");
  ImGui::EndDisabled();

  // Welche erkannte Videonorm dieses Profil holt.
  //
  // Es steht hier und nicht bei der Videonorm auf der Quellenseite, obwohl es
  // dieselbe Liste ist: dort beantwortet die Norm die Frage "was soll die Karte
  // einstellen", hier die Frage "wann bin ich gemeint". Das ist eine Aussage
  // ueber das Profil, und die gehoert dorthin, wo die Profile stehen.
  if (canAutoSelect) {
    Profile& prof = c.active();
    ImGui::Spacing();
    ImGui::SeparatorText(T("Von selbst wählen", "Choose automatically"));

    const char* never = T("Nie — nur von Hand", "Never — by hand only");
    const int chosen =
        prof.autoSelectStandard != 0 ? VideoStandardGroupOf(prof.autoSelectStandard) : -1;
    // Eine feste Norm wird nie gesucht, also wird bei ihr auch nie etwas
    // erkannt. Die Regel liefe dann ins Leere -- und das faellt niemandem auf,
    // solange sie einfach dasteht.
    const bool pinned = prof.capture.videoStandard != -1;
    ImGui::BeginDisabled(pinned);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##autoselect",
                          chosen >= 0 ? VideoStandardGroupName(chosen) : never)) {
      if (ImGui::Selectable(never, chosen < 0)) prof.autoSelectStandard = 0;
      ImGui::Separator();
      for (int i = 0; i < VideoStandardGroupCount(); ++i) {
        const long value = VideoStandardGroupPick(i, caps.availableStandards);
        if (value == 0) continue;
        if (ImGui::Selectable(VideoStandardGroupName(i), chosen == i)) {
          prof.autoSelectStandard = value;
        }
        WrappedTooltip(VideoStandardGroupHint(i));
      }
      ImGui::EndCombo();
    }
    Anchor("autoselect");
    ImGui::EndDisabled();

    if (pinned) {
      ImGui::TextWrapped(
          T("Nicht möglich, solange dieses Profil eine feste Videonorm hat: Erkannt wird nur "
            "bei „Automatisch“.",
            "Not possible while this profile uses a fixed video standard: detection only runs "
            "on \"Automatic\"."));
    } else {
      ImGui::TextWrapped(
          "%s",
          Format(
              T("Wird diese Norm erkannt, schaltet %s automatisch auf dieses Profil, bei "
                "gleichem Gerät und Eingang und nie während einer Aufnahme. Ein von Hand "
                "gewähltes Profil bleibt, bis sich die Quelle ändert.",
                "When this standard is detected, %s switches to this profile automatically, "
                "on the same device and input and never during a recording. A profile chosen "
                "by hand stays until the source changes."),
              AppNameUtf8().c_str())
              .c_str());
    }
  }

  if (ImGui::BeginPopupModal("rename_profile", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    const bool creating = renameTarget_ < 0;
    ImGui::TextUnformatted(creating ? T("Wie soll das Profil heißen?", "What should it be called?")
                                    : T("Profil umbenennen", "Rename profile"));
    if (creating) {
      ImGui::TextDisabled("%s", T("Zum Beispiel der Name der Konsole.",
                                  "The console's name, for instance."));
    }
    // Der Cursor steht gleich im Feld: wer hier landet, will tippen, nicht erst
    // hineinklicken. Nur im ersten Bild des Popups, sonst faenge es die Eingabe
    // jedes Mal neu ein.
    if (namePopupFocus_) {
      ImGui::SetKeyboardFocusHere();
      namePopupFocus_ = false;
    }
    ImGui::SetNextItemWidth(Px(320.0f));
    const bool entered = ImGui::InputText("##name", renameBuffer_, sizeof(renameBuffer_),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::Spacing();
    if (entered || ImGui::Button("OK", ImVec2(Px(100.0f), 0))) {
      const std::string trimmed = Trim(renameBuffer_);
      if (!trimmed.empty()) {
        if (creating) {
          // Das gerade aktive Profil ist der aktuelle Zustand -- die
          // Einstellungen wirken sofort und werden dorthin geschrieben.
          Profile copy = c.active();
          copy.name = trimmed;
          c.profiles.push_back(copy);
          c.activeProfile = (int)c.profiles.size() - 1;
        } else if (renameTarget_ < (int)c.profiles.size()) {
          c.profiles[(size_t)renameTarget_].name = trimmed;
        }
      }
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Abbrechen", "Cancel"), ImVec2(Px(100.0f), 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
}

}  // namespace cap
