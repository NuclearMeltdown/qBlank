#include "app_files.h"

#include "common.h"

namespace cap {
namespace {

std::string g_adopted_from;

}  // namespace

const std::string& AppNameUtf8() {
  static const std::string name = CAP_APP_NAME_UTF8;
  return name;
}

std::filesystem::path OwnFile(const char* extension) {
  return ExeFolder() / Utf8ToPath(AppNameUtf8() + "." + extension);
}

const std::string& AdoptedFrom() { return g_adopted_from; }

void SetAdoptedFrom(const std::string& name) { g_adopted_from = name; }

}  // namespace cap
