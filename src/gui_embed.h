#ifndef MEP_GUI_EMBED_H
#define MEP_GUI_EMBED_H

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
    // Asks it to close the way its window manager's close button would.
    // False when it does not take such requests (the caller signals it).
    virtual bool RequestClose() = 0;
    // The pixels of the part of it on screen (all of it, unless clipped --
    // Snapshot::clipped says which), or an empty picture when they cannot
    // be read.
    virtual Snapshot Capture() = 0;
    // Its title, "" if it has none.
    virtual std::string Title() = 0;
};

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
    virtual std::vector<std::pair<std::string, std::string>> ChildEnv() const = 0;
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
    // True once each time the program took the keyboard by itself (a click
    // into a pointer-through window): the editor then treats it as focused.
    bool TakeFocusGained();

    // Stops it: first politely (a close request, else SIGTERM), then for good.
    void Stop();
    int Stops() const { return stops_; }

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
    std::string title_;
    Snapshot snapshot_;
    std::vector<std::string> out_, err_;
};

}  // namespace mep::gui_embed

#endif  // MEP_GUI_EMBED_H
