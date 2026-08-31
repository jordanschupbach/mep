#ifndef MEP_RPC_FRAMING_H
#define MEP_RPC_FRAMING_H

#include <cstdlib>
#include <functional>
#include <string>

// Consumes as many complete Content-Length-framed messages
// ("Content-Length: N\r\n\r\n" followed by N raw bytes -- JSON-RPC 2.0's
// own wire framing, byte-count- rather than line-delimited so a JSON body
// with no trailing newline can't get concatenated with the next message)
// as `buffer` currently holds, invoking `on_message` with each message
// body in arrival order and erasing consumed bytes from `buffer` as it
// goes. Any trailing partial message is left in `buffer` for a later call
// (after more bytes have been appended) to complete -- callers don't need
// to track an `expected_len` of their own across calls, since an
// incomplete message's header bytes are left untouched and simply
// re-scanned (cheap: a `Content-Length:` line is a couple dozen bytes)
// the next time this runs.
//
// Returns false on an unrecoverable framing violation (no Content-Length
// header where a blank-line-terminated header block was expected, or a
// header/body big enough to smell like a stuck/hostile peer rather than a
// real message -- see the size caps below): once a byte-count-framed
// stream loses sync with the sender there is no general way to
// resynchronize with the *next* message's boundary (unlike a line-
// oriented protocol, there's no delimiter to resync on), so the right
// move is for the caller to drop the connection, not keep parsing
// garbage. Returns true otherwise, including "no complete message yet,
// still waiting for more bytes" -- that's normal, not an error.
//
// New consumers of a Content-Length-framed JSON-RPC stream (src/
// agent_rpc.cpp's agent-control socket, which -- unlike the LSP client
// below -- talks to an arbitrary external process rather than one of a
// handful of vetted language-server binaries, so it can't assume a
// well-behaved peer) should use this rather than hand-rolling the header-
// parsing loop a second time. lua_env.cpp's own LSP-client copy
// (PumpLspBuffer/LspClientState) predates this extraction and is left
// as-is rather than migrated, to avoid touching a working, recently-
// stabilized code path without its own test coverage.
/** @brief Consumes as many complete Content-Length-framed messages as `buffer` currently holds, invoking `on_message` with each message body in arrival order and erasing consumed bytes from `buffer`.
 *  @param buffer the accumulated input stream; consumed bytes are erased in place, leaving any trailing partial message for a later call.
 *  @param on_message callback invoked with each complete message body, in arrival order.
 *  @return false on an unrecoverable framing violation (caller should drop the connection); true otherwise, including "no complete message yet". */
inline bool PumpRpcFrames(std::string &buffer, const std::function<void(const std::string &)> &on_message) {
    // A real Content-Length header is a couple dozen bytes; a real
    // message body is a small JSON-RPC call, not a bulk payload. Both
    // caps are generous headroom over any legitimate use of this wire
    // format, and exist only to bound how much unparseable/malicious
    // input this function will buffer before giving up rather than
    // growing `buffer` without limit.
    constexpr size_t kMaxHeaderBytes = 8 * 1024;
    constexpr size_t kMaxBodyBytes = 64 * 1024 * 1024;

    for (;;) {
        size_t header_end = buffer.find("\r\n\r\n");
        if (header_end == std::string::npos) return buffer.size() <= kMaxHeaderBytes;  // waiting for more, unless it's gone on far too long
        int expected_len = -1;
        size_t pos = 0;
        while (pos < header_end) {
            size_t eol = buffer.find("\r\n", pos);
            if (eol == std::string::npos || eol > header_end) eol = header_end;
            if (buffer.compare(pos, 15, "Content-Length:") == 0) {
                expected_len = std::atoi(buffer.c_str() + pos + 15);
            }
            pos = eol + 2;
        }
        if (expected_len < 0 || static_cast<size_t>(expected_len) > kMaxBodyBytes) return false;  // malformed or implausible
        size_t body_start = header_end + 4;
        if (buffer.size() < body_start + static_cast<size_t>(expected_len)) return true;  // body incomplete, wait for more
        std::string body = buffer.substr(body_start, static_cast<size_t>(expected_len));
        buffer.erase(0, body_start + static_cast<size_t>(expected_len));
        on_message(body);
    }
}

/** @brief Wraps `body` in a Content-Length-framed JSON-RPC message header.
 *  @param body the raw message body to frame.
 *  @return the "Content-Length: N\r\n\r\n" header followed by `body`. */
inline std::string FrameRpcMessage(const std::string &body) { return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body; }

#endif  // MEP_RPC_FRAMING_H
