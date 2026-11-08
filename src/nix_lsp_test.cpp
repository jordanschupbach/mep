// Windowless test for nix_parse.cpp and nix_lsp.cpp -- the parser and
// analysis halves of mep's own Nix language server (`mep-nix-lsp`,
// src/nix_lsp_server.cpp). Drives the pure functions directly: no process,
// no JSON-RPC client, no Nix. CHECK(), never assert(): the Release build
// strips assert().
//
// The parse errors are asserted by message and position, because they are
// Nix's own (what `nix-instantiate --parse` prints); tools/nix_parse_check.py
// checks the same thing over a corpus against the real Nix.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "nix_lsp.h"
#include "nix_parse.h"

namespace {
int g_checks = 0;

void Check(bool ok, const char *expr, int line) {
    g_checks++;
    if (ok) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expr, __FILE__, line);
    std::abort();
}
#define CHECK(c) Check((c), #c, __LINE__)

using Lines = std::vector<std::string>;

Lines L(const std::string &text) {
    Lines out;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

std::vector<NixLspDiagnostic> Diags(const std::string &text) { return NixLspDiagnostics(*NixLspAnalyze(L(text))); }

int Count(const std::vector<NixLspDiagnostic> &ds, const std::string &code) {
    int n = 0;
    for (const auto &d : ds) n += d.code == code ? 1 : 0;
    return n;
}

const NixLspDiagnostic *Find(const std::vector<NixLspDiagnostic> &ds, const std::string &code) {
    for (const auto &d : ds) {
        if (d.code == code) return &d;
    }
    return nullptr;
}

// What Nix would say, as "line:col: message" (1-based), or "ok".
std::string Nix(const std::string &text) {
    const NixLspCheck c = NixLspNixCheck(text);
    if (!c.parse_error) {
        if (c.undefined.empty()) return "ok";
        int line = 0, col = 0;
        nixparse::NixLineCol(text, c.undefined[0].first, &line, &col);
        return std::to_string(line) + ":" + std::to_string(col) + ": undefined variable '" + c.undefined[0].second +
               "'";
    }
    int line = 0, col = 0;
    nixparse::NixLineCol(text, c.offset, &line, &col);
    return std::to_string(line) + ":" + std::to_string(col) + ": " + c.message;
}

bool HasLabel(const std::vector<NixLspCompletionItem> &items, const std::string &label) {
    for (const auto &it : items) {
        if (it.label == label) return true;
    }
    return false;
}

void TestParseErrors() {
    CHECK(Nix("1 + 2") == "ok");
    CHECK(Nix("{ a = 1; }") == "ok");
    CHECK(Nix("{ a = 1 }") == "1:9: syntax error, unexpected '}', expecting ';'");
    CHECK(Nix("1 % 2") == "1:3: syntax error, unexpected invalid token, expecting end of file");
    CHECK(Nix("\"abc$") == "1:6: syntax error, unexpected end of file, expecting '\"'");
    CHECK(Nix("[ ./a") == "1:3: syntax error, unexpected end of file");
    CHECK(Nix("{ a = 1; a = 2; }") == "1:10: attribute 'a' already defined at «string»:1:3");
    CHECK(Nix("{ a.b = 1; a.c = 2; }") == "ok");
    CHECK(Nix("{ a.b = 1; a = { c = 2; }; }") == "ok");
    CHECK(Nix("{ a.b = 1; a.b = 2; }") == "1:12: attribute 'a.b' already defined at «string»:1:3");
    CHECK(Nix("{ x, x }: x") == "1:6: duplicate formal function argument 'x'");
    CHECK(Nix("let ${\"a\"} = 1; in 1") == "ok");  // a static string is a plain name
    CHECK(Nix("let a = \"x\"; ${a} = 1; in 1") == "1:1: dynamic attributes not allowed in let");
    CHECK(Nix("x") == "1:1: undefined variable 'x'");
    CHECK(Nix("with {}; x") == "ok");
    CHECK(Nix("let a = b; b = 1; in a") == "ok");
    CHECK(Nix("rec { a = b; b = 1; }") == "ok");
    CHECK(Nix("{ a = b; b = 1; }") == "1:7: undefined variable 'b'");
    CHECK(Nix("x: y: x + y") == "ok");
    CHECK(Nix("{ a ? b, b }: a") == "ok");
    CHECK(Nix("map builtins.map __attrNames") == "ok");
    CHECK(Nix("attrNames") == "1:1: undefined variable 'attrNames'");
    CHECK(Nix("''\n  a\n  ${x}\n''") == "3:5: undefined variable 'x'");
    CHECK(Nix("9999999999999999999") == "1:1: invalid integer '9999999999999999999'");
    CHECK(Nix("./a/") == "1:1: path has a trailing slash");
    // The cursed or: `or` is a variable, placed where the expression before it starts -- also when an
    // argument follows (`(3.14 or) 1`).
    CHECK(Nix("map or") == "1:1: undefined variable 'or'");
    CHECK(Nix("{ x = 3.14 or 1; }") == "1:7: undefined variable 'or'");
    CHECK(Nix("[ (http://a.b or false) ]") == "1:4: undefined variable 'or'");
}

void TestDiagnostics() {
    // A syntax error is reported, and recovery keeps going.
    {
        const auto ds = Diags("{\n  a = 1\n  b = 2;\n}");
        CHECK(Count(ds, "syntax-error") == 1);
        const NixLspDiagnostic *d = Find(ds, "syntax-error");
        CHECK(d && d->range.line == 2);
    }
    // undefined-variable: precise range.
    {
        const auto ds = Diags("let a = 1; in a + bb");
        const NixLspDiagnostic *d = Find(ds, "undefined-variable");
        CHECK(d && d->range.col == 18 && d->range.end_col == 20);
        CHECK(d->severity == NixLspSeverity::Error);
    }
    // Every undefined variable, not just Nix's first.
    CHECK(Count(Diags("[ p q r ]"), "undefined-variable") == 3);
    // Not under a `with`.
    CHECK(Count(Diags("with import <nixpkgs> {}; [ hello ]"), "undefined-variable") == 0);

    // unused-let, but not `_x` and not used ones.
    {
        const auto ds = Diags("let a = 1; b = 2; _c = 3; in a");
        CHECK(Count(ds, "unused-let") == 1);
        const NixLspDiagnostic *d = Find(ds, "unused-let");
        CHECK(d && d->message.find("'b'") != std::string::npos && d->unnecessary);
    }
    // A binding used only by another (used) binding counts as used.
    CHECK(Count(Diags("let a = 1; b = a; in b"), "unused-let") == 0);
    // An inherit makes a use.
    CHECK(Count(Diags("let a = 1; in { inherit a; }"), "unused-let") == 0);

    // unused-argument: a hint.
    {
        const auto ds = Diags("{ pkgs, lib }: pkgs");
        const NixLspDiagnostic *d = Find(ds, "unused-argument");
        CHECK(d && d->severity == NixLspSeverity::Hint && d->message.find("'lib'") != std::string::npos);
    }
    CHECK(Count(Diags("x: 1"), "unused-argument") == 1);
    CHECK(Count(Diags("_: 1"), "unused-argument") == 0);

    // unused-with.
    CHECK(Count(Diags("let a = 1; in with {}; a"), "unused-with") == 1);
    CHECK(Count(Diags("with {}; a"), "unused-with") == 0);
    // An inner `with` that catches the name makes the outer one used too: either may supply it.
    CHECK(Count(Diags("with {}; with {}; a"), "unused-with") == 0);

    CHECK(Count(Diags("let in 1"), "empty-let") == 1);
    CHECK(Count(Diags("rec { a = 1; b = 2; }"), "redundant-rec") == 1);
    CHECK(Count(Diags("rec { a = 1; b = a; }"), "redundant-rec") == 0);
    CHECK(Count(Diags("let { body = 1; }"), "deprecated-let") == 1);
    CHECK(Count(Diags("http://example.org"), "url-literal") == 1);
    CHECK(Count(Diags("\"http://example.org\""), "url-literal") == 0);

    // The linter's line format.
    {
        const auto ds = Diags("x");
        CHECK(!ds.empty());
        CHECK(NixLspFormatDiagnostic("f.nix", ds[0]) == "f.nix:1:1: error: undefined variable 'x' [undefined-variable]");
        CHECK(NixLspDiagnosticFails(ds[0]));
    }
    // A clean file is clean.
    CHECK(Diags("{ pkgs ? import <nixpkgs> { } }:\npkgs.mkShell { packages = [ pkgs.hello ]; }").empty());
}

void TestCompletion() {
    // Names in scope, innermost first, then builtins and keywords.
    {
        const auto doc = NixLspAnalyze(L("let alpha = 1; beta = 2; in al"));
        const auto items = NixLspCompletions(*doc, 0, 30);
        CHECK(HasLabel(items, "alpha"));
        CHECK(HasLabel(items, "beta"));
        CHECK(HasLabel(items, "map"));
        CHECK(HasLabel(items, "builtins"));
        CHECK(!HasLabel(items, "attrNames"));  // only builtins.attrNames
        CHECK(!items.empty() && items[0].replace_start == 28 && items[0].replace_end == 30);
    }
    // builtins.
    {
        const auto doc = NixLspAnalyze(L("builtins.at"));
        const auto items = NixLspCompletions(*doc, 0, 11);
        CHECK(HasLabel(items, "attrNames"));
        CHECK(!HasLabel(items, "alpha"));
    }
    // The attributes of a let-bound set.
    {
        const auto doc = NixLspAnalyze(L("let cfg = { port = 80; host = \"x\"; }; in cfg.po"));
        const auto items = NixLspCompletions(*doc, 0, 47);
        CHECK(HasLabel(items, "port"));
        CHECK(HasLabel(items, "host"));
        CHECK(!HasLabel(items, "map"));
    }
    // Function arguments.
    {
        const auto doc = NixLspAnalyze(L("{ pkgs, lib, ... }: p"));
        CHECK(HasLabel(NixLspCompletions(*doc, 0, 21), "pkgs"));
    }
    // Nothing inside a comment or string.
    {
        const auto doc = NixLspAnalyze(L("# ma\n\"ma\""));
        CHECK(NixLspCompletions(*doc, 0, 4).empty());
        CHECK(NixLspCompletions(*doc, 1, 3).empty());
    }
}

void TestHoverAndSignature() {
    {
        const auto doc = NixLspAnalyze(L("let\n  # The port.\n  port = 80;\nin port"));
        const NixLspHoverInfo h = NixLspHover(*doc, 3, 4);
        CHECK(h.found);
        CHECK(h.text.find("port = 80") != std::string::npos);
        CHECK(h.text.find("The port.") != std::string::npos);
    }
    {
        const auto doc = NixLspAnalyze(L("map (x: x) [ ]"));
        const NixLspHoverInfo h = NixLspHover(*doc, 0, 1);
        CHECK(h.found && h.text.find("builtins.map f list") != std::string::npos);
    }
    {
        const auto doc = NixLspAnalyze(L("builtins.attrNames { }"));
        const NixLspHoverInfo h = NixLspHover(*doc, 0, 12);
        CHECK(h.found && h.text.find("builtins.attrNames set") != std::string::npos);
    }
    {
        const auto doc = NixLspAnalyze(L("with { a = 1; }; a"));
        const NixLspHoverInfo h = NixLspHover(*doc, 0, 17);
        CHECK(h.found && h.text.find("with") != std::string::npos && h.text.find("= `1`") != std::string::npos);
    }
    {
        const auto doc = NixLspAnalyze(L("map (x: x) "));
        const NixLspSignatureHelp sh = NixLspSignatureAt(*doc, 0, 11);
        CHECK(sh.found && sh.active_param == 1 && sh.params.size() == 2);
    }
    {
        const auto doc = NixLspAnalyze(L("let f = a: { b, c ? 1 }: a; in f 1 "));
        const NixLspSignatureHelp sh = NixLspSignatureAt(*doc, 0, 35);
        CHECK(sh.found && sh.active_param == 1);
        CHECK(sh.params.size() == 2 && sh.params[1] == "{ b, c ? … }");
    }
}

void TestNavigation() {
    const std::string src = "let\n  a = 1;\n  b = a + a;\nin b";
    const auto doc = NixLspAnalyze(L(src));
    {
        const auto defs = NixLspDefinition(*doc, 2, 6);
        CHECK(defs.size() == 1 && defs[0].range.line == 1 && defs[0].range.col == 2);
    }
    {
        const auto refs = NixLspReferences(*doc, 1, 2);
        CHECK(refs.size() == 3);
        CHECK(refs[0].is_definition);
    }
    {
        const NixLspRenameTarget t = NixLspPrepareRename(*doc, 2, 6);
        CHECK(t.found && t.placeholder == "a");
        std::string err;
        const auto edits = NixLspRename(*doc, 2, 6, "z", &err);
        CHECK(err.empty() && edits.size() == 3);
        NixLspRename(*doc, 2, 6, "b", &err);
        CHECK(!err.empty());
        err.clear();
        NixLspRename(*doc, 2, 6, "if", &err);
        CHECK(!err.empty());
    }
    // A formal's name is its callers' too.
    {
        const auto d2 = NixLspAnalyze(L("{ x }: x"));
        std::string err;
        NixLspRename(*d2, 0, 7, "y", &err);
        CHECK(!err.empty());
    }
    // Renaming must not capture a `with` name.
    {
        const auto d3 = NixLspAnalyze(L("let a = 1; in with {}; [ a b ]"));
        std::string err;
        NixLspRename(*d3, 0, 4, "b", &err);
        CHECK(!err.empty());
    }
    // Definition of an attribute of a known set.
    {
        const auto d4 = NixLspAnalyze(L("let s = { k = 1; }; in s.k"));
        const auto defs = NixLspDefinition(*d4, 0, 25);
        CHECK(defs.size() == 1 && defs[0].range.col == 10);
    }
}

void TestOutline() {
    const auto doc = NixLspAnalyze(L("{ pkgs }:\n{\n  name = \"x\";\n  meta = {\n    license = 1;\n  };\n  "
                                     "inherit (pkgs) hello;\n}"));
    const auto syms = NixLspSymbols(*doc);
    CHECK(syms.size() == 4);
    CHECK(syms[0].name == "name" && syms[0].kind == NixLspSymbolKind::String);
    CHECK(syms[1].name == "meta" && syms[1].kind == NixLspSymbolKind::Object);
    CHECK(syms[2].name == "license" && syms[2].parent == 1);
    CHECK(syms[3].name == "hello" && syms[3].detail == "inherit (pkgs)");
    const auto folds = NixLspFolds(*doc);
    CHECK(folds.size() == 2);
    CHECK(folds[0].start_line == 1 && folds[0].end_line == 6);
}

void TestFormat() {
    std::string err;
    const auto edits = NixLspFormat(L("{a=1;}"), 100, &err);
    CHECK(err.empty() && edits.size() == 1 && edits[0].new_text == "{ a = 1; }\n");  // nixfmt ends the file with a newline
    CHECK(NixLspFormat(L("{ a = 1; }\n"), 100, &err).empty());
    NixLspFormat(L("{ a = "), 100, &err);
    CHECK(!err.empty());
}

void TestRobustness() {
    // Every prefix of a realistic file analyses without crashing, and every query works on it.
    const std::string src =
        "{ lib, stdenv, fetchurl, ... }:\n"
        "let\n"
        "  version = \"1.0\";\n"
        "  src = fetchurl { url = \"https://x/${version}.tar.gz\"; sha256 = \"\"; };\n"
        "in stdenv.mkDerivation rec {\n"
        "  pname = \"x\";\n"
        "  inherit version src;\n"
        "  buildPhase = ''\n"
        "    make ${lib.optionalString true \"-j\"}\n"
        "  '';\n"
        "  meta = with lib; { license = licenses.mit; };\n"
        "}\n";
    for (size_t n = 0; n <= src.size(); n++) {
        const Lines lines = L(src.substr(0, n));
        const auto doc = NixLspAnalyze(lines);
        NixLspDiagnostics(*doc);
        const int last = static_cast<int>(lines.size()) - 1;
        const int col = static_cast<int>(lines.back().size());
        NixLspCompletions(*doc, last, col);
        NixLspHover(*doc, last, col);
        NixLspSignatureAt(*doc, last, col);
        NixLspSymbols(*doc);
        NixLspFolds(*doc);
    }
    CHECK(Diags(src).empty() || Find(Diags(src), "syntax-error") == nullptr);
}

}  // namespace

int main() {
    TestParseErrors();
    TestDiagnostics();
    TestCompletion();
    TestHoverAndSignature();
    TestNavigation();
    TestOutline();
    TestFormat();
    TestRobustness();
    std::printf("nix_lsp_test: %d checks passed\n", g_checks);
    return 0;
}
