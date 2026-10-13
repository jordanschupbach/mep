// gui_embed.h's WebView on X11: the page lives in mep-webview
// (webview_gtk_main.cpp), a WebKitGTK program of its own, whose window the
// X11 backend adopts into the pane exactly as it adopts any program's
// (gui_embed_x11.cpp). What :Browser shows on Linux.
//
// Why a separate process, where macOS puts a WKWebView straight into mep's
// window: WebKitGTK is a GTK 4 widget, and GTK 4 brings its own main loop
// and its own GL renderer -- sharing mep's thread with its GLX drawing
// would mean pumping GLib every frame and taking the GL context back after
// each pump, and whatever WebKit's own rendering needs of its environment
// (software GL on some machines, the DMA-BUF renderer off) could not be
// set without setting it for mep too. A process costs an X window adoption
// instead, which the backend already does for every :exec pane.
//
// So this object is two things joined: the adopted X11 window (`inner_`,
// which gets Place, Hide, Focus, HasFocus, the Ctrl-\ release) and a pipe
// to the program (Load, Back, the theme...). Until the window shows --
// the program takes a moment to start -- it is alive with nothing on
// screen, and what was asked of it (the keyboard, the address bar) is
// kept to be done once it has. The protocol is webview_gtk_main.cpp's
// header. mep polls the program's lines, and looks for its window, from
// Alive(), which EmbeddedApp::Tick asks every frame.

#include "gui_embed.h"

#if defined(MEP_GUI_EMBED_X11)

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include "job.h"

namespace mep::gui_embed {
namespace {

constexpr const char *kHelperName = "mep-webview";

// mep-webview beside the running mep ($MEP_WEBVIEW_HELPER first, a build
// or install always puts it there), else on PATH; "" when it is nowhere.
std::string FindHelper() {
    if (const char *env = std::getenv("MEP_WEBVIEW_HELPER"); env && *env) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(env, ec)) return env;
    }
    char buf[4096];
    const ssize_t len = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (len > 0) {
        std::error_code ec;
        const std::filesystem::path sibling = std::filesystem::path(std::string(buf, static_cast<size_t>(len))).parent_path() / kHelperName;
        if (std::filesystem::is_regular_file(sibling, ec) && access(sibling.c_str(), X_OK) == 0) return sibling.string();
    }
    return FindProgram(kHelperName);
}

std::string Hex(WebView::Rgb c) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", c.r, c.g, c.b);
    return buf;
}

std::string OneLine(std::string s) {
    s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == '\n' || c == '\r'; }), s.end());
    return s;
}

class HelperWebView final : public WebView {
public:
    explicit HelperWebView(Backend &be) : be_(be) {}

    ~HelperWebView() override {
        inner_.reset();
        if (job_id_ > 0 && !state_->exited) {
            Send("close");
            JobManager::Instance().CloseStdin(job_id_);
            // (It quits on its own on `close` or a closed pipe; this is for
            // one that is stuck.)
            JobManager::Instance().Kill(job_id_);
        }
        if (pid_ > 0) be_.ChildEnded(pid_);
    }

    bool Start(const std::string &helper, std::string *why) {
        std::string reason;
        if (!be_.Usable(&reason)) {
            if (why) *why = "cannot show a web page here: " + reason;
            return false;
        }
        JobManager::Callbacks cb;
        std::shared_ptr<State> st = state_;
        cb.on_stdout = [st](const std::string &line) { st->lines.push_back(line); };
        cb.on_stderr = [](const std::string &line) { std::fprintf(stderr, "%s: %s\n", kHelperName, line.c_str()); };
        cb.on_exit = [st](int code) {
            st->exited = true;
            st->exit_code = code;
        };
        be_.NoteExistingWindows();
        char cwd[4096];
        const std::string dir = getcwd(cwd, sizeof cwd) ? cwd : ".";
        job_id_ = JobManager::Instance().Spawn({helper}, dir, std::move(cb), /*use_pty=*/false, be_.ChildEnv(),
                                               /*die_with_parent=*/true);
        if (job_id_ == 0 || state_->exited) {
            if (why) *why = "could not start " + helper;
            return false;
        }
        pid_ = JobManager::Instance().Pid(job_id_);
        be_.ChildStarted(pid_);
        return true;
    }

    // --- EmbeddedWindow ---------------------------------------------------

    bool Alive() override {
        Poll();
        return !state_->exited && (!inner_ || inner_->Alive());
    }

    void Place(const Rect &full, const Rect &clip) override {
        if (!inner_) return;
        inner_->Place(full, clip);
        // The X input focus can only go to a window that is on screen:
        // what Focus asked for is done here, once it is.
        if (focus_pending_) {
            focus_pending_ = false;
            inner_->Focus(true);
        }
    }

    void Hide() override {
        if (inner_) inner_->Hide();
    }

    void Focus(bool on) override {
        if (on) {
            focus_pending_ = true;
        } else {
            focus_pending_ = false;
            if (inner_) inner_->Focus(false);
        }
    }

    void SetPointerThrough(bool on) override {
        pointer_through_ = on;
        if (inner_) inner_->SetPointerThrough(on);
    }

    // (While a Focus(true) waits for the window to be on screen, it counts
    // as held: the keyboard is not lost before the window could take it.)
    bool HasFocus() override { return focus_pending_ || (inner_ && inner_->HasFocus()); }

    bool TakeReleaseRequest() override { return inner_ && inner_->TakeReleaseRequest(); }

    bool RequestClose() override {
        if (state_->exited) return false;
        Send("close");
        return true;
    }

    // (An :exec pane keeps no pictures.)
    Snapshot Capture() override { return {}; }

    std::string Title() override { return title_; }

    // --- WebView ------------------------------------------------------------

    void Load(const std::string &target) override { Send("load " + OneLine(target)); }
    void Back() override { Send("back"); }
    void Forward() override { Send("forward"); }
    void Reload() override { Send("reload"); }
    // The address bar takes the keyboard -- which, the bar being the
    // program's, means the program does (as on macOS, where the field
    // becoming first responder is the page holding the keyboard).
    void EditAddress() override {
        Send("address");
        focus_pending_ = true;
    }
    std::string Url() override { return url_; }

    void SetTheme(const Theme &t) override {
        std::string line = std::string("theme recolor=") + (t.recolor ? "1" : "0") + " dark=" + (t.dark ? "1" : "0");
        const std::pair<const char *, Rgb> colors[] = {{"bg", t.bg},         {"fg", t.fg},         {"dim", t.dim},
                                                       {"selection", t.selection}, {"border", t.border}, {"bar", t.bar},
                                                       {"red", t.red},       {"green", t.green},   {"yellow", t.yellow},
                                                       {"blue", t.blue},     {"purple", t.purple}, {"cyan", t.cyan},
                                                       {"orange", t.orange}};
        for (const auto &kv : colors) line += std::string(" ") + kv.first + "=" + Hex(kv.second);
        Send(line);
    }

private:
    struct State {
        bool exited = false;
        int exit_code = 0;
        std::vector<std::string> lines;
    };

    void Send(const std::string &line) {
        if (job_id_ <= 0 || state_->exited) return;
        JobManager::Instance().WriteStdin(job_id_, line + "\n");
    }

    // The program's lines since last time, and its window once it has one.
    void Poll() {
        for (const std::string &line : state_->lines) {
            const size_t sp = line.find(' ');
            const std::string kind = line.substr(0, sp);
            const std::string value = sp == std::string::npos ? "" : line.substr(sp + 1);
            if (kind == "url") url_ = value;
            else if (kind == "title") title_ = value;
        }
        state_->lines.clear();
        if (inner_ || state_->exited || pid_ <= 0) return;
        inner_ = be_.Adopt(ProcessTree(pid_), /*allow_unowned=*/false);
        if (!inner_) return;
        inner_->SetPointerThrough(pointer_through_);
    }

    Backend &be_;
    std::shared_ptr<State> state_ = std::make_shared<State>();
    std::unique_ptr<EmbeddedWindow> inner_;
    int job_id_ = 0;
    int pid_ = -1;
    bool pointer_through_ = false;
    bool focus_pending_ = false;
    std::string url_, title_;
};

}  // namespace

std::unique_ptr<WebView> CreateWebView(Backend &backend, void *, std::function<bool(const std::string &, unsigned)>, std::string *why) {
    // (The reserved chords: X11 keeps none from a program -- the backend
    // has no way to -- so none are kept from the page either.)
    const std::string helper = FindHelper();
    if (helper.empty()) {
        if (why)
            *why = std::string(kHelperName) +
                   " was not found beside mep or on PATH: it is built with mep when webkitgtk-6.0 and gtk4 are installed";
        return nullptr;
    }
    auto view = std::make_unique<HelperWebView>(backend);
    if (!view->Start(helper, why)) return nullptr;
    return view;
}

}  // namespace mep::gui_embed

#endif  // MEP_GUI_EMBED_X11
