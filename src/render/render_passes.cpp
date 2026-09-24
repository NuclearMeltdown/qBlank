#include "render/render_passes.h"

#include "render/display.h"

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

}  // namespace cap
