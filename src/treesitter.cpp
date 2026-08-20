#include "treesitter.h"

#include <tree_sitter/api.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <unordered_map>

#include "treesitter_queries.h"

// The core set, compiled directly into mep (see CMakeLists.txt
// TS_GRAMMAR_NAMES) -- statically linked, so these symbols are always
// resolvable at link time.
extern "C" {
const TSLanguage *tree_sitter_c(void);
const TSLanguage *tree_sitter_cpp(void);
const TSLanguage *tree_sitter_lua(void);
const TSLanguage *tree_sitter_python(void);
const TSLanguage *tree_sitter_javascript(void);
const TSLanguage *tree_sitter_markdown(void);
const TSLanguage *tree_sitter_markdown_inline(void);
const TSLanguage *tree_sitter_org(void);
const TSLanguage *tree_sitter_r(void);
}

namespace {

struct LangEntry {
    const TSLanguage *(*language)(void);
    const char *query_source;
};

// The core set, compiled directly into mep, keyed by the bare file
// extensions mep_lsp_filetype (main.cpp) hands back. Deliberately a
// small, curated set (not "as many as possible" -- see
// NVIM_PARITY_PLAN.md Phase 19 for why): C, C++, Lua (mep's own
// scripting/config language), Python, JavaScript, Markdown (+ its
// separate inline-formatting grammar), Org, and R. Everything else is
// resolved dynamically at runtime instead -- see DynamicLanguageTable
// and LoadDynamicLanguage below.
const std::unordered_map<std::string, LangEntry> &LanguageTable() {
    static const std::unordered_map<std::string, LangEntry> table = {
        {"c", {tree_sitter_c, kHighlightsC}},
        {"h", {tree_sitter_c, kHighlightsC}},
        {"cpp", {tree_sitter_cpp, kHighlightsCpp}},
        {"cc", {tree_sitter_cpp, kHighlightsCpp}},
        {"cxx", {tree_sitter_cpp, kHighlightsCpp}},
        {"hpp", {tree_sitter_cpp, kHighlightsCpp}},
        {"hh", {tree_sitter_cpp, kHighlightsCpp}},
        {"hxx", {tree_sitter_cpp, kHighlightsCpp}},
        {"lua", {tree_sitter_lua, kHighlightsLua}},
        {"py", {tree_sitter_python, kHighlightsPython}},
        {"js", {tree_sitter_javascript, kHighlightsJavascript}},
        {"mjs", {tree_sitter_javascript, kHighlightsJavascript}},
        {"cjs", {tree_sitter_javascript, kHighlightsJavascript}},
        {"jsx", {tree_sitter_javascript, kHighlightsJavascript}},
        // markdown's entry here is only consulted for
        // TreesitterHasGrammar/query-source lookups -- TreesitterHighlight
        // special-cases "md"/"markdown" to also run the markdown_inline
        // injection pass (see HighlightMarkdown below).
        {"md", {tree_sitter_markdown, kHighlightsMarkdown}},
        {"markdown", {tree_sitter_markdown, kHighlightsMarkdown}},
        {"org", {tree_sitter_org, kHighlightsOrg}},
        {"r", {tree_sitter_r, kHighlightsR}},
        {"R", {tree_sitter_r, kHighlightsR}},
    };
    return table;
}

#if !defined(__EMSCRIPTEN__)
// Every filetype with a known highlight query but no compiled-in
// grammar, keyed the same way as LanguageTable -- resolved by
// dlopen()ing a matching `.so` at runtime instead (see
// LoadDynamicLanguage below). `canonical_name` is the grammar's own
// name as it appears in its `tree_sitter_<canonical_name>` export --
// the same string every packaging convention (Nix's
// `tree-sitter.withPlugins`, the tree-sitter CLI's own build cache,
// nvim-treesitter's installed parsers) derives its `.so` filename from,
// so it doubles as the filename LoadDynamicLanguage searches for.
struct DynLangEntry {
    const char *canonical_name;
    const char *query_source;
};

const std::unordered_map<std::string, DynLangEntry> &DynamicLanguageTable() {
    static const std::unordered_map<std::string, DynLangEntry> table = {
        {"sh", {"bash", kHighlightsBash}},
        {"bash", {"bash", kHighlightsBash}},
        {"zsh", {"bash", kHighlightsBash}},
        {"cs", {"c_sharp", kHighlightsCSharp}},
        {"css", {"css", kHighlightsCss}},
        {"go", {"go", kHighlightsGo}},
        {"hs", {"haskell", kHighlightsHaskell}},
        {"html", {"html", kHighlightsHtml}},
        {"htm", {"html", kHighlightsHtml}},
        {"java", {"java", kHighlightsJava}},
        {"json", {"json", kHighlightsJson}},
        {"jl", {"julia", kHighlightsJulia}},
        {"ml", {"ocaml", kHighlightsOcaml}},
        {"mli", {"ocaml_interface", kHighlightsOcaml}},
        {"php", {"php", kHighlightsPhp}},
        {"rb", {"ruby", kHighlightsRuby}},
        {"rs", {"rust", kHighlightsRust}},
        {"scala", {"scala", kHighlightsScala}},
        {"sbt", {"scala", kHighlightsScala}},
        {"ts", {"typescript", kHighlightsTypescript}},
        {"mts", {"typescript", kHighlightsTypescript}},
        {"cts", {"typescript", kHighlightsTypescript}},
        {"tsx", {"tsx", kHighlightsTypescript}},
        {"v", {"verilog", kHighlightsVerilog}},
        {"sv", {"verilog", kHighlightsVerilog}},
        {"vh", {"verilog", kHighlightsVerilog}},
        {"yaml", {"yaml", kHighlightsYaml}},
        {"yml", {"yaml", kHighlightsYaml}},
        {"toml", {"toml", kHighlightsToml}},
        {"nix", {"nix", kHighlightsNix}},
        {"zig", {"zig", kHighlightsZig}},
        {"elm", {"elm", kHighlightsElm}},
        {"kt", {"kotlin", kHighlightsKotlin}},
        {"kts", {"kotlin", kHighlightsKotlin}},
        {"scss", {"scss", kHighlightsScss}},
        {"svelte", {"svelte", kHighlightsSvelte}},
        {"vue", {"vue", kHighlightsVue}},
        {"csv", {"csv", kHighlightsCsv}},
        {"diff", {"diff", kHighlightsDiff}},
        {"patch", {"diff", kHighlightsDiff}},
        {"hcl", {"hcl", kHighlightsHcl}},
        {"tf", {"hcl", kHighlightsHcl}},
        {"tfvars", {"hcl", kHighlightsHcl}},
        {"mk", {"make", kHighlightsMake}},
        {"vim", {"vim", kHighlightsVim}},
        {"xml", {"xml", kHighlightsXml}},
        {"cmake", {"cmake", kHighlightsCmake}},
        {"clj", {"clojure", kHighlightsClojure}},
        {"cljs", {"clojure", kHighlightsClojure}},
        {"cljc", {"clojure", kHighlightsClojure}},
        {"edn", {"clojure", kHighlightsClojure}},
        {"dart", {"dart", kHighlightsDart}},
        {"dockerfile", {"dockerfile", kHighlightsDockerfile}},
        {"graphql", {"graphql", kHighlightsGraphql}},
        {"gql", {"graphql", kHighlightsGraphql}},
        {"proto", {"proto", kHighlightsProto}},
        {"ini", {"ini", kHighlightsIni}},
        {"fish", {"fish", kHighlightsFish}},
        {"groovy", {"groovy", kHighlightsGroovy}},
        {"gradle", {"groovy", kHighlightsGroovy}},
        {"ex", {"elixir", kHighlightsElixir}},
        {"exs", {"elixir", kHighlightsElixir}},
        {"erl", {"erlang", kHighlightsErlang}},
        {"hrl", {"erlang", kHighlightsErlang}},
    };
    return table;
}
#include <dlfcn.h>

// Search path for dynamically-loaded grammars, in priority order:
//  1. $MEP_TS_PARSER_PATH (colon-separated, like $PATH) -- explicit,
//     highest priority. flake.nix's devShell exports this pointing at a
//     `pkgs.tree-sitter.withPlugins` bundle, whose output directory
//     holds exactly the `<name>.so` files this loader looks for -- the
//     documented way to get more grammars on a Nix machine (this
//     project's own default devShell does exactly that).
//  2. The tree-sitter CLI's own build cache
//     ($XDG_CACHE_HOME/tree-sitter/lib, default ~/.cache/tree-sitter/lib)
//     -- what `tree-sitter build` populates; likely already there on a
//     machine that's used the CLI directly.
//  3. nvim-treesitter's installed-parser directory
//     ($XDG_DATA_HOME/nvim/site/parser, default
//     ~/.local/share/nvim/site/parser) -- a common pre-existing source
//     for anyone who already uses Neovim with nvim-treesitter.
//  4. mep's own config-owned directory ($XDG_CONFIG_HOME/mep/parsers,
//     default ~/.config/mep/parsers), mirroring the existing
//     ~/.config/mep/init.lua convention -- for a grammar built by hand
//     (`tree-sitter build`) with nowhere else to go.
std::vector<std::string> DynamicSearchPaths() {
    std::vector<std::string> paths;
    if (const char *env = getenv("MEP_TS_PARSER_PATH")) {
        std::string s(env);
        size_t start = 0;
        while (start <= s.size()) {
            size_t colon = s.find(':', start);
            if (colon == std::string::npos) {
                paths.push_back(s.substr(start));
                break;
            }
            paths.push_back(s.substr(start, colon - start));
            start = colon + 1;
        }
    }
    const char *home = getenv("HOME");
    std::string home_str = home ? home : "";
    if (const char *xdg_cache = getenv("XDG_CACHE_HOME")) {
        paths.push_back(std::string(xdg_cache) + "/tree-sitter/lib");
    } else if (!home_str.empty()) {
        paths.push_back(home_str + "/.cache/tree-sitter/lib");
    }
    if (const char *xdg_data = getenv("XDG_DATA_HOME")) {
        paths.push_back(std::string(xdg_data) + "/nvim/site/parser");
    } else if (!home_str.empty()) {
        paths.push_back(home_str + "/.local/share/nvim/site/parser");
    }
    if (const char *xdg_config = getenv("XDG_CONFIG_HOME")) {
        paths.push_back(std::string(xdg_config) + "/mep/parsers");
    } else if (!home_str.empty()) {
        paths.push_back(home_str + "/.config/mep/parsers");
    }
    return paths;
}

// Tries every (search path x filename variant) combination for
// `canonical_name`, dlopen()ing the first that succeeds and resolving
// its tree_sitter_<canonical_name> symbol -- the reinterpret_cast from
// dlsym's void* is technically outside strict ISO C++ (object-pointer to
// function-pointer conversion isn't guaranteed) but is the standard
// POSIX-sanctioned idiom for it, portable across every real dlopen
// implementation. The result (a language pointer, or nullptr on
// failure) is cached for the process lifetime -- a grammar dropped into
// a search path mid-session needs a restart to be picked up, same as
// every other static-once assumption this integration makes (compiled
// queries, the grammar tables themselves).
const TSLanguage *LoadDynamicLanguage(const std::string &canonical_name) {
    static std::unordered_map<std::string, const TSLanguage *> cache;
    auto it = cache.find(canonical_name);
    if (it != cache.end()) return it->second;

    const TSLanguage *result = nullptr;
    std::string symbol = "tree_sitter_" + canonical_name;
    std::vector<std::string> filenames = {
        canonical_name + ".so",
        "lib" + canonical_name + ".so",
        "libtree-sitter-" + canonical_name + ".so",
    };
    for (const std::string &dir : DynamicSearchPaths()) {
        if (dir.empty()) continue;
        for (const std::string &fname : filenames) {
            std::string path = dir + "/" + fname;
            void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!handle) continue;
            using LangFn = const TSLanguage *(*)(void);
            LangFn fn = reinterpret_cast<LangFn>(dlsym(handle, symbol.c_str()));
            if (fn) {
                result = fn();
                break;
            }
            dlclose(handle);
        }
        if (result) break;
    }
    cache[canonical_name] = result;
    return result;
}
#endif  // !__EMSCRIPTEN__

// Compiling a .scm query is real work mep.syntax_highlight() can't repay
// per call (potentially per keystroke, once mep.syntax_auto is on) -- one
// TSQuery per (language, query source), built lazily and kept for the
// process lifetime. Keyed by the query source pointer: every filetype
// sharing a language+query (e.g. cpp/cc/cxx/hpp/hh/hxx) always passes the
// exact same static string-literal address, so pointer identity is
// already a correct, zero-overhead cache key.
TSQuery *QueryFor(const TSLanguage *language, const char *query_source) {
    static std::unordered_map<const char *, TSQuery *> cache;
    auto it = cache.find(query_source);
    if (it != cache.end()) return it->second;
    uint32_t error_offset = 0;
    TSQueryError error_type = TSQueryErrorNone;
    TSQuery *q = ts_query_new(language, query_source, static_cast<uint32_t>(strlen(query_source)), &error_offset,
                               &error_type);
    cache[query_source] = q;
    return q;
}

std::string PredicateStringValue(const TSQuery *query, const TSQueryPredicateStep &step) {
    uint32_t len = 0;
    const char *s = ts_query_string_value_for_id(query, step.value_id, &len);
    return std::string(s, len);
}

std::string CaptureText(const TSQueryCapture &cap, const std::string &text) {
    uint32_t s = ts_node_start_byte(cap.node);
    uint32_t e = ts_node_end_byte(cap.node);
    if (e > text.size()) e = static_cast<uint32_t>(text.size());
    if (s >= e) return "";
    return text.substr(s, e - s);
}

// Evaluates the predicate operators these grammars' highlights.scm files
// actually use: #eq?/#not-eq?/#match?/#not-match?/#any-of?/#not-any-of?
// (each either two-capture, capture-vs-literal, or for any-of? a
// capture-vs-a-list-of-literals), plus nvim-treesitter's own
// #lua-match?/#not-lua-match? (used by the community queries vendored
// for the handful of grammars that ship no highlights.scm of their own
// -- hcl/kotlin/verilog/graphql/just) treated as #match?/#not-match?
// aliases: every pattern actually seen in the vendored queries is plain
// ASCII character-class regex, valid in both Lua patterns and
// std::regex's ECMAScript flavor. Property-only predicates like
// (#is-not? local) -- which would need a locals.scm binding analysis
// this integration doesn't do -- and anything else unrecognized are
// accepted as-is (treated as satisfied): a documented, minor precision
// loss (e.g. a local variable named `arguments` still renders as the JS
// builtin).
bool EvalPredicates(const TSQuery *query, const TSQueryMatch &match, const std::string &text) {
    uint32_t step_count = 0;
    const TSQueryPredicateStep *steps = ts_query_predicates_for_pattern(query, match.pattern_index, &step_count);
    uint32_t i = 0;
    while (i < step_count) {
        uint32_t start = i;
        while (i < step_count && steps[i].type != TSQueryPredicateStepTypeDone) i++;
        uint32_t end = i;
        if (i < step_count) i++;  // skip the Done sentinel
        if (end <= start || steps[start].type != TSQueryPredicateStepTypeString) continue;
        std::string op = PredicateStringValue(query, steps[start]);
        if (op == "lua-match?") op = "match?";
        if (op == "not-lua-match?") op = "not-match?";

        std::vector<std::string> operands;
        bool ok = true;
        for (uint32_t k = start + 1; k < end; k++) {
            if (steps[k].type == TSQueryPredicateStepTypeString) {
                operands.push_back(PredicateStringValue(query, steps[k]));
                continue;
            }
            bool found = false;
            for (uint16_t c = 0; c < match.capture_count; c++) {
                if (match.captures[c].index == steps[k].value_id) {
                    operands.push_back(CaptureText(match.captures[c], text));
                    found = true;
                    break;
                }
            }
            if (!found) {
                ok = false;
                break;
            }
        }
        if (!ok || operands.empty()) continue;

        if (op == "eq?" || op == "not-eq?") {
            if (operands.size() < 2) continue;
            bool eq = operands[0] == operands[1];
            if ((op == "eq?") != eq) return false;
        } else if (op == "match?" || op == "not-match?") {
            if (operands.size() < 2) continue;
            bool matched = false;
            try {
                std::regex re(operands[1], std::regex::ECMAScript);
                matched = std::regex_search(operands[0], re);
            } catch (const std::regex_error &) {
                matched = false;
            }
            if ((op == "match?") != matched) return false;
        } else if (op == "any-of?" || op == "not-any-of?") {
            bool any = false;
            for (size_t k = 1; k < operands.size(); k++) {
                if (operands[0] == operands[k]) {
                    any = true;
                    break;
                }
            }
            if ((op == "any-of?") != any) return false;
        }
        // Any other predicate (#is?, #is-not?, #set!, ...) passes through.
    }
    return true;
}

struct RawSpan {
    uint32_t start_byte;
    uint32_t end_byte;
    uint32_t start_row;
    uint32_t start_col;
    std::string capture;
};

// Runs `query` over every match under `root` and appends each capture
// (subject to EvalPredicates and the leading-underscore "not meant to be
// rendered" convention) as a RawSpan.
void CollectRawSpans(const TSQuery *query, TSNode root, const std::string &text, std::vector<RawSpan> &out) {
    TSQueryCursor *cursor = ts_query_cursor_new();
    ts_query_cursor_exec(cursor, query, root);
    TSQueryMatch match;
    while (ts_query_cursor_next_match(cursor, &match)) {
        if (!EvalPredicates(query, match, text)) continue;
        for (uint16_t c = 0; c < match.capture_count; c++) {
            const TSQueryCapture &cap = match.captures[c];
            uint32_t name_len = 0;
            const char *name = ts_query_capture_name_for_id(query, cap.index, &name_len);
            if (name_len == 0 || name[0] == '_') continue;  // "_foo": not meant to be rendered
            RawSpan rs;
            rs.start_byte = ts_node_start_byte(cap.node);
            rs.end_byte = std::min(ts_node_end_byte(cap.node), static_cast<uint32_t>(text.size()));
            if (rs.end_byte <= rs.start_byte) continue;
            TSPoint p = ts_node_start_point(cap.node);
            rs.start_row = p.row;
            rs.start_col = p.column;
            rs.capture.assign(name, name_len);
            out.push_back(std::move(rs));
        }
    }
    ts_query_cursor_delete(cursor);
}

// Parses `text` from scratch with `language` and collects its `query`'s
// captures into `out`.
void ParseAndCollect(const TSLanguage *language, const char *query_source, const std::string &text,
                      std::vector<RawSpan> &out) {
    TSQuery *query = QueryFor(language, query_source);
    if (!query) return;
    TSParser *parser = ts_parser_new();
    ts_parser_set_language(parser, language);
    TSTree *tree = ts_parser_parse_string(parser, nullptr, text.c_str(), static_cast<uint32_t>(text.size()));
    if (tree) {
        CollectRawSpans(query, ts_tree_root_node(tree), text, out);
        ts_tree_delete(tree);
    }
    ts_parser_delete(parser);
}

// Markdown's inline formatting (emphasis, links, code spans, inline
// code, ...) lives in a *separate* grammar (tree_sitter_markdown_inline)
// that the block grammar's own injections.scm marks as owning every
// `(inline)` node -- mirroring how a real editor's injection mechanism
// would wire the two together, but special-cased just for this one
// pairing rather than a general injection-query engine (the only other
// injection point markdown's own injections.scm defines is fenced code
// blocks by language, which would need this whole highlighter available
// recursively per embedded language; out of scope here). Each `(inline)`
// node's byte/point range is used as an `ts_parser_set_included_ranges`
// restriction on a *second* parse of the *same* full `text` with the
// inline grammar -- tree-sitter's own supported mechanism for this
// exact case, and the reason no manual row/column translation is needed:
// positions coming out of that second parse are already in the original
// document's coordinates.
void CollectMarkdownInlineSpans(TSNode block_root, const std::string &text, std::vector<RawSpan> &out) {
    TSQuery *finder = QueryFor(tree_sitter_markdown(), "(inline) @inline");
    if (!finder) return;
    TSQuery *inline_query = QueryFor(tree_sitter_markdown_inline(), kHighlightsMarkdownInline);
    if (!inline_query) return;

    TSQueryCursor *cursor = ts_query_cursor_new();
    ts_query_cursor_exec(cursor, finder, block_root);
    TSParser *iparser = ts_parser_new();
    ts_parser_set_language(iparser, tree_sitter_markdown_inline());

    TSQueryMatch match;
    while (ts_query_cursor_next_match(cursor, &match)) {
        if (match.capture_count == 0) continue;
        TSNode n = match.captures[0].node;
        TSRange r;
        r.start_point = ts_node_start_point(n);
        r.end_point = ts_node_end_point(n);
        r.start_byte = ts_node_start_byte(n);
        r.end_byte = ts_node_end_byte(n);
        if (r.end_byte <= r.start_byte) continue;
        ts_parser_set_included_ranges(iparser, &r, 1);
        TSTree *itree = ts_parser_parse_string(iparser, nullptr, text.c_str(), static_cast<uint32_t>(text.size()));
        if (itree) {
            CollectRawSpans(inline_query, ts_tree_root_node(itree), text, out);
            ts_tree_delete(itree);
        }
    }
    ts_parser_delete(iparser);
    ts_query_cursor_delete(cursor);
}

void HighlightMarkdown(const std::string &text, std::vector<RawSpan> &out) {
    TSQuery *blockQuery = QueryFor(tree_sitter_markdown(), kHighlightsMarkdown);
    if (!blockQuery) return;
    TSParser *parser = ts_parser_new();
    ts_parser_set_language(parser, tree_sitter_markdown());
    TSTree *tree = ts_parser_parse_string(parser, nullptr, text.c_str(), static_cast<uint32_t>(text.size()));
    if (tree) {
        TSNode root = ts_tree_root_node(tree);
        CollectRawSpans(blockQuery, root, text, out);
        CollectMarkdownInlineSpans(root, text, out);
        ts_tree_delete(tree);
    }
    ts_parser_delete(parser);
}

}  // namespace

bool TreesitterHasGrammar(const std::string &filetype) {
    if (LanguageTable().count(filetype)) return true;
#if !defined(__EMSCRIPTEN__)
    auto it = DynamicLanguageTable().find(filetype);
    if (it != DynamicLanguageTable().end()) return LoadDynamicLanguage(it->second.canonical_name) != nullptr;
#endif
    return false;
}

std::vector<TSHighlightSpan> TreesitterHighlight(const std::string &filetype, const std::string &text) {
    std::vector<TSHighlightSpan> out;
    std::vector<RawSpan> raw;

    if (auto it = LanguageTable().find(filetype); it != LanguageTable().end()) {
        const LangEntry &entry = it->second;
        if (filetype == "md" || filetype == "markdown") {
            HighlightMarkdown(text, raw);
        } else {
            ParseAndCollect(entry.language(), entry.query_source, text, raw);
        }
    } else {
#if !defined(__EMSCRIPTEN__)
        auto dit = DynamicLanguageTable().find(filetype);
        if (dit == DynamicLanguageTable().end()) return out;
        const TSLanguage *language = LoadDynamicLanguage(dit->second.canonical_name);
        if (!language) return out;
        ParseAndCollect(language, dit->second.query_source, text, raw);
#else
        return out;
#endif
    }

    // Widest spans first: mep's renderer paints same-namespace decoration
    // spans in insertion order and later draws win, so emitting broad
    // captures (e.g. a whole call_expression) before the narrow ones
    // nested inside them (e.g. the function name) makes the narrow, more
    // specific highlight the one that actually shows.
    std::sort(raw.begin(), raw.end(), [](const RawSpan &a, const RawSpan &b) {
        return (a.end_byte - a.start_byte) > (b.end_byte - b.start_byte);
    });

    // Split every span at line boundaries -- mep's decoration model is
    // strictly per-line. TSPoint.column is already a byte offset within
    // its row, matching the byte-oriented column convention the rest of
    // the decoration/rendering pipeline uses (see main.cpp's use of
    // std::string::substr on col_start/col_end).
    for (const RawSpan &rs : raw) {
        size_t pos = rs.start_byte;
        uint32_t row = rs.start_row;
        uint32_t col = rs.start_col;
        while (pos < rs.end_byte) {
            size_t nl = text.find('\n', pos);
            size_t line_end = (nl == std::string::npos || nl >= rs.end_byte) ? static_cast<size_t>(rs.end_byte) : nl;
            TSHighlightSpan span;
            span.row = static_cast<int>(row);
            span.col_start = static_cast<int>(col);
            span.col_end = static_cast<int>(col + (line_end - pos));
            span.capture = rs.capture;
            out.push_back(std::move(span));
            if (line_end >= rs.end_byte) break;
            pos = line_end + 1;  // skip the newline itself
            row++;
            col = 0;
        }
    }
    return out;
}
