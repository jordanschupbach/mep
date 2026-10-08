// A no-network stand-in for collab_websocket.cpp, compiled in place of it
// when the build has no OpenSSL to link against.
//
// Why this exists rather than an #ifdef inside collab_websocket.cpp: the
// real file's dependency is unconditional -- OpenSSL supplies the SHA-1
// the RFC 6455 upgrade handshake is defined in terms of, and the TLS for
// wss:// on top of that -- so there is no subset of it that builds
// without the library. Swapping the whole translation unit keeps that
// file free of a second, untestable configuration.
//
// Why a stub rather than dropping collaboration from the link: editor.cpp
// constructs a CollabSession unconditionally (one call site, in the
// :collab command's handler), so CollabSession has to exist, and
// collab_session.cpp is written against this class. Every entry point
// here therefore fails the way a refused connection already does -- a
// path CollabSession handles, because an unreachable or wrong relay URL
// has always been able to produce it. The user-visible result of running
// :collab is the same error the command already shows when the relay is
// down.
//
// The one honest gap: the message reports the ordinary failure rather
// than "this build has no WebSocket support". Making it say so would mean
// adding a distinguishable error path to the collab UI, which is more
// surface than a configuration this temporary should claim. See the
// Windows section of README.org for what enabling the real thing takes
// (an OpenSSL install, nothing in this tree).

#include "collab_websocket.h"

#include <cctype>
#include <string>
#include <utility>

namespace mep::collab {

WebSocket::WebSocket(WebSocket &&other) noexcept
    : fd_(other.fd_),
      ssl_(other.ssl_),
      ssl_ctx_(other.ssl_ctx_),
      peer_frames_masked_(other.peer_frames_masked_),
      receive_buffer_(std::move(other.receive_buffer_)) {
    other.fd_ = -1;
    other.ssl_ = nullptr;
    other.ssl_ctx_ = nullptr;
}

WebSocket &WebSocket::operator=(WebSocket &&other) noexcept {
    if (this != &other) {
        Close();
        fd_ = other.fd_;
        ssl_ = other.ssl_;
        ssl_ctx_ = other.ssl_ctx_;
        peer_frames_masked_ = other.peer_frames_masked_;
        receive_buffer_ = std::move(other.receive_buffer_);
        other.fd_ = -1;
        other.ssl_ = nullptr;
        other.ssl_ctx_ = nullptr;
    }
    return *this;
}

WebSocket::~WebSocket() { Close(); }

// Nothing is ever opened, so there is never a descriptor or TLS object to
// release -- but the invariant `valid()` reports still has to hold.
void WebSocket::Close() {
    fd_ = -1;
    ssl_ = nullptr;
    ssl_ctx_ = nullptr;
    receive_buffer_.clear();
}

bool WebSocket::Accept(std::string *request_path, std::string *error) {
    (void)request_path;
    if (error) *error = "WebSocket support is not compiled into this build";
    return false;
}

WebSocket WebSocket::Connect(const std::string &url, std::string *error) {
    (void)url;
    if (error) *error = "WebSocket support is not compiled into this build";
    return WebSocket();
}

bool WebSocket::ReceiveText(std::string *text, std::string *error) {
    (void)text;
    if (error) *error = "WebSocket support is not compiled into this build";
    return false;
}

bool WebSocket::SendText(const std::string &text) {
    (void)text;
    return false;
}

bool WebSocket::SendClose(uint16_t code, const std::string &reason) {
    (void)code;
    (void)reason;
    return false;
}

bool WebSocket::SendFrame(uint8_t opcode, const uint8_t *data, size_t size) {
    (void)opcode;
    (void)data;
    (void)size;
    return false;
}

bool WebSocket::ReceiveExact(void *data, size_t size) {
    (void)data;
    (void)size;
    return false;
}

bool WebSocket::SendAll(const void *data, size_t size) {
    (void)data;
    (void)size;
    return false;
}

// Pure string handling with no transport involved, so this one is the
// real implementation rather than a stub -- kept byte-for-byte equivalent
// to collab_websocket.cpp's so a caller cannot tell the two builds apart
// here. Malformed escapes and absent keys both yield an empty string.
std::string QueryValue(const std::string &path, const std::string &key) {
    const size_t question = path.find('?');
    if (question == std::string::npos) return std::string();
    const std::string query = path.substr(question + 1);

    size_t pos = 0;
    while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        const std::string pair = query.substr(pos, amp - pos);
        pos = amp + 1;

        const size_t equals = pair.find('=');
        if (equals == std::string::npos) continue;
        if (pair.substr(0, equals) != key) continue;

        const std::string raw = pair.substr(equals + 1);
        std::string out;
        out.reserve(raw.size());
        for (size_t i = 0; i < raw.size(); i++) {
            if (raw[i] == '+') {
                out.push_back(' ');
            } else if (raw[i] == '%') {
                if (i + 2 >= raw.size()) return std::string();
                const auto hex = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return -1;
                };
                const int hi = hex(raw[i + 1]);
                const int lo = hex(raw[i + 2]);
                if (hi < 0 || lo < 0) return std::string();
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
            } else {
                out.push_back(raw[i]);
            }
        }
        return out;
    }
    return std::string();
}

}  // namespace mep::collab
