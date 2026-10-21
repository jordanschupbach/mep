// mep-ts-corpus-test: runs grammars/tree-sitter-<name>/test/corpus/*.txt
// against the compiled-in grammars, as `tree-sitter test` does, so changing
// a grammar needs neither the tree-sitter CLI nor Node
// (plans/TREESITTER_GRAMMAR_PLAN.md Phase 9).
//
//   mep-ts-corpus-test [NAME...]      (default: every grammar)
//
// A corpus file holds cases:
//
//   ==========
//   case name
//   ==========
//   input
//   ---
//   (expected (s-expression))
//
// The tree is compared as `tree-sitter test` compares it: whitespace
// collapsed, and field names dropped from the actual tree when the
// expected one has none.
#include <tree_sitter/api.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#define MEP_TS_GRAMMARS(X) \
    X(bash) X(c) X(commonlisp) X(cpp) X(javascript) X(just) X(lua) X(markdown) X(markdown_inline) X(mepml) X(nix) X(org) X(python) X(r)

#define DECLARE(name) extern "C" const TSLanguage *tree_sitter_##name(void);
MEP_TS_GRAMMARS(DECLARE)
#undef DECLARE

namespace {

struct Grammar {
    const char *name;
    const TSLanguage *(*language)(void);
};

const Grammar kGrammars[] = {
#define ENTRY(name) {#name, tree_sitter_##name},
    MEP_TS_GRAMMARS(ENTRY)
#undef ENTRY
};

struct Case {
    std::string name, input, expected;
    int line = 0;
};

bool IsRule(const std::string &line, char c) {
    return line.size() >= 3 && std::all_of(line.begin(), line.end(), [c](char x) { return x == c; });
}

std::vector<std::string> SplitLines(const std::string &text) {
    std::vector<std::string> lines;
    std::string line;
    std::istringstream in(text);
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

// A case's input runs from its header to its divider; its expected tree
// is everything after that, up to the next header.
std::vector<Case> ParseCorpus(const std::string &text) {
    std::vector<std::string> lines = SplitLines(text);
    std::vector<Case> cases;
    size_t i = 0;
    while (i < lines.size()) {
        if (!(IsRule(lines[i], '=') && i + 2 < lines.size() && IsRule(lines[i + 2], '='))) { ++i; continue; }
        Case c;
        c.name = lines[i + 1];
        c.line = static_cast<int>(i + 2);
        size_t body = i + 3, end = body;
        while (end < lines.size() && !(IsRule(lines[end], '=') && end + 2 < lines.size() && IsRule(lines[end + 2], '='))) ++end;
        // The longest line of dashes divides (the later one of equals), so an
        // input may hold shorter ones (a Markdown or Org rule).
        size_t divider = end;
        for (size_t k = body; k < end; ++k)
            if (IsRule(lines[k], '-') && (divider == end || lines[k].size() >= lines[divider].size())) divider = k;
        for (size_t k = body; k < divider; ++k) c.input += lines[k] + "\n";
        // Like the CLI: the input loses the line break before its divider.
        if (!c.input.empty()) c.input.pop_back();
        for (size_t k = divider + 1; k < end; ++k) c.expected += lines[k] + "\n";
        cases.push_back(c);
        i = end;
    }
    return cases;
}

// Whitespace runs to one space, none after `(` or before `)`.
std::string Normalize(const std::string &sexp) {
    std::string out;
    bool space = false;
    for (char ch : sexp) {
        if (std::isspace(static_cast<unsigned char>(ch))) { space = true; continue; }
        if (space && !out.empty() && out.back() != '(' && ch != ')') out += ' ';
        space = false;
        out += ch;
    }
    return out;
}

bool HasFields(const std::string &sexp) {
    for (size_t i = 0; i + 1 < sexp.size(); ++i)
        if (sexp[i] == ':' && sexp[i + 1] == ' ' && i > 0 && (std::isalnum(static_cast<unsigned char>(sexp[i - 1])) || sexp[i - 1] == '_')) return true;
    return false;
}

// Drops every `field: ` prefix.
std::string StripFields(const std::string &sexp) {
    std::string out;
    for (size_t i = 0; i < sexp.size();) {
        size_t j = i;
        while (j < sexp.size() && (std::isalnum(static_cast<unsigned char>(sexp[j])) || sexp[j] == '_')) ++j;
        if (j > i && j + 1 < sexp.size() && sexp[j] == ':' && sexp[j + 1] == ' ' && (i == 0 || sexp[i - 1] == ' ' || sexp[i - 1] == '(')) {
            i = j + 2;
            continue;
        }
        if (j > i) { out.append(sexp, i, j - i); i = j; continue; }
        out += sexp[i++];
    }
    return out;
}

std::string ReadFile(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

// Runs one grammar's corpus; returns the number of failed cases.
int RunGrammar(const Grammar &grammar, const fs::path &root, int &passed) {
    fs::path dir = root / ("tree-sitter-" + std::string(grammar.name)) / "test" / "corpus";
    if (!fs::is_directory(dir)) return 0;
    std::vector<fs::path> files;
    for (const auto &entry : fs::directory_iterator(dir))
        if (entry.path().extension() == ".txt") files.push_back(entry.path());
    std::sort(files.begin(), files.end());

    TSParser *parser = ts_parser_new();
    ts_parser_set_language(parser, grammar.language());
    int failed = 0;
    for (const fs::path &file : files) {
        for (const Case &c : ParseCorpus(ReadFile(file))) {
            TSTree *tree = ts_parser_parse_string(parser, nullptr, c.input.data(), static_cast<uint32_t>(c.input.size()));
            char *raw = ts_node_string(ts_tree_root_node(tree));
            std::string actual = Normalize(raw);
            std::free(raw);
            ts_tree_delete(tree);
            std::string expected = Normalize(c.expected);
            if (!HasFields(expected)) actual = StripFields(actual);
            if (actual == expected) { ++passed; continue; }
            ++failed;
            std::printf("FAIL %s: %s:%d %s\n  expected: %s\n  actual:   %s\n", grammar.name, file.string().c_str(), c.line,
                        c.name.c_str(), expected.c_str(), actual.c_str());
        }
    }
    ts_parser_delete(parser);
    return failed;
}

}  // namespace

int main(int argc, char **argv) {
    const fs::path root = fs::path(MEP_SOURCE_DIR) / "grammars";
    std::vector<const Grammar *> selected;
    for (int i = 1; i < argc; i++) {
        auto it = std::find_if(std::begin(kGrammars), std::end(kGrammars), [&](const Grammar &g) { return argv[i] == std::string(g.name); });
        if (it == std::end(kGrammars)) {
            std::fprintf(stderr, "mep-ts-corpus-test: no compiled-in grammar `%s`\n", argv[i]);
            return 2;
        }
        selected.push_back(&*it);
    }
    if (selected.empty())
        for (const Grammar &g : kGrammars) selected.push_back(&g);

    int passed = 0, failed = 0;
    for (const Grammar *g : selected) failed += RunGrammar(*g, root, passed);
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
