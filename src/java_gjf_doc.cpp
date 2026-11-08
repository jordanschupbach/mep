// google-java-format's layout engine: see java_gjf_doc.h.
//
// Copyright 2015 Google Inc., Apache License 2.0, ported from
// google-java-format 1.36.1. Licensed under the Apache License, Version 2.0;
// you may not use this file except in compliance with the License. You may
// obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
#include "java_gjf_doc.h"

#include <algorithm>
#include <cstdio>

#include "java_gjf_javadoc.h"

namespace javafmt::gjf {

// ---- text helpers ---------------------------------------------------------------

int Len16(std::string_view s) {
    int n = 0;
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if ((c & 0xC0) != 0x80) n++;
        if (c >= 0xF0) n++;  // a supplementary code point is a surrogate pair
    }
    return n;
}

size_t ByteIndex16(std::string_view s, int u16) {
    size_t i = 0;
    int n = 0;
    while (i < s.size() && n < u16) {
        size_t len = 1;
        const uint32_t cp = DecodeAt(s, i, &len);
        n += cp >= 0x10000 ? 2 : 1;
        i += len;
    }
    return i;
}

uint32_t DecodeAt(std::string_view s, size_t i, size_t *len) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    auto cont = [&](size_t k) -> uint32_t {
        return i + k < s.size() ? static_cast<uint32_t>(static_cast<unsigned char>(s[i + k]) & 0x3F) : 0;
    };
    if (c < 0x80) {
        *len = 1;
        return c;
    }
    if (c >= 0xF0 && i + 3 < s.size()) {
        *len = 4;
        return (static_cast<uint32_t>(c & 0x07) << 18) | (cont(1) << 12) | (cont(2) << 6) | cont(3);
    }
    if (c >= 0xE0 && i + 2 < s.size()) {
        *len = 3;
        return (static_cast<uint32_t>(c & 0x0F) << 12) | (cont(1) << 6) | cont(2);
    }
    if (c >= 0xC0 && i + 1 < s.size()) {
        *len = 2;
        return (static_cast<uint32_t>(c & 0x1F) << 6) | cont(1);
    }
    *len = 1;
    return 0xFFFD;
}

bool IsGuavaWhitespace(uint32_t c) {
    switch (c) {
    case '\t': case '\n': case 0x0B: case '\f': case '\r': case ' ':
    case 0x85: case 0xA0: case 0x1680: case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000:
        return true;
    default:
        return c >= 0x2000 && c <= 0x200A;
    }
}

bool IsJavaWhitespace(uint32_t c) {
    if (c == '\t' || c == '\n' || c == 0x0B || c == '\f' || c == '\r' || (c >= 0x1C && c <= 0x1F) || c == ' ')
        return true;
    if (c == 0x1680 || c == 0x205F || c == 0x3000 || c == 0x2028 || c == 0x2029) return true;
    return c >= 0x2000 && c <= 0x200A && c != 0x2007;
}

namespace {
// The code point ending just before byte offset `end`.
uint32_t DecodeBefore(std::string_view s, size_t end, size_t *len) {
    size_t i = end - 1;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80 && end - i < 4) i--;
    size_t l = 1;
    const uint32_t cp = DecodeAt(s, i, &l);
    if (i + l != end) {
        *len = 1;
        return 0xFFFD;
    }
    *len = l;
    return cp;
}
}  // namespace

std::string_view TrimLeadingWs(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        size_t len = 1;
        if (!IsGuavaWhitespace(DecodeAt(s, i, &len))) break;
        i += len;
    }
    return s.substr(i);
}

std::string_view TrimTrailingWs(std::string_view s) {
    size_t e = s.size();
    while (e > 0) {
        size_t len = 1;
        if (!IsGuavaWhitespace(DecodeBefore(s, e, &len))) break;
        e -= len;
    }
    return s.substr(0, e);
}

std::string_view TrimWs(std::string_view s) { return TrimTrailingWs(TrimLeadingWs(s)); }

int IndexOfNonWs(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        size_t len = 1;
        if (!IsGuavaWhitespace(DecodeAt(s, i, &len))) return static_cast<int>(i);
        i += len;
    }
    return -1;
}

std::string_view JavaTrim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && static_cast<unsigned char>(s[b]) <= ' ') b++;
    while (e > b && static_cast<unsigned char>(s[e - 1]) <= ' ') e--;
    return s.substr(b, e - b);
}

bool StartsWith(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.substr(0, p.size()) == p; }
bool EndsWith(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.substr(s.size() - p.size()) == p;
}

namespace newlines {

std::vector<int> LineOffsets(std::string_view input) {
    std::vector<int> out{0};
    for (size_t idx = 0; idx < input.size(); idx++) {
        const char c = input[idx];
        if (c == '\r') {
            if (idx + 1 < input.size() && input[idx + 1] == '\n') idx++;
            out.push_back(static_cast<int>(idx + 1));
        } else if (c == '\n') {
            out.push_back(static_cast<int>(idx + 1));
        }
    }
    return out;
}

int Count(std::string_view input) { return static_cast<int>(LineOffsets(input).size()) - 1; }

int FirstBreak(std::string_view input) {
    for (size_t idx = 0; idx < input.size(); idx++) {
        const char c = input[idx];
        if (c == '\r') {
            if (idx + 1 < input.size() && input[idx + 1] == '\n') idx++;
            return static_cast<int>(idx + 1);
        }
        if (c == '\n') return static_cast<int>(idx + 1);
    }
    return -1;
}

bool IsNewline(std::string_view input) { return input == "\r\n" || input == "\n" || input == "\r"; }

int HasNewlineAt(std::string_view input, size_t idx) {
    // BREAKS iterates "\r\n", "\n", "\r" in that order.
    if (input.substr(idx).substr(0, 2) == "\r\n") return 2;
    if (idx < input.size() && input[idx] == '\n') return 1;
    if (idx < input.size() && input[idx] == '\r') return 1;
    return -1;
}

std::string_view GetLineEnding(std::string_view input) {
    if (EndsWith(input, "\r\n")) return "\r\n";
    if (EndsWith(input, "\n")) return "\n";
    if (EndsWith(input, "\r")) return "\r";
    return {};
}

std::string GuessLineSeparator(std::string_view text) {
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '\r') return i + 1 < text.size() && text[i + 1] == '\n' ? "\r\n" : "\r";
        if (text[i] == '\n') return "\n";
    }
    return "\n";
}

bool ContainsBreaks(std::string_view text) { return text.find_first_of("\n\r") != std::string_view::npos; }

std::vector<std::string_view> Lines(std::string_view input) {
    std::vector<std::string_view> out;
    const std::vector<int> offs = LineOffsets(input);
    for (size_t k = 0; k < offs.size(); k++) {
        const size_t lo = static_cast<size_t>(offs[k]);
        if (lo >= input.size()) break;
        const size_t hi = k + 1 < offs.size() ? static_cast<size_t>(offs[k + 1]) : input.size();
        out.push_back(input.substr(lo, hi - lo));
    }
    return out;
}

}  // namespace newlines

Range Union(Range x, Range y) {
    if (x.Empty()) return y;
    if (y.Empty()) return x;
    return Range{std::min(x.lo, y.lo), std::max(x.hi, y.hi)};
}

// ---- Tok / JavaInput ---------------------------------------------------------------

bool Tok::IsJavadocComment() const {
    // Before Java 23 (GJF's reference runtime is JDK 21) `///` is a plain comment.
    return StartsWith(text, "/**") && !StartsWith(text, "/***") && Len16(text) > 4;
}

namespace {

bool IsJavaIdentifierStart(uint32_t c) {
    if (c < 0x80) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
    return !IsJavaWhitespace(c);  // approximation: letters, currency, connectors
}

int UpdateColumn(int column, std::string_view original) {
    const std::vector<int> offs = newlines::LineOffsets(original);
    const int last = offs.back();
    if (last > 0) return Len16(original.substr(static_cast<size_t>(last)));
    return column + Len16(original);
}

// /\*[A-Za-z0-9\s_\-]+=\s*\*/
bool IsParamComment(const Tok &tok) {
    if (!tok.IsSlashStarComment()) return false;
    std::string_view t = tok.text;
    if (t.size() < 5 || !EndsWith(t, "*/")) return false;
    t = t.substr(2, t.size() - 4);
    auto is_s = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == 0x0B || c == '\f' || c == '\r'; };
    size_t e = t.size();
    while (e > 0 && is_s(t[e - 1])) e--;
    if (e == 0 || t[e - 1] != '=') return false;
    // [A-Za-z0-9\s_\-]+ then '=' : with backtracking, the class must cover t[0, eq) for some '=' at eq
    // followed only by \s; the last '=' is the only candidate (the class has no '=').
    const size_t eq = e - 1;
    if (eq == 0) return false;
    for (size_t i = 0; i < eq; i++) {
        const char c = t[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || is_s(c) || c == '_' ||
              c == '-'))
            return false;
    }
    return true;
}

}  // namespace

JavaInput::JavaInput(std::string text, const std::vector<RawTok> &raw) : text_(std::move(text)) {
    lines_ = newlines::Lines(text_);
    // buildToks
    std::vector<Tok> toks;
    int kN = 0;
    int charI = 0;
    int columnI = 0;
    for (const RawTok &t : raw) {
        if (t.kind == RawKind::Eof) break;
        const int charI0 = t.pos;
        std::string original = text_.substr(static_cast<size_t>(charI0), static_cast<size_t>(t.end - t.pos));
        std::string tok_text = t.kind == RawKind::StringLiteral ? "\"" + t.string_val + "\"" : original;
        if (tok_text.empty()) continue;
        size_t l0 = 1;
        const uint32_t c0 = DecodeAt(tok_text, 0, &l0);
        bool is_token = false, is_numbered = false;
        std::string extra_newline;
        std::vector<std::string> strings;
        if (IsJavaWhitespace(c0)) {
            for (std::string_view line : newlines::Lines(original)) {
                const std::string_view nl = newlines::GetLineEnding(line);
                if (!nl.empty()) {
                    const std::string_view spaces = line.substr(0, line.size() - nl.size());
                    if (!spaces.empty()) strings.emplace_back(spaces);
                    strings.emplace_back(nl);
                } else if (!line.empty()) {
                    strings.emplace_back(line);
                }
            }
        } else if (c0 == '\'' || c0 == '"') {
            is_token = is_numbered = true;
            strings.push_back(original);
        } else if (StartsWith(tok_text, "//") || StartsWith(tok_text, "/*")) {
            if (StartsWith(tok_text, "//") && (EndsWith(original, "\n") || EndsWith(original, "\r"))) {
                extra_newline = std::string(newlines::GetLineEnding(original));
                tok_text.resize(tok_text.size() - extra_newline.size());
                original.resize(original.size() - extra_newline.size());
            }
            is_numbered = true;
            strings.push_back(original);
        } else if (IsJavaIdentifierStart(c0) || (c0 >= '0' && c0 <= '9') ||
                   (c0 == '.' && tok_text.size() > 1 && tok_text[1] >= '0' && tok_text[1] <= '9')) {
            is_token = is_numbered = true;
            strings.push_back(tok_text);
        } else {
            // Operators are broken into one-character toks.
            is_token = is_numbered = true;
            for (size_t i = 0; i < tok_text.size();) {
                size_t len = 1;
                DecodeAt(tok_text, i, &len);
                strings.push_back(tok_text.substr(i, len));
                i += len;
            }
        }
        if (strings.size() == 1) {
            Tok tok;
            tok.index = is_numbered ? kN++ : -1;
            tok.original_text = original;
            tok.text = tok_text;
            tok.position = charI;
            tok.column = columnI;
            tok.is_token = is_token;
            tok.kind = t.kind == RawKind::StringLiteral ? "STRINGLITERAL" : t.keyword;
            tok.len16 = Len16(original);
            toks.push_back(std::move(tok));
            charI += static_cast<int>(original.size());
            columnI = UpdateColumn(columnI, original);
        } else {
            if (tok_text != original)
                throw FormatError{"Unicode escapes not allowed in whitespace or multi-character operators", 0};
            for (const std::string &str : strings) {
                Tok tok;
                tok.index = is_numbered ? kN++ : -1;
                tok.original_text = str;
                tok.text = str;
                tok.position = charI;
                tok.column = columnI;
                tok.is_token = is_token;
                tok.len16 = Len16(str);
                toks.push_back(std::move(tok));
                charI += static_cast<int>(str.size());
                columnI = UpdateColumn(columnI, original);
            }
        }
        if (!extra_newline.empty()) {
            Tok tok;
            tok.original_text = tok.text = extra_newline;
            tok.position = charI;
            tok.column = columnI;
            tok.len16 = Len16(extra_newline);
            toks.push_back(std::move(tok));
            columnI = 0;
            charI += static_cast<int>(extra_newline.size());
        }
    }
    {
        Tok eof;
        eof.index = kN;
        eof.position = charI;
        eof.column = columnI;
        eof.is_token = true;
        toks.push_back(std::move(eof));
    }
    toks_ = std::move(toks);
    kN_ = toks_.back().index;
    // computeRanges
    {
        int lineI = 0;
        for (const Tok &tok : toks_) {
            const int lineI0 = lineI;
            lineI += newlines::Count(tok.original_text);
            const int k = tok.index;
            if (k >= 0) {
                for (int i = lineI0; i <= lineI; i++) {
                    while (static_cast<int>(ranges_.size()) <= i) ranges_.push_back(Range{});
                    Range &r = ranges_[static_cast<size_t>(i)];
                    r = Range{r.Empty() ? k : r.lo, k + 1};
                }
            }
        }
    }
    for (const Tok &tok : toks_) position_column_.emplace(tok.position, tok.column);
    // buildTokens
    {
        size_t k = 0;
        const size_t n = toks_.size();
        std::vector<const Tok *> before;
        while (k < n) {
            bool restart = false;
            while (!toks_[k].is_token) {
                const Tok *tok = &toks_[k++];
                before.push_back(tok);
                if (IsParamComment(*tok)) {
                    while (k < n && toks_[k].IsNewline()) k++;
                }
            }
            const Tok *tok = &toks_[k++];
            std::vector<const Tok *> after;
            while (k < n && !toks_[k].is_token) {
                if (toks_[k].IsSlashStarComment() && (tok->text == "(" || tok->text == "<" || tok->text == "."))
                    break;
                if (toks_[k].IsJavadocComment() && tok->text == ";") break;
                if (IsParamComment(toks_[k])) {
                    tokens_.push_back(Token{tok, std::move(before), std::move(after)});
                    before.clear();
                    before.push_back(&toks_[k++]);
                    while (k < n && toks_[k].IsNewline()) k++;
                    restart = true;
                    break;
                }
                const Tok *non_token_after = &toks_[k++];
                after.push_back(non_token_after);
                if (newlines::ContainsBreaks(non_token_after->text)) break;
            }
            if (restart) continue;
            tokens_.push_back(Token{tok, std::move(before), std::move(after)});
            before.clear();
        }
    }
    for (const Token &token : tokens_) {
        const Tok *end = EndTok(token);
        int upper = end->position;
        if (!end->text.empty()) upper += static_cast<int>(end->original_text.size()) - 1;
        position_tokens_.push_back(PosRange{StartTok(token)->position, upper, &token});
    }
    kToToken_.assign(static_cast<size_t>(kN_) + 1, nullptr);
    for (const Token &token : tokens_) {
        for (const Tok *tok : token.toks_before)
            if (tok->index >= 0) kToToken_[static_cast<size_t>(tok->index)] = &token;
        kToToken_[static_cast<size_t>(token.tok->index)] = &token;
        for (const Tok *tok : token.toks_after)
            if (tok->index >= 0) kToToken_[static_cast<size_t>(tok->index)] = &token;
    }
    line_starts_ = newlines::LineOffsets(text_);
    // javac's LineMap: a line starts after "\r\n", "\n" or a lone "\r" -- the same as LineOffsets.
}

const Token *JavaInput::PositionToken(int position) const {
    auto it = std::upper_bound(position_tokens_.begin(), position_tokens_.end(), position,
                               [](int p, const PosRange &r) { return p < r.lo; });
    if (it == position_tokens_.begin()) return nullptr;
    --it;
    return position <= it->hi ? it->token : nullptr;
}

std::vector<const Token *> JavaInput::TokensIntersecting(int lo, int hi) const {
    std::vector<const Token *> out;
    for (const PosRange &r : position_tokens_)
        if (r.lo < hi && r.hi >= lo) out.push_back(r.token);
    return out;
}

int JavaInput::PositionColumn(int position) const {
    auto it = position_column_.find(position);
    return it == position_column_.end() ? -1 : it->second;
}

int JavaInput::GetLineNumber(int position) const {
    auto it = std::upper_bound(line_starts_.begin(), line_starts_.end(), position);
    return static_cast<int>(it - line_starts_.begin());
}

int JavaInput::GetColumnNumber(int position) const {
    const int line = GetLineNumber(position);
    const int start = line_starts_[static_cast<size_t>(std::max(line - 1, 0))];
    int col = 0;
    for (int i = start; i < position && i < static_cast<int>(text_.size()); i++) {
        if (text_[static_cast<size_t>(i)] == '\t') col = (col / 8 + 1) * 8;
        else if ((static_cast<unsigned char>(text_[static_cast<size_t>(i)]) & 0xC0) != 0x80) col++;
    }
    return col + 1;
}

Range JavaInput::GetRanges(int line) const {
    return 0 <= line && line < static_cast<int>(ranges_.size()) ? ranges_[static_cast<size_t>(line)] : Range{};
}

const Tok *StartTok(const Token &token) {
    for (const Tok *tok : token.toks_before)
        if (tok->index >= 0) return tok;
    return token.tok;
}

const Tok *EndTok(const Token &token) {
    for (size_t i = token.toks_after.size(); i-- > 0;)
        if (token.toks_after[i]->index >= 0) return token.toks_after[i];
    return token.tok;
}

// ---- BlankLineWanted ---------------------------------------------------------------

std::optional<bool> BlankLineWanted::Wanted() const {
    switch (kind) {
    case Yes: return true;
    case No: return false;
    case Preserve: return std::nullopt;
    case Conditional:
        for (const BreakTag *t : tags)
            if (t->WasBreakTaken()) return true;
        return std::nullopt;
    }
    return std::nullopt;
}

BlankLineWanted BlankLineWanted::Merge(const BlankLineWanted &other) const {
    if (kind != Conditional) return *this;
    if (other.kind != Conditional) return other;
    BlankLineWanted w = *this;
    w.tags.insert(w.tags.end(), other.tags.begin(), other.tags.end());
    return w;
}

// ---- comments ---------------------------------------------------------------------------

namespace {

bool IsJavaS(uint32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == 0x0B || c == '\f' || c == '\r'; }
bool IsIdentPart(uint32_t c) {
    if (c < 0x80) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$';
    return !IsJavaWhitespace(c);
}

std::u16string ToU16(std::string_view s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        size_t len = 1;
        const uint32_t cp = DecodeAt(s, i, &len);
        if (cp >= 0x10000) {
            out.push_back(static_cast<char16_t>(0xD800 + ((cp - 0x10000) >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + ((cp - 0x10000) & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
        i += len;
    }
    return out;
}

void AppendUtf8(std::string &out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string FromU16(std::u16string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (static_cast<uint32_t>(s[i + 1]) - 0xDC00);
            i++;
        }
        AppendUtf8(out, c);
    }
    return out;
}

// ^(//+)(?!noinspection|\$NON-NLS-\d+\$)[^\s/] -- the length of group 1, or 0.
size_t MissingSpacePrefix(std::string_view line) {
    size_t n = 0;
    while (n < line.size() && line[n] == '/') n++;
    if (n < 2 || n >= line.size()) return 0;
    const std::string_view rest = line.substr(n);
    if (StartsWith(rest, "noinspection")) return 0;
    if (StartsWith(rest, "$NON-NLS-")) {
        size_t i = 9;
        while (i < rest.size() && rest[i] >= '0' && rest[i] <= '9') i++;
        if (i > 9 && i < rest.size() && rest[i] == '$') return 0;
    }
    size_t len = 1;
    const uint32_t c = DecodeAt(line, n, &len);
    if (IsJavaS(c) || c == '/') return 0;
    return n;
}

std::string Spaces(int n) { return std::string(static_cast<size_t>(std::max(n, 0)), ' '); }

bool JavadocShaped(const std::vector<std::string> &lines) {
    if (lines.empty()) return false;
    const std::string_view first = JavaTrim(lines[0]);
    if (StartsWith(first, "/**")) return true;
    if (!StartsWith(first, "/*")) return false;
    for (size_t i = 1; i < lines.size(); i++)
        if (!StartsWith(JavaTrim(lines[i]), "*")) return false;
    return true;
}

}  // namespace

std::optional<std::string> ReformatParameterComment(const Tok &tok) {
    if (!tok.IsSlashStarComment()) return std::nullopt;
    // /\*\s*(\p{javaJavaIdentifierStart}\p{javaJavaIdentifierPart}*(\Q...\E)?)\s*=\s*\*/
    std::string_view t = tok.original_text;
    if (!StartsWith(t, "/*") || !EndsWith(t, "*/") || t.size() < 4) return std::nullopt;
    size_t i = 2;
    const size_t end = t.size() - 2;
    while (i < end && IsJavaS(static_cast<unsigned char>(t[i]))) i++;
    size_t len = 1;
    if (i >= end || !IsJavaIdentifierStart(DecodeAt(t, i, &len))) return std::nullopt;
    const size_t name_start = i;
    i += len;
    while (i < end) {
        const uint32_t c = DecodeAt(t, i, &len);
        if (!IsIdentPart(c)) break;
        i += len;
    }
    if (t.substr(i, 3) == "..." && i + 3 <= end) i += 3;
    const size_t name_end = i;
    while (i < end && IsJavaS(static_cast<unsigned char>(t[i]))) i++;
    if (i >= end || t[i] != '=') return std::nullopt;
    i++;
    while (i < end && IsJavaS(static_cast<unsigned char>(t[i]))) i++;
    if (i != end) return std::nullopt;
    return "/* " + std::string(t.substr(name_start, name_end - name_start)) + "= */";
}

std::string JavaCommentsHelper::Rewrite(const Tok &tok, int max_width, int column0) const {
    (void)max_width;
    if (!tok.IsComment()) return tok.original_text;
    std::string text = tok.original_text;
    if (tok.IsJavadocComment() && format_javadoc_) text = FormatJavadoc(text, column0);
    std::vector<std::string> lines;
    for (std::string_view line : newlines::Lines(text)) {
        if (tok.IsSlashSlashComment()) lines.emplace_back(TrimWs(line));
        else lines.emplace_back(TrimTrailingWs(line));
    }
    if (tok.IsSlashSlashComment()) return IndentLineComments(std::move(lines), column0);
    if (auto p = ReformatParameterComment(tok)) return *p;
    return JavadocShaped(lines) ? IndentJavadoc(lines, column0) : PreserveIndentation(lines, column0);
}

std::string JavaCommentsHelper::PreserveIndentation(const std::vector<std::string> &lines, int column0) const {
    std::string b;
    int start_col = -1;
    for (size_t i = 1; i < lines.size(); i++) {
        const int byte_idx = IndexOfNonWs(lines[i]);
        if (byte_idx < 0) continue;
        const int idx = Len16(std::string_view(lines[i]).substr(0, static_cast<size_t>(byte_idx)));
        if (start_col == -1 || idx < start_col) start_col = idx;
    }
    if (!lines.empty()) b += lines[0];
    for (size_t i = 1; i < lines.size(); ++i) {
        b += line_separator_;
        b += Spaces(column0);
        if (Len16(lines[i]) >= start_col) b += std::string_view(lines[i]).substr(ByteIndex16(lines[i], start_col));
        else b += lines[i];
    }
    return b;
}

std::string JavaCommentsHelper::IndentLineComments(std::vector<std::string> lines, int column0) const {
    lines = WrapLineComments(lines, column0);
    std::string b(JavaTrim(lines[0]));
    const std::string indent = Spaces(column0);
    for (size_t i = 1; i < lines.size(); ++i) {
        b += line_separator_;
        b += indent;
        b += JavaTrim(lines[i]);
    }
    return b;
}

std::vector<std::string> JavaCommentsHelper::WrapLineComments(const std::vector<std::string> &lines,
                                                              int column0) const {
    std::vector<std::string> result;
    for (std::string line : lines) {
        if (const size_t n = MissingSpacePrefix(line)) line = std::string(n, '/') + " " + line.substr(n);
        if (StartsWith(line, "// MOE:")) {
            result.push_back(line);
            continue;
        }
        if (Len16(line) + column0 > 100) {
            std::u16string u = ToU16(line);
            while (static_cast<int>(u.size()) + column0 > 100) {
                int idx = 100 - column0;
                while (idx >= 2 && !IsGuavaWhitespace(u[static_cast<size_t>(idx)])) idx--;
                if (idx <= 2) break;
                result.push_back(FromU16(u.substr(0, static_cast<size_t>(idx))));
                u = u"//" + u.substr(static_cast<size_t>(idx));
            }
            line = FromU16(u);
        }
        result.push_back(line);
    }
    return result;
}

std::string JavaCommentsHelper::IndentJavadoc(const std::vector<std::string> &lines, int column0) const {
    std::string b(JavaTrim(lines[0]));
    const std::string indent = Spaces(column0 + 1);
    for (size_t i = 1; i < lines.size(); ++i) {
        b += line_separator_;
        b += indent;
        const std::string_view line = JavaTrim(lines[i]);
        if (!StartsWith(line, "*")) b += "* ";
        b += line;
    }
    return b;
}

// ---- Doc ----------------------------------------------------------------------------------

int Doc::GetWidth() {
    if (!width_) width_ = ComputeWidth();
    return *width_;
}

const std::string &Doc::GetFlat() {
    if (!flat_) flat_ = ComputeFlat();
    return *flat_;
}

Range Doc::GetRange() {
    if (!range_) range_ = ComputeRange();
    return *range_;
}

namespace {
int WidthOf(const std::vector<Doc *> &docs) {
    int width = 0;
    for (Doc *doc : docs) {
        width += doc->GetWidth();
        if (width >= kMaxLineWidth) return kMaxLineWidth;
    }
    return width;
}
}  // namespace

int DocLevel::ComputeWidth() { return WidthOf(docs_); }

std::string DocLevel::ComputeFlat() {
    std::string s;
    for (Doc *doc : docs_) s += doc->GetFlat();
    return s;
}

Range DocLevel::ComputeRange() {
    Range r;
    for (Doc *doc : docs_) r = Union(r, doc->GetRange());
    return r;
}

State DocLevel::ComputeBreaks(const JavaCommentsHelper &ch, int max_width, State state) {
    const int this_width = GetWidth();
    if (state.column + this_width <= max_width) {
        one_line_ = true;
        return state.WithColumn(state.column + this_width);
    }
    const State broken = ComputeBroken(ch, max_width, State(state.indent + plus_indent_->Eval(), state.column));
    return state.WithColumn(broken.column);
}

namespace {
State ComputeSplit(const JavaCommentsHelper &ch, int max_width, const std::vector<Doc *> &docs, State state) {
    for (Doc *doc : docs) state = doc->ComputeBreaks(ch, max_width, state);
    return state;
}

State ComputeBreakAndSplit(const JavaCommentsHelper &ch, int max_width, State state, DocBreak *brk,
                           const std::vector<Doc *> &split) {
    const int break_width = brk ? brk->GetWidth() : 0;
    const int split_width = WidthOf(split);
    const bool should_break = (brk && brk->Mode() == FillMode::Unified) || state.must_break ||
                              state.column + break_width + split_width > max_width;
    if (brk) state = brk->ComputeBreaks(state, state.last_indent, should_break);
    const bool enough_room = state.column + split_width <= max_width;
    state = ComputeSplit(ch, max_width, split, state.WithMustBreak(false));
    if (!enough_room) state = state.WithMustBreak(true);
    return state;
}
}  // namespace

State DocLevel::ComputeBroken(const JavaCommentsHelper &ch, int max_width, State state) {
    splits_.clear();
    breaks_.clear();
    splits_.emplace_back();
    for (Doc *doc : docs_) {
        if (doc->op_kind == OpKind::Break) {
            breaks_.push_back(static_cast<DocBreak *>(doc));
            splits_.emplace_back();
        } else {
            splits_.back().push_back(doc);
        }
    }
    state = ComputeBreakAndSplit(ch, max_width, state, nullptr, splits_[0]);
    for (size_t i = 0; i < breaks_.size(); i++)
        state = ComputeBreakAndSplit(ch, max_width, state, breaks_[i], splits_[i + 1]);
    return state;
}

void DocLevel::Write(JavaOutput &out) {
    if (one_line_) {
        out.Append(GetFlat(), GetRange());
        return;
    }
    for (Doc *doc : splits_[0]) doc->Write(out);
    for (size_t i = 0; i < breaks_.size(); i++) {
        breaks_[i]->Write(out);
        for (Doc *doc : splits_[i + 1]) doc->Write(out);
    }
}

int DocToken::ComputeWidth() {
    const Tok *t = token_->tok;
    return newlines::FirstBreak(t->original_text) >= 0 ? kMaxLineWidth : t->Length();
}
std::string DocToken::ComputeFlat() { return token_->tok->original_text; }
Range DocToken::ComputeRange() { return Singleton(token_->tok->index); }
State DocToken::ComputeBreaks(const JavaCommentsHelper &, int, State state) {
    return state.WithColumn(state.column + ComputeWidth());
}
void DocToken::Write(JavaOutput &out) { out.Append(token_->tok->original_text, GetRange()); }

void DocSpace::Write(JavaOutput &out) { out.Append(" ", Range{}); }

State DocBreak::ComputeBreaks(State state, int last_indent, bool broken) {
    if (opt_tag_) opt_tag_->RecordBroken(broken);
    if (broken) {
        broken_ = true;
        new_indent_ = std::max(last_indent + plus_indent_->Eval(), 0);
        return state.WithColumn(new_indent_);
    }
    broken_ = false;
    new_indent_ = -1;
    return state.WithColumn(state.column + Len16(flat_text_));
}

void DocBreak::Write(JavaOutput &out) {
    if (broken_) {
        out.Append("\n", Range{});
        out.IndentBy(new_indent_);
    } else {
        out.Append(flat_text_, GetRange());
    }
}

int DocTok::ComputeWidth() {
    const int idx = newlines::FirstBreak(tok_->original_text);
    if (tok_->IsComment()) {
        if (idx > 0) return Len16(std::string_view(tok_->original_text).substr(0, static_cast<size_t>(idx)));
        if (tok_->IsSlashSlashComment() && !StartsWith(tok_->original_text, "// ")) return tok_->Length() + 1;
        if (auto p = ReformatParameterComment(*tok_)) return Len16(*p);
        return tok_->Length();
    }
    return idx != -1 ? kMaxLineWidth : tok_->Length();
}

std::string DocTok::ComputeFlat() {
    if (tok_->IsSlashSlashComment() && !StartsWith(tok_->original_text, "// "))
        return "// " + tok_->original_text.substr(2);
    if (auto p = ReformatParameterComment(*tok_)) return *p;
    return tok_->original_text;
}

State DocTok::ComputeBreaks(const JavaCommentsHelper &ch, int max_width, State state) {
    text_ = ch.Rewrite(*tok_, max_width, state.column);
    const std::vector<int> offs = newlines::LineOffsets(text_);
    const int last_line_len = Len16(std::string_view(text_).substr(static_cast<size_t>(offs.back())));
    return state.WithColumn(state.column + last_line_len);
}

void DocTok::Write(JavaOutput &out) { out.Append(text_, GetRange()); }

// ---- Arena / DocBuilder ------------------------------------------------------------------

const Indent *Arena::Const(int n) {
    auto it = consts_.find(n);
    if (it != consts_.end()) return it->second;
    indents_.emplace_back();
    indents_.back().n = n;
    consts_[n] = &indents_.back();
    return &indents_.back();
}

const Indent *Arena::If(const BreakTag *cond, const Indent *then_indent, const Indent *else_indent) {
    indents_.emplace_back();
    Indent &i = indents_.back();
    i.cond = cond;
    i.then_indent = then_indent;
    i.else_indent = else_indent;
    return &i;
}

BreakTag *Arena::Tag() {
    tags_.emplace_back();
    return &tags_.back();
}

DocBuilder::DocBuilder(Arena &arena) : arena_(arena) {
    base_ = arena_.Make<DocLevel>(arena_.Zero());
    stack_.push_back(base_);
    append_level_ = base_;
}

DocBuilder &DocBuilder::WithOps(const std::vector<Op *> &ops) {
    for (Op *op : ops) {
        switch (op->op_kind) {
        case OpKind::Open: stack_.push_back(arena_.Make<DocLevel>(static_cast<OpenOp *>(op)->plus_indent)); break;
        case OpKind::Close: {
            DocLevel *top = stack_.back();
            stack_.pop_back();
            stack_.back()->Add(top);
            break;
        }
        case OpKind::Break:
            append_level_ = stack_.back();
            append_level_->Add(static_cast<Doc *>(op));
            break;
        default: append_level_->Add(static_cast<Doc *>(op)); break;
        }
    }
    return *this;
}

// ---- JavaOutput --------------------------------------------------------------------------

JavaOutput::JavaOutput(std::string line_separator, const JavaInput &input, const JavaCommentsHelper &ch)
    : line_separator_(std::move(line_separator)), input_(input), kN_(input.kN()) {
    (void)ch;
}

void JavaOutput::BlankLine(int k, const BlankLineWanted &wanted) {
    auto it = blank_lines_.find(k);
    if (it != blank_lines_.end()) it->second = it->second.Merge(wanted);
    else blank_lines_.emplace(k, wanted);
}

void JavaOutput::MarkForPartialFormat(const Token &start, const Token &end) {
    const int lo = StartTok(start)->index;
    const int hi = EndTok(end)->index;
    // TreeRangeSet.add(Range.closed(lo, hi)): coalesce connected ranges.
    std::pair<int, int> r{lo, hi};
    std::vector<std::pair<int, int>> out;
    for (const auto &x : partial_format_ranges_) {
        if (x.second < r.first || r.second < x.first) {
            out.push_back(x);
        } else {
            r.first = std::min(r.first, x.first);
            r.second = std::max(r.second, x.second);
        }
    }
    out.push_back(r);
    std::sort(out.begin(), out.end());
    partial_format_ranges_ = std::move(out);
}

void JavaOutput::Append(std::string_view text, Range range) {
    if (!range.Empty()) {
        bool saw_newlines = false;
        const int iN = input_.LineCount();
        while (iLine_ < iN && (input_.GetRanges(iLine_).Empty() || input_.GetRanges(iLine_).hi <= range.lo)) {
            if (input_.GetRanges(iLine_).Empty()) saw_newlines = true;
            ++iLine_;
        }
        BlankLineWanted wanted = BlankLineWanted::Of(BlankLineWanted::No);
        auto it = blank_lines_.find(lastK_);
        if (it != blank_lines_.end()) wanted = it->second;
        const bool is_comment = StartsWith(text, "//") || StartsWith(text, "/*");
        if ((saw_newlines && is_comment) || wanted.Wanted().value_or(saw_newlines)) ++newlines_pending_;
    }
    if (newlines::IsNewline(text)) {
        if (newlines_pending_ == 0) ++newlines_pending_;
        spaces_pending_.clear();
    } else {
        bool ranges_set = false;
        const size_t n = text.size();
        for (size_t i = 0; i < n; i++) {
            const char c = text[i];
            switch (c) {
            case ' ': spaces_pending_.push_back(' '); break;
            case '\t': spaces_pending_.push_back('\t'); break;
            case '\r':
                if (i + 1 < n && text[i + 1] == '\n') i++;
                [[fallthrough]];
            case '\n':
                spaces_pending_.clear();
                ++newlines_pending_;
                break;
            default:
                while (newlines_pending_ > 0) {
                    if (!lines_.empty() || !line_builder_.empty()) lines_.push_back(line_builder_);
                    line_builder_.clear();
                    ranges_set = false;
                    --newlines_pending_;
                }
                if (!spaces_pending_.empty()) {
                    line_builder_ += spaces_pending_;
                    spaces_pending_.clear();
                }
                line_builder_.push_back(c);
                if (!range.Empty() && !ranges_set) {
                    while (ranges_.size() <= lines_.size()) ranges_.push_back(Range{});
                    ranges_[lines_.size()] = Union(ranges_[lines_.size()], range);
                    ranges_set = true;
                }
            }
        }
    }
    if (!range.Empty()) lastK_ = range.hi;
}

void JavaOutput::Flush() {
    if (IndexOfNonWs(line_builder_) >= 0) lines_.push_back(line_builder_);
    const size_t jN = lines_.size();
    while (ranges_.size() < jN) ranges_.push_back(Range{});
    ranges_.push_back(Range{kN_, kN_ + 1});
}

Range JavaOutput::GetRanges(int line) const {
    return 0 <= line && line < static_cast<int>(ranges_.size()) ? ranges_[static_cast<size_t>(line)] : Range{};
}

Range JavaOutput::ExpandToBreakableRegions(Range r) const {
    int lo_tok = r.lo;
    int hi_tok = r.hi - 1;
    const std::pair<int, int> *lo_r = nullptr, *hi_r = nullptr;
    for (const auto &x : partial_format_ranges_) {
        if (x.first <= lo_tok && lo_tok <= x.second) lo_r = &x;
        if (x.first <= hi_tok && hi_tok <= x.second) hi_r = &x;
    }
    if (!lo_r || !hi_r) return Range{};
    lo_tok = lo_r->first;
    hi_tok = hi_r->second;
    return Range{lo_tok, hi_tok + 1};
}

std::vector<Replacement> JavaOutput::GetFormatReplacements(const std::vector<Range> &token_ranges) const {
    std::vector<Replacement> result;
    // makeKToIJ
    std::unordered_map<int, Range> kToJ;
    const int ijN = LineCount();
    for (int ij = 0; ij <= ijN; ij++) {
        const Range range = GetRanges(ij);
        for (int k = range.lo; k < range.hi; k++) {
            auto it = kToJ.find(k);
            if (it != kToJ.end()) it->second = Range{it->second.lo, ij + 1};
            else kToJ.emplace(k, Range{ij, ij + 1});
        }
    }
    std::vector<Range> breakable;
    for (Range r : token_ranges) {
        r = Range{std::max(r.lo, 0), std::min(r.hi, input_.kN() + 1)};
        if (r.Empty()) continue;
        const Range range = ExpandToBreakableRegions(r);
        if (range == Range{}) continue;
        breakable.push_back(range);
    }
    std::sort(breakable.begin(), breakable.end(), [](Range a, Range b) { return a.lo < b.lo; });
    std::vector<Range> merged;
    for (Range r : breakable) {
        if (!merged.empty() && merged.back().hi >= r.lo) merged.back().hi = std::max(merged.back().hi, r.hi);
        else merged.push_back(r);
    }
    const std::string &text = input_.Text();
    for (Range range : merged) {
        const Tok *start_tok = StartTok(*input_.GetToken(range.lo));
        const Tok *end_tok = EndTok(*input_.GetToken(range.hi - 1));
        std::string replacement;
        int replace_from = start_tok->position;
        while (replace_from > 0) {
            size_t len = 1;
            if (!IsGuavaWhitespace(DecodeBefore(text, static_cast<size_t>(replace_from), &len))) break;
            replace_from -= static_cast<int>(len);
        }
        int i = kToJ.at(start_tok->index).lo;
        while (i > 0 && Line(i - 1).empty()) i--;
        for (; i < kToJ.at(end_tok->index).hi; i++) {
            if (i < LineCount()) {
                if (i > 0) replacement += line_separator_;
                replacement += Line(i);
            }
        }
        const int text_len = static_cast<int>(text.size());
        int replace_to = std::min(end_tok->position + static_cast<int>(end_tok->original_text.size()), text_len);
        if (end_tok->index == input_.kN() - 1) replace_to = text_len;
        int newline = -1;
        while (replace_to < text_len) {
            size_t len = 1;
            if (!IsGuavaWhitespace(DecodeAt(text, static_cast<size_t>(replace_to), &len))) break;
            const int nl = newlines::HasNewlineAt(text, static_cast<size_t>(replace_to));
            if (nl != -1) {
                newline = replace_to;
                replace_to += nl;
            } else {
                replace_to += static_cast<int>(len);
            }
        }
        if (newline != -1) replace_to = newline;
        if (newline == -1) replacement += line_separator_;
        for (; i < LineCount(); i++) {
            const std::string &after = Line(i);
            const int idx = IndexOfNonWs(after);
            if (idx == -1) {
                replacement += line_separator_;
            } else {
                if (newline == -1) replacement += after.substr(0, static_cast<size_t>(idx));
                break;
            }
        }
        result.push_back(Replacement{replace_from, replace_to, std::move(replacement)});
    }
    return result;
}

std::string ApplyReplacements(std::string_view input, std::vector<Replacement> replacements) {
    std::stable_sort(replacements.begin(), replacements.end(),
                     [](const Replacement &a, const Replacement &b) { return a.lo > b.lo; });
    std::string s(input);
    for (const Replacement &r : replacements)
        s.replace(static_cast<size_t>(r.lo), static_cast<size_t>(r.hi - r.lo), r.text);
    return s;
}

// ---- OpsBuilder ----------------------------------------------------------------------------

OpsBuilder::OpsBuilder(const JavaInput &input, JavaOutput &output, Arena &arena)
    : input_(input), output_(output), arena_(arena) {}

int OpsBuilder::ActualSize(int position, int length) const {
    const Token *start_token = input_.PositionToken(position);
    const Token *end_token = input_.PositionToken(position + length - 1);
    if (!start_token || !end_token) Fail("internal error: no token at a position");
    int start = start_token->tok->position;
    for (const Tok *tok : start_token->toks_before)
        if (tok->IsComment()) start = std::min(start, tok->position);
    int end = end_token->tok->position + static_cast<int>(end_token->tok->original_text.size());
    for (const Tok *tok : end_token->toks_after)
        if (tok->IsComment()) end = std::max(end, tok->position + static_cast<int>(tok->original_text.size()));
    if (end <= start) return end - start;
    return Len16(std::string_view(input_.Text()).substr(static_cast<size_t>(start), static_cast<size_t>(end - start)));
}

int OpsBuilder::ActualStartColumn(int position) const {
    const Token *start_token = input_.PositionToken(position);
    if (!start_token) Fail("internal error: no token at a position");
    int start = start_token->tok->position;
    const int line0 = input_.GetLineNumber(start);
    for (const Tok *tok : start_token->toks_before) {
        if (line0 != input_.GetLineNumber(tok->position)) return start;
        if (tok->IsComment()) start = std::min(start, tok->position);
    }
    return start;
}

void OpsBuilder::Add(Op *op) {
    if (op->op_kind == OpKind::Open) {
        depth_++;
    } else if (op->op_kind == OpKind::Close) {
        depth_--;
        if (depth_ < 0) Fail("internal error: unbalanced close");
    }
    ops_.push_back(op);
}

void OpsBuilder::CheckClosed(int previous) const {
    if (depth_ != previous) Fail("saw " + std::to_string(depth_) + " unclosed ops");
}

void OpsBuilder::Fail(const std::string &message) const {
    int line = 0;
    if (input_position_ >= 0) line = input_.GetLineNumber(input_position_);
    throw FormatError{message, line};
}

void OpsBuilder::Sync(int input_position) {
    if (input_position > input_position_) {
        const auto &tokens = input_.Tokens();
        input_position_ = input_position;
        if (static_cast<size_t>(tokenI_) < tokens.size() && input_position > tokens[static_cast<size_t>(tokenI_)].tok->position) {
            const Token &token = tokens[static_cast<size_t>(tokenI_++)];
            Fail("did not generate token \"" + token.tok->text + "\"");
        }
    }
}

void OpsBuilder::Drain() {
    const int input_position = static_cast<int>(input_.Text().size()) + 1;
    if (input_position > input_position_) {
        const auto &tokens = input_.Tokens();
        while (static_cast<size_t>(tokenI_) < tokens.size() &&
               input_position > tokens[static_cast<size_t>(tokenI_)].tok->position) {
            const Token &token = tokens[static_cast<size_t>(tokenI_++)];
            Add(arena_.Make<DocToken>(&token, false, arena_.Zero(), nullptr));
        }
    }
    input_position_ = input_position;
    CheckClosed(0);
}

void OpsBuilder::Open(const Indent *plus_indent) { Add(arena_.Make<OpenOp>(plus_indent)); }
void OpsBuilder::Close() { Add(arena_.Close()); }

std::optional<std::string_view> OpsBuilder::PeekToken(int skip) const {
    const auto &tokens = input_.Tokens();
    const size_t idx = static_cast<size_t>(tokenI_ + skip);
    if (idx < tokens.size()) return std::string_view(tokens[idx].tok->original_text);
    return std::nullopt;
}

const Token *OpsBuilder::CurrentToken() const {
    const auto &tokens = input_.Tokens();
    return static_cast<size_t>(tokenI_) < tokens.size() ? &tokens[static_cast<size_t>(tokenI_)] : nullptr;
}

void OpsBuilder::GuessToken(std::string_view token) { TokenOp(token, false, arena_.Zero(), nullptr); }

void OpsBuilder::TokenOp(std::string_view token, bool real, const Indent *plus_indent_comments_before,
                         const Indent *break_and_indent_trailing_comment) {
    const auto &tokens = input_.Tokens();
    const auto peek = PeekToken();
    if (peek && *peek == token) {
        Add(arena_.Make<DocToken>(&tokens[static_cast<size_t>(tokenI_++)], true, plus_indent_comments_before,
                                  break_and_indent_trailing_comment));
    } else if (real) {
        Fail("expected token: '" + std::string(peek.value_or("null")) + "'; generated " + std::string(token) +
             " instead");
    }
}

void OpsBuilder::OpChars(std::string_view op) {
    for (size_t i = 0; i < op.size(); i++) TokenOp(op.substr(i, 1), true, arena_.Zero(), nullptr);
}

void OpsBuilder::Space() { Add(arena_.Space()); }

void OpsBuilder::BreakOp(FillMode fill_mode, std::string flat, const Indent *plus_indent, BreakTag *tag) {
    Add(arena_.Make<DocBreak>(fill_mode, std::move(flat), plus_indent, tag));
}

void OpsBuilder::MarkForPartialFormat() {
    if (last_partial_format_boundary_ == -1) {
        last_partial_format_boundary_ = tokenI_;
        return;
    }
    if (tokenI_ == last_partial_format_boundary_) return;
    const auto &tokens = input_.Tokens();
    const Token &start = tokens[static_cast<size_t>(last_partial_format_boundary_)];
    const Token &end = tokens[static_cast<size_t>(tokenI_ - 1)];
    output_.MarkForPartialFormat(start, end);
    last_partial_format_boundary_ = tokenI_;
}

void OpsBuilder::BlankLineWantedOp(const BlankLineWanted &wanted) {
    const Token *token = CurrentToken();
    if (!token) return;
    output_.BlankLine(StartTok(*token)->index, wanted);
}

namespace {
bool IsForcedBreak(const Op *op) {
    return op->op_kind == OpKind::Break && static_cast<const DocBreak *>(op)->IsForced();
}
}  // namespace

std::vector<Op *> OpsBuilder::Build() {
    MarkForPartialFormat();
    const size_t opsN = ops_.size();
    std::vector<std::vector<Op *>> tok_ops(opsN + 1);
    auto make_comment = [&](std::vector<Op *> &dst, const Tok *comment) {
        dst.push_back(arena_.Make<DocTok>(comment));
        if (!comment->IsSlashStarComment()) dst.push_back(arena_.Make<DocBreak>(FillMode::Forced, "", arena_.Zero(), nullptr));
    };
    for (size_t i = 0; i < opsN; i++) {
        Op *op = ops_[i];
        if (op->op_kind != OpKind::Token) continue;
        auto *token_op = static_cast<DocToken *>(op);
        const Token *token = token_op->GetToken();
        size_t j = i;
        while (0 < j && ops_[j - 1]->op_kind == OpKind::Open) --j;
        size_t k = i;
        while (k + 1 < opsN && ops_[k + 1]->op_kind == OpKind::Close) ++k;
        if (token_op->Real()) {
            int nl = 0;
            bool space = false, last_was_comment = false, allow_blank_after_last_comment = false;
            for (const Tok *before : token->toks_before) {
                if (before->IsNewline()) {
                    nl++;
                } else if (before->IsComment()) {
                    tok_ops[j].push_back(arena_.Make<DocBreak>(
                        before->IsSlashSlashComment() ? FillMode::Forced : FillMode::Unified, "",
                        token_op->PlusIndentCommentsBefore(), nullptr));
                    make_comment(tok_ops[j], before);
                    space = before->IsSlashStarComment();
                    nl = 0;
                    last_was_comment = true;
                    if (before->IsJavadocComment())
                        tok_ops[j].push_back(arena_.Make<DocBreak>(FillMode::Forced, "", arena_.Zero(), nullptr));
                    allow_blank_after_last_comment =
                        before->IsSlashSlashComment() || (before->IsSlashStarComment() && !before->IsJavadocComment());
                }
            }
            if (allow_blank_after_last_comment && nl > 1)
                output_.BlankLine(token->tok->index, BlankLineWanted::Of(BlankLineWanted::Yes));
            if (last_was_comment && nl > 0)
                tok_ops[j].push_back(arena_.Make<DocBreak>(FillMode::Forced, "", arena_.Zero(), nullptr));
            else if (space)
                tok_ops[j].push_back(arena_.Space());
            for (const Tok *after : token->toks_after) {
                if (!after->IsComment()) continue;
                const bool break_after = after->IsJavadocComment() ||
                                         (after->IsSlashStarComment() && token_op->BreakAndIndentTrailingComment());
                if (break_after) {
                    const Indent *ind = token_op->BreakAndIndentTrailingComment();
                    tok_ops[k + 1].push_back(
                        arena_.Make<DocBreak>(FillMode::Forced, "", ind ? ind : arena_.Zero(), nullptr));
                } else {
                    tok_ops[k + 1].push_back(arena_.Space());
                }
                make_comment(tok_ops[k + 1], after);
                if (break_after)
                    tok_ops[k + 1].push_back(arena_.Make<DocBreak>(FillMode::Forced, "", arena_.Zero(), nullptr));
            }
        } else {
            int nl = 0;
            bool last_was_comment = false;
            for (const Tok *before : token->toks_before) {
                if (before->IsNewline()) {
                    nl++;
                } else if (before->IsComment()) {
                    nl = 0;
                    last_was_comment = true;
                }
                if (last_was_comment && nl > 0)
                    tok_ops[j].push_back(arena_.Make<DocBreak>(FillMode::Forced, "", arena_.Zero(), nullptr));
                tok_ops[j].push_back(arena_.Make<DocTok>(before));
            }
            for (const Tok *after : token->toks_after) tok_ops[k + 1].push_back(arena_.Make<DocTok>(after));
        }
    }
    std::vector<Op *> out;
    bool after_forced_break = false;
    for (size_t i = 0; i < opsN; i++) {
        for (Op *op : tok_ops[i]) {
            if (!(after_forced_break && op->op_kind == OpKind::Space)) {
                out.push_back(op);
                after_forced_break = IsForcedBreak(op);
            }
        }
        Op *op = ops_[i];
        if (after_forced_break &&
            (op->op_kind == OpKind::Space ||
             (op->op_kind == OpKind::Break && static_cast<DocBreak *>(op)->GetPlusIndent() == 0 &&
              static_cast<DocBreak *>(op)->GetFlat() == " ")))
            continue;
        out.push_back(op);
        if (op->op_kind != OpKind::Open) after_forced_break = IsForcedBreak(op);
    }
    for (Op *op : tok_ops[opsN]) {
        if (!(after_forced_break && op->op_kind == OpKind::Space)) {
            out.push_back(op);
            after_forced_break = IsForcedBreak(op);
        }
    }
    return out;
}

}  // namespace javafmt::gjf
