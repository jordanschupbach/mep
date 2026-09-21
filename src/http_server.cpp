#include "http_server.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

std::string HttpContentTypeForPath(const std::string &path) {
    std::string ext = fs::path(path).extension().string();
    for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const std::pair<const char *, const char *> kTypes[] = {
        {".html", "text/html; charset=utf-8"}, {".htm", "text/html; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"}, {".mjs", "text/javascript; charset=utf-8"},
        {".css", "text/css; charset=utf-8"}, {".json", "application/json"}, {".map", "application/json"},
        {".txt", "text/plain; charset=utf-8"}, {".md", "text/plain; charset=utf-8"}, {".org", "text/plain; charset=utf-8"},
        {".svg", "image/svg+xml"}, {".png", "image/png"}, {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"},
        {".gif", "image/gif"}, {".bmp", "image/bmp"}, {".ico", "image/x-icon"}, {".webp", "image/webp"},
        {".wasm", "application/wasm"}, {".wav", "audio/wav"}, {".mp3", "audio/mpeg"}, {".mp4", "video/mp4"},
        {".woff", "font/woff"}, {".woff2", "font/woff2"}, {".ttf", "font/ttf"}, {".xml", "application/xml"},
        {".pdf", "application/pdf"},
    };
    for (const auto &[suffix, type] : kTypes) {
        if (ext == suffix) return type;
    }
    return "application/octet-stream";
}

HttpStaticServer::~HttpStaticServer() { Stop(); }

#if defined(__EMSCRIPTEN__) || defined(_WIN32)

bool HttpStaticServer::Start(const std::string &, int, std::string *error) {
    if (error) *error = "the built-in web server is not available in this build";
    return false;
}
void HttpStaticServer::Stop() {}
void HttpStaticServer::AcceptLoop() {}

#else

namespace {

/** @brief Decodes %XX escapes (and leaves everything else, including '+', alone). */
std::string PercentDecode(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string HtmlEscape(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}

void SendAll(int fd, const std::string &data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return;
        sent += static_cast<size_t>(n);
    }
}

void Respond(int fd, int status, const std::string &status_text, const std::string &content_type, const std::string &body,
             bool head_only, const std::string &extra_headers = "") {
    std::ostringstream head;
    head << "HTTP/1.1 " << status << ' ' << status_text << "\r\n"
         << "Content-Type: " << content_type << "\r\n"
         << "Content-Length: " << body.size() << "\r\n"
         << "Cache-Control: no-store\r\n"
         << "Access-Control-Allow-Origin: *\r\n"
         << extra_headers << "Connection: close\r\n\r\n";
    SendAll(fd, head.str());
    if (!head_only) SendAll(fd, body);
}

/** @brief A plain HTML listing of `dir`, linked relative to `url_path` (which ends in '/'). */
std::string DirectoryListing(const fs::path &dir, const std::string &url_path) {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
        std::string name = entry.path().filename().string();
        if (entry.is_directory(ec)) name += "/";
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    std::string html = "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>Index of " + HtmlEscape(url_path) +
                       "</title></head><body><h1>Index of " + HtmlEscape(url_path) + "</h1><ul>";
    if (url_path != "/") html += "<li><a href=\"../\">../</a></li>";
    for (const std::string &name : names) html += "<li><a href=\"" + HtmlEscape(name) + "\">" + HtmlEscape(name) + "</a></li>";
    html += "</ul></body></html>";
    return html;
}

/** @brief Reads one request head, answers it from `root`, closes the socket. */
void HandleConnection(int fd, const std::string &root) {
    std::string request;
    char buf[4096];
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 65536) {
        pollfd pfd{fd, POLLIN, 0};
        if (poll(&pfd, 1, 5000) != 1) break;
        const ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        request.append(buf, static_cast<size_t>(n));
    }
    std::istringstream first(request.substr(0, request.find("\r\n")));
    std::string method, target, version;
    first >> method >> target >> version;
    const bool head_only = method == "HEAD";
    if (method.empty() || target.empty() || target[0] != '/') {
        Respond(fd, 400, "Bad Request", "text/plain; charset=utf-8", "bad request\n", false);
    } else if (method != "GET" && method != "HEAD") {
        Respond(fd, 405, "Method Not Allowed", "text/plain; charset=utf-8", "only GET and HEAD are served\n", false, "Allow: GET, HEAD\r\n");
    } else {
        const std::string url_path = PercentDecode(target.substr(0, target.find_first_of("?#")));
        std::error_code ec;
        const fs::path base = fs::weakly_canonical(fs::path(root), ec);
        fs::path wanted = fs::weakly_canonical(base / fs::path(url_path).relative_path(), ec);
        // Confinement: the canonical target must sit under the canonical root.
        const std::string base_s = base.string(), wanted_s = wanted.string();
        const bool inside = wanted_s == base_s || (wanted_s.size() > base_s.size() && wanted_s.compare(0, base_s.size(), base_s) == 0 &&
                                                   wanted_s[base_s.size()] == '/');
        if (ec || !inside || url_path.find('\0') != std::string::npos) {
            Respond(fd, 403, "Forbidden", "text/plain; charset=utf-8", "forbidden\n", head_only);
        } else if (fs::is_directory(wanted, ec)) {
            if (url_path.back() != '/') {
                // Relative links inside a directory page only resolve correctly from the slash form.
                Respond(fd, 301, "Moved Permanently", "text/plain; charset=utf-8", "", head_only, "Location: " + url_path + "/\r\n");
            } else if (fs::is_regular_file(wanted / "index.html", ec)) {
                std::ifstream in(wanted / "index.html", std::ios::binary);
                std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                Respond(fd, 200, "OK", "text/html; charset=utf-8", body, head_only);
            } else {
                Respond(fd, 200, "OK", "text/html; charset=utf-8", DirectoryListing(wanted, url_path), head_only);
            }
        } else if (fs::is_regular_file(wanted, ec)) {
            std::ifstream in(wanted, std::ios::binary);
            std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            Respond(fd, 200, "OK", HttpContentTypeForPath(wanted_s), body, head_only);
        } else {
            Respond(fd, 404, "Not Found", "text/plain; charset=utf-8", "not found: " + url_path + "\n", head_only);
        }
    }
    shutdown(fd, SHUT_RDWR);
    close(fd);
}

}  // namespace

bool HttpStaticServer::Start(const std::string &root, int port, std::string *error) {
    Stop();
    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(fs::path(root), ec);
    if (ec || !fs::is_directory(canonical, ec)) {
        if (error) *error = "not a directory: " + root;
        return false;
    }
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        if (error) *error = "cannot create a socket";
        return false;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || listen(fd, 64) != 0) {
        close(fd);
        if (error) *error = "cannot listen on 127.0.0.1:" + std::to_string(port) + " (in use?)";
        return false;
    }
    socklen_t len = sizeof(addr);
    getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len);
    root_ = canonical.string();
    port_ = ntohs(addr.sin_port);
    listen_fd_ = fd;
    running_.store(true);
    thread_ = std::thread([this] { AcceptLoop(); });
    return true;
}

void HttpStaticServer::AcceptLoop() {
    while (running_.load()) {
        pollfd pfd{listen_fd_, POLLIN, 0};
        const int ready = poll(&pfd, 1, 200);  // wake regularly so Stop() is prompt
        if (ready != 1) continue;
        const int client = accept(listen_fd_, nullptr, nullptr);
        if (client < 0) continue;
        requests_.fetch_add(1);
        // `root` is copied: the worker may outlive this server object.
        std::thread(HandleConnection, client, root_).detach();
    }
}

void HttpStaticServer::Stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    if (listen_fd_ >= 0) close(listen_fd_);
    listen_fd_ = -1;
    port_ = 0;
}

#endif
