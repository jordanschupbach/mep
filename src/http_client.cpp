#include "http_client.h"

#include "url_util.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

std::string HttpResponse::ContentType() const {
    auto it = headers.find("content-type");
    if (it == headers.end()) return "";
    std::string type = it->second.substr(0, it->second.find(';'));
    while (!type.empty() && std::isspace(static_cast<unsigned char>(type.back()))) type.pop_back();
    for (char &c : type) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return type;
}

namespace {

std::string Lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Trim(const std::string &s) {
    size_t a = 0, z = s.size();
    while (a < z && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (z > a && std::isspace(static_cast<unsigned char>(s[z - 1]))) z--;
    return s.substr(a, z - a);
}

/**
 * @brief Splits a raw HTTP/1.x response into status line, headers and (de-chunked) body.
 * @param raw Everything read from the socket.
 * @param out Response to fill.
 * @return False when `raw` doesn't start with a parseable status line and header block.
 */
bool ParseRawResponse(const std::string &raw, HttpResponse &out) {
    const size_t head_end = raw.find("\r\n\r\n");
    if (head_end == std::string::npos || raw.rfind("HTTP/", 0) != 0) return false;
    std::istringstream head(raw.substr(0, head_end));
    std::string line;
    std::getline(head, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos) return false;
    const size_t sp2 = line.find(' ', sp1 + 1);
    out.status = std::atoi(line.substr(sp1 + 1, sp2 == std::string::npos ? std::string::npos : sp2 - sp1 - 1).c_str());
    out.status_text = sp2 == std::string::npos ? "" : line.substr(sp2 + 1);
    while (std::getline(head, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        out.headers[Lower(Trim(line.substr(0, colon)))] = Trim(line.substr(colon + 1));
    }
    std::string body = raw.substr(head_end + 4);
    auto te = out.headers.find("transfer-encoding");
    if (te != out.headers.end() && Lower(te->second).find("chunked") != std::string::npos) {
        std::string decoded;
        size_t pos = 0;
        while (pos < body.size()) {
            const size_t eol = body.find("\r\n", pos);
            if (eol == std::string::npos) break;
            const size_t size = std::strtoul(body.substr(pos, eol - pos).c_str(), nullptr, 16);
            if (size == 0) break;
            pos = eol + 2;
            if (pos + size > body.size()) {
                decoded.append(body, pos, std::string::npos);
                break;
            }
            decoded.append(body, pos, size);
            pos += size + 2;
        }
        body = std::move(decoded);
    } else {
        auto cl = out.headers.find("content-length");
        if (cl != out.headers.end()) {
            const size_t n = std::strtoul(cl->second.c_str(), nullptr, 10);
            if (n < body.size()) body.resize(n);
        }
    }
    out.body = std::move(body);
    return true;
}

#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)

/** @brief Milliseconds left until `deadline`, clamped at zero. */
int MsLeft(std::chrono::steady_clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    return left < 0 ? 0 : static_cast<int>(left);
}

/**
 * @brief One plain-HTTP exchange over a fresh socket.
 * @param u Parsed target URL (scheme http).
 * @param deadline When to give up.
 * @param out Response to fill (`ok`/`error` set here).
 */
void PlainGet(const urlutil::ParsedUrl &u, std::chrono::steady_clock::time_point deadline, HttpResponse &out) {
    const int port = u.port ? u.port : 80;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *res = nullptr;
    if (getaddrinfo(u.host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
        out.error = "cannot resolve host " + u.host;
        return;
    }
    int fd = -1;
    for (addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        const int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            pollfd pfd{fd, POLLOUT, 0};
            if (poll(&pfd, 1, MsLeft(deadline)) == 1) {
                int err = 0;
                socklen_t len = sizeof(err);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                rc = err == 0 ? 0 : -1;
            } else {
                rc = -1;
            }
        }
        if (rc == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        out.error = "cannot connect to " + urlutil::HostWithPort(u);
        return;
    }
    const std::string request = "GET " + u.path + u.query + " HTTP/1.1\r\nHost: " + urlutil::HostWithPort(u) +
                                "\r\nUser-Agent: mep-browser/1.0\r\nAccept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n";
    size_t sent = 0;
    while (sent < request.size()) {
        pollfd pfd{fd, POLLOUT, 0};
        if (poll(&pfd, 1, MsLeft(deadline)) != 1) break;
        const ssize_t n = send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) break;
        sent += static_cast<size_t>(n);
    }
    if (sent < request.size()) {
        close(fd);
        out.error = "failed sending the request";
        return;
    }
    std::string raw;
    char buf[16384];
    bool timed_out = false;
    for (;;) {
        pollfd pfd{fd, POLLIN, 0};
        const int ready = poll(&pfd, 1, MsLeft(deadline));
        if (ready != 1) {
            timed_out = true;
            break;
        }
        const ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            break;
        }
        raw.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    if (!ParseRawResponse(raw, out)) {
        out.error = timed_out ? "timed out" : "malformed response";
        return;
    }
    out.ok = true;
}

/**
 * @brief GET through a `curl` subprocess (TLS, redirects, decompression are curl's job).
 * @param url The URL.
 * @param timeout_ms Whole-transfer budget.
 * @param out Response to fill.
 */
void CurlGet(const std::string &url, int timeout_ms, HttpResponse &out) {
    char head_path[] = "/tmp/mep-http-head-XXXXXX";
    char body_path[] = "/tmp/mep-http-body-XXXXXX";
    const int hfd = mkstemp(head_path);
    const int bfd = mkstemp(body_path);
    if (hfd < 0 || bfd < 0) {
        out.error = "cannot create temp files";
        return;
    }
    close(hfd);
    close(bfd);
    const std::string max_time = std::to_string(std::max(1, timeout_ms / 1000));
    std::vector<std::string> args = {"curl", "-sS", "-L", "--max-time", max_time, "-D", head_path, "-o", body_path, "--", url};
    std::vector<char *> argv;
    for (std::string &a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, "curl", nullptr, nullptr, argv.data(), environ);
    int status = 0;
    if (rc != 0 || waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        out.error = rc != 0 ? "curl is not available for https:// URLs" : "curl failed for " + url;
    } else {
        std::ifstream head(head_path, std::ios::binary);
        std::string headers((std::istreambuf_iterator<char>(head)), std::istreambuf_iterator<char>());
        // With -L, -D holds one header block per hop; the last one is the real response.
        const size_t last = headers.rfind("HTTP/");
        std::ifstream body(body_path, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(body)), std::istreambuf_iterator<char>());
        HttpResponse parsed;
        std::string block = last == std::string::npos ? "" : headers.substr(last);
        if (block.find("\r\n\r\n") == std::string::npos) block += "\r\n\r\n";
        if (ParseRawResponse(block, parsed)) {
            out = parsed;
            out.headers.erase("transfer-encoding");
            out.body = std::move(bytes);
            out.ok = true;
        } else {
            out.error = "malformed response";
        }
    }
    unlink(head_path);
    unlink(body_path);
}

#endif

}  // namespace

HttpResponse HttpGet(const std::string &url, int timeout_ms) {
    HttpResponse out;
    out.url = url;
#if defined(__EMSCRIPTEN__) || defined(_WIN32)
    (void)timeout_ms;
    out.error = "network requests are not available in this build";
    return out;
#else
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::string current = url;
    for (int hop = 0; hop < 6; hop++) {
        urlutil::ParsedUrl u = urlutil::ParseUrl(current);
        // Like a browser, "." and ".." segments are resolved before the
        // request is made; a server only ever sees a normalized path.
        if (u.valid) u.path = urlutil::RemoveDotSegments(u.path);
        if (!u.valid || (u.scheme != "http" && u.scheme != "https")) {
            out.error = "not an http(s) URL: " + current;
            return out;
        }
        out = HttpResponse();
        out.url = current;
        if (u.scheme == "https") {
            CurlGet(current, MsLeft(deadline), out);  // curl follows redirects itself
            return out;
        }
        PlainGet(u, deadline, out);
        if (!out.ok) return out;
        auto loc = out.headers.find("location");
        if (out.status >= 300 && out.status < 400 && loc != out.headers.end() && !loc->second.empty()) {
            current = urlutil::ResolveUrl(current, loc->second);
            continue;
        }
        return out;
    }
    out.ok = false;
    out.error = "too many redirects";
    return out;
#endif
}
