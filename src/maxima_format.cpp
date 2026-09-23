// mep's own Maxima formatter. See maxima_format.h for the style and
// where it comes from; this file is the printer that implements it.

#include "maxima_format.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "maxima_ast.h"

namespace mxfmt {
namespace {

// ---------------------------------------------------------------------
// Source access
// ---------------------------------------------------------------------

/** @brief Splits a source string into lines, dropping a trailing CR from CRLF input. */
std::vector<std::string> SplitLines(const std::string &src) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : src) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            lines.push_back(cur);
            cur.clear();
            continue;
        }
        cur += c;
    }
    if (!cur.empty() && cur.back() == '\r') cur.pop_back();
    lines.push_back(cur);
    return lines;
}

// The source text a range covers, exactly as written. Leaves are emitted
// from this rather than from the token's decoded value: a string's
// escapes, a number's exponent spelling and a `\`-escaped name are all
// things the formatter has no business normalizing.
class Source {
  public:
    explicit Source(const std::vector<std::string> &lines) : lines_(lines) {}

    std::string Slice(const MxRange &r) const {
        if (r.line < 0 || r.line >= static_cast<int>(lines_.size())) return std::string();
        if (r.line == r.end_line) {
            const std::string &line = lines_[static_cast<size_t>(r.line)];
            const size_t start = std::min(static_cast<size_t>(r.col), line.size());
            const size_t end = std::min(static_cast<size_t>(r.end_col), line.size());
            return end > start ? line.substr(start, end - start) : std::string();
        }
        std::string out;
        for (int i = r.line; i <= r.end_line && i < static_cast<int>(lines_.size()); i++) {
            const std::string &line = lines_[static_cast<size_t>(i)];
            const size_t start = i == r.line ? std::min(static_cast<size_t>(r.col), line.size()) : 0;
            const size_t end = i == r.end_line ? std::min(static_cast<size_t>(r.end_col), line.size()) : line.size();
            if (i != r.line) out += '\n';
            if (end > start) out += line.substr(start, end - start);
        }
        return out;
    }

    /** @brief Whether anything but whitespace precedes `col` on `line`. */
    bool CodeBefore(int line, int col) const {
        if (line < 0 || line >= static_cast<int>(lines_.size())) return false;
        const std::string &text = lines_[static_cast<size_t>(line)];
        const size_t limit = std::min(static_cast<size_t>(col), text.size());
        for (size_t i = 0; i < limit; i++) {
            if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r') return true;
        }
        return false;
    }

  private:
    const std::vector<std::string> &lines_;
};

// ---------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------

std::string RTrim(const std::string &s) {
    size_t end = s.size();
    while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r')) end--;
    return s.substr(0, end);
}

/** @brief Display width in codepoints, so a UTF-8 name does not count as its byte length. */
int Width(const std::string &s) {
    int n = 0;
    for (char c : s) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) n++;
    }
    return n;
}

// A comment as it will be written: its own text, with trailing
// whitespace on each interior line removed and the indentation of a
// multi-line comment left exactly as the author had it. Re-indenting the
// interior of a `/* ... */` would reflow prose nobody asked to reflow.
std::string NormComment(const std::string &raw) {
    std::vector<std::string> lines = SplitLines(raw);
    std::string out;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i > 0) out += '\n';
        out += RTrim(lines[i]);
    }
    return out;
}

// ---------------------------------------------------------------------
// Operator spacing
// ---------------------------------------------------------------------

// The rule the header states: structure is spaced, arithmetic is dense.
// `.` is spaced whatever it is doing, because `1 . 2` written tight is
// the number `1.2` -- the one place in this language where a missing
// space changes the meaning outright.
bool SpacedOperator(const std::string &op) {
    if (op == "+" || op == "-" || op == "*" || op == "/" || op == "^" || op == "**" || op == "^^") return false;
    return true;
}

/** @brief Reports whether an operator is one of Maxima's assignment or definition forms. */
bool IsAssignment(const std::string &op) { return op == ":" || op == "::" || op == ":=" || op == "::="; }

// ---------------------------------------------------------------------
// Comments attached to the tree
// ---------------------------------------------------------------------

struct AttachedComment {
    std::string text;
    int blanks_before = 0;  // blank lines between this comment and what came before it
};

struct StatementPlan {
    int node = -1;
    MxRange range;
    bool terminated = false;
    char terminator = ';';
    std::vector<AttachedComment> leading;  // comments on their own lines above
    // At most one single-line comment, written after the terminator on
    // the same line. Only a single-line one, and only the first: a
    // multi-line comment pushed onto that line would become several
    // lines in the output, and the next run would read them back as
    // separate comments -- the "never decide layout by measuring
    // something whose own layout can change" trap, in its Maxima form.
    std::string trailing;
    std::vector<AttachedComment> after;  // comments on their own lines below
    int blanks_before = 0;               // blank lines above the statement (or its first comment)
    // Blank lines between the last leading comment and the statement
    // itself. Dropping this gap is not only a loss -- it is a *moving*
    // loss: a comment that ends up above a statement on one run would
    // lose its gap on the next, and the file would never settle.
    int blanks_after_leading = 0;
};


// Where every comment in the file ends up. A comment above a statement
// is that statement's; one after the code on a statement's last line
// trails it; one *inside* a statement is attached to the next thing in
// it, which is what keeps a comment between two arguments of a call from
// being dropped on the floor when the call is re-wrapped.
struct CommentPlan {
    std::vector<StatementPlan> statements;
    std::vector<std::vector<AttachedComment>> node_leading;  // by node index
    std::vector<bool> deep;  // some node strictly below this one carries a comment
    std::vector<AttachedComment> tail;
};

/** @brief Whether a comment can ride after code on one line: single-line and short enough to be one. */
bool FitsOnOneLine(const std::string &text) { return text.find('\n') == std::string::npos; }

/** @brief Whether position (line, col) is at or after the start of `r`. */
bool AtOrAfter(int line, int col, const MxRange &r) {
    if (line != r.line) return line > r.line;
    return col >= r.col;
}

// Every node of a statement, in document order by start position, with
// the outermost first among nodes that start in the same place.
void CollectNodes(const MxParseResult &parse, int node, std::vector<int> *out) {
    if (node < 0) return;
    out->push_back(node);
    const MxNode &n = parse.at(node);
    for (int kid : n.kids) CollectNodes(parse, kid, out);
    for (const MxArg &arg : n.args) CollectNodes(parse, arg.value, out);
}

void MarkDeep(const MxParseResult &parse, int node, CommentPlan *plan) {
    if (node < 0) return;
    const MxNode &n = parse.at(node);
    bool any = false;
    for (int kid : n.kids) {
        MarkDeep(parse, kid, plan);
        if (!plan->node_leading[static_cast<size_t>(kid)].empty() || plan->deep[static_cast<size_t>(kid)]) any = true;
    }
    for (const MxArg &arg : n.args) {
        if (arg.value < 0) continue;
        MarkDeep(parse, arg.value, plan);
        if (!plan->node_leading[static_cast<size_t>(arg.value)].empty() ||
            plan->deep[static_cast<size_t>(arg.value)]) {
            any = true;
        }
    }
    plan->deep[static_cast<size_t>(node)] = any;
}

void AttachComments(const MxParseResult &parse, const Source &src, CommentPlan *plan) {
    plan->node_leading.assign(parse.nodes.size(), {});
    plan->deep.assign(parse.nodes.size(), false);
    size_t next = 0;
    int previous_end = -1;  // last line already accounted for
    for (StatementPlan &statement : plan->statements) {
        // Comments above the statement: each on its own line, unless it
        // shares a line with code that came before it.
        while (next < parse.comments.size() && parse.comments[next].range.line < statement.range.line) {
            const MxComment &c = parse.comments[next];
            AttachedComment attached;
            attached.text = NormComment(c.text);
            attached.blanks_before = previous_end < 0 ? 0 : std::max(0, c.range.line - previous_end - 1);
            if (src.CodeBefore(c.range.line, c.range.col)) {
                // It belongs to the line it is on, which is the previous
                // statement's; that statement takes it as its trailing
                // comment if it has none yet and it is one line.
                StatementPlan *owner = nullptr;
                for (StatementPlan &earlier : plan->statements) {
                    if (earlier.range.end_line == c.range.line) owner = &earlier;
                }
                if (owner == nullptr) {
                    statement.leading.push_back(attached);
                } else if (owner->trailing.empty() && owner->after.empty() && FitsOnOneLine(attached.text)) {
                    owner->trailing = attached.text;
                } else {
                    owner->after.push_back(attached);
                }
            } else {
                statement.leading.push_back(attached);
            }
            previous_end = c.range.end_line;
            next++;
        }
        const int leading_end = previous_end;
        // Comments inside the statement, attached to whatever comes next
        // in it. Collected outermost-first so a comment before `f(x)`
        // lands on the call rather than on the name inside it.
        std::vector<int> nodes;
        CollectNodes(parse, statement.node, &nodes);
        while (next < parse.comments.size() &&
               AtOrAfter(statement.range.end_line, statement.range.end_col, parse.comments[next].range)) {
            const MxComment &c = parse.comments[next];
            AttachedComment attached;
            attached.text = NormComment(c.text);
            attached.blanks_before = 0;
            int best = -1;
            for (int node : nodes) {
                const MxRange &r = parse.at(node).range;
                if (!AtOrAfter(r.line, r.col, c.range) || (r.line == c.range.end_line && r.col < c.range.end_col)) {
                    continue;
                }
                if (r.line < c.range.end_line) continue;
                if (best < 0) {
                    best = node;
                    continue;
                }
                const MxRange &b = parse.at(best).range;
                if (r.line < b.line || (r.line == b.line && r.col < b.col)) best = node;
            }
            if (best >= 0) {
                plan->node_leading[static_cast<size_t>(best)].push_back(attached);
            } else if (statement.trailing.empty() && statement.after.empty() && FitsOnOneLine(attached.text)) {
                // Nothing follows it inside the statement -- `f(x /* c */)`
                // -- so it rides after the terminator instead of being
                // lost. The order of the file's comments is preserved,
                // which is what the self-check checks.
                statement.trailing = attached.text;
            } else {
                statement.after.push_back(attached);
            }
            previous_end = c.range.end_line;
            next++;
        }
        MarkDeep(parse, statement.node, plan);
        statement.blanks_before = statement.leading.empty()
                                      ? (previous_end < 0 ? 0 : std::max(0, statement.range.line - previous_end - 1))
                                      : statement.leading.front().blanks_before;
        if (!statement.leading.empty()) {
            statement.blanks_after_leading = std::max(0, leading_end < 0 ? 0 : statement.range.line - leading_end - 1);
        }
        previous_end = std::max(previous_end, statement.range.end_line);
    }
    for (; next < parse.comments.size(); next++) {
        const MxComment &c = parse.comments[next];
        AttachedComment attached;
        attached.text = NormComment(c.text);
        attached.blanks_before = previous_end < 0 ? 0 : std::max(0, c.range.line - previous_end - 1);
        // A comment after the *last* statement, on that statement's own
        // line, trails it the same way one in the middle of the file
        // does. Without this the last line of a file would shed its
        // trailing comment onto a line of its own, and the next run
        // would leave it there -- two different answers for one input.
        StatementPlan *owner = nullptr;
        if (!plan->statements.empty() && src.CodeBefore(c.range.line, c.range.col)) {
            StatementPlan &last = plan->statements.back();
            if (last.range.end_line == c.range.line) owner = &last;
        }
        if (owner != nullptr && owner->trailing.empty() && owner->after.empty() && FitsOnOneLine(attached.text)) {
            owner->trailing = attached.text;
        } else if (owner != nullptr) {
            owner->after.push_back(attached);
        } else {
            plan->tail.push_back(attached);
        }
        previous_end = c.range.end_line;
    }
}

// ---------------------------------------------------------------------
// The printer
// ---------------------------------------------------------------------

// A construct nested this deep is not something a person typed, and the
// renderer is recursive: `gf` runs inside the editor, so the depth is
// capped rather than left to the stack. Same reasoning, and the same
// order of magnitude, as r_format.cpp's own cap.
constexpr int kMaxDepth = 200;

struct Printer {
    Options opt;
    const MxParseResult *parse = nullptr;
    const Source *src = nullptr;
    const CommentPlan *comments = nullptr;
    // Every operator spelling this document can lex, the file's own
    // `infix("...")` declarations included. Used only by the glue check
    // below.
    std::vector<std::string> spellings;
    std::string out;   // finished lines, each newline-terminated
    std::string line;  // the line being built, indentation included
    bool overflow = false;  // the depth cap was hit; the whole run is abandoned
    int depth = 0;

    const MxNode &At(int node) const { return parse->at(node); }

    void BuildSpellings() {
        spellings.clear();
        for (const MxOperatorInfo &op : MxOperatorTable()) spellings.emplace_back(op.text);
        if (parse != nullptr) {
            for (const MxUserOperator &op : parse->user_operators) spellings.push_back(op.text);
        }
        // Not operators, but the other three ways two pieces of
        // punctuation can weld themselves into one token.
        spellings.emplace_back("''");
        spellings.emplace_back("&&");
        spellings.emplace_back("/*");
        spellings.emplace_back("*/");
    }

    /** @brief Whether a character can be part of an operator spelling. */
    bool OperatorChar(char c) const {
        for (const std::string &spelling : spellings) {
            if (spelling.find(c) != std::string::npos && !MxIsNameStart(c)) return true;
        }
        return false;
    }

    // Wherever the style says "no space", the two spellings might weld
    // themselves into a third token. `a/_u` is the one that found this:
    // the contributed `coma` package declares `infix("/_")`, so a
    // division written tight against a name starting with `_` comes back
    // as that operator instead. Rather than special-case it, every
    // append goes through Put(), which asks whether the boundary it is
    // about to create would lex as something longer -- the same backstop
    // cpp_format.cpp needs for `>` `>` and `:` `::`.
    bool WouldGlue(char next) const {
        if (line.empty()) return false;
        size_t start = line.size();
        while (start > 0 && OperatorChar(line[start - 1])) start--;
        if (start == line.size()) return false;
        const std::string tail = line.substr(start);
        for (size_t k = 0; k < tail.size(); k++) {
            const std::string joined = tail.substr(k) + next;
            for (const std::string &spelling : spellings) {
                if (spelling.size() >= joined.size() && spelling.compare(0, joined.size(), joined) == 0) return true;
            }
        }
        return false;
    }

    /** @brief Appends token text, inserting a space where the boundary would otherwise lex differently. */
    void Put(const std::string &text) {
        if (!text.empty() && WouldGlue(text[0])) line += ' ';
        line += text;
    }

    void PushLine() {
        out += RTrim(line);
        out += '\n';
        line.clear();
    }

    void NewLine(int indent) {
        PushLine();
        line.assign(static_cast<size_t>(indent), ' ');
    }

    int Col() const { return Width(line); }

    // --- The flat form ------------------------------------------------
    //
    // One function renders any node on a single line, and it is the only
    // place spacing is decided. The multi-line renderers below choose
    // *where* to break and then call back into this for each piece, so
    // the two layouts can never disagree about whether a comma takes a
    // space.

    std::string Flat(int node) {
        Printer pr;
        pr.opt = opt;
        pr.parse = parse;
        pr.src = src;
        pr.comments = comments;
        pr.spellings = spellings;
        pr.depth = depth;
        pr.RenderFlat(node);
        if (pr.overflow) overflow = true;
        return pr.line;
    }

    /** @brief Whether a node's flat form fits in what is left of the current line. */
    bool Fits(int node, std::string *flat) {
        *flat = Flat(node);
        return !overflow && Col() + Width(*flat) <= opt.width;
    }

    void RenderFlat(int node) {
        if (node < 0 || overflow) return;
        if (depth >= kMaxDepth) {
            overflow = true;
            return;
        }
        depth++;
        FlatKind(node);
        depth--;
    }

    void FlatList(const std::vector<int> &items, const char *open, const char *close) {
        line += open;
        for (size_t i = 0; i < items.size(); i++) {
            if (i > 0) line += ", ";
            RenderFlat(items[i]);
        }
        line += close;
    }

    void FlatArgs(const MxNode &n, const char *open, const char *close) {
        line += open;
        for (size_t i = 0; i < n.args.size(); i++) {
            if (i > 0) line += ", ";
            RenderFlat(n.args[i].value);
        }
        line += close;
    }

    void FlatKind(int node) {
        const MxNode &n = At(node);
        switch (n.kind) {
            case MxNodeKind::Num:
            case MxNodeKind::Str:
            case MxNodeKind::Ident:
            case MxNodeKind::Lisp:
            case MxNodeKind::LispEsc:
            case MxNodeKind::Error: Put(src->Slice(n.range)); return;
            case MxNodeKind::Quote:
                Put(n.text);
                if (!n.kids.empty()) RenderFlat(n.kids[0]);
                return;
            case MxNodeKind::Unary:
                Put(n.text);
                // `not` is a word, so it needs the space its spelling
                // implies; `-` does not.
                if (!n.text.empty() && MxIsNameStart(n.text[0])) line += ' ';
                if (!n.kids.empty()) RenderFlat(n.kids[0]);
                return;
            case MxNodeKind::Postfix:
                if (!n.kids.empty()) RenderFlat(n.kids[0]);
                Put(n.text);
                return;
            case MxNodeKind::Binary: {
                if (n.kids.size() != 2) return;
                RenderFlat(n.kids[0]);
                if (n.text == ",") {
                    line += ", ";
                } else if (SpacedOperator(n.text)) {
                    line += ' ';
                    line += n.text;
                    line += ' ';
                } else {
                    Put(n.text);
                }
                RenderFlat(n.kids[1]);
                return;
            }
            case MxNodeKind::Call:
                if (!n.kids.empty()) RenderFlat(n.kids[0]);
                FlatArgs(n, "(", ")");
                return;
            case MxNodeKind::Index:
                if (!n.kids.empty()) RenderFlat(n.kids[0]);
                FlatArgs(n, "[", "]");
                return;
            case MxNodeKind::List: FlatList(n.kids, "[", "]"); return;
            case MxNodeKind::Set: FlatList(n.kids, "{", "}"); return;
            case MxNodeKind::Paren: FlatList(n.kids, "(", ")"); return;
            case MxNodeKind::Label:
                if (n.kids.size() == 2) {
                    RenderFlat(n.kids[0]);
                    line += " && ";
                    RenderFlat(n.kids[1]);
                }
                return;
            case MxNodeKind::If: {
                bool first = true;
                for (const MxArg &arg : n.args) {
                    if (arg.name == "cond") {
                        line += first ? "if " : " elseif ";
                        first = false;
                    } else if (arg.name == "then") {
                        line += " then ";
                    } else {
                        line += " else ";
                    }
                    RenderFlat(arg.value);
                }
                return;
            }
            case MxNodeKind::Do: {
                for (size_t i = 0; i < n.args.size(); i++) {
                    const MxArg &arg = n.args[i];
                    if (i > 0) line += ' ';
                    // `for i : 1` and `for i from 1` are the same clause
                    // to the reader, so the spelling the author used is
                    // the one that is kept -- rewriting one into the
                    // other would be a change to the token stream, not a
                    // change to the layout.
                    line += src->Slice(arg.range);
                    line += ' ';
                    RenderFlat(arg.value);
                }
                return;
            }
        }
    }

    // --- The broken forms ---------------------------------------------

    void Render(int node, int indent) {
        if (node < 0 || overflow) return;
        if (depth >= kMaxDepth) {
            overflow = true;
            return;
        }
        EmitLeading(node, indent);
        std::string flat;
        // A construct with a comment inside it can never be written on
        // one line: the flat form has nowhere to put the comment, and
        // emitting it would drop it.
        if (!DeepComment(node) && Fits(node, &flat)) {
            Put(flat);
            return;
        }
        if (overflow) return;
        depth++;
        RenderKind(node, indent);
        depth--;
    }

    /** @brief Whether anything below this node carries an attached comment. */
    bool DeepComment(int node) const {
        if (comments == nullptr || node < 0) return false;
        if (static_cast<size_t>(node) >= comments->deep.size()) return false;
        return comments->deep[static_cast<size_t>(node)];
    }

    // A comment attached to a node goes on its own line above it, at the
    // node's own indentation.
    void EmitLeading(int node, int indent) {
        if (comments == nullptr || node < 0) return;
        if (static_cast<size_t>(node) >= comments->node_leading.size()) return;
        const std::vector<AttachedComment> &list = comments->node_leading[static_cast<size_t>(node)];
        for (const AttachedComment &c : list) {
            if (Col() > indent) NewLine(indent);
            line += c.text;
            NewLine(indent);
        }
    }

    void RenderKind(int node, int indent) {
        const MxNode &n = At(node);
        switch (n.kind) {
            case MxNodeKind::Binary: RenderBinary(node, indent); return;
            case MxNodeKind::Call: RenderCall(node, indent); return;
            case MxNodeKind::Index: RenderIndex(node, indent); return;
            case MxNodeKind::List: RenderBracketed(node, indent, "[", "]"); return;
            case MxNodeKind::Set: RenderBracketed(node, indent, "{", "}"); return;
            case MxNodeKind::Paren: RenderParen(node, indent); return;
            case MxNodeKind::If: RenderIf(node, indent); return;
            case MxNodeKind::Do: RenderDo(node, indent); return;
            case MxNodeKind::Quote:
            case MxNodeKind::Unary:
                // A prefix operator cannot break; its operand can.
                Put(n.text);
                if (n.kind == MxNodeKind::Unary && !n.text.empty() && MxIsNameStart(n.text[0])) line += ' ';
                if (!n.kids.empty()) Render(n.kids[0], indent);
                return;
            case MxNodeKind::Postfix:
                if (!n.kids.empty()) Render(n.kids[0], indent);
                Put(n.text);
                return;
            case MxNodeKind::Label:
                if (n.kids.size() == 2) {
                    Render(n.kids[0], indent);
                    line += " && ";
                    Render(n.kids[1], indent);
                }
                return;
            // A leaf that does not fit is a leaf that does not fit: a
            // long string or a long name has nowhere to break.
            default: Put(src->Slice(n.range)); return;
        }
    }

    // A chain of the same operator is one group: `a+b+c` breaks at every
    // `+` or at none, rather than nesting one level deeper per operand.
    void FlattenChain(int node, const std::string &op, std::vector<int> *operands) const {
        const MxNode &n = At(node);
        if (n.kind == MxNodeKind::Binary && n.text == op && n.kids.size() == 2) {
            FlattenChain(n.kids[0], op, operands);
            operands->push_back(n.kids[1]);
            return;
        }
        operands->push_back(node);
    }

    void RenderBinary(int node, int indent) {
        const MxNode &n = At(node);
        if (n.kids.size() != 2) return;
        const std::string &op = n.text;
        const int body = indent + opt.indent;

        // An assignment or a definition keeps its left side at the
        // margin, where the name being defined can be found, and lets
        // the right side have the rest of the room.
        if (IsAssignment(op)) {
            Render(n.kids[0], indent);
            line += ' ';
            line += op;
            line += ' ';
            std::string rhs;
            // A call, a list or a block breaks inside itself, which is a
            // better place than after the `:` -- `f(x) := block(...)`
            // should not push `block` onto its own line.
            const MxNodeKind kind = At(n.kids[1]).kind;
            const bool breaks_itself = kind == MxNodeKind::Call || kind == MxNodeKind::List ||
                                       kind == MxNodeKind::Set || kind == MxNodeKind::Paren ||
                                       kind == MxNodeKind::Do || kind == MxNodeKind::If ||
                                       kind == MxNodeKind::Index;
            if (!breaks_itself && !Fits(n.kids[1], &rhs)) {
                NewLine(body);
                Render(n.kids[1], body);
                return;
            }
            Render(n.kids[1], indent);
            return;
        }

        std::vector<int> operands;
        FlattenChain(node, op, &operands);
        for (size_t i = 0; i < operands.size(); i++) {
            if (i > 0) {
                if (op == ",") {
                    line += ",";
                    NewLine(body);
                } else if (SpacedOperator(op)) {
                    // Break *before* the operator, so a long condition
                    // reads down the left edge.
                    NewLine(body);
                    line += op;
                    line += ' ';
                } else {
                    Put(op);
                    std::string item;
                    if (!Fits(operands[i], &item)) NewLine(body);
                }
            }
            Render(operands[i], body);
            if (overflow) return;
        }
    }

    // `[a, b, c]` and `{a, b}`. A list is data far more often than it is
    // structure, so when it does not fit it is packed to the margin
    // rather than exploded one element per line -- a matrix row or a
    // table of coefficients reads better that way, and it is what the C
    // formatter learned about array initializers.
    void RenderBracketed(int node, int indent, const char *open, const char *close) {
        const MxNode &n = At(node);
        line += open;
        const int body = indent + opt.indent;
        for (size_t i = 0; i < n.kids.size(); i++) {
            if (i > 0) line += ",";
            std::string item;
            const bool fits = Fits(n.kids[i], &item);
            if (i == 0) {
                NewLine(body);
            } else if (!fits || Col() + 1 + Width(item) > opt.width) {
                NewLine(body);
            } else {
                line += ' ';
            }
            Render(n.kids[i], body);
            if (overflow) return;
        }
        line += close;
    }

    // `( a, b )` -- Maxima's compound expression. Its members are
    // statements, so they go one per line.
    void RenderParen(int node, int indent) {
        const MxNode &n = At(node);
        line += "(";
        const int body = indent + opt.indent;
        for (size_t i = 0; i < n.kids.size(); i++) {
            if (i > 0) line += ",";
            NewLine(body);
            Render(n.kids[i], body);
            if (overflow) return;
        }
        line += ")";
    }

    void RenderIndex(int node, int indent) {
        const MxNode &n = At(node);
        if (!n.kids.empty()) Render(n.kids[0], indent);
        line += "[";
        RenderArguments(n, indent + opt.indent, /*one_per_line=*/false);
        line += "]";
    }

    /** @brief Whether a call's arguments are a body rather than a list of values. */
    static bool IsBlockLike(const std::string &name) {
        return name == "block" || name == "lambda" || name == "buildq" || name == "catch" || name == "errcatch";
    }

    void RenderCall(int node, int indent) {
        const MxNode &n = At(node);
        const int callee = n.kids.empty() ? -1 : n.kids[0];
        const MxNode &head = At(callee);
        const std::string name = head.kind == MxNodeKind::Ident ? head.text : std::string();
        Render(callee, indent);
        if (overflow) return;
        line += "(";
        // A `block` keeps its local list on the opening line and puts one
        // statement per line under it, which is the shape `grind` itself
        // produces and the shape every Maxima package is written in.
        RenderArguments(n, indent + opt.indent, IsBlockLike(name));
        line += ")";
    }

    // Shared by calls and subscripts. `one_per_line` explodes the
    // arguments; otherwise they are packed to the margin.
    void RenderArguments(const MxNode &n, int body, bool one_per_line) {
        for (size_t i = 0; i < n.args.size(); i++) {
            const int arg = n.args[i].value;
            if (i > 0) line += ",";
            std::string item;
            const bool fits = Fits(arg, &item);
            if (i == 0) {
                // A block's local list rides the opening line when it
                // fits there; a plain call's arguments start a fresh one.
                if (!(one_per_line && fits)) NewLine(body);
            } else if (one_per_line || !fits || Col() + 1 + Width(item) > opt.width) {
                NewLine(body);
            } else {
                line += ' ';
            }
            Render(arg, body);
            if (overflow) return;
        }
    }

    // `if c then a elseif d then b else e`. When it does not fit, the
    // break goes before each keyword: that is how the shipped packages
    // write a long conditional, and it lines the branches up under one
    // another.
    void RenderIf(int node, int indent) {
        const MxNode &n = At(node);
        const int body = indent + opt.indent;
        bool first_branch = true;
        for (const MxArg &arg : n.args) {
            if (arg.name == "cond") {
                if (first_branch) {
                    line += "if ";
                } else {
                    NewLine(indent);
                    line += "elseif ";
                }
                first_branch = false;
                Render(arg.value, body);
            } else if (arg.name == "then") {
                NewLine(body);
                line += "then ";
                Render(arg.value, body + opt.indent);
            } else {
                NewLine(body);
                line += "else ";
                Render(arg.value, body + opt.indent);
            }
            if (overflow) return;
        }
    }

    // Every Maxima loop is a bag of clauses closed by `do`. The clauses
    // stay on one line for as long as they fit; the body is what breaks.
    void RenderDo(int node, int indent) {
        const MxNode &n = At(node);
        const int body = indent + opt.indent;
        for (size_t i = 0; i < n.args.size(); i++) {
            const MxArg &arg = n.args[i];
            if (i > 0) line += ' ';
            line += src->Slice(arg.range);
            line += ' ';
            if (arg.name == "do") {
                std::string item;
                const bool fits = Fits(arg.value, &item);
                const MxNodeKind kind = At(arg.value).kind;
                const bool breaks_itself = kind == MxNodeKind::Paren || kind == MxNodeKind::Call ||
                                           kind == MxNodeKind::Do || kind == MxNodeKind::If;
                if (!fits && !breaks_itself) {
                    NewLine(body);
                    Render(arg.value, body);
                } else {
                    Render(arg.value, indent);
                }
            } else {
                Render(arg.value, body);
            }
            if (overflow) return;
        }
    }
};

// ---------------------------------------------------------------------
// The self-check
// ---------------------------------------------------------------------

// A formatter for this language only ever moves whitespace, so the
// output must lex to exactly the same tokens as the input -- no
// tolerance list, the way cpp_format.cpp's check needs none. Comment
// spellings are compared with their internal whitespace collapsed, since
// trailing spaces inside a comment are the one thing that is removed.
std::string CollapseSpace(const std::string &s) {
    std::string out;
    bool space = false;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            space = true;
            continue;
        }
        if (space && !out.empty()) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

struct Signature {
    std::vector<std::pair<int, std::string>> items;  // (kind, spelling)
};

// `user_ops` must be the operator table the *parse* used, not an empty
// one: a file that declares `infix("/_")` lexes `a/_b` as one operator,
// and a check that did not know that would compare two wrong readings to
// each other and call them equal.
Signature Sign(const std::vector<std::string> &lines, const std::vector<MxUserOperator> &user_ops) {
    Signature sig;
    std::vector<MxComment> comments;
    const std::vector<MxToken> tokens = MxTokenize(lines, &comments, nullptr, &user_ops);
    for (const MxToken &t : tokens) {
        if (t.kind == MxTokKind::End) continue;
        sig.items.emplace_back(static_cast<int>(t.kind), t.text);
    }
    // Comments are compared as their own stream: their position relative
    // to the tokens can legitimately change (a trailing comment moves
    // when the line it trails is rewrapped), but their text and their
    // order cannot.
    for (const MxComment &c : comments) sig.items.emplace_back(-1, CollapseSpace(c.text));
    return sig;
}

bool SameSignature(const Signature &a, const Signature &b, size_t *where) {
    if (a.items.size() != b.items.size()) {
        *where = std::min(a.items.size(), b.items.size());
        return false;
    }
    for (size_t i = 0; i < a.items.size(); i++) {
        if (a.items[i] != b.items[i]) {
            *where = i;
            return false;
        }
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------

Result Format(const std::string &src, const Options &opts) {
    Result result;
    result.text = src;

    const std::vector<std::string> lines = SplitLines(src);
    const MxParseResult parse = MxParse(lines);
    for (const MxParseError &e : parse.errors) {
        // A note about an operator a package declares elsewhere is not a
        // reason to refuse to format: the file reads fine, and the note
        // is the language server's business.
        if (e.code == "undeclared-operator") continue;
        result.error = e.message;
        result.error_line = e.range.line + 1;
        return result;
    }

    Source source(lines);
    CommentPlan plan;
    for (const MxStatement &st : parse.statements) {
        if (st.node < 0) continue;
        StatementPlan statement;
        statement.node = st.node;
        statement.range = st.range;
        statement.terminated = st.terminated;
        statement.terminator = st.terminator;
        plan.statements.push_back(statement);
    }
    AttachComments(parse, source, &plan);

    Printer pr;
    pr.opt = opts;
    pr.parse = &parse;
    pr.src = &source;
    pr.comments = &plan;
    pr.BuildSpellings();
    for (size_t i = 0; i < plan.statements.size(); i++) {
        const StatementPlan &statement = plan.statements[i];
        if (i > 0 && statement.blanks_before > 0) pr.out += '\n';
        for (size_t c = 0; c < statement.leading.size(); c++) {
            if (c > 0 && statement.leading[c].blanks_before > 0) pr.out += '\n';
            pr.line = statement.leading[c].text;
            pr.PushLine();
        }
        if (!statement.leading.empty() && statement.blanks_after_leading > 0) pr.out += '\n';
        // A `:lisp` line is host-Lisp source; it is emitted exactly as
        // it was written, because nothing here reads it.
        if (parse.at(statement.node).kind == MxNodeKind::LispEsc) {
            pr.line = RTrim(source.Slice(parse.at(statement.node).range));
            pr.PushLine();
            continue;
        }
        pr.line.clear();
        pr.Render(statement.node, 0);
        if (pr.overflow) {
            result.error = "expression nested too deeply to format";
            result.error_line = statement.range.line + 1;
            return result;
        }
        if (statement.terminated) pr.line += statement.terminator;
        if (!statement.trailing.empty()) {
            pr.line += ' ';
            pr.line += statement.trailing;
        }
        pr.PushLine();
        for (const AttachedComment &c : statement.after) {
            pr.line = c.text;
            pr.PushLine();
        }
    }
    for (const AttachedComment &c : plan.tail) {
        if (c.blanks_before > 0 && !pr.out.empty()) pr.out += '\n';
        pr.line = c.text;
        pr.PushLine();
    }

    std::string formatted = pr.out;
    if (formatted.empty()) formatted = "\n";
    // Exactly one trailing newline, whatever the input had.
    while (formatted.size() > 1 && formatted[formatted.size() - 1] == '\n' &&
           formatted[formatted.size() - 2] == '\n') {
        formatted.pop_back();
    }

    size_t where = 0;
    const Signature before = Sign(lines, parse.user_operators);
    const Signature after = Sign(SplitLines(formatted), parse.user_operators);
    if (!SameSignature(before, after, &where)) {
        result.error = "internal formatter error: the output does not match the input token for token";
        result.error_line = 0;
        return result;
    }

    result.ok = true;
    result.text = formatted;
    return result;
}

}  // namespace mxfmt
