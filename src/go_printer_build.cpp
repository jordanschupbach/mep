// go/build/constraint (the parts go/printer uses) and go/printer's
// gobuild.go: moving //go:build and // +build lines to the top of the
// file and keeping the two in sync. Copyright 2020 The Go Authors. All
// rights reserved. BSD-style license; the full notice is in go_parse.cpp.

#include <algorithm>
#include <memory>

#include "go_ast.h"
#include "go_doc_comment.h"
#include "go_printer_internal.h"
#include "go_tabwriter.h"

namespace goprint {

namespace {

using godoc::TrimSpace;

bool HasPrefix(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

struct Expr;
using ExprP = std::shared_ptr<Expr>;
struct Expr {
    enum Kind { Tag, Not, And, Or } kind;
    std::string tag;
    ExprP x, y;
};

ExprP MkTag(std::string t) { return std::make_shared<Expr>(Expr{Expr::Tag, std::move(t), nullptr, nullptr}); }
ExprP MkNot(ExprP x) { return std::make_shared<Expr>(Expr{Expr::Not, "", std::move(x), nullptr}); }
ExprP MkAnd(ExprP x, ExprP y) { return std::make_shared<Expr>(Expr{Expr::And, "", std::move(x), std::move(y)}); }
ExprP MkOr(ExprP x, ExprP y) { return std::make_shared<Expr>(Expr{Expr::Or, "", std::move(x), std::move(y)}); }

std::string String(const ExprP &e);
std::string AndArg(const ExprP &x) {
    std::string s = String(x);
    if (x->kind == Expr::Or) s = "(" + s + ")";
    return s;
}
std::string OrArg(const ExprP &x) {
    std::string s = String(x);
    if (x->kind == Expr::And) s = "(" + s + ")";
    return s;
}
std::string String(const ExprP &e) {
    switch (e->kind) {
    case Expr::Tag: return e->tag;
    case Expr::Not: {
        std::string s = String(e->x);
        if (e->x->kind == Expr::And || e->x->kind == Expr::Or) s = "(" + s + ")";
        return "!" + s;
    }
    case Expr::And: return AndArg(e->x) + " && " + AndArg(e->y);
    case Expr::Or: return OrArg(e->x) + " || " + OrArg(e->y);
    }
    return "";
}

bool SplitGoBuild(std::string_view line, std::string_view *expr) {
    if (!line.empty() && line.back() == '\n') line.remove_suffix(1);
    if (line.find('\n') != std::string_view::npos) return false;
    if (!HasPrefix(line, "//go:build")) return false;
    line = TrimSpace(line);
    line = line.substr(10);
    std::string_view trim = TrimSpace(line);
    if (line.size() == trim.size() && !line.empty()) return false;
    *expr = trim;
    return true;
}

bool SplitPlusBuild(std::string_view line, std::string_view *expr) {
    if (!line.empty() && line.back() == '\n') line.remove_suffix(1);
    if (line.find('\n') != std::string_view::npos) return false;
    if (!HasPrefix(line, "//")) return false;
    line = TrimSpace(line.substr(2));
    if (!HasPrefix(line, "+build")) return false;
    line = line.substr(6);
    std::string_view trim = TrimSpace(line);
    if (line.size() == trim.size() && !line.empty()) return false;
    *expr = trim;
    return true;
}

struct SyntaxError {};

constexpr int kMaxSize = 1000;

class ExprParser {
public:
    explicit ExprParser(std::string_view s) : s_(s) {}
    ExprP Parse() {
        ExprP x = Or();
        if (!tok_.empty()) throw SyntaxError{};
        return x;
    }

private:
    ExprP Or() {
        ExprP x = And();
        while (tok_ == "||") x = MkOr(x, And());
        return x;
    }
    ExprP And() {
        ExprP x = Not();
        while (tok_ == "&&") x = MkAnd(x, Not());
        return x;
    }
    ExprP Not() {
        if (++size_ > kMaxSize) throw SyntaxError{};
        Lex();
        if (tok_ == "!") {
            Lex();
            if (tok_ == "!") throw SyntaxError{};
            return MkNot(Atom());
        }
        return Atom();
    }
    ExprP Atom() {
        if (tok_ == "(") {
            ExprP x = Or();
            if (tok_ != ")") throw SyntaxError{};
            Lex();
            return x;
        }
        if (!is_tag_) throw SyntaxError{};
        std::string t(tok_);
        Lex();
        return MkTag(t);
    }
    void Lex() {
        is_tag_ = false;
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t')) i_++;
        if (i_ >= s_.size()) {
            tok_ = {};
            return;
        }
        switch (s_[i_]) {
        case '(': case ')': case '!':
            tok_ = s_.substr(i_, 1);
            i_++;
            return;
        case '&': case '|':
            if (i_ + 1 >= s_.size() || s_[i_ + 1] != s_[i_]) throw SyntaxError{};
            tok_ = s_.substr(i_, 2);
            i_ += 2;
            return;
        default: break;
        }
        size_t j = i_;
        while (j < s_.size()) {
            int w = 0;
            uint32_t c = goast::DecodeRune(s_, j, &w);
            if (!goast::UnicodeIsLetter(c) && !goast::UnicodeIsDigit(c) && c != '_' && c != '.') break;
            j += static_cast<size_t>(w);
        }
        if (j == i_) throw SyntaxError{};
        tok_ = s_.substr(i_, j - i_);
        i_ = j;
        is_tag_ = true;
    }

    std::string_view s_;
    size_t i_ = 0;
    std::string_view tok_;
    bool is_tag_ = false;
    int size_ = 0;
};

bool IsValidTag(std::string_view w) {
    if (w.empty()) return false;
    size_t j = 0;
    while (j < w.size()) {
        int n = 0;
        uint32_t c = goast::DecodeRune(w, j, &n);
        if (!goast::UnicodeIsLetter(c) && !goast::UnicodeIsDigit(c) && c != '_' && c != '.') return false;
        j += static_cast<size_t>(n);
    }
    return true;
}

// strings.Fields over Unicode white space.
std::vector<std::string_view> Fields(std::string_view s) {
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < s.size()) {
        int w = 0;
        uint32_t r = goast::DecodeRune(s, i, &w);
        if (goast::UnicodeIsSpace(r) && !(r == goast::RuneError && w == 1)) {
            i += static_cast<size_t>(w);
            continue;
        }
        size_t start = i;
        while (i < s.size()) {
            r = goast::DecodeRune(s, i, &w);
            if (goast::UnicodeIsSpace(r) && !(r == goast::RuneError && w == 1)) break;
            i += static_cast<size_t>(w);
        }
        out.push_back(s.substr(start, i - start));
    }
    return out;
}

ExprP ParsePlusBuildExpr(std::string_view text) {
    constexpr int kMaxOldSize = 100;
    int size = 0;
    ExprP x;
    for (std::string_view clause : Fields(text)) {
        ExprP y;
        size_t start = 0;
        while (true) {
            size_t comma = clause.find(',', start);
            std::string_view lit = clause.substr(start, comma == std::string_view::npos ? std::string_view::npos
                                                                                       : comma - start);
            ExprP z;
            if (HasPrefix(lit, "!!") || lit == "!") {
                z = MkTag("ignore");
            } else {
                bool neg = false;
                if (HasPrefix(lit, "!")) {
                    neg = true;
                    lit.remove_prefix(1);
                }
                z = IsValidTag(lit) ? MkTag(std::string(lit)) : MkTag("ignore");
                if (neg) z = MkNot(z);
            }
            if (!y) {
                y = z;
            } else {
                if (++size > kMaxOldSize) throw SyntaxError{};
                y = MkAnd(y, z);
            }
            if (comma == std::string_view::npos) break;
            start = comma + 1;
        }
        if (!x) {
            x = y;
        } else {
            if (++size > kMaxOldSize) throw SyntaxError{};
            x = MkOr(x, y);
        }
    }
    if (!x) x = MkTag("ignore");
    return x;
}

// constraint.Parse; nullptr on error.
ExprP Parse(std::string_view line) {
    try {
        std::string_view text;
        if (SplitGoBuild(line, &text)) return ExprParser(text).Parse();
        if (SplitPlusBuild(line, &text)) return ParsePlusBuildExpr(text);
    } catch (const SyntaxError &) {
    }
    return nullptr;
}

ExprP PushNot(const ExprP &x, bool no) {
    switch (x->kind) {
    case Expr::Not:
        if (x->x->kind == Expr::Tag && !no) return x;
        return PushNot(x->x, !no);
    case Expr::Tag: return no ? MkNot(x) : x;
    case Expr::And: {
        ExprP x1 = PushNot(x->x, no), y1 = PushNot(x->y, no);
        if (no) return MkOr(x1, y1);
        if (x1 == x->x && y1 == x->y) return x;
        return MkAnd(x1, y1);
    }
    case Expr::Or: {
        ExprP x1 = PushNot(x->x, no), y1 = PushNot(x->y, no);
        if (no) return MkAnd(x1, y1);
        if (x1 == x->x && y1 == x->y) return x;
        return MkOr(x1, y1);
    }
    }
    return x;
}

void SplitAnd(std::vector<ExprP> &list, const ExprP &x) {
    if (x->kind == Expr::And) {
        SplitAnd(list, x->x);
        SplitAnd(list, x->y);
        return;
    }
    list.push_back(x);
}
void SplitOr(std::vector<ExprP> &list, const ExprP &x) {
    if (x->kind == Expr::Or) {
        SplitOr(list, x->x);
        SplitOr(list, x->y);
        return;
    }
    list.push_back(x);
}

// constraint.PlusBuildLines; false = errComplex.
bool PlusBuildLines(ExprP x, std::vector<std::string> *lines) {
    x = PushNot(x, false);
    std::vector<std::vector<std::vector<ExprP>>> split;
    std::vector<ExprP> ors;
    SplitAnd(ors, x);
    for (const ExprP &o : ors) {
        std::vector<std::vector<ExprP>> ands;
        std::vector<ExprP> as;
        SplitOr(as, o);
        for (const ExprP &a : as) {
            std::vector<ExprP> lits;
            std::vector<ExprP> ls;
            SplitAnd(ls, a);
            for (const ExprP &lit : ls) {
                if (lit->kind != Expr::Tag && lit->kind != Expr::Not) return false;
                lits.push_back(lit);
            }
            ands.push_back(lits);
        }
        split.push_back(ands);
    }
    size_t max_or = 0;
    for (const auto &o : split) max_or = std::max(max_or, o.size());
    if (max_or == 1) {
        std::vector<ExprP> lits;
        for (const auto &o : split) lits.insert(lits.end(), o[0].begin(), o[0].end());
        split = {{lits}};
    }
    for (const auto &o : split) {
        std::string line = "// +build";
        for (const auto &a : o) {
            line += ' ';
            for (size_t i = 0; i < a.size(); i++) {
                if (i > 0) line += ',';
                line += String(a[i]);
            }
        }
        lines->push_back(line);
    }
    return true;
}

bool IsNL(char b) { return b == '\n' || b == '\f'; }

std::string_view LineAt(const std::string &out, size_t start) {
    size_t pos = start;
    while (pos < out.size() && !IsNL(out[pos])) pos++;
    if (pos < out.size()) pos++;
    return std::string_view(out).substr(start, pos - start);
}

std::string CommentTextAt(const std::string &out, size_t start) {
    if (start < out.size() && out[start] == gotab::Escape) start++;
    size_t pos = start;
    while (pos < out.size() && out[pos] != gotab::Escape && !IsNL(out[pos])) pos++;
    return out.substr(start, pos - start);
}

void AppendLines(std::string &x, std::string_view y) {
    if (!y.empty() && IsNL(y[0]) && (x.empty() || (x.size() >= 2 && IsNL(x.back()) && IsNL(x[x.size() - 2]))))
        y.remove_prefix(1);
    x += y;
}

}  // namespace

bool IsGoBuild(std::string_view line) {
    std::string_view e;
    return SplitGoBuild(line, &e);
}

bool IsPlusBuild(std::string_view line) {
    std::string_view e;
    return SplitPlusBuild(line, &e);
}

void FixGoBuildLines(std::string &output, const std::vector<int> &go_build, const std::vector<int> &plus_build) {
    if (go_build.empty() && plus_build.empty()) return;
    size_t insert = 0;
    for (size_t pos = 0;;) {
        bool blank = true;
        while (pos < output.size() && (output[pos] == ' ' || output[pos] == '\t')) pos++;
        if (pos + 3 < output.size() && output[pos] == gotab::Escape && output[pos + 1] == '/' &&
            output[pos + 2] == '/') {
            blank = false;
            while (pos < output.size() && !IsNL(output[pos])) pos++;
        }
        if (pos >= output.size() || !IsNL(output[pos])) break;
        pos++;
        if (blank) insert = pos;
    }
    if (!go_build.empty() && static_cast<size_t>(go_build[0]) < insert) {
        insert = static_cast<size_t>(go_build[0]);
    } else if (!plus_build.empty() && static_cast<size_t>(plus_build[0]) < insert) {
        insert = static_cast<size_t>(plus_build[0]);
    }

    ExprP x;
    if (go_build.empty()) {
        for (int pos : plus_build) {
            ExprP y = Parse(CommentTextAt(output, static_cast<size_t>(pos)));
            if (!y) {
                x = nullptr;
                break;
            }
            x = x ? MkAnd(x, y) : y;
        }
    } else if (go_build.size() == 1) {
        x = Parse(CommentTextAt(output, static_cast<size_t>(go_build[0])));
    }

    std::string block;
    if (!x) {
        for (int pos : go_build) block += LineAt(output, static_cast<size_t>(pos));
        for (int pos : plus_build) block += LineAt(output, static_cast<size_t>(pos));
    } else {
        block += gotab::Escape;
        block += "//go:build ";
        block += String(x);
        block += gotab::Escape;
        block += '\n';
        if (!plus_build.empty()) {
            std::vector<std::string> lines;
            if (!PlusBuildLines(x, &lines)) lines = {"// +build error: expression too complex for // +build lines"};
            for (const std::string &line : lines) {
                block += gotab::Escape;
                block += line;
                block += gotab::Escape;
                block += '\n';
            }
        }
    }
    block += '\n';

    std::vector<int> to_delete = go_build;
    to_delete.insert(to_delete.end(), plus_build.begin(), plus_build.end());
    std::sort(to_delete.begin(), to_delete.end());

    std::string after;
    size_t start = insert;
    for (int e : to_delete) {
        const size_t end = static_cast<size_t>(e);
        if (end < start) continue;
        AppendLines(after, std::string_view(output).substr(start, end - start));
        start = end + LineAt(output, end).size();
    }
    AppendLines(after, std::string_view(output).substr(std::min(start, output.size())));
    if (after.size() >= 2 && IsNL(after.back()) && IsNL(after[after.size() - 2])) after.pop_back();
    output.resize(insert);
    output += block;
    output += after;
}

}  // namespace goprint
