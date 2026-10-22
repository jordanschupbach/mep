#include "bash_format.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "bash_parse.h"

// The layout rules are listed in bash_format.h. The printer walks the tree
// in source order; what it prints between the tree's tokens (spaces, `;`,
// line breaks) is its own, what it prints for a token is the source's, so
// the check at the end only has to compare trees.

namespace bashfmt {
namespace {

struct Pos {
    int l = 0;
    int c = 0;
};
bool operator<(Pos a, Pos b) { return a.l != b.l ? a.l < b.l : a.c < b.c; }
Pos Start(const BashRange &r) { return Pos{r.line, r.col}; }
Pos End(const BashRange &r) { return Pos{r.end_line, r.end_col}; }

std::string RTrim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    return s;
}

// --- Arithmetic --------------------------------------------------------------

struct ArithTok {
    enum Kind { Operand, Op, Open, Close, Comma } kind = Operand;
    std::string text;
};

// Skips a quoted or bracketed run starting at s[i] (the opener); returns
// the index after its closer, or npos.
size_t SkipBalanced(std::string_view s, size_t i, char open, char close) {
    int depth = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (c == '\\') {
            i += 2;
            continue;
        }
        if (c == '\'' && open != '\'') {
            const size_t q = s.find('\'', i + 1);
            if (q == std::string_view::npos) return q;
            i = q + 1;
            continue;
        }
        if (c == '"' && open != '"') {
            const size_t q = SkipBalanced(s, i, '"', '"');
            if (q == std::string_view::npos) return q;
            i = q;
            continue;
        }
        if (open == close) {
            if (c == close && depth > 0) return i + 1;
            depth = 1;
            i++;
            continue;
        }
        if (c == open) depth++;
        if (c == close && --depth == 0) return i + 1;
        i++;
    }
    return std::string_view::npos;
}

bool IsNameStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool IsNameChar(char c) { return IsNameStart(c) || (c >= '0' && c <= '9'); }

bool ArithLex(std::string_view s, std::vector<ArithTok> *out) {
    static const char *const ops[] = {"<<=", ">>=", "**", "++", "--", "<=", ">=", "==", "!=", "&&", "||", "+=", "-=",
                                      "*=",  "/=",  "%=", "&=", "^=", "|=", "<<", ">>", "+",  "-",  "*",  "/",  "%",
                                      "<",   ">",   "=",  "&",  "^",  "|",  "!",  "~",  "?",  ":"};
    size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n') {
            i++;
            continue;
        }
        if (c == '\\' && i + 1 < s.size() && s[i + 1] == '\n') {
            i += 2;
            continue;
        }
        if (c == '(') {
            out->push_back({ArithTok::Open, "("});
            i++;
            continue;
        }
        if (c == ')') {
            out->push_back({ArithTok::Close, ")"});
            i++;
            continue;
        }
        if (c == ',') {
            out->push_back({ArithTok::Comma, ","});
            i++;
            continue;
        }
        // An operand: a number, a name (with a subscript), an expansion, a
        // quoted string -- and what is glued to it (`$x[1]`, `${a}b`).
        const size_t a = i;
        bool any = false;
        for (;;) {
            if (i >= s.size()) break;
            const char d = s[i];
            if (d == '$') {
                if (i + 1 < s.size() && s[i + 1] == '{') {
                    i = SkipBalanced(s, i + 1, '{', '}');
                } else if (i + 1 < s.size() && s[i + 1] == '(') {
                    i = SkipBalanced(s, i + 1, '(', ')');
                } else if (i + 1 < s.size() && IsNameStart(s[i + 1])) {
                    i++;
                    while (i < s.size() && IsNameChar(s[i])) i++;
                } else if (i + 1 < s.size() && std::string_view("0123456789#?@*!$-_").find(s[i + 1]) != std::string_view::npos) {
                    i += 2;
                } else {
                    return false;
                }
                if (i == std::string_view::npos) return false;
                any = true;
                continue;
            }
            if (d == '"') {
                i = SkipBalanced(s, i, '"', '"');
                if (i == std::string_view::npos) return false;
                any = true;
                continue;
            }
            if (d == '\'') {
                const size_t q = s.find('\'', i + 1);
                if (q == std::string_view::npos) return false;
                i = q + 1;
                any = true;
                continue;
            }
            if (d == '`') {
                const size_t q = SkipBalanced(s, i, '`', '`');
                if (q == std::string_view::npos) return false;
                i = q;
                any = true;
                continue;
            }
            if (IsNameChar(d) || d == '#' || d == '@' || d == '.') {
                i++;
                any = true;
                continue;
            }
            if (d == '[' && any) {
                i = SkipBalanced(s, i, '[', ']');
                if (i == std::string_view::npos) return false;
                continue;
            }
            break;
        }
        if (any) {
            out->push_back({ArithTok::Operand, std::string(s.substr(a, i - a))});
            continue;
        }
        bool matched = false;
        for (const char *op : ops) {
            const std::string_view o(op);
            if (s.compare(i, o.size(), o) == 0) {
                out->push_back({ArithTok::Op, std::string(o)});
                i += o.size();
                matched = true;
                break;
            }
        }
        if (!matched) return false;
    }
    return true;
}

std::string Collapse(std::string_view s) {
    std::string out;
    bool space = false;
    for (size_t i = 0; i < s.size(); i++) {
        const char c = s[i];
        if (c == '\\' && i + 1 < s.size() && s[i + 1] == '\n') {
            i++;
            space = true;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n') {
            space = true;
            continue;
        }
        if (space && !out.empty()) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

// Binary operators spaced, unary ones tight, `,` followed by a space.
std::string ArithFormat(std::string_view s) {
    std::vector<ArithTok> toks;
    if (!ArithLex(s, &toks)) return Collapse(s);
    enum Prev { None, Operand, Postfix, Binary, Unary, Open, Close, Comma } prev = None;
    std::string out;
    for (const ArithTok &t : toks) {
        switch (t.kind) {
            case ArithTok::Open:
                out += '(';
                prev = Open;
                break;
            case ArithTok::Close:
                out += ')';
                prev = Close;
                break;
            case ArithTok::Comma:
                out += ", ";
                prev = Comma;
                break;
            case ArithTok::Operand:
                out += t.text;
                prev = Operand;
                break;
            case ArithTok::Op: {
                const bool after_value = prev == Operand || prev == Close || prev == Postfix;
                if (t.text == "++" || t.text == "--") {
                    // `- --x` must not print as `---x`
                    if (!after_value && !out.empty() && (out.back() == '+' || out.back() == '-')) out += ' ';
                    out += t.text;
                    prev = after_value ? Postfix : Unary;
                } else if (!after_value && (t.text == "+" || t.text == "-" || t.text == "!" || t.text == "~")) {
                    if (!out.empty() && (out.back() == '+' || out.back() == '-') && (t.text == "+" || t.text == "-")) out += ' ';
                    out += t.text;
                    prev = Unary;
                } else {
                    out += ' ';
                    out += t.text;
                    out += ' ';
                    prev = Binary;
                }
                break;
            }
        }
    }
    std::string trimmed = RTrim(out);
    size_t a = 0;
    while (a < trimmed.size() && trimmed[a] == ' ') a++;
    return trimmed.substr(a);
}

std::string ArithSig(std::string_view s) {
    std::vector<ArithTok> toks;
    if (!ArithLex(s, &toks)) return "?" + Collapse(s);
    std::string out;
    for (const ArithTok &t : toks) {
        out += t.text;
        out += '\x01';
    }
    return out;
}

// --- The source --------------------------------------------------------------

struct Source {
    std::vector<std::string> lines;
    std::vector<size_t> starts;
    std::string text;

    explicit Source(std::string_view src) : text(src) {
        size_t p = 0;
        for (;;) {
            starts.push_back(p);
            const size_t nl = text.find('\n', p);
            if (nl == std::string::npos) {
                lines.push_back(text.substr(p));
                break;
            }
            lines.push_back(text.substr(p, nl - p));
            p = nl + 1;
        }
        // A trailing newline ends the last line rather than starting another.
        if (lines.size() > 1 && lines.back().empty()) {
            lines.pop_back();
            starts.pop_back();
        }
    }
    size_t Off(Pos p) const {
        if (p.l < 0) return 0;
        if (static_cast<size_t>(p.l) >= starts.size()) return text.size();
        return std::min(text.size(), starts[static_cast<size_t>(p.l)] + static_cast<size_t>(std::max(0, p.c)));
    }
    std::string Slice(Pos a, Pos b) const {
        const size_t x = Off(a), y = Off(b);
        return y > x ? text.substr(x, y - x) : std::string();
    }
};

// The command inside backquotes, as `$(...)` would hold it: within
// backquotes `\$`, `\`` and `\\` stand for the character after the
// backslash, and every other backslash is itself.
std::string UnescapeBackquoted(std::string_view inner) {
    std::string out;
    for (size_t i = 0; i < inner.size(); i++) {
        if (inner[i] == '\\' && i + 1 < inner.size() &&
            (inner[i + 1] == '$' || inner[i + 1] == '`' || inner[i + 1] == '\\')) {
            out += inner[++i];
            continue;
        }
        out += inner[i];
    }
    return out;
}

std::vector<std::string> SplitLines(const std::string &text);

// --- Comparing trees ---------------------------------------------------------

// A word's text with its command substitutions, arithmetic and process
// substitutions replaced by their structure, so a re-laid-out `$(...)`
// compares equal and anything else changed does not.
class Signer {
public:
    explicit Signer(const Source &src) : src_(src) {}

    std::string List(const BashList &l) {
        std::string out = "L{";
        for (const BashAndOr &ao : l.items) {
            out += "A{";
            for (size_t k = 0; k < ao.pipelines.size(); k++) {
                if (k > 0) out += ao.ops[k - 1];
                Pipeline(ao.pipelines[k], &out);
            }
            out += ao.terminator == "&" ? "&}" : ";}";
        }
        return out + "}";
    }

    std::string Word(const BashWord &w) { return Span(Start(w.range), End(w.range), w.parts); }

private:
    const Source &src_;

    void Pipeline(const BashPipeline &pl, std::string *out) {
        *out += "P{";
        if (!pl.commands.empty()) *out += Collapse(src_.Slice(Start(pl.range), Start(pl.commands[0].range)));
        *out += "|";
        for (size_t k = 0; k < pl.commands.size(); k++) {
            if (k > 0) *out += pl.ops[k - 1];
            Command(pl.commands[k], out);
        }
        *out += "}";
    }

    void Redirect(const BashRedirect &r, std::string *out) {
        *out += "R{" + r.fd + r.op;
        if (r.heredoc) {
            *out += r.target.text + "\x02";
            for (int l = r.body_range.line; l <= r.body_range.end_line && l < static_cast<int>(src_.lines.size()); l++) {
                std::string line = src_.lines[static_cast<size_t>(l)];
                if (r.op == "<<-") line.erase(0, std::min(line.size(), line.find_first_not_of('\t')));
                *out += line + "\n";
            }
        } else {
            *out += Word(r.target);
        }
        *out += "}";
    }

    void Assignment(const BashAssignment &a, std::string *out) {
        *out += "S{" + a.name + (a.has_subscript ? "[" + a.subscript + "]" : "") + (a.append ? "+=" : "=");
        if (a.is_array) {
            *out += "(";
            for (const BashWord &e : a.elements) *out += Word(e) + "\x03";
            *out += ")";
        } else {
            *out += Word(a.value);
        }
        *out += a.declaration ? "d}" : "}";
    }

    void Command(const BashCommand &c, std::string *out) {
        *out += "C" + std::to_string(static_cast<int>(c.kind)) + "{";
        for (const BashAssignment &a : c.assignments) Assignment(a, out);
        for (const BashWord &w : c.words) {
            // A declaration's argument is compared as its assignment.
            bool decl = false;
            for (const BashAssignment &a : c.assignments) {
                if (a.declaration && Start(a.range).l == w.range.line && Start(a.range).c == w.range.col) decl = true;
            }
            *out += decl ? std::string("W{=}") : "W{" + Word(w) + "}";
        }
        for (const BashRedirect &r : c.redirects) Redirect(r, out);
        for (const auto &b : c.bodies) *out += b ? List(*b) : std::string("L{}");
        if (c.has_else) *out += "else";
        if (c.kind == BashCommandKind::For || c.kind == BashCommandKind::Select) {
            *out += c.var + (c.has_in ? " in " : " ");
            for (const BashWord &w : c.items) *out += Word(w) + "\x03";
        }
        for (const BashArith &a : c.ariths) *out += "#" + ArithSig(a.text);
        if (c.kind == BashCommandKind::Case) {
            *out += Word(c.subject);
            for (const BashCaseItem &ci : c.case_items) {
                *out += "I{";
                for (const BashWord &p : ci.patterns) *out += Word(p) + "\x03";
                *out += ci.body ? List(*ci.body) : std::string("L{}");
                *out += (ci.terminator.empty() ? std::string(";;") : ci.terminator) + "}";
            }
        }
        if (c.kind == BashCommandKind::Function || c.kind == BashCommandKind::Coproc) {
            *out += c.name + (c.function_keyword ? "!" : "");
        }
        for (const BashCommand &s : c.sub) Command(s, out);
        for (const BashCondTerm &t : c.cond) *out += "T" + std::to_string(static_cast<int>(t.kind)) + Word(t.word);
        *out += "}";
    }

    std::string Part(const BashWordPart &p) {
        switch (p.kind) {
            case BashPartKind::CommandSub:
            case BashPartKind::ProcessSub:
                if (p.backquoted && p.text.size() >= 2 && p.text.find('\\') != std::string::npos) {
                    // Compared as the command it stands for, so that it
                    // equals the `$(...)` it may have become.
                    const std::string inner = UnescapeBackquoted(std::string_view(p.text).substr(1, p.text.size() - 2));
                    const Source sub(inner);
                    const BashParseResult pr = BashParse(SplitLines(inner));
                    if (pr.has_syntax_error) return p.text;
                    return "$(" + Signer(sub).List(pr.program) + ")";
                }
                if (p.body) {
                    return (p.kind == BashPartKind::ProcessSub ? p.text.substr(0, 2) : std::string("$(")) + List(*p.body) + ")";
                }
                return p.text;
            case BashPartKind::ArithSub: return "$((" + ArithSig(p.arith.text) + "))";
            case BashPartKind::DoubleQuoted:
            case BashPartKind::Locale: return Span(Start(p.range), End(p.range), p.parts);
            case BashPartKind::Param: {
                std::vector<BashWordPart> kids;
                for (const BashWord &w : p.args) kids.insert(kids.end(), w.parts.begin(), w.parts.end());
                return Span(Start(p.range), End(p.range), kids);
            }
            default: return p.text;
        }
    }

    std::string Span(Pos from, Pos to, const std::vector<BashWordPart> &parts) {
        std::string out;
        Pos cur = from;
        for (const BashWordPart &p : parts) {
            if (Start(p.range) < cur || to < End(p.range)) return src_.Slice(from, to);
            out += src_.Slice(cur, Start(p.range));
            out += Part(p);
            cur = End(p.range);
        }
        out += src_.Slice(cur, to);
        return out;
    }
};

std::vector<BashComment> Comments(const BashParseResult &pr) {
    std::vector<BashComment> cs = pr.comments;
    std::sort(cs.begin(), cs.end(),
              [](const BashComment &a, const BashComment &b) { return Start(a.range) < Start(b.range); });
    std::vector<BashComment> out;
    for (const BashComment &c : cs) {
        if (!out.empty() && out.back().range.line == c.range.line && out.back().range.col == c.range.col) continue;
        out.push_back(c);
    }
    return out;
}

// --- The printer -------------------------------------------------------------

class Printer {
public:
    Printer(const Source &src, const BashParseResult &pr, std::string unit, bool case_indent)
        : src_(src), unit_(std::move(unit)), case_indent_(case_indent), comments_(Comments(pr)) {
        blank_.assign(src_.lines.size(), false);
        for (size_t l = 0; l < src_.lines.size(); l++) {
            blank_[l] = src_.lines[l].find_first_not_of(" \t\r") == std::string::npos;
        }
        for (const BashRange &r : pr.heredoc_bodies) {
            for (int l = r.line; l <= r.end_line && l < static_cast<int>(blank_.size()); l++) {
                if (l >= 0) blank_[static_cast<size_t>(l)] = false;
            }
        }
    }

    std::string Run(const BashList &program) {
        Statements(program, Pos{static_cast<int>(src_.lines.size()), 0});
        if (!AtLineStart()) Newline();
        while (!out_.empty() && out_.back().empty() && !raw_.back()) {
            out_.pop_back();
            raw_.pop_back();
        }
        return Join();
    }

private:
    const Source &src_;
    std::string unit_;
    bool case_indent_;
    std::vector<BashComment> comments_;
    size_t next_comment_ = 0;
    std::vector<bool> blank_;

    std::vector<std::string> out_;
    std::vector<bool> raw_;  // a here-document line: never touched
    std::string cur_;
    int indent_ = 0;
    // A line a statement continues onto (after `&&`, `||`, `|`) is one level
    // in from where the statement starts -- that line only: a block opened
    // on it is indented from the statement's own level, as shfmt does.
    int next_line_indent_ = -1;  // the indent of the next line started, if not indent_
    bool in_rhs_ = false;        // printing a continued right-hand side: no deeper for nested breaks
    Pos last_tok_{-1, 0};
    int last_src_line_ = -1;
    struct Heredoc {
        int first;
        int last;
        bool strip_tabs;  // <<-
        int indent;       // the level of the line the operator is on
    };
    int line_indent_ = 0;
    std::vector<Heredoc> pending_;

    // --- output -------------------------------------------------------------

    bool AtLineStart() const { return cur_.empty(); }

    void Emit(const std::string &s) {
        if (s.empty()) return;
        if (cur_.empty()) {
            const int level = next_line_indent_ >= 0 ? next_line_indent_ : indent_;
            next_line_indent_ = -1;
            for (int k = 0; k < level; k++) cur_ += unit_;
            line_indent_ = level;
        }
        cur_ += s;
    }

    void Tok(Pos end) {
        last_tok_ = end;
        last_src_line_ = std::max(last_src_line_, end.l);
    }

    void Kw(const BashRange &r) {
        Emit(src_.Slice(Start(r), End(r)));
        Tok(End(r));
    }

    void Newline() {
        out_.push_back(cur_);
        raw_.push_back(false);
        cur_.clear();
        for (const Heredoc &h : pending_) {
            // The tabs every body line starts with; deeper ones stay deeper.
            size_t common = std::string::npos;
            for (int l = h.first; l < h.last && l < static_cast<int>(src_.lines.size()); l++) {
                const std::string &t = src_.lines[static_cast<size_t>(l)];
                const size_t f = t.find_first_not_of('\t');
                if (f == std::string::npos) continue;  // blank, or only tabs
                common = std::min(common, f);
            }
            if (common == std::string::npos) common = 0;
            for (int l = h.first; l <= h.last && l < static_cast<int>(src_.lines.size()); l++) {
                std::string line = src_.lines[static_cast<size_t>(l)];
                // bash strips a <<- body's leading tabs, so with tabs for
                // indentation the body can go one level in and the
                // delimiter level with the command, as shfmt puts them.
                if (h.strip_tabs && unit_ == "\t") {
                    const size_t f = line.find_first_not_of('\t');
                    line = f == std::string::npos ? std::string() : line.substr(l < h.last ? std::min(f, common) : f);
                    if (!line.empty()) line = std::string(static_cast<size_t>(h.indent + (l < h.last ? 1 : 0)), '\t') + line;
                }
                out_.push_back(line);
                raw_.push_back(true);
            }
            last_src_line_ = std::max(last_src_line_, h.last);
        }
        pending_.clear();
    }

    bool BlankBetween(int a, int b) const {
        for (int l = std::max(a + 1, 0); l < b && l < static_cast<int>(blank_.size()); l++) {
            if (blank_[static_cast<size_t>(l)]) return true;
        }
        return false;
    }

    // Puts what comes next (at source position `p`) on a line of its own,
    // keeping one blank line before it if the source had any.
    void StartOwnLine(Pos p) {
        if (!AtLineStart()) Newline();
        if (last_src_line_ >= 0 && BlankBetween(last_src_line_, p.l) && !out_.empty() &&
            !(out_.back().empty() && !raw_.back())) {
            out_.emplace_back();
            raw_.push_back(false);
        }
    }

    // Prints `f` on trial: kept if it stayed on the current line, else
    // undone (returns false) so the caller can print a block instead. A
    // statement written on one line can still not fit on one, when it holds
    // a `case` or a block of several statements.
    bool Try(const std::function<void()> &f) {
        const size_t lines = out_.size();
        const std::string cur = cur_;
        const int indent = indent_;
        const int next_indent = next_line_indent_;
        const int line_indent = line_indent_;
        const bool rhs = in_rhs_;
        const size_t comment = next_comment_;
        const std::vector<Heredoc> pending = pending_;
        const Pos tok = last_tok_;
        const int src_line = last_src_line_;
        f();
        if (out_.size() == lines) return true;
        out_.resize(lines);
        raw_.resize(lines);
        cur_ = cur;
        indent_ = indent;
        next_line_indent_ = next_indent;
        line_indent_ = line_indent;
        in_rhs_ = rhs;
        next_comment_ = comment;
        pending_ = pending;
        last_tok_ = tok;
        last_src_line_ = src_line;
        return false;
    }

    // The indent of the line what is printed next is on.
    int LineIndentHere() const {
        if (!AtLineStart()) return line_indent_;
        return next_line_indent_ >= 0 ? next_line_indent_ : indent_;
    }

    // Every comment before `p` not yet printed: after the code on its line,
    // or on a line of its own at the current indent.
    void CommentsBefore(Pos p) {
        while (next_comment_ < comments_.size() && Start(comments_[next_comment_].range) < p) {
            const BashComment &c = comments_[next_comment_++];
            const std::string text = RTrim(c.text);
            if (!AtLineStart() && c.range.line == last_tok_.l) {
                cur_ += '\x01';
                cur_ += text;
                Newline();
            } else {
                StartOwnLine(Start(c.range));
                Emit(text);
                Newline();
            }
            last_tok_ = End(c.range);
            last_src_line_ = std::max(last_src_line_, c.range.line);
        }
    }

    bool CommentBetween(Pos a, Pos b) const {
        for (size_t k = next_comment_; k < comments_.size(); k++) {
            const Pos s = Start(comments_[k].range);
            if (!(s < b)) break;
            if (!(s < a)) return true;
        }
        return false;
    }

    // --- words --------------------------------------------------------------

    void Word(const BashWord &w) {
        Span(Start(w.range), End(w.range), w.parts);
        Tok(End(w.range));
    }

    void Span(Pos from, Pos to, const std::vector<BashWordPart> &parts) {
        Pos cur = from;
        for (const BashWordPart &p : parts) {
            if (Start(p.range) < cur || to < End(p.range)) {
                Emit(src_.Slice(cur, to));
                return;
            }
            Emit(src_.Slice(cur, Start(p.range)));
            Part(p);
            cur = End(p.range);
        }
        Emit(src_.Slice(cur, to));
    }

    static bool StartsWithParen(const BashList &b) {
        if (b.items.empty() || b.items[0].pipelines.empty()) return false;
        const BashPipeline &pl = b.items[0].pipelines[0];
        if (pl.negated || pl.timed || pl.commands.empty()) return false;
        const BashCommandKind k = pl.commands[0].kind;
        return k == BashCommandKind::Subshell || k == BashCommandKind::Arith;
    }

    void Part(const BashWordPart &p) {
        switch (p.kind) {
            case BashPartKind::CommandSub:
            case BashPartKind::ProcessSub: {
                // `$` + backquotes as `$$(` would be $$ and a `(`.
                const size_t at = src_.Off(Start(p.range));
                const bool after_dollar = at > 0 && src_.text[at - 1] == '$';
                const bool escapes = p.backquoted && p.text.find('\\') != std::string::npos;
                const bool convertible = p.body && (!p.backquoted || (!escapes && !after_dollar));
                if (escapes && !after_dollar && p.text.size() >= 2) {
                    // With escapes: the command they stand for, as written.
                    // Not inside double quotes with a `\"` in it, whose
                    // meaning in backquotes is not what it is in `$(...)`.
                    const std::string inner = UnescapeBackquoted(std::string_view(p.text).substr(1, p.text.size() - 2));
                    // Nor with backquotes inside (`a \`b\``): converting the outer
                    // only would leave the inner for a second run to convert.
                    if (!(p.quoted && p.text.find("\\\"") != std::string::npos) && inner.find('`') == std::string::npos &&
                        !BashParse(SplitLines(inner)).has_syntax_error) {
                        // Laid out as `$(...)`'s body would be, on one line.
                        Options o;
                        o.indent = unit_;
                        o.default_indent = unit_;
                        o.case_indent = case_indent_ ? 1 : 0;
                        const Result r = Format(inner, o);
                        std::string t = r.text;
                        if (!t.empty() && t.back() == '\n') t.pop_back();
                        if (r.ok && !t.empty() && t.find('\n') == std::string::npos) {
                            Emit((t[0] == '(' ? "$( " : "$(") + t + ")");
                            Tok(End(p.range));
                            return;
                        }
                    }
                }
                if (!convertible) {
                    Emit(src_.Slice(Start(p.range), End(p.range)));
                    Tok(End(p.range));
                    return;
                }
                const std::string open = p.kind == BashPartKind::ProcessSub ? p.text.substr(0, 2) : "$(";
                Pos close = End(p.range);
                close.c = std::max(0, close.c - 1);
                Emit(open);
                Tok(Start(p.range));
                Parenthesized(*p.body, Start(p.range), close);
                Emit(")");
                Tok(End(p.range));
                return;
            }
            case BashPartKind::ArithSub:
                Emit("$((" + ArithFormat(p.arith.text) + "))");
                Tok(End(p.range));
                return;
            case BashPartKind::DoubleQuoted:
            case BashPartKind::Locale:
                Span(Start(p.range), End(p.range), p.parts);
                return;
            case BashPartKind::Param: {
                std::vector<BashWordPart> kids;
                for (const BashWord &w : p.args) kids.insert(kids.end(), w.parts.begin(), w.parts.end());
                Span(Start(p.range), End(p.range), kids);
                return;
            }
            default: Emit(src_.Slice(Start(p.range), End(p.range))); return;
        }
    }

    // The inside of `( )`, `$( )`, `<( )`: on the opener's line when it
    // was one statement there, else a block.
    void Parenthesized(const BashList &body, Pos open, Pos close) {
        if (body.items.empty()) {
            CommentsBefore(close);
            return;
        }
        // One statement hugs the parentheses even when a block inside it
        // spreads over lines (`$(a || (` ... `))`): it still starts on the
        // opener's line and ends on the closer's, so this holds next time.
        if (Hugs(body, open, close)) {
            if (StartsWithParen(body)) Emit(" ");
            InlineItems(body, false);
            return;
        }
        indent_++;
        Statements(body, close);
        indent_--;
        StartOwnLine(close);
    }

    // One statement, starting on the opener's line and ending on the
    // closer's, with no comment in it: printed between them as it is.
    bool Hugs(const BashList &body, Pos open, Pos close) const {
        if (body.items.size() != 1) return false;
        const BashAndOr &ao = body.items[0];
        return ao.range.line == open.l && ao.range.end_line == close.l && !CommentBetween(open, close);
    }

    // --- statements ---------------------------------------------------------

    void Statements(const BashList &l, Pos end) {
        // A block's statements continue lines afresh.
        const bool rhs = in_rhs_;
        in_rhs_ = false;
        struct Restore {
            bool &flag;
            bool value;
            ~Restore() { flag = value; }
        } restore{in_rhs_, rhs};
        for (const BashAndOr &ao : l.items) {
            CommentsBefore(Start(ao.range));
            StartOwnLine(Start(ao.range));
            AndOr(ao);
            if (ao.terminator == "&") Emit(" &");
        }
        CommentsBefore(end);
    }

    // Statements on one line: `a; b &`; with `final_sep` a `;` after the
    // last, for a closing keyword to follow.
    void InlineItems(const BashList &l, bool final_sep) {
        for (size_t k = 0; k < l.items.size(); k++) {
            const BashAndOr &ao = l.items[k];
            if (k > 0) {
                CommentsBefore(Start(ao.range));
                if (!AtLineStart()) Emit(" ");
            }
            AndOr(ao);
            const bool amp = ao.terminator == "&";
            if (k + 1 < l.items.size()) {
                Emit(amp ? " &" : ";");
            } else if (amp) {
                Emit(" &");
            } else if (final_sep) {
                Emit(";");
            }
        }
    }

    // `a &&` newline `b`: shfmt reads `a && b || c` as a tree and prints a
    // right-hand side that starts on a new line one level in -- while it
    // prints that side only. So a block opened there (`a &&` newline `(`)
    // is indented from the continuation, and one opened after an operator
    // with no break (`b || {`) from the statement.
    void AndOr(const BashAndOr &ao) {
        const int base = indent_;
        const bool rhs = in_rhs_;
        for (size_t k = 0; k < ao.pipelines.size(); k++) {
            if (k > 0) {
                Emit(" " + ao.ops[k - 1]);
                const bool brk = Break(ao.pipelines[k - 1].range, ao.pipelines[k].range, base);
                indent_ = brk && !rhs ? base + 1 : base;
                in_rhs_ = rhs || brk;
            }
            Pipeline(ao.pipelines[k]);
            in_rhs_ = rhs;
        }
        indent_ = base;
    }

    // After `&&`, `||` or `|`: a new line if the source had one (or a
    // comment ended the line). Returns whether it broke.
    bool Break(const BashRange &prev, const BashRange &next, int base) {
        const bool brk = next.line > prev.end_line;
        if (brk) indent_ = in_rhs_ ? base : base + 1;  // own-line comments in between go there too
        CommentsBefore(Start(next));
        if (brk || AtLineStart()) {
            if (!AtLineStart()) Newline();
            return true;
        }
        Emit(" ");
        return false;
    }

    void Pipeline(const BashPipeline &pl) {
        if (pl.commands.empty()) {  // a bare `time` or `!`
            Emit(Collapse(src_.Slice(Start(pl.range), End(pl.range))));
            Tok(End(pl.range));
            return;
        }
        const std::string prefix = Collapse(src_.Slice(Start(pl.range), Start(pl.commands[0].range)));
        if (!prefix.empty()) Emit(prefix + " ");
        const int base = indent_;
        const bool rhs = in_rhs_;
        for (size_t k = 0; k < pl.commands.size(); k++) {
            if (k > 0) {
                Emit(" " + pl.ops[k - 1]);
                const bool brk = Break(pl.commands[k - 1].range, pl.commands[k].range, base);
                indent_ = brk && !rhs ? base + 1 : base;
                in_rhs_ = rhs || brk;
            }
            Command(pl.commands[k]);
            in_rhs_ = rhs;
        }
        indent_ = base;
    }

    void Redirect(const BashRedirect &r) {
        Emit(r.fd + r.op);
        if (r.heredoc) {
            Emit(r.target.text);
            Tok(End(r.target.range));
            pending_.push_back(Heredoc{r.body_range.line, r.body_range.end_line, r.op == "<<-", line_indent_});
            return;
        }
        // `< <(cmd)` must not become `<<(cmd)`.
        if (r.target.text.size() > 1 && (r.target.text[0] == '<' || r.target.text[0] == '>') && r.target.text[1] == '(') {
            Emit(" ");
        }
        Word(r.target);
    }

    void Assignment(const BashAssignment &a) {
        const Pos vs = Start(a.value.range);
        Emit(src_.Slice(Start(a.range), vs));
        if (!a.is_array) {
            Word(a.value);
            Tok(End(a.range));
            return;
        }
        // name=( elements ): breaks where the source broke, the elements of
        // a continued line one level in.
        Emit("(");
        Tok(Pos{vs.l, vs.c + 1});
        Pos close = End(a.range);
        close.c = std::max(0, close.c - 1);
        const int save_indent = indent_;
        const int base = line_indent_;
        bool broke = false;
        Pos prev = vs;
        for (size_t k = 0; k < a.elements.size(); k++) {
            const BashWord &e = a.elements[k];
            CommentsBefore(Start(e.range));
            if (e.range.line > prev.l || AtLineStart()) {
                if (!broke) {
                    indent_ = base + 1;
                    broke = true;
                }
                if (!AtLineStart()) Newline();
            } else if (k > 0) {
                Emit(" ");
            }
            Word(e);
            prev = End(e.range);
        }
        CommentsBefore(close);
        indent_ = base;
        if (close.l > prev.l || AtLineStart()) {
            if (!AtLineStart()) Newline();
        }
        Emit(")");
        indent_ = save_indent;
        Tok(End(a.range));
    }

    void Simple(const BashCommand &c) {
        // Assignments, then the words, then the redirections (shfmt's order;
        // where a redirection sits among the words does not matter, only
        // its order among the other redirections).
        struct El {
            Pos at;
            int kind;  // 0 assignment, 1 word, 2 redirect
            size_t index;
        };
        std::vector<El> els;
        for (size_t k = 0; k < c.assignments.size(); k++) {
            if (!c.assignments[k].declaration) els.push_back({Start(c.assignments[k].range), 0, k});
        }
        for (size_t k = 0; k < c.words.size(); k++) els.push_back({Start(c.words[k].range), 1, k});
        for (size_t k = 0; k < c.redirects.size(); k++) els.push_back({Start(c.redirects[k].range), 2, k});
        // shfmt's order: what comes before the command name stays there; a
        // redirection right after the name stays after it (a here-document
        // aside); the other redirections go after the last word.
        // Redirections keep their order among themselves (`2>&1 >f` is not
        // `>f 2>&1`): once one is moved to the end, so is every later one.
        std::vector<int> redirect_rank(c.redirects.size(), 4);
        if (!c.words.empty()) {
            bool deferred = false;
            for (size_t k = 0; k < c.redirects.size(); k++) {
                const Pos at = Start(c.redirects[k].range);
                if (at < Start(c.words[0].range)) {
                    redirect_rank[k] = 0;
                    continue;
                }
                const bool after_name = c.words.size() == 1 || at < Start(c.words[1].range);
                if (c.redirects[k].heredoc || !after_name) deferred = true;
                redirect_rank[k] = deferred ? 4 : 2;
            }
        }
        auto rank = [&](const El &e) {
            if (c.words.empty()) return 0;
            if (e.kind == 0 || e.at < Start(c.words[0].range)) return 0;
            if (e.kind == 1) return e.index == 0 ? 1 : 3;
            return redirect_rank[e.index];
        };
        std::stable_sort(els.begin(), els.end(), [&](const El &a, const El &b) {
            const int ra = rank(a), rb = rank(b);
            return ra != rb ? ra < rb : a.at < b.at;
        });
        // A line broken with a backslash: the rest of the command one level
        // in from where the command starts.
        const int save_indent = indent_;
        const int base = LineIndentHere();
        int prev_line = -1;
        for (size_t k = 0; k < els.size(); k++) {
            const El &e = els[k];
            if (k > 0) {
                if (e.at.l > prev_line) {
                    Emit(" \\");
                    Newline();
                    indent_ = base + 1;
                } else {
                    Emit(" ");
                }
            }
            if (e.kind == 0) {
                Assignment(c.assignments[e.index]);
                prev_line = c.assignments[e.index].range.end_line;
            } else if (e.kind == 1) {
                const BashWord &w = c.words[e.index];
                const BashAssignment *decl = nullptr;
                for (const BashAssignment &a : c.assignments) {
                    if (a.declaration && a.range.line == w.range.line && a.range.col == w.range.col) decl = &a;
                }
                if (decl) {
                    Assignment(*decl);
                } else {
                    Word(w);
                }
                prev_line = w.range.end_line;
            } else {
                const BashRedirect &r = c.redirects[e.index];
                Redirect(r);
                prev_line = r.heredoc ? r.target.range.end_line : r.range.end_line;
            }
        }
        indent_ = save_indent;
    }

    // A keyword that follows a list on the same line: `; then`, or `then`
    // at the start of a line a comment ended.
    void After(const BashRange &kw, bool amp) {
        CommentsBefore(Start(kw));
        if (AtLineStart()) {
            StartOwnLine(Start(kw));
        } else {
            Emit(amp ? " " : "; ");
        }
        Kw(kw);
    }

    static bool LastAmp(const BashList &l) { return !l.items.empty() && l.items.back().terminator == "&"; }

    // The condition of if/elif/while/until, after its keyword: on the
    // keyword's line when it was written on one line, else a block.
    void Condition(const BashList &cond, const BashRange &kw, const BashRange &next) {
        // Inline when it starts on the keyword's line and every statement in
        // it does (the last may run on, `if a &&` newline `b; then`).
        bool one_line = !cond.items.empty() && cond.items.front().range.line == kw.end_line &&
                        cond.items.back().range.line == kw.end_line;
        for (size_t k = 0; one_line && k + 1 < cond.items.size(); k++) {
            if (cond.items[k].range.end_line != kw.end_line) one_line = false;
        }
        // A comment inside it would end the line early.
        if (one_line && CommentBetween(End(kw), Start(cond.items.back().range))) one_line = false;
        if (one_line) {
            Emit(" ");
            InlineItems(cond, false);
            // `if x # why` newline `then`: the comment goes after `then`.
            if (next_comment_ + 1 <= comments_.size() && next_comment_ < comments_.size()) {
                const BashComment &cm = comments_[next_comment_];
                const bool only = next_comment_ + 1 == comments_.size() ||
                                  !(Start(comments_[next_comment_ + 1].range) < Start(next));
                if (Start(cm.range) < Start(next) && cm.range.line == last_tok_.l && next.line > cm.range.line && only &&
                    !LastAmp(cond)) {
                    next_comment_++;
                    Emit("; ");
                    Kw(next);
                    cur_ += '\x01';
                    cur_ += RTrim(cm.text);
                    Newline();
                    last_src_line_ = std::max(last_src_line_, next.end_line);
                    return;
                }
            }
            After(next, LastAmp(cond));
            return;
        }
        Block(cond, next);
        StartOwnLine(Start(next));
        Kw(next);
    }

    void Block(const BashList &body, const BashRange &closer) {
        indent_++;
        Statements(body, Start(closer));
        indent_--;
    }

    // The keyword that ends a block body (fi, done, }, else, ...).
    void Closer(const BashRange &kw) {
        StartOwnLine(Start(kw));
        Kw(kw);
    }

    bool OneLine(const BashCommand &c, size_t from_kw, size_t to_kw) const {
        const BashRange &a = c.keywords[from_kw], &b = c.keywords[to_kw];
        return a.line == b.end_line && !CommentBetween(Start(a), End(b));
    }

    static bool AtMostOne(const std::shared_ptr<BashList> &b) { return !b || b->items.size() <= 1; }

    void If(const BashCommand &c) {
        const auto &kw = c.keywords;
        bool inline_all = kw.size() >= 3 && OneLine(c, 0, kw.size() - 1);
        for (size_t k = 0; k < c.bodies.size(); k++) {
            const bool is_cond = k % 2 == 0 && !(c.has_else && k + 1 == c.bodies.size());
            if (!is_cond && !AtMostOne(c.bodies[k])) inline_all = false;
            if (!c.bodies[k] || (!is_cond && c.bodies[k]->items.empty())) inline_all = false;
        }
        if (inline_all && Try([&] { IfLayout(c, true); })) return;
        IfLayout(c, false);
    }

    void IfLayout(const BashCommand &c, bool inline_all) {
        const auto &kw = c.keywords;
        Kw(kw[0]);
        size_t ki = 0, b = 0;
        while (b < c.bodies.size()) {
            const bool else_body = c.has_else && b + 1 == c.bodies.size();
            if (else_body) {
                // kw[ki] is `else`
                Body(*c.bodies[b], kw[ki + 1], inline_all);
                b++;
                ki++;
                continue;
            }
            // kw[ki] is if/elif, kw[ki + 1] then
            Condition(*c.bodies[b], kw[ki], kw[ki + 1]);
            Body(*c.bodies[b + 1], kw[ki + 2], inline_all);
            b += 2;
            ki += 2;
        }
        (void)ki;
    }

    // A then/else/do body, then the keyword after it.
    void Body(const BashList &body, const BashRange &next, bool inline_body) {
        if (inline_body) {
            Emit(" ");
            InlineItems(body, true);
            CommentsBefore(Start(next));
            Emit(" ");
            Kw(next);
            return;
        }
        Block(body, next);
        Closer(next);
    }

    // do ... done (or { ... }) of the loops.
    void DoGroup(const BashCommand &c, const std::shared_ptr<BashList> &body, size_t do_kw, bool amp) {
        const BashRange &open = c.keywords[do_kw];
        const BashRange &close = c.keywords[do_kw + 1];
        After(open, amp);
        if (!body) return;
        const bool inline_body = OneLine(c, do_kw, do_kw + 1) && body->items.size() == 1;
        if (inline_body && !AtLineStart() && Try([&] { Body(*body, close, true); })) return;
        Body(*body, close, false);
    }

    void Loop(const BashCommand &c) {
        const auto &kw = c.keywords;
        Kw(kw[0]);
        if (c.kind == BashCommandKind::While || c.kind == BashCommandKind::Until) {
            Condition(*c.bodies[0], kw[0], kw[1]);
            const bool inline_body = OneLine(c, 1, 2) && c.bodies[1]->items.size() == 1;
            if (inline_body && !AtLineStart() && Try([&] { Body(*c.bodies[1], kw[2], true); })) return;
            Body(*c.bodies[1], kw[2], false);
            return;
        }
        if (c.kind == BashCommandKind::ForArith) {
            std::string parts[3];
            for (size_t k = 0; k < 3 && k < c.ariths.size(); k++) parts[k] = ArithFormat(c.ariths[k].text);
            Emit(" ((" + (parts[0].empty() ? std::string(" ") : parts[0]) + ";" +
                 (parts[1].empty() ? std::string(" ") : " " + parts[1]) + ";" +
                 (parts[2].empty() ? std::string(" ") : " " + parts[2]) + "))");
            Tok(End(kw[1]));
            DoGroup(c, c.bodies.empty() ? nullptr : c.bodies[0], 2, false);
            return;
        }
        // for/select NAME [in WORDS]
        Emit(" " + c.var);
        Tok(End(c.var_range));
        size_t do_kw = 1;
        if (c.has_in) {
            Emit(" ");
            Kw(kw[1]);
            do_kw = 2;
            int prev_line = kw[1].end_line;
            const int save_indent = indent_;
            const int base = LineIndentHere();
            for (const BashWord &w : c.items) {
                if (w.range.line > prev_line) {
                    Emit(" \\");
                    Newline();
                    indent_ = base + 1;
                } else {
                    Emit(" ");
                }
                Word(w);
                prev_line = w.range.end_line;
            }
            indent_ = save_indent;
        }
        DoGroup(c, c.bodies.empty() ? nullptr : c.bodies[0], do_kw, false);
    }

    void Case(const BashCommand &c) {
        const auto &kw = c.keywords;  // case, in, esac
        // `case $x in a) b ;; esac`: one line, when it was written on one
        // with a statement at most in each item.
        bool one_line = kw.size() >= 3 && OneLine(c, 0, kw.size() - 1);
        for (const BashCaseItem &ci : c.case_items) {
            if (!AtMostOne(ci.body)) one_line = false;
        }
        if (one_line && Try([&] { CaseOneLine(c); })) return;
        CaseBlock(c);
    }

    void CaseOneLine(const BashCommand &c) {
        const auto &kw = c.keywords;
        Kw(kw[0]);
        Emit(" ");
        Word(c.subject);
        Emit(" ");
        Kw(kw[1]);
        for (const BashCaseItem &ci : c.case_items) {
            Emit(" ");
            if (!ci.patterns.empty() && ci.patterns[0].text == "esac") Emit("(");
            for (size_t p = 0; p < ci.patterns.size(); p++) {
                if (p > 0) Emit(" | ");
                Word(ci.patterns[p]);
            }
            Emit(")");
            if (ci.body && !ci.body->items.empty()) {
                Emit(" ");
                InlineItems(*ci.body, false);
            }
            Emit(" " + (ci.terminator.empty() ? std::string(";;") : ci.terminator));
        }
        Emit(" ");
        Kw(kw.back());
    }

    // Comments on lines of their own before a case item (or `esac`): with
    // the item's body when they were indented past it, else with the
    // patterns -- shfmt's reading of which one a comment belongs to.
    void CaseComments(Pos next) {
        const int at = SourceColumn(next);
        while (next_comment_ < comments_.size() && Start(comments_[next_comment_].range) < next) {
            const BashComment &cm = comments_[next_comment_];
            const bool own_line = AtLineStart() || cm.range.line != last_tok_.l;
            const int save = indent_;
            if (own_line && SourceColumn(Start(cm.range)) > at) indent_ = save + 1;
            CommentsBefore(Pos{cm.range.line, cm.range.col + 1});
            indent_ = save;
        }
    }

    int SourceColumn(Pos p) const {
        if (p.l < 0 || p.l >= static_cast<int>(src_.lines.size())) return 0;
        const std::string &line = src_.lines[static_cast<size_t>(p.l)];
        int w = 0;
        for (int k = 0; k < p.c && k < static_cast<int>(line.size()); k++) {
            w = line[static_cast<size_t>(k)] == '\t' ? w + 8 - w % 8 : w + 1;
        }
        return w;
    }

    void CaseBlock(const BashCommand &c) {
        const auto &kw = c.keywords;
        Kw(kw[0]);
        Emit(" ");
        Word(c.subject);
        Emit(" ");
        Kw(kw[1]);
        const int base = indent_;
        indent_ = base + (case_indent_ ? 1 : 0);
        for (size_t k = 0; k < c.case_items.size(); k++) {
            const BashCaseItem &ci = c.case_items[k];
            CaseComments(Start(ci.range));
            StartOwnLine(Start(ci.range));
            // `(esac)`: without its parenthesis the pattern reads as the keyword.
            if (!ci.patterns.empty() && ci.patterns[0].text == "esac") Emit("(");
            const int pattern_indent = indent_;
            for (size_t p = 0; p < ci.patterns.size(); p++) {
                if (p > 0) {
                    if (ci.patterns[p].range.line > ci.patterns[p - 1].range.end_line) {
                        Emit(" | \\");
                        Newline();
                        indent_ = pattern_indent + 1;
                    } else {
                        Emit(" | ");
                    }
                }
                Word(ci.patterns[p]);
            }
            indent_ = pattern_indent;
            Emit(")");
            const std::string term = ci.terminator.empty() ? ";;" : ci.terminator;
            const Pos item_end = ci.terminator.empty() ? Start(kw.back()) : End(ci.range);
            Pos term_pos = item_end;
            if (!ci.terminator.empty()) term_pos.c = std::max(0, term_pos.c - static_cast<int>(ci.terminator.size()));
            const int pattern_line = ci.patterns.empty() ? ci.range.line : ci.patterns.back().range.end_line;
            const bool empty = !ci.body || ci.body->items.empty();
            const bool one_line = (ci.terminator.empty() ? (empty || ci.body->range.end_line == pattern_line)
                                                         : ci.range.end_line == pattern_line) &&
                                  !CommentBetween(Start(ci.range), item_end) && AtMostOne(ci.body);
            if (empty && !CommentBetween(Start(ci.range), term_pos) &&
                (term_pos.l == pattern_line || ci.terminator.empty())) {
                Emit(" " + term);
                if (!ci.terminator.empty()) Tok(item_end);
                continue;
            }
            // `pat) cmd <<...` running over lines and ending at `;;`: hugs
            // the pattern, as one statement written that way.
            if (!empty && ci.body->items.size() == 1 && ci.body->items[0].range.line == pattern_line &&
                ci.body->items[0].range.end_line == term_pos.l && term_pos.l > pattern_line &&
                !CommentBetween(Start(ci.range), item_end) && !ci.terminator.empty()) {
                Emit(" ");
                InlineItems(*ci.body, false);
                Emit(" " + term);
                Tok(item_end);
                continue;
            }
            // Written on one line: stays there (and a block inside it that
            // spreads over lines leaves it hugging the pattern, which the
            // rule above keeps next time).
            if (one_line) {
                Emit(" ");
                InlineItems(*ci.body, false);
                Emit(" " + term);
                if (!ci.terminator.empty()) Tok(item_end);
                continue;
            }
            indent_++;
            if (ci.body) Statements(*ci.body, term_pos);
            StartOwnLine(term_pos);
            Emit(term);
            if (!ci.terminator.empty()) Tok(item_end);
            indent_--;
        }
        CaseComments(Start(kw.back()));
        indent_ = base;
        Closer(kw.back());
    }

    void Cond(const BashCommand &c) {
        const int cont = LineIndentHere() + 1;
        Kw(c.keywords[0]);
        const BashCondTerm *prev = nullptr;
        bool break_next = false;  // `a \` newline `|| b`: the break goes after the operator
        for (const BashCondTerm &t : c.cond) {
            const bool op = t.kind == BashCondTerm::And || t.kind == BashCondTerm::Or;
            bool brk = break_next || (prev && (prev->kind == BashCondTerm::And || prev->kind == BashCondTerm::Or) &&
                                      t.word.range.line > prev->word.range.end_line);
            break_next = false;
            if (op && prev && t.word.range.line > prev->word.range.end_line) {
                break_next = true;
                brk = false;
            }
            if (prev) CommentsBefore(Start(t.word.range));
            if (brk || AtLineStart()) {
                if (!AtLineStart()) Newline();
                next_line_indent_ = cont;
            } else if (!prev || (prev->kind != BashCondTerm::Open && t.kind != BashCondTerm::Close)) {
                Emit(" ");
            }
            if (t.kind == BashCondTerm::Word) {
                Word(t.word);
            } else {
                Emit(t.word.text);
                Tok(End(t.word.range));
            }
            prev = &t;
        }
        CommentsBefore(Start(c.keywords.back()));
        Emit(" ");
        Kw(c.keywords.back());
    }

    void Command(const BashCommand &c) {
        switch (c.kind) {
            case BashCommandKind::Simple: Simple(c); return;
            case BashCommandKind::Subshell: {
                Kw(c.keywords[0]);
                const BashRange &close = c.keywords.back();
                if (!c.bodies.empty() && c.bodies[0]) Parenthesized(*c.bodies[0], End(c.keywords[0]), Start(close));
                Kw(close);
                break;
            }
            case BashCommandKind::Group: {
                Kw(c.keywords[0]);
                const BashRange &close = c.keywords.back();
                const BashList &body = *c.bodies[0];
                auto f = [&] {
                    Emit(" ");
                    InlineItems(body, true);
                    Emit(" ");
                    Kw(close);
                };
                if (Hugs(body, Start(c.keywords[0]), Start(close))) {
                    f();
                } else {
                    Block(body, close);
                    Closer(close);
                }
                break;
            }
            case BashCommandKind::If: If(c); break;
            case BashCommandKind::While:
            case BashCommandKind::Until:
            case BashCommandKind::For:
            case BashCommandKind::Select:
            case BashCommandKind::ForArith: Loop(c); break;
            case BashCommandKind::Case: Case(c); break;
            case BashCommandKind::Function: {
                if (c.sub.empty()) return;
                const std::string between = src_.Slice(End(c.name_range), Start(c.sub[0].range));
                const bool parens = between.find('(') != std::string::npos;
                // `f@ ()` must not become the extglob `f@()`.
                const char last = c.name.empty() ? ' ' : c.name.back();
                const std::string paren = std::string("?*+@!").find(last) != std::string::npos ? " ()" : "()";
                Emit(c.function_keyword ? "function " + c.name + (parens ? paren : "") : c.name + paren);
                Tok(End(c.name_range));
                CommentsBefore(Start(c.sub[0].range));
                if (!AtLineStart()) Emit(" ");
                Command(c.sub[0]);
                break;
            }
            case BashCommandKind::Cond: Cond(c); break;
            case BashCommandKind::Arith:
                Emit("((" + (c.ariths.empty() ? std::string() : ArithFormat(c.ariths[0].text)) + "))");
                Tok(End(c.keywords.back()));
                break;
            case BashCommandKind::Coproc:
                Kw(c.keywords[0]);
                Emit(" ");
                if (!c.name.empty()) Emit(c.name + " ");
                if (!c.sub.empty()) Command(c.sub[0]);
                break;
        }
        // `( ... ) \` newline `>file`: the break stays.
        const int save_indent = indent_;
        const int base = LineIndentHere();
        for (const BashRedirect &r : c.redirects) {
            if (r.range.line > last_tok_.l && !AtLineStart()) {
                Emit(" \\");
                Newline();
                indent_ = base + 1;
            } else {
                Emit(" ");
            }
            Redirect(r);
        }
        indent_ = save_indent;
    }

    // --- trailing comments ---------------------------------------------------

    static int Width(const std::string &s) {
        int w = 0;
        for (const char c : s) {
            const auto ch = static_cast<unsigned char>(c);
            if (ch == '\t') {
                w += 8 - w % 8;
            } else if ((ch & 0xC0) != 0x80) {
                w++;
            }
        }
        return w;
    }

    std::string Join() {
        // Consecutive lines that end in a comment have their comments lined up.
        std::vector<std::string> lines = out_;
        size_t k = 0;
        while (k < lines.size()) {
            auto marked = [&](size_t i) {
                return !raw_[i] && lines[i].find('\x01') != std::string::npos && lines[i].find('\n') == std::string::npos;
            };
            if (!marked(k)) {
                k++;
                continue;
            }
            size_t e = k;
            while (e < lines.size() && marked(e)) e++;
            int width = 0;
            for (size_t i = k; i < e; i++) width = std::max(width, Width(lines[i].substr(0, lines[i].find('\x01'))));
            for (size_t i = k; i < e; i++) {
                const size_t m = lines[i].find('\x01');
                std::string code = lines[i].substr(0, m);
                const std::string comment = lines[i].substr(m + 1);
                if (code.find_first_not_of(" \t") == std::string::npos) {
                    lines[i] = code + comment;
                    continue;
                }
                code += std::string(static_cast<size_t>(width - Width(code) + 1), ' ');
                lines[i] = code + comment;
            }
            k = e;
        }
        std::string out;
        for (size_t i = 0; i < lines.size(); i++) {
            std::string l = lines[i];
            for (char &ch : l) {
                if (ch == '\x01' && !raw_[i]) ch = ' ';
            }
            out += l;
            out += '\n';
        }
        return out;
    }
};

// --- The file's conventions -----------------------------------------------

int Columns(const std::string &line) {
    int w = 0;
    for (char ch : line) {
        if (ch == ' ') {
            w++;
        } else if (ch == '\t') {
            w += 8 - w % 8;
        } else {
            break;
        }
    }
    return w;
}

// The indentation of the statements one level inside the top-level blocks
// (function bodies, loops, ifs): after formatting, exactly one unit, so
// reading it again gives the same unit.
void FirstLevel(const Source &src, const BashList &top, std::map<std::string, int> *units) {
    auto record = [&](const BashList &body) {
        for (const BashAndOr &ao : body.items) {
            if (ao.range.line < 0 || ao.range.line >= static_cast<int>(src.lines.size())) continue;
            const std::string &line = src.lines[static_cast<size_t>(ao.range.line)];
            const size_t f = line.find_first_not_of(" \t");
            if (f != static_cast<size_t>(ao.range.col) || f == 0 || f > 16) continue;
            const std::string lead = line.substr(0, f);
            if (lead.find_first_not_of('\t') == std::string::npos) {
                if (f == 1) (*units)["\t"]++;
            } else if (lead.find_first_not_of(' ') == std::string::npos) {
                (*units)[lead]++;
            }
        }
    };
    std::function<void(const BashCommand &)> command = [&](const BashCommand &c) {
        switch (c.kind) {
            case BashCommandKind::Function:
                for (const BashCommand &s : c.sub) {
                    for (const auto &b : s.bodies) {
                        if (b) record(*b);
                    }
                }
                break;
            case BashCommandKind::Case:
                // Its items when they are indented, else their bodies.
                for (const BashCaseItem &ci : c.case_items) {
                    const std::string &line = src.lines[static_cast<size_t>(ci.range.line)];
                    const size_t f = line.find_first_not_of(" \t");
                    if (f == static_cast<size_t>(ci.range.col) && f > 0) {
                        BashList one;
                        BashAndOr ao;
                        ao.range = ci.range;
                        one.items.push_back(ao);
                        record(one);
                    } else if (ci.body && !ci.body->items.empty() && ci.body->range.line > ci.range.line) {
                        record(*ci.body);
                    } else if (!ci.terminator.empty() && ci.range.end_line > ci.range.line) {
                        BashList one;  // `pat)` newline `;;`: the `;;` is a level in
                        BashAndOr ao;
                        ao.range.line = ao.range.end_line = ci.range.end_line;
                        ao.range.col = ci.range.end_col - static_cast<int>(ci.terminator.size());
                        one.items.push_back(ao);
                        record(one);
                    }
                }
                break;
            default:
                for (const auto &b : c.bodies) {
                    if (b) record(*b);
                }
        }
    };
    // Blocks opened on the first line of a statement that starts a line (a
    // block opened on a continuation line is two units in).
    for (const BashAndOr &ao : top.items) {
        if (ao.range.col != 0) continue;
        for (const BashPipeline &pl : ao.pipelines) {
            for (const BashCommand &c : pl.commands) {
                if (c.range.line == ao.range.line) command(c);
            }
        }
    }
}

// With no blocks to go by: lines continuing a top-level statement (after
// `&&`/`|`, or a backslash inside a command), also exactly one unit in.
void Continuations(const Source &src, const BashList &top, std::map<std::string, int> *units) {
    auto at = [&](const BashRange &r) {
        if (r.line < 0 || r.line >= static_cast<int>(src.lines.size())) return;
        const std::string &line = src.lines[static_cast<size_t>(r.line)];
        const size_t f = line.find_first_not_of(" \t");
        if (f != static_cast<size_t>(r.col) || f == 0 || f > 16) return;
        const std::string lead = line.substr(0, f);
        if (lead.find_first_not_of('\t') == std::string::npos) {
            if (f == 1) (*units)["\t"]++;
        } else if (lead.find_first_not_of(' ') == std::string::npos) {
            (*units)[lead]++;
        }
    };
    for (const BashAndOr &ao : top.items) {
        if (ao.range.col != 0) continue;
        for (size_t k = 0; k < ao.pipelines.size(); k++) {
            const BashPipeline &pl = ao.pipelines[k];
            if (k > 0 && pl.range.line > ao.pipelines[k - 1].range.end_line) at(pl.range);
            if (k == 0 && !pl.commands.empty() && pl.commands[0].kind == BashCommandKind::Simple) {
                const BashCommand &c = pl.commands[0];
                int prev = c.range.line;
                for (const BashWord &w : c.words) {
                    if (w.range.line > prev) at(w.range);
                    prev = w.range.end_line;
                }
            }
        }
    }
}

std::string DetectIndent(const Source &src, const BashParseResult &pr, const std::string &fallback) {
    std::map<std::string, int> units;
    FirstLevel(src, pr.program, &units);
    if (units.empty()) Continuations(src, pr.program, &units);
    if (!units.empty()) {
        std::string best;
        int count = 0;
        for (const auto &[u, n] : units) {
            if (n > count) {
                best = u;
                count = n;
            }
        }
        return best;
    }
    return fallback.empty() ? std::string("\t") : fallback;
}

class CaseCounter : public BashVisitor {
public:
    const Source &src;
    int indented = 0, flat = 0;
    explicit CaseCounter(const Source &s) : src(s) {}
    bool Command(const BashCommand &c, const BashCommand *) override {
        if (c.kind != BashCommandKind::Case || c.keywords.empty()) return true;
        const int base = Columns(src.lines[static_cast<size_t>(c.keywords[0].line)]);
        for (const BashCaseItem &ci : c.case_items) {
            const std::string &line = src.lines[static_cast<size_t>(ci.range.line)];
            if (line.find_first_not_of(" \t") != static_cast<size_t>(ci.range.col)) continue;  // not first on its line
            if (Columns(line) > base) {
                indented++;
            } else {
                flat++;
            }
        }
        return true;
    }
};

std::vector<std::string> SplitLines(const std::string &text) {
    std::vector<std::string> lines;
    size_t p = 0;
    for (;;) {
        const size_t nl = text.find('\n', p);
        if (nl == std::string::npos) {
            lines.push_back(text.substr(p));
            break;
        }
        lines.push_back(text.substr(p, nl - p));
        p = nl + 1;
    }
    if (lines.size() > 1 && lines.back().empty()) lines.pop_back();
    return lines;
}

}  // namespace

Result Format(std::string_view input, const Options &opt) {
    Result res;
    res.text = std::string(input);
    if (input.find('\x01') != std::string_view::npos || input.find('\r') != std::string_view::npos) {
        res.error = "the script has carriage returns or control characters; left as it is";
        return res;
    }
    const Source src(input);
    const BashParseResult pr = BashParse(src.lines);
    if (pr.has_syntax_error) {
        for (const BashParseError &e : pr.errors) {
            if (e.warning) continue;
            res.error = e.message;
            res.error_line = e.range.line + 1;
            break;
        }
        if (res.error.empty()) res.error = "syntax error";
        return res;
    }
    for (const BashParseError &e : pr.errors) {
        if (e.code == "unterminated-heredoc") {
            res.error = e.message;
            res.error_line = e.range.line + 1;
            return res;
        }
    }

    std::string unit = opt.indent.empty() ? DetectIndent(src, pr, opt.default_indent) : opt.indent;
    bool case_indent = opt.case_indent == 1;
    if (opt.case_indent < 0) {
        CaseCounter cc(src);
        BashWalk(pr.program, cc);
        case_indent = cc.indented > cc.flat;
    }

    Printer printer(src, pr, unit, case_indent);
    std::string out = printer.Run(pr.program);
    if (input.empty() || input.find_first_not_of(" \t\n") == std::string_view::npos) out = std::string(input);

    // The check: the output must read as the same script.
    const Source src2(out);
    const BashParseResult pr2 = BashParse(SplitLines(out));
    bool same = !pr2.has_syntax_error;
    int bad_line = 0;
    if (same) {
        Signer a(src), b(src2);
        const auto &ia = pr.program.items, &ib = pr2.program.items;
        if (ia.size() != ib.size()) {
            same = false;
        } else {
            for (size_t k = 0; k < ia.size(); k++) {
                BashList la, lb;
                la.items.push_back(ia[k]);
                lb.items.push_back(ib[k]);
                if (a.List(la) != b.List(lb)) {
                    same = false;
                    bad_line = ia[k].range.line + 1;
                    break;
                }
            }
        }
    }
    if (same) {
        const std::vector<BashComment> ca = Comments(pr), cb = Comments(pr2);
        if (ca.size() != cb.size()) {
            same = false;
        } else {
            for (size_t k = 0; k < ca.size(); k++) {
                if (RTrim(ca[k].text) != RTrim(cb[k].text)) {
                    same = false;
                    bad_line = ca[k].range.line + 1;
                    break;
                }
            }
        }
    }
    if (!same) {
        res.error = "could not lay this script out without changing what it does; left as it is";
        res.error_line = bad_line;
        if (opt.unchecked) res.text = out;
        return res;
    }
    res.ok = true;
    res.text = out;
    return res;
}

}  // namespace bashfmt
