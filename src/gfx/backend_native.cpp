#include "gfx/backend_native.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>

#include <sys/select.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>

// X11 headers #define a handful of short, common-word macros (None, Bool,
// True, False, Status, Above, Below, ...) at global scope -- `None` in
// particular collides textually with gfx::Key::None (an enum member of
// that exact name) used throughout this file and gfx/backend_native_internal.h's
// own includes below. Undefined immediately after the X11 includes; every
// intentional use of X11's `None` constant below uses the literal `0`/
// `0L` it expands to instead (XIDs, Atoms, and GLXContext are all
// integer/pointer types that accept a bare 0 the same way).
#undef None

// Deliberately NOT `#include <GL/glx.h>`: it unconditionally pulls in
// <GL/gl.h> (no include guard around that one line), whose GL_* token
// #defines collide with gfx/gl_loader.h's own `inline constexpr GLenum
// GL_POINTS = ...`-style declarations the same way raw <GL/gl.h> always
// would have -- exactly what gl_loader.h's own top comment says this
// codebase avoids for core GL. The handful of GLX entry points/constants
// this file actually calls are hand-declared below instead, the same
// "no vendored system GL header" pattern gl_loader.h already established
// -- values cross-checked against the real GLX 1.4/GLX_ARB_create_context
// headers, not guessed.
extern "C" {
struct GLXContextOpaque;
struct GLXFBConfigOpaque;
using GLXContext = GLXContextOpaque *;
using GLXFBConfig = GLXFBConfigOpaque *;
using GLXDrawable = XID;
using GLXExtFuncPtr = void (*)(void);

GLXFBConfig *glXChooseFBConfig(Display *dpy, int screen, const int *attrib_list, int *nelements);
XVisualInfo *glXGetVisualFromFBConfig(Display *dpy, GLXFBConfig config);
Bool glXMakeCurrent(Display *dpy, GLXDrawable drawable, GLXContext ctx);
void glXSwapBuffers(Display *dpy, GLXDrawable drawable);
void glXDestroyContext(Display *dpy, GLXContext ctx);
GLXExtFuncPtr glXGetProcAddressARB(const unsigned char *proc_name);
}

// GLX 1.3 FBConfig attribute names/values (glx.h).
constexpr int GLX_X_RENDERABLE = 0x8012;
constexpr int GLX_DRAWABLE_TYPE = 0x8010;
constexpr int GLX_WINDOW_BIT = 0x00000001;
constexpr int GLX_RENDER_TYPE = 0x8011;
constexpr int GLX_RGBA_BIT = 0x00000001;
constexpr int GLX_X_VISUAL_TYPE = 0x22;
constexpr int GLX_TRUE_COLOR = 0x8002;
constexpr int GLX_RED_SIZE = 8;
constexpr int GLX_GREEN_SIZE = 9;
constexpr int GLX_BLUE_SIZE = 10;
constexpr int GLX_ALPHA_SIZE = 11;
constexpr int GLX_DEPTH_SIZE = 12;
constexpr int GLX_DOUBLEBUFFER = 5;
// GLX_ARB_create_context (glxext.h) -- used to request a GL 3.3 core
// context via glXCreateContextAttribsARB, resolved at runtime through
// glXGetProcAddressARB below rather than linked directly (an ARB
// extension, not part of the guaranteed-exported GLX 1.x ABI).
constexpr int GLX_CONTEXT_MAJOR_VERSION_ARB = 0x2091;
constexpr int GLX_CONTEXT_MINOR_VERSION_ARB = 0x2092;
constexpr int GLX_CONTEXT_PROFILE_MASK_ARB = 0x9126;
constexpr int GLX_CONTEXT_CORE_PROFILE_BIT_ARB = 0x00000001;

#include "gfx/backend_native_internal.h"
#include "gfx/gl_loader.h"
#include "gfx/native_window_handle.h"

namespace gfx {

namespace {

// -- Key/button mapping (X11 KeySym-based) ---------------------------------
//
// gfx::Key's A..Z and Zero..Nine runs are each contiguous in alphabetical/
// numeric order (see gfx/types.h's own comment on why), and so are X11's
// XK_a..XK_z / XK_0..XK_9 (which are literally the ASCII lowercase-letter
// and digit code points -- X11's own keysymdef.h says so) -- both ranges
// map by simple offset arithmetic instead of a 36-case switch.
//
// KeySyms here are always looked up at "group 0, level 0" (XLookupKeysym's
// index 0 / plain XKeycodeToKeysym-style unshifted lookup) -- the physical-
// key identity a US layout's unshifted key produces, matching what
// GLFW_KEY_* meant before this backend replaced GLFW: "the A key",
// independent of whether Shift is held, not "the character typed" (that's
// GetCharPressed()'s job, via Xutf8LookupString below).
//
// Only this one direction (X11 -> gfx::Key) is needed: unlike the GLFW-era
// backend, key/mouse state below is indexed directly by gfx::Key's own
// ordinal (a small dense enum already), not by a backend-native code, so
// there's no reverse gfx::Key -> KeySym mapping to maintain.

gfx::Key UnmapKey(KeySym sym) {
    if (sym >= XK_a && sym <= XK_z) {
        return static_cast<gfx::Key>(static_cast<int>(gfx::Key::A) + static_cast<int>(sym - XK_a));
    }
    if (sym >= XK_0 && sym <= XK_9) {
        return static_cast<gfx::Key>(static_cast<int>(gfx::Key::Zero) + static_cast<int>(sym - XK_0));
    }
    switch (sym) {
        case XK_backslash: return gfx::Key::Backslash;
        case XK_BackSpace: return gfx::Key::Backspace;
        case XK_Delete: return gfx::Key::Delete;
        case XK_Down: return gfx::Key::Down;
        case XK_End: return gfx::Key::End;
        case XK_Return: return gfx::Key::Enter;
        case XK_equal: return gfx::Key::Equal;
        case XK_Escape: return gfx::Key::Escape;
        case XK_Home: return gfx::Key::Home;
        case XK_Insert: return gfx::Key::Insert;
        case XK_KP_Enter: return gfx::Key::KpEnter;
        case XK_Left: return gfx::Key::Left;
        case XK_Alt_L: return gfx::Key::LeftAlt;
        case XK_bracketleft: return gfx::Key::LeftBracket;
        case XK_Control_L: return gfx::Key::LeftControl;
        case XK_Shift_L: return gfx::Key::LeftShift;
        case XK_Super_L: return gfx::Key::LeftSuper;
        case XK_minus: return gfx::Key::Minus;
        case XK_Page_Down: return gfx::Key::PageDown;
        case XK_Page_Up: return gfx::Key::PageUp;
        case XK_Right: return gfx::Key::Right;
        case XK_Alt_R: return gfx::Key::RightAlt;
        case XK_bracketright: return gfx::Key::RightBracket;
        case XK_Control_R: return gfx::Key::RightControl;
        case XK_Shift_R: return gfx::Key::RightShift;
        case XK_Super_R: return gfx::Key::RightSuper;
        case XK_Tab: return gfx::Key::Tab;
        case XK_Up: return gfx::Key::Up;
        default: return gfx::Key::None;
    }
}

constexpr int kKeyCount = static_cast<int>(gfx::Key::Up) + 1;

}  // namespace

// -- Per-window input/platform state ---------------------------------------
//
// Deliberately at gfx:: scope, not in the anonymous namespace above/below
// -- backend_native_internal.h forward-declares gfx::NativeContext so
// backend_native_renderer2d.cpp/backend_native_renderer3d.cpp/
// backend_native_text.cpp can hold a pointer to it (for framebuffer-size
// queries and swap-buffers via NativeContextFramebufferSize/
// NativeContextSwapBuffers below); an anonymous-namespace type can't be
// named from another translation unit.
//
// One frame's worth of edge-triggered state (pressed/repeat/released) is
// cleared at the top of WindowShouldClose() -- the chosen once-per-frame
// poll point, mirroring raylib's (and this backend's own GLFW-era
// predecessor's) convention of polling OS events from inside
// WindowShouldClose -- then repopulated by ProcessEvent() as it drains
// whatever's pending on the X11 connection inside that same call.
struct NativeContext {
    Display *display = nullptr;
    int screen = 0;
    Window window = 0;
    Colormap colormap = 0;
    GLXContext glx_context = nullptr;
    XIM input_method = nullptr;
    XIC input_context = nullptr;

    Atom wm_delete_window = 0;
    Atom net_wm_state = 0;
    Atom net_wm_state_maximized_horz = 0;
    Atom net_wm_state_maximized_vert = 0;
    Atom clipboard_atom = 0;
    Atom utf8_string_atom = 0;
    Atom targets_atom = 0;
    Atom clipboard_property_atom = 0;

    NativeWindowHandle window_handle{};  // {display, window}, exposed via GetNativeWindowHandle()

    bool should_close = false;
    gfx::Key exit_key = gfx::Key::None;
    int target_fps = 0;
    std::string clipboard_text;  // cached text we own the CLIPBOARD selection with

    // Set on FocusOut, cleared at the top of the next frame's poll --
    // see IInputBackend::WindowFocusLostThisFrame for why a caller
    // needs to tell ReleaseAllKeys' synthetic releases from real ones.
    bool focus_lost = false;

    bool key_down[kKeyCount] = {};
    bool key_pressed[kKeyCount] = {};
    bool key_repeat[kKeyCount] = {};
    bool key_released[kKeyCount] = {};
    std::deque<int> key_queue;            // gfx::Key ordinals, drained by GetKeyPressed
    std::deque<unsigned int> char_queue;  // Unicode codepoints, drained by GetCharPressed

    bool mouse_down[3] = {};
    bool mouse_pressed[3] = {};
    bool mouse_released[3] = {};
    double mouse_x = 0.0, mouse_y = 0.0;
    double scroll_x = 0.0, scroll_y = 0.0;

    Cursor cursors[4] = {};  // indexed by gfx::MouseCursor, X11 XIDs (0 = not yet created)
};

void NativeContextFramebufferSize(NativeContext *ctx, int *w, int *h) {
    if (ctx->window == 0) {
        *w = *h = 0;
        return;
    }
    XWindowAttributes attrs;
    XGetWindowAttributes(ctx->display, ctx->window, &attrs);
    *w = attrs.width;
    *h = attrs.height;
}

void NativeContextSwapBuffers(NativeContext *ctx) { glXSwapBuffers(ctx->display, ctx->window); }

namespace {

// Bounded so a mode that never drains GetKeyPressed (nothing in editor.cpp
// does today, but this is defensive) can't grow this without limit --
// raylib's own internal queue is a small fixed-size ring buffer, not an
// unbounded one, for the same reason.
constexpr size_t kMaxQueuedEvents = 32;

int MouseButtonIndex(unsigned int x11_button) {
    if (x11_button == Button1) return 0;
    if (x11_button == Button3) return 1;
    if (x11_button == Button2) return 2;
    return -1;
}

// Releases every held key, reporting each as released this frame.
// Called on FocusOut: a key released while another window has focus (a WM
// shortcut, alt-tab, a screenshot tool grabbing the keyboard) never sends
// mep its KeyRelease, and a modifier left "stuck" down that way silently
// turns every later chord into its Shift/Ctrl variant (mod1+h resizing
// via S-h instead of focusing, say) until that key is pressed again.
void ReleaseAllKeys(NativeContext *ctx) {
    for (int i = 0; i < kKeyCount; i++) {
        if (ctx->key_down[i]) ctx->key_released[i] = true;
        ctx->key_down[i] = false;
    }
}

// Belt-and-braces for the same stuck-modifier problem within a focused
// window: every key event's `state` carries the X server's own view of
// which modifiers were held just before it, so a Shift/Control we still
// think is down but the server says isn't gets released here. Only the
// core Shift/Control masks are trusted -- which ModN Alt and Super land on
// is layout-dependent, so those rely on ReleaseAllKeys alone.
void SyncModifiersFromState(NativeContext *ctx, const XKeyEvent *xkey, gfx::Key event_key) {
    auto sync = [&](unsigned int mask, gfx::Key left, gfx::Key right) {
        if ((xkey->state & mask) != 0) return;
        for (gfx::Key k : {left, right}) {
            const int idx = static_cast<int>(k);
            if (k == event_key || !ctx->key_down[idx]) continue;
            ctx->key_down[idx] = false;
            ctx->key_released[idx] = true;
        }
    };
    sync(ShiftMask, gfx::Key::LeftShift, gfx::Key::RightShift);
    sync(ControlMask, gfx::Key::LeftControl, gfx::Key::RightControl);
}

void HandleKeyPress(NativeContext *ctx, XKeyEvent *xkey) {
    KeySym sym = XLookupKeysym(xkey, 0);
    gfx::Key k = UnmapKey(sym);
    SyncModifiersFromState(ctx, xkey, k);
    if (k != gfx::Key::None) {
        int idx = static_cast<int>(k);
        if (!ctx->key_down[idx]) {
            ctx->key_down[idx] = true;
            ctx->key_pressed[idx] = true;
            if (ctx->key_queue.size() < kMaxQueuedEvents) ctx->key_queue.push_back(idx);
            if (ctx->exit_key != gfx::Key::None && k == ctx->exit_key) ctx->should_close = true;
        } else {
            // X11 autorepeat: XkbSetDetectableAutoRepeat (enabled at Init)
            // suppresses the synthetic KeyRelease normally interleaved
            // between repeated KeyPress events, so consecutive KeyPress
            // events with no KeyRelease between them *are* the repeat
            // signal -- no release-then-press-at-same-timestamp heuristic
            // needed, unlike a non-detectable-autorepeat X server.
            ctx->key_repeat[idx] = true;
        }
    }

    // Text input, independent of the physical-key identity mapping above:
    // Xutf8LookupString applies the active keyboard layout/Shift/dead-key
    // composition and hands back a UTF-8 string (falls back to plain
    // XLookupString, Latin-1 only, if no input context is available --
    // still functional, just without full Unicode composition).
    //
    // Suppressed entirely when Control, Alt, or Super is held: mep's own
    // GetCharPressed() consumers (editor.cpp) rely on the invariant that
    // no char event fires under a recognized modifier chord (documented
    // in many of those call sites as "GLFW/raylib doesn't emit a char
    // event while Ctrl is held") -- X11's XLookupString/Xutf8LookupString
    // don't share that behavior on their own (Ctrl-chords decode to
    // ASCII control codes, not silence), so it's enforced explicitly here
    // to preserve every consumer's existing assumption without touching
    // each one, and extended to Alt/Super since mep uses both as
    // shortcut modifiers (mod1 pane commands, ModKey::Super) that must
    // not also leak stray characters into a text buffer.
    constexpr unsigned int kModifierMask = ControlMask | Mod1Mask | Mod4Mask;
    if ((xkey->state & kModifierMask) == 0) {
        char buf[32];
        KeySym ignored;
        int len;
        if (ctx->input_context != nullptr) {
            Status status = 0;
            len = Xutf8LookupString(ctx->input_context, xkey, buf, static_cast<int>(sizeof(buf) - 1), &ignored,
                                     &status);
            if (status != XLookupChars && status != XLookupBoth) len = 0;
        } else {
            len = XLookupString(xkey, buf, static_cast<int>(sizeof(buf) - 1), &ignored, nullptr);
        }
        if (len > 0) {
            buf[len] = '\0';
            // Decode the UTF-8 bytes into codepoints (Xutf8LookupString
            // can return more than one, e.g. a composed sequence).
            int i = 0;
            while (i < len) {
                unsigned char c0 = static_cast<unsigned char>(buf[i]);
                unsigned int cp = 0;
                int extra = 0;
                if (c0 < 0x80) {
                    cp = c0;
                } else if ((c0 & 0xE0) == 0xC0) {
                    cp = c0 & 0x1F;
                    extra = 1;
                } else if ((c0 & 0xF0) == 0xE0) {
                    cp = c0 & 0x0F;
                    extra = 2;
                } else if ((c0 & 0xF8) == 0xF0) {
                    cp = c0 & 0x07;
                    extra = 3;
                } else {
                    i++;
                    continue;  // invalid leading byte, skip
                }
                if (i + extra >= len + 1 && i + 1 + extra > len) break;
                bool valid = true;
                for (int e = 1; e <= extra; e++) {
                    if (i + e >= len || (static_cast<unsigned char>(buf[i + e]) & 0xC0) != 0x80) {
                        valid = false;
                        break;
                    }
                    cp = (cp << 6) | (static_cast<unsigned char>(buf[i + e]) & 0x3F);
                }
                i += 1 + extra;
                if (!valid) continue;
                // Backspace/Enter/Escape/Delete's own keysyms carry a
                // legacy ASCII control-code translation here (BS/CR-or-LF/
                // ESC/DEL) alongside the proper gfx::Key already queued
                // above -- every editor.cpp consumer of GetCharPressed()
                // treats its queue as genuine typed/pasted text and reacts
                // to these named keys separately (via GetKeyPressed()/
                // IsKeyPressed(), an escape/enter/backspace/del bool from
                // that queue, etc.), so letting the control byte through
                // too double-handles a single physical keypress. Confirmed
                // concretely for Backspace in a terminal pane (HandleTerminalInput
                // forwards it once via the proper gfx::Key path, and used to
                // forward this queue's raw 0x08 there too -- a single
                // physical Backspace press deleted 3 characters before this
                // filter, from that plus the separate gfx::IsKeyPressedRepeat
                // double-count fixed alongside it there). Text-buffer editing
                // (Editor::InsertChar) happens to already discard any
                // codepoint below 32 on its own, so this filter is a no-op
                // there rather than a second fix for the same symptom -- but
                // every consumer of this queue should never have had to rely
                // on that guard to begin with, since these bytes were never
                // meant to reach it as "typed text" in the first place.
                // Tab (0x09) is deliberately left unfiltered: it is NOT
                // similarly guarded downstream (InsertChar's own guard above
                // covers it too, incidentally, but nothing else in
                // editor.cpp was checked), so removing it here without
                // auditing every GetCharPressed() consumer risks silently
                // breaking a legitimate use this filter isn't chasing.
                // Terminal mode's own Tab forwarding filters this one byte
                // itself instead (HandleTerminalInput), since that specific
                // double-forward is confirmed and in scope here.
                constexpr unsigned int kBackspace = 0x08, kLineFeed = 0x0A, kCarriageReturn = 0x0D, kEscape = 0x1B,
                                        kDelete = 0x7F;
                if (cp == kBackspace || cp == kLineFeed || cp == kCarriageReturn || cp == kEscape || cp == kDelete) {
                    continue;
                }
                if (ctx->char_queue.size() < kMaxQueuedEvents) ctx->char_queue.push_back(cp);
            }
        }
    }
}

void HandleKeyRelease(NativeContext *ctx, XKeyEvent *xkey) {
    KeySym sym = XLookupKeysym(xkey, 0);
    gfx::Key k = UnmapKey(sym);
    SyncModifiersFromState(ctx, xkey, k);
    if (k == gfx::Key::None) return;
    int idx = static_cast<int>(k);
    ctx->key_down[idx] = false;
    ctx->key_released[idx] = true;
}

void HandleSelectionRequest(NativeContext *ctx, XSelectionRequestEvent *req) {
    XSelectionEvent resp{};
    resp.type = SelectionNotify;
    resp.display = req->display;
    resp.requestor = req->requestor;
    resp.selection = req->selection;
    resp.target = req->target;
    resp.time = req->time;
    resp.property = 0;  // None

    if (req->selection == ctx->clipboard_atom && req->property != 0) {  // 0 == None
        if (req->target == ctx->targets_atom) {
            Atom targets[2] = {ctx->targets_atom, ctx->utf8_string_atom};
            XChangeProperty(ctx->display, req->requestor, req->property, XA_ATOM, 32, PropModeReplace,
                             reinterpret_cast<unsigned char *>(targets), 2);
            resp.property = req->property;
        } else if (req->target == ctx->utf8_string_atom || req->target == XA_STRING) {
            XChangeProperty(ctx->display, req->requestor, req->property, req->target, 8, PropModeReplace,
                             reinterpret_cast<const unsigned char *>(ctx->clipboard_text.data()),
                             static_cast<int>(ctx->clipboard_text.size()));
            resp.property = req->property;
        }
    }
    XSendEvent(ctx->display, req->requestor, False, 0, reinterpret_cast<XEvent *>(&resp));
}

// Handles one already-retrieved X11 event, updating `ctx` -- shared by
// WindowShouldClose()'s main poll loop and GetClipboardText()'s own
// nested wait-for-SelectionNotify loop, so events that arrive while
// blocked waiting on a paste (a real keypress, or a SelectionRequest
// asking us to serve our own copied text to another app at that same
// moment) are still handled instead of silently dropped.
void ProcessEvent(NativeContext *ctx, const XEvent &event) {
    switch (event.type) {
        case KeyPress: HandleKeyPress(ctx, const_cast<XKeyEvent *>(&event.xkey)); break;
        case KeyRelease: HandleKeyRelease(ctx, const_cast<XKeyEvent *>(&event.xkey)); break;
        case FocusOut:
            ReleaseAllKeys(ctx);
            ctx->focus_lost = true;
            break;
        case ButtonPress: {
            unsigned int b = event.xbutton.button;
            int idx = MouseButtonIndex(b);
            if (idx >= 0) {
                ctx->mouse_down[idx] = true;
                ctx->mouse_pressed[idx] = true;
            } else if (b == Button4) {
                ctx->scroll_y += 1.0;
            } else if (b == Button5) {
                ctx->scroll_y -= 1.0;
            } else if (b == 6) {
                ctx->scroll_x -= 1.0;
            } else if (b == 7) {
                ctx->scroll_x += 1.0;
            }
            break;
        }
        case ButtonRelease: {
            int idx = MouseButtonIndex(event.xbutton.button);
            if (idx >= 0) {
                ctx->mouse_down[idx] = false;
                ctx->mouse_released[idx] = true;
            }
            break;
        }
        case MotionNotify:
            ctx->mouse_x = event.xmotion.x;
            ctx->mouse_y = event.xmotion.y;
            break;
        case ClientMessage:
            if (static_cast<Atom>(event.xclient.data.l[0]) == ctx->wm_delete_window) ctx->should_close = true;
            break;
        case SelectionRequest:
            HandleSelectionRequest(ctx, const_cast<XSelectionRequestEvent *>(&event.xselectionrequest));
            break;
        case SelectionClear:
            // Lost CLIPBOARD ownership -- nothing to clean up; clipboard_text
            // stays cached for reference but is no longer authoritative.
            break;
        default: break;
    }
}

}  // namespace

// -- Platform ---------------------------------------------------------------

class NativePlatformBackend : public IPlatformBackend {
public:
    explicit NativePlatformBackend(NativeContext *ctx) : ctx_(ctx) {}

    void InitWindow(int width, int height, const char *title) override {
        ctx_->display = XOpenDisplay(nullptr);
        if (ctx_->display == nullptr) {
            std::fprintf(stderr, "gfx native: XOpenDisplay failed\n");
            std::abort();
        }
        ctx_->screen = DefaultScreen(ctx_->display);

        int visual_attribs[] = {GLX_X_RENDERABLE,
                                 True,
                                 GLX_DRAWABLE_TYPE,
                                 GLX_WINDOW_BIT,
                                 GLX_RENDER_TYPE,
                                 GLX_RGBA_BIT,
                                 GLX_X_VISUAL_TYPE,
                                 GLX_TRUE_COLOR,
                                 GLX_RED_SIZE,
                                 8,
                                 GLX_GREEN_SIZE,
                                 8,
                                 GLX_BLUE_SIZE,
                                 8,
                                 GLX_ALPHA_SIZE,
                                 8,
                                 GLX_DEPTH_SIZE,
                                 24,
                                 GLX_DOUBLEBUFFER,
                                 True,
                                 0};  // None (attrib-list terminator)
        int fbcount = 0;
        GLXFBConfig *fbc = glXChooseFBConfig(ctx_->display, ctx_->screen, visual_attribs, &fbcount);
        if (fbc == nullptr || fbcount == 0) {
            std::fprintf(stderr, "gfx native: glXChooseFBConfig failed\n");
            std::abort();
        }
        GLXFBConfig best_fbc = fbc[0];
        XFree(fbc);

        XVisualInfo *vi = glXGetVisualFromFBConfig(ctx_->display, best_fbc);
        if (vi == nullptr) {
            std::fprintf(stderr, "gfx native: glXGetVisualFromFBConfig failed\n");
            std::abort();
        }

        ctx_->colormap = XCreateColormap(ctx_->display, RootWindow(ctx_->display, vi->screen), vi->visual, AllocNone);
        XSetWindowAttributes swa{};
        swa.colormap = ctx_->colormap;
        swa.event_mask = StructureNotifyMask | ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask |
                          ButtonReleaseMask | PointerMotionMask | FocusChangeMask;
        ctx_->window = XCreateWindow(ctx_->display, RootWindow(ctx_->display, vi->screen), 0, 0,
                                      static_cast<unsigned int>(width), static_cast<unsigned int>(height), 0,
                                      vi->depth, InputOutput, vi->visual, CWColormap | CWEventMask, &swa);
        XFree(vi);
        if (ctx_->window == 0) {
            std::fprintf(stderr, "gfx native: XCreateWindow failed\n");
            std::abort();
        }
        XStoreName(ctx_->display, ctx_->window, title);
        Atom net_wm_name = XInternAtom(ctx_->display, "_NET_WM_NAME", False);
        Atom utf8 = XInternAtom(ctx_->display, "UTF8_STRING", False);
        XChangeProperty(ctx_->display, ctx_->window, net_wm_name, utf8, 8, PropModeReplace,
                         reinterpret_cast<const unsigned char *>(title), static_cast<int>(std::strlen(title)));

        ctx_->wm_delete_window = XInternAtom(ctx_->display, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(ctx_->display, ctx_->window, &ctx_->wm_delete_window, 1);
        ctx_->net_wm_state = XInternAtom(ctx_->display, "_NET_WM_STATE", False);
        ctx_->net_wm_state_maximized_horz = XInternAtom(ctx_->display, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
        ctx_->net_wm_state_maximized_vert = XInternAtom(ctx_->display, "_NET_WM_STATE_MAXIMIZED_VERT", False);
        ctx_->clipboard_atom = XInternAtom(ctx_->display, "CLIPBOARD", False);
        ctx_->utf8_string_atom = utf8;
        ctx_->targets_atom = XInternAtom(ctx_->display, "TARGETS", False);
        ctx_->clipboard_property_atom = XInternAtom(ctx_->display, "MEP_SELECTION_DATA", False);

        XMapWindow(ctx_->display, ctx_->window);

        // Detectable autorepeat: without this, a physically-held key
        // generates KeyRelease immediately followed by KeyPress (same
        // keycode, same timestamp) for every repeat, and telling that
        // apart from a real release-then-press requires peeking ahead in
        // the event queue. With it enabled, autorepeat instead sends
        // consecutive KeyPress events with no KeyRelease between them --
        // HandleKeyPress's "already key_down" check above already detects
        // exactly that as the repeat signal.
        Bool supported = False;
        XkbSetDetectableAutoRepeat(ctx_->display, True, &supported);

        ctx_->window_handle.display = ctx_->display;
        ctx_->window_handle.window = ctx_->window;

        using CreateContextAttribsFn = GLXContext (*)(Display *, GLXFBConfig, GLXContext, Bool, const int *);
        auto create_context_attribs = reinterpret_cast<CreateContextAttribsFn>(
            glXGetProcAddressARB(reinterpret_cast<const unsigned char *>("glXCreateContextAttribsARB")));
        if (create_context_attribs == nullptr) {
            std::fprintf(stderr, "gfx native: no glXCreateContextAttribsARB\n");
            std::abort();
        }
        int context_attribs[] = {GLX_CONTEXT_MAJOR_VERSION_ARB,
                                  3,
                                  GLX_CONTEXT_MINOR_VERSION_ARB,
                                  3,
                                  GLX_CONTEXT_PROFILE_MASK_ARB,
                                  GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
                                  0};  // None (attrib-list terminator)
        ctx_->glx_context = create_context_attribs(ctx_->display, best_fbc, nullptr, True, context_attribs);
        XSync(ctx_->display, False);
        if (ctx_->glx_context == nullptr) {
            std::fprintf(stderr, "gfx native: glXCreateContextAttribsARB failed\n");
            std::abort();
        }
        glXMakeCurrent(ctx_->display, ctx_->window, ctx_->glx_context);

        if (!gfx::gl::LoadGLFunctions(&LoadProcAddress)) {
            std::fprintf(stderr, "gfx native: LoadGLFunctions failed\n");
            std::abort();
        }

        SetSwapInterval(1);

        ctx_->input_method = XOpenIM(ctx_->display, nullptr, nullptr, nullptr);
        if (ctx_->input_method != nullptr) {
            ctx_->input_context =
                XCreateIC(ctx_->input_method, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow,
                          ctx_->window, XNFocusWindow, ctx_->window, static_cast<void *>(nullptr));
        }
    }
    void CloseWindow() override {
        for (Cursor cursor : ctx_->cursors) {
            if (cursor != 0) XFreeCursor(ctx_->display, cursor);
        }
        if (ctx_->input_context != nullptr) XDestroyIC(ctx_->input_context);
        if (ctx_->input_method != nullptr) XCloseIM(ctx_->input_method);
        if (ctx_->glx_context != nullptr) {
            glXMakeCurrent(ctx_->display, 0, nullptr);  // 0 == None (no drawable)
            glXDestroyContext(ctx_->display, ctx_->glx_context);
        }
        if (ctx_->window != 0) XDestroyWindow(ctx_->display, ctx_->window);
        if (ctx_->colormap != 0) XFreeColormap(ctx_->display, ctx_->colormap);
        if (ctx_->display != nullptr) XCloseDisplay(ctx_->display);
    }
    bool IsWindowReady() override { return ctx_->window != 0; }
    bool WindowShouldClose() override {
        for (bool &b : ctx_->key_pressed) b = false;
        for (bool &b : ctx_->key_repeat) b = false;
        for (bool &b : ctx_->key_released) b = false;
        for (bool &b : ctx_->mouse_pressed) b = false;
        for (bool &b : ctx_->mouse_released) b = false;
        ctx_->focus_lost = false;
        ctx_->scroll_x = 0.0;
        ctx_->scroll_y = 0.0;
        while (XPending(ctx_->display) > 0) {
            XEvent event;
            XNextEvent(ctx_->display, &event);
            if (ctx_->input_context == nullptr || XFilterEvent(&event, 0) == False) {  // 0 == None (no target window filter)
                ProcessEvent(ctx_, event);
            }
        }
        return ctx_->should_close;
    }
    void SetWindowResizable() override {
        // X11 windows are resizable by default (no equivalent of GLFW's
        // "create as fixed-size" hint is ever requested -- mep always
        // wants a resizable window) -- nothing to configure.
    }
    void MaximizeWindow() override {
        // EWMH's _NET_WM_STATE change protocol: for an already-mapped
        // window, a state like "maximized" is requested via a
        // ClientMessage to the root window, not a direct property write
        // (see EWMH spec section on _NET_WM_STATE) -- the window manager
        // is what actually resizes/repositions the window in response.
        XEvent event{};
        event.type = ClientMessage;
        event.xclient.window = ctx_->window;
        event.xclient.message_type = ctx_->net_wm_state;
        event.xclient.format = 32;
        event.xclient.data.l[0] = 1;  // _NET_WM_STATE_ADD
        event.xclient.data.l[1] = static_cast<long>(ctx_->net_wm_state_maximized_horz);
        event.xclient.data.l[2] = static_cast<long>(ctx_->net_wm_state_maximized_vert);
        event.xclient.data.l[3] = 1;  // source indication: normal application
        XSendEvent(ctx_->display, RootWindow(ctx_->display, ctx_->screen), False,
                   SubstructureRedirectMask | SubstructureNotifyMask, &event);
        XFlush(ctx_->display);
    }
    void SetTargetFPS(int fps) override { ctx_->target_fps = fps; }
    int GetScreenWidth() override {
        int w = 0, h = 0;
        NativeContextFramebufferSize(ctx_, &w, &h);
        return w;
    }
    int GetScreenHeight() override {
        int w = 0, h = 0;
        NativeContextFramebufferSize(ctx_, &w, &h);
        return h;
    }
    double GetTime() override {
        using clock = std::chrono::steady_clock;
        static const clock::time_point start = clock::now();
        return std::chrono::duration<double>(clock::now() - start).count();
    }
    float GetFrameTime() override {
        double now = GetTime();
        float dt = static_cast<float>(now - last_frame_time_);
        last_frame_time_ = now;
        return dt;
    }
    void SetExitKey(gfx::Key key) override { ctx_->exit_key = key; }
    std::string GetClipboardText() override {
        Display *display = ctx_->display;
        Window window = ctx_->window;
        XDeleteProperty(display, window, ctx_->clipboard_property_atom);
        XConvertSelection(display, ctx_->clipboard_atom, ctx_->utf8_string_atom, ctx_->clipboard_property_atom,
                           window, CurrentTime);
        XFlush(display);

        int fd = ConnectionNumber(display);
        using clock = std::chrono::steady_clock;
        const auto deadline = clock::now() + std::chrono::milliseconds(1000);
        while (clock::now() < deadline) {
            while (XPending(display) > 0) {
                XEvent event;
                XNextEvent(display, &event);
                if (event.type == SelectionNotify && event.xselection.requestor == window &&
                    event.xselection.selection == ctx_->clipboard_atom) {
                    if (event.xselection.property == 0) return std::string();  // 0 == None: owner refused/no owner
                    Atom actual_type;
                    int actual_format;
                    unsigned long nitems, bytes_after;
                    unsigned char *data = nullptr;
                    XGetWindowProperty(display, window, ctx_->clipboard_property_atom, 0, 1L << 24, False,
                                        AnyPropertyType, &actual_type, &actual_format, &nitems, &bytes_after, &data);
                    std::string result;
                    if (data != nullptr) {
                        result.assign(reinterpret_cast<char *>(data), nitems);
                        XFree(data);
                    }
                    XDeleteProperty(display, window, ctx_->clipboard_property_atom);
                    return result;
                }
                ProcessEvent(ctx_, event);
            }
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(fd, &fds);
            struct timeval tv {
                0, 20000
            };
            select(fd + 1, &fds, nullptr, nullptr, &tv);
        }
        return std::string();  // timed out -- no CLIPBOARD owner responded
    }
    void SetClipboardText(const std::string &text) override {
        ctx_->clipboard_text = text;
        XSetSelectionOwner(ctx_->display, ctx_->clipboard_atom, ctx_->window, CurrentTime);
    }
    void SetMouseCursor(gfx::MouseCursor cursor) override {
        int index = static_cast<int>(cursor);
        if (ctx_->cursors[index] == 0) {
            unsigned int shape = XC_left_ptr;
            switch (cursor) {
                case gfx::MouseCursor::Default: shape = XC_left_ptr; break;
                case gfx::MouseCursor::PointingHand: shape = XC_hand2; break;
                case gfx::MouseCursor::ResizeEw: shape = XC_sb_h_double_arrow; break;
                case gfx::MouseCursor::ResizeNs: shape = XC_sb_v_double_arrow; break;
            }
            ctx_->cursors[index] = XCreateFontCursor(ctx_->display, shape);
        }
        XDefineCursor(ctx_->display, ctx_->window, ctx_->cursors[index]);
    }
    void *GetNativeWindowHandle() override { return &ctx_->window_handle; }
    void SetTraceLogCallback(TraceLogCallback /*callback*/) override {
        // No built-in internal-diagnostic log stream to redirect here
        // (same as the GLFW-era backend this replaced) -- main.cpp's
        // SetUpTraceLogFile still opens its log file; it just stays
        // empty under this backend, which is accurate.
    }

private:
    static void *LoadProcAddress(const char *name) {
        return reinterpret_cast<void *>(glXGetProcAddressARB(reinterpret_cast<const unsigned char *>(name)));
    }
    void SetSwapInterval(int interval) {
        using SwapIntervalExtFn = void (*)(Display *, GLXDrawable, int);
        auto swap_interval_ext = reinterpret_cast<SwapIntervalExtFn>(
            glXGetProcAddressARB(reinterpret_cast<const unsigned char *>("glXSwapIntervalEXT")));
        if (swap_interval_ext != nullptr) {
            swap_interval_ext(ctx_->display, ctx_->window, interval);
            return;
        }
        using SwapIntervalMesaFn = int (*)(int);
        auto swap_interval_mesa = reinterpret_cast<SwapIntervalMesaFn>(
            glXGetProcAddressARB(reinterpret_cast<const unsigned char *>("glXSwapIntervalMESA")));
        if (swap_interval_mesa != nullptr) {
            swap_interval_mesa(interval);
            return;
        }
        using SwapIntervalSgiFn = int (*)(int);
        auto swap_interval_sgi = reinterpret_cast<SwapIntervalSgiFn>(
            glXGetProcAddressARB(reinterpret_cast<const unsigned char *>("glXSwapIntervalSGI")));
        if (swap_interval_sgi != nullptr) swap_interval_sgi(interval);
        // Else: no swap-control extension available -- runs unthrottled
        // (matches what any of these would do if the driver lacked vsync
        // support entirely; not worth aborting over).
    }

    NativeContext *ctx_;
    double last_frame_time_ = 0.0;
};

// -- Input --------------------------------------------------------------

class NativeInputBackend : public IInputBackend {
public:
    explicit NativeInputBackend(NativeContext *ctx) : ctx_(ctx) {}

    bool IsKeyPressed(gfx::Key key) override { return InRange(key) && ctx_->key_pressed[static_cast<int>(key)]; }
    bool IsKeyPressedRepeat(gfx::Key key) override {
        return InRange(key) && (ctx_->key_pressed[static_cast<int>(key)] || ctx_->key_repeat[static_cast<int>(key)]);
    }
    bool IsKeyDown(gfx::Key key) override { return InRange(key) && ctx_->key_down[static_cast<int>(key)]; }
    bool IsKeyReleased(gfx::Key key) override { return InRange(key) && ctx_->key_released[static_cast<int>(key)]; }
    bool WindowFocusLostThisFrame() override { return ctx_->focus_lost; }
    gfx::Key GetKeyPressed() override {
        if (ctx_->key_queue.empty()) return gfx::Key::None;
        int idx = ctx_->key_queue.front();
        ctx_->key_queue.pop_front();
        return static_cast<gfx::Key>(idx);
    }
    int GetCharPressed() override {
        if (ctx_->char_queue.empty()) return 0;
        unsigned int cp = ctx_->char_queue.front();
        ctx_->char_queue.pop_front();
        return static_cast<int>(cp);
    }
    bool IsMouseButtonPressed(gfx::MouseButton b) override { return ctx_->mouse_pressed[MouseIndex(b)]; }
    bool IsMouseButtonDown(gfx::MouseButton b) override { return ctx_->mouse_down[MouseIndex(b)]; }
    bool IsMouseButtonReleased(gfx::MouseButton b) override { return ctx_->mouse_released[MouseIndex(b)]; }
    gfx::Vector2 GetMousePosition() override {
        return {static_cast<float>(ctx_->mouse_x), static_cast<float>(ctx_->mouse_y)};
    }
    gfx::Vector2 GetMouseWheelMoveV() override {
        return {static_cast<float>(ctx_->scroll_x), static_cast<float>(ctx_->scroll_y)};
    }

private:
    static bool InRange(gfx::Key key) { return key > gfx::Key::None && key <= gfx::Key::Up; }
    static int MouseIndex(gfx::MouseButton b) {
        switch (b) {
            case gfx::MouseButton::Left: return 0;
            case gfx::MouseButton::Right: return 1;
            case gfx::MouseButton::Middle: return 2;
        }
        return 0;
    }
    NativeContext *ctx_;
};

struct NativeBackendSet {
    NativeContext context;
    NativePlatformBackend platform{&context};
    NativeInputBackend input{&context};
    NativeAudioBackend audio;
    // Declaration order matters: members initialize in this order, and
    // NativeTextBackend's constructor needs &renderer2d already valid
    // (glyphs draw through the 2D renderer's own DrawTexturePro -- see
    // backend_native_renderer2d.cpp's top comment for why).
    NativeRenderer2DBackend renderer2d{&context};
    NativeTextBackend text{&renderer2d};
    NativeRenderer3DBackend renderer3d{&context};
};

NativeBackendSet *CreateNativeBackendSet() { return new NativeBackendSet(); }

Backends ToBackends(NativeBackendSet *set) {
    Backends b;
    b.platform = &set->platform;
    b.input = &set->input;
    b.audio = &set->audio;
    b.text = &set->text;
    b.renderer2d = &set->renderer2d;
    b.renderer3d = &set->renderer3d;
    return b;
}

}  // namespace gfx
