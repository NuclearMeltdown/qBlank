#pragma once

// The passes the picture goes through, as an interface.
//
// Not a graphics API wrapped call by call. What is abstracted here is the list
// of passes qBlank actually has, and a backend is handed source planes,
// parameters and a target and gives back a picture:
//
//   clean       the card's planes -> linear RGBA in source geometry
//               (colour matrix, range, dot crawl, temporal work, history)
//   convert     that -> the intermediate: fields, crop, line doubling, rotation
//   scale       the intermediate -> the window's back buffer
//               (filter, sharpen, scanlines, mask, tone mapping, picture controls)
//   deliver     the intermediate -> square pixels for everything that is not the
//               window: the recording, the screenshots, the virtual camera
//   hdr-record  the same, PQ coded in ten bits, for a recording that keeps the
//               range
//   ui-composite the interface's own layer back over the picture, in scRGB
//
// The shaders themselves are not part of this. They are one file of HLSL that a
// backend either compiles or does not; what the interface promises is the shape
// of the pipeline, not the language it is written in.
//
// Two backends: render_passes_win32.cpp for Direct3D 11 and
// render_passes_vulkan.cpp for Vulkan, each drawing with the device of the
// display it is given. In Vulkan every pass is one full screen triangle into
// one colour attachment with one uniform block and at most twelve sampled
// images, begun and ended as its own dynamic rendering; the plane uploads go
// through host visible buffers into images on the device, the readbacks are
// copies into host visible buffers. The HLSL is the same file for both: the
// Vulkan build compiles it to SPIR-V with dxc (CMakeLists.txt).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cap {

class Display;
enum class GraphicsApi;

// How one uploaded plane is read. Named by what a sample holds rather than by
// any one API's spelling of it.
enum class PlaneFormat {
  R8,     // one eight bit component: luma, or a planar chroma plane
  Rg8,    // two of them: NV12's interleaved chroma
  R16,    // sixteen bit luma, ten of them used in a P010 container
  Rg16,   // the matching chroma
  Rgba8,  // four bytes, R first: packed YUV read two pixels at a time, or RGB24
  Bgra8,  // four bytes, B first: RGB32 as the card sends it
};

// The pictures the passes keep between them, as far as anything outside needs
// to name them.
enum class PassImage {
  Intermediate,  // after convert: the picture at output resolution
  Delivery,      // after deliver: eight bit, square pixels
  DeliveryHalf,  // after deliver: linear light, square pixels
};

// One plane opened for writing. `rowPitch` is the backend's, not the source's.
struct MappedPlane {
  uint8_t* data = nullptr;
  size_t rowPitch = 0;
};

// One finished picture opened for reading.
struct MappedImage {
  const uint8_t* data = nullptr;
  size_t rowPitch = 0;
};

// Everything the clean and convert passes are told about one frame. The fields
// are the shader's own, in its order.
struct ConvertParams {
  int32_t formatKind;
  int32_t deinterlaceMode;
  int32_t fieldIndex;
  int32_t bottomUp;

  int32_t cropLeft;
  int32_t cropTop;
  int32_t srcWidth;
  int32_t srcHeight;

  int32_t outWidth;
  int32_t outHeight;
  int32_t isYuv;
  int32_t havePrev;

  float yOffset;
  float yScale;
  float cScale;
  // P010 keeps ten bits in the top of sixteen, so a sixteen bit read comes back
  // 1023*64/65535 for full scale rather than 1.0. This puts that right; it is
  // 1.0 for everything else.
  float pixelScale;

  // -1 = the fields are half a picture line apart, as a real interlaced signal
  // has them. 0 or 1 = they are co-sited, and this is the row a pair starts on.
  int32_t coSitedPhase;
  int32_t rotation;
  int32_t lineDouble;
  int32_t chromaSoft;

  float temporal;
  int32_t histCount;
  float dotNotch;
  float carrierPeriod;

  // 0 = the picture is already display referred, as everything SDR is.
  // 1 = PQ (SMPTE ST 2084), 2 = HLG. Either way the clean pass turns it into
  // linear light with 1.0 meaning diffuse white.
  int32_t transfer;
  int32_t gamut;  // 1 = the primaries are BT.2020 and want converting to BT.709
  // Where the temporal filter's motion gate lets go: how much movement it
  // ignores, and how fast it gives up above that. See TemporalGate.
  float motionSlack;
  float motionSlope;

  // The three restoration steps added in 3.3. Each one is described where it is
  // implemented; the motion compensator's comment also says what it refuses to
  // do and why, which is the part that is easy to get wrong.
  int32_t motionComp;   // 1 = follow the movement when averaging noise away
  int32_t adaptChroma;  // 1 = soften colour only where the brightness invites it
  float bandwidth;      // 0..1, how much of the rolled off luma band to restore
  // Where the A/B divider sits, as a share of the cropped width or height.
  // Negative turns the comparison off; zero cannot, because zero is a divider on
  // the left or top edge.
  float compareSplit;

  float coef[4];

  int32_t compareAxis;  // 0 = divider upright, 1 = lying across
  // How many frames the temporal average covers: 2, 3 or 4, one cycle of the
  // source's dot crawl. VideoRenderer::crawlCycle() says where it comes from.
  int32_t crawlCycle;
  // How many rows of the source one line of the same field is apart: 2 on a
  // frame woven from two fields, 1 otherwise. The motion search steps up and
  // down by this, so a vertical shift never pairs lines of different fields.
  int32_t motionRows;
};

// The same for the scale pass, which the delivery pass reuses.
struct ScaleParams {
  float srcSize[2];
  float dstSize[2];
  int32_t filter;
  float sharpen;
  int32_t transfer;    // as ConvertParams::transfer -- was the source HDR
  int32_t outputHdr;   // the target is scRGB and wants linear light

  float paperWhite;    // nits the source's diffuse white should come out at
  float sourcePeak;    // nits the brightest part of the source is assumed to reach
  float displayPeak;   // nits this display can actually manage
  float scanlines;     // 0..1, how dark the gaps between source lines go

  int32_t mask;        // 0 off, 1 aperture grille, 2 shadow mask
  float maskStrength;  // 0..1
  int32_t nativeWidth; // pixels the source really has across, 0 = leave alone
  float linePitch;     // output rows per real picture line; 0 disables scanlines

  int32_t passthrough; // resample only: no display effects, no transfer, no clamp
  float brightness;    // -1..1 added, or stops of exposure in linear light
  float contrast;      // 0..2 around a pivot; 1 neutral
  float saturation;    // 0..2; 1 neutral

  float hue;           // radians
  int32_t procAmp;     // apply the four above in this pass
  float compareSplit;  // as ConvertParams::compareSplit; the unfiltered side skips this pass's effects
  int32_t compareAxis;

  int32_t rotation;    // to find the divider's axis again after the quarter turns
};

class RenderPasses {
 public:
  virtual ~RenderPasses() = default;

  // The three preceding frames, as a ring. Filled by copying the current planes
  // just before they are overwritten, which is cheaper than uploading twice and
  // keeps the capture path untouched. One copy per frame regardless of depth;
  // the copies only happen while something that reads them is switched on.
  //
  // Three because the composite denoiser needs a four frame window -- see the
  // measurement in the shader. YADIF only ever looks at the first of them.
  static const int kHistoryDepth = 3;

  // Readback ring. Three targets: one being written, one in flight, one old
  // enough to map without stalling.
  static const int kReadbackSlots = 3;

  // The display backend this one draws with.
  virtual GraphicsApi api() const = 0;

  // Draws into the same back buffer the display presents, so it uses that
  // display's device. Fails when there is none, or when the display runs on
  // another backend.
  virtual bool Initialize(Display* display, std::string* error) = 0;
  virtual void Shutdown() = 0;

  // ---- source planes ----
  //
  // One to three of them, depending on the format, each with its ring of
  // history copies. Sizes are in samples of that plane, not in pixels of the
  // picture.
  virtual bool CreatePlane(int index, int width, int height, PlaneFormat format) = 0;
  virtual void ReleasePlanes() = 0;

  // Opens one plane for writing. The old contents are discarded rather than
  // waiting for the GPU to finish reading them.
  virtual bool MapPlane(int index, MappedPlane* mapped) = 0;
  virtual void UnmapPlane(int index) = 0;

  // Moves what is in the planes right now into history slot `slot`, together
  // with the cleaned picture that belongs to it. Called before the planes are
  // overwritten.
  virtual void PushHistory(int slot, int planeCount) = 0;

  // ---- the pictures between the passes ----

  // The picture as the card sent it with the composite artefacts taken out, and
  // the same one frame back. In source geometry.
  virtual bool EnsureClean(int width, int height) = 0;

  // The picture after the convert pass. `wide` asks for one that can hold
  // linear light past 1.0 rather than eight bits.
  virtual bool EnsureIntermediate(int width, int height, bool wide) = 0;
  virtual bool hasIntermediate() const = 0;
  virtual int intermediateWidth() const = 0;
  virtual int intermediateHeight() const = 0;
  virtual bool intermediateWide() const = 0;

  // ---- the passes ----

  // clean and convert, in one go: the planes and their history in, the
  // intermediate out. `historyWrite` is the ring slot the next copy will go
  // into, which is how the backend finds the newest frame.
  //
  // Leaves the pipeline with nothing bound as a source, so the next pass can
  // write where this one read.
  virtual void CleanAndConvert(const ConvertParams& params, int srcWidth, int srcHeight,
                               int outWidth, int outHeight, int historyWrite) = 0;

  // scale: the intermediate onto the back buffer, into the rectangle at
  // (x, y) with that size. Afterwards the whole window is the viewport again
  // and nothing is left bound, so the interface can draw on top.
  virtual void ScaleToScreen(const ScaleParams& params, int x, int y, int width,
                             int height) = 0;

  // deliver: the intermediate to square pixels at `width` x `height`. `half`
  // picks the linear light target, which the PQ recording and the wide
  // screenshot read; the other one is an ordinary eight bit picture.
  virtual bool Deliver(bool half, int width, int height, const ScaleParams& params) = 0;

  // ui-composite: a layer of its own for the interface to draw into, and the
  // step that brings it back over the picture. Only used while the output is
  // scRGB. Between the two the interface's own render target is bound.
  virtual bool BeginUiLayer(int width, int height) = 0;
  virtual void CompositeUiLayer(float paperWhiteNits) = 0;

  // ---- readback ----

  // The ring the recording reads from. Eight bit, the byte order named by
  // VideoRenderer::kReadbackPixelFormat.
  virtual bool CreateReadbackSlots(int width, int height) = 0;
  virtual void ReleaseReadbackSlots() = 0;
  virtual void CopyToReadback(int slot, PassImage from) = 0;
  // Does not wait for the GPU: false when the copy is still running.
  virtual bool MapReadback(int slot, MappedImage* mapped) = 0;
  virtual void UnmapReadback(int slot) = 0;

  // hdr-record: the same picture PQ coded in ten bits, with its own ring,
  // because the camera and the screenshots still want eight bits at the same
  // moment. The byte order is VideoRenderer::kHdrReadbackPixelFormat.
  virtual bool hasHdrRecord() const = 0;
  virtual int hdrRecordWidth() const = 0;
  virtual int hdrRecordHeight() const = 0;
  virtual bool CreateHdrRecord(int width, int height) = 0;
  virtual void RecordHdr(PassImage from, float paperWhiteNits, int slot) = 0;
  // Waits for the GPU. The caller queues two frames ahead so it does not.
  virtual bool MapHdrReadback(int slot, MappedImage* mapped) = 0;
  virtual void UnmapHdrReadback(int slot) = 0;

  // ---- stills ----
  //
  // Both wait for the GPU, which a still is allowed to do: it happens when
  // somebody presses a key, not sixty times a second.

  // Eight bit RGBA, tightly packed, alpha opaque.
  virtual bool ReadStill(PassImage from, int width, int height,
                         std::vector<uint8_t>* pixels) = 0;
  // Linear light, four half floats per pixel. The stride comes back in bytes
  // because the backend's own row padding is part of what is returned.
  virtual bool ReadStillHalf(PassImage from, int rows, std::vector<uint16_t>* out,
                             int* strideBytes) = 0;
};

// The passes for a display backend. Null when this build does not have them.
std::unique_ptr<RenderPasses> CreateRenderPasses(GraphicsApi api);

// The backends' own factories, for CreateRenderPasses. The Vulkan one only
// exists where the build has Vulkan (QBLANK_VULKAN).
std::unique_ptr<RenderPasses> CreateD3D11Passes();
std::unique_ptr<RenderPasses> CreateVulkanPasses();

}  // namespace cap
