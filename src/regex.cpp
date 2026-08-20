#include "regex.h"

#include <algorithm>
#include <cctype>
#include <functional>

namespace mep_regex {

namespace {

// --- UTF-8 helpers (regex.h documents this module as dependency-free, so
// these are standalone rather than reusing e.g. src/json.h's own). Decodes
// one codepoint starting at `pos`, advances `pos` past it. Invalid/
// truncated sequences decode as a single raw byte (never throws, never
// under-advances) so a regex over slightly-malformed UTF-8 still
// terminates instead of looping.
int32_t DecodeUtf8(const std::string &s, size_t &pos) {
    unsigned char c = s[pos];
    int len = 1;
    int32_t cp = c;
    if ((c & 0x80) == 0) {
        len = 1;
        cp = c;
    } else if ((c & 0xE0) == 0xC0) {
        len = 2;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        len = 4;
        cp = c & 0x07;
    } else {
        pos += 1;
        return c;  // stray continuation/invalid lead byte -- treat as raw
    }
    if (pos + static_cast<size_t>(len) > s.size()) {
        pos += 1;
        return c;  // truncated sequence
    }
    for (int i = 1; i < len; i++) {
        unsigned char cc = s[pos + i];
        if ((cc & 0xC0) != 0x80) {
            pos += 1;
            return c;  // malformed continuation
        }
        cp = (cp << 6) | (cc & 0x3F);
    }
    pos += len;
    return cp;
}

// Currently unused by this file's own logic (replacement expansion just
// copies substrings of the original UTF-8 text, never re-encodes a bare
// codepoint) -- kept as the natural counterpart to DecodeUtf8 above for
// future consumers (e.g. a \u case-conversion replacement escape).
[[maybe_unused]] void EncodeUtf8(int32_t cp, std::string *out) {
    if (cp < 0x80) {
        out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

int32_t FoldCase(int32_t cp) {
    // ASCII-only case folding -- matches this codebase's existing
    // ASCII-only assumption elsewhere (e.g. DispatchNormalKey's register
    // names), and full Unicode case-folding tables are real additional
    // surface with no consumer needing them yet.
    if (cp >= 'A' && cp <= 'Z') return cp - 'A' + 'a';
    return cp;
}

bool IsWordChar(int32_t cp) {
    return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9') || cp == '_';
}
bool IsDigitChar(int32_t cp) { return cp >= '0' && cp <= '9'; }
bool IsSpaceChar(int32_t cp) {
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f' || cp == '\v';
}

}  // namespace

// --- AST -------------------------------------------------------------------

enum class Kind {
    Literal,
    AnyChar,
    CharClass,
    Concat,
    Alternate,
    Star,
    Plus,
    Quest,
    Repeat,
    Group,
    StartAnchor,
    EndAnchor,
    WordBoundary,
    NotWordBoundary,
};

// One [...] range or shorthand-class member.
struct ClassItem {
    bool is_shorthand = false;
    char shorthand = 0;  // 'd','w','s' (uppercase = negated) when is_shorthand
    int32_t lo = 0, hi = 0;  // inclusive codepoint range when !is_shorthand
};

struct Regex::Node {
    Kind kind;
    int32_t literal = 0;             // Literal
    std::vector<ClassItem> items;    // CharClass
    bool negated = false;            // CharClass's leading ^
    std::vector<std::unique_ptr<Node>> children;  // Concat/Alternate
    std::unique_ptr<Node> child;     // Star/Plus/Quest/Repeat/Group
    bool greedy = true;              // Star/Plus/Quest/Repeat
    int min_rep = 0, max_rep = -1;   // Repeat (-1 = unbounded)
    int group_index = 0;             // Group (0 = non-capturing)
};

using Node = Regex::Node;

namespace {

// --- Parser ------------------------------------------------------------

class Parser {
public:
    Parser(const std::string &pattern, bool ignore_case) : p_(pattern), ignore_case_(ignore_case) {}

    std::unique_ptr<Node> Parse(int *group_count, std::string *error) {
        auto node = ParseAlternation();
        if (!error_.empty()) {
            *error = error_;
            return nullptr;
        }
        if (pos_ != p_.size()) {
            *error = "unexpected '" + std::string(1, p_[pos_]) + "' at position " + std::to_string(pos_);
            return nullptr;
        }
        *group_count = next_group_;
        return node;
    }

private:
    const std::string &p_;
    size_t pos_ = 0;
    bool ignore_case_;
    int next_group_ = 1;
    std::string error_;

    bool Eof() const { return pos_ >= p_.size(); }
    char Peek() const { return Eof() ? '\0' : p_[pos_]; }
    char Get() { return p_[pos_++]; }
    bool Consume(char c) {
        if (Peek() == c) {
            pos_++;
            return true;
        }
        return false;
    }
    void Fail(const std::string &msg) {
        if (error_.empty()) error_ = msg + " (at position " + std::to_string(pos_) + ")";
    }

    std::unique_ptr<Node> MakeConcat(std::vector<std::unique_ptr<Node>> parts) {
        if (parts.size() == 1) return std::move(parts[0]);
        auto n = std::make_unique<Node>();
        n->kind = Kind::Concat;
        n->children = std::move(parts);
        return n;
    }

    std::unique_ptr<Node> ParseAlternation() {
        std::vector<std::unique_ptr<Node>> alts;
        alts.push_back(ParseConcat());
        while (!error_.empty() ? false : Consume('|')) {
            alts.push_back(ParseConcat());
        }
        if (alts.size() == 1) return std::move(alts[0]);
        auto n = std::make_unique<Node>();
        n->kind = Kind::Alternate;
        n->children = std::move(alts);
        return n;
    }

    std::unique_ptr<Node> ParseConcat() {
        std::vector<std::unique_ptr<Node>> parts;
        while (!Eof() && Peek() != '|' && Peek() != ')' && error_.empty()) {
            parts.push_back(ParseRepeat());
        }
        if (parts.empty()) {
            auto n = std::make_unique<Node>();
            n->kind = Kind::Concat;
            return n;  // empty alternative, e.g. "a||b"
        }
        return MakeConcat(std::move(parts));
    }

    std::unique_ptr<Node> ParseRepeat() {
        auto atom = ParseAtom();
        if (!error_.empty() || Eof()) return atom;
        char c = Peek();
        if (c == '*' || c == '+' || c == '?') {
            pos_++;
            auto n = std::make_unique<Node>();
            n->kind = c == '*' ? Kind::Star : (c == '+' ? Kind::Plus : Kind::Quest);
            n->child = std::move(atom);
            n->greedy = !Consume('?');
            return n;
        }
        if (c == '{') {
            size_t save = pos_;
            pos_++;
            int min_v = 0, max_v = -1;
            bool have_min = false;
            while (!Eof() && isdigit(static_cast<unsigned char>(Peek()))) {
                min_v = min_v * 10 + (Get() - '0');
                have_min = true;
            }
            bool have_comma = Consume(',');
            int max_digits_v = 0;
            bool have_max = false;
            if (have_comma) {
                while (!Eof() && isdigit(static_cast<unsigned char>(Peek()))) {
                    max_digits_v = max_digits_v * 10 + (Get() - '0');
                    have_max = true;
                }
            }
            if (!Consume('}') || (!have_min && !have_comma)) {
                // Not a valid {..} construct -- treat '{' as a literal,
                // matching common regex-flavor leniency (Vim/PCRE both do
                // this for a bare stray brace).
                pos_ = save;
                return atom;
            }
            max_v = have_comma ? (have_max ? max_digits_v : -1) : min_v;
            auto n = std::make_unique<Node>();
            n->kind = Kind::Repeat;
            n->child = std::move(atom);
            n->min_rep = min_v;
            n->max_rep = max_v;
            n->greedy = !Consume('?');
            if (max_v >= 0 && max_v < min_v) Fail("{n,m} with m < n");
            return n;
        }
        return atom;
    }

    std::unique_ptr<Node> ParseAtom() {
        if (Eof()) {
            Fail("unexpected end of pattern");
            return std::make_unique<Node>();
        }
        char c = Peek();
        if (c == '(') {
            pos_++;
            bool capturing = true;
            if (p_.compare(pos_, 2, "?:") == 0) {
                capturing = false;
                pos_ += 2;
            }
            auto n = std::make_unique<Node>();
            n->kind = Kind::Group;
            n->group_index = capturing ? next_group_++ : 0;
            n->child = ParseAlternation();
            if (!Consume(')')) Fail("missing closing ')'");
            return n;
        }
        if (c == '.') {
            pos_++;
            auto n = std::make_unique<Node>();
            n->kind = Kind::AnyChar;
            return n;
        }
        if (c == '^') {
            pos_++;
            auto n = std::make_unique<Node>();
            n->kind = Kind::StartAnchor;
            return n;
        }
        if (c == '$') {
            pos_++;
            auto n = std::make_unique<Node>();
            n->kind = Kind::EndAnchor;
            return n;
        }
        if (c == '[') {
            return ParseCharClass();
        }
        if (c == '\\') {
            pos_++;
            if (Eof()) {
                Fail("trailing backslash");
                return std::make_unique<Node>();
            }
            char e = Get();
            if (e == 'd' || e == 'D' || e == 'w' || e == 'W' || e == 's' || e == 'S') {
                auto n = std::make_unique<Node>();
                n->kind = Kind::CharClass;
                ClassItem it;
                it.is_shorthand = true;
                it.shorthand = e;
                n->items.push_back(it);
                return n;
            }
            if (e == 'b') {
                auto n = std::make_unique<Node>();
                n->kind = Kind::WordBoundary;
                return n;
            }
            if (e == 'B') {
                auto n = std::make_unique<Node>();
                n->kind = Kind::NotWordBoundary;
                return n;
            }
            if (e == 'n') return LiteralNode('\n');
            if (e == 't') return LiteralNode('\t');
            if (e == 'r') return LiteralNode('\r');
            // Any other escaped char (including regex metachars and '\\'
            // itself) is just that char, literally.
            return LiteralNode(static_cast<unsigned char>(e));
        }
        // Plain literal -- may be a multi-byte UTF-8 codepoint.
        size_t start = pos_;
        int32_t cp = DecodeUtf8(p_, pos_);
        (void)start;
        return LiteralNode(cp);
    }

    std::unique_ptr<Node> LiteralNode(int32_t cp) {
        auto n = std::make_unique<Node>();
        n->kind = Kind::Literal;
        n->literal = ignore_case_ ? FoldCase(cp) : cp;
        return n;
    }

    std::unique_ptr<Node> ParseCharClass() {
        pos_++;  // '['
        auto n = std::make_unique<Node>();
        n->kind = Kind::CharClass;
        if (Consume('^')) n->negated = true;
        bool first = true;
        while (!Eof() && (Peek() != ']' || first)) {
            first = false;
            int32_t lo;
            if (Peek() == '\\') {
                pos_++;
                if (Eof()) {
                    Fail("trailing backslash in class");
                    break;
                }
                char e = Get();
                if (e == 'd' || e == 'D' || e == 'w' || e == 'W' || e == 's' || e == 'S') {
                    ClassItem it;
                    it.is_shorthand = true;
                    it.shorthand = e;
                    n->items.push_back(it);
                    continue;
                }
                if (e == 'n') lo = '\n';
                else if (e == 't') lo = '\t';
                else if (e == 'r') lo = '\r';
                else lo = static_cast<unsigned char>(e);
            } else {
                lo = DecodeUtf8(p_, pos_);
            }
            int32_t hi = lo;
            if (Peek() == '-' && pos_ + 1 < p_.size() && p_[pos_ + 1] != ']') {
                pos_++;  // '-'
                if (Peek() == '\\') {
                    pos_++;
                    char e = Eof() ? '\0' : Get();
                    hi = (e == 'n') ? '\n' : (e == 't') ? '\t' : (e == 'r') ? '\r' : static_cast<unsigned char>(e);
                } else {
                    hi = DecodeUtf8(p_, pos_);
                }
            }
            if (ignore_case_) {
                lo = FoldCase(lo);
                hi = FoldCase(hi);
            }
            ClassItem it;
            it.lo = lo;
            it.hi = hi;
            n->items.push_back(it);
        }
        if (!Consume(']')) Fail("missing closing ']'");
        return n;
    }
};

// --- Matcher (continuation-passing backtracker) -------------------------
//
// Match(node, pos, k): tries to match `node` starting at byte offset
// `pos`; on success, calls the continuation k(new_pos) to match
// "everything after this node" and returns whatever k returns. If k
// returns false (the rest of the pattern couldn't complete), Match must
// try any other way *it* has of matching (e.g. a different repeat count)
// before giving up -- this is what makes backtracking work: failure
// propagates back through the call stack, not just a single boolean.
class Matcher {
public:
    Matcher(const std::string &text, bool ignore_case, int group_count)
        : text_(text), ignore_case_(ignore_case) {
        groups_.assign(group_count + 1, {-1, -1});
    }

    // Tries to match starting exactly at `start`. On success, groups_[0]
    // is set to {start, end} and groups_ holds every captured group;
    // returns the end offset, or -1 on failure.
    int MatchAt(const Node *root, int start) {
        int result = -1;
        Match(root, start, [&](int end) {
            result = end;
            return true;
        });
        if (result >= 0) groups_[0] = {start, result};
        return result;
    }

    std::vector<std::pair<int, int>> &groups() { return groups_; }

private:
    const std::string &text_;
    bool ignore_case_;
    std::vector<std::pair<int, int>> groups_;

    using Cont = std::function<bool(int)>;

    int32_t CodepointAt(int pos, int *next) const {
        size_t p = static_cast<size_t>(pos);
        int32_t cp = DecodeUtf8(text_, p);
        *next = static_cast<int>(p);
        return ignore_case_ ? FoldCase(cp) : cp;
    }

    bool ClassMatches(const Node *n, int32_t cp) const {
        bool any = false;
        for (const ClassItem &it : n->items) {
            if (it.is_shorthand) {
                bool m;
                char s = static_cast<char>(std::tolower(static_cast<unsigned char>(it.shorthand)));
                if (s == 'd') m = IsDigitChar(cp);
                else if (s == 'w') m = IsWordChar(cp);
                else m = IsSpaceChar(cp);
                if (std::isupper(static_cast<unsigned char>(it.shorthand))) m = !m;
                if (m) {
                    any = true;
                    break;
                }
            } else if (cp >= it.lo && cp <= it.hi) {
                any = true;
                break;
            }
        }
        return n->negated ? !any : any;
    }

    bool Match(const Node *n, int pos, const Cont &k) {
        switch (n->kind) {
            case Kind::Literal: {
                if (static_cast<size_t>(pos) >= text_.size()) return false;
                int next;
                int32_t cp = CodepointAt(pos, &next);
                if (cp != n->literal) return false;
                return k(next);
            }
            case Kind::AnyChar: {
                if (static_cast<size_t>(pos) >= text_.size()) return false;
                int next;
                int32_t cp = CodepointAt(pos, &next);
                if (cp == '\n') return false;
                return k(next);
            }
            case Kind::CharClass: {
                if (static_cast<size_t>(pos) >= text_.size()) return false;
                int next;
                int32_t cp = CodepointAt(pos, &next);
                if (!ClassMatches(n, cp)) return false;
                return k(next);
            }
            case Kind::StartAnchor:
                return pos == 0 && k(pos);
            case Kind::EndAnchor:
                return static_cast<size_t>(pos) == text_.size() && k(pos);
            case Kind::WordBoundary:
            case Kind::NotWordBoundary: {
                bool before = pos > 0 && IsWordChar(PrevCp(pos));
                bool after = static_cast<size_t>(pos) < text_.size() && IsWordChar(PeekCp(pos));
                bool boundary = before != after;
                if (n->kind == Kind::NotWordBoundary) boundary = !boundary;
                return boundary && k(pos);
            }
            case Kind::Concat:
                return MatchConcat(n->children, 0, pos, k);
            case Kind::Alternate: {
                for (const auto &alt : n->children) {
                    auto saved = groups_;
                    if (Match(alt.get(), pos, k)) return true;
                    groups_ = saved;
                }
                return false;
            }
            case Kind::Group: {
                int idx = n->group_index;
                return Match(n->child.get(), pos, [&, idx](int end) {
                    std::pair<int, int> saved = idx > 0 ? groups_[idx] : std::pair<int, int>{-1, -1};
                    if (idx > 0) groups_[idx] = {pos, end};
                    if (k(end)) return true;
                    if (idx > 0) groups_[idx] = saved;
                    return false;
                });
            }
            case Kind::Star:
                return MatchRepeat(n->child.get(), 0, -1, n->greedy, pos, k);
            case Kind::Plus:
                return MatchRepeat(n->child.get(), 1, -1, n->greedy, pos, k);
            case Kind::Quest:
                return MatchRepeat(n->child.get(), 0, 1, n->greedy, pos, k);
            case Kind::Repeat:
                return MatchRepeat(n->child.get(), n->min_rep, n->max_rep, n->greedy, pos, k);
        }
        return false;
    }

    int32_t PeekCp(int pos) const {
        size_t p = static_cast<size_t>(pos);
        return DecodeUtf8(text_, p);
    }
    int32_t PrevCp(int pos) const {
        // Step back one byte at a time until we're not mid-sequence, then
        // decode forward -- simple and adequate for boundary checks (word
        // chars are always ASCII in this engine, so a byte-accurate
        // "is the previous *codepoint* a word char" only ever cares about
        // plain ASCII bytes anyway).
        int p = pos - 1;
        while (p > 0 && (static_cast<unsigned char>(text_[p]) & 0xC0) == 0x80) p--;
        return PeekCp(p);
    }

    bool MatchConcat(const std::vector<std::unique_ptr<Node>> &parts, size_t i, int pos, const Cont &k) {
        if (i >= parts.size()) return k(pos);
        return Match(parts[i].get(), pos, [&, i](int next) { return MatchConcat(parts, i + 1, next, k); });
    }

    // Matches `child` repeated between `min_n` and `max_n` (-1 = unbounded)
    // times, greedy or lazy, via recursion: at each step, either (greedy)
    // try consuming one more repetition first and fall back to stopping,
    // or (lazy) try stopping first and fall back to consuming one more.
    bool MatchRepeat(const Node *child, int min_n, int max_n, bool greedy, int pos, const Cont &k) {
        return MatchRepeatN(child, 0, min_n, max_n, greedy, pos, k);
    }
    bool MatchRepeatN(const Node *child, int count, int min_n, int max_n, bool greedy, int pos, const Cont &k) {
        bool can_stop = count >= min_n;
        bool can_more = (max_n < 0 || count < max_n);
        auto try_more = [&]() {
            if (!can_more) return false;
            auto saved = groups_;
            bool matched = Match(child, pos, [&](int next) {
                if (next == pos && count >= min_n) return false;  // guard: empty match can't repeat forever
                return MatchRepeatN(child, count + 1, min_n, max_n, greedy, next, k);
            });
            if (!matched) groups_ = saved;
            return matched;
        };
        auto try_stop = [&]() { return can_stop && k(pos); };
        if (greedy) {
            if (try_more()) return true;
            return try_stop();
        }
        if (try_stop()) return true;
        return try_more();
    }
};

}  // namespace

Regex::Regex(const std::string &pattern, bool ignore_case) : ignore_case_(ignore_case) {
    Parser parser(pattern, ignore_case);
    root_ = parser.Parse(&group_count_, &error_);
    ok_ = (root_ != nullptr) && error_.empty();
}

Regex::~Regex() = default;
Regex::Regex(Regex &&) noexcept = default;
Regex &Regex::operator=(Regex &&) noexcept = default;

Match Regex::Search(const std::string &text, int from) const {
    Match result;
    if (!ok_ || !root_) return result;
    for (int start = std::max(0, from); start <= static_cast<int>(text.size()); start++) {
        Matcher m(text, ignore_case_, group_count_);
        int end = m.MatchAt(root_.get(), start);
        if (end >= 0) {
            result.start = start;
            result.end = end;
            result.groups = m.groups();
            return result;
        }
        // Advance by one *codepoint*, not one byte, so we don't retry
        // mid-sequence on multi-byte UTF-8 text.
        size_t p = static_cast<size_t>(start);
        if (p < text.size()) {
            DecodeUtf8(text, p);
            start = static_cast<int>(p) - 1;  // loop's ++ brings it to p
        }
    }
    return result;
}

bool Regex::FullMatch(const std::string &text) const {
    Match m = Search(text, 0);
    return m.ok() && m.start == 0 && static_cast<size_t>(m.end) == text.size();
}

std::string Regex::ExpandReplacement(const std::string &text, const Match &m, const std::string &repl) {
    std::string out;
    for (size_t i = 0; i < repl.size(); i++) {
        char c = repl[i];
        if ((c == '\\' || c == '&') && i + (c == '&' ? 0 : 1) <= repl.size()) {
            if (c == '&') {
                out += text.substr(m.start, m.end - m.start);
                continue;
            }
            char next = (i + 1 < repl.size()) ? repl[i + 1] : '\0';
            if (next >= '0' && next <= '9') {
                int idx = next - '0';
                if (idx < static_cast<int>(m.groups.size()) && m.groups[idx].first >= 0) {
                    const auto &g = m.groups[idx];
                    out += text.substr(g.first, g.second - g.first);
                }
                i++;
                continue;
            }
            if (next == '&' || next == '\\') {
                out += next;
                i++;
                continue;
            }
        }
        out += c;
    }
    return out;
}

std::string Regex::ReplaceFirst(const std::string &text, const std::string &repl) const {
    Match m = Search(text, 0);
    if (!m.ok()) return text;
    return text.substr(0, m.start) + ExpandReplacement(text, m, repl) + text.substr(m.end);
}

std::string Regex::ReplaceAll(const std::string &text, const std::string &repl) const {
    std::string out;
    int pos = 0;
    while (pos <= static_cast<int>(text.size())) {
        Match m = Search(text, pos);
        if (!m.ok()) {
            out += text.substr(pos);
            break;
        }
        out += text.substr(pos, m.start - pos);
        out += ExpandReplacement(text, m, repl);
        if (m.end == m.start) {
            // Empty match (e.g. pattern "a*" against "bbb") -- copy one
            // codepoint forward to guarantee progress, matching how
            // sed/Vim's own :s handle a zero-width match mid-replace.
            if (static_cast<size_t>(m.end) < text.size()) {
                size_t p = static_cast<size_t>(m.end);
                size_t before = p;
                DecodeUtf8(text, p);
                out += text.substr(before, p - before);
                pos = static_cast<int>(p);
            } else {
                pos = m.end + 1;
            }
        } else {
            pos = m.end;
        }
    }
    return out;
}

}  // namespace mep_regex
