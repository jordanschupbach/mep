// rust_lex.h -- mep's Rust lexer: raw tokens, literal "cooking", token
// gluing and the delimiter (token-tree) check, with byte spans.
//
// Ported from rustc_parse/rustc_lexer (rust-lang/rust 1.97.1, MIT OR
// Apache-2.0): rustc_lexer/src/lib.rs (the raw cursor),
// rustc_parse/src/lexer/mod.rs (cooking: keywords, literal validation,
// unescape errors, unknown prefixes, guarded strings), lexer/tokentrees.rs +
// lexer/diagnostics.rs (gluing `<` `=` into `<=`, the delimiter errors) and
// rustc-literal-escaper 0.0.7 (escape checks). Every comment is kept (kind
// and span) for the formatter.
//
// Positions are byte offsets into the caller's text. Unlike rustc the text
// is not normalised first: a leading BOM is skipped and "\r\n" counts as a
// newline everywhere rustc's CRLF->LF normalisation would make it one.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace rust {

/// Half-open byte range [lo, hi) into the source text.
struct Span {
    uint32_t lo = 0;
    uint32_t hi = 0;
    constexpr Span() = default;
    constexpr Span(uint32_t l, uint32_t h) : lo(l), hi(h) {}
    /// rustc's `Span::to`: from the start of the earlier to the end of the later.
    Span to(Span o) const { return Span(lo < o.lo ? lo : o.lo, hi > o.hi ? hi : o.hi); }
    Span shrink_to_lo() const { return Span(lo, lo); }
    Span shrink_to_hi() const { return Span(hi, hi); }
    /// rustc's `Span::until`: from our start to the start of `o`.
    Span until(Span o) const { return Span(lo, o.lo > lo ? o.lo : lo); }
    /// rustc's `Span::between`: from our end to the start of `o`.
    Span between(Span o) const { return Span(hi, o.lo > hi ? o.lo : hi); }
    Span with_lo(uint32_t l) const { return Span(l, hi); }
    Span with_hi(uint32_t h) const { return Span(lo, h); }
    bool is_empty() const { return lo == hi; }
    bool contains(Span o) const { return lo <= o.lo && o.hi <= hi; }
    bool operator==(const Span& o) const { return lo == o.lo && hi == o.hi; }
    bool operator!=(const Span& o) const { return !(*this == o); }
};

enum class Edition : uint8_t { E2015, E2018, E2021, E2024 };

/// Token kinds: rustc_ast::token::TokenKind without the macro-expansion-only
/// kinds (NtIdent, NtLifetime, invisible delimiters).
enum class Tok : uint8_t {
    // Expression-operator symbols.
    Eq, Lt, Le, EqEq, Ne, Ge, Gt, AndAnd, OrOr, Bang, Tilde, Plus, Minus, Star, Slash, Percent, Caret,
    And, Or, Shl, Shr, PlusEq, MinusEq, StarEq, SlashEq, PercentEq, CaretEq, AndEq, OrEq, ShlEq, ShrEq,
    // Structural symbols.
    At, Dot, DotDot, DotDotDot, DotDotEq, Comma, Semi, Colon, PathSep, RArrow, LArrow, FatArrow, Pound,
    Dollar, Question, SingleQuote,
    OpenParen, CloseParen, OpenBrace, CloseBrace, OpenBracket, CloseBracket,
    // Literals and names.
    Literal, Ident, Lifetime, DocComment,
    Eof,
};

enum class LitKind : uint8_t { Bool, Byte, Char, Integer, Float, Str, StrRaw, ByteStr, ByteStrRaw, CStr, CStrRaw, Err };

/// rustc_ast::tokenstream::Spacing of a token relative to the next one.
enum class Spacing : uint8_t { Alone, Joint, JointHidden };

enum class Delim : uint8_t { Paren, Brace, Bracket };

/// Identifiers with a meaning to the parser, in rustc_span's `kw` order so the
/// range predicates (`is_used_keyword` ...) carry over. `None` is any other name.
enum class Kw : uint8_t {
    None,
    // Special reserved identifiers.
    Underscore,
    // Used keywords (always).
    As, Break, Const, Continue, Crate, Else, Enum, Extern, False, Fn, For, If, Impl, In, Let, Loop, Match, Mod,
    Move, Mut, Pub, Ref, Return, SelfLower, SelfUpper, Static, Struct, Super, Trait, True, Type, Unsafe, Use,
    Where, While,
    // Unused keywords (always).
    Abstract, Become, Box, Do, Final, Macro, Override, Priv, Typeof, Unsized, Virtual, Yield,
    // Edition-dependent used keywords (2018+).
    Async, Await, Dyn,
    // Edition-dependent unused keywords: gen (2024+), try (2018+).
    Gen, Try,
    // Weak keywords.
    Auto, Builtin, Catch, ContractEnsures, ContractRequires, Default, MacroRules, Raw, Reuse, Safe, Union, Yeet,
    // Other symbols the parser compares names against (`sym::...`), not keywords.
    SymIs, SymPin, SymOffsetOf, SymFieldOf, SymNull, SymCfgSelect, SymUnsafeBinderCast, SymWrapBinder,
    SymUnwrapBinder, SymBikeshed,
};

const char* kw_str(Kw k);
/// The keyword id of `name` (raw-ness ignored), or Kw::None.
Kw kw_lookup(std::string_view name);

/// rustc's Symbol predicates on a keyword id, edition-aware.
bool kw_is_special(Kw k);
bool kw_is_used_keyword(Kw k, Edition e);
bool kw_is_unused_keyword(Kw k, Edition e);
bool kw_is_reserved(Kw k, Edition e);
bool kw_is_path_segment_keyword(Kw k);

enum class CommentKind : uint8_t { Line, Block };
enum class DocStyle : uint8_t { None, Outer, Inner };

struct Token {
    Tok kind = Tok::Question;
    /// Ident: the keyword id of the name (also set for raw idents; the
    /// keyword predicates check `raw` themselves). Lifetime: the id of the
    /// name after the `'` (`'static` -> Static, `'_` -> Underscore).
    Kw kw = Kw::None;
    bool raw = false;              // Ident `r#x`, Lifetime `'r#x`
    LitKind lit = LitKind::Err;    // Literal
    uint8_t n_hashes = 0;          // raw string literals
    DocStyle doc = DocStyle::None; // DocComment: Outer or Inner
    CommentKind doc_kind = CommentKind::Line;
    Spacing spacing = Spacing::Alone;
    bool synthetic = false;        // a close delimiter inserted by delimiter recovery
    Span span;
    /// Ident: the name (without `r#`). Lifetime: the name with its `'`.
    /// Literal: rustc's symbol (the contents between the quotes, or the
    /// whole text for numbers / errors). DocComment: the text after `//!`,
    /// `/**` ... Points into the source, or into LexResult::interned.
    std::string_view sym;
    /// Literal suffix (`u8` in `1u8`), empty if none.
    std::string_view suffix;

    bool is(Tok k) const { return kind == k; }
};

struct Comment {
    CommentKind kind = CommentKind::Line;
    DocStyle doc = DocStyle::None;
    Span span;        // the whole comment, `//` to end of line (exclusive of the newline) or `/*` to `*/`
};

struct LexDiag {
    std::string message;
    Span span;
};

struct LexOptions {
    Edition edition = Edition::E2021;
    /// Strip `#!...` and a `---` frontmatter block, as rustc does for a crate root.
    bool strip_shebang_frontmatter = true;
};

struct LexResult {
    /// Glued tokens, ending in Eof. With delimiter recovery on (the default),
    /// unmatched closing delimiters are dropped and missing ones inserted as
    /// zero-width `synthetic` tokens, so delimiters always balance.
    std::vector<Token> tokens;
    /// For each delimiter token, the index of its partner; UINT32_MAX otherwise.
    std::vector<uint32_t> partner;
    std::vector<Comment> comments;
    /// Errors in rustc's emission order: lexer errors, then the delimiter
    /// errors (which in rustc stop compilation before parsing).
    std::vector<LexDiag> errors;
    /// Errors rustc stashes or reports after parsing ("lifetimes cannot start
    /// with a number", emoji identifiers).
    std::vector<LexDiag> late_errors;
    /// A lexer error rustc treats as fatal (unterminated literal / comment) or
    /// a delimiter error: rustc never parses such a file.
    bool fatal = false;
    /// Index into `errors` of the first error that was fatal, or -1.
    int fatal_index = -1;
    Span shebang;      // empty if none
    Span frontmatter;  // empty if none
    /// Backing store for token symbols that are not substrings of the source
    /// (raw lifetimes `'r#a` -> `'a`).
    std::deque<std::string> interned;
};

/// Lex `src`. The returned views point into `src`, which must outlive the result.
LexResult lex(std::string_view src, const LexOptions& opts);

/// rustc_lexer predicates.
bool is_whitespace(uint32_t c);
bool is_id_start(uint32_t c);
bool is_id_continue(uint32_t c);
/// Decode one UTF-8 code point at `s[i]`; sets `len` (1 for invalid bytes).
uint32_t decode_utf8(std::string_view s, size_t i, size_t& len);

/// pprust::token_to_string for one token: the source text for names and
/// literals, the spelling for punctuation, "<eof>" for Eof.
std::string token_to_string(const Token& t, std::string_view src);
/// The spelling of a punctuation kind ("::", "=>", ...).
const char* tok_str(Tok k);
bool tok_is_punct(Tok k);

/// Line / column (1-based, columns in characters as rustc prints them).
struct LineCol {
    uint32_t line = 1;
    uint32_t col = 1;
};
class LineIndex {
public:
    explicit LineIndex(std::string_view src);
    LineCol at(uint32_t offset) const;

private:
    std::string_view src_;
    std::vector<uint32_t> starts_;
};

}  // namespace rust
