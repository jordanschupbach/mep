#pragma once

// Small synchronous RFC 6455 WebSocket implementation used by mep-collabd.
// It intentionally has no HTTP/WebSocket dependency; OpenSSL is used only for
// SHA-1 during the WebSocket upgrade handshake.

#include <stddef.h>
#include <cstdint>
#include <string>

using SSL = struct ssl_st;
using SSL_CTX = struct ssl_ctx_st;

namespace mep::collab {

class WebSocket {
public:
    /**
     * @brief Constructs an invalid (unconnected) WebSocket.
     */
    WebSocket() = default;
    /**
     * @brief Wraps an already-connected socket file descriptor.
     * @param fd An open, connected socket file descriptor.
     */
    explicit WebSocket(int fd) : fd_(fd) {}
    /**
     * @brief Disabled: a WebSocket owns a socket and TLS state and cannot be copy-constructed.
     */
    WebSocket(const WebSocket &) = delete;
    /**
     * @brief Disabled: a WebSocket owns a socket and TLS state and cannot be copy-assigned.
     */
    WebSocket &operator=(const WebSocket &) = delete;
    /**
     * @brief Transfers ownership of the socket, TLS state, and buffered bytes from `other`, leaving it invalid.
     * @param other The WebSocket to move from.
     */
    WebSocket(WebSocket &&other) noexcept;
    /**
     * @brief Closes this socket (if any), then transfers ownership of `other`'s socket, TLS state, and buffered bytes, leaving `other` invalid.
     * @param other The WebSocket to move from.
     * @return A reference to this WebSocket.
     */
    WebSocket &operator=(WebSocket &&other) noexcept;
    /**
     * @brief Closes the socket and releases TLS resources, if any.
     */
    ~WebSocket();

    /**
     * @brief Reports whether this object holds an open socket.
     * @return True if the underlying file descriptor is valid, false otherwise.
     */
    bool valid() const { return fd_ >= 0; }
    /**
     * @brief Returns the underlying socket file descriptor.
     * @return The socket file descriptor, or a negative value if invalid.
     */
    int fd() const { return fd_; }
    /**
     * @brief Shuts down and closes the socket and frees any TLS state, making the object invalid.
     */
    void Close();

    // Performs a server-side HTTP Upgrade. `request_path` receives the target
    // (including query string) after a successful handshake.
    /**
     * @brief Reads and validates an HTTP Upgrade request on this socket and replies with the WebSocket handshake response.
     * @param request_path Out parameter set to the request target (including query string) on success.
     * @param error Out parameter set to a description of the failure on error.
     * @return True if the upgrade handshake completed successfully, false otherwise.
     */
    bool Accept(std::string *request_path, std::string *error);
    // Connects to ws:// or wss://. TLS certificate and hostname verification
    // are enabled for wss:// connections.
    /**
     * @brief Resolves and connects to a ws:// or wss:// URL and performs the client-side WebSocket handshake.
     * @param url The target URL; must start with "ws://" or "wss://".
     * @param error Out parameter set to a description of the failure on error.
     * @return A connected, valid WebSocket on success, or an invalid WebSocket on failure.
     */
    static WebSocket Connect(const std::string &url, std::string *error);

    // Reads one complete text message. Ping/Pong frames are handled here.
    // Returns false on close, malformed input, or an I/O error.
    /**
     * @brief Reads frames until one complete text message has been assembled, transparently answering Ping frames and skipping Pong frames.
     * @param text Out parameter set to the received message text on success.
     * @param error Out parameter set to a description of the failure when the read ends without a complete message.
     * @return True if a complete text message was received, false on close, malformed input, or an I/O error.
     */
    bool ReceiveText(std::string *text, std::string *error);
    /**
     * @brief Sends `text` as a single WebSocket text frame.
     * @param text The message text to send.
     * @return True if the frame was sent successfully, false otherwise.
     */
    bool SendText(const std::string &text);
    /**
     * @brief Sends a WebSocket Close frame with the given status code and reason.
     * @param code The WebSocket close status code.
     * @param reason Optional human-readable close reason.
     * @return True if the frame was sent successfully, false otherwise.
     */
    bool SendClose(uint16_t code = 1000, const std::string &reason = "");

private:
    int fd_ = -1;
    SSL *ssl_ = nullptr;
    SSL_CTX *ssl_ctx_ = nullptr;
    bool peer_frames_masked_ = true;
    std::string receive_buffer_;
    /**
     * @brief Encodes and sends one WebSocket frame, masking the payload when this side must mask outgoing frames.
     * @param opcode The WebSocket frame opcode.
     * @param data Pointer to the payload bytes.
     * @param size Number of payload bytes.
     * @return True if the frame was sent successfully, false if invalid or the payload exceeds the maximum message size.
     */
    bool SendFrame(uint8_t opcode, const uint8_t *data, size_t size);
    /**
     * @brief Fills `data` with exactly `size` bytes, first draining any buffered bytes left over from a previous read, then reading from the socket/TLS layer.
     * @param data Destination buffer.
     * @param size Number of bytes to read.
     * @return True if exactly `size` bytes were read, false on I/O error or closed connection.
     */
    bool ReceiveExact(void *data, size_t size);
    /**
     * @brief Writes exactly `size` bytes to the socket or TLS layer, looping until all bytes are sent.
     * @param data Pointer to the bytes to send.
     * @param size Number of bytes to send.
     * @return True if all bytes were sent successfully, false on I/O error.
     */
    bool SendAll(const void *data, size_t size);
};

// A percent-decoding query helper. It accepts absent keys and malformed
// percent escapes as empty strings rather than attempting to repair input.
/**
 * @brief Extracts and percent-decodes the value of one key from a URL's query string.
 * @param path A URL or request path, optionally including a "?"-prefixed query string.
 * @param key The query parameter name to look up.
 * @return The decoded value, or an empty string if the key is absent or a percent escape is malformed.
 */
std::string QueryValue(const std::string &path, const std::string &key);

}  // namespace mep::collab
