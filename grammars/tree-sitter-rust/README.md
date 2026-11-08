# tree-sitter-rust

A [tree-sitter](https://tree-sitter.github.io/) grammar for **Rust**
(rustc 1.97, editions 2015 to 2024, including the unstable syntax the
compiler and the standard library are written in), compiled into mep. It
is [tree-sitter/tree-sitter-rust](https://github.com/tree-sitter/tree-sitter-rust)
0.24.2 (MIT, Copyright (c) 2017 Maxim Sokolov; the license is reproduced in
`grammar.js`'s header) -- the grammar mep loaded from nixpkgs before this
one -- taken in-house and checked against rustc's own parser, so its node
names, fields and tree shapes are that grammar's: the same 163 node types
and 31 fields, plus one new node type (`box_pattern`), and queries or
tools written for it work unchanged. Its corpus tests (`test/corpus/`,
copied from it) pass, plus `mep.txt` for the changes below.

## What changed

Places where rustc's parser accepts code the old grammar could not read.
Most are nightly syntax, but the standard library and the compiler use
all of it, so it is what real Rust looks like there:

* **Declarative macros 2.0**: `pub macro m($x:expr) { .. }` (one
  `macro_rule`, no `=>`) and `macro m { (..) => { .. }, .. }`, as
  `macro_definition`; `macro_rules!` rules for attributes and derives
  (`derive() (..) => ..`).
* **Inside macro bodies**: metavariable expressions (`${count($x)}`,
  `${concat(a, $b)}`), a `$` on its own (`$ name`, `[$]`) and `~`.
* **Const traits**: `const trait`, `const unsafe trait`, `impl const Trait
  for T`, `const impl<T> S<T>`, bounds `[const] Trait` / `~const Trait` /
  `const Trait` (also after `impl` and `+`); **auto traits**
  (`unsafe auto trait Send {}`); **trait aliases** (`trait A = B + C;`);
  `default impl`.
* **Default field values**: `field: T = expr` in a struct, and `S { a, .. }`
  with no base expression.
* **Extern types and items**: `type Opaque;` (no `=`), `safe fn` in an
  `unsafe extern` block; associated type defaults (`type A: B = T;`).
* **Expressions**: `do yeet e` and `become f()` (read as
  `return_expression`), `const` closures and closures with a `for<'a>`
  binder, attributes on a `let` initializer (`let f = #[inline] |x| x;`)
  and on tuple elements, `super let`, `async gen` blocks, negative
  const-generic arguments (`f::<-1>()`), the `f16`/`f128` types and
  literal suffixes, a macro named like a primitive type (`str![..]`).
* **Patterns and types**: `box` patterns (`box_pattern`, the one new node
  type), attributes on struct-pattern fields, `char @ ('(' | ')')`,
  `dyn 'a + Trait`, empty generics and bounds (`struct S<>;`, `T:`), unit
  structs with a `where` clause, several attributes on one parameter.

`safe` and `auto` stay ordinary identifiers everywhere else (`safe = true;`).

## How it was checked

The corpus (`just grammar-corpus rust`) is rustc 1.97.1's own tree -- the
standard library (core, alloc, std, their tests and the crates vendored
under `library/`), the compiler, rustfmt, clippy's lints, rust-analyzer's
crates (with its parser's test data) and cargo -- plus 38 widely used
crates from its `vendor/` directory (tokio, serde, syn, regex, clap,
hashbrown, rayon, crossbeam, futures, itertools, indexmap, gimli, object,
tracing, salsa, ...), and ripgrep, fd, bat and helix from nixpkgs: 10,667
files (after dropping 342 duplicates), 4.27M lines. The oracle is rustc's
own parser: `tools/ts_grammar_diff/rust_check.py` runs
`rustc -Zparse-crate-root-only` (stable rustc, `RUSTC_BOOTSTRAP=1`): the
file is parsed and nothing is expanded, resolved or type-checked, at the
edition its crate declares (a compiletest `//@ edition:` header, else the
nearest `Cargo.toml`).

* `just grammar-symbols rust`: the old API intact -- 163 node types and 31
  fields, plus `box_pattern`.
* `just grammar-diff rust --oracle`: 10,235 of 10,667 files identical; the
  other 432 are reviewed known differences (`test/known-differences.txt`,
  by category: in 265 the old grammar had a parse error this one reads --
  decl macros, const traits and impls, `[const]` bounds, `safe fn`,
  default field values, trait aliases, metavariable expressions, ... --
  and the rest are `f16`/`f128` now read as primitive types and `box`
  patterns), 0 unexplained. Files with
  parse errors: old 493, new 228 (0 only in the new grammar). Of the 228,
  190 are files rustc rejects too (deliberately invalid test inputs); 36
  of the other 38 are rust-analyzer's parser test data, which exercise
  syntax at the edge of what rustc's parser accepts (`builtin #`,
  `impl(crate)` restrictions, `type const`, generic consts, `Fn::(..)`,
  unsafe fields, frontmatter, ...), and the last 2 are `fn(&_)` in a
  fn-pointer type (`library/alloctests/tests/slice.rs`) and
  `macro_rules ! T_` with a space (a generated file).
* `just grammar-fuzz rust`: 13,667 files (3,000 damaged) clean under ASan +
  UBSan (the external scanner's `create` needed a `(void)` prototype for
  `-fsanitize=function`).
* `just grammar-recovery rust`: 2.8% fewer bytes in ERROR nodes than the
  old grammar on the 3,000 damaged copies (better in 93 files, worse in
  17).

| File | What it is |
|---|---|
| `grammar.js` | the grammar |
| `src/scanner.c` | the external scanner (strings, raw strings, float literals, block comments, doc comments), unchanged from upstream |
| `src/parser.c`, `src/grammar.json`, `src/node-types.json`, `src/tree_sitter/` | generated by `just rust-grammar` (mep-ts-generate) -- commit them, don't edit them |
| `queries/highlights.scm` | highlight captures (nvim-treesitter naming) |
| `queries/folds.scm` | fold ranges |
| `queries/structure.scm` | the structure sidebar's outline: modules, structs, unions, enums and their variants, traits, impls (by self type), functions and methods, type aliases and associated types, consts, statics, macros |
| `test/corpus/*.txt` | corpus cases, run by `mep-ts-corpus-test` |
| `test/known-differences.txt` | the reviewed differences from the old grammar, for `just grammar-diff` |

## Working on it

```sh
just rust-grammar                    # regenerate src/ and run the corpus tests
just grammar-corpus rust             # once
just grammar-diff rust --oracle      # must report 0 unexplained differences
just grammar-fuzz rust
just grammar-recovery rust
```

Then rebuild mep, which compiles `src/` directly (`ts_rust` in
CMakeLists.txt) and embeds the queries into a generated header at
configure time. `mep-treesitter-test` checks highlights, folds and the
outline.
