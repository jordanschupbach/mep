// mep's Ruby formatter: see ruby_format.h.
//
// The community default style (rubocop's and standardrb's defaults, as far as
// they are about layout), applied conservatively over the tree-sitter-ruby
// syntax tree (grammars/tree-sitter-ruby):
//
//   * indentation: two spaces per level of nesting (modules, classes, defs,
//     blocks, if/unless/case/while/begin, and brackets whose contents start
//     on the next line); `else`/`elsif`/`when`/`in`/`rescue`/`ensure` and
//     the closing `end`/`}`/`]`/`)` line up with the line that opened them.
//     A line that continues an expression (method chains, arguments aligned
//     with the first one, conditions) keeps its indentation relative to the
//     line the expression starts on, so alignment survives.
//   * one space around binary and assignment operators (`**` and ranges are
//     left alone), after commas, after `key:`, inside block braces; none
//     before commas or inside parentheses and brackets. Extra spaces used for
//     alignment are kept. An operator written `a -b` (space before, none
//     after) is left exactly as it is: Ruby may read it as an argument.
//   * no trailing whitespace, no runs of blank lines, no blank lines at the
//     start or end of a body, one blank line between method definitions,
//     one newline at the end of the file.
//
// Never touched: the contents of strings, heredocs, `%w[]`/`%i[]` literals,
// regexps, comments, `=begin`/`=end` blocks and everything after `__END__`.
// Quotes are left as written. The result is checked before it is returned:
// it must parse to the same tree, with the same string and comment texts and
// the same non-blank characters, or the input is returned with an error.
#include "ruby_format.h"

#include <tree_sitter/api.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <vector>

extern "C" const TSLanguage *tree_sitter_ruby(void);

namespace rubyfmt {

namespace {

struct Parsed {
    std::string text;
    TSTree *tree = nullptr;
    TSParser *parser = nullptr;
    explicit Parsed(std::string t) : text(std::move(t)) {
        parser = ts_parser_new();
        ts_parser_set_language(parser, tree_sitter_ruby());
        tree = ts_parser_parse_string(parser, nullptr, text.data(), static_cast<uint32_t>(text.size()));
    }
    ~Parsed() {
        if (tree) ts_tree_delete(tree);
        if (parser) ts_parser_delete(parser);
    }
    Parsed(const Parsed &) = delete;
    Parsed &operator=(const Parsed &) = delete;
    TSNode Root() const { return ts_tree_root_node(tree); }
};

bool Is(TSNode n, const char *t) { return !ts_node_is_null(n) && std::strcmp(ts_node_type(n), t) == 0; }
std::string Type(TSNode n) { return ts_node_is_null(n) ? std::string() : std::string(ts_node_type(n)); }
uint32_t Row(TSNode n) { return ts_node_start_point(n).row; }
uint32_t EndRow(TSNode n) { return ts_node_end_point(n).row; }

// Nodes whose text is never changed: one opaque token each.
bool Protected(TSNode n) {
    static const std::set<std::string> k = {"string",       "heredoc_body",    "regex",      "comment",
                                            "string_array", "symbol_array",    "subshell",   "character",
                                            "uninterpreted", "heredoc_beginning", "delimited_symbol", "simple_symbol",
                                            "bare_string",  "bare_symbol",     "chained_string"};
    return !ts_node_is_null(n) && k.count(ts_node_type(n)) > 0;
}

// The first syntax error's 1-based line (0 when none).
int FirstErrorLine(TSNode n) {
    if (ts_node_is_error(n) || ts_node_is_missing(n)) return static_cast<int>(Row(n)) + 1;
    if (!ts_node_has_error(n)) return 0;
    const uint32_t k = ts_node_child_count(n);
    for (uint32_t i = 0; i < k; i++) {
        const int l = FirstErrorLine(ts_node_child(n, i));
        if (l) return l;
    }
    return static_cast<int>(Row(n)) + 1;
}

struct Token {
    TSNode node;
    uint32_t start, end;
    uint32_t row, end_row;
    std::string type;  // node type ("," for anonymous tokens)
    bool named;
};

void CollectTokens(TSNode n, std::vector<Token> &out) {
    const uint32_t k = ts_node_child_count(n);
    if (k == 0 || Protected(n)) {
        if (ts_node_end_byte(n) == ts_node_start_byte(n) && !Protected(n)) return;  // zero-width tokens
        out.push_back(Token{n, ts_node_start_byte(n), ts_node_end_byte(n), Row(n), EndRow(n), Type(n),
                            ts_node_is_named(n)});
        return;
    }
    for (uint32_t i = 0; i < k; i++) CollectTokens(ts_node_child(n, i), out);
}

bool OnlyBlanks(const std::string &s, uint32_t b, uint32_t e) {
    for (uint32_t i = b; i < e; i++) {
        if (s[i] != ' ' && s[i] != '\t') return false;
    }
    return true;
}

// --- pass 1: spacing within lines ------------------------------------------------------

const std::set<std::string> &Operators() {
    static const std::set<std::string> k = {"=",   "+=", "-=",  "*=", "/=", "%=", "||=", "&&=", "|=", "&=",
                                            "^=",  "<<=", ">>=", "==", "!=", "===", "=~", "!~", "<", ">",
                                            "<=",  ">=", "<=>", "+",  "-",  "*",  "/",  "%",  "|", "&",
                                            "^",   "<<", ">>",  "&&", "||", "=>"};
    return k;
}

bool OperatorToken(const Token &t) {
    if (t.named || !Operators().count(t.type)) return false;
    TSNode p = ts_node_parent(t.node);
    const std::string pt = Type(p);
    if (pt == "binary") return ts_node_eq(ts_node_child_by_field_name(p, "operator", 8), t.node);
    if (pt == "assignment" || pt == "operator_assignment") return true;
    if (pt == "optional_parameter" && t.type == "=") return true;
    if (pt == "superclass" && t.type == "<") return true;
    if ((pt == "pair" || pt == "exception_variable" || pt == "rescue") && t.type == "=>") return true;
    return false;
}

bool BracketParent(TSNode open) {
    const std::string pt = Type(ts_node_parent(open));
    return pt == "argument_list" || pt == "parenthesized_statements" || pt == "method_parameters" || pt == "array" ||
           pt == "element_reference" || pt == "lambda_parameters" || pt == "parenthesized_pattern" ||
           pt == "array_pattern" || pt == "destructured_parameter" || pt == "destructured_left_assignment";
}

std::string Spacing(const std::string &src) {
    Parsed p(src);
    std::vector<Token> toks;
    CollectTokens(p.Root(), toks);
    // gap index i: between toks[i] and toks[i+1] -> replacement
    std::map<size_t, std::string> repl;
    auto same_row_gap = [&](size_t i) {
        return i + 1 < toks.size() && toks[i].end_row == toks[i + 1].row &&
               OnlyBlanks(src, toks[i].end, toks[i + 1].start);
    };
    auto gap = [&](size_t i) { return src.substr(toks[i].end, toks[i + 1].start - toks[i].end); };
    auto set = [&](size_t i, const std::string &s) {
        if (repl.count(i)) return;  // the first rule wins
        if (gap(i) != s) repl[i] = s;
    };
    for (size_t i = 0; i + 1 < toks.size(); i++) {
        if (!same_row_gap(i)) continue;
        const Token &a = toks[i], &b = toks[i + 1];
        const std::string g = gap(i);
        if (b.type == "comment") continue;  // trailing comments keep their alignment
        // No space before a comma.
        if (!b.named && b.type == ",") {
            if (!(a.type == "(" || a.type == "[")) set(i, "");
            continue;
        }
        // One space after a comma (unless a closer or a block's `|` follows).
        if (!a.named && a.type == ",") {
            if (b.type == ")" || b.type == "]" || b.type == "}" || b.type == "|") continue;
            if (g.empty()) set(i, " ");
            continue;
        }
        // Nothing inside parentheses and brackets.
        if (!a.named && (a.type == "(" || a.type == "[") && BracketParent(a.node)) {
            set(i, "");
            continue;
        }
        if (!b.named && (b.type == ")" || b.type == "]") && BracketParent(b.node) && !(a.type == "," && g.empty())) {
            set(i, "");
            continue;
        }
        // `x.each { |y| ... }`: a space before a block's brace.
        if (!b.named && b.type == "{" && Is(ts_node_parent(b.node), "block") && g.empty() &&
            a.type != "(" && a.type != "[") {
            set(i, " ");
            continue;
        }
        // `{ |x| x }`: spaces inside block braces and after the parameters.
        if (!a.named && a.type == "{" && Is(ts_node_parent(a.node), "block") && b.type != "}") {
            if (g.empty()) set(i, " ");
            continue;
        }
        if (!b.named && b.type == "}" && Is(ts_node_parent(b.node), "block") && a.type != "{") {
            if (g.empty()) set(i, " ");
            continue;
        }
        if (!a.named && a.type == "|" && Is(ts_node_parent(a.node), "block_parameters") &&
            ts_node_eq(ts_node_child(ts_node_parent(a.node), ts_node_child_count(ts_node_parent(a.node)) - 1), a.node)) {
            if (g.empty() && b.type != "}") set(i, " ");
            continue;
        }
        // `key: value`.
        if (!a.named && a.type == ":" && (Is(ts_node_parent(a.node), "pair") || Is(ts_node_parent(a.node), "keyword_parameter"))) {
            if (g.empty()) set(i, " ");
            continue;
        }
    }
    // Operators: both sides, decided together.
    for (size_t i = 1; i + 1 < toks.size(); i++) {
        const Token &op = toks[i];
        if (!OperatorToken(op)) continue;
        if (toks[i - 1].end_row != op.row || !OnlyBlanks(src, toks[i - 1].end, op.start)) continue;  // leading operator
        const std::string before = gap(i - 1);
        const bool after_same_row = same_row_gap(i);
        const std::string after = after_same_row ? gap(i) : std::string("\n");
        // `a -b`, `a *b`, `a <<B`, `a %w`, `a /b/`: Ruby may read the operator as starting an
        // argument, so the line is left as written.
        static const std::set<std::string> prefixy = {"-", "+", "*", "&", "/", "<<", "%", "**", "::"};
        if (!before.empty() && after.empty() && prefixy.count(op.type)) continue;
        if (before.empty()) set(i - 1, " ");
        if (after.empty()) set(i, " ");
    }
    if (repl.empty()) return src;
    std::string out;
    out.reserve(src.size() + repl.size());
    size_t pos = 0;
    for (const auto &kv : repl) {
        const Token &a = toks[kv.first], &b = toks[kv.first + 1];
        out.append(src, pos, a.end - pos);
        out += kv.second;
        pos = b.start;
    }
    out.append(src, pos, std::string::npos);
    return out;
}

// --- pass 2: indentation and blank lines -----------------------------------------------

bool KeywordContainer(const std::string &t) {
    return t == "class" || t == "module" || t == "singleton_class" || t == "method" || t == "singleton_method" ||
           t == "do_block" || t == "block" || t == "if" || t == "unless" || t == "while" || t == "until" ||
           t == "for" || t == "case" || t == "case_match" || t == "begin" || t == "begin_block" || t == "end_block";
}
bool AlignableKeyword(const std::string &t) {
    return t == "if" || t == "unless" || t == "while" || t == "until" || t == "for" || t == "case" ||
           t == "case_match" || t == "begin";
}
bool BracketContainer(const std::string &t) {
    return t == "array" || t == "hash" || t == "argument_list" || t == "method_parameters" ||
           t == "parenthesized_statements" || t == "element_reference" || t == "lambda_parameters" ||
           t == "array_pattern" || t == "hash_pattern" || t == "parenthesized_pattern" || t == "find_pattern" ||
           t == "block_parameters";
}
bool Transparent(const std::string &t) {
    return t == "body_statement" || t == "block_body" || t == "then" || t == "else" || t == "ensure" || t == "do" ||
           t == "begin_block" || t == "end_block";
}
bool Middle(const std::string &t) {
    return t == "else" || t == "elsif" || t == "when" || t == "in_clause" || t == "rescue" || t == "ensure";
}

struct Layout {
    const std::string &src;
    TSNode root;
    const Options &opts;
    std::vector<uint32_t> line_start;
    std::vector<std::string> lines;
    std::vector<int> orig_indent;  // columns
    std::vector<int> new_indent;
    std::vector<bool> keep;         // verbatim (inside a string, heredoc, =begin, __END__)
    std::vector<bool> keep_trailing;  // trailing blanks are string content
    std::vector<bool> blank;

    Layout(const std::string &s, TSNode r, const Options &o) : src(s), root(r), opts(o) {
        uint32_t ls = 0;
        for (uint32_t i = 0; i <= src.size(); i++) {
            if (i == src.size() || src[i] == '\n') {
                line_start.push_back(ls);
                lines.push_back(src.substr(ls, i - ls));
                ls = i + 1;
            }
        }
        const size_t n = lines.size();
        orig_indent.assign(n, 0);
        new_indent.assign(n, -1);
        keep.assign(n, false);
        keep_trailing.assign(n, false);
        blank.assign(n, false);
        for (size_t i = 0; i < n; i++) {
            int col = 0;
            size_t k = 0;
            while (k < lines[i].size() && (lines[i][k] == ' ' || lines[i][k] == '\t')) {
                col = lines[i][k] == '\t' ? (col / 8 + 1) * 8 : col + 1;
                k++;
            }
            orig_indent[i] = col;
            blank[i] = k == lines[i].size();
        }
        MarkProtected(root);
    }

    // Lines that begin inside a protected node are kept verbatim; lines whose newline is inside
    // one keep their trailing blanks.
    void MarkProtected(TSNode n) {
        if (Protected(n) || Is(n, "heredoc_body")) {
            const uint32_t b = ts_node_start_byte(n), e = ts_node_end_byte(n);
            const bool whole_lines = Is(n, "heredoc_body") || Is(n, "uninterpreted");
            for (uint32_t row = Row(n); row <= EndRow(n) && row < lines.size(); row++) {
                const uint32_t ls = line_start[row];
                if ((whole_lines ? ls >= b : ls > b) && ls < e) keep[row] = true;
                const uint32_t nl = ls + static_cast<uint32_t>(lines[row].size());
                if (nl >= b && nl < e && !(whole_lines && nl == b)) keep_trailing[row] = true;
            }
            // `=begin` must stay at the start of its line.
            if (Is(n, "comment") && src.compare(b, 6, "=begin") == 0) {
                for (uint32_t row = Row(n); row <= EndRow(n) && row < lines.size(); row++) keep[row] = true;
            }
            if (Is(n, "heredoc_body") || Is(n, "uninterpreted")) return;
        }
        const uint32_t k = ts_node_child_count(n);
        for (uint32_t i = 0; i < k; i++) MarkProtected(ts_node_child(n, i));
    }

    uint32_t FirstByte(size_t row) const {
        uint32_t b = line_start[row];
        while (b < src.size() && (src[b] == ' ' || src[b] == '\t')) b++;
        return b;
    }

    // The token after the opener of bracket container @p c is on a later line.
    bool OpensHanging(TSNode c) const {
        TSNode open{};
        const uint32_t k = ts_node_child_count(c);
        for (uint32_t i = 0; i < k; i++) {
            TSNode ch = ts_node_child(c, i);
            const std::string t = Type(ch);
            if (!ts_node_is_named(ch) && (t == "(" || t == "[" || t == "{" || t == "|")) {
                open = ch;
                // The next non-comment sibling.
                for (uint32_t j = i + 1; j < k; j++) {
                    TSNode nx = ts_node_child(c, j);
                    if (Is(nx, "comment")) continue;
                    return Row(nx) > Row(open);
                }
                return false;
            }
            if (ts_node_is_named(ch) && !Is(ch, "comment")) return false;  // no opener first
        }
        return false;
    }

    bool Active(TSNode c, uint32_t row) const {
        if (Row(c) >= row) return false;
        const std::string t = Type(c);
        if (KeywordContainer(t)) return true;
        if (BracketContainer(t)) return OpensHanging(c);
        return false;
    }

    int Anchor(TSNode c) {
        const uint32_t r = Row(c);
        const int base = new_indent[r] >= 0 ? new_indent[r] : orig_indent[r];
        const std::string t = Type(c);
        if ((t == "do_block" || t == "block") && EndRow(c) != r) {
            // `foo(a,\n    b) do ... end`: when `end` lines up with the line the call starts on
            // rather than with the `do` line, the body is anchored there too.
            TSNode top = c;
            for (TSNode p = ts_node_parent(c); !ts_node_is_null(p) && !Is(p, "program") && !Transparent(Type(p)) &&
                                               ts_node_end_byte(p) == ts_node_end_byte(c);
                 p = ts_node_parent(p)) {
                top = p;
            }
            const uint32_t cr = Row(top), er = EndRow(c);
            const uint32_t efb = FirstByte(er);
            if (cr < r && er < lines.size() && (src.compare(efb, 3, "end") == 0 || src[efb] == '}') &&
                orig_indent[er] == orig_indent[cr] && orig_indent[er] != orig_indent[r]) {
                return new_indent[cr] >= 0 ? new_indent[cr] : orig_indent[cr];
            }
        }
        if (AlignableKeyword(t) && ts_node_start_byte(c) != FirstByte(r)) {
            // `x = if cond`: `end` under the keyword (rubocop's default) or under the line's start
            // (standardrb's), as the code already has it.
            const int col = static_cast<int>(ts_node_start_point(c).column);
            const int keyword_col = base + (col - orig_indent[r]);
            const uint32_t er = EndRow(c);
            if (er < lines.size() && er != r && orig_indent[er] == col && src.compare(FirstByte(er), 3, "end") == 0) {
                return keyword_col;
            }
            // ... or its first `when`/`in`/`else`/`elsif` lines up with the keyword (rubocop's
            // default for `case`), whatever the `end` does.
            const uint32_t k = ts_node_child_count(c);
            for (uint32_t i = 0; i < k; i++) {
                TSNode ch = ts_node_child(c, i);
                if (!Middle(Type(ch))) continue;
                const uint32_t mr = Row(ch);
                if (mr != r && ts_node_start_byte(ch) == FirstByte(mr) && orig_indent[mr] == col) return keyword_col;
                break;
            }
        }
        return base;
    }

    int IndentFor(size_t row) {
        const uint32_t fb = FirstByte(row);
        TSNode leaf = ts_node_descendant_for_byte_range(root, fb, fb);
        if (ts_node_is_null(leaf)) return orig_indent[row];
        // Walk up to the innermost active container.
        std::vector<TSNode> path;  // below the container, leaf first
        TSNode c{};
        for (TSNode n = leaf; !ts_node_is_null(n); n = ts_node_parent(n)) {
            if (!ts_node_eq(n, leaf) && Active(n, static_cast<uint32_t>(row))) {
                c = n;
                break;
            }
            if (Is(n, "program")) break;
            path.push_back(n);
        }
        const bool top = ts_node_is_null(c);
        const int anchor = top ? -opts.indent : Anchor(c);
        // The highest node below the container that starts on this line.
        TSNode s{};
        for (TSNode n : path) {
            if (Row(n) == row) s = n;
        }
        if (ts_node_is_null(s)) return orig_indent[row];
        const std::string st = Type(s);
        TSNode sp = ts_node_parent(s);
        const bool closer = !ts_node_is_named(s) && (st == "end" || st == "}" || st == "]" || st == ")" || st == "|") &&
                            (ts_node_eq(sp, c) || (Transparent(Type(sp)) && ts_node_eq(ts_node_parent(sp), c)));
        if (!top && (Middle(st) || closer)) {
            return std::max(anchor, 0);
        }
        // `when a, b` / `then x` split over two lines: `then` continues the clause.
        if (st == "then" && src.compare(fb, 4, "then") == 0 && !ts_node_is_null(sp) && Row(sp) < row) {
            const uint32_t pr = Row(sp);
            const int base = new_indent[pr] >= 0 ? new_indent[pr] : orig_indent[pr];
            return std::max(0, base + orig_indent[row] - orig_indent[pr]);
        }
        const bool fresh = (top && (Is(sp, "program") || ts_node_is_null(sp))) || (!top && ts_node_eq(sp, c)) ||
                           (!ts_node_is_null(sp) && Transparent(Type(sp)) && !Is(sp, "program"));
        if (fresh) return std::max(anchor + opts.indent, 0);
        // A continuation line: relative to the line its statement starts on.
        TSNode r{};
        for (TSNode n : path) {
            if (!Transparent(Type(n))) r = n;
        }
        if (ts_node_is_null(r) || Row(r) >= row) return orig_indent[row];
        const uint32_t rr = Row(r);
        const int base = new_indent[rr] >= 0 ? new_indent[rr] : orig_indent[rr];
        return std::max(0, base + orig_indent[row] - orig_indent[rr]);
    }

    std::string Run() {
        const size_t n = lines.size();
        std::vector<bool> comment(n, false);
        for (size_t i = 0; i < n; i++) {
            if (keep[i] || blank[i]) continue;
            const uint32_t fb = FirstByte(i);
            comment[i] = src[fb] == '#';
            new_indent[i] = IndentFor(i);
        }
        // A comment that lined up with the next line of code keeps lining up with it.
        for (size_t i = n; i-- > 0;) {
            if (!comment[i]) continue;
            size_t j = i + 1;
            while (j < n && (blank[j] || comment[j]) && !keep[j]) j++;
            if (j < n && !keep[j] && !blank[j] && orig_indent[j] == orig_indent[i]) new_indent[i] = new_indent[j];
        }
        // Blank lines to drop: runs, at the start/end of bodies, at the start/end of the file.
        std::vector<bool> drop(n, false), blank_before(n, false);
        for (size_t i = 0; i < n; i++) {
            if (!blank[i] || keep[i]) continue;
            if (i > 0 && blank[i - 1] && !keep[i - 1]) drop[i] = true;
        }
        size_t first = 0;
        while (first < n && blank[first] && !keep[first]) drop[first++] = true;
        size_t last = n;
        while (last > 0 && blank[last - 1] && !keep[last - 1]) drop[--last] = true;
        BodyBlanks(root, drop);
        DefSpacing(root, blank_before, drop);
        std::string out;
        out.reserve(src.size() + 64);
        for (size_t i = 0; i < n; i++) {
            if (drop[i]) continue;
            if (blank_before[i]) out += "\n";
            if (keep[i]) {
                out += lines[i];
            } else if (blank[i]) {
                // nothing
            } else {
                std::string body = lines[i].substr(FirstByte(i) - line_start[i]);
                if (!keep_trailing[i]) {
                    while (!body.empty() && (body.back() == ' ' || body.back() == '\t')) body.pop_back();
                }
                out.append(static_cast<size_t>(new_indent[i]), ' ');
                out += body;
            }
            if (i + 1 < n) out += "\n";
        }
        while (!out.empty() && out.back() == '\n' && !(n > 0 && keep[n - 1])) out.pop_back();
        if (!out.empty() && out.back() != '\n') out += "\n";
        return out;
    }

    // No blank lines right after `class X`/`def x`/`do |y|` or right before their `end`.
    void BodyBlanks(TSNode n, std::vector<bool> &drop) {
        const std::string t = Type(n);
        if ((t == "class" || t == "module" || t == "singleton_class" || t == "method" || t == "singleton_method" ||
             t == "do_block" || t == "block") &&
            EndRow(n) > Row(n) + 1) {
            const uint32_t er = EndRow(n);
            const uint32_t fb = FirstByte(er);
            const bool closer_line = src.compare(fb, 3, "end") == 0 || src[fb] == '}';
            // The header may span lines (parameters); the body starts after it.
            uint32_t header_end = Row(n);
            TSNode params = ts_node_child_by_field_name(n, "parameters", 10);
            if (!ts_node_is_null(params)) header_end = std::max(header_end, EndRow(params));
            TSNode sup = ts_node_child_by_field_name(n, "superclass", 10);
            if (!ts_node_is_null(sup)) header_end = std::max(header_end, EndRow(sup));
            for (uint32_t r = header_end + 1; r < er && blank[r] && !keep[r]; r++) drop[r] = true;
            if (closer_line) {
                for (uint32_t r = er; r-- > header_end + 1 && blank[r] && !keep[r];) drop[r] = true;
            }
        }
        const uint32_t k = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < k; i++) BodyBlanks(ts_node_named_child(n, i), drop);
    }

    // One blank line between two method definitions (above the second's doc comment).
    void DefSpacing(TSNode n, std::vector<bool> &blank_before, const std::vector<bool> &drop) {
        const uint32_t k = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < k; i++) {
            TSNode cur = ts_node_named_child(n, i);
            DefSpacing(cur, blank_before, drop);
            if (!(Is(cur, "method") || Is(cur, "singleton_method"))) continue;
            // Its doc comment: the comment siblings right above it.
            uint32_t top = Row(cur);
            int j = static_cast<int>(i) - 1;
            while (j >= 0) {
                TSNode prev = ts_node_named_child(n, static_cast<uint32_t>(j));
                if (Is(prev, "comment") && EndRow(prev) + 1 == top && FirstByte(Row(prev)) == ts_node_start_byte(prev)) {
                    top = Row(prev);
                    j--;
                    continue;
                }
                break;
            }
            if (j < 0) continue;
            TSNode prev = ts_node_named_child(n, static_cast<uint32_t>(j));
            if (!(Is(prev, "method") || Is(prev, "singleton_method"))) continue;
            if (EndRow(prev) + 1 != top) continue;  // already separated (or on one line)
            if (EndRow(prev) == Row(prev) && EndRow(cur) == Row(cur)) continue;  // adjacent one-liners
            if (top < blank_before.size() && !drop[top]) blank_before[top] = true;
        }
    }
};

// Everything that must survive formatting unchanged: the tree's shape, and the text of every
// protected node, in order.
std::string Fingerprint(const Parsed &p) {
    char *sexp = ts_node_string(p.Root());
    std::string s = sexp ? sexp : "";
    std::free(sexp);
    std::vector<Token> toks;
    CollectTokens(p.Root(), toks);
    for (const Token &t : toks) {
        if (!Protected(t.node)) continue;
        s += '\x01';
        if (t.type == "heredoc_body" || t.type == "comment" || t.type == "uninterpreted") {
            s.append(p.text, t.start, t.end - t.start);
        } else {
            s.append(p.text, t.start, t.end - t.start);
        }
    }
    return s;
}

std::string NonBlank(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') out += c;
    }
    return out;
}

}  // namespace

Result Format(std::string_view input, const Options &opts) {
    Result r;
    r.text = std::string(input);
    Options o = opts;
    if (o.indent <= 0 || o.indent > 16) o.indent = 2;
    // CRLF files: format with LF, then put CRLF back.
    std::string src(input);
    const bool crlf = src.find("\r\n") != std::string::npos && src.find('\n') == src.find("\r\n") + 1;
    if (crlf) {
        std::string lf;
        for (size_t i = 0; i < src.size(); i++) {
            if (src[i] == '\r' && i + 1 < src.size() && src[i + 1] == '\n') continue;
            lf += src[i];
        }
        src = lf;
    }
    Parsed original(src);
    if (ts_node_has_error(original.Root())) {
        r.error_line = FirstErrorLine(original.Root());
        r.error = "syntax error; not formatted";
        return r;
    }
    const std::string spaced = Spacing(src);
    Parsed p1(spaced);
    if (ts_node_has_error(p1.Root()) || Fingerprint(p1) != Fingerprint(original)) {
        r.error = "internal error: spacing would change the program; not formatted";
        return r;
    }
    Layout layout(p1.text, p1.Root(), o);
    std::string out = layout.Run();
    Parsed p2(out);
    if (ts_node_has_error(p2.Root()) || Fingerprint(p2) != Fingerprint(original) || NonBlank(out) != NonBlank(src)) {
        r.error = "internal error: indentation would change the program; not formatted";
        return r;
    }
    if (crlf) {
        std::string back;
        for (char c : out) {
            if (c == '\n') back += '\r';
            back += c;
        }
        out = back;
    }
    r.ok = true;
    r.text = out;
    return r;
}

}  // namespace rubyfmt
