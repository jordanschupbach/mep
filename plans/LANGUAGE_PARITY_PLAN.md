# Language parity: run, grammar, LSP, UI mode for every example language

Goal: every language in `examples/language/` has all four pillars, each
mep's own:

1. **Run** -- `<leader>rr` / `gr` runs the example as-is.
2. **Grammar** -- an in-house tree-sitter grammar in
   `grammars/tree-sitter-<name>/`, compiled in, with highlights, folds and
   structure queries.
3. **LSP** -- an in-house language server, `mep-<lang>-lsp`, the default
   for the filetype.
4. **UI mode** -- `<leader>uu` lays out a live console plus panels, like
   the R, Python, C/C++ and Common Lisp modes.

Languages are done one at a time, in the order of their example files
(alphabetical): bash, c, clojure, common_lisp, cpp, crystal, csharp, ...
Within a language: run, grammar, LSP, UI mode -- the LSP and UI mode
reuse what the grammar work teaches about the language.

Status legend: `[ ]` not started, `[~]` in progress, `[x]` done,
`[-]` not applicable.

---------------------------------------------------------------------------

## 1. Where things stand (2026-10-20)

| Example | Run | Grammar | LSP | UI | Today, where not ours |
|---|---|---|---|---|---|
| bash | [x] | [x] | [x] | [x] | |
| c | [x] | [x] | [x] | [x] | |
| clojure | [x] | [ ] | [ ] | [ ] | `clojure.so`; clojure-lsp |
| common_lisp | [x] | [x] | [x] | [x] | |
| cpp | [x] | [x] | [x] | [x] | |
| crystal | [x] | [ ] | [ ] | [ ] | `crystal.so`; crystalline |
| csharp | [x] | [ ] | [ ] | [ ] | `c_sharp.so`; OmniSharp |
| dart | [x] | [ ] | [ ] | [ ] | `dart.so`; none |
| d | [x] | [ ] | [ ] | [ ] | `d.so`; serve-d |
| elixir | [x] | [ ] | [ ] | [ ] | `elixir.so`; elixir-ls |
| erlang | [x] | [ ] | [ ] | [ ] | `erlang.so`; none |
| fish | [x] | [ ] | [ ] | [ ] | `fish.so`; none |
| fortran | [x] | [ ] | [ ] | [ ] | `fortran.so`; fortls |
| gap | [x] | [ ] | [ ] | [ ] | `gap.so`; none |
| go | [x] | [ ] | [ ] | [ ] | `go.so`; gopls |
| groovy | [x] | [ ] | [ ] | [ ] | `groovy.so`; none |
| haskell | [x] | [ ] | [ ] | [ ] | `haskell.so`; hls |
| java | [x] | [ ] | [ ] | [ ] | `java.so`; jdtls |
| javascript | [x] | [x] | [ ] | [ ] | typescript-language-server |
| julia | [x] | [ ] | [ ] | [ ] | `julia.so`; LanguageServer.jl |
| kotlin | [x] | [ ] | [ ] | [ ] | `kotlin.so`; kotlin-language-server |
| latex | [x] | [ ] | [ ] | [ ] | `latex.so`; none (Run exports) |
| lua | [x] | [x] | [ ] | [ ] | lua-language-server |
| maxima | [x] | [ ] | [x] | [ ] | `maxima.so` |
| nim | [x] | [ ] | [ ] | [ ] | `nim.so`; nimlsp |
| nix | [x] | [x] | [ ] | [ ] | none |
| notebook | [-] | [-] | [-] | [-] | `.ipynb` is a view over Python cells, not a language |
| ocaml | [x] | [ ] | [ ] | [ ] | `ocaml.so`; ocamllsp |
| octave | [x] | [ ] | [ ] | [ ] | `matlab.so`; none |
| orgmode | [x] | [x] | [x] | [-] | a document, not a language with a console |
| perl | [x] | [ ] | [ ] | [ ] | `perl.so`; Perl::LanguageServer |
| php | [x] | [ ] | [ ] | [ ] | `php.so`; intelephense |
| python | [x] | [x] | [x] | [x] | |
| r | [x] | [x] | [x] | [x] | |
| ruby | [x] | [ ] | [ ] | [ ] | `ruby.so`; solargraph |
| rust | [x] | [ ] | [ ] | [ ] | `rust.so`; rust-analyzer |
| scala | [x] | [ ] | [ ] | [ ] | `scala.so`; metals |
| scheme | [x] | [ ] | [ ] | [ ] | `scheme.so`; none |
| sql | [x] | [ ] | [ ] | [ ] | `sql.so`; none |
| tcl | [x] | [ ] | [ ] | [ ] | `tcl.so`; none |
| typescript | [x] | [ ] | [ ] | [ ] | `typescript.so`; typescript-language-server |
| zig | [x] | [ ] | [ ] | [ ] | `zig.so`; zls |

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

### clojure, crystal, csharp, ...

Planned when their turn comes, in the same shape as bash: the reference
for the grammar, the corpus and oracle, the LSP's lint set, the UI mode's
console and panels.
