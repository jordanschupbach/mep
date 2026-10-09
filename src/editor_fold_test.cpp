// mep-editor-fold-test: windowless reproduction harness for the folding
// bugs reported against Python buffers (offsets that creep in as you type,
// folds that swallow or drop lines you did not select, state that comes
// back wrong after a session restore).
//
// Drives a real Editor the way the user does -- `:normal` keystrokes and
// the public fold API -- rather than poking Buffer::folds directly, so a
// failure here is a failure a person can hit. No window: Editor's
// constructor only bootstraps a project and a theme, and nothing in the
// fold path touches the renderer.

#include "editor.h"

#include "gfx/backend.h"
#include "gfx/backend_native.h"
#include "treesitter.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Defined in main.cpp for the real binary (it reads the live window's font
// size); lua_env.cpp's mep.font_size() binding pulls it in from mep_core,
// so a windowless test has to supply it. Nothing on the fold path calls it.
float GetFontSizePx();
float GetFontSizePx() { return 16.0f; }
float GetCharWidthPx();
float GetCharWidthPx() { return 8.0f; }
void InvalidateOrgInlineImageTexture(const std::string &path);
void InvalidateOrgInlineImageTexture(const std::string &) {}
void SetFontSizePx(float px);
void SetFontSizePx(float) {}
std::string MathRenderFastPng(const std::string &tex, float size_px, const std::string &out_dir);
std::string MathRenderFastPng(const std::string &, float, const std::string &) { return std::string(); }

namespace {

int g_failures = 0;
int g_checks = 0;

void Report(bool ok, const std::string &what, const std::string &detail) {
    g_checks++;
    if (ok) {
        std::printf("  ok   %s\n", what.c_str());
        return;
    }
    g_failures++;
    std::printf("  FAIL %s\n       %s\n", what.c_str(), detail.c_str());
}

#define EXPECT(cond, what, detail) Report((cond), (what), (detail))

std::string FoldsToString(const std::vector<Fold> &folds) {
    std::string s;
    for (const Fold &f : folds) {
        s += "[" + std::to_string(f.start_row) + ".." + std::to_string(f.end_row) + " " +
             (f.closed ? "closed" : "open") + " " + f.provider + "] ";
    }
    if (s.empty()) s = "(none)";
    return s;
}

// A small but realistic Python file: two module-level functions and a
// class with two methods, so there are nested fold ranges to get wrong.
const char *kPythonSource =
    "import os\n"                       // row 0
    "import sys\n"                      // row 1
    "\n"                                // row 2
    "\n"                                // row 3
    "def alpha(x):\n"                   // row 4
    "    total = 0\n"                    // row 5
    "    for i in range(x):\n"           // row 6
    "        total += i\n"               // row 7
    "    return total\n"                 // row 8
    "\n"                                // row 9
    "\n"                                // row 10
    "def beta(y):\n"                     // row 11
    "    if y > 0:\n"                    // row 12
    "        return y\n"                 // row 13
    "    return -y\n"                    // row 14
    "\n"                                // row 15
    "\n"                                // row 16
    "class Gamma:\n"                     // row 17
    "    def one(self):\n"               // row 18
    "        return 1\n"                 // row 19
    "\n"                                // row 20
    "    def two(self):\n"               // row 21
    "        return 2\n"                 // row 22
    "\n"                                // row 23
    "\n"                                // row 24
    "print(alpha(3))\n"                  // row 25
    "print(beta(4))\n";                  // row 26

std::string WriteTempPython(const std::string &dir, const std::string &name) {
    std::filesystem::create_directories(dir);
    const std::string path = dir + "/" + name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << kPythonSource;
    out.close();
    return path;
}

// Where the harness' scratch files live.
std::string ScratchDir() {
    const char *env = std::getenv("MEP_FOLD_TEST_DIR");
    if (env && *env) return env;
    return (std::filesystem::temp_directory_path() / "mep-fold-test").string();
}

const Fold *FindFold(const std::vector<Fold> &folds, int start, int end) {
    for (const Fold &f : folds) {
        if (f.start_row == start && f.end_row == end) return &f;
    }
    return nullptr;
}

int CountFolds(const std::vector<Fold> &folds, int start, int end) {
    int n = 0;
    for (const Fold &f : folds) {
        if (f.start_row == start && f.end_row == end) n++;
    }
    return n;
}

}  // namespace

int main() {
    // No window is ever opened, but the gfx:: facade aborts the first time
    // anything reaches it with no backend installed -- and a few paths
    // Editor's constructor runs (theme/font bookkeeping) do. Installing the
    // native backend set only registers function pointers; it does not
    // touch X11 or GL until something actually asks for a window.
    gfx::SetBackends(gfx::ToBackends(gfx::CreateNativeBackendSet()));

    const std::string dir = ScratchDir();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    // ---------------------------------------------------------------- 1
    // Baseline: a manual fold over a hand-picked range stays exactly the
    // range that was picked, and hides exactly its interior.
    std::printf("\n== 1. a fresh manual fold covers exactly the chosen rows\n");
    {
        Editor ed;
        ed.LoadFile(WriteTempPython(dir, "t1.py"));
        // The body of `def alpha` as a user would select it: rows 4..8.
        ed.CreateFold(4, 8, /*closed=*/true, "manual");
        const std::vector<Fold> &folds = ed.CurrentBufferFolds();
        EXPECT(folds.size() == 1, "one fold exists", FoldsToString(folds));
        EXPECT(FindFold(folds, 4, 8) != nullptr, "fold is exactly 4..8", FoldsToString(folds));
        int hidden_start = -1;
        EXPECT(!ed.IsRowHiddenByFold(4, &hidden_start), "row 4 (summary) stays visible", "it was hidden");
        EXPECT(ed.IsRowHiddenByFold(8, &hidden_start), "row 8 (last folded row) is hidden", "it was visible");
        EXPECT(!ed.IsRowHiddenByFold(9, &hidden_start), "row 9 (after the fold) stays visible", "it was hidden");
    }

    // ---------------------------------------------------------------- 2
    // Typing above a fold: the fold must follow its text down.
    std::printf("\n== 2. inserting a line above a fold moves the fold with its text\n");
    {
        Editor ed;
        ed.LoadFile(WriteTempPython(dir, "t2.py"));
        ed.CreateFold(11, 14, /*closed=*/true, "manual");  // def beta
        ed.RunCommand("normal gg");
        ed.RunCommand("normal oimport json");  // insert a line at row 1
        const std::vector<Fold> &folds = ed.CurrentBufferFolds();
        EXPECT(FindFold(folds, 12, 15) != nullptr, "fold shifted 11..14 -> 12..15", FoldsToString(folds));
    }

    // ---------------------------------------------------------------- 3
    // The reported "it drifts as I keep editing" case: an edit followed by
    // an undo must leave the fold on the same text it started on.
    std::printf("\n== 3. undo after a line delete puts the fold back\n");
    {
        Editor ed;
        ed.LoadFile(WriteTempPython(dir, "t3.py"));
        ed.CreateFold(11, 14, /*closed=*/true, "manual");  // def beta
        const int lines_before = ed.LineCountForLua();
        ed.RunCommand("normal gg");
        ed.RunCommand("normal dd");  // delete `import os` (row 0)
        {
            const std::vector<Fold> &folds = ed.CurrentBufferFolds();
            EXPECT(FindFold(folds, 10, 13) != nullptr, "after dd the fold is 10..13", FoldsToString(folds));
        }
        ed.RunCommand("normal u");  // undo it
        EXPECT(ed.LineCountForLua() == lines_before, "undo restored the line count",
               std::to_string(ed.LineCountForLua()) + " != " + std::to_string(lines_before));
        {
            const std::vector<Fold> &folds = ed.CurrentBufferFolds();
            EXPECT(FindFold(folds, 11, 14) != nullptr, "after undo the fold is 11..14 again",
                   FoldsToString(folds) + " -- fold did not follow the undo");
            EXPECT(ed.GetLineForLua(11) == "def beta(y):", "row 11 is `def beta` again",
                   "row 11 is \"" + ed.GetLineForLua(11) + "\"");
        }
    }

    // ---------------------------------------------------------------- 4
    // The same drift, accumulated: five delete/undo rounds.
    std::printf("\n== 4. repeated edit+undo rounds do not accumulate drift\n");
    {
        Editor ed;
        ed.LoadFile(WriteTempPython(dir, "t4.py"));
        ed.CreateFold(17, 22, /*closed=*/true, "manual");  // class Gamma
        for (int round = 0; round < 5; round++) {
            ed.RunCommand("normal gg");
            ed.RunCommand("normal dd");
            ed.RunCommand("normal u");
        }
        const std::vector<Fold> &folds = ed.CurrentBufferFolds();
        EXPECT(FindFold(folds, 17, 22) != nullptr, "fold is still 17..22 after 5 rounds",
               FoldsToString(folds) + " -- expected [17..22]");
    }

    // ---------------------------------------------------------------- 5
    // Deleting a block then undoing it: the clamp in the shifter must not
    // permanently shrink a fold that ran past the (temporarily) shorter
    // file.
    std::printf("\n== 5. deleting past a fold's end then undoing keeps its span\n");
    {
        Editor ed;
        ed.LoadFile(WriteTempPython(dir, "t5.py"));
        ed.CreateFold(17, 22, /*closed=*/true, "manual");  // class Gamma
        ed.RunCommand("normal gg");
        ed.RunCommand("normal 20dd");  // delete 20 lines from the top
        ed.RunCommand("normal u");
        const std::vector<Fold> &folds = ed.CurrentBufferFolds();
        EXPECT(FindFold(folds, 17, 22) != nullptr, "fold survived the delete+undo as 17..22",
               FoldsToString(folds) + " -- expected [17..22]");
    }

    // ---------------------------------------------------------------- 6
    // Two folds over the same range must not stack up: a second identical
    // one makes za look like it does nothing (it toggles one copy while
    // the other keeps the rows hidden).
    std::printf("\n== 6. a duplicate fold over the same range does not shadow za\n");
    {
        Editor ed;
        ed.LoadFile(WriteTempPython(dir, "t6.py"));
        ed.CreateFold(4, 8, /*closed=*/true, "manual");
        ed.CreateFold(4, 8, /*closed=*/true, "manual");  // e.g. zf pressed twice
        EXPECT(CountFolds(ed.CurrentBufferFolds(), 4, 8) == 1, "identical folds are not duplicated",
               FoldsToString(ed.CurrentBufferFolds()));
        ed.ToggleFoldAtRow(4);  // za
        int hidden_start = -1;
        EXPECT(!ed.IsRowHiddenByFold(6, &hidden_start), "one za opens the fold",
               "row 6 still hidden by the fold starting at " + std::to_string(hidden_start) + "; folds: " +
                   FoldsToString(ed.CurrentBufferFolds()));
    }

    // ---------------------------------------------------------------- 7
    // A provider recompute must not silently reopen what the user closed
    // (this is what mep.syntax_fold does on every rebuild: it clears its
    // provider and recreates every range with closed=false).
    std::printf("\n== 7. a provider rebuild preserves the user's closed folds\n");
    {
        Editor ed;
        ed.LoadFile(WriteTempPython(dir, "t7.py"));
        // What mep.syntax_fold builds on first run: every range open.
        ed.CreateFold(4, 8, /*closed=*/false, "treesitter");
        ed.CreateFold(11, 14, /*closed=*/false, "treesitter");
        ed.CloseFoldsAtRow(4, /*recursive=*/false);  // the user closes one (zc)
        EXPECT(FindFold(ed.CurrentBufferFolds(), 4, 8) != nullptr &&
                   FindFold(ed.CurrentBufferFolds(), 4, 8)->closed,
               "the fold is closed before the rebuild", FoldsToString(ed.CurrentBufferFolds()));
        // The rebuild mep.syntax_fold performs verbatim.
        ed.ClearFoldsFromProvider("treesitter");
        ed.CreateFold(4, 8, /*closed=*/false, "treesitter");
        ed.CreateFold(11, 14, /*closed=*/false, "treesitter");
        const Fold *f = FindFold(ed.CurrentBufferFolds(), 4, 8);
        EXPECT(f != nullptr && f->closed, "the fold is still closed after the rebuild",
               FoldsToString(ed.CurrentBufferFolds()) + " -- the rebuild reopened it");
    }

    // ---------------------------------------------------------------- 8
    // Reloading the buffer from disk (`:e!`, or an external change picked
    // up on focus) must not leave folds pointing at rows that moved.
    std::printf("\n== 8. reloading the file from disk does not leave stale folds\n");
    {
        const std::string path = WriteTempPython(dir, "t8.py");
        Editor ed;
        ed.LoadFile(path);
        ed.CreateFold(17, 22, /*closed=*/true, "manual");  // class Gamma
        // The file grows by three lines at the top, outside the editor.
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << "# header\n# header\n# header\n" << kPythonSource;
        }
        ed.RunCommand("e!");
        EXPECT(ed.GetLineForLua(20) == "class Gamma:", "class Gamma moved to row 20",
               "row 20 is \"" + ed.GetLineForLua(20) + "\"");
        const std::vector<Fold> &folds = ed.CurrentBufferFolds();
        EXPECT(FindFold(folds, 17, 22) == nullptr, "the stale 17..22 fold was not kept verbatim",
               FoldsToString(folds) + " -- fold still claims rows 17..22, which are now `    def two` etc.");
        EXPECT(FindFold(folds, 20, 25) != nullptr, "the fold followed its text to 20..25",
               FoldsToString(folds));
        const Fold *moved = FindFold(folds, 20, 25);
        EXPECT(moved != nullptr && moved->closed, "and it is still collapsed", FoldsToString(folds));
    }

    // ---------------------------------------------------------------- 8b
    // The same reload, but the fold's own text is gone: dropping the fold
    // is right, leaving it pointing at whatever now occupies those rows is
    // not.
    std::printf("\n== 8b. a reload that deletes the folded text drops the fold\n");
    {
        const std::string path = WriteTempPython(dir, "t8b.py");
        Editor ed;
        ed.LoadFile(path);
        ed.CreateFold(17, 22, /*closed=*/true, "manual");  // class Gamma
        {
            // class Gamma deleted outright, file otherwise intact.
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << "import os\nimport sys\n\n\ndef alpha(x):\n    total = 0\n    for i in range(x):\n"
                << "        total += i\n    return total\n\n\ndef beta(y):\n    if y > 0:\n"
                << "        return y\n    return -y\n\n\nprint(alpha(3))\nprint(beta(4))\n";
        }
        ed.RunCommand("e!");
        const std::vector<Fold> &folds = ed.CurrentBufferFolds();
        EXPECT(folds.empty(), "the fold was dropped rather than left on unrelated rows",
               FoldsToString(folds));
    }

    // ---------------------------------------------------------------- 9
    // A closed fold's hidden interior counts as one displayed line for
    // j/k, and must stay consistent when two folds share a start row.
    std::printf("\n== 9. j/k steps over a closed fold as a single display line\n");
    {
        Editor ed;
        ed.LoadFile(WriteTempPython(dir, "t9.py"));
        ed.CreateFold(4, 8, /*closed=*/true, "manual");
        EXPECT(ed.StepVisibleRow(4, +1) == 9, "j from the summary row lands past the fold",
               "landed on " + std::to_string(ed.StepVisibleRow(4, +1)));
        EXPECT(ed.StepVisibleRow(9, -1) == 4, "k from below lands on the summary row",
               "landed on " + std::to_string(ed.StepVisibleRow(9, -1)));
    }

    // --------------------------------------------------------------- 10
    // Editing *inside* an open fold: the range must grow, and the rows
    // the user never selected must stay out of it.
    std::printf("\n== 10. typing inside a fold grows it; typing after it does not\n");
    {
        Editor ed;
        ed.LoadFile(WriteTempPython(dir, "t10.py"));
        ed.CreateFold(4, 8, /*closed=*/false, "manual");  // def alpha, open
        ed.RunCommand("normal 6gg");                       // row 5, inside the fold
        ed.RunCommand("normal ototal = 1");                // a line at row 6
        {
            const std::vector<Fold> &folds = ed.CurrentBufferFolds();
            EXPECT(FindFold(folds, 4, 9) != nullptr, "a line typed inside grows the fold to 4..9",
                   FoldsToString(folds));
        }
        // Now a line typed on the row *after* the fold must stay outside.
        ed.RunCommand("normal 10gg");  // row 9, the fold's new last row
        ed.RunCommand("normal o# after");
        {
            const std::vector<Fold> &folds = ed.CurrentBufferFolds();
            EXPECT(FindFold(folds, 4, 9) != nullptr, "a line typed after the fold leaves it at 4..9",
                   FoldsToString(folds) + " -- the fold swallowed a row below it");
        }
    }

    // --------------------------------------------------------------- 11
    // Pasting whole lines above a fold shifts it; pasting inside grows it.
    std::printf("\n== 11. paste above a fold shifts it rather than resizing it\n");
    {
        Editor ed;
        ed.LoadFile(WriteTempPython(dir, "t11.py"));
        ed.CreateFold(11, 14, /*closed=*/true, "manual");  // def beta
        ed.RunCommand("normal gg");
        ed.RunCommand("normal yy");
        ed.RunCommand("normal 3p");  // three copies of row 0 below it
        const std::vector<Fold> &folds = ed.CurrentBufferFolds();
        EXPECT(FindFold(folds, 14, 17) != nullptr, "fold shifted 11..14 -> 14..17", FoldsToString(folds));
    }

    // --------------------------------------------------------------- 12
    // Session round trip: what the user collapsed must come back on the
    // same text, and must not come back twice.
    std::printf("\n== 12. folds survive a save/restore round trip exactly once\n");
    {
        const std::string proj = dir + "/proj12";
        std::filesystem::create_directories(proj);
        const std::string path = WriteTempPython(proj, "t12.py");
        int saved_project = -1;
        {
            Editor ed;
            ed.LoadFile(path);
            ed.CreateFold(17, 22, /*closed=*/true, "manual");
            ed.CreateFold(4, 8, /*closed=*/true, "treesitter");
            saved_project = ed.ActiveProject().id;
            const bool ok = ed.SaveWorkspaceState(saved_project);
            EXPECT(ok, "session state saved", "SaveWorkspaceState returned false");
        }
        {
            Editor ed;
            ed.LoadFile(path);
            // What a provider would have rebuilt before the restore runs.
            ed.CreateFold(4, 8, /*closed=*/false, "treesitter");
            ed.RestoreFoldsOnly(ed.ActiveProject().id);
            const std::vector<Fold> &folds = ed.CurrentBufferFolds();
            EXPECT(CountFolds(folds, 4, 8) == 1, "the restored provider fold is not duplicated",
                   FoldsToString(folds));
            const Fold *f = FindFold(folds, 4, 8);
            EXPECT(f != nullptr && f->closed, "the provider fold came back closed", FoldsToString(folds));
            EXPECT(CountFolds(folds, 17, 22) == 1, "the manual fold came back exactly once",
                   FoldsToString(folds));
        }
    }

    // --------------------------------------------------------------- 13
    // The Python fold provider's own ranges: the rows the Treesitter query
    // hands mep.syntax_fold must be the rows the constructs actually
    // occupy, or every fold built from them starts out offset.
    std::printf("\n== 13. the Python Treesitter fold query returns the real row ranges\n");
    {
        EXPECT(TreesitterHasFoldQuery("py"), "py has a fold query", "no fold query registered for py");
        const std::vector<TSFoldRange> ranges = TreesitterFoldRanges("py", kPythonSource);
        std::string all;
        for (const TSFoldRange &r : ranges) all += "[" + std::to_string(r.start_row) + ".." + std::to_string(r.end_row) + "] ";
        bool has_alpha = false, has_beta = false, has_gamma = false;
        for (const TSFoldRange &r : ranges) {
            has_alpha = has_alpha || (r.start_row == 4 && r.end_row == 8);    // def alpha
            has_beta = has_beta || (r.start_row == 11 && r.end_row == 14);    // def beta
            has_gamma = has_gamma || (r.start_row == 17 && r.end_row == 22);  // class Gamma
        }
        EXPECT(has_alpha, "def alpha folds as 4..8", all);
        EXPECT(has_beta, "def beta folds as 11..14", all);
        EXPECT(has_gamma, "class Gamma folds as 17..22", all);
    }

    // --------------------------------------------------------------- 14
    // Real Python that makes the Treesitter query emit two captures over
    // the *same* rows -- nested brackets. mep.syntax_fold feeds each
    // capture straight to fold_create, so this is the duplicate-fold
    // shadow from check 6 arriving on its own, with no user mistake.
    std::printf("\n== 14. coincident Treesitter captures do not stack into shadowed folds\n");
    {
        const char *src =
            "matrix = [[\n"
            "    1, 2,\n"
            "]]\n";
        const std::vector<TSFoldRange> ranges = TreesitterFoldRanges("py", src);
        std::string all;
        int coincident = 0;
        for (const TSFoldRange &a : ranges) {
            all += "[" + std::to_string(a.start_row) + ".." + std::to_string(a.end_row) + "] ";
            for (const TSFoldRange &b : ranges) {
                if (&a != &b && a.start_row == b.start_row && a.end_row == b.end_row) coincident++;
            }
        }
        std::printf("       ranges: %s\n", all.c_str());
        // Whether or not the query happens to coincide here, the editor
        // must survive it: feed every range in, as mep.syntax_fold does.
        Editor ed;
        const std::string path = dir + "/t14.py";
        std::filesystem::create_directories(dir);
        { std::ofstream out(path, std::ios::binary | std::ios::trunc); out << src; }
        ed.LoadFile(path);
        for (const TSFoldRange &r : ranges) ed.CreateFold(r.start_row, r.end_row, /*closed=*/true, "treesitter");
        ed.ToggleFoldAtRow(0);  // za on the outermost
        int hidden_start = -1;
        EXPECT(!ed.IsRowHiddenByFold(1, &hidden_start),
               "one za opens the fold the provider built",
               "row 1 still hidden (fold starting at " + std::to_string(hidden_start) + "); " +
                   std::to_string(coincident) + " coincident captures; folds: " +
                   FoldsToString(ed.CurrentBufferFolds()));
    }

    // --------------------------------------------------------------- 15
    // Control: the marker provider does preserve what the user collapsed
    // across its own recompute (it looks the old fold up by start row).
    std::printf("\n== 15. control -- the marker provider keeps its closed state\n");
    {
        const std::string path = dir + "/t15.py";
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << "# region {{{\n" << "x = 1\n" << "y = 2\n" << "# }}}\n" << "z = 3\n";
        }
        Editor ed;
        ed.LoadFile(path);
        ed.RecomputeMarkerFolds();
        EXPECT(FindFold(ed.CurrentBufferFolds(), 0, 3) != nullptr, "marker fold built as 0..3",
               FoldsToString(ed.CurrentBufferFolds()));
        ed.CloseFoldsAtRow(0, false);
        ed.RecomputeMarkerFolds();
        const Fold *f = FindFold(ed.CurrentBufferFolds(), 0, 3);
        EXPECT(f != nullptr && f->closed, "still closed after the recompute",
               FoldsToString(ed.CurrentBufferFolds()));
    }

    // --------------------------------------------------------------- 16
    // A long editing session with no undo at all: pure insert/delete churn
    // above, inside and below the fold. This is the shifter's own job and
    // should hold -- it isolates the drift to the paths that skip it.
    std::printf("\n== 16. a long insert/delete session (no undo) keeps the fold on its text\n");
    {
        Editor ed;
        ed.LoadFile(WriteTempPython(dir, "t16.py"));
        ed.CreateFold(17, 22, /*closed=*/false, "manual");  // class Gamma
        for (int round = 0; round < 10; round++) {
            ed.RunCommand("normal gg");
            ed.RunCommand("normal o# churn");
            ed.RunCommand("normal gg");
            ed.RunCommand("normal dd");
        }
        const std::vector<Fold> &folds = ed.CurrentBufferFolds();
        const Fold *f = folds.empty() ? nullptr : &folds.front();
        const bool on_text = f != nullptr && ed.GetLineForLua(f->start_row) == "class Gamma:" &&
                             ed.GetLineForLua(f->end_row) == "        return 2";
        EXPECT(on_text, "the fold still spans exactly `class Gamma` .. `return 2`",
               FoldsToString(folds) + " start=\"" + (f ? ed.GetLineForLua(f->start_row) : std::string()) +
                   "\" end=\"" + (f ? ed.GetLineForLua(f->end_row) : std::string()) + "\"");
    }

    // --------------------------------------------------------------- 17
    // End to end, as the user hits it: fold something, edit and undo for a
    // while, quit (session save), reopen (session restore). What comes back
    // must be the fold that was made, not a drifted copy of it.
    std::printf("\n== 17. edit, undo, quit, reopen -- the fold comes back on its own text\n");
    {
        const std::string proj = dir + "/proj17";
        std::filesystem::create_directories(proj);
        const std::string path = WriteTempPython(proj, "t17.py");
        {
            Editor ed;
            ed.LoadFile(path);
            ed.CreateFold(17, 22, /*closed=*/true, "manual");  // class Gamma
            for (int round = 0; round < 3; round++) {
                ed.RunCommand("normal gg");
                ed.RunCommand("normal dd");
                ed.RunCommand("normal u");
            }
            ed.SaveWorkspaceState(ed.ActiveProject().id);
        }
        {
            Editor ed;
            ed.LoadFile(path);
            ed.RestoreFoldsOnly(ed.ActiveProject().id);
            const std::vector<Fold> &folds = ed.CurrentBufferFolds();
            const Fold *f = folds.empty() ? nullptr : &folds.front();
            const bool on_text = f != nullptr && ed.GetLineForLua(f->start_row) == "class Gamma:" &&
                                 ed.GetLineForLua(f->end_row) == "        return 2";
            EXPECT(on_text, "the restored fold spans `class Gamma` .. `return 2`",
                   FoldsToString(folds) + " start=\"" + (f ? ed.GetLineForLua(f->start_row) : std::string()) +
                       "\" end=\"" + (f ? ed.GetLineForLua(f->end_row) : std::string()) + "\"");
        }
    }

    // --------------------------------------------------------------- 18
    // zm/zr work off Buffer::fold_level. Vim's rule is "a fold whose level
    // is higher than 'foldlevel' is closed", so zm from zR closes the
    // *innermost* level first and a second zm takes the outer one -- this
    // check pins that down rather than assuming the other order.
    std::printf("\n== 18. zm closes one level at a time, innermost first (vim's foldlevel rule)\n");
    {
        Editor ed;
        ed.LoadFile(WriteTempPython(dir, "t18.py"));
        ed.CreateFold(17, 22, false, "manual");  // class Gamma
        ed.CreateFold(18, 19, false, "manual");  // def one, nested inside it
        ed.SetAllFoldsClosed(false);             // zR: everything open
        ed.AdjustFoldLevel(-1);                  // zm
        {
            const Fold *outer = FindFold(ed.CurrentBufferFolds(), 17, 22);
            const Fold *inner = FindFold(ed.CurrentBufferFolds(), 18, 19);
            EXPECT(inner != nullptr && inner->closed, "the first zm closes the nested fold",
                   FoldsToString(ed.CurrentBufferFolds()));
            EXPECT(outer != nullptr && !outer->closed, "the first zm leaves the outer fold open",
                   FoldsToString(ed.CurrentBufferFolds()));
        }
        ed.AdjustFoldLevel(-1);  // zm again
        {
            const Fold *outer = FindFold(ed.CurrentBufferFolds(), 17, 22);
            EXPECT(outer != nullptr && outer->closed, "the second zm closes the outer fold",
                   FoldsToString(ed.CurrentBufferFolds()));
        }
        ed.AdjustFoldLevel(1);  // zr
        {
            const Fold *outer = FindFold(ed.CurrentBufferFolds(), 17, 22);
            EXPECT(outer != nullptr && !outer->closed, "zr reopens the outer fold",
                   FoldsToString(ed.CurrentBufferFolds()));
        }
    }

    // --------------------------------------------------------------- 19
    // Joining two lines (J, and backspace at column 0) removes a row, so a
    // fold below must come up with it.
    std::printf("\n== 19. J above a fold brings it up one row\n");
    {
        Editor ed;
        ed.LoadFile(WriteTempPython(dir, "t19.py"));
        ed.CreateFold(17, 22, true, "manual");  // class Gamma
        ed.RunCommand("normal gg");
        ed.RunCommand("normal J");  // join rows 0 and 1
        const std::vector<Fold> &folds = ed.CurrentBufferFolds();
        EXPECT(FindFold(folds, 16, 21) != nullptr, "fold shifted 17..22 -> 16..21", FoldsToString(folds));
    }

    // --------------------------------------------------------------- 20
    // A delete that straddles a fold's *tail*: the fold must shrink to the
    // lines of its own that survived, not reach down and annex the first
    // line after the deleted block.
    std::printf("\n== 20. a delete overlapping a fold's end does not annex the line after it\n");
    {
        Editor ed;
        ed.LoadFile(WriteTempPython(dir, "t20.py"));
        ed.CreateFold(11, 14, /*closed=*/false, "manual");  // def beta, rows 11..14
        ed.RunCommand("normal 14gg");                       // row 13
        ed.RunCommand("normal 4dd");                        // delete rows 13..16
        const std::vector<Fold> &folds = ed.CurrentBufferFolds();
        const Fold *f = folds.empty() ? nullptr : &folds.front();
        // Rows 11..12 (`def beta` / `if y > 0:`) are all that is left of it.
        EXPECT(f != nullptr && f->start_row == 11 && f->end_row == 12,
               "the fold shrank to its surviving rows 11..12",
               FoldsToString(folds) + " end row holds \"" + (f ? ed.GetLineForLua(f->end_row) : std::string()) + "\"");
        EXPECT(f != nullptr && ed.GetLineForLua(f->end_row) == "    if y > 0:",
               "the fold's last row is still its own text",
               "it is \"" + (f ? ed.GetLineForLua(f->end_row) : std::string()) + "\"");
    }

    // --------------------------------------------------------------- 21
    // A delete that straddles a fold's *head*: the surviving tail keeps
    // exactly the rows it moved to.
    std::printf("\n== 21. a delete overlapping a fold's start keeps the surviving tail\n");
    {
        Editor ed;
        ed.LoadFile(WriteTempPython(dir, "t21.py"));
        ed.CreateFold(11, 14, /*closed=*/false, "manual");  // def beta, rows 11..14
        ed.RunCommand("normal 10gg");                       // row 9
        ed.RunCommand("normal 4dd");                        // delete rows 9..12
        const std::vector<Fold> &folds = ed.CurrentBufferFolds();
        const Fold *f = folds.empty() ? nullptr : &folds.front();
        const bool ok = f != nullptr && ed.GetLineForLua(f->start_row) == "        return y" &&
                        ed.GetLineForLua(f->end_row) == "    return -y";
        EXPECT(ok, "the fold spans exactly the two surviving rows",
               FoldsToString(folds) + " start=\"" + (f ? ed.GetLineForLua(f->start_row) : std::string()) +
                   "\" end=\"" + (f ? ed.GetLineForLua(f->end_row) : std::string()) + "\"");
    }

    // --------------------------------------------------------------- 22
    // mepml view mode (<leader>kv, Buffer::mepml_view): the cursor's row
    // is rendered like every other -- its markup concealed, its wrap
    // length the collapsed one -- rather than shown as its source; and
    // entering it leaves raw text (Buffer::mepml_raw).
    std::printf("\n== 22. mepml view mode renders the cursor's own row\n");
    {
        std::filesystem::create_directories(dir);
        const std::string path = dir + "/t22.mepml";
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << "//? Title: View\n"
                   "\n"
                   "> Heading\n"
                   "\n"
                   "Some *bold* and ~italic~ text here.\n"  // row 4
                   "\n"
                   "Another ~italic~ line.\n";  // row 6
        }
        Editor ed;
        ed.LoadFile(path);
        const int ns = ed.CreateNamespace("fold-test-mepml");
        ed.RunCommand("normal 5gg");  // the marked-up row, 4
        const int row = 4;
        const std::string &raw_line = ed.GetLineForLua(row);
        auto concealed_on = [&](int r) {
            const Buffer &buf = ed.CurrentBuffer();
            auto it = buf.decorations.find(ns);
            if (it == buf.decorations.end()) return 0;
            int n = 0;
            for (const Decoration &d : it->second)
                if (d.row == r && d.conceal) n++;
            return n;
        };
        ed.MepmlScan(ns);
        EXPECT(!ed.MepmlView(ed.CurrentBufferId()), "a document opens out of view mode", "it was in it");
        EXPECT(concealed_on(row) == 0, "editing: the cursor's row shows its markup",
               std::to_string(concealed_on(row)) + " conceal decorations on it");
        EXPECT(concealed_on(6) > 0, "editing: another marked-up row conceals its markup",
               "no conceal decoration on row 6");
        EXPECT(ed.WrapLenForRow(ed.CurrentBuffer(), row, row) == static_cast<int>(raw_line.size()),
               "editing: the cursor's row wraps at its raw length",
               std::to_string(ed.WrapLenForRow(ed.CurrentBuffer(), row, row)));

        EXPECT(ed.MepmlToggleView(), "the toggle enters view mode", "it returned false");
        EXPECT(ed.MepmlView(ed.CurrentBufferId()), "the buffer reports view mode", "it did not");
        ed.MepmlScan(ns);
        EXPECT(concealed_on(row) > 0, "view mode: the cursor's row conceals its markup like any other",
               "no conceal decoration on it");
        const int collapsed = ed.WrapLenForRow(ed.CurrentBuffer(), row, row);
        EXPECT(collapsed > 0 && collapsed < static_cast<int>(raw_line.size()),
               "view mode: the cursor's row wraps at its collapsed length",
               std::to_string(collapsed) + " vs raw " + std::to_string(raw_line.size()));
        // Moving the cursor changes nothing drawn.
        ed.RunCommand("normal 7gg");
        ed.MepmlScan(ns);
        EXPECT(concealed_on(row) > 0, "view mode: a row the cursor left stays rendered", "it lost its concealment");
        EXPECT(concealed_on(6) > 0, "view mode: the row the cursor is now on is rendered too",
               "no conceal decoration on row 6");

        EXPECT(!ed.MepmlToggleView(), "the toggle leaves view mode", "it returned true");
        ed.RunCommand("normal 5gg");
        ed.MepmlScan(ns);
        EXPECT(concealed_on(row) == 0, "editing again: the cursor's row shows its markup",
               std::to_string(concealed_on(row)) + " conceal decorations on it");

        // Raw text and view mode: entering the view leaves raw.
        EXPECT(ed.MepmlToggleRaw(), "raw text on", "it returned false");
        EXPECT(ed.MepmlToggleView(), "view mode on over raw", "it returned false");
        EXPECT(!ed.MepmlRaw(ed.CurrentBufferId()), "entering view mode turned raw text off", "raw is still on");
        ed.MepmlScan(ns);
        EXPECT(concealed_on(row) > 0, "and the document renders", "no conceal decoration on the cursor's row");
    }

    // --------------------------------------------------------------- 23
    // The same view mode for an Org and a Markdown document
    // (:OrgViewToggle, :MarkdownViewToggle -- Buffer::mepml_view): their
    // scans conceal the cursor's row too, and draw from the default sheet
    // -- a heading's size, a bullet's glyph, an emphasis' weight -- as the
    // mepml scan does, so a document and its exports look alike.
    std::printf("\n== 23. org and markdown view modes render the cursor's own row from the sheet\n");
    {
        std::filesystem::create_directories(dir);
        const std::string org_path = dir + "/t23.org", md_path = dir + "/t23.md";
        {
            std::ofstream out(org_path, std::ios::binary | std::ios::trunc);
            out << "* Heading\n"
                   "\n"
                   "Some *bold* and ~code~ text here.\n"  // row 2
                   "\n"
                   "- an item\n";  // row 4
        }
        {
            std::ofstream out(md_path, std::ios::binary | std::ios::trunc);
            out << "# Heading\n"
                   "\n"
                   "Some **bold** and `code` text here.\n"  // row 2
                   "\n"
                   "- an item\n";  // row 4
        }
        auto overlays_on = [](const Editor &ed, int ns, int r) {
            const Buffer &buf = ed.CurrentBuffer();
            auto it = buf.decorations.find(ns);
            if (it == buf.decorations.end()) return 0;
            int n = 0;
            for (const Decoration &d : it->second)
                if (d.row == r && d.virt_overlay && !d.virt_text.empty()) n++;
            return n;
        };
        auto bold_on = [](const Editor &ed, int ns, int r) {
            const Buffer &buf = ed.CurrentBuffer();
            auto it = buf.decorations.find(ns);
            if (it == buf.decorations.end()) return false;
            for (const Decoration &d : it->second)
                if (d.row == r && d.bold) return true;
            return false;
        };
        {
            Editor ed;
            ed.LoadFile(org_path);
            const int ns = ed.CreateNamespace("fold-test-org");
            ed.RunCommand("normal 3gg");  // the marked-up row, 2
            ed.OrgHighlightEmphasis(ns);
            EXPECT(overlays_on(ed, ns, 2) == 0, "org, editing: the cursor's row shows its markup",
                   std::to_string(overlays_on(ed, ns, 2)) + " overlays on it");
            EXPECT(overlays_on(ed, ns, 4) == 1, "org, editing: a bullet elsewhere is drawn as the sheet's marker",
                   std::to_string(overlays_on(ed, ns, 4)) + " overlays on row 4");
            EXPECT(ed.MepmlToggleView(), "org: the toggle enters view mode", "it returned false");
            ed.ClearNamespace(ns);
            ed.OrgHighlightEmphasis(ns);
            EXPECT(overlays_on(ed, ns, 2) == 2, "org, view mode: the cursor's row conceals its *bold* and ~code~",
                   std::to_string(overlays_on(ed, ns, 2)) + " overlays on it");
            EXPECT(bold_on(ed, ns, 2), "org: `bold` is bold, as the default sheet says", "no bold decoration on row 2");
            const Buffer &buf = ed.CurrentBuffer();
            const auto scale = buf.mepml_heading_scale.find(0);
            EXPECT(scale != buf.mepml_heading_scale.end() && scale->second > 1.59f && scale->second < 1.61f,
                   "org: the headline's size is heading[level=1]'s (1.6)",
                   scale == buf.mepml_heading_scale.end() ? "no size recorded" : std::to_string(scale->second));
            EXPECT(!ed.MepmlToggleView(), "org: the toggle leaves view mode", "it returned true");
        }
        {
            Editor ed;
            ed.LoadFile(md_path);
            const int ns = ed.CreateNamespace("fold-test-md");
            ed.RunCommand("normal 3gg");  // the marked-up row, 2
            ed.MdConceal(ns);
            EXPECT(overlays_on(ed, ns, 2) == 0, "markdown, editing: the cursor's row shows its markup",
                   std::to_string(overlays_on(ed, ns, 2)) + " overlays on it");
            EXPECT(overlays_on(ed, ns, 4) == 1, "markdown, editing: a bullet elsewhere is drawn as the sheet's marker",
                   std::to_string(overlays_on(ed, ns, 4)) + " overlays on row 4");
            EXPECT(Editor::HeadingLevelForRow(ed.CurrentBuffer(), 0) == 1, "markdown: `# Heading` is a level-1 heading",
                   std::to_string(Editor::HeadingLevelForRow(ed.CurrentBuffer(), 0)));
            EXPECT(ed.MepmlToggleView(), "markdown: the toggle enters view mode", "it returned false");
            ed.ClearNamespace(ns);
            ed.MdConceal(ns);
            EXPECT(overlays_on(ed, ns, 2) == 2, "markdown, view mode: the cursor's row conceals its **bold** and `code`",
                   std::to_string(overlays_on(ed, ns, 2)) + " overlays on it");
            EXPECT(bold_on(ed, ns, 2), "markdown: `bold` is bold, as the default sheet says", "no bold decoration on row 2");
            const OrgHeadingStyle hs = Editor::HeadingStyleForRow(ed.CurrentBuffer(), 0);
            EXPECT(hs.scale > 1.59f && hs.scale < 1.61f && hs.slots == 2, "markdown: the heading is drawn at 1.6 over two slots",
                   std::to_string(hs.scale) + " x" + std::to_string(hs.slots));
            EXPECT(!ed.MepmlToggleView(), "markdown: the toggle leaves view mode", "it returned true");
        }
    }

    // mepml tabs (\tabs, Editor::RecomputeMepmlTabFolds): one tab of a set
    // shown, the others folded into the strip on the set's opening row and
    // the rule on the shown tab's closing one; switching moves the folds.
    std::printf("\n== 24. mepml tabs show one tab at a time\n");
    {
        std::filesystem::create_directories(dir);
        const std::string path = dir + "/t24.mepml";
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << "Intro.\n"       // 0
                   "\n"             // 1
                   "\\tabs(\n"      // 2
                   "\\tab(C++,\n"   // 3
                   "int a;\n"       // 4
                   ")\n"            // 5
                   "\\tab(Python,\n"  // 6
                   "> Heading\n"    // 7
                   "a = 1\n"        // 8
                   ")\n"            // 9
                   "\\tab(R,\n"     // 10
                   "a <- 1\n"       // 11
                   ")\n"            // 12
                   ")\n"            // 13
                   "\n"             // 14
                   "After.\n";      // 15
        }
        Editor ed;
        ed.LoadFile(path);
        const int ns = ed.CreateNamespace("fold-test-tabs");
        ed.MepmlScan(ns);
        auto tab_folds = [&] {
            std::vector<std::pair<int, int>> v;
            for (const Fold &f : ed.CurrentBufferFolds())
                if (f.provider == "mepml-tabs" && f.closed) v.push_back({f.start_row, f.end_row});
            return v;
        };
        auto show = [](const std::vector<std::pair<int, int>> &v) {
            std::string o;
            for (const auto &p : v) o += "[" + std::to_string(p.first) + "," + std::to_string(p.second) + "]";
            return o;
        };
        using Ranges = std::vector<std::pair<int, int>>;
        EXPECT((tab_folds() == Ranges{{2, 3}, {5, 13}}), "the first tab is shown: the strip folds to its opener, the rule to the set's end",
               show(tab_folds()));
        const Buffer &buf = ed.CurrentBuffer();
        const auto strip = buf.mepml_tab_rows.find(2);
        EXPECT(strip != buf.mepml_tab_rows.end() && !strip->second.footer && strip->second.active == 0 &&
                   (strip->second.titles == std::vector<std::string>{"C++", "Python", "R"}),
               "the strip row lists the three titles, the first shown", "no such row");
        EXPECT(buf.mepml_tab_rows.count(5) == 1 && buf.mepml_tab_rows.at(5).footer, "the shown tab's closing row is the rule",
               "no rule row at 5");
        EXPECT(ed.MepmlSelectTab(0, 1), "selecting the second tab", "no set 0");
        EXPECT((tab_folds() == Ranges{{2, 6}, {9, 13}}), "the second tab is shown", show(tab_folds()));
        // (The heading inside it folds no further than its tab's end.)
        bool heading_ok = false;
        for (const Fold &f : ed.CurrentBufferFolds())
            if (f.provider == "mepml" && f.start_row == 7) heading_ok = f.end_row <= 8;
        EXPECT(heading_ok, "a heading in a tab folds within the tab", "it ran past it");
        EXPECT(ed.MepmlSelectTab(0, 3), "selecting past the end wraps around", "no set 0");
        EXPECT((tab_folds() == Ranges{{2, 3}, {5, 13}}), "back on the first tab", show(tab_folds()));
        ed.SetCursorForLua(2, 0);
        EXPECT(ed.MepmlCycleTab(-1) == "R", "cycling back from the first tab shows the last", "another title");
        EXPECT((tab_folds() == Ranges{{2, 10}, {12, 13}}), "the last tab is shown", show(tab_folds()));
        EXPECT(!ed.MepmlSelectTab(1, 0), "there is no second set", "it found one");
        // The cursor in a tab when the folds are built (a set just
        // written) shows that tab.
        Editor ed2;
        ed2.LoadFile(path);
        ed2.RunCommand("normal 9gg");
        ed2.MepmlScan(ed2.CreateNamespace("fold-test-tabs"));
        Ranges shown2;
        for (const Fold &f : ed2.CurrentBufferFolds())
            if (f.provider == "mepml-tabs" && f.closed) shown2.push_back({f.start_row, f.end_row});
        EXPECT((shown2 == Ranges{{2, 6}, {9, 13}}), "the cursor's tab is the one shown", show(shown2));
    }

    // Stepping through a set (Editor::MepmlTabStep): j off the last line of
    // a tab shows the next one, k off the first line the previous one; in
    // view mode the set is passed over, its tabs left as they are.
    std::printf("\n== 25. mepml tabs are stepped through one at a time\n");
    {
        const std::string path = dir + "/t25.mepml";
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << "Intro.\n"       // 0
                   "\\tabs(\n"      // 1
                   "\\tab(A,\n"     // 2
                   "a1\n"           // 3
                   "a2\n"           // 4
                   ")\n"            // 5
                   "\\tab(B,\n"     // 6
                   "b1\n"           // 7
                   ")\n"            // 8
                   "\\tab(C,\n"     // 9
                   "c1\n"           // 10
                   "c2\n"           // 11
                   ")\n"            // 12
                   ")\n"            // 13
                   "After.\n";      // 14
        }
        Editor ed;
        ed.LoadFile(path);
        ed.MepmlScan(ed.CreateNamespace("fold-test-tab-steps"));
        auto at = [&] {
            int r = 0, c = 0;
            ed.GetCursorForLua(&r, &c);
            const auto strip = ed.CurrentBuffer().mepml_tab_rows.find(1);
            const int active = strip == ed.CurrentBuffer().mepml_tab_rows.end() ? -1 : strip->second.active;
            return std::to_string(r) + "/" + std::to_string(active);
        };
        // row/shown tab after each j from the top: the strip, A's two lines,
        // B's line, C's two lines, the rule under C, then past the set.
        const std::vector<std::string> down = {"1/0", "3/0", "4/0", "7/1", "10/2", "11/2", "12/2", "14/2"};
        ed.SetCursorForLua(0, 0);
        for (const std::string &want : down) {
            ed.RunCommand("normal j");
            EXPECT(at() == want, "j steps to " + want, at());
        }
        // And back up: into C from below, then B's last line, A's last, the strip.
        const std::vector<std::string> up = {"12/2", "11/2", "10/2", "7/1", "4/0", "3/0", "1/0", "0/0"};
        for (const std::string &want : up) {
            ed.RunCommand("normal k");
            EXPECT(at() == want, "k steps to " + want, at());
        }
        // View mode: no stepping between tabs (a click on a title does it).
        ed.MepmlToggleView();
        ed.SetCursorForLua(3, 0);
        ed.RunCommand("normal j");
        ed.RunCommand("normal j");
        EXPECT(at() == "5/0", "in view mode j goes from A's end to its rule", at());
        ed.RunCommand("normal j");
        EXPECT(at() == "14/0", "and then past the set, A still shown", at());
    }

    std::printf("\n---- %d checks, %d failures ----\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
