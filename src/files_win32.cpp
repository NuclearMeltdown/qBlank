#include <windows.h>
// SHGetKnownFolderPath and the FOLDERID constants. Needs windows.h first.
#include <shlobj.h>

#include "common.h"
#include "files.h"

// The Windows side of files.h: the promises the standard library does not make,
// and the questions it does not answer.

namespace cap {
namespace {

// A file handle that closes itself, so the early returns below do not each have
// to remember to.
class Handle {
 public:
  explicit Handle(HANDLE h) : h_(h) {}
  ~Handle() { Close(); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;

  bool ok() const { return h_ != INVALID_HANDLE_VALUE; }
  HANDLE get() const { return h_; }
  void Close() {
    if (h_ != INVALID_HANDLE_VALUE) ::CloseHandle(h_);
    h_ = INVALID_HANDLE_VALUE;
  }

 private:
  HANDLE h_ = INVALID_HANDLE_VALUE;
};

// Nothing is shared while writing: whoever else has this file open at that
// moment is looking at something that is about to be wrong anyway.
HANDLE CreateForWriting(const std::filesystem::path& path) {
  return ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

bool WriteAll(HANDLE file, const void* data, size_t size) {
  const uint8_t* at = (const uint8_t*)data;
  size_t left = size;
  while (left > 0) {
    const DWORD chunk = (DWORD)(left > 0x10000000 ? 0x10000000 : left);
    DWORD written = 0;
    if (!::WriteFile(file, at, chunk, &written, nullptr) || written == 0) return false;
    at += written;
    left -= written;
  }
  return true;
}

std::filesystem::path KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR wide = nullptr;
  std::filesystem::path folder;
  if (SUCCEEDED(::SHGetKnownFolderPath(id, 0, nullptr, &wide)) && wide) folder = wide;
  if (wide) ::CoTaskMemFree(wide);
  return folder;
}

}  // namespace

int64_t FileWriteTime(const std::filesystem::path& path) {
  if (path.empty()) return 0;
  WIN32_FILE_ATTRIBUTE_DATA info = {};
  if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return 0;
  ULARGE_INTEGER ticks = {};
  ticks.LowPart = info.ftLastWriteTime.dwLowDateTime;
  ticks.HighPart = info.ftLastWriteTime.dwHighDateTime;
  if (ticks.QuadPart == 0) return 0;
  // Windows counts hundreds of nanoseconds since 1601; the interface says
  // seconds since 1970.
  return (int64_t)(ticks.QuadPart / 10000000ULL) - 11644473600LL;
}

bool ReadWholeFile(const std::filesystem::path& path, std::string* out, uint64_t limit) {
  if (out) out->clear();
  if (!out || path.empty()) return false;

  Handle file(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!file.ok()) return false;

  LARGE_INTEGER size = {};
  if (!::GetFileSizeEx(file.get(), &size) || size.QuadPart < 0) return false;
  if (limit != 0 && (uint64_t)size.QuadPart > limit) return false;
  if ((uint64_t)size.QuadPart > (uint64_t)(size_t)-1) return false;

  out->resize((size_t)size.QuadPart);
  if (out->empty()) return true;

  size_t at = 0;
  while (at < out->size()) {
    const DWORD chunk = (DWORD)((out->size() - at) > 0x10000000 ? 0x10000000 : (out->size() - at));
    DWORD read = 0;
    if (!::ReadFile(file.get(), out->data() + at, chunk, &read, nullptr) || read == 0) {
      out->clear();
      return false;
    }
    at += read;
  }
  return true;
}

bool WriteWholeFile(const std::filesystem::path& path, const void* data, size_t size) {
  if (path.empty() || (size > 0 && !data)) return false;
  Handle file(CreateForWriting(path));
  if (!file.ok()) return false;
  if (!WriteAll(file.get(), data, size)) return false;
  return true;
}

bool ReplaceWholeFile(const std::filesystem::path& path, const void* data, size_t size) {
  if (path.empty() || (size > 0 && !data)) return false;

  // Through a sibling temp file, so an interrupted write cannot leave a
  // truncated file behind where the old one was.
  std::filesystem::path temp = path;
  temp += L".tmp";

  bool ok = false;
  {
    Handle file(CreateForWriting(temp));
    if (!file.ok()) return false;
    ok = WriteAll(file.get(), data, size);
    // The flush belongs before the close: MOVEFILE_WRITE_THROUGH below promises
    // the rename has reached the disk, not the bytes being renamed.
    ok = (::FlushFileBuffers(file.get()) != 0) && ok;
  }
  if (ok) {
    ok = ::MoveFileExW(temp.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
  }
  if (!ok) ::DeleteFileW(temp.c_str());
  return ok;
}

struct FileWriter::Impl {
  HANDLE file = INVALID_HANDLE_VALUE;
  bool broken = false;  // a write that did not go through, remembered until Close
};

FileWriter::FileWriter() : impl_(std::make_unique<Impl>()) {}

FileWriter::~FileWriter() {
  if (impl_->file != INVALID_HANDLE_VALUE) ::CloseHandle(impl_->file);
}

bool FileWriter::Open(const std::filesystem::path& path) {
  Close();
  if (path.empty()) return false;
  impl_->file = CreateForWriting(path);
  impl_->broken = false;
  return impl_->file != INVALID_HANDLE_VALUE;
}

bool FileWriter::Write(const void* data, size_t size) {
  if (impl_->file == INVALID_HANDLE_VALUE) return false;
  if (size == 0) return true;
  if (!data || !WriteAll(impl_->file, data, size)) {
    impl_->broken = true;
    return false;
  }
  return true;
}

bool FileWriter::Close() {
  if (impl_->file == INVALID_HANDLE_VALUE) return false;
  ::CloseHandle(impl_->file);
  impl_->file = INVALID_HANDLE_VALUE;
  return !impl_->broken;
}

std::filesystem::path UserVideosFolder() { return KnownFolder(FOLDERID_Videos); }

std::filesystem::path UserPicturesFolder() { return KnownFolder(FOLDERID_Pictures); }

std::filesystem::path MakeTempFile(const std::string& prefix) {
  const std::filesystem::path folder = TempFolder();
  if (folder.empty()) return {};

  wchar_t name[MAX_PATH] = {};
  // Takes the first three characters of the prefix and makes the rest unique;
  // the file exists when this returns, which is what keeps two of these apart.
  if (!::GetTempFileNameW(folder.c_str(), Utf8ToPath(prefix).c_str(), 0, name)) return {};

  // Says out loud what the file is for: the cache may then spare itself the
  // trouble of ever writing it to the disk.
  ::SetFileAttributesW(name, FILE_ATTRIBUTE_TEMPORARY);
  return std::filesystem::path(name);
}

std::filesystem::path ProgramOnPath(const std::string& name) {
  if (name.empty()) return {};
  const std::wstring wanted = Utf8ToPath(name).native();
  const DWORD kRoom = MAX_PATH * 4;
  wchar_t found[kRoom] = {};
  wchar_t* filePart = nullptr;
  const DWORD n = ::SearchPathW(nullptr, wanted.c_str(), L".exe", kRoom, found, &filePart);
  if (n == 0 || n >= kRoom) return {};
  return std::filesystem::path(found);
}

}  // namespace cap
