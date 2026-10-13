// agent_ui_input.h on macOS: the agent socket's mouse methods
// ("ui.mouse_*"), as NSEvents posted to mep's own event queue. Unlike
// XTest -- or CGEventPost, its Quartz counterpart -- this needs no
// permission: the events never leave mep, and the event pump
// (backend_native_macos.mm's WindowShouldClose) hands them to MepGLView's
// mouse methods like any real click. Keys and the wheel are not done
// here: a key event mep would read correctly needs the keyboard layout's
// key codes, and :normal already covers typing from the socket.

#include "agent_ui_input.h"

#if defined(__APPLE__)

#import <Cocoa/Cocoa.h>

#include <cstdlib>

#include "gfx/native_window_handle.h"

namespace mep::agent_ui {
namespace {

NSWindow *g_window = nil;
int g_buttons_down = 0;  // bitmask by mep button number (1 left, 2 middle, 3 right)

// mep's coordinates (MepGLView's: flipped, in points) as the window's.
NSPoint WindowPoint(int x, int y) {
    NSView *view = g_window.contentView;
    return [view convertPoint:NSMakePoint(x, y) toView:nil];
}

void Post(NSEventType type, int x, int y, int button_number) {
    NSEvent *e = [NSEvent mouseEventWithType:type
                                    location:WindowPoint(x, y)
                               modifierFlags:0
                                   timestamp:[NSProcessInfo processInfo].systemUptime
                                windowNumber:g_window.windowNumber
                                     context:nil
                                 eventNumber:0
                                  clickCount:1
                                    pressure:1.0];
    if (button_number > 0) {
        // mouseEventWithType has no button number of its own: a CGEvent
        // copy carries it (only otherMouse* reads it).
        CGEventRef cg = CGEventCreateCopy(e.CGEvent);
        CGEventSetIntegerValueField(cg, kCGMouseEventButtonNumber, button_number);
        e = [NSEvent eventWithCGEvent:cg];
        CFRelease(cg);
    }
    [NSApp postEvent:e atStart:NO];
}

}  // namespace

void Init(void *native_window_handle) {
    const auto *handle = static_cast<const gfx::NativeWindowHandle *>(native_window_handle);
    g_window = handle ? (__bridge NSWindow *)handle->display : nil;
}

bool Available() { return g_window != nil; }

void MouseMove(int x, int y) {
    if (!g_window) return;
    NSEventType type = NSEventTypeMouseMoved;
    if (g_buttons_down & 1) type = NSEventTypeLeftMouseDragged;
    else if (g_buttons_down & 4) type = NSEventTypeRightMouseDragged;
    else if (g_buttons_down & 2) type = NSEventTypeOtherMouseDragged;
    Post(type, x, y, 0);
}

// `button`: X11's numbering, which the socket's methods speak (1 left,
// 2 middle, 3 right).
void MouseButton(int x, int y, int button, bool down) {
    if (!g_window) return;
    MouseMove(x, y);
    NSEventType type;
    switch (button) {
        case 1: type = down ? NSEventTypeLeftMouseDown : NSEventTypeLeftMouseUp; break;
        case 2: type = down ? NSEventTypeOtherMouseDown : NSEventTypeOtherMouseUp; break;
        case 3: type = down ? NSEventTypeRightMouseDown : NSEventTypeRightMouseUp; break;
        default: return;
    }
    const int bit = 1 << (button - 1);
    g_buttons_down = down ? (g_buttons_down | bit) : (g_buttons_down & ~bit);
    Post(type, x, y, button == 2 ? 2 : 0);
}

void Scroll(int, int, int) {}
bool KeyEvent(const std::string &, bool) { return false; }
bool TypeChar(char) { return false; }

}  // namespace mep::agent_ui

#endif  // __APPLE__
