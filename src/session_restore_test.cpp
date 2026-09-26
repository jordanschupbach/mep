// mep-session-restore-test: end-to-end cover for the session restore half
// of WORKSPACES_PLAN.md Phase 10 -- the thing no windowless test can reach,
// because a restored layout only exists in a *second* process that read the
// first one's session file. Builds a distinctive layout in one real mep
// (splits, a file per pane, a background buffer tab, a moved cursor, the
// file tree, a terminal), saves it, kills that mep, launches another on the
// same project, and asserts the split tree came back the same shape with the
// same files -- then corrupts the saved tree and asserts the next launch
// falls back to the default layout instead of a half-built one.
//
// Same live-display requirement (and the same CHECK()-never-assert()
// reasoning) as mep-agent-rpc-test, whose harness this shares. Every mep it
// spawns is pointed at a throwaway $XDG_DATA_HOME, so the real per-user
// session files are never read or written -- what `--no-session` does for
// the other live tests, which this one cannot use since the session file is
// the whole subject.
//
// Usage: mep-session-restore-test /path/to/mep

#include "agent_rpc_test_harness.h"
#include "workspace_git.h"

#include <csignal>
#include <fstream>
#include <map>

namespace {

// Every mep this test has spawned and not yet reaped. A failing CHECK ends
// the process through std::abort(), which runs no atexit handler, so without
// this a failure orphans a live mep window that also keeps the test's stdout
// pipe open -- making a perfectly clear abort message look like a hang to
// whoever is watching. The SIGABRT handler below closes that gap.
std::vector<pid_t> g_spawned;

void KillSpawnedOnAbort(int sig) {
    for (pid_t pid : g_spawned) {
        if (pid > 0) kill(pid, SIGKILL);
    }
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

// Where a spawned mep will put its agent socket and session files, given the
// throwaway data dir handed to it. Not MepAgentSocketDir(), which resolves
// *this* process's environment.
std::string SocketPathIn(const std::string &data_dir, pid_t pid) {
    return data_dir + "/mep/agent-sockets/" + std::to_string(static_cast<long>(pid)) + ".sock";
}

// Spawns `mep --project <project>` with `XDG_DATA_HOME=<data_dir>` and waits
// for its agent socket, returning the pid and a connected fd.
pid_t SpawnMep(const char *mep_path, const std::string &data_dir, const std::string &project, int *fd_out) {
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        // In the child only: the parent's own environment has to keep
        // pointing at the real data dir, since nothing here reads it but
        // everything else on this machine does.
        setenv("XDG_DATA_HOME", data_dir.c_str(), 1);
        execl(mep_path, mep_path, "--project", project.c_str(), nullptr);
        _exit(127);  // execl only returns on failure
    }
    g_spawned.push_back(pid);
    const std::string socket_path = SocketPathIn(data_dir, pid);
    int fd = -1;
    for (int i = 0; i < 200 && fd < 0; i++) {
        if (std::filesystem::exists(socket_path)) fd = ConnectOnce(socket_path);
        if (fd < 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    // Recorded before the socket wait's own CHECK, so a mep that starts but
    // never binds is cleaned up too.
    *fd_out = fd;
    CHECK_CTX(fd >= 0, "mep never bound its agent socket at " + socket_path + " within 20s");
    return pid;
}

// SIGKILL, not the `qa!` mep-agent-rpc-test uses: the layout under test has
// already been written by an explicit `:wssave`, and a clean quit would
// write it again from whatever state these assertions left behind.
void KillMep(pid_t pid, int fd) {
    close(fd);
    kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);
    g_spawned.erase(std::remove(g_spawned.begin(), g_spawned.end(), pid), g_spawned.end());
}

// The one workspace of the one project in a state.dump result.
Json OnlyWorkspace(const Json &dump) {
    CHECK_CTX(dump.get("projects").items().size() >= 1, "state.dump=[" + dump.dump() + "]");
    const Json &workspaces = dump.get("projects").items()[0].get("workspaces");
    CHECK_CTX(workspaces.items().size() >= 1, "state.dump=[" + dump.dump() + "]");
    return workspaces.items()[0];
}

// A split tree flattened to "<dir>(<child>,<child>)" / "leaf" so a whole
// layout's shape can be compared in one CHECK, with a readable failure.
std::string TreeShape(const Json &node) {
    if (node.get("dir").as_string("") == "leaf") return "leaf";
    std::string out = node.get("dir").as_string("") + "(";
    const std::vector<Json> &children = node.get("children").items();
    for (size_t i = 0; i < children.size(); i++) {
        if (i) out += ",";
        out += TreeShape(children[i]);
    }
    return out + ")";
}

// Every leaf's pane, left to right.
void CollectLeaves(const Json &node, std::vector<Json> *out) {
    if (node.get("dir").as_string("") == "leaf") {
        out->push_back(node.get("pane"));
        return;
    }
    for (const Json &child : node.get("children").items()) CollectLeaves(child, out);
}

// Maps the buffer ids of a state.dump to their filenames, so a restored
// pane can be identified by the file it shows (its *id* is deliberately
// fresh each run -- see Tab::id's comment).
std::map<int, std::string> BufferNames(int fd, int id, std::string *read_buf) {
    Json params = Json::Object();
    params["workspace"] = "all";
    const Json list = Call(fd, id, "buffer.list", params, read_buf).get("result");
    std::map<int, std::string> names;
    for (const Json &buffer : list.items()) names[buffer.get("id").as_int(-1)] = buffer.get("filename").as_string("");
    return names;
}

// The cursor row the session file records for the leaf whose saved `buffer`
// path ends in `suffix`, or -1. Read from the file rather than from a
// state.dump so the restore assertions compare against what was actually
// persisted: these tests run against a live display, where a stray keystroke
// or wheel event from whoever is at the machine can nudge a pane between the
// save and the next dump, and that must fail as "the save went wrong" rather
// than as "the restore came back wrong".
int SavedCursorRow(const Json &doc, const std::string &suffix) {
    std::function<int(const Json &)> walk = [&](const Json &node) {
        if (node.get("dir").as_string("") != "leaf") {
            for (const Json &child : node.get("children").items()) {
                const int found = walk(child);
                if (found >= 0) return found;
            }
            return -1;
        }
        const Json &pane = node.get("pane");
        const std::string file = pane.get("buffer").as_string("");
        if (file.size() < suffix.size() || file.compare(file.size() - suffix.size(), suffix.size(), suffix) != 0) return -1;
        const std::vector<Json> &cursor = pane.get("cursor").items();
        return cursor.size() == 2 ? cursor[0].as_int(-1) : -1;
    };
    const Json &workspaces = doc.get("workspaces");
    if (workspaces.items().empty()) return -1;
    const Json &tabs = workspaces.items()[0].get("tabs");
    if (tabs.items().empty()) return -1;
    return walk(tabs.items()[0].get("root"));
}

// The basename of the file a pane shows: "" for a terminal or an unnamed
// scratch buffer, which is what distinguishes a pane whose file went away.
std::string PaneFile(const Json &pane, const std::map<int, std::string> &names) {
    const auto it = names.find(pane.get("buffer_id").as_int(-1));
    if (it == names.end()) return "";
    return std::filesystem::path(it->second).filename().string();
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: mep-session-restore-test /path/to/mep\n");
        return 2;
    }
    const char *mep_path = argv[1];
    std::signal(SIGABRT, KillSpawnedOnAbort);

    // Short paths on purpose: a Unix-domain socket address is capped at ~108
    // bytes, and the agent socket lives at $XDG_DATA_HOME/mep/agent-sockets/
    // <pid>.sock -- a deep mkdtemp under a long TMPDIR overflows it, and mep
    // then starts fine but with no socket for this test to drive.
    char data_template[] = "/tmp/mep-sess-d-XXXXXX";
    char project_template[] = "/tmp/mep-sess-p-XXXXXX";
    const char *data_dir = mkdtemp(data_template);
    const char *project_dir = mkdtemp(project_template);
    CHECK(data_dir != nullptr && project_dir != nullptr);
    const std::string data(data_dir);
    const std::string project(project_dir);

    {
        std::ofstream readme(project + "/README.md");
        readme << "alpha\nbeta\ngamma\ndelta\nepsilon\nzeta\neta\ntheta\n";
        std::ofstream notes(project + "/notes.txt");
        notes << "one\ntwo\nthree\n";
        std::ofstream script(project + "/script.py");
        script << "x = 1\n";
    }

    std::error_code ec;
    const std::string project_canon = std::filesystem::canonical(project, ec).string();
    const std::string state_file = WorkspaceStatePath(data + "/mep", project_canon);

    // --- First run: build a layout worth restoring, and save it -------------
    std::string buf;
    int fd = -1;
    pid_t pid = SpawnMep(mep_path, data, project, &fd);

    // Whatever mep.project_default_layout built (a file-tree pane, a pane on
    // the readme, a terminal) is the starting point; this splits the readme
    // pane so the saved tree is deeper than any default layout can be, which
    // is what makes "the default layout was applied instead" detectable.
    const Json first = OnlyWorkspace(Call(fd, 1, "state.dump", Json::Object(), &buf).get("result"));
    CHECK_CTX(first.get("tabs").items().size() == 1, "expected one tab at startup, got [" + first.dump() + "]");
    std::vector<Json> start_leaves;
    CollectLeaves(first.get("tabs").items()[0].get("root"), &start_leaves);
    const std::map<int, std::string> start_names = BufferNames(fd, 2, &buf);
    int readme_pane = -1;
    for (const Json &pane : start_leaves) {
        if (PaneFile(pane, start_names) == "README.md") readme_pane = pane.get("id").as_int(-1);
    }
    CHECK_CTX(readme_pane >= 0, "no pane showing README.md in the default layout");

    Json focus = Json::Object();
    focus["pane_id"] = readme_pane;
    Call(fd, 3, "pane.focus", focus, &buf);
    Json split = Json::Object();
    split["dir"] = "vertical";
    split["file"] = "notes.txt";
    Call(fd, 4, "pane.split", split, &buf);
    // A second buffer tab in the new pane, so the restore has one to put
    // back: mep.pane_open adds a tab rather than replacing the pane's buffer
    // (which is what `:e` does), and leaves the newly added one showing.
    Json lua = Json::Object();
    lua["cmd"] = "lua mep.pane_open('script.py')";
    Call(fd, 5, "command.run", lua, &buf);
    // A real cursor move, last of all: `:6` is an ex line address, unlike
    // cursor.set, which moves this *connection's* agent cursor and not the
    // pane's (see EnsureConnCursorInitialized).
    focus["pane_id"] = readme_pane;
    Call(fd, 6, "pane.focus", focus, &buf);
    Json goto_line = Json::Object();
    goto_line["cmd"] = "6";
    Call(fd, 7, "command.run", goto_line, &buf);

    Json save = Json::Object();
    save["cmd"] = "wssave";
    Call(fd, 8, "command.run", save, &buf);
    CHECK_CTX(std::filesystem::exists(state_file, ec), ":wssave wrote no session file at " + state_file);

    const Json saved = OnlyWorkspace(Call(fd, 9, "state.dump", Json::Object(), &buf).get("result"));
    const Json &saved_tab = saved.get("tabs").items()[0];
    const std::string saved_shape = TreeShape(saved_tab.get("root"));
    std::vector<Json> saved_leaves;
    CollectLeaves(saved_tab.get("root"), &saved_leaves);
    const std::map<int, std::string> saved_names = BufferNames(fd, 10, &buf);
    std::vector<std::string> saved_files;
    for (const Json &pane : saved_leaves) saved_files.push_back(PaneFile(pane, saved_names));
    // The split really did deepen the tree past what the default layout
    // builds, or the comparison below proves nothing.
    CHECK_CTX(saved_shape != TreeShape(first.get("tabs").items()[0].get("root")),
              "the split didn't change the tree shape: " + saved_shape);
    // What the file itself holds is the contract the restore has to honour.
    Json saved_doc;
    CHECK_CTX(ReadJsonFile(state_file, &saved_doc), "could not read back " + state_file);
    const int saved_cursor_row = SavedCursorRow(saved_doc, "README.md");
    CHECK_CTX(saved_cursor_row == 5, "`:6` should have saved the readme cursor on row 5, got " +
                                         std::to_string(saved_cursor_row) +
                                         " (a stray keystroke into the test's mep window would do this)");
    KillMep(pid, fd);

    // --- Second run: the same layout, rebuilt from that file ---------------
    pid = SpawnMep(mep_path, data, project, &fd);
    const Json restored = OnlyWorkspace(Call(fd, 1, "state.dump", Json::Object(), &buf).get("result"));
    CHECK_CTX(restored.get("tabs").items().size() == 1, "restored=[" + restored.dump() + "]");
    const Json &restored_tab = restored.get("tabs").items()[0];
    CHECK_CTX(TreeShape(restored_tab.get("root")) == saved_shape,
              "restored tree shape [" + TreeShape(restored_tab.get("root")) + "] != saved [" + saved_shape + "]");

    std::vector<Json> restored_leaves;
    CollectLeaves(restored_tab.get("root"), &restored_leaves);
    const std::map<int, std::string> restored_names = BufferNames(fd, 2, &buf);
    std::vector<std::string> restored_files;
    for (const Json &pane : restored_leaves) restored_files.push_back(PaneFile(pane, restored_names));
    CHECK_CTX(restored_files == saved_files, "restored files differ from saved; restored one pane per saved pane but "
                                            "showing different buffers");
    // The file tree pane came back once, not twice: its saved path is a
    // directory, and letting LoadFile hand that to the on_directory_open
    // hook mid-rebuild used to add a second tree pane beside the restored
    // one (hence Editor::DirectoryPaneBuffer).
    int tree_panes = 0;
    for (const Json &pane : restored_leaves) {
        const auto it = restored_names.find(pane.get("buffer_id").as_int(-1));
        if (it != restored_names.end() && it->second == project_canon) tree_panes++;
    }
    CHECK_CTX(tree_panes == 1, "expected exactly one file-tree pane after restore, got " + std::to_string(tree_panes));
    // The cursor, and the background buffer tab (as a buffer that is open
    // without any pane showing it).
    int restored_cursor_row = -1;
    for (const Json &pane : restored_leaves) {
        if (PaneFile(pane, restored_names) == "README.md") restored_cursor_row = pane.get("cursor").get("row").as_int(-1);
    }
    CHECK_CTX(restored_cursor_row == saved_cursor_row, "readme cursor came back on row " +
                                                          std::to_string(restored_cursor_row) + ", saved as row " +
                                                          std::to_string(saved_cursor_row));
    bool notes_open = false;
    for (const auto &entry : restored_names) {
        if (std::filesystem::path(entry.second).filename().string() == "notes.txt") notes_open = true;
    }
    CHECK_CTX(notes_open, "notes.txt was a background buffer tab and should be open after restore");
    // Nothing shows a leftover placeholder: every restored pane is either on
    // a real file, the tree, or a terminal, and the throwaway empty buffers
    // the rebuild used were retired (Editor::ApplyPaneRestore).
    int unnamed_buffers = 0;
    for (const auto &entry : restored_names) {
        if (entry.second.empty()) unnamed_buffers++;
    }
    CHECK_CTX(unnamed_buffers <= 1, "restore left " + std::to_string(unnamed_buffers) +
                                        " unnamed buffers behind (expected at most the one terminal)");
    KillMep(pid, fd);

    // --- Third run: an invalid saved tree falls back to the default layout --
    // A split with no children: valid JSON, not a layout (see
    // ValidWorkspaceStateTree), which is exactly the case that must not be
    // rebuilt half-way.
    {
        Json doc;
        CHECK(ReadJsonFile(state_file, &doc));
        Json &root = doc["workspaces"].items()[0]["tabs"].items()[0]["root"];
        // Whatever the root is, make it a childless split.
        root = Json::Object();
        root["dir"] = "horizontal";
        root["children"] = Json::Array();
        CHECK(WriteJsonFile(state_file, doc));
    }
    pid = SpawnMep(mep_path, data, project, &fd);
    const Json fallback = OnlyWorkspace(Call(fd, 1, "state.dump", Json::Object(), &buf).get("result"));
    const std::string fallback_shape = TreeShape(fallback.get("tabs").items()[0].get("root"));
    CHECK_CTX(fallback_shape == TreeShape(first.get("tabs").items()[0].get("root")),
              "a malformed saved tree should fall back to the default layout, got [" + fallback_shape + "]");
    std::vector<Json> fallback_leaves;
    CollectLeaves(fallback.get("tabs").items()[0].get("root"), &fallback_leaves);
    const std::map<int, std::string> fallback_names = BufferNames(fd, 2, &buf);
    bool fallback_has_readme = false;
    for (const Json &pane : fallback_leaves) {
        if (PaneFile(pane, fallback_names) == "README.md") fallback_has_readme = true;
    }
    CHECK_CTX(fallback_has_readme, "the default layout should have opened README.md");
    KillMep(pid, fd);

    std::filesystem::remove_all(data, ec);
    std::filesystem::remove_all(project, ec);
    std::printf("session_restore_test passed\n");
    return 0;
}
