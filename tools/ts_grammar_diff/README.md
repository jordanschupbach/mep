# ts_grammar_diff

The checks behind replacing a vendored tree-sitter grammar with one of
mep's own (`plans/TREESITTER_GRAMMAR_PLAN.md`, "The method"). A
replacement is meant to be a drop-in, so the old grammar is the oracle:
the new one must produce the same trees, node for node, on real code.

| File | What it is |
|---|---|
| `dump.c` | prints a grammar's trees (every named node: depth, field, type, byte range) or its node types and fields; built once per grammar |
| `grammar_diff.py` | the commands below; the per-language table (`LANGUAGES`) says where each corpus comes from, how to ask the language itself whether a file is valid, and what to fuzz with |

## Commands

```sh
just grammar-corpus r          # fetch + unpack R's sources and 11 CRAN packages
just grammar-symbols r         # node types/fields: old vs new
just grammar-diff r --oracle   # every tree, old vs new; ask R about each difference
just grammar-fuzz r            # corpus + 3,000 damaged copies under ASan + UBSan
just grammar-recovery r        # the same damaged copies, old vs new: bytes left in ERROR nodes
```

**`corpus <lang>`** builds `build/grammar-corpus/<lang>/files.txt`: the
repository's own files in the language plus the nixpkgs sources listed for
it, unpacked (archives are extracted with tarfile's data filter, and
nothing in them is run). `nix` uses the whole nixpkgs tree; `just` also
extracts the justfiles embedded in just's own test suite.

**`symbols <name>`** compares the grammars' named node types and field
names. A type or field only the old grammar has is a query that would stop
matching, so it fails (exit 1).

**`diff <name>`** parses the corpus with both and compares every file's
trees. It prints how many are identical, files with parse errors on
either side, and the first difference in each differing file, grouped by
kind (old node -> new node) with an example and its source line. With
`--oracle` it runs the language's own check (`Rscript`'s `parse`,
`python -m ast`, `node --check`, Guile's `read` (`scheme_read.scm`), go/parser (`go_check.go`), rustc's parser (`rust_check.py`), Zig's parser (`zig fmt --stdin`, `zig_check.sh`), Octave's `__parse_file__` (`octave_check.sh`), TypeScript's `createSourceFile`
(`ts_check.js`), `luac -p`, `just --dump`,
`nix-instantiate --parse`) on every file that differs or fails only in the
new grammar, and fails if any of them is valid: a difference in a file the
language rejects is error recovery, one in a file it accepts is a bug.
Without `--oracle` any difference fails. C, C++, Org and Markdown have no
oracle (a C file's validity depends on its include path; the other two
accept anything); for them `--old-clean` is the gate. It fails on a
differing file the old grammar parsed without errors, and on any top-level
item (a function, a declaration, a directive) that both grammars parse
without errors but to different subtrees -- so a file with one unparseable
macro still has all its other items checked. Items an error in either tree
touches are error recovery disagreeing: they are counted, with the total
bytes inside ERROR nodes on each side and how many files got better or
worse, but don't fail, because two grammars' recoveries from code neither
can parse (a statement macro with no `;`, an `#if` splitting an
expression) are never going to agree byte for byte. Watch the error-bytes
line instead: it should not grow.

Some differences are the old grammar being wrong in a file the language
accepts either way (the oracle can't tell). Once reviewed, list them in
`grammars/tree-sitter-<name>/test/known-differences.txt` as
`<path glob>  # why`: they are reported by reason (`--show-known`) instead
of failing, and an entry that no longer matches a differing file is
flagged so the list doesn't go stale. Keep the bar high -- every entry is a
claim that the new grammar is right.

**`fuzz <name>`** parses the corpus and `--count` damaged copies of it
(runs deleted, delimiters inserted, bytes flipped; `--seed` makes it
repeatable) with the new grammar built with AddressSanitizer and UBSan.
Each crash or hang (`--timeout` per batch of 500) is reported with the
sanitizer's report and the file that caused it, saved under
`build/grammar-diff/<name>/failures/`, and the run carries on with the
files after it. `--fuzz-old` fuzzes the old grammar instead.

**`recovery <name>`** makes the same damaged copies and parses them with
both grammars, reporting the bytes each leaves inside ERROR nodes, in
total and how many files got better or worse (the worst listed). Code
being edited is broken most of the time, and what recovery leaves outside
the errors is what still gets coloured and outlined -- so the total should
not grow. It found two of the Python scanner's choices costing a tenth
more error bytes than the old scanner's, invisible to `diff` (which only
sees valid code). The trees are streamed, never written: for C's corpus
they run to gigabytes.

## Which grammars

"New" is `grammars/tree-sitter-<name>/src`. "Old" is
`third_party/grammars/<name>/src` while it is still vendored; once it has
been removed, the same directory from git -- `HEAD` if the removal is not
committed yet, otherwise the commit before the one that removed it -- so
the oracle outlives the vendored copy. `--old`/`--new <src dir>` point at
any other grammar (an upstream checkout, say: `--old` with nixpkgs'
`tree-sitter-grammars.tree-sitter-nix.src` is how mep's Nix grammar was
compared with upstream's). A grammar mep loaded from nixpkgs rather than
vendoring names that source in its `LANGUAGES` entry (`old`: the attribute
and the directory in it), so `--old` is not needed: TypeScript's and TSX's
are nixpkgs' tree-sitter-typescript and -tsx.

Builds go under `build/grammar-diff/<name>/` and are reused until a source
changes.

## Where things stand

| Grammar | symbols | diff --oracle | fuzz | recovery |
|---|---|---|---|---|
| r | identical (40 node types, 20 fields) | 2,608 of 2,614 files identical; the other 6 are files R rejects | clean, 5,614 files | R_RECOVERY |
| scheme | old API intact (22 node types, +`gexp`/`ungexp`/`ungexp_splicing`) | oracle is Guile's `read` with Guix's reader extensions (`scheme_read.scm`): 2,871 of 3,610 identical; 715 known differences (G-expressions, `#u8(`, `#!...!#`, all old parse errors), 24 Guile rejects, 0 unexplained | clean, 6,610 files | 45.2% fewer error bytes; better in 620 files, worse in 0 |
| maxima | old API intact (23 node types, +`lisp_escape`) | oracle is Maxima's reader (`maxima_check.py`): 758 of 774 identical; 15 declare word operators, 1 `\`+newline in a number | clean, 3,799 files | better in 1,478 files, worse in 478; +4.8% error bytes |
| nix | differs from upstream by design (`escape_sequence`, `indented_string_fragment`) | -- | clean, 44,317 files | -- |
| just | no old grammar | -- | clean, 3,857 files | -- |
| lua | identical (47 node types, 23 fields) | 3,192 of 3,214 identical; 22 known differences, 0 unexplained | clean, 6,214 files | 87% fewer error bytes; better in 955 files, worse in 435 |
| c | identical (125 node types, 39 fields) | `--old-clean`: 49,606 clean items checked, 0 unexplained differences; error bytes -2.7% | clean, 3,870 files | C_RECOVERY |
| python | identical (123 node types, 32 fields) | 7,508 of 7,523 identical; 15 known differences (one old-scanner bug), 0 unexplained | clean, 10,523 files | 9.6% fewer error bytes; better in 258 files, worse in 145 |
| sql | old API intact (523 node types, 53 fields; +11 new for the additions) | `--old-clean`: 59,970 clean items, 0 unexplained; 7 known differences; error bytes -19.0% | clean, 5,567 files | 16.1% fewer error bytes; better in 1,555 files, worse in 59 |
| typescript | old API intact (177 node types, 40 fields; +`using_declaration`) | oracle is TypeScript's `createSourceFile` (`ts_check.js`): 2,140 of 2,151 identical; 11 known differences (old parse errors, tagged-template type arguments), 0 unexplained | clean, 5,151 files | 13.3% fewer error bytes; better in 238 files, worse in 167 |
| tsx | old API intact (184 node types, 43 fields; +`using_declaration`) | 806 of 912 identical; 106 known differences (all tagged templates with type arguments, which the old grammar could not read), 0 unexplained | clean, 3,912 files | 1.9% fewer error bytes; better in 204 files, worse in 155 |
| fish | old API intact (38 node types, 9 fields) | oracle `fish --no-execute`: 1,890 of 1,956 identical; 66 known differences (`(` in double quotes, `{` blocks, `^`/`&` in words, continuations carrying comments), 0 unexplained | clean, 4,956 files | 59.5% fewer error bytes; better in 1,002 files, worse in 49 |
| tcl | identical (35 node types, 7 fields) | oracle `tcl_check.tcl` (getbytecode) accepts all 1,810 files, so `--old-clean`: 158,026 clean items, 0 unexplained; 176 known differences (`::` names, word splits, one-word commands, comments); error bytes 1,737,345 -> 27 | clean, 4,810 files | 52.4% fewer error bytes; better in 2,427 files, worse in 141 |
| matlab | old API intact (77 node types, 10 fields; +`do_statement`, `unwind_protect_statement`, `unwind_protect_cleanup_clause`) | oracle `octave_check.sh` (Octave's `__parse_file__`) accepts 6,636 of 6,676 files; the old grammar reads no `#` comment (errors in 4,241), so `--old-clean`: 367,985 clean items, 0 unexplained; 102 known differences (Octave end keywords, CRLF continuations, `catch` + newline, ...); files with parse errors 4,241 -> 8 (6 Octave accepts), error bytes 5,412,631 -> 148 | clean, 9,676 files | 72.7% fewer error bytes; better in 2,042 files, worse in 259 |
| go | identical (107 node types, 35 fields) | oracle go/parser (`go_check.go`, `just go-check`): 11,477 of 11,486 identical; 7 known differences (`new(expr)`, `const x T`, `L1: L2:`: old parse errors, and `new(nil)` read as a type), 2 go/parser rejects, 0 unexplained; files with parse errors 52 -> 46 | clean, 14,486 files | +0.9% error bytes (all from `new(expr)`); better in 59 files, worse in 50 |
| rust | old API intact (163 node types, 31 fields; +`box_pattern`) | oracle rustc's parser (`rust_check.py`, `-Zparse-crate-root-only`): 10,235 of 10,667 identical; 432 known differences (265 old parse errors: decl macros, const traits, `[const]` bounds, default field values, ...; the rest `f16`/`f128` as primitive types, `box` patterns), 0 unexplained; files with parse errors 493 -> 228 (190 rustc rejects, 36 rust-analyzer parser test data) | clean, 13,667 files | 2.8% fewer error bytes; better in 93 files, worse in 17 |
| zig | identical (95 node types, 17 fields) | oracle Zig's parser via `zig fmt --stdin` (`zig_check.sh`): 4,817 of 4,889 identical; 69 known differences (asm clobber expressions, `async`/`await` as names, `!if` types, non-primary field types, `true:` labels: all old parse errors), 3 Zig rejects, 0 unexplained; files with parse errors 156 -> 88 | clean, 7,889 files | +0.8% error bytes; better in 95 files, worse in 76 |
| fortran | identical (218 node types, 30 fields) | oracle gfortran -fsyntax-only (`fortran_check.py`): 11,344 of 16,409 identical; 4,872 known differences (4,784 fixed-form files the old grammar read as free form, 6 continued strings, 82 where both fail on unseen macros/DEC extensions), 193 gfortran rejects, 0 unexplained; files with parse errors 5,853 -> 1,210 | clean, 19,409 files | 71.3% fewer error bytes; better in 1,011 files, worse in 212 |
| gap | identical (59 node types, 23 fields) | oracle is GAP's reader (`gap_check.py`, `ReadAsFunction`): 8,931 of 8,950 identical; 19 known differences (a file opening with `local`, which the old grammar could not read), 0 unexplained; files with parse errors 79 -> 60 (59 GAP rejects, 1 backslash-newline inside a token) | clean, 11,950 files (after prototyping the scanner's `create(void)`) | 0.1% fewer error bytes; better in 9 files, worse in 2 |
| julia | identical (86 node types, 6 fields) | oracle `julia_check.jl` (JuliaSyntax) accepts all 8,216 files, so `--old-clean`: 214,303 clean items, 0 unexplained; 7,353 of 8,216 identical; 536 known differences (`isa`/`in` as names, spaced-colon ranges, trailing `;` in arguments, `~` at assignment precedence, juxtaposition after `)`/adjoint, keyword symbols, suffixed operators, `::(...)` types, char literals, old root-ERROR recoveries); files with parse errors 446 -> 79, error bytes -46.9% | clean, 11,216 files (after prototyping the scanner's `create(void)`) | 3.3% fewer error bytes; better in 544 files, worse in 487 |
