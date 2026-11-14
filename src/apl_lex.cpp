// APL tokenizer: see apl_lex.h.
#include "apl_lex.h"

#include <cctype>
#include <cstring>

namespace apl {

int Utf8Len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

bool IsNameStart(const std::string &cp) {
    if (cp.size() == 1) {
        const unsigned char c = static_cast<unsigned char>(cp[0]);
        return std::isalpha(c) || c == '_';
    }
    return cp == "∆" || cp == "⍙" || cp == "Ⓐ" || cp == "_";
}

bool IsNameChar(const std::string &cp) {
    if (IsNameStart(cp)) return true;
    if (cp.size() == 1) return std::isdigit(static_cast<unsigned char>(cp[0])) != 0;
    return cp == "¯";
}

namespace {

const char *const kPrimitives[] = {
    "+", "-", "−", "×", "÷", "⌈", "⌊", "*", "⍟", "|", "!", "○", "~", "∼", "∨", "∧", "⍱", "⍲", "<", "≤",
    "=", "≥", ">", "≠", "≡", "≢", "⍴", ",", "⍪", "⌽", "⊖", "⍉", "↑", "↓", "⊂", "⊃", "⊆", "⊇", "⌷", "⍋",
    "⍒", "⍳", "⍸", "∊", "⍷", "∪", "∩", "⊣", "⊢", "⍎", "⍕", "⌹", "?", "⊥", "⊤", "⍞", "⍯",
};
const char *const kOperators[] = {
    "/", "\\", "⌿", "⍀", "¨", "⍨", "⍣", ".", "∘", "⍤", "⍥", "@", "⌸", "⌺", "⍠", "&", "⌶", "⍛",
};

bool In(const std::string &cp, const char *const *list, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (cp == list[i]) return true;
    }
    return false;
}

std::string CodePoint(const std::string &s, size_t i) {
    if (i >= s.size()) return std::string();
    size_t n = static_cast<size_t>(Utf8Len(static_cast<unsigned char>(s[i])));
    if (i + n > s.size()) n = s.size() - i;
    return s.substr(i, n);
}

bool IsDigit(const std::string &s, size_t i) {
    return i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]));
}

// Scans a number (without the leading ¯) at i; returns the end.
size_t ScanReal(const std::string &s, size_t i) {
    while (IsDigit(s, i)) i++;
    if (i < s.size() && s[i] == '.' && (IsDigit(s, i + 1) || (i > 0 && IsDigit(s, i - 1)))) {
        i++;
        while (IsDigit(s, i)) i++;
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        size_t j = i + 1;
        if (s.compare(j, 2, "¯") == 0) j += 2;
        if (IsDigit(s, j)) {
            i = j;
            while (IsDigit(s, i)) i++;
        }
    }
    return i;
}

bool StartsNumber(const std::string &s, size_t i) {
    if (IsDigit(s, i)) return true;
    return i < s.size() && s[i] == '.' && IsDigit(s, i + 1);
}

size_t ScanName(const std::string &s, size_t i) {
    while (i < s.size()) {
        const std::string cp = CodePoint(s, i);
        if (!IsNameChar(cp)) break;
        i += cp.size();
    }
    return i;
}

}  // namespace

std::vector<Token> LexLine(const std::string &line) {
    std::vector<Token> out;
    const std::string &s = line;
    size_t i = 0;
    auto push = [&](Tok kind, size_t start, size_t end) {
        Token t;
        t.kind = kind;
        t.col = static_cast<int>(start);
        t.end = static_cast<int>(end);
        t.text = s.substr(start, end - start);
        out.push_back(t);
    };
    // )command and ]user command: a line starting with ) or ] and a letter.
    {
        size_t j = 0;
        while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) j++;
        if (j + 1 < s.size() && (s[j] == ')' || s[j] == ']') && std::isalpha(static_cast<unsigned char>(s[j + 1]))) {
            size_t e = s.size();
            while (e > j && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) e--;
            push(Tok::SysCommand, j, e);
            return out;
        }
        if (s.compare(0, 2, "#!") == 0) {
            push(Tok::Comment, 0, s.size());
            return out;
        }
    }
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f') {
            i++;
            continue;
        }
        const std::string cp = CodePoint(s, i);
        const size_t start = i;
        if (cp == "⍝") {
            push(Tok::Comment, i, s.size());
            break;
        }
        if (c == '\'' || c == '"') {
            size_t j = i + 1;
            bool closed = false;
            while (j < s.size()) {
                if (c == '"' && s[j] == '\\' && j + 1 < s.size()) {
                    j += 2;
                    continue;
                }
                if (static_cast<unsigned char>(s[j]) == c) {
                    if (c == '\'' && j + 1 < s.size() && s[j + 1] == '\'') {
                        j += 2;
                        continue;
                    }
                    closed = true;
                    j++;
                    break;
                }
                j++;
            }
            push(closed ? Tok::String : Tok::BadString, i, j);
            i = j;
            continue;
        }
        // Numbers: ¯1.5E¯3J2
        if (StartsNumber(s, i) || (cp == "¯" && (StartsNumber(s, i + 2) || s.compare(i + 2, 3, "∞") == 0))) {
            size_t j = i;
            if (cp == "¯") j += 2;
            if (s.compare(j, 3, "∞") == 0) {
                j += 3;
            } else {
                j = ScanReal(s, j);
            }
            if (j < s.size() && (s[j] == 'j' || s[j] == 'J')) {
                size_t k = j + 1;
                if (s.compare(k, 2, "¯") == 0) k += 2;
                if (StartsNumber(s, k)) j = ScanReal(s, k);
            }
            push(Tok::Number, i, j);
            i = j;
            continue;
        }
        // Names, dotted names.
        const bool quad = cp == "⎕";
        if (IsNameStart(cp) || quad || c == '#') {
            size_t j = i;
            Tok kind = Tok::Name;
            if (quad) {
                j += cp.size();
                while (j < s.size() && std::isalpha(static_cast<unsigned char>(s[j]))) j++;
                kind = Tok::SysName;
            } else if (c == '#') {
                j++;
                if (j < s.size() && s[j] == '#') j++;
                kind = Tok::Root;
            } else {
                j = ScanName(s, i);
            }
            // .name continuations
            bool dotted = false;
            while (j + 1 < s.size() && s[j] == '.' && IsNameStart(CodePoint(s, j + 1))) {
                j = ScanName(s, j + 1);
                dotted = true;
            }
            if (dotted) kind = Tok::QualName;
            push(kind, i, j);
            i = j;
            continue;
        }
        if (c == ':') {
            // :Keyword, or :: / :
            size_t j = i + 1;
            if (j < s.size() && s[j] == ':') {
                push(Tok::DoubleColon, i, j + 1);
                i = j + 1;
                continue;
            }
            // A keyword only where a statement could start: after nothing, a
            // diamond, or (:In) after a :For's names. A guard's colon follows
            // an expression; `x:If` isn't a keyword there.
            bool keyword_pos = out.empty() || out.back().kind == Tok::Diamond;
            if (!keyword_pos && !out.empty()) {
                // :In / :InEach after `:For names`
                for (const Token &t : out) {
                    if (t.kind == Tok::Keyword) {
                        std::string up;
                        for (char ch : t.text) up += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                        if (up == ":FOR") keyword_pos = true;
                    }
                }
                // after a label `L:` at the line start
                if (out.size() == 2 && out[0].kind == Tok::Name && out[1].kind == Tok::Colon) keyword_pos = true;
                // `:Class Name : Base`: a colon followed by a space is never a keyword
            }
            if (keyword_pos && j < s.size() && std::isalpha(static_cast<unsigned char>(s[j]))) {
                while (j < s.size() && std::isalpha(static_cast<unsigned char>(s[j]))) j++;
                push(Tok::Keyword, i, j);
                i = j;
                continue;
            }
            push(Tok::Colon, i, i + 1);
            i++;
            continue;
        }
        i += cp.size();
        if (cp == "←") push(Tok::Assign, start, i);
        else if (cp == "→") push(Tok::Branch, start, i);
        else if (cp == "⋄" || cp == "◊") push(Tok::Diamond, start, i);  // ◊: GNU APL's diamond
        else if (cp == "(") push(Tok::LParen, start, i);
        else if (cp == ")") push(Tok::RParen, start, i);
        else if (cp == "[") push(Tok::LBracket, start, i);
        else if (cp == "]") push(Tok::RBracket, start, i);
        else if (cp == "{") push(Tok::LBrace, start, i);
        else if (cp == "}") push(Tok::RBrace, start, i);
        else if (cp == ";") push(Tok::Semicolon, start, i);
        else if (cp == "⍫") push(Tok::Lock, start, i);
        else if (cp == "⍬") push(Tok::Zilde, start, i);
        else if (cp == "∇") {
            if (s.compare(i, 3, "∇") == 0) {
                i += 3;
                push(Tok::DelDel, start, i);
            } else {
                push(Tok::Del, start, i);
            }
        } else if (cp == "⍺" || cp == "⍵") {
            if (s.compare(i, cp.size(), cp) == 0) i += cp.size();
            push(Tok::Special, start, i);
        } else if (cp == "λ" || cp == "χ") {
            push(Tok::Special, start, i);
        } else if (In(cp, kPrimitives, sizeof(kPrimitives) / sizeof(*kPrimitives))) {
            push(Tok::Primitive, start, i);
        } else if (In(cp, kOperators, sizeof(kOperators) / sizeof(*kOperators))) {
            push(Tok::Operator, start, i);
        } else {
            push(Tok::Unknown, start, i);
        }
    }
    return out;
}

}  // namespace apl
