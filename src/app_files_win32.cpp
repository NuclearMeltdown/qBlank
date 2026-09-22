#include "app_files.h"

#include "app_identity.h"

namespace cap {

std::filesystem::path ExeFolder() { return std::filesystem::path(ExeDirectory()); }

std::filesystem::path OwnProgramFile() { return std::filesystem::path(ExePath()); }

bool SetFileAside(const std::filesystem::path& path) { return SetAside(path.native()); }

}  // namespace cap
