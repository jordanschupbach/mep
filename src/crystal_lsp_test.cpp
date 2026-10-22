// Coverage for mep's Crystal language server (crystal_lsp.h) and the
// parser under it (crystal_parser.h). The parser itself is checked against
// the real compiler on a corpus (tools/crystal_ast_dump.cr: every tree,
// every location, every syntax error); the cases here pin the compiler's
// wording of common errors and the server's features.
//
// `mep-crystal-lsp-test --parse FILE...` prints each file's tree (or its
// syntax error) in crystal_ast_dump's format, for that comparison.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "crystal_lsp.h"
#include "crystal_parser.h"

namespace {

int checks = 0;

void Check(bool condition, const char *expression, int line) {
    checks++;
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

std::vector<std::string> L(const std::string &text) {
    std::vector<std::string> out;
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

std::string ParseError(const std::string &src, int *line = nullptr, int *col = nullptr) {
    crystal::Ast ast;
    crystal::ParseResult r = crystal::Parse(src, ast);
    if (line) *line = r.error_line;
    if (col) *col = r.error_column;
    return r.ok ? std::string() : r.error;
}

bool HasCode(const std::vector<CrLspDiagnostic> &ds, const std::string &code, int line = -1) {
    for (const CrLspDiagnostic &d : ds) {
        if (d.code == code && (line < 0 || d.range.start_line == line)) return true;
    }
    return false;
}

bool HasLabel(const std::vector<CrLspCompletionItem> &items, const std::string &label) {
    for (const CrLspCompletionItem &it : items) {
        if (it.label == label) return true;
    }
    return false;
}

int ParseFiles(int argc, char **argv) {
    int status = 0;
    for (int k = 2; k < argc; k++) {
        std::ifstream in(argv[k], std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string src = ss.str();
        crystal::Ast ast;
        crystal::ParseResult r = crystal::Parse(src, ast, argv[k]);
        std::string out = std::string("=== ") + argv[k] + "\n";
        if (r.ok) {
            out += crystal::DumpAst(r.root);
        } else {
            std::string m = r.error;
            const size_t nl = m.find('\n');
            if (nl != std::string::npos) m = m.substr(0, nl);
            out += r.error_line == 0 ? "ERROR ?:? " + m + "\n"
                                     : "ERROR " + std::to_string(r.error_line) + ":" +
                                           std::to_string(r.error_column) + " " + m + "\n";
            status = 1;
        }
        for (const std::string &w : r.warnings) {
            const size_t c = w.find(": ");
            out += "WARNING " + w.substr(0, c) + " " + w.substr(c + 2) + "\n";
        }
        std::fwrite(out.data(), 1, out.size(), stdout);
    }
    return status;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc > 1 && std::strcmp(argv[1], "--parse") == 0) return ParseFiles(argc, argv);

    // --- the parser: the compiler's own errors --------------------------------------
    int line = 0, col = 0;
    CHECK(ParseError("def foo\n  1\n") == "expecting identifier 'end', not 'EOF'");
    CHECK(ParseError("x = [] \n", &line, &col) == "for empty arrays use '[] of ElementType'");
    CHECK(line == 1 && col == 5);
    CHECK(ParseError("foo(1, 2\n") == "expecting token ')', not 'EOF'");
    CHECK(ParseError("puts \"abc\n") == "Unterminated string literal");
    CHECK(ParseError("b? = 1\n") == "unexpected token: \"=\"");
    CHECK(ParseError("a = 1; a/b\n").empty());  // division, not a regex
    CHECK(ParseError("a/b\n").empty());
    CHECK(ParseError("case 1\nwhen 1\nwhen 1\nend\n") == "duplicate when 1 in case");
    CHECK(ParseError("x = 0x\n") == "numeric literal without digits");
    CHECK(ParseError("\xA7").find("malformed UTF-8") != std::string::npos);
    {
        crystal::Ast ast;
        crystal::ParseResult r = crystal::Parse("x : Int32 = 1\nx\n", ast);
        CHECK(r.ok);
        CHECK(crystal::DumpAst(r.root).rfind("Expressions 1:1-2:1 keyword=None\n", 0) == 0);
    }

    // --- diagnostics -------------------------------------------------------------------
    {
        const auto ds = CrLspDiagnostics(L("def foo(a, b)\n  x = 1\n  return a\n  puts b\nend\n"));
        CHECK(HasCode(ds, "unused-variable", 1));
        CHECK(HasCode(ds, "unreachable-code", 3));
        CHECK(!HasCode(ds, "unused-argument"));  // b is read (if unreachably)
    }
    {
        const auto ds = CrLspDiagnostics(L("def foo(a, _b)\n  1\nend\n[1].each { |x| }\n"));
        CHECK(HasCode(ds, "unused-argument", 0));
        CHECK(HasCode(ds, "unused-block-parameter", 3));
        for (const CrLspDiagnostic &d : ds) CHECK(!CrLspDiagnosticFails(d));  // hints only
    }
    {
        // Macro arguments are not locals; `$~` is not unused.
        const auto ds = CrLspDiagnostics(
            L("class A\n  getter? closed = false\n  property x : Int32 = 0\nend\nrecord P, a : Int32 = 1\n"
              "def m(s)\n  $~ = s.match(/x/)\nend\n"));
        CHECK(!HasCode(ds, "unused-variable"));
    }
    {
        const auto ds = CrLspDiagnostics(L("x = 1\n[1].each { |x| p x }\np x\n"));
        CHECK(HasCode(ds, "shadowed-variable", 1));
    }
    {
        const auto ds = CrLspDiagnostics(L("def foo\n  1\n"));
        CHECK(ds.size() == 1 && ds[0].code == "syntax" && ds[0].message == "expecting identifier 'end', not 'EOF'");
    }
    {
        const auto ds = CrLspDiagnostics(L("require \"./no_such_file\"\n"), "/nonexistent-dir/main.cr");
        CHECK(HasCode(ds, "require-not-found", 0));
        CHECK(ds.back().message == "can't find file './no_such_file' relative to '/nonexistent-dir'");
    }
    CHECK(CrLspFormatDiagnostic("a.cr", CrLspDiagnostics(L("x = 1\n"))[0]) ==
          "a.cr:1:1: warning: Useless assignment to variable 'x' [unused-variable]");

    // --- formatting ----------------------------------------------------------------------
    {
        // One edit, over the lines that changed only.
        const auto edits = CrLspFormat(L("x = 1\ndef f( a )\na\nend\n"));
        CHECK(edits.size() == 1);
        CHECK(edits[0].range.start_line == 1 && edits[0].range.end_line == 3);
        CHECK(edits[0].new_text == "\ndef f(a)\n  a\n");  // a blank line before a def, as the compiler's
        CHECK(CrLspFormat(L("x = 1\n")).empty());     // already formatted
        CHECK(CrLspFormat(L("def f\n  1 +\n")).empty());  // does not parse: left alone
    }

    // --- completion ----------------------------------------------------------------------
    {
        const auto lines = L("count = 3\nname = \"x\"\nco\n");
        const auto items = CrLspCompletions(lines, 2, 2);
        CHECK(HasLabel(items, "count"));
        CHECK(!HasLabel(items, "name"));  // filtered by the prefix
    }
    {
        const auto lines = L("s = \"hello\"\ns.up\n");
        const auto items = CrLspCompletions(lines, 1, 4);
        CHECK(HasLabel(items, "upcase"));
        CHECK(!HasLabel(items, "count"));  // Int32's? no: String#count exists -- only prefix "up"
    }
    {
        const auto lines = L("class Point\n  def initialize(@x : Int32)\n  end\n  def dist\n    @x\n  end\nend\n"
                             "pt = Point.new(1)\npt.\nPoint.\n");
        const auto items = CrLspCompletions(lines, 8, 3);
        CHECK(HasLabel(items, "dist"));
        CHECK(HasLabel(items, "to_s"));  // Object's, through the ancestors
        CHECK(!HasLabel(items, "new"));
        const auto cls = CrLspCompletions(lines, 9, 6);
        CHECK(HasLabel(cls, "new"));
        const auto ivars = CrLspCompletions(L("class A\n  @count = 0\n  def f\n    @c\n  end\nend\n"), 3, 6);
        CHECK(HasLabel(ivars, "@count"));
    }
    {
        // In a file that does not parse yet, the rest of it still helps.
        const auto lines = L("total = 0\ndef go\n  x = [1, 2]\n  x.\nend\n");
        const auto items = CrLspCompletions(lines, 3, 4);
        CHECK(HasLabel(items, "each"));
    }

    // --- hover, signatures ------------------------------------------------------------------
    {
        const auto lines = L("# Adds.\ndef add(a : Int32, b : Int32) : Int32\n  a + b\nend\nadd(1, 2)\nn = 5\nn\n");
        const CrLspHoverInfo h = CrLspHover(lines, 4, 1);
        CHECK(h.found && h.text.find("def add(a : Int32, b : Int32) : Int32") != std::string::npos);
        CHECK(h.text.find("Adds.") != std::string::npos);
        const CrLspHoverInfo hv = CrLspHover(lines, 6, 0);
        CHECK(hv.found && hv.text.find("n : Int32") != std::string::npos);
        const CrLspSignatureHelp sh = CrLspSignatures(L("def add(a : Int32, b : Int32)\nend\nadd(1, "), 2, 7);
        CHECK(sh.found && sh.active_param == 1 && sh.signatures[0].params.size() == 2);
        const CrLspSignatureHelp std_sig = CrLspSignatures(L("\"a b\".split("), 0, 12);
        CHECK(std_sig.found);
        const CrLspHoverInfo hs = CrLspHover(L("puts 1\n"), 0, 1);
        CHECK(hs.found && hs.text.find("def puts") != std::string::npos);
    }

    // --- navigation -----------------------------------------------------------------------
    {
        const auto lines = L("def helper(v)\n  v * 2\nend\nx = 1\ny = helper(x)\nx = x + y\n");
        const CrLspLocation d = CrLspDefinition(lines, 4, 5);
        CHECK(d.found && d.range.start_line == 0 && d.range.start_col == 4);
        const CrLspLocation dl = CrLspDefinition(lines, 5, 4);
        CHECK(dl.found && dl.range.start_line == 3);
        const auto refs = CrLspReferences(lines, 3, 0);
        CHECK(refs.size() == 4);  // x = 1, helper(x), x = x + y twice
        std::string error;
        const auto edits = CrLspRename(lines, 3, 0, "count", &error);
        CHECK(error.empty() && edits.size() == 4);
        CHECK(!CrLspRename(lines, 3, 0, "Count", &error).size() && !error.empty());
        const auto mrefs = CrLspReferences(lines, 0, 5);
        CHECK(mrefs.size() == 2);
    }
    {
        // Block scope: the block's own local does not leak.
        const auto lines = L("a = 1\n[1].each do |i|\n  b = i\n  a = b\nend\n");
        CHECK(CrLspReferences(lines, 0, 0).size() == 2);
        CHECK(CrLspReferences(lines, 2, 2).size() == 2);
    }

    // --- outline, folds -------------------------------------------------------------------
    {
        const auto lines = L("module Geo\n  class Point\n    getter x : Int32\n    def initialize(@x)\n    end\n"
                             "    def self.origin\n      new(0)\n    end\n  end\n  enum Color\n    Red\n    Green\n"
                             "  end\n  MAX = 10\nend\n");
        const auto syms = CrLspSymbols(lines);
        CHECK(syms.size() >= 8);
        CHECK(syms[0].name == "Geo" && syms[0].kind == CrLspSymbolKind::Module);
        CHECK(syms[1].name == "Point" && syms[1].parent == 0);
        bool origin = false, red = false, max = false, getter = false;
        for (const CrLspSymbol &s : syms) {
            if (s.name == "self.origin") origin = true;
            if (s.name == "Red" && s.kind == CrLspSymbolKind::EnumMember) red = true;
            if (s.name == "MAX" && s.kind == CrLspSymbolKind::Constant) max = true;
            if (s.name == "x" && s.kind == CrLspSymbolKind::Property) getter = true;
        }
        CHECK(origin && red && max && getter);
        const auto folds = CrLspFolds(lines);
        CHECK(!folds.empty() && folds[0].start_line == 0);
    }

    std::printf("crystal_lsp_test: all %d checks passed\n", checks);
    return 0;
}
