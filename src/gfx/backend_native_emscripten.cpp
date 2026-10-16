// Emscripten/wasm-only implementation of gfx::NativeContext/Platform/Input,
// written directly against the browser through emscripten/html5.h (DOM
// keyboard/mouse/wheel/touch/focus/resize events) and
// emscripten_webgl_create_context (a WebGL 2 context on the page's
// #canvas). Native desktop builds use backend_native.cpp (X11/GLX),
// backend_native_macos.mm (Cocoa) or backend_native_win32.cpp instead;
// CMakeLists.txt selects this file only `if(EMSCRIPTEN)`.
//
// This replaced backend_native_emscripten_glfw.cpp, which reached the
// same browser APIs through Emscripten's GLFW emulation (`-sUSE_GLFW=3`,
// DEPENDENCY_REMOVAL_PLAN.md Phase 8). Behaviour follows what that shim
// did, with these deliberate differences:
//
//  * The canvas' drawing buffer follows its CSS size (one buffer pixel per
//    CSS pixel -- devicePixelRatio is ignored, exactly as the GLFW shim
//    did without GLFW_SCALE_TO_MONITOR). The shim instead kept the buffer
//    at InitWindow's 1000x650 and let CSS stretch it over the page.
//  * Per-frame edge state (pressed/repeat/released, wheel, focus lost) is
//    cleared after every main-loop iteration. The GLFW file cleared it in
//    WindowShouldClose(), which the wasm main loop never calls, so a key
//    once pressed stayed "pressed" for good.
//  * Right-hand modifiers report as Right*, not Left* (the DOM tells them
//    apart by location); window blur releases held keys and reports
//    WindowFocusLostThisFrame(); only keys gfx::Key knows are queued for
//    GetKeyPressed (the shim queued unknown ones too, which came back as
//    Key::None and ended a caller's drain loop early).
//  * The mouse cursor shape and the clipboard work (the shim's were
//    no-ops); the clipboard shares editor.cpp's window.__mepClipboardText
//    cache, since a browser can't read the clipboard synchronously.

#include "gfx/backend_native.h"

#include <emscripten/em_js.h>
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/html5_webgl.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <string>

#include "gfx/backend_native_internal.h"
#include "gfx/gl_loader.h"

namespace gfx {

namespace {

constexpr const char *kCanvas = "#canvas";
constexpr int kKeyCount = static_cast<int>(gfx::Key::Up) + 1;
// Bounded so a mode that never drains GetKeyPressed/GetCharPressed can't
// grow these without limit (same bound as the native backends).
constexpr size_t kMaxQueuedEvents = 32;
constexpr unsigned int kDomKeyLocationRight = 2;  // KeyboardEvent.location

// DOM KeyboardEvent.keyCode -> gfx::Key. Covers every key the GLFW
// backend's MapKey/UnmapKey pair did, using the keyCodes Emscripten's
// GLFW shim (libglfw.js DOMToGLFWKeyCode) recognised for them, including
// Firefox's own codes for '-' (173) and '=' (61) and its Meta (224).
// Enter on the numpad stays Enter (location is ignored there), as with
// the shim, since most of mep checks Key::Enter alone.
gfx::Key KeyFromDom(unsigned int key_code, unsigned int location) {
    if (key_code >= 0x41 && key_code <= 0x5A) {
        return static_cast<gfx::Key>(static_cast<int>(gfx::Key::A) + static_cast<int>(key_code - 0x41));
    }
    if (key_code >= 0x30 && key_code <= 0x39) {
        return static_cast<gfx::Key>(static_cast<int>(gfx::Key::Zero) + static_cast<int>(key_code - 0x30));
    }
    const bool right = location == kDomKeyLocationRight;
    switch (key_code) {
        case 0x08: return gfx::Key::Backspace;
        case 0x09: return gfx::Key::Tab;
        case 0x0D: return gfx::Key::Enter;
        case 0x10: return right ? gfx::Key::RightShift : gfx::Key::LeftShift;
        case 0x11: return right ? gfx::Key::RightControl : gfx::Key::LeftControl;
        case 0x12: return right ? gfx::Key::RightAlt : gfx::Key::LeftAlt;
        case 0x1B: return gfx::Key::Escape;
        case 0x20: return gfx::Key::Space;
        case 0x21: return gfx::Key::PageUp;
        case 0x22: return gfx::Key::PageDown;
        case 0x23: return gfx::Key::End;
        case 0x24: return gfx::Key::Home;
        case 0x25: return gfx::Key::Left;
        case 0x26: return gfx::Key::Up;
        case 0x27: return gfx::Key::Right;
        case 0x28: return gfx::Key::Down;
        case 0x2D: return gfx::Key::Insert;
        case 0x2E: return gfx::Key::Delete;
        case 0x3D: return gfx::Key::Equal;  // Firefox
        case 0xBB: return gfx::Key::Equal;
        case 0xAD: return gfx::Key::Minus;  // Firefox
        case 0xBD: return gfx::Key::Minus;
        case 0xDB: return gfx::Key::LeftBracket;
        case 0xDC: return gfx::Key::Backslash;
        case 0xDD: return gfx::Key::RightBracket;
        case 0x5B: return right ? gfx::Key::RightSuper : gfx::Key::LeftSuper;  // Win / left Cmd (Chrome)
        case 0x5C: return gfx::Key::RightSuper;                                   // right Win
        case 0x5D: return right ? gfx::Key::RightSuper : gfx::Key::None;      // right Cmd (Chrome); else the Menu key
        case 0xE0: return right ? gfx::Key::RightSuper : gfx::Key::LeftSuper;  // Meta (Firefox)
        default: return gfx::Key::None;
    }
}

bool IsModifierKey(gfx::Key k) {
    switch (k) {
        case gfx::Key::LeftShift:
        case gfx::Key::RightShift:
        case gfx::Key::LeftControl:
        case gfx::Key::RightControl:
        case gfx::Key::LeftAlt:
        case gfx::Key::RightAlt:
        case gfx::Key::LeftSuper:
        case gfx::Key::RightSuper: return true;
        default: return false;
    }
}

int MouseButtonIndex(gfx::MouseButton b) {
    switch (b) {
        case gfx::MouseButton::Left: return 0;
        case gfx::MouseButton::Right: return 1;
        case gfx::MouseButton::Middle: return 2;
    }
    return 0;
}

// DOM MouseEvent.button -> MouseButtonIndex order; -1 for back/forward
// and anything else gfx:: has no button for.
int MouseIndexFromDom(unsigned short button) {
    switch (button) {
        case 0: return 0;  // primary
        case 1: return 2;  // auxiliary (middle)
        case 2: return 1;  // secondary (right)
        default: return -1;
    }
}

}  // namespace

// -- Per-window input state ------------------------------------------------
//
// DOM events arrive between main-loop iterations (the browser runs them
// while mep is idle waiting for the next requestAnimationFrame), so the
// callbacks below write straight into this state, the frame that follows
// reads it, and EndFrame() -- registered as an Emscripten post-main-loop
// hook -- clears the one-frame edges once that frame is done. That is the
// same "events since the last frame are visible for exactly one frame"
// contract the native backends get from polling at the top of a frame.
//
// At gfx:: scope (not anonymous) for the same reason as the native
// backends': backend_native_internal.h forward-declares it for the
// renderer/text translation units.
struct NativeContext {
    bool ready = false;
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE gl = 0;
    int fb_w = 0, fb_h = 0;              // canvas drawing-buffer size
    int fallback_w = 0, fallback_h = 0;  // InitWindow's size, for a canvas with no CSS box
    double start_ms = 0.0;
    bool should_close = false;
    gfx::Key exit_key = gfx::Key::None;
    int target_fps = 0;
    std::function<void()> flush_2d;  // NativeContextFlush2D

    bool key_down[kKeyCount] = {};
    bool key_pressed[kKeyCount] = {};
    bool key_repeat[kKeyCount] = {};
    bool key_released[kKeyCount] = {};
    std::deque<int> key_queue;            // gfx::Key ordinals, drained by GetKeyPressed
    std::deque<unsigned int> char_queue;  // Unicode codepoints, drained by GetCharPressed
    bool focus_lost = false;

    bool mouse_down[3] = {};
    bool mouse_pressed[3] = {};
    bool mouse_released[3] = {};
    double mouse_x = 0.0, mouse_y = 0.0;
    double scroll_x = 0.0, scroll_y = 0.0;
    int primary_touch = -1;  // DOM Touch.identifier driving the emulated left button
    int cursor = -1;         // last gfx::MouseCursor applied, -1 = never set
};

namespace {

NativeContext *g_ctx = nullptr;  // for the post-main-loop hook (one canvas per page)

// --- JS glue ------------------------------------------------------------

EM_JS_DEPS(mep_gfx_html5, "$registerPostMainLoop,$stringToNewUTF8,$UTF8ToString");

EM_JS(void, mep_html5_install_frame_hook, (), {
    registerPostMainLoop(function() { _mep_html5_end_frame(); });
});

// Canvas position in the viewport (CSS px). The canvas normally fills the
// page from (0, 0), but a host page may place it anywhere.
EM_JS(double, mep_html5_canvas_left, (), {
    var c = document.querySelector("#canvas");
    return c ? c.getBoundingClientRect().left : 0;
});
EM_JS(double, mep_html5_canvas_top, (), {
    var c = document.querySelector("#canvas");
    return c ? c.getBoundingClientRect().top : 0;
});

EM_JS(void, mep_html5_set_cursor, (const char *name), {
    var c = document.querySelector("#canvas");
    if (c) c.style.cursor = UTF8ToString(name);
});

// Same cache editor.cpp's mep_js_clipboard_* bridge keeps (see the comment
// there): writes go to the cache and to navigator.clipboard; reads return
// the cache and kick off a readText() whose result serves the next read.
EM_JS(void, mep_html5_clipboard_write, (const char *text_ptr), {
    var text = UTF8ToString(text_ptr);
    window.__mepClipboardText = text;
    try {
        if (navigator.clipboard && navigator.clipboard.writeText) navigator.clipboard.writeText(text).catch(function() {});
    } catch (e) {}
});
EM_JS(char *, mep_html5_clipboard_read, (), {
    try {
        if (navigator.clipboard && navigator.clipboard.readText) {
            navigator.clipboard.readText().then(function(t) { if (t) window.__mepClipboardText = t; }, function() {});
        }
    } catch (e) {}
    return stringToNewUTF8(window.__mepClipboardText || "");
});

// --- Canvas sizing --------------------------------------------------------

// Sizes the drawing buffer to the canvas' CSS box (1:1, see the top
// comment), or to InitWindow's size when the canvas has no box yet.
void SyncCanvasSize(NativeContext *ctx) {
    double css_w = 0.0, css_h = 0.0;
    emscripten_get_element_css_size(kCanvas, &css_w, &css_h);
    int w = static_cast<int>(std::floor(css_w));
    int h = static_cast<int>(std::floor(css_h));
    if (w <= 0 || h <= 0) {
        w = ctx->fallback_w;
        h = ctx->fallback_h;
    }
    int cur_w = 0, cur_h = 0;
    emscripten_get_canvas_element_size(kCanvas, &cur_w, &cur_h);
    if (cur_w != w || cur_h != h) emscripten_set_canvas_element_size(kCanvas, w, h);
    ctx->fb_w = w;
    ctx->fb_h = h;
}

// Viewport (client) coordinates -> drawing-buffer pixels.
void SetMouseFromClient(NativeContext *ctx, double client_x, double client_y) {
    double css_w = 0.0, css_h = 0.0;
    emscripten_get_element_css_size(kCanvas, &css_w, &css_h);
    const double sx = css_w > 0.0 ? static_cast<double>(ctx->fb_w) / css_w : 1.0;
    const double sy = css_h > 0.0 ? static_cast<double>(ctx->fb_h) / css_h : 1.0;
    ctx->mouse_x = (client_x - mep_html5_canvas_left()) * sx;
    ctx->mouse_y = (client_y - mep_html5_canvas_top()) * sy;
}

// --- Keyboard -------------------------------------------------------------

void ReleaseAllKeys(NativeContext *ctx) {
    for (int i = 0; i < kKeyCount; i++) {
        if (ctx->key_down[i]) ctx->key_released[i] = true;
        ctx->key_down[i] = false;
    }
}

bool OnKeyDown(int /*type*/, const EmscriptenKeyboardEvent *e, void *user) {
    auto *ctx = static_cast<NativeContext *>(user);
    const gfx::Key k = KeyFromDom(e->keyCode, e->location);
    if (k != gfx::Key::None) {
        const int idx = static_cast<int>(k);
        if (ctx->key_down[idx]) {
            ctx->key_repeat[idx] = true;  // auto-repeat: still held from an earlier keydown
        } else {
            ctx->key_down[idx] = true;
            ctx->key_pressed[idx] = true;
            if (ctx->key_queue.size() < kMaxQueuedEvents) ctx->key_queue.push_back(idx);
            if (ctx->exit_key != gfx::Key::None && k == ctx->exit_key) ctx->should_close = true;
        }
    }
    // Returning true calls preventDefault(), which also suppresses the
    // keypress that would carry this key's character -- so only for keys
    // whose browser default would get in the way and that type nothing:
    // Backspace (history back in some browsers) and Tab (focus moves off
    // the canvas) as the GLFW shim did, the scrolling keys, and
    // Ctrl/Cmd/Alt chords, which mep binds itself (web/shell.html's
    // capture listener does the same for hosts that load this page).
    switch (k) {
        case gfx::Key::Backspace:
        case gfx::Key::Tab:
        case gfx::Key::Up:
        case gfx::Key::Down:
        case gfx::Key::Left:
        case gfx::Key::Right:
        case gfx::Key::PageUp:
        case gfx::Key::PageDown:
        case gfx::Key::Home:
        case gfx::Key::End: return true;
        default: break;
    }
    if ((e->ctrlKey || e->metaKey || e->altKey) && !IsModifierKey(k) && std::strcmp(e->key, "Shift") != 0 &&
        std::strcmp(e->key, "Control") != 0 && std::strcmp(e->key, "Meta") != 0 && std::strcmp(e->key, "Alt") != 0) {
        return true;
    }
    return false;
}

bool OnKeyUp(int /*type*/, const EmscriptenKeyboardEvent *e, void *user) {
    auto *ctx = static_cast<NativeContext *>(user);
    const gfx::Key k = KeyFromDom(e->keyCode, e->location);
    if (k == gfx::Key::None) return false;
    const int idx = static_cast<int>(k);
    ctx->key_down[idx] = false;
    ctx->key_released[idx] = true;
    return false;
}

// Text input. keypress is the one DOM event carrying the typed character
// as a code point (what the GLFW shim used too), with the same filters:
// nothing under Ctrl/Cmd (those are commands) and no control characters
// (Enter/Backspace/Escape/Tab arrive through the key state instead).
// Consumed characters get preventDefault so the browser doesn't also act
// on them -- Firefox's '/' and '\'' quick-find bar, Space scrolling.
bool OnKeyPress(int /*type*/, const EmscriptenKeyboardEvent *e, void *user) {
    auto *ctx = static_cast<NativeContext *>(user);
    if (e->ctrlKey || e->metaKey) return false;
    const unsigned int cp = e->charCode;
    if (cp < 0x20) return false;
    if (ctx->char_queue.size() < kMaxQueuedEvents) ctx->char_queue.push_back(cp);
    return true;
}

bool OnBlur(int /*type*/, const EmscriptenFocusEvent * /*e*/, void *user) {
    auto *ctx = static_cast<NativeContext *>(user);
    // The window lost the keyboard: no keyup will arrive for anything held
    // (Cmd/Alt-Tab is the usual case), so release it all now, as the GLFW
    // shim's onBlur did, and say so for WindowFocusLostThisFrame.
    ReleaseAllKeys(ctx);
    ctx->focus_lost = true;
    return false;
}

// --- Mouse / wheel / touch --------------------------------------------------

bool OnMouseDown(int /*type*/, const EmscriptenMouseEvent *e, void *user) {
    auto *ctx = static_cast<NativeContext *>(user);
    SetMouseFromClient(ctx, e->clientX, e->clientY);
    const int idx = MouseIndexFromDom(e->button);
    if (idx < 0) return false;
    ctx->mouse_down[idx] = true;
    ctx->mouse_pressed[idx] = true;
    // Middle-button default is autoscroll (Windows/Linux browsers), which
    // would fight mep's own middle-drag panning.
    return idx == 2;
}

// Registered on the window rather than the canvas, so a drag that leaves
// the canvas still tracks and still sees its release.
bool OnMouseUp(int /*type*/, const EmscriptenMouseEvent *e, void *user) {
    auto *ctx = static_cast<NativeContext *>(user);
    SetMouseFromClient(ctx, e->clientX, e->clientY);
    const int idx = MouseIndexFromDom(e->button);
    if (idx < 0 || !ctx->mouse_down[idx]) return false;
    ctx->mouse_down[idx] = false;
    ctx->mouse_released[idx] = true;
    return false;
}

bool OnMouseMove(int /*type*/, const EmscriptenMouseEvent *e, void *user) {
    SetMouseFromClient(static_cast<NativeContext *>(user), e->clientX, e->clientY);
    return false;
}

// Wheel units match the GLFW shim's: a DOM_DELTA_PIXEL event is 100 px per
// step, a DOM_DELTA_LINE one 3 lines per step, a page 80 steps; any
// non-zero movement counts as at least one step; y is negated so wheel-down
// is negative (GLFW's convention, which editor.cpp's wheel code assumes).
// x keeps the DOM sign (positive = right, as the X11 backend reports it)
// but now in the same steps -- the shim passed raw pixels for x.
double WheelSteps(double delta, unsigned int mode) {
    switch (mode) {
        case DOM_DELTA_PIXEL: delta /= 100.0; break;
        case DOM_DELTA_LINE: delta /= 3.0; break;
        case DOM_DELTA_PAGE: delta *= 80.0; break;
        default: break;
    }
    if (delta == 0.0) return 0.0;
    return delta > 0.0 ? std::fmax(delta, 1.0) : std::fmin(delta, -1.0);
}

bool OnWheel(int /*type*/, const EmscriptenWheelEvent *e, void *user) {
    auto *ctx = static_cast<NativeContext *>(user);
    ctx->scroll_y += -WheelSteps(e->deltaY, e->deltaMode);
    ctx->scroll_x += WheelSteps(e->deltaX, e->deltaMode);
    return true;  // the page itself must not scroll
}

// One touch at a time drives the left button, as in the GLFW shim; the
// browser's own touch gestures (and the mouse events it would synthesise
// from them) are suppressed.
bool OnTouch(int type, const EmscriptenTouchEvent *e, void *user) {
    auto *ctx = static_cast<NativeContext *>(user);
    for (int i = 0; i < e->numTouches; i++) {
        const EmscriptenTouchPoint &t = e->touches[i];
        if (!t.isChanged) continue;
        if (type == EMSCRIPTEN_EVENT_TOUCHSTART) {
            if (ctx->primary_touch >= 0 || !t.onTarget) continue;
            ctx->primary_touch = t.identifier;
            SetMouseFromClient(ctx, t.clientX, t.clientY);
            ctx->mouse_down[0] = true;
            ctx->mouse_pressed[0] = true;
        } else if (t.identifier == ctx->primary_touch) {
            SetMouseFromClient(ctx, t.clientX, t.clientY);
            if (type == EMSCRIPTEN_EVENT_TOUCHEND || type == EMSCRIPTEN_EVENT_TOUCHCANCEL) {
                ctx->primary_touch = -1;
                ctx->mouse_down[0] = false;
                ctx->mouse_released[0] = true;
            }
        }
    }
    return true;
}

bool OnResize(int /*type*/, const EmscriptenUiEvent * /*e*/, void *user) {
    SyncCanvasSize(static_cast<NativeContext *>(user));
    return false;
}

}  // namespace

// Runs after every main-loop iteration (see NativeContext's comment).
extern "C" EMSCRIPTEN_KEEPALIVE void mep_html5_end_frame() {
    NativeContext *ctx = g_ctx;
    if (ctx == nullptr || !ctx->ready) return;
    for (bool &b : ctx->key_pressed) b = false;
    for (bool &b : ctx->key_repeat) b = false;
    for (bool &b : ctx->key_released) b = false;
    for (bool &b : ctx->mouse_pressed) b = false;
    for (bool &b : ctx->mouse_released) b = false;
    ctx->scroll_x = 0.0;
    ctx->scroll_y = 0.0;
    ctx->focus_lost = false;
    // Catches canvas CSS size changes that come without a window resize
    // (a host page re-laying itself out).
    SyncCanvasSize(ctx);
}

void NativeContextFramebufferSize(NativeContext *ctx, int *w, int *h) {
    *w = ctx->fb_w;
    *h = ctx->fb_h;
}

// The browser presents the WebGL drawing buffer by itself when the frame's
// callback returns; there is nothing to swap.
void NativeContextSwapBuffers(NativeContext * /*ctx*/) {}

void NativeContextFlush2D(NativeContext *ctx) {
    if (ctx && ctx->flush_2d) ctx->flush_2d();
}

void NativeContextSetFlush2D(NativeContext *ctx, std::function<void()> fn) { ctx->flush_2d = std::move(fn); }

// -- Platform ---------------------------------------------------------------

class NativePlatformBackend : public IPlatformBackend {
public:
    explicit NativePlatformBackend(NativeContext *ctx) : ctx_(ctx) { ctx_->start_ms = emscripten_get_now(); }

    void InitWindow(int width, int height, const char * /*title*/) override {
        ctx_->fallback_w = width;
        ctx_->fallback_h = height;
        SyncCanvasSize(ctx_);

        // WebGL 2 (GLES 3.0) -- what the GLFW backend's "3.3 core" hint got
        // with -sMAX_WEBGL_VERSION=2, and what gl_shader.cpp's
        // "#version 300 es" shaders need. Attributes match the GLFW shim's
        // defaults: 8-bit alpha, 24-bit depth, 8-bit stencil, no MSAA.
        EmscriptenWebGLContextAttributes attrs;
        emscripten_webgl_init_context_attributes(&attrs);
        attrs.majorVersion = 2;
        attrs.minorVersion = 0;
        attrs.alpha = true;
        attrs.depth = true;
        attrs.stencil = true;
        attrs.antialias = false;
        attrs.premultipliedAlpha = true;
        attrs.preserveDrawingBuffer = false;
        attrs.enableExtensionsByDefault = true;
        ctx_->gl = emscripten_webgl_create_context(kCanvas, &attrs);
        if (ctx_->gl <= 0) {
            std::fprintf(stderr, "gfx native: emscripten_webgl_create_context (WebGL 2) failed: %d\n",
                         static_cast<int>(ctx_->gl));
            std::abort();
        }
        emscripten_webgl_make_context_current(ctx_->gl);
        if (!gfx::gl::LoadGLFunctions(&emscripten_webgl_get_proc_address)) {
            std::fprintf(stderr, "gfx native: LoadGLFunctions failed\n");
            std::abort();
        }

        // Keyboard on the window in the capture phase, like the GLFW shim:
        // the canvas never holds DOM focus in a way that matters.
        const char *win = EMSCRIPTEN_EVENT_TARGET_WINDOW;
        emscripten_set_keydown_callback(win, ctx_, true, OnKeyDown);
        emscripten_set_keyup_callback(win, ctx_, true, OnKeyUp);
        emscripten_set_keypress_callback(win, ctx_, true, OnKeyPress);
        emscripten_set_blur_callback(win, ctx_, true, OnBlur);
        emscripten_set_mousedown_callback(kCanvas, ctx_, true, OnMouseDown);
        emscripten_set_mouseup_callback(win, ctx_, true, OnMouseUp);
        emscripten_set_mousemove_callback(win, ctx_, true, OnMouseMove);
        emscripten_set_wheel_callback(kCanvas, ctx_, true, OnWheel);
        emscripten_set_touchstart_callback(kCanvas, ctx_, true, OnTouch);
        emscripten_set_touchmove_callback(kCanvas, ctx_, true, OnTouch);
        emscripten_set_touchend_callback(kCanvas, ctx_, true, OnTouch);
        emscripten_set_touchcancel_callback(kCanvas, ctx_, true, OnTouch);
        emscripten_set_resize_callback(win, ctx_, false, OnResize);

        g_ctx = ctx_;
        mep_html5_install_frame_hook();
        ctx_->ready = true;
    }
    void CloseWindow() override {
        emscripten_html5_remove_all_event_listeners();
        if (ctx_->gl > 0) emscripten_webgl_destroy_context(ctx_->gl);
        ctx_->gl = 0;
        ctx_->ready = false;
        g_ctx = nullptr;
    }
    bool IsWindowReady() override { return ctx_->ready; }
    // Not a poll point here (see NativeContext): events land between
    // frames on their own and mep_html5_end_frame clears the edges.
    bool WindowShouldClose() override { return ctx_->should_close; }
    // A canvas has no window-manager state: always "resizable" (it follows
    // its CSS box), never maximized.
    void SetWindowResizable() override {}
    void MaximizeWindow() override {}
    bool IsWindowMaximized() override { return false; }
    // Stored only: the browser paces the main loop with
    // requestAnimationFrame (main.cpp's emscripten_set_main_loop(..., 0, 0)).
    void SetTargetFPS(int fps) override { ctx_->target_fps = fps; }
    int GetScreenWidth() override { return ctx_->fb_w; }
    int GetScreenHeight() override { return ctx_->fb_h; }
    double GetTime() override { return (emscripten_get_now() - ctx_->start_ms) / 1000.0; }
    float GetFrameTime() override {
        double now = GetTime();
        float dt = static_cast<float>(now - last_frame_time_);
        last_frame_time_ = now;
        return dt;
    }
    void SetExitKey(gfx::Key key) override { ctx_->exit_key = key; }
    std::string GetClipboardText() override {
        char *raw = mep_html5_clipboard_read();
        std::string text = raw != nullptr ? std::string(raw) : std::string();
        std::free(raw);
        return text;
    }
    void SetClipboardText(const std::string &text) override { mep_html5_clipboard_write(text.c_str()); }
    void SetMouseCursor(gfx::MouseCursor cursor) override {
        const int index = static_cast<int>(cursor);
        if (index == ctx_->cursor) return;
        ctx_->cursor = index;
        const char *css = "default";
        switch (cursor) {
            case gfx::MouseCursor::Default: css = "default"; break;
            case gfx::MouseCursor::PointingHand: css = "pointer"; break;
            case gfx::MouseCursor::ResizeEw: css = "ew-resize"; break;
            case gfx::MouseCursor::ResizeNs: css = "ns-resize"; break;
        }
        mep_html5_set_cursor(css);
    }
    // Nothing under Emscripten consumes this (agent_ui_input.cpp and GUI
    // embedding are native-only); non-null once the canvas has a context.
    void *GetNativeWindowHandle() override { return ctx_->ready ? ctx_ : nullptr; }
    void SetTraceLogCallback(TraceLogCallback /*callback*/) override {
        // No internal diagnostic stream to redirect (see the native backends).
    }

private:
    NativeContext *ctx_;
    double last_frame_time_ = 0.0;
};

// -- Input --------------------------------------------------------------

class NativeInputBackend : public IInputBackend {
public:
    explicit NativeInputBackend(NativeContext *ctx) : ctx_(ctx) {}

    bool IsKeyPressed(gfx::Key key) override { return InRange(key) && ctx_->key_pressed[Idx(key)]; }
    bool IsKeyPressedRepeat(gfx::Key key) override {
        return InRange(key) && (ctx_->key_pressed[Idx(key)] || ctx_->key_repeat[Idx(key)]);
    }
    bool IsKeyDown(gfx::Key key) override { return InRange(key) && ctx_->key_down[Idx(key)]; }
    bool IsKeyReleased(gfx::Key key) override { return InRange(key) && ctx_->key_released[Idx(key)]; }
    bool WindowFocusLostThisFrame() override { return ctx_->focus_lost; }
    gfx::Key GetKeyPressed() override {
        if (ctx_->key_queue.empty()) return gfx::Key::None;
        const int idx = ctx_->key_queue.front();
        ctx_->key_queue.pop_front();
        return static_cast<gfx::Key>(idx);
    }
    int GetCharPressed() override {
        if (ctx_->char_queue.empty()) return 0;
        const unsigned int cp = ctx_->char_queue.front();
        ctx_->char_queue.pop_front();
        return static_cast<int>(cp);
    }
    bool IsMouseButtonPressed(gfx::MouseButton b) override { return ctx_->mouse_pressed[MouseButtonIndex(b)]; }
    bool IsMouseButtonDown(gfx::MouseButton b) override { return ctx_->mouse_down[MouseButtonIndex(b)]; }
    bool IsMouseButtonReleased(gfx::MouseButton b) override { return ctx_->mouse_released[MouseButtonIndex(b)]; }
    gfx::Vector2 GetMousePosition() override {
        return {static_cast<float>(ctx_->mouse_x), static_cast<float>(ctx_->mouse_y)};
    }
    gfx::Vector2 GetMouseWheelMoveV() override {
        return {static_cast<float>(ctx_->scroll_x), static_cast<float>(ctx_->scroll_y)};
    }

private:
    static int Idx(gfx::Key key) { return static_cast<int>(key); }
    static bool InRange(gfx::Key key) { return key != gfx::Key::None && Idx(key) > 0 && Idx(key) < kKeyCount; }
    NativeContext *ctx_;
};

struct NativeBackendSet {
    NativeContext context;
    NativePlatformBackend platform{&context};
    NativeInputBackend input{&context};
    NativeAudioBackend audio;
    // Declaration order matters: NativeTextBackend's constructor needs
    // &renderer2d already valid (see backend_native_renderer2d.cpp).
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
