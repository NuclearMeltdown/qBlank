#include "update/release_source.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

#include "http.h"
#include "json.h"

namespace cap {
namespace {

// 1340564357 is this repository's number. It was CapView when the number was
// issued and it will keep the number whatever the repository is called next,
// including after being handed to a different account. Asking GitHub for a
// repository that has moved answers with a redirect to exactly this form.
const ReleaseSource kSource = {
    "api.github.com",
    "/repositories/1340564357/releases/latest",
    "/repos/NuclearMeltdown/qBlank/releases/latest",
    "https://github.com/NuclearMeltdown/qBlank/releases",
    "https://nuclearmeltdown.github.io/qBlank/",
};

// Named after the account rather than the program, because the account is the
// part of this that has never changed. GitHub only insists that there is one.
const char kAgent[] = "NuclearMeltdown-Updater";

#if defined(_M_ARM64)
const char kArchLabel[] = "app-arm64";
#elif defined(_M_X64)
const char kArchLabel[] = "app-x64";
#else
const char kArchLabel[] = "app-x86";
#endif

bool EndsWith(const std::string& text, const char* tail) {
  const size_t n = std::char_traits<char>::length(tail);
  if (text.size() < n) return false;
  for (size_t i = 0; i < n; ++i) {
    if (std::tolower((unsigned char)text[text.size() - n + i]) !=
        std::tolower((unsigned char)tail[i])) {
      return false;
    }
  }
  return true;
}

// What counts as part of a word inside a label. The hyphen belongs to the word
// so that "app" does not match halfway into "app-x64"; the dot does not, so a
// file name in front of the word does not glue itself to it.
bool IsLabelChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
         c == '_';
}

// A label carries a word for this program and a name for the person reading the
// release page, because GitHub shows the label *instead of* the file name. A
// label of "app-x64" leaves a download list reading "app-x64" and "migrator",
// which tells a visitor nothing about what to click. So the label is written
// "qBlank.exe (app-x64)" and the word is looked for inside it.
//
// Word, not substring: "app" must not match "app-x64", or the generic fallback
// in PickProgram would take an ARM build on an x64 machine.
bool HasLabel(const std::string& text, const char* word) {
  const size_t length = std::strlen(word);
  if (length == 0) return false;
  for (size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1)) {
    const bool startsClean = at == 0 || !IsLabelChar(text[at - 1]);
    const bool endsClean = at + length >= text.size() || !IsLabelChar(text[at + length]);
    if (startsClean && endsClean) return true;
  }
  return false;
}

// "v1.2.3" against "1.2" and so on. Missing parts count as zero, so v1.1 is
// newer than v1 and the same as v1.1.0.
std::vector<int> Parts(const std::string& text) {
  std::vector<int> parts;
  size_t i = 0;
  while (i < text.size() && !isdigit((unsigned char)text[i])) ++i;
  int value = 0;
  bool any = false;
  for (; i < text.size(); ++i) {
    if (isdigit((unsigned char)text[i])) {
      value = value * 10 + (text[i] - '0');
      any = true;
    } else if (text[i] == '.') {
      parts.push_back(value);
      value = 0;
      any = false;
    } else {
      break;
    }
  }
  if (any) parts.push_back(value);
  return parts;
}

// "2026-09-08T12:34:56Z" -> 20260908. Zero when it does not look like a date.
long DayFromIso(const std::string& text) {
  if (text.size() < 10 || text[4] != '-' || text[7] != '-') return 0;
  for (size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u}) {
    if (!isdigit((unsigned char)text[i])) return 0;
  }
  return std::atol(text.substr(0, 4).c_str()) * 10000 +
         std::atol(text.substr(5, 2).c_str()) * 100 + std::atol(text.substr(8, 2).c_str());
}

// The day this file was compiled, in the same shape. __DATE__ is "Sep  8 2026".
long BuildDay() {
  const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char* date = __DATE__;
  const char* found = strstr(months, std::string(date, 3).c_str());
  if (!found) return 0;
  const long month = (long)((found - months) / 3) + 1;
  return std::atol(date + 7) * 10000 + month * 100 + std::atol(date + 4);
}

void ReadAssets(const json::Value& root, Release* out) {
  const json::Value& assets = root["assets"];
  for (size_t i = 0; i < assets.Size(); ++i) {
    ReleaseAsset asset;
    asset.name = assets.At(i)["name"].AsString();
    asset.label = assets.At(i)["label"].AsString();
    asset.url = assets.At(i)["browser_download_url"].AsString();
    asset.size = (long long)assets.At(i)["size"].AsNumber();
    if (!asset.url.empty()) out->assets.push_back(asset);
  }
}

FetchError Reason(HttpError error) {
  switch (error) {
    case HttpError::NoNetwork: return FetchError::NoNetwork;
    case HttpError::NoServer: return FetchError::NoServer;
    case HttpError::NoRequest: return FetchError::NoRequest;
    case HttpError::NoAnswer: return FetchError::NoAnswer;
    case HttpError::Status: return FetchError::HttpStatus;
    case HttpError::Transfer: return FetchError::Transfer;
    case HttpError::None: break;
  }
  return FetchError::None;
}

bool FetchFrom(const char* path, Release* out, FetchError* error, int* httpStatus) {
  std::string body;
  if (!FetchUrl(std::string("https://") + kSource.host + path, true, &body, error, httpStatus)) {
    return false;
  }

  std::string parseError;
  const json::Value root = json::Parse(body, &parseError);
  if (!root.IsObject()) {
    if (error) *error = FetchError::Unreadable;
    return false;
  }
  out->tag = root["tag_name"].AsString();
  out->notes = root["body"].AsString();
  out->pageUrl = root["html_url"].AsString();
  out->publishedAt = root["published_at"].AsString();
  out->assets.clear();
  ReadAssets(root, out);
  return true;
}

}  // namespace

const ReleaseSource& Releases() { return kSource; }

// GitHub refuses requests without a user agent, and the API wants to be told
// which version of itself to speak.
bool FetchUrl(const std::string& url, bool api, std::string* out, FetchError* error,
              int* httpStatus) {
  HttpRequest request;
  request.url = url;
  request.userAgent = kAgent;
  request.accept = api ? "application/vnd.github+json" : "application/octet-stream";

  HttpResponse response;
  HttpError why = HttpError::None;
  if (HttpGetString(request, out, &response, &why)) return true;
  if (error) *error = Reason(why);
  if (httpStatus) *httpStatus = response.status;
  return false;
}

bool FetchLatestRelease(Release* out, FetchError* error, int* httpStatus) {
  if (FetchFrom(kSource.byNumber, out, error, httpStatus)) return true;
  // The number is the route that survives renaming, so it is tried first. The
  // name is here for the day GitHub stops answering to numbers.
  return FetchFrom(kSource.byName, out, error, httpStatus);
}

const ReleaseAsset* PickLabelled(const Release& release, const char* label) {
  for (const ReleaseAsset& asset : release.assets) {
    if (HasLabel(asset.label, label)) return &asset;
  }
  return nullptr;
}

const ReleaseAsset* PickProgram(const Release& release) {
  if (const ReleaseAsset* exact = PickLabelled(release, kArchLabel)) return exact;
  if (const ReleaseAsset* plain = PickLabelled(release, "app")) return plain;

  // No labels: take the largest executable that is not the migrator. The
  // migrator is padded to a few hundred kilobytes and the program is megabytes,
  // so this lands on the right one even if the labels were forgotten entirely.
  const ReleaseAsset* best = nullptr;
  for (const ReleaseAsset& asset : release.assets) {
    if (HasLabel(asset.label, "migrator")) continue;
    if (!EndsWith(asset.name, ".exe")) continue;
    if (!best || asset.size > best->size) best = &asset;
  }
  return best;
}

bool IsNewerRelease(const Release& release, const std::string& current) {
  const std::vector<int> a = Parts(release.tag);
  const std::vector<int> b = Parts(current);
  if (!a.empty()) {
    for (size_t i = 0; i < a.size() || i < b.size(); ++i) {
      const int x = i < a.size() ? a[i] : 0;
      const int y = i < b.size() ? b[i] : 0;
      if (x != y) return x > y;
    }
    return false;
  }

  // A tag with no numbers in it at all -- a naming scheme this build has never
  // seen. Nothing sensible can be compared, so fall back to the calendar: a
  // release published after this was built is newer than this.
  const long published = DayFromIso(release.publishedAt);
  const long built = BuildDay();
  return published != 0 && built != 0 && published > built;
}

}  // namespace cap
