#ifndef MEP_COBOL_SYNTAX_H
#define MEP_COBOL_SYNTAX_H

// COBOL line structure shared by mep-cobol-lsp (cobol_lsp.cpp) and mep's
// COBOL formatter (cobol_format.cpp): which reference format a line is in,
// where its code area is, where a comment starts, and what a doc comment
// says. (Highlighting is the tree-sitter grammar's,
// grammars/tree-sitter-cobol, whose scanner tells the formats apart the
// same way.)
//
// Formats, as GnuCOBOL reads them:
//   - fixed: columns 1-6 are the sequence area (ignored), column 7 the
//     indicator (`*` or `/` comment line, `-` continuation, `D` debugging
//     line), code from column 8. `*>` in columns 7-8 is a comment too.
//   - free: the whole line is code; `*>` starts a comment anywhere outside
//     a literal.
// `>>SOURCE [FORMAT] [IS] FREE|FIXED` (and `$SET SOURCEFORMAT"FREE"`)
// switch format from the next line on. Without a directive the format is
// guessed: fixed (GnuCOBOL's default) unless some line has code in the
// sequence area that is not a sequence number (six characters of anything
// followed by an indicator are a sequence area: GnuCOBOL ignores them).
//
// A doc comment is a line whose comment text starts with `*>>` (rosetta's
// convention; see cobol_lsp.h). It is recognized the same way in both
// formats -- first non-blank `*>>`, or `*>>` at columns 7-9 behind a
// sequence number -- so a copybook in either format documents itself.

#include <string>
#include <vector>

enum class CobolFormat { Fixed, Free };

/** @brief The format of every line (directives switch it from the line after them on). */
std::vector<CobolFormat> CobolLineFormats(const std::vector<std::string> &lines);

/** @brief Where the parts of one line are, in byte columns. */
struct CobolLineInfo {
    bool comment_line = false;  // the whole line is a comment (fixed `*` / `/`, or nothing but `*>...`)
    bool continuation = false;  // fixed format `-` indicator
    bool directive = false;     // `>>...` / `$SET` compiler directive line
    int code_start = 0;         // first column of the code area
    int code_end = 0;           // one past the code (start of a `*>` comment, or the line's end)
    int comment_start = -1;     // where the comment starts, -1 for none
};

CobolLineInfo CobolClassifyLine(const std::string &line, CobolFormat format);

/** @brief Whether `line` is a `*>>` doc comment; `text` gets what follows the marker (one space dropped). */
bool CobolDocCommentText(const std::string &line, std::string *text);

/** @brief A COBOL reserved word (case-insensitive)? */
bool CobolIsKeyword(const std::string &word);
/** @brief The reserved words, upper case (for completion). */
const std::vector<std::string> &CobolKeywords();

/** @brief Whether `c` can be part of a COBOL user-defined word. */
inline bool CobolWordChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

/** @brief Upper-cases ASCII letters (COBOL names compare case-insensitively). */
std::string CobolUpper(const std::string &s);

#endif  // MEP_COBOL_SYNTAX_H
