#include "mail_net.h"

#include "json.h"
#include "mail_crypto.h"
#include "mail_imap.h"
#if defined(__APPLE__)
#include "mail_keychain_macos.h"
#endif

// platform_compat.h first: on Windows it supplies the POSIX spellings
// (mkdir(path, mode), ...) that persist.h's inline helpers use.
#include "platform_compat.h"
#include "persist.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include <direct.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace mail {

// --- Processes ----------------------------------------------------------

#if defined(__EMSCRIPTEN__)

// No processes in a browser sandbox: every request reports spawned == false.
ProcResult RunProcess(const std::vector<std::string> &, const std::string &, int, const std::atomic<bool> *) {
    return ProcResult{};
}

#elif defined(_WIN32)

namespace {

std::wstring Widen(const std::string &s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// One argument quoted the way CommandLineToArgvW / the MSVC runtime split them.
std::wstring QuoteArg(const std::wstring &a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            backslashes++;
            continue;
        }
        if (c == L'"') out.append(backslashes * 2 + 1, L'\\');
        else out.append(backslashes, L'\\');
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

void ReadAll(HANDLE h, std::string *dst) {
    char buf[8192];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) dst->append(buf, n);
}

}  // namespace

ProcResult RunProcess(const std::vector<std::string> &argv, const std::string &stdin_data, int timeout_ms,
                      const std::atomic<bool> *cancel) {
    ProcResult r;
    if (argv.empty()) return r;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE in_r = nullptr, in_w = nullptr, out_r = nullptr, out_w = nullptr, err_r = nullptr, err_w = nullptr;
    if (!CreatePipe(&in_r, &in_w, &sa, 0) || !CreatePipe(&out_r, &out_w, &sa, 0) || !CreatePipe(&err_r, &err_w, &sa, 0)) {
        return r;
    }
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
    std::wstring cmd;
    for (const std::string &a : argv) {
        if (!cmd.empty()) cmd.push_back(L' ');
        cmd += QuoteArg(Widen(a));
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = err_w;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdbuf(cmd.begin(), cmd.end());
    cmdbuf.push_back(L'\0');
    const BOOL ok = CreateProcessW(nullptr, cmdbuf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                   &si, &pi);
    CloseHandle(in_r);
    CloseHandle(out_w);
    CloseHandle(err_w);
    if (!ok) {
        CloseHandle(in_w);
        CloseHandle(out_r);
        CloseHandle(err_r);
        return r;
    }
    r.spawned = true;
    std::thread out_thread(ReadAll, out_r, &r.out);
    std::thread err_thread(ReadAll, err_r, &r.err);
    DWORD written = 0;
    size_t off = 0;
    while (off < stdin_data.size() &&
           WriteFile(in_w, stdin_data.data() + off, static_cast<DWORD>(stdin_data.size() - off), &written, nullptr) &&
           written > 0) {
        off += written;
    }
    CloseHandle(in_w);
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) break;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        if (cancel && cancel->load()) r.cancelled = true;
        if (ms > timeout_ms) r.timed_out = true;
        if (r.cancelled || r.timed_out) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 2000);
            break;
        }
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    r.exit_code = static_cast<int>(code);
    out_thread.join();
    err_thread.join();
    CloseHandle(out_r);
    CloseHandle(err_r);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return r;
}

#else

ProcResult RunProcess(const std::vector<std::string> &argv, const std::string &stdin_data, int timeout_ms,
                      const std::atomic<bool> *cancel) {
    ProcResult r;
    if (argv.empty()) return r;
    // stdin is a socket rather than a pipe so a child that dies before
    // reading it costs an EPIPE, not a SIGPIPE through the whole editor.
    int in_sv[2] = {-1, -1}, out_p[2] = {-1, -1}, err_p[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, in_sv) != 0) return r;
    if (pipe(out_p) != 0 || pipe(err_p) != 0) {
        for (int fd : {in_sv[0], in_sv[1], out_p[0], out_p[1], err_p[0], err_p[1]}) {
            if (fd >= 0) close(fd);
        }
        return r;
    }
#if defined(__APPLE__)
    int one = 1;
    setsockopt(in_sv[0], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in_sv[1], 0);
    posix_spawn_file_actions_adddup2(&fa, out_p[1], 1);
    posix_spawn_file_actions_adddup2(&fa, err_p[1], 2);
    for (int fd : {in_sv[0], in_sv[1], out_p[0], out_p[1], err_p[0], err_p[1]}) {
        posix_spawn_file_actions_addclose(&fa, fd);
    }
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    short flags = 0;
#if defined(__APPLE__)
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;  // nothing else of mep's leaks into the child
#endif
    // The child starts with default signal dispositions.
    sigset_t def;
    sigemptyset(&def);
    sigaddset(&def, SIGPIPE);
    posix_spawnattr_setsigdefault(&attr, &def);
    flags |= POSIX_SPAWN_SETSIGDEF;
    posix_spawnattr_setflags(&attr, flags);
    std::vector<char *> args;
    args.reserve(argv.size() + 1);
    for (const std::string &a : argv) args.push_back(const_cast<char *>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, args[0], &fa, &attr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(in_sv[1]);
    close(out_p[1]);
    close(err_p[1]);
    if (rc != 0) {
        close(in_sv[0]);
        close(out_p[0]);
        close(err_p[0]);
        return r;
    }
    r.spawned = true;
    int in_fd = in_sv[0];
    fcntl(in_fd, F_SETFL, fcntl(in_fd, F_GETFL) | O_NONBLOCK);
    size_t in_off = 0;
    if (stdin_data.empty()) {
        close(in_fd);
        in_fd = -1;
    }
    int out_fd = out_p[0], err_fd = err_p[0];
    const auto start = std::chrono::steady_clock::now();
    bool killed = false;
    while (out_fd >= 0 || err_fd >= 0) {
        pollfd pfd[3];
        nfds_t n = 0;
        int idx_in = -1, idx_out = -1, idx_err = -1;
        if (in_fd >= 0) {
            idx_in = static_cast<int>(n);
            pfd[n++] = {in_fd, POLLOUT, 0};
        }
        if (out_fd >= 0) {
            idx_out = static_cast<int>(n);
            pfd[n++] = {out_fd, POLLIN, 0};
        }
        if (err_fd >= 0) {
            idx_err = static_cast<int>(n);
            pfd[n++] = {err_fd, POLLIN, 0};
        }
        const int pr = poll(pfd, n, 100);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        if (!killed && ((cancel && cancel->load()) || ms > timeout_ms)) {
            if (cancel && cancel->load()) r.cancelled = true;
            else r.timed_out = true;
            kill(pid, SIGKILL);
            killed = true;
        }
        if (pr <= 0) continue;
        if (idx_in >= 0 && (pfd[idx_in].revents & (POLLOUT | POLLERR | POLLHUP))) {
#if defined(__APPLE__)
            const ssize_t w = write(in_fd, stdin_data.data() + in_off, stdin_data.size() - in_off);
#else
            const ssize_t w = send(in_fd, stdin_data.data() + in_off, stdin_data.size() - in_off, MSG_NOSIGNAL);
#endif
            if (w > 0) in_off += static_cast<size_t>(w);
            if (w < 0 || in_off >= stdin_data.size()) {
                shutdown(in_fd, SHUT_WR);
                close(in_fd);
                in_fd = -1;
            }
        }
        char buf[16384];
        for (int which = 0; which < 2; which++) {
            const int idx = which == 0 ? idx_out : idx_err;
            int &fd = which == 0 ? out_fd : err_fd;
            if (idx < 0 || fd < 0 || !(pfd[idx].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            const ssize_t got = read(fd, buf, sizeof(buf));
            if (got > 0) {
                (which == 0 ? r.out : r.err).append(buf, static_cast<size_t>(got));
            } else {
                close(fd);
                fd = -1;
            }
        }
    }
    if (in_fd >= 0) close(in_fd);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (WIFEXITED(status)) r.exit_code = WEXITSTATUS(status);
    else r.exit_code = -1;
    return r;
}

#endif

// --- Paths --------------------------------------------------------------

namespace {

void MakeDir(const std::string &path) {
#if defined(_WIN32)
    _mkdir(path.c_str());
#else
    mkdir(path.c_str(), 0700);
#endif
}

std::string NullDevice() {
#if defined(_WIN32)
    return "NUL";
#else
    return "/dev/null";
#endif
}

std::string CurlProgram() {
#if defined(_WIN32)
    return "curl.exe";
#else
    return "curl";
#endif
}

std::string RandomName() {
    std::random_device rd;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%08x%08x", rd(), rd());
    return buf;
}

// A private temp file holding `data`; removed by the caller.
std::string WriteTempFile(const std::string &data, std::string *error) {
    const std::string dir = MailDataDir();
    if (dir.empty()) {
        *error = "no data directory for mail";
        return "";
    }
    MakeDir(dir + "/tmp");
    const std::string path = dir + "/tmp/" + RandomName() + ".eml";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            *error = "cannot write " + path;
            return "";
        }
        out << data;
    }
#if !defined(_WIN32)
    chmod(path.c_str(), 0600);
#endif
    return path;
}

}  // namespace

std::string MailDataDir() {
    std::string base = MepDataDir();
#if defined(_WIN32)
    if (base.empty()) {
        const char *appdata = std::getenv("APPDATA");
        if (appdata && *appdata) {
            base = std::string(appdata) + "\\mep";
            MakeDir(base);
        }
    }
#endif
    if (base.empty()) return "";
    const std::string dir = base + "/mail";
    MakeDir(dir);
#if !defined(_WIN32)
    // Owner-only, even if something created it looser: drafts, the vault
    // and (on systems without a keyring) secrets live under it.
    chmod(dir.c_str(), 0700);
#endif
    return dir;
}

std::string AccountsPath() {
    const std::string dir = MailDataDir();
    return dir.empty() ? "" : dir + "/accounts.json";
}

bool CurlAvailable() { return mep::compat::ProgramOnPath(CurlProgram().c_str()); }

// --- Secrets ------------------------------------------------------------

namespace {

const char kService[] = "mep-mail";

std::string SecretFilePath(const Account &a) {
    const std::string dir = MailDataDir();
    if (dir.empty()) return "";
    MakeDir(dir + "/secrets");
    return dir + "/secrets/" + SafeFilename(a.id.empty() ? a.email : a.id) + ".secret";
}

std::string FirstLine(const std::string &s) {
    const size_t nl = s.find_first_of("\r\n");
    return nl == std::string::npos ? s : s.substr(0, nl);
}

#if defined(_WIN32)
std::string DpapiProtect(const std::string &plain, bool protect) {
    DATA_BLOB in{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE *>(const_cast<char *>(plain.data()))};
    DATA_BLOB out{};
    const BOOL ok = protect ? CryptProtectData(&in, L"mep mail", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)
                            : CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out);
    if (!ok) return "";
    std::string result(reinterpret_cast<char *>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return result;
}
#endif

std::string ReadSecretFile(const Account &a, bool encrypted) {
    std::ifstream in(SecretFilePath(a), std::ios::binary);
    if (!in) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
#if defined(_WIN32)
    if (encrypted) return DpapiProtect(ss.str(), false);
#else
    (void)encrypted;
#endif
    return FirstLine(ss.str());
}

bool WriteSecretFile(const Account &a, const std::string &secret, bool encrypted, std::string *error) {
    const std::string path = SecretFilePath(a);
    if (path.empty()) {
        *error = "no data directory for mail";
        return false;
    }
    std::string data = secret;
#if defined(_WIN32)
    if (encrypted) {
        data = DpapiProtect(secret, true);
        if (data.empty()) {
            *error = "Windows could not encrypt the password (DPAPI)";
            return false;
        }
    }
#else
    (void)encrypted;
#endif
    std::remove(path.c_str());
#if !defined(_WIN32)
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        *error = "cannot write " + path;
        return false;
    }
    const ssize_t w = write(fd, data.data(), data.size());
    close(fd);
    if (w != static_cast<ssize_t>(data.size())) {
        *error = "cannot write " + path;
        return false;
    }
#else
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << data;
    if (!out) {
        *error = "cannot write " + path;
        return false;
    }
#endif
    return true;
}

std::vector<std::string> ShellArgv(const std::string &command) {
#if defined(_WIN32)
    return {"cmd.exe", "/c", command};
#else
    return {"/bin/sh", "-c", command};
#endif
}

}  // namespace

// --- OAuth clients and the mail log ------------------------------------------

namespace {

std::string HomeDirectory() {
    const char *home = std::getenv("HOME");
#if defined(_WIN32)
    if (!home || !*home) home = std::getenv("USERPROFILE");
#endif
    return home && *home ? std::string(home) : std::string();
}

std::string OAuthClientsPath() {
    const std::string dir = MailDataDir();
    return dir.empty() ? "" : dir + "/oauth_clients.json";
}

bool WritePrivateFile(const std::string &path, const std::string &data) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << data;
        if (!out) return false;
    }
#if !defined(_WIN32)
    chmod(tmp.c_str(), 0600);
#endif
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace

namespace {
std::mutex g_counts_mutex;

Json ReadCounts(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return Json::Object();
    std::ostringstream ss;
    ss << in.rdbuf();
    Json root;
    if (!Json::Parse(ss.str(), &root) || !root.is_object()) return Json::Object();
    return root;
}
}  // namespace

std::vector<std::pair<std::string, std::string>> DownloadFiles(const std::vector<std::pair<std::string, std::string>> &files,
                                                               const std::atomic<bool> *cancel) {
    std::vector<std::pair<std::string, std::string>> got;
    if (files.empty()) return got;
    // URLs come from a message: curl may speak only http(s), and the
    // config is built with the same quoting every other secret uses.
    std::string config = "proto = \"=http,https\"\nproto-redir = \"=http,https\"\nlocation\nmax-redirs = 5\n"
                         "max-filesize = 10000000\nconnect-timeout = 10\nmax-time = 30\nfail\n";
    for (const auto &f : files) {
        config += "url = " + CurlConfigQuote(f.first) + "\n";
        config += "output = " + CurlConfigQuote(f.second) + "\n";
    }
    RunProcess({CurlProgram(), "-sS", "--parallel", "--parallel-max", "8", "-K", "-"}, config, 120000, cancel);
    for (const auto &f : files) {
        std::error_code ec;
        if (std::filesystem::file_size(f.second, ec) > 0 && !ec) got.push_back(f);
    }
    return got;
}

uint32_t LoadMailboxCount(const std::string &key) {
    const std::string dir = MailDataDir();
    if (dir.empty()) return 0;
    std::lock_guard<std::mutex> lock(g_counts_mutex);
    const double n = ReadCounts(dir + "/counts.json").get(key).as_double(0.0);
    return n > 0 ? static_cast<uint32_t>(n) : 0;
}

void SaveMailboxCount(const std::string &key, uint32_t count) {
    const std::string dir = MailDataDir();
    if (dir.empty()) return;
    std::lock_guard<std::mutex> lock(g_counts_mutex);
    const std::string path = dir + "/counts.json";
    Json root = ReadCounts(path);
    if (root.get(key).as_double(-1.0) == static_cast<double>(count)) return;
    root[key] = Json(static_cast<double>(count));
    WritePrivateFile(path, root.dump() + "\n");
}

namespace {

std::mutex g_snapshot_mutex;

// MailDataDir()/cache/<readable key>-<hash>.json (the hash keeps names that
// SafeFilename folds together apart); "" without a data dir.
std::string SnapshotPath(const std::string &kind, const std::string &key) {
    const std::string dir = MailDataDir();
    if (dir.empty()) return "";
    MakeDir(dir + "/cache");
    uint64_t h = 1469598103934665603ull;  // FNV-1a: stable across runs
    for (char c : key) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(h));
    return dir + "/cache/" + kind + "-" + SafeFilename(Lower(key)) + "-" + hex + ".json";
}

bool ReadSnapshot(const std::string &path, Json *root) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    return Json::Parse(ss.str(), root) && root->is_object() && root->get("v").as_int(0) == 1;
}

Json StringsJson(const std::vector<std::string> &v) {
    Json a = Json::Array();
    for (const std::string &s : v) a.push_back(s);
    return a;
}

std::vector<std::string> JsonStrings(const Json &a) {
    std::vector<std::string> v;
    for (const Json &s : a.items()) v.push_back(s.as_string());
    return v;
}

Json AddressesJson(const std::vector<Address> &addrs) {
    Json a = Json::Array();
    for (const Address &ad : addrs) {
        Json pair = Json::Array();
        pair.push_back(ad.name);
        pair.push_back(ad.email);
        a.push_back(std::move(pair));
    }
    return a;
}

std::vector<Address> JsonAddresses(const Json &a) {
    std::vector<Address> v;
    for (const Json &pair : a.items()) {
        if (pair.items().size() != 2) continue;
        v.push_back({pair.items()[0].as_string(), pair.items()[1].as_string()});
    }
    return v;
}

}  // namespace

bool LoadListSnapshot(const std::string &key, std::vector<MessageSummary> *msgs, uint32_t *exists) {
    const std::string path = SnapshotPath("list", key);
    if (path.empty()) return false;
    Json root;
    {
        std::lock_guard<std::mutex> lock(g_snapshot_mutex);
        if (!ReadSnapshot(path, &root) || root.get("key").as_string() != key) return false;
    }
    msgs->clear();
    for (const Json &j : root.get("msgs").items()) {
        MessageSummary m;
        m.uid = static_cast<uint32_t>(j.get("uid").as_double(0));
        m.seq = static_cast<uint32_t>(j.get("seq").as_double(0));
        m.flags = JsonStrings(j.get("flags"));
        m.internal_date = static_cast<long long>(j.get("idate").as_double(-1));
        m.size = static_cast<uint64_t>(j.get("size").as_double(0));
        m.subject = j.get("subject").as_string();
        m.from = JsonAddresses(j.get("from"));
        m.to = JsonAddresses(j.get("to"));
        m.date = static_cast<long long>(j.get("date").as_double(-1));
        m.message_id = j.get("mid").as_string();
        m.has_attachment = j.get("att").as_bool(false);
        if (m.uid != 0) msgs->push_back(std::move(m));
    }
    *exists = static_cast<uint32_t>(root.get("exists").as_double(0));
    return true;
}

void SaveListSnapshot(const std::string &key, const std::vector<MessageSummary> &msgs, uint32_t exists) {
    const std::string path = SnapshotPath("list", key);
    if (path.empty()) return;
    Json root = Json::Object();
    root["v"] = 1;
    root["key"] = key;
    root["exists"] = Json(static_cast<double>(exists));
    Json arr = Json::Array();
    for (const MessageSummary &m : msgs) {
        Json j = Json::Object();
        j["uid"] = Json(static_cast<double>(m.uid));
        j["seq"] = Json(static_cast<double>(m.seq));
        j["flags"] = StringsJson(m.flags);
        j["idate"] = m.internal_date;
        j["size"] = Json(static_cast<double>(m.size));
        j["subject"] = m.subject;
        j["from"] = AddressesJson(m.from);
        j["to"] = AddressesJson(m.to);
        j["date"] = m.date;
        j["mid"] = m.message_id;
        j["att"] = m.has_attachment;
        arr.push_back(std::move(j));
    }
    root["msgs"] = std::move(arr);
    std::lock_guard<std::mutex> lock(g_snapshot_mutex);
    WritePrivateFile(path, root.dump() + "\n");
}

bool LoadFolderSnapshot(const std::string &email, std::vector<Folder> *folders, long long *saved_at) {
    const std::string path = SnapshotPath("folders", Lower(email));
    if (path.empty()) return false;
    Json root;
    {
        std::lock_guard<std::mutex> lock(g_snapshot_mutex);
        if (!ReadSnapshot(path, &root) || root.get("key").as_string() != Lower(email)) return false;
    }
    folders->clear();
    for (const Json &j : root.get("folders").items()) {
        Folder f;
        f.raw = j.get("raw").as_string();
        f.name = j.get("name").as_string();
        const std::string delim = j.get("delim").as_string("/");
        f.delim = delim.empty() ? '/' : delim[0];
        f.attrs = JsonStrings(j.get("attrs"));
        f.selectable = j.get("selectable").as_bool(true);
        f.role = j.get("role").as_string();
        if (!f.raw.empty()) folders->push_back(std::move(f));
    }
    *saved_at = static_cast<long long>(root.get("saved_at").as_double(0));
    return !folders->empty();
}

void SaveFolderSnapshot(const std::string &email, const std::vector<Folder> &folders) {
    const std::string path = SnapshotPath("folders", Lower(email));
    if (path.empty() || folders.empty()) return;
    Json root = Json::Object();
    root["v"] = 1;
    root["key"] = Lower(email);
    root["saved_at"] = static_cast<long long>(std::time(nullptr));
    Json arr = Json::Array();
    for (const Folder &f : folders) {
        Json j = Json::Object();
        j["raw"] = f.raw;
        j["name"] = f.name;
        j["delim"] = std::string(1, f.delim);
        j["attrs"] = StringsJson(f.attrs);
        j["selectable"] = f.selectable;
        j["role"] = f.role;
        arr.push_back(std::move(j));
    }
    root["folders"] = std::move(arr);
    std::lock_guard<std::mutex> lock(g_snapshot_mutex);
    WritePrivateFile(path, root.dump() + "\n");
}

namespace {

constexpr uintmax_t kMessageCacheMaxBytes = 256u << 20;  // trimmed (least recently used first) past this
std::mutex g_message_cache_mutex;
int g_message_saves = 0;  // trims run every so many saves, not on each

// MailDataDir()/cache/messages/<hash>.eml; "" without a data dir. The
// file's first two lines are the key and the Message-ID it was saved for.
std::string MessageCachePath(const std::string &key, bool create_dir) {
    const std::string dir = MailDataDir();
    if (dir.empty()) return "";
    if (create_dir) {
        MakeDir(dir + "/cache");
        MakeDir(dir + "/cache/messages");
    }
    uint64_t h = 1469598103934665603ull;  // FNV-1a: stable across runs
    for (char c : key) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(h));
    return dir + "/cache/messages/" + hex + ".eml";
}

// Drops the least recently used files until the directory fits the cap.
void TrimMessageCache(const std::string &dir) {
    std::error_code ec;
    std::vector<std::pair<std::filesystem::file_time_type, std::pair<std::string, uintmax_t>>> files;
    uintmax_t total = 0;
    for (const auto &ent : std::filesystem::directory_iterator(dir, ec)) {
        std::error_code e2;
        const uintmax_t size = ent.file_size(e2);
        if (e2) continue;
        const auto when = ent.last_write_time(e2);
        if (e2) continue;
        files.push_back({when, {ent.path().string(), size}});
        total += size;
    }
    if (total <= kMessageCacheMaxBytes) return;
    std::sort(files.begin(), files.end());
    for (const auto &f : files) {
        if (total <= kMessageCacheMaxBytes * 3 / 4) break;  // some slack, so the next saves do not trim again
        std::filesystem::remove(f.second.first, ec);
        total -= f.second.second;
    }
}

}  // namespace

bool HaveCachedMessage(const std::string &key) {
    const std::string path = MessageCachePath(key, false);
    std::error_code ec;
    return !path.empty() && std::filesystem::exists(path, ec);
}

bool LoadCachedMessage(const std::string &key, const std::string &message_id, std::string *raw) {
    const std::string path = MessageCachePath(key, false);
    if (path.empty()) return false;
    std::string data;
    {
        std::lock_guard<std::mutex> lock(g_message_cache_mutex);
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::ostringstream ss;
        ss << in.rdbuf();
        data = ss.str();
        std::error_code ec;
        std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), ec);  // recently used
    }
    const size_t nl1 = data.find('\n');
    const size_t nl2 = nl1 == std::string::npos ? std::string::npos : data.find('\n', nl1 + 1);
    if (nl2 == std::string::npos || data.compare(0, nl1, key) != 0) return false;
    // A different Message-ID: the UID now names another message (the
    // mailbox's UIDVALIDITY changed), so this copy is no good.
    const std::string saved_id = data.substr(nl1 + 1, nl2 - nl1 - 1);
    if (!message_id.empty() && !saved_id.empty() && saved_id != message_id) return false;
    raw->assign(data, nl2 + 1, std::string::npos);
    return !raw->empty();
}

void SaveCachedMessage(const std::string &key, const std::string &message_id, const std::string &raw) {
    if (raw.empty() || key.find('\n') != std::string::npos || message_id.find('\n') != std::string::npos) return;
    const std::string path = MessageCachePath(key, true);
    if (path.empty()) return;
    std::lock_guard<std::mutex> lock(g_message_cache_mutex);
    WritePrivateFile(path, key + "\n" + message_id + "\n" + raw);
    if (g_message_saves++ % 32 == 0) TrimMessageCache(path.substr(0, path.rfind('/')));
}

bool LoadSavedOAuthClient(const std::string &provider_id, OAuthClient *out) {
    std::ifstream in(OAuthClientsPath(), std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    Json root;
    if (!Json::Parse(ss.str(), &root) || !root.is_object()) return false;
    const Json &c = root.get(provider_id);
    out->id = c.get("client_id").as_string("");
    out->secret = c.get("client_secret").as_string("");
    return !out->id.empty();
}

bool SaveOAuthClient(const std::string &provider_id, const OAuthClient &client, std::string *error) {
    const std::string path = OAuthClientsPath();
    Json root = Json::Object();
    {
        std::ifstream in(path, std::ios::binary);
        if (in) {
            std::ostringstream ss;
            ss << in.rdbuf();
            Json existing;
            if (Json::Parse(ss.str(), &existing) && existing.is_object()) root = existing;
        }
    }
    Json c = Json::Object();
    c["client_id"] = Json(client.id);
    if (!client.secret.empty()) c["client_secret"] = Json(client.secret);
    root[provider_id] = std::move(c);
    if (path.empty() || !WritePrivateFile(path, root.dump() + "\n")) {
        *error = "cannot write " + path;
        return false;
    }
    return true;
}

bool HaveOAuthClient(const std::string &provider_id) {
    OAuthClient c;
    const OAuthProvider *p = OAuthProviderById(provider_id);
    if (LoadSavedOAuthClient(provider_id, &c) && (!p || !p->needs_secret || !c.secret.empty())) return true;
    return HasBuiltinOAuthClient(provider_id);
}

void FillOAuthClient(Account *a) {
    if (!a->oauth_client_id.empty() || a->oauth_provider.empty()) return;
    OAuthClient c;
    if (LoadSavedOAuthClient(a->oauth_provider, &c)) {
        a->oauth_client_id = c.id;
        a->oauth_client_secret = c.secret;
        return;
    }
    FillBuiltinOAuthClient(a);
}

bool ParseGoogleClientJson(const std::string &text, OAuthClient *out, std::string *error) {
    Json root;
    if (!Json::Parse(text, &root) || !root.is_object()) {
        *error = "that file is not JSON";
        return false;
    }
    if (root.contains("web") && !root.contains("installed")) {
        *error = "that client is a \"Web application\" -- mep needs a \"Desktop app\" client (Google Auth Platform > "
                 "Clients > Create client > Desktop app)";
        return false;
    }
    const Json &c = root.get("installed");
    out->id = Trim(c.get("client_id").as_string(""));
    out->secret = Trim(c.get("client_secret").as_string(""));
    if (out->id.empty() || out->secret.empty()) {
        *error = "no client_id/client_secret in that file (is it the JSON Google offers to download for a client?)";
        return false;
    }
    return true;
}

std::string FindGoogleClientJson() {
    const std::string home = HomeDirectory();
    if (home.empty()) return "";
    std::error_code ec;
    std::string best;
    std::filesystem::file_time_type best_time{};
    for (const auto &ent : std::filesystem::directory_iterator(home + "/Downloads", ec)) {
        const std::string name = ent.path().filename().string();
        if (name.rfind("client_secret_", 0) != 0 || ent.path().extension() != ".json") continue;
        const auto t = ent.last_write_time(ec);
        if (best.empty() || t > best_time) {
            best = ent.path().string();
            best_time = t;
        }
    }
    return best;
}

std::string MailLogPath() {
    const std::string dir = MailDataDir();
    return dir.empty() ? "" : dir + "/mail.log";
}

void MailLog(const std::string &line) {
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    const std::string path = MailLogPath();
    if (path.empty()) return;
    std::error_code ec;
    if (std::filesystem::file_size(path, ec) > 256 * 1024 && !ec) {
        std::filesystem::rename(path, path + ".1", ec);  // keep one old log, small
    }
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) return;
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
    out << stamp << "  " << line << "\n";
    out.close();
#if !defined(_WIN32)
    chmod(path.c_str(), 0600);
#endif
}

// --- The vault ---------------------------------------------------------------

namespace {

constexpr uint32_t kVaultIterations = 600000;  // OWASP's 2023 figure for PBKDF2-HMAC-SHA256
constexpr char kVaultAad[] = "mep-mail-vault-v1";

struct VaultState {
    std::mutex mutex;
    bool unlocked = false;
    std::string key;   // 32 bytes, only while unlocked
    std::string salt;
    uint32_t iterations = kVaultIterations;
    std::map<std::string, std::string> items;  // lower-cased email -> secret
};

VaultState &Vault() {
    static VaultState v;
    return v;
}

std::string VaultPath() {
    const std::string dir = MailDataDir();
    return dir.empty() ? "" : dir + "/vault.json";
}

// Seals and writes the whole vault (caller holds the mutex): a fresh nonce
// every time, temp file + rename so a crash never leaves half a vault.
bool VaultWriteLocked(VaultState &v, std::string *error) {
    Json items = Json::Object();
    for (const auto &kv : v.items) items[kv.first] = Json(kv.second);
    Json inner = Json::Object();
    inner["items"] = std::move(items);
    std::string plain = inner.dump();
    const std::string nonce = SecureRandom(12);
    const std::string sealed = AeadSeal(v.key, nonce, plain, kVaultAad);
    Wipe(&plain);
    Json outer = Json::Object();
    outer["version"] = Json(1);
    outer["kdf"] = Json("pbkdf2-sha256");
    outer["iterations"] = Json(static_cast<long long>(v.iterations));
    outer["salt"] = Json(Base64Encode(v.salt));
    outer["cipher"] = Json("chacha20-poly1305");
    outer["nonce"] = Json(Base64Encode(nonce));
    outer["data"] = Json(Base64Encode(sealed));
    const std::string path = VaultPath();
    if (path.empty()) {
        *error = "no data directory for the mail vault";
        return false;
    }
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            *error = "cannot write " + tmp;
            return false;
        }
        out << outer.dump() << "\n";
    }
#if !defined(_WIN32)
    chmod(tmp.c_str(), 0600);
#endif
    std::remove(path.c_str());
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        *error = "cannot replace " + path;
        return false;
    }
    return true;
}

}  // namespace

bool VaultExists() {
    const std::string path = VaultPath();
    std::ifstream in(path, std::ios::binary);
    return !path.empty() && static_cast<bool>(in);
}

bool VaultUnlocked() {
    std::lock_guard<std::mutex> lock(Vault().mutex);
    return Vault().unlocked;
}

bool VaultUnlock(const std::string &passphrase, bool create, std::string *error) {
    const std::string path = VaultPath();
    std::string text;
    {
        std::ifstream in(path, std::ios::binary);
        if (in) {
            std::ostringstream ss;
            ss << in.rdbuf();
            text = ss.str();
        }
    }
    if (text.empty()) {
        if (!create) {
            *error = "there is no mail vault yet";
            return false;
        }
        VaultState &v = Vault();
        std::string salt = SecureRandom(16);
        std::string key = Pbkdf2Sha256(passphrase, salt, kVaultIterations, 32);
        std::lock_guard<std::mutex> lock(v.mutex);
        v.salt = salt;
        v.iterations = kVaultIterations;
        v.key = key;
        v.items.clear();
        v.unlocked = true;
        Wipe(&key);
        return VaultWriteLocked(v, error);
    }
    Json outer;
    if (!Json::Parse(text, &outer) || !outer.is_object() || outer.get("version").as_int(0) != 1 ||
        outer.get("cipher").as_string("") != "chacha20-poly1305") {
        *error = path + " is not a mail vault this mep can read";
        return false;
    }
    const std::string salt = Base64Decode(outer.get("salt").as_string(""));
    const double iter = outer.get("iterations").as_double(0.0);
    const std::string nonce = Base64Decode(outer.get("nonce").as_string(""));
    const std::string sealed = Base64Decode(outer.get("data").as_string(""));
    // A file that asks for a trivially cheap derivation was not written by mep.
    if (salt.size() < 16 || nonce.size() != 12 || iter < 100000 || iter > 50000000) {
        *error = path + " is damaged";
        return false;
    }
    const uint32_t iterations = static_cast<uint32_t>(iter);
    std::string key = Pbkdf2Sha256(passphrase, salt, iterations, 32);
    std::string plain;
    if (!AeadOpen(key, nonce, sealed, kVaultAad, &plain)) {
        Wipe(&key);
        *error = "wrong passphrase (or the vault file was altered)";
        return false;
    }
    Json inner;
    const bool parsed = Json::Parse(plain, &inner) && inner.is_object();
    Wipe(&plain);
    if (!parsed) {
        Wipe(&key);
        *error = path + " is damaged";
        return false;
    }
    VaultState &v = Vault();
    std::lock_guard<std::mutex> lock(v.mutex);
    v.items.clear();
    const Json &items = inner.get("items");
    if (items.is_object()) {
        for (const auto &kv : items.fields()) v.items[kv.first] = kv.second.as_string("");
    }
    v.salt = salt;
    v.iterations = iterations;
    v.key = key;
    v.unlocked = true;
    Wipe(&key);
    return true;
}

void VaultLock() {
    VaultState &v = Vault();
    std::lock_guard<std::mutex> lock(v.mutex);
    Wipe(&v.key);
    for (auto &kv : v.items) Wipe(&kv.second);
    v.items.clear();
    v.unlocked = false;
}

#if defined(__APPLE__)
namespace {

std::string KeychainLabel(const Account &a) { return "mep mail: " + a.email; }

// What this process already read from (or wrote to) the Keychain, so only
// the first read can ask -- the rest of the session's connections, each a
// worker resolving the secret, reuse it. g_keychain_mutex also serializes
// whole reads: workers starting together wait for the one dialog instead
// of each showing their own.
std::mutex g_keychain_mutex;
std::map<std::string, std::string> g_keychain_cache;
std::map<std::string, std::chrono::steady_clock::time_point> g_keychain_denied;

void CacheKeychainSecret(const Account &a, const std::string &secret) {
    std::string &slot = g_keychain_cache[Lower(a.email)];
    Wipe(&slot);
    slot = secret;
    g_keychain_denied.erase(Lower(a.email));
}

void ForgetKeychainSecret(const Account &a) {
    std::lock_guard<std::mutex> lock(g_keychain_mutex);
    auto it = g_keychain_cache.find(Lower(a.email));
    if (it == g_keychain_cache.end()) return;
    Wipe(&it->second);
    g_keychain_cache.erase(it);
}

// Replaces any existing item (one only mep can read once rewritten).
bool MacKeychainWriteLocked(const Account &a, const std::string &secret, std::string *error, bool interactive) {
    using keychain::Status;
    Status del = keychain::Delete(kService, a.email, false);
    // Made by another mep build, or by the `security` command (older mep):
    // macOS asks once before an item that does not trust this build goes.
    if (del == Status::Denied && interactive) del = keychain::Delete(kService, a.email, true);
    // Silently first; macOS may want the user's permission before an app
    // adds to the login keychain, which only an interactive save asks for.
    Status st = keychain::Add(kService, a.email, KeychainLabel(a), secret, false);
    if (st == Status::Denied && interactive) st = keychain::Add(kService, a.email, KeychainLabel(a), secret, true);
    if (st != Status::Ok) {
        *error = st == Status::Denied ? "the Keychain is locked or asked for permission"
                                      : "the Keychain refused to save the password";
        return false;
    }
    CacheKeychainSecret(a, secret);
    return true;
}

bool MacKeychainWrite(const Account &a, const std::string &secret, std::string *error, bool interactive = true) {
    std::lock_guard<std::mutex> lock(g_keychain_mutex);
    return MacKeychainWriteLocked(a, secret, error, interactive);
}

std::string MacKeychainRead(const Account &a, std::string *error) {
    using keychain::Status;
    std::lock_guard<std::mutex> lock(g_keychain_mutex);
    const std::string key = Lower(a.email);
    auto cached = g_keychain_cache.find(key);
    if (cached != g_keychain_cache.end()) return cached->second;
    auto denied = g_keychain_denied.find(key);
    if (denied != g_keychain_denied.end() && std::chrono::steady_clock::now() - denied->second < std::chrono::seconds(30)) {
        // Just refused: the workers that queued behind that dialog don't each reopen it.
        *error = "access to the Keychain item for " + a.email + " was not allowed";
        return "";
    }
    std::string secret;
    bool ours = false;
    Status st = keychain::Read(kService, a.email, false, &secret, &ours);
    if (st == Status::Ok) {
        if (!ours) {
            // Readable but not marked as mep's own: re-create it so it trusts mep alone.
            std::string ignored;
            MacKeychainWriteLocked(a, secret, &ignored, false);
        }
        CacheKeychainSecret(a, secret);
        return secret;
    }
    if (st == Status::Denied) {
        // Made by another build of mep (an ad-hoc signed binary is a new program
        // to macOS after every rebuild or install) or by the `security` command
        // (older mep): one dialog, then cached for the session, and rewritten so
        // it trusts this build alone.
        st = keychain::Read(kService, a.email, true, &secret, &ours);
        if (st == Status::Ok) {
            if (!ours) {
                std::string ignored;
                MacKeychainWriteLocked(a, secret, &ignored, false);
            }
            CacheKeychainSecret(a, secret);
            return secret;
        }
        if (st == Status::Denied) g_keychain_denied[key] = std::chrono::steady_clock::now();
        *error = st == Status::Denied ? "access to the Keychain item for " + a.email + " was not allowed"
                                      : "the Keychain could not be read";
        return "";
    }
    *error = "no password for " + a.email + " in the Keychain (run :MailSetup)";
    return "";
}

}  // namespace
#endif

std::string KeychainName() {
#if defined(__APPLE__)
    return "the macOS Keychain";
#elif defined(_WIN32)
    return "a DPAPI-encrypted file";
#else
    return mep::compat::ProgramOnPath("secret-tool") ? "the Secret Service keyring" : "a private file";
#endif
}

std::string ResolveSecret(const Account &a, std::string *error) {
    if (a.secret == "command") {
        if (a.secret_command.empty()) {
            *error = "account " + a.id + " has secret = \"command\" but no secret_command";
            return "";
        }
        ProcResult p = RunProcess(ShellArgv(a.secret_command), "", 60000, nullptr);
        const std::string s = FirstLine(p.out);
        if (!p.spawned || p.exit_code != 0 || s.empty()) {
            *error = "secret_command failed: " + Trim(FirstLine(p.err.empty() ? p.out : p.err));
            return "";
        }
        return s;
    }
    if (a.secret == "vault") {
        VaultState &v = Vault();
        std::lock_guard<std::mutex> lock(v.mutex);
        if (!v.unlocked) {
            *error = "the mail vault is locked (:Mail asks for its passphrase)";
            return "";
        }
        auto it = v.items.find(Lower(a.email));
        if (it == v.items.end() || it->second.empty()) {
            *error = "no saved password for " + a.email + " in the mail vault (run :MailSetup)";
            return "";
        }
        return it->second;
    }
    if (a.secret == "file") {
        // Plain files are what older versions wrote; the editor moves them
        // into the vault as soon as it can (MailUi::MigratePlainSecrets).
        std::string s = ReadSecretFile(a, false);
        if (s.empty()) *error = "no saved password for " + a.email + " (run :MailSetup)";
        return s;
    }
    if (!IsPlainEmail(a.email)) {
        *error = "\"" + a.email + "\" is not a plain email address";
        return "";
    }
    // keychain
#if defined(__APPLE__)
    return MacKeychainRead(a, error);
#elif defined(_WIN32)
    std::string s = ReadSecretFile(a, true);
    if (s.empty()) *error = "no saved password for " + a.email + " (run :MailSetup)";
    return s;
#else
    if (mep::compat::ProgramOnPath("secret-tool")) {
        ProcResult p = RunProcess({"secret-tool", "lookup", "service", kService, "account", a.email}, "", 30000, nullptr);
        std::string s = FirstLine(p.out);
        if (p.exit_code == 0 && !s.empty()) return s;
    }
    std::string s = ReadSecretFile(a, false);
    if (s.empty()) *error = "no saved password for " + a.email + " (run :MailSetup)";
    return s;
#endif
}

bool StoreSecret(const Account &a, const std::string &secret, std::string *error, bool interactive) {
    if (a.secret == "command") return true;
    if (a.secret == "vault") {
        VaultState &v = Vault();
        std::lock_guard<std::mutex> lock(v.mutex);
        if (!v.unlocked) {
            *error = "the mail vault is locked";
            return false;
        }
        v.items[Lower(a.email)] = secret;
        return VaultWriteLocked(v, error);
    }
    if (a.secret == "file") return WriteSecretFile(a, secret, false, error);
    if (!IsPlainEmail(a.email)) {
        *error = "\"" + a.email + "\" is not a plain email address";
        return false;
    }
#if defined(__APPLE__)
    return MacKeychainWrite(a, secret, error, interactive);
#else
    // Only the macOS keychain path can prompt, so everywhere else this
    // is deliberately unused -- same (void) convention the rest of this
    // file uses for platform-conditional parameters.
    (void)interactive;
#if defined(_WIN32)
    return WriteSecretFile(a, secret, true, error);
#else
    if (mep::compat::ProgramOnPath("secret-tool")) {
        ProcResult p = RunProcess({"secret-tool", "store", "--label=mep mail " + a.email, "service", kService, "account",
                                   a.email},
                                  secret, 60000, nullptr);
        if (p.exit_code == 0) return true;
    }
    return WriteSecretFile(a, secret, false, error);
#endif
#endif
}

void ForgetSecret(const Account &a) {
    std::remove(SecretFilePath(a).c_str());
    if (a.secret == "vault") {
        VaultState &v = Vault();
        std::lock_guard<std::mutex> lock(v.mutex);
        auto it = v.items.find(Lower(a.email));
        if (v.unlocked && it != v.items.end()) {
            Wipe(&it->second);
            v.items.erase(it);
            std::string ignored;
            VaultWriteLocked(v, &ignored);
        }
        return;
    }
    if (!IsPlainEmail(a.email)) return;
    if (a.secret != "keychain") return;
#if defined(__APPLE__)
    ForgetKeychainSecret(a);
    if (mail::keychain::Delete(kService, a.email, false) == mail::keychain::Status::Denied) {
        mail::keychain::Delete(kService, a.email, true);
    }
#elif !defined(_WIN32)
    if (mep::compat::ProgramOnPath("secret-tool")) {
        RunProcess({"secret-tool", "clear", "service", kService, "account", a.email}, "", 30000, nullptr);
    }
#endif
}

// --- IMAP ---------------------------------------------------------------

namespace {

std::string AuthConfig(const Session &s) {
    std::string c;
    if (s.account.UsesOAuth()) {
        c += "user = " + CurlConfigQuote(s.account.Login()) + "\n";
        c += "oauth2-bearer = " + CurlConfigQuote(s.secret) + "\n";
    } else {
        c += "user = " + CurlConfigQuote(s.account.Login() + ":" + s.secret) + "\n";
    }
    return c;
}

std::string TlsConfig(const Account &a, const std::string &url) {
    // Plain-text schemes upgrade with STARTTLS, and must (ssl-reqd) unless
    // the account is explicitly a local, unencrypted one (a test server,
    // Proton Bridge).
    if (a.insecure) return "";
    if (url.rfind("imap://", 0) == 0 || url.rfind("smtp://", 0) == 0) return "ssl-reqd\n";
    return "";
}

std::string FirstTaggedFailure(const std::vector<ImapResponse> &rs) {
    for (const ImapResponse &r : rs) {
        if (r.tag == "*" || r.tag == "+" || r.values.empty()) continue;
        const std::string &v = r.values[0].text;
        if (v.rfind("NO", 0) == 0 || v.rfind("BAD", 0) == 0) return v;
    }
    return "";
}

}  // namespace

std::string DescribeCurlFailure(int exit_code, const std::string &stderr_text, const Account &a) {
    std::string why;
    switch (exit_code) {
        case 6: why = "cannot resolve " + a.imap_host + " (offline?)"; break;
        case 7: why = "cannot connect to the mail server (offline, or blocked port?)"; break;
        case 28: why = "the mail server timed out"; break;
        case 35: why = "TLS handshake failed"; break;
        case 51:
        case 60: why = "the server's certificate could not be verified"; break;
        case 67:
            why = "login refused for " + a.Login();
            if (a.UsesOAuth()) {
                why += " -- the sign-in token was not accepted; run :MailSignIn to sign in again";
            } else if (a.provider == "gmail") {
                why += " -- Gmail does not take your Google password here: sign in with the browser instead "
                       "(:MailSetup google), or use an app password";
            } else {
                why += " -- check the password (most providers need an app password for IMAP)";
            }
            break;
        case 78: why = "no such mailbox or message"; break;
        case 55:
        case 56: why = "the connection broke"; break;
        case 9: why = "access denied by the server"; break;
        case 21: why = "the server rejected the command"; break;
        default: why = "curl exited with code " + std::to_string(exit_code); break;
    }
    std::string detail = Trim(stderr_text);
    const size_t nl = detail.find('\n');
    if (nl != std::string::npos) detail = detail.substr(0, nl);
    if (!detail.empty() && exit_code != 67 && exit_code != 21) why += " (" + detail + ")";
    return why;
}

namespace {

// "insecure" (no TLS) is for a server on this machine (a test server,
// Proton Bridge); over a network it would send the password in clear.
bool PlaintextRefused(const Account &a, const std::string &host, std::string *error) {
    if (!a.insecure || IsLoopbackHost(host)) return false;
    *error = "refusing an unencrypted connection to " + host +
             " -- \"insecure\" is only allowed for servers on this computer (127.0.0.1 / localhost)";
    return true;
}

}  // namespace

ImapResult RunImap(const Session &s, const std::vector<ImapStep> &steps, bool traced, const std::atomic<bool> *cancel,
                   int timeout_ms) {
    ImapResult r;
    if (PlaintextRefused(s.account, s.account.imap_host, &r.error)) return r;
    if (steps.empty()) {
        r.ok = true;
        return r;
    }
    // A pooled connection of mep's own first (mail_imap.h); curl otherwise.
    if (RunImapNative(s, steps, traced, cancel, timeout_ms, &r)) return r;
    r = ImapResult{};
    std::string config;
    for (size_t i = 0; i < steps.size(); i++) {
        const ImapStep &st = steps[i];
        if (i > 0) config += "next\n";
        const std::string url = ImapUrl(s.account, st.mailbox) + st.url_suffix;
        config += AuthConfig(s);
        config += TlsConfig(s.account, url);
        config += "url = " + CurlConfigQuote(url) + "\n";
        if (!st.request.empty()) config += "request = " + CurlConfigQuote(st.request) + "\n";
        if (traced) config += "output = " + CurlConfigQuote(NullDevice()) + "\n";
        config += "connect-timeout = 20\n";
    }
    std::vector<std::string> argv = {CurlProgram(), "-sS", "-K", "-"};
    if (traced) {
        argv.push_back("--trace");
        argv.push_back("-");
    }
    const auto started = std::chrono::steady_clock::now();
    ProcResult p = RunProcess(argv, config, timeout_ms, cancel);
    if (traced) {
        r.trace = ParseCurlTrace(p.out);
        r.responses = ParseImapStream(r.trace.received);
    } else {
        r.body = std::move(p.out);
    }
    {
        // Timings for :MailLog -- each step's command verb only (never a search term or a secret).
        std::string what;
        for (const ImapStep &st : steps) {
            std::string verb = st.request.empty() ? std::string("FETCH-URL") : st.request.substr(0, st.request.find(' '));
            if ((verb == "UID" || verb == "uid") && st.request.size() > 4) {
                verb = "UID " + st.request.substr(4, st.request.find(' ', 4) - 4);
            }
            what += (what.empty() ? "" : " + ") + verb;
        }
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                                    started).count();
        // Gmail tags its answers [THROTTLED] while it holds each command
        // of a busy account back (seconds apiece) -- worth knowing when
        // mail is slow.
        const size_t bytes = traced ? r.trace.received.size() : r.body.size();
        const bool throttled = traced && r.trace.received.find("[THROTTLED]") != std::string::npos;
        MailLog("imap " + what + ": " + std::to_string(ms) + " ms, " + std::to_string(bytes / 1024) + " KB" +
                (throttled ? ", server throttling" : "") +
                (p.exit_code != 0 ? ", curl exit " + std::to_string(p.exit_code) : ""));
    }
    if (!p.spawned) {
        r.error = "could not run curl (is it installed?)";
        return r;
    }
    if (p.cancelled) {
        r.error = "cancelled";
        return r;
    }
    if (p.timed_out) {
        r.error = "the mail server timed out";
        return r;
    }
    if (p.exit_code != 0) {
        r.error = DescribeCurlFailure(p.exit_code, p.err, s.account);
        const std::string server = FirstTaggedFailure(r.responses);
        if (server.find("Application-specific password required") != std::string::npos) {
            // Gmail with 2-Step Verification: the account password is never accepted over IMAP.
            r.error = "login refused for " + s.account.Login() +
                      " -- Google wants an app password, not your account password: make one at "
                      "https://myaccount.google.com/apppasswords (or sign in with the browser: :MailSetup google)";
            return r;
        }
        if (!server.empty()) r.error += ": " + server;
        return r;
    }
    r.ok = true;
    return r;
}

bool ListFolders(const Session &s, std::vector<Folder> *out, std::string *error, const std::atomic<bool> *cancel) {
    ImapResult r = RunImap(s, {{"", "LIST \"\" \"*\"", ""}}, true, cancel);
    if (!r.ok) {
        *error = r.error;
        return false;
    }
    *out = ReadListResponses(r.responses);
    return true;
}

bool SelectMailbox(const Session &s, const std::string &mailbox, SelectInfo *out, std::string *error,
                   const std::atomic<bool> *cancel) {
    ImapResult r = RunImap(s, {{mailbox, "NOOP", ""}}, true, cancel);
    if (!r.ok) {
        *error = r.error;
        return false;
    }
    *out = ReadSelect(r.responses);
    return true;
}

bool ListFoldersAndSelect(const Session &s, const std::string &mailbox, std::vector<Folder> *folders, SelectInfo *sel,
                          std::string *error, const std::atomic<bool> *cancel) {
    ImapResult r = RunImap(s, {{"", "LIST \"\" \"*\"", ""}, {mailbox, "NOOP", ""}}, true, cancel);
    if (!r.ok) {
        *error = r.error;
        return false;
    }
    *folders = ReadListResponses(r.responses);
    *sel = ReadSelect(r.responses);
    return true;
}

bool FetchSummaries(const Session &s, const std::string &mailbox, const std::string &set, bool by_uid,
                    std::vector<MessageSummary> *out, std::string *error, const std::atomic<bool> *cancel,
                    SelectInfo *sel) {
    const std::string cmd = std::string(by_uid ? "UID FETCH " : "FETCH ") + set + " " + SummaryFetchItems();
    ImapResult r = RunImap(s, {{mailbox, cmd, ""}}, true, cancel);
    if (sel) *sel = ReadSelect(r.responses);
    if (!r.ok) {
        *error = r.error;
        return false;
    }
    *out = ReadFetchSummaries(r.responses);
    return true;
}

bool PeekMessages(const Session &s, const std::string &mailbox, const std::vector<uint32_t> &uids,
                  std::vector<std::pair<uint32_t, std::string>> *out, std::string *error,
                  const std::atomic<bool> *cancel) {
    if (uids.empty()) return true;
    ImapResult r = RunImap(s, {{mailbox, "UID FETCH " + ImapSet(uids) + " (UID BODY.PEEK[])", ""}}, true, cancel);
    if (!r.ok) {
        *error = r.error;
        return false;
    }
    *out = ReadFetchBodies(r.responses);
    return true;
}

bool FetchMessage(const Session &s, const std::string &mailbox, uint32_t uid, std::string *raw, std::string *error,
                  const std::atomic<bool> *cancel) {
    ImapResult r = RunImap(s, {{mailbox, "", ";UID=" + std::to_string(uid)}}, false, cancel);
    if (!r.ok) {
        *error = r.error;
        return false;
    }
    *raw = std::move(r.body);
    return true;
}

bool StoreFlags(const Session &s, const std::string &mailbox, const std::vector<uint32_t> &uids, const std::string &op,
                const std::string &flags, std::string *error, const std::atomic<bool> *cancel) {
    if (uids.empty()) return true;
    ImapResult r = RunImap(s, {{mailbox, "UID STORE " + ImapSet(uids) + " " + op + " " + flags, ""}}, true, cancel);
    if (!r.ok) {
        *error = r.error;
        return false;
    }
    return true;
}

bool MoveMessages(const Session &s, const std::string &mailbox, const std::vector<uint32_t> &uids,
                  const std::string &dest, std::string *error, const std::atomic<bool> *cancel) {
    if (uids.empty()) return true;
    const std::string set = ImapSet(uids);
    ImapResult r = RunImap(s, {{mailbox, "UID MOVE " + set + " " + ImapQuote(dest), ""}}, true, cancel);
    if (r.ok) return true;
    // A NO (no such folder, quota...) is the answer; only a server that
    // does not know MOVE at all (BAD) gets the COPY fallback.
    const std::string status = TaggedStatus(r.trace, r.responses, "UID MOVE");
    if (r.error == "cancelled" || status.empty() || status.rfind("NO", 0) == 0) {
        *error = status.rfind("NO", 0) == 0 ? "the server refused the move: " + Trim(status.substr(2)) : r.error;
        return false;
    }
    // No MOVE (RFC 6851): copy, mark deleted, expunge just those.
    ImapResult c = RunImap(s,
                           {{mailbox, "UID COPY " + set + " " + ImapQuote(dest), ""},
                            {mailbox, "UID STORE " + set + " +FLAGS.SILENT (\\Deleted)", ""}},
                           true, cancel);
    if (!c.ok) {
        *error = c.error;
        return false;
    }
    ImapResult e = RunImap(s, {{mailbox, "UID EXPUNGE " + set, ""}}, true, cancel);
    if (!e.ok) e = RunImap(s, {{mailbox, "EXPUNGE", ""}}, true, cancel);
    if (!e.ok) {
        *error = e.error;
        return false;
    }
    return true;
}

bool SearchMailbox(const Session &s, const std::string &mailbox, const std::string &query, std::vector<uint32_t> *uids,
                   std::string *error, const std::atomic<bool> *cancel) {
    bool ascii = true;
    for (char c : query) ascii = ascii && static_cast<unsigned char>(c) < 0x80;
    std::string cmd;
    if (s.account.provider == "gmail" || s.account.imap_host == "imap.gmail.com") {
        cmd = "UID SEARCH X-GM-RAW " + ImapQuote(query);
    } else {
        cmd = std::string("UID SEARCH ") + (ascii ? "" : "CHARSET UTF-8 ") + "TEXT " + ImapQuote(query);
    }
    ImapResult r = RunImap(s, {{mailbox, cmd, ""}}, true, cancel);
    if (!r.ok) {
        *error = r.error;
        return false;
    }
    *uids = ReadSearch(r.responses);
    return true;
}

bool SendMail(const Session &s, const std::string &from_email, const std::vector<std::string> &recipients,
              const std::string &message, std::string *error, const std::atomic<bool> *cancel) {
    if (recipients.empty()) {
        *error = "no recipients";
        return false;
    }
    if (PlaintextRefused(s.account, s.account.smtp_host, error)) return false;
    const std::string path = WriteTempFile(message, error);
    if (path.empty()) return false;
    const std::string url = SmtpUrl(s.account);
    std::string config = AuthConfig(s) + TlsConfig(s.account, url);
    config += "url = " + CurlConfigQuote(url) + "\n";
    config += "mail-from = " + CurlConfigQuote(from_email) + "\n";
    for (const std::string &rcpt : recipients) config += "mail-rcpt = " + CurlConfigQuote(rcpt) + "\n";
    config += "upload-file = " + CurlConfigQuote(path) + "\n";
    config += "connect-timeout = 20\n";
    ProcResult p = RunProcess({CurlProgram(), "-sS", "-K", "-"}, config, 300000, cancel);
    std::remove(path.c_str());
    if (!p.spawned) {
        *error = "could not run curl (is it installed?)";
        return false;
    }
    if (p.cancelled) {
        *error = "cancelled";
        return false;
    }
    if (p.timed_out) {
        *error = "the mail server timed out while sending";
        return false;
    }
    if (p.exit_code != 0) {
        Account smtp_view = s.account;
        smtp_view.imap_host = s.account.smtp_host;
        *error = DescribeCurlFailure(p.exit_code, p.err, smtp_view);
        if (p.exit_code == 55 || p.exit_code == 26 || p.exit_code == 25) *error = "the server refused the message: " + Trim(p.err);
        return false;
    }
    return true;
}

bool AppendMessage(const Session &s, const std::string &mailbox, const std::string &message, std::string *error,
                   const std::atomic<bool> *cancel) {
    if (PlaintextRefused(s.account, s.account.imap_host, error)) return false;
    const std::string path = WriteTempFile(message, error);
    if (path.empty()) return false;
    const std::string url = ImapUrl(s.account, mailbox);
    std::string config = AuthConfig(s) + TlsConfig(s.account, url);
    config += "url = " + CurlConfigQuote(url) + "\n";
    config += "upload-file = " + CurlConfigQuote(path) + "\n";
    config += "connect-timeout = 20\n";
    ProcResult p = RunProcess({CurlProgram(), "-sS", "-K", "-"}, config, 300000, cancel);
    std::remove(path.c_str());
    if (!p.spawned || p.exit_code != 0) {
        *error = p.spawned ? DescribeCurlFailure(p.exit_code, p.err, s.account) : "could not run curl";
        return false;
    }
    return true;
}

// --- OAuth2 browser sign-in -------------------------------------------------

namespace {

#if defined(_WIN32)
using SockLen = int;
void CloseSock(long long fd) { closesocket(static_cast<SOCKET>(fd)); }
bool SocketsReady() {
    static const bool ok = [] {
        WSADATA w;
        return WSAStartup(MAKEWORD(2, 2), &w) == 0;
    }();
    return ok;
}
#else
using SockLen = socklen_t;
void CloseSock(long long fd) { close(static_cast<int>(fd)); }
bool SocketsReady() { return true; }
#endif

// Waits up to `ms` for `fd` to be readable.
bool WaitReadable(long long fd, int ms) {
#if defined(_WIN32)
    WSAPOLLFD p{};
    p.fd = static_cast<SOCKET>(fd);
    p.events = POLLRDNORM;
    return WSAPoll(&p, 1, ms) > 0;
#else
    pollfd p{static_cast<int>(fd), POLLIN, 0};
    return poll(&p, 1, ms) > 0;
#endif
}

void SendAll(long long fd, const std::string &data) {
    size_t off = 0;
    while (off < data.size()) {
#if defined(_WIN32)
        const int n = send(static_cast<SOCKET>(fd), data.data() + off, static_cast<int>(data.size() - off), 0);
#elif defined(__APPLE__)
        const ssize_t n = send(static_cast<int>(fd), data.data() + off, data.size() - off, 0);
#else
        const ssize_t n = send(static_cast<int>(fd), data.data() + off, data.size() - off, MSG_NOSIGNAL);
#endif
        if (n <= 0) return;
        off += static_cast<size_t>(n);
    }
}

std::string BrowserPage(const std::string &title, const std::string &text) {
    const std::string body = "<!doctype html><meta charset=utf-8><title>mep mail</title>"
                             "<body style=\"font-family:system-ui,sans-serif;max-width:32em;margin:4em auto;line-height:1.5\">"
                             "<h2>" + title + "</h2><p>" + text + "</p></body>";
    return "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\nConnection: close\r\n\r\n" + body;
}

struct CachedToken {
    std::string token;
    long long expires_at = 0;  // unix seconds
};

std::mutex g_token_mutex;
std::map<std::string, CachedToken> g_tokens;  // account email -> access token

std::string TokenKey(const Account &a) { return Lower(a.email) + "|" + a.oauth_provider; }

OAuthTokens PostToken(const OAuthProvider &p, const std::vector<std::pair<std::string, std::string>> &fields,
                      const std::atomic<bool> *cancel) {
    if (!IsSafeAuthUrl(p.token_url)) {
        OAuthTokens refused;
        refused.error = "the token address must be https:// (" + p.token_url + ")";
        return refused;
    }
    // Form fields (the code, the refresh token) go through the config on
    // stdin like every other secret; curl url-encodes them.
    std::string config = "url = " + CurlConfigQuote(p.token_url) + "\n";
    for (const auto &f : fields) config += "data-urlencode = " + CurlConfigQuote(f.first + "=" + f.second) + "\n";
    config += "connect-timeout = 20\n";
    ProcResult r = RunProcess({CurlProgram(), "-sS", "-K", "-"}, config, 60000, cancel);
    OAuthTokens t;
    if (!r.spawned) {
        t.error = "could not run curl (is it installed?)";
    } else if (r.cancelled) {
        t.error = "cancelled";
    } else if (r.exit_code != 0) {
        t.error = "the sign-in server could not be reached (curl exit " + std::to_string(r.exit_code) + ")";
    } else {
        t = ParseTokenResponse(r.out);
    }
    return t;
}

}  // namespace

namespace {

// Starts `argv` and lets it run on its own: no pipes, nothing waited for.
// A browser may stay up for the rest of the session (a fresh `firefox`),
// so it must never be tied to -- or killed with -- the sign-in job.
bool SpawnDetached(const std::vector<std::string> &argv) {
#if defined(__EMSCRIPTEN__)
    (void)argv;
    return false;
#elif defined(_WIN32)
    std::wstring cmd;
    for (const std::string &a : argv) {
        if (!cmd.empty()) cmd.push_back(L' ');
        cmd += QuoteArg(Widen(a));
    }
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr,
                        nullptr, &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    short flags = POSIX_SPAWN_SETPGROUP;  // its own process group: no signals of mep's reach it
#if defined(__APPLE__)
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    posix_spawnattr_setflags(&attr, flags);
    posix_spawnattr_setpgroup(&attr, 0);
    std::vector<char *> args;
    for (const std::string &a : argv) args.push_back(const_cast<char *>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, args[0], &fa, &attr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) return false;
    // Reaped in the background whenever it exits, so it never lingers as a zombie.
    std::thread([pid] {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }).detach();
    return true;
#endif
}

}  // namespace

bool OpenInBrowser(const std::string &url, std::string *error) {
    std::vector<std::string> argv;
    const char *browser = std::getenv("BROWSER");
    if (browser && *browser) {
        // $BROWSER may hold several ':'-separated commands; the first is used.
        std::string cmd = browser;
#if !defined(_WIN32)
        const size_t colon = cmd.find(':');
        if (colon != std::string::npos) cmd.resize(colon);
#endif
        argv = {cmd, url};
    } else {
#if defined(__APPLE__)
        argv = {"open", url};
#elif defined(_WIN32)
        argv = {"rundll32.exe", "url.dll,FileProtocolHandler", url};
#else
        argv = {"xdg-open", url};
#endif
    }
    if (!SpawnDetached(argv)) {
        *error = "could not start " + argv[0];
        return false;
    }
    return true;
}

OAuthRedirectListener::~OAuthRedirectListener() {
    if (fd_ >= 0) CloseSock(fd_);
}

bool OAuthRedirectListener::Listen(std::string *error) {
    if (!SocketsReady()) {
        *error = "networking is unavailable";
        return false;
    }
#if defined(_WIN32)
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        *error = "cannot open a socket";
        return false;
    }
    fd_ = static_cast<long long>(s);
#else
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        *error = "cannot open a socket";
        return false;
    }
    fcntl(s, F_SETFD, FD_CLOEXEC);
    fd_ = s;
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // never reachable from other machines
    addr.sin_port = 0;
    if (bind(static_cast<decltype(s)>(fd_), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
        listen(static_cast<decltype(s)>(fd_), 8) != 0) {
        *error = "cannot listen on 127.0.0.1";
        return false;
    }
    SockLen len = sizeof(addr);
    getsockname(static_cast<decltype(s)>(fd_), reinterpret_cast<sockaddr *>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    return true;
}

std::string OAuthRedirectListener::AwaitCode(const std::string &state, int timeout_ms, const std::atomic<bool> *cancel,
                                             std::string *error) {
    const auto start = std::chrono::steady_clock::now();
    while (true) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        if (cancel && cancel->load()) {
            *error = "cancelled";
            return "";
        }
        if (ms > timeout_ms) {
            *error = "no answer from the browser sign-in (timed out)";
            return "";
        }
        if (!WaitReadable(fd_, 200)) continue;
#if defined(_WIN32)
        const SOCKET cs = accept(static_cast<SOCKET>(fd_), nullptr, nullptr);
        if (cs == INVALID_SOCKET) continue;
        const long long c = static_cast<long long>(cs);
#else
        const int ci = accept(static_cast<int>(fd_), nullptr, nullptr);
        if (ci < 0) continue;
        const long long c = ci;
#if defined(__APPLE__)
        int one = 1;
        setsockopt(ci, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
#endif
        // Read the request head (a browser sends it at once; give it 5s).
        std::string req;
        char buf[4096];
        while (req.find("\r\n\r\n") == std::string::npos && req.size() < 65536 && WaitReadable(c, 5000)) {
#if defined(_WIN32)
            const int n = recv(static_cast<SOCKET>(c), buf, sizeof(buf), 0);
#else
            const ssize_t n = recv(static_cast<int>(c), buf, sizeof(buf), 0);
#endif
            if (n <= 0) break;
            req.append(buf, static_cast<size_t>(n));
        }
        // "GET /?code=...&state=... HTTP/1.1"
        std::string target;
        if (req.rfind("GET ", 0) == 0) {
            const size_t sp = req.find(' ', 4);
            target = req.substr(4, sp == std::string::npos ? std::string::npos : sp - 4);
        }
        const size_t qm = target.find('?');
        const auto query = ParseQuery(qm == std::string::npos ? "" : std::string_view(target).substr(qm + 1));
        std::string code, got_state, err, err_desc;
        for (const auto &kv : query) {
            if (kv.first == "code") code = kv.second;
            else if (kv.first == "state") got_state = kv.second;
            else if (kv.first == "error") err = kv.second;
            else if (kv.first == "error_description") err_desc = kv.second;
        }
        if (code.empty() && err.empty()) {
            // favicon.ico and the like
            SendAll(c, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            CloseSock(c);
            continue;
        }
        if (got_state != state) {
            // Not the request this sign-in started (a stale tab, or someone else's page).
            SendAll(c, BrowserPage("Sign-in not recognised", "This sign-in was not started by mep just now. Go back to mep and try again."));
            CloseSock(c);
            continue;
        }
        if (!err.empty()) {
            SendAll(c, BrowserPage("Sign-in cancelled", "mep was not given access. You can close this tab."));
            CloseSock(c);
            *error = err == "access_denied" ? "sign-in was cancelled in the browser" : err + (err_desc.empty() ? "" : ": " + err_desc);
            return "";
        }
        SendAll(c, BrowserPage("Signed in", "mep can now read and send your mail. You can close this tab and go back to mep."));
        CloseSock(c);
        return code;
    }
}

OAuthTokens OAuthExchangeCode(const OAuthProvider &p, const Account &a, const std::string &code,
                              const std::string &verifier, const std::string &redirect_uri, const std::atomic<bool> *cancel) {
    std::vector<std::pair<std::string, std::string>> fields = {{"grant_type", "authorization_code"},
                                                               {"code", code},
                                                               {"code_verifier", verifier},
                                                               {"redirect_uri", redirect_uri},
                                                               {"client_id", a.oauth_client_id}};
    if (!a.oauth_client_secret.empty()) fields.emplace_back("client_secret", a.oauth_client_secret);
    OAuthTokens t = PostToken(p, fields, cancel);
    if (t.error.empty() && t.refresh_token.empty()) {
        t.error = "the provider gave no refresh token (sign in again, and allow offline access)";
    }
    if (t.error.empty()) {
        std::lock_guard<std::mutex> lock(g_token_mutex);
        g_tokens[TokenKey(a)] = {t.access_token, static_cast<long long>(std::time(nullptr)) + t.expires_in};
    }
    return t;
}

OAuthTokens OAuthRefresh(const OAuthProvider &p, const Account &a, const std::string &refresh_token,
                         const std::atomic<bool> *cancel) {
    std::vector<std::pair<std::string, std::string>> fields = {
        {"grant_type", "refresh_token"}, {"refresh_token", refresh_token}, {"client_id", a.oauth_client_id}};
    if (!a.oauth_client_secret.empty()) fields.emplace_back("client_secret", a.oauth_client_secret);
    if (!p.scope.empty() && p.id == "microsoft") fields.emplace_back("scope", p.scope);
    return PostToken(p, fields, cancel);
}

std::string LoginSecret(const Account &acct, const std::string &stored, std::string *new_stored, std::string *error,
                        const std::atomic<bool> *cancel) {
    if (acct.oauth_provider.empty()) return stored;  // a password, or (oauth2) a token from secret_command
    Account a = acct;
    FillOAuthClient(&a);  // the saved or built-in client, unless the account names one
    OAuthProvider p;
    if (!AccountOAuthProvider(a, &p)) {
        *error = "unknown sign-in provider \"" + a.oauth_provider + "\"";
        return "";
    }
    const long long now = static_cast<long long>(std::time(nullptr));
    {
        std::lock_guard<std::mutex> lock(g_token_mutex);
        auto it = g_tokens.find(TokenKey(a));
        if (it != g_tokens.end() && it->second.expires_at - 120 > now) return it->second.token;
    }
    OAuthTokens t = OAuthRefresh(p, a, stored, cancel);
    if (!t.error.empty()) {
        if (t.error.rfind("invalid_grant", 0) == 0) {
            *error = "the " + p.label + " sign-in has expired or was revoked -- run :MailSignIn to sign in again";
            if (p.id == "google") {
                *error += " (a Google client still in \"Testing\" expires sign-ins after 7 days; publishing it stops that)";
            }
        } else {
            *error = p.label + " sign-in failed: " + t.error;
        }
        return "";
    }
    if (!t.refresh_token.empty() && t.refresh_token != stored) {
        // Rotated (Microsoft does this on every refresh): keep the new one.
        std::string ignored;
        // In the background: no dialog. If it cannot be saved, the new token
        // still serves this session (the editor caches *new_stored).
        StoreSecret(a, t.refresh_token, &ignored, false);
        *new_stored = t.refresh_token;
    }
    std::lock_guard<std::mutex> lock(g_token_mutex);
    g_tokens[TokenKey(a)] = {t.access_token, now + (t.expires_in > 0 ? t.expires_in : 3600)};
    return t.access_token;
}

void ForgetAccessToken(const Account &a) {
    std::lock_guard<std::mutex> lock(g_token_mutex);
    auto it = g_tokens.find(TokenKey(a));
    if (it == g_tokens.end()) return;
    Wipe(&it->second.token);
    g_tokens.erase(it);
}

void ForgetAllAccessTokens() {
    std::lock_guard<std::mutex> lock(g_token_mutex);
    for (auto &kv : g_tokens) Wipe(&kv.second.token);
    g_tokens.clear();
#if defined(__APPLE__)
    std::lock_guard<std::mutex> keychain_lock(g_keychain_mutex);
    for (auto &kv : g_keychain_cache) Wipe(&kv.second);
    g_keychain_cache.clear();
#endif
}

}  // namespace mail
