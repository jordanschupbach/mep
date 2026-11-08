// mep's Go parser: see go_parse.h. A translation, function for function,
// of Go 1.26's go/scanner (scanner.go) and go/parser (parser.go), so the
// errors and the tree match go/parser in ParseComments|SkipObjectResolution
// mode (what gofmt uses). Left out: tracing, the object resolver,
// //go:build version extraction, and //line directives' effect on reported
// positions (their malformed-number errors are kept); positions here are
// always the physical ones.
//
// The Go source this is translated from carries this notice:
//
// Copyright 2009 The Go Authors.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//    * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//    * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//    * Neither the name of Google LLC nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "go_parse.h"

#include <algorithm>
#include <cstdio>

namespace goparse {

using namespace goast;

namespace {

constexpr int32_t kEof = -1;
constexpr int32_t kBom = 0xFEFF;

std::vector<int> LineTable(std::string_view src) {
    std::vector<int> lines{0};
    for (size_t i = 0; i < src.size(); i++) {
        if (src[i] == '\n' && i + 1 < src.size()) lines.push_back(static_cast<int>(i + 1));
    }
    return lines;
}

int LineOfOffset(const std::vector<int> &lines, int offset) {
    auto it = std::upper_bound(lines.begin(), lines.end(), offset);
    return static_cast<int>(it - lines.begin());  // 1-based
}

std::string Hex(uint32_t v, int width, bool lower = false) {
    char buf[16];
    std::snprintf(buf, sizeof buf, lower ? "%0*x" : "%0*X", width, v);
    return buf;
}

// fmt's %#U.
std::string FormatU(int32_t ch) {
    uint32_t r = static_cast<uint32_t>(ch);
    std::string s = "U+" + Hex(r, 4);
    if (ch >= 0 && UnicodeIsPrint(r)) {
        s += " '";
        AppendRune(s, r);
        s += "'";
    }
    return s;
}

// --- scanner -----------------------------------------------------------------

int32_t Lower(int32_t ch) { return ('a' - 'A') | ch; }
bool IsDecimal(int32_t ch) { return '0' <= ch && ch <= '9'; }
bool IsHex(int32_t ch) { return ('0' <= ch && ch <= '9') || ('a' <= Lower(ch) && Lower(ch) <= 'f'); }
bool IsLetter(int32_t ch) {
    return ('a' <= Lower(ch) && Lower(ch) <= 'z') || ch == '_' ||
           (ch >= 0x80 && UnicodeIsLetter(static_cast<uint32_t>(ch)));
}
bool IsDigit(int32_t ch) { return IsDecimal(ch) || (ch >= 0x80 && UnicodeIsDigit(static_cast<uint32_t>(ch))); }

int DigitVal(int32_t ch) {
    if ('0' <= ch && ch <= '9') return ch - '0';
    if ('a' <= Lower(ch) && Lower(ch) <= 'f') return Lower(ch) - 'a' + 10;
    return 16;
}

std::string LitName(int32_t prefix) {
    switch (prefix) {
    case 'x': return "hexadecimal literal";
    case 'o': case '0': return "octal literal";
    case 'b': return "binary literal";
    default: return "decimal literal";
    }
}

int InvalidSep(std::string_view x) {
    int32_t x1 = ' ';
    int32_t d = '.';
    size_t i = 0;
    if (x.size() >= 2 && x[0] == '0') {
        x1 = Lower(static_cast<unsigned char>(x[1]));
        if (x1 == 'x' || x1 == 'o' || x1 == 'b') {
            d = '0';
            i = 2;
        }
    }
    for (; i < x.size(); i++) {
        int32_t p = d;
        d = static_cast<unsigned char>(x[i]);
        if (d == '_') {
            if (p != '0') return static_cast<int>(i);
        } else if (IsDecimal(d) || (x1 == 'x' && IsHex(d))) {
            d = '0';
        } else {
            if (p == '_') return static_cast<int>(i) - 1;
            d = '.';
        }
    }
    if (d == '_') return static_cast<int>(x.size()) - 1;
    return -1;
}

std::string StripCR(std::string_view b, bool comment) {
    std::string c;
    c.reserve(b.size());
    for (size_t j = 0; j < b.size(); j++) {
        char ch = b[j];
        if (ch != '\r' || (comment && c.size() > 2 && c.back() == '*' && j + 1 < b.size() && b[j + 1] == '/')) {
            c += ch;
        }
    }
    return c;
}

using ErrorFn = void (*)(void *ctx, int offset, const std::string &msg);

class Scanner {
public:
    void Init(std::string_view src, ErrorFn err, void *ctx) {
        src_ = src;
        err_ = err;
        ctx_ = ctx;
        ch_ = ' ';
        offset_ = rd_offset_ = line_offset_ = 0;
        insert_semi_ = false;
        Next();
        if (ch_ == kBom) Next();
    }

    // Returns the token; *pos, *lit and (for STRING) *string_end filled in.
    Tok Scan(Pos *pos, std::string *lit, Pos *string_end);

private:
    void Error(int offs, const std::string &msg) {
        if (err_) err_(ctx_, offs, msg);
    }
    void Next();
    unsigned char Peek() const {
        return rd_offset_ < static_cast<int>(src_.size()) ? static_cast<unsigned char>(src_[static_cast<size_t>(rd_offset_)]) : 0;
    }
    std::string Slice(int a, int b) const { return std::string(src_.substr(static_cast<size_t>(a), static_cast<size_t>(b - a))); }
    std::pair<std::string, int> ScanComment();
    void UpdateLineInfo(int offs, std::string_view text);
    std::string ScanIdentifier();
    int Digits(int base, int *invalid);
    Tok ScanNumber(std::string *lit);
    bool ScanEscape(int32_t quote);
    std::string ScanRune();
    std::string ScanString();
    std::pair<std::string, int> ScanRawString();
    void SkipWhitespace() {
        while (ch_ == ' ' || ch_ == '\t' || (ch_ == '\n' && !insert_semi_) || ch_ == '\r') Next();
    }
    Tok Switch2(Tok t0, Tok t1) {
        if (ch_ == '=') {
            Next();
            return t1;
        }
        return t0;
    }
    Tok Switch3(Tok t0, Tok t1, int32_t ch2, Tok t2) {
        if (ch_ == '=') {
            Next();
            return t1;
        }
        if (ch_ == ch2) {
            Next();
            return t2;
        }
        return t0;
    }
    Tok Switch4(Tok t0, Tok t1, int32_t ch2, Tok t2, Tok t3) {
        if (ch_ == '=') {
            Next();
            return t1;
        }
        if (ch_ == ch2) {
            Next();
            if (ch_ == '=') {
                Next();
                return t3;
            }
            return t2;
        }
        return t0;
    }

    std::string_view src_;
    ErrorFn err_ = nullptr;
    void *ctx_ = nullptr;
    int32_t ch_ = ' ';
    int offset_ = 0, rd_offset_ = 0, line_offset_ = 0;
    bool insert_semi_ = false;
    Pos nl_pos_ = NoPos;
};

void Scanner::Next() {
    const int n = static_cast<int>(src_.size());
    if (rd_offset_ < n) {
        offset_ = rd_offset_;
        if (ch_ == '\n') line_offset_ = offset_;
        int32_t r = static_cast<unsigned char>(src_[static_cast<size_t>(rd_offset_)]);
        int w = 1;
        if (r == 0) {
            Error(offset_, "illegal character NUL");
        } else if (r >= 0x80) {
            r = static_cast<int32_t>(DecodeRune(src_, static_cast<size_t>(rd_offset_), &w));
            if (r == static_cast<int32_t>(RuneError) && w == 1) {
                std::string_view in = src_.substr(static_cast<size_t>(rd_offset_));
                auto b = [&](size_t k) { return static_cast<unsigned char>(in[k]); };
                if (offset_ == 0 && in.size() >= 2 && ((b(0) == 0xFF && b(1) == 0xFE) || (b(0) == 0xFE && b(1) == 0xFF))) {
                    Error(offset_, "illegal UTF-8 encoding (got UTF-16)");
                    rd_offset_ += static_cast<int>(in.size());
                } else {
                    Error(offset_, "illegal UTF-8 encoding");
                }
            } else if (r == kBom && offset_ > 0) {
                Error(offset_, "illegal byte order mark");
            }
        }
        rd_offset_ += w;
        ch_ = r;
    } else {
        offset_ = n;
        if (ch_ == '\n') line_offset_ = offset_;
        ch_ = kEof;
    }
}

std::pair<std::string, int> Scanner::ScanComment() {
    int offs = offset_ - 1;
    int next = -1;
    int num_cr = 0;
    int nl_offset = 0;
    if (ch_ == '/') {
        Next();
        while (ch_ != '\n' && ch_ >= 0) {
            if (ch_ == '\r') num_cr++;
            Next();
        }
        next = offset_;
        if (ch_ == '\n') next++;
    } else {
        Next();
        bool done = false;
        while (ch_ >= 0) {
            int32_t ch = ch_;
            if (ch == '\r') {
                num_cr++;
            } else if (ch == '\n' && nl_offset == 0) {
                nl_offset = offset_;
            }
            Next();
            if (ch == '*' && ch_ == '/') {
                Next();
                next = offset_;
                done = true;
                break;
            }
        }
        if (!done) Error(offs, "comment not terminated");
    }
    std::string_view lit = src_.substr(static_cast<size_t>(offs), static_cast<size_t>(offset_ - offs));
    if (num_cr > 0 && lit.size() >= 2 && lit[1] == '/' && lit.back() == '\r') {
        lit.remove_suffix(1);
        num_cr--;
    }
    if (next >= 0 && (lit[1] == '*' || offs == line_offset_) && lit.substr(2, 5) == "line ") {
        UpdateLineInfo(offs, lit);
    }
    std::string out = num_cr > 0 ? StripCR(lit, lit[1] == '*') : std::string(lit);
    return {out, nl_offset};
}

// trailingDigits: (index after the last ':', value, ok).
struct Trailing {
    size_t i = 0;
    uint64_t n = 0;
    bool ok = false;
};
Trailing TrailingDigits(std::string_view text) {
    Trailing t;
    size_t c = text.rfind(':');
    if (c == std::string_view::npos) return t;
    t.i = c + 1;
    std::string_view d = text.substr(c + 1);
    if (d.empty()) return t;
    uint64_t v = 0;
    for (char ch : d) {
        if (ch < '0' || ch > '9') return t;
        uint64_t nv = v * 10 + static_cast<uint64_t>(ch - '0');
        if (v > (UINT64_MAX - 9) / 10 && nv / 10 != v) return t;
        v = nv;
    }
    t.n = v;
    t.ok = true;
    return t;
}

void Scanner::UpdateLineInfo(int offs, std::string_view text) {
    if (text[1] == '*') text = text.substr(0, text.size() - 2);
    text = text.substr(7);
    offs += 7;
    Trailing a = TrailingDigits(text);
    size_t i = a.i;
    if (i == 0) return;
    if (!a.ok) {
        Error(offs + static_cast<int>(i), "invalid line number: " + std::string(text.substr(i)));
        return;
    }
    constexpr uint64_t kMaxLineCol = 1u << 30;
    uint64_t line = 0, col = 0;
    Trailing b = TrailingDigits(text.substr(0, i - 1));
    if (b.ok) {
        size_t i2 = i;
        i = b.i;
        line = b.n;
        col = a.n;
        if (col == 0 || col > kMaxLineCol) {
            Error(offs + static_cast<int>(i2), "invalid column number: " + std::string(text.substr(i2)));
            return;
        }
        text = text.substr(0, i2 - 1);
    } else {
        line = a.n;
    }
    if (line == 0 || line > kMaxLineCol) {
        Error(offs + static_cast<int>(i), "invalid line number: " + std::string(text.substr(i)));
    }
}

std::string Scanner::ScanIdentifier() {
    int offs = offset_;
    const int n = static_cast<int>(src_.size());
    for (int k = rd_offset_; k < n; k++) {
        unsigned char b = static_cast<unsigned char>(src_[static_cast<size_t>(k)]);
        if (('a' <= b && b <= 'z') || ('A' <= b && b <= 'Z') || b == '_' || ('0' <= b && b <= '9')) continue;
        rd_offset_ = k;
        if (0 < b && b < 0x80) {
            ch_ = b;
            offset_ = rd_offset_;
            rd_offset_++;
            return Slice(offs, offset_);
        }
        Next();
        while (IsLetter(ch_) || IsDigit(ch_)) Next();
        return Slice(offs, offset_);
    }
    offset_ = n;
    rd_offset_ = n;
    ch_ = kEof;
    return Slice(offs, offset_);
}

int Scanner::Digits(int base, int *invalid) {
    int digsep = 0;
    if (base <= 10) {
        int32_t max = '0' + base;
        while (IsDecimal(ch_) || ch_ == '_') {
            int ds = 1;
            if (ch_ == '_') {
                ds = 2;
            } else if (ch_ >= max && invalid && *invalid < 0) {
                *invalid = offset_;
            }
            digsep |= ds;
            Next();
        }
    } else {
        while (IsHex(ch_) || ch_ == '_') {
            digsep |= ch_ == '_' ? 2 : 1;
            Next();
        }
    }
    return digsep;
}

Tok Scanner::ScanNumber(std::string *lit) {
    int offs = offset_;
    Tok tok = Tok::ILLEGAL;
    int base = 10;
    int32_t prefix = 0;
    int digsep = 0;
    int invalid = -1;
    if (ch_ != '.') {
        tok = Tok::INT;
        if (ch_ == '0') {
            Next();
            switch (Lower(ch_)) {
            case 'x': Next(); base = 16; prefix = 'x'; break;
            case 'o': Next(); base = 8; prefix = 'o'; break;
            case 'b': Next(); base = 2; prefix = 'b'; break;
            default: base = 8; prefix = '0'; digsep = 1; break;
            }
        }
        digsep |= Digits(base, &invalid);
    }
    if (ch_ == '.') {
        tok = Tok::FLOAT;
        if (prefix == 'o' || prefix == 'b') Error(offset_, "invalid radix point in " + LitName(prefix));
        Next();
        digsep |= Digits(base, &invalid);
    }
    if ((digsep & 1) == 0) Error(offset_, LitName(prefix) + " has no digits");
    int32_t e = Lower(ch_);
    if (e == 'e' || e == 'p') {
        if (e == 'e' && prefix != 0 && prefix != '0') {
            Error(offset_, QuoteRune(static_cast<uint32_t>(ch_)) + " exponent requires decimal mantissa");
        } else if (e == 'p' && prefix != 'x') {
            Error(offset_, QuoteRune(static_cast<uint32_t>(ch_)) + " exponent requires hexadecimal mantissa");
        }
        Next();
        tok = Tok::FLOAT;
        if (ch_ == '+' || ch_ == '-') Next();
        int ds = Digits(10, nullptr);
        digsep |= ds;
        if ((ds & 1) == 0) Error(offset_, "exponent has no digits");
    } else if (prefix == 'x' && tok == Tok::FLOAT) {
        Error(offset_, "hexadecimal mantissa requires a 'p' exponent");
    }
    if (ch_ == 'i') {
        tok = Tok::IMAG;
        Next();
    }
    *lit = Slice(offs, offset_);
    if (tok == Tok::INT && invalid >= 0) {
        Error(invalid, "invalid digit " + QuoteRune(static_cast<unsigned char>((*lit)[static_cast<size_t>(invalid - offs)])) +
                           " in " + LitName(prefix));
    }
    if (digsep & 2) {
        int i = InvalidSep(*lit);
        if (i >= 0) Error(offs + i, "'_' must separate successive digits");
    }
    return tok;
}

bool Scanner::ScanEscape(int32_t quote) {
    int offs = offset_;
    int n = 0;
    uint32_t base = 0, max = 0;
    switch (ch_) {
    case 'a': case 'b': case 'f': case 'n': case 'r': case 't': case 'v': case '\\':
        Next();
        return true;
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7':
        n = 3, base = 8, max = 255;
        break;
    case 'x': Next(); n = 2, base = 16, max = 255; break;
    case 'u': Next(); n = 4, base = 16, max = 0x10FFFF; break;
    case 'U': Next(); n = 8, base = 16, max = 0x10FFFF; break;
    default:
        if (ch_ == quote) {
            Next();
            return true;
        }
        Error(offs, ch_ < 0 ? "escape sequence not terminated" : "unknown escape sequence");
        return false;
    }
    uint32_t x = 0;
    for (; n > 0; n--) {
        uint32_t d = static_cast<uint32_t>(DigitVal(ch_));
        if (d >= base) {
            Error(offset_, ch_ < 0 ? std::string("escape sequence not terminated")
                                   : "illegal character " + FormatU(ch_) + " in escape sequence");
            return false;
        }
        x = x * base + d;
        Next();
    }
    if (x > max || (0xD800 <= x && x < 0xE000)) {
        Error(offs, "escape sequence is invalid Unicode code point");
        return false;
    }
    return true;
}

std::string Scanner::ScanRune() {
    int offs = offset_ - 1;
    bool valid = true;
    int n = 0;
    for (;;) {
        int32_t ch = ch_;
        if (ch == '\n' || ch < 0) {
            if (valid) {
                Error(offs, "rune literal not terminated");
                valid = false;
            }
            break;
        }
        Next();
        if (ch == '\'') break;
        n++;
        if (ch == '\\' && !ScanEscape('\'')) valid = false;
    }
    if (valid && n != 1) Error(offs, "illegal rune literal");
    return Slice(offs, offset_);
}

std::string Scanner::ScanString() {
    int offs = offset_ - 1;
    for (;;) {
        int32_t ch = ch_;
        if (ch == '\n' || ch < 0) {
            Error(offs, "string literal not terminated");
            break;
        }
        Next();
        if (ch == '"') break;
        if (ch == '\\') ScanEscape('"');
    }
    return Slice(offs, offset_);
}

std::pair<std::string, int> Scanner::ScanRawString() {
    int offs = offset_ - 1;
    bool has_cr = false;
    for (;;) {
        int32_t ch = ch_;
        if (ch < 0) {
            Error(offs, "raw string literal not terminated");
            break;
        }
        Next();
        if (ch == '`') break;
        if (ch == '\r') has_cr = true;
    }
    std::string_view lit = src_.substr(static_cast<size_t>(offs), static_cast<size_t>(offset_ - offs));
    int raw_len = static_cast<int>(lit.size());
    return {has_cr ? StripCR(lit, false) : std::string(lit), raw_len};
}

Tok Scanner::Scan(Pos *pos_out, std::string *lit, Pos *string_end) {
    lit->clear();
    for (;;) {
        if (IsValid(nl_pos_)) {
            *pos_out = nl_pos_;
            *lit = "\n";
            nl_pos_ = NoPos;
            return Tok::SEMICOLON;
        }
        SkipWhitespace();
        Pos pos = offset_ + 1;
        *pos_out = pos;
        bool insert_semi = false;
        Tok tok = Tok::ILLEGAL;
        int32_t ch = ch_;
        if (IsLetter(ch)) {
            *lit = ScanIdentifier();
            if (lit->size() > 1) {
                tok = Lookup(*lit);
                if (tok == Tok::IDENT || tok == Tok::BREAK || tok == Tok::CONTINUE || tok == Tok::FALLTHROUGH ||
                    tok == Tok::RETURN) {
                    insert_semi = true;
                }
            } else {
                insert_semi = true;
                tok = Tok::IDENT;
            }
        } else if (IsDecimal(ch) || (ch == '.' && IsDecimal(Peek()))) {
            insert_semi = true;
            tok = ScanNumber(lit);
        } else {
            Next();
            switch (ch) {
            case kEof:
                if (insert_semi_) {
                    insert_semi_ = false;
                    *lit = "\n";
                    return Tok::SEMICOLON;
                }
                tok = Tok::EOF_;
                break;
            case '\n':
                insert_semi_ = false;
                *lit = "\n";
                return Tok::SEMICOLON;
            case '"':
                insert_semi = true;
                tok = Tok::STRING;
                *lit = ScanString();
                *string_end = pos + static_cast<int>(lit->size());
                break;
            case '\'':
                insert_semi = true;
                tok = Tok::CHAR;
                *lit = ScanRune();
                break;
            case '`': {
                insert_semi = true;
                tok = Tok::STRING;
                auto [s, raw_len] = ScanRawString();
                *lit = std::move(s);
                *string_end = pos + raw_len;
                break;
            }
            case ':': tok = Switch2(Tok::COLON, Tok::DEFINE); break;
            case '.':
                tok = Tok::PERIOD;
                if (ch_ == '.' && Peek() == '.') {
                    Next();
                    Next();
                    tok = Tok::ELLIPSIS;
                }
                break;
            case ',': tok = Tok::COMMA; break;
            case ';': tok = Tok::SEMICOLON; *lit = ";"; break;
            case '(': tok = Tok::LPAREN; break;
            case ')': insert_semi = true; tok = Tok::RPAREN; break;
            case '[': tok = Tok::LBRACK; break;
            case ']': insert_semi = true; tok = Tok::RBRACK; break;
            case '{': tok = Tok::LBRACE; break;
            case '}': insert_semi = true; tok = Tok::RBRACE; break;
            case '+':
                tok = Switch3(Tok::ADD, Tok::ADD_ASSIGN, '+', Tok::INC);
                if (tok == Tok::INC) insert_semi = true;
                break;
            case '-':
                tok = Switch3(Tok::SUB, Tok::SUB_ASSIGN, '-', Tok::DEC);
                if (tok == Tok::DEC) insert_semi = true;
                break;
            case '*': tok = Switch2(Tok::MUL, Tok::MUL_ASSIGN); break;
            case '/':
                if (ch_ == '/' || ch_ == '*') {
                    auto [comment, nl_offset] = ScanComment();
                    if (insert_semi_ && nl_offset != 0) {
                        nl_pos_ = nl_offset + 1;
                        insert_semi_ = false;
                    } else {
                        insert_semi = insert_semi_;
                    }
                    tok = Tok::COMMENT;
                    *lit = std::move(comment);
                } else {
                    tok = Switch2(Tok::QUO, Tok::QUO_ASSIGN);
                }
                break;
            case '%': tok = Switch2(Tok::REM, Tok::REM_ASSIGN); break;
            case '^': tok = Switch2(Tok::XOR, Tok::XOR_ASSIGN); break;
            case '<':
                if (ch_ == '-') {
                    Next();
                    tok = Tok::ARROW;
                } else {
                    tok = Switch4(Tok::LSS, Tok::LEQ, '<', Tok::SHL, Tok::SHL_ASSIGN);
                }
                break;
            case '>': tok = Switch4(Tok::GTR, Tok::GEQ, '>', Tok::SHR, Tok::SHR_ASSIGN); break;
            case '=': tok = Switch2(Tok::ASSIGN, Tok::EQL); break;
            case '!': tok = Switch2(Tok::NOT, Tok::NEQ); break;
            case '&':
                if (ch_ == '^') {
                    Next();
                    tok = Switch2(Tok::AND_NOT, Tok::AND_NOT_ASSIGN);
                } else {
                    tok = Switch3(Tok::AND, Tok::AND_ASSIGN, '&', Tok::LAND);
                }
                break;
            case '|': tok = Switch3(Tok::OR, Tok::OR_ASSIGN, '|', Tok::LOR); break;
            case '~': tok = Tok::TILDE; break;
            default:
                if (ch != kBom) {
                    if (ch == 0x201C || ch == 0x201D) {
                        Error(pos - 1, "curly quotation mark " + QuoteRune(static_cast<uint32_t>(ch)) + " (use neutral '\"')");
                    } else {
                        Error(pos - 1, "illegal character " + FormatU(ch));
                    }
                }
                insert_semi = insert_semi_;
                tok = Tok::ILLEGAL;
                AppendRune(*lit, static_cast<uint32_t>(ch));
                break;
            }
        }
        insert_semi_ = insert_semi;
        return tok;
    }
}

// --- parser ------------------------------------------------------------------

struct Bailout {};

enum : int { kBasic, kLabelOk, kRangeOk };

bool InSet(const uint64_t *set, Tok t) {
    auto i = static_cast<unsigned>(t);
    return (set[i / 64] >> (i % 64)) & 1u;
}

struct TokSetInit {
    uint64_t bits[2] = {0, 0};
    TokSetInit(std::initializer_list<Tok> toks) {
        for (Tok t : toks) {
            auto i = static_cast<unsigned>(t);
            bits[i / 64] |= uint64_t{1} << (i % 64);
        }
    }
};

const TokSetInit kStmtStart{Tok::BREAK, Tok::CONST, Tok::CONTINUE, Tok::DEFER, Tok::FALLTHROUGH, Tok::FOR, Tok::GO,
                            Tok::GOTO, Tok::IF, Tok::RETURN, Tok::SELECT, Tok::SWITCH, Tok::TYPE, Tok::VAR};
const TokSetInit kDeclStart{Tok::IMPORT, Tok::CONST, Tok::TYPE, Tok::VAR};
const TokSetInit kExprEnd{Tok::COMMA, Tok::COLON, Tok::SEMICOLON, Tok::RPAREN, Tok::RBRACK, Tok::RBRACE};

struct ParField {
    Ident *name = nullptr;
    Node *typ = nullptr;
};

class Parser {
public:
    Parser(std::string_view src, const Options &opts, Arena *arena, const std::vector<int> *lines)
        : opts_(opts), a_(arena), lines_(lines), size_(static_cast<int>(src.size())) {
        scanner_.Init(src, &Parser::ScanError, this);
    }

    File *ParseFile();
    std::vector<Error> errors;

private:
    static void ScanError(void *ctx, int offset, const std::string &msg) {
        static_cast<Parser *>(ctx)->AddError(offset, msg);
    }
    void AddError(int offset, const std::string &msg) {
        offset = std::clamp(offset, 0, size_);  // go/token's fixOffset: a position past EOF reports as EOF
        Error e;
        e.offset = offset;
        e.line = LineOfOffset(*lines_, offset);
        e.col = offset - (*lines_)[static_cast<size_t>(e.line - 1)] + 1;
        e.msg = msg;
        errors.push_back(std::move(e));
    }

    template <class T> T *New() { return a_->New<T>(); }

    int LineFor(Pos p) const { return LineOfOffset(*lines_, p - 1); }

    void IncNest() {
        if (++nest_lev_ > opts_.max_nest) {
            Err(pos_, "exceeded max nesting depth");
            throw Bailout{};
        }
    }
    struct NestGuard {
        Parser *p;
        explicit NestGuard(Parser *pp) : p(pp) { p->IncNest(); }
        ~NestGuard() { p->nest_lev_--; }
        NestGuard(const NestGuard &) = delete;
        NestGuard &operator=(const NestGuard &) = delete;
    };

    void Next0();
    std::pair<Comment *, int> ConsumeComment();
    std::pair<CommentGroup *, int> ConsumeCommentGroup(int n);
    void Next();
    void Err(Pos pos, const std::string &msg);
    void ErrorExpected(Pos pos, std::string msg);
    Pos Expect(Tok tok);
    Pos Expect2(Tok tok);
    Pos ExpectClosing(Tok tok, const std::string &context);
    CommentGroup *ExpectSemi();
    bool AtComma(const std::string &context, Tok follow);
    void Advance(const TokSetInit &to);

    Ident *ParseIdent();
    std::vector<Ident *> ParseIdentList();
    std::vector<Node *> ParseExprList();
    std::vector<Node *> ParseList(bool in_rhs);
    Node *ParseType();
    Node *ParseQualifiedIdent(Ident *ident);
    Node *ParseTypeName(Ident *ident);
    ArrayType *ParseArrayType(Pos lbrack, Node *len);
    std::pair<Ident *, Node *> ParseArrayFieldOrTypeInstance(Ident *x);
    Field *ParseFieldDecl();
    StructType *ParseStructType();
    StarExpr *ParsePointerType();
    Ellipsis *ParseDotsType();
    ParField ParseParamDecl(Ident *name, bool type_sets_ok);
    std::vector<Field *> ParseParameterList(Ident *name0, Node *typ0, Tok closing, bool dddok);
    FieldList *ParseTypeParameters();
    FieldList *ParseParameters(bool result);
    FuncType *ParseFuncType();
    Field *ParseMethodSpec();
    Node *EmbeddedElem(Node *x);
    Node *EmbeddedTerm();
    InterfaceType *ParseInterfaceType();
    MapType *ParseMapType();
    ChanType *ParseChanType();
    Node *ParseTypeInstance(Node *typ);
    Node *TryIdentOrType();
    std::vector<Node *> ParseStmtList();
    BlockStmt *ParseBody();
    BlockStmt *ParseBlockStmt() { return ParseBody(); }
    Node *ParseFuncTypeOrLit();
    Node *ParseOperand();
    Node *ParseSelector(Node *x);
    Node *ParseTypeAssertion(Node *x);
    Node *ParseIndexOrSliceOrInstance(Node *x);
    CallExpr *ParseCallOrConversion(Node *fun);
    Node *ParseValue();
    Node *ParseElement();
    std::vector<Node *> ParseElementList();
    Node *ParseLiteralValue(Node *typ);
    Node *ParsePrimaryExpr(Node *x);
    Node *ParseUnaryExpr();
    Node *ParseBinaryExpr(Node *x, int prec1);
    Node *ParseExpr() { return ParseBinaryExpr(nullptr, LowestPrec + 1); }
    Node *ParseRhs();
    std::pair<Node *, bool> ParseSimpleStmt(int mode);
    CallExpr *ParseCallExpr(const char *call_type);
    Node *ParseGoStmt();
    Node *ParseDeferStmt();
    Node *ParseReturnStmt();
    Node *ParseBranchStmt(Tok tok);
    Node *MakeExpr(Node *s, const std::string &want);
    void ParseIfHeader(Node **init, Node **cond);
    IfStmt *ParseIfStmt();
    CaseClause *ParseCaseClause();
    bool IsTypeSwitchGuard(Node *s);
    Node *ParseSwitchStmt();
    CommClause *ParseCommClause();
    Node *ParseSelectStmt();
    Node *ParseForStmt();
    Node *ParseStmt();
    Node *ParseImportSpec(CommentGroup *doc, Tok keyword, int iota);
    Node *ParseValueSpec(CommentGroup *doc, Tok keyword, int iota);
    void ParseGenericType(TypeSpec *spec, Pos open_pos, Ident *name0, Node *typ0);
    Node *ParseTypeSpec(CommentGroup *doc, Tok keyword, int iota);
    std::pair<Ident *, Node *> ExtractName(Node *x, bool force);
    GenDecl *ParseGenDecl(Tok keyword, Node *(Parser::*f)(CommentGroup *, Tok, int));
    FuncDecl *ParseFuncDecl();
    Node *ParseDecl(const TokSetInit &sync);
    Node *PackIndexExpr(Node *x, Pos lbrack, std::vector<Node *> exprs, Pos rbrack);

    BadExpr *Bad(Pos from, Pos to) {
        auto *b = New<BadExpr>();
        b->From = from;
        b->To = to;
        return b;
    }

    Options opts_;
    Arena *a_;
    const std::vector<int> *lines_;
    int size_ = 0;
    Scanner scanner_;
    std::vector<CommentGroup *> comments_;
    CommentGroup *lead_comment_ = nullptr;
    CommentGroup *line_comment_ = nullptr;
    Pos pos_ = NoPos;
    Tok tok_ = Tok::ILLEGAL;
    std::string lit_;
    Pos string_end_ = NoPos;
    Pos sync_pos_ = NoPos;
    int sync_cnt_ = 0;
    int expr_lev_ = 0;
    bool in_rhs_ = false;
    std::vector<ImportSpec *> imports_;
    int nest_lev_ = 0;


public:
    void Init() { Next(); }
};

void Parser::Next0() {
    for (;;) {
        Pos se = NoPos;
        tok_ = scanner_.Scan(&pos_, &lit_, &se);
        if (tok_ == Tok::COMMENT) {
            if (!opts_.parse_comments) continue;
        } else if (tok_ == Tok::STRING) {
            string_end_ = se;
        }
        break;
    }
}

std::pair<Comment *, int> Parser::ConsumeComment() {
    int endline = LineFor(pos_);
    if (lit_[1] == '*') {
        for (char c : lit_) {
            if (c == '\n') endline++;
        }
    }
    auto *c = New<Comment>();
    c->Slash = pos_;
    c->Text = lit_;
    Next0();
    return {c, endline};
}

std::pair<CommentGroup *, int> Parser::ConsumeCommentGroup(int n) {
    auto *g = New<CommentGroup>();
    int endline = LineFor(pos_);
    while (tok_ == Tok::COMMENT && LineFor(pos_) <= endline + n) {
        auto [c, e] = ConsumeComment();
        endline = e;
        g->List.push_back(c);
    }
    comments_.push_back(g);
    return {g, endline};
}

void Parser::Next() {
    lead_comment_ = nullptr;
    line_comment_ = nullptr;
    Pos prev = pos_;
    Next0();
    if (tok_ == Tok::COMMENT) {
        CommentGroup *comment = nullptr;
        int endline = 0;
        if (LineFor(pos_) == LineFor(prev)) {
            std::tie(comment, endline) = ConsumeCommentGroup(0);
            if (LineFor(pos_) != endline || tok_ == Tok::SEMICOLON || tok_ == Tok::EOF_) line_comment_ = comment;
        }
        endline = -1;
        while (tok_ == Tok::COMMENT) std::tie(comment, endline) = ConsumeCommentGroup(1);
        if (endline + 1 == LineFor(pos_)) lead_comment_ = comment;
    }
}

void Parser::Err(Pos pos, const std::string &msg) {
    int offset = pos - 1;
    if (!opts_.all_errors) {
        size_t n = errors.size();
        if (n > 0 && errors[n - 1].line == LineOfOffset(*lines_, offset)) return;
        if (n > 10) throw Bailout{};
    }
    AddError(offset, msg);
}

void Parser::ErrorExpected(Pos pos, std::string msg) {
    msg = "expected " + msg;
    if (pos == pos_) {
        if (tok_ == Tok::SEMICOLON && lit_ == "\n") {
            msg += ", found newline";
        } else if (IsLiteral(tok_)) {
            msg += ", found " + lit_;
        } else {
            msg += ", found '";
            msg += TokString(tok_);
            msg += "'";
        }
    }
    Err(pos, msg);
}

Pos Parser::Expect(Tok tok) {
    Pos pos = pos_;
    if (tok_ != tok) ErrorExpected(pos, std::string("'") + TokString(tok) + "'");
    Next();
    return pos;
}

Pos Parser::Expect2(Tok tok) {
    Pos pos = NoPos;
    if (tok_ == tok) {
        pos = pos_;
    } else {
        ErrorExpected(pos_, std::string("'") + TokString(tok) + "'");
    }
    Next();
    return pos;
}

Pos Parser::ExpectClosing(Tok tok, const std::string &context) {
    if (tok_ != tok && tok_ == Tok::SEMICOLON && lit_ == "\n") {
        Err(pos_, "missing ',' before newline in " + context);
        Next();
    }
    return Expect(tok);
}

CommentGroup *Parser::ExpectSemi() {
    switch (tok_) {
    case Tok::RPAREN:
    case Tok::RBRACE:
        return nullptr;
    case Tok::COMMA:
        ErrorExpected(pos_, "';'");
        [[fallthrough]];
    case Tok::SEMICOLON: {
        CommentGroup *comment = nullptr;
        if (lit_ == ";") {
            Next();
            comment = line_comment_;
        } else {
            comment = line_comment_;
            Next();
        }
        return comment;
    }
    default:
        ErrorExpected(pos_, "';'");
        Advance(kStmtStart);
        return nullptr;
    }
}

bool Parser::AtComma(const std::string &context, Tok follow) {
    if (tok_ == Tok::COMMA) return true;
    if (tok_ != follow) {
        std::string msg = "missing ','";
        if (tok_ == Tok::SEMICOLON && lit_ == "\n") msg += " before newline";
        Err(pos_, msg + " in " + context);
        return true;
    }
    return false;
}

void Parser::Advance(const TokSetInit &to) {
    for (; tok_ != Tok::EOF_; Next()) {
        if (InSet(to.bits, tok_)) {
            if (pos_ == sync_pos_ && sync_cnt_ < 10) {
                sync_cnt_++;
                return;
            }
            if (pos_ > sync_pos_) {
                sync_pos_ = pos_;
                sync_cnt_ = 0;
                return;
            }
        }
    }
}

Ident *Parser::ParseIdent() {
    Pos pos = pos_;
    std::string name = "_";
    if (tok_ == Tok::IDENT) {
        name = lit_;
        Next();
    } else {
        Expect(Tok::IDENT);
    }
    auto *id = New<Ident>();
    id->NamePos = pos;
    id->Name = std::move(name);
    return id;
}

std::vector<Ident *> Parser::ParseIdentList() {
    std::vector<Ident *> list{ParseIdent()};
    while (tok_ == Tok::COMMA) {
        Next();
        list.push_back(ParseIdent());
    }
    return list;
}

std::vector<Node *> Parser::ParseExprList() {
    std::vector<Node *> list{ParseExpr()};
    while (tok_ == Tok::COMMA) {
        Next();
        list.push_back(ParseExpr());
    }
    return list;
}

std::vector<Node *> Parser::ParseList(bool in_rhs) {
    bool old = in_rhs_;
    in_rhs_ = in_rhs;
    auto list = ParseExprList();
    in_rhs_ = old;
    return list;
}

Node *Parser::ParseType() {
    Node *typ = TryIdentOrType();
    if (!typ) {
        Pos pos = pos_;
        ErrorExpected(pos, "type");
        Advance(kExprEnd);
        return Bad(pos, pos_);
    }
    return typ;
}

Node *Parser::ParseQualifiedIdent(Ident *ident) {
    Node *typ = ParseTypeName(ident);
    if (tok_ == Tok::LBRACK) typ = ParseTypeInstance(typ);
    return typ;
}

Node *Parser::ParseTypeName(Ident *ident) {
    if (!ident) ident = ParseIdent();
    if (tok_ == Tok::PERIOD) {
        Next();
        auto *s = New<SelectorExpr>();
        s->X = ident;
        s->Sel = ParseIdent();
        return s;
    }
    return ident;
}

ArrayType *Parser::ParseArrayType(Pos lbrack, Node *len) {
    if (!len) {
        expr_lev_++;
        if (tok_ == Tok::ELLIPSIS) {
            auto *e = New<Ellipsis>();
            e->Ellipsis_ = pos_;
            len = e;
            Next();
        } else if (tok_ != Tok::RBRACK) {
            len = ParseRhs();
        }
        expr_lev_--;
    }
    if (tok_ == Tok::COMMA) {
        Err(pos_, "unexpected comma; expecting ]");
        Next();
    }
    Expect(Tok::RBRACK);
    auto *t = New<ArrayType>();
    t->Lbrack = lbrack;
    t->Len = len;
    t->Elt = ParseType();
    return t;
}

std::pair<Ident *, Node *> Parser::ParseArrayFieldOrTypeInstance(Ident *x) {
    Pos lbrack = Expect(Tok::LBRACK);
    Pos trailing_comma = NoPos;
    std::vector<Node *> args;
    if (tok_ != Tok::RBRACK) {
        expr_lev_++;
        args.push_back(ParseRhs());
        while (tok_ == Tok::COMMA) {
            Pos comma = pos_;
            Next();
            if (tok_ == Tok::RBRACK) {
                trailing_comma = comma;
                break;
            }
            args.push_back(ParseRhs());
        }
        expr_lev_--;
    }
    Pos rbrack = Expect(Tok::RBRACK);
    if (args.empty()) {
        auto *t = New<ArrayType>();
        t->Lbrack = lbrack;
        t->Elt = ParseType();
        return {x, t};
    }
    if (args.size() == 1) {
        Node *elt = TryIdentOrType();
        if (elt) {
            if (IsValid(trailing_comma)) Err(trailing_comma, "unexpected comma; expecting ]");
            auto *t = New<ArrayType>();
            t->Lbrack = lbrack;
            t->Len = args[0];
            t->Elt = elt;
            return {x, t};
        }
    }
    return {nullptr, PackIndexExpr(x, lbrack, std::move(args), rbrack)};
}

Field *Parser::ParseFieldDecl() {
    CommentGroup *doc = lead_comment_;
    std::vector<Ident *> names;
    Node *typ = nullptr;
    switch (tok_) {
    case Tok::IDENT: {
        Ident *name = ParseIdent();
        if (tok_ == Tok::PERIOD || tok_ == Tok::STRING || tok_ == Tok::SEMICOLON || tok_ == Tok::RBRACE) {
            typ = name;
            if (tok_ == Tok::PERIOD) typ = ParseQualifiedIdent(name);
        } else {
            names = {name};
            while (tok_ == Tok::COMMA) {
                Next();
                names.push_back(ParseIdent());
            }
            if (names.size() == 1 && tok_ == Tok::LBRACK) {
                std::tie(name, typ) = ParseArrayFieldOrTypeInstance(name);
                if (!name) names.clear();
            } else {
                typ = ParseType();
            }
        }
        break;
    }
    case Tok::MUL: {
        Pos star = pos_;
        Next();
        if (tok_ == Tok::LPAREN) {
            Err(pos_, "cannot parenthesize embedded type");
            Next();
            typ = ParseQualifiedIdent(nullptr);
            if (tok_ == Tok::RPAREN) Next();
        } else {
            typ = ParseQualifiedIdent(nullptr);
        }
        auto *s = New<StarExpr>();
        s->Star = star;
        s->X = typ;
        typ = s;
        break;
    }
    case Tok::LPAREN:
        Err(pos_, "cannot parenthesize embedded type");
        Next();
        if (tok_ == Tok::MUL) {
            Pos star = pos_;
            Next();
            auto *s = New<StarExpr>();
            s->Star = star;
            s->X = ParseQualifiedIdent(nullptr);
            typ = s;
        } else {
            typ = ParseQualifiedIdent(nullptr);
        }
        if (tok_ == Tok::RPAREN) Next();
        break;
    default: {
        Pos pos = pos_;
        ErrorExpected(pos, "field name or embedded type");
        Advance(kExprEnd);
        typ = Bad(pos, pos_);
        break;
    }
    }
    BasicLit *tag = nullptr;
    if (tok_ == Tok::STRING) {
        tag = New<BasicLit>();
        tag->ValuePos = pos_;
        tag->ValueEnd = string_end_;
        tag->Kind = tok_;
        tag->Value = lit_;
        Next();
    }
    CommentGroup *comment = ExpectSemi();
    auto *f = New<Field>();
    f->Doc = doc;
    f->Names = std::move(names);
    f->Type = typ;
    f->Tag = tag;
    f->Comment = comment;
    return f;
}

StructType *Parser::ParseStructType() {
    Pos pos = Expect(Tok::STRUCT);
    Pos lbrace = Expect(Tok::LBRACE);
    auto *fl = New<FieldList>();
    fl->Opening = lbrace;
    while (tok_ == Tok::IDENT || tok_ == Tok::MUL || tok_ == Tok::LPAREN) fl->List.push_back(ParseFieldDecl());
    fl->Closing = Expect(Tok::RBRACE);
    auto *t = New<StructType>();
    t->Struct = pos;
    t->Fields = fl;
    return t;
}

StarExpr *Parser::ParsePointerType() {
    auto *s = New<StarExpr>();
    s->Star = Expect(Tok::MUL);
    s->X = ParseType();
    return s;
}

Ellipsis *Parser::ParseDotsType() {
    auto *e = New<Ellipsis>();
    e->Ellipsis_ = Expect(Tok::ELLIPSIS);
    e->Elt = ParseType();
    return e;
}

ParField Parser::ParseParamDecl(Ident *name, bool type_sets_ok) {
    ParField f;
    Tok ptok = tok_;
    if (name) {
        tok_ = Tok::IDENT;
    } else if (type_sets_ok && tok_ == Tok::TILDE) {
        f.typ = EmbeddedElem(nullptr);
        return f;
    }
    switch (tok_) {
    case Tok::IDENT:
        if (name) {
            f.name = name;
            tok_ = ptok;
        } else {
            f.name = ParseIdent();
        }
        switch (tok_) {
        case Tok::IDENT: case Tok::MUL: case Tok::ARROW: case Tok::FUNC: case Tok::CHAN: case Tok::MAP:
        case Tok::STRUCT: case Tok::INTERFACE: case Tok::LPAREN:
            f.typ = ParseType();
            break;
        case Tok::LBRACK:
            std::tie(f.name, f.typ) = ParseArrayFieldOrTypeInstance(f.name);
            break;
        case Tok::ELLIPSIS:
            f.typ = ParseDotsType();
            return f;
        case Tok::PERIOD:
            f.typ = ParseQualifiedIdent(f.name);
            f.name = nullptr;
            break;
        case Tok::TILDE:
            if (type_sets_ok) {
                f.typ = EmbeddedElem(nullptr);
                return f;
            }
            break;
        case Tok::OR:
            if (type_sets_ok) {
                f.typ = EmbeddedElem(f.name);
                f.name = nullptr;
                return f;
            }
            break;
        default: break;
        }
        break;
    case Tok::MUL: case Tok::ARROW: case Tok::FUNC: case Tok::LBRACK: case Tok::CHAN: case Tok::MAP: case Tok::STRUCT:
    case Tok::INTERFACE: case Tok::LPAREN:
        f.typ = ParseType();
        break;
    case Tok::ELLIPSIS:
        f.typ = ParseDotsType();
        return f;
    default:
        ErrorExpected(pos_, "')'");
        Advance(kExprEnd);
        break;
    }
    if (type_sets_ok && tok_ == Tok::OR && f.typ) f.typ = EmbeddedElem(f.typ);
    return f;
}

std::vector<Field *> Parser::ParseParameterList(Ident *name0, Node *typ0, Tok closing, bool dddok) {
    std::vector<Field *> params;
    const bool tparams = closing == Tok::RBRACK;
    Pos pos0 = pos_;
    if (name0) {
        pos0 = NodePos(name0);
    } else if (typ0) {
        pos0 = NodePos(typ0);
    }
    std::vector<ParField> list;
    int named = 0, typed = 0;
    while (name0 || (tok_ != closing && tok_ != Tok::EOF_)) {
        ParField par;
        if (typ0) {
            if (tparams) typ0 = EmbeddedElem(typ0);
            par = {name0, typ0};
        } else {
            par = ParseParamDecl(name0, tparams);
        }
        name0 = nullptr;
        typ0 = nullptr;
        if (par.name || par.typ) {
            list.push_back(par);
            if (par.name && par.typ) named++;
            if (par.typ) typed++;
        }
        if (!AtComma("parameter list", closing)) break;
        Next();
    }
    if (list.empty()) return params;

    if (named == 0) {
        for (auto &par : list) {
            if (par.name) {
                par.typ = par.name;
                par.name = nullptr;
            }
        }
        if (tparams) {
            Pos err_pos;
            std::string msg;
            if (named == typed) {
                err_pos = pos_;
                msg = "missing type constraint";
            } else {
                err_pos = pos0;
                msg = "missing type parameter name";
                if (list.size() == 1) msg += " or invalid array length";
            }
            Err(err_pos, msg);
        }
    } else if (named != static_cast<int>(list.size())) {
        Pos err_pos = NoPos;
        Node *typ = nullptr;
        for (size_t i = 0; i < list.size(); i++) {
            ParField &par = list[list.size() - i - 1];
            if (par.typ) {
                typ = par.typ;
                if (!par.name) {
                    err_pos = NodePos(typ);
                    auto *n = New<Ident>();
                    n->Name = "_";
                    n->NamePos = err_pos;
                    par.name = n;
                }
            } else if (typ) {
                par.typ = typ;
            } else {
                err_pos = NodePos(par.name);
                par.typ = Bad(err_pos, pos_);
            }
        }
        if (IsValid(err_pos)) {
            std::string msg;
            if (named == typed) {
                err_pos = pos_;
                msg = tparams ? "missing type constraint" : "missing parameter type";
            } else if (tparams) {
                msg = "missing type parameter name";
                if (list.size() == 1) msg += " or invalid array length";
            } else {
                msg = "missing parameter name";
            }
            Err(err_pos, msg);
        }
    }

    bool first = true;
    for (size_t i = 0; i < list.size(); i++) {
        ParField &f = list[i];
        if (auto *t = As<Ellipsis>(f.typ); t && (!dddok || i + 1 < list.size())) {
            if (first) {
                first = false;
                Err(t->Ellipsis_, dddok ? "can only use ... with final parameter" : "invalid use of ...");
            }
            f.typ = Bad(NodePos(t), NodeEnd(t));
        }
    }

    if (named == 0) {
        for (auto &par : list) {
            auto *fld = New<Field>();
            fld->Type = par.typ;
            params.push_back(fld);
        }
        return params;
    }
    std::vector<Ident *> names;
    Node *typ = nullptr;
    auto add_params = [&] {
        auto *fld = New<Field>();
        fld->Names = std::move(names);
        fld->Type = typ;
        params.push_back(fld);
        names.clear();
    };
    for (auto &par : list) {
        if (par.typ != typ) {
            if (!names.empty()) add_params();
            typ = par.typ;
        }
        names.push_back(par.name);
    }
    if (!names.empty()) add_params();
    return params;
}

FieldList *Parser::ParseTypeParameters() {
    Pos lbrack = Expect(Tok::LBRACK);
    std::vector<Field *> list;
    if (tok_ != Tok::RBRACK) list = ParseParameterList(nullptr, nullptr, Tok::RBRACK, false);
    Pos rbrack = Expect(Tok::RBRACK);
    if (list.empty()) {
        Err(rbrack, "empty type parameter list");
        return nullptr;
    }
    auto *fl = New<FieldList>();
    fl->Opening = lbrack;
    fl->List = std::move(list);
    fl->Closing = rbrack;
    return fl;
}

FieldList *Parser::ParseParameters(bool result) {
    if (!result || tok_ == Tok::LPAREN) {
        Pos lparen = Expect(Tok::LPAREN);
        std::vector<Field *> list;
        if (tok_ != Tok::RPAREN) list = ParseParameterList(nullptr, nullptr, Tok::RPAREN, !result);
        Pos rparen = Expect(Tok::RPAREN);
        auto *fl = New<FieldList>();
        fl->Opening = lparen;
        fl->List = std::move(list);
        fl->Closing = rparen;
        return fl;
    }
    if (Node *typ = TryIdentOrType()) {
        auto *fld = New<Field>();
        fld->Type = typ;
        auto *fl = New<FieldList>();
        fl->List.push_back(fld);
        return fl;
    }
    return nullptr;
}

FuncType *Parser::ParseFuncType() {
    Pos pos = Expect(Tok::FUNC);
    if (tok_ == Tok::LBRACK) {
        FieldList *tparams = ParseTypeParameters();
        if (tparams) Err(tparams->Opening, "function type must have no type parameters");
    }
    auto *t = New<FuncType>();
    t->Func = pos;
    t->Params = ParseParameters(false);
    t->Results = ParseParameters(true);
    return t;
}

Field *Parser::ParseMethodSpec() {
    CommentGroup *doc = lead_comment_;
    std::vector<Ident *> idents;
    Node *typ = nullptr;
    Node *x = ParseTypeName(nullptr);
    if (auto *ident = As<Ident>(x)) {
        if (tok_ == Tok::LBRACK) {
            Pos lbrack = pos_;
            Next();
            expr_lev_++;
            Node *x2 = ParseExpr();
            expr_lev_--;
            auto *name0 = As<Ident>(x2);
            if (name0 && tok_ != Tok::COMMA && tok_ != Tok::RBRACK) {
                (void)ParseParameterList(name0, nullptr, Tok::RBRACK, false);
                (void)Expect(Tok::RBRACK);
                Err(lbrack, "interface method must have no type parameters");
                auto *ft = New<FuncType>();
                ft->Params = ParseParameters(false);
                ft->Results = ParseParameters(true);
                idents = {ident};
                typ = ft;
            } else {
                std::vector<Node *> list{x2};
                if (AtComma("type argument list", Tok::RBRACK)) {
                    expr_lev_++;
                    Next();
                    while (tok_ != Tok::RBRACK && tok_ != Tok::EOF_) {
                        list.push_back(ParseType());
                        if (!AtComma("type argument list", Tok::RBRACK)) break;
                        Next();
                    }
                    expr_lev_--;
                }
                Pos rbrack = ExpectClosing(Tok::RBRACK, "type argument list");
                typ = PackIndexExpr(ident, lbrack, std::move(list), rbrack);
            }
        } else if (tok_ == Tok::LPAREN) {
            auto *ft = New<FuncType>();
            ft->Params = ParseParameters(false);
            ft->Results = ParseParameters(true);
            idents = {ident};
            typ = ft;
        } else {
            typ = x;
        }
    } else {
        typ = x;
        if (tok_ == Tok::LBRACK) typ = ParseTypeInstance(typ);
    }
    auto *f = New<Field>();
    f->Doc = doc;
    f->Names = std::move(idents);
    f->Type = typ;
    return f;
}

Node *Parser::EmbeddedElem(Node *x) {
    if (!x) x = EmbeddedTerm();
    while (tok_ == Tok::OR) {
        auto *t = New<BinaryExpr>();
        t->OpPos = pos_;
        t->Op = Tok::OR;
        Next();
        t->X = x;
        t->Y = EmbeddedTerm();
        x = t;
    }
    return x;
}

Node *Parser::EmbeddedTerm() {
    if (tok_ == Tok::TILDE) {
        auto *t = New<UnaryExpr>();
        t->OpPos = pos_;
        t->Op = Tok::TILDE;
        Next();
        t->X = ParseType();
        return t;
    }
    Node *t = TryIdentOrType();
    if (!t) {
        Pos pos = pos_;
        ErrorExpected(pos, "~ term or type");
        Advance(kExprEnd);
        return Bad(pos, pos_);
    }
    return t;
}

InterfaceType *Parser::ParseInterfaceType() {
    Pos pos = Expect(Tok::INTERFACE);
    Pos lbrace = Expect(Tok::LBRACE);
    auto *fl = New<FieldList>();
    fl->Opening = lbrace;
    for (;;) {
        if (tok_ == Tok::IDENT) {
            Field *f = ParseMethodSpec();
            if (f->Names.empty()) f->Type = EmbeddedElem(f->Type);
            f->Comment = ExpectSemi();
            fl->List.push_back(f);
        } else if (tok_ == Tok::TILDE) {
            Node *typ = EmbeddedElem(nullptr);
            CommentGroup *comment = ExpectSemi();
            auto *f = New<Field>();
            f->Type = typ;
            f->Comment = comment;
            fl->List.push_back(f);
        } else if (Node *t = TryIdentOrType()) {
            Node *typ = EmbeddedElem(t);
            CommentGroup *comment = ExpectSemi();
            auto *f = New<Field>();
            f->Type = typ;
            f->Comment = comment;
            fl->List.push_back(f);
        } else {
            break;
        }
    }
    fl->Closing = Expect(Tok::RBRACE);
    auto *t = New<InterfaceType>();
    t->Interface = pos;
    t->Methods = fl;
    return t;
}

MapType *Parser::ParseMapType() {
    auto *t = New<MapType>();
    t->Map = Expect(Tok::MAP);
    Expect(Tok::LBRACK);
    t->Key = ParseType();
    Expect(Tok::RBRACK);
    t->Value = ParseType();
    return t;
}

ChanType *Parser::ParseChanType() {
    Pos pos = pos_;
    int dir = SEND | RECV;
    Pos arrow = NoPos;
    if (tok_ == Tok::CHAN) {
        Next();
        if (tok_ == Tok::ARROW) {
            arrow = pos_;
            Next();
            dir = SEND;
        }
    } else {
        arrow = Expect(Tok::ARROW);
        Expect(Tok::CHAN);
        dir = RECV;
    }
    auto *t = New<ChanType>();
    t->Begin = pos;
    t->Arrow = arrow;
    t->Dir = dir;
    t->Value = ParseType();
    return t;
}

Node *Parser::ParseTypeInstance(Node *typ) {
    Pos opening = Expect(Tok::LBRACK);
    expr_lev_++;
    std::vector<Node *> list;
    while (tok_ != Tok::RBRACK && tok_ != Tok::EOF_) {
        list.push_back(ParseType());
        if (!AtComma("type argument list", Tok::RBRACK)) break;
        Next();
    }
    expr_lev_--;
    Pos closing = ExpectClosing(Tok::RBRACK, "type argument list");
    if (list.empty()) {
        ErrorExpected(closing, "type argument list");
        auto *ix = New<IndexExpr>();
        ix->X = typ;
        ix->Lbrack = opening;
        ix->Index = Bad(opening + 1, closing);
        ix->Rbrack = closing;
        return ix;
    }
    return PackIndexExpr(typ, opening, std::move(list), closing);
}

Node *Parser::TryIdentOrType() {
    NestGuard g(this);
    switch (tok_) {
    case Tok::IDENT: {
        Node *typ = ParseTypeName(nullptr);
        if (tok_ == Tok::LBRACK) typ = ParseTypeInstance(typ);
        return typ;
    }
    case Tok::LBRACK: {
        Pos lbrack = Expect(Tok::LBRACK);
        return ParseArrayType(lbrack, nullptr);
    }
    case Tok::STRUCT: return ParseStructType();
    case Tok::MUL: return ParsePointerType();
    case Tok::FUNC: return ParseFuncType();
    case Tok::INTERFACE: return ParseInterfaceType();
    case Tok::MAP: return ParseMapType();
    case Tok::CHAN: case Tok::ARROW: return ParseChanType();
    case Tok::LPAREN: {
        Pos lparen = pos_;
        Next();
        auto *p = New<ParenExpr>();
        p->Lparen = lparen;
        p->X = ParseType();
        p->Rparen = Expect(Tok::RPAREN);
        return p;
    }
    default: return nullptr;
    }
}

std::vector<Node *> Parser::ParseStmtList() {
    std::vector<Node *> list;
    while (tok_ != Tok::CASE && tok_ != Tok::DEFAULT && tok_ != Tok::RBRACE && tok_ != Tok::EOF_) {
        list.push_back(ParseStmt());
    }
    return list;
}

BlockStmt *Parser::ParseBody() {
    auto *b = New<BlockStmt>();
    b->Lbrace = Expect(Tok::LBRACE);
    b->List = ParseStmtList();
    b->Rbrace = Expect2(Tok::RBRACE);
    return b;
}

Node *Parser::ParseFuncTypeOrLit() {
    FuncType *typ = ParseFuncType();
    if (tok_ != Tok::LBRACE) return typ;
    expr_lev_++;
    BlockStmt *body = ParseBody();
    expr_lev_--;
    auto *f = New<FuncLit>();
    f->Type = typ;
    f->Body = body;
    return f;
}

Node *Parser::ParseOperand() {
    switch (tok_) {
    case Tok::IDENT: return ParseIdent();
    case Tok::INT: case Tok::FLOAT: case Tok::IMAG: case Tok::CHAR: case Tok::STRING: {
        auto *x = New<BasicLit>();
        x->ValuePos = pos_;
        x->ValueEnd = tok_ == Tok::STRING ? string_end_ : pos_ + static_cast<int>(lit_.size());
        x->Kind = tok_;
        x->Value = lit_;
        Next();
        return x;
    }
    case Tok::LPAREN: {
        Pos lparen = pos_;
        Next();
        expr_lev_++;
        Node *x = ParseRhs();
        expr_lev_--;
        auto *p = New<ParenExpr>();
        p->Lparen = lparen;
        p->X = x;
        p->Rparen = Expect(Tok::RPAREN);
        return p;
    }
    case Tok::FUNC: return ParseFuncTypeOrLit();
    default: break;
    }
    if (Node *typ = TryIdentOrType()) return typ;
    Pos pos = pos_;
    ErrorExpected(pos, "operand");
    Advance(kStmtStart);
    return Bad(pos, pos_);
}

Node *Parser::ParseSelector(Node *x) {
    auto *s = New<SelectorExpr>();
    s->X = x;
    s->Sel = ParseIdent();
    return s;
}

Node *Parser::ParseTypeAssertion(Node *x) {
    Pos lparen = Expect(Tok::LPAREN);
    Node *typ = nullptr;
    if (tok_ == Tok::TYPE) {
        Next();
    } else {
        typ = ParseType();
    }
    auto *t = New<TypeAssertExpr>();
    t->X = x;
    t->Type = typ;
    t->Lparen = lparen;
    t->Rparen = Expect(Tok::RPAREN);
    return t;
}

Node *Parser::ParseIndexOrSliceOrInstance(Node *x) {
    Pos lbrack = Expect(Tok::LBRACK);
    if (tok_ == Tok::RBRACK) {
        ErrorExpected(pos_, "operand");
        Pos rbrack = pos_;
        Next();
        auto *ix = New<IndexExpr>();
        ix->X = x;
        ix->Lbrack = lbrack;
        ix->Index = Bad(rbrack, rbrack);
        ix->Rbrack = rbrack;
        return ix;
    }
    expr_lev_++;
    constexpr int N = 3;
    std::vector<Node *> args;
    Node *index[N] = {nullptr, nullptr, nullptr};
    Pos colons[N - 1] = {NoPos, NoPos};
    if (tok_ != Tok::COLON) index[0] = ParseRhs();
    int ncolons = 0;
    if (tok_ == Tok::COLON) {
        while (tok_ == Tok::COLON && ncolons < N - 1) {
            colons[ncolons] = pos_;
            ncolons++;
            Next();
            if (tok_ != Tok::COLON && tok_ != Tok::RBRACK && tok_ != Tok::EOF_) index[ncolons] = ParseRhs();
        }
    } else if (tok_ == Tok::COMMA) {
        args.push_back(index[0]);
        while (tok_ == Tok::COMMA) {
            Next();
            if (tok_ != Tok::RBRACK && tok_ != Tok::EOF_) args.push_back(ParseType());
        }
    }
    expr_lev_--;
    Pos rbrack = Expect(Tok::RBRACK);
    if (ncolons > 0) {
        bool slice3 = false;
        if (ncolons == 2) {
            slice3 = true;
            if (!index[1]) {
                Err(colons[0], "middle index required in 3-index slice");
                index[1] = Bad(colons[0] + 1, colons[1]);
            }
            if (!index[2]) {
                Err(colons[1], "final index required in 3-index slice");
                index[2] = Bad(colons[1] + 1, rbrack);
            }
        }
        auto *s = New<SliceExpr>();
        s->X = x;
        s->Lbrack = lbrack;
        s->Low = index[0];
        s->High = index[1];
        s->Max = index[2];
        s->Slice3 = slice3;
        s->Rbrack = rbrack;
        return s;
    }
    if (args.empty()) {
        auto *ix = New<IndexExpr>();
        ix->X = x;
        ix->Lbrack = lbrack;
        ix->Index = index[0];
        ix->Rbrack = rbrack;
        return ix;
    }
    return PackIndexExpr(x, lbrack, std::move(args), rbrack);
}

CallExpr *Parser::ParseCallOrConversion(Node *fun) {
    Pos lparen = Expect(Tok::LPAREN);
    expr_lev_++;
    std::vector<Node *> list;
    Pos ellipsis = NoPos;
    while (tok_ != Tok::RPAREN && tok_ != Tok::EOF_ && !IsValid(ellipsis)) {
        list.push_back(ParseRhs());
        if (tok_ == Tok::ELLIPSIS) {
            ellipsis = pos_;
            Next();
        }
        if (!AtComma("argument list", Tok::RPAREN)) break;
        Next();
    }
    expr_lev_--;
    Pos rparen = ExpectClosing(Tok::RPAREN, "argument list");
    auto *c = New<CallExpr>();
    c->Fun = fun;
    c->Lparen = lparen;
    c->Args = std::move(list);
    c->Ellipsis = ellipsis;
    c->Rparen = rparen;
    return c;
}

Node *Parser::ParseValue() {
    if (tok_ == Tok::LBRACE) return ParseLiteralValue(nullptr);
    return ParseExpr();
}

Node *Parser::ParseElement() {
    Node *x = ParseValue();
    if (tok_ == Tok::COLON) {
        Pos colon = pos_;
        Next();
        auto *kv = New<KeyValueExpr>();
        kv->Key = x;
        kv->Colon = colon;
        kv->Value = ParseValue();
        x = kv;
    }
    return x;
}

std::vector<Node *> Parser::ParseElementList() {
    std::vector<Node *> list;
    while (tok_ != Tok::RBRACE && tok_ != Tok::EOF_) {
        list.push_back(ParseElement());
        if (!AtComma("composite literal", Tok::RBRACE)) break;
        Next();
    }
    return list;
}

Node *Parser::ParseLiteralValue(Node *typ) {
    NestGuard g(this);
    Pos lbrace = Expect(Tok::LBRACE);
    std::vector<Node *> elts;
    expr_lev_++;
    if (tok_ != Tok::RBRACE) elts = ParseElementList();
    expr_lev_--;
    Pos rbrace = ExpectClosing(Tok::RBRACE, "composite literal");
    auto *c = New<CompositeLit>();
    c->Type = typ;
    c->Lbrace = lbrace;
    c->Elts = std::move(elts);
    c->Rbrace = rbrace;
    return c;
}

Node *Parser::ParsePrimaryExpr(Node *x) {
    if (!x) x = ParseOperand();
    int n = 0;
    struct Restore {
        Parser *p;
        int *n;
        ~Restore() { p->nest_lev_ -= *n; }
    } restore{this, &n};
    for (n = 1;; n++) {
        IncNest();
        switch (tok_) {
        case Tok::PERIOD:
            Next();
            if (tok_ == Tok::IDENT) {
                x = ParseSelector(x);
            } else if (tok_ == Tok::LPAREN) {
                x = ParseTypeAssertion(x);
            } else {
                Pos pos = pos_;
                ErrorExpected(pos, "selector or type assertion");
                if (tok_ != Tok::RBRACE) Next();
                auto *sel = New<Ident>();
                sel->NamePos = pos;
                sel->Name = "_";
                auto *s = New<SelectorExpr>();
                s->X = x;
                s->Sel = sel;
                x = s;
            }
            break;
        case Tok::LBRACK: x = ParseIndexOrSliceOrInstance(x); break;
        case Tok::LPAREN: x = ParseCallOrConversion(x); break;
        case Tok::LBRACE: {
            Node *t = Unparen(x);
            switch (t->kind) {
            case K::BadExpr: case K::Ident: case K::SelectorExpr: case K::IndexExpr: case K::IndexListExpr:
                if (expr_lev_ < 0) return x;
                break;
            case K::ArrayType: case K::StructType: case K::MapType: break;
            default: return x;
            }
            if (t != x) Err(NodePos(t), "cannot parenthesize type in composite literal");
            x = ParseLiteralValue(x);
            break;
        }
        default: return x;
        }
    }
}

Node *Parser::ParseUnaryExpr() {
    NestGuard g(this);
    switch (tok_) {
    case Tok::ADD: case Tok::SUB: case Tok::NOT: case Tok::XOR: case Tok::AND: case Tok::TILDE: {
        Pos pos = pos_;
        Tok op = tok_;
        Next();
        auto *u = New<UnaryExpr>();
        u->OpPos = pos;
        u->Op = op;
        u->X = ParseUnaryExpr();
        return u;
    }
    case Tok::ARROW: {
        Pos arrow = pos_;
        Next();
        Node *x = ParseUnaryExpr();
        if (auto *typ = As<ChanType>(x)) {
            int dir = SEND;
            while (typ && dir == SEND) {
                if (typ->Dir == RECV) ErrorExpected(typ->Arrow, "'chan'");
                Pos old_arrow = typ->Arrow;
                typ->Begin = arrow;
                typ->Arrow = arrow;
                arrow = old_arrow;
                dir = typ->Dir;
                typ->Dir = RECV;
                typ = As<ChanType>(typ->Value);
            }
            if (dir == SEND) ErrorExpected(arrow, "channel type");
            return x;
        }
        auto *u = New<UnaryExpr>();
        u->OpPos = arrow;
        u->Op = Tok::ARROW;
        u->X = x;
        return u;
    }
    case Tok::MUL: {
        Pos pos = pos_;
        Next();
        auto *s = New<StarExpr>();
        s->Star = pos;
        s->X = ParseUnaryExpr();
        return s;
    }
    default: break;
    }
    return ParsePrimaryExpr(nullptr);
}

Node *Parser::ParseBinaryExpr(Node *x, int prec1) {
    if (!x) x = ParseUnaryExpr();
    int n = 0;
    struct Restore {
        Parser *p;
        int *n;
        ~Restore() { p->nest_lev_ -= *n; }
    } restore{this, &n};
    for (n = 1;; n++) {
        IncNest();
        Tok op = tok_;
        if (in_rhs_ && op == Tok::ASSIGN) op = Tok::EQL;
        int oprec = Precedence(op);
        if (oprec < prec1) return x;
        Pos pos = Expect(op);
        Node *y = ParseBinaryExpr(nullptr, oprec + 1);
        auto *b = New<BinaryExpr>();
        b->X = x;
        b->OpPos = pos;
        b->Op = op;
        b->Y = y;
        x = b;
    }
}

Node *Parser::ParseRhs() {
    bool old = in_rhs_;
    in_rhs_ = true;
    Node *x = ParseExpr();
    in_rhs_ = old;
    return x;
}

std::pair<Node *, bool> Parser::ParseSimpleStmt(int mode) {
    std::vector<Node *> x = ParseList(false);
    switch (tok_) {
    case Tok::DEFINE: case Tok::ASSIGN: case Tok::ADD_ASSIGN: case Tok::SUB_ASSIGN: case Tok::MUL_ASSIGN:
    case Tok::QUO_ASSIGN: case Tok::REM_ASSIGN: case Tok::AND_ASSIGN: case Tok::OR_ASSIGN: case Tok::XOR_ASSIGN:
    case Tok::SHL_ASSIGN: case Tok::SHR_ASSIGN: case Tok::AND_NOT_ASSIGN: {
        Pos pos = pos_;
        Tok tok = tok_;
        Next();
        std::vector<Node *> y;
        bool is_range = false;
        if (mode == kRangeOk && tok_ == Tok::RANGE && (tok == Tok::DEFINE || tok == Tok::ASSIGN)) {
            Pos rpos = pos_;
            Next();
            auto *u = New<UnaryExpr>();
            u->OpPos = rpos;
            u->Op = Tok::RANGE;
            u->X = ParseRhs();
            y = {u};
            is_range = true;
        } else {
            y = ParseList(true);
        }
        auto *as = New<AssignStmt>();
        as->Lhs = std::move(x);
        as->TokPos = pos;
        as->Tok_ = tok;
        as->Rhs = std::move(y);
        return {as, is_range};
    }
    default: break;
    }
    if (x.size() > 1) ErrorExpected(NodePos(x[0]), "1 expression");
    switch (tok_) {
    case Tok::COLON: {
        Pos colon = pos_;
        Next();
        if (auto *label = As<Ident>(x[0]); mode == kLabelOk && label) {
            auto *ls = New<LabeledStmt>();
            ls->Label = label;
            ls->Colon = colon;
            ls->Stmt = ParseStmt();
            return {ls, false};
        }
        Err(colon, "illegal label declaration");
        auto *b = New<BadStmt>();
        b->From = NodePos(x[0]);
        b->To = colon + 1;
        return {b, false};
    }
    case Tok::ARROW: {
        Pos arrow = pos_;
        Next();
        auto *s = New<SendStmt>();
        s->Chan = x[0];
        s->Arrow = arrow;
        s->Value = ParseRhs();
        return {s, false};
    }
    case Tok::INC: case Tok::DEC: {
        auto *s = New<IncDecStmt>();
        s->X = x[0];
        s->TokPos = pos_;
        s->Tok_ = tok_;
        Next();
        return {s, false};
    }
    default: break;
    }
    auto *es = New<ExprStmt>();
    es->X = x[0];
    return {es, false};
}

CallExpr *Parser::ParseCallExpr(const char *call_type) {
    Node *x = ParseRhs();
    if (Node *t = Unparen(x); t != x) {
        Err(NodePos(x), std::string("expression in ") + call_type + " must not be parenthesized");
        x = t;
    }
    if (auto *call = As<CallExpr>(x)) return call;
    if (!As<BadExpr>(x)) Err(NodeEnd(x), std::string("expression in ") + call_type + " must be function call");
    return nullptr;
}

Node *Parser::ParseGoStmt() {
    Pos pos = Expect(Tok::GO);
    CallExpr *call = ParseCallExpr("go");
    ExpectSemi();
    if (!call) {
        auto *b = New<BadStmt>();
        b->From = pos;
        b->To = pos + 2;
        return b;
    }
    auto *s = New<GoStmt>();
    s->Go = pos;
    s->Call = call;
    return s;
}

Node *Parser::ParseDeferStmt() {
    Pos pos = Expect(Tok::DEFER);
    CallExpr *call = ParseCallExpr("defer");
    ExpectSemi();
    if (!call) {
        auto *b = New<BadStmt>();
        b->From = pos;
        b->To = pos + 5;
        return b;
    }
    auto *s = New<DeferStmt>();
    s->Defer = pos;
    s->Call = call;
    return s;
}

Node *Parser::ParseReturnStmt() {
    Pos pos = pos_;
    Expect(Tok::RETURN);
    std::vector<Node *> x;
    if (tok_ != Tok::SEMICOLON && tok_ != Tok::RBRACE) x = ParseList(true);
    ExpectSemi();
    auto *r = New<ReturnStmt>();
    r->Return = pos;
    r->Results = std::move(x);
    return r;
}

Node *Parser::ParseBranchStmt(Tok tok) {
    Pos pos = Expect(tok);
    Ident *label = nullptr;
    if (tok == Tok::GOTO || ((tok == Tok::CONTINUE || tok == Tok::BREAK) && tok_ == Tok::IDENT)) label = ParseIdent();
    ExpectSemi();
    auto *b = New<BranchStmt>();
    b->TokPos = pos;
    b->Tok_ = tok;
    b->Label = label;
    return b;
}

Node *Parser::MakeExpr(Node *s, const std::string &want) {
    if (!s) return nullptr;
    if (auto *es = As<ExprStmt>(s)) return es->X;
    std::string found = As<AssignStmt>(s) ? "assignment" : "simple statement";
    Err(NodePos(s), "expected " + want + ", found " + found + " (missing parentheses around composite literal?)");
    return Bad(NodePos(s), NodeEnd(s));
}

void Parser::ParseIfHeader(Node **init_out, Node **cond_out) {
    Node *init = nullptr, *cond = nullptr;
    if (tok_ == Tok::LBRACE) {
        Err(pos_, "missing condition in if statement");
        *init_out = nullptr;
        *cond_out = Bad(pos_, pos_);
        return;
    }
    int prev_lev = expr_lev_;
    expr_lev_ = -1;
    if (tok_ != Tok::SEMICOLON) {
        if (tok_ == Tok::VAR) {
            Next();
            Err(pos_, "var declaration not allowed in if initializer");
        }
        init = ParseSimpleStmt(kBasic).first;
    }
    Node *cond_stmt = nullptr;
    Pos semi_pos = NoPos;
    std::string semi_lit;
    if (tok_ != Tok::LBRACE) {
        if (tok_ == Tok::SEMICOLON) {
            semi_pos = pos_;
            semi_lit = lit_;
            Next();
        } else {
            Expect(Tok::SEMICOLON);
        }
        if (tok_ != Tok::LBRACE) cond_stmt = ParseSimpleStmt(kBasic).first;
    } else {
        cond_stmt = init;
        init = nullptr;
    }
    if (cond_stmt) {
        cond = MakeExpr(cond_stmt, "boolean expression");
    } else if (IsValid(semi_pos)) {
        Err(semi_pos, semi_lit == "\n" ? "unexpected newline, expecting { after if clause" : "missing condition in if statement");
    }
    if (!cond) cond = Bad(pos_, pos_);
    expr_lev_ = prev_lev;
    *init_out = init;
    *cond_out = cond;
}

IfStmt *Parser::ParseIfStmt() {
    NestGuard g(this);
    Pos pos = Expect(Tok::IF);
    Node *init = nullptr, *cond = nullptr;
    ParseIfHeader(&init, &cond);
    BlockStmt *body = ParseBlockStmt();
    Node *else_ = nullptr;
    if (tok_ == Tok::ELSE) {
        Next();
        if (tok_ == Tok::IF) {
            else_ = ParseIfStmt();
        } else if (tok_ == Tok::LBRACE) {
            else_ = ParseBlockStmt();
            ExpectSemi();
        } else {
            ErrorExpected(pos_, "if statement or block");
            auto *b = New<BadStmt>();
            b->From = pos_;
            b->To = pos_;
            else_ = b;
        }
    } else {
        ExpectSemi();
    }
    auto *s = New<IfStmt>();
    s->If = pos;
    s->Init = init;
    s->Cond = cond;
    s->Body = body;
    s->Else = else_;
    return s;
}

CaseClause *Parser::ParseCaseClause() {
    Pos pos = pos_;
    std::vector<Node *> list;
    if (tok_ == Tok::CASE) {
        Next();
        list = ParseList(true);
    } else {
        Expect(Tok::DEFAULT);
    }
    auto *c = New<CaseClause>();
    c->Case = pos;
    c->List = std::move(list);
    c->Colon = Expect(Tok::COLON);
    c->Body = ParseStmtList();
    return c;
}

bool IsTypeSwitchAssert(Node *x) {
    auto *a = As<TypeAssertExpr>(x);
    return a && !a->Type;
}

bool Parser::IsTypeSwitchGuard(Node *s) {
    if (auto *es = As<ExprStmt>(s)) return IsTypeSwitchAssert(es->X);
    if (auto *as = As<AssignStmt>(s)) {
        if (as->Lhs.size() == 1 && as->Rhs.size() == 1 && IsTypeSwitchAssert(as->Rhs[0])) {
            if (as->Tok_ == Tok::ASSIGN) {
                Err(as->TokPos, "expected ':=', found '='");
                return true;
            }
            if (as->Tok_ == Tok::DEFINE) return true;
        }
    }
    return false;
}

Node *Parser::ParseSwitchStmt() {
    Pos pos = Expect(Tok::SWITCH);
    Node *s1 = nullptr, *s2 = nullptr;
    if (tok_ != Tok::LBRACE) {
        int prev_lev = expr_lev_;
        expr_lev_ = -1;
        if (tok_ != Tok::SEMICOLON) s2 = ParseSimpleStmt(kBasic).first;
        if (tok_ == Tok::SEMICOLON) {
            Next();
            s1 = s2;
            s2 = nullptr;
            if (tok_ != Tok::LBRACE) s2 = ParseSimpleStmt(kBasic).first;
        }
        expr_lev_ = prev_lev;
    }
    bool type_switch = IsTypeSwitchGuard(s2);
    auto *body = New<BlockStmt>();
    body->Lbrace = Expect(Tok::LBRACE);
    while (tok_ == Tok::CASE || tok_ == Tok::DEFAULT) body->List.push_back(ParseCaseClause());
    body->Rbrace = Expect(Tok::RBRACE);
    ExpectSemi();
    if (type_switch) {
        auto *t = New<TypeSwitchStmt>();
        t->Switch = pos;
        t->Init = s1;
        t->Assign = s2;
        t->Body = body;
        return t;
    }
    auto *s = New<SwitchStmt>();
    s->Switch = pos;
    s->Init = s1;
    s->Tag = MakeExpr(s2, "switch expression");
    s->Body = body;
    return s;
}

CommClause *Parser::ParseCommClause() {
    Pos pos = pos_;
    Node *comm = nullptr;
    if (tok_ == Tok::CASE) {
        Next();
        std::vector<Node *> lhs = ParseList(false);
        if (tok_ == Tok::ARROW) {
            if (lhs.size() > 1) ErrorExpected(NodePos(lhs[0]), "1 expression");
            Pos arrow = pos_;
            Next();
            auto *s = New<SendStmt>();
            s->Chan = lhs[0];
            s->Arrow = arrow;
            s->Value = ParseRhs();
            comm = s;
        } else if (Tok tok = tok_; tok == Tok::ASSIGN || tok == Tok::DEFINE) {
            if (lhs.size() > 2) {
                ErrorExpected(NodePos(lhs[0]), "1 or 2 expressions");
                lhs.resize(2);
            }
            Pos apos = pos_;
            Next();
            Node *rhs = ParseRhs();
            auto *as = New<AssignStmt>();
            as->Lhs = std::move(lhs);
            as->TokPos = apos;
            as->Tok_ = tok;
            as->Rhs = {rhs};
            comm = as;
        } else {
            if (lhs.size() > 1) ErrorExpected(NodePos(lhs[0]), "1 expression");
            auto *es = New<ExprStmt>();
            es->X = lhs[0];
            comm = es;
        }
    } else {
        Expect(Tok::DEFAULT);
    }
    auto *c = New<CommClause>();
    c->Case = pos;
    c->Comm = comm;
    c->Colon = Expect(Tok::COLON);
    c->Body = ParseStmtList();
    return c;
}

Node *Parser::ParseSelectStmt() {
    Pos pos = Expect(Tok::SELECT);
    auto *body = New<BlockStmt>();
    body->Lbrace = Expect(Tok::LBRACE);
    while (tok_ == Tok::CASE || tok_ == Tok::DEFAULT) body->List.push_back(ParseCommClause());
    body->Rbrace = Expect(Tok::RBRACE);
    ExpectSemi();
    auto *s = New<SelectStmt>();
    s->Select = pos;
    s->Body = body;
    return s;
}

Node *Parser::ParseForStmt() {
    Pos pos = Expect(Tok::FOR);
    Node *s1 = nullptr, *s2 = nullptr, *s3 = nullptr;
    bool is_range = false;
    if (tok_ != Tok::LBRACE) {
        int prev_lev = expr_lev_;
        expr_lev_ = -1;
        if (tok_ != Tok::SEMICOLON) {
            if (tok_ == Tok::RANGE) {
                Pos rpos = pos_;
                Next();
                auto *u = New<UnaryExpr>();
                u->OpPos = rpos;
                u->Op = Tok::RANGE;
                u->X = ParseRhs();
                auto *as = New<AssignStmt>();
                as->Tok_ = Tok::ILLEGAL;
                as->Rhs = {u};
                s2 = as;
                is_range = true;
            } else {
                std::tie(s2, is_range) = ParseSimpleStmt(kRangeOk);
            }
        }
        if (!is_range && tok_ == Tok::SEMICOLON) {
            Next();
            s1 = s2;
            s2 = nullptr;
            if (tok_ != Tok::SEMICOLON) s2 = ParseSimpleStmt(kBasic).first;
            ExpectSemi();
            if (tok_ != Tok::LBRACE) s3 = ParseSimpleStmt(kBasic).first;
        }
        expr_lev_ = prev_lev;
    }
    BlockStmt *body = ParseBlockStmt();
    ExpectSemi();
    if (is_range) {
        auto *as = static_cast<AssignStmt *>(s2);
        Node *key = nullptr, *value = nullptr;
        switch (as->Lhs.size()) {
        case 0: break;
        case 1: key = as->Lhs[0]; break;
        case 2: key = as->Lhs[0]; value = as->Lhs[1]; break;
        default: {
            ErrorExpected(NodePos(as->Lhs.back()), "at most 2 expressions");
            auto *b = New<BadStmt>();
            b->From = pos;
            b->To = NodeEnd(body);
            return b;
        }
        }
        auto *u = static_cast<UnaryExpr *>(as->Rhs[0]);
        auto *r = New<RangeStmt>();
        r->For = pos;
        r->Key = key;
        r->Value = value;
        r->TokPos = as->TokPos;
        r->Tok_ = as->Tok_;
        r->Range = NodePos(u);
        r->X = u->X;
        r->Body = body;
        return r;
    }
    auto *f = New<ForStmt>();
    f->For = pos;
    f->Init = s1;
    f->Cond = MakeExpr(s2, "boolean or range expression");
    f->Post = s3;
    f->Body = body;
    return f;
}

Node *Parser::ParseStmt() {
    NestGuard g(this);
    Node *s = nullptr;
    switch (tok_) {
    case Tok::CONST: case Tok::TYPE: case Tok::VAR: {
        auto *d = New<DeclStmt>();
        d->Decl = ParseDecl(kStmtStart);
        s = d;
        break;
    }
    case Tok::IDENT: case Tok::INT: case Tok::FLOAT: case Tok::IMAG: case Tok::CHAR: case Tok::STRING: case Tok::FUNC:
    case Tok::LPAREN: case Tok::LBRACK: case Tok::STRUCT: case Tok::MAP: case Tok::CHAN: case Tok::INTERFACE:
    case Tok::ADD: case Tok::SUB: case Tok::MUL: case Tok::AND: case Tok::XOR: case Tok::ARROW: case Tok::NOT:
        s = ParseSimpleStmt(kLabelOk).first;
        if (!As<LabeledStmt>(s)) ExpectSemi();
        break;
    case Tok::GO: s = ParseGoStmt(); break;
    case Tok::DEFER: s = ParseDeferStmt(); break;
    case Tok::RETURN: s = ParseReturnStmt(); break;
    case Tok::BREAK: case Tok::CONTINUE: case Tok::GOTO: case Tok::FALLTHROUGH: s = ParseBranchStmt(tok_); break;
    case Tok::LBRACE:
        s = ParseBlockStmt();
        ExpectSemi();
        break;
    case Tok::IF: s = ParseIfStmt(); break;
    case Tok::SWITCH: s = ParseSwitchStmt(); break;
    case Tok::SELECT: s = ParseSelectStmt(); break;
    case Tok::FOR: s = ParseForStmt(); break;
    case Tok::SEMICOLON: {
        auto *e = New<EmptyStmt>();
        e->Semicolon = pos_;
        e->Implicit = lit_ == "\n";
        Next();
        s = e;
        break;
    }
    case Tok::RBRACE: {
        auto *e = New<EmptyStmt>();
        e->Semicolon = pos_;
        e->Implicit = true;
        s = e;
        break;
    }
    default: {
        Pos pos = pos_;
        ErrorExpected(pos, "statement");
        Advance(kStmtStart);
        auto *b = New<BadStmt>();
        b->From = pos;
        b->To = pos_;
        s = b;
        break;
    }
    }
    return s;
}

Node *Parser::ParseImportSpec(CommentGroup *doc, Tok, int) {
    Ident *ident = nullptr;
    if (tok_ == Tok::IDENT) {
        ident = ParseIdent();
    } else if (tok_ == Tok::PERIOD) {
        ident = New<Ident>();
        ident->NamePos = pos_;
        ident->Name = ".";
        Next();
    }
    Pos pos = pos_;
    Pos end = pos_;
    std::string path;
    if (tok_ == Tok::STRING) {
        path = lit_;
        end = string_end_;
        Next();
    } else if (IsLiteral(tok_)) {
        Err(pos, "import path must be a string");
        Next();
    } else {
        Err(pos, "missing import path");
        Advance(kExprEnd);
    }
    CommentGroup *comment = ExpectSemi();
    auto *spec = New<ImportSpec>();
    spec->Doc = doc;
    spec->Name = ident;
    auto *lit = New<BasicLit>();
    lit->ValuePos = pos;
    lit->ValueEnd = end;
    lit->Kind = Tok::STRING;
    lit->Value = std::move(path);
    spec->Path = lit;
    spec->Comment = comment;
    imports_.push_back(spec);
    return spec;
}

Node *Parser::ParseValueSpec(CommentGroup *doc, Tok keyword, int) {
    std::vector<Ident *> idents = ParseIdentList();
    Node *typ = nullptr;
    std::vector<Node *> values;
    if (keyword == Tok::CONST) {
        if (tok_ != Tok::EOF_ && tok_ != Tok::SEMICOLON && tok_ != Tok::RPAREN) {
            typ = TryIdentOrType();
            if (tok_ == Tok::ASSIGN) {
                Next();
                values = ParseList(true);
            }
        }
    } else {
        if (tok_ != Tok::ASSIGN) typ = ParseType();
        if (tok_ == Tok::ASSIGN) {
            Next();
            values = ParseList(true);
        }
    }
    CommentGroup *comment = ExpectSemi();
    auto *spec = New<ValueSpec>();
    spec->Doc = doc;
    spec->Names = std::move(idents);
    spec->Type = typ;
    spec->Values = std::move(values);
    spec->Comment = comment;
    return spec;
}

void Parser::ParseGenericType(TypeSpec *spec, Pos open_pos, Ident *name0, Node *typ0) {
    std::vector<Field *> list = ParseParameterList(name0, typ0, Tok::RBRACK, false);
    Pos close_pos = Expect(Tok::RBRACK);
    auto *fl = New<FieldList>();
    fl->Opening = open_pos;
    fl->List = std::move(list);
    fl->Closing = close_pos;
    spec->TypeParams = fl;
    if (tok_ == Tok::ASSIGN) {
        spec->Assign = pos_;
        Next();
    }
    spec->Type = ParseType();
}

bool IsTypeElem(Node *x) {
    switch (x->kind) {
    case K::ArrayType: case K::StructType: case K::FuncType: case K::InterfaceType: case K::MapType: case K::ChanType:
        return true;
    case K::BinaryExpr: {
        auto *b = static_cast<BinaryExpr *>(x);
        return IsTypeElem(b->X) || IsTypeElem(b->Y);
    }
    case K::UnaryExpr: return static_cast<UnaryExpr *>(x)->Op == Tok::TILDE;
    case K::ParenExpr: return IsTypeElem(static_cast<ParenExpr *>(x)->X);
    default: return false;
    }
}

std::pair<Ident *, Node *> Parser::ExtractName(Node *x, bool force) {
    switch (x->kind) {
    case K::Ident: return {static_cast<Ident *>(x), nullptr};
    case K::BinaryExpr: {
        auto *b = static_cast<BinaryExpr *>(x);
        if (b->Op == Tok::MUL) {
            if (auto *name = As<Ident>(b->X); name && (force || IsTypeElem(b->Y))) {
                auto *s = New<StarExpr>();
                s->Star = b->OpPos;
                s->X = b->Y;
                return {name, s};
            }
        } else if (b->Op == Tok::OR) {
            auto [name, lhs] = ExtractName(b->X, force || IsTypeElem(b->Y));
            if (name && lhs) {
                BinaryExpr *op = a_->Copy(*b);
                op->X = lhs;
                return {name, op};
            }
        }
        break;
    }
    case K::CallExpr: {
        auto *c = static_cast<CallExpr *>(x);
        if (auto *name = As<Ident>(c->Fun)) {
            if (c->Args.size() == 1 && c->Ellipsis == NoPos && (force || IsTypeElem(c->Args[0]))) {
                auto *p = New<ParenExpr>();
                p->Lparen = c->Lparen;
                p->X = c->Args[0];
                p->Rparen = c->Rparen;
                return {name, p};
            }
        }
        break;
    }
    default: break;
    }
    return {nullptr, x};
}

Node *Parser::ParseTypeSpec(CommentGroup *doc, Tok, int) {
    Ident *name = ParseIdent();
    auto *spec = New<TypeSpec>();
    spec->Doc = doc;
    spec->Name = name;
    if (tok_ == Tok::LBRACK) {
        Pos lbrack = pos_;
        Next();
        if (tok_ == Tok::IDENT) {
            Node *x = ParseIdent();
            if (tok_ != Tok::LBRACK) {
                expr_lev_++;
                Node *lhs = ParsePrimaryExpr(x);
                x = ParseBinaryExpr(lhs, LowestPrec + 1);
                expr_lev_--;
            }
            auto [pname, ptype] = ExtractName(x, tok_ == Tok::COMMA);
            if (pname && (ptype || tok_ != Tok::RBRACK)) {
                ParseGenericType(spec, lbrack, pname, ptype);
            } else {
                spec->Type = ParseArrayType(lbrack, x);
            }
        } else {
            spec->Type = ParseArrayType(lbrack, nullptr);
        }
    } else {
        if (tok_ == Tok::ASSIGN) {
            spec->Assign = pos_;
            Next();
        }
        spec->Type = ParseType();
    }
    spec->Comment = ExpectSemi();
    return spec;
}

GenDecl *Parser::ParseGenDecl(Tok keyword, Node *(Parser::*f)(CommentGroup *, Tok, int)) {
    CommentGroup *doc = lead_comment_;
    Pos pos = Expect(keyword);
    auto *d = New<GenDecl>();
    d->Doc = doc;
    d->TokPos = pos;
    d->Tok_ = keyword;
    if (tok_ == Tok::LPAREN) {
        d->Lparen = pos_;
        Next();
        for (int iota = 0; tok_ != Tok::RPAREN && tok_ != Tok::EOF_; iota++) {
            d->Specs.push_back((this->*f)(lead_comment_, keyword, iota));
        }
        d->Rparen = Expect(Tok::RPAREN);
        ExpectSemi();
    } else {
        d->Specs.push_back((this->*f)(nullptr, keyword, 0));
    }
    return d;
}

FuncDecl *Parser::ParseFuncDecl() {
    CommentGroup *doc = lead_comment_;
    Pos pos = Expect(Tok::FUNC);
    FieldList *recv = nullptr;
    if (tok_ == Tok::LPAREN) recv = ParseParameters(false);
    Ident *ident = ParseIdent();
    FieldList *tparams = nullptr;
    if (tok_ == Tok::LBRACK) {
        tparams = ParseTypeParameters();
        if (recv && tparams) {
            Err(tparams->Opening, "method must have no type parameters");
            tparams = nullptr;
        }
    }
    FieldList *params = ParseParameters(false);
    FieldList *results = ParseParameters(true);
    BlockStmt *body = nullptr;
    if (tok_ == Tok::LBRACE) {
        body = ParseBody();
        ExpectSemi();
    } else if (tok_ == Tok::SEMICOLON) {
        Next();
        if (tok_ == Tok::LBRACE) {
            Err(pos_, "unexpected semicolon or newline before {");
            body = ParseBody();
            ExpectSemi();
        }
    } else {
        ExpectSemi();
    }
    auto *ft = New<FuncType>();
    ft->Func = pos;
    ft->TypeParams = tparams;
    ft->Params = params;
    ft->Results = results;
    auto *d = New<FuncDecl>();
    d->Doc = doc;
    d->Recv = recv;
    d->Name = ident;
    d->Type = ft;
    d->Body = body;
    return d;
}

Node *Parser::ParseDecl(const TokSetInit &sync) {
    Node *(Parser::*f)(CommentGroup *, Tok, int) = nullptr;
    switch (tok_) {
    case Tok::IMPORT: f = &Parser::ParseImportSpec; break;
    case Tok::CONST: case Tok::VAR: f = &Parser::ParseValueSpec; break;
    case Tok::TYPE: f = &Parser::ParseTypeSpec; break;
    case Tok::FUNC: return ParseFuncDecl();
    default: {
        Pos pos = pos_;
        ErrorExpected(pos, "declaration");
        Advance(sync);
        auto *b = New<BadDecl>();
        b->From = pos;
        b->To = pos_;
        return b;
    }
    }
    return ParseGenDecl(tok_, f);
}

File *Parser::ParseFile() {
    if (!errors.empty()) return nullptr;
    CommentGroup *doc = lead_comment_;
    Pos pos = Expect(Tok::PACKAGE);
    Ident *ident = ParseIdent();
    ExpectSemi();
    if (!errors.empty()) return nullptr;
    std::vector<Node *> decls;
    while (tok_ == Tok::IMPORT) decls.push_back(ParseGenDecl(Tok::IMPORT, &Parser::ParseImportSpec));
    Tok prev = Tok::IMPORT;
    while (tok_ != Tok::EOF_) {
        if (tok_ == Tok::IMPORT && prev != Tok::IMPORT) Err(pos_, "imports must appear before other declarations");
        prev = tok_;
        decls.push_back(ParseDecl(kDeclStart));
    }
    auto *f = New<File>();
    f->Doc = doc;
    f->Package = pos;
    f->Name = ident;
    f->Decls = std::move(decls);
    f->Imports = imports_;
    f->Comments = comments_;
    return f;
}

Node *Parser::PackIndexExpr(Node *x, Pos lbrack, std::vector<Node *> exprs, Pos rbrack) {
    if (exprs.size() == 1) {
        auto *ix = New<IndexExpr>();
        ix->X = x;
        ix->Lbrack = lbrack;
        ix->Index = exprs[0];
        ix->Rbrack = rbrack;
        return ix;
    }
    auto *il = New<IndexListExpr>();
    il->X = x;
    il->Lbrack = lbrack;
    il->Indices = std::move(exprs);
    il->Rbrack = rbrack;
    return il;
}

}  // namespace

void Result::LineCol(Pos p, int *line, int *col) const {
    if (p <= 0 || lines.empty()) {
        *line = *col = 0;
        return;
    }
    int off = p - 1;
    *line = LineOfOffset(lines, off);
    *col = off - lines[static_cast<size_t>(*line - 1)] + 1;
}

int Result::Line(Pos p) const {
    int l = 0, c = 0;
    LineCol(p, &l, &c);
    return l;
}

Result ParseFile(std::string_view src, const Options &opts) {
    Result r;
    r.arena = std::make_unique<Arena>();
    r.lines = LineTable(src);
    Parser p(src, opts, r.arena.get(), &r.lines);
    File *f = nullptr;
    try {
        p.Init();
        f = p.ParseFile();
    } catch (const Bailout &) {
        f = nullptr;
    }
    if (!f) {
        f = r.arena->New<File>();
        f->Name = r.arena->New<Ident>();
    }
    f->FileStart = 1;
    f->FileEnd = 1 + static_cast<int>(src.size());
    r.file = f;
    r.errors = std::move(p.errors);
    std::stable_sort(r.errors.begin(), r.errors.end(), [](const Error &a, const Error &b) {
        if (a.line != b.line) return a.line < b.line;
        if (a.col != b.col) return a.col < b.col;
        return a.msg < b.msg;
    });
    return r;
}

std::vector<Token> Scan(std::string_view src, std::vector<Error> *errors) {
    struct Ctx {
        std::vector<int> lines;
        std::vector<Error> *errors;
    } ctx{LineTable(src), errors};
    Scanner s;
    s.Init(src, [](void *c, int offset, const std::string &msg) {
        auto *x = static_cast<Ctx *>(c);
        if (!x->errors) return;
        Error e;
        e.offset = offset;
        e.line = LineOfOffset(x->lines, offset);
        e.col = offset - x->lines[static_cast<size_t>(e.line - 1)] + 1;
        e.msg = msg;
        x->errors->push_back(std::move(e));
    }, &ctx);
    std::vector<Token> out;
    for (;;) {
        Token t;
        Pos se = NoPos;
        t.tok = s.Scan(&t.pos, &t.lit, &se);
        if (t.tok == Tok::EOF_) {
            t.end = t.pos;
            out.push_back(std::move(t));
            break;
        }
        if (t.tok == Tok::STRING) {
            t.end = se;
        } else if (t.tok == Tok::SEMICOLON && t.lit == "\n") {
            t.end = t.pos;
        } else if (t.tok == Tok::COMMENT || IsLiteral(t.tok) || t.tok == Tok::ILLEGAL || t.tok == Tok::SEMICOLON) {
            t.end = t.pos + static_cast<int>(t.lit.size());
            if (t.tok == Tok::COMMENT) {
                // CRs were stripped from the text; find the real end in the source.
                size_t off = static_cast<size_t>(t.pos - 1);
                if (t.lit[1] == '/') {
                    size_t e = src.find('\n', off);
                    if (e == std::string_view::npos) e = src.size();
                    if (e > off && src[e - 1] == '\r') e--;
                    t.end = static_cast<int>(e) + 1;
                } else {
                    size_t e = src.find("*/", off + 2);
                    t.end = static_cast<int>(e == std::string_view::npos ? src.size() : e + 2) + 1;
                }
            }
        } else {
            t.end = t.pos + static_cast<int>(std::string_view(TokString(t.tok)).size());
        }
        out.push_back(std::move(t));
    }
    return out;
}

std::string QuoteRune(uint32_t r) {
    if (r > 0x10FFFF || (r >= 0xD800 && r < 0xE000)) r = RuneError;
    std::string s = "'";
    if (r == '\'' || r == '\\') {
        s += '\\';
        s += static_cast<char>(r);
    } else if (UnicodeIsPrint(r)) {
        AppendRune(s, r);
    } else {
        switch (r) {
        case '\a': s += "\\a"; break;
        case '\b': s += "\\b"; break;
        case '\f': s += "\\f"; break;
        case '\n': s += "\\n"; break;
        case '\r': s += "\\r"; break;
        case '\t': s += "\\t"; break;
        case '\v': s += "\\v"; break;
        default:
            if (r < ' ' || r == 0x7F) {
                s += "\\x" + Hex(r, 2, true);
            } else if (r < 0x10000) {
                s += "\\u" + Hex(r, 4, true);
            } else {
                s += "\\U" + Hex(r, 8, true);
            }
            break;
        }
    }
    s += "'";
    return s;
}

bool Unquote(std::string_view lit, std::string *out) {
    out->clear();
    if (lit.size() < 2) return false;
    char q = lit[0];
    if (q != lit.back()) return false;
    std::string_view body = lit.substr(1, lit.size() - 2);
    if (q == '`') {
        if (body.find('`') != std::string_view::npos) return false;
        for (char c : body) {
            if (c != '\r') *out += c;
        }
        return true;
    }
    if (q != '"' && q != '\'') return false;
    size_t i = 0;
    int runes = 0;
    while (i < body.size()) {
        char c = body[i];
        if (c == q || c == '\n') return false;
        runes++;
        if (c != '\\') {
            *out += c;
            i++;
            continue;
        }
        if (++i >= body.size()) return false;
        char e = body[i++];
        auto hex = [&](int n, uint32_t *v) {
            *v = 0;
            for (int k = 0; k < n; k++) {
                if (i >= body.size()) return false;
                int d = DigitVal(static_cast<unsigned char>(body[i++]));
                if (d >= 16) return false;
                *v = *v * 16 + static_cast<uint32_t>(d);
            }
            return true;
        };
        uint32_t v = 0;
        switch (e) {
        case 'a': *out += '\a'; break;
        case 'b': *out += '\b'; break;
        case 'f': *out += '\f'; break;
        case 'n': *out += '\n'; break;
        case 'r': *out += '\r'; break;
        case 't': *out += '\t'; break;
        case 'v': *out += '\v'; break;
        case '\\': *out += '\\'; break;
        case '\'': case '"':
            if (e != q) return false;
            *out += e;
            break;
        case 'x':
            if (!hex(2, &v)) return false;
            *out += static_cast<char>(v);
            break;
        case 'u':
            if (!hex(4, &v)) return false;
            AppendRune(*out, v);
            break;
        case 'U':
            if (!hex(8, &v) || v > 0x10FFFF) return false;
            AppendRune(*out, v);
            break;
        default:
            if (e < '0' || e > '7') return false;
            v = static_cast<uint32_t>(e - '0');
            for (int k = 0; k < 2; k++) {
                if (i >= body.size() || body[i] < '0' || body[i] > '7') return false;
                v = v * 8 + static_cast<uint32_t>(body[i++] - '0');
            }
            if (v > 255) return false;
            *out += static_cast<char>(v);
            break;
        }
    }
    if (q == '\'' && runes != 1) return false;
    return true;
}

}  // namespace goparse
