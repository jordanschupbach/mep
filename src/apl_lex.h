#ifndef MEP_APL_LEX_H
#define MEP_APL_LEX_H

// APL tokenizer shared by mep's APL language server (apl_lsp.cpp) and
// formatter (apl_format.cpp). Works on one line at a time (APL tokens never
// span lines); columns are byte offsets into the line.
//
// Handles GNU APL and Dyalog APL source: UTF-8 glyphs, names with ∆ ⍙ and
// ¯, ⎕names, numbers with ¯ / E / J, '...' strings ('' escapes) and GNU
// "..." strings, ⍝ comments (and a #! line), :Keywords, )commands and
// ]user commands at the start of a line, dotted names (ns.fn, #.a.b).

#include <string>
#include <vector>

namespace apl {

enum class Tok {
    Name,         // foo, ∆x
    QualName,     // ns.fn, #.a.b, ⎕SE.x
    SysName,      // ⎕IO, ⎕ (bare quad), ⍞ is Primitive
    Root,         // # or ##
    Number,       // 1, ¯2.5E3, 1J2
    String,       // 'it''s', "gnu"
    Comment,      // ⍝ ... (or a #! line)
    Primitive,    // + ⍴ ⍳ ...
    Operator,     // / ¨ ⍨ . ∘ ...
    Assign,       // ←
    Branch,       // →
    Diamond,      // ⋄ (Dyalog) or ◊ (GNU APL)
    LParen, RParen, LBracket, RBracket, LBrace, RBrace,
    Semicolon,    // ;
    Colon,        // :  (guard or label)
    DoubleColon,  // ::
    Del,          // ∇
    DelDel,       // ∇∇
    Lock,         // ⍫
    Special,      // ⍺ ⍵ ⍺⍺ ⍵⍵ λ χ
    Zilde,        // ⍬
    Keyword,      // :If :EndFor ...
    SysCommand,   // )LOAD ws, ]box on  (the whole rest of the line)
    Unknown,      // a character APL has no use for
    BadString,    // an unterminated string (to the end of the line)
};

struct Token {
    Tok kind = Tok::Unknown;
    int col = 0;      // byte offset of the first byte
    int end = 0;      // byte offset past the last byte
    std::string text;
};

/** @brief Tokenizes one line (no newline). Whitespace is dropped. */
std::vector<Token> LexLine(const std::string &line);

/** @brief Byte length of the UTF-8 sequence starting with @p c (1 for invalid bytes). */
int Utf8Len(unsigned char c);
/** @brief Whether @p s (one code point) can start an APL name. */
bool IsNameStart(const std::string &cp);
/** @brief Whether @p s (one code point) can continue an APL name. */
bool IsNameChar(const std::string &cp);

}  // namespace apl

#endif  // MEP_APL_LEX_H
