// Cross-checks mepml's two parsers against each other: the tree-sitter
// grammar (grammars/tree-sitter-mepml -- highlighting, folds, other
// editors) and mep's own parser (src/mepml_doc.cpp -- rendering, export,
// running code). On test.mepml, the language's reference file, the grammar
// must produce no ERROR/MISSING node, the same blocks at the same rows, and
// the same number of each inline construct. Then a mutation run: the
// external scanner must survive thousands of damaged documents (build
// under the Sanitize config to have ASan/UBSan watch it).
// CHECK(), never assert(): the Release build strips assert() entirely.
#include <tree_sitter/api.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "mepml_doc.h"

extern "C" const TSLanguage *tree_sitter_mepml(void);

namespace {
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

using Lines = std::vector<std::string>;

struct TsNode {
    std::string type;
    int start_row, end_row;  // end_row: last row with content (inclusive)
};

// Every named node, with an inclusive end row (a node ending at column 0
// of row r really ends on row r-1).
void Collect(TSNode n, std::vector<TsNode> &out, bool *has_error) {
    if (ts_node_is_error(n) || ts_node_is_missing(n)) *has_error = true;
    if (ts_node_is_named(n)) {
        TSPoint s = ts_node_start_point(n), e = ts_node_end_point(n);
        int end_row = static_cast<int>(e.row) - (e.column == 0 && e.row > s.row ? 1 : 0);
        out.push_back({ts_node_type(n), static_cast<int>(s.row), end_row});
    }
    for (uint32_t i = 0; i < ts_node_child_count(n); ++i) Collect(ts_node_child(n, i), out, has_error);
}

std::vector<TsNode> ParseTs(TSParser *parser, const std::string &text, bool *has_error) {
    TSTree *tree = ts_parser_parse_string(parser, nullptr, text.c_str(), static_cast<uint32_t>(text.size()));
    std::vector<TsNode> nodes;
    *has_error = false;
    Collect(ts_tree_root_node(tree), nodes, has_error);
    ts_tree_delete(tree);
    return nodes;
}

std::string Join(const Lines &lines) {
    std::string s;
    for (const std::string &l : lines) s += l + "\n";
    return s;
}

bool ReadFile(const std::string &path, Lines *out) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) out->push_back(line);
    return true;
}

// The grammar's node type for one of mepml::Parse's block kinds, and the
// rows the two should agree on.
struct Expect {
    std::string type;
    int start = 0, end = 0;
};

bool Expected(const mepml::Block &b, Expect *e) {
    using mepml::BlockKind;
    e->start = b.line_start;
    e->end = b.line_end;
    switch (b.kind) {
        case BlockKind::Paragraph: e->type = "paragraph"; return true;
        case BlockKind::Heading: e->type = "heading"; return true;
        case BlockKind::Comment: e->type = "comment"; return true;
        case BlockKind::Callout: e->type = "callout"; return true;
        case BlockKind::Meta: e->type = "meta"; return true;
        case BlockKind::Import: e->type = "import"; return true;
        case BlockKind::Citation: e->type = b.keyword.empty() ? "citation" : "bibtex_entry"; return true;
        case BlockKind::MathBlock: e->type = "display_math"; return true;
        case BlockKind::Code:
            // mep's parser folds the //? option lines above a fence into the
            // block; the grammar leaves them as their own meta nodes.
            e->type = "code_block";
            e->start = b.code_line_start - 1;
            return true;
        case BlockKind::Image: e->type = "image"; return true;
        case BlockKind::Table: e->type = "table"; return true;
        case BlockKind::List: e->type = "list"; return true;
        case BlockKind::Rule: e->type = "rule_line"; return true;
        case BlockKind::Bibliography: e->type = "bibliography"; return true;
        case BlockKind::TableOfContents: e->type = "toc"; return true;
        case BlockKind::Abstract: e->type = "abstract"; return true;
        case BlockKind::SlideBegin: e->type = "slide_open"; return true;
        case BlockKind::SlideEnd: e->type = "slide_close"; return true;
        case BlockKind::Define: e->type = "define"; return true;
        case BlockKind::Raw: e->type = "raw_block"; return true;
        case BlockKind::Command: e->type = "command_block"; return true;
        case BlockKind::BoxBegin: e->type = "box_open"; return true;
        case BlockKind::BoxEnd: e->type = "box_close"; return true;
        // (Columns are boxes to the grammar: they nest and close alike.)
        case BlockKind::LayoutBegin: e->type = "box_open"; return true;
        case BlockKind::LayoutEnd: e->type = "box_close"; return true;
    }
    return false;
}

const std::map<mepml::InlineKind, std::string> kInlineNode = {
    {mepml::InlineKind::Bold, "bold"},           {mepml::InlineKind::Italic, "italic"},
    {mepml::InlineKind::Underline, "underline"}, {mepml::InlineKind::Superscript, "superscript"},
    {mepml::InlineKind::Subscript, "subscript"}, {mepml::InlineKind::Small, "small"},
    {mepml::InlineKind::Big, "big"},             {mepml::InlineKind::Mono, "monospace"},
    {mepml::InlineKind::Highlight, "highlight"}, {mepml::InlineKind::Strike, "strikethrough"},
    {mepml::InlineKind::Insert, "insertion"},    {mepml::InlineKind::Delete, "deletion"},
    {mepml::InlineKind::Verbatim, "verbatim"},   {mepml::InlineKind::Link, "link"},
    {mepml::InlineKind::Font, "font"},           {mepml::InlineKind::FontSize, "font_size"},
    {mepml::InlineKind::Class, "class"},         {mepml::InlineKind::Color, "color"},         {mepml::InlineKind::Footnote, "footnote"},
    {mepml::InlineKind::Cite, "cite"},           {mepml::InlineKind::CiteP, "citep"},
    {mepml::InlineKind::Math, "inline_math"},    {mepml::InlineKind::Comment, "inline_comment"},
    {mepml::InlineKind::Raw, "raw"},             {mepml::InlineKind::Command, "command"},
};

void CountInlines(const std::vector<mepml::Inline> &ins, std::map<std::string, int> &counts) {
    for (const mepml::Inline &x : ins) {
        auto it = kInlineNode.find(x.kind);
        if (it != kInlineNode.end()) counts[it->second]++;
        CountInlines(x.children, counts);
    }
}

// Asserts the two parsers agree on `lines`; prints every disagreement.
void CheckAgreement(TSParser *parser, const Lines &lines, const char *what) {
    bool has_error = false;
    std::vector<TsNode> nodes = ParseTs(parser, Join(lines), &has_error);
    if (has_error) std::fprintf(stderr, "%s: tree has ERROR/MISSING nodes\n", what);
    CHECK(!has_error);

    const mepml::Document doc = mepml::Parse(lines);
    int missing = 0;
    for (const mepml::Block &b : doc.blocks) {
        Expect e;
        if (!Expected(b, &e)) continue;
        bool found = false;
        for (const TsNode &n : nodes)
            if (n.type == e.type && n.start_row == e.start && n.end_row == e.end) found = true;
        if (!found) {
            std::fprintf(stderr, "%s: no %s node at rows %d-%d\n", what, e.type.c_str(), e.start + 1, e.end + 1);
            ++missing;
        }
    }
    CHECK(missing == 0);

    std::map<std::string, int> want, got;
    for (const mepml::Block &b : doc.blocks) {
        CountInlines(b.inlines, want);
        CountInlines(b.caption_inlines, want);
        for (const mepml::ListItem &it : b.items) CountInlines(it.content, want);
        for (const auto &row : b.rows)
            for (const mepml::TableCell &c : row) CountInlines(c.content, want);
    }
    // A directive's trailing `// comment` is an inline_comment to the
    // grammar but trailing text of the directive to mep's parser (which
    // keeps inline comments for prose); only prose ones are compared.
    std::set<int> directive_rows;
    for (const TsNode &n : nodes)
        if (n.type == "image" || n.type == "import" || n.type == "display_math" || n.type == "bibliography" ||
            n.type == "toc" || n.type == "caption" || n.type == "alttext" || n.type == "slide_open" ||
            n.type == "slide_close" || n.type == "box_close")
            directive_rows.insert(n.start_row);
        else if (n.type == "abstract" || n.type == "define" || n.type == "raw_block" || n.type == "command_block")
            directive_rows.insert(n.end_row);  // after its closing bracket
    for (const TsNode &n : nodes)
        if (n.type != "inline_comment" || !directive_rows.count(n.start_row)) got[n.type]++;
    int mismatched = 0;
    for (const auto &kv : kInlineNode) {
        if (want[kv.second] != got[kv.second]) {
            std::fprintf(stderr, "%s: %s: mep parser %d, grammar %d\n", what, kv.second.c_str(), want[kv.second],
                         got[kv.second]);
            ++mismatched;
        }
    }
    CHECK(mismatched == 0);
}
}  // namespace

int main() {
    TSParser *parser = ts_parser_new();
    CHECK(ts_parser_set_language(parser, tree_sitter_mepml()));

    // --- The reference file.
    Lines ref;
    CHECK(ReadFile(MEPML_REFERENCE_FILE, &ref));
    CheckAgreement(parser, ref, "test.mepml");

    // --- Headings nest into sections by depth.
    {
        bool err = false;
        std::vector<TsNode> nodes = ParseTs(parser, "> A\nx\n>> B\ny\n> C\n", &err);
        CHECK(!err);
        std::vector<std::pair<int, int>> sections;
        for (const TsNode &n : nodes)
            if (n.type == "section") sections.emplace_back(n.start_row, n.end_row);
        CHECK((sections == std::vector<std::pair<int, int>>{{0, 3}, {2, 3}, {4, 4}}));
    }

    // --- Slides: every spelling, content of every kind, a stray bracket.
    CheckAgreement(parser,
                   {"> Deck", "", "\\slide(", "> Title", "- one", "- two", ") // end", "", "\\slide{", "Text that", "runs on",
                    "}", "", "@slide{", "```{r}", "x <- 1", "```", "  )", "  }", "after", ")"},
                   "slides");

    // --- A display-maths closer with text after it still closes the block
    // (mepml_doc.cpp warns), and costs no more than one that doesn't: the
    // grammar used to fail the rule there and have error recovery rescan
    // to the next `$$` over and over -- quadratic in the rest of the
    // document (plans/MEPML_PERFORMANCE_PLAN.md). Timed as a growth
    // ratio, so any machine and the Sanitize build give the same answer:
    // linear is ~4x for 4x the text, the old behaviour ~16x.
    CheckAgreement(parser, {"$$", "  x^2", "$$.", "", "After it.", "", "\\[ a \\] and more"},
                   "display maths closer with trailing text");
    {
        auto doc_ms = [&](int sections) {
            std::string text = "> H\n\n$$\n  x^2\n$$ *unclosed \\fn(open $x^2\n\n";
            for (int i = 0; i < sections; ++i)
                text += "Prose with *bold* and $a$ here.\n\n$$\n  \\int_0^1 x\\,dx\n$$\n\\alttext(an integral)\n\n";
            double best = 1e9;
            for (int rep = 0; rep < 3; ++rep) {
                const auto t0 = std::chrono::steady_clock::now();
                TSTree *tree = ts_parser_parse_string(parser, nullptr, text.c_str(), static_cast<uint32_t>(text.size()));
                const auto t1 = std::chrono::steady_clock::now();
                ts_tree_delete(tree);
                best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
            }
            return best;
        };
        const double small = doc_ms(250), large = doc_ms(1000);
        std::printf("trailing-closer growth: %.2f ms -> %.2f ms (%.1fx for 4x the text)\n", small, large, large / small);
        CHECK(large < small * 9.0);
    }

    // --- One very long line of dense markup is linear too: the scanner used
    // to ask for the lexer's column on every call, and tree-sitter answers
    // that by walking back to the line's start.
    {
        auto line_ms = [&](int words) {
            std::string text = "> H\n\n";
            for (int i = 0; i < words; ++i) text += (i % 7 == 0) ? "*b* " : "word ";
            text += "\n";
            double best = 1e9;
            for (int rep = 0; rep < 3; ++rep) {
                const auto t0 = std::chrono::steady_clock::now();
                TSTree *tree = ts_parser_parse_string(parser, nullptr, text.c_str(), static_cast<uint32_t>(text.size()));
                const auto t1 = std::chrono::steady_clock::now();
                ts_tree_delete(tree);
                best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
            }
            return best;
        };
        const double small = line_ms(2000), large = line_ms(8000);
        std::printf("long-line growth: %.2f ms -> %.2f ms (%.1fx for 4x the text)\n", small, large, large / small);
        CHECK(large < small * 9.0);
    }

    // --- User commands, \when and \raw: blocks of their own and inline.
    CheckAgreement(parser,
                   {"\\define(box(title, body),", "\\when(html,", "\\raw(html, <div class=\"box\">(#title))", "", "#body", ")",
                    "\\otherwise(*#title.* #body)", ")", "", "\\define(today, 29 September 2026)", "", "Text before",
                    "\\box(Note, one, *two*,", "", "three) // a call",
                    "after, written \\today() with \\raw(html, <kbd>(k)</kbd>) and \\when(md, x).", "", "\\raw(tex|pdf,",
                    "\\newpage", ")", "", "\\box(inline) not a block", "\\definitionwithaveryveryverylongname(x)"},
                   "commands");

    // --- Mutations: the scanner must neither crash nor hang, whatever the
    //     damage (half-typed markers, unclosed fences, stray braces, ...).
    {
        std::mt19937 rng(424242);
        const std::string base = Join(ref);
        const std::string alphabet = "*~_^,<>|=-+!`$\\{}[]()@/?:# \n\tabcXYZ123";
        for (int iter = 0; iter < 4000; ++iter) {
            std::string text = base;
            const int edits = 1 + static_cast<int>(rng() % 30);
            for (int e = 0; e < edits && !text.empty(); ++e) {
                size_t pos = rng() % text.size();
                switch (rng() % 3) {
                    case 0: text.erase(pos, 1 + rng() % 6); break;
                    case 1: text.insert(pos, 1, alphabet[rng() % alphabet.size()]); break;
                    default: text.insert(pos, text.substr(rng() % text.size(), rng() % 40)); break;
                }
            }
            if (iter % 5 == 0) text.resize(rng() % (text.size() + 1));
            bool err = false;
            ParseTs(parser, text, &err);
        }
    }

    ts_parser_delete(parser);
    std::printf("mepml_ts_test: all checks passed\n");
    return 0;
}
