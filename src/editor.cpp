#include "editor.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "raylib.h"
#include "lua_env.h"
#include "json.h"
#include "persist.h"
#include "job.h"
#include "vterm.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>

// Bridges to launcher/serve.ts's loopback-only HTTP server, which gives
// this (otherwise sandboxed) wasm build real file access via the Deno
// process hosting the webview window -- but only there; opening mep.html
// directly in a plain browser tab has no bridge port in its URL (see
// web/shell.html), so window.__mepBridgeBase is null and these calls fail
// with an ordinary read/write error rather than a crash.
//
// This talks HTTP rather than going through webview.bind() (the more
// obvious choice): bind()'s reply is delivered back into the page by the
// native webview library calling back into WebKit's JS engine, and that
// delivery reliably never arrived in testing -- the native
// Deno.readTextFile/writeTextFile call underneath a bind()'d function
// completed for real (the file was actually read/written), but the page's
// `await` on the bind() call's promise hung forever, reproduced even with
// a plain non-wasm <script> calling the bound function directly, so it
// wasn't a wasm bug. fetch()'s promise resolution goes through WebKit's
// ordinary networking stack instead, sidestepping whatever's broken in
// that other path.
//
// These use a *synchronous* XMLHttpRequest rather than fetch()+await, so
// they're plain EM_JS, not EM_ASYNC_JS -- deliberately, not just style.
// Emscripten's Asyncify (needed to make an `await` inside EM_ASYNC_JS look
// like an ordinary blocking call to the C++ caller) instruments the
// *entire* reachable call graph, not just these functions -- and with the
// Lua interpreter loop reachable from here (mep.read_file() and friends
// are Lua-callable), that meant every mep.on_frame Lua callback, called
// 60x/sec from UpdateDrawFrame regardless of whether any file I/O was
// happening at all, ran through Asyncify's instrumentation too. Measured
// directly: an otherwise-idle session with nothing but that per-frame Lua
// polling leaked from ~900MB to 10GB+ resident in under two minutes,
// eventually hitting WebKit's own memory-pressure kill of the WebProcess
// -- see main.cpp's now-removed frame-hook throttle comment (git history)
// for the isolating measurements. A synchronous XHR blocks the JS main
// thread for the (sub-millisecond, loopback-only) duration of the
// request, which looks identical to the C++ caller as a plain blocking
// call, but needs none of Asyncify's stack-rewinding machinery -- so
// Asyncify is no longer linked at all (CMakeLists.txt), and this whole
// class of leak doesn't exist to begin with. All still return a malloc'd C
// string the caller must free(): "OK\n<content>" / "OK" on success, or
// "ERR\n<message>" on failure -- same contract as before.
EM_JS(char *, mep_js_read_file, (const char *path_ptr), {
    const path = UTF8ToString(path_ptr);
    let text;
    try {
        if (!window.__mepBridgeBase) throw new Error("no file bridge (not launched via `just run`)");
        const xhr = new XMLHttpRequest();
        xhr.open("GET", window.__mepBridgeBase + "/read?path=" + encodeURIComponent(path), false);
        xhr.send();
        const result = JSON.parse(xhr.responseText);
        text = result.ok ? ("OK\n" + result.content) : ("ERR\n" + result.error);
    } catch (e) {
        text = "ERR\n" + String(e);
    }
    const len = lengthBytesUTF8(text) + 1;
    const ptr = _malloc(len);
    stringToUTF8(text, ptr, len);
    return ptr;
});

EM_JS(char *, mep_js_write_file, (const char *path_ptr, const char *content_ptr), {
    const path = UTF8ToString(path_ptr);
    const content = UTF8ToString(content_ptr);
    let text;
    try {
        if (!window.__mepBridgeBase) throw new Error("no file bridge (not launched via `just run`)");
        const xhr = new XMLHttpRequest();
        xhr.open("POST", window.__mepBridgeBase + "/write", false);
        xhr.setRequestHeader("Content-Type", "application/json");
        xhr.send(JSON.stringify({ path, content }));
        const result = JSON.parse(xhr.responseText);
        text = result.ok ? "OK" : ("ERR\n" + result.error);
    } catch (e) {
        text = "ERR\n" + String(e);
    }
    const len = lengthBytesUTF8(text) + 1;
    const ptr = _malloc(len);
    stringToUTF8(text, ptr, len);
    return ptr;
});

// Same bridge, for listing a directory (command-line path completion --
// see UpdateCmdlineCompletion below). Result on success is "OK\n" followed
// by a JSON array of {name, is_dir} objects (parsed with json.h on the C++
// side) rather than a plain listing, since filenames can contain the
// newlines/tabs a hand-rolled text format would need to escape.
EM_JS(char *, mep_js_list_dir, (const char *path_ptr), {
    const path = UTF8ToString(path_ptr);
    let text;
    try {
        if (!window.__mepBridgeBase) throw new Error("no file bridge (not launched via `just run`)");
        const xhr = new XMLHttpRequest();
        xhr.open("GET", window.__mepBridgeBase + "/list?path=" + encodeURIComponent(path), false);
        xhr.send();
        const result = JSON.parse(xhr.responseText);
        text = result.ok ? ("OK\n" + JSON.stringify(result.entries)) : ("ERR\n" + result.error);
    } catch (e) {
        text = "ERR\n" + String(e);
    }
    const len = lengthBytesUTF8(text) + 1;
    const ptr = _malloc(len);
    stringToUTF8(text, ptr, len);
    return ptr;
});

// Same bridge, for the persisted project-bookmark list (mep.projects()
// picker -- see ListProjects/AddProject/RemoveProject below). Deno resolves
// and owns the actual $XDG_DATA_HOME/mep/projects.json path/read/write
// server-side (launcher/serve.ts's /projects endpoint) since the wasm
// sandbox has no env vars of its own to compute it from. Result on success
// is "OK\n" followed by a JSON array of path strings.
EM_JS(char *, mep_js_project_list, (), {
    let text;
    try {
        if (!window.__mepBridgeBase) throw new Error("no file bridge (not launched via `just run`)");
        const xhr = new XMLHttpRequest();
        xhr.open("GET", window.__mepBridgeBase + "/projects", false);
        xhr.send();
        const result = JSON.parse(xhr.responseText);
        text = result.ok ? ("OK\n" + JSON.stringify(result.projects)) : ("ERR\n" + result.error);
    } catch (e) {
        text = "ERR\n" + String(e);
    }
    const len = lengthBytesUTF8(text) + 1;
    const ptr = _malloc(len);
    stringToUTF8(text, ptr, len);
    return ptr;
});

// action: "add" or "remove". Same result shape as mep_js_project_list --
// the server returns the list post-mutation so the caller doesn't need a
// second round trip to refresh it.
EM_JS(char *, mep_js_project_mutate, (const char *action_ptr, const char *path_ptr), {
    const action = UTF8ToString(action_ptr);
    const path = UTF8ToString(path_ptr);
    let text;
    try {
        if (!window.__mepBridgeBase) throw new Error("no file bridge (not launched via `just run`)");
        const xhr = new XMLHttpRequest();
        xhr.open("POST", window.__mepBridgeBase + "/projects", false);
        xhr.setRequestHeader("Content-Type", "application/json");
        xhr.send(JSON.stringify({ action, path }));
        const result = JSON.parse(xhr.responseText);
        text = result.ok ? ("OK\n" + JSON.stringify(result.projects)) : ("ERR\n" + result.error);
    } catch (e) {
        text = "ERR\n" + String(e);
    }
    const len = lengthBytesUTF8(text) + 1;
    const ptr = _malloc(len);
    stringToUTF8(text, ptr, len);
    return ptr;
});

// `:terminal`/`:term` under wasm (see TerminalSpawn's wasm branch,
// Editor::TerminalWrite/TerminalResizeBackend/TerminalKillBackend,
// Editor::PollTerminals below): the wasm sandbox has no subprocess/PTY
// concept at all, so this tunnels through launcher/serve.ts's `/pty`
// WebSocket endpoint, which spawns the real process server-side. Unlike
// this file's other bridge calls (request/response HTTP, now synchronous
// XHR -- see the comment above mep_js_read_file for why), a WebSocket is
// long-lived and event-driven and can't be made synchronous the same way,
// so connecting is split into a start call plus a polled status call
// rather than one round trip:
//   - mep_js_pty_connect_start: opens the connection and sends the spawn
//     request, but returns the slot id immediately without waiting for
//     the server's ready/spawn_error response.
//   - mep_js_pty_connect_status: polled from Editor::PollTerminals (once/
//     frame, already-existing poll loop -- TerminalSession::connecting)
//     until it reports ready or failed, mirroring how mep_js_pty_poll
//     below already drains output without blocking. This used to be one
//     EM_ASYNC_JS call that awaited a Promise across the open/ready round
//     trip -- the only async bridge call in this file -- but that meant
//     it was also the only thing requiring Asyncify to be linked at all,
//     and Asyncify's whole-program instrumentation is what caused the
//     leak described above mep_js_read_file. Splitting this one call
//     into start+poll let every EM_ASYNC_JS in this file go away.
//   - mep_js_pty_write/_resize: fire-and-forget sends (already fine pre-
//     connect: state.ws.readyState !== OPEN just no-ops until ready).
//   - mep_js_pty_poll + mep_js_pty_poll_len: drains whatever raw output
//     bytes have arrived since the last poll. Two calls instead of one
//     because PTY output is an arbitrary byte stream (may contain
//     embedded NULs) -- unlike every other bridge call here, it can't be
//     handed back as a null-terminated C string without risking silent
//     truncation, and EM_JS can only return one value.
//   - mep_js_pty_exited/_exit_code: whether the process has ended.
EM_JS(int, mep_js_pty_connect_start, (const char *payload_json_ptr), {
    const payloadJson = UTF8ToString(payload_json_ptr);
    if (!window.__mepBridgeBase) {
        window.__mepLastPtyError = "no file bridge (not launched via `just run`)";
        return -1;
    }
    const wsBase = window.__mepBridgeBase.replace(/^http/, "ws");
    let ws;
    try {
        ws = new WebSocket(wsBase + "/pty");
    } catch (e) {
        window.__mepLastPtyError = String(e);
        return -1;
    }
    ws.binaryType = "arraybuffer";
    if (!window.__mepPtys) {
        window.__mepPtys = {};
        window.__mepPtyNextId = 1;
    }
    const id = window.__mepPtyNextId++;
    // connectStatus: 0 = still connecting, 1 = ready, -1 = failed --
    // mep_js_pty_connect_status() below reads this each frame.
    const state = { ws, queue: [], exited: false, exitCode: -1, connectStatus: 0 };
    window.__mepPtys[id] = state;
    ws.onopen = () => {
        ws.send(payloadJson);
    };
    ws.onmessage = (ev) => {
        if (typeof ev.data === "string") {
            try {
                const msg = JSON.parse(ev.data);
                if (msg.type === "ready") {
                    if (state.connectStatus === 0) state.connectStatus = 1;
                } else if (msg.type === "spawn_error") {
                    if (state.connectStatus === 0) {
                        state.connectStatus = -1;
                        window.__mepLastPtyError = msg.error;
                    }
                } else if (msg.type === "exit") {
                    state.exited = true;
                    state.exitCode = msg.code;
                }
            } catch (e) {
                // malformed control message -- ignore
            }
        } else {
            state.queue.push(new Uint8Array(ev.data));
        }
    };
    ws.onerror = () => {
        if (state.connectStatus === 0) {
            state.connectStatus = -1;
            window.__mepLastPtyError = "WebSocket connection to the `/pty` bridge failed";
        }
    };
    ws.onclose = () => {
        state.exited = true;
    };
    return id;
});

EM_JS(int, mep_js_pty_connect_status, (int id), {
    const state = window.__mepPtys && window.__mepPtys[id];
    if (!state) return -1;
    if (state.connectStatus === -1) delete window.__mepPtys[id];
    return state.connectStatus;
});

// Set by mep_js_pty_connect whenever it resolves to -1 -- read once, right
// after, for a status message more useful than a generic "failed."
EM_JS(char *, mep_js_pty_last_error, (), {
    const text = window.__mepLastPtyError || "unknown error";
    const len = lengthBytesUTF8(text) + 1;
    const ptr = _malloc(len);
    stringToUTF8(text, ptr, len);
    return ptr;
});

EM_JS(void, mep_js_pty_write, (int id, const char *bytes_ptr, int len), {
    const state = window.__mepPtys && window.__mepPtys[id];
    if (!state || state.ws.readyState !== WebSocket.OPEN) return;
    state.ws.send(HEAPU8.slice(bytes_ptr, bytes_ptr + len));
});

EM_JS(void, mep_js_pty_resize, (int id, int cols, int rows), {
    const state = window.__mepPtys && window.__mepPtys[id];
    if (!state || state.ws.readyState !== WebSocket.OPEN) return;
    state.ws.send(JSON.stringify({ type: "resize", cols, rows }));
});

EM_JS(void, mep_js_pty_close, (int id), {
    const state = window.__mepPtys && window.__mepPtys[id];
    if (!state) return;
    try {
        state.ws.close();
    } catch (e) {
        // already closed
    }
    delete window.__mepPtys[id];
});

// Returns a malloc'd raw-byte buffer (NOT a C string) or 0 if nothing has
// arrived since the last poll; byte count is fetched right after via
// mep_js_pty_poll_len(), before any other mep_js_pty_* call.
EM_JS(char *, mep_js_pty_poll, (int id), {
    const state = window.__mepPtys && window.__mepPtys[id];
    if (!state || state.queue.length === 0) {
        window.__mepLastPtyPollLen = 0;
        return 0;
    }
    let total = 0;
    for (const chunk of state.queue) total += chunk.length;
    const merged = new Uint8Array(total);
    let off = 0;
    for (const chunk of state.queue) {
        merged.set(chunk, off);
        off += chunk.length;
    }
    state.queue = [];
    const ptr = _malloc(total);
    HEAPU8.set(merged, ptr);
    window.__mepLastPtyPollLen = total;
    return ptr;
});

EM_JS(int, mep_js_pty_poll_len, (), { return window.__mepLastPtyPollLen || 0; });

EM_JS(int, mep_js_pty_exited, (int id), {
    const state = window.__mepPtys && window.__mepPtys[id];
    return !state || state.exited ? 1 : 0;
});

EM_JS(int, mep_js_pty_exit_code, (int id), {
    const state = window.__mepPtys && window.__mepPtys[id];
    return state ? state.exitCode : -1;
});
#endif

namespace {

// Vim's word-class model: a "word" motion (w/b/e) stops at the boundary
// between any two of these three classes, so "foo.bar" is three words
// ("foo", ".", "bar") -- unlike a WORD motion (W/B/E), which only cares
// about whitespace vs. non-whitespace.
enum class CharClass { Space, Word, Punct };

CharClass ClassOf(char c) {
    if (std::isspace(static_cast<unsigned char>(c))) return CharClass::Space;
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') return CharClass::Word;
    return CharClass::Punct;
}

// Splits file content into lines the same way std::getline does: a
// trailing newline does not produce a spurious empty final line, and a
// trailing '\r' (CRLF) is stripped from each line.
std::vector<std::string> SplitIntoLines(const std::string &content) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < content.size()) {
        size_t nl = content.find('\n', start);
        if (nl == std::string::npos) {
            lines.push_back(content.substr(start));
            break;
        }
        std::string line = content.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        start = nl + 1;
    }
    if (lines.empty()) lines.push_back("");
    return lines;
}

}  // namespace

// --- Theme engine (NVIM_PARITY_PLAN.md Part II Phase 9) ------------------
namespace {

ThemeColor Clamp255(int r, int g, int b, int a = 255) {
    auto c = [](int v) { return static_cast<unsigned char>(std::max(0, std::min(255, v))); };
    return ThemeColor{c(r), c(g), c(b), c(a)};
}
ThemeColor Lighten(ThemeColor c, int amount) { return Clamp255(c.r + amount, c.g + amount, c.b + amount, c.a); }
ThemeColor Darken(ThemeColor c, int amount) { return Lighten(c, -amount); }
ThemeColor Mix(ThemeColor a, ThemeColor b, float t) {
    return Clamp255(static_cast<int>(a.r + (b.r - a.r) * t), static_cast<int>(a.g + (b.g - a.g) * t),
                     static_cast<int>(a.b + (b.b - a.b) * t));
}

// mep's original hardcoded look (main.cpp's pre-Phase-9 literals), kept as
// the default palette so nothing visually changes for existing users.
const Palette kPaletteMepDark = {
    "mep-dark",
    /*bg*/ {30, 30, 30, 255}, /*fg*/ {211, 211, 211, 255}, /*red*/ {224, 108, 117, 255},
    /*green*/ {152, 195, 121, 255}, /*yellow*/ {229, 192, 123, 255}, /*blue*/ {97, 175, 239, 255},
    /*purple*/ {198, 120, 221, 255}, /*cyan*/ {86, 182, 194, 255}, /*orange*/ {209, 154, 102, 255},
    /*border*/ {90, 90, 95, 255},
};
const Palette kPaletteGruvboxDark = {
    "gruvbox-dark",
    {40, 40, 40, 255}, {235, 219, 178, 255}, {251, 73, 82, 255}, {184, 187, 38, 255}, {250, 189, 47, 255},
    {131, 165, 152, 255}, {211, 134, 155, 255}, {142, 192, 124, 255}, {254, 128, 25, 255}, {146, 131, 116, 255},
};
const Palette kPaletteNord = {
    "nord",
    {46, 52, 64, 255}, {216, 222, 233, 255}, {191, 97, 106, 255}, {163, 190, 140, 255}, {235, 203, 139, 255},
    {129, 161, 193, 255}, {180, 142, 173, 255}, {136, 192, 208, 255}, {208, 135, 112, 255}, {76, 86, 106, 255},
};
const Palette kPaletteGruvboxLight = {
    "gruvbox-light",
    {251, 241, 199, 255}, {60, 56, 54, 255}, {204, 36, 29, 255}, {152, 151, 26, 255}, {215, 153, 33, 255},
    {69, 133, 136, 255}, {177, 98, 134, 255}, {104, 157, 106, 255}, {214, 93, 14, 255}, {168, 153, 132, 255},
};

const Palette *FindPalette(const std::string &name) {
    static const Palette *kAll[] = {&kPaletteMepDark, &kPaletteGruvboxDark, &kPaletteNord, &kPaletteGruvboxLight};
    for (const Palette *p : kAll) {
        if (p->name == name) return p;
    }
    return nullptr;
}

// Expands a compact 10-color palette into every named highlight group main.cpp's
// chrome rendering and ResolveHlGroup's decoration-color fallback target --
// mirrors mep.nvim's palette-to-highlight-group renderer, scoped to the
// groups mep actually has consumers for today (grows as more land).
std::unordered_map<std::string, ThemeColor> BuildHighlightGroups(const Palette &p) {
    std::unordered_map<std::string, ThemeColor> g;
    // Base roles -- also what ResolveHlGroup's substring fallback reads
    // for arbitrary un-migrated hl_group names (e.g. "MepGitAdd").
    g["Normal"] = p.fg;
    g["NormalBg"] = p.bg;
    g["Red"] = p.red;
    g["Green"] = p.green;
    g["Yellow"] = p.yellow;
    g["Blue"] = p.blue;
    g["Purple"] = p.purple;
    g["Cyan"] = p.cyan;
    g["Orange"] = p.orange;
    g["Border"] = p.border;
    // Diagnostics/git/notify, matching the exact names those consumers use.
    g["Error"] = p.red;
    g["Warn"] = p.yellow;
    g["Info"] = p.blue;
    g["Hint"] = p.blue;
    g["Debug"] = p.border;
    g["Add"] = p.green;
    g["Delete"] = p.red;
    g["Change"] = p.yellow;
    g["Comment"] = p.border;
    // Chrome.
    g["StatusLine"] = Lighten(p.bg, 15);
    g["StatusLineFg"] = p.fg;
    g["MenuBar"] = Lighten(p.bg, 10);
    g["MenuBarFg"] = p.fg;
    g["MenuHighlight"] = Mix(p.blue, p.bg, 0.55f);
    g["TabBar"] = Darken(p.bg, 5);
    g["TabActive"] = Mix(p.blue, p.bg, 0.55f);
    g["TabInactive"] = Lighten(p.bg, 5);
    g["BorderActive"] = Mix(p.blue, p.fg, 0.3f);
    g["BorderInactive"] = p.border;
    g["CursorLine"] = Lighten(p.bg, 8);
    g["Visual"] = Mix(p.blue, p.bg, 0.35f);
    g["LineNr"] = p.border;
    g["FloatBg"] = Lighten(p.bg, 5);
    g["FloatBorder"] = p.border;
    g["Overlay"] = ThemeColor{0, 0, 0, 140};
    g["Sidebar"] = Lighten(p.bg, 5);
    g["SidebarBorder"] = p.border;
    g["SidebarTitle"] = Mix(p.blue, p.fg, 0.4f);
    g["Picker"] = Lighten(p.bg, 5);
    g["PickerBorder"] = p.border;
    g["PickerTitle"] = Mix(p.blue, p.fg, 0.4f);
    g["PickerSelected"] = Mix(p.blue, p.bg, 0.45f);
    return g;
}

}  // namespace

bool Editor::ApplyTheme(const std::string &name) {
    const Palette *p = FindPalette(name);
    if (!p) return false;
    current_theme_name_ = name;
    current_theme_groups_ = BuildHighlightGroups(*p);
    return true;
}

std::vector<std::string> Editor::ThemeNames() const {
    return {kPaletteMepDark.name, kPaletteGruvboxDark.name, kPaletteNord.name, kPaletteGruvboxLight.name};
}

bool Editor::ResolveHighlight(const std::string &name, ThemeColor *out) const {
    auto it = current_theme_groups_.find(name);
    if (it == current_theme_groups_.end()) return false;
    *out = it->second;
    return true;
}

Editor::Editor() {
    buffers_.push_back(Buffer{});
    Tab tab;
    tab.root = std::make_unique<SplitNode>();
    tab.root->dir = SplitDir::Leaf;
    tab.root->pane.id = next_pane_id_++;
    tab.root->pane.buffer_id = 0;
    tab.active_pane_id = tab.root->pane.id;
    tabs_.push_back(std::move(tab));
    ApplyTheme("mep-dark");
}

Editor::~Editor() = default;

void Editor::HandleInput() {
    if (HandleMod1Shortcuts()) return;
    switch (mode_) {
        case Mode::Normal:
            HandleNormalInput();
            break;
        case Mode::Insert:
            HandleInsertInput();
            break;
        case Mode::Visual:
        case Mode::VisualLine:
        case Mode::VisualBlock:
            HandleVisualInput();
            break;
        case Mode::Command:
            HandleCommandInput();
            break;
        case Mode::SearchForward:
        case Mode::SearchBackward:
            HandleSearchInput();
            break;
        case Mode::Prompt:
            HandlePromptInput();
            break;
        case Mode::Confirm:
            HandleConfirmInput();
            break;
        case Mode::Select:
            HandleSelectInput();
            break;
        case Mode::Sidebar:
            HandleSidebarInput();
            break;
        case Mode::Picker:
            HandlePickerInput();
            break;
        case Mode::WhichKey:
            HandleWhichKeyInput();
            break;
        case Mode::HintChar:
            HandleHintCharInput();
            break;
        case Mode::HintLabel:
            HandleHintLabelInput();
            break;
        case Mode::Terminal:
            HandleTerminalInput();
            break;
    }
}

void Editor::UpdateScrollForPane(int pane_id, int visible_lines) {
    SplitNode *node = FindNode(tabs_[active_tab_].root.get(), pane_id);
    if (!node) return;
    Pane &pane = node->pane;
    pane.visible_lines = std::max(1, visible_lines);
    if (pane.cursor.row < pane.scroll_row) {
        pane.scroll_row = pane.cursor.row;
    } else if (pane.cursor.row >= pane.scroll_row + visible_lines) {
        pane.scroll_row = pane.cursor.row - visible_lines + 1;
    }
    if (pane.scroll_row < 0) pane.scroll_row = 0;
}

// Ctrl-D/Ctrl-U/Ctrl-F/Ctrl-B and zz/zt/zb all read `visible_lines` as of
// last frame's render (the same field H/M/L already rely on in
// ResolveMotion) rather than anything computed fresh here -- window size
// essentially never changes frame-to-frame, so that's not a real gap.
void Editor::ScrollHalfPage(bool down) {
    Pane &p = CurPane();
    int delta = std::max(1, p.visible_lines / 2) * (down ? 1 : -1);
    int max_row = Buf().LineCount() - 1;
    p.cursor.row = std::max(0, std::min(p.cursor.row + delta, max_row));
    p.scroll_row = std::max(0, std::min(p.scroll_row + delta, max_row));
    ClampCursor();
}

void Editor::ScrollFullPage(bool down) {
    Pane &p = CurPane();
    int delta = p.visible_lines * (down ? 1 : -1);
    int max_row = Buf().LineCount() - 1;
    p.cursor.row = std::max(0, std::min(p.cursor.row + delta, max_row));
    p.scroll_row = std::max(0, std::min(p.scroll_row + delta, max_row));
    ClampCursor();
}

void Editor::ScrollCursorTo(char where) {
    Pane &p = CurPane();
    if (where == 'z') {
        p.scroll_row = p.cursor.row - p.visible_lines / 2;
    } else if (where == 't') {
        p.scroll_row = p.cursor.row;
    } else if (where == 'b') {
        p.scroll_row = p.cursor.row - p.visible_lines + 1;
    }
    p.scroll_row = std::max(0, std::min(p.scroll_row, std::max(0, Buf().LineCount() - 1)));
}

void Editor::IncrementNumberAtCursor(long long delta) {
    CursorPos &cursor = CurPane().cursor;
    std::string &line = Buf().lines[cursor.row];
    int len = static_cast<int>(line.size());
    int start = cursor.col;
    while (start < len && !std::isdigit(static_cast<unsigned char>(line[start]))) start++;
    if (start >= len) return;  // no number from the cursor onward on this line
    int a = start;
    while (a > 0 && std::isdigit(static_cast<unsigned char>(line[a - 1]))) a--;
    int b = start;
    while (b < len && std::isdigit(static_cast<unsigned char>(line[b]))) b++;
    bool negative = (a > 0 && line[a - 1] == '-');
    int sign_pos = negative ? a - 1 : a;
    int width = b - a;
    bool had_leading_zero = (width > 1 && line[a] == '0');

    long long value = 0;
    for (int i = a; i < b; i++) value = value * 10 + (line[i] - '0');
    if (negative) value = -value;
    value += delta;

    std::string digits = std::to_string(value < 0 ? -value : value);
    if (had_leading_zero && static_cast<int>(digits.size()) < width) {
        digits = std::string(width - digits.size(), '0') + digits;
    }
    std::string replacement = (value < 0 ? "-" : "") + digits;

    PushUndo();
    line.replace(sign_pos, b - sign_pos, replacement);
    Buf().modified = true;
    cursor.col = sign_pos + static_cast<int>(replacement.size()) - 1;
    ClampCursor();
}

void Editor::VisualRange(CursorPos &start, CursorPos &end) const {
    start = CurPane().visual_anchor;
    end = CurPane().cursor;
    if (start.row > end.row || (start.row == end.row && start.col > end.col)) {
        std::swap(start, end);
    }
}

void Editor::VisualBlockRange(int &top, int &bottom, int &left, int &right) const {
    const CursorPos &a = CurPane().visual_anchor;
    const CursorPos &c = CurPane().cursor;
    top = std::min(a.row, c.row);
    bottom = std::max(a.row, c.row);
    left = std::min(a.col, c.col);
    right = block_to_eol_ ? -1 : std::max(a.col, c.col);
}

int Editor::LineLen(int row) const {
    if (row < 0 || row >= Buf().LineCount()) return 0;
    return static_cast<int>(Buf().lines[row].size());
}

void Editor::ClampCursor() {
    CursorPos &cursor = CurPane().cursor;
    cursor.row = std::max(0, std::min(cursor.row, Buf().LineCount() - 1));
    // A closed fold hides every row after its own start row -- the cursor
    // can never rest inside one (matches Vim: moving into a closed fold
    // lands you on its first line). Cheap enough to check unconditionally
    // since ClampCursor already runs after every motion.
    int fold_start = 0;
    if (IsRowHiddenByFold(cursor.row, &fold_start)) cursor.row = fold_start;
    int len = LineLen(cursor.row);
    int max_col = (mode_ == Mode::Insert || mode_ == Mode::Command) ? len : std::max(0, len - 1);
    cursor.col = std::max(0, std::min(cursor.col, max_col));
}

// --- Buffer/pane/tab plumbing ----------------------------------------------

Pane &Editor::CurPane() {
    Tab &tab = tabs_[active_tab_];
    SplitNode *node = FindNode(tab.root.get(), tab.active_pane_id);
    return node->pane;
}

const Pane &Editor::CurPane() const {
    const Tab &tab = tabs_[active_tab_];
    SplitNode *node = FindNode(tab.root.get(), tab.active_pane_id);
    return node->pane;
}

SplitNode *Editor::FindNode(SplitNode *node, int pane_id) const {
    if (node->dir == SplitDir::Leaf) return (node->pane.id == pane_id) ? node : nullptr;
    for (auto &child : node->children) {
        if (SplitNode *found = FindNode(child.get(), pane_id)) return found;
    }
    return nullptr;
}

void Editor::CollectLeaves(const SplitNode *node, std::vector<int> &ids) const {
    if (node->dir == SplitDir::Leaf) {
        ids.push_back(node->pane.id);
        return;
    }
    for (auto &child : node->children) CollectLeaves(child.get(), ids);
}

void Editor::CollectBufferIds(const SplitNode *node, std::vector<int> &ids) const {
    if (node->dir == SplitDir::Leaf) {
        ids.push_back(node->pane.buffer_id);
        return;
    }
    for (auto &child : node->children) CollectBufferIds(child.get(), ids);
}

void Editor::ReapOrphanedTerminals() {
    std::vector<int> live_buffer_ids;
    for (const Tab &t : tabs_) CollectBufferIds(t.root.get(), live_buffer_ids);
    for (auto it = terminals_.begin(); it != terminals_.end();) {
        if (std::find(live_buffer_ids.begin(), live_buffer_ids.end(), it->first) == live_buffer_ids.end()) {
            if (!it->second.exited) TerminalKillBackend(it->second);
            it = terminals_.erase(it);
        } else {
            ++it;
        }
    }
}

bool Editor::RemovePaneNode(std::unique_ptr<SplitNode> &node_ptr, int pane_id) {
    SplitNode *node = node_ptr.get();
    for (size_t i = 0; i < node->children.size(); i++) {
        SplitNode *child = node->children[i].get();
        if (child->dir == SplitDir::Leaf && child->pane.id == pane_id) {
            node->children.erase(node->children.begin() + i);
            if (node->children.size() == 1) {
                node_ptr = std::move(node->children[0]);
            }
            return true;
        }
        if (child->dir != SplitDir::Leaf && RemovePaneNode(node->children[i], pane_id)) {
            return true;
        }
    }
    return false;
}

int Editor::CreateEmptyBuffer() {
    buffers_.push_back(Buffer{});
    return static_cast<int>(buffers_.size()) - 1;
}

// --- Dashboard/scratch/zen (NVIM_PARITY_PLAN.md Part III Phase 12) -------

bool Editor::ShouldShowDashboard() const {
    if (tabs_.size() != 1 || buffers_.size() != 1) return false;
    const SplitNode *root = tabs_[0].root.get();
    if (!root || root->dir != SplitDir::Leaf) return false;
    const Buffer &buf = buffers_[0];
    return !buf.modified && buf.filename.empty() && !buf.scratch && buf.lines.size() == 1 && buf.lines[0].empty();
}

void Editor::OpenScratchBuffer() {
    for (size_t i = 0; i < buffers_.size(); i++) {
        if (buffers_[i].scratch) {
            CurPane().buffer_id = static_cast<int>(i);
            ClampCursor();
            return;
        }
    }
    int id = CreateEmptyBuffer();
    buffers_[id].scratch = true;
    CurPane().buffer_id = id;
    ClampCursor();
}

int Editor::FindOrCreateBuffer(const std::string &path, bool *existed) {
    for (size_t i = 0; i < buffers_.size(); i++) {
        if (!buffers_[i].filename.empty() && buffers_[i].filename == path) {
            if (existed) *existed = true;
            return static_cast<int>(i);
        }
    }

    Buffer buf;
    buf.filename = path;

#if defined(__EMSCRIPTEN__)
    char *result = mep_js_read_file(path.c_str());
    std::string res(result);
    std::free(result);
    if (res.rfind("OK\n", 0) == 0) {
        if (existed) *existed = true;
        buf.lines = SplitIntoLines(res.substr(3));
    } else {
        // Not found (or no Deno bridge -- e.g. mep.html opened directly in
        // a plain browser tab): start a new empty buffer named `path`,
        // same as Vim treats a nonexistent path. Genuine write failures
        // still surface later, at :w.
        if (existed) *existed = false;
    }
#else
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        // No such file (or unreadable): start a new empty buffer named
        // `path`, same as Vim -- this is how you create a file, not an
        // error. Genuine write failures still surface later, at :w.
        if (existed) *existed = false;
        buffers_.push_back(std::move(buf));
        return static_cast<int>(buffers_.size()) - 1;
    }
    if (existed) *existed = true;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    buf.lines = SplitIntoLines(content);
#endif

    buffers_.push_back(std::move(buf));
    return static_cast<int>(buffers_.size()) - 1;
}

void Editor::SplitCurrentPane(SplitDir dir, const std::string &file_arg) {
    Tab &tab = tabs_[active_tab_];
    SplitNode *active = FindNode(tab.root.get(), tab.active_pane_id);
    if (!active) return;

    int new_buffer_id = active->pane.buffer_id;
    if (!file_arg.empty()) {
        int id = FindOrCreateBuffer(file_arg);
        if (id < 0) return;
        new_buffer_id = id;
    }

    Pane original_pane = active->pane;

    Pane new_pane;
    new_pane.id = next_pane_id_++;
    new_pane.buffer_id = new_buffer_id;
    new_pane.cursor = file_arg.empty() ? original_pane.cursor : CursorPos{0, 0};
    new_pane.scroll_row = file_arg.empty() ? original_pane.scroll_row : 0;

    auto original_leaf = std::make_unique<SplitNode>();
    original_leaf->dir = SplitDir::Leaf;
    original_leaf->pane = original_pane;

    auto new_leaf = std::make_unique<SplitNode>();
    new_leaf->dir = SplitDir::Leaf;
    new_leaf->pane = new_pane;

    // vim opens the new pane above/left of the old one and focuses it.
    active->dir = dir;
    active->pane = Pane{};
    active->children.clear();
    active->children.push_back(std::move(new_leaf));
    active->children.push_back(std::move(original_leaf));

    tab.active_pane_id = new_pane.id;
}

// --- Terminal panes (`:terminal`/`:term`, Part VI Phase 27+) -------------

void Editor::OpenTerminal(const std::string &args) {
    SplitCurrentPane(SplitDir::Horizontal, "");
    OpenTerminalInPlace(args);
}

void Editor::OpenTerminalInPlace(const std::string &args) {
    Tab &tab = tabs_[active_tab_];
    SplitNode *node = FindNode(tab.root.get(), tab.active_pane_id);
    if (!node) return;

    int buffer_id = CreateEmptyBuffer();
    node->pane.buffer_id = buffer_id;
    node->pane.cursor = CursorPos{0, 0};
    node->pane.scroll_row = 0;
    node->pane.buffer_tabs = {buffer_id};
    node->pane.buffer_tab_index = 0;

    const char *shell_env = std::getenv("SHELL");
    std::string shell = (shell_env && *shell_env) ? shell_env : "/bin/sh";
    std::vector<std::string> argv = args.empty() ? std::vector<std::string>{shell}
                                                  : std::vector<std::string>{shell, "-c", args};

    TerminalSession sess;
    sess.buffer_id = buffer_id;
    sess.title = args.empty() ? shell : args;
    // 24x80 is only a placeholder -- DrawPane calls ResizeTerminal with
    // the real pane's character-cell size on the very first frame it's
    // drawn, before any output can have arrived to be misjudged against
    // the wrong size.
    sess.vterm = std::make_unique<VTerm>(24, 80);
    TerminalSpawn(sess, argv);

    terminals_[buffer_id] = std::move(sess);
    mode_ = Mode::Terminal;
}

void Editor::TerminalSpawn(TerminalSession &sess, const std::vector<std::string> &argv) {
#if defined(__EMSCRIPTEN__)
    std::vector<std::string> remote_argv = argv;
    // A bare interactive shell (no `:terminal <cmd>` argument) has no
    // real tty to auto-detect interactivity from over this transport
    // (see the class-level comment above mep_js_pty_connect) -- `-i`
    // forces prompt/readline-ish behavior anyway, the best approximation
    // available without a genuine PTY.
    if (remote_argv.size() == 1) remote_argv.push_back("-i");
    Json payload = Json::Object();
    Json argv_json = Json::Array();
    for (const std::string &a : remote_argv) argv_json.push_back(Json(a));
    payload["cmd"] = argv_json;
    sess.job_id = mep_js_pty_connect_start(payload.dump().c_str());
    if (sess.job_id <= 0) {
        sess.exited = true;
        sess.exit_code = -1;
        char *err = mep_js_pty_last_error();
        status_message_ = std::string("Failed to start terminal: ") + err;
        std::free(err);
    } else {
        sess.connecting = true;
    }
#else
    VTerm *vterm_ptr = sess.vterm.get();
    int buffer_id = sess.buffer_id;
    JobManager::Callbacks cb;
    cb.on_stdout_raw = [vterm_ptr](const std::string &chunk) { vterm_ptr->Feed(chunk); };
    cb.on_exit = [this, buffer_id](int code) {
        TerminalSession *s = FindTerminal(buffer_id);
        if (s) {
            s->exited = true;
            s->exit_code = code;
        }
    };
    sess.job_id = JobManager::Instance().Spawn(argv, "", std::move(cb), /*use_pty=*/true);
    if (sess.job_id == 0) status_message_ = "Failed to start terminal";
#endif
}

void Editor::TerminalWrite(TerminalSession &sess, const std::string &bytes) {
    if (sess.job_id <= 0) return;
#if defined(__EMSCRIPTEN__)
    mep_js_pty_write(sess.job_id, bytes.data(), static_cast<int>(bytes.size()));
#else
    JobManager::Instance().WriteStdin(sess.job_id, bytes);
#endif
}

void Editor::TerminalResizeBackend(TerminalSession &sess, int cols, int rows) {
    if (sess.job_id <= 0) return;
#if defined(__EMSCRIPTEN__)
    mep_js_pty_resize(sess.job_id, cols, rows);
#else
    JobManager::Instance().ResizePty(sess.job_id, cols, rows);
#endif
}

void Editor::TerminalKillBackend(TerminalSession &sess) {
    if (sess.job_id <= 0) return;
#if defined(__EMSCRIPTEN__)
    mep_js_pty_close(sess.job_id);
#else
    JobManager::Instance().Kill(sess.job_id);
#endif
}

void Editor::PollTerminals() {
#if defined(__EMSCRIPTEN__)
    for (auto &kv : terminals_) {
        TerminalSession &sess = kv.second;
        if (sess.job_id <= 0 || sess.exited) continue;
        if (sess.connecting) {
            int status = mep_js_pty_connect_status(sess.job_id);
            if (status == 0) continue;  // still connecting -- try again next frame
            sess.connecting = false;
            if (status < 0) {
                sess.exited = true;
                sess.exit_code = -1;
                char *err = mep_js_pty_last_error();
                status_message_ = std::string("Failed to start terminal: ") + err;
                std::free(err);
                continue;
            }
        }
        char *ptr = mep_js_pty_poll(sess.job_id);
        int len = mep_js_pty_poll_len();
        if (ptr) {
            if (len > 0 && sess.vterm) sess.vterm->Feed(std::string(ptr, len));
            std::free(ptr);
        }
        if (mep_js_pty_exited(sess.job_id)) {
            sess.exited = true;
            sess.exit_code = mep_js_pty_exit_code(sess.job_id);
        }
    }
#endif
    // Native builds get output/exit updates via JobManager's own
    // callback-driven PollAll() instead (see TerminalSpawn's native
    // branch) -- nothing to do here for them.
}

TerminalSession *Editor::FindTerminal(int buffer_id) {
    auto it = terminals_.find(buffer_id);
    return it == terminals_.end() ? nullptr : &it->second;
}

bool Editor::IsTerminalBuffer(int buffer_id) const {
    return terminals_.find(buffer_id) != terminals_.end();
}

const TerminalSession *Editor::GetTerminal(int buffer_id) const {
    auto it = terminals_.find(buffer_id);
    return it == terminals_.end() ? nullptr : &it->second;
}

void Editor::ResizeTerminal(int buffer_id, int rows, int cols) {
    auto it = terminals_.find(buffer_id);
    if (it == terminals_.end()) return;
    TerminalSession &sess = it->second;
    if (sess.last_rows == rows && sess.last_cols == cols) return;
    sess.last_rows = rows;
    sess.last_cols = cols;
    if (sess.vterm) sess.vterm->Resize(rows, cols);
    if (!sess.exited) TerminalResizeBackend(sess, cols, rows);
}

void Editor::HandleTerminalInput() {
    TerminalSession *sess = FindTerminal(CurPane().buffer_id);
    if (!sess) {
        mode_ = Mode::Normal;
        return;
    }

    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

    // Keys this loop recognizes and forwards -- everything else (plain
    // letters/digits/symbols with no modifier) is deliberately left alone
    // here and picked up by the GetCharPressed() loop below instead,
    // mirroring HandleInsertInput's own split between the two. Ctrl+A-Z
    // is handled separately (as a control code) rather than through this
    // list.
    static const int kForwardedKeys[] = {
        KEY_ESCAPE,   KEY_ENTER,     KEY_KP_ENTER, KEY_BACKSPACE, KEY_TAB,      KEY_UP,
        KEY_DOWN,     KEY_LEFT,      KEY_RIGHT,    KEY_HOME,      KEY_END,      KEY_PAGE_UP,
        KEY_PAGE_DOWN, KEY_DELETE,   KEY_INSERT,
    };

    for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed()) {
        if (terminal_pending_ctrl_bs_) {
            terminal_pending_ctrl_bs_ = false;
            if (key == KEY_N && ctrl) {
                mode_ = Mode::Normal;
                return;
            }
            // Not the exit chord after all -- the buffered Ctrl-\ (FS,
            // 0x1C) really was meant for the child (e.g. SIGQUIT), so
            // send it now before handling this key.
            if (!sess->exited) TerminalWrite(*sess, std::string(1, '\x1C'));
        }
        if (key == KEY_BACKSLASH && ctrl) {
            terminal_pending_ctrl_bs_ = true;
            continue;
        }
        if (shift && (key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) && sess->vterm) {
            int rows = sess->vterm->Rows();
            if (key == KEY_PAGE_UP) {
                sess->scroll_offset = std::min(sess->scroll_offset + rows, sess->vterm->ScrollbackLines());
            } else {
                sess->scroll_offset = std::max(0, sess->scroll_offset - rows);
            }
            continue;
        }
        bool is_forwarded = false;
        for (int k : kForwardedKeys) {
            if (k == key) {
                is_forwarded = true;
                break;
            }
        }
        if (!is_forwarded && !(ctrl && key >= KEY_A && key <= KEY_Z)) continue;
        sess->scroll_offset = 0;
        if (!sess->exited) SendTerminalKey(*sess, key, 0, ctrl);
    }

    if (!sess->exited) {
        // Auto-repeat for the keys most likely to be held down.
        if (IsKeyPressedRepeat(KEY_BACKSPACE)) SendTerminalKey(*sess, KEY_BACKSPACE, 0, false);
        if (IsKeyPressedRepeat(KEY_UP)) SendTerminalKey(*sess, KEY_UP, 0, false);
        if (IsKeyPressedRepeat(KEY_DOWN)) SendTerminalKey(*sess, KEY_DOWN, 0, false);
        if (IsKeyPressedRepeat(KEY_LEFT)) SendTerminalKey(*sess, KEY_LEFT, 0, false);
        if (IsKeyPressedRepeat(KEY_RIGHT)) SendTerminalKey(*sess, KEY_RIGHT, 0, false);
    }

    int cp = GetCharPressed();
    while (cp > 0) {
        sess->scroll_offset = 0;
        if (!sess->exited) SendTerminalKey(*sess, 0, cp, false);
        cp = GetCharPressed();
    }
}

void Editor::SendTerminalKey(TerminalSession &sess, int key, int codepoint, bool ctrl) {
    std::string bytes;
    bool app_mode = sess.vterm && sess.vterm->ApplicationCursorKeys();
    if (codepoint > 0) {
        if (codepoint < 0x80) {
            bytes = std::string(1, static_cast<char>(codepoint));
        } else if (codepoint < 0x800) {
            bytes += static_cast<char>(0xC0 | (codepoint >> 6));
            bytes += static_cast<char>(0x80 | (codepoint & 0x3F));
        } else if (codepoint < 0x10000) {
            bytes += static_cast<char>(0xE0 | (codepoint >> 12));
            bytes += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            bytes += static_cast<char>(0x80 | (codepoint & 0x3F));
        } else {
            bytes += static_cast<char>(0xF0 | (codepoint >> 18));
            bytes += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
            bytes += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            bytes += static_cast<char>(0x80 | (codepoint & 0x3F));
        }
    } else if (ctrl && key >= KEY_A && key <= KEY_Z) {
        bytes = std::string(1, static_cast<char>(1 + (key - KEY_A)));
    } else {
        switch (key) {
            case KEY_ESCAPE:
                bytes = "\x1b";
                break;
            case KEY_ENTER:
            case KEY_KP_ENTER:
                bytes = "\r";
                break;
            case KEY_BACKSPACE:
                bytes = "\x7f";
                break;
            case KEY_TAB:
                bytes = "\t";
                break;
            case KEY_UP:
                bytes = app_mode ? "\x1bOA" : "\x1b[A";
                break;
            case KEY_DOWN:
                bytes = app_mode ? "\x1bOB" : "\x1b[B";
                break;
            case KEY_RIGHT:
                bytes = app_mode ? "\x1bOC" : "\x1b[C";
                break;
            case KEY_LEFT:
                bytes = app_mode ? "\x1bOD" : "\x1b[D";
                break;
            case KEY_HOME:
                bytes = app_mode ? "\x1bOH" : "\x1b[H";
                break;
            case KEY_END:
                bytes = app_mode ? "\x1bOF" : "\x1b[F";
                break;
            case KEY_PAGE_UP:
                bytes = "\x1b[5~";
                break;
            case KEY_PAGE_DOWN:
                bytes = "\x1b[6~";
                break;
            case KEY_DELETE:
                bytes = "\x1b[3~";
                break;
            case KEY_INSERT:
                bytes = "\x1b[2~";
                break;
            default:
                return;
        }
    }
    TerminalWrite(sess, bytes);
}

void Editor::ClosePane() {
    Tab &tab = tabs_[active_tab_];
    if (tab.root->dir == SplitDir::Leaf) {
        // Only one pane in this tab: closing it closes the tab.
        if (tabs_.size() > 1) {
            TabDelete();
        } else {
            status_message_ = "E444: Cannot close last window";
        }
        return;
    }

    RemovePaneNode(tab.root, tab.active_pane_id);

    std::vector<int> ids;
    CollectLeaves(tab.root.get(), ids);
    if (!ids.empty()) tab.active_pane_id = ids.front();
    ReapOrphanedTerminals();
}

void Editor::CyclePane(int delta) {
    Tab &tab = tabs_[active_tab_];
    std::vector<int> ids;
    CollectLeaves(tab.root.get(), ids);
    if (ids.size() <= 1) return;
    auto it = std::find(ids.begin(), ids.end(), tab.active_pane_id);
    if (it == ids.end()) return;
    int idx = static_cast<int>(it - ids.begin());
    int n = static_cast<int>(ids.size());
    idx = ((idx + delta) % n + n) % n;
    tab.active_pane_id = ids[idx];
}

// --- Window tiling-manager layer (NVIM_PARITY_PLAN.md Part III Phase 14) --

void Editor::EnsureBufferTabSeeded(Pane &p) const {
    bool has_current = std::find(p.buffer_tabs.begin(), p.buffer_tabs.end(), p.buffer_id) != p.buffer_tabs.end();
    if (!has_current) {
        p.buffer_tabs = {p.buffer_id};
        p.buffer_tab_index = 0;
    }
}

void Editor::PaneOpenBufferInTab(const std::string &path) {
    int buffer_id = FindOrCreateBuffer(path, nullptr);
    if (buffer_id < 0) return;
    Pane &p = CurPane();
    EnsureBufferTabSeeded(p);
    p.buffer_tabs.insert(p.buffer_tabs.begin() + p.buffer_tab_index + 1, buffer_id);
    p.buffer_tab_index++;
    p.buffer_id = buffer_id;
    ClampCursor();
}

void Editor::PaneNextBufferTab() {
    Pane &p = CurPane();
    EnsureBufferTabSeeded(p);
    if (p.buffer_tabs.size() <= 1) return;
    p.buffer_tab_index = (p.buffer_tab_index + 1) % static_cast<int>(p.buffer_tabs.size());
    p.buffer_id = p.buffer_tabs[p.buffer_tab_index];
    ClampCursor();
}

void Editor::PanePrevBufferTab() {
    Pane &p = CurPane();
    EnsureBufferTabSeeded(p);
    if (p.buffer_tabs.size() <= 1) return;
    int n = static_cast<int>(p.buffer_tabs.size());
    p.buffer_tab_index = (p.buffer_tab_index - 1 + n) % n;
    p.buffer_id = p.buffer_tabs[p.buffer_tab_index];
    ClampCursor();
}

void Editor::PaneCloseBufferTab() {
    Pane &p = CurPane();
    EnsureBufferTabSeeded(p);
    if (p.buffer_tabs.size() <= 1) {
        ClosePane();
        return;
    }
    p.buffer_tabs.erase(p.buffer_tabs.begin() + p.buffer_tab_index);
    if (p.buffer_tab_index >= static_cast<int>(p.buffer_tabs.size())) {
        p.buffer_tab_index = static_cast<int>(p.buffer_tabs.size()) - 1;
    }
    p.buffer_id = p.buffer_tabs[p.buffer_tab_index];
    ClampCursor();
}

void Editor::PaneMoveBufferTabToNeighbor(const std::string &direction) {
    Tab &tab = tabs_[active_tab_];
    int neighbor_id = FindNeighborPaneId(tab.active_pane_id, direction);
    if (neighbor_id < 0) return;
    SplitNode *neighbor_node = FindNode(tab.root.get(), neighbor_id);
    if (!neighbor_node) return;

    Pane &src = CurPane();
    EnsureBufferTabSeeded(src);
    int moved_buffer_id = src.buffer_id;
    bool was_last_tab = src.buffer_tabs.size() <= 1;
    src.buffer_tabs.erase(src.buffer_tabs.begin() + src.buffer_tab_index);
    if (!src.buffer_tabs.empty()) {
        if (src.buffer_tab_index >= static_cast<int>(src.buffer_tabs.size())) {
            src.buffer_tab_index = static_cast<int>(src.buffer_tabs.size()) - 1;
        }
        src.buffer_id = src.buffer_tabs[src.buffer_tab_index];
        ClampCursor();
    }

    Pane &dst = neighbor_node->pane;
    EnsureBufferTabSeeded(dst);
    dst.buffer_tabs.insert(dst.buffer_tabs.begin() + dst.buffer_tab_index + 1, moved_buffer_id);
    dst.buffer_tab_index++;
    dst.buffer_id = moved_buffer_id;

    if (was_last_tab) ClosePane();
}

std::unique_ptr<SplitNode> Editor::BuildSpiralLayout(std::vector<Pane> panes, bool horizontal_next) const {
    auto node = std::make_unique<SplitNode>();
    if (panes.size() == 1) {
        node->dir = SplitDir::Leaf;
        node->pane = panes[0];
        return node;
    }
    node->dir = horizontal_next ? SplitDir::Horizontal : SplitDir::Vertical;
    auto first = std::make_unique<SplitNode>();
    first->dir = SplitDir::Leaf;
    first->pane = panes.front();
    std::vector<Pane> rest(panes.begin() + 1, panes.end());
    node->children.push_back(std::move(first));
    node->children.push_back(BuildSpiralLayout(std::move(rest), !horizontal_next));
    return node;
}

void Editor::ApplyLayout(const std::string &kind) {
    Tab &tab = tabs_[active_tab_];
    std::vector<int> ids;
    CollectLeaves(tab.root.get(), ids);
    if (ids.size() <= 1) return;

    std::vector<Pane> panes;
    panes.reserve(ids.size());
    for (int id : ids) {
        SplitNode *n = FindNode(tab.root.get(), id);
        if (n) panes.push_back(n->pane);
    }

    auto make_leaf = [](const Pane &p) {
        auto n = std::make_unique<SplitNode>();
        n->dir = SplitDir::Leaf;
        n->pane = p;
        return n;
    };
    auto make_stack = [&](SplitDir dir, std::vector<Pane> items) {
        auto n = std::make_unique<SplitNode>();
        n->dir = dir;
        for (const Pane &p : items) n->children.push_back(make_leaf(p));
        return n;
    };

    std::unique_ptr<SplitNode> new_root;
    if (kind == "master-left" || kind == "master-right" || kind == "master-top" || kind == "master-bottom") {
        Pane master = panes.front();
        std::vector<Pane> rest(panes.begin() + 1, panes.end());
        bool side = (kind == "master-left" || kind == "master-right");
        bool master_first = (kind == "master-left" || kind == "master-top");
        new_root = std::make_unique<SplitNode>();
        new_root->dir = side ? SplitDir::Vertical : SplitDir::Horizontal;
        auto master_leaf = make_leaf(master);
        auto stack = make_stack(side ? SplitDir::Horizontal : SplitDir::Vertical, rest);
        if (master_first) {
            new_root->children.push_back(std::move(master_leaf));
            new_root->children.push_back(std::move(stack));
        } else {
            new_root->children.push_back(std::move(stack));
            new_root->children.push_back(std::move(master_leaf));
        }
    } else if (kind == "grid-h") {
        new_root = make_stack(SplitDir::Horizontal, panes);
    } else if (kind == "grid-v") {
        new_root = make_stack(SplitDir::Vertical, panes);
    } else if (kind == "grid") {
        int n = static_cast<int>(panes.size());
        int rows = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
        new_root = std::make_unique<SplitNode>();
        new_root->dir = SplitDir::Horizontal;
        int idx = 0;
        while (idx < n) {
            int remaining_rows = rows - static_cast<int>(new_root->children.size());
            int cols = static_cast<int>(std::ceil(static_cast<double>(n - idx) / std::max(1, remaining_rows)));
            std::vector<Pane> row(panes.begin() + idx, panes.begin() + std::min(idx + cols, n));
            new_root->children.push_back(row.size() == 1 ? make_leaf(row[0]) : make_stack(SplitDir::Vertical, row));
            idx += cols;
        }
    } else if (kind == "spiral") {
        new_root = BuildSpiralLayout(panes, true);
    } else {
        return;  // unknown layout
    }

    tab.root = std::move(new_root);
    tab.active_pane_id = panes.front().id;
}

void Editor::TabNew(const std::string &file_arg) {
    int buffer_id = file_arg.empty() ? CreateEmptyBuffer() : FindOrCreateBuffer(file_arg);
    if (buffer_id < 0) return;

    Tab tab;
    tab.root = std::make_unique<SplitNode>();
    tab.root->dir = SplitDir::Leaf;
    tab.root->pane.id = next_pane_id_++;
    tab.root->pane.buffer_id = buffer_id;
    tab.active_pane_id = tab.root->pane.id;

    tabs_.insert(tabs_.begin() + active_tab_ + 1, std::move(tab));
    active_tab_++;
}

void Editor::TabDelete() {
    if (tabs_.size() <= 1) {
        status_message_ = "E784: Cannot close last tab page";
        return;
    }
    tabs_.erase(tabs_.begin() + active_tab_);
    if (active_tab_ >= static_cast<int>(tabs_.size())) active_tab_ = static_cast<int>(tabs_.size()) - 1;
    ReapOrphanedTerminals();
}

void Editor::TabNext() {
    if (tabs_.size() <= 1) return;
    active_tab_ = (active_tab_ + 1) % static_cast<int>(tabs_.size());
}

void Editor::TabPrevious() {
    if (tabs_.size() <= 1) return;
    int n = static_cast<int>(tabs_.size());
    active_tab_ = (active_tab_ - 1 + n) % n;
}

std::string Editor::TabLabel(int tab_index) const {
    const Tab &tab = tabs_[tab_index];
    SplitNode *node = FindNode(tab.root.get(), tab.active_pane_id);
    if (!node) return "[No Name]";
    const Buffer &buf = buffers_[node->pane.buffer_id];
    return buf.filename.empty() ? "[No Name]" : buf.filename;
}

void Editor::ComputeRects(const SplitNode *node, float x0, float y0, float x1, float y1,
                           std::vector<PaneRect> &out) const {
    if (node->dir == SplitDir::Leaf) {
        out.push_back({node->pane.id, x0, y0, x1, y1});
        return;
    }
    int n = static_cast<int>(node->children.size());
    if (n == 0) return;
    bool has_shares = node->shares.size() == static_cast<size_t>(n);
    if (node->dir == SplitDir::Horizontal) {
        float y = y0;
        for (int i = 0; i < n; i++) {
            float h = has_shares ? (y1 - y0) * node->shares[i] : (y1 - y0) / n;
            float next_y = (i == n - 1) ? y1 : y + h;
            ComputeRects(node->children[i].get(), x0, y, x1, next_y, out);
            y = next_y;
        }
    } else {
        float x = x0;
        for (int i = 0; i < n; i++) {
            float w = has_shares ? (x1 - x0) * node->shares[i] : (x1 - x0) / n;
            float next_x = (i == n - 1) ? x1 : x + w;
            ComputeRects(node->children[i].get(), x, y0, next_x, y1, out);
            x = next_x;
        }
    }
}

int Editor::FindNeighborPaneId(int from_pane_id, const std::string &direction) const {
    const Tab &tab = tabs_[active_tab_];
    std::vector<PaneRect> rects;
    ComputeRects(tab.root.get(), 0.0f, 0.0f, 1.0f, 1.0f, rects);
    if (rects.size() <= 1) return -1;

    const PaneRect *cur = nullptr;
    for (const auto &r : rects) {
        if (r.pane_id == from_pane_id) cur = &r;
    }
    if (!cur) return -1;

    constexpr float kEps = 0.001f;
    const PaneRect *best = nullptr;
    float best_overlap = 0, best_distance = 0;

    for (const auto &r : rects) {
        if (r.pane_id == cur->pane_id) continue;
        bool candidate_ok = false;
        float overlap = 0, distance = 0;
        if (direction == "left") {
            candidate_ok = r.x1 <= cur->x0 + kEps;
            overlap = std::min(r.y1, cur->y1) - std::max(r.y0, cur->y0);
            distance = cur->x0 - r.x1;
        } else if (direction == "right") {
            candidate_ok = r.x0 >= cur->x1 - kEps;
            overlap = std::min(r.y1, cur->y1) - std::max(r.y0, cur->y0);
            distance = r.x0 - cur->x1;
        } else if (direction == "up") {
            candidate_ok = r.y1 <= cur->y0 + kEps;
            overlap = std::min(r.x1, cur->x1) - std::max(r.x0, cur->x0);
            distance = cur->y0 - r.y1;
        } else if (direction == "down") {
            candidate_ok = r.y0 >= cur->y1 - kEps;
            overlap = std::min(r.x1, cur->x1) - std::max(r.x0, cur->x0);
            distance = r.y0 - cur->y1;
        } else {
            return -1;  // unknown direction
        }
        if (!candidate_ok || overlap <= kEps) continue;

        bool better = !best || (overlap > best_overlap + kEps) ||
                      (std::fabs(overlap - best_overlap) <= kEps && distance < best_distance);
        if (better) {
            best = &r;
            best_overlap = overlap;
            best_distance = distance;
        }
    }
    return best ? best->pane_id : -1;
}

void Editor::NavigatePaneDirection(const std::string &direction) {
    if (mode_ == Mode::Sidebar) {
        const SidebarInstance *sb = FindSidebar(focused_sidebar_id_);
        if (!sb) return;
        bool into_panes = (sb->position == "left" && direction == "right") ||
                           (sb->position == "right" && direction == "left");
        if (into_panes) RestoreFromOverlay();
        return;
    }

    Tab &tab = tabs_[active_tab_];
    int id = FindNeighborPaneId(tab.active_pane_id, direction);
    if (id >= 0) {
        tab.active_pane_id = id;
        // mod1+hjkl (this function's only caller) is checked globally
        // before mode dispatch, so it can fire while Mode::Terminal is
        // mid-forwarding-keystrokes-to-a-child-process -- unlike every
        // other mode, leaving stray keys typed afterward to leak into
        // whatever pane focus lands on next would be a real bug (they'd
        // go to a live shell/program the user no longer meant to be
        // typing into), not just a UX quirk. Drop back to Normal the
        // moment focus actually moves to a different pane.
        if (mode_ == Mode::Terminal) mode_ = Mode::Normal;
        return;
    }
    if (direction != "left" && direction != "right") return;
    // No neighbor pane that way -- step into an open sidebar docked on the
    // same edge instead, if there is one. Iterates in registration order
    // and keeps the last match: DrawSidebars stacks same-edge sidebars
    // outward-to-inward in that same order, so the last one is the one
    // physically adjacent to the pane content.
    int target_id = 0;
    for (const SidebarInstance &sb : sidebars_) {
        if (sb.open && sb.position == direction) target_id = sb.id;
    }
    if (target_id != 0) OpenSidebar(target_id, true);
}

bool Editor::FindPathToPane(SplitNode *node, int pane_id, std::vector<std::pair<SplitNode *, int>> &path) const {
    if (node->dir == SplitDir::Leaf) return node->pane.id == pane_id;
    for (size_t i = 0; i < node->children.size(); i++) {
        if (FindPathToPane(node->children[i].get(), pane_id, path)) {
            path.push_back({node, static_cast<int>(i)});
            return true;
        }
    }
    return false;
}

void Editor::EnsureShares(SplitNode *node) {
    if (node->shares.size() == node->children.size()) return;
    node->shares.assign(node->children.size(), 1.0f / static_cast<float>(node->children.size()));
}

void Editor::ResizeActivePane(const std::string &direction, float step) {
    // A focused sidebar isn't a node in the split tree, so it can't go
    // through FindPathToPane/EnsureShares below -- grow/shrink its fixed
    // cell size directly instead, only on the axis matching its own dock
    // edge (mirrors the pane-tree branch's "no-op on the wrong axis" when
    // a leaf is alone on that axis).
    if (mode_ == Mode::Sidebar) {
        SidebarInstance *sb = FindSidebarMut(focused_sidebar_id_);
        if (!sb) return;
        constexpr int kSidebarResizeStep = 4;
        constexpr int kSidebarMinSize = 10;
        int delta = 0;
        if (sb->position == "left") {
            if (direction == "right") delta = kSidebarResizeStep;
            else if (direction == "left") delta = -kSidebarResizeStep;
        } else if (sb->position == "right") {
            if (direction == "left") delta = kSidebarResizeStep;
            else if (direction == "right") delta = -kSidebarResizeStep;
        } else if (sb->position == "top") {
            if (direction == "down") delta = kSidebarResizeStep;
            else if (direction == "up") delta = -kSidebarResizeStep;
        } else if (sb->position == "bottom") {
            if (direction == "up") delta = kSidebarResizeStep;
            else if (direction == "down") delta = -kSidebarResizeStep;
        }
        sb->size = std::max(kSidebarMinSize, sb->size + delta);
        return;
    }

    if (step <= 0.0f) step = kDefaultResizeStep;
    bool wants_columns = (direction == "left" || direction == "right");
    bool wants_rows = (direction == "up" || direction == "down");
    if (!wants_columns && !wants_rows) return;
    SplitDir want_dir = wants_columns ? SplitDir::Vertical : SplitDir::Horizontal;

    Tab &tab = tabs_[active_tab_];
    std::vector<std::pair<SplitNode *, int>> path;  // leaf-to-root order (see FindPathToPane)
    if (!FindPathToPane(tab.root.get(), tab.active_pane_id, path)) return;

    for (auto &[node, child_index] : path) {
        if (node->dir != want_dir || node->children.size() < 2) continue;
        EnsureShares(node);
        int n = static_cast<int>(node->children.size());
        bool has_next = child_index + 1 < n;
        bool has_prev = child_index > 0;
        // "right"/"down" push the active child's far edge further that
        // way (growing it) when it has a next sibling to push into;
        // "left"/"up" retract that same edge (shrinking it). With no next
        // sibling, the roles flip against the previous one instead -- see
        // ResizeActivePane's header.
        bool towards_next = (direction == "right" || direction == "down");
        int other_index;
        bool grow;
        if (has_next) {
            other_index = child_index + 1;
            grow = towards_next;
        } else if (has_prev) {
            other_index = child_index - 1;
            grow = !towards_next;
        } else {
            return;  // alone on this axis
        }

        float delta = grow ? step : -step;
        delta = std::clamp(delta, kMinPaneShare - node->shares[child_index],
                            node->shares[other_index] - kMinPaneShare);
        node->shares[child_index] += delta;
        node->shares[other_index] -= delta;
        return;
    }
}

void Editor::SetActivePaneShare(float fraction) {
    fraction = std::clamp(fraction, kMinPaneShare, 1.0f - kMinPaneShare);
    Tab &tab = tabs_[active_tab_];
    std::vector<std::pair<SplitNode *, int>> path;  // leaf-to-root order (see FindPathToPane)
    if (!FindPathToPane(tab.root.get(), tab.active_pane_id, path) || path.empty()) return;
    auto &[node, child_index] = path[0];  // immediate parent
    if (node->children.size() != 2) return;
    EnsureShares(node);
    int other_index = 1 - child_index;
    node->shares[child_index] = fraction;
    node->shares[other_index] = 1.0f - fraction;
}

// --- Global (mod1) shortcuts ------------------------------------------------

bool Editor::IsMod1Down() const {
    switch (mod1_) {
        case ModKey::Alt: return IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
        case ModKey::Control: return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
        case ModKey::Shift: return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        case ModKey::Super: return IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
    }
    return false;
}

void Editor::SetMod1(const std::string &name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "alt") {
        mod1_ = ModKey::Alt;
    } else if (lower == "ctrl" || lower == "control") {
        mod1_ = ModKey::Control;
    } else if (lower == "shift") {
        mod1_ = ModKey::Shift;
    } else if (lower == "super" || lower == "cmd" || lower == "meta") {
        mod1_ = ModKey::Super;
    } else {
        status_message_ = "Unknown mod1 key: " + name;
    }
}

void Editor::RegisterMod1Mapping(const std::string &key, int lua_ref) { mod1_mappings_[key] = lua_ref; }

bool Editor::HandleMod1Shortcuts() {
    if (!IsMod1Down() || mod1_mappings_.empty()) return false;
    // A held Ctrl/Shift alongside mod1 (e.g. mod1+Shift+h for resize,
    // mod1+Ctrl+h for move) looks up the "C-"/"S-" prefixed key instead of
    // the bare one -- see RegisterMod1Mapping. Only checked when mod1
    // itself isn't that same key, since e.g. mod1=Shift already implies
    // Shift is down for every mod1 combo.
    bool extra_ctrl =
        mod1_ != ModKey::Control && (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL));
    bool extra_shift = mod1_ != ModKey::Shift && (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT));
    // mod1+d is globally bound to pane_close_buffer (see kDefaultMod1Bindings
    // in main.cpp), but while a sidebar is focused (file tree, git status,
    // etc.) that mapping would silently close whatever buffer sits behind
    // it -- so intercept it here and close the sidebar instead, mirroring
    // the generic q/Escape handling in HandleSidebarInput.
    if (mode_ == Mode::Sidebar && !extra_ctrl && !extra_shift && IsKeyPressed(KEY_D)) {
        int id = focused_sidebar_id_;
        RestoreFromOverlay();
        CloseSidebar(id);
        while (GetCharPressed() > 0) {
        }
        return true;
    }
    for (int key = KEY_A; key <= KEY_Z; key++) {
        if (!IsKeyPressed(key)) continue;
        std::string base(1, static_cast<char>('a' + (key - KEY_A)));
        std::string k = extra_ctrl ? ("C-" + base) : extra_shift ? ("S-" + base) : base;
        auto it = mod1_mappings_.find(k);
        if (it == mod1_mappings_.end() || !lua_) continue;
        lua_->CallRef(it->second);
        // The same physical combo may also have queued a char event (e.g.
        // Alt-as-compose on some layouts); drop it so it doesn't get typed
        // into whatever mode handles input next frame.
        while (GetCharPressed() > 0) {
        }
        return true;
    }
    // Tab isn't a letter key so it falls outside the A-Z scan above; handled
    // separately for mod1+Tab / mod1+Shift+Tab (pane buffer-tab cycling).
    if (IsKeyPressed(KEY_TAB)) {
        std::string k = extra_shift ? "S-Tab" : "Tab";
        auto it = mod1_mappings_.find(k);
        if (it != mod1_mappings_.end() && lua_) {
            lua_->CallRef(it->second);
            while (GetCharPressed() > 0) {
            }
            return true;
        }
    }
    return false;
}

// --- Normal mode -----------------------------------------------------------

void Editor::HandleNormalInput() {
    // GLFW/raylib doesn't emit a char event while Ctrl is held, so these
    // are checked separately from the GetCharPressed() loop below.
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    bool record_raw = recording_macro_ && !replaying_change_ && !replaying_macro_;
    if (IsKeyPressed(KEY_R) && ctrl) {
        if (record_raw) macro_recording_buffer_.push_back(kReplayCtrlR);
        Redo();
        return;
    }
    if (IsKeyPressed(KEY_W) && ctrl) {
        if (record_raw) macro_recording_buffer_.push_back(kReplayCtrlW);
        pending_ctrl_w_ = true;
        return;
    }
    // Ctrl-V/D/U/F/B/A/X: same GetKeyPressed()-queue reasoning as
    // HandleInsertInput's Ctrl-W/Ctrl-U fix (see its comment) --
    // IsKeyPressed(KEY_V) was visibly flaky under a slow (software-
    // rendered Xvfb) frame the same way those were, so every Ctrl-combo
    // added from Phase 9 onward reads off this queue instead. Ctrl-R/
    // Ctrl-W above predate that finding and were left as-is (out of scope
    // here), worth remembering if either is ever reported flaky too.
    bool ctrl_v = false, ctrl_d = false, ctrl_u = false, ctrl_f = false, ctrl_b = false, ctrl_a = false,
         ctrl_x = false;
    for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed()) {
        if (!ctrl) continue;
        if (key == KEY_V) ctrl_v = true;
        else if (key == KEY_D) ctrl_d = true;
        else if (key == KEY_U) ctrl_u = true;
        else if (key == KEY_F) ctrl_f = true;
        else if (key == KEY_B) ctrl_b = true;
        else if (key == KEY_A) ctrl_a = true;
        else if (key == KEY_X) ctrl_x = true;
    }
    if (ctrl_v) {
        EnterVisualBlock();
        return;
    }
    if (ctrl_d) {
        ScrollHalfPage(true);
        return;
    }
    if (ctrl_u) {
        ScrollHalfPage(false);
        return;
    }
    if (ctrl_f) {
        ScrollFullPage(true);
        return;
    }
    if (ctrl_b) {
        ScrollFullPage(false);
        return;
    }
    if (ctrl_a) {
        IncrementNumberAtCursor(std::max(1, TakeRawCount()));
        return;
    }
    if (ctrl_x) {
        IncrementNumberAtCursor(-std::max(1, TakeRawCount()));
        return;
    }
    if (IsKeyPressed(KEY_ESCAPE)) {
        // A bare Escape while recording only matters as a "cancel whatever
        // was pending" if something actually was -- an Escape with nothing
        // pending is Vim's harmless no-op, not worth a macro-replay entry.
        if (record_raw && IsMidNormalCommand()) macro_recording_buffer_.push_back(kReplayEscape);
        CancelPendingNormalState();
        return;
    }

    int cp = GetCharPressed();
    while (cp > 0) {
        bool consumed = false;
        // Digits (as a pending count) and a pending find/g-prefix always
        // win over a Lua mapping for the same key -- those states consume
        // the very next key literally, and counts are closer to syntax
        // than to a remappable command.
        bool is_count_digit = (cp >= '1' && cp <= '9') || (cp == '0' && pending_count_ != 0);
        bool no_pending_state = pending_op_ == 0 && !pending_g_ && !pending_ctrl_w_ && pending_find_ == 0 &&
                                 !is_count_digit && !awaiting_register_name_;
        // Leader key (Phase 11 whichkey) only actually takes over Normal
        // mode once at least one <leader>-sequence is registered -- an
        // unconfigured leader falls through to whatever cp already does
        // (e.g. space's ordinary cursor-right movement), so nothing steals
        // a key nobody asked it to.
        if (no_pending_state && cp == static_cast<int>(leader_key_) && !whichkey_bindings_.empty()) {
            TriggerWhichKey();
            consumed = true;
        } else if (no_pending_state && cp != '"' && cp <= 127) {
            consumed = TryLuaMapping(Mode::Normal, std::string(1, static_cast<char>(cp)));
        }
        if (consumed) {
            if (recording_macro_ && !replaying_change_ && !replaying_macro_) macro_recording_buffer_.push_back(cp);
        } else {
            ProcessNormalKey(cp);
        }
        if (mode_ != Mode::Normal) break;  // key switched modes mid-loop
        cp = GetCharPressed();
    }
}

int Editor::TakeRawCount() {
    int n = pending_count_;
    pending_count_ = 0;
    return n;
}

void Editor::TakeRegisterSpec(char *name, bool *append) {
    *name = pending_register_;
    *append = pending_register_append_;
    pending_register_ = 0;
    pending_register_append_ = false;
}

Register &Editor::RegisterFor(char name) { return registers_[name != 0 ? name : '"']; }

bool Editor::TryLuaMapping(Mode mode, const std::string &key) {
    if (!lua_) return false;
    std::unordered_map<std::string, int> *map = nullptr;
    if (mode == Mode::Normal) {
        map = &normal_mappings_;
    } else if (mode == Mode::Visual || mode == Mode::VisualLine || mode == Mode::VisualBlock) {
        map = &visual_mappings_;
    } else {
        return false;
    }
    auto it = map->find(key);
    if (it == map->end()) return false;
    lua_->CallRef(it->second);
    return true;
}

bool Editor::DispatchNormalKey(int cp) {
    if (cp <= 0 || cp > 127) return false;
    char c = static_cast<char>(cp);

    // A terminal pane viewed from Normal mode (after Ctrl-\ Ctrl-N --
    // HandleTerminalInput) has no real buffer content for the rest of
    // this function's operators/motions/counts to act on, so 'i'/'a'
    // (matching Insert mode's own entry keys, and Neovim's terminal
    // convention) is special-cased ahead of all of it to jump straight
    // back into keystroke forwarding, bypassing pending-count/register/
    // operator state entirely -- none of it means anything here.
    if ((c == 'i' || c == 'a') && IsTerminalBuffer(CurPane().buffer_id)) {
        mode_ = Mode::Terminal;
        return true;
    }

    if (pending_ctrl_w_) {
        pending_ctrl_w_ = false;
        switch (c) {
            case 'w': CyclePane(1); break;
            case 'W': CyclePane(-1); break;
            case 'c': ClosePane(); break;
            case 's': SplitCurrentPane(SplitDir::Horizontal, ""); break;
            case 'v': SplitCurrentPane(SplitDir::Vertical, ""); break;
            case 'h': NavigatePaneDirection("left"); break;
            case 'j': NavigatePaneDirection("down"); break;
            case 'k': NavigatePaneDirection("up"); break;
            case 'l': NavigatePaneDirection("right"); break;
            default: break;
        }
        return true;
    }

    // Register prefix: "{a-z} (or "{A-Z} to append) names the register
    // the next operator or p/P should use, and can appear interspersed
    // with a count in either order ("a2dd or 2"add) since it doesn't
    // touch pending_count_. Checked before the digit-accumulation block
    // below for the same reason: neither should fire while the other's
    // mid-sequence.
    if (pending_find_ == 0 && !pending_g_ && !pending_ctrl_w_) {
        if (awaiting_register_name_) {
            awaiting_register_name_ = false;
            if (c >= 'a' && c <= 'z') {
                pending_register_ = c;
                pending_register_append_ = false;
            } else if (c >= 'A' && c <= 'Z') {
                pending_register_ = static_cast<char>(c - 'A' + 'a');
                pending_register_append_ = true;
            }
            // Anything else (digit, punctuation, ...): not a register
            // name mep supports (only "a-"z here -- see VIM_PARITY_PLAN.md
            // for the numbered/"% stretch goals), silently ignored.
            return true;
        }
        if (c == '"') {
            awaiting_register_name_ = true;
            return true;
        }
    }

    // Digits accumulate into a pending count rather than dispatching,
    // whether or not an operator is already pending -- Vim allows a count
    // both before an operator (2dw) and between the operator and its
    // motion (d3w), multiplying the two (see the pending_op_ branch
    // below). Not while pending_find_/pending_g_ are set: those consume
    // the very next key literally (the target char for f/F/t/T, or the
    // second half of a g-prefixed motion).
    if (pending_find_ == 0 && !pending_g_) {
        if (c >= '1' && c <= '9') {
            pending_count_ = pending_count_ * 10 + (c - '0');
            return true;
        }
        if (c == '0' && pending_count_ != 0) {
            pending_count_ = pending_count_ * 10;
            return true;
        }
    }

    // f/F/t/T: the next key is a literal target character, not a motion or
    // command in its own right, whether or not an operator is pending.
    if (pending_find_) {
        char cmd = pending_find_;
        pending_find_ = 0;
        last_find_cmd_ = cmd;
        last_find_char_ = c;
        int count = std::max(1, TakeRawCount());
        CursorPos from = pending_op_ ? pending_op_start_ : CurPane().cursor;
        CursorPos target;
        if (!ResolveFind(cmd, from, c, count, &target)) {
            pending_op_ = 0;  // find failed: cancel any pending operator too, same as Vim
            return true;
        }
        bool inclusive = (cmd == 'f' || cmd == 't');
        if (pending_op_) {
            char op = pending_op_;
            pending_op_ = 0;
            CursorPos end = target;
            if (inclusive) end.col++;
            ApplyOperator(op, from, end, false);
        } else {
            CurPane().cursor = target;
            ClampCursor();
        }
        return true;
    }

    // `/': the next key is the mark letter -- same "operator may or may
    // not be pending" shape as f/F/t/T above (`` `a `` moves, `` d`a ``
    // deletes to it).
    if (pending_mark_jump_) {
        char cmd = pending_mark_jump_;
        pending_mark_jump_ = 0;
        CursorPos from = pending_op_ ? pending_op_start_ : CurPane().cursor;
        CursorPos target;
        bool linewise = false;
        if (!ResolveMark(cmd, c, &target, &linewise)) {
            pending_op_ = 0;  // no such mark: cancel any pending operator too
            return true;
        }
        if (pending_op_) {
            char op = pending_op_;
            pending_op_ = 0;
            ApplyOperator(op, from, target, linewise);
        } else {
            RecordJumpFrom(from);
            CurPane().cursor = target;
            ClampCursor();
        }
        return true;
    }

    // m{a-z}: sets a mark. Standalone only -- unlike `/', never an
    // operator target (there's no such thing as deleting "to a
    // mark-setting").
    if (pending_mark_set_) {
        pending_mark_set_ = false;
        if (c >= 'a' && c <= 'z') Buf().marks[c] = CurPane().cursor;
        return true;
    }

    // r{char}: the replacement character (count was already captured when
    // 'r' itself was pressed, below).
    if (pending_replace_) {
        pending_replace_ = false;
        int count = pending_replace_count_;
        pending_replace_count_ = 0;
        if (c >= 32 && c < 127) ReplaceCharsAtCursor(c, count);
        return true;
    }

    // z{z,t,b}: reposition the view without moving the cursor.
    // z{a,o,c}: toggle/open/close the fold at the cursor (Phase 5).
    if (pending_z_) {
        pending_z_ = false;
        if (c == 'z' || c == 't' || c == 'b') {
            ScrollCursorTo(c);
        } else if (c == 'a') {
            ToggleFoldAtCursor();
        } else if (c == 'o' || c == 'c') {
            int row = CurPane().cursor.row;
            for (Fold &f : Buf().folds) {
                if (row >= f.start_row && row <= f.end_row) f.closed = (c == 'c');
            }
        }
        return true;
    }

    // q{a-z}/q{A-Z}: the register letter to record into (uppercase appends).
    if (pending_macro_record_) {
        pending_macro_record_ = false;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) StartMacroRecording(c);
        return true;
    }

    // @{a-z} / @@: the register to replay, or '@' again for "replay the
    // last-played macro". The count, if any, is read here (not when '@'
    // itself was pressed) for the same reason pending_find_ reads it at
    // resolution time -- it can be typed before or, in principle,
    // interspersed with the two-key command.
    if (awaiting_macro_play_) {
        awaiting_macro_play_ = false;
        char reg = (c == '@') ? last_played_macro_ : ((c >= 'a' && c <= 'z') ? c : 0);
        int count = std::max(1, TakeRawCount());
        if (reg != 0) PlayMacro(reg, count);
        return true;
    }

    // g-prefixed motions (gg / ge / gE) -- same "operator may or may not be
    // pending" shape as f/F/t/T above. gu/gU/gJ are checked first and only
    // when no operator is already pending: they're freestanding operators
    // in their own right (like d/y/c), not motions, so "gu" itself sets
    // pending_op_ (reusing 'u'/'U' as the operator letter -- see
    // ApplyOperator) and waits for ITS motion the same way "d" does,
    // rather than resolving anything here. (Only "guu"/"gUU" work as the
    // doubled-key current-line form, not "gugu"/"gUgU" -- both are valid
    // Vim spellings of the same thing, but only the first is worth the
    // extra dispatch complexity here.)
    if (pending_g_ && !pending_op_ && (c == 'u' || c == 'U')) {
        pending_g_ = false;
        pending_op_ = c;
        pending_op_start_ = CurPane().cursor;
        pending_op_count_ = TakeRawCount();
        return true;
    }
    if (pending_g_ && !pending_op_ && c == 'J') {
        pending_g_ = false;
        JoinLines(std::max(1, TakeRawCount()), false);
        return true;
    }
    if (pending_g_ && !pending_op_ && c == 'v') {
        pending_g_ = false;
        TakeRawCount();  // gv doesn't take a count here
        if (has_last_visual_) {
            mode_ = last_visual_linewise_ ? Mode::VisualLine : Mode::Visual;
            CurPane().visual_anchor = last_visual_anchor_;
            CurPane().cursor = last_visual_cursor_;
            ClampCursor();
        }
        return true;
    }
    if (pending_g_) {
        pending_g_ = false;
        CursorPos from = pending_op_ ? pending_op_start_ : CurPane().cursor;
        int count = TakeRawCount();
        CursorPos target;
        bool linewise = false, inclusive = false, ok = true;
        if (c == 'g') {
            target = FirstNonBlank(count > 0 ? std::min(Buf().LineCount() - 1, count - 1) : 0);
            linewise = true;
        } else if (c == 'e' || c == 'E') {
            target = from;
            for (int i = 0; i < std::max(1, count); i++) target = MoveWordEndBackward(target, c == 'E');
            inclusive = true;
        } else {
            ok = false;
        }
        if (!ok) {
            pending_op_ = 0;  // unknown g-motion: cancel quietly
            return true;
        }
        if (pending_op_) {
            char op = pending_op_;
            pending_op_ = 0;
            if (inclusive) target.col++;
            ApplyOperator(op, from, target, linewise);
        } else {
            if (c == 'g') RecordJumpFrom(from);  // gg is a "big jump", ge/gE aren't
            CurPane().cursor = target;
            ClampCursor();
        }
        return true;
    }

    if (pending_op_ != 0) {
        char op = pending_op_;
        CursorPos start = pending_op_start_;

        // Text object: "i"/"a" was seen last keypress (below), this one
        // names the object itself (w, ", (, p, ...).
        if (pending_textobj_scope_) {
            char scope = pending_textobj_scope_;
            pending_textobj_scope_ = 0;
            pending_op_ = 0;
            pending_op_count_ = 0;
            TakeRawCount();  // text objects don't take a count in mep (yet); discard rather than leak it
            CursorPos tstart, tend;
            bool linewise = false;
            if (ResolveTextObject(scope, c, start, &tstart, &tend, &linewise)) {
                ApplyOperator(op, tstart, tend, linewise);
            }
            return true;
        }

        if (c == op) {
            // dd / yy / cc: linewise, current line(s) -- the operator's own
            // count and any count typed between it and the doubled key
            // multiply, so both "3dd" and "d3d" delete 3 lines.
            pending_op_ = 0;
            int n = std::max(1, pending_op_count_) * std::max(1, TakeRawCount());
            pending_op_count_ = 0;
            int last_row = std::min(Buf().LineCount() - 1, start.row + n - 1);
            ApplyOperator(op, {start.row, 0}, {last_row, 0}, true);
            return true;
        }

        if (c == 'i' || c == 'a') {
            pending_textobj_scope_ = c;  // keep pending_op_ set; resolved next keypress above
            return true;
        }
        if (c == 'f' || c == 'F' || c == 't' || c == 'T') {
            pending_find_ = c;  // keep pending_op_ set; resolved above next keypress
            return true;
        }
        if (c == '`' || c == '\'') {
            pending_mark_jump_ = c;  // keep pending_op_ set; resolved above next keypress
            return true;
        }
        if (c == 'g') {
            pending_g_ = true;  // keep pending_op_ set
            return true;
        }

        pending_op_ = 0;
        int motion_count = TakeRawCount();
        int op_count = pending_op_count_;
        pending_op_count_ = 0;
        // G is an absolute line target, not a repeat count, so it doesn't
        // multiply with the operator's own count the way every other
        // motion here does -- whichever of the two was actually given
        // (preferring the one closer to the motion) is the target line.
        int count_for_resolve = (c == 'G') ? (motion_count > 0 ? motion_count : op_count)
                                            : std::max(1, op_count) * std::max(1, motion_count);
        CursorPos target;
        bool linewise = false, inclusive = false;
        if (!ResolveMotion(c, start, count_for_resolve, &target, &linewise, &inclusive)) {
            return true;  // unknown motion: cancel the pending operator quietly
        }
        if (inclusive) target.col++;
        ApplyOperator(op, start, target, linewise);
        return true;
    }

    if (c == 'f' || c == 'F' || c == 't' || c == 'T') {
        pending_find_ = c;
        return true;
    }
    if (c == '`' || c == '\'') {
        pending_mark_jump_ = c;
        return true;
    }
    if (c == 'm') {
        pending_mark_set_ = true;
        return true;
    }
    if (c == 'r') {
        pending_replace_count_ = std::max(1, TakeRawCount());
        pending_replace_ = true;
        return true;
    }
    if (c == 'g') {
        pending_g_ = true;
        return true;
    }
    if (c == 'z') {
        pending_z_ = true;
        return true;
    }
    if (c == 'q') {
        if (recording_macro_) StopMacroRecording();
        else pending_macro_record_ = true;
        return true;
    }
    if (c == '@') {
        awaiting_macro_play_ = true;
        return true;
    }
    if (c == ';' || c == ',') {
        if (last_find_cmd_ != 0) {
            char cmd = last_find_cmd_;
            if (c == ',') {
                switch (cmd) {
                    case 'f': cmd = 'F'; break;
                    case 'F': cmd = 'f'; break;
                    case 't': cmd = 'T'; break;
                    case 'T': cmd = 't'; break;
                }
            }
            int count = std::max(1, TakeRawCount());
            CursorPos target;
            if (ResolveFind(cmd, CurPane().cursor, last_find_char_, count, &target)) {
                CurPane().cursor = target;
                ClampCursor();
            }
        }
        return true;
    }

    CursorPos &cursor = CurPane().cursor;

    // Single-key motions shared with operator-pending dispatch above and
    // with Visual mode's own motion handling; peek (don't consume) the
    // pending count so an unrecognized key below still sees it -- e.g.
    // "3x" would otherwise lose its count to this check finding no motion
    // named 'x' before the switch below ever sees it.
    {
        CursorPos target;
        bool linewise = false, inclusive = false;
        if (ResolveMotion(c, cursor, pending_count_, &target, &linewise, &inclusive)) {
            pending_count_ = 0;
            if (c == 'G') RecordJumpFrom(cursor);  // "big jump"; h/j/k/l/w/etc. aren't
            cursor = target;
            ClampCursor();
            return true;
        }
    }

    switch (c) {
        case 'i': PushUndo(); EnterInsert(); break;
        case 'a':
            PushUndo();
            if (LineLen(cursor.row) > 0) cursor.col++;
            EnterInsert();
            break;
        case 'I': PushUndo(); cursor = FirstNonBlank(cursor.row); EnterInsert(); break;
        case 'A': PushUndo(); cursor.col = LineLen(cursor.row); EnterInsert(); break;
        case 'o':
            PushUndo();
            Buf().lines.insert(Buf().lines.begin() + cursor.row + 1, "");
            cursor.row++;
            cursor.col = 0;
            EnterInsert();
            break;
        case 'O':
            PushUndo();
            Buf().lines.insert(Buf().lines.begin() + cursor.row, "");
            cursor.col = 0;
            EnterInsert();
            break;
        case 'R':
            PushUndo();
            replace_mode_ = true;
            replace_overwritten_.clear();
            EnterInsert();
            break;

        case 'x': {
            int n = std::max(1, TakeRawCount());
            if (LineLen(cursor.row) > 0) {
                int end_col = std::min(LineLen(cursor.row), cursor.col + n);
                ApplyOperator('d', cursor, {cursor.row, end_col}, false);
            }
            break;
        }

        case 'd': pending_op_ = 'd'; pending_op_start_ = cursor; pending_op_count_ = TakeRawCount(); break;
        case 'y': pending_op_ = 'y'; pending_op_start_ = cursor; pending_op_count_ = TakeRawCount(); break;
        case 'c': pending_op_ = 'c'; pending_op_start_ = cursor; pending_op_count_ = TakeRawCount(); break;

        // D/C: shorthand for d$/c$. (Vim's {count}D/{count}C additionally
        // pull in count-1 whole lines below the current one; not
        // implemented here as a low-value, rarely-relied-on wrinkle -- any
        // pending count is just discarded rather than misapplied.)
        case 'D': {
            TakeRawCount();
            CursorPos target;
            bool linewise = false, inclusive = false;
            ResolveMotion('$', cursor, 0, &target, &linewise, &inclusive);
            if (inclusive) target.col++;
            ApplyOperator('d', cursor, target, false);
            break;
        }
        case 'C': {
            TakeRawCount();
            CursorPos target;
            bool linewise = false, inclusive = false;
            ResolveMotion('$', cursor, 0, &target, &linewise, &inclusive);
            if (inclusive) target.col++;
            ApplyOperator('c', cursor, target, false);
            break;
        }
        // Y: Vim quirk -- a synonym for yy (whole line), *not* y$, despite
        // superficially looking like D/C's pattern.
        case 'Y': {
            int n = std::max(1, TakeRawCount());
            int last_row = std::min(Buf().LineCount() - 1, cursor.row + n - 1);
            ApplyOperator('y', {cursor.row, 0}, {last_row, 0}, true);
            break;
        }

        case 'p': {
            char reg_name = 0;
            bool append = false;
            TakeRegisterSpec(&reg_name, &append);
            PasteAfter(std::max(1, TakeRawCount()), reg_name);
            break;
        }
        case 'P': {
            char reg_name = 0;
            bool append = false;
            TakeRegisterSpec(&reg_name, &append);
            PasteBefore(std::max(1, TakeRawCount()), reg_name);
            break;
        }

        case 'u': Undo(); break;
        case '.': RepeatLastChange(TakeRawCount()); break;

        case '~': {
            int n = std::max(1, TakeRawCount());
            int len = LineLen(cursor.row);
            if (len > 0) {
                int end_col = std::min(len, cursor.col + n);
                ApplyOperator('~', cursor, {cursor.row, end_col}, false);
                cursor.col = end_col;
            }
            break;
        }

        case 'J': JoinLines(std::max(1, TakeRawCount()), true); break;

        // >>/<<: same doubled-key-for-current-line shape as dd/yy/cc
        // (reusing pending_op_'s existing "c == op" check), plus
        // >{motion}/<{motion} via the existing operator-motion path.
        case '>': pending_op_ = '>'; pending_op_start_ = cursor; pending_op_count_ = TakeRawCount(); break;
        case '<': pending_op_ = '<'; pending_op_start_ = cursor; pending_op_count_ = TakeRawCount(); break;

        case 'v': EnterVisual(false); break;
        case 'V': EnterVisual(true); break;

        case ':': EnterCommand(); break;
        case '/': TakeRawCount(); EnterSearch(true); break;
        case '?': TakeRawCount(); EnterSearch(false); break;
        case 'n': if (!last_search_.empty()) PerformSearch(last_search_forward_); break;
        case 'N': if (!last_search_.empty()) PerformSearch(!last_search_forward_); break;
        case '*': SearchWordUnderCursor(true); break;
        case '#': SearchWordUnderCursor(false); break;

        default: return false;
    }
    ClampCursor();
    return true;
}

bool Editor::IsMidNormalCommand() const {
    return pending_op_ != 0 || pending_g_ || pending_find_ != 0 || pending_textobj_scope_ != 0 ||
           pending_mark_jump_ != 0 || pending_mark_set_ || pending_ctrl_w_ || pending_z_ || pending_count_ != 0 ||
           awaiting_register_name_ || pending_register_ != 0 || pending_macro_record_ || awaiting_macro_play_ ||
           pending_replace_;
}

void Editor::CancelPendingNormalState() {
    pending_op_ = 0;
    pending_op_count_ = 0;
    pending_g_ = false;
    pending_find_ = 0;
    pending_mark_jump_ = 0;
    pending_mark_set_ = false;
    pending_textobj_scope_ = 0;
    pending_ctrl_w_ = false;
    pending_z_ = false;
    pending_count_ = 0;
    awaiting_register_name_ = false;
    pending_register_ = 0;
    pending_register_append_ = false;
    pending_macro_record_ = false;
    awaiting_macro_play_ = false;
    pending_replace_ = false;
    pending_replace_count_ = 0;
    change_scratch_.clear();
    change_recording_active_ = false;
}

// Normal-mode entry point shared by real input (HandleNormalInput) and
// replay (RepeatLastChange/PlayMacro) -- see the ReplayKey/last_change_keys_
// comments in editor.h for the recording scheme this implements.
void Editor::ProcessNormalKey(int key) {
    bool was_mid = IsMidNormalCommand();

    bool suppress_macro = replaying_change_ || replaying_macro_;
    bool stops_macro = recording_macro_ && !suppress_macro && !was_mid && key == 'q';
    if (recording_macro_ && !suppress_macro && !stops_macro) {
        macro_recording_buffer_.push_back(key);
    }

    if (!replaying_change_) {
        if (!was_mid) {
            change_scratch_.clear();
            change_had_edit_ = false;
            // A bare '.'/'u' at the start of a command is never itself
            // repeatable -- '.' just replays the existing last change,
            // and Vim doesn't repeat undo via '.' either.
            change_recording_active_ = !(key == '.' || key == 'u');
        }
        if (change_recording_active_) change_scratch_.push_back(key);
    }

    int epoch_before = change_epoch_;
    DispatchNormalKey(key);

    if (!replaying_change_ && change_recording_active_) {
        if (change_epoch_ != epoch_before) change_had_edit_ = true;
        if (mode_ != Mode::Insert && !IsMidNormalCommand()) {
            if (change_had_edit_) last_change_keys_ = change_scratch_;
            change_scratch_.clear();
            change_recording_active_ = false;
        }
    }
}

// Insert-mode equivalent of ProcessNormalKey; `key` is either a printable
// codepoint or one of the ReplayKey sentinels (see editor.h).
void Editor::ProcessInsertKey(int key) {
    bool suppress_macro = replaying_change_ || replaying_macro_;
    if (recording_macro_ && !suppress_macro) macro_recording_buffer_.push_back(key);
    if (!replaying_change_ && change_recording_active_) change_scratch_.push_back(key);

    // Track what's actually been typed during a block-insert session (see
    // EnterVisualBlockInsert/FinishVisualBlockInsert) -- only plain typed
    // characters and Backspace matter for replication onto the other rows;
    // Vim's own block-insert has the same limitation with Enter/Delete/etc.
    if (block_insert_active_) {
        if (key > 0) {
            block_insert_typed_ += static_cast<char>(key);
        } else if (key == kReplayBackspace && !block_insert_typed_.empty()) {
            block_insert_typed_.pop_back();
        }
    }

    switch (key) {
        case kReplayEscape: EnterNormal(); break;
        case kReplayEnter: InsertNewline(); break;
        case kReplayBackspace:
            if (replace_mode_) ReplaceBackspace(); else Backspace();
            break;
        case kReplayDelete: DeleteForward(); break;
        case kReplayInsertCtrlW: DeleteWordBeforeCursorInInsert(); break;
        case kReplayInsertCtrlU: DeleteToLineStartInInsert(); break;
        default:
            if (key > 0) {
                if (replace_mode_) ReplaceChar(key); else InsertChar(key);
            }
            break;
    }

    if (key == kReplayEscape) FinishVisualBlockInsert();

    if (!replaying_change_ && change_recording_active_ && key == kReplayEscape) {
        if (change_had_edit_) last_change_keys_ = change_scratch_;
        change_scratch_.clear();
        change_recording_active_ = false;
    }
}

void Editor::RepeatLastChange(int override_count) {
    if (last_change_keys_.empty() || replaying_change_) return;
    std::vector<int> keys = last_change_keys_;
    if (override_count > 0) {
        size_t digit_run = 0;
        while (digit_run < keys.size() && keys[digit_run] >= '0' && keys[digit_run] <= '9' &&
               !(digit_run == 0 && keys[digit_run] == '0')) {
            digit_run++;
        }
        std::string digits = std::to_string(override_count);
        std::vector<int> spliced(digits.begin(), digits.end());
        spliced.insert(spliced.end(), keys.begin() + digit_run, keys.end());
        keys = spliced;
    }
    replaying_change_ = true;
    for (size_t i = 0; i < keys.size(); i++) {
        if (mode_ == Mode::Insert) {
            ProcessInsertKey(keys[i]);
        } else if (mode_ == Mode::Normal) {
            ProcessNormalKey(keys[i]);
        } else {
            break;  // shouldn't happen -- a recorded change never leaves Normal/Insert mid-way
        }
    }
    if (mode_ == Mode::Insert) EnterNormal();  // an unterminated recording shouldn't leave Insert open
    replaying_change_ = false;
}

void Editor::StartMacroRecording(char reg) {
    bool append = (reg >= 'A' && reg <= 'Z');
    char name = append ? static_cast<char>(reg - 'A' + 'a') : reg;
    recording_macro_ = true;
    recording_macro_reg_ = name;
    macro_recording_buffer_ = append ? macros_[name] : std::vector<int>{};
}

void Editor::StopMacroRecording() {
    recording_macro_ = false;
    macros_[recording_macro_reg_] = macro_recording_buffer_;
    macro_recording_buffer_.clear();
    recording_macro_reg_ = 0;
}

void Editor::PlayMacro(char reg, int count) {
    auto it = macros_.find(reg);
    if (it == macros_.end() || it->second.empty()) return;
    // A macro that calls itself (directly or via another macro) is a common,
    // intentional Vim idiom -- usually terminated by a motion or search
    // eventually failing partway through, not by an explicit count -- so
    // nesting itself is allowed; this is only a generous backstop against a
    // genuinely runaway recursion blowing the C++ stack.
    static constexpr int kMaxMacroReplayDepth = 1000;
    if (macro_replay_depth_ >= kMaxMacroReplayDepth) return;
    last_played_macro_ = reg;
    std::vector<int> keys = it->second;  // copy: the map could grow (a new macro recorded) mid-replay
    bool was_replaying = replaying_macro_;
    replaying_macro_ = true;
    macro_replay_depth_++;
    for (int rep = 0; rep < count; rep++) {
        for (size_t i = 0; i < keys.size(); i++) {
            int k = keys[i];
            if (mode_ == Mode::Insert) {
                ProcessInsertKey(k);
                continue;
            }
            if (mode_ != Mode::Normal) break;  // Command/Search mode mid-macro: not supported, bail
            switch (k) {
                case kReplayEscape: CancelPendingNormalState(); break;
                case kReplayCtrlR: Redo(); break;
                case kReplayCtrlW: pending_ctrl_w_ = true; break;
                default: ProcessNormalKey(k); break;
            }
        }
    }
    macro_replay_depth_--;
    replaying_macro_ = was_replaying;
}

// --- Insert mode -------------------------------------------------------

void Editor::HandleInsertInput() {
    // See the same GetKeyPressed()-vs-IsKeyPressed() note in
    // HandleCommandInput(): a same-frame keydown+keyup is invisible to
    // IsKeyPressed's state snapshot, so Escape/Enter/Backspace/Delete are
    // read from raylib's press queue instead, which can't miss them. Ctrl-W/
    // Ctrl-U ride the same queue for the same reason -- an early version of
    // this used IsKeyPressed(KEY_W/KEY_U) the way HandleNormalInput's
    // Ctrl-R/Ctrl-W checks do, and it was visibly flaky under a slow
    // (software-rendered Xvfb) frame for exactly that reason. GetKeyPressed()
    // still reports the raw key regardless of modifiers held (it's how
    // Escape, which has no character form at all, gets caught here), so
    // pairing it with a plain IsKeyDown() ctrl check (level state, not an
    // edge -- not subject to the same race) is enough to tell "Ctrl-W" apart
    // from a bare "w" that the GetCharPressed() loop below will see instead.
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    bool escape = false, enter = false, backspace = false, del = false, ctrl_w = false, ctrl_u = false;
    bool tab_key = false, ctrl_n = false, ctrl_p = false;
    for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed()) {
        if (key == KEY_ESCAPE) escape = true;
        else if (key == KEY_ENTER) enter = true;
        else if (key == KEY_BACKSPACE) backspace = true;
        else if (key == KEY_DELETE) del = true;
        else if (key == KEY_W && ctrl) ctrl_w = true;
        else if (key == KEY_U && ctrl) ctrl_u = true;
        else if (key == KEY_TAB) tab_key = true;
        else if (key == KEY_N && ctrl) ctrl_n = true;
        else if (key == KEY_P && ctrl) ctrl_p = true;
    }
    // Completion popup (Phase 22): intercepts only its own navigation/
    // accept/dismiss keys, and only while open -- a first Escape closes
    // just the popup (not Insert mode itself, matching every other
    // editor's completion UX); everything else (ordinary typing
    // included) falls through to normal Insert handling below and
    // re-triggers UpdateCompletionPopup() at the end of this function.
    if (completion_open_) {
        if (escape) {
            completion_open_ = false;
            return;
        }
        if (ctrl_n) {
            CompletionNext();
            return;
        }
        if (ctrl_p) {
            CompletionPrev();
            return;
        }
        if (tab_key || enter) {
            AcceptCompletion();
            return;
        }
    }
    if (escape) {
        ProcessInsertKey(kReplayEscape);
        return;
    }
    if (ctrl_w) {
        ProcessInsertKey(kReplayInsertCtrlW);
        return;
    }
    if (ctrl_u) {
        ProcessInsertKey(kReplayInsertCtrlU);
        return;
    }

    int cp = GetCharPressed();
    while (cp > 0) {
        ProcessInsertKey(cp);
        cp = GetCharPressed();
    }

    CursorPos &cursor = CurPane().cursor;
    if (enter || IsKeyPressedRepeat(KEY_ENTER)) ProcessInsertKey(kReplayEnter);
    if (backspace || IsKeyPressedRepeat(KEY_BACKSPACE)) ProcessInsertKey(kReplayBackspace);
    if (del || IsKeyPressedRepeat(KEY_DELETE)) ProcessInsertKey(kReplayDelete);
    if (IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT)) {
        if (cursor.col > 0) {
            cursor.col--;
        } else if (cursor.row > 0) {
            cursor.row--;
            cursor.col = LineLen(cursor.row);
        }
    }
    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT)) {
        if (cursor.col < LineLen(cursor.row)) {
            cursor.col++;
        } else if (cursor.row + 1 < Buf().LineCount()) {
            cursor.row++;
            cursor.col = 0;
        }
    }
    if (IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP)) {
        if (cursor.row > 0) { cursor.row--; ClampCursor(); }
    }
    if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN)) {
        if (cursor.row + 1 < Buf().LineCount()) { cursor.row++; ClampCursor(); }
    }
    if (mode_ == Mode::Insert) UpdateCompletionPopup();
}

void Editor::InsertChar(int codepoint) {
    if (codepoint < 32 || codepoint > 126) return;
    CursorPos &cursor = CurPane().cursor;
    std::string &line = Buf().lines[cursor.row];
    line.insert(line.begin() + cursor.col, static_cast<char>(codepoint));
    cursor.col++;
    Buf().modified = true;
}

void Editor::InsertNewline() {
    CursorPos &cursor = CurPane().cursor;
    std::string &line = Buf().lines[cursor.row];
    std::string remainder = line.substr(cursor.col);
    line.erase(cursor.col);
    Buf().lines.insert(Buf().lines.begin() + cursor.row + 1, remainder);
    cursor.row++;
    cursor.col = 0;
    Buf().modified = true;
}

void Editor::Backspace() {
    CursorPos &cursor = CurPane().cursor;
    if (cursor.col > 0) {
        std::string &line = Buf().lines[cursor.row];
        line.erase(cursor.col - 1, 1);
        cursor.col--;
    } else if (cursor.row > 0) {
        std::string current = Buf().lines[cursor.row];
        Buf().lines.erase(Buf().lines.begin() + cursor.row);
        cursor.row--;
        cursor.col = LineLen(cursor.row);
        Buf().lines[cursor.row] += current;
    }
    Buf().modified = true;
}

void Editor::DeleteForward() {
    CursorPos &cursor = CurPane().cursor;
    std::string &line = Buf().lines[cursor.row];
    if (cursor.col < static_cast<int>(line.size())) {
        line.erase(cursor.col, 1);
    } else if (cursor.row + 1 < Buf().LineCount()) {
        std::string next = Buf().lines[cursor.row + 1];
        Buf().lines.erase(Buf().lines.begin() + cursor.row + 1);
        line += next;
    }
    Buf().modified = true;
}

void Editor::ReplaceCharsAtCursor(int codepoint, int count) {
    CursorPos &cursor = CurPane().cursor;
    int len = LineLen(cursor.row);
    if (count <= 0 || cursor.col + count > len) return;  // not enough chars left on the line: refuse, like Vim
    PushUndo();
    std::string &line = Buf().lines[cursor.row];
    for (int i = 0; i < count; i++) line[cursor.col + i] = static_cast<char>(codepoint);
    cursor.col += count - 1;
    Buf().modified = true;
    ClampCursor();
}

void Editor::ReplaceChar(int codepoint) {
    if (codepoint < 32 || codepoint > 126) return;
    CursorPos &cursor = CurPane().cursor;
    std::string &line = Buf().lines[cursor.row];
    if (cursor.col < static_cast<int>(line.size())) {
        replace_overwritten_.push_back(line[cursor.col]);
        line[cursor.col] = static_cast<char>(codepoint);
    } else {
        replace_overwritten_.push_back('\0');  // extended the line -- nothing to restore later
        line.insert(line.begin() + cursor.col, static_cast<char>(codepoint));
    }
    cursor.col++;
    Buf().modified = true;
}

void Editor::ReplaceBackspace() {
    CursorPos &cursor = CurPane().cursor;
    if (replace_overwritten_.empty() || cursor.col == 0) {
        // Nothing from this Replace session left to restore (or the start
        // of the line) -- Vim just moves the cursor left without touching
        // text in this situation too.
        if (cursor.col > 0) cursor.col--;
        return;
    }
    char restored = replace_overwritten_.back();
    replace_overwritten_.pop_back();
    cursor.col--;
    std::string &line = Buf().lines[cursor.row];
    if (restored == '\0') {
        line.erase(cursor.col, 1);
    } else {
        line[cursor.col] = restored;
    }
    Buf().modified = true;
}

void Editor::DeleteWordBeforeCursorInInsert() {
    CursorPos &cursor = CurPane().cursor;
    if (cursor.col == 0) return;  // no line-join, unlike Backspace -- matches Vim's Ctrl-W here
    std::string &line = Buf().lines[cursor.row];
    int col = cursor.col;
    while (col > 0 && std::isspace(static_cast<unsigned char>(line[col - 1]))) col--;
    if (col > 0) {
        CharClass cls = ClassOf(line[col - 1]);
        while (col > 0 && ClassOf(line[col - 1]) == cls) col--;
    }
    line.erase(col, cursor.col - col);
    cursor.col = col;
    Buf().modified = true;
}

void Editor::DeleteToLineStartInInsert() {
    CursorPos &cursor = CurPane().cursor;
    if (cursor.col == 0) return;
    Buf().lines[cursor.row].erase(0, cursor.col);
    cursor.col = 0;
    Buf().modified = true;
}

// --- Visual mode -------------------------------------------------------

void Editor::ApplyVisualBlockOperator(char op) {
    int top, bottom, left, right;
    VisualBlockRange(top, bottom, left, right);
    bool eol = (right < 0);

    char reg_name = 0;
    bool append = false;
    TakeRegisterSpec(&reg_name, &append);
    std::vector<std::string> block_lines;
    for (int r = top; r <= bottom; r++) {
        const std::string &line = Buf().lines[r];
        int a = std::min(static_cast<int>(line.size()), left);
        int b = eol ? static_cast<int>(line.size()) : std::min(static_cast<int>(line.size()), right + 1);
        block_lines.push_back(b > a ? line.substr(a, b - a) : std::string());
    }
    // Trailing '\n' after every row (including the last), matching
    // YankRange's linewise convention -- SplitYankLines (used by
    // PasteAfter/PasteBefore) expects that shape.
    std::string joined;
    for (const std::string &l : block_lines) {
        joined += l;
        joined += '\n';
    }
    Register &target = RegisterFor(reg_name);
    if (append && reg_name != 0) {
        target.text += joined;
    } else {
        target.text = joined;
    }
    target.linewise = false;
    target.blockwise = true;
    // Same unnamed-register mirroring YankRange does.
    if (reg_name != 0) registers_['"'] = target;

    if (op == 'd') {
        PushUndo();
        for (int r = top; r <= bottom; r++) {
            std::string &line = Buf().lines[r];
            int a = std::min(static_cast<int>(line.size()), left);
            int b = eol ? static_cast<int>(line.size()) : std::min(static_cast<int>(line.size()), right + 1);
            if (b > a) line.erase(a, b - a);
        }
        Buf().modified = true;
    }
    CurPane().cursor = {top, left};
    ClampCursor();
}

void Editor::EnterVisualBlockInsert(bool at_end) {
    int top, bottom, left, right;
    VisualBlockRange(top, bottom, left, right);
    bool eol = (right < 0);
    int col = at_end ? (eol ? LineLen(top) : right + 1) : left;

    PushUndo();
    std::string &first_line = Buf().lines[top];
    if (!eol && static_cast<int>(first_line.size()) < col) {
        first_line += std::string(col - first_line.size(), ' ');
    }
    CurPane().cursor = {top, col};

    block_insert_active_ = true;
    block_insert_top_ = top;
    block_insert_bottom_ = bottom;
    block_insert_col_ = col;
    block_insert_eol_ = (at_end && eol);
    block_insert_typed_.clear();
    EnterInsert();
}

void Editor::FinishVisualBlockInsert() {
    if (!block_insert_active_) return;
    block_insert_active_ = false;
    if (!block_insert_typed_.empty()) {
        for (int r = block_insert_top_ + 1; r <= block_insert_bottom_ && r < Buf().LineCount(); r++) {
            std::string &line = Buf().lines[r];
            int col = block_insert_eol_ ? static_cast<int>(line.size()) : block_insert_col_;
            if (!block_insert_eol_ && static_cast<int>(line.size()) < col) {
                line += std::string(col - line.size(), ' ');
            }
            if (col <= static_cast<int>(line.size())) line.insert(col, block_insert_typed_);
        }
        Buf().modified = true;
    }
    block_insert_typed_.clear();
}

void Editor::PasteBlockAt(CursorPos at, const std::vector<std::string> &block, bool before) {
    int col = before ? at.col : std::min(LineLen(at.row), at.col + 1);
    for (size_t i = 0; i < block.size(); i++) {
        int row = at.row + static_cast<int>(i);
        if (row >= Buf().LineCount()) Buf().lines.push_back("");
        std::string &line = Buf().lines[row];
        if (static_cast<int>(line.size()) < col) line += std::string(col - line.size(), ' ');
        line.insert(col, block[i]);
    }
    CurPane().cursor = {at.row, col};
}

void Editor::HandleVisualInput() {
    if (IsKeyPressed(KEY_ESCAPE)) {
        EnterNormal();  // also clears pending_op_/pending_g_/pending_find_/pending_count_/etc.
        return;
    }

    int cp = GetCharPressed();
    while (cp > 0) {
        if (cp > 0 && cp <= 127) {
            char c = static_cast<char>(cp);
            CursorPos &cursor = CurPane().cursor;

            // Same register/count/find/g-prefix handling as Normal mode
            // (see DispatchNormalKey), minus operator-pending combination
            // since Visual mode's own operators (d/x/y below) always act
            // on the whole selection rather than a fresh motion.
            if (pending_find_ == 0 && !pending_g_) {
                if (awaiting_register_name_) {
                    awaiting_register_name_ = false;
                    if (c >= 'a' && c <= 'z') {
                        pending_register_ = c;
                        pending_register_append_ = false;
                    } else if (c >= 'A' && c <= 'Z') {
                        pending_register_ = static_cast<char>(c - 'A' + 'a');
                        pending_register_append_ = true;
                    }
                    cp = GetCharPressed();
                    continue;
                }
                if (c == '"') {
                    awaiting_register_name_ = true;
                    cp = GetCharPressed();
                    continue;
                }
                if (c >= '1' && c <= '9') {
                    pending_count_ = pending_count_ * 10 + (c - '0');
                    cp = GetCharPressed();
                    continue;
                }
                if (c == '0' && pending_count_ != 0) {
                    pending_count_ = pending_count_ * 10;
                    cp = GetCharPressed();
                    continue;
                }
            }

            if (pending_find_) {
                char cmd = pending_find_;
                pending_find_ = 0;
                last_find_cmd_ = cmd;
                last_find_char_ = c;
                int count = std::max(1, TakeRawCount());
                CursorPos target;
                if (ResolveFind(cmd, cursor, c, count, &target)) cursor = target;
                cp = GetCharPressed();
                continue;
            }

            if (pending_g_ && (c == 'u' || c == 'U')) {
                pending_g_ = false;
                TakeRawCount();
                CursorPos s, e;
                VisualRange(s, e);
                bool lw = (mode_ == Mode::VisualLine);
                if (!lw) e.col += 1;
                ApplyCaseChange(s, e, lw, c);
                EnterNormal();
                return;
            }
            if (pending_g_) {
                pending_g_ = false;
                int count = TakeRawCount();
                if (c == 'g') {
                    cursor = FirstNonBlank(count > 0 ? std::min(Buf().LineCount() - 1, count - 1) : 0);
                } else if (c == 'e' || c == 'E') {
                    for (int i = 0; i < std::max(1, count); i++) cursor = MoveWordEndBackward(cursor, c == 'E');
                }
                cp = GetCharPressed();
                continue;
            }

            if (pending_textobj_scope_) {
                char scope = pending_textobj_scope_;
                pending_textobj_scope_ = 0;
                CursorPos tstart, tend;
                bool linewise = false;
                if (ResolveTextObject(scope, c, cursor, &tstart, &tend, &linewise)) {
                    CurPane().visual_anchor = tstart;
                    if (linewise) {
                        mode_ = Mode::VisualLine;
                        cursor = tend;
                    } else {
                        // tend is an exclusive charwise end; Visual mode's
                        // own selection is inclusive on both ends (see
                        // ApplyOperatorToSelectionOrCurrentLine's `+1`), so
                        // land the cursor one char before it.
                        cursor = tend;
                        if (cursor.col > 0) {
                            cursor.col--;
                        } else if (cursor.row > 0) {
                            cursor.row--;
                            cursor.col = std::max(0, LineLen(cursor.row) - 1);
                        }
                    }
                }
                cp = GetCharPressed();
                continue;
            }

            if (TryLuaMapping(mode_, std::string(1, c))) {
                cp = GetCharPressed();
                continue;
            }

            if (c == 'i' || c == 'a') {
                pending_textobj_scope_ = c;
                cp = GetCharPressed();
                continue;
            }
            if (c == 'f' || c == 'F' || c == 't' || c == 'T') {
                pending_find_ = c;
                cp = GetCharPressed();
                continue;
            }
            if (c == 'g') {
                pending_g_ = true;
                cp = GetCharPressed();
                continue;
            }
            if (c == ';' || c == ',') {
                if (last_find_cmd_ != 0) {
                    char cmd = last_find_cmd_;
                    if (c == ',') {
                        switch (cmd) {
                            case 'f': cmd = 'F'; break;
                            case 'F': cmd = 'f'; break;
                            case 't': cmd = 'T'; break;
                            case 'T': cmd = 't'; break;
                        }
                    }
                    int count = std::max(1, TakeRawCount());
                    CursorPos target;
                    if (ResolveFind(cmd, cursor, last_find_char_, count, &target)) cursor = target;
                }
                cp = GetCharPressed();
                continue;
            }

            {
                CursorPos target;
                bool linewise = false, inclusive = false;
                if (ResolveMotion(c, cursor, pending_count_, &target, &linewise, &inclusive)) {
                    pending_count_ = 0;
                    cursor = target;
                    // Visual Block's own "sticky $" (see block_to_eol_): '$'
                    // extends every row of the block to its own actual end
                    // until some other motion changes the column extent.
                    if (mode_ == Mode::VisualBlock) block_to_eol_ = (c == '$');
                    cp = GetCharPressed();
                    continue;
                }
            }

            switch (c) {
                case 'o': {
                    CursorPos &anchor = CurPane().visual_anchor;
                    std::swap(anchor, cursor);
                    cp = GetCharPressed();
                    continue;
                }
                case 'd':
                case 'x':
                    if (mode_ == Mode::VisualBlock) ApplyVisualBlockOperator('d');
                    else ApplyOperatorToSelectionOrCurrentLine('d');
                    EnterNormal();
                    return;
                case 'y':
                    if (mode_ == Mode::VisualBlock) ApplyVisualBlockOperator('y');
                    else ApplyOperatorToSelectionOrCurrentLine('y');
                    EnterNormal();
                    return;
                case '~':
                    ApplyOperatorToSelectionOrCurrentLine('~');
                    EnterNormal();
                    return;
                case 'I':
                    if (mode_ != Mode::VisualBlock) break;
                    EnterVisualBlockInsert(false);
                    return;
                case 'A':
                    if (mode_ != Mode::VisualBlock) break;
                    EnterVisualBlockInsert(true);
                    return;
                case '>':
                case '<': {
                    CursorPos s, e;
                    VisualRange(s, e);
                    IndentLines(s.row, e.row, c == '>' ? 1 : -1);
                    // Vim keeps the selection active after Visual >/< (unlike
                    // its other operators) so repeated presses re-indent by
                    // another level -- deliberately not calling EnterNormal()
                    // here, and leaving anchor/cursor untouched so the exact
                    // same range is still selected.
                    cp = GetCharPressed();
                    continue;
                }
                default: break;
            }
        }
        cp = GetCharPressed();
    }

    CursorPos &cursor = CurPane().cursor;
    cursor.row = std::max(0, std::min(cursor.row, Buf().LineCount() - 1));
    int len = LineLen(cursor.row);
    cursor.col = std::max(0, std::min(cursor.col, std::max(0, len - 1)));
}

// --- Command-line mode ---------------------------------------------------

void Editor::HandleCommandInput() {
    // IsKeyPressed(KEY_ENTER) compares a once-per-frame current/previous
    // state snapshot, so a keydown+keyup that both land inside the same
    // (slow, software-rendered -- see WEBKIT_DISABLE_COMPOSITING_MODE in
    // launcher/serve.ts) frame is invisible to it: current is already back
    // to "up" by the time the snapshot is taken. GetKeyPressed() drains
    // raylib's own key-press queue instead, which is pushed to on every
    // press regardless of how fast the release follows -- immune to that
    // race, same as GetCharPressed() already is for typed text. Confirmed
    // this was silently swallowing Enter here: `:qa` would sit in the
    // command line with the app fully unresponsive after.
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    bool escape = false, enter = false, backspace = false, up = false, down = false;
    bool tab_key = false, ctrl_n = false, ctrl_p = false;
    for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed()) {
        if (key == KEY_ESCAPE) escape = true;
        else if (key == KEY_ENTER) enter = true;
        else if (key == KEY_BACKSPACE) backspace = true;
        else if (key == KEY_UP) up = true;
        else if (key == KEY_DOWN) down = true;
        else if (key == KEY_TAB) tab_key = true;
        else if (key == KEY_N && ctrl) ctrl_n = true;
        else if (key == KEY_P && ctrl) ctrl_p = true;
    }
    // Peeked (not just checked) up front, unlike the other keys above,
    // because whether a char was typed this frame feeds a decision below
    // (does typing close the completion popup) -- GetCharPressed() drains
    // raylib's char queue same as GetKeyPressed() drains the key queue
    // above, so this one has to be saved rather than re-queried, or the
    // typing loop further down would silently lose it.
    int pending_char = GetCharPressed();
    // Command-line completion popup: Tab opens/completes, Ctrl-N/Ctrl-P
    // move the highlighted item, Enter accepts (splices the item into
    // command_line_ but -- unlike a bare Enter below -- does not execute
    // it, so a second Enter is needed, same as picking a word from the
    // Insert-mode popup doesn't also leave Insert mode), Escape dismisses
    // just the popup. Any other key/char edits the line, so the (now
    // stale) list is dropped and that input falls through to ordinary
    // handling -- but a frame with no relevant input at all (the common
    // case: nothing was pressed) must leave the popup alone, or it would
    // close itself one frame after ever opening.
    if (cmdline_completion_open_) {
        if (escape) {
            cmdline_completion_open_ = false;
            return;
        }
        if (ctrl_n) {
            CmdlineCompletionNext();
            return;
        }
        if (ctrl_p) {
            CmdlineCompletionPrev();
            return;
        }
        if (tab_key || enter) {
            AcceptCmdlineCompletion();
            return;
        }
        if (backspace || up || down || pending_char > 0) cmdline_completion_open_ = false;
    }
    if (tab_key) {
        UpdateCmdlineCompletion();
        return;
    }
    if (escape) {
        EnterNormal();
        return;
    }
    if (enter) {
        std::string cmd = command_line_;
        EnterNormal();
        if (!cmd.empty() && (command_history_.empty() || command_history_.back() != cmd)) {
            command_history_.push_back(cmd);
        }
        ExecuteCommandLine(cmd);
        return;
    }
    if (backspace || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        if (!command_line_.empty()) {
            command_line_.pop_back();
        } else {
            EnterNormal();
        }
        return;
    }
    // Up/Down recall previous `:` commands, same idea as shell history --
    // Up starts browsing from the newest entry backward, remembering
    // whatever was typed so far (cmd_history_saved_) so Down can return to
    // it past the newest entry rather than just landing on "".
    if (up && !command_history_.empty()) {
        if (cmd_history_index_ == -1) {
            cmd_history_saved_ = command_line_;
            cmd_history_index_ = static_cast<int>(command_history_.size());
        }
        if (cmd_history_index_ > 0) {
            cmd_history_index_--;
            command_line_ = command_history_[cmd_history_index_];
        }
        return;
    }
    if (down && cmd_history_index_ != -1) {
        cmd_history_index_++;
        if (cmd_history_index_ >= static_cast<int>(command_history_.size())) {
            cmd_history_index_ = -1;
            command_line_ = cmd_history_saved_;
        } else {
            command_line_ = command_history_[cmd_history_index_];
        }
        return;
    }
    int cp = pending_char;
    while (cp > 0) {
        if (cp >= 32 && cp < 127) command_line_ += static_cast<char>(cp);
        cp = GetCharPressed();
    }
}

void Editor::HandleSearchInput() {
    // Same GetKeyPressed()-vs-IsKeyPressed() reasoning as HandleCommandInput.
    bool escape = false, enter = false, backspace = false, up = false, down = false;
    for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed()) {
        if (key == KEY_ESCAPE) escape = true;
        else if (key == KEY_ENTER) enter = true;
        else if (key == KEY_BACKSPACE) backspace = true;
        else if (key == KEY_UP) up = true;
        else if (key == KEY_DOWN) down = true;
    }
    if (escape) {
        EnterNormal();
        return;
    }
    if (enter) {
        bool forward = (mode_ == Mode::SearchForward);
        std::string query = search_query_;
        EnterNormal();
        if (!query.empty() && (search_history_.empty() || search_history_.back() != query)) {
            search_history_.push_back(query);
        }
        // An empty query (bare "/<Enter>") repeats the last search, in
        // whichever direction *this* invocation asked for -- matching
        // Vim, that updates last_search_forward_ even though the pattern
        // itself doesn't change.
        if (!query.empty()) last_search_ = query;
        last_search_forward_ = forward;
        if (!last_search_.empty()) PerformSearch(last_search_forward_);
        return;
    }
    if (backspace || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        if (!search_query_.empty()) {
            search_query_.pop_back();
        } else {
            EnterNormal();
        }
        return;
    }
    // Same history-browsing shape as HandleCommandInput's Up/Down, over
    // search_history_ instead of command_history_.
    if (up && !search_history_.empty()) {
        if (search_history_index_ == -1) {
            search_history_saved_ = search_query_;
            search_history_index_ = static_cast<int>(search_history_.size());
        }
        if (search_history_index_ > 0) {
            search_history_index_--;
            search_query_ = search_history_[search_history_index_];
        }
        return;
    }
    if (down && search_history_index_ != -1) {
        search_history_index_++;
        if (search_history_index_ >= static_cast<int>(search_history_.size())) {
            search_history_index_ = -1;
            search_query_ = search_history_saved_;
        } else {
            search_query_ = search_history_[search_history_index_];
        }
        return;
    }
    int cp = GetCharPressed();
    while (cp > 0) {
        if (cp >= 32 && cp < 127) search_query_ += static_cast<char>(cp);
        cp = GetCharPressed();
    }
}

// --- Modal overlays (Prompt/Confirm/Select) -------------------------------

void Editor::BeginPrompt(const std::string &title, const std::string &default_text, int on_done_ref) {
    overlay_previous_mode_ = mode_;
    prompt_title_ = title;
    prompt_input_ = default_text;
    prompt_callback_ref_ = on_done_ref;
    mode_ = Mode::Prompt;
}

void Editor::BeginConfirm(const std::string &message, bool default_yes, int on_done_ref) {
    overlay_previous_mode_ = mode_;
    confirm_message_ = message;
    confirm_default_yes_ = default_yes;
    confirm_callback_ref_ = on_done_ref;
    mode_ = Mode::Confirm;
}

void Editor::BeginSelect(const std::string &title, std::vector<std::string> items, int on_done_ref) {
    overlay_previous_mode_ = mode_;
    select_title_ = title;
    select_items_ = std::move(items);
    select_index_ = 0;
    select_callback_ref_ = on_done_ref;
    mode_ = Mode::Select;
}

void Editor::RestoreFromOverlay() {
    mode_ = overlay_previous_mode_;
    ClampCursor();
}

void Editor::HandlePromptInput() {
    // Same GetKeyPressed()-vs-IsKeyPressed() reasoning as HandleCommandInput.
    bool escape = false, enter = false, backspace = false;
    for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed()) {
        if (key == KEY_ESCAPE) escape = true;
        else if (key == KEY_ENTER) enter = true;
        else if (key == KEY_BACKSPACE) backspace = true;
    }
    if (escape) {
        int ref = prompt_callback_ref_;
        RestoreFromOverlay();
        if (lua_) {
            lua_->CallRef(ref);  // no args -> nil, matches vim.ui.input's cancel behavior
            lua_->UnrefFunction(ref);
        }
        return;
    }
    if (enter) {
        int ref = prompt_callback_ref_;
        std::string text = prompt_input_;
        RestoreFromOverlay();
        if (lua_) {
            lua_->CallRefWithString(ref, text);
            lua_->UnrefFunction(ref);
        }
        return;
    }
    if (backspace || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        if (!prompt_input_.empty()) prompt_input_.pop_back();
        return;
    }
    int cp = GetCharPressed();
    while (cp > 0) {
        if (cp >= 32 && cp < 127) prompt_input_ += static_cast<char>(cp);
        cp = GetCharPressed();
    }
}

void Editor::HandleConfirmInput() {
    bool escape = false, enter = false;
    int cp = 0;
    for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed()) {
        if (key == KEY_ESCAPE) escape = true;
        else if (key == KEY_ENTER) enter = true;
    }
    cp = GetCharPressed();
    while (cp > 0) {
        if (cp == 'y' || cp == 'Y' || cp == 'n' || cp == 'N') break;
        cp = GetCharPressed();
    }
    bool decided = false, result = false;
    if (escape) {
        decided = true;
        result = false;
    } else if (enter) {
        decided = true;
        result = confirm_default_yes_;
    } else if (cp == 'y' || cp == 'Y') {
        decided = true;
        result = true;
    } else if (cp == 'n' || cp == 'N') {
        decided = true;
        result = false;
    }
    if (!decided) return;
    int ref = confirm_callback_ref_;
    RestoreFromOverlay();
    if (lua_) {
        lua_->CallRefWithBool(ref, result);
        lua_->UnrefFunction(ref);
    }
}

void Editor::HandleSelectInput() {
    bool escape = false, enter = false;
    for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed()) {
        if (key == KEY_ESCAPE) escape = true;
        else if (key == KEY_ENTER) enter = true;
    }
    if (escape) {
        int ref = select_callback_ref_;
        RestoreFromOverlay();
        if (lua_) {
            lua_->CallRef(ref);
            lua_->UnrefFunction(ref);
        }
        return;
    }
    if (enter) {
        int ref = select_callback_ref_;
        int idx = select_index_ + 1;  // 1-indexed, matching Lua convention
        RestoreFromOverlay();
        if (lua_) {
            lua_->CallRefWithInt(ref, idx);
            lua_->UnrefFunction(ref);
        }
        return;
    }
    int cp = GetCharPressed();
    while (cp > 0) {
        if ((cp == 'j') && select_index_ + 1 < static_cast<int>(select_items_.size())) select_index_++;
        if (cp == 'k' && select_index_ > 0) select_index_--;
        cp = GetCharPressed();
    }
    if ((IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN)) &&
        select_index_ + 1 < static_cast<int>(select_items_.size())) {
        select_index_++;
    }
    if ((IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP)) && select_index_ > 0) {
        select_index_--;
    }
}

// --- Decorations -----------------------------------------------------------

int Editor::CreateNamespace(const std::string &name) {
    auto it = namespace_ids_.find(name);
    if (it != namespace_ids_.end()) return it->second;
    int id = next_namespace_id_++;
    namespace_ids_[name] = id;
    return id;
}

void Editor::ClearNamespace(int ns) { Buf().decorations.erase(ns); }

int Editor::AddDecoration(int ns, Decoration deco) {
    deco.id = Buf().next_decoration_id++;
    Buf().decorations[ns].push_back(deco);
    return deco.id;
}

const std::unordered_map<int, std::vector<Decoration>> &Editor::CurrentBufferDecorations() const {
    return Buf().decorations;
}

// Buffer-by-id variants (Part VI Phase 27): terminal/Run/REPL output
// streams into a background buffer that isn't necessarily the active
// pane's, so it needs to add decorations (ANSI-color spans) without
// going through Buf()/the active pane the same way SetBufferLinesForLua
// needed to for the lines themselves.
void Editor::ClearNamespaceInBuffer(int buffer_id, int ns) {
    if (buffer_id < 0 || buffer_id >= static_cast<int>(buffers_.size())) return;
    buffers_[buffer_id].decorations.erase(ns);
}

int Editor::AddDecorationToBuffer(int buffer_id, int ns, Decoration deco) {
    if (buffer_id < 0 || buffer_id >= static_cast<int>(buffers_.size())) return 0;
    Buffer &buf = buffers_[buffer_id];
    deco.id = buf.next_decoration_id++;
    buf.decorations[ns].push_back(deco);
    return deco.id;
}

// --- Folding -----------------------------------------------------------

void Editor::ToggleFoldAtCursor() {
    int row = CurPane().cursor.row;
    Fold *innermost = nullptr;
    for (Fold &f : Buf().folds) {
        if (row < f.start_row || row > f.end_row) continue;
        if (!innermost || (f.end_row - f.start_row) < (innermost->end_row - innermost->start_row)) innermost = &f;
    }
    if (innermost) innermost->closed = !innermost->closed;
}

void Editor::CreateFold(int start_row, int end_row, bool closed, const std::string &provider) {
    if (start_row > end_row) std::swap(start_row, end_row);
    start_row = std::max(0, start_row);
    end_row = std::min(end_row, Buf().LineCount() - 1);
    if (start_row >= end_row) return;  // a fold covering <2 lines isn't meaningful
    Buf().folds.push_back({start_row, end_row, closed, provider});
}

void Editor::ClearFoldsFromProvider(const std::string &provider) {
    auto &folds = Buf().folds;
    folds.erase(std::remove_if(folds.begin(), folds.end(), [&](const Fold &f) { return f.provider == provider; }),
                folds.end());
}

bool Editor::IsRowHiddenByFold(int row, int *fold_start_row) const {
    for (const Fold &f : Buf().folds) {
        if (f.closed && row > f.start_row && row <= f.end_row) {
            if (fold_start_row) *fold_start_row = f.start_row;
            return true;
        }
    }
    return false;
}

// --- Sidebar/panel widget ------------------------------------------------

SidebarInstance *Editor::FindSidebarMut(int id) {
    for (auto &sb : sidebars_) {
        if (sb.id == id) return &sb;
    }
    return nullptr;
}

const SidebarInstance *Editor::FindSidebar(int id) const {
    for (const auto &sb : sidebars_) {
        if (sb.id == id) return &sb;
    }
    return nullptr;
}

void Editor::SetSidebarOnKey(int id, int lua_ref) {
    if (SidebarInstance *sb = FindSidebarMut(id)) sb->on_key_ref = lua_ref;
}

std::string Editor::SidebarCursorWidgetId(int id) const {
    const SidebarInstance *sb = FindSidebar(id);
    if (!sb) return "";
    std::vector<SidebarLine> lines = FlattenSidebar(id);
    if (sidebar_cursor_ < 0 || sidebar_cursor_ >= static_cast<int>(lines.size())) return "";
    const SidebarLine &line = lines[sidebar_cursor_];
    if (line.kind != SidebarLine::Kind::Widget) return "";
    return sb->sections[line.section_index].widgets[line.widget_index].id;
}

int Editor::CreateSidebar(const std::string &title, const std::string &position, int size) {
    SidebarInstance sb;
    sb.id = next_sidebar_id_++;
    sb.title = title;
    sb.position = position;
    sb.size = size;
    sidebars_.push_back(sb);
    return sb.id;
}

void Editor::SetSidebarSections(int id, std::vector<SidebarSection> sections) {
    if (SidebarInstance *sb = FindSidebarMut(id)) sb->sections = std::move(sections);
}

void Editor::OpenSidebar(int id, bool focus) {
    SidebarInstance *sb = FindSidebarMut(id);
    if (!sb) return;
    sb->open = true;
    if (focus) {
        overlay_previous_mode_ = mode_;
        focused_sidebar_id_ = id;
        sidebar_cursor_ = 0;
        mode_ = Mode::Sidebar;
    }
}

void Editor::CloseSidebar(int id) {
    SidebarInstance *sb = FindSidebarMut(id);
    if (!sb) return;
    sb->open = false;
    if (focused_sidebar_id_ == id) {
        focused_sidebar_id_ = 0;
        if (mode_ == Mode::Sidebar) RestoreFromOverlay();
    }
}

void Editor::ToggleSidebar(int id, bool focus) {
    if (IsSidebarOpen(id)) {
        CloseSidebar(id);
    } else {
        OpenSidebar(id, focus);
    }
}

bool Editor::IsSidebarOpen(int id) const {
    const SidebarInstance *sb = FindSidebar(id);
    return sb && sb->open;
}

std::vector<SidebarLine> Editor::FlattenSidebar(int id) const {
    std::vector<SidebarLine> out;
    const SidebarInstance *sb = FindSidebar(id);
    if (!sb) return out;
    for (int si = 0; si < static_cast<int>(sb->sections.size()); si++) {
        const SidebarSection &sec = sb->sections[si];
        if (!sec.title.empty()) {
            SidebarLine line;
            line.kind = SidebarLine::Kind::SectionHeader;
            line.section_index = si;
            line.text = std::string(sec.collapsed ? "+ " : "- ") + sec.title;
            out.push_back(line);
        }
        if (sec.collapsed) continue;
        for (int wi = 0; wi < static_cast<int>(sec.widgets.size()); wi++) {
            const SidebarWidget &w = sec.widgets[wi];
            SidebarLine line;
            line.kind = SidebarLine::Kind::Widget;
            line.section_index = si;
            line.widget_index = wi;
            line.text = (w.icon.empty() ? "  " : "  " + w.icon + " ") + w.text;
            line.hl = w.hl;
            out.push_back(line);
        }
    }
    return out;
}

void Editor::HandleSidebarInput() {
    std::vector<SidebarLine> lines = FlattenSidebar(focused_sidebar_id_);
    bool escape = false, enter = false;
    for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed()) {
        if (key == KEY_ESCAPE) escape = true;
        else if (key == KEY_ENTER) enter = true;
    }
    if (escape) {
        int id = focused_sidebar_id_;
        RestoreFromOverlay();
        CloseSidebar(id);
        return;
    }
    if (enter) {
        if (sidebar_cursor_ >= 0 && sidebar_cursor_ < static_cast<int>(lines.size())) {
            const SidebarLine &line = lines[sidebar_cursor_];
            SidebarInstance *sb = FindSidebarMut(focused_sidebar_id_);
            if (sb && line.kind == SidebarLine::Kind::SectionHeader) {
                sb->sections[line.section_index].collapsed = !sb->sections[line.section_index].collapsed;
                int max_idx = static_cast<int>(FlattenSidebar(focused_sidebar_id_).size()) - 1;
                sidebar_cursor_ = std::min(sidebar_cursor_, std::max(0, max_idx));
            } else if (sb && line.kind == SidebarLine::Kind::Widget) {
                int ref = sb->sections[line.section_index].widgets[line.widget_index].on_click_ref;
                if (ref != 0 && lua_) lua_->CallRef(ref);
            }
        }
        return;
    }
    int cp = GetCharPressed();
    while (cp > 0) {
        if (cp == 'j' && sidebar_cursor_ + 1 < static_cast<int>(lines.size())) sidebar_cursor_++;
        else if (cp == 'k' && sidebar_cursor_ > 0) sidebar_cursor_--;
        else if (cp == 'q') {
            int id = focused_sidebar_id_;
            RestoreFromOverlay();
            CloseSidebar(id);
            return;
        } else if (lua_) {
            const SidebarInstance *sb = FindSidebar(focused_sidebar_id_);
            if (sb && sb->on_key_ref != 0 && cp >= 32 && cp < 127) {
                lua_->CallRefWithString(sb->on_key_ref, std::string(1, static_cast<char>(cp)));
            }
        }
        cp = GetCharPressed();
    }
    if ((IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN)) &&
        sidebar_cursor_ + 1 < static_cast<int>(lines.size())) {
        sidebar_cursor_++;
    }
    if ((IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP)) && sidebar_cursor_ > 0) {
        sidebar_cursor_--;
    }
}

// --- Icons (NVIM_PARITY_PLAN.md Part II Phase 10) -------------------------

std::string IconForFilename(const std::string &name) {
    // Full-name special cases checked before falling back to extension.
    static const std::unordered_map<std::string, std::string> kByName = {
        {"Makefile", "M"},     {"makefile", "M"},   {"CMakeLists.txt", "M"},
        {"Dockerfile", "D"},   {".gitignore", "G"}, {".gitmodules", "G"},
        {"README.md", "R"},    {"README.org", "R"}, {"README", "R"},
        {"LICENSE", "!"},      {".env", "*"},
    };
    auto by_name = kByName.find(name);
    if (by_name != kByName.end()) return by_name->second;

    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == name.size() - 1) return "-";
    std::string ext = name.substr(dot + 1);
    for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    static const std::unordered_map<std::string, std::string> kByExt = {
        {"c", "C"},    {"h", "H"},    {"cpp", "C"},  {"cc", "C"},   {"cxx", "C"},  {"hpp", "H"},
        {"lua", "L"},  {"py", "P"},   {"js", "J"},   {"ts", "T"},   {"jsx", "J"},  {"tsx", "T"},
        {"rs", "R"},   {"go", "G"},   {"java", "J"}, {"rb", "R"},   {"sh", "$"},   {"bash", "$"},
        {"zsh", "$"},  {"md", "M"},   {"org", "O"},  {"txt", "T"},  {"json", "{"}, {"yaml", "Y"},
        {"yml", "Y"},  {"toml", "T"}, {"xml", "X"},  {"html", "H"}, {"css", "S"},  {"sql", "Q"},
        {"git", "G"},  {"png", "I"},  {"jpg", "I"},  {"jpeg", "I"}, {"gif", "I"},  {"svg", "I"},
        {"pdf", "F"},  {"zip", "Z"},  {"tar", "Z"},  {"gz", "Z"},
    };
    auto by_ext = kByExt.find(ext);
    if (by_ext != kByExt.end()) return by_ext->second;
    return "-";
}

// --- Fuzzy picker ----------------------------------------------------------

int FuzzyScore(const std::string &str, const std::string &query, std::vector<int> *positions) {
    if (positions) positions->clear();
    if (query.empty()) return 0;
    bool smart_case = std::any_of(query.begin(), query.end(), [](unsigned char c) { return std::isupper(c); });
    auto norm = [&](char c) { return smart_case ? c : static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    size_t si = 0, qi = 0;
    std::vector<int> pos;
    int score = 0;
    int consecutive = 0;
    while (si < str.size() && qi < query.size()) {
        if (norm(str[si]) == norm(query[qi])) {
            bool boundary = si == 0 || str[si - 1] == ' ' || str[si - 1] == '-' || str[si - 1] == '_' ||
                             str[si - 1] == '/' || str[si - 1] == '.';
            score += (consecutive > 0) ? (15 + 5 * consecutive) : 1;
            if (boundary) score += 10;
            consecutive++;
            pos.push_back(static_cast<int>(si));
            qi++;
        } else {
            consecutive = 0;
        }
        si++;
    }
    if (qi < query.size()) return -1;  // not every query char matched, in order
    int span = pos.empty() ? 0 : (pos.back() - pos.front() + 1);
    score -= (span - static_cast<int>(query.size()));
    score -= static_cast<int>(str.size() * 0.01);
    if (positions) *positions = std::move(pos);
    return score;
}

void Editor::OpenPicker(const std::string &title, std::vector<PickerItem> items, int on_select_ref,
                         int on_query_change_ref, int on_key_ref) {
    overlay_previous_mode_ = mode_;
    picker_open_ = true;
    picker_title_ = title;
    picker_query_.clear();
    picker_items_ = std::move(items);
    picker_selected_ = 0;
    picker_on_select_ref_ = on_select_ref;
    picker_on_query_change_ref_ = on_query_change_ref;
    picker_on_key_ref_ = on_key_ref;
    mode_ = Mode::Picker;
}

void Editor::ClosePicker() {
    picker_open_ = false;
    if (mode_ == Mode::Picker) RestoreFromOverlay();
}

void Editor::ClosePickerDiscardingCallbacks() {
    int select_ref = picker_on_select_ref_;
    int query_ref = picker_on_query_change_ref_;
    int key_ref = picker_on_key_ref_;
    ClosePicker();
    if (!lua_) return;
    if (select_ref != 0) lua_->UnrefFunction(select_ref);
    if (query_ref != 0) lua_->UnrefFunction(query_ref);
    if (key_ref != 0) lua_->UnrefFunction(key_ref);
}

void Editor::SetPickerItems(std::vector<PickerItem> items) {
    picker_items_ = std::move(items);
    int max_idx = static_cast<int>(PickerFilteredResults().size()) - 1;
    picker_selected_ = std::max(0, std::min(picker_selected_, max_idx));
}

std::vector<PickerItem> Editor::PickerFilteredResults() const {
    if (picker_query_.empty()) return picker_items_;
    std::vector<std::pair<int, const PickerItem *>> scored;
    for (const PickerItem &it : picker_items_) {
        int score = FuzzyScore(it.display, picker_query_, nullptr);
        if (score >= 0) scored.emplace_back(score, &it);
    }
    std::stable_sort(scored.begin(), scored.end(),
                      [](const auto &a, const auto &b) { return a.first > b.first; });
    std::vector<PickerItem> out;
    out.reserve(scored.size());
    for (auto &[score, item] : scored) out.push_back(*item);
    return out;
}

void Editor::HandlePickerInput() {
    bool escape = false, enter = false, backspace = false;
    for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed()) {
        if (key == KEY_ESCAPE) escape = true;
        else if (key == KEY_ENTER) enter = true;
        else if (key == KEY_BACKSPACE) backspace = true;
    }
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (escape) {
        int ref = picker_on_select_ref_;
        int qref = picker_on_query_change_ref_;
        int kref = picker_on_key_ref_;
        ClosePicker();
        if (lua_) {
            lua_->CallRef(ref);
            lua_->UnrefFunction(ref);
            if (qref != 0) lua_->UnrefFunction(qref);
            if (kref != 0) lua_->UnrefFunction(kref);
        }
        return;
    }
    if (enter) {
        std::vector<PickerItem> results = PickerFilteredResults();
        bool has_selection = picker_selected_ >= 0 && picker_selected_ < static_cast<int>(results.size());
        std::string data = has_selection ? results[picker_selected_].data : std::string();
        int ref = picker_on_select_ref_;
        int qref = picker_on_query_change_ref_;
        int kref = picker_on_key_ref_;
        ClosePicker();
        if (lua_) {
            if (has_selection) {
                lua_->CallRefWithString(ref, data);
            } else {
                lua_->CallRef(ref);  // no results to choose from: same as cancel
            }
            lua_->UnrefFunction(ref);
            if (qref != 0) lua_->UnrefFunction(qref);
            if (kref != 0) lua_->UnrefFunction(kref);
        }
        return;
    }
    // Ctrl+<letter> shortcuts (e.g. mep.projects()'s Ctrl-A for "add
    // current directory"), for any letter besides N/P below (already
    // reserved for next/prev) -- only if this picker registered a
    // callback. IsKeyPressed rather than the GetKeyPressed() queue drained
    // above matches this function's existing Ctrl-N/Ctrl-P checks.
    if (ctrl && picker_on_key_ref_ != 0 && lua_) {
        for (int key = KEY_A; key <= KEY_Z; key++) {
            if (key == KEY_N || key == KEY_P || !IsKeyPressed(key)) continue;
            lua_->CallRefWithString(picker_on_key_ref_, std::string(1, static_cast<char>('a' + (key - KEY_A))));
            while (GetCharPressed() > 0) {
            }
            return;
        }
    }
    bool query_changed = false;
    if (backspace || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        if (!picker_query_.empty()) {
            picker_query_.pop_back();
            picker_selected_ = 0;
            query_changed = true;
        }
    }
    if ((ctrl && IsKeyPressed(KEY_N)) || IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN)) {
        int n = static_cast<int>(PickerFilteredResults().size());
        if (picker_selected_ + 1 < n) picker_selected_++;
    }
    if ((ctrl && IsKeyPressed(KEY_P)) || IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP)) {
        if (picker_selected_ > 0) picker_selected_--;
    }
    int cp = GetCharPressed();
    while (cp > 0) {
        if (cp >= 32 && cp < 127) {
            picker_query_ += static_cast<char>(cp);
            picker_selected_ = 0;
            query_changed = true;
        }
        cp = GetCharPressed();
    }
    if (query_changed && lua_ && picker_on_query_change_ref_ != 0) {
        lua_->CallRefWithString(picker_on_query_change_ref_, picker_query_);
    }
}

// --- Whichkey (NVIM_PARITY_PLAN.md Part II Phase 11) -----------------------

void Editor::RegisterWhichKey(const std::string &sequence, const std::string &description, int lua_ref) {
    whichkey_bindings_.push_back({sequence, description, lua_ref});
}

void Editor::TriggerWhichKey() {
    whichkey_prefix_.clear();
    overlay_previous_mode_ = mode_;
    mode_ = Mode::WhichKey;
}

std::vector<std::pair<std::string, std::string>> Editor::WhichKeyMatches() const {
    std::vector<std::pair<std::string, std::string>> out;
    for (const WhichKeyBinding &b : whichkey_bindings_) {
        if (b.sequence.size() >= whichkey_prefix_.size() &&
            b.sequence.compare(0, whichkey_prefix_.size(), whichkey_prefix_) == 0) {
            out.push_back({b.sequence.substr(whichkey_prefix_.size()), b.description});
        }
    }
    return out;
}

void Editor::HandleWhichKeyInput() {
    for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed()) {
        if (key == KEY_ESCAPE) {
            RestoreFromOverlay();
            return;
        }
    }
    int cp = GetCharPressed();
    if (cp <= 0) return;
    if (cp < 32 || cp > 127) return;
    whichkey_prefix_ += static_cast<char>(cp);

    // Exact match: fire it and leave, regardless of any longer sequences
    // that also start with this prefix (mirrors real whichkey -- a leaf
    // fires the moment its full sequence is typed).
    for (const WhichKeyBinding &b : whichkey_bindings_) {
        if (b.sequence == whichkey_prefix_) {
            int ref = b.lua_ref;
            RestoreFromOverlay();
            if (lua_) lua_->CallRef(ref);
            return;
        }
    }
    // No binding starts with this prefix: nothing to descend into, cancel.
    if (WhichKeyMatches().empty()) {
        status_message_ = "No such group: " + whichkey_prefix_;
        RestoreFromOverlay();
    }
}

// --- Hints (NVIM_PARITY_PLAN.md Part III Phase 13) -------------------------

namespace {
// Home-row-first label pool, mirroring hop.nvim/leap.nvim/flash.nvim's
// convention: labels for the closest/most-likely targets use the easiest-
// to-reach keys. Single-char labels cover the first 26 matches; beyond
// that, two-char combinations (pool[i/26] + pool[i%26]) cover up to 702.
const char kHintLabelPool[] = "asdfghjklqwertyuiopzxcvbnm";
constexpr int kHintPoolSize = 26;

std::string HintLabelForIndex(int i) {
    if (i < kHintPoolSize) return std::string(1, kHintLabelPool[i]);
    int rest = i - kHintPoolSize;
    return std::string(1, kHintLabelPool[(rest / kHintPoolSize) % kHintPoolSize]) +
           std::string(1, kHintLabelPool[rest % kHintPoolSize]);
}
}  // namespace

void Editor::BeginHints() {
    hint_matches_.clear();
    hint_typed_.clear();
    overlay_previous_mode_ = mode_;
    mode_ = Mode::HintChar;
}

void Editor::HandleHintCharInput() {
    for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed()) {
        if (key == KEY_ESCAPE) {
            RestoreFromOverlay();
            return;
        }
    }
    int cp = GetCharPressed();
    if (cp <= 0) return;
    if (cp < 32 || cp > 127) return;
    char target = static_cast<char>(cp);

    const Buffer &buf = Buf();
    const Pane &pane = CurPane();
    int last_row = std::min(pane.scroll_row + std::max(1, pane.visible_lines), buf.LineCount());
    std::vector<HintMatch> matches;
    for (int row = pane.scroll_row; row < last_row; row++) {
        const std::string &line = buf.lines[row];
        for (int col = 0; col < static_cast<int>(line.size()); col++) {
            if (line[col] == target) matches.push_back({row, col, ""});
        }
    }
    if (matches.empty()) {
        status_message_ = "No matches for '" + std::string(1, target) + "'";
        RestoreFromOverlay();
        return;
    }
    if (matches.size() == 1) {
        CurPane().cursor = {matches[0].row, matches[0].col};
        ClampCursor();
        RestoreFromOverlay();
        return;
    }
    for (size_t i = 0; i < matches.size(); i++) matches[i].label = HintLabelForIndex(static_cast<int>(i));
    hint_matches_ = std::move(matches);
    mode_ = Mode::HintLabel;
}

void Editor::HandleHintLabelInput() {
    for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed()) {
        if (key == KEY_ESCAPE) {
            RestoreFromOverlay();
            return;
        }
    }
    int cp = GetCharPressed();
    if (cp <= 0) return;
    if (cp < 32 || cp > 127) return;
    hint_typed_ += static_cast<char>(cp);

    for (const HintMatch &m : hint_matches_) {
        if (m.label == hint_typed_) {
            CursorPos target{m.row, m.col};
            RestoreFromOverlay();
            CurPane().cursor = target;
            ClampCursor();
            return;
        }
    }
    bool any_prefix = std::any_of(hint_matches_.begin(), hint_matches_.end(), [&](const HintMatch &m) {
        return m.label.size() >= hint_typed_.size() && m.label.compare(0, hint_typed_.size(), hint_typed_) == 0;
    });
    if (!any_prefix) RestoreFromOverlay();
}

// --- Completion engine (NVIM_PARITY_PLAN.md Part V Phase 22) --------------

void Editor::UpdateCompletionPopup() {
    if (completion_source_ref_ == 0 || !lua_) {
        completion_open_ = false;
        return;
    }
    const CursorPos &cursor = CurPane().cursor;
    const std::string &line = Buf().lines[cursor.row];
    int start = cursor.col;
    while (start > 0 && (std::isalnum(static_cast<unsigned char>(line[start - 1])) || line[start - 1] == '_')) start--;
    std::string prefix = line.substr(start, cursor.col - start);
    if (prefix.size() < 2) {
        completion_open_ = false;
        return;
    }
    std::vector<std::string> words;
    if (!lua_->CallRefWithStringForStrings(completion_source_ref_, prefix, &words) || words.empty()) {
        completion_open_ = false;
        return;
    }
    completion_items_.clear();
    for (const std::string &w : words) completion_items_.push_back({w, w});
    completion_selected_ = 0;
    completion_word_start_col_ = start;
    completion_open_ = true;
}

void Editor::CompletionNext() {
    if (completion_selected_ + 1 < static_cast<int>(completion_items_.size())) completion_selected_++;
}

void Editor::CompletionPrev() {
    if (completion_selected_ > 0) completion_selected_--;
}

void Editor::AcceptCompletion() {
    if (completion_selected_ < 0 || completion_selected_ >= static_cast<int>(completion_items_.size())) {
        completion_open_ = false;
        return;
    }
    const std::string &word = completion_items_[completion_selected_].data;
    CursorPos &cursor = CurPane().cursor;
    std::string &line = Buf().lines[cursor.row];
    PushUndo();
    line.erase(completion_word_start_col_, cursor.col - completion_word_start_col_);
    line.insert(completion_word_start_col_, word);
    cursor.col = completion_word_start_col_ + static_cast<int>(word.size());
    Buf().modified = true;
    completion_open_ = false;
}

// --- Command-line completion (`:` command bar) -----------------------------

namespace {

// Every literal name ExecuteCommandLine's dispatch chain recognizes after
// the range-command special cases (:s :g :v :d :y :m :t/:co, which take
// inline syntax rather than a plain "name<space>args" shape and aren't
// useful to offer here). Kept as a plain list rather than derived from the
// dispatch chain itself since that chain is an if/else ladder, not a table.
const std::vector<std::string> &BuiltinCommandNames() {
    static const std::vector<std::string> kNames = {
        "w", "write", "wa", "wall", "q", "quit", "q!", "quit!", "qa", "qall", "qa!", "qall!",
        "wq", "x", "wqa", "xa", "wqall", "xall", "e", "edit", "split", "sp", "vsplit", "vs",
        "terminal", "term",
        "close", "tabnew", "tabdelete", "tabclose", "tabnext", "tabn", "tabprevious", "tabp", "tabN",
        "set", "normal", "norm", "normal!", "norm!", "MepNotifyClear", "MepNotifyDismiss",
        "MepNotifyPanel", "MepLayout", "MepScratch", "MepZen", "colorscheme", "colo", "lua", "source",
    };
    return kNames;
}

// Commands whose last argument is a filesystem path, so completion after
// the command name should list directory entries rather than command names.
bool CommandTakesFileArg(const std::string &name) {
    static const std::vector<std::string> kFileCommands = {
        "w", "write", "wq", "x", "wqa", "xa", "wqall", "xall", "e", "edit",
        "split", "sp", "vsplit", "vs", "tabnew", "source",
    };
    return std::find(kFileCommands.begin(), kFileCommands.end(), name) != kFileCommands.end();
}

}  // namespace

// Recomputed only on Tab (unlike the Insert-mode popup, which recomputes on
// every keystroke) -- command names and directory listings are cheap, small
// sets that don't need to auto-narrow as you type the way word completion
// does. A single match completes in place with no popup; multiple matches
// splice in their longest common prefix (classic shell-style partial
// completion) and open the popup so Ctrl-N/Ctrl-P/Enter can pick one.
void Editor::UpdateCmdlineCompletion() {
    cmdline_completion_open_ = false;
    cmdline_completion_items_.clear();

    std::vector<std::string> candidates;
    int word_start = 0;

    size_t sp = command_line_.find(' ');
    if (sp == std::string::npos) {
        const std::string &prefix = command_line_;
        if (prefix.empty()) return;
        for (const std::string &n : BuiltinCommandNames()) {
            if (n.compare(0, prefix.size(), prefix) == 0) candidates.push_back(n);
        }
        for (const std::string &n : LuaCommandNames()) {
            if (n.compare(0, prefix.size(), prefix) == 0) candidates.push_back(n);
        }
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
        word_start = 0;
    } else {
        std::string name = command_line_.substr(0, sp);
        if (!CommandTakesFileArg(name)) return;
        size_t token_start = command_line_.find_last_of(' ') + 1;
        std::string token = command_line_.substr(token_start);
        size_t slash = token.find_last_of('/');
        std::string dir = (slash == std::string::npos) ? "" : token.substr(0, slash + 1);
        std::string base_prefix = (slash == std::string::npos) ? token : token.substr(slash + 1);
        std::string list_dir = dir.empty() ? "." : dir;

        std::vector<DirEntry> entries = ListDirectory(list_dir);
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                      [&](const DirEntry &e) {
                                          return e.name.compare(0, base_prefix.size(), base_prefix) != 0;
                                      }),
                      entries.end());
        std::sort(entries.begin(), entries.end(), [](const DirEntry &a, const DirEntry &b) {
            if (a.is_dir != b.is_dir) return a.is_dir;
            return a.name < b.name;
        });
        for (const DirEntry &e : entries) candidates.push_back(dir + e.name + (e.is_dir ? "/" : ""));
        word_start = static_cast<int>(token_start);
    }

    if (candidates.empty()) return;

    if (candidates.size() == 1) {
        command_line_ = command_line_.substr(0, word_start) + candidates[0];
        return;
    }

    std::string common = candidates[0];
    for (size_t i = 1; i < candidates.size(); i++) {
        size_t j = 0;
        while (j < common.size() && j < candidates[i].size() && common[j] == candidates[i][j]) j++;
        common.resize(j);
    }
    command_line_ = command_line_.substr(0, word_start) + common;

    for (const std::string &c : candidates) cmdline_completion_items_.push_back({c, c});
    cmdline_completion_selected_ = 0;
    cmdline_completion_word_start_ = word_start;
    cmdline_completion_open_ = true;
}

void Editor::CmdlineCompletionNext() {
    if (cmdline_completion_selected_ + 1 < static_cast<int>(cmdline_completion_items_.size())) {
        cmdline_completion_selected_++;
    }
}

void Editor::CmdlineCompletionPrev() {
    if (cmdline_completion_selected_ > 0) cmdline_completion_selected_--;
}

void Editor::AcceptCmdlineCompletion() {
    if (cmdline_completion_selected_ < 0 ||
        cmdline_completion_selected_ >= static_cast<int>(cmdline_completion_items_.size())) {
        cmdline_completion_open_ = false;
        return;
    }
    const std::string &item = cmdline_completion_items_[cmdline_completion_selected_].data;
    command_line_ = command_line_.substr(0, cmdline_completion_word_start_) + item;
    cmdline_completion_open_ = false;
}

// --- Mode transitions ----------------------------------------------------

void Editor::EnterInsert() {
    mode_ = Mode::Insert;
    status_message_.clear();
}

void Editor::EnterNormal() {
    // PushUndo() (and the change_epoch_ bump inside it) fires once at
    // Insert-mode *entry*, capturing the pre-edit state as the undo
    // checkpoint -- matching vim's own "one insert session = one undo
    // step" semantics, but leaving change_epoch_ stale for the entire
    // duration of an insert session: a poller (mep.on_buffer_changed)
    // watching change_epoch_ would see the buffer as unchanged from the
    // moment typing starts until the *next* distinct edit begins,
    // completely missing whatever was just typed. Bump the epoch again
    // here (not PushUndo() -- that would record a second, spurious undo
    // checkpoint) so leaving Insert mode also counts as a change.
    if (mode_ == Mode::Insert) change_epoch_++;
    // Remember the selection being left so `gv` can restore it later --
    // must happen before mode_ is overwritten below. Vim's `gv` also
    // restores Visual Block as a block; mep's last-visual memory only has
    // room for a linewise/charwise bool (has_last_visual_/
    // last_visual_linewise_), so a block selection is remembered as a
    // plain charwise one covering the same anchor/cursor -- a known,
    // small degradation versus restoring the actual block shape.
    if (mode_ == Mode::Visual || mode_ == Mode::VisualLine || mode_ == Mode::VisualBlock) {
        has_last_visual_ = true;
        last_visual_linewise_ = (mode_ == Mode::VisualLine);
        last_visual_anchor_ = CurPane().visual_anchor;
        last_visual_cursor_ = CurPane().cursor;
    }
    mode_ = Mode::Normal;
    replace_mode_ = false;
    replace_overwritten_.clear();
    pending_op_ = 0;
    pending_op_count_ = 0;
    pending_g_ = false;
    pending_find_ = 0;
    pending_mark_jump_ = 0;
    pending_mark_set_ = false;
    pending_textobj_scope_ = 0;
    pending_ctrl_w_ = false;
    pending_z_ = false;
    pending_count_ = 0;
    awaiting_register_name_ = false;
    pending_register_ = 0;
    pending_register_append_ = false;
    pending_macro_record_ = false;
    awaiting_macro_play_ = false;
    pending_replace_ = false;
    pending_replace_count_ = 0;
    // last_find_cmd_/last_find_char_ deliberately NOT reset here: ; and ,
    // should keep working after a trip through Insert mode or another
    // buffer, matching Vim.
    command_line_.clear();
    ClampCursor();
}

void Editor::EnterVisual(bool linewise) {
    mode_ = linewise ? Mode::VisualLine : Mode::Visual;
    CurPane().visual_anchor = CurPane().cursor;
}

void Editor::EnterVisualBlock() {
    mode_ = Mode::VisualBlock;
    CurPane().visual_anchor = CurPane().cursor;
    block_to_eol_ = false;
}

void Editor::EnterCommand() {
    mode_ = Mode::Command;
    command_line_.clear();
    cmd_history_index_ = -1;
}

void Editor::EnterSearch(bool forward) {
    mode_ = forward ? Mode::SearchForward : Mode::SearchBackward;
    search_query_.clear();
    search_history_index_ = -1;
}

// --- Search --------------------------------------------------------------

namespace {
std::string ToLowerAscii(const std::string &s) {
    std::string out = s;
    for (char &c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
}  // namespace

// `:set ignorecase`-aware wrappers around find/rfind -- when it's off
// (the default), these are exactly find/rfind with no extra cost; when on,
// both sides are lowercased first (ASCII only, matching the rest of mep's
// plain-substring search -- no locale/Unicode case folding).
size_t Editor::CiFind(const std::string &hay, const std::string &needle, size_t from) const {
    if (!ignore_case_) return hay.find(needle, from);
    return ToLowerAscii(hay).find(ToLowerAscii(needle), from);
}

size_t Editor::CiRfind(const std::string &hay, const std::string &needle, size_t limit) const {
    if (!ignore_case_) return hay.rfind(needle, limit);
    return ToLowerAscii(hay).rfind(ToLowerAscii(needle), limit);
}

bool Editor::SearchOnce(const std::string &pattern, CursorPos from, bool forward, CursorPos *result) const {
    if (pattern.empty()) return false;
    int n = Buf().LineCount();
    if (forward) {
        for (int r = from.row; r < n; r++) {
            const std::string &line = Buf().lines[r];
            size_t start = (r == from.row) ? static_cast<size_t>(from.col) + 1 : 0;
            if (start > line.size()) continue;
            size_t pos = CiFind(line, pattern, start);
            if (pos != std::string::npos) {
                *result = {r, static_cast<int>(pos)};
                return true;
            }
        }
        return false;
    }
    for (int r = from.row; r >= 0; r--) {
        const std::string &line = Buf().lines[r];
        int limit = (r == from.row) ? from.col - 1 : static_cast<int>(line.size());
        if (limit < 0) continue;
        size_t pos = CiRfind(line, pattern, static_cast<size_t>(limit));
        if (pos != std::string::npos) {
            *result = {r, static_cast<int>(pos)};
            return true;
        }
    }
    return false;
}

bool Editor::FindNext(const std::string &pattern, bool forward, CursorPos *result, bool *wrapped) const {
    *wrapped = false;
    CursorPos cursor = CurPane().cursor;
    if (SearchOnce(pattern, cursor, forward, result)) return true;
    if (!wrapscan_) return false;
    CursorPos edge = forward ? CursorPos{0, -1} : CursorPos{Buf().LineCount() - 1, LineLen(Buf().LineCount() - 1)};
    if (SearchOnce(pattern, edge, forward, result)) {
        *wrapped = true;
        return true;
    }
    return false;
}

void Editor::PerformSearch(bool forward) {
    CursorPos result;
    bool wrapped = false;
    if (FindNext(last_search_, forward, &result, &wrapped)) {
        RecordJumpFrom(CurPane().cursor);
        CurPane().cursor = result;
        ClampCursor();
        status_message_ = wrapped ? (forward ? "search hit BOTTOM, continuing at TOP"
                                              : "search hit TOP, continuing at BOTTOM")
                                   : "";
    } else {
        status_message_ = "E486: Pattern not found: " + last_search_;
    }
}

void Editor::SearchWordUnderCursor(bool forward) {
    CursorPos start, end;
    WordObjectRange(CurPane().cursor, false, false, &start, &end);
    if (start.row != end.row || end.col <= start.col) {
        status_message_ = "E348: No string under cursor";
        return;
    }
    last_search_ = Buf().lines[start.row].substr(start.col, end.col - start.col);
    last_search_forward_ = forward;
    PerformSearch(forward);
}

// --- Motions ---------------------------------------------------------------

// word motion: stops at the boundary between whitespace, "word" chars
// (alnum/'_'), and "punct" chars (everything else) -- so "foo.bar" is
// three words. Crosses line boundaries at start/end of line, stopping on
// a blank line as a word of its own, matching Vim.
CursorPos Editor::MoveWordForward(CursorPos from) const {
    int row = from.row, col = from.col;
    int nrows = Buf().LineCount();
    if (col < LineLen(row)) {
        CharClass start_class = ClassOf(Buf().lines[row][col]);
        if (start_class != CharClass::Space) {
            while (col < LineLen(row) && ClassOf(Buf().lines[row][col]) == start_class) col++;
        }
    }
    while (col >= LineLen(row) || ClassOf(Buf().lines[row][col]) == CharClass::Space) {
        if (col >= LineLen(row)) {
            if (row + 1 >= nrows) return {row, LineLen(row)};
            row++;
            col = 0;
            if (LineLen(row) == 0) return {row, 0};  // blank line: a word of its own
        } else {
            col++;
        }
    }
    return {row, col};
}

CursorPos Editor::MoveWordBackward(CursorPos from) const {
    int row = from.row, col = from.col;
    if (col > 0) {
        col--;
    } else if (row > 0) {
        row--;
        col = LineLen(row) - 1;
        if (col < 0) return {row, 0};  // landed on a blank line: that's the stop
    } else {
        return {0, 0};
    }
    while (true) {
        if (col < 0) {
            if (row == 0) return {0, 0};
            row--;
            col = LineLen(row) - 1;
            if (LineLen(row) == 0) return {row, 0};
            continue;
        }
        if (ClassOf(Buf().lines[row][col]) == CharClass::Space) {
            col--;
            continue;
        }
        break;
    }
    if (col < 0) return {row, 0};
    CharClass cc = ClassOf(Buf().lines[row][col]);
    while (col > 0 && ClassOf(Buf().lines[row][col - 1]) == cc) col--;
    return {row, col};
}

// WORD motion: only whitespace separates words (no word/punct distinction).
CursorPos Editor::MoveWORDForward(CursorPos from) const {
    int row = from.row, col = from.col;
    int nrows = Buf().LineCount();
    if (col < LineLen(row) && !std::isspace(static_cast<unsigned char>(Buf().lines[row][col]))) {
        while (col < LineLen(row) && !std::isspace(static_cast<unsigned char>(Buf().lines[row][col]))) col++;
    }
    while (col >= LineLen(row) || std::isspace(static_cast<unsigned char>(Buf().lines[row][col]))) {
        if (col >= LineLen(row)) {
            if (row + 1 >= nrows) return {row, LineLen(row)};
            row++;
            col = 0;
            if (LineLen(row) == 0) return {row, 0};
        } else {
            col++;
        }
    }
    return {row, col};
}

CursorPos Editor::MoveWORDBackward(CursorPos from) const {
    int row = from.row, col = from.col;
    if (col > 0) {
        col--;
    } else if (row > 0) {
        row--;
        col = LineLen(row) - 1;
        if (col < 0) return {row, 0};
    } else {
        return {0, 0};
    }
    while (true) {
        if (col < 0) {
            if (row == 0) return {0, 0};
            row--;
            col = LineLen(row) - 1;
            if (LineLen(row) == 0) return {row, 0};
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(Buf().lines[row][col]))) {
            col--;
            continue;
        }
        break;
    }
    if (col < 0) return {row, 0};
    while (col > 0 && !std::isspace(static_cast<unsigned char>(Buf().lines[row][col - 1]))) col--;
    return {row, col};
}

// e/E: end of word/WORD forward, inclusive as an operator target.
CursorPos Editor::MoveWordEndForward(CursorPos from, bool big) const {
    int row = from.row, col = from.col + 1;
    int nrows = Buf().LineCount();
    while (true) {
        if (col >= LineLen(row)) {
            if (row + 1 >= nrows) return {row, std::max(0, LineLen(row) - 1)};
            row++;
            col = 0;
            continue;
        }
        if (ClassOf(Buf().lines[row][col]) == CharClass::Space) {
            col++;
            continue;
        }
        break;
    }
    if (big) {
        while (col + 1 < LineLen(row) && ClassOf(Buf().lines[row][col + 1]) != CharClass::Space) col++;
    } else {
        CharClass cc = ClassOf(Buf().lines[row][col]);
        while (col + 1 < LineLen(row) && ClassOf(Buf().lines[row][col + 1]) == cc) col++;
    }
    return {row, col};
}

// ge/gE: end of word/WORD backward.
CursorPos Editor::MoveWordEndBackward(CursorPos from, bool big) const {
    int row = from.row, col = from.col;
    // Step off whatever run `from` is currently within, if any, so this
    // doesn't just land one character back inside the SAME word.
    if (col < LineLen(row) && ClassOf(Buf().lines[row][col]) != CharClass::Space) {
        if (big) {
            while (col > 0 && ClassOf(Buf().lines[row][col - 1]) != CharClass::Space) col--;
        } else {
            CharClass cc = ClassOf(Buf().lines[row][col]);
            while (col > 0 && ClassOf(Buf().lines[row][col - 1]) == cc) col--;
        }
    }
    col--;
    while (true) {
        if (col < 0) {
            if (row == 0) return {0, 0};
            row--;
            col = LineLen(row) - 1;
            if (LineLen(row) == 0) return {row, 0};
            continue;
        }
        if (ClassOf(Buf().lines[row][col]) == CharClass::Space) {
            col--;
            continue;
        }
        break;
    }
    if (col < 0) return {row, 0};
    return {row, col};
}

CursorPos Editor::FirstNonBlank(int row) const {
    row = std::max(0, std::min(row, Buf().LineCount() - 1));
    const std::string &line = Buf().lines[row];
    int col = 0;
    while (col < static_cast<int>(line.size()) && std::isspace(static_cast<unsigned char>(line[col]))) col++;
    if (col >= static_cast<int>(line.size())) col = std::max(0, static_cast<int>(line.size()) - 1);
    return {row, col};
}

// Paragraphs are delimited by blank lines -- a simplified but Vim-compatible
// reading for the common case; doesn't specially collapse runs of several
// consecutive blank lines into a single boundary the way Vim does.
CursorPos Editor::MoveParagraphForward(CursorPos from) const {
    int row = from.row + 1;
    int n = Buf().LineCount();
    while (row < n && !Buf().lines[row].empty()) row++;
    if (row >= n) row = n - 1;
    return {row, 0};
}

CursorPos Editor::MoveParagraphBackward(CursorPos from) const {
    int row = from.row - 1;
    while (row > 0 && !Buf().lines[row].empty()) row--;
    if (row < 0) row = 0;
    return {row, 0};
}

// %: jumps from the nearest bracket at-or-after the cursor on the current
// line to its match, anywhere in the buffer (proper nesting depth, not
// just the next same-glyph bracket).
CursorPos Editor::MoveMatchingBracket(CursorPos from) const {
    static const std::string kOpens = "([{";
    static const std::string kCloses = ")]}";
    const std::string &line0 = Buf().lines[from.row];
    int col = from.col;
    while (col < static_cast<int>(line0.size()) && kOpens.find(line0[col]) == std::string::npos &&
           kCloses.find(line0[col]) == std::string::npos) {
        col++;
    }
    if (col >= static_cast<int>(line0.size())) return from;  // no bracket on this line: no-op

    char c = line0[col];
    size_t open_idx = kOpens.find(c);
    bool forward = open_idx != std::string::npos;
    char open_c = forward ? c : kOpens[kCloses.find(c)];
    char close_c = forward ? kCloses[open_idx] : c;
    int depth = 0;
    if (forward) {
        for (int r = from.row; r < Buf().LineCount(); r++) {
            const std::string &l = Buf().lines[r];
            int start_c = (r == from.row) ? col : 0;
            for (int cc = start_c; cc < static_cast<int>(l.size()); cc++) {
                if (l[cc] == open_c) {
                    depth++;
                } else if (l[cc] == close_c && --depth == 0) {
                    return {r, cc};
                }
            }
        }
    } else {
        for (int r = from.row; r >= 0; r--) {
            const std::string &l = Buf().lines[r];
            int start_c = (r == from.row) ? col : static_cast<int>(l.size()) - 1;
            for (int cc = start_c; cc >= 0; cc--) {
                if (l[cc] == close_c) {
                    depth++;
                } else if (l[cc] == open_c && --depth == 0) {
                    return {r, cc};
                }
            }
        }
    }
    return from;  // unmatched: no-op
}

// --- Text objects ------------------------------------------------------

bool Editor::FindEnclosingBracketPair(CursorPos cursor, char open_c, char close_c, CursorPos *open_pos,
                                       CursorPos *close_pos) const {
    int row = cursor.row, col = cursor.col;
    bool on_open = (col < LineLen(row) && Buf().lines[row][col] == open_c);

    CursorPos op;
    if (on_open) {
        op = cursor;
    } else {
        // Scan backward for the nearest open bracket not already matched
        // by a close bracket seen along the way -- that's the enclosing
        // pair's open. Start one before the cursor: if we're sitting on
        // the close bracket, its own matching open is what we want (not
        // one level further out), and if we're strictly inside, the
        // character at the cursor itself is irrelevant to this scan.
        int depth = 0;
        bool found = false;
        for (int r = row; r >= 0 && !found; r--) {
            const std::string &l = Buf().lines[r];
            int start_c = (r == row) ? col - 1 : static_cast<int>(l.size()) - 1;
            for (int cc = start_c; cc >= 0; cc--) {
                if (l[cc] == close_c) {
                    depth++;
                } else if (l[cc] == open_c) {
                    if (depth == 0) {
                        op = {r, cc};
                        found = true;
                        break;
                    }
                    depth--;
                }
            }
        }
        if (!found) return false;
    }
    *open_pos = op;

    int depth = 0;
    int nrows = Buf().LineCount();
    for (int r = op.row; r < nrows; r++) {
        const std::string &l = Buf().lines[r];
        int start_c = (r == op.row) ? op.col + 1 : 0;
        for (int cc = start_c; cc < static_cast<int>(l.size()); cc++) {
            if (l[cc] == open_c) {
                depth++;
            } else if (l[cc] == close_c) {
                if (depth == 0) {
                    *close_pos = {r, cc};
                    return true;
                }
                depth--;
            }
        }
    }
    return false;  // unmatched
}

bool Editor::BracketObjectRange(CursorPos cursor, char open_c, char close_c, bool around, CursorPos *start,
                                 CursorPos *end) const {
    CursorPos op, cl;
    if (!FindEnclosingBracketPair(cursor, open_c, close_c, &op, &cl)) return false;
    if (around) {
        *start = op;
        *end = {cl.row, cl.col + 1};
    } else {
        *start = {op.row, op.col + 1};
        *end = cl;
    }
    return true;
}

// Quote objects only ever look within the current line, same as Vim: scan
// for quote characters, pick the first pair whose close is at-or-after the
// cursor (covers both "cursor inside a pair" and "cursor before any pair
// on the line, jump to the first one").
bool Editor::QuoteObjectRange(CursorPos cursor, char quote, bool around, CursorPos *start, CursorPos *end) const {
    const std::string &line = Buf().lines[cursor.row];
    std::vector<int> positions;
    for (int i = 0; i < static_cast<int>(line.size()); i++) {
        if (line[i] == quote) positions.push_back(i);
    }
    for (size_t i = 0; i + 1 < positions.size(); i += 2) {
        int a = positions[i], b = positions[i + 1];
        if (cursor.col > b) continue;
        if (around) {
            *start = {cursor.row, a};
            *end = {cursor.row, b + 1};
        } else {
            *start = {cursor.row, a + 1};
            *end = {cursor.row, b};
        }
        return true;
    }
    return false;
}

void Editor::WordObjectRange(CursorPos cursor, bool around, bool big, CursorPos *start, CursorPos *end) const {
    int row = cursor.row;
    int len = LineLen(row);
    if (len == 0) {
        *start = {row, 0};
        *end = {row, 0};
        return;
    }
    int col = std::min(cursor.col, len - 1);

    auto classify = [&](int c) -> int {
        if (big) return std::isspace(static_cast<unsigned char>(Buf().lines[row][c])) ? 0 : 1;
        CharClass cc = ClassOf(Buf().lines[row][c]);
        return cc == CharClass::Space ? 0 : (cc == CharClass::Word ? 1 : 2);
    };

    int cls = classify(col);
    int a = col, b = col;
    while (a > 0 && classify(a - 1) == cls) a--;
    while (b + 1 < len && classify(b + 1) == cls) b++;
    int excl_end = b + 1;

    if (!around) {
        *start = {row, a};
        *end = {row, excl_end};
        return;
    }

    // Around: pull in trailing whitespace; if there is none, pull in
    // leading whitespace instead -- Vim's actual rule for "a word".
    int trail = excl_end;
    while (trail < len && classify(trail) == 0) trail++;
    if (trail > excl_end) {
        *start = {row, a};
        *end = {row, trail};
        return;
    }
    int lead = a;
    while (lead > 0 && classify(lead - 1) == 0) lead--;
    *start = {row, lead};
    *end = {row, excl_end};
}

// Paragraphs are blank-line-delimited, same simplified reading as {/}
// above. "Around" pulls in one adjacent run of the opposite kind (e.g. the
// blank lines right after a text paragraph), same as Vim.
void Editor::ParagraphObjectRange(CursorPos cursor, bool around, CursorPos *start, CursorPos *end) const {
    int n = Buf().LineCount();
    int row = cursor.row;
    bool on_blank = Buf().lines[row].empty();
    int top = row, bottom = row;
    while (top > 0 && Buf().lines[top - 1].empty() == on_blank) top--;
    while (bottom + 1 < n && Buf().lines[bottom + 1].empty() == on_blank) bottom++;
    if (around) {
        int extra_bottom = bottom;
        while (extra_bottom + 1 < n && Buf().lines[extra_bottom + 1].empty() != on_blank) extra_bottom++;
        if (extra_bottom > bottom) {
            bottom = extra_bottom;
        } else {
            int extra_top = top;
            while (extra_top > 0 && Buf().lines[extra_top - 1].empty() != on_blank) extra_top--;
            top = extra_top;
        }
    }
    *start = {top, 0};
    *end = {bottom, 0};  // inclusive, matching the linewise convention used throughout (e.g. dd's range)
}

bool Editor::ResolveTextObject(char scope, char obj, CursorPos cursor, CursorPos *start, CursorPos *end,
                                bool *linewise) const {
    *linewise = false;
    bool around = (scope == 'a');
    switch (obj) {
        case 'w': WordObjectRange(cursor, around, false, start, end); return true;
        case 'W': WordObjectRange(cursor, around, true, start, end); return true;
        case '"': return QuoteObjectRange(cursor, '"', around, start, end);
        case '\'': return QuoteObjectRange(cursor, '\'', around, start, end);
        case '`': return QuoteObjectRange(cursor, '`', around, start, end);
        case '(':
        case ')':
        case 'b':
            return BracketObjectRange(cursor, '(', ')', around, start, end);
        case '{':
        case '}':
        case 'B':
            return BracketObjectRange(cursor, '{', '}', around, start, end);
        case '[':
        case ']':
            return BracketObjectRange(cursor, '[', ']', around, start, end);
        case '<':
        case '>':
            return BracketObjectRange(cursor, '<', '>', around, start, end);
        case 'p':
            ParagraphObjectRange(cursor, around, start, end);
            *linewise = true;
            return true;
        default: return false;
    }
}

bool Editor::ResolveMotion(char c, CursorPos from, int count, CursorPos *target, bool *linewise,
                            bool *inclusive) const {
    *linewise = false;
    *inclusive = false;
    int n = std::max(1, count);
    switch (c) {
        case 'h': *target = {from.row, std::max(0, from.col - n)}; return true;
        case 'l': *target = {from.row, from.col + n}; return true;
        case 'j': {
            int row = std::min(Buf().LineCount() - 1, from.row + n);
            *target = {row, from.col};  // preserve column, like Vim's "desired column"
            *linewise = true;
            return true;
        }
        case 'k': {
            int row = std::max(0, from.row - n);
            *target = {row, from.col};
            *linewise = true;
            return true;
        }
        case '0': *target = {from.row, 0}; return true;
        case '^': *target = FirstNonBlank(from.row); return true;
        case '$': {
            int row = std::min(Buf().LineCount() - 1, from.row + n - 1);
            *target = {row, LineLen(row)};
            *inclusive = LineLen(row) > 0;
            return true;
        }
        case 'w': {
            CursorPos t = from;
            for (int i = 0; i < n; i++) t = MoveWordForward(t);
            *target = t;
            return true;
        }
        case 'b': {
            CursorPos t = from;
            for (int i = 0; i < n; i++) t = MoveWordBackward(t);
            *target = t;
            return true;
        }
        case 'W': {
            CursorPos t = from;
            for (int i = 0; i < n; i++) t = MoveWORDForward(t);
            *target = t;
            return true;
        }
        case 'B': {
            CursorPos t = from;
            for (int i = 0; i < n; i++) t = MoveWORDBackward(t);
            *target = t;
            return true;
        }
        case 'e': {
            CursorPos t = from;
            for (int i = 0; i < n; i++) t = MoveWordEndForward(t, false);
            *target = t;
            *inclusive = true;
            return true;
        }
        case 'E': {
            CursorPos t = from;
            for (int i = 0; i < n; i++) t = MoveWordEndForward(t, true);
            *target = t;
            *inclusive = true;
            return true;
        }
        case '{': {
            CursorPos t = from;
            for (int i = 0; i < n; i++) t = MoveParagraphBackward(t);
            *target = t;
            return true;
        }
        case '}': {
            CursorPos t = from;
            for (int i = 0; i < n; i++) t = MoveParagraphForward(t);
            *target = t;
            return true;
        }
        case '%':
            // Vim's {count}% jumps to a percentage of the file instead of
            // matching a bracket; not implemented (low value, easy to
            // confuse with the far more common bracket-match use), so %
            // always matches brackets regardless of any pending count.
            *target = MoveMatchingBracket(from);
            *inclusive = true;
            return true;
        case 'G': {
            int row = count > 0 ? std::min(Buf().LineCount() - 1, count - 1) : Buf().LineCount() - 1;
            *target = FirstNonBlank(row);
            *linewise = true;
            return true;
        }
        case 'H': {
            const Pane &p = CurPane();
            int row = std::min(Buf().LineCount() - 1, p.scroll_row + n - 1);
            *target = FirstNonBlank(row);
            *linewise = true;
            return true;
        }
        case 'L': {
            const Pane &p = CurPane();
            int bottom = std::min(Buf().LineCount() - 1, p.scroll_row + p.visible_lines - 1);
            int row = std::max(p.scroll_row, bottom - n + 1);
            *target = FirstNonBlank(row);
            *linewise = true;
            return true;
        }
        case 'M': {
            const Pane &p = CurPane();
            int bottom = std::min(Buf().LineCount() - 1, p.scroll_row + p.visible_lines - 1);
            int row = (p.scroll_row + bottom) / 2;
            *target = FirstNonBlank(row);
            *linewise = true;
            return true;
        }
        default: return false;
    }
}

bool Editor::ResolveFind(char cmd, CursorPos from, char ch, int count, CursorPos *target) const {
    const std::string &line = Buf().lines[from.row];
    int len = static_cast<int>(line.size());
    int col = from.col;
    for (int i = 0; i < count; i++) {
        if (cmd == 'f' || cmd == 't') {
            int search_from = (cmd == 't' && i > 0) ? col + 2 : col + 1;
            int pos = -1;
            for (int cc = search_from; cc < len; cc++) {
                if (line[cc] == ch) {
                    pos = cc;
                    break;
                }
            }
            if (pos < 0) return false;
            col = (cmd == 't') ? pos - 1 : pos;
        } else {  // 'F' or 'T'
            int search_from = (cmd == 'T' && i > 0) ? col - 2 : col - 1;
            int pos = -1;
            for (int cc = search_from; cc >= 0; cc--) {
                if (line[cc] == ch) {
                    pos = cc;
                    break;
                }
            }
            if (pos < 0) return false;
            col = (cmd == 'T') ? pos + 1 : pos;
        }
    }
    *target = {from.row, col};
    return true;
}

// `` `x `` jumps to a mark's exact position; `'x` jumps to the first
// non-blank of its line (linewise). `` `` ``/`''` reuse this for the
// "previous jump" pseudo-mark: cmd == mark (`` `` `` or `''`) means "the
// spot recorded by the last RecordJumpFrom call" rather than a named mark.
bool Editor::ResolveMark(char cmd, char mark, CursorPos *target, bool *linewise) const {
    *linewise = (cmd == '\'');
    CursorPos pos;
    if (mark == cmd) {
        if (!Buf().has_last_jump) return false;
        pos = Buf().last_jump_from;
    } else if (mark >= 'a' && mark <= 'z') {
        auto it = Buf().marks.find(mark);
        if (it == Buf().marks.end()) return false;
        pos = it->second;
    } else {
        return false;
    }
    pos.row = std::max(0, std::min(pos.row, Buf().LineCount() - 1));
    pos.col = std::min(pos.col, LineLen(pos.row));
    *target = *linewise ? FirstNonBlank(pos.row) : pos;
    return true;
}

// Records where a "big jump" (G, gg, a mark jump, a search) started from,
// so `` `` ``/`''` can return to it. Called with the pre-jump cursor.
void Editor::RecordJumpFrom(CursorPos pos) {
    Buf().last_jump_from = pos;
    Buf().has_last_jump = true;
}

// --- Operators -------------------------------------------------------------

void Editor::ApplyOperator(char op, CursorPos start, CursorPos end, bool linewise) {
    if (linewise) {
        if (start.row > end.row) std::swap(start, end);
    } else {
        if (start.row > end.row || (start.row == end.row && start.col > end.col)) {
            std::swap(start, end);
        }
    }

    // Consumed regardless of which operator this turns out to be, so a
    // "{reg} typed before a case/indent operator (which don't use
    // registers) doesn't linger and leak into some later, unrelated
    // command.
    char reg_name = 0;
    bool append = false;
    TakeRegisterSpec(&reg_name, &append);

    if (op == 'y') {
        YankRange(start, end, linewise, reg_name, append);
        CurPane().cursor = start;
        ClampCursor();
        return;
    }
    if (op == 'u' || op == 'U' || op == '~') {
        ApplyCaseChange(start, end, linewise, op);
        CurPane().cursor = start;
        ClampCursor();
        return;
    }
    if (op == '>' || op == '<') {
        // Always a whole-line operation regardless of `linewise` -- even
        // e.g. `>w` in Vim indents the line(s) the motion touches, not
        // just the charwise span itself.
        IndentLines(start.row, end.row, op == '>' ? 1 : -1);
        CurPane().cursor = FirstNonBlank(start.row);
        ClampCursor();
        return;
    }

    PushUndo();
    YankRange(start, end, linewise, reg_name, append);
    DeleteRange(start, end, linewise);
    CurPane().cursor = start;
    if (op == 'c') {
        EnterInsert();
    } else {
        ClampCursor();
    }
}

void Editor::ApplyOperatorToSelectionOrCurrentLine(char op) {
    if (HasVisualSelection()) {
        CursorPos s, e;
        VisualRange(s, e);
        bool linewise = (mode_ == Mode::VisualLine);
        if (!linewise) e.col += 1;
        ApplyOperator(op, s, e, linewise);
    } else {
        int row = CurPane().cursor.row;
        ApplyOperator(op, {row, 0}, {row, 0}, true);
    }
}

void Editor::ApplyCaseChange(CursorPos start, CursorPos end, bool linewise, char mode) {
    PushUndo();
    auto transform = [mode](char c) -> char {
        unsigned char uc = static_cast<unsigned char>(c);
        if (mode == 'u') return static_cast<char>(std::tolower(uc));
        if (mode == 'U') return static_cast<char>(std::toupper(uc));
        if (std::isupper(uc)) return static_cast<char>(std::tolower(uc));
        if (std::islower(uc)) return static_cast<char>(std::toupper(uc));
        return c;
    };
    if (linewise) {
        int last = std::min(end.row, Buf().LineCount() - 1);
        for (int r = start.row; r <= last; r++) {
            for (char &c : Buf().lines[r]) c = transform(c);
        }
    } else if (start.row == end.row) {
        std::string &line = Buf().lines[start.row];
        int a = std::min(static_cast<int>(line.size()), start.col);
        int b = std::min(static_cast<int>(line.size()), end.col);
        for (int i = a; i < b; i++) line[i] = transform(line[i]);
    } else {
        std::string &first = Buf().lines[start.row];
        int a = std::min(static_cast<int>(first.size()), start.col);
        for (int i = a; i < static_cast<int>(first.size()); i++) first[i] = transform(first[i]);
        for (int r = start.row + 1; r < end.row; r++) {
            for (char &c : Buf().lines[r]) c = transform(c);
        }
        int end_row = std::min(end.row, Buf().LineCount() - 1);
        std::string &last = Buf().lines[end_row];
        int b = std::min(static_cast<int>(last.size()), end.col);
        for (int i = 0; i < b; i++) last[i] = transform(last[i]);
    }
    Buf().modified = true;
}

void Editor::IndentLines(int start_row, int end_row, int levels) {
    static const std::string kShift = "    ";  // fixed 4-space shiftwidth
    PushUndo();
    end_row = std::min(end_row, Buf().LineCount() - 1);
    for (int r = std::max(0, start_row); r <= end_row; r++) {
        std::string &line = Buf().lines[r];
        if (levels > 0) {
            for (int i = 0; i < levels; i++) line = kShift + line;
        } else {
            for (int i = 0; i < -levels; i++) {
                size_t remove = 0;
                while (remove < line.size() && remove < kShift.size() && line[remove] == ' ') remove++;
                line.erase(0, remove);
            }
        }
    }
    Buf().modified = true;
}

void Editor::JoinLines(int count, bool with_space) {
    int n = std::max(2, count);  // Vim: "Join [count] lines, with a minimum of two."
    CursorPos &cursor = CurPane().cursor;
    int row = cursor.row;
    int joins = std::min(n - 1, Buf().LineCount() - 1 - row);
    if (joins <= 0) return;
    PushUndo();
    int join_col = LineLen(row);
    for (int i = 0; i < joins; i++) {
        std::string next = Buf().lines[row + 1];
        Buf().lines.erase(Buf().lines.begin() + row + 1);
        size_t s = 0;
        while (s < next.size() && std::isspace(static_cast<unsigned char>(next[s]))) s++;
        next = next.substr(s);
        std::string &line = Buf().lines[row];
        join_col = static_cast<int>(line.size());
        if (with_space && !next.empty() && next[0] != ')' && !line.empty()) {
            line += ' ';
            join_col = static_cast<int>(line.size()) - 1;
        }
        line += next;
    }
    cursor = {row, join_col};
    Buf().modified = true;
    ClampCursor();
}

void Editor::DeleteRange(CursorPos start, CursorPos end, bool linewise) {
    if (linewise) {
        int first = start.row;
        int last = std::min(end.row, Buf().LineCount() - 1);
        Buf().lines.erase(Buf().lines.begin() + first, Buf().lines.begin() + last + 1);
        if (Buf().lines.empty()) Buf().lines.push_back("");
        Buf().modified = true;
        return;
    }
    if (start.row == end.row) {
        std::string &line = Buf().lines[start.row];
        int a = std::min(static_cast<int>(line.size()), start.col);
        int b = std::min(static_cast<int>(line.size()), end.col);
        if (b > a) line.erase(a, b - a);
    } else {
        // Multi-line charwise (e.g. `d}`, or a bracket/quote text object
        // spanning lines): join what's left of the start line (its own
        // prefix) with what's left of the end line (its own suffix),
        // dropping everything strictly between -- same shape as Vim's own
        // "join the two halves" behavior for an exclusive multi-line range.
        std::string &first_line = Buf().lines[start.row];
        int a = std::min(static_cast<int>(first_line.size()), start.col);
        std::string prefix = first_line.substr(0, a);
        int end_row = std::min(end.row, Buf().LineCount() - 1);
        const std::string &last_line = Buf().lines[end_row];
        int b = std::min(static_cast<int>(last_line.size()), end.col);
        std::string suffix = last_line.substr(b);
        Buf().lines[start.row] = prefix + suffix;
        Buf().lines.erase(Buf().lines.begin() + start.row + 1, Buf().lines.begin() + end_row + 1);
    }
    Buf().modified = true;
}

void Editor::YankRange(CursorPos start, CursorPos end, bool linewise, char reg_name, bool append) {
    std::string text;
    if (linewise) {
        int last = std::min(end.row, Buf().LineCount() - 1);
        for (int r = start.row; r <= last; r++) {
            text += Buf().lines[r];
            text += "\n";
        }
    } else if (start.row == end.row) {
        const std::string &line = Buf().lines[start.row];
        int a = std::min(static_cast<int>(line.size()), start.col);
        int b = std::min(static_cast<int>(line.size()), end.col);
        text = (b > a) ? line.substr(a, b - a) : "";
    } else {
        const std::string &first_line = Buf().lines[start.row];
        int a = std::min(static_cast<int>(first_line.size()), start.col);
        text += first_line.substr(a);
        for (int r = start.row + 1; r < end.row; r++) {
            text += "\n";
            text += Buf().lines[r];
        }
        int end_row = std::min(end.row, Buf().LineCount() - 1);
        const std::string &last_line = Buf().lines[end_row];
        int b = std::min(static_cast<int>(last_line.size()), end.col);
        text += "\n";
        text += last_line.substr(0, b);
    }

    Register &target = RegisterFor(reg_name);
    if (append && reg_name != 0) {
        target.text += text;
        target.linewise = target.linewise || linewise;
    } else {
        target.text = text;
        target.linewise = linewise;
    }
    // The unnamed register always mirrors the most recent yank/delete's
    // final result, regardless of which named register (if any) it also
    // went to, matching Vim.
    if (reg_name != 0) registers_['"'] = target;
}

namespace {
// Used for linewise yank text, which YankRange always terminates with a
// trailing '\n' (one after every line, including the last) -- so every
// segment split out here is a real line, and any text after a final '\n'
// (there never is any) would correctly be dropped.
std::vector<std::string> SplitYankLines(const std::string &text) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    return lines;
}

// Used for charwise yank text, which -- unlike the linewise case above --
// is spliced into whatever line already surrounds the insertion point, so
// the content after the final '\n' (if any) is real "continues on this
// line" text, not a line of its own, and must be kept.
std::vector<std::string> SplitKeepingLast(const std::string &text) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) {
            parts.push_back(text.substr(start));
            break;
        }
        parts.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    return parts;
}
}  // namespace

// Splices `text` (which may itself contain embedded newlines, e.g. a
// multi-line charwise yank) into the buffer at `pos`, joining rather than
// overwriting whatever's already on that line -- the charwise-paste
// equivalent of DeleteRange's multi-line branch above. Returns the
// position just past the inserted text.
CursorPos Editor::InsertCharwiseTextAt(CursorPos pos, const std::string &text) {
    if (text.find('\n') == std::string::npos) {
        std::string &line = Buf().lines[pos.row];
        int at = std::min(static_cast<int>(line.size()), pos.col);
        line.insert(at, text);
        return {pos.row, at + static_cast<int>(text.size())};
    }
    std::vector<std::string> parts = SplitKeepingLast(text);
    std::string &line = Buf().lines[pos.row];
    int at = std::min(static_cast<int>(line.size()), pos.col);
    std::string suffix = line.substr(at);
    std::string new_first = line.substr(0, at) + parts.front();
    std::string new_last = parts.back() + suffix;

    Buf().lines[pos.row] = new_first;
    std::vector<std::string> to_insert(parts.begin() + 1, parts.end() - 1);
    to_insert.push_back(new_last);
    Buf().lines.insert(Buf().lines.begin() + pos.row + 1, to_insert.begin(), to_insert.end());

    int end_row = pos.row + static_cast<int>(parts.size()) - 1;
    int end_col = static_cast<int>(parts.back().size());
    return {end_row, end_col};
}

void Editor::PasteAfter(int count, char reg_name) {
    Register &reg = RegisterFor(reg_name);
    if (reg.text.empty() || count <= 0) return;
    PushUndo();
    CursorPos &cursor = CurPane().cursor;
    if (reg.blockwise) {
        std::vector<std::string> block = SplitYankLines(reg.text);
        PasteBlockAt(cursor, block, false);
    } else if (reg.linewise) {
        std::vector<std::string> block = SplitYankLines(reg.text);
        std::vector<std::string> new_lines;
        for (int i = 0; i < count; i++) new_lines.insert(new_lines.end(), block.begin(), block.end());
        int insert_at = cursor.row + 1;
        Buf().lines.insert(Buf().lines.begin() + insert_at, new_lines.begin(), new_lines.end());
        cursor = {insert_at, 0};
    } else {
        std::string text;
        for (int i = 0; i < count; i++) text += reg.text;
        int at = std::min(LineLen(cursor.row), cursor.col + 1);
        CursorPos end = InsertCharwiseTextAt({cursor.row, at}, text);
        // Land on the last inserted character (Vim's `p`), not just past it.
        cursor = end;
        if (cursor.col > 0) cursor.col--;
    }
    Buf().modified = true;
    ClampCursor();
}

void Editor::PasteBefore(int count, char reg_name) {
    Register &reg = RegisterFor(reg_name);
    if (reg.text.empty() || count <= 0) return;
    PushUndo();
    CursorPos &cursor = CurPane().cursor;
    if (reg.blockwise) {
        std::vector<std::string> block = SplitYankLines(reg.text);
        PasteBlockAt(cursor, block, true);
    } else if (reg.linewise) {
        std::vector<std::string> block = SplitYankLines(reg.text);
        std::vector<std::string> new_lines;
        for (int i = 0; i < count; i++) new_lines.insert(new_lines.end(), block.begin(), block.end());
        int insert_at = cursor.row;
        Buf().lines.insert(Buf().lines.begin() + insert_at, new_lines.begin(), new_lines.end());
        cursor = {insert_at, 0};
    } else {
        std::string text;
        for (int i = 0; i < count; i++) text += reg.text;
        int at = std::min(LineLen(cursor.row), cursor.col);
        CursorPos pos = {cursor.row, at};
        InsertCharwiseTextAt(pos, text);
        cursor = pos;  // land on the first inserted character (Vim's `P`)
    }
    Buf().modified = true;
    ClampCursor();
}

// --- Undo/redo ---------------------------------------------------------

void Editor::PushUndo() {
    Buf().undo_stack.push_back(Buf().lines);
    if (Buf().undo_stack.size() > kMaxUndo) Buf().undo_stack.erase(Buf().undo_stack.begin());
    Buf().redo_stack.clear();
    change_epoch_++;
}

void Editor::Undo() {
    if (Buf().undo_stack.empty()) {
        status_message_ = "Already at oldest change";
        return;
    }
    Buf().redo_stack.push_back(Buf().lines);
    Buf().lines = Buf().undo_stack.back();
    Buf().undo_stack.pop_back();
    Buf().modified = true;
    ClampCursor();
}

void Editor::Redo() {
    if (Buf().redo_stack.empty()) {
        status_message_ = "Already at newest change";
        return;
    }
    Buf().undo_stack.push_back(Buf().lines);
    Buf().lines = Buf().redo_stack.back();
    Buf().redo_stack.pop_back();
    Buf().modified = true;
    ClampCursor();
}

// --- Command line ------------------------------------------------------

// Parses a single ex-command address at `pos`: `.` (current line), `$`
// (last line), a bare line number (1-indexed, converted to 0-indexed here),
// or `'{a-z}` (a mark's line) -- optionally followed by a `+N`/`-N` offset
// (bare `+`/`-` meaning 1, same as Vim). Returns false, leaving `pos`
// untouched, if nothing address-shaped starts at `pos` (including an
// unknown mark).
bool Editor::ParseExAddress(const std::string &cmd, size_t &pos, int *row) const {
    size_t n = cmd.size();
    if (pos >= n) return false;
    int base;
    if (cmd[pos] == '.') {
        base = CurPane().cursor.row;
        pos++;
    } else if (cmd[pos] == '$') {
        base = Buf().LineCount() - 1;
        pos++;
    } else if (cmd[pos] == '\'' && pos + 1 < n && cmd[pos + 1] >= 'a' && cmd[pos + 1] <= 'z') {
        auto it = Buf().marks.find(cmd[pos + 1]);
        if (it == Buf().marks.end()) return false;
        base = it->second.row;
        pos += 2;
    } else if (std::isdigit(static_cast<unsigned char>(cmd[pos]))) {
        size_t start = pos;
        while (pos < n && std::isdigit(static_cast<unsigned char>(cmd[pos]))) pos++;
        base = std::stoi(cmd.substr(start, pos - start)) - 1;
    } else {
        return false;
    }
    while (pos < n && (cmd[pos] == '+' || cmd[pos] == '-')) {
        int sign = (cmd[pos] == '+') ? 1 : -1;
        pos++;
        size_t start = pos;
        while (pos < n && std::isdigit(static_cast<unsigned char>(cmd[pos]))) pos++;
        int delta = (pos > start) ? std::stoi(cmd.substr(start, pos - start)) : 1;
        base += sign * delta;
    }
    *row = base;
    return true;
}

// Parses an optional range prefix (`%`, `addr`, or `addr,addr`) at `pos`.
// Returns false, leaving `pos` untouched, when there's no range at all --
// callers should fall back to their own default range in that case, not
// treat it as an error.
bool Editor::ParseExRange(const std::string &cmd, size_t &pos, int *start_row, int *end_row) const {
    if (pos < cmd.size() && cmd[pos] == '%') {
        *start_row = 0;
        *end_row = Buf().LineCount() - 1;
        pos++;
        return true;
    }
    size_t save = pos;
    int a;
    if (!ParseExAddress(cmd, pos, &a)) {
        pos = save;
        return false;
    }
    *start_row = *end_row = a;
    if (pos < cmd.size() && cmd[pos] == ',') {
        size_t after_comma = pos + 1;
        int b;
        if (ParseExAddress(cmd, after_comma, &b)) {
            *end_row = b;
            pos = after_comma;
        }
    }
    return true;
}

void Editor::ExSubstitute(int start_row, int end_row, const std::string &pattern, const std::string &replacement,
                           bool global_flag) {
    if (pattern.empty()) {
        status_message_ = "E35: No previous regular expression";
        return;
    }
    start_row = std::max(0, start_row);
    end_row = std::min(end_row, Buf().LineCount() - 1);
    if (start_row > end_row) return;
    int replacements = 0, lines_changed = 0;
    int last_row = start_row;
    bool pushed_undo = false;
    for (int r = start_row; r <= end_row; r++) {
        std::string &line = Buf().lines[r];
        size_t pos = 0;
        bool changed_this_line = false;
        while (true) {
            size_t found = CiFind(line, pattern, pos);
            if (found == std::string::npos) break;
            // Deferred until the first real match, not called unconditionally
            // up front -- otherwise a :s with zero matches (which returns
            // "E486: Pattern not found" below and touches nothing) would
            // still push a wasted, do-nothing undo snapshot.
            if (!pushed_undo) {
                PushUndo();
                pushed_undo = true;
            }
            line.replace(found, pattern.size(), replacement);
            replacements++;
            changed_this_line = true;
            pos = found + replacement.size();
            if (!global_flag) break;
            if (pattern.empty()) break;  // guard against an infinite loop on an empty match
        }
        if (changed_this_line) {
            lines_changed++;
            last_row = r;
        }
    }
    if (replacements == 0) {
        status_message_ = "E486: Pattern not found: " + pattern;
        return;
    }
    Buf().modified = true;
    last_search_ = pattern;  // matches Vim: :s's pattern becomes the search pattern too
    CurPane().cursor = FirstNonBlank(last_row);
    ClampCursor();
    status_message_ = (lines_changed == 1 && replacements == 1)
                           ? ""
                           : std::to_string(replacements) + " substitution(s) on " + std::to_string(lines_changed) +
                                 " line(s)";
}

void Editor::ExGlobal(int start_row, int end_row, bool invert, const std::string &pattern,
                       const std::string &subcmd) {
    if (pattern.empty()) {
        status_message_ = "E35: No previous regular expression";
        return;
    }
    start_row = std::max(0, start_row);
    end_row = std::min(end_row, Buf().LineCount() - 1);
    if (start_row > end_row || subcmd.empty()) return;
    std::vector<int> matches;
    for (int r = start_row; r <= end_row; r++) {
        bool found = CiFind(Buf().lines[r], pattern, 0) != std::string::npos;
        if (found != invert) matches.push_back(r);
    }
    PushUndo();
    int delta = 0;
    for (int r : matches) {
        int adjusted = r + delta;
        if (adjusted < 0 || adjusted >= Buf().LineCount()) continue;
        CurPane().cursor = {adjusted, 0};
        int before = Buf().LineCount();
        ExecuteCommandLine(subcmd);
        delta += Buf().LineCount() - before;
    }
    ClampCursor();
}

void Editor::ExMoveOrCopy(int start_row, int end_row, int dest_row, bool is_copy) {
    start_row = std::max(0, start_row);
    end_row = std::min(end_row, Buf().LineCount() - 1);
    if (start_row > end_row) return;
    if (!is_copy && dest_row >= start_row && dest_row <= end_row) {
        status_message_ = "E134: Move lines into themselves";
        return;
    }
    PushUndo();
    std::vector<std::string> chunk(Buf().lines.begin() + start_row, Buf().lines.begin() + end_row + 1);
    int insert_after = dest_row;
    if (!is_copy) {
        Buf().lines.erase(Buf().lines.begin() + start_row, Buf().lines.begin() + end_row + 1);
        if (dest_row >= start_row) insert_after -= static_cast<int>(chunk.size());
    }
    int insert_at = std::max(0, std::min(insert_after + 1, static_cast<int>(Buf().lines.size())));
    Buf().lines.insert(Buf().lines.begin() + insert_at, chunk.begin(), chunk.end());
    CurPane().cursor = {insert_at + static_cast<int>(chunk.size()) - 1, 0};
    Buf().modified = true;
    ClampCursor();
}

void Editor::ExecuteCommandLine(const std::string &raw) {
    size_t b = raw.find_first_not_of(" \t");
    if (b == std::string::npos) return;
    size_t e = raw.find_last_not_of(" \t");
    std::string cmd = raw.substr(b, e - b + 1);

    bool all_digits = std::all_of(cmd.begin(), cmd.end(),
                                   [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
    if (all_digits) {
        int n = std::stoi(cmd);
        CursorPos &cursor = CurPane().cursor;
        cursor.row = std::max(0, std::min(n - 1, Buf().LineCount() - 1));
        cursor.col = 0;
        ClampCursor();
        return;
    }

    // Range-taking ex commands (:s :g :v :d :y :m :t/:co) have their own
    // inline syntax rather than the generic "name<space>args" shape below,
    // so they're recognized up front, ahead of the space-based split. Each
    // one only commits to being that command once its syntax actually
    // checks out (e.g. a delimiter right after "s", a valid address right
    // after "m") -- otherwise nothing is consumed and execution falls
    // through to the ordinary dispatch below, so ":sp"/":split" and
    // ":tabnew" (which also start with a range-command's letter) aren't
    // shadowed by it.
    {
        size_t pos = 0;
        int rstart = 0, rend = 0;
        bool has_range = ParseExRange(cmd, pos, &rstart, &rend);
        size_t cmd_pos = pos;
        while (cmd_pos < cmd.size() && cmd[cmd_pos] == ' ') cmd_pos++;
        int default_row = CurPane().cursor.row;

        if (cmd_pos < cmd.size() && cmd[cmd_pos] == 's' &&
            (cmd_pos + 1 >= cmd.size() || !std::isalnum(static_cast<unsigned char>(cmd[cmd_pos + 1])))) {
            size_t p = cmd_pos + 1;
            if (p < cmd.size()) {
                char delim = cmd[p++];
                size_t pat_end = cmd.find(delim, p);
                std::string pattern = (pat_end == std::string::npos) ? cmd.substr(p) : cmd.substr(p, pat_end - p);
                std::string replacement;
                std::string flags;
                if (pat_end != std::string::npos) {
                    size_t r_start = pat_end + 1;
                    size_t r_end = cmd.find(delim, r_start);
                    replacement = (r_end == std::string::npos) ? cmd.substr(r_start) : cmd.substr(r_start, r_end - r_start);
                    if (r_end != std::string::npos) flags = cmd.substr(r_end + 1);
                }
                int s = has_range ? rstart : default_row;
                int en = has_range ? rend : default_row;
                ExSubstitute(s, en, pattern, replacement, flags.find('g') != std::string::npos);
                return;
            }
        }

        if (cmd_pos < cmd.size() && (cmd[cmd_pos] == 'g' || cmd[cmd_pos] == 'v')) {
            bool invert = (cmd[cmd_pos] == 'v');
            size_t p = cmd_pos + 1;
            if (cmd.compare(cmd_pos, 6, "global") == 0) p = cmd_pos + 6;
            if (!invert && p < cmd.size() && cmd[p] == '!') {
                invert = true;
                p++;
            }
            if (p < cmd.size() && !std::isalnum(static_cast<unsigned char>(cmd[p]))) {
                char delim = cmd[p++];
                size_t pat_end = cmd.find(delim, p);
                std::string pattern = (pat_end == std::string::npos) ? cmd.substr(p) : cmd.substr(p, pat_end - p);
                std::string subcmd = (pat_end == std::string::npos) ? "" : cmd.substr(pat_end + 1);
                int s = has_range ? rstart : 0;
                int en = has_range ? rend : Buf().LineCount() - 1;
                ExGlobal(s, en, invert, pattern, subcmd);
                return;
            }
        }

        // :d/:delete and :y/:yank take no inline args of their own (no
        // pattern, no destination address) -- require the rest of the line
        // to be exactly one of these spellings, rather than the
        // next-char-isn't-alnum heuristic used above, so unexpected
        // trailing text (a register/count mep doesn't support, or a typo)
        // falls through to "E492: Not an editor command" instead of being
        // silently ignored.
        std::string command_rest = cmd.substr(cmd_pos);
        if (command_rest == "d" || command_rest == "delete") {
            int s = has_range ? rstart : default_row;
            int en = has_range ? rend : default_row;
            ApplyOperator('d', {s, 0}, {en, 0}, true);
            return;
        }
        if (command_rest == "y" || command_rest == "yank") {
            int s = has_range ? rstart : default_row;
            int en = has_range ? rend : default_row;
            ApplyOperator('y', {s, 0}, {en, 0}, true);
            return;
        }

        if (cmd_pos < cmd.size() && cmd[cmd_pos] == 'm') {
            size_t p = cmd_pos + 1;
            while (p < cmd.size() && cmd[p] == ' ') p++;
            int dest;
            if (ParseExAddress(cmd, p, &dest) && p == cmd.size()) {
                int s = has_range ? rstart : default_row;
                int en = has_range ? rend : default_row;
                ExMoveOrCopy(s, en, dest, false);
                return;
            }
        }

        if (cmd_pos < cmd.size() && (cmd[cmd_pos] == 't' || cmd.compare(cmd_pos, 2, "co") == 0)) {
            size_t p = cmd_pos + (cmd.compare(cmd_pos, 2, "co") == 0 ? 2 : 1);
            while (p < cmd.size() && cmd[p] == ' ') p++;
            int dest;
            if (ParseExAddress(cmd, p, &dest) && p == cmd.size()) {
                int s = has_range ? rstart : default_row;
                int en = has_range ? rend : default_row;
                ExMoveOrCopy(s, en, dest, true);
                return;
            }
        }
    }

    size_t sp = cmd.find(' ');
    std::string name = (sp == std::string::npos) ? cmd : cmd.substr(0, sp);
    std::string args = (sp == std::string::npos) ? "" : cmd.substr(sp + 1);

    if (name == "w" || name == "write") {
        SaveFile(args.empty() ? Buf().filename : args);
    } else if (name == "wa" || name == "wall") {
        WriteAllModified();
    } else if (name == "q" || name == "quit") {
        QuitCurrent(false);
    } else if (name == "q!" || name == "quit!") {
        QuitCurrent(true);
    } else if (name == "qa" || name == "qall") {
        QuitAll(false);
    } else if (name == "qa!" || name == "qall!") {
        QuitAll(true);
    } else if (name == "wq" || name == "x") {
        if (SaveFile(args.empty() ? Buf().filename : args)) QuitCurrent(true);
    } else if (name == "wqa" || name == "xa" || name == "wqall" || name == "xall") {
        if (WriteAllModified()) QuitAll(true);
    } else if (name == "e" || name == "edit") {
        LoadFile(args);
    } else if (name == "split" || name == "sp") {
        SplitCurrentPane(SplitDir::Horizontal, args);
    } else if (name == "vsplit" || name == "vs") {
        SplitCurrentPane(SplitDir::Vertical, args);
    } else if (name == "terminal" || name == "term") {
        OpenTerminal(args);
    } else if (name == "close") {
        ClosePane();
    } else if (name == "tabnew") {
        TabNew(args);
    } else if (name == "tabdelete" || name == "tabclose") {
        TabDelete();
    } else if (name == "tabnext" || name == "tabn") {
        TabNext();
    } else if (name == "tabprevious" || name == "tabp" || name == "tabN") {
        TabPrevious();
    } else if (name == "set") {
        size_t pos = 0;
        while (pos < args.size()) {
            size_t end = args.find(' ', pos);
            if (end == std::string::npos) end = args.size();
            std::string opt = args.substr(pos, end - pos);
            pos = end + 1;
            if (opt.empty()) continue;
            bool negate = opt.rfind("no", 0) == 0 && opt.size() > 2;
            std::string key = negate ? opt.substr(2) : opt;
            bool value = !negate;
            if (key == "number" || key == "nu") {
                show_line_numbers_ = value;
            } else if (key == "ignorecase" || key == "ic") {
                ignore_case_ = value;
            } else if (key == "wrapscan" || key == "ws") {
                wrapscan_ = value;
            } else {
                status_message_ = "E518: Unknown option: " + opt;
            }
        }
    } else if (name == "normal" || name == "norm" || name == "normal!" || name == "norm!") {
        RunNormalKeys(args);
    } else if (name == "MepNotifyClear") {
        ClearNotifyHistory();
    } else if (name == "MepNotifyDismiss") {
        DismissAllToasts();
    } else if (name == "MepNotifyPanel") {
        ToggleNotifyHistoryPanel();
    } else if (name == "MepLayout") {
        ApplyLayout(args);
    } else if (name == "MepScratch") {
        OpenScratchBuffer();
    } else if (name == "MepZen") {
        ToggleZenMode();
    } else if (name == "colorscheme" || name == "colo") {
        if (args.empty()) {
            status_message_ = current_theme_name_;
        } else if (!ApplyTheme(args)) {
            status_message_ = "E185: Cannot find color scheme '" + args + "'";
        }
    } else if (name == "lua") {
        if (lua_) lua_->DoString(args);
    } else if (name == "source") {
#if defined(__EMSCRIPTEN__)
        char *result = mep_js_read_file(args.c_str());
        std::string res(result);
        std::free(result);
        if (res.rfind("OK\n", 0) == 0) {
            if (lua_) lua_->DoString(res.substr(3));
        } else {
            status_message_ = "E484: Can't open file " + args;
        }
#else
        if (lua_) lua_->DoFile(args);
#endif
    } else {
        auto it = lua_commands_.find(name);
        if (it != lua_commands_.end() && lua_) {
            lua_->CallRef(it->second);
        } else {
            status_message_ = "E492: Not an editor command: " + name;
        }
    }
}

void Editor::RunNormalKeys(const std::string &keys) {
    for (unsigned char c : keys) {
        if (mode_ == Mode::Insert) {
            ProcessInsertKey(c == 27 ? static_cast<int>(kReplayEscape) : static_cast<int>(c));
        } else if (mode_ == Mode::Normal) {
            ProcessNormalKey(static_cast<int>(c));
        } else {
            break;  // Visual/Command/Search mid-:normal: not supported, bail (see header comment)
        }
    }
    if (mode_ == Mode::Insert) {
        ProcessInsertKey(kReplayEscape);
    } else if (mode_ != Mode::Normal) {
        EnterNormal();
    }
}

// --- Lua-facing API --------------------------------------------------------

std::string Editor::GetLineForLua(int row) const {
    if (row < 0 || row >= Buf().LineCount()) return "";
    return Buf().lines[row];
}

void Editor::SetLineForLua(int row, const std::string &text) {
    if (row < 0 || row >= Buf().LineCount()) return;
    PushUndo();
    Buf().lines[row] = text;
    Buf().modified = true;
}

int Editor::LineCountForLua() const { return Buf().LineCount(); }

void Editor::ReplaceLinesForLua(int start_row, int end_row, const std::vector<std::string> &lines) {
    Buffer &buf = Buf();
    start_row = std::max(0, std::min(start_row, buf.LineCount()));
    end_row = std::max(start_row, std::min(end_row, buf.LineCount()));
    PushUndo();
    buf.lines.erase(buf.lines.begin() + start_row, buf.lines.begin() + end_row);
    buf.lines.insert(buf.lines.begin() + start_row, lines.begin(), lines.end());
    if (buf.lines.empty()) buf.lines.push_back("");
    buf.modified = true;
    ClampCursor();
}

void Editor::SetBufferLinesForLua(int buffer_id, const std::vector<std::string> &lines) {
    if (buffer_id < 0 || buffer_id >= static_cast<int>(buffers_.size())) return;
    Buffer &buf = buffers_[buffer_id];
    // No PushUndo/undo-stack entry -- this is for streaming external
    // process output (Part VI Phase 27's terminal/Run/REPL) into a
    // dedicated buffer that isn't the active pane's, where "undo" has no
    // sensible meaning (there's nothing the user typed to undo).
    buf.lines = lines.empty() ? std::vector<std::string>{""} : lines;
    buf.modified = false;
}

std::string Editor::BufferLabelForLua(int buffer_id) const {
    if (buffer_id < 0 || buffer_id >= static_cast<int>(buffers_.size())) return "";
    const Buffer &buf = buffers_[buffer_id];
    std::string label = buf.filename.empty() ? "[No Name]" : buf.filename;
    if (buf.modified) label += " [+]";
    return label;
}

void Editor::SwitchToBufferForLua(int buffer_id) {
    if (buffer_id < 0 || buffer_id >= static_cast<int>(buffers_.size())) return;
    CurPane().buffer_id = buffer_id;
    ClampCursor();
}

std::vector<std::string> Editor::LuaCommandNames() const {
    std::vector<std::string> names;
    names.reserve(lua_commands_.size());
    for (const auto &kv : lua_commands_) names.push_back(kv.first);
    return names;
}

void Editor::GetCursorForLua(int *row, int *col) const {
    *row = CurPane().cursor.row;
    *col = CurPane().cursor.col;
}

void Editor::SetCursorForLua(int row, int col) {
    CurPane().cursor = {row, col};
    ClampCursor();
}

void Editor::InsertTextForLua(const std::string &text) {
    PushUndo();
    for (char c : text) {
        if (c == '\n') InsertNewline();
        else InsertChar(static_cast<unsigned char>(c));
    }
}

void Editor::SetStatusMessage(const std::string &msg) { status_message_ = msg; }

// --- Notifications -------------------------------------------------------

namespace {
double NotifyTimeoutSeconds(Editor::NotifyLevel level) {
    switch (level) {
        case Editor::NotifyLevel::Error: return 8.0;
        case Editor::NotifyLevel::Warn: return 6.0;
        case Editor::NotifyLevel::Info: return 4.0;
        case Editor::NotifyLevel::Debug: return 3.0;
    }
    return 4.0;
}
}  // namespace

void Editor::Notify(const std::string &msg, NotifyLevel level) {
    status_message_ = msg;  // existing lightweight status-bar sink, unchanged

    NotifyEntry entry;
    entry.id = next_notify_id_++;
    entry.message = msg;
    entry.level = level;
    entry.created_at = last_notify_now_;
    entry.expires_at = last_notify_now_ + NotifyTimeoutSeconds(level);

    toasts_.push_back(entry);
    if (static_cast<int>(toasts_.size()) > kMaxVisibleToasts) {
        toasts_.erase(toasts_.begin());  // oldest evicted immediately, not queued
    }

    notify_history_.insert(notify_history_.begin(), entry);  // newest-first
    if (notify_history_.size() > kMaxNotifyHistory) notify_history_.pop_back();
}

void Editor::PruneExpiredToasts(double now) {
    last_notify_now_ = now;
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(),
                                  [now](const NotifyEntry &e) { return e.expires_at > 0 && now >= e.expires_at; }),
                   toasts_.end());
}

void Editor::DismissToast(int id) {
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [id](const NotifyEntry &e) { return e.id == id; }),
                   toasts_.end());
}

void Editor::DismissAllToasts() { toasts_.clear(); }

void Editor::ClearNotifyHistory() { notify_history_.clear(); }

void Editor::ToggleNotifyHistoryPanel() {
    if (notify_sidebar_id_ == 0) notify_sidebar_id_ = CreateSidebar("Notifications", "right", 44);
    if (IsSidebarOpen(notify_sidebar_id_)) {
        CloseSidebar(notify_sidebar_id_);
        return;
    }
    SidebarSection sec;
    sec.title = "";  // bare list, no collapsible header
    for (const NotifyEntry &e : notify_history_) {
        SidebarWidget w;
        const char *tag = e.level == NotifyLevel::Error   ? "[ERROR] "
                           : e.level == NotifyLevel::Warn  ? "[WARN] "
                           : e.level == NotifyLevel::Debug ? "[DEBUG] "
                                                            : "[INFO] ";
        w.text = std::string(tag) + e.message;
        w.hl = e.level == NotifyLevel::Error ? "Error" : e.level == NotifyLevel::Warn ? "Warn" : "";
        sec.widgets.push_back(std::move(w));
    }
    SetSidebarSections(notify_sidebar_id_, {sec});
    OpenSidebar(notify_sidebar_id_, true);
}

void Editor::RegisterLuaCommand(const std::string &name, int lua_ref) {
    lua_commands_[name] = lua_ref;
}

void Editor::RegisterLuaMapping(Mode mode, const std::string &key, int lua_ref) {
    if (mode == Mode::Normal) {
        normal_mappings_[key] = lua_ref;
    } else if (mode == Mode::Visual || mode == Mode::VisualLine) {
        visual_mappings_[key] = lua_ref;
    }
}

// --- File I/O ------------------------------------------------------------

bool Editor::SaveBuffer(Buffer &buf, const std::string &path) {
    if (path.empty()) {
        status_message_ = "E32: No file name";
        return false;
    }
#if defined(__EMSCRIPTEN__)
    std::string content;
    for (const auto &l : buf.lines) {
        content += l;
        content += "\n";
    }
    char *result = mep_js_write_file(path.c_str(), content.c_str());
    std::string res(result);
    std::free(result);
    if (res != "OK") {
        status_message_ = "E212: Can't open file for writing" +
                           (res.rfind("ERR\n", 0) == 0 ? " (" + res.substr(4) + ")" : "");
        return false;
    }
    buf.filename = path;
    buf.modified = false;
    save_epoch_++;
    status_message_ = "\"" + path + "\" " + std::to_string(buf.LineCount()) + "L written";
    return true;
#else
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        status_message_ = "E212: Can't open file for writing";
        return false;
    }
    for (const auto &l : buf.lines) out << l << "\n";
    buf.filename = path;
    buf.modified = false;
    save_epoch_++;
    status_message_ = "\"" + path + "\" " + std::to_string(buf.LineCount()) + "L written";
    return true;
#endif
}

bool Editor::SaveFile(const std::string &path) { return SaveBuffer(Buf(), path); }

std::vector<Editor::DirEntry> Editor::ListDirectory(const std::string &path) const {
    std::vector<DirEntry> entries;
#if defined(__EMSCRIPTEN__)
    char *result = mep_js_list_dir(path.c_str());
    std::string res(result);
    std::free(result);
    if (res.rfind("OK\n", 0) == 0) {
        Json j;
        if (Json::Parse(res.substr(3), &j) && j.is_array()) {
            for (const Json &item : j.items()) {
                entries.push_back({item.get("name").as_string(""), item.get("is_dir").as_bool(false)});
            }
        }
    }
#else
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(
             path, std::filesystem::directory_options::skip_permission_denied, ec)) {
        entries.push_back({entry.path().filename().string(), entry.is_directory(ec)});
    }
#endif
    return entries;
}

#if !defined(__EMSCRIPTEN__)
namespace {
std::string ProjectListPath() { return MepDataDir() + "/projects.json"; }
}  // namespace
#endif

std::vector<std::string> Editor::ListProjects() const {
    std::vector<std::string> out;
#if defined(__EMSCRIPTEN__)
    char *result = mep_js_project_list();
    std::string res(result);
    std::free(result);
    if (res.rfind("OK\n", 0) == 0) {
        Json j;
        if (Json::Parse(res.substr(3), &j) && j.is_array()) {
            for (const Json &v : j.items()) {
                if (v.type() == Json::Type::String) out.push_back(v.as_string());
            }
        }
    }
#else
    Json doc;
    if (ReadJsonFile(ProjectListPath(), &doc)) {
        const Json &arr = doc.get("projects");
        if (arr.type() == Json::Type::Array) {
            for (const Json &v : arr.items()) {
                if (v.type() == Json::Type::String) out.push_back(v.as_string());
            }
        }
    }
#endif
    return out;
}

void Editor::AddProject(const std::string &path) {
#if defined(__EMSCRIPTEN__)
    // Resolved server-side (launcher/serve.ts's /projects POST, via
    // Deno.realPath) -- the wasm sandbox has no real filesystem of its own
    // to resolve "." against, same reasoning as ListDirectory above.
    char *result = mep_js_project_mutate("add", path.c_str());
    std::free(result);
#else
    // Resolved to an absolute, symlink-free path before storing -- callers
    // pass "." (mep.projects()'s "add current directory"), and a bare "."
    // would both display uselessly in the picker (every entry just says
    // ".") and silently mean "whatever mep's cwd happens to be later" when
    // mep.project_open() eventually mep.chdir()s to it, rather than the
    // directory actually meant at add time.
    std::error_code ec;
    std::filesystem::path resolved = std::filesystem::canonical(path, ec);
    std::string canonical_path = ec ? path : resolved.string();

    std::vector<std::string> list = ListProjects();
    if (std::find(list.begin(), list.end(), canonical_path) == list.end()) {
        list.push_back(canonical_path);
        Json doc = Json::Object();
        Json arr = Json::Array();
        for (const std::string &p : list) arr.push_back(Json(p));
        doc["projects"] = arr;
        WriteJsonFile(ProjectListPath(), doc);
    }
#endif
}

void Editor::RemoveProject(const std::string &path) {
#if defined(__EMSCRIPTEN__)
    char *result = mep_js_project_mutate("remove", path.c_str());
    std::free(result);
#else
    std::vector<std::string> list = ListProjects();
    list.erase(std::remove(list.begin(), list.end(), path), list.end());
    Json doc = Json::Object();
    Json arr = Json::Array();
    for (const std::string &p : list) arr.push_back(Json(p));
    doc["projects"] = arr;
    WriteJsonFile(ProjectListPath(), doc);
#endif
}

bool Editor::WriteAllModified() {
    int written = 0;
    bool all_ok = true;
    for (auto &buf : buffers_) {
        if (!buf.modified) continue;
        if (SaveBuffer(buf, buf.filename)) {
            written++;
        } else {
            all_ok = false;
        }
    }
    if (all_ok) {
        status_message_ = std::to_string(written) + " buffer(s) written";
    } else {
        status_message_ = "E141: Some buffers were not written (no file name, or a write error)";
    }
    return all_ok;
}

bool Editor::IsOnlyPaneOverall() const { return tabs_.size() == 1 && tabs_[0].root->dir == SplitDir::Leaf; }

bool Editor::AnyBufferModified() const {
    for (const auto &buf : buffers_) {
        if (buf.modified) return true;
    }
    return false;
}

void Editor::QuitCurrent(bool force) {
    if (!force && Buf().modified) {
        status_message_ = "E37: No write since last change (add ! to override)";
        return;
    }
    if (IsOnlyPaneOverall()) {
        should_quit_ = true;
    } else {
        ClosePane();
    }
}

void Editor::QuitAll(bool force) {
    if (!force && AnyBufferModified()) {
        status_message_ = "E37: Some buffers have unsaved changes (add ! to override)";
        return;
    }
    should_quit_ = true;
}

void Editor::LoadFile(const std::string &path) {
    if (path.empty()) {
        status_message_ = "E32: No file name";
        return;
    }
    bool existed = false;
    int id = FindOrCreateBuffer(path, &existed);
    if (id < 0) return;  // FindOrCreateBuffer already set status_message_
    CurPane().buffer_id = id;
    CurPane().cursor = {0, 0};
    CurPane().scroll_row = 0;
    status_message_ = existed ? "\"" + path + "\" " + std::to_string(buffers_[id].LineCount()) + "L loaded"
                               : "\"" + path + "\" [New]";
}

// --- Menu-facing API -------------------------------------------------------

void Editor::Copy() { ApplyOperatorToSelectionOrCurrentLine('y'); }
void Editor::Cut() { ApplyOperatorToSelectionOrCurrentLine('d'); }
void Editor::Paste() { PasteAfter(); }

void Editor::NewBuffer() {
    if (Buf().modified) {
        status_message_ = "E37: No write since last change (add ! to override)";
        return;
    }
    int id = CreateEmptyBuffer();
    CurPane().buffer_id = id;
    CurPane().cursor = {0, 0};
    CurPane().scroll_row = 0;
    status_message_.clear();
}

void Editor::BeginCommand(const std::string &prefix) {
    mode_ = Mode::Command;
    command_line_ = prefix;
}

void Editor::RunCommand(const std::string &cmd) { ExecuteCommandLine(cmd); }
