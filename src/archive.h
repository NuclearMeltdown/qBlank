#pragma once

// The two things that happen to a downloaded archive before anything inside it
// is used: it is held against the checksum that was published with it, and the
// files wanted are taken out of it. This program carries neither a hash nor an
// archive reader of its own -- both come from the system it runs on, which is
// why they are behind an interface rather than in the downloader.

#include <filesystem>
#include <string>
#include <vector>

namespace cap {

// SHA-256 of the file's contents as lowercase hex, empty when the file cannot be
// read. Lowercase because that is how the checksums published next to a download
// are written, and comparing two strings beats explaining which case they are in.
std::string Sha256HexOfFile(const std::filesystem::path& path);

// The system's reader takes 7z as well as zip. A 7z is the smaller download,
// where it can be read.
bool ArchiveReads7z();

// Takes `members` out of a zip or 7z archive, in one pass, and puts them in
// `intoFolder`, flat: the folders they sit in inside the archive are not
// recreated. A member is how the archive spells it and may use * for one whole
// path element; `foldersToDrop` says how many leading elements to throw away.
// False when that failed, with `error` saying what to tell the user. Whether
// every member was there is for the caller to look.
bool ExtractFromArchive(const std::filesystem::path& archive,
                        const std::vector<std::string>& members, int foldersToDrop,
                        const std::filesystem::path& intoFolder, std::string* error);

}  // namespace cap
