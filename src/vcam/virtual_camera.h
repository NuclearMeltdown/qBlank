#pragma once

// qBlank's end of the virtual camera: the Windows side of camera_sink.h, which
// is what the rest of the program talks to.
//
// Two things live here that are easy to confuse. Installing puts the DirectShow
// filter's DLL into the registry, needs administrator rights, and is done once.
// Turning the camera on happens afterwards under the ordinary user account and
// needs no rights at all: it creates a shared section and starts publishing
// into it.
//
// What this class does *not* do any more is decide what the camera looks like.
// CapView 2.x scaled and letterboxed every frame into one of three hardcoded
// sizes at thirty frames a second, in this process, once -- so a 576i50 SNES
// arrived in OBS as a stretched 1080p30. Now the picture is published exactly
// as it is displayed, at the rate it arrives, and each consumer's own copy of
// the filter fits it to whatever that consumer asked for. OBS takes it
// untouched; Discord, which will not go above 720p30, gets that; neither costs
// the other anything.

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "camera_sink.h"

namespace cap {

class VirtualCamera {
 public:
  enum class Install {
    Missing,    // the filter is not registered
    Installed,  // registered, and pointing at this build's file
    Stale,      // registered, but pointing at another build's file or none
  };

  // One application reading the camera, as its own copy of the filter reports
  // itself. The name comes for free: the filter runs inside that application.
  using Consumer = CameraSink::Consumer;

  ~VirtualCamera();

  VirtualCamera(const VirtualCamera&) = delete;
  VirtualCamera& operator=(const VirtualCamera&) = delete;
  VirtualCamera() = default;

  // Where the registration stands right now. Cheap enough to call per frame of
  // UI, but it does touch the registry, so the settings tab caches it.
  static Install Status();

  // Both raise a UAC prompt and wait for it. False with `error` filled when the
  // user declines or regsvr32 fails.
  static bool InstallSource(std::string* error);
  static bool UninstallSource(std::string* error);

  // Deletes the copies left behind by earlier installs. Called once at start,
  // when whatever had them open has usually let go.
  static void CleanUpOldSources();

  // Whether to publish the ten bit picture when there is one. Takes effect on
  // the next frame; consumers already connected are converted rather than cut
  // off, which is the filter's business, not this one's.
  void SetWideOffered(bool offered);
  bool wantsWide() const { return wantsWide_.load(std::memory_order_relaxed); }

  // What the camera would publish right now: the size qBlank is showing, the
  // rate the source runs at, and whether the picture would be the ten bit one.
  //
  // Called whether or not anybody is watching, and that is the point. A
  // consumer reads all of this the moment it connects, before it has asked for
  // a single frame. Left until the first picture, every consumer would
  // negotiate against a placeholder and OBS would get 720p from a 1080p source.
  void SetSourceShape(int width, int height, double fps, bool wide);

  // Turning the camera on and off. Starting is quick now -- a section and a
  // thread, no service to wake -- but the asynchronous shape is kept because
  // the settings tab is written against it.
  void StartAsync();
  void Stop();
  bool running() const { return running_.load(std::memory_order_relaxed); }
  bool starting() const { return starting_.load(std::memory_order_relaxed); }

  // Takes the reason the last start failed, once. Empty means it did not.
  bool takeError(std::string* out);

  // True while at least one application is actually reading the camera. Until
  // then pushing frames costs nothing, so callers need not check first.
  bool consumed() const { return consumed_.load(std::memory_order_relaxed); }

  // Hands over one displayed frame, RGBA8, top row first, at whatever size
  // qBlank is showing. Returns immediately: the copy is cheap and the
  // conversion happens on a thread of its own.
  void PushFrame(const uint8_t* rgba, int stride, int width, int height);

  // The ten bit path. `packed` is one uint32 per pixel as DXGI writes
  // R10G10B10A2 -- already PQ encoded and already BT.2020, because the shader
  // that produced it for the recorder had to do that anyway.
  void PushFrameWide(const uint8_t* packed, int stride, int width, int height);

  // Who is reading, and in what. Empty when nobody is.
  void consumers(std::vector<Consumer>* out) const;

 private:
  void WorkerLoop();
  bool StartBlocking(std::string* error);
  bool CreateControl(std::string* error);
  void DestroyControl();
  bool EnsureFrameSection(uint32_t width, uint32_t height, uint32_t pixel);
  void Publish(const uint8_t* pixels, int stride, int width, int height, uint32_t pixel);
  void WakeConsumers();
  void PruneConsumers();

  std::atomic<bool> running_{false};
  std::atomic<bool> consumed_{false};
  std::atomic<bool> wantsWide_{false};
  std::atomic<bool> quit_{false};
  std::atomic<bool> starting_{false};
  std::mutex errorMutex_;
  std::string startError_;

  std::thread worker_;
  HANDLE wake_ = nullptr;

  // The handover from the render thread: one frame deep, newest wins. A webcam
  // that shows the second newest frame is worse than one that skips it.
  std::mutex pendingMutex_;
  std::vector<uint8_t> pending_;
  int pendingWidth_ = 0;
  int pendingHeight_ = 0;
  bool pendingWide_ = false;
  bool pendingFull_ = false;

  // The control section, which lives as long as the camera is on.
  HANDLE controlSection_ = nullptr;
  void* controlView_ = nullptr;

  // The frame section, which lives as long as the source keeps its shape. A
  // mapped section cannot grow, so a new size means a new one and a generation
  // number that tells the consumers to look again.
  HANDLE frameSection_ = nullptr;
  void* frameView_ = nullptr;
  uint32_t frameWidth_ = 0;
  uint32_t frameHeight_ = 0;
  uint32_t framePixel_ = 0;
  uint32_t generation_ = 0;
  uint32_t writeIndex_ = 0;
  uint32_t published_ = 0;

  // One per consumer slot, created up front. Auto-reset, so a published picture
  // wakes each reader exactly once; a single shared event cannot do that.
  HANDLE consumerWake_[8] = {};

  // The shape, remembered here as well as in the control block: it is known
  // before the camera is started and has to survive until there is somewhere
  // to write it.
  std::atomic<int64_t> sourceInterval_{333333};
  std::atomic<uint32_t> sourceWidth_{0};
  std::atomic<uint32_t> sourceHeight_{0};
  std::atomic<uint32_t> sourcePixel_{0};

  unsigned long lastReport_ = 0;
};

}  // namespace cap
