// Tests for gui_embed.h's abstraction, EmbeddedApp, driven through a fake
// implementor -- the point of the bridge split: everything that decides
// *what* happens to an embedded program (when its window is adopted,
// shown, hidden, focused, released, how it is stopped, what it leaves
// behind) runs here with real child processes but no windowing system.
// The X11 implementor itself is exercised live (help/mepml.org's exec-gui
// section describes what to try).

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "gui_embed.h"
#include "job.h"

namespace {

int g_failures = 0;
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "gui_embed_test:%d: CHECK failed: %s\n", line, expression);
    ++g_failures;
}
#define CHECK(expr) Check(static_cast<bool>(expr), #expr, __LINE__)

using mep::gui_embed::Backend;
using mep::gui_embed::EmbeddedApp;
using mep::gui_embed::EmbeddedWindow;
using mep::gui_embed::Rect;
using mep::gui_embed::Snapshot;

// What the fake window was told to do.
struct WindowLog {
    bool alive = true;
    bool shown = false;
    int places = 0, hides = 0;
    Rect full, clip;
    bool focused = false;
    bool has_focus = false;  // what the "windowing system" says
    bool release_request = false;
    bool takes_close = true;
    int close_requests = 0;
    int captures = 0;
    // What the next Capture() hands back: a whole window by default, a
    // piece of one (the pane's edge cut it off) when `capture_clipped`.
    bool capture_clipped = false;
    int capture_height = 1;
};

class FakeWindow final : public EmbeddedWindow {
public:
    explicit FakeWindow(std::shared_ptr<WindowLog> log) : log_(std::move(log)) {}
    bool Alive() override { return log_->alive; }
    void Place(const Rect &full, const Rect &clip) override {
        ++log_->places;
        log_->shown = true;
        log_->full = full;
        log_->clip = clip;
    }
    void Hide() override {
        ++log_->hides;
        log_->shown = false;
    }
    void Focus(bool on) override {
        log_->focused = on;
        log_->has_focus = on;
    }
    bool HasFocus() override { return log_->has_focus; }
    bool TakeReleaseRequest() override {
        const bool r = log_->release_request;
        log_->release_request = false;
        return r;
    }
    bool RequestClose() override {
        ++log_->close_requests;
        return log_->takes_close;
    }
    Snapshot Capture() override {
        ++log_->captures;
        Snapshot s;
        s.width = 2;
        s.height = log_->capture_height;
        s.clipped = log_->capture_clipped;
        s.rgba.assign(static_cast<size_t>(s.width) * static_cast<size_t>(s.height) * 4, 255);
        return s;
    }
    std::string Title() override { return "fake"; }

private:
    std::shared_ptr<WindowLog> log_;
};

class FakeBackend final : public Backend {
public:
    std::string Name() const override { return "fake"; }
    bool Usable(std::string *) const override { return true; }
    std::vector<std::pair<std::string, std::string>> ChildEnv() const override { return {{"MEP_GUI_EMBED_FAKE", "1"}}; }
    void NoteExistingWindows() override { ++noted; }
    std::unique_ptr<EmbeddedWindow> Adopt(const std::vector<int> &pids, bool allow_unowned) override {
        last_pids = pids;
        last_allow_unowned = allow_unowned;
        if (!window_ready) return nullptr;
        window_ready = false;
        return std::make_unique<FakeWindow>(log);
    }
    void Pump() override {}
    void Flush() override {}

    std::shared_ptr<WindowLog> log = std::make_shared<WindowLog>();
    bool window_ready = false;
    int noted = 0;
    std::vector<int> last_pids;
    bool last_allow_unowned = false;
};

// Runs frames (job polling + Tick) until `done` or ~5 s.
template <typename Done>
bool RunUntil(EmbeddedApp &app, double &now, Done done) {
    for (int k = 0; k < 500; ++k) {
        JobManager::Instance().PollAll();
        now += 0.01;
        app.Tick(now, false);
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

void TestUnsupported() {
    // No window at all (a headless run): the unsupported implementor, with a reason.
    std::unique_ptr<Backend> none = mep::gui_embed::CreateBackend(nullptr);
    std::string why;
    CHECK(none->Name() == "unsupported");
    CHECK(!none->Usable(&why));
    CHECK(why.find("headless") != std::string::npos);
    EmbeddedApp app(*none);
    std::string error;
    CHECK(!app.Start({"true"}, ".", &error));
    CHECK(error.find("Cannot show") != std::string::npos);
}

void TestLifecycle() {
    FakeBackend be;
    EmbeddedApp app(be);
    std::string error;
    // A program that runs until told to stop, printing a line first.
    CHECK(app.Start({"/bin/sh", "-c", "echo hello; echo oops >&2; exec sleep 30"}, ".", &error));
    CHECK(be.noted == 1);
    double now = 0.0;
    // No window yet: it keeps asking, with the program's pid among those it asks about.
    RunUntil(app, now, [&] { return !be.last_pids.empty(); });
    CHECK(app.GetState() == EmbeddedApp::State::Starting);
    CHECK(!be.last_pids.empty());
    CHECK(!app.Focus(true));  // nothing to focus

    // The window appears: adopted.
    be.window_ready = true;
    RunUntil(app, now, [&] { return app.GetState() == EmbeddedApp::State::Shown; });
    CHECK(app.GetState() == EmbeddedApp::State::Shown);
    CHECK(app.Title() == "fake");

    // Placed while drawn; hidden the frame it is not.
    app.Place(Rect{10, 20, 300, 200}, Rect{0, 0, 800, 600});
    app.EndFrame();
    CHECK(be.log->shown && be.log->full == (Rect{10, 20, 300, 200}));
    app.EndFrame();  // a frame that did not draw it
    CHECK(!be.log->shown && be.log->hides == 1);

    // Photographed while on screen (clipped or not), never while hidden,
    // and at most four times a second.
    const int before = be.log->captures;
    now += 5.0;
    app.Tick(now, false);  // hidden last frame
    CHECK(be.log->captures == before);
    app.Place(Rect{10, 20, 300, 200}, Rect{0, 100, 800, 600});  // top clipped
    app.EndFrame();
    now += 5.0;
    app.Tick(now, false);
    CHECK(be.log->captures == before + 1);
    app.Place(Rect{10, 20, 300, 200}, Rect{0, 0, 800, 600});
    app.EndFrame();
    now += 0.1;
    app.Tick(now, false);
    CHECK(be.log->captures == before + 1);
    CHECK(!app.LastSnapshot().Empty());

    // The keyboard: given, then handed back from inside the program.
    CHECK(app.Focus(true) && app.Focused() && be.log->focused);
    be.log->release_request = true;
    app.Tick(now, false);
    CHECK(!app.Focused() && !be.log->focused && app.TakeFocusLost());
    CHECK(!app.TakeFocusLost());  // reported once
    // ... and lost when the user puts the focus elsewhere.
    app.Focus(true);
    be.log->has_focus = false;
    app.Tick(now, false);
    CHECK(!app.Focused() && app.TakeFocusLost());
    // ... and when it goes off screen while focused.
    app.Place(Rect{10, 20, 300, 200}, Rect{0, 0, 800, 600});
    app.EndFrame();
    app.Focus(true);
    app.EndFrame();  // not placed this frame
    CHECK(!app.Focused() && app.TakeFocusLost());

    // Stopping: a close request first. This fake window accepts it but its
    // program ignores it (sleep has no window), so a second stop signals.
    app.Stop();
    CHECK(be.log->close_requests == 1 && app.Stops() == 1);
    CHECK(app.GetState() != EmbeddedApp::State::Exited);
    app.Stop();
    RunUntil(app, now, [&] { return app.GetState() == EmbeddedApp::State::Exited; });
    CHECK(app.GetState() == EmbeddedApp::State::Exited);
    CHECK(app.Stops() == 2);
    CHECK(!app.Stdout().empty() && app.Stdout().front() == "hello");
    CHECK(!app.Stderr().empty() && app.Stderr().front() == "oops");
    CHECK(!app.LastSnapshot().Empty());  // kept after the window is gone
}

void TestWindowClosesFirst() {
    FakeBackend be;
    be.window_ready = true;
    EmbeddedApp app(be);
    std::string error;
    CHECK(app.Start({"/bin/sh", "-c", "exec sleep 30"}, ".", &error));
    double now = 0.0;
    RunUntil(app, now, [&] { return app.GetState() == EmbeddedApp::State::Shown; });
    // The program closes its window but keeps running.
    be.log->alive = false;
    app.Tick(now, false);
    CHECK(app.GetState() == EmbeddedApp::State::NoWindow);
    // A window that takes no close requests: the first stop signals at once.
    app.Stop();
    RunUntil(app, now, [&] { return app.GetState() == EmbeddedApp::State::Exited; });
    CHECK(app.GetState() == EmbeddedApp::State::Exited);
}

void TestExitStatus() {
    FakeBackend be;
    EmbeddedApp app(be);
    std::string error;
    CHECK(app.Start({"/bin/sh", "-c", "exit 3"}, ".", &error));
    double now = 0.0;
    RunUntil(app, now, [&] { return app.GetState() == EmbeddedApp::State::Exited; });
    CHECK(app.GetState() == EmbeddedApp::State::Exited && app.ExitCode() == 3 && app.Stops() == 0);
}

// A window taller than the pane captures as the part that showed. That
// piece must not replace a whole picture already taken: an export saves
// whatever LastSnapshot holds, and a scroll just before it would otherwise
// leave a sliver of the window in the document.
void TestClippedSnapshotKeepsWholePicture() {
    FakeBackend be;
    be.window_ready = true;
    EmbeddedApp app(be);
    std::string error;
    CHECK(app.Start({"/bin/sh", "-c", "exec sleep 30"}, ".", &error));
    double now = 0.0;
    RunUntil(app, now, [&] { return app.GetState() == EmbeddedApp::State::Shown; });

    // Fully on screen: the whole window, 40 rows of it.
    be.log->capture_clipped = false;
    be.log->capture_height = 40;
    app.Place(Rect{0, 0, 2, 40}, Rect{0, 0, 800, 600});
    app.EndFrame();
    now += 5.0;
    app.Tick(now, false);
    CHECK(app.LastSnapshot().height == 40 && !app.LastSnapshot().clipped);

    // Scrolled so only its top shows: the sliver does not take its place.
    be.log->capture_clipped = true;
    be.log->capture_height = 3;
    app.Place(Rect{0, 0, 2, 40}, Rect{0, 0, 800, 3});
    app.EndFrame();
    now += 5.0;
    app.Tick(now, false);
    CHECK(app.LastSnapshot().height == 40 && !app.LastSnapshot().clipped);

    // A fresh whole picture still does.
    be.log->capture_clipped = false;
    be.log->capture_height = 41;
    app.Place(Rect{0, 0, 2, 41}, Rect{0, 0, 800, 600});
    app.EndFrame();
    now += 5.0;
    app.Tick(now, false);
    CHECK(app.LastSnapshot().height == 41);

    app.Stop();
    app.Stop();
    RunUntil(app, now, [&] { return app.GetState() == EmbeddedApp::State::Exited; });
}

// A window that never fit keeps the best piece seen: something to show
// beats nothing.
void TestClippedSnapshotWhenNothingWholeSeen() {
    FakeBackend be;
    be.window_ready = true;
    EmbeddedApp app(be);
    std::string error;
    CHECK(app.Start({"/bin/sh", "-c", "exec sleep 30"}, ".", &error));
    double now = 0.0;
    RunUntil(app, now, [&] { return app.GetState() == EmbeddedApp::State::Shown; });

    be.log->capture_clipped = true;
    be.log->capture_height = 3;
    app.Place(Rect{0, 0, 2, 40}, Rect{0, 0, 800, 3});
    app.EndFrame();
    now += 5.0;
    app.Tick(now, false);
    CHECK(app.LastSnapshot().height == 3 && app.LastSnapshot().clipped);

    // More of it showed: that is the better piece.
    be.log->capture_height = 20;
    app.Place(Rect{0, 0, 2, 40}, Rect{0, 0, 800, 20});
    app.EndFrame();
    now += 5.0;
    app.Tick(now, false);
    CHECK(app.LastSnapshot().height == 20 && app.LastSnapshot().clipped);

    app.Stop();
    app.Stop();
    RunUntil(app, now, [&] { return app.GetState() == EmbeddedApp::State::Exited; });
}

void TestProcessTree() {
    // A shell with a child: both are in the tree, the shell first.
    const int id = JobManager::Instance().Spawn({"/bin/sh", "-c", "sleep 30 & wait"}, ".", {});
    const int pid = JobManager::Instance().Pid(id);
    CHECK(pid > 0);
    std::vector<int> tree;
    for (int k = 0; k < 200 && tree.size() < 2; ++k) {
        tree = mep::gui_embed::ProcessTree(pid);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(tree.size() >= 2 && tree.front() == pid);
    JobManager::Instance().KillHard(id);
    CHECK(mep::gui_embed::ProcessTree(-1).empty());
}

}  // namespace

int main() {
    TestUnsupported();
    TestLifecycle();
    TestWindowClosesFirst();
    TestExitStatus();
    TestClippedSnapshotKeepsWholePicture();
    TestClippedSnapshotWhenNothingWholeSeen();
    TestProcessTree();
    JobManager::Instance().ShutdownAll(200);
    if (g_failures) {
        std::fprintf(stderr, "gui_embed_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("gui_embed_test: all checks passed\n");
    return 0;
}
