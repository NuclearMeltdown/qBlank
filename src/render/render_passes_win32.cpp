#include "render/render_passes.h"

#include <d3d11_1.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "common_win32.h"
#include "i18n.h"
#include "inflate.h"
#include "render/display_win32.h"
#include "render/shared_frames.h"
// The build compiles the shaders and packs the bytecode (CMakeLists.txt).
// check.bat compiles this file on its own, with no build behind it, and only
// needs to know that the bytecode is there.
#ifdef QBLANK_SHADERS_PACKED
#include "shaders_dxbc.h"
#else
namespace cap {
extern const Packed kFullscreenVS, kCleanPS, kConvertPS, kScalePS, kYuvPS, kUiCompositePS,
    kRemDecPS, kRemResPS, kRemCombPS, kGridPS;
}
#endif

namespace cap {
namespace {

// The constant buffers as the shaders declare them: the interface's parameters
// followed by the padding a sixteen byte register needs. The parameters are the
// same fields in the same order, so this is a copy and not a conversion.
// ConvertParams fills its last register exactly and needs none.
using ConvertCB = ConvertParams;
static_assert(sizeof(ConvertCB) % 16 == 0, "constant buffer must be 16 byte aligned");

struct ScaleCB {
  ScaleParams params;  // fills its last register exactly
};
static_assert(sizeof(ScaleCB) % 16 == 0, "constant buffer must be 16 byte aligned");

DXGI_FORMAT PlaneDxgiFormat(PlaneFormat format) {
  switch (format) {
    case PlaneFormat::R8: return DXGI_FORMAT_R8_UNORM;
    case PlaneFormat::Rg8: return DXGI_FORMAT_R8G8_UNORM;
    case PlaneFormat::R16: return DXGI_FORMAT_R16_UNORM;
    case PlaneFormat::Rg16: return DXGI_FORMAT_R16G16_UNORM;
    case PlaneFormat::Bgra8: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case PlaneFormat::Rgba8:
    default: return DXGI_FORMAT_R8G8B8A8_UNORM;
  }
}

// A shader as the build compiled it: Direct3D 11 bytecode, packed. Empty when
// it does not unpack, which takes a damaged executable.
std::vector<uint8_t> ShaderCode(const Packed& packed, const char* target, std::string* error) {
  std::vector<uint8_t> code(packed.unpackedSize);
  if (!Inflate(packed, code.data())) {
    ReportError(error, CAP_SAID(T("Shader (", "Shader (") + std::string(target) +
                          T(") ist beschädigt", ") is damaged")));
    CAP_ERR("Shader %s: unpacking failed", target);
    code.clear();
  }
  return code;
}

class D3D11Passes : public RenderPasses {
 public:
  ~D3D11Passes() override { Shutdown(); }

  GraphicsApi api() const override { return GraphicsApi::D3D11; }
  bool Initialize(Display* display, std::string* error) override;
  void Shutdown() override;

  bool CreatePlane(int index, int width, int height, PlaneFormat format) override;
  void ReleasePlanes() override;
  bool MapPlane(int index, MappedPlane* mapped) override;
  void UnmapPlane(int index) override;
  void PushHistory(int slot, int planeCount) override;
  bool AttachSourceFrames(std::shared_ptr<SharedFrames> frames) override;
  bool CopySourceFrame(int slot) override;

  bool EnsureClean(int width, int height) override;
  bool EnsureIntermediate(int width, int height, bool wide) override;
  bool hasIntermediate() const override { return intermediate_.Get() != nullptr; }
  int intermediateWidth() const override { return intermediateWidth_; }
  int intermediateHeight() const override { return intermediateHeight_; }
  bool intermediateWide() const override {
    return intermediateFormat_ == DXGI_FORMAT_R16G16B16A16_FLOAT;
  }

  void CleanAndConvert(const ConvertParams& params, int srcWidth, int srcHeight, int outWidth,
                       int outHeight, int historyWrite) override;
  void ScaleToScreen(const ScaleParams& params, int x, int y, int width, int height) override;
  bool Deliver(PassImage target, int width, int height, const ScaleParams& params) override;
  void RebuildGrid(const ScaleParams& params, int rowsPerLine, const int* stages,
                   int count) override;
  bool BeginUiLayer(int width, int height) override;
  void CompositeUiLayer(float paperWhiteNits) override;

  bool CreateYuvStream(int stream, int width, int rows, bool sixteenBit) override;
  void ReleaseYuvStream(int stream) override;
  void ConvertToYuv(int stream, int slot, PassImage from, const YuvParams& params) override;
  bool MapYuv(int stream, int slot, MappedImage* mapped) override;
  void UnmapYuv(int stream, int slot) override;
  uint64_t AdapterId() const override;
  bool AttachSharedYuv(int stream, std::shared_ptr<SharedFrames> frames) override;
  bool ConvertToShared(int stream, int slot, PassImage from, const YuvParams& params) override;

  std::unique_ptr<StillCopy> CopyStill(PassImage from, int width, int height) override {
    return QueueStillCopy(NativeContext(*display_), Texture(from), width, height);
  }

 private:
  bool CreateShaders(std::string* error);
  bool CreateStates(std::string* error);
  void DrawYuv(ID3D11RenderTargetView* rtv, int width, int rows, PassImage from,
               const YuvParams& params);
  bool EnsureDelivery(int width, int height, PassImage target);
  bool EnsureUiLayer(int width, int height);

  // Each drops one group of pictures and forgets its size.
  void ReleaseClean();
  void ReleaseIntermediate();
  void ReleaseUiLayer();

  ID3D11Device* dev() const { return NativeDevice(*display_); }
  ID3D11DeviceContext* dc() const { return NativeContext(*display_); }

  ID3D11Texture2D* Texture(PassImage image) const {
    if (image == PassImage::Intermediate) return intermediate_.Get();
    return delivery_[DeliveryIndex(image)].tex.Get();
  }
  ID3D11ShaderResourceView* View(PassImage image) const {
    if (image == PassImage::Intermediate) return intermediateSrv_.Get();
    return delivery_[DeliveryIndex(image)].srv.Get();
  }

  Display* display_ = nullptr;

  ComPtr<ID3D11VertexShader> vs_;
  ComPtr<ID3D11PixelShader> psClean_;
  ComPtr<ID3D11PixelShader> psConvert_;
  ComPtr<ID3D11PixelShader> psScale_;
  ComPtr<ID3D11PixelShader> psUiComposite_;
  ComPtr<ID3D11Texture2D> uiTex_;
  ComPtr<ID3D11RenderTargetView> uiRtv_;
  ComPtr<ID3D11ShaderResourceView> uiSrv_;
  ComPtr<ID3D11Buffer> cbUi_;
  ComPtr<ID3D11BlendState> blendPremultiplied_;
  int uiWidth_ = 0;
  int uiHeight_ = 0;
  ComPtr<ID3D11Buffer> cbConvert_;
  ComPtr<ID3D11Buffer> cbScale_;
  ComPtr<ID3D11SamplerState> sampPoint_;
  ComPtr<ID3D11SamplerState> sampLinear_;
  ComPtr<ID3D11RasterizerState> raster_;
  ComPtr<ID3D11BlendState> blendOpaque_;

  // Input planes. Which of these exist depends on the source format.
  ComPtr<ID3D11Texture2D> plane_[3];
  ComPtr<ID3D11ShaderResourceView> planeSrv_[3];
  ComPtr<ID3D11Texture2D> planeHist_[kHistoryDepth][3];
  ComPtr<ID3D11ShaderResourceView> planeHistSrv_[kHistoryDepth][3];
  // The screen capture's textures, copied into plane 0 (AttachSourceFrames).
  std::shared_ptr<SharedFrames> source_;
  ComPtr<ID3D11Texture2D> sourceTex_[SharedFrames::kSlots];
  ComPtr<IDXGIKeyedMutex> sourceMutex_[SharedFrames::kSlots];

  // Between the capture planes and the deinterlacer: the picture as the card
  // sent it, in its own geometry, with the composite artefacts already taken
  // out. Floating point because limited range material legitimately runs past
  // both ends once it has been expanded, and rounding that back into eight bits
  // here would throw away what the expansion just recovered.
  ComPtr<ID3D11Texture2D> cleanTex_;
  ComPtr<ID3D11ShaderResourceView> cleanSrv_;
  ComPtr<ID3D11RenderTargetView> cleanRtv_;
  // The same, one frame back. YADIF is the only thing that reads it.
  ComPtr<ID3D11Texture2D> cleanPrevTex_;
  ComPtr<ID3D11ShaderResourceView> cleanPrevSrv_;
  int cleanWidth_ = 0;
  int cleanHeight_ = 0;

  // Intermediate render target between the two passes.
  ComPtr<ID3D11Texture2D> intermediate_;
  ComPtr<ID3D11ShaderResourceView> intermediateSrv_;
  ComPtr<ID3D11RenderTargetView> intermediateRtv_;
  int intermediateWidth_ = 0;
  int intermediateHeight_ = 0;
  DXGI_FORMAT intermediateFormat_ = DXGI_FORMAT_R8G8B8A8_UNORM;

  // Where the picture goes when it is leaving for something that is not the
  // window and cannot be handed the intermediate as it stands -- because it
  // needs square pixels, or because it is eight bit and the intermediate is
  // not. The half ones stay in linear light: a resample that must not be tone
  // mapped on the way, because a PQ recording and a wide screenshot come off
  // it. In PassImage's order.
  struct DeliveryTarget {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    int width = 0;
    int height = 0;
  };
  DeliveryTarget delivery_[kDeliveryTargets];

  // The yuv pass: one picture per stream, one plane of luma rows and then
  // chroma rows, and the readbacks it is copied into.
  struct YuvStream {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11Texture2D> staging[kReadbackSlots];
    int width = 0;
    int rows = 0;
    // Or the recorder's textures, opened here, in place of all of that.
    std::shared_ptr<SharedFrames> shared;
    ComPtr<IDXGIKeyedMutex> sharedMutex[SharedFrames::kSlots];
    ComPtr<ID3D11RenderTargetView> sharedRtv[SharedFrames::kSlots];
  };
  ComPtr<ID3D11PixelShader> psYuv_;
  ComPtr<ID3D11Buffer> cbYuv_;
  YuvStream yuv_[kYuvStreams];

  // The crawl remover. A ring of decoded frames, the eight
  // residual targets the model fits write, and a ring of the pictures it hands
  // the clean pass, which stands in for that pass's history so the crawl fix
  // after it sees the same frames it is cleaning. Made on first use, so nobody
  // without composite pays for them.
  static constexpr int kRemRing = 12;
  static constexpr int kRemRs = 8;
  static constexpr int kRemOut = kHistoryDepth + 1;
  static constexpr int kRemImages = kRemRing + kRemRs + kRemOut;
  bool EnsureRemover(int width, int height);
  void ReleaseRemover();
  ComPtr<ID3D11PixelShader> psRemDec_;
  ComPtr<ID3D11PixelShader> psRemRes_;
  ComPtr<ID3D11PixelShader> psRemComb_;
  ComPtr<ID3D11Texture2D> remTex_[kRemImages];
  ComPtr<ID3D11ShaderResourceView> remSrv_[kRemImages];
  ComPtr<ID3D11RenderTargetView> remRtv_[kRemImages];
  int remWidth_ = 0;
  int remHeight_ = 0;
  int remHead_ = 0;
  int remOutHead_ = 0;
  int remCount_ = 0;
  bool remOk_ = false;

  // The console pixels (RebuildGrid): two pictures one texel per console
  // pixel, the stages going back and forth between them, the result in [0].
  // Valid for the grid and intermediate they were made from.
  bool EnsureGrid(int width, int height);
  // The grid lookup for a scale or deliver pass, if the grid is the one built.
  void UseGrid(ScaleParams* params) const;
  ComPtr<ID3D11PixelShader> psGrid_;
  ComPtr<ID3D11Texture2D> gridTex_[2];
  ComPtr<ID3D11ShaderResourceView> gridSrv_[2];
  ComPtr<ID3D11RenderTargetView> gridRtv_[2];
  int gridWidth_ = 0;
  int gridHeight_ = 0;
  bool gridValid_ = false;
  float gridSpan_ = 0.0f;
  float gridOffset_ = 0.0f;
};

// ------------------------------------------------------------------ lifetime

bool D3D11Passes::Initialize(Display* display, std::string* error) {
  display_ = display;
  if (!display_ || !NativeDevice(*display_)) {
    ReportError(error, CAP_SAID(T("Kein Direct3D-Gerät", "No Direct3D device")));
    return false;
  }
  if (!CreateShaders(error)) return false;
  if (!CreateStates(error)) return false;
  return true;
}

void D3D11Passes::Shutdown() {
  // All of it, not just what is expensive to keep: everything here was made on
  // the display's device, and any one piece still held keeps that device alive
  // past the display's own shutdown.
  for (int s = 0; s < kYuvStreams; ++s) ReleaseYuvStream(s);
  ReleaseUiLayer();
  for (DeliveryTarget& target : delivery_) target = DeliveryTarget();
  ReleaseIntermediate();
  ReleaseClean();
  ReleasePlanes();
  AttachSourceFrames(nullptr);
  ReleaseRemover();
  psRemComb_.Reset();
  psRemRes_.Reset();
  psRemDec_.Reset();
  for (int i = 0; i < 2; ++i) {
    gridRtv_[i].Reset();
    gridSrv_[i].Reset();
    gridTex_[i].Reset();
  }
  gridWidth_ = gridHeight_ = 0;
  gridValid_ = false;
  psGrid_.Reset();
  blendPremultiplied_.Reset();
  blendOpaque_.Reset();
  raster_.Reset();
  sampLinear_.Reset();
  sampPoint_.Reset();
  cbYuv_.Reset();
  cbUi_.Reset();
  cbScale_.Reset();
  cbConvert_.Reset();
  psYuv_.Reset();
  psUiComposite_.Reset();
  psScale_.Reset();
  psConvert_.Reset();
  psClean_.Reset();
  vs_.Reset();
  display_ = nullptr;
}

bool D3D11Passes::CreateShaders(std::string* error) {
  ID3D11Device* dev = NativeDevice(*display_);

  const std::vector<uint8_t> vsCode = ShaderCode(kFullscreenVS, "vs_4_0", error);
  if (vsCode.empty()) return false;
  if (FAILED(CAP_HR(dev->CreateVertexShader(vsCode.data(), vsCode.size(), nullptr, &vs_)))) {
    ReportError(error, CAP_SAID(T("Vertex-Shader konnte nicht erstellt werden",
                                     "The vertex shader could not be created")));
    return false;
  }

  const std::vector<uint8_t> cleanCode = ShaderCode(kCleanPS, "ps_4_0", error);
  if (cleanCode.empty()) return false;
  if (FAILED(CAP_HR(dev->CreatePixelShader(cleanCode.data(), cleanCode.size(), nullptr,
                                           &psClean_)))) {
    ReportError(error, CAP_SAID(T("Aufbereitungs-Shader konnte nicht erstellt werden",
                                     "The cleanup shader could not be created")));
    return false;
  }

  const std::vector<uint8_t> convertCode = ShaderCode(kConvertPS, "ps_4_0", error);
  if (convertCode.empty()) return false;
  if (FAILED(CAP_HR(dev->CreatePixelShader(convertCode.data(), convertCode.size(), nullptr,
                                           &psConvert_)))) {
    ReportError(error, CAP_SAID(T("Konvertierungs-Shader konnte nicht erstellt werden",
                                     "The conversion shader could not be created")));
    return false;
  }

  const std::vector<uint8_t> scaleCode = ShaderCode(kScalePS, "ps_4_0", error);
  if (scaleCode.empty()) return false;
  if (FAILED(CAP_HR(dev->CreatePixelShader(scaleCode.data(), scaleCode.size(), nullptr,
                                           &psScale_)))) {
    ReportError(error, CAP_SAID(T("Skalierungs-Shader konnte nicht erstellt werden",
                                     "The scaling shader could not be created")));
    return false;
  }

  const std::vector<uint8_t> yuvCode = ShaderCode(kYuvPS, "ps_4_0", error);
  if (yuvCode.empty()) return false;
  if (FAILED(CAP_HR(dev->CreatePixelShader(yuvCode.data(), yuvCode.size(), nullptr,
                                           &psYuv_)))) {
    ReportError(error, CAP_SAID(T("Aufnahme-Shader konnte nicht erstellt werden",
                                     "The recording shader could not be created")));
    return false;
  }

  const std::vector<uint8_t> uiCode = ShaderCode(kUiCompositePS, "ps_4_0", error);
  if (uiCode.empty()) return false;
  if (FAILED(CAP_HR(dev->CreatePixelShader(uiCode.data(), uiCode.size(), nullptr,
                                           &psUiComposite_)))) {
    ReportError(error, CAP_SAID(T("Oberflächen-Shader konnte nicht erstellt werden",
                                     "The interface shader could not be created")));
    return false;
  }

  // The crawl remover. Not fatal: without it everything else runs as
  // before, and the setting that asks for it is simply dropped.
  {
    std::string remError;
    const std::vector<uint8_t> dec = ShaderCode(kRemDecPS, "ps_5_0", &remError);
    const std::vector<uint8_t> res = ShaderCode(kRemResPS, "ps_5_0", &remError);
    const std::vector<uint8_t> comb = ShaderCode(kRemCombPS, "ps_5_0", &remError);
    remOk_ = !dec.empty() && !res.empty() && !comb.empty() &&
             SUCCEEDED(CAP_HR(dev->CreatePixelShader(dec.data(), dec.size(), nullptr,
                                                     &psRemDec_))) &&
             SUCCEEDED(CAP_HR(dev->CreatePixelShader(res.data(), res.size(), nullptr,
                                                     &psRemRes_))) &&
             SUCCEEDED(CAP_HR(dev->CreatePixelShader(comb.data(), comb.size(), nullptr,
                                                     &psRemComb_)));
    if (!remOk_) CAP_ERR("Crawl remover unavailable: %s", remError.c_str());
  }
  // The console pixel pass. Not fatal either: the scale pass rebuilds them
  // itself without it.
  {
    std::string gridError;
    const std::vector<uint8_t> grid = ShaderCode(kGridPS, "ps_5_0", &gridError);
    if (grid.empty() ||
        FAILED(CAP_HR(dev->CreatePixelShader(grid.data(), grid.size(), nullptr, &psGrid_)))) {
      psGrid_.Reset();
      CAP_ERR("Pixel grid pass unavailable: %s", gridError.c_str());
    }
  }

  D3D11_BUFFER_DESC bd = {};
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

  bd.ByteWidth = sizeof(ConvertCB);
  if (FAILED(CAP_HR(dev->CreateBuffer(&bd, nullptr, &cbConvert_)))) {
    ReportError(error, CAP_SAID(T("Konstantenpuffer konnte nicht erstellt werden",
                                     "The constant buffer could not be created")));
    return false;
  }
  bd.ByteWidth = sizeof(YuvParams);
  if (FAILED(CAP_HR(dev->CreateBuffer(&bd, nullptr, &cbYuv_)))) {
    ReportError(error, CAP_SAID(T("Konstantenpuffer konnte nicht erstellt werden",
                                     "The constant buffer could not be created")));
    return false;
  }
  bd.ByteWidth = 16;  // one float plus padding
  if (FAILED(CAP_HR(dev->CreateBuffer(&bd, nullptr, &cbUi_)))) {
    ReportError(error, CAP_SAID(T("Konstantenpuffer konnte nicht erstellt werden",
                                     "The constant buffer could not be created")));
    return false;
  }

  // Premultiplied: what arrives has already been multiplied by its own coverage,
  // so the source contributes as it is rather than being scaled again.
  D3D11_BLEND_DESC pm = {};
  pm.RenderTarget[0].BlendEnable = TRUE;
  pm.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
  pm.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  pm.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  pm.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  pm.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  pm.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  pm.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  if (FAILED(CAP_HR(dev->CreateBlendState(&pm, &blendPremultiplied_)))) {
    ReportError(error, CAP_SAID(T("Blend-State konnte nicht erstellt werden",
                                     "The blend state could not be created")));
    return false;
  }

  bd.ByteWidth = sizeof(ScaleCB);
  if (FAILED(CAP_HR(dev->CreateBuffer(&bd, nullptr, &cbScale_)))) {
    ReportError(error, CAP_SAID(T("Konstantenpuffer konnte nicht erstellt werden",
                                     "The constant buffer could not be created")));
    return false;
  }
  return true;
}

bool D3D11Passes::CreateStates(std::string* error) {
  ID3D11Device* dev = NativeDevice(*display_);

  D3D11_SAMPLER_DESC sd = {};
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sd.MaxLOD = D3D11_FLOAT32_MAX;

  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  if (FAILED(CAP_HR(dev->CreateSamplerState(&sd, &sampPoint_)))) {
    ReportError(error, CAP_SAID(T("Sampler konnte nicht erstellt werden",
                                     "The sampler could not be created")));
    return false;
  }
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  if (FAILED(CAP_HR(dev->CreateSamplerState(&sd, &sampLinear_)))) {
    ReportError(error, CAP_SAID(T("Sampler konnte nicht erstellt werden",
                                     "The sampler could not be created")));
    return false;
  }

  D3D11_RASTERIZER_DESC rd = {};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  if (FAILED(CAP_HR(dev->CreateRasterizerState(&rd, &raster_)))) {
    ReportError(error, CAP_SAID(T("Rasterizer-State konnte nicht erstellt werden",
                                     "The rasteriser state could not be created")));
    return false;
  }

  D3D11_BLEND_DESC bd = {};
  bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  if (FAILED(CAP_HR(dev->CreateBlendState(&bd, &blendOpaque_)))) {
    ReportError(error, CAP_SAID(T("Blend-State konnte nicht erstellt werden",
                                     "The blend state could not be created")));
    return false;
  }
  return true;
}

// -------------------------------------------------------------- source planes

bool D3D11Passes::CreatePlane(int index, int width, int height, PlaneFormat format) {
  ID3D11Device* dev = NativeDevice(*display_);

  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)std::max(1, width);
  td.Height = (UINT)std::max(1, height);
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = PlaneDxgiFormat(format);
  td.SampleDesc.Count = 1;
  // Copied into on the GPU for a source that is there already.
  const bool gpuFed = index == 0 && source_;
  td.Usage = gpuFed ? D3D11_USAGE_DEFAULT : D3D11_USAGE_DYNAMIC;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  td.CPUAccessFlags = gpuFed ? 0 : D3D11_CPU_ACCESS_WRITE;
  if (FAILED(CAP_HR(dev->CreateTexture2D(&td, nullptr, &plane_[index])))) return false;
  if (FAILED(CAP_HR(dev->CreateShaderResourceView(plane_[index].Get(), nullptr,
                                                  &planeSrv_[index])))) {
    return false;
  }

  // The history copies. Written by the GPU rather than the CPU, so they are
  // plain default resources; they are allocated whether or not anything reads
  // them, because switching a filter on mid-stream should not have to
  // reallocate anything. Seven copies of a 576 line frame are under six
  // megabytes.
  td.Usage = D3D11_USAGE_DEFAULT;
  td.CPUAccessFlags = 0;
  for (int h = 0; h < kHistoryDepth; ++h) {
    if (FAILED(CAP_HR(dev->CreateTexture2D(&td, nullptr, &planeHist_[h][index])))) return false;
    if (FAILED(CAP_HR(dev->CreateShaderResourceView(planeHist_[h][index].Get(), nullptr,
                                                    &planeHistSrv_[h][index])))) {
      return false;
    }
  }
  return true;
}

void D3D11Passes::ReleasePlanes() {
  InvalidateClean();
  for (int i = 0; i < 3; ++i) {
    planeSrv_[i].Reset();
    plane_[i].Reset();
    for (int h = 0; h < kHistoryDepth; ++h) {
      planeHistSrv_[h][i].Reset();
      planeHist_[h][i].Reset();
    }
  }
  remCount_ = 0;  // a new source, the ring starts over
}

bool D3D11Passes::MapPlane(int index, MappedPlane* mapped) {
  InvalidateClean();
  D3D11_MAPPED_SUBRESOURCE sub = {};
  if (FAILED(dc()->Map(plane_[index].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &sub))) return false;
  mapped->data = (uint8_t*)sub.pData;
  mapped->rowPitch = sub.RowPitch;
  return true;
}

void D3D11Passes::UnmapPlane(int index) {
  dc()->Unmap(plane_[index].Get(), 0);
}

void D3D11Passes::PushHistory(int slot, int planeCount) {
  InvalidateClean();
  ID3D11DeviceContext* c = dc();
  // The cleaned picture currently in cleanTex_ belongs to the frame that is
  // about to be replaced, so this is the moment it becomes the previous one.
  if (cleanTex_ && cleanPrevTex_) c->CopyResource(cleanPrevTex_.Get(), cleanTex_.Get());
  for (int i = 0; i < planeCount; ++i) {
    if (plane_[i] && planeHist_[slot][i]) {
      c->CopyResource(planeHist_[slot][i].Get(), plane_[i].Get());
    }
  }
}

bool D3D11Passes::AttachSourceFrames(std::shared_ptr<SharedFrames> frames) {
  for (int i = 0; i < SharedFrames::kSlots; ++i) {
    sourceMutex_[i].Reset();
    sourceTex_[i].Reset();
  }
  source_.reset();
  if (!frames) return true;
  ComPtr<ID3D11Device1> dev1;
  if (FAILED(CAP_HR(dev()->QueryInterface(IID_PPV_ARGS(&dev1))))) return false;
  for (int i = 0; i < SharedFrames::kSlots; ++i) {
    if (FAILED(CAP_HR(dev1->OpenSharedResource1(frames->handle[i], IID_PPV_ARGS(&sourceTex_[i])))) ||
        FAILED(CAP_HR(sourceTex_[i].As(&sourceMutex_[i])))) {
      AttachSourceFrames(nullptr);
      return false;
    }
  }
  source_ = std::move(frames);
  return true;
}

bool D3D11Passes::CopySourceFrame(int slot) {
  if (!source_ || !plane_[0] || slot < 0 || slot >= SharedFrames::kSlots) return false;
  // Key 1 for a new picture; 0 when this one was copied already and comes again.
  IDXGIKeyedMutex* mutex = sourceMutex_[slot].Get();
  if (mutex->AcquireSync(source_->key[slot], 0) != S_OK) return false;
  InvalidateClean();
  dc()->CopyResource(plane_[0].Get(), sourceTex_[slot].Get());
  mutex->ReleaseSync(0);
  source_->key[slot] = 0;
  return true;
}

// ------------------------------------------------- the pictures between passes

void D3D11Passes::ReleaseClean() {
  InvalidateClean();
  cleanSrv_.Reset();
  cleanRtv_.Reset();
  cleanTex_.Reset();
  cleanPrevSrv_.Reset();
  cleanPrevTex_.Reset();
  cleanWidth_ = 0;
  cleanHeight_ = 0;
}

void D3D11Passes::ReleaseIntermediate() {
  intermediateRtv_.Reset();
  intermediateSrv_.Reset();
  intermediate_.Reset();
  intermediateWidth_ = 0;
  intermediateHeight_ = 0;
  intermediateFormat_ = DXGI_FORMAT_R8G8B8A8_UNORM;
}

void D3D11Passes::ReleaseUiLayer() {
  uiSrv_.Reset();
  uiRtv_.Reset();
  uiTex_.Reset();
  uiWidth_ = 0;
  uiHeight_ = 0;
}

bool D3D11Passes::EnsureClean(int width, int height) {
  width = std::max(1, width);
  height = std::max(1, height);
  if (cleanTex_ && cleanWidth_ == width && cleanHeight_ == height) return true;

  ReleaseClean();

  ID3D11Device* dev = NativeDevice(*display_);
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)width;
  td.Height = (UINT)height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  // Here and in the other Ensure functions: a failure takes back what was
  // already made. A texture left without its views would pass for a finished
  // one on the next call.
  if (FAILED(CAP_HR(dev->CreateTexture2D(&td, nullptr, &cleanTex_))) ||
      FAILED(CAP_HR(dev->CreateShaderResourceView(cleanTex_.Get(), nullptr, &cleanSrv_))) ||
      FAILED(CAP_HR(dev->CreateRenderTargetView(cleanTex_.Get(), nullptr, &cleanRtv_)))) {
    ReleaseClean();
    return false;
  }

  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(CAP_HR(dev->CreateTexture2D(&td, nullptr, &cleanPrevTex_))) ||
      FAILED(CAP_HR(dev->CreateShaderResourceView(cleanPrevTex_.Get(), nullptr,
                                                  &cleanPrevSrv_)))) {
    ReleaseClean();
    return false;
  }

  cleanWidth_ = width;
  cleanHeight_ = height;
  return true;
}

// The crawl remover's pictures.
void D3D11Passes::ReleaseRemover() {
  for (int i = 0; i < kRemImages; ++i) {
    remRtv_[i].Reset();
    remSrv_[i].Reset();
    remTex_[i].Reset();
  }
  remWidth_ = 0;
  remHeight_ = 0;
  remHead_ = 0;
  remOutHead_ = 0;
  remCount_ = 0;
}

bool D3D11Passes::EnsureRemover(int width, int height) {
  width = std::max(1, width);
  height = std::max(1, height);
  if (remTex_[0] && remWidth_ == width && remHeight_ == height) return true;

  ReleaseRemover();

  ID3D11Device* dev = NativeDevice(*display_);
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)width;
  td.Height = (UINT)height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  for (int i = 0; i < kRemImages; ++i) {
    if (FAILED(CAP_HR(dev->CreateTexture2D(&td, nullptr, &remTex_[i]))) ||
        FAILED(CAP_HR(dev->CreateShaderResourceView(remTex_[i].Get(), nullptr, &remSrv_[i]))) ||
        FAILED(CAP_HR(dev->CreateRenderTargetView(remTex_[i].Get(), nullptr, &remRtv_[i])))) {
      ReleaseRemover();
      remOk_ = false;  // do not try again every frame
      CAP_ERR("Crawl remover: its pictures could not be made");
      return false;
    }
  }
  remWidth_ = width;
  remHeight_ = height;
  CAP_LOG("Crawl remover running at %dx%d", width, height);
  return true;
}

bool D3D11Passes::EnsureIntermediate(int width, int height, bool wide) {
  width = std::max(1, width);
  height = std::max(1, height);
  const DXGI_FORMAT format = wide ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
  if (intermediate_ && intermediateWidth_ == width && intermediateHeight_ == height &&
      intermediateFormat_ == format) {
    return true;
  }

  ReleaseIntermediate();

  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)width;
  td.Height = (UINT)height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = format;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

  ID3D11Device* dev = NativeDevice(*display_);
  if (FAILED(CAP_HR(dev->CreateTexture2D(&td, nullptr, &intermediate_))) ||
      FAILED(CAP_HR(dev->CreateShaderResourceView(intermediate_.Get(), nullptr,
                                                  &intermediateSrv_))) ||
      FAILED(CAP_HR(dev->CreateRenderTargetView(intermediate_.Get(), nullptr,
                                                &intermediateRtv_)))) {
    ReleaseIntermediate();
    return false;
  }
  intermediateWidth_ = width;
  intermediateHeight_ = height;
  intermediateFormat_ = format;
  return true;
}

bool D3D11Passes::EnsureDelivery(int width, int height, PassImage target) {
  width = std::max(1, width);
  height = std::max(1, height);

  DeliveryTarget& t = delivery_[DeliveryIndex(target)];
  if (t.tex && t.width == width && t.height == height) return true;

  t = DeliveryTarget();

  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)width;
  td.Height = (UINT)height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = HalfImage(target) ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  // Both are read again afterwards, by the yuv pass.
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

  ID3D11Device* dev = NativeDevice(*display_);
  if (FAILED(CAP_HR(dev->CreateTexture2D(&td, nullptr, &t.tex))) ||
      FAILED(CAP_HR(dev->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv))) ||
      FAILED(CAP_HR(dev->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv)))) {
    t = DeliveryTarget();
    return false;
  }
  t.width = width;
  t.height = height;
  return true;
}

bool D3D11Passes::EnsureUiLayer(int width, int height) {
  if (uiTex_ && uiWidth_ == width && uiHeight_ == height) return true;
  ReleaseUiLayer();
  if (width <= 0 || height <= 0) return false;

  // Eight bit on purpose. This holds an ordinary sRGB interface, and giving it
  // more precision than the thing that drew it would buy nothing.
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)width;
  td.Height = (UINT)height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

  ID3D11Device* dev = NativeDevice(*display_);
  if (FAILED(CAP_HR(dev->CreateTexture2D(&td, nullptr, &uiTex_))) ||
      FAILED(CAP_HR(dev->CreateRenderTargetView(uiTex_.Get(), nullptr, &uiRtv_))) ||
      FAILED(CAP_HR(dev->CreateShaderResourceView(uiTex_.Get(), nullptr, &uiSrv_)))) {
    ReleaseUiLayer();
    return false;
  }
  uiWidth_ = width;
  uiHeight_ = height;
  return true;
}

// ------------------------------------------------------------------ the passes

void D3D11Passes::CleanAndConvert(const ConvertParams& params, int srcWidth, int srcHeight,
                                  int outWidth, int outHeight, int historyWrite) {
  ID3D11DeviceContext* dc = NativeContext(*display_);

  // The crawl remover. Its own copy of the parameters, to tell the shaders
  // how far back the ring reaches; the ring moves on once per new frame,
  // however often the frame is drawn.
  ConvertCB cb = params;
  const bool rem = cb.remover != 0 && remOk_ && EnsureRemover(srcWidth, srcHeight);
  bool remRun = false;
  if (rem) {
    if (cb.remNew != 0 || remCount_ == 0) {
      remHead_ = (remHead_ + 1) % kRemRing;
      remOutHead_ = (remOutHead_ + 1) % kRemOut;
      remCount_ = std::min(remCount_ + 1, kRemRing);
      remRun = true;
    }
    cb.remHist = remCount_ - 1;
    // The crawl fix reads the remover's pictures as its history, so it can only
    // look back as far as those go.
    cb.histCount = std::min(cb.histCount, std::min(remCount_ - 1, (int)kHistoryDepth));
  } else {
    cb.remover = 0;
    remCount_ = 0;
  }

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (SUCCEEDED(dc->Map(cbConvert_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    memcpy(mapped.pData, &cb, sizeof(ConvertCB));
    dc->Unmap(cbConvert_.Get(), 0);
  }

  ID3D11ShaderResourceView* nullSrvs[25] = {};
  dc->IASetInputLayout(nullptr);
  dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  dc->VSSetShader(vs_.Get(), nullptr, 0);
  ID3D11Buffer* cbs[] = {cbConvert_.Get()};
  dc->PSSetConstantBuffers(0, 1, cbs);
  dc->RSSetState(raster_.Get());
  const float blendFactor[4] = {0, 0, 0, 0};
  dc->OMSetBlendState(blendOpaque_.Get(), blendFactor, 0xFFFFFFFF);

  // ---- The crawl remover, in source geometry ----
  //
  // Decode the new frame into the ring, fit the models against the ring's
  // history into the residual targets, then combine. Render targets are set
  // before the views that read the same pictures, so nothing is ever bound both
  // ways at once.
  if (remRun) {
    D3D11_VIEWPORT vp = {};
    vp.Width = (float)srcWidth;
    vp.Height = (float)srcHeight;
    vp.MaxDepth = 1.0f;
    dc->RSSetViewports(1, &vp);

    ID3D11RenderTargetView* rtv[kRemRs] = {remRtv_[remHead_].Get()};
    ID3D11ShaderResourceView* srvs[kRemRing + kRemRs] = {};
    dc->OMSetRenderTargets(1, rtv, nullptr);
    for (int i = 0; i < 3; ++i) srvs[i] = planeSrv_[i].Get();
    dc->PSSetShaderResources(0, 3, srvs);
    dc->PSSetShader(psRemDec_.Get(), nullptr, 0);
    dc->Draw(3, 0);
    dc->PSSetShaderResources(0, 3, nullSrvs);

    for (int i = 0; i < kRemRs; ++i) rtv[i] = remRtv_[kRemRing + i].Get();
    dc->OMSetRenderTargets(kRemRs, rtv, nullptr);
    for (int k = 0; k < kRemRing; ++k) {
      srvs[k] = remSrv_[(remHead_ - k + kRemRing) % kRemRing].Get();
    }
    dc->PSSetShaderResources(0, kRemRing, srvs);
    dc->PSSetShader(psRemRes_.Get(), nullptr, 0);
    dc->Draw(3, 0);

    rtv[0] = remRtv_[kRemRing + kRemRs + remOutHead_].Get();
    dc->OMSetRenderTargets(1, rtv, nullptr);
    for (int i = 0; i < kRemRs; ++i) srvs[kRemRing + i] = remSrv_[kRemRing + i].Get();
    dc->PSSetShaderResources(0, kRemRing + kRemRs, srvs);
    dc->PSSetShader(psRemComb_.Get(), nullptr, 0);
    dc->Draw(3, 0);
    dc->PSSetShaderResources(0, kRemRing + kRemRs, nullSrvs);
  }

  // ---- pass 1: decode the planes and clean the signal, in source geometry ----
  //
  // Deliberately before anything touches the field structure. A cleanup that
  // runs after a deinterlacer has to work out which source line the pixel in
  // front of it came from, and for the interpolating modes there is no single
  // answer -- which is how the same class of artefact kept coming back.
  //
  // Skipped when nothing it reads has changed; see CleanStale.
  if (remRun) InvalidateClean();
  if (CleanStale(cb)) {
    ID3D11RenderTargetView* rtv[] = {cleanRtv_.Get()};
    dc->OMSetRenderTargets(1, rtv, nullptr);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)srcWidth;
    vp.Height = (float)srcHeight;
    vp.MaxDepth = 1.0f;
    dc->RSSetViewports(1, &vp);

    // Newest history first, so the shader can treat them as "one frame ago",
    // "two frames ago", "three frames ago" without knowing where the ring
    // happens to stand. historyWrite points at the slot that will be
    // overwritten next, which is the oldest one.
    // 25 slots, the remover's picture at t24.
    static_assert(3 + kHistoryDepth * 3 <= 24, "the remover's picture sits at t24");
    ID3D11ShaderResourceView* srvs[25] = {};
    for (int i = 0; i < 3; ++i) srvs[i] = planeSrv_[i].Get();
    for (int h = 0; h < kHistoryDepth; ++h) {
      const int slot = (historyWrite - 1 - h + kHistoryDepth * 2) % kHistoryDepth;
      for (int i = 0; i < 3; ++i) srvs[3 + h * 3 + i] = planeHistSrv_[slot][i].Get();
    }
    if (rem) {
      srvs[24] = remSrv_[kRemRing + kRemRs + remOutHead_].Get();
      for (int h = 0; h < kHistoryDepth; ++h) {
        srvs[3 + h * 3] = remSrv_[kRemRing + kRemRs + (remOutHead_ - 1 - h + 2 * kRemOut) % kRemOut].Get();
      }
    }
    dc->PSSetShaderResources(0, 25, srvs);
    dc->PSSetShader(psClean_.Get(), nullptr, 0);
    dc->Draw(3, 0);
    dc->PSSetShaderResources(0, 25, nullSrvs);
  }

  // ---- pass 2: fields, cropping, line doubling, rotation ----
  {
    ID3D11RenderTargetView* rtv[] = {intermediateRtv_.Get()};
    dc->OMSetRenderTargets(1, rtv, nullptr);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)outWidth;
    vp.Height = (float)outHeight;
    vp.MaxDepth = 1.0f;
    dc->RSSetViewports(1, &vp);

    ID3D11ShaderResourceView* srvs[2] = {cleanSrv_.Get(), cleanPrevSrv_.Get()};
    dc->PSSetShaderResources(0, 2, srvs);
    dc->PSSetShader(psConvert_.Get(), nullptr, 0);
    dc->Draw(3, 0);
  }

  // The intermediate is about to be read by whatever comes next, so nothing may
  // still have it bound as a source.
  dc->PSSetShaderResources(0, 3, nullSrvs);
  gridValid_ = false;  // built from the picture just replaced
}

bool D3D11Passes::EnsureGrid(int width, int height) {
  if (gridTex_[0] && gridWidth_ == width && gridHeight_ == height) return true;
  for (int i = 0; i < 2; ++i) {
    gridRtv_[i].Reset();
    gridSrv_[i].Reset();
    gridTex_[i].Reset();
  }
  gridWidth_ = gridHeight_ = 0;
  ID3D11Device* dev = NativeDevice(*display_);
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)width;
  td.Height = (UINT)height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  for (int i = 0; i < 2; ++i) {
    if (FAILED(CAP_HR(dev->CreateTexture2D(&td, nullptr, &gridTex_[i]))) ||
        FAILED(CAP_HR(dev->CreateShaderResourceView(gridTex_[i].Get(), nullptr, &gridSrv_[i]))) ||
        FAILED(CAP_HR(dev->CreateRenderTargetView(gridTex_[i].Get(), nullptr, &gridRtv_[i])))) {
      for (int j = 0; j < 2; ++j) {
        gridRtv_[j].Reset();
        gridSrv_[j].Reset();
        gridTex_[j].Reset();
      }
      psGrid_.Reset();  // do not try again every frame
      CAP_ERR("Pixel grid: its pictures could not be made");
      return false;
    }
  }
  gridWidth_ = width;
  gridHeight_ = height;
  return true;
}

void D3D11Passes::RebuildGrid(const ScaleParams& params, int rowsPerLine, const int* stages,
                              int count) {
  gridValid_ = false;
  if (!psGrid_ || !intermediateSrv_ || params.nativeSpan <= 0.0f || count < 1) return;
  const int width =
      (int)std::ceil((params.srcSize[0] - params.nativeOffset) / params.nativeSpan) + 1;
  const int height = (int)params.srcSize[1];
  if (width < 2 || height < 1 || !EnsureGrid(width, height)) return;

  ID3D11DeviceContext* dc = NativeContext(*display_);
  dc->IASetInputLayout(nullptr);
  dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  dc->VSSetShader(vs_.Get(), nullptr, 0);
  dc->RSSetState(raster_.Get());
  const float blendFactor[4] = {0, 0, 0, 0};
  dc->OMSetBlendState(blendOpaque_.Get(), blendFactor, 0xFFFFFFFF);
  dc->PSSetShader(psGrid_.Get(), nullptr, 0);
  ID3D11Buffer* cbs[] = {cbScale_.Get()};
  dc->PSSetConstantBuffers(0, 1, cbs);
  D3D11_VIEWPORT vp = {};
  vp.Width = (float)width;
  vp.Height = (float)height;
  vp.MaxDepth = 1.0f;
  dc->RSSetViewports(1, &vp);

  // intermediate -> one picture, then back and forth, so that the last stage
  // lands in [0]. The target is set before the view that reads the other
  // picture, so neither is ever bound both ways.
  ScaleCB sc = {};
  sc.params = params;
  sc.params.grid[1] = width;
  sc.params.grid[2] = std::max(1, rowsPerLine);
  ID3D11ShaderResourceView* none[] = {nullptr};
  for (int i = 0; i < count; ++i) {
    sc.params.grid[0] = stages[i];
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(dc->Map(cbScale_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
      memcpy(mapped.pData, &sc, sizeof(sc));
      dc->Unmap(cbScale_.Get(), 0);
    }
    const int to = (count - 1 - i) & 1;
    dc->PSSetShaderResources(0, 1, none);
    ID3D11RenderTargetView* rtv[] = {gridRtv_[to].Get()};
    dc->OMSetRenderTargets(1, rtv, nullptr);
    ID3D11ShaderResourceView* srv[] = {i == 0 ? intermediateSrv_.Get()
                                              : gridSrv_[1 - to].Get()};
    dc->PSSetShaderResources(0, 1, srv);
    dc->Draw(3, 0);
  }
  dc->PSSetShaderResources(0, 1, none);
  dc->OMSetRenderTargets(0, nullptr, nullptr);
  gridValid_ = true;
  gridSpan_ = params.nativeSpan;
  gridOffset_ = params.nativeOffset;
}

void D3D11Passes::UseGrid(ScaleParams* params) const {
  params->grid[0] = 0;
  params->grid[1] = gridValid_ && params->nativeSpan == gridSpan_ &&
                            params->nativeOffset == gridOffset_ &&
                            (int)params->srcSize[1] == gridHeight_
                        ? gridWidth_
                        : 0;
  params->grid[2] = params->grid[3] = 0;
}

void D3D11Passes::ScaleToScreen(const ScaleParams& params, int x, int y, int width, int height) {
  ID3D11DeviceContext* dc = NativeContext(*display_);

  ScaleCB sc = {};
  sc.params = params;
  UseGrid(&sc.params);

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (SUCCEEDED(dc->Map(cbScale_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    memcpy(mapped.pData, &sc, sizeof(sc));
    dc->Unmap(cbScale_.Get(), 0);
  }

  // The same state the pass before it leaves behind, set again so this pass
  // does not depend on having run after that one.
  dc->IASetInputLayout(nullptr);
  dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  dc->VSSetShader(vs_.Get(), nullptr, 0);
  dc->RSSetState(raster_.Get());
  const float blendFactor[4] = {0, 0, 0, 0};
  dc->OMSetBlendState(blendOpaque_.Get(), blendFactor, 0xFFFFFFFF);

  ID3D11RenderTargetView* backbuffer[] = {NativeBackBuffer(*display_)};
  dc->OMSetRenderTargets(1, backbuffer, nullptr);

  D3D11_VIEWPORT vp = {};
  vp.MaxDepth = 1.0f;
  vp.TopLeftX = (float)x;
  vp.TopLeftY = (float)y;
  vp.Width = (float)width;
  vp.Height = (float)height;
  dc->RSSetViewports(1, &vp);

  ID3D11ShaderResourceView* scaleSrv[] = {
      intermediateSrv_.Get(), sc.params.grid[1] > 0 ? gridSrv_[0].Get() : nullptr};
  dc->PSSetShaderResources(0, 2, scaleSrv);
  ID3D11SamplerState* samplers[] = {sampPoint_.Get(), sampLinear_.Get()};
  dc->PSSetSamplers(0, 2, samplers);
  dc->PSSetShader(psScale_.Get(), nullptr, 0);
  ID3D11Buffer* scaleCbs[] = {cbScale_.Get()};
  dc->PSSetConstantBuffers(0, 1, scaleCbs);
  dc->Draw(3, 0);

  // Leave the pipeline clean so ImGui's own state setup starts from scratch.
  ID3D11ShaderResourceView* nullSrvs[2] = {};
  dc->PSSetShaderResources(0, 2, nullSrvs);

  // Restore the full window viewport for whatever draws next.
  vp.TopLeftX = 0;
  vp.TopLeftY = 0;
  vp.Width = (float)display_->width();
  vp.Height = (float)display_->height();
  dc->RSSetViewports(1, &vp);
}

bool D3D11Passes::Deliver(PassImage target, int width, int height, const ScaleParams& params) {
  if (!intermediate_) return false;
  if (!EnsureDelivery(width, height, target)) return false;

  ID3D11DeviceContext* dc = NativeContext(*display_);

  ScaleCB sc = {};
  sc.params = params;
  UseGrid(&sc.params);

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (SUCCEEDED(dc->Map(cbScale_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    memcpy(mapped.pData, &sc, sizeof(sc));
    dc->Unmap(cbScale_.Get(), 0);
  }

  ID3D11RenderTargetView* rtv[] = {delivery_[DeliveryIndex(target)].rtv.Get()};
  dc->OMSetRenderTargets(1, rtv, nullptr);

  D3D11_VIEWPORT vp = {};
  vp.Width = (float)width;
  vp.Height = (float)height;
  vp.MaxDepth = 1.0f;
  dc->RSSetViewports(1, &vp);

  dc->IASetInputLayout(nullptr);
  dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  dc->VSSetShader(vs_.Get(), nullptr, 0);
  dc->PSSetShader(psScale_.Get(), nullptr, 0);
  ID3D11ShaderResourceView* srv[] = {
      intermediateSrv_.Get(), sc.params.grid[1] > 0 ? gridSrv_[0].Get() : nullptr};
  dc->PSSetShaderResources(0, 2, srv);
  ID3D11SamplerState* samplers[] = {sampPoint_.Get(), sampLinear_.Get()};
  dc->PSSetSamplers(0, 2, samplers);
  ID3D11Buffer* cbs[] = {cbScale_.Get()};
  dc->PSSetConstantBuffers(0, 1, cbs);
  dc->RSSetState(raster_.Get());
  // Set here rather than inherited: whatever drew last may have left blending
  // on, and this pass writes a picture, not something to lay over one.
  const float blendFactor[4] = {0, 0, 0, 0};
  dc->OMSetBlendState(blendOpaque_.Get(), blendFactor, 0xFFFFFFFF);
  dc->Draw(3, 0);

  ID3D11ShaderResourceView* none[] = {nullptr, nullptr};
  dc->PSSetShaderResources(0, 2, none);
  return true;
}

bool D3D11Passes::BeginUiLayer(int width, int height) {
  if (!EnsureUiLayer(width, height)) return false;

  ID3D11DeviceContext* dc = NativeContext(*display_);
  const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  dc->ClearRenderTargetView(uiRtv_.Get(), clear);
  ID3D11RenderTargetView* rtv[] = {uiRtv_.Get()};
  dc->OMSetRenderTargets(1, rtv, nullptr);
  return true;
}

void D3D11Passes::CompositeUiLayer(float paperWhiteNits) {
  if (!uiSrv_) return;

  ID3D11DeviceContext* dc = NativeContext(*display_);

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (SUCCEEDED(dc->Map(cbUi_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    float value[4] = {paperWhiteNits, 0.0f, 0.0f, 0.0f};
    memcpy(mapped.pData, value, sizeof(value));
    dc->Unmap(cbUi_.Get(), 0);
  }

  ID3D11RenderTargetView* backbuffer[] = {NativeBackBuffer(*display_)};
  dc->OMSetRenderTargets(1, backbuffer, nullptr);

  D3D11_VIEWPORT vp = {};
  vp.Width = (float)display_->width();
  vp.Height = (float)display_->height();
  vp.MaxDepth = 1.0f;
  dc->RSSetViewports(1, &vp);

  dc->IASetInputLayout(nullptr);
  dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  dc->VSSetShader(vs_.Get(), nullptr, 0);
  dc->PSSetShader(psUiComposite_.Get(), nullptr, 0);
  ID3D11ShaderResourceView* srv[] = {uiSrv_.Get()};
  dc->PSSetShaderResources(0, 1, srv);
  ID3D11SamplerState* samp[] = {sampPoint_.Get()};
  dc->PSSetSamplers(0, 1, samp);
  ID3D11Buffer* cbs[] = {cbUi_.Get()};
  dc->PSSetConstantBuffers(0, 1, cbs);
  dc->RSSetState(raster_.Get());

  const float blendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  dc->OMSetBlendState(blendPremultiplied_.Get(), blendFactor, 0xffffffff);
  dc->Draw(3, 0);
  dc->OMSetBlendState(nullptr, blendFactor, 0xffffffff);

  ID3D11ShaderResourceView* none[] = {nullptr};
  dc->PSSetShaderResources(0, 1, none);
}

// ------------------------------------------------------------------------- yuv

void D3D11Passes::ReleaseYuvStream(int stream) {
  YuvStream& s = yuv_[stream];
  for (int i = 0; i < kReadbackSlots; ++i) s.staging[i].Reset();
  s.rtv.Reset();
  s.tex.Reset();
  s.width = 0;
  s.rows = 0;
  for (int i = 0; i < SharedFrames::kSlots; ++i) {
    s.sharedRtv[i].Reset();
    s.sharedMutex[i].Reset();
  }
  s.shared.reset();
}

bool D3D11Passes::CreateYuvStream(int stream, int width, int rows, bool sixteenBit) {
  ReleaseYuvStream(stream);
  if (width < 2 || rows < 3) return false;
  YuvStream& s = yuv_[stream];
  ID3D11Device* dev = NativeDevice(*display_);

  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)width;
  td.Height = (UINT)rows;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = sixteenBit ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R8_UINT;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  if (FAILED(CAP_HR(dev->CreateTexture2D(&td, nullptr, &s.tex))) ||
      FAILED(CAP_HR(dev->CreateRenderTargetView(s.tex.Get(), nullptr, &s.rtv)))) {
    ReleaseYuvStream(stream);
    return false;
  }

  td.Usage = D3D11_USAGE_STAGING;
  td.BindFlags = 0;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  for (int i = 0; i < kReadbackSlots; ++i) {
    if (FAILED(CAP_HR(dev->CreateTexture2D(&td, nullptr, &s.staging[i])))) {
      ReleaseYuvStream(stream);
      return false;
    }
  }
  s.width = width;
  s.rows = rows;
  return true;
}

void D3D11Passes::ConvertToYuv(int stream, int slot, PassImage from, const YuvParams& params) {
  YuvStream& s = yuv_[stream];
  DrawYuv(s.rtv.Get(), s.width, s.rows, from, params);
  dc()->CopyResource(s.staging[slot].Get(), s.tex.Get());
}

uint64_t D3D11Passes::AdapterId() const {
  ComPtr<IDXGIDevice> device;
  ComPtr<IDXGIAdapter> adapter;
  ComPtr<IDXGIAdapter1> adapter1;
  DXGI_ADAPTER_DESC1 desc = {};
  if (!display_ || FAILED(dev()->QueryInterface(IID_PPV_ARGS(&device))) ||
      FAILED(device->GetAdapter(&adapter)) || FAILED(adapter.As(&adapter1)) ||
      FAILED(adapter1->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
    return 0;
  }
  uint64_t luid = 0;
  memcpy(&luid, &desc.AdapterLuid, sizeof(luid));
  return luid;
}

bool D3D11Passes::AttachSharedYuv(int stream, std::shared_ptr<SharedFrames> frames) {
  ReleaseYuvStream(stream);
  if (!frames) return true;
  YuvStream& s = yuv_[stream];
  ComPtr<ID3D11Device1> dev1;
  if (FAILED(CAP_HR(dev()->QueryInterface(IID_PPV_ARGS(&dev1))))) return false;
  for (int i = 0; i < SharedFrames::kSlots; ++i) {
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(CAP_HR(dev1->OpenSharedResource1(frames->handle[i], IID_PPV_ARGS(&tex)))) ||
        FAILED(CAP_HR(tex.As(&s.sharedMutex[i]))) ||
        FAILED(CAP_HR(dev()->CreateRenderTargetView(tex.Get(), nullptr, &s.sharedRtv[i])))) {
      ReleaseYuvStream(stream);
      return false;
    }
  }
  s.shared = std::move(frames);
  return true;
}

bool D3D11Passes::ConvertToShared(int stream, int slot, PassImage from, const YuvParams& params) {
  YuvStream& s = yuv_[stream];
  if (!s.shared) return false;
  // A free slot was released with its key, so this does not wait. The other
  // key only after a hand-over that went wrong on the recorder's side.
  IDXGIKeyedMutex* mutex = s.sharedMutex[slot].Get();
  uint64_t key = s.shared->key[slot];
  if (mutex->AcquireSync(key, 0) != S_OK) {
    key = 1 - key;
    if (mutex->AcquireSync(key, 0) != S_OK) return false;
  }
  DrawYuv(s.sharedRtv[slot].Get(), s.shared->width, s.shared->rows, from, params);
  dc()->OMSetRenderTargets(0, nullptr, nullptr);
  mutex->ReleaseSync(1);
  s.shared->key[slot] = 1;
  return true;
}

void D3D11Passes::DrawYuv(ID3D11RenderTargetView* target, int width, int rows, PassImage from,
                          const YuvParams& params) {
  ID3D11DeviceContext* dc = NativeContext(*display_);

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (SUCCEEDED(dc->Map(cbYuv_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    memcpy(mapped.pData, &params, sizeof(params));
    dc->Unmap(cbYuv_.Get(), 0);
  }

  ID3D11RenderTargetView* rtv[] = {target};
  dc->OMSetRenderTargets(1, rtv, nullptr);

  D3D11_VIEWPORT vp = {};
  vp.Width = (float)width;
  vp.Height = (float)rows;
  vp.MaxDepth = 1.0f;
  dc->RSSetViewports(1, &vp);

  dc->IASetInputLayout(nullptr);
  dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  dc->VSSetShader(vs_.Get(), nullptr, 0);
  dc->PSSetShader(psYuv_.Get(), nullptr, 0);
  ID3D11ShaderResourceView* srv[] = {View(from)};
  dc->PSSetShaderResources(0, 1, srv);
  ID3D11Buffer* cbs[] = {cbYuv_.Get()};
  dc->PSSetConstantBuffers(0, 1, cbs);
  dc->RSSetState(raster_.Get());
  // The same as the delivery pass: opaque, whatever was bound before.
  const float blendFactor[4] = {0, 0, 0, 0};
  dc->OMSetBlendState(blendOpaque_.Get(), blendFactor, 0xFFFFFFFF);
  dc->Draw(3, 0);

  ID3D11ShaderResourceView* none[] = {nullptr};
  dc->PSSetShaderResources(0, 1, none);
}

bool D3D11Passes::MapYuv(int stream, int slot, MappedImage* mapped) {
  D3D11_MAPPED_SUBRESOURCE sub = {};
  if (FAILED(dc()->Map(yuv_[stream].staging[slot].Get(), 0, D3D11_MAP_READ,
                       D3D11_MAP_FLAG_DO_NOT_WAIT, &sub))) {
    return false;
  }
  mapped->data = (const uint8_t*)sub.pData;
  mapped->rowPitch = sub.RowPitch;
  return true;
}

void D3D11Passes::UnmapYuv(int stream, int slot) {
  dc()->Unmap(yuv_[stream].staging[slot].Get(), 0);
}

}  // namespace

std::unique_ptr<RenderPasses> CreateD3D11Passes() {
  return std::unique_ptr<RenderPasses>(new D3D11Passes());
}

}  // namespace cap
