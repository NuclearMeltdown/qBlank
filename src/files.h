#pragma once

// What this program needs from a file system, in the terms it thinks in: whole
// files, a place to put something that gets thrown away again, and the folders
// the user keeps recordings and pictures in.
//
// Paths are std::filesystem::path throughout. The standard library carries the
// platform's own spelling of a path inside it, which is why nothing here says
// wchar_t and nothing here takes a string of bytes and hopes.
//
// files.cpp holds what the standard library already does the same way
// everywhere, and needs no second version. files_win32.cpp holds the rest: the
// places where the promise is the point -- a settings file is either the old one
// or the new one and never half of either -- and the questions the standard has
// no answer for: where the user's Videos folder is, and what is on the search
// path.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace cap {

// ------------------------------------------------------------------- questions

// True when there is anything at all at this path, file or folder.
bool PathExists(const std::filesystem::path& path);

// True when there is a file there. A folder answers false.
bool IsFile(const std::filesystem::path& path);

// When the file was last written, in seconds since 1970-01-01 UTC, 0 when that
// cannot be told. For comparing two files and for showing a date -- no promise
// beyond whole seconds.
int64_t FileWriteTime(const std::filesystem::path& path);

// The files directly in `folder` whose extension is `extension` (".json", with
// the dot, compared without regard to case). Folders are left out, nothing is
// followed into subfolders, and the order is whatever the file system hands
// over -- whoever cares about the order sorts.
std::vector<std::filesystem::path> FilesWithExtension(const std::filesystem::path& folder,
                                                      const std::string& extension);

// ----------------------------------------------------------------- whole files

// Reads the whole file into `out`. `limit` refuses anything bigger, in bytes;
// 0 means no limit. False when the file cannot be read, or is too big.
bool ReadWholeFile(const std::filesystem::path& path, std::string* out, uint64_t limit = 0);

// Writes the file, replacing whatever was there. Nothing more is promised: a
// crash halfway leaves a short file behind, which is fine for something that is
// thrown away afterwards anyway.
bool WriteWholeFile(const std::filesystem::path& path, const void* data, size_t size);

// The same, with the two promises the settings depend on: afterwards the file is
// either entirely the old one or entirely the new one, and it has reached the
// disk rather than a cache that a power cut takes with it. A backend that cannot
// keep both of those should fail rather than write the file the ordinary way.
bool ReplaceWholeFile(const std::filesystem::path& path, const void* data, size_t size);

// Writes a file in pieces, for the ones too big to hold in memory first. Open,
// Write as often as needed, then Close -- and Close is what says whether all of
// it arrived. Destruction closes as well, so an abandoned writer cannot leave
// the file open behind it.
class FileWriter {
 public:
  FileWriter();
  ~FileWriter();
  FileWriter(const FileWriter&) = delete;
  FileWriter& operator=(const FileWriter&) = delete;

  // Creates the file, or truncates one that is already there.
  bool Open(const std::filesystem::path& path);
  bool Write(const void* data, size_t size);
  bool Close();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ------------------------------------------------------- moving and removing

// Removes the file. True when it is gone afterwards, which includes the case of
// a file that was not there to begin with.
bool RemoveFile(const std::filesystem::path& path);

// Renames, replacing a file already at `to`. Both paths are expected on the same
// volume, which is the case everywhere this is used.
bool RenameOver(const std::filesystem::path& from, const std::filesystem::path& to);

// ------------------------------------------------------------ where things go

// The folders the user keeps recordings and pictures in, as the desktop names
// them. Empty when the system does not name one -- the caller then falls back to
// its own folder, which is always there.
std::filesystem::path UserVideosFolder();
std::filesystem::path UserPicturesFolder();

// The folder for files that get thrown away again.
std::filesystem::path TempFolder();

// An empty file in the temp folder that nothing else is using, the first few
// characters of `prefix` in its name. Empty when none could be made. It exists
// when this returns, and whoever asked for it removes it again.
std::filesystem::path MakeTempFile(const std::string& prefix);

// The full path of a program on the system's search path -- "ffmpeg" gives
// "C:\tools\ffmpeg.exe". Empty when it is not on the path. The name comes
// without the platform's extension for a program; that is the backend's to add.
std::filesystem::path ProgramOnPath(const std::string& name);

}  // namespace cap
