#ifndef MEP_HTTP_CLIENT_H
#define MEP_HTTP_CLIENT_H

#include <map>
#include <string>

// The browser pane's blocking HTTP GET. `http://` goes over a plain POSIX
// socket (HTTP/1.1, `Connection: close`, Content-Length or chunked bodies,
// a handful of redirects) -- everything a page served from localhost
// needs, with no external tool in the loop. `https://` is handed to a
// `curl` subprocess (spawned directly, never through a shell), in line
// with WEBKIT_PARITY_PLAN.md's decision not to grow an in-house TLS stack.
//
// Blocking is deliberate: the pane already parses, styles and runs a
// page's scripts synchronously when it opens, and a page's subresources
// (stylesheets, scripts in document order) must be in hand before its
// scripts run. Localhost answers in well under a millisecond; a remote
// host is bounded by `timeout_ms`.
//
// Not supported on the wasm build (no sockets in a browser sandbox): every
// request fails with an error message there.
struct HttpResponse {
    bool ok = false;         // a response was received (any status); false = transport failure
    int status = 0;          // HTTP status code
    std::string status_text;
    std::string url;         // final URL after redirects
    std::map<std::string, std::string> headers;  // names lower-cased
    std::string body;
    std::string error;       // why `ok` is false

    /** @brief The response's media type without parameters ("text/html"), lower-cased; empty if absent. */
    std::string ContentType() const;
};

/**
 * @brief Performs a blocking GET.
 * @param url An absolute http:// or https:// URL.
 * @param timeout_ms Connect/read budget for the whole exchange.
 * @return The response; check `ok` first, then `status`.
 */
HttpResponse HttpGet(const std::string &url, int timeout_ms = 10000);

#endif  // MEP_HTTP_CLIENT_H
