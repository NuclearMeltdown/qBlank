#pragma once

// A drawing surface for a window that shows nothing but the interface.
//
// The preview has render/display.h, which owns a swap chain built for latency.
// This is the other case: the settings window, which draws Dear ImGui and
// nothing else, and wants to follow the mouse smoothly rather than be on the
// screen in a millisecond.
//
// What a backend has to promise, spelled out here rather than left to whatever
// the implementation happens to do:
//
//   * A graphics device of its own, not the preview's. Sharing one was measured
//     to cost the preview its short queue and its own command stream -- the two
//     things its latency is built on.
//   * Up to three frames queued. This surface is allowed a queue; the preview
//     is not.
//   * Three buffers, so a present that arrives before the previous one was
//     retired does not wait for it.
//   * Present immediately, without waiting for a vertical blank, and tear where
//     the adapter allows it. A flip model chain with no tearing still hands the
//     frame over at a blank, which measured 12 to 17 ms a present and parked
//     the thread that owns both windows.
//   * Keep the occluded answer. A fully covered window otherwise runs into the
//     compositor's own throttle and every present takes far longer than it
//     looks like it should.
//
// A backend that cannot do one of these should fail rather than quietly do
// something else.
//
// The Windows half is ui_surface_win32.cpp, Direct3D 11.

#include <memory>
#include <string>

#include "window.h"

namespace cap {

class UiSurface {
 public:
  UiSurface();
  ~UiSurface();

  UiSurface(const UiSurface&) = delete;
  UiSurface& operator=(const UiSurface&) = delete;

  // Its own device, before there is a window. Failing here is fatal for the
  // window; everything after it can be unwound with the calls below.
  bool CreateDevice(std::string* error);

  // A chain on that window. `allowTearing` comes from the preview, which has
  // already asked the adapter.
  bool Attach(const Window& window, bool allowTearing, std::string* error);

  // The interface's renderer backend, for the ImGui context that is current.
  bool InitUi();
  // Undone again, and safe when InitUi never ran or already failed.
  void ShutdownUi();

  void ReleaseSwapchain();
  void ReleaseDevice();

  // After the window changed size. Cheap when there is no chain.
  void Resize();
  bool hasTarget() const;

  // True while the window is covered and there is no point drawing. Asks the
  // cheap question rather than presenting a frame nobody sees.
  bool StillOccluded();

  // Starts the renderer backend's frame. Between this and Present the caller
  // builds the interface.
  void NewUiFrame();
  // Clears to `clear`, draws what the interface built, and puts it on screen.
  void Present(const float clear[4]);

  int width() const;
  int height() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cap
