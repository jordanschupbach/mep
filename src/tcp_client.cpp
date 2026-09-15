#include "tcp_client.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
#define MEP_TCP_POSIX 1
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {
constexpr int kConnectTimeoutMs = 300;
constexpr size_t kReadChunkSize = 4096;
}  // namespace

TcpConnection::TcpConnection(const std::string &host, int port) {
#if MEP_TCP_POSIX
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || res == nullptr) {
        connect_failed_ = true;
        finished_ = true;
        return;
    }
    int fd = -1;
    for (struct addrinfo *rp = res; rp != nullptr; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = connect(fd, rp->ai_addr, rp->ai_addrlen);
        if (rc == 0) break;  // connected immediately (rare for TCP, but handle it)
        if (errno != EINPROGRESS) {
            close(fd);
            fd = -1;
            continue;
        }
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv;
        tv.tv_sec = kConnectTimeoutMs / 1000;
        tv.tv_usec = (kConnectTimeoutMs % 1000) * 1000;
        rc = select(fd + 1, nullptr, &wfds, nullptr, &tv);
        if (rc <= 0) {
            close(fd);
            fd = -1;
            continue;
        }
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0) {
            close(fd);
            fd = -1;
            continue;
        }
        break;  // connected
    }
    freeaddrinfo(res);
    if (fd < 0) {
        connect_failed_ = true;
        finished_ = true;
        return;
    }
    // Back to blocking mode -- the reader thread's loop below is a plain
    // blocking recv(), same shape as Job::ReaderLoop's pipe reads.
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    fd_ = fd;
    reader_thread_ = std::thread(&TcpConnection::ReaderLoop, this);
#else
    (void)host;
    (void)port;
    connect_failed_ = true;
    finished_ = true;
#endif
}

TcpConnection::~TcpConnection() {
    Close();
    if (reader_thread_.joinable()) reader_thread_.join();
}

bool TcpConnection::Send(const std::string &data) {
#if MEP_TCP_POSIX
    if (fd_ < 0) return false;
    size_t total = 0;
    while (total < data.size()) {
        ssize_t n = write(fd_, data.data() + total, data.size() - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        total += static_cast<size_t>(n);
    }
    return true;
#else
    (void)data;
    return false;
#endif
}

void TcpConnection::Close() {
#if MEP_TCP_POSIX
    if (fd_ >= 0) {
        shutdown(fd_, SHUT_RDWR);
        close(fd_);
        fd_ = -1;
    }
#endif
}

std::vector<std::string> TcpConnection::DrainRaw() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out(pending_.begin(), pending_.end());
    pending_.clear();
    return out;
}

#if MEP_TCP_POSIX
void TcpConnection::ReaderLoop() {
    char buf[kReadChunkSize];
    for (;;) {
        ssize_t n = read(fd_, buf, sizeof(buf));
        if (n > 0) {
            std::lock_guard<std::mutex> lk(mu_);
            pending_.emplace_back(buf, static_cast<size_t>(n));
        } else if (n == 0) {
            break;  // peer closed
        } else {
            if (errno == EINTR) continue;
            break;  // socket error/closed out from under us
        }
    }
    finished_ = true;
}
#endif

// --- TcpJsonRpcManager --------------------------------------------------

TcpJsonRpcManager &TcpJsonRpcManager::Instance() {
    static TcpJsonRpcManager instance;
    return instance;
}

int TcpJsonRpcManager::Connect(const std::string &host, int port, Callbacks callbacks) {
    Entry entry;
    entry.id = next_id_++;
    entry.conn = std::make_shared<TcpConnection>(host, port);
    entry.callbacks = std::move(callbacks);
    conns_.push_back(std::move(entry));
    return conns_.back().id;
}

bool TcpJsonRpcManager::Send(int id, const std::string &data) {
    TcpConnection *c = Find(id);
    return c ? c->Send(data) : false;
}

void TcpJsonRpcManager::Close(int id) {
    if (TcpConnection *c = Find(id)) c->Close();
}

bool TcpJsonRpcManager::IsRunning(int id) const {
    for (const auto &e : conns_) {
        if (e.id == id) return !e.conn->Finished();
    }
    return false;
}

TcpConnection *TcpJsonRpcManager::Find(int id) {
    for (auto &e : conns_) {
        if (e.id == id) return e.conn.get();
    }
    return nullptr;
}

void TcpJsonRpcManager::PollAll() {
    // Same index-snapshot-then-copy-callbacks discipline as
    // JobManager::PollAll (see its own comment for the full rationale):
    // a callback below could in principle reconnect (re-entering
    // Connect(), which push_back's onto conns_), so this loop must not
    // hold a reference into conns_'s backing array across a callback call.
    size_t n = conns_.size();
    for (size_t i = 0; i < n; i++) {
        std::shared_ptr<TcpConnection> conn = conns_[i].conn;
        auto on_data_raw = conns_[i].callbacks.on_data_raw;
        if (on_data_raw) {
            for (const std::string &chunk : conn->DrainRaw()) on_data_raw(chunk);
        }
        if (conn->Finished() && !conns_[i].exit_reported) {
            conns_[i].exit_reported = true;
            auto on_exit = conns_[i].callbacks.on_exit;
            if (on_exit) on_exit(-1);
        }
    }
    conns_.erase(std::remove_if(conns_.begin(), conns_.end(), [](const Entry &e) { return e.exit_reported; }),
                 conns_.end());
}

void TcpJsonRpcManager::ShutdownAll() {
    for (auto &e : conns_) {
        if (e.conn) e.conn->Close();
    }
    conns_.clear();
}
