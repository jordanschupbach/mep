// Real ConPTY integration checks. Run with a process timeout in CI.
#include "job.h"
#include "vterm.h"
#include <windows.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
int failures = 0;
void Check(bool ok, const char *what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}
std::string Screen(const VTerm &term) {
    std::string text;
    for (int r = 0; r < term.Rows(); ++r) {
        for (int c = 0; c < term.Cols(); ++c) text += term.At(r, c).ch;
        text += '\n';
    }
    return text;
}
bool Await(Job &job, VTerm &term, const std::string &text) {
    const auto deadline = Clock::now() + std::chrono::seconds(8);
    do {
        for (const auto &chunk : job.DrainRaw()) {
            auto reply = term.Feed(chunk);
            if (!reply.empty()) job.WriteStdin(reply);
        }
        if (Screen(term).find(text) != std::string::npos) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (Clock::now() < deadline);
    std::fprintf(stderr, "Child finished=%d exit=%d\n", job.Finished(), job.ExitCode());
    std::fprintf(stderr, "Wanted %s, screen:\n%s\n", text.c_str(), Screen(term).c_str());
    return false;
}
int Child(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    DWORD mode = 0;
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleMode(output, &mode)) return 90;
    SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    if (argc > 2 && std::string(argv[2]) == "--flood") {
        for (;;) { std::cout << std::string(2048, 'x') << std::endl; }
    }
    std::cout << "\x1b[32mREADY\x1b[0m\n";
    if (const char *env = std::getenv("MEP_PTY_TEST")) std::cout << "ENV=" << env << '\n';
    std::cout << "CWD=" << std::filesystem::current_path().filename().string() << '\n';
    for (int i = 2; i < argc; ++i) std::cout << "ARG" << i << "=" << argv[i] << "!\n";
    std::cout.flush();
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "size") {
            CONSOLE_SCREEN_BUFFER_INFO info{};
            GetConsoleScreenBufferInfo(output, &info);
            std::cout << "SIZE=" << info.dwSize.X << 'x' << info.dwSize.Y << std::endl;
        } else if (line == "grandchild") {
            char own[MAX_PATH]; GetModuleFileNameA(nullptr, own, MAX_PATH);
            std::string command = "\"" + std::string(own) + "\" --child";
            STARTUPINFOA startup{}; startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            if (CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
                std::cout << "GRANDCHILD=" << process.dwProcessId << std::endl;
                CloseHandle(process.hThread); CloseHandle(process.hProcess);
            }
        } else if (line == "quit") { std::cout << "FINAL_OUTPUT" << std::endl; return 7; }
        else { std::cout << "INPUT=" << line << std::endl; }
    }
    return 0;
}
}
int main(int argc, char **argv) {
    if (argc > 1 && std::string(argv[1]) == "--child") return Child(argc, argv);
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const auto folder = std::filesystem::temp_directory_path() / ("mep pty test " + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(folder);
    {
        Job job({exe, "--child", "two words", "", "quote\"here", "trailing\\"}, folder.string(), true, true,
                {{"MEP_PTY_TEST", "child-only"}, {"TERM", "xterm-256color"}});
        Check(!job.SpawnFailed(), "ConPTY child starts");
        VTerm term(24, 80);
        Check(Await(job, term, "ARG5=trailing\\!"), "argv quoting preserves trailing slash");
        auto screen = Screen(term);
        Check(screen.find("ENV=child-only") != std::string::npos, "child receives environment override");
        Check(screen.find("CWD=" + folder.filename().string()) != std::string::npos, "child uses requested cwd");
        Check(screen.find("ARG2=two words!") != std::string::npos, "argument containing spaces");
        Check(screen.find("ARG3=!") != std::string::npos, "empty argument");
        Check(screen.find("ARG4=quote\"here!") != std::string::npos, "quoted argument");
        Check(std::getenv("MEP_PTY_TEST") == nullptr, "parent environment unchanged");
        Check(job.WriteStdin("hello\r"), "input accepted");
        Check(Await(job, term, "INPUT=hello"), "input reaches child");
        job.ResizePty(100, 35); term.Resize(35, 100);
        job.WriteStdin("size\r");
        Check(Await(job, term, "SIZE=100x35"), "resize reaches child console");
        job.WriteStdin("quit\r");
        Check(Await(job, term, "FINAL_OUTPUT"), "last output reaches VTerm before exit");
        auto end = Clock::now() + std::chrono::seconds(5);
        while (!job.Finished() && Clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        Check(job.Finished() && job.ExitCode() == 7, "exit status is collected");
    }
    {
        const char *comspec = std::getenv("COMSPEC");
        Job shell({comspec ? comspec : "cmd.exe"}, folder.string(), true, true);
        VTerm term(24, 80);
        Check(shell.WriteStdin("@echo SHELL_^WORKS\r"), "interactive shell input");
        Check(Await(shell, term, "SHELL_WORKS"), "interactive cmd.exe renders");
        shell.WriteStdin("exit\r");
    }
    {
        Job command({"cmd.exe", "/d", "/s", "/c", "echo COMMAND_WORKS & exit /b 3"}, folder.string(), true, true);
        VTerm term(24, 80);
        Check(Await(command, term, "COMMAND_WORKS"), ":term command uses cmd syntax");
    }
    {
        Job powershell({"powershell.exe", "-NoLogo", "-NoProfile", "-Command", "Write-Output POWERSHELL_WORKS"}, "", true, true);
        VTerm term(24, 80);
        Check(Await(powershell, term, "POWERSHELL_WORKS"), "PowerShell executes inside ConPTY");
    }
    {
        Job bad({"mep-no-such-terminal-program.exe"}, "", true, true);
        Check(bad.SpawnFailed() && bad.Finished(), "failed executable spawn is reported");
        Job badcwd({"cmd.exe"}, (folder / "missing").string(), true, true);
        Check(badcwd.SpawnFailed(), "invalid cwd is reported");
    }
    {
        Job job({exe, "--child"}, "", true, true);
        VTerm term(24, 80);
        Check(Await(job, term, "READY"), "parent console starts");
        job.WriteStdin("grandchild\r");
        if (Await(job, term, "GRANDCHILD=")) {
            const auto screen = Screen(term);
            const auto start = screen.find("GRANDCHILD=") + 11;
            const DWORD pid = static_cast<DWORD>(std::stoul(screen.substr(start)));
            HANDLE child = OpenProcess(SYNCHRONIZE, FALSE, pid);
            Check(child != nullptr, "grandchild starts");
            job.Kill();
            if (child) {
                Check(WaitForSingleObject(child, 3000) == WAIT_OBJECT_0, "kill reaches console grandchildren");
                CloseHandle(child);
            }
        } else Check(false, "grandchild pid is reported");
    }
    {
        auto job = std::make_unique<Job>(std::vector<std::string>{exe, "--child", "--flood"}, "", true, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        // Queue more input than a non-reading child can consume and leave the
        // output undrained: both pipe directions must still tear down promptly.
        job->WriteStdin(std::string(2 * 1024 * 1024, 'a'));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto start = Clock::now(); job.reset();
        Check(Clock::now() - start < std::chrono::seconds(3), "backlogged input/output teardown stays prompt");
    }
    std::filesystem::remove(folder);
    std::printf("ConPTY integration: %d failures\n", failures);
    return failures ? 1 : 0;
}
