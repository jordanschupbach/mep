#pragma once

// mep's own in-house C++ code formatter -- TODO.org's "c++ formatter" ("I'd
// like an inhouse version of a formatter for c++ ... entirely inhouse").
// Nothing here shells out: there is no clang-format, no libclang, no LLVM and
// no compiler invocation anywhere in this file's dependency set (the .cpp
// includes <string>, <vector>, <algorithm> and <cctype> and nothing else),
// the same "keep dependencies to an absolute minimum" ethos as spell.cpp's
// hand-rolled checker, r_format.cpp and python_format.cpp.
//
// What it is modeled on is clang-format, because clang-format's output is
// what a C++ buffer is expected to look like -- and specifically the dialect
// mep's own source is written in: 4-space indents, `Type *name` with the star
// on the name, `if (x)` with a space but `foo(x)` without, namespace bodies
// not indented, access specifiers flush with the class, case labels indented
// one level inside the switch, continuation lines aligned under the open
// bracket and bin-packed, runs of trailing `//` comments lined up in one
// column with two spaces in front of them, and a braced initializer carrying
// a trailing comma left exploded one element per line.
//
// Where it differs from clang-format is in *how* it gets there, and that
// difference bounds what it can do. clang-format runs clang's real lexer,
// builds an annotated token tree and picks line breaks by solving a shortest
// path over break penalties. This reformats the *token stream* -- lex, group
// into unwrapped lines at `;`/`{`/`}`/labels, re-render each one under a
// spacing table, split the ones that are too long at their brackets and
// operators, greedily. That is enough for the overwhelming majority of real
// code and it is dramatically less machinery, but it means constructs whose
// layout depends on knowing the grammar rather than the token shape are
// deliberately left alone rather than guessed at. The known gaps, so nobody
// has to rediscover them:
//
//   - Preprocessor directives are re-spaced but never re-wrapped, and a
//     directive carrying a backslash continuation (the usual multi-line
//     `#define`) is emitted byte for byte. Code inside a macro body is not
//     formatted: without expanding it there is no way to know whether the
//     body is a statement, an expression or a fragment of either.
//   - `#` directives go flush left (clang-format's IndentPPDirectives: None),
//     including inside a function body.
//   - Greedy, not optimal, line breaking. clang-format will pick a worse
//     break early to get a better one later; this takes the first split that
//     fits and recurses. Long chained calls (`a.b().c().d()`) in particular
//     wrap less prettily than clang-format's.
//   - No comment reflowing. A `//` comment that is too long stays too long,
//     and a block comment's interior is only re-indented when every line of
//     it starts with a single `*` -- the standard `/* ... \n * ... \n */`
//     shape. A banner drawn with `**`, ASCII art or commented-out code is
//     art, and moving it would be a guess about what the author was lining
//     it up with.
//   - No `#include` sorting or grouping, and no insertion or rewriting of
//     anything that is not already in the token stream -- no braces, no
//     `const`, no blank lines, and in particular no fixing up of a
//     `}  // namespace foo` comment, which clang-format does and which is an
//     edit to the code's text rather than to its layout.
//   - Macro invocations that do not end in a semicolon (`ABSL_NAMESPACE_BEGIN`,
//     `GIO_AVAILABLE_IN_ALL`) read as part of the declaration that follows
//     them, because from a token stream that is exactly what they look like.
//   - Short constructs (`if (c) return;`, `int f() { return 1; }`) are kept on
//     one line if the input had them on one line and they fit; they are never
//     *joined* onto one line if the input had them apart. That preserves the
//     mixed style real C++ is written in rather than imposing one, at the cost
//     of not being canonical.
//
// Safety is the other half of the design, and C++ makes it easier than Python
// did: this formatter only ever changes whitespace, so the check is exact.
// Format() never returns text it has not proof-read -- the output is
// re-lexed and its token stream compared against the input's, token kind and
// spelling, with no tolerance at all except trailing whitespace inside
// comments. Any mismatch, like any lexer error, is reported as a failure with
// the original text left untouched. That is what makes a bug in the spacing
// table a refusal to format rather than a silently corrupted buffer: emitting
// `a+ +b` where the input said `a++b`, or letting a token drift past a `//`,
// changes the token stream and is caught. The caller ("gf" via mep.format_cpp)
// shows the message and does not write the buffer.
//
// There is exactly one place in C++ where whitespace between two tokens is
// *semantic* rather than cosmetic, and the check above is blind to it because
// the token streams really are identical: `#define X(a)` defines a
// function-like macro and `#define X (a)` an object-like one. That single
// adjacency is carried over from the source instead of being re-derived.
//
// The heuristics that C++ needs and a token stream cannot settle -- is this
// `<` a template bracket or a less-than, is this `*` a pointer declarator or a
// multiplication, is this `{` a block or a braced-init-list -- are therefore
// *cosmetic only*. Guessing wrong produces ugly output, never wrong output,
// because whichever way it guesses the same tokens come out in the same order
// and the verifier proves it.

#include <string>
#include <string_view>
#include <vector>

namespace cppfmt {

struct Options {
    // Column the splitter tries to keep lines within. A line with nothing to
    // split at (a very long string literal, a `//` comment, a preprocessor
    // directive) is still emitted too long -- this is a target, not a hard
    // guarantee.
    int column_limit = 100;

    // Columns per indent level. The input's own columns are read only to
    // preserve blank lines, to keep a short construct on the line it was
    // written on, and to tell a wrapped trailing comment from a standalone
    // one -- never to carry indentation over -- so a file indented with
    // tabs, 2 spaces or 7 comes out indented with this.
    int indent_width = 4;

    // Extra columns for a continuation line that could not be aligned under
    // its open bracket (clang-format's ContinuationIndentWidth).
    int continuation_indent = 4;

    // Columns between code and a trailing `//` comment on the same line. Two
    // is what mep's own source uses; clang-format's LLVM preset uses one.
    int spaces_before_trailing_comment = 2;

    // Line a run of trailing `//` comments up in one column, the way
    // clang-format's AlignTrailingComments does. A comment written hanging to
    // the right of its own statement -- the continuation of a trailing
    // comment that ran over -- joins the run above it; one written at the
    // statement's own indent does not.
    bool align_trailing_comments = true;

    // Consecutive blank lines are collapsed to at most this many. Blank
    // lines the author put at the edges of a block are kept, the way
    // clang-format's LLVM preset keeps them; only its Google preset strips
    // them, and stripping them is an opinion about the code rather than
    // about its layout.
    int max_empty_lines = 1;

    // Indent the contents of `namespace x { ... }`. Off is clang-format's
    // NamespaceIndentation: None and what mep's own source does.
    bool indent_namespaces = false;

    // Indent `case`/`default` one level inside the switch block, with their
    // statements one level further (clang-format's IndentCaseLabels: true).
    // Off puts the labels at the switch block's own indent.
    bool indent_case_labels = true;

    // Re-lex the result and compare token streams before returning it. Leave
    // this on: it is what makes a formatter bug a refusal to format rather
    // than a silently corrupted buffer. Off only for tests that deliberately
    // inspect intermediate output.
    bool verify = true;
};

struct Result {
    // False means nothing should be written back: `text` is empty and `error`
    // says why (something the lexer could not read, or a failed equivalence
    // check).
    bool ok = false;
    std::string text;
    std::string error;
    // 1-based source line the error was found on, or 0 if it is not tied to
    // one. Lets the caller put the cursor on the offending line.
    int error_line = 0;
};

// Formats `source` -- a whole translation unit or header, or any
// self-contained fragment starting at indent level 0 (an org
// `#+begin_src cpp` block body, already dedented by the caller, is the other
// real caller). The returned text always ends in exactly one newline when
// non-empty.
Result Format(std::string_view source, const Options &opts = Options());

}  // namespace cppfmt
