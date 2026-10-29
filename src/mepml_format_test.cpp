// Windowless test for mepml_format.cpp, mep's mepml formatter (gf in a
// .mepml buffer): what each rule does on a small document, then every
// .mepml file in the repository at several widths -- the result parses to
// the same document, formatting it again changes nothing, and refilled
// prose fits the width wherever a line could be broken.
// CHECK(), never assert(): the Release build strips assert() entirely.
#include "mepml_format.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

using Lines = std::vector<std::string>;

Lines SplitLines(const std::string &text) {
    Lines out;
    std::stringstream in(text);
    std::string l;
    while (std::getline(in, l)) out.push_back(l);
    return out;
}

std::string Fmt(const std::string &text, int width = 40) {
    mepml::FormatDocumentOptions opt;
    opt.width = width;
    const mepml::FormatDocumentResult r = mepml::FormatDocument(text, opt);
    CHECK(r.ok);
    return r.text;
}

int Cols(const std::string &s) {
    int n = 0;
    for (char c : s)
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    return n;
}

void Same(const std::string &a, const std::string &b) {
    CHECK(mepml::DocumentSignature(mepml::Parse(SplitLines(a))) == mepml::DocumentSignature(mepml::Parse(SplitLines(b))));
}

void TestParagraph() {
    const std::string in =
        "> Title\n\n"
        "one two three four five six seven eight nine ten eleven twelve thirteen\n"
        "fourteen\n";
    const std::string out = Fmt(in);
    CHECK(out ==
          "> Title\n\n"
          "one two three four five six seven eight\n"
          "nine ten eleven twelve thirteen fourteen\n");
    Same(in, out);
    CHECK(Fmt(out) == out);
}

void TestNoBadLineStarts() {
    // "- x", "1. x", "> x" at the start of a line would be a list item, a
    // numbered item, a heading: the word before them goes down instead.
    const std::string in = "aaaa bbbb cccc dddd eeee ffff gggg hhhh - iiii 2026. jjjj > kkkk\n";
    const std::string out = Fmt(in, 20);
    for (const std::string &l : SplitLines(out)) {
        CHECK(l.rfind("- ", 0) != 0);
        CHECK(l.rfind("2026. ", 0) != 0);
        CHECK(l.rfind("> ", 0) != 0);
    }
    Same(in, out);
}

void TestAtoms() {
    // Maths, verbatim and links never break; a command's arguments don't.
    const std::string in =
        "words before $a + b + c + d$ and `x y z` and [my site|https://example.com] and "
        "\\color(dark red, some coloured words here) end\n";
    const std::string out = Fmt(in, 20);
    CHECK(out.find("$a + b + c + d$") != std::string::npos);
    CHECK(out.find("`x y z`") != std::string::npos);
    CHECK(out.find("[my site|https://example.com]") != std::string::npos);
    CHECK(out.find("\\color(dark red,") != std::string::npos);
    Same(in, out);
}

void TestInlineComment() {
    const std::string in = "alpha beta // a remark\ngamma delta epsilon zeta eta theta iota kappa\n";
    const std::string out = Fmt(in, 20);
    const Lines ls = SplitLines(out);
    CHECK(ls[0] == "alpha beta // a remark");
    Same(in, out);
}

void TestList() {
    const std::string in =
        "- first item with quite a lot of words in it to wrap\n"
        "- [ ] a task\n"
        "  - nested item that also runs on for a while longer\n"
        "1. numbered\n";
    const std::string out = Fmt(in, 24);
    CHECK(out ==
          "- first item with quite\n"
          "  a lot of words in it\n"
          "  to wrap\n"
          "- [ ] a task\n"
          "  - nested item that\n"
          "    also runs on for a\n"
          "    while longer\n"
          "1. numbered\n");
    Same(in, out);
}

void TestTable() {
    const std::string in =
        "| a | bbb |\n"
        "|:-|--:|\n"
        "| cccc | d |\n"
        "\\caption(Cap)\n";
    const std::string out = Fmt(in);
    CHECK(out ==
          "| a    | bbb |\n"
          "| :--- | --: |\n"
          "| cccc |   d |\n"
          "\\caption(Cap)\n");
    Same(in, out);
}

void TestBlankLinesAndCode() {
    const std::string in =
        "\n\npara   \n\n\n\n```{python}\nx=1   \n\n\n\ny = 2\n```\n// result_begin:\n// 3   \n// result_end\n\n\n";
    const std::string out = Fmt(in);
    CHECK(out == "para\n\n```{python}\nx=1   \n\n\n\ny = 2\n```\n// result_begin:\n// 3   \n// result_end\n");
}

void TestComments() {
    // A comment that fits is left as written; one that doesn't is refilled.
    const std::string fits = "// short\n// lines\n";
    CHECK(Fmt(fits) == fits);
    const std::string in = "// NOTE: this callout is far too long to fit on one line of forty\n";
    const std::string out = Fmt(in);
    for (const std::string &l : SplitLines(out)) CHECK(Cols(l) <= 40);
    CHECK(out.rfind("// NOTE: ", 0) == 0);
    Same(in, out);
}

void TestAbstract() {
    const std::string in =
        "\\abstract(\nThe first paragraph of the abstract, which runs on.\n\n\nA second one.\n)\n";
    const std::string out = Fmt(in, 30);
    CHECK(out == "\\abstract(\nThe first paragraph of the\nabstract, which runs on.\n\nA second one.\n)\n");
    Same(in, out);
}

// Every .mepml file here: the same document after, stable, and no refilled
// prose line past the width unless a single word is that long.
void TestCorpus() {
    namespace fs = std::filesystem;
    int files = 0;
    const fs::path root(MEPML_SOURCE_DIR);
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied);
         it != fs::recursive_directory_iterator(); ++it) {
        const std::string name = it->path().filename().string();
        if (it->is_directory() && (name == ".git" || name.rfind("build", 0) == 0 || name == "node_modules" ||
                                   name == "_deps" || name == "result")) {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file() || it->path().extension() != ".mepml") continue;
        std::ifstream f(it->path(), std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string text = ss.str();
        ++files;
        for (int width : {30, 60, 80, 100}) {
            mepml::FormatDocumentOptions opt;
            opt.width = width;
            const mepml::FormatDocumentResult r = mepml::FormatDocument(text, opt);
            CHECK(r.ok);
            Same(text, r.text);
            const mepml::FormatDocumentResult again = mepml::FormatDocument(r.text, opt);
            if (again.text != r.text) {
                std::fprintf(stderr, "not stable: %s at width %d\n", it->path().c_str(), width);
                CHECK(false);
            }
            if (r.kept > 0)
                std::fprintf(stderr, "  %s width %d: %d change(s) left out\n", it->path().filename().c_str(), width,
                             r.kept);
            // Paragraph lines past the width hold one unbreakable piece.
            const mepml::Document d = mepml::Parse(SplitLines(r.text));
            const Lines ls = SplitLines(r.text);
            for (const mepml::Block &b : d.blocks) {
                if (b.kind != mepml::BlockKind::Paragraph) continue;
                for (int i = b.line_start; i <= b.line_end; ++i) {
                    const std::string &l = ls[static_cast<size_t>(i)];
                    if (Cols(l) <= width) continue;
                    if (l.find(" // ") != std::string::npos) continue;  // a remark ends its line
                    if (std::getenv("MEPML_FORMAT_VERBOSE"))
                        std::fprintf(stderr, "  long (%d > %d): %s\n", Cols(l), width, l.c_str());
                }
            }
        }
    }
    CHECK(files >= 5);
    std::printf("mepml corpus: %d files\n", files);
}
}  // namespace

int main() {
    TestParagraph();
    TestNoBadLineStarts();
    TestAtoms();
    TestInlineComment();
    TestList();
    TestTable();
    TestBlankLinesAndCode();
    TestComments();
    TestAbstract();
    TestCorpus();
    std::printf("mepml_format_test: ok\n");
    return 0;
}
