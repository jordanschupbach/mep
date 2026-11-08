# tree-sitter-matlab

A [tree-sitter](https://tree-sitter.github.io/) grammar for **MATLAB and
Octave** (`.m` files), written for mep and compiled into it. It reads
Octave the way Octave 11's own parser does (`libinterp/parse-tree/lex.ll`
and `oct-parse.yy`), MATLAB included.

It started from
[acristoffers/tree-sitter-matlab](https://github.com/acristoffers/tree-sitter-matlab)
1.3.0 (MIT, Copyright (c) 2023 Álan Crístoffer, with Amaan Qureshi), the
grammar mep loaded from nixpkgs before this one. Its `grammar.js` and
`src/scanner.c` are that grammar's, extended; its node names, fields and
tree shapes are kept, so queries and tools written for it work unchanged:
all 77 of its node types and 10 fields, plus three new ones for Octave's
own statements (`do_statement`, `unwind_protect_statement`,
`unwind_protect_cleanup_clause`). Its corpus tests (`test/corpus/`, copied
from it) pass, except four that tested MATLAB's `!` shell escape and two
error-recovery shapes, rewritten for Octave; `test/corpus/octave.txt` adds
Octave's constructs.

## What Octave adds

The old grammar read MATLAB only. What this one adds, each checked
against Octave's parser:

* **Comments**: `#` as well as `%`, and `#{ ... #}` blocks (either
  character opens or closes one).
* **End keywords**: `endif`, `endwhile`, `endfor`, `endparfor`,
  `endswitch`, `end_try_catch`, `end_unwind_protect`, `endfunction`,
  `endclassdef`, `endproperties`, `endmethods`, `endevents`,
  `endenumeration`, `endarguments`, `endspmd`.
* **Statements**: `do ... until cond`, `unwind_protect ...
  unwind_protect_cleanup ... end_unwind_protect`, `for [val, key] = s`.
* **Operators**: `!` and `!=`, `++`/`--` (prefix and postfix), the
  compound assignments (`+=`, `-=`, `*=`, `/=`, `\=`, `^=`, `.*=`, `./=`,
  `.\=`, `.^=`, `&=`, `|=`). MATLAB's `!cmd` shell escape is gone: in
  Octave `!x` is logical not.
* **Expressions**: any value can be indexed (`(x > 0)(:)`, `[a; b](i)`,
  `{':'}(ones (1, n))`, `x.'(:)`, `"01"(k)`) or have a field taken
  (`(get (h, "p")).name`); an assignment is an expression inside
  parentheses and arguments (`if ((n = numel (x)) > 1)`,
  `f ([~, i] = sort (x))`) and chains (`a = b = 0`); `global x = 1`;
  default parameter values (`function f (a, b = 2)`); `_` between digits
  (`10_000`, `0xFF_FF`); a bare `:` in a cell (`{:, :}`).
* **Strings**: in a double-quoted string a backslash escapes whatever
  follows it (`"say \"hi\""`).
* **Newlines**: inside parentheses a newline is a blank (`error ("msg",\n
  arg)` needs no `...`), so the scanner makes a statement-ending newline
  token only where a statement can end; a block's last statement needs no
  separator before the keyword closing it (`if done, break end`); a file
  need not end in a newline; `...` before a CRLF takes the whole line end.
* **Command syntax** follows Octave's rule: after `name `, an operator
  followed by a blank continues an expression (`x - 1`, `x += 1`) and one
  that is not starts a command argument (`hold -on`); `(`, `{` and `=`
  always continue an expression.

## How it was checked

The corpus (`just grammar-corpus matlab`) is Octave 10.3's own `scripts/`,
`test/` and `examples/` and 30 Octave Forge packages (statistics, signal,
control, image, io, optim, symbolic, ltfat, matgeom, interval, ...):
6,676 files, 954k lines. Octave's parser
(`tools/ts_grammar_diff/octave_check.sh`, `__parse_file__`, which parses
without running) accepts 6,636 of them.

The old grammar has parse errors in 4,241 of them (5.4 MB inside ERROR
nodes: it reads no `#` comment, and most of the corpus starts with a `##`
header); this one has errors in 8 (148 bytes), 6 of them files Octave
accepts, each a construct it does not read:

* `f (name = value, positional)` -- an assignment as an argument when its
  left side is a plain name stays MATLAB's name=value argument, which
  must come last (2 files);
* an assignment inside a matrix (`[0 ym=max(x)]`), a field taken from a
  matrix (`[c{:}].strs`), `[ , ]`, and an anonymous function in a cell
  whose body is a call written with a space (`{@(h) set (h, ...)}`, where
  Octave's lexer stops treating whitespace as a separator) -- 1 each.

So the gate is `just grammar-diff matlab --old-clean`: every top-level
item the old grammar read *without* errors must come out the same. Of
367,985 such items, the differing ones fall in 102 files, all listed with
their reasons in `test/known-differences.txt` -- places where this grammar
follows Octave and the old one did not: an Octave end keyword the old
grammar read as a command (44 files, many of them nested functions
closed by `endfunction`), `...` before a CRLF (47, matgeom and nurbs), a
`catch` whose next line the old grammar took for the error identifier (3),
leading blank lines (5), and `+=`, `unwind_protect` and `do` read as
commands (1 each). 0 unexplained.

Fuzzing (`just grammar-fuzz matlab`): 9,676 files, 3,000 of them damaged,
clean under ASan and UBSan. Error recovery on the same 3,000 damaged files
(`just grammar-recovery matlab`): 72.7% fewer bytes in ERROR nodes than
the old grammar, better in 2,042 files, worse in 259.

| File | What it is |
|---|---|
| `grammar.js` | the grammar |
| `src/scanner.c` | the external scanner: comments, continuations, command syntax, strings, matrix element separators, statement-ending newlines |
| `src/parser.c`, `src/grammar.json`, `src/node-types.json`, `src/tree_sitter/` | generated by `just matlab-grammar` (mep-ts-generate) -- commit them, don't edit them |
| `queries/highlights.scm` | highlight captures (the old grammar's nvim queries, plus Octave's keywords and operators) |
| `queries/folds.scm` | fold ranges |
| `queries/structure.scm` | the structure sidebar's outline: classdefs, functions (nested ones nest), methods, properties, events, enumeration members |
| `test/corpus/*.txt` | corpus cases, run by `mep-ts-corpus-test` |
| `test/known-differences.txt` | the reviewed differences from the old grammar, for `just grammar-diff` |

## Why a scanner

MATLAB's syntax depends on whitespace in ways a context-free grammar
cannot see: `disp hello` is a command and `disp - 1` an expression;
`[a -1]` has two elements and `[a - 1]` one; `a'` transposes while `'a'`
is a string. The scanner decides these from the characters around the
token, as Octave's lexer does, and emits the zero-width element
separators inside brackets. Its state (inside a command, inside a string,
a pending separator, whether the end-of-input newline was made) is
serialized for incremental reparses.

## Working on it

```sh
just matlab-grammar                       # regenerate src/parser.c and run the corpus tests
just grammar-corpus matlab                # once
just grammar-diff matlab --old-clean      # must report 0 unexplained differences
just grammar-fuzz matlab
just grammar-recovery matlab
```

Then rebuild mep, which compiles `src/` directly (`ts_matlab` in
CMakeLists.txt) and embeds the queries into a generated header at
configure time. `mep-treesitter-test` checks highlights, folds, the
outline and incremental reparsing.
