// mep's two readers of org agree: the tree-sitter grammar
// (grammars/tree-sitter-org: highlighting, folds, the structure sidebar)
// and org_doc.cpp (the outline behind the agenda, kanban and org-lsp; the
// link scan behind concealment), over every .org file in the repository.
// Where they disagree, one of them is wrong (plans/TREESITTER_GRAMMAR_PLAN.md
// Phase 6), so this fails and names the line.
//
//   - Headlines: the same rows, levels, priorities and tags. Neither
//     counts a headline-looking line inside a block.
//   - Bracket links: the same `[[...]]` at the same columns, on lines in
//     prose. Not compared: lines inside a block (the grammar keeps a
//     block's contents as text, quote blocks included; org_doc's scan is
//     per line and leaves blocks to its caller), comment lines (org parses
//     no links there; org_doc's scan does not know a line is a comment),
//     and inline =verbatim=/~code~ (literal in org and in org_doc; the
//     grammar has no emphasis objects, so a link inside one is a link).
// CHECK(), never assert(): the Release build strips assert() entirely.
#include "org_doc.h"

#include <tree_sitter/api.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

extern "C" const TSLanguage *tree_sitter_org(void);

namespace {

void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

using Lines = std::vector<std::string>;

struct Headline {
    int level = 0;
    char priority = 0;
    std::vector<std::string> tags;
};

struct Tree {
    std::map<int, Headline> headlines;      // by 0-based row
    std::set<std::pair<int, int>> links;    // (row, column) of each [[
    std::vector<std::pair<int, int>> rows;  // [first, last] rows of each block and comment
    bool error = false;
};

std::string Text(TSNode node, const std::string &src) {
    const uint32_t start = ts_node_start_byte(node);
    return src.substr(start, ts_node_end_byte(node) - start);
}

TSNode Field(TSNode node, const char *name) {
    return ts_node_child_by_field_name(node, name, static_cast<uint32_t>(std::strlen(name)));
}

void Walk(TSNode node, const std::string &src, Tree *tree) {
    const char *type = ts_node_type(node);
    if (ts_node_is_error(node) || ts_node_is_missing(node)) tree->error = true;
    if (!std::strcmp(type, "headline")) {
        Headline h;
        const TSNode stars = Field(node, "stars");
        if (!ts_node_is_null(stars)) h.level = static_cast<int>(ts_node_end_byte(stars) - ts_node_start_byte(stars));
        const TSNode item = Field(node, "item");
        if (!ts_node_is_null(item)) {
            const TSNode priority = Field(item, "priority");
            if (!ts_node_is_null(priority)) h.priority = Text(priority, src)[2];
        }
        const TSNode tags = Field(node, "tags");
        if (!ts_node_is_null(tags)) {
            for (uint32_t i = 0; i < ts_node_named_child_count(tags); i++) {
                h.tags.push_back(Text(ts_node_named_child(tags, i), src));
            }
        }
        tree->headlines[static_cast<int>(ts_node_start_point(node).row)] = h;
    } else if (!std::strcmp(type, "link") || !std::strcmp(type, "link_desc")) {
        const TSPoint p = ts_node_start_point(node);
        tree->links.insert({static_cast<int>(p.row), static_cast<int>(p.column)});
    } else if (!std::strcmp(type, "block") || !std::strcmp(type, "dynamic_block") || !std::strcmp(type, "comment")) {
        tree->rows.push_back({static_cast<int>(ts_node_start_point(node).row),
                              static_cast<int>(ts_node_end_point(node).row)});
    }
    for (uint32_t i = 0; i < ts_node_child_count(node); i++) Walk(ts_node_child(node, i), src, tree);
}

bool ReadFile(const std::filesystem::path &path, std::string *text, Lines *lines) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    *text = ss.str();
    std::istringstream in(*text);
    std::string line;
    while (std::getline(in, line)) lines->push_back(line);
    return true;
}

// Compares the two readers on one document; returns the disagreements.
int CheckAgreement(TSParser *parser, const std::string &name, const std::string &src, const Lines &lines) {
    TSTree *ts = ts_parser_parse_string(parser, nullptr, src.c_str(), static_cast<uint32_t>(src.size()));
    Tree tree;
    Walk(ts_tree_root_node(ts), src, &tree);
    ts_tree_delete(ts);
    int bad = 0;
    auto report = [&](int row, const char *what) {
        if (bad++ < 10) {
            std::fprintf(stderr, "%s:%d: %s: %s\n", name.c_str(), row + 1, what,
                         row < static_cast<int>(lines.size()) ? lines[static_cast<size_t>(row)].c_str() : "");
        }
    };
    if (tree.error) std::fprintf(stderr, "%s: the tree has ERROR/MISSING nodes\n", name.c_str());

    const OrgOutline outline = ParseOrgOutline(lines);
    std::map<int, const OrgHeadline *> org_headlines;
    for (const OrgHeadline &h : outline.headlines) org_headlines[h.line_start] = &h;
    for (const auto &[row, h] : tree.headlines) {
        const auto it = org_headlines.find(row);
        if (it == org_headlines.end()) {
            report(row, "a headline to the grammar, not to ParseOrgOutline");
        } else if (it->second->level != h.level || it->second->priority != h.priority || it->second->tags != h.tags) {
            report(row, "level, priority or tags differ");
        }
    }
    for (const auto &[row, h] : org_headlines) {
        if (!tree.headlines.count(row)) report(row, "a headline to ParseOrgOutline, not to the grammar");
    }

    std::vector<bool> compared(lines.size(), true);
    for (const auto &[first, last] : tree.rows) {
        for (int r = first; r <= last && r < static_cast<int>(lines.size()); r++) compared[static_cast<size_t>(r)] = false;
    }
    const std::vector<OrgLiteralSpan> literal = OrgLiteralSpans(lines);
    auto in_literal = [&](int row, int col) {
        for (const OrgLiteralSpan &l : literal) {
            if (l.row == row + 1 && col + 1 >= l.col_start && col + 1 < l.col_end) return true;
        }
        return false;
    };
    std::set<std::pair<int, int>> org_links;
    for (size_t r = 0; r < lines.size(); r++) {
        if (!compared[r]) continue;
        for (const OrgLinkSpanInfo &link : ScanOrgLinkSpans(lines[r])) {
            if (link.bracketed && !in_literal(static_cast<int>(r), link.col_start)) {
                org_links.insert({static_cast<int>(r), link.col_start});
            }
        }
    }
    for (const auto &link : tree.links) {
        if (compared[static_cast<size_t>(link.first)] && !in_literal(link.first, link.second) && !org_links.count(link)) {
            report(link.first, "a link to the grammar, not to ScanOrgLinkSpans");
        }
    }
    for (const auto &link : org_links) {
        if (!tree.links.count(link)) report(link.first, "a link to ScanOrgLinkSpans, not to the grammar");
    }
    return bad;
}

}  // namespace

int main() {
    TSParser *parser = ts_parser_new();
    CHECK(ts_parser_set_language(parser, tree_sitter_org()));

    // The cases each side once got wrong.
    {
        const Lines doc = {
            "* TODO [#A] Head :a:b:",
            "#+begin_example",
            "* Not a headline [[file:x]]",
            "#+end_example",
            "#+begin_details",
            "#+begin_summary",
            "S",
            "#+end_summary",
            "#+end_details",
            "** Two",
            "See [[https://orgmode.org][Org]] and =[[file:y]]=.",
        };
        std::string src;
        for (const std::string &l : doc) src += l + "\n";
        CHECK(CheckAgreement(parser, "cases", src, doc) == 0);
    }

    int files = 0, bad = 0;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(MEP_SOURCE_DIR)) {
        const std::filesystem::path &path = entry.path();
        if (!entry.is_regular_file() || path.extension() != ".org") continue;
        const std::string rel = std::filesystem::relative(path, MEP_SOURCE_DIR).string();
        if (rel.rfind("build", 0) == 0 || rel.rfind(".git", 0) == 0 || rel.rfind("third_party", 0) == 0) continue;
        std::string src;
        Lines lines;
        CHECK(ReadFile(path, &src, &lines));
        files++;
        bad += CheckAgreement(parser, rel, src, lines);
    }
    std::printf("org_ts_test: %d files, %d disagreements\n", files, bad);
    CHECK(files > 50);
    CHECK(bad == 0);
    ts_parser_delete(parser);
    std::printf("org_ts_test: all checks passed\n");
    return 0;
}
