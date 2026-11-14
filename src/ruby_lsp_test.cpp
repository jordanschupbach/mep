// Windowless test for mep's own Ruby language server (`mep-ruby-lsp`): drives
// ruby_lsp.h's pure functions directly -- no process, no JSON-RPC client.
// CHECK(), never assert(): the Release build strips assert().
//
// The load-path tests use test/fixtures/ruby/datamunge, a stand-in for the
// YARD stub rosetta's Ruby backend generates (a module of classes with
// defaults, keywords, @param/@return/@raise/@option/@example/@overload).
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "ruby_lsp.h"

namespace fs = std::filesystem;

namespace {
int g_checks = 0;

void Check(bool ok, const char *expr, int line) {
    g_checks++;
    if (ok) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expr, __FILE__, line);
    std::abort();
}
#define CHECK(c) Check((c), #c, __LINE__)

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

bool Has(const std::string &hay, const std::string &needle) { return hay.find(needle) != std::string::npos; }

const RubyLspCompletionItem *Item(const std::vector<RubyLspCompletionItem> &items, const std::string &label) {
    for (const auto &it : items) {
        if (it.label == label) return &it;
    }
    return nullptr;
}

std::string ReadFile(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

const std::string kFixture = std::string(MEP_SOURCE_DIR) + "/test/fixtures/ruby/datamunge";

// Where @p needle's @p nth occurrence starts (plus @p offset), as (line, col).
std::pair<int, int> Pos(const std::string &src, const std::string &needle, int nth = 0, int offset = 0) {
    size_t at = std::string::npos;
    size_t from = 0;
    for (int i = 0; i <= nth; i++) {
        at = src.find(needle, from);
        if (at == std::string::npos) {
            std::fprintf(stderr, "test bug: %s not found\n", needle.c_str());
            std::abort();
        }
        from = at + 1;
    }
    at += static_cast<size_t>(offset);
    int line = 0;
    size_t ls = 0;
    for (size_t i = 0; i < at; i++) {
        if (src[i] == '\n') {
            line++;
            ls = i + 1;
        }
    }
    return {line, static_cast<int>(at - ls)};
}

// --- Diagnostics ------------------------------------------------------------------------

void TestSyntaxDiagnostics() {
    auto diags = RubyLspDiagnostics(*RubyLspAnalyze(L("def foo\n  1 +\nend\n")));
    CHECK(!diags.empty() && diags[0].code == "syntax-error");
    diags = RubyLspDiagnostics(*RubyLspAnalyze(L("class Foo\n  def bar\n    1\n  end\n")));
    CHECK(diags.size() == 1 && Has(diags[0].message, "`class` without a matching `end`"));
    diags = RubyLspDiagnostics(*RubyLspAnalyze(L("x = [1, 2\nputs x\n")));
    CHECK(!diags.empty());
    diags = RubyLspDiagnostics(*RubyLspAnalyze(L("foo(1, 2))\n")));
    CHECK(!diags.empty() && diags[0].range.line == 0);
    // Valid Ruby: no diagnostics at all (zero false positives).
    const char *valid[] = {
        "x = 1\nputs x\n",
        "def m(a, b = 2, *r, c:, d: 3, **kw, &blk) = a + b\n",
        "text = <<~EOS\n  hello #{name}\n  world\nEOS\nputs text\n",
        "words = %w[a b c]\nsyms = %i[x y]\n",
        "=begin\nblock comment\n=end\nputs 1\n",
        "case x\nwhen 1 then :a\nelse :b\nend\n",
        "case [1, 2]\nin [y, *] then y\nend\n",
        "foo&.bar ||= baz\n",
        "begin\n  risky\nrescue ArgumentError, TypeError => e\n  warn e.message\nensure\n  done\nend\n",
        "h = { a: 1, 'b' => 2 }\n",
        "l = ->(x, y = 1) { x * y }\nl.(2)\n",
        "class Foo < Struct.new(:a, :b)\n  def to_s = \"#{a}#{b}\"\nend\n",
        "puts(/a+b/.match?(\"aab\") ? 1 : 2)\n",
        "x = if a then b else c end\n",
        "obj.method_call arg1, key: value do |r|\n  r.next\nend\n",
        "__END__\nanything goes here ( [ {\n",
        "def self.x; end\nclass << self\n  attr_reader :y\nend\n",
        "a, (b, c), *d = 1, [2, 3], 4\n",
        "puts \"a\" \\\n  \"b\"\n",
        "value = data.dig(:a, 0)&.then { _1 * 2 }\n",
        "",
    };
    for (const char *src : valid) {
        const auto d = RubyLspDiagnostics(*RubyLspAnalyze(L(src)));
        if (!d.empty()) std::fprintf(stderr, "unexpected diagnostic in: %s -> %s\n", src, d[0].message.c_str());
        CHECK(d.empty());
    }
}

void TestRequireRelativeDiagnostic() {
    const std::string path = kFixture + "/lib/datamunge.rb";
    const auto ok = RubyLspAnalyze(L(ReadFile(path)), path);
    CHECK(RubyLspDiagnostics(*ok).empty());
    const auto bad = RubyLspAnalyze(L("require_relative 'not_there'\nrequire 'not_on_path_either'\n"), path);
    const auto d = RubyLspDiagnostics(*bad);
    // A missing `require` is not reported (gems, stdlib); a missing relative file is.
    CHECK(d.size() == 1 && d[0].code == "missing-require" && d[0].severity == RubyLspSeverity::Warning);
}

// --- Outline / folding ------------------------------------------------------------------

void TestSymbols() {
    const std::string src =
        "module Outer\n"
        "  VERSION = '1'\n"
        "  class Inner < Base\n"
        "    attr_accessor :x, :y\n"
        "    def initialize(x)\n"
        "      @x = x\n"
        "    end\n"
        "    def self.build = new(1)\n"
        "    class << self\n"
        "      def helper; end\n"
        "    end\n"
        "    private\n"
        "    def secret; end\n"
        "  end\n"
        "end\n"
        "def top_level(a); end\n";
    const auto syms = RubyLspSymbols(*RubyLspAnalyze(L(src)));
    auto find = [&](const std::string &name) -> const RubyLspSymbol * {
        for (const auto &s : syms) {
            if (s.name == name) return &s;
        }
        return nullptr;
    };
    const RubyLspSymbol *outer = find("Outer");
    const RubyLspSymbol *inner = find("Inner");
    CHECK(outer && outer->kind == RubyLspSymbolKind::Module && outer->parent == -1);
    CHECK(inner && inner->kind == RubyLspSymbolKind::Class && inner->detail == "< Base");
    CHECK(syms[static_cast<size_t>(inner->parent)].name == "Outer");
    CHECK(find("VERSION") && find("VERSION")->kind == RubyLspSymbolKind::Constant);
    CHECK(find("x") && find("x")->kind == RubyLspSymbolKind::Property && find("y"));
    CHECK(find("initialize") && find("initialize")->detail == "(x)");
    CHECK(find("self.build") && find("self.helper") && find("secret"));
    CHECK(syms[static_cast<size_t>(find("secret")->parent)].name == "Inner");
    CHECK(find("top_level") && find("top_level")->kind == RubyLspSymbolKind::Function && find("top_level")->parent == -1);
    // Folds: the module, the multi-line def, comment runs.
    const auto folds = RubyLspFolds(*RubyLspAnalyze(L("# a\n# b\nmodule M\n  def f\n    1\n  end\nend\n")));
    bool module_fold = false, def_fold = false, comment_fold = false;
    for (const auto &f : folds) {
        if (f.start_line == 2 && f.end_line == 5) module_fold = true;
        if (f.start_line == 3 && f.end_line == 4) def_fold = true;
        if (f.start_line == 0 && f.end_line == 1 && f.kind == "comment") comment_fold = true;
    }
    CHECK(module_fold && def_fold && comment_fold);
}

// --- In-document hover / completion / definition ---------------------------------------------

void TestLocalAnalysis() {
    const std::string src =
        "class Point\n"
        "  # The x coordinate.\n"
        "  # @return [Float]\n"
        "  attr_reader :x\n"
        "\n"
        "  # Makes a point.\n"
        "  # @param x [Float] across\n"
        "  # @param y [Float] down\n"
        "  def initialize(x, y = 0.0)\n"
        "    @x = x\n"
        "    @y = y\n"
        "  end\n"
        "\n"
        "  # Distance to +other+.\n"
        "  # @param other [Point]\n"
        "  # @return [Float]\n"
        "  def dist(other)\n"
        "    dx = other.x - x\n"
        "    Math.sqrt(dx * dx)\n"
        "  end\n"
        "end\n"
        "\n"
        "p1 = Point.new(1.0, 2.0)\n"
        "d = p1.dist(Point.new(0.0))\n"
        "names = %w[a b]\n"
        "names.each do |n|\n"
        "  puts n.upcase\n"
        "end\n"
        "p1.";
    const auto doc = RubyLspAnalyze(L(src));
    // Hover on a method call: the YARD docs and the signature.
    auto [l, c] = Pos(src, "dist(Point", 0, 1);
    auto h = RubyLspHover(*doc, l, c);
    CHECK(h.found && Has(h.text, "Point#dist(other) -> Float") && Has(h.text, "Distance to +other+."));
    // Hover on a local: its inferred type.
    std::tie(l, c) = Pos(src, "p1.dist");
    h = RubyLspHover(*doc, l, c);
    CHECK(h.found && Has(h.text, "p1: Point") && Has(h.text, "local variable"));
    std::tie(l, c) = Pos(src, "d = p1");
    CHECK(Has(RubyLspHover(*doc, l, c).text, "d: Float"));
    // A parameter's type from @param.
    std::tie(l, c) = Pos(src, "other.x");
    h = RubyLspHover(*doc, l, c);
    CHECK(Has(h.text, "other: Point") && Has(h.text, "parameter"));
    // `other.x`: the attribute, through the parameter's YARD type.
    std::tie(l, c) = Pos(src, "other.x", 0, 6);
    h = RubyLspHover(*doc, l, c);
    CHECK(h.found && Has(h.text, "Point#x -> Float") && Has(h.text, "The x coordinate."));
    // `Point.new`: initialize's signature and docs.
    std::tie(l, c) = Pos(src, "Point.new(1.0", 0, 6);
    h = RubyLspHover(*doc, l, c);
    CHECK(h.found && Has(h.text, "Point.new(x, y = 0.0)") && Has(h.text, "Makes a point."));
    // Block parameter over %w[]: a String.
    std::tie(l, c) = Pos(src, "n.upcase");
    CHECK(Has(RubyLspHover(*doc, l, c).text, "n: String"));
    std::tie(l, c) = Pos(src, "upcase");
    CHECK(Has(RubyLspHover(*doc, l, c).text, "String#upcase"));
    // An instance variable's type.
    std::tie(l, c) = Pos(src, "@y = y", 0, 1);
    CHECK(Has(RubyLspHover(*doc, l, c).text, "@y: Float"));
    // Completion after `p1.`: Point's methods (not initialize), with docs.
    const int last = static_cast<int>(L(src).size()) - 1;
    auto items = RubyLspCompletions(*doc, last, 3);
    CHECK(Item(items, "dist") && Has(Item(items, "dist")->documentation, "Distance to"));
    CHECK(Item(items, "dist")->detail == "dist(other) -> Float  (Point)");
    CHECK(Item(items, "x") && !Item(items, "initialize"));
    CHECK(Item(items, "frozen?"));  // Object's methods too
    // Bare completion: locals, methods of self, constants, keywords.
    const std::string body = src.substr(0, src.size() - 3);
    items = RubyLspCompletions(*RubyLspAnalyze(L(body + "Po")), last, 2);
    CHECK(Item(items, "Point") && Item(items, "Point")->kind == RubyLspKind::Class);
    items = RubyLspCompletions(*RubyLspAnalyze(L(body + "n")), last, 1);
    CHECK(Item(items, "names") && Item(items, "names")->detail == "Array<String>");
    CHECK(Item(items, "next") && Item(items, "next")->kind == RubyLspKind::Keyword);
    CHECK(!Item(items, "n"));  // the block parameter is out of scope here
    // Definition of a method and of a local.
    std::tie(l, c) = Pos(src, "dist(Point", 0, 1);
    auto defs = RubyLspDefinition(*doc, l, c);
    CHECK(defs.size() == 1 && defs[0].path.empty() && defs[0].range.line == 16);
    std::tie(l, c) = Pos(src, "dx * dx");
    defs = RubyLspDefinition(*doc, l, c);
    CHECK(defs.size() == 1 && defs[0].range.line == 17 && defs[0].range.col == 4);
    // References and rename of a local.
    std::tie(l, c) = Pos(src, "dx = other");
    const auto refs = RubyLspReferences(*doc, l, c);
    CHECK(refs.size() == 3 && refs[0].is_definition && !refs[1].is_definition);
    std::string err;
    const auto edits = RubyLspRename(*doc, l, c, "delta", &err);
    CHECK(err.empty() && edits.size() == 3 && edits[2].new_text == "delta");
    RubyLspRename(*doc, l, c, "Bad", &err);
    CHECK(!err.empty());
    err.clear();
    std::tie(l, c) = Pos(src, "dist(Point", 0, 1);
    RubyLspRename(*doc, l, c, "x", &err);
    CHECK(!err.empty());  // methods are not renamed
    CHECK(!RubyLspPrepareRename(*doc, l, c).found);
    // References of a method: every call and the def.
    CHECK(RubyLspReferences(*doc, l, c).size() == 2);
}

void TestSignatureHelpLocal() {
    const std::string src =
        "# Greets.\n"
        "# @param name [String] who\n"
        "# @param punct [String] the ending\n"
        "# @param loud [Boolean] shout\n"
        "def greet(name, punct = '!', loud: false)\n"
        "end\n"
        "greet('bob', ";
    const auto doc = RubyLspAnalyze(L(src));
    auto sh = RubyLspSignatureAt(*doc, 6, 13);
    CHECK(sh.found && sh.signatures.size() == 1);
    CHECK(sh.signatures[0].label == "greet(name, punct = '!', loud: false)");
    CHECK(sh.active_param == 1 && sh.signatures[0].params.size() == 3);
    CHECK(sh.signatures[0].params[1].label == "punct = '!'" && Has(sh.signatures[0].params[1].documentation, "the ending"));
    CHECK(Has(sh.signatures[0].documentation, "Greets."));
    // A keyword argument selects its parameter.
    sh = RubyLspSignatureAt(*RubyLspAnalyze(L(src + "loud: ")), 6, 19);
    CHECK(sh.found && sh.active_param == 2);
    // Not inside a call: nothing; a def's own parameter list is not a call.
    CHECK(!RubyLspSignatureAt(*RubyLspAnalyze(L("x = (1 + ")), 0, 9).found);
    CHECK(!RubyLspSignatureAt(*RubyLspAnalyze(L("def greet(na")), 0, 12).found);
    // Strings with parentheses and commas do not confuse it.
    sh = RubyLspSignatureAt(*RubyLspAnalyze(L(src + "'a, (b'")), 6, 20);
    CHECK(sh.found && sh.active_param == 1);
    // Builtins: Kernel#format.
    sh = RubyLspSignatureAt(*RubyLspAnalyze(L("format('%d', ")), 0, 13);
    CHECK(sh.found && sh.signatures[0].label == "format(format_string, *args)" && sh.active_param == 1);
}

// --- The datamunge stub through the load path -------------------------------------------------

void TestLoadPathStub() {
    const std::string example = kFixture + "/examples/basic.rb";
    const std::string src = ReadFile(example);
    // `# CMD: ruby -I ../lib` finds lib/datamunge.rb (so does the ancestors' lib/ search).
    const auto doc = RubyLspAnalyze(L(src), example);
    CHECK(RubyLspDiagnostics(*doc).empty());
    // Hover: a class method with keywords, its YARD docs, and where it is.
    auto [l, c] = Pos(src, "read_csv");
    auto h = RubyLspHover(*doc, l, c);
    CHECK(h.found && Has(h.text, "Datamunge::Frame.read_csv(path, sep: \",\") -> Frame"));
    CHECK(Has(h.text, "Reads a CSV file.") && Has(h.text, "**Parameters**") && Has(h.text, "`path` (`String`)"));
    CHECK(Has(h.text, "**Raises** `IOError`") && Has(h.text, "datamunge.rb:"));
    // The local's type comes from @return; the overloaded method shows both signatures.
    std::tie(l, c) = Pos(src, "frame.column", 0, 7);
    h = RubyLspHover(*doc, l, c);
    CHECK(h.found && Has(h.text, "Datamunge::Frame#column(index) -> Column"));
    CHECK(Has(h.text, "Datamunge::Frame#column(name, default = nil) -> Column") && Has(h.text, "By position."));
    // `col` is a Column (resolved inside the Datamunge namespace), defined in another file.
    std::tie(l, c) = Pos(src, "col.sum", 0, 4);
    h = RubyLspHover(*doc, l, c);
    CHECK(h.found && Has(h.text, "Datamunge::Column#sum -> Float") && Has(h.text, "column.rb:"));
    // A module method and the namespaces.
    std::tie(l, c) = Pos(src, "mean");
    h = RubyLspHover(*doc, l, c);
    CHECK(h.found && Has(h.text, "Datamunge.mean(values, skip_nan: false) -> Float") &&
          Has(h.text, "**Raises** `ArgumentError` — if +values+ is empty"));
    std::tie(l, c) = Pos(src, "Datamunge::Frame", 0, 12);
    h = RubyLspHover(*doc, l, c);
    CHECK(h.found && Has(h.text, "class Datamunge::Frame < NativeObject") && Has(h.text, "A column-oriented data frame."));
    std::tie(l, c) = Pos(src, "Datamunge::Frame", 0, 2);
    CHECK(Has(RubyLspHover(*doc, l, c).text, "Data frames and statistics."));
    // The require: the file it loads.
    std::tie(l, c) = Pos(src, "datamunge'", 0, 2);
    h = RubyLspHover(*doc, l, c);
    CHECK(h.found && Has(h.text, "lib/datamunge.rb"));

    // Definition: into the stub files.
    std::tie(l, c) = Pos(src, "read_csv");
    auto defs = RubyLspDefinition(*doc, l, c);
    CHECK(defs.size() == 1 && Has(defs[0].path, "lib/datamunge.rb") && defs[0].range.line > 0);
    std::tie(l, c) = Pos(src, "col.sum", 0, 4);
    defs = RubyLspDefinition(*doc, l, c);
    CHECK(defs.size() == 1 && Has(defs[0].path, "datamunge/column.rb"));
    std::tie(l, c) = Pos(src, "datamunge'", 0, 2);
    defs = RubyLspDefinition(*doc, l, c);
    CHECK(defs.size() == 1 && Has(defs[0].path, "lib/datamunge.rb") && defs[0].range.line == 0);

    // Completion after `frame.`: Frame's methods with their YARD docs, plus inherited ones.
    const std::string typed = src + "frame.";
    const int last = static_cast<int>(L(typed).size()) - 1;
    auto items = RubyLspCompletions(*RubyLspAnalyze(L(typed), example), last, 6);
    const RubyLspCompletionItem *filter = Item(items, "filter");
    CHECK(filter && filter->kind == RubyLspKind::Method);
    CHECK(filter->detail == "filter(column, value = nil, **options) -> Frame  (Datamunge::Frame)");
    CHECK(Has(filter->documentation, "Keeps the rows") && Has(filter->documentation, "**Options**") &&
          Has(filter->documentation, "`:strict` (`Boolean`, default `false`)") &&
          Has(filter->documentation, "```ruby\nframe.filter(\"x\", 1.0)\n```"));
    CHECK(Item(items, "alive?") && Item(items, "rows") && Item(items, "name") && Item(items, "map"));
    CHECK(!Item(items, "read_csv") && !Item(items, "initialize"));
    // `Datamunge::Frame.`: class methods and new (with initialize's docs).
    items = RubyLspCompletions(*RubyLspAnalyze(L(src + "Datamunge::Frame."), example), last, 17);
    CHECK(Item(items, "read_csv") && Item(items, "new") && Has(Item(items, "new")->documentation, "Creates a frame"));
    CHECK(Has(Item(items, "new")->detail, "new(rows = 0, name = nil)"));
    // `Datamunge::`: the namespace's members and module methods.
    items = RubyLspCompletions(*RubyLspAnalyze(L(src + "Datamunge::"), example), last, 11);
    CHECK(Item(items, "Frame") && Item(items, "Column") && Item(items, "GOLDEN") && Item(items, "VERSION") &&
          Item(items, "mean"));
    CHECK(Has(Item(items, "GOLDEN")->documentation, "The golden ratio."));
    // `require '...'`: files on the load path.
    items = RubyLspCompletions(*RubyLspAnalyze(L("# CMD: ruby -I ../lib\nrequire 'datamunge/c"), example), 1, 20);
    CHECK(Item(items, "column") && items.size() == 1);
    items = RubyLspCompletions(*RubyLspAnalyze(L("# CMD: ruby -I ../lib\nrequire 'datam"), example), 1, 14);
    CHECK(Item(items, "datamunge") && Item(items, "datamunge/"));

    // Signature help: overloads, keywords, `new`, module functions.
    const std::string call = src + "frame.column(\"x\", ";
    auto sh = RubyLspSignatureAt(*RubyLspAnalyze(L(call), example), last, 18);
    CHECK(sh.found && sh.signatures.size() == 2 && sh.signatures[0].label == "column(index)");
    CHECK(sh.signatures[1].label == "column(name, default = nil)" && sh.active_signature == 1 && sh.active_param == 1);
    CHECK(Has(sh.signatures[1].params[1].documentation, "`Column, nil`") && Has(sh.signatures[1].documentation, "By name."));
    const std::string call2 = src + "Datamunge::Frame.read_csv(\"a.csv\", sep: ";
    sh = RubyLspSignatureAt(*RubyLspAnalyze(L(call2), example), last, static_cast<int>(L(call2).back().size()));
    CHECK(sh.found && sh.signatures[0].label == "read_csv(path, sep: \",\")" && sh.active_param == 1);
    const std::string call3 = src + "Datamunge::Frame.new(";
    sh = RubyLspSignatureAt(*RubyLspAnalyze(L(call3), example), last, static_cast<int>(L(call3).back().size()));
    CHECK(sh.found && sh.signatures[0].label == "new(rows = 0, name = nil)" && sh.active_param == 0);
    CHECK(Has(sh.signatures[0].params[0].documentation, "the initial row count"));
    const std::string call4 = src + "Datamunge::Column::Stats.variance([1.0], ";
    sh = RubyLspSignatureAt(*RubyLspAnalyze(L(call4), example), last, static_cast<int>(L(call4).back().size()));
    CHECK(sh.found && sh.signatures[0].label == "variance(values, ddof = 1)" && sh.active_param == 1);

    // Block parameters typed by @yieldparam; Array<Column> elements through `each`.
    const std::string blk = src + "frame.each { |row| row.first }\nframe.columns.each { |cc| cc.sum }\n";
    const auto doc4 = RubyLspAnalyze(L(blk), example);
    std::tie(l, c) = Pos(blk, "row.first");
    CHECK(Has(RubyLspHover(*doc4, l, c).text, "row: Array<Float>"));
    std::tie(l, c) = Pos(blk, "cc.sum", 0, 3);
    CHECK(Has(RubyLspHover(*doc4, l, c).text, "Datamunge::Column#sum"));

    // --doc lookups (the Ruby UI's Help).
    CHECK(Has(RubyLspDocFor(*doc, "Datamunge::Frame#filter"), "Keeps the rows"));
    CHECK(Has(RubyLspDocFor(*doc, "Datamunge::Frame.read_csv"), "Reads a CSV file."));
    CHECK(Has(RubyLspDocFor(*doc, "Datamunge::Frame.new"), "Creates a frame"));
    CHECK(Has(RubyLspDocFor(*doc, "Datamunge::Frame"), "**Methods**") &&
          Has(RubyLspDocFor(*doc, "Datamunge::Frame"), "`#filter`"));
    CHECK(Has(RubyLspDocFor(*doc, "Frame"), "A column-oriented data frame."));
    CHECK(Has(RubyLspDocFor(*doc, "filter"), "Keeps the rows"));
    CHECK(Has(RubyLspDocFor(*doc, "puts"), "standard output"));
    CHECK(Has(RubyLspDocFor(*doc, "Datamunge::GOLDEN"), "The golden ratio."));
    CHECK(RubyLspDocFor(*doc, "Nope::Missing").empty());
}

void TestRubylibAndLoadPath() {
    // RUBYLIB (here the test override) finds the stub from anywhere.
    const std::vector<std::string> dirs{kFixture + "/lib"};
    RubyLspSetLoadPath(&dirs);
    const std::string src = "require 'datamunge'\nf = Datamunge::Frame.new\nf.filter";
    const auto doc = RubyLspAnalyze(L(src), "/tmp/nowhere/script.rb");
    auto h = RubyLspHover(*doc, 2, 4);
    CHECK(h.found && Has(h.text, "Keeps the rows"));
    RubyLspSetLoadPath(nullptr);
    const std::vector<std::string> none;
    RubyLspSetLoadPath(&none);
    // Without it, and without a lib/ nearby, nothing is found (and nothing is reported).
    const auto doc2 = RubyLspAnalyze(L(src), "/tmp/nowhere/script.rb");
    CHECK(!RubyLspHover(*doc2, 2, 4).found);
    CHECK(RubyLspDiagnostics(*doc2).empty());

    // $LOAD_PATH.unshift File.expand_path('../vendor_lib', __dir__) in the file itself.
    const fs::path tmp = fs::temp_directory_path() / ("mep-ruby-lsp-test-" + std::to_string(::getpid()));
    fs::create_directories(tmp / "app" / "bin");
    fs::create_directories(tmp / "app" / "vendor_lib" / "shapes");
    std::ofstream(tmp / "app" / "vendor_lib" / "shapes.rb") << "require 'shapes/circle'\n";
    std::ofstream(tmp / "app" / "vendor_lib" / "shapes" / "circle.rb")
        << "module Shapes\n  # A circle.\n  class Circle\n    # The area.\n    # @return [Float]\n    def area; end\n"
           "  end\nend\n";
    const std::string script =
        "$LOAD_PATH.unshift File.expand_path('../vendor_lib', __dir__)\nrequire 'shapes'\nShapes::Circle.new.area\n";
    const auto doc3 = RubyLspAnalyze(L(script), (tmp / "app" / "bin" / "run.rb").string());
    h = RubyLspHover(*doc3, 2, 21);
    CHECK(h.found && Has(h.text, "Shapes::Circle#area -> Float") && Has(h.text, "The area."));
    // The workspace root's lib dirs: <root>/*/lib.
    fs::create_directories(tmp / "ws" / "ruby" / "lib");
    std::ofstream(tmp / "ws" / "ruby" / "lib" / "wsgem.rb") << "# Workspace gem.\nmodule Wsgem\nend\n";
    RubyLspSetWorkspaceRoot((tmp / "ws").string());
    const auto doc4 = RubyLspAnalyze(L("require 'wsgem'\nWsgem\n"), (tmp / "ws" / "x.rb").string());
    CHECK(Has(RubyLspHover(*doc4, 1, 2).text, "Workspace gem."));
    RubyLspSetWorkspaceRoot(std::string());
    RubyLspSetLoadPath(nullptr);
    fs::remove_all(tmp);
}

void TestYardEdgeCases() {
    const std::string src =
        "# Summary line.\n"
        "#\n"
        "# Longer description with `code`.\n"
        "#\n"
        "# @param [Integer] count how many (types before the name)\n"
        "# @param opts [Hash{Symbol => Object}] options\n"
        "# @option opts [String] :label (\"none\") the label\n"
        "# @yield [item] each item\n"
        "# @yieldparam item [String] the item\n"
        "# @return [Array<String>, nil] the items\n"
        "#   or nil when there are none\n"
        "# @note Slow.\n"
        "# @deprecated Use {#other} instead.\n"
        "# @see #other\n"
        "# @example\n"
        "#   things(2) do |t|\n"
        "#     puts t\n"
        "#   end\n"
        "def things(count, opts = {}); end\n"
        "things(1)\n";
    const auto doc = RubyLspAnalyze(L(src));
    const auto h = RubyLspHover(*doc, 19, 2);
    CHECK(h.found);
    CHECK(Has(h.text, "Summary line.\n\nLonger description with `code`."));
    CHECK(Has(h.text, "`count` (`Integer`) — how many (types before the name)"));
    CHECK(Has(h.text, "`opts` (`Hash{Symbol => Object}`)"));
    CHECK(Has(h.text, "`:label` (`String`, default `\"none\"`) in `opts` — the label"));
    CHECK(Has(h.text, "**Yields** [item] each item") && Has(h.text, "`item` (`String`)"));
    CHECK(Has(h.text, "**Returns** `Array<String>, nil` — the items or nil when there are none"));
    CHECK(Has(h.text, "**Note:** Slow.") && Has(h.text, "**Deprecated.** Use {#other} instead."));
    CHECK(Has(h.text, "```ruby\nthings(2) do |t|\n  puts t\nend\n```"));
    CHECK(Has(h.text, "-> Array<String>, nil"));
    // A comment separated by a blank line is not the doc; magic comments are not either.
    const auto doc2 = RubyLspAnalyze(L("# frozen_string_literal: true\nclass A; end\n# stray\n\ndef b; end\nb\n"));
    CHECK(!Has(RubyLspHover(*doc2, 1, 7).text, "frozen"));
    CHECK(RubyLspHover(*doc2, 5, 0).found && !Has(RubyLspHover(*doc2, 5, 0).text, "stray"));
}

void TestInheritanceAndMixins() {
    const std::string src =
        "module Greeting\n"
        "  # Says hi.\n"
        "  def hi; end\n"
        "end\n"
        "class Animal\n"
        "  # Makes a sound.\n"
        "  def speak; end\n"
        "  # @return [Animal]\n"
        "  def self.create; end\n"
        "end\n"
        "class Dog < Animal\n"
        "  include Greeting\n"
        "  def fetch\n"
        "    speak\n"
        "    hi\n"
        "    self.speak\n"
        "  end\n"
        "end\n"
        "Dog.create.speak\n";
    const auto doc = RubyLspAnalyze(L(src));
    CHECK(Has(RubyLspHover(*doc, 13, 5).text, "Animal#speak") && Has(RubyLspHover(*doc, 13, 5).text, "Makes a sound."));
    CHECK(Has(RubyLspHover(*doc, 14, 4).text, "Says hi."));
    // Class methods are inherited; @return [Animal] types the chain.
    CHECK(Has(RubyLspHover(*doc, 18, 5).text, "Animal.create"));
    CHECK(Has(RubyLspHover(*doc, 18, 12).text, "Animal#speak"));
    // Completion while typing `self.` (the class does not parse yet).
    std::string typed = src;
    typed.replace(typed.find("self.speak"), 10, "self.");
    const auto items = RubyLspCompletions(*RubyLspAnalyze(L(typed)), 15, 9);
    CHECK(Item(items, "fetch") && Item(items, "speak") && Item(items, "hi"));
}

void TestRobustness() {
    // Every query on every prefix of a file must not crash.
    const std::string src = ReadFile(kFixture + "/lib/datamunge.rb") +
                            "x = Datamunge::Frame.new(1)\nx.filter('a', strict: true) { |r| r }\n<<~E\n  h\nE\n";
    for (size_t n = 0; n <= src.size(); n += 7) {
        const auto lines = L(src.substr(0, n));
        const auto doc = RubyLspAnalyze(lines, kFixture + "/lib/datamunge.rb");
        const int last = static_cast<int>(lines.size()) - 1;
        const int col = static_cast<int>(lines.back().size());
        RubyLspDiagnostics(*doc);
        RubyLspCompletions(*doc, last, col);
        RubyLspHover(*doc, last, col);
        RubyLspSignatureAt(*doc, last, col);
        RubyLspDefinition(*doc, last, col);
        RubyLspReferences(*doc, last, col);
        RubyLspSymbols(*doc);
        RubyLspFolds(*doc);
    }
    // Out-of-range positions.
    const auto doc = RubyLspAnalyze(L("x = 1\n"));
    RubyLspHover(*doc, 99, 99);
    RubyLspCompletions(*doc, -1, 0);
    RubyLspSignatureAt(*doc, 5, 5);
    RubyLspDefinition(*doc, 0, 999);
    // Formatting through the server: one edit over the changed lines, or none when formatted.
    std::string err;
    auto edits = RubyLspFormat(L("def f(a,b)\nx=a+b\nend\n"), 100, &err);
    CHECK(err.empty() && edits.size() == 1 && edits[0].new_text == "def f(a, b)\n  x = a + b\n");
    CHECK(edits[0].range.line == 0 && edits[0].range.end_line == 2 && edits[0].range.end_col == 0);
    edits = RubyLspFormat(L("def f(a, b)\n  a\nend\n"), 100, &err);
    CHECK(err.empty() && edits.empty());
    RubyLspFormat(L("def f(\n"), 100, &err);
    CHECK(!err.empty());
}

}  // namespace

int main() {
    TestSyntaxDiagnostics();
    TestRequireRelativeDiagnostic();
    TestSymbols();
    TestLocalAnalysis();
    TestSignatureHelpLocal();
    TestLoadPathStub();
    TestRubylibAndLoadPath();
    TestYardEdgeCases();
    TestInheritanceAndMixins();
    TestRobustness();
    std::printf("ruby_lsp_test: %d checks passed\n", g_checks);
    return 0;
}
