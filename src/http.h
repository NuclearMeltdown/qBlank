#pragma once

// One HTTP GET. That is the whole of this program's networking: ask GitHub what
// the newest release is, fetch a checksum, read a version out of a redirect,
// download a file. Nothing is ever posted, no session is kept, no cookie is
// stored.
//
// A request is named by its URL, because that is the one address form that does
// not belong to any particular HTTP library. Splitting it into host and path is
// the implementation's job.
//
// The Windows half is http_win32.cpp, and it deliberately depends on nothing
// else in the program: the migrator compiles it too.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace cap {

enum class HttpError {
  None,
  NoNetwork,  // no client could be made at all -- no network stack to speak to
  NoServer,   // the host could not be reached
  NoRequest,  // the request could not be put together, usually a bad URL
  NoAnswer,   // it went out and nothing came back
  Status,     // answered, but not with what was asked for -- see status
  Transfer,   // the body broke off part way through
};

struct HttpRequest {
  std::string url;  // absolute, http or https

  // Sent as the Accept header; empty sends none.
  std::string accept;

  // Servers that hand out anonymous downloads tend to refuse a request without
  // one, and GitHub's API insists on it.
  std::string userAgent;

  // False means the redirect itself is the answer: nothing is followed, and a
  // 3xx counts as success so the caller can read `location`. This is how the
  // ffmpeg version is found -- the stable URL redirects to a versioned one.
  bool followRedirects = true;

  // Applies to each stage separately, the way the system's own timeouts do:
  // resolving, connecting, sending, receiving.
  uint32_t timeoutMs = 20000;
};

struct HttpResponse {
  int status = 0;

  // Where the answer points instead. Only filled for a redirect that was not
  // followed.
  std::string location;

  // What the answer said its body would be, or 0 if it did not say. Available
  // before the first piece of the body arrives, which is what a progress bar
  // needs.
  uint64_t contentLength = 0;
};

// Handed each piece of the body as it arrives, in order. Returning false stops
// the transfer, and the call then fails with `Transfer` -- which is also how a
// caller cancels.
using HttpSink = std::function<bool(const void* data, size_t size)>;

// Asks, and hands the body to `sink`. An empty sink asks for the answer without
// its body, which is all a redirect is good for. `response` and `error` may be
// null.
bool HttpGet(const HttpRequest& request, HttpResponse* response, const HttpSink& sink,
             HttpError* error);

// The same, with the body collected in memory. `limit` stops the transfer once
// that many bytes have arrived; 0 means no limit, which is only safe for
// something known to be small.
bool HttpGetString(const HttpRequest& request, std::string* out, HttpResponse* response,
                   HttpError* error, size_t limit = 0);

}  // namespace cap
