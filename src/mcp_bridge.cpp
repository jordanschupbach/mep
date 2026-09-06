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

    bool WriteAll(const std::string &data) {
        size_t off = 0;
        while (off < data.size()) {
            ssize_t n = write(fd_, data.data() + off, data.size() - off);
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
