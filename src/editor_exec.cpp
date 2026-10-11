// :exec panes -- another program's own window shown as a pane (`:exec
// firefox`). The windowing is gui_embed.h's, shared with mepml's exec-gui
// blocks (editor_mepml.cpp): this file only decides what to run, keeps one
// EmbeddedApp per pane buffer, and hands the keyboard back and forth.
// DrawPane (main.cpp, DrawExecAppPane) puts the window over the pane's text
// area each frame; a frame that does not (another buffer shown, the pane
// closed) leaves it hidden.

#include <algorithm>
#include <string>
#include <vector>

#include "editor.h"
#include "gfx/input.h"
#include "gui_embed.h"

namespace {

std::string Trimmed(const std::string &s) {
    const size_t b = s.find_first_not_of(" \t\n");
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(" \t\n") - b + 1);
}

}  // namespace

bool Editor::IsExecAppBuffer(int buffer_id) const { return exec_apps_.find(buffer_id) != exec_apps_.end(); }

void Editor::OpenExecInPlace(const std::string &args) {
    const std::string line = Trimmed(args);
    if (line.empty()) {
        status_message_ = "Usage: :exec <program> [arguments] -- e.g. :exec firefox";
        return;
    }
    if (!gui_backend_) gui_backend_ = mep::gui_embed::CreateBackend(native_window_handle_);
    std::string why;
    if (!gui_backend_->Usable(&why)) {
        status_message_ = "Cannot show a program's window here: " + why;
        return;
    }
    ExecAppSession sess;
    sess.label = line;
    sess.cwd = ActiveRoot();
    std::vector<std::string> words;
    if (mep::gui_embed::SplitCommandLine(line, &words)) {
        std::string exe = mep::gui_embed::FindProgram(words[0]);
        if (exe.empty()) {
            // An application rather than a command: the whole line first
            // ("Visual Studio Code"), then its first word with the rest as
            // its arguments.
            exe = gui_backend_->ResolveApplication(line);
            if (!exe.empty()) {
                words = {exe};
            } else {
                exe = gui_backend_->ResolveApplication(words[0]);
                if (!exe.empty()) words[0] = exe;
            }
        } else {
            words[0] = exe;
        }
        if (exe.empty()) {
            status_message_ = "E-exec: no program or application named " + words[0];
            return;
        }
        sess.argv = words;
        sess.handoff_exe = exe;
    } else {
        // The shell's own: `exec` so its pid is the program's.
        sess.argv = {"/bin/sh", "-c", "exec " + line};
    }
    auto app = std::make_unique<mep::gui_embed::EmbeddedApp>(*gui_backend_);
    app->SetSnapshots(false);
    if (!sess.handoff_exe.empty()) app->SetHandoff(sess.handoff_exe);
    std::string error;
    if (!app->Start(sess.argv, sess.cwd, &error)) {
        status_message_ = error;
        return;
    }
    sess.app = std::move(app);
    const int buffer_id = CreateEmptyBuffer();
    exec_apps_[buffer_id] = std::move(sess);
    CurPane().buffer_id = buffer_id;
    CurPane().cursor = {0, 0};
    CurPane().scroll_row = 0;
    status_message_ = "Starting " + line + " -- its window fills this pane; Ctrl-\\ gives the keyboard back to mep, :bd closes it";
}

bool Editor::ExecAppViewOf(int buffer_id, ExecAppView *out) const {
    auto it = exec_apps_.find(buffer_id);
    if (it == exec_apps_.end()) return false;
    const ExecAppSession &sess = it->second;
    using State = mep::gui_embed::EmbeddedApp::State;
    out->shown = false;
    out->focused = exec_app_focus_ == buffer_id;
    if (!sess.app) {
        out->status = sess.label + " is not running -- r starts it, :bd closes this pane";
        return true;
    }
    switch (sess.app->GetState()) {
        case State::Starting: out->status = "starting " + sess.label + " ..."; break;
        case State::NoWindow: out->status = sess.label + " is running but has not opened a window -- :bd stops it"; break;
        case State::Shown: out->shown = true; out->status.clear(); break;
        case State::Exited:
            if (sess.app->HandedOff() || sess.app->ExitCode() == 0) {
                out->status = sess.label + " has closed -- r starts it again, :bd closes this pane";
            } else {
                const std::vector<std::string> &err = sess.app->Stderr();
                out->status = sess.label + " ended (exit " + std::to_string(sess.app->ExitCode()) + ")" +
                              (err.empty() ? std::string() : ": " + err.back()) + " -- r starts it again";
            }
            break;
    }
    return true;
}

void Editor::ExecAppPlace(int buffer_id, const mep::gui_embed::Rect &full, const mep::gui_embed::Rect &clip) {
    auto it = exec_apps_.find(buffer_id);
    if (it == exec_apps_.end() || !it->second.app) return;
    it->second.placed = full;
    it->second.app->Place(full, clip);
}

void Editor::ExecAppFocus(int buffer_id) {
    auto it = exec_apps_.find(buffer_id);
    if (it == exec_apps_.end() || !it->second.app) return;
    mep::gui_embed::EmbeddedApp &app = *it->second.app;
    if (app.GetState() != mep::gui_embed::EmbeddedApp::State::Shown || !app.Focus(true)) {
        status_message_ = "Its window is not showing yet";
        return;
    }
    exec_app_focus_ = buffer_id;
    const std::string name = app.Title().empty() ? it->second.label : app.Title();
    status_message_ = "Typing into " + name + " -- Ctrl-\\ (or a click outside it) returns to mep";
}

void Editor::ExecAppRestart(int buffer_id) {
    auto it = exec_apps_.find(buffer_id);
    if (it == exec_apps_.end() || !gui_backend_) return;
    ExecAppSession &sess = it->second;
    if (sess.app && sess.app->GetState() != mep::gui_embed::EmbeddedApp::State::Exited) {
        status_message_ = sess.label + " is still running";
        return;
    }
    auto app = std::make_unique<mep::gui_embed::EmbeddedApp>(*gui_backend_);
    app->SetSnapshots(false);
    if (!sess.handoff_exe.empty()) app->SetHandoff(sess.handoff_exe);
    std::string error;
    if (!app->Start(sess.argv, sess.cwd, &error)) {
        status_message_ = error;
        return;
    }
    sess.app = std::move(app);
    sess.focus_when_shown = true;
    status_message_ = "Starting " + sess.label;
}

void Editor::ExecAppClose(int buffer_id) {
    auto it = exec_apps_.find(buffer_id);
    if (it == exec_apps_.end()) return;
    if (exec_app_focus_ == buffer_id) exec_app_focus_ = -1;
    std::unique_ptr<mep::gui_embed::EmbeddedApp> app = std::move(it->second.app);
    exec_apps_.erase(it);
    if (!app || app->GetState() == mep::gui_embed::EmbeddedApp::State::Exited) return;
    // Asked to close the way its own close button would (it may ask about
    // unsaved work); what still runs after a few seconds is stopped for
    // good -- unless it is the user's own copy of the program, which is
    // only ever let go of (EmbeddedApp::Stop).
    app->Stop();
    exec_apps_closing_.emplace_back(std::move(app), now_ + 5.0);
}

int Editor::ExecAppsWaiting() const {
    int n = 0;
    for (const auto &kv : exec_apps_)
        if (kv.second.app && kv.second.app->GetState() == mep::gui_embed::EmbeddedApp::State::Starting) ++n;
    return n;
}

void Editor::ExecAppsTick(bool allow_unowned) {
    using State = mep::gui_embed::EmbeddedApp::State;
    for (auto it = exec_apps_closing_.begin(); it != exec_apps_closing_.end();) {
        it->first->Tick(now_, false);
        if (it->first->GetState() == State::Exited) {
            it = exec_apps_closing_.erase(it);
        } else if (now_ >= it->second) {
            // Still running: once more (a SIGTERM; nothing for a handed-off
            // window, which is just let go of), then gone.
            it->first->Stop();
            it = exec_apps_closing_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto &kv : exec_apps_) {
        const int buffer_id = kv.first;
        ExecAppSession &sess = kv.second;
        if (!sess.app) continue;
        mep::gui_embed::EmbeddedApp &app = *sess.app;
        const State before = app.GetState();
        app.Tick(now_, allow_unowned);
        const State state = app.GetState();
        // Its window just showed in the pane being worked in: it gets the
        // keyboard, the way a new terminal does.
        if (state == State::Shown && sess.focus_when_shown) {
            sess.focus_when_shown = false;
            if (CurPane().buffer_id == buffer_id && app.Focus(true)) {
                exec_app_focus_ = buffer_id;
                status_message_ = sess.label + " has the keyboard -- Ctrl-\\ (or a click outside it) returns to mep";
            }
        }
        if (app.TakeFocusGained()) {
            // Clicked into (on macOS a click always reaches the program).
            exec_app_focus_ = buffer_id;
            status_message_ = "The program has the keyboard: Ctrl-\\ or a click outside its window comes back";
        }
        if (exec_app_focus_ == buffer_id) {
            bool lost = app.TakeFocusLost() || !app.Focused();
            // mep only sees a click while the program has the keyboard when
            // it lands outside the program's window: that click takes the
            // keyboard back.
            if (!lost && (gfx::IsMouseButtonPressed(gfx::MouseButton::Left) || gfx::IsMouseButtonPressed(gfx::MouseButton::Right) ||
                          gfx::IsMouseButtonPressed(gfx::MouseButton::Middle))) {
                const gfx::Vector2 m = gfx::GetMousePosition();
                const mep::gui_embed::Rect &r = sess.placed;
                if (m.x < static_cast<float>(r.x) || m.y < static_cast<float>(r.y) || m.x >= static_cast<float>(r.x + r.w) ||
                    m.y >= static_cast<float>(r.y + r.h)) {
                    app.Focus(false);
                    lost = true;
                }
            }
            if (lost) {
                exec_app_focus_ = -1;
                status_message_ = "Back in mep";
            }
        }
        if (state == State::Exited && before != State::Exited) {
            if (exec_app_focus_ == buffer_id) exec_app_focus_ = -1;
            status_message_ = sess.label + " has closed";
        }
    }
}

void Editor::ExecAppsEndFrame() {
    for (auto &kv : exec_apps_) {
        if (!kv.second.app) continue;
        kv.second.app->EndFrame();
        if (exec_app_focus_ == kv.first && kv.second.app->TakeFocusLost()) {
            exec_app_focus_ = -1;
            status_message_ = "Back in mep";
        }
    }
    // (Never placed again: hidden, while they finish closing.)
    for (auto &closing : exec_apps_closing_) closing.first->EndFrame();
}

void Editor::HandleExecAppInput() {
    const int buffer_id = CurPane().buffer_id;
    if (!IsExecAppBuffer(buffer_id)) {
        mode_ = Mode::Normal;
        return;
    }
    if (gfx::IsKeyPressed(gfx::Key::Enter)) ExecAppFocus(buffer_id);
    int cp = gfx::GetCharPressed();
    while (cp > 0) {
        if (cp == ':') {
            EnterCommand();
            return;  // mode_ is no longer ExecApp -- stop draining as this mode
        } else if (cp == static_cast<int>(leader_key_) && !whichkey_bindings_.empty()) {
            TriggerWhichKey();
            return;
        } else if (cp == 'i' || cp == 'a') {
            ExecAppFocus(buffer_id);
        } else if (cp == 'r') {
            ExecAppRestart(buffer_id);
        }
        // Anything else is the program's to have, once it has the keyboard.
        cp = gfx::GetCharPressed();
    }
}
