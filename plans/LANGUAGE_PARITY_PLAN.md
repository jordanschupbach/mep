# Language parity: run, grammar, LSP, UI mode, format for every example language

Goal: every language in `examples/language/` has all five pillars, each
mep's own:

1. **Run** -- `<leader>rr` / `gr` runs the example as-is.
2. **Grammar** -- an in-house tree-sitter grammar in
   `grammars/tree-sitter-<name>/`, compiled in, with highlights, folds and
   structure queries.
3. **LSP** -- an in-house language server, `mep-<lang>-lsp`, the default
   for the filetype.
4. **UI mode** -- `<leader>uu` lays out a live console plus panels, like
   the R, Python, C/C++ and Common Lisp modes.
5. **Format** -- `gf` runs a formatter built into mep (a `builtin` entry in
   `mep.format_languages`), shaped after the language's standard tool and
   checked against it on the corpus. (Added 2026-10-21, after bash.)

Languages were first done one at a time in alphabetical order (bash ...
csharp, then sql). From 2026-11-07 the remaining work is **Grammar, LSP
and Format only** -- Run already exists everywhere and UI modes are out of
scope for now -- in the order of section 4, which groups languages that
share machinery. Within a language: grammar, then LSP, then format (the
formatter usually prints the LSP's own parse tree, and the LSP's
`textDocument/formatting` runs the formatter).

Status legend: `[ ]` not started, `[~]` in progress, `[x]` done,
`[-]` not applicable.

---------------------------------------------------------------------------

## 1. Where things stand (2026-11-07)

Checked against the code, not the old table: grammars = `mep_grammar(...)`
in CMakeLists.txt, LSPs = `mep.bundled_tool('mep-<x>-lsp')` in main.cpp,
formatters = `builtin = 'format_<x>'` entries in `mep.format_languages`.

| Example | Run | Grammar | LSP | UI | Format | Wave (sec. 4) | Today, where not ours |
|---|---|---|---|---|---|---|---|
| bash | [x] | [x] | [x] | [x] | [x] | done |  |
| c | [x] | [x] | [x] | [x] | [x] | done |  |
| clojure | [x] | [x] | [x] | [x] | [x] | done |  |
| common_lisp | [x] | [x] | [x] | [x] | [x] | done |  |
| cpp | [x] | [x] | [x] | [x] | [x] | done |  |
| crystal | [x] | [x] | [x] | [x] | [x] | done |  |
| csharp | [x] | [x] | [x] | [x] | [x] | done |  |
| python | [x] | [x] | [x] | [x] | [x] | done |  |
| r | [x] | [x] | [x] | [x] | [x] | done |  |
| sql | [x] | [x] | [x] | [x] | [x] | done |  |
| orgmode | [x] | [x] | [x] | [-] | [-] | done | a document, not a language with a console |
| notebook | [-] | [-] | [-] | [-] | [-] | -- | `.ipynb` is a view over Python cells |
| nix | [x] | [x] | [x] | [ ] | [x] | done |  |
| lua | [x] | [x] | [x] | [ ] | [x] | done |  |
| maxima | [x] | [x] | [x] | [ ] | [x] | done |  |
| javascript | [x] | [x] | [x] | [ ] | [x] | done |  |
| typescript | [x] | [x] | [x] | [ ] | [x] | done |  |
| scheme | [x] | [x] | [x] | [ ] | [x] | done |  |
| fish | [x] | [x] | [x] | [ ] | [x] | done |  |
| tcl | [x] | [x] | [x] | [ ] | [x] | done |  |
| octave | [x] | [x] | [x] | [ ] | [x] | done |  |
| julia | [x] | [x] | [x] | [ ] | [x] | done | LanguageServer.jl registered as the alternative; Runic via `{'runic'}` |
| fortran | [x] | [x] | [x] | [ ] | [x] | 4 | fortls (kept, `filetypes = {}`) |
| gap | [x] | [x] | [x] | [ ] | [x] | done |  |
| go | [x] | [x] | [x] | [ ] | [x] | done | gopls registered as the alternative |
| zig | [x] | [x] | [ ] | [ ] | [ ] | 5 | zls |
| rust | [x] | [x] | [ ] | [ ] | [ ] | 5 | rust-analyzer |
| d | [x] | [ ] | [ ] | [ ] | [ ] | 5 | `d.so`; serve-d |
| nim | [x] | [ ] | [ ] | [ ] | [ ] | 5 | `nim.so`; nimlsp |
| java | [x] | [ ] | [ ] | [ ] | [ ] | 6 | `java.so`; jdtls |
| kotlin | [x] | [ ] | [ ] | [ ] | [ ] | 6 | `kotlin.so`; kotlin-language-server |
| scala | [x] | [ ] | [ ] | [ ] | [ ] | 6 | `scala.so`; metals |
| groovy | [x] | [ ] | [ ] | [ ] | [ ] | 6 | `groovy.so`; none |
| dart | [x] | [ ] | [ ] | [ ] | [ ] | 6 | `dart.so`; none |
| ocaml | [x] | [ ] | [ ] | [ ] | [ ] | 7 | `ocaml.so`; ocamllsp |
| haskell | [x] | [ ] | [ ] | [ ] | [ ] | 7 | `haskell.so`; hls |
| elixir | [x] | [ ] | [ ] | [ ] | [ ] | 7 | `elixir.so`; elixir-ls |
| erlang | [x] | [ ] | [ ] | [ ] | [ ] | 7 | `erlang.so`; none |
| ruby | [x] | [ ] | [ ] | [ ] | [ ] | 8 | `ruby.so`; solargraph |
| perl | [x] | [ ] | [ ] | [ ] | [ ] | 8 | `perl.so`; Perl::LanguageServer |
| php | [x] | [ ] | [ ] | [ ] | [ ] | 8 | `php.so`; intelephense |
| latex | [x] | [ ] | [ ] | [ ] | [ ] | 9 | `latex.so`; texlab |

Remaining: 26 grammars, 27 language servers, 27 formatters.

"Run [x]" means a default exists in `mep.opt.run_button_defaults`; each
language's turn still re-checks that its example actually runs.

---------------------------------------------------------------------------

## 2. What "done" means, per pillar

The Common Lisp work (commits `e93501c`, `cb0ad10`) is the template for
all four.

### Run

- A default in `mep.opt.run_button_defaults` (`src/main.cpp`), aliases for
  the language's other extensions.
- The example runs to completion with the tool from `flake.nix`'s dev
  shell; add the tool there if it's missing.
- A row in `help/run-button.org`'s table when the command isn't obvious.

### Grammar

The method in `tools/ts_grammar_diff/README.md` (and the retired
`TREESITTER_GRAMMAR_PLAN.md`, `git show 5888949^:plans/TREESITTER_GRAMMAR_PLAN.md`):

1. Pin the node API from the dynamically loaded `.so`
   (`$MEP_TS_PARSER_PATH/<name>.so`): it is the contract, since the
   highlight query in `treesitter_queries.h` and users' own tooling use
   its node names and fields. `just grammar-symbols <name>`.
2. Write `grammar.js` (+ `src/scanner.c` if needed) from the language's
   reference, keeping the upstream node names, fields and shapes.
3. Corpus: add the language to `LANGUAGES` in `grammar_diff.py`
   (real-world sources from nixpkgs plus the repo's own files) and an
   oracle (the language's own syntax check) where one exists. `just
   grammar-diff <name> --oracle` until every difference is a file the
   language rejects or a reviewed `test/known-differences.txt` entry.
4. `just grammar-fuzz <name>` clean under ASan + UBSan; `just
   grammar-recovery <name>` no worse than the old grammar.
5. `queries/highlights.scm`, `folds.scm`, `structure.scm` beside the
   grammar (mep's convention: general patterns first); remove the old
   query from `treesitter_queries.h`.
6. Tests in `mep-treesitter-test`: highlights, folds, outline, and an
   incremental-reparse case when there's a scanner.
7. Swap in: `mep_grammar(...)` in CMakeLists.txt, the compiled-in tables in
   `treesitter.cpp` (highlight, fold, structure), drop it from
   `DynamicLanguageTable` and from `tsGrammars` in `flake.nix`, a
   `just <name>-grammar` recipe, a README, `help/syntax.org`. Check it
   live: highlighting and the structure sidebar.

### LSP

- `src/<lang>_lsp.{h,cpp}` (analysis, no I/O), `<lang>_lsp_server.cpp`
  (JSON-RPC over stdio), `<lang>_lsp_vocab.cpp` (builtin names,
  signatures, docs), `<lang>_lsp_test.cpp`. No mep_core, no tree-sitter
  dependency: built beside `mep` and found through `mep.bundled_tool`.
- Capabilities, as `mep-lisp-lsp`: diagnostics (syntax, then a lint set
  tuned on a real corpus for false positives), completion, hover,
  signature help, definition, references, document highlight, rename,
  document symbols, folding ranges, formatting, and a `--lint FILE...`
  command-line mode.
- Registered in `mep.lsp_servers` as the default for the filetype; the
  third-party server stays listed with `filetypes = {}` so a user can opt
  back in, as with clangd/pyright.
- `help/<lang>-lsp.org` (the codes table) and a line in `help/lsp.org`.

### UI mode

- `kBuiltinLanguageUi<Lang>` in `src/main.cpp`, built on
  `kBuiltinLanguageUiCommon` (`mep.language_ui_layout`): a console (the
  language's REPL or shell) below the source, panels on the right fed by
  files the console's init script rewrites after each prompt, a Help tab
  and a `gh` provider, `<leader>re` to send the form/statement at the
  cursor, `run_source` so `<leader>rr` runs into the console.
- `mep.opt.<lang>_ui_*` options, `help/<lang>-mode.org`, a row in
  `help/language-ui.org`.
- Checked live under Xvfb (`tools/pdf_live_check.py`'s driver).

### Format

- `src/<lang>_format.{h,cpp}` with `Format(text, Options) -> Result{ok,
  text, error, error_line}`, over the LSP's own parser where it has one; a
  `mep.format_<lang>` Lua builtin and a `mep.format_languages` entry (every
  extension aliased); the LSP's `textDocument/formatting` runs the same code.
- The layout of the language's standard formatter (shfmt, black,
  clang-format, cljfmt, ...): measured against it on the corpus, every
  deliberate difference written down in `help/formatting.org`.
- Never returns what it has not checked: the result is parsed again and
  compared with the input (tokens or tree); a refusal leaves the buffer
  alone with a reason and a line.
- On the corpus: idempotent everywhere, nothing the language rejects
  afterwards, no crash under ASan + UBSan; `mep-<lang>-format-test`.

---------------------------------------------------------------------------

## 3. Per language

### bash  `[x]`

- Run `[x]`: `bash <file>`.
- Grammar `[x]` (2026-10-20): `grammars/tree-sitter-bash` (README there).
  Drop-in for tree-sitter-bash 0.25.1: same 59 node types / 19 fields /
  rule order. Corpus: 3,616 files (bash-completion, git's tests, bash's
  examples and tests, nixpkgs' `.sh`, bats, asdf, dehydrated). Every file
  and top-level item the old grammar parsed cleanly is identical except 82
  reviewed improvements (`test/known-differences.txt`); files with errors
  354 -> 232; valid files failing 259 -> 116; fuzz clean; recovery -1.4%
  error bytes. Fixed an upstream misgrouping where `a | b | c` followed by
  an `x && y` line silently swallowed the second line.
  Method notes for the next ports: the old grammar's source is
  `nix build --inputs-from . nixpkgs#tree-sitter-grammars.tree-sitter-<x>.src`
  (the version the flake's `.so` was built from), passed as `--old
  <it>/src`; with fixes on top of the port, `--old-clean` is the gate.
- LSP `[x]` (2026-10-21): `mep-bash-lsp` (help/bash-lsp.org). Its own
  parser (`src/bash_parse.cpp`, from bash's grammar, not tree-sitter's) agrees
  with `bash -n` on 3,518 of the 3,522 corpus scripts bash accepts and 87 of
  the 89 it rejects, with the same message on the same line in 84 of 85 (the
  rest: bash's own test suite's deliberate corners -- `cat <<EOF)` closing a
  comsub, `index[7<(4+2)]` in `[[ ]]`, the here-document count limit).
  23 diagnostic codes, calibrated on the corpus (e.g. undefined-variable
  reports 5 names across it, two of them real bugs in nixpkgs scripts).
  Completion, hover, signature help (function parameters named from `local
  x=$1`), scope-aware definition/references/rename, outline, folds, and an
  idempotent re-indenting formatter that follows the file's own conventions
  (indent unit; `&&` chains flat or indented) -- leaves 80% of the corpus
  untouched and never breaks a script. bash-language-server is opt-in.
- UI `[x]` (2026-10-21): help/bash-mode.org. The login bash (or `bash`) with
  ~/.bashrc and an init script; Variables, Functions (click: go to the
  definition), Inspect, Jobs, History, Help (`help`/`man`/function body).
  `<leader>rr` sources the file into the console with `exit` and `set -e`
  neutralized and options restored; `<leader>re` (now one dispatcher for
  every mode: `mep.language_ui_eval_handlers`) sends the top-level block.

- Format `[x]` (2026-10-21): `src/bash_format.cpp` (`mep.format_bash`, the
  `sh`/`bash` entries; mep-bash-lsp's formatting now runs it too, replacing
  its re-indenter). shfmt's layout over `bash_parse`'s tree. Corpus (3,432
  scripts shfmt also reads, tabs): 3,302 files (96.2%) byte-identical to
  shfmt, 99.9% of lines; idempotent on all 3,504 formatted files in both
  modes (forced tabs, and the file's own unit -- read from its first-level
  blocks, so stable); none rejected by `bash -n` afterwards; 10 files bash
  accepts are refused, all in bash's own test suite (the self-check, or the
  parser's known disagreements); clean under ASan + UBSan. Deliberate
  differences: array subscripts not re-spaced (shfmt turns `[x86_64-linux]`
  into `[x86_64 - linux]`, which breaks nixpkgs update scripts), `${x/p}`
  not rewritten to `${x/p/}`, here-document bodies not formatted.
### c  `[x]`

Run, grammar, LSP and UI predate this plan (help/c-lsp.org, help/language-ui.org).

- Format `[x]` (2026-10-21): `src/c_format*.cpp` (`mep.format_c`, the `c`
  entry; mep-c-lsp now answers `textDocument/formatting` with it). A port
  of clang-format 21's pipeline restricted to C -- lexer, unwrapped-line
  parser (one run per `#if` combination), annotator, continuation indenter
  with the penalty search, line joiner, breakable comments/strings, the
  whitespace manager -- in LLVM style; attribution (Apache-2.0 WITH
  LLVM-exception) in `c_format_internal.h`. Corpus (`just grammar-corpus
  c`, 842 files: Lua, CPython, SQLite, tree-sitter): all 841 files
  clang-format finishes are byte-identical to `clang-format
  --style="{BasedOnStyle: LLVM, SortIncludes: Never}"` (699,944 lines);
  sqlite3.c is refused up front (252 `#if` runs over 9 MB; clang-format
  itself runs for minutes in 24 GB on it) by a runs x tokens budget known
  after the first run. Not idempotent on 8 files, exactly where
  clang-format is not, with the same second pass. Clean under ASan + UBSan
  over the corpus. Deliberate difference: includes never sorted. Not yet:
  reading a project's `.clang-format` (the argv entry puts clang-format
  back for that).

### clojure  `[x]`

- Run `[x]`: `bb <file>` (babashka).
- Grammar `[x]` (2026-10-21): `grammars/tree-sitter-clojure` (README there).
  Drop-in for sogaiu/tree-sitter-clojure 0.0.13: same 35 node types / 11
  fields / rule order, no external scanner. Corpus (`just grammar-corpus
  clojure`): 1,777 distinct files (386,570 lines) from the jars of
  clojure-lsp, babashka, clj-kondo, polylith, jet, cljfmt, plus neil -- all
  identical trees to the old grammar, none with errors; recovery on 3,000
  damaged copies +0.02% error bytes; fuzz clean (ASan + UBSan). The oracle
  is babashka's edamame (`tools/ts_grammar_diff/clojure_read.clj`);
  `grammar_diff.py` learned to unpack jars. Covers .clj/.cljs/.cljc/.cljd/
  .edn/.bb; outline query included.
- LSP `[x]` (2026-10-21): `mep-clojure-lsp` (help/clojure-lsp.org). Its own
  reader (`src/clojure_reader.cpp`) gives the JVM reader's errors, worded
  the same, and agrees with `clojure.core/read` on 4,754 of 4,755 files
  (corpus + damaged copies; `tools/clojure_reader_check.clj` is the oracle).
  clojure.core and the shipped namespaces come from a table generated from
  Clojure itself (`tools/gen_clojure_vocab.clj` ->
  `src/clojure_lsp_core_names.cpp`: arglists, docs, added, deprecated,
  private). Analysis: the `ns` form, refers/aliases, scopes and
  destructuring, core macros form by form, `#?` per platform, unknown
  macros soft-bind their vector symbols. 30-odd codes: reader errors,
  compile errors (unresolved symbol/namespace/var, arity, use before
  definition, malformed def/fn/let/if/case), and style (unused bindings,
  requires, refers, private vars; redefined vars; misplaced docstrings;
  deprecated vars...). Calibrated against clj-kondo over the corpus: the
  compile-error checks report next to nothing on code that compiles.
  Workspace index for cross-file definition, hover, `alias/` completion and
  arity; completion, hover, signature help, definition, references,
  rename, outline, folds, formatting; `--lint`. clojure-lsp is opt-in.
- UI `[x]` (2026-10-21): help/clojure-mode.org. A `clojure.main/repl` (bb,
  else `clojure -M`; under rlwrap) with prompt/eval hooks writing the panel
  files: Vars (click: inspect), Inspect (`mep-ui/view`), Namespaces,
  History, Help (`doc`, source for on-disk vars, `apropos`). `<leader>re`
  evaluates the top-level form in the namespace of the `ns` above it;
  `<leader>rr` loads the file and moves the REPL into its namespace (even
  when the load fails). Checked live under Xvfb with bb. Notes: on a tty
  bb's `main/repl` swaps in its own JLine REPL unless given `:read`, and
  needs `-Dorg.jline.terminal.provider=exec` to stop an "Arena.ofShared"
  warning per line; bb's `source-fn` on a built-in fetches bb's own
  dependencies from the network, so source is only shown for on-disk vars.
- Format `[x]` (2026-10-21): `src/clojure_format.cpp` (`mep.format_clojure`,
  every Clojure extension; mep-clojure-lsp's formatting runs it). cljfmt
  0.16's default layout: gap rules, then a top-down re-indenter with
  cljfmt's indentation table, then a token-signature self-check. Corpus
  (1,772 files that read): 1,718 (97.0%) byte-identical to cljfmt, 99.95%
  of lines; idempotent everywhere; the 5 refused are polylith's mustache
  templates (odd maps). Remaining differences are cljfmt's own corners
  (indentation inside some `#?(...)` and syntax-quoted forms, comma-only
  lines).

### common_lisp  `[x]`

Run, grammar, LSP and UI predate this plan (help/lisp-lsp.org, help/language-ui.org).

- Format `[x]` (2026-10-21): `src/lisp_format.cpp` (`mep.format_lisp`, the
  lisp/lsp/cl/asd entries; mep-lisp-lsp's formatting and the Enter key's
  indent (`src/lisp_indent.cpp`) run it). The reference is Emacs's
  `common-lisp-indent-function` (what SLIME/Sly use), via
  `tools/cl_indent_oracle.el` and `tools/cl_format_compare.py`, plus
  whitespace rules: no blanks inside parens, lone closers joined, no
  trailing blanks, at most two blank lines. Corpus (`just grammar-corpus
  commonlisp`, SBCL and its packaged libraries): 1,519 of 1,521 files
  byte-identical to Emacs, every one of 701,176 lines; idempotent
  everywhere; 2 refused (SBCL's deliberately unbalanced test data).
  Maxima 5.49's 309 source files: all byte-identical to Emacs. Clean under
  ASan + UBSan on 1,301 SBCL/Maxima files and flattened/truncated/tabbed
  copies. Deliberate differences (help/formatting.org): `#|...|#` lines
  and single-`;` comment lines keep their column; `defmethod` qualifiers
  counted from the form itself (Emacs's `beginning-of-defun` can land on
  the wrong form in an unindented file). Per-operator specs via the
  entry's `indent` table.

### crystal  `[x]`

- Run `[x]`: `crystal run <file>`; the example runs.
- Grammar `[x]` (2026-10-21): `grammars/tree-sitter-crystal` (README there),
  with an external scanner for Crystal's parser-dependent tokens. Drop-in
  for crystal-lang-tools/tree-sitter-crystal: same 152 named node types /
  41 fields / rule order. Corpus (`just grammar-corpus crystal`): 4,602
  files (compiler, stdlib, specs; crystalline, ameba, invidious, mint,
  shards, gi-crystal and the other nixpkgs Crystal projects with their
  shards) -- all identical trees to the old grammar, the same 73 with
  errors in both; recovery on 3,000 damaged copies identical to the byte;
  fuzz clean (ASan + UBSan). Oracle: `crystal tool format`'s parse
  (`tools/ts_grammar_diff/crystal_check.sh`).
- LSP `[x]` (2026-10-21): `mep-crystal-lsp` (help/crystal-lsp.org). A
  method-for-method port of the compiler's lexer, parser and AST
  (`src/crystal_syntax.cpp`, `crystal_parser.cpp`, `crystal_ast.cpp`, 1.19):
  every tree identical to the compiler's, node and location, on the 2,211
  compiler/stdlib/spec files and the 4,602-file corpus (oracle:
  `tools/crystal_ast_dump.cr`, `just crystal-ast-dump`); 1,746 of the
  parser spec's 1,747 cases (the miss is a regex PCRE2 rejects). Syntax
  errors worded as the compiler's. Stdlib table generated from `crystal
  docs` (`just crystal-vocab` -> `src/crystal_lsp_stdlib.cpp`). Lints:
  syntax, parser warnings, missing requires, unused variables/arguments/
  block parameters, shadowing, unreachable code -- 17 warnings and 1 real
  error over the compiler's own 480k lines. Workspace index; hover,
  completion (by receiver type through ancestors), signature help,
  definition, references, rename, outline, folds, formatting; `--lint`.
  crystalline is opt-in.
- UI `[x]` (2026-10-21): help/crystal-mode.org. This Crystal has no
  interpreter (`crystal i` is compiled out), so the console is mep's own
  accumulate-and-recompile REPL (a small Crystal program using the
  compiler's parser, built once per Crystal version into
  ~/.cache/mep/crystal-ui): ~1 s per statement. Panels: Variables (typed),
  Definitions, Inspect, Session, History, Help (stdlib doc comments, no
  network). `<leader>rr` runs the file in the console. Checked live under
  Xvfb.
- Format `[x]` (2026-10-21): `src/crystal_format*.cpp` (`mep.format_crystal`,
  the `cr` entry; mep-crystal-lsp formats with it). A port of the
  compiler's `formatter.cr` over mep's parser port, with a proof-read: the
  result is re-parsed and its tree and comments compared with the input's.
  All 1,128 single-line cases of `formatter_spec.cr` pass. Corpus: all
  4,602 files byte-identical to `crystal tool format`, and 4,454 of 4,456
  copies with indentation and spacing scrambled (the 146 others no longer
  parse for Crystal either); idempotent on every one. Deliberate
  difference (the other 2): a multi-line string keeps its lines' trailing
  blanks -- Crystal strips them, changing the string's value (mep used to
  refuse the file over it).

### csharp  `[x]`

- Run `[x]`: `dotnet run <file>.cs` (a .NET 10 file-based app).
- Grammar `[x]` (2026-10-21): `grammars/tree-sitter-c_sharp` (README there),
  with an external scanner for interpolated and raw strings. Drop-in for
  tree-sitter-c-sharp 0.23.5: same node types / fields / supertypes / rule
  order (its productions reproduce the old parse table exactly; the
  corrections are rules added after them). Corpus (`just grammar-corpus
  c_sharp`): 17,828 files (Roslyn's compilers, analyzers and tests;
  jellyfin, osu!, OpenRA, Pinta, ILSpy, BTCPay, Wasabi, ArchiSteamFarm,
  OmniSharp...) -- 17,752 identical trees, 76 reviewed improvements
  (`test/known-differences.txt`; `--old-clean` passes); files with errors
  156 -> 106, none new, ERROR bytes -18.6%; recovery on 3,000 damaged
  copies -0.0% error bytes; fuzz clean (ASan + UBSan). Fixed upstream
  misreadings: `{{{x}}}`, `""` before a verbatim string's closing quote,
  `$""` and escapes in a format (the scanner); `[.., x]` and `.. var rest`
  list patterns; `*(p + i)`, `*(int*)p`, `*p++` (= `*(p++)`); `[type]` and
  `with` as names; `ref readonly` function-pointer parameters; `unsafe
  ~C()`. Oracle: Roslyn's parser from the SDK (`tools/csharp_oracle`, `just
  csharp-oracle`), which accepts 17,821 of the files; of those, 101 still
  have errors, mostly `#if` branches splitting a construct (the grammar
  parses every branch), plus `async`/`await` as names and assignment to a
  ref-returning call. Highlights, folds and outline queries; checked live.
- LSP `[x]` (2026-10-21): `mep-csharp-lsp` (help/csharp-lsp.org). A
  method-for-method translation of Roslyn's lexer and parsers (`Lexer*.cs`,
  `LanguageParser*.cs`, `DirectiveParser.cs`, `DocumentationCommentParser.cs`,
  `SyntaxParser.cs` at the commit the .NET 10 SDK's compiler was built from:
  `src/csharp_lexer*.cpp`, `csharp_parser_*.cpp`, `csharp_directive_parser.cpp`,
  `csharp_doc_parser.cpp` over Roslyn-shaped green trees in `csharp_syntax.cpp`;
  node factories, kinds and error texts generated from Roslyn's Syntax.xml,
  SyntaxKind.cs and resources, `just csharp-syntax-gen`): every tree identical
  to Roslyn's -- each node, token, trivia and syntax error, spans and wording
  -- on all 17,828 corpus files and on 24,000 damaged copies of them (oracle:
  `csharp-oracle dump`, format in tools/csharp_oracle/DUMP.md; `--parse` in
  `mep-csharp-lsp-test`); ASan + UBSan clean on those 41,828 files and with
  every feature exercised on 3,000 of them. BCL table generated from the
  reference assemblies and XML docs with the SDK's own Roslyn (`just
  csharp-vocab` -> `src/csharp_lsp_bcl.cpp`: 1,874 types, 21,895 members).
  Lints, each Roslyn's own warning with its rules (constant-write CS0219, `out`
  as a use, no unreachable `throw`, `#pragma warning`): unused locals / labels /
  local functions / private fields, unreachable code, mistaken empty
  statements, duplicate usings, self-assignment and -comparison; hints for
  unused parameters, private methods and usings. Calibrated against Roslyn
  compiling each of the 1,727 corpus files that compile alone: all 27 of
  mep's warnings there are the compiler's too; over the whole corpus (5.5M
  lines) `--lint` prints 36 warnings, each real. Workspace index (types and
  members of every .cs, cached lookup tables); hover, completion (members
  through base types and interfaces, extension methods of the imported
  namespaces, namespaces), signature help, definition, references, rename,
  outline, folds, formatting (mep's C# formatter, below); `--lint`.
  OmniSharp is opt-in.
- UI `[x]` (2026-10-21): help/csharp-mode.org. The SDK has no C# REPL or
  CSharp.Scripting, but its Roslyn does interactive submissions natively,
  so the console is mep's own: each input a script compilation chained to
  the previous one, emitted to memory and run through its submission
  factory, as C# Interactive does -- state persists, nothing re-runs
  (`src/language_ui_csharp.h`: a small .NET program built once per SDK
  version into ~/.cache/mep/csharp-ui against the SDK's own Roslyn, no
  NuGet). ~4 s build once; prompt in ~0.2 s; 10-50 ms per input after a
  ~1.5 s background warm-up. Values printed as csi does; Roslyn's
  diagnostics naming the input; `IsCompleteSubmission` for multi-line
  input; rlwrap. Panels: Variables (declared types), Definitions, Inspect,
  Session, History, Help (the reference pack's XML docs through Roslyn's
  symbols, extension methods on a variable's type, no network).
  `<leader>rr` runs the file (top-level statements, or `Main`; namespaces
  unwrapped; `#:package` noted as unsupported); `<leader>re` sends the
  top-level statement/member at the cursor, found by Roslyn. Checked live
  under Xvfb.
- Format `[x]` (2026-10-21): `src/csharp_format*.cpp` (`mep.format_csharp`,
  the `cs` entry; mep-csharp-lsp's `textDocument/formatting` runs it). A port
  of Roslyn's formatting engine -- `AbstractFormatEngine` and its operation
  applier, `TokenStream`, `FormattingContext` with Roslyn's AVL interval
  trees (its quirky smallest-containing-interval search, .NET's unstable
  introsort for the indent operations), `TriviaDataFactory`,
  `AbstractTriviaFormatter`/`CSharpTriviaFormatter` (comments, doc comments,
  directives through the structured-trivia engine), the C# rules with the
  default options -- at the commit the SDK's dotnet-format was built from,
  over mep's port of Roslyn's parser; attribution (MIT) in
  `csharp_format_internal.h`. Proof-read: the result is re-parsed and must
  give the same tree, the same syntax errors and the same comments and
  directives up to whitespace. Oracle: `csharp-oracle format` (Formatter
  over an AdhocWorkspace with the SDK's Workspaces DLLs; byte-identical to
  `dotnet format whitespace --folder` on the files cross-checked, BOMs and
  CRLF included). All 324 of Roslyn's own `AssertFormatAsync(expected,
  input)` tests the oracle agrees with pass (`tools/csharp_format_cases.py`
  -> `mep-csharp-format-test`). Corpus (17,828 files): 17,827 byte-identical
  to the oracle; all 17,828 copies with indentation and spacing scrambled
  (two seeds; 17,792 of them changed by formatting) byte-identical too;
  idempotent on every corpus file -- on 613 scrambled copies a second pass
  moves a continued argument's contents again, exactly as Roslyn's own second
  pass does (byte-identical); never a changed tree (the proof-read never
  refused); clean under ASan + UBSan (leaks included) on all 35,656 plain and
  scrambled files; 6.5 MB file in 1.8 s. `indent`/`tabs` options match
  `dotnet format` with the same `.editorconfig`. Like dotnet format, files
  with syntax errors are formatted (space next to missing tokens untouched)
  and a BOM is kept. Deliberate difference (the 1 corpus file): a file that
  is not valid UTF-8 is refused (dotnet format writes U+FFFD over the bytes;
  the file is Shift-JIS). Not yet: reading a project's `.editorconfig`
  (help/formatting.org has the dotnet format entry for that).

### sql  `[x]`

Dialect: SQLite 3 (the dev shell's `sqlite3`, also the Run button's). A
`-- database: PATH` line among a file's leading comments ties it to a
database file: the language server reads its schema, the UI mode's console
opens it. Example: `examples/sql/bookstore.sql`.

- Run `[x]`: `sqlite3 < <file>`.
- Grammar `[x]` (2026-11-01): `grammars/tree-sitter-sql` (README there), a
  port of DerekStride/tree-sitter-sql 0.3.11 (MIT): all 523 node types / 53
  fields kept, 11 added. Corpus: 2,567 files (sqlfluff's fixtures for 30
  dialects, PostgreSQL's regression/contrib/PL scripts, the repo's own):
  `--old-clean` passes (all 59,970 statements the old grammar parsed cleanly
  identical), 7 reviewed differences; files with errors 2,096 -> 1,963, none
  new; ERROR bytes -19%; recovery -16.1%; fuzz clean (the old scanner failed
  UBSan on every file). Fixes: psql meta-commands and sqlite3 dot-commands as
  extras, `;;`, `EXPLAIN (...)`, `DO $$`, GRANT/REVOKE, COLLATE in ORDER BY,
  LIKE ESCAPE, bare VACUUM/RESET; scanner rewritten (state freed while
  serialized, leaked on restore, `$$x$y$$`); numbers were highlighted as
  strings (Lua patterns in `#match?`). Oracle: `tools/ts_grammar_diff/sql_check.py`.
- LSP `[x]` (2026-11-01): `mep-sql-lsp` (help/sql-lsp.org). Its own lexer and
  parser after SQLite's `parse.y`, with SQLite's error texts: agrees with
  sqlite3_prepare_v2 on all comparable statements of SQLite's test suite
  (71,470) and 2,884 nixpkgs .sql files (69,248), same message and offset
  every time; mutation test 3,234/3,234 rejected mutants flagged, 0 false
  syntax errors. 23 codes; precision 99.4% (SQLite's tests) / 99.96%
  (nixpkgs). Schema from the file, the workspace and the `-- database:` file
  (read through `sqlite3 -readonly`); vocabulary generated from sqlite3 and
  its docs (`just sql-lsp-vocab`). Completion, hover, signature help,
  definition/references/rename (tables, columns, CTEs, aliases), outline,
  folds, formatting, `--lint`, `--describe NAME` (the UI mode's Help).
- UI `[x]` (2026-11-01): help/sql-mode.org. The sqlite3 CLI as the console
  (a scratch database file per session when there is no `-- database:`);
  Schema, History, Results, Help panels refreshed by separate read-only
  sqlite3 jobs, so nothing runs twice. `<leader>re` sends the statement(s)
  at the cursor/selection, rows captured into Results; `<leader>rr` `.read`s
  the file. sql/sqlite blocks in Org, MEPML and Rmd open it too.
- Format `[x]` (2026-11-01): `src/sql_format.cpp` (`mep.format_sql`, the
  `sql` entry; mep-sql-lsp formats with it). Clause-per-line layout in the
  sqlfluff/pgFormatter style (compared with both; differences in
  help/formatting.org), uppercase keywords (or `keyword_case='preserve'`).
  The result is re-lexed and must give the same tokens and comments.
  SQLite's test suite: 40,085 scripts formatted, 271 refused (242 at
  syntax errors SQLite reports too), idempotent everywhere, SQLite's
  verdict never changed; ASan + UBSan clean.

---------------------------------------------------------------------------

## 4. Remaining work, in order (2026-11-07)

Scope: Grammar, LSP and Format for every language marked `[ ]` in
section 1. Each pillar is "done" by section 2's definition: corpus +
oracle for the grammar, a calibrated lint set and the full capability list
for the LSP, the reference formatter's layout measured on the corpus (with
a re-parse proof-read) for the formatter.

Ordering: finish what's half done first, then languages that reuse
machinery we already have, then the larger mainstream languages grouped by
family so each family's second member is cheaper than its first. When the
order changes, note it here.

Each language below lists its references:
- **ts**: the upstream tree-sitter grammar our port must be a drop-in for
  (the `.so` loaded today; `just grammar-symbols <name>` pins its API).
- **oracle**: the language's own parser, for the grammar diff and the
  LSP's syntax errors.
- **fmt**: the formatter whose layout ours matches.
- `(shell)` = already in the dev shell; `(nix shell)` = fetch with
  `nix shell nixpkgs#<x>` for verification, add to flake.nix if it's
  used by a `just` recipe.

### 4.0 Shared groundwork

- [x] `tools/format_compare.py <lang>`: one corpus-wide formatter
      comparison (byte-identical %, % of lines, idempotence, refusals,
      "language rejects the result" via the oracle), replacing the
      per-language one-offs (`cl_format_compare.py` stays as an oracle
      shim). Every formatter below reports through it. (`just
      format-compare <lang>`; nix: 300-file sample 100% byte-identical
      with nixfmt.)
- [x] `just lsp-lint-corpus <lang>`: run `mep-<lang>-lsp --lint` over
      `just grammar-corpus <lang>` and tally codes -- the false-positive
      calibration step every LSP repeats.
- [x] A skeleton generator (`tools/new_language.py <lang> --ext ...
      [--write]`) that stamps out `src/<lang>_lsp{.h,.cpp,_server.cpp,_test.cpp}`,
      `<lang>_format{.h,.cpp,_test.cpp}`, `help/<lang>-lsp.org`, and splices
      in the CMake targets, ctest/`just test` lists, flake.nix's installed
      tools, the `mep.lsp_servers` / `mep.format_languages` entries and the
      `mep.format_<lang>` binding beside Nix's (the reference language).
      Verified: a dummy language builds clean (strict flags) with `mep`,
      its tests pass, and a second run changes nothing.

### 4.1 Wave 1 -- finish the half-done ones

#### nix
- [x] Grammar  - [x] LSP  - [x] Format
- Done 2026-11-07: `mep-nix-lsp` (src/nix_parse.cpp, nix_lsp.cpp,
  nix_lsp_server.cpp; help/nix-lsp.org). Parser = Nix's lexer rules +
  LALR tables generated from parser.y's grammar (tools/nix_lalr.py,
  identical to bison's). `just nix-parse-check --mutants 1` over all
  41,317 corpus files + one damaged copy each: every verdict, message and
  line:col equal to `nix-instantiate --parse`. `just lsp-lint-corpus nix`:
  no errors on nixpkgs; every unused-let checked was real.
- oracle: `nix-instantiate --parse` (shell). Builtins vocabulary from
  `builtins` + `nix __dump-language` (names, arities, docs); nixpkgs `lib`
  as a generated table for hover/completion.
- LSP lints: undefined/unused bindings (`let`, function args, `with`
  makes a scope unknowable, like Python's star import), duplicate attrs,
  `rec` that isn't needed. Corpus: nixpkgs.

#### lua
- [x] Grammar  - [x] LSP  - [x] Format
- LSP `[x]` (2026-11-07): `mep-lua-lsp` (help/lua-lsp.org). Its parser
  (`src/lua_parse.cpp`) ports `llex.c`/`lparser.c`: `just lua-parse-check
  --mutants 1` -- 6,428 of 6,428 (corpus + a damaged copy each) get luac's
  verdict, message and line. Vocab (`tools/gen_lua_vocab.py` ->
  `src/lua_lsp_vocab.cpp`): 1,611 names -- the 5.4 stdlib with the manual's
  text (MIT), the `mep.*` API from kMepFuncs + the builtin chunks, and
  other-version names. 17 lint codes, calibrated on the corpus from 17,214
  reports to ~2,200: undefined-global only as a "did you mean" near miss
  (a host defines the rest), unused-argument only for functions with their
  own signature (callbacks/methods are interfaces), shadowing only within
  one function. 95 checks in mep-lua-lsp-test.
- Format `[x]` (2026-11-07): `src/lua_format*.cpp`, a port of StyLua 2.3.1
  (own lossless CST with full-moon-style trivia). `just format-compare lua
  --oracle`: 3,179 of 3,179 byte-identical with `stylua -` (33 files StyLua
  rejects skipped); also 100% at widths 40/80/100 and 2-space indent. 2
  refusals by the self-check (StyLua drops a `--DOC_HIDE` comment's code
  there); 15 non-idempotent, exactly as StyLua is. Depth cap 400 + a work
  budget (StyLua blows up exponentially on deep calls). 33 checks.
- oracle: `luac -p` (shell; Lua 5.4, the version mep embeds).
- fmt: StyLua (nix shell).
- LSP: must know the `mep.*` API (generated from the builtin chunks and
  `help/`) -- mep's own config language is the biggest Lua user here.
  Lints after luacheck's (globals, unused, shadowing).

#### maxima
- [x] Grammar  - [x] LSP  - [x] Format
- Grammar `[x]` (2026-11-07): `grammars/tree-sitter-maxima` (README there),
  the old node API intact (+`lisp_escape`). Precedence is Maxima's Pratt
  table encoded as tree-sitter precedences (lbp on `operand op`, rbp on the
  right operand). Oracle for *trees*, not just validity: `just
  maxima-grammar-check` compares every statement with src/maxima_ast.cpp's
  tree -- 758 of 774 accepted files identical; the rest declare word
  operators at run time (15) or split a number with `\`+newline (1). Fuzz
  clean (3,799 files); recovery better in 1,478 / worse in 478 (+4.8% bytes,
  `block`/`lambda` must stay usable as names). Dropped tsGrammarMaxima from
  flake.nix.

### 4.2 Wave 2 -- JavaScript and TypeScript (one parser, two dialects)

#### javascript
- [x] Grammar  - [x] LSP  - [x] Format
- Format `[x]` (2026-11-07): `src/js_format*.cpp` (`mep.format_js`, the
  js/jsx/mjs/cjs entries), a port of Prettier 3.6.2 (doc IR + printer,
  comment attachment, estree printer) on `js_parse.h`. `just format-compare
  javascript` (`prettier --parser acorn`): 2,108 of 2,108 byte-identical (1
  file prettier rejects skipped), 0 refusals, 24 non-idempotent exactly as
  Prettier is. Self-check: reparse + tree compare + every comment kept.
  Not ported: embedded CSS/GraphQL/HTML/Markdown in template literals.
- LSP `[x]` (2026-11-07): `mep-js-lsp` (help/js-lsp.org), the default for
  js/jsx/mjs/cjs (and, since, ts/tsx/mts/cts: see typescript). On `js_parse.h`
  (acorn 8.15 + acorn-jsx): first syntax error in acorn's words; module vs
  script as ParseAuto. Own eslint-scope-style scope analysis. Vocab
  (`src/js_lsp_vocab.cpp`): 9,269 names from TypeScript's lib files. 20
  lint codes, all ESLint rule names, eslint-disable(-line/-next-line),
  `/* global */`, `/* exported */`, `/* eslint-env */`. Oracle ESLint 9
  over the 2,109-file corpus: with the narrowings off the engine matches
  it finding for finding (no-undef 9,932/9,932, no-unused-vars 1,119/1,119,
  every other rule equal); calibrated from 11,181 reports to 296, every one
  also an ESLint finding -- no-undef only as a near miss (or a strict-mode
  write), unused params only for functions with their own signature
  (after-used), `_` names and rest siblings skipped, a browser script's
  top-level names skipped, shadowing only within one function. Mutation
  check: every typo landing on a reference reported. Hover with JSDoc,
  scope-aware completion with typed members (`document.body.` ->
  HTMLElement chain), signature help, definition/references/rename
  (refuses captures), outline, folds; `--lint`; UTF-8/UTF-16 positions.
  176 checks in mep-js-lsp-test. Formatting is a hook only
  (`kHasFormatter` in js_lsp_server.cpp). TypeScript: see below.
- oracle: `node --check` (shell); acorn for ESTree dumps.
- fmt: prettier (nix shell), default options.

#### typescript
- [x] Grammar  - [x] LSP  - [x] Format
- Format `[x]` (2026-11-07): the same `src/js_format*.cpp` with
  `jsfmt::Options::typescript` (+`jsx`, `dts`; `SetDialectForPath`),
  printer-estree's TS printers ported in `src/js_format_ts.cpp` (types,
  interfaces, enums, generics, union/intersection layout, mapped and
  conditional types, casts, decorators) plus the TS cases of needs-parens,
  handle-comments and the shared printers. `mep.format_js` with
  `dialect = 'ts'`/`'tsx'` (the ts/mts/cts/tsx entries). `just
  format-compare typescript` (`prettier --no-config --no-editorconfig
  --parser typescript`): 2,151 of 2,151 byte-identical, 0 refusals, 2
  non-idempotent exactly as Prettier is (it merges two line comments);
  `tsx`: 910 of 912 (the 2 are styled-components CSS, an embed), 0
  refusals, 0 non-idempotent. Prettier's own tests/format/typescript
  fixtures it accepts: 525 of 536 (embeds 6, JSDoc-only types 1, top-level
  `await(1)` module-ness 3, 1 parser refusal). ASan+UBSan clean over all
  three sets. 109 checks in mep-js-format-test.
- LSP `[x]` (2026-11-07): `mep-js-lsp` is the default for ts/tsx/mts/cts
  (`typescript_language_server` registered with no filetypes; help/js-lsp.org
  "Using typescript-language-server instead"). `JsLspDialect::Ts/Tsx/Dts`
  parse with `Options::typescript` (+`jsx`, `dts`): tsc's parser's syntax
  errors. Scope analysis (js_lsp_scope.cpp) grows typescript-eslint's two
  namespaces -- `Var::ns`/`Ref::ns` value/type bits, one variable per name
  so declaration merging (interface+class, const+type, overloads,
  namespace+anything) is one variable; type refs (annotations, heritage,
  type args, qualified names) and `typeof x` value refs; scopes for type
  params, `infer`, mapped keys, signatures, namespace bodies; overload
  signatures/function types declare no params; parameter properties,
  `this` params, enum members (in scope in initializers), decorators;
  ambient (`declare`, `declare module/global`, .d.ts) never unused. Lints
  as typescript-eslint recommended: TS-aware no-unused-vars (self-refs of
  types don't count, "only used as a type", type params a hint), no-undef
  off, no-dupe-class-members allows overloads/`declare` fields,
  no-redeclare allows merging, no-unreachable skips type-only statements.
  Oracle: ESLint 9 + typescript-eslint 8.60 `recommended` over the 3,063
  .ts/.tsx corpus (all parse). Narrowings off: no-unused-vars 532 vs
  their 620, 531 shared; their other 89 are 88 in TypeScript's lib .d.ts
  (ambient, by design) + 1 overloaded implementation's parameter; our 1
  extra is `x = x || y` only feeding itself. Narrowed (same rules as JS):
  1 no-unused-vars (from 0 before: TS was unlinted), no-redeclare 0 of
  their 939 (all `interface X`/`const X` merging), plus 116 no-shadow
  infos, 1 no-empty, 1 no-self-compare. Hover shows declared types
  (`const p: Point`, signatures with return types, parameter properties,
  interfaces, aliases, enum members, type params) and TS lib docs;
  completion is type-position aware (types/keywords/lib types vs values)
  and uses declared types for members (`m: Map<K,V>` -> Map's); outline
  lists interfaces/enums/namespaces/parameter properties, overloads once.
  241 checks in mep-js-lsp-test (65 TS). ASan+UBSan clean running every
  feature at a grid of positions (every 7th line, 9th column) over 2,600
  corpus files. Formatting is the
  Prettier port's (see Format below), called in the document's dialect
  (`jsfmt::SetDialectForPath` on the URI; `insertSpaces: false` -> tabs,
  tabSize ignored since mep sends 4 for every language).
- Grammar `[x]` (2026-11-07): `grammars/tree-sitter-typescript` and
  `-tsx` (README there), the dialects of one `common/define-grammar.js`
  that extends our JavaScript grammar; old API intact (177/184 node types,
  40/43 fields, +`using_declaration`). Oracle: TypeScript's
  `createSourceFile` parse diagnostics (`tools/ts_grammar_diff/ts_check.js`).
  2,140 of 2,151 `.ts` and 806 of 912 `.tsx` files identical; the 117
  known differences are files the old grammar misread (tagged templates
  with type arguments, `<in out T>`, `[symbol: S]`, `keyof readonly`), and
  no corpus file has a parse error (old: 98). Over TypeScript's own test
  suite: 414 failures where the old had 471. Fuzz clean (5,151 + 3,912
  files); recovery 13.3% / 1.9% fewer error bytes. Dropped
  tree-sitter-typescript/-tsx from flake.nix.
- Parser (2026-11-07, for the LSP and formatter, not yet wired into
  either): `Options::typescript` (+ `jsx` for .tsx, `dts` for .d.ts) in
  `src/js_parse*` -- `TsParser : JsxParser` in `src/js_parse_ts*.cpp`,
  making TypeScript 5.9 parser.ts's decisions (arrow/JSX/`<T>x`
  ambiguities, tsc's leniency: no strict mode or early errors it leaves to
  the checker) with acorn-style messages and typescript-estree node
  names/shapes. `tools/js_parse_ts_check.py` (oracle
  `tools/js_parse_ts_oracle.js`: `ts.createSourceFile` parseDiagnostics +
  typescript-estree trees). Corpus (3,063 .ts/.tsx): 3,063 accepted by
  both, trees identical in 3,063/3,063 (ignoring TemplateElement, whose
  typescript-estree range includes the `` ` ``/`${`/`}` delimiters).
  TypeScript's tests/cases/{conformance,compiler} split per `@filename`
  (14,805 units): 14,032 accepted by both + 570 rejected at the same
  position, 203 rejected at another position, 0 over/under (acceptance
  agrees on all); trees identical in 13,982/14,030 (the rest are mostly
  invalid code typescript-estree reshapes or drops, JSDoc types,
  non-literal `import(T)`). Damaged copies (one mutant per file): no
  crash, acceptance agrees on 3,063/3,063 and 14,805/14,805. `Options::mts`
  marks a .mts file (always a module: top-level `await(1)` is an
  AwaitExpression); JSDoc types are TSJSDoc* nodes. ASan+UBSan
  clean over ~196k originals + damaged copies. JS parse unchanged
  (`tools/js_parse_check.py` 100%, also `--mutants 1`).
- ts: tree-sitter-typescript, which `require`s the JavaScript grammar.js --
  ours should extend ours the same way (typescript + tsx).
- oracle: `typescript`'s `createSourceFile` parse diagnostics (nix shell
  nodePackages.typescript).
- One `src/js_parse*` with a TS flag serves both LSPs (`mep-js-lsp`
  covering js/ts/tsx is fine) and both formatters; lib.d.ts generated
  into the vocabulary table.

### 4.3 Wave 3 -- small languages close to ones we have

#### scheme
- [x] Grammar  - [x] LSP  - [x] Format
- Grammar `[x]` (2026-11-07): `grammars/tree-sitter-scheme` (README there),
  a drop-in for 6cdh/tree-sitter-scheme (MIT, credited): the same 22 node
  types, rule order and token boundaries (with its token set, all 3,610
  corpus files gave identical trees), plus R7RS `#u8(`, Guile's `#!/...!#`
  script header and Guix's G-expressions (`gexp`/`ungexp`/
  `ungexp_splicing`). Corpus (`just grammar-corpus scheme`: Guile, Guix,
  haunt, guile-lib, chibi, Chez, Gauche; 2.18M lines): 2,871 identical, 715
  reviewed known differences (all old parse errors), 24 Guile rejects, 0
  unexplained; files with parse errors 1,085 -> 380. Oracle: Guile's `read`
  with Guix's reader extensions (`tools/ts_grammar_diff/scheme_read.scm`).
  Fuzz clean (6,610 files); recovery 45.2% fewer error bytes (better in
  620, worse in 0). Dropped tree-sitter-scheme from flake.nix.
- LSP `[x]` (2026-11-07): `mep-scheme-lsp` (help/scheme-lsp.org). Its own
  reader (`src/scheme_reader.cpp`) at parity with Guile's `read`
  (`just scheme-read-check --mutants 2`): 10,830 files (3,610 corpus + 7,220
  damaged copies), all agree -- 7,930 accepted, 2,900 rejected with the same
  `line:col` and message (146 of them positionless, verdict only).
  Vocabulary generated from a running Guile (`just scheme-lsp-vocab`,
  `tools/gen_scheme_vocab.scm`): R7RS-small, (guile), the SRFIs, (ice-9)
  and (rnrs) modules -- names, kinds, arities from
  `find-program-arities`, argument lists; no prose. Hover, completion,
  signature help, definition/references/rename over lexical scopes
  (let/named let/do/match/syntax-rules/syntax-case/records/define-values
  ...), symbols, folds, formatting. Lints, calibrated on 3,557 corpus files
  (Guile, Guix, haunt, guile-lib, chibi, Chez, Gauche): unused-parameter
  (hint) 6,266 in 863 files, unused-variable (warning) 456/213,
  unused-destructured (hint) 246/73, unused-definition (warning) 96/62,
  wrong-arity (warning) 6/4 -- every one a real bug (chibi highlight.scm,
  http.scm, commands.scm x3; Guix package.scm) -- and duplicate-binding
  (error) 1, Gauche's deliberately invalid macro test; unbalanced parens
  are the reader's own errors. Checked against Guile's compiler:
  unused-variable on 354 Guile modules, 76 of mep's 80 confirmed by
  `guild compile -W unused-variable` (the 4 others only ever set!);
  arity mutation test (600 dropped/added operands in Guile's modules):
  of the 37 Guile warns on mep catches 27 (the 10 are inside macro uses,
  deliberately lenient), plus 53 Guile's compiler misses (all genuine).
  Arity checks run only where every import is a known module and no
  keyword argument is passed; lints stop at the first reader error.
- Format `[x]` (2026-11-07): `format_scheme` (src/scheme_format.cpp), Emacs
  30 `scheme-mode`'s indentation without Emacs -- `calculate-lisp-indent`,
  `scheme-indent-function` and all 84 of scheme.el's
  `scheme-indent-function` properties, `scheme-let-indent`, the `#;`
  datum-comment propertizing, Emacs's Unicode syntax classes -- over
  scheme-mode's own `indent-region-line-by-line` (no column cache, unlike
  lisp-mode), plus lisp_format's whitespace rules; `indent` takes extra
  specs. Oracle `tools/scheme_indent_oracle.el` (emacs-nox 30.2;
  `just format-compare scheme`): 3,532 of 3,549 byte-identical (99.5%;
  99.96% of lines), 0 not idempotent, 8 refused (Chez/Gauche reader syntax
  mep's lexer does not read). The 17 others are deliberate: Emacs reads
  Guile's `#!...!#` comments (7 files), a top-level `#!eof`'s trailing
  prose (3) and a multi-line `#{...}#` symbol (1) as code, and changes
  Gauche `#[ \t]` char sets / misreads `#/` inside them (6) -- mep keeps
  all of those as written.
- ts: 6cdh/tree-sitter-scheme. oracle: guile `read` (shell).
- Reuses the Lisp reader/indent machinery; fmt: Emacs `scheme-mode`
  indentation via the `cl_indent_oracle.el` approach. R7RS-small + Guile
  vocabulary.

#### fish
- [x] Grammar  - [x] LSP  - [x] Format
- Grammar `[x]` (2026-11-07): `grammars/tree-sitter-fish` (README there),
  from ram02z's (Unlicense) with its node API intact (38 node types, 9
  fields). Fixed against fish 4.8's tokenizer: `(` in double quotes is text,
  `{` at command position is a block, `&`/`^` in words, brace-element
  blanks, newlines after `&&`/`||`/`|`, continuations (a gap where a command
  starts) and the comment lines they carry over (scanner state), no final
  newline, empty `for` lists. Corpus 1,956 files (fish's share/ and tests,
  28 fishPlugins): 1,890 identical to the old trees, 66 reviewed known
  differences, 0 unexplained (`fish --no-execute` accepts every corpus
  file); files with parse errors 42 -> 3. Fuzz clean (4,956 files);
  recovery 59.5% fewer error bytes. Dropped tree-sitter-fish from flake.nix.
- LSP `[x]` (2026-11-07): `mep-fish-lsp` (`src/fish_lsp*.cpp`,
  help/fish-lsp.org) over `src/fish_parse*.cpp`, a reader making fish
  4.8's decisions (tokenizer, AST populator with its error recovery, the
  pre-execution checks: argument expansion, `$?`/`${x}` bash-isms,
  command-position cmdsubs, break/continue, `end` arguments, unknown
  builtins). `--fish-check` vs `fish --no-execute --no-config`
  (`just fish-parse-check`, `tools/fish_parse_check.py`): corpus 1,956/1,956
  agree; `--mutants 4 --seed 7` 9,780/9,780 (8,465 ok, 1,315 errors, same
  message and line); 0 crashes. Vocabulary (`just fish-vocab`,
  `tools/fish_vocab.py`): 135 commands (64 builtins, 71 functions) --
  synopsis lines and option spellings only (fish is GPL-2, no prose).
  Lints: reserved-function-name, read-only-variable, unknown-subcommand,
  unclosed-test, unreachable-code (hint); calibrated with
  `lsp_lint_corpus.py fish`: 12 in 8/1,956 files, all true positives.
  Hover, completion (vars/commands/options/subcommands), signature help,
  definition/references/rename (fish scoping, argparse `_flag_`), outline,
  folds, formatting. 49 checks in mep-fish-lsp-test.
- Format `[x]` (2026-11-07): `src/fish_format.cpp` (`mep.format_fish`),
  fish_indent's rules rebuilt from its behaviour over the same reader
  (indent computation incl. cmdsubs and quotes, gap/comment/newline
  handling, preferred `;`, unquoting, redirection spacing), plus a
  self-check (words, operators and comments must read back the same, else
  refuse). `just format-compare fish` vs fish_indent 4.8: 1,956/1,956
  byte-identical, 0 refused; damaged copies (3 per file) 5,805/5,868
  identical, 63 refused -- all cases where fish_indent itself changes the
  script (mostly a newline added inside a string left open at EOF); the
  non-idempotent outputs (1 corpus file, 18 mutants) are fish_indent's own,
  matched byte for byte on the second pass too. 3,000-deep nesting
  refused (fish_indent overflows its stack). 51 checks in
  mep-fish-format-test.
- ts: ram02z/tree-sitter-fish. oracle: `fish --no-execute` (shell).
- fmt: `fish_indent` (shell) -- deterministic, aim for byte identity.
  Builtins/functions vocabulary from `builtin -n`, `functions -a` and
  fish's man pages.

#### tcl
- [x] Grammar  - [x] LSP  - [x] Format
- Grammar `[x]` (2026-11-07): `grammars/tree-sitter-tcl` (README there),
  a drop-in for tree-sitter-grammars/tree-sitter-tcl (MIT, credited): the
  same 35 node types and 7 fields, its 19 corpus cases pass. Follows
  tclParse.c: brace counting through comments (scanner state), `{*}` only
  before a word, `::` names, backslash-newline in comments, braced text
  that is not a script read as data. Corpus (`just grammar-corpus tcl`:
  Tcl 8.6.16 library/tests/tools, Tk, tcllib; 1,810 files, 973k lines):
  Tcl's parser (`tools/ts_grammar_diff/tcl_check.tcl`, getbytecode)
  accepts every file, so the gate is `--old-clean`: 158,026 top-level
  commands the old grammar read cleanly, 144,143 identical, the rest in 176
  reviewed known differences, 0 unexplained; files with parse errors
  1,118 -> 5, bytes in ERROR nodes 1,737,345 -> 27. Fuzz clean (4,810
  files); recovery 52.4% fewer error bytes (better in 2,427, worse in 141).
  Dropped tree-sitter-tcl from flake.nix.
- LSP `[x]` (2026-11-07): `mep-tcl-lsp` (help/tcl-lsp.org). Parser
  `src/tcl_parse.*` ports tclParse.c (Tcl_ParseCommand, words, variables,
  braces, quotes, backslashes), tclCompExpr.c's ParseExpr/ParseLexeme,
  TclParseNumber's scanner and TclFindElement, with Tcl's messages (BSD
  notice kept). Every script Tcl would run is parsed in place (proc
  bodies, if/while/for/foreach/switch/try/catch/dict for bodies and
  conditions, namespace eval, TclOO/itcl/snit class bodies and methods,
  tcltest bodies, `[...]`, braced expr). Parity (`just tcl-parse-check`,
  oracle `tools/tcl_parse_oracle.c` = libtcl's Tcl_ParseCommand /
  Tcl_ParseExpr on the same spans): corpus 332,140 spans, 0
  disagreements; with 10 damaged copies per file 3,415,459 spans (4,386
  errors), 0 disagreements on verdict, message or position. Vocab
  (`tools/gen_tcl_vocab.py` + `tools/tcl_vocab_probe.tcl`): 105 commands,
  208 subcommands, 31 expr functions, 22 variables; argument counts and
  usage lines measured by calling each builtin in a child interp with
  0..30/40 args, docs from Tcl's doc/*.n (license.terms embedded). Lints
  `syntax-error`, `wrong-args` (Tcl's usage line), `unknown-subcommand`,
  `unused-variable` (plain `set` only, dynamic procs skipped),
  `unbraced-expr`, `missing-dollar`. Calibration
  (`lsp-lint-corpus tcl`): 126 of 1,092 library files get a diagnostic --
  190 unused variables (samples: dead stores), 85 unbraced exprs, 10 real
  bugs (foreach without a list x2, `array exists` x2 / `array set` arity,
  an unbalanced paren in an expr, 4 missing `$`), 13 FPs (dict getnull
  from tcllib's dicttool in files that do not mention it); in the 718 test
  files the wrong-args/syntax-error hits are the tests' deliberate
  `catch {set}` cases. Hover/completion/signature help/definition/
  references/rename/outline/folds/formatting; 52 checks in
  mep-tcl-lsp-test. ASan+UBSan clean running every feature (diagnostics, hover, completion, signature, definition, references, rename, symbols, folds, format) at a 7-line × 9-column grid of positions over all 1,810 corpus files.
- Format `[x]` (2026-11-07): `src/tcl_format*`, a port of tclint 0.6.2's
  `tclfmt` (MIT, Noah Moroze; format.py/parser.py/lexer.py + its builtin
  command table), defaults (indent 4, max 2 blank lines, no spaces in
  braces, namespace eval indented; tclfmt never wraps). `just
  format-compare tcl`: 1,801 of 1,803 files tclfmt accepts byte-identical,
  100.00% of lines, 0 non-idempotent, 0 refused, 0 crashes; the 2
  differences are where tclfmt changes meaning ($a() -> $a, dropping the
  blank line after a comment ending in `\`), which the port refuses to
  copy. Damaged copies: 541 of 542 identical. Self-check reparses the
  output (same tree as tclfmt's --debug check); depth caps refuse instead
  of crashing. 154 checks in mep-tcl-format-test. `format_tcl` builtin
  and the LSP's formatting run it.
- ts: tree-sitter-grammars/tree-sitter-tcl. oracle: `info complete` in
  tclsh (shell) plus Tcl's own parser for errors.
- fmt: tclint's `tclfmt` (check nixpkgs; else write down our own rules).

### 4.4 Wave 4 -- scientific languages

#### octave
- [x] Grammar  - [x] LSP  - [x] Format
- Grammar `[x]` (2026-11-07): `grammars/tree-sitter-matlab` (README there),
  a drop-in for acristoffers/tree-sitter-matlab 1.3.0 (MIT, credited;
  still named `matlab`): all 77 node types and 10 fields kept, +3
  (`do_statement`, `unwind_protect_statement`,
  `unwind_protect_cleanup_clause`); its corpus cases pass (MATLAB's `!`
  shell escape and two recovery shapes rewritten for Octave) plus 12
  Octave cases, 167 in all. Adds `#` comments and `#{ #}` blocks, Octave
  end keywords, `do/until`, `unwind_protect`, `!`/`!=`, `++`/`--`,
  compound assignments, indexing any value (`(x > 0)(:)`), assignments
  inside parentheses/arguments, defaults, `_` in numbers, `\` escapes in
  double-quoted strings, newlines as blanks inside parentheses, Octave's
  command-syntax rule. Corpus (`just grammar-corpus matlab`: Octave's
  scripts/test/examples + 30 Octave Forge packages; 6,676 files, 954k
  lines): Octave's `__parse_file__` (`tools/ts_grammar_diff/octave_check.sh`)
  accepts 6,636; the old grammar reads no `#` comment, so the gate is
  `--old-clean`: 367,985 clean top-level items, 0 unexplained; 102
  reviewed known differences (end keywords 44, CRLF continuations 47,
  `catch` + newline 3, leading blank lines 5, `+=`/`unwind_protect`/`do`
  1 each); files with parse errors 4,241 -> 8 (6 Octave accepts, listed
  in the README), bytes in ERROR nodes 5,412,631 -> 148. Fuzz clean (9,676
  files); recovery 72.7% fewer error bytes (better in 2,042, worse in 259).
- LSP `[x]` (2026-11-08): `mep-octave-lsp` (`src/octave_lsp*.cpp`,
  help/octave-lsp.org) over `src/octave_parse.cpp`, a reader that follows
  Octave 11's `lex.ll`/`oct-parse.yy` rule for rule and reproduces
  `__parse_file__`'s messages, lines and columns (`--octave-check`), plus
  its post-parse checks (command syntax vs variable, end-keyword mismatch,
  classdef/file name, duplicate functions/parameters). Vocabulary: 1,763
  functions with usage lines (`just octave-lsp-vocab`,
  `tools/gen_octave_vocab.py`). `just octave-parse-check`
  (`tools/octave_parse_check.py`, oracle `tools/octave_parse_oracle.m`):
  corpus + 5 damaged copies each, 40,056 files: PARSECHECK_RESULT.
  Lints: syntax-error, duplicate-case, unused-local (hint; skips
  eval/load/assignin/... functions, scripts, `%#ok`), unreachable-code.
  Corpus calibration: 22 syntax errors = the 22 files Octave rejects, 13
  duplicate-case (real, 2 files), 8 unreachable (real), unused-local
  1,086 in 666 files (sampled: genuine). Hover/signature from usage
  lines, completion (scoped, nested functions see parents, fields, works
  in broken files via line repair), definition/references/rename,
  outline, folding, formatting. 62 test checks.
- Format `[x]` (2026-11-08): `src/octave_format.cpp`, a port of MISS_HIT
  0.9.44's `mh_style --fix` (lexer, Token_Buffer replay, stage-3 rules,
  parser's end-of-statement, row-list and redundant-bracket fixes) driven
  by mep's Octave AST; `gf` via `format_octave`, also the LSP's
  formatting. Defaults differ on purpose: no added `;` (changes what
  Octave prints; `semicolons = true` restores), runs to a fixed point.
  `just format-compare octave` (`--mh-style` = one mh_style run;
  `tools/mh_style_ref.py`): mh_style fixes 3,020 of 6,676 files (it
  rejects Octave syntax); mep refuses 2 (duplicate functions, which Octave
  rejects too) and is byte-identical on 2,976 of 3,018 (98.6%; 99.95% of
  lines). The 42 differ by mh_style bugs/gaps: it loses the line break
  after removing `if (c) stmt`'s brackets (unparseable output), drops
  `[..]([1 3 2 4])`'s index brackets, reads `do/until` as a call, and
  rewrites non-ASCII comment bytes (cp1252). Default mode over the whole
  corpus: all 6,654 files Octave accepts formatted, self-check (re-parse,
  same tree) passes, idempotent; the 22 refused are the 22 Octave
  rejects. 33 test checks.
- ts: acristoffers/tree-sitter-matlab (`matlab.so`). oracle: octave's
  `__parse_file__` (shell).
- fmt: MISS_HIT `mh_style` (nix shell python3Packages / pip). Vocabulary
  from octave's `__list_functions__` + help texts.

#### julia
- [x] Grammar  - [x] LSP  - [x] Format
- Grammar `[x]` (2026-11-08): `grammars/tree-sitter-julia`, tree-sitter-julia
  0.25.0 (MIT, credited) taken in-house with its node API intact (86 node
  types, 6 fields; its 63 corpus cases pass). Fixed against JuliaSyntax:
  `isa`/`in` as names and never the right side of juxtaposition, newlines
  after infix `in`/`isa` and `public`, `for outer in`, `begin` as an index
  inside `[...]` (external scanner token), `;` after a comma list in
  `[...]`, `where` in a struct's type head. Corpus 8,216 files; oracle
  `tools/ts_grammar_diff/julia_check.jl` (`Meta.parseall`) accepts them all;
  `--old-clean` 214,303 clean items, 0 unexplained; 7,353 identical, 536
  reviewed known differences (`test/known-differences.txt`; reasons assigned
  by category heuristics, spot-checked); files with parse errors 446 -> 79,
  none new. Fuzz clean (11,216 files); recovery 3.3% fewer error bytes.
- LSP `[x]` (2026-11-07): `mep-julia-lsp` (`src/julia_lsp*.cpp`,
  help/julia-lsp.org) over `src/julia_parse*.cpp`/`julia_tokenize.cpp`/
  `julia_parser.cpp`, a port of JuliaSyntax (MIT): green trees as
  JuliaSyntax 0.4.10 builds them (the version Runic walks), diagnostics in
  the words of Julia 1.12's JuliaSyntax 1.0.2. `just julia-parse-check`
  (`tools/julia_parse_check.py`, oracle `tools/julia_parse_oracle.jl`):
  corpus 8,216/8,216 identical trees, diagnostics and first errors; with
  3 damaged copies each (32,864 files) trees 100%, diagnostics 99.93%
  (seed 1; 22 differ) / 99.90% under ASan+UBSan (seed 2; 34 differ),
  first error 32,863/32,864, 0 crashes, no sanitizer reports -- every
  remaining difference reduced and explained (1.12 builds another tree:
  `true`/`false` as numbers, bracket resync after a generator,
  `do ... end"a"`), listed atop julia_parser.cpp. `--julia-check` prints
  Julia's first error. Lowering errors emulated in lowering's own words
  (duplicate argument/field/keyword argument, break outside loop,
  local/global/argument conflicts, local const); `tools/julia_lower_check.jl`
  lowers the corpus (187,702 statements, 0 such errors), and the LSP
  reports 0 of them there. Vocabulary (`just julia-vocab`,
  `tools/gen_julia_vocab.jl`): 1,929 Base/Core/stdlib names with
  signatures and docstrings (MIT). Lints calibrated with
  `tools/lsp_lint_corpus.py julia` over the corpus (8,216 files):
  unused-type-parameter 2 (both Julia's own warning), soft-scope 113 in
  53 files (Julia's warning, top-level test loops; skipped under macros),
  undefined-name 3 (only in an explicit module with no include/package
  `using`, near misses only; 2 are real stdlib bugs: `nargs` in
  Base.Cartesian, `_capturescalars` in SparseArrays), unused-local 1,017
  in 483 files (sampled: real), compare-nothing 273 (info), unreachable 91
  and unused-argument (untyped, non-splat, named functions only) as
  greyed hints; 0 syntax errors, 21 syntax warnings (JuliaSyntax's own).
  Scope model: hard (function, ->, do, macro, struct), soft (for, while,
  try, comprehensions), let, global/local. Hover (docstrings, methods),
  completion (scope, `Module.` members, keywords, vocab, macros),
  signature help, definition/references/rename, outline, folds,
  formatting; 51 checks in mep-julia-lsp-test, plus mep-julia-parse-test.
  `julials` (LanguageServer.jl) stays as the documented alternative.
- Format `[x]` (2026-11-07): `src/julia_format*.cpp`, a port of Runic.jl
  1.5.0 (MIT, no options) function by function over the JuliaSyntax-port
  trees (shared child lists kept, so its in-place passes carry over; every
  Runic assertion refuses the file as Runic does). `just format-compare
  julia` (reference `tools/julia_runic_ref.py` reading the cache that
  `tools/julia_runic_cache.jl` fills in one julia process): 8,210 of 8,210
  files Runic accepts byte-identical (100.0%), 6 Runic refuses skipped, 0
  refused by mep, 0 crashes; 8 not idempotent, each exactly as Runic
  itself (mep's second pass = Runic's second pass). Self-check re-parses
  the output and compares leaf tokens up to Runic's deliberate edits;
  never fires on the corpus. 1,200 truncated/spliced files: no crashes.
  213 KB in 0.38 s. Not ported: range formatting (`--lines`). 33 checks in
  mep-julia-format-test. `format_julia` builtin (`gf`, width 92) and the
  LSP's formatting run it; `{'runic'}` puts Runic back.
- ts: tree-sitter-julia. oracle: `Meta.parseall` / JuliaSyntax (shell).
- fmt: Runic.jl or JuliaFormatter.jl (pick one; Runic has no options,
  which makes byte identity a fair target). Base/stdlib vocabulary
  generated from `names(Base)` + docstrings.

#### fortran
- [x] Grammar  - [x] LSP  - [x] Format
- Grammar `[x]` (2026-11-07): `grammars/tree-sitter-fortran` (README
  there), a drop-in for stadelmanma/tree-sitter-fortran (0.4.0, commit
  022b032; MIT, credited, LICENSE kept): all 218 node types and 30 fields,
  none added; its 121 corpus cases pass. The external scanner now reads
  fixed form (detected from the text and latched: column-1 `C c * D d`
  comments, column-6 continuations as a zero-width `line_continuation`,
  comment lines between continuations, strings across continuation lines),
  free-form `'abc&`/`&def'` strings, `q` exponents, comments as an external
  token, and directive-ending newlines (a `#define`/`PUSH_SUB(x)` line ends
  at its own newline). Corpus (`just grammar-corpus fortran`: LAPACK/BLAS,
  scipy, ARPACK, Wannier90, Quantum ESPRESSO, netCDF-Fortran, HDF5,
  SIESTA, Octopus, gcc 14.3's gfortran testsuite; 16,409 files). Oracle
  `tools/ts_grammar_diff/fortran_check.py` (gfortran -fsyntax-only against
  corpus-built .mod files, missing modules/includes stubbed, syntax vs
  semantic messages sorted): 15,324 accepted, 1,085 with a syntax error.
  11,344 trees identical to the old grammar's; files with parse errors
  5,853 -> 1,210, none only in new; of the 5,065 that differ 193 are
  gfortran rejects and 4,872 reviewed known differences (4,784 fixed-form
  files the old grammar read as free form, 6 continued strings, 82
  free-form files where both fail on macros from unseen headers or DEC
  extensions), 0 unexplained. Fuzz clean (19,409 files); recovery 71.3%
  fewer error bytes (better in 1,011, worse in 212). The nixpkgs grammar
  is gone from flake.nix/ts_grammars.tsv.
- LSP `[x]` (2026-11-08): `mep-fortran-lsp` (src/fortran_parse* reader:
  free/fixed-form line assembly, a cpp model for .F90/.F, a lenient
  statement parser and a construct-structure pass; src/fortran_lsp*
  analysis + server; help/fortran-lsp.org). Diagnostics: gfortran-worded
  syntax errors, then `undeclared` (IMPLICIT NONE), `unused-variable`,
  `undefined-label`, `line-too-long`; completion (scopes, `%` components,
  USE modules, 956 intrinsics/keywords from `tools/gen_fortran_vocab.py`),
  hover, signature help, definition, references, rename, symbols, folds,
  formatting. Parity (`just fortran-parse-check`, gfortran 15
  -fsyntax-only, 16,409 corpus files): of 15,315 gfortran accepts mep
  accepts 15,278 (37 false positives: blank-insensitive fixed-form
  spellings, Hollerith with `!`, DEC extensions); verdicts agree 95.05%;
  where both fail, same line in 255 of 319, same words in 151. Mutants
  (1,500 files x 2): verdicts agree 88.7%, 0 crashes. Lint calibration on
  the 12,734 clean files: `undeclared` 4 hits, `unused-variable` 99/100
  agree with gfortran -Wunused-variable on a sample. 70 test checks
  (mep-fortran-lsp-test). fortls stays in mep.lsp_servers with
  `filetypes = {}`.
- Format `[x]` (2026-11-08): src/fortran_format.* (`mep.format_fortran`,
  `gf` on .f90/.f95/.f03/.f08/.F90), fprettify 0.3.7 defaults (indent 3,
  width 132), no code copied. `tools/format_compare.py fortran` over 11,317
  free-form files: 11,267 of 11,267 byte-identical, 23 where fprettify
  fails (skipped), 27 refused (token check: fprettify would change the
  code), 0 crashes/timeouts. Fixed form refused outright (as fprettify).
  89 checks in mep-fortran-format-test; 3,000 fuzz inputs clean under
  sanitizers.
- ts: stadelmanma/tree-sitter-fortran. oracle: `gfortran -fsyntax-only`
  (shell).
- fmt: fprettify (nix shell). Intrinsics vocabulary from gfortran docs.

#### gap
- [x] Grammar  - [x] LSP  - [x] Format
- Grammar `[x]` (2026-11-08): `grammars/tree-sitter-gap` (README there),
  gap-system/tree-sitter-gap at 96fe2e4 (MIT, Max Horn, license in
  grammar.js) taken in-house with its node API intact (59 node types, 23
  fields; its 38 corpus cases pass, plus 2 of mep's). Fixed against GAP's
  reader: a file opening with `local` (source_file `locals` field); and the
  scanner's unprototyped `create()`, which UBSan flagged on every parse.
  Corpus (`just grammar-corpus gap`: GAP 4.15.1's lib/grp/hpcgap/tst and
  its ~160 bundled packages plus the .tst transcripts' inputs; 8,950 files,
  3.25M lines). Oracle GAP's `ReadAsFunction` (`gap_check.py`): 8,931
  identical, 19 reviewed known differences (all the leading `local`), 0
  unexplained; files with parse errors 79 -> 60 (59 GAP rejects; 1 is
  linecontinuation.tst, backslash-newline splices inside tokens). Fuzz
  clean (11,950 files); recovery -0.1% error bytes (better in 9, worse in
  2). Queries (highlights, folds, structure) beside the grammar; the old
  grammar is out of flake.nix, scripts/ts_grammars.tsv and
  scripts/fetch-grammars.sh.
- LSP `[x]` (2026-11-07): `mep-gap-lsp` (src/gap_parse.* reader, src/gap_lsp*,
  help/gap-lsp.org). The reader gives GAP 4.15's first syntax error, message
  and line (`--gap-check`, `tools/gap_parse_check.py` against
  `ReadAsFunction`). Diagnostics: syntax-error; unbound-global = GAP's own
  read-time "Unbound global variable" warning, rules measured against real
  GAP (inside functions only; IsBound operand exempt, Unbind not; top-level
  assignment targets bind from their token on, the statement's own target
  included; Declare*/BindGlobal calls from the next statement; forward
  references are info); unassigned-local (found real latent bugs in
  lib/tom.gi, lib/grppc.gi); unused-local hints (loop vars exempt).
  Calibration over the 8,950-file corpus: the server reads the package's
  other files (PackageInfo.g root, or the file's directory) and its
  NeededOther/SuggestedOtherPackages installed beside it; HasX/SetX of a
  known X, escaped operator names, `@`-namespaced names, HPC-GAP kernel
  names count as bound; a package loading a kernel extension switches the
  check off; stray start-up names like `x`, `g`, `gens` do not count. Result:
  917 unbound-global in 111 files (was 5,052). Vocab
  (tools/gen_gap_vocab.py): names/kinds/arguments of 17,134 documented forms
  + 32,923 start-up globals, no GPL prose. Completion (token-level scope, so
  it works in unfinished code), hover, signature help, definition /
  references / rename (strings in Declare*("X") included), outline, folds.
- Format `[x]` (2026-11-07): src/gap_format.cpp re-indents only (GAP's
  printer's 4-column blocks, continuation +1, closers back, trailing blanks
  dropped); tokens, comments and line breaks untouched. `mep-gap-format-test
  --check` over the corpus: 0 token changes, idempotent, 65 refused (syntax
  errors).
- ts: gap-system/tree-sitter-gap. oracle: GAP's reader (shell `gap`).
- fmt: no standard tool -- GAP's own function printer (`Print(f)`) is the
  indentation reference. Vocabulary from GAP's manual index.

### 4.5 Wave 5 -- systems languages

#### go
- [x] Grammar  - [x] LSP  - [x] Format
- Grammar `[x]` (2026-11-07): `grammars/tree-sitter-go` (README there),
  tree-sitter-go 0.25.0 (MIT, credited, license in grammar.js) taken
  in-house with its node API intact (107 node types, 35 fields; its 67
  corpus cases pass, plus 3 of mep's). Fixed against go/parser: `new(expr)`
  (Go 1.26) and `new`/`make` with an expression argument, `const x T` with
  no value, `L1: L2:` at the end of a block. Corpus (`just grammar-corpus
  go`: Go 1.26.5's src/ tree, gh, hugo, fzf, delve, caddy, go-tools; 11,486
  files, 3.74M lines). Oracle go/parser (`tools/ts_grammar_diff/go_check.go`,
  `just go-check`): 11,477 identical, 7 reviewed known differences, 2 in
  files go/parser rejects, 0 unexplained; files with parse errors 52 -> 46
  (39 go/parser rejects too; 7 type-checker test inputs using a type as an
  operand, `[...]T` as a type, `~x` or `.(type)` out of place). Fuzz clean
  (14,486 files); recovery +0.9% error bytes (better in 59, worse in 50;
  seed 11 +0.7%), all from letting expressions into `new(...)`. Queries
  (highlights, folds, structure) beside the grammar; dropped
  tree-sitter-go from flake.nix and scripts/ts_grammars.tsv.
- LSP `[x]` (2026-11-07): `mep-go-lsp` (help/go-lsp.org). Parser
  `src/go_parse.cpp` is go/scanner + go/parser 1.26 ported function for
  function over `src/go_ast.{h,cpp}` (go/ast's nodes and Pos/End rules);
  `just go-parse-check --mutants N` compares the *whole* AllErrors list
  (message, line, column, order) with go/parser via go_check: 11,486
  originals + ~57k mutants (seeds 1/2/7), 0 crashes, every difference a
  `//line` directive (go/token adjusts reported positions; ours stay
  physical, by design). Nesting cap 10,000 (2,500 under sanitizers) instead
  of go/parser's 1e5, same message. Analysis (`go_lsp.cpp`, resolver over
  Go's blocks in one file; `go_lsp_features.cpp`): the compiler's own
  errors that one file decides for certain (unused var/import/label,
  undefined label, misplaced break/continue/fallthrough, missing return,
  redeclared, no new variables), vet's unreachable and self-assignment,
  `pkg.Name` missing from a stdlib package (gated on linux/amd64 build
  constraints from the file name and `//go:build`, and off for an external
  test package's own package), deprecated stdlib names as hints. Never an
  undefined plain name (other files of the package are unseen). Calibration
  (`--lint` over the 9,931 non-testdata corpus files): 1 unused-variable
  and 4 unreachable, all real (a typo in a `//go:build ignore` file), plus
  202 deprecation hints; the untouched-by-build gate removed 97 false
  `undefined`. Unused-import for non-stdlib paths only when no unknown
  `name.` is used in the file. Best-effort typing from declared types,
  composite literals, conversions and function results (stdlib signatures
  parsed with the same parser): hover 90% of identifiers in net/http, 80%
  in gh (selectors into third-party packages are the residue). Completion
  (scope, `pkg.` members, `x.` fields/methods incl. stdlib types, import
  paths), signature help, definition/references (fields and methods via
  selectors), rename (locals only; package-level refused), outline, folds.
  ASan/UBSan clean over the corpus and over every query at 60 random
  positions in 1,600 files plus 1,600 mutants. 61 checks in
  mep-go-lsp-test. Vocabulary `just go-lsp-vocab` (go/doc, BSD prose).
- Format `[x]` (2026-11-07): a port of go/printer + text/tabwriter +
  go/doc/comment (doc-comment reflow) + ast.SortImports + gofmt's
  `//go:build` fixer and stdin fragment mode (`src/go_printer*`,
  `go_tabwriter`, `go_doc_comment`, `go_sort_imports`, `go_format`).
  `just format-compare go`: 11,442 of 11,442 files gofmt accepts
  byte-identical, 0 refusals/crashes; the 6 "not idempotent" are gofmt's
  own (internal/types testdata; our second pass equals gofmt's second
  pass). All 101 go/printer + cmd/gofmt testdata .input/.golden pairs
  match. The printer refuses past 4,000 levels of recursion (gofmt would
  print a 4,000-term sum). 20 checks in mep-go-format-test.
- ts: tree-sitter-go. oracle: `gofmt -e` / go/parser (shell).
- fmt: gofmt (shell) -- no options, byte identity on the whole corpus is
  the bar (Go's own source tree is the corpus). Stdlib vocabulary from
  `go doc -all`.

#### zig
- [x] Grammar  - [ ] LSP  - [ ] Format
- Grammar `[x]` (2026-11-08): `grammars/tree-sitter-zig` (README there),
  tree-sitter-grammars/tree-sitter-zig 1.1.2 (MIT, credited, license in
  grammar.js) taken in-house with its node API intact (95 node types, 17
  fields). Fixed against Zig 0.16's parser: asm clobbers as one expression
  (`::: .{ .memory = true }`), `async`/`await` as names (no longer keywords),
  an `if` type after `!`/`[]`/`[N]`, `error{...}`/parenthesized/labeled-block
  field types, `true:` as a label. Corpus (`just grammar-corpus zig`: Zig
  0.16's lib/, src/ and test/, zls, ghostty, river, waylock, ncdu,
  linuxwave, tigerbeetle, bun; 4,889 files, 2.53M lines). Oracle Zig's own
  parser via `zig fmt --stdin` (`tools/ts_grammar_diff/zig_check.sh`; `zig
  ast-check` would add AstGen errors): 4,817 identical, 69 reviewed known
  differences (all old parse errors), 3 in files Zig rejects, 0
  unexplained; files with parse errors 156 -> 88 (64 Zig rejects, 24
  compile-error tests with malformed literals Zig's tokenizer reads as one
  token). Fuzz clean (7,889 files); recovery +0.8% error bytes (better in
  95, worse in 76; seed 11 +0.3%). Queries (highlights, folds, structure)
  beside the grammar; dropped tree-sitter-zig from flake.nix and
  scripts/ts_grammars.tsv.
- ts: tree-sitter-zig. oracle: `zig ast-check` (shell).
- fmt: `zig fmt` (shell), byte identity. Zig's own std is the corpus.

#### rust
- [x] Grammar  - [~] LSP  - [~] Format
- Grammar `[x]` (2026-11-08): `grammars/tree-sitter-rust` (README there),
  tree-sitter-rust 0.24.2 (MIT, credited, license in grammar.js) taken
  in-house with its node API intact (163 node types, 31 fields, plus
  `box_pattern`; its 151 corpus cases pass, plus 11 of mep's). Fixed
  against rustc's parser, mostly the nightly syntax std and the compiler
  are written in: decl macros (`macro m(..) {..}`), metavariable
  expressions and a lone `$` in macro bodies, macro_rules attr/derive
  rules, const/auto traits, `impl const`, `[const]`/`~const` bounds, trait
  aliases, default field values and `S { .. }`, extern types, `safe fn`,
  associated type defaults, `do yeet`/`become`, const and `for<'a>`
  closures, attributes on let initializers/tuple elements/pattern fields,
  `super let`, `async gen`, `box` patterns, f16/f128. Corpus (`just
  grammar-corpus rust`: rustc 1.97.1's library/, compiler/, rustfmt,
  clippy lints, rust-analyzer, cargo, 38 vendored crates (tokio, serde,
  syn, regex, ...), ripgrep, fd, bat, helix; 10,667 files, 4.27M lines).
  Oracle rustc itself: `RUSTC_BOOTSTRAP=1 rustc -Zparse-crate-root-only`
  at the crate's edition (`tools/ts_grammar_diff/rust_check.py`, stable
  rustc from the dev shell): 10,235 identical, 432 reviewed known
  differences (old parse errors, f16/f128 as primitive types), 0
  unexplained; files with parse errors 493 -> 228 (190 rustc rejects too,
  36 rust-analyzer parser test data, 2 others). Fuzz clean (13,667
  files); recovery 2.8% fewer error bytes (better in 93, worse in 17).
  Queries (highlights, folds, structure) beside the grammar; dropped
  tree-sitter-rust from flake.nix and scripts/ts_grammars.tsv.
- ts: tree-sitter-rust. oracle: rustc's parser (`rustc -Zparse-crate-root-only`
  with `RUSTC_BOOTSTRAP=1` on the stable rustc, `rust_check.py`).
- fmt: rustfmt (nix shell), default config. Macros are token trees: the
  LSP resolves `macro_rules!` only shallowly; no type inference beyond
  what hover/completion needs.

#### d
- [ ] Grammar  - [ ] LSP  - [ ] Format
- ts: gdamore/tree-sitter-d. oracle: `ldc2 -o-` / dmd (shell).
- fmt: dfmt (nix shell).

#### nim
- [ ] Grammar  - [ ] LSP  - [ ] Format
- ts: alaviss/tree-sitter-nim. oracle: `nim check` (shell).
- fmt: nimpretty (shell).

### 4.6 Wave 6 -- JVM family and Dart (C-style syntax, shared layout ideas)

#### java
- [ ] Grammar  - [ ] LSP  - [ ] Format
- ts: tree-sitter-java. oracle: javac `-XDshould-stop.at=PARSE`
  (shell). fmt: google-java-format (nix shell).
- JDK vocabulary generated from the `jrt:/` modules (javadoc for hover).

#### kotlin
- [ ] Grammar  - [ ] LSP  - [ ] Format
- ts: fwcd/tree-sitter-kotlin. oracle: kotlinc's PSI parser (shell; slow,
  batch it). fmt: ktfmt or ktlint (nix shell).

#### scala
- [ ] Grammar  - [ ] LSP  - [ ] Format
- ts: tree-sitter-scala. oracle: `scalac -Ystop-after:parser` (shell),
  Scala 3 and 2 syntax. fmt: scalafmt (nix shell), default config.

#### groovy
- [ ] Grammar  - [ ] LSP  - [ ] Format
- ts: murtaza64/tree-sitter-groovy. oracle: groovyc to the CONVERSION
  phase (shell). fmt: no standard -- npm-groovy-lint's formatter or our
  own written-down rules. Gradle build scripts belong in the corpus.

#### dart
- [ ] Grammar  - [ ] LSP  - [ ] Format
- ts: UserNobody14/tree-sitter-dart. oracle: the `analyzer` package's
  parser (shell `dart`). fmt: `dart format` (shell), byte identity.

### 4.7 Wave 7 -- functional languages

#### ocaml
- [ ] Grammar  - [ ] LSP  - [ ] Format
- ts: tree-sitter-ocaml (+ `.mli` interface). oracle: `ocamlc
  -stop-after parsing` (shell). fmt: ocamlformat (nix shell).

#### haskell
- [ ] Grammar  - [ ] LSP  - [ ] Format
- ts: tree-sitter-haskell (layout scanner). oracle: GHC's parser
  (`ghc -fno-code`, or ghc-lib-parser in a small tool) (shell).
- fmt: ormolu (nix shell) -- no options, byte identity.

#### elixir
- [ ] Grammar  - [ ] LSP  - [ ] Format
- ts: elixir-lang/tree-sitter-elixir. oracle: `Code.string_to_quoted`
  (shell). fmt: `mix format` (shell), byte identity.

#### erlang
- [ ] Grammar  - [ ] LSP  - [ ] Format
- ts: WhatsApp/tree-sitter-erlang. oracle: `epp` + `erl_parse` in an
  escript (shell). fmt: erlfmt (nix shell).

### 4.8 Wave 8 -- scripting languages

#### ruby
- [ ] Grammar  - [ ] LSP  - [ ] Format
- ts: tree-sitter-ruby. oracle: prism (`ruby --parser=prism -c`) (shell).
- fmt: RuboCop's Layout cops (`rubocop -x`, shell).

#### perl
- [ ] Grammar  - [ ] LSP  - [ ] Format
- ts: tree-sitter-perl/tree-sitter-perl. oracle: `perl -c` runs BEGIN
  blocks -- use it only inside a sandbox on corpus files, PPI otherwise.
- fmt: perltidy (nix shell), default profile.

#### php
- [ ] Grammar  - [ ] LSP  - [ ] Format
- ts: tree-sitter-php (php + php_only). oracle: `php -l` (shell).
- fmt: php-cs-fixer, PSR-12 (nix shell).

### 4.9 Wave 9 -- LaTeX

#### latex
- [ ] Grammar  - [ ] LSP  - [ ] Format
- ts: latex-lsp/tree-sitter-latex. oracle: none at parse level -- a
  `pdflatex -draftmode` compile for the error checks, chktex for lint
  calibration (nix shell).
- LSP after texlab: labels/refs, citations from `.bib`, `\usepackage`
  command completion, environments outline. Share mep's LaTeX math engine
  for hover previews.
- fmt: latexindent (nix shell), default settings.
