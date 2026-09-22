#include "files.h"

#include <system_error>

#include "common.h"

// The part of files.h the standard library answers the same way on every
// platform. It compiles anywhere and has no second version; whatever a backend
// has to do differently lives in files_<platform>.cpp.
//
// Every call here takes the std::error_code form. The throwing overloads would
// turn a missing file -- the most ordinary thing that can happen to a program
// that looks next to its own executable -- into an exception, and nothing in
// this program catches one.

namespace cap {

bool PathExists(const std::filesystem::path& path) {
  if (path.empty()) return false;
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}

bool IsFile(const std::filesystem::path& path) {
  if (path.empty()) return false;
  std::error_code ec;
  return std::filesystem::is_regular_file(path, ec);
}

std::vector<std::filesystem::path> FilesWithExtension(const std::filesystem::path& folder,
                                                      const std::string& extension) {
  std::vector<std::filesystem::path> out;
  if (folder.empty()) return out;

  const std::string wanted = ToUpper(extension);
  std::error_code ec;
  std::filesystem::directory_iterator it(folder, ec);
  if (ec) return out;
  for (const std::filesystem::directory_entry& entry : it) {
    std::error_code entryError;
    if (!entry.is_regular_file(entryError) || entryError) continue;
    if (!wanted.empty() && ToUpper(PathToUtf8(entry.path().extension())) != wanted) continue;
    out.push_back(entry.path());
  }
  return out;
}

bool RemoveFile(const std::filesystem::path& path) {
  if (path.empty()) return false;
  std::error_code ec;
  std::filesystem::remove(path, ec);
  // remove() answers false for a file that was not there, and that counts as
  // gone: every caller here wants the file not to exist, not to have done it.
  return !ec;
}

bool RenameOver(const std::filesystem::path& from, const std::filesystem::path& to) {
  if (from.empty() || to.empty()) return false;
  std::error_code ec;
  std::filesystem::rename(from, to, ec);
  return !ec;
}

std::filesystem::path TempFolder() {
  std::error_code ec;
  const std::filesystem::path folder = std::filesystem::temp_directory_path(ec);
  return ec ? std::filesystem::path() : folder;
}

}  // namespace cap
