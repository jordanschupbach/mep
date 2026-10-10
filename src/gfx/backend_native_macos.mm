// The macOS implementor of the gfx:: native backend: Cocoa/NSOpenGL
// windowing/input/context, the platform counterpart of the X11/GLX
// implementation in gfx/backend_native.cpp (which stays Linux-only --
// exactly one of the two is compiled per platform, selected in
// CMakeLists.txt). The renderer2d/text/renderer3d/audio classes are
// shared (backend_native_renderer2d.cpp etc.); they reach this file only
// through the four NativeContext* free functions declared in
// gfx/backend_native_internal.h, so everything Cocoa stays confined
// here the same way everything Xlib stays confined to backend_native.cpp.
//
// Deliberate differences from the X11 backend, all macOS-shaped:
//   - Command maps to gfx::Key::LeftSuper/RightSuper and Option to
//     LeftAlt/RightAlt, so mep's Super/Alt chords land on the keys a Mac
//     user expects to press.
//   - The GL surface is created at 1x (wantsBestResolutionOpenGLSurface
//     NO): mep lays its UI out in framebuffer pixels with no DPI scale
//     factor anywhere, so a Retina-resolution surface would render
//     everything at half size. 1x keeps sizes right (the window server
//     upscales); proper HiDPI support is future work.
//   - WaitEvents with an extra_fd polls in short slices rather than
//     sleeping on a single select(): there is no selectable fd for the
//     Cocoa event queue, so the wait blocks on the event queue in ~10ms
//     slices and checks the fd between slices. UI events still wake it
//     immediately; fd readability is seen at worst one slice later.
//   - OpenGL is deprecated on macOS but fully functional (GL 4.1 core
//     via NSOpenGLProfileVersion3_2Core, a superset of the 3.3 core
//     feature set the renderers use); GL_SILENCE_DEPRECATION below keeps
//     the SDK's deprecation warnings out of a -Werror build.

#define GL_SILENCE_DEPRECATION 1

// GL_SILENCE_DEPRECATION covers the OpenGL framework's own symbols but
// not AppKit's NSOpenGL-adjacent members (setView:, the
// wantsBestResolutionOpenGLSurface property) -- using NSOpenGL here is
// this backend's deliberate, documented choice (see the top comment), so
// the deprecation nags are silenced for this one file rather than fought
// call site by call site.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#import <Cocoa/Cocoa.h>

#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <thread>

#include <sys/select.h>

#include "gfx/backend_native.h"
#include "gfx/backend_native_internal.h"
#include "gfx/gl_loader.h"
#include "gfx/native_window_handle.h"

@class MepGLView;
@class MepWindowDelegate;
@class MepAppDelegate;

namespace gfx {

namespace {

constexpr int kKeyCount = static_cast<int>(gfx::Key::Up) + 1;

// Same role as backend_native.cpp's MonotonicSeconds: its own epoch,
// only ever compared against itself (frame pacing below).
double MonotonicSeconds() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double>(clock::now() - start).count();
}

// macOS virtual keycodes (Carbon's kVK_* values -- stable hardware
// positions, the same "physical key, not typed character" identity the
// X11 backend's XLookupKeysym(index 0) gives) -> gfx::Key.
gfx::Key KeyFromKeyCode(unsigned short code) {
    switch (code) {
        case 0x00: return gfx::Key::A;
        case 0x0B: return gfx::Key::B;
        case 0x08: return gfx::Key::C;
        case 0x02: return gfx::Key::D;
        case 0x0E: return gfx::Key::E;
        case 0x03: return gfx::Key::F;
        case 0x05: return gfx::Key::G;
        case 0x04: return gfx::Key::H;
        case 0x22: return gfx::Key::I;
        case 0x26: return gfx::Key::J;
        case 0x28: return gfx::Key::K;
        case 0x25: return gfx::Key::L;
        case 0x2E: return gfx::Key::M;
        case 0x2D: return gfx::Key::N;
        case 0x1F: return gfx::Key::O;
        case 0x23: return gfx::Key::P;
        case 0x0C: return gfx::Key::Q;
        case 0x0F: return gfx::Key::R;
        case 0x01: return gfx::Key::S;
        case 0x11: return gfx::Key::T;
        case 0x20: return gfx::Key::U;
        case 0x09: return gfx::Key::V;
        case 0x0D: return gfx::Key::W;
        case 0x07: return gfx::Key::X;
        case 0x10: return gfx::Key::Y;
        case 0x06: return gfx::Key::Z;
        case 0x1D: return gfx::Key::Zero;
        case 0x12: return gfx::Key::One;
        case 0x13: return gfx::Key::Two;
        case 0x14: return gfx::Key::Three;
        case 0x15: return gfx::Key::Four;
        case 0x17: return gfx::Key::Five;
        case 0x16: return gfx::Key::Six;
        case 0x1A: return gfx::Key::Seven;
        case 0x1C: return gfx::Key::Eight;
        case 0x19: return gfx::Key::Nine;
        case 0x2A: return gfx::Key::Backslash;
        case 0x33: return gfx::Key::Backspace;  // kVK_Delete: the Mac "delete" key is backspace
        case 0x75: return gfx::Key::Delete;     // kVK_ForwardDelete
        case 0x7D: return gfx::Key::Down;
        case 0x77: return gfx::Key::End;
        case 0x24: return gfx::Key::Enter;
        case 0x18: return gfx::Key::Equal;
        case 0x35: return gfx::Key::Escape;
        case 0x73: return gfx::Key::Home;
        case 0x72: return gfx::Key::Insert;  // kVK_Help -- the closest thing a Mac keyboard has
        case 0x4C: return gfx::Key::KpEnter;
        case 0x7B: return gfx::Key::Left;
        case 0x3A: return gfx::Key::LeftAlt;  // Option
        case 0x21: return gfx::Key::LeftBracket;
        case 0x3B: return gfx::Key::LeftControl;
        case 0x38: return gfx::Key::LeftShift;
        case 0x37: return gfx::Key::LeftSuper;  // Command
        case 0x1B: return gfx::Key::Minus;
        case 0x79: return gfx::Key::PageDown;
        case 0x74: return gfx::Key::PageUp;
        case 0x7C: return gfx::Key::Right;
        case 0x3D: return gfx::Key::RightAlt;
        case 0x1E: return gfx::Key::RightBracket;
        case 0x3E: return gfx::Key::RightControl;
        case 0x3C: return gfx::Key::RightShift;
        case 0x36: return gfx::Key::RightSuper;
        case 0x31: return gfx::Key::Space;
        case 0x30: return gfx::Key::Tab;
        case 0x7E: return gfx::Key::Up;
        default: return gfx::Key::None;
    }
}

// Same bound and reason as backend_native.cpp's kMaxQueuedEvents.
constexpr size_t kMaxQueuedEvents = 32;

}  // namespace

// -- Per-window input/platform state ---------------------------------------
//
// The Cocoa counterpart of backend_native.cpp's NativeContext: at gfx::
// scope (not anonymous) because backend_native_internal.h forward-declares
// it for the shared renderer/text implementation files. Edge-triggered
// state (pressed/repeat/released) is cleared at the top of
// WindowShouldClose() and repopulated by the view/delegate callbacks that
// fire while that same call pumps the Cocoa event queue.
struct NativeContext {
    NSWindow *window = nil;
    MepGLView *view = nil;
    NSOpenGLContext *gl_context = nil;
    MepWindowDelegate *window_delegate = nil;
    MepAppDelegate *app_delegate = nil;

    NativeWindowHandle window_handle{};  // {NSWindow*, 0}, exposed via GetNativeWindowHandle()

    bool should_close = false;
    gfx::Key exit_key = gfx::Key::None;
    // SetTargetFPS's frame cap -- identical semantics to the X11
    // backend's (see NativeContextSwapBuffers there for the full story).
    int target_fps = 0;
    double frame_deadline = -1.0;
    std::function<void()> flush_2d;  // NativeContextFlush2D
    bool events_this_frame = true;   // EventsThisFrame
    bool focus_lost = false;         // set on resign-key, cleared each poll
    // Whether the window has its title bar (traffic lights). Off by
    // default -- see ApplyTitleBarStyle.
    bool titlebar_visible = false;

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
};

}  // namespace gfx

// -- Cocoa glue object declarations ------------------------------------------
// Declared before the gfx:: helpers below so those can message them
// (a forward @class alone can't receive messages).

// The content view: first responder for the window, translating NSEvents
// into NativeContext state. Flipped so its coordinate origin is the
// top-left like every other gfx:: coordinate.
@interface MepGLView : NSView {
  @public
    gfx::NativeContext *ctx;
}
@end

// A window that can take the keyboard without a title bar: AppKit only
// lets a titled window become key/main unless the subclass says
// otherwise, and mep's window is borderless whenever its title bar is
// hidden (the default -- ApplyTitleBarStyle).
@interface MepWindow : NSWindow
@end

@interface MepWindowDelegate : NSObject <NSWindowDelegate> {
  @public
    gfx::NativeContext *ctx;
}
@end

@interface MepAppDelegate : NSObject <NSApplicationDelegate> {
  @public
    gfx::NativeContext *ctx;
}
@end

namespace gfx {

void NativeContextFlush2D(NativeContext *ctx) {
    if (ctx && ctx->flush_2d) ctx->flush_2d();
}

// Puts the window's style mask in line with ctx->titlebar_visible. Hidden
// means borderless-but-resizable (no title bar, no traffic lights), the
// way iTerm2's "No Title Bar" window style does it; the frame is kept, so
// the content view grows into the strip the title bar gave up (or gives
// it back) and the next frame simply sees a new screen size. Left alone
// while full screen: AppKit's full screen needs a titled window, so
// SetWindowFullscreen restores the title bar going in and
// windowDidExitFullScreen calls this again coming out.
void ApplyTitleBarStyle(NativeContext *ctx) {
    if (ctx->window == nil) return;
    if (([ctx->window styleMask] & NSWindowStyleMaskFullScreen) != 0) return;
    NSUInteger style = NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    if (ctx->titlebar_visible) style |= NSWindowStyleMaskTitled;
    if ([ctx->window styleMask] == style) return;
    // setStyleMask keeps the content size and moves the frame edge; keep
    // the frame instead, so the window stays put and its content takes
    // over (or gives back) the title bar's strip.
    const NSRect frame = [ctx->window frame];
    [ctx->window setStyleMask:style];
    [ctx->window setFrame:frame display:YES];
    [ctx->window setHasShadow:YES];
    [ctx->window makeFirstResponder:ctx->view];
    [ctx->gl_context update];
    ctx->events_this_frame = true;
}

void NativeContextSetFlush2D(NativeContext *ctx, std::function<void()> fn) { ctx->flush_2d = std::move(fn); }

void NativeContextFramebufferSize(NativeContext *ctx, int *w, int *h) {
    if (ctx->view == nil) {
        *w = *h = 0;
        return;
    }
    // 1x surface (wantsBestResolutionOpenGLSurface NO): the GL drawable
    // is exactly the view's bounds in points, so no backing conversion.
    NSRect bounds = [ctx->view bounds];
    *w = static_cast<int>(bounds.size.width);
    *h = static_cast<int>(bounds.size.height);
}

// Ends the frame: presents it, then applies SetTargetFPS's pacing cap.
// Same structure and semantics as the X11 backend's (see the long
// comment there); only the present call itself differs.
void NativeContextSwapBuffers(NativeContext *ctx) {
    [ctx->gl_context flushBuffer];
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

// Releases every held key, reporting each as released this frame -- same
// stuck-modifier insurance as the X11 backend's ReleaseAllKeys (Cmd-Tab
// is macOS's alt-tab: mep sees Cmd go down, the system takes Tab, and
// the resign-key that follows is the only release signal mep gets).
void ReleaseAllKeys(NativeContext *ctx) {
    for (int i = 0; i < kKeyCount; i++) {
        if (ctx->key_down[i]) ctx->key_released[i] = true;
        ctx->key_down[i] = false;
    }
}

void HandleKeyChange(NativeContext *ctx, gfx::Key k, bool down, bool is_repeat) {
    if (k == gfx::Key::None) return;
    const int idx = static_cast<int>(k);
    if (down) {
        if (!ctx->key_down[idx]) {
            ctx->key_down[idx] = true;
            ctx->key_pressed[idx] = true;
            if (ctx->key_queue.size() < kMaxQueuedEvents) ctx->key_queue.push_back(idx);
            if (ctx->exit_key != gfx::Key::None && k == ctx->exit_key) ctx->should_close = true;
        } else if (is_repeat) {
            ctx->key_repeat[idx] = true;
        }
    } else {
        ctx->key_down[idx] = false;
        ctx->key_released[idx] = true;
    }
}

// Text input for one keyDown: the typed characters, with the same
// "no char events under a shortcut modifier" invariant the X11 backend
// enforces for its GetCharPressed consumers (Command/Control/Option all
// count -- Option because mep uses Alt as a chord modifier, so Option
// deliberately does not compose international characters here, matching
// X11's Mod1 suppression). Cocoa reports named keys (arrows, F-keys,
// Home/End, ...) as codepoints in the function-key private-use range
// 0xF700-0xF8FF, and Backspace/Enter/Escape/Delete as the same legacy
// ASCII control codes X11 does -- both filtered for the same
// double-handling reason documented at length in backend_native.cpp.
void HandleTypedCharacters(NativeContext *ctx, NSEvent *event) {
    const NSEventModifierFlags mods = [event modifierFlags];
    if ((mods & (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption)) != 0) return;
    NSString *chars = [event characters];
    const NSUInteger len = [chars length];
    for (NSUInteger i = 0; i < len; i++) {
        unsigned int cp = [chars characterAtIndex:i];
        // Combine UTF-16 surrogate pairs into one codepoint.
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len) {
            const unsigned int low = [chars characterAtIndex:i + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                i++;
            }
        }
        if (cp >= 0xF700 && cp <= 0xF8FF) continue;  // function-key range (arrows, F-keys, ...)
        constexpr unsigned int kBackspace = 0x08, kLineFeed = 0x0A, kCarriageReturn = 0x0D, kEscape = 0x1B,
                                kDelete = 0x7F;
        if (cp == kBackspace || cp == kLineFeed || cp == kCarriageReturn || cp == kEscape || cp == kDelete) continue;
        if (ctx->char_queue.size() < kMaxQueuedEvents) ctx->char_queue.push_back(cp);
    }
}

void HandleMouseButton(NativeContext *ctx, int idx, bool down) {
    if (idx < 0 || idx > 2) return;
    if (down) {
        ctx->mouse_down[idx] = true;
        ctx->mouse_pressed[idx] = true;
    } else {
        ctx->mouse_down[idx] = false;
        ctx->mouse_released[idx] = true;
    }
}

void HandleMouseMove(NativeContext *ctx, NSEvent *event) {
    // The view is flipped (see MepGLView), so converted coordinates are
    // already top-left-origin, matching what every gfx:: caller expects.
    const NSPoint p = [ctx->view convertPoint:[event locationInWindow] fromView:nil];
    ctx->mouse_x = p.x;
    ctx->mouse_y = p.y;
}

}  // namespace
}  // namespace gfx

// -- Cocoa glue object implementations ----------------------------------------

@implementation MepGLView
- (BOOL)isFlipped {
    return YES;
}
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (BOOL)canBecomeKeyView {
    return YES;
}
- (void)keyDown:(NSEvent *)event {
    // Deliberately no [super keyDown:]: an unhandled keyDown reaching
    // NSView's default implementation beeps.
    gfx::HandleKeyChange(ctx, gfx::KeyFromKeyCode([event keyCode]), true, [event isARepeat]);
    gfx::HandleTypedCharacters(ctx, event);
}
- (BOOL)performKeyEquivalent:(NSEvent *)event {
    // Ctrl-Tab / Ctrl-Shift-Tab never reach keyDown: -- AppKit treats
    // them as key equivalents and spends them on key-view-loop focus
    // cycling (Ctrl-Tab is the "tab out of a text view" chord). mep has
    // no other key views, and binds them to next / previous tab, so
    // claim them here and feed them through the ordinary keyDown path.
    if ([event type] == NSEventTypeKeyDown && [event keyCode] == 0x30 &&
        ([event modifierFlags] & NSEventModifierFlagControl) != 0) {
        [self keyDown:event];
        return YES;
    }
    return [super performKeyEquivalent:event];
}
- (void)keyUp:(NSEvent *)event {
    gfx::HandleKeyChange(ctx, gfx::KeyFromKeyCode([event keyCode]), false, false);
}
- (void)flagsChanged:(NSEvent *)event {
    // One flagsChanged fires per modifier press and per release; which of
    // the two it was is read from the device-dependent modifier bit for
    // that specific key (the shared NSEventModifierFlagShift-style masks
    // can't distinguish releasing one of two held Shifts).
    const unsigned short code = [event keyCode];
    const gfx::Key k = gfx::KeyFromKeyCode(code);
    if (k == gfx::Key::None) return;
    NSUInteger device_bit = 0;
    switch (code) {
        case 0x38: device_bit = 0x0002; break;  // NX_DEVICELSHIFTKEYMASK
        case 0x3C: device_bit = 0x0004; break;  // NX_DEVICERSHIFTKEYMASK
        case 0x3B: device_bit = 0x0001; break;  // NX_DEVICELCTLKEYMASK
        case 0x3E: device_bit = 0x2000; break;  // NX_DEVICERCTLKEYMASK
        case 0x3A: device_bit = 0x0020; break;  // NX_DEVICELALTKEYMASK
        case 0x3D: device_bit = 0x0040; break;  // NX_DEVICERALTKEYMASK
        case 0x37: device_bit = 0x0008; break;  // NX_DEVICELCMDKEYMASK
        case 0x36: device_bit = 0x0010; break;  // NX_DEVICERCMDKEYMASK
        default: return;
    }
    const bool down = ([event modifierFlags] & device_bit) != 0;
    gfx::HandleKeyChange(ctx, k, down, false);
}
- (void)mouseDown:(NSEvent *)event {
    // Ctrl+Cmd+drag anywhere moves the window -- with the title bar
    // hidden (the default) there is nothing else to grab it by. The click
    // never reaches mep.
    const NSEventModifierFlags move_mods = NSEventModifierFlagControl | NSEventModifierFlagCommand;
    if (([event modifierFlags] & move_mods) == move_mods) {
        [[self window] performWindowDragWithEvent:event];
        return;
    }
    gfx::HandleMouseMove(ctx, event);
    gfx::HandleMouseButton(ctx, 0, true);
}
- (void)mouseUp:(NSEvent *)event {
    gfx::HandleMouseMove(ctx, event);
    gfx::HandleMouseButton(ctx, 0, false);
}
- (void)rightMouseDown:(NSEvent *)event {
    gfx::HandleMouseMove(ctx, event);
    gfx::HandleMouseButton(ctx, 1, true);
}
- (void)rightMouseUp:(NSEvent *)event {
    gfx::HandleMouseMove(ctx, event);
    gfx::HandleMouseButton(ctx, 1, false);
}
- (void)otherMouseDown:(NSEvent *)event {
    gfx::HandleMouseMove(ctx, event);
    if ([event buttonNumber] == 2) gfx::HandleMouseButton(ctx, 2, true);
}
- (void)otherMouseUp:(NSEvent *)event {
    gfx::HandleMouseMove(ctx, event);
    if ([event buttonNumber] == 2) gfx::HandleMouseButton(ctx, 2, false);
}
- (void)mouseMoved:(NSEvent *)event {
    gfx::HandleMouseMove(ctx, event);
}
- (void)mouseDragged:(NSEvent *)event {
    gfx::HandleMouseMove(ctx, event);
}
- (void)rightMouseDragged:(NSEvent *)event {
    gfx::HandleMouseMove(ctx, event);
}
- (void)otherMouseDragged:(NSEvent *)event {
    gfx::HandleMouseMove(ctx, event);
}
- (void)scrollWheel:(NSEvent *)event {
    double dx = [event scrollingDeltaX];
    double dy = [event scrollingDeltaY];
    if ([event hasPreciseScrollingDeltas]) {
        // Trackpad/Magic Mouse deltas are in pixels; a conventional
        // wheel "line" is ~12px, and gfx:: callers consume wheel units.
        dx /= 12.0;
        dy /= 12.0;
    }
    ctx->scroll_x += dx;
    ctx->scroll_y += dy;
}
@end

@implementation MepWindow
- (BOOL)canBecomeKeyWindow {
    return YES;
}
- (BOOL)canBecomeMainWindow {
    return YES;
}
@end

@implementation MepWindowDelegate
- (BOOL)windowShouldClose:(NSWindow *)sender {
    (void)sender;
    // mep decides when to actually close (unsaved-changes prompts, session
    // save) -- report the request and keep the window, same as the X11
    // backend's WM_DELETE_WINDOW handling.
    ctx->should_close = true;
    return NO;
}
- (void)windowDidResize:(NSNotification *)notification {
    (void)notification;
    [ctx->gl_context update];
}
- (void)windowDidMove:(NSNotification *)notification {
    (void)notification;
    [ctx->gl_context update];
}
- (void)windowDidExitFullScreen:(NSNotification *)notification {
    (void)notification;
    gfx::ApplyTitleBarStyle(ctx);
}
- (void)windowDidResignKey:(NSNotification *)notification {
    (void)notification;
    gfx::ReleaseAllKeys(ctx);
    ctx->focus_lost = true;
}
@end

@implementation MepAppDelegate
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender {
    (void)sender;
    // Dock "Quit" / Cmd-Q route here: convert to mep's own close request
    // instead of letting Cocoa kill the process out from under it.
    ctx->should_close = true;
    return NSTerminateCancel;
}
@end

namespace gfx {

// -- Platform ---------------------------------------------------------------

class NativePlatformBackend : public IPlatformBackend {
public:
    explicit NativePlatformBackend(NativeContext *ctx) : ctx_(ctx) {}

    void InitWindow(int width, int height, const char *title) override {
        @autoreleasepool {
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

            ctx_->app_delegate = [[MepAppDelegate alloc] init];
            ctx_->app_delegate->ctx = ctx_;
            [NSApp setDelegate:ctx_->app_delegate];

            // Minimal menu bar: just the application menu with Quit, so
            // Cmd-Q works and the app looks normal in the menu bar.
            NSMenu *menubar = [[NSMenu alloc] init];
            NSMenuItem *app_item = [[NSMenuItem alloc] init];
            [menubar addItem:app_item];
            NSMenu *app_menu = [[NSMenu alloc] init];
            NSMenuItem *quit_item = [[NSMenuItem alloc] initWithTitle:@"Quit mep"
                                                               action:@selector(terminate:)
                                                        keyEquivalent:@"q"];
            [app_menu addItem:quit_item];
            [app_item setSubmenu:app_menu];
            [NSApp setMainMenu:menubar];

            [NSApp finishLaunching];

            // No title bar to start with (ctx_->titlebar_visible's
            // default); ApplyTitleBarStyle below and SetWindowTitleBarVisible
            // add it back on request.
            const NSUInteger style = NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                                     NSWindowStyleMaskResizable |
                                     (ctx_->titlebar_visible ? NSWindowStyleMaskTitled : 0);
            ctx_->window = [[MepWindow alloc]
                initWithContentRect:NSMakeRect(0, 0, static_cast<CGFloat>(width), static_cast<CGFloat>(height))
                          styleMask:style
                            backing:NSBackingStoreBuffered
                              defer:NO];
            if (ctx_->window == nil) {
                std::fprintf(stderr, "gfx native: NSWindow creation failed\n");
                std::abort();
            }
            [ctx_->window setTitle:[NSString stringWithUTF8String:title]];
            [ctx_->window setAcceptsMouseMovedEvents:YES];
            [ctx_->window setReleasedWhenClosed:NO];
            [ctx_->window setHasShadow:YES];
            // Fullscreen is a first-class window state mep requests
            // (SetWindowFullscreen), not just a user gesture.
            [ctx_->window setCollectionBehavior:[ctx_->window collectionBehavior] |
                                                NSWindowCollectionBehaviorFullScreenPrimary];

            ctx_->view = [[MepGLView alloc] initWithFrame:NSMakeRect(0, 0, static_cast<CGFloat>(width),
                                                                     static_cast<CGFloat>(height))];
            ctx_->view->ctx = ctx_;
            // 1x GL surface -- see this file's top comment on Retina.
            [ctx_->view setWantsBestResolutionOpenGLSurface:NO];
            [ctx_->window setContentView:ctx_->view];
            [ctx_->window makeFirstResponder:ctx_->view];

            ctx_->window_delegate = [[MepWindowDelegate alloc] init];
            ctx_->window_delegate->ctx = ctx_;
            [ctx_->window setDelegate:ctx_->window_delegate];

            const NSOpenGLPixelFormatAttribute attrs[] = {
                NSOpenGLPFAOpenGLProfile,
                NSOpenGLProfileVersion3_2Core,  // 3.2+ core: GL 4.1 in practice
                NSOpenGLPFAColorSize,
                24,
                NSOpenGLPFAAlphaSize,
                8,
                NSOpenGLPFADepthSize,
                24,
                NSOpenGLPFADoubleBuffer,
                NSOpenGLPFAAccelerated,
                0,
            };
            NSOpenGLPixelFormat *pixel_format = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
            if (pixel_format == nil) {
                std::fprintf(stderr, "gfx native: NSOpenGLPixelFormat creation failed\n");
                std::abort();
            }
            ctx_->gl_context = [[NSOpenGLContext alloc] initWithFormat:pixel_format shareContext:nil];
            if (ctx_->gl_context == nil) {
                std::fprintf(stderr, "gfx native: NSOpenGLContext creation failed\n");
                std::abort();
            }

            [ctx_->window center];
            [ctx_->window makeKeyAndOrderFront:nil];

            [ctx_->gl_context setView:ctx_->view];
            [ctx_->gl_context makeCurrentContext];
            [ctx_->gl_context update];

            if (!gfx::gl::LoadGLFunctions(&LoadProcAddress)) {
                std::fprintf(stderr, "gfx native: LoadGLFunctions failed\n");
                std::abort();
            }

            GLint swap_interval = 1;
            [ctx_->gl_context setValues:&swap_interval forParameter:NSOpenGLContextParameterSwapInterval];

            ctx_->window_handle.display = static_cast<void *>(ctx_->window);
            ctx_->window_handle.window = 0;

            [NSApp activateIgnoringOtherApps:YES];
        }
    }
    void CloseWindow() override {
        @autoreleasepool {
            if (ctx_->gl_context != nil) {
                [NSOpenGLContext clearCurrentContext];
                [ctx_->gl_context clearDrawable];
                ctx_->gl_context = nil;
            }
            if (ctx_->window != nil) {
                [ctx_->window setDelegate:nil];
                [ctx_->window close];
                ctx_->window = nil;
            }
            ctx_->view = nil;
            ctx_->window_delegate = nil;
        }
    }
    bool IsWindowReady() override { return ctx_->window != nil; }
    bool EventsThisFrame() override { return ctx_->events_this_frame; }
    void WaitEvents(double timeout_sec, int extra_fd) override {
        if (ctx_->window == nil || timeout_sec <= 0.0) return;
        @autoreleasepool {
            if (extra_fd < 0) {
                // Block on the event queue alone (dequeue:NO -- the next
                // WindowShouldClose poll is what consumes it).
                [NSApp nextEventMatchingMask:NSEventMaskAny
                                   untilDate:[NSDate dateWithTimeIntervalSinceNow:timeout_sec]
                                      inMode:NSDefaultRunLoopMode
                                     dequeue:NO];
                return;
            }
            // No selectable fd exists for the Cocoa event queue, so wait
            // in short slices: block on the queue for a slice (an event
            // wakes it immediately), then poll extra_fd between slices --
            // see this file's top comment.
            const double deadline = MonotonicSeconds() + timeout_sec;
            for (;;) {
                const double remaining = deadline - MonotonicSeconds();
                if (remaining <= 0.0) return;
                const double slice = std::min(remaining, 0.01);
                NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                    untilDate:[NSDate dateWithTimeIntervalSinceNow:slice]
                                                       inMode:NSDefaultRunLoopMode
                                                      dequeue:NO];
                if (event != nil) return;
                fd_set fds;
                FD_ZERO(&fds);
                FD_SET(extra_fd, &fds);
                timeval tv{0, 0};
                if (select(extra_fd + 1, &fds, nullptr, nullptr, &tv) > 0) return;
            }
        }
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
        @autoreleasepool {
            for (;;) {
                NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                    untilDate:[NSDate distantPast]
                                                       inMode:NSDefaultRunLoopMode
                                                      dequeue:YES];
                if (event == nil) break;
                ctx_->events_this_frame = true;
                // sendEvent routes to MepGLView's responder methods (keys,
                // mouse) and to the window/system for everything else.
                [NSApp sendEvent:event];
            }
        }
        return ctx_->should_close;
    }
    void SetWindowResizable() override {
        // The window is created with NSWindowStyleMaskResizable --
        // nothing to configure, same as the X11 backend.
    }
    void MaximizeWindow() override {
        if (ctx_->window != nil && ![ctx_->window isZoomed]) [ctx_->window zoom:nil];
    }
    bool IsWindowMaximized() override {
        if (ctx_->window == nil) return false;
        if (([ctx_->window styleMask] & NSWindowStyleMaskFullScreen) != 0) return false;
        return [ctx_->window isZoomed];
    }
    void SetWindowFullscreen(bool on) override {
        if (ctx_->window == nil) return;
        const bool is_fullscreen = ([ctx_->window styleMask] & NSWindowStyleMaskFullScreen) != 0;
        if (is_fullscreen == on) return;
        // A borderless window can't enter AppKit full screen; give it its
        // title bar back for the trip (windowDidExitFullScreen re-applies
        // ctx_->titlebar_visible on the way out).
        if (on && ([ctx_->window styleMask] & NSWindowStyleMaskTitled) == 0) {
            [ctx_->window setStyleMask:[ctx_->window styleMask] | NSWindowStyleMaskTitled];
        }
        [ctx_->window toggleFullScreen:nil];
    }
    bool SupportsWindowTitleBarToggle() override { return true; }
    bool IsWindowTitleBarVisible() override { return ctx_->titlebar_visible; }
    void SetWindowTitleBarVisible(bool visible) override {
        ctx_->titlebar_visible = visible;
        @autoreleasepool {
            ApplyTitleBarStyle(ctx_);
        }
    }
    void SetTargetFPS(int fps) override {
        const int capped = fps > 0 ? fps : 0;
        if (capped != ctx_->target_fps) ctx_->frame_deadline = -1.0;
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
        double now = GetTime();
        float dt = static_cast<float>(now - last_frame_time_);
        last_frame_time_ = now;
        return dt;
    }
    void SetExitKey(gfx::Key key) override { ctx_->exit_key = key; }
    std::string GetClipboardText() override {
        @autoreleasepool {
            NSString *text = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
            if (text == nil) return std::string();
            const char *utf8 = [text UTF8String];
            return utf8 != nullptr ? std::string(utf8) : std::string();
        }
    }
    void SetClipboardText(const std::string &text) override {
        @autoreleasepool {
            NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
            [pasteboard clearContents];
            [pasteboard setString:[NSString stringWithUTF8String:text.c_str()] forType:NSPasteboardTypeString];
        }
    }
    void SetMouseCursor(gfx::MouseCursor cursor) override {
        @autoreleasepool {
            NSCursor *ns_cursor = nil;
            switch (cursor) {
                case gfx::MouseCursor::Default: ns_cursor = [NSCursor arrowCursor]; break;
                case gfx::MouseCursor::PointingHand: ns_cursor = [NSCursor pointingHandCursor]; break;
                case gfx::MouseCursor::ResizeEw: ns_cursor = [NSCursor resizeLeftRightCursor]; break;
                case gfx::MouseCursor::ResizeNs: ns_cursor = [NSCursor resizeUpDownCursor]; break;
            }
            if (ns_cursor != nil) [ns_cursor set];
        }
    }
    void *GetNativeWindowHandle() override { return &ctx_->window_handle; }
    void SetTraceLogCallback(TraceLogCallback /*callback*/) override {
        // No internal diagnostic log stream to redirect, same as the X11
        // backend (see its comment).
    }

private:
    static void *LoadProcAddress(const char *name) {
        // The OpenGL framework is linked in, so its symbols -- including
        // every post-1.1 entry point -- resolve from the process image.
        return dlsym(RTLD_DEFAULT, name);
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
        // The user's System Settings values, directly -- no caching
        // needed (no server round-trip, unlike XkbGetAutoRepeatRate).
        const double delay = static_cast<double>([NSEvent keyRepeatDelay]);
        const double interval = static_cast<double>([NSEvent keyRepeatInterval]);
        if (delay <= 0.0 || interval <= 0.0) return false;
        if (delay_sec != nullptr) *delay_sec = delay;
        if (interval_sec != nullptr) *interval_sec = interval;
        return true;
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
    // Same declaration-order requirement as the X11 backend's set.
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
