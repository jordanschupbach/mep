// A dependency-free replacement for mcp/server.ts: speaks the Model
// Context Protocol directly over stdio (newline-delimited JSON-RPC 2.0,
// per the MCP spec's stdio transport) and relays every tool call onto
// mep's own agent-control Unix socket (src/agent_rpc.h/.cpp, Content-
// Length-framed JSON-RPC 2.0, via rpc_framing.h). Exists so that using
// mep's MCP integration needs nothing beyond a C++ toolchain -- no Deno/
// Node/npm runtime, no @modelcontextprotocol/sdk, no zod -- since
// mcp/server.ts turned out to depend on a `deno` binary that isn't
// guaranteed to be on $PATH outside this project's own `nix develop`
// shell. mcp/server.ts + mcp/mep_client.ts remain as a reference
// implementation/fallback; this binary is the one `claude mcp add`
// should point at going forward (see README.html's setup instructions).
//
// Deliberately single-threaded and fully synchronous: an MCP client
// drives this over stdio one request at a time and waits for each
// response before sending the next, and mep's own agent socket answers
// each request synchronously within one PollOnce() (see agent_rpc.h's
// own threading comment) -- there is never a reason to have two socket
// operations in flight at once, so no thread/mutex/condvar machinery is
// needed anywhere in this file.
//
// Not linked against mep_core/raylib (see CMakeLists.txt) -- this talks
// to an already-running mep purely as an external client of its Unix
// socket, the same way `socat`, a human, or mcp/mep_client.ts would.

#include "json.h"
#include "persist.h"
#include "rpc_framing.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// --- mep agent-socket client ------------------------------------------------

// Mirrors mcp/mep_client.ts's discoverSocketPath(): an explicit
// MEP_AGENT_SOCKET always wins; otherwise exactly one `*.sock` under
// MepAgentSocketDir() (persist.h -- the same helper mep's own
// src/agent_rpc.cpp binds its listening socket under, so this can never
// drift out of sync with where mep actually puts it) is used
// automatically. Zero or several are both errors, same as the JS
// version, for the same reason: guessing which of several running mep
// windows to drive would be worse than asking.
std::string DiscoverSocketPath() {
    if (const char *env = std::getenv("MEP_AGENT_SOCKET")) return env;

    std::string dir = MepAgentSocketDir();
    if (dir.empty()) throw std::runtime_error("cannot determine mep's agent-sockets directory (no $HOME/$XDG_DATA_HOME)");

    std::vector<std::string> candidates;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".sock") candidates.push_back(entry.path().string());
    }
    if (ec) throw std::runtime_error("no mep agent sockets found (" + dir + " doesn't exist) -- is mep running? it must be a native build, not wasm");
    if (candidates.empty()) {
        throw std::runtime_error("no mep agent sockets found in " + dir + " -- is mep running? it must be a native build, not wasm");
    }
    if (candidates.size() > 1) {
        std::string list;
        for (size_t i = 0; i < candidates.size(); i++) {
            if (i) list += ", ";
            list += candidates[i];
        }
        throw std::runtime_error("multiple mep agent sockets found: " + list +
                                  " -- set MEP_AGENT_SOCKET to the one you want (call the \"mep_session_info\" tool "
                                  "against each, or run :AgentSocket in the mep window, to tell them apart)");
    }
    return candidates[0];
}

// One persistent connection to mep's agent socket, reconnected on demand
// after a drop -- mirrors MepClient's own lifecycle (mcp/mep_client.ts)
// but synchronous throughout instead of promise-based, since this whole
// process only ever does one thing at a time (see this file's own top
// comment). `events_`/`parsed_queue_` mirror MepClient's #events/#framer
// split: parsed_queue_ holds frames read off the wire but not yet
// classified as "the answer to the call in progress" vs "an unsolicited
// event.* push"; events_ holds the latter, for mep_poll_events to drain.
class MepSocket {
public:
    ~MepSocket() { Disconnect(); }

    // Sends `method`/`params` as a new JSON-RPC request and blocks until
    // that exact request's response arrives, queuing any interleaved
    // event.* notification for DrainEvents(). Throws on any failure
    // (connect, write, disconnect mid-wait, or an RPC-level error
    // response) -- callers turn that into an MCP tool error result.
    Json Call(const std::string &method, const Json &params) {
        EnsureConnected();
        const int id = next_id_++;
        Json request = Json::Object();
        request["jsonrpc"] = "2.0";
        request["id"] = id;
        request["method"] = method;
        request["params"] = params;
        if (!WriteAll(FrameRpcMessage(request.dump()))) {
            Disconnect();
            throw std::runtime_error("mep agent socket: write failed (connection closed)");
        }
        for (;;) {
            Json msg = ReadNextMessage();
            if (msg.contains("id") && static_cast<int>(msg.get("id").as_double()) == id) {
                if (msg.contains("error")) {
                    const Json &err = msg.get("error");
                    throw std::runtime_error("mep RPC error " + std::to_string(err.get("code").as_int()) + ": " +
                                              err.get("message").as_string("(no message)"));
                }
                return msg.get("result");
            }
            if (msg.contains("method")) QueueEvent(msg);
            // An "id" that isn't ours and isn't an event notification shouldn't
            // happen on this strictly one-call-at-a-time protocol -- ignore it
            // rather than getting stuck waiting for a response that already
            // went to a request we're not tracking.
        }
    }

    // Non-blocking: drains whatever's already sitting on the socket into
    // events_ (best-effort -- a disconnect here is silently swallowed,
    // since mep_poll_events has nothing useful to report beyond "nothing
    // new"), then hands back and clears everything queued so far.
    Json DrainEvents() {
        DrainAvailableNonBlocking();
        Json out = Json::Array();
        for (auto &ev : events_) out.push_back(ev);
        events_.clear();
        return out;
    }

private:
    static constexpr size_t kMaxQueuedEvents = 2000;

    int fd_ = -1;
    int next_id_ = 1;
    std::string read_buffer_;
    std::deque<Json> parsed_queue_;
    std::vector<Json> events_;

    void QueueEvent(const Json &msg) {
        events_.push_back(msg);
        if (events_.size() > kMaxQueuedEvents) events_.erase(events_.begin(), events_.begin() + static_cast<long>(events_.size() - kMaxQueuedEvents));
    }

    void EnsureConnected() {
        if (fd_ >= 0) return;
        const std::string path = DiscoverSocketPath();
        fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ < 0) throw std::runtime_error(std::string("socket() failed: ") + std::strerror(errno));
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (path.size() >= sizeof(addr.sun_path)) {
            close(fd_);
            fd_ = -1;
            throw std::runtime_error("socket path too long: " + path);
        }
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        if (connect(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
            const std::string err = std::strerror(errno);
            close(fd_);
            fd_ = -1;
            throw std::runtime_error("failed to connect to " + path + ": " + err);
        }
    }

    void Disconnect() {
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
        read_buffer_.clear();
        parsed_queue_.clear();
    }

    // send() with MSG_NOSIGNAL, not write() -- mep can exit/restart out
    // from under an already-connected bridge (the whole point of this
    // being a separate long-lived process), and writing to a socket
    // whose peer already closed raises SIGPIPE by default, which kills
    // this entire process before the `n <= 0` check below ever runs.
    // MSG_NOSIGNAL turns that into an ordinary EPIPE return instead, so
    // the existing Disconnect()-and-let-the-next-Call()-reconnect path
    // actually gets to run.
    bool WriteAll(const std::string &data) {
        size_t off = 0;
        while (off < data.size()) {
            ssize_t n = send(fd_, data.data() + off, data.size() - off, MSG_NOSIGNAL);
            if (n <= 0) return false;
            off += static_cast<size_t>(n);
        }
        return true;
    }

    // Blocks until at least one full frame is available (parsing more off
    // the wire as needed) and returns the oldest one not yet handed back.
    Json ReadNextMessage() {
        for (;;) {
            if (!parsed_queue_.empty()) {
                Json m = std::move(parsed_queue_.front());
                parsed_queue_.pop_front();
                return m;
            }
            char buf[65536];
            ssize_t n = recv(fd_, buf, sizeof(buf), 0);
            if (n <= 0) {
                Disconnect();
                throw std::runtime_error("mep agent socket connection closed");
            }
            read_buffer_.append(buf, static_cast<size_t>(n));
            bool ok = PumpRpcFrames(read_buffer_, [&](const std::string &body) {
                Json parsed;
                if (Json::Parse(body, &parsed)) parsed_queue_.push_back(std::move(parsed));
            });
            if (!ok) {
                Disconnect();
                throw std::runtime_error("mep agent socket: framing error");
            }
        }
    }

    void DrainAvailableNonBlocking() {
        if (fd_ < 0) return;
        for (;;) {
            while (!parsed_queue_.empty()) {
                Json m = std::move(parsed_queue_.front());
                parsed_queue_.pop_front();
                if (m.contains("method")) QueueEvent(m);
            }
            pollfd pfd{fd_, POLLIN, 0};
            if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN)) return;  // nothing waiting right now
            char buf[65536];
            ssize_t n = recv(fd_, buf, sizeof(buf), 0);
            if (n <= 0) {
                Disconnect();
                return;  // best-effort -- see this method's own comment
            }
            read_buffer_.append(buf, static_cast<size_t>(n));
            PumpRpcFrames(read_buffer_, [&](const std::string &body) {
                Json parsed;
                if (Json::Parse(body, &parsed)) parsed_queue_.push_back(std::move(parsed));
            });
        }
    }
};

MepSocket g_mep;

// --- Tool table --------------------------------------------------------
// One entry per mcp_server.ts registerTool call it mirrors -- keep the
// two in sync by hand (see this file's own top comment on why both
// still exist). `input_schema` is a literal JSON Schema object (as raw
// text, parsed once into the table below) standing in for what the JS
// side's zod schemas produce; every default value (button="left",
// clicks=1, steps=12, ...) is applied by the mep-side C++ handler itself
// (main.cpp's RegisterUiAutomationMethods, agent_rpc.cpp's Dispatch), not
// here, so omitting an optional argument is always safe. `rpc_method`
// empty means "handled locally, never reaches mep's socket" -- only
// mep_poll_events does this (see HandleToolsCall).
struct ToolSpec {
    const char *name;
    const char *rpc_method;
    const char *description;
    const char *input_schema;  // JSON Schema, as literal text
    bool read_only;            // matches server.ts's READ_ONLY_METHODS
};

constexpr const char *kNoInput = R"({"type":"object"})";

const ToolSpec kTools[] = {
    {"mep_identify", "session.identify",
     "Set your own display name, shown at your cursor and in mep's tab-bar participant list (with a robot icon, "
     "since you're an AI agent) so the human can see who's editing what. Called automatically once when this MCP "
     "server starts (default name from MEP_AGENT_NAME, else \"Claude\") -- call this again any time to rename "
     "yourself mid-session.",
     R"({"type":"object","properties":{"name":{"type":"string"}},"required":["name"]})", false},
    {"mep_set_status", "session.setStatus",
     "Report what you're currently doing, shown as a small badge on your tab-bar chip so the human can tell at a "
     "glance without reading your output: \"thinking\" (reasoning, no edits yet), \"writing\" (set automatically by "
     "mep_buffer_insert_text/set_line/replace_lines too), \"awaiting_input\" (you've asked the human a question and "
     "are waiting on their reply), \"done\" (finished this task), or \"idle\" (clear the badge). Your next real "
     "action after \"done\" auto-clears the badge to \"thinking\" if you forget to call this yourself.",
     R"({"type":"object","properties":{"status":{"type":"string","enum":["idle","thinking","writing","awaiting_input","done"]}},"required":["status"]})",
     false},
    {"mep_list_participants", "session.listParticipants",
     "List everyone currently present in this mep instance -- other connected AI agents and human :CollabJoin "
     "peers -- with each one's name, kind, buffer_id/cursor (if positioned), and status badge (agents only).",
     kNoInput, true},
    {"mep_cursor_get", "cursor.get", "Get your own cursor's position (0-indexed row/col) and which buffer it's in.",
     kNoInput, true},
    {"mep_cursor_set", "cursor.set",
     "Move your own cursor to a specific position (0-indexed row/col), optionally in a different buffer_id -- does "
     "not affect the human's real cursor or any other participant's.",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer","description":"defaults to wherever your cursor already is"},"row":{"type":"integer"},"col":{"type":"integer"}},"required":["row","col"]})",
     false},
    {"mep_buffer_insert_text", "buffer.insertText",
     "Insert text at your own cursor position, as if typed, and advance your cursor past it -- does not touch the "
     "human's real cursor or type into whatever buffer they currently have open.",
     R"({"type":"object","properties":{"text":{"type":"string","description":"Text to insert; use \n for newlines"}},"required":["text"]})",
     false},
    {"mep_buffer_set_line", "buffer.setLine",
     "Replace one line (0-indexed) of the buffer your own cursor is currently in with new text.",
     R"({"type":"object","properties":{"row":{"type":"integer"},"text":{"type":"string"}},"required":["row","text"]})",
     false},
    {"mep_buffer_replace_lines", "buffer.replaceLines",
     "Replace lines [start, end) (0-indexed, end exclusive) of the buffer your own cursor is currently in with the "
     "given lines -- a general multi-line splice.",
     R"({"type":"object","properties":{"start":{"type":"integer"},"end":{"type":"integer"},"lines":{"type":"array","items":{"type":"string"}}},"required":["start","end","lines"]})",
     false},
    {"mep_buffer_set_lines", "buffer.setLines",
     "Replace a specific buffer's *entire* content by id, regardless of which pane is active -- for writing to a "
     "buffer you aren't currently viewing. Does not create undo history.",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"lines":{"type":"array","items":{"type":"string"}}},"required":["buffer_id","lines"]})",
     false},
    {"mep_buffer_switch", "buffer.switch",
     "Move your own cursor to a different buffer by id (does not change what the human's real pane is showing).",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"}},"required":["buffer_id"]})", false},
    {"mep_buffer_create", "buffer.create", "Create a new empty buffer without switching any pane to it. Returns its buffer_id.",
     kNoInput, false},
    {"mep_buffer_filename", "buffer.filename", "Get a buffer's filename by id (empty string for an unsaved/terminal buffer).",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"}},"required":["buffer_id"]})", true},
    {"mep_buffer_list", "buffer.list",
     "List open buffers: id, filename, modified flag, line count, workspace_id. Scoped to the active workspace by "
     "default; pass workspace: \"all\" for every workspace, or a workspace id for one specific workspace.",
     R"({"type":"object","properties":{"workspace":{"description":"an integer workspace id, or \"all\""}}})", true},
    {"mep_workspace_list", "workspace.list",
     "List the active project's workspaces: id, name, root directory, git branch, primary flag, creating flag (git "
     "worktree still being added), active flag.",
     kNoInput, true},
    {"mep_workspace_switch", "workspace.switch",
     "Switch to a workspace by id or name. Changes the working directory to that workspace's root (its git worktree).",
     R"({"type":"object","properties":{"id":{"type":"integer"},"name":{"type":"string"}}})", false},
    {"mep_workspace_create", "workspace.create",
     "Create a workspace. On a git project this adds a worktree on a new branch of the same name (asynchronously: "
     "the reply has creating=true until git finishes -- poll mep_workspace_list, or watch mep_poll_events). "
     "attach=true attaches to an existing branch instead of creating one.",
     R"({"type":"object","properties":{"name":{"type":"string"},"attach":{"type":"boolean"}},"required":["name"]})",
     false},
    {"mep_workspace_delete", "workspace.delete",
     "Delete a workspace by id or name (removes its git worktree; the branch is kept). Refuses the primary "
     "workspace, and one with unsaved buffers unless force=true.",
     R"({"type":"object","properties":{"id":{"type":"integer"},"name":{"type":"string"},"force":{"type":"boolean"}}})",
     false},
    {"mep_project_list", "project.list", "List the loaded projects: id, name, root, is_git, workspace_count, active flag.",
     kNoInput, true},
    {"mep_project_switch", "project.switch", "Switch to a loaded project by id or name.",
     R"({"type":"object","properties":{"id":{"type":"integer"},"name":{"type":"string"}}})", false},
    {"mep_project_open", "project.open",
     "Load a directory as a project (or switch to it if already loaded) and make it active; its saved "
     "workspaces/tabs are restored.",
     R"({"type":"object","properties":{"root":{"type":"string"}},"required":["root"]})", false},
    {"mep_buffer_get_lines", "buffer.getLines",
     "Read a range of lines [start, end) (0-indexed, end exclusive) from a buffer by id. Omit start/end for the "
     "whole buffer.",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"start":{"type":"integer"},"end":{"type":"integer"}},"required":["buffer_id"]})",
     true},
    {"mep_file_open", "file.open", "Open a file by path (creates it, same as :e in vim, if it doesn't exist yet).",
     R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})", false},
    {"mep_file_save", "file.save", "Save a buffer to disk. Omit path to save the active buffer to its own existing filename.",
     R"({"type":"object","properties":{"path":{"type":"string"}}})", false},
    {"mep_pane_split", "pane.split", "Split the active pane, focusing the new one. Returns the new pane's id.",
     R"({"type":"object","properties":{"dir":{"type":"string","enum":["horizontal","vertical"],"description":"default horizontal"},"file":{"type":"string","description":"file to open in the new pane; default reuses the current buffer"}}})",
     false},
    {"mep_pane_close", "pane.close", "Close the active pane.", kNoInput, false},
    {"mep_pane_resize", "pane.resize", "Nudge the active pane's split-tree share in a direction.",
     R"({"type":"object","properties":{"direction":{"type":"string","description":"e.g. \"left\"/\"right\"/\"up\"/\"down\""},"step":{"type":"number"}},"required":["direction"]})",
     false},
    {"mep_pane_focus", "pane.focus", "Make a specific pane (by id) the active one within its tab.",
     R"({"type":"object","properties":{"pane_id":{"type":"integer"}},"required":["pane_id"]})", false},
    {"mep_pane_split_with_buffer", "pane.splitWithBuffer",
     "Move a buffer tab from one pane into a new split off another pane (drag-and-drop-onto-an-edge equivalent).",
     R"({"type":"object","properties":{"source_pane_id":{"type":"integer"},"buffer_id":{"type":"integer"},"dest_pane_id":{"type":"integer"},"dir":{"type":"string","enum":["horizontal","vertical"]},"before":{"type":"boolean","description":"place the new split before (left/top of) dest_pane_id, else after"}},"required":["source_pane_id","buffer_id","dest_pane_id","dir","before"]})",
     false},
    {"mep_pane_get", "pane.get", "Get one pane's id/buffer_id/cursor/selection/scroll. Omit pane_id for the active pane.",
     R"({"type":"object","properties":{"pane_id":{"type":"integer"}}})", true},
    {"mep_command_run", "command.run",
     "Run any mep `:` ex-command (without the leading colon), e.g. \"w\", \"s/foo/bar/g\", \"split\", \"qa!\". The "
     "general escape hatch for anything not covered by a more specific tool.",
     R"({"type":"object","properties":{"cmd":{"type":"string"}},"required":["cmd"]})", false},
    {"mep_session_info", "session.info",
     "Get this mep instance's pid, working directory (== the active workspace's root), active project/workspace "
     "names, workspace_root, git branch, and list of open file paths -- useful for telling multiple running "
     "instances apart.",
     kNoInput, true},
    {"mep_state_dump", "state.dump",
     "Full editor state snapshot: every open buffer (id/filename/modified/line count), every tab's complete pane "
     "split-tree (layout, per-pane buffer/cursor/selection/scroll), and which tab/pane is active.",
     kNoInput, true},
    {"mep_poll_events", "",
     "Drain and return every editor event (cursor moved, buffer changed, pane focus changed, mode changed, a "
     "notification fired) queued since the last call to this tool -- this connection's own event backlog, not a "
     "live stream. Call it periodically, or right after taking an action, to see what happened in the editor "
     "(including the human user's own activity, not just this agent's own actions) since you last checked.",
     kNoInput, true},
    {"mep_screenshot", "ui.screenshot",
     "Capture mep's current window as a PNG and return its file path (read the file to see it). Takes no "
     "arguments. Coordinates in every other mep_mouse_*/mep_scroll tool are in this same pixel space.",
     kNoInput, true},
    {"mep_mouse_move", "ui.mouse_move", "Move the mouse pointer to (x, y) in mep's window (no click).",
     R"({"type":"object","properties":{"x":{"type":"integer"},"y":{"type":"integer"}},"required":["x","y"]})", false},
    {"mep_mouse_click", "ui.mouse_click",
     "Move to (x, y) and click a mouse button there. Use clicks:2 for a double-click. For a plain "
     "click-and-hold-a-modifier (e.g. Shift-click), call mep_key_down first, then this, then mep_key_up.",
     R"({"type":"object","properties":{"x":{"type":"integer"},"y":{"type":"integer"},"button":{"type":"string","enum":["left","middle","right"],"default":"left"},"clicks":{"type":"integer","minimum":1,"maximum":3,"default":1}},"required":["x","y"]})",
     false},
    {"mep_mouse_down", "ui.mouse_down",
     "Press (and hold) a mouse button at (x, y). Pair with mep_mouse_up -- use this instead of mep_mouse_click to "
     "drag by hand with your own mep_mouse_move calls in between.",
     R"({"type":"object","properties":{"x":{"type":"integer"},"y":{"type":"integer"},"button":{"type":"string","enum":["left","middle","right"],"default":"left"}},"required":["x","y"]})",
     false},
    {"mep_mouse_up", "ui.mouse_up", "Release a mouse button at (x, y). See mep_mouse_down.",
     R"({"type":"object","properties":{"x":{"type":"integer"},"y":{"type":"integer"},"button":{"type":"string","enum":["left","middle","right"],"default":"left"}},"required":["x","y"]})",
     false},
    {"mep_mouse_drag", "ui.mouse_drag",
     "Press a button at (x1, y1), move smoothly to (x2, y2) in `steps` increments, then release -- one call for a "
     "paint stroke, a slider drag, a selection drag, etc.",
     R"({"type":"object","properties":{"x1":{"type":"integer"},"y1":{"type":"integer"},"x2":{"type":"integer"},"y2":{"type":"integer"},"button":{"type":"string","enum":["left","middle","right"],"default":"left"},"steps":{"type":"integer","minimum":1,"maximum":200,"default":12}},"required":["x1","y1","x2","y2"]})",
     false},
    {"mep_scroll", "ui.scroll",
     "Scroll the mouse wheel at (x, y). Positive delta scrolls up, negative scrolls down; each unit is one wheel click.",
     R"({"type":"object","properties":{"x":{"type":"integer"},"y":{"type":"integer"},"delta":{"type":"integer"}},"required":["x","y","delta"]})",
     false},
    {"mep_key_press", "ui.key_press",
     "Press and release one key: a single character (\"e\", \"[\", \"?\") or an X11 keysym name for anything "
     "without one (\"Escape\", \"Return\", \"Tab\", \"BackSpace\", \"Left\"/\"Right\"/\"Up\"/\"Down\", "
     "\"F1\"..\"F12\", \"Control_L\", \"Shift_L\", \"Alt_L\"). For an uppercase letter or shifted symbol, either "
     "pass it directly (Shift is applied automatically) or wrap with mep_key_down(\"Shift_L\")/mep_key_up(\"Shift_L\") "
     "for a held modifier across other calls (e.g. a Ctrl-click).",
     R"({"type":"object","properties":{"key":{"type":"string"}},"required":["key"]})", false},
    {"mep_key_down", "ui.key_down",
     "Press and hold one key (see mep_key_press for name syntax) without releasing it -- for held modifiers "
     "(\"Shift_L\", \"Control_L\", \"Alt_L\") spanning other mep_mouse_*/mep_key_* calls. Pair with mep_key_up.",
     R"({"type":"object","properties":{"key":{"type":"string"}},"required":["key"]})", false},
    {"mep_key_up", "ui.key_up", "Release a key previously held with mep_key_down.",
     R"({"type":"object","properties":{"key":{"type":"string"}},"required":["key"]})", false},
    {"mep_type_text", "ui.type_text",
     "Type a string one keystroke at a time (printable ASCII only; auto-shifts uppercase letters and symbols). For "
     "most editing, mep_buffer_insert_text is far more direct -- reach for this only when you specifically need "
     "real keystrokes, e.g. exercising mep's own key handling or a text field with no buffer-level API.",
     R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"]})", false},

    // --- In-pane 3D modeler (MODEL3D.md) -- unlike the raster image editor
    // (UI-automation only, no structured API), this gives you a real
    // scene-authoring surface: build/inspect/transform a scene with these
    // tools alone, no mep_mouse_*/mep_screenshot needed. Either open a
    // model file with mep_file_open (.obj/.gltf/.glb/.iqm/.vox/.m3d/.blend
    // -- lands directly in the 3D modeler) or start from nothing with
    // mep_model_new, then use `buffer_id` from mep_state_dump/
    // mep_session_info/mep_model_new's own return value. See
    // MEP_AGENT_API.md's "in-pane 3D modeler" section for a worked example.
    {"mep_model_new", "model.new",
     "Create a fresh, empty 3D-modeler scene (no source file needed) and switch to it -- the "
     "\"build from scratch\" entry point. Returns the new buffer's id.",
     kNoInput, false},
    {"mep_model_list_objects", "model.listObjects",
     "List every object in a 3D-modeler scene: id, name, visibility, position/rotation/scale, base color, and "
     "triangle count.",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"}},"required":["buffer_id"]})", true},
    {"mep_model_scene_stats", "model.sceneStats", "Get a 3D-modeler scene's object count and total triangle count.",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"}},"required":["buffer_id"]})", true},
    {"mep_model_primitive_info", "model.primitiveInfo",
     "Look up a primitive kind's pivot point and default dimensions (e.g. cylinder/cone are base-pivoted and extend "
     "+Y, not centered) -- pass kind for just that one, or omit it to get every kind at once. No buffer_id needed, "
     "this is static reference data, not scene state. Use this before stacking parts instead of having to already "
     "know or go re-read MEP_AGENT_API.md's pivot table.",
     R"({"type":"object","properties":{"kind":{"type":"string","enum":["cube","sphere","cylinder","cone","plane","torus","wedge"]}}})",
     true},
    {"mep_model_add_primitive", "model.addPrimitive",
     "Add a procedurally generated primitive object (cube/sphere/cylinder/cone/plane/torus/wedge) to a 3D-modeler "
     "scene, optionally setting its initial transform. Returns the new object's id.",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"kind":{"type":"string","enum":["cube","sphere","cylinder","cone","plane","torus","wedge"]},"transform":{"type":"object","description":"optional position/rotation/scale, each an optional {x,y,z}; rotation in degrees","properties":{"position":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},"rotation":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},"scale":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}}}}},"required":["buffer_id","kind"]})",
     false},
    {"mep_model_delete_object", "model.deleteObject",
     "Delete an object from a 3D-modeler scene. cascade (default false) also deletes every transitive "
     "descendant (Scene::Descendants) instead of just un-parenting them -- a real 'delete this group "
     "and everything in it.'",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"object_id":{"type":"integer"},"cascade":{"type":"boolean"}},"required":["buffer_id","object_id"]})",
     false},
    {"mep_model_duplicate_object", "model.duplicateObject",
     "Duplicate an object (same mesh, transform, and material) in a 3D-modeler scene. Returns the new "
     "object's id. cascade (default false) also duplicates every transitive descendant, re-parented to "
     "mirror the original hierarchy under the new copy -- a real 'duplicate this group and everything "
     "in it,' rather than just the one top-level node (whose children would otherwise still point at "
     "the original).",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"object_id":{"type":"integer"},"cascade":{"type":"boolean"}},"required":["buffer_id","object_id"]})",
     false},
    {"mep_model_set_transform", "model.setTransform",
     "Set an object's position/rotation/scale in a 3D-modeler scene -- each of the three is optional, only the "
     "ones given are changed.",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"object_id":{"type":"integer"},"position":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},"rotation":{"type":"object","description":"Euler XYZ, degrees","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},"scale":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}}},"required":["buffer_id","object_id"]})",
     false},
    {"mep_model_set_material", "model.setMaterial", "Set an object's base color (0..1 floats; a defaults to 1.0) in a 3D-modeler scene.",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"object_id":{"type":"integer"},"color":{"type":"object","properties":{"r":{"type":"number"},"g":{"type":"number"},"b":{"type":"number"},"a":{"type":"number"}},"required":["r","g","b"]}},"required":["buffer_id","object_id","color"]})",
     false},
    {"mep_model_set_texture", "model.setTexture",
     "Set (or, with an empty path, clear) an object's base-color/albedo texture in a 3D-modeler "
     "scene, loaded from an image file (PNG/JPG/BMP/...). The texture is sampled and then tinted by "
     "the object's own mep_model_set_material color, same as glTF's baseColorTexture + "
     "baseColorFactor. No normal/metallic-roughness/emissive maps -- this is base color only.",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"object_id":{"type":"integer"},"path":{"type":"string","description":"image file path, or empty string to clear"}},"required":["buffer_id","object_id","path"]})",
     false},
    {"mep_model_rename_object", "model.renameObject", "Rename an object in a 3D-modeler scene.",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"object_id":{"type":"integer"},"name":{"type":"string"}},"required":["buffer_id","object_id","name"]})",
     false},
    {"mep_model_set_visible", "model.setVisible", "Show or hide an object in a 3D-modeler scene (kept, not deleted).",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"object_id":{"type":"integer"},"visible":{"type":"boolean"}},"required":["buffer_id","object_id","visible"]})",
     false},
    {"mep_model_select", "model.select",
     "Replace the current selection in a 3D-modeler scene (silently drops any object_id that doesn't exist). Not "
     "an undoable edit.",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"object_ids":{"type":"array","items":{"type":"integer"}}},"required":["buffer_id","object_ids"]})",
     false},
    {"mep_model_get_selection", "model.getSelection", "Get the currently selected object ids in a 3D-modeler scene.",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"}},"required":["buffer_id"]})", true},
    {"mep_model_camera_set", "model.cameraSet",
     "Update a 3D-modeler pane's orbit camera (target/yaw/pitch/distance/fov) -- each field is optional, only the "
     "ones given are changed. Not an undoable edit.",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"target":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},"yaw":{"type":"number","description":"degrees"},"pitch":{"type":"number","description":"degrees, clamped to [-89,89]"},"distance":{"type":"number"},"fov":{"type":"number","description":"degrees"}},"required":["buffer_id"]})",
     false},
    {"mep_model_camera_get", "model.cameraGet", "Get a 3D-modeler pane's current orbit camera state.",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"}},"required":["buffer_id"]})", true},
    {"mep_model_undo", "model.undo", "Undo the last edit in a 3D-modeler scene.",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"}},"required":["buffer_id"]})", false},
    {"mep_model_redo", "model.redo", "Redo the last undone edit in a 3D-modeler scene.",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"}},"required":["buffer_id"]})", false},
    {"mep_model_render_to_image", "model.renderToImage",
     "Render a 3D-modeler scene's viewport to a PNG file -- a clean render (no selection outline, no "
     "menubar/sidebars) at whatever resolution you ask for, unlike mep_screenshot which always captures the "
     "whole mep window. Needs the real GUI window (same requirement as mep_screenshot/mep_mouse_*).",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"path":{"type":"string","description":"destination PNG path"},"width":{"type":"integer","description":"default 1024, clamped to [16,4096]"},"height":{"type":"integer","description":"default 768, clamped to [16,4096]"},"transparent":{"type":"boolean","description":"clear to a transparent background instead of the theme background; default false"},"show_grid":{"type":"boolean","description":"default: the pane's own current grid setting"},"wireframe":{"type":"boolean","description":"default: the pane's own current wireframe setting"}},"required":["buffer_id","path"]})",
     true},
    {"mep_model_set_view", "model.setView",
     "Set a 3D-modeler pane's grid/wireframe/snap view toggles -- each field optional, only the ones given are "
     "changed. Not an undoable edit. The same thing the tool sidebar's Grid/Wireframe/Snap buttons do, exposed for "
     "scripting. snap makes subsequent Move/Rotate/Scale gizmo and free drags round to a fixed grid step (0.25 "
     "units), angle step (15 degrees), or scale step (0.25); it does not affect mep_model_set_transform, which "
     "always sets the exact value given.",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"show_grid":{"type":"boolean"},"wireframe":{"type":"boolean"},"snap":{"type":"boolean"}},"required":["buffer_id"]})",
     false},
    {"mep_model_frame_all", "model.frameAll",
     "Reframe a 3D-modeler pane's orbit camera (target + distance) to fit the whole scene's true world bounds "
     "-- yaw/pitch are left as they are. Not an undoable edit.",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"}},"required":["buffer_id"]})", false},
    {"mep_model_set_transforms", "model.setTransforms",
     "Set position/rotation/scale on many objects in one call (each entry's fields are independently "
     "optional, only given ones are changed) -- for repositioning several objects without one round-trip "
     "per object. Returns how many updates were actually applied.",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"updates":{"type":"array","items":{"type":"object","properties":{"object_id":{"type":"integer"},"position":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},"rotation":{"type":"object","description":"Euler XYZ, degrees","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},"scale":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}}},"required":["object_id"]}}},"required":["buffer_id","updates"]})",
     false},
    {"mep_model_set_materials", "model.setMaterials",
     "Set base color on many objects in one call. Returns how many updates were actually applied.",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"updates":{"type":"array","items":{"type":"object","properties":{"object_id":{"type":"integer"},"color":{"type":"object","properties":{"r":{"type":"number"},"g":{"type":"number"},"b":{"type":"number"},"a":{"type":"number"}},"required":["r","g","b"]}},"required":["object_id","color"]}}},"required":["buffer_id","updates"]})",
     false},
    {"mep_model_delete_objects", "model.deleteObjects",
     "Delete many objects from a 3D-modeler scene in one call. Returns how many were actually deleted. "
     "Optional cascade (default false, applied to every object_id) also deletes each one's whole "
     "descendant subtree, same as mep_model_delete_object's own cascade flag.",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"object_ids":{"type":"array","items":{"type":"integer"}},"cascade":{"type":"boolean"}},"required":["buffer_id","object_ids"]})",
     false},
    {"mep_model_duplicate_mirrored", "model.duplicateMirrored",
     "Duplicate an object with its position mirrored across the given world axis through the origin, "
     "reflecting the copy's own rotation to match (exact for a simple single-axis rotation -- a compound "
     "rotation may need a manual touch-up afterward). Returns the new object's id.",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"object_id":{"type":"integer"},"axis":{"type":"string","enum":["x","y","z"]}},"required":["buffer_id","object_id","axis"]})",
     false},
    {"mep_model_radial_array", "model.radialArray",
     "Duplicate an object count-1 times, evenly spaced in a ring around the given axis through the origin -- "
     "e.g. a fin offset on X, arrayed 4x around Y, lands one at each 90-degree step, each still facing "
     "outward the way the original did. The original object is left as-is and not counted. Returns the new "
     "objects' ids, in order.",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"object_id":{"type":"integer"},"count":{"type":"integer","description":"total copies including the original -- this many minus one new objects are created"},"axis":{"type":"string","enum":["x","y","z"]}},"required":["buffer_id","object_id","count","axis"]})",
     false},
    {"mep_model_group_objects", "model.groupObjects",
     "Create a new empty group node (no mesh, invisible in the viewport, positioned at the centroid of "
     "the grouped objects) and parent every object in object_ids under it. Object3D.parent is purely an "
     "organizational/group-move link -- it is never composed into a child's own transform, so grouping "
     "does not move or change how anything renders. Returns the new group's object id.",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"object_ids":{"type":"array","items":{"type":"integer"}}},"required":["buffer_id","object_ids"]})",
     false},
    {"mep_model_set_parent", "model.setParent",
     "Set (or clear) one object's parent, for Outliner grouping/nesting and Move-tool group-drag "
     "cascading. Fails (ok: false) on a nonexistent object/parent, parent_id == object_id, or a "
     "parent_id that's already a descendant of object_id (would create a cycle).",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"object_id":{"type":"integer"},"parent_id":{"type":"integer","description":"-1 (or omit) to clear/un-parent"}},"required":["buffer_id","object_id"]})",
     false},
    {"mep_model_list_vertices", "model.listVertices",
     "List every vertex of an object's mesh: index, local-space (pre-object-transform) x/y/z, and (when "
     "the mesh has normals) nx/ny/nz. The first real vertex-level mesh-editing primitive -- combine "
     "with mep_model_set_vertex_position to nudge individual vertices instead of only whole-object "
     "transforms.",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"object_id":{"type":"integer"}},"required":["buffer_id","object_id"]})",
     true},
    {"mep_model_list_triangles", "model.listTriangles",
     "List every triangle of an object's mesh: index and the vertex-unit indices (a/b/c) of its 3 "
     "corners. Read-only mesh-connectivity introspection -- without this, there's no way to discover "
     "which vertices actually form a triangle together (the thing mep_model_subdivide_faces/"
     "extrude_faces/inset_faces/dissolve_vertex all key off of) besides positions alone. Combine with "
     "mep_model_list_vertices to find, e.g., which vertex triples share the same position (candidates "
     "for mep_model_merge_vertices) or which edges only appear in one triangle (a mesh boundary).",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"object_id":{"type":"integer"}},"required":["buffer_id","object_id"]})",
     true},
    {"mep_model_set_vertex_position", "model.setVertexPosition",
     "Set one vertex's local-space (pre-object-transform) position on an object's mesh. If the "
     "object currently shares its mesh with another object (a radial array, a mirrored duplicate, a "
     "multi-object import), it's transparently given its own private copy first, so this never "
     "deforms other objects. vertex_index is in vertex units (from mep_model_list_vertices or "
     "mep_model_list_objects' own vertex_count), not a raw float offset.",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"object_id":{"type":"integer"},"vertex_index":{"type":"integer"},"position":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}},"required":["x","y","z"]}},"required":["buffer_id","object_id","vertex_index","position"]})",
     false},
    {"mep_model_delete_vertices", "model.deleteVertices",
     "Delete the given vertices (vertex-units indices) and every triangle referencing any of them "
     "from an object's mesh -- leaves a hole rather than retriangulating/filling it, and does not "
     "attempt to reconnect the surrounding geometry. Safely clones a shared mesh first, same as "
     "mep_model_set_vertex_position. Out-of-range/duplicate indices are harmless no-ops.",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"object_id":{"type":"integer"},"vertex_indices":{"type":"array","items":{"type":"integer"}}},"required":["buffer_id","object_id","vertex_indices"]})",
     false},
    {"mep_model_merge_vertices", "model.mergeVertices",
     "Weld the given vertices (vertex-units indices) of an object's mesh into a single vertex at their "
     "averaged position/normal/texcoord -- any triangle that becomes degenerate as a result (two or "
     "more of its corners now the same vertex) is dropped rather than kept as zero-area. Safely clones "
     "a shared mesh first, same as mep_model_set_vertex_position. Out-of-range/duplicate indices are "
     "harmless no-ops; fewer than 2 distinct valid indices is a no-op (nothing to merge).",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"object_id":{"type":"integer"},"vertex_indices":{"type":"array","items":{"type":"integer"}}},"required":["buffer_id","object_id","vertex_indices"]})",
     false},
    {"mep_model_recalculate_normals", "model.recalculateNormals",
     "Recompute an object's mesh's per-vertex normals from its current triangle geometry (smooth -- "
     "each vertex's normal is the normalized, area-weighted average of every adjacent triangle's own "
     "normal). Has zero visible effect on this app's own rendering (its default shaders never read "
     "vertex normals) -- use it to fix up normals left stale by mep_model_set_vertex_position/"
     "delete_vertices/merge_vertices before exporting for a tool that does read them, e.g. Blender or a "
     "glTF viewer with real lighting. Safely clones a shared mesh first, same as "
     "mep_model_set_vertex_position.",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"object_id":{"type":"integer"}},"required":["buffer_id","object_id"]})",
     false},
    {"mep_model_subdivide_faces", "model.subdivideFaces",
     "Centroid-subdivide every triangle of an object's mesh whose all 3 corners are in vertex_indices -- "
     "this app's stand-in for real face-selection tooling (same convention as merge/extrude): a 'face' "
     "here just means whichever triangles are fully covered by the given vertex set. Each such triangle "
     "gets one new vertex at its centroid and is replaced by 3 new triangles fanning out to it; a "
     "triangle with fewer than all 3 corners selected is left untouched. Safely clones a shared mesh "
     "first, same as mep_model_set_vertex_position. Returns the newly-created centroid vertex indices "
     "(empty if no triangle was fully covered).",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"object_id":{"type":"integer"},"vertex_indices":{"type":"array","items":{"type":"integer"}}},"required":["buffer_id","object_id","vertex_indices"]})",
     false},
    {"mep_model_extrude_faces", "model.extrudeFaces",
     "Extrude the face formed by every triangle of an object's mesh whose all 3 corners are in "
     "vertex_indices (same 'face' convention as mep_model_subdivide_faces), by distance along that "
     "face's own geometrically-derived normal -- not read from stored per-vertex normals, which may be "
     "stale or absent. Every vertex used by a selected triangle is duplicated and offset; the selected "
     "triangles are re-pointed at the duplicates (lifting the cap into place) while a wall quad connects "
     "the untouched original ring to the new one along each *boundary* edge of the selected group (an "
     "edge shared between two selected triangles, e.g. a face's own diagonal, is correctly left un-"
     "walled). A vertex also used by a triangle outside the selection (e.g. a cube's adjacent side face "
     "sharing a top corner) naturally stays attached there too. Extrusion follows actual mesh "
     "*connectivity*, not spatial adjacency -- a raylib-generated primitive's unwelded per-face vertices "
     "(see mep_model_merge_vertices) mean selecting an entire such mesh extrudes each of its faces "
     "independently; weld first if that's not wanted. Safely clones a shared mesh first, same as "
     "mep_model_set_vertex_position. Returns the new cap vertex indices (empty if no triangle was fully "
     "covered, or the selection was degenerate/zero-area) so the caller can immediately continue editing "
     "the just-extruded face.",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"object_id":{"type":"integer"},"vertex_indices":{"type":"array","items":{"type":"integer"}},"distance":{"type":"number"}},"required":["buffer_id","object_id","vertex_indices","distance"]})",
     false},
    {"mep_model_dissolve_vertex", "model.dissolveVertex",
     "Remove one vertex from an object's mesh and patch the surrounding faces back together, unlike "
     "mep_model_delete_vertices' own deliberately-left hole. Only works cleanly on a proper interior "
     "vertex whose incident triangles form a single closed fan around it; when they don't (a mesh-"
     "boundary vertex, a non-manifold fan, or an isolated vertex with no incident triangles) this "
     "silently falls back to the same hole-leaving removal mep_model_delete_vertices does, rather than "
     "risk a malformed retriangulation -- there's no way to tell from the response which path was taken "
     "besides comparing triangle counts before/after. The retriangulation is a simple fan (not a "
     "'nicest possible' one), so a very non-convex surrounding ring can produce a visibly thin sliver "
     "triangle or two. Safely clones a shared mesh first, same as mep_model_set_vertex_position.",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"object_id":{"type":"integer"},"vertex_index":{"type":"integer"}},"required":["buffer_id","object_id","vertex_index"]})",
     false},
    {"mep_model_inset_faces", "model.insetFaces",
     "Inset the face formed by every triangle of an object's mesh whose all 3 corners are in "
     "vertex_indices (same 'face' convention as mep_model_subdivide_faces/mep_model_extrude_faces): "
     "every vertex the face uses is duplicated and moved toward the face group's own centroid (the "
     "average position of every vertex it uses) by amount, a 0..1 fraction (clamped) -- 0 is a "
     "degenerate zero-width inset, 1 fully collapses the new cap onto the centroid. The selected "
     "triangles are re-pointed at the duplicates (shrinking the cap in place, no lift along any normal, "
     "unlike mep_model_extrude_faces), and a wall quad connects the untouched original ring to the new "
     "shrunk one along each boundary edge of the group (an edge shared between two selected triangles, "
     "e.g. a face's own diagonal, is correctly left un-walled). Safely clones a shared mesh first, same "
     "as mep_model_set_vertex_position. Returns the new cap vertex indices (empty if no triangle was "
     "fully covered) -- chain straight into mep_model_extrude_faces on the returned indices for a "
     "raised-platform-with-border look.",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"object_id":{"type":"integer"},"vertex_indices":{"type":"array","items":{"type":"integer"}},"amount":{"type":"number"}},"required":["buffer_id","object_id","vertex_indices","amount"]})",
     false},
    {"mep_model_add_vertex", "model.addVertex",
     "Append one new, isolated vertex to an object's mesh at the given local-space (pre-object-"
     "transform) position -- no triangle references it, so it won't render until connected via "
     "mep_model_make_face or similar. The deliberate counterpart to subdivide/extrude/inset (which all "
     "work on existing triangles): this is how to build genuinely new geometry from scratch. Safely "
     "clones a shared mesh first, same as mep_model_set_vertex_position. Returns the new vertex's index "
     "(vertex units), or -1 if the object doesn't exist or has no mesh.",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"object_id":{"type":"integer"},"position":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}},"required":["x","y","z"]}},"required":["buffer_id","object_id","position"]})",
     false},
    {"mep_model_make_face", "model.makeFace",
     "Create new triangle(s) of an object's mesh connecting existing vertices -- fan-triangulated from "
     "the first of vertex_indices (3 vertices become 1 new triangle, 4 become 2, a pentagon 3), "
     "Blender's own 'Make Edge/Face' (F key) in spirit. Unlike mep_model_subdivide_faces/extrude_faces/"
     "inset_faces, the given vertices don't need to already form a triangle -- this is how to connect "
     "vertices (including ones just added via mep_model_add_vertex) that aren't adjacent yet. No new "
     "vertices are created, and no check is made for whether the resulting triangle(s) duplicate or "
     "overlap ones that already exist; winding (and so which side ends up 'front') follows the given "
     "vertex order. Safely clones a shared mesh first, same as mep_model_set_vertex_position. Returns "
     "false if fewer than 3 distinct valid vertices remain after filtering duplicates/out-of-range "
     "entries.",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"object_id":{"type":"integer"},"vertex_indices":{"type":"array","items":{"type":"integer"}}},"required":["buffer_id","object_id","vertex_indices"]})",
     false},
    {"mep_model_merge_by_distance", "model.mergeByDistance",
     "Automatically weld every group of an object's mesh's vertices whose positions are all mutually "
     "within threshold of each other -- Blender's own 'Merge by Distance'/'Remove Doubles', and the "
     "'just fix all of them' counterpart to mep_model_merge_vertices (which needs the caller to already "
     "know which indices are duplicates). Especially useful right after importing/generating a "
     "primitive, since raylib's own generated meshes emit unwelded duplicate vertices at every shared "
     "corner. threshold=0 welds only exact (bit-identical) position duplicates; grouping is transitive "
     "through a chain of close-enough pairs. Each group is welded to its averaged position/normal/"
     "texcoord, and a triangle that becomes degenerate as a result is dropped, same as "
     "mep_model_merge_vertices. Safely clones a shared mesh first, same as "
     "mep_model_set_vertex_position. Returns how many vertices were removed (0 if nothing was within "
     "threshold of anything else).",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"object_id":{"type":"integer"},"threshold":{"type":"number"}},"required":["buffer_id","object_id","threshold"]})",
     false},
    {"mep_model_flip_normals", "model.flipNormals",
     "Reverse every triangle's winding and negate every vertex normal of an object's mesh, flipping "
     "which side renders as 'front' -- the fix for geometry that came out inside-out, e.g. a "
     "mep_model_make_face call given vertices in the wrong order, or some imported files. Operates on "
     "the whole mesh, not a selection. Safely clones a shared mesh first, same as "
     "mep_model_set_vertex_position.",
     R"({"type":"object","properties":{"buffer_id":{"type":"integer"},"object_id":{"type":"integer"}},"required":["buffer_id","object_id"]})",
     false},
};

const ToolSpec *FindTool(const std::string &name) {
    for (const auto &spec : kTools) {
        if (name == spec.name) return &spec;
    }
    return nullptr;
}

// --- MCP request handling -----------------------------------------------

Json ToolResult(const Json &value) {
    Json content = Json::Array();
    Json block = Json::Object();
    block["type"] = "text";
    block["text"] = value.dump();
    content.push_back(block);
    Json out = Json::Object();
    out["content"] = content;
    return out;
}

Json ToolErrorResult(const std::string &message) {
    Json content = Json::Array();
    Json block = Json::Object();
    block["type"] = "text";
    block["text"] = message;
    content.push_back(block);
    Json out = Json::Object();
    out["content"] = content;
    out["isError"] = true;
    return out;
}

// Mirrors server.ts's own module-scope `lastKnownStatus` and callTool():
// auto-nudges the tab-bar status badge to "thinking" the moment any
// non-read-only tool runs right after a stale "done"/unset status, and
// tracks buffer.insertText/setLine/replaceLines as an implicit "writing"
// the same way agent_rpc.cpp's own Connection::status auto-transition
// does -- see server.ts's own comment for the full "reported bug" story
// behind why this exists.
std::string g_last_status;

Json HandleToolsCall(const std::string &name, const Json &arguments) {
    if (name == "mep_poll_events") return ToolResult(g_mep.DrainEvents());

    const ToolSpec *spec = FindTool(name);
    if (!spec) return ToolErrorResult("unknown tool: " + name);

    const bool is_stale = g_last_status == "done" || g_last_status.empty() || g_last_status == "idle";
    if (std::string(spec->rpc_method) != "session.setStatus" && !spec->read_only && is_stale) {
        try {
            Json params = Json::Object();
            params["status"] = "thinking";
            g_mep.Call("session.setStatus", params);
            g_last_status = "thinking";
        } catch (const std::exception &) {
            // Best-effort -- if mep is unreachable the real call below fails too
            // and surfaces that error to the model normally.
        }
    }
    try {
        Json result = g_mep.Call(spec->rpc_method, arguments);
        const std::string method = spec->rpc_method;
        if (method == "session.setStatus") {
            if (arguments.contains("status")) g_last_status = arguments.get("status").as_string();
        } else if (method == "buffer.insertText" || method == "buffer.setLine" || method == "buffer.replaceLines") {
            g_last_status = "writing";
        }
        return ToolResult(result);
    } catch (const std::exception &ex) {
        return ToolErrorResult(ex.what());
    }
}

Json ToolSpecToJson(const ToolSpec &spec) {
    Json schema;
    if (!Json::Parse(spec.input_schema, &schema)) Json::Parse(kNoInput, &schema);  // shouldn't happen -- every literal above is valid JSON
    Json out = Json::Object();
    out["name"] = spec.name;
    out["description"] = spec.description;
    out["inputSchema"] = schema;
    return out;
}

// Writes one newline-delimited JSON-RPC message to stdout and flushes --
// MCP's stdio transport frames messages by line, so nothing else in this
// process may ever write to stdout (stray output would look like a
// malformed extra message to the client); diagnostics go to stderr only.
void WriteMessage(const Json &msg) {
    std::cout << msg.dump() << "\n";
    std::cout.flush();
}

void HandleMessage(const Json &req) {
    const std::string method = req.get("method").as_string();
    const bool has_id = req.contains("id");

    if (method == "notifications/initialized" || method == "notifications/cancelled") return;  // nothing to do, no reply

    Json result;
    bool is_error = false;
    int error_code = -32601;
    std::string error_message = "method not found: " + method;

    if (method == "initialize") {
        Json capabilities = Json::Object();
        capabilities["tools"] = Json::Object();
        Json server_info = Json::Object();
        server_info["name"] = "mep-agent";
        server_info["version"] = "0.1.0";
        result = Json::Object();
        // Echo the client's requested version back rather than asserting a
        // fixed one of our own -- this bridge speaks a small enough subset
        // of MCP (tools only, no resources/prompts/sampling) that it has no
        // version-specific behavior to negotiate.
        result["protocolVersion"] = req.get("params").get("protocolVersion").as_string("2024-11-05");
        result["capabilities"] = capabilities;
        result["serverInfo"] = server_info;
        is_error = false;
    } else if (method == "ping") {
        result = Json::Object();
    } else if (method == "tools/list") {
        Json tools = Json::Array();
        for (const auto &spec : kTools) tools.push_back(ToolSpecToJson(spec));
        result = Json::Object();
        result["tools"] = tools;
    } else if (method == "tools/call") {
        const Json &params = req.get("params");
        const std::string name = params.get("name").as_string();
        const Json arguments = params.contains("arguments") ? params.get("arguments") : Json::Object();
        result = HandleToolsCall(name, arguments);
    } else {
        is_error = true;
    }

    if (!has_id) return;  // a notification we don't otherwise recognize -- no reply, ever

    Json response = Json::Object();
    response["jsonrpc"] = "2.0";
    response["id"] = req.get("id");
    if (is_error) {
        Json err = Json::Object();
        err["code"] = error_code;
        err["message"] = error_message;
        response["error"] = err;
    } else {
        response["result"] = result;
    }
    WriteMessage(response);
}

// Best-effort self-identification at startup, mirroring server.ts's own
// top-level try/catch: a failure here (mep not running yet, ambiguous
// socket) is logged to stderr but never fatal -- the first real tool
// call surfaces the same error to the model clearly enough on its own.
void IdentifyAtStartup() {
    try {
        Json params = Json::Object();
        const char *name_env = std::getenv("MEP_AGENT_NAME");
        params["name"] = name_env ? std::string(name_env) : std::string("Claude");
        if (const char *term_env = std::getenv("MEP_TERMINAL_BUFFER")) {
            char *end = nullptr;
            long value = std::strtol(term_env, &end, 10);
            if (end != term_env && *end == '\0' && value >= 0) params["terminal_buffer_id"] = static_cast<int>(value);
        }
        g_mep.Call("session.identify", params);
    } catch (const std::exception &ex) {
        std::cerr << "mep-agent: session.identify failed at startup: " << ex.what() << "\n";
    }
}

}  // namespace

int main() {
    IdentifyAtStartup();
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        Json request;
        if (!Json::Parse(line, &request)) continue;  // malformed -- nothing sane to reply with, since we may not even have an id
        HandleMessage(request);
    }
    return 0;
}
