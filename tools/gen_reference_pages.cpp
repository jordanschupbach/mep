// Generate the reference pages of the help manual.
//
// Three kinds: the Lua API pages, the command index and the key index.
//
// mep exposes ~445 Lua bindings. Hand-writing a reference for that many, and
// then keeping it correct as they change, is not realistic -- so these six
// pages are generated from `src/lua_env.cpp`, which is where the bindings and
// their doc comments already live.
//
// Each binding is registered as `{"name", l_name}` and documented either by a
// Doxygen `@brief` on its `l_name` implementation or by a plain `// mep.name(...)`
// comment above it. Both forms are read here.
//
// Run by `just help`, before the Org sources are exported. The generated
// help/api-*.org files are committed like any other page; regenerating them is
// part of the normal docs build, and `just help-check` fails if a committed
// page has drifted from what its source produces.
//
// Built as the `mep-gen-reference-pages` host tool; replaces
// scripts/gen_reference_pages.py with byte-identical output.
//
// Usage: mep-gen-reference-pages [repo-root]   (default: the source tree
// this tool was configured from)

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "py_text.h"

#ifndef MEP_SOURCE_DIR
#define MEP_SOURCE_DIR "."
#endif

namespace {

namespace fs = std::filesystem;
using pytext::U;
using Str = std::u32string;

bool IsNameChar(char32_t c) {
    return (c >= U'a' && c <= U'z') || c == U'_' || (c >= U'0' && c <= U'9');
}

struct Family {
    std::string slug;
    std::string title;
    int order;
    std::string intro;
    std::vector<std::string> prefixes;  // re.match alternatives; "open$" is exact
};

// Which page each binding lands on. Order matters: the first family that
// matches wins, so the more specific families come first.
const std::vector<Family> &Families() {
    static const std::vector<Family> kFamilies = {
        {"api-docs", "Documents and formats", 50,
         "Org, Markdown, HTML, PDF, office, spreadsheet, notebook, image, 3D model and "
         "learning-deck bindings -- everything that drives one of mep's document viewers or editors.",
         {"org_", "md_", "html_", "pdf_", "office_", "sheet_", "notebook_", "model_", "image_", "buf_",
          "learn_", "doc_export", "ansi_render", "babel_", "roam_graph", "is_image_path", "is_pdf_buffer"}},
        {"api-lang", "Language tooling", 40,
         "Language servers, completion, snippets, syntax, spell checking, formatting, debugging and git.",
         {"lsp_", "completion_", "snippet_", "spell_", "syntax_", "dap_", "git_", "docs_", "colorize",
          "accept_inline", "clear_inline", "inline_suggestion", "set_inline", "set_completion",
          "set_insert_tab_hook", "leetcode_", "diff_lines", "ts_"}},
        {"api-ui", "Panes, panels and pickers", 20,
         "Everything that puts something on screen: panes and tabs, sidebars, pickers, floats, hovers, "
         "notifications, themes and icons.",
         {"pane_", "tab_new", "sidebar_", "picker_", "float_", "hover_", "notify", "layout",
          "set_statusline", "set_winbar_click", "icon_for_file", "hl_for_file", "colorscheme",
          "current_theme", "theme_names", "font_size", "hint_jump", "quick_jump", "pick_pane_open",
          "resize_pane", "nav_pane", "focus_top_left_pane", "deco_add", "ns_"}},
        {"api-workspace", "Workspaces, files and processes", 30,
         "Workspaces and projects, the filesystem, subprocesses, HTTP and the environment mep runs in.",
         {"workspace_", "project_", "fs_", "list_dir", "getcwd", "chdir", "setenv", "unsetenv", "platform",
          "job_", "http_", "run_config", "direnv", "participant", "bundled_help_root", "list_urls_scan",
          "fuzzy_score", "read_lines", "open$", "scratch", "quit", "now"}},
        {"api-events", "Commands, keys and events", 60,
         "Defining commands and key bindings, and reacting to what happens in the editor.",
         {"on_", "command", "map", "map_", "leader_", "mapping_descriptions", "set_leader", "set_mod1", "cmd",
          "enter_insert", "enter_normal", "is_insert_mode", "is_terminal_buffer", "terminal_",
          "active_todo_set", "activity_", "stt_", "set_on_directory_open", "set_completion_accept_hook",
          "set_completion_resolve_hook"}},
        {"api-editing", "Buffers, text and the cursor", 10,
         "Reading and changing buffer contents, moving the cursor, the clipboard, folds and undo.",
         {""}},  // catch-all, so nothing is dropped
    };
    return kFamilies;
}

bool FamilyMatches(const Family &f, const std::string &name) {
    for (const std::string &p : f.prefixes) {
        if (!p.empty() && p.back() == '$') {
            if (name == p.substr(0, p.size() - 1)) return true;
        } else if (p.empty()) {
            if (!name.empty() && name[0] != '\n') return true;  // r"."
        } else if (name.compare(0, p.size(), p) == 0) {
            return true;
        }
    }
    return false;
}

// `\{"([a-z_0-9]+)",\s*l_[a-z_0-9]+\}` -- every registration, in source order.
std::vector<Str> Bindings(const Str &text) {
    std::vector<Str> out;
    size_t pos = 0;
    while ((pos = text.find(U"{\"", pos)) != Str::npos) {
        size_t i = pos + 2;
        const size_t name_start = i;
        while (i < text.size() && IsNameChar(text[i])) ++i;
        const size_t name_end = i;
        bool ok = name_end > name_start && pytext::At(text, i, U"\",");
        if (ok) {
            i += 2;
            while (i < text.size() && pytext::IsSpace(text[i])) ++i;
            ok = pytext::At(text, i, U"l_");
        }
        if (ok) {
            i += 2;
            const size_t impl_start = i;
            while (i < text.size() && IsNameChar(text[i])) ++i;
            ok = i > impl_start && i < text.size() && text[i] == U'}';
        }
        if (ok) {
            out.push_back(text.substr(name_start, name_end - name_start));
            pos = i + 1;
        } else {
            pos += 1;
        }
    }
    return out;
}

// Length of the terminator `\n\s*\*\s*@|\n\s*\*/` at `e`, or 0 if none.
size_t BriefTerminator(const Str &text, size_t e) {
    if (e >= text.size() || text[e] != U'\n') return 0;
    size_t i = e + 1;
    while (i < text.size() && pytext::IsSpace(text[i])) ++i;
    if (i >= text.size() || text[i] != U'*') return 0;
    ++i;
    size_t j = i;
    while (j < text.size() && pytext::IsSpace(text[j])) ++j;
    if (j < text.size() && text[j] == U'@') return j + 1 - e;
    if (i < text.size() && text[i] == U'/') return i + 1 - e;
    return 0;
}

// `name(...)`'s `Implements mep\.([a-z_0-9]+)` search: the binding name or empty.
Str ImplementsName(const Str &blurb) {
    const Str kw = U"Implements mep.";
    size_t pos = 0;
    while ((pos = blurb.find(kw, pos)) != Str::npos) {
        size_t i = pos + kw.size();
        const size_t start = i;
        while (i < blurb.size() && IsNameChar(blurb[i])) ++i;
        if (i > start) return blurb.substr(start, i - start);
        pos += 1;
    }
    return Str();
}

// re.sub(r"^Implements mep\.[a-z_0-9]+(\([^)]*\))?:?\s*", "", blurb)
Str StripImplements(const Str &blurb) {
    const Str kw = U"Implements mep.";
    if (!pytext::At(blurb, 0, kw)) return blurb;
    size_t i = kw.size();
    const size_t start = i;
    while (i < blurb.size() && IsNameChar(blurb[i])) ++i;
    if (i == start) return blurb;
    if (i < blurb.size() && blurb[i] == U'(') {
        const size_t close = blurb.find(U')', i + 1);
        if (close != Str::npos) i = close + 1;
    }
    if (i < blurb.size() && blurb[i] == U':') ++i;
    while (i < blurb.size() && pytext::IsSpace(blurb[i])) ++i;
    return blurb.substr(i);
}

// name -> one-line description, from @brief or a `// mep.name(...)` comment.
std::map<Str, Str> Briefs(const Str &text) {
    std::map<Str, Str> out;
    // `@brief\s+(.*?)(?:\n\s*\*\s*@|\n\s*\*/)` with re.S, via finditer.
    const Str kw = U"@brief";
    size_t pos = 0;
    while ((pos = text.find(kw, pos)) != Str::npos) {
        size_t ws_end = pos + kw.size();
        while (ws_end < text.size() && pytext::IsSpace(text[ws_end])) ++ws_end;
        bool matched = false;
        // \s+ is greedy: try the longest whitespace run first, backing off.
        for (size_t g = ws_end; g > pos + kw.size(); --g) {
            size_t e = g;
            size_t term = 0;
            for (; e < text.size(); ++e) {
                if ((term = BriefTerminator(text, e)) != 0) break;
            }
            if (term == 0) continue;
            const Str group = text.substr(g, e - g);
            std::vector<Str> parts;
            for (const Str &part : pytext::SplitChar(group, U'\n')) parts.push_back(pytext::StripChars(part, U" *"));
            const Str blurb = pytext::Strip(pytext::Join(parts, U" "));
            const Str name = ImplementsName(blurb);
            if (!name.empty()) {
                out.emplace(name, pytext::RStripChars(StripImplements(blurb), U"."));
            }
            pos = e + term;
            matched = true;
            break;
        }
        if (!matched) pos += 1;
    }
    // `^// mep\.([a-z_0-9]+)\(([^)]*)\)([^\n]*)` with re.M, via finditer.
    const Str kw2 = U"// mep.";
    pos = 0;
    while ((pos = text.find(kw2, pos)) != Str::npos) {
        if (pos != 0 && text[pos - 1] != U'\n') {
            pos += 1;
            continue;
        }
        size_t i = pos + kw2.size();
        const size_t name_start = i;
        while (i < text.size() && IsNameChar(text[i])) ++i;
        const size_t name_end = i;
        if (name_end == name_start || i >= text.size() || text[i] != U'(') {
            pos += 1;
            continue;
        }
        const size_t close = text.find(U')', i + 1);
        if (close == Str::npos) {
            pos += 1;
            continue;
        }
        size_t rest_end = close + 1;
        while (rest_end < text.size() && text[rest_end] != U'\n') ++rest_end;
        const Str rest = text.substr(close + 1, rest_end - close - 1);
        out.emplace(text.substr(name_start, name_end - name_start),
                    pytext::RStripChars(pytext::LStripChars(rest, U" .->:"), U"."));
        pos = rest_end;
    }
    return out;
}

bool IsLowerDigitParen(char32_t c) {
    return (c >= U'a' && c <= U'z') || (c >= U'0' && c <= U'9') || c == U')';
}

// Keep the reference scannable: one clause per binding, not a paragraph.
Str FirstSentence(const Str &raw, size_t limit = 150) {
    Str text = pytext::CollapseWs(raw);
    // re.split(r"(?<=[a-z0-9)])\.\s+(?=[A-Z])", text)[0]
    for (size_t i = 1; i < text.size(); ++i) {
        if (text[i] != U'.' || !IsLowerDigitParen(text[i - 1])) continue;
        size_t j = i + 1;
        while (j < text.size() && pytext::IsSpace(text[j])) ++j;
        if (j == i + 1 || j >= text.size() || text[j] < U'A' || text[j] > U'Z') continue;
        text = text.substr(0, i);
        break;
    }
    if (text.size() > limit) {
        Str cut = text.substr(0, limit);
        const size_t sp = cut.rfind(U' ');
        if (sp != Str::npos) cut = cut.substr(0, sp);
        text = cut + U"...";
    }
    // `|` would split an Org table cell, and `=` pairs would read as markup.
    Str out;
    for (char32_t c : text) {
        if (c == U'|') {
            out.push_back(U'/');
        } else if (c != U'=') {
            out.push_back(c);
        }
    }
    return out;
}

bool WriteLines(const fs::path &path, const std::vector<Str> &lines) {
    if (!pytext::WriteBytes(path.string(), pytext::Encode(pytext::Join(lines, U"\n")))) {
        std::fprintf(stderr, "gen_reference_pages: cannot write %s\n", path.string().c_str());
        return false;
    }
    return true;
}

std::u32string N(size_t n) { return U(std::to_string(n)); }

bool Page(const fs::path &help, const Family &f, const std::vector<std::pair<Str, Str>> &entries) {
    std::vector<Str> lines = {
        U("#+TITLE: " + f.title),
        U"#+HTML_HEAD: <meta name=\"help-section\" content=\"Lua API\">",
        U("#+HTML_HEAD: <meta name=\"help-order\" content=\"" + std::to_string(f.order) + "\">"),
        U"#+HTML_HEAD: <link rel=\"stylesheet\" href=\"help.css\">",
        U"",
        U"* What is here",
        U"",
        U(f.intro),
        U"",
        U"This page is generated from mep's own binding table, so it lists what",
        U"the running editor actually exposes. Call any of these from",
        U"=init.lua=, from =:lua=, or from a =mep-lua= org block.",
        U"",
        U"* The bindings (" + N(entries.size()) + U")",
        U"",
        U"A dash means that binding has no doc comment in the source yet, not",
        U"that it does nothing.",
        U"",
        U"| Function | Does |",
        U"|----------+------|",
    };
    for (const auto &[name, desc] : entries) {
        lines.push_back(U"| =mep." + name + U"= | " + (desc.empty() ? U"--" : desc) + U" |");
    }
    for (const char *l : {"", "* See also", "", "- [[file:config.html][Configuration]] -- where to call these from.",
                          "- [[file:recipes.html][Configuration recipes]] -- worked examples.", ""}) {
        lines.push_back(U(l));
    }
    return WriteLines(help / (f.slug + ".org"), lines);
}

struct HelpPage {
    Str title;
    Str body;
};

// `#\+TITLE:\s*(.+)` via re.search, then .strip(); nullopt-like flag if absent.
bool FindTitle(const Str &text, Str *title) {
    const Str kw = U"#+TITLE:";
    size_t pos = 0;
    while ((pos = text.find(kw, pos)) != Str::npos) {
        const size_t a = pos + kw.size();
        size_t b = a;
        while (b < text.size() && pytext::IsSpace(text[b])) ++b;
        // \s* is greedy, then backs off until `.+` (one or more non-newline
        // characters) can match.
        for (size_t k = b + 1; k-- > a;) {
            if (k >= text.size() || text[k] == U'\n') continue;
            size_t end = k;
            while (end < text.size() && text[end] != U'\n') ++end;
            *title = pytext::Strip(text.substr(k, end - k));
            return true;
        }
        pos += 1;
    }
    return false;
}

// command-or-key -> the page that mentions it (sorted by stem).
std::map<Str, HelpPage> DocumentedIn(const fs::path &help) {
    std::map<Str, HelpPage> where;
    for (const auto &entry : fs::directory_iterator(help)) {
        if (!entry.is_regular_file() && !entry.is_symlink()) continue;
        const fs::path &p = entry.path();
        if (p.extension() != ".org") continue;
        const std::string name = p.filename().string();
        if (name.rfind("_", 0) == 0 || name.rfind("api-", 0) == 0 || name.rfind("command-index", 0) == 0 ||
            name.rfind("key-index", 0) == 0) {
            continue;
        }
        Str body;
        if (!pytext::ReadText(p.string(), &body)) continue;
        const Str stem = pytext::Decode(p.stem().string());
        Str title;
        if (!FindTitle(body, &title)) title = stem;
        where[stem] = HelpPage{title, body};
    }
    return where;
}

Str PageFor(const Str &token, const std::map<Str, HelpPage> &where, bool leader) {
    const Str needle = leader ? U"<leader>" + token : token;
    // A page naming the command as such (=:exec=) explains it; one that
    // merely contains the word somewhere is only the fallback.
    if (!leader) {
        const Str named = U"=:" + token + U"=";
        for (const auto &[stem, pg] : where) {
            if (pg.body.find(named) != Str::npos) return U"[[file:" + stem + U".html][" + pg.title + U"]]";
        }
    }
    for (const auto &[stem, pg] : where) {
        if (pg.body.find(needle) != Str::npos) return U"[[file:" + stem + U".html][" + pg.title + U"]]";
    }
    return U"--";
}

// Every `<prefix>'([A-Za-z0-9_]+)'` / quoted capture, finditer-style.
std::set<Str> LuaCommands(const Str &text) {
    std::set<Str> out;
    const Str kw = U"mep.command('";
    size_t pos = 0;
    while ((pos = text.find(kw, pos)) != Str::npos) {
        size_t i = pos + kw.size();
        const size_t start = i;
        while (i < text.size() && (pytext::IsWord(text[i]) && text[i] < 0x80)) ++i;
        if (i > start && i < text.size() && text[i] == U'\'') {
            out.insert(text.substr(start, i - start));
            pos = i + 1;
        } else {
            pos += 1;
        }
    }
    return out;
}

// `"([^"]+)"` findall.
std::set<Str> QuotedStrings(const Str &text) {
    std::set<Str> out;
    size_t pos = 0;
    while ((pos = text.find(U'"', pos)) != Str::npos) {
        const size_t close = text.find(U'"', pos + 1);
        if (close == Str::npos) break;
        if (close > pos + 1) {
            out.insert(text.substr(pos + 1, close - pos - 1));
            pos = close + 1;
        } else {
            pos += 1;
        }
    }
    return out;
}

Str Lower(const Str &s) {
    Str out = s;
    for (char32_t &c : out) c = pytext::AsciiLower(c);
    return out;
}

bool GenCommandIndex(const fs::path &repo, const fs::path &help, const std::map<Str, HelpPage> &where,
                     size_t *count) {
    Str main_text, editor_text;
    if (!pytext::ReadText((repo / "src" / "main.cpp").string(), &main_text) ||
        !pytext::ReadText((repo / "src" / "editor.cpp").string(), &editor_text)) {
        std::fprintf(stderr, "gen_reference_pages: cannot read src/main.cpp or src/editor.cpp\n");
        return false;
    }
    const std::set<Str> lua_cmds = LuaCommands(main_text);
    std::set<Str> native_cmds;
    const Str kw = U"static const std::vector<std::string> kNames = {";
    const size_t at = editor_text.find(kw);
    if (at != Str::npos) {
        const size_t end = editor_text.find(U"};", at + kw.size());
        if (end != Str::npos) native_cmds = QuotedStrings(editor_text.substr(at + kw.size(), end - at - kw.size()));
    }
    std::set<Str> all = lua_cmds;
    all.insert(native_cmds.begin(), native_cmds.end());
    std::vector<Str> sorted(all.begin(), all.end());
    std::stable_sort(sorted.begin(), sorted.end(), [](const Str &a, const Str &b) { return Lower(a) < Lower(b); });

    std::vector<Str> lines = {
        U"#+TITLE: Command index",
        U"#+HTML_HEAD: <meta name=\"help-section\" content=\"Reference\">",
        U"#+HTML_HEAD: <meta name=\"help-order\" content=\"10\">",
        U"#+HTML_HEAD: <link rel=\"stylesheet\" href=\"help.css\">",
        U"",
        U"* Every command",
        U"",
        U"All " + N(all.size()) + U" ex commands mep defines: " + N(native_cmds.size()) + U" built in and " +
            N(lua_cmds.size()) + U" registered from Lua.",
        U"",
        U"=Tab= at the =:= prompt completes these, and =<leader>hk= lists the key",
        U"bindings instead. A dash means no page covers that command yet.",
        U"",
        U"| Command | Documented in |",
        U"|---------+---------------|",
    };
    for (const Str &cmd : sorted) lines.push_back(U"| =:" + cmd + U"= | " + PageFor(cmd, where, false) + U" |");
    for (const char *l : {"", "* See also", "", "- [[file:key-index.html][Key index]] -- the same, for keys.",
                          "- [[file:command-line.html][The command line]] -- how to run one.", ""}) {
        lines.push_back(U(l));
    }
    *count = sorted.size();
    return WriteLines(help / "command-index.org", lines);
}

// `mep\.leader_map\('([^']+)'\s*,\s*'([^']*)'` findall.
std::set<std::pair<Str, Str>> LeaderMaps(const Str &text) {
    std::set<std::pair<Str, Str>> out;
    const Str kw = U"mep.leader_map('";
    size_t pos = 0;
    while ((pos = text.find(kw, pos)) != Str::npos) {
        size_t i = pos + kw.size();
        const size_t seq_end = text.find(U'\'', i);
        bool ok = seq_end != Str::npos && seq_end > i;
        size_t desc_start = 0, desc_end = 0;
        if (ok) {
            size_t j = seq_end + 1;
            while (j < text.size() && pytext::IsSpace(text[j])) ++j;
            ok = j < text.size() && text[j] == U',';
            if (ok) {
                ++j;
                while (j < text.size() && pytext::IsSpace(text[j])) ++j;
                ok = j < text.size() && text[j] == U'\'';
            }
            if (ok) {
                desc_start = j + 1;
                desc_end = text.find(U'\'', desc_start);
                ok = desc_end != Str::npos;
            }
        }
        if (ok) {
            out.emplace(text.substr(i, seq_end - i), text.substr(desc_start, desc_end - desc_start));
            pos = desc_end + 1;
        } else {
            pos += 1;
        }
    }
    return out;
}

bool GenKeyIndex(const fs::path &repo, const fs::path &help, const std::map<Str, HelpPage> &where, size_t *count) {
    Str main_text;
    if (!pytext::ReadText((repo / "src" / "main.cpp").string(), &main_text)) return false;
    std::vector<Str> rows;
    for (const auto &[seq, desc] : LeaderMaps(main_text)) {
        rows.push_back(U"| =<leader>" + seq + U"= | " + desc + U" | " + PageFor(seq, where, true) + U" |");
    }
    std::vector<Str> lines = {
        U"#+TITLE: Key index",
        U"#+HTML_HEAD: <meta name=\"help-section\" content=\"Reference\">",
        U"#+HTML_HEAD: <meta name=\"help-order\" content=\"20\">",
        U"#+HTML_HEAD: <link rel=\"stylesheet\" href=\"help.css\">",
        U"",
        U"* Leader bindings",
        U"",
        U"All " + N(rows.size()) + U" =<leader>= sequences registered at startup. Press",
        U"=<Space>= for the same list as a popup, or =<leader>hk= for every",
        U"binding including your own.",
        U"",
        U"Motions, operators and the other Normal-mode keys are on the pages in",
        U"the Editing section; this index covers the leader layer, which is where",
        U"the features live.",
        U"",
        U"| Key | Does | Documented in |",
        U"|-----+------+---------------|",
    };
    lines.insert(lines.end(), rows.begin(), rows.end());
    for (const char *l : {"", "* See also", "", "- [[file:command-index.html][Command index]] -- the same, for commands.",
                          "- [[file:which-key.html][Leader and which-key]] -- the groups.",
                          "- [[file:keymaps.html][Remapping keys]] -- changing them.", ""}) {
        lines.push_back(U(l));
    }
    *count = rows.size();
    return WriteLines(help / "key-index.org", lines);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc > 2) {
        std::fprintf(stderr, "Usage: mep-gen-reference-pages [repo-root]\n");
        return 2;
    }
    const fs::path repo = argc == 2 ? fs::path(argv[1]) : fs::path(MEP_SOURCE_DIR);
    const fs::path help = repo / "help";

    Str text;
    if (!pytext::ReadText((repo / "src" / "lua_env.cpp").string(), &text)) {
        std::fprintf(stderr, "gen_reference_pages: cannot read %s\n", (repo / "src" / "lua_env.cpp").string().c_str());
        return 1;
    }
    const std::vector<Str> found = Bindings(text);
    const std::set<Str> name_set(found.begin(), found.end());
    const std::vector<Str> names(name_set.begin(), name_set.end());
    if (names.empty()) {
        std::fprintf(stderr, "gen_reference_pages: found no bindings -- has lua_env.cpp changed shape?\n");
        return 1;
    }
    const std::map<Str, Str> desc = Briefs(text);
    auto desc_of = [&](const Str &n) {
        const auto it = desc.find(n);
        return it == desc.end() ? Str() : it->second;
    };

    std::set<Str> claimed;
    size_t total = 0;
    for (const Family &f : Families()) {
        std::vector<std::pair<Str, Str>> entries;
        for (const Str &n : names) {
            if (claimed.count(n) || !FamilyMatches(f, pytext::Encode(n))) continue;
            entries.emplace_back(n, FirstSentence(desc_of(n)));
        }
        for (const auto &e : entries) claimed.insert(e.first);
        if (!Page(help, f, entries)) return 1;
        total += entries.size();
    }
    const std::map<Str, HelpPage> where = DocumentedIn(help);
    size_t n_cmds = 0, n_keys = 0;
    if (!GenCommandIndex(repo, help, where, &n_cmds)) return 1;
    if (!GenKeyIndex(repo, help, where, &n_keys)) return 1;
    std::printf("gen_reference_pages: %zu commands, %zu leader keys indexed\n", n_cmds, n_keys);
    size_t undocumented = 0;
    for (const Str &n : names) {
        if (desc_of(n).empty()) ++undocumented;
    }
    std::printf("gen_reference_pages: %zu bindings across %zu pages (%zu with no doc comment in lua_env.cpp)\n", total,
                Families().size(), undocumented);
    return 0;
}
