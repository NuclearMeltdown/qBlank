#include "http.h"

#include <windows.h>

#include <winhttp.h>

#include <vector>

namespace cap {
namespace {

// Kept local rather than taken from common.h: the migrator compiles this file
// too, and it has no business pulling in the rest of the program.
std::wstring Widen(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring out((size_t)n, L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
  return out;
}

std::string Narrow(const std::wstring& s) {
  if (s.empty()) return std::string();
  const int n =
      ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
  std::string out((size_t)n, '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n, nullptr, nullptr);
  return out;
}

struct Handles {
  HINTERNET session = nullptr;
  HINTERNET connect = nullptr;
  HINTERNET request = nullptr;
  ~Handles() {
    if (request) ::WinHttpCloseHandle(request);
    if (connect) ::WinHttpCloseHandle(connect);
    if (session) ::WinHttpCloseHandle(session);
  }
};

// Host, path and port out of the URL. The query string belongs to the path as
// far as WinHTTP is concerned.
struct Address {
  std::wstring host;
  std::wstring path;
  INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
  bool secure = true;
};

bool CrackUrl(const std::string& url, Address* out) {
  const std::wstring wide = Widen(url);
  URL_COMPONENTS parts = {};
  parts.dwStructSize = sizeof(parts);
  parts.dwHostNameLength = (DWORD)-1;
  parts.dwUrlPathLength = (DWORD)-1;
  parts.dwExtraInfoLength = (DWORD)-1;
  if (!::WinHttpCrackUrl(wide.c_str(), (DWORD)wide.size(), 0, &parts)) return false;
  if (parts.dwHostNameLength == 0) return false;

  out->host.assign(parts.lpszHostName, parts.dwHostNameLength);
  out->path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
  if (parts.dwExtraInfoLength > 0) out->path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
  out->port = parts.nPort;
  out->secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
  return true;
}

std::wstring ReadHeader(HINTERNET request, DWORD header) {
  DWORD size = 0;
  ::WinHttpQueryHeaders(request, header, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size,
                        WINHTTP_NO_HEADER_INDEX);
  if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) return {};
  std::wstring value(size / sizeof(wchar_t), L'\0');
  if (!::WinHttpQueryHeaders(request, header, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &size,
                             WINHTTP_NO_HEADER_INDEX)) {
    return {};
  }
  value.resize(wcslen(value.c_str()));
  return value;
}

bool Fail(HttpError* error, HttpError which) {
  if (error) *error = which;
  return false;
}

}  // namespace

bool HttpGet(const HttpRequest& request, HttpResponse* response, const HttpSink& sink,
             HttpError* error) {
  Address address;
  if (!CrackUrl(request.url, &address)) return Fail(error, HttpError::NoRequest);

  Handles h;
  const std::wstring agent = Widen(request.userAgent);
  h.session = ::WinHttpOpen(agent.empty() ? nullptr : agent.c_str(),
                            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
  if (!h.session) return Fail(error, HttpError::NoNetwork);

  const DWORD timeout = (DWORD)request.timeoutMs;
  ::WinHttpSetTimeouts(h.session, timeout, timeout, timeout, timeout);

  h.connect = ::WinHttpConnect(h.session, address.host.c_str(), address.port, 0);
  if (!h.connect) return Fail(error, HttpError::NoServer);

  h.request = ::WinHttpOpenRequest(h.connect, L"GET", address.path.c_str(), nullptr,
                                   WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   address.secure ? WINHTTP_FLAG_SECURE : 0);
  if (!h.request) return Fail(error, HttpError::NoRequest);

  if (!request.followRedirects) {
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    ::WinHttpSetOption(h.request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
  }

  std::wstring headers;
  if (!request.accept.empty()) headers = L"Accept: " + Widen(request.accept) + L"\r\n";
  if (!::WinHttpSendRequest(h.request,
                            headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            headers.empty() ? 0 : (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !::WinHttpReceiveResponse(h.request, nullptr)) {
    return Fail(error, HttpError::NoAnswer);
  }

  DWORD status = 0, size = sizeof(status);
  ::WinHttpQueryHeaders(h.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
  const bool redirect = status >= 300 && status < 400;
  if (response) {
    response->status = (int)status;
    if (redirect) response->location = Narrow(ReadHeader(h.request, WINHTTP_QUERY_LOCATION));
    const std::wstring length = ReadHeader(h.request, WINHTTP_QUERY_CONTENT_LENGTH);
    response->contentLength = length.empty() ? 0 : _wcstoui64(length.c_str(), nullptr, 10);
  }
  // A redirect that was not followed is what the caller asked for; anything
  // else that is not 200 is not an answer to the question.
  if (status != 200 && !(redirect && !request.followRedirects)) {
    return Fail(error, HttpError::Status);
  }

  if (!sink) return true;
  std::vector<char> buffer;
  for (;;) {
    DWORD available = 0;
    // A query that fails here means there is no more coming, not that what
    // arrived is bad. That is how both callers have always read it.
    if (!::WinHttpQueryDataAvailable(h.request, &available) || available == 0) break;
    buffer.resize(available);
    DWORD read = 0;
    if (!::WinHttpReadData(h.request, buffer.data(), available, &read)) {
      return Fail(error, HttpError::Transfer);
    }
    if (read == 0) break;
    if (!sink(buffer.data(), read)) return Fail(error, HttpError::Transfer);
  }
  return true;
}

bool HttpGetString(const HttpRequest& request, std::string* out, HttpResponse* response,
                   HttpError* error, size_t limit) {
  out->clear();
  bool enough = false;
  HttpError why = HttpError::None;
  const bool ok = HttpGet(
      request, response,
      [&](const void* data, size_t size) {
        out->append((const char*)data, size);
        if (limit != 0 && out->size() >= limit) {
          enough = true;
          return false;
        }
        return true;
      },
      &why);
  // Stopping at the limit is not a broken transfer: the rest was not wanted.
  if (ok || enough) return true;
  if (error) *error = why;
  return false;
}

}  // namespace cap
