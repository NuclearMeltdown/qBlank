// What the parts split off App ask of it. Each one talks to the program
// through a small Host interface; these are App's answers.

#include "app.h"

namespace cap {

float App::NoticesHost::UiScale() const { return app_.uiScale_; }
void App::NoticesHost::Toast(const std::string& text) { app_.Toast(text); }
void App::NoticesHost::Restart() { app_.QuitAndRestart(); }

void App::CropHost::Toast(const std::string& text) { app_.Toast(text); }
void App::CropHost::OpenSettings() { app_.OpenSettings({}); }
void App::CropHost::CloseSettings() { app_.settings_.Close(); }

bool App::RecordingHost::CaptureRunning() const {
  return app_.captureState_ == CaptureState::Running;
}
std::filesystem::path App::RecordingHost::ResolveOutputFolder(
    std::string* configured, const std::filesystem::path& fallback) {
  return app_.ResolveOutputFolder(configured, fallback);
}
void App::RecordingHost::DropViewAids() {
  if (app_.frozen_) {
    app_.frozen_ = false;
    app_.delayLine_.Clear();
  }
  app_.compare_ = false;
  app_.bypass_ = false;
}
void App::RecordingHost::Toast(const std::string& text, const std::filesystem::path& file) {
  app_.Toast(text, file);
}

bool App::StandardSearchHost::CaptureRunning() const {
  return app_.captureState_ == CaptureState::Running;
}
bool App::StandardSearchHost::SourceIsAnalogue() const { return app_.SourceIsAnalogue(); }
AnalogConnector App::StandardSearchHost::ResolvedConnector() const {
  return app_.ResolvedConnector();
}
bool App::StandardSearchHost::ConnectorHasColourCarrier() const {
  return app_.ConnectorHasColourCarrier();
}
long App::StandardSearchHost::AppliedVideoStandard() const {
  return app_.applied_.videoStandard;
}
bool App::StandardSearchHost::ReleaseStandardBoundFormat(int newLines) {
  return app_.ReleaseStandardBoundFormat(newLines);
}
bool App::StandardSearchHost::StartCapture(std::string* error) {
  return app_.StartCapture(error);
}
void App::StandardSearchHost::Toast(const std::string& text) { app_.Toast(text); }

}  // namespace cap
