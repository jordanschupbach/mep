# Tree-sitter grammars: all in-house

Goal: every tree-sitter grammar compiled into mep is mep's own -- written
from the language's reference, living in `grammars/tree-sitter-<name>/`
with its queries beside it -- and nothing under `third_party/grammars/`
remains. Each replacement is a drop-in: same node names, fields and tree
shapes as the grammar it replaces, so every query, test and user's own
tooling keeps working, and the switch is invisible except where the new
grammar is more correct.

Related: `DEPENDENCY_REMOVAL_PLAN.md` §4 ("Lua and tree-sitter") covers
the tree-sitter *runtime* and the tree-sitter CLI; this plan is about the
grammars. Phase 9 below picks up the CLI.

Status legend: `[ ]` not started, `[~]` in progress, `[x]` done.

---------------------------------------------------------------------------

## 1. Where things stand (2026-10-18)

### Done

| Grammar | Replaced | Notes |
| --- | --- | --- |
| mepml | (always ours) | `grammars/tree-sitter-mepml`; its outline is built from headings in Lua |
| nix | dynamically loaded `tree-sitter-nix` | 41,317 nixpkgs files parse with no errors; agrees with upstream on 1,499/1,500 sampled files (the 1 is an upstream bug) |
| just | dead vendored query, no grammar | all 589 justfiles from just's own test suite that `just` accepts parse, with the recipes/assignments/aliases `just --dump` reports |
| r | vendored r-lib/tree-sitter-r v1.3.0 | identical trees on every file R accepts in R 4.5's sources + 11 CRAN packages (2,611 files, 543k lines) |

### Still vendored (`third_party/grammars/`)

| Grammar | Upstream | `parser.c` | Scanner | Queries mep uses | Hard-coded in `treesitter.cpp` |
| --- | --- | --- | --- | --- | --- |
| lua | tree-sitter-grammars v0.5.0 | 348K | 195 lines (long brackets) | highlights 206, folds 12, structure 9 | -- |
| c | tree-sitter v0.24.2 | 3.7M | none | highlights 83, folds 8, structure 10 | -- |
| python | tree-sitter v0.25.0 | 3.3M | 437 lines (indentation, strings) | highlights 278, folds 14, structure 4 | -- |
| javascript | tree-sitter v0.25.0 | 2.7M | 364 lines (ASI, templates, regex) | highlights 206, folds 14, structure 8 | -- |
| cpp | tree-sitter v0.23.4 | **16.9M** | 148 lines (raw strings) | highlights 154, folds 13, structure 18 | -- |
| org | nvim-orgmode 2.0.4 | 3.3M | 422 lines | highlights 80 (`Org*` captures), folds 6 | -- |
| markdown | tree-sitter-grammars v0.5.3 | 2.0M | **1,602 lines** | highlights 69 | `(inline)` nodes feed the inline pass |
| markdown_inline | tree-sitter-grammars v0.5.3 | 2.2M | 397 lines | highlights 58 | parsed over `(inline)` ranges |

Nothing outside `src/treesitter.cpp` walks a tree; every other consumer
(syntax, folds, the structure sidebar, mepml/org/markdown code blocks,
the notebook view, `bench_syntax`) goes through its API and the queries.
So a grammar's contract with mep is: its node names, fields and shapes as
the queries use them -- plus, for markdown, the `(inline)` node.

### Out of scope

The ~40 grammars `DynamicLanguageTable` loads from the user's machine
(Rust, Go, TypeScript, YAML, ...) are not vendored, cost the repository
nothing, and are not part of this plan. Any of them can be brought
in-house the same way nix and just were when there is a reason to (a
language mep users edit a lot, or one whose upstream grammar is wrong);
list candidates in §5 as they come up.

---------------------------------------------------------------------------

## 2. The method

What the R port did, and what every port below follows. Each step exists
because skipping it would have shipped a real bug in R (noted).

1. **Pin the public node API from the vendored parser itself.** Dump the
   compiled grammar's symbols and fields (`ts_language_symbol_name`,
   `ts_language_field_name_for_id` over `tree_sitter_<name>()`), and probe
   its trees on the language's awkward constructs. The vendored revision is
   the contract, not upstream's current `node-types.json` (for R they had
   already diverged: upstream added a `return` node and string fields).
2. **Write the grammar from the language's reference**, not from the
   upstream `grammar.js`: the language's own grammar/lexer source and
   spec (R: `gram.y` and `?Syntax`). Keep upstream's names, fields and
   shapes (which fields on `program` vs. bodies, which tokens are named
   nodes, how empty arguments appear) exactly.
3. **Differential test against the vendored parser** over a large real
   corpus: two tiny programs, one linked to each parser, print every named
   node as `depth field type start_byte end_byte`; a script diffs them per
   file and buckets the first difference by kind. Iterate until every
   difference is either (a) in a file the language's own implementation
   rejects, or (b) a case where the reference implementation agrees with
   us and the old parser was wrong. Ask the reference implementation when
   unsure (`Rscript -e 'parse(f)'`, `just --dump`, `python -m ast`,
   `node --check`, `luac -p`, `cc -fsyntax-only`).
   *R: took the trees from 16/2,611 identical to 2,604/2,611; it caught a
   function body on the next line ending the expression, `...length()`
   lexed as `...`, `..2dge` lexed as `..2`, and `?` binding tighter than a
   function body.*
4. **Fuzz the scanner under ASan + UBSan**: the corpus plus a few thousand
   randomly damaged copies (deleted runs, inserted delimiters, flipped
   bytes). Scanners only see their nastiest input during error recovery.
5. **Queries move beside the grammar** (`queries/highlights.scm`,
   `folds.scm`, `structure.scm`) and are rewritten in mep's convention
   (general patterns first: mep paints the later pattern on top), then
   removed from `treesitter_queries.h` / `treesitter_structure_queries.h`.
   Every grammar gets a structure query (the sidebar) -- that is part of
   done, not a follow-up.
6. **Tests in `mep-treesitter-test`**: highlight expectations, folds, the
   outline, and a case in `TestIncrementalRowsMatchWholeText` with edits
   that open and close the scanner's constructs (a stateful scanner's
   serialize/deserialize only matters on incremental reparses).
7. **Swap it in**: `mep_grammar(<name> <Camel>)` in CMakeLists.txt, drop
   it from `TS_GRAMMAR_NAMES`, register it in `treesitter.cpp`'s tables,
   `git rm` `third_party/grammars/<name>`, update `third_party/README.md`,
   `help/syntax.org`, add a `just <name>-grammar` recipe and a README.
   Build, run the related test binaries, and check it live in the editor
   (highlighting and the structure sidebar).

### Phase 0: keep the harness  `[ ]`

The R port's dump/diff/fuzz harness lived in a scratch directory. Commit
it so every port reuses it:

- [ ] `tools/ts_grammar_diff/dump.c` -- prints every named node of each
      file named on stdin (depth, field, type, byte range, error count).
- [ ] `tools/ts_grammar_diff/diff.py` -- per-file comparison, first
      difference bucketed by kind, files with errors in either parser.
- [ ] `tools/ts_grammar_diff/mutate.py` -- damaged copies for the fuzz run.
- [ ] `just grammar-diff <name> <corpus-list>` -- builds the old parser
      from the last commit that still vendored it (`git show
      <rev>:third_party/grammars/<name>/src/parser.c`), so the oracle
      outlives the vendored copy, and the new one from `grammars/`.
- [ ] A short "corpus" note per language (below) with the nixpkgs
      attributes that provide it, as `nixpkgs#R.src` did for R.

---------------------------------------------------------------------------

## 3. The ports, in order

Ordered smallest-risk first, so the harness and the method harden on the
easy ones; C++ follows C because it extends it; markdown is last because
its scanner is the largest and its two grammars are coupled to mep's code.

### Phase 1: Lua  `[ ]`

mep's own scripting language, so the one mep users edit most.

- Reference: the Lua 5.4 manual §9 (complete syntax) and `llex.c`.
- Scanner: long brackets `[==[ ... ]==]` for strings and `--[==[`
  comments (level kept as state, like R's raw strings).
- Corpus: Lua's own test suite (`lua-5.4.x-tests`), mep's `.lua` files,
  the Lua embedded in `src/main.cpp`'s `kBuiltin*` strings (extract them),
  nvim plugin sources from nixpkgs (`vimPlugins.*.src`).
- Oracle: `luac -p`.

### Phase 2: C  `[ ]`

- Reference: C23 (N3096) Annex A, plus GNU extensions the vendored
  grammar accepts (attributes, `__asm__`, statement expressions).
- No scanner today; keep it that way if possible. The preprocessor is the
  hard part: `#if`/`#else` arms holding half-statements, macros used as
  types/statements. Match the vendored grammar's error-tolerant shapes
  (`preproc_if`, `preproc_ifdef`, ...) exactly -- C code often relies on
  them parsing *somehow*.
- Corpus: the tree-sitter runtime and Lua C sources, SQLite amalgamation,
  CPython's `Objects/`/`Python/`, mep's own `.c`/`.h`.
- Oracle: `cc -fsyntax-only` on preprocessed files (for the
  differences that survive preprocessing).

### Phase 3: Python  `[ ]`

Where most of `mep-treesitter-test`'s expectations live, so the strictest
check of all.

- Reference: the Python grammar (`Grammar/python.gram`) and tokenizer
  docs; PEP 701 f-strings (nested quotes, `{` expressions, format specs,
  `=` debugging), PEP 695 type parameters, `match` soft keywords.
- Scanner: INDENT/DEDENT with a stack (stateful, serialized), line
  joining inside brackets, string prefixes and f-string interiors.
- Corpus: CPython's `Lib/` from `nixpkgs#python3.src` (~2,000 files),
  plus a few large packages (numpy, django sources).
- Oracle: `python -c 'import ast,sys; ast.parse(open(sys.argv[1]).read())'`.

### Phase 4: JavaScript  `[ ]`

- Reference: ECMA-262 (current edition) Annex A, plus JSX.
- Scanner: automatic semicolon insertion, regex vs. division, template
  literals with `${}` nesting, HTML comments in scripts, hashbang.
- Corpus: Node's `lib/` from `nixpkgs#nodejs.src`, mep's `web/` and
  `launcher/`, a few npm packages.
- Oracle: `node --check`.

### Phase 5: C++  `[ ]`

- Reference: the C++23 grammar (N4950 Annex A).
- Build on Phase 2's grammar (`grammar.js` can `require` the C grammar and
  extend it, as upstream does) so the C shapes stay shared.
- The vendored `parser.c` is **16.9 MB** -- by far mep's largest compile
  unit. Set a size budget (aim well under half) and measure each grammar
  change against it: template-argument ambiguity resolved with fewer GLR
  conflicts is where the size goes.
- Scanner: raw strings `R"delim(...)delim"`.
- Corpus: mep's own `src/` (422k lines of C++), libc++ headers, a few
  large projects (fmt, abseil) from nixpkgs.
- Oracle: `clang++ -fsyntax-only`.

### Phase 6: Org  `[ ]`

- Reference: the Org syntax spec (orgmode.org/worg/org-syntax.html) and
  mep's own `org_doc.cpp`, which already parses org for the org view --
  where the two disagree, decide which is right and fix the other, the way
  mepml's grammar and `mepml_doc.cpp` are kept in agreement.
- The highlight query uses its own `Org*` capture names, coloured by
  `mep.ts_capture_hl` in `main.cpp`; keep them.
- The sidebar outline is built in Lua (`mep_structure_org_items`); leave
  it, or replace it with a `structure.scm` if the tree makes it simpler.
- Corpus: the repo's 146 `.org` files, worg, the Org manual's source.

### Phase 7: Markdown + markdown_inline  `[ ]`

The largest scanner (1,602 lines) and the only grammar `treesitter.cpp`
itself depends on: `CollectMarkdownInlineSpans` reparses every `(inline)`
node's range with the inline grammar.

- Reference: CommonMark 0.31 + GFM (tables, task lists, strikethrough,
  autolinks), and the spec's ~650 examples as a second oracle: each example
  must produce the block structure its expected HTML implies.
- Keep the two-grammar split and the `(inline)` node (or change
  `treesitter.cpp` in the same commit, with its own test).
- The sidebar outline is built in Lua (`mep_structure_md_items`).
- Corpus: CommonMark spec examples, the repo's `.md` files, READMEs and
  docs of a few hundred nixpkgs sources.

### Phase 8: Retire `third_party/grammars/`  `[ ]`

- [ ] Remove `ts_build()`, `TS_GRAMMAR_NAMES` and the directory.
- [ ] `third_party/README.md`: the runtime only.
- [ ] `treesitter_queries.h` holds only the dynamically-loaded languages'
      queries; `treesitter_structure_queries.h` likewise.
- [ ] `DEPENDENCY_REMOVAL_PLAN.md`'s audit row: "tree-sitter runtime;
      grammars are mep's own".

### Phase 9: No tree-sitter CLI or Node to regenerate  `[ ]`

Every grammar still needs `tree-sitter generate` (and Node, to run
`grammar.js`) when it changes. Per `DEPENDENCY_REMOVAL_PLAN.md` §4: an
in-house generator that reads the committed `src/grammar.json` and emits
`parser.c` would remove that dev-time dependency for all of them at once.
Byte-identical output to the CLI's for the existing grammars is the
acceptance test.

---------------------------------------------------------------------------

## 4. Definition of done, per grammar

- [ ] Same named-node set and fields as the vendored revision (or every
      difference listed in the grammar's README, with why).
- [ ] Differential run: identical trees on every corpus file the
      language's own implementation accepts; remaining differences
      explained.
- [ ] Scanner clean under ASan + UBSan on corpus + damaged copies.
- [ ] `queries/highlights.scm`, `folds.scm`, `structure.scm`, with
      `mep-treesitter-test` expectations for each, and an incremental
      reparse case.
- [ ] Vendored copy removed; build, related test binaries and the
      editor (highlighting + structure sidebar) checked.
- [ ] README with the corpus numbers, `just <name>-grammar` recipe.

## 5. Candidates beyond the vendored set

Dynamically loaded today; worth bringing in-house when there's a reason.

- bash (shell blocks in org/mepml/markdown and justfile bodies)
- json, yaml, toml (config files)
- rust, go, typescript (common enough to justify, large grammars)
