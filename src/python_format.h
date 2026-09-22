#pragma once

// mep's own in-house Python code formatter -- TODO.org's "python formatter"
// ("I'd like an inhouse version of a formatter for python ... entirely
// inhouse"). Nothing here shells out: there is no `black`, no `ruff format`,
// no Python interpreter and no library linkage anywhere in this file's
// dependency set (it includes <string> and <vector> and nothing else), the
// same "keep dependencies to an absolute minimum" ethos as spell.cpp's
// hand-rolled checker and editor.h's in-house fzf-style FuzzyScore.
//
// What it is modeled on is black, because black's output is what a Python
// buffer is expected to look like: 4-space indents, double quotes, 88-column
// lines, one argument per line with a trailing comma once a call has to be
// exploded, two blank lines around top-level defs. Where this differs from
// black is in *how* it gets there, and that difference bounds what it can do.
// black parses Python into a full lib2to3 syntax tree and reformats the tree;
// this reformats the *token stream* -- tokenize, group into logical lines,
// re-render each one under a spacing table, split the ones that are too long
// at their brackets. That is enough for the overwhelming majority of real
// code and it is dramatically less machinery, but it means constructs whose
// formatting depends on knowing the grammar rather than the token shape are
// deliberately left alone rather than guessed at. The known gaps, so nobody
// has to rediscover them:
//
//   - No "invisible parens". black wraps a too-long `return a and b` or
//     `x = a + b` in parentheses it invents in order to split it. This never
//     invents brackets, so an over-long line with no bracket in it is emitted
//     over-long rather than restructured.
//   - No `match`/`case` splitting. They are soft keywords (`match = 1` is a
//     valid assignment), so telling a match *statement* from a name needs the
//     grammar; both are left as written rather than risk mangling the latter.
//   - Backslash continuations are joined into their logical line and re-split
//     at brackets; a logical line that only fits *because* of a backslash
//     stays long rather than have the backslash re-invented.
//   - Comments are normalized and kept attached to the token they follow, but
//     are not moved between lines the way black's comment placement does.
//
// Safety is the other half of the design. A formatter that silently changes
// what code *means* is far worse than no formatter at all, so Format() never
// returns text it has not proof-read: the output is re-tokenized and its
// token stream compared against the input's (VerifyEquivalent, modulo exactly
// the normalizations this is allowed to make -- whitespace, string quoting,
// numeric literal case, and the trailing commas an exploded bracket gains).
// Any mismatch, like any tokenizer error, is reported as a failure with the
// original text left untouched. The caller ("gf" via mep.format_python) shows
// the message and does not write the buffer.

#include <string>
#include <string_view>
#include <vector>

namespace pyfmt {

struct Options {
    // Column the splitter tries to keep lines within, black's default. A line
    // with no bracket to split at (or whose own head already overruns) is
    // still emitted too long -- this is a target, not a hard guarantee.
    int line_length = 88;

    // Columns per indent level in the output. The *input's* indentation is
    // only ever read for its nesting structure (see Tokenizer's indent stack),
    // so a file indented with tabs, 2 spaces or 7 comes out indented with
    // this regardless.
    int indent_width = 4;

    // Rewrite string literals to double quotes where that does not add
    // backslash escapes, drop redundant `u` prefixes and lowercase prefixes.
    bool normalize_strings = true;

    // Lowercase numeric prefixes/exponent/`j` suffix and uppercase hex digits
    // (`0XAb1E5` -> `0xAB1e5` is wrong; `0XAb1` -> `0xAB1`, `1E5` -> `1e5`).
    bool normalize_numbers = true;

    // Re-tokenize the result and compare token streams before returning it.
    // Leave this on: it is what makes a formatter bug a refusal to format
    // rather than a silently corrupted buffer. Off only for the tests that
    // deliberately inspect intermediate output.
    bool verify = true;
};

struct Result {
    // False means nothing should be written back: `text` is empty and `error`
    // says why (a syntax error the tokenizer tripped on, or a failed
    // equivalence check).
    bool ok = false;
    std::string text;
    std::string error;
    // 1-based source line the error was found on, or 0 if it is not tied to
    // one. Lets the caller put the cursor on the offending line.
    int error_line = 0;
};

// Formats `source` (a whole Python file, or any self-contained fragment whose
// first line is at indent level 0 -- an org `#+begin_src python` block body,
// already dedented by the caller, is the other real caller). The returned
// text always ends in exactly one newline when non-empty.
Result Format(std::string_view source, const Options &opts = Options());

}  // namespace pyfmt
