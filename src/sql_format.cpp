// mep's SQL formatter: see sql_format.h.
//
// A writer over the token stream of sql_ast.h's parse. Every token goes out
// through one cursor (next_) in order, so nothing can be dropped or
// reordered; the layout only decides where the line breaks and the spaces
// go. The parse supplies the structure the token stream alone does not
// show: where each statement, CREATE TABLE column definition and trigger
// body statement begins and ends, and which keyword tokens are really
// names (a column called `key`, the replace() function) and so keep the
// case they were written in.
#include "sql_format.h"

#include <algorithm>
#include <set>
#include <vector>

#include "sql_ast.h"

namespace sqlfmt {

namespace {

// --- names: the keyword tokens the parse used as identifiers ----------------

void Name(const SqlName &n, std::set<int> *out) {
    if (n.ok()) out->insert(n.tok);
}

void SelectNames(const SqlSelect &s, std::set<int> *out);

void ExprNames(const SqlExpr *e, std::set<int> *out) {
    if (!e) return;
    Name(e->db, out);
    Name(e->table, out);
    Name(e->column, out);
    Name(e->func, out);
    Name(e->in_db, out);
    Name(e->in_table, out);
    Name(e->over_name, out);
    Name(e->collation, out);
    for (const SqlExprP &a : e->args) ExprNames(a.get(), out);
    for (const SqlOrderTerm &o : e->order) ExprNames(o.expr.get(), out);
    ExprNames(e->filter.get(), out);
    if (e->over) {
        Name(e->over->base, out);
        for (const SqlExprP &a : e->over->partition) ExprNames(a.get(), out);
        for (const SqlOrderTerm &o : e->over->order) ExprNames(o.expr.get(), out);
        for (const SqlExprP &a : e->over->frame) ExprNames(a.get(), out);
    }
    if (e->select) SelectNames(*e->select, out);
}

void FromNames(const std::vector<SqlFromItem> &items, std::set<int> *out) {
    for (const SqlFromItem &f : items) {
        Name(f.db, out);
        Name(f.name, out);
        Name(f.alias, out);
        Name(f.indexed, out);
        for (const SqlName &u : f.using_cols) Name(u, out);
        for (const SqlExprP &a : f.args) ExprNames(a.get(), out);
        if (f.select) SelectNames(*f.select, out);
        FromNames(f.group, out);
        ExprNames(f.on.get(), out);
    }
}

void WithNames(const SqlWith *w, std::set<int> *out) {
    if (!w) return;
    for (const SqlCte &c : w->ctes) {
        Name(c.name, out);
        for (const SqlName &n : c.columns) Name(n, out);
        if (c.select) SelectNames(*c.select, out);
    }
}

void SelectNames(const SqlSelect &s, std::set<int> *out) {
    WithNames(s.with.get(), out);
    for (const SqlCore &c : s.cores) {
        for (const auto &row : c.rows) {
            for (const SqlExprP &e : row) ExprNames(e.get(), out);
        }
        for (const SqlResultCol &r : c.cols) {
            ExprNames(r.expr.get(), out);
            Name(r.alias, out);
            Name(r.star_table, out);
        }
        FromNames(c.from, out);
        ExprNames(c.where.get(), out);
        for (const SqlExprP &e : c.group) ExprNames(e.get(), out);
        ExprNames(c.having.get(), out);
        for (const SqlWindowDef &w : c.windows) {
            Name(w.name, out);
            Name(w.spec.base, out);
            for (const SqlExprP &a : w.spec.partition) ExprNames(a.get(), out);
            for (const SqlOrderTerm &o : w.spec.order) ExprNames(o.expr.get(), out);
            for (const SqlExprP &a : w.spec.frame) ExprNames(a.get(), out);
        }
        for (const SqlOrderTerm &o : c.order) ExprNames(o.expr.get(), out);
        ExprNames(c.limit.get(), out);
        ExprNames(c.offset.get(), out);
    }
}

void StmtNames(const SqlStmt &st, std::set<int> *out) {
    WithNames(st.with.get(), out);
    if (st.select) SelectNames(*st.select, out);
    Name(st.db, out);
    Name(st.name, out);
    Name(st.alias, out);
    for (const SqlName &n : st.columns) Name(n, out);
    for (const SqlColumnDef &cd : st.coldefs) {
        Name(cd.name, out);
        for (const SqlExprP &e : cd.exprs) ExprNames(e.get(), out);
        Name(cd.ref_table, out);
        for (const SqlName &n : cd.ref_cols) Name(n, out);
        Name(cd.collation, out);
    }
    for (const SqlTableConstraint &tc : st.constraints) {
        Name(tc.name, out);
        for (const SqlOrderTerm &o : tc.terms) ExprNames(o.expr.get(), out);
        ExprNames(tc.check.get(), out);
        for (const SqlName &n : tc.cols) Name(n, out);
        Name(tc.ref_table, out);
        for (const SqlName &n : tc.ref_cols) Name(n, out);
    }
    for (const SqlSetItem &si : st.set) {
        for (const SqlName &n : si.cols) Name(n, out);
        ExprNames(si.value.get(), out);
    }
    FromNames(st.from, out);
    ExprNames(st.where.get(), out);
    for (const SqlResultCol &r : st.returning) {
        ExprNames(r.expr.get(), out);
        Name(r.alias, out);
    }
    for (const SqlUpsert &u : st.upserts) {
        for (const SqlOrderTerm &o : u.target) ExprNames(o.expr.get(), out);
        ExprNames(u.target_where.get(), out);
        for (const SqlSetItem &si : u.set) {
            for (const SqlName &n : si.cols) Name(n, out);
            ExprNames(si.value.get(), out);
        }
        ExprNames(u.where.get(), out);
    }
    for (const SqlOrderTerm &o : st.order) ExprNames(o.expr.get(), out);
    ExprNames(st.limit.get(), out);
    ExprNames(st.offset.get(), out);
    Name(st.on_db, out);
    Name(st.on_table, out);
    for (const SqlOrderTerm &o : st.index_terms) ExprNames(o.expr.get(), out);
    for (const SqlStmt &b : st.body) StmtNames(b, out);
    ExprNames(st.when.get(), out);
    Name(st.module, out);
    Name(st.column, out);
    Name(st.new_name, out);
    if (st.add_column) {
        Name(st.add_column->name, out);
        for (const SqlExprP &e : st.add_column->exprs) ExprNames(e.get(), out);
        Name(st.add_column->ref_table, out);
    }
    for (const SqlExprP &e : st.exprs) ExprNames(e.get(), out);
}

// --- the writer ----------------------------------------------------------------

class Writer {
public:
    Writer(const SqlParse &p, const Options &o) : p_(p), o_(o), lines_(p.text) {
        for (const SqlStmt &st : p.stmts) StmtNames(st, &names_);
    }

    std::string Run() {
        const int eof = static_cast<int>(p_.tokens.size()) - 1;
        size_t si = 0;
        for (size_t ci = 0; ci < p_.chunks.size(); ci++) {
            const SqlChunk &c = p_.chunks[ci];
            // A comment trailing the previous statement on its line stays there.
            while (next_ < c.end_tok && Type(next_) == SqlTok::Comment && last_ >= 0 &&
                   LineOf(next_) == EndLineOf(last_)) {
                Tok(next_);
            }
            if (next_ >= c.end_tok) continue;
            if (!out_.empty() || HasText()) {
                const int gap = LineOf(next_) - EndLineOf(last_);
                Break(0);
                if (gap >= 2) blank_ = true;
            }
            const SqlStmt *st = nullptr;
            while (si < p_.stmts.size() && p_.stmts[si].first < c.end_tok) {
                if (p_.stmts[si].first >= c.first_tok) {
                    st = &p_.stmts[si];
                    si++;
                    break;
                }
                si++;
            }
            if (st && st->kind != SqlStmt::Empty && st->first >= 0) {
                Through(st->first - 1);  // leading comments
                if (next_ == st->first && last_ >= 0 && Type(last_) == SqlTok::Comment) {
                    if (LineOf(next_) > EndLineOf(last_)) Break(0);
                    if (LineOf(next_) - EndLineOf(last_) >= 2) blank_ = true;
                }
                Statement(*st, 0, c.end_tok);
            }
            Through(c.end_tok - 1);
        }
        Through(eof - 1);
        Flush();
        std::string text;
        for (size_t i = 0; i < out_.size(); i++) {
            if (i) text += '\n';
            text += out_[i];
        }
        return text;
    }

private:
    const SqlParse &p_;
    const Options &o_;
    SqlLineIndex lines_;
    std::set<int> names_;
    std::vector<std::string> out_;
    std::string cur_;
    bool cur_has_ = false;  // cur_ holds a token (not just indentation)
    int cur_indent_ = 0;
    int next_ = 0;   // the next token to emit
    int last_ = -1;  // the last token emitted
    bool pending_break_ = false;  // a line comment was emitted: the next token starts a line
    bool blank_ = false;          // a blank line goes before the next line
    bool unary_ = false;          // the last token was a prefix operator

    // --- tokens ---

    SqlTok Type(int i) const { return p_.tokens[static_cast<size_t>(i)].type; }
    int LineOf(int i) const { return lines_.Line(p_.tokens[static_cast<size_t>(i)].start); }
    int EndLineOf(int i) const {
        const SqlToken &t = p_.tokens[static_cast<size_t>(i)];
        return lines_.Line(t.start + std::max(0, t.len - 1));
    }
    bool IsLineComment(int i) const {
        return Type(i) == SqlTok::Comment && p_.text.compare(static_cast<size_t>(p_.tokens[static_cast<size_t>(i)].start), 2, "--") == 0;
    }
    /** @brief The next non-comment token at or after i (stopping at end). */
    int Sig(int i, int end) const {
        while (i < end && Type(i) == SqlTok::Comment) i++;
        return i;
    }

    std::string TextOf(int i) const {
        const SqlTok t = Type(i);
        std::string s(p_.Text(i));
        if (o_.upper_keywords && SqlIsKeyword(t) && !names_.count(i)) {
            for (char &c : s) {
                if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
            }
        }
        return s;
    }

    static bool Operand(SqlTok t) {
        return t == SqlTok::Id || t == SqlTok::String || t == SqlTok::Integer || t == SqlTok::Float ||
               t == SqlTok::QNumber || t == SqlTok::Blob || t == SqlTok::Variable || t == SqlTok::RP ||
               t == SqlTok::Null || t == SqlTok::End || t == SqlTok::CtimeKw;
    }

    bool NameLike(int i) const { return Type(i) == SqlTok::Id || (SqlIsKeyword(Type(i)) && names_.count(i)); }

    /** @brief Whether `(` right after token prev is a call's (no space) rather than a list's. */
    bool CallParen(int prev) const {
        const SqlTok t = Type(prev);
        if (t == SqlTok::Cast || t == SqlTok::Raise) return true;
        if (!NameLike(prev) && !(SqlIsKeyword(t) && SqlIsFallback(t) && names_.count(prev))) return false;
        if (t == SqlTok::String) return false;
        // `INSERT INTO t (a, b)`, `REFERENCES p (id)`, `ON t (a)`, `TABLE t (`, `VIEW v (`: a name and its list.
        int b = prev - 1;
        while (b >= 0 && Type(b) == SqlTok::Comment) b--;
        if (b >= 1 && Type(b) == SqlTok::Dot) {
            b -= 2;
            while (b >= 0 && Type(b) == SqlTok::Comment) b--;
        }
        if (b < 0) return true;
        const SqlTok bt = Type(b);
        return !(bt == SqlTok::Into || bt == SqlTok::Table || bt == SqlTok::References || bt == SqlTok::On ||
                 bt == SqlTok::View || bt == SqlTok::Exists || bt == SqlTok::Index || bt == SqlTok::Replace ||
                 (bt == SqlTok::Id && Type(prev) == SqlTok::Id && false));
    }

    bool SpaceBetween(int prev, int cur, bool prev_unary) const {
        const SqlTok a = Type(prev), b = Type(cur);
        if (b == SqlTok::Comment || a == SqlTok::Comment) return true;
        if (b == SqlTok::Comma || b == SqlTok::Semi || b == SqlTok::RP || b == SqlTok::Dot) return false;
        if (a == SqlTok::LP || a == SqlTok::Dot) return false;
        if (prev_unary) return b == SqlTok::Minus || b == SqlTok::Plus || SqlIsKeyword(b);
        if (b == SqlTok::LP) return !CallParen(prev);
        return true;
    }

    // --- output ---

    std::string Indent(int n) const {
        if (o_.use_tabs) return std::string(static_cast<size_t>(n), '\t');
        return std::string(static_cast<size_t>(n * o_.indent_width), ' ');
    }
    int Col() const {
        int c = 0;
        for (char ch : cur_) c += ch == '\t' ? o_.indent_width : 1;
        return c;
    }
    bool HasText() const { return cur_has_; }

    void Flush() {
        if (!cur_has_) return;
        if (blank_ && !out_.empty()) out_.emplace_back();
        blank_ = false;
        size_t e = cur_.size();
        while (e > 0 && (cur_[e - 1] == ' ' || cur_[e - 1] == '\t')) e--;
        out_.push_back(cur_.substr(0, e));
        cur_.clear();
        cur_has_ = false;
    }

    /** @brief Ends the current line (when it has anything) and starts one at `indent`. */
    void Break(int indent) {
        Flush();
        cur_ = Indent(indent);
        cur_indent_ = indent;
        pending_break_ = false;
    }

    /** @brief Emits token i, which must be next_. */
    void Tok(int i) {
        const SqlTok t = Type(i);
        if (t == SqlTok::Eof) {
            next_ = i + 1;
            return;
        }
        // A blank line the source had next to a comment survives (at most one): it is what separates a
        // section's heading comment from the statement under it.
        const bool blank_gap = last_ >= 0 && LineOf(i) - EndLineOf(last_) >= 2 &&
                               (t == SqlTok::Comment || Type(last_) == SqlTok::Comment);
        if (pending_break_) {
            Break(cur_indent_);
            if (blank_gap) blank_ = true;
        }
        if (t == SqlTok::Comment && cur_has_ && last_ >= 0 && LineOf(i) > EndLineOf(last_)) {
            Break(cur_indent_);  // a comment that had its own line keeps it
            if (blank_gap) blank_ = true;
        } else if (cur_has_ && last_ >= 0 && Type(last_) == SqlTok::Comment && LineOf(i) > EndLineOf(last_)) {
            Break(cur_indent_);  // ... and so does what followed it on the next line
            if (blank_gap) blank_ = true;
        }
        if (cur_has_ && SpaceBetween(last_, i, unary_)) cur_ += ' ';
        const bool was_operand = last_ >= 0 && cur_has_ && Operand(Type(last_));
        cur_ += TextOf(i);
        cur_has_ = true;
        unary_ = (t == SqlTok::Minus || t == SqlTok::Plus || t == SqlTok::BitNot) && !was_operand &&
                 !(last_ >= 0 && NameLike(last_));
        if (t == SqlTok::Comment && IsLineComment(i)) pending_break_ = true;
        if (t == SqlTok::Comment && !IsLineComment(i) && EndLineOf(i) > LineOf(i)) pending_break_ = true;
        last_ = i;
        next_ = i + 1;
    }

    /**
     * @brief Copies tokens [a, b] exactly as written, the whitespace between them included: a virtual table's
     *        module arguments, which the module (not SQLite) parses.
     */
    void Verbatim(int a, int b) {
        Through(a - 1);
        if (b < a) return;
        Tok(a);
        const SqlToken &ta = p_.tokens[static_cast<size_t>(a)];
        const SqlToken &tb = p_.tokens[static_cast<size_t>(b)];
        const size_t from = static_cast<size_t>(ta.start + ta.len);
        const std::string raw = p_.text.substr(from, static_cast<size_t>(tb.start + tb.len) - from);
        size_t s = 0;
        while (true) {
            const size_t e = raw.find('\n', s);
            if (e == std::string::npos) {
                cur_ += raw.substr(s);
                break;
            }
            cur_ += raw.substr(s, e - s);
            // keep the line as it is (no trimming: the text belongs to the module)
            if (blank_ && !out_.empty()) out_.emplace_back();
            blank_ = false;
            out_.push_back(cur_);
            cur_.clear();
            s = e + 1;
        }
        cur_has_ = true;
        last_ = b;
        next_ = b + 1;
        unary_ = false;
        pending_break_ = false;
    }

    /** @brief Emits every token up to and including i. */
    void Through(int i) {
        while (next_ <= i) Tok(next_);
    }

    // --- measuring ---

    /** @brief Whether [a, b) must break: a line comment, or a comment that had a line of its own. */
    bool Forced(int a, int b) const {
        for (int i = a; i < b; i++) {
            if (Type(i) != SqlTok::Comment) continue;
            if (IsLineComment(i) || EndLineOf(i) > LineOf(i)) return true;
            if (i > 0 && LineOf(i) > EndLineOf(i - 1)) return true;
            if (i + 1 < b && LineOf(i + 1) > EndLineOf(i)) return true;
        }
        return false;
    }

    /** @brief The width [a, b) takes on one line (the leading space included when the line has text). */
    int FlatLen(int a, int b) const {
        int n = 0;
        int prev = a > 0 ? a - 1 : -1;
        bool un = unary_;
        bool started = cur_has_;
        if (a == next_) prev = last_;
        for (int i = a; i < b; i++) {
            if (Type(i) == SqlTok::Eof) break;
            if (started && prev >= 0 && SpaceBetween(prev, i, un)) n++;
            n += static_cast<int>(TextOf(i).size());
            const SqlTok t = Type(i);
            un = (t == SqlTok::Minus || t == SqlTok::Plus || t == SqlTok::BitNot) &&
                 !(prev >= 0 && (Operand(Type(prev)) || NameLike(prev)));
            prev = i;
            started = true;
        }
        return n;
    }

    bool Fits(int a, int b, int extra = 0) const {
        return !Forced(a, b) && Col() + FlatLen(a, b) + extra <= o_.width;
    }

    /** @brief The `)` matching the `(` at i (or end). */
    int Match(int i, int end) const {
        int depth = 0;
        for (int k = i; k < end; k++) {
            if (Type(k) == SqlTok::LP) depth++;
            if (Type(k) == SqlTok::RP && --depth == 0) return k;
        }
        return end;
    }

    /** @brief Top-level comma positions in [a, b). */
    std::vector<int> Commas(int a, int b) const {
        std::vector<int> out;
        int depth = 0, cases = 0;
        for (int k = a; k < b; k++) {
            const SqlTok t = Type(k);
            if (t == SqlTok::LP) depth++;
            if (t == SqlTok::RP) depth--;
            if (t == SqlTok::Case) cases++;
            if (t == SqlTok::End && cases > 0) cases--;
            if (t == SqlTok::Comma && depth == 0 && cases == 0) out.push_back(k);
        }
        return out;
    }

    bool IsQuery(int a, int b) const {
        const int s = Sig(a, b);
        if (s >= b) return false;
        const SqlTok t = Type(s);
        return t == SqlTok::Select || t == SqlTok::With || t == SqlTok::Values;
    }

    // --- expressions ---

    /**
     * @brief Emits [a, b) starting on the current line, an expression or list item whose line starts at `indent`:
     *        flat when it fits, else broken before top-level AND/OR, inside parentheses, and across CASE arms.
     */
    void Expr(int a, int b, int indent) {
        if (a >= b) return;
        if (Fits(a, b)) {
            Through(b - 1);
            return;
        }
        // AND / OR at the top level (not BETWEEN's AND).
        std::vector<int> splits;
        {
            int depth = 0, cases = 0, between = 0;
            for (int k = a; k < b; k++) {
                const SqlTok t = Type(k);
                if (t == SqlTok::LP) depth++;
                if (t == SqlTok::RP) depth--;
                if (depth) continue;
                if (t == SqlTok::Case) cases++;
                if (t == SqlTok::End && cases > 0) cases--;
                if (cases) continue;
                if (t == SqlTok::Between) between++;
                if (t == SqlTok::And && between > 0) {
                    between--;
                    continue;
                }
                if ((t == SqlTok::And || t == SqlTok::Or) && k > a) splits.push_back(k);
            }
        }
        if (!splits.empty()) {
            Walk(a, splits[0], indent);
            for (size_t s = 0; s < splits.size(); s++) {
                const int e = s + 1 < splits.size() ? splits[s + 1] : b;
                Break(indent + 1);
                Through(splits[s]);
                Walk(splits[s] + 1, e, indent + 1);
            }
            return;
        }
        Walk(a, b, indent);
    }

    /** @brief Token by token: parentheses and CASE that do not fit are broken, everything else flows. */
    void Walk(int a, int b, int indent) {
        int i = a;
        while (i < b) {
            const SqlTok t = Type(i);
            if (t == SqlTok::LP) {
                const int j = Match(i, b);
                if (j >= b) {
                    Through(i);
                    i++;
                    continue;
                }
                // The tokens right after `)` that belong on its line (an alias, `AS x`) count towards fitting.
                if (IsQuery(i + 1, j)) {
                    if (Fits(i, j + 1) && FlatLen(i, j + 1) <= 50) {
                        Through(j);
                    } else {
                        Through(i);
                        Break(indent + 1);
                        Query(i + 1, j, indent + 1);
                        Break(indent);
                        Through(j);
                    }
                    i = j + 1;
                    continue;
                }
                if (Fits(i, j + 1)) {
                    Through(j);
                    i = j + 1;
                    continue;
                }
                Through(i);
                List(i + 1, j, indent + 1, true);
                Break(indent);
                Through(j);
                i = j + 1;
                continue;
            }
            if (t == SqlTok::Case) {
                int depth = 0, end = b;
                for (int k = i; k < b; k++) {
                    if (Type(k) == SqlTok::Case) depth++;
                    if (Type(k) == SqlTok::End && --depth == 0) {
                        end = k;
                        break;
                    }
                }
                if (end >= b || Fits(i, end + 1)) {
                    Through(end >= b ? i : end);
                    i = (end >= b ? i : end) + 1;
                    continue;
                }
                Case(i, end, indent);
                i = end + 1;
                continue;
            }
            Through(i);
            i++;
        }
    }

    void Case(int c, int end, int indent) {
        // CASE [operand] / WHEN .. THEN .. / ELSE .. / END
        std::vector<int> arms;
        int depth = 0, cases = 0;
        for (int k = c + 1; k < end; k++) {
            const SqlTok t = Type(k);
            if (t == SqlTok::LP) depth++;
            if (t == SqlTok::RP) depth--;
            if (depth) continue;
            if (t == SqlTok::Case) cases++;
            if (t == SqlTok::End && cases > 0) {
                cases--;
                continue;
            }
            if (cases) continue;
            if (t == SqlTok::When || t == SqlTok::Else) arms.push_back(k);
        }
        const int first = arms.empty() ? end : arms[0];
        Through(c);
        Expr(c + 1, first, indent + 1);
        for (size_t s = 0; s < arms.size(); s++) {
            const int e = s + 1 < arms.size() ? arms[s + 1] : end;
            Break(indent + 1);
            Expr(arms[s], e, indent + 1);
        }
        Break(indent);
        Through(end);
    }

    /**
     * @brief A comma list [a, b): one item per line at `indent` with trailing commas (its caller already decided it
     *        does not fit), comments after a comma staying on that item's line.
     */
    void List(int a, int b, int indent, bool break_first) {
        std::vector<int> commas = Commas(a, b);
        int s = a;
        for (size_t k = 0; k <= commas.size(); k++) {
            const int e = k < commas.size() ? commas[k] : b;
            if (k > 0 || break_first) Break(indent);
            Expr(s, e, indent);
            if (k < commas.size()) {
                Through(e);
                int n = e + 1;
                while (n < b && Type(n) == SqlTok::Comment && LineOf(n) == EndLineOf(e)) {
                    Through(n);
                    n++;
                }
                s = n;
            }
        }
    }

    // --- statements ---

    struct Clause {
        int head = 0;      // first token of the clause keyword(s)
        int body = 0;      // first token after them
        int end = 0;
        enum Kind { Other, List, Cond, Join, Compound, With, Values } kind = Other;
    };

    bool Keyword(int i, SqlTok t, int end) const { return i < end && Type(i) == t && !names_.count(i); }

    /** @brief Splits a query-like statement [a, end) at its top-level clause keywords. */
    std::vector<Clause> Clauses(int a, int end) const {
        std::vector<Clause> out;
        int depth = 0, cases = 0;
        int prev_sig = -1;
        for (int k = a; k < end; k++) {
            const SqlTok t = Type(k);
            if (t == SqlTok::Comment) continue;
            if (t == SqlTok::LP) depth++;
            if (t == SqlTok::RP) depth--;
            if (t == SqlTok::Case && !depth) cases++;
            if (t == SqlTok::End && !depth && cases > 0) cases--;
            const int ps = prev_sig;
            prev_sig = k;
            if (depth || cases || names_.count(k)) continue;
            Clause c;
            c.head = k;
            int h = k + 1;
            auto take = [&](SqlTok want) {
                const int n = Sig(h, end);
                if (n < end && Type(n) == want) {
                    h = n + 1;
                    return true;
                }
                return false;
            };
            const SqlTok pt = ps >= 0 ? Type(ps) : SqlTok::Eof;
            switch (t) {
                case SqlTok::With:
                    take(SqlTok::Recursive);
                    c.kind = Clause::With;
                    break;
                case SqlTok::Select:
                    if (!take(SqlTok::Distinct)) take(SqlTok::All);
                    c.kind = Clause::List;
                    break;
                case SqlTok::From:
                    if (pt == SqlTok::Distinct || pt == SqlTok::Delete) continue;
                    c.kind = Clause::List;
                    break;
                case SqlTok::Where:
                case SqlTok::Having:
                    c.kind = Clause::Cond;
                    break;
                case SqlTok::Group:
                case SqlTok::Order:
                    take(SqlTok::By);
                    c.kind = Clause::List;
                    break;
                case SqlTok::Window:
                case SqlTok::Returning:
                case SqlTok::Set:
                    if (t == SqlTok::Set && pt != SqlTok::Id && !SqlIsKeyword(pt) && pt != SqlTok::String) continue;
                    c.kind = Clause::List;
                    break;
                case SqlTok::Limit:
                    break;
                case SqlTok::JoinKw:
                case SqlTok::Join:
                    if (t == SqlTok::JoinKw && SqlIsKeyword(pt) && Type(ps) == SqlTok::JoinKw) continue;
                    while (true) {
                        const int n = Sig(h, end);
                        if (n < end && (Type(n) == SqlTok::JoinKw || (t == SqlTok::JoinKw && Type(n) == SqlTok::Join))) {
                            h = n + 1;
                            if (Type(n) == SqlTok::Join) break;
                            continue;
                        }
                        break;
                    }
                    if (t == SqlTok::JoinKw && Type(h - 1) != SqlTok::Join) continue;  // `NATURAL` alone?
                    c.kind = Clause::Join;
                    break;
                case SqlTok::Union:
                    take(SqlTok::All);
                    c.kind = Clause::Compound;
                    break;
                case SqlTok::Intersect:
                case SqlTok::Except:
                    c.kind = Clause::Compound;
                    break;
                case SqlTok::Values:
                    if (pt == SqlTok::Default) continue;
                    c.kind = Clause::Values;
                    break;
                case SqlTok::Default:
                    if (!take(SqlTok::Values)) continue;
                    break;
                case SqlTok::Insert:
                case SqlTok::Replace:
                    if (t == SqlTok::Replace && pt == SqlTok::Or) continue;
                    if (take(SqlTok::Or)) {
                        const int n = Sig(h, end);
                        if (n < end) h = n + 1;
                    }
                    take(SqlTok::Into);
                    break;
                case SqlTok::Update:
                    if (take(SqlTok::Or)) {
                        const int n = Sig(h, end);
                        if (n < end) h = n + 1;
                    }
                    if (pt == SqlTok::Do || pt == SqlTok::On) continue;
                    break;
                case SqlTok::Delete:
                    if (pt == SqlTok::On) continue;
                    take(SqlTok::From);
                    break;
                case SqlTok::On:
                    if (!take(SqlTok::Conflict)) continue;
                    break;
                default:
                    continue;
            }
            c.body = h;
            out.push_back(c);
            // the head's other words are not clauses of their own
            for (int s = k + 1; s < h; s++) {
                if (Type(s) != SqlTok::Comment) prev_sig = s;
            }
            k = h - 1;
        }
        for (size_t i = 0; i < out.size(); i++) out[i].end = i + 1 < out.size() ? out[i + 1].head : end;
        return out;
    }

    /** @brief A SELECT / VALUES / INSERT / UPDATE / DELETE (with its WITH), from the current position. */
    void Query(int a, int end, int indent) {
        std::vector<Clause> cl = Clauses(a, end);
        if (cl.empty() || cl[0].head != Sig(a, end)) {
            Expr(a, cl.empty() ? end : cl[0].head, indent);
        }
        for (size_t i = 0; i < cl.size(); i++) {
            const Clause &c = cl[i];
            if (i > 0 || HasTextBeyondIndent()) {
                if (i > 0) Break(indent);
            }
            Through(c.head);
            if (c.kind == Clause::With) {
                WithClause(c.head + 1, c.end, indent);
                continue;
            }
            Through(c.body - 1);
            int body_end = c.end;
            if (c.kind == Clause::Compound) continue;
            switch (c.kind) {
                case Clause::List:
                case Clause::Values:
                    if (Fits(c.body, body_end)) {
                        Through(body_end - 1);
                    } else if (c.kind == Clause::Values) {
                        List(c.body, body_end, indent + 1, true);
                    } else if (Commas(c.body, body_end).empty()) {
                        Expr(c.body, body_end, indent);
                    } else {
                        List(c.body, body_end, indent + 1, true);
                    }
                    break;
                case Clause::Cond:
                    Expr(c.body, body_end, indent);
                    break;
                case Clause::Join: {
                    if (Fits(c.body, body_end)) {
                        Through(body_end - 1);
                        break;
                    }
                    int on = -1;
                    int depth = 0;
                    for (int k = c.body; k < body_end; k++) {
                        if (Type(k) == SqlTok::LP) depth++;
                        if (Type(k) == SqlTok::RP) depth--;
                        if (!depth && (Type(k) == SqlTok::On || Type(k) == SqlTok::Using) && !names_.count(k)) {
                            on = k;
                            break;
                        }
                    }
                    if (on < 0) {
                        Expr(c.body, body_end, indent);
                    } else {
                        Expr(c.body, on, indent);
                        Break(indent + 1);
                        Expr(on, body_end, indent + 1);
                    }
                    break;
                }
                default:
                    Expr(c.body, body_end, indent);
                    break;
            }
        }
    }

    bool HasTextBeyondIndent() const { return cur_has_; }

    /** @brief `WITH [RECURSIVE]` then each `name [(cols)] AS [NOT] [MATERIALIZED] (body)`; the main query follows. */
    void WithClause(int a, int end, int indent) {
        // CTEs are separated by top-level commas until the first top-level token after a `)` that is not a comma.
        int k = a;
        while (k < end) {
            // up to the body's `(`
            int lp = k;
            while (lp < end && Type(lp) != SqlTok::LP) lp++;
            // `name (cols) AS (`: skip a column list
            int as = lp;
            {
                int s = k;
                while (s < end && Type(s) != SqlTok::As) {
                    if (Type(s) == SqlTok::LP) s = Match(s, end);
                    s++;
                }
                as = s;
                lp = as;
                while (lp < end && Type(lp) != SqlTok::LP) lp++;
            }
            if (lp >= end) {
                Expr(k, end, indent);
                return;
            }
            const int rp = Match(lp, end);
            Through(lp);
            Break(indent + 1);
            Query(lp + 1, rp, indent + 1);
            Break(indent);
            Through(rp);
            const int n = Sig(rp + 1, end);
            if (n < end && Type(n) == SqlTok::Comma) {
                Through(n);
                int m = n + 1;
                while (m < end && Type(m) == SqlTok::Comment && LineOf(m) == EndLineOf(n)) {
                    Through(m);
                    m++;
                }
                Break(indent);
                k = m;
                continue;
            }
            return;  // the main statement's clause follows (its head is the next clause)
        }
    }

    void CreateTable(const SqlStmt &st, int indent, int end) {
        // header up to the column list's `(`
        int lp = st.name.tok + 1;
        while (lp < end && Type(lp) != SqlTok::LP && Type(lp) != SqlTok::As) lp++;
        if (lp >= end || Type(lp) == SqlTok::As) {
            // CREATE TABLE t AS select
            Through(lp < end ? lp : end - 1);
            if (lp < end) {
                Break(indent);
                Query(lp + 1, StmtEnd(st, end), indent);
            }
            return;
        }
        Through(lp);
        const int rp = Match(lp, end);
        // Items: column definitions and table constraints, in token order.
        struct Item {
            int first, last, name, type_last;
        };
        std::vector<Item> items;
        for (const SqlColumnDef &cd : st.coldefs) {
            items.push_back({cd.first >= 0 ? cd.first : cd.name.tok, cd.last >= 0 ? cd.last : cd.name.tok,
                             cd.name.tok, cd.type_last});
        }
        for (const SqlTableConstraint &tc : st.constraints) {
            items.push_back({tc.first, tc.last, -1, -1});
        }
        std::sort(items.begin(), items.end(), [](const Item &x, const Item &y) { return x.first < y.first; });
        bool ok = !items.empty();
        for (const Item &it : items) ok = ok && it.first > lp && it.last < rp && it.first >= 0;
        if (!ok) {
            List(lp + 1, rp, indent + 1, true);
            Break(indent);
            Through(rp);
            return;
        }
        size_t name_w = 0;
        for (const Item &it : items) {
            if (it.name >= 0 && it.type_last >= 0) name_w = std::max(name_w, TextOf(it.name).size());
        }
        for (const Item &it : items) {
            Break(indent + 1);
            Through(it.first - 1);  // comments before it (and the previous comma's trailing ones)
            if (pending_break_ || (last_ >= 0 && Type(last_) == SqlTok::Comment && cur_has_)) Break(indent + 1);
            if (it.name >= 0) {
                Through(it.name);
                if (it.type_last >= 0 && !Forced(it.name, it.name + 2)) {
                    const size_t w = TextOf(it.name).size();
                    cur_ += std::string(name_w - w, ' ');
                }
                Expr(it.name + 1, it.last + 1, indent + 1);
            } else {
                Expr(it.first, it.last + 1, indent + 1);
            }
            // the comma and any comment on its line
            int n = it.last + 1;
            if (n < rp && Type(Sig(n, rp)) == SqlTok::Comma) {
                Through(Sig(n, rp));
                n = Sig(n, rp) + 1;
                while (n < rp && Type(n) == SqlTok::Comment && LineOf(n) == EndLineOf(n - 1)) {
                    Through(n);
                    n++;
                }
            } else {
                // the last item has no comma, but a comment on its line still belongs to it
                while (n < rp && Type(n) == SqlTok::Comment && LineOf(n) == EndLineOf(n - 1)) {
                    Through(n);
                    n++;
                }
            }
        }
        Break(indent);
        Through(rp);
    }

    int StmtEnd(const SqlStmt &st, int end) const {
        int e = st.last >= 0 ? st.last : end - 1;
        if (e >= 0 && Type(e) == SqlTok::Semi) return e;
        return e + 1;
    }

    void Statement(const SqlStmt &st, int indent, int end) {
        const int stop = StmtEnd(st, end);
        int a = st.first;
        // EXPLAIN [QUERY PLAN] stays in front of its statement.
        if (st.explain) {
            int k = Sig(a, stop);
            Through(k);
            if (st.explain == 2) {
                k = Sig(k + 1, stop);
                Through(k);
                k = Sig(k + 1, stop);
                Through(k);
            }
            a = k + 1;
        }
        switch (st.kind) {
            case SqlStmt::Select:
            case SqlStmt::Insert:
            case SqlStmt::Update:
            case SqlStmt::Delete:
                Query(a, stop, indent);
                break;
            case SqlStmt::CreateTable:
                if (st.name.ok()) {
                    CreateTable(st, indent, stop);
                    // table options after `)`
                    Expr(next_, stop, indent);
                } else {
                    Expr(a, stop, indent);
                }
                break;
            case SqlStmt::CreateView: {
                int as = st.name.tok + 1;
                while (as < stop && !(Type(as) == SqlTok::As && !names_.count(as))) {
                    if (Type(as) == SqlTok::LP) as = Match(as, stop);
                    as++;
                }
                Expr(a, std::min(as + 1, stop), indent);
                if (as < stop) {
                    Break(indent);
                    Query(as + 1, stop, indent);
                }
                break;
            }
            case SqlStmt::CreateVirtualTable: {
                // `USING module(args)`: the arguments are the module's text, copied as written.
                int lp = st.module.ok() ? st.module.tok + 1 : stop;
                lp = Sig(lp, stop);
                if (lp < stop && Type(lp) == SqlTok::LP) {
                    const int rp = Match(lp, stop);
                    Through(lp - 1);
                    Verbatim(lp, std::min(rp, stop - 1));
                }
                Through(stop - 1);
                break;
            }
            case SqlStmt::CreateTrigger: {
                const int begin = st.begin_tok;
                if (begin < 0) {
                    Expr(a, stop, indent);
                    break;
                }
                // The header: WHEN on a line of its own when it does not fit.
                Expr(a, begin, indent);
                Through(begin);
                for (const SqlStmt &body : st.body) {
                    Break(indent + 1);
                    Through(body.first - 1);
                    if (last_ >= 0 && Type(last_) == SqlTok::Comment && cur_has_ && LineOf(body.first) > EndLineOf(last_)) {
                        Break(indent + 1);
                    }
                    Statement(body, indent + 1, stop);
                    Through(body.last);
                }
                // END
                int e = Sig(next_, stop);
                Break(indent);
                Through(std::min(e, stop - 1));
                break;
            }
            default:
                Expr(a, stop, indent);
                break;
        }
        Through(st.last >= 0 ? st.last : stop - 1);
    }
};

/** @brief Tokens compare equal apart from whitespace and keyword case. */
bool SameTokens(const SqlParse &a, const SqlParse &b, int *where) {
    *where = -1;
    size_t i = 0, j = 0;
    while (i < a.tokens.size() && j < b.tokens.size()) {
        const SqlToken &x = a.tokens[i];
        const SqlToken &y = b.tokens[j];
        if (x.type != y.type) {
            *where = static_cast<int>(i);
            return false;
        }
        std::string_view xs = a.Text(static_cast<int>(i));
        std::string_view ys = b.Text(static_cast<int>(j));
        if (x.type == SqlTok::Comment && xs.substr(0, 2) == "--") {
            // a line comment's trailing blanks are whitespace too
            while (!xs.empty() && (xs.back() == ' ' || xs.back() == '\t' || xs.back() == '\r')) xs.remove_suffix(1);
            while (!ys.empty() && (ys.back() == ' ' || ys.back() == '\t' || ys.back() == '\r')) ys.remove_suffix(1);
        }
        bool same = xs == ys;
        if (!same && SqlIsKeyword(x.type) && xs.size() == ys.size()) {
            same = true;
            for (size_t k = 0; k < xs.size(); k++) {
                char c1 = xs[k], c2 = ys[k];
                if (c1 >= 'a' && c1 <= 'z') c1 = static_cast<char>(c1 - 'a' + 'A');
                if (c2 >= 'a' && c2 <= 'z') c2 = static_cast<char>(c2 - 'a' + 'A');
                same = same && c1 == c2;
            }
        }
        if (!same) {
            *where = static_cast<int>(i);
            return false;
        }
        i++;
        j++;
    }
    if (i != a.tokens.size() || j != b.tokens.size()) {
        *where = static_cast<int>(std::min(i, a.tokens.size() - 1));
        return false;
    }
    return true;
}

std::vector<std::string> SplitLines(const std::string &t) {
    std::vector<std::string> out;
    size_t s = 0;
    while (true) {
        const size_t e = t.find('\n', s);
        if (e == std::string::npos) {
            out.push_back(t.substr(s));
            break;
        }
        out.push_back(t.substr(s, e - s));
        s = e + 1;
    }
    return out;
}

}  // namespace

Result Format(const std::string &text, const Options &opts_in) {
    Result r;
    Options opts = opts_in;
    if (opts.indent_width <= 0) opts.indent_width = 4;
    if (opts.width < 20) opts.width = 20;
    // Segments: runs of SQL lines and the dot-command lines between them (which the shell reads only where no
    // statement is pending).
    const std::vector<std::string> lines = SplitLines(text);
    std::vector<std::pair<bool, std::vector<std::string>>> segs;  // (is_sql, lines)
    std::string pending;
    std::string blanked;  // the whole script with dot lines blanked, for the syntax check
    for (const std::string &l : lines) {
        const bool dot = SqlIsDotCommandLine(l) && SqlSettled(pending);
        if (dot) {
            if (segs.empty() || segs.back().first) segs.emplace_back(false, std::vector<std::string>());
            segs.back().second.push_back(l);
            pending.clear();
            blanked += "\n";
            continue;
        }
        if (segs.empty() || !segs.back().first) segs.emplace_back(true, std::vector<std::string>());
        segs.back().second.push_back(l);
        pending += l + "\n";
        blanked += l + "\n";
    }
    {
        const SqlParse whole = SqlParseScript(blanked);
        if (!whole.errors.empty()) {
            const SqlSyntaxError &e = whole.errors.front();
            SqlLineIndex li(whole.text);
            int tok = e.tok;
            if (tok >= 0 && whole.tokens[static_cast<size_t>(tok)].type == SqlTok::Eof) {
                while (tok > 0 && (whole.tokens[static_cast<size_t>(tok)].type == SqlTok::Eof ||
                                   whole.tokens[static_cast<size_t>(tok)].type == SqlTok::Comment))
                    tok--;
            }
            r.error = e.message;
            r.error_line = tok >= 0 ? li.Line(whole.tokens[static_cast<size_t>(tok)].start) : -1;
            return r;
        }
    }
    std::vector<std::string> out;
    bool prev_blank_after = false;
    for (size_t si = 0; si < segs.size(); si++) {
        const auto &seg = segs[si];
        // Blank lines at the segment's edges become at most one separating blank line.
        size_t a = 0, b = seg.second.size();
        auto blank = [](const std::string &l) { return l.find_first_not_of(" \t\r") == std::string::npos; };
        while (a < b && blank(seg.second[a])) a++;
        while (b > a && blank(seg.second[b - 1])) b--;
        const bool lead_blank = a > 0;
        const bool trail_blank = b < seg.second.size();
        if (a == b) {
            prev_blank_after = prev_blank_after || lead_blank;
            continue;
        }
        if (!out.empty() && (lead_blank || prev_blank_after)) out.emplace_back();
        prev_blank_after = trail_blank;
        if (!seg.first) {
            for (size_t i = a; i < b; i++) out.push_back(seg.second[i]);
            continue;
        }
        std::string sql;
        for (size_t i = a; i < b; i++) sql += seg.second[i] + (i + 1 < b ? "\n" : "");
        const SqlParse p = SqlParseScript(sql);
        Writer w(p, opts);
        const std::string formatted = w.Run();
        const SqlParse q = SqlParseScript(formatted);
        int where = -1;
        if (!SameTokens(p, q, &where) || !q.errors.empty()) {
            SqlLineIndex li(p.text);
            r.error = "the formatter could not lay this out without changing it (a bug: please report it)";
            if (where >= 0) {
                const int base = static_cast<int>(lines.size()) - static_cast<int>(lines.size());
                (void)base;
            }
            int line = 0;
            for (size_t k = 0; k < si; k++) line += static_cast<int>(segs[k].second.size());
            r.error_line = line + static_cast<int>(a) + (where >= 0 ? li.Line(p.tokens[static_cast<size_t>(where)].start) : 0);
            return r;
        }
        for (const std::string &l : SplitLines(formatted)) out.push_back(l);
    }
    std::string res;
    for (size_t i = 0; i < out.size(); i++) {
        if (i) res += '\n';
        res += out[i];
    }
    if (!text.empty() && text.back() == '\n' && !res.empty()) res += '\n';
    r.ok = true;
    r.text = res;
    return r;
}

}  // namespace sqlfmt
