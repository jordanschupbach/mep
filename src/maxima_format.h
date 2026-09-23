#pragma once

// TODO.org's "maxima formatter": mep's own Maxima source formatter, with
// no dependency on an external binary -- no Maxima installation, no
// subprocess, nothing to install. The same "keep dependencies to an
// absolute minimum" ethos as r_format.h and cpp_format.h next to it.
//
// Unlike those two, this one does not carry its own lexer and parser: it
// reads the source with maxima_ast.h, the same reader mep's Maxima
// language server uses. That is worth more here than the decoupling
// would be. Maxima's lexical rules are unusual enough (a `\` at end of
// line vanishes *inside* a number, `?foo` swallows whatever follows it,
// a file can add operators to the grammar with `infix("~")`) that a
// second implementation of them would be a second set of the same bugs
// -- and because the formatter's own self-check re-lexes its output with
// that reader, having one reader is what makes the check mean something.
//
// --- The style, and where it comes from -------------------------------
//
// Not invented. Maxima ships its own code printer, `grind`, and that is
// what a Maxima user already sees when they ask the system to show them
// a function. Its conventions were measured against the 657 `.mac` files
// Maxima's own distribution ships, and the two agree on the things that
// matter:
//
//   - A call's parenthesis is tight: `f(x)`, never `f (x)` (the corpus
//     writes it tight 113,051 times against 22,752).
//   - Arithmetic is dense: `a+b`, `u^2`, `x/2` (18,040 against 3,378).
//   - Comparisons and logic are spaced: `a > 0`, `p and q`, `x # y` --
//     `grind` prints them that way too, and it is what keeps a condition
//     legible inside dense arithmetic.
//   - Two-space indentation (the corpus's most common by a wide margin).
//
// Where `grind` and the corpus disagree, or where the corpus is split
// down the middle, this formatter chooses the readable option and says
// so here:
//
//   - `,` is followed by a space. `grind` packs them tight; real files
//     are split 100,398 to 56,218, and every other formatter mep ships
//     puts a space there.
//   - `:` and `:=` are spaced: `x : 1`, `f(u) := u^2`. The corpus is
//     nearly even (9,114 to 7,974) and this is the tiebreak that makes
//     the rule above worth having -- *structure is spaced, arithmetic is
//     dense*, so `a : x+y` shows at a glance which part is the
//     assignment and which is the sum.
//
// Formatting is width-driven rather than layout-preserving: the input's
// own line breaks inside an expression are discarded and recomputed, so
// the same code always comes out the same way. What is preserved is
// everything the parse tree cannot re-derive: comments (with their
// attachment to the construct they precede or trail), blank lines
// between statements (collapsed to at most one), and -- this one
// matters in Maxima and nowhere else -- each statement's own terminator,
// because `;` displays the result and `$` does not.
//
// --- Safety ----------------------------------------------------------
//
// Format() never returns text it is not sure about. A file it cannot
// parse comes back unchanged with `ok == false` and the offending line
// in `error`, and even a file it *can* parse is re-lexed after
// formatting and compared against the input token for token. A Maxima
// formatter only ever moves whitespace, so that comparison needs no
// tolerance list at all: any difference is a formatter bug, and the
// original text is returned rather than a mangled buffer.

#include <string>

namespace mxfmt {

struct Options {
    // Right margin the formatter tries to keep every line inside, in
    // characters (UTF-8 codepoints, not bytes). A line can still exceed
    // it when nothing in it is breakable -- a long string literal, a
    // deeply subscripted name.
    int width = 80;
    // Spaces per indentation level.
    int indent = 2;
};

struct Result {
    // False if the source could not be read, or if the self-check caught
    // a formatting bug. `text` is then the input verbatim.
    bool ok = false;
    std::string text;
    // Human-readable, ready to show in a notification; empty when ok.
    std::string error;
    // 1-indexed source line the error was detected on, 0 if not known.
    int error_line = 0;
};

// Formats a whole Maxima source file. `src` may use either line ending;
// the result always uses "\n" and ends with exactly one.
Result Format(const std::string &src, const Options &opts = Options());

}  // namespace mxfmt
