// mep-bench: a real, windowless Editor driven the way a person drives it
// (`:normal` keys and ex commands) -- the same approach as
// mep-editor-fold-test. What this measures is the editor core alone: the
// buffer, undo, search/substitute, the Lua-facing line accessors. Hooks,
// highlighting and drawing need a live editor (tools/bench_live.lua).

#include "bench_util.h"
#include "bench/fixtures.h"

#include "editor.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

// Defined in main.cpp for the real binary; mep_core's Lua bindings pull
// them in, and a windowless Editor never calls them (see editor_fold_test.cpp).
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

using mep::bench::DoNotOptimize;
using mep::bench::Measure;

namespace {

std::string ScratchDir() {
    static const std::string dir = [] {
        std::error_code ec;
        std::string d = (std::filesystem::temp_directory_path(ec) / "mep-bench-editor").string();
        std::filesystem::create_directories(d + "/share", ec);
        // Keep the Editor's session/state files out of the user's real data dir.
#if !defined(_WIN32)
        setenv("XDG_DATA_HOME", (d + "/share").c_str(), 1);
#else
        _putenv_s("XDG_DATA_HOME", (d + "/share").c_str());
#endif
        return d;
    }();
    return dir;
}

/** @brief A C++ file of exactly `lines` lines (src/editor.cpp grown), written once to the scratch dir. */
std::string CppFile(size_t lines) {
    const std::string path = ScratchDir() + "/big_" + std::to_string(lines) + ".cpp";
    if (!std::filesystem::exists(path)) {
        const auto src = mep::bench::SplitLines(mep::bench::ReadFileOrEmpty(mep::bench::RepoPath("src/editor.cpp")));
        std::ofstream(path, std::ios::binary) << mep::bench::JoinLines(mep::bench::GrowLines(src, lines));
    }
    return path;
}

void BenchFile(size_t lines) {
    const std::string path = CppFile(lines);
    const long long n = static_cast<long long>(lines);

    Measure("load_file", n, [&] {
        Editor ed;
        ed.LoadFile(path);
        DoNotOptimize(ed.LineCountForLua());
    });

    Editor ed;
    ed.LoadFile(path);
    ed.RunCommand("normal " + std::to_string(lines / 2) + "G");

    // A normal-mode edit: every one pushes an undo snapshot. `x` then `u`
    // keeps the buffer the same size forever.
    Measure("normal_x_then_undo", n, [&] {
        ed.RunCommand("normal x");
        ed.RunCommand("normal u");
    });
    // An insert session: enter insert, type a word, leave.
    Measure("insert_word_session", n, [&] {
        ed.RunCommand("normal Ahello");
        ed.RunCommand("normal u");
    });
    Measure("open_line_below", n, [&] {
        ed.RunCommand("normal ofoo();");
        ed.RunCommand("normal u");
    });
    // Big yank/put: 1000 lines pasted mid-file, then undone.
    Measure("paste_1000_lines", n, [&] {
        ed.RunCommand("normal 1000yyP");
        ed.RunCommand("normal u");
    });
    // :s over the whole buffer (a common identifier, many matches), then undo.
    Measure("substitute_global", n, [&] {
        ed.RunCommand("%s/Buf()/BUF()/g");
        ed.RunCommand("normal u");
    });
    // :g deletes every matching line, one :d (and one undo push) per match.
    Measure("global_delete", n, [&] {
        ed.RunCommand("g/status_message_/d");
        ed.RunCommand("normal u");
    });
    // Searching: a word that is not in the file, so every search scans the
    // whole buffer (and wraps) -- the worst case of `/`, and `n` after it.
    // (Through :s with no match: the same per-line SearchOnce-style scan as
    // `/`, which `:normal` cannot drive -- its search prompt is closed again
    // before <CR> runs it.)
    Measure("search_miss", n, [&] { ed.RunCommand("%s/ZzNoSuchIdentifierZz//"); });
    Measure("search_regex_miss", n, [&] { ed.RunCommand("%s/Zz[a-z]\\+Ident//"); });
    // What a Lua hook pays to read the buffer: mep.get_line over every row.
    Measure("lua_get_all_lines", n, [&] {
        size_t total = 0;
        const int count = ed.LineCountForLua();
        for (int r = 0; r < count; r++) total += ed.GetLineForLua(r).size();
        DoNotOptimize(total);
    });
    // ...and to write 100 lines back one mep.set_line at a time.
    Measure("lua_set_line_x100", 100, [&] {
        const int base = static_cast<int>(lines / 3);
        for (int r = base; r < base + 100; r++) ed.SetLineForLua(r, ed.GetLineForLua(r));
    });
    ed.RunCommand("normal gg");
}

}  // namespace

MEP_BENCH("editor/construct") {
    ScratchDir();
    Measure("new_editor", 1, [] {
        Editor ed;
        DoNotOptimize(ed.LineCountForLua());
    });
}
MEP_BENCH("editor/cpp_10k") { BenchFile(10000); }
MEP_BENCH("editor/cpp_60k") { BenchFile(60000); }
