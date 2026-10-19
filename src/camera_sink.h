#pragma once

// The picture leaving the program again: other applications open it as a camera
// and get exactly what qBlank is showing, at the size and rate it is showing it.
//
// Two things here are easy to confuse. Making the camera visible to other
// applications at all is a system wide installation -- done once, and it needs
// administrator rights. Turning the camera on afterwards happens under the
// ordinary user account and needs no rights at all.
//
// What this does not do is decide what the camera looks like. The picture is
// published as it is displayed, and fitting it to what a particular application
// asked for belongs to the far end, where it can be done once per application
// instead of once for all of them.
//
// The Windows half is a DirectShow source filter, and qBlank's end of it is
// vcam/virtual_camera.h.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cap {

class CameraSink {
 public:
  // Where the system wide part stands right now.
  enum class Setup {
    Missing,    // not installed
    Installed,  // installed, and pointing at this build
    Stale,      // installed, but pointing at another build's file or none
  };

  // One application reading the camera, as it reports itself. The format is
  // per reader: one may take the source untouched while another asked for
  // something smaller.
  struct Consumer {
    std::string name;
    uint32_t pid = 0;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    bool wide = false;
    bool streaming = false;
  };

  CameraSink();
  ~CameraSink();
  CameraSink(const CameraSink&) = delete;
  CameraSink& operator=(const CameraSink&) = delete;

  // Cheap enough to call per frame of interface, but it does ask the system, so
  // the settings tab caches it.
  static Setup Status();

  // Installs the system wide part, or removes it. Both ask the user for
  // administrator rights and wait for the answer, so they run on a thread of
  // their own and the picture goes on meanwhile. The camera goes off first. A
  // request while one is still running is ignored.
  void ChangeSetupAsync(bool install);
  bool changingSetup() const;
  struct SetupResult {
    bool install = false;
    bool ok = false;
    std::string error;  // also when the user declined
  };
  // How the last ChangeSetupAsync went, once.
  bool TakeSetupResult(SetupResult* out);

  // Removes what earlier installations left behind. Called once at start, when
  // whatever had those files open has usually let go.
  static void CleanUpOldInstalls();

  // What the camera is called in other applications' lists of cameras. Fixed,
  // and not the program's own name: once it has been installed under this name,
  // that is the name everything that found it goes on looking for.
  static std::string DeviceName();

  // Whether to publish the ten bit picture when there is one. Takes effect on
  // the next frame; readers already connected are converted rather than cut
  // off, which is the far end's business and not this one's.
  void SetWideOffered(bool offered);
  bool wantsWide() const;

  // What the camera would publish right now: the size qBlank is showing, the
  // rate the source runs at, and whether the picture would be the ten bit one.
  //
  // Called whether or not anybody is watching, and that is the point. A reader
  // asks for all of this the moment it connects, before it has asked for a
  // single frame. Left until the first picture, every reader would negotiate
  // against a placeholder and get 720p from a 1080p source.
  void SetSourceShape(int width, int height, double fps, bool wide);

  // Turning the camera on and off. Starting is quick, but it is asynchronous
  // because it can fail in ways that have to be shown rather than waited for.
  void StartAsync();
  void Stop();
  bool running() const;
  bool starting() const;

  // Takes the reason the last start failed, once. Empty means it did not.
  bool takeError(std::string* out);

  // True while at least one application is actually reading. Until then pushing
  // frames costs nothing, so callers need not check first.
  bool consumed() const;

  // Hands over one displayed frame at whatever size qBlank is showing, already
  // in what the camera publishes: NV12 in BT.601, or with `wide` P010 in
  // BT.2020 PQ (VideoRenderer::Yuv). Returns immediately: the copy is cheap
  // and anything further happens on a thread of its own.
  void PushFrame(const uint8_t* yuv, int stride, int width, int height, bool wide);

  // Who is reading, and in what. Empty when nobody is.
  void consumers(std::vector<Consumer>* out) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cap
