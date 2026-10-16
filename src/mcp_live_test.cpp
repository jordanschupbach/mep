// mep-mcp-live-test: spawns a real mep instance AND the real `mep-mcp`
// bridge (src/mcp_bridge.cpp) wrapping it, then drives the bridge exactly
// as an MCP client (Claude Code, Claude Desktop, ...) would -- the
// initialize handshake, tools/list, and a series of tools/call requests
// checked against the real running editor. Same shape and reasoning as
// src/agent_rpc_test.cpp (real transport, no mocking, CHECKs that survive
// NDEBUG) one layer up the stack: this is the agent socket as seen
// through MCP's newline-delimited JSON-RPC stdio transport.
//
// Ported from the end-to-end test of the Deno/TypeScript MCP server that
// preceded mep-mcp; both have since been removed.
//
// Isolation: mep runs with --no-session and a throwaway, deliberately
// short $XDG_DATA_HOME (a Unix socket path has to fit in ~104 bytes on
// macOS), so its agent socket, project list and session files never mix
// with a real per-user mep the human may have running. The bridge is
// pointed at that socket with MEP_AGENT_SOCKET.
//
// Needs a live display (native `mep` requires one), so it lives in
// `just test-gui`, not `just test`.
//
// Usage: mep-mcp-live-test /path/to/mep [/path/to/mep-mcp]
//        (mep-mcp defaults to the binary next to mep)

#include "agent_rpc_test_harness.h"

#include <fcntl.h>
#include <signal.h>

#include <cerrno>
#include <cstdlib>
#include <string>

namespace {

// Child-process plumbing for mep-mcp: its stdin is our request pipe, its
// stdout our response pipe (MCP's stdio transport: one JSON-RPC message
// per line), stderr inherited so its diagnostics land in the test log.
struct Bridge {
    pid_t pid = -1;
    int to_child = -1;
    int from_child = -1;
    std::string read_buf;
    int next_id = 1;
};

void WriteLine(Bridge &b, const Json &msg) {
    const std::string line = msg.dump() + "\n";
    size_t off = 0;
    while (off < line.size()) {
        const ssize_t n = write(b.to_child, line.data() + off, line.size() - off);
        CHECK_CTX(n > 0, "write to mep-mcp's stdin failed: " + std::string(std::strerror(errno)));
        off += static_cast<size_t>(n);
    }
}

// Reads one newline-terminated message from the bridge, failing the test
// rather than hanging forever if none arrives within `timeout_ms`.
Json ReadLine(Bridge &b, int timeout_ms = 15000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const size_t nl = b.read_buf.find('\n');
        if (nl != std::string::npos) {
            const std::string line = b.read_buf.substr(0, nl);
            b.read_buf.erase(0, nl + 1);
            Json msg;
            CHECK_CTX(Json::Parse(line, &msg), "mep-mcp wrote a line that is not JSON: [" + line + "]");
            return msg;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        CHECK_CTX(left > 0, "timed out waiting for a line from mep-mcp");
        pollfd pfd{b.from_child, POLLIN, 0};
        const int ready = poll(&pfd, 1, static_cast<int>(left));
        if (ready <= 0) continue;
        char chunk[65536];
        const ssize_t n = read(b.from_child, chunk, sizeof(chunk));
        CHECK_CTX(n > 0, "mep-mcp closed its stdout (or the read failed) mid-session");
        b.read_buf.append(chunk, static_cast<size_t>(n));
    }
}

// One MCP request/response round trip. The bridge is strictly
// request/response (it never sends server-initiated notifications), so
// the next line must be the answer to this id.
Json Request(Bridge &b, const std::string &method, const Json &params) {
    const int id = b.next_id++;
    Json req = Json::Object();
    req["jsonrpc"] = "2.0";
    req["id"] = id;
    req["method"] = method;
    req["params"] = params;
    WriteLine(b, req);
    Json resp = ReadLine(b);
    CHECK_CTX(resp.get("jsonrpc").as_string("") == "2.0", "response lacks jsonrpc 2.0: " + resp.dump());
    CHECK_CTX(resp.get("id").as_int(-1) == id, "response id mismatch for " + method + ": " + resp.dump());
    return resp;
}

// tools/call: returns the MCP CallToolResult ({content, isError?}). A
// JSON-RPC-level error here (as opposed to a tool error result) is
// always a bug in the bridge.
Json CallTool(Bridge &b, const std::string &name, const Json &args = Json::Object()) {
    Json params = Json::Object();
    params["name"] = name;
    params["arguments"] = args;
    Json resp = Request(b, "tools/call", params);
    CHECK_CTX(resp.contains("result"), "tools/call " + name + " got a JSON-RPC error: " + resp.dump());
    return resp.get("result");
}

bool IsError(const Json &result) { return result.get("isError").as_bool(false); }

void AssertNotError(const Json &result, const std::string &label) {
    CHECK_CTX(!IsError(result), label + " returned a tool error: " + result.dump());
}

// The first text block of a tool result, parsed as JSON -- every
// successful mep-mcp result is the RPC result serialized into one text
// block.
Json ParseToolJson(const Json &result, const std::string &label) {
    AssertNotError(result, label);
    for (const Json &block : result.get("content").items()) {
        if (block.get("type").as_string("") != "text") continue;
        Json parsed;
        CHECK_CTX(Json::Parse(block.get("text").as_string(""), &parsed), label + ": text content is not JSON: " + result.dump());
        return parsed;
    }
    CHECK_CTX(false, label + ": tool result had no text content: " + result.dump());
    return Json();
}

Json Args1(const std::string &key, const Json &value) {
    Json a = Json::Object();
    a[key] = value;
    return a;
}

const Json *FindBy(const Json &arr, const std::string &key, const std::string &value) {
    for (const Json &item : arr.items()) {
        if (item.get(key).as_string("") == value) return &item;
    }
    return nullptr;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: mep-mcp-live-test /path/to/mep [/path/to/mep-mcp]\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);  // a dead bridge should fail a CHECK, not kill us silently
    const std::string mep_path = argv[1];
    const std::string bridge_path = argc > 2 ? std::string(argv[2])
                                             : (std::filesystem::path(mep_path).parent_path() / "mep-mcp").string();
    CHECK_CTX(std::filesystem::exists(bridge_path), "mep-mcp not found at " + bridge_path + " (build the mep-mcp target)");

    // Short on purpose -- see the top comment on the socket path limit.
    char data_home_tmpl[] = "/tmp/mepmcp.XXXXXX";
    CHECK(mkdtemp(data_home_tmpl) != nullptr);
    const std::string data_home = data_home_tmpl;

    // --- mep ----------------------------------------------------------------
    // stdout/stderr go to /dev/null: nothing reads them, and mep's boot
    // logging into an unread pipe would eventually block mep itself.
    pid_t mep_pid = fork();
    CHECK(mep_pid >= 0);
    if (mep_pid == 0) {
        setenv("XDG_DATA_HOME", data_home.c_str(), 1);
        const int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        execl(mep_path.c_str(), mep_path.c_str(), "--no-session", nullptr);
        _exit(127);
    }
    const std::string socket_path = data_home + "/mep/agent-sockets/" + std::to_string(static_cast<long>(mep_pid)) + ".sock";
    Bridge bridge;
    auto fail_cleanup = [&]() {
        if (bridge.pid > 0) kill(bridge.pid, SIGKILL);
        kill(mep_pid, SIGKILL);
    };
    // CHECK aborts; make sure neither child outlives a failed run.
    static std::function<void()> g_cleanup;
    g_cleanup = fail_cleanup;
    signal(SIGABRT, [](int) {
        if (g_cleanup) g_cleanup();
        signal(SIGABRT, SIG_DFL);
        std::abort();
    });

    bool bound = false;
    for (int i = 0; i < 150 && !bound; i++) {
        if (std::filesystem::exists(socket_path)) {
            const int probe = ConnectOnce(socket_path);
            if (probe >= 0) {
                close(probe);
                bound = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK_CTX(bound, "mep never bound its agent socket at " + socket_path + " within 15s");

    // --- mep-mcp ------------------------------------------------------------
    int in_pipe[2];   // test -> bridge stdin
    int out_pipe[2];  // bridge stdout -> test
    CHECK(pipe(in_pipe) == 0);
    CHECK(pipe(out_pipe) == 0);
    bridge.pid = fork();
    CHECK(bridge.pid >= 0);
    if (bridge.pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        setenv("MEP_AGENT_SOCKET", socket_path.c_str(), 1);
        setenv("XDG_DATA_HOME", data_home.c_str(), 1);
        unsetenv("MEP_AGENT_NAME");
        unsetenv("MEP_TERMINAL_BUFFER");
        execl(bridge_path.c_str(), bridge_path.c_str(), nullptr);
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    bridge.to_child = in_pipe[1];
    bridge.from_child = out_pipe[0];

    // --- MCP handshake ------------------------------------------------------
    {
        Json params = Json::Object();
        params["protocolVersion"] = "2025-06-18";
        params["capabilities"] = Json::Object();
        Json client_info = Json::Object();
        client_info["name"] = "mep-mcp-live-test";
        client_info["version"] = "0.0.1";
        params["clientInfo"] = client_info;
        Json init = Request(bridge, "initialize", params);
        CHECK_CTX(init.contains("result"), "initialize failed: " + init.dump());
        const Json &r = init.get("result");
        CHECK_CTX(r.get("protocolVersion").as_string("") == "2025-06-18", "initialize should echo the protocol version: " + init.dump());
        CHECK_CTX(r.get("capabilities").contains("tools"), "initialize should advertise tools: " + init.dump());
        CHECK_CTX(!r.get("serverInfo").get("name").as_string("").empty(), "initialize lacks serverInfo.name: " + init.dump());
        Json note = Json::Object();
        note["jsonrpc"] = "2.0";
        note["method"] = "notifications/initialized";
        WriteLine(bridge, note);  // a notification: no reply expected
        Json ping = Request(bridge, "ping", Json::Object());
        CHECK_CTX(ping.contains("result"), "ping failed: " + ping.dump());
        Json bogus = Request(bridge, "no/such/method", Json::Object());
        CHECK_CTX(bogus.contains("error") && bogus.get("error").get("code").as_int(0) == -32601,
                  "an unknown MCP method should be JSON-RPC error -32601: " + bogus.dump());
    }

    // --- tools/list ---------------------------------------------------------
    {
        Json list = Request(bridge, "tools/list", Json::Object());
        CHECK_CTX(list.contains("result"), "tools/list failed: " + list.dump());
        const Json &tools = list.get("result").get("tools");
        std::vector<std::string> names;
        for (const Json &t : tools.items()) {
            names.push_back(t.get("name").as_string(""));
            CHECK_CTX(t.get("inputSchema").get("type").as_string("") == "object", "tool without an object inputSchema: " + t.dump());
            CHECK_CTX(!t.get("description").as_string("").empty(), "tool without a description: " + t.dump());
        }
        std::string joined;
        for (const auto &n : names) joined += n + " ";
        auto has = [&](const std::string &n) { return std::find(names.begin(), names.end(), n) != names.end(); };
        for (const char *t : {"mep_cursor_set", "mep_identify", "mep_workspace_list", "mep_workspace_switch",
                              "mep_workspace_create", "mep_workspace_delete", "mep_project_list", "mep_project_switch",
                              "mep_project_open", "mep_poll_events", "mep_set_status", "mep_list_participants"}) {
            CHECK_CTX(has(t), std::string("expected ") + t + " among tools: " + joined);
        }
        CHECK_CTX(names.size() >= 31, "expected at least 31 tools, got " + std::to_string(names.size()) + ": " + joined);
        std::vector<std::string> sorted = names;
        std::sort(sorted.begin(), sorted.end());
        CHECK_CTX(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end(), "duplicate tool names: " + joined);
    }

    // --- Workspaces (WORKSPACES_PLAN.md Phase 11): the freshly launched
    // instance has exactly one project with one primary "main" workspace;
    // session.info and buffer.list carry the workspace fields.
    Json projects;
    {
        Json ws = ParseToolJson(CallTool(bridge, "mep_workspace_list"), "mep_workspace_list");
        CHECK_CTX(ws.size() == 1 && ws.items()[0].get("name").as_string("") == "main" &&
                      ws.items()[0].get("primary").as_bool(false) && ws.items()[0].get("active").as_bool(false),
                  "expected a single active primary \"main\" workspace, got: " + ws.dump());
        projects = ParseToolJson(CallTool(bridge, "mep_project_list"), "mep_project_list");
        CHECK_CTX(projects.size() == 1 && projects.items()[0].get("workspace_count").as_int(-1) == 1 &&
                      projects.items()[0].get("active").as_bool(false),
                  "expected one active project with one workspace, got: " + projects.dump());
        Json info = ParseToolJson(CallTool(bridge, "mep_session_info"), "mep_session_info");
        CHECK_CTX(info.get("workspace").as_string("") == "main" &&
                      info.get("workspace_root").as_string("") == info.get("cwd").as_string("?") &&
                      info.get("project").as_string("") == projects.items()[0].get("name").as_string("?"),
                  "session.info should describe the main workspace (cwd == workspace_root), got: " + info.dump());
        CHECK_CTX(info.get("pid").as_int(-1) == static_cast<int>(mep_pid), "session.info pid should be the mep we spawned: " + info.dump());
        Json scoped = ParseToolJson(CallTool(bridge, "mep_buffer_list"), "mep_buffer_list");
        Json all = ParseToolJson(CallTool(bridge, "mep_buffer_list", Args1("workspace", Json("all"))), "mep_buffer_list all");
        CHECK_CTX(scoped.size() > 0 && all.size() >= scoped.size(),
                  "buffer.list scoping looks wrong: scoped=" + scoped.dump() + " all=" + all.dump());
        Json no_ws = CallTool(bridge, "mep_workspace_switch", Args1("name", Json("no-such-workspace")));
        CHECK_CTX(IsError(no_ws), "switching to a nonexistent workspace should be a tool error, got: " + no_ws.dump());
    }

    // mep-mcp auto-identifies on startup (default name "Claude" unless
    // MEP_AGENT_NAME is set -- unset above) -- confirm that participant
    // exists, and that mep_identify renames that same participant.
    {
        Json before = ParseToolJson(CallTool(bridge, "mep_list_participants"), "mep_list_participants (startup)");
        CHECK_CTX(FindBy(before, "name", "Claude") != nullptr, "expected the startup identify to register \"Claude\": " + before.dump());
        Json identify = ParseToolJson(CallTool(bridge, "mep_identify", Args1("name", Json("Test Agent"))), "mep_identify");
        CHECK_CTX(identify.get("name").as_string("") == "Test Agent", "expected mep_identify to report the new name, got: " + identify.dump());
    }

    // --- Own cursor + editing -------------------------------------------------
    {
        Json at = Json::Object();
        at["row"] = 0;
        at["col"] = 0;
        AssertNotError(CallTool(bridge, "mep_cursor_set", at), "mep_cursor_set");
        AssertNotError(CallTool(bridge, "mep_buffer_insert_text", Args1("text", Json("mcp test"))), "mep_buffer_insert_text");
        Json cursor = ParseToolJson(CallTool(bridge, "mep_cursor_get"), "mep_cursor_get");
        CHECK_CTX(cursor.get("col").as_int(-1) == 8 && cursor.get("row").as_int(-1) == 0,
                  "expected cursor at 0:8 after inserting \"mcp test\", got " + cursor.dump());
        const int buffer_id = cursor.get("buffer_id").as_int(-1);
        Json lines_args = Json::Object();
        lines_args["buffer_id"] = buffer_id;
        lines_args["start"] = 0;
        lines_args["end"] = 1;
        Json lines = ParseToolJson(CallTool(bridge, "mep_buffer_get_lines", lines_args), "mep_buffer_get_lines");
        CHECK_CTX(lines.dump().find("mcp test") != std::string::npos, "inserted text should be in the buffer: " + lines.dump());
    }

    {
        Json dump = ParseToolJson(CallTool(bridge, "mep_state_dump"), "mep_state_dump");
        CHECK_CTX(dump.get("buffers").is_array() && dump.get("buffers").size() >= 1,
                  "expected state.dump to report at least one buffer, got: " + dump.dump());
    }

    // --- Pane split + the bridge's own event queue ----------------------------
    {
        (void)ParseToolJson(CallTool(bridge, "mep_poll_events"), "mep_poll_events (drain)");
        AssertNotError(CallTool(bridge, "mep_pane_split", Args1("dir", Json("vertical"))), "mep_pane_split");
        // Events can lag a frame or two behind the response (same reasoning
        // as agent_rpc_test.cpp's DrainEvents); retry briefly rather than
        // racing a single fixed sleep.
        bool saw_focus = false;
        Json collected = Json::Array();
        for (int i = 0; i < 20 && !saw_focus; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            Json events = ParseToolJson(CallTool(bridge, "mep_poll_events"), "mep_poll_events");
            CHECK_CTX(events.is_array(), "mep_poll_events should return an array: " + events.dump());
            for (const Json &e : events.items()) {
                collected.push_back(e);
                if (e.get("method").as_string("") == "event.paneFocusChanged") saw_focus = true;
            }
        }
        CHECK_CTX(saw_focus, "expected event.paneFocusChanged among polled events after pane.split, got: " + collected.dump());
    }

    // --- Error results for bad input ------------------------------------------
    {
        Json bad_buffer = CallTool(bridge, "mep_buffer_get_lines", Args1("buffer_id", Json(9999)));
        CHECK_CTX(IsError(bad_buffer), "expected a bad buffer_id to be a tool error, got: " + bad_buffer.dump());
        Json bad_tool = CallTool(bridge, "mep_no_such_tool");
        CHECK_CTX(IsError(bad_tool), "expected an unknown tool name to be a tool error, got: " + bad_tool.dump());
    }

    // --- Participants & status (COLLAB_CURSORS_PLAN.md Phase 1g) ---------------
    // mep_buffer_insert_text above should already have auto-set the status
    // to "writing".
    std::string self_id;
    auto status_of_self = [&]() {
        Json ps = ParseToolJson(CallTool(bridge, "mep_list_participants"), "mep_list_participants");
        const Json *me = FindBy(ps, "id", self_id);
        CHECK_CTX(me != nullptr, "lost our own participant " + self_id + ": " + ps.dump());
        return me->get("status").as_string("");
    };
    {
        Json ps = ParseToolJson(CallTool(bridge, "mep_list_participants"), "mep_list_participants");
        const Json *self = FindBy(ps, "name", "Test Agent");
        CHECK_CTX(self != nullptr, "expected to find \"Test Agent\" in mep_list_participants, got: " + ps.dump());
        CHECK_CTX(self->get("status").as_string("") == "writing", "expected auto-status \"writing\" after an edit, got: " + self->dump());
        self_id = self->get("id").as_string("");
        CHECK_CTX(!self_id.empty(), "participant has no string id: " + self->dump());
        CHECK_CTX(FindBy(ps, "name", "Claude") == nullptr, "the rename should not have left a second \"Claude\" participant: " + ps.dump());
    }
    AssertNotError(CallTool(bridge, "mep_set_status", Args1("status", Json("awaiting_input"))), "mep_set_status");
    CHECK_CTX(status_of_self() == "awaiting_input", "expected mep_set_status(\"awaiting_input\") to stick");
    {
        Json bad_status = CallTool(bridge, "mep_set_status", Args1("status", Json("not-a-real-status")));
        CHECK_CTX(IsError(bad_status), "expected an unknown status to be a tool error, got: " + bad_status.dump());
    }

    // Status auto-nudge: "done" must survive pure introspection
    // (mep_state_dump, mep_list_participants), but the next real action
    // clears it to "thinking" even if the model forgets to say so itself.
    AssertNotError(CallTool(bridge, "mep_set_status", Args1("status", Json("done"))), "mep_set_status done");
    AssertNotError(CallTool(bridge, "mep_state_dump"), "mep_state_dump");  // read-only
    AssertNotError(CallTool(bridge, "mep_cursor_get"), "mep_cursor_get");  // read-only
    CHECK_CTX(status_of_self() == "done", "read-only tool calls should not clear a \"done\" status");
    {
        Json at = Json::Object();
        at["row"] = 0;
        at["col"] = 0;
        AssertNotError(CallTool(bridge, "mep_cursor_set", at), "mep_cursor_set after done");
    }
    CHECK_CTX(status_of_self() == "thinking", "expected a real action after \"done\" to auto-nudge status to \"thinking\"");

    // --- Quit through the bridge ------------------------------------------------
    AssertNotError(CallTool(bridge, "mep_command_run", Args1("cmd", Json("qa!"))), "mep_command_run qa!");

    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (;;) {
        const pid_t waited = waitpid(mep_pid, &status, WNOHANG);
        if (waited == mep_pid) break;
        CHECK_CTX(std::chrono::steady_clock::now() < deadline, "mep was still running 20s after :qa! via mep_command_run");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK_CTX(WIFEXITED(status) && WEXITSTATUS(status) == 0, "mep exited abnormally (status " + std::to_string(status) + ")");
    CHECK_CTX(!std::filesystem::exists(socket_path), "mep's socket " + socket_path + " should have been unlinked on clean exit");

    // With mep gone, a tool call must come back as a tool error, not wedge
    // or crash the bridge -- it outlives the editor it talks to.
    {
        Json gone = CallTool(bridge, "mep_session_info");
        CHECK_CTX(IsError(gone), "a tool call with mep gone should be a tool error, got: " + gone.dump());
    }

    // EOF on stdin is how an MCP client ends a stdio server.
    close(bridge.to_child);
    int bridge_status = 0;
    const auto bridge_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        const pid_t waited = waitpid(bridge.pid, &bridge_status, WNOHANG);
        if (waited == bridge.pid) break;
        CHECK_CTX(std::chrono::steady_clock::now() < bridge_deadline, "mep-mcp did not exit on stdin EOF");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    g_cleanup = nullptr;
    CHECK_CTX(WIFEXITED(bridge_status) && WEXITSTATUS(bridge_status) == 0,
              "mep-mcp exited abnormally (status " + std::to_string(bridge_status) + ")");
    close(bridge.from_child);
    std::filesystem::remove_all(data_home);

    std::printf("mcp_live_test passed (initialize, tools/list, workspaces/projects/session, identify, cursor/insert, "
                "state dump, pane split + events, error results, participant status incl. done-nudge, quit via MCP)\n");
    return 0;
}
