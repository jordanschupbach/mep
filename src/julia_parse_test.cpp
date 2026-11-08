// Checks for the Julia parser (julia_parse.h), and the dump modes
// tools/julia_parse_check.py compares against Julia's own parsers:
//
//   julia-parse-test                      run the unit checks
//   julia-parse-test --tree FILE...       green tree of each file
//   julia-parse-test --diag FILE...       diagnostics of each file
//
// Dump output: for each file a header line `### N` (N = 0-based argument
// index), then the dump in tools/julia_parse_oracle.jl's format:
//   tree: DEPTH \t KIND \t FLAGS \t SPAN \t L|N   (preorder)
//   diag: FIRST_BYTE \t LAST_BYTE \t LEVEL \t MESSAGE
//
// Never use assert() here: the Release build defines NDEBUG.

#include "julia_parse.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using namespace jlparse;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

bool ReadFile(const char *path, std::string *out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

void DumpTree(const Tree &t, std::string &out) {
    // depth via an explicit stack of subtree ends
    std::vector<uint32_t> ends;
    for (uint32_t i = 0; i < t.nodes.size(); ++i) {
        while (!ends.empty() && i >= ends.back()) ends.pop_back();
        const Node &n = t.nodes[i];
        out += std::to_string(ends.size());
        out += '\t';
        out += KindName(n.kind);
        out += '\t';
        out += std::to_string(n.flags);
        out += '\t';
        out += std::to_string(n.span);
        out += '\t';
        out += n.leaf ? 'L' : 'N';
        out += '\n';
        ends.push_back(n.end);
    }
}

void DumpDiags(const std::vector<Diagnostic> &ds, std::string &out) {
    for (const Diagnostic &d : ds) {
        out += std::to_string(d.first_byte);
        out += '\t';
        out += std::to_string(d.last_byte);
        out += '\t';
        out += d.level == Level::Error ? "error" : "warning";
        out += '\t';
        for (char c : d.message) {
            if (c == '\n')
                out += "\\n";
            else
                out += c;
        }
        out += '\n';
    }
}

std::string Sexpr(const Tree &t, std::string_view text, uint32_t i = 0) {
    // A compact S-expression for the unit checks: non-trivia nodes only,
    // leaves as their text.
    const Node &n = t.nodes[i];
    if (n.leaf) return std::string(text.substr(n.offset, n.span));
    std::string s = "(" + HeadString(n.head());
    t.ForEachChild(i, [&](uint32_t c) {
        if (is_trivia(t.nodes[c].flags) && !is_error(t.nodes[c].kind)) return;
        s += " " + Sexpr(t, text, c);
    });
    return s + ")";
}

std::string Parse1(std::string_view text) {
    ParseResult r = Parse(text);
    // the single statement under toplevel
    std::vector<uint32_t> kids = r.tree.Children(0);
    std::string out;
    for (uint32_t c : kids) {
        if (is_trivia(r.tree.nodes[c].flags) && !is_error(r.tree.nodes[c].kind)) continue;
        if (!out.empty()) out += " ";
        out += Sexpr(r.tree, text, c);
    }
    return out;
}

bool SpansCover(const Tree &t, size_t text_size) {
    // Children tile their parent exactly.
    if (t.nodes.empty()) return false;
    if (t.nodes[0].offset != 0 || t.nodes[0].span != text_size) return false;
    for (uint32_t i = 0; i < t.nodes.size(); ++i) {
        const Node &n = t.nodes[i];
        if (n.leaf) {
            if (n.end != i + 1) return false;
            continue;
        }
        uint32_t pos = n.offset;
        bool ok = true;
        t.ForEachChild(i, [&](uint32_t c) {
            if (t.nodes[c].offset != pos) ok = false;
            pos += t.nodes[c].span;
        });
        if (!ok || pos != n.offset + n.span) return false;
    }
    return true;
}

void UnitChecks() {
    CHECK(K("call") == KindFromName("call"));
    CHECK(std::string(KindName(K("+="))) == "+=");
    CHECK(KindFromName("BEGIN_OPS") == K("ErrorInvalidOperator"));
    CHECK(KindFromName("no such kind") == K("None"));
    CHECK(is_operator(K("+")) && !is_operator(K("Identifier")));
    CHECK(is_keyword(K("function")) && is_contextual_keyword(K("mutable")));
    CHECK(is_prec_assignment(K("+=")) && is_prec_comparison(K("<:")) && is_prec_power(K("^")));
    CHECK(HeadString({K("call"), INFIX_FLAG}) == "call-i");
    CHECK(HeadString({K("+"), DOTOP_FLAG}) == ".+");

    // Examples from JuliaSyntax's parser.jl comments
    CHECK(Parse1("a + b + c") == "(call-i a + b c)");
    CHECK(Parse1("a .+ b") == "(dotcall-i a + b)");
    CHECK(Parse1("x, = xs") == "(= (tuple x) xs)");
    CHECK(Parse1("a ? b : c") == "(? a b c)");
    CHECK(Parse1("x -> y") == "(-> x y)");
    CHECK(Parse1("f(a; b=2)") == "(call f a (parameters (= b 2)))");
    CHECK(Parse1("[x y ; z w]") == "(vcat (row x y) (row z w))");
    CHECK(Parse1("[x ; y ;; z ]") == "(ncat-2 (nrow-1 x y) z)");
    CHECK(Parse1("x\"str\"") == "(macrocall x (string-r str))");
    CHECK(Parse1("2x") == "(juxtapose 2 x)");
    CHECK(Parse1("-2^x") == "(call-pre - (call-i 2 ^ x))");
    CHECK(Parse1("function f() end") == "(function (call f) (block))");
    CHECK(Parse1("mutable struct A end") == "(struct-mut A (block))");
    CHECK(Parse1("import A: x, y") == "(import (: (importpath A) (importpath x) (importpath y)))");
    CHECK(Parse1("for x in xs end") == "(for (= x xs) (block))");

    // Lossless: the tree covers every byte, trivia included.
    const char *samples[] = {"", "x", "  # c\n\nf(x) = 2x  #= c =# \n", "\"\"\"\n  a\n  $b\n  \"\"\"",
                             "(((", "end end", "\xff\xfe", "'", "x = 1 +", "[1 2; 3 4"};
    for (const char *s : samples) {
        ParseResult r = Parse(s);
        CHECK(SpansCover(r.tree, std::strlen(s)));
    }

    // Diagnostics
    {
        ParseResult r = Parse("x = (1, 2]");
        CHECK(r.diagnostics.size() == 2);
        CHECK(r.diagnostics[0].first_byte == 10 && r.diagnostics[0].last_byte == 9);
        CHECK(r.diagnostics[0].message == "Expected `)` or `,`");
        CHECK(r.diagnostics[1].message == "extra tokens after end of expression");
    }
    {
        ParseResult r = Parse("x = \"\xff\x80\" + 1e500");
        CHECK(r.diagnostics.size() == 2);
        CHECK(r.diagnostics[0].message == "invalid UTF-8 sequence \"\\xff\\x80\"");
        CHECK(r.diagnostics[1].message == "overflow in floating point literal");
    }
    {
        // 1.0.2 takes a macro call as a function signature, but not a plain
        // `...` command literal (a macrocall only in 0.4.10's tree).
        ParseResult r = Parse("function `a` end");
        CHECK(r.diagnostics.size() == 1 &&
              r.diagnostics[0].message == "Invalid signature in function definition");
        CHECK(Parse("function x`a` end").diagnostics.empty());
        CHECK(Parse("function @m(x) end").diagnostics.empty());
        CHECK(Parse("function (`a`) end").diagnostics.empty());
        CHECK(Parse("function (@m(x)) end").diagnostics.size() == 1);
    }
    {
        SourceIndex idx("ab\ncαd\n");
        LineCol lc = idx.Location(7);  // 'd'
        CHECK(lc.line == 2 && lc.column == 3);
    }

    // Depth safety: deep nesting must not overflow the stack.
    {
        std::string deep(20000, '(');
        deep += "x";
        deep += std::string(20000, ')');
        ParseResult r = Parse(deep);
        CHECK(r.depth_limited);
        CHECK(SpansCover(r.tree, deep.size()));
        CHECK(!r.diagnostics.empty() && r.diagnostics[0].message == "expression nested too deeply");
    }
    {
        std::string deep;
        for (int i = 0; i < 50000; ++i) deep += "-";
        deep += "x";
        ParseResult r = Parse(deep);
        CHECK(r.depth_limited && SpansCover(r.tree, deep.size()));
    }
    {
        std::string deep = "if a\n";
        for (int i = 0; i < 30000; ++i) deep += "elseif\n";
        deep += "end";
        ParseResult r = Parse(deep);
        CHECK(SpansCover(r.tree, deep.size()));
    }
    {
        std::string deep;
        for (int i = 0; i < 30000; ++i) deep += "a = ";
        deep += "1";
        ParseResult r = Parse(deep);
        CHECK(SpansCover(r.tree, deep.size()));
    }
    {
        // Modest nesting stays exact
        std::string s(200, '[');
        s += std::string(200, ']');
        ParseResult r = Parse(s);
        CHECK(!r.depth_limited && r.diagnostics.empty());
    }
}

}  // namespace

int main(int argc, char **argv) {
    if (argc >= 2 && (std::strcmp(argv[1], "--tree") == 0 || std::strcmp(argv[1], "--diag") == 0)) {
        bool tree = std::strcmp(argv[1], "--tree") == 0;
        std::string out;
        for (int i = 2; i < argc; ++i) {
            out += "### " + std::to_string(i - 2) + "\n";
            std::string text;
            if (!ReadFile(argv[i], &text)) {
                out += "CRASH unreadable\n";
                continue;
            }
            ParseResult r = Parse(text);
            if (tree)
                DumpTree(r.tree, out);
            else
                DumpDiags(r.diagnostics, out);
            if (out.size() > (1u << 20)) {
                std::fwrite(out.data(), 1, out.size(), stdout);
                out.clear();
            }
        }
        std::fwrite(out.data(), 1, out.size(), stdout);
        return 0;
    }
    UnitChecks();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("julia_parse_test: all checks passed\n");
    return 0;
}
