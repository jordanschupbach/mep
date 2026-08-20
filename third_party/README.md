# third_party/

Vendored source for Treesitter (Phase 19, `NVIM_PARITY_PLAN.md`): the
runtime library plus a small **core set** of grammars, compiled
directly into mep. Only what actually gets compiled is kept —
`parser.c`, an optional external scanner (`scanner.c`/`scanner.cc`),
and the grammar's own copy of `tree_sitter/parser.h` — not the full
upstream repo (docs, language bindings, test corpora,
`grammar.js`/`grammar.json`, examples, ...).

**This is deliberately a small set, not "as many languages as
possible".** Every language beyond the core set is resolved at runtime
instead, by `dlopen()`ing a `.so` the user already has on disk — see
"Everything else: dynamic loading" below. mep went through a "vendor
everything" phase first (~55 grammars, ~340MB) before settling here;
that history — and why it didn't stick — is worth knowing if you're
tempted to grow this list back:

1. Fetching every grammar via CMake `FetchContent` at configure time
   didn't scale: a clean `build/` directory meant tens of minutes of
   sequential git clones, and once, catastrophically, hours —
   `tree-sitter-ocaml`'s `examples/` directory vendors several huge,
   unrelated upstream projects (`js_of_ocaml`, `dune`, ...) as git
   submodules, and a plain recursive clone pulled gigabytes of them in
   just to reach the handful of files under `src/` that matter.
2. Switching to committing the trimmed sources directly (this
   directory's current mechanism) fixed the build-time problem, but at
   ~55 grammars added ~340MB to the repository's working tree —
   compressed size for network transfer was actually small (~15MB,
   these generated parser tables are extremely repetitive), but the
   on-disk footprint after checkout was real.
3. Decided that wasn't the right tradeoff for languages beyond a small
   core: most users only ever edit a handful of languages regularly, and
   for everything else, dynamically loading whatever grammar is already
   on the machine (via Nix, the tree-sitter CLI, or an existing
   nvim-treesitter install) gets the same result — real Treesitter
   highlighting — without paying for it in the repository.

## The core set

C, C++, Lua (mep's own scripting/config language — `~/.config/mep/init.lua`
and any `.lua` file need this regardless of what else is available),
Python, JavaScript, Markdown (+ its separate inline-formatting grammar),
Org, and R.

| name | repo | tag/commit | subdir |
|---|---|---|---|
| c | https://github.com/tree-sitter/tree-sitter-c | v0.24.2 | . |
| cpp | https://github.com/tree-sitter/tree-sitter-cpp | v0.23.4 | . |
| lua | https://github.com/tree-sitter-grammars/tree-sitter-lua | v0.5.0 | . |
| python | https://github.com/tree-sitter/tree-sitter-python | v0.25.0 | . |
| javascript | https://github.com/tree-sitter/tree-sitter-javascript | v0.25.0 | . |
| markdown | https://github.com/tree-sitter-grammars/tree-sitter-markdown | v0.5.3 | tree-sitter-markdown |
| markdown_inline | https://github.com/tree-sitter-grammars/tree-sitter-markdown | v0.5.3 | tree-sitter-markdown-inline |
| org | https://github.com/nvim-orgmode/tree-sitter-org | 2.0.4 | . |
| r | https://github.com/r-lib/tree-sitter-r | v1.3.0 | . |

To add another grammar to this compiled-in set: fetch its `src/`
directory (dropping `grammar.json`/`node-types.json`) into
`third_party/grammars/<name>/src/`, add `<name>` to `TS_GRAMMAR_NAMES` in
`CMakeLists.txt`, add its file extension(s) to `LanguageTable()` in
`src/treesitter.cpp`, and embed its `queries/highlights.scm` in
`src/treesitter_queries.h`. Think twice first, though — see "Everything
else" below for why the dynamic path is usually the better fit.

## Everything else: dynamic loading

Every other filetype `src/treesitter.cpp` has a highlight query embedded
for (`DynamicLanguageTable`, `src/treesitter_queries.h` — around 40
languages: Rust, Go, TypeScript, YAML, Nix, Zig, and more) is resolved
at runtime by `dlopen()`ing a `<canonical-name>.so` and pulling its
`tree_sitter_<canonical_name>` symbol (`LoadDynamicLanguage` in
`src/treesitter.cpp`), searched for in order:

1. `$MEP_TS_PARSER_PATH` (colon-separated, like `$PATH`) — explicit,
   highest priority. `flake.nix`'s devShell exports this pointing at a
   `pkgs.tree-sitter.withPlugins` bundle, so entering this project's own
   `nix develop` gets ~35 more languages for free with no manual setup —
   extend the list in `flake.nix` for anything missing.
2. The tree-sitter CLI's own build cache
   (`$XDG_CACHE_HOME/tree-sitter/lib`, default
   `~/.cache/tree-sitter/lib`) — what `tree-sitter build` populates.
3. nvim-treesitter's installed-parser directory
   (`$XDG_DATA_HOME/nvim/site/parser`, default
   `~/.local/share/nvim/site/parser`) — a common pre-existing source for
   anyone who already uses Neovim with nvim-treesitter. (Confirmed
   working against a real installed set during development — Rust,
   Ruby, and others picked up automatically with zero configuration.)
4. mep's own config-owned directory
   (`$XDG_CONFIG_HOME/mep/parsers`, default `~/.config/mep/parsers`),
   mirroring the existing `~/.config/mep/init.lua` convention — for a
   grammar built by hand (`tree-sitter build`) with nowhere else to go.

This path is **native builds only**. The Emscripten/wasm build has no
filesystem access to arbitrary shared libraries in a browser, so it's
compiled out entirely there (`#if !defined(__EMSCRIPTEN__)` in
`src/treesitter.cpp`) — only the core set above is ever available in
the browser build.

## tree-sitter runtime

| | |
|---|---|
| Source | https://github.com/tree-sitter/tree-sitter |
| Tag | `v0.26.12` |
| Vendored | `lib/src/` (the `lib.c` amalgamation and everything it `#include`s), `lib/include/tree_sitter/` |

Grammars considered but never vendorable at all (upstream ships no
generated `parser.c`, only `grammar.js`/`grammar.json` — would need the
tree-sitter CLI + a Node/Rust toolchain to compile, which is exactly the
kind of extra build-time dependency this project avoids): `tree-sitter-sql`
(derekstride), `tree-sitter-latex` (latex-lsp), `tree-sitter-swift`
(alex-pinkus). Not in `DynamicLanguageTable` either (no `highlights.scm`
was ever fetched for them) — SQL/LaTeX/Swift files render through
`kBuiltinSyntax`'s hand-rolled fallback lexer today, not Treesitter.
Adding one is mechanical: fetch its `queries/highlights.scm`, embed it in
`src/treesitter_queries.h`, and add a `DynamicLanguageTable` entry in
`src/treesitter.cpp` naming the grammar's `tree_sitter_<name>` symbol —
no local grammar source needed, since Nix (or the tree-sitter CLI) does
the actual compiling.
