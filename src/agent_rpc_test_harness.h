#ifndef MEP_AGENT_RPC_TEST_HARNESS_H
#define MEP_AGENT_RPC_TEST_HARNESS_H

// The bits every "spawn a real mep and drive its agent socket" test needs:
// connecting to the Unix socket, one Content-Length-framed JSON-RPC round
// trip, and a CHECK that survives a Release build.
//
// Extracted from agent_rpc_test.cpp when the sketcher (plans/
// CAD_FEM_PLAN.md Part D.5) needed the same harness. Kept as a header
// rather than copied, so that the NDEBUG trap documented below is fixed
// in one place if it is ever got wrong again.

#include "json.h"
#include "persist.h"
#include "rpc_framing.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {

// Always active regardless of NDEBUG -- see the file-level comment above
// for why plain assert() is the wrong tool here.
/**
 * @brief Prints a CHECK-failure message (with file/line and optional context) to stderr and aborts the process.
 * @param expr The source text of the failed condition.
 * @param file The source file the check ran in.
 * @param line The source line the check ran on.
 * @param context Extra diagnostic text to print, or "" to omit it.
 */
inline void CheckFailed(const char *expr, const char *file, int line, const std::string &context) {
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expr, file, line);
    if (!context.empty()) std::fprintf(stderr, "  context: %s\n", context.c_str());
    std::fflush(stderr);
    std::abort();
}
#define CHECK(cond) ((cond) ? (void)0 : CheckFailed(#cond, __FILE__, __LINE__, ""))
#define CHECK_CTX(cond, context) ((cond) ? (void)0 : CheckFailed(#cond, __FILE__, __LINE__, (context)))

/**
 * @brief Opens a single Unix-domain stream socket and connects it to the given path.
 * @param path The Unix socket path to connect to.
 * @return The connected socket's file descriptor, or -1 on failure (socket creation or connect).
 */
inline int ConnectOnce(const std::string &path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) return fd;
    close(fd);
    return -1;
}

// Issues one JSON-RPC request and blocks for its response, framing/
// parsing exactly as a real client (the future MCP server, `socat`, ...)
// would -- reusing the same rpc_framing.h the client side of that traffic
// is expected to share, same as agent_rpc.cpp's server side does.
//
// Since M3, the connection also carries unsolicited server-initiated
// event.* notifications (no "id" field) interleaved with request/response
// traffic -- Call() skips over those while waiting (appending each to
// `*events_out` if non-null, so a caller that cares which events arrived
// around a given request can inspect them) rather than assuming the very
// next complete message must be its response. Once a message *with* an
// "id" does arrive, it's required to match this call's id -- this is a
// strictly synchronous, one-outstanding-request-at-a-time protocol, so an
// id mismatch on an actual response means something is genuinely wrong
// with the connection state, not something to silently paper over.
/**
 * @brief Sends one framed JSON-RPC request over `fd` and blocks until its matching response arrives, skipping over any interleaved event.* notifications along the way.
 * @param fd The connected socket to send the request on and read the response from.
 * @param id The request id; the eventual response must echo this id.
 * @param method The JSON-RPC method name to call.
 * @param params The method's parameters.
 * @param read_buf This connection's accumulated-but-not-yet-framed read buffer, reused/extended across calls.
 * @param events_out If non-null, every event.* notification seen while waiting is appended here.
 * @return The parsed JSON-RPC response matching `id`.
 */
inline Json Call(int fd, int id, const std::string &method, const Json &params, std::string *read_buf, std::vector<Json> *events_out = nullptr) {
    Json req = Json::Object();
    req["jsonrpc"] = Json("2.0");
    req["id"] = id;
    req["method"] = method;
    req["params"] = params;
    const std::string framed = FrameRpcMessage(req.dump());
    size_t offset = 0;
    while (offset < framed.size()) {
        ssize_t n = send(fd, framed.data() + offset, framed.size() - offset, 0);
        CHECK_CTX(n > 0, "send() failed sending request id=" + std::to_string(id));
        offset += static_cast<size_t>(n);
    }
    Json response;
    bool got_one = false;
    while (!got_one) {
        char chunk[4096];
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        CHECK_CTX(n > 0, "recv() failed/EOF waiting on response to id=" + std::to_string(id));
        read_buf->append(chunk, static_cast<size_t>(n));
        bool ok = PumpRpcFrames(*read_buf, [&](const std::string &body) {
            Json msg;
            const bool parsed = Json::Parse(body, &msg);
            CHECK_CTX(parsed, "Json::Parse failed for a message while waiting on response to id=" + std::to_string(id) + ", raw body=[" + body + "]");
            if (!msg.contains("id")) {
                if (events_out) events_out->push_back(msg);  // a pushed event.* notification, not our response -- keep waiting
                return;
            }
            CHECK_CTX(msg.get("id").as_int(-1) == id,
                      "response id mismatch: expected " + std::to_string(id) + ", got body=[" + body + "]");
            response = msg;
            got_one = true;
        });
        CHECK_CTX(ok, "fatal framing violation waiting on response to id=" + std::to_string(id));
    }
    return response;
}

// Server-pushed event.* notifications resulting from a request aren't
// guaranteed to arrive bundled in the same read as that request's
// response -- PollOnce sends the response and computes/sends events in
// the same server-side call, but those are separate socket writes, and
// nothing guarantees the client's kernel receive buffer coalesces them
// into one readable chunk. Call()'s own event capture only catches
// events that happen to already be sitting in `*read_buf` (or arrive in
// the same recv() as the response); this fills the real gap -- a bounded
// wait (poll() with a timeout, not a blocking recv()) for whatever
// shows up shortly after, appending every notification found to
// `*events_out`. Confirmed this gap is real, not hypothetical: an
// earlier version of the M3 test section below relied solely on Call()'s
// bundled capture and failed close to 100% of the time waiting on
// event.paneFocusChanged from `:terminal`, with a clear CHECK failure
// (not a crash -- exactly what CHECK/CHECK_CTX are for) rather than a
// silent pass.
/**
 * @brief Polls `fd` with a timeout, collecting every server-pushed event.* notification that arrives shortly after, until nothing more shows up within `timeout_ms`.
 * @param fd The connected socket to read from.
 * @param read_buf This connection's accumulated-but-not-yet-framed read buffer, reused/extended across calls.
 * @param events_out Every notification found is appended here.
 * @param timeout_ms How long to wait (in milliseconds) for each next message before giving up.
 */
// `inline` rather than static: this is a header now, and a test that
// only needs Call() should not have to pay for an unused-function error.
inline void DrainEvents(int fd, std::string *read_buf, std::vector<Json> *events_out, int timeout_ms = 500) {
    pollfd pfd{fd, POLLIN, 0};
    for (;;) {
        pfd.revents = 0;
        const int ready = poll(&pfd, 1, timeout_ms);
        if (ready <= 0) return;  // timed out (or a poll() error): nothing more arriving
        char chunk[4096];
        const ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        CHECK(n > 0);
        read_buf->append(chunk, static_cast<size_t>(n));
        const bool ok = PumpRpcFrames(*read_buf, [&](const std::string &body) {
            Json msg;
            CHECK_CTX(Json::Parse(body, &msg), "Json::Parse failed while draining events, raw body=[" + body + "]");
            CHECK_CTX(!msg.contains("id"), "DrainEvents saw a response-shaped message (has \"id\"), not a notification: [" + body + "]");
            events_out->push_back(msg);
        });
        CHECK(ok);
    }
}

}  // namespace

/**
 * @brief End-to-end test entry point: spawns a real mep instance, drives its agent-control socket through cursor/buffer/pane/state/participant-cursor/status RPCs plus the M3 event stream, then asks it to quit and verifies clean exit and socket cleanup.
 * @param argc Argument count; must be 2.
 * @param argv Argument vector; argv[1] is the path to the mep binary to spawn.
 * @return 2 on bad usage; otherwise the process aborts via CHECK on any failure, or prints a success message and returns 0 (falls off the end of main).
 */

#endif
