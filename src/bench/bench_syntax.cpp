// mep-bench: tree-sitter highlighting and folding (src/treesitter.cpp) --
// the C++ half of what mep.syntax_highlight costs on every edit. The Lua
// half (building the text from the buffer, re-adding every decoration) is
// measured inside a live editor by tools/bench_live.lua.

#include "bench_util.h"
#include "bench/fixtures.h"

#include "treesitter.h"

#include <string>
#include <vector>

using mep::bench::DoNotOptimize;
using mep::bench::Measure;

namespace {

struct LangFixture {
    const char *ft;
    const char *source;  // a real file in this checkout, grown to `lines`
    size_t lines;
};

const LangFixture kLangs[] = {
    {"cpp", "src/editor.cpp", 40000},
    {"py", "tools/mepml_editor_bench.py", 20000},
    {"lua", "tools/mepml_editor_bench.lua", 20000},
    {"md", "plans/CAD_FEM_PLAN.md", 20000},
    {"org", "README.org", 20000},
    {"mepml", "test.mepml", 20000},
};

void BenchLang(const LangFixture &f) {
    if (!TreesitterHasGrammar(f.ft)) {
        std::fprintf(stderr, "syntax: no grammar for %s, skipped\n", f.ft);
        return;
    }
    const std::vector<std::string> src = mep::bench::SplitLines(mep::bench::ReadFileOrEmpty(mep::bench::RepoPath(f.source)));
    if (src.empty()) return;
    std::vector<std::string> lines = mep::bench::GrowLines(src, f.lines);
    const std::string text = mep::bench::JoinLines(lines);
    const long long n = static_cast<long long>(lines.size());
    const std::string ft = f.ft;

    // Cold: a cache key never seen before, so a full parse plus a full query.
    int cold = 0;
    Measure("highlight_cold", n, [&] {
        DoNotOptimize(TreesitterHighlight(ft, text, "bench-cold-" + ft + std::to_string(cold++)).size());
    });

    // One keystroke in the middle of the file: what typing pays. Two texts
    // alternate so every call is a real (one-character) change.
    std::vector<std::string> edited = lines;
    edited[lines.size() / 2] += "x";
    const std::string text_b = mep::bench::JoinLines(edited);
    bool flip = false;
    const std::string key = "bench-edit-" + ft;
    TreesitterHighlightRows(ft, text, key);
    Measure("highlight_rows_keystroke", n, [&] {
        flip = !flip;
        DoNotOptimize(TreesitterHighlightRows(ft, flip ? text_b : text, key).size());
    });
    // The same keystroke through the whole-text API (what mep.ts_highlight returns to Lua).
    const std::string key2 = "bench-edit2-" + ft;
    TreesitterHighlight(ft, text, key2);
    Measure("highlight_full_keystroke", n, [&] {
        flip = !flip;
        DoNotOptimize(TreesitterHighlight(ft, flip ? text_b : text, key2).size());
    });
    // Unchanged text (a cursor move re-running the hook): should be ~free.
    Measure("highlight_rows_unchanged", n, [&] { DoNotOptimize(TreesitterHighlightRows(ft, text, key).size()); });

    // Two open files of one language, highlighted alternately (switching
    // buffers): under one shared key each switch evicts the other file's
    // tree and reparses from scratch; with a key per buffer (what
    // mep.syntax_highlight passes) each keeps its own.
    std::vector<std::string> other_lines(lines.begin() + static_cast<long>(lines.size() / 3), lines.end());
    other_lines.insert(other_lines.end(), lines.begin(), lines.begin() + static_cast<long>(lines.size() / 3));
    const std::string other = mep::bench::JoinLines(other_lines);
    Measure("switch_files_shared_key", n, [&] {
        flip = !flip;
        DoNotOptimize(TreesitterHighlightRows(ft, flip ? other : text, "bench-shared-" + ft).size());
    });
    Measure("switch_files_own_keys", n, [&] {
        flip = !flip;
        DoNotOptimize(TreesitterHighlightRows(ft, flip ? other : text, flip ? "buf:9001:" + ft : "buf:9002:" + ft).size());
    });

    // Building the text the way the Lua hook does (every line copied and joined).
    Measure("join_buffer_text", n, [&] { DoNotOptimize(mep::bench::JoinLines(lines).size()); });

    if (TreesitterHasFoldQuery(ft)) {
        Measure("fold_ranges", n, [&] {
            flip = !flip;
            DoNotOptimize(TreesitterFoldRanges(ft, flip ? text_b : text).size());
        });
    }
}

}  // namespace

MEP_BENCH("syntax/cpp") { BenchLang(kLangs[0]); }
MEP_BENCH("syntax/py") { BenchLang(kLangs[1]); }
MEP_BENCH("syntax/lua") { BenchLang(kLangs[2]); }
MEP_BENCH("syntax/md") { BenchLang(kLangs[3]); }
MEP_BENCH("syntax/org") { BenchLang(kLangs[4]); }
MEP_BENCH("syntax/mepml") { BenchLang(kLangs[5]); }
