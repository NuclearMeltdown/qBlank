// Writes the HLSL from src/render/shaders.h out as files, one per shader.
//
// A build step, not a tool anyone runs: the Vulkan renderer needs the shaders
// as SPIR-V, compiled ahead of time by dxc, and dxc reads files. The source
// stays the one in shaders.h that Direct3D 11 compiles at startup, so the two
// renderers cannot drift apart -- see CMakeLists.txt.
//
//   qblank_hlsl_dump.exe <output directory>

#include <cstdio>
#include <string>

#include "render/shaders.h"

namespace {

bool WriteShader(const std::string& dir, const char* name, const char* text) {
  std::string path = dir + "/" + name + ".hlsl";
  FILE* file = std::fopen(path.c_str(), "wb");
  if (!file) {
    std::fprintf(stderr, "hlsl_dump: cannot write %s\n", path.c_str());
    return false;
  }
  bool ok = std::fputs(text, file) >= 0;
  ok = std::fclose(file) == 0 && ok;
  if (!ok) std::fprintf(stderr, "hlsl_dump: writing %s failed\n", path.c_str());
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: qblank_hlsl_dump <output directory>\n");
    return 2;
  }
  const std::string dir = argv[1];
  bool ok = true;
  ok = WriteShader(dir, "fullscreen_vs", cap::kFullscreenVS) && ok;
  ok = WriteShader(dir, "clean_ps", cap::kCleanPS) && ok;
  ok = WriteShader(dir, "convert_ps", cap::kConvertPS) && ok;
  ok = WriteShader(dir, "scale_ps", cap::kScalePS) && ok;
  ok = WriteShader(dir, "hdr_record_ps", cap::kHdrRecordPS) && ok;
  ok = WriteShader(dir, "ui_composite_ps", cap::kUiCompositePS) && ok;
  return ok ? 0 : 1;
}
