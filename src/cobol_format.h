#pragma once

// mep's own COBOL formatter: the builtin behind `gf` in a COBOL buffer
// (mep.format_cobol) and textDocument/formatting in mep-cobol-lsp.
//
// It re-indents and changes nothing else -- no line is split, joined,
// re-cased or re-worded -- in a conservative GnuCOBOL layout, for fixed
// format (sequence area columns 1-6, indicator column 7, Area A 8-11, Area
// B 12-72, identification area 73-80) and free format alike. The format of
// each line is read the way mep-cobol-lsp reads it (cobol_syntax.h's
// CobolLineFormats: `>>SOURCE` / `$SET SOURCEFORMAT` directives, else a
// guess from the sequence area).
//
// The layout ("Area A" is column 8 in fixed format and column 1 in free
// format; "Area B" is column 12, or one indent in from column 1):
//   - division, section and paragraph headers, FD/SD/RD entries, level
//     01/66/77/78 entries, DECLARATIVES and END PROGRAM in Area A;
//   - data entries indented by their place in the record's hierarchy (an
//     entry nested under a lower level number one indent further in, an 88
//     one indent under its item), whatever their level numbers are;
//   - statements and environment division entries in Area B; the bodies of
//     IF/ELSE, EVALUATE ... WHEN, inline PERFORM ... END-PERFORM, SEARCH
//     ... WHEN and the conditional phrases (AT END, ON SIZE ERROR, INVALID
//     KEY, ON EXCEPTION, ON OVERFLOW, their NOT forms) one indent further
//     in per level, scope terminators (END-IF, ...) and ELSE back at their
//     statement, WHEN and the phrases one indent in from theirs; a period
//     closes every open scope;
//   - a statement or entry continued on further lines keeps its
//     continuation lines where they were relative to its first line (in
//     the procedure division at least one indent in);
//   - with align_pic, a PICTURE clause right after the name of a one-line
//     data entry starts at Area A + 32 (column 40 in fixed format) when the
//     name ends before that.
//
// What it leaves exactly as it is: comment lines, blank lines, compiler
// directives (`>>...`, `$SET`), COPY and REPLACE statements, EXEC ...
// END-EXEC blocks, debugging lines (`D` in column 7), comment entries
// (AUTHOR. and the like), continued literals and the lines continuing them
// (`-` in column 7), lines with tabs before their code, the sequence and
// identification areas, and in fixed format any line that would no longer
// fit in columns 8-72 -- such a line is left unchanged rather than broken.
// Every changed line is checked to hold the same words and literals as
// before; anything else is refused (ok = false, text = the input).
// Formatting formatted text changes nothing.

#include <string>
#include <string_view>

namespace cobolfmt {

struct Options {
    int indent = 4;          // columns per scope level and per data level
    bool align_pic = true;   // line PICTURE clauses up (see above)
};

struct Result {
    bool ok = false;
    std::string text;
    std::string error;
    int error_line = 0;  // 1-based; 0 when not tied to a line
};

/** @brief Formats COBOL source (fixed or free format); on failure `ok` is false and `text` is the input. */
Result Format(std::string_view src, const Options &opts = {});

}  // namespace cobolfmt
