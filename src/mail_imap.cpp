// mep's own IMAP connections, pooled per account (mail_imap.h).
#include "mail_imap.h"

#include "mail.h"
#include "mail_crypto.h"  // Wipe

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#if defined(MEP_MAIL_NATIVE_IMAP)
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mail {

#if !defined(MEP_MAIL_NATIVE_IMAP)

bool NativeImapAvailable() { return false; }

bool RunImapNative(const Session &, const std::vector<ImapStep> &, bool, const std::atomic<bool> *, int, ImapResult *) {
    return false;
}

void CloseImapConnections() {}

#else

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kIdleMax = std::chrono::minutes(10);  // a pooled connection idle longer is dropped, not reused
constexpr size_t kPoolMax = 4;                       // idle connections kept per account
constexpr int kConnectTimeoutMs = 20000;             // curl's connect-timeout

enum class Io { Ok, Failed, Cancelled, TimedOut };

struct Deadline {
    Clock::time_point at;
    const std::atomic<bool> *cancel = nullptr;
    Io Check() const {
        if (cancel && cancel->load()) return Io::Cancelled;
        if (Clock::now() > at) return Io::TimedOut;
        return Io::Ok;
    }
};

bool StartsWithI(const std::string &s, const std::string &prefix) {
    return s.size() >= prefix.size() && Lower(s.substr(0, prefix.size())) == Lower(prefix);
}

// The status of a tagged reply line: "OK ...", "NO ...", "BAD ...".
std::string StatusOf(const std::string &line, const std::string &tag) {
    std::string rest = line.substr(tag.size() + 1);
    while (!rest.empty() && (rest.back() == '\r' || rest.back() == '\n')) rest.pop_back();
    return rest;
}

// "* 12 EXISTS" -> (12, "EXISTS"); otherwise kind empty.
void UntaggedNumber(const std::string &resp, uint32_t *n, std::string *kind) {
    kind->clear();
    if (resp.size() < 4 || resp[0] != '*' || resp[1] != ' ') return;
    size_t i = 2;
    uint32_t v = 0;
    bool digits = false;
    while (i < resp.size() && resp[i] >= '0' && resp[i] <= '9') {
        v = v * 10 + static_cast<uint32_t>(resp[i] - '0');
        digits = true;
        i++;
    }
    if (!digits || i >= resp.size() || resp[i] != ' ') return;
    size_t e = i + 1;
    while (e < resp.size() && resp[e] != ' ' && resp[e] != '\r' && resp[e] != '\n') e++;
    *n = v;
    *kind = Lower(resp.substr(i + 1, e - i - 1));
}

class Conn {
public:
    Conn() = default;
    Conn(const Conn &) = delete;
    Conn &operator=(const Conn &) = delete;
    ~Conn() { Close(); }

    std::string selected;  // raw mailbox name the server has selected ("" = none)
    SelectInfo sel;        // ... its counts, kept current from untagged EXISTS / EXPUNGE
    Clock::time_point last_used;
    bool dead = false;

    // TCP, TLS (or STARTTLS), greeting and capabilities. `*tls_failed`:
    // the handshake or the certificate check was what failed.
    bool Open(const Account &a, const Deadline &d, std::string *error, bool *tls_failed) {
        *tls_failed = false;
        if (!Connect(a.imap_host, a.imap_port, d, error)) return false;
        const bool implicit_tls = !a.insecure && a.imap_port != 143;
        if (implicit_tls && !Handshake(a.imap_host, d, error)) {
            *tls_failed = true;
            return false;
        }
        std::string greeting;
        if (ReadResponse(d, &greeting) != Io::Ok) {
            *error = "the connection broke";
            return false;
        }
        if (StartsWithI(greeting, "* BYE")) {
            *error = "the server refused the connection: " + Trim(greeting.substr(5));
            return false;
        }
        if (!a.insecure && !implicit_tls) {
            // Port 143: STARTTLS, required (curl's ssl-reqd).
            std::string tag, received, status;
            if (Command("STARTTLS", d, &tag, &received, &status) != Io::Ok || !StartsWithI(status, "OK")) {
                *error = "the server does not offer STARTTLS";
                return false;
            }
            if (!Handshake(a.imap_host, d, error)) {
                *tls_failed = true;
                return false;
            }
        }
        std::string tag, received, status;
        if (Command("CAPABILITY", d, &tag, &received, &status) != Io::Ok || !StartsWithI(status, "OK")) {
            *error = "the connection broke";
            return false;
        }
        caps_ = " " + Lower(received) + " ";
        return true;
    }

    // `*io` tells a broken connection (Failed) from a refused login (Ok).
    bool Login(const Session &s, const Deadline &d, std::string *error, Io *io) {
        const Account &a = s.account;
        std::string cmd, ir;
        if (a.UsesOAuth()) {
            const bool bearer = !HasCap("auth=xoauth2") && HasCap("auth=oauthbearer");
            if (bearer) {
                cmd = "AUTHENTICATE OAUTHBEARER";
                ir = Base64Encode("n,a=" + a.Login() + ",\x01" "auth=Bearer " + s.secret + "\x01\x01");
            } else {
                cmd = "AUTHENTICATE XOAUTH2";
                ir = Base64Encode("user=" + a.Login() + "\x01" "auth=Bearer " + s.secret + "\x01\x01");
            }
        } else if (HasCap("auth=plain") || HasCap("logindisabled")) {
            cmd = "AUTHENTICATE PLAIN";
            ir = Base64Encode(std::string(1, '\0') + a.Login() + std::string(1, '\0') + s.secret);
        } else {
            cmd = "LOGIN " + ImapQuote(a.Login()) + " " + ImapQuote(s.secret);
        }
        const bool sasl = !ir.empty();
        const bool sasl_ir = sasl && HasCap("sasl-ir");
        const std::string tag = NextTag();
        std::string line = tag + " " + cmd + (sasl_ir ? " " + ir : std::string()) + "\r\n";
        *io = Write(line, d);
        Wipe(&line);
        bool sent_ir = sasl_ir;
        std::string status;
        while (*io == Io::Ok) {
            std::string resp;
            *io = ReadResponse(d, &resp);
            if (*io != Io::Ok) break;
            if (resp[0] == '+') {
                // The challenge for the initial response, or (after it) the
                // server's error details: answered with an empty line.
                std::string reply = (sasl && !sent_ir ? ir : std::string()) + "\r\n";
                sent_ir = true;
                *io = Write(reply, d);
                Wipe(&reply);
                continue;
            }
            if (resp.rfind(tag + " ", 0) == 0) {
                status = StatusOf(resp, tag);
                break;
            }
        }
        Wipe(&ir);
        if (*io != Io::Ok) {
            *error = *io == Io::Cancelled ? "cancelled" : *io == Io::TimedOut ? "the mail server timed out" : "the connection broke";
            return false;
        }
        if (StartsWithI(status, "OK")) return true;
        *error = DescribeCurlFailure(67, "", a);
        if (status.find("Application-specific password required") != std::string::npos) {
            // Gmail with 2-Step Verification: the account password is never accepted over IMAP.
            *error = "login refused for " + a.Login() +
                     " -- Google wants an app password, not your account password: make one at "
                     "https://myaccount.google.com/apppasswords (or sign in with the browser: :MailSetup google)";
        } else if (!a.UsesOAuth() && !status.empty()) {
            *error += ": " + status;
        }
        return false;
    }

    // Sends `cmd` under a fresh tag and reads everything up to its tagged
    // reply into `*received`; `*status` is that reply ("OK ...").
    Io Command(const std::string &cmd, const Deadline &d, std::string *tag, std::string *received, std::string *status) {
        *tag = NextTag();
        Io io = Write(*tag + " " + cmd + "\r\n", d);
        while (io == Io::Ok) {
            std::string resp;
            io = ReadResponse(d, &resp);
            if (io != Io::Ok) break;
            if (resp[0] == '+') {
                io = Write("\r\n", d);  // nothing here sends literals; do not hang on one
                continue;
            }
            received->append(resp);
            if (resp.rfind(*tag + " ", 0) == 0) {
                *status = StatusOf(resp, *tag);
                return Io::Ok;
            }
            Track(resp);
        }
        dead = true;  // mid-command: the stream is out of step now
        return io;
    }

    // Takes in what the server said while the connection sat in the pool
    // (new-mail counts, a BYE); false when it is gone.
    bool Alive() {
        if (dead || fd_ < 0) return false;
        const Deadline now{Clock::now() + std::chrono::milliseconds(200), nullptr};
        while (true) {
            pollfd p{fd_, POLLIN, 0};
            const bool pending = (ssl_ && SSL_pending(ssl_) > 0) || poll(&p, 1, 0) > 0;
            if (!pending) break;
            if (Fill(now, /*once=*/true) != Io::Ok) return false;
            std::string resp;
            while (TakeResponse(&resp)) {
                if (StartsWithI(resp, "* BYE")) dead = true;
                Track(resp);
            }
            if (dead) return false;
        }
        return true;
    }

    void Close() {
        if (ssl_) {
            SSL_free(ssl_);
            ssl_ = nullptr;
        }
        if (ctx_) {
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
        }
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
        dead = true;
    }

    // Best effort, without waiting for the reply.
    void Logout() {
        if (dead || fd_ < 0) return;
        Write(NextTag() + " LOGOUT\r\n", Deadline{Clock::now() + std::chrono::milliseconds(200), nullptr});
    }

private:
    int fd_ = -1;
    SSL_CTX *ctx_ = nullptr;
    SSL *ssl_ = nullptr;
    std::string in_;  // received, not yet consumed
    int next_tag_ = 1;
    std::string caps_;

    bool HasCap(const std::string &cap) const { return caps_.find(" " + cap + " ") != std::string::npos; }

    std::string NextTag() {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "M%03d", next_tag_++);
        return buf;
    }

    void Track(const std::string &resp) {
        if (selected.empty()) return;
        uint32_t n = 0;
        std::string kind;
        UntaggedNumber(resp, &n, &kind);
        if (kind == "exists") sel.exists = n;
        else if (kind == "expunge" && sel.exists > 0) sel.exists--;
    }

    static bool WaitFd(int fd, short events, int ms) {
        pollfd p{fd, events, 0};
        return poll(&p, 1, ms) > 0;
    }

    bool Connect(const std::string &host, int port, const Deadline &d, std::string *error) {
        addrinfo hints{};
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_family = AF_UNSPEC;
        addrinfo *addrs = nullptr;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &addrs) != 0 || !addrs) {
            *error = "cannot resolve " + host + " (offline?)";
            return false;
        }
        const auto connect_by = std::min(d.at, Clock::now() + std::chrono::milliseconds(kConnectTimeoutMs));
        for (addrinfo *it = addrs; it && fd_ < 0; it = it->ai_next) {
            const int fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
            if (fd < 0) continue;
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
            fcntl(fd, F_SETFD, FD_CLOEXEC);
#if defined(__APPLE__)
            int one = 1;
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
            bool ok = connect(fd, it->ai_addr, it->ai_addrlen) == 0;
            if (!ok && errno == EINPROGRESS) {
                while (!ok && Clock::now() < connect_by && d.Check() == Io::Ok) {
                    if (!WaitFd(fd, POLLOUT, 100)) continue;
                    int err = 0;
                    socklen_t len = sizeof(err);
                    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                    if (err != 0) break;
                    ok = true;
                }
            }
            if (ok) fd_ = fd;
            else close(fd);
        }
        freeaddrinfo(addrs);
        if (fd_ < 0) {
            *error = d.Check() == Io::Cancelled ? "cancelled" : "cannot connect to the mail server (offline, or blocked port?)";
            return false;
        }
        return true;
    }

    bool Handshake(const std::string &host, const Deadline &d, std::string *error) {
        ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ctx_ || SSL_CTX_set_default_verify_paths(ctx_) != 1) {
            *error = "TLS handshake failed (no trust store)";
            return false;
        }
        SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
        SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
        ssl_ = SSL_new(ctx_);
        // SSL_set_tlsext_host_name is a macro with an old-style cast inside
        // <openssl/ssl.h> (as in collab_websocket.cpp).
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
        const bool named = ssl_ && SSL_set_tlsext_host_name(ssl_, host.c_str()) == 1 && SSL_set1_host(ssl_, host.c_str()) == 1;
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
        if (!named || SSL_set_fd(ssl_, fd_) != 1) {
            *error = "TLS handshake failed";
            return false;
        }
        while (true) {
            if (d.Check() != Io::Ok) {
                *error = d.Check() == Io::Cancelled ? "cancelled" : "the mail server timed out";
                return false;
            }
            ERR_clear_error();
            const int rc = SSL_connect(ssl_);
            if (rc == 1) break;
            const int e = SSL_get_error(ssl_, rc);
            if (e == SSL_ERROR_WANT_READ) WaitFd(fd_, POLLIN, 100);
            else if (e == SSL_ERROR_WANT_WRITE) WaitFd(fd_, POLLOUT, 100);
            else {
                *error = SSL_get_verify_result(ssl_) != X509_V_OK ? "the server's certificate could not be verified"
                                                                  : "TLS handshake failed";
                return false;
            }
        }
        if (SSL_get_verify_result(ssl_) != X509_V_OK) {
            *error = "the server's certificate could not be verified";
            return false;
        }
        return true;
    }

    Io Write(const std::string &data, const Deadline &d) {
        size_t off = 0;
        while (off < data.size()) {
            const Io c = d.Check();
            if (c != Io::Ok) return c;
            const size_t left = data.size() - off;
            if (ssl_) {
                ERR_clear_error();
                const int n = SSL_write(ssl_, data.data() + off, static_cast<int>(std::min<size_t>(left, 1u << 20)));
                if (n > 0) {
                    off += static_cast<size_t>(n);
                    continue;
                }
                const int e = SSL_get_error(ssl_, n);
                if (e == SSL_ERROR_WANT_WRITE) WaitFd(fd_, POLLOUT, 100);
                else if (e == SSL_ERROR_WANT_READ) WaitFd(fd_, POLLIN, 100);
                else return Io::Failed;
            } else {
#if defined(MSG_NOSIGNAL)
                const ssize_t n = send(fd_, data.data() + off, left, MSG_NOSIGNAL);
#else
                const ssize_t n = send(fd_, data.data() + off, left, 0);
#endif
                if (n > 0) {
                    off += static_cast<size_t>(n);
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) WaitFd(fd_, POLLOUT, 100);
                else return Io::Failed;
            }
        }
        return Io::Ok;
    }

    // Reads at least one chunk into in_ (waiting for it, unless `once`
    // and nothing is there).
    Io Fill(const Deadline &d, bool once = false) {
        char buf[65536];
        while (true) {
            const Io c = d.Check();
            if (c != Io::Ok) return c;
            if (ssl_) {
                ERR_clear_error();
                const int n = SSL_read(ssl_, buf, static_cast<int>(sizeof(buf)));
                if (n > 0) {
                    in_.append(buf, static_cast<size_t>(n));
                    return Io::Ok;
                }
                const int e = SSL_get_error(ssl_, n);
                if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
                    if (once) return Io::Ok;
                    WaitFd(fd_, static_cast<short>(e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT), 100);
                    continue;
                }
                return Io::Failed;
            }
            const ssize_t n = recv(fd_, buf, sizeof(buf), 0);
            if (n > 0) {
                in_.append(buf, static_cast<size_t>(n));
                return Io::Ok;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                if (once) return Io::Ok;
                WaitFd(fd_, POLLIN, 100);
                continue;
            }
            return Io::Failed;
        }
    }

    // One whole response -- its first line through any {n} literals and
    // the lines after them -- taken from in_ when it is all there.
    bool TakeResponse(std::string *out) {
        size_t pos = 0;
        while (true) {
            const size_t nl = in_.find('\n', pos);
            if (nl == std::string::npos) return false;
            size_t end = nl;
            if (end > pos && in_[end - 1] == '\r') end--;
            size_t literal = 0;
            bool has_literal = false;
            if (end > pos && in_[end - 1] == '}') {
                const size_t open = in_.rfind('{', end - 1);
                if (open != std::string::npos && open >= pos) {
                    std::string num = in_.substr(open + 1, end - 1 - open - 1);
                    if (!num.empty() && num.back() == '+') num.pop_back();
                    bool digits = !num.empty() && num.size() < 12;
                    for (char c : num) digits = digits && c >= '0' && c <= '9';
                    if (digits) {
                        has_literal = true;
                        literal = static_cast<size_t>(std::strtoull(num.c_str(), nullptr, 10));
                    }
                }
            }
            pos = nl + 1;
            if (!has_literal) {
                out->assign(in_, 0, pos);
                in_.erase(0, pos);
                return true;
            }
            if (in_.size() < pos + literal) return false;
            pos += literal;
        }
    }

    Io ReadResponse(const Deadline &d, std::string *out) {
        while (!TakeResponse(out)) {
            const Io io = Fill(d);
            if (io != Io::Ok) {
                dead = true;
                return io;
            }
        }
        return Io::Ok;
    }
};

std::mutex g_pool_mutex;
// Leaked on purpose: worker threads may still hand connections back while
// the process exits.
auto &g_pool = *new std::map<std::string, std::vector<std::unique_ptr<Conn>>>();

std::string PoolKey(const Account &a) {
    return Lower(a.Login()) + "|" + Lower(a.imap_host) + ":" + std::to_string(a.imap_port) + (a.insecure ? "|plain" : "");
}

// An idle pooled connection for `key`, preferring one that has `mailbox`
// selected already; null when there is none worth reusing.
std::unique_ptr<Conn> Acquire(const std::string &key, const std::string &mailbox) {
    std::vector<std::unique_ptr<Conn>> stale;
    std::unique_ptr<Conn> got;
    {
        std::lock_guard<std::mutex> lock(g_pool_mutex);
        auto it = g_pool.find(key);
        if (it == g_pool.end()) return nullptr;
        auto &idle = it->second;
        const auto now = Clock::now();
        for (size_t i = 0; i < idle.size();) {
            if (now - idle[i]->last_used > kIdleMax) {
                stale.push_back(std::move(idle[i]));
                idle.erase(idle.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                i++;
            }
        }
        size_t pick = idle.size();
        for (size_t i = 0; i < idle.size(); i++) {
            if (idle[i]->selected == mailbox) pick = i;
        }
        if (pick == idle.size() && !idle.empty()) pick = idle.size() - 1;  // the most recently used
        if (pick < idle.size()) {
            got = std::move(idle[pick]);
            idle.erase(idle.begin() + static_cast<std::ptrdiff_t>(pick));
        }
    }
    for (auto &c : stale) c->Logout();
    return got;
}

void Release(const std::string &key, std::unique_ptr<Conn> c) {
    if (!c || c->dead) return;
    c->last_used = Clock::now();
    std::unique_ptr<Conn> evicted;
    {
        std::lock_guard<std::mutex> lock(g_pool_mutex);
        auto &idle = g_pool[key];
        idle.push_back(std::move(c));
        if (idle.size() > kPoolMax) {
            evicted = std::move(idle.front());
            idle.erase(idle.begin());
        }
    }
    if (evicted) evicted->Logout();
}

// ";UID=42" -> 42; 0 for anything else.
uint32_t UidOfSuffix(const std::string &suffix) {
    if (!StartsWithI(suffix, ";UID=")) return 0;
    return static_cast<uint32_t>(std::strtoul(suffix.c_str() + 5, nullptr, 10));
}

enum class Outcome { Done, Broken, Unavailable };

// The steps over `c`. Broken: the connection failed before the first
// command was answered (a reused one the server had dropped) -- safe to
// run them again on a fresh one.
Outcome RunSteps(Conn &c, const Session &s, const std::vector<ImapStep> &steps, bool traced, const Deadline &d,
                 ImapResult *r) {
    std::string received;
    bool answered = false;
    bool touched_mailbox = false;
    auto fail_io = [&](Io io) {
        if (io == Io::Failed && !answered) return Outcome::Broken;
        r->error = io == Io::Cancelled ? "cancelled" : io == Io::TimedOut ? "the mail server timed out" : "the connection broke";
        return Outcome::Done;
    };
    for (const ImapStep &st : steps) {
        std::string tag, status;
        if (!st.mailbox.empty() && st.mailbox != c.selected) {
            touched_mailbox = true;
            c.selected.clear();
            std::string sel_received;
            const Io io = c.Command("SELECT " + ImapQuote(st.mailbox), d, &tag, &sel_received, &status);
            if (io != Io::Ok) return fail_io(io);
            answered = true;
            r->trace.sent.push_back(tag + " SELECT " + ImapQuote(st.mailbox));
            received += sel_received;
            if (!StartsWithI(status, "OK")) {
                r->error = DescribeCurlFailure(78, "", s.account) + ": " + status;
                r->trace.received = received;
                if (traced) r->responses = ParseImapStream(received);
                return Outcome::Done;
            }
            c.selected = st.mailbox;
            c.sel = ReadSelect(ParseImapStream(sel_received));
        } else if (!st.mailbox.empty()) {
            touched_mailbox = true;
        }
        std::string cmd = st.request;
        uint32_t fetch_uid = 0;
        if (cmd.empty()) {
            fetch_uid = UidOfSuffix(st.url_suffix);
            if (fetch_uid == 0) return Outcome::Unavailable;  // a URL form only curl knows
            cmd = "UID FETCH " + std::to_string(fetch_uid) + " BODY[]";
        }
        std::string out;
        const Io io = c.Command(cmd, d, &tag, &out, &status);
        if (io != Io::Ok) return fail_io(io);
        answered = true;
        r->trace.sent.push_back(tag + " " + cmd);
        received += out;
        if (!StartsWithI(status, "OK")) {
            r->error = DescribeCurlFailure(fetch_uid ? 78 : 21, "", s.account);
            if (!fetch_uid) r->error += ": " + status;
            break;
        }
        if (fetch_uid) {
            for (auto &body : ReadFetchBodies(ParseImapStream(out))) {
                if (body.first == fetch_uid) r->body += body.second;
            }
            if (r->body.empty()) {
                r->error = DescribeCurlFailure(78, "", s.account);
                break;
            }
        }
    }
    // A reused selection sent no SELECT this time: its counts, as curl's
    // SELECT would have reported them, close the stream (ReadSelect keeps
    // the last EXISTS it sees).
    if (touched_mailbox && !c.selected.empty()) {
        received += "* " + std::to_string(c.sel.exists) + " EXISTS\r\n";
        if (c.sel.uidvalidity) received += "* OK [UIDVALIDITY " + std::to_string(c.sel.uidvalidity) + "]\r\n";
        if (c.sel.uidnext) received += "* OK [UIDNEXT " + std::to_string(c.sel.uidnext) + "]\r\n";
    }
    r->trace.received = std::move(received);
    if (traced) r->responses = ParseImapStream(r->trace.received);
    r->ok = r->error.empty();
    return Outcome::Done;
}

}  // namespace

bool NativeImapAvailable() {
    static const bool off = [] {
        const char *v = std::getenv("MEP_MAIL_CURL");
        return v && *v && std::string(v) != "0";
    }();
    return !off;
}

bool RunImapNative(const Session &s, const std::vector<ImapStep> &steps, bool traced, const std::atomic<bool> *cancel,
                   int timeout_ms, ImapResult *r) {
    if (!NativeImapAvailable()) return false;
    const Deadline d{Clock::now() + std::chrono::milliseconds(timeout_ms), cancel};
    const std::string key = PoolKey(s.account);
    const std::string first_mailbox = steps.front().mailbox;
    const auto started = Clock::now();
    std::string how;
    for (int attempt = 0; attempt < 2; attempt++) {
        std::unique_ptr<Conn> c = attempt == 0 ? Acquire(key, first_mailbox) : nullptr;
        if (c && !c->Alive()) c.reset();
        const bool reused = c != nullptr;
        if (!c) {
            c = std::make_unique<Conn>();
            std::string error;
            bool tls_failed = false;
            const auto t0 = Clock::now();
            if (!c->Open(s.account, d, &error, &tls_failed)) {
                if (tls_failed) {
                    MailLog("imap: own connection failed (" + error + "), using curl");
                    return false;
                }
                *r = ImapResult{};
                r->error = error;
                return true;
            }
            Io io = Io::Ok;
            if (!c->Login(s, d, &error, &io)) {
                *r = ImapResult{};
                r->error = error;
                return true;
            }
            how = "new connection (login " +
                  std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count()) + " ms)";
        } else {
            how = c->selected == first_mailbox && !first_mailbox.empty() ? "reused connection, already selected"
                                                                         : "reused connection";
        }
        *r = ImapResult{};
        const Outcome o = RunSteps(*c, s, steps, traced, d, r);
        if (o == Outcome::Unavailable) {
            Release(key, std::move(c));
            return false;
        }
        if (o == Outcome::Broken && reused) continue;  // the server had dropped it: once more on a fresh one
        if (o == Outcome::Broken) r->error = "the connection broke";
        Release(key, std::move(c));
        break;
    }
    // Timings for :MailLog -- each command's verb only (never a search term or a secret).
    std::string what;
    for (const ImapStep &st : steps) {
        std::string verb = st.request.empty() ? std::string("FETCH-URL") : st.request.substr(0, st.request.find(' '));
        if ((verb == "UID" || verb == "uid") && st.request.size() > 4) {
            verb = "UID " + st.request.substr(4, st.request.find(' ', 4) - 4);
        }
        what += (what.empty() ? "" : " + ") + verb;
    }
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    const size_t bytes = traced ? r->trace.received.size() : r->body.size();
    const bool throttled = r->trace.received.find("[THROTTLED]") != std::string::npos;
    MailLog("imap " + what + ": " + std::to_string(ms) + " ms, " + std::to_string(bytes / 1024) + " KB, " + how +
            (throttled ? ", server throttling" : "") + (r->error.empty() ? "" : ", failed: " + r->error));
    return true;
}

void CloseImapConnections() {
    std::map<std::string, std::vector<std::unique_ptr<Conn>>> all;
    {
        std::lock_guard<std::mutex> lock(g_pool_mutex);
        all.swap(g_pool);
    }
    for (auto &kv : all) {
        for (auto &c : kv.second) c->Logout();
    }
}

#endif

}  // namespace mail
