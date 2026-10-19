#include "treesitter.h"

#include <tree_sitter/api.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <regex>
#include <stdint.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#if !defined(__EMSCRIPTEN__)
#include <thread>
#endif

// Vendored verbatim from each grammar repo (see treesitter_queries.h's own
// top comment) -- not hand-written, so suppress our strict warnings here
// rather than editing vendored content to satisfy them.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif
#include "treesitter_queries.h"
#include "treesitter_structure_queries.h"
// mep's own grammars ship their queries as .scm files
// (grammars/tree-sitter-<name>/queries); CMake embeds them here.
#include "mepml_queries.h"
#include "nix_queries.h"
#include "just_queries.h"
#include "r_queries.h"
#include "lua_queries.h"
#include "c_queries.h"
#include "python_queries.h"
#include "javascript_queries.h"
#include "frame_activity.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

// The core set, compiled directly into mep (see CMakeLists.txt
// TS_GRAMMAR_NAMES) -- statically linked, so these symbols are always
// resolvable at link time.
extern "C" {
const TSLanguage *tree_sitter_cpp(void);
const TSLanguage *tree_sitter_markdown(void);
const TSLanguage *tree_sitter_markdown_inline(void);
const TSLanguage *tree_sitter_org(void);
const TSLanguage *tree_sitter_mepml(void);  // mep's own: grammars/tree-sitter-mepml
const TSLanguage *tree_sitter_nix(void);    // mep's own: grammars/tree-sitter-nix
const TSLanguage *tree_sitter_just(void);   // mep's own: grammars/tree-sitter-just
const TSLanguage *tree_sitter_r(void);      // mep's own: grammars/tree-sitter-r
const TSLanguage *tree_sitter_lua(void);    // mep's own: grammars/tree-sitter-lua
const TSLanguage *tree_sitter_c(void);      // mep's own: grammars/tree-sitter-c
const TSLanguage *tree_sitter_python(void); // mep's own: grammars/tree-sitter-python
const TSLanguage *tree_sitter_javascript(void); // mep's own: grammars/tree-sitter-javascript
}

// The process-wide caches below -- parse trees and row answers per key,
// compiled queries, loaded grammars, compiled predicate regexes -- are
// shared with the background highlighter (TreesitterHighlightRowsMaybeAsync),
// so every lookup or insertion into one of them holds this lock. Only
// briefly: parsing and querying run outside it, on per-key state that one
// thread at a time works on (the background thread's key is never also
// highlighted here, and is never evicted while in use -- KeysInUse).
// Recursive because a few of those short sections nest (eviction inside
// the row cache's lookup).
namespace {
std::recursive_mutex &TreesitterMutex() {
    static std::recursive_mutex m;
    return m;
}
}  // namespace
#define MEP_TS_LOCK() std::lock_guard<std::recursive_mutex> ts_lock_(TreesitterMutex())

namespace {
// Keys the background highlighter is working on right now: TreesitterForget
// leaves them alone (their state is being read and written off-thread).
std::unordered_set<std::string> &KeysInUse() {
    static std::unordered_set<std::string> keys;
    return keys;
}
// Set on the background highlighter's thread: it never evicts (see
// TouchBufferKey), so a key the UI thread is in the middle of reading can
// only ever be evicted by the UI thread itself, which never does that.
thread_local bool g_ts_background_thread = false;
}  // namespace

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
/**
 * @brief Returns the process-lifetime table of compiled-in grammars,
 * keyed by bare file extension.
 * @return Reference to the static filetype-to-LangEntry table for the core compiled-in languages.
 */
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
        // .pyi stub files are a strict syntactic subset of regular Python
        // (no runtime statements, just signatures/annotations) -- the same
        // grammar/query pairing parses them fine, this was just a missing
        // table entry (mep_lsp_filetype in main.cpp already resolves
        // "foo.pyi" to the bare extension "pyi", same as any other file).
        {"pyi", {tree_sitter_python, kHighlightsPython}},
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
        {"mepml", {tree_sitter_mepml, kHighlightsMepml}},
        {"nix", {tree_sitter_nix, kHighlightsNix}},
        // justfiles: LspFiletype maps `justfile`, `Justfile` and `.justfile` here too.
        {"just", {tree_sitter_just, kHighlightsJust}},
    };
    return table;
}

// Fold queries: only the core compiled-in languages get one (see
// treesitter_queries.h's kFolds* comment for why markdown is excluded
// but org isn't), keyed the same way as LanguageTable.
/**
 * @brief Returns the process-lifetime table of fold queries for the core
 * compiled-in languages, keyed by bare file extension.
 * @return Reference to the static filetype-to-LangEntry fold-query table.
 */
const std::unordered_map<std::string, LangEntry> &FoldQueryTable() {
    static const std::unordered_map<std::string, LangEntry> table = {
        {"c", {tree_sitter_c, kFoldsC}},
        {"h", {tree_sitter_c, kFoldsC}},
        {"cpp", {tree_sitter_cpp, kFoldsCpp}},
        {"cc", {tree_sitter_cpp, kFoldsCpp}},
        {"cxx", {tree_sitter_cpp, kFoldsCpp}},
        {"hpp", {tree_sitter_cpp, kFoldsCpp}},
        {"hh", {tree_sitter_cpp, kFoldsCpp}},
        {"hxx", {tree_sitter_cpp, kFoldsCpp}},
        {"lua", {tree_sitter_lua, kFoldsLua}},
        {"py", {tree_sitter_python, kFoldsPython}},
        {"pyi", {tree_sitter_python, kFoldsPython}},
        {"js", {tree_sitter_javascript, kFoldsJavascript}},
        {"mjs", {tree_sitter_javascript, kFoldsJavascript}},
        {"cjs", {tree_sitter_javascript, kFoldsJavascript}},
        {"jsx", {tree_sitter_javascript, kFoldsJavascript}},
        {"r", {tree_sitter_r, kFoldsR}},
        {"R", {tree_sitter_r, kFoldsR}},
        {"org", {tree_sitter_org, kFoldsOrg}},
        {"mepml", {tree_sitter_mepml, kFoldsMepml}},
        {"nix", {tree_sitter_nix, kFoldsNix}},
        {"just", {tree_sitter_just, kFoldsJust}},
    };
    return table;
}

// Structure (document outline) queries for the core compiled-in
// languages -- see treesitter_structure_queries.h for what each pattern
// actually captures. Every dynamically-loaded grammar's own structure
// query lives in DynamicStructureQueryTable below instead (native builds
// only, same split LanguageTable/DynamicLanguageTable already draws).
/**
 * @brief Returns the process-lifetime table of structure (document
 * outline) queries for the core compiled-in languages, keyed by bare file
 * extension.
 * @return Reference to the static filetype-to-LangEntry structure-query table.
 */
const std::unordered_map<std::string, LangEntry> &StructureQueryTable() {
    static const std::unordered_map<std::string, LangEntry> table = {
        {"c", {tree_sitter_c, kStructureC}},
        {"h", {tree_sitter_c, kStructureC}},
        {"cpp", {tree_sitter_cpp, kStructureCpp}},
        {"cc", {tree_sitter_cpp, kStructureCpp}},
        {"cxx", {tree_sitter_cpp, kStructureCpp}},
        {"hpp", {tree_sitter_cpp, kStructureCpp}},
        {"hh", {tree_sitter_cpp, kStructureCpp}},
        {"hxx", {tree_sitter_cpp, kStructureCpp}},
        {"lua", {tree_sitter_lua, kStructureLua}},
        {"py", {tree_sitter_python, kStructurePython}},
        {"pyi", {tree_sitter_python, kStructurePython}},
        {"js", {tree_sitter_javascript, kStructureJavascript}},
        {"mjs", {tree_sitter_javascript, kStructureJavascript}},
        {"cjs", {tree_sitter_javascript, kStructureJavascript}},
        {"jsx", {tree_sitter_javascript, kStructureJavascript}},
        {"r", {tree_sitter_r, kStructureR}},
        {"R", {tree_sitter_r, kStructureR}},
        // mep's own grammars keep theirs in grammars/tree-sitter-<name>/
        // queries/structure.scm (embedded via <name>_queries.h).
        {"nix", {tree_sitter_nix, kStructureNix}},
        {"just", {tree_sitter_just, kStructureJust}},
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

// .mli files share the OCaml query, except that the interface grammar has
// no shebang node -- left in, it makes ts_query_new reject the whole query.
/**
 * @brief Returns the OCaml highlight query with its shebang capture removed,
 * for the ocaml_interface grammar; built once for the process lifetime.
 * @return The adapted query source, stable for QueryFor's pointer-keyed cache.
 */
const char *OcamlInterfaceHighlights() {
    static const std::string query = [] {
        std::string q = kHighlightsOcaml;
        const std::string shebang = " (shebang)";
        size_t pos = q.find(shebang);
        if (pos != std::string::npos) q.erase(pos, shebang.size());
        return q;
    }();
    return query.c_str();
}

// tree-sitter-typescript's highlights.scm only covers what TypeScript adds
// on top of JavaScript -- upstream layers it over the JavaScript query
// (its tree-sitter.json lists both), and on its own a .ts file got no
// strings, functions or most keywords. JavaScript's patterns go first so
// the TypeScript ones, later, win where both match.
//
// Not all of mep's JavaScript query: a pattern naming a node or shape
// TypeScript's grammar lacks fails the whole query there, so
// grammars/tree-sitter-javascript/queries/highlights.scm keeps those in
// two trailing sections, found by their headers: "JavaScript only" (bare
// parameters, which TypeScript wraps in required_parameter) is left out
// of both, "JSX" is kept for .tsx alone.
/**
 * @brief Returns the part of the JavaScript highlight query TypeScript's
 * grammars share, with the JSX section too when `jsx`, followed by the
 * TypeScript query.
 * @param jsx Whether to keep the JSX section (for the tsx grammar).
 * @return The combined query source.
 */
std::string LayeredTypescriptHighlights(bool jsx) {
    const std::string js = kHighlightsJavascript;
    const size_t js_only = js.find("; ---- JavaScript only");
    const size_t jsx_section = js.find("; ---- JSX");
    std::string query = js.substr(0, js_only);
    if (jsx && jsx_section != std::string::npos) query += js.substr(jsx_section);
    return query + kHighlightsTypescript;
}

/**
 * @brief Returns the .ts highlight query, built once and kept for the
 * process lifetime.
 * @return The combined query source, stable for QueryFor's pointer-keyed cache.
 */
const char *TypescriptHighlights() {
    static const std::string query = LayeredTypescriptHighlights(false);
    return query.c_str();
}

/**
 * @brief Returns the .tsx highlight query (TypeScript's, plus JavaScript's
 * JSX patterns), built once and kept for the process lifetime.
 * @return The combined query source, stable for QueryFor's pointer-keyed cache.
 */
const char *TsxHighlights() {
    static const std::string query = LayeredTypescriptHighlights(true);
    return query.c_str();
}

/**
 * @brief Returns the process-lifetime table of filetypes with a known
 * highlight query but no compiled-in grammar, keyed by bare file
 * extension, each resolved by dlopen()ing a matching `.so` at runtime.
 * @return Reference to the static filetype-to-DynLangEntry table.
 */
const std::unordered_map<std::string, DynLangEntry> &DynamicLanguageTable() {
    static const std::unordered_map<std::string, DynLangEntry> table = {
        {"sh", {"bash", kHighlightsBash}},
        {"bash", {"bash", kHighlightsBash}},
        {"zsh", {"bash", kHighlightsBash}},
        {"cs", {"c_sharp", kHighlightsCSharp}},
        {"css", {"css", kHighlightsCss}},
        // mepml style sheets are CSS in syntax (docs/mepml-spec/style.md).
        {"mepss", {"css", kHighlightsCss}},
        {"go", {"go", kHighlightsGo}},
        {"hs", {"haskell", kHighlightsHaskell}},
        {"html", {"html", kHighlightsHtml}},
        {"htm", {"html", kHighlightsHtml}},
        {"java", {"java", kHighlightsJava}},
        {"json", {"json", kHighlightsJson}},
        {"jl", {"julia", kHighlightsJulia}},
        {"ml", {"ocaml", kHighlightsOcaml}},
        {"mli", {"ocaml_interface", OcamlInterfaceHighlights()}},
        {"php", {"php", kHighlightsPhp}},
        {"rb", {"ruby", kHighlightsRuby}},
        {"rs", {"rust", kHighlightsRust}},
        {"scala", {"scala", kHighlightsScala}},
        {"sbt", {"scala", kHighlightsScala}},
        {"ts", {"typescript", TypescriptHighlights()}},
        {"mts", {"typescript", TypescriptHighlights()}},
        {"cts", {"typescript", TypescriptHighlights()}},
        {"tsx", {"tsx", TsxHighlights()}},
        {"v", {"verilog", kHighlightsVerilog}},
        {"sv", {"verilog", kHighlightsVerilog}},
        {"vh", {"verilog", kHighlightsVerilog}},
        {"yaml", {"yaml", kHighlightsYaml}},
        {"yml", {"yaml", kHighlightsYaml}},
        {"toml", {"toml", kHighlightsToml}},
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
        // Added for org-babel src-block highlighting (mep_org_babel_lang_ts_ft
        // in main.cpp's kBuiltinSyntax) -- mep.org_babel_langs already
        // supports all five, but no grammar query existed for them until now.
        {"pl", {"perl", kHighlightsPerl}},
        {"pm", {"perl", kHighlightsPerl}},
        {"f90", {"fortran", kHighlightsFortran}},
        {"f95", {"fortran", kHighlightsFortran}},
        {"f", {"fortran", kHighlightsFortran}},
        {"for", {"fortran", kHighlightsFortran}},
        {"d", {"d", kHighlightsD}},
        {"nim", {"nim", kHighlightsNim}},
        {"cr", {"crystal", kHighlightsCrystal}},
        // .m is MATLAB/Octave here, not Objective-C (no ObjC grammar is
        // wired up at all).
        {"m", {"matlab", kHighlightsMatlab}},
        {"scm", {"scheme", kHighlightsScheme}},
        {"ss", {"scheme", kHighlightsScheme}},
        {"tcl", {"tcl", kHighlightsTcl}},
        {"sql", {"sql", kHighlightsSql}},
        {"g", {"gap", kHighlightsGap}},
        {"gi", {"gap", kHighlightsGap}},
        {"gd", {"gap", kHighlightsGap}},
        {"gap", {"gap", kHighlightsGap}},
        {"mac", {"maxima", kHighlightsMaxima}},
        {"max", {"maxima", kHighlightsMaxima}},
        // LaTeX + BibTeX (latex-lsp's grammars; see kHighlightsLatex's
        // own comment for why the query is written from scratch). .sty/
        // .cls are the same language as .tex proper -- package/class
        // files are just LaTeX with \makeatletter conventions, and the
        // grammar parses them fine.
        {"tex", {"latex", kHighlightsLatex}},
        {"sty", {"latex", kHighlightsLatex}},
        {"cls", {"latex", kHighlightsLatex}},
        {"bib", {"bibtex", kHighlightsBibtex}},
    };
    return table;
}

// Structure queries for dynamically-loaded grammars -- same
// canonical_name convention as DynamicLanguageTable (used to find/dlopen
// the `.so`), but only the subset of DynamicLanguageTable's filetypes
// that actually have a structure query written (treesitter_structure_
// queries.h): the broad mainstream languages, not every highlight-only
// grammar mep can highlight. mts/cts/tsx all reuse kStructureTypescript
// -- see that query's own comment for why one query covers all three.
/**
 * @brief Returns the process-lifetime table of structure queries for
 * dynamically-loaded grammars, keyed by bare file extension.
 * @return Reference to the static filetype-to-DynLangEntry structure-query table.
 */
const std::unordered_map<std::string, DynLangEntry> &DynamicStructureQueryTable() {
    static const std::unordered_map<std::string, DynLangEntry> table = {
        {"go", {"go", kStructureGo}},
        {"rs", {"rust", kStructureRust}},
        {"java", {"java", kStructureJava}},
        {"rb", {"ruby", kStructureRuby}},
        {"cs", {"c_sharp", kStructureCSharp}},
        {"php", {"php", kStructurePhp}},
        {"ts", {"typescript", kStructureTypescript}},
        {"mts", {"typescript", kStructureTypescript}},
        {"cts", {"typescript", kStructureTypescript}},
        {"tsx", {"tsx", kStructureTypescript}},
    };
    return table;
}
#if defined(_WIN32)
// Windows has no <dlfcn.h>. The three calls this file makes map one-to-one
// onto LoadLibrary/GetProcAddress/FreeLibrary, so they are adapted here
// rather than threading an #ifdef through the loader below -- the
// semantics that matter to the caller (null handle on failure, null symbol
// when absent) are the same on both.
//
// RTLD_NOW/RTLD_LOCAL have no counterpart and no bearing here: a grammar
// module exports one function and links nothing of its own, so eager vs.
// lazy binding and symbol visibility are both moot.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
namespace {
constexpr int RTLD_NOW = 0;
constexpr int RTLD_LOCAL = 0;
void *dlopen(const char *path, int /*flags*/) { return LoadLibraryA(path); }
void *dlsym(void *handle, const char *symbol) {
    // Function pointer to void*: outside strict ISO C++ the same way
    // dlsym's own return type is, and the same conversion the POSIX path
    // below relies on.
    return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(handle), symbol));
}
int dlclose(void *handle) { return FreeLibrary(static_cast<HMODULE>(handle)) ? 0 : 1; }
}  // namespace
#else
#include <dlfcn.h>
#endif

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
/**
 * @brief Builds the ordered list of directories to search for a dynamic
 * grammar's `.so` file: $MEP_TS_PARSER_PATH entries first, then the
 * tree-sitter CLI cache, nvim-treesitter's parser directory, and mep's
 * own config-owned parsers directory.
 * @return Search directories in priority order (may include empty strings from a malformed $MEP_TS_PARSER_PATH).
 */
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
/**
 * @brief Finds and dlopen()s the `.so` for a grammar by trying every
 * (search path x filename variant) combination and resolving its
 * `tree_sitter_<canonical_name>` symbol, caching the result (including
 * failures) for the process lifetime.
 * @param canonical_name The grammar's own name, as it appears in its `tree_sitter_<canonical_name>` export.
 * @return Pointer to the loaded language, or nullptr if no matching library/symbol was found.
 */
const TSLanguage *LoadDynamicLanguage(const std::string &canonical_name) {
    MEP_TS_LOCK();
    static std::unordered_map<std::string, const TSLanguage *> cache;
    auto it = cache.find(canonical_name);
    if (it != cache.end()) return it->second;

    const TSLanguage *result = nullptr;
    std::string symbol = "tree_sitter_" + canonical_name;
    // A grammar module's name is whatever the tool that built it chose,
    // so every known spelling is tried in turn. On Windows the extension
    // is .dll, but the `lib`-prefixed spellings are kept: a grammar built
    // under MSYS2/MinGW keeps the Unix naming convention even though the
    // file is a DLL.
#if defined(_WIN32)
    std::vector<std::string> filenames = {
        canonical_name + ".dll",
        "lib" + canonical_name + ".dll",
        "libtree-sitter-" + canonical_name + ".dll",
        "tree-sitter-" + canonical_name + ".dll",
    };
#else
    std::vector<std::string> filenames = {
        canonical_name + ".so",
        "lib" + canonical_name + ".so",
        "libtree-sitter-" + canonical_name + ".so",
    };
#endif
    for (const std::string &dir : DynamicSearchPaths()) {
        if (dir.empty()) continue;
        for (const std::string &fname : filenames) {
            std::string path = dir;
            path += "/";
            path += fname;
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
/**
 * @brief Returns the compiled TSQuery for a (language, query source) pair,
 * building and caching it lazily for the process lifetime; the query
 * source's pointer identity is used as the cache key.
 * @param language The tree-sitter language to compile the query against.
 * @param query_source The `.scm` query source text.
 * @return Pointer to the cached compiled query, or nullptr if compilation failed.
 */
TSQuery *QueryFor(const TSLanguage *language, const char *query_source) {
    MEP_TS_LOCK();
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

/**
 * @brief Looks up the literal string value of a query predicate step.
 * @param query The compiled query the step belongs to.
 * @param step The predicate step whose string value_id should be resolved.
 * @return The step's literal string value.
 */
std::string PredicateStringValue(const TSQuery *query, const TSQueryPredicateStep &step) {
    uint32_t len = 0;
    const char *s = ts_query_string_value_for_id(query, step.value_id, &len);
    return std::string(s, len);
}

/**
 * @brief Extracts the source text spanned by a query capture's node,
 * clamped to `text`'s bounds.
 * @param cap The query capture whose node's text should be extracted.
 * @param text The full source text the node's byte offsets index into.
 * @return The substring of `text` covered by the capture's node, or an empty string if the range is invalid.
 */
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
// -- hcl/kotlin/verilog/graphql) treated as #match?/#not-match?
// aliases: every pattern actually seen in the vendored queries is plain
// ASCII character-class regex, valid in both Lua patterns and
// std::regex's ECMAScript flavor. Property-only predicates like
// (#is-not? local) -- which would need a locals.scm binding analysis
// this integration doesn't do -- and anything else unrecognized are
// accepted as-is (treated as satisfied): a documented, minor precision
// loss (e.g. a local variable named `arguments` still renders as the JS
// builtin).
/**
 * @brief Evaluates every predicate attached to a query match's pattern
 * (#eq?/#not-eq?/#match?/#not-match?/#any-of?/#not-any-of?, plus
 * #lua-match?/#not-lua-match? treated as #match?/#not-match? aliases),
 * treating any unrecognized predicate as satisfied.
 * @param query The compiled query the match belongs to.
 * @param match The query match whose predicates should be evaluated.
 * @param text The full source text captures' nodes index into.
 * @return True if every recognized predicate is satisfied (or none apply); false as soon as one fails.
 */
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
            // Compiled once per pattern, not once per match: building the
            // std::regex was ~15% of every highlight pass in a large mepml
            // buffer (plans/MEPML_PERFORMANCE_PLAN.md). The patterns come
            // from the queries, so the set is small and fixed. A pattern
            // std::regex rejects is remembered as nullptr: never matches.
            static std::unordered_map<std::string, std::unique_ptr<std::regex>> compiled;
            std::unique_lock<std::recursive_mutex> regex_lock(TreesitterMutex());
            auto it = compiled.find(operands[1]);
            if (it == compiled.end()) {
                std::unique_ptr<std::regex> re;
                try {
                    re = std::make_unique<std::regex>(operands[1], std::regex::ECMAScript);
                } catch (const std::regex_error &) {
                    re = nullptr;
                }
                it = compiled.emplace(operands[1], std::move(re)).first;
            }
            const std::regex *re = it->second.get();
            regex_lock.unlock();  // (a compiled regex is safe to share; the map is what needs the lock)
            const bool matched = re && std::regex_search(operands[0], *re);
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
    uint32_t start_byte = 0;
    uint32_t end_byte = 0;
    uint32_t start_row = 0;
    uint32_t start_col = 0;
    // Index of the query pattern this capture came from, kept only as
    // the tiebreak for two captures covering the exact same bytes (see
    // the sort in TreesitterHighlight).
    uint32_t pattern_index = 0;
    std::string capture;
};

// Runs `query` over every match under `root` and appends each capture
// (subject to EvalPredicates and the leading-underscore "not meant to be
// rendered" convention) as a RawSpan.
/**
 * @brief Runs `query` over every match under `root` and appends each
 * capture (subject to EvalPredicates and the leading-underscore "not meant
 * to be rendered" convention) as a RawSpan.
 * @param query The compiled query to run.
 * @param root The root node to search under.
 * @param text The full source text captures' nodes index into.
 * @param out Vector to append every accepted capture's RawSpan to.
 * @param row_lo First row to collect captures for (default: the whole tree).
 * @param row_hi One past the last row (default: the whole tree).
 */
void CollectRawSpans(const TSQuery *query, const TSNode &root, const std::string &text, std::vector<RawSpan> &out,
                     uint32_t row_lo = 0, uint32_t row_hi = UINT32_MAX) {
    TSQueryCursor *cursor = ts_query_cursor_new();
    // Rows [row_lo, row_hi) only: matches whose nodes intersect them
    // (a capture starting above row_lo still comes back whole).
    if (row_lo > 0 || row_hi != UINT32_MAX)
        ts_query_cursor_set_point_range(cursor, TSPoint{row_lo, 0}, TSPoint{row_hi, 0});
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
            rs.pattern_index = match.pattern_index;
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

// Incremental-reparse cache: one entry per filetype string, holding the
// last text parsed for that filetype and the TSTree that resulted, so a
// later call for the same filetype can hand tree-sitter both the old
// tree (edited via ts_tree_edit) and the old tree's own structure to
// reuse via ts_parser_parse_string's incremental path, instead of always
// parsing from scratch (nullptr old_tree). Keyed by filetype string
// rather than true buffer identity -- see the class-level tradeoff note
// on TreesitterHighlight in treesitter.h: switching between two
// same-filetype buffers costs a full-reparse-equivalent that one time
// (the diff against the "wrong" buffer's old text degenerates to
// "replace everything"), which is always *safe* (ts_tree_edit's contract
// only requires the edit region to be a superset of what actually
// changed, and "the whole text changed" trivially satisfies that), just
// not the incremental win in that specific case. mep's actual call
// pattern -- debounced rerun on every edit of the buffer currently being
// typed in -- hits the fast path on essentially every call.
struct ParseCache {
    std::string text;
    TSTree *tree = nullptr;
    const TSLanguage *language = nullptr;
    double last_parse_ms = 0.0;  // the last parse that finished (sets the next one's budget)
    /**
     * @brief Deletes the cached TSTree, if one is held.
     */
    ~ParseCache() {
        if (tree) ts_tree_delete(tree);
    }
    // Held as the mapped_type of std::unordered_map<std::string,
    // ParseCache> (ParseCacheTable() below); node-based containers like
    // unordered_map never copy/move their values on rehash, and every
    // insertion here goes through operator[] (constructs in place), so
    // copy/move are simply disabled rather than implemented -- an
    // implicit copy would double-delete `tree`.
    ParseCache() = default;
    ParseCache(const ParseCache &) = delete;
    ParseCache &operator=(const ParseCache &) = delete;
    ParseCache(ParseCache &&) = delete;
    ParseCache &operator=(ParseCache &&) = delete;
};

/**
 * @brief Returns the process-lifetime table of incremental-reparse
 * caches, keyed by filetype string.
 * @return Reference to the static filetype-to-ParseCache table.
 */
std::unordered_map<std::string, ParseCache> &ParseCacheTable() {
    static std::unordered_map<std::string, ParseCache> table;
    return table;
}

// Row/column (TSPoint) of byte offset `off` within `text`, scanning from
// the start -- tree-sitter points are line/column, not byte offsets.
/**
 * @brief Computes the row/column (TSPoint) of a byte offset within `text`
 * by scanning from the start.
 * @param text The full source text to scan.
 * @param off The byte offset (clamped to `text`'s size) to locate.
 * @return The TSPoint (row, column) corresponding to `off`.
 */
TSPoint PointFor(const std::string &text, size_t off) {
    uint32_t row = 0, col = 0;
    size_t n = std::min(off, text.size());
    for (size_t i = 0; i < n; i++) {
        if (text[i] == '\n') {
            row++;
            col = 0;
        } else {
            col++;
        }
    }
    return TSPoint{row, col};
}

// Synthesizes a TSInputEdit describing old_text -> new_text via longest
// common prefix/suffix -- always a *valid* edit region per tree-sitter's
// contract (the changed span must be a superset of what actually
// changed) even though it isn't always the *minimal* one a real
// character-level diff would find. mep doesn't track precise edit
// regions through every text-mutating call site in editor.cpp (that
// would be much more invasive, cross-cutting work -- see NVIM_PARITY_
// PLAN.md's own Phase 19 note), so this recomputes the region fresh from
// the two full text snapshots on every call instead. Still linear in the
// text size, same order as the full reparse this replaces, so it can't
// make the on-demand/debounced call pattern asymptotically worse.
/**
 * @brief Synthesizes a TSInputEdit describing the transformation from
 * `old_text` to `new_text` via longest common prefix/suffix -- a valid
 * (if not always minimal) edit region for tree-sitter's incremental
 * reparse contract.
 * @param old_text The previously-parsed text.
 * @param new_text The current text to diff against `old_text`.
 * @return The TSInputEdit describing the changed byte/point range.
 */
TSInputEdit ComputeEdit(const std::string &old_text, const std::string &new_text) {
    size_t max_common = std::min(old_text.size(), new_text.size());
    size_t prefix = 0;
    while (prefix < max_common && old_text[prefix] == new_text[prefix]) prefix++;
    size_t old_end = old_text.size();
    size_t new_end = new_text.size();
    while (old_end > prefix && new_end > prefix && old_text[old_end - 1] == new_text[new_end - 1]) {
        old_end--;
        new_end--;
    }
    TSInputEdit edit;
    edit.start_byte = static_cast<uint32_t>(prefix);
    edit.old_end_byte = static_cast<uint32_t>(old_end);
    edit.new_end_byte = static_cast<uint32_t>(new_end);
    edit.start_point = PointFor(old_text, prefix);
    edit.old_end_point = PointFor(old_text, old_end);
    edit.new_end_point = PointFor(new_text, new_end);
    return edit;
}

// Returns the parsed tree for `text` under `cache_key`/`language`,
// reparsing incrementally against the previous call's tree when one is
// cached for the same key and language (or reusing it outright, with no
// reparse at all, when `text` is byte-identical to last time -- the
// common case when a highlight pass and a fold pass run back-to-back
// against the same just-edited buffer, see TreesitterFoldRanges).
// Ownership stays with the cache: callers must NOT delete the returned
// tree, and must not hold onto it past their own call (the next GetTree
// call for the same `cache_key` may ts_tree_edit or replace it).
/**
 * @brief Returns the parsed tree for `text` under `cache_key`/`language`,
 * reusing or incrementally reparsing the previous call's cached tree when
 * one exists for the same key and language, or parsing from scratch
 * otherwise.
 * @param cache_key Identifier (typically the filetype string) the parse cache is keyed by.
 * @param language The tree-sitter language to parse with.
 * @param text The full text to parse.
 * @return The current tree for `cache_key`; ownership stays with the cache -- callers must not delete or retain it past their own call.
 */
TSTree *GetTree(const std::string &cache_key, const TSLanguage *language, const std::string &text) {
    ParseCache &cache = [&]() -> ParseCache & {
        MEP_TS_LOCK();
        return ParseCacheTable()[cache_key];  // (a node container: the entry stays put)
    }();
    if (cache.tree && cache.language == language && cache.text == text) {
        return cache.tree;
    }
    TSParser *parser = ts_parser_new();
    ts_parser_set_language(parser, language);

    TSTree *old_tree = nullptr;
    if (cache.tree && cache.language == language) {
        TSInputEdit edit = ComputeEdit(cache.text, text);
        ts_tree_edit(cache.tree, &edit);
        old_tree = cache.tree;
    } else if (cache.tree) {
        // Different language now claims this filetype key (e.g. a
        // dynamic grammar loaded after this filetype was first seen) --
        // the cached tree's structure isn't meaningful input for a
        // different grammar's parser, so discard it and parse fresh.
        ts_tree_delete(cache.tree);
        cache.tree = nullptr;
    }

    // A time budget for incremental reparses: max(100 ms, 3x the last
    // parse that finished). A grammar with a quadratic corner (mepml's
    // display maths had one: 1.7 s a keystroke on 4000 lines,
    // plans/MEPML_PERFORMANCE_PLAN.md) would otherwise stall the UI on
    // every edit for as long as the text stays in that shape. Over
    // budget, the previous tree -- already shifted by ts_tree_edit to
    // the new text's positions -- stands in until a reparse finishes, so
    // the highlights stay roughly in place instead of the editor
    // freezing. A first parse has no tree to fall back to, and a large
    // file's first parse is legitimately slow, so it gets no budget.
    struct Budget {
        std::chrono::steady_clock::time_point deadline;
    } budget{std::chrono::steady_clock::now() +
             std::chrono::microseconds(static_cast<long long>(std::max(100.0, cache.last_parse_ms * 3.0) * 1000.0))};
    TSInput input{};
    input.payload = const_cast<std::string *>(&text);
    input.read = [](void *payload, uint32_t byte_index, TSPoint, uint32_t *bytes_read) -> const char * {
        const std::string &t = *static_cast<const std::string *>(payload);
        if (byte_index >= t.size()) {
            *bytes_read = 0;
            return "";
        }
        *bytes_read = static_cast<uint32_t>(t.size() - byte_index);
        return t.data() + byte_index;
    };
    input.encoding = TSInputEncodingUTF8;
    TSParseOptions options{};
    options.payload = &budget;
    // Returning true cancels the parse.
    options.progress_callback = [](TSParseState *state) -> bool {
        return std::chrono::steady_clock::now() > static_cast<Budget *>(state->payload)->deadline;
    };
    const auto parse_start = std::chrono::steady_clock::now();
    TSTree *tree = old_tree ? ts_parser_parse_with_options(parser, old_tree, input, options)
                            : ts_parser_parse_string(parser, nullptr, text.c_str(), static_cast<uint32_t>(text.size()));
    if (!tree && old_tree) {
        cache.tree = old_tree;  // over budget: the edited old tree stands in
        cache.text = text;
        ts_parser_delete(parser);
        return cache.tree;
    }
    cache.last_parse_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - parse_start).count();
    if (old_tree) ts_tree_delete(old_tree);  // superseded by `tree` (or by nothing, if parsing failed)
    cache.tree = tree;
    cache.text = text;
    cache.language = language;
    ts_parser_delete(parser);
    return tree;
}

// Parses `text` with `language` (via GetTree's incremental cache) and
// collects the `query`'s captures into `out`.
/**
 * @brief Parses `text` with `language` (via GetTree's incremental cache)
 * and collects the `query_source`'s compiled query captures into `out`.
 * @param cache_key Identifier the parse cache is keyed by.
 * @param language The tree-sitter language to parse with.
 * @param query_source The `.scm` highlight query source text to run.
 * @param text The full text to parse.
 * @param out Vector to append every accepted capture's RawSpan to.
 */
void ParseAndCollect(const std::string &cache_key, const TSLanguage *language, const char *query_source,
                      const std::string &text, std::vector<RawSpan> &out) {
    const TSQuery *query = QueryFor(language, query_source);
    if (!query) return;
    const TSTree *tree = GetTree(cache_key, language, text);
    if (tree) CollectRawSpans(query, ts_tree_root_node(tree), text, out);
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
/**
 * @brief Finds every `(inline)` node under `block_root` and re-parses each
 * one's restricted byte range of `text` with the separate markdown-inline
 * grammar, appending its highlight captures to `out`.
 * @param block_root Root node of the already-parsed markdown block tree.
 * @param text The full source text (shared by both the block and inline parses).
 * @param out Vector to append every accepted inline capture's RawSpan to.
 */
void CollectMarkdownInlineSpans(const TSNode &block_root, const std::string &text, std::vector<RawSpan> &out,
                                uint32_t row_lo = 0, uint32_t row_hi = UINT32_MAX) {
    const TSQuery *finder = QueryFor(tree_sitter_markdown(), "(inline) @inline");
    if (!finder) return;
    const TSQuery *inline_query = QueryFor(tree_sitter_markdown_inline(), kHighlightsMarkdownInline);
    if (!inline_query) return;

    TSQueryCursor *cursor = ts_query_cursor_new();
    // Rows [row_lo, row_hi) only: just the paragraphs (inline nodes) that
    // touch them are parsed -- TreesitterHighlightRows' incremental path
    // after an edit, where re-parsing every paragraph in the document was
    // the whole cost of a keystroke in Markdown (plans/PERFORMANCE_PLAN.md 2.6).
    if (row_lo > 0 || row_hi != UINT32_MAX) ts_query_cursor_set_point_range(cursor, TSPoint{row_lo, 0}, TSPoint{row_hi, 0});
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

namespace {
/**
 * @brief Orders raw captures the way the highlight passes emit them: widest first, then ascending pattern index.
 * @param raw The captures, sorted in place.
 */
void SortRawSpans(std::vector<RawSpan> &raw) {
    // Widest spans first: mep's renderer paints same-namespace decoration
    // spans in insertion order and later draws win, so emitting broad
    // captures (e.g. a whole call_expression) before the narrow ones
    // nested inside them (e.g. the function name) makes the narrow, more
    // specific highlight the one that actually shows.
    //
    // Two captures covering the exact *same* bytes can't be separated
    // that way, and those are common: a highlights.scm routinely starts
    // with a catch-all ((identifier) @variable) and then narrows it with
    // later, predicate-guarded patterns over the same node (python's
    // @constructor/@constant/@variable.builtin identifier conventions,
    // for one). These queries are all written to nvim's convention, where
    // the *last* pattern to match a node is the one that wins, so equal-
    // width spans go in ascending pattern order -- the later pattern
    // paints last. Without that tiebreak std::sort's own unspecified
    // order for equal keys decided it, which is why e.g. an ALL_CAPS
    // python name could come out Blue (@constructor) in one place and
    // Cyan (@constant) in another within the same file. stable_sort, so
    // anything still fully tied (two captures of the same span from the
    // same pattern, or from the separate markdown-inline query) keeps its
    // collection order rather than reshuffling run to run.
    // Comparator: widest span first, then ascending query pattern index.
    std::stable_sort(raw.begin(), raw.end(), [](const RawSpan &a, const RawSpan &b) {
        uint32_t a_len = a.end_byte - a.start_byte, b_len = b.end_byte - b.start_byte;
        if (a_len != b_len) return a_len > b_len;
        return a.pattern_index < b.pattern_index;
    });

}

/**
 * @brief Splits raw captures at line boundaries into per-line spans, in `raw`'s order.
 * @param raw The captures (already sorted).
 * @param text The text they index into.
 * @param out Where the per-line spans go.
 */
void ClipRawSpans(const std::vector<RawSpan> &raw, const std::string &text, std::vector<TSHighlightSpan> &out) {
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
}
}  // namespace

/**
 * @brief Runs markdown's block highlight query over `text` (via GetTree's
 * cache) and then layers in the separate inline-formatting pass via
 * CollectMarkdownInlineSpans, appending both to `out`.
 * @param cache_key Identifier the parse cache is keyed by.
 * @param text The full markdown source text to parse and highlight.
 * @param out Vector to append every accepted capture's RawSpan to.
 */
void HighlightMarkdown(const std::string &cache_key, const std::string &text, std::vector<RawSpan> &out) {
    const TSQuery *blockQuery = QueryFor(tree_sitter_markdown(), kHighlightsMarkdown);
    if (!blockQuery) return;
    const TSTree *tree = GetTree(cache_key, tree_sitter_markdown(), text);
    if (!tree) return;
    // The block tree's own incremental structure comes from GetTree's
    // cache; the inline injection pass below always re-parses each
    // `(inline)` node's restricted range from scratch (see its own
    // comment) -- those parses are already cheap by construction (small,
    // per-node ranges), so caching them too isn't worth the extra
    // per-node cache-key bookkeeping it would take.
    TSNode root = ts_tree_root_node(tree);
    CollectRawSpans(blockQuery, root, text, out);
    CollectMarkdownInlineSpans(root, text, out);
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

std::vector<TSHighlightSpan> TreesitterHighlight(const std::string &filetype, const std::string &text,
                                                 const std::string &cache_key) {
    std::vector<TSHighlightSpan> out;
    std::vector<RawSpan> raw;
    const std::string &key = cache_key.empty() ? filetype : cache_key;

    if (auto it = LanguageTable().find(filetype); it != LanguageTable().end()) {
        const LangEntry &entry = it->second;
        if (filetype == "md" || filetype == "markdown") {
            HighlightMarkdown(key, text, raw);
        } else {
            ParseAndCollect(key, entry.language(), entry.query_source, text, raw);
        }
    } else {
#if !defined(__EMSCRIPTEN__)
        auto dit = DynamicLanguageTable().find(filetype);
        if (dit == DynamicLanguageTable().end()) return out;
        const TSLanguage *language = LoadDynamicLanguage(dit->second.canonical_name);
        if (!language) return out;
        ParseAndCollect(key, language, dit->second.query_source, text, raw);
#else
        return out;
#endif
    }

    SortRawSpans(raw);
    ClipRawSpans(raw, text, out);
    return out;
}

namespace {
// TreesitterHighlightRows' state, one per cache key: the per-row spans it
// last answered with, the text they describe, and a copy of the tree they
// came from. Its own copy (not ParseCache's tree), so another consumer
// reparsing the same key in between -- the fold pass does -- cannot lose
// the "what changed since I last looked" this needs.
struct RowHighlightState {
    std::string text;
    TSTree *tree = nullptr;
    const TSLanguage *language = nullptr;
    const TSQuery *query = nullptr;
    std::vector<std::vector<TSHighlightSpan>> rows;
    // The whole-text fallback (Markdown, or no plain query) has no tree to
    // ask what changed; `rows` is reused only for exactly the same text.
    bool fallback_valid = false;
    std::string fallback_filetype;
    // Bumped whenever `rows` may have changed (every path below except the
    // two "same text" early returns): TreesitterRowsGeneration.
    uint64_t generation = 0;
    RowHighlightState() = default;
    RowHighlightState(const RowHighlightState &) = delete;
    RowHighlightState &operator=(const RowHighlightState &) = delete;
    ~RowHighlightState() {
        if (tree) ts_tree_delete(tree);
    }
};
std::unordered_map<std::string, RowHighlightState> &RowHighlightTable() {
    static std::unordered_map<std::string, RowHighlightState> table;
    return table;
}

// Per-buffer keys ("buf:<id>:<ft>", mep.syntax_highlight) give every open
// buffer its own incremental tree, so switching between two files of one
// language no longer reparses each from scratch -- but a buffer's tree,
// text and row spans are tens of MB for a large file, and nothing tells
// this layer a buffer closed. So only the most recently highlighted few
// keep theirs; a closed buffer's simply ages out.
constexpr size_t kMaxBufferKeys = 8;

void TouchBufferKey(const std::string &key) {
    if (key.compare(0, 4, "buf:") != 0) return;
    static std::unordered_map<std::string, uint64_t> last_use;
    static uint64_t tick = 0;
    last_use[key] = ++tick;
    if (g_ts_background_thread) return;
    while (last_use.size() > kMaxBufferKeys) {
        auto oldest = last_use.begin();
        for (auto it = last_use.begin(); it != last_use.end(); ++it)
            if (it->second < oldest->second) oldest = it;
        TreesitterForget(oldest->first);
        last_use.erase(oldest);
    }
}
}  // namespace

uint64_t TreesitterRowsGeneration(const std::string &key) {
    MEP_TS_LOCK();
    auto it = RowHighlightTable().find(key);
    return it == RowHighlightTable().end() ? 0 : it->second.generation;
}

void TreesitterForget(const std::string &key) {
    MEP_TS_LOCK();
    if (KeysInUse().count(key)) return;  // the background highlighter is working on it
    ParseCacheTable().erase(key);
    RowHighlightTable().erase(key);
}

std::vector<TSCodeBlockRange> TreesitterMepmlCodeBlocks(const std::string &text, const std::string &cache_key) {
    std::vector<TSCodeBlockRange> out;
    auto it = LanguageTable().find("mepml");
    if (it == LanguageTable().end()) return out;
    const TSTree *tree = GetTree(cache_key.empty() ? "mepml" : cache_key, it->second.language(), text);
    if (!tree) return out;
    // Code blocks are block-level, but may sit inside sections, slides and
    // other containers: walk the whole tree, not only the root's children.
    std::vector<TSNode> stack{ts_tree_root_node(tree)};
    std::vector<TSCodeBlockRange> found;
    while (!stack.empty()) {
        const TSNode node = stack.back();
        stack.pop_back();
        if (std::strcmp(ts_node_type(node), "code_block") == 0) {
            TSCodeBlockRange b;
            bool has_body = false;
            const uint32_t n = ts_node_child_count(node);
            for (uint32_t i = 0; i < n; ++i) {
                const TSNode c = ts_node_child(node, i);
                const char *type = ts_node_type(c);
                if (std::strcmp(type, "info") == 0) {
                    const TSNode lang = ts_node_child_by_field_name(c, "language", 8);
                    if (!ts_node_is_null(lang))
                        b.lang = text.substr(ts_node_start_byte(lang), ts_node_end_byte(lang) - ts_node_start_byte(lang));
                } else if (std::strcmp(type, "code_content") == 0) {
                    const TSPoint s0 = ts_node_start_point(c), e0 = ts_node_end_point(c);
                    b.first_row = static_cast<int>(s0.row);
                    b.last_row = static_cast<int>(e0.column == 0 && e0.row > s0.row ? e0.row - 1 : e0.row);
                    has_body = b.last_row >= b.first_row;
                }
            }
            if (has_body) found.push_back(std::move(b));
            continue;  // a code block holds no code blocks
        }
        const uint32_t n = ts_node_child_count(node);
        for (uint32_t i = n; i > 0; --i) stack.push_back(ts_node_child(node, i - 1));
    }
    std::sort(found.begin(), found.end(), [](const TSCodeBlockRange &a, const TSCodeBlockRange &b) { return a.first_row < b.first_row; });
    out = std::move(found);
    return out;
}

const std::vector<std::vector<TSHighlightSpan>> &TreesitterHighlightRows(const std::string &filetype,
                                                                        const std::string &text,
                                                                        const std::string &cache_key) {
    const std::string &key = cache_key.empty() ? filetype : cache_key;
    RowHighlightState &st = [&]() -> RowHighlightState & {
        MEP_TS_LOCK();
        TouchBufferKey(key);  // (before taking `st`: it may evict another key's entry)
        return RowHighlightTable()[key];
    }();
    const size_t n_rows = static_cast<size_t>(std::count(text.begin(), text.end(), '\n')) + 1;
    auto from_spans = [&](const std::vector<TSHighlightSpan> &spans) {
        st.rows.assign(n_rows, {});
        for (const TSHighlightSpan &sp : spans)
            if (sp.row >= 0 && static_cast<size_t>(sp.row) < n_rows) st.rows[static_cast<size_t>(sp.row)].push_back(sp);
    };

    const TSLanguage *language = nullptr;
    const char *query_source = nullptr;
    // Markdown is two grammars: the block grammar is the tree kept here, and
    // each paragraph's inline grammar is layered on by CollectMarkdownInlineSpans
    // -- for the changed rows' paragraphs only, on the incremental path below,
    // where it used to be the whole-text pass on every keystroke.
    const bool markdown = filetype == "md" || filetype == "markdown";
    if (auto it = LanguageTable().find(filetype); it != LanguageTable().end()) {
        language = it->second.language();
        query_source = it->second.query_source;
    }
#if !defined(__EMSCRIPTEN__)
    else if (auto dit = DynamicLanguageTable().find(filetype); dit != DynamicLanguageTable().end()) {
        language = LoadDynamicLanguage(dit->second.canonical_name);
        query_source = dit->second.query_source;
    }
#endif
    const TSQuery *query = language ? QueryFor(language, query_source) : nullptr;
    // Markdown's second (inline) grammar, or anything without a plain
    // grammar and query: the whole-text pass, grouped by row.
    if (!query) {
        // Unchanged text (a cursor move, a frame re-running the hook) is
        // free here as it is on the incremental path below. Without this
        // a 20k-line Markdown file paid ~260 ms -- a whole block parse
        // plus an inline parse of every paragraph -- on every call
        // (plans/PERFORMANCE_PLAN.md E4).
        if (st.fallback_valid && st.fallback_filetype == filetype && st.text == text) return st.rows;
        st.generation++;
        from_spans(TreesitterHighlight(filetype, text, cache_key));
        st.text = text;
        st.fallback_valid = true;
        st.fallback_filetype = filetype;
        if (st.tree) ts_tree_delete(st.tree);
        st.tree = nullptr;
        return st.rows;
    }
    st.fallback_valid = false;
    const TSTree *tree = GetTree(key, language, text);
    if (!tree) {
        st.generation++;
        st.rows.clear();
        return st.rows;
    }
    if (st.tree && st.language == language && st.query == query && st.text == text) return st.rows;
    st.generation++;

    // Every capture in rows [lo, hi): the query's, then Markdown's inline layer.
    auto collect = [&](std::vector<RawSpan> &raw, uint32_t lo, uint32_t hi) {
        CollectRawSpans(query, ts_tree_root_node(tree), text, raw, lo, hi);
        if (markdown) CollectMarkdownInlineSpans(ts_tree_root_node(tree), text, raw, lo, hi);
    };
    auto full = [&] {
        std::vector<RawSpan> raw;
        collect(raw, 0, UINT32_MAX);
        SortRawSpans(raw);
        std::vector<TSHighlightSpan> spans;
        ClipRawSpans(raw, text, spans);
        from_spans(spans);
    };

    if (!st.tree || st.language != language || st.query != query) {
        full();
    } else {
        // Rows that can differ: the edited rows (the text diff) and every
        // range whose syntax changed (an opened string or comment reaches
        // far past the edit), in the new text's rows.
        const TSInputEdit edit = ComputeEdit(st.text, text);
        ts_tree_edit(st.tree, &edit);
        std::vector<std::pair<uint32_t, uint32_t>> spans_of_rows = {{edit.start_point.row, edit.new_end_point.row}};
        uint32_t count = 0;
        TSRange *changed = ts_tree_get_changed_ranges(st.tree, tree, &count);
        for (uint32_t i = 0; i < count; ++i) spans_of_rows.emplace_back(changed[i].start_point.row, changed[i].end_point.row);
        free(changed);
        // Markdown: an inline construct (emphasis, a link, a code span) can
        // run across a paragraph's lines without the block tree changing,
        // so a changed row's whole paragraph is re-highlighted -- every
        // (inline) node the rows touch, start to end.
        if (markdown) {
            const TSQuery *finder = QueryFor(tree_sitter_markdown(), "(inline) @inline");
            for (auto &iv : spans_of_rows) {
                if (!finder) break;
                TSQueryCursor *qc = ts_query_cursor_new();
                ts_query_cursor_set_point_range(qc, TSPoint{iv.first, 0}, TSPoint{iv.second + 1, 0});
                ts_query_cursor_exec(qc, finder, ts_tree_root_node(tree));
                TSQueryMatch m;
                while (ts_query_cursor_next_match(qc, &m)) {
                    if (m.capture_count == 0) continue;
                    iv.first = std::min(iv.first, ts_node_start_point(m.captures[0].node).row);
                    iv.second = std::max(iv.second, ts_node_end_point(m.captures[0].node).row);
                }
                ts_query_cursor_delete(qc);
            }
        }
        std::sort(spans_of_rows.begin(), spans_of_rows.end());
        std::vector<std::pair<uint32_t, uint32_t>> merged;
        size_t affected = 0;
        for (auto iv : spans_of_rows) {
            iv.second = std::min<uint32_t>(iv.second, static_cast<uint32_t>(n_rows - 1));
            if (iv.first > iv.second) continue;
            if (!merged.empty() && iv.first <= merged.back().second + 1) merged.back().second = std::max(merged.back().second, iv.second);
            else merged.push_back(iv);
        }
        for (const auto &iv : merged) affected += iv.second - iv.first + 1;
        if (affected * 2 > n_rows) {
            full();
        } else {
            // Unaffected rows carry over, those after the edit shifted by
            // the lines it added or removed.
            const long delta = static_cast<long>(edit.new_end_point.row) - static_cast<long>(edit.old_end_point.row);
            std::vector<std::vector<TSHighlightSpan>> rows(n_rows);
            for (size_t r = 0; r < n_rows; ++r) {
                long from = -1;
                if (r < edit.start_point.row) from = static_cast<long>(r);
                else if (r > edit.new_end_point.row) from = static_cast<long>(r) - delta;
                if (from < 0 || static_cast<size_t>(from) >= st.rows.size()) continue;
                rows[r] = std::move(st.rows[static_cast<size_t>(from)]);
                for (TSHighlightSpan &sp : rows[r]) sp.row = static_cast<int>(r);
            }
            for (const auto &iv : merged) {
                for (uint32_t r = iv.first; r <= iv.second; ++r) rows[r].clear();
                std::vector<RawSpan> raw;
                collect(raw, iv.first, iv.second + 1);
                SortRawSpans(raw);
                std::vector<TSHighlightSpan> spans;
                ClipRawSpans(raw, text, spans);
                for (TSHighlightSpan &sp : spans)
                    if (sp.row >= static_cast<int>(iv.first) && sp.row <= static_cast<int>(iv.second))
                        rows[static_cast<size_t>(sp.row)].push_back(std::move(sp));
            }
            st.rows = std::move(rows);
        }
    }
    st.text = text;
    st.language = language;
    st.query = query;
    if (st.tree) ts_tree_delete(st.tree);
    st.tree = ts_tree_copy(tree);
    return st.rows;
}

bool TreesitterHasFoldQuery(const std::string &filetype) {
    return FoldQueryTable().count(filetype) != 0;
}

std::vector<TSFoldRange> TreesitterFoldRanges(const std::string &filetype, const std::string &text) {
    std::vector<TSFoldRange> out;
    auto it = FoldQueryTable().find(filetype);
    if (it == FoldQueryTable().end()) return out;
    const LangEntry &entry = it->second;
    const TSQuery *query = QueryFor(entry.language(), entry.query_source);
    if (!query) return out;
    // Same cache_key ("filetype") TreesitterHighlight itself uses --
    // when a fold pass runs right after a highlight pass for the same
    // buffer (mep's actual call pattern, see kBuiltinSyntax), this reuses
    // that already-current tree outright with no reparse at all.
    const TSTree *tree = GetTree(filetype, entry.language(), text);
    if (!tree) return out;

    TSQueryCursor *cursor = ts_query_cursor_new();
    ts_query_cursor_exec(cursor, query, ts_tree_root_node(tree));
    TSQueryMatch match;
    while (ts_query_cursor_next_match(cursor, &match)) {
        for (uint16_t c = 0; c < match.capture_count; c++) {
            TSNode n = match.captures[c].node;
            TSPoint sp = ts_node_start_point(n);
            TSPoint ep = ts_node_end_point(n);
            int end_row = static_cast<int>(ep.row);
            // A node whose range trails its content with a consumed
            // newline (org's `section`, e.g.) ends at column 0 of the
            // row *after* its real last line, not on that line itself --
            // every brace/indent-delimited node the other fold queries
            // capture ends mid-line instead, so this was previously
            // silent for them and only ever mattered once org's own
            // query (kFoldsOrg) started running through here.
            if (ep.column == 0 && end_row > 0) end_row--;
            if (end_row <= static_cast<int>(sp.row)) continue;  // single-line node: nothing to fold
            TSFoldRange fr;
            fr.start_row = static_cast<int>(sp.row);
            fr.end_row = end_row;
            out.push_back(fr);
        }
    }
    ts_query_cursor_delete(cursor);
    return out;
}

bool TreesitterHasStructureQuery(const std::string &filetype) {
    if (StructureQueryTable().count(filetype)) return true;
#if !defined(__EMSCRIPTEN__)
    auto it = DynamicStructureQueryTable().find(filetype);
    if (it != DynamicStructureQueryTable().end()) return LoadDynamicLanguage(it->second.canonical_name) != nullptr;
#endif
    return false;
}

namespace {

// One raw `@definition.<kind>`/`@name` match pair, before dedup/nesting.
struct RawStructureEntry {
    uint32_t def_start_byte = 0;
    uint32_t def_end_byte = 0;
    int def_start_row = 0;
    int def_end_row = 0;
    int name_row = 0;
    int name_col = 0;
    std::string name;
    std::string kind;
};

/**
 * @brief Runs `query` over every match under `root` and, for each match
 * with both a `@definition.<kind>` and a `@name` capture, appends a
 * RawStructureEntry describing it (skipping matches missing either half,
 * or whose name text is empty).
 * @param query The compiled structure query to run.
 * @param root The root node to search under.
 * @param text The full source text captures' nodes index into.
 * @param out Vector to append every usable RawStructureEntry to.
 */
void CollectStructureEntries(const TSQuery *query, const TSNode &root, const std::string &text,
                              std::vector<RawStructureEntry> &out) {
    TSQueryCursor *cursor = ts_query_cursor_new();
    ts_query_cursor_exec(cursor, query, root);
    TSQueryMatch match;
    while (ts_query_cursor_next_match(cursor, &match)) {
        // #eq?/#any-of? and the rest, as for highlights: R's outline tells
        // setClass(...) from any other call by its function's name.
        if (!EvalPredicates(query, match, text)) continue;
        TSNode def_node{};
        bool has_def = false;
        std::string kind;
        TSNode name_node{};
        bool has_name = false;
        for (uint16_t c = 0; c < match.capture_count; c++) {
            const TSQueryCapture &cap = match.captures[c];
            uint32_t nlen = 0;
            const char *cname = ts_query_capture_name_for_id(query, cap.index, &nlen);
            std::string capture_name(cname, nlen);
            if (capture_name == "name") {
                name_node = cap.node;
                has_name = true;
            } else if (capture_name.rfind("definition.", 0) == 0) {
                def_node = cap.node;
                has_def = true;
                kind = capture_name.substr(std::strlen("definition."));
            }
        }
        if (!has_def || !has_name) continue;  // a pattern missing either half isn't a usable outline entry
        RawStructureEntry e;
        e.def_start_byte = ts_node_start_byte(def_node);
        e.def_end_byte = ts_node_end_byte(def_node);
        e.def_start_row = static_cast<int>(ts_node_start_point(def_node).row);
        e.def_end_row = static_cast<int>(ts_node_end_point(def_node).row);
        TSPoint np = ts_node_start_point(name_node);
        e.name_row = static_cast<int>(np.row);
        e.name_col = static_cast<int>(np.column);
        uint32_t name_s = ts_node_start_byte(name_node);
        uint32_t name_e = std::min(ts_node_end_byte(name_node), static_cast<uint32_t>(text.size()));
        e.name = name_e > name_s ? text.substr(name_s, name_e - name_s) : "";
        e.kind = std::move(kind);
        if (e.name.empty()) continue;
        out.push_back(std::move(e));
    }
    ts_query_cursor_delete(cursor);
}

}  // namespace

std::vector<TSStructureNode> TreesitterStructure(const std::string &filetype, const std::string &text) {
    std::vector<TSStructureNode> out;

    const TSLanguage *language = nullptr;
    const char *query_source = nullptr;
    if (auto it = StructureQueryTable().find(filetype); it != StructureQueryTable().end()) {
        language = it->second.language();
        query_source = it->second.query_source;
    } else {
#if !defined(__EMSCRIPTEN__)
        auto dit = DynamicStructureQueryTable().find(filetype);
        if (dit == DynamicStructureQueryTable().end()) return out;
        language = LoadDynamicLanguage(dit->second.canonical_name);
        if (!language) return out;
        query_source = dit->second.query_source;
#else
        return out;
#endif
    }

    const TSQuery *query = QueryFor(language, query_source);
    if (!query) return out;
    // Same cache_key ("filetype") every other Treesitter* entry point
    // uses -- a structure pass right after a highlight/fold pass for the
    // same buffer reuses that already-current tree with no reparse.
    const TSTree *tree = GetTree(filetype, language, text);
    if (!tree) return out;

    std::vector<RawStructureEntry> raw;
    CollectStructureEntries(query, ts_tree_root_node(tree), text, raw);

    // Dedup exact-same-node matches (two patterns landing on one
    // definition, e.g. Go's generic `type` pattern and its more specific
    // struct/interface pattern -- see treesitter_structure_queries.h's
    // top comment): group by byte span, keep the first non-"type" kind
    // seen for that span, else the first entry outright.
    // Comparator: orders entries by ascending (start_byte, end_byte).
    std::sort(raw.begin(), raw.end(), [](const RawStructureEntry &a, const RawStructureEntry &b) {
        if (a.def_start_byte != b.def_start_byte) return a.def_start_byte < b.def_start_byte;
        return a.def_end_byte < b.def_end_byte;
    });
    std::vector<RawStructureEntry> deduped;
    for (RawStructureEntry &e : raw) {
        if (!deduped.empty() && deduped.back().def_start_byte == e.def_start_byte &&
            deduped.back().def_end_byte == e.def_end_byte) {
            if (deduped.back().kind == "type" && e.kind != "type") deduped.back() = std::move(e);
            continue;
        }
        deduped.push_back(std::move(e));
    }

    // Nesting depth via a containment stack: sorted by start_byte already
    // (a valid pre-order for properly-nested definitions -- a parent's
    // definition always starts before any of its members' own), pop every
    // ancestor whose span ends before this entry starts, then depth is
    // however many ancestors remain open.
    std::vector<uint32_t> stack_end_bytes;
    for (const RawStructureEntry &e : deduped) {
        while (!stack_end_bytes.empty() && stack_end_bytes.back() <= e.def_start_byte) stack_end_bytes.pop_back();
        TSStructureNode node;
        node.row = e.name_row;
        node.col = e.name_col;
        node.start_row = e.def_start_row;
        node.end_row = e.def_end_row;
        node.depth = static_cast<int>(stack_end_bytes.size());
        node.name = e.name;
        node.kind = e.kind;
        out.push_back(std::move(node));
        stack_end_bytes.push_back(e.def_end_byte);
    }
    return out;
}

// --- Off-thread highlighting (plans/PERFORMANCE_PLAN.md 2.7) ---------------
//
// Typing into a 60k-line C++ file spent ~30 ms of every highlight pass in
// tree-sitter's incremental reparse: where the grammar meets code it cannot
// parse (embedded JS in a macro, say), error recovery makes the reparse
// linear in the file's size, and the UI thread waited for it. So a key
// whose pass has been slow is highlighted on a background thread instead:
// the newest text asked for is parsed there (a request not yet started is
// replaced by a newer one), the finished rows are copied out, and the main
// loop is woken to apply them. Small files never leave the UI thread.

namespace {

constexpr double kAsyncThresholdMs = 8.0;   // half a 60 Hz frame
constexpr uint64_t kAsyncGenerationBase = uint64_t{1} << 40;  // apart from the row cache's own counters

struct AsyncDone {
    std::string text;
    size_t lines = 0;
    uint64_t generation = 0;
    std::shared_ptr<const std::vector<std::vector<TSHighlightSpan>>> rows;
};

struct AsyncJob {
    std::string filetype;
    std::string key;
    std::string text;
};

struct AsyncHighlighter {
    std::mutex m;
    std::condition_variable cv;
    bool busy = false;
    std::string busy_key;  // the key being (or about to be) highlighted off-thread
    bool stop = false;
    bool result_waiting = false;
    bool has_job = false;
    AsyncJob job;
    std::unordered_map<std::string, double> cost_ms;  // the last pass's time, per key
    std::unordered_map<std::string, AsyncDone> done;
    uint64_t next_generation = kAsyncGenerationBase;
#if !defined(__EMSCRIPTEN__)
    std::thread worker;

    void Run() {
        g_ts_background_thread = true;
        std::unique_lock<std::mutex> lock(m);
        while (true) {
            cv.wait(lock, [&] { return stop || has_job; });
            if (stop) return;
            AsyncJob j = std::move(job);
            has_job = false;
            busy = true;
            busy_key = j.key;
            lock.unlock();
            AsyncDone d;
            const auto t0 = std::chrono::steady_clock::now();
            const uint64_t before = TreesitterRowsGeneration(j.key);
            {
                MEP_TS_LOCK();
                KeysInUse().insert(j.key);
            }
            d.rows = std::make_shared<const std::vector<std::vector<TSHighlightSpan>>>(
                TreesitterHighlightRows(j.filetype, j.text, j.key));
            {
                MEP_TS_LOCK();
                KeysInUse().erase(j.key);
            }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            d.lines = static_cast<size_t>(std::count(j.text.begin(), j.text.end(), '\n')) + 1;
            d.text = std::move(j.text);
            lock.lock();
            d.generation = ++next_generation;
            // (Only a pass that did the work says what it costs -- see below.)
            if (TreesitterRowsGeneration(j.key) != before) cost_ms[j.key] = ms;
            done[j.key] = std::move(d);
            // (A closed buffer's result ages out with its tree; see kMaxBufferKeys.)
            while (done.size() > 8) done.erase(done.begin());
            busy = false;
            busy_key.clear();
            result_waiting = true;
            lock.unlock();
            mep::NoteActivity();
            mep::WakeMainLoop();
            lock.lock();
        }
    }

    ~AsyncHighlighter() {
        {
            std::lock_guard<std::mutex> lock(m);
            stop = true;
        }
        cv.notify_all();
        if (worker.joinable()) worker.join();
    }
#endif
};

AsyncHighlighter &Async() {
    static AsyncHighlighter a;
    return a;
}

}  // namespace

TSRowsAnswer TreesitterHighlightRowsMaybeAsync(const std::string &filetype, const std::string &text,
                                               const std::string &cache_key) {
    const std::string key = cache_key.empty() ? filetype : cache_key;
    AsyncHighlighter &a = Async();
    std::unique_lock<std::mutex> lock(a.m);
    auto cost = a.cost_ms.find(key);
#if !defined(__EMSCRIPTEN__)
    const bool async = cost != a.cost_ms.end() && cost->second > kAsyncThresholdMs;
#else
    const bool async = false;
#endif
    const bool worker_owns = (a.busy && a.busy_key == key) || (a.has_job && a.job.key == key);
    if (!async && !worker_owns) {
        // Cheap enough to do now, on this thread, as it always was -- for
        // any key but the one the background thread is working on.
        lock.unlock();
        const auto t0 = std::chrono::steady_clock::now();
        const uint64_t before = TreesitterRowsGeneration(key);
        const auto &rows = TreesitterHighlightRows(filetype, text, cache_key);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        lock.lock();
        // Only an incremental pass that did the work says what typing costs:
        // a call on unchanged text returns at once (counting it flipped a
        // slow key back to this thread for its next real pass), and the
        // first, from-scratch parse of a file is a one-off.
        if (before != 0 && TreesitterRowsGeneration(key) != before) a.cost_ms[key] = ms;
        // (Not owned: the row cache's own vector, read before the next call.)
        return {std::shared_ptr<const std::vector<std::vector<TSHighlightSpan>>>(&rows, [](const void *) {}),
                TreesitterRowsGeneration(key), false};
    }
#if !defined(__EMSCRIPTEN__)
    auto d = a.done.find(key);
    if (d != a.done.end() && d->second.text == text) return {d->second.rows, d->second.generation, false};
    // The row cache may already describe exactly this text (the same edit
    // seen again by another hook): answer from it, no job.
    if (!worker_owns) {
        MEP_TS_LOCK();
        auto it = RowHighlightTable().find(key);
        if (it != RowHighlightTable().end() && it->second.text == text && it->second.generation != 0) {
            const auto *rows = &it->second.rows;
            return {std::shared_ptr<const std::vector<std::vector<TSHighlightSpan>>>(rows, [](const void *) {}),
                    it->second.generation, false};
        }
    }
    // Ask for this text (replacing a request not yet started).
    a.job = {filetype, key, text};
    a.has_job = true;
    if (!a.worker.joinable()) a.worker = std::thread([&a] { a.Run(); });
    a.cv.notify_one();
    // Meanwhile, the newest finished rows -- if they still line up with the
    // text row for row (no line added or removed since), at worst an edited
    // line's colours trail by one pass; otherwise nothing, and the current
    // decorations (which follow line edits on their own) stay until the
    // answer for this text arrives.
    if (d != a.done.end() && d->second.lines == static_cast<size_t>(std::count(text.begin(), text.end(), '\n')) + 1)
        return {d->second.rows, d->second.generation, true};
#endif
    return {nullptr, 0, true};
}

bool TreesitterAsyncResultWaiting() {
    AsyncHighlighter &a = Async();
    std::lock_guard<std::mutex> lock(a.m);
    const bool waiting = a.result_waiting;
    a.result_waiting = false;
    return waiting;
}

