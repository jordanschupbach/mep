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
#include "lua_env.h"

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
    EnsureGuiBackend();
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
        case State::NoWindow: {
            const std::string why = gui_backend_ ? gui_backend_->WhyNoWindow() : std::string();
            out->status = sess.label + " is running but has not opened a window" + (why.empty() ? "" : " (" + why + ")") + " -- :bd stops it";
            break;
        }
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
    it->second.pointer.clip = clip;
    it->second.pointer.placed_now = true;
    it->second.app->Place(full, clip);
}

mep::gui_embed::EmbeddedApp *Editor::ExecAppOf(int buffer_id) {
    auto it = exec_apps_.find(buffer_id);
    return it == exec_apps_.end() ? nullptr : it->second.app.get();
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
    // A chord of mep's own the program was kept from: run once the loop is
    // done (it may close a pane, and with it an entry of exec_apps_).
    std::string chord;
    unsigned chord_mods = 0;
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
                app.TakeReservedChord(&chord, &chord_mods);
            }
        }
        if (state == State::Exited && before != State::Exited) {
            if (exec_app_focus_ == buffer_id) exec_app_focus_ = -1;
            status_message_ = sess.label + " has closed";
        }
    }
    if (!chord.empty()) RunReservedChord(chord, chord_mods);
}

void Editor::ExecAppsEndFrame() {
    for (auto &kv : exec_apps_) {
        kv.second.pointer.visible = kv.second.pointer.placed_now;
        kv.second.pointer.placed_now = false;
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

// --- Windows mep draws itself (EmbeddedWindow::DrawnByMep) -------------------
// A program on a private display (gui_embed_xvfb.cpp) is shown only as the
// picture DrawPane / DrawMepmlGui draw, so mep is what gets its pointer and
// keys: they are passed on from here. The keyboard never leaves mep, which
// is what keeps mep's own chords (mod1 pane moves, tabs) mep's while a
// program "has" it.

namespace {

// The X11 keysym of a key mep reads as a key rather than as text (0: none).
unsigned KeysymOf(gfx::Key key) {
    using K = gfx::Key;
    if (key >= K::A && key <= K::Z) return 'a' + static_cast<unsigned>(key - K::A);
    if (key >= K::Zero && key <= K::Nine) return '0' + static_cast<unsigned>(key - K::Zero);
    switch (key) {
        case K::Escape: return 0xff1b;
        case K::Enter: return 0xff0d;
        case K::KpEnter: return 0xff8d;
        case K::Backspace: return 0xff08;
        case K::Tab: return 0xff09;
        case K::Delete: return 0xffff;
        case K::Insert: return 0xff63;
        case K::Home: return 0xff50;
        case K::End: return 0xff57;
        case K::PageUp: return 0xff55;
        case K::PageDown: return 0xff56;
        case K::Left: return 0xff51;
        case K::Up: return 0xff52;
        case K::Right: return 0xff53;
        case K::Down: return 0xff54;
        case K::Space: return ' ';
        case K::Minus: return '-';
        case K::Equal: return '=';
        case K::LeftBracket: return '[';
        case K::RightBracket: return ']';
        case K::Backslash: return '\\';
        default: return 0;
    }
}

// Keys that are never text: always passed on as keys (letters, digits and
// the like only with a modifier -- plain, they come as text).
bool NeverText(gfx::Key key) {
    using K = gfx::Key;
    switch (key) {
        case K::Escape: case K::Enter: case K::KpEnter: case K::Backspace: case K::Tab: case K::Delete: case K::Insert:
        case K::Home: case K::End: case K::PageUp: case K::PageDown: case K::Left: case K::Up: case K::Right: case K::Down:
            return true;
        default: return false;
    }
}

bool Within(const mep::gui_embed::Rect &r, float x, float y) {
    return x >= static_cast<float>(r.x) && y >= static_cast<float>(r.y) && x < static_cast<float>(r.x + r.w) && y < static_cast<float>(r.y + r.h);
}

mep::gui_embed::Rect Intersect(const mep::gui_embed::Rect &a, const mep::gui_embed::Rect &b) {
    mep::gui_embed::Rect c;
    c.x = std::max(a.x, b.x);
    c.y = std::max(a.y, b.y);
    c.w = std::min(a.x + a.w, b.x + b.w) - c.x;
    c.h = std::min(a.y + a.h, b.y + b.h) - c.y;
    return c;
}

// One program's share of this frame's pointer. `wheel_ok`: the wheel over
// it is its own (else it scrolls what is under it, the document).
// `covered`: one of mep's popups is up, maybe over it -- clicks are the
// popup's.
bool FeedPointer(mep::gui_embed::EmbeddedApp &app, const mep::gui_embed::Rect &full, Editor::EmbeddedPointerState &p, gfx::Vector2 wheel,
                 bool wheel_ok, bool covered) {
    if (!app.DrawnByMep() || !p.visible) {
        p.held = 0;
        return false;
    }
    const gfx::Vector2 m = gfx::GetMousePosition();
    const bool inside = Within(Intersect(full, p.clip), m.x, m.y);
    // A drag that began on it keeps going to it outside, until let go.
    if (!inside && p.held == 0) return false;
    const int x = std::clamp(static_cast<int>(m.x) - full.x, 0, std::max(0, full.w - 1));
    const int y = std::clamp(static_cast<int>(m.y) - full.y, 0, std::max(0, full.h - 1));
    if (x != p.x || y != p.y) {
        app.PointerMove(x, y);
        p.x = x;
        p.y = y;
    }
    static const std::pair<gfx::MouseButton, int> kButtons[] = {
        {gfx::MouseButton::Left, 1}, {gfx::MouseButton::Middle, 2}, {gfx::MouseButton::Right, 3}};
    for (const auto &[button, n] : kButtons) {
        const int bit = 1 << n;
        if (inside && !covered && gfx::IsMouseButtonPressed(button)) {
            app.PointerButton(n, true);
            p.held |= bit;
        }
        if ((p.held & bit) && !gfx::IsMouseButtonDown(button)) {
            app.PointerButton(n, false);
            p.held &= ~bit;
        }
    }
    if (!inside || covered || !wheel_ok || (wheel.x == 0.0f && wheel.y == 0.0f)) return false;
    // Whole notches only: a trackpad's small steps add up to one.
    auto notches = [&app](float *acc, float move, int up, int down) {
        *acc += move;
        for (; *acc >= 1.0f; *acc -= 1.0f) {
            app.PointerButton(up, true);
            app.PointerButton(up, false);
        }
        for (; *acc <= -1.0f; *acc += 1.0f) {
            app.PointerButton(down, true);
            app.PointerButton(down, false);
        }
    };
    notches(&p.wheel_y, wheel.y, 4, 5);
    notches(&p.wheel_x, wheel.x, 6, 7);
    return true;
}

}  // namespace

bool Editor::HandleEmbeddedPointer(gfx::Vector2 wheel) {
    bool wheel_taken = false;
    const bool covered = mode_ == Mode::Picker || mode_ == Mode::Command || mode_ == Mode::WhichKey || mode_ == Mode::Prompt ||
                         mode_ == Mode::Confirm;
    for (auto &kv : exec_apps_)
        if (kv.second.app) wheel_taken |= FeedPointer(*kv.second.app, kv.second.placed, kv.second.pointer, wheel, !wheel_taken, covered);
    // A document's block takes the wheel only while it has the keyboard:
    // otherwise the wheel scrolls the document past it.
    for (auto &kv : mepml_guis_)
        if (kv.second.app)
            wheel_taken |=
                FeedPointer(*kv.second.app, kv.second.placed, kv.second.pointer, wheel, !wheel_taken && mepml_gui_focus_ == kv.first, covered);
    return wheel_taken;
}

void Editor::ReleaseDrawnAppKeyboard() {
    if (exec_app_focus_ >= 0) {
        if (mep::gui_embed::EmbeddedApp *app = ExecAppOf(exec_app_focus_)) app->Focus(false);
        exec_app_focus_ = -1;
    }
    if (mepml_gui_focus_ >= 0) {
        if (mep::gui_embed::EmbeddedApp *app = MepmlGuiAppOf(mepml_gui_focus_)) app->Focus(false);
        mepml_gui_focus_ = -1;
    }
}

bool Editor::HandleEmbeddedKeys() {
    mep::gui_embed::EmbeddedApp *app = nullptr;
    bool still_there = false;  // its pane (or document) is still the one being worked in
    if (exec_app_focus_ >= 0) {
        app = ExecAppOf(exec_app_focus_);
        still_there = CurPane().buffer_id == exec_app_focus_;
    } else if (mepml_gui_focus_ >= 0) {
        app = MepmlGuiAppOf(mepml_gui_focus_);
        auto it = mepml_guis_.find(mepml_gui_focus_);
        still_there = it != mepml_guis_.end() && it->second.buffer_id == CurrentBufferId();
    }
    if (!app || !app->DrawnByMep()) return false;
    // Moved away (a mod1 pane move, a buffer switch): the keyboard stays
    // with mep.
    if (!still_there) {
        ReleaseDrawnAppKeyboard();
        status_message_ = "Back in mep";
        return false;
    }
    // Something of mep's own is up (a picker, the command line): its keys.
    if (mode_ != Mode::Normal && mode_ != Mode::ExecApp) return false;
    const bool ctrl = gfx::IsKeyDown(gfx::Key::LeftControl) || gfx::IsKeyDown(gfx::Key::RightControl);
    const bool shift = gfx::IsKeyDown(gfx::Key::LeftShift) || gfx::IsKeyDown(gfx::Key::RightShift);
    const bool alt = gfx::IsKeyDown(gfx::Key::LeftAlt) || gfx::IsKeyDown(gfx::Key::RightAlt);
    const bool super = gfx::IsKeyDown(gfx::Key::LeftSuper) || gfx::IsKeyDown(gfx::Key::RightSuper);
    const unsigned mods = (shift ? mep::gui_embed::kModShift : 0u) | (ctrl ? mep::gui_embed::kModCtrl : 0u) |
                          (alt ? mep::gui_embed::kModAlt : 0u) | (super ? mep::gui_embed::kModSuper : 0u);
    const bool chord = ctrl || alt || super;
    for (gfx::Key key = gfx::GetKeyPressed(); key != gfx::Key::None; key = gfx::GetKeyPressed()) {
        if (ctrl && key == gfx::Key::Backslash) {
            ReleaseDrawnAppKeyboard();
            while (gfx::GetCharPressed() > 0) {
            }
            status_message_ = "Back in mep";
            return true;
        }
        if (!NeverText(key) && !chord) continue;  // (comes as text, below)
        if (const unsigned sym = KeysymOf(key)) app->Key(sym, mods);
    }
    // Held keys that are never text repeat here (text repeats by itself).
    // Only a genuine repeat: IsKeyPressedRepeat is also true on the press
    // the loop above already passed on.
    static const gfx::Key kRepeating[] = {gfx::Key::Backspace, gfx::Key::Delete, gfx::Key::Left, gfx::Key::Right,
                                          gfx::Key::Up,        gfx::Key::Down,   gfx::Key::PageUp, gfx::Key::PageDown};
    for (gfx::Key key : kRepeating)
        if (gfx::IsKeyPressedRepeat(key) && !gfx::IsKeyPressed(key)) app->Key(KeysymOf(key), mods);
    for (int cp = gfx::GetCharPressed(); cp > 0; cp = gfx::GetCharPressed()) {
        // A chord was passed on as its key; Tab as a key already.
        if (chord || cp == '\t') continue;
        app->Text(static_cast<char32_t>(cp));
    }
    return true;
}

// --- mep's own chords, kept from a program that has the keyboard -----------
// Where the windowing system hands a program the keys itself (a Mac app's
// own window), the backend keeps mep's mod1 chords from it and hands them
// back with the keyboard (EmbeddedWindow::TakeReservedChord): mod1+h moves
// to the pane on the left whether or not a program has the keyboard.

void Editor::EnsureGuiBackend() {
    if (gui_backend_) return;
    gui_backend_ = mep::gui_embed::CreateBackend(native_window_handle_);
    gui_backend_->SetReservedChords([this](const std::string &key, unsigned mods) { return IsReservedChord(key, mods); });
}

namespace {
// The name HandleMod1Shortcuts looks a chord up by ("C-S-h"), "" when the
// chord does not hold mod1 (`mod1`: its KeyMod bit).
std::string ChordMappingName(unsigned mod1, const std::string &key, unsigned mods) {
    using namespace mep::gui_embed;
    if (!(mods & mod1)) return "";
    const bool ctrl = mod1 != kModCtrl && (mods & kModCtrl);
    const bool shift = mod1 != kModShift && (mods & kModShift);
    if (ctrl && shift) return "C-S-" + key;
    if (ctrl) return "C-" + key;
    if (shift) return "S-" + key;
    return key;
}
}  // namespace

unsigned Editor::Mod1Bit() const {
    switch (mod1_) {
        case ModKey::Alt: return mep::gui_embed::kModAlt;
        case ModKey::Control: return mep::gui_embed::kModCtrl;
        case ModKey::Shift: return mep::gui_embed::kModShift;
        case ModKey::Super: return mep::gui_embed::kModSuper;
    }
    return mep::gui_embed::kModAlt;
}

bool Editor::IsReservedChord(const std::string &key, unsigned mods) const {
    const std::string name = ChordMappingName(Mod1Bit(), key, mods);
    if (name.empty()) return false;
    auto it = mod1_mappings_.find(name);
    return it != mod1_mappings_.end() && !it->second.normal_only;
}

void Editor::RunReservedChord(const std::string &key, unsigned mods) {
    auto it = mod1_mappings_.find(ChordMappingName(Mod1Bit(), key, mods));
    if (it != mod1_mappings_.end() && lua_) lua_->CallRef(it->second.lua_ref);
}
