#pragma once

// Small synchronous RFC 6455 WebSocket implementation used by mep-collabd.
// It intentionally has no HTTP/WebSocket dependency; OpenSSL is used only for
// SHA-1 during the WebSocket upgrade handshake.

#include <cstdint>
#include <string>

typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;

namespace mep::collab {

class WebSocket {
public:
    WebSocket() = default;
    explicit WebSocket(int fd) : fd_(fd) {}
    WebSocket(const WebSocket &) = delete;
    WebSocket &operator=(const WebSocket &) = delete;
    WebSocket(WebSocket &&other) noexcept;
    WebSocket &operator=(WebSocket &&other) noexcept;
    ~WebSocket();

    bool valid() const { return fd_ >= 0; }
    int fd() const { return fd_; }
    void Close();

    // Performs a server-side HTTP Upgrade. `request_path` receives the target
    // (including query string) after a successful handshake.
    bool Accept(std::string *request_path, std::string *error);
    // Connects to ws:// or wss://. TLS certificate and hostname verification
    // are enabled for wss:// connections.
    static WebSocket Connect(const std::string &url, std::string *error);

    // Reads one complete text message. Ping/Pong frames are handled here.
    // Returns false on close, malformed input, or an I/O error.
    bool ReceiveText(std::string *text, std::string *error);
    bool SendText(const std::string &text);
    bool SendClose(uint16_t code = 1000, const std::string &reason = "");

private:
    int fd_ = -1;
    SSL *ssl_ = nullptr;
    SSL_CTX *ssl_ctx_ = nullptr;
    bool peer_frames_masked_ = true;
    std::string receive_buffer_;
    bool SendFrame(uint8_t opcode, const uint8_t *data, size_t size);
    bool ReceiveExact(void *data, size_t size);
    bool SendAll(const void *data, size_t size);
};

// A percent-decoding query helper. It accepts absent keys and malformed
// percent escapes as empty strings rather than attempting to repair input.
std::string QueryValue(const std::string &path, const std::string &key);

}  // namespace mep::collab
