// The passes of render_passes.h on Vulkan, drawn with the display's device
// (display_vulkan.h) into its swapchain image.
//
// A port of render_passes_win32.cpp, and meant to stay one: the same passes in
// the same order with the same shaders, compiled to SPIR-V from the same HLSL.
// What differs is what Vulkan makes the caller do and Direct3D 11 does behind
// its back:
//
//   - Every image carries its layout, and each pass moves its sources and its
//     target into the one it needs (VulkanDevice::Transition). The barriers
//     that come with that are what orders one pass after the other.
//   - A pass's constants and images are one descriptor set from an arena that
//     is reset once the GPU is done with the frame that used it. That is what
//     D3D11_MAP_WRITE_DISCARD does inside the driver.
//   - A plane is written into one of two host visible buffers and copied into
//     its image on the device. A DYNAMIC texture is the same thing done by the
//     driver.
//   - A readback is a copy into a host visible buffer, finished when the
//     submission that carries it is.
//   - There is a pipeline for each shader and target format. The ones a frame
//     needs are made up front, so the first frame does not wait for the
//     driver's compiler.
//
// Nothing here waits for the GPU on the way through a frame. The only waits
// are the ones render_passes.h allows: the stills, the ten bit recording's map
// and a second plane upload before anything was submitted.

#include "render/render_passes.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "common.h"
#include "i18n.h"
#include "inflate.h"
#include "render/display.h"
#include "render/display_vulkan.h"
// Made and packed by the build from the HLSL in shaders.h (CMakeLists.txt).
#include "shaders_spirv.h"

namespace cap {
namespace {

// The constant buffers as the shaders declare them, as in the Direct3D 11
// passes: the parameters, then the padding to sixteen bytes.
using ConvertCB = ConvertParams;  // fills its last register exactly
static_assert(sizeof(ConvertCB) % 16 == 0, "constant buffer must be 16 byte aligned");

struct ScaleCB {
  ScaleParams params;
  int32_t pad[3];
};
static_assert(sizeof(ScaleCB) % 16 == 0, "constant buffer must be 16 byte aligned");

// The recording and interface shaders are told one value: the nits diffuse
// white comes out at.
struct NitsCB {
  float nits;
  float pad[3];
};
static_assert(sizeof(NitsCB) == 16, "constant buffer must be 16 bytes");

VkFormat PlaneVkFormat(PlaneFormat format) {
  switch (format) {
    case PlaneFormat::R8: return VK_FORMAT_R8_UNORM;
    case PlaneFormat::Rg8: return VK_FORMAT_R8G8_UNORM;
    case PlaneFormat::R16: return VK_FORMAT_R16_UNORM;
    case PlaneFormat::Rg16: return VK_FORMAT_R16G16_UNORM;
    case PlaneFormat::Bgra8: return VK_FORMAT_B8G8R8A8_UNORM;
    case PlaneFormat::Rgba8:
    default: return VK_FORMAT_R8G8B8A8_UNORM;
  }
}

// Bytes per pixel of the formats used here.
int TexelBytes(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
    case VK_FORMAT_R8_UNORM: return 1;
    case VK_FORMAT_R8G8_UNORM:
    case VK_FORMAT_R16_UNORM: return 2;
    default: return 4;
  }
}

VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

// The one descriptor set layout all passes share, as the shaders are compiled
// for it (CMakeLists.txt, -fvk-t-shift 0 -fvk-s-shift 16 -fvk-b-shift 32):
// textures from binding 0, samplers from 16, the constant buffer at 32. The
// clean pass reads twelve textures, the planes and three frames of history.
// The others read fewer, and a black image of one pixel fills the rest, as an
// empty slot reads black in Direct3D 11.
const uint32_t kImageBindings = 3 + RenderPasses::kHistoryDepth * 3;
static_assert(kImageBindings == 12, "the clean shader reads twelve textures");
const uint32_t kPointBinding = 16;
const uint32_t kLinearBinding = 17;
const uint32_t kUniformBinding = 32;

// Descriptor sets per pool. A frame takes six at most.
const uint32_t kSetsPerArena = 32;

class VulkanPasses : public RenderPasses {
 public:
  ~VulkanPasses() override { Shutdown(); }

  GraphicsApi api() const override { return GraphicsApi::Vulkan; }

  bool Initialize(Display* display, std::string* error) override;
  void Shutdown() override;

  bool CreatePlane(int index, int width, int height, PlaneFormat format) override;
  void ReleasePlanes() override;
  bool MapPlane(int index, MappedPlane* mapped) override;
  void UnmapPlane(int index) override;
  void PushHistory(int slot, int planeCount) override;

  bool EnsureClean(int width, int height) override;
  bool EnsureIntermediate(int width, int height, bool wide) override;
  bool hasIntermediate() const override { return (bool)intermediate_; }
  int intermediateWidth() const override { return intermediate_.width; }
  int intermediateHeight() const override { return intermediate_.height; }
  bool intermediateWide() const override {
    return intermediate_.format == VK_FORMAT_R16G16B16A16_SFLOAT;
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
  // The buffers stay mapped for their life.
  void UnmapReadback(int slot) override {}

  bool hasHdrRecord() const override { return (bool)hdrRec_; }
  int hdrRecordWidth() const override { return hdrRec_.width; }
  int hdrRecordHeight() const override { return hdrRec_.height; }
  bool CreateHdrRecord(int width, int height) override;
  void RecordHdr(PassImage from, float paperWhiteNits, int slot) override;
  bool MapHdrReadback(int slot, MappedImage* mapped) override;
  void UnmapHdrReadback(int slot) override {}

  bool ReadStill(PassImage from, int width, int height, std::vector<uint8_t>* pixels) override;
  bool ReadStillHalf(PassImage from, int rows, std::vector<uint16_t>* out,
                     int* strideBytes) override;

 private:
  // One per shader, target format and blend. A failed one is kept as null, so
  // it is reported once and not tried again every frame.
  struct Pipeline {
    VkShaderModule fs;
    VkFormat format;
    bool blend;
    VkPipeline pipeline;
  };

  // A descriptor pool and the constants its sets point at, used by one
  // recording at a time and reset once the GPU has finished that one.
  struct Arena {
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VulkanBuffer uniforms;
    VkDeviceSize used = 0;
    uint32_t sets = 0;
    uint64_t serial = 0;
  };

  struct Plane {
    VulkanImage image;
    VulkanImage history[kHistoryDepth];
    // Two, so the next frame's upload can be written while the GPU still copies
    // the last one.
    VulkanBuffer staging[2];
    uint64_t stagingSerial[2] = {};
    int mapped = -1;
    size_t rowPitch = 0;
  };

  // A host visible buffer holding one picture, tightly packed, and the
  // submission that last copied into it.
  struct Readback {
    VulkanBuffer buffer;
    uint64_t serial = 0;
    int width = 0;
    int height = 0;
  };

  // `words` is only room to unpack into, handed around so the six share it.
  bool CreateModule(const Packed& packed, std::vector<uint32_t>* words, VkShaderModule* out);
  VkPipeline PipelineFor(VkShaderModule fs, VkFormat format, bool blend);
  Arena* ArenaFor(VkDeviceSize need);
  VkDescriptorSet AllocateSet(VulkanImage* const* images, const void* constants, size_t size);
  // One pass: the full screen triangle with `fs` into `target`, the viewport at
  // (x, y) with that size, the sources on bindings 0.. in order.
  void Draw(VkShaderModule fs, VulkanImage* target, int x, int y, int width, int height,
            VulkanImage* const* sources, int count, const void* constants, size_t size,
            bool blend = false);

  // Zero, through a transfer. For the images that are never drawn into.
  void ClearImage(VkCommandBuffer cmd, VulkanImage* image);
  void CopyImage(VkCommandBuffer cmd, VulkanImage* from, VulkanImage* to);
  bool CreateReadback(int width, int height, Readback* out);
  // Copies as much of `from` as fits into `to` and makes it readable by the
  // CPU once this recording has finished.
  bool CopyOut(VkCommandBuffer cmd, VulkanImage* from, Readback* to);
  void ReleaseReadback(Readback* readback);

  VulkanImage* Image(PassImage image) {
    switch (image) {
      case PassImage::Delivery: return &delivery_;
      case PassImage::DeliveryHalf: return &deliveryHalf_;
      case PassImage::Intermediate:
      default: return &intermediate_;
    }
  }
  // Only the two the passes ever read from, as in Direct3D 11: the eight bit
  // delivery target is copied out rather than sampled.
  VulkanImage* Sampled(PassImage image) {
    return image == PassImage::Delivery ? nullptr : Image(image);
  }

  bool EnsureDelivery(int width, int height, bool half);
  bool EnsureUiLayer(int width, int height);
  void ReleasePlane(int index);
  void ReleaseClean();
  void ReleaseIntermediate();
  void ReleaseDelivery(bool half);
  void ReleaseUiLayer();
  void ReleaseHdrRecord();
  // Drops every handle without destroying it, for when the device went first.
  void Forget();

  Display* display_ = nullptr;
  VulkanDevice* vk_ = nullptr;
  // The device the objects below were made on, to see that it is still there.
  VkDevice device_ = VK_NULL_HANDLE;

  VkShaderModule vs_ = VK_NULL_HANDLE;
  VkShaderModule fsClean_ = VK_NULL_HANDLE;
  VkShaderModule fsConvert_ = VK_NULL_HANDLE;
  VkShaderModule fsScale_ = VK_NULL_HANDLE;
  VkShaderModule fsRecord_ = VK_NULL_HANDLE;
  VkShaderModule fsUi_ = VK_NULL_HANDLE;
  VkSampler samplerPoint_ = VK_NULL_HANDLE;
  VkSampler samplerLinear_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
  std::vector<Pipeline> pipelines_;

  std::vector<Arena> arenas_;
  uint64_t arenaSerial_ = 0;
  VkDeviceSize uniformAlign_ = 256;

  VulkanImage dummy_;
  Plane planes_[3];
  VulkanImage clean_;
  VulkanImage cleanPrev_;
  VulkanImage intermediate_;
  VulkanImage delivery_;
  VulkanImage deliveryHalf_;
  VulkanImage uiLayer_;

  Readback readback_[kReadbackSlots];
  VulkanImage hdrRec_;
  Readback hdrReadback_[kReadbackSlots];
};

// ------------------------------------------------------------------- lifetime

bool VulkanPasses::CreateModule(const Packed& packed, std::vector<uint32_t>* words,
                                VkShaderModule* out) {
  // Unpacked into words: that is how Vulkan takes SPIR-V, aligned as such.
  words->resize(packed.unpackedSize / 4);
  if (!Inflate(packed, reinterpret_cast<uint8_t*>(words->data()))) {
    *out = VK_NULL_HANDLE;
    CAP_ERR("Vulkan: a shader did not unpack");
    return false;
  }
  VkShaderModuleCreateInfo info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = packed.unpackedSize;
  info.pCode = words->data();
  const VkResult result = vkCreateShaderModule(vk_->device(), &info, nullptr, out);
  if (result != VK_SUCCESS) {
    *out = VK_NULL_HANDLE;
    CAP_ERR("Vulkan: shader module failed: %s", VkResultName(result));
    return false;
  }
  return true;
}

bool VulkanPasses::Initialize(Display* display, std::string* error) {
  Shutdown();
  VulkanDevice* vk = display ? NativeVulkan(*display) : nullptr;
  if (!vk) return ReportError(error, CAP_SAID(T("Kein Vulkan-Gerät", "No Vulkan device")));
  display_ = display;
  vk_ = vk;
  device_ = vk->device();
  VkDevice device = device_;

  struct Module {
    const Packed& code;
    VkShaderModule* out;
    const char* de;
    const char* en;
  };
  const Module modules[] = {
      {kSpirvFullscreenVS, &vs_, "Vertex-Shader konnte nicht erstellt werden",
       "The vertex shader could not be created"},
      {kSpirvClean, &fsClean_, "Aufbereitungs-Shader konnte nicht erstellt werden",
       "The cleanup shader could not be created"},
      {kSpirvConvert, &fsConvert_, "Konvertierungs-Shader konnte nicht erstellt werden",
       "The conversion shader could not be created"},
      {kSpirvScale, &fsScale_, "Skalierungs-Shader konnte nicht erstellt werden",
       "The scaling shader could not be created"},
      {kSpirvHdrRecord, &fsRecord_, "Aufnahme-Shader konnte nicht erstellt werden",
       "The recording shader could not be created"},
      {kSpirvUiComposite, &fsUi_, "Oberflächen-Shader konnte nicht erstellt werden",
       "The interface shader could not be created"},
  };
  std::vector<uint32_t> words;
  for (const Module& module : modules) {
    if (!CreateModule(module.code, &words, module.out)) {
      Shutdown();
      return ReportError(error, CAP_SAID(T(module.de, module.en)));
    }
  }

  // The same two as in Direct3D 11: clamped, one nearest and one linear.
  VkSamplerCreateInfo sampler = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler.maxLod = VK_LOD_CLAMP_NONE;
  sampler.magFilter = VK_FILTER_NEAREST;
  sampler.minFilter = VK_FILTER_NEAREST;
  sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  VkResult result = vkCreateSampler(device, &sampler, nullptr, &samplerPoint_);
  if (result == VK_SUCCESS) {
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    result = vkCreateSampler(device, &sampler, nullptr, &samplerLinear_);
  }
  if (result != VK_SUCCESS) {
    Shutdown();
    return ReportError(error, CAP_SAID(T("Sampler konnte nicht erstellt werden",
                                         "The sampler could not be created")));
  }

  // The samplers are part of the layout, so no set ever has to name them.
  VkDescriptorSetLayoutBinding bindings[kImageBindings + 3] = {};
  for (uint32_t i = 0; i < kImageBindings; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  bindings[kImageBindings] = {kPointBinding, VK_DESCRIPTOR_TYPE_SAMPLER, 1,
                              VK_SHADER_STAGE_FRAGMENT_BIT, &samplerPoint_};
  bindings[kImageBindings + 1] = {kLinearBinding, VK_DESCRIPTOR_TYPE_SAMPLER, 1,
                                  VK_SHADER_STAGE_FRAGMENT_BIT, &samplerLinear_};
  bindings[kImageBindings + 2] = {kUniformBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                  VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo setInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  setInfo.bindingCount = kImageBindings + 3;
  setInfo.pBindings = bindings;
  result = vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &setLayout_);
  if (result == VK_SUCCESS) {
    VkPipelineLayoutCreateInfo layoutInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout_;
    result = vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout_);
  }
  if (result != VK_SUCCESS) {
    Shutdown();
    return ReportError(error, CAP_SAID(T("Pipeline-Layout konnte nicht erstellt werden",
                                         "The pipeline layout could not be created")));
  }

  uniformAlign_ = std::max<VkDeviceSize>(
      16, vk->properties().limits.minUniformBufferOffsetAlignment);

  // The black image the unused texture slots read.
  VkCommandBuffer cmd = vk->Commands();
  if (!cmd || !vk->CreateImage(1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               &dummy_)) {
    Shutdown();
    return ReportError(error, CAP_SAID(T("Vulkan-Bild konnte nicht erstellt werden",
                                         "A Vulkan image could not be created")));
  }
  ClearImage(cmd, &dummy_);
  vk->Transition(cmd, &dummy_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  // What a frame draws with, made now rather than in the middle of the first
  // one. The display's own formats come first: the interface's, which the back
  // buffer has in SDR, and scRGB for HDR.
  struct Wanted {
    VkShaderModule fs;
    VkFormat format;
    bool blend;
  };
  const Wanted wanted[] = {
      {fsClean_, VK_FORMAT_R16G16B16A16_SFLOAT, false},
      {fsConvert_, VK_FORMAT_R8G8B8A8_UNORM, false},
      {fsConvert_, VK_FORMAT_R16G16B16A16_SFLOAT, false},
      {fsScale_, vk->uiFormat(), false},
      {fsScale_, VK_FORMAT_R16G16B16A16_SFLOAT, false},
      {fsScale_, VK_FORMAT_R8G8B8A8_UNORM, false},
      {fsRecord_, VK_FORMAT_A2B10G10R10_UNORM_PACK32, false},
      {fsUi_, VK_FORMAT_R16G16B16A16_SFLOAT, true},
  };
  for (const Wanted& want : wanted) {
    if (!PipelineFor(want.fs, want.format, want.blend)) {
      Shutdown();
      return ReportError(error, CAP_SAID(T("Vulkan-Pipeline konnte nicht erstellt werden",
                                           "A Vulkan pipeline could not be created")));
    }
  }

  if (!vk->Flush()) {
    Shutdown();
    return ReportError(error, CAP_SAID(T("Das Vulkan-Gerät reagiert nicht",
                                         "The Vulkan device does not respond")));
  }
  CAP_LOG("Vulkan passes ready (%zu pipelines)", pipelines_.size());
  return true;
}

void VulkanPasses::Shutdown() {
  if (!vk_) return;

  // Only while the device these were made on is still there. The display may
  // have gone first, and with it everything made from its device.
  const bool live = display_ && NativeVulkan(*display_) == vk_ && vk_->device() == device_;
  if (live) {
    ReleasePlanes();
    ReleaseClean();
    ReleaseIntermediate();
    ReleaseDelivery(false);
    ReleaseDelivery(true);
    ReleaseUiLayer();
    ReleaseReadbackSlots();
    ReleaseHdrRecord();
    vk_->DestroyImage(&dummy_);
    for (Arena& arena : arenas_) vk_->DestroyBuffer(&arena.uniforms);

    std::vector<VkPipeline> pipelines;
    for (const Pipeline& p : pipelines_) {
      if (p.pipeline) pipelines.push_back(p.pipeline);
    }
    std::vector<VkDescriptorPool> pools;
    for (const Arena& arena : arenas_) {
      if (arena.pool) pools.push_back(arena.pool);
    }
    const VkShaderModule modules[] = {vs_, fsClean_, fsConvert_, fsScale_, fsRecord_, fsUi_};
    std::vector<VkShaderModule> moduleList(std::begin(modules), std::end(modules));
    const VkPipelineLayout pipelineLayout = pipelineLayout_;
    const VkDescriptorSetLayout setLayout = setLayout_;
    const VkSampler samplers[2] = {samplerPoint_, samplerLinear_};
    vk_->Defer([pipelines, pools, moduleList, pipelineLayout, setLayout,
                samplers](VkDevice device) {
      for (VkPipeline pipeline : pipelines) vkDestroyPipeline(device, pipeline, nullptr);
      for (VkDescriptorPool pool : pools) vkDestroyDescriptorPool(device, pool, nullptr);
      if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
      if (setLayout) vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
      for (VkSampler sampler : samplers) {
        if (sampler) vkDestroySampler(device, sampler, nullptr);
      }
      for (VkShaderModule module : moduleList) {
        if (module) vkDestroyShaderModule(device, module, nullptr);
      }
    });
  }
  Forget();
}

void VulkanPasses::Forget() {
  display_ = nullptr;
  vk_ = nullptr;
  device_ = VK_NULL_HANDLE;
  vs_ = fsClean_ = fsConvert_ = fsScale_ = fsRecord_ = fsUi_ = VK_NULL_HANDLE;
  samplerPoint_ = samplerLinear_ = VK_NULL_HANDLE;
  setLayout_ = VK_NULL_HANDLE;
  pipelineLayout_ = VK_NULL_HANDLE;
  pipelines_.clear();
  arenas_.clear();
  arenaSerial_ = 0;
  dummy_ = VulkanImage();
  for (Plane& plane : planes_) plane = Plane();
  clean_ = cleanPrev_ = intermediate_ = VulkanImage();
  delivery_ = deliveryHalf_ = uiLayer_ = VulkanImage();
  for (Readback& readback : readback_) readback = Readback();
  hdrRec_ = VulkanImage();
  for (Readback& readback : hdrReadback_) readback = Readback();
}

// ------------------------------------------------------------------ machinery

VkPipeline VulkanPasses::PipelineFor(VkShaderModule fs, VkFormat format, bool blend) {
  for (const Pipeline& p : pipelines_) {
    if (p.fs == fs && p.format == format && p.blend == blend) return p.pipeline;
  }

  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs_;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fs;
  stages[1].pName = "main";

  // No vertices: the vertex shader makes the triangle from its index.
  VkPipelineVertexInputStateCreateInfo vertexInput = {
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo assembly = {
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo viewport = {
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster = {
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample = {
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  // Opaque, or the interface's layer laid over the picture: premultiplied, as
  // in Direct3D 11.
  VkPipelineColorBlendAttachmentState attachment = {};
  attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  if (blend) {
    attachment.blendEnable = VK_TRUE;
    attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.colorBlendOp = VK_BLEND_OP_ADD;
    attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.alphaBlendOp = VK_BLEND_OP_ADD;
  }
  VkPipelineColorBlendStateCreateInfo colorBlend = {
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  colorBlend.attachmentCount = 1;
  colorBlend.pAttachments = &attachment;

  const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamicStates;

  VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachmentFormats = &format;

  VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.pNext = &rendering;
  info.stageCount = 2;
  info.pStages = stages;
  info.pVertexInputState = &vertexInput;
  info.pInputAssemblyState = &assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pColorBlendState = &colorBlend;
  info.pDynamicState = &dynamic;
  info.layout = pipelineLayout_;

  VkPipeline pipeline = VK_NULL_HANDLE;
  const VkResult result =
      vkCreateGraphicsPipelines(vk_->device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
  if (result != VK_SUCCESS) {
    pipeline = VK_NULL_HANDLE;
    CAP_ERR("Vulkan: pipeline for format %d failed: %s", (int)format, VkResultName(result));
  }
  pipelines_.push_back({fs, format, blend, pipeline});
  return pipeline;
}

VulkanPasses::Arena* VulkanPasses::ArenaFor(VkDeviceSize need) {
  const uint64_t serial = vk_->RecordingSerial();
  // A new recording: the arenas the GPU is done with start over. Commands() has
  // waited for the one before, so that is all of them but this one's.
  if (serial != arenaSerial_) {
    arenaSerial_ = serial;
    for (Arena& arena : arenas_) {
      if (arena.sets > 0 && arena.serial != serial && vk_->Completed(arena.serial)) {
        vkResetDescriptorPool(vk_->device(), arena.pool, 0);
        arena.sets = 0;
        arena.used = 0;
      }
    }
  }
  for (Arena& arena : arenas_) {
    const bool ours = arena.sets == 0 || arena.serial == serial;
    if (ours && arena.sets < kSetsPerArena && arena.used + need <= arena.uniforms.size) {
      arena.serial = serial;
      return &arena;
    }
  }

  Arena arena;
  VkDescriptorPoolSize sizes[] = {
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kImageBindings * kSetsPerArena},
      {VK_DESCRIPTOR_TYPE_SAMPLER, 2 * kSetsPerArena},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kSetsPerArena},
  };
  VkDescriptorPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  poolInfo.maxSets = kSetsPerArena;
  poolInfo.poolSizeCount = 3;
  poolInfo.pPoolSizes = sizes;
  if (vkCreateDescriptorPool(vk_->device(), &poolInfo, nullptr, &arena.pool) != VK_SUCCESS) {
    CAP_ERR("Vulkan: descriptor pool failed");
    return nullptr;
  }
  const VkDeviceSize largest = std::max({sizeof(ConvertCB), sizeof(ScaleCB), sizeof(NitsCB)});
  const VkDeviceSize size = std::max(need, AlignUp(largest, uniformAlign_)) * kSetsPerArena;
  if (!vk_->CreateBuffer(size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VulkanMemory::Upload,
                         &arena.uniforms)) {
    vkDestroyDescriptorPool(vk_->device(), arena.pool, nullptr);
    CAP_ERR("Vulkan: constant buffer failed");
    return nullptr;
  }
  arena.serial = serial;
  arenas_.push_back(arena);
  return &arenas_.back();
}

VkDescriptorSet VulkanPasses::AllocateSet(VulkanImage* const* images, const void* constants,
                                          size_t size) {
  const VkDeviceSize need = AlignUp(size, uniformAlign_);
  Arena* arena = ArenaFor(need);
  if (!arena) return VK_NULL_HANDLE;

  VkDescriptorSetAllocateInfo alloc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = arena->pool;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &setLayout_;
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (vkAllocateDescriptorSets(vk_->device(), &alloc, &set) != VK_SUCCESS) return VK_NULL_HANDLE;

  const VkDeviceSize offset = arena->used;
  memcpy(arena->uniforms.mapped + offset, constants, size);
  arena->used += need;
  ++arena->sets;

  VkDescriptorImageInfo imageInfo[kImageBindings] = {};
  VkWriteDescriptorSet writes[kImageBindings + 1] = {};
  for (uint32_t i = 0; i < kImageBindings; ++i) {
    imageInfo[i].imageView = images[i]->view;
    imageInfo[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[i].pImageInfo = &imageInfo[i];
  }
  VkDescriptorBufferInfo bufferInfo = {arena->uniforms.buffer, offset, size};
  VkWriteDescriptorSet& uniform = writes[kImageBindings];
  uniform.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  uniform.dstSet = set;
  uniform.dstBinding = kUniformBinding;
  uniform.descriptorCount = 1;
  uniform.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  uniform.pBufferInfo = &bufferInfo;
  vkUpdateDescriptorSets(vk_->device(), kImageBindings + 1, writes, 0, nullptr);
  return set;
}

void VulkanPasses::Draw(VkShaderModule fs, VulkanImage* target, int x, int y, int width,
                        int height, VulkanImage* const* sources, int count,
                        const void* constants, size_t size, bool blend) {
  if (!target || !*target || width <= 0 || height <= 0) return;
  // The part of the viewport that lies on the target. The viewport itself may
  // reach past it, as a zoomed picture does.
  const int left = std::max(0, x);
  const int top = std::max(0, y);
  const int right = std::min(target->width, x + width);
  const int bottom = std::min(target->height, y + height);
  if (right <= left || bottom <= top) return;

  const VkPipeline pipeline = PipelineFor(fs, target->format, blend);
  VkCommandBuffer cmd = vk_->Commands();
  if (!pipeline || !cmd) return;

  VulkanImage* bound[kImageBindings];
  for (uint32_t i = 0; i < kImageBindings; ++i) {
    VulkanImage* source = (int)i < count ? sources[i] : nullptr;
    bound[i] = source && *source ? source : &dummy_;
    vk_->Transition(cmd, bound[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
  const VkDescriptorSet set = AllocateSet(bound, constants, size);
  if (!set) return;
  vk_->Transition(cmd, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

  // What was there before only matters where this pass does not cover it, or
  // where it is laid over it. None of the shaders discards, so a pass over the
  // whole target writes every pixel of it.
  const bool whole = left == 0 && top == 0 && right == target->width &&
                     bottom == target->height;
  VkRenderingAttachmentInfo attachment = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  attachment.imageView = target->view;
  attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attachment.loadOp = blend || !whole ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

  const VkRect2D area = {{left, top}, {(uint32_t)(right - left), (uint32_t)(bottom - top)}};
  VkRenderingInfo rendering = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea = area;
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachments = &attachment;
  vkCmdBeginRendering(cmd, &rendering);

  const VkViewport viewport = {(float)x, (float)y, (float)width, (float)height, 0.0f, 1.0f};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &area);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &set, 0,
                          nullptr);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRendering(cmd);
}

void VulkanPasses::ClearImage(VkCommandBuffer cmd, VulkanImage* image) {
  if (!cmd || !image || !*image) return;
  vk_->Transition(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  const VkClearColorValue zero = {};
  VkImageSubresourceRange range = {};
  range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  range.levelCount = 1;
  range.layerCount = 1;
  vkCmdClearColorImage(cmd, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
}

void VulkanPasses::CopyImage(VkCommandBuffer cmd, VulkanImage* from, VulkanImage* to) {
  if (!cmd || !from || !*from || !to || !*to) return;
  vk_->Transition(cmd, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  vk_->Transition(cmd, to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkImageCopy region = {};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.extent = {(uint32_t)std::min(from->width, to->width),
                   (uint32_t)std::min(from->height, to->height), 1};
  vkCmdCopyImage(cmd, from->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to->image,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

bool VulkanPasses::CreateReadback(int width, int height, Readback* out) {
  width = std::max(1, width);
  height = std::max(1, height);
  *out = Readback();
  if (!vk_->CreateBuffer((VkDeviceSize)width * (VkDeviceSize)height * 4,
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT, VulkanMemory::Readback,
                         &out->buffer)) {
    return false;
  }
  out->width = width;
  out->height = height;
  return true;
}

bool VulkanPasses::CopyOut(VkCommandBuffer cmd, VulkanImage* from, Readback* to) {
  if (!cmd || !from || !*from || !to->buffer) return false;
  // Four bytes a pixel, which is what the buffers hold.
  if (TexelBytes(from->format) != 4) return false;
  vk_->Transition(cmd, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  VkBufferImageCopy region = {};
  region.bufferRowLength = (uint32_t)to->width;
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {(uint32_t)std::min(from->width, to->width),
                        (uint32_t)std::min(from->height, to->height), 1};
  vkCmdCopyImageToBuffer(cmd, from->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         to->buffer.buffer, 1, &region);
  vk_->HostReadBarrier(cmd);
  to->serial = vk_->RecordingSerial();
  return true;
}

void VulkanPasses::ReleaseReadback(Readback* readback) {
  if (vk_) vk_->DestroyBuffer(&readback->buffer);
  *readback = Readback();
}

// -------------------------------------------------------------- source planes

bool VulkanPasses::CreatePlane(int index, int width, int height, PlaneFormat format) {
  if (!vk_ || index < 0 || index >= 3) return false;
  ReleasePlane(index);
  width = std::max(1, width);
  height = std::max(1, height);

  const VkFormat vkFormat = PlaneVkFormat(format);
  VkFormatProperties props = {};
  vkGetPhysicalDeviceFormatProperties(vk_->physical(), vkFormat, &props);
  if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
    CAP_ERR("Vulkan: plane format %d cannot be sampled on this GPU", (int)vkFormat);
    return false;
  }

  // The plane, and its history as in Direct3D 11: allocated whether or not
  // anything reads it, so switching a filter on never has to wait for memory.
  Plane& plane = planes_[index];
  const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  bool ok = vk_->CreateImage(width, height, vkFormat, usage, &plane.image);
  for (int h = 0; ok && h < kHistoryDepth; ++h) {
    ok = vk_->CreateImage(width, height, vkFormat, usage, &plane.history[h]);
  }
  plane.rowPitch = (size_t)width * (size_t)TexelBytes(vkFormat);
  for (int s = 0; ok && s < 2; ++s) {
    ok = vk_->CreateBuffer((VkDeviceSize)plane.rowPitch * (VkDeviceSize)height,
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VulkanMemory::Upload,
                           &plane.staging[s]);
  }
  VkCommandBuffer cmd = ok ? vk_->Commands() : VK_NULL_HANDLE;
  if (!cmd) {
    ReleasePlane(index);
    return false;
  }

  // Black until the first frame arrives, as a new texture is in Direct3D 11.
  ClearImage(cmd, &plane.image);
  vk_->Transition(cmd, &plane.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  for (VulkanImage& history : plane.history) {
    ClearImage(cmd, &history);
    vk_->Transition(cmd, &history, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
  return true;
}

void VulkanPasses::ReleasePlane(int index) {
  Plane& plane = planes_[index];
  if (vk_) {
    vk_->DestroyImage(&plane.image);
    for (VulkanImage& history : plane.history) vk_->DestroyImage(&history);
    for (VulkanBuffer& staging : plane.staging) vk_->DestroyBuffer(&staging);
  }
  plane = Plane();
}

void VulkanPasses::ReleasePlanes() {
  for (int i = 0; i < 3; ++i) ReleasePlane(i);
}

bool VulkanPasses::MapPlane(int index, MappedPlane* mapped) {
  if (!vk_ || index < 0 || index >= 3 || !planes_[index].image) return false;
  Plane& plane = planes_[index];
  // Begun first: that is where the previous submission is waited for, and
  // with it the copy out of the older buffer.
  if (!vk_->Commands()) return false;

  const uint64_t recording = vk_->RecordingSerial();
  int pick = -1;
  for (int s = 0; s < 2 && pick < 0; ++s) {
    if (plane.stagingSerial[s] != recording && vk_->Completed(plane.stagingSerial[s])) pick = s;
  }
  if (pick < 0) {
    // Both already carry a copy in this recording: the frame before was never
    // submitted, as when the window cannot show one. Send it off rather than
    // overwrite what it still has to copy.
    if (!vk_->Flush() || !vk_->Commands()) return false;
    pick = 0;
  }
  plane.mapped = pick;
  mapped->data = plane.staging[pick].mapped;
  mapped->rowPitch = plane.rowPitch;
  return true;
}

void VulkanPasses::UnmapPlane(int index) {
  if (!vk_ || index < 0 || index >= 3) return;
  Plane& plane = planes_[index];
  if (plane.mapped < 0 || !plane.image) return;
  VkCommandBuffer cmd = vk_->Commands();
  if (!cmd) return;

  const int s = plane.mapped;
  plane.mapped = -1;
  vk_->Transition(cmd, &plane.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkBufferImageCopy region = {};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {(uint32_t)plane.image.width, (uint32_t)plane.image.height, 1};
  vkCmdCopyBufferToImage(cmd, plane.staging[s].buffer, plane.image.image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  vk_->Transition(cmd, &plane.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  plane.stagingSerial[s] = vk_->RecordingSerial();
}

void VulkanPasses::PushHistory(int slot, int planeCount) {
  if (!vk_ || slot < 0 || slot >= kHistoryDepth) return;
  VkCommandBuffer cmd = vk_->Commands();
  if (!cmd) return;
  // The cleaned picture belongs to the frame about to be replaced, so this is
  // the moment it becomes the previous one.
  CopyImage(cmd, &clean_, &cleanPrev_);
  for (int i = 0; i < std::min(planeCount, 3); ++i) {
    CopyImage(cmd, &planes_[i].image, &planes_[i].history[slot]);
  }
}

// ------------------------------------------------- the pictures between passes

void VulkanPasses::ReleaseClean() {
  if (vk_) {
    vk_->DestroyImage(&clean_);
    vk_->DestroyImage(&cleanPrev_);
  }
}

void VulkanPasses::ReleaseIntermediate() {
  if (vk_) vk_->DestroyImage(&intermediate_);
}

void VulkanPasses::ReleaseDelivery(bool half) {
  if (vk_) vk_->DestroyImage(half ? &deliveryHalf_ : &delivery_);
}

void VulkanPasses::ReleaseUiLayer() {
  if (!vk_) return;
  if (vk_->uiTarget() == &uiLayer_) vk_->SetUiTarget(nullptr);
  vk_->DestroyImage(&uiLayer_);
}

bool VulkanPasses::EnsureClean(int width, int height) {
  if (!vk_) return false;
  width = std::max(1, width);
  height = std::max(1, height);
  if (clean_ && clean_.width == width && clean_.height == height) return true;

  ReleaseClean();
  const VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
  VkCommandBuffer cmd = vk_->Commands();
  // Here and in the other Ensure functions: a failure takes back what was
  // already made, so a half finished pair never passes for a finished one.
  if (!cmd ||
      !vk_->CreateImage(width, height, format,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        &clean_) ||
      !vk_->CreateImage(width, height, format,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        &cleanPrev_)) {
    ReleaseClean();
    return false;
  }
  // Both start black, as they do in Direct3D 11: the first frame's previous
  // one is copied out of clean_ before anything was drawn into it.
  const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  vk_->Clear(cmd, &clean_, zero);
  ClearImage(cmd, &cleanPrev_);
  return true;
}

bool VulkanPasses::EnsureIntermediate(int width, int height, bool wide) {
  if (!vk_) return false;
  width = std::max(1, width);
  height = std::max(1, height);
  const VkFormat format = wide ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
  if (intermediate_ && intermediate_.width == width && intermediate_.height == height &&
      intermediate_.format == format) {
    return true;
  }

  ReleaseIntermediate();
  VkCommandBuffer cmd = vk_->Commands();
  if (!cmd || !vk_->CreateImage(width, height, format,
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                &intermediate_)) {
    ReleaseIntermediate();
    return false;
  }
  const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  vk_->Clear(cmd, &intermediate_, zero);
  return true;
}

bool VulkanPasses::EnsureDelivery(int width, int height, bool half) {
  width = std::max(1, width);
  height = std::max(1, height);
  VulkanImage& image = half ? deliveryHalf_ : delivery_;
  if (image && image.width == width && image.height == height) return true;

  ReleaseDelivery(half);
  // The half float one is read again, by the shader that lays the PQ curve
  // over it. The eight bit one is only ever copied out.
  VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (half) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
  const VkFormat format = half ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
  if (!vk_->CreateImage(width, height, format, usage, &image)) {
    ReleaseDelivery(half);
    return false;
  }
  return true;
}

bool VulkanPasses::EnsureUiLayer(int width, int height) {
  if (!vk_) return false;
  // In the interface's own format, eight bits, as in Direct3D 11: it holds an
  // ordinary sRGB interface, and more precision than what drew it buys nothing.
  // It is also the format the interface's pipeline was made for.
  const VkFormat format = vk_->uiFormat();
  if (uiLayer_ && uiLayer_.width == width && uiLayer_.height == height &&
      uiLayer_.format == format) {
    return true;
  }
  ReleaseUiLayer();
  if (width <= 0 || height <= 0) return false;
  if (!vk_->CreateImage(width, height, format,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        &uiLayer_)) {
    ReleaseUiLayer();
    return false;
  }
  return true;
}

// ------------------------------------------------------------------ the passes

void VulkanPasses::CleanAndConvert(const ConvertParams& params, int srcWidth, int srcHeight,
                                   int outWidth, int outHeight, int historyWrite) {
  if (!vk_ || !clean_ || !intermediate_) return;

  const ConvertCB& cb = params;

  // ---- pass 1: decode the planes and clean the signal, in source geometry ----
  //
  // Newest history first, as in Direct3D 11, so the shader reads "one frame
  // ago", "two", "three" without knowing where the ring stands. historyWrite is
  // the slot the next copy goes into, which is the oldest.
  VulkanImage* sources[kImageBindings] = {};
  for (int i = 0; i < 3; ++i) sources[i] = &planes_[i].image;
  for (int h = 0; h < kHistoryDepth; ++h) {
    const int slot = (historyWrite - 1 - h + kHistoryDepth * 2) % kHistoryDepth;
    for (int i = 0; i < 3; ++i) sources[3 + h * 3 + i] = &planes_[i].history[slot];
  }
  Draw(fsClean_, &clean_, 0, 0, srcWidth, srcHeight, sources, (int)kImageBindings, &cb,
       sizeof(cb));

  // ---- pass 2: fields, cropping, line doubling, rotation ----
  VulkanImage* cleaned[2] = {&clean_, &cleanPrev_};
  Draw(fsConvert_, &intermediate_, 0, 0, outWidth, outHeight, cleaned, 2, &cb, sizeof(cb));
}

void VulkanPasses::ScaleToScreen(const ScaleParams& params, int x, int y, int width,
                                 int height) {
  if (!vk_ || !intermediate_) return;
  ScaleCB sc = {};
  sc.params = params;
  VulkanImage* source[1] = {&intermediate_};
  Draw(fsScale_, vk_->backBuffer(), x, y, width, height, source, 1, &sc, sizeof(sc));
}

bool VulkanPasses::Deliver(bool half, int width, int height, const ScaleParams& params) {
  if (!vk_ || !intermediate_) return false;
  if (!EnsureDelivery(width, height, half)) return false;
  ScaleCB sc = {};
  sc.params = params;
  VulkanImage* source[1] = {&intermediate_};
  VulkanImage* target = half ? &deliveryHalf_ : &delivery_;
  Draw(fsScale_, target, 0, 0, target->width, target->height, source, 1, &sc, sizeof(sc));
  return true;
}

bool VulkanPasses::BeginUiLayer(int width, int height) {
  if (!EnsureUiLayer(width, height)) return false;
  VkCommandBuffer cmd = vk_->Commands();
  if (!cmd) return false;
  const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  vk_->Clear(cmd, &uiLayer_, clear);
  vk_->SetUiTarget(&uiLayer_);
  return true;
}

void VulkanPasses::CompositeUiLayer(float paperWhiteNits) {
  if (!vk_) return;
  vk_->SetUiTarget(nullptr);
  VulkanImage* target = vk_->backBuffer();
  if (!uiLayer_ || !target) return;
  NitsCB cb = {};
  cb.nits = paperWhiteNits;
  VulkanImage* source[1] = {&uiLayer_};
  Draw(fsUi_, target, 0, 0, target->width, target->height, source, 1, &cb, sizeof(cb), true);
}

// -------------------------------------------------------------------- readback

bool VulkanPasses::CreateReadbackSlots(int width, int height) {
  if (!vk_) return false;
  ReleaseReadbackSlots();
  for (Readback& readback : readback_) {
    if (!CreateReadback(width, height, &readback)) {
      ReleaseReadbackSlots();
      return false;
    }
  }
  return true;
}

void VulkanPasses::ReleaseReadbackSlots() {
  for (Readback& readback : readback_) ReleaseReadback(&readback);
}

void VulkanPasses::CopyToReadback(int slot, PassImage from) {
  if (!vk_ || slot < 0 || slot >= kReadbackSlots) return;
  CopyOut(vk_->Commands(), Image(from), &readback_[slot]);
}

bool VulkanPasses::MapReadback(int slot, MappedImage* mapped) {
  if (!vk_ || slot < 0 || slot >= kReadbackSlots) return false;
  Readback& readback = readback_[slot];
  // Never waits: the slot asked for was queued two frames ago.
  if (!readback.buffer || !vk_->Completed(readback.serial)) return false;
  vk_->Invalidate(readback.buffer);
  mapped->data = readback.buffer.mapped;
  mapped->rowPitch = (size_t)readback.width * 4;
  return true;
}

void VulkanPasses::ReleaseHdrRecord() {
  if (vk_) vk_->DestroyImage(&hdrRec_);
  for (Readback& readback : hdrReadback_) ReleaseReadback(&readback);
}

bool VulkanPasses::CreateHdrRecord(int width, int height) {
  if (!vk_) return false;
  width = std::max(1, width);
  height = std::max(1, height);
  ReleaseHdrRecord();

  // Ten bits a channel and two of alpha, R in the lowest bits: the byte order
  // of DXGI's R10G10B10A2_UNORM, which Vulkan spells from the other end.
  if (!vk_->CreateImage(width, height, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        &hdrRec_)) {
    ReleaseHdrRecord();
    return false;
  }
  for (Readback& readback : hdrReadback_) {
    if (!CreateReadback(width, height, &readback)) {
      ReleaseHdrRecord();
      return false;
    }
  }
  return true;
}

void VulkanPasses::RecordHdr(PassImage from, float paperWhiteNits, int slot) {
  if (!vk_ || !hdrRec_ || slot < 0 || slot >= kReadbackSlots) return;
  NitsCB cb = {};
  cb.nits = paperWhiteNits;
  VulkanImage* source[1] = {Sampled(from)};
  Draw(fsRecord_, &hdrRec_, 0, 0, hdrRec_.width, hdrRec_.height, source, 1, &cb, sizeof(cb));
  CopyOut(vk_->Commands(), &hdrRec_, &hdrReadback_[slot]);
}

bool VulkanPasses::MapHdrReadback(int slot, MappedImage* mapped) {
  if (!vk_ || slot < 0 || slot >= kReadbackSlots) return false;
  Readback& readback = hdrReadback_[slot];
  if (!readback.buffer || !vk_->Wait(readback.serial)) return false;
  vk_->Invalidate(readback.buffer);
  mapped->data = readback.buffer.mapped;
  mapped->rowPitch = (size_t)readback.width * 4;
  return true;
}

// ---------------------------------------------------------------------- stills

bool VulkanPasses::ReadStill(PassImage from, int width, int height,
                             std::vector<uint8_t>* pixels) {
  if (!vk_) return false;
  // A buffer of its own rather than a slot from the readback ring, as in
  // Direct3D 11: the ring only exists while recording, and its slots are two
  // frames old. A screenshot is the picture on screen when the key went down.
  Readback staging;
  if (!CreateReadback(width, height, &staging)) return false;
  const bool copied = CopyOut(vk_->Commands(), Image(from), &staging);
  if (!copied || !vk_->Flush()) {
    ReleaseReadback(&staging);
    return false;
  }
  vk_->Invalidate(staging.buffer);

  const size_t rowBytes = (size_t)staging.width * 4;
  pixels->resize(rowBytes * (size_t)staging.height);
  const uint8_t* src = staging.buffer.mapped;
  for (int y = 0; y < staging.height; ++y) {
    uint8_t* dst = pixels->data() + (size_t)y * rowBytes;
    memcpy(dst, src + (size_t)y * rowBytes, rowBytes);
    // Whatever ended up in the alpha channel, a screenshot of opaque video is
    // opaque.
    for (size_t x = 3; x < rowBytes; x += 4) dst[x] = 0xFF;
  }
  ReleaseReadback(&staging);
  return true;
}

bool VulkanPasses::ReadStillHalf(PassImage from, int rows, std::vector<uint16_t>* out,
                                 int* strideBytes) {
  if (!vk_) return false;
  VulkanImage* source = Image(from);
  if (!source || !*source || TexelBytes(source->format) != 8) return false;
  VkCommandBuffer cmd = vk_->Commands();
  if (!cmd) return false;

  // The whole picture, tightly packed: the stride is the picture's own width.
  const size_t pitch = (size_t)source->width * 8;
  rows = std::max(0, std::min(rows, source->height));
  VulkanBuffer staging;
  if (!vk_->CreateBuffer((VkDeviceSize)pitch * (VkDeviceSize)source->height,
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT, VulkanMemory::Readback, &staging)) {
    return false;
  }
  vk_->Transition(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  VkBufferImageCopy region = {};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {(uint32_t)source->width, (uint32_t)source->height, 1};
  vkCmdCopyImageToBuffer(cmd, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         staging.buffer, 1, &region);
  vk_->HostReadBarrier(cmd);
  if (!vk_->Flush()) {
    vk_->DestroyBuffer(&staging);
    return false;
  }
  vk_->Invalidate(staging);

  out->resize(pitch / sizeof(uint16_t) * (size_t)rows);
  memcpy(out->data(), staging.mapped, out->size() * sizeof(uint16_t));
  *strideBytes = (int)pitch;
  vk_->DestroyBuffer(&staging);
  return true;
}

}  // namespace

std::unique_ptr<RenderPasses> CreateVulkanPasses() {
  return std::unique_ptr<RenderPasses>(new VulkanPasses());
}

}  // namespace cap
