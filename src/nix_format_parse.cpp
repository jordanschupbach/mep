// The reader half of mep's Nix formatter: a port of nixfmt's lexer and
// parser (Nixfmt/Lexer.hs, Nixfmt/Parser.hs, Nixfmt/Parser/Float.hs) plus
// the tree helpers of Nixfmt/Types.hs. See nix_format_internal.h.
//
// nixfmt's parser is megaparsec over a state monad holding "pending
// trivia": every token is read together with the comments and blank lines
// after it, the ones on its own line become its trailing comment and the
// rest wait in the state for the next token (or become a Comments item in
// a list or set). This reader keeps the same state and backtracks it with
// the input position, which is all megaparsec's `try` does here.

#include "nix_format_internal.h"

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <utility>

namespace nixfmt::detail {

namespace {

constexpr int kMaxDepth = 300;  // nesting cap: the reader, printer and layout all recurse

bool IsAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsAlnum(char c) { return IsAlpha(c) || IsDigit(c); }
bool IdentChar(char c) { return IsAlnum(c) || c == '_' || c == '\'' || c == '-'; }
bool PathChar(char c) { return IsAlnum(c) || c == '.' || c == '_' || c == '-' || c == '+' || c == '~'; }
bool SchemeChar(char c) { return IsAlnum(c) || c == '-' || c == '.' || c == '+'; }
bool UriChar(char c) {
    return IsAlnum(c) || std::string_view("~!@$%&*-=_+:',./?").find(c) != std::string_view::npos;
}
// Between tokens: any space but a line break.
size_t HSpaceAt(const std::string &s, size_t i) {
    if (i < s.size() && (s[i] == '\n' || s[i] == '\r')) return 0;
    return SpaceAt(s, i);
}

std::string StripEnd(std::string s) {
    while (const size_t n = SpaceBefore(s, s.size())) s.resize(s.size() - n);
    return s;
}
std::string StripStart(const std::string &s) {
    size_t i = 0;
    while (const size_t n = SpaceAt(s, i)) i += n;
    return s.substr(i);
}
std::string Strip(const std::string &s) { return StripEnd(StripStart(s)); }
bool AllSpaces(const std::string &s) {
    return std::all_of(s.begin(), s.end(), [](char c) { return c == ' '; });
}
size_t LeadingSpaces(const std::string &s) {
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') i++;
    return i;
}

const char *const kReserved[] = {"let", "in", "if", "then", "else", "assert", "with", "rec", "inherit"};

// A comment, blank-line run or language annotation as the lexer first sees
// it, before it is sorted into trailing comment vs leading trivia.
struct PT {
    enum Kind : unsigned char { Newlines, LineComment, BlockComment, LangAnnot };
    Kind kind = Newlines;
    int n = 0;
    int col = 0;  // LineComment: 1-based column of the `#`
    bool doc = false;
    std::string text;
    std::vector<std::string> lines;
};

bool IsTrailing(const PT &p) {
    return p.kind == PT::LineComment || (p.kind == PT::BlockComment && !p.doc && p.lines.size() <= 1);
}

Trivia ConvertLeading(const std::vector<PT> &pts, size_t from) {
    Trivia out;
    for (size_t i = from; i < pts.size(); i++) {
        const PT &p = pts[i];
        Trivium t;
        switch (p.kind) {
        case PT::Newlines:
            if (p.n == 1) continue;
            t.kind = Trivium::EmptyLine;
            break;
        case PT::LineComment:
            t.kind = Trivium::LineComment;
            t.text = p.text;
            break;
        case PT::BlockComment:
            if (p.lines.empty()) continue;
            if (!p.doc && p.lines.size() == 1) {
                t.kind = Trivium::LineComment;
                t.text = " " + Strip(p.lines[0]);
            } else {
                t.kind = Trivium::BlockComment;
                t.doc = p.doc;
                t.lines = p.lines;
            }
            break;
        case PT::LangAnnot:
            t.kind = Trivium::LangAnnotation;
            t.text = p.text;
            break;
        }
        out.push_back(std::move(t));
    }
    return out;
}

std::optional<std::string> ConvertTrailing(const std::vector<PT> &pts, size_t count) {
    std::string joined;
    for (size_t i = 0; i < count; i++) {
        std::string t;
        if (pts[i].kind == PT::LineComment) t = Strip(pts[i].text);
        else if (pts[i].kind == PT::BlockComment && !pts[i].doc && pts[i].lines.size() == 1) t = Strip(pts[i].lines[0]);
        if (t.empty()) continue;
        if (!joined.empty()) joined += ' ';
        joined += t;
    }
    if (joined.empty()) return std::nullopt;
    return joined;
}

// The block comment's lines, normalized the way nixfmt's lexer does:
// a column of `*` aligned under the opening `/*` dropped, trailing blanks
// stripped, the common indentation removed.
std::vector<std::string> BlockLines(const std::string &raw, int pos) {
    std::string chars;
    chars.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); i++) {
        if (raw[i] == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n') continue;
        chars += raw[i];
    }
    std::vector<std::string> lines;
    if (!chars.empty()) {
        size_t start = 0;
        for (;;) {
            const size_t nl = chars.find('\n', start);
            if (nl == std::string::npos) {
                if (start < chars.size()) lines.push_back(chars.substr(start));
                break;
            }
            lines.push_back(chars.substr(start, nl - start));
            start = nl + 1;
        }
    }
    for (auto &l : lines) l = StripEnd(l);
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    const auto upos = static_cast<size_t>(pos);
    // removeStars
    if (!lines.empty()) {
        const std::string star = std::string(upos, ' ') + " *";
        bool all = true;
        for (size_t i = 1; i < lines.size(); i++) {
            if (lines[i].compare(0, star.size(), star) != 0) {
                all = false;
                break;
            }
        }
        if (all) {
            for (size_t i = 1; i < lines.size(); i++) lines[i] = std::string(upos, ' ') + lines[i].substr(star.size());
        }
    }
    for (auto &l : lines) l = StripEnd(l);
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    // fixIndent
    if (!lines.empty()) {
        const bool lead_space = !lines[0].empty() && lines[0][0] == ' ';
        size_t n = upos + (lead_space ? 3 : 2);
        for (size_t i = 1; i < lines.size(); i++) {
            if (AllSpaces(lines[i])) continue;
            n = std::min(n, LeadingSpaces(lines[i]));
        }
        lines[0] = Strip(lines[0]);
        for (size_t i = 1; i < lines.size(); i++) {
            if (lines[i].size() >= n && LeadingSpaces(lines[i]) >= n) lines[i] = lines[i].substr(n);
            else lines[i] = StripStart(lines[i]);
        }
    }
    size_t drop = 0;
    while (drop < lines.size() && lines[drop].empty()) drop++;
    lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(drop));
    return lines;
}

bool IsEmptyStrLine(const StrLine &l) {
    if (l.empty()) return true;
    return l.size() == 1 && !l[0].interp && AllSpaces(l[0].text);
}

StrLine NormalizeLine(const StrLine &l) {
    StrLine out;
    for (const auto &p : l) {
        if (!p.interp && p.text.empty()) continue;
        if (!p.interp && !out.empty() && !out.back().interp) {
            out.back().text += p.text;
            continue;
        }
        out.push_back(p);
    }
    return out;
}

// splitLines: a line's text parts cut at each '\n'.
std::vector<StrLine> SplitLines(const StrLine &l) {
    std::vector<StrLine> out(1);
    for (const auto &p : l) {
        if (p.interp) {
            out.back().push_back(p);
            continue;
        }
        size_t start = 0;
        for (;;) {
            const size_t nl = p.text.find('\n', start);
            StringPart tp;
            tp.text = p.text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            out.back().push_back(std::move(tp));
            if (nl == std::string::npos) break;
            out.emplace_back();
            start = nl + 1;
        }
    }
    return out;
}

std::vector<StrLine> FixSimpleString(const StrLine &parts) {
    std::vector<StrLine> out;
    for (auto &l : SplitLines(parts)) out.push_back(NormalizeLine(l));
    return out;
}

std::vector<StrLine> FixIndentedString(std::vector<StrLine> lines) {
    // fixFirstLine
    if (!lines.empty()) {
        StrLine first = NormalizeLine(lines[0]);
        if (IsEmptyStrLine(first) && lines.size() > 1) lines.erase(lines.begin());
        else lines[0] = std::move(first);
    }
    // fixLastLine
    if (!lines.empty()) {
        StrLine last = NormalizeLine(lines.back());
        if (IsEmptyStrLine(last)) last.clear();
        lines.back() = std::move(last);
    }
    // stripIndentation
    std::optional<size_t> ind;
    for (const auto &l : lines) {
        if (l.empty() || IsEmptyStrLine(l)) continue;
        const size_t n = l[0].interp ? 0 : LeadingSpaces(l[0].text);
        ind = ind ? std::min(*ind, n) : n;
    }
    if (!ind) {
        for (auto &l : lines) l.clear();
    } else {
        for (auto &l : lines) {
            if (l.empty() || l[0].interp) continue;
            std::string &t = l[0].text;
            if (LeadingSpaces(t) >= *ind) t = t.substr(*ind);
            else t.clear();
        }
    }
    std::vector<StrLine> out;
    for (const auto &l : lines) {
        for (auto &s : SplitLines(l)) out.push_back(NormalizeLine(s));
    }
    return out;
}

class Reader {
public:
    Reader(const std::string &s, bool strict) : s_(s), strict_(strict) {
        line_starts_.push_back(0);
        for (size_t i = 0; i < s_.size(); i++) {
            if (s_[i] == '\n') line_starts_.push_back(i + 1);
        }
    }

    bool File(Whole &w) {
        SkipH();
        pending_ = ConvertLeading(TriviaList(), 0);
        ExprP e;
        if (!Expression(e)) return false;
        if (p_ != s_.size()) {
            Fail();
            return false;
        }
        w.e = std::move(e);
        w.trivia = std::move(pending_);
        pending_.clear();
        return true;
    }

    size_t far_ = 0;
    bool too_deep_ = false;
    int Line(size_t pos) const {
        return static_cast<int>(std::upper_bound(line_starts_.begin(), line_starts_.end(), pos) - line_starts_.begin());
    }

private:
    const std::string &s_;
    size_t p_ = 0;
    Trivia pending_;
    bool strict_;
    int depth_ = 0;
    std::vector<size_t> line_starts_;

    struct Saved {
        size_t p;
        Trivia pending;
    };
    Saved Save() const { return {p_, pending_}; }
    void Restore(const Saved &sv) {
        p_ = sv.p;
        pending_ = sv.pending;
    }
    struct DepthGuard {
        int &d;
        explicit DepthGuard(int &x) : d(x) { d++; }
        ~DepthGuard() { d--; }
        DepthGuard(const DepthGuard &) = delete;
        DepthGuard &operator=(const DepthGuard &) = delete;
    };

    bool AtEnd(size_t off = 0) const { return p_ + off >= s_.size(); }
    char C(size_t off = 0) const { return p_ + off < s_.size() ? s_[p_ + off] : '\0'; }
    bool Lit(std::string_view t) {
        if (s_.compare(p_, t.size(), t) != 0) return false;
        p_ += t.size();
        return true;
    }
    void Fail() { far_ = std::max(far_, p_); }
    void SkipH() {
        while (const size_t n = HSpaceAt(s_, p_)) p_ += n;
    }
    // megaparsec's column: 1-based, code points, tab stops every 8.
    int Col(size_t pos) const {
        size_t i = line_starts_[static_cast<size_t>(Line(pos) - 1)];
        int col = 1;
        for (; i < pos; i++) {
            const auto c = static_cast<unsigned char>(s_[i]);
            if ((c & 0xC0) == 0x80) continue;
            if (c == '\t') col = col + 8 - ((col - 1) % 8);
            else col++;
        }
        return col;
    }

    // ---- trivia ----

    std::vector<PT> TriviaList() {
        std::vector<PT> out;
        for (;;) {
            PT pt;
            if (LangAnnotation(pt) || LineCommentT(pt) || BlockCommentT(pt) || NewlinesT(pt)) {
                out.push_back(std::move(pt));
                continue;
            }
            return out;
        }
    }
    bool NewlinesT(PT &pt) {
        int n = 0;
        for (;;) {
            if (C() == '\n') p_++;
            else if (C() == '\r' && C(1) == '\n') p_ += 2;
            else break;
            SkipH();
            n++;
        }
        if (n == 0) return false;
        pt.kind = PT::Newlines;
        pt.n = n;
        return true;
    }
    bool LineCommentT(PT &pt) {
        if (C() != '#') return false;
        pt.kind = PT::LineComment;
        pt.col = Col(p_);
        p_++;
        const size_t st = p_;
        while (!AtEnd() && C() != '\n' && C() != '\r') p_++;
        pt.text = StripEnd(s_.substr(st, p_ - st));
        SkipH();
        return true;
    }
    bool BlockCommentT(PT &pt) {
        if (!(C() == '/' && C(1) == '*')) return false;
        const size_t save = p_;
        const int pos = Col(p_) - 1;
        p_ += 2;
        const bool doc = C() == '*' && C(1) != '/' && !AtEnd(1);
        if (doc) p_++;
        const size_t end = s_.find("*/", p_);
        if (end == std::string::npos) {
            p_ = save;
            return false;
        }
        const std::string raw = s_.substr(p_, end - p_);
        p_ = end + 2;
        SkipH();
        pt.kind = PT::BlockComment;
        pt.doc = doc;
        pt.lines = BlockLines(raw, pos);
        return true;
    }
    bool LangAnnotation(PT &pt) {
        const size_t save = p_;
        PT b;
        if (!BlockCommentT(b)) return false;
        if (b.doc || b.lines.size() != 1) {
            p_ = save;
            return false;
        }
        size_t q = p_;
        while (const size_t n = HSpaceAt(s_, q)) q += n;
        if (q < s_.size() && s_[q] == '\n') q++;
        else if (q + 1 < s_.size() && s_[q] == '\r' && s_[q + 1] == '\n') q += 2;
        while (const size_t n = HSpaceAt(s_, q)) q += n;
        const bool delim = q < s_.size() && (s_[q] == '"' || s_.compare(q, 2, "''") == 0);
        const std::string content = Strip(b.lines[0]);
        bool valid = !content.empty() && content.size() <= 30;
        for (char c : content) {
            if (!(IsAlnum(c) || c == '-' || c == '+' || c == '.' || c == '_')) valid = false;
        }
        if (!delim || !valid) {
            p_ = save;
            return false;
        }
        pt.kind = PT::LangAnnot;
        pt.text = content;
        return true;
    }

    // lexeme: the token, then the trivia after it split into this token's
    // trailing comment and the next token's leading trivia.
    template <class F>
    bool Lexeme(AnnHdr &h, F &&token) {
        const size_t save = p_;
        Trivia lead = std::move(pending_);
        pending_.clear();
        const int line = strict_ ? 1 : Line(p_);
        if (!token()) {
            p_ = save;
            pending_ = std::move(lead);
            Fail();
            return false;
        }
        SkipH();
        std::vector<PT> pts = TriviaList();
        const int col = Col(p_);
        size_t k = 0;
        while (k < pts.size() && IsTrailing(pts[k])) k++;
        bool all_leading = false;
        if (k == 1 && pts[0].kind == PT::LineComment) {
            if (pts.size() >= 3 && pts[1].kind == PT::Newlines && pts[1].n == 1 && pts[2].kind == PT::LineComment &&
                pts[0].col == pts[2].col)
                all_leading = true;
            if (pts.size() == 2 && pts[1].kind == PT::Newlines && pts[1].n == 1 && pts[0].col == col) all_leading = true;
        }
        if (all_leading) {
            h.trail.reset();
            pending_ = ConvertLeading(pts, 0);
        } else {
            h.trail = ConvertTrailing(pts, k);
            pending_ = ConvertLeading(pts, k);
        }
        h.pre = std::move(lead);
        h.line = line;
        return true;
    }

    bool Symbol(Leaf &l, Tok t, std::string_view text) {
        l.tok = t;
        return Lexeme(l, [&] { return Lit(text); });
    }
    bool Reserved(Leaf &l, Tok t, std::string_view kw) {
        l.tok = t;
        return Lexeme(l, [&] {
            const size_t st = p_;
            if (!Lit(kw)) return false;
            if (AtEnd() || IdentChar(C()) || PathChar(C())) {
                p_ = st;
                return false;
            }
            return true;
        });
    }
    // An operator, not the prefix of a longer one.
    bool Operator(Leaf &l, Tok t) {
        std::string_view text, not_next;
        switch (t) {
        case Tok::Concat: text = "++"; break;
        case Tok::Mul: text = "*"; not_next = "/"; break;
        case Tok::Div: text = "/"; not_next = "/*"; break;
        case Tok::Plus: text = "+"; not_next = "+"; break;
        case Tok::Minus: text = "-"; not_next = ">"; break;
        case Tok::Not: text = "!"; not_next = "="; break;
        case Tok::Update: text = "//"; break;
        case Tok::Less: text = "<"; not_next = "=|"; break;
        case Tok::Greater: text = ">"; not_next = "="; break;
        case Tok::LessEqual: text = "<="; break;
        case Tok::GreaterEqual: text = ">="; break;
        case Tok::Equal: text = "=="; break;
        case Tok::Unequal: text = "!="; break;
        case Tok::And: text = "&&"; break;
        case Tok::Or: text = "||"; break;
        case Tok::Implies: text = "->"; break;
        case Tok::PipeForward: text = "|>"; break;
        case Tok::PipeBackward: text = "<|"; break;
        case Tok::Question: text = "?"; break;
        default: return false;
        }
        l.tok = t;
        return Lexeme(l, [&] {
            const size_t st = p_;
            if (!Lit(text)) return false;
            if (!AtEnd() && !not_next.empty() && not_next.find(C()) != std::string_view::npos) {
                p_ = st;
                return false;
            }
            return true;
        });
    }

    // ---- values ----

    bool Identifier(Leaf &l) {
        l.tok = Tok::Identifier;
        return Lexeme(l, [&] {
            if (!(IsAlpha(C()) || C() == '_')) return false;
            const size_t st = p_;
            p_++;
            while (!AtEnd() && IdentChar(C())) p_++;
            std::string id = s_.substr(st, p_ - st);
            for (const char *r : kReserved) {
                if (id == r) {
                    p_ = st;
                    return false;
                }
            }
            l.text = std::move(id);
            return true;
        });
    }
    bool Integer(Leaf &l) {
        l.tok = Tok::Integer;
        return Lexeme(l, [&] {
            const size_t st = p_;
            while (!AtEnd() && IsDigit(C())) p_++;
            if (p_ == st) return false;
            l.text = s_.substr(st, p_ - st);
            return true;
        });
    }
    // (([1-9][0-9]*\.[0-9]*)|(0?\.[0-9]+))([Ee][+-]?[0-9]+)?
    bool Float(Leaf &l) {
        l.tok = Tok::Float;
        return Lexeme(l, [&] {
            const size_t st = p_;
            bool ok = false;
            if (C() >= '1' && C() <= '9') {
                p_++;
                while (IsDigit(C())) p_++;
                if (C() == '.') {
                    p_++;
                    while (IsDigit(C())) p_++;
                    ok = true;
                } else {
                    p_ = st;
                }
            }
            if (!ok) {
                if (C() == '0') p_++;
                if (C() != '.') {
                    p_ = st;
                    return false;
                }
                p_++;
                if (!IsDigit(C())) {
                    p_ = st;
                    return false;
                }
                while (IsDigit(C())) p_++;
            }
            if (C() == 'e' || C() == 'E') {
                size_t q = p_ + 1;
                if (q < s_.size() && (s_[q] == '+' || s_[q] == '-')) q++;
                if (q < s_.size() && IsDigit(s_[q])) {
                    while (q < s_.size() && IsDigit(s_[q])) q++;
                    p_ = q;
                } else {
                    p_ = st;  // an exponent marker with no digits fails the float
                    return false;
                }
            }
            l.text = s_.substr(st, p_ - st);
            return true;
        });
    }
    bool SomePathChars() {
        const size_t st = p_;
        while (!AtEnd() && PathChar(C())) p_++;
        return p_ > st;
    }
    bool Slash() {
        if (C() != '/' || C(1) == '/') return false;
        p_++;
        return true;
    }
    bool EnvPath(Leaf &l) {
        l.tok = Tok::EnvPath;
        return Lexeme(l, [&] {
            const size_t st = p_;
            if (C() != '<') return false;
            p_++;
            if (!SomePathChars()) {
                p_ = st;
                return false;
            }
            for (;;) {
                const size_t q = p_;
                if (Slash() && SomePathChars()) continue;
                p_ = q;
                break;
            }
            if (C() != '>') {
                p_ = st;
                return false;
            }
            p_++;
            l.text = s_.substr(st + 1, p_ - st - 2);
            return true;
        });
    }

    // ${ expression } -- a whole of its own: its trivia never mixes with the
    // surrounding token's.
    bool Interpolation(StringPart &out) {
        if (!(C() == '$' && C(1) == '{')) return false;
        Saved sv = Save();
        p_ += 2;
        Trivia outer = std::move(pending_);
        pending_.clear();
        SkipH();
        pending_ = ConvertLeading(TriviaList(), 0);
        ExprP e;
        // nixfmt --strict forgets line numbers with mapAllTokens, which never
        // descends into a string's interpolations: inside them it keeps them.
        const bool strict = strict_;
        strict_ = false;
        const bool ok = Expression(e) && C() == '}';
        strict_ = strict;
        if (!ok) {
            Fail();
            pending_ = std::move(outer);
            Restore(sv);
            return false;
        }
        p_++;
        auto w = std::make_shared<Whole>();
        w->e = std::move(e);
        w->trivia = std::move(pending_);
        pending_ = std::move(outer);
        out.interp = true;
        out.whole = std::move(w);
        return true;
    }

    bool Path(PathAnn &pa) {
        return Lexeme(pa, [&] {
            const size_t st = p_;
            StrLine parts;
            {
                const size_t q = p_;
                if (SomePathChars()) {
                    StringPart tp;
                    tp.text = s_.substr(q, p_ - q);
                    parts.push_back(std::move(tp));
                }
            }
            int traversals = 0;
            for (;;) {
                const size_t q = p_;
                if (!Slash()) break;
                StrLine seg;
                StringPart slash;
                slash.text = "/";
                seg.push_back(std::move(slash));
                int n = 0;
                for (;;) {
                    const size_t r = p_;
                    if (SomePathChars()) {
                        StringPart tp;
                        tp.text = s_.substr(r, p_ - r);
                        seg.push_back(std::move(tp));
                        n++;
                        continue;
                    }
                    StringPart ip;
                    if (Interpolation(ip)) {
                        seg.push_back(std::move(ip));
                        n++;
                        continue;
                    }
                    break;
                }
                if (n == 0) {
                    p_ = q;
                    break;
                }
                parts.insert(parts.end(), seg.begin(), seg.end());
                traversals++;
            }
            if (traversals == 0) {
                p_ = st;
                return false;
            }
            pa.parts = NormalizeLine(parts);
            return true;
        });
    }

    bool SimpleStringRaw(std::vector<StrLine> &lines) {
        const size_t st = p_;
        if (C() != '"') return false;
        p_++;
        StrLine parts;
        for (;;) {
            std::string text;
            for (;;) {
                if (AtEnd()) break;
                const char c = C();
                if (c == '\\') {
                    if (AtEnd(1)) break;
                    text += c;
                    text += C(1);
                    p_ += 2;
                } else if (c == '$' && C(1) == '$') {
                    text += "$$";
                    p_ += 2;
                } else if (c == '$' && C(1) != '{') {
                    text += c;
                    p_++;
                } else if (c != '"' && c != '$') {
                    text += c;
                    p_++;
                } else {
                    break;
                }
            }
            if (!text.empty()) {
                StringPart tp;
                tp.text = std::move(text);
                parts.push_back(std::move(tp));
                continue;
            }
            StringPart ip;
            if (Interpolation(ip)) {
                parts.push_back(std::move(ip));
                continue;
            }
            break;
        }
        if (C() != '"') {
            p_ = st;
            return false;
        }
        p_++;
        lines = FixSimpleString(parts);
        return true;
    }
    bool Uri(std::vector<StrLine> &lines) {
        const size_t st = p_;
        while (!AtEnd() && SchemeChar(C())) p_++;
        if (p_ == st || C() != ':') {
            p_ = st;
            return false;
        }
        p_++;
        const size_t rest = p_;
        while (!AtEnd() && UriChar(C())) p_++;
        if (p_ == rest) {
            p_ = st;
            return false;
        }
        StringPart tp;
        tp.text = s_.substr(st, p_ - st);
        lines = {StrLine{tp}};
        return true;
    }
    // One run of literal text inside '' '', matching Nix's own lexer rules.
    bool IndentedText(std::string &text) {
        const size_t st = p_;
        for (;;) {
            const char c = C();
            if (AtEnd()) break;
            if (c != '$' && c != '\'' && c != '\n') {
                text += c;
                p_++;
            } else if (c == '$' && !AtEnd(1) && C(1) != '{' && C(1) != '\'' && C(1) != '\n') {
                text += c;
                text += C(1);
                p_ += 2;
            } else if (c == '\'' && !AtEnd(1) && C(1) != '\'' && C(1) != '$' && C(1) != '\n') {
                text += c;
                text += C(1);
                p_ += 2;
            } else if (s_.compare(p_, 3, "''$") == 0 || s_.compare(p_, 3, "'''") == 0) {
                text += s_.substr(p_, 3);
                p_ += 3;
            } else if (s_.compare(p_, 3, "''\\") == 0) {
                text += "''\\";
                p_ += 3;
                if (!AtEnd() && C() != '\n') {
                    text += C();
                    p_++;
                }
            } else if (c == '$' && C(1) != '{') {
                text += c;
                p_++;
            } else if (c == '\'' && C(1) != '\'') {
                text += c;
                p_++;
            } else {
                break;
            }
        }
        return p_ > st;
    }
    bool IndentedStringRaw(std::vector<StrLine> &out) {
        const size_t st = p_;
        if (!Lit("''")) return false;
        std::vector<StrLine> lines;
        for (;;) {
            StrLine line;
            for (;;) {
                std::string text;
                if (IndentedText(text)) {
                    StringPart tp;
                    tp.text = std::move(text);
                    line.push_back(std::move(tp));
                    continue;
                }
                StringPart ip;
                if (Interpolation(ip)) {
                    line.push_back(std::move(ip));
                    continue;
                }
                break;
            }
            lines.push_back(std::move(line));
            if (C() == '\n') {
                p_++;
                continue;
            }
            break;
        }
        if (!Lit("''")) {
            p_ = st;
            return false;
        }
        out = FixIndentedString(std::move(lines));
        return true;
    }

    bool String(Term &t) {
        StrAnn sa;
        if (Lexeme(sa, [&] { return SimpleStringRaw(sa.lines) || Uri(sa.lines); })) {
            t.kind = Term::SimpleString;
            t.str = std::move(sa);
            return true;
        }
        if (!Lexeme(sa, [&] { return IndentedStringRaw(sa.lines); })) return false;
        // classifyString: a one-line '' string with nothing to escape is
        // written as a "" string.
        bool simple = sa.lines.size() <= 1;
        for (const auto &l : sa.lines) {
            for (const auto &p : l) {
                if (!p.interp && p.text.find_first_of("\"\\") != std::string::npos) simple = false;
            }
        }
        if (simple) {
            for (auto &l : sa.lines) {
                for (auto &p : l) {
                    if (p.interp) continue;
                    std::string o;
                    for (size_t i = 0; i < p.text.size();) {
                        if (p.text.compare(i, 3, "''$") == 0) {
                            o += "\\$";
                            i += 3;
                        } else if (p.text.compare(i, 3, "'''") == 0) {
                            o += "''";
                            i += 3;
                        } else {
                            o += p.text[i++];
                        }
                    }
                    p.text = std::move(o);
                }
            }
            t.kind = Term::SimpleString;
        } else {
            t.kind = Term::IndentedString;
        }
        t.str = std::move(sa);
        return true;
    }

    // ---- terms ----

    bool Parens(Term &t) {
        Saved sv = Save();
        Leaf open, close;
        ExprP e;
        if (!Symbol(open, Tok::ParenOpen, "(")) return false;
        if (!Expression(e) || !Symbol(close, Tok::ParenClose, ")")) {
            Restore(sv);
            return false;
        }
        t.kind = Term::Parenthesized;
        t.open = std::move(open);
        t.close = std::move(close);
        t.paren = std::move(e);
        return true;
    }

    bool SimpleSelector(SimpleSel &s, bool restricted) {
        if (Identifier(s.id)) {
            s.kind = SimpleSel::Id;
            return true;
        }
        {
            Saved sv = Save();
            InterpAnn ia;
            if (Lexeme(ia, [&] { return Interpolation(ia.part); })) {
                const Expr &e = *ia.part.whole->e;
                if (!restricted || (e.kind == Expr::Term && e.term->kind == Term::SimpleString)) {
                    s.kind = SimpleSel::Interp;
                    s.interp = std::move(ia);
                    return true;
                }
                Restore(sv);
            }
        }
        StrAnn sa;
        if (Lexeme(sa, [&] { return SimpleStringRaw(sa.lines); })) {
            s.kind = SimpleSel::Str;
            s.str = std::move(sa);
            return true;
        }
        return false;
    }
    bool SelectorP(Selector &sel, bool with_dot) {
        Saved sv = Save();
        if (with_dot) {
            Leaf dot;
            if (!Symbol(dot, Tok::Dot, ".")) return false;
            sel.dot = std::move(dot);
        }
        {
            Saved look = Save();
            PathAnn pa;
            const bool path = Path(pa);
            Restore(look);
            if (path) {
                Restore(sv);
                return false;
            }
        }
        if (!SimpleSelector(sel.sel, false)) {
            Restore(sv);
            return false;
        }
        return true;
    }
    bool SelectorPath(std::vector<Selector> &sels) {
        Selector first;
        if (!SelectorP(first, false)) return false;
        sels.push_back(std::move(first));
        for (;;) {
            Selector s;
            if (!SelectorP(s, true)) break;
            sels.push_back(std::move(s));
        }
        return true;
    }

    bool SimpleTerm(Term &t) {
        if (String(t)) return true;
        {
            PathAnn pa;
            if (Path(pa)) {
                t.kind = Term::Path;
                t.path = std::move(pa);
                return true;
            }
        }
        if (EnvPath(t.leaf) || Float(t.leaf) || Integer(t.leaf) || Identifier(t.leaf)) {
            t.kind = Term::Token;
            return true;
        }
        if (Parens(t)) return true;
        if (Set(t)) return true;
        return List(t);
    }

    bool TermP_(TermP &out) {
        DepthGuard g(depth_);
        if (depth_ > kMaxDepth) {
            too_deep_ = true;
            return false;
        }
        Saved sv = Save();
        auto t = std::make_shared<Term>();
        if (!SimpleTerm(*t)) return false;
        std::vector<Selector> sels;
        for (;;) {
            Selector s;
            if (!SelectorP(s, true)) break;
            sels.push_back(std::move(s));
        }
        if (sels.empty()) {
            out = std::move(t);
            return true;
        }
        auto sel = std::make_shared<Term>();
        sel->kind = Term::Selection;
        sel->sel_term = std::move(t);
        sel->sels = std::move(sels);
        {
            Saved sv2 = Save();
            Leaf kw;
            TermP def;
            if (Reserved(kw, Tok::KOr, "or")) {
                if (TermP_(def)) {
                    sel->or_kw = std::move(kw);
                    sel->or_def = std::move(def);
                } else {
                    Restore(sv2);
                }
            }
        }
        (void)sv;
        out = std::move(sel);
        return true;
    }

    template <class T, class F>
    void Items(std::vector<Item<T>> &items, F &&item) {
        for (;;) {
            if (!pending_.empty()) {
                Item<T> c;
                c.comments = true;
                c.trivia = std::move(pending_);
                pending_.clear();
                items.push_back(std::move(c));
                continue;
            }
            Item<T> it;
            if (!item(it.value)) break;
            items.push_back(std::move(it));
        }
    }

    bool Inherit(Binder &b) {
        Saved sv = Save();
        if (!Reserved(b.kw, Tok::KInherit, "inherit")) return false;
        b.kind = Binder::Inherit;
        {
            auto from = std::make_shared<Term>();
            if (Parens(*from)) b.from = std::move(from);
        }
        for (;;) {
            SimpleSel s;
            if (!SimpleSelector(s, true)) break;
            b.ids.push_back(std::move(s));
        }
        if (!Symbol(b.semi, Tok::Semicolon, ";")) {
            Restore(sv);
            return false;
        }
        return true;
    }
    bool Assignment(Binder &b) {
        Saved sv = Save();
        b.kind = Binder::Assignment;
        if (!SelectorPath(b.sels) || !Symbol(b.eq, Tok::Assign, "=") || !Expression(b.rhs) ||
            !Symbol(b.semi, Tok::Semicolon, ";")) {
            Restore(sv);
            b = Binder{};
            return false;
        }
        return true;
    }
    bool BinderP(Binder &b) {
        if (Assignment(b)) return true;
        b = Binder{};
        return Inherit(b);
    }

    bool Set(Term &t) {
        Saved sv = Save();
        {
            Leaf kw;
            if (Reserved(kw, Tok::KRec, "rec") || Reserved(kw, Tok::KLet, "let")) t.rec = std::move(kw);
        }
        if (!Symbol(t.open, Tok::BraceOpen, "{")) {
            Restore(sv);
            t.rec.reset();
            return false;
        }
        Items(t.set_items, [&](Binder &b) { return BinderP(b); });
        if (!Symbol(t.close, Tok::BraceClose, "}")) {
            Restore(sv);
            t.rec.reset();
            t.set_items.clear();
            return false;
        }
        t.kind = Term::Set;
        return true;
    }
    bool List(Term &t) {
        Saved sv = Save();
        if (!Symbol(t.open, Tok::BrackOpen, "[")) return false;
        Items(t.list_items, [&](TermP &x) { return TermP_(x); });
        if (!Symbol(t.close, Tok::BrackClose, "]")) {
            Restore(sv);
            t.list_items.clear();
            return false;
        }
        t.kind = Term::List;
        return true;
    }

    // ---- abstractions ----

    bool AttrParameter(ParamAttr &a, bool with_comma) {
        Saved sv = Save();
        if (!Identifier(a.name)) return false;
        {
            Saved sv2 = Save();
            Leaf q;
            ExprP def;
            if (Symbol(q, Tok::Question, "?")) {
                if (Expression(def)) {
                    a.qmark = std::move(q);
                    a.def = std::move(def);
                } else {
                    Restore(sv2);
                }
            }
        }
        if (with_comma) {
            Leaf comma;
            if (!Symbol(comma, Tok::Comma, ",")) {
                Restore(sv);
                a = ParamAttr{};
                return false;
            }
            a.comma = std::move(comma);
        }
        return true;
    }
    bool IdParameter(ParamP &out) {
        auto p = std::make_shared<Param>();
        if (!Identifier(p->name)) return false;
        p->kind = Param::Id;
        out = std::move(p);
        return true;
    }
    bool SetParameter(ParamP &out) {
        Saved sv = Save();
        auto p = std::make_shared<Param>();
        p->kind = Param::Set;
        if (!Symbol(p->open, Tok::BraceOpen, "{")) return false;
        for (;;) {
            ParamAttr a;
            if (!AttrParameter(a, true)) break;
            p->attrs.push_back(std::move(a));
        }
        {
            ParamAttr a;
            if (AttrParameter(a, false)) {
                p->attrs.push_back(std::move(a));
            } else {
                ParamAttr e;
                e.ellipsis = true;
                if (Symbol(e.name, Tok::Ellipsis, "...")) p->attrs.push_back(std::move(e));
            }
        }
        if (!Symbol(p->close, Tok::BraceClose, "}")) {
            Restore(sv);
            return false;
        }
        out = std::move(p);
        return true;
    }
    bool ContextParameter(ParamP &out) {
        Saved sv = Save();
        auto p = std::make_shared<Param>();
        p->kind = Param::Context;
        if (SetParameter(p->first) && Symbol(p->at, Tok::At, "@") && IdParameter(p->second)) {
            out = std::move(p);
            return true;
        }
        Restore(sv);
        p = std::make_shared<Param>();
        p->kind = Param::Context;
        if (IdParameter(p->first) && Symbol(p->at, Tok::At, "@") && SetParameter(p->second)) {
            out = std::move(p);
            return true;
        }
        Restore(sv);
        return false;
    }
    bool Abstraction(ExprP &out) {
        Saved sv = Save();
        auto e = std::make_shared<Expr>();
        e->kind = Expr::Abstraction;
        if (!(ContextParameter(e->param) || SetParameter(e->param) || IdParameter(e->param))) return false;
        if (!Symbol(e->k1, Tok::Colon, ":") || !Expression(e->a)) {
            Restore(sv);
            return false;
        }
        out = std::move(e);
        return true;
    }

    // ---- operators (nixfmt's `operators` table, strongest first) ----

    enum class Fix : unsigned char { Apply, Prefix, Postfix, InfixL, InfixN, InfixR };
    struct Level {
        Fix fix;
        std::vector<Tok> ops;
    };
    static const std::vector<Level> &Levels() {
        static const std::vector<Level> levels = {
            {Fix::Apply, {}},
            {Fix::Prefix, {Tok::Minus}},
            {Fix::Postfix, {Tok::Question}},
            {Fix::InfixR, {Tok::Concat}},
            {Fix::InfixL, {Tok::Mul, Tok::Div}},
            {Fix::InfixL, {Tok::Plus, Tok::Minus}},
            {Fix::Prefix, {Tok::Not}},
            {Fix::InfixR, {Tok::Update}},
            {Fix::InfixN, {Tok::Less, Tok::Greater, Tok::LessEqual, Tok::GreaterEqual}},
            {Fix::InfixN, {Tok::Equal, Tok::Unequal}},
            {Fix::InfixL, {Tok::And}},
            {Fix::InfixL, {Tok::Or}},
            {Fix::InfixL, {Tok::Implies}},
            {Fix::InfixL, {Tok::PipeForward}},
            {Fix::InfixR, {Tok::PipeBackward}},
        };
        return levels;
    }

    bool AnyOp(const Level &lv, Leaf &op) {
        for (Tok t : lv.ops) {
            if (Operator(op, t)) return true;
        }
        return false;
    }

    // `Term <$> term <* notFollowedBy (oneOf ":@")`
    bool BaseTerm(ExprP &out) {
        Saved sv = Save();
        TermP t;
        if (!TermP_(t)) return false;
        if (C() == ':' || C() == '@') {
            Restore(sv);
            return false;
        }
        auto e = std::make_shared<Expr>();
        e->kind = Expr::Term;
        e->term = std::move(t);
        out = std::move(e);
        return true;
    }

    static ExprP Bin(ExprP a, Leaf op, ExprP b) {
        auto e = std::make_shared<Expr>();
        e->kind = Expr::Operation;
        e->a = std::move(a);
        e->k1 = std::move(op);
        e->b = std::move(b);
        return e;
    }

    // makeExprParser's pTerm for level k: prefix? (level k-1) postfix?
    bool LevelTerm(int k, ExprP &out) {
        if (k < 0) return BaseTerm(out);
        const Level &lv = Levels()[static_cast<size_t>(k)];
        Saved sv = Save();
        std::vector<Leaf> prefix;
        if (lv.fix == Fix::Prefix) {
            for (;;) {
                Leaf op;
                if (!AnyOp(lv, op)) break;
                prefix.push_back(std::move(op));
            }
        }
        ExprP x;
        if (!LevelExpr(k - 1, x)) {
            Restore(sv);
            return false;
        }
        for (size_t i = prefix.size(); i-- > 0;) {
            auto e = std::make_shared<Expr>();
            e->kind = prefix[i].tok == Tok::Minus ? Expr::Negation : Expr::Inversion;
            e->k1 = std::move(prefix[i]);
            e->a = std::move(x);
            x = std::move(e);
        }
        if (lv.fix == Fix::Postfix) {
            Saved sv2 = Save();
            Leaf q;
            std::vector<Selector> sels;
            if (AnyOp(lv, q)) {
                if (SelectorPath(sels)) {
                    auto e = std::make_shared<Expr>();
                    e->kind = Expr::MemberCheck;
                    e->a = std::move(x);
                    e->k1 = std::move(q);
                    e->sels = std::move(sels);
                    x = std::move(e);
                } else {
                    Restore(sv2);
                }
            }
        }
        out = std::move(x);
        return true;
    }

    bool LevelExpr(int k, ExprP &out) {
        if (k < 0) return BaseTerm(out);
        const Level &lv = Levels()[static_cast<size_t>(k)];
        ExprP x;
        if (!LevelTerm(k, x)) return false;
        switch (lv.fix) {
        case Fix::Apply:
            for (;;) {
                ExprP y;
                if (!LevelTerm(k, y)) break;
                auto e = std::make_shared<Expr>();
                e->kind = Expr::Application;
                e->a = std::move(x);
                e->b = std::move(y);
                x = std::move(e);
            }
            break;
        case Fix::InfixL:
            for (;;) {
                Saved sv = Save();
                Leaf op;
                ExprP y;
                if (!AnyOp(lv, op)) break;
                if (!LevelTerm(k, y)) {
                    Restore(sv);
                    break;
                }
                // nixfmt's HACK: `+` chains are re-associated to the right
                // so the printer can see the first operand.
                if (op.tok == Tok::Plus && x->kind == Expr::Operation && x->k1.tok == Tok::Plus) {
                    x = Bin(x->a, x->k1, Bin(x->b, std::move(op), std::move(y)));
                } else {
                    x = Bin(std::move(x), std::move(op), std::move(y));
                }
            }
            break;
        case Fix::InfixN: {
            Saved sv = Save();
            Leaf op;
            ExprP y;
            if (AnyOp(lv, op)) {
                if (LevelTerm(k, y)) x = Bin(std::move(x), std::move(op), std::move(y));
                else Restore(sv);
            }
            break;
        }
        case Fix::InfixR: {
            std::vector<ExprP> operands{x};
            std::vector<Leaf> ops;
            for (;;) {
                Saved sv = Save();
                Leaf op;
                ExprP y;
                if (!AnyOp(lv, op)) break;
                if (!LevelTerm(k, y)) {
                    Restore(sv);
                    break;
                }
                ops.push_back(std::move(op));
                operands.push_back(std::move(y));
            }
            x = operands.back();
            for (size_t i = ops.size(); i-- > 0;) x = Bin(operands[i], std::move(ops[i]), std::move(x));
            break;
        }
        case Fix::Prefix:
        case Fix::Postfix:
            break;
        }
        out = std::move(x);
        return true;
    }

    // ---- expressions ----

    bool With(ExprP &out) {
        Saved sv = Save();
        auto e = std::make_shared<Expr>();
        e->kind = Expr::With;
        if (!Reserved(e->k1, Tok::KWith, "with")) return false;
        if (!Expression(e->a) || !Symbol(e->k2, Tok::Semicolon, ";") || !Expression(e->b)) {
            Restore(sv);
            return false;
        }
        out = std::move(e);
        return true;
    }
    bool LetIn(ExprP &out) {
        Saved sv = Save();
        auto e = std::make_shared<Expr>();
        e->kind = Expr::Let;
        if (!Reserved(e->k1, Tok::KLet, "let")) return false;
        Items(e->binders, [&](Binder &b) { return BinderP(b); });
        if (!Reserved(e->k2, Tok::KIn, "in") || !Expression(e->a)) {
            Restore(sv);
            return false;
        }
        out = std::move(e);
        return true;
    }
    bool IfThenElse(ExprP &out) {
        Saved sv = Save();
        auto e = std::make_shared<Expr>();
        e->kind = Expr::If;
        if (!Reserved(e->k1, Tok::KIf, "if")) return false;
        if (!Expression(e->a) || !Reserved(e->k2, Tok::KThen, "then") || !Expression(e->b) ||
            !Reserved(e->k3, Tok::KElse, "else") || !Expression(e->c)) {
            Restore(sv);
            return false;
        }
        out = std::move(e);
        return true;
    }
    bool AssertE(ExprP &out) {
        Saved sv = Save();
        auto e = std::make_shared<Expr>();
        e->kind = Expr::Assert;
        if (!Reserved(e->k1, Tok::KAssert, "assert")) return false;
        if (!Expression(e->a) || !Symbol(e->k2, Tok::Semicolon, ";") || !Expression(e->b)) {
            Restore(sv);
            return false;
        }
        out = std::move(e);
        return true;
    }

    bool Expression(ExprP &out) {
        DepthGuard g(depth_);
        if (depth_ > kMaxDepth) {
            too_deep_ = true;
            return false;
        }
        if (too_deep_) return false;
        return LevelExpr(static_cast<int>(Levels().size()) - 1, out) || Abstraction(out) || With(out) ||
               LetIn(out) || IfThenElse(out) || AssertE(out);
    }
};

// ---- equality ----------------------------------------------------------------

bool Eq(const Leaf &a, const Leaf &b) { return a.tok == b.tok && a.text == b.text; }
bool Eq(const std::optional<Leaf> &a, const std::optional<Leaf> &b) {
    if (a.has_value() != b.has_value()) return false;
    return !a || Eq(*a, *b);
}
bool Eq(const Expr &a, const Expr &b);
bool Eq(const ExprP &a, const ExprP &b) {
    if (!a || !b) return !a && !b;
    return Eq(*a, *b);
}
bool Eq(const StringPart &a, const StringPart &b) {
    if (a.interp != b.interp) return false;
    return a.interp ? Eq(a.whole->e, b.whole->e) : a.text == b.text;
}
bool Eq(const StrLine &a, const StrLine &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (!Eq(a[i], b[i])) return false;
    }
    return true;
}
bool Eq(const std::vector<StrLine> &a, const std::vector<StrLine> &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (!Eq(a[i], b[i])) return false;
    }
    return true;
}
bool Eq(const SimpleSel &a, const SimpleSel &b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
    case SimpleSel::Id: return Eq(a.id, b.id);
    case SimpleSel::Interp: return Eq(a.interp.part, b.interp.part);
    case SimpleSel::Str: return Eq(a.str.lines, b.str.lines);
    }
    return false;
}
bool Eq(const Selector &a, const Selector &b) { return Eq(a.dot, b.dot) && Eq(a.sel, b.sel); }
template <class T, class F>
bool EqVec(const std::vector<T> &a, const std::vector<T> &b, F &&f) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (!f(a[i], b[i])) return false;
    }
    return true;
}
bool EqSels(const std::vector<Selector> &a, const std::vector<Selector> &b) {
    return EqVec(a, b, [](const Selector &x, const Selector &y) { return Eq(x, y); });
}
template <class T, class F>
bool EqItems(const std::vector<Item<T>> &a, const std::vector<Item<T>> &b, F &&f) {
    std::vector<const T *> xa, xb;
    for (const auto &i : a) {
        if (!i.comments) xa.push_back(&i.value);
    }
    for (const auto &i : b) {
        if (!i.comments) xb.push_back(&i.value);
    }
    if (xa.size() != xb.size()) return false;
    for (size_t i = 0; i < xa.size(); i++) {
        if (!f(*xa[i], *xb[i])) return false;
    }
    return true;
}
bool Eq(const Term &a, const Term &b);
bool Eq(const TermP &a, const TermP &b) {
    if (!a || !b) return !a && !b;
    return Eq(*a, *b);
}
bool Eq(const Binder &a, const Binder &b) {
    if (a.kind != b.kind || !Eq(a.semi, b.semi)) return false;
    if (a.kind == Binder::Inherit) {
        return Eq(a.kw, b.kw) && Eq(a.from, b.from) &&
               EqVec(a.ids, b.ids, [](const SimpleSel &x, const SimpleSel &y) { return Eq(x, y); });
    }
    return EqSels(a.sels, b.sels) && Eq(a.eq, b.eq) && Eq(a.rhs, b.rhs);
}
bool EqBinders(const std::vector<Item<Binder>> &a, const std::vector<Item<Binder>> &b) {
    return EqItems(a, b, [](const Binder &x, const Binder &y) { return Eq(x, y); });
}
bool Eq(const Term &a, const Term &b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
    case Term::Token: return Eq(a.leaf, b.leaf);
    case Term::SimpleString:
    case Term::IndentedString: return Eq(a.str.lines, b.str.lines);
    case Term::Path: return Eq(a.path.parts, b.path.parts);
    case Term::List:
        return Eq(a.open, b.open) && Eq(a.close, b.close) &&
               EqItems(a.list_items, b.list_items, [](const TermP &x, const TermP &y) { return Eq(x, y); });
    case Term::Set: return Eq(a.rec, b.rec) && Eq(a.open, b.open) && Eq(a.close, b.close) && EqBinders(a.set_items, b.set_items);
    case Term::Selection:
        return Eq(a.sel_term, b.sel_term) && EqSels(a.sels, b.sels) && Eq(a.or_kw, b.or_kw) && Eq(a.or_def, b.or_def);
    case Term::Parenthesized: return Eq(a.open, b.open) && Eq(a.close, b.close) && Eq(a.paren, b.paren);
    }
    return false;
}
bool Eq(const ParamAttr &a, const ParamAttr &b, bool ignore_comma) {
    if (a.ellipsis != b.ellipsis || !Eq(a.name, b.name) || !Eq(a.qmark, b.qmark) || !Eq(a.def, b.def)) return false;
    return ignore_comma || Eq(a.comma, b.comma);
}
bool Eq(const Param &a, const Param &b);
bool Eq(const ParamP &a, const ParamP &b) {
    if (!a || !b) return !a && !b;
    return Eq(*a, *b);
}
bool Eq(const Param &a, const Param &b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
    case Param::Id: return Eq(a.name, b.name);
    case Param::Set: {
        if (!Eq(a.open, b.open) || !Eq(a.close, b.close) || a.attrs.size() != b.attrs.size()) return false;
        for (size_t i = 0; i < a.attrs.size(); i++) {
            const bool last = i + 1 == a.attrs.size() && !a.attrs[i].ellipsis && !b.attrs[i].ellipsis;
            if (!Eq(a.attrs[i], b.attrs[i], last)) return false;
        }
        return true;
    }
    case Param::Context: return Eq(a.first, b.first) && Eq(a.at, b.at) && Eq(a.second, b.second);
    }
    return false;
}
bool Eq(const Expr &a, const Expr &b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
    case Expr::Term: return Eq(a.term, b.term);
    case Expr::With:
    case Expr::Assert: return Eq(a.k1, b.k1) && Eq(a.a, b.a) && Eq(a.k2, b.k2) && Eq(a.b, b.b);
    case Expr::Let: return Eq(a.k1, b.k1) && EqBinders(a.binders, b.binders) && Eq(a.k2, b.k2) && Eq(a.a, b.a);
    case Expr::If:
        return Eq(a.k1, b.k1) && Eq(a.a, b.a) && Eq(a.k2, b.k2) && Eq(a.b, b.b) && Eq(a.k3, b.k3) && Eq(a.c, b.c);
    case Expr::Abstraction: return Eq(a.param, b.param) && Eq(a.k1, b.k1) && Eq(a.a, b.a);
    case Expr::Application: return Eq(a.a, b.a) && Eq(a.b, b.b);
    case Expr::Operation: return Eq(a.a, b.a) && Eq(a.k1, b.k1) && Eq(a.b, b.b);
    case Expr::MemberCheck: return Eq(a.a, b.a) && Eq(a.k1, b.k1) && EqSels(a.sels, b.sels);
    case Expr::Negation:
    case Expr::Inversion: return Eq(a.k1, b.k1) && Eq(a.a, b.a);
    }
    return false;
}

// ---- comment words -------------------------------------------------------------

struct WordCollector {
    std::vector<std::string> &out;
    void Words(const std::string &s) {
        size_t i = 0;
        while (i < s.size()) {
            while (const size_t n = SpaceAt(s, i)) i += n;
            const size_t st = i;
            while (i < s.size() && !SpaceAt(s, i)) i++;
            if (i > st) out.push_back(s.substr(st, i - st));
        }
    }
    void Triv(const Trivia &t) {
        for (const auto &x : t) {
            Words(x.text);
            for (const auto &l : x.lines) Words(l);
        }
    }
    void Hdr(const AnnHdr &h) {
        Triv(h.pre);
        if (h.trail) Words(*h.trail);
    }
    void Line(const StrLine &l) {
        for (const auto &p : l) {
            if (p.interp) WholeW(*p.whole);
        }
    }
    void WholeW(const Whole &w) {
        E(*w.e);
        Triv(w.trivia);
    }
    void Simple(const SimpleSel &s) {
        switch (s.kind) {
        case SimpleSel::Id: Hdr(s.id); break;
        case SimpleSel::Interp:
            Hdr(s.interp);
            if (s.interp.part.interp) WholeW(*s.interp.part.whole);
            break;
        case SimpleSel::Str:
            Hdr(s.str);
            for (const auto &l : s.str.lines) Line(l);
            break;
        }
    }
    void Sel(const Selector &s) {
        if (s.dot) Hdr(*s.dot);
        Simple(s.sel);
    }
    void B(const Binder &b) {
        if (b.kind == Binder::Inherit) {
            Hdr(b.kw);
            if (b.from) T(*b.from);
            for (const auto &s : b.ids) Simple(s);
        } else {
            for (const auto &s : b.sels) Sel(s);
            Hdr(b.eq);
            E(*b.rhs);
        }
        Hdr(b.semi);
    }
    void Binders(const std::vector<Item<Binder>> &items) {
        for (const auto &i : items) {
            if (i.comments) Triv(i.trivia);
            else B(i.value);
        }
    }
    void T(const Term &t) {
        switch (t.kind) {
        case Term::Token: Hdr(t.leaf); break;
        case Term::SimpleString:
        case Term::IndentedString:
            Hdr(t.str);
            for (const auto &l : t.str.lines) Line(l);
            break;
        case Term::Path:
            Hdr(t.path);
            Line(t.path.parts);
            break;
        case Term::List:
            Hdr(t.open);
            for (const auto &i : t.list_items) {
                if (i.comments) Triv(i.trivia);
                else T(*i.value);
            }
            Hdr(t.close);
            break;
        case Term::Set:
            if (t.rec) Hdr(*t.rec);
            Hdr(t.open);
            Binders(t.set_items);
            Hdr(t.close);
            break;
        case Term::Selection:
            T(*t.sel_term);
            for (const auto &s : t.sels) Sel(s);
            if (t.or_kw) {
                Hdr(*t.or_kw);
                T(*t.or_def);
            }
            break;
        case Term::Parenthesized:
            Hdr(t.open);
            E(*t.paren);
            Hdr(t.close);
            break;
        }
    }
    void P(const Param &p) {
        switch (p.kind) {
        case Param::Id: Hdr(p.name); break;
        case Param::Set:
            Hdr(p.open);
            for (const auto &a : p.attrs) {
                Hdr(a.name);
                if (a.qmark) {
                    Hdr(*a.qmark);
                    E(*a.def);
                }
                if (a.comma) Hdr(*a.comma);
            }
            Hdr(p.close);
            break;
        case Param::Context:
            P(*p.first);
            Hdr(p.at);
            P(*p.second);
            break;
        }
    }
    void E(const Expr &e) {
        switch (e.kind) {
        case Expr::Term: T(*e.term); break;
        case Expr::With:
        case Expr::Assert:
            Hdr(e.k1);
            E(*e.a);
            Hdr(e.k2);
            E(*e.b);
            break;
        case Expr::Let:
            Hdr(e.k1);
            Binders(e.binders);
            Hdr(e.k2);
            E(*e.a);
            break;
        case Expr::If:
            Hdr(e.k1);
            E(*e.a);
            Hdr(e.k2);
            E(*e.b);
            Hdr(e.k3);
            E(*e.c);
            break;
        case Expr::Abstraction:
            P(*e.param);
            Hdr(e.k1);
            E(*e.a);
            break;
        case Expr::Application:
            E(*e.a);
            E(*e.b);
            break;
        case Expr::Operation:
            E(*e.a);
            Hdr(e.k1);
            E(*e.b);
            break;
        case Expr::MemberCheck:
            E(*e.a);
            Hdr(e.k1);
            for (const auto &s : e.sels) Sel(s);
            break;
        case Expr::Negation:
        case Expr::Inversion:
            Hdr(e.k1);
            E(*e.a);
            break;
        }
    }
};

// ---- first / last token -----------------------------------------------------------

AnnHdr &SimpleHdr(SimpleSel &s) {
    switch (s.kind) {
    case SimpleSel::Id: return s.id;
    case SimpleSel::Interp: return s.interp;
    case SimpleSel::Str: return s.str;
    }
    return s.id;
}
const AnnHdr &SimpleHdr(const SimpleSel &s) { return SimpleHdr(const_cast<SimpleSel &>(s)); }

}  // namespace

size_t SpaceAt(const std::string &s, size_t i) {
    if (i >= s.size()) return 0;
    const auto b = [&](size_t k) { return i + k < s.size() ? static_cast<unsigned char>(s[i + k]) : 0u; };
    const unsigned c = b(0);
    if (c == ' ' || (c >= 0x09 && c <= 0x0d)) return 1;
    if (c == 0xC2 && b(1) == 0xA0) return 2;                                   // U+00A0
    if (c == 0xE1 && b(1) == 0x9A && b(2) == 0x80) return 3;                   // U+1680
    if (c == 0xE2 && b(1) == 0x80 && (b(2) <= 0x8A || b(2) == 0xAF)) return 3;  // U+2000-200A, U+202F
    if (c == 0xE2 && b(1) == 0x81 && b(2) == 0x9F) return 3;                   // U+205F
    if (c == 0xE3 && b(1) == 0x80 && b(2) == 0x80) return 3;                   // U+3000
    return 0;
}
size_t SpaceBefore(const std::string &s, size_t end) {
    for (size_t n = 1; n <= 3 && n <= end; n++) {
        if (SpaceAt(s, end - n) == n) return n;
    }
    return 0;
}
std::string StripSpace(const std::string &s) { return Strip(s); }

std::string TokenText(const Leaf &l) {
    switch (l.tok) {
    case Tok::Integer:
    case Tok::Float:
    case Tok::Identifier: return l.text;
    case Tok::EnvPath: return "<" + l.text + ">";
    case Tok::KAssert: return "assert";
    case Tok::KElse: return "else";
    case Tok::KIf: return "if";
    case Tok::KIn: return "in";
    case Tok::KInherit: return "inherit";
    case Tok::KLet: return "let";
    case Tok::KOr: return "or";
    case Tok::KRec: return "rec";
    case Tok::KThen: return "then";
    case Tok::KWith: return "with";
    case Tok::BraceOpen: return "{";
    case Tok::BraceClose: return "}";
    case Tok::BrackOpen: return "[";
    case Tok::BrackClose: return "]";
    case Tok::InterOpen: return "${";
    case Tok::InterClose: return "}";
    case Tok::ParenOpen: return "(";
    case Tok::ParenClose: return ")";
    case Tok::Assign: return "=";
    case Tok::At: return "@";
    case Tok::Colon: return ":";
    case Tok::Comma: return ",";
    case Tok::Dot: return ".";
    case Tok::DoubleQuote: return "\"";
    case Tok::DoubleSingleQuote: return "''";
    case Tok::Ellipsis: return "...";
    case Tok::Question: return "?";
    case Tok::Semicolon: return ";";
    case Tok::Concat: return "++";
    case Tok::Negate:
    case Tok::Minus: return "-";
    case Tok::Update: return "//";
    case Tok::Plus: return "+";
    case Tok::Mul: return "*";
    case Tok::Div: return "/";
    case Tok::And: return "&&";
    case Tok::Or: return "||";
    case Tok::Equal: return "==";
    case Tok::Greater: return ">";
    case Tok::GreaterEqual: return ">=";
    case Tok::Implies: return "->";
    case Tok::Less: return "<";
    case Tok::LessEqual: return "<=";
    case Tok::Not: return "!";
    case Tok::Unequal: return "!=";
    case Tok::PipeForward: return "|>";
    case Tok::PipeBackward: return "<|";
    }
    return "";
}

const AnnHdr &FirstTok(const Term &t) {
    switch (t.kind) {
    case Term::Token: return t.leaf;
    case Term::SimpleString:
    case Term::IndentedString: return t.str;
    case Term::Path: return t.path;
    case Term::List: return t.open;
    case Term::Set: return t.rec ? *t.rec : t.open;
    case Term::Selection: return FirstTok(*t.sel_term);
    case Term::Parenthesized: return t.open;
    }
    return t.leaf;
}
const AnnHdr &FirstTok(const Param &p) {
    switch (p.kind) {
    case Param::Id: return p.name;
    case Param::Set: return p.open;
    case Param::Context: return FirstTok(*p.first);
    }
    return p.name;
}
const AnnHdr &FirstTok(const Expr &e) {
    switch (e.kind) {
    case Expr::Term: return FirstTok(*e.term);
    case Expr::With:
    case Expr::Let:
    case Expr::Assert:
    case Expr::If:
    case Expr::Negation:
    case Expr::Inversion: return e.k1;
    case Expr::Abstraction: return FirstTok(*e.param);
    case Expr::Application:
    case Expr::Operation:
    case Expr::MemberCheck: return FirstTok(*e.a);
    }
    return e.k1;
}

TermP MapFirst(const TermP &t, const HdrFn &f) {
    auto c = std::make_shared<Term>(*t);
    switch (c->kind) {
    case Term::Token: f(c->leaf); break;
    case Term::SimpleString:
    case Term::IndentedString: f(c->str); break;
    case Term::Path: f(c->path); break;
    case Term::List: f(c->open); break;
    case Term::Set:
        if (c->rec) f(*c->rec);
        else f(c->open);
        break;
    case Term::Selection: c->sel_term = MapFirst(c->sel_term, f); break;
    case Term::Parenthesized: f(c->open); break;
    }
    return c;
}
ParamP MapFirst(const ParamP &p, const HdrFn &f) {
    auto c = std::make_shared<Param>(*p);
    switch (c->kind) {
    case Param::Id: f(c->name); break;
    case Param::Set: f(c->open); break;
    case Param::Context: c->first = MapFirst(c->first, f); break;
    }
    return c;
}
ExprP MapFirst(const ExprP &e, const HdrFn &f) {
    auto c = std::make_shared<Expr>(*e);
    switch (c->kind) {
    case Expr::Term: c->term = MapFirst(c->term, f); break;
    case Expr::With:
    case Expr::Let:
    case Expr::Assert:
    case Expr::If:
    case Expr::Negation:
    case Expr::Inversion: f(c->k1); break;
    case Expr::Abstraction: c->param = MapFirst(c->param, f); break;
    case Expr::Application:
    case Expr::Operation:
    case Expr::MemberCheck: c->a = MapFirst(c->a, f); break;
    }
    return c;
}

namespace {
TermP MapLastTerm(const TermP &t, const HdrFn &f);
const AnnHdr &LastTokTerm(const Term &t);
}  // namespace

ExprP MapLast(const ExprP &e, const HdrFn &f) {
    auto c = std::make_shared<Expr>(*e);
    switch (c->kind) {
    case Expr::Term: c->term = MapLastTerm(c->term, f); break;
    case Expr::With:
    case Expr::Assert:
    case Expr::Application:
    case Expr::Operation: c->b = MapLast(c->b, f); break;
    case Expr::Let:
    case Expr::Abstraction:
    case Expr::Negation:
    case Expr::Inversion: c->a = MapLast(c->a, f); break;
    case Expr::If: c->c = MapLast(c->c, f); break;
    case Expr::MemberCheck:
        if (c->sels.empty()) f(c->k1);
        else f(SimpleHdr(c->sels.back().sel));
        break;
    }
    return c;
}
const AnnHdr &LastTok(const Expr &e) {
    switch (e.kind) {
    case Expr::Term: return LastTokTerm(*e.term);
    case Expr::With:
    case Expr::Assert:
    case Expr::Application:
    case Expr::Operation: return LastTok(*e.b);
    case Expr::Let:
    case Expr::Abstraction:
    case Expr::Negation:
    case Expr::Inversion: return LastTok(*e.a);
    case Expr::If: return LastTok(*e.c);
    case Expr::MemberCheck: return e.sels.empty() ? static_cast<const AnnHdr &>(e.k1) : SimpleHdr(e.sels.back().sel);
    }
    return e.k1;
}

namespace {
TermP MapLastTerm(const TermP &t, const HdrFn &f) {
    auto c = std::make_shared<Term>(*t);
    switch (c->kind) {
    case Term::Token: f(c->leaf); break;
    case Term::SimpleString:
    case Term::IndentedString: f(c->str); break;
    case Term::Path: f(c->path); break;
    case Term::List:
    case Term::Set:
    case Term::Parenthesized: f(c->close); break;
    case Term::Selection:
        if (c->or_def) c->or_def = MapLastTerm(c->or_def, f);
        else if (!c->sels.empty()) f(SimpleHdr(c->sels.back().sel));
        else c->sel_term = MapLastTerm(c->sel_term, f);
        break;
    }
    return c;
}
const AnnHdr &LastTokTerm(const Term &t) {
    switch (t.kind) {
    case Term::Token: return t.leaf;
    case Term::SimpleString:
    case Term::IndentedString: return t.str;
    case Term::Path: return t.path;
    case Term::List:
    case Term::Set:
    case Term::Parenthesized: return t.close;
    case Term::Selection:
        if (t.or_def) return LastTokTerm(*t.or_def);
        if (!t.sels.empty()) return SimpleHdr(t.sels.back().sel);
        return LastTokTerm(*t.sel_term);
    }
    return t.leaf;
}
}  // namespace

bool Equal(const Whole &a, const Whole &b) { return Eq(a.e, b.e); }

int MaxDepth(const Whole &w) {
    struct Frame {
        const Expr *e;
        const Term *t;
        const Param *p;
        int d;
    };
    std::vector<Frame> st;
    auto expr = [&](const ExprP &e, int d) {
        if (e) st.push_back({e.get(), nullptr, nullptr, d});
    };
    auto term = [&](const TermP &t, int d) {
        if (t) st.push_back({nullptr, t.get(), nullptr, d});
    };
    auto param = [&](const ParamP &p, int d) {
        if (p) st.push_back({nullptr, nullptr, p.get(), d});
    };
    auto line = [&](const StrLine &l, int d) {
        for (const auto &part : l) {
            if (part.interp) expr(part.whole->e, d);
        }
    };
    auto simple = [&](const SimpleSel &s, int d) {
        if (s.kind == SimpleSel::Interp && s.interp.part.interp) expr(s.interp.part.whole->e, d);
        if (s.kind == SimpleSel::Str) {
            for (const auto &l : s.str.lines) line(l, d);
        }
    };
    auto sels = [&](const std::vector<Selector> &v, int d) {
        for (const auto &s : v) simple(s.sel, d);
    };
    auto binders = [&](const std::vector<Item<Binder>> &items, int d) {
        for (const auto &i : items) {
            if (i.comments) continue;
            term(i.value.from, d);
            expr(i.value.rhs, d);
            sels(i.value.sels, d);
            for (const auto &s : i.value.ids) simple(s, d);
        }
    };
    int max = 0;
    expr(w.e, 1);
    while (!st.empty()) {
        const Frame f = st.back();
        st.pop_back();
        max = std::max(max, f.d);
        const int d = f.d + 1;
        if (f.e) {
            term(f.e->term, d);
            expr(f.e->a, d);
            expr(f.e->b, d);
            expr(f.e->c, d);
            param(f.e->param, d);
            binders(f.e->binders, d);
            sels(f.e->sels, d);
        } else if (f.t) {
            for (const auto &i : f.t->list_items) {
                if (!i.comments) term(i.value, d);
            }
            binders(f.t->set_items, d);
            term(f.t->sel_term, d);
            term(f.t->or_def, d);
            expr(f.t->paren, d);
            for (const auto &l : f.t->str.lines) line(l, d);
            line(f.t->path.parts, d);
            sels(f.t->sels, d);
        } else {
            for (const auto &a : f.p->attrs) expr(a.def, d);
            param(f.p->first, d);
            param(f.p->second, d);
        }
    }
    return max;
}

void CommentWords(const Whole &w, std::vector<std::string> &out) {
    WordCollector c{out};
    c.WholeW(w);
}

ParseResult ParseFile(const std::string &src, bool strict) {
    ParseResult r;
    Reader rd(src, strict);
    if (rd.File(r.file)) {
        r.ok = true;
        return r;
    }
    if (rd.too_deep_) {
        r.error = "nested too deeply to format";
        r.error_line = 0;
        return r;
    }
    const size_t at = std::min(rd.far_, src.size());
    r.error_line = rd.Line(at);
    if (at >= src.size()) {
        r.error = "syntax error: unexpected end of input";
    } else {
        size_t e = at;
        while (e < src.size() && e - at < 12 && src[e] != '\n') e++;
        r.error = "syntax error near '" + src.substr(at, e - at) + "'";
    }
    return r;
}

}  // namespace nixfmt::detail
