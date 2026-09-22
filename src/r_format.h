#pragma once

// TODO.org's "r formatter": mep's own R source formatter, with no
// dependency on an external binary -- no `air`, no `styler`, no R
// installation at all. Everything from tokenizing to line breaking is
// in this module, the same "keep dependencies to an absolute minimum"
// ethos as spell.h's in-house checker and the in-house PDF stack.
//
// The style is tidyverse/air's: two-space indent, spaces around binary
// operators (except the tight ones -- ^ : :: ::: $ @), `<-` for
// assignment, double-quoted strings, and an 80-column margin that calls,
// argument lists and operator chains are broken across lines to respect.
// Formatting is width-driven rather than layout-preserving: the input's
// own line breaks inside an expression are discarded and recomputed, so
// the same code always comes out the same way. What *is* preserved is
// everything the parse tree cannot re-derive -- comments (with their
// attachment to the construct they precede or trail), and blank lines
// between statements (collapsed to at most one).
//
// Safety: Format() never returns text it is not sure about. A file it
// cannot parse comes back unchanged with `ok == false` and the offending
// line in `error`, and even a file it *can* parse is re-tokenized after
// formatting and compared against the input token-for-token (see
// r_format.cpp's Signature) -- a mismatch means a formatter bug, and the
// original text is returned rather than a mangled buffer.

#include <string>

namespace rfmt {

struct Options {
    // Right margin the formatter tries to keep every line inside, in
    // characters (UTF-8 codepoints, not bytes). A line can still exceed
    // it when nothing in it is breakable -- a long string literal, a
    // deeply indented identifier -- exactly as air behaves.
    int width = 80;
    // Spaces per indentation level.
    int indent = 2;
};

struct Result {
    // False if the source could not be parsed, or if the self-check
    // below caught a formatting bug. `text` is then the input verbatim.
    bool ok = false;
    std::string text;
    // Human-readable, ready to show in a notification; empty when ok.
    std::string error;
    // 1-indexed source line the error was detected on, 0 if not known.
    int error_line = 0;
};

// Formats a whole R source file. `src` may use either line ending; the
// result always uses "\n" and ends with exactly one.
Result Format(const std::string &src, const Options &opts = Options());

}  // namespace rfmt
