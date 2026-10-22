# Language parity: run, grammar, LSP, UI mode for every example language

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

Languages are done one at a time, in the order of their example files
(alphabetical): bash, c, clojure, common_lisp, cpp, crystal, csharp, ...
Within a language: run, grammar, LSP, UI mode, format -- the LSP, UI
mode and formatter reuse what the grammar work teaches about the
language (the formatter usually prints the LSP's own parse tree).

Status legend: `[ ]` not started, `[~]` in progress, `[x]` done,
`[-]` not applicable.

---------------------------------------------------------------------------

## 1. Where things stand (2026-10-21)

| Example | Run | Grammar | LSP | UI | Format | Today, where not ours |
|---|---|---|---|---|---|---|
| bash | [x] | [x] | [x] | [x] | [x] |  |
| c | [x] | [x] | [x] | [x] | [x] |  |
| clojure | [x] | [x] | [x] | [x] | [x] |  |
| common_lisp | [x] | [x] | [x] | [x] | [x] |  |
| cpp | [x] | [x] | [x] | [x] | [x] |  |
| crystal | [x] | [x] | [x] | [x] | [x] |  |
| csharp | [x] | [x] | [x] | [x] | [x] |  |
| dart | [x] | [ ] | [ ] | [ ] | [ ] | `dart.so`; none |
| d | [x] | [ ] | [ ] | [ ] | [ ] | `d.so`; serve-d |
| elixir | [x] | [ ] | [ ] | [ ] | [ ] | `elixir.so`; elixir-ls |
| erlang | [x] | [ ] | [ ] | [ ] | [ ] | `erlang.so`; none |
| fish | [x] | [ ] | [ ] | [ ] | [ ] | `fish.so`; none |
| fortran | [x] | [ ] | [ ] | [ ] | [ ] | `fortran.so`; fortls |
| gap | [x] | [ ] | [ ] | [ ] | [ ] | `gap.so`; none |
| go | [x] | [ ] | [ ] | [ ] | [ ] | `go.so`; gopls |
| groovy | [x] | [ ] | [ ] | [ ] | [ ] | `groovy.so`; none |
| haskell | [x] | [ ] | [ ] | [ ] | [ ] | `haskell.so`; hls |
| java | [x] | [ ] | [ ] | [ ] | [ ] | `java.so`; jdtls |
| javascript | [x] | [x] | [ ] | [ ] | [ ] | typescript-language-server |
| julia | [x] | [ ] | [ ] | [ ] | [ ] | `julia.so`; LanguageServer.jl |
| kotlin | [x] | [ ] | [ ] | [ ] | [ ] | `kotlin.so`; kotlin-language-server |
| latex | [x] | [ ] | [ ] | [ ] | [ ] | `latex.so`; none (Run exports) |
| lua | [x] | [x] | [ ] | [ ] | [ ] | lua-language-server |
| maxima | [x] | [ ] | [x] | [ ] | [x] | `maxima.so` |
| nim | [x] | [ ] | [ ] | [ ] | [ ] | `nim.so`; nimlsp |
| nix | [x] | [x] | [ ] | [ ] | [ ] | none |
| notebook | [-] | [-] | [-] | [-] | [-] | `.ipynb` is a view over Python cells, not a language |
| ocaml | [x] | [ ] | [ ] | [ ] | [ ] | `ocaml.so`; ocamllsp |
| octave | [x] | [ ] | [ ] | [ ] | [ ] | `matlab.so`; none |
| orgmode | [x] | [x] | [x] | [-] | [-] | a document, not a language with a console |
| perl | [x] | [ ] | [ ] | [ ] | [ ] | `perl.so`; Perl::LanguageServer |
| php | [x] | [ ] | [ ] | [ ] | [ ] | `php.so`; intelephense |
| python | [x] | [x] | [x] | [x] | [x] |  |
| r | [x] | [x] | [x] | [x] | [x] |  |
| ruby | [x] | [ ] | [ ] | [ ] | [ ] | `ruby.so`; solargraph |
| rust | [x] | [ ] | [ ] | [ ] | [ ] | `rust.so`; rust-analyzer |
| scala | [x] | [ ] | [ ] | [ ] | [ ] | `scala.so`; metals |
| scheme | [x] | [ ] | [ ] | [ ] | [ ] | `scheme.so`; none |
| sql | [x] | [ ] | [ ] | [ ] | [ ] | `sql.so`; none |
| tcl | [x] | [ ] | [ ] | [ ] | [ ] | `tcl.so`; none |
| typescript | [x] | [ ] | [ ] | [ ] | [ ] | `typescript.so`; typescript-language-server |
| zig | [x] | [ ] | [ ] | [ ] | [ ] | `zig.so`; zls |

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

### dart, ...

Planned when their turn comes, in the same shape as bash: the reference
for the grammar, the corpus and oracle, the LSP's lint set, the UI mode's
console and panels.
