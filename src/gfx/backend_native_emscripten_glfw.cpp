// Emscripten/wasm-only implementation of gfx::NativeContext/Platform/Input
// -- see GLFW_REMOVAL_PLAN.md. Native desktop builds use
// gfx/backend_native.cpp's own hand-written X11/GLX implementation
// instead; this file is the platform-specific counterpart that CMakeLists.txt
// selects only `if(EMSCRIPTEN)`.
//
// Still built on GLFW deliberately: under Emscripten, `-sUSE_GLFW=3` is
// emcc's own link-time shim -- a GLFW-API-compatible reimplementation
// backed by the browser's own canvas/keyboard/mouse/pointer-lock APIs,
// not the real X11 GLFW backend the native build used to link. There's
// no "in-house version" of a browser's own input/canvas event model
// worth writing (GLFW_REMOVAL_PLAN.md's own Phase 5 non-goal); calling
// this GLFW-shaped API is already the thin, correct way to reach the
// browser here, so this file keeps doing exactly what
// gfx/backend_native.cpp did before that plan's X11 rewrite.

#include "gfx/backend_native.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "gfx/backend_native_internal.h"
#include "gfx/gl_loader.h"

namespace gfx {

namespace {

// -- Key/button/cursor mapping ---------------------------------------------
//
// gfx::Key's A..Z and Zero..Nine runs are each contiguous in alphabetical/
// numeric order (see gfx/types.h's own comment on why), and so are GLFW's
// GLFW_KEY_A..GLFW_KEY_Z / GLFW_KEY_0..GLFW_KEY_9 -- both ranges map by
// simple offset arithmetic instead of a 36-case switch.

int MapKey(gfx::Key key) {
    if (key >= gfx::Key::A && key <= gfx::Key::Z) return GLFW_KEY_A + (key - gfx::Key::A);
    if (key >= gfx::Key::Zero && key <= gfx::Key::Nine) return GLFW_KEY_0 + (key - gfx::Key::Zero);
    switch (key) {
        case gfx::Key::None: return GLFW_KEY_UNKNOWN;
        case gfx::Key::Backslash: return GLFW_KEY_BACKSLASH;
        case gfx::Key::Backspace: return GLFW_KEY_BACKSPACE;
        case gfx::Key::Delete: return GLFW_KEY_DELETE;
        case gfx::Key::Down: return GLFW_KEY_DOWN;
        case gfx::Key::End: return GLFW_KEY_END;
        case gfx::Key::Enter: return GLFW_KEY_ENTER;
        case gfx::Key::Equal: return GLFW_KEY_EQUAL;
        case gfx::Key::Escape: return GLFW_KEY_ESCAPE;
        case gfx::Key::Home: return GLFW_KEY_HOME;
        case gfx::Key::Insert: return GLFW_KEY_INSERT;
        case gfx::Key::KpEnter: return GLFW_KEY_KP_ENTER;
        case gfx::Key::Left: return GLFW_KEY_LEFT;
        case gfx::Key::LeftAlt: return GLFW_KEY_LEFT_ALT;
        case gfx::Key::LeftBracket: return GLFW_KEY_LEFT_BRACKET;
        case gfx::Key::LeftControl: return GLFW_KEY_LEFT_CONTROL;
        case gfx::Key::LeftShift: return GLFW_KEY_LEFT_SHIFT;
        case gfx::Key::LeftSuper: return GLFW_KEY_LEFT_SUPER;
        case gfx::Key::Minus: return GLFW_KEY_MINUS;
        case gfx::Key::PageDown: return GLFW_KEY_PAGE_DOWN;
        case gfx::Key::PageUp: return GLFW_KEY_PAGE_UP;
        case gfx::Key::Right: return GLFW_KEY_RIGHT;
        case gfx::Key::RightAlt: return GLFW_KEY_RIGHT_ALT;
        case gfx::Key::RightBracket: return GLFW_KEY_RIGHT_BRACKET;
        case gfx::Key::RightControl: return GLFW_KEY_RIGHT_CONTROL;
        case gfx::Key::RightShift: return GLFW_KEY_RIGHT_SHIFT;
        case gfx::Key::RightSuper: return GLFW_KEY_RIGHT_SUPER;
        case gfx::Key::Tab: return GLFW_KEY_TAB;
        case gfx::Key::Up: return GLFW_KEY_UP;
        default: return GLFW_KEY_UNKNOWN;
    }
}

gfx::Key UnmapKey(int glfw_key) {
    if (glfw_key >= GLFW_KEY_A && glfw_key <= GLFW_KEY_Z) {
        return static_cast<gfx::Key>(static_cast<int>(gfx::Key::A) + (glfw_key - GLFW_KEY_A));
    }
    if (glfw_key >= GLFW_KEY_0 && glfw_key <= GLFW_KEY_9) {
        return static_cast<gfx::Key>(static_cast<int>(gfx::Key::Zero) + (glfw_key - GLFW_KEY_0));
    }
    switch (glfw_key) {
        case GLFW_KEY_BACKSLASH: return gfx::Key::Backslash;
        case GLFW_KEY_BACKSPACE: return gfx::Key::Backspace;
        case GLFW_KEY_DELETE: return gfx::Key::Delete;
        case GLFW_KEY_DOWN: return gfx::Key::Down;
        case GLFW_KEY_END: return gfx::Key::End;
        case GLFW_KEY_ENTER: return gfx::Key::Enter;
        case GLFW_KEY_EQUAL: return gfx::Key::Equal;
        case GLFW_KEY_ESCAPE: return gfx::Key::Escape;
        case GLFW_KEY_HOME: return gfx::Key::Home;
        case GLFW_KEY_INSERT: return gfx::Key::Insert;
        case GLFW_KEY_KP_ENTER: return gfx::Key::KpEnter;
        case GLFW_KEY_LEFT: return gfx::Key::Left;
        case GLFW_KEY_LEFT_ALT: return gfx::Key::LeftAlt;
        case GLFW_KEY_LEFT_BRACKET: return gfx::Key::LeftBracket;
        case GLFW_KEY_LEFT_CONTROL: return gfx::Key::LeftControl;
        case GLFW_KEY_LEFT_SHIFT: return gfx::Key::LeftShift;
        case GLFW_KEY_LEFT_SUPER: return gfx::Key::LeftSuper;
        case GLFW_KEY_MINUS: return gfx::Key::Minus;
        case GLFW_KEY_PAGE_DOWN: return gfx::Key::PageDown;
        case GLFW_KEY_PAGE_UP: return gfx::Key::PageUp;
        case GLFW_KEY_RIGHT: return gfx::Key::Right;
        case GLFW_KEY_RIGHT_ALT: return gfx::Key::RightAlt;
        case GLFW_KEY_RIGHT_BRACKET: return gfx::Key::RightBracket;
        case GLFW_KEY_RIGHT_CONTROL: return gfx::Key::RightControl;
        case GLFW_KEY_RIGHT_SHIFT: return gfx::Key::RightShift;
        case GLFW_KEY_RIGHT_SUPER: return gfx::Key::RightSuper;
        case GLFW_KEY_TAB: return gfx::Key::Tab;
        case GLFW_KEY_UP: return gfx::Key::Up;
        default: return gfx::Key::None;
    }
}

int MapMouseButton(gfx::MouseButton b) {
    switch (b) {
        case gfx::MouseButton::Left: return GLFW_MOUSE_BUTTON_LEFT;
        case gfx::MouseButton::Right: return GLFW_MOUSE_BUTTON_RIGHT;
        case gfx::MouseButton::Middle: return GLFW_MOUSE_BUTTON_MIDDLE;
    }
    return GLFW_MOUSE_BUTTON_LEFT;
}

}  // namespace

// -- Per-window input state --------------------------------------------
//
// GLFW callbacks are plain C function pointers, so this lives behind
// glfwGetWindowUserPointer rather than a capturing lambda/member
// function. One frame's worth of edge-triggered state (pressed/repeat/
// released) is cleared at the top of WindowShouldClose() -- the chosen
// once-per-frame poll point, mirroring raylib's own desktop backend,
// which likewise polls OS events from inside its WindowShouldClose --
// then repopulated by whatever callbacks glfwPollEvents fires
// synchronously inside that same call.
//
// Deliberately at gfx:: scope, not in the anonymous namespace above/
// below (unlike MapKey/the GLFW callbacks) -- backend_native_internal.h
// forward-declares gfx::NativeContext so backend_native_renderer2d.cpp/
// backend_native_renderer3d.cpp/backend_native_text.cpp can hold a
// pointer to it (for framebuffer-size queries and swap-buffers via
// NativeContextFramebufferSize/NativeContextSwapBuffers below); an
// anonymous-namespace type can't be named from another translation unit.
struct NativeContext {
    GLFWwindow *window = nullptr;
    gfx::Key exit_key = gfx::Key::None;
    int target_fps = 0;

    bool key_down[GLFW_KEY_LAST + 1] = {};
    bool key_pressed[GLFW_KEY_LAST + 1] = {};
    bool key_repeat[GLFW_KEY_LAST + 1] = {};
    bool key_released[GLFW_KEY_LAST + 1] = {};
    std::deque<int> key_queue;            // raw GLFW key codes, drained by GetKeyPressed
    std::deque<unsigned int> char_queue;  // Unicode codepoints, drained by GetCharPressed

    bool mouse_down[GLFW_MOUSE_BUTTON_LAST + 1] = {};
    bool mouse_pressed[GLFW_MOUSE_BUTTON_LAST + 1] = {};
    bool mouse_released[GLFW_MOUSE_BUTTON_LAST + 1] = {};
    double mouse_x = 0.0, mouse_y = 0.0;
    double scroll_x = 0.0, scroll_y = 0.0;

    GLFWcursor *cursors[4] = {};  // indexed by gfx::MouseCursor
};

void NativeContextFramebufferSize(NativeContext *ctx, int *w, int *h) { glfwGetFramebufferSize(ctx->window, w, h); }
void NativeContextSwapBuffers(NativeContext *ctx) { glfwSwapBuffers(ctx->window); }

namespace {

NativeContext *ContextOf(GLFWwindow *window) {
    return static_cast<NativeContext *>(glfwGetWindowUserPointer(window));
}

// Bounded so a mode that never drains GetKeyPressed (nothing in editor.cpp
// does today, but this is defensive) can't grow this without limit --
// raylib's own internal queue is a small fixed-size ring buffer, not an
// unbounded one, for the same reason.
constexpr size_t kMaxQueuedEvents = 32;

void KeyCallback(GLFWwindow *window, int key, int /*scancode*/, int action, int /*mods*/) {
    NativeContext *ctx = ContextOf(window);
    if (ctx == nullptr || key < 0 || key > GLFW_KEY_LAST) return;
    if (action == GLFW_PRESS) {
        ctx->key_down[key] = true;
        ctx->key_pressed[key] = true;
        if (ctx->key_queue.size() < kMaxQueuedEvents) ctx->key_queue.push_back(key);
        if (ctx->exit_key != gfx::Key::None && UnmapKey(key) == ctx->exit_key) {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
    } else if (action == GLFW_REPEAT) {
        ctx->key_repeat[key] = true;
    } else if (action == GLFW_RELEASE) {
        ctx->key_down[key] = false;
        ctx->key_released[key] = true;
    }
}

void CharCallback(GLFWwindow *window, unsigned int codepoint) {
    NativeContext *ctx = ContextOf(window);
    if (ctx == nullptr) return;
    if (ctx->char_queue.size() < kMaxQueuedEvents) ctx->char_queue.push_back(codepoint);
}

void MouseButtonCallback(GLFWwindow *window, int button, int action, int /*mods*/) {
    NativeContext *ctx = ContextOf(window);
    if (ctx == nullptr || button < 0 || button > GLFW_MOUSE_BUTTON_LAST) return;
    if (action == GLFW_PRESS) {
        ctx->mouse_down[button] = true;
        ctx->mouse_pressed[button] = true;
    } else if (action == GLFW_RELEASE) {
        ctx->mouse_down[button] = false;
        ctx->mouse_released[button] = true;
    }
}

void CursorPosCallback(GLFWwindow *window, double x, double y) {
    NativeContext *ctx = ContextOf(window);
    if (ctx == nullptr) return;
    ctx->mouse_x = x;
    ctx->mouse_y = y;
}

void ScrollCallback(GLFWwindow *window, double dx, double dy) {
    NativeContext *ctx = ContextOf(window);
    if (ctx == nullptr) return;
    ctx->scroll_x += dx;
    ctx->scroll_y += dy;
}

}  // namespace

// -- Platform ---------------------------------------------------------------

class NativePlatformBackend : public IPlatformBackend {
public:
    explicit NativePlatformBackend(NativeContext *ctx) : ctx_(ctx) {}

    void InitWindow(int width, int height, const char *title) override {
        glfwInit();  // idempotent if SetWindowResizable() already called it
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        GLFWwindow *window = glfwCreateWindow(width, height, title, nullptr, nullptr);
        if (window == nullptr) {
            std::fprintf(stderr, "gfx native: glfwCreateWindow failed\n");
            std::abort();
        }
        ctx_->window = window;
        glfwSetWindowUserPointer(window, ctx_);
        glfwMakeContextCurrent(window);
        glfwSwapInterval(1);
        if (!gfx::gl::LoadGLFunctions(reinterpret_cast<gfx::gl::GLProcAddressFn>(&glfwGetProcAddress))) {
            std::fprintf(stderr, "gfx native: LoadGLFunctions failed\n");
            std::abort();
        }
        glfwSetKeyCallback(window, KeyCallback);
        glfwSetCharCallback(window, CharCallback);
        glfwSetMouseButtonCallback(window, MouseButtonCallback);
        glfwSetCursorPosCallback(window, CursorPosCallback);
        glfwSetScrollCallback(window, ScrollCallback);
    }
    void CloseWindow() override {
        for (GLFWcursor *cursor : ctx_->cursors) {
            if (cursor != nullptr) glfwDestroyCursor(cursor);
        }
        if (ctx_->window != nullptr) glfwDestroyWindow(ctx_->window);
        glfwTerminate();
    }
    bool IsWindowReady() override { return ctx_->window != nullptr; }
    bool WindowShouldClose() override {
        // The once-per-frame poll point (see NativeContext's own comment):
        // clear this frame's edge-triggered state, then let glfwPollEvents
        // fire callbacks that repopulate it for whatever happened just now.
        for (bool &b : ctx_->key_pressed) b = false;
        for (bool &b : ctx_->key_repeat) b = false;
        for (bool &b : ctx_->key_released) b = false;
        for (bool &b : ctx_->mouse_pressed) b = false;
        for (bool &b : ctx_->mouse_released) b = false;
        ctx_->scroll_x = 0.0;
        ctx_->scroll_y = 0.0;
        glfwPollEvents();
        return glfwWindowShouldClose(ctx_->window) == GLFW_TRUE;
    }
    void SetWindowResizable() override {
        glfwInit();
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    }
    void MaximizeWindow() override { glfwMaximizeWindow(ctx_->window); }
    void SetTargetFPS(int fps) override { ctx_->target_fps = fps; }
    int GetScreenWidth() override {
        int w = 0, h = 0;
        glfwGetFramebufferSize(ctx_->window, &w, &h);
        return w;
    }
    int GetScreenHeight() override {
        int w = 0, h = 0;
        glfwGetFramebufferSize(ctx_->window, &w, &h);
        return h;
    }
    double GetTime() override { return glfwGetTime(); }
    float GetFrameTime() override {
        double now = glfwGetTime();
        float dt = static_cast<float>(now - last_frame_time_);
        last_frame_time_ = now;
        return dt;
    }
    void SetExitKey(gfx::Key key) override { ctx_->exit_key = key; }
    std::string GetClipboardText() override {
        const char *text = glfwGetClipboardString(ctx_->window);
        return text != nullptr ? std::string(text) : std::string();
    }
    void SetClipboardText(const std::string &text) override { glfwSetClipboardString(ctx_->window, text.c_str()); }
    void SetMouseCursor(gfx::MouseCursor cursor) override {
        int index = static_cast<int>(cursor);
        if (ctx_->cursors[index] == nullptr) {
            int shape = GLFW_ARROW_CURSOR;
            switch (cursor) {
                // Old (pre-3.4) cursor-shape names, not the GLFW_POINTING_HAND_CURSOR/
                // GLFW_RESIZE_*_CURSOR names added in GLFW 3.4 -- identical values on
                // desktop (3.4 keeps the old names as aliases), but Emscripten's
                // bundled GLFW shim only implements this older, longer-standing set.
                case gfx::MouseCursor::Default: shape = GLFW_ARROW_CURSOR; break;
                case gfx::MouseCursor::PointingHand: shape = GLFW_HAND_CURSOR; break;
                case gfx::MouseCursor::ResizeEw: shape = GLFW_HRESIZE_CURSOR; break;
                case gfx::MouseCursor::ResizeNs: shape = GLFW_VRESIZE_CURSOR; break;
            }
            ctx_->cursors[index] = glfwCreateStandardCursor(shape);
        }
        glfwSetCursor(ctx_->window, ctx_->cursors[index]);
    }
    // No agent_ui_input.cpp consumer under Emscripten (that module is
    // Linux/X11-only, see its own top comment) -- the exact handle shape
    // here doesn't need to match the native backend's
    // gfx::NativeWindowHandle contract, so this just returns the
    // GLFWwindow* directly, same as before this plan's X11 rewrite.
    void *GetNativeWindowHandle() override { return ctx_->window; }
    void SetTraceLogCallback(TraceLogCallback /*callback*/) override {
        // GLFW has no built-in internal-diagnostic log stream the way
        // raylib's TraceLog is (texture/font/shader load chatter) -- this
        // backend has nothing to redirect. main.cpp's SetUpTraceLogFile
        // still opens its log file; it just stays empty under this
        // backend, which is accurate (there's nothing to log yet).
    }

private:
    NativeContext *ctx_;
    double last_frame_time_ = 0.0;
};

// -- Input --------------------------------------------------------------

class NativeInputBackend : public IInputBackend {
public:
    explicit NativeInputBackend(NativeContext *ctx) : ctx_(ctx) {}

    bool IsKeyPressed(gfx::Key key) override { return InRange(key) && ctx_->key_pressed[MapKey(key)]; }
    bool IsKeyPressedRepeat(gfx::Key key) override {
        return InRange(key) && (ctx_->key_pressed[MapKey(key)] || ctx_->key_repeat[MapKey(key)]);
    }
    bool IsKeyDown(gfx::Key key) override { return InRange(key) && ctx_->key_down[MapKey(key)]; }
    bool IsKeyReleased(gfx::Key key) override { return InRange(key) && ctx_->key_released[MapKey(key)]; }
    // Always false here: this backend has no ReleaseAllKeys-on-FocusOut
    // step to disambiguate in the first place (emcc's GLFW shim owns the
    // canvas' key state), so every release it reports is a real one.
    bool WindowFocusLostThisFrame() override { return false; }
    gfx::Key GetKeyPressed() override {
        if (ctx_->key_queue.empty()) return gfx::Key::None;
        int glfw_key = ctx_->key_queue.front();
        ctx_->key_queue.pop_front();
        return UnmapKey(glfw_key);
    }
    int GetCharPressed() override {
        if (ctx_->char_queue.empty()) return 0;
        unsigned int cp = ctx_->char_queue.front();
        ctx_->char_queue.pop_front();
        return static_cast<int>(cp);
    }
    bool IsMouseButtonPressed(gfx::MouseButton b) override { return ctx_->mouse_pressed[MapMouseButton(b)]; }
    bool IsMouseButtonDown(gfx::MouseButton b) override { return ctx_->mouse_down[MapMouseButton(b)]; }
    bool IsMouseButtonReleased(gfx::MouseButton b) override { return ctx_->mouse_released[MapMouseButton(b)]; }
    gfx::Vector2 GetMousePosition() override {
        return {static_cast<float>(ctx_->mouse_x), static_cast<float>(ctx_->mouse_y)};
    }
    gfx::Vector2 GetMouseWheelMoveV() override {
        return {static_cast<float>(ctx_->scroll_x), static_cast<float>(ctx_->scroll_y)};
    }

private:
    static bool InRange(gfx::Key key) { return MapKey(key) != GLFW_KEY_UNKNOWN; }
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
