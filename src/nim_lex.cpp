// mep's Nim lexer: a port of the Nim 2.2 compiler's compiler/lexer.nim and
// nimlexbase.nim (MIT, Copyright (c) 2006-2026 Andreas Rumpf). See
// nim_parse.h. The source is held whole in memory, '\0'-terminated as
// nimlexbase's buffer is (a NUL byte ends the file there too).

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include "nim_parse.h"

namespace nimparse {

namespace {

constexpr char kCR = '\r';
constexpr char kLF = '\n';
constexpr char kEOF = '\0';

bool IsSymChar(char c) {
    unsigned char u = static_cast<unsigned char>(c);
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || u >= 0x80;
}
bool IsSymStartChar(char c) {
    unsigned char u = static_cast<unsigned char>(c);
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || u >= 0x80;
}
bool IsOpChar(char c) {
    switch (c) {
    case '+': case '-': case '*': case '/': case '\\': case '<': case '>': case '!': case '?': case '^':
    case '.': case '|': case '=': case '%': case '&': case '$': case '@': case '~': case ':':
        return true;
    default:
        return false;
    }
}
bool IsUnicodeOprStart(char c) {
    unsigned char u = static_cast<unsigned char>(c);
    return u == 226 || u == 194 || u == 195;
}
bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsHex(char c) { return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

// wordrecg.nim's TSpecialWord spellings, in enum order (index = ident id).
const char *const kSpecialWords[] = {
#include "nim_wordrecg.inc"
};
constexpr int kSpecialWordCount = static_cast<int>(sizeof(kSpecialWords) / sizeof(kSpecialWords[0]));
constexpr int kKeywordLow = 1;                                                   // wAddr
constexpr int kKeywordHigh = static_cast<int>(Tok::Yield) - static_cast<int>(Tok::Symbol);  // wYield
constexpr int kOprLow = kKeywordHigh + 1;                                        // wColon
constexpr int kOprHigh = kOprLow + 4;                                            // wDotDot

std::string LowerAscii(std::string s) {
    for (char &c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

}  // namespace

const char *TokName(Tok t) {
    static const char *const kNames[] = {
        "tkInvalid", "[EOF]", "tkSymbol",
        "addr", "and", "as", "asm", "bind", "block", "break", "case", "cast", "concept", "const", "continue",
        "converter", "defer", "discard", "distinct", "div", "do", "elif", "else", "end", "enum", "except", "export",
        "finally", "for", "from", "func", "if", "import", "in", "include", "interface", "is", "isnot", "iterator",
        "let", "macro", "method", "mixin", "mod", "nil", "not", "notin", "object", "of", "or", "out", "proc", "ptr",
        "raise", "ref", "return", "shl", "shr", "static", "template", "try", "tuple", "type", "using", "var", "when",
        "while", "xor", "yield",
        "tkIntLit", "tkInt8Lit", "tkInt16Lit", "tkInt32Lit", "tkInt64Lit", "tkUIntLit", "tkUInt8Lit", "tkUInt16Lit",
        "tkUInt32Lit", "tkUInt64Lit", "tkFloatLit", "tkFloat32Lit", "tkFloat64Lit", "tkFloat128Lit", "tkStrLit",
        "tkRStrLit", "tkTripleStrLit", "tkGStrLit", "tkGTripleStrLit", "tkCharLit", "tkCustomLit",
        "(", ")", "[", "]", "{", "}", "[.", ".]", "{.", ".}", "(.", ".)", ",", ";", ":", "::", "=", ".", "..", "[:",
        "tkOpr", "tkComment", "`",
    };
    return kNames[static_cast<int>(t)];
}

// ---- idents.nim ---------------------------------------------------------------

std::string IdentCache::Normalize(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (i > 0) {
            if (c == '_') continue;
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        out += c;
    }
    return out;
}

bool SameIdent(std::string_view a, std::string_view b) {
    if (a.empty() || b.empty()) return a.empty() && b.empty();
    return IdentCache::Normalize(a) == IdentCache::Normalize(b);
}

IdentCache::IdentCache() {
    map_.reserve(1024);
    for (int i = 0; i < kSpecialWordCount; i++) {
        std::string key = Normalize(kSpecialWords[i]);
        auto it = map_.find(key);
        if (it == map_.end()) map_.emplace(std::move(key), Entry{kSpecialWords[i], i});
        else it->second.id = i;  // getIdent(...).id = ord(s): the later word's id
    }
    counter_ = 1;
}

const std::string &IdentCache::Get(std::string_view s, int *id) {
    std::string key = Normalize(s);
    auto it = map_.find(key);
    if (it == map_.end()) {
        counter_++;
        it = map_.emplace(std::move(key), Entry{std::string(s), -counter_}).first;
    }
    if (id) *id = it->second.id;
    return it->second.spelling;
}

// ---- nimlexbase.nim -----------------------------------------------------------

Lexer::Lexer(std::string_view src, IdentCache *c, std::vector<Diag> *d, bool pretty)
    : nimpretty(pretty), cache(c), diags(d) {
    buf.assign(src.data(), src.size());
    src_len = static_cast<int>(src.size());
    buf.append(8, '\0');
    // skipUTF8BOM
    if (src.size() >= 3 && buf[0] == '\xEF' && buf[1] == '\xBB' && buf[2] == '\xBF') {
        bufpos = 3;
        line_start = 3;
    }
}

int Lexer::HandleCRLF(int pos) {
    if (buf[static_cast<size_t>(pos)] == kCR) {
        line_number++;
        pos++;
        line_start = pos;
        if (buf[static_cast<size_t>(pos)] == kLF) {
            pos++;
            line_start = pos;
        }
        return pos;
    }
    if (buf[static_cast<size_t>(pos)] == kLF) {
        line_number++;
        pos++;
        line_start = pos;
        return pos;
    }
    return pos;
}

void Lexer::Message(const std::string &msg, bool warning) {
    if (diags) diags->push_back(Diag{line_number, ColNumber(bufpos), msg, warning});
}
void Lexer::MessageTok(const Token &tok, const std::string &msg, bool warning) {
    if (diags) diags->push_back(Diag{tok.line, tok.col, msg, warning});
}
void Lexer::MessagePos(int pos, const std::string &msg) {
    if (diags) diags->push_back(Diag{line_number, pos - line_start, msg, false});
}

// ---- lexer.nim ------------------------------------------------------------------

#define B(i) buf[static_cast<size_t>(i)]

void Lexer::LexMessageLitNum(const std::string &msg, int startpos, bool warning) {
    auto literalish = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || IsDigit(c) || c == '_' || c == '.' || c == '\'';
    };
    int msg_pos = bufpos;
    std::string t;
    bufpos = startpos;
    while (literalish(B(bufpos))) t += B(bufpos++);
    if ((B(bufpos) == '+' || B(bufpos) == '-') && (B(bufpos - 1) == 'e' || B(bufpos - 1) == 'E')) {
        t += B(bufpos++);
        while (literalish(B(bufpos))) t += B(bufpos++);
    }
    if (literalish(B(bufpos))) {
        t += B(bufpos++);
        while (IsDigit(B(bufpos))) t += B(bufpos++);
    }
    bufpos = msg_pos;
    std::string text = msg;
    size_t at = text.find("$1");
    if (at != std::string::npos) text.replace(at, 2, t);
    Message(text, warning);
}

namespace {

// parseutils' parseBiggestInt / parseBiggestUInt over a literal with '_'
// separators: false on overflow or a malformed literal.
bool ParseInt64(const std::string &s, int64_t *out, bool *overflow) {
    size_t i = 0;
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        neg = s[i] == '-';
        i++;
    }
    if (i >= s.size() || !IsDigit(s[i])) return false;
    // Accumulate as a negative number so INT64_MIN fits.
    int64_t v = 0;
    for (; i < s.size(); i++) {
        if (s[i] == '_') continue;
        if (!IsDigit(s[i])) return false;
        int d = s[i] - '0';
        if (v < (std::numeric_limits<int64_t>::min() + d) / 10) {
            *overflow = true;
            return false;
        }
        v = v * 10 - d;
    }
    if (!neg) {
        if (v == std::numeric_limits<int64_t>::min()) {
            *overflow = true;
            return false;
        }
        v = -v;
    }
    *out = v;
    return true;
}

bool ParseUInt64(const std::string &s, uint64_t *out, bool *overflow) {
    size_t i = 0;
    if (i < s.size() && s[i] == '+') i++;
    if (i < s.size() && s[i] == '-') {
        *overflow = true;  // parseBiggestUInt rejects a sign: out of range
        return false;
    }
    if (i >= s.size() || !IsDigit(s[i])) return false;
    uint64_t v = 0;
    for (; i < s.size(); i++) {
        if (s[i] == '_') continue;
        if (!IsDigit(s[i])) return false;
        uint64_t d = static_cast<uint64_t>(s[i] - '0');
        if (v > (std::numeric_limits<uint64_t>::max() - d) / 10) {
            *overflow = true;
            return false;
        }
        v = v * 10 + d;
    }
    *out = v;
    return true;
}

}  // namespace

void Lexer::GetNumber(Token &tok) {
    auto matchUnderscoreChars = [&](bool (*in)(char)) {
        int pos = bufpos;
        int count = 0;
        while (true) {
            if (in(B(pos))) {
                tok.literal += B(pos);
                pos++;
                count++;
            } else {
                break;
            }
            if (B(pos) == '_') {
                if (!in(B(pos + 1))) {
                    Message("only single underscores may occur in a token and token may not "
                            "end with an underscore: e.g. '1__1' and '1_' are invalid");
                    break;
                }
                tok.literal += '_';
                pos++;
            }
        }
        bufpos = pos;
        return count;
    };
    auto dec = [](char c) { return IsDigit(c); };
    auto oct = [](char c) { return c >= '0' && c <= '7'; };
    auto hex = [](char c) { return IsHex(c); };
    auto bin = [](char c) { return c == '0' || c == '1'; };
    auto baseCode = [](char c) { return c == 'X' || c == 'x' || c == 'o' || c == 'b' || c == 'B' || c == 'c' || c == 'C'; };
    auto literalish = [&](char c) {
        return baseCode(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f') || IsDigit(c) || c == '_' || c == '\'';
    };
    auto isFloat = [](Tok t) { return t == Tok::FloatLit || t == Tok::Float32Lit || t == Tok::Float64Lit || t == Tok::Float128Lit; };

    uint64_t xi = 0;
    int num_digits = 0;
    tok.type = Tok::IntLit;
    tok.literal.clear();
    tok.base = 0;
    tok.offset_a = bufpos;
    bool positive = true;
    if (B(bufpos) == '-') {
        tok.literal += B(bufpos++);
        positive = false;
    }
    const int startpos = bufpos;

    if (B(bufpos) == '0' && (baseCode(B(bufpos + 1)) || B(bufpos + 1) == 'O')) {
        tok.literal += '0';
        bufpos++;
        switch (B(bufpos)) {
        case 'c': case 'C':
            LexMessageLitNum("$1 will soon be invalid for oct literals; Use '0o' for octals. 'c', 'C' prefix",
                             startpos, true);
            tok.literal += 'c';
            bufpos++;
            num_digits = matchUnderscoreChars(oct);
            break;
        case 'O':
            LexMessageLitNum("$1 is an invalid int literal; For octal literals use the '0o' prefix.", startpos);
            break;
        case 'x': case 'X':
            tok.literal += 'x';
            bufpos++;
            num_digits = matchUnderscoreChars(hex);
            break;
        case 'o':
            tok.literal += 'o';
            bufpos++;
            num_digits = matchUnderscoreChars(oct);
            break;
        case 'b': case 'B':
            tok.literal += 'b';
            bufpos++;
            num_digits = matchUnderscoreChars(bin);
            break;
        default:
            break;
        }
        if (num_digits == 0) LexMessageLitNum("invalid number: '$1'", startpos);
    } else {
        matchUnderscoreChars(dec);
        if (B(bufpos) == '.' && IsDigit(B(bufpos + 1))) {
            tok.type = Tok::FloatLit;
            tok.literal += '.';
            bufpos++;
            matchUnderscoreChars(dec);
        }
        if (B(bufpos) == 'e' || B(bufpos) == 'E') {
            tok.type = Tok::FloatLit;
            tok.literal += B(bufpos++);
            if (B(bufpos) == '+' || B(bufpos) == '-') tok.literal += B(bufpos++);
            matchUnderscoreChars(dec);
        }
    }
    const int endpos = bufpos;
    int post = endpos;
    char pc = B(post);
    if (pc == '\'' || pc == 'f' || pc == 'F' || pc == 'd' || pc == 'D' || pc == 'i' || pc == 'I' || pc == 'u' ||
        pc == 'U') {
        const int err_pos = post;
        bool custom_possible = false;
        if (B(post) == '\'') {
            post++;
            custom_possible = true;
        }
        if (IsSymChar(B(post))) {
            std::string suffix;
            while (true) {
                suffix += B(post);
                post++;
                if (!IsSymChar(B(post)) && B(post) != '_') break;
            }
            std::string low = LowerAscii(suffix);
            if (low == "f" || low == "f32") tok.type = Tok::Float32Lit;
            else if (low == "d" || low == "f64") tok.type = Tok::Float64Lit;
            else if (low == "f128") tok.type = Tok::Float128Lit;
            else if (low == "i8") tok.type = Tok::Int8Lit;
            else if (low == "i16") tok.type = Tok::Int16Lit;
            else if (low == "i32") tok.type = Tok::Int32Lit;
            else if (low == "i64") tok.type = Tok::Int64Lit;
            else if (low == "u") tok.type = Tok::UIntLit;
            else if (low == "u8") tok.type = Tok::UInt8Lit;
            else if (low == "u16") tok.type = Tok::UInt16Lit;
            else if (low == "u32") tok.type = Tok::UInt32Lit;
            else if (low == "u64") tok.type = Tok::UInt64Lit;
            else if (custom_possible) {
                tok.i_number = static_cast<int64_t>(tok.literal.size());
                tok.literal += '\'';
                tok.literal += suffix;
                tok.type = Tok::CustomLit;
            } else {
                LexMessageLitNum("invalid number suffix: '$1'", err_pos);
            }
        } else {
            LexMessageLitNum("invalid number suffix: '$1'", err_pos);
        }
    }
    if (literalish(B(post)) || (B(post) == '.' && IsDigit(B(post + 1))))
        LexMessageLitNum("invalid number: '$1'", startpos);

    if (tok.type != Tok::CustomLit) {
        bufpos = startpos;
        int pos = startpos;
        bool value_error = false, overflow = false;
        auto setNumber = [&](int64_t v) { tok.i_number = positive ? v : static_cast<int64_t>(0ULL - static_cast<uint64_t>(v)); };
        auto setFloat = [&](double v) { tok.f_number = positive ? v : -v; };
        if (B(pos) == '0' && baseCode(B(pos + 1))) {
            pos += 2;
            xi = 0;
            switch (B(pos - 1)) {
            case 'b': case 'B':
                tok.base = 1;
                while (pos < endpos) {
                    if (B(pos) != '_') xi = (xi << 1) | static_cast<uint64_t>(B(pos) - '0');
                    pos++;
                }
                break;
            case 'o': case 'c': case 'C':
                tok.base = 2;
                while (pos < endpos) {
                    if (B(pos) != '_') xi = (xi << 3) | static_cast<uint64_t>(B(pos) - '0');
                    pos++;
                }
                break;
            default:  // x, X
                tok.base = 3;
                while (pos < endpos) {
                    char c = B(pos);
                    if (c == '_') pos++;
                    else if (IsDigit(c)) { xi = (xi << 4) | static_cast<uint64_t>(c - '0'); pos++; }
                    else if (c >= 'a' && c <= 'f') { xi = (xi << 4) | static_cast<uint64_t>(c - 'a' + 10); pos++; }
                    else if (c >= 'A' && c <= 'F') { xi = (xi << 4) | static_cast<uint64_t>(c - 'A' + 10); pos++; }
                    else break;
                }
                break;
            }
            const int64_t sxi = static_cast<int64_t>(xi);
            switch (tok.type) {
            case Tok::IntLit: case Tok::Int64Lit: setNumber(sxi); break;
            case Tok::Int8Lit: setNumber(static_cast<int64_t>(static_cast<int8_t>(xi & 0xff))); break;
            case Tok::Int16Lit: setNumber(static_cast<int64_t>(static_cast<int16_t>(xi & 0xffff))); break;
            case Tok::Int32Lit: setNumber(static_cast<int64_t>(static_cast<int32_t>(xi & 0xffffffffu))); break;
            case Tok::UIntLit: case Tok::UInt64Lit: setNumber(sxi); break;
            case Tok::UInt8Lit: setNumber(static_cast<int64_t>(xi & 0xff)); break;
            case Tok::UInt16Lit: setNumber(static_cast<int64_t>(xi & 0xffff)); break;
            case Tok::UInt32Lit: setNumber(static_cast<int64_t>(xi & 0xffffffffu)); break;
            case Tok::Float32Lit: {
                uint32_t bits = static_cast<uint32_t>(xi & 0xffffffffu);
                float f;
                std::memcpy(&f, &bits, sizeof f);
                setFloat(static_cast<double>(f));
                break;
            }
            case Tok::Float64Lit: case Tok::FloatLit: {
                double d;
                std::memcpy(&d, &xi, sizeof d);
                setFloat(d);
                break;
            }
            default: break;
            }
            if (tok.base != 0 && InRange(tok.type, Tok::IntLit, Tok::UInt64Lit) && num_digits > 0) {
                int bits_per = tok.base == 1 ? 1 : tok.base == 2 ? 3 : 4;
                int width = 64;
                switch (tok.type) {
                case Tok::Int8Lit: case Tok::UInt8Lit: width = 8; break;
                case Tok::Int16Lit: case Tok::UInt16Lit: width = 16; break;
                case Tok::Int32Lit: case Tok::UInt32Lit: width = 32; break;
                default: break;
                }
                int max_digits = (width + bits_per - 1) / bits_per;
                if (num_digits > max_digits)
                    LexMessageLitNum("number has " + std::to_string(num_digits) + " digits but type only supports " +
                                         std::to_string(max_digits) + " digits: '$1'",
                                     startpos, true);
            }
            if (!isFloat(tok.type)) {
                bool out_of_range = false;
                switch (tok.type) {
                case Tok::UInt8Lit: case Tok::UInt16Lit: case Tok::UInt32Lit: out_of_range = tok.i_number != sxi; break;
                case Tok::Int8Lit: out_of_range = sxi > 255 || sxi < 0 ? (xi > 255) : false; break;
                case Tok::Int16Lit: out_of_range = xi > 65535; break;
                case Tok::Int32Lit: out_of_range = xi > 0xffffffffULL; break;
                default: break;
                }
                if (tok.type == Tok::Int8Lit) out_of_range = sxi > 255 || sxi < 0;
                if (tok.type == Tok::Int16Lit) out_of_range = sxi > 65535 || sxi < 0;
                if (tok.type == Tok::Int32Lit) out_of_range = sxi > 0xffffffffLL || sxi < 0;
                if (out_of_range) LexMessageLitNum("number out of range: '$1'", startpos);
            }
        } else {
            if (isFloat(tok.type)) {
                std::string digits;
                for (char c : tok.literal)
                    if (c != '_') digits += c;
                tok.f_number = std::strtod(digits.c_str(), nullptr);
            } else if (tok.type == Tok::UInt64Lit || tok.type == Tok::UIntLit) {
                uint64_t v = 0;
                if (ParseUInt64(tok.literal, &v, &overflow)) tok.i_number = static_cast<int64_t>(v);
                else if (!overflow) value_error = true;
            } else {
                int64_t v = 0;
                if (ParseInt64(tok.literal, &v, &overflow)) tok.i_number = v;
                else if (!overflow) value_error = true;
            }
            if (!value_error && !overflow) {
                int64_t n = tok.i_number;
                bool out_of_range = false;
                switch (tok.type) {
                case Tok::Int8Lit: out_of_range = n > 127 || n < -128; break;
                case Tok::UInt8Lit: out_of_range = n > 255 || n < 0; break;
                case Tok::Int16Lit: out_of_range = n > 32767 || n < -32768; break;
                case Tok::UInt16Lit: out_of_range = n > 65535 || n < 0; break;
                case Tok::Int32Lit: out_of_range = n > 2147483647LL || n < -2147483648LL; break;
                case Tok::UInt32Lit: out_of_range = n > 4294967295LL || n < 0; break;
                default: break;
                }
                if (out_of_range) LexMessageLitNum("number out of range: '$1'", startpos);
            }
        }
        if (value_error) LexMessageLitNum("invalid number: '$1'", startpos);
        else if (overflow) LexMessageLitNum("number out of range: '$1'", startpos);
        else if (tok.type == Tok::IntLit && (tok.i_number > 2147483647LL || tok.i_number < -2147483648LL))
            tok.type = Tok::Int64Lit;
    }
    tok.offset_b = post - 1;
    bufpos = post;
}

void Lexer::HandleHexChar(int &xi, int position) {
    char c = B(bufpos);
    auto invalid = [&]() {
        Message(std::string("expected a hex digit, but found: ") + c + "; maybe prepend with 0");
    };
    if (IsDigit(c)) { xi = (xi << 4) | (c - '0'); bufpos++; }
    else if (c >= 'a' && c <= 'f') { xi = (xi << 4) | (c - 'a' + 10); bufpos++; }
    else if (c >= 'A' && c <= 'F') { xi = (xi << 4) | (c - 'A' + 10); bufpos++; }
    else if (c == '"' || c == '\'') {
        if (position <= 1) invalid();
        if (position == 0) bufpos++;
    } else {
        invalid();
        bufpos++;
    }
}

namespace {
void AddUnicodeCodePoint(std::string &s, int64_t i64) {
    uint64_t i = static_cast<uint64_t>(i64);
    auto ch = [](uint64_t v) { return static_cast<char>(static_cast<unsigned char>(v & 0xff)); };
    if (i <= 127) s += ch(i);
    else if (i <= 0x07FF) { s += ch((i >> 6) | 0xC0); s += ch((i & 63) | 0x80); }
    else if (i <= 0xFFFF) { s += ch((i >> 12) | 0xE0); s += ch(((i >> 6) & 63) | 0x80); s += ch((i & 63) | 0x80); }
    else if (i <= 0x1FFFFF) {
        s += ch((i >> 18) | 0xF0); s += ch(((i >> 12) & 63) | 0x80); s += ch(((i >> 6) & 63) | 0x80); s += ch((i & 63) | 0x80);
    } else if (i <= 0x3FFFFFF) {
        s += ch((i >> 24) | 0xF8); s += ch(((i >> 18) & 63) | 0x80); s += ch(((i >> 12) & 63) | 0x80);
        s += ch(((i >> 6) & 63) | 0x80); s += ch((i & 63) | 0x80);
    } else if (i <= 0x7FFFFFFF) {
        s += ch((i >> 30) | 0xFC); s += ch(((i >> 24) & 63) | 0x80); s += ch(((i >> 18) & 63) | 0x80);
        s += ch(((i >> 12) & 63) | 0x80); s += ch(((i >> 6) & 63) | 0x80); s += ch((i & 63) | 0x80);
    }
}
}  // namespace

void Lexer::GetEscapedChar(Token &tok) {
    bufpos++;  // skip '\'
    char c = B(bufpos);
    switch (c) {
    case 'n': case 'N': tok.literal += '\n'; bufpos++; break;
    case 'p': case 'P':
        if (tok.type == Tok::CharLit) Message("\\p not allowed in character literal");
        tok.literal += '\n';
        bufpos++;
        break;
    case 'r': case 'R': case 'c': case 'C': tok.literal += '\r'; bufpos++; break;
    case 'l': case 'L': tok.literal += '\n'; bufpos++; break;
    case 'f': case 'F': tok.literal += '\f'; bufpos++; break;
    case 'e': case 'E': tok.literal += '\x1B'; bufpos++; break;
    case 'a': case 'A': tok.literal += '\a'; bufpos++; break;
    case 'b': case 'B': tok.literal += '\b'; bufpos++; break;
    case 'v': case 'V': tok.literal += '\v'; bufpos++; break;
    case 't': case 'T': tok.literal += '\t'; bufpos++; break;
    case '\'': case '"': tok.literal += c; bufpos++; break;
    case '\\': tok.literal += '\\'; bufpos++; break;
    case 'x': case 'X': {
        bufpos++;
        int xi = 0;
        HandleHexChar(xi, 1);
        HandleHexChar(xi, 2);
        tok.literal += static_cast<char>(xi & 0xff);
        break;
    }
    case 'u': case 'U': {
        if (tok.type == Tok::CharLit) Message("\\u not allowed in character literal");
        bufpos++;
        int xi = 0;
        if (B(bufpos) == '{') {
            bufpos++;
            int start = bufpos;
            int guard = 0;
            while (B(bufpos) != '}') {
                // Nim loops here until a '}' (reading past the end of its buffer on an
                // unterminated escape); stop at the end of the file instead.
                if (B(bufpos) == kEOF && bufpos >= src_len) break;
                HandleHexChar(xi, 0);
                if (xi > 0x7FFFFFF) xi = 0x7FFFFFF + (xi & 0xF);  // keep the int from overflowing
                if (++guard > (1 << 20)) break;
            }
            if (start == bufpos) Message("Unicode codepoint cannot be empty");
            bufpos++;
            if (xi > 0x10FFFF) {
                std::string hexs = buf.substr(static_cast<size_t>(start), static_cast<size_t>(bufpos - 1 - start));
                Message("Unicode codepoint must be lower than 0x10FFFF, but was: " + hexs);
            }
        } else {
            HandleHexChar(xi, 1);
            HandleHexChar(xi, 2);
            HandleHexChar(xi, 3);
            HandleHexChar(xi, 4);
        }
        AddUnicodeCodePoint(tok.literal, xi);
        break;
    }
    default:
        if (IsDigit(c)) {
            if (B(bufpos) == '0' && IsDigit(B(bufpos + 1))) Message("octal escape sequences do not exist; leading zero is ignored", true);
            int64_t xi = 0;
            while (IsDigit(B(bufpos))) {
                if (xi < (int64_t{1} << 40)) xi = xi * 10 + (B(bufpos) - '0');
                bufpos++;
            }
            if (xi <= 255) tok.literal += static_cast<char>(xi);
            else Message("invalid character constant");
        } else {
            Message("invalid character constant");
        }
        break;
    }
}

// mode: 0 normal, 1 raw, 2 generalized
void Lexer::GetString(Token &tok, int mode) {
    int pos = bufpos;
    int line = line_number;
    tok.offset_a = pos - (mode == 1 ? 1 : 0);
    pos++;  // skip "
    if (B(pos) == '"' && B(pos + 1) == '"') {
        tok.type = Tok::TripleStrLit;
        pos += 2;
        if (B(pos) == ' ' || B(pos) == '\t') {
            int np = pos + 1;
            while (B(np) == ' ' || B(np) == '\t') np++;
            if (B(np) == kCR || B(np) == kLF) pos = np;
        }
        pos = HandleCRLF(pos);
        while (true) {
            char c = B(pos);
            if (c == '"') {
                if (B(pos + 1) == '"' && B(pos + 2) == '"' && B(pos + 3) != '"') {
                    tok.offset_b = pos + 2;
                    bufpos = pos + 3;
                    break;
                }
                tok.literal += '"';
                pos++;
            } else if (c == kCR || c == kLF) {
                tok.offset_b = pos;
                pos = HandleCRLF(pos);
                tok.literal += '\n';
            } else if (c == kEOF) {
                tok.offset_b = pos;
                int line2 = line_number;
                line_number = line;
                MessagePos(line_start, "closing \"\"\" expected, but end of file reached");
                line_number = line2;
                bufpos = pos;
                break;
            } else {
                tok.literal += c;
                pos++;
            }
        }
    } else {
        tok.type = mode != 0 ? Tok::RStrLit : Tok::StrLit;
        while (true) {
            char c = B(pos);
            if (c == '"') {
                if (mode != 0 && B(pos + 1) == '"') {
                    pos += 2;
                    tok.literal += '"';
                } else {
                    tok.offset_b = pos;
                    pos++;
                    break;
                }
            } else if (c == kCR || c == kLF || c == kEOF) {
                tok.offset_b = pos;
                bufpos = pos;  // lexMessage reports at L.bufpos (not yet moved): match Nim below
                Message("closing \" expected");
                break;
            } else if (c == '\\' && mode == 0) {
                bufpos = pos;
                GetEscapedChar(tok);
                pos = bufpos;
            } else {
                tok.literal += c;
                pos++;
            }
        }
        bufpos = pos;
    }
}

void Lexer::GetCharacter(Token &tok) {
    tok.offset_a = bufpos;
    const int start_pos = bufpos;
    bufpos++;
    char c = B(bufpos);
    if ((static_cast<unsigned char>(c) < ' ') || c == '\'') {
        Message("invalid character literal");
        tok.literal = std::string(1, c);
    } else if (c == '\\') {
        GetEscapedChar(tok);
    } else {
        tok.literal = std::string(1, c);
        bufpos++;
    }
    if (B(bufpos) == '\'') {
        tok.offset_b = bufpos;
        bufpos++;
    } else {
        if (start_pos > 0 && B(start_pos - 1) == '`') {
            tok.literal = "'";
            bufpos = start_pos + 1;
        } else {
            Message("missing closing ' for character literal");
        }
        tok.offset_b = bufpos;
    }
}

int UnicodeOprLen(const char *s, bool *mul) {
    auto u = [&](int i) { return static_cast<unsigned char>(s[i]); };
    bool m = true;
    int len = 0;
    if (u(0) == 226) {
        if (u(1) == 136) {
            switch (u(2)) {
            case 152: case 153: case 167: case 169: len = 3; break;
            case 168: case 170: len = 3; m = false; break;
            default: break;
            }
        } else if (u(1) == 138) {
            switch (u(2)) {
            case 147: case 151: case 152: case 153: case 155: case 160: case 161: len = 3; break;
            case 148: case 149: case 150: case 158: case 159: len = 3; m = false; break;
            default: break;
            }
        } else if (u(1) == 152 && u(2) == 133) {
            len = 3;
        }
    } else if (u(0) == 194) {
        if (u(1) == 177) { len = 2; m = false; }
    } else if (u(0) == 195) {
        if (u(1) == 151) len = 2;
    }
    if (mul) *mul = m;
    return len;
}

void Lexer::GetSymbol(Token &tok) {
    int pos = bufpos;
    tok.offset_a = pos;
    while (true) {
        char c = B(pos);
        if ((c >= 'a' && c <= 'z') || IsDigit(c) || (c >= 'A' && c <= 'Z')) {
            pos++;
        } else if (c == '_') {
            if (!IsSymChar(B(pos + 1))) {
                Message("invalid token: trailing underscore");
                break;
            }
            pos++;
        } else if (static_cast<unsigned char>(c) >= 0x80) {
            if (IsUnicodeOprStart(c) && UnicodeOprLen(&buf[static_cast<size_t>(pos)]) != 0) break;
            pos++;
        } else {
            break;
        }
    }
    tok.offset_b = pos - 1;
    int id = 0;
    tok.ident = cache->Get(std::string_view(buf).substr(static_cast<size_t>(bufpos), static_cast<size_t>(pos - bufpos)), &id);
    tok.has_ident = true;
    if (id < kKeywordLow || id > kKeywordHigh) tok.type = Tok::Symbol;
    else tok.type = static_cast<Tok>(id + static_cast<int>(Tok::Symbol));
    bufpos = pos;
}

void Lexer::EndOperator(Token &tok, int pos) {
    int id = 0;
    tok.ident = cache->Get(std::string_view(buf).substr(static_cast<size_t>(bufpos), static_cast<size_t>(pos - bufpos)), &id);
    tok.has_ident = true;
    if (id < kOprLow || id > kOprHigh) tok.type = Tok::Opr;
    else tok.type = static_cast<Tok>(id - kOprLow + static_cast<int>(Tok::Colon));
    bufpos = pos;
}

void Lexer::GetOperator(Token &tok) {
    int pos = bufpos;
    tok.offset_a = pos;
    while (true) {
        char c = B(pos);
        if (IsOpChar(c)) {
            pos++;
        } else if (IsUnicodeOprStart(c)) {
            int n = UnicodeOprLen(&buf[static_cast<size_t>(pos)]);
            if (n == 0) break;
            pos += n;
        } else {
            break;
        }
    }
    EndOperator(tok, pos);
    tok.offset_b = pos - 1;
    tok.spacing = static_cast<uint8_t>(tok.spacing & ~(kTrailing | kSpacingEof));
    bool trailing = false;
    while (B(pos) == ' ') {
        pos++;
        trailing = true;
    }
    if (B(pos) == kCR || B(pos) == kLF || B(pos) == kEOF) tok.spacing |= kSpacingEof;
    else if (trailing) tok.spacing |= kTrailing;
}

int Precedence(const Token &tok) {
    constexpr int kMul = 9, kPlus = 8;
    switch (tok.type) {
    case Tok::Opr: {
        const std::string &s = tok.ident;
        if (s.empty()) return 2;
        char rc = s[0];
        if (s.size() > 1 && s.back() == '>' && (s[s.size() - 2] == '-' || s[s.size() - 2] == '~' || s[s.size() - 2] == '='))
            return 0;
        auto asgn = [&](int v) { return s.back() == '=' ? 1 : v; };
        switch (rc) {
        case '$': case '^': return asgn(10);
        case '*': case '%': case '/': case '\\': return asgn(kMul);
        case '~': return 8;
        case '+': case '-': case '|': return asgn(kPlus);
        case '&': return asgn(7);
        case '=': case '<': case '>': case '!': return 5;
        case '.': return asgn(6);
        case '?': return 2;
        default:
            if (IsUnicodeOprStart(rc)) {
                if (s.back() == '=') return 1;
                bool mul = true;
                int n = UnicodeOprLen(s.c_str(), &mul);
                if (n != 0) return mul ? kMul : kPlus;
                return 2;
            }
            return asgn(2);
        }
    }
    case Tok::Div: case Tok::Mod: case Tok::Shl: case Tok::Shr: return 9;
    case Tok::DotDot: return 6;
    case Tok::In: case Tok::Notin: case Tok::Is: case Tok::Isnot: case Tok::Of: case Tok::As: case Tok::From: return 5;
    case Tok::And: return 4;
    case Tok::Or: case Tok::Xor: case Tok::Ptr: case Tok::Ref: return 3;
    default: return -10;
    }
}

void Lexer::SkipMultiLineComment(Token &tok, int start, bool is_doc) {
    int pos = start;
    int to_strip = 0;
    tok.offset_a = pos;
    if (is_doc) {
        to_strip = ColNumber(pos);
        while (B(pos) == ' ') { pos++; to_strip++; }
        while (B(pos) == kCR || B(pos) == kLF) {
            pos = HandleCRLF(pos);
            to_strip = 0;
            while (B(pos) == ' ') { pos++; to_strip++; }
        }
    }
    int nesting = 0;
    while (true) {
        char c = B(pos);
        if (c == '#') {
            if (is_doc) {
                if (B(pos + 1) == '#' && B(pos + 2) == '[') nesting++;
                tok.literal += '#';
            } else if (B(pos + 1) == '[') {
                nesting++;
            }
            pos++;
        } else if (c == ']') {
            if (is_doc) {
                if (B(pos + 1) == '#' && B(pos + 2) == '#') {
                    if (nesting == 0) {
                        tok.offset_b = pos + 2;
                        pos += 3;
                        break;
                    }
                    nesting--;
                }
                tok.literal += ']';
            } else if (B(pos + 1) == '#') {
                if (nesting == 0) {
                    tok.offset_b = pos + 1;
                    pos += 2;
                    break;
                }
                nesting--;
            }
            pos++;
        } else if (c == kCR || c == kLF) {
            tok.offset_b = pos;
            pos = HandleCRLF(pos);
            if (nimpretty) tok.literal += '\n';
            if (is_doc) {
                if (!nimpretty) tok.literal += '\n';
                int k = to_strip;
                while (B(pos) == ' ' && k > 0) { pos++; k--; }
            }
        } else if (c == kEOF) {
            tok.offset_b = pos;
            MessagePos(pos, "end of multiline comment expected");
            break;
        } else {
            if (is_doc || nimpretty) tok.literal += c;
            pos++;
        }
    }
    bufpos = pos;
    if (nimpretty) tok.comment_offset_b = pos - 1;
}

void Lexer::ScanComment(Token &tok) {
    int pos = bufpos;
    tok.type = Tok::Comment;
    if (nimpretty) tok.comment_offset_a = pos;
    if (B(pos + 2) == '[') {
        SkipMultiLineComment(tok, pos + 3, true);
        return;
    }
    tok.offset_a = pos;
    pos += 2;
    int to_strip = 0;
    bool strip_init = false;
    while (true) {
        if (!strip_init) {
            while (B(pos) == ' ') { pos++; to_strip++; }
            if (B(pos) == kCR || B(pos) == kLF) to_strip = 0;
            else strip_init = true;
        }
        while (B(pos) != kCR && B(pos) != kLF && B(pos) != kEOF) tok.literal += B(pos++);
        tok.offset_b = pos;
        pos = HandleCRLF(pos);
        int indent = 0;
        while (B(pos) == ' ') { pos++; indent++; }
        if (B(pos) == '#' && B(pos + 1) == '#') {
            tok.literal += '\n';
            pos += 2;
            if (strip_init) {
                int k = to_strip;
                while (B(pos) == ' ' && k > 0) { pos++; k--; }
            }
        } else {
            if (static_cast<unsigned char>(B(pos)) > ' ') indent_ahead = indent;
            tok.offset_b = pos;
            break;
        }
    }
    bufpos = pos;
    if (nimpretty) tok.comment_offset_b = pos - 1;
}

void Lexer::Skip(Token &tok) {
    int pos = bufpos;
    tok.offset_a = pos;
    tok.spacing = static_cast<uint8_t>(tok.spacing & ~kLeading);
    bool has_comment = false;
    int comment_indent = curr_line_indent;
    if (nimpretty) {
        tok.comment_offset_a = pos;
        tok.comment_offset_b = pos;
        tok.line = -1;
    }
    while (true) {
        char c = B(pos);
        if (c == ' ') {
            pos++;
            tok.spacing |= kLeading;
        } else if (c == '\t') {
            MessagePos(pos, "tabs are not allowed, use spaces instead");
            pos++;
        } else if (c == kCR || c == kLF) {
            pos = HandleCRLF(pos);
            int indent = 0;
            while (true) {
                if (B(pos) == ' ') {
                    pos++;
                    indent++;
                } else if (B(pos) == '#' && B(pos + 1) == '[') {
                    if (nimpretty) {
                        has_comment = true;
                        if (tok.line < 0) {
                            tok.line = line_number;
                            comment_indent = indent;
                        }
                    }
                    int cstart = pos;
                    SkipMultiLineComment(tok, pos + 2, false);
                    pos = bufpos;
                    plain_comments.push_back({cstart, pos});
                } else {
                    break;
                }
            }
            tok.spacing = static_cast<uint8_t>(tok.spacing & ~kLeading);
            if (nimpretty && B(pos) == '#' && tok.line < 0) comment_indent = indent;
            if (static_cast<unsigned char>(B(pos)) > ' ' && (B(pos) != '#' || B(pos + 1) == '#')) {
                tok.indent = indent;
                curr_line_indent = indent;
                break;
            }
        } else if (c == '#') {
            if (B(pos + 1) == '#') break;
            if (nimpretty) {
                has_comment = true;
                if (tok.line < 0) tok.line = line_number;
            }
            int cstart = pos;
            if (B(pos + 1) == '[') {
                SkipMultiLineComment(tok, pos + 2, false);
                pos = bufpos;
            } else {
                tok.offset_a = pos;
                while (B(pos) != kCR && B(pos) != kLF && B(pos) != kEOF) {
                    if (nimpretty) tok.literal += B(pos);
                    pos++;
                }
                tok.offset_b = pos + 1;
                if (nimpretty) tok.comment_offset_b = pos + 1;
            }
            plain_comments.push_back({cstart, pos});
        } else {
            break;
        }
    }
    bufpos = pos;
    if (nimpretty && has_comment) {
        tok.comment_offset_b = pos - 1;
        tok.type = Tok::Comment;
        tok.indent = comment_indent;
    }
}

void Lexer::RawGetTok(Token &tok) {
    tok = Token();
    if (indent_ahead >= 0) {
        tok.indent = indent_ahead;
        curr_line_indent = indent_ahead;
        indent_ahead = -1;
    } else {
        tok.indent = -1;
    }
    Skip(tok);
    if (nimpretty && tok.type == Tok::Comment) {
        indent_ahead = curr_line_indent;
        tok.start = tok.comment_offset_a;
        tok.end = bufpos;
        return;
    }
    char c = B(bufpos);
    tok.line = line_number;
    tok.col = ColNumber(bufpos);
    tok.start = bufpos;
    if (IsSymStartChar(c) && c != 'r' && c != 'R' && !IsUnicodeOprStart(c)) {
        GetSymbol(tok);
    } else {
        switch (c) {
        case '#':
            ScanComment(tok);
            break;
        case '*':
            if (B(bufpos + 1) == ':' && !IsOpChar(B(bufpos + 2))) EndOperator(tok, bufpos + 1);
            else GetOperator(tok);
            break;
        case ',': tok.type = Tok::Comma; bufpos++; break;
        case 'r': case 'R':
            if (B(bufpos + 1) == '"') {
                bufpos++;
                GetString(tok, 1);
            } else {
                GetSymbol(tok);
            }
            break;
        case '(':
            bufpos++;
            if (B(bufpos) == '.' && B(bufpos + 1) != '.') { tok.type = Tok::ParDotLe; bufpos++; }
            else tok.type = Tok::ParLe;
            break;
        case ')': tok.type = Tok::ParRi; bufpos++; break;
        case '[':
            bufpos++;
            if (B(bufpos) == '.' && B(bufpos + 1) != '.') { tok.type = Tok::BracketDotLe; bufpos++; }
            else if (B(bufpos) == ':') { tok.type = Tok::BracketLeColon; bufpos++; }
            else tok.type = Tok::BracketLe;
            break;
        case ']': tok.type = Tok::BracketRi; bufpos++; break;
        case '.':
            if (B(bufpos + 1) == ']') { tok.type = Tok::BracketDotRi; bufpos += 2; }
            else if (B(bufpos + 1) == '}') { tok.type = Tok::CurlyDotRi; bufpos += 2; }
            else if (B(bufpos + 1) == ')') { tok.type = Tok::ParDotRi; bufpos += 2; }
            else GetOperator(tok);
            break;
        case '{':
            bufpos++;
            if (B(bufpos) == '.' && B(bufpos + 1) != '.') { tok.type = Tok::CurlyDotLe; bufpos++; }
            else tok.type = Tok::CurlyLe;
            break;
        case '}': tok.type = Tok::CurlyRi; bufpos++; break;
        case ';': tok.type = Tok::SemiColon; bufpos++; break;
        case '`': tok.type = Tok::Accent; bufpos++; break;
        case '_':
            bufpos++;
            if (!IsSymChar(B(bufpos)) && B(bufpos) != '_') {
                tok.type = Tok::Symbol;
                tok.ident = cache->Get("_");
                tok.has_ident = true;
            } else {
                tok.literal = std::string(1, c);
                tok.type = Tok::Invalid;
                Message(std::string("invalid token: ") + c + " (\\" + std::to_string(static_cast<unsigned char>(c)) + ")");
            }
            break;
        case '"': {
            int mode = (bufpos > 0 && IsSymChar(B(bufpos - 1))) ? 2 : 0;
            GetString(tok, mode);
            if (mode == 2) tok.type = static_cast<Tok>(static_cast<int>(tok.type) + 2);
            break;
        }
        case '\'':
            tok.type = Tok::CharLit;
            GetCharacter(tok);
            tok.type = Tok::CharLit;
            break;
        default:
            if (IsUnicodeOprStart(c)) {
                if (UnicodeOprLen(&buf[static_cast<size_t>(bufpos)]) != 0) GetOperator(tok);
                else GetSymbol(tok);
            } else if (IsDigit(c) ||
                       (c == '-' && IsDigit(B(bufpos + 1)) &&
                        (bufpos - 1 == 0 ||
                         (bufpos > 0 && (B(bufpos - 1) == ' ' || B(bufpos - 1) == '\t' || B(bufpos - 1) == '\n' ||
                                         B(bufpos - 1) == '\r' || B(bufpos - 1) == ',' || B(bufpos - 1) == ';' ||
                                         B(bufpos - 1) == '(' || B(bufpos - 1) == '[' || B(bufpos - 1) == '{'))))) {
                GetNumber(tok);
                char d = B(bufpos);
                if (IsSymChar(d) || d == '_') {
                    if (!(IsUnicodeOprStart(d) && UnicodeOprLen(&buf[static_cast<size_t>(bufpos)]) != 0))
                        Message("invalid token: no whitespace between number and identifier");
                }
            } else if (IsOpChar(c)) {
                GetOperator(tok);
            } else if (c == kEOF) {
                tok.type = Tok::Eof;
                tok.indent = 0;
            } else {
                tok.literal = std::string(1, c);
                tok.type = Tok::Invalid;
                Message(std::string("invalid token: ") + c + " (\\" + std::to_string(static_cast<unsigned char>(c)) + ")");
                bufpos++;
            }
            break;
        }
    }
    tok.end = bufpos;
}

#undef B

int IndentWidth(std::string_view src) {
    IdentCache cache;
    std::vector<Diag> diags;
    Lexer lex(src, &cache, &diags, true);
    Token tok;
    Tok prev = Tok::Eof;
    int result = 0;
    while (tok.type != Tok::Eof) {
        lex.RawGetTok(tok);
        if (tok.indent > 0 && (prev == Tok::Colon || prev == Tok::Equals || prev == Tok::Type || prev == Tok::Const ||
                               prev == Tok::Let || prev == Tok::Var || prev == Tok::Using)) {
            result = tok.indent;
            if (result > 0) break;
        }
        prev = tok.type;
    }
    return result;
}

namespace {
std::string NimFloat(double f) {
    if (std::isnan(f)) return "nan";
    if (std::isinf(f)) return f > 0 ? "inf" : "-inf";
    char b[64];
    for (int prec = 1; prec <= 17; prec++) {
        std::snprintf(b, sizeof b, "%.*g", prec, f);
        if (std::strtod(b, nullptr) == f) break;
    }
    std::string s = b;
    if (s.find_first_of(".en") == std::string::npos) s += ".0";
    return s;
}
}  // namespace

std::string TokString(const Token &tok) {
    if (InRange(tok.type, Tok::IntLit, Tok::Int64Lit)) return std::to_string(tok.i_number);
    if (InRange(tok.type, Tok::FloatLit, Tok::Float64Lit)) return NimFloat(tok.f_number);
    if (tok.type == Tok::Invalid || InRange(tok.type, Tok::StrLit, Tok::CharLit) || tok.type == Tok::Comment)
        return tok.literal;
    if (InRange(tok.type, Tok::ParLe, Tok::Colon) || tok.type == Tok::Eof || tok.type == Tok::Accent)
        return TokName(tok.type);
    return tok.has_ident ? tok.ident : std::string();
}

std::string PrettyTok(const Token &tok) {
    if (IsKeyword(tok.type)) return "keyword " + tok.ident;
    return TokString(tok);
}

std::vector<Token> Tokenize(std::string_view src, std::vector<Diag> *errors) {
    IdentCache cache;
    std::vector<Diag> diags;
    Lexer lex(src, &cache, &diags, true);
    std::vector<Token> out;
    Token tok;
    do {
        lex.RawGetTok(tok);
        out.push_back(tok);
    } while (tok.type != Tok::Eof && out.size() < 50'000'000);
    if (errors)
        for (Diag &d : diags)
            if (!d.warning) errors->push_back(d);
    return out;
}

}  // namespace nimparse
