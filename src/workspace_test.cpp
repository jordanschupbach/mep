// mep-workspace-test: windowless unit tests for WORKSPACES_PLAN.md's pure
// helpers (workspace_git.cpp) and the session-file JSON shape. Links only
// workspace_git.cpp + json.h/persist.h, so it runs anywhere -- no raylib,
// no display, no git. CHECK(), never assert(): the Release build strips
// assert() (see agent_rpc_test.cpp's own comment on exactly this bug).

#include "json.h"
#include "persist.h"
#include "workspace_git.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
/**
 * @brief Prints a CHECK-failure message (with file/line) to stderr and aborts the process.
 */
void CheckFailed(const char *expression, const char *file, int line) {
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, file, line);
    std::abort();
}
}  // namespace

#define CHECK(cond) ((cond) ? (void)0 : CheckFailed(#cond, __FILE__, __LINE__))

int main() {
    // --- ValidWorkspaceName ------------------------------------------------
    CHECK(ValidWorkspaceName("main"));
    CHECK(ValidWorkspaceName("feat-login_2.0"));
    CHECK(!ValidWorkspaceName(""));
    CHECK(!ValidWorkspaceName(".hidden"));
    CHECK(!ValidWorkspaceName("-flag"));
    CHECK(!ValidWorkspaceName("a..b"));
    CHECK(!ValidWorkspaceName("x.lock"));
    CHECK(!ValidWorkspaceName("has space"));
    CHECK(!ValidWorkspaceName("slash/y"));
    CHECK(!ValidWorkspaceName("caret^"));

    // --- DeriveWorktreeDir --------------------------------------------------
    CHECK(DeriveWorktreeDir("/home/u/src/mep", "feat") == "/home/u/src/mep.worktrees/feat");
    CHECK(DeriveWorktreeDir("/home/u/src/mep", "feat", "") == "/home/u/src/mep.worktrees/feat");
    // Absolute override.
    CHECK(DeriveWorktreeDir("/home/u/src/mep", "feat", "/tmp/wt") == "/tmp/wt/feat");
    // Relative override is taken relative to the project root (and
    // normalised).
    CHECK(DeriveWorktreeDir("/home/u/src/mep", "feat", ".worktrees") == "/home/u/src/mep/.worktrees/feat");
    CHECK(DeriveWorktreeDir("/home/u/src/mep", "feat", "../wt/./x") == "/home/u/src/wt/x/feat");
    // Root "/" has no basename to derive from.
    CHECK(DeriveWorktreeDir("/", "feat") == "/repo.worktrees/feat");

    // --- ParseWorktreeList --------------------------------------------------
    {
        const std::string porcelain =
            "worktree /home/u/src/mep\n"
            "HEAD 0123456789abcdef0123456789abcdef01234567\n"
            "branch refs/heads/main\n"
            "\n"
            "worktree /home/u/src/mep.worktrees/feat\n"
            "HEAD 89abcdef0123456789abcdef0123456789abcdef\n"
            "branch refs/heads/feat\n"
            "locked\n"
            "\n"
            "worktree /home/u/src/detached\n"
            "HEAD fedcba9876543210fedcba9876543210fedcba98\n"
            "detached\n"
            "\n"
            "worktree /home/u/src/gone\n"
            "HEAD 1111111111111111111111111111111111111111\n"
            "branch refs/heads/gone\n"
            "prunable gitdir file points to non-existent location\n"
            "\n"
            "worktree /srv/bare.git\n"
            "bare\n"
            "\n";
        std::vector<WorktreeEntry> entries = ParseWorktreeList(porcelain);
        CHECK(entries.size() == 5);
        CHECK(entries[0].path == "/home/u/src/mep");
        CHECK(entries[0].branch == "main");
        CHECK(entries[0].head == "0123456789abcdef0123456789abcdef01234567");
        CHECK(!entries[0].detached && !entries[0].bare && !entries[0].prunable && !entries[0].locked);
        CHECK(entries[1].path == "/home/u/src/mep.worktrees/feat");
        CHECK(entries[1].branch == "feat");
        CHECK(entries[1].locked);
        CHECK(entries[2].detached);
        CHECK(entries[2].branch.empty());
        CHECK(entries[2].head == "fedcba9876543210fedcba9876543210fedcba98");
        CHECK(entries[3].prunable);
        CHECK(entries[3].branch == "gone");
        CHECK(entries[4].bare);
        CHECK(entries[4].head.empty());
    }
    // No trailing blank line, CRLF, and a missing separator between stanzas.
    {
        std::vector<WorktreeEntry> entries = ParseWorktreeList("worktree /a\r\nHEAD abc\r\nbranch refs/heads/x\r\nworktree /b\nbare");
        CHECK(entries.size() == 2);
        CHECK(entries[0].path == "/a" && entries[0].branch == "x" && entries[0].head == "abc");
        CHECK(entries[1].path == "/b" && entries[1].bare);
    }
    CHECK(ParseWorktreeList("").empty());
    CHECK(ParseWorktreeList("\n\n").empty());
    CHECK(ParseWorktreeList("HEAD abc\nbranch refs/heads/x\n").empty());  // stanza without `worktree`

    // --- ProjectSlug / ProjectHash / WorkspaceStatePath ----------------------
    CHECK(ProjectSlug("/home/u/src/mep") == "mep");
    CHECK(ProjectSlug("/home/u/src/My Project!") == "my-project");
    CHECK(ProjectSlug("/") == "root");
    CHECK(ProjectSlug("/x/...") == "project");
    CHECK(ProjectHash("/home/u/src/mep").size() == 8);
    CHECK(ProjectHash("/home/u/src/mep") == ProjectHash("/home/u/src/mep"));
    CHECK(ProjectHash("/home/u/src/mep") != ProjectHash("/home/u/work/mep"));
    // Same basename, different roots -> different files (decision 10).
    CHECK(WorkspaceStatePath("/data", "/home/u/src/mep") != WorkspaceStatePath("/data", "/home/u/work/mep"));
    CHECK(WorkspaceStatePath("/data", "/home/u/src/mep").rfind("/data/workspaces/mep-", 0) == 0);
    CHECK(WorkspaceStatePath("/data", "/home/u/src/mep").size() == std::string("/data/workspaces/mep-").size() + 8 + 5);

    // --- Session JSON round trip (the v1 schema Editor::WorkspaceStateJson
    // writes and RestoreWorkspaceState reads) through the same
    // Write/ReadJsonFile helpers, incl. a pane whose file no longer exists
    // (restore must skip it, so the schema has to survive the round trip
    // unchanged for the editor to notice).
    {
        Json doc = Json::Object();
        doc["version"] = 1;
        doc["root"] = "/abs/project";
        doc["active_workspace"] = "feat-x";
        Json ws = Json::Object();
        ws["name"] = "feat-x";
        ws["root"] = "/abs/project.worktrees/feat-x";
        ws["branch"] = "feat-x";
        ws["primary"] = false;
        ws["active_tab"] = 0;
        Json pane = Json::Object();
        pane["id"] = 3;
        pane["kind"] = "file";
        pane["buffer"] = "src/missing.cpp";
        pane["cursor"] = Json::Array();
        pane["cursor"].push_back(Json(12));
        pane["cursor"].push_back(Json(4));
        pane["scroll"] = 7;
        pane["buffer_tabs"] = Json::Array();
        pane["buffer_tabs"].push_back(Json("README.md"));
        Json leaf = Json::Object();
        leaf["dir"] = "leaf";
        leaf["pane"] = pane;
        Json term = Json::Object();
        term["dir"] = "leaf";
        Json tpane = Json::Object();
        tpane["id"] = 4;
        tpane["kind"] = "terminal";
        term["pane"] = tpane;
        Json split = Json::Object();
        split["dir"] = "horizontal";
        split["children"] = Json::Array();
        split["children"].push_back(leaf);
        split["children"].push_back(term);
        split["shares"] = Json::Array();
        split["shares"].push_back(Json(0.7));
        split["shares"].push_back(Json(0.3));
        Json tab = Json::Object();
        tab["active_pane"] = 3;
        tab["root"] = split;
        ws["tabs"] = Json::Array();
        ws["tabs"].push_back(tab);
        doc["workspaces"] = Json::Array();
        doc["workspaces"].push_back(ws);

        char tmpl[] = "/tmp/mep-ws-json-XXXXXX";
        const char *dir = mkdtemp(tmpl);
        CHECK(dir != nullptr);
        const std::string path = std::string(dir) + "/session.json";
        CHECK(WriteJsonFile(path, doc));
        Json back;
        CHECK(ReadJsonFile(path, &back));
        CHECK(back.get("version").as_int() == 1);
        CHECK(back.get("active_workspace").as_string() == "feat-x");
        const Json &w = back.get("workspaces").items()[0];
        CHECK(w.get("name").as_string() == "feat-x");
        CHECK(w.get("primary").as_bool(true) == false);
        const Json &root = w.get("tabs").items()[0].get("root");
        CHECK(root.get("dir").as_string() == "horizontal");
        CHECK(root.get("children").items().size() == 2);
        CHECK(root.get("shares").items()[0].as_double() > 0.69 && root.get("shares").items()[0].as_double() < 0.71);
        const Json &p0 = root.get("children").items()[0].get("pane");
        CHECK(p0.get("kind").as_string() == "file");
        CHECK(p0.get("buffer").as_string() == "src/missing.cpp");
        CHECK(p0.get("cursor").items()[0].as_int() == 12 && p0.get("cursor").items()[1].as_int() == 4);
        CHECK(p0.get("scroll").as_int() == 7);
        CHECK(p0.get("buffer_tabs").items()[0].as_string() == "README.md");
        CHECK(root.get("children").items()[1].get("pane").get("kind").as_string() == "terminal");
        CHECK(w.get("tabs").items()[0].get("active_pane").as_int() == 3);
        // The referenced file really is missing -- what restore keys off.
        CHECK(!std::filesystem::exists(w.get("root").as_string() + "/" + p0.get("buffer").as_string()));
        // Malformed JSON is reported as "no state", never a crash.
        std::ofstream(path, std::ios::trunc) << "{ this is not json";
        Json bad;
        CHECK(!ReadJsonFile(path, &bad));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    std::printf("workspace_test passed\n");
    return 0;
}
