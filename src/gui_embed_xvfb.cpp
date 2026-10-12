// A gui_embed.h implementor that needs no windowing system's help: every
// program gets a private X server of its own (Xvfb, a server that draws
// into memory and shows nothing), and mep shows that server's screen.
//
//   1. ChildEnv starts an Xvfb as big as any pane can be and hands the
//      program its DISPLAY; ChildStarted ties the server to the program.
//   2. Adopt takes the program's main window: on a server of its own, any
//      ordinary top-level window is the program's.
//   3. Place sizes the server's screen (RandR) and the main window to the
//      block's area. The whole screen is the picture, so the program's
//      menus and dialogs show inside the block too (no window manager:
//      a dialog is wherever its program puts it on that small screen).
//   4. LiveFrame is the screen's pixels, read back where DAMAGE says they
//      changed; mep draws them as a texture (DrawnByMep).
//   5. Pointer and keys are mep's: the editor passes them on (Pointer*,
//      Key, Text), and XTEST plays them to the server. mep keeps the
//      keyboard throughout -- its own chords never reach the program.
//
// Only X11 programs draw here (Tk built for X11, xterm, GTK/Qt built for
// X11 ...). On macOS that excludes every ordinary Mac app, which ignores
// DISPLAY and opens a window of its own: the layered backend
// (CreateLayeredBackend) then adopts that one through Accessibility.
//
// Xlib and its extensions are loaded at run time (dlopen), so mep starts
// whether or not they are installed: on macOS they come with XQuartz
// (/opt/X11), as does Xvfb. MEP_XVFB names another Xvfb.

#include "gui_embed.h"

#if defined(MEP_GUI_EMBED_XVFB)

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrandr.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "frame_activity.h"

extern char **environ;

namespace mep::gui_embed {
namespace {

// The largest screen a program gets (RandR shrinks it to the block; it
// cannot grow past what the server started with).
constexpr int kMaxScreenW = 3840, kMaxScreenH = 2400;
// How long a new server has to come up.
constexpr int kServerStartMs = 5000;

// --- Xlib, loaded at run time ------------------------------------------------

struct Xlib {
#define MEP_X(name) decltype(&::name) name = nullptr;
    MEP_X(XOpenDisplay)
    MEP_X(XCloseDisplay)
    MEP_X(XDefaultRootWindow)
    MEP_X(XFlush)
    MEP_X(XSync)
    MEP_X(XPending)
    MEP_X(XNextEvent)
    MEP_X(XFree)
    MEP_X(XQueryTree)
    MEP_X(XGetWindowAttributes)
    MEP_X(XMoveResizeWindow)
    MEP_X(XGetImage)
    MEP_X(XInternAtom)
    MEP_X(XGetWindowProperty)
    MEP_X(XSendEvent)
    MEP_X(XSetInputFocus)
    MEP_X(XKeysymToKeycode)
    MEP_X(XGetKeyboardMapping)
    MEP_X(XChangeKeyboardMapping)
    MEP_X(XDisplayKeycodes)
    MEP_X(XSelectInput)
    MEP_X(XSetErrorHandler)
    MEP_X(XSetIOErrorHandler)
    MEP_X(XSetIOErrorExitHandler)
    MEP_X(XFetchName)
    MEP_X(XGetTransientForHint)
    MEP_X(XTestQueryExtension)
    MEP_X(XTestFakeKeyEvent)
    MEP_X(XTestFakeButtonEvent)
    MEP_X(XTestFakeMotionEvent)
    MEP_X(XDamageQueryExtension)
    MEP_X(XDamageCreate)
    MEP_X(XDamageSubtract)
    MEP_X(XRRSetScreenSize)
#undef MEP_X
    bool ok = false;
    std::string why;
};

void *OpenLibrary(std::initializer_list<const char *> names) {
    for (const char *n : names)
        if (void *h = dlopen(n, RTLD_NOW | RTLD_LOCAL)) return h;
    return nullptr;
}

const Xlib &X() {
    static Xlib x = [] {
        Xlib l;
#if defined(__APPLE__)
        void *x11 = OpenLibrary({"/opt/X11/lib/libX11.6.dylib", "libX11.6.dylib"});
        void *xtst = OpenLibrary({"/opt/X11/lib/libXtst.6.dylib", "libXtst.6.dylib"});
        void *xdamage = OpenLibrary({"/opt/X11/lib/libXdamage.1.dylib", "libXdamage.1.dylib"});
        void *xrandr = OpenLibrary({"/opt/X11/lib/libXrandr.2.dylib", "libXrandr.2.dylib"});
#else
        void *x11 = OpenLibrary({"libX11.so.6"});
        void *xtst = OpenLibrary({"libXtst.so.6"});
        void *xdamage = OpenLibrary({"libXdamage.so.1"});
        void *xrandr = OpenLibrary({"libXrandr.so.2"});
#endif
        if (!x11 || !xtst || !xdamage || !xrandr) {
            l.why = "Xlib is not installed (on macOS: install XQuartz, https://www.xquartz.org)";
            return l;
        }
        bool all = true;
        auto load = [&all](void *lib, const char *name, auto &fn) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
            if (!fn) all = false;
        };
#define MEP_X(lib, name) load(lib, #name, l.name);
        MEP_X(x11, XOpenDisplay)
        MEP_X(x11, XCloseDisplay)
        MEP_X(x11, XDefaultRootWindow)
        MEP_X(x11, XFlush)
        MEP_X(x11, XSync)
        MEP_X(x11, XPending)
        MEP_X(x11, XNextEvent)
        MEP_X(x11, XFree)
        MEP_X(x11, XQueryTree)
        MEP_X(x11, XGetWindowAttributes)
        MEP_X(x11, XMoveResizeWindow)
        MEP_X(x11, XGetImage)
        MEP_X(x11, XInternAtom)
        MEP_X(x11, XGetWindowProperty)
        MEP_X(x11, XSendEvent)
        MEP_X(x11, XSetInputFocus)
        MEP_X(x11, XKeysymToKeycode)
        MEP_X(x11, XGetKeyboardMapping)
        MEP_X(x11, XChangeKeyboardMapping)
        MEP_X(x11, XDisplayKeycodes)
        MEP_X(x11, XSelectInput)
        MEP_X(x11, XSetErrorHandler)
        MEP_X(x11, XSetIOErrorHandler)
        MEP_X(x11, XSetIOErrorExitHandler)
        MEP_X(x11, XFetchName)
        MEP_X(x11, XGetTransientForHint)
        MEP_X(xtst, XTestQueryExtension)
        MEP_X(xtst, XTestFakeKeyEvent)
        MEP_X(xtst, XTestFakeButtonEvent)
        MEP_X(xtst, XTestFakeMotionEvent)
        MEP_X(xdamage, XDamageQueryExtension)
        MEP_X(xdamage, XDamageCreate)
        MEP_X(xdamage, XDamageSubtract)
        MEP_X(xrandr, XRRSetScreenSize)
#undef MEP_X
        // (XSetIOErrorExitHandler is libX11 1.7+: without it a server
        // that dies would take mep with it.)
        if (!all) {
            l.why = "the installed Xlib is too old (libX11 1.7 or newer is needed)";
            return l;
        }
        // Requests about windows that have just gone are expected here
        // (a program closing while it is being looked at): ignored.
        l.XSetErrorHandler([](Display *, XErrorEvent *) { return 0; });
        l.XSetIOErrorHandler([](Display *) { return 0; });
        l.ok = true;
        return l;
    }();
    return x;
}

std::string FindXvfb() {
    if (const char *env = std::getenv("MEP_XVFB"); env && *env) return env;
    std::string p = FindProgram("Xvfb");
    if (!p.empty()) return p;
#if defined(__APPLE__)
    if (access("/opt/X11/bin/Xvfb", X_OK) == 0) return "/opt/X11/bin/Xvfb";
#endif
    return "";
}

// Servers stopped but not yet reaped (Pump reaps them).
std::vector<pid_t> &Stopped() {
    static std::vector<pid_t> v;
    return v;
}

// --- One program's server ----------------------------------------------------

struct Session {
    pid_t server = -1;
    std::string number;  // its display number
    Display *dpy = nullptr;
    Window root = 0;
    int damage_event = 0;
    Damage damage = 0;
    bool dead = false;  // the connection is gone: never touched again
    int width = 0, height = 0;
    // The screen's pixels, and what has changed since they were read.
    Snapshot frame;
    uint64_t version = 0;
    bool all_dirty = true;
    std::vector<XRectangle> dirty;
    std::vector<Window> mapped;  // top-level windows mapped since last asked (ShowNewest)
    Atom wm_protocols = 0, wm_delete = 0, net_wm_name = 0, utf8 = 0;
    // Characters with no key of their own are typed on spare keycodes,
    // each kept for its character while it is the most recent ones'.
    std::vector<KeyCode> spare;
    std::vector<KeySym> spare_sym;
    size_t spare_next = 0;

    // Wakes mep's main loop whenever the server has something to say (a
    // change to the picture, mostly): an idle mep sleeps between frames,
    // and a program that changes by itself (a clock, an animation) would
    // otherwise only show it at the idle frame rate. Re-armed by Drain, so
    // that unread data does not wake it again and again.
    std::thread watcher;
    std::atomic<bool> stop{false}, armed{true};

    void Watch() {
        const int fd = reinterpret_cast<_XPrivDisplay>(dpy)->fd;  // (ConnectionNumber, without its C cast)
        watcher = std::thread([this, fd] {
            while (!stop.load()) {
                if (!armed.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(4));
                    continue;
                }
                pollfd p{fd, POLLIN, 0};
                if (poll(&p, 1, 100) > 0 && !stop.load()) {
                    armed.store(false);
                    mep::NoteActivity();
                    mep::WakeMainLoop();
                }
            }
        });
    }

    ~Session() {
        stop.store(true);
        if (watcher.joinable()) watcher.join();
        if (dpy && !dead) X().XCloseDisplay(dpy);
        if (server > 0) {
            kill(server, SIGTERM);
            Stopped().push_back(server);
        }
    }

    void Drain() {
        if (dead) return;
        const Xlib &x = X();
        while (!dead && x.XPending(dpy) > 0) {
            XEvent ev;
            x.XNextEvent(dpy, &ev);
            if (ev.type == damage_event + XDamageNotify) {
                const auto &d = reinterpret_cast<const XDamageNotifyEvent &>(ev);
                dirty.push_back(XRectangle{d.area.x, d.area.y, d.area.width, d.area.height});
                if (dirty.size() > 32) all_dirty = true;
            } else if (ev.type == MapNotify && !ev.xmap.override_redirect) {
                mapped.push_back(ev.xmap.window);
            }
        }
        armed.store(true);
    }

    // Reads back what changed: true when the picture did.
    bool Refresh() {
        if (dead || width <= 0 || height <= 0) return false;
        const Xlib &x = X();
        if (damage) x.XDamageSubtract(dpy, damage, 0, 0);
        if (frame.width != width || frame.height != height) {
            frame.width = width;
            frame.height = height;
            frame.rgba.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0);
            all_dirty = true;
        }
        if (!all_dirty && dirty.empty()) return false;
        std::vector<XRectangle> rects;
        if (all_dirty) rects.push_back(XRectangle{0, 0, static_cast<unsigned short>(width), static_cast<unsigned short>(height)});
        else rects.swap(dirty);
        all_dirty = false;
        dirty.clear();
        bool changed = false;
        for (const XRectangle &r : rects) {
            const int x0 = std::max(0, static_cast<int>(r.x)), y0 = std::max(0, static_cast<int>(r.y));
            const int x1 = std::min(width, r.x + static_cast<int>(r.width)), y1 = std::min(height, r.y + static_cast<int>(r.height));
            if (x1 <= x0 || y1 <= y0) continue;
            XImage *img = x.XGetImage(dpy, root, x0, y0, static_cast<unsigned>(x1 - x0), static_cast<unsigned>(y1 - y0), ~0UL /* AllPlanes */, ZPixmap);
            if (!img) continue;
            if (img->bits_per_pixel == 32) {
                const bool lsb = img->byte_order == LSBFirst;
                for (int row = 0; row < y1 - y0; ++row) {
                    const auto *src = reinterpret_cast<const unsigned char *>(img->data) + static_cast<size_t>(row) * static_cast<size_t>(img->bytes_per_line);
                    unsigned char *dst = frame.rgba.data() + (static_cast<size_t>(y0 + row) * static_cast<size_t>(width) + static_cast<size_t>(x0)) * 4;
                    for (int col = 0; col < x1 - x0; ++col, src += 4, dst += 4) {
                        // 0x00RRGGBB in the server's byte order.
                        dst[0] = lsb ? src[2] : src[1];
                        dst[1] = lsb ? src[1] : src[2];
                        dst[2] = lsb ? src[0] : src[3];
                        dst[3] = 255;
                    }
                }
                changed = true;
            }
            // (XDestroyImage is a macro over the image's own function.)
            img->f.destroy_image(img);
        }
        if (changed) ++version;
        return changed;
    }

    // The keycode that types `sym`, and whether Shift must be held for it.
    KeyCode CodeFor(KeySym sym, bool *shift) {
        const Xlib &x = X();
        *shift = false;
        KeyCode code = x.XKeysymToKeycode(dpy, sym);
        if (code) {
            int per = 0;
            KeySym *syms = x.XGetKeyboardMapping(dpy, code, 1, &per);
            if (syms) {
                if (per > 1 && syms[0] != sym && syms[1] == sym) *shift = true;
                x.XFree(syms);
            }
            return code;
        }
        // No key has it: a spare keycode is given it.
        if (spare.empty()) {
            int lo = 0, hi = 0;
            x.XDisplayKeycodes(dpy, &lo, &hi);
            int per = 0;
            KeySym *syms = x.XGetKeyboardMapping(dpy, static_cast<KeyCode>(lo), hi - lo + 1, &per);
            if (!syms) return 0;
            for (int c = hi; c >= lo && spare.size() < 8; --c) {
                bool empty = true;
                for (int k = 0; k < per; ++k)
                    if (syms[(c - lo) * per + k] != NoSymbol) empty = false;
                if (empty) spare.push_back(static_cast<KeyCode>(c));
            }
            x.XFree(syms);
            spare_sym.assign(spare.size(), NoSymbol);
            if (spare.empty()) return 0;
        }
        for (size_t k = 0; k < spare.size(); ++k)
            if (spare_sym[k] == sym) return spare[k];
        const size_t k = spare_next++ % spare.size();
        KeySym both[2] = {sym, sym};
        x.XChangeKeyboardMapping(dpy, spare[k], 2, both, 1);
        x.XSync(dpy, False);
        spare_sym[k] = sym;
        return spare[k];
    }
};

void OnConnectionLost(Display *, void *data) { static_cast<Session *>(data)->dead = true; }

// Starts a server; null (and `why`) when it would not come up.
std::shared_ptr<Session> StartServer(const std::string &xvfb, std::string *why) {
    int fds[2];
    if (pipe(fds) != 0) {
        *why = std::strerror(errno);
        return nullptr;
    }
    // -displayfd: the server picks a free display and writes its number
    // once it takes connections. -terminate: it goes when its last client
    // does (mep's own connection included -- no server outlives mep).
    const std::vector<std::string> args = {xvfb, "-displayfd", "3", "-screen", "0",
                                           std::to_string(kMaxScreenW) + "x" + std::to_string(kMaxScreenH) + "x24",
                                           "-nolisten", "tcp", "-br", "-terminate", "-dpi", "96"};
    std::vector<char *> argv;
    for (const std::string &a : args) argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    // (In this order: the read end may itself be fd 3.)
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 3);
    if (fds[1] != 3) posix_spawn_file_actions_addclose(&fa, fds[1]);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid = -1;
    const int rc = posix_spawn(&pid, xvfb.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        *why = "could not start " + xvfb + ": " + std::strerror(rc);
        return nullptr;
    }
    auto s = std::make_shared<Session>();
    s->server = pid;
    std::string number;
    pollfd p{fds[0], POLLIN, 0};
    while (number.find('\n') == std::string::npos && poll(&p, 1, kServerStartMs) > 0) {
        char buf[16];
        const ssize_t n = read(fds[0], buf, sizeof buf);
        if (n <= 0) break;
        number.append(buf, static_cast<size_t>(n));
    }
    close(fds[0]);
    while (!number.empty() && (number.back() == '\n' || number.back() == ' ')) number.pop_back();
    if (number.empty()) {
        *why = xvfb + " did not start";
        return nullptr;  // (~Session stops it)
    }
    const Xlib &x = X();
    const std::string display = ":" + number;
    s->dpy = x.XOpenDisplay(display.c_str());
    if (!s->dpy) {
        *why = "could not connect to " + xvfb + " on " + display;
        return nullptr;
    }
    x.XSetIOErrorExitHandler(s->dpy, OnConnectionLost, s.get());
    int ev = 0, err = 0, major = 0, minor = 0;
    if (!x.XTestQueryExtension(s->dpy, &ev, &err, &major, &minor) || !x.XDamageQueryExtension(s->dpy, &s->damage_event, &err)) {
        *why = xvfb + " lacks the XTEST or DAMAGE extension";
        return nullptr;
    }
    s->root = x.XDefaultRootWindow(s->dpy);
    s->damage = x.XDamageCreate(s->dpy, s->root, XDamageReportRawRectangles);
    x.XSelectInput(s->dpy, s->root, SubstructureNotifyMask);
    s->wm_protocols = x.XInternAtom(s->dpy, "WM_PROTOCOLS", False);
    s->wm_delete = x.XInternAtom(s->dpy, "WM_DELETE_WINDOW", False);
    s->net_wm_name = x.XInternAtom(s->dpy, "_NET_WM_NAME", False);
    s->utf8 = x.XInternAtom(s->dpy, "UTF8_STRING", False);
    s->width = kMaxScreenW;
    s->height = kMaxScreenH;
    x.XSync(s->dpy, False);
    s->number = number;
    s->Watch();
    return s;
}

// --- The program's window ------------------------------------------------------

class XvfbWindow final : public EmbeddedWindow {
public:
    XvfbWindow(std::shared_ptr<Session> s, Window win) : s_(std::move(s)), win_(win) {}

    bool DrawnByMep() const override { return true; }

    bool Alive() override {
        if (s_->dead) return false;
        XWindowAttributes a;
        return X().XGetWindowAttributes(s_->dpy, win_, &a) != 0;
    }

    void Place(const Rect &full, const Rect &clip) override {
        (void)clip;  // (mep draws only the part of the picture inside it)
        if (s_->dead || full.Empty()) return;
        const int w = std::min(full.w, kMaxScreenW), h = std::min(full.h, kMaxScreenH);
        if (w == placed_w_ && h == placed_h_) return;
        placed_w_ = w;
        placed_h_ = h;
        const Xlib &x = X();
        // The screen is the block: what the program puts anywhere on it
        // (a menu, a dialog) shows there.
        x.XRRSetScreenSize(s_->dpy, s_->root, w, h, w * 254 / 960, h * 254 / 960);
        x.XMoveResizeWindow(s_->dpy, win_, 0, 0, static_cast<unsigned>(w), static_cast<unsigned>(h));
        x.XFlush(s_->dpy);
        s_->width = w;
        s_->height = h;
        s_->all_dirty = true;
    }

    void Hide() override {}

    void Focus(bool on) override {
        focused_ = on;
        if (on && !s_->dead) {
            X().XSetInputFocus(s_->dpy, FocusTarget(), PointerRoot /* RevertToPointerRoot */, CurrentTime);
            X().XFlush(s_->dpy);
        }
    }
    bool HasFocus() override { return focused_ && !s_->dead; }
    bool TakeReleaseRequest() override { return false; }

    bool RequestClose() override {
        if (s_->dead) return false;
        const Xlib &x = X();
        Atom *protocols = nullptr;
        bool takes_delete = false;
        Atom type = 0;
        int format = 0;
        unsigned long n = 0, after = 0;
        unsigned char *data = nullptr;
        if (x.XGetWindowProperty(s_->dpy, win_, s_->wm_protocols, 0, 64, False, 4 /* XA_ATOM */, &type, &format, &n, &after, &data) == Success && data) {
            protocols = reinterpret_cast<Atom *>(data);
            for (unsigned long k = 0; k < n; ++k)
                if (protocols[k] == s_->wm_delete) takes_delete = true;
            x.XFree(data);
        }
        if (!takes_delete) return false;
        XEvent ev{};
        ev.xclient.type = ClientMessage;
        ev.xclient.window = win_;
        ev.xclient.message_type = s_->wm_protocols;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = static_cast<long>(s_->wm_delete);
        ev.xclient.data.l[1] = CurrentTime;
        x.XSendEvent(s_->dpy, win_, False, NoEventMask, &ev);
        x.XFlush(s_->dpy);
        return true;
    }

    Snapshot Capture() override {
        uint64_t v = 0;
        const Snapshot *f = LiveFrame(&v);
        return f ? *f : Snapshot{};
    }

    std::string Title() override {
        if (s_->dead) return "";
        const Xlib &x = X();
        std::string title;
        Atom type = 0;
        int format = 0;
        unsigned long n = 0, after = 0;
        unsigned char *data = nullptr;
        if (x.XGetWindowProperty(s_->dpy, win_, s_->net_wm_name, 0, 256, False, s_->utf8, &type, &format, &n, &after, &data) == Success && data) {
            title.assign(reinterpret_cast<const char *>(data), n);
            x.XFree(data);
        }
        if (title.empty()) {
            char *name = nullptr;
            if (x.XFetchName(s_->dpy, win_, &name) && name) {
                title = name;
                x.XFree(name);
            }
        }
        return title;
    }

    const Snapshot *LiveFrame(uint64_t *version) override {
        s_->Drain();
        // A dialog (any new ordinary window) that opened while the program
        // has the keyboard is where the keys go next.
        if (!s_->mapped.empty()) {
            if (focused_) Focus(true);
            s_->mapped.clear();
        }
        s_->Refresh();
        if (version) *version = s_->version;
        return s_->frame.Empty() ? nullptr : &s_->frame;
    }

    void PointerMove(int px, int py) override {
        if (s_->dead) return;
        X().XTestFakeMotionEvent(s_->dpy, 0, px, py, CurrentTime);
        X().XFlush(s_->dpy);
    }
    void PointerButton(int button, bool down) override {
        if (s_->dead || button < 1) return;
        X().XTestFakeButtonEvent(s_->dpy, static_cast<unsigned>(button), down ? True : False, CurrentTime);
        X().XFlush(s_->dpy);
    }

    void Key(unsigned keysym, unsigned mods) override {
        if (s_->dead) return;
        const Xlib &x = X();
        bool shift = false;
        const KeyCode code = s_->CodeFor(keysym, &shift);
        if (!code) return;
        if (shift) mods |= kModShift;
        std::vector<KeyCode> held;
        auto hold = [&](unsigned bit, KeySym sym) {
            if (!(mods & bit)) return;
            bool unused = false;
            if (KeyCode c = s_->CodeFor(sym, &unused)) {
                x.XTestFakeKeyEvent(s_->dpy, c, True, CurrentTime);
                held.push_back(c);
            }
        };
        hold(kModShift, XK_Shift_L);
        hold(kModCtrl, XK_Control_L);
        hold(kModAlt, XK_Alt_L);
        hold(kModSuper, XK_Super_L);
        x.XTestFakeKeyEvent(s_->dpy, code, True, CurrentTime);
        x.XTestFakeKeyEvent(s_->dpy, code, False, CurrentTime);
        for (auto it = held.rbegin(); it != held.rend(); ++it) x.XTestFakeKeyEvent(s_->dpy, *it, False, CurrentTime);
        x.XFlush(s_->dpy);
    }

    void Text(char32_t cp) override {
        if (cp == '\r' || cp == '\n') return Key(XK_Return, 0);
        if (cp == '\t') return Key(XK_Tab, 0);
        if (cp < 0x20 || cp == 0x7f) return;
        // Latin-1 characters are their own keysyms; the rest are 0x01000000 + the code point.
        Key(cp < 0x100 ? static_cast<unsigned>(cp) : 0x01000000u + static_cast<unsigned>(cp), 0);
    }

private:
    // The newest ordinary mapped top-level window: a dialog over the main
    // window takes the keys before it.
    Window FocusTarget() {
        const Xlib &x = X();
        Window root = 0, parent = 0, *kids = nullptr, target = win_;
        unsigned n = 0;
        if (x.XQueryTree(s_->dpy, s_->root, &root, &parent, &kids, &n) && kids) {
            for (unsigned k = 0; k < n; ++k) {  // bottom to top
                XWindowAttributes a;
                if (x.XGetWindowAttributes(s_->dpy, kids[k], &a) && a.map_state == IsViewable && !a.override_redirect &&
                    a.c_class == InputOutput)
                    target = kids[k];
            }
            x.XFree(kids);
        }
        return target;
    }

    std::shared_ptr<Session> s_;
    Window win_;
    int placed_w_ = 0, placed_h_ = 0;
    bool focused_ = false;
};

// --- The backend -----------------------------------------------------------------

class XvfbBackend final : public Backend {
public:
    explicit XvfbBackend(std::string xvfb) : xvfb_(std::move(xvfb)) {}

    std::string Name() const override { return "xvfb"; }
    bool Usable(std::string *why) const override {
        (void)why;
        return true;
    }
    std::vector<std::pair<std::string, std::string>> ChildEnv() override {
        std::string why;
        pending_ = StartServer(xvfb_, &why);
        if (!pending_) {
            why_ = why;
            return {};
        }
        // A program with a choice (GTK, Qt, SDL) is told to draw on X11.
        return {{"DISPLAY", ":" + pending_->number}, {"GDK_BACKEND", "x11"}, {"QT_QPA_PLATFORM", "xcb"}, {"SDL_VIDEODRIVER", "x11"}};
    }
    void ChildStarted(int pid) override {
        if (pending_) sessions_[pid] = std::move(pending_);
        pending_.reset();
    }
    void ChildEnded(int pid) override { sessions_.erase(pid); }
    std::string WhyNoWindow() const override { return why_; }
    void NoteExistingWindows() override {}

    std::unique_ptr<EmbeddedWindow> Adopt(const std::vector<int> &pids, bool allow_unowned) override {
        (void)allow_unowned;  // (everything on its server is the program's)
        if (pids.empty()) return nullptr;
        auto it = sessions_.find(pids.front());
        if (it == sessions_.end() || it->second->dead) return nullptr;
        Session &s = *it->second;
        s.Drain();
        const Xlib &x = X();
        // Its largest ordinary top-level window that is no dialog.
        Window root = 0, parent = 0, *kids = nullptr, best = 0;
        unsigned n = 0;
        long best_area = 0;
        if (x.XQueryTree(s.dpy, s.root, &root, &parent, &kids, &n) && kids) {
            for (unsigned k = 0; k < n; ++k) {
                XWindowAttributes a;
                if (!x.XGetWindowAttributes(s.dpy, kids[k], &a) || a.map_state != IsViewable || a.override_redirect ||
                    a.c_class != InputOutput || a.width < 2 || a.height < 2)
                    continue;
                Window owner = 0;
                if (x.XGetTransientForHint(s.dpy, kids[k], &owner) && owner) continue;
                const long area = static_cast<long>(a.width) * a.height;
                if (area > best_area) {
                    best_area = area;
                    best = kids[k];
                }
            }
            x.XFree(kids);
        }
        if (!best) return nullptr;
        return std::make_unique<XvfbWindow>(it->second, best);
    }

    void Pump() override {
        // (Shown or not: what the servers say is read every frame.)
        for (auto &kv : sessions_) kv.second->Drain();
        auto &stopped = Stopped();
        stopped.erase(std::remove_if(stopped.begin(), stopped.end(), [](pid_t p) { return waitpid(p, nullptr, WNOHANG) != 0; }),
                      stopped.end());
    }
    void Flush() override {}

private:
    std::string xvfb_;
    std::shared_ptr<Session> pending_;
    std::map<int, std::shared_ptr<Session>> sessions_;
    std::string why_;
};

}  // namespace

std::unique_ptr<Backend> CreateXvfbBackend(std::string *why) {
    const Xlib &x = X();
    if (!x.ok) {
        if (why) *why = x.why;
        return nullptr;
    }
    const std::string xvfb = FindXvfb();
    if (xvfb.empty()) {
        if (why) *why = "there is no Xvfb (on macOS it comes with XQuartz, https://www.xquartz.org)";
        return nullptr;
    }
    return std::make_unique<XvfbBackend>(xvfb);
}

}  // namespace mep::gui_embed

#endif  // MEP_GUI_EMBED_XVFB
