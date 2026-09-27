// The X11 implementor of gui_embed.h: another program's top-level window,
// reparented into mep's own X window.
//
// How a window is taken in (X11Backend::Adopt):
//   1. Found: among the screen's client windows (the ones a window manager
//      manages; with no window manager, root's own mapped children), the
//      one whose _NET_WM_PID is the program or one of its children --
//      the largest, if it has several, and never a transient dialog.
//   2. Withdrawn (XWithdrawWindow), so a window manager lets go of it and
//      takes its frame away.
//   3. Reparented into a *container*: a plain child window of mep's that
//      this backend owns and sizes to the part of the block on screen, so
//      the program is clipped at the pane's edges instead of drawing over
//      the tab bar and status line.
//   4. Covered by a *shield*: an InputOnly window over the container.
//      Clicks and keys over it fall through to mep's own window (an
//      InputOnly window of ours selects nothing, so events propagate to
//      mep's), which is what keeps the document usable with the mouse
//      resting over the program. Focus(true) takes the shield away and
//      gives the program the X input focus; a passive grab of Ctrl-\ on
//      the container is how the keyboard comes back (it is the one key
//      mep still sees while the program has focus).
//
// Everything goes through this backend's *own* connection to the X server
// (not the gfx backend's): a program's window can vanish between any two
// requests, and Xlib's default reaction to the BadWindow that follows is
// to exit the process. Errors on this connection are trapped and ignored;
// errors on any other are passed to whatever handler was there before.

#include "gui_embed.h"

#if defined(MEP_GUI_EMBED_X11)

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <set>
#include <thread>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include "gfx/native_window_handle.h"

namespace mep::gui_embed {
namespace {

// Xlib's error handler is process-wide: this connection's errors are
// swallowed, every other connection's go to the handler found first.
Display *g_trap_display = nullptr;
XErrorHandler g_prev_handler = nullptr;
int g_last_error = 0;

int TrapErrors(Display *d, XErrorEvent *e) {
    if (d == g_trap_display) {
        g_last_error = e->error_code;
        return 0;
    }
    return g_prev_handler ? g_prev_handler(d, e) : 0;
}

class X11Window;

class X11Backend final : public Backend {
public:
    X11Backend(Display *dpy, Window parent, std::string display_name)
        : dpy_(dpy), parent_(parent), root_(DefaultRootWindow(dpy)), display_name_(std::move(display_name)) {
        wm_state_ = XInternAtom(dpy_, "WM_STATE", False);
        net_wm_pid_ = XInternAtom(dpy_, "_NET_WM_PID", False);
        net_wm_name_ = XInternAtom(dpy_, "_NET_WM_NAME", False);
        utf8_ = XInternAtom(dpy_, "UTF8_STRING", False);
        wm_protocols_ = XInternAtom(dpy_, "WM_PROTOCOLS", False);
        wm_delete_ = XInternAtom(dpy_, "WM_DELETE_WINDOW", False);
        g_trap_display = dpy_;
        g_prev_handler = XSetErrorHandler(TrapErrors);
        if (g_prev_handler == TrapErrors) g_prev_handler = nullptr;
        // mep's own top-level (its window manager frame, if any): never a candidate.
        own_top_ = TopLevelOf(parent_);
    }
    ~X11Backend() override {
        XSync(dpy_, False);
        XCloseDisplay(dpy_);
        if (g_trap_display == dpy_) g_trap_display = nullptr;
    }

    std::string Name() const override { return "x11"; }
    bool Usable(std::string *) const override { return true; }
    std::vector<std::pair<std::string, std::string>> ChildEnv() const override {
        // The program's window has to be an X window: toolkits that would
        // pick Wayland when it is there are told not to.
        return {{"DISPLAY", display_name_},        {"GDK_BACKEND", "x11"},
                {"QT_QPA_PLATFORM", "xcb"},         {"SDL_VIDEODRIVER", "x11"},
                {"MOZ_ENABLE_WAYLAND", "0"},        {"ELECTRON_OZONE_PLATFORM_HINT", "x11"},
                {"WINIT_UNIX_BACKEND", "x11"}};
    }
    void NoteExistingWindows() override {
        known_.clear();
        for (Window w : ClientWindows()) known_.insert(w);
    }
    std::unique_ptr<EmbeddedWindow> Adopt(const std::vector<int> &pids, bool allow_unowned) override;
    void Pump() override;
    void Flush() override { XFlush(dpy_); }

    // --- for X11Window --------------------------------------------------
    Display *dpy() const { return dpy_; }
    Window parent() const { return parent_; }
    Window root() const { return root_; }
    Atom net_wm_name() const { return net_wm_name_; }
    Atom utf8() const { return utf8_; }
    Atom wm_protocols() const { return wm_protocols_; }
    Atom wm_delete() const { return wm_delete_; }
    void Register(Window w, X11Window *win) { windows_[w] = win; }
    void Unregister(X11Window *win) {
        for (auto it = windows_.begin(); it != windows_.end();)
            it = it->second == win ? windows_.erase(it) : std::next(it);
    }
    bool IsInside(Window w, Window ancestor) const {
        for (int depth = 0; w != 0 && depth < 32; ++depth) {
            if (w == ancestor) return true;
            Window root = 0, parent = 0, *kids = nullptr;
            unsigned int n = 0;
            if (!XQueryTree(dpy_, w, &root, &parent, &kids, &n)) return false;
            if (kids) XFree(kids);
            if (parent == root) return false;
            w = parent;
        }
        return false;
    }

private:
    bool HasProperty(Window w, Atom prop) const {
        Atom type = 0;
        int format = 0;
        unsigned long n = 0, after = 0;
        unsigned char *data = nullptr;
        const int ok = XGetWindowProperty(dpy_, w, prop, 0, 0, False, AnyPropertyType, &type, &format, &n, &after, &data);
        if (data) XFree(data);
        return ok == Success && type != 0;
    }
    int PidOf(Window w) const {
        Atom type = 0;
        int format = 0;
        unsigned long n = 0, after = 0;
        unsigned char *data = nullptr;
        int pid = -1;
        if (XGetWindowProperty(dpy_, w, net_wm_pid_, 0, 1, False, XA_CARDINAL, &type, &format, &n, &after, &data) == Success &&
            data && format == 32 && n == 1)
            pid = static_cast<int>(*reinterpret_cast<unsigned long *>(data));
        if (data) XFree(data);
        return pid;
    }
    Window TopLevelOf(Window w) const {
        for (int depth = 0; w != 0 && depth < 32; ++depth) {
            Window root = 0, parent = 0, *kids = nullptr;
            unsigned int n = 0;
            if (!XQueryTree(dpy_, w, &root, &parent, &kids, &n)) return w;
            if (kids) XFree(kids);
            if (parent == root || parent == 0) return w;
            w = parent;
        }
        return w;
    }
    // The client window inside a top-level: the one with WM_STATE (set by
    // a window manager on each window it manages), else the top-level
    // itself.
    Window ClientIn(Window top, int depth) const {
        if (HasProperty(top, wm_state_)) return top;
        if (depth < 3) {
            Window root = 0, parent = 0, *kids = nullptr;
            unsigned int n = 0;
            if (XQueryTree(dpy_, top, &root, &parent, &kids, &n) && kids) {
                Window found = 0;
                for (unsigned int k = n; k-- > 0 && !found;) found = ClientIn(kids[k], depth + 1);
                XFree(kids);
                if (found) return found;
            }
        }
        // No WM_STATE anywhere below: no window manager framed it, so the
        // top-level is the client itself (even one that sets no
        // properties at all -- a bare Xlib window).
        return depth == 0 ? top : 0;
    }
    // Every viewable, ordinary client window on the screen, top of the
    // stack first.
    std::vector<Window> ClientWindows() const {
        std::vector<Window> out;
        Window root = 0, parent = 0, *kids = nullptr;
        unsigned int n = 0;
        if (!XQueryTree(dpy_, root_, &root, &parent, &kids, &n) || !kids) return out;
        for (unsigned int k = n; k-- > 0;) {
            const Window top = kids[k];
            if (top == own_top_) continue;
            XWindowAttributes a;
            if (!XGetWindowAttributes(dpy_, top, &a) || a.override_redirect || a.map_state != IsViewable || a.c_class == InputOnly)
                continue;
            const Window client = ClientIn(top, 0);
            if (client && client != parent_) out.push_back(client);
        }
        XFree(kids);
        return out;
    }

    Display *dpy_;
    Window parent_, root_, own_top_ = 0;
    std::string display_name_;
    Atom wm_state_ = 0, net_wm_pid_ = 0, net_wm_name_ = 0, utf8_ = 0, wm_protocols_ = 0, wm_delete_ = 0;
    std::set<Window> known_;
    std::map<Window, X11Window *> windows_;  // the adopted window and its container -> which
};

class X11Window final : public EmbeddedWindow {
public:
    X11Window(X11Backend &be, Window win) : be_(be), win_(win) {
        Display *d = be_.dpy();
        XSetWindowAttributes sa;
        std::memset(&sa, 0, sizeof sa);
        sa.background_pixel = BlackPixel(d, DefaultScreen(d));
        sa.event_mask = KeyPressMask;  // the Ctrl-\ grab reports here
        container_ = XCreateWindow(d, be_.parent(), 0, 0, 1, 1, 0, CopyFromParent, InputOutput, CopyFromParent,
                                   CWBackPixel | CWEventMask, &sa);
        XSetWindowAttributes ia;
        std::memset(&ia, 0, sizeof ia);
        shield_ = XCreateWindow(d, be_.parent(), 0, 0, 1, 1, 0, 0, InputOnly, CopyFromParent, 0, &ia);
        // Its own structure events: destroyed (the program closed it or
        // exited), resized or moved by the program itself.
        XSelectInput(d, win_, StructureNotifyMask | PropertyChangeMask);
        be_.Register(win_, this);
        be_.Register(container_, this);
        // Let go by any window manager, then taken in.
        XWithdrawWindow(d, win_, DefaultScreen(d));
        XSync(d, False);
        // (A window manager takes its frame away asynchronously; a late
        // one is handled by Reparented.)
        for (int k = 0; k < 100 && ParentOf(win_) != be_.root() && ParentOf(win_) != 0; ++k)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        XReparentWindow(d, win_, container_, 0, 0);
        XMapWindow(d, win_);
        XSync(d, False);
    }
    ~X11Window() override {
        Display *d = be_.dpy();
        be_.Unregister(this);
        if (grabbed_) Grab(false);
        // Never destroyed along with the container: a window still in it
        // is handed back to the screen (its program is being stopped, or
        // has already lost it).
        if (ParentOf(win_) == container_) {
            XUnmapWindow(d, win_);
            XReparentWindow(d, win_, be_.root(), 0, 0);
        }
        if (focused_ && !dead_) XSetInputFocus(d, be_.parent(), RevertToParent, CurrentTime);
        XDestroyWindow(d, shield_);
        XDestroyWindow(d, container_);
        XSync(d, False);
    }

    void MarkDead() { dead_ = true; }
    // A ReparentNotify for the window. Only where it is *now* counts (the
    // event may be stale): in the container, fine; back at the root --
    // a window manager finishing the withdraw after we had already taken
    // it -- taken in again; anywhere else, someone else has it.
    void Reparented() {
        if (dead_) return;
        const Window now = ParentOf(win_);
        if (now == container_) return;
        if (now == be_.root() && retakes_ < 5) {
            ++retakes_;
            Display *d = be_.dpy();
            XReparentWindow(d, win_, container_, inner_rect_.x, inner_rect_.y);
            XMapWindow(d, win_);
            geometry_ok_ = false;
            return;
        }
        dead_ = true;
    }
    void MarkMoved() { geometry_ok_ = false; }
    void MarkReleaseRequest() { release_requested_ = true; }

    bool Alive() override { return !dead_; }

    void Place(const Rect &full, const Rect &clip) override {
        if (dead_) return;
        // The part of the window on screen: the container's geometry.
        Rect c;
        c.x = std::max(full.x, clip.x);
        c.y = std::max(full.y, clip.y);
        c.w = std::min(full.x + full.w, clip.x + clip.w) - c.x;
        c.h = std::min(full.y + full.h, clip.y + clip.h) - c.y;
        if (c.Empty()) {
            Hide();
            return;
        }
        Display *d = be_.dpy();
        if (c != container_rect_) {
            XMoveResizeWindow(d, container_, c.x, c.y, static_cast<unsigned>(c.w), static_cast<unsigned>(c.h));
            XMoveResizeWindow(d, shield_, c.x, c.y, static_cast<unsigned>(c.w), static_cast<unsigned>(c.h));
            container_rect_ = c;
        }
        const Rect inner{full.x - c.x, full.y - c.y, full.w, full.h};
        if (inner != inner_rect_ || !geometry_ok_) {
            XMoveResizeWindow(d, win_, inner.x, inner.y, static_cast<unsigned>(full.w), static_cast<unsigned>(full.h));
            inner_rect_ = inner;
            geometry_ok_ = true;
        }
        if (!shown_) {
            XMapWindow(d, container_);
            shown_ = true;
        }
        const bool shield = !focused_;
        if (shield != shield_up_) {
            if (shield) {
                XMapRaised(d, shield_);
            } else {
                XUnmapWindow(d, shield_);
            }
            shield_up_ = shield;
        }
    }

    void Hide() override {
        if (!shown_) return;
        Display *d = be_.dpy();
        XUnmapWindow(d, shield_);
        XUnmapWindow(d, container_);
        shown_ = shield_up_ = false;
    }

    void Focus(bool on) override {
        if (dead_) return;
        Display *d = be_.dpy();
        focused_ = on;
        if (on) {
            if (shield_up_) {
                XUnmapWindow(d, shield_);
                shield_up_ = false;
            }
            XSetInputFocus(d, win_, RevertToParent, CurrentTime);
            Grab(true);
        } else {
            Grab(false);
            if (shown_ && !shield_up_) {
                XMapRaised(d, shield_);
                shield_up_ = true;
            }
            // Back to mep -- unless the user has already put the focus
            // somewhere else entirely.
            Window f = 0;
            int revert = 0;
            XGetInputFocus(d, &f, &revert);
            if (f == win_ || f == container_ || be_.IsInside(f, win_)) XSetInputFocus(d, be_.parent(), RevertToParent, CurrentTime);
        }
        XFlush(d);
    }

    bool HasFocus() override {
        if (dead_) return false;
        Window f = 0;
        int revert = 0;
        XGetInputFocus(be_.dpy(), &f, &revert);
        return f == win_ || be_.IsInside(f, win_);
    }

    bool TakeReleaseRequest() override {
        const bool r = release_requested_;
        release_requested_ = false;
        return r;
    }

    bool RequestClose() override {
        if (dead_) return false;
        Display *d = be_.dpy();
        Atom *protocols = nullptr;
        int n = 0;
        bool takes_delete = false;
        if (XGetWMProtocols(d, win_, &protocols, &n)) {
            for (int k = 0; k < n; ++k) takes_delete = takes_delete || protocols[k] == be_.wm_delete();
            XFree(protocols);
        }
        if (!takes_delete) return false;
        XEvent ev;
        std::memset(&ev, 0, sizeof ev);
        ev.xclient.type = ClientMessage;
        ev.xclient.window = win_;
        ev.xclient.message_type = be_.wm_protocols();
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = static_cast<long>(be_.wm_delete());
        ev.xclient.data.l[1] = CurrentTime;
        XSendEvent(d, win_, False, NoEventMask, &ev);
        XFlush(d);
        return true;
    }

    Snapshot Capture() override {
        Snapshot s;
        if (dead_ || !shown_) return s;
        Display *d = be_.dpy();
        XWindowAttributes a;
        if (!XGetWindowAttributes(d, win_, &a) || a.map_state != IsViewable || a.width <= 0 || a.height <= 0) return s;
        // Only the part inside the container is on screen (the rest has no
        // defined pixels): in the window's own coordinates.
        const int vx = std::max(0, -inner_rect_.x), vy = std::max(0, -inner_rect_.y);
        const int vw = std::min(a.width, container_rect_.w - inner_rect_.x) - vx;
        const int vh = std::min(a.height, container_rect_.h - inner_rect_.y) - vy;
        if (vw <= 0 || vh <= 0) return s;
        a.width = vw;
        a.height = vh;
        g_last_error = 0;
        XImage *img = XGetImage(d, win_, vx, vy, static_cast<unsigned>(vw), static_cast<unsigned>(vh), AllPlanes, ZPixmap);
        XSync(d, False);
        if (!img || g_last_error != 0) {
            if (img) XDestroyImage(img);
            return s;
        }
        s.width = a.width;
        s.height = a.height;
        s.rgba.resize(static_cast<size_t>(a.width) * static_cast<size_t>(a.height) * 4);
        const unsigned long rm = img->red_mask, gm = img->green_mask, bm = img->blue_mask;
        auto shift_of = [](unsigned long m) {
            int sh = 0;
            while (m && !(m & 1)) {
                m >>= 1;
                ++sh;
            }
            return sh;
        };
        auto scale = [](unsigned long v, unsigned long m) {
            // `v` already shifted down; `m` the shifted mask.
            return m ? static_cast<unsigned char>((v * 255) / m) : static_cast<unsigned char>(0);
        };
        const int rs = shift_of(rm), gs = shift_of(gm), bs = shift_of(bm);
        const bool fast = img->bits_per_pixel == 32 && rm == 0xff0000 && gm == 0xff00 && bm == 0xff && img->byte_order == LSBFirst;
        for (int y = 0; y < a.height; ++y) {
            unsigned char *out = s.rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(a.width) * 4;
            if (fast) {
                const unsigned char *row = reinterpret_cast<const unsigned char *>(img->data) + static_cast<size_t>(y) * static_cast<size_t>(img->bytes_per_line);
                for (int x = 0; x < a.width; ++x) {
                    out[x * 4 + 0] = row[x * 4 + 2];
                    out[x * 4 + 1] = row[x * 4 + 1];
                    out[x * 4 + 2] = row[x * 4 + 0];
                    out[x * 4 + 3] = 255;
                }
                continue;
            }
            for (int x = 0; x < a.width; ++x) {
                const unsigned long p = XGetPixel(img, x, y);
                out[x * 4 + 0] = scale((p & rm) >> rs, rm >> rs);
                out[x * 4 + 1] = scale((p & gm) >> gs, gm >> gs);
                out[x * 4 + 2] = scale((p & bm) >> bs, bm >> bs);
                out[x * 4 + 3] = 255;
            }
        }
        XDestroyImage(img);
        return s;
    }

    std::string Title() override {
        if (dead_) return "";
        Display *d = be_.dpy();
        Atom type = 0;
        int format = 0;
        unsigned long n = 0, after = 0;
        unsigned char *data = nullptr;
        std::string title;
        if (XGetWindowProperty(d, win_, be_.net_wm_name(), 0, 256, False, be_.utf8(), &type, &format, &n, &after, &data) == Success &&
            data && format == 8)
            title.assign(reinterpret_cast<const char *>(data), n);
        if (data) XFree(data);
        if (title.empty()) {
            char *name = nullptr;
            if (XFetchName(d, win_, &name) && name) {
                title = name;
                XFree(name);
            }
        }
        return title;
    }

    Window win() const { return win_; }
    Window container() const { return container_; }

private:
    // The window's parent now, 0 if it is gone.
    Window ParentOf(Window w) const {
        Window root = 0, parent = 0, *kids = nullptr;
        unsigned int n = 0;
        if (!XQueryTree(be_.dpy(), w, &root, &parent, &kids, &n)) return 0;
        if (kids) XFree(kids);
        return parent;
    }
    // Ctrl-\ (with or without Caps Lock / Num Lock) while the program has
    // the keyboard: reported to this backend's connection, on the container.
    void Grab(bool on) {
        Display *d = be_.dpy();
        const KeyCode kc = XKeysymToKeycode(d, XK_backslash);
        if (kc == 0) return;
        const unsigned int locks[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
        for (unsigned int lock : locks) {
            if (on) XGrabKey(d, kc, ControlMask | lock, container_, False, GrabModeAsync, GrabModeAsync);
            else XUngrabKey(d, kc, ControlMask | lock, container_);
        }
        grabbed_ = on;
    }

    X11Backend &be_;
    Window win_, container_ = 0, shield_ = 0;
    Rect container_rect_, inner_rect_;
    bool shown_ = false, shield_up_ = false, focused_ = false, grabbed_ = false;
    bool dead_ = false, geometry_ok_ = false, release_requested_ = false;
    int retakes_ = 0;
};

std::unique_ptr<EmbeddedWindow> X11Backend::Adopt(const std::vector<int> &pids, bool allow_unowned) {
    Window best = 0;
    long best_area = -1;
    for (Window w : ClientWindows()) {
        const int pid = PidOf(w);
        const bool mine = pid > 0 ? std::find(pids.begin(), pids.end(), pid) != pids.end()
                                  : allow_unowned && known_.count(w) == 0;
        if (!mine) continue;
        Window transient_for = 0;
        if (XGetTransientForHint(dpy_, w, &transient_for) && transient_for != 0) continue;  // a dialog, not its main window
        XWindowAttributes a;
        if (!XGetWindowAttributes(dpy_, w, &a) || a.width <= 1 || a.height <= 1) continue;
        const long area = static_cast<long>(a.width) * a.height;
        if (area > best_area) {
            best = w;
            best_area = area;
        }
    }
    if (!best) return nullptr;
    return std::make_unique<X11Window>(*this, best);
}

void X11Backend::Pump() {
    while (XPending(dpy_) > 0) {
        XEvent ev;
        XNextEvent(dpy_, &ev);
        auto it = windows_.find(ev.xany.window);
        if (it == windows_.end()) continue;
        X11Window *w = it->second;
        switch (ev.type) {
            case DestroyNotify:
                if (ev.xdestroywindow.window == w->win()) w->MarkDead();
                break;
            case ConfigureNotify:
                // The program moved or resized itself: put it back next frame.
                if (ev.xconfigure.window == w->win()) w->MarkMoved();
                break;
            case ReparentNotify:
                if (ev.xreparent.window == w->win()) w->Reparented();
                break;
            case KeyPress: w->MarkReleaseRequest(); break;
            default: break;
        }
    }
}

}  // namespace

std::unique_ptr<Backend> CreateX11Backend(void *native_window_handle, std::string *why) {
    const auto *handle = static_cast<const gfx::NativeWindowHandle *>(native_window_handle);
    if (!handle || !handle->display || !handle->window) {
        if (why) *why = "mep's window is not an X11 window";
        return nullptr;
    }
    const char *name = DisplayString(static_cast<Display *>(handle->display));
    Display *dpy = XOpenDisplay(name);
    if (!dpy) {
        if (why) *why = std::string("cannot connect to the X server ") + (name ? name : "");
        return nullptr;
    }
    return std::make_unique<X11Backend>(dpy, static_cast<Window>(handle->window), name ? name : "");
}

}  // namespace mep::gui_embed

#endif  // MEP_GUI_EMBED_X11
