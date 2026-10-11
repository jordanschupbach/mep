#include "mail_net.h"

// platform_compat.h first: on Windows it supplies the POSIX spellings
// (mkdir(path, mode), ...) that persist.h's inline helpers use.
#include "platform_compat.h"
#include "persist.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#include <direct.h>
#else
#include <fcntl.h>
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
    if (a.secret == "file") {
        std::string s = ReadSecretFile(a, false);
        if (s.empty()) *error = "no saved password for " + a.email + " (run :MailSetup)";
        return s;
    }
    // keychain
#if defined(__APPLE__)
    ProcResult p = RunProcess({"security", "find-generic-password", "-s", kService, "-a", a.email, "-w"}, "", 30000, nullptr);
    std::string s = FirstLine(p.out);
    if (p.exit_code != 0 || s.empty()) {
        *error = "no password for " + a.email + " in the Keychain (run :MailSetup)";
        return "";
    }
    return s;
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

bool StoreSecret(const Account &a, const std::string &secret, std::string *error) {
    if (a.secret == "command") return true;
    if (a.secret == "file") return WriteSecretFile(a, secret, false, error);
#if defined(__APPLE__)
    // `security -i` reads its command from stdin, and -X takes the
    // password as hex: nothing secret in argv, nothing to quote.
    static const char kHex[] = "0123456789abcdef";
    std::string hex;
    for (char c : secret) {
        const unsigned char u = static_cast<unsigned char>(c);
        hex.push_back(kHex[u >> 4]);
        hex.push_back(kHex[u & 15]);
    }
    const std::string cmd =
        "add-generic-password -U -s " + std::string(kService) + " -a " + a.email + " -l mep-mail -X " + hex + "\n";
    ProcResult p = RunProcess({"security", "-i"}, cmd, 30000, nullptr);
    if (!p.spawned || p.exit_code != 0 || p.err.find("rror") != std::string::npos) {
        *error = "the Keychain refused the password: " + Trim(p.err);
        return false;
    }
    return true;
#elif defined(_WIN32)
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
}

void ForgetSecret(const Account &a) {
    std::remove(SecretFilePath(a).c_str());
    if (a.secret != "keychain") return;
#if defined(__APPLE__)
    RunProcess({"security", "delete-generic-password", "-s", kService, "-a", a.email}, "", 30000, nullptr);
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
    if (s.account.oauth2) {
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
            if (a.provider == "gmail") {
                why += " -- Gmail needs an app password (Google account > Security > 2-Step Verification > App "
                       "passwords), not your account password";
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

ImapResult RunImap(const Session &s, const std::vector<ImapStep> &steps, bool traced, const std::atomic<bool> *cancel,
                   int timeout_ms) {
    ImapResult r;
    if (steps.empty()) {
        r.ok = true;
        return r;
    }
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
    ProcResult p = RunProcess(argv, config, timeout_ms, cancel);
    if (!p.spawned) {
        r.error = "could not run curl (is it installed?)";
        return r;
    }
    if (traced) {
        r.trace = ParseCurlTrace(p.out);
        r.responses = ParseImapStream(r.trace.received);
    } else {
        r.body = std::move(p.out);
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

bool FetchSummaries(const Session &s, const std::string &mailbox, const std::string &set, bool by_uid,
                    std::vector<MessageSummary> *out, std::string *error, const std::atomic<bool> *cancel) {
    const std::string cmd = std::string(by_uid ? "UID FETCH " : "FETCH ") + set + " " + SummaryFetchItems();
    ImapResult r = RunImap(s, {{mailbox, cmd, ""}}, true, cancel);
    if (!r.ok) {
        *error = r.error;
        return false;
    }
    *out = ReadFetchSummaries(r.responses);
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

}  // namespace mail
