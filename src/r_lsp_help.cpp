#include "r_lsp_help.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
constexpr auto kHelpTimeout = std::chrono::seconds(5);
constexpr size_t kMaxHelpBytes = 1024 * 1024;

// Arguments are data, never interpolated into R code. help() reads installed
// aliases, including names such as spline.des whose page is named splineDesign.
// Do not library()/requireNamespace() the target package: documentation lookup
// must not run its .onLoad or any code in the document being edited.
constexpr const char *kHelpScript = R"R(
options(useFancyQuotes = FALSE)
args <- commandArgs(trailingOnly = TRUE)
topic <- args[[1L]]
explicit <- args[[2L]]
lookup <- function(pkg = NULL, all = FALSE) {
  tryCatch(suppressWarnings(suppressMessages(
    do.call(utils::help, list(topic = topic, package = pkg,
                             try.all.packages = all, help_type = "text"))
  )), error = function(e) character())
}
h <- character()
if (nzchar(explicit)) {
  h <- lookup(explicit)
} else {
  for (pkg in args[-c(1L, 2L)]) {
    h <- lookup(pkg)
    if (length(h)) break
  }
  if (!length(h)) h <- lookup()
  if (!length(h)) h <- lookup(all = TRUE)
}
if (!length(h)) quit(status = 0L)
if (length(h) > 1L) {
  pkgs <- basename(dirname(dirname(as.character(h))))
  cat(topic, "\n\nHelp is available in multiple packages; qualify the name:\n", sep = "")
  cat(paste0(pkgs, "::", topic), sep = "\n")
} else {
  pkg <- basename(dirname(dirname(as.character(h))))
  text <- capture.output(tools::Rd2txt(utils:::.getHelpFile(h),
                          options = list(width = 90, underline_titles = FALSE)))
  cat(topic, "\n\nPackage: ", pkg, "\n\n", sep = "")
  cat(text, sep = "\n")
}
)R";

#if defined(_WIN32)
// CreateProcess receives an argv-encoded command line directly, without cmd.exe.
std::string QuoteArgument(const std::string &arg) {
    std::string out = "\"";
    size_t slashes = 0;
    for (char ch : arg) {
        if (ch == '\\') { slashes++; continue; }
        out.append(slashes * (ch == '"' ? 2 : 1), '\\');
        slashes = 0;
        if (ch == '"') out += '\\';
        out += ch;
    }
    out.append(slashes * 2, '\\');
    return out + '"';
}
#endif

std::string RunHelp(const std::vector<std::string> &args, const std::string &cwd) {
    const auto deadline = std::chrono::steady_clock::now() + kHelpTimeout;
    std::string out;
#if defined(_WIN32)
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE reader = nullptr, writer = nullptr;
    if (!CreatePipe(&reader, &writer, &security, 0)) return {};
    SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0);
    HANDLE null = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &security, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = null;
    startup.hStdOutput = writer;
    startup.hStdError = null;
    PROCESS_INFORMATION process{};
    std::string command;
    for (const auto &arg : args) command += (command.empty() ? "" : " ") + QuoteArgument(arg);
    const BOOL started = CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                       nullptr, cwd.empty() ? nullptr : cwd.c_str(), &startup, &process);
    CloseHandle(writer);
    if (null != INVALID_HANDLE_VALUE) CloseHandle(null);
    if (!started) { CloseHandle(reader); return {}; }
    bool failed = false;
    for (;;) {
        if (std::chrono::steady_clock::now() >= deadline) { failed = true; break; }
        DWORD available = 0;
        if (!PeekNamedPipe(reader, nullptr, 0, nullptr, &available, nullptr)) break;
        if (available != 0) {
            char buffer[4096];
            DWORD got = 0;
            if (!ReadFile(reader, buffer, std::min<DWORD>(available, sizeof(buffer)), &got, nullptr)) break;
            out.append(buffer, got);
            if (out.size() > kMaxHelpBytes) { failed = true; break; }
        } else if (WaitForSingleObject(process.hProcess, 10) == WAIT_OBJECT_0) break;
    }
    if (failed || WaitForSingleObject(process.hProcess, 0) != WAIT_OBJECT_0) {
        failed = true;
        TerminateProcess(process.hProcess, 1);
    }
    WaitForSingleObject(process.hProcess, 1000);
    DWORD status = 1;
    GetExitCodeProcess(process.hProcess, &status);
    CloseHandle(reader);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return failed || status != 0 ? std::string() : out;
#else
    int fds[2];
    if (pipe(fds) != 0) return {};
    std::vector<char *> argv;
    for (const auto &arg : args) argv.push_back(const_cast<char *>(arg.c_str()));
    argv.push_back(nullptr);
    const pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return {}; }
    if (pid == 0) {
        setpgid(0, 0);
        const int null = open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, STDIN_FILENO); dup2(null, STDERR_FILENO); }
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(127);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(fds[1]);
    bool failed = false;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) { failed = true; break; }
        pollfd fd{fds[0], POLLIN, 0};
        const int ready = poll(&fd, 1, static_cast<int>(left.count()));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) { failed = true; break; }
        char buffer[4096];
        const ssize_t got = read(fds[0], buffer, sizeof(buffer));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        out.append(buffer, static_cast<size_t>(got));
        if (out.size() > kMaxHelpBytes) { failed = true; break; }
    }
    close(fds[0]);
    int status = 0;
    // Bound the wait as well as the read, including a child that closes stdout.
    while (!failed) {
        const pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? out : std::string();
        if (done < 0 && errno != EINTR) return {};
        if (std::chrono::steady_clock::now() >= deadline) { failed = true; break; }
        poll(nullptr, 0, 10);
    }
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return {};
#endif
}
}  // namespace

std::string RLspInstalledHelp(const RLspHoverInfo &info, const std::string &cwd) {
    if (info.help_topic.empty()) return {};
    const char *configured = std::getenv("MEP_RSCRIPT");
    const std::string rscript = configured != nullptr && configured[0] != '\0' ? configured : "Rscript";
    std::vector<std::string> args{rscript, "--vanilla", "-e", kHelpScript, info.help_topic, info.help_package};
    args.insert(args.end(), info.help_packages.begin(), info.help_packages.end());
    return RunHelp(args, cwd);
}
