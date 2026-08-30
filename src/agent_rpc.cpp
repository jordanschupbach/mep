#include "agent_rpc.h"
#include "editor.h"

#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)

#include "json.h"
#include "persist.h"
#include "rpc_framing.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace mep::agent {
namespace {

// --- Connection / transport --------------------------------------------
// Defined before "Method dispatch" below: Dispatch() takes a Connection&
// (COLLAB_CURSORS_PLAN.md's per-connection identity/cursor) and needs the
// complete type, not just a forward declaration.

void CloseFd(int fd) {
    if (fd >= 0) close(fd);
}

// One connected client. The reader thread owns `read_buffer` and only
// ever *parses* bytes into `pending` (guarded by `mutex`) -- it never
// touches Editor. `Send` (a blocking socket write, bounded by the
// SO_SNDTIMEO set at accept() time so one hung client can stall the main
// thread's PollOnce for at most that timeout rather than indefinitely) is
// only ever called from the main thread, so it needs no write-side lock.
struct Connection {
    int fd = -1;
    std::thread reader_thread;
    std::atomic<bool> closed{false};

    std::string read_buffer;
    std::mutex mutex;
    std::deque<Json> pending;

    // --- Participant identity + independent cursor (COLLAB_CURSORS_PLAN.md) ---
    // Assigned once at accept time; read/written only from the main
    // thread (inside Dispatch(), called from PollOnce), same as every
    // other main-thread-only field already documented in this codebase
    // (e.g. State's own event-tracking fields below) -- no locking needed.
    std::string participant_id;
    std::string display_name = "AI Agent";
    // -1 = not yet positioned; lazily initialized on this connection's
    // first cursor.get/set or buffer.insertText/setLine/replaceLines/
    // switch call, to the human's *current* active buffer/cursor as a
    // one-time starting snapshot -- fully independent of it after that.
    int cursor_buffer_id = -1;
    CursorPos cursor;
    // "" until either an explicit session.setStatus call or a buffer
    // mutation sets it (COLLAB_CURSORS_PLAN.md Phase 1g) -- "" renders no
    // status badge at all on this agent's tab-bar chip, same as an agent
    // that predates this feature. "thinking"/"awaiting_input"/"done" are
    // never inferred (mep has no way to observe an agent's own reasoning
    // or its conversation with the human elsewhere) and only ever change
    // via an explicit session.setStatus call; "writing" is the one status
    // set automatically, by every buffer.insertText/setLine/replaceLines
    // call, since that *is* directly observable here -- see Dispatch's
    // session.setStatus case and each buffer.* handler below.
    std::string status;

    ~Connection() {
        closed = true;
        if (fd >= 0) shutdown(fd, SHUT_RDWR);  // unblocks a thread parked in recv()
        if (reader_thread.joinable()) reader_thread.join();
        CloseFd(fd);
    }

    void ReaderLoop() {
        char chunk[4096];
        for (;;) {
            ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
            if (n <= 0) {
                closed = true;
                return;
            }
            read_buffer.append(chunk, static_cast<size_t>(n));
            bool ok = PumpRpcFrames(read_buffer, [this](const std::string &body) {
                Json msg;
                if (!Json::Parse(body, &msg) || !msg.is_object()) return;
                std::lock_guard<std::mutex> lock(mutex);
                pending.push_back(std::move(msg));
            });
            if (!ok) {
                // Unrecoverable framing violation (see rpc_framing.h) --
                // there's no way to find the next message boundary in a
                // byte-count-framed stream once sync is lost, so this
                // connection can't be trusted further. Drop it; a real
                // client can always reconnect.
                closed = true;
                return;
            }
        }
    }

    bool Send(const Json &message) {
        const std::string framed = FrameRpcMessage(message.dump());
        size_t offset = 0;
        while (offset < framed.size()) {
            ssize_t n = send(fd, framed.data() + offset, framed.size() - offset, MSG_NOSIGNAL);
            if (n <= 0) return false;
            offset += static_cast<size_t>(n);
        }
        return true;
    }
};

// --- Method dispatch -------------------------------------------------------

struct RpcError {
    int code;
    std::string message;
};

std::vector<std::string> JsonStringArray(const Json &value) {
    std::vector<std::string> out;
    if (!value.is_array()) return out;
    for (const Json &item : value.items()) out.push_back(item.as_string());
    return out;
}

Json CursorJson(int row, int col) {
    Json j = Json::Object();
    j["row"] = row;
    j["col"] = col;
    return j;
}

Json PaneJson(const Pane &pane) {
    Json j = Json::Object();
    j["id"] = pane.id;
    j["buffer_id"] = pane.buffer_id;
    j["cursor"] = CursorJson(pane.cursor.row, pane.cursor.col);
    j["visual_anchor"] = CursorJson(pane.visual_anchor.row, pane.visual_anchor.col);
    j["scroll_row"] = pane.scroll_row;
    return j;
}

Json SplitNodeJson(const SplitNode &node) {
    Json j = Json::Object();
    if (node.dir == SplitDir::Leaf) {
        j["dir"] = "leaf";
        j["pane"] = PaneJson(node.pane);
        return j;
    }
    j["dir"] = node.dir == SplitDir::Horizontal ? "horizontal" : "vertical";
    Json children = Json::Array();
    for (const auto &child : node.children) children.push_back(SplitNodeJson(*child));
    j["children"] = std::move(children);
    Json shares = Json::Array();
    for (float share : node.shares) shares.push_back(Json(static_cast<double>(share)));
    j["shares"] = std::move(shares);
    return j;
}

// Depth-first search for the leaf pane with `pane_id`, across every open
// tab -- Pane::id is stable across split-tree restructuring (its own doc
// comment in editor.h) and a client that only knows a pane_id (e.g. from
// an earlier pane.split/state.dump result) has no reason to also track
// which tab it lives in.
const Pane *FindPane(const SplitNode &node, int pane_id) {
    if (node.dir == SplitDir::Leaf) return node.pane.id == pane_id ? &node.pane : nullptr;
    for (const auto &child : node.children) {
        if (const Pane *found = FindPane(*child, pane_id)) return found;
    }
    return nullptr;
}

Json BufferSummaryJson(Editor &editor, int buffer_id) {
    const Buffer &buf = editor.GetBuffer(buffer_id);
    Json j = Json::Object();
    j["id"] = buffer_id;
    j["filename"] = buf.filename;
    j["modified"] = buf.modified;
    j["line_count"] = static_cast<int>(buf.lines.size());
    return j;
}

// COLLAB_CURSORS_PLAN.md Phase 1g -- lets a connected agent see who else
// is here (other agents, human :CollabJoin peers) and each agent's
// status badge, the same information main.cpp's tab-bar chip strip
// renders, without needing to look at the screen. `has_location` false
// means the human peer's collaboration hasn't reported a presence yet
// (an agent's own cursor always has a location once identified -- see
// EnsureConnCursorInitialized).
Json ParticipantJson(const Editor::ParticipantInfo &p) {
    Json j = Json::Object();
    j["id"] = p.id;
    j["name"] = p.name;
    j["kind"] = p.kind == Editor::ParticipantKind::Agent ? "agent" : "human";
    j["buffer_id"] = p.buffer_id;
    j["has_location"] = p.has_location;
    if (p.has_location) j["cursor"] = CursorJson(p.row, p.col);
    j["status"] = p.status;  // "" for a human peer, or an agent that hasn't reported one
    return j;
}

// Best-effort discovery of "the project's readme" -- a never-yet-
// positioned agent's default cursor location (COLLAB_CURSORS_PLAN.md).
// Case-insensitive match on a regular file whose name starts with
// "readme" in the current working directory (covers README, README.md,
// README.org, readme.txt, Readme.rst, ...). Returns "" (caller falls
// back to something else) if none is found or the directory can't be
// listed.
std::string FindReadmePath() {
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(std::filesystem::current_path(), ec)) {
        if (ec || !entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        std::string lower_name = name;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), [](unsigned char c) { return std::tolower(c); });
        // Bare, ORIGINAL-case filename, not entry.path().string() and not
        // the lowercased copy used only for the case-insensitive prefix
        // check -- FindOrCreateBuffer dedups by exact Buffer::filename
        // string equality, and a buffer opened via `mep README.org` or
        // `:e README.org` stores exactly that relative, original-case
        // name. Two earlier bugs here each silently created a second,
        // empty-looking duplicate buffer instead of matching the already-
        // open one: returning the absolute cwd-joined path, then
        // returning the lowercased comparison copy instead of `name`.
        // Caught by screenshotting a freshly identified agent's cursor,
        // finding no marker anywhere in the visible (buffer 0) pane, and
        // cross-checking with buffer.list.
        if (lower_name.rfind("readme", 0) == 0) return name;
    }
    return "";
}

// Seeds a connection's own independent cursor (COLLAB_CURSORS_PLAN.md)
// the first time it's needed -- either right after session.identify (so
// a newly registered agent has a real, visible cursor immediately, not
// only once it happens to make its first edit) or lazily on first use by
// a participant-scoped method for a connection that skipped identify.
// Idempotent (a no-op once already seeded), so a real edit's position is
// never clobbered by a later call: "no edits yet this session" -> the
// project's readme, row 0, col 0 (found via FindReadmePath, without
// switching any pane to it -- FindOrCreateBufferForLua only resolves a
// buffer_id); readme not found -> falls back to a one-time snapshot of
// the human's current buffer/cursor, same as this function's own
// original (pre-this-feature) behavior. Once any real cursor.set/
// buffer.insertText/etc. call happens, that position sticks -- "last
// known edit location" for the rest of this connection's life.
void EnsureConnCursorInitialized(Editor &editor, Connection &conn) {
    if (conn.cursor_buffer_id >= 0) return;
    const std::string readme_path = FindReadmePath();
    if (!readme_path.empty()) {
        conn.cursor_buffer_id = editor.FindOrCreateBufferForLua(readme_path);
        conn.cursor = {0, 0};
        return;
    }
    conn.cursor_buffer_id = editor.CurrentBufferId();
    conn.cursor = editor.Cursor();
}

// Every RPC method is a thin wrapper around an Editor method that already
// exists for the embedded-Lua `mep.*` API (SetCursorForLua, RunCommand,
// ...) -- see agent_rpc.h. Throws RpcError for an unknown method;
// individual handlers are deliberately as lenient about malformed/missing
// params as Json::as_int/as_string themselves are (a missing field reads
// as 0/"" rather than erroring), matching how the rest of this codebase's
// Lua-facing API already treats its arguments. session.setStatus is the
// one exception to "wraps an Editor method" -- there's no editor-side
// concept of an agent's own status, it's purely Connection-local state
// (COLLAB_CURSORS_PLAN.md Phase 1g), surfaced to rendering only via
// AgentParticipant::status.
//
// cursor.get/set and buffer.insertText/setLine/replaceLines/switch are
// *participant-scoped*: they read/write `conn`'s own independent cursor
// (see EnsureConnCursorInitialized above and Connection's own field
// comments), not the human's real Pane::cursor -- COLLAB_CURSORS_PLAN.md
// Phase 1b. Before that change these all acted on the human's real,
// shared cursor: an agent calling buffer.insertText would yank the
// human's actual on-screen cursor and type into whatever buffer they had
// open. Everything else here (buffer.setLines/create/filename/list/
// getLines, file.*, pane.*, command.run, session.info, state.dump)
// remains global/real-editor-scoped -- these are "control the editor"
// actions, not "type as this participant" ones.
Json Dispatch(Editor &editor, Connection &conn, const std::string &method, const Json &params) {
    if (method == "cursor.get") {
        EnsureConnCursorInitialized(editor, conn);
        Json j = CursorJson(conn.cursor.row, conn.cursor.col);
        j["buffer_id"] = conn.cursor_buffer_id;
        return j;
    }
    if (method == "cursor.set") {
        EnsureConnCursorInitialized(editor, conn);
        if (params.contains("buffer_id")) conn.cursor_buffer_id = params.get("buffer_id").as_int();
        conn.cursor = editor.ClampPositionInBuffer(conn.cursor_buffer_id, {params.get("row").as_int(), params.get("col").as_int()});
        return Json::Object();
    }
    if (method == "buffer.insertText") {
        EnsureConnCursorInitialized(editor, conn);
        conn.cursor = editor.InsertTextAt(conn.cursor_buffer_id, conn.cursor, params.get("text").as_string());
        conn.status = "writing";
        return Json::Object();
    }
    if (method == "buffer.setLine") {
        EnsureConnCursorInitialized(editor, conn);
        const int row = params.contains("row") ? params.get("row").as_int() : conn.cursor.row;
        editor.SetLineAt(conn.cursor_buffer_id, row, params.get("text").as_string());
        conn.status = "writing";
        return Json::Object();
    }
    if (method == "buffer.replaceLines") {
        EnsureConnCursorInitialized(editor, conn);
        editor.ReplaceLinesAt(conn.cursor_buffer_id, params.get("start").as_int(), params.get("end").as_int(), JsonStringArray(params.get("lines")));
        conn.status = "writing";
        return Json::Object();
    }
    if (method == "buffer.setLines") {
        editor.SetBufferLinesForLua(params.get("buffer_id").as_int(-1), JsonStringArray(params.get("lines")));
        return Json::Object();
    }
    if (method == "buffer.switch") {
        EnsureConnCursorInitialized(editor, conn);
        const int buffer_id = params.get("buffer_id").as_int(-1);
        if (buffer_id < 0 || buffer_id >= editor.BufferCountForLua()) throw RpcError{-32602, "unknown buffer_id"};
        conn.cursor_buffer_id = buffer_id;
        conn.cursor = {0, 0};
        return Json::Object();
    }
    if (method == "session.identify") {
        const std::string name = params.get("name").as_string();
        if (!name.empty()) conn.display_name = name;
        // A registered (identified) agent should have a real, visible
        // cursor right away, not only once it happens to make its first
        // edit -- see EnsureConnCursorInitialized's own comment for the
        // readme-default/idempotent-once-real-edits-happen behavior.
        EnsureConnCursorInitialized(editor, conn);
        Json j = Json::Object();
        j["participant_id"] = conn.participant_id;
        j["name"] = conn.display_name;
        return j;
    }
    if (method == "session.setStatus") {
        static const std::set<std::string> kValidStatuses = {"idle", "thinking", "writing", "awaiting_input", "done"};
        const std::string status = params.get("status").as_string();
        if (!kValidStatuses.count(status)) {
            throw RpcError{-32602, "status must be one of: idle, thinking, writing, awaiting_input, done"};
        }
        conn.status = status;
        return Json::Object();
    }
    if (method == "session.listParticipants") {
        Json arr = Json::Array();
        for (const auto &p : editor.Participants()) arr.push_back(ParticipantJson(p));
        return arr;
    }
    if (method == "buffer.create") {
        Json j = Json::Object();
        j["buffer_id"] = editor.CreateBufferForLua();
        return j;
    }
    if (method == "buffer.filename") {
        Json j = Json::Object();
        j["filename"] = editor.BufferFilenameForLua(params.get("buffer_id").as_int(-1));
        return j;
    }
    if (method == "file.open") {
        editor.LoadFile(params.get("path").as_string());
        return Json::Object();
    }
    if (method == "file.save") {
        const std::string path =
            params.contains("path") ? params.get("path").as_string() : editor.BufferFilenameForLua(editor.CurrentBufferId());
        Json j = Json::Object();
        j["ok"] = editor.SaveFile(path);
        return j;
    }
    if (method == "pane.split") {
        const bool vertical = params.get("dir").as_string() == "vertical";
        const std::string file = params.contains("file") ? params.get("file").as_string() : "";
        editor.RunCommand((vertical ? "vsplit " : "split ") + file);
        Json j = Json::Object();
        j["pane_id"] = editor.ActivePaneId();
        return j;
    }
    if (method == "pane.close") {
        editor.RunCommand("close");
        return Json::Object();
    }
    if (method == "pane.resize") {
        editor.ResizeActivePane(params.get("direction").as_string(), static_cast<float>(params.get("step").as_double()));
        return Json::Object();
    }
    if (method == "pane.focus") {
        editor.FocusPaneById(params.get("pane_id").as_int());
        return Json::Object();
    }
    if (method == "pane.splitWithBuffer") {
        const SplitDir dir = params.get("dir").as_string() == "horizontal" ? SplitDir::Horizontal : SplitDir::Vertical;
        editor.SplitPaneWithBufferTab(params.get("source_pane_id").as_int(), params.get("buffer_id").as_int(),
                                       params.get("dest_pane_id").as_int(), dir, params.get("before").as_bool());
        return Json::Object();
    }
    if (method == "command.run") {
        std::string cmd = params.get("cmd").as_string();
        if (!cmd.empty() && cmd[0] == ':') cmd.erase(0, 1);  // tolerate a pasted-looking ":cmd" same as bare "cmd"
        editor.RunCommand(cmd);
        return Json::Object();
    }
    if (method == "session.info") {
        Json j = Json::Object();
        j["pid"] = static_cast<int>(getpid());
        j["cwd"] = std::filesystem::current_path().string();
        Json files = Json::Array();
        for (int i = 0; i < editor.BufferCountForLua(); i++) {
            std::string filename = editor.BufferFilenameForLua(i);
            if (!filename.empty()) files.push_back(Json(filename));
        }
        j["open_files"] = std::move(files);
        return j;
    }
    if (method == "state.dump") {
        Json tabs = Json::Array();
        for (int i = 0; i < editor.TabCount(); i++) {
            Json tab = Json::Object();
            tab["active_pane_id"] = editor.TabActivePaneId(i);
            const SplitNode *root = editor.TabRoot(i);
            tab["root"] = root ? SplitNodeJson(*root) : Json::Object();
            tabs.push_back(std::move(tab));
        }
        Json buffers = Json::Array();
        for (int i = 0; i < editor.BufferCountForLua(); i++) buffers.push_back(BufferSummaryJson(editor, i));
        Json j = Json::Object();
        j["active_tab"] = editor.ActiveTabIndex();
        j["tabs"] = std::move(tabs);
        j["buffers"] = std::move(buffers);
        return j;
    }
    if (method == "pane.get") {
        const int pane_id = params.contains("pane_id") ? params.get("pane_id").as_int() : editor.ActivePaneId();
        for (int i = 0; i < editor.TabCount(); i++) {
            const SplitNode *root = editor.TabRoot(i);
            if (!root) continue;
            if (const Pane *pane = FindPane(*root, pane_id)) return PaneJson(*pane);
        }
        throw RpcError{-32602, "no such pane_id"};
    }
    if (method == "buffer.list") {
        Json buffers = Json::Array();
        for (int i = 0; i < editor.BufferCountForLua(); i++) buffers.push_back(BufferSummaryJson(editor, i));
        return buffers;
    }
    if (method == "buffer.getLines") {
        const int buffer_id = params.get("buffer_id").as_int(-1);
        if (buffer_id < 0 || buffer_id >= editor.BufferCountForLua()) throw RpcError{-32602, "unknown buffer_id"};
        const Buffer &buf = editor.GetBuffer(buffer_id);
        const int line_count = static_cast<int>(buf.lines.size());
        int start = params.contains("start") ? params.get("start").as_int() : 0;
        int end = params.contains("end") ? params.get("end").as_int() : line_count;
        start = std::max(0, std::min(start, line_count));
        end = std::max(start, std::min(end, line_count));
        Json lines = Json::Array();
        for (int i = start; i < end; i++) lines.push_back(Json(buf.lines[i]));
        Json j = Json::Object();
        j["buffer_id"] = buffer_id;
        j["lines"] = std::move(lines);
        return j;
    }
    throw RpcError{-32601, "method not found: " + method};
}

Json BuildResponse(const Json &id, const Json &result) {
    Json resp = Json::Object();
    resp["jsonrpc"] = Json("2.0");
    resp["id"] = id;
    resp["result"] = result;
    return resp;
}

Json BuildErrorResponse(const Json &id, int code, const std::string &message) {
    Json resp = Json::Object();
    resp["jsonrpc"] = Json("2.0");
    resp["id"] = id;
    Json err = Json::Object();
    err["code"] = code;
    err["message"] = message;
    resp["error"] = err;
    return resp;
}

// Server-initiated push, no "id" -- a JSON-RPC 2.0 notification, not a
// response to anything the client asked for.
Json BuildNotification(const std::string &method, const Json &params) {
    Json note = Json::Object();
    note["jsonrpc"] = Json("2.0");
    note["method"] = method;
    note["params"] = params;
    return note;
}

const char *NotifyLevelName(Editor::NotifyLevel level) {
    switch (level) {
        case Editor::NotifyLevel::Debug: return "debug";
        case Editor::NotifyLevel::Info: return "info";
        case Editor::NotifyLevel::Warn: return "warn";
        case Editor::NotifyLevel::Error: return "error";
    }
    return "info";
}

struct State {
    std::mutex mutex;  // guards every field below
    int listener_fd = -1;
    // Self-pipe (the classic portable way to interrupt a thread blocked in
    // accept()/poll()): closing or even shutdown()-ing listener_fd from
    // another thread is NOT reliably observed by a concurrent blocking
    // accept() call on it -- confirmed the hard way, via a live hang
    // during verification, close() alone left the accept thread parked in
    // the kernel forever (visible as __skb_wait_for_more_packets in its
    // /proc/<pid>/task/<tid>/wchan) with Stop()'s own join() then blocked
    // on it right behind, so mep never exited on :qa!. AcceptLoop instead
    // poll()s both listener_fd and wakeup_fds[0]; Stop() writes a byte to
    // wakeup_fds[1] to guarantee poll() returns immediately.
    int wakeup_fds[2] = {-1, -1};
    std::string socket_path;
    std::thread accept_thread;
    std::atomic<bool> stopping{false};
    std::vector<std::unique_ptr<Connection>> connections;
    // Assigns each connection's Connection::participant_id ("agent-N").
    // Atomic rather than mutex-guarded since AcceptLoop reads it before
    // taking state.mutex to push the new connection.
    std::atomic<int> next_participant_id{1};

    // --- Event-stream tracking (M3) --------------------------------------
    // Read/written only from PollOnce, i.e. only ever from the main
    // thread -- no locking needed for these, unlike everything above.
    // `events_seeded` is false only for the very first PollOnce call after
    // Start(): that call records a baseline (whatever the editor's state
    // happens to be at startup) without emitting anything, so a client
    // that connects moments after launch doesn't get a spurious "cursor
    // moved to 0,0 / mode changed to Normal" burst describing the
    // process's own initialization rather than anything that actually
    // happened.
    bool events_seeded = false;
    int last_pane_id = 0;
    CursorPos last_cursor;
    Mode last_mode = Mode::Normal;
    bool last_replace_mode = false;
    int last_change_epoch = 0;
    int last_notify_id = 0;
};

State &Instance() {
    static State state;
    return state;
}

void AcceptLoop(int listener_fd, int wakeup_fd) {
    pollfd fds[2] = {{listener_fd, POLLIN, 0}, {wakeup_fd, POLLIN, 0}};
    for (;;) {
        fds[0].revents = 0;
        fds[1].revents = 0;
        int ready = poll(fds, 2, -1);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (fds[1].revents & POLLIN) return;  // Stop() woke us: shut down
        if (!(fds[0].revents & POLLIN)) continue;

        int fd = accept(listener_fd, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return;  // listener gone or a fatal accept error -- either way, done
        }
        // Bounds Connection::Send's worst-case blocking time so a hung/
        // malicious client can stall PollOnce (main thread) for at most
        // this long instead of indefinitely.
        timeval timeout{};
        timeout.tv_usec = 200000;  // 200ms
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        State &state = Instance();
        auto conn = std::make_unique<Connection>();
        conn->fd = fd;
        conn->participant_id = "agent-" + std::to_string(state.next_participant_id.fetch_add(1));
        Connection *raw = conn.get();
        conn->reader_thread = std::thread([raw] { raw->ReaderLoop(); });

        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.stopping) return;  // Stop() ran while we were accepting; conn tears itself down
        state.connections.push_back(std::move(conn));
    }
}

void Broadcast(const std::vector<Connection *> &conns, const Json &notification) {
    for (Connection *conn : conns) conn->Send(notification);
}

// Diffs the editor's current cursor/focus/mode/buffer/notification state
// against what was last seen (State's last_* fields, main-thread-only) and
// pushes one event.* notification per thing that changed, to every
// connected client. Called once per frame from PollOnce, same cadence as
// the request dispatch above.
//
// Deliberately per-frame *polling* of existing public Editor accessors,
// not per-call-site "mark dirty" hooks at every place that might move the
// cursor/change the mode/refocus a pane: those turned out to have no
// small, reliable set of choke points to hook (checked during design --
// cursor motions alone have real paths that skip the one function that
// looked like a natural single hook point, e.g. EnterVisualBlockInsert;
// mode_ has 90+ direct assignment sites across every input handler, with
// no centralizing setter; active_pane_id has a dozen+ assignment sites
// across split/close/cycle/layout/tab/navigate/drag-drop). Polling is
// immune to missing a call site by construction -- it just compares
// current vs. last-seen state -- at the cost of only reporting the
// *current* value after a burst of same-frame changes, not every
// intermediate step, which matches this feature's own stated goal
// (coalesce a burst into one event, don't flood).
//
// buffer.changed is the one exception with a real known gap: it's driven
// by Editor::ChangeEpoch(), which the embedded-Lua mep.on_buffer_changed
// hook already uses for the identical purpose (same semantics an agent
// and a Lua script both get, by construction) -- but that epoch does NOT
// advance for the duration of an Insert-mode session (only at entry/exit,
// see editor.cpp's EnterNormal/EnterInsert) or for SetBufferLinesForLua
// (deliberately, per its own doc comment -- background terminal/REPL
// output has no undo-stack-worthy "change" to record). A connected agent
// therefore sees one bufferChanged event when a user finishes an editing
// burst and returns to Normal mode, not one per keystroke -- treated as a
// feature (matches the "coalesce, don't flood" goal above) for the common
// interactive-typing case, but background-buffer streaming via
// mep.set_buffer_lines stays invisible to this event; poll buffer.getLines
// if that matters.
void FlushAgentEvents(Editor &editor, const std::vector<Connection *> &conns) {
    State &state = Instance();

    const int pane_id = editor.ActivePaneId();
    const CursorPos cursor = editor.Cursor();
    const Mode mode = editor.CurrentMode();
    const bool replace_mode = editor.IsReplaceMode();
    const int change_epoch = editor.ChangeEpoch();
    const auto &notify_history = editor.NotifyHistory();  // newest-first, see Editor::Notify

    if (!state.events_seeded) {
        state.events_seeded = true;
        state.last_pane_id = pane_id;
        state.last_cursor = cursor;
        state.last_mode = mode;
        state.last_replace_mode = replace_mode;
        state.last_change_epoch = change_epoch;
        state.last_notify_id = notify_history.empty() ? 0 : notify_history.front().id;
        return;
    }

    if (!conns.empty()) {
        if (cursor.row != state.last_cursor.row || cursor.col != state.last_cursor.col || pane_id != state.last_pane_id) {
            Json params = Json::Object();
            params["pane_id"] = pane_id;
            params["row"] = cursor.row;
            params["col"] = cursor.col;
            Broadcast(conns, BuildNotification("event.cursorMoved", params));
        }
        if (pane_id != state.last_pane_id) {
            Json params = Json::Object();
            params["pane_id"] = pane_id;
            Broadcast(conns, BuildNotification("event.paneFocusChanged", params));
        }
        if (mode != state.last_mode || replace_mode != state.last_replace_mode) {
            Json params = Json::Object();
            params["mode"] = std::string(ModeName(mode, replace_mode));
            Broadcast(conns, BuildNotification("event.modeChanged", params));
        }
        if (change_epoch != state.last_change_epoch) {
            const int buffer_id = editor.CurrentBufferId();
            Json params = Json::Object();
            params["buffer_id"] = buffer_id;
            params["start_row"] = 0;
            params["end_row"] = static_cast<int>(editor.GetBuffer(buffer_id).lines.size());
            Broadcast(conns, BuildNotification("event.bufferChanged", params));
        }
        if (!notify_history.empty() && notify_history.front().id != state.last_notify_id) {
            std::vector<const Editor::NotifyEntry *> fresh;
            for (const auto &entry : notify_history) {
                if (entry.id <= state.last_notify_id) break;  // newest-first: everything after this is older still
                fresh.push_back(&entry);
            }
            for (auto it = fresh.rbegin(); it != fresh.rend(); ++it) {  // emit oldest-first
                Json params = Json::Object();
                params["level"] = NotifyLevelName((*it)->level);
                params["message"] = (*it)->message;
                Broadcast(conns, BuildNotification("event.notification", params));
            }
        }
    }

    state.last_pane_id = pane_id;
    state.last_cursor = cursor;
    state.last_mode = mode;
    state.last_replace_mode = replace_mode;
    state.last_change_epoch = change_epoch;
    state.last_notify_id = notify_history.empty() ? state.last_notify_id : notify_history.front().id;
}

// Removes other mep instances' leftover *.sock files whose listening process
// is gone -- e.g. mep was killed rather than exiting via :qa! (the only path
// that runs Stop()'s own unlink). Left in place, a dead file makes
// mcp/mep_client.ts's discoverSocketPath() glob match more than one socket,
// which it treats as an unresolvable ambiguity (several live mep windows is
// a real case it refuses to guess between) -- so the MCP server's startup
// session.identify silently fails and the tab-bar agent chip never appears,
// for every future mep session, until someone notices and removes it by hand.
void PruneStaleSockets(const std::string &dir, const std::string &own_path) {
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) return;
        if (entry.path().extension() != ".sock") continue;
        const std::string entry_path = entry.path().string();
        if (entry_path == own_path) continue;

        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) continue;
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (entry_path.size() >= sizeof(addr.sun_path)) {
            CloseFd(fd);
            continue;
        }
        std::strncpy(addr.sun_path, entry_path.c_str(), sizeof(addr.sun_path) - 1);
        // AF_UNIX connect() is a local rendezvous, not a network round trip --
        // it returns immediately whether or not a listener is present, so no
        // poll()/timeout dance is needed here (unlike AcceptLoop's poll() above).
        const int rc = connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
        const int connect_errno = errno;
        CloseFd(fd);
        if (rc == 0) continue;  // a live mep instance is listening -- leave it alone
        if (connect_errno == ECONNREFUSED || connect_errno == ENOENT) {
            unlink(entry_path.c_str());  // file outlived its listener -- safe to remove
        }
    }
}

}  // namespace

void Start() {
    State &state = Instance();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.listener_fd >= 0) return;  // already started

    const std::string dir = MepAgentSocketDir();
    if (dir.empty()) {
        std::cerr << "mep: agent socket unavailable (no data directory)\n";
        return;
    }
    const std::string path = dir + "/" + std::to_string(static_cast<long>(getpid())) + ".sock";
    PruneStaleSockets(dir, path);
    unlink(path.c_str());  // stale socket left behind if a pid was ever reused

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "mep: agent socket: socket() failed: " << std::strerror(errno) << "\n";
        return;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        std::cerr << "mep: agent socket path too long: " << path << "\n";
        CloseFd(fd);
        return;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || listen(fd, 16) != 0) {
        std::cerr << "mep: agent socket: bind/listen failed: " << std::strerror(errno) << "\n";
        CloseFd(fd);
        unlink(path.c_str());
        return;
    }
    chmod(path.c_str(), 0600);  // belt-and-suspenders alongside the 0700 MepAgentSocketDir()

    if (pipe(state.wakeup_fds) != 0) {
        std::cerr << "mep: agent socket: pipe() failed: " << std::strerror(errno) << "\n";
        CloseFd(fd);
        unlink(path.c_str());
        return;
    }

    state.listener_fd = fd;
    state.socket_path = path;
    const int wakeup_read = state.wakeup_fds[0];
    state.accept_thread = std::thread([fd, wakeup_read] { AcceptLoop(fd, wakeup_read); });
    std::cout << "mep: agent socket listening at " << path << "\n";
}

void Stop() {
    State &state = Instance();
    int fd = -1;
    int wakeup_write = -1;
    int wakeup_read = -1;
    std::string path;
    std::thread accept_thread;
    std::vector<std::unique_ptr<Connection>> connections;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.listener_fd < 0) return;
        state.stopping = true;
        fd = state.listener_fd;
        wakeup_read = state.wakeup_fds[0];
        wakeup_write = state.wakeup_fds[1];
        path = state.socket_path;
        accept_thread = std::move(state.accept_thread);
        connections = std::move(state.connections);
        state.listener_fd = -1;
        state.wakeup_fds[0] = state.wakeup_fds[1] = -1;
        state.socket_path.clear();
    }
    const char byte = 0;
    ssize_t written = write(wakeup_write, &byte, 1);  // guaranteed to unblock AcceptLoop's poll(), unlike closing listener_fd
    (void)written;  // nothing more to do if this failed -- the join below would just hang, which is no worse than before this fix
    if (accept_thread.joinable()) accept_thread.join();
    CloseFd(fd);
    CloseFd(wakeup_read);
    CloseFd(wakeup_write);
    connections.clear();  // each ~Connection() shuts down its fd and joins its reader thread
    if (!path.empty()) unlink(path.c_str());
}

void PollOnce(Editor &editor) {
    State &state = Instance();

    // Snapshot which connections exist, then release state.mutex *before*
    // dispatching into Editor -- Dispatch() runs arbitrary Editor code
    // (ExecuteCommandLine can run Lua, ex-commands, anything), and the
    // :AgentSocket ex-command specifically calls back into this module's
    // own SocketPath(), which locks state.mutex itself. Holding the lock
    // across that callback would self-deadlock the main thread (this is
    // not a hypothetical: it did, during verification, hang mep on the
    // very first `:AgentSocket` sent over the RPC socket). Connection
    // objects are heap-allocated and only ever erased below, from this
    // same function, so a raw pointer taken here stays valid for the rest
    // of this call even though state.connections itself (the vector of
    // unique_ptr) may be appended to concurrently by AcceptLoop.
    std::vector<Connection *> conns;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.listener_fd < 0) return;
        conns.reserve(state.connections.size());
        for (auto &conn : state.connections) conns.push_back(conn.get());
    }

    for (Connection *conn : conns) {
        std::deque<Json> requests;
        {
            std::lock_guard<std::mutex> conn_lock(conn->mutex);
            requests.swap(conn->pending);
        }
        for (const Json &request : requests) {
            const Json &id = request.get("id");
            const std::string method = request.get("method").as_string();
            const Json params = request.contains("params") ? request.get("params") : Json::Object();
            Json response;
            try {
                response = BuildResponse(id, Dispatch(editor, *conn, method, params));
            } catch (const RpcError &err) {
                response = BuildErrorResponse(id, err.code, err.message);
            } catch (const std::exception &ex) {
                response = BuildErrorResponse(id, -32000, ex.what());
            }
            if (!request.contains("id")) continue;  // a notification: side effects already ran, no reply expected
            conn->Send(response);
        }
    }

    // After request-driven mutations have run: diff current editor state
    // against what was last observed and push any event.* notifications.
    FlushAgentEvents(editor, conns);

    std::lock_guard<std::mutex> lock(state.mutex);
    state.connections.erase(
        std::remove_if(state.connections.begin(), state.connections.end(), [](const std::unique_ptr<Connection> &c) { return c->closed.load(); }),
        state.connections.end());
}

std::string SocketPath() {
    State &state = Instance();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.socket_path;
}

std::vector<AgentParticipant> AgentParticipants() {
    State &state = Instance();
    std::vector<AgentParticipant> result;
    std::lock_guard<std::mutex> lock(state.mutex);
    result.reserve(state.connections.size());
    for (const auto &conn : state.connections) {
        AgentParticipant p;
        p.id = conn->participant_id;
        p.name = conn->display_name;
        p.buffer_id = conn->cursor_buffer_id;
        p.row = conn->cursor.row;
        p.col = conn->cursor.col;
        p.has_location = conn->cursor_buffer_id >= 0;
        p.status = conn->status;
        result.push_back(std::move(p));
    }
    return result;
}

}  // namespace mep::agent

#endif  // !defined(__EMSCRIPTEN__) && !defined(_WIN32)
