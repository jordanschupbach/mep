// Windows terminal transport. Included only by job.cpp so Windows handles
// and SDK headers stay out of job.h and the editor's other translation units.
#include <windows.h>
#include <map>

namespace {
void FeedChunk(std::string &partial, const char *data, size_t len, bool is_stderr, std::mutex &mu,
               std::deque<JobLine> &pending);
std::wstring TerminalWide(const std::string &s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (!n) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

// CreateProcess takes one command line. Escape each argv element using the
// Windows CRT rules, including empty arguments and backslashes before quotes.
std::wstring TerminalQuote(const std::wstring &s) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : s) {
        if (c == L'\\') { ++slashes; continue; }
        out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L'"';
}
struct TerminalEnvLess {
    bool operator()(const std::wstring &a, const std::wstring &b) const {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    }
};
void TerminalCloseHandle(HANDLE &h) {
    if (h) CloseHandle(h);
    h = nullptr;
}
}  // namespace

struct Job::WindowsPty {
    HANDLE process = nullptr, tree = nullptr, input = nullptr, output = nullptr, error = nullptr;
    HPCON console = nullptr;
    std::mutex console_mu;
    std::thread writer;
    std::atomic<bool> writer_done{false};
    std::atomic<bool> stdin_closing{false};
    ~WindowsPty() {
        // Close output before the pseudoconsole: ClosePseudoConsole can emit
        // a final frame and would otherwise wait forever for an absent reader.
        TerminalCloseHandle(output);
        TerminalCloseHandle(error);
        TerminalCloseHandle(input);
        if (console) ClosePseudoConsole(console);
        TerminalCloseHandle(process);
        TerminalCloseHandle(tree);
    }
};

void Job::SpawnWindowsPty(const std::vector<std::string> &argv, const std::string &cwd) {
    // Pipe-based jobs have a separate protocol (distinct stderr and EOF).
    // Only terminal jobs go through ConPTY; never send VT framing to an LSP.
    auto fail = [this] { spawn_failed_ = true; finished_ = true; };
    if (!use_pty_ || argv.empty() || argv[0].empty()) { fail(); return; }
    auto state = std::make_unique<WindowsPty>();
    struct PipeEnds {
        HANDLE input = nullptr, output = nullptr;
        ~PipeEnds() { TerminalCloseHandle(input); TerminalCloseHandle(output); }
    } ends;
    HANDLE &child_input = ends.input, &child_output = ends.output;
    if (!CreatePipe(&child_input, &state->input, nullptr, 65536) ||
        !CreatePipe(&state->output, &child_output, nullptr, 65536)) {
        TerminalCloseHandle(child_input); TerminalCloseHandle(child_output);
        fail(); return;
    }
    HRESULT hr = CreatePseudoConsole(COORD{80, 24}, child_input, child_output, 0, &state->console);
    if (FAILED(hr)) { fail(); return; }

    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> storage(bytes);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    // Do not copy mep's redirected standard handles into the console child.
    // Null handles are populated from its new console during attachment.
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes)) { fail(); return; }
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                  state->console, sizeof(HPCON), nullptr, nullptr)) {
        DeleteProcThreadAttributeList(startup.lpAttributeList); fail(); return;
    }

    std::map<std::wstring, std::wstring, TerminalEnvLess> env;
    if (wchar_t *block = GetEnvironmentStringsW()) {
        for (const wchar_t *p = block; *p; p += wcslen(p) + 1) {
            std::wstring entry(p);
            size_t eq = entry.find(L'=', entry[0] == L'=' ? 1 : 0);
            if (eq != std::wstring::npos) env[entry.substr(0, eq)] = entry.substr(eq + 1);
        }
        FreeEnvironmentStringsW(block);
    }
    for (const auto &[key, value] : extra_env_) env[TerminalWide(key)] = TerminalWide(value);
    std::vector<wchar_t> environment;
    for (const auto &[key, value] : env) {
        const std::wstring entry = key + L'=' + value;
        environment.insert(environment.end(), entry.begin(), entry.end());
        environment.push_back(0);
    }
    environment.push_back(0);
    std::wstring command;
    for (const auto &arg : argv) {
        if (!command.empty()) command += L' ';
        command += TerminalQuote(TerminalWide(arg));
    }
    // cmd.exe parses /c itself, rather than using CRT argv unescaping.
    // Preserve the command string verbatim inside the /s /c outer quotes.
    const auto executable = TerminalWide(argv[0]);
    const size_t slash = executable.find_last_of(L"/\\");
    const auto basename = executable.substr(slash == std::wstring::npos ? 0 : slash + 1);
    if (_wcsicmp(basename.c_str(), L"cmd.exe") == 0 && argv.size() == 5 && argv[3] == "/c")
        command = TerminalQuote(executable) + L" /d /s /c \"" + TerminalWide(argv[4]) + L"\"";
    const std::wstring directory = TerminalWide(cwd);
    PROCESS_INFORMATION process{};
    BOOL ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
        environment.data(), cwd.empty() ? nullptr : directory.c_str(), &startup.StartupInfo, &process);
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    TerminalCloseHandle(child_input); TerminalCloseHandle(child_output);
    if (!ok) { fail(); return; }
    state->process = process.hProcess;
    // Own the tree, including grandchildren; closing a pane must not leave a
    // shell's foreground program alive. ConPTY sessions also die with mep.
    state->tree = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!state->tree || !SetInformationJobObject(state->tree, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
        !AssignProcessToJobObject(state->tree, state->process)) {
        TerminateProcess(state->process, 1); CloseHandle(process.hThread); fail(); return;
    }
    pid_ = static_cast<pid_t>(process.dwProcessId);
    windows_ = std::move(state);
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    windows_->writer = std::thread(&Job::WriteWindowsPty, this);
    reader_thread_ = std::thread(&Job::ReadWindowsPty, this);
}

void Job::SpawnWindowsPipes(const std::vector<std::string> &argv, const std::string &cwd) {
    auto fail = [this] { spawn_failed_ = true; finished_ = true; };
    if (argv.empty() || argv[0].empty()) { fail(); return; }
    auto state = std::make_unique<WindowsPty>();
    struct ChildHandles {
        HANDLE input = nullptr, output = nullptr, error = nullptr;
        ~ChildHandles() { TerminalCloseHandle(input); TerminalCloseHandle(output); TerminalCloseHandle(error); }
    } child;
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    if (!CreatePipe(&child.input, &state->input, &security, 65536) ||
        !CreatePipe(&state->output, &child.output, &security, 65536) ||
        !CreatePipe(&state->error, &child.error, &security, 65536) ||
        !SetHandleInformation(state->input, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(state->output, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(state->error, HANDLE_FLAG_INHERIT, 0)) { fail(); return; }

    // Only these three handles may cross into the child. Other concurrent
    // jobs must not inherit pipe ends and keep one another's EOF alive.
    HANDLE handles[]{child.input, child.output, child.error};
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> storage(bytes);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = child.input;
    startup.StartupInfo.hStdOutput = child.output;
    startup.StartupInfo.hStdError = child.error;
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes)) { fail(); return; }
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                  handles, sizeof(handles), nullptr, nullptr)) {
        DeleteProcThreadAttributeList(startup.lpAttributeList); fail(); return;
    }
    std::map<std::wstring, std::wstring, TerminalEnvLess> env;
    if (wchar_t *block = GetEnvironmentStringsW()) {
        for (const wchar_t *p = block; *p; p += wcslen(p) + 1) {
            const std::wstring entry(p);
            const size_t eq = entry.find(L'=', entry[0] == L'=' ? 1 : 0);
            if (eq != std::wstring::npos) env[entry.substr(0, eq)] = entry.substr(eq + 1);
        }
        FreeEnvironmentStringsW(block);
    }
    for (const auto &[key, value] : extra_env_) env[TerminalWide(key)] = TerminalWide(value);
    std::vector<wchar_t> environment;
    for (const auto &[key, value] : env) {
        const auto entry = key + L'=' + value;
        environment.insert(environment.end(), entry.begin(), entry.end());
        environment.push_back(0);
    }
    environment.push_back(0);
    std::wstring command;
    for (const auto &arg : argv) {
        if (!command.empty()) command += L' ';
        command += TerminalQuote(TerminalWide(arg));
    }
    const auto directory = TerminalWide(cwd);
    PROCESS_INFORMATION process{};
    const BOOL ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | CREATE_NO_WINDOW,
        environment.data(), cwd.empty() ? nullptr : directory.c_str(), &startup.StartupInfo, &process);
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (!ok) { fail(); return; }
    state->process = process.hProcess;
    state->tree = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!state->tree || !SetInformationJobObject(state->tree, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
        !AssignProcessToJobObject(state->tree, state->process)) {
        TerminateProcess(state->process, 1); CloseHandle(process.hThread); fail(); return;
    }
    TerminalCloseHandle(child.input); TerminalCloseHandle(child.output); TerminalCloseHandle(child.error);
    pid_ = static_cast<pid_t>(process.dwProcessId);
    windows_ = std::move(state);
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    windows_->writer = std::thread(&Job::WriteWindowsPty, this);
    reader_thread_ = std::thread(&Job::ReadWindowsPipes, this);
}

void Job::ReadWindowsPipes() {
    std::string out_partial, err_partial;
    while (!stopping_.load() && (windows_->output || windows_->error)) {
        bool progress = false;
        for (bool is_error : {false, true}) {
            HANDLE &pipe = is_error ? windows_->error : windows_->output;
            if (!pipe) continue;
            if (!is_error && raw_stdout_) {
                std::lock_guard<std::mutex> lock(mu_);
                if (pending_raw_bytes_ >= kMaxPendingRawBytes) continue;
            }
            DWORD available = 0;
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
                TerminalCloseHandle(pipe); continue;
            }
            if (!available) continue;
            char data[8192];
            DWORD count = 0;
            if (!ReadFile(pipe, data, std::min<DWORD>(available, sizeof(data)), &count, nullptr) || !count) {
                TerminalCloseHandle(pipe); continue;
            }
            if (!is_error && raw_stdout_) {
                std::lock_guard<std::mutex> lock(mu_);
                pending_raw_.emplace_back(data, count);
                pending_raw_bytes_ += count;
            } else {
                FeedChunk(is_error ? err_partial : out_partial, data, count, is_error, mu_, pending_);
            }
            progress = true;
            mep::WakeMainLoop();
        }
        if (!progress) Sleep(5);
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!out_partial.empty()) pending_.push_back({false, out_partial});
        if (!err_partial.empty()) pending_.push_back({true, err_partial});
    }
    if (stopping_.load()) Kill();
    WaitForSingleObject(windows_->process, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(windows_->process, &code);
    exit_code_ = static_cast<int>(code);
    finished_ = true;
    mep::WakeMainLoop();
}

void Job::WriteWindowsPty() {
    while (!stopping_.load() && !finished_.load()) {
        std::string chunk;
        {
            std::lock_guard<std::mutex> lock(stdin_mu_);
            const size_t n = std::min<size_t>(stdin_queue_.size(), 4096);
            chunk = stdin_queue_.substr(0, n);
            // Keep the bytes counted until the blocking write completes.
        }
        if (chunk.empty()) {
            if (windows_->stdin_closing.load()) { TerminalCloseHandle(windows_->input); break; }
            Sleep(5); continue;
        }
        DWORD written = 0;
        if (!WriteFile(windows_->input, chunk.data(), static_cast<DWORD>(chunk.size()), &written, nullptr)) break;
        std::lock_guard<std::mutex> lock(stdin_mu_);
        stdin_queue_.erase(0, written);
    }
    windows_->writer_done = true;
}

void Job::ReadWindowsPty() {
    std::thread closer;
    for (;;) {
        if (stopping_.load()) break;
        if (!closer.joinable() && WaitForSingleObject(windows_->process, 0) == WAIT_OBJECT_0) {
            // Close can emit output, so keep draining concurrently until EOF.
            closer = std::thread([this] {
                std::lock_guard<std::mutex> lock(windows_->console_mu);
                ClosePseudoConsole(windows_->console);
                windows_->console = nullptr;
            });
        }
        bool backlogged;
        { std::lock_guard<std::mutex> lock(mu_); backlogged = pending_raw_bytes_ >= kMaxPendingRawBytes; }
        if (backlogged) { Sleep(5); continue; }
        DWORD available = 0;
        if (!PeekNamedPipe(windows_->output, nullptr, 0, nullptr, &available, nullptr)) break;
        if (!available) { Sleep(5); continue; }
        char data[8192];
        DWORD read = 0;
        if (!ReadFile(windows_->output, data, std::min<DWORD>(available, sizeof(data)), &read, nullptr) || !read) break;
        {
            std::lock_guard<std::mutex> lock(mu_);
            pending_raw_.emplace_back(data, read);
            pending_raw_bytes_ += read;
        }
        mep::WakeMainLoop();
    }
    if (stopping_.load()) {
        Kill();
        TerminalCloseHandle(windows_->output); // release a closer blocked on its final frame
    }
    if (closer.joinable()) closer.join();
    WaitForSingleObject(windows_->process, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(windows_->process, &code);
    exit_code_ = static_cast<int>(code);
    finished_ = true;
    mep::WakeMainLoop();
}

void Job::CloseWindowsPty() {
    if (!windows_) return;
    RequestStop();
    Kill();
    // A paste may have filled the input pipe. Cancel its synchronous writer
    // until it observes stopping_; never join a thread parked in WriteFile.
    while (windows_->writer.joinable() && !windows_->writer_done.load()) {
        CancelSynchronousIo(windows_->writer.native_handle());
        Sleep(1);
    }
    if (windows_->writer.joinable()) windows_->writer.join();
    if (reader_thread_.joinable()) reader_thread_.join();
    windows_.reset();
}
