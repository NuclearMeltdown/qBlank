#include "camera_sink.h"

#include "text_win32.h"
#include "vcam/vcam_shared.h"
#include "vcam/virtual_camera.h"

namespace cap {

// On Windows the camera is a DirectShow source filter, and everything qBlank has
// to do for it -- the shared section, the control block, the consumer slots --
// is VirtualCamera. That class stays as it is: it is the one place in the program
// where the shape of a Windows interface decides the shape of the code, and
// pretending otherwise would only move the DirectShow terms into a header that
// the rest of the program reads.
struct CameraSink::Impl {
  VirtualCamera camera;
};

CameraSink::CameraSink() : impl_(std::make_unique<Impl>()) {}

CameraSink::~CameraSink() = default;

CameraSink::Setup CameraSink::Status() {
  switch (VirtualCamera::Status()) {
    case VirtualCamera::Install::Installed:
      return Setup::Installed;
    case VirtualCamera::Install::Stale:
      return Setup::Stale;
    default:
      return Setup::Missing;
  }
}

bool CameraSink::InstallSystemWide(std::string* error) {
  return VirtualCamera::InstallSource(error);
}

bool CameraSink::RemoveSystemWide(std::string* error) {
  return VirtualCamera::UninstallSource(error);
}

void CameraSink::CleanUpOldInstalls() { VirtualCamera::CleanUpOldSources(); }

// The name the filter registers itself under, which is the one thing about the
// far end that the settings tab has to be able to say out loud.
std::string CameraSink::DeviceName() { return ToUtf8(vcam::kFilterName); }

void CameraSink::SetWideOffered(bool offered) { impl_->camera.SetWideOffered(offered); }

bool CameraSink::wantsWide() const { return impl_->camera.wantsWide(); }

void CameraSink::SetSourceShape(int width, int height, double fps, bool wide) {
  impl_->camera.SetSourceShape(width, height, fps, wide);
}

void CameraSink::StartAsync() { impl_->camera.StartAsync(); }

void CameraSink::Stop() { impl_->camera.Stop(); }

bool CameraSink::running() const { return impl_->camera.running(); }

bool CameraSink::starting() const { return impl_->camera.starting(); }

bool CameraSink::takeError(std::string* out) { return impl_->camera.takeError(out); }

bool CameraSink::consumed() const { return impl_->camera.consumed(); }

void CameraSink::PushFrame(const uint8_t* rgba, int stride, int width, int height) {
  impl_->camera.PushFrame(rgba, stride, width, height);
}

void CameraSink::PushFrameWide(const uint8_t* packed, int stride, int width, int height) {
  impl_->camera.PushFrameWide(packed, stride, width, height);
}

void CameraSink::consumers(std::vector<Consumer>* out) const {
  impl_->camera.consumers(out);
}

}  // namespace cap
