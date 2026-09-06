#include "editor.h"
#include "agent_rpc.h"
#include "lua_env.h"
#include "job.h"
#include "regex.h"
#include "vterm.h"
#include "image_doc.h"
#include "js_engine.h"
#include "pdf_doc.h"
#include "treesitter.h"
#include "workspace_git.h"

#include <algorithm>
#include <cctype>
#include <random>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <sstream>
#include <system_error>
#include <unordered_set>

#include "raylib.h"
#include "json.h"
#include "persist.h"
#include "collab_session.h"

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
/**
 * @brief Reads a text file's contents via the launcher's loopback HTTP bridge (wasm build only).
 * @param path_ptr UTF-8 path to read, as seen by the Deno-hosted launcher process.
 * @return A malloc'd C string: "OK\n<content>" on success, or "ERR\n<message>" on failure; caller must free().
 */
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
/**
 * @brief Reads a binary file (e.g. an image) via the launcher bridge's base64-safe endpoint (wasm build only).
 * @param path_ptr UTF-8 path to read, as seen by the Deno-hosted launcher process.
 * @return A malloc'd C string: "OK\n<base64 payload>" on success, or "ERR\n<message>" on failure; caller must free().
 */
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

/**
 * @brief Writes a text file's contents via the launcher's loopback HTTP bridge (wasm build only).
 * @param path_ptr UTF-8 path to write, as seen by the Deno-hosted launcher process.
 * @param content_ptr UTF-8 content to write to the file.
 * @return A malloc'd C string: "OK" on success, or "ERR\n<message>" on failure; caller must free().
 */
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
/**
 * @brief Lists a directory's entries via the launcher bridge, for command-line path completion (wasm build only).
 * @param path_ptr UTF-8 directory path to list, as seen by the Deno-hosted launcher process.
 * @return A malloc'd C string: "OK\n<JSON array of {name, is_dir}>" on success, or "ERR\n<message>" on failure; caller must free().
 */
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
/**
 * @brief Fetches the persisted project-bookmark list via the launcher bridge (wasm build only).
 * @return A malloc'd C string: "OK\n<JSON array of path strings>" on success, or "ERR\n<message>" on failure; caller must free().
 */
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
/**
 * @brief Adds or removes a project bookmark via the launcher bridge and returns the list post-mutation (wasm build only).
 * @param action_ptr The mutation to perform: "add" or "remove".
 * @param path_ptr UTF-8 project path to add or remove.
 * @return A malloc'd C string: "OK\n<JSON array of path strings>" on success, or "ERR\n<message>" on failure; caller must free().
 */
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
/**
 * @brief Opens a WebSocket to the launcher's `/pty` endpoint and sends the spawn request, without waiting for readiness (wasm build only).
 * @param payload_json_ptr UTF-8 JSON spawn-request payload to send once the socket opens.
 * @return The new PTY slot id (poll with mep_js_pty_connect_status), or -1 if the socket could not be created.
 */
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

/**
 * @brief Polls whether a PTY connection started by mep_js_pty_connect_start has become ready or failed (wasm build only).
 * @param id The PTY slot id returned by mep_js_pty_connect_start.
 * @return 0 while still connecting, 1 once ready, or -1 if the slot is unknown or the connection failed.
 */
EM_JS(int, mep_js_pty_connect_status, (int id), {
    const state = window.__mepPtys && window.__mepPtys[id];
    if (!state) return -1;
    if (state.connectStatus === -1) delete window.__mepPtys[id];
    return state.connectStatus;
});

// Set by mep_js_pty_connect whenever it resolves to -1 -- read once, right
// after, for a status message more useful than a generic "failed."
/**
 * @brief Retrieves the error message from the most recent failed PTY connect attempt (wasm build only).
 * @return A malloc'd C string with the error message (or "unknown error" if none was recorded); caller must free().
 */
EM_JS(char *, mep_js_pty_last_error, (), {
    const text = window.__mepLastPtyError || "unknown error";
    const len = lengthBytesUTF8(text) + 1;
    const ptr = _malloc(len);
    stringToUTF8(text, ptr, len);
    return ptr;
});

/**
 * @brief Sends raw bytes to a connected PTY's WebSocket; a no-op if the socket isn't open yet (wasm build only).
 * @param id The PTY slot id.
 * @param bytes_ptr Pointer to the raw bytes to send.
 * @param len Number of bytes to send.
 */
EM_JS(void, mep_js_pty_write, (int id, const char *bytes_ptr, int len), {
    const state = window.__mepPtys && window.__mepPtys[id];
    if (!state || state.ws.readyState !== WebSocket.OPEN) return;
    state.ws.send(HEAPU8.slice(bytes_ptr, bytes_ptr + len));
});

/**
 * @brief Sends a terminal resize message to a connected PTY's WebSocket; a no-op if the socket isn't open yet (wasm build only).
 * @param id The PTY slot id.
 * @param cols The new column count.
 * @param rows The new row count.
 */
EM_JS(void, mep_js_pty_resize, (int id, int cols, int rows), {
    const state = window.__mepPtys && window.__mepPtys[id];
    if (!state || state.ws.readyState !== WebSocket.OPEN) return;
    state.ws.send(JSON.stringify({ type: "resize", cols, rows }));
});

/**
 * @brief Closes a PTY's WebSocket and discards its tracked state (wasm build only).
 * @param id The PTY slot id to close.
 */
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
/**
 * @brief Drains and merges any raw output bytes a PTY has received since the last poll (wasm build only).
 * @param id The PTY slot id to poll.
 * @return A malloc'd raw byte buffer (not a null-terminated string; length is read separately via mep_js_pty_poll_len), or 0 if nothing has arrived; caller must free() a non-zero result.
 */
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

/**
 * @brief Returns the byte length of the buffer most recently returned by mep_js_pty_poll (wasm build only).
 * @return The number of bytes in the last poll's buffer, or 0 if none.
 */
EM_JS(int, mep_js_pty_poll_len, (), { return window.__mepLastPtyPollLen || 0; });

/**
 * @brief Checks whether a PTY's process has exited (or its slot no longer exists) (wasm build only).
 * @param id The PTY slot id to check.
 * @return 1 if the process has exited or the slot is unknown, 0 if it is still running.
 */
EM_JS(int, mep_js_pty_exited, (int id), {
    const state = window.__mepPtys && window.__mepPtys[id];
    return !state || state.exited ? 1 : 0;
});

/**
 * @brief Retrieves the exit code of a PTY's process, if it has exited (wasm build only).
 * @param id The PTY slot id to check.
 * @return The process exit code, or -1 if the slot is unknown or the process hasn't exited.
 */
EM_JS(int, mep_js_pty_exit_code, (int id), {
    const state = window.__mepPtys && window.__mepPtys[id];
    return state ? state.exitCode : -1;
});

// System clipboard bridge for the wasm build (Editor::SystemClipboardRead/
// Write). raylib's own web-platform GetClipboardText() is a stub, and
// navigator.clipboard.readText() is async (and permission-gated) -- so,
// same as the file bridge above, nothing here awaits (no Asyncify).
// Instead the JS side keeps a cache, window.__mepClipboardText, fed by:
//   - every write mep itself makes (so a yank/paste round-trip inside mep
//     always works, even where the browser refuses clipboard access);
//   - the document's "paste" event (the browser hands over the real
//     clipboard text on a Ctrl-V/Ctrl-Shift-V keypress without any
//     permission prompt -- the one reliable read path in a webview);
//   - a readText() kicked off on window focus and on every read, whose
//     result lands in the cache for the *next* read (best-effort: it
//     depends on the page being a secure context and the user granting
//     clipboard-read, neither of which a file:// or loopback webview is
//     guaranteed to have).
// Writes try navigator.clipboard.writeText() first (needs a recent user
// activation -- a keypress within the last few seconds counts, which a
// yank always is) and fall back to the hidden-textarea execCommand("copy")
// trick the org HTML export's copy button (main.cpp) already uses.
/**
 * @brief Writes text to the system clipboard and the JS-side cache (wasm build only).
 * @param text_ptr UTF-8 text to copy.
 */
EM_JS(void, mep_js_clipboard_write, (const char *text_ptr), {
    const text = UTF8ToString(text_ptr);
    window.__mepClipboardText = text;
    try {
        if (navigator.clipboard && navigator.clipboard.writeText) {
            navigator.clipboard.writeText(text).catch(function() {});
            return;
        }
    } catch (e) {}
    try {
        const ta = document.createElement("textarea");
        ta.value = text;
        ta.style.position = "fixed";
        ta.style.opacity = "0";
        document.body.appendChild(ta);
        ta.select();
        document.execCommand("copy");
        document.body.removeChild(ta);
    } catch (e) {}
});

/**
 * @brief Returns the cached system clipboard text, installing the paste/focus listeners that keep it fresh on first call (wasm build only).
 * @return A malloc'd C string with the cached clipboard text ("" if none); caller must free().
 */
EM_JS(char *, mep_js_clipboard_read, (), {
    if (!window.__mepClipboardRefresh) {
        window.__mepClipboardRefresh = function() {
            try {
                if (navigator.clipboard && navigator.clipboard.readText) {
                    navigator.clipboard.readText().then(function(t) {
                        if (t) window.__mepClipboardText = t;
                    }, function() {});
                }
            } catch (e) {}
        };
        document.addEventListener("paste", function(ev) {
            try {
                const t = ev.clipboardData && ev.clipboardData.getData("text/plain");
                if (t) window.__mepClipboardText = t;
            } catch (e) {}
        });
        window.addEventListener("focus", window.__mepClipboardRefresh);
    }
    window.__mepClipboardRefresh();
    const text = window.__mepClipboardText || "";
    const len = lengthBytesUTF8(text) + 1;
    const ptr = _malloc(len);
    stringToUTF8(text, ptr, len);
    return ptr;
});
#endif

namespace {

// Vim's word-class model: a "word" motion (w/b/e) stops at the boundary
// between any two of these three classes, so "foo.bar" is three words
// ("foo", ".", "bar") -- unlike a WORD motion (W/B/E), which only cares
// about whitespace vs. non-whitespace.
enum class CharClass { Space, Word, Punct };

/**
 * @brief Classifies a character into vim's word-motion character class (whitespace, word, or punctuation).
 * @param c The character to classify.
 * @return The CharClass the character belongs to.
 */
CharClass ClassOf(char c) {
    if (std::isspace(static_cast<unsigned char>(c))) return CharClass::Space;
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') return CharClass::Word;
    return CharClass::Punct;
}

// Splits file content into lines the same way std::getline does: a
// trailing newline does not produce a spurious empty final line, and a
// trailing '\r' (CRLF) is stripped from each line.
/**
 * @brief Splits text into lines, stripping a trailing CRLF's '\r' and avoiding a spurious empty final line.
 * @param content The raw file content to split.
 * @return The content split into lines (always at least one, even for empty input).
 */
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
    if (lines.empty()) lines.emplace_back("");
    return lines;
}

}  // namespace

// --- Theme engine (NVIM_PARITY_PLAN.md Part II Phase 9) ------------------
namespace {

/**
 * @brief Builds a ThemeColor from int channel values, clamping each to the valid [0, 255] range.
 * @param r Red channel (may be out of range).
 * @param g Green channel (may be out of range).
 * @param b Blue channel (may be out of range).
 * @param a Alpha channel (may be out of range; defaults to fully opaque).
 * @return The resulting color with all channels clamped to [0, 255].
 */
ThemeColor Clamp255(int r, int g, int b, int a = 255) {
    // Clamps a single channel value into the valid [0, 255] byte range.
    auto c = [](int v) { return static_cast<unsigned char>(std::max(0, std::min(255, v))); };
    return ThemeColor{c(r), c(g), c(b), c(a)};
}
/**
 * @brief Lightens a color by adding a fixed amount to each RGB channel.
 * @param c The color to lighten.
 * @param amount The amount to add to each RGB channel.
 * @return The lightened color.
 */
ThemeColor Lighten(ThemeColor c, int amount) { return Clamp255(c.r + amount, c.g + amount, c.b + amount, c.a); }
/**
 * @brief Darkens a color by subtracting a fixed amount from each RGB channel.
 * @param c The color to darken.
 * @param amount The amount to subtract from each RGB channel.
 * @return The darkened color.
 */
ThemeColor Darken(ThemeColor c, int amount) { return Lighten(c, -amount); }
/**
 * @brief Linearly interpolates between two colors.
 * @param a The color at t = 0.
 * @param b The color at t = 1.
 * @param t The interpolation factor.
 * @return The blended color (alpha taken from clamping defaults, not interpolated).
 */
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

/**
 * @brief Looks up a registered color palette by name.
 * @param name The palette name to search for (e.g. "gruvbox-dark").
 * @return A pointer to the matching Palette, or nullptr if no palette has that name.
 */
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
/**
 * @brief Expands a compact 10-color palette into the full set of named highlight groups used by the UI.
 * @param p The source palette to expand.
 * @return A map from highlight-group name to resolved ThemeColor.
 */
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
    // WORKSPACES_PLAN.md Phase 8: the `[project] [ws1] [ws2*]` labels. Text,
    // not glyph fills, so the inactive one needs to stay legible (a muted
    // fg) where TabInactive's near-bg tint only works for circle outlines.
    g["ProjectLabel"] = Mix(p.blue, p.fg, 0.4f);
    g["WorkspaceActive"] = p.fg;
    g["WorkspaceActiveBg"] = Mix(p.blue, p.bg, 0.55f);
    g["WorkspaceInactive"] = Mix(p.fg, p.bg, 0.5f);
    g["BorderActive"] = Mix(p.blue, p.fg, 0.3f);
    g["BorderInactive"] = p.border;
    g["CursorLine"] = Lighten(p.bg, 8);
    g["Visual"] = Mix(p.blue, p.bg, 0.35f);
    // The status bar's active-todo chip (kBuiltinActivityBar's Todo
    // sidebar clock, main.cpp's DrawFrame status line): green while a todo
    // is clocked in, red while none is -- toned toward the background so
    // StatusLineFg stays legible on top of either.
    g["TodoActive"] = Mix(p.green, p.bg, 0.3f);
    g["TodoInactive"] = Mix(p.red, p.bg, 0.3f);
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
    // Office (docx/odt WYSIWYG) pane's own chrome -- kept as its own small
    // group of names, rather than reusing e.g. TabActive/Visual for the
    // page/active-button fill the way the pane's toolbar briefly hardcoded
    // a fixed light palette, so :colorscheme still governs it like
    // everything else. "Accent" is deliberately the palette's raw blue
    // (not blended toward bg/fg like BorderActive/MenuHighlight/Visual
    // already are) so an active toolbar/format-panel control reads clearly
    // as "the accent color" against any theme; "AccentTint" is that same
    // blue blended toward the current bg for its active-state fill, so the
    // tint stays subtle in both light and dark palettes instead of a fixed
    // light-blue that would glow against a dark one.
    g["Accent"] = p.blue;
    g["AccentTint"] = Mix(p.blue, p.bg, 0.18f);
    // A dimmer foreground than Normal for secondary text (word/page count,
    // placeholder text, inert rail icons) -- Comment (p.border) already
    // fills a similar role for syntax comments, but that's tuned for
    // against-code contrast, not this UI's own chrome.
    g["MutedFg"] = Mix(p.fg, p.bg, 0.55f);
    // The page itself: lighter than the surrounding canvas/chrome in every
    // palette (light or dark) so it still reads as "a sheet of paper" set
    // against the work surface, without hardcoding an actual white.
    g["OfficePage"] = Lighten(p.bg, 14);
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

bool Editor::ThemePalette(const std::string &name, Palette *out) const {
    const Palette *p = FindPalette(name);
    if (!p) return false;
    *out = *p;
    return true;
}

std::vector<DiffHunk> MyersDiffHunks(const std::vector<std::string> &a, const std::vector<std::string> &b) {
    int n = static_cast<int>(a.size()), m = static_cast<int>(b.size());
    int max_d = n + m;
    if (max_d == 0) return {};
    // trace[d] stores the V array (x-coordinates of furthest-reaching D-paths
    // for each diagonal k) at step d, needed to walk the path back afterward.
    std::vector<std::vector<int>> trace;
    std::vector<int> v(static_cast<size_t>(2 * max_d + 1), 0);
    /**
     * @brief Converts a diagonal index k (which may be negative) into a non-negative offset into the v array.
     * @param k The diagonal index.
     * @return The corresponding index into the v array.
     */
    auto vidx = [max_d](int k) { return k + max_d; };
    int found_d = -1;
    for (int d = 0; d <= max_d; d++) {
        trace.push_back(v);
        for (int k = -d; k <= d; k += 2) {
            int x;
            if (k == -d || (k != d && v[static_cast<size_t>(vidx(k - 1))] < v[static_cast<size_t>(vidx(k + 1))])) {
                x = v[static_cast<size_t>(vidx(k + 1))];
            } else {
                x = v[static_cast<size_t>(vidx(k - 1))] + 1;
            }
            int y = x - k;
            while (x < n && y < m && a[static_cast<size_t>(x)] == b[static_cast<size_t>(y)]) {
                x++;
                y++;
            }
            v[static_cast<size_t>(vidx(k))] = x;
            if (x >= n && y >= m) {
                found_d = d;
                break;
            }
        }
        if (found_d >= 0) break;
    }

    // Walk the recorded traces backward from (n,m) to (0,0), emitting
    // per-line ops, then coalesce contiguous runs into hunks below.
    struct Op {
        char kind;  // '=' / '-' (only in a) / '+' (only in b)
    };
    std::vector<Op> ops;
    int x = n, y = m;
    for (int d = found_d; d > 0; d--) {
        const std::vector<int> &vd = trace[static_cast<size_t>(d)];
        int k = x - y;
        int prev_k = (k == -d || (k != d && vd[static_cast<size_t>(vidx(k - 1))] < vd[static_cast<size_t>(vidx(k + 1))])) ? k + 1 : k - 1;
        int prev_x = vd[static_cast<size_t>(vidx(prev_k))];
        int prev_y = prev_x - prev_k;
        while (x > prev_x && y > prev_y) {
            ops.push_back({'='});
            x--;
            y--;
        }
        if (x == prev_x) {
            ops.push_back({'+'});
            y--;
        } else {
            ops.push_back({'-'});
            x--;
        }
    }
    while (x > 0 && y > 0) {
        ops.push_back({'='});
        x--;
        y--;
    }
    std::reverse(ops.begin(), ops.end());

    std::vector<DiffHunk> hunks;
    size_t i = 0;
    int a_pos = 0, b_pos = 0;  // 0-indexed count of `a`/`b` lines consumed so far
    while (i < ops.size()) {
        if (ops[i].kind == '=') {
            a_pos++;
            b_pos++;
            i++;
            continue;
        }
        // A pure insertion has no `a` anchor of its own -- gitsigns
        // convention: report it at the line *after* which it was inserted
        // (0 if at the very top), i.e. `a_pos` (0-indexed) before the hunk.
        int old_start = a_pos, new_start = b_pos;
        int old_count = 0, new_count = 0;
        while (i < ops.size() && ops[i].kind != '=') {
            if (ops[i].kind == '-') {
                old_count++;
                a_pos++;
            } else {
                new_count++;
                b_pos++;
            }
            i++;
        }
        hunks.push_back({old_start + 1, old_count, new_start + 1, new_count});
    }
    return hunks;
}

// mep_lsp_filetype's own `'%.([%w_]+)$'` port: the last `.`-delimited
// run of [%w_] characters extending to the end of the string, or "" if
// there is none. Only the *last* dot in the string can ever satisfy this
// (any earlier dot leaves another literal '.' in its own remainder,
// which can't match `[%w_]+$`), so a plain find_last_of already gives
// the same leftmost-successful-match position Lua's :match would find.
std::string LspFiletype(const std::string &fname) {
    size_t pos = fname.find_last_of('.');
    if (pos == std::string::npos) return "";
    size_t start = pos + 1;
    size_t i = start;
    while (i < fname.size() && (std::isalnum(static_cast<unsigned char>(fname[i])) || fname[i] == '_')) i++;
    if (i != fname.size() || i == start) return "";
    return fname.substr(start);
}

// mep_lsp_abspath's own port.
std::string LspAbspath(const std::string &fname) {
    if (!fname.empty() && fname[0] == '/') return fname;
#if !defined(__EMSCRIPTEN__)
    std::error_code ec;
    std::string cwd = std::filesystem::current_path(ec).string();
    if (ec) cwd.clear();
#else
    std::string cwd;
#endif
    return cwd + "/" + fname;
}

std::string OrgRoamSlugify(const std::string &title) {
    std::string lower = title;
    for (char &c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string collapsed;
    size_t i = 0;
    while (i < lower.size()) {
        if (std::isalnum(static_cast<unsigned char>(lower[i]))) {
            collapsed += lower[i];
            i++;
        } else {
            collapsed += '-';
            while (i < lower.size() && !std::isalnum(static_cast<unsigned char>(lower[i]))) i++;
        }
    }
    size_t start = 0, end = collapsed.size();
    while (start < end && collapsed[start] == '-') start++;
    while (end > start && collapsed[end - 1] == '-') end--;
    return collapsed.substr(start, end - start);
}

std::string OrgExportHeading(const std::string &format, int level, const std::string &title) {
    if (format == "html") {
        std::string lvl = std::to_string(level);
        return "<h" + lvl + ">" + title + "</h" + lvl + ">";
    }
    if (format == "markdown") {
        return std::string(static_cast<size_t>(std::max(0, level)), '#') + " " + title;
    }
    std::string upper = title;
    for (char &c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return std::string(static_cast<size_t>(std::max(0, level - 1)) * 2, ' ') + upper;
}

std::string OrgHtmlEscape(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '&') {
            out += "&amp;";
        } else if (c == '<') {
            out += "&lt;";
        } else if (c == '>') {
            out += "&gt;";
        } else {
            out += c;
        }
    }
    return out;
}

int OrgSubtreeEndLines(const std::vector<std::string> &lines, int row, const std::vector<std::string> &todo_keywords) {
    int level = 0;
    if (row >= 1 && row <= static_cast<int>(lines.size())) {
        OrgHeadlineParse h = ParseOrgHeadline(lines[static_cast<size_t>(row - 1)], todo_keywords);
        if (h.is_headline) level = h.level;
    }
    for (int i = row + 1; i <= static_cast<int>(lines.size()); i++) {
        OrgHeadlineParse hi = ParseOrgHeadline(lines[static_cast<size_t>(i - 1)], todo_keywords);
        if (hi.is_headline && hi.level <= level) return i;
    }
    return static_cast<int>(lines.size()) + 1;
}

namespace {
// Forward declaration -- defined later in this file (OrgLatex section);
// same anonymous namespace, just needed here ahead of its definition.
size_t SkipWs(const std::string &s, size_t pos);

// kBuiltinOrgExport's own `'^%s*#%+MACRO:%s*([%w_%-]+)%s+(.*)$'` port.
/**
 * @brief Matches a `#+MACRO: name body` definition line and extracts the macro name and body.
 * @param line The line to match.
 * @param name Set to the macro name on success.
 * @param body Set to the macro body text on success.
 * @return True if the line is a macro definition, false otherwise.
 */
bool MatchMacroDef(const std::string &line, std::string *name, std::string *body) {
    size_t i = SkipWs(line, 0);
    static const std::string kTag = "#+MACRO:";
    if (line.compare(i, kTag.size(), kTag) != 0) return false;
    i = SkipWs(line, i + kTag.size());
    size_t name_start = i;
    while (i < line.size() &&
           (std::isalnum(static_cast<unsigned char>(line[i])) || line[i] == '_' || line[i] == '-')) {
        i++;
    }
    if (i == name_start) return false;
    size_t name_end = i;
    size_t ws_start = i;
    i = SkipWs(line, i);
    if (i == ws_start) return false;
    *name = line.substr(name_start, name_end - name_start);
    *body = line.substr(i);
    return true;
}
}  // namespace

std::map<std::string, std::string> OrgCollectMacros(const std::vector<std::string> &lines) {
    std::map<std::string, std::string> macros;
    for (const std::string &line : lines) {
        std::string name, body;
        if (MatchMacroDef(line, &name, &body)) macros[name] = body;
    }
    return macros;
}

namespace {
// kBuiltinOrgExport's own `mep_org_split_args` port.
/**
 * @brief Splits a comma-separated macro argument string into individual argument tokens.
 * @param s The comma-separated argument text.
 * @return The individual arguments, in order (an empty input yields a single empty argument).
 */
std::vector<std::string> OrgSplitArgs(const std::string &s) {
    std::vector<std::string> args;
    std::string with_trailer = s + ",";
    size_t start = 0;
    for (size_t i = 0; i < with_trailer.size(); i++) {
        if (with_trailer[i] == ',') {
            args.push_back(with_trailer.substr(start, i - start));
            start = i + 1;
        }
    }
    return args;
}

// kBuiltinOrgExport's own `'%$(%d+)'` port: replaces every `$N` with
// the Nth (1-indexed) element of `args`, or "" if out of range.
/**
 * @brief Replaces every `$N` placeholder in a macro body with the Nth (1-indexed) argument.
 * @param def The macro body/definition text containing `$N` placeholders.
 * @param args The argument list to substitute in ($1 is args[0], etc.).
 * @return The macro body with all `$N` references expanded (out-of-range N expands to "").
 */
std::string ExpandMacroDollarRefs(const std::string &def, const std::vector<std::string> &args) {
    std::string out;
    size_t pos = 0;
    while (pos < def.size()) {
        if (def[pos] == '$' && pos + 1 < def.size() && std::isdigit(static_cast<unsigned char>(def[pos + 1]))) {
            size_t i = pos + 1;
            while (i < def.size() && std::isdigit(static_cast<unsigned char>(def[i]))) i++;
            int n = std::stoi(def.substr(pos + 1, i - (pos + 1)));
            if (n >= 1 && n <= static_cast<int>(args.size())) out += args[static_cast<size_t>(n - 1)];
            pos = i;
        } else {
            out += def[pos];
            pos++;
        }
    }
    return out;
}
}  // namespace

std::string OrgExpandMacroLine(const std::string &line, const std::map<std::string, std::string> &macros) {
    // Pass 1: {{{name(args)}}}
    std::string pass1;
    size_t pos = 0;
    while (true) {
        size_t s = line.find("{{{", pos);
        if (s == std::string::npos) {
            pass1 += line.substr(pos);
            break;
        }
        pass1 += line.substr(pos, s - pos);
        size_t i = s + 3;
        size_t name_start = i;
        while (i < line.size() &&
               (std::isalnum(static_cast<unsigned char>(line[i])) || line[i] == '_' || line[i] == '-')) {
            i++;
        }
        bool matched = false;
        if (i > name_start && i < line.size() && line[i] == '(') {
            std::string name = line.substr(name_start, i - name_start);
            size_t arg_start = i + 1;
            size_t boundary = line.find('}', arg_start);
            if (boundary != std::string::npos && boundary >= arg_start + 1 && line[boundary - 1] == ')' &&
                boundary + 2 < line.size() && line[boundary] == '}' && line[boundary + 1] == '}' &&
                line[boundary + 2] == '}') {
                std::string argstr = line.substr(arg_start, (boundary - 1) - arg_start);
                auto it = macros.find(name);
                if (it == macros.end()) {
                    pass1 += "{{{";
                    pass1 += name;
                    pass1 += "(";
                    pass1 += argstr;
                    pass1 += ")}}}";
                } else {
                    pass1 += ExpandMacroDollarRefs(it->second, OrgSplitArgs(argstr));
                }
                pos = boundary + 3;
                matched = true;
            }
        }
        if (!matched) {
            pass1 += "{{{";
            pos = s + 3;
        }
    }
    // Pass 2: {{{name}}}
    std::string out;
    pos = 0;
    while (true) {
        size_t s = pass1.find("{{{", pos);
        if (s == std::string::npos) {
            out += pass1.substr(pos);
            break;
        }
        out += pass1.substr(pos, s - pos);
        size_t i = s + 3;
        size_t name_start = i;
        while (i < pass1.size() &&
               (std::isalnum(static_cast<unsigned char>(pass1[i])) || pass1[i] == '_' || pass1[i] == '-')) {
            i++;
        }
        if (i > name_start && i + 2 < pass1.size() && pass1[i] == '}' && pass1[i + 1] == '}' &&
            pass1[i + 2] == '}') {
            std::string name = pass1.substr(name_start, i - name_start);
            auto it = macros.find(name);
            out += (it != macros.end()) ? it->second : ("{{{" + name + "}}}");
            pos = i + 3;
        } else {
            out += "{{{";
            pos = s + 3;
        }
    }
    return out;
}

namespace {
// Forward declarations -- defined later in this file (OrgLatex/OrgBib
// sections); same anonymous namespace, just needed here ahead of their
// definitions.
bool MatchCiLiteral(const std::string &s, size_t pos, const std::string &lit);
bool IsSrcClose(const std::string &line);
std::string JoinNewline(const std::vector<std::string> &lines);

// kBuiltinOrgBabel's own `'#%+[Bb][Ee][Gg][Ii][Nn]_[Ss][Rr][Cc]'` open-
// line port. Generic (no language requirement), unlike OrgLatex's own
// IsSrcLatexOpen -- reused here plus IsSrcClose above (identical to
// what that check already needs).
/**
 * @brief Checks whether a line opens an org `#+BEGIN_SRC` block, case-insensitively and for any language.
 * @param line The line to check.
 * @return True if the line is a `#+BEGIN_SRC` opener.
 */
bool IsSrcBlockOpen(const std::string &line) {
    size_t i = SkipWs(line, 0);
    if (i + 1 >= line.size() || line[i] != '#' || line[i + 1] != '+') return false;
    i += 2;
    return MatchCiLiteral(line, i, "BEGIN_SRC");
}

// Extracts the (optional) language tag and the rest of a
// `#+BEGIN_SRC [lang] [args...]` header line as `args_str`, mirroring
// the original's own two-pattern (`'...%s+(%S+)'` /
// `'...%s+%S+%s*(.*)$'`) extraction exactly, including "no lang token
// at all" leaving both has_lang=false and args_str empty.
/**
 * @brief Parses a `#+BEGIN_SRC [lang] [args...]` header line into its language tag and trailing args.
 * @param header The full `#+BEGIN_SRC ...` header line.
 * @param has_lang Set to whether a language token was present.
 * @param lang Set to the language token (empty if has_lang is false).
 * @param args_str Set to the remaining header-args text after the language token.
 */
void ParseSrcHeader(const std::string &header, bool *has_lang, std::string *lang, std::string *args_str) {
    size_t i = SkipWs(header, 0);
    i += 2;  // "#+"
    i += 9;  // "BEGIN_SRC"
    size_t ws_start = i;
    i = SkipWs(header, i);
    if (i == ws_start || i >= header.size()) {
        *has_lang = false;
        *args_str = "";
        return;
    }
    size_t lang_start = i;
    while (i < header.size() && !std::isspace(static_cast<unsigned char>(header[i]))) i++;
    *lang = header.substr(lang_start, i - lang_start);
    *has_lang = true;
    i = SkipWs(header, i);
    *args_str = header.substr(i);
}

// kBuiltinOrgBabel's own `':KEY%s+(%S+)'` port (unanchored search,
// required whitespace, one-or-more non-whitespace captured) -- shared
// shape behind `:main`/`:tangle`/`:cache`/`:file` header-arg extraction.
/**
 * @brief Finds a `:key value` header-arg token in a babel header-args string.
 * @param s The header-args text to search.
 * @param key The key to look for (without the leading colon).
 * @param val Set to the token's value on success.
 * @return True if the key was found with a following value token, false otherwise.
 */
bool MatchHeaderArgToken(const std::string &s, const std::string &key, std::string *val) {
    std::string needle = ":" + key;
    size_t pos = 0;
    while (true) {
        size_t p = s.find(needle, pos);
        if (p == std::string::npos) return false;
        size_t i = p + needle.size();
        size_t ws_start = i;
        i = SkipWs(s, i);
        if (i > ws_start) {
            size_t tok_start = i;
            while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) i++;
            if (i > tok_start) {
                *val = s.substr(tok_start, i - tok_start);
                return true;
            }
        }
        pos = p + 1;
    }
}
}  // namespace

bool OrgBabelShouldWrapMain(const std::string &lang_key, const std::string &args_str) {
    std::string main_val;
    if (MatchHeaderArgToken(args_str, "main", &main_val)) {
        if (main_val == "yes") return true;
        if (main_val == "no") return false;
    }
    return lang_key == "php";
}

namespace {
/**
 * @brief Checks whether a string is a plain numeric literal (optional leading '-', digits, optional decimal part).
 * @param s The string to check.
 * @return True if the entire string is a numeric literal.
 */
bool IsNumericLiteral(const std::string &s) {
    size_t i = 0;
    if (i < s.size() && s[i] == '-') i++;
    size_t digit_start = i;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) i++;
    if (i == digit_start) return false;
    if (i < s.size() && s[i] == '.') {
        i++;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) i++;
    }
    return i == s.size();
}
}  // namespace

std::string OrgBabelFormatLiteral(const std::string &raw) {
    if (IsNumericLiteral(raw)) return raw;
    std::string esc;
    for (char c : raw) {
        switch (c) {
            case '\\':
                esc += "\\\\";
                break;
            case '"':
                esc += "\\\"";
                break;
            case '\n':
                esc += "\\n";
                break;
            case '\r':
                esc += "\\r";
                break;
            case '\t':
                esc += "\\t";
                break;
            default:
                esc += c;
        }
    }
    return "\"" + esc + "\"";
}

namespace {
// kBuiltinOrgBabel's own `':var%s+([%w_]+)='` port.
/**
 * @brief Finds the next `:var name=` declaration in a babel header-args string starting at a given position.
 * @param s The header-args text to search.
 * @param pos The offset to start searching from.
 * @param decl_end Set to the offset of the '=' that ends the declaration on success.
 * @param name Set to the variable name on success.
 * @return True if a `:var name=` declaration was found, false otherwise.
 */
bool FindNextVarDecl(const std::string &s, size_t pos, size_t *decl_end, std::string *name) {
    while (true) {
        size_t p = s.find(":var", pos);
        if (p == std::string::npos) return false;
        size_t i = p + 4;
        size_t ws_start = i;
        i = SkipWs(s, i);
        if (i == ws_start) {
            pos = p + 1;
            continue;
        }
        size_t name_start = i;
        while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) i++;
        if (i == name_start) {
            pos = p + 1;
            continue;
        }
        size_t name_end = i;
        if (i >= s.size() || s[i] != '=') {
            pos = p + 1;
            continue;
        }
        *name = s.substr(name_start, name_end - name_start);
        *decl_end = i;
        return true;
    }
}
}  // namespace

std::map<std::string, std::string> OrgParseVars(const std::string &args_str) {
    std::map<std::string, std::string> vars;
    size_t pos = 0;
    while (true) {
        size_t decl_end;
        std::string name;
        if (!FindNextVarDecl(args_str, pos, &decl_end, &name)) break;
        size_t vstart = decl_end + 1;
        if (vstart < args_str.size() && args_str[vstart] == '"') {
            std::string buf;
            size_t j = vstart + 1;
            while (j < args_str.size()) {
                char c = args_str[j];
                if (c == '\\' && j + 1 < args_str.size()) {
                    buf += args_str[j + 1];
                    j += 2;
                } else if (c == '"') {
                    j++;
                    break;
                } else {
                    buf += c;
                    j++;
                }
            }
            vars[name] = buf;
            pos = j;
        } else {
            size_t tok_begin = args_str.find_first_not_of(" \t\n\r\f\v", vstart);
            if (tok_begin == std::string::npos) {
                vars[name] = "";
                pos = vstart;
            } else {
                size_t tok_end = tok_begin;
                while (tok_end < args_str.size() && !std::isspace(static_cast<unsigned char>(args_str[tok_end]))) {
                    tok_end++;
                }
                vars[name] = args_str.substr(tok_begin, tok_end - tok_begin);
                pos = tok_end;
            }
        }
    }
    return vars;
}

std::set<std::string> OrgParseResults(const std::string &args_str) {
    std::set<std::string> modes;
    static const std::string kNeedle = ":results";
    size_t pos = 0;
    std::string results_str;
    while (true) {
        size_t p = args_str.find(kNeedle, pos);
        if (p == std::string::npos) break;
        size_t i = p + kNeedle.size();
        size_t ws_start = i;
        i = SkipWs(args_str, i);
        if (i == ws_start) {
            pos = p + 1;
            continue;
        }
        size_t val_start = i;
        while (i < args_str.size() && args_str[i] != ':') i++;
        results_str = args_str.substr(val_start, i - val_start);
        break;
    }
    size_t end = results_str.size();
    while (end > 0 && std::isspace(static_cast<unsigned char>(results_str[end - 1]))) end--;
    results_str.resize(end);
    size_t i = 0;
    while (i < results_str.size()) {
        while (i < results_str.size() && std::isspace(static_cast<unsigned char>(results_str[i]))) i++;
        size_t start = i;
        while (i < results_str.size() && !std::isspace(static_cast<unsigned char>(results_str[i]))) i++;
        if (i > start) modes.insert(results_str.substr(start, i - start));
    }
    return modes;
}

OrgSrcBlock Editor::OrgSrcBlockAt(int row) const {
    OrgSrcBlock result;
    const int n = Buf().LineCount();
    int start_row = 0;
    for (int i = row; i >= 1; i--) {
        if (i > n) continue;
        if (IsSrcBlockOpen(Buf().lines[static_cast<size_t>(i - 1)])) {
            start_row = i;
            break;
        }
        if (IsSrcClose(Buf().lines[static_cast<size_t>(i - 1)])) return result;
    }
    if (start_row == 0) return result;
    int end_row = 0;
    for (int i = start_row + 1; i <= n; i++) {
        if (IsSrcClose(Buf().lines[static_cast<size_t>(i - 1)])) {
            end_row = i;
            break;
        }
    }
    if (end_row == 0 || end_row < row) return result;
    const std::string &header = Buf().lines[static_cast<size_t>(start_row - 1)];
    bool has_lang = false;
    std::string lang, args_str;
    ParseSrcHeader(header, &has_lang, &lang, &args_str);
    if (has_lang) {
        // cppcheck-suppress knownEmptyContainer
        // lang is populated via the std::string* out-param ParseSrcHeader
        // writes through above (*lang = header.substr(...), guarded by the
        // same has_lang flag) -- cppcheck's value-flow doesn't trace writes
        // through pointer out-parameters across the call, so it sees this
        // as still default-constructed/empty.
        for (char &c : lang) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    std::vector<std::string> body_lines;
    for (int i = start_row + 1; i <= end_row - 1; i++) {
        if (i < 1 || i > n) continue;
        body_lines.push_back(Buf().lines[static_cast<size_t>(i - 1)]);
    }
    result.found = true;
    result.start_row = start_row;
    result.end_row = end_row;
    result.has_lang = has_lang;
    result.lang = lang;
    result.vars = OrgParseVars(args_str);
    result.has_tangle = MatchHeaderArgToken(args_str, "tangle", &result.tangle);
    result.has_cache = MatchHeaderArgToken(args_str, "cache", &result.cache);
    result.has_file = MatchHeaderArgToken(args_str, "file", &result.file);
    result.results_modes = OrgParseResults(args_str);
    result.args_str = args_str;
    result.body = JoinNewline(body_lines);
    return result;
}

std::vector<std::string> LspDiagWrap(const std::string &text, int width) {
    std::vector<std::string> out;
    std::string line;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) i++;
        size_t start = i;
        while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) i++;
        if (i == start) continue;
        std::string word = text.substr(start, i - start);
        if (line.empty()) {
            line = word;
        } else if (static_cast<int>(line.size() + 1 + word.size()) <= width) {
            line += " " + word;
        } else {
            out.push_back(line);
            line = word;
        }
    }
    if (!line.empty()) out.push_back(line);
    if (out.empty()) out.emplace_back("");
    return out;
}

std::pair<bool, std::string> Editor::LspWordAtCursor() const {
    int row = 0, col = 0;
    GetCursorForLua(&row, &col);
    row += 1;
    col += 1;
    if (row < 1 || row > Buf().LineCount()) return {false, ""};
    const std::string &line = Buf().lines[static_cast<size_t>(row - 1)];
    int n = static_cast<int>(line.size());
    int s = col, e = col;
    /**
     * @brief Checks whether the character at a 1-indexed column of `line` is a word character.
     * @param pos1 The 1-indexed column to check.
     * @return True if the column is in range and holds an alphanumeric or underscore character.
     */
    auto is_word = [&](int pos1) {
        // pos1 is 1-indexed; matches Lua's `[%w_]` class.
        if (pos1 < 1 || pos1 > n) return false;
        char c = line[static_cast<size_t>(pos1 - 1)];
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
    };
    while (s > 1 && is_word(s - 1)) s--;
    while (e <= n && is_word(e)) e++;
    if (s >= e) return {false, ""};
    return {true, line.substr(static_cast<size_t>(s - 1), static_cast<size_t>(e - s))};
}

void Editor::LspApplyTextEdit(int start_line, int start_char, int end_line, int end_char,
                               const std::string &new_text) {
    int sl = start_line + 1, sc = start_char + 1;
    int el = end_line + 1, ec = end_char + 1;
    std::vector<std::string> new_lines;
    size_t pos = 0;
    while (true) {
        size_t nl = new_text.find('\n', pos);
        if (nl == std::string::npos) {
            new_lines.push_back(new_text.substr(pos));
            break;
        }
        new_lines.push_back(new_text.substr(pos, nl - pos));
        pos = nl + 1;
    }
    std::string first_line = GetLineForLua(sl - 1);
    std::string last_line = (el == sl) ? first_line : GetLineForLua(el - 1);
    size_t prefix_len = std::min(static_cast<size_t>(sc - 1), first_line.size());
    std::string prefix = first_line.substr(0, prefix_len);
    std::string suffix;
    size_t suffix_start = static_cast<size_t>(ec - 1);
    if (suffix_start < last_line.size()) suffix = last_line.substr(suffix_start);
    new_lines.front() = prefix + new_lines.front();
    new_lines.back() = new_lines.back() + suffix;
    ReplaceLinesForLua(sl - 1, el, new_lines);
}

void Editor::LspApplyEditsCurrentBuffer(std::vector<LspTextEdit> edits) {
    // Sorts edits in reverse document order (latest line/column first) so applying them in order never
    // invalidates the positions of edits still to come.
    std::sort(edits.begin(), edits.end(), [](const LspTextEdit &a, const LspTextEdit &b) {
        if (a.start_line != b.start_line) return a.start_line > b.start_line;
        return a.start_char > b.start_char;
    });
    for (const LspTextEdit &e : edits) {
        LspApplyTextEdit(e.start_line, e.start_char, e.end_line, e.end_char, e.new_text);
    }
}

namespace {
/**
 * @brief Checks whether a character is valid within an org headline tag (alphanumeric, '_', ':', or '@').
 * @param c The character to check.
 * @return True if the character is a valid org tag character.
 */
bool IsOrgTagChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ':' || c == '@';
}

// mep_org_parse_headline's own `:([%w_:@]+):%s*$` port: does `rest` end
// (modulo trailing whitespace) with a `:tags:` block? Uses "leftmost
// colon within the trailing run of tag-class characters" as the opening
// delimiter -- matches Lua's own leftmost-then-greedy match for every
// realistic headline (a clean tags block, no stray colons earlier in the
// title); doesn't attempt to replicate full regex backtracking for a
// pathological title with other colons positioned to make that leftmost
// choice fail via an empty capture.
/**
 * @brief Extracts a trailing `:tag1:tag2:` tag block from the remainder of an org headline.
 * @param rest The headline text after the stars/TODO-keyword/priority.
 * @param tags Set to the extracted tag block text on success.
 * @param tag_block_start Set to the offset within `rest` where the tag block begins, on success.
 * @return True if a trailing tag block was found, false otherwise.
 */
bool ExtractOrgTags(const std::string &rest, std::string *tags, size_t *tag_block_start) {
    size_t end_pos = rest.size();
    while (end_pos > 0 && std::isspace(static_cast<unsigned char>(rest[end_pos - 1]))) end_pos--;
    if (end_pos == 0 || rest[end_pos - 1] != ':') return false;
    size_t run_start = end_pos - 1;
    while (run_start > 0 && IsOrgTagChar(rest[run_start - 1])) run_start--;
    size_t open = run_start;
    while (open < end_pos - 1 && rest[open] != ':') open++;
    if (open >= end_pos - 2) return false;  // no room for a non-empty capture
    *tags = rest.substr(open + 1, (end_pos - 1) - (open + 1));
    *tag_block_start = open;
    return true;
}
}  // namespace

OrgHeadlineParse ParseOrgHeadline(const std::string &line, const std::vector<std::string> &todo_keywords) {
    OrgHeadlineParse h;
    size_t i = 0;
    while (i < line.size() && line[i] == '*') i++;
    if (i == 0) return h;
    size_t stars = i;
    size_t ws_end = i;
    while (ws_end < line.size() && std::isspace(static_cast<unsigned char>(line[ws_end]))) ws_end++;
    if (ws_end == stars) return h;  // %s+ requires at least one whitespace char
    std::string rest = line.substr(ws_end);
    h.is_headline = true;
    h.level = static_cast<int>(stars);

    for (const std::string &kw : todo_keywords) {
        std::string prefix = kw + " ";
        if (rest.size() >= prefix.size() && rest.compare(0, prefix.size(), prefix) == 0) {
            h.todo = kw;
            h.has_todo = true;
            rest = rest.substr(prefix.size());
            break;
        }
    }

    if (rest.size() >= 4 && rest[0] == '[' && rest[1] == '#' &&
        std::isalpha(static_cast<unsigned char>(rest[2])) && rest[3] == ']') {
        h.priority = std::string(1, rest[2]);
        h.has_priority = true;
        size_t p = 4;
        while (p < rest.size() && std::isspace(static_cast<unsigned char>(rest[p]))) p++;
        rest = rest.substr(p);
    }

    std::string tags;
    size_t tag_block_start = 0;
    if (ExtractOrgTags(rest, &tags, &tag_block_start)) {
        h.tags = tags;
        h.has_tags = true;
        std::string title = rest.substr(0, tag_block_start);
        size_t te = title.size();
        while (te > 0 && std::isspace(static_cast<unsigned char>(title[te - 1]))) te--;
        h.title = title.substr(0, te);
    } else {
        h.title = rest;
    }
    return h;
}

int Editor::OrgCurrentHeadlineRow(int row, const std::vector<std::string> &todo_keywords) const {
    if (row <= 0) {
        int r = 0, c = 0;
        GetCursorForLua(&r, &c);
        row = r + 1;
    }
    const int n = Buf().LineCount();
    for (int i = row; i >= 1; i--) {
        if (i > n) continue;
        if (ParseOrgHeadline(Buf().lines[static_cast<size_t>(i - 1)], todo_keywords).is_headline) return i;
    }
    return 0;
}

int Editor::OrgSubtreeEnd(int row, const std::vector<std::string> &todo_keywords) const {
    const int n = Buf().LineCount();
    int level = 0;
    if (row >= 1 && row <= n) {
        OrgHeadlineParse h = ParseOrgHeadline(Buf().lines[static_cast<size_t>(row - 1)], todo_keywords);
        if (h.is_headline) level = h.level;
    }
    for (int i = row + 1; i <= n; i++) {
        OrgHeadlineParse h = ParseOrgHeadline(Buf().lines[static_cast<size_t>(i - 1)], todo_keywords);
        if (h.is_headline && h.level <= level) return i;
    }
    return n + 1;
}

namespace {
// The open-clock line matcher (kBuiltinOrgClock's own
// `'^%s*CLOCK:%s*%[[^%]]+%]%s*$'` port) now lives in org_doc.h as
// OrgMatchOpenClockLine, shared with the Todo sidebar's path-based
// clocking (OrgClockStartLines/OrgClockStopLines).

/**
 * @brief Checks whether a line is an org `:LOGBOOK:` drawer opener (ignoring surrounding whitespace).
 * @param line The line to check.
 * @return True if the line consists only of `:LOGBOOK:` (modulo whitespace).
 */
bool IsLogbookOpenLine(const std::string &line) {
    size_t i = 0;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) i++;
    static const std::string kTag = ":LOGBOOK:";
    if (line.compare(i, kTag.size(), kTag) != 0) return false;
    i += kTag.size();
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) i++;
    return i == line.size();
}

/**
 * @brief Formats the current local time as an org inactive-timestamp body (`YYYY-MM-DD Day HH:MM`).
 * @return The formatted timestamp text.
 */
std::string FormatOrgTimestampNow() {
    std::time_t now = std::time(nullptr);
    std::tm tmv{};
    localtime_r(&now, &tmv);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %a %H:%M", &tmv);
    return std::string(buf);
}

/**
 * @brief Parses exactly `count` consecutive decimal digits starting at a position.
 * @param s The string to read from.
 * @param pos The offset to start reading at.
 * @param count The exact number of digit characters required.
 * @param out Set to the parsed integer value on success.
 * @return True if `count` digit characters were present at `pos`, false otherwise.
 */
bool MatchDigits(const std::string &s, size_t pos, int count, int *out) {
    if (pos + static_cast<size_t>(count) > s.size()) return false;
    int val = 0;
    for (int k = 0; k < count; k++) {
        char c = s[pos + static_cast<size_t>(k)];
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        val = val * 10 + (c - '0');
    }
    *out = val;
    return true;
}

// kBuiltinOrgClock's own `'(%d%d%d%d)-(%d%d)-(%d%d) %a+ (%d%d):(%d%d)'`
// port (unanchored search, matching Lua's :match semantics).
/**
 * @brief Searches a string for a `YYYY-MM-DD Day HH:MM`-shaped org timestamp and parses its fields.
 * @param s The string to search (an unanchored scan, like Lua's `:match`).
 * @param y Set to the parsed year on success.
 * @param mo Set to the parsed month on success.
 * @param d Set to the parsed day on success.
 * @param hh Set to the parsed hour on success.
 * @param mm Set to the parsed minute on success.
 * @return True if a matching timestamp was found, false otherwise.
 */
bool ParseClockTimestamp(const std::string &s, int *y, int *mo, int *d, int *hh, int *mm) {
    for (size_t pos = 0; pos <= s.size(); pos++) {
        size_t i = pos;
        if (!MatchDigits(s, i, 4, y)) continue;
        i += 4;
        if (i >= s.size() || s[i] != '-') continue;
        i++;
        if (!MatchDigits(s, i, 2, mo)) continue;
        i += 2;
        if (i >= s.size() || s[i] != '-') continue;
        i++;
        if (!MatchDigits(s, i, 2, d)) continue;
        i += 2;
        if (i >= s.size() || s[i] != ' ') continue;
        i++;
        size_t wd_start = i;
        while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) i++;
        if (i == wd_start) continue;
        if (i >= s.size() || s[i] != ' ') continue;
        i++;
        if (!MatchDigits(s, i, 2, hh)) continue;
        i += 2;
        if (i >= s.size() || s[i] != ':') continue;
        i++;
        if (!MatchDigits(s, i, 2, mm)) continue;
        return true;
    }
    return false;
}

/**
 * @brief Converts calendar/time-of-day fields (in local time) into a std::time_t.
 * @param y The year (e.g. 2026).
 * @param mo The month (1-12).
 * @param d The day of month.
 * @param hh The hour (0-23).
 * @param mm The minute.
 * @return The corresponding std::time_t, with seconds set to 0 and DST auto-detected.
 */
std::time_t MakeLocalTime(int y, int mo, int d, int hh, int mm) {
    std::tm tmv{};
    tmv.tm_year = y - 1900;
    tmv.tm_mon = mo - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = hh;
    tmv.tm_min = mm;
    tmv.tm_sec = 0;
    tmv.tm_isdst = -1;
    return std::mktime(&tmv);
}

// kBuiltinOrgClock's own `'=>%s*(%d+):(%d%d)%s*$'` port: scans back from
// the end of the line for "=>  H:MM" (optional surrounding whitespace).
/**
 * @brief Parses a trailing `=> H:MM` clock-duration suffix from a closed CLOCK line.
 * @param line The line to scan (searched from the end).
 * @param total_minutes Set to the total duration in minutes on success.
 * @return True if a trailing `=> H:MM` suffix was found, false otherwise.
 */
bool MatchClockDuration(const std::string &line, int *total_minutes) {
    size_t end_pos = line.size();
    while (end_pos > 0 && std::isspace(static_cast<unsigned char>(line[end_pos - 1]))) end_pos--;
    if (end_pos < 2) return false;
    if (!std::isdigit(static_cast<unsigned char>(line[end_pos - 1])) ||
        !std::isdigit(static_cast<unsigned char>(line[end_pos - 2]))) {
        return false;
    }
    int h2 = (line[end_pos - 2] - '0') * 10 + (line[end_pos - 1] - '0');
    size_t p = end_pos - 2;
    if (p == 0 || line[p - 1] != ':') return false;
    p--;
    size_t h1_end = p;
    while (p > 0 && std::isdigit(static_cast<unsigned char>(line[p - 1]))) p--;
    if (p == h1_end) return false;
    int h1 = std::stoi(line.substr(p, h1_end - p));
    size_t q = p;
    while (q > 0 && std::isspace(static_cast<unsigned char>(line[q - 1]))) q--;
    if (q < 2 || line[q - 2] != '=' || line[q - 1] != '>') return false;
    *total_minutes = h1 * 60 + h2;
    return true;
}
}  // namespace

void Editor::OrgClockIn() {
    const int n = Buf().LineCount();
    for (int i = 1; i <= n; i++) {
        if (OrgMatchOpenClockLine(Buf().lines[static_cast<size_t>(i - 1)], nullptr)) {
            Notify("A clock is already running", NotifyLevel::Warn);
            return;
        }
    }
    std::vector<std::string> kws = lua_ ? lua_->GetOrgTodoKeywords() : std::vector<std::string>{};
    int row = OrgCurrentHeadlineRow(0, kws);
    if (row <= 0) {
        Notify("Not on a headline", NotifyLevel::Warn);
        return;
    }
    int e = OrgSubtreeEnd(row, kws);
    int logbook_start = 0;
    for (int i = row + 1; i <= e - 1; i++) {
        if (i < 1 || i > n) continue;
        if (IsLogbookOpenLine(Buf().lines[static_cast<size_t>(i - 1)])) {
            logbook_start = i;
            break;
        }
    }
    std::string entry = "  CLOCK: [" + FormatOrgTimestampNow() + "]";
    if (logbook_start == 0) {
        ReplaceLinesForLua(row, row, {"  :LOGBOOK:", entry, "  :END:"});
    } else {
        ReplaceLinesForLua(logbook_start, logbook_start, {entry});
    }
    Notify("Clock started");
}

void Editor::OrgClockOut() {
    const int n = Buf().LineCount();
    for (int i = 1; i <= n; i++) {
        std::string start_ts;
        if (!OrgMatchOpenClockLine(Buf().lines[static_cast<size_t>(i - 1)], &start_ts)) continue;
        int y = 0, mo = 0, d = 0, hh = 0, mm = 0;
        if (!ParseClockTimestamp(start_ts, &y, &mo, &d, &hh, &mm)) continue;
        std::time_t start_time = MakeLocalTime(y, mo, d, hh, mm);
        std::time_t now = std::time(nullptr);
        long long mins = static_cast<long long>(std::floor(std::difftime(now, start_time) / 60.0));
        if (mins < 0) mins = 0;
        char durbuf[32];
        std::snprintf(durbuf, sizeof(durbuf), "%lld:%02lld", mins / 60, mins % 60);
        std::string new_line = "  CLOCK: [" + start_ts + "]--[" + FormatOrgTimestampNow() + "] =>  " + durbuf;
        SetLineForLua(i - 1, new_line);
        Notify(std::string("Clock stopped: ") + durbuf);
        return;
    }
    Notify("No running clock", NotifyLevel::Warn);
}

int Editor::OrgClockMinutesInRange(int a, int b) const {
    int total = 0;
    const int n = Buf().LineCount();
    for (int i = a; i <= b; i++) {
        if (i < 1 || i > n) continue;
        int mins = 0;
        if (MatchClockDuration(Buf().lines[static_cast<size_t>(i - 1)], &mins)) total += mins;
    }
    return total;
}

std::vector<std::string> Editor::OrgClockTableItems() const {
    std::vector<std::string> items;
    std::vector<std::string> kws = lua_ ? lua_->GetOrgTodoKeywords() : std::vector<std::string>{};
    const int n = Buf().LineCount();
    for (int i = 1; i <= n; i++) {
        OrgHeadlineParse h = ParseOrgHeadline(Buf().lines[static_cast<size_t>(i - 1)], kws);
        if (!h.is_headline) continue;
        int mins = OrgClockMinutesInRange(i, OrgSubtreeEnd(i, kws) - 1);
        if (mins <= 0) continue;
        std::string indent(static_cast<size_t>(std::max(0, h.level - 1)) * 2, ' ');
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d:%02d", mins / 60, mins % 60);
        items.push_back(indent + h.title + "  " + buf);
    }
    return items;
}

namespace {
/**
 * @brief Checks whether a line is an org `:PROPERTIES:` drawer opener (ignoring surrounding whitespace).
 * @param line The line to check.
 * @return True if the line consists only of `:PROPERTIES:` (modulo whitespace).
 */
bool IsPropertiesOpenLine(const std::string &line) {
    size_t i = 0;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) i++;
    static const std::string kTag = ":PROPERTIES:";
    if (line.compare(i, kTag.size(), kTag) != 0) return false;
    i += kTag.size();
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) i++;
    return i == line.size();
}

/**
 * @brief Checks whether a line is a drawer-closing `:END:` line (ignoring surrounding whitespace).
 * @param line The line to check.
 * @return True if the line consists only of `:END:` (modulo whitespace).
 */
bool IsDrawerEndLine(const std::string &line) {
    size_t i = 0;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) i++;
    static const std::string kTag = ":END:";
    if (line.compare(i, kTag.size(), kTag) != 0) return false;
    i += kTag.size();
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) i++;
    return i == line.size();
}

// kBuiltinOrg's own `'^%s*:([%w_]+):%s*(.*)$'` port: a ":KEY: value" line
// inside a drawer. Unlike MatchDrawerKeyPrefix below, requires the value
// half too (used by org_property_get's read path).
/**
 * @brief Matches a `:KEY: value` property-drawer line and extracts both the key and value.
 * @param line The line to match.
 * @param key Set to the property key on success.
 * @param value Set to the property value on success.
 * @return True if the line matches the `:KEY: value` shape, false otherwise.
 */
bool MatchPropertyLine(const std::string &line, std::string *key, std::string *value) {
    size_t i = 0;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) i++;
    if (i >= line.size() || line[i] != ':') return false;
    i++;
    size_t key_start = i;
    while (i < line.size() && (std::isalnum(static_cast<unsigned char>(line[i])) || line[i] == '_')) i++;
    if (i == key_start) return false;
    size_t key_end = i;
    if (i >= line.size() || line[i] != ':') return false;
    i++;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) i++;
    *key = line.substr(key_start, key_end - key_start);
    *value = line.substr(i);
    return true;
}

// kBuiltinOrg's own `'^%s*:([%w_]+):'` port (no value, no end anchor) --
// org_property_set/remove's own "is this drawer line for KEY" scan.
/**
 * @brief Matches a `:KEY:` prefix at the start of a drawer line, without requiring or capturing a value.
 * @param line The line to match.
 * @param key Set to the property key on success.
 * @return True if the line begins with `:KEY:`, false otherwise.
 */
bool MatchDrawerKeyPrefix(const std::string &line, std::string *key) {
    size_t i = 0;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) i++;
    if (i >= line.size() || line[i] != ':') return false;
    i++;
    size_t key_start = i;
    while (i < line.size() && (std::isalnum(static_cast<unsigned char>(line[i])) || line[i] == '_')) i++;
    if (i == key_start) return false;
    size_t key_end = i;
    if (i >= line.size() || line[i] != ':') return false;
    *key = line.substr(key_start, key_end - key_start);
    return true;
}

/**
 * @brief Compares two strings for equality, ignoring ASCII case.
 * @param a The first string.
 * @param b The second string.
 * @return True if the strings have equal length and match character-by-character ignoring case.
 */
bool EqualsIgnoreCase(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}
}  // namespace

std::pair<bool, std::string> Editor::OrgPropertyGet(int row, const std::string &key,
                                                      const std::vector<std::string> &todo_keywords) const {
    if (row <= 0) row = OrgCurrentHeadlineRow(0, todo_keywords);
    if (row <= 0) return {false, ""};
    int e = OrgSubtreeEnd(row, todo_keywords);
    const int n = Buf().LineCount();
    bool in_drawer = false;
    for (int i = row + 1; i <= e - 1; i++) {
        if (i < 1 || i > n) continue;
        const std::string &line = Buf().lines[static_cast<size_t>(i - 1)];
        if (IsPropertiesOpenLine(line)) {
            in_drawer = true;
        } else if (IsDrawerEndLine(line)) {
            break;
        } else if (in_drawer) {
            std::string k, v;
            if (MatchPropertyLine(line, &k, &v) && EqualsIgnoreCase(k, key)) return {true, v};
        }
    }
    return {false, ""};
}

void Editor::OrgPropertySet(int row, const std::string &key, const std::string &value,
                             const std::vector<std::string> &todo_keywords) {
    if (row <= 0) row = OrgCurrentHeadlineRow(0, todo_keywords);
    if (row <= 0) return;
    int e = OrgSubtreeEnd(row, todo_keywords);
    int drawer_start = 0, drawer_end = 0;
    for (int i = row + 1; i <= e - 1; i++) {
        if (i < 1 || i > Buf().LineCount()) continue;
        const std::string &line = Buf().lines[static_cast<size_t>(i - 1)];
        if (IsPropertiesOpenLine(line)) {
            drawer_start = i;
        } else if (IsDrawerEndLine(line) && drawer_start) {
            drawer_end = i;
            break;
        }
    }
    if (drawer_start == 0) {
        ReplaceLinesForLua(row, row, {":PROPERTIES:", ":END:"});
        drawer_start = row + 1;
        drawer_end = row + 2;
    }
    for (int i = drawer_start + 1; i <= drawer_end - 1; i++) {
        if (i < 1 || i > Buf().LineCount()) continue;
        std::string k;
        if (MatchDrawerKeyPrefix(Buf().lines[static_cast<size_t>(i - 1)], &k) && EqualsIgnoreCase(k, key)) {
            std::string drawer_line = ":";
            drawer_line += key;
            drawer_line += ": ";
            drawer_line += value;
            SetLineForLua(i - 1, drawer_line);
            return;
        }
    }
    ReplaceLinesForLua(drawer_end - 1, drawer_end - 1, {":" + key + ": " + value});
}

void Editor::OrgPropertyRemove(int row, const std::string &key, const std::vector<std::string> &todo_keywords) {
    if (row <= 0) row = OrgCurrentHeadlineRow(0, todo_keywords);
    if (row <= 0) return;
    int e = OrgSubtreeEnd(row, todo_keywords);
    for (int i = row + 1; i <= e - 1; i++) {
        if (i < 1 || i > Buf().LineCount()) continue;
        std::string k;
        if (MatchDrawerKeyPrefix(Buf().lines[static_cast<size_t>(i - 1)], &k) && EqualsIgnoreCase(k, key)) {
            ReplaceLinesForLua(i - 1, i, {});
            return;
        }
    }
}

namespace {
// kBuiltinOrgDrill's own `mep_org_sm2` port: SM-2 spaced-repetition
// update (ef/reps/interval are read-modify-write in place).
/**
 * @brief Applies one SM-2 spaced-repetition update step, adjusting ease factor, repetition count, and interval in place.
 * @param ef The ease factor, read and updated in place.
 * @param reps The repetition count, read and updated in place.
 * @param interval The interval (in days), read and updated in place.
 * @param quality The recall quality grade for this review (0-5; below 3 resets the schedule).
 */
void Sm2Update(double *ef, int *reps, int *interval, int quality) {
    if (quality < 3) {
        *reps = 0;
        *interval = 1;
    } else {
        (*reps)++;
        if (*reps == 1) {
            *interval = 1;
        } else if (*reps == 2) {
            *interval = 6;
        } else {
            *interval = static_cast<int>(std::floor(*interval * (*ef) + 0.5));
        }
    }
    *ef = *ef + (0.1 - (5 - quality) * (0.08 + (5 - quality) * 0.02));
    if (*ef < 1.3) *ef = 1.3;
}

/**
 * @brief Parses an OrgPropertyGet-style (found, text) result as a double, falling back to a default.
 * @param r The (found, value-text) pair returned by a property lookup.
 * @param def The default value to use if not found or not parseable.
 * @return The parsed number, or `def` if the property was absent or not numeric.
 */
double PropertyNumberOr(const std::pair<bool, std::string> &r, double def) {
    if (!r.first) return def;
    char *end = nullptr;
    double v = std::strtod(r.second.c_str(), &end);
    return end == r.second.c_str() ? def : v;
}

/**
 * @brief Parses an OrgPropertyGet-style (found, text) result as an int, falling back to a default.
 * @param r The (found, value-text) pair returned by a property lookup.
 * @param def The default value to use if not found or not parseable.
 * @return The parsed integer, or `def` if the property was absent or not numeric.
 */
int PropertyIntOr(const std::pair<bool, std::string> &r, int def) {
    if (!r.first) return def;
    char *end = nullptr;
    long v = std::strtol(r.second.c_str(), &end, 10);
    return end == r.second.c_str() ? def : static_cast<int>(v);
}
}  // namespace

void Editor::OrgDrillGrade(int row, int quality, const std::vector<std::string> &todo_keywords) {
    double ef = PropertyNumberOr(OrgPropertyGet(row, "DRILL_EF", todo_keywords), 2.5);
    int reps = PropertyIntOr(OrgPropertyGet(row, "DRILL_REPS", todo_keywords), 0);
    int interval = PropertyIntOr(OrgPropertyGet(row, "DRILL_INTERVAL", todo_keywords), 0);
    Sm2Update(&ef, &reps, &interval, quality);
    char efbuf[32];
    std::snprintf(efbuf, sizeof(efbuf), "%.2f", ef);
    OrgPropertySet(row, "DRILL_EF", efbuf, todo_keywords);
    OrgPropertySet(row, "DRILL_REPS", std::to_string(reps), todo_keywords);
    OrgPropertySet(row, "DRILL_INTERVAL", std::to_string(interval), todo_keywords);
    std::time_t due_time = std::time(nullptr) + static_cast<std::time_t>(interval) * 86400;
    std::tm tmv{};
    localtime_r(&due_time, &tmv);
    char datebuf[16];
    std::strftime(datebuf, sizeof(datebuf), "%Y-%m-%d", &tmv);
    OrgPropertySet(row, "DRILL_DUE", datebuf, todo_keywords);
}

std::string Editor::OrgResolvePath(const std::string &path) const {
    if (!path.empty() && path[0] == '/') return path;
    if (!path.empty() && path[0] == '~') {
        const char *home = std::getenv("HOME");
        if (home) return std::string(home) + path.substr(1);
        return path;
    }
    std::string abs = LspAbspath(CurrentBuffer().filename);
    size_t slash = abs.find_last_of('/');
    if (slash == std::string::npos) return path;
    return abs.substr(0, slash) + "/" + path;
}

namespace {
/**
 * @brief Checks whether a path's extension identifies it as a supported image format (png/jpg/jpeg/bmp/gif).
 * @param path The file path to check.
 * @return True if the path has a recognized image extension.
 */
bool IsOrgImageExtension(const std::string &path) {
    size_t pos = path.find_last_of('.');
    if (pos == std::string::npos) return false;
    size_t start = pos + 1;
    size_t i = start;
    while (i < path.size() && std::isalnum(static_cast<unsigned char>(path[i]))) i++;
    if (i != path.size() || i == start) return false;
    std::string ext = path.substr(start);
    for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "bmp" || ext == "gif";
}

// kBuiltinOrgImages' own `'^([^%]]+)%]%[.+$'` port: strips a
// "][description" suffix off a `[[target][description]]` link's inner
// text, same as mep.org_link_at_cursor's own handling.
/**
 * @brief Strips a "][description" suffix off a `[[target][description]]` org link's inner text.
 * @param inner The link's inner text (between the outer `[[` and `]]`).
 * @return The target portion, with any `][description` suffix removed.
 */
std::string ExtractOrgImageLinkTarget(const std::string &inner) {
    size_t rb = inner.find(']');
    if (rb == std::string::npos || rb == 0) return inner;
    if (rb + 1 >= inner.size() || inner[rb + 1] != '[') return inner;
    if (rb + 2 >= inner.size()) return inner;
    return inner.substr(0, rb);
}
}  // namespace

void Editor::OrgImageScan() {
    ClearOrgImageRows();
    if (LspFiletype(CurrentBuffer().filename) != "org") return;
    const int n = Buf().LineCount();
    for (int i = 1; i <= n; i++) {
        const std::string &line = Buf().lines[static_cast<size_t>(i - 1)];
        size_t pos = 0;
        while (true) {
            size_t open = line.find("[[", pos);
            if (open == std::string::npos) break;
            size_t close = line.find("]]", open + 2);
            if (close == std::string::npos) break;
            std::string inner = line.substr(open + 2, close - (open + 2));
            pos = close + 2;
            std::string target = ExtractOrgImageLinkTarget(inner);
            if (target.compare(0, 5, "file:") == 0) {
                std::string rest = target.substr(5);
                size_t hash = rest.find('#');
                std::string path = hash == std::string::npos ? rest : rest.substr(0, hash);
                if (IsOrgImageExtension(path)) {
                    SetOrgImageRow(i - 1, OrgResolvePath(path));
                }
            }
        }
    }
}

namespace {
// Classic greedy-with-backtrack `*` wildcard match (single-char literals
// only -- no `?`/character classes, matching kBuiltinOrgAgenda's own
// glob subset: only `*` within the final path component). Equivalent to
// the original's own "escape everything else, turn `*` into Lua's `.*`,
// anchor with ^...$" approach, just without building an intermediate
// pattern string.
/**
 * @brief Matches a filename against a `*`-wildcard glob pattern (single-char literals, greedy with backtrack).
 * @param name The filename to test.
 * @param glob The glob pattern (only `*` is special; no `?` or character classes).
 * @return True if the whole name matches the whole pattern.
 */
bool MatchesGlob(const std::string &name, const std::string &glob) {
    size_t n = 0, g = 0, star_g = std::string::npos, star_n = 0;
    while (n < name.size()) {
        if (g < glob.size() && glob[g] == '*') {
            star_g = g++;
            star_n = n;
        } else if (g < glob.size() && glob[g] == name[n]) {
            g++;
            n++;
        } else if (star_g != std::string::npos) {
            g = star_g + 1;
            n = ++star_n;
        } else {
            return false;
        }
    }
    while (g < glob.size() && glob[g] == '*') g++;
    return g == glob.size();
}
}  // namespace

std::vector<std::string> Editor::OrgAgendaExpandGlob(const std::string &pattern) const {
    size_t slash = pattern.find_last_of('/');
    if (slash == std::string::npos) return {pattern};
    std::string dir = pattern.substr(0, slash);
    std::string filepat = pattern.substr(slash + 1);
    if (filepat.find('*') == std::string::npos) return {pattern};
    std::vector<std::string> results;
    for (const DirEntry &e : ListDirectory(dir)) {
        if (!e.is_dir && MatchesGlob(e.name, filepat)) results.push_back(dir + "/" + e.name);
    }
    return results;
}

namespace {
// kBuiltinOrgAgenda's own `'SCHEDULED:%s*<([^>]+)>'`/`'DEADLINE:%s*<([^>]+)>'`
// port (unanchored search for `tag`, then the bracketed timestamp body).
/**
 * @brief Finds a planning-line tag (e.g. `SCHEDULED:` or `DEADLINE:`) and extracts its bracketed timestamp body.
 * @param line The line to search (an unanchored search for `tag`).
 * @param tag The planning keyword to look for, including its trailing colon (e.g. "SCHEDULED:").
 * @param out Set to the timestamp text between the angle brackets on success.
 * @return True if the tag was found followed by a `<...>` timestamp, false otherwise.
 */
bool MatchPlanningTag(const std::string &line, const std::string &tag, std::string *out) {
    size_t pos = line.find(tag);
    if (pos == std::string::npos) return false;
    size_t i = pos + tag.size();
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) i++;
    if (i >= line.size() || line[i] != '<') return false;
    size_t start = i + 1;
    size_t close = line.find('>', start);
    if (close == std::string::npos || close == start) return false;
    *out = line.substr(start, close - start);
    return true;
}
}  // namespace

std::vector<Editor::OrgAgendaEntry> Editor::OrgAgendaScanLines(const std::vector<std::string> &lines,
                                                                const std::string &path,
                                                                const std::vector<std::string> &todo_keywords) const {
    std::vector<OrgAgendaEntry> entries;
    for (size_t idx = 0; idx < lines.size(); idx++) {
        OrgHeadlineParse h = ParseOrgHeadline(lines[idx], todo_keywords);
        if (!h.is_headline) continue;
        OrgAgendaEntry e;
        e.file = path;
        e.line = static_cast<int>(idx) + 1;
        e.todo = h.todo;
        e.has_todo = h.has_todo;
        e.title = h.title;
        e.tags = h.tags;
        e.has_tags = h.has_tags;
        e.priority = h.priority;
        e.has_priority = h.has_priority;
        if (idx + 1 < lines.size()) {
            const std::string &nextline = lines[idx + 1];
            std::string sched, dead;
            if (MatchPlanningTag(nextline, "SCHEDULED:", &sched)) {
                e.scheduled = sched;
                e.has_scheduled = true;
            }
            if (MatchPlanningTag(nextline, "DEADLINE:", &dead)) {
                e.deadline = dead;
                e.has_deadline = true;
            }
        }
        entries.push_back(std::move(e));
    }
    return entries;
}

namespace {
/**
 * @brief Replaces every non-overlapping literal occurrence of a substring with another string.
 * @param s The string to search and replace within (taken by value, modified and returned).
 * @param from The literal substring to search for.
 * @param to The replacement text.
 * @return The string with all occurrences of `from` replaced by `to`.
 */
std::string ReplaceAllLiteral(std::string s, const std::string &from, const std::string &to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

// Date-only sibling of FormatOrgTimestampNow (OrgClock's own helper,
// above) -- org-capture's %u/%t placeholders want no time-of-day.
/**
 * @brief Formats the current local date (no time-of-day) as an org date string (`YYYY-MM-DD Day`).
 * @return The formatted date text.
 */
std::string FormatOrgDateOnlyNow() {
    std::time_t now = std::time(nullptr);
    std::tm tmv{};
    localtime_r(&now, &tmv);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %a", &tmv);
    return std::string(buf);
}
}  // namespace

std::string Editor::OrgExpandCaptureTemplate(const std::string &tmpl) const {
    std::string out = tmpl;
    out = ReplaceAllLiteral(out, "%U", "[" + FormatOrgTimestampNow() + "]");
    out = ReplaceAllLiteral(out, "%u", "[" + FormatOrgDateOnlyNow() + "]");
    out = ReplaceAllLiteral(out, "%T", "<" + FormatOrgTimestampNow() + ">");
    out = ReplaceAllLiteral(out, "%t", "<" + FormatOrgDateOnlyNow() + ">");
    out = ReplaceAllLiteral(out, "%a", "[[file:" + CurrentBuffer().filename + "]]");
    out = ReplaceAllLiteral(out, "%%", "%");
    return out;
}

int Editor::OrgRefileMove(int target_row, const std::vector<std::string> &todo_keywords) {
    int row = OrgCurrentHeadlineRow(0, todo_keywords);
    if (row <= 0) return 0;
    int e = OrgSubtreeEnd(row, todo_keywords);
    std::vector<std::string> lines;
    for (int i = row; i <= e - 1; i++) {
        if (i < 1 || i > Buf().LineCount()) continue;
        lines.push_back(Buf().lines[static_cast<size_t>(i - 1)]);
    }
    if (lines.empty()) return 0;
    int src_level = ParseOrgHeadline(lines[0], todo_keywords).level;
    if (target_row < 1 || target_row > Buf().LineCount()) return 0;
    int target_level = ParseOrgHeadline(Buf().lines[static_cast<size_t>(target_row - 1)], todo_keywords).level;
    int target_end = OrgSubtreeEnd(target_row, todo_keywords);

    int delta = (target_level + 1) - src_level;
    std::vector<std::string> reindented;
    for (const std::string &line : lines) {
        size_t stars = 0;
        while (stars < line.size() && line[stars] == '*') stars++;
        if (stars > 0 && delta != 0) {
            int new_stars = std::max(1, static_cast<int>(stars) + delta);
            reindented.push_back(std::string(static_cast<size_t>(new_stars), '*') + line.substr(stars));
        } else {
            reindented.push_back(line);
        }
    }

    int dest_row;
    if (target_row > row) {
        ReplaceLinesForLua(target_end - 1, target_end - 1, reindented);
        ReplaceLinesForLua(row - 1, e - 1, {});
        dest_row = target_end - (e - row);
    } else {
        ReplaceLinesForLua(row - 1, e - 1, {});
        ReplaceLinesForLua(target_end - 1, target_end - 1, reindented);
        dest_row = target_end;
    }
    SetCursorForLua(dest_row - 1, 0);
    return dest_row;
}

namespace {
/**
 * @brief Trims leading and trailing whitespace from a string.
 * @param s The string to trim.
 * @return The string with leading/trailing whitespace removed.
 */
std::string LatexTrim(const std::string &s) {
    size_t start = 0, end = s.size();
    while (start < end && std::isspace(static_cast<unsigned char>(s[start]))) start++;
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) end--;
    return s.substr(start, end - start);
}

/**
 * @brief Checks whether a string is wrapped in a given open/close delimiter pair with non-empty content between.
 * @param s The string to check.
 * @param open The required prefix delimiter.
 * @param close The required suffix delimiter.
 * @return True if `s` starts with `open`, ends with `close`, and has content between them.
 */
bool LatexWrapped(const std::string &s, const std::string &open, const std::string &close) {
    if (s.size() <= open.size() + close.size()) return false;
    if (s.compare(0, open.size(), open) != 0) return false;
    return s.compare(s.size() - close.size(), close.size(), close) == 0;
}

/**
 * @brief Checks whether a literal appears case-insensitively at a given position in a string.
 * @param s The string to check within.
 * @param pos The position in `s` where the literal must start.
 * @param lit The literal text to match, case-insensitively.
 * @return True if `lit` matches `s` at `pos` ignoring case.
 */
bool MatchCiLiteral(const std::string &s, size_t pos, const std::string &lit) {
    if (pos + lit.size() > s.size()) return false;
    for (size_t k = 0; k < lit.size(); k++) {
        if (std::tolower(static_cast<unsigned char>(s[pos + k])) != std::tolower(static_cast<unsigned char>(lit[k]))) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Advances an index past any whitespace characters in a string.
 * @param s The string to scan.
 * @param pos The starting position.
 * @return The position of the first non-whitespace character at or after `pos` (or `s.size()`).
 */
size_t SkipWs(const std::string &s, size_t pos) {
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) pos++;
    return pos;
}

// kBuiltinOrgLatex's own case-insensitive `#+BEGIN_LATEX`/`#+END_LATEX`/
// `#+BEGIN_SRC latex`/`#+END_SRC` line matchers.
/**
 * @brief Checks whether a line is a standalone `#+BEGIN_LATEX` line.
 * @param line The line to check.
 * @return True if `line` is (ignoring leading/trailing whitespace) exactly `#+BEGIN_LATEX`.
 */
bool IsLatexBlockOpen(const std::string &line) {
    size_t i = SkipWs(line, 0);
    if (i + 1 >= line.size() || line[i] != '#' || line[i + 1] != '+') return false;
    i += 2;
    if (!MatchCiLiteral(line, i, "BEGIN_LATEX")) return false;
    i = SkipWs(line, i + 11);
    return i == line.size();
}

/**
 * @brief Checks whether a line is a `#+END_LATEX` line.
 * @param line The line to check.
 * @return True if `line` starts (after leading whitespace) with `#+END_LATEX`.
 */
bool IsLatexBlockClose(const std::string &line) {
    size_t i = SkipWs(line, 0);
    if (i + 1 >= line.size() || line[i] != '#' || line[i + 1] != '+') return false;
    return MatchCiLiteral(line, i + 2, "END_LATEX");
}

/**
 * @brief Checks whether a line is a `#+BEGIN_SRC latex` line.
 * @param line The line to check.
 * @return True if `line` is a `#+BEGIN_SRC` line whose language argument is `latex`.
 */
bool IsSrcLatexOpen(const std::string &line) {
    size_t i = SkipWs(line, 0);
    if (i + 1 >= line.size() || line[i] != '#' || line[i + 1] != '+') return false;
    i += 2;
    if (!MatchCiLiteral(line, i, "BEGIN_SRC")) return false;
    size_t ws_start = i + 9;
    size_t after_ws = SkipWs(line, ws_start);
    if (after_ws == ws_start) return false;
    return MatchCiLiteral(line, after_ws, "latex");
}

/**
 * @brief Checks whether a line is a `#+END_SRC` line.
 * @param line The line to check.
 * @return True if `line` starts (after leading whitespace) with `#+END_SRC`.
 */
bool IsSrcClose(const std::string &line) {
    size_t i = SkipWs(line, 0);
    if (i + 1 >= line.size() || line[i] != '#' || line[i + 1] != '+') return false;
    return MatchCiLiteral(line, i + 2, "END_SRC");
}

/**
 * @brief Joins a list of lines into a single string with `\n` separators.
 * @param lines The lines to join.
 * @return The joined text, with no trailing newline.
 */
std::string JoinNewline(const std::vector<std::string> &lines) {
    std::string out;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i > 0) out += '\n';
        out += lines[i];
    }
    return out;
}

// kBuiltinOrgLatex's own `mep_org_latex_scan_inline` port: every
// $..$/\(..\)/\[..\]/$$..$$ fragment embedded *within* a line that isn't
// wholly consumed by one (Editor::OrgLatexScanFragments' whole-line
// forms already handle that case). `row` is left unset (0); the caller
// fills it in, since this scans one already-extracted line at a time.
// Bare `$` is the only ambiguous delimiter -- see this function's own
// Lua-source comment (main.cpp, kBuiltinOrgLatex) for the disambiguation
// rule this mirrors exactly.
/**
 * @brief Scans a single line for inline LaTeX fragments (`$..$`, `$$..$$`, `\(..\)`, `\[..\]`).
 * @param line The line to scan.
 * @return The inline spans found, in left-to-right order, with `row` left unset (0).
 */
std::vector<Editor::OrgLatexInlineSpan> ScanLatexInlineSpans(const std::string &line) {
    std::vector<Editor::OrgLatexInlineSpan> spans;
    size_t n = line.size();
    size_t i = 0;
    while (i < n) {
        std::string body;
        size_t span_end = 0;  // 0-indexed position of the span's last included char

        if (i + 1 < n && line[i] == '\\' && line[i + 1] == '[') {
            size_t s = line.find("\\]", i + 2);
            if (s != std::string::npos) {
                span_end = s + 1;
                body = line.substr(i, span_end - i + 1);
            }
        } else if (i + 1 < n && line[i] == '$' && line[i + 1] == '$') {
            size_t s = line.find("$$", i + 2);
            if (s != std::string::npos) {
                span_end = s + 1;
                body = line.substr(i, span_end - i + 1);
            }
        } else if (i + 1 < n && line[i] == '\\' && line[i + 1] == '(') {
            size_t s = line.find("\\)", i + 2);
            if (s != std::string::npos) {
                span_end = s + 1;
                body = line.substr(i, span_end - i + 1);
            }
        } else if (line[i] == '$') {
            char next_char = (i + 1 < n) ? line[i + 1] : '\0';
            if (next_char != '\0' && next_char != ' ' && next_char != '$') {
                size_t search_from = i + 1;
                while (true) {
                    size_t s = line.find('$', search_from);
                    if (s == std::string::npos) break;
                    char prev_char = (s > 0) ? line[s - 1] : '\0';
                    char after_char = (s + 1 < n) ? line[s + 1] : '\0';
                    bool after_is_digit = after_char != '\0' && std::isdigit(static_cast<unsigned char>(after_char));
                    if (prev_char != ' ' && !after_is_digit) {
                        span_end = s;
                        body = line.substr(i, span_end - i + 1);
                        break;
                    }
                    search_from = s + 1;
                }
            }
        }

        if (body.size() > 2) {
            Editor::OrgLatexInlineSpan sp;
            sp.row = 0;
            sp.col_start = static_cast<int>(i) + 1;
            sp.col_end = static_cast<int>(span_end) + 2;
            sp.body = std::move(body);
            spans.push_back(std::move(sp));
            i = span_end + 1;
        } else {
            i++;
        }
    }
    return spans;
}
}  // namespace

Editor::OrgLatexScanResult Editor::OrgLatexScanFragments() const {
    OrgLatexScanResult result;
    const int n = Buf().LineCount();
    int i = 1;
    while (i <= n) {
        const std::string &line = Buf().lines[static_cast<size_t>(i - 1)];
        std::string trimmed = LatexTrim(line);
        std::string body;
        int end_row = 0;

        if (IsLatexBlockOpen(line)) {
            std::vector<std::string> lines;
            int j = i + 1;
            while (j <= n && !IsLatexBlockClose(Buf().lines[static_cast<size_t>(j - 1)])) {
                lines.push_back(Buf().lines[static_cast<size_t>(j - 1)]);
                j++;
            }
            if (j <= n) {
                body = JoinNewline(lines);
                end_row = j;
            }
        } else if (IsSrcLatexOpen(line)) {
            std::vector<std::string> lines;
            int j = i + 1;
            while (j <= n && !IsSrcClose(Buf().lines[static_cast<size_t>(j - 1)])) {
                lines.push_back(Buf().lines[static_cast<size_t>(j - 1)]);
                j++;
            }
            if (j <= n) {
                body = JoinNewline(lines);
                end_row = j;
            }
        } else if (LatexWrapped(trimmed, "\\[", "\\]")) {
            body = trimmed;
            end_row = i;
        } else if (trimmed == "\\[") {
            std::vector<std::string> lines;
            int j = i + 1;
            while (j <= n && LatexTrim(Buf().lines[static_cast<size_t>(j - 1)]) != "\\]") {
                lines.push_back(Buf().lines[static_cast<size_t>(j - 1)]);
                j++;
            }
            if (j <= n) {
                body = "\\[" + JoinNewline(lines) + "\\]";
                end_row = j;
            }
        } else if (LatexWrapped(trimmed, "$$", "$$")) {
            body = trimmed;
            end_row = i;
        } else if (trimmed == "$$") {
            std::vector<std::string> lines;
            int j = i + 1;
            while (j <= n && LatexTrim(Buf().lines[static_cast<size_t>(j - 1)]) != "$$") {
                lines.push_back(Buf().lines[static_cast<size_t>(j - 1)]);
                j++;
            }
            if (j <= n) {
                body = "$$" + JoinNewline(lines) + "$$";
                end_row = j;
            }
        } else if (LatexWrapped(trimmed, "\\(", "\\)")) {
            body = trimmed;
            end_row = i;
        } else if (trimmed.size() > 2 && trimmed.front() == '$' && trimmed.back() == '$' &&
                   trimmed[1] != '$' && trimmed[trimmed.size() - 2] != '$') {
            body = trimmed;
            end_row = i;
        }

        if (!body.empty()) {
            result.blocks.push_back({i, end_row, body});
            i = end_row + 1;
        } else {
            for (OrgLatexInlineSpan &span : ScanLatexInlineSpans(line)) {
                span.row = i;
                result.inlines.push_back(std::move(span));
            }
            i++;
        }
    }
    return result;
}

namespace {
// kBuiltinOrgBib's own `mep_org_bib_split_top_level` port: splits `s` on
// `sep` at brace-depth 0 / outside quotes only (quotes only toggle at
// brace-depth 0, since a quote inside a `{...}` group is already
// protected by the braces).
/**
 * @brief Splits a BibTeX string on a separator, but only at brace-depth 0 and outside quotes.
 * @param s The string to split.
 * @param sep The separator character.
 * @return The parts between top-level (unbraced, unquoted) occurrences of `sep`.
 */
std::vector<std::string> BibSplitTopLevel(const std::string &s, char sep) {
    std::vector<std::string> parts;
    int depth = 0;
    bool in_quotes = false;
    size_t start = 0;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '"' && depth == 0) {
            in_quotes = !in_quotes;
        } else if (c == '{' && !in_quotes) {
            depth++;
        } else if (c == '}' && !in_quotes) {
            depth--;
        } else if (c == sep && depth == 0 && !in_quotes) {
            parts.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    parts.push_back(s.substr(start));
    return parts;
}

// kBuiltinOrgBib's own `mep_org_bib_expand_value` port.
/**
 * @brief Expands a raw BibTeX field value, resolving `#`-concatenated string-macro references and stripping braces/quotes.
 * @param val The raw field value text.
 * @param strings The map of previously defined `@string` macro names (lowercased) to their expanded values.
 * @return The expanded, unwrapped value text.
 */
std::string BibExpandValue(const std::string &val, const std::map<std::string, std::string> &strings) {
    std::vector<std::string> parts = BibSplitTopLevel(val, '#');
    if (parts.size() == 1 && !val.empty() && (val[0] == '{' || val[0] == '"')) {
        if (val.size() >= 2 && val.front() == '{' && val.back() == '}') return val.substr(1, val.size() - 2);
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') return val.substr(1, val.size() - 2);
        return val;
    }
    std::string buf;
    for (const std::string &p : parts) {
        std::string t = LatexTrim(p);
        if (t.size() >= 2 && t.front() == '{' && t.back() == '}') {
            buf += t.substr(1, t.size() - 2);
        } else if (t.size() >= 2 && t.front() == '"' && t.back() == '"') {
            buf += t.substr(1, t.size() - 2);
        } else {
            std::string lower_t = t;
            for (char &c : lower_t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            auto it = strings.find(lower_t);
            buf += (it != strings.end()) ? it->second : t;
        }
    }
    return buf;
}

// kBuiltinOrgBib's own `text:gmatch('@(%a+)%s*(%b{})')` port: finds the
// next `@TYPE{balanced brace group}` occurrence at/after `pos`. Letters
// consumed by TYPE and whitespace consumed between TYPE and `{` can
// never themselves be `{`, so there's no backtracking case where a
// shorter TYPE/whitespace span would expose a `{` a longer greedy scan
// missed -- a plain greedy scan-then-check is equivalent to Lua's
// (potentially-backtracking) pattern engine here.
/**
 * @brief Finds the next `@TYPE{balanced brace group}` BibTeX entry at or after a given position.
 * @param text The full BibTeX text to search.
 * @param pos The position to begin searching from.
 * @param etype Set to the entry type text (e.g. "article") on success.
 * @param body Set to the text inside the entry's outermost balanced braces on success.
 * @param next_pos Set to the position just past the entry's closing brace on success.
 * @return True if an entry was found, false if there are no more.
 */
bool FindNextBibEntry(const std::string &text, size_t pos, std::string *etype, std::string *body, size_t *next_pos) {
    while (true) {
        size_t at = text.find('@', pos);
        if (at == std::string::npos) return false;
        size_t i = at + 1;
        size_t type_start = i;
        while (i < text.size() && std::isalpha(static_cast<unsigned char>(text[i]))) i++;
        if (i == type_start) {
            pos = at + 1;
            continue;
        }
        std::string type_str = text.substr(type_start, i - type_start);
        size_t j = SkipWs(text, i);
        if (j >= text.size() || text[j] != '{') {
            pos = at + 1;
            continue;
        }
        int depth = 0;
        size_t close = std::string::npos;
        for (size_t k = j; k < text.size(); k++) {
            if (text[k] == '{') {
                depth++;
            } else if (text[k] == '}') {
                depth--;
                if (depth == 0) {
                    close = k;
                    break;
                }
            }
        }
        if (close == std::string::npos) {
            pos = at + 1;
            continue;
        }
        *etype = type_str;
        *body = text.substr(j + 1, close - j - 1);
        *next_pos = close + 1;
        return true;
    }
}

// kBuiltinOrgBib's own `'^%s*([%w_%-]+)%s*=%s*(.-)%s*$'` port (used for
// both `@string{name = value}` and, after the field-splitter, each
// `name = value` field -- the original's field variant is a separate
// two-step match-then-trim, but nets out to the exact same result).
/**
 * @brief Matches a `name = value` assignment (used for `@string{name = value}` and BibTeX entry fields).
 * @param s The text to match against.
 * @param name Set to the parsed name on success.
 * @param val Set to the parsed (untrimmed-of-braces) value text on success.
 * @return True if `s` matched the `name = value` pattern.
 */
bool BibMatchNameEqVal(const std::string &s, std::string *name, std::string *val) {
    size_t i = SkipWs(s, 0);
    size_t name_start = i;
    while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_' || s[i] == '-')) i++;
    if (i == name_start) return false;
    *name = s.substr(name_start, i - name_start);
    i = SkipWs(s, i);
    if (i >= s.size() || s[i] != '=') return false;
    i++;
    i = SkipWs(s, i);
    size_t val_start = i;
    size_t val_end = s.size();
    while (val_end > val_start && std::isspace(static_cast<unsigned char>(s[val_end - 1]))) val_end--;
    *val = s.substr(val_start, val_end - val_start);
    return true;
}

// kBuiltinOrgBib's own `'^%s*([^,]+),(.*)$'` port.
/**
 * @brief Splits a BibTeX entry body into its citation key and the remaining field-list text.
 * @param body The entry body text (everything inside the entry's outer braces).
 * @param key_raw Set to the citation key text (up to the first comma) on success.
 * @param fieldstr Set to the remaining text after that comma on success.
 * @return True if a comma-separated key was found, false otherwise.
 */
bool BibMatchKeyAndFieldstr(const std::string &body, std::string *key_raw, std::string *fieldstr) {
    size_t i = SkipWs(body, 0);
    size_t key_start = i;
    while (i < body.size() && body[i] != ',') i++;
    if (i >= body.size() || i == key_start) return false;
    *key_raw = body.substr(key_start, i - key_start);
    *fieldstr = body.substr(i + 1);
    return true;
}

/**
 * @brief Returns a lowercased copy of a string.
 * @param s The string to lowercase.
 * @return The lowercased copy.
 */
std::string BibLower(const std::string &s) {
    std::string out = s;
    for (char &c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// kBuiltinOrgBib's own `mep_org_bib_parse` port.
/**
 * @brief Parses BibTeX source text, accumulating `@string` macros and bibliography entries.
 * @param text The BibTeX source text to parse.
 * @param strings The map of lowercased `@string` macro names to expanded values, updated in place as macros are found.
 * @param entries The list of parsed entries, appended to in place (comment/preamble/string entries are not added).
 */
void BibParseText(const std::string &text, std::map<std::string, std::string> *strings,
                   std::vector<Editor::OrgBibEntry> *entries) {
    size_t pos = 0;
    std::string etype, body;
    size_t next_pos = 0;
    while (FindNextBibEntry(text, pos, &etype, &body, &next_pos)) {
        pos = next_pos;
        std::string lower_type = BibLower(etype);
        if (lower_type == "string") {
            std::string name, val;
            if (BibMatchNameEqVal(body, &name, &val)) {
                (*strings)[BibLower(name)] = BibExpandValue(val, *strings);
            }
        } else if (lower_type != "comment" && lower_type != "preamble") {
            std::string key_raw, fieldstr;
            if (BibMatchKeyAndFieldstr(body, &key_raw, &fieldstr)) {
                Editor::OrgBibEntry entry;
                entry.type = lower_type;
                entry.key = LatexTrim(key_raw);
                for (const std::string &part : BibSplitTopLevel(fieldstr, ',')) {
                    std::string name, val;
                    if (BibMatchNameEqVal(part, &name, &val)) {
                        entry.fields[BibLower(name)] = BibExpandValue(val, *strings);
                    }
                }
                entries->push_back(std::move(entry));
            }
        }
    }
}
}  // namespace

std::vector<Editor::OrgBibEntry> Editor::OrgBibParseFiles(const std::vector<std::string> &file_texts) const {
    std::map<std::string, std::string> strings;
    std::vector<OrgBibEntry> entries;
    for (const std::string &text : file_texts) {
        BibParseText(text, &strings, &entries);
    }
    std::map<std::string, OrgBibEntry *> by_key;
    for (OrgBibEntry &e : entries) by_key[e.key] = &e;
    for (OrgBibEntry &e : entries) {
        auto fit = e.fields.find("crossref");
        if (fit == e.fields.end()) continue;
        auto pit = by_key.find(fit->second);
        if (pit == by_key.end()) pit = by_key.find(BibLower(fit->second));
        if (pit == by_key.end()) continue;
        for (const auto &kv : pit->second->fields) {
            e.fields.try_emplace(kv.first, kv.second);
        }
    }
    return entries;
}

namespace {
/**
 * @brief Checks whether a character can appear in a bare BibTeX word (alphanumeric or underscore).
 * @param c The character to check.
 * @return True if `c` is alphanumeric or `_`.
 */
bool IsBibWordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
/**
 * @brief Checks whether a character can appear in a citation key (alphanumeric, `_`, `-`, or `:`).
 * @param c The character to check.
 * @return True if `c` is a valid citation-key character.
 */
bool IsBibKeyChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ':'; }

// kBuiltinOrgBib's own `'^%s*@?(%S+)%s*$'` port.
/**
 * @brief Parses a single citation key, optionally prefixed with `@`, from an otherwise whitespace-only string.
 * @param s The text to match.
 * @param key Set to the parsed key on success.
 * @return True if `s` (trimmed) is a single `@`-optional key with no other content.
 */
bool BibMatchAtKey(const std::string &s, std::string *key) {
    size_t i = SkipWs(s, 0);
    if (i < s.size() && s[i] == '@') i++;
    size_t key_start = i;
    while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) i++;
    if (i == key_start) return false;
    size_t key_end = i;
    i = SkipWs(s, i);
    if (i != s.size()) return false;
    *key = s.substr(key_start, key_end - key_start);
    return true;
}

// kBuiltinOrgBib's own `'%[cite[%a/]-:(.-)%]'` port: `[%a/]-` is lazy,
// but since neither a letter nor `/` can ever be `:`, a greedy
// scan-then-check for the next `:` lands at the exact same position a
// (potentially-backtracking) lazy match would -- same reasoning as
// FindNextBibEntry above.
/**
 * @brief Finds the next `[cite<letters/slashes>:body]` org-cite span at or after a given position in a line.
 * @param line The line to search.
 * @param pos The position to begin searching from.
 * @param match_start Set to the index of the span's opening `[` on success.
 * @param match_end Set to the index of the span's closing `]` on success.
 * @param body Set to the text between the `:` and the closing `]` on success.
 * @return True if a span was found, false if there are no more.
 */
bool FindNextOrgCiteSpan(const std::string &line, size_t pos, size_t *match_start, size_t *match_end,
                          std::string *body) {
    static const std::string kPrefix = "[cite";
    while (true) {
        if (pos + kPrefix.size() > line.size()) return false;
        size_t s = line.find(kPrefix, pos);
        if (s == std::string::npos) return false;
        size_t i = s + kPrefix.size();
        while (i < line.size() && (std::isalpha(static_cast<unsigned char>(line[i])) || line[i] == '/')) i++;
        if (i >= line.size() || line[i] != ':') {
            pos = s + 1;
            continue;
        }
        size_t body_start = i + 1;
        size_t close = line.find(']', body_start);
        if (close == std::string::npos) {
            pos = s + 1;
            continue;
        }
        *match_start = s;
        *match_end = close;
        *body = line.substr(body_start, close - body_start);
        return true;
    }
}

// kBuiltinOrgBib's own `mep_org_bib_cite_spans` port.
/**
 * @brief Scans a line for org-cite citation spans, both the `[cite:...]` syntax and legacy `citep:`/`citet:`/etc. prefixes.
 * @param line The line to scan.
 * @return The citation spans found, each with its column range and the citation keys it references.
 */
std::vector<Editor::OrgBibCiteSpan> BibCiteSpans(const std::string &line) {
    std::vector<Editor::OrgBibCiteSpan> spans;

    size_t pos = 0;
    size_t match_start = 0, match_end = 0;
    std::string body;
    while (FindNextOrgCiteSpan(line, pos, &match_start, &match_end, &body)) {
        pos = match_end + 1;
        std::vector<std::string> keys;
        for (const std::string &part : BibSplitTopLevel(body, ';')) {
            std::string k;
            if (BibMatchAtKey(part, &k)) keys.push_back(k);
        }
        if (!keys.empty()) {
            spans.push_back({static_cast<int>(match_start) + 1, static_cast<int>(match_end) + 1, std::move(keys)});
        }
    }

    static const std::vector<std::string> kLegacyPrefixes = {"citeauthor", "citeyear", "citep", "citet", "cite"};
    size_t n = line.size();
    size_t p = 0;
    while (p < n) {
        bool matched = false;
        char prev = (p > 0) ? line[p - 1] : '\0';
        bool prev_is_word = prev != '\0' && IsBibWordChar(prev);
        if (prev != '[' && !prev_is_word) {
            for (const std::string &prefix : kLegacyPrefixes) {
                size_t plen = prefix.size();
                if (p + plen < n && line.compare(p, plen, prefix) == 0 && line[p + plen] == ':') {
                    size_t after = p + plen + 1;
                    char after_char = (after < n) ? line[after] : '\0';
                    bool after_is_key = after_char != '\0' && IsBibKeyChar(after_char);
                    if (after_char != '@' && after_is_key) {
                        size_t j = after;
                        std::vector<std::string> keys;
                        size_t key_start = after;
                        while (j < n) {
                            char c = line[j];
                            if (IsBibKeyChar(c)) {
                                j++;
                            } else if (c == ',' && j + 1 < n && IsBibKeyChar(line[j + 1])) {
                                keys.push_back(line.substr(key_start, j - key_start));
                                j++;
                                key_start = j;
                            } else {
                                break;
                            }
                        }
                        keys.push_back(line.substr(key_start, j - key_start));
                        spans.push_back({static_cast<int>(p) + 1, static_cast<int>(j), std::move(keys)});
                        p = j;
                        matched = true;
                        break;
                    }
                }
            }
        }
        if (!matched) p++;
    }

    // Order spans left-to-right by their starting column.
    std::sort(spans.begin(), spans.end(), [](const Editor::OrgBibCiteSpan &a, const Editor::OrgBibCiteSpan &b) {
        return a.col_start < b.col_start;
    });
    return spans;
}
}  // namespace

std::vector<std::string> Editor::OrgBibCiteAtCursor() const {
    int row = 0, col = 0;
    GetCursorForLua(&row, &col);
    row += 1;
    col += 1;
    if (row < 1 || row > Buf().LineCount()) return {};
    const std::string &line = Buf().lines[static_cast<size_t>(row - 1)];
    for (const OrgBibCiteSpan &span : BibCiteSpans(line)) {
        if (col >= span.col_start && col <= span.col_end) return span.keys;
    }
    return {};
}

namespace {
/**
 * @brief Checks whether a filename has a `.org` extension.
 * @param name The filename to check.
 * @return True if `name` ends with `.org`.
 */
bool HasOrgExtension(const std::string &name) {
    static const std::string kExt = ".org";
    return name.size() >= kExt.size() && name.compare(name.size() - kExt.size(), kExt.size(), kExt) == 0;
}

// kBuiltinOrgRoam's own `'^#%+[Tt][Ii][Tt][Ll][Ee]:%s*(.*)$'` port.
/**
 * @brief Matches a `#+TITLE:` keyword line and extracts its value.
 * @param line The line to check.
 * @param title Set to the title text (after `#+TITLE:` and whitespace) on success.
 * @return True if `line` is a `#+TITLE:` line.
 */
bool MatchTitleKeyword(const std::string &line, std::string *title) {
    if (line.size() < 2 || line[0] != '#' || line[1] != '+') return false;
    if (!MatchCiLiteral(line, 2, "TITLE:")) return false;
    size_t i = SkipWs(line, 8);
    *title = line.substr(i);
    return true;
}

// kBuiltinOrgRoam's own `'^%s*:ID:%s*(%S+)'` port (unanchored end).
/**
 * @brief Matches an `:ID:` property line and extracts its value.
 * @param line The line to check.
 * @param id Set to the ID value on success.
 * @return True if `line` (after leading whitespace) is a `:ID:` property with a value.
 */
bool MatchIdProperty(const std::string &line, std::string *id) {
    size_t i = SkipWs(line, 0);
    static const std::string kTag = ":ID:";
    if (line.compare(i, kTag.size(), kTag) != 0) return false;
    i = SkipWs(line, i + kTag.size());
    size_t start = i;
    while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) i++;
    if (i == start) return false;
    *id = line.substr(start, i - start);
    return true;
}

// mep_org_roam_gen_id's own port. Uses C++'s own <random> rather than
// Lua's math.random (see editor.h's own comment on OrgRoamEnsureId).
/**
 * @brief Generates a new org-roam ID from the current local timestamp plus a random 4-digit suffix.
 * @return The generated ID, formatted `YYYYMMDDHHMMSS-NNNN`.
 */
std::string GenerateRoamId() {
    std::time_t now = std::time(nullptr);
    std::tm tmv{};
    localtime_r(&now, &tmv);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d%H%M%S", &tmv);
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(1000, 9999);
    return std::string(buf) + "-" + std::to_string(dist(rng));
}

// kBuiltinOrgRoam's own `line:gmatch('%[%[id:([%w%-]+)')` port.
/**
 * @brief Extracts every `[[id:TARGET...]]` link target ID from a line.
 * @param line The line to scan.
 * @return The target IDs found, in order of appearance.
 */
std::vector<std::string> ExtractIdLinkTargets(const std::string &line) {
    std::vector<std::string> targets;
    static const std::string kPrefix = "[[id:";
    size_t pos = 0;
    while (true) {
        size_t s = line.find(kPrefix, pos);
        if (s == std::string::npos) break;
        size_t i = s + kPrefix.size();
        size_t start = i;
        while (i < line.size() && (std::isalnum(static_cast<unsigned char>(line[i])) || line[i] == '-')) i++;
        if (i > start) {
            targets.push_back(line.substr(start, i - start));
            pos = i;
        } else {
            pos = s + 1;
        }
    }
    return targets;
}
}  // namespace

std::vector<std::string> Editor::OrgRoamFilesIn(const std::vector<std::string> &dirs) const {
    std::vector<std::string> files;
    for (const std::string &dir : dirs) {
        for (const DirEntry &e : ListDirectory(dir)) {
            if (HasOrgExtension(e.name)) files.push_back(dir + "/" + e.name);
        }
    }
    return files;
}

std::pair<bool, std::string> Editor::OrgRoamTitleOf(const std::vector<std::string> &lines,
                                                      const std::vector<std::string> &todo_keywords) const {
    for (const std::string &line : lines) {
        std::string title;
        if (MatchTitleKeyword(line, &title)) return {true, title};
    }
    for (const std::string &line : lines) {
        OrgHeadlineParse h = ParseOrgHeadline(line, todo_keywords);
        if (h.is_headline) return {true, h.title};
    }
    return {false, ""};
}

std::string Editor::OrgRoamEnsureId() {
    const int n = Buf().LineCount();
    for (int i = 1; i <= n; i++) {
        if (ParseOrgHeadline(Buf().lines[static_cast<size_t>(i - 1)], {}).is_headline) break;
        std::string id;
        if (MatchIdProperty(Buf().lines[static_cast<size_t>(i - 1)], &id)) return id;
    }
    std::string id = GenerateRoamId();
    ReplaceLinesForLua(0, 0, {":PROPERTIES:", ":ID:       " + id, ":END:"});
    return id;
}

std::vector<int> Editor::OrgRoamFindBacklinkLines(const std::vector<std::string> &lines,
                                                   const std::string &target_id) const {
    std::vector<int> result;
    std::string needle = "[[id:" + target_id;
    for (size_t i = 0; i < lines.size(); i++) {
        if (lines[i].find(needle) != std::string::npos) result.push_back(static_cast<int>(i) + 1);
    }
    return result;
}

Editor::OrgRoamFileIndexEntry Editor::OrgRoamParseFileIndex(const std::vector<std::string> &lines,
                                                             const std::vector<std::string> &todo_keywords) const {
    OrgRoamFileIndexEntry result;
    for (const std::string &line : lines) {
        if (ParseOrgHeadline(line, {}).is_headline) break;
        std::string id;
        if (MatchIdProperty(line, &id)) {
            result.has_id = true;
            result.id = id;
            break;
        }
    }
    if (!result.has_id) return result;
    std::pair<bool, std::string> title_result = OrgRoamTitleOf(lines, todo_keywords);
    result.has_title = title_result.first;
    result.title = title_result.second;
    std::vector<std::string> links;
    std::unordered_set<std::string> seen;
    for (const std::string &line : lines) {
        for (const std::string &lid : ExtractIdLinkTargets(line)) {
            if (seen.insert(lid).second) links.push_back(lid);
        }
    }
    result.links = std::move(links);
    return result;
}

namespace {
// mep_org_table_row's own port. Private to this file (the original was
// a `local function` too, used only by org_table_align).
struct OrgTableRow {
    bool is_row = false;
    bool is_sep = false;
    std::vector<std::string> cells;
};

// kBuiltinOrgLinks' own `'^%s*|%-'`/inner-cell-split port.
/**
 * @brief Parses a line as an org table row: a separator row (`|-...`) or a row of `|`-delimited cells.
 * @param line The line to parse.
 * @return The parsed row; `is_row` is false if the line isn't a table row at all.
 */
OrgTableRow ParseOrgTableRowImpl(const std::string &line) {
    OrgTableRow result;
    size_t i = SkipWs(line, 0);
    if (i >= line.size() || line[i] != '|') return result;
    result.is_row = true;
    if (i + 1 < line.size() && line[i + 1] == '-') {
        result.is_sep = true;
        return result;
    }
    size_t bar_pos = i;
    size_t end_trimmed = line.size();
    while (end_trimmed > bar_pos + 1 && std::isspace(static_cast<unsigned char>(line[end_trimmed - 1]))) {
        end_trimmed--;
    }
    std::string inner;
    if (end_trimmed > bar_pos + 1 && line[end_trimmed - 1] == '|') {
        inner = line.substr(bar_pos + 1, (end_trimmed - 1) - (bar_pos + 1));
    } else {
        inner = line.substr(bar_pos + 1, end_trimmed - (bar_pos + 1));
    }
    std::string with_trailer = inner + "|";
    size_t start = 0;
    for (size_t k = 0; k < with_trailer.size(); k++) {
        if (with_trailer[k] == '|') {
            result.cells.push_back(LatexTrim(with_trailer.substr(start, k - start)));
            start = k + 1;
        }
    }
    return result;
}
}  // namespace

void Editor::OrgTableAlign() {
    int row = 0, col = 0;
    GetCursorForLua(&row, &col);
    row += 1;
    const int n = Buf().LineCount();
    if (row < 1 || row > n || !ParseOrgTableRowImpl(Buf().lines[static_cast<size_t>(row - 1)]).is_row) {
        Notify("Not on a table row", NotifyLevel::Warn);
        return;
    }
    int top = row;
    while (top > 1 && ParseOrgTableRowImpl(Buf().lines[static_cast<size_t>(top - 2)]).is_row) top--;
    int bot = row;
    while (bot < n && ParseOrgTableRowImpl(Buf().lines[static_cast<size_t>(bot)]).is_row) bot++;

    std::vector<int> widths;
    std::vector<std::pair<int, OrgTableRow>> rows;
    for (int i = top; i <= bot; i++) {
        OrgTableRow r = ParseOrgTableRowImpl(Buf().lines[static_cast<size_t>(i - 1)]);
        if (!r.is_sep) {
            for (size_t ci = 0; ci < r.cells.size(); ci++) {
                if (ci >= widths.size()) widths.push_back(0);
                widths[ci] = std::max(widths[ci], static_cast<int>(r.cells[ci].size()));
            }
        }
        rows.emplace_back(i, std::move(r));
    }
    for (const std::pair<int, OrgTableRow> &entry : rows) {
        int i = entry.first;
        const OrgTableRow &r = entry.second;
        std::string line = "|";
        if (r.is_sep) {
            for (size_t wi = 0; wi < widths.size(); wi++) {
                if (wi > 0) line += "+";
                line += std::string(static_cast<size_t>(widths[wi] + 2), '-');
            }
        } else {
            for (size_t ci = 0; ci < widths.size(); ci++) {
                if (ci > 0) line += "|";
                std::string cell = ci < r.cells.size() ? r.cells[ci] : "";
                line += " " + cell + std::string(static_cast<size_t>(widths[ci]) - cell.size(), ' ') + " ";
            }
        }
        line += "|";
        SetLineForLua(i - 1, line);
    }
}

namespace {
// kBuiltinOrgLinks' own `'^([^%]]+)%]%[(.+)$'` port: splits inner link
// text into (target, desc) if it has a `target][desc` shape, else
// leaves it as a bare target with no description. Same underlying
// shape as ExtractOrgImageLinkTarget (editor.cpp, OrgImageScan), but
// that one discards desc -- org_link_at_cursor needs it back.
/**
 * @brief Splits an org link's inner text into a target and an optional description (`target][desc` shape).
 * @param inner The link's inner text (between the outer `[[` and `]]`).
 * @param target Set to the target portion.
 * @param has_desc Set to whether a `][description` suffix was present.
 * @param desc Set to the description text if `has_desc` is set.
 */
void SplitLinkTargetDesc(const std::string &inner, std::string *target, bool *has_desc, std::string *desc) {
    size_t rb = inner.find(']');
    if (rb == std::string::npos || rb == 0 || rb + 1 >= inner.size() || inner[rb + 1] != '[' || rb + 2 >= inner.size()) {
        *target = inner;
        *has_desc = false;
        return;
    }
    *target = inner.substr(0, rb);
    *has_desc = true;
    *desc = inner.substr(rb + 2);
}
}  // namespace

Editor::OrgLinkAtCursorResult Editor::OrgLinkAtCursor() const {
    OrgLinkAtCursorResult result;
    int row = 0, col = 0;
    GetCursorForLua(&row, &col);
    row += 1;
    col += 1;
    if (row < 1 || row > Buf().LineCount()) return result;
    const std::string &line = Buf().lines[static_cast<size_t>(row - 1)];
    size_t pos = 0;
    while (true) {
        size_t open = line.find("[[", pos);
        if (open == std::string::npos) break;
        size_t close = line.find("]]", open + 2);
        if (close == std::string::npos) break;
        int s = static_cast<int>(open) + 1;
        int e = static_cast<int>(close) + 2;
        if (col >= s && col <= e) {
            std::string inner = line.substr(open + 2, close - (open + 2));
            result.found = true;
            SplitLinkTargetDesc(inner, &result.target, &result.has_desc, &result.desc);
            return result;
        }
        pos = close + 2;
    }
    return result;
}

namespace {
/**
 * @brief Checks whether a run of `count` consecutive digit characters starts at a given position.
 * @param s The string to check.
 * @param pos The starting position of the run.
 * @param count The number of digit characters required.
 * @return True if `s` has `count` digits starting at `pos`.
 */
bool IsDigitRun(const std::string &s, size_t pos, int count) {
    if (pos + static_cast<size_t>(count) > s.size()) return false;
    for (int k = 0; k < count; k++) {
        if (!std::isdigit(static_cast<unsigned char>(s[pos + static_cast<size_t>(k)]))) return false;
    }
    return true;
}

// kBuiltinOrgLinks' own `'<(%d%d%d%d%-%d%d%-%d%d[^>]-)>'`/
// `'%[(%d%d%d%d%-%d%d%-%d%d[^%]]-)%]'` port: finds the next
// `open YYYY-MM-DD ... close` span at/after `pos`.
/**
 * @brief Finds the next `open YYYY-MM-DD ... close` org timestamp span at or after a given position in a line.
 * @param line The line to search.
 * @param pos The position to begin searching from.
 * @param open The opening delimiter character (`<` for active, `[` for inactive).
 * @param close The matching closing delimiter character.
 * @param s Set to the index of the opening delimiter on success.
 * @param e Set to the index of the closing delimiter on success.
 * @param body Set to the timestamp text between the delimiters on success.
 * @return True if a valid timestamp span was found, false if there are no more.
 */
bool FindTimestampAt(const std::string &line, size_t pos, char open, char close, size_t *s, size_t *e,
                      std::string *body) {
    while (true) {
        size_t o = line.find(open, pos);
        if (o == std::string::npos) return false;
        size_t i = o + 1;
        bool ok = IsDigitRun(line, i, 4) && i + 4 < line.size() && line[i + 4] == '-' &&
                  IsDigitRun(line, i + 5, 2) && i + 7 < line.size() && line[i + 7] == '-' && IsDigitRun(line, i + 8, 2);
        if (!ok) {
            pos = o + 1;
            continue;
        }
        size_t after_date = i + 10;
        size_t close_pos = line.find(close, after_date);
        if (close_pos == std::string::npos) {
            pos = o + 1;
            continue;
        }
        *s = o;
        *e = close_pos;
        *body = line.substr(i, close_pos - i);
        return true;
    }
}

// kBuiltinOrgLinks' own `'^%d%d%d%d%-%d%d%-%d%d%s*%a*(.*)$'` port: the
// trailing text after the date and an optional weekday name (e.g. a
// repeater like " +1w").
/**
 * @brief Extracts the trailing text (e.g. a repeater like " +1w") following the date and optional weekday name in a timestamp body.
 * @param body The timestamp body text (starting with `YYYY-MM-DD`).
 * @return The text after the date and any weekday name.
 */
std::string ExtractTimestampRest(const std::string &body) {
    size_t i = SkipWs(body, 10);
    while (i < body.size() && std::isalpha(static_cast<unsigned char>(body[i]))) i++;
    return body.substr(i);
}
}  // namespace

Editor::OrgTimestampMatch Editor::OrgTimestampAt(const std::string &line, int col) const {
    OrgTimestampMatch result;
    for (int pass = 0; pass < 2; pass++) {
        char open = pass == 0 ? '<' : '[';
        char close = pass == 0 ? '>' : ']';
        size_t pos = 0;
        while (true) {
            size_t s0 = 0, e0 = 0;
            std::string body;
            if (!FindTimestampAt(line, pos, open, close, &s0, &e0, &body)) break;
            int s = static_cast<int>(s0) + 1;
            int e = static_cast<int>(e0) + 1;
            if (col >= s && col <= e) {
                result.found = true;
                result.col_start = s;
                result.col_end = e;
                result.body = body;
                result.active = pass == 0;
                return result;
            }
            pos = e0 + 1;
        }
    }
    return result;
}

void Editor::OrgTimestampInsert(bool active) {
    int row = 0, col = 0;
    GetCursorForLua(&row, &col);
    row += 1;
    col += 1;
    std::string body = FormatOrgDateOnlyNow();
    std::string ts = active ? ("<" + body + ">") : ("[" + body + "]");
    const std::string &line = Buf().lines[static_cast<size_t>(row - 1)];
    std::string new_line = line.substr(0, static_cast<size_t>(col - 1)) + ts + line.substr(static_cast<size_t>(col - 1));
    SetLineForLua(row - 1, new_line);
    SetCursorForLua(row - 1, col - 1 + static_cast<int>(ts.size()));
}

void Editor::OrgTimestampShift(int delta_days) {
    int row = 0, col = 0;
    GetCursorForLua(&row, &col);
    row += 1;
    col += 1;
    if (row < 1 || row > Buf().LineCount()) return;
    const std::string &line = Buf().lines[static_cast<size_t>(row - 1)];
    OrgTimestampMatch m = OrgTimestampAt(line, col);
    if (!m.found) {
        Notify("No timestamp under cursor", NotifyLevel::Warn);
        return;
    }
    int y = std::stoi(m.body.substr(0, 4));
    int mo = std::stoi(m.body.substr(5, 2));
    int d = std::stoi(m.body.substr(8, 2));
    std::string rest = ExtractTimestampRest(m.body);
    std::time_t t = MakeLocalTime(y, mo, d, 12, 0);
    t += static_cast<std::time_t>(delta_days) * 86400;
    std::tm tmv{};
    localtime_r(&t, &tmv);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %a", &tmv);
    std::string newbody = std::string(buf) + rest;
    char openc = m.active ? '<' : '[';
    char closec = m.active ? '>' : ']';
    std::string new_line = line.substr(0, static_cast<size_t>(m.col_start - 1)) + openc + newbody + closec + line.substr(static_cast<size_t>(m.col_end));
    SetLineForLua(row - 1, new_line);
}

namespace {
// kBuiltinOrgLinks' own `'%[fn:([%w_%-]+)%]'` port.
/**
 * @brief Checks whether a given column falls within a `[fn:name]` footnote reference on a line, extracting its name.
 * @param line The line to search.
 * @param col The 1-based column to test.
 * @param name Set to the footnote name on success.
 * @return True if `col` falls within a footnote reference.
 */
bool FindFootnoteRefAt(const std::string &line, int col, std::string *name) {
    static const std::string kPrefix = "[fn:";
    size_t pos = 0;
    while (true) {
        size_t s = line.find(kPrefix, pos);
        if (s == std::string::npos) return false;
        size_t i = s + kPrefix.size();
        size_t name_start = i;
        while (i < line.size() &&
               (std::isalnum(static_cast<unsigned char>(line[i])) || line[i] == '_' || line[i] == '-')) {
            i++;
        }
        if (i == name_start || i >= line.size() || line[i] != ']') {
            pos = s + 1;
            continue;
        }
        int s1 = static_cast<int>(s) + 1;
        int e1 = static_cast<int>(i) + 1;
        if (col >= s1 && col <= e1) {
            *name = line.substr(name_start, i - name_start);
            return true;
        }
        pos = i + 1;
    }
}
}  // namespace

void Editor::OrgFootnoteJump() {
    int row = 0, col = 0;
    GetCursorForLua(&row, &col);
    row += 1;
    col += 1;
    if (row < 1 || row > Buf().LineCount()) return;
    const std::string &line = Buf().lines[static_cast<size_t>(row - 1)];
    std::string name;
    if (!FindFootnoteRefAt(line, col, &name)) {
        Notify("No footnote under cursor", NotifyLevel::Warn);
        return;
    }
    const int n = Buf().LineCount();
    std::string def_prefix = "[fn:" + name + "]";
    for (int i = 1; i <= n; i++) {
        if (i == row) continue;
        const std::string &l = Buf().lines[static_cast<size_t>(i - 1)];
        if (l.compare(0, def_prefix.size(), def_prefix) == 0) {
            SetCursorForLua(i - 1, 0);
            return;
        }
    }
    for (int i = 1; i <= n; i++) {
        if (i == row) continue;
        if (Buf().lines[static_cast<size_t>(i - 1)].find(def_prefix) != std::string::npos) {
            SetCursorForLua(i - 1, 0);
            return;
        }
    }
    Notify("No counterpart found for [fn:" + name + "]", NotifyLevel::Warn);
}

void Editor::OrgSetPlanning(const std::string &kind, const std::vector<std::string> &todo_keywords) {
    int row = OrgCurrentHeadlineRow(0, todo_keywords);
    if (row <= 0) return;
    std::string body = FormatOrgDateOnlyNow();
    std::string text = kind + ": <" + body + ">";
    const int n = Buf().LineCount();
    bool has_next = row + 1 <= n;
    std::string next_line = has_next ? Buf().lines[static_cast<size_t>(row)] : "";
    size_t i = SkipWs(next_line, 0);
    std::string tag = kind + ":";
    bool matches = next_line.compare(i, tag.size(), tag) == 0;
    if (matches) {
        SetLineForLua(row, text);
    } else {
        ReplaceLinesForLua(row, row, {text});
    }
}

Editor::Editor() {
    buffers_.emplace_back();
    BootstrapInitialProject();
    ApplyTheme("mep-dark");
}

// WORKSPACES_PLAN.md Phase 1: the editor always has exactly one project
// with one primary workspace holding one tab before anything else runs --
// every accessor (ActiveTab(), CurPane(), ...) indexes unchecked into
// projects_[active_project_].workspaces[active_workspace].tabs[active_tab]
// and relies on this. The project root is the process cwd (decision 4);
// `mep file.txt` keeps that cwd even when the file lives elsewhere.
void Editor::BootstrapInitialProject() {
    Project project;
    project.id = next_project_id_++;
    std::error_code ec;
    std::filesystem::path cwd = std::filesystem::current_path(ec);
    if (!ec) {
        std::filesystem::path canon = std::filesystem::canonical(cwd, ec);
        project.root = (ec ? cwd : canon).string();
    }
    if (project.root.empty()) project.root = "/";
    project.name = std::filesystem::path(project.root).filename().string();
    if (project.name.empty()) project.name = project.root;

    Workspace ws;
    ws.id = next_workspace_id_++;
    ws.name = "main";
    ws.root = project.root;
    ws.primary = true;

    Tab tab;
    tab.root = std::make_unique<SplitNode>();
    tab.root->dir = SplitDir::Leaf;
    tab.root->pane.id = next_pane_id_++;
    tab.root->pane.buffer_id = 0;
    tab.active_pane_id = tab.root->pane.id;
    tab.id = next_tab_id_++;
    ws.tabs.push_back(std::move(tab));

    project.workspaces.push_back(std::move(ws));
    projects_.push_back(std::move(project));
    active_project_ = 0;
}

const Workspace *Editor::FindWorkspace(int id) const {
    for (const Project &p : projects_) {
        for (const Workspace &ws : p.workspaces) {
            if (ws.id == id) return &ws;
        }
    }
    return nullptr;
}

Workspace *Editor::FindWorkspace(int id) {
    for (Project &p : projects_) {
        for (Workspace &ws : p.workspaces) {
            if (ws.id == id) return &ws;
        }
    }
    return nullptr;
}

const Workspace *Editor::FindWorkspaceByName(const std::string &name) const {
    for (const Workspace &ws : ActiveProject().workspaces) {
        if (ws.name == name) return &ws;
    }
    return nullptr;
}

const Project *Editor::FindProject(int id) const {
    for (const Project &p : projects_) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

Project *Editor::FindProject(int id) {
    for (Project &p : projects_) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

Editor::~Editor() = default;

void Editor::HandleInput() {
    TickCollaboration();
    TickWorkspacePersistence(GetTime());
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
        case Mode::KanbanNormal:
            HandleKanbanNormalInput();
            break;
        case Mode::KanbanInsert:
            HandleKanbanInsertInput();
            break;
        case Mode::GanttNormal:
            HandleGanttNormalInput();
            break;
        case Mode::GanttInsert:
            HandleGanttInsertInput();
            break;
        case Mode::HoverFocus:
            HandleHoverFocusInput();
            break;
    }
    TickCollaboration();
}

void Editor::UpdateScrollForPane(int pane_id, int visible_lines, int wrap_cols) {
    SplitNode *node = (float_node_ && float_node_->pane.id == pane_id) ? float_node_.get()
                                                                        : FindNode(ActiveTab().root.get(), pane_id);
    if (!node) return;
    Pane &pane = node->pane;
    visible_lines = std::max(1, visible_lines);
    pane.visible_lines = visible_lines;
    if (pane.buffer_id < 0 || pane.buffer_id >= static_cast<int>(buffers_.size())) return;
    const Buffer &buf = buffers_[static_cast<size_t>(pane.buffer_id)];

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
        /**
         * @brief Computes how many visual scroll "slots" a given buffer row occupies (folds/org images/LaTeX fragments expand or collapse a row; soft-wrap can expand it too).
         * @param r The buffer row to measure.
         * @return The number of visual slots the row contributes.
         */
        auto row_slots = [&](int r) {
            if (org_images_visible_ && buf.org_image_rows.count(r)) return kOrgInlineImageSlots;
            if (org_latex_visible_) {
                auto it = buf.org_latex_rows.find(r);
                if (it != buf.org_latex_rows.end()) return it->second.slots;
            }
            // Soft-wrap (:set wrap, wrap_cols>0): a row's *raw* text length
            // determines how many visual slots it claims, same "one row ->
            // N slots" shape as the fold/org-image cases above -- except a
            // closed fold's own start row never soft-wraps (its rendered
            // content is the one-line "+-- N lines: ... ---" summary, not
            // buf.lines[r] itself), so it's excluded here the same way
            // DrawPane's row loop (main.cpp) excludes it from wrapping.
            if (wrap_cols > 0) {
                bool fold_start = false;
                for (const Fold &f : buf.folds) {
                    if (f.closed && f.start_row == r) {
                        fold_start = true;
                        break;
                    }
                }
                if (!fold_start) {
                    int len = static_cast<int>(buf.lines[static_cast<size_t>(r)].size());
                    return std::max(1, (len + wrap_cols - 1) / wrap_cols);
                }
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
    /**
     * @brief Gets the length of the text in the office document paragraph the cursor currently sits on.
     * @return The paragraph text's length, in characters.
     */
    auto cur_len = [&]() { return static_cast<int>(sess.doc.paragraphs[static_cast<size_t>(sess.cursor_para)].text.size()); };
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
    int max_pan_x = sess.doc ? std::max(0, static_cast<int>(static_cast<float>(sess.doc->Width()) * sess.zoom) - sess.viewport_w) : 0;
    int max_pan_y = sess.doc ? std::max(0, static_cast<int>(static_cast<float>(sess.doc->Height()) * sess.zoom) - sess.viewport_h) : 0;
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

void Editor::WheelScrollSidebar(float dy) {
    SidebarInstance *sb = FindSidebarMut(focused_sidebar_id_);
    if (!sb) return;
    int steps = WheelSteps(wheel_accum_sidebar_, -dy, kWheelLinesPerNotch);
    if (steps == 0) return;
    int max_scroll = std::max(0, static_cast<int>(FlattenSidebar(focused_sidebar_id_).size()) - 1);
    sb->scroll_offset = std::clamp(sb->scroll_offset + steps, 0, max_scroll);
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
        case Mode::Sidebar:
            WheelScrollSidebar(dy);
            break;
        default:
            break;
    }
}

void Editor::IncrementNumberAtCursor(long long delta) {
    CursorPos &cursor = CurPane().cursor;
    std::string &line = Buf().lines[static_cast<size_t>(cursor.row)];
    int len = static_cast<int>(line.size());
    int start = cursor.col;
    while (start < len && !std::isdigit(static_cast<unsigned char>(line[static_cast<size_t>(start)]))) start++;
    if (start >= len) return;  // no number from the cursor onward on this line
    int a = start;
    while (a > 0 && std::isdigit(static_cast<unsigned char>(line[static_cast<size_t>(a - 1)]))) a--;
    int b = start;
    while (b < len && std::isdigit(static_cast<unsigned char>(line[static_cast<size_t>(b)]))) b++;
    bool negative = (a > 0 && line[static_cast<size_t>(a - 1)] == '-');
    int sign_pos = negative ? a - 1 : a;
    int width = b - a;
    bool had_leading_zero = (width > 1 && line[static_cast<size_t>(a)] == '0');

    long long value = 0;
    for (int i = a; i < b; i++) value = value * 10 + (line[static_cast<size_t>(i)] - '0');
    if (negative) value = -value;
    value += delta;

    std::string digits = std::to_string(value < 0 ? -value : value);
    if (had_leading_zero && static_cast<int>(digits.size()) < width) {
        digits = std::string(static_cast<size_t>(width) - digits.size(), '0') + digits;
    }
    std::string replacement = (value < 0 ? "-" : "") + digits;

    PushUndo();
    line.replace(static_cast<size_t>(sign_pos), static_cast<size_t>(b - sign_pos), replacement);
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
            const std::string &line = Buf().lines[static_cast<size_t>(r)];
            int a = std::min(static_cast<int>(line.size()), left);
            int b = (right < 0) ? static_cast<int>(line.size()) : std::min(static_cast<int>(line.size()), right + 1);
            text += (b > a) ? line.substr(static_cast<size_t>(a), static_cast<size_t>(b - a)) : "";
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
    return static_cast<int>(Buf().lines[static_cast<size_t>(row)].size());
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

// An open floating pane (OpenFloatPane) holds the cursor: it is "the
// current pane" for every editing path without touching the tab's own
// active_pane_id, which is what focus returns to once it closes.
Pane &Editor::CurPane() {
    if (float_node_) return float_node_->pane;
    Tab &tab = ActiveTab();
    SplitNode *node = FindNode(tab.root.get(), tab.active_pane_id);
    return node->pane;
}

const Pane &Editor::CurPane() const {
    if (float_node_) return float_node_->pane;
    const Tab &tab = ActiveTab();
    const SplitNode *node = FindNode(tab.root.get(), tab.active_pane_id);
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

void Editor::CollectLeafBuffers(const SplitNode *node, std::vector<int> &ids) const {
    if (node->dir == SplitDir::Leaf) {
        ids.push_back(node->pane.buffer_id);
        return;
    }
    for (auto &child : node->children) CollectLeafBuffers(child.get(), ids);
}

std::vector<int> Editor::PaneBuffersInActiveTab() const {
    std::vector<int> ids;
    CollectLeafBuffers(ActiveTab().root.get(), ids);
    return ids;
}

int Editor::FindPaneIdForBuffer(const SplitNode *node, int buffer_id) const {
    if (node->dir == SplitDir::Leaf) {
        return node->pane.buffer_id == buffer_id ? node->pane.id : -1;
    }
    for (auto &child : node->children) {
        int found = FindPaneIdForBuffer(child.get(), buffer_id);
        if (found >= 0) return found;
    }
    return -1;
}

bool Editor::FocusPaneShowingBuffer(int buffer_id) {
    Tab &tab = ActiveTab();
    int pane_id = FindPaneIdForBuffer(tab.root.get(), buffer_id);
    if (pane_id < 0) return false;
    tab.active_pane_id = pane_id;
    // Same "leaving a live terminal's keystroke-forwarding mode behind
    // when focus actually moves" rule as NavigatePaneDirection's own.
    if (mode_ == Mode::Terminal) mode_ = Mode::Normal;
    SyncModeToActivePaneBuffer();
    return true;
}

namespace {
// Leaf whose visible buffer *or* any hidden buffer tab is `buffer_id`;
// `*tab_index` (into pane.buffer_tabs) tells the caller which, so it can
// bring a hidden tab to the front. Visible-match-first would be nicer in
// a tie, but a buffer is only ever in one pane per tab in practice.
SplitNode *FindLeafHoldingBuffer(SplitNode *node, int buffer_id, int *tab_index) {
    if (!node) return nullptr;
    if (node->dir == SplitDir::Leaf) {
        if (node->pane.buffer_id == buffer_id) {
            *tab_index = -1;
            return node;
        }
        for (size_t i = 0; i < node->pane.buffer_tabs.size(); i++) {
            if (node->pane.buffer_tabs[i] == buffer_id) {
                *tab_index = static_cast<int>(i);
                return node;
            }
        }
        return nullptr;
    }
    for (auto &child : node->children) {
        if (SplitNode *found = FindLeafHoldingBuffer(child.get(), buffer_id, tab_index)) return found;
    }
    return nullptr;
}
}  // namespace

bool Editor::JumpToBuffer(int buffer_id) {
    if (buffer_id < 0 || buffer_id >= static_cast<int>(buffers_.size())) return false;
    const int ws_id = buffers_[static_cast<size_t>(buffer_id)].workspace_id;
    // -1 = unscoped (dashboard/scratch): visible from any workspace, so
    // stay put. WorkspaceSwitch is a no-op when already there and also
    // hops projects when the workspace belongs to another loaded one.
    if (ws_id != -1 && ws_id != ActiveWorkspace().id && !WorkspaceSwitch(ws_id)) return false;

    Workspace &ws = MutableActiveWorkspace();
    for (size_t ti = 0; ti < ws.tabs.size(); ti++) {
        int tab_index = -1;
        SplitNode *leaf = FindLeafHoldingBuffer(ws.tabs[ti].root.get(), buffer_id, &tab_index);
        if (!leaf) continue;
        GoToTab(static_cast<int>(ti));
        ws.tabs[ti].active_pane_id = leaf->pane.id;
        if (tab_index >= 0) {
            leaf->pane.buffer_tab_index = tab_index;
            leaf->pane.buffer_id = buffer_id;
        }
        // Same mode bookkeeping as FocusPaneShowingBuffer: drop a live
        // terminal's forwarding mode if focus left one, then re-derive
        // the mode from the buffer landed on (Terminal again for a
        // terminal, Normal otherwise -- which also leaves Mode::Sidebar
        // when this was invoked from a sidebar's Enter).
        if (mode_ == Mode::Terminal) mode_ = Mode::Normal;
        ClampCursor();
        SyncModeToActivePaneBuffer();
        return true;
    }
    // Nowhere in this workspace's layout (its pane was closed): show it
    // in the current pane instead so Enter still lands somewhere useful.
    if (mode_ == Mode::Terminal) mode_ = Mode::Normal;
    SwitchToBufferForLua(buffer_id);
    return true;
}

int Editor::CursorRowForBuffer(int buffer_id) const {
    const Tab &tab = ActiveTab();
    int pane_id = FindPaneIdForBuffer(tab.root.get(), buffer_id);
    if (pane_id < 0) return -1;
    const SplitNode *node = FindNode(tab.root.get(), pane_id);
    return node ? node->pane.cursor.row : -1;
}

void Editor::FocusTopLeftPane() {
    Tab &tab = ActiveTab();
    std::vector<PaneRect> rects;
    ComputeRects(tab.root.get(), 0.0f, 0.0f, 1.0f, 1.0f, rects);
    if (rects.empty()) return;
    constexpr float kEps = 0.001f;
    const PaneRect *best = &rects[0];
    for (const PaneRect &r : rects) {
        if (r.y0 < best->y0 - kEps ||
            (std::fabs(r.y0 - best->y0) <= kEps && r.x0 < best->x0 - kEps)) {
            best = &r;
        }
    }
    tab.active_pane_id = best->pane_id;
    if (mode_ == Mode::Terminal) mode_ = Mode::Normal;
    SyncModeToActivePaneBuffer();
}

bool Editor::RemovePaneNode(std::unique_ptr<SplitNode> &node_ptr, int pane_id) {
    SplitNode *node = node_ptr.get();
    for (size_t i = 0; i < node->children.size(); i++) {
        const SplitNode *child = node->children[i].get();
        if (child->dir == SplitDir::Leaf && child->pane.id == pane_id) {
            node->children.erase(node->children.begin() + static_cast<long>(i));
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
    buffers_.emplace_back();
    // Scoped to the active workspace (WORKSPACES_PLAN.md Phase 4); the
    // few deliberately unscoped buffers (startup buffer 0, :MepScratch)
    // reset this to -1 themselves.
    buffers_.back().workspace_id = ActiveWorkspace().id;
    return static_cast<int>(buffers_.size()) - 1;
}

// --- Dashboard/scratch/zen (NVIM_PARITY_PLAN.md Part III Phase 12) -------

bool Editor::ShouldShowDashboard() const {
    if (ProjectCount() != 1 || WorkspaceCount() != 1 || Tabs().size() != 1 || buffers_.size() != 1) return false;
    const SplitNode *root = Tabs()[0].root.get();
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
    buffers_[static_cast<size_t>(id)].scratch = true;
    buffers_[static_cast<size_t>(id)].workspace_id = -1;  // one scratch buffer, visible from every workspace
    CurPane().buffer_id = id;
    ClampCursor();
}

int Editor::FindOrCreateBuffer(const std::string &path, bool *existed) {
    // Keyed by (workspace, filename) -- decision 3: the same out-of-tree
    // file opened from two workspaces is two buffers; inside worktrees the
    // paths differ anyway.
    for (size_t i = 0; i < buffers_.size(); i++) {
        if (!BufferInActiveWorkspace(static_cast<int>(i))) continue;
        if (!buffers_[i].filename.empty() && buffers_[i].filename == path) {
            if (existed) *existed = true;
            // Un-deletes it (BufferDelete's own comment) -- re-opening a
            // path whose buffer was `:bd`'d reuses that same buffer
            // object (its content/undo history is still sitting right
            // there, this dedup-by-filename match already found it)
            // rather than silently staying hidden from buffer_list/
            // bnext/bprev while LoadFile happily starts editing it again.
            buffers_[i].deleted = false;
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
    if (float_node_) CloseFloatPane();
    Tab &tab = ActiveTab();
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

void Editor::SplitTabBottom(int buffer_id, float share) {
    if (buffer_id < 0 || buffer_id >= static_cast<int>(buffers_.size())) return;
    if (buffers_[static_cast<size_t>(buffer_id)].deleted) return;
    if (float_node_) CloseFloatPane();
    Tab &tab = ActiveTab();
    if (!tab.root) return;

    auto new_leaf = std::make_unique<SplitNode>();
    new_leaf->dir = SplitDir::Leaf;
    new_leaf->pane.id = next_pane_id_++;
    new_leaf->pane.buffer_id = buffer_id;
    new_leaf->pane.buffer_tabs = {buffer_id};
    new_leaf->pane.buffer_tab_index = 0;
    const int new_pane_id = new_leaf->pane.id;

    // Horizontal stacks its children top to bottom (ComputeRects), so
    // [old root, new leaf] puts the new pane along the bottom, spanning
    // the tab's full width whatever the old root was split into.
    auto new_root = std::make_unique<SplitNode>();
    new_root->dir = SplitDir::Horizontal;
    new_root->children.push_back(std::move(tab.root));
    new_root->children.push_back(std::move(new_leaf));
    share = std::clamp(share, kMinPaneShare, 1.0f - kMinPaneShare);
    new_root->shares = {1.0f - share, share};
    tab.root = std::move(new_root);

    tab.active_pane_id = new_pane_id;
    // Focus moved off whatever pane was live before: same "leave Terminal
    // mode's keystroke forwarding behind" rule as NavigatePaneDirection,
    // then SyncModeToActivePaneBuffer re-enters it if the *new* pane is
    // itself a terminal.
    if (mode_ == Mode::Terminal) mode_ = Mode::Normal;
    SyncModeToActivePaneBuffer();
}

// --- Terminal panes (`:terminal`/`:term`, Part VI Phase 27+) -------------

void Editor::OpenTerminal(const std::string &args) {
    SplitCurrentPane(SplitDir::Horizontal, "");
    OpenTerminalInPlace(args);
}

void Editor::OpenTerminalInPlace(const std::string &args) {
    const char *shell_env = std::getenv("SHELL");
    std::string shell = (shell_env && *shell_env) ? shell_env : "/bin/sh";
    std::vector<std::string> argv = args.empty() ? std::vector<std::string>{shell}
                                                  : std::vector<std::string>{shell, "-c", args};
    OpenTerminalInPlaceArgv(argv, args.empty() ? shell : args);
}

void Editor::OpenTerminalInPlaceArgv(const std::vector<std::string> &argv, const std::string &title) {
    if (argv.empty()) {
        status_message_ = "Failed to start terminal: empty command";
        return;
    }
    Tab &tab = ActiveTab();
    SplitNode *node = FindNode(tab.root.get(), tab.active_pane_id);
    if (!node) return;

    int buffer_id = CreateEmptyBuffer();
    node->pane.buffer_id = buffer_id;
    node->pane.cursor = CursorPos{0, 0};
    node->pane.scroll_row = 0;
    node->pane.buffer_tabs = {buffer_id};
    node->pane.buffer_tab_index = 0;

    TerminalSession sess;
    sess.buffer_id = buffer_id;
    sess.title = title.empty() ? argv[0] : title;
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
    for (const std::string &a : remote_argv) argv_json.push_back(a);
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
    /**
     * @brief Feeds a chunk of raw child-process output into the terminal's VTerm for parsing/rendering.
     * @param chunk The raw bytes received from the child process.
     */
    cb.on_stdout_raw = [vterm_ptr](const std::string &chunk) { vterm_ptr->Feed(chunk); };
    /**
     * @brief Marks this terminal session as exited and records its exit code, once the child process terminates.
     * @param code The child process's exit code.
     */
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
    //
    // COLORTERM=truecolor for the same reason, added alongside TERM after
    // a report that a real full-screen TUI (Claude Code's own, running
    // inside :terminal) looked visibly worse here than in an ordinary
    // terminal emulator. TERM=xterm-256color only advertises 256-indexed
    // colors on its own -- COLORTERM is the separate, additional signal
    // most modern color-detection libraries (supports-color/chalk, which
    // Node/Ink-based TUIs including Claude Code's use) check before
    // emitting real 24-bit SGR instead of quantizing to a 256-color
    // palette. Confirmed empirically, not just from reading chalk's
    // source: Node's own tty.WriteStream.getColorDepth() -- the same
    // primitive supports-color is built on -- reports 24 (truecolor) with
    // COLORTERM=truecolor ambient, but only 8 (256-color) with TERM=
    // xterm-256color alone, run both ways under `script` (real PTY) with
    // an otherwise-identical environment. Like TERM above, an honest
    // claim, not a placeholder: VTerm already parses/renders full 24-bit
    // RGB SGI (38/48;2;r;g;b), so nothing downstream is left over-
    // promising.
    //
    // cwd is the active workspace root (WORKSPACES_PLAN.md decision 4) --
    // explicit rather than inherited so a terminal opened in one worktree
    // stays there even after the process cwd follows a workspace switch.
    // MEP_WORKSPACE/MEP_PROJECT let shell prompts show where they are.
    //
    // MEP_AGENT_SOCKET pins this window's own agent-control socket
    // (agent_rpc.cpp) for anything started from inside the terminal --
    // mcp/mep_client.ts's discoverSocketPath() honors it ahead of its
    // *.sock directory scan, which errors out as ambiguous the moment a
    // second mep window is open. An AI agent launched in a :terminal
    // (mep.ai_terminal_open, kBuiltinAiTerminal in main.cpp) therefore
    // always drives the very instance it's sitting in, never a sibling
    // window. Only set when the socket actually bound (native builds;
    // agent_rpc.h's wasm/Windows stub returns "").
    std::vector<std::pair<std::string, std::string>> extra_env = {{"TERM", "xterm-256color"},
                                                                  {"COLORTERM", "truecolor"},
                                                                  {"MEP_WORKSPACE", ActiveWorkspace().name},
                                                                  {"MEP_PROJECT", ActiveProject().name}};
    std::string agent_socket = mep::agent::SocketPath();
    if (!agent_socket.empty()) extra_env.emplace_back("MEP_AGENT_SOCKET", agent_socket);
    // MEP_TERMINAL_BUFFER: this terminal's own buffer id, so an agent
    // started inside it can report which pane it lives in (mcp/server.ts
    // forwards it as session.identify's terminal_buffer_id) -- how the
    // AI-agents sidebar pairs a connected agent with a jump target.
    extra_env.emplace_back("MEP_TERMINAL_BUFFER", std::to_string(sess.buffer_id));
    sess.job_id = JobManager::Instance().Spawn(argv, ActiveRoot(), std::move(cb), /*use_pty=*/true, std::move(extra_env));
    if (sess.job_id == 0) status_message_ = "Failed to start terminal";
#endif
}

void Editor::TerminalWrite(const TerminalSession &sess, const std::string &bytes) {
    if (sess.job_id <= 0) return;
#if defined(__EMSCRIPTEN__)
    mep_js_pty_write(sess.job_id, bytes.data(), static_cast<int>(bytes.size()));
#else
    JobManager::Instance().WriteStdin(sess.job_id, bytes);
#endif
}

void Editor::TerminalResizeBackend(const TerminalSession &sess, int cols, int rows) {
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

bool Editor::WriteToTerminalBuffer(int buffer_id, const std::string &text) {
    const TerminalSession *sess = FindTerminal(buffer_id);
    if (!sess || sess->exited) return false;
    TerminalWrite(*sess, text);
    return true;
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
    int max_pan_x = sess.doc ? std::max(0, static_cast<int>(static_cast<float>(sess.doc->Width()) * sess.zoom) - w) : 0;
    int max_pan_y = sess.doc ? std::max(0, static_cast<int>(static_cast<float>(sess.doc->Height()) * sess.zoom) - h) : 0;
    sess.pan_x = std::clamp(sess.pan_x, 0, max_pan_x);
    sess.pan_y = std::clamp(sess.pan_y, 0, max_pan_y);
}

void Editor::OpenImageInPlace(const std::string &path, const unsigned char *bytes, size_t len) {
    int buffer_id = -1;
    for (size_t i = 0; i < buffers_.size(); i++) {
        if (!BufferInActiveWorkspace(static_cast<int>(i))) continue;
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
        buffers_[static_cast<size_t>(buffer_id)].filename = path;
        ImageSession sess;
        sess.buffer_id = buffer_id;
        sess.doc = std::move(doc);
        images_[buffer_id] = std::move(sess);
    } else {
        // `path` is already open in a buffer -- re-decode `bytes` into it
        // instead of leaving the cached ImageDoc stale. Callers that expect
        // to see the file's *current* disk content on a repeat open (e.g.
        // the R language-UI mode's figures pane, reopened on a poll to show
        // the latest plot) would otherwise keep showing whatever was on
        // disk the first time this path was opened.
        auto it = images_.find(buffer_id);
        if (it != images_.end()) {
            auto doc = std::make_unique<ImageDoc>();
            if (!doc->LoadFromMemory(bytes, len)) {
                status_message_ = "E-\"" + path + "\": " + doc->Error();
                return;
            }
            it->second.doc = std::move(doc);
            it->second.pan_x = 0;
            it->second.pan_y = 0;
        }
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
    } else if (IsKanbanViewActive(CurPane().buffer_id)) {
        // Unlike every branch above, this isn't a buffer *identity* check
        // (a Kanban/Gantt view sits over an ordinary org text buffer) --
        // see org_view_mode_'s own comment. Always re-enters at
        // KanbanNormal, never resumes mid-KanbanInsert, same reasoning as
        // Office/Sheet above.
        mode_ = Mode::KanbanNormal;
    } else if (IsGanttViewActive(CurPane().buffer_id)) {
        mode_ = Mode::GanttNormal;
    } else if (mode_ == Mode::Terminal || mode_ == Mode::Image || mode_ == Mode::Pdf || mode_ == Mode::Html ||
               mode_ == Mode::OfficeNormal || mode_ == Mode::OfficeInsert || mode_ == Mode::OfficeVisual ||
               mode_ == Mode::SheetNormal || mode_ == Mode::SheetInsert || mode_ == Mode::SheetVisual ||
               mode_ == Mode::KanbanNormal || mode_ == Mode::KanbanInsert || mode_ == Mode::GanttNormal ||
               mode_ == Mode::GanttInsert || mode_ == Mode::Sidebar) {
        // Mode::Sidebar included here (unlike every other case above,
        // it's not a *buffer-type*-driven mode, it's an input-focus one)
        // -- opening a plain-text file from a sidebar (e.g. the built-in
        // file tree's Enter-on-a-file, kBuiltinFileTree's on_click ->
        // mep.open -> LoadFile -> here) landed on an ordinary buffer that
        // doesn't match any branch above, so mode_ was silently left at
        // Mode::Sidebar even though the real pane it just switched to is
        // now showing plain text. Every keypress still routed to
        // HandleSidebarInput() (main.cpp's mode_ dispatch switches purely
        // on mode_, independent of which pane/buffer is "active"), so
        // typing did nothing and even hjkl was reinterpreted as sidebar
        // navigation -- reported as "stuck in sidebar mode after opening
        // a file, only a mouse click in the pane recovers it." A newly
        // stale focused_sidebar_id_ is fine left as-is here, same as the
        // mod1+hjkl pane-blur case (main.cpp's own DrawSidebars comment)
        // -- every reader of it already gates on mode_ == Mode::Sidebar
        // first, not on focused_sidebar_id_ alone.
        mode_ = Mode::Normal;
    }
}

void Editor::ApplyImageZoom(ImageSession &sess, float new_zoom) {
    if (!sess.doc || sess.doc->Width() <= 0 || sess.doc->Height() <= 0) return;
    new_zoom = std::clamp(new_zoom, kMinImageZoom, kMaxImageZoom);
    float ratio = new_zoom / sess.zoom;
    float center_x = static_cast<float>(sess.pan_x) + static_cast<float>(sess.viewport_w) / 2.0f;
    float center_y = static_cast<float>(sess.pan_y) + static_cast<float>(sess.viewport_h) / 2.0f;
    sess.zoom = new_zoom;
    sess.pan_x = static_cast<int>(center_x * ratio - static_cast<float>(sess.viewport_w) / 2.0f);
    sess.pan_y = static_cast<int>(center_y * ratio - static_cast<float>(sess.viewport_h) / 2.0f);
    int mx = std::max(0, static_cast<int>(static_cast<float>(sess.doc->Width()) * sess.zoom) - sess.viewport_w);
    int my = std::max(0, static_cast<int>(static_cast<float>(sess.doc->Height()) * sess.zoom) - sess.viewport_h);
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
    int max_pan_x = sess->doc ? std::max(0, static_cast<int>(static_cast<float>(sess->doc->Width()) * sess->zoom) - sess->viewport_w) : 0;
    int max_pan_y = sess->doc ? std::max(0, static_cast<int>(static_cast<float>(sess->doc->Height()) * sess->zoom) - sess->viewport_h) : 0;

    // IsKeyPressed(Repeat) rather than draining GetKeyPressed(): GLFW only
    // enqueues the initial key-down into the GetKeyPressed() queue, so
    // holding a key down (OS auto-repeat) would otherwise pan exactly once.
    /**
     * @brief Checks whether a key was just pressed or is auto-repeating.
     * @param key The GLFW/raylib key code to check.
     * @return True if the key is freshly pressed or repeating this frame.
     */
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
    if (ctrl && held(KEY_D)) sess->pan_y = std::clamp(sess->pan_y + sess->viewport_h / 2, 0, max_pan_y);
    if (ctrl && held(KEY_U)) sess->pan_y = std::clamp(sess->pan_y - sess->viewport_h / 2, 0, max_pan_y);

    // +/-/= zoom: +/- multiply or divide the zoom factor by kImageZoomStep,
    // re-anchored on whatever image point is currently at the viewport's
    // center (so the thing you're looking at stays put instead of the view
    // snapping back to the image's top-left corner on every keypress); =
    // fits the whole image into the current viewport and resets pan.
    // ApplyImageZoom (editor.h) does the actual work -- also reused by
    // HandleMouseWheel's Image branch for Ctrl-scroll.
    /**
     * @brief Applies a new zoom level to the current image session, re-anchored on the viewport center.
     * @param new_zoom The target zoom factor.
     */
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
            float fit = std::min(static_cast<float>(sess->viewport_w) / static_cast<float>(sess->doc->Width()),
                                  static_cast<float>(sess->viewport_h) / static_cast<float>(sess->doc->Height()));
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
/**
 * @brief Appends a Unicode codepoint to a string, UTF-8-encoded.
 * @param s The string to append to.
 * @param cp The Unicode codepoint to encode.
 */
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

std::vector<int> Editor::HtmlBufferIds() const {
    std::vector<int> ids;
    for (const auto &[id, sess] : htmldocs_) ids.push_back(id);
    return ids;
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

void Editor::AdvanceHtmlMedia(int buffer_id, double seconds) {
    auto it = htmldocs_.find(buffer_id);
    if (it == htmldocs_.end()) return;
    AdvanceHtmlMediaClock(it->second.doc, seconds);
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
    // Local <audio>/<video> sources are decoded before scripts so
    // `duration`/`readyState` are already meaningful to inline code.
    LoadHtmlMedia(sess.doc, std::filesystem::path(source).parent_path().string());
    // Info-level console messages surface as plain notifications; script errors are prefixed with the page's source.
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
            if (!BufferInActiveWorkspace(static_cast<int>(i))) continue;
            if (buffers_[i].filename == source) {
                buffer_id = static_cast<int>(i);
                break;
            }
        }
    }
    if (buffer_id < 0) {
        buffer_id = CreateEmptyBuffer();
        buffers_[static_cast<size_t>(buffer_id)].filename = source;
    }
    if (!IsHtmlBuffer(buffer_id)) {
        buffers_[static_cast<size_t>(buffer_id)].lines.clear();
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
    buffers_[static_cast<size_t>(buffer_id)].filename = source;
    PopulateHtmlSession(it->second, origin, source, bytes, len);
}

void Editor::ConvertHtmlBufferToText(int buffer_id) {
    auto it = htmldocs_.find(buffer_id);
    if (it == htmldocs_.end()) return;
    std::string path = buffers_[static_cast<size_t>(buffer_id)].filename;
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
    buffers_[static_cast<size_t>(buffer_id)].lines = std::move(lines);
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
    for (const auto &l : buffers_[static_cast<size_t>(buffer_id)].lines) {
        content += l;
        content += "\n";
    }
    const std::string &path = buffers_[static_cast<size_t>(buffer_id)].filename;
    HtmlSession sess;
    sess.buffer_id = buffer_id;
    PopulateHtmlSession(sess, path, path, reinterpret_cast<const unsigned char *>(content.data()), content.size());
    htmldocs_[buffer_id] = std::move(sess);
    buffers_[static_cast<size_t>(buffer_id)].lines.clear();
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
    /**
     * @brief Checks whether a key was just pressed or is auto-repeating.
     * @param key The GLFW/raylib key code to check.
     * @return True if the key is freshly pressed or repeating this frame.
     */
    auto held = [](int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); };
    if (held(KEY_J) || held(KEY_DOWN)) sess->scroll_y += kScrollStep;
    if (held(KEY_K) || held(KEY_UP)) sess->scroll_y = std::max(0.0f, sess->scroll_y - kScrollStep);
    if ((ctrl && held(KEY_D)) || held(KEY_PAGE_DOWN)) {
        sess->scroll_y += static_cast<float>(sess->viewport_h) * 0.5f;
    }
    if ((ctrl && held(KEY_U)) || held(KEY_PAGE_UP)) {
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
    return static_cast<float>(PdfPageSizePt(sess, page_index).second * static_cast<double>(sess.rendered_scale) * static_cast<double>(sess.zoom));
}

void Editor::ResizePdfViewport(int buffer_id, int w, int h) {
    auto it = pdfs_.find(buffer_id);
    if (it == pdfs_.end()) return;
    PdfSession &sess = it->second;
    sess.viewport_w = w;
    sess.viewport_h = h;
    if (!sess.doc || sess.doc->PageCount() <= 0) return;
    double page_w_pt = PdfPageSizePt(sess, sess.page).first;
    int page_w_px = static_cast<int>(page_w_pt * static_cast<double>(sess.rendered_scale) * static_cast<double>(sess.zoom));
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
        if (!BufferInActiveWorkspace(static_cast<int>(i))) continue;
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
        buffers_[static_cast<size_t>(buffer_id)].filename = path;
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
    /**
     * @brief Returns a PDF page's rendered height in screen pixels at the session's current scale/zoom.
     * @param idx The page index.
     * @return The page's on-screen height in pixels.
     */
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
    int mx = std::max(0, static_cast<int>(page_w_pt * static_cast<double>(sess.rendered_scale) * static_cast<double>(sess.zoom)) - sess.viewport_w);
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
    float center_x = static_cast<float>(sess.pan_x) + static_cast<float>(sess.viewport_w) / 2.0f;
    float center_y = sess.scroll_y + static_cast<float>(sess.viewport_h) / 2.0f;
    sess.zoom = new_zoom;
    sess.pan_x = static_cast<int>(center_x * ratio - static_cast<float>(sess.viewport_w) / 2.0f);
    sess.scroll_y = center_y * ratio - static_cast<float>(sess.viewport_h) / 2.0f;
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

    /**
     * @brief Re-bases the current PDF page's scroll position after a scroll or zoom change.
     */
    auto rebase_scroll = [&]() { RebasePdfScroll(*sess); };
    /**
     * @brief Clamps the current PDF page's horizontal pan offset to valid bounds.
     */
    auto clamp_pan_x = [&]() { ClampPdfPanX(*sess); };

    constexpr int kScrollStep = 40;
    // Same IsKeyPressed||IsKeyPressedRepeat reasoning as HandleImageInput --
    // GetKeyPressed() only fires on initial key-down, not OS auto-repeat.
    /**
     * @brief Checks whether a key was just pressed or is auto-repeating.
     * @param key The GLFW/raylib key code to check.
     * @return True if the key is freshly pressed or repeating this frame.
     */
    auto held = [](int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); };
    bool scrolled = false;
    if (held(KEY_J) || held(KEY_DOWN)) { sess->scroll_y += kScrollStep; scrolled = true; }
    if (held(KEY_K) || held(KEY_UP)) { sess->scroll_y -= kScrollStep; scrolled = true; }
    if (scrolled) rebase_scroll();
    if (held(KEY_H) || held(KEY_LEFT)) { sess->pan_x -= kScrollStep; clamp_pan_x(); }
    if (held(KEY_L) || held(KEY_RIGHT)) { sess->pan_x += kScrollStep; clamp_pan_x(); }

    /**
     * @brief Jumps to a clamped PDF page number and resets vertical scroll to its top.
     * @param new_page The target page index (clamped to the valid page range).
     */
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
    if (ctrl && held(KEY_D)) { sess->scroll_y += static_cast<float>(sess->viewport_h) * 0.5f; rebase_scroll(); }
    if (ctrl && held(KEY_U)) { sess->scroll_y -= static_cast<float>(sess->viewport_h) * 0.5f; rebase_scroll(); }
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
    /**
     * @brief Settles the current PDF zoom, folding excess zoom into rendered_scale and clearing cached rasters when out of range.
     */
    auto settle_zoom = [&]() { SettlePdfZoom(*sess); };
    /**
     * @brief Applies a new zoom level to the current PDF session, re-anchored on the viewport center.
     * @param new_zoom The target zoom factor.
     */
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
            double base_w = pw * static_cast<double>(sess->rendered_scale), base_h = ph * static_cast<double>(sess->rendered_scale);
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

// Jumps the cursor to the start of a given paragraph -- used by the
// Outline panel's click-to-jump (main.cpp's DrawOfficeSidePanels).
// Without also moving the cursor, DrawPane's own scroll-follow-cursor scan
// would just walk scroll_para back toward wherever the cursor still sits,
// one paragraph a frame, undoing the jump right after it happened.
void Editor::SetOfficeCursorPara(int buffer_id, int para) {
    auto it = officedocs_.find(buffer_id);
    if (it == officedocs_.end()) return;
    OfficeSession &sess = it->second;
    int max_para = std::max(0, static_cast<int>(sess.doc.paragraphs.size()) - 1);
    sess.cursor_para = std::clamp(para, 0, max_para);
    sess.cursor_col = 0;
    sess.has_selection = false;
}

void Editor::SetOfficeCursorWrapLines(int buffer_id, int cursor_para, std::vector<std::pair<int, int>> lines) {
    auto it = officedocs_.find(buffer_id);
    if (it == officedocs_.end()) return;
    it->second.cursor_wrap_lines_para = cursor_para;
    it->second.cursor_wrap_lines = std::move(lines);
}

bool Editor::MoveOfficeCursorVisualLine(OfficeSession *sess, int dir) {
    const std::vector<std::pair<int, int>> &lines = sess->cursor_wrap_lines;
    bool cache_fresh = sess->cursor_wrap_lines_para == sess->cursor_para && !lines.empty();
    if (cache_fresh) {
        // Which cached visual line cursor_col is currently on -- the LAST
        // line whose start it has reached, same tie-break as main.cpp's
        // own cursor_line_in_para (see that comment for why: a contiguous
        // wrap point has line[k].end == line[k+1].start, and "first line
        // whose end it doesn't exceed" would always resolve that shared
        // value to line k, one line short of where the cursor actually is).
        int line_index = 0;
        for (int i = 0; i < static_cast<int>(lines.size()); i++) {
            if (lines[static_cast<size_t>(i)].first > sess->cursor_col) break;
            line_index = i;
        }
        int target_index = line_index + (dir < 0 ? -1 : 1);
        if (target_index >= 0 && target_index < static_cast<int>(lines.size())) {
            // Preserve cursor_col's offset *into* its current visual line
            // (like a screen-column goal) rather than an absolute byte
            // offset, so moving through a run of same-width wrapped lines
            // tracks the same visual column, the way plain h/l-adjacent
            // vertical motion should.
            int offset = sess->cursor_col - lines[static_cast<size_t>(line_index)].first;
            const std::pair<int, int> &target = lines[static_cast<size_t>(target_index)];
            sess->cursor_col = std::clamp(target.first + offset, target.first, target.second);
            return false;  // stayed within the same paragraph
        }
    }
    // Off the paragraph's own first/last visual line (or no fresh wrap
    // cache yet, e.g. the very first frame) -- fall back to the plain
    // prev/next-paragraph step every mode's j/k/Up/Down already had before
    // visual-line stepping existed.
    int para_count = static_cast<int>(sess->doc.paragraphs.size());
    int new_para = std::clamp(sess->cursor_para + dir, 0, para_count - 1);
    bool moved = new_para != sess->cursor_para;
    sess->cursor_para = new_para;
    sess->cursor_col = std::min(sess->cursor_col, static_cast<int>(sess->doc.paragraphs[static_cast<size_t>(sess->cursor_para)].text.size()));
    return moved;
}

void Editor::OpenOfficeInPlace(const std::string &path, const unsigned char *bytes, size_t len) {
    int buffer_id = -1;
    for (size_t i = 0; i < buffers_.size(); i++) {
        if (!BufferInActiveWorkspace(static_cast<int>(i))) continue;
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
        buffers_[static_cast<size_t>(buffer_id)].filename = path;
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

    // Document-viewer zoom is intentionally available before table/nav
    // handling, so Ctrl+= and Ctrl+- work from every ordinary office
    // reading state rather than becoming table-specific commands.
    const bool ctrl_down = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (ctrl_down && (IsKeyPressed(KEY_EQUAL) || IsKeyPressedRepeat(KEY_EQUAL))) {
        SetOfficeZoom(1.1f);
        status_message_ = "Document zoom: " + std::to_string(static_cast<int>(std::lround(sess->zoom * 100.0f))) + "%";
        return;
    }
    if (ctrl_down && (IsKeyPressed(KEY_MINUS) || IsKeyPressedRepeat(KEY_MINUS))) {
        SetOfficeZoom(1.0f / 1.1f);
        status_message_ = "Document zoom: " + std::to_string(static_cast<int>(std::lround(sess->zoom * 100.0f))) + "%";
        return;
    }

    /**
     * @brief Returns the character length of the paragraph the office cursor is currently on.
     * @return The length, in characters, of the current paragraph's text.
     */
    auto cur_len = [&]() { return static_cast<int>(sess->doc.paragraphs[static_cast<size_t>(sess->cursor_para)].text.size()); };
    /**
     * @brief Moves the office cursor to a clamped paragraph index, entering a table if the destination paragraph anchors one.
     * @param new_para The target paragraph index (clamped to the valid range).
     */
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
            int tref = sess->doc.paragraphs[static_cast<size_t>(sess->cursor_para)].table_ref;
            if (tref >= 0) EnterOfficeTable(tref);
        }
    };

    /**
     * @brief Checks whether a key was just pressed or is auto-repeating.
     * @param key The GLFW/raylib key code to check.
     * @return True if the key is freshly pressed or repeating this frame.
     */
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

    // h/l move within the current paragraph's flat text (like column
    // motion over Buffer::lines[row]); j/k move by *visual* (word-wrapped)
    // line when MoveOfficeCursorVisualLine has fresh wrap data for the
    // current paragraph, falling back to a plain prev/next-paragraph step
    // at its first/last visual line -- see that function's own comment.
    // Reproduces goto_para's table-auto-entry on an actual paragraph
    // change (MoveOfficeCursorVisualLine returning true) since it's
    // bypassing goto_para itself for the visual-line-internal case.
    if (held(KEY_H) || held(KEY_LEFT)) sess->cursor_col = std::max(0, sess->cursor_col - 1);
    if (held(KEY_L) || held(KEY_RIGHT)) sess->cursor_col = std::min(cur_len(), sess->cursor_col + 1);
    if (held(KEY_J) || held(KEY_DOWN)) {
        if (MoveOfficeCursorVisualLine(sess, 1)) {
            int tref = sess->doc.paragraphs[static_cast<size_t>(sess->cursor_para)].table_ref;
            if (tref >= 0) EnterOfficeTable(tref);
        }
    }
    if (held(KEY_K) || held(KEY_UP)) {
        if (MoveOfficeCursorVisualLine(sess, -1)) {
            int tref = sess->doc.paragraphs[static_cast<size_t>(sess->cursor_para)].table_ref;
            if (tref >= 0) EnterOfficeTable(tref);
        }
    }

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
    if (ctrl && (held(KEY_D) || held(KEY_U))) {
        bool down = held(KEY_D);
        // Paragraph count, not visual line count -- word-wrap needs
        // main.cpp's MeasureTextEx, which this raylib-model-level function
        // can't call (see ResizeOfficeViewport's own comment on why
        // scroll-follow itself lives in main.cpp), so "half a screen" is
        // approximated from the body font's line height instead of an
        // exact wrapped-line count. Reuses goto_para so it gets the same
        // clamp-without-re-entering-a-table behavior as j/k.
        float line_h = std::max(1.0f, sess->base_font_pt * sess->zoom * 1.35f);
        int half_page = std::max(1, static_cast<int>(static_cast<float>(sess->viewport_h) / (2.0f * line_h)));
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
    sess.cursor_col = std::min(sess.cursor_col, static_cast<int>(sess.doc.paragraphs[static_cast<size_t>(sess.cursor_para)].text.size()));
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
    sess.cursor_col = std::min(sess.cursor_col, static_cast<int>(sess.doc.paragraphs[static_cast<size_t>(sess.cursor_para)].text.size()));
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

    const bool ctrl_down = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (ctrl_down && (IsKeyPressed(KEY_EQUAL) || IsKeyPressedRepeat(KEY_EQUAL))) {
        SetOfficeZoom(1.1f);
        status_message_ = "Document zoom: " + std::to_string(static_cast<int>(std::lround(sess->zoom * 100.0f))) + "%";
        return;
    }
    if (ctrl_down && (IsKeyPressed(KEY_MINUS) || IsKeyPressedRepeat(KEY_MINUS))) {
        SetOfficeZoom(1.0f / 1.1f);
        status_message_ = "Document zoom: " + std::to_string(static_cast<int>(std::lround(sess->zoom * 100.0f))) + "%";
        return;
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

    /**
     * @brief Returns the character length of the paragraph the office cursor is currently on.
     * @return The length, in characters, of the current paragraph's text.
     */
    auto cur_len = [&]() { return static_cast<int>(sess->doc.paragraphs[static_cast<size_t>(sess->cursor_para)].text.size()); };

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
            DocParagraph &p = sess->doc.paragraphs[static_cast<size_t>(sess->cursor_para)];
            ApplyInsertToParagraph(p, sess->cursor_col, std::string(1, static_cast<char>(cp)));
            sess->cursor_col++;
            sess->modified = true;
        }
        cp = GetCharPressed();
    }

    if (enter || IsKeyPressedRepeat(KEY_ENTER)) {
        DocParagraph &p = sess->doc.paragraphs[static_cast<size_t>(sess->cursor_para)];
        DocParagraph second = SplitParagraphAt(p, sess->cursor_col);
        sess->doc.paragraphs.insert(sess->doc.paragraphs.begin() + sess->cursor_para + 1, std::move(second));
        sess->cursor_para++;
        sess->cursor_col = 0;
        sess->modified = true;
    }
    if (backspace || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        if (sess->cursor_col > 0) {
            DocParagraph &p = sess->doc.paragraphs[static_cast<size_t>(sess->cursor_para)];
            ApplyDeleteToParagraph(p, sess->cursor_col - 1, sess->cursor_col);
            sess->cursor_col--;
            sess->modified = true;
        } else if (sess->cursor_para > 0) {
            int prev_len = static_cast<int>(sess->doc.paragraphs[static_cast<size_t>(sess->cursor_para - 1)].text.size());
            MergeParagraphs(sess->doc.paragraphs[static_cast<size_t>(sess->cursor_para - 1)], sess->doc.paragraphs[static_cast<size_t>(sess->cursor_para)]);
            sess->doc.paragraphs.erase(sess->doc.paragraphs.begin() + sess->cursor_para);
            sess->cursor_para--;
            sess->cursor_col = prev_len;
            sess->modified = true;
        }
    }
    if (del || IsKeyPressedRepeat(KEY_DELETE)) {
        int len = cur_len();
        if (sess->cursor_col < len) {
            DocParagraph &p = sess->doc.paragraphs[static_cast<size_t>(sess->cursor_para)];
            ApplyDeleteToParagraph(p, sess->cursor_col, sess->cursor_col + 1);
            sess->modified = true;
        } else if (sess->cursor_para + 1 < static_cast<int>(sess->doc.paragraphs.size())) {
            MergeParagraphs(sess->doc.paragraphs[static_cast<size_t>(sess->cursor_para)], sess->doc.paragraphs[static_cast<size_t>(sess->cursor_para) + 1]);
            sess->doc.paragraphs.erase(sess->doc.paragraphs.begin() + sess->cursor_para + 1);
            sess->modified = true;
        }
    }
    if (IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT)) sess->cursor_col = std::max(0, sess->cursor_col - 1);
    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT)) sess->cursor_col = std::min(cur_len(), sess->cursor_col + 1);
    // Visual-line-aware, like HandleOfficeNormalInput's own j/k -- see
    // MoveOfficeCursorVisualLine's comment.
    if (IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP)) MoveOfficeCursorVisualLine(sess, -1);
    if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN)) MoveOfficeCursorVisualLine(sess, 1);
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
    const bool ctrl_down = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (ctrl_down && (IsKeyPressed(KEY_EQUAL) || IsKeyPressedRepeat(KEY_EQUAL))) {
        SetOfficeZoom(1.1f);
        status_message_ = "Document zoom: " + std::to_string(static_cast<int>(std::lround(sess->zoom * 100.0f))) + "%";
        return;
    }
    if (ctrl_down && (IsKeyPressed(KEY_MINUS) || IsKeyPressedRepeat(KEY_MINUS))) {
        SetOfficeZoom(1.0f / 1.1f);
        status_message_ = "Document zoom: " + std::to_string(static_cast<int>(std::lround(sess->zoom * 100.0f))) + "%";
        return;
    }
    int para_count = static_cast<int>(sess->doc.paragraphs.size());
    if (para_count <= 0) {
        mode_ = Mode::OfficeNormal;
        return;
    }
    /**
     * @brief Returns the character length of the paragraph the office cursor is currently on.
     * @return The length, in characters, of the current paragraph's text.
     */
    auto cur_len = [&]() { return static_cast<int>(sess->doc.paragraphs[static_cast<size_t>(sess->cursor_para)].text.size()); };
    /**
     * @brief Moves the office cursor to a clamped paragraph index, keeping the column within the new paragraph's bounds.
     * @param new_para The target paragraph index (clamped to the valid range).
     */
    auto goto_para = [&](int new_para) {
        sess->cursor_para = std::clamp(new_para, 0, para_count - 1);
        sess->cursor_col = std::min(sess->cursor_col, cur_len());
    };
    /**
     * @brief Checks whether a key was just pressed or is auto-repeating.
     * @param key The GLFW/raylib key code to check.
     * @return True if the key is freshly pressed or repeating this frame.
     */
    auto held = [](int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); };
    if (held(KEY_H) || held(KEY_LEFT)) sess->cursor_col = std::max(0, sess->cursor_col - 1);
    if (held(KEY_L) || held(KEY_RIGHT)) sess->cursor_col = std::min(cur_len(), sess->cursor_col + 1);
    // Visual-line-aware, like HandleOfficeNormalInput's own j/k -- see
    // MoveOfficeCursorVisualLine's comment. No table-auto-entry follow-up
    // here, matching this mode's own goto_para (above), which never had it
    // either.
    if (held(KEY_J) || held(KEY_DOWN)) MoveOfficeCursorVisualLine(sess, 1);
    if (held(KEY_K) || held(KEY_UP)) MoveOfficeCursorVisualLine(sess, -1);

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
            ToggleFormatOverRange(sess.doc.paragraphs[static_cast<size_t>(pa)], ca, cb, field);
        } else {
            ToggleFormatOverRange(sess.doc.paragraphs[static_cast<size_t>(pa)], ca, static_cast<int>(sess.doc.paragraphs[static_cast<size_t>(pa)].text.size()),
                                   field);
            for (int pi = pa + 1; pi < pb; pi++) {
                ToggleFormatOverRange(sess.doc.paragraphs[static_cast<size_t>(pi)], 0, static_cast<int>(sess.doc.paragraphs[static_cast<size_t>(pi)].text.size()),
                                       field);
            }
            ToggleFormatOverRange(sess.doc.paragraphs[static_cast<size_t>(pb)], 0, cb, field);
        }
        sess.has_selection = false;
        sess.cursor_para = pa;
        sess.cursor_col = ca;
        mode_ = Mode::OfficeNormal;
    } else {
        int len = static_cast<int>(sess.doc.paragraphs[static_cast<size_t>(sess.cursor_para)].text.size());
        int a = std::clamp(sess.cursor_col, 0, len);
        int b = std::min(a + 1, len);
        if (b > a) ToggleFormatOverRange(sess.doc.paragraphs[static_cast<size_t>(sess.cursor_para)], a, b, field);
    }
    sess.modified = true;
}

bool Editor::OfficeFormatActive(char which) const {
    auto it = officedocs_.find(CurPane().buffer_id);
    if (it == officedocs_.end()) return false;
    const OfficeSession &sess = it->second;
    int para_count = static_cast<int>(sess.doc.paragraphs.size());
    if (para_count <= 0) return false;
    /**
     * @brief Selects which DocFormat boolean field corresponds to the requested format character.
     * @param f The format record to read.
     * @return The value of the bold/italic/strike/underline field matching `which`.
     */
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
    col = std::min(col, static_cast<int>(sess.doc.paragraphs[static_cast<size_t>(cp)].text.size()));
    return field_of(FormatAt(sess.doc.paragraphs[static_cast<size_t>(cp)], col));
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
            SetFormatFieldOverRange(sess.doc.paragraphs[static_cast<size_t>(pa)], ca, cb, apply);
        } else {
            SetFormatFieldOverRange(sess.doc.paragraphs[static_cast<size_t>(pa)], ca, static_cast<int>(sess.doc.paragraphs[static_cast<size_t>(pa)].text.size()),
                                     apply);
            for (int pi = pa + 1; pi < pb; pi++) {
                SetFormatFieldOverRange(sess.doc.paragraphs[static_cast<size_t>(pi)], 0, static_cast<int>(sess.doc.paragraphs[static_cast<size_t>(pi)].text.size()),
                                         apply);
            }
            SetFormatFieldOverRange(sess.doc.paragraphs[static_cast<size_t>(pb)], 0, cb, apply);
        }
        sess.has_selection = false;
        sess.cursor_para = pa;
        sess.cursor_col = ca;
        mode_ = Mode::OfficeNormal;
    } else {
        int len = static_cast<int>(sess.doc.paragraphs[static_cast<size_t>(sess.cursor_para)].text.size());
        int a = std::clamp(sess.cursor_col, 0, len);
        int b = std::min(a + 1, len);
        if (b > a) SetFormatFieldOverRange(sess.doc.paragraphs[static_cast<size_t>(sess.cursor_para)], a, b, apply);
    }
    sess.modified = true;
}

void Editor::SetOfficeFontFamily(OfficeFontFamily family) {
    // Sets the font family field on every format record in the selection (or current char).
    ApplyOfficeFormatFieldOverSelection([family](DocFormat &f) { f.font_family = family; });
}

void Editor::SetOfficeFontSizePt(float pt) {
    // Sets the font size (in points) field on every format record in the selection (or current char).
    ApplyOfficeFormatFieldOverSelection([pt](DocFormat &f) { f.font_size_pt = pt; });
}

void Editor::SetOfficeColor(unsigned char r, unsigned char g, unsigned char b) {
    // Enables and sets the explicit text color on every format record in the selection (or current char).
    ApplyOfficeFormatFieldOverSelection([r, g, b](DocFormat &f) {
        f.has_color = true;
        f.color_r = r; f.color_g = g; f.color_b = b;
    });
}

void Editor::ClearOfficeColor() {
    // Clears the explicit text color flag on every format record in the selection (or current char).
    ApplyOfficeFormatFieldOverSelection([](DocFormat &f) { f.has_color = false; });
}

void Editor::SetOfficeHighlight(unsigned char r, unsigned char g, unsigned char b) {
    // Enables and sets the highlight color on every format record in the selection (or current char).
    ApplyOfficeFormatFieldOverSelection([r, g, b](DocFormat &f) {
        f.has_highlight = true;
        f.highlight_r = r; f.highlight_g = g; f.highlight_b = b;
    });
}

void Editor::ClearOfficeHighlight() {
    // Clears the highlight color flag on every format record in the selection (or current char).
    ApplyOfficeFormatFieldOverSelection([](DocFormat &f) { f.has_highlight = false; });
}

void Editor::ToggleOfficeSuperscript() {
    // Reads the current state once (single-char/first-selection-char
    // sample, same approximation OfficeFormatActive's own comment
    // documents) so the whole selection ends up in one consistent
    // resulting state rather than each character flipping independently.
    bool active = OfficeSuperscriptActiveInternal(true);
    // Flips superscript to the opposite of its sampled state and clears subscript (mutually exclusive).
    ApplyOfficeFormatFieldOverSelection([active](DocFormat &f) {
        f.superscript = !active;
        f.subscript = false;
    });
}

void Editor::ToggleOfficeSubscript() {
    bool active = OfficeSuperscriptActiveInternal(false);
    // Flips subscript to the opposite of its sampled state and clears superscript (mutually exclusive).
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
    col = std::min(col, static_cast<int>(sess.doc.paragraphs[static_cast<size_t>(cp)].text.size()));
    DocFormat f = FormatAt(sess.doc.paragraphs[static_cast<size_t>(cp)], col);
    return super ? f.superscript : f.subscript;
}

void Editor::SetOfficeAlignment(DocParagraph::Align align) {
    auto it = officedocs_.find(CurPane().buffer_id);
    if (it == officedocs_.end()) return;
    OfficeSession &sess = it->second;
    if (sess.doc.paragraphs.empty()) return;
    PushUndoOffice();
    // cppcheck-suppress duplicateAssignExpression
    // Intentional: range starts as the single-point [cursor, cursor]
    // before being widened below when there's an active selection.
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
    return cp >= 0 && sess.doc.paragraphs[static_cast<size_t>(cp)].align == align;
}

void Editor::SetOfficeListKind(DocParagraph::ListKind kind) {
    auto it = officedocs_.find(CurPane().buffer_id);
    if (it == officedocs_.end()) return;
    OfficeSession &sess = it->second;
    if (sess.doc.paragraphs.empty()) return;
    PushUndoOffice();
    // cppcheck-suppress duplicateAssignExpression
    // Intentional: range starts as the single-point [cursor, cursor]
    // before being widened below when there's an active selection.
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
    return cp >= 0 && sess.doc.paragraphs[static_cast<size_t>(cp)].list_kind == kind;
}

void Editor::InsertOfficeText(const std::string &utf8) {
    auto it = officedocs_.find(CurPane().buffer_id);
    if (it == officedocs_.end()) return;
    OfficeSession &sess = it->second;
    if (sess.doc.paragraphs.empty() || utf8.empty()) return;
    PushUndoOffice();
    DocParagraph &p = sess.doc.paragraphs[static_cast<size_t>(sess.cursor_para)];
    ApplyInsertToParagraph(p, sess.cursor_col, utf8);
    sess.cursor_col += static_cast<int>(utf8.size());
    sess.modified = true;
}

void Editor::InsertOfficeMath() {
    if (!IsOfficeBuffer(CurPane().buffer_id)) return;
    int buffer_id = CurPane().buffer_id;
    // Once the LaTeX text is entered, inserts it into the paragraph as a new math-formatted span.
    BeginPromptNative("Insert math (LaTeX):", "", [this, buffer_id](const std::string &latex) {
        if (latex.empty()) return;
        auto it = officedocs_.find(buffer_id);
        if (it == officedocs_.end()) return;
        OfficeSession &sess = it->second;
        if (sess.doc.paragraphs.empty()) return;
        PushUndoOffice();
        DocParagraph &p = sess.doc.paragraphs[static_cast<size_t>(sess.cursor_para)];
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
    // Once the row count is entered, prompts for the column count.
    BeginPromptNative("Insert table (rows):", "2", [this, buffer_id](const std::string &rows_str) {
        int rows = std::clamp(std::atoi(rows_str.c_str()), 1, 50);
        // Once the column count is entered, builds an empty rows x cols table and enters it for editing.
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
            sess.doc.paragraphs[static_cast<size_t>(sess.cursor_para)].table_ref = table_ref;
            sess.modified = true;
            EnterOfficeTable(table_ref);
        });
    });
}

void Editor::InsertOfficeImagePrompt() {
    if (!IsOfficeBuffer(CurPane().buffer_id)) return;
    int buffer_id = CurPane().buffer_id;
    // Once a path is entered, decodes it and inserts it as a new image anchor on the current paragraph.
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
        sess.doc.paragraphs[static_cast<size_t>(sess.cursor_para)].image_ref = static_cast<int>(sess.doc.images.size()) - 1;
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
        if (!BufferInActiveWorkspace(static_cast<int>(i))) continue;
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
        buffers_[static_cast<size_t>(buffer_id)].filename = path;
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
    const Sheet &sh = sess->wb.sheets[static_cast<size_t>(sess->active_sheet)];

    // 2D grid navigation -- h/l move columns, j/k move rows (unlike
    // Office's 1D paragraph navigation). No upper clamp beyond 0 --
    // hjkl can freely move past the sheet's current "used range" so the
    // user can navigate to an empty cell to start typing new data; gg/G/
    // 0/$ below jump specifically to the used-range corners.
    /**
     * @brief Checks whether a key was just pressed or is auto-repeating.
     * @param key The GLFW/raylib key code to check.
     * @return True if the key is freshly pressed or repeating this frame.
     */
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
    if (ctrl && held(KEY_D)) {
        sess->cursor_row += std::max(1, sess->viewport_h / kSheetRowHeight / 2);
    }
    if (ctrl && held(KEY_U)) {
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
/**
 * @brief Clamps the sheet cursor and active sheet index back into the swapped-in workbook's valid range after undo/redo.
 * @param sess The sheet session whose cursor/active_sheet were just swapped.
 */
void ClampSheetCursorAfterSwap(SheetSession &sess) {
    if (sess.wb.sheets.empty()) return;
    sess.active_sheet = std::clamp(sess.active_sheet, 0, static_cast<int>(sess.wb.sheets.size()) - 1);
    const Sheet &sh = sess.wb.sheets[static_cast<size_t>(sess.active_sheet)];
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
    status_message_ = "-- " + sess.wb.sheets[static_cast<size_t>(sess.active_sheet)].name + " --";
}

void Editor::PrevSheet() {
    auto it = sheetdocs_.find(CurPane().buffer_id);
    if (it == sheetdocs_.end()) return;
    SheetSession &sess = it->second;
    if (sess.wb.sheets.size() < 2) return;
    int n = static_cast<int>(sess.wb.sheets.size());
    sess.active_sheet = (sess.active_sheet - 1 + n) % n;
    ClampSheetCursorAfterSwap(sess);
    status_message_ = "-- " + sess.wb.sheets[static_cast<size_t>(sess.active_sheet)].name + " --";
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
    /**
     * @brief Writes the in-progress edit buffer into the current cell and returns to SheetNormal mode.
     */
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
    const Sheet &sh = sess->wb.sheets[static_cast<size_t>(sess->active_sheet)];

    /**
     * @brief Checks whether a key was just pressed or is auto-repeating.
     * @param key The GLFW/raylib key code to check.
     * @return True if the key is freshly pressed or repeating this frame.
     */
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

// --- Kanban board / Gantt chart panes -----------------------------------
//
// Unlike every doc-type block above, a Kanban/Gantt "session" does NOT
// stand in for Buffer::lines the way SheetSession/OfficeSession/PdfSession
// do (those buffers' `lines` stay a dummy single empty line forever) --
// Buf().lines here stays the buffer's real, saved org text throughout; a
// KanbanSession/GanttSession is a disposable parsed-OrgOutline + UI-state
// cache layered over it (see org_doc.h's top comment). Every mutation
// method below therefore goes through the SAME PushUndo()-backed
// primitives an ordinary text edit would (ReplaceLinesForLua, ExMoveOrCopy)
// rather than a separate snapshot stack like PushUndoSheet/PushUndoOffice,
// and always finishes by re-parsing `outline` from the fresh Buf().lines --
// never a partial in-memory patch, so a stray index can't drift out of
// sync with the text it describes.

bool Editor::IsKanbanViewActive(int buffer_id) const {
    auto it = org_view_mode_.find(buffer_id);
    return it != org_view_mode_.end() && it->second == OrgViewMode::Kanban;
}

bool Editor::IsGanttViewActive(int buffer_id) const {
    auto it = org_view_mode_.find(buffer_id);
    return it != org_view_mode_.end() && it->second == OrgViewMode::Gantt;
}

const KanbanSession *Editor::GetKanban(int buffer_id) const {
    auto it = kanban_views_.find(buffer_id);
    return it == kanban_views_.end() ? nullptr : &it->second;
}

KanbanSession *Editor::GetKanbanMutable(int buffer_id) {
    auto it = kanban_views_.find(buffer_id);
    return it == kanban_views_.end() ? nullptr : &it->second;
}

const GanttSession *Editor::GetGantt(int buffer_id) const {
    auto it = gantt_views_.find(buffer_id);
    return it == gantt_views_.end() ? nullptr : &it->second;
}

GanttSession *Editor::GetGanttMutable(int buffer_id) {
    auto it = gantt_views_.find(buffer_id);
    return it == gantt_views_.end() ? nullptr : &it->second;
}

std::vector<std::string> Editor::KanbanColumns(int buffer_id) const {
    auto it = kanban_views_.find(buffer_id);
    if (it == kanban_views_.end()) return {};
    std::vector<std::string> cols = it->second.outline.todo_keywords;
    cols.insert(cols.end(), it->second.outline.done_keywords.begin(), it->second.outline.done_keywords.end());
    return cols;
}

std::vector<int> Editor::KanbanCardsInColumn(int buffer_id, int column_index) const {
    std::vector<int> out;
    auto it = kanban_views_.find(buffer_id);
    if (it == kanban_views_.end()) return out;
    std::vector<std::string> cols = KanbanColumns(buffer_id);
    if (column_index < 0 || column_index >= static_cast<int>(cols.size())) return out;
    const std::string &kw = cols[static_cast<size_t>(column_index)];
    const OrgOutline &outline = it->second.outline;
    for (size_t i = 0; i < outline.headlines.size(); i++) {
        if (outline.headlines[i].todo_keyword == kw) out.push_back(static_cast<int>(i));
    }
    return out;
}

std::vector<int> Editor::GanttRows(int buffer_id) const {
    std::vector<int> out;
    auto it = gantt_views_.find(buffer_id);
    if (it == gantt_views_.end()) return out;
    const OrgOutline &outline = it->second.outline;
    for (size_t i = 0; i < outline.headlines.size(); i++) {
        if (!outline.headlines[i].scheduled.present) continue;
        bool hidden = false;
        for (int parent = outline.headlines[i].parent_index; parent >= 0; parent = outline.headlines[static_cast<size_t>(parent)].parent_index) {
            if (it->second.collapsed_headlines.count(parent)) { hidden = true; break; }
        }
        if (!hidden) out.push_back(static_cast<int>(i));
    }
    return out;
}

void Editor::OpenKanbanView() {
    if (!IsOrgBuffer()) {
        status_message_ = "Kanban view requires a .org buffer";
        return;
    }
    int buffer_id = CurPane().buffer_id;
    KanbanSession &sess = kanban_views_[buffer_id];  // keeps prior UI state if this buffer's already been viewed
    sess.buffer_id = buffer_id;
    sess.outline = ParseOrgOutline(Buf().lines);
    sess.editing = false;
    org_view_mode_[buffer_id] = OrgViewMode::Kanban;
    pending_g_ = false;
    status_message_.clear();
    SyncModeToActivePaneBuffer();
}

void Editor::OpenGanttView() {
    if (!IsOrgBuffer()) {
        status_message_ = "Gantt view requires a .org buffer";
        return;
    }
    int buffer_id = CurPane().buffer_id;
    GanttSession &sess = gantt_views_[buffer_id];
    sess.buffer_id = buffer_id;
    sess.outline = ParseOrgOutline(Buf().lines);
    if (sess.anchor_day == 0) {
        // First-ever open for this buffer (a freshly default-constructed
        // session): center the initial view a few days before the
        // earliest SCHEDULED date found, rather than the 1970 epoch. A
        // later toggle back in (sess already existed) keeps whatever
        // anchor_day the user had already panned to.
        long long earliest = 0;
        bool found = false;
        for (const auto &h : sess.outline.headlines) {
            if (!h.scheduled.present) continue;
            long long d = OrgDayNumber(h.scheduled.year, h.scheduled.month, h.scheduled.day);
            if (!found || d < earliest) {
                earliest = d;
                found = true;
            }
        }
        if (found) sess.anchor_day = earliest - 3;
    }
    org_view_mode_[buffer_id] = OrgViewMode::Gantt;
    pending_g_ = false;
    status_message_.clear();
    SyncModeToActivePaneBuffer();
}

void Editor::CloseOrgView() {
    org_view_mode_[CurPane().buffer_id] = OrgViewMode::Text;
    pending_g_ = false;
    SyncModeToActivePaneBuffer();
}

void Editor::KanbanSetCardColumn(int headline_index, const std::string &new_keyword) {
    auto it = kanban_views_.find(CurPane().buffer_id);
    if (it == kanban_views_.end()) return;
    KanbanSession &sess = it->second;
    if (headline_index < 0 || headline_index >= static_cast<int>(sess.outline.headlines.size())) return;
    const OrgHeadline &h = sess.outline.headlines[static_cast<size_t>(headline_index)];
    std::string new_line = RewriteHeadlineKeyword(Buf().lines[static_cast<size_t>(h.line_start)], new_keyword, sess.outline.todo_keywords,
                                                   sess.outline.done_keywords);
    ReplaceLinesForLua(h.line_start, h.line_start + 1, {new_line});
    sess.outline = ParseOrgOutline(Buf().lines);
}

void Editor::KanbanMoveCardBefore(int headline_index, int before_headline_index) {
    auto it = kanban_views_.find(CurPane().buffer_id);
    if (it == kanban_views_.end()) return;
    KanbanSession &sess = it->second;
    int n = static_cast<int>(sess.outline.headlines.size());
    if (headline_index < 0 || headline_index >= n || before_headline_index == headline_index) return;
    const OrgHeadline &h = sess.outline.headlines[static_cast<size_t>(headline_index)];
    int dest_row;
    if (before_headline_index < 0 || before_headline_index >= n) {
        dest_row = Buf().LineCount() - 1;
    } else {
        const OrgHeadline &target = sess.outline.headlines[static_cast<size_t>(before_headline_index)];
        if (target.line_start >= h.line_start && target.line_start <= h.line_end) return;  // inside its own subtree
        dest_row = target.line_start - 1;
    }
    ExMoveOrCopy(h.line_start, h.line_end, dest_row, /*is_copy=*/false);
    sess.outline = ParseOrgOutline(Buf().lines);
}

void Editor::KanbanRenameCard(int headline_index, const std::string &new_title) {
    auto it = kanban_views_.find(CurPane().buffer_id);
    if (it == kanban_views_.end()) return;
    KanbanSession &sess = it->second;
    if (headline_index < 0 || headline_index >= static_cast<int>(sess.outline.headlines.size())) return;
    const OrgHeadline &h = sess.outline.headlines[static_cast<size_t>(headline_index)];
    std::string new_line = FormatHeadlineLine(h.level, h.todo_keyword, h.priority, new_title, h.tags);
    ReplaceLinesForLua(h.line_start, h.line_start + 1, {new_line});
    sess.outline = ParseOrgOutline(Buf().lines);
}

int Editor::KanbanNewCard(const std::string &column_keyword, const std::string &title) {
    auto it = kanban_views_.find(CurPane().buffer_id);
    if (it == kanban_views_.end()) return -1;
    KanbanSession &sess = it->second;
    std::string new_line = FormatHeadlineLine(1, column_keyword, 0, title, {});
    int end = Buf().LineCount();
    ReplaceLinesForLua(end, end, {new_line});
    sess.outline = ParseOrgOutline(Buf().lines);
    return static_cast<int>(sess.outline.headlines.size()) - 1;
}

void Editor::KanbanBeginRenameNewCard(int headline_index) {
    KanbanSession *sess = GetKanbanMutable(CurPane().buffer_id);
    if (!sess || headline_index < 0 || headline_index >= static_cast<int>(sess->outline.headlines.size())) return;
    sess->editing_headline_index = headline_index;
    sess->edit_buffer.clear();
    sess->edit_cursor = 0;
    sess->editing = true;
    mode_ = Mode::KanbanInsert;
}

void Editor::KanbanDeleteCard(int headline_index) {
    auto it = kanban_views_.find(CurPane().buffer_id);
    if (it == kanban_views_.end()) return;
    KanbanSession &sess = it->second;
    if (headline_index < 0 || headline_index >= static_cast<int>(sess.outline.headlines.size())) return;
    const OrgHeadline &h = sess.outline.headlines[static_cast<size_t>(headline_index)];
    ReplaceLinesForLua(h.line_start, h.line_end + 1, {});
    sess.outline = ParseOrgOutline(Buf().lines);
}

void Editor::EnsureOrgTodoLine() {
    auto it = kanban_views_.find(CurPane().buffer_id);
    if (it == kanban_views_.end()) return;
    if (FindOrgTodoLineIndex(Buf().lines) >= 0) return;
    ReplaceLinesForLua(0, 0, {FormatTodoLine({"TODO"}, {"DONE"})});
    it->second.outline = ParseOrgOutline(Buf().lines);
}

int Editor::KanbanAddColumn(const std::string &name) {
    EnsureOrgTodoLine();
    auto it = kanban_views_.find(CurPane().buffer_id);
    if (it == kanban_views_.end()) return -1;
    KanbanSession &sess = it->second;
    for (const auto &kw : KanbanColumns(CurPane().buffer_id)) {
        if (kw == name) {
            status_message_ = "Column \"" + name + "\" already exists";
            return -1;
        }
    }
    int line_idx = FindOrgTodoLineIndex(Buf().lines);
    if (line_idx < 0) return -1;
    std::vector<std::string> new_todo = sess.outline.todo_keywords;
    new_todo.push_back(name);
    ReplaceLinesForLua(line_idx, line_idx + 1, {FormatTodoLine(new_todo, sess.outline.done_keywords)});
    sess.outline = ParseOrgOutline(Buf().lines);
    status_message_.clear();
    return static_cast<int>(new_todo.size()) - 1;
}

void Editor::KanbanRenameColumn(int column_index, const std::string &new_name) {
    EnsureOrgTodoLine();
    auto it = kanban_views_.find(CurPane().buffer_id);
    if (it == kanban_views_.end()) return;
    KanbanSession &sess = it->second;
    std::vector<std::string> columns = KanbanColumns(CurPane().buffer_id);
    if (column_index < 0 || column_index >= static_cast<int>(columns.size()) || new_name.empty()) return;
    const std::string old_name = columns[static_cast<size_t>(column_index)];
    if (old_name == new_name) return;
    for (const auto &kw : columns) {
        if (kw == new_name) {
            status_message_ = "Column \"" + new_name + "\" already exists";
            return;
        }
    }

    // Capture the OLD keyword lists before mutating anything -- every
    // existing headline's line still literally contains the old token, so
    // RewriteHeadlineKeyword needs the OLD lists to recognize and relocate
    // it (same reasoning KanbanSetCardColumn already relies on).
    const std::vector<std::string> old_todo = sess.outline.todo_keywords;
    const std::vector<std::string> old_done = sess.outline.done_keywords;
    for (const OrgHeadline &h : sess.outline.headlines) {
        if (h.todo_keyword != old_name) continue;
        std::string new_line = RewriteHeadlineKeyword(Buf().lines[static_cast<size_t>(h.line_start)], new_name, old_todo, old_done);
        ReplaceLinesForLua(h.line_start, h.line_start + 1, {new_line});
    }

    bool todo_side = column_index < static_cast<int>(old_todo.size());
    std::vector<std::string> new_todo = old_todo, new_done = old_done;
    if (todo_side) {
        new_todo[static_cast<size_t>(column_index)] = new_name;
    } else {
        new_done[static_cast<size_t>(column_index - static_cast<int>(old_todo.size()))] = new_name;
    }
    int line_idx = FindOrgTodoLineIndex(Buf().lines);
    if (line_idx >= 0) ReplaceLinesForLua(line_idx, line_idx + 1, {FormatTodoLine(new_todo, new_done)});
    sess.outline = ParseOrgOutline(Buf().lines);
    status_message_.clear();
}

void Editor::KanbanMoveColumn(int column_index, int before_column) {
    // A board with no explicit sequence still renders the default TODO/DONE
    // columns. Materialize that sequence before reordering it so the drag is
    // persistent for those ordinary Org files too.
    EnsureOrgTodoLine();
    auto it = kanban_views_.find(CurPane().buffer_id);
    if (it == kanban_views_.end()) return;
    KanbanSession &sess = it->second;
    std::vector<std::string> columns = KanbanColumns(CurPane().buffer_id);
    int count = static_cast<int>(columns.size());
    if (column_index < 0 || column_index >= count || before_column < 0 || before_column > count ||
        before_column == column_index || before_column == column_index + 1) {
        return;
    }

    // `before_column` names a gap in the pre-move ordering. Erasing an item
    // to its left shifts that gap left by one.
    std::string moved = columns[static_cast<size_t>(column_index)];
    columns.erase(columns.begin() + column_index);
    if (before_column > column_index) --before_column;
    columns.insert(columns.begin() + before_column, std::move(moved));

    // Org's TODO syntax has one fixed separator. Keeping it at the same
    // display position lets the board represent the exact dragged order;
    // a keyword crossing that separator intentionally becomes TODO/DONE.
    int todo_count = static_cast<int>(sess.outline.todo_keywords.size());
    std::vector<std::string> new_todo(columns.begin(), columns.begin() + todo_count);
    std::vector<std::string> new_done(columns.begin() + todo_count, columns.end());
    int line_idx = FindOrgTodoLineIndex(Buf().lines);
    if (line_idx < 0) return;
    ReplaceLinesForLua(line_idx, line_idx + 1, {FormatTodoLine(new_todo, new_done)});
    sess.outline = ParseOrgOutline(Buf().lines);
    sess.focused_column = before_column;
    sess.focused_row = 0;
    status_message_.clear();
}

void Editor::KanbanDeleteColumn(int column_index) {
    EnsureOrgTodoLine();
    int buffer_id = CurPane().buffer_id;
    auto it = kanban_views_.find(buffer_id);
    if (it == kanban_views_.end()) return;
    KanbanSession &sess = it->second;
    std::vector<std::string> columns = KanbanColumns(buffer_id);
    if (column_index < 0 || column_index >= static_cast<int>(columns.size())) return;
    if (columns.size() <= 1) {
        status_message_ = "Can't delete the last column";
        return;
    }
    std::vector<int> cards = KanbanCardsInColumn(buffer_id, column_index);
    if (!cards.empty()) {
        status_message_ = "Move " + std::to_string(cards.size()) + " card(s) out of \"" + columns[static_cast<size_t>(column_index)] +
                           "\" before deleting it";
        return;
    }
    std::vector<std::string> new_todo = sess.outline.todo_keywords, new_done = sess.outline.done_keywords;
    bool todo_side = column_index < static_cast<int>(new_todo.size());
    if (todo_side) {
        new_todo.erase(new_todo.begin() + column_index);
    } else {
        new_done.erase(new_done.begin() + (column_index - static_cast<int>(sess.outline.todo_keywords.size())));
    }
    int line_idx = FindOrgTodoLineIndex(Buf().lines);
    if (line_idx < 0) return;
    ReplaceLinesForLua(line_idx, line_idx + 1, {FormatTodoLine(new_todo, new_done)});
    sess.outline = ParseOrgOutline(Buf().lines);
    status_message_.clear();
}

void Editor::GanttShiftHeadline(int headline_index, int delta_days) {
    auto it = gantt_views_.find(CurPane().buffer_id);
    if (it == gantt_views_.end()) return;
    GanttSession &sess = it->second;
    if (headline_index < 0 || headline_index >= static_cast<int>(sess.outline.headlines.size())) return;
    const OrgHeadline &h = sess.outline.headlines[static_cast<size_t>(headline_index)];
    if (h.planning_line < 0) return;
    std::string line = Buf().lines[static_cast<size_t>(h.planning_line)];
    if (h.scheduled.present) line = RewriteTimestampInLine(line, false, ShiftTimestamp(h.scheduled, delta_days));
    if (h.deadline.present) line = RewriteTimestampInLine(line, true, ShiftTimestamp(h.deadline, delta_days));
    ReplaceLinesForLua(h.planning_line, h.planning_line + 1, {line});
    sess.outline = ParseOrgOutline(Buf().lines);
}

void Editor::GanttSetHeadlineDate(int headline_index, bool is_deadline, long long new_day) {
    auto it = gantt_views_.find(CurPane().buffer_id);
    if (it == gantt_views_.end()) return;
    GanttSession &sess = it->second;
    if (headline_index < 0 || headline_index >= static_cast<int>(sess.outline.headlines.size())) return;
    const OrgHeadline &h = sess.outline.headlines[static_cast<size_t>(headline_index)];
    if (h.planning_line < 0 || !h.scheduled.present) return;

    if (!is_deadline) {
        if (h.deadline.present) {
            long long deadline_day = OrgDayNumber(h.deadline.year, h.deadline.month, h.deadline.day);
            if (new_day >= deadline_day) return;  // start can't cross past the end
        }
        OrgTimestamp new_ts = h.scheduled;
        OrgDateFromDayNumber(new_day, new_ts.year, new_ts.month, new_ts.day);
        std::string line = RewriteTimestampInLine(Buf().lines[static_cast<size_t>(h.planning_line)], false, new_ts);
        ReplaceLinesForLua(h.planning_line, h.planning_line + 1, {line});
    } else {
        long long scheduled_day = OrgDayNumber(h.scheduled.year, h.scheduled.month, h.scheduled.day);
        if (new_day <= scheduled_day) return;  // end can't cross before the start
        OrgTimestamp new_ts = h.deadline.present ? h.deadline : OrgTimestamp{};
        OrgDateFromDayNumber(new_day, new_ts.year, new_ts.month, new_ts.day);
        new_ts.present = true;
        std::string line = Buf().lines[static_cast<size_t>(h.planning_line)];
        line = h.deadline.present ? RewriteTimestampInLine(line, true, new_ts)
                                   : (line + " DEADLINE: " + FormatOrgTimestamp(new_ts));
        ReplaceLinesForLua(h.planning_line, h.planning_line + 1, {line});
    }
    sess.outline = ParseOrgOutline(Buf().lines);
}

void Editor::GanttSetHeadlineProgress(int headline_index, int progress) {
    auto it = gantt_views_.find(CurPane().buffer_id);
    if (it == gantt_views_.end()) return;
    GanttSession &sess = it->second;
    if (headline_index < 0 || headline_index >= static_cast<int>(sess.outline.headlines.size())) return;
    progress = std::clamp(progress, 0, 100);
    const OrgHeadline &h = sess.outline.headlines[static_cast<size_t>(headline_index)];
    int insert_at = h.line_start + 1;
    if (h.planning_line >= 0) insert_at = h.planning_line + 1;
    /**
     * @brief Returns a line with its leading spaces removed.
     * @param line The line to strip.
     * @return The line's text starting at its first non-space character (empty if all spaces).
     */
    auto stripped = [](const std::string &line) {
        size_t first = line.find_first_not_of(' ');
        return first == std::string::npos ? std::string() : line.substr(first);
    };
    /**
     * @brief Checks whether a (property-drawer) line declares the PROGRESS property.
     * @param line The line to check.
     * @return True if the line, ignoring leading spaces, starts with ":PROGRESS:" (case-insensitive).
     */
    auto is_progress_property = [&](const std::string &line) {
        std::string text = stripped(line);
        return text.rfind(":PROGRESS:", 0) == 0 || text.rfind(":progress:", 0) == 0;
    };
    if (insert_at < Buf().LineCount() && stripped(Buf().lines[static_cast<size_t>(insert_at)]) == ":PROPERTIES:") {
        for (int line = insert_at + 1; line < Buf().LineCount() && stripped(Buf().lines[static_cast<size_t>(line)]) != ":END:"; ++line) {
            if (is_progress_property(Buf().lines[static_cast<size_t>(line)])) {
                ReplaceLinesForLua(line, line + 1, {":PROGRESS: " + std::to_string(progress)});
                sess.outline = ParseOrgOutline(Buf().lines);
                return;
            }
        }
        int end = insert_at + 1;
        while (end < Buf().LineCount() && stripped(Buf().lines[static_cast<size_t>(end)]) != ":END:") ++end;
        ReplaceLinesForLua(end, end, {":PROGRESS: " + std::to_string(progress)});
    } else {
        ReplaceLinesForLua(insert_at, insert_at, {":PROPERTIES:", ":PROGRESS: " + std::to_string(progress), ":END:"});
    }
    sess.outline = ParseOrgOutline(Buf().lines);
}

void Editor::HandleKanbanNormalInput() {
    KanbanSession *sess = nullptr;
    {
        auto it = kanban_views_.find(CurPane().buffer_id);
        if (it == kanban_views_.end()) {
            mode_ = Mode::Normal;
            return;
        }
        sess = &it->second;
    }
    int buffer_id = CurPane().buffer_id;
    std::vector<std::string> columns = KanbanColumns(buffer_id);
    if (columns.empty()) return;
    sess->focused_column = std::clamp(sess->focused_column, 0, static_cast<int>(columns.size()) - 1);
    std::vector<int> cards = KanbanCardsInColumn(buffer_id, sess->focused_column);
    sess->focused_row = cards.empty() ? 0 : std::clamp(sess->focused_row, 0, static_cast<int>(cards.size()) - 1);

    /**
     * @brief Checks whether a key was just pressed or is auto-repeating.
     * @param key The GLFW/raylib key code to check.
     * @return True if the key is freshly pressed or repeating this frame.
     */
    auto held = [](int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); };
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    // Shift+H/L moves the focused *card* into the adjacent column (the same
    // KanbanSetCardColumn a cross-column drag-release already commits);
    // plain H/L (or the arrows, which have no shift variant to conflict
    // with) just moves focus. Checked as its own branch rather than
    // alongside the plain moves below since both would otherwise fire off
    // the same physical H/L keypress.
    if (shift && held(KEY_H) && !cards.empty() && sess->focused_column > 0) {
        int hi = cards[static_cast<size_t>(sess->focused_row)];
        int desired_row = sess->focused_row;
        int new_column = sess->focused_column - 1;
        KanbanSetCardColumn(hi, columns[static_cast<size_t>(new_column)]);
        sess->focused_column = new_column;
        std::vector<int> moved_cards = KanbanCardsInColumn(buffer_id, new_column);
        // Horizontal movement keeps the cursor's board row. The moved card
        // may occupy a different document-order slot in its new column, but
        // navigating the board should not jump the user's focus to row 0.
        sess->focused_row = moved_cards.empty() ? 0 : std::min(desired_row, static_cast<int>(moved_cards.size()) - 1);
        return;
    }
    if (shift && held(KEY_L) && !cards.empty() && sess->focused_column + 1 < static_cast<int>(columns.size())) {
        int hi = cards[static_cast<size_t>(sess->focused_row)];
        int desired_row = sess->focused_row;
        int new_column = sess->focused_column + 1;
        KanbanSetCardColumn(hi, columns[static_cast<size_t>(new_column)]);
        sess->focused_column = new_column;
        std::vector<int> moved_cards = KanbanCardsInColumn(buffer_id, new_column);
        sess->focused_row = moved_cards.empty() ? 0 : std::min(desired_row, static_cast<int>(moved_cards.size()) - 1);
        return;
    }
    if (!shift && (held(KEY_H) || held(KEY_LEFT))) {
        sess->focused_column = std::max(0, sess->focused_column - 1);
        sess->focused_row = 0;
    }
    if (!shift && (held(KEY_L) || held(KEY_RIGHT))) {
        sess->focused_column = std::min(static_cast<int>(columns.size()) - 1, sess->focused_column + 1);
        sess->focused_row = 0;
    }
    if (held(KEY_J) || held(KEY_DOWN)) {
        if (!cards.empty()) sess->focused_row = std::min(static_cast<int>(cards.size()) - 1, sess->focused_row + 1);
    }
    if (held(KEY_K) || held(KEY_UP)) sess->focused_row = std::max(0, sess->focused_row - 1);

    int cp = GetCharPressed();
    while (cp > 0) {
        if (cp == ':') {
            EnterCommand();
            return;
        } else if (cp == static_cast<int>(leader_key_) && !whichkey_bindings_.empty()) {
            TriggerWhichKey();
            return;
        } else if (cp == 'i' && !cards.empty()) {
            int hi = cards[static_cast<size_t>(sess->focused_row)];
            sess->editing_headline_index = hi;
            sess->edit_buffer = sess->outline.headlines[static_cast<size_t>(hi)].title;
            sess->edit_cursor = static_cast<int>(sess->edit_buffer.size());
            sess->editing = true;
            mode_ = Mode::KanbanInsert;
            return;
        } else if (cp == 'n') {
            // Captured by value into the lambda below, which escapes this
            // function's scope (async prompt callback); the checker's
            // "only used as const reference" analysis doesn't see that.
            // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
            std::string column_keyword = columns[static_cast<size_t>(sess->focused_column)];
            // Once a title is entered, creates a new card in the focused column with that title.
            BeginPromptNative("New card title", "", [this, column_keyword](const std::string &title) {
                if (!title.empty()) KanbanNewCard(column_keyword, title);
            });
            return;
        } else if ((cp == 'x' || cp == 'd') && !cards.empty()) {
            KanbanDeleteCard(cards[static_cast<size_t>(sess->focused_row)]);
            cards = KanbanCardsInColumn(buffer_id, sess->focused_column);
            sess->focused_row = cards.empty() ? 0 : std::clamp(sess->focused_row, 0, static_cast<int>(cards.size()) - 1);
        } else if (cp == 'u') {
            Undo();
            sess->outline = ParseOrgOutline(Buf().lines);
        }
        cp = GetCharPressed();
    }
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (ctrl && IsKeyPressed(KEY_R)) {
        Redo();
        sess->outline = ParseOrgOutline(Buf().lines);
    }
}

void Editor::HandleKanbanInsertInput() {
    KanbanSession *sess = nullptr;
    {
        auto it = kanban_views_.find(CurPane().buffer_id);
        if (it == kanban_views_.end()) {
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
    if (escape) {
        sess->editing = false;
        mode_ = Mode::KanbanNormal;
        return;
    }
    if (enter) {
        int hi = sess->editing_headline_index;
        std::string title = sess->edit_buffer;
        sess->editing = false;
        mode_ = Mode::KanbanNormal;
        if (hi >= 0) KanbanRenameCard(hi, title);
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

void Editor::HandleGanttNormalInput() {
    GanttSession *sess = nullptr;
    {
        auto it = gantt_views_.find(CurPane().buffer_id);
        if (it == gantt_views_.end()) {
            mode_ = Mode::Normal;
            return;
        }
        sess = &it->second;
    }
    int buffer_id = CurPane().buffer_id;
    std::vector<int> rows = GanttRows(buffer_id);
    if (!rows.empty()) sess->focused_row = std::clamp(sess->focused_row, 0, static_cast<int>(rows.size()) - 1);

    /**
     * @brief Checks whether a key was just pressed or is auto-repeating.
     * @param key The GLFW/raylib key code to check.
     * @return True if the key is freshly pressed or repeating this frame.
     */
    auto held = [](int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); };
    if (held(KEY_J) || held(KEY_DOWN)) {
        if (!rows.empty()) sess->focused_row = std::min(static_cast<int>(rows.size()) - 1, sess->focused_row + 1);
    }
    if (held(KEY_K) || held(KEY_UP)) sess->focused_row = std::max(0, sess->focused_row - 1);
    if (held(KEY_H) || held(KEY_LEFT)) sess->anchor_day -= 1;
    if (held(KEY_L) || held(KEY_RIGHT)) sess->anchor_day += 1;

    /**
     * @brief Checks whether a headline has any child headlines in the current outline.
     * @param headline_index Index of the candidate parent headline.
     * @return True if some headline's parent_index equals headline_index.
     */
    auto has_children = [&](int headline_index) {
        for (const OrgHeadline &candidate : sess->outline.headlines) {
            if (candidate.parent_index == headline_index) return true;
        }
        return false;
    };
    /**
     * @brief Toggles the collapsed/expanded fold state of a headline, if it has children.
     * @param headline_index Index of the headline to toggle.
     */
    auto toggle_fold = [&](int headline_index) {
        if (!has_children(headline_index)) return;
        if (sess->collapsed_headlines.count(headline_index)) sess->collapsed_headlines.erase(headline_index);
        else sess->collapsed_headlines.insert(headline_index);
    };

    int cp = GetCharPressed();
    while (cp > 0) {
        if (sess->pending_fold_command) {
            sess->pending_fold_command = false;
            int focused = rows.empty() ? -1 : rows[static_cast<size_t>(sess->focused_row)];
            if ((cp == 'a' || cp == 'A') && focused >= 0) toggle_fold(focused);          // za
            else if (cp == 'c' && focused >= 0) { if (has_children(focused)) sess->collapsed_headlines.insert(focused); } // zc
            else if (cp == 'o' && focused >= 0) sess->collapsed_headlines.erase(focused); // zo
            else if (cp == 'M') {
                for (int i = 0; i < static_cast<int>(sess->outline.headlines.size()); ++i) {
                    if (has_children(i)) sess->collapsed_headlines.insert(i);
                }
            } else if (cp == 'R') {
                sess->collapsed_headlines.clear();
            } else if (cp == 'm') {
                int level = std::numeric_limits<int>::max();
                for (int i = 0; i < static_cast<int>(sess->outline.headlines.size()); ++i) {
                    if (has_children(i) && !sess->collapsed_headlines.count(i)) level = std::min(level, sess->outline.headlines[static_cast<size_t>(i)].level);
                }
                if (level != std::numeric_limits<int>::max()) {
                    for (int i = 0; i < static_cast<int>(sess->outline.headlines.size()); ++i) {
                        if (has_children(i) && sess->outline.headlines[static_cast<size_t>(i)].level == level) sess->collapsed_headlines.insert(i);
                    }
                }
            } else if (cp == 'r') {
                int level = std::numeric_limits<int>::max();
                for (int i : sess->collapsed_headlines) level = std::min(level, sess->outline.headlines[static_cast<size_t>(i)].level);
                if (level != std::numeric_limits<int>::max()) {
                    for (auto it = sess->collapsed_headlines.begin(); it != sess->collapsed_headlines.end();) {
                        if (sess->outline.headlines[static_cast<size_t>(*it)].level == level) it = sess->collapsed_headlines.erase(it);
                        else ++it;
                    }
                }
            }
        } else if (cp == 'z') {
            sess->pending_fold_command = true;
        } else if (cp == ':') {
            EnterCommand();
            return;
        } else if (cp == static_cast<int>(leader_key_) && !whichkey_bindings_.empty()) {
            TriggerWhichKey();
            return;
        } else if (cp == '+' || cp == '=') {
            sess->pixels_per_day = std::clamp(sess->pixels_per_day * 1.25f, 2.0f, 400.0f);
        } else if (cp == '-' || cp == '_') {
            sess->pixels_per_day = std::clamp(sess->pixels_per_day / 1.25f, 2.0f, 400.0f);
        } else if (cp == 'f') {
            long long first = 0, last = 0;
            bool have_dates = false;
            for (const OrgHeadline &h : sess->outline.headlines) {
                if (!h.scheduled.present) continue;
                long long start = OrgDayNumber(h.scheduled.year, h.scheduled.month, h.scheduled.day);
                long long end = h.deadline.present ? OrgDayNumber(h.deadline.year, h.deadline.month, h.deadline.day) : start;
                if (!have_dates) { first = start; last = end; have_dates = true; }
                else { first = std::min(first, start); last = std::max(last, end); }
            }
            if (have_dates && sess->content_w > sess->label_col_w) {
                sess->anchor_day = first - 2;
                sess->pixels_per_day = std::clamp((sess->content_w - sess->label_col_w) /
                                                       static_cast<float>(std::max(1LL, last - first + 5)),
                                                   2.0f, 400.0f);
                status_message_ = "Gantt: fit schedule";
            }
        } else if (cp == ' ' && !rows.empty()) {
            toggle_fold(rows[static_cast<size_t>(sess->focused_row)]);
        } else if (cp == 'p' && !rows.empty()) {
            int hi = rows[static_cast<size_t>(sess->focused_row)];
            std::string current = std::to_string(sess->outline.headlines[static_cast<size_t>(hi)].progress);
            // Once a numeric value is entered, sets the headline's progress percentage.
            BeginPromptNative("Progress (0-100)", current, [this, hi](const std::string &value) {
                char *end = nullptr;
                long parsed = std::strtol(value.c_str(), &end, 10);
                if (end != value.c_str() && *end == '\0') GanttSetHeadlineProgress(hi, static_cast<int>(parsed));
            });
            return;
        } else if (cp == 't') {
            switch (sess->ruler_scale) {
                case GanttSession::RulerScale::Days:
                    sess->ruler_scale = GanttSession::RulerScale::Months;
                    status_message_ = "Gantt scale: months";
                    break;
                case GanttSession::RulerScale::Months:
                    sess->ruler_scale = GanttSession::RulerScale::Years;
                    status_message_ = "Gantt scale: years";
                    break;
                case GanttSession::RulerScale::Years:
                    sess->ruler_scale = GanttSession::RulerScale::Days;
                    status_message_ = "Gantt scale: days";
                    break;
            }
        } else if (cp == 'i' && !rows.empty()) {
            GanttBeginRename(rows[static_cast<size_t>(sess->focused_row)]);
            return;
        } else if (cp == 'u') {
            Undo();
            sess->outline = ParseOrgOutline(Buf().lines);
        }
        cp = GetCharPressed();
    }
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (ctrl && IsKeyPressed(KEY_R)) {
        Redo();
        sess->outline = ParseOrgOutline(Buf().lines);
    }
}

void Editor::GanttBeginRename(int headline_index) {
    GanttSession *sess = GetGanttMutable(CurPane().buffer_id);
    if (!sess || headline_index < 0 || headline_index >= static_cast<int>(sess->outline.headlines.size())) return;
    sess->editing_headline_index = headline_index;
    sess->edit_buffer = sess->outline.headlines[static_cast<size_t>(headline_index)].title;
    sess->edit_cursor = static_cast<int>(sess->edit_buffer.size());
    sess->editing = true;
    mode_ = Mode::GanttInsert;
}

void Editor::GanttRenameHeadline(int headline_index, const std::string &new_title) {
    auto it = gantt_views_.find(CurPane().buffer_id);
    if (it == gantt_views_.end()) return;
    GanttSession &sess = it->second;
    if (headline_index < 0 || headline_index >= static_cast<int>(sess.outline.headlines.size())) return;
    const OrgHeadline &h = sess.outline.headlines[static_cast<size_t>(headline_index)];
    std::string new_line = FormatHeadlineLine(h.level, h.todo_keyword, h.priority, new_title, h.tags);
    ReplaceLinesForLua(h.line_start, h.line_start + 1, {new_line});
    sess.outline = ParseOrgOutline(Buf().lines);
}

void Editor::HandleGanttInsertInput() {
    GanttSession *sess = nullptr;
    {
        auto it = gantt_views_.find(CurPane().buffer_id);
        if (it == gantt_views_.end()) {
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
    if (escape) {
        sess->editing = false;
        mode_ = Mode::GanttNormal;
        return;
    }
    if (enter) {
        int hi = sess->editing_headline_index;
        std::string title = sess->edit_buffer;
        sess->editing = false;
        mode_ = Mode::GanttNormal;
        if (hi >= 0) GanttRenameHeadline(hi, title);
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
        // Ctrl-Shift-V: paste the system clipboard (== unnamed register,
        // so a yank from any mep buffer pastes here too) into the child,
        // the terminal-emulator convention -- plain Ctrl-V stays a
        // literal 0x16 for the child, same as in any other terminal.
        // Newlines go as CR: that's what a terminal sends for Enter, and
        // what a shell reading pasted lines expects. Sent raw (no
        // bracketed-paste wrapping): VTerm doesn't track whether the
        // child ever enabled mode 2004 (vterm.h), so it can't know when
        // the child would want the brackets.
        if (key == KEY_V && ctrl && shift) {
            std::string text = RegisterTextForPaste('"');
            for (char &c : text) {
                if (c == '\n') c = '\r';
            }
            sess->scroll_offset = 0;
            if (!sess->exited && !text.empty()) TerminalWrite(*sess, text);
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
        if (!sess->exited) SendTerminalKey(*sess, key, 0, ctrl, shift);
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

// Default defers to mep's own active theme so a terminal pane still
// matches whatever colorscheme is active; Indexed covers both the classic
// 16-color ANSI palette (fixed, standard-ish xterm values) and the
// 16-255 6x6x6 color cube + grayscale ramp most modern CLI tools (ls
// --color, git, npm, ripgrep, ...) actually use; Rgb is 24-bit true color.
ThemeColor Editor::ResolveVTermColor(const VTermColor &c, bool is_fg) const {
    static const ThemeColor kAnsi16[16] = {
        {0, 0, 0, 255},       {205, 49, 49, 255},   {13, 188, 121, 255}, {229, 229, 16, 255},
        {36, 114, 200, 255},  {188, 63, 188, 255},  {17, 168, 205, 255}, {229, 229, 229, 255},
        {102, 102, 102, 255}, {241, 76, 76, 255},   {35, 209, 139, 255}, {245, 245, 67, 255},
        {59, 142, 234, 255},  {214, 112, 214, 255}, {41, 184, 219, 255}, {255, 255, 255, 255},
    };
    switch (c.kind) {
        case VTermColorKind::Default: {
            ThemeColor tc;
            ResolveHighlight(is_fg ? "Normal" : "NormalBg", &tc);
            return tc;
        }
        case VTermColorKind::Rgb:
            return ThemeColor{c.r, c.g, c.b, 255};
        case VTermColorKind::Indexed:
        default: {
            int idx = c.index;
            if (idx < 16) return kAnsi16[idx];
            if (idx < 232) {
                int n = idx - 16;
                int r = n / 36, g = (n / 6) % 6, b = n % 6;
                /**
                 * @brief Converts a 0-5 color-cube component into its 0-255 xterm ramp value.
                 * @param v The 0-5 color-cube component.
                 * @return 0 for v==0, otherwise v*40+55.
                 */
                auto ramp = [](int v) { return static_cast<unsigned char>(v == 0 ? 0 : v * 40 + 55); };
                return ThemeColor{ramp(r), ramp(g), ramp(b), 255};
            }
            unsigned char v = static_cast<unsigned char>(8 + (idx - 232) * 10);
            return ThemeColor{v, v, v, 255};
        }
    }
}

// A *snapshot*, not a live view: VTerm's scrollback+grid is copied into
// CurPane()'s buffer once, here, rather than re-synced every frame while
// browsing. A live view would need to keep the user's cursor position
// meaningful across re-syncs as new output arrives and old scrollback
// rotates out from under it (VTerm caps scrollback at 5000 lines --
// vterm.h's kMaxScrollback) -- real surgery for comparatively little
// payoff here, and out of step with this codebase's explicit "does not
// attempt full Vim parity" scope (editor.h's own class comment). Ctrl-\ Ctrl-N
// again always re-snapshots, so "stale" is at most one chord away
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

    // Snapshotted cell colors are carried as Decorations in a namespace of
    // their own -- without this, the plain Normal-mode buffer this
    // function builds has no way to know a cell was colored at all
    // (VTermCell::fg/bg live on the TerminalSession, which this buffer
    // isn't attached to), so every ANSI color a shell prompt/CLI tool used
    // would otherwise vanish the instant Ctrl-\ Ctrl-N converted the live
    // grid into ordinary text. Cleared and rebuilt every call (a fresh
    // chord always describes exactly *this* snapshot, never a stale one).
    int buffer_id = CurPane().buffer_id;
    int color_ns = CreateNamespace("terminal_normal_colors");
    ClearNamespaceInBuffer(buffer_id, color_ns);
    ThemeColor default_fg;
    ResolveHighlight("Normal", &default_fg);

    const VTerm *term = sess.vterm.get();
    int cursor_line = 0, cursor_col = 0;
    if (term) {
        int sb_lines = term->ScrollbackLines();
        int rows = term->Rows(), cols = term->Cols();
        for (int i = 0; i < sb_lines + rows; i++) {
            std::string line;
            line.reserve(static_cast<size_t>(cols));
            // One resolved foreground color per *cell* (i.e. per on-screen
            // column), not per byte -- Decoration::col_start/col_end are
            // character-column indices everywhere else they're consumed
            // (DrawPane's decoration rendering positions text at
            // text_x + col*g_char_width, one draw call per column, the
            // same convention DrawLineFast's own codepoint-at-a-time walk
            // uses for the base text under it), and a cell's glyph can be
            // more than one byte -- indexing by byte here would drift out
            // of alignment with every decoration draw call after the
            // first multi-byte cell on the line, recoloring (and re-
            // positioning) the wrong stretch of text for the rest of it.
            std::vector<ThemeColor> cell_fg;
            cell_fg.reserve(static_cast<size_t>(cols));
            for (int c = 0; c < cols; c++) {
                const VTermCell &cell = i < sb_lines ? term->ScrollbackAt(i, c) : term->At(i - sb_lines, c);
                const std::string &ch = cell.ch.empty() ? " " : cell.ch;
                // Same reverse-video/faint handling DrawTerminalGrid
                // (main.cpp) applies live -- reverse swaps which of the
                // cell's two colors is "foreground" for display purposes,
                // faint halves its brightness.
                const VTermColor &fg_c = cell.reverse ? cell.bg : cell.fg;
                ThemeColor fg = ResolveVTermColor(fg_c, true);
                if (cell.faint) {
                    fg.r = static_cast<unsigned char>(fg.r / 2);
                    fg.g = static_cast<unsigned char>(fg.g / 2);
                    fg.b = static_cast<unsigned char>(fg.b / 2);
                }
                line += ch;
                cell_fg.push_back(fg);
            }
            // Trailing blanks are real (unused) cells, not meaningful
            // content -- trimmed for a cleaner view/yank, same as any
            // other program's "don't pad every line to the terminal
            // width" convention. Safe as a byte-level trim on `line`
            // itself (an ASCII space (0x20) never appears as a
            // continuation/lead byte of a multi-byte UTF-8 cell) -- but
            // since each trimmed trailing space is exactly one cell too,
            // the number of *bytes* trimmed off `line` still equals the
            // number of trailing *cells* to drop from cell_fg.
            size_t end = line.find_last_not_of(' ');
            if (end == std::string::npos) {
                buf.lines.emplace_back();
                continue;
            }
            size_t trimmed_bytes = line.size() - (end + 1);
            buf.lines.push_back(line.substr(0, end + 1));
            cell_fg.resize(cell_fg.size() - trimmed_bytes);
            // Run-length-encode into one Decoration per contiguous same-
            // color run of columns, skipping runs that already match the
            // plain default foreground -- most terminal text is
            // uncolored, and skipping those keeps the decoration count
            // (and DrawPane's own per-row decoration-scan cost) down to
            // just what's actually colored.
            int row = static_cast<int>(buf.lines.size()) - 1;
            size_t run_start = 0;
            while (run_start < cell_fg.size()) {
                size_t run_end = run_start + 1;
                while (run_end < cell_fg.size() && cell_fg[run_end].r == cell_fg[run_start].r &&
                       cell_fg[run_end].g == cell_fg[run_start].g && cell_fg[run_end].b == cell_fg[run_start].b) {
                    run_end++;
                }
                const ThemeColor &fg = cell_fg[run_start];
                if (fg.r != default_fg.r || fg.g != default_fg.g || fg.b != default_fg.b) {
                    Decoration d;
                    d.row = row;
                    d.col_start = static_cast<int>(run_start);
                    d.col_end = static_cast<int>(run_end);
                    d.has_fg_color = true;
                    d.fg_color = fg;
                    AddDecorationToBuffer(buffer_id, color_ns, d);
                }
                run_start = run_end;
            }
        }
        cursor_line = std::clamp(sb_lines + term->CursorRow(), 0, std::max(0, static_cast<int>(buf.lines.size()) - 1));
        // Trailing all-blank lines below the live cursor are just unused
        // screen space (e.g. a shell prompt with most of the window still
        // empty) -- trimmed the same way, but never past the cursor's own
        // line, which must survive even when it's itself blank (a fresh
        // prompt with nothing typed yet).
        size_t min_lines = static_cast<size_t>(cursor_line) + 1;
        while (buf.lines.size() > min_lines && buf.lines.size() > 1 && buf.lines.back().empty()) {
            buf.lines.pop_back();
        }
        cursor_col = std::clamp(term->CursorCol(), 0, static_cast<int>(buf.lines[static_cast<size_t>(cursor_line)].size()));
    }
    if (buf.lines.empty()) buf.lines.emplace_back("");

    CursorPos &cur = CurPane().cursor;
    cur.row = cursor_line;
    cur.col = cursor_col;
    mode_ = Mode::Normal;
}

void Editor::SendTerminalKey(const TerminalSession &sess, int key, int codepoint, bool ctrl, bool shift) {
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
                // Shift+Tab is CBT (Cursor Backward Tabulation, CSI Z) in
                // every xterm-class terminal -- a distinct byte sequence
                // from plain Tab (0x09), not the same byte with a
                // modifier flag the child could inspect. Previously this
                // sent a plain "\t" regardless of shift, so any program
                // relying on Shift+Tab as its own binding (readline's
                // reverse completion, or Claude Code's own "shift+tab to
                // cycle" modes, reported broken here) saw indistinguishable
                // plain-Tab input and could never react to it.
                bytes = shift ? "\x1b[Z" : "\t";
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
    // :close (and the header's x) on the floating pane closes the float,
    // never the docked pane underneath it.
    if (float_node_) {
        CloseFloatPane();
        return;
    }
    Tab &tab = ActiveTab();
    if (tab.root->dir == SplitDir::Leaf) {
        // Only one pane in this tab: closing it closes the tab.
        if (Tabs().size() > 1) {
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

// --- Floating editable pane (see editor.h) ---------------------------------

bool Editor::OpenFloatPane(const std::string &path, int row, bool save_on_close, int on_close_ref) {
    if (path.empty()) {
        status_message_ = "E32: No file name";
        if (on_close_ref != 0 && lua_) lua_->UnrefFunction(on_close_ref);
        return false;
    }
    if (float_node_) CloseFloatPane();
    int buffer_id = FindOrCreateBuffer(path);
    if (buffer_id < 0) {
        status_message_ = "E484: Can't open file \"" + path + "\"";
        if (on_close_ref != 0 && lua_) lua_->UnrefFunction(on_close_ref);
        return false;
    }
    // Opened from a focused sidebar (the Todo panel's 'e'): leave sidebar
    // mode the way Escape would, remembering which row to come back to.
    float_return_sidebar_id_ = 0;
    if (mode_ == Mode::Sidebar) {
        float_return_sidebar_id_ = focused_sidebar_id_;
        float_return_sidebar_row_ = sidebar_cursor_;
        focused_sidebar_id_ = 0;
        RestoreFromOverlay();
    }
    auto node = std::make_unique<SplitNode>();
    node->dir = SplitDir::Leaf;
    node->pane.id = next_pane_id_++;
    node->pane.buffer_id = buffer_id;
    node->pane.buffer_tabs = {buffer_id};
    node->pane.cursor = ClampPositionInBuffer(buffer_id, CursorPos{std::max(0, row), 0});
    float_node_ = std::move(node);
    float_workspace_id_ = ActiveWorkspace().id;
    float_tab_index_ = ActiveWorkspace().active_tab;
    float_save_on_close_ = save_on_close;
    float_on_close_ref_ = on_close_ref;
    CancelPendingNormalState();
    mode_ = Mode::Normal;
    SyncModeToActivePaneBuffer();
    ClampCursor();
    return true;
}

void Editor::CloseFloatPane(bool force_write) {
    if (!float_node_) return;
    const int buffer_id = float_node_->pane.buffer_id;
    bool wrote = false;
    if ((float_save_on_close_ || force_write) && buffer_id >= 0 && buffer_id < static_cast<int>(buffers_.size())) {
        // Buf() is still the float's buffer here (float_node_ is set), so
        // SaveFile writes exactly what was edited.
        const Buffer &b = buffers_[static_cast<size_t>(buffer_id)];
        if ((b.modified || force_write) && !b.deleted && !b.filename.empty()) wrote = SaveFile(b.filename);
    }
    float_node_.reset();
    const int sidebar_id = float_return_sidebar_id_;
    const int sidebar_row = float_return_sidebar_row_;
    float_return_sidebar_id_ = 0;
    CancelPendingNormalState();
    mode_ = Mode::Normal;
    SyncModeToActivePaneBuffer();
    ClampCursor();
    // Back to the sidebar row this was opened from, if that sidebar is
    // still open (FocusSidebarRow clamps the row -- the list may have
    // changed under the edit).
    if (sidebar_id != 0 && IsSidebarOpen(sidebar_id)) FocusSidebarRow(sidebar_id, sidebar_row);
    // Last, once the float is fully gone: the callback (the git panel's
    // commit-on-close) may itself open prompts or another float.
    const int on_close_ref = float_on_close_ref_;
    float_on_close_ref_ = 0;
    if (on_close_ref != 0 && lua_) {
        lua_->CallRefWithBool(on_close_ref, wrote);
        lua_->UnrefFunction(on_close_ref);
    }
}

void Editor::ValidateFloatPane() {
    if (!float_node_) return;
    const int buffer_id = float_node_->pane.buffer_id;
    const bool buffer_gone =
        buffer_id < 0 || buffer_id >= static_cast<int>(buffers_.size()) || buffers_[static_cast<size_t>(buffer_id)].deleted;
    if (buffer_gone || ActiveWorkspace().id != float_workspace_id_ || ActiveWorkspace().active_tab != float_tab_index_) {
        // The buffer may already be gone (or belong to another workspace's
        // panes): never write it from here.
        float_save_on_close_ = false;
        float_return_sidebar_id_ = 0;
        CloseFloatPane();
    }
}

void Editor::CyclePane(int delta) {
    if (float_node_) CloseFloatPane();
    Tab &tab = ActiveTab();
    std::vector<int> ids;
    CollectLeaves(tab.root.get(), ids);
    if (ids.size() <= 1) return;
    auto it = std::find(ids.begin(), ids.end(), tab.active_pane_id);
    if (it == ids.end()) return;
    int idx = static_cast<int>(it - ids.begin());
    int n = static_cast<int>(ids.size());
    idx = ((idx + delta) % n + n) % n;
    tab.active_pane_id = ids[static_cast<size_t>(idx)];
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
    p.buffer_id = p.buffer_tabs[static_cast<size_t>(p.buffer_tab_index)];
    ClampCursor();
    SyncModeToActivePaneBuffer();
}

void Editor::PanePrevBufferTab() {
    Pane &p = CurPane();
    EnsureBufferTabSeeded(p);
    if (p.buffer_tabs.size() <= 1) return;
    int n = static_cast<int>(p.buffer_tabs.size());
    p.buffer_tab_index = (p.buffer_tab_index - 1 + n) % n;
    p.buffer_id = p.buffer_tabs[static_cast<size_t>(p.buffer_tab_index)];
    ClampCursor();
    SyncModeToActivePaneBuffer();
}

void Editor::GoToPaneBufferTab(int index) {
    Pane &p = CurPane();
    EnsureBufferTabSeeded(p);
    if (p.buffer_tabs.empty()) return;
    p.buffer_tab_index = std::max(0, std::min(index, static_cast<int>(p.buffer_tabs.size()) - 1));
    p.buffer_id = p.buffer_tabs[static_cast<size_t>(p.buffer_tab_index)];
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
    p.buffer_id = p.buffer_tabs[static_cast<size_t>(p.buffer_tab_index)];
    ClampCursor();
    SyncModeToActivePaneBuffer();
}

void Editor::PaneCloseBufferTabAndDelete() {
    // The float's header x: close the float, keep the buffer (it may
    // still be open in a docked pane, and the Todo panel reads it).
    if (float_node_) {
        CloseFloatPane();
        return;
    }
    Pane &p = CurPane();
    EnsureBufferTabSeeded(p);
    const int target = p.buffer_id;
    if (target < 0 || target >= static_cast<int>(buffers_.size())) return;
    if (buffers_[static_cast<size_t>(target)].modified) {
        status_message_ = "E37: No write since last change (:w to save, :bd! to discard)";
        return;
    }
    // Tab first, then buffer: PaneCloseBufferTab handles this pane's own
    // bookkeeping (next tab becomes active / pane closes / E444 on the
    // last window), and BufferDeleteById then sweeps `target` out of
    // every remaining pane -- including this one in the last-window case,
    // where it swaps in a fallback buffer instead.
    PaneCloseBufferTab();
    BufferDeleteById(target, /*force=*/false);
}

void Editor::BufferDelete(bool force) { BufferDeleteById(CurPane().buffer_id, force); }

void Editor::BufferDeleteById(int target, bool force) {
    if (target < 0 || target >= static_cast<int>(buffers_.size())) return;
    Buffer &buf = buffers_[static_cast<size_t>(target)];
    if (buf.deleted) return;  // already gone (e.g. a stray repeated :bd)
    if (!force && buf.modified) {
        status_message_ = "E37: No write since last change (add ! to override)";
        return;
    }
    if (float_node_ && float_node_->pane.buffer_id == target) CloseFloatPane();
    buf.deleted = true;

    // Computed lazily -- only if some pane actually ends up with nothing
    // left in its own buffer_tabs once `target` is removed from it.
    int fallback_id = -1;
    /**
     * @brief Finds (and caches) a buffer id to fall back to when a pane's last remaining buffer tab is the one being deleted.
     * @return The id of an existing non-deleted buffer other than `target`, or a freshly created empty buffer if none exist.
     */
    auto pick_fallback = [&]() {
        if (fallback_id >= 0) return fallback_id;
        for (int i = 0; i < static_cast<int>(buffers_.size()); i++) {
            if (i != target && !buffers_[static_cast<size_t>(i)].deleted) {
                fallback_id = i;
                return fallback_id;
            }
        }
        fallback_id = CreateEmptyBuffer();  // every buffer was deleted -- never leave a pane with none
        return fallback_id;
    };

    for (Tab &tab : Tabs()) {
        std::vector<int> pane_ids;
        CollectLeaves(tab.root.get(), pane_ids);
        for (int pane_id : pane_ids) {
            SplitNode *node = FindNode(tab.root.get(), pane_id);
            if (!node) continue;
            Pane &p = node->pane;
            EnsureBufferTabSeeded(p);
            auto it = std::find(p.buffer_tabs.begin(), p.buffer_tabs.end(), target);
            if (it == p.buffer_tabs.end()) continue;  // this pane never had `target` open at all
            const bool was_active = (p.buffer_id == target);
            const int removed_idx = static_cast<int>(it - p.buffer_tabs.begin());
            p.buffer_tabs.erase(it);
            if (p.buffer_tabs.empty()) p.buffer_tabs = {pick_fallback()};
            if (was_active) {
                // Land on whatever now occupies the removed tab's own
                // position (the tab that used to sit right after it),
                // clamped down if it was the last one -- same "which tab
                // becomes active" rule PaneCloseBufferTab already uses.
                p.buffer_tab_index = std::min(removed_idx, static_cast<int>(p.buffer_tabs.size()) - 1);
                p.buffer_id = p.buffer_tabs[static_cast<size_t>(p.buffer_tab_index)];
                // p's own cursor may not even be in-bounds for whatever
                // buffer it just landed on (a different buffer entirely,
                // in the fallback case) -- ClampPositionInBuffer, not
                // ClampCursor, since this may not be CurPane() (the same
                // deleted buffer can be the active tab in more than one
                // split pane at once).
                p.cursor = ClampPositionInBuffer(p.buffer_id, p.cursor);
            } else {
                // `target` was only a background tab here -- stay on
                // whatever was already active, just re-locate its
                // (possibly shifted-down-by-one) index in the array.
                auto ait = std::find(p.buffer_tabs.begin(), p.buffer_tabs.end(), p.buffer_id);
                p.buffer_tab_index = (ait != p.buffer_tabs.end()) ? static_cast<int>(ait - p.buffer_tabs.begin()) : 0;
            }
        }
    }
    ClampCursor();
    SyncModeToActivePaneBuffer();
    status_message_ = "Buffer " + std::to_string(target) + " deleted";
}

void Editor::PaneMoveBufferTabToNeighbor(const std::string &direction) {
    // A focused sidebar isn't a node in the split tree and has no buffer
    // tabs to move; the "move it that way" gesture becomes reordering the
    // dock's vertical stack instead (see the header comment).
    if (mode_ == Mode::Sidebar) {
        SwapSidebarInStack(focused_sidebar_id_, direction);
        return;
    }
    Tab &tab = ActiveTab();
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
        src.buffer_id = src.buffer_tabs[static_cast<size_t>(src.buffer_tab_index)];
        ClampCursor();
    }

    Pane &dst = neighbor_node->pane;
    EnsureBufferTabSeeded(dst);
    dst.buffer_tabs.insert(dst.buffer_tabs.begin() + dst.buffer_tab_index + 1, moved_buffer_id);
    dst.buffer_tab_index++;
    dst.buffer_id = moved_buffer_id;

    if (was_last_tab) ClosePane();
}

std::unique_ptr<SplitNode> Editor::BuildSpiralLayout(const std::vector<Pane> &panes, bool horizontal_next) const {
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
    node->children.push_back(BuildSpiralLayout(rest, !horizontal_next));
    return node;
}

void Editor::ApplyLayout(const std::string &kind) {
    Tab &tab = ActiveTab();
    std::vector<int> ids;
    CollectLeaves(tab.root.get(), ids);
    if (ids.size() <= 1) return;

    std::vector<Pane> panes;
    panes.reserve(ids.size());
    for (int id : ids) {
        SplitNode *n = FindNode(tab.root.get(), id);
        if (n) panes.push_back(n->pane);
    }

    /**
     * @brief Wraps a pane in a fresh leaf SplitNode.
     * @param p The pane to wrap.
     * @return A newly allocated leaf SplitNode holding `p`.
     */
    auto make_leaf = [](const Pane &p) {
        auto n = std::make_unique<SplitNode>();
        n->dir = SplitDir::Leaf;
        n->pane = p;
        return n;
    };
    /**
     * @brief Builds a SplitNode containing one leaf child per pane, stacked along the given direction.
     * @param dir The split direction for the stack.
     * @param items The panes to stack, in order.
     * @return A newly allocated SplitNode whose children are leaf nodes for each pane in `items`.
     */
    auto make_stack = [&](SplitDir dir, const std::vector<Pane> &items) {
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
    tab.id = next_tab_id_++;

    Workspace &ws = MutableActiveWorkspace();
    ws.tabs.insert(ws.tabs.begin() + ws.active_tab + 1, std::move(tab));
    ws.active_tab++;
    SyncModeToActivePaneBuffer();
}

void Editor::TabDelete() {
    Workspace &ws = MutableActiveWorkspace();
    if (ws.tabs.size() <= 1) {
        status_message_ = "E784: Cannot close last tab page";
        return;
    }
    ws.tabs.erase(ws.tabs.begin() + ws.active_tab);
    if (ws.active_tab >= static_cast<int>(ws.tabs.size())) ws.active_tab = static_cast<int>(ws.tabs.size()) - 1;
    SyncModeToActivePaneBuffer();
}

void Editor::TabNext() {
    Workspace &ws = MutableActiveWorkspace();
    if (ws.tabs.size() <= 1) return;
    ws.active_tab = (ws.active_tab + 1) % static_cast<int>(ws.tabs.size());
    SyncModeToActivePaneBuffer();
}

void Editor::TabPrevious() {
    Workspace &ws = MutableActiveWorkspace();
    if (ws.tabs.size() <= 1) return;
    int n = static_cast<int>(ws.tabs.size());
    ws.active_tab = (ws.active_tab - 1 + n) % n;
    SyncModeToActivePaneBuffer();
}

void Editor::BufferNext() {
    if (buffers_.size() <= 1) return;
    const int n = static_cast<int>(buffers_.size());
    int next = CurPane().buffer_id;
    // Bounded by n, not unbounded -- every buffer (including the current
    // one) could be `:bd`'d, in which case this just steps all the way
    // around back to where it started and SwitchToBufferForLua below is
    // a same-buffer no-op, matching BufferPrevious' own bound.
    for (int i = 0; i < n; i++) {
        next = (next + 1) % n;
        if (!buffers_[static_cast<size_t>(next)].deleted && BufferInActiveWorkspace(next)) break;
    }
    SwitchToBufferForLua(next);
}

void Editor::BufferPrevious() {
    if (buffers_.size() <= 1) return;
    const int n = static_cast<int>(buffers_.size());
    int prev = CurPane().buffer_id;
    for (int i = 0; i < n; i++) {
        prev = (prev - 1 + n) % n;
        if (!buffers_[static_cast<size_t>(prev)].deleted && BufferInActiveWorkspace(prev)) break;
    }
    SwitchToBufferForLua(prev);
}

// Click-to-switch (NVIM_PARITY_PLAN.md Phase 11 gap): jumps directly to a
// tab by index, unlike TabNext/TabPrevious's relative stepping -- what a
// mouse click on a specific tab box in the tab bar needs. Out-of-range
// indices are silently clamped rather than ignored, matching this file's
// existing tolerant style (e.g. TabDelete's active_tab clamp above) since
// the click hit-testing that calls this only ever passes a valid index
// anyway.
void Editor::GoToTab(int index) {
    Workspace &ws = MutableActiveWorkspace();
    if (ws.tabs.empty()) return;
    ws.active_tab = std::max(0, std::min(index, static_cast<int>(ws.tabs.size()) - 1));
}

// --- Workspaces (WORKSPACES_PLAN.md Phase 2) --------------------------------

Workspace Editor::MakeWorkspace(const std::string &name, const std::string &root, const std::string &branch) {
    Workspace ws;
    ws.id = next_workspace_id_++;
    ws.name = name;
    ws.root = root;
    ws.branch = branch;
    int buffer_id = CreateEmptyBuffer();
    buffers_[static_cast<size_t>(buffer_id)].workspace_id = ws.id;
    Tab tab;
    tab.root = std::make_unique<SplitNode>();
    tab.root->dir = SplitDir::Leaf;
    tab.root->pane.id = next_pane_id_++;
    tab.root->pane.buffer_id = buffer_id;
    tab.active_pane_id = tab.root->pane.id;
    tab.id = next_tab_id_++;
    ws.tabs.push_back(std::move(tab));
    return ws;
}

const Workspace *Editor::FindWorkspaceByNameIn(const Project &project, const std::string &name) {
    for (const Workspace &ws : project.workspaces) {
        if (ws.name == name) return &ws;
    }
    return nullptr;
}

int Editor::WorkspaceNewIn(Project &project, const std::string &name, const std::string &root,
                           const std::string &branch) {
    if (!ValidWorkspaceName(name)) {
        status_message_ = "Invalid workspace name '" + name + "' (use letters, digits, . _ -)";
        return -1;
    }
    if (FindWorkspaceByNameIn(project, name)) {
        status_message_ = "Workspace '" + name + "' already exists";
        return -1;
    }
    Workspace ws = MakeWorkspace(name, root.empty() ? project.root : root, branch);
    int id = ws.id;
    project.workspaces.push_back(std::move(ws));
    workspace_change_epoch_++;
    return id;
}

int Editor::WorkspaceNew(const std::string &name, const std::string &root, const std::string &branch) {
    return WorkspaceNewIn(MutableActiveProject(), name, root, branch);
}

Project *Editor::ProjectOfWorkspace(int workspace_id) {
    for (Project &p : projects_) {
        for (const Workspace &ws : p.workspaces) {
            if (ws.id == workspace_id) return &p;
        }
    }
    return nullptr;
}

const Project *Editor::ProjectOfWorkspace(int workspace_id) const {
    for (const Project &p : projects_) {
        for (const Workspace &ws : p.workspaces) {
            if (ws.id == workspace_id) return &p;
        }
    }
    return nullptr;
}

void Editor::ChdirToActiveRoot() {
#if !defined(__EMSCRIPTEN__)
    std::error_code ec;
    std::filesystem::current_path(ActiveRoot(), ec);
#endif
}

void Editor::AfterWorkspaceActivated() {
    ChdirToActiveRoot();
    // Same "leaving a live terminal's keystroke-forwarding mode behind when
    // focus actually moves" rule as NavigatePaneDirection's own.
    if (mode_ == Mode::Terminal) mode_ = Mode::Normal;
    SyncModeToActivePaneBuffer();
    workspace_change_epoch_++;
}

bool Editor::WorkspaceSwitch(int id) {
    for (size_t pi = 0; pi < projects_.size(); pi++) {
        Project &project = projects_[pi];
        for (size_t wi = 0; wi < project.workspaces.size(); wi++) {
            Workspace &ws = project.workspaces[wi];
            if (ws.id != id) continue;
            if (ws.creating) {
                status_message_ = "Workspace '" + ws.name + "' is still being created";
                return false;
            }
            if (static_cast<int>(pi) == active_project_ && static_cast<int>(wi) == project.active_workspace) return true;
            active_project_ = static_cast<int>(pi);
            project.active_workspace = static_cast<int>(wi);
            AfterWorkspaceActivated();
            status_message_ = "workspace " + ws.name + (ws.branch.empty() ? "" : " on branch " + ws.branch);
            return true;
        }
    }
    status_message_ = "No such workspace";
    return false;
}

void Editor::WorkspaceNext() {
    const Project &project = ActiveProject();
    const int n = static_cast<int>(project.workspaces.size());
    if (n <= 1) return;
    for (int step = 1; step < n; step++) {
        const Workspace &ws = project.workspaces[static_cast<size_t>((project.active_workspace + step) % n)];
        if (ws.creating) continue;
        WorkspaceSwitch(ws.id);
        return;
    }
}

void Editor::WorkspacePrevious() {
    const Project &project = ActiveProject();
    const int n = static_cast<int>(project.workspaces.size());
    if (n <= 1) return;
    for (int step = 1; step < n; step++) {
        const Workspace &ws = project.workspaces[static_cast<size_t>((project.active_workspace - step + n * step) % n)];
        if (ws.creating) continue;
        WorkspaceSwitch(ws.id);
        return;
    }
}

std::string Editor::ResolveBufferPath(const Buffer &buf, const std::string &path) const {
    if (path.empty() || path[0] == '/') return path;
    if (buf.workspace_id < 0 || buf.workspace_id == ActiveWorkspace().id) return path;
    const Workspace *ws = FindWorkspace(buf.workspace_id);
    if (!ws || ws->root.empty()) return path;
    return ws->root + "/" + path;
}

bool Editor::WorkspaceHasModifiedBuffers(int id) const {
    for (const Buffer &buf : buffers_) {
        if (buf.workspace_id == id && buf.modified && !buf.deleted) return true;
    }
    return false;
}

bool Editor::WorkspaceDelete(int id, bool force) {
    for (size_t pi = 0; pi < projects_.size(); pi++) {
        Project &project = projects_[pi];
        for (size_t wi = 0; wi < project.workspaces.size(); wi++) {
            Workspace &ws = project.workspaces[wi];
            if (ws.id != id) continue;
            if (ws.primary) {
                status_message_ = "Cannot delete the primary workspace '" + ws.name + "'";
                return false;
            }
            if (ws.creating) {
                status_message_ = "Workspace '" + ws.name + "' is still being created";
                return false;
            }
            if (!force && WorkspaceHasModifiedBuffers(id)) {
                status_message_ = "E37: workspace '" + ws.name + "' has unsaved buffers (add ! to override)";
                return false;
            }
            ReleaseWorkspaceResources(id);
            const std::string name = ws.name;
            const bool was_active = static_cast<int>(pi) == active_project_ &&
                                    static_cast<int>(wi) == project.active_workspace;
            project.workspaces.erase(project.workspaces.begin() + static_cast<long>(wi));
            if (project.active_workspace > static_cast<int>(wi)) {
                project.active_workspace--;
            } else if (project.active_workspace >= static_cast<int>(project.workspaces.size())) {
                project.active_workspace = static_cast<int>(project.workspaces.size()) - 1;
            }
            if (was_active) {
                AfterWorkspaceActivated();
            } else {
                workspace_change_epoch_++;
            }
            status_message_ = "Deleted workspace " + name;
            return true;
        }
    }
    status_message_ = "No such workspace";
    return false;
}

bool Editor::WorkspaceReset(int id, bool force) {
    for (size_t pi = 0; pi < projects_.size(); pi++) {
        Project &project = projects_[pi];
        for (size_t wi = 0; wi < project.workspaces.size(); wi++) {
            Workspace &ws = project.workspaces[wi];
            if (ws.id != id) continue;
            if (ws.creating) {
                status_message_ = "Workspace '" + ws.name + "' is still being created";
                return false;
            }
            if (!force && WorkspaceHasModifiedBuffers(id)) {
                status_message_ = "E37: workspace '" + ws.name + "' has unsaved buffers (add ! to override)";
                return false;
            }
            ReleaseWorkspaceResources(id);
            // Rebuilt by hand rather than via MakeWorkspace so the fresh
            // buffer is scoped to *this* workspace's id (MakeWorkspace
            // would scope it to a newly allocated one).
            const int buffer_id = CreateEmptyBuffer();
            buffers_[static_cast<size_t>(buffer_id)].workspace_id = id;
            Tab tab;
            tab.root = std::make_unique<SplitNode>();
            tab.root->dir = SplitDir::Leaf;
            tab.root->pane.id = next_pane_id_++;
            tab.root->pane.buffer_id = buffer_id;
            tab.active_pane_id = tab.root->pane.id;
            tab.id = next_tab_id_++;
            ws.tabs.clear();
            ws.tabs.push_back(std::move(tab));
            ws.active_tab = 0;
            const bool was_active = static_cast<int>(pi) == active_project_ &&
                                    static_cast<int>(wi) == project.active_workspace;
            if (was_active) {
                AfterWorkspaceActivated();
            } else {
                workspace_change_epoch_++;
            }
            status_message_ = "Cleared workspace " + ws.name;
            return true;
        }
    }
    status_message_ = "No such workspace";
    return false;
}

void Editor::ReleaseWorkspaceResources(int workspace_id) {
    // Kill this workspace's terminals: the PTY child would otherwise keep
    // running with no pane able to ever show it again.
    for (auto it = terminals_.begin(); it != terminals_.end();) {
        const int bid = it->first;
        const bool mine = bid >= 0 && bid < static_cast<int>(buffers_.size()) &&
                          buffers_[static_cast<size_t>(bid)].workspace_id == workspace_id;
        if (!mine) {
            ++it;
            continue;
        }
#if !defined(__EMSCRIPTEN__)
        if (it->second.job_id > 0) JobManager::Instance().Kill(it->second.job_id);
#endif
        it = terminals_.erase(it);
    }
    // Soft-delete (never erase -- Buffer::deleted's own comment) so ids
    // stay stable and FindOrCreateBuffer can revive them if the workspace
    // is recreated with the same root.
    for (Buffer &buf : buffers_) {
        if (buf.workspace_id == workspace_id) buf.deleted = true;
    }
}

bool Editor::WorkspaceRename(int id, const std::string &name) {
    Workspace *ws = FindWorkspace(id);
    if (!ws) {
        status_message_ = "No such workspace";
        return false;
    }
    if (!ValidWorkspaceName(name)) {
        status_message_ = "Invalid workspace name '" + name + "' (use letters, digits, . _ -)";
        return false;
    }
    if (const Workspace *other = FindWorkspaceByName(name); other && other != ws) {
        status_message_ = "Workspace '" + name + "' already exists";
        return false;
    }
    ws->name = name;
    workspace_change_epoch_++;
    return true;
}

int Editor::ResolveWorkspaceArg(const std::string &arg) const {
    if (arg.empty()) return ActiveWorkspace().id;
    if (const Workspace *ws = FindWorkspaceByName(arg)) return ws->id;
    bool digits = !arg.empty() && std::all_of(arg.begin(), arg.end(),
                                              [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
    if (digits) {
        int idx = std::atoi(arg.c_str()) - 1;
        if (idx >= 0 && idx < WorkspaceCount()) return ActiveProject().workspaces[static_cast<size_t>(idx)].id;
    }
    return -1;
}

namespace {
// A Lua string literal for `text` (long-bracket form, so no escaping of
// quotes/backslashes is needed; the bracket level grows past any `]=]`
// the text itself contains).
std::string LuaQuote(const std::string &text) {
    std::string eq;
    while (text.find("]" + eq + "]") != std::string::npos) eq += "=";
    return "[" + eq + "[" + text + "]" + eq + "]";
}
std::string TrimWs(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    return s.substr(b);
}
}  // namespace

void Editor::WorkspaceCreate(const std::string &name, bool attach_existing) {
#if !defined(__EMSCRIPTEN__)
    Project &project = MutableActiveProject();
    if (project.is_git) {
        // Decision 6: the worktree is created asynchronously and the
        // workspace only becomes real (switchable, deletable) on exit 0.
        // Until then a `creating` placeholder is drawn dimmed in the bar.
        if (!ValidWorkspaceName(name)) {
            status_message_ = "Invalid workspace name '" + name + "' (use letters, digits, . _ -)";
            return;
        }
        if (FindWorkspaceByName(name)) {
            status_message_ = "Workspace '" + name + "' already exists";
            return;
        }
        const std::string dir = DeriveWorktreeDir(project.root, name, worktree_dir_override_);
        Workspace ws = MakeWorkspace(name, dir, name);
        ws.creating = true;
        const int id = ws.id;
        project.workspaces.push_back(std::move(ws));
        workspace_change_epoch_++;

        std::vector<std::string> argv = attach_existing
                                            ? std::vector<std::string>{"git", "worktree", "add", dir, name}
                                            : std::vector<std::string>{"git", "worktree", "add", "-b", name, dir, "HEAD"};
        auto err = std::make_shared<std::string>();
        JobManager::Callbacks cb;
        cb.on_stderr = [err](const std::string &line) { *err += line + "\n"; };
        cb.on_exit = [this, id, name, dir, err](int code) {
            Workspace *created = FindWorkspace(id);
            if (!created) return;  // pruned/closed meanwhile
            if (code == 0) {
                created->creating = false;
                created->root = dir;
                created->branch = name;
                WorkspaceSwitch(id);
                Notify("workspace " + name + " on branch " + name);
                return;
            }
            created->creating = false;
            WorkspaceDelete(id, /*force=*/true);
            std::string msg = TrimWs(*err);
            if (msg.empty()) msg = "git worktree add exited " + std::to_string(code);
            if (msg.find("already exists") != std::string::npos) msg += " (use :wsnew! " + name + " to attach to the existing branch)";
            Notify("Workspace '" + name + "': " + msg, NotifyLevel::Error);
        };
        // Always run from the primary checkout's toplevel, never the active
        // workspace's root, or nested worktree dirs appear ("Risks").
        int job = JobManager::Instance().Spawn(argv, project.git_toplevel.empty() ? project.root : project.git_toplevel, cb);
        if (job == 0) {
            Workspace *created = FindWorkspace(id);
            if (created) created->creating = false;
            WorkspaceDelete(id, /*force=*/true);
            Notify("Failed to run git", NotifyLevel::Error);
            return;
        }
        status_message_ = "Creating workspace " + name + "...";
        return;
    }
#else
    (void)attach_existing;
#endif
    // Non-git project (decision 8): a named tab group sharing the project
    // root, created synchronously.
    int id = WorkspaceNew(name, "", "");
    if (id < 0) return;
    WorkspaceSwitch(id);
    if (!ActiveProject().is_git) status_message_ = "workspace " + name + " (not a git repository: no worktree created)";
}

void Editor::WorkspaceRemove(const std::string &arg, bool force) {
    int id = ResolveWorkspaceArg(arg);
    const Workspace *ws = id >= 0 ? FindWorkspace(id) : nullptr;
    if (!ws) {
        status_message_ = "No such workspace: " + arg;
        return;
    }
    // The in-memory refusals (primary, creating, unsaved buffers) are
    // checked *before* touching git so a refused delete never leaves a
    // removed worktree behind.
    if (ws->primary) {
        status_message_ = "Cannot delete the primary workspace '" + ws->name + "'";
        return;
    }
    if (ws->creating) {
        status_message_ = "Workspace '" + ws->name + "' is still being created";
        return;
    }
    if (!force && WorkspaceHasModifiedBuffers(id)) {
        status_message_ = "E37: workspace '" + ws->name + "' has unsaved buffers (add ! to override)";
        return;
    }
#if !defined(__EMSCRIPTEN__)
    const Project *project = ProjectOfWorkspace(id);
    if (project && project->is_git && ws->root != project->root) {
        std::vector<std::string> argv = {"git", "worktree", "remove"};
        if (force) argv.emplace_back("--force");
        argv.push_back(ws->root);
        auto err = std::make_shared<std::string>();
        const std::string name = ws->name;
        JobManager::Callbacks cb;
        cb.on_stderr = [err](const std::string &line) { *err += line + "\n"; };
        cb.on_exit = [this, id, name, force, err](int code) {
            if (code == 0) {
                if (WorkspaceDelete(id, force)) Notify("Removed workspace " + name + " (branch kept)");
                return;
            }
            std::string msg = TrimWs(*err);
            if (msg.empty()) msg = "git worktree remove exited " + std::to_string(code);
            // Without `!` git itself refuses on a dirty tree -- surfaced
            // verbatim (decision 5).
            Notify("Workspace '" + name + "': " + msg, NotifyLevel::Error);
        };
        if (JobManager::Instance().Spawn(argv, project->git_toplevel.empty() ? project->root : project->git_toplevel, cb) == 0) {
            Notify("Failed to run git", NotifyLevel::Error);
            return;
        }
        status_message_ = "Removing worktree " + ws->root + "...";
        return;
    }
#endif
    WorkspaceDelete(id, force);
}

void Editor::ProjectDetectGit(int project_id) {
#if !defined(__EMSCRIPTEN__)
    Project *project = FindProject(project_id);
    if (!project) return;
    auto out = std::make_shared<std::string>();
    JobManager::Callbacks cb;
    cb.on_stdout = [out](const std::string &line) {
        if (out->empty()) *out = TrimWs(line);
    };
    cb.on_exit = [this, project_id, out](int code) {
        Project *p = FindProject(project_id);
        if (!p) return;
        if (code != 0 || out->empty()) {
            p->is_git = false;
            p->git_toplevel.clear();
            return;
        }
        p->is_git = true;
        p->git_toplevel = *out;
        auto text = std::make_shared<std::string>();
        JobManager::Callbacks cb2;
        cb2.on_stdout = [text](const std::string &line) { *text += line + "\n"; };
        cb2.on_exit = [this, project_id, text](int code2) {
            if (code2 == 0) AdoptWorktrees(project_id, ParseWorktreeList(*text));
        };
        JobManager::Instance().Spawn({"git", "worktree", "list", "--porcelain"}, p->git_toplevel, cb2);
    };
    JobManager::Instance().Spawn({"git", "rev-parse", "--show-toplevel"}, project->root, cb);
#else
    (void)project_id;
#endif
}

void Editor::AdoptWorktrees(int project_id, const std::vector<WorktreeEntry> &entries) {
    Project *project = FindProject(project_id);
    if (!project) return;
    auto canon = [](const std::string &path) {
        std::error_code ec;
        std::filesystem::path c = std::filesystem::canonical(path, ec);
        return ec ? path : c.string();
    };
    // The first entry is git's main worktree: the checkout every `git
    // worktree add` must run from ("Risks"), whatever directory mep was
    // launched in.
    if (!entries.empty() && !entries[0].bare) project->git_toplevel = canon(entries[0].path);
    const std::string derived_base =
        std::filesystem::path(DeriveWorktreeDir(project->root, "x", worktree_dir_override_)).parent_path().string();
    bool changed = false;
    for (const WorktreeEntry &entry : entries) {
        if (entry.bare || entry.prunable) continue;
        const std::string path = canon(entry.path);
        const std::string branch = entry.branch.empty() ? (entry.head.empty() ? "" : "(" + entry.head.substr(0, 7) + ")") : entry.branch;
        bool matched = false;
        for (Workspace &ws : project->workspaces) {
            if (ws.root != path) continue;
            if (ws.branch != branch) {
                ws.branch = branch;
                changed = true;
            }
            matched = true;
            break;
        }
        if (matched) continue;
        // Only worktrees under the derived directory are adopted
        // automatically (decision 7); others wait for :wsadopt.
        if (std::filesystem::path(path).parent_path().string() != derived_base) continue;
        std::string name = std::filesystem::path(path).filename().string();
        if (!ValidWorkspaceName(name) || FindWorkspaceByNameIn(*project, name)) continue;
        if (WorkspaceNewIn(*project, name, path, branch) >= 0) changed = true;
    }
    if (changed) workspace_change_epoch_++;
}

void Editor::WorkspaceAdopt(const std::string &path_or_branch) {
#if !defined(__EMSCRIPTEN__)
    Project &project = MutableActiveProject();
    if (!project.is_git) {
        status_message_ = "Not a git repository";
        return;
    }
    if (path_or_branch.empty()) {
        status_message_ = "Usage: :wsadopt <path-or-branch>";
        return;
    }
    const int project_id = project.id;
    auto text = std::make_shared<std::string>();
    JobManager::Callbacks cb;
    cb.on_stdout = [text](const std::string &line) { *text += line + "\n"; };
    cb.on_exit = [this, project_id, path_or_branch, text](int code) {
        Project *p = FindProject(project_id);
        if (!p || code != 0) {
            Notify("git worktree list failed", NotifyLevel::Error);
            return;
        }
        std::error_code ec;
        std::filesystem::path want = std::filesystem::canonical(path_or_branch, ec);
        const std::string want_path = ec ? "" : want.string();
        for (const WorktreeEntry &entry : ParseWorktreeList(*text)) {
            if (entry.bare || entry.prunable) continue;
            std::filesystem::path c = std::filesystem::canonical(entry.path, ec);
            const std::string path = ec ? entry.path : c.string();
            const std::string base = std::filesystem::path(path).filename().string();
            if (path != want_path && entry.branch != path_or_branch && base != path_or_branch) continue;
            for (const Workspace &ws : p->workspaces) {
                if (ws.root == path) {
                    Notify("Already a workspace: " + ws.name);
                    return;
                }
            }
            std::string name = entry.branch.empty() ? base : entry.branch;
            if (!ValidWorkspaceName(name) || FindWorkspaceByNameIn(*p, name)) name = base;
            if (!ValidWorkspaceName(name) || FindWorkspaceByNameIn(*p, name)) {
                Notify("Cannot derive a free workspace name for " + path, NotifyLevel::Error);
                return;
            }
            int id = WorkspaceNewIn(*p, name, path, entry.branch);
            if (id >= 0) {
                WorkspaceSwitch(id);
                Notify("Adopted worktree " + path + " as workspace " + name);
            }
            return;
        }
        Notify("No worktree matches '" + path_or_branch + "'", NotifyLevel::Warn);
    };
    JobManager::Instance().Spawn({"git", "worktree", "list", "--porcelain"},
                                 project.git_toplevel.empty() ? project.root : project.git_toplevel, cb);
#else
    (void)path_or_branch;
#endif
}

void Editor::WorkspacePrune() {
#if !defined(__EMSCRIPTEN__)
    Project &project = MutableActiveProject();
    const int project_id = project.id;
    auto drop_vanished = [this, project_id] {
        Project *p = FindProject(project_id);
        if (!p) return;
        std::vector<int> gone;
        for (const Workspace &ws : p->workspaces) {
            if (ws.primary || ws.creating) continue;
            std::error_code ec;
            if (!std::filesystem::is_directory(ws.root, ec)) gone.push_back(ws.id);
        }
        for (int id : gone) WorkspaceDelete(id, /*force=*/true);
        Notify(gone.empty() ? "No workspaces to prune" : "Pruned " + std::to_string(gone.size()) + " workspace(s)");
    };
    if (!project.is_git) {
        drop_vanished();
        return;
    }
    JobManager::Callbacks cb;
    cb.on_exit = [drop_vanished](int) { drop_vanished(); };
    JobManager::Instance().Spawn({"git", "worktree", "prune"},
                                 project.git_toplevel.empty() ? project.root : project.git_toplevel, cb);
#endif
}

// --- Projects (WORKSPACES_PLAN.md Phase 9) ----------------------------------

namespace {
std::string CanonicalDir(const std::string &path) {
    std::error_code ec;
    std::filesystem::path p = std::filesystem::canonical(path, ec);
    if (ec || !std::filesystem::is_directory(p, ec)) return "";
    return p.string();
}
std::string BasenameOrPath(const std::string &root) {
    std::string name = std::filesystem::path(root).filename().string();
    return name.empty() ? root : name;
}
}  // namespace

int Editor::ProjectLoad(const std::string &root_arg, bool *restored) {
    if (restored) *restored = false;
    const std::string root = CanonicalDir(root_arg.empty() ? "." : root_arg);
    if (root.empty()) {
        status_message_ = "Not a directory: " + root_arg;
        return -1;
    }
    for (size_t i = 0; i < projects_.size(); i++) {
        if (projects_[i].root != root) continue;
        if (restored) *restored = true;  // already has whatever layout the user built
        ProjectSwitch(projects_[i].id);
        return projects_[i].id;
    }
    Project project;
    project.id = next_project_id_++;
    project.root = root;
    project.name = BasenameOrPath(root);
    Workspace ws = MakeWorkspace("main", root, "");
    ws.primary = true;
    project.workspaces.push_back(std::move(ws));
    projects_.push_back(std::move(project));
    const int id = projects_.back().id;
    active_project_ = static_cast<int>(projects_.size()) - 1;
    AfterWorkspaceActivated();
    if (RestoreWorkspaces() && RestoreWorkspaceState(id, /*keep_primary_tabs=*/false) && restored) *restored = true;
    ProjectDetectGit(id);
    status_message_ = "project " + ActiveProject().name + " (" + root + ")";
    return id;
}

bool Editor::ProjectSwitch(int id) {
    for (size_t i = 0; i < projects_.size(); i++) {
        if (projects_[i].id != id) continue;
        if (static_cast<int>(i) == active_project_) return true;
        active_project_ = static_cast<int>(i);
        Project &p = projects_[i];
        if (p.active_workspace < 0 || p.active_workspace >= static_cast<int>(p.workspaces.size())) p.active_workspace = 0;
        AfterWorkspaceActivated();
        status_message_ = "project " + p.name + " / workspace " + ActiveWorkspace().name;
        return true;
    }
    status_message_ = "No such project";
    return false;
}

bool Editor::ProjectClose(int id, bool force) {
    if (projects_.size() <= 1) {
        status_message_ = "Cannot close the last project";
        return false;
    }
    for (size_t i = 0; i < projects_.size(); i++) {
        Project &p = projects_[i];
        if (p.id != id) continue;
        if (!force) {
            for (const Workspace &ws : p.workspaces) {
                if (WorkspaceHasModifiedBuffers(ws.id)) {
                    status_message_ = "E37: project '" + p.name + "' has unsaved buffers in workspace '" + ws.name +
                                      "' (add ! to override)";
                    return false;
                }
            }
        }
        for (const Workspace &ws : p.workspaces) {
            if (ws.creating) {
                status_message_ = "Workspace '" + ws.name + "' is still being created";
                return false;
            }
        }
        SaveWorkspaceState(id);
        for (const Workspace &ws : p.workspaces) ReleaseWorkspaceResources(ws.id);
        const std::string name = p.name;
        const bool was_active = static_cast<int>(i) == active_project_;
        projects_.erase(projects_.begin() + static_cast<long>(i));
        if (active_project_ > static_cast<int>(i)) {
            active_project_--;
        } else if (active_project_ >= static_cast<int>(projects_.size())) {
            active_project_ = static_cast<int>(projects_.size()) - 1;
        }
        if (was_active) {
            AfterWorkspaceActivated();
        } else {
            workspace_change_epoch_++;
        }
        status_message_ = "Closed project " + name;
        return true;
    }
    status_message_ = "No such project";
    return false;
}

void Editor::ProjectNext() {
    if (projects_.size() <= 1) return;
    ProjectSwitch(projects_[static_cast<size_t>((active_project_ + 1) % static_cast<int>(projects_.size()))].id);
}

void Editor::ProjectPrevious() {
    if (projects_.size() <= 1) return;
    const int n = static_cast<int>(projects_.size());
    ProjectSwitch(projects_[static_cast<size_t>((active_project_ - 1 + n) % n)].id);
}

int Editor::ResolveProjectArg(const std::string &arg) const {
    if (arg.empty()) return ActiveProject().id;
    for (const Project &p : projects_) {
        if (p.name == arg || p.root == arg) return p.id;
    }
    bool digits = std::all_of(arg.begin(), arg.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
    if (digits) {
        int idx = std::atoi(arg.c_str()) - 1;
        if (idx >= 0 && idx < ProjectCount()) return projects_[static_cast<size_t>(idx)].id;
    }
    const std::string canon = CanonicalDir(arg);
    if (!canon.empty()) {
        for (const Project &p : projects_) {
            if (p.root == canon) return p.id;
        }
    }
    return -1;
}

bool Editor::RerootInitialProject(const std::string &root_arg) {
    const std::string root = CanonicalDir(root_arg);
    if (root.empty()) {
        status_message_ = "Not a directory: " + root_arg;
        return false;
    }
    Project &project = MutableActiveProject();
    project.root = root;
    project.name = BasenameOrPath(root);
    for (Workspace &ws : project.workspaces) ws.root = root;
    ChdirToActiveRoot();
    return true;
}

// --- Persistence (WORKSPACES_PLAN.md Phase 10) ------------------------------

namespace {
constexpr int kWorkspaceStateVersion = 1;

// Path of `filename` relative to `root` when it lives under it (so a moved
// repo still restores); otherwise as-is.
std::string RelativeToRoot(const std::string &filename, const std::string &root) {
    if (filename.empty() || filename[0] != '/') return filename;
    if (!root.empty() && filename.size() > root.size() + 1 && filename.compare(0, root.size(), root) == 0 &&
        filename[root.size()] == '/') {
        return filename.substr(root.size() + 1);
    }
    return filename;
}
}  // namespace

std::string Editor::WorkspaceStateFile(const Project &project) const {
    const std::string data = MepDataDir();
    if (data.empty()) return "";
    return WorkspaceStatePath(data, project.root);
}

Json Editor::SplitStateJson(const Workspace &ws, const SplitNode &node) const {
    Json j = Json::Object();
    if (node.dir == SplitDir::Leaf) {
        const Pane &pane = node.pane;
        j["dir"] = "leaf";
        Json pj = Json::Object();
        pj["id"] = pane.id;
        pj["cursor"] = Json::Array();
        pj["cursor"].push_back(Json(pane.cursor.row));
        pj["cursor"].push_back(Json(pane.cursor.col));
        pj["scroll"] = pane.scroll_row;
        const int bid = pane.buffer_id;
        const bool valid = bid >= 0 && bid < static_cast<int>(buffers_.size());
        if (valid && GetTerminal(bid)) {
            pj["kind"] = "terminal";
        } else if (valid && !buffers_[static_cast<size_t>(bid)].filename.empty()) {
            pj["kind"] = "file";
            pj["buffer"] = RelativeToRoot(buffers_[static_cast<size_t>(bid)].filename, ws.root);
        } else {
            pj["kind"] = "empty";
        }
        Json tabs = Json::Array();
        for (int other : pane.buffer_tabs) {
            if (other == bid || other < 0 || other >= static_cast<int>(buffers_.size())) continue;
            const Buffer &b = buffers_[static_cast<size_t>(other)];
            if (b.deleted || b.filename.empty() || GetTerminal(other)) continue;
            tabs.push_back(Json(RelativeToRoot(b.filename, ws.root)));
        }
        pj["buffer_tabs"] = std::move(tabs);
        j["pane"] = std::move(pj);
        return j;
    }
    j["dir"] = node.dir == SplitDir::Horizontal ? "horizontal" : "vertical";
    Json children = Json::Array();
    for (const auto &child : node.children) children.push_back(SplitStateJson(ws, *child));
    j["children"] = std::move(children);
    Json shares = Json::Array();
    for (float share : node.shares) shares.push_back(Json(static_cast<double>(share)));
    j["shares"] = std::move(shares);
    return j;
}

Json Editor::WorkspaceStateJson(const Project &project) const {
    Json j = Json::Object();
    j["version"] = kWorkspaceStateVersion;
    j["root"] = project.root;
    const int aw = project.active_workspace;
    j["active_workspace"] = (aw >= 0 && aw < static_cast<int>(project.workspaces.size()))
                                ? project.workspaces[static_cast<size_t>(aw)].name
                                : std::string("main");
    Json workspaces = Json::Array();
    for (const Workspace &ws : project.workspaces) {
        if (ws.creating) continue;
        Json wj = Json::Object();
        wj["name"] = ws.name;
        wj["root"] = ws.root;
        wj["branch"] = ws.branch;
        wj["primary"] = ws.primary;
        wj["active_tab"] = ws.active_tab;
        Json tabs = Json::Array();
        for (const Tab &tab : ws.tabs) {
            Json tj = Json::Object();
            tj["active_pane"] = tab.active_pane_id;
            tj["root"] = tab.root ? SplitStateJson(ws, *tab.root) : Json::Object();
            tabs.push_back(std::move(tj));
        }
        wj["tabs"] = std::move(tabs);
        workspaces.push_back(std::move(wj));
    }
    j["workspaces"] = std::move(workspaces);
    return j;
}

bool Editor::SaveWorkspaceState(int project_id) {
#if defined(__EMSCRIPTEN__)
    (void)project_id;
    return false;
#else
    if (!session_enabled_) return false;
    const Project *project = FindProject(project_id);
    if (!project) return false;
    const std::string path = WorkspaceStateFile(*project);
    if (path.empty()) return false;
    mkdir(std::filesystem::path(path).parent_path().string().c_str(), 0755);
    return WriteJsonFile(path, WorkspaceStateJson(*project));
#endif
}

void Editor::SaveAllWorkspaceState() {
    for (const Project &p : projects_) SaveWorkspaceState(p.id);
}

std::unique_ptr<SplitNode> Editor::SplitFromStateJson(const Json &node, std::vector<std::pair<int, Json>> &leaves,
                                                      std::unordered_map<int, int> &id_map) {
    auto out = std::make_unique<SplitNode>();
    const std::string dir = node.get("dir").as_string("leaf");
    if (dir == "horizontal" || dir == "vertical") {
        const Json &children = node.get("children");
        if (children.is_array() && children.items().size() >= 2) {
            out->dir = dir == "horizontal" ? SplitDir::Horizontal : SplitDir::Vertical;
            for (const Json &child : children.items()) out->children.push_back(SplitFromStateJson(child, leaves, id_map));
            const Json &shares = node.get("shares");
            if (shares.is_array() && shares.items().size() == out->children.size()) {
                for (const Json &sh : shares.items()) out->shares.push_back(static_cast<float>(sh.as_double(0.0)));
            }
            return out;
        }
        // A degenerate split (0-1 children) collapses to one leaf.
    }
    out->dir = SplitDir::Leaf;
    out->pane.id = next_pane_id_++;
    out->pane.buffer_id = 0;
    const Json &pane = node.get("pane");
    if (pane.is_object()) {
        id_map[pane.get("id").as_int(-1)] = out->pane.id;
        leaves.emplace_back(out->pane.id, pane);
    } else {
        leaves.emplace_back(out->pane.id, Json::Object());
    }
    return out;
}

int Editor::RestoreWorkspaceTabs(Workspace &ws, const Json &ws_json) {
    // Assumes `ws` is the active workspace (so LoadFile's relative paths,
    // FindOrCreateBuffer's scoping and OpenTerminalInPlace's cwd all land
    // in it) -- RestoreWorkspaceState arranges that.
    int skipped = 0;
    const Json &tabs_json = ws_json.get("tabs");
    std::vector<Tab> new_tabs;
    struct PendingLeaf {
        size_t tab_index;
        int pane_id;
        Json pane;
    };
    std::vector<PendingLeaf> pending;
    std::vector<int> active_pane_ids;
    if (tabs_json.is_array()) {
        for (const Json &tj : tabs_json.items()) {
            std::vector<std::pair<int, Json>> leaves;
            std::unordered_map<int, int> id_map;
            Tab tab;
            tab.id = next_tab_id_++;
            tab.root = SplitFromStateJson(tj.get("root"), leaves, id_map);
            auto it = id_map.find(tj.get("active_pane").as_int(-1));
            tab.active_pane_id = it != id_map.end() ? it->second : leaves.front().first;
            for (auto &leaf : leaves) pending.push_back({new_tabs.size(), leaf.first, leaf.second});
            new_tabs.push_back(std::move(tab));
        }
    }
    if (new_tabs.empty()) return 0;  // unrestorable -> keep the single empty tab (decision 10)
    // Every leaf starts on its own fresh empty buffer so a skipped file
    // still leaves a valid pane.
    for (Tab &tab : new_tabs) {
        std::vector<int> ids;
        CollectLeaves(tab.root.get(), ids);
        for (int pid : ids) {
            SplitNode *n = FindNode(tab.root.get(), pid);
            if (n) n->pane.buffer_id = CreateEmptyBuffer();
        }
    }
    // Retire the placeholder tab's buffer (MakeWorkspace's) if untouched.
    for (Tab &old : ws.tabs) {
        std::vector<int> bids;
        CollectLeafBuffers(old.root.get(), bids);
        for (int bid : bids) {
            if (bid <= 0 || bid >= static_cast<int>(buffers_.size())) continue;
            Buffer &b = buffers_[static_cast<size_t>(bid)];
            if (b.filename.empty() && !b.modified && !GetTerminal(bid)) b.deleted = true;
        }
    }
    ws.tabs = std::move(new_tabs);
    ws.active_tab = std::max(0, std::min(ws_json.get("active_tab").as_int(0), static_cast<int>(ws.tabs.size()) - 1));
    const int saved_active_tab = ws.active_tab;
    for (const PendingLeaf &leaf : pending) {
        ws.active_tab = static_cast<int>(leaf.tab_index);
        Tab &tab = ws.tabs[leaf.tab_index];
        const int saved_pane = tab.active_pane_id;
        tab.active_pane_id = leaf.pane_id;
        const int placeholder_buffer = CurPane().buffer_id;
        const std::string kind = leaf.pane.get("kind").as_string("empty");
        if (kind == "terminal") {
            OpenTerminalInPlace("");
        } else if (kind == "file") {
            const std::string rel = leaf.pane.get("buffer").as_string("");
            std::error_code ec;
            const std::string abs = rel.empty() ? "" : (rel[0] == '/' ? rel : ws.root + "/" + rel);
            if (!rel.empty() && std::filesystem::exists(abs, ec)) {
                LoadFile(rel);
            } else if (!rel.empty()) {
                skipped++;
            }
        }
        Pane &pane = CurPane();
        // The pre-seeded empty buffer is only kept when nothing replaced it
        // (kind "empty", or a skipped file); otherwise it would linger as a
        // stray "[No Name]" in :ls.
        if (pane.buffer_id != placeholder_buffer && placeholder_buffer > 0 &&
            placeholder_buffer < static_cast<int>(buffers_.size())) {
            buffers_[static_cast<size_t>(placeholder_buffer)].deleted = true;
        }
        const Json &cur = leaf.pane.get("cursor");
        if (cur.is_array() && cur.items().size() == 2) {
            pane.cursor.row = cur.items()[0].as_int(0);
            pane.cursor.col = cur.items()[1].as_int(0);
        }
        pane.scroll_row = std::max(0, leaf.pane.get("scroll").as_int(0));
        ClampCursor();
        const Json &extra = leaf.pane.get("buffer_tabs");
        if (extra.is_array()) {
            EnsureBufferTabSeeded(pane);
            for (const Json &e : extra.items()) {
                const std::string rel = e.as_string("");
                if (rel.empty()) continue;
                std::error_code ec;
                const std::string abs = rel[0] == '/' ? rel : ws.root + "/" + rel;
                if (!std::filesystem::exists(abs, ec)) {
                    skipped++;
                    continue;
                }
                int bid = FindOrCreateBuffer(rel);
                if (bid >= 0 && std::find(pane.buffer_tabs.begin(), pane.buffer_tabs.end(), bid) == pane.buffer_tabs.end()) {
                    pane.buffer_tabs.push_back(bid);
                }
            }
        }
        tab.active_pane_id = saved_pane;
    }
    ws.active_tab = saved_active_tab;
    if (mode_ == Mode::Terminal) mode_ = Mode::Normal;
    SyncModeToActivePaneBuffer();
    return skipped;
}

bool Editor::RestoreWorkspaceState(int project_id, bool keep_primary_tabs) {
#if defined(__EMSCRIPTEN__)
    (void)project_id;
    (void)keep_primary_tabs;
    return false;
#else
    Project *project = FindProject(project_id);
    if (!project) return false;
    const std::string path = WorkspaceStateFile(*project);
    if (path.empty()) return false;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return false;
    Json doc;
    if (!ReadJsonFile(path, &doc) || !doc.is_object() || !doc.get("workspaces").is_array()) {
        Notify("Ignoring malformed workspace session file " + path, NotifyLevel::Warn);
        return false;
    }
    if (doc.get("version").as_int(0) != kWorkspaceStateVersion) {
        Notify("Ignoring workspace session file with unknown version", NotifyLevel::Warn);
        return false;
    }
    // Restore runs with each workspace active in turn (see
    // RestoreWorkspaceTabs); remember where to land afterwards.
    const int saved_project = active_project_;
    for (size_t i = 0; i < projects_.size(); i++) {
        if (projects_[i].id == project_id) active_project_ = static_cast<int>(i);
    }
    int skipped = 0;
    int pruned = 0;
    bool any = false;
    for (const Json &wj : doc.get("workspaces").items()) {
        if (!wj.is_object()) continue;
        const std::string name = wj.get("name").as_string("");
        const bool primary = wj.get("primary").as_bool(false);
        std::string root = wj.get("root").as_string("");
        Workspace *target = nullptr;
        if (primary) {
            for (Workspace &ws : project->workspaces) {
                if (ws.primary) target = &ws;
            }
            if (target && !wj.get("branch").as_string("").empty() && target->branch.empty()) {
                target->branch = wj.get("branch").as_string("");
            }
            if (keep_primary_tabs) continue;
        } else {
            if (root.empty() || !ValidWorkspaceName(name)) continue;
            if (!std::filesystem::is_directory(root, ec)) {
                pruned++;  // worktree gone (decision 10 / :wsprune)
                continue;
            }
            for (Workspace &ws : project->workspaces) {
                if (ws.root == root || ws.name == name) target = &ws;
            }
            if (!target) {
                int id = WorkspaceNewIn(*project, name, root, wj.get("branch").as_string(""));
                if (id < 0) continue;
                target = FindWorkspace(id);
            }
        }
        if (!target) continue;
        for (size_t i = 0; i < project->workspaces.size(); i++) {
            if (&project->workspaces[i] == target) project->active_workspace = static_cast<int>(i);
        }
        ChdirToActiveRoot();
        skipped += RestoreWorkspaceTabs(*target, wj);
        any = true;
    }
    const std::string active_name = doc.get("active_workspace").as_string("");
    for (size_t i = 0; i < project->workspaces.size(); i++) {
        if (project->workspaces[i].name == active_name) project->active_workspace = static_cast<int>(i);
    }
    active_project_ = saved_project;
    for (size_t i = 0; i < projects_.size(); i++) {
        if (projects_[i].id == project_id) active_project_ = static_cast<int>(i);
    }
    AfterWorkspaceActivated();
    std::string msg;
    if (skipped > 0) msg += std::to_string(skipped) + " file(s) skipped (no longer exist)";
    if (pruned > 0) msg += (msg.empty() ? "" : ", ") + std::to_string(pruned) + " workspace(s) pruned (worktree gone)";
    if (!msg.empty()) Notify("Restored workspaces: " + msg, NotifyLevel::Warn);
    return any;
#endif
}

uint64_t Editor::LayoutFingerprint() const {
    // Cheap per-frame structural hash: what changes when a workspace, tab,
    // pane or the buffer shown in a pane changes -- deliberately not the
    // cursor, which is read at save time instead (decision 10).
    // 64-bit arithmetic regardless of size_t's width (wasm is 32-bit).
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](size_t v) {
        h ^= static_cast<uint64_t>(v) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    };
    std::function<void(const SplitNode &)> walk = [&](const SplitNode &node) {
        mix(static_cast<size_t>(node.dir));
        if (node.dir == SplitDir::Leaf) {
            mix(static_cast<size_t>(node.pane.id));
            mix(static_cast<size_t>(node.pane.buffer_id) * 31u);
            mix(node.pane.buffer_tabs.size());
            return;
        }
        for (const auto &c : node.children) walk(*c);
    };
    mix(projects_.size());
    mix(static_cast<size_t>(active_project_));
    for (const Project &p : projects_) {
        mix(std::hash<std::string>{}(p.root));
        mix(static_cast<size_t>(p.active_workspace));
        for (const Workspace &ws : p.workspaces) {
            mix(std::hash<std::string>{}(ws.name));
            mix(static_cast<size_t>(ws.active_tab));
            mix(ws.creating ? 1u : 0u);
            for (const Tab &t : ws.tabs) {
                mix(static_cast<size_t>(t.active_pane_id));
                if (t.root) walk(*t.root);
            }
        }
    }
    for (const Buffer &b : buffers_) mix(std::hash<std::string>{}(b.filename));
    return h;
}

void Editor::TickWorkspacePersistence(double now) {
    // Runs on the wasm build too (SaveAllWorkspaceState is a no-op there)
    // so the bookkeeping fields aren't dead code under -Werror.
    if (!session_enabled_) return;
    const uint64_t fp = LayoutFingerprint();
    if (!persistence_primed_) {
        persistence_primed_ = true;
        last_layout_fingerprint_ = fp;
        return;
    }
    if (fp != last_layout_fingerprint_) {
        last_layout_fingerprint_ = fp;
        layout_dirty_ = true;
        layout_dirty_since_ = now;
        return;
    }
    if (layout_dirty_ && now - layout_dirty_since_ >= 0.5) {
        layout_dirty_ = false;
        SaveAllWorkspaceState();
    }
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
            float h = has_shares ? (y1 - y0) * node->shares[static_cast<size_t>(i)] : (y1 - y0) / static_cast<float>(n);
            float next_y = (i == n - 1) ? y1 : y + h;
            ComputeRects(node->children[static_cast<size_t>(i)].get(), x0, y, x1, next_y, out);
            y = next_y;
        }
    } else {
        float x = x0;
        for (int i = 0; i < n; i++) {
            float w = has_shares ? (x1 - x0) * node->shares[static_cast<size_t>(i)] : (x1 - x0) / static_cast<float>(n);
            float next_x = (i == n - 1) ? x1 : x + w;
            ComputeRects(node->children[static_cast<size_t>(i)].get(), x, y0, next_x, y1, out);
            x = next_x;
        }
    }
}

int Editor::FindNeighborPaneId(int from_pane_id, const std::string &direction) const {
    const Tab &tab = ActiveTab();
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
    if (float_node_) CloseFloatPane();
    if (mode_ == Mode::Sidebar) {
        const SidebarInstance *sb = FindSidebar(focused_sidebar_id_);
        if (!sb) return;
        bool into_panes = (sb->position == "left" && direction == "right") ||
                           (sb->position == "right" && direction == "left");
        if (into_panes) {
            RestoreFromOverlay();
            return;
        }
        // Up/down inside a left/right dock walks the vertical stack of
        // sidebars sharing that edge (DrawSidebars merges them into one
        // column; OpenSidebarIdsOn's order is top-to-bottom there). A
        // no-op at either end of the stack.
        if ((sb->position == "left" || sb->position == "right") && (direction == "up" || direction == "down")) {
            std::vector<int> ids = OpenSidebarIdsOn(sb->position);
            auto it = std::find(ids.begin(), ids.end(), sb->id);
            if (it == ids.end()) return;
            const int index = static_cast<int>(it - ids.begin()) + (direction == "down" ? 1 : -1);
            if (index >= 0 && index < static_cast<int>(ids.size())) OpenSidebar(ids[static_cast<size_t>(index)], true);
            return;
        }
        // Pressing further *outward* -- away from the pane tree, into the
        // screen edge the sidebar is already docked against -- has nowhere
        // left to navigate, so this is a no-op. Resizing that edge is
        // mod1+Shift+hjkl's job (ResizeActivePane's own Mode::Sidebar
        // branch) exclusively -- bare mod1+hjkl must stay navigate-only,
        // matching the same convention used everywhere else in the pane
        // tree, so it must NOT also fall through to ResizeActivePane here.
        return;
    }

    Tab &tab = ActiveTab();
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
    // same edge instead, if there is one. Same-edge sidebars all share one
    // merged column (DrawSidebars), so every one of them is equally
    // adjacent to the pane content: prefer the one that had focus last
    // (mod1+hjkl out of a sidebar leaves focused_sidebar_id_ set for
    // exactly this round trip), else the topmost of the stack.
    std::vector<int> ids = OpenSidebarIdsOn(direction);
    if (ids.empty()) return;
    int target_id = ids.front();
    if (std::find(ids.begin(), ids.end(), focused_sidebar_id_) != ids.end()) target_id = focused_sidebar_id_;
    OpenSidebar(target_id, true);
}

bool Editor::FindPathToPane(SplitNode *node, int pane_id, std::vector<std::pair<SplitNode *, int>> &path) const {
    if (node->dir == SplitDir::Leaf) return node->pane.id == pane_id;
    for (size_t i = 0; i < node->children.size(); i++) {
        if (FindPathToPane(node->children[i].get(), pane_id, path)) {
            path.emplace_back(node, static_cast<int>(i));
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
    // Shared by both the Mode::Sidebar branch (a focused sidebar isn't a
    // node in the split tree, so it can't go through FindPathToPane/
    // EnsureShares below) and the pane-tree branch's own "nothing to
    // negotiate space with" fallback further down -- growing/shrinking a
    // sidebar's fixed cell size directly, only on the axis matching its own
    // dock edge (a no-op if `direction` doesn't match either edge of that
    // axis).
    /**
     * @brief Grows or shrinks a sidebar's fixed size along its dock edge, in response to a resize direction.
     * @param sb The sidebar instance to resize (no-op if null).
     * @param dir The resize direction ("left"/"right"/"up"/"down"); only the axis matching the sidebar's own dock edge has any effect.
     */
    auto resize_sidebar = [](SidebarInstance *sb, const std::string &dir) {
        if (!sb) return;
        constexpr int kSidebarResizeStep = 4;
        constexpr int kSidebarMinSize = 10;
        int delta = 0;
        if (sb->position == "left") {
            if (dir == "right") delta = kSidebarResizeStep;
            else if (dir == "left") delta = -kSidebarResizeStep;
        } else if (sb->position == "right") {
            if (dir == "left") delta = kSidebarResizeStep;
            else if (dir == "right") delta = -kSidebarResizeStep;
        } else if (sb->position == "top") {
            if (dir == "down") delta = kSidebarResizeStep;
            else if (dir == "up") delta = -kSidebarResizeStep;
        } else if (sb->position == "bottom") {
            if (dir == "up") delta = kSidebarResizeStep;
            else if (dir == "down") delta = -kSidebarResizeStep;
        }
        sb->size = std::max(kSidebarMinSize, sb->size + delta);
    };
    if (mode_ == Mode::Sidebar) {
        SidebarInstance *sb = FindSidebarMut(focused_sidebar_id_);
        if (!sb) return;
        // Up/down on a left/right-docked sidebar moves the divider between
        // it and its stack neighbor (same-edge sidebars share one column,
        // split vertically by stack_share) -- grows against the one below
        // it if there is one, else shrinks against the one above, the same
        // convention the pane-tree branch below uses between siblings.
        if ((sb->position == "left" || sb->position == "right") && (direction == "up" || direction == "down")) {
            std::vector<int> ids = OpenSidebarIdsOn(sb->position);
            auto it = std::find(ids.begin(), ids.end(), sb->id);
            if (it == ids.end() || ids.size() < 2) return;
            const int index = static_cast<int>(it - ids.begin());
            const bool has_next = index + 1 < static_cast<int>(ids.size());
            const int other = has_next ? index + 1 : index - 1;
            const bool grow = has_next ? direction == "down" : direction == "up";
            const int upper = std::min(index, other), lower = std::max(index, other);
            const SidebarInstance *u = FindSidebar(ids[static_cast<size_t>(upper)]);
            const SidebarInstance *l = FindSidebar(ids[static_cast<size_t>(lower)]);
            const float total = u->stack_share + l->stack_share;
            const float frac = total > 0.0f ? u->stack_share / total : 0.5f;
            constexpr float kStackResizeStep = 0.05f;
            // Growing the focused sidebar moves the divider down when it's
            // the upper one, up when it's the lower one.
            const float delta = ((index == upper) == grow) ? kStackResizeStep : -kStackResizeStep;
            SetSidebarStackShares(u->id, l->id, frac + delta);
            return;
        }
        resize_sidebar(sb, direction);
        SetSidebarSize(sb->id, sb->size);  // propagates to the rest of the dock
        return;
    }

    if (step <= 0.0f) step = kDefaultResizeStep;
    bool wants_columns = (direction == "left" || direction == "right");
    bool wants_rows = (direction == "up" || direction == "down");
    if (!wants_columns && !wants_rows) return;
    SplitDir want_dir = wants_columns ? SplitDir::Vertical : SplitDir::Horizontal;

    Tab &tab = ActiveTab();
    std::vector<std::pair<SplitNode *, int>> path;  // leaf-to-root order (see FindPathToPane)
    if (FindPathToPane(tab.root.get(), tab.active_pane_id, path)) {
        for (auto &[node, child_index] : path) {
            if (node->dir != want_dir || node->children.size() < 2) continue;
            EnsureShares(node);
            int n = static_cast<int>(node->children.size());
            bool has_next = child_index + 1 < n;
            bool has_prev = child_index > 0;
            // "right"/"down" push the active child's far edge further that
            // way (growing it) when it has a next sibling to push into;
            // "left"/"up" retract that same edge (shrinking it). With no
            // next sibling, the roles flip against the previous one
            // instead -- see ResizeActivePane's header.
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
                break;  // alone on this axis -- fall through to the sidebar check below
            }

            float delta = grow ? step : -step;
            delta = std::clamp(delta, kMinPaneShare - node->shares[static_cast<size_t>(child_index)],
                                node->shares[static_cast<size_t>(other_index)] - kMinPaneShare);
            node->shares[static_cast<size_t>(child_index)] += delta;
            node->shares[static_cast<size_t>(other_index)] -= delta;
            return;
        }
    }

    // No sibling pane to negotiate space with on this axis (a lone unsplit
    // pane is the common case, same as the user's report -- "only one pane
    // to its right"), so there's nowhere in the split tree for this resize
    // to go. Treat any open sidebar docked on this same axis (both edges,
    // not just the one `direction` points at -- unlike
    // NavigatePaneDirection's own "step into a sidebar" fallback, which
    // only cares about the edge being moved *towards*) as if it were just
    // another pane to negotiate space with: resize_sidebar's own
    // direction-vs-position sign logic (identical to the Mode::Sidebar
    // branch above) already does the right thing for either edge --
    // e.g. with only a left-docked file tree open, "left" shrinks it and
    // "right" grows it, matching the same h-shrinks-the-left-side/
    // l-grows-the-left-side convention the pane-tree branch above already
    // uses between two ordinary panes.
    for (SidebarInstance &sb : sidebars_) {
        if (!sb.open) continue;
        bool on_axis = wants_columns ? (sb.position == "left" || sb.position == "right")
                                      : (sb.position == "top" || sb.position == "bottom");
        if (on_axis) resize_sidebar(&sb, direction);
    }
}

void Editor::SetActivePaneShare(float fraction) {
    fraction = std::clamp(fraction, kMinPaneShare, 1.0f - kMinPaneShare);
    Tab &tab = ActiveTab();
    std::vector<std::pair<SplitNode *, int>> path;  // leaf-to-root order (see FindPathToPane)
    if (!FindPathToPane(tab.root.get(), tab.active_pane_id, path) || path.empty()) return;
    auto &[node, child_index] = path[0];  // immediate parent
    if (node->children.size() != 2) return;
    EnsureShares(node);
    int other_index = 1 - child_index;
    node->shares[static_cast<size_t>(child_index)] = fraction;
    node->shares[static_cast<size_t>(other_index)] = 1.0f - fraction;
}

void Editor::FocusPaneById(int pane_id) {
    // The float's own header buttons/body focus it by id: it already has
    // focus, nothing to do. Any other pane means leaving the float.
    if (float_node_) {
        if (pane_id == float_node_->pane.id) return;
        CloseFloatPane();
    }
    Tab &tab = ActiveTab();
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

bool Editor::FocusPaneByIdForLua(int pane_id) {
    if (!FindNode(ActiveTab().root.get(), pane_id)) return false;
    FocusPaneById(pane_id);
    return true;
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
    Tab &tab = ActiveTab();
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
            src.buffer_id = src.buffer_tabs[static_cast<size_t>(src.buffer_tab_index)];
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

    Tab &tab = ActiveTab();
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

    Tab &tab = ActiveTab();
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

void Editor::OpenFileInPane(int dest_pane_id, const std::string &path, bool split, SplitDir dir, bool before) {
    if (path.empty()) return;
    Tab &tab = ActiveTab();
    SplitNode *dst_node = FindNode(tab.root.get(), dest_pane_id);
    if (!dst_node) return;
    if (!split) {
        FocusPaneById(dest_pane_id);
        LoadFile(path);
        return;
    }
    // Same tree surgery as SplitPaneWithBufferTab, except the new leaf
    // starts on a throwaway empty buffer that LoadFile then replaces (and
    // which is retired below so it doesn't linger as a stray [No Name]).
    const int placeholder = CreateEmptyBuffer();
    Pane new_pane;
    new_pane.id = next_pane_id_++;
    new_pane.buffer_id = placeholder;
    new_pane.buffer_tabs = {placeholder};
    new_pane.buffer_tab_index = 0;

    Pane existing_pane = dst_node->pane;

    auto new_leaf = std::make_unique<SplitNode>();
    new_leaf->dir = SplitDir::Leaf;
    new_leaf->pane = std::move(new_pane);
    auto existing_leaf = std::make_unique<SplitNode>();
    existing_leaf->dir = SplitDir::Leaf;
    existing_leaf->pane = std::move(existing_pane);

    const int new_pane_id = new_leaf->pane.id;
    dst_node->dir = dir;
    dst_node->pane = Pane{};
    dst_node->children.clear();
    dst_node->shares.clear();
    if (before) {
        dst_node->children.push_back(std::move(new_leaf));
        dst_node->children.push_back(std::move(existing_leaf));
    } else {
        dst_node->children.push_back(std::move(existing_leaf));
        dst_node->children.push_back(std::move(new_leaf));
    }
    FocusPaneById(new_pane_id);
    LoadFile(path);
    Pane &pane = CurPane();
    if (pane.buffer_id != placeholder) {
        buffers_[static_cast<size_t>(placeholder)].deleted = true;
        pane.buffer_tabs.erase(std::remove(pane.buffer_tabs.begin(), pane.buffer_tabs.end(), placeholder), pane.buffer_tabs.end());
        EnsureBufferTabSeeded(pane);
    }
    ClampCursor();
    SyncModeToActivePaneBuffer();
}

void Editor::SetPaneBorderShare(SplitNode *node, int child_index, float new_share) {
    if (!node || child_index < 0 || child_index + 1 >= static_cast<int>(node->children.size())) return;
    EnsureShares(node);
    float pair_total = node->shares[static_cast<size_t>(child_index)] + node->shares[static_cast<size_t>(child_index) + 1];
    new_share = std::clamp(new_share, kMinPaneShare, pair_total - kMinPaneShare);
    node->shares[static_cast<size_t>(child_index)] = new_share;
    node->shares[static_cast<size_t>(child_index) + 1] = pair_total - new_share;
}

float Editor::PaneBorderPairTotal(SplitNode *node, int child_index) {
    if (!node || child_index < 0 || child_index + 1 >= static_cast<int>(node->children.size())) return 0.0f;
    EnsureShares(node);
    return node->shares[static_cast<size_t>(child_index)] + node->shares[static_cast<size_t>(child_index) + 1];
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
    // Lowercases each character in place.
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

void Editor::RegisterGMapping(const std::string &key, int lua_ref) { g_mappings_[key] = lua_ref; }
void Editor::RegisterVisualGMapping(const std::string &key, int lua_ref) { visual_g_mappings_[key] = lua_ref; }

void Editor::RegisterBracketPrevMapping(const std::string &key, int lua_ref) { bracket_prev_mappings_[key] = lua_ref; }

void Editor::RegisterBracketNextMapping(const std::string &key, int lua_ref) { bracket_next_mappings_[key] = lua_ref; }

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
    // the generic q handling in HandleSidebarInput (Escape deliberately
    // doesn't close a docked sidebar).
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
    // Same for a popped-out sidebar (mod1+m) showing a preview: mod1+j/k
    // would otherwise step focus to the sidebar stacked above/below the
    // docked panel (NavigatePaneDirection's Mode::Sidebar branch), which
    // is invisible behind the float -- scroll the preview column instead,
    // exactly as the picker's own preview does. Without a preview the
    // stack-navigation binding stays untouched (and, since it refocuses a
    // different sidebar, simply ends the popout -- see
    // RefreshSidebarPopoutPreview).
    if (SidebarPopoutActive() && !extra_ctrl && !extra_shift && !SidebarPopoutPreview().empty() &&
        (IsKeyPressed(KEY_J) || IsKeyPressed(KEY_K))) {
        ScrollSidebarPopoutPreview(IsKeyPressed(KEY_J) ? 1 : -1);
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
    // Return isn't a letter key either -- same "falls outside the A-Z
    // scan" reasoning as Tab above. Checked in every mode (this whole
    // function runs before mode dispatch -- see its own call site in
    // HandleInput), which is what lets a "CR" mapping tell Normal from
    // Visual apart itself via mep.visual_selection() rather than needing
    // two separate registrations here.
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
        std::string k = extra_shift ? "S-CR" : "CR";
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

// Ctrl-T (new tab), Ctrl-Tab / Ctrl-Shift-Tab (next / previous tab) and
// Alt-1..Alt-9 (switch to workspace by number) -- fixed global bindings,
// not user-remappable mod1 mappings (mod1 itself defaults to Alt, so
// routing these through that system would make mod1=alt users' Alt-1..9
// ambiguous with whatever else mod1 might bind there; and mod1=Shift
// would turn every typed '!'..'(' into a workspace switch). Checked
// unconditionally alongside HandleMod1Shortcuts, before mode dispatch, so
// e.g. Ctrl-T opens a new tab from Insert mode the same way it would in
// Normal.
bool Editor::HandleTabShortcuts() {
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    bool alt = IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    // Workspace chords (WORKSPACES_PLAN.md Phase 2): Ctrl-Shift-T prompts
    // for a new workspace name (a Lua ui_input, so it lives in
    // kBuiltinWorkspaces), Ctrl-Alt-]/[ cycle workspaces. Checked before
    // the plain Ctrl-T / Ctrl-Tab / Alt-N chords below. <leader>w* leader
    // maps cover the same actions for layouts where these don't arrive.
    if (ctrl && shift && IsKeyPressed(KEY_T)) {
        if (lua_) lua_->DoString("mep.workspace_new_prompt()");
        return true;
    }
    if (ctrl && alt && IsKeyPressed(KEY_RIGHT_BRACKET)) {
        WorkspaceNext();
        return true;
    }
    if (ctrl && alt && IsKeyPressed(KEY_LEFT_BRACKET)) {
        WorkspacePrevious();
        return true;
    }
    if (ctrl && IsKeyPressed(KEY_T)) {
        TabNew("");
        return true;
    }
    // Ctrl-Tab / Ctrl-Shift-Tab: step through the active workspace's tabs
    // (the same relative move as :tabnext / :tabprevious). Ctrl+Alt+Tab is
    // left alone so it can't be confused with mod1+Tab's buffer cycling.
    if (ctrl && !alt && IsKeyPressed(KEY_TAB)) {
        if (shift) TabPrevious();
        else TabNext();
        return true;
    }
    // Alt-1..9: switch to the Nth workspace of the active project (the
    // same 1-based order :ws N uses and the tab bar lists). An index past
    // the last workspace, or one whose worktree is still being created, is
    // reported on the status line rather than ignored silently.
    if (alt && !ctrl) {
        for (int key = KEY_ONE; key <= KEY_NINE; key++) {
            if (!IsKeyPressed(key)) continue;
            const int idx = key - KEY_ONE;
            if (idx < WorkspaceCount()) {
                WorkspaceSwitch(ActiveProject().workspaces[static_cast<size_t>(idx)].id);
            } else {
                status_message_ = "No workspace " + std::to_string(idx + 1);
            }
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
    // Held-repeat for the four page-scroll combos only (D/U/F/B) -- not
    // the queue-drained loop above (which only ever sees a key's initial
    // down-transition, by design: see this block's own comment; raylib's
    // GetKeyPressed() queue never emits a second event for an OS/GLFW
    // auto-repeat of an already-held key), so without this, holding
    // Ctrl-D/U/F/B down scrolled once and then stopped. IsKeyPressedRepeat
    // reads a distinct signal (a key already known to be down re-firing on
    // its own repeat timer) than the single-frame press/release transition
    // IsKeyPressed missed under a slow frame -- adding it here doesn't
    // reintroduce that flakiness. The other Ctrl-combos in this same
    // batch (V/A/X/O/I/C) are one-shot actions (enter a mode, jump once,
    // nudge a number) where holding the key down repeating doesn't apply,
    // so they're deliberately left on the queue-only path above.
    if (ctrl) {
        if (IsKeyPressedRepeat(KEY_D)) ctrl_d = true;
        if (IsKeyPressedRepeat(KEY_U)) ctrl_u = true;
        if (IsKeyPressedRepeat(KEY_F)) ctrl_f = true;
        if (IsKeyPressedRepeat(KEY_B)) ctrl_b = true;
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
        // In a floating pane, the "nothing pending" Escape that is Vim's
        // harmless no-op everywhere else dismisses the float instead
        // (Insert-mode Escape still just returns to Normal first, and one
        // that cancels a pending operator/count still only cancels it).
        if (float_node_ && !IsMidNormalCommand() && !insert_one_shot_normal_) {
            CloseFloatPane();
            return;
        }
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

    // Bare Enter/KP_Enter: unbound here otherwise (this editor's Normal
    // mode has never given plain Enter a default motion the way real
    // Vim's own CR is), so a registered buffer-scoped hook (SetBufferOnEnter,
    // this class's own header comment) can claim it with no default
    // behavior to preserve. Checked ahead of the GetCharPressed() loop
    // below since raylib never reports Enter as a char event there anyway.
    if ((IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) && enter_hook_ref_ != 0 &&
        CurPane().buffer_id == enter_hook_buffer_id_ && lua_) {
        lua_->CallRef(enter_hook_ref_);
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
    bool no_pending_state_now = pending_op_ == 0 && !pending_g_ && !pending_bracket_prev_ && !pending_bracket_next_ &&
                                 !pending_ctrl_w_ && !pending_org_export_ && pending_find_ == 0 && !count_pending_now &&
                                 !awaiting_register_name_;
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
        bool no_pending_state = pending_op_ == 0 && !pending_g_ && !pending_bracket_prev_ && !pending_bracket_next_ &&
                                 !pending_ctrl_w_ && !pending_org_export_ && pending_find_ == 0 && !is_count_digit &&
                                 !awaiting_register_name_;
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
    // "+ and "* are the system clipboard, which mep keeps identical to
    // the unnamed register (see the header comment) -- so they simply
    // *are* the unnamed register here. Callers that write through this
    // (YankRange etc.) then push to the real clipboard via
    // SyncUnnamedToSystemClipboard, and readers pull first.
    if (name == '+' || name == '*') name = 0;
    return registers_[name != 0 ? name : '"'];
}

std::string Editor::SystemClipboardRead() {
    std::string text;
#if defined(__EMSCRIPTEN__)
    char *raw = mep_js_clipboard_read();
    if (raw) {
        text = raw;
        free(raw);
    }
#else
    if (!IsWindowReady()) return text;
    const char *raw = GetClipboardText();
    if (raw) text = raw;
#endif
    // Other apps (Windows ones especially, but anything that round-trips
    // through a browser too) hand over CRLF line endings; mep's buffers
    // are LF-only, and a stray '\r' would otherwise land verbatim in the
    // pasted text.
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;
        out += text[i];
    }
    return out;
}

void Editor::SystemClipboardWrite(const std::string &text) {
#if defined(__EMSCRIPTEN__)
    mep_js_clipboard_write(text.c_str());
#else
    if (!IsWindowReady()) return;
    SetClipboardText(text.c_str());
#endif
}

void Editor::SyncUnnamedToSystemClipboard() {
    const Register &unnamed = registers_['"'];
    // An empty register (e.g. yanking a zero-width range) leaves the
    // system clipboard alone rather than clearing what some other app put
    // there.
    if (unnamed.text.empty()) return;
    clipboard_synced_text_ = unnamed.text;
    SystemClipboardWrite(unnamed.text);
}

void Editor::PullSystemClipboard() {
    std::string text = SystemClipboardRead();
    if (text.empty() || text == clipboard_synced_text_) return;
    Register &unnamed = registers_['"'];
    unnamed.text = text;
    unnamed.linewise = text.back() == '\n';
    unnamed.blockwise = false;
    clipboard_synced_text_ = text;
}

std::string Editor::RegisterTextForPaste(int name) {
    if (name >= 'A' && name <= 'Z') name = name - 'A' + 'a';
    bool known = (name >= 'a' && name <= 'z') || (name >= '0' && name <= '9') || name == '-' ||
                 name == '%' || name == '"' || name == '+' || name == '*';
    if (!known) return "";
    char reg_name = static_cast<char>(name);
    if (reg_name == '"' || reg_name == '+' || reg_name == '*') {
        reg_name = 0;
        PullSystemClipboard();
    }
    return RegisterFor(reg_name).text;
}

void Editor::InsertTextAsTyped(const std::string &text) {
    // Decode UTF-8 to codepoints -- ProcessInsertKey speaks codepoints
    // (it's what GetCharPressed() hands HandleInsertInput), not bytes.
    // Malformed sequences are skipped byte-by-byte rather than inserted
    // as garbage.
    size_t i = 0;
    while (i < text.size()) {
        unsigned char b = static_cast<unsigned char>(text[i]);
        int cp = 0;
        size_t len = 1;
        if (b < 0x80) {
            cp = b;
        } else if ((b & 0xE0) == 0xC0) {
            cp = b & 0x1F;
            len = 2;
        } else if ((b & 0xF0) == 0xE0) {
            cp = b & 0x0F;
            len = 3;
        } else if ((b & 0xF8) == 0xF0) {
            cp = b & 0x07;
            len = 4;
        } else {
            i++;
            continue;
        }
        if (i + len > text.size()) break;
        bool valid = true;
        for (size_t k = 1; k < len; k++) {
            unsigned char c = static_cast<unsigned char>(text[i + k]);
            if ((c & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            cp = (cp << 6) | (c & 0x3F);
        }
        if (!valid) {
            i++;
            continue;
        }
        i += len;
        if (cp == '\n') {
            ProcessInsertKey(kReplayEnter);
        } else if (cp == '\r') {
            continue;  // stray CR (CRLF is already normalized on read)
        } else {
            ProcessInsertKey(cp);
        }
    }
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
    // mid-sequence. Also gated off while pending_textobj_scope_ is set --
    // real Vim only ever accepts the register prefix *before* the whole
    // command, never in the middle of it, and without this guard `ci"`/
    // `di"`/`da"`/`yi"` (an "i"/"a" scope waiting on its object char) had
    // their closing '"' swallowed here as a fresh register-name prefix
    // instead of ever reaching ResolveTextObject below -- `ci'` never hit
    // this because '\'' has no such collision with register-prefix syntax.
    if (pending_find_ == 0 && !pending_g_ && !pending_ctrl_w_ && pending_textobj_scope_ == 0) {
        if (awaiting_register_name_) {
            awaiting_register_name_ = false;
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '%' || c == '+' ||
                c == '*') {
                // "0-"9 (numbered/last-yank), "- (small-delete), "%
                // (filename, read-only), "+/"* (system clipboard -- one
                // and the same as the unnamed register, see RegisterFor)
                // -- all single-char names taken literally, same as
                // "a-"z. Writes to "% silently no-op (see YankRange/
                // ApplyVisualBlockOperator); "0-"9/"- are written
                // automatically by ApplyOperator rather than by naming
                // them explicitly before an operator, but reading them
                // via "1p etc. works the same as any other register.
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
        // Marker folds (`{{{`/`}}}`) get the same lazy treatment, but for
        // every filetype -- not just org.
        if (c == 'a' || c == 'o' || c == 'c' || c == 'm' || c == 'r' || c == 'R' || c == 'M') {
            if (IsOrgBuffer()) RecomputeOrgFolds();
            RecomputeMarkerFolds();
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

    // ZZ: the float pane "confirm" chord (see CloseFloatPane's force_write
    // param) -- a no-op outside a float, or if the second key isn't
    // another 'Z' (ZQ, real vim's discard-and-quit, isn't implemented).
    if (pending_capital_z_) {
        pending_capital_z_ = false;
        if (c == 'Z' && float_node_) CloseFloatPane(/*force_write=*/true);
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
    // pending" shape as f/F/t/T above. gu/gU/gq/gJ are checked first and
    // only when no operator is already pending: they're freestanding
    // operators in their own right (like d/y/c), not motions, so "gu" (or
    // "gq") itself sets pending_op_ (reusing 'u'/'U'/'q' as the operator
    // letter -- see ApplyOperator) and waits for ITS motion the same way
    // "d" does, rather than resolving anything here. (Only "guu"/"gUU"/
    // "gqq" work as the doubled-key current-line form, not "gugu"/"gUgU"/
    // "gqgq" -- all are valid Vim spellings of the same thing, but only
    // the first is worth the extra dispatch complexity here.)
    if (pending_g_ && !pending_op_ && (c == 'u' || c == 'U' || c == 'q')) {
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
    // g-prefixed actions registered from Lua (mep.map_g -- e.g. "d" for
    // mep.lsp_goto_definition's "gd"): checked before the built-in
    // gg/ge/gE fallback below so a Lua binding can claim any letter
    // those don't already use. Unlike gg/ge/gE, these are never
    // resolved as a motion for a pending operator to act on -- an LSP
    // goto-definition is fundamentally asynchronous (a round-trip away),
    // so there's no target position to hand ApplyOperator synchronously
    // the way "dgg"/"dge" get one. Any pending operator is silently
    // cancelled instead, the same "freestanding action, not a motion"
    // shape gu/gU/gJ/gv above already use.
    if (pending_g_ && lua_) {
        auto git = g_mappings_.find(std::string(1, c));
        if (git != g_mappings_.end()) {
            pending_g_ = false;
            pending_op_ = 0;
            TakeRawCount();
            lua_->CallRef(git->second);
            return true;
        }
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
    // [-prefixed / ]-prefixed actions registered from Lua (mep.map_
    // bracket_prev/mep.map_bracket_next -- e.g. "e" for LSP diagnostic
    // navigation's "[e"/"]e"). Modeled directly on pending_g_'s own
    // Lua-mapping block just above: freestanding actions only, never
    // resolved as a motion for a pending operator (this is reached only
    // once no operator is already pending -- see this whole function's
    // top-to-bottom "already-consumed" structure), any unrecognized
    // second key quietly cancels rather than falling through to whatever
    // that bare key would otherwise do (matching "unknown g-motion:
    // cancel quietly" above, not vim's own real `[`/`]` motion family,
    // none of which mep implements here beyond mep.map_g's own `i[`/`a[`
    // text objects -- a wholly separate mechanism, pending_textobj_scope_).
    if ((pending_bracket_prev_ || pending_bracket_next_) && lua_) {
        std::unordered_map<std::string, int> &table = pending_bracket_prev_ ? bracket_prev_mappings_ : bracket_next_mappings_;
        pending_bracket_prev_ = false;
        pending_bracket_next_ = false;
        auto bit = table.find(std::string(1, c));
        TakeRawCount();
        if (bit != table.end()) lua_->CallRef(bit->second);
        return true;
    }
    pending_bracket_prev_ = false;
    pending_bracket_next_ = false;

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
    if (c == '[') {
        pending_bracket_prev_ = true;
        return true;
    }
    if (c == ']') {
        pending_bracket_next_ = true;
        return true;
    }
    if (c == 'z') {
        pending_z_ = true;
        return true;
    }
    if (c == 'Z') {
        pending_capital_z_ = true;
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
                    default: break;
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
            ShiftFoldsForLineEdit(cursor.row + 1, 1);
            cursor.row++;
            cursor.col = 0;
            EnterInsert();
            break;
        case 'O':
            PushUndo();
            Buf().lines.insert(Buf().lines.begin() + cursor.row, "");
            ShiftMarksForLineEdit(cursor.row, 1);
            ShiftFoldsForLineEdit(cursor.row, 1);
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
    return pending_op_ != 0 || pending_g_ || pending_bracket_prev_ || pending_bracket_next_ || pending_find_ != 0 ||
           pending_textobj_scope_ != 0 || pending_mark_jump_ != 0 || pending_mark_set_ || pending_ctrl_w_ ||
           pending_org_export_ || pending_z_ || pending_capital_z_ || pending_count_ != 0 ||
           awaiting_register_name_ || pending_register_ != 0 || pending_macro_record_ || awaiting_macro_play_ ||
           pending_replace_;
}

void Editor::CancelPendingNormalState() {
    pending_op_ = 0;
    pending_op_count_ = 0;
    pending_g_ = false;
    pending_bracket_prev_ = false;
    pending_bracket_next_ = false;
    pending_find_ = 0;
    pending_mark_jump_ = 0;
    pending_mark_set_ = false;
    pending_textobj_scope_ = 0;
    pending_ctrl_w_ = false;
    pending_org_export_ = false;
    pending_z_ = false;
    pending_capital_z_ = false;
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
        spliced.insert(spliced.end(), keys.begin() + static_cast<long>(digit_run), keys.end());
        keys = spliced;
    }
    replaying_change_ = true;
    for (int key : keys) {
        if (mode_ == Mode::Insert) {
            ProcessInsertKey(key);
        } else if (mode_ == Mode::Normal) {
            ProcessNormalKey(key);
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
            if (key == kReplayEscape) EnterNormal();
            else if (key > 0) ProcessVisualKey(key);
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
        for (int k : keys) {
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
    bool shift_down = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    bool escape = false, enter = false, backspace = false, del = false, ctrl_w = false, ctrl_u = false;
    bool tab_key = false, ctrl_n = false, ctrl_p = false, ctrl_o = false, ctrl_r = false, ctrl_shift_v = false;
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
        else if (key == KEY_R && ctrl) ctrl_r = true;
        else if (key == KEY_V && ctrl && shift_down) ctrl_shift_v = true;
    }
    // A pending Ctrl-R only survives until the next *character*; any
    // special key in between (Escape especially) cancels it, so an
    // abandoned Ctrl-R can't swallow the first letter typed after Escape
    // and re-entering Insert.
    if (escape || enter || backspace || del || ctrl_w || ctrl_u || tab_key || ctrl_o || ctrl_shift_v) {
        insert_pending_ctrl_r_ = false;
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
    // Ctrl-Shift-V: paste the system clipboard (== unnamed register) as
    // if typed -- the terminal-emulator convention, for people who reach
    // for it before Vim's own Ctrl-R. Ctrl-R {reg}: Vim's Insert-mode
    // register paste; the register name arrives as the next character
    // (handled in the GetCharPressed() loop below), same "+/"* spelling
    // as the Normal-mode prefix. Neither is a KEY_V/KEY_R char event --
    // GLFW never delivers a char for a Ctrl-chorded key, which is what
    // keeps a bare 'r'/'v' from also landing in the buffer.
    if (ctrl_shift_v) {
        InsertTextAsTyped(RegisterTextForPaste('"'));
        return;
    }
    if (ctrl_r) {
        insert_pending_ctrl_r_ = true;
        return;
    }

    int cp = GetCharPressed();
    while (cp > 0) {
        if (insert_pending_ctrl_r_) {
            insert_pending_ctrl_r_ = false;
            InsertTextAsTyped(RegisterTextForPaste(cp));
        } else {
            ProcessInsertKey(cp);
        }
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
    std::string &line = Buf().lines[static_cast<size_t>(cursor.row)];
    line.insert(line.begin() + cursor.col, static_cast<char>(codepoint));
    cursor.col++;
    Buf().modified = true;
}

void Editor::InsertNewline() {
    CursorPos &cursor = CurPane().cursor;
    std::string &line = Buf().lines[static_cast<size_t>(cursor.row)];
    std::string remainder = line.substr(static_cast<size_t>(cursor.col));
    line.erase(static_cast<size_t>(cursor.col));
    Buf().lines.insert(Buf().lines.begin() + cursor.row + 1, remainder);
    ShiftMarksForLineEdit(cursor.row + 1, 1);
    ShiftFoldsForLineEdit(cursor.row + 1, 1);
    cursor.row++;
    cursor.col = 0;
    Buf().modified = true;
}

void Editor::Backspace() {
    CursorPos &cursor = CurPane().cursor;
    if (cursor.col > 0) {
        std::string &line = Buf().lines[static_cast<size_t>(cursor.row)];
        line.erase(static_cast<size_t>(cursor.col - 1), 1);
        cursor.col--;
    } else if (cursor.row > 0) {
        std::string current = Buf().lines[static_cast<size_t>(cursor.row)];
        Buf().lines.erase(Buf().lines.begin() + cursor.row);
        ShiftMarksForLineEdit(cursor.row, -1);
        ShiftFoldsForLineEdit(cursor.row, -1);
        cursor.row--;
        cursor.col = LineLen(cursor.row);
        Buf().lines[static_cast<size_t>(cursor.row)] += current;
    }
    Buf().modified = true;
}

void Editor::DeleteForward() {
    CursorPos &cursor = CurPane().cursor;
    std::string &line = Buf().lines[static_cast<size_t>(cursor.row)];
    if (cursor.col < static_cast<int>(line.size())) {
        line.erase(static_cast<size_t>(cursor.col), 1);
    } else if (cursor.row + 1 < Buf().LineCount()) {
        std::string next = Buf().lines[static_cast<size_t>(cursor.row) + 1];
        Buf().lines.erase(Buf().lines.begin() + cursor.row + 1);
        ShiftMarksForLineEdit(cursor.row + 1, -1);
        ShiftFoldsForLineEdit(cursor.row + 1, -1);
        line += next;
    }
    Buf().modified = true;
}

void Editor::ReplaceCharsAtCursor(int codepoint, int count) {
    CursorPos &cursor = CurPane().cursor;
    int len = LineLen(cursor.row);
    if (count <= 0 || cursor.col + count > len) return;  // not enough chars left on the line: refuse, like Vim
    PushUndo();
    std::string &line = Buf().lines[static_cast<size_t>(cursor.row)];
    for (int i = 0; i < count; i++) line[static_cast<size_t>(cursor.col) + static_cast<size_t>(i)] = static_cast<char>(codepoint);
    cursor.col += count - 1;
    Buf().modified = true;
    ClampCursor();
}

void Editor::ReplaceChar(int codepoint) {
    if (codepoint < 32 || codepoint > 126) return;
    CursorPos &cursor = CurPane().cursor;
    std::string &line = Buf().lines[static_cast<size_t>(cursor.row)];
    if (cursor.col < static_cast<int>(line.size())) {
        replace_overwritten_.push_back(line[static_cast<size_t>(cursor.col)]);
        line[static_cast<size_t>(cursor.col)] = static_cast<char>(codepoint);
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
    std::string &line = Buf().lines[static_cast<size_t>(cursor.row)];
    if (restored == '\0') {
        line.erase(static_cast<size_t>(cursor.col), 1);
    } else {
        line[static_cast<size_t>(cursor.col)] = restored;
    }
    Buf().modified = true;
}

void Editor::DeleteWordBeforeCursorInInsert() {
    CursorPos &cursor = CurPane().cursor;
    if (cursor.col == 0) return;  // no line-join, unlike Backspace -- matches Vim's Ctrl-W here
    std::string &line = Buf().lines[static_cast<size_t>(cursor.row)];
    int col = cursor.col;
    while (col > 0 && std::isspace(static_cast<unsigned char>(line[static_cast<size_t>(col - 1)]))) col--;
    if (col > 0) {
        CharClass cls = ClassOf(line[static_cast<size_t>(col - 1)]);
        while (col > 0 && ClassOf(line[static_cast<size_t>(col - 1)]) == cls) col--;
    }
    line.erase(static_cast<size_t>(col), static_cast<size_t>(cursor.col - col));
    cursor.col = col;
    Buf().modified = true;
}

void Editor::DeleteToLineStartInInsert() {
    CursorPos &cursor = CurPane().cursor;
    if (cursor.col == 0) return;
    Buf().lines[static_cast<size_t>(cursor.row)].erase(0, static_cast<size_t>(cursor.col));
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
    // "% is read-only (see YankRange's own comment) -- redirect to unnamed;
    // "+/"* are the unnamed register outright (same as in YankRange).
    if (reg_name == '%' || reg_name == '+' || reg_name == '*') reg_name = 0;
    std::vector<std::string> block_lines;
    for (int r = top; r <= bottom; r++) {
        const std::string &line = Buf().lines[static_cast<size_t>(r)];
        int a = std::min(static_cast<int>(line.size()), left);
        int b = eol ? static_cast<int>(line.size()) : std::min(static_cast<int>(line.size()), right + 1);
        block_lines.push_back(b > a ? line.substr(static_cast<size_t>(a), static_cast<size_t>(b - a)) : std::string());
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
    SyncUnnamedToSystemClipboard();
    // "0 mirrors the most recent pure yank here too (block delete below
    // does NOT touch it, matching Vim -- only op == 'y' counts as a yank).
    if (op == 'y') registers_['0'] = registers_['"'];

    if (op == 'd') {
        PushUndo();
        for (int r = top; r <= bottom; r++) {
            std::string &line = Buf().lines[static_cast<size_t>(r)];
            int a = std::min(static_cast<int>(line.size()), left);
            int b = eol ? static_cast<int>(line.size()) : std::min(static_cast<int>(line.size()), right + 1);
            if (b > a) line.erase(static_cast<size_t>(a), static_cast<size_t>(b - a));
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
    std::string &first_line = Buf().lines[static_cast<size_t>(top)];
    if (!eol && static_cast<int>(first_line.size()) < col) {
        first_line += std::string(static_cast<size_t>(col) - first_line.size(), ' ');
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
            std::string &line = Buf().lines[static_cast<size_t>(r)];
            int col = block_insert_eol_ ? static_cast<int>(line.size()) : block_insert_col_;
            if (!block_insert_eol_ && static_cast<int>(line.size()) < col) {
                line += std::string(static_cast<size_t>(col) - line.size(), ' ');
            }
            if (col <= static_cast<int>(line.size())) line.insert(static_cast<size_t>(col), block_insert_typed_);
        }
        Buf().modified = true;
    }
    block_insert_typed_.clear();
}

void Editor::PasteBlockAt(CursorPos at, const std::vector<std::string> &block, bool before) {
    int col = before ? at.col : std::min(LineLen(at.row), at.col + 1);
    for (size_t i = 0; i < block.size(); i++) {
        int row = at.row + static_cast<int>(i);
        if (row >= Buf().LineCount()) Buf().lines.emplace_back("");
        std::string &line = Buf().lines[static_cast<size_t>(row)];
        if (static_cast<int>(line.size()) < col) line += std::string(static_cast<size_t>(col) - line.size(), ' ');
        line.insert(static_cast<size_t>(col), block[i]);
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

    // Ctrl-D/Ctrl-U half-page scroll, same as Normal mode (HandleNormalInput)
    // -- GLFW/raylib doesn't emit a char event while Ctrl is held, so these
    // can never be reached via the GetCharPressed() loop below and need
    // their own check. ScrollHalfPage moves CurPane().cursor.row directly,
    // which naturally extends the Visual selection (anchor stays put) same
    // as any other cursor-moving motion in Visual mode. IsKeyPressedRepeat
    // is checked too so holding the combo down scrolls repeatedly, matching
    // the fix already applied to HandleNormalInput/HandleOfficeNormalInput/
    // HandleSheetNormalInput/HandleImageInput/HandleHtmlInput/HandlePdfInput.
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (ctrl && (IsKeyPressed(KEY_D) || IsKeyPressedRepeat(KEY_D))) {
        ScrollHalfPage(true);
        return;
    }
    if (ctrl && (IsKeyPressed(KEY_U) || IsKeyPressedRepeat(KEY_U))) {
        ScrollHalfPage(false);
        return;
    }

    int cp = GetCharPressed();
    while (cp > 0) {
        if (cp <= 127) {
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
    // rather than a fresh motion. Still gated off pending_textobj_scope_
    // though (vi"/va" etc. -- Visual mode's own i/a text objects, resolved
    // below): same '"'-vs-register-prefix collision as DispatchNormalKey's
    // own guard, see its comment.
    if (pending_find_ == 0 && !pending_g_ && pending_textobj_scope_ == 0) {
        if (awaiting_register_name_) {
            awaiting_register_name_ = false;
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '%' || c == '+' ||
                c == '*') {
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
    if (pending_g_ && c == 'q') {
        // gq always reformats whole lines (like >/< above), regardless of
        // whether the selection is charwise or linewise -- matching Vim,
        // where gq is one of the operators whose motion is forced linewise.
        pending_g_ = false;
        TakeRawCount();
        CursorPos s, e;
        VisualRange(s, e);
        FormatLines(s.row, e.row);
        CurPane().cursor = FirstNonBlank(std::min(s.row, Buf().LineCount() - 1));
        EnterNormal();
        return;
    }
    if (pending_g_ && lua_) {
        auto git = visual_g_mappings_.find(std::string(1, c));
        if (git != visual_g_mappings_.end()) {
            pending_g_ = false;
            TakeRawCount();
            lua_->CallRef(git->second);
            return;
        }
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
                    default: break;
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
    bool shift_down = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    bool escape = false, enter = false, backspace = false, up = false, down = false;
    bool tab_key = false, ctrl_n = false, ctrl_p = false, ctrl_r = false, ctrl_shift_v = false;
    for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed()) {
        if (key == KEY_ESCAPE) escape = true;
        else if (key == KEY_ENTER) enter = true;
        else if (key == KEY_BACKSPACE) backspace = true;
        else if (key == KEY_UP) up = true;
        else if (key == KEY_DOWN) down = true;
        else if (key == KEY_TAB) tab_key = true;
        else if (key == KEY_N && ctrl) ctrl_n = true;
        else if (key == KEY_P && ctrl) ctrl_p = true;
        else if (key == KEY_R && ctrl) ctrl_r = true;
        else if (key == KEY_V && ctrl && shift_down) ctrl_shift_v = true;
    }
    // Same cancel-on-any-special-key rule as HandleInsertInput's Ctrl-R.
    if (escape || enter || backspace || up || down || tab_key || ctrl_shift_v) prompt_pending_ctrl_r_ = false;
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
            command_line_ = command_history_[static_cast<size_t>(cmd_history_index_)];
        }
        return;
    }
    if (down && cmd_history_index_ != -1) {
        cmd_history_index_++;
        if (cmd_history_index_ >= static_cast<int>(command_history_.size())) {
            cmd_history_index_ = -1;
            command_line_ = cmd_history_saved_;
        } else {
            command_line_ = command_history_[static_cast<size_t>(cmd_history_index_)];
        }
        return;
    }
    // Ctrl-Shift-V / Ctrl-R {reg}: paste into the command line, same pair
    // as Insert mode. The line is single-line printable ASCII (the typing
    // loop below enforces exactly that), so pasted text is filtered the
    // same way -- a newline ends up as a space, not as a submit.
    /**
     * @brief Appends `text` to the command line under its printable-ASCII, single-line rule.
     * @param text The pasted text; newlines become spaces, anything else outside 32..126 is dropped.
     */
    auto paste_into_cmdline = [&](const std::string &text) {
        for (char c : text) {
            if (c == '\n') c = ' ';
            if (c >= 32 && c < 127) command_line_ += c;
        }
    };
    if (ctrl_shift_v) {
        paste_into_cmdline(RegisterTextForPaste('"'));
        return;
    }
    if (ctrl_r) {
        prompt_pending_ctrl_r_ = true;
        return;
    }
    int cp = pending_char;
    while (cp > 0) {
        if (prompt_pending_ctrl_r_) {
            prompt_pending_ctrl_r_ = false;
            paste_into_cmdline(RegisterTextForPaste(cp));
        } else if (cp >= 32 && cp < 127) {
            command_line_ += static_cast<char>(cp);
        }
        cp = GetCharPressed();
    }
}

void Editor::HandleSearchInput() {
    // Same GetKeyPressed()-vs-IsKeyPressed() reasoning as HandleCommandInput.
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    bool shift_down = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    bool escape = false, enter = false, backspace = false, up = false, down = false;
    bool ctrl_r = false, ctrl_shift_v = false;
    for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed()) {
        if (key == KEY_ESCAPE) escape = true;
        else if (key == KEY_ENTER) enter = true;
        else if (key == KEY_BACKSPACE) backspace = true;
        else if (key == KEY_UP) up = true;
        else if (key == KEY_DOWN) down = true;
        else if (key == KEY_R && ctrl) ctrl_r = true;
        else if (key == KEY_V && ctrl && shift_down) ctrl_shift_v = true;
    }
    if (escape || enter || backspace || up || down || ctrl_shift_v) prompt_pending_ctrl_r_ = false;
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
            search_query_ = search_history_[static_cast<size_t>(search_history_index_)];
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
            search_query_ = search_history_[static_cast<size_t>(search_history_index_)];
        }
        UpdateIncSearch();
        return;
    }
    // Ctrl-Shift-V / Ctrl-R {reg}: same paste pair as HandleCommandInput,
    // under the search prompt's own printable-ASCII rule; a pasted
    // newline is dropped (a search pattern has no use for a space there
    // the way a command line might).
    /**
     * @brief Appends `text` to the search query, keeping only printable ASCII.
     * @param text The pasted text.
     * @return True if anything was appended (so incsearch needs refreshing).
     */
    auto paste_into_search = [&](const std::string &text) {
        bool any = false;
        for (char c : text) {
            if (c >= 32 && c < 127) {
                search_query_ += c;
                any = true;
            }
        }
        return any;
    };
    if (ctrl_shift_v) {
        if (paste_into_search(RegisterTextForPaste('"'))) UpdateIncSearch();
        return;
    }
    if (ctrl_r) {
        prompt_pending_ctrl_r_ = true;
        return;
    }
    int cp = GetCharPressed();
    bool typed = false;
    while (cp > 0) {
        if (prompt_pending_ctrl_r_) {
            prompt_pending_ctrl_r_ = false;
            if (paste_into_search(RegisterTextForPaste(cp))) typed = true;
        } else if (cp >= 32 && cp < 127) {
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
    // While focused (Mode::HoverFocus), the real cursor never moves and
    // Escape is HandleHoverFocusInput's job (cancel selection, then leave
    // focus) -- this must not also see that same Escape and race it into
    // slamming hover_open_ shut a frame early, collapsing "Escape then
    // Escape" into a single press. HandleHoverFocusInput's own exit path
    // returns mode_ to Normal without moving the cursor, so the very next
    // frame's check below (mode_ == Normal, no fresh Escape yet) leaves
    // hover_open_ alone -- exactly the "one more Escape to actually close
    // it" behavior Mode::HoverFocus's doc comment describes.
    if (mode_ == Mode::HoverFocus) return;
    if (mode_ != Mode::Normal || IsKeyPressed(KEY_ESCAPE)) {
        hover_open_ = false;
        return;
    }
    CursorPos cur = Cursor();
    if (cur.row != hover_anchor_pos_.row || cur.col != hover_anchor_pos_.col) hover_open_ = false;
}

namespace {
// Local to hover-focus navigation -- deliberately not shared with
// main.cpp's identically-behaved SplitLines (editor.cpp doesn't link
// against main.cpp's translation unit).
/**
 * @brief Splits a hover popup's text into individual lines on '\n'.
 * @param text The full hover text to split.
 * @return The lines of `text`; always contains at least one (possibly empty) entry.
 */
std::vector<std::string> SplitHoverLines(const std::string &text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (true) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    if (lines.empty()) lines.emplace_back("");
    return lines;
}
}  // namespace

void Editor::EnterHoverFocus() {
    if (!hover_open_) return;
    overlay_previous_mode_ = mode_;
    hover_focus_row_ = 0;
    hover_focus_col_ = 0;
    hover_focus_scroll_ = 0;
    hover_focus_pending_g_ = false;
    hover_focus_pending_y_ = false;
    hover_focus_selecting_ = false;
    hover_focus_select_linewise_ = false;
    mode_ = Mode::HoverFocus;
}

// Read-only navigation over the open hover popup's own text (Mode::
// HoverFocus, entered via EnterHoverFocus): hjkl/arrows move a caret
// exactly the way Normal mode's does over a real buffer, gg/G jump to the
// first/last line, v/V start a charwise/linewise selection (toggled off by
// pressing the same key again, matching Visual mode), and y either yanks
// the active selection or -- pressed twice with no selection, mirroring
// "yy" -- the current line, into the unnamed/"0 registers so a plain 'p'
// back in the real buffer pastes it. Escape cancels a selection first;
// with no selection, it hands control back to RestoreFromOverlay (mode_
// only -- the real cursor was never touched), leaving hover_open_ itself
// alone so MaybeDismissHover's ordinary Escape-closes-it check on the next
// frame is what actually dismisses the popup, not this function. 'q'
// (vim's usual "close this window" key) skips that two-step dance and
// closes the popup outright in one press, regardless of selection state.
void Editor::HandleHoverFocusInput() {
    std::vector<std::string> lines = SplitHoverLines(hover_text_);
    int last_row = static_cast<int>(lines.size()) - 1;
    /**
     * @brief Clamps a column to a valid caret position within the given hover line.
     * @param row Index of the hover-text line the column is being clamped against.
     * @param col Candidate column value to clamp.
     * @return The column clamped to [0, line length - 1] (0 for an empty line).
     */
    auto clamp_col = [&](int row, int col) {
        int len = static_cast<int>(lines[static_cast<size_t>(row)].size());
        return std::max(0, std::min(col, std::max(0, len - 1)));
    };
    /**
     * @brief Moves the hover-focus caret up or down by a row delta, clamping row and column.
     * @param delta Number of rows to move by (negative moves up).
     */
    auto move_row = [&](int delta) {
        hover_focus_row_ = std::max(0, std::min(last_row, hover_focus_row_ + delta));
        hover_focus_col_ = clamp_col(hover_focus_row_, hover_focus_col_);
    };
    /**
     * @brief Begins a charwise or linewise selection anchored at the current hover-focus caret.
     * @param linewise Whether the new selection is linewise (true) or charwise (false).
     */
    auto start_select = [&](bool linewise) {
        hover_focus_selecting_ = true;
        hover_focus_select_linewise_ = linewise;
        hover_focus_select_anchor_row_ = hover_focus_row_;
        hover_focus_select_anchor_col_ = hover_focus_col_;
    };
    /**
     * @brief Extracts the text between an anchor position and the current hover-focus caret and
     *        yanks it into the unnamed/"0 registers.
     * @param anchor_row Row of the selection anchor.
     * @param anchor_col Column of the selection anchor.
     * @param linewise Whether the selection is linewise (whole lines) or charwise.
     */
    auto yank_selection = [&](int anchor_row, int anchor_col, bool linewise) {
        int r0 = anchor_row, c0 = anchor_col;
        int r1 = hover_focus_row_, c1 = hover_focus_col_;
        if (r0 > r1 || (r0 == r1 && c0 > c1)) {
            std::swap(r0, r1);
            std::swap(c0, c1);
        }
        std::string text;
        if (linewise) {
            for (int r = r0; r <= r1; r++) {
                text += lines[static_cast<size_t>(r)];
                text += '\n';
            }
        } else if (r0 == r1) {
            const std::string &line = lines[static_cast<size_t>(r0)];
            int a = std::min(c0, static_cast<int>(line.size()));
            int b = std::min(c1 + 1, static_cast<int>(line.size()));
            if (b > a) text = line.substr(static_cast<size_t>(a), static_cast<size_t>(b - a));
        } else {
            for (int r = r0; r <= r1; r++) {
                const std::string &line = lines[static_cast<size_t>(r)];
                if (r == r0) {
                    text += line.substr(static_cast<size_t>(std::min(c0, static_cast<int>(line.size()))));
                } else if (r == r1) {
                    text += line.substr(0, static_cast<size_t>(std::min(c1 + 1, static_cast<int>(line.size()))));
                } else {
                    text += line;
                }
                if (r != r1) text += '\n';
            }
        }
        Register &target = RegisterFor(0);
        target.text = text;
        target.linewise = linewise;
        target.blockwise = false;
        registers_['0'] = target;
        SyncUnnamedToSystemClipboard();
        SetStatusMessage("Yanked from hover doc");
    };

    if (IsKeyPressed(KEY_ESCAPE)) {
        hover_focus_pending_g_ = false;
        hover_focus_pending_y_ = false;
        if (hover_focus_selecting_) {
            hover_focus_selecting_ = false;
        } else {
            RestoreFromOverlay();
        }
        return;
    }

    if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN)) move_row(1);
    if (IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP)) move_row(-1);
    if (IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT)) hover_focus_col_ = std::max(0, hover_focus_col_ - 1);
    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT))
        hover_focus_col_ = clamp_col(hover_focus_row_, hover_focus_col_ + 1);

    for (int cp = GetCharPressed(); cp != 0; cp = GetCharPressed()) {
        if (hover_focus_pending_g_) {
            hover_focus_pending_g_ = false;
            if (cp == 'g') {
                hover_focus_row_ = 0;
                hover_focus_col_ = clamp_col(0, hover_focus_col_);
            }
            continue;
        }
        if (hover_focus_pending_y_) {
            hover_focus_pending_y_ = false;
            if (cp == 'y') yank_selection(hover_focus_row_, hover_focus_col_, /*linewise=*/true);
            continue;
        }
        switch (cp) {
            case 'h':
                hover_focus_col_ = std::max(0, hover_focus_col_ - 1);
                break;
            case 'l':
                hover_focus_col_ = clamp_col(hover_focus_row_, hover_focus_col_ + 1);
                break;
            case 'j':
                move_row(1);
                break;
            case 'k':
                move_row(-1);
                break;
            case '0':
                hover_focus_col_ = 0;
                break;
            case '$':
                hover_focus_col_ = clamp_col(hover_focus_row_, static_cast<int>(lines[static_cast<size_t>(hover_focus_row_)].size()));
                break;
            case 'g':
                hover_focus_pending_g_ = true;
                break;
            case 'G':
                hover_focus_row_ = last_row;
                hover_focus_col_ = clamp_col(last_row, hover_focus_col_);
                break;
            case 'v':
                if (hover_focus_selecting_ && !hover_focus_select_linewise_) {
                    hover_focus_selecting_ = false;
                } else {
                    start_select(false);
                }
                break;
            case 'V':
                if (hover_focus_selecting_ && hover_focus_select_linewise_) {
                    hover_focus_selecting_ = false;
                } else {
                    start_select(true);
                }
                break;
            case 'y':
                if (hover_focus_selecting_) {
                    yank_selection(hover_focus_select_anchor_row_, hover_focus_select_anchor_col_,
                                    hover_focus_select_linewise_);
                    hover_focus_selecting_ = false;
                } else {
                    hover_focus_pending_y_ = true;
                }
                break;
            case 'q':
                hover_focus_pending_g_ = false;
                hover_focus_pending_y_ = false;
                hover_focus_selecting_ = false;
                RestoreFromOverlay();
                hover_open_ = false;
                return;
            default:
                break;
        }
    }

    if (hover_focus_row_ < hover_focus_scroll_) hover_focus_scroll_ = hover_focus_row_;
    if (hover_focus_row_ >= hover_focus_scroll_ + kHoverFocusVisibleRows)
        hover_focus_scroll_ = hover_focus_row_ - kHoverFocusVisibleRows + 1;
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
    buffers_[static_cast<size_t>(buffer_id)].decorations.erase(ns);
}

int Editor::AddDecorationToBuffer(int buffer_id, int ns, Decoration deco) {
    if (buffer_id < 0 || buffer_id >= static_cast<int>(buffers_.size())) return 0;
    Buffer &buf = buffers_[static_cast<size_t>(buffer_id)];
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
    // Matches folds whose provider tag equals the one being cleared.
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
/**
 * @brief Counts how many other folds in `folds` strictly contain `target`'s row range.
 * @param folds The full set of folds to search for containing ranges.
 * @param target The fold whose nesting depth is being computed.
 * @return The number of distinct folds that strictly contain `target`.
 */
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
/**
 * @brief Computes the deepest fold nesting level present, 1-indexed like vim's 'foldlevel'.
 * @param folds The full set of folds to inspect.
 * @return The maximum nesting depth + 1 across all folds, or 0 if `folds` is empty.
 */
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

// Rebuilds provider="marker" folds from literal `{{{`/`}}}` text markers --
// vim's classic foldmethod=marker, with mep's default foldmarker of
// `{{{,}}}` -- and unlike RecomputeOrgFolds/MdComputeFolds, applies to
// every filetype: this is what lets main.cpp fold itself along the exact
// `{{{ module: X` / `}}} module: X` markers described in its own top-of-
// file comment. Markers are matched purely as text, left to right within
// and across lines (no comment-syntax awareness needed, same as vim's own
// default), with each `}}}` closing the innermost still-open `{{{`; an
// unmatched trailing `{{{` is simply left unclosed, same as vim. Existing
// marker folds are preserved by matching on start_row, same convention as
// RecomputeOrgFolds.
void Editor::RecomputeMarkerFolds() {
    std::vector<Fold> old_folds;
    for (const Fold &f : Buf().folds) {
        if (f.provider == "marker") old_folds.push_back(f);
    }
    ClearFoldsFromProvider("marker");

    std::vector<int> stack;  // rows of still-open `{{{` markers
    const int n = Buf().LineCount();
    for (int i = 0; i < n; i++) {
        const std::string &line = Buf().lines[static_cast<size_t>(i)];
        size_t pos = 0;
        while (pos < line.size()) {
            size_t open = line.find("{{{", pos);
            size_t close = line.find("}}}", pos);
            if (open == std::string::npos && close == std::string::npos) break;
            if (close == std::string::npos || (open != std::string::npos && open < close)) {
                stack.push_back(i);
                pos = open + 3;
            } else {
                if (!stack.empty()) {
                    int start_row = stack.back();
                    stack.pop_back();
                    bool fold_closed = false;
                    for (const Fold &of : old_folds) {
                        if (of.start_row == start_row) {
                            fold_closed = of.closed;
                            break;
                        }
                    }
                    CreateFold(start_row, i, fold_closed, "marker");
                }
                pos = close + 3;
            }
        }
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

void Editor::SetSidebarOnPreview(int id, int lua_ref) {
    if (SidebarInstance *sb = FindSidebarMut(id)) sb->on_preview_ref = lua_ref;
}

void Editor::SetSidebarTabs(int id, std::vector<std::string> tabs, int active) {
    SidebarInstance *sb = FindSidebarMut(id);
    if (!sb) return;
    sb->tabs = std::move(tabs);
    sb->active_tab = sb->tabs.empty() ? 0 : std::clamp(active, 0, static_cast<int>(sb->tabs.size()) - 1);
}

void Editor::SetSidebarOnTab(int id, int lua_ref) {
    if (SidebarInstance *sb = FindSidebarMut(id)) sb->on_tab_ref = lua_ref;
}

void Editor::SelectSidebarTab(int id, int index) {
    SidebarInstance *sb = FindSidebarMut(id);
    if (!sb || sb->tabs.empty()) return;
    const int n = static_cast<int>(sb->tabs.size());
    sb->active_tab = ((index % n) + n) % n;
    // A different view is a different list: start it from the top rather
    // than leaving the cursor/scroll parked at a row index that meant
    // something only in the previous one.
    sb->scroll_offset = 0;
    if (focused_sidebar_id_ == id) sidebar_cursor_ = 0;
    if (id == sidebar_popout_id_) sidebar_popout_preview_dirty_ = true;
    if (sb->on_tab_ref != 0 && lua_) lua_->CallRefWithInt(sb->on_tab_ref, sb->active_tab + 1);
}

int Editor::SidebarActiveTab(int id) const {
    const SidebarInstance *sb = FindSidebar(id);
    return sb ? sb->active_tab : 0;
}

// --- Sidebar popout (mod1+m) ---------------------------------------------

void Editor::OpenSidebarPopout(int id) {
    // Only the focused sidebar can be popped out: the popout reuses the
    // focused sidebar's own cursor/input path wholesale (see the header
    // comment), so popping out an unfocused one would show a float that
    // no key reaches.
    if (mode_ != Mode::Sidebar || id == 0 || id != focused_sidebar_id_) return;
    const SidebarInstance *sb = FindSidebar(id);
    if (!sb || !sb->open) return;
    if (sidebar_popout_id_ != id) {
        // A stale preview from an earlier popout (a different sidebar, or
        // this one before it was collapsed) must never flash up for a
        // frame before RefreshSidebarPopoutPreview's own callback replaces
        // it -- start from an empty column each time.
        SetSidebarPopoutPreview("");
        sidebar_popout_preview_key_.clear();
    }
    sidebar_popout_id_ = id;
    sidebar_popout_preview_dirty_ = true;
}

void Editor::CloseSidebarPopout() {
    sidebar_popout_id_ = 0;
    sidebar_popout_preview_key_.clear();
    sidebar_popout_preview_dirty_ = false;
    SetSidebarPopoutPreview("");
}

void Editor::ToggleSidebarPopout(int id) {
    if (id == 0) id = focused_sidebar_id_;
    if (SidebarPopoutActive() && sidebar_popout_id_ == id) {
        CloseSidebarPopout();
    } else {
        OpenSidebarPopout(id);
    }
}

void Editor::RefreshSidebarPopoutPreview() {
    if (!SidebarPopoutActive()) {
        // Focus left the popped-out sidebar by some route that doesn't go
        // through CloseSidebarPopout (mod1+hjkl into the panes, a widget's
        // on_click opening a file, ...): forget the popout entirely so
        // refocusing the sidebar later comes back docked, not still
        // zoomed -- and so a later mod1+m opens fresh rather than toggling
        // a phantom popout closed.
        if (sidebar_popout_id_ != 0) CloseSidebarPopout();
        return;
    }
    const SidebarInstance *sb = FindSidebar(sidebar_popout_id_);
    if (!sb) return;
    // Row identity, not just the widget id: two section headers both
    // resolve to "" as a widget id, and a header row must still tell the
    // source "nothing is under the cursor now" once, so it can clear a
    // previous widget's preview.
    std::vector<SidebarLine> lines = FlattenSidebar(sidebar_popout_id_);
    std::string key;
    if (sidebar_cursor_ >= 0 && sidebar_cursor_ < static_cast<int>(lines.size())) {
        const SidebarLine &line = lines[static_cast<size_t>(sidebar_cursor_)];
        key = line.kind == SidebarLine::Kind::Widget ? "w:" + SidebarLineWidgetId(sidebar_popout_id_, sidebar_cursor_)
                                                      : "h:" + std::to_string(line.section_index);
    }
    if (!sidebar_popout_preview_dirty_ && key == sidebar_popout_preview_key_) return;
    sidebar_popout_preview_dirty_ = false;
    sidebar_popout_preview_key_ = key;
    if (sb->on_preview_ref == 0 || !lua_) return;
    lua_->CallRefWithString(sb->on_preview_ref, SidebarCursorWidgetId(sidebar_popout_id_));
}

void Editor::ScrollSidebarPopoutPreview(int delta) {
    int line_count = 1 + static_cast<int>(std::count(sidebar_popout_preview_text_.begin(),
                                                     sidebar_popout_preview_text_.end(), '\n'));
    int max_scroll = std::max(0, line_count - 1);
    sidebar_popout_preview_scroll_ = std::clamp(sidebar_popout_preview_scroll_ + delta, 0, max_scroll);
}

std::string Editor::SidebarCursorWidgetId(int id) const {
    const SidebarInstance *sb = FindSidebar(id);
    if (!sb) return "";
    std::vector<SidebarLine> lines = FlattenSidebar(id);
    if (sidebar_cursor_ < 0 || sidebar_cursor_ >= static_cast<int>(lines.size())) return "";
    const SidebarLine &line = lines[static_cast<size_t>(sidebar_cursor_)];
    if (line.kind != SidebarLine::Kind::Widget) return "";
    return sb->sections[static_cast<size_t>(line.section_index)].widgets[static_cast<size_t>(line.widget_index)].id;
}

void Editor::FocusSidebarRow(int id, int line_index) {
    const SidebarInstance *sb = FindSidebarMut(id);
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
    pending_g_ = false;  // avoid gg/G leakage from whatever mode was active before this click
    focused_sidebar_id_ = id;
    mode_ = Mode::Sidebar;
    std::vector<SidebarLine> lines = FlattenSidebar(id);
    sidebar_cursor_ = std::clamp(line_index, 0, std::max(0, static_cast<int>(lines.size()) - 1));
}

std::string Editor::SidebarLineWidgetId(int id, int line_index) const {
    std::vector<SidebarLine> lines = FlattenSidebar(id);
    if (line_index < 0 || line_index >= static_cast<int>(lines.size())) return "";
    const SidebarLine &line = lines[static_cast<size_t>(line_index)];
    if (line.kind != SidebarLine::Kind::Widget) return "";
    for (const SidebarInstance &sb : sidebars_) {
        if (sb.id != id) continue;
        if (line.section_index < 0 || line.section_index >= static_cast<int>(sb.sections.size())) return "";
        const SidebarSection &section = sb.sections[static_cast<size_t>(line.section_index)];
        if (line.widget_index < 0 || line.widget_index >= static_cast<int>(section.widgets.size())) return "";
        return section.widgets[static_cast<size_t>(line.widget_index)].id;
    }
    return "";
}

void Editor::ActivateSidebarLine(int id, int line_index) {
    std::vector<SidebarLine> lines = FlattenSidebar(id);
    if (line_index < 0 || line_index >= static_cast<int>(lines.size())) return;
    const SidebarLine &line = lines[static_cast<size_t>(line_index)];
    SidebarInstance *sb = FindSidebarMut(id);
    if (!sb) return;
    if (line.kind == SidebarLine::Kind::SectionHeader) {
        sb->sections[static_cast<size_t>(line.section_index)].collapsed = !sb->sections[static_cast<size_t>(line.section_index)].collapsed;
        if (id == focused_sidebar_id_) {
            int max_idx = static_cast<int>(FlattenSidebar(id).size()) - 1;
            sidebar_cursor_ = std::min(sidebar_cursor_, std::max(0, max_idx));
        }
    } else if (line.kind == SidebarLine::Kind::Widget) {
        int ref = sb->sections[static_cast<size_t>(line.section_index)].widgets[static_cast<size_t>(line.widget_index)].on_click_ref;
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
    // Every open sidebar on the same edge renders in one merged column
    // (DrawSidebars, DockSize), so a resize of any one of them is a resize
    // of the column: keep the members equal rather than letting the max
    // silently win and the dragged sidebar's own size drift underneath it.
    for (SidebarInstance &other : sidebars_) {
        if (other.open && other.position == sb->position) other.size = sb->size;
    }
}

std::vector<int> Editor::OpenSidebarIdsOn(const std::string &position) const {
    std::vector<int> ids;
    for (const SidebarInstance &sb : sidebars_) {
        if (sb.open && sb.position == position) ids.push_back(sb.id);
    }
    return ids;
}

int Editor::DockSize(const std::string &position) const {
    int size = 0;
    for (const SidebarInstance &sb : sidebars_) {
        if (sb.open && sb.position == position) size = std::max(size, sb.size);
    }
    return size;
}

void Editor::SetSidebarStackShares(int upper_id, int lower_id, float upper_fraction) {
    SidebarInstance *upper = FindSidebarMut(upper_id);
    SidebarInstance *lower = FindSidebarMut(lower_id);
    if (!upper || !lower || upper == lower) return;
    // The pair's combined share is held constant (same idea as
    // SetPaneBorderShare's border_pair_total) so moving one divider never
    // disturbs the other members of the stack.
    float total = upper->stack_share + lower->stack_share;
    if (total <= 0.0f) total = 2.0f;
    constexpr float kMinStackFraction = 0.1f;  // keeps at least a title row's worth of each visible
    upper_fraction = std::clamp(upper_fraction, kMinStackFraction, 1.0f - kMinStackFraction);
    upper->stack_share = upper_fraction * total;
    lower->stack_share = total - upper->stack_share;
}

bool Editor::SwapSidebarInStack(int id, const std::string &direction) {
    if (direction != "up" && direction != "down") return false;
    const SidebarInstance *sb = FindSidebar(id);
    if (!sb || !sb->open || (sb->position != "left" && sb->position != "right")) return false;
    const std::vector<int> ids = OpenSidebarIdsOn(sb->position);
    auto it = std::find(ids.begin(), ids.end(), id);
    if (it == ids.end()) return false;
    const int index = static_cast<int>(it - ids.begin()) + (direction == "down" ? 1 : -1);
    if (index < 0 || index >= static_cast<int>(ids.size())) return false;
    const int other_id = ids[static_cast<size_t>(index)];
    auto by_id = [&](int want) {
        return std::find_if(sidebars_.begin(), sidebars_.end(), [want](const SidebarInstance &s) { return s.id == want; });
    };
    auto a = by_id(id), b = by_id(other_id);
    if (a == sidebars_.end() || b == sidebars_.end() || a == b) return false;
    // Closed sidebars (or ones docked elsewhere) sitting between the two
    // in sidebars_ don't participate in this edge's stack, so swapping the
    // two entries outright -- rather than rotating the range -- is exactly
    // "these two trade slots" as far as OpenSidebarIdsOn is concerned.
    std::iter_swap(a, b);
    return true;
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
    // The row under the popout's cursor may now name something else (a
    // git status refresh reordering its list, the file tree expanding a
    // directory) even though the cursor index itself never moved.
    if (id == sidebar_popout_id_) sidebar_popout_preview_dirty_ = true;
}

void Editor::OpenSidebar(int id, bool focus) {
    SidebarInstance *sb = FindSidebarMut(id);
    if (!sb) return;
    sb->open = true;
    if (focus) {
        // Only capture the mode to return to on the genuine transition into
        // sidebar focus. Refocusing from one sidebar to another (mod1+j/k
        // through a stack, a tab-bar button opening a second panel while
        // the first is focused, :MepGitStatus from inside the file tree)
        // arrives here with mode_ already Sidebar -- capturing that would
        // make RestoreFromOverlay (mod1+h/l back into the panes, Escape, q)
        // "restore" straight back into Sidebar mode, trapping focus there
        // until the sidebar was closed some other way. Same guard as
        // FocusSidebarRow's.
        if (mode_ != Mode::Sidebar) overlay_previous_mode_ = mode_;
        pending_g_ = false;  // avoid gg/G leakage from whatever mode was active before this
        focused_sidebar_id_ = id;
        sidebar_cursor_ = 0;
        mode_ = Mode::Sidebar;
    }
}

void Editor::CloseSidebar(int id) {
    SidebarInstance *sb = FindSidebarMut(id);
    if (!sb) return;
    sb->open = false;
    if (id == sidebar_popout_id_) CloseSidebarPopout();
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
        const SidebarSection &sec = sb->sections[static_cast<size_t>(si)];
        if (!sec.title.empty()) {
            SidebarLine line;
            line.kind = SidebarLine::Kind::SectionHeader;
            line.section_index = si;
            line.text = std::string(sec.collapsed ? "+ " : "- ") + sec.title;
            out.push_back(line);
        }
        if (sec.collapsed) continue;
        for (int wi = 0; wi < static_cast<int>(sec.widgets.size()); wi++) {
            const SidebarWidget &w = sec.widgets[static_cast<size_t>(wi)];
            SidebarLine line;
            line.kind = SidebarLine::Kind::Widget;
            line.section_index = si;
            line.widget_index = wi;
            line.text = (w.icon.empty() ? "  " : "  " + w.icon + " ") + w.text;
            line.hl = w.hl;
            line.current = w.current;
            out.push_back(line);
        }
    }
    return out;
}

void Editor::UpdateScrollForSidebar(int id, int visible_lines) {
    SidebarInstance *sb = FindSidebarMut(id);
    if (!sb) return;
    visible_lines = std::max(1, visible_lines);
    int total = static_cast<int>(FlattenSidebar(id).size());
    int max_scroll = std::max(0, total - visible_lines);
    // Only the focused sidebar has a live cursor to chase -- an unfocused
    // one (another open sidebar, or this one after mod1+hjkl blurred it
    // back into the pane tree) just gets its scroll_offset clamped back in
    // range below, same as a pane that shrank out from under its own
    // scroll_row.
    if (id == focused_sidebar_id_ && mode_ == Mode::Sidebar) {
        if (sidebar_cursor_ < sb->scroll_offset) {
            sb->scroll_offset = sidebar_cursor_;
        } else if (sidebar_cursor_ >= sb->scroll_offset + visible_lines) {
            sb->scroll_offset = sidebar_cursor_ - visible_lines + 1;
        }
    }
    sb->scroll_offset = std::clamp(sb->scroll_offset, 0, max_scroll);
}

void Editor::HandleSidebarInput() {
    std::vector<SidebarLine> lines = FlattenSidebar(focused_sidebar_id_);
    bool escape = false, enter = false;
    int tab_delta = 0;
    const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed()) {
        if (key == KEY_ESCAPE) escape = true;
        else if (key == KEY_ENTER) enter = true;
        else if (key == KEY_TAB) tab_delta += shift ? -1 : 1;
    }
    // Tab/Shift-Tab: next/previous view of a tabbed sidebar
    // (SidebarInstance::tabs) -- a no-op for one without tabs.
    if (tab_delta != 0) {
        pending_g_ = false;
        SelectSidebarTab(focused_sidebar_id_, SidebarActiveTab(focused_sidebar_id_) + tab_delta);
        return;
    }
    // While popped out (mod1+m), Escape/q step back down to the docked
    // panel instead of closing the sidebar outright -- the popout is a
    // temporary zoom, and the natural "undo" of a zoom is un-zooming, not
    // losing the panel (and its cursor) entirely. A second q then closes
    // as before. Escape on a *docked* sidebar deliberately does nothing:
    // closing is reserved for q and mod1+d (HandleMod1Shortcuts), so a
    // reflexive Escape can't throw away the panel and its cursor.
    const bool popped_out = SidebarPopoutActive();
    if (escape) {
        pending_g_ = false;  // an unmatched 'g' shouldn't survive an Escape
        if (popped_out) CloseSidebarPopout();
        return;
    }
    if (enter) {
        pending_g_ = false;  // don't leak a lone unmatched 'g' into whatever mode this activation lands in
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
        // gg/G: jump to the first/last flattened line, same "G always
        // resolves immediately, g waits one more keystroke" shape as
        // Normal mode's own pending_g_ (DispatchNormalKey) -- reusing that
        // same flag here since Sidebar is its own mode and can't otherwise
        // collide with Normal mode's use of it. Any key below that isn't
        // itself continuing a pending 'g' clears it, so a stray 'g' can't
        // wrongly arm a later unrelated 'G'.
        if (cp == 'G') {
            sidebar_cursor_ = std::max(0, static_cast<int>(lines.size()) - 1);
            pending_g_ = false;
        } else if (cp == 'g' && pending_g_) {
            sidebar_cursor_ = 0;
            pending_g_ = false;
        } else if (cp == 'g') {
            pending_g_ = true;
        } else if (cp == 'j' && sidebar_cursor_ + 1 < static_cast<int>(lines.size())) {
            sidebar_cursor_++;
            pending_g_ = false;
        } else if (cp == 'k' && sidebar_cursor_ > 0) {
            sidebar_cursor_--;
            pending_g_ = false;
        } else if (cp == 'q') {
            pending_g_ = false;  // don't leak a lone unmatched 'g' into whatever mode q restores
            if (popped_out) {
                CloseSidebarPopout();  // same un-zoom-first rule as Escape above
                return;
            }
            int id = focused_sidebar_id_;
            RestoreFromOverlay();
            CloseSidebar(id);
            return;
        } else if (lua_) {
            pending_g_ = false;
            const SidebarInstance *sb = FindSidebar(focused_sidebar_id_);
            if (sb && sb->on_key_ref != 0 && cp >= 32 && cp < 127) {
                lua_->CallRefWithString(sb->on_key_ref, std::string(1, static_cast<char>(cp)));
                // The callback just run (e.g. mep.tree_on_key's 'a'/'r'/'d')
                // may have opened a Prompt/Confirm overlay, which leaves
                // this sidebar's own mode -- stop draining the char queue
                // the instant that happens, rather than keep feeding
                // whatever's left in it to tree_on_key as more single-key
                // commands. Left undrained, a still-queued 'a'/'r'/'d'/'o'
                // (e.g. the leading letters of a filename typed quickly
                // right after the 'a' that opened this very prompt) would
                // re-enter mep.ui_input/ui_confirm while one is already
                // open, capturing Mode::Prompt itself as
                // overlay_previous_mode_ instead of the real prior mode --
                // an unrecoverable stuck prompt (Escape/Enter restore back
                // into Mode::Prompt) that was reproduced exactly this way.
                // Any characters left in the queue roll over to next
                // frame's dispatch, which by then reads the *new* mode and
                // routes them to the right handler (e.g. HandlePromptInput,
                // so they land in the prompt's own text instead).
                if (mode_ != Mode::Sidebar) return;
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

std::string HlGroupForFilename(const std::string &name) {
    // Filename/extension -> highlight-group name (one of BuildHighlightGroups'
    // named roles), so file tree rows read as "the theme's blue/green/etc."
    // rather than a hardcoded RGB value. Grouped by rough language/file
    // family rather than 1:1 with IconForFilename's glyph choice, since the
    // palette only has 7 hue roles to work with.
    static const std::unordered_map<std::string, const char *> kByName = {
        {"Makefile", "Red"},        {"makefile", "Red"},        {"CMakeLists.txt", "Red"},
        {"Dockerfile", "Red"},      {".gitignore", "Orange"},   {".gitmodules", "Orange"},
        {"README.md", "Cyan"},      {"README.org", "Cyan"},     {"README", "Cyan"},
        {"LICENSE", "MutedFg"},     {".env", "Yellow"},         {".editorconfig", "MutedFg"},
    };
    auto by_name = kByName.find(name);
    if (by_name != kByName.end()) return by_name->second;

    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == name.size() - 1) return "Normal";
    std::string ext = name.substr(dot + 1);
    for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    static const std::unordered_map<std::string, const char *> kByExt = {
        {"c", "Blue"},      {"h", "Purple"},    {"cpp", "Purple"},  {"cc", "Purple"},   {"cxx", "Purple"},
        {"hpp", "Purple"},  {"lua", "Blue"},    {"py", "Yellow"},   {"js", "Yellow"},   {"ts", "Blue"},
        {"jsx", "Yellow"},  {"tsx", "Blue"},    {"rs", "Orange"},   {"go", "Cyan"},     {"java", "Red"},
        {"rb", "Red"},      {"sh", "Green"},    {"bash", "Green"},  {"zsh", "Green"},
        {"md", "Cyan"},     {"markdown", "Cyan"}, {"org", "Green"}, {"txt", "MutedFg"}, {"json", "Yellow"},
        {"yaml", "Yellow"}, {"yml", "Yellow"},  {"toml", "Yellow"}, {"xml", "Orange"},  {"html", "Orange"},
        {"css", "Blue"},    {"scss", "Blue"},   {"sql", "Purple"},  {"vim", "Green"},   {"lock", "MutedFg"},
        {"log", "MutedFg"}, {"cs", "Red"},      {"php", "Purple"},  {"csv", "Green"},   {"env", "Yellow"},
        {"git", "Orange"},  {"png", "Purple"},  {"jpg", "Purple"},  {"jpeg", "Purple"}, {"gif", "Purple"},
        {"svg", "Purple"},  {"pdf", "Red"},     {"zip", "Orange"},  {"tar", "Orange"},  {"gz", "Orange"},
    };
    auto by_ext = kByExt.find(ext);
    if (by_ext != kByExt.end()) return by_ext->second;
    return "Normal";
}

// --- Fuzzy picker ----------------------------------------------------------

int FuzzyScore(const std::string &str, const std::string &query, std::vector<int> *positions) {
    if (positions) positions->clear();
    if (query.empty()) return 0;
    // True if the query contains any uppercase letter, enabling smart-case matching.
    bool smart_case = std::any_of(query.begin(), query.end(), [](unsigned char c) { return std::isupper(c); });
    /**
     * @brief Normalizes a character for comparison, lowercasing it unless smart-case matching
     *        (an uppercase letter in the query) is active.
     * @param c The character to normalize.
     * @return `c` unchanged if `smart_case` is set, otherwise its lowercased form.
     */
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
    score -= static_cast<int>(static_cast<double>(str.size()) * 0.01);
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
    picker_preview_spans_.clear();
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
    // Orders scored items by descending fuzzy-match score (best match first).
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
            pre_nav_data = pre_nav[static_cast<size_t>(picker_selected_)].data;
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
        std::string data = has_selection ? results[static_cast<size_t>(picker_selected_)].data : std::string();
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
            const std::string &post_nav_data = post_nav[static_cast<size_t>(picker_selected_)].data;
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
            FuzzyScore(roam_graph_nodes_[static_cast<size_t>(i)].title, roam_graph_query_) >= 0) {
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
        std::string path = has_selection ? roam_graph_nodes_[static_cast<size_t>(filtered[static_cast<size_t>(roam_graph_selected_)])].path : std::string();
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

// Enter is the only non-printable key a leader sequence can contain (see
// HandleWhichKeyInput); it's a literal '\r' byte internally, spelled
// "<CR>" at the Lua/display boundary. Case-insensitive on the way in
// ("<cr>"/"<Cr>"), and "<Return>"/"<Enter>" are accepted too, since all
// three spellings are common in vim configs -- the display form is
// always the canonical "<CR>".
std::string Editor::NormalizeWhichKeySequence(const std::string &seq) {
    static const char *const kEnterSpellings[] = {"<cr>", "<return>", "<enter>"};
    std::string out;
    size_t i = 0;
    while (i < seq.size()) {
        bool matched = false;
        if (seq[i] == '<') {
            for (const char *spelling : kEnterSpellings) {
                size_t n = std::strlen(spelling);
                if (i + n > seq.size()) continue;
                bool eq = true;
                for (size_t k = 0; k < n && eq; k++) {
                    eq = std::tolower(static_cast<unsigned char>(seq[i + k])) == spelling[k];
                }
                if (eq) {
                    out += '\r';
                    i += n;
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) out += seq[i++];
    }
    return out;
}

std::string Editor::WhichKeySequenceDisplay(const std::string &seq) {
    std::string out;
    for (char c : seq) {
        if (c == '\r') {
            out += "<CR>";
        } else {
            out += c;
        }
    }
    return out;
}

void Editor::RegisterWhichKey(const std::string &sequence, const std::string &description, int lua_ref) {
    whichkey_bindings_.push_back({NormalizeWhichKeySequence(sequence), description, lua_ref});
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
            out.emplace_back(b.sequence.substr(whichkey_prefix_.size()), b.description);
        }
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> Editor::WhichKeyDisplayEntries() const {
    // Bucket the raw (remaining-suffix, description) matches by their very
    // next character -- a "()" (empty-remainder) entry can't occur here,
    // since HandleWhichKeyInput fires and leaves WhichKey mode the instant
    // whichkey_prefix_ exactly equals some binding's full sequence.
    std::map<char, std::vector<std::pair<std::string, std::string>>> by_next_char;
    for (const auto &m : WhichKeyMatches()) by_next_char[m.first[0]].push_back(m);

    std::vector<std::pair<std::string, std::string>> out;
    for (const auto &bucket : by_next_char) {
        const auto &leaves = bucket.second;
        auto group_it = leaves.size() > 1 ? whichkey_groups_.find(whichkey_prefix_ + bucket.first) : whichkey_groups_.end();
        if (group_it != whichkey_groups_.end()) {
            out.emplace_back(WhichKeySequenceDisplay(std::string(1, bucket.first)), "+" + group_it->second);
        } else {
            for (const auto &leaf : leaves) out.emplace_back(WhichKeySequenceDisplay(leaf.first), leaf.second);
        }
    }
    return out;
}

void Editor::HandleWhichKeyInput() {
    // Enter arrives only through the key queue -- raylib's char queue
    // (GetCharPressed) never carries it -- so it's picked up here, as the
    // one non-printable key a sequence may contain (stored as '\r', see
    // NormalizeWhichKeySequence). Escape still cancels, as before.
    int cp = 0;
    for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed()) {
        if (key == KEY_ESCAPE) {
            RestoreFromOverlay();
            return;
        }
        if (key == KEY_ENTER || key == KEY_KP_ENTER) cp = '\r';
    }
    if (cp == 0) {
        cp = GetCharPressed();
        if (cp <= 0) return;
        if (cp < 32 || cp > 127) return;
    }
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
        status_message_ = "No such group: " + WhichKeySequenceDisplay(whichkey_prefix_);
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

/**
 * @brief Produces the hint label for a match at the given index, home-row-first.
 * @param i Zero-based index of the match to label.
 * @return A one-character label for the first 26 matches, or a two-character
 *         combination from `kHintLabelPool` for indices beyond that (up to 702).
 */
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
        const std::string &line = buf.lines[static_cast<size_t>(row)];
        for (int col = 0; col < static_cast<int>(line.size()); col++) {
            if (line[static_cast<size_t>(col)] == target) matches.push_back({row, col, ""});
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
    // True if any hint label still has hint_typed_ as a prefix (so typing could still narrow to it).
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
    const std::string &line = Buf().lines[static_cast<size_t>(cursor.row)];
    int start = cursor.col;
    while (start > 0 && (std::isalnum(static_cast<unsigned char>(line[static_cast<size_t>(start - 1)])) || line[static_cast<size_t>(start - 1)] == '_')) start--;
    std::string prefix = line.substr(static_cast<size_t>(start), static_cast<size_t>(cursor.col - start));
    // Member-access trigger: cursor sits right after a bare '.' with
    // nothing typed since (prefix empty, since '.' isn't alnum/'_' so the
    // backward scan above stops on it immediately) -- e.g. "np." for
    // numpy's own exported names. Recognized the same way any other
    // prefix is, just with an empty one, rather than requiring 2+ chars
    // the way a plain identifier-word query does (NVIM_PARITY_PLAN.md
    // Phase 22 gap: dotted/member completion never reached the
    // completion source at all before this).
    bool dot_trigger = prefix.empty() && start > 0 && line[static_cast<size_t>(start - 1)] == '.';
    if (prefix.size() < 2 && !dot_trigger) {
        completion_open_ = false;
        completion_last_query_prefix_ = "\x01";
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
    // completion_last_query_prefix_ is reset to "\x01" (a value prefix can
    // never equal -- it's always either "" for a dot-trigger or >=2 chars
    // otherwise, never a single byte) on Insert-mode exit (EnterNormal) so
    // a later session can't skip its first query by coincidentally
    // starting with the same prefix text some earlier, unrelated session
    // ended on -- "" specifically can't be reused as that sentinel once a
    // dot-trigger's own real prefix is "".
    if (prefix == completion_last_query_prefix_) return;
    constexpr double kMinQueryIntervalSec = 0.05;
    double now = GetTime();
    if (now - completion_last_query_time_ < kMinQueryIntervalSec) return;
    completion_last_query_prefix_ = prefix;
    completion_last_query_time_ = now;

    std::vector<std::string> texts, kinds, details, docs;
    if (!lua_->CallRefWithStringForCompletionItems(completion_source_ref_, prefix, &texts, &kinds, &details, &docs) ||
        texts.empty()) {
        completion_open_ = false;
        return;
    }
    completion_items_.clear();
    for (size_t i = 0; i < texts.size(); i++) completion_items_.push_back({texts[i], kinds[i], details[i], docs[i]});
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
    const std::string word = completion_items_[static_cast<size_t>(completion_selected_)].text;
    CursorPos &cursor = CurPane().cursor;
    std::string &line = Buf().lines[static_cast<size_t>(cursor.row)];
    PushUndo();
    line.erase(static_cast<size_t>(completion_word_start_col_), static_cast<size_t>(cursor.col - completion_word_start_col_));
    line.insert(static_cast<size_t>(completion_word_start_col_), word);
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

bool Editor::CompletionResolveInfo(const std::string &text, std::string *detail, std::string *doc) const {
    if (completion_resolve_hook_ref_ == 0 || !lua_) return false;
    return lua_->CallRefWithStringForDetailDoc(completion_resolve_hook_ref_, text, detail, doc);
}

// Display name for a Mode -- the status line's own label (main.cpp) and,
// since the addition of the agent-control socket, event.modeChanged's
// "mode" field (agent_rpc.cpp) both want the same string for the same
// mode, hence living here rather than as a main.cpp-private helper.
const char *ModeName(Mode m, bool replace_mode) {
    switch (m) {
        case Mode::Normal: return "NORMAL";
        case Mode::Insert: return replace_mode ? "REPLACE" : "INSERT";
        case Mode::Visual: return "VISUAL";
        case Mode::VisualLine: return "V-LINE";
        case Mode::VisualBlock: return "V-BLOCK";
        case Mode::Command: return "COMMAND";
        case Mode::SearchForward:
        case Mode::SearchBackward:
            return "SEARCH";
        case Mode::Prompt: return "INPUT";
        case Mode::Confirm: return "CONFIRM";
        case Mode::Select: return "SELECT";
        case Mode::Preview: return "PREVIEW";
        case Mode::Sidebar: return "SIDEBAR";
        case Mode::Picker: return "PICKER";
        case Mode::RoamGraph: return "ROAM-GRAPH";
        case Mode::WhichKey: return "WHICHKEY";
        case Mode::HintChar:
        case Mode::HintLabel:
            return "HINT";
        case Mode::Terminal: return "TERMINAL";
        case Mode::Image: return "IMAGE";
        case Mode::Pdf: return "PDF";
        case Mode::Html: return "HTML";
        case Mode::OfficeNormal: return "NORMAL";
        case Mode::OfficeInsert: return "INSERT";
        case Mode::OfficeVisual: return "VISUAL";
        case Mode::SheetNormal: return "NORMAL";
        case Mode::SheetInsert: return "INSERT";
        case Mode::SheetVisual: return "VISUAL";
        case Mode::KanbanNormal: return "NORMAL";
        case Mode::KanbanInsert: return "INSERT";
        case Mode::GanttNormal: return "NORMAL";
        case Mode::GanttInsert: return "INSERT";
        case Mode::HoverFocus: return "HOVER";
    }
    return "?";
}

ThemeColor ParticipantColor(const std::string &id) {
    // A small fixed palette of vivid, mutually-distinguishable colors --
    // chosen to read reasonably against both a light and a dark theme's
    // background without per-theme tuning (this codebase has no existing
    // "identity color" infrastructure to hook into, see this function's
    // own declaration comment in editor.h).
    static const ThemeColor kPalette[] = {
        {230, 76, 60, 255},   // red
        {230, 148, 40, 255},  // orange
        {170, 185, 40, 255},  // yellow-green
        {46, 184, 110, 255},  // green
        {32, 178, 170, 255},  // teal
        {52, 130, 230, 255},  // blue
        {155, 89, 220, 255},  // purple
        {230, 90, 160, 255},  // pink
    };
    // FNV-1a: cheap, stable across runs (unlike std::hash<std::string>,
    // which is only guaranteed stable within one process execution --
    // fine here since ids aren't persisted across restarts anyway, but
    // FNV-1a is just as simple to write directly).
    uint32_t hash = 2166136261u;
    for (char raw : id) {
        unsigned char c = static_cast<unsigned char>(raw);
        hash ^= c;
        hash *= 16777619u;
    }
    return kPalette[hash % (sizeof(kPalette) / sizeof(kPalette[0]))];
}

// --- Command-line completion (`:` command bar) -----------------------------

namespace {

// Every literal name ExecuteCommandLine's dispatch chain recognizes after
// the range-command special cases (:s :g :v :d :y :m :t/:co, which take
// inline syntax rather than a plain "name<space>args" shape and aren't
// useful to offer here). Kept as a plain list rather than derived from the
// dispatch chain itself since that chain is an if/else ladder, not a table.
/**
 * @brief Returns the static list of builtin command-line command names offered for completion.
 * @return A reference to the static list of builtin command names.
 */
const std::vector<std::string> &BuiltinCommandNames() {
    static const std::vector<std::string> kNames = {
        "w", "write", "wa", "wall", "q", "quit", "q!", "quit!", "qa", "qall", "qa!", "qall!",
        "wq", "x", "wqa", "xa", "wqall", "xall", "e", "edit", "e!", "edit!", "split", "sp", "vsplit", "vs",
        "terminal", "term",
        "close", "tabnew", "tabdelete", "tabclose", "tabnext", "tabn", "tabprevious", "tabp", "tabN",
        "wsnew", "wsnew!", "wsdelete", "wsdelete!", "wsclose", "wsclose!", "wsnext", "wsn", "wsprevious", "wsp",
        "wsrename", "ws", "workspace", "wslist", "workspaces", "wsadopt", "wsprune", "wssave", "wsrestore",
        "project", "projectclose", "projectclose!", "projectnext", "projectn", "projectprevious", "projectprev",
        "projectp", "projects",
        "bnext", "bn", "bprevious", "bprev", "bp", "bNext", "bN", "bdelete", "bd", "bdelete!", "bd!",
        "set", "normal", "norm", "normal!", "norm!", "MepNotifyClear", "MepNotifyDismiss",
        "MepNotifyPanel", "MepLayout", "MepScratch", "MepZen", "colorscheme", "colo", "lua", "source",
        "MepNextSheet", "MepPrevSheet", "Kanban", "Gantt", "Org", "Text", "CollabJoin", "CollabLeave", "CollabStatus",
        "AgentSocket",
    };
    return kNames;
}

// Commands whose last argument is a filesystem path, so completion after
// the command name should list directory entries rather than command names.
/**
 * @brief Checks whether a command name's last argument is a filesystem path.
 * @param name The command name to check.
 * @return True if `name` is one of the commands that takes a file-path argument.
 */
bool CommandTakesFileArg(const std::string &name) {
    static const std::vector<std::string> kFileCommands = {
        "w", "write", "wq", "x", "wqa", "xa", "wqall", "xall", "e", "edit", "e!", "edit!",
        "split", "sp", "vsplit", "vs", "tabnew", "source", "project",
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
        // Drop entries whose name doesn't start with the already-typed prefix.
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                      [&](const DirEntry &e) {
                                          return e.name.compare(0, base_prefix.size(), base_prefix) != 0;
                                      }),
                      entries.end());
        // Directories sort before files; within each group, sort by name.
        std::sort(entries.begin(), entries.end(), [](const DirEntry &a, const DirEntry &b) {
            if (a.is_dir != b.is_dir) return a.is_dir;
            return a.name < b.name;
        });
        for (const DirEntry &e : entries) candidates.push_back(dir + e.name + (e.is_dir ? "/" : ""));
        word_start = static_cast<int>(token_start);
    }

    if (candidates.empty()) return;

    if (candidates.size() == 1) {
        command_line_.replace(static_cast<size_t>(word_start), std::string::npos, candidates[0]);
        return;
    }

    std::string common = candidates[0];
    for (size_t i = 1; i < candidates.size(); i++) {
        size_t j = 0;
        while (j < common.size() && j < candidates[i].size() && common[j] == candidates[i][j]) j++;
        common.resize(j);
    }
    command_line_.replace(static_cast<size_t>(word_start), std::string::npos, common);

    for (const std::string &c : candidates) cmdline_completion_items_.push_back({c, c, {}});
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
    const std::string &item = cmdline_completion_items_[static_cast<size_t>(cmdline_completion_selected_)].data;
    command_line_.replace(static_cast<size_t>(cmdline_completion_word_start_), std::string::npos, item);
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
    completion_last_query_prefix_ = "\x01";
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
    pending_capital_z_ = false;
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
    const std::string &line = Buf().lines[static_cast<size_t>(result.row)];
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
/**
 * @brief Lowercases a string using ASCII-only case folding (no locale/Unicode support).
 * @param s The string to lowercase.
 * @return A copy of `s` with each ASCII uppercase letter converted to lowercase.
 */
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
            const std::string &line = Buf().lines[static_cast<size_t>(r)];
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
        const std::string &line = Buf().lines[static_cast<size_t>(r)];
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
    last_search_ = Buf().lines[static_cast<size_t>(start.row)].substr(static_cast<size_t>(start.col), static_cast<size_t>(end.col - start.col));
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
        CharClass start_class = ClassOf(Buf().lines[static_cast<size_t>(row)][static_cast<size_t>(col)]);
        if (start_class != CharClass::Space) {
            while (col < LineLen(row) && ClassOf(Buf().lines[static_cast<size_t>(row)][static_cast<size_t>(col)]) == start_class) col++;
        }
    }
    while (col >= LineLen(row) || ClassOf(Buf().lines[static_cast<size_t>(row)][static_cast<size_t>(col)]) == CharClass::Space) {
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
        if (ClassOf(Buf().lines[static_cast<size_t>(row)][static_cast<size_t>(col)]) == CharClass::Space) {
            col--;
            continue;
        }
        break;
    }
    if (col < 0) return {row, 0};
    CharClass cc = ClassOf(Buf().lines[static_cast<size_t>(row)][static_cast<size_t>(col)]);
    while (col > 0 && ClassOf(Buf().lines[static_cast<size_t>(row)][static_cast<size_t>(col - 1)]) == cc) col--;
    return {row, col};
}

// WORD motion: only whitespace separates words (no word/punct distinction).
CursorPos Editor::MoveWORDForward(CursorPos from) const {
    int row = from.row, col = from.col;
    int nrows = Buf().LineCount();
    if (col < LineLen(row) && !std::isspace(static_cast<unsigned char>(Buf().lines[static_cast<size_t>(row)][static_cast<size_t>(col)]))) {
        while (col < LineLen(row) && !std::isspace(static_cast<unsigned char>(Buf().lines[static_cast<size_t>(row)][static_cast<size_t>(col)]))) col++;
    }
    while (col >= LineLen(row) || std::isspace(static_cast<unsigned char>(Buf().lines[static_cast<size_t>(row)][static_cast<size_t>(col)]))) {
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
        if (std::isspace(static_cast<unsigned char>(Buf().lines[static_cast<size_t>(row)][static_cast<size_t>(col)]))) {
            col--;
            continue;
        }
        break;
    }
    if (col < 0) return {row, 0};
    while (col > 0 && !std::isspace(static_cast<unsigned char>(Buf().lines[static_cast<size_t>(row)][static_cast<size_t>(col - 1)]))) col--;
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
        if (ClassOf(Buf().lines[static_cast<size_t>(row)][static_cast<size_t>(col)]) == CharClass::Space) {
            col++;
            continue;
        }
        break;
    }
    if (big) {
        while (col + 1 < LineLen(row) && ClassOf(Buf().lines[static_cast<size_t>(row)][static_cast<size_t>(col) + 1]) != CharClass::Space) col++;
    } else {
        CharClass cc = ClassOf(Buf().lines[static_cast<size_t>(row)][static_cast<size_t>(col)]);
        while (col + 1 < LineLen(row) && ClassOf(Buf().lines[static_cast<size_t>(row)][static_cast<size_t>(col) + 1]) == cc) col++;
    }
    return {row, col};
}

// ge/gE: end of word/WORD backward.
CursorPos Editor::MoveWordEndBackward(CursorPos from, bool big) const {
    int row = from.row, col = from.col;
    // Step off whatever run `from` is currently within, if any, so this
    // doesn't just land one character back inside the SAME word.
    if (col < LineLen(row) && ClassOf(Buf().lines[static_cast<size_t>(row)][static_cast<size_t>(col)]) != CharClass::Space) {
        if (big) {
            while (col > 0 && ClassOf(Buf().lines[static_cast<size_t>(row)][static_cast<size_t>(col - 1)]) != CharClass::Space) col--;
        } else {
            CharClass cc = ClassOf(Buf().lines[static_cast<size_t>(row)][static_cast<size_t>(col)]);
            while (col > 0 && ClassOf(Buf().lines[static_cast<size_t>(row)][static_cast<size_t>(col - 1)]) == cc) col--;
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
        if (ClassOf(Buf().lines[static_cast<size_t>(row)][static_cast<size_t>(col)]) == CharClass::Space) {
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
    const std::string &line = Buf().lines[static_cast<size_t>(row)];
    int col = 0;
    while (col < static_cast<int>(line.size()) && std::isspace(static_cast<unsigned char>(line[static_cast<size_t>(col)]))) col++;
    if (col >= static_cast<int>(line.size())) col = std::max(0, static_cast<int>(line.size()) - 1);
    return {row, col};
}

// Paragraphs are delimited by blank lines -- a simplified but Vim-compatible
// reading for the common case; doesn't specially collapse runs of several
// consecutive blank lines into a single boundary the way Vim does.
CursorPos Editor::MoveParagraphForward(CursorPos from) const {
    int row = from.row + 1;
    int n = Buf().LineCount();
    while (row < n && !Buf().lines[static_cast<size_t>(row)].empty()) row++;
    if (row >= n) row = n - 1;
    return {row, 0};
}

CursorPos Editor::MoveParagraphBackward(CursorPos from) const {
    int row = from.row - 1;
    while (row > 0 && !Buf().lines[static_cast<size_t>(row)].empty()) row--;
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
            char c = Buf().lines[static_cast<size_t>(row)][static_cast<size_t>(col)];
            if (c == '.' || c == '!' || c == '?') {
                int p = col + 1;
                while (p < len) {
                    char cc = Buf().lines[static_cast<size_t>(row)][static_cast<size_t>(p)];
                    if (cc == ')' || cc == ']' || cc == '"' || cc == '\'') {
                        p++;
                        continue;
                    }
                    break;
                }
                if (p >= len || Buf().lines[static_cast<size_t>(row)][static_cast<size_t>(p)] == ' ' || Buf().lines[static_cast<size_t>(row)][static_cast<size_t>(p)] == '\t') {
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
                        char wc = Buf().lines[static_cast<size_t>(r)][static_cast<size_t>(c2)];
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
    /**
     * @brief Checks whether a position is at or before `from` in buffer order.
     * @param p The position to compare.
     * @return True if `p` is on an earlier row than `from`, or on the same row at or before its column.
     */
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
    const std::string &line0 = Buf().lines[static_cast<size_t>(from.row)];
    int col = from.col;
    while (col < static_cast<int>(line0.size()) && kOpens.find(line0[static_cast<size_t>(col)]) == std::string::npos &&
           kCloses.find(line0[static_cast<size_t>(col)]) == std::string::npos) {
        col++;
    }
    if (col >= static_cast<int>(line0.size())) return from;  // no bracket on this line: no-op

    char c = line0[static_cast<size_t>(col)];
    size_t open_idx = kOpens.find(c);
    bool forward = open_idx != std::string::npos;
    char open_c = forward ? c : kOpens[kCloses.find(c)];
    char close_c = forward ? kCloses[open_idx] : c;
    int depth = 0;
    if (forward) {
        for (int r = from.row; r < Buf().LineCount(); r++) {
            const std::string &l = Buf().lines[static_cast<size_t>(r)];
            int start_c = (r == from.row) ? col : 0;
            for (int cc = start_c; cc < static_cast<int>(l.size()); cc++) {
                if (l[static_cast<size_t>(cc)] == open_c) {
                    depth++;
                } else if (l[static_cast<size_t>(cc)] == close_c && --depth == 0) {
                    return {r, cc};
                }
            }
        }
    } else {
        for (int r = from.row; r >= 0; r--) {
            const std::string &l = Buf().lines[static_cast<size_t>(r)];
            int start_c = (r == from.row) ? col : static_cast<int>(l.size()) - 1;
            for (int cc = start_c; cc >= 0; cc--) {
                if (l[static_cast<size_t>(cc)] == close_c) {
                    depth++;
                } else if (l[static_cast<size_t>(cc)] == open_c && --depth == 0) {
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
    bool on_open = (col < LineLen(row) && Buf().lines[static_cast<size_t>(row)][static_cast<size_t>(col)] == open_c);

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
            const std::string &l = Buf().lines[static_cast<size_t>(r)];
            int start_c = (r == row) ? col - 1 : static_cast<int>(l.size()) - 1;
            for (int cc = start_c; cc >= 0; cc--) {
                if (l[static_cast<size_t>(cc)] == close_c) {
                    depth++;
                } else if (l[static_cast<size_t>(cc)] == open_c) {
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
        const std::string &l = Buf().lines[static_cast<size_t>(r)];
        int start_c = (r == op.row) ? op.col + 1 : 0;
        for (int cc = start_c; cc < static_cast<int>(l.size()); cc++) {
            if (l[static_cast<size_t>(cc)] == open_c) {
                depth++;
            } else if (l[static_cast<size_t>(cc)] == close_c) {
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
    const std::string &line = Buf().lines[static_cast<size_t>(cursor.row)];
    std::vector<int> positions;
    for (int i = 0; i < static_cast<int>(line.size()); i++) {
        if (line[static_cast<size_t>(i)] == quote) positions.push_back(i);
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

    /**
     * @brief Classifies a character in the current line for word/WORD boundary detection.
     * @param c The column of the character to classify within `row`.
     * @return For a WORD (`big`): 0 if whitespace, 1 otherwise. For a word: 0 for space, 1 for word chars, 2 for punctuation.
     */
    auto classify = [&](int c) -> int {
        if (big) return std::isspace(static_cast<unsigned char>(Buf().lines[static_cast<size_t>(row)][static_cast<size_t>(c)])) ? 0 : 1;
        CharClass cc = ClassOf(Buf().lines[static_cast<size_t>(row)][static_cast<size_t>(c)]);
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
    bool on_blank = Buf().lines[static_cast<size_t>(row)].empty();
    int top = row, bottom = row;
    while (top > 0 && Buf().lines[static_cast<size_t>(top) - 1].empty() == on_blank) top--;
    while (bottom + 1 < n && Buf().lines[static_cast<size_t>(bottom) + 1].empty() == on_blank) bottom++;
    if (around) {
        int extra_bottom = bottom;
        while (extra_bottom + 1 < n && Buf().lines[static_cast<size_t>(extra_bottom) + 1].empty() != on_blank) extra_bottom++;
        if (extra_bottom > bottom) {
            bottom = extra_bottom;
        } else {
            int extra_top = top;
            while (extra_top > 0 && Buf().lines[static_cast<size_t>(extra_top - 1)].empty() != on_blank) extra_top--;
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
    const std::string &line = Buf().lines[static_cast<size_t>(from.row)];
    int len = static_cast<int>(line.size());
    int col = from.col;
    for (int i = 0; i < count; i++) {
        if (cmd == 'f' || cmd == 't') {
            int search_from = (cmd == 't' && i > 0) ? col + 2 : col + 1;
            int pos = -1;
            for (int cc = search_from; cc < len; cc++) {
                if (line[static_cast<size_t>(cc)] == ch) {
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
                if (line[static_cast<size_t>(cc)] == ch) {
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

// Same shifting rule as ShiftMarksForLineEdit, applied independently to
// each fold's start_row and end_row so the range as a whole grows/shrinks
// correctly when the edit lands inside it rather than before it -- e.g.
// inserting a line inside an open `#+begin_src` block extends end_row
// without moving start_row, while inserting before the block shifts both.
// Keeps org src-block/headline fold ranges (and any other provider's)
// accurate between edits instead of only at the next z-fold command's
// RecomputeOrgFolds(), which previously left StepVisibleRow/ClampCursor
// checking stale ranges and made j/k seem to stick at a block boundary
// until a second press caught up.
void Editor::ShiftFoldsForLineEdit(int at_row, int count) {
    if (count == 0 || Buf().folds.empty()) return;
    int n = Buf().LineCount();
    /**
     * @brief Shifts a fold boundary row to account for lines inserted or removed at `at_row`.
     * @param row The fold boundary row to shift.
     * @return The adjusted row, clamped to the current valid line range.
     */
    auto shift_row = [&](int row) {
        if (count > 0) {
            if (row >= at_row) row += count;
        } else {
            int removed = -count;
            if (row >= at_row + removed) {
                row += count;  // count already negative
            } else if (row >= at_row) {
                row = at_row;
            }
        }
        return std::max(0, std::min(row, n - 1));
    };
    for (Fold &f : Buf().folds) {
        f.start_row = shift_row(f.start_row);
        f.end_row = shift_row(f.end_row);
    }
    // A fold collapsed to <2 lines by a deletion isn't meaningful anymore
    // (same rule CreateFold applies when one is first created).
    auto &folds = Buf().folds;
    folds.erase(std::remove_if(folds.begin(), folds.end(), [](const Fold &f) { return f.start_row >= f.end_row; }),
                folds.end());
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
    GoToJumpEntry(p.jumplist[static_cast<size_t>(p.jumplist_index)]);
}

void Editor::JumpListForward() {
    Pane &p = CurPane();
    if (p.jumplist_index + 1 >= static_cast<int>(p.jumplist.size())) return;
    p.jumplist_index++;
    GoToJumpEntry(p.jumplist[static_cast<size_t>(p.jumplist_index)]);
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
    if (op == 'q') {
        // gq: same "always whole-line, even for a charwise motion" shape
        // as >/< just above -- Vim documents gq as one of the operators
        // whose motion is forced linewise regardless of how it was
        // spelled (gqw reflows the rest of the paragraph's lines, not
        // just up to the next word boundary).
        FormatLines(start.row, end.row);
        CurPane().cursor = FirstNonBlank(std::min(start.row, Buf().LineCount() - 1));
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
    /**
     * @brief Applies the case-change mode to a single character.
     * @param c The character to transform.
     * @return `c` lowercased (mode 'u'), uppercased (mode 'U'), or with its case toggled (any other mode).
     */
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
            for (char &c : Buf().lines[static_cast<size_t>(r)]) c = transform(c);
        }
    } else if (start.row == end.row) {
        std::string &line = Buf().lines[static_cast<size_t>(start.row)];
        int a = std::min(static_cast<int>(line.size()), start.col);
        int b = std::min(static_cast<int>(line.size()), end.col);
        for (int i = a; i < b; i++) line[static_cast<size_t>(i)] = transform(line[static_cast<size_t>(i)]);
    } else {
        std::string &first = Buf().lines[static_cast<size_t>(start.row)];
        int a = std::min(static_cast<int>(first.size()), start.col);
        for (int i = a; i < static_cast<int>(first.size()); i++) first[static_cast<size_t>(i)] = transform(first[static_cast<size_t>(i)]);
        for (int r = start.row + 1; r < end.row; r++) {
            for (char &c : Buf().lines[static_cast<size_t>(r)]) c = transform(c);
        }
        int end_row = std::min(end.row, Buf().LineCount() - 1);
        std::string &last = Buf().lines[static_cast<size_t>(end_row)];
        int b = std::min(static_cast<int>(last.size()), end.col);
        for (int i = 0; i < b; i++) last[static_cast<size_t>(i)] = transform(last[static_cast<size_t>(i)]);
    }
    Buf().modified = true;
}

void Editor::IndentLines(int start_row, int end_row, int levels) {
    static const std::string kShift = "    ";  // fixed 4-space shiftwidth
    PushUndo();
    end_row = std::min(end_row, Buf().LineCount() - 1);
    for (int r = std::max(0, start_row); r <= end_row; r++) {
        std::string &line = Buf().lines[static_cast<size_t>(r)];
        if (levels > 0) {
            for (int i = 0; i < levels; i++) line.insert(0, kShift);
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

// gq: reflows [start_row, end_row] to wrap at text_width_ columns
// (":set textwidth="/"tw=", default 80), one blank-line-delimited
// paragraph at a time -- same simplified "no comment leader" paragraph
// boundary (Buf().lines[row].empty()) that MoveParagraphForward/
// ParagraphObjectRange already use. A paragraph that extends beyond
// end_row is only rewrapped where it overlaps [start_row, end_row],
// matching Vim's gq{motion} (not the whole paragraph the motion happens
// to graze). Every wrapped line reuses the *first* line's leading
// whitespace as its indent.
void Editor::FormatLines(int start_row, int end_row) {
    const int kFormatWidth = std::max(1, text_width_);
    end_row = std::min(end_row, Buf().LineCount() - 1);
    if (start_row > end_row) return;
    PushUndo();

    int row = std::max(0, start_row);
    while (row <= end_row) {
        if (Buf().lines[static_cast<size_t>(row)].empty()) {
            row++;
            continue;
        }
        int para_start = row;
        int para_end = row;
        while (para_end + 1 <= end_row && !Buf().lines[static_cast<size_t>(para_end) + 1].empty()) para_end++;

        const std::string &first_line = Buf().lines[static_cast<size_t>(para_start)];
        size_t indent_len = 0;
        while (indent_len < first_line.size() &&
               (first_line[indent_len] == ' ' || first_line[indent_len] == '\t')) {
            indent_len++;
        }
        std::string indent = first_line.substr(0, indent_len);

        std::vector<std::string> words;
        for (int r = para_start; r <= para_end; r++) {
            std::istringstream iss(Buf().lines[static_cast<size_t>(r)]);
            std::string w;
            while (iss >> w) words.push_back(w);
        }

        std::vector<std::string> wrapped;
        if (words.empty()) {
            wrapped.push_back(indent);
        } else {
            std::string cur = indent;
            bool cur_has_word = false;
            for (const std::string &w : words) {
                if (cur_has_word && cur.size() + 1 + w.size() > static_cast<size_t>(kFormatWidth)) {
                    wrapped.push_back(cur);
                    cur = indent + w;
                } else {
                    if (cur_has_word) cur += ' ';
                    cur += w;
                }
                cur_has_word = true;
            }
            wrapped.push_back(cur);
        }

        int old_count = para_end - para_start + 1;
        int new_count = static_cast<int>(wrapped.size());
        Buf().lines.erase(Buf().lines.begin() + para_start, Buf().lines.begin() + para_end + 1);
        ShiftMarksForLineEdit(para_start, -old_count);
        ShiftFoldsForLineEdit(para_start, -old_count);
        Buf().lines.insert(Buf().lines.begin() + para_start, wrapped.begin(), wrapped.end());
        ShiftMarksForLineEdit(para_start, new_count);
        ShiftFoldsForLineEdit(para_start, new_count);

        end_row += new_count - old_count;
        row = para_start + new_count;
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
        std::string next = Buf().lines[static_cast<size_t>(row) + 1];
        Buf().lines.erase(Buf().lines.begin() + row + 1);
        ShiftMarksForLineEdit(row + 1, -1);
        ShiftFoldsForLineEdit(row + 1, -1);
        size_t s = 0;
        while (s < next.size() && std::isspace(static_cast<unsigned char>(next[s]))) s++;
        next = next.substr(s);
        std::string &line = Buf().lines[static_cast<size_t>(row)];
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
        ShiftFoldsForLineEdit(first, -(last - first + 1));
        if (Buf().lines.empty()) Buf().lines.emplace_back("");
        Buf().modified = true;
        return;
    }
    if (start.row == end.row) {
        std::string &line = Buf().lines[static_cast<size_t>(start.row)];
        int a = std::min(static_cast<int>(line.size()), start.col);
        int b = std::min(static_cast<int>(line.size()), end.col);
        if (b > a) line.erase(static_cast<size_t>(a), static_cast<size_t>(b - a));
    } else {
        // Multi-line charwise (e.g. `d}`, or a bracket/quote text object
        // spanning lines): join what's left of the start line (its own
        // prefix) with what's left of the end line (its own suffix),
        // dropping everything strictly between -- same shape as Vim's own
        // "join the two halves" behavior for an exclusive multi-line range.
        const std::string &first_line = Buf().lines[static_cast<size_t>(start.row)];
        int a = std::min(static_cast<int>(first_line.size()), start.col);
        std::string prefix = first_line.substr(0, static_cast<size_t>(a));
        int end_row = std::min(end.row, Buf().LineCount() - 1);
        const std::string &last_line = Buf().lines[static_cast<size_t>(end_row)];
        int b = std::min(static_cast<int>(last_line.size()), end.col);
        std::string suffix = last_line.substr(static_cast<size_t>(b));
        Buf().lines[static_cast<size_t>(start.row)] = prefix + suffix;
        Buf().lines.erase(Buf().lines.begin() + start.row + 1, Buf().lines.begin() + end_row + 1);
        ShiftMarksForLineEdit(start.row + 1, -(end_row - start.row));
        ShiftFoldsForLineEdit(start.row + 1, -(end_row - start.row));
    }
    Buf().modified = true;
}

std::string Editor::ExtractRangeText(CursorPos start, CursorPos end, bool linewise) const {
    std::string text;
    if (linewise) {
        int last = std::min(end.row, Buf().LineCount() - 1);
        for (int r = start.row; r <= last; r++) {
            text += Buf().lines[static_cast<size_t>(r)];
            text += "\n";
        }
    } else if (start.row == end.row) {
        const std::string &line = Buf().lines[static_cast<size_t>(start.row)];
        int a = std::min(static_cast<int>(line.size()), start.col);
        int b = std::min(static_cast<int>(line.size()), end.col);
        text = (b > a) ? line.substr(static_cast<size_t>(a), static_cast<size_t>(b - a)) : "";
    } else {
        const std::string &first_line = Buf().lines[static_cast<size_t>(start.row)];
        int a = std::min(static_cast<int>(first_line.size()), start.col);
        text += first_line.substr(static_cast<size_t>(a));
        for (int r = start.row + 1; r < end.row; r++) {
            text += "\n";
            text += Buf().lines[static_cast<size_t>(r)];
        }
        int end_row = std::min(end.row, Buf().LineCount() - 1);
        const std::string &last_line = Buf().lines[static_cast<size_t>(end_row)];
        int b = std::min(static_cast<int>(last_line.size()), end.col);
        text += "\n";
        text += last_line.substr(0, static_cast<size_t>(b));
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
    // "+/"* *are* the unnamed register (RegisterFor) -- normalized here
    // too so the `reg_name != 0` mirroring below isn't a self-assign.
    if (reg_name == '+' || reg_name == '*') reg_name = 0;

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
    // ...and the system clipboard mirrors the unnamed register.
    SyncUnnamedToSystemClipboard();
}

namespace {
// Used for linewise yank text, which YankRange always terminates with a
// trailing '\n' (one after every line, including the last) -- so every
// segment split out here is a real line, and any text after a final '\n'
// (there never is any) would correctly be dropped.
/**
 * @brief Splits linewise-yanked text (always '\n'-terminated after every line) into its lines.
 * @param text The linewise yank text to split.
 * @return The lines of `text`, one entry per line, with the trailing '\n' after each removed.
 */
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
/**
 * @brief Splits charwise-yanked text on '\n', keeping any trailing segment after the last newline.
 * @param text The charwise yank text to split.
 * @return The segments of `text` between newlines, including a final segment with no trailing '\n'.
 */
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
        std::string &line = Buf().lines[static_cast<size_t>(pos.row)];
        int at = std::min(static_cast<int>(line.size()), pos.col);
        line.insert(static_cast<size_t>(at), text);
        return {pos.row, at + static_cast<int>(text.size())};
    }
    std::vector<std::string> parts = SplitKeepingLast(text);
    const std::string &line = Buf().lines[static_cast<size_t>(pos.row)];
    int at = std::min(static_cast<int>(line.size()), pos.col);
    std::string suffix = line.substr(static_cast<size_t>(at));
    std::string new_first = line.substr(0, static_cast<size_t>(at)) + parts.front();
    std::string new_last = parts.back() + suffix;

    Buf().lines[static_cast<size_t>(pos.row)] = new_first;
    std::vector<std::string> to_insert(parts.begin() + 1, parts.end() - 1);
    to_insert.push_back(new_last);
    Buf().lines.insert(Buf().lines.begin() + pos.row + 1, to_insert.begin(), to_insert.end());
    ShiftMarksForLineEdit(pos.row + 1, static_cast<int>(to_insert.size()));
    ShiftFoldsForLineEdit(pos.row + 1, static_cast<int>(to_insert.size()));

    int end_row = pos.row + static_cast<int>(parts.size()) - 1;
    int end_col = static_cast<int>(parts.back().size());
    return {end_row, end_col};
}

void Editor::PasteAfter(int count, char reg_name) {
    // Pasting from the unnamed register (plain p, "+p, "*p) picks up
    // whatever another app copied since mep last touched the clipboard.
    if (reg_name == 0 || reg_name == '"' || reg_name == '+' || reg_name == '*') PullSystemClipboard();
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
        ShiftFoldsForLineEdit(insert_at, static_cast<int>(new_lines.size()));
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
    if (reg_name == 0 || reg_name == '"' || reg_name == '+' || reg_name == '*') PullSystemClipboard();
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
        ShiftFoldsForLineEdit(insert_at, static_cast<int>(new_lines.size()));
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
        std::string &line = Buf().lines[static_cast<size_t>(r)];
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
        bool found = CiFind(Buf().lines[static_cast<size_t>(r)], pattern, 0) != std::string::npos;
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

    // Checks whether every character of the trimmed command is a digit (a bare ":N" line-jump).
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
    } else if (name == "e" || name == "edit" || name == "e!" || name == "edit!") {
        // Bare `:e`/`:e!` (no path argument) reloads the *current*
        // buffer's own file from disk instead of erroring with "no file
        // name" (LoadFile's own behavior for an empty path, correct for
        // every other caller but not this one) -- ReloadCurrentBuffer's
        // own comment has the full reasoning (force/E37, cursor
        // preserved not reset). `:e path`/`:e! path` are unaffected:
        // force_text=true, same escape-hatch reasoning as before -- the
        // bang there only ever meant "no path given" routing, LoadFile
        // itself has no unsaved-changes guard to override when opening a
        // *different* file (mep's buffers persist independently, so
        // switching away from a modified one never loses it).
        bool bang = !name.empty() && name.back() == '!';
        if (args.empty()) {
            ReloadCurrentBuffer(bang);
        } else {
            LoadFile(args, true);
        }
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
    } else if (name == "wsnew" || name == "wsnew!") {
        if (args.empty()) {
            status_message_ = "Usage: :wsnew[!] <name>";
        } else {
            WorkspaceCreate(args, name.back() == '!');
        }
    } else if (name == "wsdelete" || name == "wsdelete!" || name == "wsclose" || name == "wsclose!") {
        WorkspaceRemove(args, name.back() == '!');
    } else if (name == "wsnext" || name == "wsn") {
        WorkspaceNext();
    } else if (name == "wsprevious" || name == "wsp" || name == "wsN") {
        WorkspacePrevious();
    } else if (name == "wsrename") {
        if (args.empty()) {
            status_message_ = "Usage: :wsrename <name>";
        } else if (WorkspaceRename(ActiveWorkspace().id, args)) {
            status_message_ = "workspace renamed to " + args;
        }
    } else if (name == "ws" || name == "workspace") {
        if (args.empty()) {
            const Workspace &ws = ActiveWorkspace();
            status_message_ = "workspace " + ws.name + " (" + ws.root + ")" +
                              (ws.branch.empty() ? "" : " on branch " + ws.branch);
        } else {
            int id = ResolveWorkspaceArg(args);
            if (id < 0) {
                status_message_ = "No such workspace: " + args;
            } else {
                WorkspaceSwitch(id);
            }
        }
    } else if (name == "wslist" || name == "workspaces") {
        if (lua_) lua_->DoString("mep.workspaces()");
    } else if (name == "wsadopt") {
        WorkspaceAdopt(args);
    } else if (name == "wsprune") {
        WorkspacePrune();
    } else if (name == "wssave") {
        status_message_ = SaveWorkspaceState(ActiveProject().id) ? "Saved workspace state to " + WorkspaceStateFile(ActiveProject())
                                                                 : "Could not save workspace state";
    } else if (name == "wsrestore") {
        if (!RestoreWorkspaceState(ActiveProject().id, /*keep_primary_tabs=*/false)) {
            status_message_ = "No saved workspace state for " + ActiveProject().root;
        }
    } else if (name == "project") {
        if (args.empty()) {
            const Project &p = ActiveProject();
            status_message_ = "project " + p.name + " (" + p.root + ")" + (p.is_git ? " [git]" : "") + ", " +
                              std::to_string(p.workspaces.size()) + " workspace(s)";
        } else if (lua_) {
            // Through Lua so the readme/terminal default layout for a
            // never-seen project is applied too (mep.project_open).
            lua_->DoString("mep.project_open(" + LuaQuote(args) + ")");
        } else {
            ProjectLoad(args);
        }
    } else if (name == "projectclose" || name == "projectclose!") {
        int id = ResolveProjectArg(args);
        if (id < 0) {
            status_message_ = "No such project: " + args;
        } else {
            ProjectClose(id, name.back() == '!');
        }
    } else if (name == "projectclear" || name == "projectclear!") {
        // Through Lua (mep.project_clear) so the legacy readme/tree/
        // terminal layout is rebuilt on the emptied workspace too.
        const bool force = name.back() == '!';
        if (lua_) {
            lua_->DoString(std::string("mep.project_clear(") + (force ? "true" : "false") + ")");
        } else {
            WorkspaceReset(ActiveWorkspace().id, force);
        }
    } else if (name == "projectnext" || name == "projectn") {
        ProjectNext();
    } else if (name == "projectprevious" || name == "projectprev" || name == "projectp") {
        ProjectPrevious();
    } else if (name == "projects") {
        if (lua_) lua_->DoString("mep.projects_open()");
    } else if (name == "bnext" || name == "bn") {
        BufferNext();
    } else if (name == "bprevious" || name == "bprev" || name == "bp" || name == "bNext" || name == "bN") {
        BufferPrevious();
    } else if (name == "bd" || name == "bdelete" || name == "bd!" || name == "bdelete!") {
        BufferDelete(!name.empty() && name.back() == '!');
    } else if (name == "set") {
        size_t pos = 0;
        while (pos < args.size()) {
            size_t end = args.find(' ', pos);
            if (end == std::string::npos) end = args.size();
            std::string opt = args.substr(pos, end - pos);
            pos = end + 1;
            if (opt.empty()) continue;
            // Numeric "key=value" options -- checked before the boolean
            // "no{key}" negation below since '=' never appears in one of
            // those. Currently just textwidth/tw; any other numeric-style
            // option added later belongs here too rather than growing a
            // second table.
            size_t eq = opt.find('=');
            if (eq != std::string::npos) {
                std::string key = opt.substr(0, eq);
                std::string val = opt.substr(eq + 1);
                if (key == "textwidth" || key == "tw") {
                    char *val_end = nullptr;
                    long w = std::strtol(val.c_str(), &val_end, 10);
                    if (val_end != val.c_str() && *val_end == '\0' && w > 0) {
                        text_width_ = static_cast<int>(w);
                    } else {
                        status_message_ = "E521: Number required after =: " + opt;
                    }
                } else {
                    status_message_ = "E518: Unknown option: " + key;
                }
                continue;
            }
            bool negate = opt.rfind("no", 0) == 0 && opt.size() > 2;
            std::string key = negate ? opt.substr(2) : opt;
            bool value = !negate;
            if (key == "number" || key == "nu") {
                show_line_numbers_ = value;
            } else if (key == "relativenumber" || key == "rnu") {
                show_relative_numbers_ = value;
            } else if (key == "cursorline" || key == "cul") {
                show_cursorline_ = value;
            } else if (key == "wrap") {
                wrap_ = value;
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
    } else if (name == "Kanban") {
        OpenKanbanView();
    } else if (name == "Gantt") {
        OpenGanttView();
    } else if (name == "Org" || name == "Text") {
        CloseOrgView();
    } else if (name == "CollabJoin") {
        const size_t split = args.find(' ');
        const std::string url = split == std::string::npos ? args : args.substr(0, split);
        const std::string participant = split == std::string::npos ? "" : args.substr(split + 1);
        JoinCollaboration(url, participant);
    } else if (name == "CollabLeave") {
        LeaveCollaboration();
    } else if (name == "CollabStatus") {
        if (!CollaborationActive()) status_message_ = "Collaboration: disconnected";
        else status_message_ = "Collaboration: " + std::to_string(CollaborationPeers().size()) + " collaborator(s)";
    } else if (name == "AgentSocket") {
        const std::string path = mep::agent::SocketPath();
        status_message_ = path.empty() ? "Agent socket: unavailable" : ("Agent socket: " + path);
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
    for (char raw : keys) {
        unsigned char c = static_cast<unsigned char>(raw);
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
    return Buf().lines[static_cast<size_t>(row)];
}

void Editor::SetLineForLua(int row, const std::string &text) {
    if (row < 0 || row >= Buf().LineCount()) return;
    PushUndo();
    Buf().lines[static_cast<size_t>(row)] = text;
    Buf().modified = true;
}

int Editor::LineCountForLua() const { return Buf().LineCount(); }

std::vector<Editor::TodoMatch> Editor::TodoScanMatches() const {
    static const char *const kKeywords[] = {"TODO", "FIXME", "HACK", "NOTE"};
    std::vector<TodoMatch> matches;
    const int n = Buf().LineCount();
    for (int row = 0; row < n; row++) {
        const std::string &line = Buf().lines[static_cast<size_t>(row)];
        for (const char *kw : kKeywords) {
            size_t pos = line.find(kw);
            if (pos != std::string::npos) {
                matches.push_back({row, static_cast<int>(pos), static_cast<int>(pos + std::strlen(kw)), kw});
            }
        }
    }
    return matches;
}

void Editor::DapToggleBreakpoint() {
    int ns = CreateNamespace("dap");
    int row = 0, col = 0;
    GetCursorForLua(&row, &col);
    int line = row + 1;  // 1-indexed, matching mep.cursor()/DAP's own convention
    std::vector<int> &bps = dap_breakpoints_[Buf().filename];
    auto it = std::find(bps.begin(), bps.end(), line);
    if (it != bps.end()) {
        bps.erase(it);
        // Only the remove path needs a full rebuild -- decorations have no
        // per-item removal, only ClearNamespace (a whole-namespace wipe),
        // so the surviving breakpoints' signs must be re-added. The add
        // path below doesn't touch existing decorations at all, matching
        // the original Lua's own asymmetric behavior exactly.
        ClearNamespace(ns);
        for (int r : bps) {
            Decoration d;
            d.row = r - 1;
            d.sign = "";
            d.sign_hl = "Red";
            AddDecoration(ns, d);
        }
        return;
    }
    bps.push_back(line);
    Decoration d;
    d.row = line - 1;
    d.sign = "";
    d.sign_hl = "Red";
    AddDecoration(ns, d);
}

std::vector<int> Editor::DapBreakpointLines(const std::string &filename) const {
    auto it = dap_breakpoints_.find(filename);
    if (it == dap_breakpoints_.end()) return {};
    return it->second;
}

bool Editor::TermsendRegister(int source, int target) {
    if (!IsTerminalBuffer(target)) {
        Notify("mep.termsend: buffer " + std::to_string(target) + " is not a terminal buffer", NotifyLevel::Error);
        return false;
    }
    termsend_targets_[source] = target;
    return true;
}

int Editor::TermsendTarget(int source) const {
    auto it = termsend_targets_.find(source);
    if (it == termsend_targets_.end()) return 0;
    return IsTerminalBuffer(it->second) ? it->second : 0;
}

void Editor::TermsendUnregister(int source) { termsend_targets_.erase(source); }

std::vector<int> Editor::TermsendCandidates() const {
    std::vector<int> out;
    for (int id : PaneBuffersInActiveTab()) {
        if (IsTerminalBuffer(id)) out.push_back(id);
    }
    return out;
}

namespace {

bool IsOrgTodoPath(const std::string &path) {
    return path.size() >= 4 && path.compare(path.size() - 4, 4, ".org") == 0;
}

// Absolute, lexically normalized form of `path` for FindOpenBufferForPath's
// equality test -- no realpath(3): a symlinked TODO.org opened under two
// spellings is an accepted miss, not worth a filesystem round trip per
// buffer per panel refresh.
std::string NormalizedAbsolutePath(const std::string &path) {
    std::error_code ec;
    std::filesystem::path p(path);
    if (p.is_relative()) {
        std::filesystem::path cwd = std::filesystem::current_path(ec);
        if (ec) return path;
        p = cwd / p;
    }
    return p.lexically_normal().string();
}

// Reads a whole text file as lines, SplitIntoLines-style (a missing or
// unreadable file is an empty vector, which for an org todo file means
// "no items yet", not an error).
bool ReadFileLines(const std::string &path, std::vector<std::string> &lines) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    lines = SplitIntoLines(content);
    return true;
}

}  // namespace

bool Editor::ReadLinesForPath(const std::string &path, int max_lines, std::vector<std::string> *out) const {
    if (!out) return false;
    out->clear();
    std::vector<std::string> lines;
    int buffer_id = FindOpenBufferForPath(path);
    if (buffer_id >= 0) {
        lines = buffers_[static_cast<size_t>(buffer_id)].lines;
    } else if (!ReadFileLines(path, lines)) {
        return false;
    }
    if (max_lines > 0 && static_cast<int>(lines.size()) > max_lines) {
        lines.resize(static_cast<size_t>(max_lines));
        lines.emplace_back("...");
    }
    *out = std::move(lines);
    return true;
}

int Editor::FindOpenBufferForPath(const std::string &path) const {
    if (path.empty()) return -1;
    const std::string want = NormalizedAbsolutePath(path);
    for (size_t i = 0; i < buffers_.size(); i++) {
        const Buffer &buf = buffers_[static_cast<size_t>(i)];
        if (buf.deleted || buf.filename.empty() || GetTerminal(static_cast<int>(i))) continue;
        if (NormalizedAbsolutePath(ResolveBufferPath(buf, buf.filename)) == want) return static_cast<int>(i);
    }
    return -1;
}

std::vector<Editor::ActivityTodoItem> Editor::ActivityTodoLoad(const std::string &path) const {
    std::vector<ActivityTodoItem> items;
    if (IsOrgTodoPath(path)) {
        int buffer_id = FindOpenBufferForPath(path);
        if (buffer_id >= 0) return OrgTodoListItems(buffers_[static_cast<size_t>(buffer_id)].lines);
        std::vector<std::string> lines;
        if (!ReadFileLines(path, lines)) return items;
        return OrgTodoListItems(lines);
    }
    std::ifstream f(path);
    if (!f) return items;
    std::string line;
    while (std::getline(f, line)) {
        if (line.size() >= 2 && std::isdigit(static_cast<unsigned char>(line[0])) && line[1] == '|') {
            ActivityTodoItem it;
            it.done = line[0] == '1';
            it.text = line.substr(2);
            items.push_back(std::move(it));
        }
    }
    return items;
}

bool Editor::WriteLinesForPath(const std::string &path, const std::vector<std::string> &updated) {
    int buffer_id = FindOpenBufferForPath(path);
    if (buffer_id >= 0) {
        Buffer &buf = buffers_[static_cast<size_t>(buffer_id)];
        std::vector<std::string> next = updated;
        if (next.empty()) next.emplace_back("");  // a buffer's "empty file" is one empty line
        if (next == buf.lines) return true;
        const bool had_pending_edits = buf.modified;
        ReplaceLinesAt(buffer_id, 0, static_cast<int>(buf.lines.size()), next);
        if (CurPane().buffer_id == buffer_id) ClampCursor();
        if (!had_pending_edits) SaveBuffer(buf, buf.filename);
        return true;
    }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    for (const auto &l : updated) f << l << '\n';
    return true;
}

void Editor::ActivityTodoSave(const std::string &path, const std::vector<ActivityTodoItem> &items) {
    if (IsOrgTodoPath(path)) {
        std::vector<std::string> lines;
        const bool existed = ReadLinesForPath(path, 0, &lines);
        std::vector<std::string> updated = OrgTodoListApply(lines, items);
        if (existed && updated == lines) return;
        WriteLinesForPath(path, updated);
        return;
    }
    std::ofstream f(path, std::ios::trunc);
    if (!f) return;
    for (const auto &it : items) {
        f << (it.done ? '1' : '0') << '|' << it.text << '\n';
    }
}

Editor::ActivityTodoClock Editor::ActivityTodoClockStatus(const std::string &path) const {
    ActivityTodoClock out;
    if (!IsOrgTodoPath(path)) return out;
    std::vector<std::string> lines;
    if (!ReadLinesForPath(path, 0, &lines)) return out;
    OrgOpenClock clock = OrgFindOpenClock(lines);
    if (clock.line < 0 || clock.headline_line < 0) return out;
    out.line = clock.headline_line;
    out.start_ts = clock.start_ts;
    int y = 0, mo = 0, d = 0, hh = 0, mm = 0;
    if (OrgParseClockTimestamp(clock.start_ts, &y, &mo, &d, &hh, &mm)) {
        out.start_epoch = static_cast<long long>(MakeLocalTime(y, mo, d, hh, mm));
    }
    // The same parse OrgTodoListItems uses for the sidebar rows (the
    // file's own "#+TODO:" keywords), so the chip's title matches the row.
    out.title = lines[static_cast<size_t>(clock.headline_line)];
    for (const OrgHeadline &h : ParseOrgOutline(lines).headlines) {
        if (h.line_start == clock.headline_line) {
            out.title = h.title;
            break;
        }
    }
    return out;
}

bool Editor::ActivityTodoClockStart(const std::string &path, int line) {
    if (!IsOrgTodoPath(path)) return false;
    std::vector<std::string> lines;
    if (!ReadLinesForPath(path, 0, &lines)) return false;
    std::vector<std::string> updated = OrgClockStartLines(lines, line, FormatOrgTimestampNow());
    if (updated == lines) return false;
    return WriteLinesForPath(path, updated);
}

int Editor::ActivityTodoClockStop(const std::string &path) {
    if (!IsOrgTodoPath(path)) return -1;
    std::vector<std::string> lines;
    if (!ReadLinesForPath(path, 0, &lines)) return -1;
    int minutes = -1;
    std::vector<std::string> updated = OrgClockStopLines(lines, FormatOrgTimestampNow(), &minutes);
    if (minutes < 0) return -1;
    if (!WriteLinesForPath(path, updated)) return -1;
    return minutes;
}

bool Editor::ActivityTodoRetitle(const std::string &path, int line, const std::string &text) {
    if (!IsOrgTodoPath(path)) return false;
    std::vector<std::string> lines;
    if (!ReadLinesForPath(path, 0, &lines)) return false;
    std::vector<std::string> updated = OrgTodoListRetitle(lines, line, text);
    if (updated == lines) return false;
    return WriteLinesForPath(path, updated);
}

bool Editor::ActivityTodoArchive(const std::string &path, int line) {
    if (!IsOrgTodoPath(path)) return false;
    std::vector<std::string> lines;
    if (!ReadLinesForPath(path, 0, &lines)) return false;
    std::vector<std::string> updated = OrgTodoListArchive(lines, line);
    if (updated == lines) return false;
    return WriteLinesForPath(path, updated);
}

std::vector<Editor::ActivityTestFailureLine> Editor::ActivityTestFailureLines(
    const std::vector<std::string> &output) const {
    std::vector<ActivityTestFailureLine> out;
    for (size_t i = 0; i < output.size(); i++) {
        std::string lower = output[i];
        // Lowercases one character (used to build a case-insensitive copy of the line).
        std::transform(lower.begin(), lower.end(), lower.begin(),
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower.find("fail") != std::string::npos) {
            out.push_back({static_cast<int>(i + 1), output[i]});
        }
    }
    return out;
}

void Editor::SyntaxHighlightFallback(int ns, const std::vector<std::string> &keywords,
                                      const std::string &comment_prefix) {
    std::unordered_set<std::string> kwset(keywords.begin(), keywords.end());
    const int n = Buf().LineCount();
    for (int row = 0; row < n; row++) {
        const std::string &line = Buf().lines[static_cast<size_t>(row)];
        const int len = static_cast<int>(line.size());
        int i = 0;
        while (i < len) {
            char c = line[static_cast<size_t>(i)];
            if (!comment_prefix.empty() && line.compare(static_cast<size_t>(i), comment_prefix.size(), comment_prefix) == 0) {
                Decoration d;
                d.row = row;
                d.col_start = i;
                d.col_end = len;
                d.hl_group = "Comment";
                AddDecoration(ns, d);
                break;
            } else if (c == '"' || c == '\'') {
                char q = c;
                int j = i + 1;
                while (j < len && line[static_cast<size_t>(j)] != q) {
                    if (line[static_cast<size_t>(j)] == '\\') j++;
                    j++;
                }
                Decoration d;
                d.row = row;
                d.col_start = i;
                d.col_end = std::min(j + 1, len);
                d.hl_group = "Green";
                AddDecoration(ns, d);
                i = j + 1;
            } else if (std::isdigit(static_cast<unsigned char>(c))) {
                int j = i;
                while (j < len && (std::isdigit(static_cast<unsigned char>(line[static_cast<size_t>(j)])) || line[static_cast<size_t>(j)] == '.')) j++;
                Decoration d;
                d.row = row;
                d.col_start = i;
                d.col_end = j;
                d.hl_group = "Cyan";
                AddDecoration(ns, d);
                i = j;
            } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                int j = i;
                while (j < len && (std::isalnum(static_cast<unsigned char>(line[static_cast<size_t>(j)])) || line[static_cast<size_t>(j)] == '_')) j++;
                if (kwset.count(line.substr(static_cast<size_t>(i), static_cast<size_t>(j - i)))) {
                    Decoration d;
                    d.row = row;
                    d.col_start = i;
                    d.col_end = j;
                    d.hl_group = "Purple";
                    AddDecoration(ns, d);
                }
                i = j;
            } else {
                i++;
            }
        }
    }
}

namespace {
// ^\s*#\+KEYWORD\a+ (case-insensitive on KEYWORD), matching kBuiltinSyntax's
// own '^%s*#%+[Bb][Ee][Gg][Ii][Nn]_%a+'/'^%s*#%+[Ee][Nn][Dd]_%a+' patterns --
// `keyword` must already be lowercase ("begin_"/"end_").
/**
 * @brief Checks whether a line matches an org-mode "#+KEYWORD..." block marker (case-insensitive on keyword).
 * @param line The line of text to test.
 * @param keyword The lowercase marker keyword to match (e.g. "begin_" or "end_").
 * @return True if the line, after leading whitespace, starts with "#+" followed by `keyword` and at least one more letter.
 */
bool MatchesOrgBlockMarker(const std::string &line, const char *keyword) {
    size_t i = 0;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) i++;
    if (i + 1 >= line.size() || line[i] != '#' || line[i + 1] != '+') return false;
    i += 2;
    size_t klen = std::strlen(keyword);
    if (i + klen > line.size()) return false;
    for (size_t k = 0; k < klen; k++) {
        if (std::tolower(static_cast<unsigned char>(line[i + k])) != keyword[k]) return false;
    }
    i += klen;
    return i < line.size() && std::isalpha(static_cast<unsigned char>(line[i]));
}
}  // namespace

void Editor::OrgHighlightEmphasis(int ns) {
    static const std::unordered_map<char, char> kMarkerKind = {
        {'*', 'b'}, {'/', 'i'}, {'_', 'u'}, {'+', 's'}, {'=', 'v'}, {'~', 'c'},
    };
    /**
     * @brief Fetches the character at `idx` in `s`, or the NUL character if `idx` is out of bounds.
     * @param s The string to index into.
     * @param idx The index to read.
     * @return The character at `idx`, or '\0' if `idx` is out of range.
     */
    auto at = [](const std::string &s, int idx) -> char {
        return (idx >= 0 && idx < static_cast<int>(s.size())) ? s[static_cast<size_t>(idx)] : '\0';
    };
    /**
     * @brief Checks whether a character is a non-NUL alphanumeric "word" character.
     * @param c The character to test.
     * @return True if `c` is alphanumeric.
     */
    auto is_word = [](char c) { return c != '\0' && std::isalnum(static_cast<unsigned char>(c)); };

    bool in_block = false;
    const int n = Buf().LineCount();
    for (int row = 0; row < n; row++) {
        const std::string &line = Buf().lines[static_cast<size_t>(row)];
        if (in_block) {
            if (MatchesOrgBlockMarker(line, "end_")) in_block = false;
            continue;
        }
        if (MatchesOrgBlockMarker(line, "begin_")) {
            in_block = true;
            continue;
        }
        int i = 0;
        const int len = static_cast<int>(line.size());
        while (i < len) {
            char ch = line[static_cast<size_t>(i)];
            auto it = kMarkerKind.find(ch);
            if (it == kMarkerKind.end()) {
                i++;
                continue;
            }
            char pre = at(line, i - 1);
            char nxt = at(line, i + 1);
            bool boundary_ok = (i == 0 || !is_word(pre)) && nxt != '\0' && nxt != ' ' && nxt != ch;
            if (!boundary_ok) {
                i++;
                continue;
            }
            size_t search_from = static_cast<size_t>(i) + 1;
            int found_end = -1;
            while (true) {
                size_t s = line.find(ch, search_from);
                if (s == std::string::npos) break;
                char prev_char = at(line, static_cast<int>(s) - 1);
                char after_char = at(line, static_cast<int>(s) + 1);
                if (prev_char != ' ' && prev_char != ch && !is_word(after_char)) {
                    found_end = static_cast<int>(s);
                    break;
                }
                search_from = s + 1;
            }
            if (found_end < 0) {
                i++;
                continue;
            }
            Decoration d;
            d.row = row;
            d.col_start = i;
            d.col_end = found_end + 1;
            switch (it->second) {
                case 'b': d.bold = true; break;
                case 'i': d.italic = true; break;
                case 'u': d.underline = true; break;
                case 's': d.strikethrough = true; d.hl_group = "Comment"; break;
                case 'v': d.hl_group = "Green"; break;
                case 'c': d.hl_group = "Cyan"; break;
                default: break;
            }
            AddDecoration(ns, d);
            i = found_end + 1;
        }
    }
}

void Editor::MdToggleCheckbox() {
    int row = 0, col = 0;
    GetCursorForLua(&row, &col);
    const std::string &line = Buf().lines[static_cast<size_t>(row)];
    size_t i = 0;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) i++;
    bool ok = i < line.size() && (line[i] == '-' || line[i] == '*' || line[i] == '+');
    if (ok) {
        i++;
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) i++;
        ok = i < line.size() && line[i] == '[';
    }
    char mark = 0;
    if (ok) {
        i++;
        ok = i < line.size();
        if (ok) {
            mark = line[i];
            ok = (mark == ' ' || mark == 'x' || mark == 'X');
        }
    }
    if (!ok) {
        Notify("No checkbox on this line", NotifyLevel::Warn);
        return;
    }
    char newmark = (mark == ' ') ? 'x' : ' ';
    SetLineForLua(row, line.substr(0, i) + newmark + line.substr(i + 1));
}

void Editor::MdComputeFolds() {
    ClearFoldsFromProvider("markdown");
    struct Entry {
        int level;
        int row;
    };
    std::vector<Entry> stack;
    bool in_fence = false;
    int fence_start = 0;
    const int n = Buf().LineCount();
    for (int i = 0; i < n; i++) {
        const std::string &line = Buf().lines[static_cast<size_t>(i)];
        if (line.compare(0, 3, "```") == 0) {
            if (in_fence) {
                CreateFold(fence_start, i, true, "markdown");
                in_fence = false;
            } else {
                in_fence = true;
                fence_start = i;
            }
        } else if (!in_fence) {
            size_t h = 0;
            while (h < line.size() && line[h] == '#') h++;
            if (h > 0 && h < line.size() && std::isspace(static_cast<unsigned char>(line[h]))) {
                int level = static_cast<int>(h);
                while (!stack.empty() && stack.back().level >= level) {
                    Entry top = stack.back();
                    stack.pop_back();
                    if (top.row < i - 1) CreateFold(top.row, i - 1, true, "markdown");
                }
                stack.push_back({level, i});
            }
        }
    }
    while (!stack.empty()) {
        Entry top = stack.back();
        stack.pop_back();
        if (top.row < n - 1) CreateFold(top.row, n - 1, true, "markdown");
    }
}

namespace {
// A GFM pipe-table row, parsed by ParseMdTableRow (mep_md_table_row's own
// C++ port): either not a table row at all, a separator row (---/:--/--:/
// :-:) carrying per-column alignment, or a data row carrying trimmed
// cell text.
struct MdTableRowResult {
    bool is_table_row = false;
    bool is_sep = false;
    std::vector<std::string> cells;   // data row only
    std::vector<std::string> aligns;  // sep row only: "left"/"right"/"center"/"none"
};

// ^:?-+:?$ -- a single GFM separator cell (optional leading/trailing ':'
// around one-or-more '-').
/**
 * @brief Checks whether a table cell's text is a valid GFM separator cell (dashes with optional colon alignment markers).
 * @param c The trimmed cell text to test.
 * @return True if `c` matches the separator-cell pattern.
 */
bool IsMdSepCell(const std::string &c) {
    size_t i = 0, n = c.size();
    if (i < n && c[i] == ':') i++;
    size_t dash_start = i;
    while (i < n && c[i] == '-') i++;
    if (i == dash_start) return false;
    if (i < n && c[i] == ':') i++;
    return i == n;
}

/**
 * @brief Parses a single line as a GFM pipe-table row, classifying it as a separator row or a data row.
 * @param line The raw line of text to parse.
 * @return An MdTableRowResult describing whether the line is a table row, and if so its alignments (separator row) or trimmed cell text (data row).
 */
MdTableRowResult ParseMdTableRow(const std::string &line) {
    MdTableRowResult r;
    size_t i = 0;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) i++;
    if (i >= line.size() || line[i] != '|') return r;
    r.is_table_row = true;
    std::string rest = line.substr(i + 1);
    size_t end = rest.size();
    while (end > 0 && std::isspace(static_cast<unsigned char>(rest[end - 1]))) end--;
    std::string inner = (end > 0 && rest[end - 1] == '|') ? rest.substr(0, end - 1) : rest.substr(0, end);
    std::vector<std::string> cells;
    size_t start = 0;
    for (size_t p = 0; p <= inner.size(); p++) {
        if (p == inner.size() || inner[p] == '|') {
            size_t a = start, b = p;
            while (a < b && std::isspace(static_cast<unsigned char>(inner[a]))) a++;
            while (b > a && std::isspace(static_cast<unsigned char>(inner[b - 1]))) b--;
            cells.push_back(inner.substr(a, b - a));
            start = p + 1;
        }
    }
    bool is_sep = !cells.empty();
    for (const auto &c : cells) {
        if (!IsMdSepCell(c)) {
            is_sep = false;
            break;
        }
    }
    if (is_sep) {
        r.is_sep = true;
        for (const auto &c : cells) {
            bool l = !c.empty() && c.front() == ':';
            bool rr = !c.empty() && c.back() == ':';
            r.aligns.emplace_back((l && rr) ? "center" : (rr ? "right" : (l ? "left" : "none")));
        }
    } else {
        r.cells = cells;
    }
    return r;
}

/**
 * @brief Builds a GFM table separator cell of a given display width for a given column alignment.
 * @param w The target width of the separator cell.
 * @param al The column alignment: "left", "right", "center", or anything else for none.
 * @return A string of dashes, with leading/trailing ':' markers according to `al`.
 */
std::string MdSepCell(int w, const std::string &al) {
    if (al == "left") return ":" + std::string(static_cast<size_t>(std::max(1, w - 1)), '-');
    if (al == "right") return std::string(static_cast<size_t>(std::max(1, w - 1)), '-') + ":";
    if (al == "center") return ":" + std::string(static_cast<size_t>(std::max(1, w - 2)), '-') + ":";
    return std::string(static_cast<size_t>(w), '-');
}
}  // namespace

void Editor::MdTableAlign() {
    int row = 0, col = 0;
    GetCursorForLua(&row, &col);
    if (!ParseMdTableRow(Buf().lines[static_cast<size_t>(row)]).is_table_row) {
        Notify("Not on a table row", NotifyLevel::Warn);
        return;
    }
    int top = row;
    while (top > 0 && ParseMdTableRow(Buf().lines[static_cast<size_t>(top) - 1]).is_table_row) top--;
    int bot = row;
    const int n = Buf().LineCount();
    while (bot < n - 1 && ParseMdTableRow(Buf().lines[static_cast<size_t>(bot) + 1]).is_table_row) bot++;

    std::vector<int> widths;
    std::vector<std::string> aligns;
    struct RowEntry {
        int row;
        MdTableRowResult r;
    };
    std::vector<RowEntry> rows;
    for (int i = top; i <= bot; i++) {
        MdTableRowResult r = ParseMdTableRow(Buf().lines[static_cast<size_t>(i)]);
        if (r.is_sep) {
            for (size_t ci = 0; ci < r.aligns.size(); ci++) {
                if (aligns.size() <= ci) aligns.resize(ci + 1);
                aligns[ci] = r.aligns[ci];
            }
        } else {
            for (size_t ci = 0; ci < r.cells.size(); ci++) {
                if (widths.size() <= ci) widths.resize(ci + 1, 3);
                widths[ci] = std::max(widths[ci], static_cast<int>(r.cells[ci].size()));
            }
        }
        rows.push_back({i, std::move(r)});
    }
    for (const auto &entry : rows) {
        std::string out = "|";
        for (size_t ci = 0; ci < widths.size(); ci++) {
            std::string al = (ci < aligns.size() && !aligns[ci].empty()) ? aligns[ci] : "none";
            if (entry.r.is_sep) {
                out += " " + MdSepCell(widths[ci], al) + " |";
            } else {
                std::string cell = ci < entry.r.cells.size() ? entry.r.cells[ci] : "";
                int pad = std::max(0, widths[ci] - static_cast<int>(cell.size()));
                std::string padded;
                if (al == "right") {
                    padded = std::string(static_cast<size_t>(pad), ' ') + cell;
                } else if (al == "center") {
                    int lp = pad / 2;
                    padded = std::string(static_cast<size_t>(lp), ' ') + cell + std::string(static_cast<size_t>(pad - lp), ' ');
                } else {
                    padded = cell + std::string(static_cast<size_t>(pad), ' ');
                }
                out += " " + padded + " |";
            }
        }
        SetLineForLua(entry.row, out);
    }
}

void Editor::MdTableInsertRow() {
    int row = 0, col = 0;
    GetCursorForLua(&row, &col);
    MdTableRowResult r = ParseMdTableRow(Buf().lines[static_cast<size_t>(row)]);
    if (!r.is_table_row || r.is_sep) {
        Notify("Not on a table data row", NotifyLevel::Warn);
        return;
    }
    std::string blank = "|";
    for (size_t i = 0; i < r.cells.size(); i++) blank += " |";
    std::vector<std::string> newlines = {Buf().lines[static_cast<size_t>(row)], blank};
    ReplaceLinesForLua(row, row + 1, newlines);
    SetCursorForLua(row + 1, 1);
    MdTableAlign();
}

void Editor::MdTableInsertCol() {
    int row = 0, col = 0;
    GetCursorForLua(&row, &col);
    if (!ParseMdTableRow(Buf().lines[static_cast<size_t>(row)]).is_table_row) {
        Notify("Not on a table row", NotifyLevel::Warn);
        return;
    }
    int top = row;
    while (top > 0 && ParseMdTableRow(Buf().lines[static_cast<size_t>(top) - 1]).is_table_row) top--;
    int bot = row;
    const int n = Buf().LineCount();
    while (bot < n - 1 && ParseMdTableRow(Buf().lines[static_cast<size_t>(bot) + 1]).is_table_row) bot++;
    std::vector<std::string> newlines;
    for (int i = top; i <= bot; i++) {
        std::string suffix = ParseMdTableRow(Buf().lines[static_cast<size_t>(i)]).is_sep ? "---|" : " |";
        const std::string &line = Buf().lines[static_cast<size_t>(i)];
        size_t end = line.size();
        while (end > 0 && std::isspace(static_cast<unsigned char>(line[end - 1]))) end--;
        newlines.push_back(line.substr(0, end) + suffix);
    }
    ReplaceLinesForLua(top, bot + 1, newlines);
    MdTableAlign();
}

namespace {
struct MdConcealSpan {
    int col_start = 0;
    int col_end = 0;
    std::string text;
    std::string hl;
};

// Plain-literal-find-based scanner (no Lua patterns involved in the
// original either) for **bold**/__bold__, *italic*/_italic_ (with the
// same mid-identifier-'_'/'*' exclusion OrgHighlightEmphasis's is_word
// check uses), and [text](url)/[text][ref] links -- mep_md_conceal_spans'
// own C++ port.
/**
 * @brief Scans a line for markdown emphasis (bold via ** or __, italic via * or _) and link spans to conceal.
 * @param line The line of text to scan.
 * @return The list of spans found, each with its column range, the text to reveal in place of the markup, and a highlight group.
 */
std::vector<MdConcealSpan> ScanMdConcealSpans(const std::string &line) {
    std::vector<MdConcealSpan> spans;
    const int n = static_cast<int>(line.size());
    /**
     * @brief Fetches the character at `idx` in `line`, or the NUL character if `idx` is out of bounds.
     * @param idx The index to read.
     * @return The character at `idx`, or '\0' if `idx` is out of range.
     */
    auto at = [&](int idx) -> char { return (idx >= 0 && idx < n) ? line[static_cast<size_t>(idx)] : '\0'; };
    /**
     * @brief Checks whether a character is a non-NUL alphanumeric "word" character.
     * @param c The character to test.
     * @return True if `c` is alphanumeric.
     */
    auto is_word = [](char c) { return c != '\0' && std::isalnum(static_cast<unsigned char>(c)); };
    int i = 0;
    while (i < n) {
        char c0 = line[static_cast<size_t>(i)];
        char c1 = at(i + 1);
        if ((c0 == '*' && c1 == '*') || (c0 == '_' && c1 == '_')) {
            std::string two{c0, c1};
            size_t close = line.find(two, static_cast<size_t>(i) + 2);
            if (close != std::string::npos) {
                spans.push_back({i, static_cast<int>(close) + 2,
                                  line.substr(static_cast<size_t>(i) + 2, close - static_cast<size_t>(i) - 2), "Yellow"});
                i = static_cast<int>(close) + 2;
            } else {
                i++;
            }
        } else if (c0 == '*' || c0 == '_') {
            char before = at(i - 1);
            long close = -1;
            if (!is_word(before)) {
                size_t s = line.find(c0, static_cast<size_t>(i) + 1);
                if (s != std::string::npos) close = static_cast<long>(s);
            }
            if (close >= 0 && close > i + 1 && !is_word(at(static_cast<int>(close) + 1))) {
                spans.push_back({i, static_cast<int>(close) + 1,
                                  line.substr(static_cast<size_t>(i) + 1, static_cast<size_t>(close) - static_cast<size_t>(i) - 1), "Cyan"});
                i = static_cast<int>(close) + 1;
            } else {
                i++;
            }
        } else if (c0 == '[') {
            size_t closeb = line.find(']', static_cast<size_t>(i) + 1);
            char after = closeb != std::string::npos ? at(static_cast<int>(closeb) + 1) : '\0';
            if (closeb != std::string::npos && after == '(') {
                size_t closep = line.find(')', closeb + 2);
                if (closep != std::string::npos) {
                    spans.push_back({i, static_cast<int>(closep) + 1,
                                      line.substr(static_cast<size_t>(i) + 1, closeb - static_cast<size_t>(i) - 1), "Blue"});
                    i = static_cast<int>(closep) + 1;
                } else {
                    i++;
                }
            } else if (closeb != std::string::npos && after == '[') {
                size_t closer2 = line.find(']', closeb + 2);
                if (closer2 != std::string::npos) {
                    spans.push_back({i, static_cast<int>(closer2) + 1,
                                      line.substr(static_cast<size_t>(i) + 1, closeb - static_cast<size_t>(i) - 1), "Blue"});
                    i = static_cast<int>(closer2) + 1;
                } else {
                    i++;
                }
            } else {
                i++;
            }
        } else {
            i++;
        }
    }
    return spans;
}
}  // namespace

void Editor::MdConceal(int ns) {
    int cur_row = 0, col = 0;
    GetCursorForLua(&cur_row, &col);
    bool in_fence = false;
    const int n = Buf().LineCount();
    for (int row = 0; row < n; row++) {
        const std::string &line = Buf().lines[static_cast<size_t>(row)];
        if (line.compare(0, 3, "```") == 0) {
            in_fence = !in_fence;
            continue;
        }
        if (in_fence || row == cur_row) continue;
        for (const MdConcealSpan &sp : ScanMdConcealSpans(line)) {
            Decoration d;
            d.row = row;
            d.col_start = sp.col_start;
            d.col_end = sp.col_end;
            d.virt_text = sp.text;
            d.virt_text_hl = sp.hl;
            d.virt_overlay = true;
            d.priority = 10;
            AddDecoration(ns, d);
        }
    }
}

std::vector<std::string> Editor::CompletionScanBufferWords(const std::string &prefix) const {
    std::unordered_set<std::string> seen;
    std::vector<std::string> words;
    const int n = Buf().LineCount();
    for (int i = 0; i < n; i++) {
        const std::string &line = Buf().lines[static_cast<size_t>(i)];
        size_t j = 0;
        const size_t len = line.size();
        while (j < len) {
            if (std::isalnum(static_cast<unsigned char>(line[j])) || line[j] == '_') {
                size_t start = j;
                while (j < len && (std::isalnum(static_cast<unsigned char>(line[j])) || line[j] == '_')) j++;
                std::string w = line.substr(start, j - start);
                if (w.size() > prefix.size() && w.compare(0, prefix.size(), prefix) == 0 && !seen.count(w)) {
                    seen.insert(w);
                    words.push_back(w);
                }
            } else {
                j++;
            }
        }
    }
    return words;
}

namespace {
// ctx contains "word" immediately followed by exactly one whitespace
// character somewhere -- mep_completion_path_prefix's own
// ctx:find('import%s')/ctx:find('from%s') (a bare %s, not %s*, so this is
// a plain substring-then-one-whitespace-char search, not a general regex).
/**
 * @brief Checks whether `s` contains `word` immediately followed by a single whitespace character somewhere.
 * @param s The string to search.
 * @param word The substring to look for.
 * @return True if some occurrence of `word` in `s` is directly followed by a whitespace character.
 */
bool FindWordFollowedBySpace(const std::string &s, const std::string &word) {
    size_t pos = 0;
    while ((pos = s.find(word, pos)) != std::string::npos) {
        size_t after = pos + word.size();
        if (after < s.size() && std::isspace(static_cast<unsigned char>(s[after]))) return true;
        pos++;
    }
    return false;
}

// ctx:find('require%s*%(%s*$') -- does ctx, ignoring trailing whitespace,
// end with "require", optional whitespace, then "("?
/**
 * @brief Checks whether `ctx`, ignoring trailing whitespace, ends with "require", optional whitespace, then "(".
 * @param ctx The context string to test.
 * @return True if `ctx` ends with an opening `require(` call (allowing whitespace before the paren).
 */
bool EndsWithRequireCallOpen(const std::string &ctx) {
    size_t end = ctx.size();
    while (end > 0 && std::isspace(static_cast<unsigned char>(ctx[end - 1]))) end--;
    if (end == 0 || ctx[end - 1] != '(') return false;
    end--;
    while (end > 0 && std::isspace(static_cast<unsigned char>(ctx[end - 1]))) end--;
    static const std::string kRequire = "require";
    if (end < kRequire.size()) return false;
    return ctx.compare(end - kRequire.size(), kRequire.size(), kRequire) == 0;
}
}  // namespace

Editor::CompletionPathPrefix Editor::CompletionPathPrefixFor(const std::string &prefix, int col,
                                                              const std::string &line) const {
    CompletionPathPrefix result;
    int start = col - 1 - static_cast<int>(prefix.size());
    if (start < 0) start = 0;
    std::string before = line.substr(0, std::min<size_t>(static_cast<size_t>(start), line.size()));
    char trigger = before.empty() ? '\0' : before.back();
    bool is_path_dot = false;
    if (trigger == '.') {
        char prev = before.size() >= 2 ? before[before.size() - 2] : '\0';
        is_path_dot = !(std::isalnum(static_cast<unsigned char>(prev)) || prev == '_');
    }
    if (trigger == '/' || is_path_dot) {
        size_t p = before.size();
        while (p > 0) {
            char c = before[p - 1];
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-' || c == '/') {
                p--;
            } else {
                break;
            }
        }
        std::string token = before.substr(p);
        size_t slash = token.find_last_of('/');
        result.found = true;
        result.dir = (slash == std::string::npos) ? "." : token.substr(0, slash + 1);
        result.base = prefix;
        return result;
    }
    if (trigger == '"' || trigger == '\'') {
        std::string without_quote = before.substr(0, before.size() - 1);
        std::string ctx =
            without_quote.size() > 40 ? without_quote.substr(without_quote.size() - 40) : without_quote;
        if (EndsWithRequireCallOpen(ctx) || FindWordFollowedBySpace(ctx, "import") ||
            FindWordFollowedBySpace(ctx, "from")) {
            result.found = true;
            result.dir = ".";
            result.base = prefix;
        }
    }
    return result;
}

namespace {
// A snippet template line's tabstop, positioned in the *cleaned* (marker
// syntax stripped) output text -- mep_snippet_scan_line's own port.
struct SnippetLineTabstop {
    int col = 0;  // 1-indexed, within the cleaned line
    int num = 0;
};

/**
 * @brief Strips snippet tabstop markers ($1, ${1:default}, \$) from a template line and records their positions.
 * @param tmpl The raw snippet template line, containing tabstop markers.
 * @return A pair of the cleaned line text (markers replaced by their default text, if any) and the list of tabstops found, each with its column in the cleaned text and its tabstop number.
 */
std::pair<std::string, std::vector<SnippetLineTabstop>> ScanSnippetLine(const std::string &tmpl) {
    std::string out;
    std::vector<SnippetLineTabstop> tabstops;
    size_t i = 0;
    const size_t n = tmpl.size();
    while (i < n) {
        char c = tmpl[i];
        if (c == '\\' && i + 1 < n && tmpl[i + 1] == '$') {
            out += '$';
            i += 2;
        } else if (c == '$' && i + 1 < n) {
            size_t numstart = i + 1;
            size_t j = numstart;
            while (j < n && std::isdigit(static_cast<unsigned char>(tmpl[j]))) j++;
            if (j > numstart) {
                int num = std::stoi(tmpl.substr(numstart, j - numstart));
                tabstops.push_back({static_cast<int>(out.size()) + 1, num});
                i = j;
            } else if (tmpl[i + 1] == '{') {
                size_t close = tmpl.find('}', i + 2);
                bool matched = false;
                if (close != std::string::npos) {
                    std::string inner = tmpl.substr(i + 2, close - (i + 2));
                    size_t k = 0;
                    while (k < inner.size() && std::isdigit(static_cast<unsigned char>(inner[k]))) k++;
                    if (k > 0 && (k == inner.size() || inner[k] == ':')) {
                        int idx = std::stoi(inner.substr(0, k));
                        tabstops.push_back({static_cast<int>(out.size()) + 1, idx});
                        if (k < inner.size()) out += inner.substr(k + 1);
                        i = close + 1;
                        matched = true;
                    }
                }
                if (!matched) {
                    out += c;
                    i++;
                }
            } else {
                out += c;
                i++;
            }
        } else {
            out += c;
            i++;
        }
    }
    return {out, tabstops};
}
}  // namespace

void Editor::SnippetSplice(int row, const std::string &before, const std::string &after,
                            const std::vector<std::string> &body) {
    std::vector<std::string> out_lines;
    std::vector<SnippetTabstop> all_tabstops;
    for (size_t li = 0; li < body.size(); li++) {
        auto [cleaned, stops] = ScanSnippetLine(body[li]);
        int col_offset = (li == 0) ? static_cast<int>(before.size()) : 0;
        for (const auto &s : stops) {
            all_tabstops.push_back({static_cast<int>(li) + 1, s.col + col_offset, s.num});
        }
        std::string line = (li == 0 ? before : "") + cleaned + (li == body.size() - 1 ? after : "");
        out_lines.push_back(std::move(line));
    }
    if (out_lines.size() == 1) {
        SetLineForLua(row, out_lines[0]);
    } else {
        ReplaceLinesForLua(row, row + 1, out_lines);
    }
    // Orders tabstops by number, treating an unnumbered tabstop (0, the final-cursor "$0") as sorting last.
    std::stable_sort(all_tabstops.begin(), all_tabstops.end(), [](const SnippetTabstop &a, const SnippetTabstop &b) {
        int an = (a.num == 0) ? 999 : a.num;
        int bn = (b.num == 0) ? 999 : b.num;
        return an < bn;
    });
    snippet_tabstops_ = std::move(all_tabstops);
    snippet_index_ = 0;
    snippet_base_row_ = row;
    has_snippet_state_ = true;
    SnippetJump(1);
}

void Editor::SnippetJump(int delta) {
    if (!has_snippet_state_) return;
    snippet_index_ += delta;
    if (snippet_index_ < 1 || snippet_index_ > static_cast<int>(snippet_tabstops_.size())) {
        has_snippet_state_ = false;
        return;
    }
    const SnippetTabstop &ts = snippet_tabstops_[static_cast<size_t>(snippet_index_ - 1)];
    SetCursorForLua(snippet_base_row_ + ts.line_idx - 1, ts.col - 1);
}

void Editor::PreviewFile(const std::string &path, int max_lines) {
    std::ifstream f(path);
    if (!f) {
        SetPickerPreview("(cannot open " + path + ")");
        return;
    }
    if (max_lines <= 0) max_lines = 40;
    std::vector<std::string> lines;
    std::string line;
    bool truncated = false;
    while (std::getline(f, line)) {
        if (static_cast<int>(lines.size()) >= max_lines) {
            truncated = true;
            break;
        }
        lines.push_back(std::move(line));
    }
    if (truncated) lines.emplace_back("...");
    std::string text;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i > 0) text += '\n';
        text += lines[i];
    }
    SetPickerPreview(text);
}

namespace {
/**
 * @brief Joins a directory path and an entry name with a single '/' separator.
 * @param dir The directory path, with or without a trailing slash.
 * @param name The entry name to append.
 * @return `dir` and `name` joined by exactly one '/'.
 */
std::string JoinTreePath(const std::string &dir, const std::string &name) {
    if (!dir.empty() && dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

/**
 * @brief Recursively appends file-tree rows for `dir` and, for each expanded subdirectory, its children.
 * @param ed The editor used to list each directory's contents.
 * @param root The tree's root directory, used to compute entries' paths relative to it.
 * @param dir The directory currently being listed.
 * @param depth The nesting depth of `dir` below `root`, used as each row's indent level.
 * @param expanded_paths The set of full directory paths that should be recursed into.
 * @param show_hidden Whether dotfiles/dot-directories should be included.
 * @param ignored_relpaths Paths (relative to `root`) to exclude entirely.
 * @param out The row list to append to.
 */
void BuildFileTreeRowsRecursive(const Editor *ed, const std::string &root, const std::string &dir, int depth,
                                 const std::unordered_set<std::string> &expanded_paths, bool show_hidden,
                                 const std::unordered_set<std::string> &ignored_relpaths,
                                 std::vector<Editor::FileTreeRow> *out) {
    std::vector<Editor::DirEntry> entries = ed->ListDirectory(dir);
    // Sorts directory entries directories-first, then alphabetically by name.
    std::sort(entries.begin(), entries.end(), [](const Editor::DirEntry &a, const Editor::DirEntry &b) {
        if (a.is_dir != b.is_dir) return a.is_dir;
        return a.name < b.name;
    });
    for (const Editor::DirEntry &e : entries) {
        bool hidden = !e.name.empty() && e.name[0] == '.';
        std::string rel = dir.size() > root.size() ? dir.substr(root.size() + 1) : "";
        std::string relpath = rel.empty() ? e.name : (rel + "/" + e.name);
        if ((show_hidden || !hidden) && !ignored_relpaths.count(relpath)) {
            std::string full = JoinTreePath(dir, e.name);
            bool expanded = e.is_dir && expanded_paths.count(full) > 0;
            out->push_back({full, e.name, e.is_dir, depth, expanded});
            if (expanded) BuildFileTreeRowsRecursive(ed, root, full, depth + 1, expanded_paths, show_hidden,
                                                      ignored_relpaths, out);
        }
    }
}
}  // namespace

std::vector<Editor::FileTreeRow> Editor::BuildFileTreeRows(const std::string &root,
                                                            const std::vector<std::string> &expanded_paths,
                                                            bool show_hidden,
                                                            const std::vector<std::string> &ignored_relpaths) const {
    std::unordered_set<std::string> expanded_set(expanded_paths.begin(), expanded_paths.end());
    std::unordered_set<std::string> ignored_set(ignored_relpaths.begin(), ignored_relpaths.end());
    std::vector<FileTreeRow> rows;
    BuildFileTreeRowsRecursive(this, root, root, 0, expanded_set, show_hidden, ignored_set, &rows);
    return rows;
}

std::string Editor::ProjectReadmePath(const std::string &dir) const {
    static const char *const kNames[] = {"README.md", "README.org", "README.txt", "README"};
    std::vector<DirEntry> entries = ListDirectory(dir);
    for (const char *name : kNames) {
        for (const DirEntry &e : entries) {
            if (!e.is_dir && e.name == name) return dir + "/" + name;
        }
    }
    return "";
}

namespace {
// CSS3/SVG named-color keywords (148 entries, incl. both gray/grey
// spellings and rebeccapurple) -- mep.colorize's own MEP_CSS_COLORS,
// mechanically extracted from the original Lua table (not user-facing
// config -- a Lua local, not a mep.* global).
const std::unordered_map<std::string, std::string> kCssColors = {
    {"aliceblue", "f0f8ff"}, {"antiquewhite", "faebd7"}, {"aqua", "00ffff"}, {"aquamarine", "7fffd4"},
    {"azure", "f0ffff"}, {"beige", "f5f5dc"}, {"bisque", "ffe4c4"}, {"black", "000000"},
    {"blanchedalmond", "ffebcd"}, {"blue", "0000ff"}, {"blueviolet", "8a2be2"}, {"brown", "a52a2a"},
    {"burlywood", "deb887"}, {"cadetblue", "5f9ea0"}, {"chartreuse", "7fff00"}, {"chocolate", "d2691e"},
    {"coral", "ff7f50"}, {"cornflowerblue", "6495ed"}, {"cornsilk", "fff8dc"}, {"crimson", "dc143c"},
    {"cyan", "00ffff"}, {"darkblue", "00008b"}, {"darkcyan", "008b8b"}, {"darkgoldenrod", "b8860b"},
    {"darkgray", "a9a9a9"}, {"darkgreen", "006400"}, {"darkgrey", "a9a9a9"}, {"darkkhaki", "bdb76b"},
    {"darkmagenta", "8b008b"}, {"darkolivegreen", "556b2f"}, {"darkorange", "ff8c00"}, {"darkorchid", "9932cc"},
    {"darkred", "8b0000"}, {"darksalmon", "e9967a"}, {"darkseagreen", "8fbc8f"}, {"darkslateblue", "483d8b"},
    {"darkslategray", "2f4f4f"}, {"darkslategrey", "2f4f4f"}, {"darkturquoise", "00ced1"}, {"darkviolet", "9400d3"},
    {"deeppink", "ff1493"}, {"deepskyblue", "00bfff"}, {"dimgray", "696969"}, {"dimgrey", "696969"},
    {"dodgerblue", "1e90ff"}, {"firebrick", "b22222"}, {"floralwhite", "fffaf0"}, {"forestgreen", "228b22"},
    {"fuchsia", "ff00ff"}, {"gainsboro", "dcdcdc"}, {"ghostwhite", "f8f8ff"}, {"gold", "ffd700"},
    {"goldenrod", "daa520"}, {"gray", "808080"}, {"green", "008000"}, {"greenyellow", "adff2f"},
    {"grey", "808080"}, {"honeydew", "f0fff0"}, {"hotpink", "ff69b4"}, {"indianred", "cd5c5c"},
    {"indigo", "4b0082"}, {"ivory", "fffff0"}, {"khaki", "f0e68c"}, {"lavender", "e6e6fa"},
    {"lavenderblush", "fff0f5"}, {"lawngreen", "7cfc00"}, {"lemonchiffon", "fffacd"}, {"lightblue", "add8e6"},
    {"lightcoral", "f08080"}, {"lightcyan", "e0ffff"}, {"lightgoldenrodyellow", "fafad2"}, {"lightgray", "d3d3d3"},
    {"lightgreen", "90ee90"}, {"lightgrey", "d3d3d3"}, {"lightpink", "ffb6c1"}, {"lightsalmon", "ffa07a"},
    {"lightseagreen", "20b2aa"}, {"lightskyblue", "87cefa"}, {"lightslategray", "778899"},
    {"lightslategrey", "778899"},
    {"lightsteelblue", "b0c4de"}, {"lightyellow", "ffffe0"}, {"lime", "00ff00"}, {"limegreen", "32cd32"},
    {"linen", "faf0e6"}, {"magenta", "ff00ff"}, {"maroon", "800000"}, {"mediumaquamarine", "66cdaa"},
    {"mediumblue", "0000cd"}, {"mediumorchid", "ba55d3"}, {"mediumpurple", "9370db"}, {"mediumseagreen", "3cb371"},
    {"mediumslateblue", "7b68ee"}, {"mediumspringgreen", "00fa9a"}, {"mediumturquoise", "48d1cc"},
    {"mediumvioletred", "c71585"},
    {"midnightblue", "191970"}, {"mintcream", "f5fffa"}, {"mistyrose", "ffe4e1"}, {"moccasin", "ffe4b5"},
    {"navajowhite", "ffdead"}, {"navy", "000080"}, {"oldlace", "fdf5e6"}, {"olive", "808000"},
    {"olivedrab", "6b8e23"}, {"orange", "ffa500"}, {"orangered", "ff4500"}, {"orchid", "da70d6"},
    {"palegoldenrod", "eee8aa"}, {"palegreen", "98fb98"}, {"paleturquoise", "afeeee"}, {"palevioletred", "db7093"},
    {"papayawhip", "ffefd5"}, {"peachpuff", "ffdab9"}, {"peru", "cd853f"}, {"pink", "ffc0cb"},
    {"plum", "dda0dd"}, {"powderblue", "b0e0e6"}, {"purple", "800080"}, {"rebeccapurple", "663399"},
    {"red", "ff0000"}, {"rosybrown", "bc8f8f"}, {"royalblue", "4169e1"}, {"saddlebrown", "8b4513"},
    {"salmon", "fa8072"}, {"sandybrown", "f4a460"}, {"seagreen", "2e8b57"}, {"seashell", "fff5ee"},
    {"sienna", "a0522d"}, {"silver", "c0c0c0"}, {"skyblue", "87ceeb"}, {"slateblue", "6a5acd"},
    {"slategray", "708090"}, {"slategrey", "708090"}, {"snow", "fffafa"}, {"springgreen", "00ff7f"},
    {"steelblue", "4682b4"}, {"tan", "d2b48c"}, {"teal", "008080"}, {"thistle", "d8bfd8"},
    {"tomato", "ff6347"}, {"turquoise", "40e0d0"}, {"violet", "ee82ee"}, {"wheat", "f5deb3"},
    {"white", "ffffff"}, {"whitesmoke", "f5f5f5"}, {"yellow", "ffff00"}, {"yellowgreen", "9acd32"},
};

/**
 * @brief Converts a single hex digit character to its numeric value.
 * @param c The hex digit character ('0'-'9', 'a'-'f', or 'A'-'F'); behavior is undefined for any other character.
 * @return The digit's value from 0 to 15.
 */
int HexDigitVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return 10 + (c - 'a');
}

/**
 * @brief Converts a pair of hex digit characters to the byte value they encode.
 * @param hi The high-nibble hex digit.
 * @param lo The low-nibble hex digit.
 * @return The combined byte value from 0 to 255.
 */
int HexPairVal(char hi, char lo) { return HexDigitVal(hi) * 16 + HexDigitVal(lo); }

/**
 * @brief Checks whether `count` characters of `line` starting at `start` are all hex digits.
 * @param line The line of text to check.
 * @param start The starting index of the range to check.
 * @param count The number of characters to check.
 * @return True if every character in the range is a hex digit.
 */
bool AllHexDigits(const std::string &line, int start, int count) {
    for (int k = 0; k < count; k++) {
        if (!std::isxdigit(static_cast<unsigned char>(line[static_cast<size_t>(start) + static_cast<size_t>(k)]))) return false;
    }
    return true;
}

/**
 * @brief Adds a color-swatch decoration spanning a column range of a line.
 * @param ed The editor to add the decoration through.
 * @param ns The decoration namespace to add it under.
 * @param row The zero-based row the swatch belongs to.
 * @param col_start The starting column of the swatch span.
 * @param col_end The ending column (exclusive) of the swatch span.
 * @param r The swatch color's red component (0-255).
 * @param g The swatch color's green component (0-255).
 * @param b The swatch color's blue component (0-255).
 */
void AddSwatch(Editor *ed, int ns, int row, int col_start, int col_end, int r, int g, int b) {
    Decoration d;
    d.row = row;
    d.col_start = col_start;
    d.col_end = col_end;
    d.has_swatch = true;
    d.swatch_color = {static_cast<unsigned char>(r), static_cast<unsigned char>(g), static_cast<unsigned char>(b)};
    ed->AddDecoration(ns, d);
}
}  // namespace

void Editor::Colorize() {
    int ns = CreateNamespace("colorizer");
    ClearNamespace(ns);
    const int n = Buf().LineCount();
    for (int row = 0; row < n; row++) {
        const std::string &line = Buf().lines[static_cast<size_t>(row)];
        const int len = static_cast<int>(line.size());
        std::vector<bool> covered(static_cast<size_t>(len), false);

        // #RRGGBBAA (8 hex) -- unconditional (first to claim), alpha
        // consumed but unused for the swatch color.
        for (int i = 0; i < len;) {
            if (line[static_cast<size_t>(i)] == '#' && i + 9 <= len && AllHexDigits(line, i + 1, 8)) {
                AddSwatch(this, ns, row, i, i + 9, HexPairVal(line[static_cast<size_t>(i) + 1], line[static_cast<size_t>(i) + 2]),
                          HexPairVal(line[static_cast<size_t>(i) + 3], line[static_cast<size_t>(i) + 4]), HexPairVal(line[static_cast<size_t>(i) + 5], line[static_cast<size_t>(i) + 6]));
                for (int k = i; k < i + 9; k++) covered[static_cast<size_t>(k)] = true;
                i += 9;
            } else {
                i++;
            }
        }
        // #RRGGBB (6 hex) -- only if not already claimed by the 8-hex
        // pass, but the scan itself still advances past every match
        // found either way (mirrors gmatch's own non-overlapping
        // progression, independent of the covered check).
        for (int i = 0; i < len;) {
            if (line[static_cast<size_t>(i)] == '#' && i + 7 <= len && AllHexDigits(line, i + 1, 6)) {
                if (!covered[static_cast<size_t>(i)]) {
                    AddSwatch(this, ns, row, i, i + 7, HexPairVal(line[static_cast<size_t>(i) + 1], line[static_cast<size_t>(i) + 2]),
                              HexPairVal(line[static_cast<size_t>(i) + 3], line[static_cast<size_t>(i) + 4]), HexPairVal(line[static_cast<size_t>(i) + 5], line[static_cast<size_t>(i) + 6]));
                    for (int k = i; k < i + 7; k++) covered[static_cast<size_t>(k)] = true;
                }
                i += 7;
            } else {
                i++;
            }
        }
        // #RGB (3 hex, each digit doubled) -- same claim-check as above,
        // but (matching the original exactly) doesn't itself mark
        // covered afterward.
        for (int i = 0; i < len;) {
            if (line[static_cast<size_t>(i)] == '#' && i + 4 <= len && AllHexDigits(line, i + 1, 3)) {
                if (!covered[static_cast<size_t>(i)]) {
                    int r = HexPairVal(line[static_cast<size_t>(i) + 1], line[static_cast<size_t>(i) + 1]);
                    int g = HexPairVal(line[static_cast<size_t>(i) + 2], line[static_cast<size_t>(i) + 2]);
                    int b = HexPairVal(line[static_cast<size_t>(i) + 3], line[static_cast<size_t>(i) + 3]);
                    AddSwatch(this, ns, row, i, i + 4, r, g, b);
                }
                i += 4;
            } else {
                i++;
            }
        }
        // rgb(r,g,b)/rgba(r,g,b,...) -- independent of the claim-tracking
        // entirely (no covered check, doesn't mark it either); the swatch
        // deliberately only spans the "rgb"/"rgba" keyword itself (3
        // chars), matching the original's own col_end = s+3, not the
        // whole call. `i` advances to the end of the *whole* matched
        // text (through the 3rd number), mirroring gmatch's real
        // progression, not the narrower decorated span.
        for (int i = 0; i < len;) {
            int p = i;
            bool matched = false;
            if (line.compare(static_cast<size_t>(p), 3, "rgb") == 0) {
                p += 3;
                if (p < len && line[static_cast<size_t>(p)] == 'a') p++;
                if (p < len && line[static_cast<size_t>(p)] == '(') {
                    p++;
                    while (p < len && std::isspace(static_cast<unsigned char>(line[static_cast<size_t>(p)]))) p++;
                    int r_start = p;
                    while (p < len && std::isdigit(static_cast<unsigned char>(line[static_cast<size_t>(p)]))) p++;
                    if (p > r_start) {
                        int r = std::stoi(line.substr(static_cast<size_t>(r_start), static_cast<size_t>(p - r_start)));
                        while (p < len && std::isspace(static_cast<unsigned char>(line[static_cast<size_t>(p)]))) p++;
                        if (p < len && line[static_cast<size_t>(p)] == ',') {
                            p++;
                            while (p < len && std::isspace(static_cast<unsigned char>(line[static_cast<size_t>(p)]))) p++;
                            int g_start = p;
                            while (p < len && std::isdigit(static_cast<unsigned char>(line[static_cast<size_t>(p)]))) p++;
                            if (p > g_start) {
                                int g = std::stoi(line.substr(static_cast<size_t>(g_start), static_cast<size_t>(p - g_start)));
                                while (p < len && std::isspace(static_cast<unsigned char>(line[static_cast<size_t>(p)]))) p++;
                                if (p < len && line[static_cast<size_t>(p)] == ',') {
                                    p++;
                                    while (p < len && std::isspace(static_cast<unsigned char>(line[static_cast<size_t>(p)]))) p++;
                                    int b_start = p;
                                    while (p < len && std::isdigit(static_cast<unsigned char>(line[static_cast<size_t>(p)]))) p++;
                                    if (p > b_start) {
                                        int b = std::stoi(line.substr(static_cast<size_t>(b_start), static_cast<size_t>(p - b_start)));
                                        AddSwatch(this, ns, row, i, i + 3, r, g, b);
                                        matched = true;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            if (matched) {
                i = p;
            } else {
                i++;
            }
        }
        // CSS/SVG named colors -- every maximal run of letters (word-
        // boundary-anchored, same as gmatch's %f[%a](%a+)%f[%A]), only
        // decorated when it's an exact (case-sensitive) key in
        // kCssColors and not already claimed.
        for (int i = 0; i < len;) {
            if (std::isalpha(static_cast<unsigned char>(line[static_cast<size_t>(i)]))) {
                int start = i;
                while (i < len && std::isalpha(static_cast<unsigned char>(line[static_cast<size_t>(i)]))) i++;
                std::string word = line.substr(static_cast<size_t>(start), static_cast<size_t>(i - start));
                auto it = kCssColors.find(word);
                if (it != kCssColors.end() && !covered[static_cast<size_t>(start)]) {
                    AddSwatch(this, ns, row, start, i, HexPairVal(it->second[0], it->second[1]),
                              HexPairVal(it->second[2], it->second[3]), HexPairVal(it->second[4], it->second[5]));
                }
            } else {
                i++;
            }
        }
    }
}

namespace {
/**
 * @brief Checks whether a character is valid inside the body of a URL (after "scheme://").
 * @param c The character to test.
 * @return True if `c` is alphanumeric or one of the allowed URL body punctuation characters.
 */
bool IsUrlBodyChar(unsigned char c) {
    if (std::isalnum(c)) return true;
    static const char *const kExtra = "-._~:/?#[]@!$&'()*+,;=%";
    return std::strchr(kExtra, static_cast<char>(c)) != nullptr;
}

struct UrlSpan {
    int col_start = 0;
    int col_end = 0;  // exclusive
};

// mep.nvim's own MEP_URL_PATTERN
// ("https?://[%w%-%._~:/?#%[%]@!$&'()*+,;=%%]+"): "http" + optional 's'
// + "://" + one-or-more of the body charset above.
/**
 * @brief Finds all http(s):// URLs in a line of text.
 * @param line The line of text to scan.
 * @return The column spans of every URL found, in order of appearance.
 */
std::vector<UrlSpan> FindUrlSpans(const std::string &line) {
    std::vector<UrlSpan> spans;
    const int len = static_cast<int>(line.size());
    int i = 0;
    while (i < len) {
        if (line.compare(static_cast<size_t>(i), 4, "http") == 0) {
            int j = i + 4;
            if (j < len && line[static_cast<size_t>(j)] == 's') j++;
            if (line.compare(static_cast<size_t>(j), 3, "://") == 0) {
                int body_start = j + 3;
                int k = body_start;
                while (k < len && IsUrlBodyChar(static_cast<unsigned char>(line[static_cast<size_t>(k)]))) k++;
                if (k > body_start) {
                    spans.push_back({i, k});
                    i = k;
                    continue;
                }
            }
        }
        i++;
    }
    return spans;
}
}  // namespace

std::string Editor::UrlUnderCursor() const {
    int row = 0, col = 0;
    GetCursorForLua(&row, &col);
    if (row < 0 || row >= Buf().LineCount()) return "";
    const std::string &line = Buf().lines[static_cast<size_t>(row)];
    for (const UrlSpan &sp : FindUrlSpans(line)) {
        if (col >= sp.col_start && col < sp.col_end) return line.substr(static_cast<size_t>(sp.col_start), static_cast<size_t>(sp.col_end - sp.col_start));
    }
    return "";
}

std::vector<std::string> Editor::ListUrls() const {
    std::vector<std::string> urls;
    const int n = Buf().LineCount();
    for (int row = 0; row < n; row++) {
        const std::string &line = Buf().lines[static_cast<size_t>(row)];
        for (const UrlSpan &sp : FindUrlSpans(line)) {
            urls.push_back(line.substr(static_cast<size_t>(sp.col_start), static_cast<size_t>(sp.col_end - sp.col_start)));
        }
    }
    return urls;
}

void Editor::GitGutterRefresh(const std::string &base) {
    const std::string &fname = Buf().filename;
    if (fname.empty()) return;
    int ns = CreateNamespace("git");
    // `base:path` is a pathspec, resolved relative to its own cwd -- run
    // with cwd set to the buffer's own directory and just its basename,
    // matching kBuiltinGit's own comment on why (an absolute path here
    // makes git refuse the pathspec outright).
    size_t slash = fname.find_last_of('/');
    std::string dir = slash == std::string::npos ? "." : fname.substr(0, slash);
    std::string base_name = slash == std::string::npos ? fname : fname.substr(slash + 1);
    std::string spec = base + ":" + base_name;

    auto lines = std::make_shared<std::vector<std::string>>();
    JobManager::Callbacks cb;
    /**
     * @brief Collects one line of `git show`'s stdout output as it streams in.
     * @param line The next line of output.
     */
    cb.on_stdout = [lines](const std::string &line) { lines->push_back(line); };
    /**
     * @brief Once `git show` exits, diffs the fetched base-revision lines against the current buffer and redraws the git-gutter decorations.
     * @param code The process exit code (unused).
     */
    cb.on_exit = [this, ns, lines](int /*code*/) {
        git_base_lines_ = *lines;
        std::vector<std::string> cur;
        const int n = Buf().LineCount();
        cur.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; i++) cur.push_back(Buf().lines[static_cast<size_t>(i)]);
        git_hunks_ = MyersDiffHunks(*lines, cur);
        ClearNamespace(ns);
        for (const DiffHunk &h : git_hunks_) {
            if (h.old_count == 0) {
                for (int r = h.new_start; r < h.new_start + h.new_count; r++) {
                    Decoration d;
                    d.row = r - 1;
                    d.whole_line = true;
                    d.hl_group = "Add";
                    d.sign = "+";
                    d.sign_hl = "Add";
                    AddDecoration(ns, d);
                }
            } else if (h.new_count == 0) {
                Decoration d;
                d.row = std::max(0, h.new_start - 1);
                d.whole_line = false;
                d.sign = "_";
                d.sign_hl = "Red";
                AddDecoration(ns, d);
            } else {
                for (int r = h.new_start; r < h.new_start + h.new_count; r++) {
                    Decoration d;
                    d.row = r - 1;
                    d.whole_line = true;
                    d.hl_group = "Yellow";
                    d.sign = "~";
                    d.sign_hl = "Yellow";
                    AddDecoration(ns, d);
                }
            }
        }
    };
    JobManager::Instance().Spawn({"git", "show", spec}, dir, cb);
}

const DiffHunk *Editor::GitHunkAtCursor() const {
    int row = 0, col = 0;
    GetCursorForLua(&row, &col);
    int row_1idx = row + 1;
    for (const DiffHunk &h : git_hunks_) {
        int lo = h.new_start;
        int hi = h.new_start + std::max(1, h.new_count) - 1;
        if (row_1idx >= lo && row_1idx <= hi) return &h;
    }
    return nullptr;
}

int Editor::GitNextHunkRow() const {
    int row = 0, col = 0;
    GetCursorForLua(&row, &col);
    int row_1idx = row + 1;
    for (const DiffHunk &h : git_hunks_) {
        if (h.new_start > row_1idx) return h.new_start;
    }
    if (!git_hunks_.empty()) return git_hunks_.front().new_start;
    return 0;
}

int Editor::GitPrevHunkRow() const {
    int row = 0, col = 0;
    GetCursorForLua(&row, &col);
    int row_1idx = row + 1;
    for (auto it = git_hunks_.rbegin(); it != git_hunks_.rend(); ++it) {
        if (it->new_start < row_1idx) return it->new_start;
    }
    if (!git_hunks_.empty()) return git_hunks_.back().new_start;
    return 0;
}

std::pair<bool, std::string> Editor::GitPreviewHunkText() const {
    const DiffHunk *h = GitHunkAtCursor();
    if (!h) return {false, ""};
    std::vector<std::string> lines;
    for (int i = h->old_start; i < h->old_start + h->old_count; i++) {
        std::string old_line =
            (i - 1 >= 0 && i - 1 < static_cast<int>(git_base_lines_.size())) ? git_base_lines_[static_cast<size_t>(i - 1)] : "";
        lines.push_back("-" + old_line);
    }
    const int n = Buf().LineCount();
    for (int i = h->new_start; i < h->new_start + h->new_count; i++) {
        std::string cur_line = (i - 1 >= 0 && i - 1 < n) ? Buf().lines[static_cast<size_t>(i - 1)] : "";
        lines.push_back("+" + cur_line);
    }
    if (lines.empty()) lines.emplace_back("(empty hunk)");
    std::string text;
    for (size_t k = 0; k < lines.size(); k++) {
        if (k > 0) text += '\n';
        text += lines[k];
    }
    return {true, text};
}

void Editor::GitResetHunk(const std::string &base) {
    const DiffHunk *h = GitHunkAtCursor();
    if (!h) {
        Notify("No hunk under cursor", NotifyLevel::Warn);
        return;
    }
    std::vector<std::string> repl;
    for (int i = h->old_start; i < h->old_start + h->old_count; i++) {
        if (i - 1 >= 0 && i - 1 < static_cast<int>(git_base_lines_.size())) repl.push_back(git_base_lines_[static_cast<size_t>(i - 1)]);
    }
    int new_start = h->new_start, new_count = h->new_count;
    ReplaceLinesForLua(new_start - 1, new_start - 1 + new_count, repl);
    GitGutterRefresh(base);
}

void Editor::GitStageHunk() {
    const DiffHunk *h = GitHunkAtCursor();
    const std::string &fname = Buf().filename;
    if (!h || fname.empty()) {
        Notify("No hunk under cursor", NotifyLevel::Warn);
        return;
    }
    std::string old_hdr = h->old_count == 0 ? (std::to_string(h->old_start) + ",0")
                                             : (std::to_string(h->old_start) + "," + std::to_string(h->old_count));
    std::string new_hdr = h->new_count == 0 ? (std::to_string(h->new_start) + ",0")
                                             : (std::to_string(h->new_start) + "," + std::to_string(h->new_count));
    std::string patch;
    patch += "diff --git a/" + fname + " b/" + fname + "\n";
    patch += "--- a/" + fname + "\n";
    patch += "+++ b/" + fname + "\n";
    patch += "@@ -" + old_hdr + " +" + new_hdr + " @@\n";
    const int n = Buf().LineCount();
    for (int i = h->old_start; i < h->old_start + h->old_count; i++) {
        std::string old_line =
            (i - 1 >= 0 && i - 1 < static_cast<int>(git_base_lines_.size())) ? git_base_lines_[static_cast<size_t>(i - 1)] : "";
        patch += "-" + old_line + "\n";
    }
    for (int i = h->new_start; i < h->new_start + h->new_count; i++) {
        std::string cur_line = (i - 1 >= 0 && i - 1 < n) ? Buf().lines[static_cast<size_t>(i - 1)] : "";
        patch += "+" + cur_line + "\n";
    }
    JobManager::Callbacks cb;
    /**
     * @brief Reports whether `git apply --cached` succeeded in staging the hunk.
     * @param code The process exit code; 0 means success.
     */
    cb.on_exit = [this](int code) {
        if (code == 0) {
            Notify("Staged hunk");
        } else {
            Notify("git apply failed", NotifyLevel::Error);
        }
    };
    int id = JobManager::Instance().Spawn({"git", "apply", "--cached", "--unidiff-zero", "-"}, ActiveRoot(), cb);
    JobManager::Instance().WriteStdin(id, patch);
    JobManager::Instance().CloseStdin(id);
}

void Editor::ReplaceLinesForLua(int start_row, int end_row, const std::vector<std::string> &lines) {
    Buffer &buf = Buf();
    start_row = std::max(0, std::min(start_row, buf.LineCount()));
    end_row = std::max(start_row, std::min(end_row, buf.LineCount()));
    PushUndo();
    buf.lines.erase(buf.lines.begin() + start_row, buf.lines.begin() + end_row);
    buf.lines.insert(buf.lines.begin() + start_row, lines.begin(), lines.end());
    if (buf.lines.empty()) buf.lines.emplace_back("");
    buf.modified = true;
    ClampCursor();
}

void Editor::SetBufferLinesForLua(int buffer_id, const std::vector<std::string> &lines) {
    if (buffer_id < 0 || buffer_id >= static_cast<int>(buffers_.size())) return;
    Buffer &buf = buffers_[static_cast<size_t>(buffer_id)];
    // No PushUndo/undo-stack entry -- this is for streaming external
    // process output (Part VI Phase 27's terminal/Run/REPL) into a
    // dedicated buffer that isn't the active pane's, where "undo" has no
    // sensible meaning (there's nothing the user typed to undo).
    buf.lines = lines.empty() ? std::vector<std::string>{""} : lines;
    buf.modified = false;
}

// --- Participant-addressed editing (COLLAB_CURSORS_PLAN.md) ---------------

void Editor::PushUndoForBuffer(int buffer_id) {
    if (buffer_id < 0 || buffer_id >= static_cast<int>(buffers_.size())) return;
    Buffer &buf = buffers_[static_cast<size_t>(buffer_id)];
    buf.undo_stack.push_back(buf.lines);
    if (buf.undo_stack.size() > kMaxUndo) buf.undo_stack.erase(buf.undo_stack.begin());
    buf.redo_stack.clear();
    change_epoch_++;
}

CursorPos Editor::ClampPositionInBuffer(int buffer_id, CursorPos pos) const {
    if (buffer_id < 0 || buffer_id >= static_cast<int>(buffers_.size())) return {0, 0};
    const Buffer &buf = buffers_[static_cast<size_t>(buffer_id)];
    const int row = std::max(0, std::min(pos.row, static_cast<int>(buf.lines.size()) - 1));
    const int col = std::max(0, std::min(pos.col, static_cast<int>(buf.lines[static_cast<size_t>(row)].size())));
    return {row, col};
}

void Editor::SetLineAt(int buffer_id, int row, const std::string &text) {
    if (buffer_id < 0 || buffer_id >= static_cast<int>(buffers_.size())) return;
    Buffer &buf = buffers_[static_cast<size_t>(buffer_id)];
    if (row < 0 || row >= static_cast<int>(buf.lines.size())) return;
    PushUndoForBuffer(buffer_id);
    buf.lines[static_cast<size_t>(row)] = text;
    buf.modified = true;
}

void Editor::ReplaceLinesAt(int buffer_id, int start_row, int end_row, const std::vector<std::string> &lines) {
    if (buffer_id < 0 || buffer_id >= static_cast<int>(buffers_.size())) return;
    Buffer &buf = buffers_[static_cast<size_t>(buffer_id)];
    start_row = std::max(0, std::min(start_row, static_cast<int>(buf.lines.size())));
    end_row = std::max(start_row, std::min(end_row, static_cast<int>(buf.lines.size())));
    PushUndoForBuffer(buffer_id);
    buf.lines.erase(buf.lines.begin() + start_row, buf.lines.begin() + end_row);
    buf.lines.insert(buf.lines.begin() + start_row, lines.begin(), lines.end());
    if (buf.lines.empty()) buf.lines.emplace_back("");
    buf.modified = true;
}

// Splices `text` in as raw bytes at an explicit buffer_id/position --
// see this method's declaration in editor.h for why it doesn't reuse
// InsertChar/InsertNewline (their ASCII-only/byte-wise handling silently
// drops non-ASCII text). Mark/fold shifting (ShiftMarksForLineEdit/
// ShiftFoldsForLineEdit) is deliberately skipped here -- both are Buf()-
// implicit (operate on the active buffer only), and generalizing them is
// out of scope for v1; a remote participant inserting/deleting lines in
// a buffer that isn't currently active can leave that buffer's own
// marks/folds stale, same accepted limitation as ClampPositionInBuffer's
// own fold-unaware clamping.
CursorPos Editor::InsertTextAt(int buffer_id, CursorPos at, const std::string &text) {
    if (buffer_id < 0 || buffer_id >= static_cast<int>(buffers_.size())) return at;
    Buffer &buf = buffers_[static_cast<size_t>(buffer_id)];
    const CursorPos start = ClampPositionInBuffer(buffer_id, at);

    std::vector<std::string> segments;
    size_t begin = 0;
    for (;;) {
        const size_t nl = text.find('\n', begin);
        segments.push_back(text.substr(begin, nl == std::string::npos ? std::string::npos : nl - begin));
        if (nl == std::string::npos) break;
        begin = nl + 1;
    }

    PushUndoForBuffer(buffer_id);
    std::string &line = buf.lines[static_cast<size_t>(start.row)];
    const std::string tail = line.substr(static_cast<size_t>(start.col));
    line.replace(static_cast<size_t>(start.col), std::string::npos, segments.front());

    CursorPos result;
    if (segments.size() == 1) {
        line += tail;
        result = {start.row, start.col + static_cast<int>(segments.front().size())};
    } else {
        for (size_t i = 1; i + 1 < segments.size(); i++) {
            buf.lines.insert(buf.lines.begin() + start.row + static_cast<int>(i), segments[i]);
        }
        buf.lines.insert(buf.lines.begin() + start.row + static_cast<int>(segments.size()) - 1, segments.back() + tail);
        result = {start.row + static_cast<int>(segments.size()) - 1, static_cast<int>(segments.back().size())};
    }
    buf.modified = true;
    return result;
}

std::string Editor::BufferLabelForLua(int buffer_id) const {
    if (buffer_id < 0 || buffer_id >= static_cast<int>(buffers_.size())) return "";
    // `:bd`/`:bdelete`'d (Buffer::deleted) -- l_buffer_list (lua_env.cpp)
    // already skips any buffer_id whose label comes back empty, so this
    // is what actually keeps a deleted buffer out of the Buffers picker
    // without needing a second, separate filter there.
    if (buffers_[static_cast<size_t>(buffer_id)].deleted) return "";
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
    const Buffer &buf = buffers_[static_cast<size_t>(buffer_id)];
    std::string label = buf.filename.empty() ? "[No Name]" : buf.filename;
    if (buf.modified) label += " [+]";
    return label;
}

std::string Editor::BufferFilenameForLua(int buffer_id) const {
    if (buffer_id < 0 || buffer_id >= static_cast<int>(buffers_.size())) return "";
    if (GetTerminal(buffer_id)) return "";
    return buffers_[static_cast<size_t>(buffer_id)].filename;
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
    // Called from an async job callback (mep.insert_text) an arbitrary
    // time after the request that produced `text` was kicked off --
    // AI chat streaming and speech-to-text dictation both land their
    // response/transcript this way, one chunk per callback, over a
    // period of several seconds. Neither pins down which buffer/pane it
    // targets: every chunk just inserts at whatever CurPane().cursor
    // happens to be *at that moment*, matching typed-text semantics but
    // meaning the cursor can have moved -- buffer switched, lines
    // deleted, a shorter file opened in the same pane -- since the
    // stream started. InsertNewline/InsertChar index Buf().lines[row]
    // with no bounds check at all, so a stale row/col (most reliably hit
    // with two such streams live at once, e.g. dictating while an AI
    // response streams in) reads past the vector into uninitialized
    // memory and corrupts/crashes on the very next edit -- confirmed via
    // two independent core dumps, both faulting inside InsertNewline's
    // vector<string>::insert on a garbage length read this way. Re-clamp
    // against whatever Buf() actually is right now, right before
    // touching it, the same way ordinary cursor motion already does via
    // ClampCursor() -- but with insert-mode column bounds (col == line
    // length is valid, appending at end-of-line) regardless of the
    // current mode_, since typed/dictated/streamed text always behaves
    // like Insert-mode input here even if a stream never called
    // mep.enter_insert() first (mep.ai_send_text doesn't).
    CursorPos &cursor = CurPane().cursor;
    cursor.row = std::max(0, std::min(cursor.row, Buf().LineCount() - 1));
    cursor.col = std::max(0, std::min(cursor.col, LineLen(cursor.row)));
    PushUndo();
    for (char c : text) {
        if (c == '\n') InsertNewline();
        else InsertChar(static_cast<unsigned char>(c));
    }
}

void Editor::ChangeVisualSelectionForLua() { ApplyOperatorToSelectionOrCurrentLine('c'); }

void Editor::EnterNormalForLua() { EnterNormal(); }

void Editor::EnterInsertForLua() {
    PushUndo();
    EnterInsert();
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
    // WORKSPACES_PLAN.md Phase 4: a relative `path` belongs to the
    // buffer's own workspace root, which is only the process cwd while
    // that workspace is active -- `:wa` from another workspace (a global
    // reader by design) must still write the right file. Only the I/O
    // below uses the resolved form; buf.filename/status text keep the
    // user's own spelling.
    const std::string io_path = ResolveBufferPath(buf, path);
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
            char *result = mep_js_write_file(io_path.c_str(), content.c_str());
            std::string res(result);
            std::free(result);
            if (res != "OK") {
                status_message_ = "E212: Can't open file for writing" +
                                   (res.rfind("ERR\n", 0) == 0 ? " (" + res.substr(4) + ")" : "");
                return false;
            }
#else
            std::ofstream csv_out(io_path, std::ios::binary);
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
        std::ofstream sheet_out(io_path, std::ios::binary);
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
        std::ofstream office_out(io_path, std::ios::binary);
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
    char *result = mep_js_write_file(io_path.c_str(), content.c_str());
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
    std::ofstream out(io_path, std::ios::binary);
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
        for (const std::string &p : list) arr.push_back(p);
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
    for (const std::string &p : list) arr.push_back(p);
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

bool Editor::IsOnlyPaneOverall() const { return Tabs().size() == 1 && Tabs()[0].root->dir == SplitDir::Leaf; }

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
    if (float_node_) {
        CloseFloatPane();
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

void Editor::ReloadCurrentBuffer(bool force) {
    if (Buf().filename.empty()) {
        status_message_ = "E32: No file name";
        return;
    }
    if (!force && Buf().modified) {
        status_message_ = "E37: No write since last change (add ! to override)";
        return;
    }
    std::ifstream in(Buf().filename, std::ios::binary);
    if (!in) {
        status_message_ = "E484: Can't open file \"" + Buf().filename + "\"";
        return;
    }
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    PushUndo();
    Buf().lines = SplitIntoLines(content);
    Buf().modified = false;
    ClampCursor();
    status_message_ = "\"" + Buf().filename + "\" " + std::to_string(Buf().LineCount()) + "L reloaded";
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
            if (!BufferInActiveWorkspace(static_cast<int>(i))) continue;
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
    status_message_ = existed ? "\"" + path + "\" " + std::to_string(buffers_[static_cast<size_t>(id)].LineCount()) + "L loaded"
                               : "\"" + path + "\" [New]";
    SyncModeToActivePaneBuffer();
}

void Editor::DropUnusedInitialBuffer() {
    if (buffers_.size() < 2 || CurPane().buffer_id == 0) return;
    const Buffer &first = buffers_[0];
    if (!first.filename.empty() || first.modified || first.lines.size() != 1 || !first.lines[0].empty()) return;
    buffers_.erase(buffers_.begin());
    CurPane().buffer_id--;
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

namespace {
std::string BufferText(const Buffer &buffer) {
    std::string text;
    for (size_t i = 0; i < buffer.lines.size(); ++i) { if (i) text.push_back('\n'); text += buffer.lines[i]; }
    return text;
}
std::vector<std::string> BufferLines(const std::string &text) {
    std::vector<std::string> lines; size_t begin = 0;
    while (begin <= text.size()) { const size_t end = text.find('\n', begin); lines.push_back(text.substr(begin, end == std::string::npos ? std::string::npos : end - begin)); if (end == std::string::npos) break; begin = end + 1; }
    if (lines.empty()) { lines.emplace_back(""); }
    return lines;
}
}  // namespace

void Editor::JoinCollaboration(const std::string &url, const std::string &name) {
    if (url.empty()) { status_message_ = "Usage: :CollabJoin wss://host/v1/session/<id>?secret=<secret> [name]"; return; }
    LeaveCollaboration();
    collaboration_ = std::make_unique<mep::collab::CollabSession>(url, name, BufferText(Buf()));
    collaboration_buffer_id_ = CurrentBufferId();
    collaboration_->Start();
    status_message_ = "Collaboration: connecting…";
}
void Editor::LeaveCollaboration() {
    if (collaboration_) { collaboration_->Stop(); collaboration_.reset(); collaboration_buffer_id_ = -1; status_message_ = "Collaboration: disconnected"; }
}
void Editor::TickCollaboration() {
    if (!collaboration_) return;
    if (CurrentBufferId() != collaboration_buffer_id_) return;
    std::string merged;
    if (collaboration_->Synchronize(BufferText(Buf()), &merged)) {
        Buf().lines = BufferLines(merged); Buf().modified = true; ClampCursor();
        status_message_ = "Collaboration: remote changes applied";
    }
    collaboration_->SetPresence(CurPane().cursor.row, CurPane().cursor.col);
    const std::string error = collaboration_->error();
    if (!error.empty()) status_message_ = "Collaboration: " + error;
}
bool Editor::CollaborationActive() const {
    return collaboration_ && collaboration_->connected();
}
std::vector<Editor::CollaborationPeerInfo> Editor::CollaborationPeers() const {
    std::vector<CollaborationPeerInfo> result;
    if (collaboration_) for (const auto &peer : collaboration_->Collaborators()) result.push_back({peer.id, peer.name, peer.row, peer.col, peer.has_location});
    return result;
}
std::vector<Editor::ParticipantInfo> Editor::Participants() const {
    std::vector<ParticipantInfo> result;
    for (const auto &peer : CollaborationPeers()) {
        result.push_back({peer.id, peer.name, ParticipantKind::Human, collaboration_buffer_id_, peer.row, peer.col, peer.has_location, ""});
    }
    for (const auto &agent : mep::agent::AgentParticipants()) {
        result.push_back({agent.id, agent.name, ParticipantKind::Agent, agent.buffer_id, agent.row, agent.col, agent.has_location, agent.status,
                          agent.terminal_buffer_id});
    }
    for (const auto &local : local_participants_) {
        result.push_back(local);
    }
    return result;
}

void Editor::SetLocalParticipant(const std::string &id, const std::string &name, int buffer_id, int row, int col, const std::string &status) {
    CursorPos clamped = ClampPositionInBuffer(buffer_id, {row, col});
    for (auto &p : local_participants_) {
        if (p.id != id) continue;
        p.name = name;
        p.buffer_id = buffer_id;
        p.row = clamped.row;
        p.col = clamped.col;
        p.has_location = true;
        p.status = status;
        return;
    }
    local_participants_.push_back({id, name, ParticipantKind::Agent, buffer_id, clamped.row, clamped.col, true, status});
}

void Editor::ClearLocalParticipant(const std::string &id) {
    local_participants_.erase(
        std::remove_if(local_participants_.begin(), local_participants_.end(),
                        [&id](const ParticipantInfo &p) { return p.id == id; }),
        local_participants_.end());
}

bool Editor::JumpToParticipant(const std::string &id) {
    for (const auto &p : Participants()) {
        if (p.id != id) continue;
        if (!p.has_location) {
            status_message_ = (p.name.empty() ? p.id : p.name) + " has no known cursor location yet";
            return false;
        }
        if (p.buffer_id >= 0 && p.buffer_id < static_cast<int>(buffers_.size()) && p.buffer_id != CurPane().buffer_id) {
            SwitchToBufferForLua(p.buffer_id);
        }
        CurPane().cursor = {p.row, p.col};
        ClampCursor();
        status_message_ = "Jumped to " + (p.name.empty() ? p.id : p.name);
        return true;
    }
    status_message_ = "No such participant";
    return false;
}
