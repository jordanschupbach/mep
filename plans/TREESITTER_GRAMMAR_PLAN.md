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

## 1. Where things stand (2026-10-19)

### Done

| Grammar | Replaced | Notes |
| --- | --- | --- |
| mepml | (always ours) | `grammars/tree-sitter-mepml`; its outline is built from headings in Lua |
| nix | dynamically loaded `tree-sitter-nix` | 41,317 nixpkgs files parse with no errors; agrees with upstream on 1,499/1,500 sampled files (the 1 is an upstream bug) |
| just | dead vendored query, no grammar | all 589 justfiles from just's own test suite that `just` accepts parse, with the recipes/assignments/aliases `just --dump` reports |
| r | vendored r-lib/tree-sitter-r v1.3.0 | identical trees on every file R accepts in R 4.5's sources + 11 CRAN packages (2,614 files, 543k lines) |
| lua | vendored tree-sitter-grammars/tree-sitter-lua v0.5.0 | same 47 node types / 23 fields; identical trees on 3,192 of 3,214 files, the other 22 reviewed differences where Lua agrees with us (`test/known-differences.txt`) |
| c | vendored tree-sitter/tree-sitter-c v0.24.2 | same 125 node types / 39 fields; every top-level item both grammars parse without errors is identical (49,606 checked), except 3 files where the old grammar misread a preprocessor-split `else`; 2.7% fewer bytes in error regions |
| python | vendored tree-sitter/tree-sitter-python v0.25.0 | same 123 node types / 32 fields; identical trees on 7,508 of 7,523 files, the other 15 an old-scanner bug (`test/known-differences.txt`); 9.6% fewer bytes in errors on damaged code |
| javascript | vendored tree-sitter/tree-sitter-javascript v0.25.0 | same 114 node types / 36 fields; identical trees on 2,093 of 2,096 files (585 `.jsx`), the other 3 an old-grammar `await` bug (`test/known-differences.txt`); 5.2% fewer bytes in errors on damaged code |
| org | vendored nvim-orgmode/tree-sitter-org 2.0.4 (extended) | same 56 node types / 36 fields; all 1,459 cleanly parsed top-level items identical; files in error 16 -> 4; blocks close by name; agrees with org_doc.cpp (`mep-org-ts-test`) |
| markdown, markdown_inline | vendored tree-sitter-grammars/tree-sitter-markdown v0.5.3 | same 51 + 26 node types; 1,607 of 1,609 files identical for each, the 2 + 2 others CommonMark 0.31 corrections, checked against cmark by a structural oracle |
| commonlisp | (new: no grammar before) | written from CLHS ch. 2 (the reader); 1,512 of 1,521 files from SBCL's sources and six libraries parse with no errors, the other 9 user reader macros or deliberately malformed tests; scanner clean under ASan + UBSan on 4,521 files |
| cpp | vendored tree-sitter/tree-sitter-cpp v0.23.4 | same 206 node types / 50 fields, same rule order; built on the C grammar; every top-level item both grammars parse without errors is identical (38,871 checked); `parser.c` 16.2 MB against 17.3 MB, size budget relaxed (Phase 5) |

### Still vendored

None: `third_party/grammars/` is gone (Phase 8).

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
   Then **compare error recovery** on the same damaged copies (`just
   grammar-recovery <name>`): the bytes each grammar leaves in ERROR
   nodes. Code being edited is mostly broken, and step 3 only sees valid
   code. *Python: caught missing reserved keywords and two scanner
   choices, which together cost 14% against the old grammar before the fix.*
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

### Phase 0: keep the harness  `[x]`

Done 2026-10-18: `tools/ts_grammar_diff/` (README there) and four `just`
recipes. Step 1 is `just grammar-symbols <name>`, step 3 `just
grammar-corpus <lang>` then `just grammar-diff <name> --oracle`, step 4
`just grammar-fuzz <name>`.

- [x] `dump.c` -- every named node of each file (depth, field, type, byte
      range, error count), or `--symbols` for the node types and fields.
- [x] `grammar_diff.py diff` -- per-file comparison, first difference
      bucketed by kind with an example and its source line, files with
      errors in either parser; `--oracle` runs the language's own check on
      every suspect file and fails only on files the language accepts.
- [x] `grammar_diff.py fuzz` -- damaged copies (seeded), ASan + UBSan,
      crashes and hangs reported with their file and saved; checked
      against a scanner with a planted out-of-bounds write and a planted
      infinite loop (both caught, the run carrying on past each).
- [x] The old parser is found automatically: `third_party/grammars/<name>`
      while vendored, then the same path from git (`HEAD`, or the commit
      before its removal). `--old`/`--new` point anywhere else.
- [x] Corpora: the `LANGUAGES` table in `grammar_diff.py`, every nixpkgs
      attribute in it checked to exist: R (`R.src`, 11 `rPackages`), Nix
      (the nixpkgs tree), just (`just.src` + its test suite), Lua
      (luarocks, awesome, lua-language-server, 3 neovim plugins), C
      (sqlite, Lua, CPython), C++ (fmt, abseil, nlohmann_json), Python
      (CPython `Lib/`), JavaScript (Node `lib/`), Org (Emacs's manuals, ox-hugo, org-roam, org-ql, org-super-agenda), Markdown
      (cmark, Node's docs), plus the repository's own files in each.

Baselines it produced: R identical on every file R accepts (it also caught
the one remaining difference, `program` starting after leading whitespace,
now fixed); the R, Nix and just scanners clean under ASan + UBSan on 5,614,
44,317 and 3,857 files (Nix and just had never been fuzzed before).

---------------------------------------------------------------------------

## 3. The ports, in order

Ordered smallest-risk first, so the harness and the method harden on the
easy ones; C++ follows C because it extends it; markdown is last because
its scanner is the largest and its two grammars are coupled to mep's code.

### Phase 1: Lua  `[x]`

Done 2026-10-18: `grammars/tree-sitter-lua` (README there), vendored copy
removed.

- Reference: the Lua 5.4 manual §9 and `llex.c`; Lua 5.5's `global`
  declarations, which the vendored grammar already had.
- Scanner: long brackets (level kept as state) and what follows `--`.
- Corpus: luarocks, awesome, lua-language-server, telescope.nvim,
  nvim-treesitter, lazy.nvim and the repository's `.lua` (3,214 files);
  `luac -p` as oracle.
- Result: identical trees on 3,192 files. The 22 others are recorded in
  `test/known-differences.txt` -- `global` used as a name (18), `--[[`
  inside a quoted string (2), all-dash comments (2) -- each a vendored bug
  `luac` cannot arbitrate (it accepts both readings' files). Same 5 parse
  failures as the old grammar, all rejected by `luac`. Fuzz clean on
  6,214 files.
- Queries rewritten: highlights add Lua 5.4's builtins, the standard
  library tables as `@module`, and `self`/`_G`/`_ENV`/`mep` as builtins;
  folds add block comments and long strings; the outline adds
  `M.f = function` assignments and functions in table constructors.
- Tooling added on the way: `known-differences.txt` support in
  `grammar-diff` (reviewed differences are reported by reason and don't
  fail the gate; stale entries are flagged).

### Phase 2: C  `[x]`

Done 2026-10-18: `grammars/tree-sitter-c` (README there), vendored copy
removed.

- Reference: C23 (N3096) Annex A, plus the GNU and Microsoft extensions
  the vendored grammar accepts. No scanner.
- Corpus: SQLite, Lua's C, CPython's `Objects/` `Python/` `Modules/`
  `Include/`, and the repository's C (874 files, 2.4M lines). The repo's
  own `src/*.h` are C++ and left out.
- Gate: no oracle for C, so `grammar-diff --old-clean` (added for this
  phase): every top-level item the old grammar parsed without errors must
  be identical wherever the new grammar also parses it without errors --
  49,606 items; differences inside error regions are reported (with error
  bytes old vs new) but don't fail.
- Result: 0 items parsed cleanly by both but differently, apart from 3
  reviewed known differences. Error recovery: 2.7% fewer bytes in ERROR
  nodes overall, better in 57 files, worse in 9. Fuzz clean on 3,870
  files (the run first turned up a stack overflow in the harness's own
  tree walk on 17 MB damaged files -- `dump.c` now walks iteratively).
- One deliberate improvement: an `else_clause` may stand alone as a block
  item, so if/else chains split by `#if`/`#ifdef` parse (the old grammar
  failed, or read `else if (b) {...}` as a function named `if` returning
  `else`). It is also what most of the error-byte reduction comes from.
- Bugs the comparison caught on the way: string concatenation through
  macros (`"%" PRId64 "\n"`), a bare `NAME;` read as a declaration, and
  K&R vs modern parameters in `f(b) {}` -- then, from a corpus test, that
  the K&R fix had broken real K&R definitions.
- Queries rewritten: highlights split control flow into conditional/
  repeat/return, colour preprocessor directives, macros, parameters,
  labels, enumerators, attributes, escapes; folds add preprocessor arms,
  comments and argument/parameter lists; the outline adds typedefs of
  anonymous structs/unions/enums and `**`-returning functions.
- Known shared limitation, for later: SQLite's 9 MB amalgamation is one
  ERROR root in both grammars (one outer `#ifndef` and something inside it
  neither parses); and statement macros without `;` (`Py_BEGIN_ALLOW_THREADS`)
  are the most common error source -- worth a dedicated rule eventually.

### Phase 3: Python  `[x]`

Done 2026-10-19: `grammars/tree-sitter-python` (README there), vendored
copy removed.

- Reference: the Python 3.14 reference (lexical analysis, `python.gram`);
  PEP 701 f-strings, PEP 750 t-strings, PEP 695 type parameters and
  `type`, PEP 634 `match`, PEP 654 `except*`; Python 2's `print`/`exec`.
- Scanner: an indentation stack and a string-delimiter stack, both
  serialized (a string in an f-string's field may reuse its quotes).
- Corpus: CPython 3.13's `Lib/`, numpy, Django, requests, SymPy and the
  repository's Python (7,523 files, 2.6M lines); `python -m ast` as oracle.
- Result: same 123 node types / 32 fields; identical trees on 7,508
  files. The other 15 are one old-scanner bug (`test/known-differences.txt`):
  a raw string ending in `\\` lost its text. Same 2 parse failures as the
  old grammar -- one file Python rejects, and CPython's `test_compile.py`,
  whose bracketed line indented below its block (`(bar.\n    baz)`) the
  scanner reads as a dedent in both (it cannot see bracket depth). Fuzz
  clean on 10,523 files (the corpus plus 3,000 damaged copies).
- Lessons, now in the method:
  - **Rule order is load-bearing.** When two readings of an ambiguous
    construct (`f(*a.b)`: a splat of `a.b`, or an attribute of `*a`?)
    survive to the same point, tree-sitter keeps the one whose node types
    are numbered lower, and declaration order sets the numbering. The
    first draft differed on 504 files only because of that; the fix was
    to declare rules in the old grammar's order (compare the `enum
    ts_symbol_identifiers` of the two `parser.c`s).
  - **Reserved words and error recovery.** The old grammar used
    tree-sitter 0.25's `reserved` keywords: valid code parses identically
    with or without them, but on damaged code without them `in` and `def`
    were read as names and recovery got worse. `diff` cannot see this;
    the new `grammar-recovery` command (bytes left in ERROR nodes on
    damaged copies, old vs new) can. With the keywords reserved, and two
    scanner choices measured there, this grammar leaves 9.6% fewer bytes
    in errors than the old one (better in 258 files, worse in 145).
- Queries: highlights kept as they were (mep's own, already in mep's
  convention, and most of `mep-treesitter-test`'s expectations), plus `...`
  and line continuations; folds add each clause (elif/else/except/
  finally/case), comprehensions, sets, tuples, argument and parameter
  lists, parenthesized imports and multi-line strings; the outline adds
  `type` aliases and ALL_CAPS module constants.
- Tooling added on the way: `grammar-recovery`, and GC roots for the
  corpus's nixpkgs sources (Nix garbage-collected numpy's and Django's
  source directories in the middle of the phase).

### Phase 4: JavaScript  `[x]`

Done 2026-10-19: `grammars/tree-sitter-javascript` (README there), vendored
copy removed.

- Reference: ECMA-262 (2025) Annex A and §12, Annex B's HTML-like
  comments, stage-3 decorators and `using`, and JSX.
- Scanner: stateless -- automatic semicolons, template text, a
  conditional's `?` vs `?.`, HTML-like comments, JSX text; the parser's
  valid-token set tells it whether it is in an expression or a literal.
- Corpus: Node 22's `lib/` (and its bundled `deps/`), three React front
  ends for JSX (homepage-dashboard, navidrome, Mastodon) and the
  repository's JavaScript (2,096 files, 371k lines); `node --check` as
  oracle, esbuild for `.jsx` (Node rejects JSX).
- Result: same 114 node types / 36 fields; identical trees on 2,093
  files. The other 3 are one old-grammar bug (`test/known-differences.txt`):
  `await (f)()` and `await` + line break + operand were read as a name
  `await`. Same 6 parse failures as the old grammar, all accepted by the
  language, two shared limitations (README): a JSX attribute named by a
  reserved word right after the tag name (`<Collapse in={x}>`; putting the
  attributes under the `properties` reserved context was tried and does
  not fix it), and no automatic semicolon between an arrow function's `{}`
  body and a next line starting with `[`. Fuzz clean on 5,096 files;
  recovery 5.2% fewer error bytes (better in 66, worse in 84).
- Queries: highlights rewritten in mep's convention, and TypeScript's
  query now layers over only the part of it TypeScript's grammar shares
  (`LayeredTypescriptHighlights`: a "JavaScript only" section for bare
  parameters, and a "JSX" section kept for `.tsx`); folds add clauses,
  patterns, argument/parameter lists, imports, templates, comments and JSX
  elements; the outline adds `#private` methods, function-valued fields,
  `const C = class {}`, `module.exports.f =` assignments, object members
  and top-level ALL_CAPS constants.
- Tooling fixed on the way: `grammar-recovery` deadlocked once both pipes
  to the dump filled (the file list is now fed from a thread); and
  `CheckRowsMatchWholeText` compares rows in painting order, not listing
  order -- a row the incremental path carries over keeps its old listing
  when an edit elsewhere lengthens a node through it (an opened template).

### Phase 5: C++  `[x]`

Done 2026-10-19: `grammars/tree-sitter-cpp` (README there), vendored copy
removed.

- Reference: C++23 (N4950 Annex A). `grammar.js` requires the C grammar
  and extends it; C's rules are reordered to the old grammar's order and
  the C-only ones (K&R definitions, `macro_type_specifier`,
  `variadic_parameter`, C's annotated declaration declarators) dropped
  first, so the node types *and* their numbering match the old grammar.
- Scanner: raw strings, the open string's delimiter as state.
- Corpus: fmt, abseil, nlohmann_json and the repository's C++ (1,622
  files, 1.48M lines). No oracle: `clang++ -fsyntax-only` needs each
  project's include paths and configuration, and half the corpus is
  headers. Gate: `--old-clean`, as for C.
- Result: same 206 node types / 50 fields; every top-level item both
  grammars parse without errors is identical (38,871 checked), 1,587 files
  identical throughout. Both fail on the same 817 files (macro-heavy
  code), with 0.4% fewer error bytes here. Fuzz clean on 4,622 files.
  Recovery on damaged copies: 0.4% fewer error bytes,
  but worse in 87 files and better in 45 (two large regressions, in
  pathological generated code; README).
- Improvements where the old grammar had an error: `->*`, `p->B::~B()`,
  `template class X<int>;`.
- The diff caught: C's number literal (C++ suffixes are its own:
  `75106db` is `75106` + suffix `db`), `a.b < c || a.b >= d` read as a
  template call then `= d` (a plain field now has dynamic precedence over
  `template_method`), and `a.B::c` (a qualified field).
- Queries: highlights rebuilt on C's (C++ keywords, namespaces as
  `@module`, constructors, template brackets, raw strings, user-defined
  literal suffixes); folds add template and parameter lists, initializer
  lists, lambdas, requirement bodies, cases and catches; the outline adds
  aliases, concepts, operators, constructors, out-of-class `C::f`, and
  members declared in a class.
- **Size: budget relaxed** (2026-10-19). The plan asked for well under
  half the old 16.9 MB; `parser.c` is 16.2 MB (old 17.3 MB; object 3.3 MB,
  ~2 s at `-O2`). Removing features one at a time costs a few percent
  each at most (the README has the analysis): the bulk is ~530 contexts
  an expression can end in, each a "large" state tree-sitter writes as a
  full row because C++ has so many tokens that may follow an expression.
  Halving it with identical trees means many small restructurings, or a
  generator that renders the tables more compactly (Phase 9's). Neither
  is worth it for ~2 s of compile time, so the budget is now only that
  the parser does not grow: measure `parser.c` when changing the grammar.

### Phase 6: Org  `[x]`

Done 2026-10-19: `grammars/tree-sitter-org` (README there), vendored copy
removed.

- Reference: the Org syntax spec and Org 9.7's manual. The vendored grammar
  was nvim-orgmode's 2.0.4 *extended* (links, citations, inline source,
  `$` math, priority as a token), with no `grammar.js`: its shapes were
  pinned from the vendored tables and a 20-case probe suite comparing every
  node, anonymous tokens included (the queries match `"str"`, `"["`, ...).
- Scanner: the old one's list/section/`$` behaviour, rewritten (its
  serialization truncated indents over 127), plus zero-width tokens for
  what only the rest of a line or file tells: a drawer's `:NAME:` line, a
  property drawer's place, a block's matching `#+end_NAME`, and closers for
  `\(`, `\[`, `$$`, `[fn:`.
- Corpus: the repository's 146 `.org` files, Emacs's Org and Modus manuals,
  ox-hugo, org-roam, org-ql and org-super-agenda (228 files, 75k lines).
  No oracle: gate `--old-clean`.
- Result: same 56 node types / 36 fields; all 1,459 top-level items the old
  grammar parsed without errors are identical, the 4 other differing files
  one reviewed old bug (`test/known-differences.txt`: a `# comment` after a
  directive read as text). Files in error 16 -> 4 (99.3% fewer error
  bytes); recovery on damaged copies 58.8% fewer error bytes (better in
  243, worse in 106); fuzz clean on 3,228 files.
- Improvements, all where the old grammar had errors: blocks close by
  name (`#+begin_example` around a source block, `#+begin_details` around
  `#+begin_summary`), falling back to any `#+end_` when no matching one
  exists; openers need closers (Org shown in `=verbatim=`: four of mep's
  own help pages had errors); inline footnote definitions; markup in
  headlines.
- Lessons, now in the README:
  - **Lexical precedence must be numeric.** Named precedences in `token()`
    order nothing in the lexer; three bugs (a checkbox's `]`, a table's
    `|`, inline code's `}`) came from assuming they did.
  - **Shared tokens keep GLR alive.** Where two readings run to the line's
    end (plan or paragraph, tags or title, drawer or text), any token one
    reading has to itself lets the lexer kill the other early: `<`, `[`,
    `:` and the entry/tag/label tokens are shared for that reason.
  - **Not every line break can be a GLR fork.** Keeping "continue the
    paragraph or start a drawer" open at each line overflowed the parser's
    version limit on `:`-heavy text and split paragraphs; the scanner
    deciding the drawer line instead fixed it.
- org_doc.cpp: `mep-org-ts-test` holds the two to agreement on every
  repository `.org` file (headlines exactly, bracket links in prose). It
  found `ParseOrgOutline` counting a headline-looking line inside a block
  (an example block showing org) as a headline -- fixed, with a test --
  and the structure sidebar now takes its headlines from the tree
  (`structure.scm`), so the same holds there.
- Queries: highlights kept (Org's own capture names, coloured in main.cpp)
  plus the priority cookie; folds add drawers, property drawers, dynamic
  blocks and LaTeX environments; a new `structure.scm` (headline levels).
- Left for later: greater blocks' contents (quote, special) are text, not
  elements -- the old grammar's shape, kept; no emphasis/verbatim objects.

### Phase 7: Markdown + markdown_inline  `[x]`

Done 2026-10-19: `grammars/tree-sitter-markdown` and
`grammars/tree-sitter-markdown_inline` (READMEs there), vendored copies
removed; the two-grammar split and the `(inline)` node kept, so
treesitter.cpp's `CollectMarkdownInlineSpans` is unchanged.

- Reference: CommonMark 0.31 and GFM 0.29 (tables, task lists,
  strikethrough), front matter, `$` LaTeX spans. Shared rules (escapes,
  references, link parts) in `tree-sitter-markdown/common.js`.
- Scanners: the block one's open-block stack, line-start matching and
  soft/hard line-ending look-ahead, rewritten (one byte per block saved,
  bounded; the old one used four and did not check the buffer); the
  inline one's delimiter runs and code/LaTeX spans.
- Corpus: the repository's Markdown, cmark's and Node 22's docs, and every
  example of the CommonMark and GFM specs and both suites' regression and
  extension tests, one file each (`@spec-examples`, 1,403 files; 1,609 in
  all, 228k lines).
- Oracle: structural (`grammar-diff markdown --oracle`): each differing
  file's blocks -- paragraphs, headings by level, lists, items, quotes,
  code, HTML, rules, tables -- against cmark-gfm's and cmark 0.31's XML;
  fails only where the old grammar agrees and the new does not. Over the
  whole corpus the new grammar's blocks agree with cmark on 1,588 files
  (the old one's on 1,586).
- Harness: dump.c's `TS_RANGES_LANGUAGE` runs the inline grammar over the
  block grammar's `inline` ranges, as mep does; both variants take ranges
  from the old block grammar, so an inline difference is its own.
- Result: same node types and rule order for both. Block: 1,607 of 1,609
  files identical, the other 2 CommonMark 0.31 (`<textarea>` HTML blocks;
  only a list starting at 1 interrupts a paragraph), both agreeing with
  cmark. Inline: 1,607 identical, the other 2 0.31's declarations (`<!A>`
  is raw HTML). Recovery identical for both (Markdown has almost no
  errors); fuzz clean on 4,609 files each.
- Lessons: the old inline grammar's dynamic precedences were readable from
  its parse tables (each REDUCE action carries its production's) -- the
  first draft had two from memory wrong and resolved 219 files' links
  differently. And a block-start character the scanner consumes decides
  the scan either way: falling through to the line-ending check after it
  ate a `*` shifted every heading's line ending.
- Queries: highlights kept for both; a new `structure.scm` (ATX and setext
  headings) now drives the sidebar's Markdown outline, which therefore
  skips headings in `~~~` fences, indented code and HTML blocks, and lists
  setext headings. No fold query (main.cpp's provider folds Markdown).

### Phase 8: Retire `third_party/grammars/`  `[x]`

Done 2026-10-19.

- [x] `ts_build()`, `TS_GRAMMAR_NAMES` and the directory removed;
      `mep_grammar` builds all 12 compiled-in grammars.
- [x] `third_party/README.md`: the runtime only.
- [x] `treesitter_queries.h` holds only the dynamically-loaded languages'
      highlight queries (its orphaned fold-query comment moved to
      treesitter.cpp's `FoldQueryTable`); `treesitter_structure_queries.h`
      likewise.
- [x] `DEPENDENCY_REMOVAL_PLAN.md`'s audit row: "tree-sitter runtime;
      grammars are mep's own".
- `grammar-diff` keeps working: with the directory gone it takes the old
  grammar from the last commit that had it.
- Found on the way: the help pages (`just help-check`) were stale since
  Phase 1's Lua query, because org/HTML export kept an arbitrary one of
  several captures starting at the same column (`table.sort` is not
  stable; the old query's catch-all `@variable` had masked it). Export
  now lets the later pattern win, as the editor does, and the pages are
  regenerated: `mep.f()` calls and both braces of a table coloured again,
  `mep` itself as a builtin.

### Phase 9: No tree-sitter CLI or Node to regenerate  `[x]`

Done 2026-10-19: `tools/ts_generate` (README there). `just <name>-grammar`
now uses mep's own tools for the CLI's two jobs:

- `mep-ts-generate` replaces `tree-sitter generate` 0.25.10. It ports the
  CLI's `cli/generate` (grammar.json -> tables -> `parser.c`,
  `node-types.json`), and it runs `grammar.js` itself on mep's JS engine
  with an in-house `dsl.js`, so Node isn't needed either.
- `mep-ts-corpus-test` replaces `tree-sitter test`: each grammar's
  `test/corpus` against the compiled-in grammar, now part of `just test`.

- **Acceptance: byte-identical, end to end.** `just
  grammar-generate-check` regenerates all 12 grammars from `grammar.js`
  into a scratch directory. Every file (`grammar.json`, `parser.c`,
  `node-types.json`, the three headers) matches what the CLI committed.
  It takes ~23 s in all, most of it C++.
  The corpus runner passes all 77 cases, and a corrupted expectation fails.
- Byte-identity meant porting incidental behaviour, not just the
  algorithm:
  - Rust's derived orderings.
  - `sort_unstable`'s order for equal elements: it numbers the parse
    states.
  - regex-syntax's IR and Unicode tables.
  - serde_json's pretty printer.
- Found on the way:
  - **An engine bug.** An exception thrown inside a callback a native
    function calls (`map`, `forEach`, `call`, `bind`) reached `catch` as
    an Error with an empty message. The thrown value was dropped. It now
    arrives as itself (`RethrowFromNative`; test in `mep-html-doc-test`).
  - **A silent wrong answer.** mep's regex engine is ASCII-only, so Org's
    `caseInsensitive` (`/\p{L}/gu`) matched nothing and produced a
    different grammar without any error. Unicode property escapes now
    throw (`replace`/`match`/`test`/`exec`). Org's keywords are ASCII and
    use `[a-zA-Z]`, so its output is unchanged. A `\p{...}` used only as a
    token pattern is still fine.
- `tools/ts_grammar_diff` never needed the CLI: it compiles each side's
  `parser.c` directly.

---------------------------------------------------------------------------

## 4. Definition of done, per grammar

- [ ] Same named-node set and fields as the vendored revision (or every
      difference listed in the grammar's README, with why).
- [ ] Differential run: identical trees on every corpus file the
      language's own implementation accepts; remaining differences
      explained in `test/known-differences.txt` (each a case where the
      language agrees with the new grammar).
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
