#include "ui/startup_notices.h"

#include "app_name.h"
#include "desktop.h"
#include "files.h"
#include "i18n.h"
#include "imgui.h"
#include "platform.h"
#include "record/ffmpeg_locator.h"

namespace cap {

StartupNotices::StartupNotices(Updater& updater, FfmpegDownloader& ffmpeg, Host& host)
    : updater_(updater), ffmpeg_(ffmpeg), host_(host) {}

void StartupNotices::QueueCrashNotice(const UnfinishedSession& session) {
  unfinishedSession_ = session;
  crashNoticeQueued_ = true;
}

void StartupNotices::QueueWelcome(bool startMenu, bool desktop) {
  welcomeStartMenu_ = startMenu;
  welcomeDesktop_ = desktop;
  welcomeQueued_ = welcomeStartMenu_ || welcomeDesktop_;
}

void StartupNotices::OpenReleasePage(const UpdateStatus& status) { OpenUrl(ReleasePageUrl(status)); }

void StartupNotices::DrawUpdatePrompt() {
  // The startup check runs on its own thread, so the result turns up a second or
  // two in. Raised once per session and never again, whatever the user does with
  // it -- a notice that keeps coming back is an advertisement.
  if (!updatePromptRaised_ && updater_.status().announce &&
      updater_.status().state == UpdateStatus::State::Available) {
    updatePromptRaised_ = true;
    updatePromptQueued_ = true;
  }

  const char* id = T("Update verfügbar###app_update", "Update available###app_update");
  // Not over the crash notice or the welcome: opened at the same level, it
  // would replace them.
  if (updatePromptQueued_ && !ImGui::IsPopupOpen("###crash_notice") &&
      !ImGui::IsPopupOpen("###welcome") && !ImGui::IsPopupOpen("###ffmpeg_update")) {
    ImGui::OpenPopup(id);
    updatePromptQueued_ = false;
  }

  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                 viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                          ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

  const UpdateStatus st = updater_.status();
  ImGui::Text(T("%s %s ist verfügbar.", "%s %s is available."), AppNameUtf8().c_str(),
              st.latestVersion.c_str());
  ImGui::TextDisabled(T("Installiert ist %s.", "This build is %s."), Updater::currentVersion());
  ImGui::Spacing();

  switch (st.state) {
    case UpdateStatus::State::Downloading:
      ImGui::TextDisabled("%s", T("wird geladen ...", "downloading ..."));
      break;
    case UpdateStatus::State::Ready:
      ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "%s",
                         T("Eingesetzt. Ein Neustart übernimmt sie.",
                           "Installed. A restart picks it up."));
      break;
    case UpdateStatus::State::Failed:
      ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.35f, 1.0f), "%s", UpdateErrorText(st).c_str());
      break;
    default:
      break;
  }

  ImGui::Spacing();
  const float buttonWidth = 130.0f * host_.UiScale();

  if (st.state == UpdateStatus::State::Ready) {
    if (ImGui::Button(T("Jetzt neu starten", "Restart now"), ImVec2(buttonWidth, 0))) {
      host_.Restart();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Später", "Later"), ImVec2(buttonWidth, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }

  ImGui::BeginDisabled(updater_.busy());
  if (ImGui::Button(T("Installieren", "Install"), ImVec2(buttonWidth, 0))) {
    updater_.InstallAsync();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button(T("Später", "Later"), ImVec2(buttonWidth, 0))) ImGui::CloseCurrentPopup();
  ImGui::SameLine();
  if (ImGui::Button(T("Was ist neu", "What is new"), ImVec2(buttonWidth, 0))) {
    // The release page, not the Updates tab. The tab shows the notes trimmed to
    // something that fits; the page has the whole of them, the file, and the
    // history above it.
    OpenReleasePage(st);
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void StartupNotices::DrawFfmpegPrompt() {
  // Same rules as for qBlank itself: from the check at startup only, and once.
  if (!ffmpegPromptRaised_ && ffmpeg_.announced() && ffmpeg_.updateAvailable() &&
      ffmpeg_.state() == FfmpegDownloader::State::Done) {
    ffmpegPromptRaised_ = true;
    ffmpegPromptQueued_ = true;
  }

  const char* id = T("ffmpeg-Update verfügbar###ffmpeg_update",
                     "ffmpeg update available###ffmpeg_update");
  if (ffmpegPromptQueued_ && !ImGui::IsPopupOpen("###crash_notice") &&
      !ImGui::IsPopupOpen("###welcome") && !ImGui::IsPopupOpen("###app_update")) {
    ImGui::OpenPopup(id);
    ffmpegPromptQueued_ = false;
  }

  if (ffmpegUpdating_ && !ffmpeg_.busy() && !ImGui::IsPopupOpen(id)) {
    ffmpegUpdating_ = false;
    host_.Toast(ffmpeg_.message());
  }

  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                 viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                          ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

  const std::string remote = ffmpeg_.remoteVersion();
  const FfmpegDownloader::State state = ffmpeg_.state();
  const bool busy = ffmpeg_.busy();
  const bool installed = ffmpegUpdating_ && state == FfmpegDownloader::State::Done;
  const float buttonWidth = 130.0f * host_.UiScale();

  if (!installed) {
    const std::string current = ffmpeg_.installedVersion();
    ImGui::Text(T("ffmpeg %s ist verfügbar.", "ffmpeg %s is available."), remote.c_str());
    ImGui::TextDisabled(T("Installiert ist %s.", "%s is installed."), current.c_str());
    if (IsMajorFfmpegStep(remote, current)) {
      ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
      ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1.0f), "%s",
                         T("Neue Hauptversion: Dort fallen manchmal Optionen weg. Danach "
                           "einmal kurz zur Probe aufnehmen.",
                           "A new major version: options sometimes go away there. Make a "
                           "short test recording afterwards."));
      ImGui::PopTextWrapPos();
    }
  }
  ImGui::Spacing();

  if (busy) {
    const float p = ffmpeg_.progress();
    // As wide as the two buttons: a window that sizes itself has no width of
    // its own for the bar to fill.
    if (p >= 0.0f) {
      ImGui::ProgressBar(p, ImVec2(buttonWidth * 2.0f + ImGui::GetStyle().ItemSpacing.x, 0.0f));
    }
    ImGui::TextDisabled("%s", ffmpeg_.message().c_str());
  } else if (ffmpegUpdating_) {
    const bool failed = state == FfmpegDownloader::State::Failed;
    ImGui::TextColored(failed ? ImVec4(0.95f, 0.5f, 0.35f, 1.0f) : ImVec4(0.55f, 0.85f, 0.55f, 1.0f),
                       "%s", ffmpeg_.message().c_str());
  }

  ImGui::Spacing();

  if (installed) {
    if (ImGui::Button("OK", ImVec2(buttonWidth, 0))) {
      ffmpegUpdating_ = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    return;
  }

  ImGui::BeginDisabled(busy);
  if (ImGui::Button(T("Aktualisieren", "Update"), ImVec2(buttonWidth, 0))) {
    // The one being replaced may be running; the download steps round it.
    if (ffmpeg_.Start(OwnFfmpegFolder())) ffmpegUpdating_ = true;
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  // While it runs, "Later" only closes the window: the download carries on and
  // says how it went when it is done.
  if (ImGui::Button(busy ? T("Ausblenden", "Hide") : T("Später", "Later"),
                    ImVec2(buttonWidth, 0))) {
    if (!busy) ffmpegUpdating_ = false;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void StartupNotices::DrawCrashNotice() {
  const char* id = T("Nicht normal beendet###crash_notice", "Did not close normally###crash_notice");
  if (crashNoticeQueued_) {
    ImGui::OpenPopup(id);
    crashNoticeQueued_ = false;
  }

  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                 viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                          ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

  const std::string when = ShortDateAndTime(unfinishedSession_.started);

  ImGui::Text(T("%s wurde beim letzten Mal nicht normal beendet.",
                "%s did not close normally last time."),
              AppNameUtf8().c_str());
  if (!when.empty()) {
    ImGui::TextDisabled(T("Sitzung vom %s, Version %s", "Session from %s, version %s"),
                        when.c_str(), unfinishedSession_.version.c_str());
  }
  ImGui::Spacing();
  ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
  ImGui::TextUnformatted(T("Die Sitzung hat im Protokoll keine Endzeile: meist ein Absturz, "
                           "sonst Task-Manager oder Stromausfall. Was zuletzt passiert ist, "
                           "steht dort über der nachgetragenen Zeile \"ended abnormally\".",
                           "The session has no end line in the log: usually a crash, otherwise "
                           "the task manager or a power cut. What happened last is right above "
                           "the \"ended abnormally\" line added there now."));
  ImGui::PopTextWrapPos();
  ImGui::Spacing();

  const float buttonWidth = 130.0f * host_.UiScale();
  if (ImGui::Button(T("Protokoll öffnen", "Open log"), ImVec2(buttonWidth, 0))) {
    OpenFile(OwnFile("log"));
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (ImGui::Button("OK", ImVec2(buttonWidth, 0))) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

void StartupNotices::DrawWelcome() {
  const char* id = T("Erster Start###welcome", "First start###welcome");
  if (welcomeQueued_) {
    ImGui::OpenPopup(id);
    welcomeQueued_ = false;
  }

  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                 viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                          ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

  ImGui::Text(T("Sieht so aus, als wäre das dein erster Start mit %s.",
                "Looks like this is your first time using %s."),
              AppNameUtf8().c_str());
  ImGui::TextUnformatted(T("Verknüpfungen anlegen?", "Create shortcuts?"));
  ImGui::Spacing();
  if (welcomeStartMenu_) {
    ImGui::Checkbox(T("Im Startmenü", "In the Start menu"), &welcomeWantStartMenu_);
  }
  if (welcomeDesktop_) {
    ImGui::Checkbox(T("Auf dem Desktop", "On the desktop"), &welcomeWantDesktop_);
  }
  ImGui::Spacing();
  ImGui::TextDisabled("%s", T("Geht auch später, in den Einstellungen unter Anzeige.",
                              "Also possible later, in the settings under Display."));
  ImGui::Spacing();

  const bool startMenu = welcomeStartMenu_ && welcomeWantStartMenu_;
  const bool desktop = welcomeDesktop_ && welcomeWantDesktop_;
  const float buttonWidth = 130.0f * host_.UiScale();
  ImGui::BeginDisabled(!startMenu && !desktop);
  if (ImGui::Button(T("Anlegen", "Create"), ImVec2(buttonWidth, 0))) {
    bool ok = true;
    if (startMenu) ok = CreateShortcut(ShortcutPlace::StartMenu) && ok;
    if (desktop) ok = CreateShortcut(ShortcutPlace::Desktop) && ok;
    if (!ok) {
      host_.Toast(T("Die Verknüpfung ließ sich nicht anlegen.", "The shortcut could not be created."));
    }
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button(T("Nein danke", "No thanks"), ImVec2(buttonWidth, 0))) {
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

}  // namespace cap
