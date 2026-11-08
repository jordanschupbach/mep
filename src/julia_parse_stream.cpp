// ParseStream, token validation, literal parsing and tree building for
// julia_parse.h.
//
// Ported from JuliaSyntax.jl 0.4.10 src/parse_stream.jl, literal_parsing.jl,
// green_tree.jl and parser_api.jl (MIT "Expat" licence, Copyright (c) 2021
// Julia Computing and contributors).

#include "julia_parse_internal.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace jlparse {

ParseStream::ParseStream(std::string_view t) : text(t), lexer(t, 0) {
    // Initial sentinel token containing the first byte of the first real token.
    tokens.push_back(SyntaxToken{K("TOMBSTONE"), EMPTY_FLAGS, K("TOMBSTONE"), false, 1});
}

//------------------------------------------------------------------------------
// Return true when a token was emitted last at stream position `pos`
bool ParseStream::token_is_last(ParseStreamPosition pos) const {
    return pos.range_index == 0 || pos.token_index > rng(pos.range_index).last_token;
}

//------------------------------------------------------------------------------
// Stream input interface - the peek_* family of functions

void ParseStream::_buffer_lookahead_tokens() {
    bool had_whitespace = false;
    int token_count = 0;
    while (true) {
        RawToken raw = lexer.next_token();
        Kind k = raw.kind;
        bool was_whitespace = is_whitespace(k);
        had_whitespace = had_whitespace || was_whitespace;
        RawFlags f = EMPTY_FLAGS;
        if (raw.dotop) f |= DOTOP_FLAG;
        if (raw.suffix) f |= SUFFIXED_FLAG;
        lookahead.push_back(SyntaxToken{k, f, k, had_whitespace, static_cast<uint32_t>(raw.endbyte + 2)});
        token_count += 1;
        if (k == K("EndMarker")) break;
        if (!was_whitespace) {
            had_whitespace = false;
            if (token_count > 100) break;
        }
    }
}

size_t ParseStream::_lookahead_index(size_t n, bool skip_newlines) {
    size_t i = lookahead_index;
    while (true) {
        if (i + 1 > lookahead.size()) {
            size_t n_to_delete = lookahead_index - 1;
            if (static_cast<double>(n_to_delete) > 0.9 * static_cast<double>(lookahead.size())) {
                lookahead.erase(lookahead.begin(), lookahead.begin() + static_cast<std::ptrdiff_t>(n_to_delete));
                i -= n_to_delete;
                lookahead_index = 1;
            }
            _buffer_lookahead_tokens();
            continue;
        }
        Kind k = la(i).kind;
        if (!((k == K("Whitespace") || k == K("Comment")) || (k == K("NewlineWs") && skip_newlines))) {
            if (n == 1) return i;
            n -= 1;
        }
        i += 1;
    }
}

namespace {
struct ParserStuck {};
}  // namespace

Kind ParseStream::peek(size_t n, bool skip_newlines, bool skip_whitespace) {
    return peek_token(n, skip_newlines, skip_whitespace).kind;
}

SyntaxToken ParseStream::peek_token(size_t n, bool skip_newlines, bool skip_whitespace) {
    peek_count += 1;
    if (peek_count > 100000) throw ParserStuck{};
    size_t i = _lookahead_index(n, skip_newlines);
    if (!skip_whitespace) i = lookahead_index;
    return la(i);
}

FullToken ParseStream::peek_full_token(size_t n, bool skip_newlines, bool skip_whitespace) {
    peek_count += 1;
    if (peek_count > 100000) throw ParserStuck{};
    size_t i = _lookahead_index(n, skip_newlines);
    if (!skip_whitespace) i = lookahead_index;
    const SyntaxToken &t = la(i);
    return FullToken{t.kind, t.flags, lookahead_token_first_byte(i), lookahead_token_last_byte(i)};
}

PeekBehind ParseStream::peek_behind(ParseStreamPosition pos) const {
    if (token_is_last(pos) && pos.token_index > 0) {
        const SyntaxToken &t = tok(pos.token_index);
        return PeekBehind{t.kind, static_cast<RawFlags>(t.flags & ~NUMERIC_FLAGS), t.orig_kind, true};
    } else if (!ranges.empty() && pos.range_index > 0) {
        const TaggedRange &r = rng(pos.range_index);
        return PeekBehind{r.kind, r.flags, K("None"), false};
    } else {
        return PeekBehind{K("None"), EMPTY_FLAGS, K("None"), true};
    }
}

ParseStreamPosition ParseStream::first_child_position(ParseStreamPosition pos) const {
    // Find the first nontrivia range which is a child of this range but not a
    // child of the child
    uint32_t c = 0;
    const TaggedRange &parent = rng(pos.range_index);
    for (uint32_t i = pos.range_index - 1; i >= 1; --i) {
        if (rng(i).first_token < parent.first_token) break;
        if ((c == 0 || rng(i).first_token < rng(c).first_token) && !is_trivia(rng(i).flags)) c = i;
    }
    // Find first nontrivia token
    uint32_t t = 0;
    for (uint32_t i = parent.first_token; i <= parent.last_token; ++i) {
        if (!is_trivia(tok(i).flags)) {
            t = i;
            break;
        }
    }
    if (c != 0) {
        if (t != 0) {
            if (rng(c).first_token > t) {
                return ParseStreamPosition{t, 0};
            } else {
                return ParseStreamPosition{rng(c).last_token, c};
            }
        } else {
            return ParseStreamPosition{rng(c).last_token, c};
        }
    } else {
        return ParseStreamPosition{t, c};
    }
}

ParseStreamPosition ParseStream::last_child_position(ParseStreamPosition pos) const {
    // From JuliaSyntax 1.0.2 (used only to reproduce its diagnostics).
    const TaggedRange &parent = rng(pos.range_index);
    uint32_t c = 0;
    if (pos.range_index > 1) {
        uint32_t i = pos.range_index - 1;
        if (rng(i).first_token >= parent.first_token) c = i;
    }
    uint32_t t = 0;
    for (uint32_t i = parent.last_token; i >= parent.first_token && i >= 1; --i) {
        if (!is_trivia(tok(i).flags)) {
            t = i;
            break;
        }
    }
    if (c == 0 || (t != 0 && rng(c).last_token < t)) return ParseStreamPosition{t, 0};
    return ParseStreamPosition{rng(c).last_token, c};
}

ParseStreamPosition ParseStream::peek_behind_pos(bool skip_trivia, bool skip_parens) const {
    uint32_t token_index = static_cast<uint32_t>(tokens.size());
    uint32_t range_index = static_cast<uint32_t>(ranges.size());
    if (skip_parens) {
        while (range_index >= 1 && rng(range_index).kind == K("parens")) range_index -= 1;
    }
    uint32_t last_token_in_nonterminal = range_index == 0 ? 0 : rng(range_index).last_token;
    while (token_index > last_token_in_nonterminal) {
        const SyntaxToken &t = tok(token_index);
        if (t.kind != K("TOMBSTONE") && (!skip_trivia || !is_trivia(t.flags))) break;
        token_index -= 1;
    }
    return ParseStreamPosition{token_index, range_index};
}

//------------------------------------------------------------------------------
// Stream output interface - the `bump_*` and `emit_*` family of functions

void ParseStream::_bump_until_n(size_t n, RawFlags flags, Kind remap_kind) {
    if (n < lookahead_index) return;
    for (size_t i = lookahead_index; i <= n; ++i) {
        const SyntaxToken &t = la(i);
        Kind k = t.kind;
        if (k == K("EndMarker")) break;
        RawFlags f = static_cast<RawFlags>(flags | (t.flags & ~NUMERIC_FLAGS));
        bool trivia = is_whitespace(k);
        if (trivia) f |= TRIVIA_FLAG;
        Kind outk = (trivia || remap_kind == K("None")) ? k : remap_kind;
        tokens.push_back(SyntaxToken{outk, f, t.kind, t.preceding_whitespace, t.next_byte});
    }
    lookahead_index = n + 1;
    peek_count = 0;
}

ParseStreamPosition ParseStream::bump(RawFlags flags, bool skip_newlines, const char *error, Kind remap_kind) {
    ParseStreamPosition emark = position();
    _bump_until_n(_lookahead_index(1, skip_newlines), flags, remap_kind);
    if (error != nullptr) {
        std::string e = error;
        emit(emark, K("error"), flags, &e);
    }
    return position();
}

ParseStreamPosition ParseStream::bump(RawFlags flags, bool skip_newlines, const std::string &error, Kind remap_kind) {
    return bump(flags, skip_newlines, error.c_str(), remap_kind);
}

ParseStreamPosition ParseStream::bump_trivia(RawFlags flags, bool skip_newlines, const char *error) {
    ParseStreamPosition emark = position();
    _bump_until_n(_lookahead_index(1, skip_newlines) - 1, EMPTY_FLAGS);
    if (error != nullptr) {
        std::string e = error;
        emit(emark, K("error"), flags, &e);
    }
    return position();
}

ParseStreamPosition ParseStream::bump_invisible(Kind kind, RawFlags flags, const std::string *error) {
    uint32_t b = _next_byte();
    tokens.push_back(SyntaxToken{kind, flags, kind, false, b});
    if (error != nullptr) emit_diagnostic_range(b, static_cast<int64_t>(b) - 1, Level::Error, *error);
    peek_count = 0;
    return position();
}

ParseStreamPosition ParseStream::bump_glue(Kind kind, RawFlags flags) {
    size_t i = lookahead_index;
    tokens.push_back(SyntaxToken{kind, flags, kind, false, la(i + 1).next_byte});
    lookahead_index += 2;
    peek_count = 0;
    return position();
}

ParseStreamPosition ParseStream::bump_split(std::initializer_list<SplitSpec> split_spec) {
    SyntaxToken t = la(lookahead_index);
    lookahead_index += 1;
    uint32_t b = _next_byte();
    size_t i = 0;
    for (const SplitSpec &s : split_spec) {
        ++i;
        b = (i == split_spec.size()) ? t.next_byte : b + static_cast<uint32_t>(s.nbyte);
        tokens.push_back(SyntaxToken{s.kind, s.flags, t.kind, false, b});
    }
    peek_count = 0;
    return position();
}

void ParseStream::reset_node(ParseStreamPosition pos, const Kind *kind, const RawFlags *flags) {
    if (token_is_last(pos)) {
        SyntaxToken &t = tok(pos.token_index);
        if (kind) t.kind = *kind;
        if (flags) t.flags = *flags;
    } else {
        TaggedRange &r = rng(pos.range_index);
        if (kind) r.kind = *kind;
        if (flags) r.flags = *flags;
    }
}

bool ParseStream::steal_token_bytes(ParseStreamPosition pos, uint32_t numbytes) {
    size_t i = pos.token_index;
    SyntaxToken &t1 = tok(i);
    SyntaxToken &t2 = tok(i + 1);
    uint32_t t1_next_byte = t1.next_byte + numbytes;
    t1.next_byte = t1_next_byte;
    bool t2_is_empty = t1_next_byte == t2.next_byte;
    if (t2_is_empty) {
        t2.kind = K("TOMBSTONE");
        t2.flags = EMPTY_FLAGS;
    }
    return t2_is_empty;
}

ParseStreamPosition ParseStream::emit(ParseStreamPosition mark, Kind kind, RawFlags flags, const std::string *error) {
    uint32_t first_token = mark.token_index + 1;
    TaggedRange range{kind, flags, first_token, static_cast<uint32_t>(tokens.size())};
    if (error != nullptr) {
        // The first child must be a leaf, otherwise ranges would be improperly nested.
        uint32_t fbyte = token_first_byte(first_token);
        uint32_t lbyte = token_last_byte(tokens.size());
        emit_diagnostic_range(fbyte, lbyte, Level::Error, *error);
    }
    ranges.push_back(range);
    return position();
}

void ParseStream::emit_diagnostic_range(int64_t fbyte, int64_t lbyte, Level level, std::string msg) {
    if (depth_exceeded) return;  // only the nesting error itself after the cap is hit
    diagnostics.push_back(Diagnostic{fbyte, lbyte, level, std::move(msg)});
}

void ParseStream::emit_diagnostic_here(bool whitespace, Level level, std::string msg) {
    size_t i = _lookahead_index(1, true);
    size_t begin_tok_i = i;
    size_t end_tok_i = i;
    if (whitespace) {
        begin_tok_i = lookahead_index;
        end_tok_i = is_whitespace(la(i).kind) ? i : std::max(lookahead_index, i - 1);
    }
    uint32_t fbyte = lookahead_token_first_byte(begin_tok_i);
    uint32_t lbyte = lookahead_token_last_byte(end_tok_i);
    emit_diagnostic_range(fbyte, lbyte, level, std::move(msg));
}

void ParseStream::emit_diagnostic_mark(ParseStreamPosition mark, Level level, std::string msg, bool trim_whitespace) {
    size_t i = mark.token_index;
    size_t j = tokens.size();
    if (trim_whitespace) {
        while (i < j && is_whitespace(tok(j).kind)) j -= 1;
        while (i + 1 < j && is_whitespace(tok(i + 1).kind)) i += 1;
    }
    int64_t f = tok(i).next_byte;
    int64_t l = static_cast<int64_t>(tok(j).next_byte) - 1;
    emit_diagnostic_range(f, l, level, std::move(msg));
}

void ParseStream::emit_diagnostic_marks(ParseStreamPosition mark, ParseStreamPosition end_mark, Level level,
                                        std::string msg) {
    int64_t fbyte = tok(mark.token_index).next_byte;
    int64_t lbyte = static_cast<int64_t>(tok(end_mark.token_index).next_byte) - 1;
    emit_diagnostic_range(fbyte, lbyte, level, std::move(msg));
}

//------------------------------------------------------------------------------
// Literal parsing (literal_parsing.jl)

FloatStatus parse_float_literal(bool is_float32, std::string_view txt, size_t firstind, size_t endind,
                                double *value) {
    // firstind/endind are 1-based, endind exclusive. _copy_normalize_number!
    size_t strsize = endind - firstind;
    std::string buf;
    buf.reserve(strsize + 1);
    const char *src = txt.data() + (firstind - 1);
    size_t i = 0;
    while (i < strsize) {
        unsigned char b = static_cast<unsigned char>(src[i]);
        if (b == '_') {
            i += 1;
            continue;
        } else if (b == 0xe2 && i + 2 < strsize && static_cast<unsigned char>(src[i + 1]) == 0x88 &&
                   static_cast<unsigned char>(src[i + 2]) == 0x92) {
            b = '-';
            i += 2;
        }
        buf.push_back(static_cast<char>(b));
        i += 1;
    }
    FloatStatus status = FloatStatus::Ok;
    if (is_float32) {
        // Convert float exponent 'f' to 'e' for strtof
        for (size_t p = buf.size(); p-- > 0;) {
            if (buf[p] == 'f') {
                buf[p] = 'e';
                break;
            }
        }
        errno = 0;
        float x = std::strtof(buf.c_str(), nullptr);
        if (errno == ERANGE) status = std::fabs(x) < 1.0f ? FloatStatus::Underflow : FloatStatus::Overflow;
        *value = static_cast<double>(x);
    } else {
        errno = 0;
        double x = std::strtod(buf.c_str(), nullptr);
        if (errno == ERANGE) status = std::fabs(x) < 1.0 ? FloatStatus::Underflow : FloatStatus::Overflow;
        *value = x;
    }
    return status;
}

namespace {
void PutChar(std::string *out, uint32_t n) {
    // print(io, Char(n)): UTF-8 encoding of any n <= 0x10ffff (surrogates too)
    if (!out) return;
    JChar c = C(static_cast<char32_t>(n));
    int len = ncodeunits(c);
    for (int k = 0; k < len; ++k) out->push_back(static_cast<char>((c >> (24 - 8 * k)) & 0xff));
}
}  // namespace

bool unescape_julia_string(std::string *out, std::string_view txt, size_t firstind, size_t endind,
                           std::vector<Diagnostic> *diags) {
    auto at = [&](size_t i) { return static_cast<unsigned char>(txt[i - 1]); };
    auto diag = [&](int64_t a, int64_t b, const char *msg) {
        if (diags) diags->push_back(Diagnostic{a, b, Level::Error, msg});
    };
    bool had_error = false;
    size_t i = firstind;
    while (i < endind) {
        unsigned char c = at(i);
        if (c != '\\') {
            if (c == '\r') {
                if (i + 1 < endind && at(i + 1) == '\n') i += 1;
                c = '\n';
            }
            if (out) out->push_back(static_cast<char>(c));
            i += 1;
            continue;
        }
        size_t escstart = i;
        i += 1;
        if (i >= endind) {
            diag(static_cast<int64_t>(escstart), static_cast<int64_t>(endind) - 1, "invalid escape sequence");
            had_error = true;
            break;
        }
        c = at(i);
        if (c == 'x' || c == 'u' || c == 'U') {
            uint32_t n = 0;
            int k = 0;
            int m = c == 'x' ? 2 : c == 'u' ? 4 : 8;
            while ((k += 1) <= m && i + 1 < endind) {
                unsigned char nc = at(i + 1);
                if ('0' <= nc && nc <= '9')
                    n = (n << 4) + static_cast<uint32_t>(nc - '0');
                else if ('a' <= nc && nc <= 'f')
                    n = (n << 4) + static_cast<uint32_t>(nc - 'a' + 10);
                else if ('A' <= nc && nc <= 'F')
                    n = (n << 4) + static_cast<uint32_t>(nc - 'A' + 10);
                else
                    break;
                i += 1;
            }
            if (k == 1 || n > 0x10ffff) {
                const char *msg = (m == 2) ? "invalid hex escape sequence" : "invalid unicode escape sequence";
                diag(static_cast<int64_t>(escstart), static_cast<int64_t>(i), msg);
                had_error = true;
            } else {
                if (m == 2) {
                    if (out) out->push_back(static_cast<char>(n));
                } else {
                    PutChar(out, n);
                }
            }
        } else if ('0' <= c && c <= '7') {
            int k = 1;
            uint32_t n = static_cast<uint32_t>(c - '0');
            while ((k += 1) <= 3 && i + 1 < endind) {
                c = at(i + 1);
                if ('0' <= c && c <= '7')
                    n = (n << 3) + static_cast<uint32_t>(c - '0');
                else
                    break;
                i += 1;
            }
            if (n > 255) {
                diag(static_cast<int64_t>(escstart), static_cast<int64_t>(i), "invalid octal escape sequence");
                had_error = true;
            } else {
                if (out) out->push_back(static_cast<char>(n));
            }
        } else {
            int u = c == 'n'    ? '\n'
                    : c == 't'  ? '\t'
                    : c == 'r'  ? '\r'
                    : c == 'e'  ? 0x1b
                    : c == 'b'  ? '\b'
                    : c == 'f'  ? '\f'
                    : c == 'v'  ? '\v'
                    : c == 'a'  ? '\a'
                    : c == '\\' ? '\\'
                    : c == '\'' ? '\''
                    : c == '"'  ? '"'
                    : c == '$'  ? '$'
                    : c == '`'  ? '`'
                                : -1;
            if (u < 0) {
                diag(static_cast<int64_t>(escstart), static_cast<int64_t>(i), "invalid escape sequence");
                had_error = true;
            } else {
                if (out) out->push_back(static_cast<char>(u));
            }
        }
        i += 1;
    }
    return had_error;
}

//------------------------------------------------------------------------------
// ParseStream Post-processing

namespace {
const char *TokenErrorDescription(Kind k) {
    if (k == K("ErrorEofMultiComment")) return "unterminated multi-line comment #= ... =#";
    if (k == K("ErrorInvalidNumericConstant")) return "invalid numeric constant";
    if (k == K("ErrorHexFloatMustContainP")) return "hex float literal must contain `p` or `P`";
    if (k == K("ErrorAmbiguousNumericConstant"))
        return "ambiguous `.` syntax; add whitespace to clarify (eg `1.+2` might be `1.0+2` or `1 .+ 2`)";
    if (k == K("ErrorAmbiguousNumericDotMultiply"))
        return "numeric constant cannot be implicitly multiplied because it ends with `.`";
    if (k == K("ErrorInvalidInterpolationTerminator"))
        return "interpolated variable ends with invalid character; use `$(...)` instead";
    if (k == K("ErrorNumericOverflow")) return "overflow in numeric literal";
    if (k == K("ErrorInvalidEscapeSequence")) return "invalid string escape sequence";
    if (k == K("ErrorOverLongCharacter")) return "character literal contains multiple characters";
    if (k == K("ErrorInvalidUTF8")) return "invalid UTF-8 sequence";
    if (k == K("ErrorInvisibleChar")) return "invisible character";
    if (k == K("ErrorUnknownCharacter")) return "unknown unicode character";
    if (k == K("ErrorBidiFormatting")) return "unbalanced bidirectional unicode formatting";
    if (k == K("ErrorInvalidOperator")) return "invalid operator";
    if (k == K("Error**"))
        return "use `x^y` instead of `x**y` for exponentiation, and `x...` instead of `**x` for splatting";
    return "unknown error token";
}
}  // namespace

void ParseStream::validate_tokens() {
    for (size_t i = 2; i <= tokens.size(); ++i) {
        SyntaxToken &t = tok(i);
        Kind k = t.kind;
        size_t fbyte = tok(i - 1).next_byte;
        size_t nbyte = t.next_byte;
        int64_t r1 = static_cast<int64_t>(fbyte), r2 = static_cast<int64_t>(nbyte) - 1;
        Kind error_kind = K("None");
        if (k == K("Integer") || k == K("BinInt") || k == K("OctInt") || k == K("HexInt")) {
            // cannot error
        } else if (k == K("Float") || k == K("Float32")) {
            double x = 0;
            FloatStatus code = parse_float_literal(k == K("Float32"), text, fbyte, nbyte, &x);
            bool underflow0 = code == FloatStatus::Underflow && x == 0;
            if (code == FloatStatus::Ok) {
            } else if (code == FloatStatus::Overflow) {
                emit_diagnostic_range(r1, r2, Level::Error, "overflow in floating point literal");
                error_kind = K("ErrorNumericOverflow");
            } else if (underflow0) {
                emit_diagnostic_range(r1, r2, Level::Warning, "underflow to zero in floating point literal");
            }
        } else if (k == K("Char")) {
            std::string charbuf;
            std::vector<Diagnostic> ds;
            bool had_error = unescape_julia_string(&charbuf, text, fbyte, nbyte, &ds);
            for (auto &d : ds) diagnostics.push_back(std::move(d));
            if (had_error) {
                error_kind = K("ErrorInvalidEscapeSequence");
            } else {
                size_t len = 0;
                if (!charbuf.empty()) ReadCharIO(charbuf, 0, &len);
                if (len < charbuf.size()) {
                    error_kind = K("ErrorOverLongCharacter");
                    emit_diagnostic_range(r1, r2, Level::Error, "character literal contains multiple characters");
                }
            }
        } else if (k == K("String") && !has_flags(t.flags, RAW_STRING_FLAG)) {
            std::vector<Diagnostic> ds;
            bool had_error = unescape_julia_string(nullptr, text, fbyte, nbyte, &ds);
            for (auto &d : ds) diagnostics.push_back(std::move(d));
            if (had_error) error_kind = K("ErrorInvalidEscapeSequence");
        } else if (is_error(k) && k != K("error") &&
                   std::find(remapped_102.begin(), remapped_102.end(), i) == remapped_102.end()) {
            std::string_view tokstr = text.substr(fbyte - 1, nbyte - fbyte);
            std::string msg;
            if (k == K("ErrorInvisibleChar") || k == K("ErrorUnknownCharacter")) {
                size_t len = 0;
                JChar c = tokstr.empty() ? 0 : ReadCharStr(tokstr, 0, &len);
                // JuliaSyntax 1.0.2 (Julia 1.12) reports identifier characters
                // which can't start an identifier as ErrorIdentifierStart.
                if (k == K("ErrorUnknownCharacter") && is_identifier_char(c))
                    msg = std::string("identifier cannot begin with character ") + ReprChar(c);
                else
                    msg = std::string(TokenErrorDescription(k)) + " " + ReprChar(c);
            } else if (k == K("ErrorInvalidUTF8") || k == K("ErrorBidiFormatting")) {
                msg = std::string(TokenErrorDescription(k)) + " " + ReprString(tokstr);
            } else {
                msg = TokenErrorDescription(k);
            }
            emit_diagnostic_range(r1, r2, Level::Error, std::move(msg));
        }
        if (error_kind != K("None")) {
            t.kind = error_kind;
            t.flags = EMPTY_FLAGS;
        }
    }
    std::stable_sort(diagnostics.begin(), diagnostics.end(),
                     [](const Diagnostic &a, const Diagnostic &b) { return a.first_byte < b.first_byte; });
}

//------------------------------------------------------------------------------
// Tree construction (build_tree / build_tree(GreenNode, ...))

Tree ParseStream::build_tree() const {
    // Postorder nodes first, as JuliaSyntax builds them, then a preorder copy.
    struct PNode {
        Kind kind;
        RawFlags flags;
        uint32_t first_byte;  // 1-based
        uint32_t span;
        bool leaf;
        uint32_t child_begin, child_end;  // into `kids`
    };
    std::vector<PNode> pnodes;
    std::vector<uint32_t> kids;
    struct StackEntry {
        uint32_t first_token;
        uint32_t node;
    };
    std::vector<StackEntry> stack;
    pnodes.reserve(tokens.size() + ranges.size());

    size_t i = 1;
    size_t j = 1;
    while (true) {
        size_t last_token = j <= ranges.size() ? rng(j).last_token : tokens.size();
        while (i <= last_token) {
            const SyntaxToken &t = tok(i);
            if (t.kind == K("TOMBSTONE")) {
                i += 1;
                continue;
            }
            uint32_t fb = tok(i - 1).next_byte;
            uint32_t span = t.next_byte - fb;
            pnodes.push_back(PNode{t.kind, t.flags, fb, span, true, 0, 0});
            stack.push_back(StackEntry{static_cast<uint32_t>(i), static_cast<uint32_t>(pnodes.size() - 1)});
            i += 1;
        }
        if (j > ranges.size()) break;
        while (j <= ranges.size()) {
            const TaggedRange &r = rng(j);
            if (r.last_token != last_token) break;
            if (r.kind == K("TOMBSTONE")) {
                j += 1;
                continue;
            }
            size_t k = stack.size() + 1;
            while (k > 1 && r.first_token <= stack[k - 2].first_token) k -= 1;
            uint32_t fb = tok(r.first_token - 1).next_byte;
            uint32_t span = tok(r.last_token).next_byte - fb;
            uint32_t cb = static_cast<uint32_t>(kids.size());
            for (size_t n = k; n <= stack.size(); ++n) kids.push_back(stack[n - 1].node);
            pnodes.push_back(PNode{r.kind, r.flags, fb, span, false, cb, static_cast<uint32_t>(kids.size())});
            stack.resize(k - 1);
            stack.push_back(StackEntry{r.first_token, static_cast<uint32_t>(pnodes.size() - 1)});
            j += 1;
        }
    }
    uint32_t root;
    if (stack.size() == 1) {
        root = stack[0].node;
    } else {
        uint32_t fb = tokens.front().next_byte;
        uint32_t span = tokens.back().next_byte - fb;
        uint32_t cb = static_cast<uint32_t>(kids.size());
        for (const StackEntry &e : stack) kids.push_back(e.node);
        pnodes.push_back(PNode{K("wrapper"), EMPTY_FLAGS, fb, span, false, cb, static_cast<uint32_t>(kids.size())});
        root = static_cast<uint32_t>(pnodes.size() - 1);
    }

    // Preorder, iteratively.
    Tree tree;
    tree.nodes.reserve(pnodes.size());
    struct Frame {
        uint32_t pnode;
        uint32_t out;     // index in tree.nodes
        uint32_t next;    // next child slot in kids
    };
    std::vector<Frame> work;
    auto push = [&](uint32_t p) {
        const PNode &n = pnodes[p];
        tree.nodes.push_back(Node{n.kind, n.flags, n.first_byte - 1, n.span, 0, n.leaf});
        work.push_back(Frame{p, static_cast<uint32_t>(tree.nodes.size() - 1), n.child_begin});
    };
    push(root);
    while (!work.empty()) {
        Frame &f = work.back();
        const PNode &n = pnodes[f.pnode];
        if (f.next < n.child_end) {
            uint32_t c = kids[f.next++];
            push(c);
        } else {
            tree.nodes[f.out].end = static_cast<uint32_t>(tree.nodes.size());
            work.pop_back();
        }
    }
    return tree;
}

//------------------------------------------------------------------------------
// API

ParseResult Parse(std::string_view text) {
    ParseResult result;
    ParseStream stream(text);
    try {
        ParseToplevel(stream);
    } catch (const ParserStuck &) {
        // JuliaSyntax throws "The parser seems stuck"; keep what we have and
        // cover the rest of the input with an error node.
        stream.depth_exceeded = false;
        ParseStreamPosition mark = stream.position();
        while (stream.peek(1, true, false) != K("EndMarker")) stream.bump(TRIVIA_FLAG, true);
        std::string e = "parser stuck";
        stream.emit(mark, K("error"), TRIVIA_FLAG, &e);
        ParseStreamPosition all{0, 0};
        stream.emit(all, K("toplevel"));
    }
    result.depth_limited = stream.depth_exceeded;
    stream.depth_exceeded = false;  // token diagnostics are all reported
    stream.validate_tokens();
    result.tree = stream.build_tree();
    result.diagnostics = std::move(stream.diagnostics);
    return result;
}

}  // namespace jlparse
