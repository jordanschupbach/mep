#include "collab_websocket.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits.h>
#include <sstream>
#include <utility>
#include <vector>

#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace mep::collab {
namespace {
constexpr size_t kMaxHttpHeader = size_t{16} * 1024;
constexpr uint64_t kMaxMessage = uint64_t{8} * 1024 * 1024;

/**
 * @brief Closes a platform socket handle.
 * @param fd The socket file descriptor to close.
 */
void CloseFd(int fd) {
#ifdef _WIN32
    closesocket(static_cast<SOCKET>(fd));
#else
    close(fd);
#endif
}
/**
 * @brief Shuts down both directions of a platform socket handle, used to unblock a thread waiting in recv().
 * @param fd The socket file descriptor to shut down.
 */
void ShutdownFd(int fd) {
#ifdef _WIN32
    shutdown(static_cast<SOCKET>(fd), SD_BOTH);
#else
    shutdown(fd, SHUT_RDWR);
#endif
}

/**
 * @brief Strips leading and trailing whitespace (spaces, tabs, CR, LF) from a string.
 * @param value The string to trim.
 * @return The trimmed string, or an empty string if `value` is all whitespace.
 */
std::string Trim(const std::string &value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

/**
 * @brief Converts a string to lowercase (ASCII).
 * @param value The string to convert.
 * @return The lowercased string.
 */
std::string Lower(std::string value) {
    for (char &c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

/**
 * @brief Encodes a byte buffer as standard Base64 text.
 * @param data Pointer to the bytes to encode.
 * @param size Number of bytes to encode.
 * @return The Base64-encoded string, padded with '=' as needed.
 */
std::string Base64(const unsigned char *data, size_t size) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t a = data[i];
        const uint32_t b = i + 1 < size ? data[i + 1] : 0;
        const uint32_t c = i + 2 < size ? data[i + 2] : 0;
        const uint32_t chunk = (a << 16) | (b << 8) | c;
        out.push_back(kAlphabet[(chunk >> 18) & 63]);
        out.push_back(kAlphabet[(chunk >> 12) & 63]);
        out.push_back(i + 1 < size ? kAlphabet[(chunk >> 6) & 63] : '=');
        out.push_back(i + 2 < size ? kAlphabet[chunk & 63] : '=');
    }
    return out;
}

/**
 * @brief Checks whether a comma-separated HTTP header value contains a given token, ignoring case and surrounding whitespace.
 * @param value The comma-separated header value to search.
 * @param needle The token to look for.
 * @return True if `needle` appears as one of the comma-separated tokens, false otherwise.
 */
bool HeaderHasToken(const std::string &value, const std::string &needle) {
    std::stringstream stream(value);
    std::string token;
    while (std::getline(stream, token, ',')) {
        if (Lower(Trim(token)) == Lower(needle)) return true;
    }
    return false;
}

/**
 * @brief Converts one hexadecimal digit character to its numeric value.
 * @param c The character to convert.
 * @return The digit's value (0-15), or -1 if `c` is not a hex digit.
 */
int Hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}  // namespace

WebSocket::WebSocket(WebSocket &&other) noexcept
    : fd_(other.fd_), ssl_(other.ssl_), ssl_ctx_(other.ssl_ctx_), peer_frames_masked_(other.peer_frames_masked_), receive_buffer_(std::move(other.receive_buffer_)) {
    other.fd_ = -1;
    other.ssl_ = nullptr;
    other.ssl_ctx_ = nullptr;
}
WebSocket &WebSocket::operator=(WebSocket &&other) noexcept {
    if (this != &other) {
        Close();
        fd_ = other.fd_;
        ssl_ = other.ssl_; other.ssl_ = nullptr;
        ssl_ctx_ = other.ssl_ctx_; other.ssl_ctx_ = nullptr;
        peer_frames_masked_ = other.peer_frames_masked_;
        receive_buffer_ = std::move(other.receive_buffer_);
        other.fd_ = -1;
    }
    return *this;
}
WebSocket::~WebSocket() { Close(); }
void WebSocket::Close() {
    // Close is also used to interrupt a worker blocked in recv(). Do the
    // kernel shutdown first; SSL_shutdown may wait for a peer close-notify
    // and would make :CollabLeave hang indefinitely on a broken network.
    if (fd_ >= 0) ShutdownFd(fd_);
    if (ssl_) { SSL_free(ssl_); ssl_ = nullptr; }
    if (ssl_ctx_) { SSL_CTX_free(ssl_ctx_); ssl_ctx_ = nullptr; }
    if (fd_ >= 0) { CloseFd(fd_); fd_ = -1; }
}

bool WebSocket::SendAll(const void *data, size_t size) {
    const char *p = static_cast<const char *>(data);
    while (size > 0) {
        int sent = 0;
        if (ssl_) sent = SSL_write(ssl_, p, static_cast<int>(std::min<size_t>(size, INT_MAX)));
        else {
#ifdef _WIN32
        sent = send(static_cast<SOCKET>(fd_), p, static_cast<int>(std::min<size_t>(size, INT_MAX)), 0);
#else
        sent = static_cast<int>(send(fd_, p, size, MSG_NOSIGNAL));
#endif
        }
        if (sent <= 0) return false;
        p += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

bool WebSocket::ReceiveExact(void *data, size_t size) {
    char *p = static_cast<char *>(data);
    if (!receive_buffer_.empty()) {
        const size_t available = std::min(size, receive_buffer_.size());
        std::memcpy(p, receive_buffer_.data(), available);
        receive_buffer_.erase(0, available);
        p += available;
        size -= available;
    }
    while (size > 0) {
        int received = 0;
        if (ssl_) received = SSL_read(ssl_, p, static_cast<int>(std::min<size_t>(size, INT_MAX)));
        else {
#ifdef _WIN32
        received = recv(static_cast<SOCKET>(fd_), p, static_cast<int>(std::min<size_t>(size, INT_MAX)), 0);
#else
        received = static_cast<int>(recv(fd_, p, size, 0));
#endif
        }
        if (received <= 0) return false;
        p += received;
        size -= static_cast<size_t>(received);
    }
    return true;
}

bool WebSocket::Accept(std::string *request_path, std::string *error) {
    std::string request;
    request.reserve(1024);
    std::array<char, 1024> chunk{};
    while (request.find("\r\n\r\n") == std::string::npos) {
#ifdef _WIN32
        const int n = recv(static_cast<SOCKET>(fd_), chunk.data(), static_cast<int>(chunk.size()), 0);
#else
        const ssize_t n = recv(fd_, chunk.data(), chunk.size(), 0);
#endif
        if (n <= 0) { if (error) *error = "connection closed during HTTP upgrade"; return false; }
        request.append(chunk.data(), static_cast<size_t>(n));
        if (request.size() > kMaxHttpHeader) { if (error) *error = "HTTP upgrade headers too large"; return false; }
    }
    const size_t header_end = request.find("\r\n\r\n");
    std::istringstream lines(request.substr(0, header_end));
    std::string line;
    if (!std::getline(lines, line)) { if (error) *error = "missing HTTP request line"; return false; }
    std::istringstream request_line(line);
    std::string method, path, version;
    request_line >> method >> path >> version;
    if (method != "GET" || path.empty() || version.rfind("HTTP/", 0) != 0) { if (error) *error = "expected HTTP GET upgrade"; return false; }
    std::string key, upgrade, connection, ws_version;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string name = Lower(Trim(line.substr(0, colon)));
        const std::string value = Trim(line.substr(colon + 1));
        if (name == "sec-websocket-key") key = value;
        else if (name == "upgrade") upgrade = value;
        else if (name == "connection") connection = value;
        else if (name == "sec-websocket-version") ws_version = value;
    }
    if (key.empty() || Lower(upgrade) != "websocket" || !HeaderHasToken(connection, "upgrade") || ws_version != "13") {
        if (error) *error = "invalid WebSocket upgrade headers";
        return false;
    }
    const std::string seed = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char digest[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char *>(seed.data()), seed.size(), digest);
    const std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + Base64(digest, sizeof(digest)) + "\r\n\r\n";
    if (!SendAll(response.data(), response.size())) { if (error) *error = "unable to send WebSocket upgrade"; return false; }
    // recv() may have returned the first WebSocket frame along with the
    // HTTP headers. Keep those bytes for ReceiveExact rather than dropping
    // them at the upgrade boundary.
    receive_buffer_ = request.substr(header_end + 4);
    if (request_path) *request_path = path;
    return true;
}

WebSocket WebSocket::Connect(const std::string &url, std::string *error) {
    const bool secure = url.rfind("wss://", 0) == 0;
    const bool plain = url.rfind("ws://", 0) == 0;
    if (!secure && !plain) { if (error) *error = "collaboration URL must start with ws:// or wss://"; return {}; }
    std::string rest = url.substr(secure ? 6 : 5);
    const size_t slash = rest.find('/'); const std::string authority = rest.substr(0, slash); const std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
    std::string host = authority, port = secure ? "443" : "80";
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) { host = authority.substr(0, colon); port = authority.substr(colon + 1); }
    if (host.empty() || port.empty()) { if (error) *error = "invalid collaboration URL"; return {}; }
    addrinfo hints{}; hints.ai_socktype = SOCK_STREAM; hints.ai_family = AF_UNSPEC; addrinfo *addresses = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &addresses) != 0) { if (error) *error = "unable to resolve relay host"; return {}; }
    int sock_fd = -1;
    for (addrinfo *it = addresses; it; it = it->ai_next) { sock_fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol); if (sock_fd >= 0 && connect(sock_fd, it->ai_addr, it->ai_addrlen) == 0) break; if (sock_fd >= 0) { CloseFd(sock_fd); sock_fd = -1; } }
    freeaddrinfo(addresses); if (sock_fd < 0) { if (error) *error = "unable to connect to relay"; return {}; }
    WebSocket socket(sock_fd); socket.peer_frames_masked_ = false;
    if (secure) {
        socket.ssl_ctx_ = SSL_CTX_new(TLS_client_method());
        if (!socket.ssl_ctx_ || SSL_CTX_set_default_verify_paths(socket.ssl_ctx_) != 1) { if (error) *error = "unable to initialize TLS trust store"; return {}; }
        SSL_CTX_set_verify(socket.ssl_ctx_, SSL_VERIFY_PEER, nullptr); socket.ssl_ = SSL_new(socket.ssl_ctx_);
        // SSL_set_tlsext_host_name is an OpenSSL macro that expands to an
        // old-style (void*) cast inside <openssl/ssl.h> -- not our code,
        // so the warning is suppressed locally rather than editing a
        // system header.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
        if (!socket.ssl_ || SSL_set_tlsext_host_name(socket.ssl_, host.c_str()) != 1 || SSL_set1_host(socket.ssl_, host.c_str()) != 1) { if (error) *error = "unable to initialize TLS connection"; return {}; }
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
        SSL_set_fd(socket.ssl_, sock_fd); if (SSL_connect(socket.ssl_) != 1 || SSL_get_verify_result(socket.ssl_) != X509_V_OK) { if (error) *error = "TLS certificate verification failed"; return {}; }
    }
    unsigned char nonce[16]; if (RAND_bytes(nonce, sizeof(nonce)) != 1) { if (error) *error = "unable to create WebSocket nonce"; return {}; }
    const std::string key = Base64(nonce, sizeof(nonce));
    const std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + authority + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " + key + "\r\n\r\n";
    if (!socket.SendAll(request.data(), request.size())) { if (error) *error = "unable to request WebSocket upgrade"; return {}; }
    std::string response; char c;
    while (response.find("\r\n\r\n") == std::string::npos && response.size() < kMaxHttpHeader) { if (!socket.ReceiveExact(&c, 1)) { if (error) *error = "relay closed during WebSocket upgrade"; return {}; } response.push_back(c); }
    if (response.rfind("HTTP/1.1 101", 0) != 0) { if (error) *error = "relay rejected WebSocket upgrade"; return {}; }
    const std::string seed = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"; unsigned char digest[SHA_DIGEST_LENGTH]; SHA1(reinterpret_cast<const unsigned char *>(seed.data()), seed.size(), digest);
    if (response.find("Sec-WebSocket-Accept: " + Base64(digest, sizeof(digest))) == std::string::npos) { if (error) *error = "relay returned invalid WebSocket accept key"; return {}; }
    return socket;
}

bool WebSocket::SendFrame(uint8_t opcode, const uint8_t *data, size_t size) {
    if (!valid() || size > kMaxMessage) return false;
    std::vector<uint8_t> header;
    header.push_back(static_cast<uint8_t>(0x80 | opcode));
    const bool mask = !peer_frames_masked_;
    if (size < 126) header.push_back(static_cast<uint8_t>((mask ? 0x80 : 0) | size));
    else if (size <= 0xffff) {
        header.push_back(mask ? 0xfe : 126); header.push_back(static_cast<uint8_t>(size >> 8)); header.push_back(static_cast<uint8_t>(size));
    } else {
        header.push_back(mask ? 0xff : 127);
        for (int shift = 56; shift >= 0; shift -= 8) header.push_back(static_cast<uint8_t>(static_cast<uint64_t>(size) >> shift));
    }
    if (!SendAll(header.data(), header.size())) return false;
    if (!mask) return size == 0 || SendAll(data, size);
    unsigned char mask_bytes[4]; if (RAND_bytes(mask_bytes, sizeof(mask_bytes)) != 1 || !SendAll(mask_bytes, sizeof(mask_bytes))) return false;
    std::vector<unsigned char> masked(data, data + size); for (size_t i = 0; i < size; ++i) masked[i] ^= mask_bytes[i % 4];
    return size == 0 || SendAll(masked.data(), size);
}

bool WebSocket::SendText(const std::string &text) { return SendFrame(0x1, reinterpret_cast<const uint8_t *>(text.data()), text.size()); }
bool WebSocket::SendClose(uint16_t code, const std::string &reason) {
    std::string payload;
    payload.push_back(static_cast<char>(code >> 8)); payload.push_back(static_cast<char>(code)); payload += reason;
    return SendFrame(0x8, reinterpret_cast<const uint8_t *>(payload.data()), payload.size());
}

bool WebSocket::ReceiveText(std::string *text, std::string *error) {
    if (text) text->clear();
    std::string assembled;
    bool fragmented = false;
    for (;;) {
        uint8_t header[2];
        if (!ReceiveExact(header, sizeof(header))) { if (error) *error = "socket read failed"; return false; }
        const bool fin = (header[0] & 0x80) != 0;
        const uint8_t opcode = header[0] & 0x0f;
        const bool masked = (header[1] & 0x80) != 0;
        uint64_t size = header[1] & 0x7f;
        if (masked != peer_frames_masked_) { if (error) *error = "WebSocket frame used the wrong masking mode"; return false; }
        if (size == 126) { uint8_t x[2]; if (!ReceiveExact(x, 2)) return false; size = (static_cast<uint64_t>(x[0]) << 8) | x[1]; }
        else if (size == 127) { uint8_t x[8]; if (!ReceiveExact(x, 8)) return false; size = 0; for (uint8_t b : x) size = (size << 8) | b; }
        if (size > kMaxMessage) { if (error) *error = "WebSocket message exceeds 8 MiB limit"; return false; }
        uint8_t mask[4]{}; if (masked && !ReceiveExact(mask, sizeof(mask))) return false;
        std::vector<uint8_t> payload(static_cast<size_t>(size));
        if (size && !ReceiveExact(payload.data(), payload.size())) return false;
        for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= mask[i % 4];
        if (opcode == 0x8) { SendClose(); if (error) *error = "peer closed WebSocket"; return false; }
        if (opcode == 0x9) { if (!SendFrame(0xA, payload.data(), payload.size())) return false; continue; }
        if (opcode == 0xA) continue;
        if (opcode != 0 && opcode != 0x1) { if (error) *error = "unsupported WebSocket frame opcode"; return false; }
        if (opcode == 0x1 && fragmented) { if (error) *error = "new data frame while fragmented message active"; return false; }
        if (opcode == 0x1 || opcode == 0x0) assembled.append(reinterpret_cast<const char *>(payload.data()), payload.size());
        if (assembled.size() > kMaxMessage) { if (error) *error = "fragmented message exceeds 8 MiB limit"; return false; }
        fragmented = !fin;
        if (fin) { if (text) *text = std::move(assembled); return true; }
    }
}

std::string QueryValue(const std::string &path, const std::string &key) {
    const size_t question = path.find('?');
    if (question == std::string::npos) return "";
    const std::string query = path.substr(question + 1);
    size_t pos = 0;
    while (pos <= query.size()) {
        const size_t amp = query.find('&', pos);
        const std::string item = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        const size_t equals = item.find('=');
        if (item.substr(0, equals) == key && equals != std::string::npos) {
            std::string out;
            for (size_t i = equals + 1; i < item.size(); ++i) {
                if (item[i] == '+' ) out.push_back(' ');
                else if (item[i] == '%' && i + 2 < item.size()) {
                    const int a = Hex(item[i + 1]), b = Hex(item[i + 2]);
                    if (a < 0 || b < 0) return "";
                    out.push_back(static_cast<char>((a << 4) | b)); i += 2;
                } else out.push_back(item[i]);
            }
            return out;
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return "";
}

}  // namespace mep::collab
