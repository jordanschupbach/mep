// mep-collabd: a small self-hosted relay for MEP's operation-based CRDT.
// It deliberately stores no document contents: authenticated clients exchange
// opaque CRDT operations, while this process holds only transient rooms and
// participant presence. Put it behind a TLS reverse proxy for public use.

#include "collab_websocket.h"
#include "json.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <openssl/rand.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace {
using mep::collab::WebSocket;

void CloseFd(int fd) {
#ifdef _WIN32
    closesocket(static_cast<SOCKET>(fd));
#else
    close(fd);
#endif
}

bool SendAll(int fd, const std::string &message) {
    size_t offset = 0;
    while (offset < message.size()) {
#ifdef _WIN32
        const int n = send(static_cast<SOCKET>(fd), message.data() + offset, static_cast<int>(message.size() - offset), 0);
#else
        const ssize_t n = send(fd, message.data() + offset, message.size() - offset, MSG_NOSIGNAL);
#endif
        if (n <= 0) return false;
        offset += static_cast<size_t>(n);
    }
    return true;
}

bool ReadHttpRequest(int fd, std::string *request) {
    request->clear();
    char chunk[1024];
    while (request->find("\r\n\r\n") == std::string::npos && request->size() < 16 * 1024) {
#ifdef _WIN32
        const int n = recv(static_cast<SOCKET>(fd), chunk, sizeof(chunk), MSG_PEEK);
#else
        const ssize_t n = recv(fd, chunk, sizeof(chunk), MSG_PEEK);
#endif
        if (n <= 0) return false;
        request->append(chunk, static_cast<size_t>(n));
        const size_t end = request->find("\r\n\r\n");
        if (end != std::string::npos) return true;
        // MSG_PEEK sees the same bytes every time; this path means a peer sent
        // an overlong header, not that we should append duplicates forever.
        if (request->size() >= sizeof(chunk)) return false;
    }
    return request->find("\r\n\r\n") != std::string::npos;
}

// Reads (consumes) a small non-upgrade HTTP request. The relay API has only a
// session-creation endpoint, so bounded request bodies are enough.
bool ConsumeHttpRequest(int fd, std::string *request) {
    request->clear();
    char chunk[1024];
    while (request->find("\r\n\r\n") == std::string::npos && request->size() < 16 * 1024) {
#ifdef _WIN32
        const int n = recv(static_cast<SOCKET>(fd), chunk, sizeof(chunk), 0);
#else
        const ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
#endif
        if (n <= 0) return false;
        request->append(chunk, static_cast<size_t>(n));
    }
    return request->find("\r\n\r\n") != std::string::npos;
}

void Http(int fd, int status, const std::string &body) {
    const char *reason = status == 200 ? "OK" : status == 201 ? "Created" : status == 401 ? "Unauthorized" : status == 404 ? "Not Found" : "Bad Request";
    SendAll(fd, "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
}

std::string RandomToken(size_t bytes = 24) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::vector<unsigned char> random(bytes * 2);
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1) return "";
    std::string out;
    out.reserve(bytes * 2);
    for (unsigned char value : random) out += alphabet[value & 63];
    return out;
}

struct Peer {
    std::string id;
    std::string name;
    std::shared_ptr<WebSocket> socket;
    std::mutex send_mutex;
};
struct Session { std::string secret; std::vector<std::shared_ptr<Peer>> peers; };

class Relay {
public:
    Relay(std::string admin_token, std::string public_url) : admin_token_(std::move(admin_token)), public_url_(std::move(public_url)) {}

    void Handle(int fd) {
        std::string peek;
        if (!ReadHttpRequest(fd, &peek)) { CloseFd(fd); return; }
        if (peek.find("Upgrade: websocket") == std::string::npos && peek.find("Upgrade: WebSocket") == std::string::npos) {
            HandleHttp(fd); CloseFd(fd); return;
        }
        auto socket = std::make_shared<WebSocket>(fd);
        std::string path, error;
        if (!socket->Accept(&path, &error)) { socket->Close(); return; }
        const std::string prefix = "/v1/session/";
        const size_t query = path.find('?');
        const std::string route = path.substr(0, query);
        if (route.rfind(prefix, 0) != 0 || route.size() == prefix.size()) { socket->SendClose(1008, "unknown session"); return; }
        const std::string session_id = route.substr(prefix.size());
        const std::string secret = mep::collab::QueryValue(path, "secret");
        const std::string name = mep::collab::QueryValue(path, "name");
        std::shared_ptr<Peer> peer = std::make_shared<Peer>();
        peer->id = RandomToken(8); peer->name = name.empty() ? "Anonymous" : name.substr(0, 80); peer->socket = std::move(socket);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = sessions_.find(session_id);
            if (it == sessions_.end() || it->second.secret != secret) { peer->socket->SendClose(1008, "invalid session capability"); return; }
            it->second.peers.push_back(peer);
            SendRosterLocked(it->second, peer);
            BroadcastLocked(it->second, Message("peer_join", *peer), peer->id);
        }
        std::string text;
        while (peer->socket->ReceiveText(&text, &error)) HandleMessage(session_id, peer, text);
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(session_id);
        if (it != sessions_.end()) {
            auto &peers = it->second.peers;
            peers.erase(std::remove_if(peers.begin(), peers.end(), [&](const std::shared_ptr<Peer> &p) { return p == peer; }), peers.end());
            BroadcastLocked(it->second, Message("peer_leave", *peer), peer->id);
        }
    }

private:
    std::mutex mutex_;
    std::map<std::string, Session> sessions_;
    std::string admin_token_;
    std::string public_url_;

    static Json Message(const std::string &type, const Peer &peer) {
        Json message = Json::Object(); message["type"] = type; message["peer"] = peer.id; message["name"] = peer.name; return message;
    }
    static void Send(const std::shared_ptr<Peer> &peer, const Json &message) {
        std::lock_guard<std::mutex> send_lock(peer->send_mutex);
        peer->socket->SendText(message.dump());
    }
    static void BroadcastLocked(const Session &session, const Json &message, const std::string &except = "") {
        for (const auto &peer : session.peers) if (peer->id != except) Send(peer, message);
    }
    static void SendRosterLocked(const Session &session, const std::shared_ptr<Peer> &peer) {
        Json welcome = Message("welcome", *peer);
        Json roster = Json::Array();
        for (const auto &other : session.peers) {
            if (other == peer) continue;  // a client must never render itself as a remote cursor
            Json item = Json::Object(); item["peer"] = other->id; item["name"] = other->name; roster.push_back(std::move(item));
        }
        welcome["peers"] = std::move(roster); Send(peer, welcome);
    }
    void HandleHttp(int fd) {
        std::string request;
        if (!ConsumeHttpRequest(fd, &request)) return;
        const size_t line_end = request.find("\r\n");
        const std::string line = request.substr(0, line_end);
        const bool auth = request.find("\r\nAuthorization: Bearer " + admin_token_ + "\r\n") != std::string::npos;
        if (line != "POST /v1/sessions HTTP/1.1") { Http(fd, 404, "{\"error\":\"not found\"}"); return; }
        if (admin_token_.empty() || !auth) { Http(fd, 401, "{\"error\":\"admin authorization required\"}"); return; }
        const std::string id = RandomToken(10), secret = RandomToken(24);
        if (id.empty() || secret.empty()) { Http(fd, 400, "{\"error\":\"unable to generate session capability\"}"); return; }
        { std::lock_guard<std::mutex> lock(mutex_); sessions_[id].secret = secret; }
        Json body = Json::Object(); body["id"] = id; body["secret"] = secret; body["path"] = "/v1/session/" + id;
        if (!public_url_.empty()) {
            while (!public_url_.empty() && public_url_.back() == '/') public_url_.pop_back();
            body["link"] = public_url_ + "/v1/session/" + id + "?secret=" + secret;
        }
        Http(fd, 201, body.dump());
    }
    void HandleMessage(const std::string &session_id, const std::shared_ptr<Peer> &peer, const std::string &text) {
        Json message;
        if (!Json::Parse(text, &message) || !message.is_object()) return;
        const std::string type = message.get("type").as_string();
        // Only protocol payloads are accepted. `update` is intentionally
        // opaque to the relay; clients validate and apply CRDT operations.
        if (type != "update" && type != "presence" && type != "sync_request" && type != "sync_response") return;
        message["peer"] = peer->id; message["name"] = peer->name;
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(session_id);
        if (it != sessions_.end()) BroadcastLocked(it->second, message, peer->id);
    }
};

int Listen(uint16_t port) {
    const int fd = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    if (fd < 0) return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&yes), sizeof(yes));
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_ANY); address.sin_port = htons(port);
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || listen(fd, 64) != 0) { CloseFd(fd); return -1; }
    return fd;
}
}  // namespace

int main(int argc, char **argv) {
    uint16_t port = 8787;
    std::string admin_token = std::getenv("MEP_COLLAB_ADMIN_TOKEN") ? std::getenv("MEP_COLLAB_ADMIN_TOKEN") : "";
    std::string public_url = std::getenv("MEP_COLLAB_PUBLIC_URL") ? std::getenv("MEP_COLLAB_PUBLIC_URL") : "";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--admin-token" && i + 1 < argc) admin_token = argv[++i];
        else if (arg == "--public-url" && i + 1 < argc) public_url = argv[++i];
        else if (arg == "--help") { std::cout << "Usage: mep-collabd [--port 8787] [--admin-token TOKEN] [--public-url wss://host]\n"; return 0; }
    }
    if (admin_token.empty()) { std::cerr << "mep-collabd: set MEP_COLLAB_ADMIN_TOKEN or --admin-token\n"; return 2; }
#ifdef _WIN32
    WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 2;
#endif
    const int listener = Listen(port);
    if (listener < 0) { std::cerr << "mep-collabd: unable to listen on port " << port << "\n"; return 1; }
    std::cout << "mep-collabd listening on 0.0.0.0:" << port << "\n";
    Relay relay(admin_token, public_url);
    for (;;) {
        const int client = static_cast<int>(accept(listener, nullptr, nullptr));
        if (client < 0) continue;
        std::thread([&relay, client] { relay.Handle(client); }).detach();
    }
}
