#include <windows.h>
// BCrypt. Needs windows.h first.
#include <bcrypt.h>

#include <cstdio>
#include <vector>

#include "archive.h"
#include "child_process.h"
#include "common.h"
#include "files.h"
#include "i18n.h"

// The Windows side of archive.h: SHA-256 from BCrypt, and unpacking from the
// tar.exe that ships with Windows.

namespace cap {

std::string Sha256HexOfFile(const std::filesystem::path& path) {
  if (path.empty()) return {};
  HANDLE f = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return {};

  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  std::string result;
  std::vector<uint8_t> hashObject;
  std::vector<uint8_t> digest;

  do {
    if (::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) break;
    DWORD objLen = 0, hashLen = 0, cb = 0;
    if (::BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objLen, sizeof(objLen), &cb, 0) != 0) break;
    if (::BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, (PUCHAR)&hashLen, sizeof(hashLen), &cb, 0) != 0) break;
    hashObject.resize(objLen);
    digest.resize(hashLen);
    if (::BCryptCreateHash(alg, &hash, hashObject.data(), objLen, nullptr, 0, 0) != 0) break;

    std::vector<uint8_t> buffer(1 << 20);
    DWORD read = 0;
    while (::ReadFile(f, buffer.data(), (DWORD)buffer.size(), &read, nullptr) && read > 0) {
      if (::BCryptHashData(hash, buffer.data(), read, 0) != 0) break;
    }
    if (::BCryptFinishHash(hash, digest.data(), hashLen, 0) != 0) break;

    char hex[3];
    for (uint8_t b : digest) {
      std::snprintf(hex, sizeof(hex), "%02x", b);
      result += hex;
    }
  } while (false);

  if (hash) ::BCryptDestroyHash(hash);
  if (alg) ::BCryptCloseAlgorithmProvider(alg, 0);
  ::CloseHandle(f);
  return result;
}

// Windows 10 1803 and later ship bsdtar as tar.exe, and it reads ZIP. That saves
// carrying a zip library for one button. Taken from the system folder by its full
// path rather than from the search path: a tar.exe that happens to be on the path
// is somebody else's.
bool ExtractFromZip(const std::filesystem::path& archive, const std::string& member,
                    int foldersToDrop, const std::filesystem::path& intoFolder,
                    std::string* error) {
  wchar_t system32[MAX_PATH] = {};
  if (::GetSystemDirectoryW(system32, MAX_PATH) == 0) return false;
  const std::filesystem::path tar = std::filesystem::path(system32) / L"tar.exe";
  if (!IsFile(tar)) {
    ReportError(error, CAP_SAID(T("tar.exe fehlt (Windows 10 1803 oder neuer nötig).",
                                  "tar.exe is missing (needs Windows 10 1803 or newer).")));
    return false;
  }

  // Pull out only the one member, dropping its folders. The pattern is quoted in
  // a shell to keep the shell from expanding it; as one argument among others it
  // reaches tar unchanged either way.
  ProcessSpec spec;
  spec.program = PathToUtf8(tar);
  spec.Add("-xf", PathToUtf8(archive));
  spec.Add("-C", PathToUtf8(intoFolder));
  spec.Add("--strip-components=" + std::to_string(foldersToDrop));
  spec.Add(member);

  int code = 1;
  if (!RunAndWait(spec, &code, 120000)) {
    ReportError(error, CAP_SAID(T("tar.exe ließ sich nicht starten.", "Could not start tar.exe.")));
    return false;
  }
  if (code != 0) {
    return ReportError(error, CAP_SAID(Format(T("Entpacken fehlgeschlagen (tar %lu).",
                                                "Extracting failed (tar %lu)."),
                                              (unsigned long)code)));
  }
  return true;
}

}  // namespace cap
