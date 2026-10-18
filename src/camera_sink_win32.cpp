#include "camera_sink.h"

#include <atomic>
#include <mutex>
#include <thread>

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

  std::thread setup;
  std::atomic<bool> settingUp{false};
  std::mutex resultMutex;
  bool hasResult = false;
  SetupResult result;

  ~Impl() {
    if (setup.joinable()) setup.join();
  }
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

void CameraSink::ChangeSetupAsync(bool install) {
  Impl& impl = *impl_;
  if (impl.settingUp.exchange(true, std::memory_order_acq_rel)) return;
  if (impl.setup.joinable()) impl.setup.join();
  // Registering under a running camera would pull the source out from under
  // its readers.
  impl.camera.Stop();
  impl.setup = std::thread([&impl, install] {
    SetupResult result;
    result.install = install;
    result.ok = install ? VirtualCamera::InstallSource(&result.error)
                        : VirtualCamera::UninstallSource(&result.error);
    {
      std::lock_guard<std::mutex> lock(impl.resultMutex);
      impl.result = std::move(result);
      impl.hasResult = true;
    }
    impl.settingUp.store(false, std::memory_order_release);
  });
}

bool CameraSink::changingSetup() const {
  return impl_->settingUp.load(std::memory_order_acquire);
}

bool CameraSink::TakeSetupResult(SetupResult* out) {
  if (impl_->settingUp.load(std::memory_order_acquire)) return false;
  std::lock_guard<std::mutex> lock(impl_->resultMutex);
  if (!impl_->hasResult) return false;
  *out = std::move(impl_->result);
  impl_->hasResult = false;
  return true;
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
