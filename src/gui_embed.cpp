#include "gui_embed.h"

#include <algorithm>
#include <cstdlib>
#if defined(__linux__)
#include <dirent.h>  // ProcessTree's /proc walk, below -- Linux only
#endif
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>  // ProcessTree's process-table snapshot
#endif
#include <fstream>
#include <map>
#include <sstream>

#include "job.h"

// The platform-free half of gui_embed.h: the factory that picks an
// implementor, the implementor for platforms without one, and
// EmbeddedApp, the abstraction everything else talks to.

namespace mep::gui_embed {

namespace {

// --- The implementor for everything that cannot embed yet -------------------

class UnsupportedBackend final : public Backend {
public:
    explicit UnsupportedBackend(std::string why) : why_(std::move(why)) {}
    std::string Name() const override { return "unsupported"; }
    bool Usable(std::string *why) const override {
        if (why) *why = why_;
        return false;
    }
    std::vector<std::pair<std::string, std::string>> ChildEnv() const override { return {}; }
    void NoteExistingWindows() override {}
    std::unique_ptr<EmbeddedWindow> Adopt(const std::vector<int> &, bool) override { return nullptr; }
    void Pump() override {}
    void Flush() override {}

private:
    std::string why_;
};

// How long a program may take to open its window before the block says it
// has none (it keeps running; a late window is still adopted).
constexpr double kWindowWaitSec = 20.0;
// How often a shown program's picture is refreshed, for the results it
// leaves behind when it closes itself -- its window is gone by then, so
// the last picture taken is the one kept; often enough that it is the
// frame the user last saw.
constexpr double kSnapshotEverySec = 0.25;
// Lines of stdout/stderr kept.
constexpr size_t kKeepLines = 200;

void Keep(std::vector<std::string> &lines, const std::string &line) {
    lines.push_back(line);
    if (lines.size() > kKeepLines) lines.erase(lines.begin());
}

}  // namespace

std::unique_ptr<Backend> CreateUnsupportedBackend(std::string why) { return std::make_unique<UnsupportedBackend>(std::move(why)); }

std::unique_ptr<Backend> CreateBackend(void *native_window_handle) {
    if (native_window_handle == nullptr) return CreateUnsupportedBackend("there is no window to embed a program into (a headless run)");
#if defined(MEP_GUI_EMBED_X11)
    std::string why;
    if (std::unique_ptr<Backend> x11 = CreateX11Backend(native_window_handle, &why)) return x11;
    return CreateUnsupportedBackend(why);
#elif defined(MEP_GUI_EMBED_MACOS)
    std::string why;
    if (std::unique_ptr<Backend> mac = CreateMacOSBackend(native_window_handle, &why)) return mac;
    return CreateUnsupportedBackend(why);
#elif defined(__EMSCRIPTEN__)
    return CreateUnsupportedBackend("a browser tab cannot hold another program's window");
#else
    return CreateUnsupportedBackend("embedding a program's window is not implemented on this platform yet (only X11 and macOS are)");
#endif
}

std::vector<int> ProcessTree(int pid) {
    std::vector<int> out;
    if (pid <= 0) return out;
    out.push_back(pid);
#if defined(__linux__)
    // parent -> children, from every /proc/<pid>/stat.
    std::multimap<int, int> children;
    if (DIR *d = opendir("/proc")) {
        while (const dirent *e = readdir(d)) {
            const int p = std::atoi(e->d_name);
            if (p <= 0) continue;
            std::ifstream f(std::string("/proc/") + e->d_name + "/stat");
            std::string stat;
            if (!std::getline(f, stat)) continue;
            // "pid (comm) state ppid ...": comm may hold spaces and parens.
            const size_t close = stat.rfind(')');
            if (close == std::string::npos) continue;
            std::istringstream rest(stat.substr(close + 1));
            std::string state;
            int ppid = 0;
            if (rest >> state >> ppid) children.emplace(ppid, p);
        }
        closedir(d);
    }
    for (size_t k = 0; k < out.size(); ++k) {
        auto range = children.equal_range(out[k]);
        for (auto it = range.first; it != range.second; ++it)
            if (std::find(out.begin(), out.end(), it->second) == out.end()) out.push_back(it->second);
    }
#elif defined(__APPLE__)
    // parent -> children, from the kernel's process table (sysctl; libproc's
    // proc_listchildpids is not used: what it returns, a count or a byte
    // size, differs between macOS versions).
    std::multimap<int, int> children;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
    size_t len = 0;
    if (sysctl(mib, 4, nullptr, &len, nullptr, 0) == 0 && len > 0) {
        std::vector<char> buf(len + 64 * sizeof(kinfo_proc));  // room for processes started meanwhile
        len = buf.size();
        if (sysctl(mib, 4, buf.data(), &len, nullptr, 0) == 0) {
            const auto *procs = reinterpret_cast<const kinfo_proc *>(buf.data());
            for (size_t k = 0; k < len / sizeof(kinfo_proc); ++k)
                children.emplace(static_cast<int>(procs[k].kp_eproc.e_ppid), static_cast<int>(procs[k].kp_proc.p_pid));
        }
    }
    for (size_t k = 0; k < out.size(); ++k) {
        auto range = children.equal_range(out[k]);
        for (auto it = range.first; it != range.second; ++it)
            if (it->second > 0 && std::find(out.begin(), out.end(), it->second) == out.end()) out.push_back(it->second);
    }
#elif defined(_WIN32)
    // parent -> children, from a Toolhelp snapshot of the process table --
    // the same shape as the two branches above, just a different source.
    //
    // Worth knowing: a Windows parent PID is not reclaimed-safe. The field
    // keeps pointing at whatever PID created the process even after that
    // process exits, and PIDs are reused, so an unrelated new process can
    // appear to be a child of one of ours. The walk below is only ever
    // used to clean up a tree mep itself started moments earlier, which
    // makes the window for that narrow -- but it is not zero, and this is
    // the reason the result should not be treated as authoritative.
    std::multimap<int, int> children;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                children.emplace(static_cast<int>(entry.th32ParentProcessID), static_cast<int>(entry.th32ProcessID));
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
    for (size_t k = 0; k < out.size(); ++k) {
        auto range = children.equal_range(out[k]);
        for (auto it = range.first; it != range.second; ++it)
            if (it->second > 0 && std::find(out.begin(), out.end(), it->second) == out.end()) out.push_back(it->second);
    }
#endif
    return out;
}

// --- EmbeddedApp --------------------------------------------------------------

struct EmbeddedApp::JobState {
    bool exited = false;
    int exit_code = 0;
    std::vector<std::string> out, err;
};

EmbeddedApp::~EmbeddedApp() {
    window_.reset();
    if (job_id_ > 0 && !exited_) JobManager::Instance().KillHard(job_id_);
}

bool EmbeddedApp::Start(const std::vector<std::string> &argv, const std::string &cwd, std::string *error) {
    std::string why;
    if (!backend_.Usable(&why)) {
        if (error) *error = "Cannot show a program's window here: " + why;
        return false;
    }
    if (argv.empty()) {
        if (error) *error = "Nothing to run";
        return false;
    }
#if defined(__EMSCRIPTEN__)
    (void)cwd;
    if (error) *error = "Running a program needs the desktop build";
    return false;
#else
    auto state = std::make_shared<JobState>();
    job_state_ = state;
    JobManager::Callbacks cb;
    cb.on_stdout = [state](const std::string &line) { Keep(state->out, line); };
    cb.on_stderr = [state](const std::string &line) { Keep(state->err, line); };
    cb.on_exit = [state](int code) {
        state->exited = true;
        state->exit_code = code;
    };
    backend_.NoteExistingWindows();
    // (No pty to hang up when mep goes: the program is tied to mep instead.)
    job_id_ = JobManager::Instance().Spawn(argv, cwd, std::move(cb), /*use_pty=*/false, backend_.ChildEnv(),
                                           /*die_with_parent=*/true);
    if (job_id_ == 0) {
        if (error) *error = "Could not start " + argv.front();
        job_state_.reset();
        return false;
    }
    pid_ = JobManager::Instance().Pid(job_id_);
    state_ = State::Starting;
    return true;
#endif
}

void EmbeddedApp::Tick(double now, bool allow_unowned) {
    if (started_at_ < 0) started_at_ = now;
    if (job_state_) {
        out_ = job_state_->out;
        err_ = job_state_->err;
        if (job_state_->exited && !exited_) {
            exited_ = true;
            exit_code_ = job_state_->exit_code;
        }
    }
    if (exited_) {
        window_.reset();
        focused_ = false;
        state_ = State::Exited;
        return;
    }
    if (!window_) {
        if (pid_ > 0) window_ = backend_.Adopt(ProcessTree(pid_), allow_unowned);
        if (window_) {
            window_->SetPointerThrough(pointer_through_);
            state_ = State::Shown;
            title_ = window_->Title();
            last_snapshot_ = -1.0;
        } else if (now - started_at_ > kWindowWaitSec) {
            state_ = State::NoWindow;
        }
        return;
    }
    if (!window_->Alive()) {
        // Closed its window but still running (or about to exit): the
        // picture it last showed is what the results keep.
        window_.reset();
        focused_ = false;
        state_ = State::NoWindow;
        return;
    }
    if (focused_) {
        if (window_->TakeReleaseRequest() || !window_->HasFocus()) {
            window_->Focus(false);
            focused_ = false;
            focus_lost_ = true;
        }
    } else if ((pointer_through_ || backend_.PointerThroughOnly()) && shown_ && window_->HasFocus()) {
        // Clicked into (the pointer reaches it directly): it has taken the
        // keyboard, so it is focused -- Ctrl-\ or a click outside gives it back.
        window_->Focus(true);
        focused_ = true;
        focus_gained_ = true;
    }
    // (What was on screen -- a window clipped by the pane's edge captures
    // as the part that showed, which TakeSnapshot keeps only while it has
    // nothing whole.)
    if (shown_ && (last_snapshot_ < 0 || now - last_snapshot_ >= kSnapshotEverySec)) {
        TakeSnapshot();
        last_snapshot_ = now;
    }
}

void EmbeddedApp::TakeSnapshot() {
    if (!window_) return;
    Snapshot s = window_->Capture();
    // A window the pane's edge cuts off captures as a piece of itself. That
    // piece may be all there is to show, but it must not replace a whole
    // picture already taken: an export saves whatever is kept here, and a
    // scroll just before it would otherwise leave a sliver of the window.
    const bool worse = s.clipped && !snapshot_.Empty() && !snapshot_.clipped;
    if (!s.Empty() && !worse) snapshot_ = std::move(s);
    const std::string t = window_->Title();
    if (!t.empty()) title_ = t;
}

void EmbeddedApp::Place(const Rect &full, const Rect &clip) {
    if (!window_) return;
    placed_ = true;
    window_->Place(full, clip);
}

void EmbeddedApp::EndFrame() {
    shown_ = placed_;
    if (!window_) {
        placed_ = false;
        return;
    }
    if (!placed_) {
        window_->Hide();
        if (focused_) {
            // Off screen: nothing to type into.
            window_->Focus(false);
            focused_ = false;
            focus_lost_ = true;
        }
    }
    placed_ = false;
}

bool EmbeddedApp::Focus(bool on) {
    if (!window_) return false;
    if (on == focused_) return true;
    window_->Focus(on);
    focused_ = on;
    return true;
}

void EmbeddedApp::SetPointerThrough(bool on) {
    pointer_through_ = on;
    if (window_) window_->SetPointerThrough(on);
}

bool EmbeddedApp::TakeFocusGained() {
    const bool gained = focus_gained_;
    focus_gained_ = false;
    return gained;
}

bool EmbeddedApp::TakeFocusLost() {
    const bool lost = focus_lost_;
    focus_lost_ = false;
    return lost;
}

void EmbeddedApp::Stop() {
    if (exited_ || job_id_ <= 0) return;
    if (stops_++ == 0) {
        // Its picture as it was when stopped, then a close request (the
        // program may ask about unsaved work), else SIGTERM.
        TakeSnapshot();
        if (!window_ || !window_->RequestClose()) JobManager::Instance().Kill(job_id_);
    } else if (stops_ == 2) {
        JobManager::Instance().Kill(job_id_);
    } else {
        JobManager::Instance().KillHard(job_id_);
    }
}

}  // namespace mep::gui_embed
