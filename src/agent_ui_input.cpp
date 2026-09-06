#include "agent_ui_input.h"

#if !defined(__EMSCRIPTEN__) && !defined(_WIN32) && !defined(__APPLE__)
#define MEP_AGENT_UI_X11 1
#endif

#if defined(MEP_AGENT_UI_X11)

#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <X11/extensions/XTest.h>

#include <cctype>
#include <cstdlib>
#include <unordered_map>

namespace mep::agent_ui {
namespace {

Display *g_display = nullptr;
Window g_window = 0;
bool g_available = false;

// X11 keysym *names* (XStringToKeysym) for printable-ASCII punctuation
// that isn't its own name -- letters/digits ARE their own keysym name
// ("a", "A", "5"), so this table only needs the rest of 0x20-0x7e.
// Doubles as TypeChar's "does this need Shift" answer: every entry here
// that's the shifted glyph of a base key (e.g. '!' over '1') is listed
// with shift_needed=true; unshifted punctuation (e.g. '-', '.', '/')
// isn't, matching a standard US QWERTY layout -- see this file's own
// top-of-function comment on TypeChar for the layout-assumption caveat.
struct KeyEntry {
    const char *name;
    bool shift_needed;
};
const std::unordered_map<char, KeyEntry> &PunctuationTable() {
    static const std::unordered_map<char, KeyEntry> table = {
        {' ', {"space", false}},        {'!', {"exclam", true}},        {'"', {"quotedbl", true}},
        {'#', {"numbersign", true}},    {'$', {"dollar", true}},        {'%', {"percent", true}},
        {'&', {"ampersand", true}},     {'\'', {"apostrophe", false}},  {'(', {"parenleft", true}},
        {')', {"parenright", true}},    {'*', {"asterisk", true}},      {'+', {"plus", true}},
        {',', {"comma", false}},        {'-', {"minus", false}},        {'.', {"period", false}},
        {'/', {"slash", false}},        {':', {"colon", true}},         {';', {"semicolon", false}},
        {'<', {"less", true}},          {'=', {"equal", false}},        {'>', {"greater", true}},
        {'?', {"question", true}},      {'@', {"at", true}},            {'[', {"bracketleft", false}},
        {'\\', {"backslash", false}},   {']', {"bracketright", false}}, {'^', {"asciicircum", true}},
        {'_', {"underscore", true}},    {'`', {"grave", false}},        {'{', {"braceleft", true}},
        {'|', {"bar", true}},           {'}', {"braceright", true}},    {'~', {"asciitilde", true}},
    };
    return table;
}

void WindowOrigin(int &out_x, int &out_y) {
    Window child = 0;
    XTranslateCoordinates(g_display, g_window, DefaultRootWindow(g_display), 0, 0, &out_x, &out_y, &child);
}

}  // namespace

void Init(void *glfw_window_handle) {
    auto *window = reinterpret_cast<GLFWwindow *>(glfw_window_handle);
    if (!window) return;
    g_display = glfwGetX11Display();
    g_window = glfwGetX11Window(window);
    if (!g_display || !g_window) return;
    int major = 0, minor = 0;
    g_available = XTestQueryExtension(g_display, &major, &minor, &major, &minor);
}

bool Available() { return g_available; }

void MouseMove(int x, int y) {
    if (!g_available) return;
    int ox = 0, oy = 0;
    WindowOrigin(ox, oy);
    XTestFakeMotionEvent(g_display, -1, ox + x, oy + y, CurrentTime);
    XFlush(g_display);
}

void MouseButton(int x, int y, int button, bool down) {
    if (!g_available) return;
    MouseMove(x, y);
    XTestFakeButtonEvent(g_display, static_cast<unsigned int>(button), down, CurrentTime);
    XFlush(g_display);
}

void Scroll(int x, int y, int ticks) {
    if (!g_available || ticks == 0) return;
    MouseMove(x, y);
    const unsigned int button = ticks > 0 ? 4 : 5;  // X11 legacy wheel buttons: 4=up, 5=down
    for (int i = 0; i < std::abs(ticks); i++) {
        XTestFakeButtonEvent(g_display, button, True, CurrentTime);
        XTestFakeButtonEvent(g_display, button, False, CurrentTime);
    }
    XFlush(g_display);
}

bool KeyEvent(const std::string &key, bool down) {
    if (!g_available || key.empty()) return false;
    KeySym sym = NoSymbol;
    if (key.size() == 1) {
        unsigned char c = static_cast<unsigned char>(key[0]);
        if (std::isalnum(c)) {
            sym = XStringToKeysym(key.c_str());
        } else {
            auto it = PunctuationTable().find(key[0]);
            if (it != PunctuationTable().end()) sym = XStringToKeysym(it->second.name);
        }
    } else {
        sym = XStringToKeysym(key.c_str());  // named key: "Escape", "Return", "Control_L", "F5", ...
    }
    if (sym == NoSymbol) return false;
    KeyCode code = XKeysymToKeycode(g_display, sym);
    if (code == 0) return false;
    XTestFakeKeyEvent(g_display, code, down, CurrentTime);
    XFlush(g_display);
    return true;
}

bool TypeChar(char c) {
    if (c < 0x20 || c > 0x7e) return false;  // printable ASCII only -- see header comment
    bool shift_needed = false;
    if (std::isupper(static_cast<unsigned char>(c))) {
        shift_needed = true;
    } else if (!std::isalnum(static_cast<unsigned char>(c))) {
        auto it = PunctuationTable().find(c);
        if (it == PunctuationTable().end()) return false;
        shift_needed = it->second.shift_needed;
    }
    std::string key(1, c);
    if (shift_needed) KeyEvent("Shift_L", true);
    bool ok = KeyEvent(key, true) && KeyEvent(key, false);
    if (shift_needed) KeyEvent("Shift_L", false);
    return ok;
}

}  // namespace mep::agent_ui

#else  // !MEP_AGENT_UI_X11

namespace mep::agent_ui {
void Init(void *) {}
bool Available() { return false; }
void MouseMove(int, int) {}
void MouseButton(int, int, int, bool) {}
void Scroll(int, int, int) {}
bool KeyEvent(const std::string &, bool) { return false; }
bool TypeChar(char) { return false; }
}  // namespace mep::agent_ui

#endif  // MEP_AGENT_UI_X11
