#pragma once

// The main window's picture on the screen: a back buffer to draw into and the
// presentation that takes it to the panel. There are two backends behind it,
// chosen when the display is initialised: Direct3D 11 and DXGI in
// display_win32.cpp, and Vulkan in display_vulkan.cpp where the build found
// the Vulkan SDK. display_backend.h is what they share; display_win32.h and
// display_vulkan.h hand each device to the renderer's passes, the only other
// code that draws into it.
//
// What presenting has to mean. This is not a detail of one backend but the
// point of the program -- the picture reaches the screen as early as the
// hardware allows -- so a backend that cannot promise all of it must fail
// Initialize rather than quietly do something slower:
//
//   - Present immediately. Without vsync nothing waits for a vertical blank,
//     and tearing is allowed wherever the system supports it.
//   - At most one frame ahead: drawing never runs more than one frame in front
//     of what the screen shows.
//   - The window's own two buffers go to the screen in turn (flip model); the
//     picture is not copied into anybody else's buffer on the way.
//   - With vsync, one present per vertical blank and no more.
//
// In Direct3D 11 that is a FLIP_DISCARD chain of two buffers, tearing allowed,
// and a frame latency of one. In Vulkan it is a swapchain of two images (more
// only where the driver's minimum is higher), IMMEDIATE present mode (MAILBOX
// where that is missing) or FIFO with vsync, and one frame in flight behind
// one fence.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ImDrawData;

namespace cap {

class Window;

// Names of the installed graphics adapters, joined. Used to notice that the
// machine's hardware changed since the encoder test was cached -- a saved
// result from someone's old card is worse than no result at all.
std::string GraphicsAdapterSignature();

// The graphics interface the main window draws with. Direct3D 11 is the
// default and every build has it; Vulkan only where the build found the SDK.
enum class GraphicsApi { D3D11, Vulkan };
const char* GraphicsApiName(GraphicsApi api);
bool GraphicsApiBuilt(GraphicsApi api);

// A picture for Dear ImGui to draw, made once from pixels and never changed.
// Empty when it could not be made.
struct UiImage {
  std::shared_ptr<void> texture;

  // What ImGui::Image takes.
  unsigned long long id() const { return (unsigned long long)(uintptr_t)texture.get(); }
  explicit operator bool() const { return texture != nullptr; }
};

class Display {
 public:
  Display();
  ~Display();

  Display(const Display&) = delete;
  Display& operator=(const Display&) = delete;

  // Fails where the backend is not built or cannot promise what the top of
  // this file asks; the display is then as if never initialised, and another
  // backend can be tried.
  bool Initialize(const Window& window, std::string* error,
                  GraphicsApi api = GraphicsApi::D3D11);
  void Shutdown();
  bool initialized() const;
  // The backend Initialize was given. D3D11 before that.
  GraphicsApi api() const;

  // Recreates the back buffer for the window's current client size. Safe to
  // call on every resize message.
  void Resize();

  // What the screen this window is on can actually do. Asked of the system
  // rather than of a setting, and re-asked when the window moves: dragging a
  // window from an HDR screen to an ordinary one has to change what is sent to
  // it.
  struct DisplayCapability {
    bool hdr = false;      // the output is in HDR10 mode right now
    float peakNits = 100.0f;
    float minNits = 0.0f;
  };
  DisplayCapability displayCapability() const;
  void RefreshDisplayCapability();

  // Switches the back buffer between eight bit sRGB and half float scRGB. scRGB
  // is linear with 1.0 fixed at eighty nits, so highlights simply carry on past
  // one -- which is why it suits a pipeline that already works in float.
  // Returns false and stays where it was if the switch could not be made.
  bool SetHdrOutput(bool enabled, std::string* error);
  bool hdrOutput() const;

  // Binds the back buffer and clears it. Returns false when the window is
  // occluded or has no area, in which case the frame should be skipped.
  bool BeginFrame(const float clearColor[4]);

  // Presents, as described at the top. `vsync` false is immediate presentation
  // with tearing allowed where the system supports it.
  void EndFrame(bool vsync);

  // Liest den Rueckpuffer als RGBA aus, also das Bild samt allem, was darauf
  // gezeichnet wurde. Muss vor dem Present passieren -- danach ist der Inhalt
  // eines Flip-Puffers undefiniert.
  //
  // Nur im Acht-Bit-Modus. Ist die HDR-Ausgabe aktiv, ist der Puffer scRGB in
  // halben Gleitkommazahlen, und ihn ohne Pruefung auf echter Hardware nach
  // SDR umzurechnen hiesse, ungetestete Farbmathematik auszuliefern.
  bool GrabBackBuffer(std::vector<uint8_t>* rgba, int* width, int* height);

  int width() const;
  int height() const;
  bool tearingSupported() const;

  // Dear ImGui's renderer half, drawing into this back buffer. RenderUi goes
  // between BeginFrame and EndFrame.
  bool InitUi();
  void ShutdownUi();
  void NewUiFrame();
  void RenderUi(ImDrawData* data);

  // RGBA, premultiplied, `width` * 4 bytes a row.
  UiImage CreateUiImage(const uint8_t* rgba, int width, int height);

  // The backend behind it, for the renderer's passes. display_backend.h.
  struct Impl;
  const Impl* impl() const { return impl_.get(); }

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace cap
