// mep's Go formatter: cmd/gofmt's processFile, parse and format
// (gofmt.go, internal.go) over the ported go/parser and go/printer.
// Copyright 2009 The Go Authors. All rights reserved. BSD-style license;
// the full notice is in go_parse.cpp.
#include "go_format.h"

#include "go_doc_comment.h"
#include "go_parse.h"
#include "go_printer.h"

namespace gofmt {

namespace {

enum class Adj { None, Decls, Stmts };

struct Parsed {
    goparse::Result r;
    std::string src;  // the text that was parsed (with any fragment wrapper)
    Adj adj = Adj::None;
    int indent_adj = 0;
};

goparse::Options ParserOptions() {
    goparse::Options o;
    o.all_errors = false;  // gofmt without -e
    o.parse_comments = true;
    return o;
}

bool Contains(const std::string &s, std::string_view what) { return s.find(what) != std::string::npos; }

// gofmt's parse() with fragmentOk.
Parsed Parse(std::string_view src) {
    Parsed p;
    p.src = std::string(src);
    p.r = goparse::ParseFile(p.src, ParserOptions());
    if (p.r.errors.empty() || !Contains(p.r.errors[0].msg, "expected 'package'")) return p;

    Parsed q;
    q.src = "package p;" + std::string(src);
    q.r = goparse::ParseFile(q.src, ParserOptions());
    if (q.r.errors.empty()) {
        q.adj = Adj::Decls;
        return q;
    }
    if (!Contains(q.r.errors[0].msg, "expected declaration")) return q;

    Parsed s;
    s.src = "package p; func _() {" + std::string(src) + "\n\n}";
    s.r = goparse::ParseFile(s.src, ParserOptions());
    if (s.r.errors.empty()) {
        s.adj = Adj::Stmts;
        s.indent_adj = -1;
    }
    return s;
}

bool IsSpace(char b) { return b == ' ' || b == '\t' || b == '\n' || b == '\r'; }

bool SourceAdj(Adj adj, const std::string &buf, int indent, std::string *out) {
    std::string_view s = buf;
    if (adj == Adj::Decls) {
        const size_t skip = static_cast<size_t>(indent) + std::string_view("package p\n").size();
        if (indent < 0 || skip > s.size()) return false;
        *out = std::string(godoc::TrimSpace(s.substr(skip)));
        return true;
    }
    if (indent < 0) indent = 0;
    const size_t skip = 2 * static_cast<size_t>(indent) + std::string_view("package p\n\nfunc _() {").size();
    if (skip > s.size()) return false;
    s = s.substr(skip);
    if (s.size() < 2) return false;
    s = s.substr(0, s.size() - 2);
    *out = std::string(godoc::TrimSpace(s));
    return true;
}

Result Fail(std::string_view src, std::string msg, int line) {
    Result r;
    r.ok = false;
    r.text = std::string(src);
    r.error = std::move(msg);
    r.error_line = line;
    return r;
}

}  // namespace

Result Format(std::string_view src, const Options &opts) {
    (void)opts;
    Parsed p = Parse(src);
    if (!p.r.errors.empty()) {
        const goparse::Error &e = p.r.errors[0];
        return Fail(src, e.msg, e.line);
    }
    goprint::LineTable lines(p.r.lines, static_cast<int>(p.src.size()));
    goprint::SortImports(lines, p.r.file);

    goprint::Config cfg = goprint::GofmtConfig();
    std::string out, error;
    if (p.adj == Adj::None) {
        if (!goprint::Fprint(cfg, lines, p.r.file, &out, &error)) return Fail(src, error, 0);
    } else {
        // gofmt's format() for a fragment: keep the leading and trailing
        // white space, and indent the result like the fragment's first line.
        size_t i = 0, j = 0;
        while (j < src.size() && IsSpace(src[j])) {
            if (src[j] == '\n') i = j + 1;
            j++;
        }
        std::string res(src.substr(0, i));
        int indent = 0;
        bool has_space = false;
        for (char b : src.substr(i, j - i)) {
            if (b == ' ') has_space = true;
            else if (b == '\t') indent++;
        }
        if (indent == 0 && has_space) indent = 1;
        res.append(static_cast<size_t>(indent), '\t');
        cfg.indent = indent + p.indent_adj;
        std::string buf;
        if (!goprint::Fprint(cfg, lines, p.r.file, &buf, &error)) return Fail(src, error, 0);
        std::string adj;
        if (!SourceAdj(p.adj, buf, cfg.indent, &adj)) return Fail(src, "cannot format this fragment", 0);
        if (adj.empty()) {
            out = std::string(src);
        } else {
            res += adj;
            size_t k = src.size();
            while (k > 0 && IsSpace(src[k - 1])) k--;
            res += src.substr(k);
            out = std::move(res);
        }
    }

    // Safety net for the editor: the result must still parse.
    if (p.adj == Adj::None) {
        const goparse::Result check = goparse::ParseFile(out, ParserOptions());
        if (!check.errors.empty())
            return Fail(src, "internal error: the formatted text does not parse: " + check.errors[0].msg,
                        check.errors[0].line);
    }
    Result r;
    r.ok = true;
    r.text = std::move(out);
    return r;
}

}  // namespace gofmt
