#ifndef MEP_AGENT_UI_INPUT_H
#define MEP_AGENT_UI_INPUT_H

#include <string>

// Synthetic mouse/keyboard input for the agent-control socket's "ui.*"
// RPC methods (main.cpp registers them via agent_rpc's RegisterUiMethod)
// -- lets an external agent (an MCP client driving this MCP__mep-agent
// server) click/drag/type into mep's own real window the same way
// Playwright drives a browser, by injecting real X11 events via the
// XTest extension at the window's actual screen position.
//
// Deliberately its own translation unit, NOT included from main.cpp
// alongside raylib.h: X11's Xlib.h typedefs `Font` (as `XID`), which
// collides with raylib's own `Font` struct typedef the moment both
// headers land in one translation unit. Keeping every Xlib/GLFW-native
// include inside agent_ui_input.cpp (and out of this header) means
// main.cpp only ever sees the plain, X11-free declarations below.
//
// Linux/X11 only (guarded by the same NOT EMSCRIPTEN/NOT WIN32/NOT APPLE
// condition as CMakeLists.txt's link step) -- Available() reports false
// everywhere else, and every function below is then a safe no-op.
namespace mep::agent_ui {

// Call once, after InitWindow() (so GLFW has a live window) and before
// registering any "ui.*" RPC method -- `glfw_window_handle` is raylib's
// own GetWindowHandle() (a GLFWwindow*, passed as void* so this header
// stays GLFW-free). Resolves and caches the window's X11 Display/Window
// and confirms the XTest extension is present; Available() reflects
// whether that succeeded.
void Init(void *glfw_window_handle);

// True once Init() has resolved a usable X11 Display/Window and XTest is
// present. Every function below is a no-op (mouse/key: return false)
// when this is false, so callers need no #ifdef/availability check of
// their own beyond surfacing that to the RPC caller as an error.
bool Available();

// Moves the pointer to (x, y) in mep's own window-client coordinates --
// the same space raylib's GetMousePosition()/RegisterClickRegion
// rectangles use -- by translating to absolute screen coordinates via
// the window's current on-screen origin (re-read every call, so this
// tracks the window if it's been moved).
void MouseMove(int x, int y);

// Presses or releases a mouse button at (x, y) (moves the pointer there
// first). `button` is an X11 button number: 1=left, 2=middle, 3=right.
void MouseButton(int x, int y, int button, bool down);

// Scrolls at (x, y): `ticks` wheel clicks, positive = up, negative =
// down, injected as legacy X11 button-4/5 click pairs (what GLFW's X11
// backend already listens for -- see this module's .cpp for why that's
// the reliable choice over XInput2 smooth-scroll here).
void Scroll(int x, int y, int ticks);

// Presses or releases one key by name: a single ASCII character ("a",
// "A", "5", "!" -- resolved via a small punctuation-name table plus
// XStringToKeysym) or an X11 keysym name for anything else ("Escape",
// "Return", "Tab", "BackSpace", "Left"/"Right"/"Up"/"Down", "Control_L",
// "Shift_L", "Alt_L", "Super_L", "F1".."F12", ...). Returns false (no
// event sent) if the name doesn't resolve to a keysym with a keycode on
// the current keyboard mapping, or if Available() is false.
bool KeyEvent(const std::string &key, bool down);

// Types one character as a complete keystroke (down then up), holding
// Shift around it first if the character needs it on a US keyboard
// layout (the layout this sandbox's X server is assumed to use -- see
// this module's .cpp). Returns false if the character has no ASCII
// keysym mapping (only printable ASCII, 0x20-0x7e, is supported) or if
// Available() is false.
bool TypeChar(char c);

}  // namespace mep::agent_ui

#endif  // MEP_AGENT_UI_INPUT_H
