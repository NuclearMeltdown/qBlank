#include "render/render_passes.h"

#include "render/display.h"

#include <cstring>

namespace cap {

std::unique_ptr<RenderPasses> CreateRenderPasses(GraphicsApi api) {
  switch (api) {
    case GraphicsApi::D3D11: return CreateD3D11Passes();
    case GraphicsApi::Vulkan:
#ifdef QBLANK_VULKAN
      return CreateVulkanPasses();
#else
      return nullptr;
#endif
  }
  return nullptr;
}

bool RenderPasses::CleanStale(const ConvertParams& params) {
  ConvertParams key = params;
  key.fieldIndex = 0;  // only pass two picks a field
  key.remNew = 0;      // a new frame has said so through InvalidateClean already
  if (cleanValid_ && memcmp(&key, &cleanParams_, sizeof(key)) == 0) return false;
  cleanParams_ = key;
  cleanValid_ = true;
  return true;
}

}  // namespace cap
