// mep's Fortran formatter -- see fortran_format.h.
//
// The layout rules are rebuilt from fprettify 0.3.7's documented and observed
// behaviour (README, --help and black-box runs over a 11k-file corpus); no
// code was taken from it. fprettify works in two passes over the whole file,
// and so does this: the first normalises whitespace inside each statement
// and keeps every line where it was, the second re-reads that result and
// re-indents it by scope, aligning continuation lines under the bracket,
// `=` or `::` they continue. Matching it byte for byte means reproducing its
// heuristics exactly, quirks included; the comments below say what each step
// does in Fortran terms.
//
// Text is handled as UTF-32 so that columns count characters, as fprettify's
// do. Matching is hand-written (no regex engine), linear in the line length
// apart from a few bounded look-backs, so hostile input cannot make it hang.

#include "fortran_format.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fortranfmt {
namespace {

using UStr = std::u32string;
using SV = std::u32string_view;
using L = long;

// ---------------------------------------------------------------------------
// Failure: the places where fprettify itself gives up on a file.

struct Failure {
    std::string msg;
    int line = 0;
};

[[noreturn]] void Fail(const char *msg, int line = 0) { throw Failure{msg, line}; }

// ---------------------------------------------------------------------------
// Character classes, as Python's str.isspace() and the regex \w see them.

bool IsSpace(char32_t c) {
    switch (c) {
    case 9: case 10: case 11: case 12: case 13: case 0x1c: case 0x1d: case 0x1e: case 0x1f:
    case 0x20: case 0x85: case 0xa0: case 0x1680: case 0x2028: case 0x2029: case 0x202f:
    case 0x205f: case 0x3000:
        return true;
    default:
        return c >= 0x2000 && c <= 0x200a;
    }
}

bool IsWord(char32_t c) {
    if (c < 0x80) return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    if (IsSpace(c)) return false;
    if (c < 0xC0)
        return c == 0xAA || c == 0xB2 || c == 0xB3 || c == 0xB5 || c == 0xB9 || c == 0xBA || c == 0xBC ||
               c == 0xBD || c == 0xBE;
    if (c == 0xD7 || c == 0xF7) return false;
    if (c >= 0x2000 && c <= 0x206F) return false;  // general punctuation
    if (c >= 0x20A0 && c <= 0x20FF) return false;  // currency, combining marks for symbols
    if (c >= 0x2190 && c <= 0x2BFF) return false;  // arrows, maths, box drawing, shapes
    if (c >= 0x3000 && c <= 0x303F) return false;
    if (c >= 0xFF00 && c <= 0xFF0F) return false;
    if (c == 0xFEFF || (c >= 0xFFF0 && c <= 0xFFFF)) return false;  // byte-order mark, specials
    if (c >= 0xE000 && c <= 0xF8FF) return false;
    if (c >= 0x1F000) return false;
    return true;
}

bool IsDigit(char32_t c) { return c >= '0' && c <= '9'; }
char32_t Low(char32_t c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

L Len(SV s) { return static_cast<L>(s.size()); }
char32_t At(SV s, L i) { return (i >= 0 && i < Len(s)) ? s[static_cast<size_t>(i)] : 0; }
size_t Z(L i) { return static_cast<size_t>(i); }

/** @brief s[a:b] with Python's clamping for non-negative bounds. */
UStr Sub(SV s, L a, L b = -1) {
    const L n = Len(s);
    if (b < 0 || b > n) b = n;
    if (a < 0) a = 0;
    if (a >= b) return {};
    return UStr(s.substr(Z(a), Z(b - a)));
}

/** @brief Case-insensitive (ASCII) match of the lower-case literal `kw` at i. */
bool Kw(SV s, L i, const char *kw) {
    if (i < 0) return false;
    for (L k = 0; kw[k]; k++)
        if (Low(At(s, i + k)) != static_cast<char32_t>(static_cast<unsigned char>(kw[k])) || i + k >= Len(s))
            return false;
    return true;
}
L KwLen(const char *kw) { return static_cast<L>(std::char_traits<char>::length(kw)); }

L SkipWs(SV s, L i) {
    while (i < Len(s) && IsSpace(s[Z(i)])) i++;
    return i;
}
L SkipWord(SV s, L i) {
    while (i < Len(s) && IsWord(s[Z(i)])) i++;
    return i;
}
L BackWs(SV s, L i) {  // first index of the whitespace run ending at i
    while (i > 0 && IsSpace(s[Z(i - 1)])) i--;
    return i;
}
L SkipSpaces(SV s, L i) {  // ' ' only
    while (i < Len(s) && s[Z(i)] == ' ') i++;
    return i;
}

UStr StripIf(SV s, bool left, bool right, bool (*pred)(char32_t)) {
    L a = 0, b = Len(s);
    if (left)
        while (a < b && pred(s[Z(a)])) a++;
    if (right)
        while (b > a && pred(s[Z(b - 1)])) b--;
    return UStr(s.substr(Z(a), Z(b - a)));
}
bool IsBlank(char32_t c) { return c == ' '; }
bool IsAmp(char32_t c) { return c == '&'; }
bool IsNl(char32_t c) { return c == '\n'; }
UStr Strip(SV s) { return StripIf(s, true, true, IsSpace); }
UStr LStrip(SV s) { return StripIf(s, true, false, IsSpace); }
UStr RStrip(SV s) { return StripIf(s, false, true, IsSpace); }
UStr StripSp(SV s) { return StripIf(s, true, true, IsBlank); }
UStr LStripSp(SV s) { return StripIf(s, true, false, IsBlank); }
UStr RStripSp(SV s) { return StripIf(s, false, true, IsBlank); }
UStr StripAmp(SV s) { return StripIf(s, true, true, IsAmp); }
UStr LStripAmp(SV s) { return StripIf(s, true, false, IsAmp); }
UStr RStripNl(SV s) { return StripIf(s, false, true, IsNl); }
bool AllWs(SV s) {
    for (char32_t c : s)
        if (!IsSpace(c)) return false;
    return true;
}
bool StartsWith(SV s, const char *lit) {
    for (L k = 0; lit[k]; k++)
        if (At(s, k) != static_cast<char32_t>(static_cast<unsigned char>(lit[k])) || k >= Len(s)) return false;
    return true;
}
L LeadingSpaces(SV s) {
    L k = 0;
    while (k < Len(s) && s[Z(k)] == ' ') k++;
    return k;
}
UStr Spaces(L n) { return n > 0 ? UStr(Z(n), U' ') : UStr(); }

// ---------------------------------------------------------------------------
// Line-start tests: comments, preprocessor and fypp directives.

/** @brief Does a comment-like construct start at s[i]? `fypp` also counts fypp's `#:`/`$:`/`@:`/`#!`. */
bool NotFortranAt(SV s, L i, bool fypp) {
    const char32_t c = At(s, i);
    if (i >= Len(s)) return false;
    if (c == '!') return true;
    const bool two = i + 1 < Len(s);
    const char32_t d = At(s, i + 1);
    if (c == '#' && two) {
        if (fypp) return d != '{' && d != '}';
        return d != '!' && d != ':' && d != '{' && d != '}';
    }
    if (fypp && two && (c == '$' || c == '@') && d == ':') return true;
    return false;
}
bool FyppLineAt(SV s, L i) {
    if (i + 1 >= Len(s)) return false;
    const char32_t c = s[Z(i)], d = s[Z(i + 1)];
    return (c == '#' && (d == '!' || d == ':')) || ((c == '$' || c == '@') && d == ':');
}
bool FyppOpenAt(SV s, L i) {
    const char32_t c = At(s, i);
    return i + 1 < Len(s) && (c == '#' || c == '$' || c == '@') && s[Z(i + 1)] == '{';
}
bool FyppCloseAt(SV s, L i) {
    const char32_t d = At(s, i + 1);
    return i + 1 < Len(s) && s[Z(i)] == '}' && (d == '#' || d == '$' || d == '@');
}
bool CppLine(SV s) {  // a line (already left-stripped) that is a C preprocessor directive
    if (Len(s) < 2 || s[0] != '#') return false;
    const char32_t d = s[1];
    return d != '!' && d != ':' && d != '{' && d != '}';
}
// A part of a statement that starts a string or comment (left alone by the operator spacing).
bool StrOpen(SV s) {
    const char32_t c = At(s, 0);
    if (s.empty()) return false;
    if (c == '\'' || c == '"' || c == '!') return true;
    return FyppOpenAt(s, 0);
}

// ---------------------------------------------------------------------------
// The character walk every step uses: yields the characters of a line that
// are code, skipping string literals (and fypp `${ }$` inlines) and stopping
// at a comment. Its string state deliberately survives Reset(), which is how
// a string continued onto the next line is followed.

class CodeChars {
public:
    explicit CodeChars(SV s, bool stop_at_comment = true, bool skip_strings = true, bool fypp = true)
        : s_(s), stop_(stop_at_comment), skip_(skip_strings), fypp_(fypp) {}

    void Reset(SV s) {
        s_ = s;
        it_ = 0;
    }

    bool Next(L &pos, char32_t &ch) {
        for (;;) {
            if (it_ >= Len(s_)) return false;
            const L p = it_++;
            const char32_t c = s_[Z(p)];
            if (quote_.empty()) {
                if (!comment_) {
                    if (FyppOpenAt(s_, p)) {
                        quote_ = UStr(s_.substr(Z(p), 2));
                        fypp_inline_ = true;
                    } else if (NotFortranAt(s_, p, fypp_)) {
                        comment_ = true;
                    } else if (c == '"' || c == '\'') {
                        quote_ = UStr(1, c);
                    }
                }
            } else if (fypp_inline_) {
                if (FyppCloseAt(s_, p)) {
                    quote_.clear();
                    fypp_inline_ = false;
                    if (skip_) {
                        L p2 = 0;
                        char32_t c2 = 0;
                        if (!Next(p2, c2)) return false;
                        continue;
                    }
                }
            } else if ((c == '"' || c == '\'') && quote_.size() == 1 && quote_[0] == c) {
                quote_.clear();
                if (skip_) continue;
            }
            if (stop_ && comment_) return false;
            if (skip_ && !quote_.empty()) continue;
            pos = p;
            ch = c;
            return true;
        }
    }

    bool InString() const { return !quote_.empty(); }

private:
    SV s_;
    L it_ = 0;
    UStr quote_;
    bool fypp_inline_ = false;
    bool comment_ = false;
    bool stop_, skip_, fypp_;
};

UStr CodeOnly(SV s, bool fypp) {
    UStr out;
    CodeChars cc(s, true, true, fypp);
    L p = 0;
    char32_t c = 0;
    while (cc.Next(p, c)) out += c;
    return out;
}

// ---------------------------------------------------------------------------
// Statement shape tests. Each is a hand-written test for one statement form
// (case-insensitive, `^`-anchored at the statement start where the form is).

/** @brief Index from which the rest of s is blanks with at most one ';' (the statement end). */
L EolStart(SV s) {
    L r = BackWs(s, Len(s));
    if (r > 0 && s[Z(r - 1)] == ';') r = BackWs(s, r - 1);
    return r;
}
bool IsEol(SV, L i, L q) { return i >= q; }

/** @brief `name :` statement-label prefix, tried both with and without. */
template <class F>
bool WithOptLabel(SV s, F &&rest) {
    const L i = SkipWs(s, 0);
    if (rest(i)) return true;
    if (IsWord(At(s, i))) {
        const L k = SkipWs(s, SkipWord(s, i));
        if (At(s, k) == ':') return rest(SkipWs(s, k + 1));
    }
    return false;
}

/** @brief Is there a ')' after `open` that ends the statement? */
bool ParenToEol(SV s, L open, L q) { return q - 1 > open && At(s, q - 1) == ')'; }

/** @brief `( ... ) THEN` to the statement end, the '(' at open. */
bool ParenThenEol(SV s, L open, L q) {
    L r = q;
    if (r < 4 || !Kw(s, r - 4, "then")) return false;
    r = BackWs(s, r - 4);
    return r - 1 > open && At(s, r - 1) == ')';
}

bool IfThenRe(SV s) {
    const L q = EolStart(s);
    return WithOptLabel(s, [&](L i) {
        if (!Kw(s, i, "if")) return false;
        const L j = SkipWs(s, i + 2);
        return At(s, j) == '(' && ParenThenEol(s, j, q);
    });
}
bool ElseRe(SV s) {
    const L q = EolStart(s);
    const L i = SkipWs(s, 0);
    if (!Kw(s, i, "else")) return false;
    if (IsEol(s, i + 4, q)) return true;
    const L k = SkipWs(s, i + 4);
    if (!Kw(s, k, "if")) return false;
    const L m = SkipWs(s, k + 2);
    return At(s, m) == '(' && ParenThenEol(s, m, q);
}
/** @brief END <kw> [name], with or without the blank (`endif`, `end if foo`). */
bool EndKwName(SV s, const char *kw, bool name) {
    const L q = EolStart(s);
    const L i = SkipWs(s, 0);
    if (!Kw(s, i, "end")) return false;
    const L j = SkipWs(s, i + 3);
    if (!Kw(s, j, kw)) return false;
    const L k = j + KwLen(kw);
    if (IsEol(s, k, q)) return true;
    if (!name || !IsSpace(At(s, k))) return false;
    const L m = SkipWs(s, k);
    if (!IsWord(At(s, m))) return false;
    return IsEol(s, SkipWord(s, m), q);
}
bool DoRe(SV s) {
    const L q = EolStart(s);
    return WithOptLabel(s, [&](L i) {
        if (!Kw(s, i, "do")) return false;
        const L j = i + 2;
        if (IsEol(s, j, q)) return true;
        return IsSpace(At(s, j)) && IsWord(At(s, SkipWs(s, j)));
    });
}
bool SelectRe(SV s) {
    const L q = EolStart(s);
    const L i = SkipWs(s, 0);
    if (!Kw(s, i, "select")) return false;
    const L j = SkipWs(s, i + 6);
    if (!Kw(s, j, "case") && !Kw(s, j, "rank") && !Kw(s, j, "type")) return false;
    const L k = SkipWs(s, j + 4);
    return At(s, k) == '(' && ParenToEol(s, k, q);
}
bool CaseRe(SV s) {
    const L q = EolStart(s);
    const L i = SkipWs(s, 0);
    auto tail = [&](L j) {
        const L k = SkipWs(s, j);
        if (At(s, k) == '(' && ParenToEol(s, k, q)) return true;
        return Kw(s, k, "default") && IsEol(s, k + 7, q);
    };
    if (Kw(s, i, "case") && tail(i + 4)) return true;
    if (Kw(s, i, "rank") && tail(i + 4)) return true;
    for (const char *kw : {"type", "class"}) {
        if (!Kw(s, i, kw)) continue;
        const L j = i + KwLen(kw);
        if (!IsSpace(At(s, j))) continue;
        const L m = SkipWs(s, j);
        if (Kw(s, m, "is") && tail(m + 2)) return true;
        if (kw[0] == 'c' && Kw(s, m, "default") && IsEol(s, m + 7, q)) return true;
    }
    return false;
}
bool EndSelectRe(SV s) { return EndKwName(s, "select", false); }
bool AssociateRe(SV s) {
    const L q = EolStart(s);
    const L i = SkipWs(s, 0);
    if (!Kw(s, i, "associate")) return false;
    const L k = SkipWs(s, i + 9);
    return At(s, k) == '(' && ParenToEol(s, k, q);
}
bool EndAssociateRe(SV s) { return EndKwName(s, "associate", false); }
bool BlockRe(SV s) {
    const L q = EolStart(s);
    return WithOptLabel(s, [&](L i) { return Kw(s, i, "block") && IsEol(s, i + 5, q); });
}
bool EndBlockRe(SV s) { return EndKwName(s, "block", true); }

/** @brief Every place `kw` may start a procedure header: at 0, or after a blank with no quote before. */
template <class F>
bool ProcHeader(SV s, const char *kw, F &&rest) {
    const L n = Len(s), kl = KwLen(kw);
    for (L k = 0; k + kl <= n; k++) {
        const char32_t c = s[Z(k)];
        if (c == '"' || c == '\'') return false;  // nothing after a quote can follow the prefix
        if (k > 0 && s[Z(k - 1)] != ' ') continue;
        if (Kw(s, k, kw) && rest(k + kl)) return true;
    }
    return false;
}
bool SubroutineRe(SV s) {
    const L q = EolStart(s);
    return ProcHeader(s, "subroutine", [&](L j) {
        if (!IsSpace(At(s, j))) return false;
        const L m = SkipWs(s, j);
        if (!IsWord(At(s, m))) return false;
        const L e = SkipWs(s, SkipWord(s, m));
        return IsEol(s, e, q) || (At(s, e) == '(' && ParenToEol(s, e, q));
    });
}
bool EndSubroutineRe(SV s) { return EndKwName(s, "subroutine", true); }

/** @brief `RESULT ( name )` to the statement end, starting at i. */
bool ResultTail(SV s, L i, L q) {
    const L r = SkipWs(s, i);
    if (!Kw(s, r, "result")) return false;
    const L t = SkipWs(s, r + 6);
    if (At(s, t) != '(' || !IsWord(At(s, t + 1))) return false;
    const L w = SkipWord(s, t + 1);
    return At(s, w) == ')' && IsEol(s, w + 1, q);
}
bool FunctionRe(SV s) {
    const L q = EolStart(s);
    return ProcHeader(s, "function", [&](L j) {
        if (!IsSpace(At(s, j))) return false;
        const L m = SkipWs(s, j);
        if (!IsWord(At(s, m))) return false;
        const L e = SkipWs(s, SkipWord(s, m));
        if (IsEol(s, e, q) || ResultTail(s, e, q)) return true;
        if (At(s, e) != '(') return false;
        if (ParenToEol(s, e, q)) return true;
        // ( ... ) RESULT ( name ): find the RESULT clause from the end.
        if (At(s, q - 1) != ')') return false;
        L w = q - 1;
        const L wend = w;
        while (w > 0 && IsWord(s[Z(w - 1)])) w--;
        if (w == wend || At(s, w - 1) != '(') return false;
        L r = BackWs(s, w - 1);
        if (r < 6 || !Kw(s, r - 6, "result")) return false;
        r = BackWs(s, r - 6);
        return r - 1 > e && At(s, r - 1) == ')';
    });
}
bool EndFunctionRe(SV s) { return EndKwName(s, "function", true); }

bool KwThenName(SV s, const char *kw) {  // ^ KW <blanks> name EOL
    const L q = EolStart(s);
    const L i = SkipWs(s, 0);
    if (!Kw(s, i, kw)) return false;
    const L j = i + KwLen(kw);
    if (!IsSpace(At(s, j))) return false;
    const L m = SkipWs(s, j);
    if (!IsWord(At(s, m))) return false;
    return IsEol(s, SkipWord(s, m), q);
}
bool ModuleRe(SV s) { return KwThenName(s, "module"); }
bool EndModuleRe(SV s) { return EndKwName(s, "module", true); }
bool ProgramRe(SV s) { return KwThenName(s, "program"); }
bool EndProgramRe(SV s) { return EndKwName(s, "program", true); }
bool SubmoduleRe(SV s) {
    const L q = EolStart(s);
    const L i = SkipWs(s, 0);
    if (!Kw(s, i, "submodule")) return false;
    const L j = SkipWs(s, i + 9);
    if (At(s, j) != '(' || !IsWord(At(s, j + 1))) return false;
    const L w = SkipWord(s, j + 1);
    if (At(s, w) != ')' || !IsSpace(At(s, w + 1))) return false;
    const L m = SkipWs(s, w + 1);
    return IsWord(At(s, m)) && IsEol(s, SkipWord(s, m), q);
}
bool EndSubmoduleRe(SV s) { return EndKwName(s, "submodule", true); }

/** @brief s[a:b] is `\s+` or `\s*::\s*` (what separates a derived-type header from its name). */
bool TypeSep(SV s, L a, L b) {
    if (a >= b) return false;
    L i = SkipWs(s, a);
    if (i >= b) return true;
    if (At(s, i) != ':' || At(s, i + 1) != ':' || i + 2 > b) return false;
    i = SkipWs(s, i + 2);
    return i >= b;
}
bool TypeRe(SV s) {
    const L q = EolStart(s);
    const L i = SkipWs(s, 0);
    if (!Kw(s, i, "type")) return false;
    L w0 = q;
    while (w0 > 0 && IsWord(s[Z(w0 - 1)])) w0--;
    if (w0 == q || w0 < i + 4) return false;
    // Attribute list: (\s*,\s*(BIND(C)|EXTENDS(...)|ABSTRACT|PUBLIC|PRIVATE))* then an optional comma.
    std::vector<char> seen(Z(Len(s) + 1), 0);
    std::vector<L> todo{i + 4};
    while (!todo.empty()) {
        const L p = todo.back();
        todo.pop_back();
        if (p > w0 || seen[Z(p)]) continue;
        seen[Z(p)] = 1;
        if (TypeSep(s, p, w0)) return true;
        const L k = SkipWs(s, p);
        if (At(s, k) != ',') continue;
        if (TypeSep(s, k + 1, w0)) return true;
        const L m = SkipWs(s, k + 1);
        if (Kw(s, m, "bind")) {
            L t = SkipWs(s, m + 4);
            if (At(s, t) == '(') {
                t = SkipWs(s, t + 1);
                if (Low(At(s, t)) == 'c') {
                    t = SkipWs(s, t + 1);
                    if (At(s, t) == ')') todo.push_back(t + 1);
                }
            }
        }
        if (Kw(s, m, "extends")) {
            const L t = SkipWs(s, m + 7);
            if (At(s, t) == '(')
                for (L p2 = t + 1; p2 < w0; p2++) {
                    if (s[Z(p2)] == '\n') break;
                    if (s[Z(p2)] == ')') todo.push_back(p2 + 1);
                }
        }
        for (const char *kw : {"abstract", "public", "private"})
            if (Kw(s, m, kw)) todo.push_back(m + KwLen(kw));
    }
    return false;
}
bool EndTypeRe(SV s) { return EndKwName(s, "type", true); }

/** @brief The optional `name` / `OPERATOR(...)` / `ASSIGNMENT(...)` after INTERFACE, then EOL. */
bool InterfaceTail(SV s, L j, L q) {
    if (IsEol(s, j, q)) return true;
    if (!IsSpace(At(s, j))) return false;
    const L m = SkipWs(s, j);
    if (IsWord(At(s, m)) && IsEol(s, SkipWord(s, m), q)) return true;
    for (const char *kw : {"operator", "assignment"}) {
        if (!Kw(s, m, kw)) continue;
        const L t = SkipWs(s, m + KwLen(kw));
        if (At(s, t) == '(' && ParenToEol(s, t, q)) return true;
    }
    return false;
}
bool InterfaceRe(SV s) {
    const L q = EolStart(s);
    return ProcHeader(s, "interface", [&](L j) { return InterfaceTail(s, j, q); });
}
bool EndInterfaceRe(SV s) {
    const L q = EolStart(s);
    const L i = SkipWs(s, 0);
    if (!Kw(s, i, "end")) return false;
    const L j = SkipWs(s, i + 3);
    return Kw(s, j, "interface") && InterfaceTail(s, j + 9, q);
}
bool ContainsRe(SV s) {
    const L i = SkipWs(s, 0);
    return Kw(s, i, "contains") && IsEol(s, i + 8, EolStart(s));
}
bool EnumRe(SV s) {
    const L q = EolStart(s);
    const L i = SkipWs(s, 0);
    if (!Kw(s, i, "enum")) return false;
    std::vector<L> starts{i + 4};
    {
        const L k = SkipWs(s, i + 4);
        if (At(s, k) == ',') {
            L t = SkipWs(s, k + 1);
            if (Kw(s, t, "bind")) {
                t = SkipWs(s, t + 4);
                if (At(s, t) == '(') {
                    t = SkipWs(s, t + 1);
                    if (Low(At(s, t)) == 'c') {
                        t = SkipWs(s, t + 1);
                        if (At(s, t) == ')') starts.push_back(t + 1);
                    }
                }
            }
        }
    }
    L w0 = q;
    while (w0 > 0 && IsWord(s[Z(w0 - 1)])) w0--;
    for (const L p : starts) {
        if (IsEol(s, p, q)) return true;
        if (w0 < q && TypeSep(s, p, w0)) return true;
    }
    return false;
}
bool EndEnumRe(SV s) { return EndKwName(s, "enum", true); }
bool EndAnyRe(SV s) {
    const L i = SkipWs(s, 0);
    return Kw(s, i, "end") && IsEol(s, i + 3, EolStart(s));
}

/** @brief WHERE/FORALL: a block only when the mask's ')' ends the statement. */
bool MaskBlock(SV s) {
    int level = 0;
    CodeChars cc(s);
    L pos = 0;
    char32_t c = 0;
    while (cc.Next(pos, c)) {
        if (c == '(' && At(s, pos + 1) != '/') level++;
        if (c == ')' && !(pos > 0 && false)) {
            if (level == 1) return AllWs(s.substr(Z(pos + 1)));
            level--;
        }
    }
    return false;
}
bool MaskRe(SV s, const char *kw) {
    const L q = EolStart(s);
    const bool head = WithOptLabel(s, [&](L i) {
        if (!Kw(s, i, kw)) return false;
        const L k = SkipWs(s, i + KwLen(kw));
        return At(s, k) == '(' && ParenToEol(s, k, q);
    });
    return head && MaskBlock(s);
}
bool WhereRe(SV s) { return MaskRe(s, "where"); }
bool ForallRe(SV s) { return MaskRe(s, "forall"); }
bool EndWhereRe(SV s) { return EndKwName(s, "where", true); }
bool EndForallRe(SV s) { return EndKwName(s, "forall", true); }
bool ElseWhereRe(SV s) {
    const L q = EolStart(s);
    const L i = SkipWs(s, 0);
    if (!Kw(s, i, "else")) return false;
    const L j = SkipWs(s, i + 4);
    if (!Kw(s, j, "where")) return false;
    const L k = j + 5;
    auto tail = [&](L x) {
        if (IsEol(s, x, q)) return true;
        const L m = SkipWs(s, x);
        if (m >= q) return false;
        for (L t = m; t < q; t++)
            if (!IsWord(s[Z(t)])) return false;
        return true;
    };
    if (tail(k)) return true;
    if (At(s, k) != '(') return false;
    if (ParenToEol(s, k, q)) return true;
    L w0 = q;
    while (w0 > 0 && IsWord(s[Z(w0 - 1)])) w0--;
    if (w0 == q) return false;
    const L p = BackWs(s, w0) - 1;
    return p > k && At(s, p) == ')';
}

/** @brief fypp directive `#:kw` at the line start; `blank` requires whitespace after it. */
bool FyppDirective(SV s, const char *kw, bool blank) {
    const L i = SkipWs(s, 0);
    if (At(s, i) != '#' || At(s, i + 1) != ':' || !Kw(s, i + 2, kw)) return false;
    return !blank || IsSpace(At(s, i + 2 + KwLen(kw)));
}
bool FyppDefRe(SV s) { return FyppDirective(s, "def", true); }
bool FyppEndDefRe(SV s) { return FyppDirective(s, "enddef", false); }
bool FyppIfRe(SV s) { return FyppDirective(s, "if", true); }
bool FyppElifElseRe(SV s) { return FyppDirective(s, "elif", true) || FyppDirective(s, "else", false); }
bool FyppEndIfRe(SV s) { return FyppDirective(s, "endif", false); }
bool FyppForRe(SV s) { return FyppDirective(s, "for", true); }
bool FyppEndForRe(SV s) { return FyppDirective(s, "endfor", false); }
bool FyppBlockRe(SV s) { return FyppDirective(s, "block", true); }
bool FyppEndBlockRe(SV s) { return FyppDirective(s, "endblock", false); }
bool FyppCallRe(SV s) { return FyppDirective(s, "call", true); }
bool FyppEndCallRe(SV s) { return FyppDirective(s, "endcall", false); }
bool FyppMuteRe(SV s) { return FyppDirective(s, "mute", false); }
bool FyppEndMuteRe(SV s) { return FyppDirective(s, "endmute", false); }

bool AccessRe(SV s, const char *kw) {  // ^ PUBLIC :: / ^ PRIVATE ::
    const L i = SkipWs(s, 0);
    if (!Kw(s, i, kw)) return false;
    const L j = SkipWs(s, i + KwLen(kw));
    return At(s, j) == ':' && At(s, j + 1) == ':';
}

/** @brief A type declaration, by fprettify's (loose) test: a type keyword starts the statement. */
bool VarDeclRe(SV s) {
    L i = 0;
    while (i < Len(s) && s[Z(i)] == ' ') i++;
    for (const char *kw : {"integer", "logical", "character", "real", "complex", "type"}) {
        if (!Kw(s, i, kw)) continue;
        const L j = i + KwLen(kw);
        for (L t = j; t < Len(s); t++)
            if (s[Z(t)] != '\n') return true;
    }
    return false;
}

/** @brief A USE statement: `use m`, `use :: m`, `use, intrinsic :: m`, with a rename or ONLY list. */
bool UseRe(SV s) {
    const L i = SkipWs(s, 0);
    if (!Kw(s, i, "use")) return false;
    const L n = Len(s);
    const L lend = (n > 0 && s[Z(n - 1)] == '\n') ? n - 1 : n;  // where `$` may match
    auto after_name = [&](L m) {
        if (!IsWord(At(s, m))) return false;
        const L e = SkipWord(s, m);
        if (e == lend || e == n) return true;
        if (At(s, e) != ',') return false;
        for (L t = e; t < lend; t++)
            if (s[Z(t)] == '\n') return false;
        // , ... => ...
        for (L t = e + 2; t + 2 < lend; t++)
            if (s[Z(t)] == '=' && s[Z(t + 1)] == '>') return true;
        // , ONLY : ...
        const L o = SkipWs(s, e + 1);
        if (!Kw(s, o, "only")) return false;
        const L c = SkipWs(s, o + 4);
        return At(s, c) == ':' && c + 1 < lend;
    };
    const L j = i + 3;
    if (IsSpace(At(s, j)) && after_name(SkipWs(s, j))) return true;
    if (At(s, j) == ',' || (At(s, j) == ':' && At(s, j + 1) == ':')) {
        const L start = At(s, j) == ',' ? j + 2 : j;
        for (L t = start; t + 1 < n; t++) {
            if (s[Z(t)] == '\n' && At(s, j) == ',') break;
            if (s[Z(t)] == ':' && s[Z(t + 1)] == ':' && after_name(SkipWs(s, t + 2))) return true;
            if (At(s, j) == ':') break;
        }
    }
    return false;
}

bool EmptyRe(SV s) { return AllWs(s); }

/** @brief ^ NAMELIST ... / ... / (on its first line). */
bool NamelistStmt(SV s) {
    const L i = SkipWs(s, 0);
    if (!Kw(s, i, "namelist")) return false;
    int slashes = 0;
    for (L t = i + 8; t < Len(s) && s[Z(t)] != '\n'; t++)
        if (s[Z(t)] == '/') slashes++;
    return slashes >= 2;
}
bool DataStmt(SV s) {
    const L i = SkipWs(s, 0);
    if (!Kw(s, i, "data") || !IsSpace(At(s, i + 4))) return false;
    return IsWord(At(s, SkipWs(s, i + 4)));
}

/** @brief The keyword after END that END_RE recognises; returns its length, 0 when none. */
L EndKeyword(SV s, L j) {
    static const char *const kws[] = {"if",        "do",       "select",  "associate", "block",
                                      "subroutine", "function", "module",  "submodule", "type",
                                      "program",   "interface", "enum",    "where",     "forall"};
    for (const char *kw : kws)
        if (Kw(s, j, kw)) return KwLen(kw);
    return 0;
}

// ---------------------------------------------------------------------------
// Scope table: what opens, continues and closes an indented block. The
// indices pair an opener with its closer, as fprettify's tables do.

using Test = bool (*)(SV);
struct ScopeKind {
    Test open, cont, close;
    bool strict;  // false: a bare END closes whatever is open
};

const ScopeKind kScopes[] = {
        {IfThenRe, ElseRe, [](SV s) { return EndKwName(s, "if", true); }, true},
        {DoRe, nullptr, [](SV s) { return EndKwName(s, "do", true); }, true},
        {SelectRe, CaseRe, EndSelectRe, true},
        {SubroutineRe, ContainsRe, EndSubroutineRe, true},
        {FunctionRe, ContainsRe, EndFunctionRe, true},
        {InterfaceRe, nullptr, EndInterfaceRe, true},
        {TypeRe, ContainsRe, EndTypeRe, true},
        {EnumRe, nullptr, EndEnumRe, true},
        {AssociateRe, nullptr, EndAssociateRe, true},
        {nullptr, nullptr, EndAnyRe, false},
        {BlockRe, nullptr, EndBlockRe, true},
        {WhereRe, ElseWhereRe, EndWhereRe, true},
        {ForallRe, nullptr, EndForallRe, true},
        {ModuleRe, ContainsRe, EndModuleRe, true},
        {SubmoduleRe, ContainsRe, EndSubmoduleRe, true},
        {ProgramRe, ContainsRe, EndProgramRe, true},
        // fypp preprocessor blocks (second pass only)
        {FyppDefRe, nullptr, FyppEndDefRe, true},
        {FyppIfRe, FyppElifElseRe, FyppEndIfRe, true},
        {FyppForRe, nullptr, FyppEndForRe, true},
        {FyppBlockRe, nullptr, FyppEndBlockRe, true},
        {FyppCallRe, nullptr, FyppEndCallRe, true},
        {FyppMuteRe, nullptr, FyppEndMuteRe, true},
};
constexpr int kFortranScopes = 16;
constexpr int kAllScopes = 22;

/** @brief END + keyword, the statement the first pass writes as `end <keyword>`. */
bool IsEndStatement(SV s, int nscopes) {
    for (int k = 0; k < nscopes; k++)
        if (kScopes[k].close(s)) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Operator spacing inside a statement (first pass).

/** @brief Length of the relational operator at j (.EQ. == /= < <= > >= ...), 0 when none. */
L RelOpAt(SV s, L j) {
    const char32_t c = At(s, j), d = At(s, j + 1);
    if (j >= Len(s)) return 0;
    if (c == '.') {
        for (const char *op : {".eq.", ".ne.", ".lt.", ".le.", ".gt.", ".ge."})
            if (Kw(s, j, op)) return 4;
        return 0;
    }
    const bool has_d = j + 1 < Len(s);
    if (c == '=' && has_d && d == '=') return 2;
    if (c == '/' && has_d && d == '=') return 2;
    if (c == '<') return (has_d && d == '=') ? 2 : 1;
    if (c == '>') {
        if (has_d && d == '=') return 2;
        return (j > 0 && s[Z(j - 1)] == '=') ? 0 : 1;
    }
    return 0;
}

struct OpMatch {
    L start, op, op_end, end;
};

/** @brief Next relational operator match at or after i, with the blanks around it. */
bool NextRel(SV s, L i, OpMatch &m) {
    const L n = Len(s);
    while (i <= n) {
        const L j = SkipWs(s, i);
        const L len = RelOpAt(s, j);
        L end = -1;
        if (len) {
            const L k = j + len;
            const L t = SkipWs(s, k);
            if (At(s, t) == ')' && t < n) end = t > k ? t - 1 : -1;
            else end = t;
        }
        if (end < 0) {
            i = j + 1;
            continue;
        }
        L start = i;
        if (i > 0 && s[Z(i - 1)] == '(') {
            if (i < j) start = i + 1;
            else {
                i = j + 1;
                continue;
            }
        }
        m = {start, j, j + len, end};
        return true;
    }
    return false;
}
bool RelSearch(SV s) {
    OpMatch m{};
    return NextRel(s, 0, m);
}

std::vector<UStr> SplitRel(SV s) {
    std::vector<UStr> parts;
    L last = 0, i = 0;
    OpMatch m{};
    while (i <= Len(s) && NextRel(s, i, m)) {
        parts.push_back(Sub(s, last, m.start));
        parts.push_back(Sub(s, m.op, m.op_end));
        last = i = m.end;
    }
    parts.push_back(Sub(s, last));
    return parts;
}

std::vector<UStr> SplitLog(SV s) {
    std::vector<UStr> parts;
    L last = 0, i = 0;
    const L n = Len(s);
    while (i < n) {
        const L j = SkipWs(s, i);
        L len = 0;
        if (At(s, j) == '.')
            for (const char *op : {".and.", ".or.", ".eqv.", ".neqv."})
                if (Kw(s, j, op)) {
                    len = KwLen(op);
                    break;
                }
        if (!len) {
            i = j + 1;
            continue;
        }
        parts.push_back(Sub(s, last, i));
        parts.push_back(Sub(s, j, j + len));
        last = i = SkipWs(s, j + len);
    }
    parts.push_back(Sub(s, last));
    return parts;
}

bool AfterOperand(char32_t c) { return IsWord(c) || c == ')' || c == ']'; }

/** @brief Does `part` end in the mantissa of a real literal (`1.5e`, `2d`), so a following +/- is its exponent sign? */
bool EndsInMantissa(SV s) {
    const L q = EolStart(s);
    if (q == 0) return false;
    const char32_t e = Low(s[Z(q - 1)]);
    if (e != 'd' && e != 'e') return false;
    int dots = 0, digits = 0;
    for (L p = q - 2; p >= 0; p--) {
        const char32_t c = s[Z(p)];
        if (c == '.') dots++;
        else if (IsDigit(c)) digits++;
        else break;
        if (dots > 1) break;
        if (digits == 0) continue;
        // \b before the number
        const bool before = p > 0 && IsWord(s[Z(p - 1)]);
        if (before != IsWord(c)) return true;
    }
    return false;
}

std::vector<UStr> SplitPlusMinus(SV s) {
    std::vector<UStr> raw;
    L last = 0;
    const L n = Len(s);
    for (L i = 1; i <= n; i++) {
        if (!AfterOperand(s[Z(i - 1)])) continue;
        const L j = SkipWs(s, i);
        const char32_t c = At(s, j);
        if (j >= n || (c != '+' && c != '-')) continue;
        raw.push_back(Sub(s, last, i));
        raw.push_back(Sub(s, j, j + 1));
        last = SkipWs(s, j + 1);
        i = last - 1;
        if (last <= j) i = j;
    }
    raw.push_back(Sub(s, last));
    if (raw.size() == 1) return raw;
    std::vector<UStr> out;
    for (size_t k = 1; k < raw.size(); k += 2) {
        if (k == 1) out.push_back(raw[0]);
        if (k + 1 >= raw.size()) Fail("non-standard expression involving + or -");
        if (EndsInMantissa(raw[k - 1])) {
            out.back() += raw[k] + raw[k + 1];
        } else {
            out.push_back(raw[k]);
            out.push_back(raw[k + 1]);
        }
    }
    return out;
}

std::vector<UStr> SplitMultDiv(SV s) {
    std::vector<UStr> parts;
    const L n = Len(s);
    // no_dcolon[k]: no "::" in s[k:] before a newline
    std::vector<char> no_dcolon(Z(n + 2), 1);
    for (L k = n - 1; k >= 0; k--) {
        if (s[Z(k)] == '\n') no_dcolon[Z(k)] = 1;
        else if (s[Z(k)] == ':' && At(s, k + 1) == ':') no_dcolon[Z(k)] = 0;
        else no_dcolon[Z(k)] = no_dcolon[Z(k + 1)];
    }
    L last = 0;
    for (L i = 1; i <= n; i++) {
        if (!AfterOperand(s[Z(i - 1)])) continue;
        const L j = SkipWs(s, i);
        if (j >= n) continue;
        const char32_t c = s[Z(j)];
        if (c != '*' && c != '/') continue;
        if (At(s, j - 1) == c || At(s, j + 1) == c) continue;
        const char32_t a = At(s, j + 1);
        if (j + 1 >= n || !(IsSpace(a) || IsWord(a) || a == '(')) continue;
        if (!no_dcolon[Z(j + 1)]) continue;
        parts.push_back(Sub(s, last, i));
        parts.push_back(Sub(s, j, j + 1));
        last = j + 1;
        i = j;
    }
    parts.push_back(Sub(s, last));
    return parts;
}

std::vector<UStr> SplitPrint(SV s) {
    std::vector<UStr> parts;
    L last = 0;
    const L n = Len(s);
    for (L i = 4; i <= n; i++) {
        const bool pr = i >= 5 && Kw(s, i - 5, "print") && !IsWord(At(s, i - 6));
        const bool rd = Kw(s, i - 4, "read") && !IsWord(At(s, i - 5));
        if (!pr && !rd) continue;
        const L j = SkipWs(s, i);
        if (At(s, j) != '*' || j >= n) continue;
        L k = j + 1;
        if (At(s, k) == ',' && k < n) k++;
        parts.push_back(Sub(s, last, i));
        parts.push_back(Sub(s, j, k));
        last = SkipWs(s, k);
        i = last - 1;
        if (i < j) i = j;
    }
    parts.push_back(Sub(s, last));
    return parts;
}

std::vector<UStr> SplitNamelist(SV s) {
    std::vector<UStr> parts;
    L last = 0;
    const L n = Len(s);
    for (L i = 0; i < n; i++) {
        if (s[Z(i)] != '/' || !IsWord(At(s, i + 1))) continue;
        const L k = SkipWord(s, i + 1);
        if (At(s, k) != '/') continue;
        parts.push_back(Sub(s, last, i));
        parts.push_back(Sub(s, i, k + 1));
        last = k + 1;
        i = k;
    }
    parts.push_back(Sub(s, last));
    return parts;
}

UStr Join(const std::vector<UStr> &parts, const UStr &sep) {
    UStr out;
    for (size_t k = 0; k < parts.size(); k++) {
        if (k) out += sep;
        out += parts[k];
    }
    return out;
}

// ---------------------------------------------------------------------------
// First pass, step 1: drop blanks except between two words (and around `::`).

UStr RemoveExtraBlanks(SV line) {
    const L n = Len(line);
    std::vector<L> next_nw(Z(n + 1)), prev_nw(Z(n + 1));
    next_nw[Z(n)] = n;
    for (L k = n - 1; k >= 0; k--) next_nw[Z(k)] = IsSpace(line[Z(k)]) ? next_nw[Z(k + 1)] : k;
    prev_nw[0] = -1;
    for (L k = 1; k <= n; k++) prev_nw[Z(k)] = IsSpace(line[Z(k - 1)]) ? prev_nw[Z(k - 1)] : k - 1;
    UStr out;
    L pos_prev = -1, pos = -1;
    CodeChars cc(line);
    L p = 0;
    char32_t c = 0;
    while (cc.Next(p, c)) {
        pos = p;
        const L nn = next_nw[Z(p)], pp = prev_nw[Z(p)];
        const bool is_decl = (At(line, nn) == ':' && At(line, nn + 1) == ':' && nn + 1 < n) ||
                             (pp >= 1 && line[Z(pp)] == ':' && line[Z(pp - 1)] == ':');
        if (p > pos_prev + 1) out += line.substr(Z(pos_prev + 1), Z(p - pos_prev - 1));
        if (c == ' ') {
            if (!out.empty() && (IsWord(out.back()) || is_decl)) out += c;
        } else {
            if (!out.empty() && out.back() == ' ' && !IsWord(c) && !is_decl) out.pop_back();
            out += c;
        }
        pos_prev = p;
    }
    out += Sub(line, pos + 1);
    return out;
}

// ---------------------------------------------------------------------------
// First pass, step 2: blanks at brackets, commas, `%`, `.not.` and `=`.

const char *DelimOpen(SV s, L i) {
    if (At(s, i) == '(' && i < Len(s)) return At(s, i + 1) == '/' && i + 1 < Len(s) ? "(/" : "(";
    if (At(s, i) == '[' && i < Len(s)) return "[";
    return nullptr;
}
const char *DelimClose(SV s, L i) {
    if (At(s, i) == '/' && At(s, i + 1) == ')' && i + 1 < Len(s)) return "/)";
    if (At(s, i) == ')' && i < Len(s)) return ")";
    if (At(s, i) == ']' && i < Len(s)) return "]";
    return nullptr;
}

/** @brief Does the statement before an opening bracket at `pos` want a blank before it? */
bool BlankBeforeParen(SV line, L pos, L first_word_start, L first_word_end) {
    const SV pre = line.substr(0, Z(pos));
    const L b = BackWs(pre, pos);
    bool tight = false;
    if (b > 0) {
        const char32_t c = pre[Z(b - 1)];
        tight = IsWord(c) || c == '(' || c == '[' || c == '*' || c == '/' || c == '=' || c == '+' || c == '-' ||
                c == ':';
    }
    if (!tight && b > 0) return true;  // after a `)`, `,`, an operator such as `.and.`, ...
    // Keyword statements written `if (`, `select case (`, `write (` ...
    const L i0 = SkipWs(pre, 0);
    auto ends_at = [&](L k) { return SkipWs(pre, k) == pos; };
    auto after_label = [&](auto &&rest) {
        if (rest(i0)) return true;
        if (first_word_end > first_word_start) {
            const L k = SkipWs(pre, first_word_end);
            if (At(pre, k) == ':' && k < pos) return rest(k + 1);
        }
        return false;
    };
    // (label:)? (ELSE)? \s* IF
    if (after_label([&](L i) {
            L j = i;
            if (Kw(pre, j, "else")) {
                const L t = SkipWs(pre, j + 4);
                if (Kw(pre, t, "if") && ends_at(t + 2)) return true;
            }
            j = SkipWs(pre, i);
            return Kw(pre, j, "if") && ends_at(j + 2);
        }))
        return true;
    // (label:)? \s* DO \s+ WHILE
    if (after_label([&](L i) {
            const L j = SkipWs(pre, i);
            if (!Kw(pre, j, "do") || !IsSpace(At(pre, j + 2))) return false;
            const L t = SkipWs(pre, j + 2);
            return Kw(pre, t, "while") && ends_at(t + 5);
        }))
        return true;
    for (const char *kw : {"case", "rank"}) {
        L j = i0;
        if (Kw(pre, j, kw) && ends_at(j + 4)) return true;
        if (Kw(pre, j, "select")) {
            j = SkipWs(pre, j + 6);
            if (Kw(pre, j, kw) && ends_at(j + 4)) return true;
        }
    }
    if (Kw(pre, i0, "select")) {
        const L j = SkipWs(pre, i0 + 6);
        if (Kw(pre, j, "type") && ends_at(j + 4)) return true;
    }
    if (Kw(pre, i0, "class")) {
        const L j = SkipWs(pre, i0 + 5);
        if (Kw(pre, j, "default") && ends_at(j + 7)) return true;
    }
    for (const char *kw : {"type", "class"}) {
        if (!Kw(pre, i0, kw)) continue;
        const L j = i0 + KwLen(kw);
        if (!IsSpace(At(pre, j))) continue;
        const L t = SkipWs(pre, j);
        if (Kw(pre, t, "is") && ends_at(t + 2)) return true;
    }
    // I/O and allocation statements: `write (`, `allocate (` (not `x%write(`).
    if (b > 0 && IsWord(pre[Z(b - 1)])) {
        L a = b;
        while (a > 0 && IsWord(pre[Z(a - 1)])) a--;
        if (a == 0 || pre[Z(a - 1)] != '%') {
            static const char *const kws[] = {"allocate", "deallocate", "open",      "close",   "read",
                                              "write",    "flush",      "endfile",   "rewind",  "backspace",
                                              "inquire",  "forall",     "where",     "associate", "nullify"};
            for (const char *kw : kws)
                if (b - a == KwLen(kw) && Kw(pre, a, kw)) return true;
        }
    }
    return false;
}

/** @brief The first pass's character-wise spacing; see the header comment of this section. */
UStr SpaceCharwise(SV line, int nscopes) {
    const L n = Len(line);
    UStr out;
    L cur = 0, end = n;
    std::vector<L> pos_eq;
    L end_of_delim = -1;
    int level = 0;
    const L fw0 = SkipWs(line, 0), fw1 = SkipWord(line, fw0);

    auto flush_to = [&](L pos) {
        if (pos > cur) out.append(line.substr(Z(cur), Z(pos - cur)));
        if (pos > cur) cur = pos;
    };
    auto rstrip_out = [&] {
        while (!out.empty() && out.back() == ' ') out.pop_back();
    };
    auto skip_rhs_blanks = [&] {
        while (cur < end && line[Z(cur)] == ' ') cur++;
    };
    auto rstrip_all = [&] {
        while (end > cur && line[Z(end - 1)] == ' ') end--;
        if (cur >= end) rstrip_out();
    };

    CodeChars cc(line);
    L pos = 0;
    char32_t c = 0;
    while (cc.Next(pos, c)) {
        if (pos >= end) continue;
        const char *open = nullptr, *close = nullptr;
        if (pos > end_of_delim) {
            open = DelimOpen(line, pos);
            close = DelimClose(line, pos);
        }
        if (open || close) {
            const char *delim = open ? open : close;
            const L dl = KwLen(delim);
            L sep1 = 0, sep2 = 0;
            if (open) {
                level++;
                if (BlankBeforeParen(line, pos, fw0, fw1)) sep1 = 1;
            } else {
                if (level > 0) level--;
                const L j = SkipWs(line, pos + 1);
                const char32_t d = At(line, j);
                const bool tight = j < n && (d == ')' || d == ']' || d == ',' || d == '%' || d == ':' || d == '/' ||
                                             d == '*');
                if (!tight) sep2 = 1;
                else if (d == ':' && At(line, j + 1) == ':' && j + 1 < n)
                    sep2 = LeadingSpaces(line.substr(Z(std::min(pos + dl, end)), Z(end - std::min(pos + dl, end))));
            }
            end_of_delim = pos + dl - 1;
            flush_to(pos);
            rstrip_out();
            out += Spaces(sep1);
            for (L k = 0; k < dl; k++) out += static_cast<char32_t>(delim[k]);
            out += Spaces(sep2);
            cur = std::min(pos + dl, end);
            skip_rhs_blanks();
        }
        if (c == ',' || c == ';' || c == '%') {
            flush_to(pos);
            rstrip_out();
            out += c;
            if (c != '%') out += ' ';
            cur = pos + 1;
            skip_rhs_blanks();
            rstrip_all();
        }
        if (Kw(line, pos, ".not.")) {
            flush_to(pos);
            rstrip_out();
            out.append(line.substr(Z(pos), 5));
            out += ' ';
            cur = std::min(pos + 5, end);
            skip_rhs_blanks();
        }
        if (c == '=') {
            UStr window;
            if (pos == 0) {
                if (n - 1 < 2) window = Sub(line, n - 1, 2);
            } else {
                window = Sub(line, pos - 1, pos + 2);
            }
            if (!RelSearch(window)) {
                flush_to(pos);
                rstrip_out();
                const L eq = Len(out);
                out += '=';
                cur = pos + 1;
                skip_rhs_blanks();
                if (pos + 1 >= n) Fail("assignment at the end of a line");
                const bool is_pointer = line[Z(pos + 1)] == '>';
                if (level == 0 || is_pointer) pos_eq.push_back(eq);
            }
        }
    }
    if (cur < end) out.append(line.substr(Z(cur), Z(end - cur)));
    UStr ftd = std::move(out);

    if (!pos_eq.empty()) {
        UStr r;
        L prev = 0;
        for (const L p : pos_eq) {
            if (p < prev) continue;
            r.append(ftd, Z(prev), Z(p - prev));
            while (!r.empty() && r.back() == ' ') r.pop_back();
            const bool is_pointer = At(ftd, p + 1) == '>';
            r += is_pointer ? U" => " : U" = ";
            prev = p + 1 + (is_pointer ? 1 : 0);
            while (prev < Len(ftd) && ftd[Z(prev)] == ' ') prev++;
        }
        r.append(ftd, Z(std::min(prev, Len(ftd))));
        ftd = std::move(r);
    }

    // END<kw> becomes `END <kw>`.
    {
        const L i = SkipWs(ftd, 0);
        if (Kw(ftd, i, "end")) {
            const L j = SkipWs(ftd, i + 3);
            const L kl = EndKeyword(ftd, j);
            if (kl && IsEndStatement(ftd, nscopes))
                ftd = Sub(ftd, i, i + 3) + U" " + Sub(ftd, j, j + kl) + Sub(ftd, j + kl);
        }
    }
    return ftd;
}

// ---------------------------------------------------------------------------
// First pass, step 3: operators, split around strings.

bool LabelColon(SV s) {  // ^\s*\w+\s*:
    const L i = SkipWs(s, 0);
    if (!IsWord(At(s, i))) return false;
    return At(s, SkipWs(s, SkipWord(s, i))) == ':';
}

UStr SpaceOperators(SV line) {
    std::vector<UStr> parts{UStr()};
    {
        L pos_prev = -1, pos = -1;
        CodeChars cc(line);
        L p = 0;
        char32_t c = 0;
        while (cc.Next(p, c)) {
            pos = p;
            if (p > pos_prev + 1) {
                parts.push_back(Strip(line.substr(Z(pos_prev + 1), Z(p - pos_prev - 1))));
                parts.emplace_back();
            }
            parts.back() += c;
            pos_prev = p;
        }
        if (pos + 1 < Len(line)) parts.push_back(Sub(line, pos + 1));
    }
    const bool nml = NamelistStmt(line);
    const bool data = DataStmt(line);
    if (nml)
        for (UStr &part : parts)
            if (!StrOpen(part)) part = Join(SplitNamelist(part), U" ");
    if (!nml && !data) {
        for (int op = 0; op < 5; op++)
            for (UStr &part : parts) {
                if (StrOpen(part)) continue;
                switch (op) {
                case 0: part = Join(SplitRel(part), U" "); break;
                case 1: part = Join(SplitLog(part), U" "); break;
                case 2: part = Join(SplitPlusMinus(part), U" "); break;
                case 3: part = Join(SplitMultDiv(part), U""); break;
                default: part = Join(SplitPrint(part), U" "); break;
                }
            }
    }
    UStr out;
    for (const UStr &part : parts) out += part;

    for (Test t : {IfThenRe, DoRe, BlockRe}) {
        if (!t(out) || !LabelColon(out)) continue;
        const size_t k = out.find(U':');
        out = Strip(SV(out).substr(0, k)) + U": " + Strip(SV(out).substr(k + 1));
    }
    if (UseRe(out)) {
        UStr r;
        const L n = Len(out);
        L i = 0;
        while (i < n) {
            if (Kw(out, i, "only")) {
                const L j = SkipWs(out, i + 4);
                if (At(out, j) == ':' && j < n) {
                    r.append(out, Z(i), 4);
                    r += U": ";
                    i = SkipWs(out, j + 1);
                    continue;
                }
            }
            r += out[Z(i)];
            i++;
        }
        out = std::move(r);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Continuation helpers.

/** @brief Is s[a] an '&' that ends the line (only blanks or a comment after it)? */
bool LinebreakAmp(SV s, L a) {
    if (At(s, a) != '&' || a >= Len(s)) return false;
    const L n = Len(s);
    const L t = SkipWs(s, a + 1);
    if (t >= n) return true;
    if (s[Z(t)] != '!') return false;
    for (L k = t; k < n; k++)
        if (s[Z(k)] == '\n') return k == n - 1;
    return true;
}

/** @brief Is there a line-ending '&' whose preceding non-blank text satisfies `before`? */
template <class F>
bool BeforeLinebreak(SV s, F &&before) {
    for (L a = 0; a < Len(s); a++) {
        if (s[Z(a)] != '&' || !LinebreakAmp(s, a)) continue;
        if (before(BackWs(s, a))) return true;
    }
    return false;
}

std::vector<L> LinebreakPositions(const std::vector<UStr> &lines, bool fypp) {
    std::vector<L> raw;
    for (const UStr &line : lines) {
        L found = -1;
        CodeChars cc(line, true, false, true);
        L p = 0;
        char32_t c = 0;
        while (cc.Next(p, c))
            if (c == '&' && LinebreakAmp(line, p)) found = p;
        if (found > 0) raw.push_back(found);
        else if (NotFortranAt(LStripSp(line), 0, fypp)) raw.push_back(0);
    }
    std::vector<L> out;
    L sum = 0;
    for (const L v : raw) {
        sum += v;
        out.push_back(sum - 1);
    }
    return out;
}

/** @brief Re-split a reformatted statement where the original broke it. */
std::vector<UStr> SplitAtBreaks(SV orig, std::vector<L> breaks, const std::vector<L> &amp_sep, SV line) {
    std::sort(breaks.begin(), breaks.end(), std::greater<L>());
    std::vector<L> ftd{0};
    L pn = 0, po = 0;
    const L nn = Len(line), no = Len(orig);
    for (;;) {
        if (pn == nn || po == no) break;
        if (line[Z(pn)] != orig[Z(po)]) Fail("failed at finding line break position");
        if (!breaks.empty() && po > breaks.back()) {
            breaks.pop_back();
            ftd.push_back(pn);
            continue;
        }
        pn = SkipSpaces(line, pn + 1);
        po = SkipSpaces(orig, po + 1);
    }
    std::vector<UStr> out;
    for (size_t k = 0; k + 1 < ftd.size(); k++) {
        const L l = ftd[k], r = ftd[k + 1];
        if (k >= amp_sep.size()) Fail("unbalanced continuation lines");
        out.push_back(RStripSp(Sub(line, l, r)) + Spaces(amp_sep[k]) + (r > l ? U"&" : U""));
    }
    out.push_back(Sub(line, ftd.back()));
    return out;
}

// ---------------------------------------------------------------------------
// Reading logical lines: physical lines joined at '&', split at ';'.

struct LogicalLine {
    UStr joined;
    std::vector<UStr> comments;
    std::vector<UStr> lines;
};

class LineReader {
public:
    LineReader(const std::vector<UStr> &raw, bool fypp_comments) : raw_(raw), fypp_(fypp_comments) {}

    int line_nr = 0;

    LogicalLine Next() {
        LogicalLine out;
        bool continuation = false, fypp_cont = false;
        CodeChars strings(SV{});
        for (;;) {
            if (buf_.empty()) Fill(strings, fypp_cont);
            Piece pc = std::move(buf_.front());
            buf_.pop_front();
            if (pc.line.empty()) break;
            const UStr &line = pc.line;
            out.lines.push_back(pc.omp + line);
            UStr core = Sub(line, 0, pc.endpos + 1);
            UStr comment;
            if ((pc.endpos + 1 < Len(line) && NotFortranAt(line, pc.endpos + 1, fypp_)) || fypp_cont)
                comment = Sub(line, pc.endpos + 1);
            const bool newline = !core.empty() && core.back() == '\n';
            core = Strip(core);
            if (!core.empty() && !NotFortranAt(core, 0, true)) continuation = false;
            if (!core.empty() && core.back() == '&') continuation = true;
            if (!comment.empty()) {
                if (FyppLineAt(line, pc.endpos + 1) || fypp_cont) {
                    const UStr st = Strip(comment);
                    if (st.empty()) Fail("empty fypp continuation", line_nr);
                    fypp_cont = st.back() == '&';
                } else {
                    fypp_cont = false;
                }
            }
            core = StripAmp(core);
            out.comments.push_back(RStripNl(comment));
            if (!AllWs(out.joined)) {
                while (!out.joined.empty() && out.joined.back() == '\n') out.joined.pop_back();
                out.joined += core;
            } else {
                out.joined = pc.omp + core;
            }
            if (newline) out.joined += '\n';
            if (!(continuation || fypp_cont)) break;
        }
        return out;
    }

private:
    struct Piece {
        UStr line;
        L endpos;
        UStr omp;
    };

    void Push(UStr line, L endpos, UStr omp) { buf_.push_back(Piece{std::move(line), endpos, std::move(omp)}); }

    void Fill(CodeChars &strings, bool fypp_cont) {
        if (next_ >= raw_.size()) {  // end of file, even inside an unterminated string (fprettify loops here)
            line_nr++;
            Push(UStr(), 0, UStr());
            return;
        }
        UStr line = raw_[next_++];
        line_nr++;
        UStr omp;
        {
            const L k = SkipWs(line, 0);
            if (Kw(line, k, "!$ ")) {
                omp = U"!$ ";
                line.erase(Z(k), 3);
            }
        }
        if (strings.InString()) {
            const UStr ls = LStrip(line);
            if (ls.empty() || ls[0] != '&') line = U"&" + line;
        }
        cur_ = std::move(line);
        const UStr &ln = cur_;
        const L n = Len(ln);
        L line_start = 0, pos = -1;
        strings.Reset(ln);
        L p = 0;
        char32_t c = 0;
        while (strings.Next(p, c)) {
            pos = p;
            if (c == ';' || p + 1 == n) {
                Push(Sub(ln, line_start, p + 1), p - line_start, omp);
                omp.clear();
                line_start = p + 1;
            }
        }
        if (pos + 1 < n) {
            if (fypp_cont) {
                Push(ln, -1, omp);
            } else {
                const SV rest = SV(ln).substr(Z(pos + 1));
                CodeChars cc(rest, false, true, true);
                L pa = 0;
                char32_t ca = 0;
                while (cc.Next(pa, ca)) {
                    if (NotFortranAt(ln, pos + 1 + pa, fypp_)) {
                        Push(Sub(ln, line_start), pos + pa - line_start, omp);
                        break;
                    }
                }
            }
        }
        if (buf_.empty()) Push(ln, n, UStr());
    }

    const std::vector<UStr> &raw_;
    size_t next_ = 0;
    bool fypp_;
    std::deque<Piece> buf_;
    UStr cur_;
};

// ---------------------------------------------------------------------------
// Statement labels and OpenMP conditional lines.

/** @brief `^\s*!\$ ` replaced by three blanks. */
bool StripOmp(UStr &s) {
    const L k = SkipWs(s, 0);
    if (!Kw(s, k, "!$ ")) return false;
    s = U"   " + Sub(s, k + 3);
    return true;
}

/** @brief The leading digits of a statement label, or empty. */
UStr LabelOf(SV s) {
    const L i = SkipWs(s, 0);
    L j = i;
    while (j < Len(s) && IsDigit(s[Z(j)])) j++;
    return Sub(s, i, j);
}
/** @brief `^\s*\d+` replaced by `width` blanks. */
void BlankLabel(UStr &s, L width) {
    const L i = SkipWs(s, 0);
    L j = i;
    while (j < Len(s) && IsDigit(s[Z(j)])) j++;
    if (j == i) return;
    s = Spaces(width) + Sub(s, j);
}

// ---------------------------------------------------------------------------
// Second pass: continuation alignment.

class Aligner {
public:
    void Process(SV f_line, const std::vector<UStr> &lines, L rel_ind) {
        line_indents_.assign(1, 0);
        level_ = 0;
        br_.assign(1, 0);
        const bool is_decl = VarDeclRe(f_line) || AccessRe(f_line, "public") || AccessRe(f_line, "private");
        const bool is_use = UseRe(f_line);
        for (size_t k = 0; k < lines.size(); k++) {
            AlignLine(lines[k], is_decl, is_use, rel_ind);
            if (k + 1 < lines.size()) line_indents_.push_back(Top());
        }
    }
    const std::vector<L> &Indents() const { return line_indents_; }

private:
    L Top() const {
        if (br_.empty()) Fail("internal: empty continuation stack");
        return br_.back();
    }
    void Pop() {
        if (br_.empty()) Fail("internal: empty continuation stack");
        br_.pop_back();
    }

    void AlignLine(SV line, bool is_decl, bool is_use, L indent_size) {
        int level = level_;
        L pos_eq = 0;
        const L rel = Top();
        L end_of_delim = -1;
        const L n = Len(line);
        const bool eq_break = BeforeLinebreak(line, [&](L b) {
            return At(line, b - 1) == '=' || (At(line, b - 1) == '>' && At(line, b - 2) == '=');
        });
        const bool dcolon_break =
                BeforeLinebreak(line, [&](L b) { return At(line, b - 1) == ':' && At(line, b - 2) == ':'; });
        const bool colon_break = BeforeLinebreak(line, [&](L b) { return At(line, b - 1) == ':'; });
        CodeChars cc(line);
        L pos = 0;
        char32_t c = 0;
        while (cc.Next(pos, c)) {
            const char *open = nullptr, *close = nullptr;
            if (pos > end_of_delim) {
                open = DelimOpen(line, pos);
                close = DelimClose(line, pos);
            }
            if (open) {
                const L dl = KwLen(open);
                end_of_delim = pos + dl - 1;
                level++;
                br_.push_back(pos + dl + rel);
            }
            if (close) {
                end_of_delim = pos + KwLen(close) - 1;
                if (level > 0) {
                    level--;
                    Pop();
                }
            }
            if (c == ',' && level == 0 && pos_eq > 0) {
                pos_eq = 0;
                Pop();
            }
            bool assign = false;
            if (level == 0 && !is_decl && c == '=') {
                const UStr window = Sub(line, std::max<L>(0, pos - 1), std::min(pos + 2, n));
                assign = !RelSearch(window);
            }
            if (assign) {
                if (pos_eq > 0) Fail("found more than one assignment in the same Fortran line");
                if (pos + 1 >= n) Fail("assignment at the end of a line");
                const L is_pointer = line[Z(pos + 1)] == '>' ? 1 : 0;
                pos_eq = pos + 1;
                if (!eq_break) br_.push_back(pos_eq + 1 + is_pointer + Top());
            } else if (is_decl && c == ':' && At(line, pos + 1) == ':' && pos + 1 < n && !dcolon_break) {
                br_.push_back(pos + 3 + Top());
            } else if (is_use && c == ':' && !colon_break) {
                br_.push_back(pos + 2 + Top());
            }
        }
        if (level) {
            const bool open_break = BeforeLinebreak(line, [&](L b) {
                const char32_t d = At(line, b - 1);
                return (d == '(' || d == '[' || (d == '/' && At(line, b - 2) == '(')) && b > 0;
            });
            if (open_break) {
                if (br_.size() > 1) br_.back() = br_[br_.size() - 2];
                else if (!br_.empty()) br_.back() = 0;
            }
        }
        if (Top() == 0) br_.back() = indent_size;
        level_ = level;
    }

    std::vector<L> line_indents_;
    std::vector<L> br_;
    int level_ = 0;
};

// ---------------------------------------------------------------------------
// Second pass: scope indentation.

class Indenter {
public:
    Indenter(L first_indent, L rel_indent) {
        if (rel_indent > 0 && first_indent >= 0) {
            L start = first_indent % rel_indent;
            for (L k = start; k <= first_indent; k += rel_indent) indents_.push_back(k);
        } else if (rel_indent > 0) {
            // range(first % rel, first + 1, rel) with first = -1 is empty
        }
        if (indents_.empty()) indents_.push_back(0);
    }

    void Process(SV f_line, const std::vector<UStr> &lines, L rel_ind, L rel_ind_con,
                 const std::vector<L> *manual) {
        if (initial_ && (ProgramRe(f_line) || ModuleRe(f_line))) indents_.back() = 0;
        std::vector<L> line_indents(lines.size(), 0);
        const UStr filtered = CodeOnly(f_line, false);

        bool is_new = false, is_con = false, valid_con = false, is_end = false, valid_end = false;
        for (int k = 0; k < kAllScopes; k++) {
            const ScopeKind &sk = kScopes[k];
            if (sk.open && sk.open(filtered) && !sk.close(filtered)) {
                is_new = true;
                scopes_.push_back(k);
            }
        }
        for (int k = 0; k < kAllScopes; k++) {
            const ScopeKind &sk = kScopes[k];
            if (sk.cont && sk.cont(filtered)) {
                is_con = true;
                if (!scopes_.empty() && scopes_.back() == k) valid_con = true;
            }
        }
        for (int k = 0; k < kAllScopes; k++) {
            const ScopeKind &sk = kScopes[k];
            if (sk.close(filtered)) {
                is_end = true;
                if (!scopes_.empty()) {
                    const int what = scopes_.back();
                    scopes_.pop_back();
                    if (what == k || !sk.strict) valid_end = true;
                } else {
                    valid_end = true;
                }
            }
        }
        // fypp directives inside continuation lines
        if (lines.size() > 1 && !FyppLineAt(filtered, 0)) {
            for (int k = kFortranScopes; k < kAllScopes; k++)
                for (const UStr &l : lines)
                    if (kScopes[k].open(l)) {
                        is_new = true;
                        scopes_.push_back(k - kFortranScopes);
                    }
            for (int k = kFortranScopes; k < kAllScopes; k++)
                for (const UStr &l : lines)
                    if (kScopes[k].close(l)) {
                        is_end = true;
                        valid_end = true;
                        if (!scopes_.empty()) scopes_.pop_back();
                    }
        }

        std::vector<L> br;
        if (!manual) {
            aligner_.Process(f_line, lines, rel_ind_con);
            br = aligner_.Indents();
        } else {
            br = *manual;
        }
        for (size_t k = 0; k + 1 < lines.size(); k++) {
            if (k + 1 >= br.size()) Fail("internal: continuation indents");
            line_indents[k + 1] = br[k + 1];
        }

        if (is_new && !is_end) {
            for (L &v : line_indents) v += indents_.back();
            indents_.push_back(rel_ind + indents_.back());
        } else if (!is_new && (is_con || is_end)) {
            const bool valid = is_con ? valid_con : valid_end;
            if (!valid) {
                for (L &v : line_indents) v += indents_.back();
            } else if (indents_.size() > 1 || initial_) {
                const L base = indents_[indents_.size() - (initial_ ? 1 : 2)];
                for (L &v : line_indents) v += base;
            }
            if (is_end && valid) {
                if (indents_.size() > 1) indents_.pop_back();
                else indents_.back() = 0;
            }
        } else {
            for (L &v : line_indents) v += indents_.back();
        }
        initial_ = false;
        line_indents_ = std::move(line_indents);
    }

    L FlineIndent() const { return indents_.back(); }
    const std::vector<L> &LineIndents() const { return line_indents_; }

private:
    std::vector<int> scopes_;
    std::vector<L> indents_;
    std::vector<L> line_indents_;
    Aligner aligner_;
    bool initial_ = true;
};

// ---------------------------------------------------------------------------
// One pass over the file.

std::vector<UStr> SplitLines(SV text) {
    std::vector<UStr> lines;
    size_t a = 0;
    while (a < text.size()) {
        size_t b = text.find(U'\n', a);
        b = b == SV::npos ? text.size() : b + 1;
        UStr line;
        for (size_t k = a; k < b; k++) {
            if (text[k] == '\t') line.append(8, U' ');
            else line += text[k];
        }
        lines.push_back(std::move(line));
        a = b;
    }
    return lines;
}

struct PassConfig {
    bool indent = false;      // second pass: impose indentation
    bool whitespace = false;  // first pass: impose whitespace
    L indent_size = 3;
    L llength = 132;
};

/** @brief Each statement's indent relative to the one before, and the first statement's offset. */
void InspectIndents(const std::vector<UStr> &raw, bool fypp_comments, L indent_size, std::vector<L> &req,
                    L &first_indent) {
    LineReader rd(raw, fypp_comments);
    L prev_offset = 0;
    first_indent = -1;
    for (;;) {
        LogicalLine ll = rd.Next();
        if (ll.lines.empty()) break;
        const UStr label = LabelOf(ll.joined);
        if (!label.empty()) {
            BlankLabel(ll.joined, Len(label));
            BlankLabel(ll.lines[0], Len(label));
        }
        const L offset = LeadingSpaces(ll.lines[0]);
        if (!AllWs(ll.joined) && first_indent == -1) first_indent = offset;
        L ind = offset - prev_offset;
        if (IfThenRe(ll.joined) || DoRe(ll.joined)) {
            if (prev_offset != offset) ind = indent_size;
        } else {
            ind = indent_size;
        }
        req.push_back(ind);
        prev_offset = offset;
    }
}

UStr RunPass(SV text, const PassConfig &cfg) {
    const std::vector<UStr> raw = SplitLines(text);
    const bool indent_fypp = cfg.indent;
    const bool fypp_comments = !indent_fypp;
    const int nscopes = indent_fypp ? kAllScopes : kFortranScopes;
    std::vector<L> req;
    L first_indent = -1;
    InspectIndents(raw, fypp_comments, cfg.indent_size, req, first_indent);

    int indent_special = cfg.indent ? 0 : 3;
    Indenter indenter(first_indent, cfg.indent_size);
    size_t nfl = 0;
    bool use_same_line = false, skip_blank = false, in_off = false;
    LineReader rd(raw, fypp_comments);
    UStr out;

    for (;;) {
        LogicalLine ll = rd.Next();
        if (ll.lines.empty()) break;
        nfl++;
        std::vector<UStr> lines = std::move(ll.lines);
        std::vector<UStr> orig = lines;
        UStr f_line = std::move(ll.joined);
        const std::vector<UStr> &comments = ll.comments;
        const int lnr = rd.line_nr;

        std::vector<L> indent(lines.size(), 0);
        if (indent_special == 3)
            for (size_t k = 0; k < lines.size(); k++)
                indent[k] = Len(lines[k]) - Len(LStripAmp(LStripSp(lines[k])));

        // comments, with the blanks that separated them from the code
        std::vector<UStr> comment_lines;
        for (size_t k = 0; k < std::min(lines.size(), comments.size()); k++) {
            const UStr &line = lines[k];
            const UStr &comment = comments[k];
            L sep = 0;
            if (!AllWs(comment)) {
                UStr lmc;
                size_t a = 0;
                for (;;) {
                    const size_t b = line.find(comment, a);
                    if (b == UStr::npos) break;
                    lmc.append(line, a, b - a);
                    a = b + comment.size();
                }
                lmc.append(line, a);
                sep = Len(RStripNl(lmc)) - Len(RStrip(lmc));
            }
            if (!AllWs(line)) comment_lines.push_back(Spaces(sep) + Strip(comment));
        }

        // !& / !&< / !&> directives, and lines continued with a leading '&'
        bool auto_align = true;
        for (const UStr &l : lines) {
            const L i = SkipWs(l, 0);
            if (At(l, i) != '&' || i >= Len(l)) continue;
            const L j = SkipWs(l, i + 1);
            if (j < Len(l) && !IsSpace(l[Z(j)]) && l[Z(j)] != '*') auto_align = false;
        }
        bool auto_format = !in_off;
        for (const UStr &cl : comment_lines)
            if (StartsWith(LStrip(cl), "!&")) auto_format = false;
        if (!auto_format) auto_align = false;
        if (lines.size() == 1) {
            const UStr st = Strip(lines[0]);
            bool valid = true;
            if (StartsWith(st, "!&<")) {
                if (in_off) valid = false;
                else in_off = true;
            }
            if (StartsWith(st, "!&>")) {
                if (!in_off) valid = false;
                else in_off = false;
            }
            if (!valid) Fail("wrong usage of formatting-specific directives '&', '!&', '!&<' or '!&>'", lnr);
        }

        UStr omp_probe = f_line;
        const bool is_omp = StripOmp(omp_probe);
        if (is_omp) {
            f_line = omp_probe;
            for (UStr &l : lines) StripOmp(l);
        }
        UStr label = LabelOf(f_line);
        if (!label.empty()) {
            BlankLabel(f_line, Len(label));
            BlankLabel(lines[0], Len(label));
            if (!is_omp) orig[0] = lines[0];
        }

        // special lines: fypp directives and FORD `!!` doc comments keep their place
        std::vector<char> is_special(lines.size(), 0);
        for (size_t k = 0; k < lines.size(); k++) {
            const UStr ls = LStrip(lines[k]);
            if (indent_fypp) is_special[k] = StartsWith(ls, "!!") || (k > 0 && FyppLineAt(ls, 0));
            else is_special[k] = FyppLineAt(ls, 0) || StartsWith(ls, "!!");
        }
        if (!is_special.empty() && is_special[0]) std::fill(is_special.begin(), is_special.end(), 1);

        bool do_format = false, prev_indent = false, is_blank = false;
        if (EmptyRe(f_line)) {
            bool any = false;
            for (const UStr &cm : comments) any = any || !cm.empty();
            if (any) {
                if (!lines.empty() && !lines[0].empty() && lines[0][0] == ' ') prev_indent = true;
            } else {
                is_blank = true;
            }
            for (size_t k = 0; k < lines.size(); k++)
                if (!is_special[k]) lines[k] = StripSp(lines[k]);
        } else {
            do_format = true;
        }

        if (!is_special.empty() && is_special[0]) indent_special = 3;
        if (prev_indent && indent_special == 0) indent_special = 2;
        if (is_blank && skip_blank) continue;

        if (!do_format) {
            if (indent_special == 2) std::fill(indent.begin(), indent.end(), indenter.FlineIndent());
            else if (indent_special == 0) indent_special = 1;
        } else {
            std::vector<L> manual;
            if (!auto_align) {
                for (const UStr &l : lines) manual.push_back(Len(l) - Len(LStripAmp(LStripSp(l))));
                const L m0 = manual.empty() ? 0 : manual[0];
                for (L &v : manual) v -= m0;
            }
            // leading '&' of continuation lines, and the blanks before the previous line's '&'
            std::vector<UStr> pre_amp;
            std::vector<L> amp_sep;
            for (size_t k = 0; k < lines.size(); k++) {
                const UStr &l = lines[k];
                const L i = SkipWs(l, 0);
                if (At(l, i) == '&' && i < Len(l)) {
                    pre_amp.push_back(Sub(l, i, SkipWs(l, i + 1)));
                    const UStr &prev = k == 0 ? lines.back() : lines[k - 1];
                    L sep = -1;
                    for (L a = 0; a < Len(prev); a++)
                        if (prev[Z(a)] == '&' && LinebreakAmp(prev, a)) {
                            sep = a - BackWs(prev, a);
                            break;
                        }
                    if (sep < 0) Fail("bad continuation line format", lnr);
                    amp_sep.push_back(sep);
                } else {
                    pre_amp.emplace_back();
                    if (k > 0) amp_sep.push_back(1);
                }
            }
            for (size_t k = 0; k < lines.size(); k++)
                if (!is_special[k]) lines[k] = StripAmp(StripSp(lines[k]));
            const std::vector<L> breaks = LinebreakPositions(lines, fypp_comments);
            f_line = StripSp(f_line);

            if (cfg.whitespace) {
                UStr fmt = f_line;
                if (auto_format) {
                    fmt = RemoveExtraBlanks(fmt);
                    fmt = SpaceCharwise(fmt, nscopes);
                    fmt = SpaceOperators(fmt);
                }
                lines = SplitAtBreaks(f_line, breaks, amp_sep, fmt);
                // append the comments
                const size_t m = std::min(lines.size(), comment_lines.size());
                for (size_t k = 0; k < m; k++) {
                    UStr comment = comment_lines[k];
                    bool has_nl;
                    if (k + 1 < lines.size()) {
                        has_nl = true;
                        if (AllWs(lines[k]) && !is_special[k]) comment = LStrip(comment);
                    } else {
                        const L q = BackWs(lines[k], Len(lines[k]));
                        has_nl = !(q > 0 && lines[k][Z(q - 1)] == ';');
                    }
                    lines[k] = RStripSp(lines[k]) + comment + (has_nl ? U"\n" : U"");
                }
            }
            const L rel_indent = nfl < req.size() ? req[nfl] : 0;
            if (indent_special != 3) {
                indenter.Process(f_line, lines, rel_indent, cfg.indent_size, auto_align ? nullptr : &manual);
                indent = indenter.LineIndents();
            }
            for (size_t k = 0; k < lines.size(); k++) {
                if (k >= pre_amp.size()) Fail("internal: continuation lines", lnr);
                if (pre_amp[k].empty()) continue;
                if (k >= indent.size()) Fail("internal: continuation lines", lnr);
                indent[k] -= 1;
                lines[k] = pre_amp[k] + LStrip(lines[k]);
            }
        }

        bool any_special = false;
        for (char sp : is_special) any_special = any_special || sp;
        if (any_special)
            for (size_t k = 0; k < lines.size(); k++) {
                if (k >= is_special.size() || k >= indent.size()) Fail("internal: special lines", lnr);
                if (!is_special[k]) continue;
                indent[k] = LeadingSpaces(lines[k]);
                lines[k] = LStripSp(lines[k]);
            }

        for (UStr &l : lines) {
            if (!l.empty() && IsSpace(l.back())) {
                l.erase(Z(BackWs(l, Len(l))));
                l += '\n';
            }
        }

        // write
        {
            UStr lab = label;
            const size_t m = std::min({indent.size(), lines.size(), orig.size()});
            for (size_t k = 0; k < m; k++) {
                const UStr &line = lines[k];
                L length = 0;
                {
                    CodeChars cc(line);
                    L p = 0;
                    char32_t c = 0;
                    while (cc.Next(p, c)) length = p;
                    length += 1;
                }
                L ind_use = indent_special != 1 ? indent[k] : (use_same_line ? 1 : 0);
                if (CppLine(LStrip(line))) ind_use = 0;
                UStr label_use;
                if (!lab.empty()) {
                    label_use = lab + U" ";
                    lab.clear();
                }
                const UStr ls = LStripSp(line);
                const L lead = Len(line) - Len(ls);
                const L omp = is_omp ? 1 : 0;
                if (ind_use + length <= cfg.llength + 1) {
                    if (omp) out += U"!$ ";
                    out += label_use;
                    out += Spaces(ind_use - 3 * omp - Len(label_use) + lead);
                    out += ls;
                } else if (length <= cfg.llength + 1) {
                    if (omp) out += U"!$ ";
                    out += label_use;
                    out += Spaces((cfg.llength + 1) - 3 * omp - Len(label_use) - Len(ls));
                    out += ls;
                } else {
                    out += orig[k];
                }
            }
        }

        {
            const L q = BackWs(f_line, Len(f_line));
            const bool semi = q > 0 && f_line[Z(q - 1)] == ';';
            use_same_line = semi;
            if (cfg.indent) indent_special = semi ? 1 : 0;
        }
        bool any_comment = false;
        for (const UStr &cm : comments) any_comment = any_comment || !cm.empty();
        skip_blank = EmptyRe(f_line) && !any_comment && !is_omp && label.empty();
    }
    return out;
}

// ---------------------------------------------------------------------------
// The self-check: a free-form Fortran lexer, run over input and output.

struct Token {
    UStr text;
    int line;
};

bool IsOpWord(SV s, L i, L &len) {  // .name. operator or logical constant
    if (At(s, i) != '.') return false;
    L j = i + 1;
    while (j < Len(s) && ((s[Z(j)] >= 'a' && s[Z(j)] <= 'z') || (s[Z(j)] >= 'A' && s[Z(j)] <= 'Z'))) j++;
    if (j == i + 1 || At(s, j) != '.') return false;
    len = j + 1 - i;
    return true;
}

UStr CollapseWs(SV s) {
    UStr out;
    bool gap = false;
    for (char32_t c : s) {
        if (IsSpace(c)) {
            gap = true;
            continue;
        }
        if (gap && !out.empty()) out += ' ';
        gap = false;
        out += c;
    }
    return out;
}

std::vector<Token> Lex(SV text) {
    std::vector<Token> toks;
    const std::vector<UStr> lines = SplitLines(text);
    bool in_string = false;
    char32_t quote = 0;
    UStr str;
    int str_line = 0;
    bool stmt_open = false;
    bool prev_continued = false;  // a blank or comment line does not end a continued statement
    bool code_tokens = false;
    auto eos = [&](int ln) {
        if (stmt_open) toks.push_back({U";", ln});
        stmt_open = false;
    };
    for (size_t li = 0; li < lines.size(); li++) {
        const int ln = static_cast<int>(li) + 1;
        UStr line = lines[li];
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        L i = 0;
        const L n = Len(line);
        bool continued = false;
        code_tokens = false;
        if (in_string) {
            const L t = SkipWs(line, 0);
            if (At(line, t) == '&' && t < n) i = t + 1;
        } else {
            i = SkipWs(line, 0);
            if (At(line, i) == '#' && i < n) {  // preprocessor line
                if (!prev_continued) eos(ln);
                toks.push_back({U"#" + CollapseWs(SV(line).substr(Z(i + 1))), ln});
                continue;
            }
            if (Kw(line, i, "!$ ")) {
                toks.push_back({U"!$", ln});
                i += 3;
                i = SkipWs(line, i);
            }
            if (At(line, i) == '&' && i < n) i++;
        }
        while (i < n) {
            if (in_string) {
                const char32_t c = line[Z(i)];
                if (c == quote) {
                    if (At(line, i + 1) == quote && i + 1 < n) {
                        str += c;
                        str += c;
                        i += 2;
                        continue;
                    }
                    str += c;
                    toks.push_back({str, str_line});
                    in_string = false;
                    stmt_open = true;
                    code_tokens = true;
                    i++;
                    continue;
                }
                if (c == '&' && SkipWs(line, i + 1) >= n) {
                    continued = true;
                    break;
                }
                str += c;
                i++;
                continue;
            }
            const char32_t c = line[Z(i)];
            if (IsSpace(c)) {
                i++;
                continue;
            }
            if (c == '!') {
                toks.push_back({CollapseWs(SV(line).substr(Z(i))), ln});
                break;
            }
            if (c == '&') {
                const L t = SkipWs(line, i + 1);
                if (t >= n || line[Z(t)] == '!') {
                    continued = true;
                    if (t < n) toks.push_back({CollapseWs(SV(line).substr(Z(t))), ln});
                    break;
                }
                toks.push_back({U"&", ln});
                i++;
                continue;
            }
            stmt_open = true;
            code_tokens = true;
            if (c == '\'' || c == '"') {
                in_string = true;
                quote = c;
                str = UStr(1, c);
                str_line = ln;
                i++;
                continue;
            }
            if (c == ';') {
                eos(ln);
                i++;
                continue;
            }
            L ol = 0;
            if (IsOpWord(line, i, ol)) {
                toks.push_back({Sub(line, i, i + ol), ln});
                i += ol;
                continue;
            }
            if (IsDigit(c) || (c == '.' && IsDigit(At(line, i + 1)))) {
                L j = i;
                while (IsDigit(At(line, j))) j++;
                L tl = 0;
                if (At(line, j) == '.' && !IsOpWord(line, j, tl)) {
                    j++;
                    while (IsDigit(At(line, j))) j++;
                }
                const char32_t e = Low(At(line, j));
                if (e == 'e' || e == 'd' || e == 'q') {
                    L k = j + 1;
                    if (At(line, k) == '+' || At(line, k) == '-') k++;
                    if (IsDigit(At(line, k))) {
                        j = k;
                        while (IsDigit(At(line, j))) j++;
                    }
                }
                while (IsWord(At(line, j)) && j < n) j++;
                toks.push_back({Sub(line, i, j), ln});
                i = j;
                continue;
            }
            if (IsWord(c)) {
                const L j = SkipWord(line, i);
                toks.push_back({Sub(line, i, j), ln});
                i = j;
                continue;
            }
            static const char *const two[] = {"**", "//", "==", "/=", "<=", ">=", "=>", "::"};
            bool done = false;
            for (const char *op : two)
                if (Kw(line, i, op)) {
                    toks.push_back({Sub(line, i, i + 2), ln});
                    i += 2;
                    done = true;
                    break;
                }
            if (done) continue;
            toks.push_back({UStr(1, c), ln});
            i++;
        }
        if (in_string) {
            if (!continued) {  // unterminated: ends with the line
                toks.push_back({str, str_line});
                in_string = false;
            }
        }
        if (!continued && !in_string) {
            if (!code_tokens && prev_continued) continued = true;
            else eos(ln);
        }
        prev_continued = continued || in_string;
    }
    if (in_string) toks.push_back({str, str_line});
    eos(static_cast<int>(lines.size()));
    // fprettify writes ENDIF as END IF: compare them as one word.
    std::vector<Token> merged;
    for (size_t k = 0; k < toks.size(); k++) {
        if (k + 1 < toks.size() && toks[k].text.size() == 3 && Kw(toks[k].text, 0, "end") &&
            EndKeyword(toks[k + 1].text, 0) == Len(toks[k + 1].text)) {
            merged.push_back({toks[k].text + toks[k + 1].text, toks[k].line});
            k++;
            continue;
        }
        merged.push_back(toks[k]);
    }
    return merged;
}

// ---------------------------------------------------------------------------
// UTF-8 <-> UTF-32

bool Decode(std::string_view in, UStr &out) {
    out.clear();
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        char32_t cp = 0;
        size_t len = 0;
        if (c < 0x80) {
            cp = c;
            len = 1;
        } else if (c >= 0xC2 && c < 0xE0) {
            cp = c & 0x1Fu;
            len = 2;
        } else if (c >= 0xE0 && c < 0xF0) {
            cp = c & 0x0Fu;
            len = 3;
        } else if (c >= 0xF0 && c < 0xF5) {
            cp = c & 0x07u;
            len = 4;
        } else {
            return false;
        }
        if (i + len > in.size()) return false;
        for (size_t k = 1; k < len; k++) {
            const unsigned char d = static_cast<unsigned char>(in[i + k]);
            if ((d & 0xC0u) != 0x80u) return false;
            cp = (cp << 6) | (d & 0x3Fu);
        }
        if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
            (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        out += cp;
        i += len;
    }
    return true;
}

std::string Encode(SV in) {
    std::string out;
    out.reserve(in.size());
    for (char32_t cp : in) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

/** @brief Universal newlines, as Python reads a text file: CRLF and lone CR become LF. */
UStr NormaliseNewlines(SV s) {
    UStr out;
    out.reserve(s.size());
    for (size_t k = 0; k < s.size(); k++) {
        if (s[k] == '\r') {
            out += '\n';
            if (k + 1 < s.size() && s[k + 1] == '\n') k++;
        } else {
            out += s[k];
        }
    }
    return out;
}

constexpr size_t kMaxInput = 64u << 20;      // bytes
constexpr size_t kMaxStatement = 1u << 20;   // characters in one physical line
constexpr size_t kMaxContinuations = 20000;  // physical lines in one statement

}  // namespace

Result Format(std::string_view src, const Options &opts) {
    Result res;
    res.text = std::string(src);
    if (opts.fixed_form) {
        res.error = "fixed-form Fortran is not formatted (fprettify handles free form only)";
        return res;
    }
    if (src.size() > kMaxInput) {
        res.error = "file too large to format";
        return res;
    }
    try {
        UStr text;
        if (!Decode(src, text)) {
            res.error = "not valid UTF-8";
            return res;
        }
        text = NormaliseNewlines(text);
        {
            size_t line_start = 0, cont = 0;
            int ln = 1;
            for (size_t k = 0; k <= text.size(); k++) {
                if (k < text.size() && text[k] != '\n') continue;
                if (k - line_start > kMaxStatement) {
                    res.error = "line too long to format";
                    res.error_line = ln;
                    return res;
                }
                // a long run of continued lines makes one huge statement
                size_t e = k;
                while (e > line_start && IsSpace(text[e - 1])) e--;
                cont = (e > line_start && text[e - 1] == '&') ? cont + 1 : 0;
                if (cont > kMaxContinuations) {
                    res.error = "statement too long to format";
                    res.error_line = ln;
                    return res;
                }
                line_start = k + 1;
                ln++;
            }
        }
        PassConfig p1;
        p1.whitespace = true;
        p1.indent_size = opts.indent;
        p1.llength = opts.width == 0 ? 1024 : opts.width;
        PassConfig p2 = p1;
        p2.whitespace = false;
        p2.indent = true;
        const UStr spaced = RunPass(text, p1);
        const UStr out = RunPass(spaced, p2);

#ifdef FORTRANFMT_NO_SELFCHECK  // scratch builds measuring the layout alone
        const std::vector<Token> want, got;
        (void)Lex;
#else
        const std::vector<Token> want = Lex(text), got = Lex(out);
#endif
        const size_t m = std::min(want.size(), got.size());
        size_t k = 0;
        while (k < m && want[k].text == got[k].text) k++;
        if (k < want.size() || k < got.size()) {
            res.error = "formatting would change the code";
            if (k < want.size()) res.error += " near `" + Encode(want[k].text) + "`";
            res.error_line = k < want.size() ? want[k].line : (k < got.size() ? got[k].line : 0);
            return res;
        }
        res.text = Encode(out);
        res.ok = true;
    } catch (const Failure &f) {
        res.error = f.msg;
        res.error_line = f.line;
    } catch (const std::bad_alloc &) {
        res.error = "out of memory while formatting";
    } catch (const std::exception &e) {
        res.error = e.what();
    }
    if (!res.ok) res.text = std::string(src);
    return res;
}

}  // namespace fortranfmt
