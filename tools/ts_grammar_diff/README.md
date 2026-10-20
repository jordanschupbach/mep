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
`python -m ast`, `node --check`, `luac -p`, `just --dump`,
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
compared with upstream's).

Builds go under `build/grammar-diff/<name>/` and are reused until a source
changes.

## Where things stand

| Grammar | symbols | diff --oracle | fuzz | recovery |
|---|---|---|---|---|
| r | identical (40 node types, 20 fields) | 2,608 of 2,614 files identical; the other 6 are files R rejects | clean, 5,614 files | R_RECOVERY |
| nix | differs from upstream by design (`escape_sequence`, `indented_string_fragment`) | -- | clean, 44,317 files | -- |
| just | no old grammar | -- | clean, 3,857 files | -- |
| lua | identical (47 node types, 23 fields) | 3,192 of 3,214 identical; 22 known differences, 0 unexplained | clean, 6,214 files | 87% fewer error bytes; better in 955 files, worse in 435 |
| c | identical (125 node types, 39 fields) | `--old-clean`: 49,606 clean items checked, 0 unexplained differences; error bytes -2.7% | clean, 3,870 files | C_RECOVERY |
| python | identical (123 node types, 32 fields) | 7,508 of 7,523 identical; 15 known differences (one old-scanner bug), 0 unexplained | clean, 10,523 files | 9.6% fewer error bytes; better in 258 files, worse in 145 |
