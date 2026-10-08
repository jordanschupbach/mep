// The Win32/WGL implementor of gfx::IPlatformBackend + gfx::IInputBackend
// -- the Windows counterpart of backend_native.cpp's X11/GLX layer and
// backend_native_macos.mm's Cocoa/NSOpenGL one. Everything else in the
// native backend set (the 2D/3D renderers, the text rasterizer, the model
// importers, the audio stub) is already platform-free and is shared
// verbatim with the other two; only the window, the GL context, the event
// pump and the clipboard are rewritten here.
//
// Semantics are matched to backend_native.cpp deliberately, not
// approximated: the once-per-frame edge-state clear happens at the top of
// WindowShouldClose(), repeat is "a key-down for a key already down",
// focus loss releases every held key (so a WM shortcut cannot leave a
// modifier stuck), and GetCharPressed() never fires under a
// Ctrl/Alt/Super chord. editor.cpp relies on all four of those on every
// platform, so a Windows-flavoured reading of any of them would show up
// as input bugs rather than as a missing feature.

// windows.h must come before the gfx headers, and several of its macros
// have to go: it #defines LoadImage -> LoadImageW and DrawTextEx ->
// DrawTextExW, and IRenderer2DBackend/ITextBackend have member functions
// of exactly those two names (gfx/backend.h). Including windows.h first
// and undefining them here means the gfx declarations below are parsed
// with the real names; nothing in this file calls the Win32 originals.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#undef LoadImage
#undef DrawText
#undef DrawTextEx
#undef GetObject
#undef near
#undef far

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <thread>

#include "gfx/backend_native.h"
#include "gfx/backend_native_internal.h"
#include "gfx/gl_loader.h"
#include "gfx/native_window_handle.h"

// WGL_ARB_pixel_format / WGL_ARB_create_context / WGL_EXT_swap_control
// token values, hand-declared rather than pulled from a wglext.h (which
// is not part of the Windows SDK -- it ships with the GL registry, and
// this project vendors no system GL headers; see gl_loader.h's own top
// comment). Values are from the published ARB/EXT specs.
namespace {

constexpr int WGL_DRAW_TO_WINDOW_ARB = 0x2001;
constexpr int WGL_ACCELERATION_ARB = 0x2003;
constexpr int WGL_SUPPORT_OPENGL_ARB = 0x2010;
constexpr int WGL_DOUBLE_BUFFER_ARB = 0x2011;
constexpr int WGL_PIXEL_TYPE_ARB = 0x2013;
constexpr int WGL_COLOR_BITS_ARB = 0x2014;
constexpr int WGL_ALPHA_BITS_ARB = 0x201B;
constexpr int WGL_DEPTH_BITS_ARB = 0x2022;
constexpr int WGL_STENCIL_BITS_ARB = 0x2023;
constexpr int WGL_FULL_ACCELERATION_ARB = 0x2027;
constexpr int WGL_TYPE_RGBA_ARB = 0x202B;

constexpr int WGL_CONTEXT_MAJOR_VERSION_ARB = 0x2091;
constexpr int WGL_CONTEXT_MINOR_VERSION_ARB = 0x2092;
constexpr int WGL_CONTEXT_PROFILE_MASK_ARB = 0x9126;
constexpr int WGL_CONTEXT_CORE_PROFILE_BIT_ARB = 0x00000001;

using WglChoosePixelFormatArbFn = BOOL(WINAPI *)(HDC, const int *, const FLOAT *, UINT, int *, UINT *);
using WglCreateContextAttribsArbFn = HGLRC(WINAPI *)(HDC, HGLRC, const int *);
using WglSwapIntervalExtFn = BOOL(WINAPI *)(int);

WglChoosePixelFormatArbFn g_wgl_choose_pixel_format = nullptr;
WglCreateContextAttribsArbFn g_wgl_create_context_attribs = nullptr;
WglSwapIntervalExtFn g_wgl_swap_interval = nullptr;

const wchar_t *const kWindowClassName = L"MepNativeWindow";

// The IDC_* standard-cursor names resolve to MAKEINTRESOURCEA (an LPSTR)
// unless the whole build defines UNICODE, which this one does not -- and
// every Win32 call in this file is explicitly the W variant, so they
// cannot be passed through as they come. Respelled here with the wide
// MAKEINTRESOURCE; the numbers are the IDC_* values from winuser.h.
const wchar_t *const kCursorArrow = MAKEINTRESOURCEW(32512);   // IDC_ARROW
const wchar_t *const kCursorHand = MAKEINTRESOURCEW(32649);    // IDC_HAND
const wchar_t *const kCursorSizeWe = MAKEINTRESOURCEW(32644);  // IDC_SIZEWE
const wchar_t *const kCursorSizeNs = MAKEINTRESOURCEW(32645);  // IDC_SIZENS

}  // namespace

namespace gfx {

namespace {

// -- Key mapping (Win32 virtual-key based) --------------------------------
//
// Same contract as the X11 backend's UnmapKey: this is the *physical key
// identity* on a US layout ("the A key"), not the character the key would
// type -- WM_CHAR handles text separately below. Win32's A..Z and 0..9
// have no named VK constants because they are literally the ASCII codes
// for 'A'..'Z' and '0'..'9', and gfx::Key's own A..Z / Zero..Nine runs are
// contiguous too, so both ranges map by offset arithmetic.
gfx::Key UnmapKey(UINT vk) {
    if (vk >= 'A' && vk <= 'Z') {
        return static_cast<gfx::Key>(static_cast<int>(gfx::Key::A) + static_cast<int>(vk - 'A'));
    }
    if (vk >= '0' && vk <= '9') {
        return static_cast<gfx::Key>(static_cast<int>(gfx::Key::Zero) + static_cast<int>(vk - '0'));
    }
    switch (vk) {
        case VK_OEM_5: return gfx::Key::Backslash;  // '\|' on a US layout
        case VK_BACK: return gfx::Key::Backspace;
        case VK_DELETE: return gfx::Key::Delete;
        case VK_DOWN: return gfx::Key::Down;
        case VK_END: return gfx::Key::End;
        case VK_RETURN: return gfx::Key::Enter;
        case VK_OEM_PLUS: return gfx::Key::Equal;
        case VK_ESCAPE: return gfx::Key::Escape;
        case VK_HOME: return gfx::Key::Home;
        case VK_INSERT: return gfx::Key::Insert;
        case VK_LEFT: return gfx::Key::Left;
        case VK_LMENU: return gfx::Key::LeftAlt;
        case VK_OEM_4: return gfx::Key::LeftBracket;
        case VK_LCONTROL: return gfx::Key::LeftControl;
        case VK_LSHIFT: return gfx::Key::LeftShift;
        case VK_LWIN: return gfx::Key::LeftSuper;
        case VK_OEM_MINUS: return gfx::Key::Minus;
        case VK_NEXT: return gfx::Key::PageDown;
        case VK_PRIOR: return gfx::Key::PageUp;
        case VK_RIGHT: return gfx::Key::Right;
        case VK_RMENU: return gfx::Key::RightAlt;
        case VK_OEM_6: return gfx::Key::RightBracket;
        case VK_RCONTROL: return gfx::Key::RightControl;
        case VK_RSHIFT: return gfx::Key::RightShift;
        case VK_RWIN: return gfx::Key::RightSuper;
        case VK_SPACE: return gfx::Key::Space;
        case VK_TAB: return gfx::Key::Tab;
        case VK_UP: return gfx::Key::Up;
        default: return gfx::Key::None;
    }
}

constexpr int kKeyCount = static_cast<int>(gfx::Key::Up) + 1;

// Seconds since the first call -- see backend_native.cpp's identical
// helper for why this has its own epoch rather than sharing GetTime()'s.
double MonotonicSeconds() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double>(clock::now() - start).count();
}

}  // namespace

// -- Per-window input/platform state ---------------------------------------
//
// At gfx:: scope (not in an anonymous namespace) for the same reason as
// backend_native.cpp's: backend_native_internal.h forward-declares
// gfx::NativeContext so the shared renderer/text translation units can
// hold a pointer to it.
struct NativeContext {
    HWND hwnd = nullptr;
    HDC hdc = nullptr;
    HGLRC gl_context = nullptr;
    HINSTANCE instance = nullptr;

    // GetNativeWindowHandle(): `display` carries the HWND. The struct's
    // `window` field is an `unsigned long` (32-bit under LLP64) and so
    // cannot hold an HWND without truncation, hence the void* slot -- see
    // native_window_handle.h. No Windows code consumes it today
    // (agent_ui_input.cpp and gui_embed are both X11-only and compile to
    // stubs here), so this exists to keep the contract honest rather than
    // to serve a caller.
    NativeWindowHandle window_handle{};

    bool should_close = false;
    gfx::Key exit_key = gfx::Key::None;
    int target_fps = 0;
    double frame_deadline = -1.0;
    std::function<void()> flush_2d;
    bool events_this_frame = true;
    bool focus_lost = false;

    // SetWindowFullscreen: the pre-fullscreen placement/style to put back.
    bool fullscreen = false;
    WINDOWPLACEMENT saved_placement{};
    LONG_PTR saved_style = 0;
    LONG_PTR saved_ex_style = 0;

    // WM_CHAR delivers UTF-16 code units; a non-BMP character arrives as
    // two messages (a high surrogate then a low one) that have to be
    // recombined into the single codepoint GetCharPressed() promises.
    wchar_t pending_high_surrogate = 0;

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

    // Buttons currently held, so the implicit pointer grab (SetCapture on
    // the first press, ReleaseCapture when the last one goes up) matches
    // the one X11 performs automatically -- without it a drag that leaves
    // the window stops reporting motion, which breaks divider dragging.
    int capture_count = 0;

    HCURSOR cursors[4] = {};  // indexed by gfx::MouseCursor
    gfx::MouseCursor active_cursor = gfx::MouseCursor::Default;
};

void NativeContextFlush2D(NativeContext *ctx) {
    if (ctx && ctx->flush_2d) ctx->flush_2d();
}

void NativeContextSetFlush2D(NativeContext *ctx, std::function<void()> fn) { ctx->flush_2d = std::move(fn); }

void NativeContextFramebufferSize(NativeContext *ctx, int *w, int *h) {
    if (ctx->hwnd == nullptr) {
        *w = *h = 0;
        return;
    }
    RECT rect{};
    GetClientRect(ctx->hwnd, &rect);
    *w = static_cast<int>(rect.right - rect.left);
    *h = static_cast<int>(rect.bottom - rect.top);
}

// Ends the frame: presents it, then sleeps out whatever is left of this
// frame's budget if SetTargetFPS asked for a cap. Identical pacing policy
// to backend_native.cpp's -- see that function's comment for why the cap
// composes with vsync and why an overrun resyncs instead of banking debt.
void NativeContextSwapBuffers(NativeContext *ctx) {
    if (ctx->hdc != nullptr) SwapBuffers(ctx->hdc);
    if (ctx->target_fps <= 0) {
        ctx->frame_deadline = -1.0;
        return;
    }
    const double now = MonotonicSeconds();
    if (ctx->frame_deadline < 0.0) ctx->frame_deadline = now;
    ctx->frame_deadline += 1.0 / static_cast<double>(ctx->target_fps);
    if (ctx->frame_deadline <= now) {
        ctx->frame_deadline = now;
        return;
    }
    std::this_thread::sleep_for(std::chrono::duration<double>(ctx->frame_deadline - now));
}

namespace {

constexpr size_t kMaxQueuedEvents = 32;

// Releases every held key, reporting each as released this frame. Called
// on WM_KILLFOCUS, for the reason backend_native.cpp's twin documents: a
// key let go while another window holds the keyboard (alt-tab, a global
// hotkey, the Start menu) never sends its key-up here, and a modifier
// stuck down silently turns every later chord into its Shift/Ctrl variant.
void ReleaseAllKeys(NativeContext *ctx) {
    for (int i = 0; i < kKeyCount; i++) {
        if (ctx->key_down[i]) ctx->key_released[i] = true;
        ctx->key_down[i] = false;
    }
}

// WM_KEYDOWN/WM_KEYUP report the *unsided* VK_SHIFT/VK_CONTROL/VK_MENU for
// both the left and right keys; mep binds the two sides separately
// (gfx::Key::LeftControl vs RightControl), so they are resolved here from
// the message's own scancode and extended-key bit the way the Win32 docs
// prescribe.
UINT ResolveSidedVirtualKey(UINT vk, LPARAM lparam) {
    const UINT scancode = static_cast<UINT>((lparam >> 16) & 0xFF);
    const bool extended = ((lparam >> 24) & 1) != 0;
    switch (vk) {
        case VK_SHIFT:
            // MAPVK_VSC_TO_VK_EX is what turns a scancode into the sided
            // VK_LSHIFT/VK_RSHIFT (plain MAPVK_VSC_TO_VK would hand back
            // the unsided VK_SHIFT again).
            return MapVirtualKeyW(scancode, MAPVK_VSC_TO_VK_EX);
        case VK_CONTROL: return extended ? VK_RCONTROL : VK_LCONTROL;
        case VK_MENU: return extended ? VK_RMENU : VK_LMENU;
        default: return vk;
    }
}

// Numpad Enter shares VK_RETURN with the main Enter key and is told apart
// only by the extended-key bit.
gfx::Key KeyFromMessage(UINT vk, LPARAM lparam) {
    const UINT sided = ResolveSidedVirtualKey(vk, lparam);
    if (sided == VK_RETURN && ((lparam >> 24) & 1) != 0) return gfx::Key::KpEnter;
    return UnmapKey(sided);
}

void HandleKeyDown(NativeContext *ctx, UINT vk, LPARAM lparam) {
    const gfx::Key k = KeyFromMessage(vk, lparam);
    if (k == gfx::Key::None) return;
    const int idx = static_cast<int>(k);
    if (!ctx->key_down[idx]) {
        ctx->key_down[idx] = true;
        ctx->key_pressed[idx] = true;
        if (ctx->key_queue.size() < kMaxQueuedEvents) ctx->key_queue.push_back(idx);
        if (ctx->exit_key != gfx::Key::None && k == ctx->exit_key) ctx->should_close = true;
    } else {
        // Auto-repeat: Windows sends repeated WM_KEYDOWNs with no
        // intervening WM_KEYUP (lParam bit 30 flags it too), which is the
        // same signal the X11 backend gets once detectable autorepeat is
        // on -- so "a key-down for a key already down" means repeat here
        // exactly as it does there.
        ctx->key_repeat[idx] = true;
    }
}

void HandleKeyUp(NativeContext *ctx, UINT vk, LPARAM lparam) {
    const gfx::Key k = KeyFromMessage(vk, lparam);
    if (k == gfx::Key::None) return;
    const int idx = static_cast<int>(k);
    ctx->key_down[idx] = false;
    ctx->key_released[idx] = true;
}

// A typed character, from WM_CHAR's UTF-16 code unit.
//
// Suppressed entirely while Control, Alt or Super is held, enforcing the
// invariant every GetCharPressed() consumer in editor.cpp relies on ("no
// char event fires under a recognized modifier chord"). Windows does not
// provide that on its own -- a Ctrl-chord produces a WM_CHAR carrying the
// corresponding ASCII control code -- so, exactly as the X11 backend does
// with its own kModifierMask check, it is imposed here rather than at each
// of the call sites that assume it.
void HandleChar(NativeContext *ctx, wchar_t unit) {
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool super = ((GetKeyState(VK_LWIN) & 0x8000) != 0) || ((GetKeyState(VK_RWIN) & 0x8000) != 0);
    if (ctrl || alt || super) {
        ctx->pending_high_surrogate = 0;
        return;
    }

    unsigned int cp = 0;
    if (unit >= 0xD800 && unit <= 0xDBFF) {
        ctx->pending_high_surrogate = unit;  // wait for the low half
        return;
    }
    if (unit >= 0xDC00 && unit <= 0xDFFF) {
        if (ctx->pending_high_surrogate == 0) return;  // unpaired low surrogate
        cp = 0x10000u + ((static_cast<unsigned int>(ctx->pending_high_surrogate) - 0xD800u) << 10) +
             (static_cast<unsigned int>(unit) - 0xDC00u);
        ctx->pending_high_surrogate = 0;
    } else {
        ctx->pending_high_surrogate = 0;
        cp = static_cast<unsigned int>(unit);
    }

    // The same control bytes the X11 backend drops: Backspace/Enter/
    // Escape/Delete each already reach the editor as a named gfx::Key via
    // the queue above, and letting the legacy ASCII control code through
    // here too would double-handle one physical keypress. Tab (0x09) is
    // deliberately left in, matching that backend exactly -- see its
    // comment for the audit that decision rests on.
    constexpr unsigned int kBackspace = 0x08, kLineFeed = 0x0A, kCarriageReturn = 0x0D, kEscape = 0x1B,
                           kDelete = 0x7F;
    if (cp == kBackspace || cp == kLineFeed || cp == kCarriageReturn || cp == kEscape || cp == kDelete) return;
    if (ctx->char_queue.size() < kMaxQueuedEvents) ctx->char_queue.push_back(cp);
}

void HandleMouseButton(NativeContext *ctx, int idx, bool down) {
    if (idx < 0 || idx > 2) return;
    if (down) {
        ctx->mouse_down[idx] = true;
        ctx->mouse_pressed[idx] = true;
        if (ctx->capture_count++ == 0) SetCapture(ctx->hwnd);
    } else {
        ctx->mouse_down[idx] = false;
        ctx->mouse_released[idx] = true;
        if (ctx->capture_count > 0 && --ctx->capture_count == 0) ReleaseCapture();
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    // The context pointer is attached in WM_NCCREATE (the first message a
    // window ever gets), so every later message can reach it; the few that
    // arrive before it -- WM_GETMINMAXINFO does -- fall through to
    // DefWindowProc.
    if (msg == WM_NCCREATE) {
        auto *cs = reinterpret_cast<CREATESTRUCTW *>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    auto *ctx = reinterpret_cast<NativeContext *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (ctx == nullptr) return DefWindowProcW(hwnd, msg, wparam, lparam);

    switch (msg) {
        case WM_CLOSE: ctx->should_close = true; return 0;
        case WM_ERASEBKGND:
            // GL owns every pixel of the client area; letting GDI clear it
            // first only adds a flash on resize.
            return 1;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            HandleKeyDown(ctx, static_cast<UINT>(wparam), lparam);
            // WM_SYSKEYDOWN still goes on to DefWindowProc so Alt+F4 and
            // the other system chords keep working; WM_KEYDOWN is fully
            // handled here.
            if (msg == WM_SYSKEYDOWN) break;
            return 0;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            HandleKeyUp(ctx, static_cast<UINT>(wparam), lparam);
            if (msg == WM_SYSKEYUP) break;
            return 0;
        case WM_CHAR: HandleChar(ctx, static_cast<wchar_t>(wparam)); return 0;
        case WM_SYSCHAR:
            // An Alt-chord's character. Swallowed rather than queued (the
            // chord is a shortcut, not text) and kept away from
            // DefWindowProc, which would otherwise ring the menu bell for
            // every Alt combination mep binds.
            return 0;
        case WM_SYSCOMMAND:
            // Tapping Alt alone opens the (nonexistent) window menu and
            // beeps; mep uses bare Alt as a modifier, so that is dropped.
            if ((wparam & 0xFFF0) == SC_KEYMENU) return 0;
            break;
        case WM_KILLFOCUS:
            ReleaseAllKeys(ctx);
            ctx->focus_lost = true;
            // A capture is lost along with the focus; keep our own count
            // in step so the next press re-captures.
            ctx->capture_count = 0;
            return 0;
        case WM_CAPTURECHANGED: ctx->capture_count = 0; return 0;
        case WM_LBUTTONDOWN: HandleMouseButton(ctx, 0, true); return 0;
        case WM_LBUTTONUP: HandleMouseButton(ctx, 0, false); return 0;
        case WM_RBUTTONDOWN: HandleMouseButton(ctx, 1, true); return 0;
        case WM_RBUTTONUP: HandleMouseButton(ctx, 1, false); return 0;
        case WM_MBUTTONDOWN: HandleMouseButton(ctx, 2, true); return 0;
        case WM_MBUTTONUP: HandleMouseButton(ctx, 2, false); return 0;
        case WM_MOUSEMOVE:
            ctx->mouse_x = GET_X_LPARAM(lparam);
            ctx->mouse_y = GET_Y_LPARAM(lparam);
            return 0;
        case WM_MOUSEWHEEL:
            ctx->scroll_y += static_cast<double>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
            return 0;
        case WM_MOUSEHWHEEL:
            ctx->scroll_x += static_cast<double>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
            return 0;
        case WM_SETCURSOR:
            // Windows resets the cursor to the window class's every time
            // the pointer moves over the client area, so whichever one
            // SetMouseCursor last chose is reapplied here; the non-client
            // area (borders, title bar) keeps its own resize arrows.
            if (LOWORD(lparam) == HTCLIENT) {
                const int index = static_cast<int>(ctx->active_cursor);
                if (ctx->cursors[index] != nullptr) {
                    SetCursor(ctx->cursors[index]);
                    return TRUE;
                }
            }
            break;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// UTF-8 <-> UTF-16 for the clipboard, which is UTF-16 on Windows while
// every gfx:: string is UTF-8.
std::wstring Utf8ToWide(const std::string &text) {
    if (text.empty()) return std::wstring();
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed);
    return out;
}

// `length` is a code-unit count, or -1 for "NUL-terminated"; with -1 the
// terminator is included in the result, which the one caller strips.
std::string WideToUtf8(const wchar_t *text, int length) {
    if (text == nullptr || length == 0) return std::string();
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return std::string();
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), needed, nullptr, nullptr);
    return out;
}

// Resolves a GL entry point.
//
// The Windows-specific trap this exists for: wglGetProcAddress returns
// null for everything in OpenGL 1.1, because those symbols are exported
// directly from opengl32.dll instead. gl_loader.cpp asks for plenty of
// them (glClear, glViewport, glTexImage2D, glDrawArrays, ...), so a loader
// that only called wglGetProcAddress would fail at startup on every
// machine. Try the extension mechanism first, fall back to the DLL export
// table.
void *LoadProcAddressWin32(const char *name) {
    if (auto *p = reinterpret_cast<void *>(wglGetProcAddress(name))) {
        // Some drivers historically returned these sentinel values rather
        // than null for an unsupported entry point.
        const auto raw = reinterpret_cast<INT_PTR>(p);
        if (raw != 0 && raw != 1 && raw != 2 && raw != 3 && raw != -1) return p;
    }
    static HMODULE opengl32 = LoadLibraryW(L"opengl32.dll");
    if (opengl32 == nullptr) return nullptr;
    return reinterpret_cast<void *>(GetProcAddress(opengl32, name));
}

// A GL 3.3 core context needs wglCreateContextAttribsARB, and choosing a
// pixel format properly needs wglChoosePixelFormatARB -- both of which are
// themselves WGL extensions, so they can only be resolved through a
// context that already exists. The standard way out is this throwaway
// window: give it a legacy ChoosePixelFormat pixel format, make a legacy
// context on it, read the function pointers, then destroy the lot before
// the real window is created (a window's pixel format can be set only
// once, which is why the dummy cannot simply be reused).
void LoadWglExtensions() {
    if (g_wgl_create_context_attribs != nullptr) return;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MepWglBootstrap";
    RegisterClassExW(&wc);

    HWND dummy = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1, 1,
                                 nullptr, nullptr, wc.hInstance, nullptr);
    if (dummy != nullptr) {
        HDC dc = GetDC(dummy);
        PIXELFORMATDESCRIPTOR pfd{};
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cDepthBits = 24;
        const int format = ChoosePixelFormat(dc, &pfd);
        if (format != 0 && SetPixelFormat(dc, format, &pfd)) {
            HGLRC rc = wglCreateContext(dc);
            if (rc != nullptr && wglMakeCurrent(dc, rc)) {
                g_wgl_choose_pixel_format =
                    reinterpret_cast<WglChoosePixelFormatArbFn>(wglGetProcAddress("wglChoosePixelFormatARB"));
                g_wgl_create_context_attribs =
                    reinterpret_cast<WglCreateContextAttribsArbFn>(wglGetProcAddress("wglCreateContextAttribsARB"));
                g_wgl_swap_interval = reinterpret_cast<WglSwapIntervalExtFn>(wglGetProcAddress("wglSwapIntervalEXT"));
                wglMakeCurrent(nullptr, nullptr);
            }
            if (rc != nullptr) wglDeleteContext(rc);
        }
        ReleaseDC(dummy, dc);
        DestroyWindow(dummy);
    }
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}

// Per-monitor DPI awareness, resolved at run time rather than declared in
// a manifest (there is no manifest in this build). Without it Windows
// scales the window's backing store for us and everything mep renders --
// which is every pixel, since it rasterizes its own glyphs -- comes out
// blurred on any display above 100% scaling.
void EnableDpiAwareness() {
    HMODULE user32 = LoadLibraryW(L"user32.dll");
    if (user32 == nullptr) return;
    using SetProcessDpiAwarenessContextFn = BOOL(WINAPI *)(HANDLE);
    auto set_awareness =
        reinterpret_cast<SetProcessDpiAwarenessContextFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (set_awareness != nullptr) {
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
        set_awareness(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)));
    }
    FreeLibrary(user32);
}

}  // namespace

// -- Platform ---------------------------------------------------------------

class NativePlatformBackend : public IPlatformBackend {
public:
    explicit NativePlatformBackend(NativeContext *ctx) : ctx_(ctx) {}

    void InitWindow(int width, int height, const char *title) override {
        EnableDpiAwareness();
        ctx_->instance = GetModuleHandleW(nullptr);

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        // CS_OWNDC: the window keeps one private device context for its
        // lifetime, which is what lets the GL context stay bound to the
        // single HDC cached in NativeContext instead of being re-fetched
        // and re-bound every frame.
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = ctx_->instance;
        wc.hCursor = LoadCursorW(nullptr, kCursorArrow);
        wc.lpszClassName = kWindowClassName;
        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            std::fprintf(stderr, "gfx native: RegisterClassExW failed (%lu)\n", GetLastError());
            std::abort();
        }

        LoadWglExtensions();

        // `width`/`height` are the client area mep wants to draw into, so
        // the frame is added on top rather than eaten out of it.
        RECT rect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0);

        const std::wstring wide_title = Utf8ToWide(title != nullptr ? title : "");
        ctx_->hwnd = CreateWindowExW(0, kWindowClassName, wide_title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                     CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                                     ctx_->instance, ctx_);
        if (ctx_->hwnd == nullptr) {
            std::fprintf(stderr, "gfx native: CreateWindowExW failed (%lu)\n", GetLastError());
            std::abort();
        }
        ctx_->hdc = GetDC(ctx_->hwnd);

        SetUpPixelFormat();
        CreateGlContext();

        if (!gfx::gl::LoadGLFunctions(&LoadProcAddressWin32)) {
            std::fprintf(stderr, "gfx native: LoadGLFunctions failed\n");
            std::abort();
        }
        if (g_wgl_swap_interval != nullptr) g_wgl_swap_interval(1);

        ctx_->window_handle.display = ctx_->hwnd;
        ctx_->window_handle.window = 0;

        ShowWindow(ctx_->hwnd, SW_SHOW);
        UpdateWindow(ctx_->hwnd);
        SetForegroundWindow(ctx_->hwnd);
        SetFocus(ctx_->hwnd);
    }

    void CloseWindow() override {
        if (ctx_->gl_context != nullptr) {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(ctx_->gl_context);
            ctx_->gl_context = nullptr;
        }
        if (ctx_->hdc != nullptr && ctx_->hwnd != nullptr) {
            ReleaseDC(ctx_->hwnd, ctx_->hdc);
            ctx_->hdc = nullptr;
        }
        if (ctx_->hwnd != nullptr) {
            DestroyWindow(ctx_->hwnd);
            ctx_->hwnd = nullptr;
        }
        UnregisterClassW(kWindowClassName, ctx_->instance);
        // The cursors are LoadCursorW handles on the system's own shared
        // images, which must not be destroyed.
    }

    bool IsWindowReady() override { return ctx_->hwnd != nullptr; }
    bool EventsThisFrame() override { return ctx_->events_this_frame; }

    // Sleeps until a message arrives or the timeout expires.
    //
    // `extra_fd` is ignored: it is a POSIX file descriptor, and the one
    // caller that passes a real one (the agent RPC socket in main.cpp) is
    // compiled out on Windows along with the rest of the socket layer. The
    // timeout still bounds the wait, so a future Windows RPC path would
    // see at most one timeout's worth of latency rather than a hang -- but
    // this needs revisiting (MsgWaitForMultipleObjects over a
    // WSAEventSelect handle) if that layer is ever enabled here.
    void WaitEvents(double timeout_sec, int extra_fd) override {
        (void)extra_fd;
        if (ctx_->hwnd == nullptr || timeout_sec <= 0.0) return;
        MSG msg;
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE)) return;
        const DWORD ms = static_cast<DWORD>(std::min(timeout_sec * 1000.0, 4.0e9));
        MsgWaitForMultipleObjects(0, nullptr, FALSE, ms, QS_ALLINPUT);
    }

    bool WindowShouldClose() override {
        for (bool &b : ctx_->key_pressed) b = false;
        for (bool &b : ctx_->key_repeat) b = false;
        for (bool &b : ctx_->key_released) b = false;
        for (bool &b : ctx_->mouse_pressed) b = false;
        for (bool &b : ctx_->mouse_released) b = false;
        ctx_->focus_lost = false;
        ctx_->scroll_x = 0.0;
        ctx_->scroll_y = 0.0;
        ctx_->events_this_frame = false;

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ctx_->events_this_frame = true;
            if (msg.message == WM_QUIT) {
                ctx_->should_close = true;
                break;
            }
            TranslateMessage(&msg);  // turns WM_KEYDOWN into the WM_CHAR text events
            DispatchMessageW(&msg);
        }
        return ctx_->should_close;
    }

    void SetWindowResizable() override {
        // Created WS_OVERLAPPEDWINDOW, which already includes
        // WS_THICKFRAME -- mep always wants a resizable window and never
        // asks for the opposite, same as on X11.
    }

    void MaximizeWindow() override {
        if (ctx_->hwnd != nullptr) ShowWindow(ctx_->hwnd, SW_MAXIMIZE);
    }

    bool IsWindowMaximized() override {
        // Asks the window itself rather than tracking it: the user
        // maximizing by double-clicking the title bar or pressing Win+Up
        // never goes through MaximizeWindow().
        return ctx_->hwnd != nullptr && IsZoomed(ctx_->hwnd) != 0;
    }

    void SetWindowFullscreen(bool on) override {
        if (ctx_->hwnd == nullptr || on == ctx_->fullscreen) return;
        if (on) {
            // Borderless-on-the-monitor fullscreen: remember the framed
            // geometry and style, strip the frame, then size to the
            // monitor the window is currently on (not the primary one).
            ctx_->saved_placement.length = sizeof(ctx_->saved_placement);
            GetWindowPlacement(ctx_->hwnd, &ctx_->saved_placement);
            ctx_->saved_style = GetWindowLongPtrW(ctx_->hwnd, GWL_STYLE);
            ctx_->saved_ex_style = GetWindowLongPtrW(ctx_->hwnd, GWL_EXSTYLE);

            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (!GetMonitorInfoW(MonitorFromWindow(ctx_->hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return;

            SetWindowLongPtrW(ctx_->hwnd, GWL_STYLE,
                              (ctx_->saved_style & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW)) |
                                  static_cast<LONG_PTR>(WS_POPUP));
            SetWindowLongPtrW(ctx_->hwnd, GWL_EXSTYLE,
                              ctx_->saved_ex_style & ~static_cast<LONG_PTR>(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE));
            SetWindowPos(ctx_->hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        } else {
            SetWindowLongPtrW(ctx_->hwnd, GWL_STYLE, ctx_->saved_style);
            SetWindowLongPtrW(ctx_->hwnd, GWL_EXSTYLE, ctx_->saved_ex_style);
            SetWindowPos(ctx_->hwnd, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            SetWindowPlacement(ctx_->hwnd, &ctx_->saved_placement);
        }
        ctx_->fullscreen = on;
    }

    void SetKeyboardFocusProxy(bool on) override {
        // An X11-specific problem with no Windows equivalent: keyboard
        // focus here follows the focused window, not the pointer, so an
        // embedded child window cannot steal mep's keys just by sitting
        // under the cursor. (gui_embed is X11-only and stubs out here
        // anyway.)
        (void)on;
    }

    void SetTargetFPS(int fps) override {
        const int capped = fps > 0 ? fps : 0;
        if (capped != ctx_->target_fps) ctx_->frame_deadline = -1.0;  // restart pacing, don't inherit a stale deadline
        ctx_->target_fps = capped;
    }

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
        const double now = GetTime();
        const float dt = static_cast<float>(now - last_frame_time_);
        last_frame_time_ = now;
        return dt;
    }

    void SetExitKey(gfx::Key key) override { ctx_->exit_key = key; }

    std::string GetClipboardText() override {
        if (!OpenClipboard(ctx_->hwnd)) return std::string();
        std::string result;
        if (HANDLE handle = GetClipboardData(CF_UNICODETEXT)) {
            if (auto *text = static_cast<const wchar_t *>(GlobalLock(handle))) {
                result = WideToUtf8(text, -1);
                // A -1 length counts the terminating NUL too; drop it so
                // the string's size is the text's.
                if (!result.empty() && result.back() == '\0') result.pop_back();
                GlobalUnlock(handle);
            }
        }
        CloseClipboard();
        return result;
    }

    void SetClipboardText(const std::string &text) override {
        if (!OpenClipboard(ctx_->hwnd)) return;
        EmptyClipboard();
        const std::wstring wide = Utf8ToWide(text);
        const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
        // Ownership of the block passes to the clipboard on a successful
        // SetClipboardData, so it is freed here only when that fails.
        if (HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            if (auto *dest = static_cast<wchar_t *>(GlobalLock(handle))) {
                std::memcpy(dest, wide.c_str(), bytes);
                GlobalUnlock(handle);
                if (SetClipboardData(CF_UNICODETEXT, handle) == nullptr) GlobalFree(handle);
            } else {
                GlobalFree(handle);
            }
        }
        CloseClipboard();
    }

    void SetMouseCursor(gfx::MouseCursor cursor) override {
        const int index = static_cast<int>(cursor);
        if (ctx_->cursors[index] == nullptr) {
            const wchar_t *shape = kCursorArrow;
            switch (cursor) {
                case gfx::MouseCursor::Default: shape = kCursorArrow; break;
                case gfx::MouseCursor::PointingHand: shape = kCursorHand; break;
                case gfx::MouseCursor::ResizeEw: shape = kCursorSizeWe; break;
                case gfx::MouseCursor::ResizeNs: shape = kCursorSizeNs; break;
            }
            ctx_->cursors[index] = LoadCursorW(nullptr, shape);
        }
        // Remembered as well as set, because WM_SETCURSOR has to reapply
        // it on every pointer move over the client area.
        ctx_->active_cursor = cursor;
        if (ctx_->cursors[index] != nullptr) SetCursor(ctx_->cursors[index]);
    }

    void *GetNativeWindowHandle() override { return &ctx_->window_handle; }

    void SetTraceLogCallback(TraceLogCallback /*callback*/) override {
        // No internal diagnostic log stream of its own to redirect, same
        // as the X11 and Cocoa backends.
    }

private:
    void SetUpPixelFormat() {
        int format = 0;
        UINT num_formats = 0;
        if (g_wgl_choose_pixel_format != nullptr) {
            const int attribs[] = {WGL_DRAW_TO_WINDOW_ARB,
                                   TRUE,
                                   WGL_SUPPORT_OPENGL_ARB,
                                   TRUE,
                                   WGL_DOUBLE_BUFFER_ARB,
                                   TRUE,
                                   WGL_ACCELERATION_ARB,
                                   WGL_FULL_ACCELERATION_ARB,
                                   WGL_PIXEL_TYPE_ARB,
                                   WGL_TYPE_RGBA_ARB,
                                   WGL_COLOR_BITS_ARB,
                                   32,
                                   WGL_ALPHA_BITS_ARB,
                                   8,
                                   WGL_DEPTH_BITS_ARB,
                                   24,
                                   WGL_STENCIL_BITS_ARB,
                                   8,
                                   0};
            if (!g_wgl_choose_pixel_format(ctx_->hdc, attribs, nullptr, 1, &format, &num_formats) ||
                num_formats == 0) {
                format = 0;
            }
        }

        PIXELFORMATDESCRIPTOR pfd{};
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cAlphaBits = 8;
        pfd.cDepthBits = 24;
        pfd.cStencilBits = 8;
        if (format == 0) format = ChoosePixelFormat(ctx_->hdc, &pfd);
        if (format == 0) {
            std::fprintf(stderr, "gfx native: no suitable pixel format\n");
            std::abort();
        }
        // DescribePixelFormat before SetPixelFormat: required when the
        // format index came from wglChoosePixelFormatARB, which the PFD
        // above does not describe.
        DescribePixelFormat(ctx_->hdc, format, sizeof(pfd), &pfd);
        if (!SetPixelFormat(ctx_->hdc, format, &pfd)) {
            std::fprintf(stderr, "gfx native: SetPixelFormat failed (%lu)\n", GetLastError());
            std::abort();
        }
    }

    void CreateGlContext() {
        if (g_wgl_create_context_attribs == nullptr) {
            std::fprintf(stderr, "gfx native: no wglCreateContextAttribsARB (OpenGL 3.3 core unavailable)\n");
            std::abort();
        }
        const int attribs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB,
                               3,
                               WGL_CONTEXT_MINOR_VERSION_ARB,
                               3,
                               WGL_CONTEXT_PROFILE_MASK_ARB,
                               WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
                               0};
        ctx_->gl_context = g_wgl_create_context_attribs(ctx_->hdc, nullptr, attribs);
        if (ctx_->gl_context == nullptr) {
            std::fprintf(stderr, "gfx native: wglCreateContextAttribsARB failed (%lu)\n", GetLastError());
            std::abort();
        }
        if (!wglMakeCurrent(ctx_->hdc, ctx_->gl_context)) {
            std::fprintf(stderr, "gfx native: wglMakeCurrent failed (%lu)\n", GetLastError());
            std::abort();
        }
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
    bool GetKeyRepeatRate(double *delay_sec, double *interval_sec) override {
        // SystemParametersInfo reports both as small opaque indices rather
        // than times: the delay is 0..3 meaning 250ms..1000ms in 250ms
        // steps, and the speed is 0..31 meaning roughly 2.5..30 repeats
        // per second, linearly. Both conversions are the ones the Win32
        // docs give for SPI_GETKEYBOARDDELAY/SPI_GETKEYBOARDSPEED.
        DWORD delay_index = 0;
        DWORD speed_index = 0;
        if (!SystemParametersInfoW(SPI_GETKEYBOARDDELAY, 0, &delay_index, 0)) return false;
        if (!SystemParametersInfoW(SPI_GETKEYBOARDSPEED, 0, &speed_index, 0)) return false;
        const double delay = 0.25 * static_cast<double>(delay_index + 1);
        const double repeats_per_sec = 2.5 + (27.5 * static_cast<double>(speed_index) / 31.0);
        if (repeats_per_sec <= 0.0) return false;
        if (delay_sec != nullptr) *delay_sec = delay;
        if (interval_sec != nullptr) *interval_sec = 1.0 / repeats_per_sec;
        return true;
    }
    bool IsKeyDown(gfx::Key key) override { return InRange(key) && ctx_->key_down[static_cast<int>(key)]; }
    bool IsKeyReleased(gfx::Key key) override { return InRange(key) && ctx_->key_released[static_cast<int>(key)]; }
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
    // Declaration order matters -- see backend_native.cpp's twin: the text
    // backend's constructor needs &renderer2d already valid.
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
