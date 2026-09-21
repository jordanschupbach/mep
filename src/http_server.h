#ifndef MEP_HTTP_SERVER_H
#define MEP_HTTP_SERVER_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// A minimal static-file HTTP server the editor can host in-process, so a
// web project can be served to http://localhost and opened in the browser
// pane (or any other browser) without leaving mep: `:Serve [dir] [port]`.
//
// Scope, deliberately small: GET and HEAD only, HTTP/1.1 with
// `Connection: close`, one request per connection. A request path is
// percent-decoded, resolved under the root and refused (403) if it would
// escape it; a directory serves its index.html, or a plain listing when it
// has none. Content types come from the file suffix. Responses are
// `Cache-Control: no-store`, since the point is to look at files that are
// being edited.
//
// It binds 127.0.0.1 only -- this is a development convenience, not a
// network service -- and runs its accept loop on a background thread,
// handling each connection on a short-lived detached worker; nothing here
// touches editor or Lua state, so no synchronisation with the UI thread is
// needed beyond Stop().
//
// Not available on the wasm/emscripten build or Windows (no POSIX
// sockets): Start() returns false with an error there.
class HttpStaticServer {
public:
    HttpStaticServer() = default;
    ~HttpStaticServer();
    HttpStaticServer(const HttpStaticServer &) = delete;
    HttpStaticServer &operator=(const HttpStaticServer &) = delete;

    /**
     * @brief Starts serving `root` on 127.0.0.1.
     * @param root Directory to serve (made absolute/canonical).
     * @param port TCP port, or 0 to let the OS pick a free one.
     * @param error Filled with the reason on failure.
     * @return True once the socket is listening; Port() is then valid.
     */
    bool Start(const std::string &root, int port, std::string *error);

    /** @brief Stops accepting, closes the listener and joins the accept thread. Idempotent. */
    void Stop();

    /** @brief The bound port (the OS-assigned one when Start was given 0); 0 when not running. */
    int Port() const { return port_; }
    /** @brief The canonical directory being served. */
    const std::string &Root() const { return root_; }
    /** @brief True between a successful Start() and Stop(). */
    bool Running() const { return running_.load(); }
    /** @brief Requests answered so far (any status). */
    long RequestCount() const { return requests_.load(); }

private:
    void AcceptLoop();

    std::string root_;
    int port_ = 0;
    int listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::atomic<long> requests_{0};
    std::thread thread_;
};

/** @brief The Content-Type for a path's suffix ("text/html; charset=utf-8" for .html, ...); application/octet-stream when unknown. */
std::string HttpContentTypeForPath(const std::string &path);

#endif  // MEP_HTTP_SERVER_H
