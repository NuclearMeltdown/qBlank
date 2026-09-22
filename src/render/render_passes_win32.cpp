#include "render/render_passes.h"

#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>

#include "app_identity.h"
#include "common_win32.h"
#include "i18n.h"
#include "render/display_win32.h"
#include "render/shaders.h"

namespace cap {
namespace {

// The constant buffers as the shaders declare them: the interface's parameters
// followed by the padding a sixteen byte register needs. The parameters are the
// same fields in the same order, so this is a copy and not a conversion.
struct ConvertCB {
  ConvertParams params;
  int32_t pad[3];
};
static_assert(sizeof(ConvertCB) % 16 == 0, "constant buffer must be 16 byte aligned");

struct ScaleCB {
  ScaleParams params;
  int32_t pad[3];
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

// Where a compiled shader is kept between runs. The conversion shader has grown
// into several hundred lines of branching, and D3DCompile spends seconds on it
// at optimisation level three -- seconds the user waits through before the
// window appears, every single time, to arrive at byte-for-byte the same answer.
//
// The name carries a hash of the source and the target profile, so editing the
// shader or changing the profile simply misses the cache rather than loading
// something stale. A miss costs what it always cost; there is nothing to
// invalidate by hand.
std::wstring ShaderCachePath(const char* source, const char* target) {
  uint64_t hash = 1469598103934665603ull;  // FNV-1a
  for (const char* p = source; *p; ++p) {
    hash = (hash ^ (unsigned char)*p) * 1099511628211ull;
  }
  for (const char* p = target; *p; ++p) {
    hash = (hash ^ (unsigned char)*p) * 1099511628211ull;
  }
  wchar_t name[64];
  ::swprintf(name, 64, L"shader-%016llx.cso", (unsigned long long)hash);
  return ExeDirectory() + name;
}

ComPtr<ID3DBlob> LoadCachedShader(const std::wstring& path) {
  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return nullptr;
  LARGE_INTEGER size = {};
  ComPtr<ID3DBlob> blob;
  if (::GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 22) &&
      SUCCEEDED(::D3DCreateBlob((SIZE_T)size.QuadPart, &blob))) {
    DWORD read = 0;
    if (!::ReadFile(file, blob->GetBufferPointer(), (DWORD)size.QuadPart, &read, nullptr) ||
        read != size.QuadPart) {
      blob.Reset();
    }
  }
  ::CloseHandle(file);
  return blob;
}

void StoreCachedShader(const std::wstring& path, ID3DBlob* code) {
  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;  // read-only folder: compile every time, no harm
  DWORD written = 0;
  ::WriteFile(file, code->GetBufferPointer(), (DWORD)code->GetBufferSize(), &written, nullptr);
  ::CloseHandle(file);
}

// Was in diesem Lauf tatsaechlich gebraucht wurde. Alles andere neben der exe
// ist ein Rest aus einer aelteren Fassung des Shaders.
std::vector<std::wstring>& ShaderCacheInUse() {
  static std::vector<std::wstring> names;
  return names;
}

// Loescht die Dateien, die kein Shader dieser Fassung mehr beansprucht. Der
// Cache ist nach Inhalt benannt, eine geaenderte Quelle trifft also eine neue
// Datei und die alte bleibt sonst fuer immer liegen -- nach ein paar Releases
// steht da ein Dutzend toter Blobs.
void PruneShaderCache() {
  const std::wstring dir = ExeDirectory();
  WIN32_FIND_DATAW found = {};
  HANDLE search = ::FindFirstFileW((dir + L"shader-*.cso").c_str(), &found);
  if (search == INVALID_HANDLE_VALUE) return;
  int removed = 0;
  do {
    if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    const std::wstring path = dir + found.cFileName;
    bool live = false;
    for (const std::wstring& used : ShaderCacheInUse()) {
      if (_wcsicmp(used.c_str(), path.c_str()) == 0) {
        live = true;
        break;
      }
    }
    // Ein Fehlschlag ist keiner: liegt die Datei fest, weil eine zweite Instanz
    // sie gerade liest, ist der naechste Start wieder an der Reihe.
    if (!live && ::DeleteFileW(path.c_str())) ++removed;
  } while (::FindNextFileW(search, &found));
  ::FindClose(search);
  if (removed > 0) CAP_LOG("Shader cache: %d stale file(s) removed", removed);
}

ComPtr<ID3DBlob> CompileShader(const char* source, const char* target, std::string* error) {
  const std::wstring cachePath = ShaderCachePath(source, target);
  ShaderCacheInUse().push_back(cachePath);
  if (ComPtr<ID3DBlob> cached = LoadCachedShader(cachePath)) return cached;

  UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS;
  ComPtr<ID3DBlob> code;
  ComPtr<ID3DBlob> errors;
  const DWORD started = ::GetTickCount();
  HRESULT hr = ::D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, "main", target,
                            flags, 0, &code, &errors);
  if (SUCCEEDED(hr)) {
    CAP_LOG("Shader %s compiled in %lu ms, cached", target,
            (unsigned long)(::GetTickCount() - started));
    StoreCachedShader(cachePath, code.Get());
  }
  if (FAILED(hr)) {
    std::string detail = errors ? std::string((const char*)errors->GetBufferPointer(),
                                              errors->GetBufferSize())
                                : HrToString(hr);
    ReportError(error, CAP_SAID(T("Shader (", "Shader (") + std::string(target) +
                          T(") konnte nicht kompiliert werden: ", ") could not be compiled: ") + detail));
    CAP_ERR("Shader error: %s", detail.c_str());
    return nullptr;
  }
  return code;
}

class D3D11Passes : public RenderPasses {
 public:
  ~D3D11Passes() override { Shutdown(); }

  bool Initialize(Display* display, std::string* error) override;
  void Shutdown() override;

  bool CreatePlane(int index, int width, int height, PlaneFormat format) override;
  void ReleasePlanes() override;
  bool MapPlane(int index, MappedPlane* mapped) override;
  void UnmapPlane(int index) override;
  void PushHistory(int slot, int planeCount) override;

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
  bool Deliver(bool half, int width, int height, const ScaleParams& params) override;
  bool BeginUiLayer(int width, int height) override;
  void CompositeUiLayer(float paperWhiteNits) override;

  bool CreateReadbackSlots(int width, int height) override;
  void ReleaseReadbackSlots() override;
  void CopyToReadback(int slot, PassImage from) override;
  bool MapReadback(int slot, MappedImage* mapped) override;
  void UnmapReadback(int slot) override;

  bool hasHdrRecord() const override { return hdrRecTex_.Get() != nullptr; }
  int hdrRecordWidth() const override { return hdrRecWidth_; }
  int hdrRecordHeight() const override { return hdrRecHeight_; }
  bool CreateHdrRecord(int width, int height) override;
  void RecordHdr(PassImage from, float paperWhiteNits, int slot) override;
  bool MapHdrReadback(int slot, MappedImage* mapped) override;
  void UnmapHdrReadback(int slot) override;

  bool ReadStill(PassImage from, int width, int height, std::vector<uint8_t>* pixels) override;
  bool ReadStillHalf(PassImage from, int rows, std::vector<uint16_t>* out,
                     int* strideBytes) override;

 private:
  bool CreateShaders(std::string* error);
  bool CreateStates(std::string* error);
  bool EnsureDelivery(int width, int height, bool half);
  bool EnsureUiLayer(int width, int height);

  ID3D11Device* dev() const { return NativeDevice(*display_); }
  ID3D11DeviceContext* dc() const { return NativeContext(*display_); }

  ID3D11Texture2D* Texture(PassImage image) const {
    switch (image) {
      case PassImage::Delivery: return delivery_.Get();
      case PassImage::DeliveryHalf: return deliveryHalf_.Get();
      case PassImage::Intermediate:
      default: return intermediate_.Get();
    }
  }
  // Only the two the passes ever read from; the eight bit delivery target is
  // copied out rather than sampled.
  ID3D11ShaderResourceView* View(PassImage image) const {
    switch (image) {
      case PassImage::DeliveryHalf: return deliveryHalfSrv_.Get();
      case PassImage::Delivery: return nullptr;
      case PassImage::Intermediate:
      default: return intermediateSrv_.Get();
    }
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
  // not.
  ComPtr<ID3D11Texture2D> delivery_;
  ComPtr<ID3D11RenderTargetView> deliveryRtv_;
  int deliveryTexWidth_ = 0;
  int deliveryTexHeight_ = 0;
  // The same, still in linear light: a resample that must not be tone mapped
  // on the way, because a PQ recording and a wide screenshot come off it.
  ComPtr<ID3D11Texture2D> deliveryHalf_;
  ComPtr<ID3D11RenderTargetView> deliveryHalfRtv_;
  ComPtr<ID3D11ShaderResourceView> deliveryHalfSrv_;
  int deliveryHalfTexWidth_ = 0;
  int deliveryHalfTexHeight_ = 0;

  ComPtr<ID3D11Texture2D> readbackTex_[kReadbackSlots];

  ComPtr<ID3D11PixelShader> psHdrRecord_;
  ComPtr<ID3D11Buffer> cbRecord_;
  ComPtr<ID3D11Texture2D> hdrRecTex_;
  ComPtr<ID3D11RenderTargetView> hdrRecRtv_;
  ComPtr<ID3D11Texture2D> hdrReadbackTex_[kReadbackSlots];
  int hdrRecWidth_ = 0;
  int hdrRecHeight_ = 0;
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
  ReleaseReadbackSlots();
  ReleasePlanes();
  intermediateRtv_.Reset();
  intermediateSrv_.Reset();
  intermediate_.Reset();
  blendOpaque_.Reset();
  raster_.Reset();
  sampLinear_.Reset();
  sampPoint_.Reset();
  cbScale_.Reset();
  cbConvert_.Reset();
  psScale_.Reset();
  psConvert_.Reset();
  vs_.Reset();
  display_ = nullptr;
}

bool D3D11Passes::CreateShaders(std::string* error) {
  ID3D11Device* dev = NativeDevice(*display_);

  ComPtr<ID3DBlob> vsCode = CompileShader(kFullscreenVS, "vs_4_0", error);
  if (!vsCode) return false;
  if (FAILED(CAP_HR(dev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(),
                                            nullptr, &vs_)))) {
    ReportError(error, CAP_SAID(T("Vertex-Shader konnte nicht erstellt werden",
                                     "The vertex shader could not be created")));
    return false;
  }

  ComPtr<ID3DBlob> cleanCode = CompileShader(kCleanPS, "ps_4_0", error);
  if (!cleanCode) return false;
  if (FAILED(CAP_HR(dev->CreatePixelShader(cleanCode->GetBufferPointer(),
                                           cleanCode->GetBufferSize(), nullptr, &psClean_)))) {
    ReportError(error, CAP_SAID(T("Aufbereitungs-Shader konnte nicht erstellt werden",
                                     "The cleanup shader could not be created")));
    return false;
  }

  ComPtr<ID3DBlob> convertCode = CompileShader(kConvertPS, "ps_4_0", error);
  if (!convertCode) return false;
  if (FAILED(CAP_HR(dev->CreatePixelShader(convertCode->GetBufferPointer(),
                                           convertCode->GetBufferSize(), nullptr, &psConvert_)))) {
    ReportError(error, CAP_SAID(T("Konvertierungs-Shader konnte nicht erstellt werden",
                                     "The conversion shader could not be created")));
    return false;
  }

  ComPtr<ID3DBlob> scaleCode = CompileShader(kScalePS, "ps_4_0", error);
  if (!scaleCode) return false;
  if (FAILED(CAP_HR(dev->CreatePixelShader(scaleCode->GetBufferPointer(), scaleCode->GetBufferSize(),
                                           nullptr, &psScale_)))) {
    ReportError(error, CAP_SAID(T("Skalierungs-Shader konnte nicht erstellt werden",
                                     "The scaling shader could not be created")));
    return false;
  }

  ComPtr<ID3DBlob> recordCode = CompileShader(kHdrRecordPS, "ps_4_0", error);
  if (!recordCode) return false;
  if (FAILED(CAP_HR(dev->CreatePixelShader(recordCode->GetBufferPointer(),
                                           recordCode->GetBufferSize(), nullptr,
                                           &psHdrRecord_)))) {
    ReportError(error, CAP_SAID(T("Aufnahme-Shader konnte nicht erstellt werden",
                                     "The recording shader could not be created")));
    return false;
  }

  ComPtr<ID3DBlob> uiCode = CompileShader(kUiCompositePS, "ps_4_0", error);
  if (!uiCode) return false;
  if (FAILED(CAP_HR(dev->CreatePixelShader(uiCode->GetBufferPointer(), uiCode->GetBufferSize(),
                                           nullptr, &psUiComposite_)))) {
    ReportError(error, CAP_SAID(T("Oberflächen-Shader konnte nicht erstellt werden",
                                     "The interface shader could not be created")));
    return false;
  }

  // Erst hier, wo feststeht, welche Dateien diese Fassung braucht.
  PruneShaderCache();

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
  bd.ByteWidth = 16;  // one float plus padding
  if (FAILED(CAP_HR(dev->CreateBuffer(&bd, nullptr, &cbRecord_)))) {
    ReportError(error, CAP_SAID(T("Konstantenpuffer konnte nicht erstellt werden",
                                     "The constant buffer could not be created")));
    return false;
  }
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
  td.Usage = D3D11_USAGE_DYNAMIC;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(CAP_HR(dev->CreateTexture2D(&td, nullptr, &plane_[index])))) return false;
  if (FAILED(CAP_HR(dev->CreateShaderResourceView(plane_[index].Get(), nullptr,
                                                  &planeSrv_[index])))) {
    return false;
  }

  // The history copies. Written by the GPU rather than the CPU, so they are
  // plain default resources; they are allocated whether or not anything reads
  // them, because switching a filter on mid-stream should not have to
  // reallocate anything. Three copies of a 480 line frame is under two
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
  for (int i = 0; i < 3; ++i) {
    planeSrv_[i].Reset();
    plane_[i].Reset();
    for (int h = 0; h < kHistoryDepth; ++h) {
      planeHistSrv_[h][i].Reset();
      planeHist_[h][i].Reset();
    }
  }
}

bool D3D11Passes::MapPlane(int index, MappedPlane* mapped) {
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

// ------------------------------------------------- the pictures between passes

bool D3D11Passes::EnsureClean(int width, int height) {
  width = std::max(1, width);
  height = std::max(1, height);
  if (cleanTex_ && cleanWidth_ == width && cleanHeight_ == height) return true;

  cleanSrv_.Reset();
  cleanRtv_.Reset();
  cleanTex_.Reset();
  cleanPrevSrv_.Reset();
  cleanPrevTex_.Reset();
  cleanWidth_ = 0;
  cleanHeight_ = 0;

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
  if (FAILED(CAP_HR(dev->CreateTexture2D(&td, nullptr, &cleanTex_)))) return false;
  if (FAILED(CAP_HR(dev->CreateShaderResourceView(cleanTex_.Get(), nullptr, &cleanSrv_)))) {
    return false;
  }
  if (FAILED(CAP_HR(dev->CreateRenderTargetView(cleanTex_.Get(), nullptr, &cleanRtv_)))) {
    return false;
  }

  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(CAP_HR(dev->CreateTexture2D(&td, nullptr, &cleanPrevTex_)))) return false;
  if (FAILED(CAP_HR(dev->CreateShaderResourceView(cleanPrevTex_.Get(), nullptr,
                                                  &cleanPrevSrv_)))) {
    return false;
  }

  cleanWidth_ = width;
  cleanHeight_ = height;
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

  intermediateRtv_.Reset();
  intermediateSrv_.Reset();
  intermediate_.Reset();

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
  if (FAILED(CAP_HR(dev->CreateTexture2D(&td, nullptr, &intermediate_)))) return false;
  if (FAILED(CAP_HR(dev->CreateShaderResourceView(intermediate_.Get(), nullptr, &intermediateSrv_)))) {
    return false;
  }
  if (FAILED(CAP_HR(dev->CreateRenderTargetView(intermediate_.Get(), nullptr, &intermediateRtv_)))) {
    return false;
  }
  intermediateWidth_ = width;
  intermediateHeight_ = height;
  intermediateFormat_ = format;
  return true;
}

bool D3D11Passes::EnsureDelivery(int width, int height, bool half) {
  width = std::max(1, width);
  height = std::max(1, height);

  ComPtr<ID3D11Texture2D>& tex = half ? deliveryHalf_ : delivery_;
  ComPtr<ID3D11RenderTargetView>& rtv = half ? deliveryHalfRtv_ : deliveryRtv_;
  int& haveW = half ? deliveryHalfTexWidth_ : deliveryTexWidth_;
  int& haveH = half ? deliveryHalfTexHeight_ : deliveryTexHeight_;
  if (tex && haveW == width && haveH == height) return true;

  if (half) deliveryHalfSrv_.Reset();
  rtv.Reset();
  tex.Reset();
  haveW = 0;
  haveH = 0;

  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)width;
  td.Height = (UINT)height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = half ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  // The half float one is read again afterwards, by the shader that lays the PQ
  // curve over it. The eight bit one is only ever copied out.
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  if (half) td.BindFlags |= D3D11_BIND_SHADER_RESOURCE;

  ID3D11Device* dev = NativeDevice(*display_);
  if (FAILED(CAP_HR(dev->CreateTexture2D(&td, nullptr, &tex)))) return false;
  if (FAILED(CAP_HR(dev->CreateRenderTargetView(tex.Get(), nullptr, &rtv)))) {
    tex.Reset();
    return false;
  }
  if (half &&
      FAILED(CAP_HR(dev->CreateShaderResourceView(tex.Get(), nullptr, &deliveryHalfSrv_)))) {
    rtv.Reset();
    tex.Reset();
    return false;
  }
  haveW = width;
  haveH = height;
  return true;
}

bool D3D11Passes::EnsureUiLayer(int width, int height) {
  if (uiTex_ && uiWidth_ == width && uiHeight_ == height) return true;
  uiSrv_.Reset();
  uiRtv_.Reset();
  uiTex_.Reset();
  uiWidth_ = 0;
  uiHeight_ = 0;
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
  if (FAILED(CAP_HR(dev->CreateTexture2D(&td, nullptr, &uiTex_)))) return false;
  if (FAILED(CAP_HR(dev->CreateRenderTargetView(uiTex_.Get(), nullptr, &uiRtv_)))) return false;
  if (FAILED(CAP_HR(dev->CreateShaderResourceView(uiTex_.Get(), nullptr, &uiSrv_)))) return false;
  uiWidth_ = width;
  uiHeight_ = height;
  return true;
}

// ------------------------------------------------------------------ the passes

void D3D11Passes::CleanAndConvert(const ConvertParams& params, int srcWidth, int srcHeight,
                                  int outWidth, int outHeight, int historyWrite) {
  ID3D11DeviceContext* dc = NativeContext(*display_);

  ConvertCB cb = {};
  cb.params = params;

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (SUCCEEDED(dc->Map(cbConvert_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    memcpy(mapped.pData, &cb, sizeof(cb));
    dc->Unmap(cbConvert_.Get(), 0);
  }

  ID3D11ShaderResourceView* nullSrvs[3 + kHistoryDepth * 3] = {};
  dc->IASetInputLayout(nullptr);
  dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  dc->VSSetShader(vs_.Get(), nullptr, 0);
  ID3D11Buffer* cbs[] = {cbConvert_.Get()};
  dc->PSSetConstantBuffers(0, 1, cbs);
  dc->RSSetState(raster_.Get());
  const float blendFactor[4] = {0, 0, 0, 0};
  dc->OMSetBlendState(blendOpaque_.Get(), blendFactor, 0xFFFFFFFF);

  // ---- pass 1: decode the planes and clean the signal, in source geometry ----
  //
  // Deliberately before anything touches the field structure. A cleanup that
  // runs after a deinterlacer has to work out which source line the pixel in
  // front of it came from, and for the interpolating modes there is no single
  // answer -- which is how the same class of artefact kept coming back.
  {
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
    ID3D11ShaderResourceView* srvs[3 + kHistoryDepth * 3] = {};
    for (int i = 0; i < 3; ++i) srvs[i] = planeSrv_[i].Get();
    for (int h = 0; h < kHistoryDepth; ++h) {
      const int slot = (historyWrite - 1 - h + kHistoryDepth * 2) % kHistoryDepth;
      for (int i = 0; i < 3; ++i) srvs[3 + h * 3 + i] = planeHistSrv_[slot][i].Get();
    }
    dc->PSSetShaderResources(0, 3 + kHistoryDepth * 3, srvs);
    dc->PSSetShader(psClean_.Get(), nullptr, 0);
    dc->Draw(3, 0);
    dc->PSSetShaderResources(0, 3 + kHistoryDepth * 3, nullSrvs);
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
}

void D3D11Passes::ScaleToScreen(const ScaleParams& params, int x, int y, int width, int height) {
  ID3D11DeviceContext* dc = NativeContext(*display_);

  ScaleCB sc = {};
  sc.params = params;

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

  ID3D11ShaderResourceView* scaleSrv[] = {intermediateSrv_.Get()};
  dc->PSSetShaderResources(0, 1, scaleSrv);
  ID3D11SamplerState* samplers[] = {sampPoint_.Get(), sampLinear_.Get()};
  dc->PSSetSamplers(0, 2, samplers);
  dc->PSSetShader(psScale_.Get(), nullptr, 0);
  ID3D11Buffer* scaleCbs[] = {cbScale_.Get()};
  dc->PSSetConstantBuffers(0, 1, scaleCbs);
  dc->Draw(3, 0);

  // Leave the pipeline clean so ImGui's own state setup starts from scratch.
  ID3D11ShaderResourceView* nullSrvs[1] = {};
  dc->PSSetShaderResources(0, 1, nullSrvs);

  // Restore the full window viewport for whatever draws next.
  vp.TopLeftX = 0;
  vp.TopLeftY = 0;
  vp.Width = (float)display_->width();
  vp.Height = (float)display_->height();
  dc->RSSetViewports(1, &vp);
}

bool D3D11Passes::Deliver(bool half, int width, int height, const ScaleParams& params) {
  if (!intermediate_) return false;
  if (!EnsureDelivery(width, height, half)) return false;

  ID3D11DeviceContext* dc = NativeContext(*display_);

  ScaleCB sc = {};
  sc.params = params;

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (SUCCEEDED(dc->Map(cbScale_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    memcpy(mapped.pData, &sc, sizeof(sc));
    dc->Unmap(cbScale_.Get(), 0);
  }

  ID3D11RenderTargetView* rtv[] = {half ? deliveryHalfRtv_.Get() : deliveryRtv_.Get()};
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
  ID3D11ShaderResourceView* srv[] = {intermediateSrv_.Get()};
  dc->PSSetShaderResources(0, 1, srv);
  ID3D11SamplerState* samplers[] = {sampPoint_.Get(), sampLinear_.Get()};
  dc->PSSetSamplers(0, 2, samplers);
  ID3D11Buffer* cbs[] = {cbScale_.Get()};
  dc->PSSetConstantBuffers(0, 1, cbs);
  dc->RSSetState(raster_.Get());
  dc->Draw(3, 0);

  ID3D11ShaderResourceView* none[] = {nullptr};
  dc->PSSetShaderResources(0, 1, none);
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

// -------------------------------------------------------------------- readback

bool D3D11Passes::CreateReadbackSlots(int width, int height) {
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)width;
  td.Height = (UINT)height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_STAGING;
  td.BindFlags = 0;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

  for (int i = 0; i < kReadbackSlots; ++i) {
    if (FAILED(CAP_HR(NativeDevice(*display_)->CreateTexture2D(&td, nullptr, &readbackTex_[i])))) {
      return false;
    }
  }
  return true;
}

void D3D11Passes::ReleaseReadbackSlots() {
  for (int i = 0; i < kReadbackSlots; ++i) readbackTex_[i].Reset();
}

void D3D11Passes::CopyToReadback(int slot, PassImage from) {
  dc()->CopyResource(readbackTex_[slot].Get(), Texture(from));
}

bool D3D11Passes::MapReadback(int slot, MappedImage* mapped) {
  D3D11_MAPPED_SUBRESOURCE sub = {};
  HRESULT hr = dc()->Map(readbackTex_[slot].Get(), 0, D3D11_MAP_READ,
                         D3D11_MAP_FLAG_DO_NOT_WAIT, &sub);
  if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return false;
  if (FAILED(hr)) return false;
  mapped->data = (const uint8_t*)sub.pData;
  mapped->rowPitch = sub.RowPitch;
  return true;
}

void D3D11Passes::UnmapReadback(int slot) {
  dc()->Unmap(readbackTex_[slot].Get(), 0);
}

bool D3D11Passes::CreateHdrRecord(int width, int height) {
  width = std::max(1, width);
  height = std::max(1, height);

  hdrRecRtv_.Reset();
  hdrRecTex_.Reset();
  for (int i = 0; i < kReadbackSlots; ++i) hdrReadbackTex_[i].Reset();

  ID3D11Device* dev = NativeDevice(*display_);

  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)width;
  td.Height = (UINT)height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  if (FAILED(CAP_HR(dev->CreateTexture2D(&td, nullptr, &hdrRecTex_)))) return false;
  if (FAILED(CAP_HR(dev->CreateRenderTargetView(hdrRecTex_.Get(), nullptr, &hdrRecRtv_)))) {
    return false;
  }

  td.Usage = D3D11_USAGE_STAGING;
  td.BindFlags = 0;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  for (int i = 0; i < kReadbackSlots; ++i) {
    if (FAILED(CAP_HR(dev->CreateTexture2D(&td, nullptr, &hdrReadbackTex_[i])))) return false;
  }

  hdrRecWidth_ = width;
  hdrRecHeight_ = height;
  return true;
}

void D3D11Passes::RecordHdr(PassImage from, float paperWhiteNits, int slot) {
  ID3D11DeviceContext* dc = NativeContext(*display_);

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (SUCCEEDED(dc->Map(cbRecord_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    const float value[4] = {paperWhiteNits, 0.0f, 0.0f, 0.0f};
    memcpy(mapped.pData, value, sizeof(value));
    dc->Unmap(cbRecord_.Get(), 0);
  }

  ID3D11RenderTargetView* rtv[] = {hdrRecRtv_.Get()};
  dc->OMSetRenderTargets(1, rtv, nullptr);

  D3D11_VIEWPORT vp = {};
  vp.Width = (float)hdrRecWidth_;
  vp.Height = (float)hdrRecHeight_;
  vp.MaxDepth = 1.0f;
  dc->RSSetViewports(1, &vp);

  dc->IASetInputLayout(nullptr);
  dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  dc->VSSetShader(vs_.Get(), nullptr, 0);
  dc->PSSetShader(psHdrRecord_.Get(), nullptr, 0);
  ID3D11ShaderResourceView* srv[] = {View(from)};
  dc->PSSetShaderResources(0, 1, srv);
  ID3D11SamplerState* samp[] = {sampPoint_.Get()};
  dc->PSSetSamplers(0, 1, samp);
  ID3D11Buffer* cbs[] = {cbRecord_.Get()};
  dc->PSSetConstantBuffers(0, 1, cbs);
  dc->RSSetState(raster_.Get());
  dc->Draw(3, 0);

  ID3D11ShaderResourceView* none[] = {nullptr};
  dc->PSSetShaderResources(0, 1, none);

  dc->CopyResource(hdrReadbackTex_[slot].Get(), hdrRecTex_.Get());
}

bool D3D11Passes::MapHdrReadback(int slot, MappedImage* mapped) {
  D3D11_MAPPED_SUBRESOURCE sub = {};
  if (FAILED(dc()->Map(hdrReadbackTex_[slot].Get(), 0, D3D11_MAP_READ, 0, &sub))) return false;
  mapped->data = (const uint8_t*)sub.pData;
  mapped->rowPitch = sub.RowPitch;
  return true;
}

void D3D11Passes::UnmapHdrReadback(int slot) {
  dc()->Unmap(hdrReadbackTex_[slot].Get(), 0);
}

// ---------------------------------------------------------------------- stills

bool D3D11Passes::ReadStill(PassImage from, int width, int height,
                            std::vector<uint8_t>* pixels) {
  // A staging texture of its own rather than a slot from the readback ring: the
  // ring only exists while recording, and its slots are deliberately two frames
  // stale. A screenshot should be the picture that was on screen when the key
  // went down.
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)width;
  td.Height = (UINT)height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_STAGING;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

  ComPtr<ID3D11Texture2D> staging;
  if (FAILED(CAP_HR(NativeDevice(*display_)->CreateTexture2D(&td, nullptr, &staging)))) {
    return false;
  }

  ID3D11DeviceContext* dc = NativeContext(*display_);
  dc->CopyResource(staging.Get(), Texture(from));

  // Blocking map: D3D11_MAP_READ without DO_NOT_WAIT flushes and waits.
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (FAILED(CAP_HR(dc->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))) return false;

  const size_t rowBytes = (size_t)width * 4;
  pixels->resize(rowBytes * (size_t)height);
  const uint8_t* src = (const uint8_t*)mapped.pData;
  for (int y = 0; y < height; ++y) {
    uint8_t* dst = pixels->data() + (size_t)y * rowBytes;
    memcpy(dst, src + (size_t)y * (size_t)mapped.RowPitch, rowBytes);
    // The pipeline carries an alpha channel it has no use for. Whatever ended up
    // in it, a screenshot of opaque video is opaque.
    for (size_t x = 3; x < rowBytes; x += 4) dst[x] = 0xFF;
  }
  dc->Unmap(staging.Get(), 0);
  return true;
}

bool D3D11Passes::ReadStillHalf(PassImage from, int rows, std::vector<uint16_t>* out,
                                int* strideBytes) {
  ID3D11Texture2D* source = Texture(from);
  if (!source) return false;

  D3D11_TEXTURE2D_DESC td = {};
  source->GetDesc(&td);
  td.Usage = D3D11_USAGE_STAGING;
  td.BindFlags = 0;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  td.MiscFlags = 0;

  ComPtr<ID3D11Texture2D> staging;
  if (FAILED(CAP_HR(NativeDevice(*display_)->CreateTexture2D(&td, nullptr, &staging)))) {
    return false;
  }

  ID3D11DeviceContext* dc = NativeContext(*display_);
  dc->CopyResource(staging.Get(), source);

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (FAILED(CAP_HR(dc->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))) return false;

  out->resize((size_t)mapped.RowPitch / sizeof(uint16_t) * rows);
  memcpy(out->data(), mapped.pData, out->size() * sizeof(uint16_t));
  dc->Unmap(staging.Get(), 0);

  *strideBytes = (int)mapped.RowPitch;
  return true;
}

}  // namespace

std::unique_ptr<RenderPasses> CreateRenderPasses() {
  return std::unique_ptr<RenderPasses>(new D3D11Passes());
}

}  // namespace cap
