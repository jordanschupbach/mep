// rust_lex.cpp -- mep's Rust lexer (see rust_lex.h).
//
// Ported from rustc_parse/rustc_lexer (rust-lang/rust 1.97.1, MIT OR
// Apache-2.0) and rustc-literal-escaper 0.0.7 (MIT OR Apache-2.0).

#include "rust_lex.h"

#include <algorithm>
#include <cstring>

namespace rust {

namespace {

struct CpRange {
    uint32_t lo;
    uint32_t hi;
};
struct SubstEntry {
    uint32_t cp;
    uint8_t ascii;
};

#include "rust_lex_tables.inc"

const char* const kSubstAscii[] = {" ", "!", "\"", "&", "'", "(", ")", "*", "+", ",", "-", ".", "/",
                                   ":", ";", "<", "=", "==", ">", "?", "[", "\\", "]", "_", "{", "}"};

template <size_t N>
bool in_ranges(const CpRange (&table)[N], uint32_t c) {
    size_t lo = 0, hi = N;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (c < table[mid].lo)
            hi = mid;
        else if (c > table[mid].hi)
            lo = mid + 1;
        else
            return true;
    }
    return false;
}

// unicode-properties' Emoji property, approximated by its main blocks
// (only used to classify identifiers containing emoji as invalid).
bool is_emoji_char(uint32_t c) {
    static const CpRange kEmoji[] = {
        {0xA9, 0xA9},       {0xAE, 0xAE},       {0x203C, 0x203C},   {0x2049, 0x2049},   {0x2122, 0x2122},
        {0x2139, 0x2139},   {0x2194, 0x2199},   {0x21A9, 0x21AA},   {0x231A, 0x231B},   {0x2328, 0x2328},
        {0x23CF, 0x23CF},   {0x23E9, 0x23F3},   {0x23F8, 0x23FA},   {0x24C2, 0x24C2},   {0x25AA, 0x25AB},
        {0x25B6, 0x25B6},   {0x25C0, 0x25C0},   {0x25FB, 0x25FE},   {0x2600, 0x27BF},   {0x2934, 0x2935},
        {0x2B05, 0x2B07},   {0x2B1B, 0x2B1C},   {0x2B50, 0x2B50},   {0x2B55, 0x2B55},   {0x3030, 0x3030},
        {0x303D, 0x303D},   {0x3297, 0x3297},   {0x3299, 0x3299},   {0x1F000, 0x1FAFF},
    };
    return in_ranges(kEmoji, c);
}

constexpr uint32_t EOF_CHAR = 0;

}  // namespace

uint32_t decode_utf8(std::string_view s, size_t i, size_t& len) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
        len = 1;
        return c;
    }
    size_t n = 0;
    uint32_t cp = 0;
    if ((c & 0xE0) == 0xC0) {
        n = 2;
        cp = c & 0x1Fu;
    } else if ((c & 0xF0) == 0xE0) {
        n = 3;
        cp = c & 0x0Fu;
    } else if ((c & 0xF8) == 0xF0) {
        n = 4;
        cp = c & 0x07u;
    } else {
        len = 1;
        return 0xFFFD;
    }
    if (i + n > s.size()) {
        len = 1;
        return 0xFFFD;
    }
    for (size_t k = 1; k < n; ++k) {
        unsigned char cc = static_cast<unsigned char>(s[i + k]);
        if ((cc & 0xC0) != 0x80) {
            len = 1;
            return 0xFFFD;
        }
        cp = (cp << 6) | (cc & 0x3Fu);
    }
    len = n;
    return cp;
}

bool is_whitespace(uint32_t c) {
    switch (c) {
    case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x85: case 0x2028: case 0x2029: case 0x200E:
    case 0x200F: case 0x09: case 0x20:
        return true;
    default:
        return false;
    }
}

static bool is_horizontal_whitespace(uint32_t c) { return c == 0x09 || c == 0x20; }

bool is_id_start(uint32_t c) {
    if (c < 0x80) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    return in_ranges(kXidStart, c);
}

bool is_id_continue(uint32_t c) {
    if (c < 0x80) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (c >= '0' && c <= '9');
    return in_ranges(kXidContinue, c);
}

// ---------------------------------------------------------------------------
// Keywords.

namespace {
struct KwName {
    Kw kw;
    const char* name;
};
const KwName kKwNames[] = {
    {Kw::Underscore, "_"},
    {Kw::As, "as"}, {Kw::Break, "break"}, {Kw::Const, "const"}, {Kw::Continue, "continue"}, {Kw::Crate, "crate"},
    {Kw::Else, "else"}, {Kw::Enum, "enum"}, {Kw::Extern, "extern"}, {Kw::False, "false"}, {Kw::Fn, "fn"},
    {Kw::For, "for"}, {Kw::If, "if"}, {Kw::Impl, "impl"}, {Kw::In, "in"}, {Kw::Let, "let"}, {Kw::Loop, "loop"},
    {Kw::Match, "match"}, {Kw::Mod, "mod"}, {Kw::Move, "move"}, {Kw::Mut, "mut"}, {Kw::Pub, "pub"},
    {Kw::Ref, "ref"}, {Kw::Return, "return"}, {Kw::SelfLower, "self"}, {Kw::SelfUpper, "Self"},
    {Kw::Static, "static"}, {Kw::Struct, "struct"}, {Kw::Super, "super"}, {Kw::Trait, "trait"}, {Kw::True, "true"},
    {Kw::Type, "type"}, {Kw::Unsafe, "unsafe"}, {Kw::Use, "use"}, {Kw::Where, "where"}, {Kw::While, "while"},
    {Kw::Abstract, "abstract"}, {Kw::Become, "become"}, {Kw::Box, "box"}, {Kw::Do, "do"}, {Kw::Final, "final"},
    {Kw::Macro, "macro"}, {Kw::Override, "override"}, {Kw::Priv, "priv"}, {Kw::Typeof, "typeof"},
    {Kw::Unsized, "unsized"}, {Kw::Virtual, "virtual"}, {Kw::Yield, "yield"},
    {Kw::Async, "async"}, {Kw::Await, "await"}, {Kw::Dyn, "dyn"}, {Kw::Gen, "gen"}, {Kw::Try, "try"},
    {Kw::Auto, "auto"}, {Kw::Builtin, "builtin"}, {Kw::Catch, "catch"}, {Kw::ContractEnsures, "contract_ensures"},
    {Kw::ContractRequires, "contract_requires"}, {Kw::Default, "default"}, {Kw::MacroRules, "macro_rules"},
    {Kw::Raw, "raw"}, {Kw::Reuse, "reuse"}, {Kw::Safe, "safe"}, {Kw::Union, "union"}, {Kw::Yeet, "yeet"},
    {Kw::SymIs, "is"}, {Kw::SymPin, "pin"}, {Kw::SymOffsetOf, "offset_of"}, {Kw::SymFieldOf, "field_of"},
    {Kw::SymNull, "null"}, {Kw::SymCfgSelect, "cfg_select"}, {Kw::SymUnsafeBinderCast, "unsafe_binder_cast"},
    {Kw::SymWrapBinder, "wrap_binder"}, {Kw::SymUnwrapBinder, "unwrap_binder"}, {Kw::SymBikeshed, "bikeshed"},
};
}  // namespace

const char* kw_str(Kw k) {
    for (const KwName& n : kKwNames)
        if (n.kw == k) return n.name;
    return "";
}

Kw kw_lookup(std::string_view name) {
    if (name.empty() || name.size() > 18) return Kw::None;
    char c = name[0];
    if (!((c >= 'a' && c <= 'z') || c == 'S' || c == '_')) return Kw::None;
    for (const KwName& n : kKwNames)
        if (name == n.name) return n.kw;
    return Kw::None;
}

bool kw_is_special(Kw k) { return k == Kw::Underscore; }
static bool used_always(Kw k) { return k >= Kw::As && k <= Kw::While; }
static bool unused_always(Kw k) { return k >= Kw::Abstract && k <= Kw::Yield; }
static bool used_cond(Kw k, Edition e) { return k >= Kw::Async && k <= Kw::Dyn && e >= Edition::E2018; }
static bool unused_cond(Kw k, Edition e) {
    return (k == Kw::Gen && e >= Edition::E2024) || (k == Kw::Try && e >= Edition::E2018);
}
bool kw_is_used_keyword(Kw k, Edition e) { return used_always(k) || used_cond(k, e); }
bool kw_is_unused_keyword(Kw k, Edition e) { return unused_always(k) || unused_cond(k, e); }
bool kw_is_reserved(Kw k, Edition e) {
    return kw_is_special(k) || used_always(k) || unused_always(k) || used_cond(k, e) || unused_cond(k, e);
}
bool kw_is_path_segment_keyword(Kw k) {
    return k == Kw::Super || k == Kw::SelfLower || k == Kw::SelfUpper || k == Kw::Crate;
}

// ---------------------------------------------------------------------------
// Token spelling.

const char* tok_str(Tok k) {
    switch (k) {
    case Tok::Eq: return "=";
    case Tok::Lt: return "<";
    case Tok::Le: return "<=";
    case Tok::EqEq: return "==";
    case Tok::Ne: return "!=";
    case Tok::Ge: return ">=";
    case Tok::Gt: return ">";
    case Tok::AndAnd: return "&&";
    case Tok::OrOr: return "||";
    case Tok::Bang: return "!";
    case Tok::Tilde: return "~";
    case Tok::Plus: return "+";
    case Tok::Minus: return "-";
    case Tok::Star: return "*";
    case Tok::Slash: return "/";
    case Tok::Percent: return "%";
    case Tok::Caret: return "^";
    case Tok::And: return "&";
    case Tok::Or: return "|";
    case Tok::Shl: return "<<";
    case Tok::Shr: return ">>";
    case Tok::PlusEq: return "+=";
    case Tok::MinusEq: return "-=";
    case Tok::StarEq: return "*=";
    case Tok::SlashEq: return "/=";
    case Tok::PercentEq: return "%=";
    case Tok::CaretEq: return "^=";
    case Tok::AndEq: return "&=";
    case Tok::OrEq: return "|=";
    case Tok::ShlEq: return "<<=";
    case Tok::ShrEq: return ">>=";
    case Tok::At: return "@";
    case Tok::Dot: return ".";
    case Tok::DotDot: return "..";
    case Tok::DotDotDot: return "...";
    case Tok::DotDotEq: return "..=";
    case Tok::Comma: return ",";
    case Tok::Semi: return ";";
    case Tok::Colon: return ":";
    case Tok::PathSep: return "::";
    case Tok::RArrow: return "->";
    case Tok::LArrow: return "<-";
    case Tok::FatArrow: return "=>";
    case Tok::Pound: return "#";
    case Tok::Dollar: return "$";
    case Tok::Question: return "?";
    case Tok::SingleQuote: return "'";
    case Tok::OpenParen: return "(";
    case Tok::CloseParen: return ")";
    case Tok::OpenBrace: return "{";
    case Tok::CloseBrace: return "}";
    case Tok::OpenBracket: return "[";
    case Tok::CloseBracket: return "]";
    case Tok::Literal: return "<literal>";
    case Tok::Ident: return "<ident>";
    case Tok::Lifetime: return "<lifetime>";
    case Tok::DocComment: return "<doc comment>";
    case Tok::Eof: return "<eof>";
    }
    return "";
}

bool tok_is_punct(Tok k) { return k <= Tok::SingleQuote; }

std::string token_to_string(const Token& t, std::string_view src) {
    switch (t.kind) {
    case Tok::Literal:
    case Tok::Ident:
    case Tok::Lifetime:
    case Tok::DocComment:
        if (t.span.hi <= src.size()) {
            // pprust prints raw identifiers with their `r#` and doc comments
            // in full, which is exactly the source text.
            std::string s(src.substr(t.span.lo, t.span.hi - t.span.lo));
            if (t.kind == Tok::Ident && !t.raw) return std::string(t.sym);
            return s;
        }
        return std::string(t.sym);
    default:
        return tok_str(t.kind);
    }
}

// ---------------------------------------------------------------------------
// LineIndex.

LineIndex::LineIndex(std::string_view src) : src_(src) {
    starts_.push_back(0);
    for (size_t i = 0; i < src.size(); ++i)
        if (src[i] == '\n' && i + 1 < src.size()) starts_.push_back(static_cast<uint32_t>(i + 1));
}

LineCol LineIndex::at(uint32_t offset) const {
    if (offset > src_.size()) offset = static_cast<uint32_t>(src_.size());
    auto it = std::upper_bound(starts_.begin(), starts_.end(), offset);
    size_t line = static_cast<size_t>(it - starts_.begin());  // 1-based
    uint32_t start = starts_[line - 1];
    if (line == 1 && src_.size() >= 3 && src_.substr(0, 3) == "\xEF\xBB\xBF" && offset >= 3) start = 3;
    uint32_t col = 1;
    for (uint32_t i = start; i < offset;) {
        size_t len = 1;
        decode_utf8(src_, i, len);
        bool cr_lf = src_[i] == '\r' && i + 1 < src_.size() && src_[i + 1] == '\n';
        if (!cr_lf) ++col;
        i += static_cast<uint32_t>(len);
    }
    LineCol lc;
    lc.line = static_cast<uint32_t>(line);
    lc.col = col;
    return lc;
}

// ---------------------------------------------------------------------------
// rustc_lexer: the raw cursor.

namespace {

enum class RawKind : uint8_t {
    LineComment, BlockComment, Whitespace, Frontmatter, Ident, InvalidIdent, RawIdent, UnknownPrefix,
    UnknownPrefixLifetime, RawLifetime, GuardedStrPrefix, Literal, Lifetime,
    Semi, Comma, Dot, OpenParen, CloseParen, OpenBrace, CloseBrace, OpenBracket, CloseBracket, At, Pound, Tilde,
    Question, Colon, Dollar, Eq, Bang, Lt, Gt, Minus, And, Or, Plus, Star, Slash, Caret, Percent, Unknown, Eof,
};

enum class RawLit : uint8_t { Int, Float, Char, Byte, Str, ByteStr, CStr, RawStr, RawByteStr, RawCStr };
enum class Base : uint8_t { Binary = 2, Octal = 8, Decimal = 10, Hexadecimal = 16 };

struct RawToken {
    RawKind kind = RawKind::Eof;
    uint32_t len = 0;
    // LineComment / BlockComment
    DocStyle doc = DocStyle::None;
    bool terminated = true;
    // Frontmatter
    bool fm_invalid_ws = false;
    bool fm_invalid_infostring = false;
    // Literal
    RawLit lit = RawLit::Int;
    Base base = Base::Decimal;
    bool empty_int = false;
    bool empty_exponent = false;
    int n_hashes = -1;  // raw strings: -1 = error
    uint32_t suffix_start = 0;
    // Lifetime
    bool starts_with_number = false;
};

enum class RawStrErrKind { None, InvalidStarter, NoTerminator, TooManyDelimiters };
struct RawStrResult {
    RawStrErrKind err = RawStrErrKind::None;
    uint32_t n_hashes = 0;
    uint32_t bad_char = 0;
    uint32_t expected = 0, found = 0;
};

struct GuardedStr {
    uint32_t n_hashes = 0;
    bool terminated = false;
    uint32_t token_len = 0;
};

class Cursor {
public:
    Cursor(std::string_view src, size_t pos, bool frontmatter_allowed)
        : src_(src), pos_(pos), tok_start_(pos), frontmatter_allowed_(frontmatter_allowed) {}

    size_t pos() const { return pos_; }
    std::string_view rest() const { return src_.substr(pos_); }
    bool is_eof() const { return pos_ >= src_.size(); }
    uint32_t peek(size_t nth) const {
        size_t p = pos_;
        uint32_t c = EOF_CHAR;
        for (size_t k = 0; k <= nth; ++k) {
            if (p >= src_.size()) return EOF_CHAR;
            size_t len = 1;
            c = decode_utf8(src_, p, len);
            p += len;
        }
        return c;
    }
    uint32_t first() const {
        if (pos_ >= src_.size()) return EOF_CHAR;
        unsigned char b = static_cast<unsigned char>(src_[pos_]);
        if (b < 0x80) return b;
        return peek(0);
    }
    uint32_t second() const { return peek(1); }
    uint32_t third() const { return peek(2); }
    bool bump(uint32_t* out = nullptr) {
        if (pos_ >= src_.size()) return false;
        size_t len = 1;
        uint32_t c = decode_utf8(src_, pos_, len);
        pos_ += len;
        prev_ = c;
        if (out) *out = c;
        return true;
    }
    uint32_t prev() const { return prev_; }
    uint32_t pos_within_token() const { return static_cast<uint32_t>(pos_ - tok_start_); }
    void reset_pos_within_token() { tok_start_ = pos_; }
    template <class F>
    void eat_while(F pred) {
        while (!is_eof() && pred(first())) bump();
    }
    void eat_until(char byte) {
        size_t i = src_.find(byte, pos_);
        pos_ = i == std::string_view::npos ? src_.size() : i;
    }
    void bump_bytes(size_t n) { pos_ += n; }

    RawToken advance_token();
    GuardedStr guarded_double_quoted_string(bool& ok);
    RawStrResult raw_double_quoted_string(uint32_t prefix_len);

private:
    RawToken frontmatter(bool has_invalid_preceding_whitespace);
    RawToken line_comment();
    RawToken block_comment();
    RawToken ident_or_unknown_prefix();
    RawToken invalid_ident();
    RawToken c_or_byte_string(bool is_byte);
    void number(uint32_t first_digit, RawToken& t);
    RawToken lifetime_or_char();
    bool single_quoted_string();
    bool double_quoted_string();
    RawStrResult raw_string_unvalidated(uint32_t prefix_len);
    bool eat_decimal_digits();
    bool eat_hexadecimal_digits();
    bool eat_float_exponent();
    void eat_literal_suffix() { eat_identifier(); }
    void eat_identifier() {
        if (!is_id_start(first())) return;
        bump();
        eat_while(is_id_continue);
    }

    std::string_view src_;
    size_t pos_;
    size_t tok_start_;
    uint32_t prev_ = EOF_CHAR;

public:
    bool frontmatter_allowed_;
};

RawToken mk(RawKind k) {
    RawToken t;
    t.kind = k;
    return t;
}

RawToken Cursor::advance_token() {
    uint32_t c = 0;
    if (!bump(&c)) return mk(RawKind::Eof);
    RawToken t;
    if (frontmatter_allowed_ && is_whitespace(c)) {
        uint32_t last = c;
        while (is_whitespace(first())) {
            uint32_t cc = 0;
            if (!bump(&cc)) break;
            last = cc;
        }
        if (last != '\n' && rest().substr(0, 3) == "---") {
            bump();
            t = frontmatter(true);
        } else {
            t = mk(RawKind::Whitespace);
        }
    } else if (c == '-' && frontmatter_allowed_ && rest().substr(0, 2) == "--") {
        t = frontmatter(false);
    } else if (c == '/' && first() == '/') {
        t = line_comment();
    } else if (c == '/' && first() == '*') {
        t = block_comment();
    } else if (c == '/') {
        t = mk(RawKind::Slash);
    } else if (is_whitespace(c)) {
        eat_while(is_whitespace);
        t = mk(RawKind::Whitespace);
    } else if (c == 'r' && first() == '#' && is_id_start(second())) {
        bump();
        eat_identifier();
        t = mk(RawKind::RawIdent);
    } else if (c == 'r' && (first() == '#' || first() == '"')) {
        RawStrResult res = raw_double_quoted_string(1);
        t = mk(RawKind::Literal);
        t.lit = RawLit::RawStr;
        t.suffix_start = pos_within_token();
        if (res.err == RawStrErrKind::None) {
            eat_literal_suffix();
            t.n_hashes = static_cast<int>(res.n_hashes);
        }
    } else if (c == 'b') {
        t = c_or_byte_string(true);
    } else if (c == 'c') {
        t = c_or_byte_string(false);
    } else if (c >= '0' && c <= '9') {
        t = mk(RawKind::Literal);
        number(c, t);
        t.suffix_start = pos_within_token();
        eat_literal_suffix();
    } else if (is_id_start(c)) {
        t = ident_or_unknown_prefix();
    } else if (c == '#' && (first() == '"' || first() == '#')) {
        bump();
        t = mk(RawKind::GuardedStrPrefix);
    } else {
        switch (c) {
        case ';': t = mk(RawKind::Semi); break;
        case ',': t = mk(RawKind::Comma); break;
        case '.': t = mk(RawKind::Dot); break;
        case '(': t = mk(RawKind::OpenParen); break;
        case ')': t = mk(RawKind::CloseParen); break;
        case '{': t = mk(RawKind::OpenBrace); break;
        case '}': t = mk(RawKind::CloseBrace); break;
        case '[': t = mk(RawKind::OpenBracket); break;
        case ']': t = mk(RawKind::CloseBracket); break;
        case '@': t = mk(RawKind::At); break;
        case '#': t = mk(RawKind::Pound); break;
        case '~': t = mk(RawKind::Tilde); break;
        case '?': t = mk(RawKind::Question); break;
        case ':': t = mk(RawKind::Colon); break;
        case '$': t = mk(RawKind::Dollar); break;
        case '=': t = mk(RawKind::Eq); break;
        case '!': t = mk(RawKind::Bang); break;
        case '<': t = mk(RawKind::Lt); break;
        case '>': t = mk(RawKind::Gt); break;
        case '-': t = mk(RawKind::Minus); break;
        case '&': t = mk(RawKind::And); break;
        case '|': t = mk(RawKind::Or); break;
        case '+': t = mk(RawKind::Plus); break;
        case '*': t = mk(RawKind::Star); break;
        case '^': t = mk(RawKind::Caret); break;
        case '%': t = mk(RawKind::Percent); break;
        case '\'': t = lifetime_or_char(); break;
        case '"': {
            bool terminated = double_quoted_string();
            t = mk(RawKind::Literal);
            t.lit = RawLit::Str;
            t.terminated = terminated;
            t.suffix_start = pos_within_token();
            if (terminated) eat_literal_suffix();
            break;
        }
        default:
            if (c >= 0x80 && is_emoji_char(c))
                t = invalid_ident();
            else
                t = mk(RawKind::Unknown);
        }
    }
    if (frontmatter_allowed_ && t.kind != RawKind::Whitespace) frontmatter_allowed_ = false;
    t.len = pos_within_token();
    reset_pos_within_token();
    return t;
}

RawToken Cursor::frontmatter(bool has_invalid_preceding_whitespace) {
    uint32_t pos = pos_within_token();
    eat_while([](uint32_t c) { return c == '-'; });
    uint32_t length_opening = pos_within_token() - pos + 1;
    eat_while([](uint32_t ch) { return ch != '\n' && is_horizontal_whitespace(ch); });
    if (is_id_start(first())) {
        bump();
        eat_while([](uint32_t c) { return is_id_continue(c) || c == '-' || c == '.'; });
    }
    eat_while([](uint32_t ch) { return ch != '\n' && is_horizontal_whitespace(ch); });
    bool invalid_infostring = first() != '\n';
    bool found = false;
    std::string nl_fence = "\n" + std::string(length_opening, '-');
    size_t closing = rest().find(nl_fence);
    if (closing != std::string_view::npos) {
        bump_bytes(closing + nl_fence.size());
        eat_until('\n');
        found = true;
    }
    if (!found) {
        std::string_view r = rest();
        size_t potential = std::string_view::npos;
        size_t f = r.find("\n---");
        if (f != std::string_view::npos)
            potential = f + 1;
        else if ((f = r.find("\nuse ")) != std::string_view::npos)
            potential = f;
        else if ((f = r.find("\n//!")) != std::string_view::npos)
            potential = f;
        else if ((f = r.find("\n#![")) != std::string_view::npos)
            potential = f;
        if (potential == std::string_view::npos) {
            size_t base = 0;
            std::string_view rr = r;
            for (;;) {
                size_t cl = rr.find("---");
                if (cl == std::string_view::npos) break;
                size_t nl = rr.substr(0, cl).rfind('\n');
                size_t pstart = nl == std::string_view::npos ? 0 : nl + 1;
                bool all_ws = true;
                for (size_t k = pstart; k < cl; ++k)
                    if (rr[k] != ' ' && rr[k] != '\t') all_ws = false;
                if (all_ws) {
                    potential = cl + base;
                    break;
                }
                rr = rr.substr(cl + 3);
                base += cl + 3;
            }
        }
        if (potential != std::string_view::npos) {
            bump_bytes(potential);
            eat_until('\n');
        } else {
            pos_ = src_.size();
        }
    }
    RawToken t = mk(RawKind::Frontmatter);
    t.fm_invalid_ws = has_invalid_preceding_whitespace;
    t.fm_invalid_infostring = invalid_infostring;
    return t;
}

RawToken Cursor::line_comment() {
    bump();
    RawToken t = mk(RawKind::LineComment);
    if (first() == '!')
        t.doc = DocStyle::Inner;
    else if (first() == '/' && second() != '/')
        t.doc = DocStyle::Outer;
    eat_until('\n');
    return t;
}

RawToken Cursor::block_comment() {
    bump();
    RawToken t = mk(RawKind::BlockComment);
    if (first() == '!')
        t.doc = DocStyle::Inner;
    else if (first() == '*' && !(second() == '*' || second() == '/'))
        t.doc = DocStyle::Outer;
    size_t depth = 1;
    uint32_t c = 0;
    while (bump(&c)) {
        if (c == '/' && first() == '*') {
            bump();
            ++depth;
        } else if (c == '*' && first() == '/') {
            bump();
            --depth;
            if (depth == 0) break;
        }
    }
    t.terminated = depth == 0;
    return t;
}

RawToken Cursor::ident_or_unknown_prefix() {
    eat_while(is_id_continue);
    uint32_t c = first();
    if (c == '#' || c == '"' || c == '\'') return mk(RawKind::UnknownPrefix);
    if (c >= 0x80 && is_emoji_char(c)) return invalid_ident();
    return mk(RawKind::Ident);
}

RawToken Cursor::invalid_ident() {
    eat_while([](uint32_t c) { return is_id_continue(c) || (c >= 0x80 && is_emoji_char(c)) || c == 0x200D; });
    return mk(RawKind::InvalidIdent);
}

RawToken Cursor::c_or_byte_string(bool is_byte) {
    uint32_t f = first(), s = second();
    RawToken t = mk(RawKind::Literal);
    if (f == '\'' && is_byte) {
        bump();
        bool terminated = single_quoted_string();
        t.lit = RawLit::Byte;
        t.terminated = terminated;
        t.suffix_start = pos_within_token();
        if (terminated) eat_literal_suffix();
        return t;
    }
    if (f == '"') {
        bump();
        bool terminated = double_quoted_string();
        t.lit = is_byte ? RawLit::ByteStr : RawLit::CStr;
        t.terminated = terminated;
        t.suffix_start = pos_within_token();
        if (terminated) eat_literal_suffix();
        return t;
    }
    if (f == 'r' && (s == '"' || s == '#')) {
        bump();
        RawStrResult res = raw_double_quoted_string(2);
        t.lit = is_byte ? RawLit::RawByteStr : RawLit::RawCStr;
        t.suffix_start = pos_within_token();
        if (res.err == RawStrErrKind::None) {
            eat_literal_suffix();
            t.n_hashes = static_cast<int>(res.n_hashes);
        }
        return t;
    }
    return ident_or_unknown_prefix();
}

void Cursor::number(uint32_t first_digit, RawToken& t) {
    Base base = Base::Decimal;
    t.lit = RawLit::Int;
    if (first_digit == '0') {
        uint32_t f = first();
        if (f == 'b' || f == 'o' || f == 'x') {
            base = f == 'b' ? Base::Binary : f == 'o' ? Base::Octal : Base::Hexadecimal;
            bump();
            bool ok = base == Base::Hexadecimal ? eat_hexadecimal_digits() : eat_decimal_digits();
            if (!ok) {
                t.base = base;
                t.empty_int = true;
                return;
            }
        } else if ((f >= '0' && f <= '9') || f == '_') {
            eat_decimal_digits();
        } else if (f == '.' || f == 'e' || f == 'E') {
        } else {
            t.base = base;
            return;
        }
    } else {
        eat_decimal_digits();
    }
    t.base = base;
    uint32_t f = first();
    if (f == '.' && second() != '.' && !is_id_start(second())) {
        bump();
        bool empty_exponent = false;
        if (first() >= '0' && first() <= '9') {
            eat_decimal_digits();
            if (first() == 'e' || first() == 'E') {
                bump();
                empty_exponent = !eat_float_exponent();
            }
        }
        t.lit = RawLit::Float;
        t.empty_exponent = empty_exponent;
    } else if (f == 'e' || f == 'E') {
        bump();
        t.lit = RawLit::Float;
        t.empty_exponent = !eat_float_exponent();
    }
}

RawToken Cursor::lifetime_or_char() {
    bool can_be_a_lifetime;
    if (second() == '\'')
        can_be_a_lifetime = false;
    else
        can_be_a_lifetime = is_id_start(first()) || (first() >= '0' && first() <= '9');
    if (!can_be_a_lifetime) {
        bool terminated = single_quoted_string();
        RawToken t = mk(RawKind::Literal);
        t.lit = RawLit::Char;
        t.terminated = terminated;
        t.suffix_start = pos_within_token();
        if (terminated) eat_literal_suffix();
        return t;
    }
    if (first() == 'r' && second() == '#' && is_id_start(third())) {
        bump();
        bump();
        bump();
        eat_while(is_id_continue);
        return mk(RawKind::RawLifetime);
    }
    bool starts_with_number = first() >= '0' && first() <= '9';
    bump();
    eat_while(is_id_continue);
    if (first() == '\'') {
        bump();
        RawToken t = mk(RawKind::Literal);
        t.lit = RawLit::Char;
        t.terminated = true;
        t.suffix_start = pos_within_token();
        return t;
    }
    if (first() == '#' && !starts_with_number) return mk(RawKind::UnknownPrefixLifetime);
    RawToken t = mk(RawKind::Lifetime);
    t.starts_with_number = starts_with_number;
    return t;
}

bool Cursor::single_quoted_string() {
    if (second() == '\'' && first() != '\\') {
        bump();
        bump();
        return true;
    }
    for (;;) {
        uint32_t f = first();
        if (f == '\'') {
            bump();
            return true;
        } else if (f == '/') {
            break;
        } else if (f == '\n' && second() != '\'') {
            break;
        } else if (f == EOF_CHAR && is_eof()) {
            break;
        } else if (f == '\\') {
            bump();
            bump();
        } else {
            bump();
        }
    }
    return false;
}

bool Cursor::double_quoted_string() {
    uint32_t c = 0;
    while (bump(&c)) {
        if (c == '"') return true;
        if (c == '\\' && (first() == '\\' || first() == '"')) bump();
    }
    return false;
}

GuardedStr Cursor::guarded_double_quoted_string(bool& ok) {
    GuardedStr g;
    uint32_t n_start = 0;
    while (first() == '#') {
        ++n_start;
        bump();
    }
    if (first() != '"') {
        ok = false;
        return g;
    }
    ok = true;
    bump();
    bool terminated = double_quoted_string();
    if (!terminated) {
        g.n_hashes = n_start;
        g.terminated = false;
        g.token_len = pos_within_token();
        reset_pos_within_token();
        return g;
    }
    uint32_t n_end = 0;
    while (first() == '#' && n_end < n_start) {
        ++n_end;
        bump();
    }
    eat_literal_suffix();
    g.n_hashes = n_start;
    g.terminated = true;
    g.token_len = pos_within_token();
    reset_pos_within_token();
    return g;
}

RawStrResult Cursor::raw_double_quoted_string(uint32_t prefix_len) {
    RawStrResult r = raw_string_unvalidated(prefix_len);
    if (r.err == RawStrErrKind::None && r.n_hashes > 255) {
        r.err = RawStrErrKind::TooManyDelimiters;
        r.found = r.n_hashes;
    }
    return r;
}

RawStrResult Cursor::raw_string_unvalidated(uint32_t /*prefix_len*/) {
    RawStrResult r;
    uint32_t max_hashes = 0;
    uint32_t eaten = 0;
    while (first() == '#') {
        ++eaten;
        bump();
    }
    uint32_t n_start = eaten;
    uint32_t c = EOF_CHAR;
    if (!bump(&c) || c != '"') {
        r.err = RawStrErrKind::InvalidStarter;
        r.bad_char = c;
        return r;
    }
    for (;;) {
        eat_until('"');
        if (is_eof()) {
            r.err = RawStrErrKind::NoTerminator;
            r.expected = n_start;
            r.found = max_hashes;
            return r;
        }
        bump();
        uint32_t n_end = 0;
        while (first() == '#' && n_end < n_start) {
            ++n_end;
            bump();
        }
        if (n_end == n_start) {
            r.n_hashes = n_start;
            return r;
        }
        if (n_end > max_hashes) max_hashes = n_end;
    }
}

bool Cursor::eat_decimal_digits() {
    bool has = false;
    for (;;) {
        uint32_t f = first();
        if (f == '_') {
            bump();
        } else if (f >= '0' && f <= '9') {
            has = true;
            bump();
        } else {
            break;
        }
    }
    return has;
}

bool Cursor::eat_hexadecimal_digits() {
    bool has = false;
    for (;;) {
        uint32_t f = first();
        if (f == '_') {
            bump();
        } else if ((f >= '0' && f <= '9') || (f >= 'a' && f <= 'f') || (f >= 'A' && f <= 'F')) {
            has = true;
            bump();
        } else {
            break;
        }
    }
    return has;
}

bool Cursor::eat_float_exponent() {
    if (first() == '-' || first() == '+') bump();
    return eat_decimal_digits();
}

// ---------------------------------------------------------------------------
// rustc-literal-escaper: check_for_errors.

enum class Mode { Char, Byte, Str, RawStr, ByteStr, RawByteStr, CStr, RawCStr };

enum class EscErr {
    ZeroChars, MoreThanOneChar, LoneSlash, InvalidEscape, BareCarriageReturn, BareCarriageReturnInRawString,
    EscapeOnlyChar, TooShortHexEscape, InvalidCharInHexEscape, OutOfRangeHexEscape, NoBraceInUnicodeEscape,
    InvalidCharInUnicodeEscape, EmptyUnicodeEscape, UnclosedUnicodeEscape, LeadingUnderscoreUnicodeEscape,
    OverlongUnicodeEscape, LoneSurrogateUnicodeEscape, OutOfRangeUnicodeEscape, UnicodeEscapeInByte,
    NonAsciiCharInByte, NulInCStr, UnskippedWhitespaceWarning, MultipleSkippedLinesWarning,
};

// A char iterator over a string_view that knows CRLF normalisation: a "\r\n"
// pair reads as a single '\n'.
class Chars {
public:
    Chars(std::string_view s, size_t pos = 0) : s_(s), pos_(pos) {}
    bool next(uint32_t& c) {
        if (pos_ >= s_.size()) return false;
        if (s_[pos_] == '\r' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '\n') {
            pos_ += 2;
            c = '\n';
            return true;
        }
        size_t len = 1;
        c = decode_utf8(s_, pos_, len);
        pos_ += len;
        return true;
    }
    bool peek(uint32_t& c) const {
        Chars copy = *this;
        return copy.next(c);
    }
    size_t pos() const { return pos_; }
    void set_pos(size_t p) { pos_ = p; }
    std::string_view rest() const { return s_.substr(pos_); }

private:
    std::string_view s_;
    size_t pos_;
};

int hex_digit(uint32_t c) {
    if (c >= '0' && c <= '9') return static_cast<int>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<int>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<int>(c - 'A' + 10);
    return -1;
}

// Result of unescaping one unit: ok, or an error. `value` is the code point.
struct Unit {
    bool ok = true;
    EscErr err = EscErr::ZeroChars;
    uint32_t value = 0;
    bool high_byte = false;  // CStr: a \x80..\xff byte
};
Unit uerr(EscErr e) {
    Unit u;
    u.ok = false;
    u.err = e;
    return u;
}
Unit uok(uint32_t v) {
    Unit u;
    u.value = v;
    return u;
}

enum class UKind { Str, Bytes, CStr };

Unit char2unit(UKind k, uint32_t c) {
    switch (k) {
    case UKind::Str: return uok(c);
    case UKind::Bytes: return c < 0x80 ? uok(c) : uerr(EscErr::NonAsciiCharInByte);
    case UKind::CStr: return c == 0 ? uerr(EscErr::NulInCStr) : uok(c);
    }
    return uok(c);
}

bool hex_escape(Chars& chars, uint32_t& out, EscErr& err) {
    uint32_t hi = 0, lo = 0;
    if (!chars.next(hi)) {
        err = EscErr::TooShortHexEscape;
        return false;
    }
    int h = hex_digit(hi);
    if (h < 0) {
        err = EscErr::InvalidCharInHexEscape;
        return false;
    }
    if (!chars.next(lo)) {
        err = EscErr::TooShortHexEscape;
        return false;
    }
    int l = hex_digit(lo);
    if (l < 0) {
        err = EscErr::InvalidCharInHexEscape;
        return false;
    }
    out = static_cast<uint32_t>(h * 16 + l);
    return true;
}

bool unicode_escape(Chars& chars, uint32_t& out, EscErr& err) {
    uint32_t c = 0;
    if (!chars.next(c) || c != '{') {
        err = EscErr::NoBraceInUnicodeEscape;
        return false;
    }
    if (!chars.next(c)) {
        err = EscErr::UnclosedUnicodeEscape;
        return false;
    }
    if (c == '_') {
        err = EscErr::LeadingUnderscoreUnicodeEscape;
        return false;
    }
    if (c == '}') {
        err = EscErr::EmptyUnicodeEscape;
        return false;
    }
    int d = hex_digit(c);
    if (d < 0) {
        err = EscErr::InvalidCharInUnicodeEscape;
        return false;
    }
    uint32_t value = static_cast<uint32_t>(d);
    int n_digits = 1;
    for (;;) {
        if (!chars.next(c)) {
            err = EscErr::UnclosedUnicodeEscape;
            return false;
        }
        if (c == '_') continue;
        if (c == '}') {
            if (n_digits > 6) {
                err = EscErr::OverlongUnicodeEscape;
                return false;
            }
            out = value;
            return true;
        }
        int dd = hex_digit(c);
        if (dd < 0) {
            err = EscErr::InvalidCharInUnicodeEscape;
            return false;
        }
        ++n_digits;
        if (n_digits > 6) continue;
        value = value * 16 + static_cast<uint32_t>(dd);
    }
}

Unit unescape_1(UKind k, Chars& chars) {
    uint32_t c = 0;
    if (!chars.next(c)) return uerr(EscErr::LoneSlash);
    if (c == '0') return k == UKind::CStr ? uerr(EscErr::NulInCStr) : uok(0);
    switch (c) {
    case '"': case 'n': case 'r': case 't': case '\\': case '\'': return uok(c);
    case 'x': {
        uint32_t b = 0;
        EscErr e = EscErr::ZeroChars;
        if (!hex_escape(chars, b, e)) return uerr(e);
        if (k == UKind::Str) return b < 0x80 ? uok(b) : uerr(EscErr::OutOfRangeHexEscape);
        if (k == UKind::CStr && b == 0) return uerr(EscErr::NulInCStr);
        Unit u = uok(b);
        u.high_byte = b >= 0x80;
        return u;
    }
    case 'u': {
        uint32_t v = 0;
        EscErr e = EscErr::ZeroChars;
        if (!unicode_escape(chars, v, e)) {
            if (k == UKind::Bytes) return uerr(EscErr::UnicodeEscapeInByte);
            return uerr(e);
        }
        if (k == UKind::Bytes) return uerr(EscErr::UnicodeEscapeInByte);
        if (v > 0x10FFFF) return uerr(EscErr::OutOfRangeUnicodeEscape);
        if (v >= 0xD800 && v <= 0xDFFF) return uerr(EscErr::LoneSurrogateUnicodeEscape);
        return char2unit(k, v);
    }
    default: return uerr(EscErr::InvalidEscape);
    }
}

Unit unescape_single(UKind k, Chars& chars) {
    uint32_t c = 0;
    if (!chars.next(c)) return uerr(EscErr::ZeroChars);
    Unit res;
    if (c == '\\')
        res = unescape_1(k, chars);
    else if (c == '\n' || c == '\t' || c == '\'')
        res = uerr(EscErr::EscapeOnlyChar);
    else if (c == '\r')
        res = uerr(EscErr::BareCarriageReturn);
    else
        res = char2unit(k, c);
    if (!res.ok) return res;
    uint32_t extra = 0;
    if (chars.next(extra)) return uerr(EscErr::MoreThanOneChar);
    return res;
}

template <class F>
void unescape(UKind k, std::string_view src, F&& cb) {
    Chars chars(src);
    uint32_t c = 0;
    for (;;) {
        size_t start = chars.pos();
        if (!chars.next(c)) break;
        Unit res;
        if (c == '\\') {
            uint32_t nx = 0;
            if (chars.peek(nx) && nx == '\n') {
                chars.next(nx);
                // skip_ascii_whitespace: only warnings, which we drop.
                size_t p = chars.pos();
                while (p < src.size() && (src[p] == ' ' || src[p] == '\t' || src[p] == '\n' || src[p] == '\r')) ++p;
                chars.set_pos(p);
                continue;
            }
            res = unescape_1(k, chars);
        } else if (c == '"') {
            res = uerr(EscErr::EscapeOnlyChar);
        } else if (c == '\r') {
            res = uerr(EscErr::BareCarriageReturn);
        } else {
            res = char2unit(k, c);
        }
        if (!res.ok) cb(start, chars.pos(), res.err);
    }
}

template <class F>
void check_raw(UKind k, std::string_view src, F&& cb) {
    Chars chars(src);
    uint32_t c = 0;
    for (;;) {
        size_t start = chars.pos();
        if (!chars.next(c)) break;
        Unit res;
        if (c == '\r')
            res = uerr(EscErr::BareCarriageReturnInRawString);
        else
            res = char2unit(k, c);
        if (!res.ok) cb(start, chars.pos(), res.err);
    }
}

template <class F>
void check_for_errors(std::string_view src, Mode mode, F&& cb) {
    switch (mode) {
    case Mode::Char:
    case Mode::Byte: {
        Chars chars(src);
        Unit u = unescape_single(mode == Mode::Char ? UKind::Str : UKind::Bytes, chars);
        if (!u.ok) cb(size_t{0}, chars.pos(), u.err);
        break;
    }
    case Mode::Str: unescape(UKind::Str, src, cb); break;
    case Mode::ByteStr: unescape(UKind::Bytes, src, cb); break;
    case Mode::CStr: unescape(UKind::CStr, src, cb); break;
    case Mode::RawStr: check_raw(UKind::Str, src, cb); break;
    case Mode::RawByteStr: check_raw(UKind::Bytes, src, cb); break;
    case Mode::RawCStr: check_raw(UKind::CStr, src, cb); break;
    }
}

// rustc's escaped_char: printable ASCII as is, everything else escape_default.
std::string escaped_char(uint32_t c) {
    if (c >= 0x20 && c <= 0x7E) return std::string(1, static_cast<char>(c));
    switch (c) {
    case '\t': return "\\t";
    case '\r': return "\\r";
    case '\n': return "\\n";
    default: break;
    }
    char buf[16];
    snprintf(buf, sizeof buf, "\\u{%x}", c);
    return buf;
}

// ---------------------------------------------------------------------------
// The cooking lexer (rustc_parse::lexer::Lexer) producing raw-glue tokens.

struct PreToken {
    Token tok;
    bool preceded_by_ws = false;
};

class Lexer {
public:
    Lexer(std::string_view src, size_t start, const LexOptions& opts, LexResult& out)
        : src_(src), opts_(opts), out_(out), cursor_(src, start, false), pos_(static_cast<uint32_t>(start)) {}

    void set_frontmatter_allowed(bool b) { cursor_.frontmatter_allowed_ = b; }
    // next_token_from_cursor. Returns false on a fatal error.
    bool next(PreToken& out);

private:
    std::string_view str_from(uint32_t start) const { return src_.substr(start, pos_ - start); }
    std::string_view str_from_to(uint32_t a, uint32_t b) const { return src_.substr(a, b - a); }
    void err(std::string msg, Span sp) { out_.errors.push_back({std::move(msg), sp}); }
    void fatal(std::string msg, Span sp) {
        out_.fatal = true;
        if (out_.fatal_index < 0) out_.fatal_index = static_cast<int>(out_.errors.size());
        out_.errors.push_back({std::move(msg), sp});
    }
    Token ident_tok(uint32_t start) {
        Token t;
        t.kind = Tok::Ident;
        t.span = Span(start, pos_);
        t.sym = str_from(start);
        t.kw = kw_lookup(t.sym);
        return t;
    }
    void report_unknown_prefix(uint32_t start) {
        if (opts_.edition >= Edition::E2021) {
            std::string prefix(str_from(start));
            err("prefix `" + prefix + "` is unknown", Span(start, pos_));
        }
    }
    bool cook_literal(uint32_t start, uint32_t end, const RawToken& rt, Token& t);
    void cook_quoted(LitKind kind, Mode mode, uint32_t start, uint32_t end, uint32_t prefix_len,
                     uint32_t postfix_len, Token& t);
    void emit_unescape_error(std::string_view lit, uint32_t lit_start, Span full_lit_span, Mode mode, size_t r0,
                             size_t r1, EscErr e, bool& is_err);
    bool report_raw_str_error(uint32_t start, uint32_t prefix_len);
    void validate_frontmatter(uint32_t start, const RawToken& rt);

    std::string_view src_;
    const LexOptions& opts_;
    LexResult& out_;
    Cursor cursor_;
    uint32_t pos_;
    bool nbsp_is_whitespace_ = false;
    size_t swallow_next_invalid_ = 0;
};

bool Lexer::next(PreToken& out) {
    bool preceded_by_whitespace = false;
    for (;;) {
        size_t before = cursor_.pos();
        RawToken rt = cursor_.advance_token();
        uint32_t start = pos_;
        pos_ += rt.len;
        Token t;
        switch (rt.kind) {
        case RawKind::LineComment: {
            uint32_t hi = pos_;
            if (hi > start && src_[hi - 1] == '\r') --hi;  // CRLF: the \r is the newline's
            Comment c;
            c.kind = CommentKind::Line;
            c.doc = rt.doc;
            c.span = Span(start, hi);
            out_.comments.push_back(c);
            if (rt.doc == DocStyle::None) {
                preceded_by_whitespace = true;
                continue;
            }
            uint32_t content_start = start + 3;
            std::string_view content = str_from_to(content_start, hi);
            for (size_t i = 0; i < content.size(); ++i)
                if (content[i] == '\r')
                    err("bare CR not allowed in doc-comment",
                        Span(content_start + static_cast<uint32_t>(i), content_start + static_cast<uint32_t>(i) + 1));
            t.kind = Tok::DocComment;
            t.doc = rt.doc;
            t.doc_kind = CommentKind::Line;
            t.sym = content;
            t.span = Span(start, hi);
            break;
        }
        case RawKind::BlockComment: {
            Comment c;
            c.kind = CommentKind::Block;
            c.doc = rt.doc;
            c.span = Span(start, pos_);
            out_.comments.push_back(c);
            if (!rt.terminated) {
                fatal(rt.doc != DocStyle::None ? "unterminated block doc-comment" : "unterminated block comment",
                      Span(start, pos_));
                return false;
            }
            if (rt.doc == DocStyle::None) {
                preceded_by_whitespace = true;
                continue;
            }
            uint32_t content_start = start + 3;
            uint32_t content_end = pos_ - 2;
            std::string_view content = str_from_to(content_start, content_end);
            for (size_t i = 0; i < content.size(); ++i)
                if (content[i] == '\r' && !(i + 1 < content.size() && content[i + 1] == '\n'))
                    err("bare CR not allowed in block doc-comment",
                        Span(content_start + static_cast<uint32_t>(i), content_start + static_cast<uint32_t>(i) + 1));
            t.kind = Tok::DocComment;
            t.doc = rt.doc;
            t.doc_kind = CommentKind::Block;
            t.sym = content;
            t.span = Span(start, pos_);
            break;
        }
        case RawKind::Frontmatter:
            validate_frontmatter(start, rt);
            preceded_by_whitespace = true;
            continue;
        case RawKind::Whitespace:
            preceded_by_whitespace = true;
            continue;
        case RawKind::Ident:
            t = ident_tok(start);
            break;
        case RawKind::RawIdent: {
            t.kind = Tok::Ident;
            t.raw = true;
            t.span = Span(start, pos_);
            t.sym = str_from(start + 2);
            t.kw = kw_lookup(t.sym);
            if (t.sym == "_" || kw_is_path_segment_keyword(t.kw))
                err("`" + std::string(t.sym) + "` cannot be a raw identifier", t.span);
            break;
        }
        case RawKind::UnknownPrefix:
            report_unknown_prefix(start);
            t = ident_tok(start);
            break;
        case RawKind::UnknownPrefixLifetime:
            report_unknown_prefix(start);
            t.kind = Tok::Lifetime;
            t.span = Span(start, pos_);
            t.sym = str_from(start);
            t.kw = kw_lookup(t.sym.substr(1));
            break;
        case RawKind::InvalidIdent:
        case RawKind::Unknown: {
            bool single_in_array = false;
            if (rt.kind == RawKind::InvalidIdent) {
                std::string_view s = str_from(start);
                size_t len = 1;
                uint32_t c0 = decode_utf8(s, 0, len);
                if (len == s.size())
                    for (const SubstEntry& e : kSubst)
                        if (e.cp == c0) single_in_array = true;
                if (!single_in_array) {
                    t = ident_tok(start);
                    t.kw = Kw::None;
                    out_.late_errors.push_back({"identifiers cannot contain emoji: `" + std::string(s) + "`", t.span});
                    break;
                }
            }
            if (swallow_next_invalid_ > 0) {
                --swallow_next_invalid_;
                continue;
            }
            size_t len = 1;
            uint32_t c = decode_utf8(src_, start, len);
            if (c == 0xA0) {
                if (nbsp_is_whitespace_) {
                    preceded_by_whitespace = true;
                    continue;
                }
                nbsp_is_whitespace_ = true;
            }
            size_t repeats = 0;
            for (size_t p = start + len; p < src_.size();) {
                size_t l2 = 1;
                uint32_t c2 = decode_utf8(src_, p, l2);
                if (c2 != c) break;
                ++repeats;
                p += l2;
            }
            const SubstEntry* sub = nullptr;
            for (const SubstEntry& e : kSubst)
                if (e.cp == c) sub = &e;
            err("unknown start of token: " + escaped_char(c),
                Span(start, pos_ + static_cast<uint32_t>(repeats * len)));
            if (repeats > 0) swallow_next_invalid_ = repeats;
            Tok subst_tok = Tok::Eof;
            bool subst_ident = false;
            if (sub) {
                std::string_view a = kSubstAscii[sub->ascii];
                if (a == "_") subst_ident = true;
                else if (a == "-") subst_tok = Tok::Minus;
                else if (a == ",") subst_tok = Tok::Comma;
                else if (a == ";") subst_tok = Tok::Semi;
                else if (a == ":") subst_tok = Tok::Colon;
                else if (a == "!") subst_tok = Tok::Bang;
                else if (a == "?") subst_tok = Tok::Question;
                else if (a == ".") subst_tok = Tok::Dot;
                else if (a == "(") subst_tok = Tok::OpenParen;
                else if (a == ")") subst_tok = Tok::CloseParen;
                else if (a == "[") subst_tok = Tok::OpenBracket;
                else if (a == "]") subst_tok = Tok::CloseBracket;
                else if (a == "{") subst_tok = Tok::OpenBrace;
                else if (a == "}") subst_tok = Tok::CloseBrace;
                else if (a == "*") subst_tok = Tok::Star;
                else if (a == "/") subst_tok = Tok::Slash;
                else if (a == "&") subst_tok = Tok::And;
                else if (a == "+") subst_tok = Tok::Plus;
                else if (a == "<") subst_tok = Tok::Lt;
                else if (a == "=") subst_tok = Tok::Eq;
                else if (a == "==") subst_tok = Tok::EqEq;
                else if (a == ">") subst_tok = Tok::Gt;
            }
            if (subst_ident) {
                t.kind = Tok::Ident;
                t.kw = Kw::Underscore;
                t.sym = "_";
                t.span = Span(start, pos_);
                break;
            }
            if (subst_tok == Tok::Eof) {
                preceded_by_whitespace = true;
                continue;
            }
            t.kind = subst_tok;
            t.span = Span(start, pos_);
            break;
        }
        case RawKind::GuardedStrPrefix: {
            bool ed2024 = opts_.edition >= Edition::E2024;
            // rustc builds a cursor on str_before (at the first '#') and calls
            // guarded_double_quoted_string, which counts the leading hashes itself.
            Cursor c2(src_, before, false);
            bool ok = false;
            GuardedStr g = c2.guarded_double_quoted_string(ok);
            if (ok) {
                uint32_t end = start + g.token_len;
                if (ed2024) {
                    if (!g.terminated) {
                        uint32_t str_start = start + g.n_hashes;
                        cursor_ = Cursor(src_, end, false);
                        pos_ = end;
                        fatal("unterminated double quote string", Span(str_start, pos_));
                        return false;
                    }
                    cursor_ = Cursor(src_, end, false);
                    pos_ = end;
                    err("invalid string literal", Span(start, end));
                    t.kind = Tok::Literal;
                    t.lit = LitKind::Err;
                    t.span = Span(start, pos_);
                    t.sym = str_from(start);
                    break;
                }
            } else if (ed2024) {
                err("reserved multi-hash token is forbidden", Span(start, pos_));
                t.kind = Tok::Literal;
                t.lit = LitKind::Err;
                t.span = Span(start, pos_);
                t.sym = str_from(start);
                break;
            }
            // Before 2024: just a `#`.
            pos_ = start + 1;
            cursor_ = Cursor(src_, start + 1, false);
            t.kind = Tok::Pound;
            t.span = Span(start, pos_);
            break;
        }
        case RawKind::Literal: {
            if ((rt.lit == RawLit::CStr || rt.lit == RawLit::RawCStr) && opts_.edition < Edition::E2021) {
                uint32_t prefix_len = rt.lit == RawLit::CStr ? 1 : 2;
                uint32_t lit_start = start + prefix_len;
                pos_ = lit_start;
                cursor_ = Cursor(src_, lit_start, false);
                t = ident_tok(start);
                out.tok = t;
                out.preceded_by_ws = preceded_by_whitespace;
                return true;
            }
            uint32_t suffix_start = start + rt.suffix_start;
            if (!cook_literal(start, suffix_start, rt, t)) return false;
            if (suffix_start < pos_) {
                std::string_view s = str_from(suffix_start);
                if (s == "_")
                    err("underscore literal suffix is not allowed", Span(suffix_start, pos_));
                else
                    t.suffix = s;
            }
            t.kind = Tok::Literal;
            t.span = Span(start, pos_);
            break;
        }
        case RawKind::Lifetime: {
            t.kind = Tok::Lifetime;
            t.span = Span(start, pos_);
            t.sym = str_from(start);
            t.kw = kw_lookup(t.sym.substr(1));
            if (rt.starts_with_number) out_.late_errors.push_back({"lifetimes cannot start with a number", t.span});
            break;
        }
        case RawKind::RawLifetime: {
            uint32_t ident_start = start + 3;
            if (opts_.edition >= Edition::E2021) {
                if (cursor_.first() == '\'') err("character literal may only contain one codepoint", Span(start, pos_ + 1));
                std::string_view name = str_from(ident_start);
                if (name == "_" || kw_is_path_segment_keyword(kw_lookup(name)))
                    err("`" + std::string(name) + "` cannot be a raw lifetime", Span(start, pos_));
                out_.interned.push_back("'" + std::string(name));
                t.kind = Tok::Lifetime;
                t.raw = true;
                t.span = Span(start, pos_);
                t.sym = out_.interned.back();
                t.kw = kw_lookup(name);
            } else {
                uint32_t lt_start = start + 2;
                pos_ = lt_start;
                cursor_ = Cursor(src_, lt_start, false);
                t.kind = Tok::Lifetime;
                t.span = Span(start, pos_);
                t.sym = str_from(start);  // `'r`
                t.kw = kw_lookup(t.sym.substr(1));
            }
            break;
        }
        case RawKind::Semi: t.kind = Tok::Semi; break;
        case RawKind::Comma: t.kind = Tok::Comma; break;
        case RawKind::Dot: t.kind = Tok::Dot; break;
        case RawKind::OpenParen: t.kind = Tok::OpenParen; break;
        case RawKind::CloseParen: t.kind = Tok::CloseParen; break;
        case RawKind::OpenBrace: t.kind = Tok::OpenBrace; break;
        case RawKind::CloseBrace: t.kind = Tok::CloseBrace; break;
        case RawKind::OpenBracket: t.kind = Tok::OpenBracket; break;
        case RawKind::CloseBracket: t.kind = Tok::CloseBracket; break;
        case RawKind::At: t.kind = Tok::At; break;
        case RawKind::Pound: t.kind = Tok::Pound; break;
        case RawKind::Tilde: t.kind = Tok::Tilde; break;
        case RawKind::Question: t.kind = Tok::Question; break;
        case RawKind::Colon: t.kind = Tok::Colon; break;
        case RawKind::Dollar: t.kind = Tok::Dollar; break;
        case RawKind::Eq: t.kind = Tok::Eq; break;
        case RawKind::Bang: t.kind = Tok::Bang; break;
        case RawKind::Lt: t.kind = Tok::Lt; break;
        case RawKind::Gt: t.kind = Tok::Gt; break;
        case RawKind::Minus: t.kind = Tok::Minus; break;
        case RawKind::And: t.kind = Tok::And; break;
        case RawKind::Or: t.kind = Tok::Or; break;
        case RawKind::Plus: t.kind = Tok::Plus; break;
        case RawKind::Star: t.kind = Tok::Star; break;
        case RawKind::Slash: t.kind = Tok::Slash; break;
        case RawKind::Caret: t.kind = Tok::Caret; break;
        case RawKind::Percent: t.kind = Tok::Percent; break;
        case RawKind::Eof: t.kind = Tok::Eof; break;
        }
        if (t.span.hi == 0 && t.span.lo == 0) t.span = Span(start, pos_);
        out.tok = t;
        out.preceded_by_ws = preceded_by_whitespace;
        return true;
    }
}

void Lexer::validate_frontmatter(uint32_t start, const RawToken& rt) {
    std::string_view s = str_from(start);
    size_t real_start = s.find("---");
    if (real_start == std::string_view::npos) return;
    uint32_t opening_pos = start + static_cast<uint32_t>(real_start);
    std::string_view real_s = s.substr(real_start);
    size_t len_opening = 0;
    while (len_opening < real_s.size() && real_s[len_opening] == '-') ++len_opening;
    uint32_t opening_end = opening_pos + static_cast<uint32_t>(len_opening);
    if (out_.frontmatter.is_empty()) out_.frontmatter = Span(opening_pos, pos_);
    if (rt.fm_invalid_ws) {
        size_t nl = s.substr(0, real_start).rfind('\n');
        uint32_t line_start = start + (nl == std::string_view::npos ? 0 : static_cast<uint32_t>(nl + 1));
        err("invalid preceding whitespace for frontmatter opening", Span(line_start, opening_end));
    }
    size_t line_end = real_s.find('\n');
    if (line_end == std::string_view::npos) line_end = real_s.size();
    if (rt.fm_invalid_infostring)
        err("invalid infostring for frontmatter", Span(opening_end, opening_pos + static_cast<uint32_t>(line_end)));
    size_t last_nl = real_s.rfind('\n');
    size_t last_line_start = last_nl == std::string_view::npos ? line_end : last_nl + 1;
    std::string_view last_line = real_s.substr(last_line_start);
    size_t trim = 0;
    while (trim < last_line.size() && (last_line[trim] == ' ' || last_line[trim] == '\t')) ++trim;
    std::string_view last_trimmed = last_line.substr(trim);
    if (last_trimmed.substr(0, 3) != "---") {
        err("unclosed frontmatter", Span(opening_pos, pos_));
        return;
    }
    uint32_t last_line_start_pos = opening_pos + static_cast<uint32_t>(last_line_start);
    if (trim != 0)
        err("invalid preceding whitespace for frontmatter close",
            Span(last_line_start_pos, last_line_start_pos + static_cast<uint32_t>(last_line.size())));
    size_t len_close = 0;
    while (len_close < last_trimmed.size() && last_trimmed[len_close] == '-') ++len_close;
    if (len_close != len_opening) err("frontmatter close does not match the opening", Span(opening_pos, pos_));
    std::string_view rest = last_trimmed.substr(len_close);
    bool extra = false;
    for (char ch : rest)
        if (ch != ' ' && ch != '\t' && ch != '\r') extra = true;
    if (extra) err("extra characters after frontmatter close are not allowed", Span(last_line_start_pos, pos_));
}

bool Lexer::report_raw_str_error(uint32_t start, uint32_t prefix_len) {
    Cursor c(src_, start, false);
    for (uint32_t i = 0; i < prefix_len; ++i) c.bump();
    RawStrResult r = c.raw_double_quoted_string(prefix_len);
    switch (r.err) {
    case RawStrErrKind::InvalidStarter:
        fatal("found invalid character; only `#` is allowed in raw string delimitation: " + escaped_char(r.bad_char),
              Span(start, pos_));
        break;
    case RawStrErrKind::NoTerminator:
        fatal("unterminated raw string", Span(start, start));
        break;
    case RawStrErrKind::TooManyDelimiters:
        fatal("too many `#` symbols: raw strings may be delimited by up to 255 `#` symbols, but found " +
                  std::to_string(r.found),
              Span(start, pos_));
        break;
    case RawStrErrKind::None:
        fatal("unterminated raw string", Span(start, start));
        break;
    }
    return false;
}

bool Lexer::cook_literal(uint32_t start, uint32_t end, const RawToken& rt, Token& t) {
    switch (rt.lit) {
    case RawLit::Char:
        if (!rt.terminated) {
            fatal("unterminated character literal", Span(start, end));
            return false;
        }
        cook_quoted(LitKind::Char, Mode::Char, start, end, 1, 1, t);
        return true;
    case RawLit::Byte:
        if (!rt.terminated) {
            fatal("unterminated byte constant", Span(start + 1, end));
            return false;
        }
        cook_quoted(LitKind::Byte, Mode::Byte, start, end, 2, 1, t);
        return true;
    case RawLit::Str:
        if (!rt.terminated) {
            fatal("unterminated double quote string", Span(start, end));
            return false;
        }
        cook_quoted(LitKind::Str, Mode::Str, start, end, 1, 1, t);
        return true;
    case RawLit::ByteStr:
        if (!rt.terminated) {
            fatal("unterminated double quote byte string", Span(start + 1, end));
            return false;
        }
        cook_quoted(LitKind::ByteStr, Mode::ByteStr, start, end, 2, 1, t);
        return true;
    case RawLit::CStr:
        if (!rt.terminated) {
            fatal("unterminated C string", Span(start + 1, end));
            return false;
        }
        cook_quoted(LitKind::CStr, Mode::CStr, start, end, 2, 1, t);
        return true;
    case RawLit::RawStr:
    case RawLit::RawByteStr:
    case RawLit::RawCStr: {
        uint32_t pre = rt.lit == RawLit::RawStr ? 1 : 2;
        if (rt.n_hashes < 0) return report_raw_str_error(start, pre);
        uint32_t n = static_cast<uint32_t>(rt.n_hashes);
        t.n_hashes = static_cast<uint8_t>(n);
        LitKind k = rt.lit == RawLit::RawStr ? LitKind::StrRaw : rt.lit == RawLit::RawByteStr ? LitKind::ByteStrRaw : LitKind::CStrRaw;
        Mode m = rt.lit == RawLit::RawStr ? Mode::RawStr : rt.lit == RawLit::RawByteStr ? Mode::RawByteStr : Mode::RawCStr;
        cook_quoted(k, m, start, end, pre + 1 + n, 1 + n, t);
        return true;
    }
    case RawLit::Int: {
        t.lit = LitKind::Integer;
        if (rt.empty_int) {
            err("no valid digits found for number", Span(start, end));
            t.lit = LitKind::Err;
        } else if (rt.base == Base::Binary || rt.base == Base::Octal) {
            uint32_t base = static_cast<uint32_t>(rt.base);
            std::string_view s = str_from_to(start + 2, end);
            for (size_t i = 0; i < s.size();) {
                size_t len = 1;
                uint32_t c = decode_utf8(s, i, len);
                bool ok = c == '_' || (c >= '0' && c < '0' + (base < 10 ? base : 10));
                if (!ok) {
                    err("invalid digit for a base " + std::to_string(base) + " literal",
                        Span(start + 2 + static_cast<uint32_t>(i), start + 2 + static_cast<uint32_t>(i + len)));
                    t.lit = LitKind::Err;
                }
                i += len;
            }
        }
        t.sym = str_from_to(start, end);
        return true;
    }
    case RawLit::Float: {
        t.lit = LitKind::Float;
        if (rt.empty_exponent) {
            err("expected at least one digit in exponent", Span(start, pos_));
            t.lit = LitKind::Err;
        }
        const char* base = rt.base == Base::Hexadecimal ? "hexadecimal"
                           : rt.base == Base::Octal     ? "octal"
                           : rt.base == Base::Binary    ? "binary"
                                                        : nullptr;
        if (base) {
            err(std::string(base) + " float literal is not supported", Span(start, end));
            t.lit = LitKind::Err;
        }
        t.sym = str_from_to(start, end);
        return true;
    }
    }
    return true;
}

void Lexer::cook_quoted(LitKind kind, Mode mode, uint32_t start, uint32_t end, uint32_t prefix_len,
                        uint32_t postfix_len, Token& t) {
    uint32_t content_start = start + prefix_len;
    uint32_t content_end = end - postfix_len;
    std::string_view lit = str_from_to(content_start, content_end);
    bool is_err = false;
    check_for_errors(lit, mode, [&](size_t r0, size_t r1, EscErr e) {
        emit_unescape_error(lit, content_start, Span(start, end), mode, r0, r1, e, is_err);
    });
    t.lit = is_err ? LitKind::Err : kind;
    t.sym = is_err ? str_from_to(start, end) : lit;
}

void Lexer::emit_unescape_error(std::string_view lit, uint32_t lit_start, Span full_lit_span, Mode mode, size_t r0,
                                size_t r1, EscErr e, bool& is_err) {
    Span err_span(lit_start + static_cast<uint32_t>(r0), lit_start + static_cast<uint32_t>(r1));
    auto last_char = [&](uint32_t& c) {
        // the last char of lit[r0..r1]
        size_t p = r1;
        while (p > r0 && (static_cast<unsigned char>(lit[p - 1]) & 0xC0) == 0x80) --p;
        if (p > r0) --p;
        while (p > r0 && (static_cast<unsigned char>(lit[p]) & 0xC0) == 0x80) --p;
        size_t len = 1;
        c = decode_utf8(lit, p, len);
        return Span(lit_start + static_cast<uint32_t>(p), lit_start + static_cast<uint32_t>(r1));
    };
    bool in_dq = !(mode == Mode::Char || mode == Mode::Byte);
    is_err = true;
    uint32_t c = 0;
    switch (e) {
    case EscErr::LoneSurrogateUnicodeEscape:
    case EscErr::OutOfRangeUnicodeEscape: err("invalid unicode character escape", err_span); break;
    case EscErr::MoreThanOneChar: err("character literal may only contain one codepoint", full_lit_span); break;
    case EscErr::EscapeOnlyChar: {
        last_char(c);
        err(std::string(mode == Mode::Byte ? "byte" : "character") + " constant must be escaped: `" +
                escaped_char(c) + "`",
            err_span);
        break;
    }
    case EscErr::BareCarriageReturn:
        err(in_dq ? "bare CR not allowed in string, use `\\r` instead" : "character constant must be escaped: `\\r`",
            err_span);
        break;
    case EscErr::BareCarriageReturnInRawString: err("bare CR not allowed in raw string", err_span); break;
    case EscErr::InvalidEscape: {
        Span sp = last_char(c);
        bool byte = mode == Mode::Byte || mode == Mode::ByteStr;
        err(std::string(byte ? "unknown byte escape" : "unknown character escape") + ": `" + escaped_char(c) + "`",
            sp);
        break;
    }
    case EscErr::TooShortHexEscape: err("numeric character escape is too short", err_span); break;
    case EscErr::InvalidCharInHexEscape:
    case EscErr::InvalidCharInUnicodeEscape: {
        Span sp = last_char(c);
        err(std::string("invalid character in ") +
                (e == EscErr::InvalidCharInHexEscape ? "numeric character" : "unicode") + " escape: `" +
                escaped_char(c) + "`",
            sp);
        break;
    }
    case EscErr::NonAsciiCharInByte: {
        Span sp = last_char(c);
        const char* desc = mode == Mode::Byte ? "byte literal" : mode == Mode::ByteStr ? "byte string literal" : "raw byte string literal";
        err(std::string("non-ASCII character in ") + desc, sp);
        break;
    }
    case EscErr::OutOfRangeHexEscape: err("out of range hex escape", err_span); break;
    case EscErr::LeadingUnderscoreUnicodeEscape: {
        Span sp = last_char(c);
        err("invalid start of unicode escape: `_`", sp);
        break;
    }
    case EscErr::OverlongUnicodeEscape: err("overlong unicode escape", err_span); break;
    case EscErr::UnclosedUnicodeEscape: err("unterminated unicode escape", err_span); break;
    case EscErr::NoBraceInUnicodeEscape: err("incorrect unicode escape sequence", err_span); break;
    case EscErr::UnicodeEscapeInByte: err("unicode escape in byte string", err_span); break;
    case EscErr::EmptyUnicodeEscape: err("empty unicode escape", err_span); break;
    case EscErr::ZeroChars: err("empty character literal", err_span); break;
    case EscErr::LoneSlash: err("invalid trailing slash in literal", err_span); break;
    case EscErr::NulInCStr: err("null characters in C string literals are not supported", err_span); break;
    case EscErr::UnskippedWhitespaceWarning:
    case EscErr::MultipleSkippedLinesWarning: is_err = false; break;
    }
}

// Token::glue.
bool glue(Tok a, Tok b, Tok& out) {
    switch (a) {
    case Tok::Eq:
        if (b == Tok::Eq) { out = Tok::EqEq; return true; }
        if (b == Tok::Gt) { out = Tok::FatArrow; return true; }
        return false;
    case Tok::Lt:
        if (b == Tok::Eq) { out = Tok::Le; return true; }
        if (b == Tok::Lt) { out = Tok::Shl; return true; }
        if (b == Tok::Le) { out = Tok::ShlEq; return true; }
        if (b == Tok::Minus) { out = Tok::LArrow; return true; }
        return false;
    case Tok::Gt:
        if (b == Tok::Eq) { out = Tok::Ge; return true; }
        if (b == Tok::Gt) { out = Tok::Shr; return true; }
        if (b == Tok::Ge) { out = Tok::ShrEq; return true; }
        return false;
    case Tok::Bang: if (b == Tok::Eq) { out = Tok::Ne; return true; } return false;
    case Tok::Plus: if (b == Tok::Eq) { out = Tok::PlusEq; return true; } return false;
    case Tok::Minus:
        if (b == Tok::Eq) { out = Tok::MinusEq; return true; }
        if (b == Tok::Gt) { out = Tok::RArrow; return true; }
        return false;
    case Tok::Star: if (b == Tok::Eq) { out = Tok::StarEq; return true; } return false;
    case Tok::Slash: if (b == Tok::Eq) { out = Tok::SlashEq; return true; } return false;
    case Tok::Percent: if (b == Tok::Eq) { out = Tok::PercentEq; return true; } return false;
    case Tok::Caret: if (b == Tok::Eq) { out = Tok::CaretEq; return true; } return false;
    case Tok::And:
        if (b == Tok::Eq) { out = Tok::AndEq; return true; }
        if (b == Tok::And) { out = Tok::AndAnd; return true; }
        return false;
    case Tok::Or:
        if (b == Tok::Eq) { out = Tok::OrEq; return true; }
        if (b == Tok::Or) { out = Tok::OrOr; return true; }
        return false;
    case Tok::Shl: if (b == Tok::Eq) { out = Tok::ShlEq; return true; } return false;
    case Tok::Shr: if (b == Tok::Eq) { out = Tok::ShrEq; return true; } return false;
    case Tok::Dot:
        if (b == Tok::Dot) { out = Tok::DotDot; return true; }
        if (b == Tok::DotDot) { out = Tok::DotDotDot; return true; }
        return false;
    case Tok::DotDot:
        if (b == Tok::Dot) { out = Tok::DotDotDot; return true; }
        if (b == Tok::Eq) { out = Tok::DotDotEq; return true; }
        return false;
    case Tok::Colon: if (b == Tok::Colon) { out = Tok::PathSep; return true; } return false;
    default: return false;
    }
}

bool is_open(Tok k) { return k == Tok::OpenParen || k == Tok::OpenBrace || k == Tok::OpenBracket; }
bool is_close(Tok k) { return k == Tok::CloseParen || k == Tok::CloseBrace || k == Tok::CloseBracket; }
Delim delim_of(Tok k) {
    switch (k) {
    case Tok::OpenParen: case Tok::CloseParen: return Delim::Paren;
    case Tok::OpenBracket: case Tok::CloseBracket: return Delim::Bracket;
    default: return Delim::Brace;
    }
}
Tok close_of(Delim d) { return d == Delim::Paren ? Tok::CloseParen : d == Delim::Bracket ? Tok::CloseBracket : Tok::CloseBrace; }
const char* close_str(Delim d) { return d == Delim::Paren ? ")" : d == Delim::Bracket ? "]" : "}"; }

struct Unmatched {
    bool found_is_close = false;  // false: Eof
    Delim found_delim = Delim::Paren;
    Span found_span;
    bool has_unclosed = false;
    Span unclosed_span;
};

// lexer/tokentrees.rs, iteratively: the delimiter errors in rustc's order.
void check_delims(const std::vector<Token>& toks, std::string_view /*src*/, LexResult& out) {
    std::vector<std::pair<Delim, Span>> open;  // diag_info.open_delimiters
    std::vector<Unmatched> unmatched;
    bool have_last_unclosed = false;
    Span last_unclosed_found_span;
    std::vector<size_t> frames;  // open_delimiters depth per frame (one per entered delimiter)
    bool has_final = false;
    std::string final_msg;
    Span final_span;
    size_t i = 0;
    for (;;) {
        const Token& t = toks[i];
        if (is_open(t.kind)) {
            open.push_back({delim_of(t.kind), t.span});
            frames.push_back(open.size());
            ++i;
            continue;
        }
        if (is_close(t.kind)) {
            if (frames.empty()) {
                // close_delim_err: report_missing_open_delim drops the
                // paren/bracket mismatches.
                std::vector<Unmatched> kept;
                for (const Unmatched& u : unmatched)
                    if (!(u.found_is_close && (u.found_delim == Delim::Paren || u.found_delim == Delim::Bracket)))
                        kept.push_back(u);
                unmatched.swap(kept);
                has_final = true;
                final_msg = std::string("unexpected closing delimiter: `") + tok_str(t.kind) + "`";
                final_span = t.span;
                break;
            }
            Delim cd = delim_of(t.kind);
            Delim od = open.back().first;
            frames.pop_back();
            if (cd == od) {
                open.pop_back();
                ++i;
                continue;
            }
            if (!have_last_unclosed || last_unclosed_found_span != t.span) {
                have_last_unclosed = true;
                last_unclosed_found_span = t.span;
                Unmatched u;
                u.found_is_close = true;
                u.found_delim = cd;
                u.found_span = t.span;
                if (!open.empty()) {
                    u.has_unclosed = true;
                    u.unclosed_span = open.back().second;
                }
                open.pop_back();
                unmatched.push_back(u);
            } else {
                open.pop_back();
            }
            bool any = false;
            for (auto& o : open)
                if (o.first == cd) any = true;
            if (!any) ++i;
            continue;
        }
        if (t.kind == Tok::Eof) {
            if (!frames.empty()) {
                has_final = true;
                final_msg = "this file contains an unclosed delimiter";
                final_span = t.span;
            }
            break;
        }
        ++i;
    }
    for (const Unmatched& u : unmatched) {
        if (!u.found_is_close) continue;
        Span primary = u.found_span;
        if (u.has_unclosed && u.unclosed_span.lo < primary.lo) primary = u.unclosed_span;
        out.errors.push_back({std::string("mismatched closing delimiter: `") + close_str(u.found_delim) + "`", primary});
        out.fatal = true;
        if (out.fatal_index < 0) out.fatal_index = static_cast<int>(out.errors.size()) - 1;
    }
    if (has_final) {
        out.errors.push_back({final_msg, final_span});
        out.fatal = true;
        if (out.fatal_index < 0) out.fatal_index = static_cast<int>(out.errors.size()) - 1;
    }
}

// Make the delimiters balance (for parsing after a delimiter error) and fill
// in `partner`.
void balance_delims(std::vector<Token>& toks, std::vector<uint32_t>& partner) {
    std::vector<Token> outv;
    outv.reserve(toks.size() + 8);
    std::vector<size_t> stack;  // indexes into outv
    auto synth_close = [&](size_t open_idx, Span at) {
        Token c;
        c.kind = close_of(delim_of(outv[open_idx].kind));
        c.span = Span(at.lo, at.lo);
        c.synthetic = true;
        outv.push_back(c);
    };
    for (size_t i = 0; i < toks.size(); ++i) {
        const Token& t = toks[i];
        if (is_open(t.kind)) {
            stack.push_back(outv.size());
            outv.push_back(t);
        } else if (is_close(t.kind)) {
            Delim d = delim_of(t.kind);
            size_t found = SIZE_MAX;
            for (size_t k = stack.size(); k-- > 0;)
                if (delim_of(outv[stack[k]].kind) == d) {
                    found = k;
                    break;
                }
            if (found == SIZE_MAX) continue;  // unmatched closer: drop it
            while (stack.size() > found + 1) {
                synth_close(stack.back(), t.span);
                stack.pop_back();
            }
            stack.pop_back();
            outv.push_back(t);
        } else if (t.kind == Tok::Eof) {
            while (!stack.empty()) {
                synth_close(stack.back(), t.span);
                stack.pop_back();
            }
            outv.push_back(t);
        } else {
            outv.push_back(t);
        }
    }
    toks.swap(outv);
    partner.assign(toks.size(), UINT32_MAX);
    std::vector<uint32_t> st;
    for (uint32_t i = 0; i < toks.size(); ++i) {
        if (is_open(toks[i].kind)) {
            st.push_back(i);
        } else if (is_close(toks[i].kind) && !st.empty()) {
            partner[i] = st.back();
            partner[st.back()] = i;
            st.pop_back();
        }
    }
}

// rustc_lexer::strip_shebang
size_t strip_shebang(std::string_view src) {
    if (src.substr(0, 2) != "#!") return 0;
    Cursor c(src, 2, false);
    for (;;) {
        RawToken t = c.advance_token();
        if (t.kind == RawKind::Whitespace) continue;
        if (t.kind == RawKind::LineComment && t.doc == DocStyle::None) continue;
        if (t.kind == RawKind::BlockComment && t.doc == DocStyle::None) continue;
        if (t.kind == RawKind::OpenBracket) return 0;
        break;
    }
    size_t nl = src.find('\n');
    return nl == std::string_view::npos ? src.size() : nl;
}

}  // namespace

LexResult lex(std::string_view src, const LexOptions& opts) {
    LexResult out;
    size_t start = 0;
    if (src.size() >= 3 && src.substr(0, 3) == "\xEF\xBB\xBF") start = 3;
    if (opts.strip_shebang_frontmatter) {
        size_t n = strip_shebang(src.substr(start));
        if (n > 0) {
            out.shebang = Span(static_cast<uint32_t>(start), static_cast<uint32_t>(start + n));
            start += n;
        }
    }
    Lexer lx(src, start, opts, out);
    lx.set_frontmatter_allowed(opts.strip_shebang_frontmatter);
    std::vector<PreToken> pre;
    pre.reserve(src.size() / 4 + 16);
    bool ok = true;
    for (;;) {
        PreToken p;
        if (!lx.next(p)) {
            ok = false;
            break;
        }
        pre.push_back(p);
        if (p.tok.kind == Tok::Eof) break;
    }
    if (!ok) {
        // A fatal lexer error: rustc stops here. Still hand back what was lexed
        // (with an Eof at the error) so a caller can show something.
        PreToken e;
        e.tok.kind = Tok::Eof;
        uint32_t end = static_cast<uint32_t>(src.size());
        e.tok.span = Span(end, end);
        e.preceded_by_ws = true;
        pre.push_back(e);
    }
    // Glue adjacent punctuation and compute spacing.
    std::vector<Token>& toks = out.tokens;
    toks.reserve(pre.size());
    for (size_t i = 0; i < pre.size(); ++i) {
        Token t = pre[i].tok;
        while (i + 1 < pre.size() && !pre[i + 1].preceded_by_ws) {
            Tok g;
            if (!glue(t.kind, pre[i + 1].tok.kind, g)) break;
            t.kind = g;
            t.span = t.span.to(pre[i + 1].tok.span);
            ++i;
        }
        if (i + 1 < pre.size()) {
            const PreToken& nx = pre[i + 1];
            if (nx.preceded_by_ws)
                t.spacing = Spacing::Alone;
            else if (tok_is_punct(nx.tok.kind))
                t.spacing = Spacing::Joint;
            else if (nx.tok.kind == Tok::Eof)
                t.spacing = Spacing::Alone;
            else
                t.spacing = Spacing::JointHidden;
        }
        toks.push_back(t);
    }
    if (ok) {
        size_t n_lex = out.errors.size();
        int lex_fatal = out.fatal_index;
        check_delims(toks, src, out);
        if (out.errors.size() > n_lex && lex_fatal < 0 &&
            out.errors.back().message.rfind("unexpected closing delimiter", 0) == 0) {
            // The pass stopped there: lexer errors past it come after.
            uint32_t stop = out.errors.back().span.lo;
            std::vector<LexDiag> before, after;
            for (size_t k = 0; k < n_lex; ++k) (out.errors[k].span.lo < stop ? before : after).push_back(out.errors[k]);
            if (!after.empty()) {
                std::vector<LexDiag> all = before;
                out.fatal_index = static_cast<int>(all.size());
                for (size_t k = n_lex; k < out.errors.size(); ++k) all.push_back(out.errors[k]);
                for (const LexDiag& e : after) all.push_back(e);
                out.errors.swap(all);
            }
        }
    } else if (out.fatal_index >= 0) {
        // rustc lexes while building token trees: an unexpected closing
        // delimiter at the top level ends the pass before the fatal error is
        // reached, so its delimiter errors are what gets printed.
        LexResult d;
        check_delims(toks, src, d);
        Span fatal_span = out.errors[static_cast<size_t>(out.fatal_index)].span;
        if (!d.errors.empty() && d.errors.back().message.rfind("unexpected closing delimiter", 0) == 0 &&
            d.errors.back().span.lo < fatal_span.lo) {
            uint32_t stop = d.errors.back().span.lo;
            std::vector<LexDiag> kept;
            for (size_t k = 0; k < static_cast<size_t>(out.fatal_index); ++k)
                if (out.errors[k].span.lo < stop) kept.push_back(out.errors[k]);
            out.fatal_index = static_cast<int>(kept.size());
            for (const LexDiag& e : d.errors) kept.push_back(e);
            // What rustc never got to stays, marked as after the fatal error.
            for (size_t k = 0; k < out.errors.size(); ++k)
                if (out.errors[k].span.lo >= stop) kept.push_back(out.errors[k]);
            out.errors.swap(kept);
        }
    }
    balance_delims(toks, out.partner);
    return out;
}

}  // namespace rust
