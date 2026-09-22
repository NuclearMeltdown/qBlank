#pragma once

// The two things that happen to a downloaded archive before anything inside it
// is used: it is held against the checksum that was published with it, and one
// file is taken out of it. This program carries neither a hash nor a zip reader
// of its own -- both come from the system it runs on, which is why they are
// behind an interface rather than in the downloader.

#include <filesystem>
#include <string>

namespace cap {

// SHA-256 of the file's contents as lowercase hex, empty when the file cannot be
// read. Lowercase because that is how the checksums published next to a download
// are written, and comparing two strings beats explaining which case they are in.
std::string Sha256HexOfFile(const std::filesystem::path& path);

// Takes one member out of a zip archive and puts it in `intoFolder`, flat: the
// folders it sits in inside the archive are not recreated. `member` is how the
// archive spells it and may use * for one whole path element; `foldersToDrop`
// says how many leading elements of that name to throw away. False when nothing
// came out, with `error` saying what to tell the user.
bool ExtractFromZip(const std::filesystem::path& archive, const std::string& member,
                    int foldersToDrop, const std::filesystem::path& intoFolder,
                    std::string* error);

}  // namespace cap
