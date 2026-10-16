#ifndef MEP_GUI_EMBED_H
#define MEP_GUI_EMBED_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Embedding another program's GUI window inside mep's own window -- what a
// mepml ```{exec-gui} block (or results=exec-gui) shows in its results.
//
// Split along the bridge pattern, so that the part that knows what an
// embedded program *is* never learns which windowing system it lives on:
//
//   Abstraction   EmbeddedApp -- one program: spawns it, waits for its
//                 window, keeps it placed where the document says, hands
//                 it the keyboard and takes it back, stops it, keeps a
//                 picture of its last frame. Platform-free.
//   Implementor   Backend + EmbeddedWindow -- the windowing system's side:
//                 find a process's window, adopt it into mep's window,
//                 move/clip/hide it, focus it, ask it to close, read its
//                 pixels.
//
// Implementors today:
//   - X11 (gui_embed_x11.cpp): reparents the program's top-level window
//     into mep's own X window. Works under XWayland too, as long as mep
//     itself runs there (its gfx backend is X11/GLX).
//   - macOS (gui_embed_macos.mm): no reparenting there -- the program's
//     window stays its own, moved and sized over the block through the
//     Accessibility API and kept just above mep's window. Needs the
//     Accessibility permission (and Screen Recording for its pictures).
//   - Private X server (gui_embed_xvfb.cpp): each program gets an Xvfb of
//     its own; mep draws the server's screen as a texture and passes it
//     the pointer and keys (EmbeddedWindow::DrawnByMep). No permission
//     and no window manager involved, but only X11 programs draw there.
//     On macOS it is layered over the Accessibility backend
//     (CreateLayeredBackend): an X11 program lands in its Xvfb, a Mac one
//     in its own window as before.
//   - Unsupported (gui_embed.cpp): every other build/session; says why.
// Adding a platform (native Wayland, Windows) means writing one
// more Backend/EmbeddedWindow pair and teaching CreateBackend to pick it;
// EmbeddedApp and everything above it stay as they are. (Native Wayland
// has no reparenting: its backend will be a small nested compositor that
// the child connects to, drawing the child's buffers as a texture and
// forwarding input -- which is why EmbeddedWindow speaks in terms of
// "place / focus / snapshot", not X-isms.)
//
// Xlib-free on purpose (see gfx/native_window_handle.h: Xlib's `Font`
// typedef collides with mep's own), so editor code can include it.

namespace mep::gui_embed {

// A rectangle in mep's window, in pixels.
struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool operator==(const Rect &o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
    bool operator!=(const Rect &o) const { return !(*this == o); }
    bool Empty() const { return w <= 0 || h <= 0; }
};

// An RGBA picture of a window (row-major, 4 bytes per pixel).
struct Snapshot {
    int width = 0, height = 0;
    // True when the pane's edge cut the window off, so this is a piece of
    // its picture and not the whole of it. Enough to show while nothing
    // better has been seen; never what an export should save.
    bool clipped = false;
    std::vector<unsigned char> rgba;
    bool Empty() const { return width <= 0 || height <= 0 || rgba.empty(); }
};

// --- Implementor ----------------------------------------------------------

// One adopted window of another program.
class EmbeddedWindow {
public:
    virtual ~EmbeddedWindow() = default;
    // False once the window is gone (its program closed it or exited).
    virtual bool Alive() = 0;
    // Shows the window at `full` (its whole size), clipped to `clip` (the
    // part of mep's window it may cover: its pane's text area). Cheap to
    // call every frame; only a change reaches the windowing system.
    virtual void Place(const Rect &full, const Rect &clip) = 0;
    // Takes it off screen (scrolled away, another buffer shown).
    virtual void Hide() = 0;
    // Gives it the keyboard (true) or gives the keyboard back to mep.
    virtual void Focus(bool on) = 0;
    // The pointer goes straight to the program, with no shield over it (a
    // web page: hovering and dragging have to just work). The keyboard
    // still stays with mep until the program takes it itself.
    virtual void SetPointerThrough(bool on) { (void)on; }
    // True while it actually holds the keyboard (the user may have clicked
    // another window since Focus(true)).
    virtual bool HasFocus() = 0;
    // True once, after the user asked (from inside the program) to give
    // the keyboard back to mep -- Ctrl-\ on X11.
    virtual bool TakeReleaseRequest() = 0;
    // True once, after one of mep's own chords (Backend::SetReservedChords)
    // was pressed while the program had the keyboard: it was kept from the
    // program, the keyboard is being given back (TakeReleaseRequest), and
    // the chord is mep's to act on. `key`/`mods` as SetReservedChords says.
    virtual bool TakeReservedChord(std::string *key, unsigned *mods) {
        (void)key;
        (void)mods;
        return false;
    }
    // Asks it to close the way its window manager's close button would.
    // False when it does not take such requests (the caller signals it).
    virtual bool RequestClose() = 0;
    // The pixels of the part of it on screen (all of it, unless clipped --
    // Snapshot::clipped says which), or an empty picture when they cannot
    // be read.
    virtual Snapshot Capture() = 0;
    // Its title, "" if it has none.
    virtual std::string Title() = 0;

    // --- A window no windowing system shows: mep draws it ---------------
    // True when mep itself draws the window (LiveFrame) and passes it the
    // pointer and the keys (Pointer*, Key, Text): it lives on a screen
    // nobody sees. Then HasFocus is simply what Focus last said, and the
    // keyboard never leaves mep -- mep's own chords stay mep's.
    virtual bool DrawnByMep() const { return false; }
    // Its picture as it is now, the whole of it, or null when there is
    // none yet. `version` changes whenever the pixels do.
    virtual const Snapshot *LiveFrame(uint64_t *version) {
        (void)version;
        return nullptr;
    }
    // The pointer at (x, y), in the window's own pixels.
    virtual void PointerMove(int x, int y) {
        (void)x;
        (void)y;
    }
    // A button pressed or let go: 1 left, 2 middle, 3 right, 4/5 wheel
    // up/down, 6/7 wheel left/right (X11's numbering).
    virtual void PointerButton(int button, bool down) {
        (void)button;
        (void)down;
    }
    // A key pressed and let go: an X11 keysym (the one portable name for
    // keys there is), held down with `mods` (KeyMod bits).
    virtual void Key(unsigned keysym, unsigned mods) {
        (void)keysym;
        (void)mods;
    }
    // One character typed.
    virtual void Text(char32_t cp) { (void)cp; }
};

// EmbeddedWindow::Key's modifiers.
enum KeyMod : unsigned { kModShift = 1, kModCtrl = 2, kModAlt = 4, kModSuper = 8 };

class Backend;

// A web page shown by mep itself, through the platform's WebKit (WKWebView
// on macOS) -- what :Browser opens. No other program is involved: the page
// is a view inside mep's own window, so EmbeddedApp::StartWindow takes it
// in place of a process's window, and the :exec pane machinery places it,
// hands it the keyboard and takes it back exactly as for a program.
class WebView : public EmbeddedWindow {
public:
    // Goes to `target`: a URL, an absolute path to a local file,
    // "localhost:8000", "example.com", or anything else as a web search.
    virtual void Load(const std::string &target) = 0;
    virtual void Back() = 0;
    virtual void Forward() = 0;
    // Loads the page again, or stops it while it is still loading.
    virtual void Reload() = 0;
    // Puts the keyboard in the address bar, its text selected.
    virtual void EditAddress() = 0;
    // The page's URL, "" before anything has loaded.
    virtual std::string Url() = 0;

    // mep's colours, for the page and the address bar to take on.
    struct Rgb {
        unsigned char r = 0, g = 0, b = 0;
    };
    struct Theme {
        // False: the page in its own colours (only the address bar follows
        // mep's).
        bool recolor = true;
        bool dark = false;          // what prefers-color-scheme says
        Rgb bg, fg;                 // the paper and the ink
        Rgb dim;                    // secondary text (placeholders)
        Rgb selection, border, bar;  // a text selection, an edge, the address bar
        // The palette's hues: a coloured link, button or highlight on the
        // page becomes the nearest of these.
        Rgb red, green, yellow, blue, purple, cyan, orange;
    };
    // Applies `theme` to the page shown now and to every page after it.
    virtual void SetTheme(const Theme &theme) = 0;
};

/**
 * @brief A new web view in mep's window (hidden until placed), or null where there is no WebKit to show one with.
 * @param backend The windowing system (outlives the view): on X11 the view is a program of its own, mep-webview, whose
 * window the backend adopts (gui_embed_webview_x11.cpp); on macOS it is a WKWebView of mep's own and needs nothing of it.
 * @param native_window_handle gfx::GetNativeWindowHandle()'s value.
 * @param reserved Which chords stay mep's while the page has the keyboard (Backend::SetReservedChords's terms).
 * @param why Set to the reason when null is returned.
 */
std::unique_ptr<WebView> CreateWebView(Backend &backend, void *native_window_handle,
                                       std::function<bool(const std::string &key, unsigned mods)> reserved, std::string *why);

// The windowing system.
class Backend {
public:
    virtual ~Backend() = default;
    // "x11", "unsupported", ...
    virtual std::string Name() const = 0;
    // Whether programs can be embedded at all; `why` says why not.
    virtual bool Usable(std::string *why) const = 0;
    // Environment a child needs so it opens its window where this backend
    // can adopt it (DISPLAY, and toolkit hints like GDK_BACKEND=x11).
    // Called once for every program, right before it is started (a
    // backend may set up a display for it here: ChildStarted says whose).
    virtual std::vector<std::pair<std::string, std::string>> ChildEnv() = 0;
    // The program ChildEnv was last called for has started as `pid` (or
    // failed to: never called).
    virtual void ChildStarted(int pid) { (void)pid; }
    // The program `pid` has ended, or no longer needs what ChildEnv set up
    // for it.
    virtual void ChildEnded(int pid) { (void)pid; }
    // Why a program that is running shows no window, when the backend
    // knows ("" otherwise) -- a Mac program, say, while mep may not move
    // windows.
    virtual std::string WhyNoWindow() const { return ""; }
    // True when this windowing system cannot put a shield over a program
    // (macOS): the pointer always reaches it, so a click into it takes the
    // keyboard, as SetPointerThrough(true) arranges elsewhere.
    virtual bool PointerThroughOnly() const { return false; }
    // Called at spawn time: remembers which windows already exist, so a
    // window that cannot be traced to a process can still be recognised
    // as new.
    virtual void NoteExistingWindows() = 0;
    // Adopts the main window of one of `pids` (the program and its
    // children), or returns null when none of them has one yet.
    // `allow_unowned`: also accept a new window that names no process at
    // all (old toolkits set no _NET_WM_PID) -- only safe while a single
    // program is waiting for its window.
    virtual std::unique_ptr<EmbeddedWindow> Adopt(const std::vector<int> &pids, bool allow_unowned) = 0;
    // Adopts a window a program *handed off*: most browsers, editors and
    // office suites, started while a copy is already running, ask that copy
    // for a new window and exit. A window that was not there when
    // NoteExistingWindows ran, owned by a process (none of `own_pids`)
    // running one of `executables` (absolute, canonical paths: what was
    // started, and what it ran -- a launcher script's real program). That process is
    // not mep's: the window must be hidden and closed on its own, never by
    // anything that touches the rest of the program (its other windows are
    // the user's). Null when there is none (yet), or the backend cannot.
    virtual std::unique_ptr<EmbeddedWindow> AdoptHandoff(const std::vector<std::string> &executables, const std::vector<int> &own_pids) {
        (void)executables;
        (void)own_pids;
        return nullptr;
    }
    // The program to run for an application known by name rather than as a
    // command on PATH -- "firefox" or "Visual Studio Code" on macOS, whose
    // applications are bundles: the bundle's executable. "" when there is
    // no such application (or the platform has none of that kind).
    virtual std::string ResolveApplication(const std::string &name) const {
        (void)name;
        return "";
    }
    // Which chords are mep's even while a program has the keyboard (its
    // mod1 pane moves): `key` is "a".."z", "0".."9", "Tab" or "CR", `mods`
    // KeyMod bits. Where the windowing system hands the program the keys
    // itself, the backend keeps these from it (TakeReservedChord) if it
    // can; where mep draws the window it never needs to.
    virtual void SetReservedChords(std::function<bool(const std::string &key, unsigned mods)> reserved) { (void)reserved; }
    // Once per frame: drains the windowing system's events.
    virtual void Pump() = 0;
    // Once per frame, after every Place/Hide: pushes the changes out.
    virtual void Flush() = 0;
};

/**
 * @brief Picks the backend for the window mep is running in.
 * @param native_window_handle gfx::GetNativeWindowHandle()'s value (null when there is no window: headless runs, tests).
 * @return A backend -- the unsupported one when nothing better fits, never null.
 */
std::unique_ptr<Backend> CreateBackend(void *native_window_handle);

/**
 * @brief A backend that is two: every program is started for both (both ChildEnvs), and its window is looked for in
 * `first`, then in `second` -- where it shows up depends on the program (an X11 one on `first`'s private server, a Mac
 * one as its own window). Hand-offs and named applications are `second`'s.
 */
std::unique_ptr<Backend> CreateLayeredBackend(std::unique_ptr<Backend> first, std::unique_ptr<Backend> second);

#if defined(MEP_GUI_EMBED_XVFB)
/**
 * @brief The private X server backend (gui_embed_xvfb.cpp), or null when there is no Xvfb or Xlib to run it with.
 * @param why Set to the reason when null is returned.
 */
std::unique_ptr<Backend> CreateXvfbBackend(std::string *why);
#endif

/**
 * @brief The backend for platforms with no embedding yet.
 * @param why What to tell the user.
 */
std::unique_ptr<Backend> CreateUnsupportedBackend(std::string why);

#if defined(MEP_GUI_EMBED_X11)
/**
 * @brief The X11 backend (gui_embed_x11.cpp), or null when X cannot be reached.
 * @param native_window_handle A gfx::NativeWindowHandle* for mep's own window.
 * @param why Set to the reason when null is returned.
 */
std::unique_ptr<Backend> CreateX11Backend(void *native_window_handle, std::string *why);
#endif

#if defined(MEP_GUI_EMBED_MACOS)
/**
 * @brief The macOS backend (gui_embed_macos.mm), or null when mep has no Cocoa window.
 * @param native_window_handle A gfx::NativeWindowHandle* for mep's own window.
 * @param why Set to the reason when null is returned.
 */
std::unique_ptr<Backend> CreateMacOSBackend(void *native_window_handle, std::string *why);
#endif

/**
 * @brief The process and every descendant of it (Linux: from /proc; macOS: libproc), the process first.
 * @param pid The root process.
 */
std::vector<int> ProcessTree(int pid);

/**
 * @brief Splits a plain command line into words the way the shell would ('...' and "..." quoting, backslash escapes).
 * @param line The command line.
 * @param words Set to its words.
 * @return False (words empty) when the line uses more of the shell than that -- pipes, redirections, variables, globs, ~ -- and so is /bin/sh's to run.
 */
bool SplitCommandLine(const std::string &line, std::vector<std::string> *words);

/**
 * @brief The program a command names, as a canonical path: `name` itself when it has a '/', else the first match on PATH; "" when there is none.
 * @param name The command's first word.
 */
std::string FindProgram(const std::string &name);

/**
 * @brief The canonical path of the program a process runs (Linux: /proc/<pid>/exe; macOS: proc_pidpath), "" when unknown.
 * @param pid The process.
 */
std::string ProcessExecutable(int pid);

/**
 * @brief The programs a launcher script hands over to: for a file starting with `#!`, the canonical path of every
 * existing executable it names with an absolute path after `exec` (Homebrew's `firefox` is a bash script doing
 * `exec '/Applications/Firefox.app/Contents/MacOS/firefox' "$@"`); empty for anything else.
 * @param path The program as FindProgram resolved it.
 */
std::vector<std::string> LauncherTargets(const std::string &path);

// --- Abstraction ------------------------------------------------------------

// One program shown inside the document.
class EmbeddedApp {
public:
    enum class State {
        Starting,  // spawned, no window yet
        Shown,     // its window is adopted
        NoWindow,  // it is running but never opened a window we could find
        Exited,    // the program has ended
    };

    /**
     * @param backend The windowing system (outlives this object).
     */
    explicit EmbeddedApp(Backend &backend) : backend_(backend) {}
    ~EmbeddedApp();
    EmbeddedApp(const EmbeddedApp &) = delete;
    EmbeddedApp &operator=(const EmbeddedApp &) = delete;

    /**
     * @brief Starts the program.
     * @param argv Its command line.
     * @param cwd Its working directory.
     * @param error Set to why it could not start.
     * @return Whether it started.
     */
    bool Start(const std::vector<std::string> &argv, const std::string &cwd, std::string *error);

    /**
     * @brief Shows a window mep made itself (a WebView) instead of starting a program: there is no process, the window is
     * the program -- it has ended once the window is gone, and stopping it asks the window to close.
     * @param window The window.
     */
    void StartWindow(std::unique_ptr<EmbeddedWindow> window);
    bool InProcess() const { return in_process_; }
    // The window shown, null while there is none.
    EmbeddedWindow *Window() const { return window_.get(); }

    /**
     * @brief Once per frame: finds its window, notices the program ending and the window closing, keeps its last picture.
     * @param now Seconds (any monotonic clock).
     * @param allow_unowned Whether an ownerless new window may be taken as its own (Backend::Adopt).
     */
    void Tick(double now, bool allow_unowned);

    // Where the document shows it this frame (see EmbeddedWindow::Place).
    void Place(const Rect &full, const Rect &clip);
    // Called after the frame is drawn: hides it if nothing placed it.
    void EndFrame();

    bool Focus(bool on);
    bool Focused() const { return focused_; }
    // True once when the user gave the keyboard back from inside the
    // program, or clicked away from it.
    bool TakeFocusLost();
    // See EmbeddedWindow::SetPointerThrough; kept for a window adopted later.
    void SetPointerThrough(bool on);
    // Before Start: the canonical path of the program being started, so a
    // window it hands off to an already-running copy of itself is adopted
    // instead (Backend::AdoptHandoff). From then on the window *is* the
    // program: it ends when the window closes, and stopping it only ever
    // asks the window to close -- the process belongs to the user. A
    // launcher script's targets (LauncherTargets) count as the program too:
    // the copy already running is the real program, never the script, and
    // the script's own process may be gone before it is seen running it.
    void SetHandoff(const std::string &executable);
    bool HandedOff() const { return handed_off_; }
    // Whether to keep pictures of it (LastSnapshot) -- a document's block
    // keeps its last frame; a pane showing the program itself needs none.
    void SetSnapshots(bool on) { snapshots_ = on; }
    // True once each time the program took the keyboard by itself (a click
    // into a pointer-through window): the editor then treats it as focused.
    bool TakeFocusGained();

    // A window mep draws and feeds itself (EmbeddedWindow::DrawnByMep):
    // its picture, and the pointer and keys, in the window's own pixels.
    bool DrawnByMep() const { return window_ && window_->DrawnByMep(); }
    const Snapshot *LiveFrame(uint64_t *version) { return window_ ? window_->LiveFrame(version) : nullptr; }
    void PointerMove(int x, int y) {
        if (window_) window_->PointerMove(x, y);
    }
    void PointerButton(int button, bool down) {
        if (window_) window_->PointerButton(button, down);
    }
    void Key(unsigned keysym, unsigned mods) {
        if (window_) window_->Key(keysym, mods);
    }
    void Text(char32_t cp) {
        if (window_) window_->Text(cp);
    }
    // See EmbeddedWindow::TakeReservedChord.
    bool TakeReservedChord(std::string *key, unsigned *mods) { return window_ && window_->TakeReservedChord(key, mods); }

    // Stops it: first politely (a close request, else SIGTERM), then for good.
    void Stop();
    int Stops() const { return stops_; }
    // When it was started (the first Tick's `now`), -1 before that.
    double StartedAt() const { return started_at_; }

    State GetState() const { return state_; }
    int ExitCode() const { return exit_code_; }
    const std::string &Title() const { return title_; }
    // Its last picture (taken while it was shown, and again right before
    // a stop), empty if it never showed anything.
    const Snapshot &LastSnapshot() const { return snapshot_; }
    // What it printed on stdout / stderr (the last lines of each).
    const std::vector<std::string> &Stdout() const { return out_; }
    const std::vector<std::string> &Stderr() const { return err_; }

private:
    // What the job's callbacks write into: shared with them, so one that
    // fires after this object is gone writes into nothing that matters.
    struct JobState;
    void TakeSnapshot();

    Backend &backend_;
    std::shared_ptr<JobState> job_state_;
    std::unique_ptr<EmbeddedWindow> window_;
    int job_id_ = 0;
    int pid_ = -1;
    State state_ = State::Starting;
    int exit_code_ = 0;
    bool exited_ = false;
    int stops_ = 0;
    double started_at_ = -1.0;
    double last_snapshot_ = -1.0;
    bool placed_ = false;       // placed during the frame being drawn
    bool pointer_through_ = false;
    bool focus_gained_ = false;
    bool shown_ = false;        // placed during the last frame drawn
    bool focused_ = false;
    bool focus_lost_ = false;
    bool snapshots_ = true;
    std::vector<std::string> handoff_exes_;  // SetHandoff, plus what its processes ran while it waited
    bool handed_off_ = false;   // its window is a handed-off one
    bool in_process_ = false;   // its window is mep's own (StartWindow)
    double exited_at_ = -1.0;   // when the process ended (a hand-off is looked for a while after)
    std::string title_;
    Snapshot snapshot_;
    std::vector<std::string> out_, err_;
};

}  // namespace mep::gui_embed

#endif  // MEP_GUI_EMBED_H
