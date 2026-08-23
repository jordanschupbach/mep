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
#include "regex.h"
#include "vterm.h"
#include "image_doc.h"
#include "js_engine.h"
#include "pdf_doc.h"
#include "treesitter.h"

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
        if (!window.__mepBridgeBase) throw new Error("no file bridge (not launched via `just run-wasm`)");
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

// Binary-safe counterpart to mep_js_read_file, used only for opening image
// files (LoadFile's IsImagePath branch): the plain /read endpoint round-
// trips through Deno.readTextFile + JSON, which corrupts arbitrary binary
// bytes (invalid UTF-8 sequences get replaced/mangled), so this hits a
// separate /read-binary endpoint (launcher/serve.ts) that base64-encodes
// the raw file instead. Same synchronous-XHR shape and "OK\n<payload>" /
// "ERR\n<message>" contract as mep_js_read_file above (see the comment
// above it for why this is plain EM_JS, not EM_ASYNC_JS) -- the payload
// here is base64 text, not raw file content, decoded back to bytes by
// Base64Decode (image_doc.cpp) on the C++ side.
EM_JS(char *, mep_js_read_file_binary, (const char *path_ptr), {
    const path = UTF8ToString(path_ptr);
    let text;
    try {
        if (!window.__mepBridgeBase) throw new Error("no file bridge (not launched via `just run-wasm`)");
        const xhr = new XMLHttpRequest();
        xhr.open("GET", window.__mepBridgeBase + "/read-binary?path=" + encodeURIComponent(path), false);
        xhr.send();
        const result = JSON.parse(xhr.responseText);
        text = result.ok ? ("OK\n" + result.content_b64) : ("ERR\n" + result.error);
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
        if (!window.__mepBridgeBase) throw new Error("no file bridge (not launched via `just run-wasm`)");
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
        if (!window.__mepBridgeBase) throw new Error("no file bridge (not launched via `just run-wasm`)");
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
        if (!window.__mepBridgeBase) throw new Error("no file bridge (not launched via `just run-wasm`)");
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
        if (!window.__mepBridgeBase) throw new Error("no file bridge (not launched via `just run-wasm`)");
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
        window.__mepLastPtyError = "no file bridge (not launched via `just run-wasm`)";
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

// NVIM_PARITY_PLAN.md's own "Full theme palette set (28 in mep.nvim)" note
// says the registry is just data, low-value to front-load, trivial to grow
// later -- growing it here. Every palette below is a real, published
// colorscheme's actual hex values (not invented), ported into this
// compact bg/fg/red/green/yellow/blue/purple/cyan/orange/border shape;
// `border` is each scheme's own subtle "selection/line-highlight" tone,
// not a literal border color, matching how the four original palettes
// above already used that slot.
const Palette kPaletteDracula = {
    "dracula",
    {40, 42, 54, 255}, {248, 248, 242, 255}, {255, 85, 85, 255}, {80, 250, 123, 255}, {241, 250, 140, 255},
    {98, 114, 164, 255}, {189, 147, 249, 255}, {139, 233, 253, 255}, {255, 184, 108, 255}, {68, 71, 90, 255},
};
const Palette kPaletteTokyonightStorm = {
    "tokyonight-storm",
    {36, 40, 59, 255}, {192, 202, 245, 255}, {247, 118, 142, 255}, {158, 206, 106, 255}, {224, 175, 104, 255},
    {122, 162, 247, 255}, {187, 154, 247, 255}, {125, 207, 255, 255}, {255, 158, 100, 255}, {65, 72, 104, 255},
};
const Palette kPaletteTokyonightNight = {
    "tokyonight-night",
    {26, 27, 38, 255}, {192, 202, 245, 255}, {247, 118, 142, 255}, {158, 206, 106, 255}, {224, 175, 104, 255},
    {122, 162, 247, 255}, {187, 154, 247, 255}, {125, 207, 255, 255}, {255, 158, 100, 255}, {41, 46, 66, 255},
};
const Palette kPaletteTokyonightMoon = {
    "tokyonight-moon",
    {34, 36, 54, 255}, {200, 211, 245, 255}, {255, 117, 127, 255}, {195, 232, 141, 255}, {255, 199, 119, 255},
    {130, 170, 255, 255}, {192, 153, 255, 255}, {134, 225, 252, 255}, {255, 150, 108, 255}, {47, 51, 77, 255},
};
const Palette kPaletteCatppuccinMocha = {
    "catppuccin-mocha",
    {30, 30, 46, 255}, {205, 214, 244, 255}, {243, 139, 168, 255}, {166, 227, 161, 255}, {249, 226, 175, 255},
    {137, 180, 250, 255}, {203, 166, 247, 255}, {148, 226, 213, 255}, {250, 179, 135, 255}, {69, 71, 90, 255},
};
const Palette kPaletteCatppuccinMacchiato = {
    "catppuccin-macchiato",
    {36, 39, 58, 255}, {202, 211, 245, 255}, {237, 135, 150, 255}, {166, 218, 149, 255}, {238, 212, 159, 255},
    {138, 173, 244, 255}, {198, 160, 246, 255}, {139, 213, 202, 255}, {245, 169, 127, 255}, {73, 77, 100, 255},
};
const Palette kPaletteCatppuccinFrappe = {
    "catppuccin-frappe",
    {48, 52, 70, 255}, {198, 208, 245, 255}, {231, 130, 132, 255}, {166, 209, 137, 255}, {229, 200, 144, 255},
    {140, 170, 238, 255}, {202, 158, 230, 255}, {129, 200, 190, 255}, {239, 159, 118, 255}, {81, 87, 109, 255},
};
const Palette kPaletteCatppuccinLatte = {
    "catppuccin-latte",
    {239, 241, 245, 255}, {76, 79, 105, 255}, {210, 15, 57, 255}, {64, 160, 43, 255}, {223, 142, 29, 255},
    {30, 102, 245, 255}, {136, 57, 239, 255}, {23, 146, 153, 255}, {254, 100, 11, 255}, {188, 192, 204, 255},
};
const Palette kPaletteEverforestDark = {
    "everforest-dark",
    {45, 53, 59, 255}, {211, 198, 170, 255}, {230, 126, 128, 255}, {167, 192, 128, 255}, {219, 188, 127, 255},
    {127, 187, 179, 255}, {214, 153, 182, 255}, {131, 192, 146, 255}, {230, 152, 117, 255}, {79, 88, 94, 255},
};
const Palette kPaletteEverforestLight = {
    "everforest-light",
    {253, 246, 227, 255}, {92, 106, 114, 255}, {248, 85, 82, 255}, {141, 161, 1, 255}, {223, 160, 0, 255},
    {58, 148, 197, 255}, {223, 105, 186, 255}, {53, 167, 124, 255}, {245, 125, 38, 255}, {224, 220, 199, 255},
};
const Palette kPaletteKanagawa = {
    "kanagawa",
    {31, 31, 40, 255}, {220, 215, 186, 255}, {195, 64, 67, 255}, {118, 148, 106, 255}, {192, 163, 110, 255},
    {126, 156, 216, 255}, {149, 127, 184, 255}, {106, 149, 137, 255}, {255, 160, 102, 255}, {84, 84, 109, 255},
};
const Palette kPaletteOnedark = {
    "one-dark",
    {40, 44, 52, 255}, {171, 178, 191, 255}, {224, 108, 117, 255}, {152, 195, 121, 255}, {229, 192, 123, 255},
    {97, 175, 239, 255}, {198, 120, 221, 255}, {86, 182, 194, 255}, {209, 154, 102, 255}, {62, 68, 81, 255},
};
const Palette kPaletteOneLight = {
    "one-light",
    {250, 250, 250, 255}, {56, 58, 66, 255}, {228, 86, 73, 255}, {80, 161, 79, 255}, {193, 132, 1, 255},
    {64, 120, 242, 255}, {166, 38, 164, 255}, {1, 132, 188, 255}, {152, 104, 1, 255}, {211, 211, 211, 255},
};
const Palette kPaletteSolarizedDark = {
    "solarized-dark",
    {0, 43, 54, 255}, {131, 148, 150, 255}, {220, 50, 47, 255}, {133, 153, 0, 255}, {181, 137, 0, 255},
    {38, 139, 210, 255}, {108, 113, 196, 255}, {42, 161, 152, 255}, {203, 75, 22, 255}, {88, 110, 117, 255},
};
const Palette kPaletteSolarizedLight = {
    "solarized-light",
    {253, 246, 227, 255}, {101, 123, 131, 255}, {220, 50, 47, 255}, {133, 153, 0, 255}, {181, 137, 0, 255},
    {38, 139, 210, 255}, {108, 113, 196, 255}, {42, 161, 152, 255}, {203, 75, 22, 255}, {147, 161, 161, 255},
};
// The remaining palettes below close out full parity with mep.nvim/lua/
// mep/theme/palettes.lua's 28-entry set (every name in that file's
// `M.palettes` table now has a same-named, same-hex-value counterpart
// here) -- `one-dark`/`onedark` above was also renamed to match that
// file's hyphenated name exactly.
const Palette kPaletteNordLight = {
    "nord-light",
    {236, 239, 244, 255}, {46, 52, 64, 255}, {191, 97, 106, 255}, {163, 190, 140, 255}, {235, 203, 139, 255},
    {94, 129, 172, 255}, {180, 142, 173, 255}, {136, 192, 208, 255}, {208, 135, 112, 255}, {216, 222, 233, 255},
};
const Palette kPaletteTokyoNight = {
    "tokyo-night",
    {26, 27, 38, 255}, {192, 202, 245, 255}, {247, 118, 142, 255}, {158, 206, 106, 255}, {224, 175, 104, 255},
    {122, 162, 247, 255}, {187, 154, 247, 255}, {125, 207, 255, 255}, {255, 158, 100, 255}, {65, 72, 104, 255},
};
const Palette kPaletteRosePine = {
    "rose-pine",
    {25, 23, 36, 255}, {224, 222, 244, 255}, {235, 111, 146, 255}, {49, 116, 143, 255}, {246, 193, 119, 255},
    {156, 207, 216, 255}, {196, 167, 231, 255}, {156, 207, 216, 255}, {235, 188, 186, 255}, {64, 61, 82, 255},
};
const Palette kPaletteRosePineDawn = {
    "rose-pine-dawn",
    {250, 244, 237, 255}, {87, 82, 121, 255}, {180, 99, 122, 255}, {40, 105, 131, 255}, {234, 157, 52, 255},
    {86, 148, 159, 255}, {144, 122, 169, 255}, {86, 148, 159, 255}, {215, 130, 126, 255}, {223, 218, 217, 255},
};
const Palette kPaletteMonokai = {
    "monokai",
    {39, 40, 34, 255}, {248, 248, 242, 255}, {249, 38, 114, 255}, {166, 226, 46, 255}, {230, 219, 116, 255},
    {102, 217, 239, 255}, {174, 129, 255, 255}, {102, 217, 239, 255}, {253, 151, 31, 255}, {73, 72, 62, 255},
};
const Palette kPaletteAyuDark = {
    "ayu-dark",
    {10, 14, 20, 255}, {179, 177, 173, 255}, {255, 51, 51, 255}, {194, 217, 76, 255}, {255, 180, 84, 255},
    {89, 194, 255, 255}, {210, 166, 255, 255}, {149, 230, 203, 255}, {255, 143, 64, 255}, {19, 23, 33, 255},
};
const Palette kPaletteAyuMirage = {
    "ayu-mirage",
    {33, 39, 51, 255}, {217, 215, 206, 255}, {255, 51, 51, 255}, {187, 230, 126, 255}, {255, 196, 76, 255},
    {128, 212, 255, 255}, {212, 191, 255, 255}, {92, 207, 230, 255}, {255, 174, 87, 255}, {61, 71, 81, 255},
};
const Palette kPaletteGithubDark = {
    "github-dark",
    {13, 17, 23, 255}, {201, 209, 217, 255}, {255, 123, 114, 255}, {63, 185, 80, 255}, {210, 153, 34, 255},
    {88, 166, 255, 255}, {210, 168, 255, 255}, {57, 197, 207, 255}, {255, 166, 87, 255}, {48, 54, 61, 255},
};
const Palette kPaletteGithubLight = {
    "github-light",
    {255, 255, 255, 255}, {36, 41, 47, 255}, {207, 34, 46, 255}, {26, 127, 55, 255}, {154, 103, 0, 255},
    {9, 105, 218, 255}, {130, 80, 223, 255}, {27, 124, 131, 255}, {149, 56, 0, 255}, {208, 215, 222, 255},
};
const Palette kPaletteNightfox = {
    "nightfox",
    {19, 26, 36, 255}, {205, 206, 207, 255}, {201, 79, 109, 255}, {129, 178, 154, 255}, {219, 192, 116, 255},
    {113, 156, 214, 255}, {157, 121, 214, 255}, {99, 205, 207, 255}, {244, 162, 97, 255}, {43, 59, 81, 255},
};
const Palette kPaletteHorizon = {
    "horizon",
    {28, 30, 38, 255}, {213, 216, 218, 255}, {233, 86, 120, 255}, {9, 247, 160, 255}, {250, 183, 149, 255},
    {37, 176, 188, 255}, {184, 119, 219, 255}, {33, 191, 194, 255}, {240, 148, 131, 255}, {46, 48, 62, 255},
};
const Palette kPaletteZenburn = {
    "zenburn",
    {63, 63, 63, 255}, {220, 220, 204, 255}, {204, 147, 147, 255}, {127, 159, 127, 255}, {240, 223, 175, 255},
    {140, 176, 211, 255}, {220, 140, 195, 255}, {147, 224, 227, 255}, {223, 175, 143, 255}, {95, 95, 95, 255},
};
const Palette kPaletteSynthwave84 = {
    "synthwave84",
    {38, 35, 53, 255}, {255, 255, 255, 255}, {254, 68, 80, 255}, {114, 241, 184, 255}, {254, 222, 93, 255},
    {46, 226, 250, 255}, {255, 126, 219, 255}, {54, 249, 246, 255}, {255, 139, 57, 255}, {64, 61, 78, 255},
};
const Palette kPaletteOxocarbonDark = {
    "oxocarbon-dark",
    {22, 22, 22, 255}, {242, 244, 248, 255}, {238, 83, 150, 255}, {66, 190, 101, 255}, {255, 126, 182, 255},
    {120, 169, 255, 255}, {190, 149, 255, 255}, {61, 219, 217, 255}, {255, 126, 182, 255}, {57, 57, 57, 255},
};
const Palette kPaletteOxocarbonLight = {
    "oxocarbon-light",
    {242, 244, 248, 255}, {22, 22, 22, 255}, {238, 83, 150, 255}, {66, 190, 101, 255}, {255, 171, 145, 255},
    {15, 98, 254, 255}, {103, 58, 183, 255}, {8, 189, 186, 255}, {255, 111, 0, 255}, {82, 82, 82, 255},
};

const Palette *FindPalette(const std::string &name) {
    static const Palette *kAll[] = {
        &kPaletteMepDark,        &kPaletteGruvboxDark,          &kPaletteNord,
        &kPaletteGruvboxLight,   &kPaletteDracula,              &kPaletteTokyonightStorm,
        &kPaletteTokyonightNight, &kPaletteTokyonightMoon,      &kPaletteCatppuccinMocha,
        &kPaletteCatppuccinMacchiato, &kPaletteCatppuccinFrappe, &kPaletteCatppuccinLatte,
        &kPaletteEverforestDark, &kPaletteEverforestLight,      &kPaletteKanagawa,
        &kPaletteOnedark,        &kPaletteOneLight,             &kPaletteSolarizedDark,
        &kPaletteSolarizedLight, &kPaletteNordLight,            &kPaletteTokyoNight,
        &kPaletteRosePine,       &kPaletteRosePineDawn,         &kPaletteMonokai,
        &kPaletteAyuDark,        &kPaletteAyuMirage,            &kPaletteGithubDark,
        &kPaletteGithubLight,    &kPaletteNightfox,             &kPaletteHorizon,
        &kPaletteZenburn,        &kPaletteSynthwave84,          &kPaletteOxocarbonDark,
        &kPaletteOxocarbonLight,
    };
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
    // incsearch's live match preview (Phase 4 stretch item) -- a span
    // recolor via the plain Decoration/hl_group pipeline (see
    // Editor::UpdateIncSearch), so it wants to read as "found" against
    // either theme rather than blend in like Visual's selection tint does.
    g["IncSearch"] = Mix(p.orange, p.fg, 0.5f);
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
    theme_epoch_++;
    return true;
}

std::vector<std::string> Editor::ThemeNames() const {
    return {
        kPaletteMepDark.name,        kPaletteGruvboxDark.name,          kPaletteNord.name,
        kPaletteGruvboxLight.name,   kPaletteDracula.name,              kPaletteTokyonightStorm.name,
        kPaletteTokyonightNight.name, kPaletteTokyonightMoon.name,      kPaletteCatppuccinMocha.name,
        kPaletteCatppuccinMacchiato.name, kPaletteCatppuccinFrappe.name, kPaletteCatppuccinLatte.name,
        kPaletteEverforestDark.name, kPaletteEverforestLight.name,      kPaletteKanagawa.name,
        kPaletteOnedark.name,        kPaletteOneLight.name,             kPaletteSolarizedDark.name,
        kPaletteSolarizedLight.name, kPaletteNordLight.name,            kPaletteTokyoNight.name,
        kPaletteRosePine.name,       kPaletteRosePineDawn.name,         kPaletteMonokai.name,
        kPaletteAyuDark.name,        kPaletteAyuMirage.name,            kPaletteGithubDark.name,
        kPaletteGithubLight.name,    kPaletteNightfox.name,             kPaletteHorizon.name,
        kPaletteZenburn.name,        kPaletteSynthwave84.name,          kPaletteOxocarbonDark.name,
        kPaletteOxocarbonLight.name,
    };
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
    MaybeDismissHover();
    {
        Vector2 wheel = GetMouseWheelMoveV();
        if (wheel.x != 0.0f || wheel.y != 0.0f) HandleMouseWheel(wheel.x, wheel.y);
    }
    if (HandleMod1Shortcuts()) return;
    if (HandleTabShortcuts()) return;
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
        case Mode::Preview:
            HandlePreviewInput();
            break;
        case Mode::Sidebar:
            HandleSidebarInput();
            break;
        case Mode::Picker:
            HandlePickerInput();
            break;
        case Mode::RoamGraph:
            HandleRoamGraphInput();
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
        case Mode::Image:
            HandleImageInput();
            break;
        case Mode::Pdf:
            HandlePdfInput();
            break;
        case Mode::Html:
            HandleHtmlInput();
            break;
        case Mode::OfficeNormal:
            HandleOfficeNormalInput();
            break;
        case Mode::OfficeInsert:
            HandleOfficeInsertInput();
            break;
        case Mode::OfficeVisual:
            HandleOfficeVisualInput();
            break;
        case Mode::SheetNormal:
            HandleSheetNormalInput();
            break;
        case Mode::SheetInsert:
            HandleSheetInsertInput();
            break;
        case Mode::SheetVisual:
            HandleSheetVisualInput();
            break;
    }
}

void Editor::UpdateScrollForPane(int pane_id, int visible_lines) {
    SplitNode *node = FindNode(tabs_[active_tab_].root.get(), pane_id);
    if (!node) return;
    Pane &pane = node->pane;
    visible_lines = std::max(1, visible_lines);
    pane.visible_lines = visible_lines;
    if (pane.buffer_id < 0 || pane.buffer_id >= static_cast<int>(buffers_.size())) return;
    const Buffer &buf = buffers_[pane.buffer_id];

    // How far the cursor's own row moved since the last call -- feeds the
    // smoothing cap below. A pane that's never run this before (-1) is
    // "just arrived here" (a fresh buffer switch, which already resets
    // cursor/scroll_row together in LoadFile) rather than mid-navigation,
    // so its first-ever catch-up isn't throttled.
    int cursor_delta = (pane.scroll_follow_last_cursor_row < 0)
                            ? std::max(1, visible_lines)
                            : std::abs(pane.cursor.row - pane.scroll_follow_last_cursor_row);
    pane.scroll_follow_last_cursor_row = pane.cursor.row;

    int target;
    if (pane.cursor.row < pane.scroll_row) {
        target = pane.cursor.row;
    } else {
        // How many *visual* slots [scroll_row, cursor.row] takes -- a
        // closed fold's hidden interior counts as one slot no matter how
        // many raw rows it spans, mirroring DrawPane's own render loop
        // (main.cpp) and StepVisibleRow's upward step. The old plain
        // `cursor.row >= scroll_row + visible_lines` check compared raw
        // row counts instead: with folds collapsing most of the buffer,
        // that could both trigger a scroll the pane didn't actually need
        // (the cursor's fold was still comfortably on screen -- folds
        // between it and scroll_row make the true visible range cover
        // *more* raw rows than `visible_lines`, not fewer) and, when it
        // did scroll, land scroll_row on an arbitrary raw row that could
        // sit inside some other closed fold's hidden middle -- a state
        // the render loop never expects, since every other path onto a
        // fold's rows starts exactly at fold_start. An org inline image
        // (Editor::OrgImagesVisible()/Buffer::org_image_rows) is folds'
        // own mirror image -- it *expands* one row into kOrgInlineImageSlots
        // instead of collapsing several into one -- so the row it's
        // stepped onto here contributes that many slots instead of 1;
        // must stay in exact agreement with DrawPane's row loop and its
        // cursor-Y lookup (main.cpp), the same three-way constraint the
        // comment above already calls out for folds.
        auto row_slots = [&](int r) {
            if (org_images_visible_ && buf.org_image_rows.count(r)) return kOrgInlineImageSlots;
            if (org_latex_visible_) {
                auto it = buf.org_latex_rows.find(r);
                if (it != buf.org_latex_rows.end()) return it->second.slots;
            }
            return 1;
        };
        int slots = row_slots(pane.cursor.row);  // the cursor's own row is always the first slot(s)
        int row = pane.cursor.row;
        while (row > pane.scroll_row && slots < visible_lines) {
            row--;
            for (const Fold &f : buf.folds) {
                if (f.closed && row > f.start_row && row <= f.end_row) {
                    row = f.start_row;
                    break;
                }
            }
            // Same containment jump-back as the Fold loop just above, for
            // a multi-line LaTeX fragment's own raw-row span -- row_slots
            // only knows how to answer for a fragment's *start* row, so
            // landing anywhere else inside one (its remaining raw source
            // rows, skipped outright by DrawPane/the cursor-Y lookup,
            // main.cpp) needs the same rewind before calling it.
            if (org_latex_visible_) {
                for (const auto &kv : buf.org_latex_rows) {
                    if (row > kv.first && row <= kv.second.end_row) {
                        row = kv.first;
                        break;
                    }
                }
            }
            slots += row_slots(row);
        }
        target = std::max(row, pane.scroll_row);
    }

    // Smooth catch-up: advance scroll_row toward `target` by at most
    // cursor_delta rows this call instead of snapping straight there.
    // An ordinary single-row step (j/k) only ever needs scroll_row to
    // move by 1 anyway UNLESS the row just stepped onto/off is an org
    // inline image or LaTeX fragment (row_slots, many slots tall for one
    // buffer row) -- that's the one case `target` can land far from
    // scroll_row despite cursor_delta staying 1, and capping the advance
    // to cursor_delta turns it into a multi-frame slide instead of an
    // instant jump: UpdateScrollForPane runs every rendered frame (see
    // its DrawPane call site, main.cpp), so the remaining distance keeps
    // closing at this same rate even across frames with no further
    // input, until scroll_row reaches target. A genuinely large cursor
    // jump (G, gg, a search, a counted motion like 15j) moves
    // cursor_delta by roughly the same amount target needs to move, so
    // it stays effectively uncapped and still lands in a single frame.
    int cap = std::max(1, cursor_delta);
    int jump = target - pane.scroll_row;
    if (jump > cap) pane.scroll_row += cap;
    else if (jump < -cap) pane.scroll_row -= cap;
    else pane.scroll_row = target;

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

// --- Mouse-wheel / trackpad scrolling ---------------------------------------
//
// Sign convention: for every content type below except WheelScrollTerminal,
// "forward" (revealing later content -- a row/scroll position that
// increases) is `-dy`. This matches GLFW/raylib's own mouse-wheel
// convention, where scrolling the wheel *down* (or a trackpad two-finger
// swipe up, i.e. "content follows your fingers") reports a *negative*
// yoffset. WheelScrollTerminal inverts this on purpose -- see its own
// comment, since TerminalSession::scroll_offset counts backward from the
// live tail rather than forward through the content.
namespace {
constexpr float kWheelLinesPerNotch = 3.0f;   // text/office/sheet/terminal row-or-paragraph step
constexpr float kWheelColsPerNotch = 6.0f;    // text/office column step
constexpr float kWheelPixelsPerNotch = 50.0f; // pdf/image/html pixel step
// Ctrl-scroll zoom (HandleMouseWheel's Image/Pdf/Html/Office branches):
// one full notch (dy = 1.0) multiplies zoom by this, same "feel" as one
// press of the +/- zoom key each viewer already has. A trackpad's
// fractional per-frame dy values compound the same way exponentially
// (pow(step, dy)), so a two-finger pinch/scroll zooms smoothly instead
// of snapping in whole-notch jumps.
constexpr float kWheelZoomStepPerNotch = 1.25f;
// Shared by HandleImageInput's own +/-/= keys and ApplyImageZoom (this
// function used to be a local lambda inside HandleImageInput; hoisted
// out, along with these three constants, so HandleMouseWheel's Image
// branch can call the exact same math -- see ApplyImageZoom's own
// header, editor.h).
constexpr float kImageZoomStep = 1.25f;
constexpr float kMinImageZoom = 0.05f;
constexpr float kMaxImageZoom = 20.0f;
// Same reasoning as the three above, for HandlePdfInput/ApplyPdfZoom/
// SettlePdfZoom.
constexpr float kPdfZoomStep = 1.25f;
constexpr float kMinPdfZoom = 0.1f;
constexpr float kMaxPdfZoom = 8.0f;
constexpr float kZoomSettleLo = 0.5f, kZoomSettleHi = 2.0f;
}  // namespace

int Editor::WheelSteps(float &accum, float delta, float units_per_notch) {
    accum += delta * units_per_notch;
    int steps = static_cast<int>(accum);  // truncates toward zero
    accum -= static_cast<float>(steps);
    return steps;
}

void Editor::WheelScrollTextBuffer(float dx, float dy) {
    Pane &p = CurPane();
    if (dy != 0.0f) {
        int steps = WheelSteps(wheel_accum_text_row_, -dy, kWheelLinesPerNotch);
        if (steps != 0) {
            int row = p.cursor.row;
            int dir = steps > 0 ? 1 : -1;
            for (int i = 0; i < std::abs(steps); i++) row = StepVisibleRow(row, dir);
            p.cursor.row = row;
            ClampCursor();
        }
    }
    if (dx != 0.0f) {
        int steps = WheelSteps(wheel_accum_text_col_, dx, kWheelColsPerNotch);
        if (steps != 0) {
            p.cursor.col = std::max(0, p.cursor.col + steps);
            ClampCursor();
        }
    }
}

void Editor::WheelScrollOffice(float dx, float dy) {
    auto it = officedocs_.find(CurPane().buffer_id);
    if (it == officedocs_.end()) return;
    OfficeSession &sess = it->second;
    int para_count = static_cast<int>(sess.doc.paragraphs.size());
    if (para_count <= 0) return;
    // Ctrl-scroll zooms instead of moving the cursor -- same clamped
    // multiplier as the toolbar's Z-/Z+ buttons (SetOfficeZoom), just
    // driven by the wheel's vertical delta. See WheelScrollPdf's own
    // comment for the sign convention. Checked before the table-nav-exit
    // side effect below so a zoom gesture doesn't also drop out of an
    // active table cell the way a real scroll deliberately does.
    if ((IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) && dy != 0.0f) {
        SetOfficeZoom(std::pow(kWheelZoomStepPerNotch, dy));
        return;
    }
    if (sess.in_table_edit >= 0) {
        // Scrolling away from a table naturally exits cell/table nav --
        // otherwise the highlighted "active cell" would keep pointing at
        // a table_ref the cursor paragraph no longer anchors once wheel
        // scroll moves cursor_para elsewhere (same reasoning Escape's own
        // ExitOfficeTable call has in HandleOfficeNormalInput).
        sess.in_table_edit = -1;
        sess.table_cell_editing = false;
    }
    auto cur_len = [&]() { return static_cast<int>(sess.doc.paragraphs[sess.cursor_para].text.size()); };
    if (dy != 0.0f) {
        int steps = WheelSteps(wheel_accum_office_para_, -dy, kWheelLinesPerNotch);
        if (steps != 0) {
            sess.cursor_para = std::clamp(sess.cursor_para + steps, 0, para_count - 1);
            sess.cursor_col = std::min(sess.cursor_col, cur_len());
        }
    }
    if (dx != 0.0f) {
        int steps = WheelSteps(wheel_accum_office_col_, dx, kWheelColsPerNotch);
        if (steps != 0) sess.cursor_col = std::clamp(sess.cursor_col + steps, 0, cur_len());
    }
}

void Editor::WheelScrollSheet(float dx, float dy) {
    auto it = sheetdocs_.find(CurPane().buffer_id);
    if (it == sheetdocs_.end()) return;
    SheetSession &sess = it->second;
    if (sess.wb.sheets.empty()) return;
    // No upper clamp beyond 0, matching HandleSheetNormalInput's own hjkl
    // (a sheet has no fixed bottom-right corner to stop at -- see its own
    // comment).
    if (dy != 0.0f) {
        int steps = WheelSteps(wheel_accum_sheet_row_, -dy, kWheelLinesPerNotch);
        if (steps != 0) sess.cursor_row = std::max(0, sess.cursor_row + steps);
    }
    if (dx != 0.0f) {
        int steps = WheelSteps(wheel_accum_sheet_col_, dx, kWheelColsPerNotch);
        if (steps != 0) sess.cursor_col = std::max(0, sess.cursor_col + steps);
    }
}

void Editor::WheelScrollPdf(float dx, float dy) {
    auto it = pdfs_.find(CurPane().buffer_id);
    if (it == pdfs_.end()) return;
    PdfSession &sess = it->second;
    if (sess.search_active) return;  // don't fight the search-input overlay
    int page_count = sess.doc ? sess.doc->PageCount() : 0;
    if (page_count <= 0) return;
    // Ctrl-scroll zooms instead of scrolling/panning -- same center-
    // anchored math as the +/- zoom keys (HandlePdfInput's apply_zoom),
    // just driven by the wheel's vertical delta. Positive dy (scroll up/
    // away, or a trackpad two-finger swipe down) zooms in, matching a
    // browser's own Ctrl-scroll convention. Stray dx from a diagonal
    // trackpad gesture is ignored while ctrl is held, same as the pan
    // below being ignored during the pinch itself -- this IS the zoom
    // gesture, not a scroll.
    if ((IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) && dy != 0.0f) {
        ApplyPdfZoom(sess, sess.zoom * std::pow(kWheelZoomStepPerNotch, dy));
        return;
    }
    if (dy != 0.0f) {
        sess.scroll_y += -dy * kWheelPixelsPerNotch;
        RebasePdfScroll(sess);
    }
    if (dx != 0.0f) {
        sess.pan_x += static_cast<int>(dx * kWheelPixelsPerNotch);
        ClampPdfPanX(sess);
    }
}

void Editor::WheelScrollImage(float dx, float dy) {
    auto it = images_.find(CurPane().buffer_id);
    if (it == images_.end()) return;
    ImageSession &sess = it->second;
    // Ctrl-scroll zooms instead of panning -- see WheelScrollPdf's own
    // comment for the sign/dx-ignored reasoning (identical here).
    if ((IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) && dy != 0.0f) {
        ApplyImageZoom(sess, sess.zoom * std::pow(kWheelZoomStepPerNotch, dy));
        return;
    }
    int max_pan_x = sess.doc ? std::max(0, static_cast<int>(sess.doc->Width() * sess.zoom) - sess.viewport_w) : 0;
    int max_pan_y = sess.doc ? std::max(0, static_cast<int>(sess.doc->Height() * sess.zoom) - sess.viewport_h) : 0;
    if (dy != 0.0f) {
        sess.pan_y = std::clamp(sess.pan_y + static_cast<int>(-dy * kWheelPixelsPerNotch), 0, max_pan_y);
    }
    if (dx != 0.0f) {
        sess.pan_x = std::clamp(sess.pan_x + static_cast<int>(dx * kWheelPixelsPerNotch), 0, max_pan_x);
    }
}

void Editor::WheelScrollHtml(float dx, float dy) {
    (void)dx;  // no horizontal scroll model for html -- HandleHtmlInput's own hjkl only binds j/k
    auto it = htmldocs_.find(CurPane().buffer_id);
    if (it == htmldocs_.end()) return;
    HtmlSession &sess = it->second;
    // Ctrl-scroll zooms instead of scrolling -- same +/-/= range
    // (HandleHtmlInput) as the keyboard shortcuts, just driven by the
    // wheel's vertical delta. See WheelScrollPdf's own comment for the
    // sign convention.
    if ((IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) && dy != 0.0f) {
        sess.zoom = std::clamp(sess.zoom * std::pow(kWheelZoomStepPerNotch, dy), 0.3f, 3.0f);
        return;
    }
    if (dy != 0.0f) sess.scroll_y = std::max(0.0f, sess.scroll_y + (-dy * kWheelPixelsPerNotch));
}

void Editor::WheelScrollTerminal(float dy) {
    TerminalSession *sess = FindTerminal(CurPane().buffer_id);
    if (!sess || !sess->vterm) return;
    // scroll_offset counts lines *back* from the live tail (0 = live), the
    // opposite sense of every other content type's forward-increasing
    // scroll position above -- so unlike them, this uses +dy directly:
    // scrolling up increases scroll_offset (further into history),
    // scrolling down decreases it (back toward the live tail). Mirrors
    // Shift+PageUp/PageDown's own direction (HandleTerminalInput).
    int steps = WheelSteps(wheel_accum_term_, dy, kWheelLinesPerNotch);
    if (steps != 0) sess->scroll_offset = std::clamp(sess->scroll_offset + steps, 0, sess->vterm->ScrollbackLines());
}

// Dispatched once per frame from HandleInput(), before the mode-specific
// handler -- see this method's own declaration (editor.h) for why. Modal
// overlays (Picker/Sidebar/Prompt/Command/etc.) and the Insert-family
// modes (Insert/OfficeInsert/SheetInsert) are deliberately excluded: an
// overlay has its own separate input focus the wheel isn't wired into yet,
// and every content type's scroll position here is cursor-derived (see
// UpdateScrollForPane/the Office and Sheet scroll-follow passes in
// main.cpp) -- silently relocating the actual text-insertion point out
// from under an actively-typing user via a passive scroll gesture would be
// far more surprising than the wheel simply doing nothing while typing.
void Editor::HandleMouseWheel(float dx, float dy) {
    switch (mode_) {
        case Mode::Normal:
        case Mode::Visual:
        case Mode::VisualLine:
        case Mode::VisualBlock:
            WheelScrollTextBuffer(dx, dy);
            break;
        case Mode::OfficeNormal:
        case Mode::OfficeVisual:
            WheelScrollOffice(dx, dy);
            break;
        case Mode::SheetNormal:
        case Mode::SheetVisual:
            WheelScrollSheet(dx, dy);
            break;
        case Mode::Pdf:
            WheelScrollPdf(dx, dy);
            break;
        case Mode::Image:
            WheelScrollImage(dx, dy);
            break;
        case Mode::Html:
            WheelScrollHtml(dx, dy);
            break;
        case Mode::Terminal:
            // Only affects TerminalSession::scroll_offset (the scrollback
            // view), never forwarded to the child process -- safe
            // regardless of what program is running, unlike Ctrl-D/Ctrl-U
            // (see HandleTerminalInput's own comment on why those stay as
            // plain forwarded keystrokes here instead).
            WheelScrollTerminal(dy);
            break;
        default:
            break;
    }
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

std::string Editor::CurrentVisualSelectionText() const {
    if (!HasVisualSelection()) return "";
    if (IsVisualBlock()) {
        int top, bottom, left, right;
        VisualBlockRange(top, bottom, left, right);
        std::string text;
        for (int r = top; r <= bottom; r++) {
            const std::string &line = Buf().lines[r];
            int a = std::min(static_cast<int>(line.size()), left);
            int b = (right < 0) ? static_cast<int>(line.size()) : std::min(static_cast<int>(line.size()), right + 1);
            text += (b > a) ? line.substr(a, b - a) : "";
            text += "\n";
        }
        return text;
    }
    CursorPos s, e;
    VisualRange(s, e);
    bool linewise = (mode_ == Mode::VisualLine);
    // VisualRange's end is inclusive; ExtractRangeText's charwise end is
    // exclusive (same adjustment ApplyOperatorToSelectionOrCurrentLine
    // already makes before calling into the shared operator/yank path).
    if (!linewise) e.col += 1;
    return ExtractRangeText(s, e, linewise);
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
    // Plain execvp() (see job.cpp) means the shell would otherwise inherit
    // mep's own TERM verbatim -- fine when mep itself was launched from a
    // real interactive terminal, but silently wrong (typically TERM=dumb
    // or unset) whenever mep was started some other way (a GUI/.desktop
    // launcher, an editor's own "open terminal" wrapper, ...), and shows
    // up downstream as prompt tools like starship refusing to render
    // ("disabled due to TERM=dumb"). VTerm (see its own class comment)
    // implements real xterm-class cursor motion/erase/256+truecolor SGR,
    // so xterm-256color is an honest claim, not just a placeholder value
    // -- matches what the web build's own launcher/serve.ts pty bridge
    // already hardcodes for the same reason.
    sess.job_id =
        JobManager::Instance().Spawn(argv, "", std::move(cb), /*use_pty=*/true, {{"TERM", "xterm-256color"}});
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

// --- Image-viewer panes ----------------------------------------------------

bool Editor::IsImageBuffer(int buffer_id) const { return images_.find(buffer_id) != images_.end(); }

const ImageSession *Editor::GetImage(int buffer_id) const {
    auto it = images_.find(buffer_id);
    return it == images_.end() ? nullptr : &it->second;
}

void Editor::ResizeImageViewport(int buffer_id, int w, int h) {
    auto it = images_.find(buffer_id);
    if (it == images_.end()) return;
    ImageSession &sess = it->second;
    sess.viewport_w = w;
    sess.viewport_h = h;
    int max_pan_x = sess.doc ? std::max(0, static_cast<int>(sess.doc->Width() * sess.zoom) - w) : 0;
    int max_pan_y = sess.doc ? std::max(0, static_cast<int>(sess.doc->Height() * sess.zoom) - h) : 0;
    sess.pan_x = std::clamp(sess.pan_x, 0, max_pan_x);
    sess.pan_y = std::clamp(sess.pan_y, 0, max_pan_y);
}

void Editor::OpenImageInPlace(const std::string &path, const unsigned char *bytes, size_t len) {
    int buffer_id = -1;
    for (size_t i = 0; i < buffers_.size(); i++) {
        if (!buffers_[i].filename.empty() && buffers_[i].filename == path) {
            buffer_id = static_cast<int>(i);
            break;
        }
    }
    if (buffer_id < 0) {
        auto doc = std::make_unique<ImageDoc>();
        if (!doc->LoadFromMemory(bytes, len)) {
            status_message_ = "E-\"" + path + "\": " + doc->Error();
            return;
        }
        buffer_id = CreateEmptyBuffer();
        buffers_[buffer_id].filename = path;
        ImageSession sess;
        sess.buffer_id = buffer_id;
        sess.doc = std::move(doc);
        images_[buffer_id] = std::move(sess);
    }
    CurPane().buffer_id = buffer_id;
    CurPane().cursor = {0, 0};
    CurPane().scroll_row = 0;
    status_message_.clear();
}

void Editor::SyncModeToActivePaneBuffer() {
    if (IsTerminalBuffer(CurPane().buffer_id)) {
        // Focus landing on a terminal pane always re-enters live keystroke
        // forwarding, never a stale Ctrl-\ Ctrl-N snapshot (EnterTerminal-
        // NormalMode) left over from a previous visit -- matches DrawPane's
        // (main.cpp) own comment that the live grid resumes "the instant
        // ... focus moves to a different pane [and back]". Without this,
        // switching focus here while mode_ was already Mode::Normal (e.g.
        // arriving from an ordinary buffer pane) left mode_ untouched, so
        // DrawPane's show_live_grid check -- which treats an active pane's
        // Mode::Normal as "snapshotted" -- rendered this pane's buffer
        // instead, which for a terminal holds no real text (blank) until
        // the 'i'/'a' special case in DispatchNormalKey jumped back to
        // Mode::Terminal.
        mode_ = Mode::Terminal;
    } else if (IsImageBuffer(CurPane().buffer_id)) {
        mode_ = Mode::Image;
    } else if (IsPdfBuffer(CurPane().buffer_id)) {
        mode_ = Mode::Pdf;
    } else if (IsHtmlBuffer(CurPane().buffer_id)) {
        mode_ = Mode::Html;
    } else if (IsOfficeBuffer(CurPane().buffer_id)) {
        // Always re-enters at OfficeNormal, never resumes mid-
        // OfficeInsert/Visual -- matches every other mode transition here.
        mode_ = Mode::OfficeNormal;
    } else if (IsSheetBuffer(CurPane().buffer_id)) {
        // Always re-enters at SheetNormal, same reasoning as Office above.
        mode_ = Mode::SheetNormal;
    } else if (mode_ == Mode::Terminal || mode_ == Mode::Image || mode_ == Mode::Pdf || mode_ == Mode::Html ||
               mode_ == Mode::OfficeNormal || mode_ == Mode::OfficeInsert || mode_ == Mode::OfficeVisual ||
               mode_ == Mode::SheetNormal || mode_ == Mode::SheetInsert || mode_ == Mode::SheetVisual) {
        mode_ = Mode::Normal;
    }
}

void Editor::ApplyImageZoom(ImageSession &sess, float new_zoom) {
    if (!sess.doc || sess.doc->Width() <= 0 || sess.doc->Height() <= 0) return;
    new_zoom = std::clamp(new_zoom, kMinImageZoom, kMaxImageZoom);
    float ratio = new_zoom / sess.zoom;
    float center_x = sess.pan_x + sess.viewport_w / 2.0f;
    float center_y = sess.pan_y + sess.viewport_h / 2.0f;
    sess.zoom = new_zoom;
    sess.pan_x = static_cast<int>(center_x * ratio - sess.viewport_w / 2.0f);
    sess.pan_y = static_cast<int>(center_y * ratio - sess.viewport_h / 2.0f);
    int mx = std::max(0, static_cast<int>(sess.doc->Width() * sess.zoom) - sess.viewport_w);
    int my = std::max(0, static_cast<int>(sess.doc->Height() * sess.zoom) - sess.viewport_h);
    sess.pan_x = std::clamp(sess.pan_x, 0, mx);
    sess.pan_y = std::clamp(sess.pan_y, 0, my);
}

void Editor::HandleImageInput() {
    ImageSession *sess = nullptr;
    {
        auto it = images_.find(CurPane().buffer_id);
        if (it == images_.end()) {
            mode_ = Mode::Normal;
            return;
        }
        sess = &it->second;
    }

    constexpr int kPanStep = 40;
    int max_pan_x = sess->doc ? std::max(0, static_cast<int>(sess->doc->Width() * sess->zoom) - sess->viewport_w) : 0;
    int max_pan_y = sess->doc ? std::max(0, static_cast<int>(sess->doc->Height() * sess->zoom) - sess->viewport_h) : 0;

    // IsKeyPressed(Repeat) rather than draining GetKeyPressed(): GLFW only
    // enqueues the initial key-down into the GetKeyPressed() queue, so
    // holding a key down (OS auto-repeat) would otherwise pan exactly once.
    auto held = [](int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); };
    if (held(KEY_H) || held(KEY_LEFT)) {
        sess->pan_x = std::clamp(sess->pan_x - kPanStep, 0, max_pan_x);
    }
    if (held(KEY_L) || held(KEY_RIGHT)) {
        sess->pan_x = std::clamp(sess->pan_x + kPanStep, 0, max_pan_x);
    }
    if (held(KEY_K) || held(KEY_UP)) {
        sess->pan_y = std::clamp(sess->pan_y - kPanStep, 0, max_pan_y);
    }
    if (held(KEY_J) || held(KEY_DOWN)) {
        sess->pan_y = std::clamp(sess->pan_y + kPanStep, 0, max_pan_y);
    }
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (ctrl && IsKeyPressed(KEY_D)) sess->pan_y = std::clamp(sess->pan_y + sess->viewport_h / 2, 0, max_pan_y);
    if (ctrl && IsKeyPressed(KEY_U)) sess->pan_y = std::clamp(sess->pan_y - sess->viewport_h / 2, 0, max_pan_y);

    // +/-/= zoom: +/- multiply or divide the zoom factor by kImageZoomStep,
    // re-anchored on whatever image point is currently at the viewport's
    // center (so the thing you're looking at stays put instead of the view
    // snapping back to the image's top-left corner on every keypress); =
    // fits the whole image into the current viewport and resets pan.
    // ApplyImageZoom (editor.h) does the actual work -- also reused by
    // HandleMouseWheel's Image branch for Ctrl-scroll.
    auto apply_zoom = [&](float new_zoom) { ApplyImageZoom(*sess, new_zoom); };

    int cp = GetCharPressed();
    while (cp > 0) {
        if (cp == ':') {
            EnterCommand();
            return;  // mode_ is no longer Image -- stop draining as this mode
        } else if (cp == static_cast<int>(leader_key_) && !whichkey_bindings_.empty()) {
            TriggerWhichKey();
            return;
        } else if (cp == '+') {
            apply_zoom(sess->zoom * kImageZoomStep);
        } else if (cp == '-') {
            apply_zoom(sess->zoom / kImageZoomStep);
        } else if (cp == '=' && sess->doc && sess->doc->Width() > 0 && sess->doc->Height() > 0 &&
                   sess->viewport_w > 0 && sess->viewport_h > 0) {
            float fit = std::min(static_cast<float>(sess->viewport_w) / sess->doc->Width(),
                                  static_cast<float>(sess->viewport_h) / sess->doc->Height());
            sess->zoom = std::clamp(fit, kMinImageZoom, kMaxImageZoom);
            sess->pan_x = 0;
            sess->pan_y = 0;
        }
        // Every other printable key is a deliberate no-op -- see
        // Mode::Image's own comment for why (no text to insert/operate on).
        cp = GetCharPressed();
    }
}

// --- PDF-viewer panes -------------------------------------------------------

namespace {
// GetCharPressed() yields full Unicode codepoints (raylib's char callback,
// not a raw keycode), so a PDF search query typed via HandlePdfSearchInput
// needs to UTF-8-encode anything beyond ASCII itself -- std::string here is
// always UTF-8 (matching PdfDoc::Search's own expectation, which decodes it
// back to UTF-16 for PDFium).
void AppendUtf8(std::string &s, int cp) {
    if (cp < 0x80) {
        s.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}
}  // namespace

bool Editor::IsHtmlBuffer(int buffer_id) const { return htmldocs_.find(buffer_id) != htmldocs_.end(); }

const HtmlSession *Editor::GetHtml(int buffer_id) const {
    auto it = htmldocs_.find(buffer_id);
    return it == htmldocs_.end() ? nullptr : &it->second;
}

void Editor::ResizeHtmlViewport(int buffer_id, int w, int h) {
    auto it = htmldocs_.find(buffer_id);
    if (it == htmldocs_.end()) return;
    it->second.viewport_w = w;
    it->second.viewport_h = h;
}

void Editor::ClampHtmlScroll(int buffer_id, float max_scroll) {
    auto it = htmldocs_.find(buffer_id);
    if (it == htmldocs_.end()) return;
    it->second.scroll_y = std::clamp(it->second.scroll_y, 0.0f, max_scroll);
}

// Parses `bytes` into `sess`'s DOM and runs its scripts -- shared by
// OpenHtmlInPlace's create-branch (a fresh HtmlSession) and
// ReloadHtmlBuffer (an existing one, overwritten in place). Runs any
// <script> content once, synchronously, right after parsing -- see
// RunScripts' own header (js_engine.h) for why a textContent mutation it
// makes doesn't need ComputeStyles re-run, and why a failing script can't
// leave the DOM half-mutated in a way that matters here.
void Editor::PopulateHtmlSession(HtmlSession &sess, const std::string &origin, const std::string &source,
                                  const unsigned char *bytes, size_t len) {
    sess.origin = origin;
    sess.source = source;
    sess.doc = HtmlDoc();
    ParseHtml(std::string(reinterpret_cast<const char *>(bytes), len), sess.doc);
    RunScripts(
        sess.doc, [this](const std::string &msg) { Notify(msg, NotifyLevel::Info); },
        [this, source](const std::string &msg) { Notify(source + ": " + msg, NotifyLevel::Error); });
    sess.scroll_y = 0;
}

// Dedup is by `source`, not by Buffer::filename the way OpenImageInPlace/
// OpenPdfInPlace dedup by their own `path` -- for a real local .html file
// they're the same string anyway, but mep.browse_command (kBuiltinTextTools)
// fetches a remote URL to a fresh temp file per open specifically so this
// dedup can never mistake stale cached content for a re-fetch: reusing the
// same temp path across two different fetches would otherwise silently
// show the *first* fetch's content again, since (like Image/Pdf) an
// existing session is reused as-is rather than re-parsed.
void Editor::OpenHtmlInPlace(const std::string &origin, const std::string &source, const unsigned char *bytes,
                              size_t len) {
    int buffer_id = -1;
    for (const auto &[id, sess] : htmldocs_) {
        if (sess.source == source) {
            buffer_id = id;
            break;
        }
    }
    if (buffer_id < 0) {
        // A plain-text buffer may already exist for this exact path (the
        // Ctrl-E/Ctrl-V view toggle, or `:e` force_text -- see
        // ConvertHtmlBufferToText/LoadFile's own comments) -- reuse it
        // instead of pushing a second buffer with the same filename,
        // which FindOrCreateBuffer's dedup (by Buffer::filename alone)
        // couldn't tell apart from this one afterward.
        for (size_t i = 0; i < buffers_.size(); i++) {
            if (buffers_[i].filename == source) {
                buffer_id = static_cast<int>(i);
                break;
            }
        }
    }
    if (buffer_id < 0) {
        buffer_id = CreateEmptyBuffer();
        buffers_[buffer_id].filename = source;
    }
    if (!IsHtmlBuffer(buffer_id)) {
        buffers_[buffer_id].lines.clear();
        HtmlSession sess;
        sess.buffer_id = buffer_id;
        PopulateHtmlSession(sess, origin, source, bytes, len);
        htmldocs_[buffer_id] = std::move(sess);
    }
    CurPane().buffer_id = buffer_id;
    CurPane().cursor = {0, 0};
    CurPane().scroll_row = 0;
    status_message_.clear();
    // Unlike OpenImageInPlace/OpenPdfInPlace (whose only caller, LoadFile,
    // already calls this right afterward itself), this has exactly one
    // caller (l_html_open, lua_env.cpp) that's outside the Editor class
    // and so can't reach this private method directly -- simplest to just
    // call it here instead of also exposing a public wrapper for one use.
    SyncModeToActivePaneBuffer();
}

// Reload (mep.browse_reload) and address-bar navigation
// (mep.browse_open_bar) both funnel through this -- see its own header
// in editor.h. Unlike OpenHtmlInPlace, no dedup lookup and no new
// buffer/pane switch: `buffer_id` must already be a live HTML session, or
// this is a silent no-op (the Lua caller already checked via
// mep.html_current_origin() before getting here, so this shouldn't
// normally happen -- but the pane could in principle have been closed
// between that check and a slow curl fetch completing).
void Editor::ReloadHtmlBuffer(int buffer_id, const std::string &origin, const std::string &source,
                               const unsigned char *bytes, size_t len) {
    auto it = htmldocs_.find(buffer_id);
    if (it == htmldocs_.end()) return;
    buffers_[buffer_id].filename = source;
    PopulateHtmlSession(it->second, origin, source, bytes, len);
}

void Editor::ConvertHtmlBufferToText(int buffer_id) {
    auto it = htmldocs_.find(buffer_id);
    if (it == htmldocs_.end()) return;
    std::string path = buffers_[buffer_id].filename;
    std::vector<std::string> lines;
#if defined(__EMSCRIPTEN__)
    char *result = mep_js_read_file(path.c_str());
    std::string res(result);
    std::free(result);
    if (res.rfind("OK\n", 0) == 0) lines = SplitIntoLines(res.substr(3));
#else
    std::ifstream in(path, std::ios::binary);
    if (in) {
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        lines = SplitIntoLines(content);
    }
#endif
    htmldocs_.erase(it);
    buffers_[buffer_id].lines = std::move(lines);
    // Bumps mep.buffer_change_epoch() so the polling mep.on_buffer_changed
    // hooks (syntax highlighting, colorizer, folds, ...) notice this
    // buffer now has real content -- without it they stay silent, since
    // the buffer_id/filename are unchanged across the toggle (nothing
    // else about this looks like the "switched to a different file"
    // event those hooks otherwise key on) and this bypasses the normal
    // edit path (PushUndo) that would bump it. Deliberately not routed
    // through PushUndo itself: this isn't a user edit, so it shouldn't
    // add an undo-stack entry a later 'u' would land on.
    change_epoch_++;
}

void Editor::ConvertTextBufferToHtml(int buffer_id) {
    if (IsHtmlBuffer(buffer_id)) return;
    std::string content;
    for (const auto &l : buffers_[buffer_id].lines) {
        content += l;
        content += "\n";
    }
    const std::string &path = buffers_[buffer_id].filename;
    HtmlSession sess;
    sess.buffer_id = buffer_id;
    PopulateHtmlSession(sess, path, path, reinterpret_cast<const unsigned char *>(content.data()), content.size());
    htmldocs_[buffer_id] = std::move(sess);
    buffers_[buffer_id].lines.clear();
    // See ConvertHtmlBufferToText's own comment on this bump.
    change_epoch_++;
}

void Editor::HandleHtmlInput() {
    HtmlSession *sess = nullptr;
    {
        auto it = htmldocs_.find(CurPane().buffer_id);
        if (it == htmldocs_.end()) {
            mode_ = Mode::Normal;
            return;
        }
        sess = &it->second;
    }

    constexpr float kScrollStep = 60.0f;
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    // IsKeyPressed(Repeat) rather than draining GetKeyPressed(): GLFW only
    // enqueues the initial key-down into the GetKeyPressed() queue, so
    // holding a key down (OS auto-repeat) would otherwise scroll exactly
    // once -- same reasoning as HandleImageInput's own `held` helper.
    auto held = [](int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); };
    if (held(KEY_J) || held(KEY_DOWN)) sess->scroll_y += kScrollStep;
    if (held(KEY_K) || held(KEY_UP)) sess->scroll_y = std::max(0.0f, sess->scroll_y - kScrollStep);
    if ((ctrl && IsKeyPressed(KEY_D)) || IsKeyPressed(KEY_PAGE_DOWN)) {
        sess->scroll_y += static_cast<float>(sess->viewport_h) * 0.5f;
    }
    if ((ctrl && IsKeyPressed(KEY_U)) || IsKeyPressed(KEY_PAGE_UP)) {
        sess->scroll_y = std::max(0.0f, sess->scroll_y - static_cast<float>(sess->viewport_h) * 0.5f);
    }
    // Mirrors PdfSession::theme_colors' own Ctrl-R toggle (HandlePdfInput) --
    // same key, same "the editor's color scheme takes over" meaning, and
    // same default (true; see HtmlSession::theme_colors' own comment for
    // why). The actual recoloring is entirely a DrawPane concern
    // (main.cpp); this just flips the flag.
    if (ctrl && IsKeyPressed(KEY_R)) sess->theme_colors = !sess->theme_colors;
    // Ctrl-E: escape hatch to the plain-text view of this same file --
    // the opposite direction of HandleNormalInput's own Ctrl-V on an
    // .html/.htm buffer (see ConvertHtmlBufferToText's own comment).
    // `sess` is dangling after this (its HtmlSession got erased), so
    // nothing below may touch it -- return immediately.
    if (ctrl && IsKeyPressed(KEY_E)) {
        ConvertHtmlBufferToText(CurPane().buffer_id);
        CurPane().cursor = {0, 0};
        CurPane().scroll_row = 0;
        status_message_.clear();
        SyncModeToActivePaneBuffer();
        return;
    }

    // ':' and the leader key still work while parked on an html pane, same
    // as every other flat single-mode viewer (Mode::Image's own comment);
    // +/-/= zoom mirrors HandleImageInput's own char-drain convention for
    // those three keys. 'r'/'o' dispatch to a registered Lua command
    // (MepBrowseReload/MepBrowseOpen, kBuiltinTextTools) the same
    // lua_commands_ lookup + CallRefWithString pattern
    // TryRunOrgBabelAtCursor/TryRunOrgExport use -- the actual reload
    // (re-fetch if remote) / address-bar (mep.ui_input prompt) logic
    // lives in Lua, this just triggers it. Every other printable key is a
    // deliberate no-op -- there's no text to insert/operate on.
    int cp = GetCharPressed();
    while (cp > 0) {
        if (cp == ':') {
            EnterCommand();
            return;  // mode_ is no longer Html -- stop draining as this mode
        } else if (cp == static_cast<int>(leader_key_) && !whichkey_bindings_.empty()) {
            TriggerWhichKey();
            return;
        } else if (cp == '+') {
            sess->zoom = std::min(3.0f, sess->zoom + 0.1f);
        } else if (cp == '-') {
            sess->zoom = std::max(0.3f, sess->zoom - 0.1f);
        } else if (cp == '=') {
            sess->zoom = 1.0f;
        } else if (cp == 'r') {
            auto it = lua_commands_.find("MepBrowseReload");
            if (it != lua_commands_.end() && lua_) lua_->CallRefWithString(it->second, "");
        } else if (cp == 'o') {
            auto it = lua_commands_.find("MepBrowseOpen");
            if (it != lua_commands_.end() && lua_) lua_->CallRefWithString(it->second, "");
        }
        cp = GetCharPressed();
    }
}

bool Editor::IsPdfBuffer(int buffer_id) const { return pdfs_.find(buffer_id) != pdfs_.end(); }

const PdfSession *Editor::GetPdf(int buffer_id) const {
    auto it = pdfs_.find(buffer_id);
    return it == pdfs_.end() ? nullptr : &it->second;
}

std::pair<double, double> Editor::PdfPageSizePt(PdfSession &sess, int page_index) {
    auto it = sess.page_size_pt.find(page_index);
    if (it != sess.page_size_pt.end()) return it->second;
    double w = 0, h = 0;
    if (sess.doc) {
        w = sess.doc->PageWidthPt(page_index);
        h = sess.doc->PageHeightPt(page_index);
    }
    auto result = std::make_pair(w, h);
    sess.page_size_pt[page_index] = result;
    return result;
}

float Editor::PdfPageScreenHeightPx(PdfSession &sess, int page_index) {
    return static_cast<float>(PdfPageSizePt(sess, page_index).second * sess.rendered_scale * sess.zoom);
}

void Editor::ResizePdfViewport(int buffer_id, int w, int h) {
    auto it = pdfs_.find(buffer_id);
    if (it == pdfs_.end()) return;
    PdfSession &sess = it->second;
    sess.viewport_w = w;
    sess.viewport_h = h;
    if (!sess.doc || sess.doc->PageCount() <= 0) return;
    double page_w_pt = PdfPageSizePt(sess, sess.page).first;
    int page_w_px = static_cast<int>(page_w_pt * sess.rendered_scale * sess.zoom);
    int max_pan_x = std::max(0, page_w_px - w);
    sess.pan_x = std::clamp(sess.pan_x, 0, max_pan_x);
}

void Editor::EnsurePdfPagesRastered(int buffer_id) {
    auto it = pdfs_.find(buffer_id);
    if (it == pdfs_.end()) return;
    PdfSession &sess = it->second;
    if (!sess.doc) return;
    int page_count = sess.doc->PageCount();
    if (page_count <= 0) return;

    int lo = std::max(0, sess.page - 1);
    int hi = std::min(page_count - 1, sess.page + 1);
    for (int idx = lo; idx <= hi; idx++) {
        if (sess.rasters.find(idx) != sess.rasters.end()) continue;
        PdfSession::PageRaster pr;
        if (!sess.doc->RenderPage(idx, sess.rendered_scale, pr.rgba, pr.w, pr.h)) continue;
        pr.generation = sess.next_raster_generation++;
        if (!sess.search_matches.empty()) pr.highlights = sess.doc->MatchRectsForPage(idx, sess.rendered_scale, sess.search_matches);
        sess.rasters[idx] = std::move(pr);
    }
    for (auto rit = sess.rasters.begin(); rit != sess.rasters.end();) {
        if (rit->first < lo || rit->first > hi) rit = sess.rasters.erase(rit);
        else ++rit;
    }
}

void Editor::RecomputePdfPageHighlights(PdfSession &sess) {
    if (!sess.doc) return;
    for (auto &kv : sess.rasters) {
        kv.second.highlights =
            sess.search_matches.empty() ? std::vector<PdfHighlightRect>{}
                                         : sess.doc->MatchRectsForPage(kv.first, sess.rendered_scale, sess.search_matches);
    }
}

void Editor::RunPdfSearch(PdfSession &sess, const std::string &query) {
    sess.search_query = query;
    sess.search_matches = sess.doc ? sess.doc->Search(query) : std::vector<PdfTextMatch>{};
    sess.search_current = -1;
    RecomputePdfPageHighlights(sess);
}

void Editor::GotoPdfMatch(PdfSession &sess, int index) {
    if (sess.search_matches.empty() || !sess.doc) return;
    int n = static_cast<int>(sess.search_matches.size());
    index = ((index % n) + n) % n;
    sess.search_current = index;
    const PdfTextMatch &m = sess.search_matches[static_cast<size_t>(index)];
    sess.page = m.page;
    sess.pan_x = 0;
    sess.scroll_y = 0;
    if (!m.rects_pt.empty()) {
        // One-off conversion (not sess.rasters[m.page].highlights, which
        // may not be cached -- this jump can land far outside the current
        // {page-1,page,page+1} window) just to position scroll_y; roughly
        // centers the match vertically rather than only bringing the page
        // top into view.
        std::vector<PdfTextMatch> just_this = {m};
        auto rects = sess.doc->MatchRectsForPage(m.page, sess.rendered_scale, just_this);
        if (!rects.empty()) {
            sess.scroll_y = rects.front().y0 - static_cast<float>(sess.viewport_h) / 2.0f;
            if (sess.scroll_y < 0) sess.scroll_y = 0;
        }
    }
}

void Editor::OpenPdfInPlace(const std::string &path, const unsigned char *bytes, size_t len) {
    int buffer_id = -1;
    for (size_t i = 0; i < buffers_.size(); i++) {
        if (!buffers_[i].filename.empty() && buffers_[i].filename == path) {
            buffer_id = static_cast<int>(i);
            break;
        }
    }
    if (buffer_id < 0) {
        auto doc = std::make_unique<PdfDoc>();
        if (!doc->LoadFromMemory(bytes, len)) {
            status_message_ = "E-\"" + path + "\": " + doc->Error();
            return;
        }
        buffer_id = CreateEmptyBuffer();
        buffers_[buffer_id].filename = path;
        PdfSession sess;
        sess.buffer_id = buffer_id;
        sess.doc = std::move(doc);
        sess.rendered_scale = 144.0f / 72.0f;  // baseline ~144dpi, same as before
        pdfs_[buffer_id] = std::move(sess);
        // No render call here -- EnsurePdfPagesRastered (called every frame
        // from DrawPane, including the first) handles it lazily.
    }
    CurPane().buffer_id = buffer_id;
    CurPane().cursor = {0, 0};
    CurPane().scroll_row = 0;
    pending_g_ = false;  // avoid gg/G leakage from whatever mode preceded this
    status_message_.clear();
}

// Crosses `page` forward/backward as scroll_y drifts past the current
// anchor page's on-screen bounds, re-basing scroll_y relative to the new
// anchor each time -- this is what makes j/k (or the mouse wheel) held
// down scroll smoothly *through* page boundaries instead of stopping dead
// at each one. Also called after zoom changes (viewport-relative math can
// push scroll_y out of range even without a direct scroll key) and clamps
// at the very top of page 0 / bottom of the last page. A no-op if the
// document has no pages.
void Editor::RebasePdfScroll(PdfSession &sess) {
    int page_count = sess.doc ? sess.doc->PageCount() : 0;
    if (page_count <= 0) return;
    auto page_screen_h = [&](int idx) { return PdfPageScreenHeightPx(sess, idx); };
    for (;;) {
        float cur_h = page_screen_h(sess.page);
        if (sess.scroll_y >= cur_h + kPdfPageGapPx && sess.page + 1 < page_count) {
            sess.scroll_y -= (cur_h + kPdfPageGapPx);
            sess.page += 1;
        } else if (sess.scroll_y < 0 && sess.page > 0) {
            sess.page -= 1;
            sess.scroll_y += (page_screen_h(sess.page) + kPdfPageGapPx);
        } else {
            break;
        }
    }
    if (sess.page == 0 && sess.scroll_y < 0) sess.scroll_y = 0;
    if (sess.page == page_count - 1) {
        float cur_h = page_screen_h(sess.page);
        if (sess.scroll_y > cur_h) sess.scroll_y = cur_h;
    }
}

// Clamps pan_x against the anchor page's on-screen width at the current
// zoom/rendered_scale.
void Editor::ClampPdfPanX(PdfSession &sess) {
    double page_w_pt = PdfPageSizePt(sess, sess.page).first;
    int mx = std::max(0, static_cast<int>(page_w_pt * sess.rendered_scale * sess.zoom) - sess.viewport_w);
    sess.pan_x = std::clamp(sess.pan_x, 0, mx);
}

void Editor::SettlePdfZoom(PdfSession &sess) {
    if (sess.zoom < kZoomSettleLo || sess.zoom > kZoomSettleHi) {
        sess.rendered_scale *= sess.zoom;
        sess.zoom = 1.0f;
        sess.rasters.clear();
    }
}

void Editor::ApplyPdfZoom(PdfSession &sess, float new_zoom) {
    new_zoom = std::clamp(new_zoom, kMinPdfZoom, kMaxPdfZoom);
    float ratio = new_zoom / sess.zoom;
    float center_x = sess.pan_x + sess.viewport_w / 2.0f;
    float center_y = sess.scroll_y + sess.viewport_h / 2.0f;
    sess.zoom = new_zoom;
    sess.pan_x = static_cast<int>(center_x * ratio - sess.viewport_w / 2.0f);
    sess.scroll_y = center_y * ratio - sess.viewport_h / 2.0f;
    SettlePdfZoom(sess);
    RebasePdfScroll(sess);
    ClampPdfPanX(sess);
}

void Editor::HandlePdfInput() {
    PdfSession *sess = nullptr;
    {
        auto it = pdfs_.find(CurPane().buffer_id);
        if (it == pdfs_.end()) {
            mode_ = Mode::Normal;
            return;
        }
        sess = &it->second;
    }
    if (sess->search_active) {
        HandlePdfSearchInput(*sess);
        return;
    }
    int page_count = sess->doc ? sess->doc->PageCount() : 0;
    if (page_count <= 0) return;

    auto rebase_scroll = [&]() { RebasePdfScroll(*sess); };
    auto clamp_pan_x = [&]() { ClampPdfPanX(*sess); };

    constexpr int kScrollStep = 40;
    // Same IsKeyPressed||IsKeyPressedRepeat reasoning as HandleImageInput --
    // GetKeyPressed() only fires on initial key-down, not OS auto-repeat.
    auto held = [](int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); };
    bool scrolled = false;
    if (held(KEY_J) || held(KEY_DOWN)) { sess->scroll_y += kScrollStep; scrolled = true; }
    if (held(KEY_K) || held(KEY_UP)) { sess->scroll_y -= kScrollStep; scrolled = true; }
    if (scrolled) rebase_scroll();
    if (held(KEY_H) || held(KEY_LEFT)) { sess->pan_x -= kScrollStep; clamp_pan_x(); }
    if (held(KEY_L) || held(KEY_RIGHT)) { sess->pan_x += kScrollStep; clamp_pan_x(); }

    auto goto_page = [&](int new_page) {
        sess->page = std::clamp(new_page, 0, page_count - 1);
        sess->scroll_y = 0;
    };

    // Ctrl-f/Ctrl-b/Ctrl-r: same GetKeyPressed()-drain-while-ctrl-held
    // pattern as HandleNormalInput's own Ctrl-combos (IsKeyPressed() alone
    // was found flaky for these under slow/software-rendered frames -- see
    // its comment at this function's normal-mode counterpart).
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    bool next_page = false, prev_page = false, toggle_theme = false;
    if (ctrl && IsKeyPressed(KEY_D)) { sess->scroll_y += static_cast<float>(sess->viewport_h) * 0.5f; rebase_scroll(); }
    if (ctrl && IsKeyPressed(KEY_U)) { sess->scroll_y -= static_cast<float>(sess->viewport_h) * 0.5f; rebase_scroll(); }
    for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed()) {
        if (key == KEY_PAGE_DOWN) next_page = true;
        else if (key == KEY_PAGE_UP) prev_page = true;
        else if (ctrl && key == KEY_F) next_page = true;
        else if (ctrl && key == KEY_B) prev_page = true;
        else if (ctrl && key == KEY_R) toggle_theme = true;
    }
    if (next_page) goto_page(sess->page + 1);
    if (prev_page) goto_page(sess->page - 1);
    if (toggle_theme) sess->theme_colors = !sess->theme_colors;

    // +/-/= zoom: same center-anchored shape as HandleImageInput's
    // apply_zoom. Folds drift outside [kZoomSettleLo, kZoomSettleHi] into
    // rendered_scale and clears the raster cache so every cached page gets
    // re-rendered at the new native resolution (text stays crisp instead
    // of blurring) -- scroll_y/pan_x themselves don't need rescaling for
    // this, since rendered_scale absorbs exactly the zoom drift being
    // removed, leaving each page's on-screen size unchanged. SettlePdfZoom/
    // ApplyPdfZoom (editor.h) do the actual work -- also reused by
    // HandleMouseWheel's Pdf branch for Ctrl-scroll.
    auto settle_zoom = [&]() { SettlePdfZoom(*sess); };
    auto apply_zoom = [&](float new_zoom) { ApplyPdfZoom(*sess, new_zoom); };

    int cp = GetCharPressed();
    while (cp > 0) {
        if (cp == ':') {
            EnterCommand();
            return;  // mode_ is no longer Pdf -- stop draining as this mode
        } else if (cp == static_cast<int>(leader_key_) && !whichkey_bindings_.empty()) {
            TriggerWhichKey();
            return;
        } else if (cp == '+') {
            apply_zoom(sess->zoom * kPdfZoomStep);
        } else if (cp == '-') {
            apply_zoom(sess->zoom / kPdfZoomStep);
        } else if (cp == '=' && sess->viewport_w > 0 && sess->viewport_h > 0) {
            auto [pw, ph] = PdfPageSizePt(*sess, sess->page);
            double base_w = pw * sess->rendered_scale, base_h = ph * sess->rendered_scale;
            if (base_w > 0 && base_h > 0) {
                float fit = std::min(static_cast<float>(sess->viewport_w) / static_cast<float>(base_w),
                                      static_cast<float>(sess->viewport_h) / static_cast<float>(base_h));
                sess->zoom = std::clamp(fit, kMinPdfZoom, kMaxPdfZoom);
                sess->pan_x = 0;
                sess->scroll_y = 0;
                settle_zoom();
            }
        } else if (cp == 'g') {
            // gg -> first page, reusing pending_g_ the same way normal mode's
            // own gg does (see HandleNormalInput) -- reset on entry to this
            // mode in OpenPdfInPlace so it can't leak in from elsewhere.
            if (pending_g_) {
                pending_g_ = false;
                goto_page(0);
            } else {
                pending_g_ = true;
            }
        } else if (cp == 'G') {
            pending_g_ = false;
            goto_page(page_count - 1);
        } else if (cp == '/') {
            pending_g_ = false;
            sess->search_active = true;
            sess->search_input.clear();
        } else if (cp == 'n' && !sess->search_matches.empty()) {
            pending_g_ = false;
            GotoPdfMatch(*sess, sess->search_current + 1);
        } else if (cp == 'p' && !sess->search_matches.empty()) {
            pending_g_ = false;
            GotoPdfMatch(*sess, sess->search_current - 1);
        } else {
            pending_g_ = false;
        }
        // Every other printable key is a deliberate no-op -- see
        // Mode::Pdf's own comment for why (read-only content).
        cp = GetCharPressed();
    }
}

void Editor::HandlePdfSearchInput(PdfSession &sess) {
    if (IsKeyPressed(KEY_ESCAPE)) {
        // Cancel: discard the in-progress query text, but leave any prior
        // *completed* search (search_query/search_matches/highlights)
        // exactly as it was -- matches vim's own '/' escape behavior.
        sess.search_active = false;
        return;
    }
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
        sess.search_active = false;
        RunPdfSearch(sess, sess.search_input);
        if (!sess.search_matches.empty()) {
            // First match at/after the current page; wrap to the very
            // first match in the document if the rest of it has none.
            int start = 0;
            for (size_t i = 0; i < sess.search_matches.size(); i++) {
                if (sess.search_matches[i].page >= sess.page) {
                    start = static_cast<int>(i);
                    break;
                }
            }
            GotoPdfMatch(sess, start);
        }
        return;
    }
    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        if (!sess.search_input.empty()) sess.search_input.pop_back();
    }
    for (int cp = GetCharPressed(); cp > 0; cp = GetCharPressed()) AppendUtf8(sess.search_input, cp);
}

// --- WYSIWYG office-document panes ------------------------------------------

bool Editor::IsOfficeBuffer(int buffer_id) const { return officedocs_.find(buffer_id) != officedocs_.end(); }

const OfficeSession *Editor::GetOffice(int buffer_id) const {
    auto it = officedocs_.find(buffer_id);
    return it == officedocs_.end() ? nullptr : &it->second;
}

void Editor::ResizeOfficeViewport(int buffer_id, int w, int h) {
    auto it = officedocs_.find(buffer_id);
    if (it == officedocs_.end()) return;
    OfficeSession &sess = it->second;
    sess.viewport_w = w;
    sess.viewport_h = h;
    int max_para = std::max(0, static_cast<int>(sess.doc.paragraphs.size()) - 1);
    sess.scroll_para = std::clamp(sess.scroll_para, 0, max_para);
}

void Editor::SetOfficeScroll(int buffer_id, int scroll_para, int scroll_line_in_para) {
    auto it = officedocs_.find(buffer_id);
    if (it == officedocs_.end()) return;
    OfficeSession &sess = it->second;
    int max_para = std::max(0, static_cast<int>(sess.doc.paragraphs.size()) - 1);
    sess.scroll_para = std::clamp(scroll_para, 0, max_para);
    sess.scroll_line_in_para = std::max(0, scroll_line_in_para);
}

void Editor::SetOfficeScrollFollowCursorPara(int buffer_id, int cursor_para) {
    auto it = officedocs_.find(buffer_id);
    if (it == officedocs_.end()) return;
    it->second.scroll_follow_last_cursor_para = cursor_para;
}

void Editor::OpenOfficeInPlace(const std::string &path, const unsigned char *bytes, size_t len) {
    int buffer_id = -1;
    for (size_t i = 0; i < buffers_.size(); i++) {
        if (!buffers_[i].filename.empty() && buffers_[i].filename == path) {
            buffer_id = static_cast<int>(i);
            break;
        }
    }
    if (buffer_id < 0) {
        OfficeDoc doc;
        std::string error;
        bool ok = false;
        if (IsDocxPath(path)) {
            ok = LoadDocxFromMemory(bytes, len, doc, error);
        } else if (IsOdtPath(path)) {
            ok = LoadOdtFromMemory(bytes, len, doc, error);
        } else {
            error = "unsupported office document format";
        }
        if (!ok) {
            status_message_ = "E-\"" + path + "\": " + error;
            return;
        }
        buffer_id = CreateEmptyBuffer();
        buffers_[buffer_id].filename = path;
        OfficeSession sess;
        sess.buffer_id = buffer_id;
        sess.doc = std::move(doc);
        sess.original_bytes.assign(bytes, bytes + len);
        officedocs_[buffer_id] = std::move(sess);
    }
    CurPane().buffer_id = buffer_id;
    CurPane().cursor = {0, 0};
    CurPane().scroll_row = 0;
    pending_g_ = false;  // avoid gg/G leakage from whatever mode preceded this
    status_message_.clear();
}

void Editor::HandleOfficeNormalInput() {
    OfficeSession *sess = nullptr;
    {
        auto it = officedocs_.find(CurPane().buffer_id);
        if (it == officedocs_.end()) {
            mode_ = Mode::Normal;
            return;
        }
        sess = &it->second;
    }
    int para_count = static_cast<int>(sess->doc.paragraphs.size());
    if (para_count <= 0) return;

    auto cur_len = [&]() { return static_cast<int>(sess->doc.paragraphs[sess->cursor_para].text.size()); };
    auto goto_para = [&](int new_para) {
        int clamped = std::clamp(new_para, 0, para_count - 1);
        // Only auto-enter a table on an actual paragraph change -- a
        // no-op goto_para (already at the first/last paragraph, so the
        // clamp lands back where the cursor already was) must NOT
        // re-trigger entry, otherwise Escape-ing out of a table anchored
        // at the very start/end of the document, then pressing k/j to
        // leave, would immediately re-trap the cursor right back inside
        // it since there's nowhere left to clamp to.
        bool moved = clamped != sess->cursor_para;
        sess->cursor_para = clamped;
        sess->cursor_col = std::min(sess->cursor_col, cur_len());
        // A table/image anchor is a single paragraph-motion step, never
        // entered implicitly by landing on it -- but a *table* anchor is
        // immediately enterable (hjkl/Tab over cells) the moment the
        // cursor lands on its paragraph, since there's no other content on
        // that paragraph to edit; an image anchor has nothing further to
        // "enter" (no per-image editing in v1).
        if (moved) {
            int tref = sess->doc.paragraphs[sess->cursor_para].table_ref;
            if (tref >= 0) EnterOfficeTable(tref);
        }
    };

    auto held = [](int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); };

    // Table-cell navigation mode: while the cursor is anchored on a table
    // (in_table_edit >= 0) and not currently editing one cell's text,
    // hjkl/arrows/Tab move between cells instead of moving the paragraph
    // cursor; stepping off the table's top/bottom row exits it and resumes
    // normal paragraph motion on the neighboring paragraph (so the table
    // still acts like a single step from outside, matching office_doc.h's
    // own "anchored table/image is a single step" comment) while stepping
    // off its left/right edge simply clamps in place, matching this
    // codebase's existing column-motion-clamps-at-line-ends convention.
    if (sess->in_table_edit >= 0 && !sess->table_cell_editing) {
        if (IsKeyPressed(KEY_ESCAPE)) {
            ExitOfficeTable();
            return;
        }
        const DocTable &t = sess->doc.tables[static_cast<size_t>(sess->in_table_edit)];
        if (held(KEY_J) || held(KEY_DOWN)) {
            if (sess->table_cursor_row + 1 < t.rows) {
                MoveOfficeTableCell(1, 0);
            } else {
                ExitOfficeTable();
                goto_para(sess->cursor_para + 1);
                sess->cursor_col = 0;
            }
        } else if (held(KEY_K) || held(KEY_UP)) {
            if (sess->table_cursor_row > 0) {
                MoveOfficeTableCell(-1, 0);
            } else {
                ExitOfficeTable();
                goto_para(sess->cursor_para - 1);
                sess->cursor_col = cur_len();
            }
        } else if (held(KEY_H) || held(KEY_LEFT)) {
            MoveOfficeTableCell(0, -1);
        } else if (held(KEY_L) || held(KEY_RIGHT)) {
            MoveOfficeTableCell(0, 1);
        } else if (held(KEY_TAB)) {
            bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
            if (!shift) {
                if (sess->table_cursor_col + 1 < t.cols) MoveOfficeTableCell(0, 1);
                else if (sess->table_cursor_row + 1 < t.rows) MoveOfficeTableCell(1, -sess->table_cursor_col);
            } else {
                if (sess->table_cursor_col > 0) MoveOfficeTableCell(0, -1);
                else if (sess->table_cursor_row > 0) MoveOfficeTableCell(-1, t.cols - 1 - sess->table_cursor_col);
            }
        }
        int tcp = GetCharPressed();
        while (tcp > 0) {
            if (tcp == 'i') {
                PushUndoOffice();
                sess->table_cell_editing = true;
                sess->table_cell_col = 0;
                mode_ = Mode::OfficeInsert;
                return;
            } else if (tcp == 'a') {
                PushUndoOffice();
                sess->table_cell_editing = true;
                sess->table_cell_col =
                    static_cast<int>(t.Cell(sess->table_cursor_row, sess->table_cursor_col).size());
                mode_ = Mode::OfficeInsert;
                return;
            } else if (tcp == ':') {
                EnterCommand();
                return;
            }
            tcp = GetCharPressed();
        }
        return;
    }

    // Word-wrap-oblivious navigation -- see Mode::OfficeNormal's own
    // comment for why: h/l move within the current paragraph's flat text
    // (like column motion over Buffer::lines[row]), j/k move to the
    // prev/next *paragraph* (like row motion), not to the next *visual*
    // (word-wrapped) line, which only main.cpp's renderer knows about.
    if (held(KEY_H) || held(KEY_LEFT)) sess->cursor_col = std::max(0, sess->cursor_col - 1);
    if (held(KEY_L) || held(KEY_RIGHT)) sess->cursor_col = std::min(cur_len(), sess->cursor_col + 1);
    if (held(KEY_J) || held(KEY_DOWN)) goto_para(sess->cursor_para + 1);
    if (held(KEY_K) || held(KEY_UP)) goto_para(sess->cursor_para - 1);

    int cp = GetCharPressed();
    while (cp > 0) {
        if (cp == ':') {
            EnterCommand();
            return;  // mode_ is no longer OfficeNormal -- stop draining as this mode
        } else if (cp == static_cast<int>(leader_key_) && !whichkey_bindings_.empty()) {
            TriggerWhichKey();
            return;
        } else if (cp == 'g') {
            // gg -> first paragraph, reusing pending_g_ the same way
            // normal mode's own gg and HandlePdfInput's gg do.
            if (pending_g_) {
                pending_g_ = false;
                goto_para(0);
                sess->cursor_col = 0;
            } else {
                pending_g_ = true;
            }
        } else if (cp == 'G') {
            pending_g_ = false;
            goto_para(para_count - 1);
            sess->cursor_col = 0;
        } else if (cp == '0') {
            pending_g_ = false;
            sess->cursor_col = 0;
        } else if (cp == '$') {
            pending_g_ = false;
            sess->cursor_col = cur_len();
        } else if (cp == 'i') {
            pending_g_ = false;
            PushUndoOffice();
            mode_ = Mode::OfficeInsert;
            return;
        } else if (cp == 'a') {
            pending_g_ = false;
            PushUndoOffice();
            sess->cursor_col = std::min(cur_len(), sess->cursor_col + 1);
            mode_ = Mode::OfficeInsert;
            return;
        } else if (cp == 'v') {
            pending_g_ = false;
            sess->has_selection = true;
            sess->sel_anchor_para = sess->cursor_para;
            sess->sel_anchor_col = sess->cursor_col;
            mode_ = Mode::OfficeVisual;
            return;
        } else if (cp == 'u') {
            pending_g_ = false;
            UndoOffice();
        } else {
            pending_g_ = false;
        }
        cp = GetCharPressed();
    }
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (IsKeyPressed(KEY_R) && ctrl) {
        RedoOffice();
    }
    if (ctrl && (IsKeyPressed(KEY_D) || IsKeyPressed(KEY_U))) {
        bool down = IsKeyPressed(KEY_D);
        // Paragraph count, not visual line count -- word-wrap needs
        // main.cpp's MeasureTextEx, which this raylib-model-level function
        // can't call (see ResizeOfficeViewport's own comment on why
        // scroll-follow itself lives in main.cpp), so "half a screen" is
        // approximated from the body font's line height instead of an
        // exact wrapped-line count. Reuses goto_para so it gets the same
        // clamp-without-re-entering-a-table behavior as j/k.
        float line_h = std::max(1.0f, sess->base_font_pt * sess->zoom * 1.35f);
        int half_page = std::max(1, static_cast<int>(sess->viewport_h / (2.0f * line_h)));
        goto_para(sess->cursor_para + (down ? half_page : -half_page));
    }
}

void Editor::PushUndoOffice() {
    auto it = officedocs_.find(CurPane().buffer_id);
    if (it == officedocs_.end()) return;
    OfficeSession &sess = it->second;
    sess.undo_stack.push_back(sess.doc.paragraphs);
    if (sess.undo_stack.size() > kMaxUndo) sess.undo_stack.erase(sess.undo_stack.begin());
    sess.redo_stack.clear();
}

void Editor::UndoOffice() {
    auto it = officedocs_.find(CurPane().buffer_id);
    if (it == officedocs_.end()) return;
    OfficeSession &sess = it->second;
    if (sess.undo_stack.empty()) {
        status_message_ = "Already at oldest change";
        return;
    }
    sess.redo_stack.push_back(sess.doc.paragraphs);
    sess.doc.paragraphs = sess.undo_stack.back();
    sess.undo_stack.pop_back();
    sess.modified = true;
    int max_para = std::max(0, static_cast<int>(sess.doc.paragraphs.size()) - 1);
    sess.cursor_para = std::clamp(sess.cursor_para, 0, max_para);
    sess.cursor_col = std::min(sess.cursor_col, static_cast<int>(sess.doc.paragraphs[sess.cursor_para].text.size()));
}

void Editor::RedoOffice() {
    auto it = officedocs_.find(CurPane().buffer_id);
    if (it == officedocs_.end()) return;
    OfficeSession &sess = it->second;
    if (sess.redo_stack.empty()) {
        status_message_ = "Already at newest change";
        return;
    }
    sess.undo_stack.push_back(sess.doc.paragraphs);
    sess.doc.paragraphs = sess.redo_stack.back();
    sess.redo_stack.pop_back();
    sess.modified = true;
    int max_para = std::max(0, static_cast<int>(sess.doc.paragraphs.size()) - 1);
    sess.cursor_para = std::clamp(sess.cursor_para, 0, max_para);
    sess.cursor_col = std::min(sess.cursor_col, static_cast<int>(sess.doc.paragraphs[sess.cursor_para].text.size()));
}

void Editor::HandleOfficeInsertInput() {
    OfficeSession *sess = nullptr;
    {
        auto it = officedocs_.find(CurPane().buffer_id);
        if (it == officedocs_.end()) {
            mode_ = Mode::Normal;
            return;
        }
        sess = &it->second;
    }

    // Table-cell text editing: a cell is plain text (DocTable::cells has
    // no spans/paragraphs), so this is a much smaller edit loop than the
    // paragraph one below -- no split/merge, and Enter just ends the edit
    // (same as Escape) rather than inserting a line break, since a cell
    // stays single-line in v1 (matches office_doc.h's own DocTable scope
    // note: plain text, no per-cell rich formatting).
    if (sess->in_table_edit >= 0 && sess->table_cell_editing) {
        DocTable &t = sess->doc.tables[static_cast<size_t>(sess->in_table_edit)];
        std::string &cell = t.Cell(sess->table_cursor_row, sess->table_cursor_col);
        int &cc = sess->table_cell_col;
        cc = std::clamp(cc, 0, static_cast<int>(cell.size()));
        for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed()) {
            if (key == KEY_ESCAPE || key == KEY_ENTER) {
                sess->table_cell_editing = false;
                mode_ = Mode::OfficeNormal;
                return;
            } else if (key == KEY_BACKSPACE) {
                if (cc > 0) {
                    cell.erase(static_cast<size_t>(cc - 1), 1);
                    cc--;
                    sess->modified = true;
                }
            } else if (key == KEY_DELETE) {
                if (cc < static_cast<int>(cell.size())) {
                    cell.erase(static_cast<size_t>(cc), 1);
                    sess->modified = true;
                }
            } else if (key == KEY_LEFT) {
                cc = std::max(0, cc - 1);
            } else if (key == KEY_RIGHT) {
                cc = std::min(static_cast<int>(cell.size()), cc + 1);
            }
        }
        for (int cp2 = GetCharPressed(); cp2 > 0; cp2 = GetCharPressed()) {
            if (cp2 >= 32 && cp2 <= 126) {
                cell.insert(cell.begin() + cc, static_cast<char>(cp2));
                cc++;
                sess->modified = true;
            }
        }
        return;
    }

    auto cur_len = [&]() { return static_cast<int>(sess->doc.paragraphs[sess->cursor_para].text.size()); };

    bool escape = false, enter = false, backspace = false, del = false;
    for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed()) {
        if (key == KEY_ESCAPE) escape = true;
        else if (key == KEY_ENTER) enter = true;
        else if (key == KEY_BACKSPACE) backspace = true;
        else if (key == KEY_DELETE) del = true;
    }
    if (escape) {
        mode_ = Mode::OfficeNormal;
        sess->cursor_col = std::max(0, std::min(sess->cursor_col, cur_len()));
        return;
    }

    int cp = GetCharPressed();
    while (cp > 0) {
        if (cp >= 32 && cp <= 126) {
            DocParagraph &p = sess->doc.paragraphs[sess->cursor_para];
            ApplyInsertToParagraph(p, sess->cursor_col, std::string(1, static_cast<char>(cp)));
            sess->cursor_col++;
            sess->modified = true;
        }
        cp = GetCharPressed();
    }

    if (enter || IsKeyPressedRepeat(KEY_ENTER)) {
        DocParagraph &p = sess->doc.paragraphs[sess->cursor_para];
        DocParagraph second = SplitParagraphAt(p, sess->cursor_col);
        sess->doc.paragraphs.insert(sess->doc.paragraphs.begin() + sess->cursor_para + 1, std::move(second));
        sess->cursor_para++;
        sess->cursor_col = 0;
        sess->modified = true;
    }
    if (backspace || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        if (sess->cursor_col > 0) {
            DocParagraph &p = sess->doc.paragraphs[sess->cursor_para];
            ApplyDeleteToParagraph(p, sess->cursor_col - 1, sess->cursor_col);
            sess->cursor_col--;
            sess->modified = true;
        } else if (sess->cursor_para > 0) {
            int prev_len = static_cast<int>(sess->doc.paragraphs[sess->cursor_para - 1].text.size());
            MergeParagraphs(sess->doc.paragraphs[sess->cursor_para - 1], sess->doc.paragraphs[sess->cursor_para]);
            sess->doc.paragraphs.erase(sess->doc.paragraphs.begin() + sess->cursor_para);
            sess->cursor_para--;
            sess->cursor_col = prev_len;
            sess->modified = true;
        }
    }
    if (del || IsKeyPressedRepeat(KEY_DELETE)) {
        int len = cur_len();
        if (sess->cursor_col < len) {
            DocParagraph &p = sess->doc.paragraphs[sess->cursor_para];
            ApplyDeleteToParagraph(p, sess->cursor_col, sess->cursor_col + 1);
            sess->modified = true;
        } else if (sess->cursor_para + 1 < static_cast<int>(sess->doc.paragraphs.size())) {
            MergeParagraphs(sess->doc.paragraphs[sess->cursor_para], sess->doc.paragraphs[sess->cursor_para + 1]);
            sess->doc.paragraphs.erase(sess->doc.paragraphs.begin() + sess->cursor_para + 1);
            sess->modified = true;
        }
    }
    if (IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT)) sess->cursor_col = std::max(0, sess->cursor_col - 1);
    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT)) sess->cursor_col = std::min(cur_len(), sess->cursor_col + 1);
    if (IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP)) {
        if (sess->cursor_para > 0) {
            sess->cursor_para--;
            sess->cursor_col = std::min(sess->cursor_col, cur_len());
        }
    }
    if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN)) {
        if (sess->cursor_para + 1 < static_cast<int>(sess->doc.paragraphs.size())) {
            sess->cursor_para++;
            sess->cursor_col = std::min(sess->cursor_col, cur_len());
        }
    }
}

void Editor::HandleOfficeVisualInput() {
    OfficeSession *sess = nullptr;
    {
        auto it = officedocs_.find(CurPane().buffer_id);
        if (it == officedocs_.end()) {
            mode_ = Mode::Normal;
            return;
        }
        sess = &it->second;
    }
    int para_count = static_cast<int>(sess->doc.paragraphs.size());
    if (para_count <= 0) {
        mode_ = Mode::OfficeNormal;
        return;
    }
    auto cur_len = [&]() { return static_cast<int>(sess->doc.paragraphs[sess->cursor_para].text.size()); };
    auto goto_para = [&](int new_para) {
        sess->cursor_para = std::clamp(new_para, 0, para_count - 1);
        sess->cursor_col = std::min(sess->cursor_col, cur_len());
    };
    auto held = [](int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); };
    if (held(KEY_H) || held(KEY_LEFT)) sess->cursor_col = std::max(0, sess->cursor_col - 1);
    if (held(KEY_L) || held(KEY_RIGHT)) sess->cursor_col = std::min(cur_len(), sess->cursor_col + 1);
    if (held(KEY_J) || held(KEY_DOWN)) goto_para(sess->cursor_para + 1);
    if (held(KEY_K) || held(KEY_UP)) goto_para(sess->cursor_para - 1);

    int cp = GetCharPressed();
    while (cp > 0) {
        if (cp == 'g') {
            if (pending_g_) {
                pending_g_ = false;
                goto_para(0);
                sess->cursor_col = 0;
            } else {
                pending_g_ = true;
            }
        } else if (cp == 'G') {
            pending_g_ = false;
            goto_para(para_count - 1);
            sess->cursor_col = cur_len();
        } else if (cp == '0') {
            pending_g_ = false;
            sess->cursor_col = 0;
        } else if (cp == '$') {
            pending_g_ = false;
            sess->cursor_col = cur_len();
        } else if (cp == 'b' || cp == 'i' || cp == 'u' || cp == 's') {
            // ToggleOfficeFormat does the identical Visual-selection
            // range-toggle + PushUndoOffice + return-to-OfficeNormal this
            // branch used to inline -- also reachable from the toolbar's
            // B/I/U/S buttons (main.cpp), now shared instead of duplicated.
            pending_g_ = false;
            ToggleOfficeFormat(static_cast<char>(cp));
            return;
        } else if (cp == 'c' || cp == 'L' || cp == 'r' || cp == 'f') {
            // Alignment: c=center, L=left (capital -- lowercase l is
            // cursor-right), r=right, f=full/justify (not j -- cursor-down).
            pending_g_ = false;
            DocParagraph::Align align = cp == 'c'   ? DocParagraph::Align::Center
                                         : cp == 'L' ? DocParagraph::Align::Left
                                         : cp == 'r' ? DocParagraph::Align::Right
                                                      : DocParagraph::Align::Justify;
            SetOfficeAlignment(align);
            return;
        } else if (cp == '*' || cp == '#') {
            pending_g_ = false;
            SetOfficeListKind(cp == '*' ? DocParagraph::ListKind::Bullet : DocParagraph::ListKind::Numbered);
            return;
        } else if (cp == '^' || cp == '_') {
            pending_g_ = false;
            if (cp == '^') ToggleOfficeSuperscript(); else ToggleOfficeSubscript();
            return;
        } else {
            pending_g_ = false;
        }
        cp = GetCharPressed();
    }

    for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed()) {
        if (key == KEY_ESCAPE) {
            sess->has_selection = false;
            mode_ = Mode::OfficeNormal;
            return;
        }
    }
}

void Editor::ToggleOfficeFormat(char which) {
    auto it = officedocs_.find(CurPane().buffer_id);
    if (it == officedocs_.end()) return;
    OfficeSession &sess = it->second;
    int para_count = static_cast<int>(sess.doc.paragraphs.size());
    if (para_count <= 0) return;
    bool DocFormat::*field = (which == 'b')   ? &DocFormat::bold
                              : (which == 'i') ? &DocFormat::italic
                              : (which == 's') ? &DocFormat::strike
                                                : &DocFormat::underline;
    PushUndoOffice();
    if (mode_ == Mode::OfficeVisual && sess.has_selection) {
        int ap = sess.sel_anchor_para, ac = sess.sel_anchor_col;
        int cp = sess.cursor_para, cc = sess.cursor_col;
        int pa, ca, pb, cb;
        if (ap < cp || (ap == cp && ac <= cc)) {
            pa = ap; ca = ac; pb = cp; cb = cc;
        } else {
            pa = cp; ca = cc; pb = ap; cb = ac;
        }
        if (pa == pb) {
            ToggleFormatOverRange(sess.doc.paragraphs[pa], ca, cb, field);
        } else {
            ToggleFormatOverRange(sess.doc.paragraphs[pa], ca, static_cast<int>(sess.doc.paragraphs[pa].text.size()),
                                   field);
            for (int pi = pa + 1; pi < pb; pi++) {
                ToggleFormatOverRange(sess.doc.paragraphs[pi], 0, static_cast<int>(sess.doc.paragraphs[pi].text.size()),
                                       field);
            }
            ToggleFormatOverRange(sess.doc.paragraphs[pb], 0, cb, field);
        }
        sess.has_selection = false;
        sess.cursor_para = pa;
        sess.cursor_col = ca;
        mode_ = Mode::OfficeNormal;
    } else {
        int len = static_cast<int>(sess.doc.paragraphs[sess.cursor_para].text.size());
        int a = std::clamp(sess.cursor_col, 0, len);
        int b = std::min(a + 1, len);
        if (b > a) ToggleFormatOverRange(sess.doc.paragraphs[sess.cursor_para], a, b, field);
    }
    sess.modified = true;
}

bool Editor::OfficeFormatActive(char which) const {
    auto it = officedocs_.find(CurPane().buffer_id);
    if (it == officedocs_.end()) return false;
    const OfficeSession &sess = it->second;
    int para_count = static_cast<int>(sess.doc.paragraphs.size());
    if (para_count <= 0) return false;
    auto field_of = [&](const DocFormat &f) {
        return which == 'b' ? f.bold : which == 'i' ? f.italic : which == 's' ? f.strike : f.underline;
    };
    int cp, col;
    if (mode_ == Mode::OfficeVisual && sess.has_selection) {
        int ap = sess.sel_anchor_para, ac = sess.sel_anchor_col;
        int ccp = sess.cursor_para, ccc = sess.cursor_col;
        // Samples the format at the selection's first character -- an
        // approximation of ToggleFormatOverRange's real all-on check,
        // good enough for a toolbar's pressed-look without re-walking
        // every span in the selection each frame.
        if (ap < ccp || (ap == ccp && ac <= ccc)) {
            cp = ap; col = ac;
        } else {
            cp = ccp; col = ccc;
        }
    } else {
        cp = sess.cursor_para;
        col = sess.cursor_col;
    }
    cp = std::clamp(cp, 0, para_count - 1);
    col = std::min(col, static_cast<int>(sess.doc.paragraphs[cp].text.size()));
    return field_of(FormatAt(sess.doc.paragraphs[cp], col));
}

void Editor::ApplyOfficeFormatFieldOverSelection(const std::function<void(DocFormat &)> &apply) {
    auto it = officedocs_.find(CurPane().buffer_id);
    if (it == officedocs_.end()) return;
    OfficeSession &sess = it->second;
    int para_count = static_cast<int>(sess.doc.paragraphs.size());
    if (para_count <= 0) return;
    PushUndoOffice();
    if (mode_ == Mode::OfficeVisual && sess.has_selection) {
        int ap = sess.sel_anchor_para, ac = sess.sel_anchor_col;
        int cp = sess.cursor_para, cc = sess.cursor_col;
        int pa, ca, pb, cb;
        if (ap < cp || (ap == cp && ac <= cc)) {
            pa = ap; ca = ac; pb = cp; cb = cc;
        } else {
            pa = cp; ca = cc; pb = ap; cb = ac;
        }
        if (pa == pb) {
            SetFormatFieldOverRange(sess.doc.paragraphs[pa], ca, cb, apply);
        } else {
            SetFormatFieldOverRange(sess.doc.paragraphs[pa], ca, static_cast<int>(sess.doc.paragraphs[pa].text.size()),
                                     apply);
            for (int pi = pa + 1; pi < pb; pi++) {
                SetFormatFieldOverRange(sess.doc.paragraphs[pi], 0, static_cast<int>(sess.doc.paragraphs[pi].text.size()),
                                         apply);
            }
            SetFormatFieldOverRange(sess.doc.paragraphs[pb], 0, cb, apply);
        }
        sess.has_selection = false;
        sess.cursor_para = pa;
        sess.cursor_col = ca;
        mode_ = Mode::OfficeNormal;
    } else {
        int len = static_cast<int>(sess.doc.paragraphs[sess.cursor_para].text.size());
        int a = std::clamp(sess.cursor_col, 0, len);
        int b = std::min(a + 1, len);
        if (b > a) SetFormatFieldOverRange(sess.doc.paragraphs[sess.cursor_para], a, b, apply);
    }
    sess.modified = true;
}

void Editor::SetOfficeFontFamily(OfficeFontFamily family) {
    ApplyOfficeFormatFieldOverSelection([family](DocFormat &f) { f.font_family = family; });
}

void Editor::SetOfficeFontSizePt(float pt) {
    ApplyOfficeFormatFieldOverSelection([pt](DocFormat &f) { f.font_size_pt = pt; });
}

void Editor::SetOfficeColor(unsigned char r, unsigned char g, unsigned char b) {
    ApplyOfficeFormatFieldOverSelection([r, g, b](DocFormat &f) {
        f.has_color = true;
        f.color_r = r; f.color_g = g; f.color_b = b;
    });
}

void Editor::ClearOfficeColor() {
    ApplyOfficeFormatFieldOverSelection([](DocFormat &f) { f.has_color = false; });
}

void Editor::SetOfficeHighlight(unsigned char r, unsigned char g, unsigned char b) {
    ApplyOfficeFormatFieldOverSelection([r, g, b](DocFormat &f) {
        f.has_highlight = true;
        f.highlight_r = r; f.highlight_g = g; f.highlight_b = b;
    });
}

void Editor::ClearOfficeHighlight() {
    ApplyOfficeFormatFieldOverSelection([](DocFormat &f) { f.has_highlight = false; });
}

void Editor::ToggleOfficeSuperscript() {
    // Reads the current state once (single-char/first-selection-char
    // sample, same approximation OfficeFormatActive's own comment
    // documents) so the whole selection ends up in one consistent
    // resulting state rather than each character flipping independently.
    bool active = OfficeSuperscriptActiveInternal(true);
    ApplyOfficeFormatFieldOverSelection([active](DocFormat &f) {
        f.superscript = !active;
        f.subscript = false;
    });
}

void Editor::ToggleOfficeSubscript() {
    bool active = OfficeSuperscriptActiveInternal(false);
    ApplyOfficeFormatFieldOverSelection([active](DocFormat &f) {
        f.subscript = !active;
        f.superscript = false;
    });
}

bool Editor::OfficeSuperscriptActiveInternal(bool super) const {
    auto it = officedocs_.find(CurPane().buffer_id);
    if (it == officedocs_.end()) return false;
    const OfficeSession &sess = it->second;
    int para_count = static_cast<int>(sess.doc.paragraphs.size());
    if (para_count <= 0) return false;
    int cp, col;
    if (mode_ == Mode::OfficeVisual && sess.has_selection) {
        int ap = sess.sel_anchor_para, ac = sess.sel_anchor_col;
        int ccp = sess.cursor_para, ccc = sess.cursor_col;
        if (ap < ccp || (ap == ccp && ac <= ccc)) { cp = ap; col = ac; } else { cp = ccp; col = ccc; }
    } else {
        cp = sess.cursor_para;
        col = sess.cursor_col;
    }
    cp = std::clamp(cp, 0, para_count - 1);
    col = std::min(col, static_cast<int>(sess.doc.paragraphs[cp].text.size()));
    DocFormat f = FormatAt(sess.doc.paragraphs[cp], col);
    return super ? f.superscript : f.subscript;
}

void Editor::SetOfficeAlignment(DocParagraph::Align align) {
    auto it = officedocs_.find(CurPane().buffer_id);
    if (it == officedocs_.end()) return;
    OfficeSession &sess = it->second;
    if (sess.doc.paragraphs.empty()) return;
    PushUndoOffice();
    int first = sess.cursor_para, last = sess.cursor_para;
    if (mode_ == Mode::OfficeVisual && sess.has_selection) {
        first = std::min(sess.sel_anchor_para, sess.cursor_para);
        last = std::max(sess.sel_anchor_para, sess.cursor_para);
        sess.has_selection = false;
        mode_ = Mode::OfficeNormal;
    }
    SetParagraphAlignment(sess.doc.paragraphs, first, last, align);
    sess.modified = true;
}

bool Editor::OfficeAlignmentActive(DocParagraph::Align align) const {
    auto it = officedocs_.find(CurPane().buffer_id);
    if (it == officedocs_.end()) return false;
    const OfficeSession &sess = it->second;
    int cp = std::clamp(sess.cursor_para, 0, static_cast<int>(sess.doc.paragraphs.size()) - 1);
    return cp >= 0 && sess.doc.paragraphs[cp].align == align;
}

void Editor::SetOfficeListKind(DocParagraph::ListKind kind) {
    auto it = officedocs_.find(CurPane().buffer_id);
    if (it == officedocs_.end()) return;
    OfficeSession &sess = it->second;
    if (sess.doc.paragraphs.empty()) return;
    PushUndoOffice();
    int first = sess.cursor_para, last = sess.cursor_para;
    if (mode_ == Mode::OfficeVisual && sess.has_selection) {
        first = std::min(sess.sel_anchor_para, sess.cursor_para);
        last = std::max(sess.sel_anchor_para, sess.cursor_para);
        sess.has_selection = false;
        mode_ = Mode::OfficeNormal;
    }
    // Click/press-again-to-undo: if every paragraph in range already has
    // `kind`, clear back to None instead of re-applying it.
    bool all_already = true;
    for (int i = first; i <= last && all_already; i++) {
        if (sess.doc.paragraphs[static_cast<size_t>(i)].list_kind != kind) all_already = false;
    }
    SetParagraphListKind(sess.doc.paragraphs, first, last,
                          all_already ? DocParagraph::ListKind::None : kind);
    sess.modified = true;
}

bool Editor::OfficeListKindActive(DocParagraph::ListKind kind) const {
    auto it = officedocs_.find(CurPane().buffer_id);
    if (it == officedocs_.end()) return false;
    const OfficeSession &sess = it->second;
    int cp = std::clamp(sess.cursor_para, 0, static_cast<int>(sess.doc.paragraphs.size()) - 1);
    return cp >= 0 && sess.doc.paragraphs[cp].list_kind == kind;
}

void Editor::InsertOfficeText(const std::string &utf8) {
    auto it = officedocs_.find(CurPane().buffer_id);
    if (it == officedocs_.end()) return;
    OfficeSession &sess = it->second;
    if (sess.doc.paragraphs.empty() || utf8.empty()) return;
    PushUndoOffice();
    DocParagraph &p = sess.doc.paragraphs[sess.cursor_para];
    ApplyInsertToParagraph(p, sess.cursor_col, utf8);
    sess.cursor_col += static_cast<int>(utf8.size());
    sess.modified = true;
}

void Editor::InsertOfficeMath() {
    if (!IsOfficeBuffer(CurPane().buffer_id)) return;
    int buffer_id = CurPane().buffer_id;
    BeginPromptNative("Insert math (LaTeX):", "", [this, buffer_id](const std::string &latex) {
        if (latex.empty()) return;
        auto it = officedocs_.find(buffer_id);
        if (it == officedocs_.end()) return;
        OfficeSession &sess = it->second;
        if (sess.doc.paragraphs.empty()) return;
        PushUndoOffice();
        DocParagraph &p = sess.doc.paragraphs[sess.cursor_para];
        int start = sess.cursor_col;
        ApplyInsertToParagraph(p, start, latex);
        DocFormat fmt;
        fmt.math = true;
        // ApplyInsertToParagraph already shifted every span at/after
        // `start`; a fresh math span for exactly the inserted range is
        // added directly (not via SetFormatFieldOverRange, which expects
        // the range to already exist as plain/other-formatted text) then
        // coalesced same as any parser-built span list.
        p.spans.push_back({start, start + static_cast<int>(latex.size()), fmt});
        CoalesceSpans(p.spans);
        sess.cursor_col = start + static_cast<int>(latex.size());
        sess.modified = true;
    });
}

void Editor::InsertOfficeTablePrompt() {
    if (!IsOfficeBuffer(CurPane().buffer_id)) return;
    int buffer_id = CurPane().buffer_id;
    BeginPromptNative("Insert table (rows):", "2", [this, buffer_id](const std::string &rows_str) {
        int rows = std::clamp(std::atoi(rows_str.c_str()), 1, 50);
        BeginPromptNative("Insert table (cols):", "2", [this, buffer_id, rows](const std::string &cols_str) {
            int cols = std::clamp(std::atoi(cols_str.c_str()), 1, 20);
            auto it = officedocs_.find(buffer_id);
            if (it == officedocs_.end()) return;
            OfficeSession &sess = it->second;
            if (sess.doc.paragraphs.empty()) return;
            PushUndoOffice();
            DocTable table;
            table.rows = rows;
            table.cols = cols;
            table.cells.assign(static_cast<size_t>(rows) * static_cast<size_t>(cols), std::string());
            sess.doc.tables.push_back(std::move(table));
            int table_ref = static_cast<int>(sess.doc.tables.size()) - 1;
            sess.doc.paragraphs[sess.cursor_para].table_ref = table_ref;
            sess.modified = true;
            EnterOfficeTable(table_ref);
        });
    });
}

void Editor::InsertOfficeImagePrompt() {
    if (!IsOfficeBuffer(CurPane().buffer_id)) return;
    int buffer_id = CurPane().buffer_id;
    BeginPromptNative("Insert image (path):", "", [this, buffer_id](const std::string &path) {
        if (path.empty()) return;
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            Notify("Insert image: can't read \"" + path + "\"", NotifyLevel::Error);
            return;
        }
        std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        ImageDoc probe;
        if (!probe.LoadFromMemory(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size())) {
            Notify("Insert image: \"" + path + "\" isn't a decodable image", NotifyLevel::Error);
            return;
        }
        auto it = officedocs_.find(buffer_id);
        if (it == officedocs_.end()) return;
        OfficeSession &sess = it->second;
        if (sess.doc.paragraphs.empty()) return;
        PushUndoOffice();
        DocImage img;
        img.bytes = std::move(bytes);
        img.natural_w = probe.Width();
        img.natural_h = probe.Height();
        sess.doc.images.push_back(std::move(img));
        sess.doc.paragraphs[sess.cursor_para].image_ref = static_cast<int>(sess.doc.images.size()) - 1;
        sess.modified = true;
    });
}

void Editor::EnterOfficeTable(int table_ref) {
    auto it = officedocs_.find(CurPane().buffer_id);
    if (it == officedocs_.end()) return;
    OfficeSession &sess = it->second;
    if (table_ref < 0 || table_ref >= static_cast<int>(sess.doc.tables.size())) return;
    sess.in_table_edit = table_ref;
    sess.table_cursor_row = 0;
    sess.table_cursor_col = 0;
    sess.table_cell_editing = false;
}

void Editor::ExitOfficeTable() {
    auto it = officedocs_.find(CurPane().buffer_id);
    if (it == officedocs_.end()) return;
    it->second.in_table_edit = -1;
    it->second.table_cell_editing = false;
}

void Editor::MoveOfficeTableCell(int dr, int dc) {
    auto it = officedocs_.find(CurPane().buffer_id);
    if (it == officedocs_.end()) return;
    OfficeSession &sess = it->second;
    if (sess.in_table_edit < 0 || sess.in_table_edit >= static_cast<int>(sess.doc.tables.size())) return;
    const DocTable &t = sess.doc.tables[static_cast<size_t>(sess.in_table_edit)];
    sess.table_cursor_row = std::clamp(sess.table_cursor_row + dr, 0, t.rows - 1);
    sess.table_cursor_col = std::clamp(sess.table_cursor_col + dc, 0, t.cols - 1);
}

void Editor::SetOfficeZoom(float factor) {
    auto it = officedocs_.find(CurPane().buffer_id);
    if (it == officedocs_.end()) return;
    it->second.zoom = std::clamp(it->second.zoom * factor, 0.5f, 3.0f);
}

// --- Spreadsheet panes --------------------------------------------------

bool Editor::IsSheetBuffer(int buffer_id) const { return sheetdocs_.find(buffer_id) != sheetdocs_.end(); }

const SheetSession *Editor::GetSheet(int buffer_id) const {
    auto it = sheetdocs_.find(buffer_id);
    return it == sheetdocs_.end() ? nullptr : &it->second;
}

void Editor::ResizeSheetViewport(int buffer_id, int w, int h) {
    auto it = sheetdocs_.find(buffer_id);
    if (it == sheetdocs_.end()) return;
    SheetSession &sess = it->second;
    sess.viewport_w = w;
    sess.viewport_h = h;
    int visible_rows = std::max(1, h / kSheetRowHeight);
    int visible_cols = std::max(1, (w - kSheetRowHeaderW) / kSheetColWidth);
    if (sess.cursor_row < sess.scroll_row) sess.scroll_row = sess.cursor_row;
    if (sess.cursor_row >= sess.scroll_row + visible_rows) sess.scroll_row = sess.cursor_row - visible_rows + 1;
    if (sess.cursor_col < sess.scroll_col) sess.scroll_col = sess.cursor_col;
    if (sess.cursor_col >= sess.scroll_col + visible_cols) sess.scroll_col = sess.cursor_col - visible_cols + 1;
    sess.scroll_row = std::max(0, sess.scroll_row);
    sess.scroll_col = std::max(0, sess.scroll_col);
}

CellValue Editor::EvaluateSheetCell(int buffer_id, int row, int col) {
    auto it = sheetdocs_.find(buffer_id);
    if (it == sheetdocs_.end()) return CellValue{};
    SheetSession &sess = it->second;
    return EvaluateCell(sess.wb, sess.active_sheet, row, col);
}

void Editor::OpenSheetInPlace(const std::string &path, const unsigned char *bytes, size_t len) {
    int buffer_id = -1;
    for (size_t i = 0; i < buffers_.size(); i++) {
        if (!buffers_[i].filename.empty() && buffers_[i].filename == path) {
            buffer_id = static_cast<int>(i);
            break;
        }
    }
    if (buffer_id < 0) {
        Workbook wb;
        std::string error;
        bool ok = false;
        if (IsCsvPath(path)) {
            ok = LoadCsvFromMemory(bytes, len, wb, error);
        } else if (IsXlsxPath(path)) {
            ok = LoadXlsxFromMemory(bytes, len, wb, error);
        } else if (IsOdsPath(path)) {
            ok = LoadOdsFromMemory(bytes, len, wb, error);
        } else {
            error = "unsupported spreadsheet format";
        }
        if (!ok) {
            status_message_ = "E-\"" + path + "\": " + error;
            return;
        }
        buffer_id = CreateEmptyBuffer();
        buffers_[buffer_id].filename = path;
        SheetSession sess;
        sess.buffer_id = buffer_id;
        sess.wb = std::move(wb);
        sess.original_bytes.assign(bytes, bytes + len);
        sheetdocs_[buffer_id] = std::move(sess);
    }
    CurPane().buffer_id = buffer_id;
    CurPane().cursor = {0, 0};
    CurPane().scroll_row = 0;
    pending_g_ = false;  // avoid gg/G leakage from whatever mode preceded this
    status_message_.clear();
}

void Editor::HandleSheetNormalInput() {
    SheetSession *sess = nullptr;
    {
        auto it = sheetdocs_.find(CurPane().buffer_id);
        if (it == sheetdocs_.end()) {
            mode_ = Mode::Normal;
            return;
        }
        sess = &it->second;
    }
    if (sess->wb.sheets.empty()) return;
    Sheet &sh = sess->wb.sheets[sess->active_sheet];

    // 2D grid navigation -- h/l move columns, j/k move rows (unlike
    // Office's 1D paragraph navigation). No upper clamp beyond 0 --
    // hjkl can freely move past the sheet's current "used range" so the
    // user can navigate to an empty cell to start typing new data; gg/G/
    // 0/$ below jump specifically to the used-range corners.
    auto held = [](int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); };
    if (held(KEY_H) || held(KEY_LEFT)) sess->cursor_col = std::max(0, sess->cursor_col - 1);
    if (held(KEY_L) || held(KEY_RIGHT)) sess->cursor_col++;
    if (held(KEY_J) || held(KEY_DOWN)) sess->cursor_row++;
    if (held(KEY_K) || held(KEY_UP)) sess->cursor_row = std::max(0, sess->cursor_row - 1);

    int cp = GetCharPressed();
    while (cp > 0) {
        if (cp == ':') {
            EnterCommand();
            return;  // mode_ is no longer SheetNormal -- stop draining as this mode
        } else if (cp == static_cast<int>(leader_key_) && !whichkey_bindings_.empty()) {
            TriggerWhichKey();
            return;
        } else if (cp == 'g') {
            if (pending_g_) {
                pending_g_ = false;
                sess->cursor_row = 0;
            } else {
                pending_g_ = true;
            }
        } else if (cp == 'G') {
            pending_g_ = false;
            sess->cursor_row = sh.max_row;
        } else if (cp == '0') {
            pending_g_ = false;
            sess->cursor_col = 0;
        } else if (cp == '$') {
            pending_g_ = false;
            sess->cursor_col = sh.max_col;
        } else if (cp == 'i' || cp == 'a') {
            pending_g_ = false;
            PushUndoSheet();
            const Cell *cell = sh.FindCell(sess->cursor_row, sess->cursor_col);
            sess->edit_buffer = cell ? cell->raw : "";
            sess->edit_cursor = static_cast<int>(sess->edit_buffer.size());
            sess->editing = true;
            mode_ = Mode::SheetInsert;
            return;
        } else if (cp == 'v') {
            pending_g_ = false;
            sess->has_selection = true;
            sess->sel_anchor_row = sess->cursor_row;
            sess->sel_anchor_col = sess->cursor_col;
            mode_ = Mode::SheetVisual;
            return;
        } else if (cp == 'x' || cp == 'd') {
            // Both clear the current cell -- no dd/dw-style operator+
            // motion grammar in v1 (a single key clears one cell), same
            // simplification the Office pane made by skipping operator-
            // pending mode entirely.
            pending_g_ = false;
            PushUndoSheet();
            SetCellRaw(sess->wb, sess->active_sheet, sess->cursor_row, sess->cursor_col, "");
            sess->modified = true;
        } else if (cp == 'u') {
            pending_g_ = false;
            UndoSheet();
        } else {
            pending_g_ = false;
        }
        cp = GetCharPressed();
    }
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (IsKeyPressed(KEY_R) && ctrl) {
        RedoSheet();
    }
    // Excel's own next/prev-sheet convention -- matches real spreadsheet
    // muscle memory better than inventing a new mep-specific binding.
    if (IsKeyPressed(KEY_PAGE_DOWN) && ctrl) {
        NextSheet();
    }
    if (IsKeyPressed(KEY_PAGE_UP) && ctrl) {
        PrevSheet();
    }
    if (ctrl && IsKeyPressed(KEY_D)) {
        sess->cursor_row += std::max(1, sess->viewport_h / kSheetRowHeight / 2);
    }
    if (ctrl && IsKeyPressed(KEY_U)) {
        sess->cursor_row = std::max(0, sess->cursor_row - std::max(1, sess->viewport_h / kSheetRowHeight / 2));
    }
}

void Editor::PushUndoSheet() {
    auto it = sheetdocs_.find(CurPane().buffer_id);
    if (it == sheetdocs_.end()) return;
    SheetSession &sess = it->second;
    sess.undo_stack.push_back(sess.wb);
    if (sess.undo_stack.size() > kMaxUndo) sess.undo_stack.erase(sess.undo_stack.begin());
    sess.redo_stack.clear();
}

namespace {
// Shared by UndoSheet/RedoSheet: re-clamps the cursor into the swapped-in
// Workbook's used range (sheet count/active_sheet stay fixed across
// undo/redo in v1 -- no structural sheet edits -- so only row/col need
// clamping).
void ClampSheetCursorAfterSwap(SheetSession &sess) {
    if (sess.wb.sheets.empty()) return;
    sess.active_sheet = std::clamp(sess.active_sheet, 0, static_cast<int>(sess.wb.sheets.size()) - 1);
    Sheet &sh = sess.wb.sheets[sess.active_sheet];
    sess.cursor_row = std::clamp(sess.cursor_row, 0, sh.max_row);
    sess.cursor_col = std::clamp(sess.cursor_col, 0, sh.max_col);
}
}  // namespace

void Editor::UndoSheet() {
    auto it = sheetdocs_.find(CurPane().buffer_id);
    if (it == sheetdocs_.end()) return;
    SheetSession &sess = it->second;
    if (sess.undo_stack.empty()) {
        status_message_ = "Already at oldest change";
        return;
    }
    sess.redo_stack.push_back(sess.wb);
    sess.wb = sess.undo_stack.back();
    sess.undo_stack.pop_back();
    sess.modified = true;
    ClampSheetCursorAfterSwap(sess);
}

void Editor::RedoSheet() {
    auto it = sheetdocs_.find(CurPane().buffer_id);
    if (it == sheetdocs_.end()) return;
    SheetSession &sess = it->second;
    if (sess.redo_stack.empty()) {
        status_message_ = "Already at newest change";
        return;
    }
    sess.undo_stack.push_back(sess.wb);
    sess.wb = sess.redo_stack.back();
    sess.redo_stack.pop_back();
    sess.modified = true;
    ClampSheetCursorAfterSwap(sess);
}

void Editor::NextSheet() {
    auto it = sheetdocs_.find(CurPane().buffer_id);
    if (it == sheetdocs_.end()) return;
    SheetSession &sess = it->second;
    if (sess.wb.sheets.size() < 2) return;
    sess.active_sheet = (sess.active_sheet + 1) % static_cast<int>(sess.wb.sheets.size());
    ClampSheetCursorAfterSwap(sess);
    status_message_ = "-- " + sess.wb.sheets[sess.active_sheet].name + " --";
}

void Editor::PrevSheet() {
    auto it = sheetdocs_.find(CurPane().buffer_id);
    if (it == sheetdocs_.end()) return;
    SheetSession &sess = it->second;
    if (sess.wb.sheets.size() < 2) return;
    int n = static_cast<int>(sess.wb.sheets.size());
    sess.active_sheet = (sess.active_sheet - 1 + n) % n;
    ClampSheetCursorAfterSwap(sess);
    status_message_ = "-- " + sess.wb.sheets[sess.active_sheet].name + " --";
}

void Editor::HandleSheetInsertInput() {
    SheetSession *sess = nullptr;
    {
        auto it = sheetdocs_.find(CurPane().buffer_id);
        if (it == sheetdocs_.end()) {
            mode_ = Mode::Normal;
            return;
        }
        sess = &it->second;
    }

    bool escape = false, enter = false, backspace = false, del = false;
    for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed()) {
        if (key == KEY_ESCAPE) escape = true;
        else if (key == KEY_ENTER) enter = true;
        else if (key == KEY_BACKSPACE) backspace = true;
        else if (key == KEY_DELETE) del = true;
    }
    auto commit = [&]() {
        SetCellRaw(sess->wb, sess->active_sheet, sess->cursor_row, sess->cursor_col, sess->edit_buffer);
        sess->modified = true;
        sess->editing = false;
        mode_ = Mode::SheetNormal;
    };
    if (escape) {
        commit();
        return;
    }
    if (enter) {
        commit();
        sess->cursor_row++;  // real-spreadsheet "Enter commits and advances" convention
        return;
    }

    int cp = GetCharPressed();
    while (cp > 0) {
        if (cp >= 32 && cp <= 126) {
            sess->edit_buffer.insert(sess->edit_buffer.begin() + sess->edit_cursor, static_cast<char>(cp));
            sess->edit_cursor++;
        }
        cp = GetCharPressed();
    }
    if (backspace || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        if (sess->edit_cursor > 0) {
            sess->edit_buffer.erase(sess->edit_buffer.begin() + sess->edit_cursor - 1);
            sess->edit_cursor--;
        }
    }
    if (del || IsKeyPressedRepeat(KEY_DELETE)) {
        if (sess->edit_cursor < static_cast<int>(sess->edit_buffer.size())) {
            sess->edit_buffer.erase(sess->edit_buffer.begin() + sess->edit_cursor);
        }
    }
    if (IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT)) sess->edit_cursor = std::max(0, sess->edit_cursor - 1);
    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT)) {
        sess->edit_cursor = std::min(static_cast<int>(sess->edit_buffer.size()), sess->edit_cursor + 1);
    }
}

void Editor::HandleSheetVisualInput() {
    SheetSession *sess = nullptr;
    {
        auto it = sheetdocs_.find(CurPane().buffer_id);
        if (it == sheetdocs_.end()) {
            mode_ = Mode::Normal;
            return;
        }
        sess = &it->second;
    }
    if (sess->wb.sheets.empty()) {
        mode_ = Mode::SheetNormal;
        return;
    }
    Sheet &sh = sess->wb.sheets[sess->active_sheet];

    auto held = [](int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); };
    if (held(KEY_H) || held(KEY_LEFT)) sess->cursor_col = std::max(0, sess->cursor_col - 1);
    if (held(KEY_L) || held(KEY_RIGHT)) sess->cursor_col++;
    if (held(KEY_J) || held(KEY_DOWN)) sess->cursor_row++;
    if (held(KEY_K) || held(KEY_UP)) sess->cursor_row = std::max(0, sess->cursor_row - 1);

    int cp = GetCharPressed();
    while (cp > 0) {
        if (cp == 'g') {
            if (pending_g_) {
                pending_g_ = false;
                sess->cursor_row = 0;
            } else {
                pending_g_ = true;
            }
        } else if (cp == 'G') {
            pending_g_ = false;
            sess->cursor_row = sh.max_row;
        } else if (cp == '0') {
            pending_g_ = false;
            sess->cursor_col = 0;
        } else if (cp == '$') {
            pending_g_ = false;
            sess->cursor_col = sh.max_col;
        } else if (cp == 'x' || cp == 'd') {
            pending_g_ = false;
            PushUndoSheet();
            int r0 = std::min(sess->sel_anchor_row, sess->cursor_row);
            int r1 = std::max(sess->sel_anchor_row, sess->cursor_row);
            int c0 = std::min(sess->sel_anchor_col, sess->cursor_col);
            int c1 = std::max(sess->sel_anchor_col, sess->cursor_col);
            for (int r = r0; r <= r1; r++) {
                for (int c = c0; c <= c1; c++) SetCellRaw(sess->wb, sess->active_sheet, r, c, "");
            }
            sess->modified = true;
            sess->has_selection = false;
            sess->cursor_row = r0;
            sess->cursor_col = c0;
            mode_ = Mode::SheetNormal;
            return;
        } else {
            pending_g_ = false;
        }
        cp = GetCharPressed();
    }
    for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed()) {
        if (key == KEY_ESCAPE) {
            sess->has_selection = false;
            mode_ = Mode::SheetNormal;
            return;
        }
    }
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
                EnterTerminalNormalMode(*sess);
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

// A *snapshot*, not a live view: VTerm's scrollback+grid is copied into
// CurPane()'s buffer once, here, rather than re-synced every frame while
// browsing. A live view would need to keep the user's cursor position
// meaningful across re-syncs as new output arrives and old scrollback
// rotates out from under it (VTerm caps scrollback at 5000 lines --
// vterm.h's kMaxScrollback) -- real surgery for comparatively little
// payoff here, and out of step with this codebase's explicit "does not
// attempt full Vim parity" scope (editor.h's own class comment). Ctrl-\
// Ctrl-N again always re-snapshots, so "stale" is at most one chord away
// from "current".
//
// This is also what makes every other Normal-mode facility (hjkl/word
// motions, gg/G, Ctrl-W pane commands, Visual-mode selection + yank,
// search, marks, macros, `:` commands) work here for free: they all
// operate on Buffer::lines already, and after this call, CurPane()'s
// buffer genuinely contains the terminal's text as ordinary lines. See
// DrawPane (main.cpp) for the other half -- it renders this buffer
// normally instead of the live VTerm grid exactly while this state
// applies, and switches back to the live grid the instant it doesn't
// (mode_ != Mode::Normal, or focus moves to a different pane).
void Editor::EnterTerminalNormalMode(TerminalSession &sess) {
    Buffer &buf = Buf();
    buf.lines.clear();
    buf.undo_stack.clear();
    buf.redo_stack.clear();
    buf.marks.clear();
    buf.modified = false;

    const VTerm *term = sess.vterm.get();
    int cursor_line = 0, cursor_col = 0;
    if (term) {
        int sb_lines = term->ScrollbackLines();
        int rows = term->Rows(), cols = term->Cols();
        for (int i = 0; i < sb_lines + rows; i++) {
            std::string line;
            line.reserve(static_cast<size_t>(cols));
            for (int c = 0; c < cols; c++) {
                const VTermCell &cell = i < sb_lines ? term->ScrollbackAt(i, c) : term->At(i - sb_lines, c);
                line += cell.ch.empty() ? " " : cell.ch;
            }
            // Trailing blanks are real (unused) cells, not meaningful
            // content -- trimmed for a cleaner view/yank, same as any
            // other program's "don't pad every line to the terminal
            // width" convention. Safe as a byte-level trim: an ASCII
            // space (0x20) never appears as a continuation/lead byte of
            // a multi-byte UTF-8 cell.
            size_t end = line.find_last_not_of(' ');
            buf.lines.push_back(end == std::string::npos ? std::string() : line.substr(0, end + 1));
        }
        cursor_line = std::clamp(sb_lines + term->CursorRow(), 0, std::max(0, static_cast<int>(buf.lines.size()) - 1));
        // Trailing all-blank lines below the live cursor are just unused
        // screen space (e.g. a shell prompt with most of the window still
        // empty) -- trimmed the same way, but never past the cursor's own
        // line, which must survive even when it's itself blank (a fresh
        // prompt with nothing typed yet).
        size_t min_lines = static_cast<size_t>(cursor_line + 1);
        while (buf.lines.size() > min_lines && buf.lines.size() > 1 && buf.lines.back().empty()) {
            buf.lines.pop_back();
        }
        cursor_col = std::clamp(term->CursorCol(), 0, static_cast<int>(buf.lines[cursor_line].size()));
    }
    if (buf.lines.empty()) buf.lines.push_back("");

    CursorPos &cur = CurPane().cursor;
    cur.row = cursor_line;
    cur.col = cursor_col;
    mode_ = Mode::Normal;
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
    // A terminal's buffer/PTY session outlives the pane that showed it,
    // same as any other buffer type -- switchable back into view later via
    // mep.buffers() (l_buffer_switch), not killed just because its one
    // pane closed.
    SyncModeToActivePaneBuffer();
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
    SyncModeToActivePaneBuffer();
}

void Editor::PaneNextBufferTab() {
    Pane &p = CurPane();
    EnsureBufferTabSeeded(p);
    if (p.buffer_tabs.size() <= 1) return;
    p.buffer_tab_index = (p.buffer_tab_index + 1) % static_cast<int>(p.buffer_tabs.size());
    p.buffer_id = p.buffer_tabs[p.buffer_tab_index];
    ClampCursor();
    SyncModeToActivePaneBuffer();
}

void Editor::PanePrevBufferTab() {
    Pane &p = CurPane();
    EnsureBufferTabSeeded(p);
    if (p.buffer_tabs.size() <= 1) return;
    int n = static_cast<int>(p.buffer_tabs.size());
    p.buffer_tab_index = (p.buffer_tab_index - 1 + n) % n;
    p.buffer_id = p.buffer_tabs[p.buffer_tab_index];
    ClampCursor();
    SyncModeToActivePaneBuffer();
}

void Editor::GoToPaneBufferTab(int index) {
    Pane &p = CurPane();
    EnsureBufferTabSeeded(p);
    if (p.buffer_tabs.empty()) return;
    p.buffer_tab_index = std::max(0, std::min(index, static_cast<int>(p.buffer_tabs.size()) - 1));
    p.buffer_id = p.buffer_tabs[p.buffer_tab_index];
    ClampCursor();
    SyncModeToActivePaneBuffer();
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
    SyncModeToActivePaneBuffer();
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
    SyncModeToActivePaneBuffer();
}

void Editor::TabDelete() {
    if (tabs_.size() <= 1) {
        status_message_ = "E784: Cannot close last tab page";
        return;
    }
    tabs_.erase(tabs_.begin() + active_tab_);
    if (active_tab_ >= static_cast<int>(tabs_.size())) active_tab_ = static_cast<int>(tabs_.size()) - 1;
    SyncModeToActivePaneBuffer();
}

void Editor::TabNext() {
    if (tabs_.size() <= 1) return;
    active_tab_ = (active_tab_ + 1) % static_cast<int>(tabs_.size());
    SyncModeToActivePaneBuffer();
}

void Editor::TabPrevious() {
    if (tabs_.size() <= 1) return;
    int n = static_cast<int>(tabs_.size());
    active_tab_ = (active_tab_ - 1 + n) % n;
    SyncModeToActivePaneBuffer();
}

// Click-to-switch (NVIM_PARITY_PLAN.md Phase 11 gap): jumps directly to a
// tab by index, unlike TabNext/TabPrevious's relative stepping -- what a
// mouse click on a specific tab box in the tab bar needs. Out-of-range
// indices are silently clamped rather than ignored, matching this file's
// existing tolerant style (e.g. TabDelete's active_tab_ clamp above) since
// the click hit-testing that calls this only ever passes a valid index
// anyway.
void Editor::GoToTab(int index) {
    if (tabs_.empty()) return;
    active_tab_ = std::max(0, std::min(index, static_cast<int>(tabs_.size()) - 1));
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
        if (into_panes) {
            RestoreFromOverlay();
            return;
        }
        // Smart resize (mirrors ResizeActivePane's own Mode::Sidebar
        // branch): pressing further *outward* -- away from the pane tree,
        // into the screen edge the sidebar is already docked against --
        // has nowhere left to navigate, so shrink the sidebar instead of
        // just doing nothing. mod1+h against an already-focused, left-
        // docked file tree (with no pane further left to step back into
        // either) is the common case; the other three edges follow the
        // same rule.
        bool outward = (sb->position == "left" && direction == "left") ||
                       (sb->position == "right" && direction == "right") ||
                       (sb->position == "top" && direction == "up") ||
                       (sb->position == "bottom" && direction == "down");
        if (outward) ResizeActivePane(direction);
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
        SyncModeToActivePaneBuffer();
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

void Editor::FocusPaneById(int pane_id) {
    Tab &tab = tabs_[active_tab_];
    if (!FindNode(tab.root.get(), pane_id)) return;
    tab.active_pane_id = pane_id;
    // Mirrors NavigatePaneDirection's own same guard: focus genuinely
    // moving to a different pane must drop Terminal mode's keystroke-
    // forwarding, or stray keys typed afterward would leak into a live
    // shell/program the user no longer meant to be typing into.
    if (mode_ == Mode::Terminal) mode_ = Mode::Normal;
    // Clicking into a pane while a sidebar has focus is the mouse
    // equivalent of NavigatePaneDirection's into_panes branch -- leave
    // the overlay the same way (RestoreFromOverlay), not just Sidebar
    // mode's own bookkeeping, since overlay_previous_mode_ may itself be
    // something other than Normal (e.g. Insert, resumed below via
    // SyncModeToActivePaneBuffer as usual).
    if (mode_ == Mode::Sidebar) RestoreFromOverlay();
    SyncModeToActivePaneBuffer();
}

// Removes buffer_id from source_pane_id's own buffer_tabs (erasing at the
// tab's own index -- a drag can start from any chip, not just the pane's
// currently-active one -- and fixing up buffer_tab_index/buffer_id the
// same way PaneMoveBufferTabToNeighbor does), then ClosePane()s the
// source pane if that was its last tab. Shared by MoveBufferTabToPane and
// SplitPaneWithBufferTab, both of which need this exact removal+fixup
// before touching the destination side. Returns false (nothing removed,
// nothing to do) if buffer_id isn't actually one of source's tabs.
bool Editor::RemoveBufferTabFromPane(int source_pane_id, int buffer_id) {
    Tab &tab = tabs_[active_tab_];
    SplitNode *src_node = FindNode(tab.root.get(), source_pane_id);
    if (!src_node) return false;
    Pane &src = src_node->pane;
    EnsureBufferTabSeeded(src);
    auto it = std::find(src.buffer_tabs.begin(), src.buffer_tabs.end(), buffer_id);
    if (it == src.buffer_tabs.end()) return false;
    int removed_index = static_cast<int>(it - src.buffer_tabs.begin());
    bool was_active_tab = (removed_index == src.buffer_tab_index);
    bool was_last_tab = src.buffer_tabs.size() <= 1;
    src.buffer_tabs.erase(it);
    if (!src.buffer_tabs.empty()) {
        if (was_active_tab) {
            if (src.buffer_tab_index >= static_cast<int>(src.buffer_tabs.size())) {
                src.buffer_tab_index = static_cast<int>(src.buffer_tabs.size()) - 1;
            }
            src.buffer_id = src.buffer_tabs[src.buffer_tab_index];
            ClampCursor();
        } else if (removed_index < src.buffer_tab_index) {
            src.buffer_tab_index--;  // shift left to keep pointing at the same still-active tab
        }
    }
    if (was_last_tab) {
        tab.active_pane_id = source_pane_id;
        ClosePane();  // restructures the tree -- any raw SplitNode* obtained before this call is no longer trustworthy
    }
    return true;
}

void Editor::MoveBufferTabToPane(int source_pane_id, int buffer_id, int dest_pane_id) {
    if (source_pane_id == dest_pane_id) return;
    if (!RemoveBufferTabFromPane(source_pane_id, buffer_id)) return;

    Tab &tab = tabs_[active_tab_];
    // Re-found *after* RemoveBufferTabFromPane, never reused from before
    // it -- ClosePane() may have restructured the tree.
    SplitNode *dst_node = FindNode(tab.root.get(), dest_pane_id);
    if (!dst_node) return;
    Pane &dst = dst_node->pane;
    EnsureBufferTabSeeded(dst);
    dst.buffer_tabs.insert(dst.buffer_tabs.begin() + dst.buffer_tab_index + 1, buffer_id);
    dst.buffer_tab_index++;
    dst.buffer_id = buffer_id;
    tab.active_pane_id = dest_pane_id;
    ClampCursor();
    SyncModeToActivePaneBuffer();
}

void Editor::SplitPaneWithBufferTab(int source_pane_id, int buffer_id, int dest_pane_id, SplitDir dir, bool before) {
    // Unlike MoveBufferTabToPane, source_pane_id == dest_pane_id is a real
    // case here, not a no-op to guard against: dragging a chip out to an
    // edge zone of its own pane (dropping it back in the pane's center is
    // still the merge-into-self no-op MoveBufferTabToPane skips) means
    // "split this pane, keep my other tabs on one side, this tab on the
    // other" -- RemoveBufferTabFromPane below leaves the remaining tabs
    // behind in dst_node->pane (re-found afterward, same as the cross-pane
    // case), which is exactly the "existing_pane" this function already
    // moves into its own leaf below.
    if (!RemoveBufferTabFromPane(source_pane_id, buffer_id)) return;

    Tab &tab = tabs_[active_tab_];
    // Re-found *after* RemoveBufferTabFromPane, same reasoning as
    // MoveBufferTabToPane above.
    SplitNode *dst_node = FindNode(tab.root.get(), dest_pane_id);
    if (!dst_node) return;

    Pane new_pane;
    new_pane.id = next_pane_id_++;
    new_pane.buffer_id = buffer_id;
    new_pane.buffer_tabs = {buffer_id};
    new_pane.buffer_tab_index = 0;

    Pane existing_pane = dst_node->pane;  // keeps its own id/buffer_tabs/cursor/scroll/jumplist

    auto new_leaf = std::make_unique<SplitNode>();
    new_leaf->dir = SplitDir::Leaf;
    new_leaf->pane = std::move(new_pane);

    auto existing_leaf = std::make_unique<SplitNode>();
    existing_leaf->dir = SplitDir::Leaf;
    existing_leaf->pane = std::move(existing_pane);

    int new_pane_id = new_leaf->pane.id;
    dst_node->dir = dir;
    dst_node->pane = Pane{};
    dst_node->children.clear();
    dst_node->shares.clear();  // fresh split -- equal shares, same as SplitCurrentPane leaves it
    if (before) {
        dst_node->children.push_back(std::move(new_leaf));
        dst_node->children.push_back(std::move(existing_leaf));
    } else {
        dst_node->children.push_back(std::move(existing_leaf));
        dst_node->children.push_back(std::move(new_leaf));
    }

    tab.active_pane_id = new_pane_id;
    ClampCursor();
    SyncModeToActivePaneBuffer();
}

void Editor::SetPaneBorderShare(SplitNode *node, int child_index, float new_share) {
    if (!node || child_index < 0 || child_index + 1 >= static_cast<int>(node->children.size())) return;
    EnsureShares(node);
    float pair_total = node->shares[child_index] + node->shares[child_index + 1];
    new_share = std::clamp(new_share, kMinPaneShare, pair_total - kMinPaneShare);
    node->shares[child_index] = new_share;
    node->shares[child_index + 1] = pair_total - new_share;
}

float Editor::PaneBorderPairTotal(SplitNode *node, int child_index) {
    if (!node || child_index < 0 || child_index + 1 >= static_cast<int>(node->children.size())) return 0.0f;
    EnsureShares(node);
    return node->shares[child_index] + node->shares[child_index + 1];
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
    // mod1+j/k is globally bound to pane-nav (down/up, kDefaultMod1Bindings
    // in main.cpp), but while a picker with an active preview column is
    // open, that pane-nav mapping would just blur focus behind the picker
    // overlay to no visible effect -- so intercept it here and scroll the
    // preview instead, same reasoning as the Sidebar+D case above.
    if (mode_ == Mode::Picker && !extra_ctrl && !extra_shift && !PickerPreview().empty() &&
        (IsKeyPressed(KEY_J) || IsKeyPressed(KEY_K))) {
        ScrollPickerPreview(IsKeyPressed(KEY_J) ? 1 : -1);
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

void Editor::ScrollPickerPreview(int delta) {
    int line_count = 1 + static_cast<int>(std::count(picker_preview_text_.begin(), picker_preview_text_.end(), '\n'));
    int max_scroll = std::max(0, line_count - 1);
    picker_preview_scroll_ = std::max(0, std::min(picker_preview_scroll_ + delta, max_scroll));
}

// Ctrl-T (new tab) / Alt-1..Alt-9 (jump to tab by number) -- fixed global
// bindings, not user-remappable mod1 mappings (mod1 itself defaults to
// Alt, so routing these through that system would make mod1=alt users'
// Alt-1..9 ambiguous with whatever else mod1 might bind there). Checked
// unconditionally alongside HandleMod1Shortcuts, before mode dispatch, so
// e.g. Ctrl-T opens a new tab from Insert mode the same way it would in
// Normal.
bool Editor::HandleTabShortcuts() {
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    bool alt = IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
    if (ctrl && IsKeyPressed(KEY_T)) {
        TabNew("");
        return true;
    }
    if (alt) {
        for (int key = KEY_ONE; key <= KEY_NINE; key++) {
            if (!IsKeyPressed(key)) continue;
            GoToTab(key - KEY_ONE);
            // Same reasoning as HandleMod1Shortcuts' own drain: Alt-as-
            // compose on some layouts can still queue a char event for the
            // digit alongside the key event handled above.
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
         ctrl_x = false, ctrl_o = false, ctrl_i = false, ctrl_c = false, ctrl_e = false;
    for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed()) {
        if (!ctrl) continue;
        if (key == KEY_V) ctrl_v = true;
        else if (key == KEY_D) ctrl_d = true;
        else if (key == KEY_U) ctrl_u = true;
        else if (key == KEY_F) ctrl_f = true;
        else if (key == KEY_B) ctrl_b = true;
        else if (key == KEY_A) ctrl_a = true;
        else if (key == KEY_X) ctrl_x = true;
        else if (key == KEY_O) ctrl_o = true;
        else if (key == KEY_I) ctrl_i = true;
        else if (key == KEY_C) ctrl_c = true;
        else if (key == KEY_E) ctrl_e = true;
    }
    if (ctrl_v) {
        // On an .html/.htm buffer, Ctrl-V is repurposed as the escape
        // hatch back to the rendered browser view instead of Vim's usual
        // Visual Block -- the opposite direction of HandleHtmlInput's own
        // Ctrl-E (see ConvertTextBufferToHtml's own comment). Every other
        // buffer keeps plain Visual Block, unaffected.
        if (IsHtmlPath(Buf().filename)) {
            ConvertTextBufferToHtml(CurPane().buffer_id);
            CurPane().cursor = {0, 0};
            CurPane().scroll_row = 0;
            status_message_.clear();
            SyncModeToActivePaneBuffer();
            return;
        }
        // Routed through ProcessNormalKey (via the kReplayCtrlV sentinel --
        // see its own comment in editor.h) rather than calling
        // EnterVisualBlock() directly, so Ctrl-V participates in `.`/macro
        // recording exactly like 'v'/'V' already do (those go through
        // HandleNormalChar -> ProcessNormalKey normally, since they're plain
        // printable chars DispatchNormalKey's switch handles).
        ProcessNormalKey(kReplayCtrlV);
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
    // Normal-mode Ctrl-O/Ctrl-I: jumplist back/forward -- NOT the same
    // Ctrl-O as Insert mode's one-shot-normal-command (see HandleInsertInput);
    // Vim itself disambiguates the two purely by which mode you're in.
    if (ctrl_o) {
        JumpListBack();
        return;
    }
    if (ctrl_i) {
        JumpListForward();
        return;
    }
    if (ctrl_c) {
        if (pending_ctrl_c_ && (now_ - pending_ctrl_c_time_) < kCtrlCChordTimeoutSec) {
            pending_ctrl_c_ = false;
            TryRunOrgBabelAtCursor();
        } else {
            pending_ctrl_c_ = true;
            pending_ctrl_c_time_ = now_;
        }
        return;
    }
    // Ctrl-E completing a Ctrl-C Ctrl-E chord -- org export-format
    // dispatch (mirrors real Emacs org-mode's own C-c C-e). A bare
    // Ctrl-E with no preceding Ctrl-C (or after the chord timeout) is
    // unbound, same "silently absorbed" treatment an unrecognized second
    // key gets everywhere else in this function.
    if (ctrl_e) {
        if (pending_ctrl_c_ && (now_ - pending_ctrl_c_time_) < kCtrlCChordTimeoutSec) {
            pending_ctrl_c_ = false;
            pending_org_export_ = true;
        }
        return;
    }
    if (IsKeyPressed(KEY_ESCAPE)) {
        // A bare Escape while recording only matters as a "cancel whatever
        // was pending" if something actually was -- an Escape with nothing
        // pending is Vim's harmless no-op, not worth a macro-replay entry.
        if (record_raw && IsMidNormalCommand()) macro_recording_buffer_.push_back(kReplayEscape);
        CancelPendingNormalState();
        // A bare Escape doesn't go through ProcessNormalKey (see above),
        // so it wouldn't otherwise hit the "one-shot command finished"
        // check there -- a no-op Escape still counts as finishing
        // Insert-mode Ctrl-O's one-shot command, matching Vim.
        if (insert_one_shot_normal_) {
            insert_one_shot_normal_ = false;
            EnterInsert();
            replace_mode_ = insert_one_shot_was_replace_;
            insert_one_shot_was_replace_ = false;
        }
        return;
    }

    // Bare h/j/k/l: confirmed sustained holds move via a per-frame
    // IsKeyDown() fast path instead of replaying every individual queued
    // repeat notification in the GetCharPressed() drain loop below.
    // Native doesn't need this -- a healthy frame keeps up with even a
    // fast OS repeat rate, so the queue never really backs up -- but
    // confirmed on the wasm/webview build specifically: holding a motion
    // key and releasing it still produces a steady trickle of repeat
    // notifications for several hundred ms afterward (something in
    // WebKitGTK's own input pipeline apparently coalesces/delays
    // repeat-keydown delivery; native has no such layer in between and
    // doesn't show it).
    //
    // Two guards keep this from ever dropping a real keystroke, which an
    // earlier version of this fix got wrong (confirmed empirically: three
    // separate, deliberate taps could land only one column of movement,
    // because IsKeyDown() can miss a press/release pair that both happen
    // to fall within one polling window -- unlike GetCharPressed(), whose
    // queue is filled straight from the press event and can't miss a tap
    // that way):
    //   1. kMotionHoldConfirmSec -- a key only starts being treated as
    //      "held" (fast path takes over, queue discards its repeats)
    //      once IsKeyDown() has read continuously true for this long. A
    //      human tap, even a fast one, doesn't remotely approach this;
    //      only a genuine sustained hold does. Below this threshold nothing
    //      here changes anything -- the original, fully-reliable
    //      queue-driven path handles it exactly as before this fix existed.
    //   2. kMotionDiscardCooldownSec -- once a *confirmed* hold ends, the
    //      queue keeps discarding that key's repeats for this long
    //      afterward too (comfortably above the worst observed trickle,
    //      measured empirically at a few hundred ms), since those are
    //      presumed to be the delayed tail rather than a new keystroke.
    //      The cost is a same-key re-tap inside this window being
    //      swallowed too -- a minor, self-correcting annoyance (retrying
    //      once the window passes fixes it), traded against not replaying
    //      a stale backlog as visible extra motion.
    constexpr double kMotionHoldConfirmSec = 0.2;
    constexpr double kMotionDiscardCooldownSec = 0.7;
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    bool count_pending_now = pending_count_ != 0;
    bool no_pending_state_now = pending_op_ == 0 && !pending_g_ && !pending_ctrl_w_ && !pending_org_export_ && pending_find_ == 0 &&
                                 !count_pending_now && !awaiting_register_name_;
    double now = GetTime();
    static const std::pair<int, char> kMotionKeys[] = {
        {KEY_H, 'h'},
        {KEY_J, 'j'},
        {KEY_K, 'k'},
        {KEY_L, 'l'},
    };
    for (int i = 0; i < 4; i++) {
        MotionRepeatState &st = motion_repeat_[i];
        bool down = IsKeyDown(kMotionKeys[i].first);
        if (down) {
            if (st.down_since < 0.0) st.down_since = now;
            bool confirmed = (now - st.down_since) >= kMotionHoldConfirmSec;
            if (confirmed) {
                st.discard_until = now + kMotionDiscardCooldownSec;
                if (no_pending_state_now && !ctrl && !shift) {
                    HandleNormalChar(static_cast<int>(kMotionKeys[i].second), no_pending_state_now);
                    if (mode_ != Mode::Normal) return;  // key switched modes
                }
            }
        } else if (st.down_since >= 0.0) {
            if ((now - st.down_since) >= kMotionHoldConfirmSec) st.discard_until = now + kMotionDiscardCooldownSec;
            st.down_since = -1.0;
        }
    }

    int cp = GetCharPressed();
    while (cp > 0) {
        // Digits (as a pending count) and a pending find/g-prefix always
        // win over a Lua mapping for the same key -- those states consume
        // the very next key literally, and counts are closer to syntax
        // than to a remappable command.
        bool is_count_digit = (cp >= '1' && cp <= '9') || (cp == '0' && pending_count_ != 0);
        bool no_pending_state = pending_op_ == 0 && !pending_g_ && !pending_ctrl_w_ && !pending_org_export_ && pending_find_ == 0 &&
                                 !is_count_digit && !awaiting_register_name_;
        // A confirmed-held bare h/j/k/l is handled by the fast path above
        // (or is within its post-release discard window) -- drop the
        // queued notification here instead of replaying/double-counting
        // it. Anything below the hold-confirm threshold (ordinary taps),
        // shifted (H/L), or counted/operator-pending never enters this
        // window at all and falls through to the normal handling below,
        // completely unaffected.
        if (no_pending_state && !ctrl && !shift) {
            int idx = cp == 'h' ? 0 : cp == 'j' ? 1 : cp == 'k' ? 2 : cp == 'l' ? 3 : -1;
            if (idx >= 0 && now < motion_repeat_[idx].discard_until) {
                cp = GetCharPressed();
                continue;
            }
        }
        HandleNormalChar(cp, no_pending_state);
        if (mode_ != Mode::Normal) break;  // key switched modes mid-loop
        cp = GetCharPressed();
    }
}

void Editor::HandleNormalChar(int cp, bool no_pending_state) {
    bool consumed = false;
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

Register &Editor::RegisterFor(char name) {
    if (name == '%') {
        // "%: current filename, read-only. Refreshed on every read rather
        // than kept in sync at rename/write time, so it's always current;
        // writes into it (e.g. "%yy) are guarded out at the call sites
        // that actually mutate a register (YankRange, ApplyVisualBlockOperator)
        // and redirected to the unnamed register instead, matching Vim's
        // silent no-op there.
        Register &r = registers_['%'];
        r.text = Buf().filename;
        r.linewise = false;
        r.blockwise = false;
        return r;
    }
    return registers_[name != 0 ? name : '"'];
}

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
    // Defense in depth: Mode::Image (HandleImageInput) is what normally
    // keeps Normal-mode dispatch from ever running against an image
    // buffer, but if mode_ somehow ended up Normal here anyway (a missed
    // SyncModeToActivePaneBuffer() call site), there's still nothing for
    // insert-entry to do -- an image buffer's Buffer::lines is a dummy
    // empty line, and typing into it risks SaveBuffer's own guard being
    // the only thing standing between that and corrupting the real file.
    if ((c == 'i' || c == 'a') && IsImageBuffer(CurPane().buffer_id)) {
        return true;
    }

    if (pending_org_export_) {
        pending_org_export_ = false;
        TryRunOrgExport(c);
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
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '%') {
                // "0-"9 (numbered/last-yank), "- (small-delete), "%
                // (filename, read-only) -- all single-char names taken
                // literally, same as "a-"z. Writes to "% silently no-op
                // (see YankRange/ApplyVisualBlockOperator); "0-"9/"- are
                // written automatically by ApplyOperator rather than by
                // naming them explicitly before an operator, but reading
                // them via "1p etc. works the same as any other register.
                pending_register_ = c;
                pending_register_append_ = false;
            } else if (c >= 'A' && c <= 'Z') {
                pending_register_ = static_cast<char>(c - 'A' + 'a');
                pending_register_append_ = true;
            }
            // Anything else (punctuation not covered above): not a
            // register name mep supports, silently ignored.
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
    // z{m,r,R,M}: vim-style fold-level stepping/extremes (org headlines).
    if (pending_z_) {
        pending_z_ = false;
        // Org buffers have no persistent fold provider (no watcher re-runs
        // this on every edit) -- recomputing right before any fold command
        // touches the buffer means it's always current without paying a
        // rescan on every keystroke that isn't actually about to use it.
        if (c == 'a' || c == 'o' || c == 'c' || c == 'm' || c == 'r' || c == 'R' || c == 'M') {
            if (IsOrgBuffer()) RecomputeOrgFolds();
        }
        if (c == 'z' || c == 't' || c == 'b') {
            ScrollCursorTo(c);
        } else if (c == 'a') {
            ToggleFoldAtCursor();
        } else if (c == 'o' || c == 'c') {
            int row = CurPane().cursor.row;
            for (Fold &f : Buf().folds) {
                if (row >= f.start_row && row <= f.end_row) f.closed = (c == 'c');
            }
        } else if (c == 'M') {
            SetAllFoldsClosed(true);
        } else if (c == 'R') {
            SetAllFoldsClosed(false);
        } else if (c == 'm') {
            AdjustFoldLevel(-1);
        } else if (c == 'r') {
            AdjustFoldLevel(1);
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
        // Search as an operator motion (VIM_PARITY_PLAN.md Phase 4:
        // `d/foo<Enter>` deletes from the operator's start up to -- but not
        // including -- the next match of "foo"; `y?bar<Enter>` yanks
        // backward to the previous match of "bar", same exclusive-charwise
        // shape as every other operator+motion here). Unlike f/F/t/T and
        // `/'` above, the target isn't resolved on the very next keypress:
        // TakeRawCount() (a count typed between the operator and '/'/'?'
        // has no meaning for a search motion in Vim either, so it's just
        // discarded rather than leaking into the next command) and enter
        // search mode, leaving pending_op_/pending_op_start_ untouched so
        // HandleSearchInput's Enter handler -- run once the pattern is
        // typed and confirmed -- can read them back and apply the operator
        // over the found range instead of just moving the cursor.
        if (c == '/' || c == '?') {
            TakeRawCount();
            EnterSearch(c == '/');
            return true;
        }
        // `dn`/`dN` etc: reuse the last confirmed search pattern as the
        // motion target, synchronously (no prompt needed) -- same
        // exclusive-charwise shape as `d/foo<Enter>` above, just skipping
        // straight to FindNext with whatever direction n/N resolve to.
        if ((c == 'n' || c == 'N') && !last_search_.empty()) {
            pending_op_ = 0;
            pending_op_count_ = 0;
            TakeRawCount();
            bool forward = (c == 'n') ? last_search_forward_ : !last_search_forward_;
            CursorPos saved_cursor = CurPane().cursor;
            CurPane().cursor = start;
            CursorPos result;
            bool wrapped = false;
            if (FindNext(last_search_, forward, &result, &wrapped)) {
                ApplyOperator(op, start, result, false);
                status_message_ = wrapped ? (forward ? "search hit BOTTOM, continuing at TOP"
                                                       : "search hit TOP, continuing at BOTTOM")
                                           : "";
            } else {
                CurPane().cursor = saved_cursor;
                ClampCursor();
                status_message_ = "E486: Pattern not found: " + last_search_;
            }
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
            ShiftMarksForLineEdit(cursor.row + 1, 1);
            cursor.row++;
            cursor.col = 0;
            EnterInsert();
            break;
        case 'O':
            PushUndo();
            Buf().lines.insert(Buf().lines.begin() + cursor.row, "");
            ShiftMarksForLineEdit(cursor.row, 1);
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
           pending_mark_jump_ != 0 || pending_mark_set_ || pending_ctrl_w_ || pending_org_export_ || pending_z_ ||
           pending_count_ != 0 || awaiting_register_name_ || pending_register_ != 0 || pending_macro_record_ ||
           awaiting_macro_play_ || pending_replace_;
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
    pending_org_export_ = false;
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
    // kReplayCtrlV never reaches DispatchNormalKey (which rejects <= 0
    // outright, same as every other ReplayKey sentinel) -- special-cased
    // here instead so entering Visual Block via Ctrl-V goes through the
    // exact same was_mid/change_scratch_/macro-buffer bookkeeping as 'v'/'V'
    // already do (those dispatch normally, since they're plain printable
    // chars DispatchNormalKey's own switch already handles).
    if (key == kReplayCtrlV) {
        EnterVisualBlock();
    } else {
        DispatchNormalKey(key);
    }

    if (!replaying_change_ && change_recording_active_) {
        if (change_epoch_ != epoch_before) change_had_edit_ = true;
        // A Visual-mode entry (v/V/Ctrl-V) landing here must NOT finalize
        // the in-progress change_scratch_ the way any other "nothing
        // pending, back to a stable mode" key would -- Visual mode has its
        // own multi-key session (selection motions + operator) that isn't
        // done yet, and ProcessVisualKey (see HandleVisualInput) is what
        // actually finalizes it once that session's operator commits.
        bool visual_like = (mode_ == Mode::Visual || mode_ == Mode::VisualLine || mode_ == Mode::VisualBlock);
        if (mode_ != Mode::Insert && !visual_like && !IsMidNormalCommand()) {
            if (change_had_edit_) last_change_keys_ = change_scratch_;
            change_scratch_.clear();
            change_recording_active_ = false;
        }
    }

    // Insert-mode Ctrl-O's one-shot Normal command: once it's actually
    // finished (not still mid-operator/mid-count/etc. -- same test just
    // above) and it didn't already switch to Insert itself (i/a/o/c{motion}/
    // ...), hop back to Insert. If the command left mode_ somewhere other
    // than Normal/Insert (e.g. `v` into Visual), the flag just stays
    // pending -- see insert_one_shot_normal_'s own comment in editor.h.
    if (insert_one_shot_normal_ && mode_ == Mode::Normal && !IsMidNormalCommand()) {
        insert_one_shot_normal_ = false;
        EnterInsert();
        replace_mode_ = insert_one_shot_was_replace_;
        insert_one_shot_was_replace_ = false;
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
        } else if (mode_ == Mode::Visual || mode_ == Mode::VisualLine || mode_ == Mode::VisualBlock) {
            // A recorded Visual-mode change (see ProcessVisualKey) is the
            // literal key sequence from mode entry (v/V/Ctrl-V) through the
            // operator that committed it, replayed the same way a Normal-
            // mode change's motions re-resolve at the new cursor position
            // rather than against stale coordinates -- this is what gives
            // "." a same-*shaped* selection at the new location for free,
            // without a bespoke structured repeat representation. kReplayEscape
            // shouldn't actually appear here in practice (an Escaped-out-of
            // Visual session never edited anything, so ProcessVisualKey never
            // commits it to last_change_keys_), but handled defensively.
            if (keys[i] == kReplayEscape) EnterNormal();
            else if (keys[i] > 0) ProcessVisualKey(keys[i]);
        } else {
            break;  // shouldn't happen -- a recorded change never leaves Normal/Insert/Visual mid-way
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
            if (mode_ == Mode::Visual || mode_ == Mode::VisualLine || mode_ == Mode::VisualBlock) {
                // A macro can legitimately contain a real "escaped out of
                // Visual without committing" sequence (unlike RepeatLastChange's
                // last_change_keys_, which never keeps one) -- e.g. `qa` `v`
                // `j` `Escape` `q` records exactly that, and @a must reproduce
                // leaving Visual mode with no edit, not silently drop the key.
                if (k == kReplayEscape) EnterNormal();
                else if (k > 0) ProcessVisualKey(k);
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
    bool tab_key = false, ctrl_n = false, ctrl_p = false, ctrl_o = false;
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
        else if (key == KEY_O && ctrl) ctrl_o = true;
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
    // Phase 23 tabstop-cycling gap: Insert mode has no built-in Tab
    // behavior of its own (no auto-indent-on-Tab, nothing) to give up, so
    // this only ever *adds* behavior -- consulted after the completion
    // popup above (which still wins Tab when open) and before Escape/etc.
    // below, mirroring the mod1 Tab/S-Tab dispatch's own extra_shift check.
    if (tab_key && insert_tab_hook_ref_ != 0 && lua_) {
        bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        if (lua_->CallRefWithBoolForBool(insert_tab_hook_ref_, shift)) return;
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
    if (ctrl_o) {
        // Insert-mode Ctrl-O: drop into Normal for exactly one command,
        // then hop back to Insert automatically -- see
        // insert_one_shot_normal_'s own comment in editor.h. Distinct from
        // Normal-mode Ctrl-O (jumplist back), which HandleNormalInput
        // handles separately; Vim disambiguates the two by mode too.
        // Not routed through ProcessInsertKey: it's a mode switch, not a
        // text edit, and doesn't participate in macro/`.` recording (same
        // known scope-out as Visual-mode operations, noted in
        // VIM_PARITY_PLAN.md's Phase 9).
        insert_one_shot_normal_ = true;
        insert_one_shot_was_replace_ = replace_mode_;
        EnterNormal();
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
    ShiftMarksForLineEdit(cursor.row + 1, 1);
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
        ShiftMarksForLineEdit(cursor.row, -1);
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
        ShiftMarksForLineEdit(cursor.row + 1, -1);
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
    // "% is read-only (see YankRange's own comment) -- redirect to unnamed.
    if (reg_name == '%') reg_name = 0;
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
    // "0 mirrors the most recent pure yank here too (block delete below
    // does NOT touch it, matching Vim -- only op == 'y' counts as a yank).
    if (op == 'y') registers_['0'] = registers_['"'];

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
        // A cancelled Visual session (no operator ever applied) never
        // becomes a `.`-repeatable change -- ProcessVisualKey only ever
        // commits change_scratch_ into last_change_keys_ when an operator
        // actually fires (see its own comment), and this Escape bypasses
        // ProcessVisualKey entirely (same as Normal mode's own bare Escape,
        // handled before HandleNormalChar's per-key loop), so the
        // in-progress change_scratch_/change_recording_active_ here is just
        // left as stale state for the next real Normal-mode keystroke to
        // overwrite (ProcessNormalKey resets both at the top of every
        // fresh, not-mid-command call) -- nothing to clean up explicitly.
        // Still recorded into an in-progress macro, though: `qa` `v` `j`
        // `Escape` `q` is a real, meaningful macro (selects then
        // deliberately cancels), and @a must reproduce leaving Visual mode.
        if (recording_macro_ && !replaying_change_ && !replaying_macro_) {
            macro_recording_buffer_.push_back(kReplayEscape);
        }
        EnterNormal();  // also clears pending_op_/pending_g_/pending_find_/pending_count_/etc.
        return;
    }

    int cp = GetCharPressed();
    while (cp > 0) {
        if (cp > 0 && cp <= 127) {
            ProcessVisualKey(cp);
            // An operator that committed the change exits back to Normal
            // (d/x/y/~/c/u/U) or into Insert (c, Visual Block I/A) --
            // either way, stop draining this frame's queued chars as
            // Visual-mode keys (matching the old inline code's own
            // `return` from those same cases) and let whichever mode is
            // now active pick up the rest next frame.
            if (mode_ != Mode::Visual && mode_ != Mode::VisualLine && mode_ != Mode::VisualBlock) return;
        }
        cp = GetCharPressed();
    }

    CursorPos &cursor = CurPane().cursor;
    cursor.row = std::max(0, std::min(cursor.row, Buf().LineCount() - 1));
    int len = LineLen(cursor.row);
    cursor.col = std::max(0, std::min(cursor.col, std::max(0, len - 1)));
}

// Wraps DispatchVisualKey with the same macro/`.`-repeat bookkeeping
// ProcessNormalKey/ProcessInsertKey give Normal/Insert mode -- see this
// function's own comment in editor.h for the two distinct ways a Visual
// "session" reaches a commit point (an operator that exits to Normal/Insert,
// or '>'/'<' which deliberately stays in Visual mode).
void Editor::ProcessVisualKey(int key) {
    bool suppress_macro = replaying_change_ || replaying_macro_;
    if (recording_macro_ && !suppress_macro) macro_recording_buffer_.push_back(key);
    if (!replaying_change_ && change_recording_active_) change_scratch_.push_back(key);

    int epoch_before = change_epoch_;
    DispatchVisualKey(key);

    if (!replaying_change_ && change_recording_active_) {
        bool edited_this_key = (change_epoch_ != epoch_before);
        if (edited_this_key) change_had_edit_ = true;
        bool still_visual = (mode_ == Mode::Visual || mode_ == Mode::VisualLine || mode_ == Mode::VisualBlock);
        bool left_to_normal = (!still_visual && mode_ == Mode::Normal && !IsMidNormalCommand());
        // still_visual + edited_this_key only happens for '>'/'<' (the only
        // Visual operators that both edit the buffer and deliberately leave
        // you in Visual mode afterward -- see DispatchVisualKey's own case)
        // -- no dedicated "operator just committed" flag needed, this
        // combination is already unambiguous.
        bool stayed_and_edited = (still_visual && edited_this_key);
        if (left_to_normal || stayed_and_edited) {
            if (change_had_edit_) last_change_keys_ = change_scratch_;
            if (!still_visual) {
                change_scratch_.clear();
                change_had_edit_ = false;
                change_recording_active_ = false;
            }
            // else (stayed_and_edited): leave change_scratch_/
            // change_had_edit_/change_recording_active_ untouched -- a
            // further key within the same still-selected session (another
            // '>' before Escape, say) extends the same accumulated
            // sequence, so its own eventual commit replays the *whole*
            // session, not just the latest bare '>'.
        }
    }
}

// The actual per-key Visual-mode dispatch, called once per key by
// ProcessVisualKey -- formerly HandleVisualInput's own inline loop body
// before Visual mode was wired into repeat/macro recording; unchanged
// logic, just extracted so ProcessVisualKey can wrap it uniformly on both
// real input and replay (RepeatLastChange/PlayMacro).
void Editor::DispatchVisualKey(int cp) {
    char c = static_cast<char>(cp);
    CursorPos &cursor = CurPane().cursor;

    // Same register/count/find/g-prefix handling as Normal mode (see
    // DispatchNormalKey), minus operator-pending combination since Visual
    // mode's own operators (d/x/y below) always act on the whole selection
    // rather than a fresh motion.
    if (pending_find_ == 0 && !pending_g_) {
        if (awaiting_register_name_) {
            awaiting_register_name_ = false;
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '%') {
                pending_register_ = c;
                pending_register_append_ = false;
            } else if (c >= 'A' && c <= 'Z') {
                pending_register_ = static_cast<char>(c - 'A' + 'a');
                pending_register_append_ = true;
            }
            return;
        }
        if (c == '"') {
            awaiting_register_name_ = true;
            return;
        }
        if (c >= '1' && c <= '9') {
            pending_count_ = pending_count_ * 10 + (c - '0');
            return;
        }
        if (c == '0' && pending_count_ != 0) {
            pending_count_ = pending_count_ * 10;
            return;
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
        return;
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
        return;
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
                // tend is an exclusive charwise end; Visual mode's own
                // selection is inclusive on both ends (see
                // ApplyOperatorToSelectionOrCurrentLine's `+1`), so land the
                // cursor one char before it.
                cursor = tend;
                if (cursor.col > 0) {
                    cursor.col--;
                } else if (cursor.row > 0) {
                    cursor.row--;
                    cursor.col = std::max(0, LineLen(cursor.row) - 1);
                }
            }
        }
        return;
    }

    if (TryLuaMapping(mode_, std::string(1, c))) return;

    if (c == 'i' || c == 'a') {
        pending_textobj_scope_ = c;
        return;
    }
    if (c == 'f' || c == 'F' || c == 't' || c == 'T') {
        pending_find_ = c;
        return;
    }
    if (c == 'g') {
        pending_g_ = true;
        return;
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
        return;
    }

    {
        CursorPos target;
        bool linewise = false, inclusive = false;
        if (ResolveMotion(c, cursor, pending_count_, &target, &linewise, &inclusive)) {
            pending_count_ = 0;
            cursor = target;
            // Visual Block's own "sticky $" (see block_to_eol_): '$' extends
            // every row of the block to its own actual end until some other
            // motion changes the column extent.
            if (mode_ == Mode::VisualBlock) block_to_eol_ = (c == '$');
            return;
        }
    }

    switch (c) {
        case 'o': {
            CursorPos &anchor = CurPane().visual_anchor;
            std::swap(anchor, cursor);
            return;
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
        case 'c':
            // ApplyOperator's own op == 'c' branch already switches to
            // Insert mode itself (delete the selection, then EnterInsert())
            // -- unlike d/x/y/~/u/U below, this must NOT also call
            // EnterNormal() afterward, or it would immediately undo that
            // transition. No Visual Block case: Vim doesn't define block-c
            // as distinct from block-d-then-I, and mep doesn't either.
            ApplyOperatorToSelectionOrCurrentLine('c');
            return;
        case '~':
            ApplyOperatorToSelectionOrCurrentLine('~');
            EnterNormal();
            return;
        case 'u':
        case 'U':
            // Lowercase/uppercase the selection -- distinct from Normal
            // mode's own 'u' (undo), which HandleVisualInput never reaches
            // since this dispatch is a completely separate mode.
            ApplyOperatorToSelectionOrCurrentLine(c);
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
            // Vim keeps the selection active after Visual >/< (unlike its
            // other operators) so repeated presses re-indent by another
            // level -- deliberately not calling EnterNormal() here, and
            // leaving anchor/cursor untouched so the exact same range is
            // still selected. ProcessVisualKey detects this "edited but
            // still in Visual mode" combination itself (see its own
            // comment) to commit the accumulated change_scratch_ without
            // ending the recording session.
            return;
        }
        default: break;
    }
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
        ClearNamespace(CreateNamespace("__mep_incsearch"));
        CurPane().cursor = search_anchor_;  // undo incsearch's live preview move
        EnterNormal();  // also cancels any pending operator ("d/foo<Esc>" applies nothing), matching Vim
        return;
    }
    if (enter) {
        bool forward = (mode_ == Mode::SearchForward);
        std::string query = search_query_;
        // Search as an operator motion: capture the pending operator
        // *before* EnterNormal() clears it below -- see the ProcessNormalKey
        // '/'/'?' case (which entered search mode without touching
        // pending_op_/pending_op_start_) for how these got here.
        char op = pending_op_;
        CursorPos op_start = pending_op_start_;
        ClearNamespace(CreateNamespace("__mep_incsearch"));
        CurPane().cursor = search_anchor_;  // undo incsearch's live preview move before the real search runs
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
        if (last_search_.empty()) return;
        if (op != 0) {
            CurPane().cursor = op_start;
            ClampCursor();
            CursorPos result;
            bool wrapped = false;
            if (FindNext(last_search_, last_search_forward_, &result, &wrapped)) {
                // Exclusive charwise, same as every other operator+motion
                // (f/`/w/...): ApplyOperator treats [start, end) as the
                // range and swaps them itself if the match landed before
                // op_start (a backward `?` search).
                ApplyOperator(op, op_start, result, false);
                status_message_ = wrapped ? (last_search_forward_ ? "search hit BOTTOM, continuing at TOP"
                                                                    : "search hit TOP, continuing at BOTTOM")
                                           : "";
            } else {
                status_message_ = "E486: Pattern not found: " + last_search_;
            }
            return;
        }
        PerformSearch(last_search_forward_);
        return;
    }
    if (backspace || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        if (!search_query_.empty()) {
            search_query_.pop_back();
            UpdateIncSearch();
        } else {
            ClearNamespace(CreateNamespace("__mep_incsearch"));
            CurPane().cursor = search_anchor_;
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
        UpdateIncSearch();
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
        UpdateIncSearch();
        return;
    }
    int cp = GetCharPressed();
    bool typed = false;
    while (cp > 0) {
        if (cp >= 32 && cp < 127) {
            search_query_ += static_cast<char>(cp);
            typed = true;
        }
        cp = GetCharPressed();
    }
    if (typed) UpdateIncSearch();
}

// --- Modal overlays (Prompt/Confirm/Select) -------------------------------

void Editor::BeginPrompt(const std::string &title, const std::string &default_text, int on_done_ref,
                          bool masked) {
    overlay_previous_mode_ = mode_;
    prompt_title_ = title;
    prompt_input_ = default_text;
    prompt_callback_ref_ = on_done_ref;
    prompt_native_callback_ = nullptr;
    prompt_masked_ = masked;
    mode_ = Mode::Prompt;
}

void Editor::BeginPromptNative(const std::string &title, const std::string &default_text,
                                std::function<void(const std::string &)> on_done) {
    overlay_previous_mode_ = mode_;
    prompt_title_ = title;
    prompt_input_ = default_text;
    prompt_callback_ref_ = 0;
    prompt_native_callback_ = std::move(on_done);
    prompt_masked_ = false;
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

void Editor::BeginPreview(const std::string &title, const std::string &text) {
    overlay_previous_mode_ = mode_;
    preview_title_ = title;
    preview_text_ = text;
    mode_ = Mode::Preview;
}

void Editor::ShowHover(const std::string &title, const std::string &text) {
    hover_open_ = true;
    hover_title_ = title;
    hover_text_ = text;
    hover_anchor_pos_ = Cursor();
}

void Editor::MaybeDismissHover() {
    if (!hover_open_) return;
    if (mode_ != Mode::Normal || IsKeyPressed(KEY_ESCAPE)) {
        hover_open_ = false;
        return;
    }
    CursorPos cur = Cursor();
    if (cur.row != hover_anchor_pos_.row || cur.col != hover_anchor_pos_.col) hover_open_ = false;
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
        prompt_native_callback_ = nullptr;  // cancel: never invoked, matches the Lua path's own on_done() case
        RestoreFromOverlay();
        if (ref != 0 && lua_) {
            lua_->CallRef(ref);  // no args -> nil, matches vim.ui.input's cancel behavior
            lua_->UnrefFunction(ref);
        }
        return;
    }
    if (enter) {
        int ref = prompt_callback_ref_;
        std::string text = prompt_input_;
        auto native_cb = std::move(prompt_native_callback_);
        prompt_native_callback_ = nullptr;
        RestoreFromOverlay();
        if (native_cb) {
            native_cb(text);
        } else if (ref != 0 && lua_) {
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

// Preview is purely informational -- no on_done callback to invoke, no
// text/selection to edit, so unlike the three overlays above there's
// nothing to decide: any key (Escape included, but not special-cased)
// or a click just acknowledges and closes it.
void Editor::HandlePreviewInput() {
    bool dismiss = false;
    for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed()) dismiss = true;
    for (int cp = GetCharPressed(); cp != 0; cp = GetCharPressed()) dismiss = true;
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) dismiss = true;
    if (dismiss) RestoreFromOverlay();
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

void Editor::ToggleFoldAtCursor() { ToggleFoldAtRow(CurPane().cursor.row); }

// Shared by ToggleFoldAtCursor (keyboard, `za`) and the gutter fold-marker
// click dispatch (NVIM_PARITY_PLAN.md Phase 11 gap: mouse click on the
// statuscolumn's fold indicator) -- the latter needs to toggle the fold at
// whatever row was clicked without first moving the cursor there.
void Editor::ToggleFoldAtRow(int row) {
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

void Editor::SetOrgImageRow(int row, const std::string &path) {
    if (row < 0 || row >= Buf().LineCount()) return;
    Buf().org_image_rows[row] = path;
}

void Editor::ClearOrgImageRows() { Buf().org_image_rows.clear(); }

bool Editor::ToggleOrgImages() {
    org_images_visible_ = !org_images_visible_;
    return org_images_visible_;
}

void Editor::SetOrgLatexRow(int row, const std::string &path, int slots, int end_row) {
    if (row < 0 || row >= Buf().LineCount()) return;
    Buf().org_latex_rows[row] = Buffer::OrgLatexRender{path, std::max(1, slots), std::max(row, end_row)};
}

void Editor::ClearOrgLatexRows() { Buf().org_latex_rows.clear(); }

bool Editor::ToggleOrgLatex() {
    org_latex_visible_ = !org_latex_visible_;
    return org_latex_visible_;
}

void Editor::AddOrgLatexInlineSpan(int row, int col_start, int col_end, const std::string &path) {
    if (row < 0 || row >= Buf().LineCount()) return;
    Buf().org_latex_inline[row].push_back({col_start, col_end, path});
}

void Editor::ClearOrgLatexInlineSpans() { Buf().org_latex_inline.clear(); }

bool Editor::IsRowHiddenByFold(int row, int *fold_start_row) const {
    for (const Fold &f : Buf().folds) {
        if (f.closed && row > f.start_row && row <= f.end_row) {
            if (fold_start_row) *fold_start_row = f.start_row;
            return true;
        }
    }
    return false;
}

// Sitting on a closed fold's start row and stepping down used to move one
// *buffer* row at a time -- landing one row into the fold's own hidden
// interior, which ClampCursor (below) then snapped straight back to
// fold_start since a cursor can never rest there. Net effect: j on a
// folded line didn't move at all, however many rows the fold hid. Vim
// instead steps by *displayed* lines, where a closed fold -- however many
// rows it spans -- counts as exactly one, so moving off its start row
// must clear the whole hidden interior in this one step.
int Editor::StepVisibleRow(int row, int dir) const {
    if (dir > 0) {
        // Compares against the original `row`, not the (possibly already
        // widened) loop variable -- otherwise two closed folds sharing a
        // start row would only ever apply the first one found, since its
        // wider end_row would make the second's `f.start_row == row`
        // check fail.
        int end = row;
        for (const Fold &f : Buf().folds) {
            if (f.closed && f.start_row == row) end = std::max(end, f.end_row);
        }
        row = end + 1;
    } else {
        row--;
        int fold_start;
        if (row >= 0 && IsRowHiddenByFold(row, &fold_start)) row = fold_start;
    }
    return std::max(0, std::min(row, Buf().LineCount() - 1));
}

namespace {
// Nesting depth of `target` among `folds`: how many other folds strictly
// contain its row range. There's no explicit fold tree (Buffer::folds'
// own comment) -- containment is recomputed from the flat row-range list
// every time, which is fine at the sizes an org outline or a manual fold
// set actually reaches.
int FoldNestingDepth(const std::vector<Fold> &folds, const Fold &target) {
    int depth = 0;
    for (const Fold &f : folds) {
        if (&f == &target) continue;
        bool same_range = f.start_row == target.start_row && f.end_row == target.end_row;
        if (!same_range && f.start_row <= target.start_row && f.end_row >= target.end_row) depth++;
    }
    return depth;
}

// The deepest nesting level present, 1-indexed to match vim's own
// 'foldlevel' (foldlevel N shows levels 1..N open) -- 0 if there are no
// folds at all.
int MaxFoldNestingDepth(const std::vector<Fold> &folds) {
    int max_depth = 0;
    for (const Fold &f : folds) max_depth = std::max(max_depth, FoldNestingDepth(folds, f) + 1);
    return max_depth;
}
}  // namespace

bool Editor::IsOrgBuffer() const {
    const std::string &f = Buf().filename;
    return f.size() >= 4 && f.compare(f.size() - 4, 4, ".org") == 0;
}

// Rebuilds provider="org" folds from the buffer's real headline *and*
// greater-block structure via org's own tree-sitter grammar (kFoldsOrg's
// `[(section) (block)] @fold` -- see treesitter_queries.h's comment on
// it): a headline's `section` node already spans exactly itself through
// its last nested subsection, body included, so this reproduces the same
// nesting a hand-rolled `^(%*+)%s+` line scan would -- but *correctly*
// excludes anything inside a `#+begin_src`/`#+begin_example`/etc. block,
// where such a scan would misparse an asterisk sitting at column 0 of the
// block's own contents (a `* comment` in the embedded code, `char *p =
// ...`, ...) as a real headline and corrupt the nesting for the rest of
// the file. `block` folds (one per `#+begin_X ... #+end_X`, whatever X
// is) come from the very same query and land in the very same
// Buf().folds list, nesting inside their enclosing section automatically
// -- no separate code-block-folding pass needed. A degenerate node with
// no body (a single-line section, or an empty block) creates no fold,
// same as CreateFold's own "<2 lines isn't meaningful" rule --
// TreesitterFoldRanges already applies that filter.
//
// Existing org folds (headline or block alike) are preserved by matching
// on start_row: an edit that doesn't move a fold's own opening line keeps
// whatever open/closed state the user left it in; anything new defaults
// open, same as a freshly loaded file where nothing has been folded yet.
void Editor::RecomputeOrgFolds() {
    std::vector<Fold> old_folds;
    for (const Fold &f : Buf().folds) {
        if (f.provider == "org") old_folds.push_back(f);
    }
    ClearFoldsFromProvider("org");

    const std::vector<std::string> &lines = Buf().lines;
    std::string text;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i > 0) text += '\n';
        text += lines[i];
    }

    for (const TSFoldRange &r : TreesitterFoldRanges("org", text)) {
        bool closed = false;
        for (const Fold &of : old_folds) {
            if (of.start_row == r.start_row) {
                closed = of.closed;
                break;
            }
        }
        Buf().folds.push_back({r.start_row, r.end_row, closed, "org"});
    }
}

void Editor::SetAllFoldsClosed(bool closed) {
    for (Fold &f : Buf().folds) f.closed = closed;
    Buf().fold_level = closed ? 0 : MaxFoldNestingDepth(Buf().folds);
}

void Editor::AdjustFoldLevel(int delta) {
    const std::vector<Fold> &folds = Buf().folds;
    int max_depth = MaxFoldNestingDepth(folds);
    int &level = Buf().fold_level;
    int cur = std::clamp((level < 0 ? max_depth : level) + delta, 0, max_depth);
    level = cur;
    for (Fold &f : Buf().folds) f.closed = (FoldNestingDepth(folds, f) + 1 > cur);
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

void Editor::FocusSidebarRow(int id, int line_index) {
    SidebarInstance *sb = FindSidebarMut(id);
    if (!sb || !sb->open) return;
    // Unlike OpenSidebar (called once, when focus actually transitions into
    // a sidebar), this fires on every single mouse click on a row -- most of
    // which land while mode_ is ALREADY Mode::Sidebar (clicking a different
    // row of the same focused sidebar, or a double-click's own first half).
    // Capturing overlay_previous_mode_ unconditionally there would clobber
    // the real pre-sidebar mode with Sidebar itself, permanently breaking
    // RestoreFromOverlay (Escape, and pane-body click-to-focus) until the
    // sidebar is closed some other way. Only capture on the genuine
    // transition into sidebar focus, same as OpenSidebar's own guard.
    if (mode_ != Mode::Sidebar) overlay_previous_mode_ = mode_;
    focused_sidebar_id_ = id;
    mode_ = Mode::Sidebar;
    std::vector<SidebarLine> lines = FlattenSidebar(id);
    sidebar_cursor_ = std::clamp(line_index, 0, std::max(0, static_cast<int>(lines.size()) - 1));
}

void Editor::ActivateSidebarLine(int id, int line_index) {
    std::vector<SidebarLine> lines = FlattenSidebar(id);
    if (line_index < 0 || line_index >= static_cast<int>(lines.size())) return;
    const SidebarLine &line = lines[line_index];
    SidebarInstance *sb = FindSidebarMut(id);
    if (!sb) return;
    if (line.kind == SidebarLine::Kind::SectionHeader) {
        sb->sections[line.section_index].collapsed = !sb->sections[line.section_index].collapsed;
        if (id == focused_sidebar_id_) {
            int max_idx = static_cast<int>(FlattenSidebar(id).size()) - 1;
            sidebar_cursor_ = std::min(sidebar_cursor_, std::max(0, max_idx));
        }
    } else if (line.kind == SidebarLine::Kind::Widget) {
        int ref = sb->sections[line.section_index].widgets[line.widget_index].on_click_ref;
        if (ref != 0 && lua_) lua_->CallRef(ref);
    }
}

void Editor::SetSidebarSize(int id, int size) {
    SidebarInstance *sb = FindSidebarMut(id);
    if (!sb) return;
    // 10 mirrors ResizeActivePane's own local kSidebarMinSize (a function-
    // local constant there, not shared class scope -- not worth a bigger
    // refactor to share a single named constant for one duplicated literal).
    sb->size = std::max(10, size);
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
        ActivateSidebarLine(focused_sidebar_id_, sidebar_cursor_);
        return;
    }
    // Ctrl-E/Ctrl-V: html view-toggle escape hatch (see LoadFile's own
    // force_text param) -- GLFW/raylib doesn't emit a char event while
    // Ctrl is held (same reasoning as HandleNormalInput's own Ctrl-combo
    // comment), so these can't reach mep.tree_on_key's fn(char) through
    // the GetCharPressed() drain below the way every other binding
    // (R/H/o/a/r/d/?) does. Passed through as the sentinels "C-e"/"C-v",
    // never produced by GetCharPressed() (only single printable-ASCII
    // chars), so on_key can tell a Ctrl-combo apart from a bare keypress
    // via the same callback.
    {
        bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
        bool ce = ctrl && IsKeyPressed(KEY_E);
        bool cv = ctrl && IsKeyPressed(KEY_V);
        if (ce || cv) {
            const SidebarInstance *sb = FindSidebar(focused_sidebar_id_);
            if (sb && sb->on_key_ref != 0 && lua_) {
                lua_->CallRefWithString(sb->on_key_ref, ce ? "C-e" : "C-v");
            }
            return;
        }
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

std::string Utf8FromCodepoint(int cp) {
    std::string out;
    if (cp <= 0x7F) {
        out += static_cast<char>(cp);
    } else if (cp <= 0x7FF) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return out;
}

std::string IconForFilename(const std::string &name) {
    // Full-name special cases checked before falling back to extension.
    // Codepoints match mep.nvim/lua/mep/icons/data.lua's own M.nerd_font
    // table (nvim-web-devicons' defaults) so both editors show the same
    // icon for the same file; LICENSE/.env weren't in the C++ side's old
    // ASCII table's mep.nvim counterpart either, so they fall back to
    // generic file/lock icons below instead of inventing new codepoints.
    static const std::unordered_map<std::string, int> kByName = {
        {"Makefile", 0xe779},        {"makefile", 0xe779},        {"CMakeLists.txt", 0xf15b},
        {"Dockerfile", 0xf0868},     {".gitignore", 0xe702},      {".gitmodules", 0xe702},
        {"README.md", 0xf48a},       {"README.org", 0xe633},      {"README", 0xf48a},
        {"LICENSE", 0xe60a},         {".env", 0xf462},            {".editorconfig", 0xe652},
    };
    auto by_name = kByName.find(name);
    if (by_name != kByName.end()) return Utf8FromCodepoint(by_name->second);

    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == name.size() - 1) return Utf8FromCodepoint(0xf15b);  // nf-fa-file
    std::string ext = name.substr(dot + 1);
    for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    static const std::unordered_map<std::string, int> kByExt = {
        {"c", 0xe61e},    {"h", 0xf0fd},    {"cpp", 0xe61d},  {"cc", 0xe61d},   {"cxx", 0xe61d},  {"hpp", 0xf0fd},
        {"lua", 0xe620},  {"py", 0xe606},   {"js", 0xe60c},   {"ts", 0xe628},   {"jsx", 0xe625},  {"tsx", 0xe7ba},
        {"rs", 0xe68b},   {"go", 0xe627},   {"java", 0xe738}, {"rb", 0xe791},   {"sh", 0xe795},   {"bash", 0xe760},
        {"zsh", 0xe795},  {"md", 0xf48a},   {"markdown", 0xe609}, {"org", 0xe633}, {"txt", 0xf0219}, {"json", 0xe60b},
        {"yaml", 0xe8eb}, {"yml", 0xe8eb},  {"toml", 0xe6b2}, {"xml", 0xf05c0}, {"html", 0xe736}, {"css", 0xe6b8},
        {"scss", 0xe603}, {"sql", 0xe706},  {"vim", 0xe62b},  {"lock", 0xe672}, {"log", 0xf0331}, {"cs", 0xf031b},
        {"php", 0xe608},  {"csv", 0xe64a},  {"env", 0xf462},
        {"git", 0xe702},  {"png", 0xe60d},  {"jpg", 0xe60d},  {"jpeg", 0xe60d}, {"gif", 0xe60d},  {"svg", 0xf0721},
        {"pdf", 0xeaeb},  {"zip", 0xf410},  {"tar", 0xf410},  {"gz", 0xf410},
    };
    auto by_ext = kByExt.find(ext);
    if (by_ext != kByExt.end()) return Utf8FromCodepoint(by_ext->second);
    return Utf8FromCodepoint(0xf15b);  // nf-fa-file, generic fallback
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
                         int on_query_change_ref, int on_key_ref, int on_select_change_ref, bool raw_results) {
    overlay_previous_mode_ = mode_;
    picker_open_ = true;
    picker_title_ = title;
    picker_query_.clear();
    picker_items_ = std::move(items);
    picker_selected_ = 0;
    picker_on_select_ref_ = on_select_ref;
    picker_on_query_change_ref_ = on_query_change_ref;
    picker_on_key_ref_ = on_key_ref;
    picker_on_select_change_ref_ = on_select_change_ref;
    picker_raw_results_ = raw_results;
    picker_preview_text_.clear();
    picker_preview_scroll_ = 0;
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
    int select_change_ref = picker_on_select_change_ref_;
    ClosePicker();
    if (!lua_) return;
    if (select_ref != 0) lua_->UnrefFunction(select_ref);
    if (query_ref != 0) lua_->UnrefFunction(query_ref);
    if (key_ref != 0) lua_->UnrefFunction(key_ref);
    if (select_change_ref != 0) lua_->UnrefFunction(select_change_ref);
}

void Editor::SetPickerItems(std::vector<PickerItem> items) {
    picker_items_ = std::move(items);
    int max_idx = static_cast<int>(PickerFilteredResults().size()) - 1;
    picker_selected_ = std::max(0, std::min(picker_selected_, max_idx));
}

std::vector<PickerItem> Editor::PickerFilteredResults() const {
    if (picker_raw_results_ || picker_query_.empty()) return picker_items_;
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
    // Snapshot the currently-highlighted item's `data` *before* this frame's
    // navigation/query edits are applied below, so the on_select_change_ref
    // firing at the bottom of this function can tell whether the effective
    // selection actually moved to a different item (arrow keys, Ctrl-N/P, or
    // the query narrowing to a new top match) -- live theme preview and
    // similar consumers only want to fire on a real change, not every frame.
    std::string pre_nav_data;
    if (picker_on_select_change_ref_ != 0) {
        std::vector<PickerItem> pre_nav = PickerFilteredResults();
        if (picker_selected_ >= 0 && picker_selected_ < static_cast<int>(pre_nav.size())) {
            pre_nav_data = pre_nav[picker_selected_].data;
        }
    }
    if (escape) {
        int ref = picker_on_select_ref_;
        int qref = picker_on_query_change_ref_;
        int kref = picker_on_key_ref_;
        int scref = picker_on_select_change_ref_;
        ClosePicker();
        if (lua_) {
            lua_->CallRef(ref);
            lua_->UnrefFunction(ref);
            if (qref != 0) lua_->UnrefFunction(qref);
            if (kref != 0) lua_->UnrefFunction(kref);
            if (scref != 0) lua_->UnrefFunction(scref);
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
        int scref = picker_on_select_change_ref_;
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
            if (scref != 0) lua_->UnrefFunction(scref);
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
    // Live-preview hook (NVIM_PARITY_PLAN.md Phase 9's theme picker gap):
    // fires whenever the highlighted item actually changed this frame,
    // whether from explicit navigation or the query re-filtering to a new
    // top match. Deliberately compares by `data` rather than by index --
    // an unchanged index after a query edit can point at a different item.
    if (lua_ && picker_on_select_change_ref_ != 0) {
        std::vector<PickerItem> post_nav = PickerFilteredResults();
        if (picker_selected_ >= 0 && picker_selected_ < static_cast<int>(post_nav.size())) {
            const std::string &post_nav_data = post_nav[picker_selected_].data;
            if (post_nav_data != pre_nav_data) {
                lua_->CallRefWithString(picker_on_select_change_ref_, post_nav_data);
            }
        }
    }
}

// --- Roam backlink-graph view (NVIM_PARITY_PLAN.md Phase 37's flagged
// "no fuzzy backlink-graph visualization" gap, closed) ---------------------
//
// Scope decision (see NVIM_PARITY_PLAN.md's updated Phase 37 section for
// the full writeup): a real force-directed/physics graph layout was judged
// genuinely excessive for this editor, so the layout here is deterministic
// -- hop distance from the note the view was opened on picks a ring
// (main.cpp's ComputeRoamGraphPositions), index within that ring picks an
// angle. No iterative relaxation, no edge-crossing minimization. What *is*
// real: the node/edge graph itself (parsed straight from every roam note's
// `[[id:...]]` links by the Lua side, reusing the same file-scanning
// helpers the flat backlinks sidebar already used), the fuzzy filter
// (Phase 8's FuzzyScore, same scorer the picker uses), and the
// navigation/selection/Enter-to-open flow below.

void Editor::OpenRoamGraph(const std::string &title, std::vector<RoamGraphNode> nodes,
                            std::vector<RoamGraphEdge> edges, int on_select_ref) {
    overlay_previous_mode_ = mode_;
    roam_graph_open_ = true;
    roam_graph_title_ = title;
    roam_graph_nodes_ = std::move(nodes);
    roam_graph_edges_ = std::move(edges);
    roam_graph_query_.clear();
    // Start on the first non-center node (index 0 is always the note the
    // view was opened *on* -- re-opening it from its own graph is rarely
    // what Enter is for), falling back to 0 if it's the only node (an
    // isolated note with no links either direction). Valid because the
    // filtered-index list is the identity mapping at open time (query
    // starts empty).
    roam_graph_selected_ = roam_graph_nodes_.size() > 1 ? 1 : 0;
    if (roam_graph_on_select_ref_ != 0 && lua_) lua_->UnrefFunction(roam_graph_on_select_ref_);
    roam_graph_on_select_ref_ = on_select_ref;
    mode_ = Mode::RoamGraph;
}

void Editor::CloseRoamGraphDiscardingCallback() {
    roam_graph_open_ = false;
    if (mode_ == Mode::RoamGraph) RestoreFromOverlay();
    int ref = roam_graph_on_select_ref_;
    roam_graph_on_select_ref_ = 0;
    if (ref != 0 && lua_) lua_->UnrefFunction(ref);
}

std::vector<int> Editor::RoamGraphFilteredIndices() const {
    std::vector<int> out;
    for (int i = 0; i < static_cast<int>(roam_graph_nodes_.size()); i++) {
        // Node 0 (the center note the view was opened on) is always kept
        // visible/selectable -- it's the anchor, not a search result.
        if (i == 0 || roam_graph_query_.empty() ||
            FuzzyScore(roam_graph_nodes_[i].title, roam_graph_query_) >= 0) {
            out.push_back(i);
        }
    }
    return out;
}

void Editor::HandleRoamGraphInput() {
    bool escape = false, enter = false, backspace = false;
    for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed()) {
        if (key == KEY_ESCAPE) escape = true;
        else if (key == KEY_ENTER) enter = true;
        else if (key == KEY_BACKSPACE) backspace = true;
    }
    if (escape) {
        int ref = roam_graph_on_select_ref_;
        roam_graph_on_select_ref_ = 0;
        roam_graph_open_ = false;
        RestoreFromOverlay();
        if (lua_ && ref != 0) {
            lua_->CallRef(ref);  // no argument: same nil-on-cancel convention as the picker
            lua_->UnrefFunction(ref);
        }
        return;
    }
    if (enter) {
        std::vector<int> filtered = RoamGraphFilteredIndices();
        bool has_selection = roam_graph_selected_ >= 0 && roam_graph_selected_ < static_cast<int>(filtered.size());
        std::string path = has_selection ? roam_graph_nodes_[filtered[roam_graph_selected_]].path : std::string();
        int ref = roam_graph_on_select_ref_;
        roam_graph_on_select_ref_ = 0;
        roam_graph_open_ = false;
        RestoreFromOverlay();
        if (lua_ && ref != 0) {
            if (has_selection) lua_->CallRefWithString(ref, path);
            else lua_->CallRef(ref);
            lua_->UnrefFunction(ref);
        }
        return;
    }
    bool query_changed = false;
    if (backspace || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        if (!roam_graph_query_.empty()) {
            roam_graph_query_.pop_back();
            query_changed = true;
        }
    }
    // Left/Up and Right/Down cycle through the filtered set in order --
    // a flat cyclic walk rather than spatial nearest-neighbor search over
    // the ring layout: simpler, and every node stays reachable in a
    // bounded number of presses, which is what "arrow keys move a
    // selected node highlight between graph nodes" actually needs; click-
    // to-select is deliberately not implemented (see NVIM_PARITY_PLAN.md's
    // scope-cut note -- the plan's own wording offers "arrow keys or
    // click", and arrow keys alone already give full node coverage).
    int filtered_count = static_cast<int>(RoamGraphFilteredIndices().size());
    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT) || IsKeyPressed(KEY_DOWN) ||
        IsKeyPressedRepeat(KEY_DOWN)) {
        if (filtered_count > 0) roam_graph_selected_ = (roam_graph_selected_ + 1) % filtered_count;
    }
    if (IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT) || IsKeyPressed(KEY_UP) ||
        IsKeyPressedRepeat(KEY_UP)) {
        if (filtered_count > 0) roam_graph_selected_ = (roam_graph_selected_ - 1 + filtered_count) % filtered_count;
    }
    int cp = GetCharPressed();
    while (cp > 0) {
        if (cp >= 32 && cp < 127) {
            roam_graph_query_ += static_cast<char>(cp);
            query_changed = true;
        }
        cp = GetCharPressed();
    }
    if (query_changed) {
        int n = static_cast<int>(RoamGraphFilteredIndices().size());
        roam_graph_selected_ = std::max(0, std::min(roam_graph_selected_, std::max(0, n - 1)));
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
        completion_last_query_prefix_.clear();
        return;
    }
    // The completion source (mep.completion_buffer_words by default) is an
    // O(buffer size) Lua scan -- re-running it unconditionally every frame
    // (as this used to) is the single biggest cause of typing feeling
    // laggy in anything but a tiny file: this function runs every frame
    // Insert mode is active with a 2+ char prefix, at 60fps, regardless of
    // whether a character was typed *that* frame. Two guards, mirroring
    // the "coalesce to an interval, not every trigger" idiom
    // kBuiltinEditHooks' mep.on_buffer_changed already uses elsewhere:
    // skip entirely once the prefix stops changing (the overwhelming
    // majority of frames, since 60fps vastly outpaces keystroke rate --
    // this alone eliminates nearly all the redundant work), and debounce
    // genuine prefix changes to a modest interval so a fast typing burst
    // doesn't demand a full rescan for every single character.
    // completion_last_query_prefix_ is reset to empty (a value this can
    // never equal, since prefix is always >= 2 chars here) on Insert-mode
    // exit (EnterNormal) so a later session can't skip its first query by
    // coincidentally starting with the same prefix text some earlier,
    // unrelated session ended on.
    if (prefix == completion_last_query_prefix_) return;
    constexpr double kMinQueryIntervalSec = 0.05;
    double now = GetTime();
    if (now - completion_last_query_time_ < kMinQueryIntervalSec) return;
    completion_last_query_prefix_ = prefix;
    completion_last_query_time_ = now;

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
    const std::string word = completion_items_[completion_selected_].data;
    CursorPos &cursor = CurPane().cursor;
    std::string &line = Buf().lines[cursor.row];
    PushUndo();
    line.erase(completion_word_start_col_, cursor.col - completion_word_start_col_);
    line.insert(completion_word_start_col_, word);
    cursor.col = completion_word_start_col_ + static_cast<int>(word.size());
    Buf().modified = true;
    completion_open_ = false;
    // Phase 23 LSP-snippet gap: let a registered Lua hook inspect what was
    // just inserted (cursor is already at its end) and, if `word` turns out
    // to be a Snippet-format LSP item's raw insertText rather than plain
    // text, replace it with a real tabstop expansion. See
    // SetCompletionAcceptHookRef's comment (editor.h) for why this passes
    // just the text rather than a richer struct.
    if (completion_accept_hook_ref_ != 0 && lua_) {
        lua_->CallRefWithString(completion_accept_hook_ref_, word);
    }
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
        "MepNextSheet", "MepPrevSheet",
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
    // See UpdateCompletionPopup's own comment: without this, a later
    // insert session could skip its first completion query by
    // coincidentally starting with the same prefix text this one ended
    // on, showing stale completions left over from a different context.
    completion_last_query_prefix_.clear();
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
    pending_org_export_ = false;
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
    // incsearch previews matches from -- and Escape restores the cursor
    // to -- wherever the cursor was when the prompt opened, not wherever a
    // pending operator's own start was (the two coincide whenever '/'/'?'
    // is the very first key of the command, which is every case reachable
    // today, but keeping them conceptually separate matches how
    // pending_op_start_ itself is captured independently of the cursor).
    search_anchor_ = CurPane().cursor;
    ClearNamespace(CreateNamespace("__mep_incsearch"));
}

// Live preview for the search prompt (VIM_PARITY_PLAN.md Phase 4's
// incsearch stretch item): re-searches from search_anchor_ on every
// keystroke, previewing the cursor at the match (scrolling it into view
// for free, since the normal per-frame scroll-follows-cursor logic doesn't
// care why the cursor moved) and highlighting the matched span via the
// same Decoration/hl_group pipeline every other feature (diagnostics,
// git-gutter, colorizer, ...) uses. An empty query or a failed search
// leaves the cursor at the anchor with nothing highlighted, matching Vim's
// incsearch (it doesn't jump around on a pattern that doesn't match yet).
void Editor::UpdateIncSearch() {
    int ns = CreateNamespace("__mep_incsearch");
    ClearNamespace(ns);
    CurPane().cursor = search_anchor_;
    ClampCursor();
    if (search_query_.empty()) return;
    bool forward = (mode_ == Mode::SearchForward);
    CursorPos result;
    if (!SearchOnce(search_query_, search_anchor_, forward, &result)) return;
    CurPane().cursor = result;
    ClampCursor();
    // SearchOnce only reports the match's start column, not its length --
    // recompute the length the same way it found the match in the first
    // place (regex if the pattern compiles as one, else the literal
    // pattern's own length) purely for the highlight span's extent; the
    // match itself was already found above.
    const std::string &line = Buf().lines[result.row];
    int match_len = static_cast<int>(search_query_.size());
    mep_regex::Regex re(search_query_, ignore_case_);
    if (re.ok()) {
        mep_regex::Match m = re.Search(line, result.col);
        if (m.ok() && m.start == result.col && m.end > m.start) match_len = m.end - m.start;
    }
    int col_end = std::min(static_cast<int>(line.size()), result.col + std::max(1, match_len));
    if (col_end > result.col) {
        Decoration deco;
        deco.row = result.row;
        deco.col_start = result.col;
        deco.col_end = col_end;
        deco.hl_group = "IncSearch";
        deco.priority = 100;
        AddDecoration(ns, deco);
    }
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
    // Search is regex by default now (matching real Vim -- plain-substring
    // was an explicit, documented stretch-cut in VIM_PARITY_PLAN.md, kept
    // only as long as no real regex engine existed yet). A pattern that
    // fails to *compile* as regex (e.g. an unbalanced '(' or '[' someone
    // typed meaning it literally, not as regex syntax) falls back to the
    // original plain-substring CiFind/CiRfind below instead of just
    // erroring, so every search that already worked before regex support
    // landed keeps working exactly the same, and "search for a literal
    // paren" doesn't require learning to escape it.
    mep_regex::Regex re(pattern, ignore_case_);
    bool use_regex = re.ok();
    int n = Buf().LineCount();
    if (forward) {
        for (int r = from.row; r < n; r++) {
            const std::string &line = Buf().lines[r];
            size_t start = (r == from.row) ? static_cast<size_t>(from.col) + 1 : 0;
            if (start > line.size()) continue;
            if (use_regex) {
                mep_regex::Match m = re.Search(line, static_cast<int>(start));
                if (m.ok()) {
                    *result = {r, m.start};
                    return true;
                }
                continue;
            }
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
        if (use_regex) {
            // Backward search wants the *rightmost* match at or before
            // `limit` -- this engine has no native reverse search, so scan
            // every match left-to-right and keep the last one that still
            // qualifies. Lines are short enough for this to be cheap.
            int best = -1;
            int pos = 0;
            while (pos <= static_cast<int>(line.size())) {
                mep_regex::Match m = re.Search(line, pos);
                if (!m.ok() || m.start > limit) break;
                best = m.start;
                pos = (m.end > m.start) ? m.end : m.start + 1;
            }
            if (best >= 0) {
                *result = {r, best};
                return true;
            }
            continue;
        }
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

// Shared by MoveSentenceForward/MoveSentenceBackward: finds the start of
// the next sentence at-or-after `from`. A sentence ends at '.', '!', or
// '?', optionally followed by any run of closing ')', ']', '"', '\''
// characters, followed by end-of-line or a space/tab (Vim's own :help
// sentence definition) -- the returned position is the first non-blank
// character after that boundary. A blank line is also a sentence (and
// paragraph) boundary in its own right, matching `}`'s own treatment of
// one, and stops the scan even with no punctuation involved.
CursorPos Editor::NextSentenceStart(CursorPos from) const {
    int nrows = Buf().LineCount();
    int row = from.row, col = from.col;
    while (row < nrows) {
        int len = LineLen(row);
        // A blank line is a boundary -- but not the one we started ON
        // (nothing "new" to stop at there).
        if (len == 0 && row != from.row) return {row, 0};
        while (col < len) {
            char c = Buf().lines[row][col];
            if (c == '.' || c == '!' || c == '?') {
                int p = col + 1;
                while (p < len) {
                    char cc = Buf().lines[row][p];
                    if (cc == ')' || cc == ']' || cc == '"' || cc == '\'') {
                        p++;
                        continue;
                    }
                    break;
                }
                if (p >= len || Buf().lines[row][p] == ' ' || Buf().lines[row][p] == '\t') {
                    // Boundary found. Skip whitespace forward (across
                    // lines, but stopping AT a blank line rather than
                    // past it) to land on the next sentence's first
                    // non-blank character.
                    int r = row, c2 = p;
                    while (true) {
                        int l = LineLen(r);
                        if (l == 0) return {r, 0};
                        if (c2 >= l) {
                            r++;
                            c2 = 0;
                            if (r >= nrows) return {nrows - 1, LineLen(nrows - 1)};
                            continue;
                        }
                        char wc = Buf().lines[r][c2];
                        if (wc == ' ' || wc == '\t') {
                            c2++;
                            continue;
                        }
                        return {r, c2};
                    }
                }
            }
            col++;
        }
        row++;
        col = 0;
    }
    // No further sentence: clamp to the very end, same as `}` clamping to
    // the last line.
    int last = std::max(0, nrows - 1);
    return {last, LineLen(last)};
}

// ): sentence forward -- just the next-sentence-start scan above.
CursorPos Editor::MoveSentenceForward(CursorPos from) const { return NextSentenceStart(from); }

// (: sentence backward. Enumerates sentence-start positions from the top
// of the buffer via NextSentenceStart (matching a "...punctuation then
// whitespace" pattern is far easier left-to-right than in reverse), and
// returns the last one strictly before `from` -- or, if `from` is already
// sitting exactly on one, the one before THAT, matching Vim's "already at
// the start: go to the previous sentence" behavior.
CursorPos Editor::MoveSentenceBackward(CursorPos from) const {
    auto le_from = [&](CursorPos p) { return p.row < from.row || (p.row == from.row && p.col <= from.col); };
    CursorPos cur{0, 0};
    CursorPos prev{0, 0};
    while (le_from(cur)) {
        CursorPos next = NextSentenceStart(cur);
        bool progressed = next.row != cur.row || next.col != cur.col;
        if (!progressed || !le_from(next)) break;
        prev = cur;
        cur = next;
    }
    if (cur.row == from.row && cur.col == from.col) return prev;
    return cur;
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
            int row = from.row;
            for (int i = 0; i < n; i++) row = StepVisibleRow(row, 1);
            *target = {row, from.col};  // preserve column, like Vim's "desired column"
            *linewise = true;
            return true;
        }
        case 'k': {
            int row = from.row;
            for (int i = 0; i < n; i++) row = StepVisibleRow(row, -1);
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
        case '(': {
            CursorPos t = from;
            for (int i = 0; i < n; i++) t = MoveSentenceBackward(t);
            *target = t;
            return true;
        }
        case ')': {
            CursorPos t = from;
            for (int i = 0; i < n; i++) t = MoveSentenceForward(t);
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

// Keeps marks pointing at the same TEXT (not the same row number) across
// edits that insert/delete whole lines -- called right after the
// Buf().lines.insert/erase that does the actual shifting, so LineCount()
// already reflects the new state. See the declaration in editor.h for the
// at_row/count sign convention.
void Editor::ShiftMarksForLineEdit(int at_row, int count) {
    if (count == 0 || Buf().marks.empty()) return;
    int n = Buf().LineCount();
    for (auto &kv : Buf().marks) {
        CursorPos &pos = kv.second;
        if (count > 0) {
            if (pos.row >= at_row) pos.row += count;
        } else {
            int removed = -count;
            if (pos.row >= at_row + removed) {
                pos.row += count;  // count already negative
            } else if (pos.row >= at_row) {
                // Mark pointed inside the deleted range: clamp to the
                // deletion point rather than drop it or let it go stale.
                pos.row = at_row;
            }
        }
        pos.row = std::max(0, std::min(pos.row, n - 1));
    }
}

// Records where a "big jump" (G, gg, a mark jump, a search) started from,
// so `` `` ``/`''` can return to it. Called with the pre-jump cursor.
void Editor::RecordJumpFrom(CursorPos pos) {
    Buf().last_jump_from = pos;
    Buf().has_last_jump = true;
    // Same set of call sites also feeds the full jumplist (Ctrl-O/Ctrl-I) --
    // see PushJump's own comment for why this is a separate structure
    // rather than replacing last_jump_from above.
    PushJump(pos, CurPane().buffer_id);
}

// --- Jumplist (Ctrl-O/Ctrl-I) -----------------------------------------------

void Editor::PushJump(CursorPos pos, int buffer_id) {
    Pane &p = CurPane();
    // A new jump truncates any forward ("Ctrl-I-able") history past the
    // current index, same as a fresh edit truncates redo history.
    if (p.jumplist_index < static_cast<int>(p.jumplist.size())) {
        p.jumplist.erase(p.jumplist.begin() + p.jumplist_index, p.jumplist.end());
    }
    // Jumping to the exact same spot twice in a row (e.g. repeated `` `` ``
    // toggling) shouldn't grow the list -- matches Vim.
    if (!p.jumplist.empty() && p.jumplist.back().buffer_id == buffer_id && p.jumplist.back().pos.row == pos.row &&
        p.jumplist.back().pos.col == pos.col) {
        p.jumplist_index = static_cast<int>(p.jumplist.size());
        return;
    }
    p.jumplist.push_back({buffer_id, pos});
    if (p.jumplist.size() > kMaxJumplist) p.jumplist.erase(p.jumplist.begin());
    p.jumplist_index = static_cast<int>(p.jumplist.size());
}

void Editor::GoToJumpEntry(const JumpEntry &entry) {
    if (entry.buffer_id >= 0 && entry.buffer_id < static_cast<int>(buffers_.size())) {
        CurPane().buffer_id = entry.buffer_id;
    }
    CurPane().cursor = entry.pos;
    ClampCursor();
}

void Editor::JumpListBack() {
    Pane &p = CurPane();
    if (p.jumplist.empty()) return;
    if (p.jumplist_index >= static_cast<int>(p.jumplist.size())) {
        // First Ctrl-O from "live" (never backed up yet): remember where
        // we are right now so a later Ctrl-I can return here, matching
        // Vim's own jumplist behavior.
        p.jumplist.push_back({p.buffer_id, p.cursor});
        p.jumplist_index = static_cast<int>(p.jumplist.size()) - 1;
    }
    if (p.jumplist_index <= 0) return;
    p.jumplist_index--;
    GoToJumpEntry(p.jumplist[p.jumplist_index]);
}

void Editor::JumpListForward() {
    Pane &p = CurPane();
    if (p.jumplist_index + 1 >= static_cast<int>(p.jumplist.size())) return;
    p.jumplist_index++;
    GoToJumpEntry(p.jumplist[p.jumplist_index]);
}

void Editor::TryRunOrgBabelAtCursor() {
    const std::string &filename = Buf().filename;
    if (filename.size() < 4 || filename.compare(filename.size() - 4, 4, ".org") != 0) return;
    auto it = lua_commands_.find("MepOrgBabelExecute");
    if (it != lua_commands_.end() && lua_) lua_->CallRefWithString(it->second, "");
}

void Editor::TryRunOrgExport(char format_key) {
    const std::string &filename = Buf().filename;
    if (filename.size() < 4 || filename.compare(filename.size() - 4, 4, ".org") != 0) return;
    const char *command = nullptr;
    switch (format_key) {
        case 'h': command = "MepOrgExportHtml"; break;
        case 'p': command = "MepOrgExportPdf"; break;
        case 'o': command = "MepOrgExportOdt"; break;
        case 'm': command = "MepOrgExportMarkdown"; break;
        case 'a': command = "MepOrgExportAscii"; break;
        default: return;
    }
    auto it = lua_commands_.find(command);
    if (it != lua_commands_.end() && lua_) lua_->CallRefWithString(it->second, "");
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
        // "0: last-yank register. Always mirrors the most recent *pure*
        // yank (never a delete/change) regardless of whether a named
        // register was also targeted, and never shifts -- matching Vim.
        // registers_['"'] already holds the final text YankRange just
        // wrote (see its own comment), so just copy that.
        registers_['0'] = registers_['"'];
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
    // Numbered registers: real Vim shifts "1-"9 down (dropping "9) and
    // stores the freshly deleted/changed text in "1 -- but only for
    // deletes of at least a full line (linewise, or charwise spanning
    // multiple lines). A delete smaller than one line goes to the
    // small-delete register "- instead and never touches "1-"9, matching
    // Vim's own documented split between quote1 and quote_ ("-).
    {
        std::string deleted_text = ExtractRangeText(start, end, linewise);
        bool small_delete = !linewise && start.row == end.row;
        if (small_delete) {
            if (!deleted_text.empty()) registers_['-'] = Register{deleted_text, false, false};
        } else {
            for (char r = '9'; r > '1'; r--) registers_[r] = registers_[r - 1];
            registers_['1'] = Register{deleted_text, linewise, false};
        }
    }
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
        ShiftMarksForLineEdit(row + 1, -1);
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
        ShiftMarksForLineEdit(first, -(last - first + 1));
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
        ShiftMarksForLineEdit(start.row + 1, -(end_row - start.row));
    }
    Buf().modified = true;
}

std::string Editor::ExtractRangeText(CursorPos start, CursorPos end, bool linewise) const {
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
    return text;
}

void Editor::YankRange(CursorPos start, CursorPos end, bool linewise, char reg_name, bool append) {
    std::string text = ExtractRangeText(start, end, linewise);

    // "% is a read-only pseudo-register (current filename); writing into
    // it, e.g. via "%yy or "%dd, is a silent no-op in Vim -- fall back to
    // the unnamed register so the yank/delete itself still behaves
    // normally otherwise.
    if (reg_name == '%') reg_name = 0;

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
    ShiftMarksForLineEdit(pos.row + 1, static_cast<int>(to_insert.size()));

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
        ShiftMarksForLineEdit(insert_at, static_cast<int>(new_lines.size()));
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
        ShiftMarksForLineEdit(insert_at, static_cast<int>(new_lines.size()));
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
    // Regex by default, same reasoning and same literal-pattern fallback
    // as SearchOnce above (see its own comment) -- :s's pattern is the
    // same "last search pattern" search itself uses (see last_search_
    // assignment below), so the two had better agree on what counts as a
    // match. `replacement` may reference \1-\9/\0/& (Regex::
    // ExpandReplacement, shared with normal Search()-based :s/normal
    // substring replace both use the *plain* pattern.size()-based splice
    // below when regex compilation fails, so \-group references in
    // `replacement` are only meaningful when the pattern actually
    // compiled as regex -- matches real Vim, where backreferences in a
    // replacement only make sense once the search side is regex too).
    mep_regex::Regex re(pattern, ignore_case_);
    bool use_regex = re.ok();
    int replacements = 0, lines_changed = 0;
    int last_row = start_row;
    bool pushed_undo = false;
    for (int r = start_row; r <= end_row; r++) {
        std::string &line = Buf().lines[r];
        size_t pos = 0;
        bool changed_this_line = false;
        while (true) {
            size_t found;
            size_t match_len;
            std::string this_replacement = replacement;
            if (use_regex) {
                mep_regex::Match m = re.Search(line, static_cast<int>(pos));
                if (!m.ok()) break;
                found = static_cast<size_t>(m.start);
                match_len = static_cast<size_t>(m.end - m.start);
                this_replacement = mep_regex::Regex::ExpandReplacement(line, m, replacement);
            } else {
                found = CiFind(line, pattern, pos);
                if (found == std::string::npos) break;
                match_len = pattern.size();
            }
            // Deferred until the first real match, not called unconditionally
            // up front -- otherwise a :s with zero matches (which returns
            // "E486: Pattern not found" below and touches nothing) would
            // still push a wasted, do-nothing undo snapshot.
            if (!pushed_undo) {
                PushUndo();
                pushed_undo = true;
            }
            line.replace(found, match_len, this_replacement);
            replacements++;
            changed_this_line = true;
            pos = found + this_replacement.size();
            if (match_len == 0) pos = found + 1;  // guard against an infinite loop on a zero-width match
            if (!global_flag) break;
            if (pattern.empty()) break;
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
        // force_text=true: the literal `:e`/`:edit` ex-command is the one
        // deliberate escape hatch back to plain-text for an .html/.htm
        // file (LoadFile's own comment) -- every other file-open path in
        // this editor (Enter in the file tree/pickers, LSP jumps, etc.)
        // goes through mep.open instead, which always opens the default
        // (rendered) view.
        LoadFile(args, true);
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
            } else if (key == "cursorline" || key == "cul") {
                show_cursorline_ = value;
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
    } else if (name == "MepNextSheet") {
        NextSheet();
    } else if (name == "MepPrevSheet") {
        PrevSheet();
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
            // `args` is whatever followed the command name verbatim (""
            // if nothing did) -- e.g. lets `:MepGitGutter base <ref>`
            // (Phase 17 gap) be one mep.command('MepGitGutter', fn)
            // registration that switches on its first word, rather than
            // needing a second ex-command. Existing zero-arg handlers
            // (declared `function() ... end`) are unaffected: Lua
            // silently discards an argument nothing accepts.
            lua_->CallRefWithString(it->second, args);
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
    // Terminal buffers have no filename (they're never saved), so without
    // this they'd all show as the same indistinguishable "[No Name]" --
    // defeating the point of surfacing them here at all now that closing
    // their pane no longer kills them (see ClosePane/TabDelete): this is
    // how a backgrounded terminal actually gets found again to switch back
    // into view. Same live-title-over-argv[0] preference as DrawPane's own
    // pane-header label, for the same reason (a shell's OSC title update).
    if (const TerminalSession *sess = GetTerminal(buffer_id)) {
        const std::string &live_title = (sess->vterm && !sess->vterm->Title().empty()) ? sess->vterm->Title() : sess->title;
        std::string label = "[Terminal] " + live_title;
        if (sess->exited) label += " [exited: " + std::to_string(sess->exit_code) + "]";
        return label;
    }
    const Buffer &buf = buffers_[buffer_id];
    std::string label = buf.filename.empty() ? "[No Name]" : buf.filename;
    if (buf.modified) label += " [+]";
    return label;
}

void Editor::SwitchToBufferForLua(int buffer_id) {
    if (buffer_id < 0 || buffer_id >= static_cast<int>(buffers_.size())) return;
    CurPane().buffer_id = buffer_id;
    ClampCursor();
    SyncModeToActivePaneBuffer();
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

void Editor::RegisterLuaMapping(Mode mode, const std::string &key, int lua_ref, const std::string &description) {
    char mode_char = 'n';
    if (mode == Mode::Normal) {
        normal_mappings_[key] = lua_ref;
    } else if (mode == Mode::Visual || mode == Mode::VisualLine) {
        visual_mappings_[key] = lua_ref;
        mode_char = 'v';
    } else {
        return;
    }
    if (!description.empty()) mapping_descriptions_[std::string(1, mode_char) + ":" + key] = description;
}

std::vector<Editor::MappingDescription> Editor::AllMappingDescriptions() const {
    std::vector<MappingDescription> out;
    out.reserve(mapping_descriptions_.size());
    for (const auto &kv : mapping_descriptions_) {
        // kv.first is "<mode-char>:<key>" -- split back apart.
        out.push_back({kv.first[0], kv.first.substr(2), kv.second});
    }
    return out;
}

// --- File I/O ------------------------------------------------------------

bool Editor::SaveBuffer(Buffer &buf, const std::string &path) {
    if (path.empty()) {
        status_message_ = "E32: No file name";
        return false;
    }
    // `buf` is always a reference to an element of buffers_ (both callers
    // pass one) -- pointer arithmetic recovers its buffer_id to check
    // images_/pdfs_ without threading an id through every SaveBuffer call
    // site. An image/PDF buffer's Buffer::lines is a dummy single empty
    // line (see OpenImageInPlace/OpenPdfInPlace); writing it out would
    // silently replace the real file with that instead -- confirmed this
    // was missing for PDF specifically (only the image guard existed),
    // meaning `:w` on a focused PDF pane was overwriting the real PDF file
    // on disk with a blank line.
    int buffer_id = static_cast<int>(&buf - buffers_.data());
    if (IsImageBuffer(buffer_id)) {
        status_message_ = "E382: Cannot write, image buffer";
        return false;
    }
    if (IsPdfBuffer(buffer_id)) {
        status_message_ = "E382: Cannot write, PDF buffer";
        return false;
    }
    if (IsSheetBuffer(buffer_id)) {
        auto it = sheetdocs_.find(buffer_id);
        if (it == sheetdocs_.end()) {
            status_message_ = "E382: Cannot write, spreadsheet buffer";
            return false;
        }
        SheetSession &sess = it->second;
        if (sess.wb.source_format == "csv") {
            // Plain text, same as the main-buffer save path below -- works
            // natively and under wasm alike (no binary-write bridge needed).
            std::string content;
            std::string err;
            if (!SaveCsvToMemory(sess.wb, content, err)) {
                status_message_ = "E212: Can't write \"" + path + "\": " + err;
                return false;
            }
#if defined(__EMSCRIPTEN__)
            char *result = mep_js_write_file(path.c_str(), content.c_str());
            std::string res(result);
            std::free(result);
            if (res != "OK") {
                status_message_ = "E212: Can't open file for writing" +
                                   (res.rfind("ERR\n", 0) == 0 ? " (" + res.substr(4) + ")" : "");
                return false;
            }
#else
            std::ofstream csv_out(path, std::ios::binary);
            if (!csv_out) {
                status_message_ = "E212: Can't open file for writing";
                return false;
            }
            csv_out << content;
            csv_out.close();
#endif
            buf.filename = path;
            buf.modified = false;
            sess.modified = false;
            save_epoch_++;
            status_message_ = "\"" + path + "\" written";
            return true;
        }
#if defined(__EMSCRIPTEN__)
        // Native-only for v1 -- same wasm binary-write-bridge blocker as
        // the Office pane (mep_js_write_file is string-only).
        status_message_ = "E382: Spreadsheet documents aren't supported in the web build";
        return false;
#else
        std::vector<unsigned char> out_bytes;
        std::string err;
        bool ok = false;
        if (sess.wb.source_format == "xlsx") {
            ok = SaveXlsxToMemory(sess.wb, sess.original_bytes, out_bytes, err);
        } else if (sess.wb.source_format == "ods") {
            ok = SaveOdsToMemory(sess.wb, sess.original_bytes, out_bytes, err);
        } else {
            err = "unknown spreadsheet format";
        }
        if (!ok) {
            status_message_ = "E212: Can't write \"" + path + "\": " + err;
            return false;
        }
        std::ofstream sheet_out(path, std::ios::binary);
        if (!sheet_out) {
            status_message_ = "E212: Can't open file for writing";
            return false;
        }
        sheet_out.write(reinterpret_cast<const char *>(out_bytes.data()), static_cast<std::streamsize>(out_bytes.size()));
        sheet_out.close();
        // Re-baselines original_bytes to what was just written, mirroring
        // OfficeSession's own save path -- a later save in the same
        // session copies untouched zip parts from the latest saved
        // structure, not the file's state from open time.
        sess.original_bytes = std::move(out_bytes);
        buf.filename = path;
        buf.modified = false;
        sess.modified = false;
        save_epoch_++;
        status_message_ = "\"" + path + "\" written";
        return true;
#endif
    }
    if (IsOfficeBuffer(buffer_id)) {
#if defined(__EMSCRIPTEN__)
        // Native-only for v1 -- the wasm file-bridge has no binary-write
        // path (mep_js_write_file is string-only), unlike the plain-text
        // save below; see NVIM_PARITY_PLAN.md's Office phase.
        status_message_ = "E382: Office documents aren't supported in the web build";
        return false;
#else
        auto it = officedocs_.find(buffer_id);
        if (it == officedocs_.end()) {
            status_message_ = "E382: Cannot write, office buffer";
            return false;
        }
        OfficeSession &sess = it->second;
        std::vector<unsigned char> out_bytes;
        std::string err;
        bool ok = false;
        if (sess.doc.source_format == "docx") {
            ok = SaveDocxToMemory(sess.doc, sess.original_bytes, out_bytes, err);
        } else if (sess.doc.source_format == "odt") {
            ok = SaveOdtToMemory(sess.doc, sess.original_bytes, out_bytes, err);
        } else {
            err = "unknown office document format";
        }
        if (!ok) {
            status_message_ = "E212: Can't write \"" + path + "\": " + err;
            return false;
        }
        std::ofstream office_out(path, std::ios::binary);
        if (!office_out) {
            status_message_ = "E212: Can't open file for writing";
            return false;
        }
        office_out.write(reinterpret_cast<const char *>(out_bytes.data()), static_cast<std::streamsize>(out_bytes.size()));
        office_out.close();
        // Re-baselines original_bytes to what was just written so a later
        // save in the same session copies untouched ZIP entries from the
        // latest saved structure, not the file's state from open time.
        sess.original_bytes = std::move(out_bytes);
        buf.filename = path;
        buf.modified = false;
        sess.modified = false;
        save_epoch_++;
        status_message_ = "\"" + path + "\" written";
        return true;
#endif
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

void Editor::LoadFile(const std::string &path, bool force_text) {
    if (path.empty()) {
        status_message_ = "E32: No file name";
        return;
    }
    if (IsImagePath(path)) {
#if defined(__EMSCRIPTEN__)
        char *result = mep_js_read_file_binary(path.c_str());
        std::string res(result);
        std::free(result);
        if (res.rfind("OK\n", 0) == 0) {
            std::vector<unsigned char> bytes = Base64Decode(res.substr(3));
            OpenImageInPlace(path, bytes.data(), bytes.size());
        } else {
            status_message_ = "E212: Can't open \"" + path + "\"" +
                               (res.rfind("ERR\n", 0) == 0 ? " (" + res.substr(4) + ")" : "");
        }
#else
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            status_message_ = "E484: Can't open file \"" + path + "\"";
        } else {
            std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            OpenImageInPlace(path, bytes.data(), bytes.size());
        }
#endif
        SyncModeToActivePaneBuffer();
        return;
    }
    if (IsPdfPath(path)) {
#if defined(__EMSCRIPTEN__)
        char *result = mep_js_read_file_binary(path.c_str());
        std::string res(result);
        std::free(result);
        if (res.rfind("OK\n", 0) == 0) {
            std::vector<unsigned char> bytes = Base64Decode(res.substr(3));
            OpenPdfInPlace(path, bytes.data(), bytes.size());
        } else {
            status_message_ = "E212: Can't open \"" + path + "\"" +
                               (res.rfind("ERR\n", 0) == 0 ? " (" + res.substr(4) + ")" : "");
        }
#else
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            status_message_ = "E484: Can't open file \"" + path + "\"";
        } else {
            std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            OpenPdfInPlace(path, bytes.data(), bytes.size());
        }
#endif
        SyncModeToActivePaneBuffer();
        return;
    }
    if (IsDocxPath(path) || IsOdtPath(path)) {
#if defined(__EMSCRIPTEN__)
        char *result = mep_js_read_file_binary(path.c_str());
        std::string res(result);
        std::free(result);
        if (res.rfind("OK\n", 0) == 0) {
            std::vector<unsigned char> bytes = Base64Decode(res.substr(3));
            OpenOfficeInPlace(path, bytes.data(), bytes.size());
        } else {
            status_message_ = "E212: Can't open \"" + path + "\"" +
                               (res.rfind("ERR\n", 0) == 0 ? " (" + res.substr(4) + ")" : "");
        }
#else
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            status_message_ = "E484: Can't open file \"" + path + "\"";
        } else {
            std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            OpenOfficeInPlace(path, bytes.data(), bytes.size());
        }
#endif
        SyncModeToActivePaneBuffer();
        return;
    }
    if (IsHtmlPath(path)) {
        // Mirrors the IsPdfPath/IsImagePath branches above -- opening a
        // .html/.htm file renders it in the same viewer :Browse uses
        // (OpenHtmlInPlace) instead of showing raw markup as plain text,
        // UNLESS force_text (the literal `:e`/`:edit` ex-command's own
        // escape hatch -- RunCommand; mep.open, what every picker/
        // sidebar/LSP jump uses instead, always passes force_text=false).
        // `origin`/`source` are both just `path` here (no remote-fetch
        // indirection -- that only applies to mep.browse_command's own
        // curl-to-tempfile path, kBuiltinTextTools).
        int existing_id = -1;
        for (size_t i = 0; i < buffers_.size(); i++) {
            if (buffers_[i].filename == path) {
                existing_id = static_cast<int>(i);
                break;
            }
        }
        if (existing_id >= 0) {
            // Already have a buffer for this exact path, in one view or
            // the other -- convert it in place if the requested view
            // doesn't match what it already is (both Convert* helpers
            // are no-ops when it's already the requested view) rather
            // than creating a second buffer with the same filename,
            // which FindOrCreateBuffer's dedup (by Buffer::filename
            // alone) couldn't tell apart from this one afterward.
            if (force_text) ConvertHtmlBufferToText(existing_id);
            else ConvertTextBufferToHtml(existing_id);
            CurPane().buffer_id = existing_id;
            CurPane().cursor = {0, 0};
            CurPane().scroll_row = 0;
            status_message_.clear();
            SyncModeToActivePaneBuffer();
            return;
        }
        if (!force_text) {
#if defined(__EMSCRIPTEN__)
            char *result = mep_js_read_file_binary(path.c_str());
            std::string res(result);
            std::free(result);
            if (res.rfind("OK\n", 0) == 0) {
                std::vector<unsigned char> bytes = Base64Decode(res.substr(3));
                OpenHtmlInPlace(path, path, bytes.data(), bytes.size());
            } else {
                status_message_ = "E212: Can't open \"" + path + "\"" +
                                   (res.rfind("ERR\n", 0) == 0 ? " (" + res.substr(4) + ")" : "");
            }
#else
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                status_message_ = "E484: Can't open file \"" + path + "\"";
            } else {
                std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                OpenHtmlInPlace(path, path, bytes.data(), bytes.size());
            }
#endif
            SyncModeToActivePaneBuffer();
            return;
        }
        // force_text with no existing buffer for this path yet: fall
        // through to the ordinary plain-text open below (FindOrCreateBuffer
        // reads it fresh, same as any other file).
    }
    if (IsCsvPath(path) || IsXlsxPath(path) || IsOdsPath(path)) {
#if defined(__EMSCRIPTEN__)
        char *result = mep_js_read_file_binary(path.c_str());
        std::string res(result);
        std::free(result);
        if (res.rfind("OK\n", 0) == 0) {
            std::vector<unsigned char> bytes = Base64Decode(res.substr(3));
            OpenSheetInPlace(path, bytes.data(), bytes.size());
        } else {
            status_message_ = "E212: Can't open \"" + path + "\"" +
                               (res.rfind("ERR\n", 0) == 0 ? " (" + res.substr(4) + ")" : "");
        }
#else
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            status_message_ = "E484: Can't open file \"" + path + "\"";
        } else {
            std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            OpenSheetInPlace(path, bytes.data(), bytes.size());
        }
#endif
        SyncModeToActivePaneBuffer();
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
    SyncModeToActivePaneBuffer();
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
