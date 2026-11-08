// Compares mep's Maxima tree-sitter grammar (grammars/tree-sitter-maxima)
// with mep's Maxima reader (src/maxima_ast.cpp, which agrees with Maxima's
// own mread-raw on the whole share/ tree): for each file named on stdin, the
// reader's statements are rendered as the tree the grammar should build,
// and both are printed as one s-expression per statement, leaves with
// their byte ranges. tools/ts_grammar_diff/maxima_check.py builds this and
// diffs the two.
//
// Output per file: "@@FILE <path>", then "@@REJECT <message>" when the
// reader rejects the file (the grammar is not compared there), else
// "R <sexpr>" / "T <sexpr>" line pairs, one per top-level item, and
// "@@COUNT <reader items> <grammar items> <grammar errors>".
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <tree_sitter/api.h>

#include "maxima_ast.h"

extern "C" const TSLanguage *tree_sitter_maxima(void);

namespace {

struct Doc {
    std::string text;
    std::vector<std::string> lines;
    std::vector<size_t> starts;
    size_t Off(int line, int col) const {
        if (line < 0) return 0;
        if (static_cast<size_t>(line) >= starts.size()) return text.size();
        return starts[static_cast<size_t>(line)] + static_cast<size_t>(col);
    }
};

std::string Leaf(const char *type, size_t a, size_t b) {
    return std::string("(") + type + " " + std::to_string(a) + " " + std::to_string(b) + ")";
}

// ---- the reader's tree, as the grammar's node types ----

struct Reader {
    const Doc &doc;
    const MxParseResult &r;

    std::string Atom(const MxNode &n) const {
        const char *leaf = n.kind == MxNodeKind::Num ? "number" : n.kind == MxNodeKind::Str ? "string" : "identifier";
        const size_t a = doc.Off(n.range.line, n.range.col), b = doc.Off(n.range.end_line, n.range.end_col);
        return "(atom " + Leaf(leaf, a, b) + ")";
    }
    std::string Ident(int i) const {
        const MxNode &n = r.nodes[static_cast<size_t>(i)];
        if (n.kind != MxNodeKind::Ident && n.kind != MxNodeKind::Lisp) return Node(i);
        return Leaf("identifier", doc.Off(n.range.line, n.range.col), doc.Off(n.range.end_line, n.range.end_col));
    }
    std::string Node(int i) const {
        if (i < 0) return "(MISSING)";
        const MxNode &n = r.nodes[static_cast<size_t>(i)];
        auto kids = [&](const std::vector<int> &ks) {
            std::string s;
            for (int k : ks) s += " " + Node(k);
            return s;
        };
        auto args = [&](const std::vector<MxArg> &as) {
            std::string s;
            for (const MxArg &a : as) s += " " + Node(a.value);
            return s;
        };
        switch (n.kind) {
            case MxNodeKind::Num:
            case MxNodeKind::Str:
            case MxNodeKind::Ident:
            case MxNodeKind::Lisp:
                return Atom(n);
            case MxNodeKind::List:
                return "(list" + kids(n.kids) + ")";
            case MxNodeKind::Set:
                return "(set" + kids(n.kids) + ")";
            case MxNodeKind::Call: {
                const MxNode &callee = r.nodes[static_cast<size_t>(n.kids[0])];
                if (callee.kind == MxNodeKind::Ident && (callee.text == "block" || callee.text == "lambda"))
                    return "(" + callee.text + "_expression" + args(n.args) + ")";
                return "(function_call " + Node(n.kids[0]) + args(n.args) + ")";
            }
            case MxNodeKind::Index:
                return "(array_ref " + Node(n.kids[0]) + args(n.args) + ")";
            case MxNodeKind::Paren:
                return "(parenthesized_expression" + kids(n.kids) + ")";
            case MxNodeKind::Binary:
                return "(binary_expression" + kids(n.kids) + ")";
            case MxNodeKind::Label:
                return "(binary_expression " + Ident(n.kids[0]) + " " + Node(n.kids[1]) + ")";
            case MxNodeKind::Unary:
            case MxNodeKind::Quote:
                return "(prefix_expression" + kids(n.kids) + ")";
            case MxNodeKind::Postfix:
                return "(postfix_expression" + kids(n.kids) + ")";
            case MxNodeKind::If: {
                std::string s = "(if_expression";
                size_t k = 0;
                for (; k + 1 < n.args.size() && n.args[k].name == "cond"; k += 2) {
                    if (k == 0)
                        s += " " + Node(n.args[k].value) + " " + Node(n.args[k + 1].value);
                    else
                        s += " (elseif_clause " + Node(n.args[k].value) + " " + Node(n.args[k + 1].value) + ")";
                }
                for (; k < n.args.size(); k++) s += " " + Node(n.args[k].value);
                return s + ")";
            }
            case MxNodeKind::Do: {
                std::string first = n.args.empty() ? "do" : n.args[0].name;
                bool has_in = false;
                for (const MxArg &a : n.args) has_in = has_in || a.name == "in";
                const char *type = has_in ? "for_in_expression"
                                   : first == "while" ? "while_expression"
                                   : first == "unless" ? "unless_expression"
                                                       : "for_expression";
                std::string s = std::string("(") + type;
                for (const MxArg &a : n.args) s += " " + (a.name == "for" ? Ident(a.value) : Node(a.value));
                return s + ")";
            }
            case MxNodeKind::LispEsc:
                return Leaf("lisp_escape", doc.Off(n.range.line, n.range.col), doc.Off(n.range.end_line, n.range.end_col));
            case MxNodeKind::Error:
                return "(ERROR)";
        }
        return "(?)";
    }
};

// ---- the grammar's tree ----

std::string TsNode(TSNode n, const Doc &doc) {
    const char *type = ts_node_is_missing(n) ? "MISSING" : ts_node_type(n);
    const uint32_t count = ts_node_named_child_count(n);
    std::string kids;
    for (uint32_t i = 0; i < count; i++) {
        TSNode c = ts_node_named_child(n, i);
        if (std::strcmp(ts_node_type(c), "comment") == 0) continue;
        kids += " " + TsNode(c, doc);
    }
    if (std::strcmp(type, "number") == 0 || std::strcmp(type, "string") == 0 || std::strcmp(type, "identifier") == 0 ||
        std::strcmp(type, "lisp_escape") == 0) {
        size_t a = ts_node_start_byte(n), b = ts_node_end_byte(n);
        if (std::strcmp(type, "lisp_escape") == 0) {
            // The reader's range ends at the line's last token, the grammar's at the line end.
            while (b > a && (doc.text[b - 1] == ' ' || doc.text[b - 1] == '\t' || doc.text[b - 1] == '\r')) b--;
        }
        return Leaf(type, a, b);
    }
    return std::string("(") + type + kids + ")";
}

int CountErrors(TSNode n) {
    int e = (ts_node_is_error(n) || ts_node_is_missing(n)) ? 1 : 0;
    const uint32_t count = ts_node_child_count(n);
    for (uint32_t i = 0; i < count; i++) e += CountErrors(ts_node_child(n, i));
    return e;
}

}  // namespace

int main() {
    TSParser *parser = ts_parser_new();
    ts_parser_set_language(parser, tree_sitter_maxima());
    std::string path;
    while (std::getline(std::cin, path)) {
        if (path.empty()) continue;
        std::ifstream in(path, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        Doc doc;
        doc.text = ss.str();
        {
            size_t i = 0;
            doc.starts.push_back(0);
            for (;;) {
                const size_t j = doc.text.find('\n', i);
                if (j == std::string::npos) {
                    doc.lines.push_back(doc.text.substr(i));
                    break;
                }
                doc.lines.push_back(doc.text.substr(i, j - i));
                i = j + 1;
                doc.starts.push_back(i);
            }
        }
        std::printf("@@FILE %s\n", path.c_str());
        const MxParseResult r = MxParse(doc.lines);
        bool reject = false;
        for (const MxParseError &e : r.errors) {
            // The reader's lint-level findings are not rejections.
            if (e.code != "undeclared-operator" && e.code != "empty-statement") {
                std::printf("@@REJECT %d:%d %s %s\n", e.range.line + 1, e.range.col + 1, e.code.c_str(),
                            e.message.c_str());
                reject = true;
                break;
            }
        }
        bool undeclared = false;
        for (const MxParseError &e : r.errors) undeclared = undeclared || e.code == "undeclared-operator";
        if (!r.user_operators.empty() || undeclared) std::printf("@@USEROPS %zu\n", r.user_operators.size());
        TSTree *tree = ts_parser_parse_string(parser, nullptr, doc.text.data(), static_cast<uint32_t>(doc.text.size()));
        TSNode root = ts_tree_root_node(tree);
        const int ts_errors = CountErrors(root);
        if (!reject) {
            Reader rd{doc, r};
            std::vector<std::string> rs, ts;
            for (const MxStatement &st : r.statements) {
                if (st.node < 0) continue;
                const MxNode &n = r.nodes[static_cast<size_t>(st.node)];
                if (n.kind == MxNodeKind::LispEsc)
                    rs.push_back(rd.Node(st.node));
                else
                    rs.push_back("(statement " + rd.Node(st.node) + ")");
            }
            const uint32_t count = ts_node_named_child_count(root);
            for (uint32_t i = 0; i < count; i++) {
                TSNode c = ts_node_named_child(root, i);
                if (std::strcmp(ts_node_type(c), "comment") == 0) continue;
                ts.push_back(TsNode(c, doc));
            }
            const size_t m = std::max(rs.size(), ts.size());
            for (size_t i = 0; i < m; i++) {
                std::printf("R %s\n", i < rs.size() ? rs[i].c_str() : "-");
                std::printf("T %s\n", i < ts.size() ? ts[i].c_str() : "-");
            }
            std::printf("@@COUNT %zu %zu %d\n", rs.size(), ts.size(), ts_errors);
        } else {
            std::printf("@@COUNT 0 0 %d\n", ts_errors);
        }
        ts_tree_delete(tree);
    }
    ts_parser_delete(parser);
    return 0;
}
