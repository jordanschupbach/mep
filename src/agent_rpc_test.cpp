// mep-agent-rpc-test: spawns a real mep instance and drives its agent-
// control socket exactly as an external AI agent would -- connect over
// the real Unix socket, issue a few JSON-RPC round trips using the real
// Content-Length-framed wire protocol, then ask it to quit and confirm
// both a clean process exit and the socket file's own cleanup. Mirrors
// collab_session_test.cpp's own shape (a standalone executable against
// the real transport, no mocking) -- unlike that test, this one needs a
// live display (X11/whatever DESKTOP GLFW backend is in use): it launches
// the actual native `mep` binary, same requirement `just run` already
// has, not a new one this test introduces.
//
// Deliberately does NOT use assert() for anything that must actually
// run: this project's own default CMAKE_BUILD_TYPE is Release (see
// CMakeLists.txt), which defines NDEBUG, and NDEBUG compiles assert(...)
// to nothing at all -- not just the check, the entire expression,
// including any side-effecting call inside it. An earlier version of
// this file wrapped every RPC call directly in assert(...), which in a
// Release build never actually sent the requests (including the final
// "qa!") at all: the spawned mep process just ran forever, healthy and
// orphaned, while this test reported success regardless. CHECK() below
// is the replacement -- a plain runtime if() that always executes and
// aborts with a clear message on failure, in every build configuration.
//
// Usage: mep-agent-rpc-test /path/to/mep

#include "agent_rpc_test_harness.h"

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: mep-agent-rpc-test /path/to/mep\n");
        return 2;
    }

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        // --no-session: never read or write the real per-project session
        // file for whatever directory this test happens to run in.
        execl(argv[1], argv[1], "--no-session", nullptr);
        _exit(127);  // execl only returns on failure
    }

    const std::string socket_path = MepAgentSocketDir() + "/" + std::to_string(static_cast<long>(pid)) + ".sock";
    int fd = -1;
    for (int i = 0; i < 100 && fd < 0; i++) {
        if (std::filesystem::exists(socket_path)) fd = ConnectOnce(socket_path);
        if (fd < 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK_CTX(fd >= 0, "mep never bound its agent socket within 10s");

    std::string read_buf;

    // Pin this connection's virtual cursor to buffer 0 explicitly rather
    // than relying on EnsureConnCursorInitialized's own default (the
    // project README, per COLLAB_CURSORS_PLAN.md's "always-exists cursor"
    // work) -- this section is exercising buffer.insertText's mechanics,
    // not the readme-default behavior, and buffer_list further down
    // assumes "hi" landed in buffer_list.items()[0].
    Json cursor_params = Json::Object();
    cursor_params["buffer_id"] = 0;
    cursor_params["row"] = 0;
    cursor_params["col"] = 0;
    Json set_cursor_resp = Call(fd, 1, "cursor.set", cursor_params, &read_buf);
    CHECK(set_cursor_resp.contains("result"));

    Json insert_params = Json::Object();
    insert_params["text"] = "hi";
    Json insert_resp = Call(fd, 2, "buffer.insertText", insert_params, &read_buf);
    CHECK(insert_resp.contains("result"));

    Json got_cursor = Call(fd, 3, "cursor.get", Json::Object(), &read_buf);
    CHECK(got_cursor.get("result").get("col").as_int() == 2);

    Json split_params = Json::Object();
    split_params["dir"] = "vertical";
    Json split = Call(fd, 4, "pane.split", split_params, &read_buf);
    CHECK(split.contains("result"));
    CHECK(split.get("result").contains("pane_id"));

    // --- M2: state introspection ---
    Json dump = Call(fd, 5, "state.dump", Json::Object(), &read_buf);
    Json dump_result = dump.get("result");
    CHECK_CTX(dump_result.get("tabs").items().size() == 1, "state.dump=[" + dump.dump() + "]");
    Json tab0 = dump_result.get("tabs").items()[0];
    Json root = tab0.get("root");
    CHECK(root.get("dir").as_string() == "vertical");
    CHECK(root.get("children").items().size() == 2);
    const int active_pane_id = tab0.get("active_pane_id").as_int();

    Json pane_get = Call(fd, 6, "pane.get", Json::Object(), &read_buf);  // no pane_id: defaults to the active pane
    CHECK(pane_get.get("result").get("id").as_int() == active_pane_id);

    Json bad_pane_params = Json::Object();
    bad_pane_params["pane_id"] = -999;
    Json bad_pane = Call(fd, 7, "pane.get", bad_pane_params, &read_buf);
    CHECK(bad_pane.contains("error"));
    CHECK(bad_pane.get("error").get("code").as_int() == -32602);

    Json buffer_list = Call(fd, 8, "buffer.list", Json::Object(), &read_buf);
    CHECK(buffer_list.get("result").is_array());
    CHECK(buffer_list.get("result").items().size() >= 1);
    const int buffer_id = buffer_list.get("result").items()[0].get("id").as_int();

    Json lines_params = Json::Object();
    lines_params["buffer_id"] = buffer_id;
    Json lines = Call(fd, 9, "buffer.getLines", lines_params, &read_buf);
    CHECK(lines.get("result").get("lines").items().size() >= 1);
    CHECK(lines.get("result").get("lines").items()[0].as_string() == "hi");  // from buffer.insertText above

    Json bad_buffer_params = Json::Object();
    bad_buffer_params["buffer_id"] = 9999;
    Json bad_buffer = Call(fd, 10, "buffer.getLines", bad_buffer_params, &read_buf);
    CHECK(bad_buffer.contains("error"));
    CHECK(bad_buffer.get("error").get("code").as_int() == -32602);

    Json unknown = Call(fd, 11, "no.such.method", Json::Object(), &read_buf);
    CHECK(unknown.contains("error"));
    CHECK(unknown.get("error").get("code").as_int() == -32601);

    // --- M3: event stream ---
    // `:normal i` (tried first during design) doesn't work for exercising
    // event.modeChanged -- RunNormalKeys (editor.cpp) deliberately force-
    // exits back to Normal mode if the simulated keys leave it anywhere
    // else, so mode_ never has a net change across one PollOnce for that
    // command. `:terminal` does persist a real mode change (Terminal mode
    // stays active until the pane is closed), so it's used here instead.
    /**
     * @brief Checks whether any event in a list has the given method name.
     * @param events The events to search.
     * @param method The event.* method name to look for.
     * @return true if some event's "method" equals `method`.
     */
    auto has_event = [](const std::vector<Json> &events, const std::string &method) {
        for (const Json &e : events) if (e.get("method").as_string() == method) return true;
        return false;
    };
    /**
     * @brief Finds the first event in a list with the given method name.
     * @param events The events to search.
     * @param method The event.* method name to look for.
     * @return A pointer to the first matching event, or nullptr if none match.
     */
    auto find_event = [](const std::vector<Json> &events, const std::string &method) -> const Json * {
        for (const Json &e : events)
            if (e.get("method").as_string() == method) return &e;
        return nullptr;
    };

    std::vector<Json> term_events;
    // Builds {cmd: "terminal"} inline as this call's params.
    Call(fd, 12, "command.run", [] { Json p = Json::Object(); p["cmd"] = "terminal"; return p; }(), &read_buf, &term_events);
    DrainEvents(fd, &read_buf, &term_events);
    CHECK_CTX(has_event(term_events, "event.paneFocusChanged"), "opening :terminal should push a pane focus change");
    const Json *mode_ev = find_event(term_events, "event.modeChanged");
    CHECK_CTX(mode_ev != nullptr, "opening :terminal should push a mode change to TERMINAL");
    CHECK_CTX(mode_ev->get("params").get("mode").as_string() == "TERMINAL", "mode=[" + mode_ev->get("params").get("mode").as_string() + "]");

    std::vector<Json> close_events;
    // Builds {cmd: "close"} inline as this call's params.
    Call(fd, 13, "command.run", [] { Json p = Json::Object(); p["cmd"] = "close"; return p; }(), &read_buf, &close_events);
    DrainEvents(fd, &read_buf, &close_events);
    const Json *back_to_normal = find_event(close_events, "event.modeChanged");
    CHECK_CTX(back_to_normal != nullptr && back_to_normal->get("params").get("mode").as_string() == "NORMAL",
              "closing the terminal pane should push a mode change back to NORMAL");

    std::vector<Json> notify_events;
    // Builds {cmd: "lua mep.notify(...)"} inline as this call's params.
    Call(fd, 14, "command.run",
         [] { Json p = Json::Object(); p["cmd"] = "lua mep.notify('m3 test notification', 'warn')"; return p; }(), &read_buf, &notify_events);
    DrainEvents(fd, &read_buf, &notify_events);
    const Json *notif = find_event(notify_events, "event.notification");
    CHECK_CTX(notif != nullptr, "mep.notify() should push event.notification");
    CHECK(notif->get("params").get("level").as_string() == "warn");
    CHECK(notif->get("params").get("message").as_string() == "m3 test notification");

    // A second, purely idle connection should also see events triggered
    // by the first connection's requests -- this is a push/broadcast to
    // every connected client, not a private reply channel to whoever
    // issued the request that caused the change.
    const int watcher_fd = ConnectOnce(socket_path);
    CHECK(watcher_fd >= 0);
    // buffer.insertText is guaranteed to move the cursor (advances by the
    // inserted text's length regardless of where it started) and bump
    // change_epoch_ (PushUndo runs unconditionally), so this reliably
    // produces at least one event regardless of whatever state earlier
    // steps left the cursor/mode in.
    std::vector<Json> actor_events;
    // Builds {text: "watcher-test"} inline as this call's params.
    Call(fd, 15, "buffer.insertText", [] { Json p = Json::Object(); p["text"] = "watcher-test"; return p; }(), &read_buf, &actor_events);
    std::vector<Json> watcher_events;
    std::string watcher_buf;
    DrainEvents(watcher_fd, &watcher_buf, &watcher_events);
    CHECK_CTX(!watcher_events.empty(), "idle watcher connection should have received a broadcast event without ever sending a request");
    close(watcher_fd);

    // --- Participant-scoped cursors (COLLAB_CURSORS_PLAN.md Phase 1b) ---
    // cursor.get/set and buffer.insertText/setLine/replaceLines/switch
    // now act on each *connection's own* independent virtual cursor, not
    // the human's real, shared Pane::cursor -- confirm two concurrent
    // connections don't disturb each other or the real active pane.
    const int fd2 = ConnectOnce(socket_path);
    CHECK(fd2 >= 0);
    std::string read_buf2;

    // Builds {name: "Agent A"} inline as this call's params.
    Json ident_a = Call(fd, 16, "session.identify", [] { Json p = Json::Object(); p["name"] = "Agent A"; return p; }(), &read_buf);
    CHECK(ident_a.get("result").get("participant_id").as_string() != "");
    // Builds {name: "Agent B"} inline as this call's params.
    Json ident_b = Call(fd2, 16, "session.identify", [] { Json p = Json::Object(); p["name"] = "Agent B"; return p; }(), &read_buf2);
    CHECK(ident_b.get("result").get("participant_id").as_string() != ident_a.get("result").get("participant_id").as_string());

    const Json real_pane_before = Call(fd, 17, "pane.get", Json::Object(), &read_buf);
    const int real_row_before = real_pane_before.get("result").get("cursor").get("row").as_int();
    const int real_col_before = real_pane_before.get("result").get("cursor").get("col").as_int();

    Json set_a = Json::Object();
    set_a["buffer_id"] = 0;
    set_a["row"] = 0;
    set_a["col"] = 0;
    Call(fd, 18, "cursor.set", set_a, &read_buf);
    // Builds {text: "AAAA"} inline as this call's params.
    Call(fd, 19, "buffer.insertText", [] { Json p = Json::Object(); p["text"] = "AAAA"; return p; }(), &read_buf);
    Call(fd2, 17, "cursor.set", set_a, &read_buf2);  // same target position, independent connection
    // Builds {text: "BBBB"} inline as this call's params.
    Call(fd2, 18, "buffer.insertText", [] { Json p = Json::Object(); p["text"] = "BBBB"; return p; }(), &read_buf2);

    const Json cursor_a = Call(fd, 20, "cursor.get", Json::Object(), &read_buf);
    const Json cursor_b = Call(fd2, 19, "cursor.get", Json::Object(), &read_buf2);
    CHECK_CTX(cursor_a.get("result").get("col").as_int() == 4, "Agent A's own cursor should have advanced by its own 4-char insert");
    CHECK_CTX(cursor_b.get("result").get("col").as_int() == 4, "Agent B's own cursor should have advanced by its own 4-char insert, independent of A");

    const Json real_pane_after = Call(fd, 21, "pane.get", Json::Object(), &read_buf);
    CHECK_CTX(real_pane_after.get("result").get("cursor").get("row").as_int() == real_row_before &&
                  real_pane_after.get("result").get("cursor").get("col").as_int() == real_col_before,
              "two agents editing buffer 0 should not have moved the real active pane's own cursor at all");

    const Json created = Call(fd, 22, "buffer.create", Json::Object(), &read_buf);
    const int new_buffer_id = created.get("result").get("buffer_id").as_int();
    Json switch_params = Json::Object();
    switch_params["buffer_id"] = new_buffer_id;
    Call(fd, 23, "buffer.switch", switch_params, &read_buf);
    const Json cursor_after_switch = Call(fd, 24, "cursor.get", Json::Object(), &read_buf);
    CHECK(cursor_after_switch.get("result").get("buffer_id").as_int() == new_buffer_id);
    const Json real_pane_after_switch = Call(fd, 25, "pane.get", Json::Object(), &read_buf);
    CHECK_CTX(real_pane_after_switch.get("result").get("buffer_id").as_int() != new_buffer_id,
              "buffer.switch should redirect only the calling connection's own cursor, not the real active pane's buffer");

    close(fd2);

    // --- Always-exists cursor, README-default (COLLAB_CURSORS_PLAN.md
    // "always exist for every registered agent" work) --- a freshly
    // identified agent that has never called cursor.set/buffer.insertText
    // should already have a real cursor location: row 0, col 0 of the
    // project's README, not merely "unset until it types something".
    const int fd3 = ConnectOnce(socket_path);
    CHECK(fd3 >= 0);
    std::string read_buf3;
    // Builds {name: "Agent C"} inline as this call's params.
    Json ident_c = Call(fd3, 1, "session.identify", [] { Json p = Json::Object(); p["name"] = "Agent C"; return p; }(), &read_buf3);
    CHECK(ident_c.get("result").get("participant_id").as_string() != "");
    const Json cursor_c = Call(fd3, 2, "cursor.get", Json::Object(), &read_buf3);
    CHECK_CTX(cursor_c.get("result").get("row").as_int() == 0 && cursor_c.get("result").get("col").as_int() == 0,
              "a freshly identified agent's default cursor should be row 0, col 0, not wherever the human's real pane happens to be");
    const int readme_buffer_id = cursor_c.get("result").get("buffer_id").as_int();
    Json filename_params = Json::Object();
    filename_params["buffer_id"] = readme_buffer_id;
    const Json readme_filename = Call(fd3, 3, "buffer.filename", filename_params, &read_buf3);
    std::string lower_name = readme_filename.get("result").get("filename").as_string();
    for (char &c : lower_name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    CHECK_CTX(lower_name.find("readme") != std::string::npos,
              "a freshly identified agent's default buffer should be the project README, got=[" + lower_name + "]");

    // Once the agent makes a real edit, EnsureConnCursorInitialized is a
    // no-op (cursor_buffer_id is already set) -- its cursor should track
    // that edit location from then on, not keep resetting to the readme.
    // Builds {text: "agent C was here"} inline as this call's params.
    Call(fd3, 4, "buffer.insertText", [] { Json p = Json::Object(); p["text"] = "agent C was here"; return p; }(), &read_buf3);
    const Json cursor_c_after_edit = Call(fd3, 5, "cursor.get", Json::Object(), &read_buf3);
    CHECK_CTX(cursor_c_after_edit.get("result").get("col").as_int() == 16,
              "after a real edit, the agent's cursor should reflect that edit, not reset to the readme default");
    CHECK(cursor_c_after_edit.get("result").get("buffer_id").as_int() == readme_buffer_id);

    // --- Agent status badge (COLLAB_CURSORS_PLAN.md Phase 1g) ---
    // The buffer.insertText call above already exercised the automatic
    // "writing" status; confirm it via session.listParticipants (the
    // same info main.cpp's tab-bar chip badge renders), then walk the
    // explicit states an agent can only ever report itself -- mep has no
    // way to observe "thinking" or "awaiting_input" from RPC traffic
    // alone.
    const std::string agent_c_id = ident_c.get("result").get("participant_id").as_string();
    /**
     * @brief Looks up agent C's reported status within a session.listParticipants response.
     * @param list_result The full JSON-RPC response from a session.listParticipants call.
     * @return Agent C's "status" field, or "<not found>" if it isn't present in the result.
     */
    auto find_participant_status = [&](const Json &list_result) -> std::string {
        for (const Json &p : list_result.get("result").items()) {
            if (p.get("id").as_string() == agent_c_id) return p.get("status").as_string();
        }
        return "<not found>";
    };

    const Json participants_after_write = Call(fd3, 6, "session.listParticipants", Json::Object(), &read_buf3);
    CHECK_CTX(find_participant_status(participants_after_write) == "writing",
              "buffer.insertText should auto-set status to \"writing\", got=[" + find_participant_status(participants_after_write) + "]");

    // Builds {status: "thinking"} inline as this call's params.
    Call(fd3, 7, "session.setStatus", [] { Json p = Json::Object(); p["status"] = "thinking"; return p; }(), &read_buf3);
    const Json participants_after_thinking = Call(fd3, 8, "session.listParticipants", Json::Object(), &read_buf3);
    CHECK_CTX(find_participant_status(participants_after_thinking) == "thinking",
              "explicit session.setStatus(\"thinking\") should override the earlier auto-\"writing\" status");

    // Builds {status: "awaiting_input"} inline as this call's params.
    Call(fd3, 9, "session.setStatus", [] { Json p = Json::Object(); p["status"] = "awaiting_input"; return p; }(), &read_buf3);
    // Builds {status: "done"} inline as this call's params.
    Json set_done_resp = Call(fd3, 10, "session.setStatus", [] { Json p = Json::Object(); p["status"] = "done"; return p; }(), &read_buf3);
    CHECK(set_done_resp.contains("result"));
    const Json participants_after_done = Call(fd3, 11, "session.listParticipants", Json::Object(), &read_buf3);
    CHECK_CTX(find_participant_status(participants_after_done) == "done",
              "session.setStatus(\"done\") should be the final reported status after awaiting_input");

    Json bad_status = Json::Object();
    bad_status["status"] = "not-a-real-status";
    const Json bad_status_resp = Call(fd3, 12, "session.setStatus", bad_status, &read_buf3);
    CHECK_CTX(bad_status_resp.contains("error"), "an unknown status value should be rejected, not silently accepted");
    CHECK(bad_status_resp.get("error").get("code").as_int() == -32602);

    close(fd3);

    // --- A stalled client must never stall the editor --------------------
    // The mep-mcp bridge only reads its socket while it's inside a tool
    // call; between Claude Code's tool calls nobody drains the event.*
    // notifications mep pushes to it. Before Connection got its
    // non-blocking outbox, that meant: a few hundred cursor moves after the
    // agent's last tool call, the kernel send buffer toward that client was
    // full and every later frame with a cursor move blocked the main thread
    // in send() for the full 200ms SO_SNDTIMEO -- "hold j/k for a couple of
    // seconds with an AI terminal open and the editor drops to ~5fps".
    // Model that client exactly: connect, never read, and keep moving the
    // human's cursor through a *different* connection while timing each
    // round trip. Each Call() here costs one editor frame; a stall of the
    // old kind shows up as ~200ms per call, so the bound below is generous
    // for any healthy machine (even a 60Hz vsync frame is ~17ms) while
    // still an order of magnitude under the failure mode.
    {
        const int stalled_fd = ConnectOnce(socket_path);
        CHECK(stalled_fd >= 0);

        // A buffer with enough lines that alternating between two rows
        // always genuinely moves the cursor (and so always emits an
        // event.cursorMoved), regardless of what earlier sections left on
        // screen.
        std::error_code ec;
        const std::string tall_path = (std::filesystem::temp_directory_path(ec) / ("mep-agent-rpc-test-" + std::to_string(static_cast<long>(getpid())) + ".txt")).string();
        {
            std::string tall;
            for (int i = 0; i < 200; i++) tall += "line " + std::to_string(i) + "\n";
            FILE *f = std::fopen(tall_path.c_str(), "w");
            CHECK(f != nullptr);
            CHECK(std::fwrite(tall.data(), 1, tall.size(), f) == tall.size());
            std::fclose(f);
        }
        Json open_tall = Call(fd, 27, "file.open", [&] { Json p = Json::Object(); p["path"] = tall_path; return p; }(), &read_buf);
        CHECK_CTX(open_tall.contains("result"), "file.open=[" + open_tall.dump() + "]");

        // Well past the point where a 212992-byte (Linux default) send
        // buffer full of ~100-byte notifications (each charged its skb
        // overhead, so roughly 300 of them) used to start blocking.
        constexpr int kMoves = 1200;
        constexpr double kMaxRoundTripMs = 100.0;
        double worst_ms = 0.0;
        double total_ms = 0.0;
        for (int i = 0; i < kMoves; i++) {
            const std::string cmd = "normal " + std::to_string(i % 2 == 0 ? 10 : 150) + "G";
            const auto t0 = std::chrono::steady_clock::now();
            Json moved = Call(fd, 28, "command.run", [&] { Json p = Json::Object(); p["cmd"] = cmd; return p; }(), &read_buf);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            CHECK(moved.contains("result"));
            worst_ms = std::max(worst_ms, ms);
            total_ms += ms;
            CHECK_CTX(ms < kMaxRoundTripMs,
                      "cursor move #" + std::to_string(i) + " took " + std::to_string(ms) +
                          "ms with a connected client that never reads -- the main thread is blocking on a full socket send buffer");
        }
        std::fprintf(stderr, "stalled-client section: %d cursor moves, worst %.1fms, mean %.2fms\n", kMoves, worst_ms, total_ms / kMoves);

        // The healthy connection kept getting its own events the whole
        // time -- shedding load toward the stalled peer must not affect
        // anyone else -- and still works after the stalled one goes away.
        std::vector<Json> after_events;
        Call(fd, 29, "command.run", [] { Json p = Json::Object(); p["cmd"] = "normal 100G"; return p; }(), &read_buf, &after_events);
        DrainEvents(fd, &read_buf, &after_events, 200);
        CHECK_CTX(has_event(after_events, "event.cursorMoved"), "healthy connection should still receive events while another client is stalled");
        close(stalled_fd);
        Json still_alive = Call(fd, 30, "session.info", Json::Object(), &read_buf);
        CHECK(still_alive.contains("result"));
        std::filesystem::remove(tall_path, ec);
    }

    // --- File tree delete/rename vs. open buffers: deleting a file that
    // has an open buffer (mep.fs_delete, the tree's 'd') and then creating
    // a new file at the same path must show the new, empty file -- not the
    // deleted file's content resurrected by FindOrCreateBuffer's dedup-by-
    // filename. Same for a rename ('r') followed by recreating the old name.
    {
        std::error_code ec;
        const std::string base = (std::filesystem::temp_directory_path(ec) / ("mep-agent-rpc-fs-" + std::to_string(static_cast<long>(getpid())))).string();
        std::filesystem::create_directories(base, ec);
        const std::string doomed = base + "/doomed.txt";
        const std::string renamed = base + "/renamed.txt";
        /**
         * @brief Writes `text` to `path`, replacing it.
         */
        auto write_file = [](const std::string &path, const std::string &text) {
            FILE *f = std::fopen(path.c_str(), "w");
            CHECK(f != nullptr);
            CHECK(std::fwrite(text.data(), 1, text.size(), f) == text.size());
            std::fclose(f);
        };
        /**
         * @brief Opens `path` in the active pane and returns that pane's buffer's lines.
         */
        auto open_and_read = [&](int id, const std::string &path) {
            Json opened = Call(fd, id, "file.open", [&] { Json p = Json::Object(); p["path"] = path; return p; }(), &read_buf);
            CHECK_CTX(opened.contains("result"), "file.open=[" + opened.dump() + "]");
            Json pane = Call(fd, id + 1, "pane.get", Json::Object(), &read_buf);
            Json lp = Json::Object();
            lp["buffer_id"] = pane.get("result").get("buffer_id").as_int();
            return Call(fd, id + 2, "buffer.getLines", lp, &read_buf).get("result").get("lines");
        };
        /**
         * @brief Runs a one-line Lua chunk through :lua.
         */
        auto lua = [&](int id, const std::string &chunk) {
            Json r = Call(fd, id, "command.run", [&] { Json p = Json::Object(); p["cmd"] = "lua " + chunk; return p; }(), &read_buf);
            CHECK_CTX(r.contains("result"), "lua " + chunk + " -> " + r.dump());
        };

        write_file(doomed, "old content\n");
        Json before = open_and_read(40, doomed);
        CHECK_CTX(before.items().size() >= 1 && before.items()[0].as_string() == "old content", "before=[" + before.dump() + "]");
        lua(43, "mep.fs_delete('" + doomed + "')");
        CHECK(!std::filesystem::exists(doomed));
        lua(44, "mep.fs_create_file('" + doomed + "')");
        Json recreated = open_and_read(45, doomed);
        CHECK_CTX(recreated.items().size() == 1 && recreated.items()[0].as_string().empty(),
                  "recreated file after fs_delete should be empty, got lines=[" + recreated.dump() + "]");

        write_file(doomed, "before rename\n");
        lua(48, "mep.buffer_delete(mep.current_buffer(), true)");  // :bd'd buffers must also re-read from disk
        Json reread = open_and_read(49, doomed);
        CHECK_CTX(reread.items().size() >= 1 && reread.items()[0].as_string() == "before rename", "reread=[" + reread.dump() + "]");
        // Rename while open: the same buffer (same id, still the pane's
        // active buffer, unsaved edits intact) now carries the new name,
        // and nothing in buffer.list still claims the old one.
        const int open_id = Call(fd, 52, "pane.get", Json::Object(), &read_buf).get("result").get("buffer_id").as_int();
        Call(fd, 53, "command.run", [] { Json p = Json::Object(); p["cmd"] = "normal Ounsaved"; return p; }(), &read_buf);
        lua(54, "mep.fs_rename('" + doomed + "', '" + renamed + "')");
        /**
         * @brief Returns buffer.list's filenames as one string, for CHECK context.
         */
        auto list_names = [&](int id) {
            std::vector<std::string> names;
            const Json listed = Call(fd, id, "buffer.list", Json::Object(), &read_buf);
            for (const Json &b : listed.get("result").items()) names.push_back(b.get("filename").as_string());
            return names;
        };
        /**
         * @brief Returns true if `names` contains `want`.
         */
        auto has_name = [](const std::vector<std::string> &names, const std::string &want) {
            return std::find(names.begin(), names.end(), want) != names.end();
        };
        std::vector<std::string> names = list_names(55);
        std::string names_ctx;
        for (const std::string &n : names) names_ctx += n + ";";
        CHECK_CTX(has_name(names, renamed) && !has_name(names, doomed), "buffer.list after rename=[" + names_ctx + "]");
        Json pane_after = Call(fd, 56, "pane.get", Json::Object(), &read_buf).get("result");
        CHECK_CTX(pane_after.get("buffer_id").as_int() == open_id, "pane.get after rename=[" + pane_after.dump() + "]");
        Json at_new = open_and_read(57, renamed);
        CHECK_CTX(at_new.items().size() == 2 && at_new.items()[0].as_string() == "unsaved" && at_new.items()[1].as_string() == "before rename",
                  "renamed buffer should keep its unsaved edit, got lines=[" + at_new.dump() + "]");
        CHECK(Call(fd, 60, "pane.get", Json::Object(), &read_buf).get("result").get("buffer_id").as_int() == open_id);
        lua(61, "mep.fs_create_file('" + doomed + "')");
        Json old_name = open_and_read(62, doomed);
        CHECK_CTX(old_name.items().size() == 1 && old_name.items()[0].as_string().empty(),
                  "recreated file after fs_rename should be empty, got lines=[" + old_name.dump() + "]");

        // A buffer already named after the destination but with nothing on
        // disk (an unsaved `:e fresh.txt`) must not shadow the renamed one.
        const std::string fresh = base + "/fresh.txt";
        open_and_read(65, fresh);
        lua(68, "mep.fs_rename('" + renamed + "', '" + fresh + "')");
        Json at_fresh = open_and_read(69, fresh);
        CHECK_CTX(at_fresh.items().size() == 2 && at_fresh.items()[0].as_string() == "unsaved",
                  "renaming onto a stale buffer's name should show the renamed buffer, got lines=[" + at_fresh.dump() + "]");
        CHECK(Call(fd, 72, "pane.get", Json::Object(), &read_buf).get("result").get("buffer_id").as_int() == open_id);
        names = list_names(71);  // and the stale one is gone, not merely outranked by a lower buffer id
        CHECK_CTX(std::count(names.begin(), names.end(), fresh) == 1, "expected exactly one " + fresh + " in buffer.list");

        // A buffer opened by a relative path keeps a relative name.
        const std::string test_cwd = std::filesystem::current_path(ec).string();
        write_file(base + "/rel.txt", "relative\n");
        lua(73, "mep.chdir('" + base + "')");
        open_and_read(74, "rel.txt");
        lua(77, "mep.fs_rename('" + base + "/rel.txt', '" + base + "/sub/rel2.txt')");  // sub/ doesn't exist yet
        CHECK(!std::filesystem::exists(base + "/sub/rel2.txt"));  // so the rename fails and nothing moves
        std::filesystem::create_directories(base + "/sub", ec);
        lua(78, "mep.fs_rename('" + base + "/rel.txt', '" + base + "/sub/rel2.txt')");
        names = list_names(79);
        names_ctx.clear();
        for (const std::string &n : names) names_ctx += n + ";";
        CHECK_CTX(has_name(names, "sub/rel2.txt") && !has_name(names, "rel.txt"), "buffer.list after relative rename=[" + names_ctx + "]");
        lua(80, "mep.chdir('" + test_cwd + "')");
        std::filesystem::remove_all(base, ec);
    }

    Json quit_params = Json::Object();
    quit_params["cmd"] = "qa!";
    Json quit_resp = Call(fd, 26, "command.run", quit_params, &read_buf);
    CHECK(quit_resp.contains("result"));
    close(fd);

    int status = 0;
    pid_t waited = waitpid(pid, &status, 0);
    CHECK(waited == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(!std::filesystem::exists(socket_path));

    // --- Workspaces (WORKSPACES_PLAN.md Phase 11): a second mep launched
    // inside a throwaway git repo. workspace.create must add a worktree on
    // a branch of the same name (asynchronously), state.dump must nest
    // projects[] -> workspaces[] -> tabs[], buffer.list must be scoped to
    // the active workspace, and a relative file.open must land inside the
    // worktree.
    {
        char tmpl[] = "/tmp/mep-ws-test-XXXXXX";
        const char *tmp = mkdtemp(tmpl);
        CHECK_CTX(tmp != nullptr, "mkdtemp failed");
        const std::string repo = tmp;
        auto sh = [&repo](const std::string &cmd) {
            const std::string full = "cd '" + repo + "' && " + cmd + " >/dev/null 2>&1";
            return std::system(full.c_str());
        };
        CHECK_CTX(sh("git init -q && git config user.email t@t && git config user.name t && "
                     "echo hello > README.md && git add README.md && git commit -q -m init") == 0,
                  "could not set up the temporary git repo in " + repo);

        pid_t pid2 = fork();
        CHECK(pid2 >= 0);
        if (pid2 == 0) {
            if (chdir(repo.c_str()) != 0) _exit(126);
            execl(argv[1], argv[1], "--no-session", nullptr);
            _exit(127);
        }
        const std::string socket2 = MepAgentSocketDir() + "/" + std::to_string(static_cast<long>(pid2)) + ".sock";
        int ws_fd = -1;
        for (int i = 0; i < 100 && ws_fd < 0; i++) {
            if (std::filesystem::exists(socket2)) ws_fd = ConnectOnce(socket2);
            if (ws_fd < 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        CHECK_CTX(ws_fd >= 0, "second mep never bound its agent socket within 10s");
        std::string ws_buf;

        std::error_code ec;
        const std::string repo_canon = std::filesystem::canonical(repo, ec).string();
        Json info = Call(ws_fd, 1, "session.info", Json::Object(), &ws_buf).get("result");
        CHECK_CTX(info.get("workspace").as_string() == "main", "session.info=[" + info.dump() + "]");
        CHECK_CTX(info.get("workspace_root").as_string() == repo_canon, "session.info=[" + info.dump() + "]");

        // Git detection is async (git rev-parse at startup): wait for it.
        bool is_git = false;
        for (int i = 0; i < 100 && !is_git; i++) {
            Json plist = Call(ws_fd, 2, "project.list", Json::Object(), &ws_buf).get("result");
            is_git = !plist.items().empty() && plist.items()[0].get("is_git").as_bool(false);
            if (!is_git) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        CHECK_CTX(is_git, "project never detected as a git repo");

        Json create = Call(ws_fd, 3, "workspace.create", [] { Json p = Json::Object(); p["name"] = "feat-x"; return p; }(), &ws_buf);
        CHECK_CTX(create.contains("result"), "workspace.create=[" + create.dump() + "]");
        CHECK(create.get("result").get("creating").as_bool(false));
        // Poll until `git worktree add` finished and the workspace became active.
        bool ready = false;
        for (int i = 0; i < 200 && !ready; i++) {
            Json wl = Call(ws_fd, 4, "workspace.list", Json::Object(), &ws_buf).get("result");
            for (const Json &w : wl.items()) {
                if (w.get("name").as_string() == "feat-x" && !w.get("creating").as_bool(true) && w.get("active").as_bool(false)) ready = true;
            }
            if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        CHECK_CTX(ready, "workspace feat-x never finished creating (git worktree add)");
        const std::string worktree = std::filesystem::path(repo_canon).parent_path().string() + "/" +
                                     std::filesystem::path(repo_canon).filename().string() + ".worktrees/feat-x";
        CHECK_CTX(std::filesystem::is_directory(worktree), "expected worktree dir at " + worktree);
        CHECK_CTX(sh("git worktree list | grep -q feat-x") == 0, "git worktree list should show feat-x");

        Json info2 = Call(ws_fd, 5, "session.info", Json::Object(), &ws_buf).get("result");
        CHECK_CTX(info2.get("workspace").as_string() == "feat-x", "session.info=[" + info2.dump() + "]");
        CHECK_CTX(info2.get("branch").as_string() == "feat-x", "session.info=[" + info2.dump() + "]");
        CHECK_CTX(info2.get("cwd").as_string() == std::filesystem::canonical(worktree, ec).string(),
                  "cwd should be the worktree, session.info=[" + info2.dump() + "]");

        // A relative open resolves against the worktree, scoped to it.
        Call(ws_fd, 6, "file.open", [] { Json p = Json::Object(); p["path"] = "README.md"; return p; }(), &ws_buf);
        Json scoped = Call(ws_fd, 7, "buffer.list", Json::Object(), &ws_buf).get("result");
        Json all = Call(ws_fd, 8, "buffer.list", [] { Json p = Json::Object(); p["workspace"] = "all"; return p; }(), &ws_buf).get("result");
        bool saw_readme = false;
        for (const Json &b : scoped.items()) {
            if (b.get("filename").as_string() == "README.md") saw_readme = true;
        }
        CHECK_CTX(saw_readme, "buffer.list (scoped)=[" + scoped.dump() + "]");
        CHECK_CTX(all.items().size() >= scoped.items().size(), "all=[" + all.dump() + "] scoped=[" + scoped.dump() + "]");

        Json ws_dump = Call(ws_fd, 9, "state.dump", Json::Object(), &ws_buf).get("result");
        CHECK_CTX(ws_dump.get("projects").items().size() == 1, "state.ws_dump=[" + ws_dump.dump() + "]");
        const Json &wss = ws_dump.get("projects").items()[0].get("workspaces");
        CHECK_CTX(wss.items().size() == 2, "state.ws_dump=[" + ws_dump.dump() + "]");
        CHECK(wss.items()[0].get("name").as_string() == "main");
        CHECK(wss.items()[1].get("name").as_string() == "feat-x");
        CHECK(wss.items()[1].get("tabs").items().size() >= 1);

        // Back to main: the worktree's buffer is hidden from the scoped list.
        Call(ws_fd, 10, "workspace.switch", [] { Json p = Json::Object(); p["name"] = "main"; return p; }(), &ws_buf);
        Json main_scoped = Call(ws_fd, 11, "buffer.list", Json::Object(), &ws_buf).get("result");
        bool leaked = false;
        for (const Json &b : main_scoped.items()) {
            if (b.get("filename").as_string() == "README.md") leaked = true;
        }
        CHECK_CTX(!leaked, "feat-x's README.md leaked into main's buffer.list=[" + main_scoped.dump() + "]");

        // Delete removes the worktree but keeps the branch (decision 5).
        Call(ws_fd, 12, "workspace.delete", [] { Json p = Json::Object(); p["name"] = "feat-x"; return p; }(), &ws_buf);
        bool removed = false;
        for (int i = 0; i < 200 && !removed; i++) {
            Json wl = Call(ws_fd, 13, "workspace.list", Json::Object(), &ws_buf).get("result");
            removed = wl.items().size() == 1;
            if (!removed) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        CHECK_CTX(removed, "workspace feat-x was never removed");
        CHECK_CTX(!std::filesystem::exists(worktree), "worktree dir should be gone: " + worktree);
        CHECK_CTX(sh("git branch --list feat-x | grep -q feat-x") == 0, "branch feat-x should survive workspace deletion");

        Json quit2 = Call(ws_fd, 14, "command.run", [] { Json p = Json::Object(); p["cmd"] = "qa!"; return p; }(), &ws_buf);
        CHECK(quit2.contains("result"));
        close(ws_fd);
        int status2 = 0;
        CHECK(waitpid(pid2, &status2, 0) == pid2);
        CHECK(WIFEXITED(status2) && WEXITSTATUS(status2) == 0);
        std::filesystem::remove_all(repo, ec);
        std::filesystem::remove_all(std::filesystem::path(repo).parent_path() / (std::filesystem::path(repo).filename().string() + ".worktrees"), ec);
    }

    std::printf("agent_rpc_test passed\n");
}
