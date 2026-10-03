// Windowless test for mepml_doc.cpp: the block/inline parser, the editor
// highlight spans, HTML export, and code-result splicing -- plus a parse of
// the repo's own test.mepml reference file, which is meant to exercise
// every construct the language has.
// CHECK(), never assert(): the Release build strips assert() entirely.
#include "mepml_doc.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <fstream>
#include <map>
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
using namespace mepml;

const Inline *FindKind(const std::vector<Inline> &ins, InlineKind k) {
    for (const Inline &x : ins) {
        if (x.kind == k) return &x;
        if (const Inline *c = FindKind(x.children, k)) return c;
    }
    return nullptr;
}

// Keeps every parse alive for the whole run, so FindKind(P(...)) pointers
// never dangle.
const std::vector<Inline> &P(const std::string &s) {
    static std::deque<std::vector<Inline>> keep;
    keep.push_back(ParseInlines(s));
    return keep.back();
}

bool HasSpan(const std::vector<Span> &spans, int line, int cb, int ce, std::uint32_t style, bool markup) {
    for (const Span &s : spans)
        if (s.line == line && s.col_start == cb && s.col_end == ce && (s.style & style) == style && s.markup == markup)
            return true;
    return false;
}

bool ReadFile(const std::string &path, Lines *out) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) out->push_back(line);
    return true;
}
}  // namespace

int main() {
    // --- Values: int, double, string, quoted string.
    {
        CHECK(Value::Parse("10").kind == ValueKind::Int && Value::Parse("10").i == 10);
        CHECK(Value::Parse(" 1.1 ").kind == ValueKind::Double && Value::Parse("1.1").d == 1.1);
        Value s = Value::Parse("\"asdf\\\"x\"");
        CHECK(s.kind == ValueKind::String && s.s == "asdf\"x");
        CHECK(Value::Parse("\"10\"").kind == ValueKind::String);
        CHECK(Value::Parse("value1").kind == ValueKind::String && Value::Parse("value1").s == "value1");
    }

    // --- Inline emphasis: each marker, and their boundary rules.
    {
        struct Case {
            const char *src;
            InlineKind kind;
            const char *inner;
        } cases[] = {
            {"a *bold* b", InlineKind::Bold, "bold"},       {"a ~it~ b", InlineKind::Italic, "it"},
            {"a _u_ b", InlineKind::Underline, "u"},        {"x^2^ b", InlineKind::Superscript, "2"},
            {"H,,2,,O", InlineKind::Subscript, "2"},        {"a <small> b", InlineKind::Small, "small"},
            {"a >big< b", InlineKind::Big, "big"},          {"a |mono| b", InlineKind::Mono, "mono"},
            {"a =hi= b", InlineKind::Highlight, "hi"},      {"a -gone- b", InlineKind::Strike, "gone"},
            {"a +new+ b", InlineKind::Insert, "new"},       {"a !del! b", InlineKind::Delete, "del"},
        };
        for (const Case &c : cases) {
            const std::vector<Inline> &ins = P(c.src);
            const Inline *x = FindKind(ins, c.kind);
            CHECK(x != nullptr);
            CHECK(x->children.size() == 1 && x->children[0].text == c.inner);
        }
        // Not emphasis: identifiers, arithmetic, spaced markers, hyphenated words.
        CHECK(!FindKind(P("snake_case_name"), InlineKind::Underline));
        CHECK(!FindKind(P("1 + 2 + 3"), InlineKind::Insert));
        CHECK(!FindKind(P("a * b * c"), InlineKind::Bold));
        CHECK(!FindKind(P("well-known-thing"), InlineKind::Strike));
        CHECK(!FindKind(P("a == b"), InlineKind::Highlight));
        CHECK(!FindKind(P("if a < b and c > d"), InlineKind::Small));
        // Nesting and multi-line spans.
        const Inline *b = FindKind(P("*bold ~and italic~*"), InlineKind::Bold);
        CHECK(b && FindKind(b->children, InlineKind::Italic));
        CHECK(FindKind(P("!deleted\ntext!."), InlineKind::Delete));
    }

    // --- Verbatim, escapes, math, links, commands, comments.
    {
        const std::vector<Inline> &v = P("`*not bold*` ok");
        CHECK(v[0].kind == InlineKind::Verbatim && v[0].text == "*not bold*");
        const std::vector<Inline> &e = P("\\*lit\\*");
        CHECK(!FindKind(e, InlineKind::Bold) && e[0].text == "*" && e[0].start == 0 && e[0].inner_start == 1);

        const Inline *m = FindKind(P("inline $E=mc^2$ here"), InlineKind::Math);
        CHECK(m && m->text == "E=mc^2");
        CHECK(!FindKind(P("costs $5 and $6"), InlineKind::Math));
        const Inline *m2 = FindKind(P("x \\(\\frac{a}{b}\\)@alttext{More alt text} y"), InlineKind::Math);
        CHECK(m2 && m2->text == "\\frac{a}{b}" && m2->alt == "More alt text");

        const Inline *l = FindKind(P("see [my website|https://yabla.com]."), InlineKind::Link);
        CHECK(l && l->arg == "https://yabla.com" && l->children[0].text == "my website");
        const Inline *l2 = FindKind(P("[https://x.org/a]"), InlineKind::Link);
        CHECK(l2 && l2->arg == "https://x.org/a");
        CHECK(!FindKind(P("an [aside] here"), InlineKind::Link));

        const std::vector<Inline> &f = P("\\f{Helvetica}{\\fs{12}{Helvetica 12}} and \\color{red}{red}");
        const Inline *font = FindKind(f, InlineKind::Font);
        CHECK(font && font->arg == "Helvetica");
        const Inline *fs = FindKind(font->children, InlineKind::FontSize);
        CHECK(fs && fs->arg == "12" && fs->children[0].text == "Helvetica 12");
        const Inline *col = FindKind(f, InlineKind::Color);
        CHECK(col && col->arg == "red");

        int fn = 0;
        std::vector<Inline> n = ParseInlines("a\\fn{one} b\\fn{two}", &fn);
        CHECK(fn == 2);
        const Inline *cite = FindKind(P("\\cite{k1} \\citep{k2}"), InlineKind::Cite);
        CHECK(cite && cite->text == "k1");
        CHECK(FindKind(P("\\citep{k2}"), InlineKind::CiteP)->text == "k2");

        const std::vector<Inline> &c = P("text // hidden *x*\nnext");
        const Inline *cm = FindKind(c, InlineKind::Comment);
        CHECK(cm && cm->text == "hidden *x*");
        CHECK(!FindKind(c, InlineKind::Bold));
        CHECK(!FindKind(P("see https://example.com/a"), InlineKind::Comment));
    }

    // --- Blocks: meta, headings, comments/callouts, code with options/results.
    {
        Lines src = {
            "//? Title: Some title",            // 0
            "//? Option: OptionA=10",           // 1
            "//? Option: OptionC=\"x\"",        // 2
            "",                                 // 3
            "// plain comment",                 // 4
            "// NOTE: a note",                  // 5
            "// that continues",                // 6
            "> Heading *one*",                  // 7
            ">> Two",                           // 8
            ">big< is not a heading",           // 9
            "",                                 // 10
            "//? Option: Option1=value1",       // 11
            "```{bash, Option3=\"value3\"}",    // 12
            "echo \"Hello, world\"",            // 13
            "```",                              // 14
            "// result_begin:",                 // 15
            "// Hello, world",                  // 16
            "// result_end",                    // 17
        };
        Document d = Parse(src);
        CHECK(d.title == "Some title");
        CHECK(d.FindOption("OptionA") && d.FindOption("OptionA")->value.i == 10);
        CHECK(d.FindOption("OptionC")->value.s == "x");
        CHECK(!d.FindOption("Option1"));  // belongs to the code block
        std::map<BlockKind, int> count;
        for (const Block &b : d.blocks) ++count[b.kind];
        CHECK(count[BlockKind::Meta] == 3 && count[BlockKind::Comment] == 1 && count[BlockKind::Callout] == 1);
        CHECK(count[BlockKind::Heading] == 2 && count[BlockKind::Paragraph] == 1 && count[BlockKind::Code] == 1);
        const Block &callout = d.blocks[static_cast<size_t>(d.BlockAtLine(6))];
        CHECK(callout.kind == BlockKind::Callout && callout.keyword == "NOTE" && callout.line_end == 6);
        const Block &h = d.blocks[static_cast<size_t>(d.BlockAtLine(7))];
        CHECK(h.level == 1 && FindKind(h.inlines, InlineKind::Bold));
        CHECK(d.blocks[static_cast<size_t>(d.BlockAtLine(9))].kind == BlockKind::Paragraph);
        const Block &code = d.blocks[static_cast<size_t>(d.BlockAtLine(13))];
        CHECK(code.kind == BlockKind::Code && code.lang == "bash" && code.line_start == 11 && code.line_end == 17);
        CHECK(code.options.size() == 2 && code.options[0].name == "Option1" && code.options[1].value.s == "value3");
        CHECK(code.code == "echo \"Hello, world\"");
        CHECK(code.result_line_start == 15 && code.result_line_end == 17);
    {
        // A callout's body is one run across its lines: maths and emphasis
        // wrapped onto a continuation line (as gq leaves them) still close,
        // and the continuation lines' `//` is not part of what they hold.
        const std::vector<std::string> wrapped = {
            "// EXAMPLE: $a +",
            "// b$ and *one",
            "// two* end.",
        };
        Document cd = Parse(wrapped);
        CHECK(cd.blocks.size() == 1 && cd.blocks[0].kind == BlockKind::Callout && cd.blocks[0].line_end == 2);
        const Inline *math = FindKind(cd.blocks[0].inlines, InlineKind::Math);
        CHECK(math != nullptr && math->text.find('a') != std::string::npos && math->text.find('b') != std::string::npos);
        CHECK(math != nullptr && math->text.find("//") == std::string::npos);
        CHECK(FindKind(cd.blocks[0].inlines, InlineKind::Bold) || FindKind(cd.blocks[0].inlines, InlineKind::Italic));
    }
        CHECK(code.result_lines.size() == 1 && code.result_lines[0] == "Hello, world");

        int first = 0, last = 0;
        ResultsReplaceRange(code, &first, &last);
        CHECK(first == 15 && last == 18);
        Lines r = FormatResults("a\n\nb\n");
        CHECK((r == Lines{"// result_begin:", "// a", "//", "// b", "// result_end"}));
        // Splicing fresh results and re-parsing round-trips the output.
        Lines spliced = src;
        spliced.erase(spliced.begin() + first, spliced.begin() + last);
        spliced.insert(spliced.begin() + first, r.begin(), r.end());
        Document d2 = Parse(spliced);
        const Block &code2 = d2.blocks[static_cast<size_t>(d2.BlockAtLine(13))];
        CHECK((code2.result_lines == Lines{"a", "", "b"}));
        // A block without results inserts right after its closing fence.
        Document d3 = Parse({"```sh", "ls", "```", "after"});
        ResultsReplaceRange(d3.blocks[0], &first, &last);
        CHECK(first == 3 && last == 3 && d3.blocks[0].lang == "sh");
    }

    // --- GitHub-flavoured Markdown tables: outer pipes optional after a
    // header and a delimiter row; pipes in prose or list items are no table.
    {
        Lines src = {
            "Before it.",                  // 0 (a paragraph the header ends)
            "Name | Value",                // 1
            ":-- | --:",                   // 2
            "a \\| b | 1",                 // 3
            "| c | 2 |",                   // 4
            "short",                       // 5 (no pipe: ends the table)
            "",                            // 6
            "prose | with a pipe",         // 7
            "",                            // 8
            "- a | b",                     // 9
            "- | -",                       // 10
            "",                            // 11
            "x | y",                       // 12
            "- | -",                       // 13 (a list item, not a delimiter)
        };
        Document d = Parse(src);
        CHECK(d.blocks.size() >= 5);
        CHECK(d.blocks[0].kind == BlockKind::Paragraph && d.blocks[0].line_end == 0);
        const Block &t = d.blocks[1];
        CHECK(t.kind == BlockKind::Table && t.line_start == 1 && t.line_end == 4 && t.rows_end == 4);
        CHECK(t.header_rows == 1 && t.separator_line == 2 && t.rows.size() == 3);
        CHECK(t.aligns.size() == 2 && t.aligns[0] == Align::Left && t.aligns[1] == Align::Right);
        CHECK(t.rows[1].size() == 2 && t.rows[2].size() == 2);
        const TableCell &esc = t.rows[1][0];
        CHECK(t.text.substr(static_cast<size_t>(esc.start), static_cast<size_t>(esc.end - esc.start)) == "a \\| b");
        CHECK(d.blocks[2].kind == BlockKind::Paragraph && d.blocks[2].line_start == 5);
        CHECK(d.blocks[3].kind == BlockKind::Paragraph && d.blocks[3].line_start == 7);
        CHECK(d.blocks[4].kind == BlockKind::List);
        for (const Block &b : d.blocks) CHECK(b.kind != BlockKind::Table || b.line_start == 1);
        // TableCells: outer pipes optional, escapes and `verbatim` respected.
        CHECK(TableCells("a | b").size() == 2 && TableCells("| a | b |").size() == 2);
        CHECK(TableCells("| a | b").size() == 2 && TableCells("a | `x|y` | c \\| d").size() == 3);
        CHECK(TableCells("no pipe").empty() && TableCells("|").empty());
    }

    // --- results=markdown: output written raw and read as the document's own.
    {
        Lines r = FormatResults("\n\n|a|b|\n|-|-|\n|1|2|\n\n", "markdown");
        CHECK((r == Lines{"// result_begin: markdown", "|a|b|", "|-|-|", "|1|2|", "// result_end"}));
        Document opts = Parse({"```{r, results=markdown}", "x", "```"});
        CHECK(ResultFormatFor(opts.blocks[0]) == "markdown");
        CHECK(ResultFormatFor(Parse({"```{r, results=asis}", "x", "```"}).blocks[0]) == "markdown");
        CHECK(ResultFormatFor(Parse({"```{r, results=md}", "x", "```"}).blocks[0]) == "markdown");
        CHECK(ResultFormatFor(Parse({"```{r, results=html}", "x", "```"}).blocks[0]) == "html");
        CHECK(ResultFormatFor(Parse({"```{r, results=output}", "x", "```"}).blocks[0]).empty());
        Lines src = {
            "```{r, results=markdown}",    // 0
            "kable(x)",                    // 1
            "```",                         // 2
            "// result_begin: md",         // 3
            "| A | B |",                   // 4
            "|--:|:--|",                   // 5
            "| 1 | 2 |",                   // 6
            "// result_end",               // 7
            "@caption{Printed}",           // 8
            "",                            // 9
            "| typed | table |",           // 10
        };
        Document d = Parse(src);
        CHECK(d.blocks.size() == 3);
        const Block &code = d.blocks[0];
        CHECK(code.kind == BlockKind::Code && code.result_format == "markdown");
        CHECK(code.line_end == 3 && code.result_line_start == 3 && code.result_line_end == 7);
        CHECK(code.result_lines.size() == 3 && code.result_lines[0] == "| A | B |");
        const Block &t = d.blocks[1];
        CHECK(t.kind == BlockKind::Table && t.line_start == 4 && t.rows_end == 6 && t.line_end == 8);
        CHECK(t.caption == "Printed" && t.rows.size() == 2);
        // Printed tables are numbered with typed ones.
        const std::vector<std::string> labels = BlockLabels(d);
        CHECK(labels[0].empty() && labels[1] == "Table 1" && labels[2] == "Table 2");
        // Re-running replaces the whole region, table included.
        int first = 0, last = 0;
        ResultsReplaceRange(code, &first, &last);
        CHECK(first == 3 && last == 8);
        // The HTML export draws the table once, as a table.
        const std::string html = ToHtml(d);
        CHECK(html.find("results") == std::string::npos || html.find("<pre class=\"results\">") == std::string::npos);
        CHECK(html.find("Table 1:") != std::string::npos);
        // No diagnostics for a well-formed region; one for an unclosed one.
        CHECK(d.diagnostics.empty());
        Document bad = Parse({"```{r, results=markdown}", "x", "```", "// result_begin: markdown", "| a |"});
        CHECK(!bad.diagnostics.empty());
    }

    // --- What the exports show: the header's Exports:, a block's exports= or echo=.
    {
        Lines src = {
            "//? Exports: results",            // 0
            "```{r}",                          // 1
            "1",                               // 2
            "```",                             // 3
            "```{r, echo=false, file=\"a.png\"}", // 4
            "plot(1)",                         // 5
            "```",                             // 6
            "// result_begin:",                // 7
            "// @image{a.png}",                // 8
            "// result_end",                   // 9
            "```{r, results=markdown, exports=code}", // 10
            "kable(x)",                        // 11
            "```",                             // 12
            "// result_begin: markdown",       // 13
            "| a | b |",                       // 14
            "|---|---|",                       // 15
            "| 1 | 2 |",                       // 16
            "// result_end",                   // 17
            "@caption{Hidden}",                // 18
            "",                                // 19
            "| c | d |",                       // 20
            "@caption{Shown}",                 // 21
            "```{r, exports=both}",            // 22
            "2",                               // 23
            "```",                             // 24
            "```{r, exports=none}",            // 25
            "3",                               // 26
            "```",                             // 27
        };
        Document d = Parse(src);
        auto exports = [&](int line, bool code, bool results) {
            const Block &b = d.blocks[static_cast<size_t>(d.BlockAtLine(line))];
            bool c = false, r = false;
            CodeExports(d, b, &c, &r);
            return c == code && r == results;
        };
        CHECK(exports(2, false, true));   // the header's default
        CHECK(exports(5, false, true));   // echo=false
        CHECK(exports(11, true, false));  // exports=code
        CHECK(exports(23, true, true));   // exports=both
        CHECK(exports(26, false, false)); // exports=none
        const std::vector<bool> hidden = ExportHidden(d);
        const std::vector<std::string> labels = BlockLabels(d);
        int hidden_tables = 0;
        for (size_t i = 0; i < d.blocks.size(); ++i) {
            if (d.blocks[i].kind != BlockKind::Table) continue;
            if (hidden[i]) {
                ++hidden_tables;
                CHECK(labels[i].empty());  // not numbered
            } else {
                CHECK(labels[i] == "Table 1" && d.blocks[i].caption == "Shown");
            }
        }
        CHECK(hidden_tables == 1);
        CHECK(labels[static_cast<size_t>(d.BlockAtLine(5))] == "Figure 1");
        const std::string html = ToHtml(d);
        CHECK(html.find("kable(x)") != std::string::npos && html.find(">1</code>") == std::string::npos);
        CHECK(html.find("plot(1)") == std::string::npos && html.find("a.png") != std::string::npos);
        CHECK(html.find("Hidden") == std::string::npos && html.find("Table 1: ") != std::string::npos);
        CHECK(html.find(">3</code>") == std::string::npos && html.find(">2</code>") != std::string::npos);
    }

    // --- Display math, images, captions, tables, lists, rules, citations.
    {
        Lines src = {
            "$$",                                         // 0
            "  \\int_0^1 x dx",                           // 1
            "$$",                                         // 2
            "@alttext{area}",                             // 3
            "\\[ a^2 \\]",                                // 4
            "",                                           // 5
            "@image{./img.png} // NOTE: optional",        // 6
            "@caption{A *caption*}",                      // 7
            "",                                           // 8
            "| A | B |",                                  // 9
            "| :-- | --: |",                              // 10
            "| 1 | `x|y` |",                              // 11
            "@caption{Tab}",                              // 12
            "",                                           // 13
            "- one",                                      // 14
            "  - [x] done",                               // 15
            "2. two",                                     // 16
            "",                                           // 17
            "-----",                                      // 18
            "@citation{asdf1}{",                          // 19
            "  author = author1,",                        // 20
            "  title = {The title},",                     // 21
            "  year = 1999",                              // 22
            "}",                                          // 23
            "Cite \\cite{asdf1} and \\citep{nope}.",      // 24
            "@bibliography",                         // 25
            "@caption{orphan}",                           // 26
        };
        Document d = Parse(src);
        const Block &m = d.blocks[0];
        CHECK(m.kind == BlockKind::MathBlock && m.code == "\\int_0^1 x dx" && m.alt == "area" && m.line_end == 3);
        const Block &m2 = d.blocks[1];
        CHECK(m2.kind == BlockKind::MathBlock && m2.code == "a^2" && m2.line_start == 4 && m2.line_end == 4);
        const Block &img = d.blocks[2];
        CHECK(img.kind == BlockKind::Image && img.value == "./img.png" && img.caption == "A *caption*");
        CHECK(FindKind(img.caption_inlines, InlineKind::Bold));
        const Block &t = d.blocks[3];
        CHECK(t.kind == BlockKind::Table && t.header_rows == 1 && t.rows.size() == 2 && t.caption == "Tab");
        CHECK(t.aligns.size() == 2 && t.aligns[0] == Align::Left && t.aligns[1] == Align::Right);
        CHECK(t.rows[1].size() == 2 && FindKind(t.rows[1][1].content, InlineKind::Verbatim));
        const Block &l = d.blocks[4];
        CHECK(l.kind == BlockKind::List && l.items.size() == 3);
        CHECK(l.items[1].indent == 2 && l.items[1].checkbox == 1 && l.items[2].ordered && l.items[2].number == 2);
        CHECK(d.blocks[5].kind == BlockKind::Rule);
        CHECK(d.citations.count("asdf1") && d.citations.at("asdf1").fields.at("title") == "The title");
        CHECK(d.citations.at("asdf1").fields.at("year") == "1999");
        CHECK(d.cite_order.size() == 1 && d.cite_order[0] == "asdf1");
        CHECK(CiteLabel(d, "asdf1", false) == "author1 (1999)" && CiteLabel(d, "asdf1", true) == "(author1, 1999)");
        int unknown = 0, orphan = 0;
        for (const Diagnostic &dg : d.diagnostics) {
            if (dg.message.find("nope") != std::string::npos && dg.line == 24) ++unknown;
            if (dg.message.find("must directly follow") != std::string::npos && dg.line == 26) ++orphan;
        }
        CHECK(unknown == 1 && orphan == 1);

        // Highlight: markup is split out so the editor can conceal it.
        std::vector<Span> spans = Highlight(d);
        CHECK(HasSpan(spans, 24, 5, 17, kCite, true));
        CHECK(HasSpan(spans, 18, 0, 5, kRule, true));
        CHECK(HasSpan(spans, 9, 0, 1, kTable, true));
        CHECK(HasSpan(spans, 6, 18, 35, kComment | kCallout, false));

        std::string html = ToHtml(d);
        CHECK(html.find("<figcaption>Figure 1: A <strong>caption</strong></figcaption>") != std::string::npos);
        CHECK(html.find("<th style=\"text-align:left\">A</th>") != std::string::npos);
        CHECK(html.find("<code>x|y</code>") != std::string::npos);
        CHECK(html.find("aria-label=\"area\"") != std::string::npos);
        CHECK(html.find("id=\"cite-asdf1\"") != std::string::npos);
        CHECK(html.find("<input type=\"checkbox\" disabled checked>") != std::string::npos);
        // "- one" then "2. two" at the same depth are two lists, not one.
        CHECK(html.find("</li></ul><ol start=\"2\"><li>two") != std::string::npos);
    }

    // --- Pictures in table cells: a cell of nothing but `@image{path}`.
    {
        Lines src = {
            "| name | @image{a_b_c.png} |",       // 0
            "| x @image{no.png} | @image{} |",   // 1
        };
        Document d = Parse(src);
        CHECK(d.blocks.size() == 1 && d.blocks[0].kind == BlockKind::Table && d.blocks[0].rows.size() == 2);
        const Block &t = d.blocks[0];
        CHECK(t.rows[0][0].image.empty() && t.rows[0][1].image == "a_b_c.png");
        // Text around it, or no path, is an ordinary cell.
        CHECK(t.rows[1][0].image.empty() && t.rows[1][1].image.empty());
        // One directive span over the cell -- no underline markup read out
        // of the path's underscores.
        std::vector<Span> spans = Highlight(d);
        bool directive = false, underline = false;
        for (const Span &sp : spans) {
            if (sp.line != 0) continue;
            if ((sp.style & kDirective) && sp.target == "a_b_c.png" && sp.col_start == 9 && sp.col_end == 26) directive = true;
            if (sp.style & kUnderline) underline = true;
        }
        CHECK(directive && !underline);
        std::string html = ToHtml(d);
        CHECK(html.find("<td><img src=\"a_b_c.png\" alt=\"\"></td>") != std::string::npos);
        CHECK(html.find("<td>x @image{no.png}</td>") != std::string::npos);
    }

    // --- Captions and alt text over several lines: the braces on lines of
    //     their own, the text read as one line, the export unchanged.
    {
        Lines src = {
            "@image{a.png}",                 // 0
            "@caption{",                     // 1
            "  A *long* caption that",       // 2
            "  wraps",                       // 3
            "}",                             // 4
            "@alttext{A plot",               // 5
            "of x}  // note",                // 6
            "",                              // 7
            "$$",                            // 8
            "x^2",                           // 9
            "$$",                            // 10
            "@alttext{",                     // 11
            "Squared",                       // 12
            "}",                             // 13
            "@caption{never closed",         // 14
            "",                              // 15
            "Para.",                         // 16
            "",                              // 17
            "@caption{orphan",               // 18
            "over two}",                     // 19
            "After.",                        // 20
        };
        Document d = Parse(src);
        const Block &img = d.blocks[0];
        CHECK(img.kind == BlockKind::Image && img.line_end == 6);
        CHECK(img.caption == "A *long* caption that wraps");
        CHECK(img.caption_line == 1 && img.caption_line_end == 4 && img.caption_close_col == 0);
        CHECK(FindKind(img.caption_inlines, InlineKind::Bold));
        CHECK(img.alt == "A plot of x" && img.alt_line == 5 && img.alt_line_end == 6 && img.alt_close_col == 4);
        const Block &math = d.blocks[1];
        CHECK(math.kind == BlockKind::MathBlock && math.alt == "Squared" && math.alt_line_end == 13);
        bool unterminated = false, misplaced = false;
        for (const Diagnostic &dg : d.diagnostics) {
            if (dg.line == 14 && dg.message.find("unterminated") != std::string::npos) unterminated = true;
            if (dg.line == 14 && dg.message.find("must directly follow") != std::string::npos) misplaced = true;
        }
        CHECK(unterminated && !misplaced);
        // An orphan still covers its whole group, so its second line is no paragraph.
        bool orphan = false, stray = false;
        for (const Block &b : d.blocks) {
            if (b.kind == BlockKind::Comment && b.line_start == 18 && b.line_end == 19) orphan = true;
            if (b.kind == BlockKind::Paragraph && b.line_start == 19) stray = true;
        }
        CHECK(orphan && !stray);
        // Opener and closing brace are markup; the caption's text is styled.
        bool open = false, close = false, bold = false;
        for (const Span &sp : Highlight(d)) {
            if (sp.line == 1 && sp.markup && sp.replace == "Caption: ") open = true;
            if (sp.line == 4 && sp.markup && sp.col_start == 0 && sp.col_end == 1) close = true;
            if (sp.line == 2 && (sp.style & kBold) && !sp.markup) bold = true;
        }
        CHECK(open && close && bold);
        const std::string html = ToHtml(d);
        CHECK(html.find("<figcaption>Figure 1: A <strong>long</strong> caption that wraps</figcaption>") != std::string::npos);
        CHECK(html.find("alt=\"A plot of x\"") != std::string::npos);
        // Rendered for the editor: wrapped to the width, centred, the label
        // and the caption's own styling carried as spans.
        const std::vector<RenderedLine> cap = RenderCaption(d, img, "Figure 1", 20, true);
        CHECK(cap.size() == 2);
        CHECK(cap[0].text == "  Figure 1: A long" && cap[1].text == " caption that wraps");
        bool label = false, bolded = false;
        for (const RenderedSpan &sp : cap[0].spans) {
            const std::string t = cap[0].text.substr(static_cast<size_t>(sp.col_start),
                                                     static_cast<size_t>(sp.col_end - sp.col_start));
            if (t == "Figure 1:" && (sp.style & kBold) && (sp.style & kDirective)) label = true;
            if (t == "long" && (sp.style & kBold) && (sp.style & kItalic)) bolded = true;
        }
        CHECK(label && bolded);
        for (const RenderedLine &l : cap) CHECK(static_cast<int>(l.text.size()) <= 20);
        const std::vector<RenderedLine> alt = RenderAltText(img.alt, 8, false);
        CHECK(alt.size() == 2 && alt[0].text == "A plot" && alt[1].text == "of x");
        CHECK(alt[0].spans.size() == 1 && (alt[0].spans[0].style & kComment) && alt[0].spans[0].col_end == 6);
    }

    // --- Figures: a code block's `@image{}` result line, shared numbering
    //     with @image figures, tables numbered apart, and the export.
    {
        Lines src = {
            "@image{a.png}",                         // 0
            "@caption{First}",                        // 1
            "```{r, file=\"plot.png\"}",             // 2
            "plot(1:3)",                              // 3
            "```",                                    // 4
            "// result_begin:",                       // 5
            "// [1] done",                            // 6
            "// @image{plot.png}",                    // 7
            "// result_end",                          // 8
            "@caption{Second}",                       // 9
            "",                                       // 10
            "| a |",                                  // 11
            "@caption{Tab}",                          // 12
        };
        Document d = Parse(src);
        const Block &code = d.blocks[1];
        CHECK(code.kind == BlockKind::Code && code.result_images.size() == 1);
        CHECK(code.result_images[0].first == 7 && code.result_images[0].second == "plot.png");
        CHECK(code.caption == "Second" && code.line_end == 9);
        std::string p;
        CHECK(ResultImagePath("  @image{ x/y.png } ", &p) && p == "x/y.png");
        CHECK(!ResultImagePath("@image{}", &p) && !ResultImagePath("text @image{x}", &p));
        std::vector<std::string> labels = BlockLabels(d);
        CHECK(labels[0] == "Figure 1" && labels[1] == "Figure 2" && labels[2] == "Table 1");
        std::vector<Span> spans = Highlight(d);
        bool target = false;
        for (const Span &sp : spans)
            if (sp.line == 7 && sp.target == "plot.png" && (sp.style & kDirective)) target = true;
        CHECK(target);
        std::string html = ToHtml(d);
        CHECK(html.find("<pre class=\"results\">[1] done</pre>") != std::string::npos);
        CHECK(html.find("<img src=\"plot.png\"") != std::string::npos);
        CHECK(html.find("<figcaption>Figure 2: Second</figcaption>") != std::string::npos);
        CHECK(html.find("<caption>Table 1: Tab</caption>") != std::string::npos);
        CHECK(html.find("@image{plot.png}") == std::string::npos);
        // A table scrolls in its own box rather than past the margin; the
        // page is sized for a phone's width.
        CHECK(html.find("<div class=\"table-wrap\"><table><caption>Table 1: Tab</caption>") != std::string::npos);
        CHECK(html.find("<meta name=\"viewport\"") != std::string::npos);
        // Light by default, whatever the reader's system theme.
        CHECK(html.find("prefers-color-scheme") == std::string::npos && html.find("color-scheme: light") != std::string::npos);
        // Several plots from one block: one figure, every figure closed.
        const Document two = Parse({"```r", "x", "```", "// result_begin:", "// @image{a.png}", "// @image{b.png}", "// result_end",
                                    "@caption{Two}"});
        CHECK(two.blocks[0].result_images.size() == 2);
        const std::string html2 = ToHtml(two);
        CHECK(html2.find("<figure><img src=\"a.png\" alt=\"\"><img src=\"b.png\" alt=\"\"><figcaption>") != std::string::npos);
        size_t opened = 0, closed = 0;
        for (size_t at = 0; (at = html2.find("<figure", at)) != std::string::npos; ++at) ++opened;
        for (size_t at = 0; (at = html2.find("</figure>", at)) != std::string::npos; ++at) ++closed;
        CHECK(opened == closed);
        // The HTML importer finds code results by this class's text, so
        // the stylesheet must not spell it.
        CHECK(html2.find("results-html") == std::string::npos);
    }

    // --- \abstract: prose over lines, paragraphs split by blank lines.
    {
        Lines src = {
            "\\abstract(",                         // 0
            "We show *this*",                      // 1
            "- across lines \\fn(a note).",        // 2  (not a list inside an abstract)
            "",                                    // 3
            "  ",                                  // 4
            "Second (bracketed) part.",            // 5
            ") // done",                           // 6
            "After.",                              // 7
            "\\abstract(One line.)",               // 8
            "\\abstract(never closed",             // 9
        };
        Document d = Parse(src);
        const Block &a = d.blocks[0];
        CHECK(a.kind == BlockKind::Abstract && a.line_start == 0 && a.line_end == 6);
        std::vector<std::vector<Inline>> paras = AbstractParagraphs(a);
        CHECK(paras.size() == 2);
        CHECK(InlinePlainText(paras[0]) == "We show this\n- across lines .");
        CHECK(InlinePlainText(paras[1]) == "Second (bracketed) part.");
        CHECK(d.footnote_count == 1);
        CHECK(d.blocks[1].kind == BlockKind::Paragraph && d.blocks[1].line_start == 7);
        CHECK(d.blocks[2].kind == BlockKind::Abstract && AbstractParagraphs(d.blocks[2]).size() == 1);
        CHECK(d.blocks[3].kind == BlockKind::Abstract && d.blocks[3].line_end == 9);
        bool second = false, unclosed = false, trailing = false;
        for (const Diagnostic &dg : d.diagnostics) {
            if (dg.line == 8 && dg.message.find("second") != std::string::npos) second = true;
            if (dg.line == 9 && dg.message.find("never closed") != std::string::npos) unclosed = true;
            if (dg.line == 6) trailing = true;
        }
        CHECK(second && unclosed && !trailing);
        // The editor's spans: the opener reads as a label, the closer goes.
        bool label = false, closer = false;
        for (const Span &sp : Highlight(d)) {
            if (sp.line == 0 && sp.markup && (sp.style & kAbstract) && sp.replace == "Abstract") label = true;
            if (sp.line == 6 && sp.markup && (sp.style & kAbstract) && sp.col_start == 0 && sp.col_end == 1) closer = true;
        }
        CHECK(label && closer);
        const std::string html = ToHtml(d);
        CHECK(html.find("<section class=\"abstract\"><p class=\"abstract-title\">Abstract</p><p>We show <strong>this</strong>") !=
              std::string::npos);
        CHECK(html.find("<p>Second (bracketed) part.</p></section>") != std::string::npos);
    }

    // --- \slide( ... ): markers around ordinary blocks.
    {
        Lines src = {
            "\\slide(",                  // 0
            "> Results",                  // 1
            "- one",                      // 2
            "- two",                      // 3
            ") // end",                   // 4
            "",                           // 5
            "\\slide{",                  // 6
            "Prose that",                 // 7
            "runs on",                    // 8
            "}",                          // 9
            "@slide{",                    // 10
            "  )",                        // 11 (not this slide's bracket: prose)
            "  }",                        // 12
            ")",                          // 13 (closes nothing: prose)
            "\\slide( text here",        // 14
            "\\slide(",                  // 15 (ends the one above, reported)
            "Last",                       // 16
        };
        Document d = Parse(src);
        std::vector<BlockKind> kinds;
        for (const Block &b : d.blocks) kinds.push_back(b.kind);
        const std::vector<BlockKind> want = {
            BlockKind::SlideBegin, BlockKind::Heading, BlockKind::List, BlockKind::SlideEnd,
            BlockKind::SlideBegin, BlockKind::Paragraph, BlockKind::SlideEnd,
            BlockKind::SlideBegin, BlockKind::Paragraph, BlockKind::SlideEnd,
            BlockKind::Paragraph,
            BlockKind::SlideBegin, BlockKind::SlideBegin, BlockKind::Paragraph,
        };
        CHECK(kinds == want);
        CHECK(d.blocks[2].line_end == 3);  // the list stops at the closer
        CHECK(d.blocks[3].line_start == 4 && d.blocks[3].level == 1);
        CHECK(d.blocks[5].line_start == 7 && d.blocks[5].line_end == 8);
        CHECK(d.blocks[8].line_start == 11 && d.blocks[8].line_end == 11);
        CHECK(d.blocks[9].line_start == 12 && d.blocks[9].level == 3);
        CHECK(d.blocks[10].line_start == 13);
        CHECK(d.blocks[11].level == 4 && d.blocks[12].level == 5);
        int never_closed = 0;
        bool trailing = false, comment_ok = true;
        for (const Diagnostic &dg : d.diagnostics) {
            if (dg.message.find("never closed") != std::string::npos) {
                CHECK(dg.line == 14 || dg.line == 15);
                ++never_closed;
            }
            if (dg.line == 14 && dg.message.find("content goes on the lines after") != std::string::npos) trailing = true;
            if (dg.line == 4) comment_ok = false;
        }
        CHECK(never_closed == 2 && trailing && comment_ok);

        const std::vector<Slide> slides = Slides(d, static_cast<int>(src.size()));
        CHECK(slides.size() == 5);
        CHECK(slides[0].line_start == 0 && slides[0].line_end == 4 && slides[0].closed && slides[0].title == "Results");
        CHECK(slides[0].first_block == 0 && slides[0].last_block == 4);
        CHECK(slides[1].title.empty() && slides[1].line_end == 9);
        CHECK(slides[3].line_start == 14 && slides[3].line_end == 14 && !slides[3].closed);
        CHECK(slides[4].line_end == 16 && !slides[4].closed && slides[4].last_block == d.blocks.size());

        // The editor's spans: "Slide N" on the opener, the closer's bracket.
        bool label = false, closer = false;
        for (const Span &sp : Highlight(d)) {
            if (sp.line == 0 && sp.markup && (sp.style & kSlide) && sp.replace == "Slide 1" && sp.col_end == 7) label = true;
            if (sp.line == 4 && sp.markup && (sp.style & kSlide) && sp.col_start == 0 && sp.col_end == 1) closer = true;
        }
        CHECK(label && closer);

        // HTML: one section per slide, a slide left open closed for it.
        const std::string html = ToHtml(d);
        CHECK(html.find("<section class=\"slide\" id=\"slide-1\">\n<h1 id=\"results\">Results</h1>") != std::string::npos);
        size_t opens = 0, closes = 0;
        for (size_t k = html.find("<section class=\"slide\""); k != std::string::npos; k = html.find("<section class=\"slide\"", k + 1)) ++opens;
        for (size_t k = html.find("</section>"); k != std::string::npos; k = html.find("</section>", k + 1)) ++closes;
        CHECK(opens == 5 && closes == 5);
    }

    // --- Boxes: \definition( ... ) and its kin, markers around ordinary blocks.
    {
        Lines src = {
            "\\definition(Odds and *log*-odds,",                // 0
            "The odds are $\\pi / (1 - \\pi)$.",               // 1
            "",                                                // 2
            "$$",                                              // 3
            "x",                                               // 4
            "$$",                                              // 5
            ") // end",                                        // 6
            "\\theorem(Pythagoras, If $a \\perp b$ then",      // 7
            "the lengths add.",                                // 8
            ")",                                               // 9
            "\\remark(Short, with a comma.)",                  // 10
            "\\proof(",                                       // 11
            "\\lemma(Inner\\, with a comma, text",             // 12
            "that ends here.)",                                // 13
            ")",                                               // 14
            "\\example(All text and no title)",                 // 15
            "\\example(f(x) is fine)",                         // 16
            "\\fact(Never closed,",                           // 17
            "Text.",                                           // 18
        };
        Document d = Parse(src);
        std::vector<BlockKind> kinds;
        for (const Block &b : d.blocks) kinds.push_back(b.kind);
        const std::vector<BlockKind> want = {
            BlockKind::BoxBegin, BlockKind::Paragraph, BlockKind::MathBlock, BlockKind::BoxEnd,
            BlockKind::BoxBegin, BlockKind::BoxEnd,
            BlockKind::BoxBegin,
            BlockKind::BoxBegin, BlockKind::BoxBegin, BlockKind::BoxEnd,
            BlockKind::BoxBegin, BlockKind::BoxBegin,
            BlockKind::BoxBegin, BlockKind::Paragraph,
        };
        CHECK(kinds == want);
        const Block &def = d.blocks[0];
        CHECK(def.keyword == "definition" && def.caption == "Odds and *log*-odds" && def.inlines.empty() && !def.box_closed);
        CHECK(InlinePlainText(def.caption_inlines) == "Odds and log-odds" && def.level == 1);
        CHECK(BoxHeading(def) == "Definition: Odds and log-odds");
        CHECK(d.blocks[3].keyword == "definition" && d.blocks[3].line_start == 6);
        // Text after the title runs on as the box's first paragraph.
        const Block &thm = d.blocks[4];
        CHECK(thm.line_start == 7 && thm.line_end == 8 && thm.caption == "Pythagoras" && !thm.box_closed);
        CHECK(InlinePlainText(thm.inlines) == "If a \\perp b then\nthe lengths add.");
        CHECK(d.blocks[5].line_start == 9);
        // Closed where its text ends.
        CHECK(d.blocks[6].box_closed && d.blocks[6].caption == "Short" && InlinePlainText(d.blocks[6].inlines) == "with a comma.");
        // Nested: the inner one closes at the end of its paragraph, an
        // escaped comma stays in its title.
        CHECK(d.blocks[7].keyword == "proof" && d.blocks[7].caption.empty() && d.blocks[7].level == 1);
        CHECK(d.blocks[8].keyword == "lemma" && d.blocks[8].level == 2 && d.blocks[8].box_closed && d.blocks[8].line_end == 13);
        CHECK(InlinePlainText(d.blocks[8].caption_inlines) == "Inner, with a comma");
        CHECK(InlinePlainText(d.blocks[8].inlines) == "text\nthat ends here.");
        CHECK(d.blocks[9].keyword == "proof" && d.blocks[9].line_start == 14);
        // No comma: one closed on its line is all text, parentheses balance.
        CHECK(d.blocks[10].caption.empty() && InlinePlainText(d.blocks[10].inlines) == "All text and no title");
        CHECK(d.blocks[11].box_closed && InlinePlainText(d.blocks[11].inlines) == "f(x) is fine");
        int never = 0;
        for (const Diagnostic &dg : d.diagnostics) {
            if (dg.message.find("\\fact is never closed") == 0) {
                CHECK(dg.line == 17);
                ++never;
            } else if (dg.severity != Diagnostic::Info) {
                std::fprintf(stderr, "diag %d: %s\n", dg.line + 1, dg.message.c_str());
                CHECK(false);
            }
        }
        CHECK(never == 1);
        // A box's name is built in: no \define may take it, and a call of
        // it mid-paragraph is plain text.
        CHECK(Parse({"\\define(definition(a), x)"}).commands.empty());
        CHECK(Parse({"See \\definition(x) here."}).blocks[0].kind == BlockKind::Paragraph);

        // The editor's spans: the label in place of `\definition(`, the title
        // bold, the comma and the `)` concealed.
        const std::vector<Span> spans = Highlight(d);
        bool label = false, title = false, comma = false, closer = false, run_in = false;
        for (const Span &sp : spans) {
            if (!(sp.style & kBox)) continue;
            if (sp.line == 0 && sp.markup && sp.col_start == 0 && sp.col_end == 12 && sp.replace == "Definition: " && sp.target == "definition") label = true;
            if (sp.line == 0 && !sp.markup && (sp.style & kBold) && sp.col_start == 12) title = true;
            if (sp.line == 0 && sp.markup && sp.col_start == static_cast<int>(src[0].size()) - 1 && sp.replace.empty()) comma = true;
            if (sp.line == 6 && sp.markup && sp.col_start == 0 && sp.col_end == 1) closer = true;
            if (sp.line == 7 && sp.markup && sp.replace == ".") run_in = true;  // the comma before text on its line (its space stays)
        }
        CHECK(label && title && comma && closer && run_in);

        // HTML: a div per box, nested ones inside, the one left open closed.
        const std::string html = ToHtml(d);
        CHECK(html.find("<div class=\"mbox mbox-definition\" data-kind=\"definition\"><p class=\"mbox-title\"><span class=\"mbox-label\">Definition</span> "
                        "<span class=\"mbox-name\">Odds and <strong>log</strong>-odds</span></p>") != std::string::npos);
        CHECK(html.find("<span class=\"mbox-label\">Proof</span></p>\n<div class=\"mbox mbox-lemma\"") != std::string::npos);
        CHECK(html.find(".mbox-theorem { --c: #6a51a3; }") != std::string::npos);
        size_t opens = 0, closes = 0;
        for (size_t k = html.find("<div class=\"mbox "); k != std::string::npos; k = html.find("<div class=\"mbox ", k + 1)) ++opens;
        for (size_t k = html.find("</div>"); k != std::string::npos; k = html.find("</div>", k + 1)) ++closes;
        CHECK(opens == 8 && closes >= 8);

        // On a slide: a box closes before the slide, and one left open is
        // closed by the slide's end.
        const Document sd = Parse({"\\slide(", "\\definition(T,", "x", ")", "\\fact(U,", "y", ")"});
        std::vector<BlockKind> sk;
        for (const Block &b : sd.blocks) sk.push_back(b.kind);
        CHECK((sk == std::vector<BlockKind>{BlockKind::SlideBegin, BlockKind::BoxBegin, BlockKind::Paragraph, BlockKind::BoxEnd,
                                            BlockKind::BoxBegin, BlockKind::Paragraph, BlockKind::BoxEnd}));
        const std::vector<SlideHtml> frags = SlideFragments(sd);
        CHECK(frags.size() == 1 && frags[0].body.find("</div>\n<div class=\"mbox mbox-fact\"") != std::string::npos);

        // Commands expand in a box's title and text; its lines stay.
        const Lines with_cmd = {"\\define(R, $\\mathbb{R}$)", "\\definition(Reals \\R(),", "In \\R().", ")"};
        const std::vector<std::string> ex = ExpandCommands(with_cmd, Parse(with_cmd).commands, {"html"});
        CHECK(ex.size() == 3 && ex[0] == "\\definition(Reals $\\mathbb{R}$," && ex[1] == "In $\\mathbb{R}$." && ex[2] == ")");
    }

    // --- //? Type: presentation, and what its slide exports leave out.
    {
        Document d = Parse({"//? Type: Presentation", "", "Off the slides.", "", "// a comment is fine", "\\slide(", "> T", "On.", ")"});
        CHECK(IsPresentation(d) && MetaValue(d, "TYPE") == "Presentation");
        int off = 0;
        for (const Diagnostic &dg : d.diagnostics) off += dg.message.find("not on any slide") != std::string::npos ? (dg.line == 2 ? 1 : 100) : 0;
        CHECK(off == 1);
        Document none = Parse({"//? Type: slides", "", "Text."});
        CHECK(IsPresentation(none));
        bool empty = false;
        for (const Diagnostic &dg : none.diagnostics) empty = empty || dg.message.find("no \\slide") != std::string::npos;
        CHECK(empty);
        // `//? Export:` names a known format; beamer needs a slide.
        CHECK(Parse({"//? Export: pdf"}).diagnostics.empty() && Parse({"//? Export: .HTML"}).diagnostics.empty());
        const Document bad_export = Parse({"//? Export: pfd"});
        CHECK(bad_export.diagnostics.size() == 1 && bad_export.diagnostics[0].message.find("unknown export format 'pfd'") == 0);
        const Document no_deck = Parse({"//? Export: beamer", "", "Text."});
        CHECK(no_deck.diagnostics.size() == 1 && no_deck.diagnostics[0].message.find("a Beamer export with no") == 0);
        CHECK(Parse({"//? Export: beamer", "\\slide(", "One.", ")"}).diagnostics.empty());
        Document odd = Parse({"//? Type: poster"});
        CHECK(!IsPresentation(odd) && odd.diagnostics.size() == 1 && odd.diagnostics[0].message.find("unknown document type") == 0);
        CHECK(Parse({"//? Type: document", "", "Text."}).diagnostics.empty());
        // The slideshow: a title slide from the header, then the slides.
        Document deck = Parse({"//? Title: Deck", "//? Type: presentation", "\\slide(", "> One", "Text.", ")", "\\slide(", "Two.", ")"});
        const std::string html = ToSlidesHtml(deck);
        CHECK(html.find("<section class=\"slide title-slide\" id=\"slide-1\"><h1>Deck</h1>") != std::string::npos);
        CHECK(html.find("id=\"slide-2\"><h2 class=\"slide-title\">One</h2>") != std::string::npos);
        CHECK(html.find("id=\"slide-3\"><div class=\"slide-body\">") != std::string::npos);
        // Previous/next buttons in the bottom strip, and h/l to step.
        CHECK(html.find("<nav class=\"nav\"><button class=\"prev\"") != std::string::npos);
        CHECK(html.find("k === 'l'") != std::string::npos && html.find("k === 'h'") != std::string::npos);
        const std::vector<SlideHtml> frags = SlideFragments(deck);
        CHECK(frags.size() == 2 && frags[0].title == "One" && frags[0].body == "<p>Text.</p>\n" && frags[1].title.empty());
    }

    // --- The presentation view's pages (PresentationPages).
    {
        const std::map<std::string, Lines> files = {
            {"/d/style.mepml",
             {"\\define(claim(name, body), \\when(html, \\raw(html, <b>#name</b>) #body)\\otherwise(*#name.* #body))",
              "\\citation(knuth, author = Donald Knuth, title = Literate Programming, year = 1984)"}},
        };
        ReadFileFn read = [&](const std::string &p, Lines *out) {
            auto it = files.find(p);
            if (it == files.end()) return false;
            *out = it->second;
            return true;
        };
        Lines src = {
            "//? Title: Deck",                        // 0
            "//? Subtitle: A *test*",                 // 1
            "//? Author: Ada",                        // 2
            "//? Type: presentation",                 // 3
            "",                                       // 4
            "\\import(style.mepml)",                  // 5
            "",                                       // 6
            "\\slide(",                               // 7
            "> First",                                // 8
            "// a note to self",                      // 9
            "",                                       // 10
            "",                                       // 11
            "\\claim(Big, Things grow) as \\citep(knuth).",  // 12
            "",                                       // 13
            "```{r, fig=1}",                          // 14
            "plot(1)",                                // 15
            "```",                                    // 16
            "// result_begin:",                       // 17
            "// [1] 42",                              // 18
            "// \\image(a.png)",                      // 19
            "// result_end",                          // 20
            "\\caption(A plot)",                      // 21
            ")",                                      // 22
            "\\slide(",                               // 23
            "```{python, exports=both}",              // 24
            "print(1)",                               // 25
            "```",                                    // 26
            "// result_begin:",                       // 27
            "// 1",                                   // 28
            "// result_end",                          // 29
            "",                                       // 30
            "```{r}",                                 // 31
            "never_run()",                            // 32
            "```",                                    // 33
            "",                                       // 34
            "```{r, exports=none}",                   // 35
            "hidden()",                               // 36
            "```",                                    // 37
            "\\bibliography",                         // 38
            ")",                                      // 39
        };
        const std::vector<PresentationPage> pages = PresentationPages("/d/main.mepml", src, read);
        CHECK(pages.size() == 3);
        if (pages.size() == 3) {
            // The title page, from the header.
            CHECK(pages[0].number == 0 && pages[0].source_line == -1 && pages[0].title == "Deck");
            CHECK((pages[0].lines == Lines{"> Deck", "", ">A \\*test\\*<", "", "Ada"}));
            // Commands expanded (\otherwise: not html), citations rendered,
            // comments and code options gone, results only by default.
            CHECK(pages[1].number == 1 && pages[1].source_line == 7 && pages[1].source_end == 22 && pages[1].title == "First");
            CHECK((pages[1].lines == Lines{"> First", "", "*Big.* Things grow as (Knuth, 1984).", "", "```", "[1] 42",
                                           "```", "", "\\image(a.png)", "\\caption(A plot)"}));
            // exports=both keeps both; a block never run shows its code;
            // exports=none nothing; the bibliography is its entries.
            CHECK(pages[2].number == 2 && pages[2].source_line == 23 && pages[2].source_end == 39 && pages[2].title.empty());
            const Lines want = {"```python", "print(1)", "```", "", "```", "1", "```", "", "```r", "never_run()", "```", "",
                                "1. Donald Knuth (1984). ~Literate Programming~."};
            CHECK(pages[2].lines == want);
            // The code blocks each page shows, to run from it: the fence's
            // line on the page and in the document (results-only code has
            // none, nor does exports=none).
            CHECK(pages[1].code_blocks.empty());
            CHECK((pages[2].code_blocks == std::vector<std::pair<int, int>>{{0, 24}, {8, 31}}));
            // Every page parses clean as mepml of its own.
            for (const PresentationPage &pg : pages) {
                const Document d = Parse(pg.lines);
                for (const Diagnostic &dg : d.diagnostics)
                    std::fprintf(stderr, "page %d: line %d: %s\n", pg.number, dg.line, dg.message.c_str());
                CHECK(d.diagnostics.empty());
            }
        }
        // A document header saying Exports: code shows the code instead,
        // and a document that isn't a presentation has no title page.
        const std::vector<PresentationPage> code = PresentationPages(
            "/d/x.mepml", {"//? Title: T", "//? Exports: code", "\\slide(", "```r", "x", "```", "// result_begin:", "// 1", "// result_end", ")"}, read);
        CHECK(code.size() == 1 && (code[0].lines == Lines{"```r", "x", "```"}));
    }

    // --- Live output (results=web): the page in an HTML export, the slide's
    // block extents and its running window's fence.
    {
        const Lines src = {
            "//? Type: presentation",                       // 0
            "//? Exports: results",                         // 1
            "\\slide(",                                     // 2
            "```{html, results=web, rows=10}",              // 3
            "<p id=\"x\">d3 goes here</p>",                 // 4
            "```",                                          // 5
            "// result_begin:",                             // 6
            "// \\image(shot.png)",                         // 7
            "// result_end",                                // 8
            ")",                                            // 9
            "\\slide(",                                     // 10
            "```{r, results=web}",                          // 11
            "shiny::runApp()",                              // 12
            "```",                                          // 13
            "// result_begin: gui",                         // 14
            "// [running] r program",                       // 15
            "// result_end",                                // 16
            "",                                             // 17
            "```{r, results=web}",                          // 18
            "never_run()",                                  // 19
            "```",                                          // 20
            ")",                                            // 21
        };
        const Document d = Parse(src);
        const std::string html = ToHtml(d);
        CHECK(html.find("<iframe class=\"web-page\" srcdoc=\"&lt;p id=&quot;x&quot;&gt;d3 goes here&lt;/p&gt;\" style=\"width:100%;height:240px") !=
              std::string::npos);
        CHECK(html.find("shot.png") == std::string::npos);  // the page, not its picture
        // A Beamer deck (made from this HTML) takes the picture instead.
        Document beamer = d;
        beamer.export_tags = {"beamer", "tex", "latex", "slides"};
        CHECK(ToHtml(beamer).find("<iframe") == std::string::npos && ToHtml(beamer).find("shot.png") != std::string::npos);

        const std::vector<PresentationPage> pages =
            PresentationPages("/d/x.mepml", src, [](const std::string &, Lines *) { return false; });
        CHECK(pages.size() == 2);
        if (pages.size() == 2) {
            CHECK((pages[0].lines == Lines{"\\image(shot.png)"}));
            CHECK(pages[0].blocks.size() == 1 && pages[0].blocks[0].first == 0 && pages[0].blocks[0].last == 0 &&
                  pages[0].blocks[0].source_fence == 3 && !pages[0].blocks[0].code && pages[0].blocks[0].live &&
                  pages[0].blocks[0].live_fence == -1);
            // Running: its window goes under the output fence. Never run: a
            // line saying how to start it.
            CHECK((pages[1].lines == Lines{"```", "[running] r program", "```", "", "```", "(not running -- C-c C-c starts it)", "```"}));
            CHECK(pages[1].blocks.size() == 2 && pages[1].blocks[0].live_fence == 0 && pages[1].blocks[0].source_fence == 11 &&
                  pages[1].blocks[1].first == 4 && pages[1].blocks[1].last == 6 && pages[1].blocks[1].source_fence == 18);
        }
    }

    // --- FillProse: fills by the width drawn, not the bytes written.
    {
        CHECK((FillProse("one two three four", 9) == Lines{"one two", "three", "four"}));
        // Markup is concealed: `*two*` is three columns wide, not five.
        CHECK((FillProse("one *two* six", 10) == Lines{"one *two*", "six"}));
        CHECK((FillProse("one *two* six", 11) == Lines{"one *two* six"}));
        CHECK((FillProse("one *two* x", 11) == Lines{"one *two* x"}));
        // Maths is as wide as it typesets, and never broken.
        CHECK((FillProse("a $\\hat{\\beta}_1 + x$ b", 14) == Lines{"a $\\hat{\\beta}_1 + x$ b"}));
        CHECK((FillProse("a $\\hat{\\beta}_1 + x$ b", 8) == Lines{"a", "$\\hat{\\beta}_1 + x$", "b"}));
        CHECK((FillProse("aaaa bbbb $x + y$ c", 10) == Lines{"aaaa bbbb", "$x + y$ c"}));
        // A list item's marker leads, the indent under it follows; an
        // overlong word runs over rather than being cut.
        CHECK((FillProse("alpha beta gamma", 10, "- ", "  ") == Lines{"- alpha", "  beta", "  gamma"}));
        CHECK((FillProse("abcdefghijklmnop q", 10) == Lines{"abcdefghijklmnop", "q"}));
        CHECK((FillProse("", 10, "- ") == Lines{"- "}));
        // Pages filled to a width: the source's line breaks and comments go.
        const std::vector<PresentationPage> pages = PresentationPages(
            "/d/x.mepml",
            {"\\slide(", "> Title", "Some prose that was", "broken by hand. // a comment", "", "- item one that is",
             "  long", "- two", ")"},
            [](const std::string &, Lines *) { return false; }, 20);
        CHECK(pages.size() == 1 &&
              (pages[0].lines == Lines{"> Title", "", "Some prose that was", "broken by hand.", "", "- item one that is",
                                       "  long", "- two"}));
        const std::vector<PresentationPage> wide = PresentationPages(
            "/d/x.mepml", {"\\slide(", "Some prose that was", "broken by hand.", ")"},
            [](const std::string &, Lines *) { return false; }, 80);
        CHECK(wide.size() == 1 && (wide[0].lines == Lines{"Some prose that was broken by hand."}));
        // Plain header text stays as written; a paragraph with maths over
        // several lines keeps its lines.
        const std::vector<PresentationPage> plain = PresentationPages(
            "/d/x.mepml",
            {"//? Type: presentation", "//? Title: Fitting, a straight-line model", "\\slide(", "Then", "\\(", "x", "\\)", "end.", ")"},
            [](const std::string &, Lines *) { return false; }, 80);
        CHECK(plain.size() == 2 && (plain[0].lines == Lines{"> Fitting, a straight-line model"}) &&
              (plain[1].lines == Lines{"Then", "\\(", "x", "\\)", "end."}));
    }

    // --- The old @abstract{...} no longer makes an abstract: it says so.
    {
        Document d = Parse({"@abstract{Old.}", "", "text"});
        CHECK(d.blocks[0].kind == BlockKind::Paragraph);
        bool moved = false;
        for (const Diagnostic &dg : d.diagnostics)
            if (dg.line == 0 && dg.message.find("now \\abstract(") != std::string::npos) moved = true;
        CHECK(moved);
    }

    // --- \name(...) directives, the new spelling of @name{...}.
    {
        Lines src = {
            "\\image(./a.png)",                                  // 0
            "\\caption(A (small) caption, with a comma)",        // 1
            "\\alttext(",                                        // 2
            "  Alt, over lines",                                 // 3
            ")",                                                 // 4
            "",                                                  // 5
            "\\import(refs.mepml) // a comment",                 // 6
            "\\toc",                                             // 7
            "\\bibliography",                                    // 8
            "\\citation(k1, author = Ada Lovelace, title = {A, B}, year = 1843)",  // 9
            "\\citation(k2,",                                    // 10
            "  author = Alan Turing,",                           // 11
            "  year = 1936",                                     // 12
            ")",                                                 // 13
            "\\fs(24, big) starts a paragraph \\cite(k1).",      // 14
        };
        Document d = Parse(src);
        CHECK(d.blocks[0].kind == BlockKind::Image && d.blocks[0].value == "./a.png");
        CHECK(d.blocks[0].caption == "A (small) caption, with a comma");
        CHECK(d.blocks[0].alt == "Alt, over lines" && d.blocks[0].line_end == 4);
        CHECK(d.blocks[1].kind == BlockKind::Import && d.blocks[1].value == "refs.mepml");
        CHECK(d.blocks[2].kind == BlockKind::TableOfContents && d.blocks[3].kind == BlockKind::Bibliography);
        CHECK(d.blocks[4].kind == BlockKind::Citation && d.blocks[4].value == "k1");
        CHECK(d.citations.at("k1").fields.at("title") == "A, B");
        CHECK(d.citations.at("k1").fields.at("year") == "1843");
        CHECK(d.blocks[5].kind == BlockKind::Citation && d.blocks[5].line_end == 13);
        CHECK(d.citations.at("k2").fields.at("author") == "Alan Turing");
        CHECK(d.blocks[6].kind == BlockKind::Paragraph && d.blocks[6].line_start == 14);
        CHECK(d.cite_order.size() == 1 && d.cite_order[0] == "k1");
        for (const Diagnostic &dg : d.diagnostics) CHECK(dg.line == 1000);  // none
        // The editor's spans: the directive's `\caption(` opener is markup
        // relabelled, its `)` is markup.
        bool cap_open = false, cap_close = false, image = false;
        for (const Span &sp : Highlight(d)) {
            if (sp.line == 1 && sp.markup && sp.col_start == 0 && sp.col_end == 9 && sp.replace == "Caption: ") cap_open = true;
            if (sp.line == 1 && sp.markup && sp.col_start == 40 && sp.col_end == 41) cap_close = true;
            if (sp.line == 0 && (sp.style & kDirective) && sp.col_start == 0 && sp.col_end == 15) image = true;
        }
        CHECK(cap_open && cap_close && image);
        std::string path;
        CHECK(ResultImagePath("  \\image(out.png) ", &path) && path == "out.png");
        CHECK(ResultImagePath("@image{old.png}", &path) && path == "old.png");
        CHECK(!ResultImagePath("\\image{mixed.png)", &path));
    }

    // --- \name(arg, text) inline commands, and the old \name{arg}{text}.
    {
        Document d = Parse({"a \\f(Helvetica, \\fs(12, b, c)) \\color(#ff0000, *r*) \\fs{9}{old} \\fn(n (1)) \\citep(x)"});
        const std::vector<Inline> &in = d.blocks[0].inlines;
        CHECK(in.size() == 10);
        CHECK(in[1].kind == InlineKind::Font && in[1].arg == "Helvetica");
        CHECK(in[1].children.size() == 1 && in[1].children[0].kind == InlineKind::FontSize);
        CHECK(in[1].children[0].arg == "12" && InlinePlainText(in[1].children[0].children) == "b, c");
        CHECK(in[3].kind == InlineKind::Color && in[3].arg == "#ff0000" && in[3].children[0].kind == InlineKind::Bold);
        CHECK(in[5].kind == InlineKind::FontSize && in[5].arg == "9" && InlinePlainText(in[5].children) == "old");
        CHECK(in[7].kind == InlineKind::Footnote && InlinePlainText(in[7].children) == "n (1)");
        CHECK(in[9].kind == InlineKind::CiteP && in[9].text == "x");
        // No comma: not a two-argument command, so the text stays literal.
        Document lit = Parse({"\\fs(12) and \\) and \\( too"});
        CHECK(lit.blocks[0].inlines.size() >= 1 && lit.blocks[0].inlines[0].kind == InlineKind::Text);
        CHECK(InlinePlainText(lit.blocks[0].inlines) == "\\fs(12) and ) and ( too");
        // An emphasis marker inside a command's parentheses stays inside it.
        Document em = Parse({"*see \\fs(12, a*b) here*"});
        CHECK(em.blocks[0].inlines.size() == 1 && em.blocks[0].inlines[0].kind == InlineKind::Bold);
        // Maths with the new alt text.
        Document m = Parse({"x \\(\\frac{a}{b}\\)\\alttext(a over b) y"});
        CHECK(m.blocks[0].inlines[1].kind == InlineKind::Math && m.blocks[0].inlines[1].alt == "a over b");
    }

    // --- Generated content: table of contents and bibliography.
    {
        Lines src = {
            "> One *bold*",                                   // 0
            ">> Two",                                         // 1
            "See \\citep{b} then \\cite{a}.",                  // 2
            "@citation{a}{author = Ada Lovelace, title = {Notes on the Analytical Engine}, journal = {Taylor}, year = 1843}",
            "@citation{b}{author = Alan Turing, year = 1936}",  // 4
            "@toc",                                            // 5
            "@bibliography",                                   // 6
            "@printbibliography",                              // 7
        };
        Document d = Parse(src);
        CHECK(d.blocks[5].kind == BlockKind::TableOfContents && d.blocks[6].kind == BlockKind::Bibliography);
        CHECK(d.blocks[7].kind == BlockKind::Bibliography);  // the old name still works...
        bool renamed = false;
        for (const Diagnostic &dg : d.diagnostics)
            if (dg.line == 7 && dg.message.find("now \\bibliography") != std::string::npos) renamed = true;
        CHECK(renamed);  // ...with a note

        std::vector<RenderedLine> toc = RenderToc(d, 80);
        CHECK(toc.size() == 3 && toc[0].text == "Contents");
        CHECK(toc[1].text == "  One bold" && toc[1].target_line == 0);
        CHECK(toc[2].text == "    Two" && toc[2].target_line == 1 && toc[2].spans[0].heading_level == 2);

        std::vector<RenderedLine> bib = RenderBibliography(d, 80);
        CHECK(bib[0].text == "References");
        CHECK(bib[1].text == "  [1] Alan Turing (1936).");  // first cited first
        CHECK(bib[2].text.rfind("  [2] Ada Lovelace (1843). Notes on the Analytical Engine, Taylor.", 0) == 0);
        bool italic = false;
        for (const RenderedSpan &sp : bib[2].spans)
            if ((sp.style & kItalic) &&
                bib[2].text.substr(static_cast<size_t>(sp.col_start), static_cast<size_t>(sp.col_end - sp.col_start)) ==
                    "Notes on the Analytical Engine")
                italic = true;
        CHECK(italic);  // the title, and only the title, is italic
        // Narrow: wrapped with a hanging indent under the entry's text.
        std::vector<RenderedLine> narrow = RenderBibliography(d, 30);
        CHECK(narrow.size() > bib.size());
        for (const RenderedLine &l : narrow) CHECK(l.text.size() <= 30);
        CHECK(narrow[3].text.rfind("      ", 0) == 0);
        const BibEntryParts e = BibEntry(d.citations.at("b"));
        CHECK(e.lead == "Alan Turing (1936)." && e.title.empty() && e.rest.empty());
        std::string html = ToHtml(d);
        // The entry, and its fields as data-bib-* attributes (so an HTML
        // import can rebuild the @citation).
        CHECK(html.find("<li id=\"cite-a\" data-key=\"a\"") != std::string::npos);
        CHECK(html.find("data-bib-journal=\"Taylor\"") != std::string::npos);
        CHECK(html.find(">Ada Lovelace (1843). <em>Notes on the Analytical Engine</em>, Taylor.</li>") != std::string::npos);
    }

    // --- Highlight positions across lines for a multi-line inline construct.
    {
        Document d = Parse({"some \\fs{24}{font size", "24} text"});
        std::vector<Span> spans = Highlight(d);
        CHECK(HasSpan(spans, 0, 5, 13, 0, true));   // "\fs{24}{"
        CHECK(HasSpan(spans, 0, 13, 22, 0, false)); // "font size"
        CHECK(HasSpan(spans, 1, 0, 2, 0, false));   // "24"
        CHECK(HasSpan(spans, 1, 2, 3, 0, true));    // "}"
        bool sized = false;
        for (const Span &s : spans)
            if (s.line == 1 && s.col_start == 0 && s.font_size == 24.0f) sized = true;
        CHECK(sized);
    }

    // --- Imports: expansion, bibtex from an imported file, cycles, missing.
    {
        std::map<std::string, Lines> files = {
            {"/d/refs.bib", {"@book{b1,", "  author = {Ada Lovelace and Charles Babbage},", "  year = {1843}", "}"}},
            {"/d/loop.mepml", {"@import{main.mepml}"}},
        };
        ReadFileFn read = [&](const std::string &p, Lines *out) {
            auto it = files.find(p);
            if (it == files.end()) return false;
            *out = it->second;
            return true;
        };
        Lines main_src = {"@import{refs.bib}", "@import{./missing.mepml}", "@import{loop.mepml}", "See \\citep{b1}."};
        Document d = ParseWithImports("/d/main.mepml", main_src, read);
        CHECK(d.citations.count("b1") && CiteLabel(d, "b1", true) == "(Lovelace and Babbage, 1843)");
        int missing = 0, cycle = 0;
        for (const Diagnostic &dg : d.diagnostics) {
            if (dg.message.find("cannot read") != std::string::npos && dg.line == 1) ++missing;
            if (dg.message.find("circular") != std::string::npos && dg.line == 2) ++cycle;
            CHECK(dg.message.find("unknown citation") == std::string::npos);
        }
        // The cycle closes inside loop.mepml, and is reported on the line
        // here that leads into it ("in loop.mepml: circular import ...").
        CHECK(missing == 1 && cycle == 1);
        CHECK(ResolvePath("/a/b/c.mepml", "./x/y.png") == "/a/b/x/y.png");
        CHECK(ResolvePath("/a/b/c.mepml", "/abs") == "/abs");
    }

    // --- Colors.
    {
        std::uint32_t rgb = 0;
        CHECK(ParseColor("red", &rgb) && rgb == 0xe06c75);
        CHECK(ParseColor("#0f0", &rgb) && rgb == 0x00ff00);
        CHECK(ParseColor("#123456", &rgb) && rgb == 0x123456);
        CHECK(!ParseColor("notacolor", &rgb));
    }

    // --- User commands: \define, calls, \when/\otherwise, \raw.
    {
        const Lines src = {
            "\\define(box(title, body),",       // 0
            "\\when(html, <b>#title</b>: #body)",  // 1
            "\\otherwise(#{title}!: #body)",     // 2
            ")",                                 // 3
            "",                                  // 4
            "\\define(today, 29 September 2026)",  // 5
            "",                                  // 6
            "\\box(Note, one, two, three)",      // 7  a block of its own
            "",                                  // 8
            "Written \\today() by \\who(me) \\raw(html, <kbd>k</kbd>).",  // 9
            "\\raw(tex|pdf,",                    // 10
            "\\newpage",                         // 11
            ")",                                 // 12
            "\\box(only)",                       // 13
        };
        const Document d = Parse(src);
        CHECK(d.blocks.size() == 6);
        CHECK(d.blocks[0].kind == BlockKind::Define && d.blocks[0].keyword == "box" && d.blocks[0].line_end == 3);
        CHECK(d.blocks[0].field_order == Lines({"title", "body"}));
        CHECK(d.blocks[0].code == "\\when(html, <b>#title</b>: #body)\n\\otherwise(#{title}!: #body)");
        CHECK(d.blocks[0].code_line_start == 1 && d.blocks[0].code_line_end == 2);
        CHECK(d.commands.size() == 2 && d.commands.at("today").params.empty() && d.commands.at("today").body == "29 September 2026");
        CHECK(d.blocks[2].kind == BlockKind::Command && d.blocks[2].keyword == "box" && d.blocks[2].value == "Note, one, two, three");
        // A paragraph holds inline calls and raw text.
        const Block &para = d.blocks[3];
        CHECK(para.kind == BlockKind::Paragraph);
        const Inline *today = FindKind(para.inlines, InlineKind::Command);
        CHECK(today && today->arg == "today" && today->text.empty());
        const Inline *raw = FindKind(para.inlines, InlineKind::Raw);
        CHECK(raw && raw->arg == "html" && raw->text == "<kbd>k</kbd>");
        CHECK(d.blocks[4].kind == BlockKind::Raw && d.blocks[4].lang == "tex|pdf" && d.blocks[4].code == "\\newpage");
        // An unknown command and a missing argument are flagged; \today() is fine.
        int unknown = 0, missing = 0;
        for (const Diagnostic &g : d.diagnostics) {
            unknown += g.message.find("unknown command \\who") == 0 && g.line == 9;
            missing += g.message.find("missing argument: \\box") == 0 && g.line == 13;
        }
        CHECK(unknown == 1 && missing == 1 && d.diagnostics.size() == 2);
        // Highlighting: the call's name is a directive; the define's body code.
        const std::vector<Span> spans = Highlight(d);
        CHECK(HasSpan(spans, 7, 0, 5, kDirective, false));
        CHECK(HasSpan(spans, 1, 0, 33, kCode, false));

        // Expansion, per export.
        const Lines html = ExpandCommands(src, d.commands, {"html"});
        CHECK(html == Lines({"", "", "<b>Note</b>: one, two, three", "",
                             "Written 29 September 2026 by \\who(me) \\raw(html, <kbd>k</kbd>).", "<b>only</b>:"}));
        const Lines pdf = ExpandCommands(src, d.commands, {"pdf", "tex", "latex"});
        CHECK(pdf == Lines({"", "", "Note!: one, two, three", "", "Written 29 September 2026 by \\who(me).", "\\raw(tex|pdf,",
                            "\\newpage", ")", "only!:"}));
        // Formats: lists, `|`, negation, `*`.
        CHECK(FormatsMatch("html tex", {"tex", "latex"}) && FormatsMatch("html|slides", {"html", "slides"}));
        CHECK(!FormatsMatch("!html", {"html"}) && FormatsMatch("!html", {"md"}) && FormatsMatch("*", {"docx"}));
        CHECK(!FormatsMatch("", {"html"}) && !FormatsMatch("beamer", {"tex", "latex"}));
        // Parameters: #name, #{name}, #1; ## is #; a colour is not a parameter.
        const Lines p = {"\\define(c(x), ## #x #{x}y #1 #1a #2c7fb8 #nope)", "\\c(v)"};
        CHECK(ExpandCommands(p, Parse(p).commands, {"html"}) == Lines({"# v vy v #1a #2c7fb8 #nope"}));
        // Commands call commands; one calling itself stops, with a message.
        const Lines nest = {"\\define(a(x), [\\b(#x)])", "\\define(b(y), <#y>)", "\\a(z)", "", "\\define(loop, \\loop())", "\\loop()"};
        std::vector<std::string> errors;
        const Lines out = ExpandCommands(nest, Parse(nest).commands, {"md"}, &errors);
        CHECK(out.size() >= 1 && out[0] == "[<z>]");
        CHECK(errors.size() == 1 && errors[0].find("64 deep") != std::string::npos);
        // Code, maths and verbatim are left alone.
        const Lines lit = {"\\define(x, X)", "`\\x()` $\\x()$ \\x()", "```", "\\x()", "```"};
        CHECK(ExpandCommands(lit, Parse(lit).commands, {"md"}) == Lines({"`\\x()` $\\x()$ X", "```", "\\x()", "```"}));
        // ParseForExport picks up commands from an import.
        const auto read = [](const std::string &path, Lines *l) {
            if (path != "/d/defs.mepml") return false;
            *l = {"\\define(hi(n), Hello, #n.)"};
            return true;
        };
        const Document ex = ParseForExport("/d/m.mepml", {"//? Import: defs.mepml", "", "\\hi(you)"}, read, {"html"});
        CHECK(ex.export_tags == Lines({"html"}));
        bool hello = false;
        for (const Block &b : ex.blocks) hello = hello || (b.kind == BlockKind::Paragraph && InlinePlainText(b.inlines) == "Hello, you.");
        CHECK(hello);
        // Built-in names cannot be redefined.
        CHECK(Parse({"\\define(image(p), x)"}).commands.empty());
    }

    // --- The reference file parses without surprises.
    {
        Lines src;
        CHECK(ReadFile(MEPML_REFERENCE_FILE, &src));
        // Its options and citations live in the files its header imports
        // (opts.mepml, refs.mepml, beside it).
        Document d = ParseWithImports(MEPML_REFERENCE_FILE, src, [](const std::string &p, Lines *l) { return ReadFile(p, l); });
        CHECK(d.title == "A Test Document");
        CHECK(d.FindOption("asdf") && d.FindOption("asdf")->value.kind == ValueKind::Int);
        CHECK(d.FindOption("fdsa") && d.FindOption("fdsa")->value.kind == ValueKind::Double);
        CHECK(d.FindOption("qwer") && d.FindOption("qwer")->value.kind == ValueKind::String);
        // opts.mepml's content comes right after the header line naming it.
        for (size_t i = 0; i + 1 < d.blocks.size(); ++i) {
            if (d.blocks[i].kind == BlockKind::Meta && d.blocks[i].value == "opts.mepml") {
                CHECK(d.blocks[i + 1].origin.find("opts.mepml") != std::string::npos);
                break;
            }
        }
        std::map<BlockKind, int> count;
        for (const Block &b : d.blocks) ++count[b.kind];
        CHECK(count[BlockKind::Heading] >= 4);
        CHECK(count[BlockKind::Code] >= 4);
        CHECK(count[BlockKind::Image] == 2);
        CHECK(count[BlockKind::Table] == 4);  // two typed, one GFM, one printed by a block
        CHECK(count[BlockKind::MathBlock] == 3);
        CHECK(count[BlockKind::BoxBegin] == 5 && count[BlockKind::BoxEnd] == 3);
        CHECK(count[BlockKind::Citation] == 2);
        CHECK(count[BlockKind::Callout] >= 3);
        CHECK(count[BlockKind::List] >= 1);
        CHECK(count[BlockKind::Bibliography] == 1);
        CHECK(count[BlockKind::Abstract] == 1);
        CHECK(count[BlockKind::SlideBegin] == 2 && count[BlockKind::SlideEnd] == 2);
        CHECK(count[BlockKind::Define] >= 1 && count[BlockKind::Raw] >= 1 && count[BlockKind::Command] >= 1);
        CHECK(d.footnote_count == 1);
        CHECK(d.cite_order.size() == 2);
        // Every inline kind appears somewhere.
        std::map<InlineKind, int> kinds;
        std::function<void(const std::vector<Inline> &)> walk = [&](const std::vector<Inline> &ins) {
            for (const Inline &x : ins) {
                ++kinds[x.kind];
                walk(x.children);
            }
        };
        for (const Block &b : d.blocks) {
            walk(b.inlines);
            for (const ListItem &it : b.items) walk(it.content);
        }
        for (InlineKind k : {InlineKind::Bold, InlineKind::Italic, InlineKind::Underline, InlineKind::Superscript,
                             InlineKind::Subscript, InlineKind::Small, InlineKind::Big, InlineKind::Mono,
                             InlineKind::Highlight, InlineKind::Strike, InlineKind::Insert, InlineKind::Delete,
                             InlineKind::Verbatim, InlineKind::Link, InlineKind::Font, InlineKind::FontSize,
                             InlineKind::Color, InlineKind::Footnote, InlineKind::Cite, InlineKind::CiteP,
                             InlineKind::Math, InlineKind::Comment, InlineKind::Raw, InlineKind::Command}) {
            if (!kinds[k]) std::fprintf(stderr, "reference file lacks inline kind %d\n", static_cast<int>(k));
            CHECK(kinds[k] > 0);
        }
        // Nothing is wrong with it; refs.mepml, imported by the header and
        // again in the body, is included once (a note on the second).
        for (const Diagnostic &dg : d.diagnostics) {
            if (dg.severity == Diagnostic::Info) continue;
            std::fprintf(stderr, "diag %d: %s\n", dg.line + 1, dg.message.c_str());
            CHECK(false);
        }
        std::string html = ToHtml(d);
        CHECK(html.find("<title>A Test Document</title>") != std::string::npos);
        CHECK(html.find("<sub>subscript</sub>") != std::string::npos);
        CHECK(html.find("class=\"callout callout-note\"") != std::string::npos);
    }

    // --- Header imports: `//? Import: file` inherits the file's header and
    // includes its content, in the order the header lists them.
    {
        const std::map<std::string, Lines> files = {
            {"/d/a.mepml", {"//? Option: shared=2", "//? Option: only_a=3", "//? Title: From A", "//? Author: Ada", "Content of a."}},
            {"/d/b.mepml", {"//? Import: c.mepml", "Content of b."}},
            {"/d/c.mepml", {"//? Option: from_c=\"deep\"", "Content of c."}},
            {"/d/loop.mepml", {"//? Import: main.mepml", "Loops."}},
        };
        auto read = [&](const std::string &path, Lines *l) {
            auto it = files.find(path);
            if (it == files.end()) return false;
            *l = it->second;
            return true;
        };
        auto paragraphs = [](const Document &doc) {
            std::vector<std::string> out;
            for (const Block &b : doc.blocks)
                if (b.kind == BlockKind::Paragraph) out.push_back(InlinePlainText(b.inlines));
            return out;
        };
        const Lines main = {"//? Option: shared=1", "//? Import: a.mepml", "//? Import: b.mepml", "", "Own content."};
        const Document d = ParseWithImports("/d/main.mepml", main, read);
        CHECK(d.diagnostics.empty());
        // This document's option wins; the rest are inherited, nested ones too.
        CHECK(d.FindOption("shared")->value.i == 1);
        CHECK(d.FindOption("only_a")->value.i == 3);
        CHECK(d.FindOption("from_c") && d.FindOption("from_c")->value.s == "deep");
        // No title of its own: the import's; other keys fill gaps.
        CHECK(d.title == "From A");
        bool author = false;
        for (const auto &kv : d.meta) author = author || (kv.first == "Author" && kv.second == "Ada");
        CHECK(author);
        // Content in header order (a, then b with c inside it), then its own.
        const std::vector<std::string> paras = paragraphs(d);
        CHECK(paras.size() == 4);
        CHECK(paras[0] == "Content of a." && paras[1] == "Content of c." && paras[2] == "Content of b." && paras[3] == "Own content.");
        // Its own title stays.
        Lines titled = main;
        titled.insert(titled.begin(), "//? Title: Mine");
        CHECK(ParseWithImports("/d/main.mepml", titled, read).title == "Mine");
        // Imported twice: included once, with a note.
        const Document twice = ParseWithImports("/d/main.mepml", {"//? Import: a.mepml", "@import{a.mepml}"}, read);
        CHECK(paragraphs(twice).size() == 1);
        CHECK(twice.diagnostics.size() == 1 && twice.diagnostics[0].severity == Diagnostic::Info && twice.diagnostics[0].line == 1);
        // Missing, circular, empty.
        const Document bad = ParseWithImports("/d/main.mepml", {"//? Import: nope.mepml", "//? Import: loop.mepml", "//? Import:"}, read);
        CHECK(bad.diagnostics.size() == 3);
        CHECK(Parse({"//? Import:"}).diagnostics.size() == 1);
    }

    // --- results=html: the results' kind rides on the opening marker.
    {
        const Lines src = {"```{python, results=html}", "print('<b>x</b>')", "```", "// result_begin: html", "// <b>x</b>",
                           "// result_end", "", "```r", "1", "```", "// result_begin:", "// [1] 1", "// result_end"};
        const Document d = Parse(src);
        CHECK(d.diagnostics.empty());
        CHECK(d.blocks[0].kind == BlockKind::Code && d.blocks[0].result_format == "html");
        CHECK(d.blocks[0].result_lines.size() == 1 && d.blocks[0].result_lines[0] == "<b>x</b>");
        CHECK(ResultFormatFor(d.blocks[0]) == "html");
        CHECK(d.blocks[1].result_format.empty() && ResultFormatFor(d.blocks[1]).empty());
        CHECK(FormatResults("<p>a</p>", "html").front() == "// result_begin: html");
        CHECK(FormatResults("a").front() == "// result_begin:");
        // A terminal's final screen: another results kind on the marker.
        const Document t = Parse({"```exec", "top", "```", "// result_begin: terminal", "// load 0.5", "// result_end"});
        CHECK(t.diagnostics.empty() && t.blocks[0].result_format == "terminal" && t.blocks[0].result_lines.size() == 1);
        // Exported as the page's own markup, not as escaped text.
        const std::string html = ToHtml(d);
        CHECK(html.find("<div class=\"results results-html\">\n<b>x</b>") != std::string::npos);
        CHECK(html.find("&lt;b&gt;") == std::string::npos || html.find("<pre class=\"results\">[1] 1</pre>") != std::string::npos);
        // A whole document becomes its body (with its head's styles).
        const std::string whole = HtmlResultFragment(
            "<!DOCTYPE html><html><head><title>t</title><style>td{color:red}</style></head><body><table></table></body></html>");
        CHECK(whole.find("<style>td{color:red}</style>") != std::string::npos && whole.find("<table></table>") != std::string::npos);
        CHECK(whole.find("<title>") == std::string::npos && whole.find("<body") == std::string::npos);
        CHECK(HtmlResultFragment("<p>just a fragment</p>") == "<p>just a fragment</p>");
    }

    std::printf("mepml_doc_test: all checks passed\n");
    return 0;
}
