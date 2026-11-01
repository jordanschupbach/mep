// Name resolution for mep-sql-lsp: see sql_lsp_index.h.
//
// What a name in a statement can mean, in the order SQLite looks:
//   - in FROM / INSERT INTO / UPDATE / ... : a CTE in scope, else a schema
//     object (the document's own DDL first, then the `-- database:` file,
//     then the workspace's other .sql files);
//   - a column: the FROM sources of the innermost SELECT that has one with
//     that name (two sources with it is "ambiguous"), then the enclosing
//     SELECTs (a correlated subquery), then the result-column aliases --
//     SQLite accepts those in WHERE, GROUP BY, HAVING and ORDER BY;
//   - NEW / OLD in a trigger, `excluded` in an upsert.
//
// The checks that need the schema calibrate themselves to what is known
// (help/sql-lsp.org): an unknown table is only reported when the document
// declares tables itself or a database was read, and an unknown column only
// when every table in scope has known columns; a statement with a syntax
// error is not resolved at all.
#include <algorithm>
#include <set>

#include "sql_lsp_index.h"

std::string SqlLower(const std::string &s) {
    std::string out = s;
    for (char &c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::string SqlJoinLines(const std::vector<std::string> &lines) {
    std::string text;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) text += '\n';
        text += lines[i];
    }
    return text;
}

SqlLspRange SqlIndex::RangeOf(int tok) const {
    SqlLspRange r;
    if (tok < 0 || static_cast<size_t>(tok) >= parse.tokens.size()) return r;
    const SqlToken &t = parse.tokens[static_cast<size_t>(tok)];
    r.line = lines.Line(t.start);
    r.col = lines.Col(t.start);
    r.end_line = lines.Line(t.start + t.len);
    r.end_col = lines.Col(t.start + t.len);
    return r;
}

int SqlIndex::TokAt(int line, int col) const {
    const int off = lines.Offset(line, col);
    // The token containing off, or the one ending exactly at off (the cursor just after a name).
    int best = -1;
    for (size_t i = 0; i + 1 < parse.tokens.size(); i++) {
        const SqlToken &t = parse.tokens[i];
        if (t.start > off) break;
        if (off >= t.start && off < t.start + t.len) return static_cast<int>(i);
        if (off == t.start + t.len && t.type != SqlTok::Comment) best = static_cast<int>(i);
    }
    return best;
}

int SqlIndex::OccAt(int line, int col) const {
    const int off = lines.Offset(line, col);
    int best = -1;
    for (size_t i = 0; i < occs.size(); i++) {
        const SqlToken &t = parse.tokens[static_cast<size_t>(occs[i].tok)];
        if (off >= t.start && off < t.start + t.len) return static_cast<int>(i);
        if (off == t.start + t.len) best = static_cast<int>(i);
    }
    return best;
}

bool SqlIsSystemTable(const std::string &n) {
    return n == "sqlite_schema" || n == "sqlite_master" || n == "sqlite_temp_schema" || n == "sqlite_temp_master" ||
           n == "sqlite_sequence" || n == "sqlite_stat1" || n == "sqlite_stat2" || n == "sqlite_stat3" ||
           n == "sqlite_stat4" || n == "sqlite_dbpage" || n == "dbstat" || n == "sqlite_stmt" || n == "bytecode" ||
           n == "tables_used";
}

bool SqlTableFunctionColumns(const std::string &name, std::vector<std::string> *cols) {
    const std::string n = SqlLower(name);
    if (n == "json_each" || n == "json_tree" || n == "jsonb_each" || n == "jsonb_tree") {
        *cols = {"key", "value", "type", "atom", "id", "parent", "fullkey", "path", "json", "root"};
        return true;
    }
    if (n == "generate_series") {
        *cols = {"value", "start", "stop", "step"};
        return true;
    }
    if (n == "pragma_table_info") {
        *cols = {"cid", "name", "type", "notnull", "dflt_value", "pk", "arg", "schema"};
        return true;
    }
    if (n == "pragma_table_xinfo") {
        *cols = {"cid", "name", "type", "notnull", "dflt_value", "pk", "hidden", "arg", "schema"};
        return true;
    }
    if (n == "pragma_index_list") {
        *cols = {"seq", "name", "unique", "origin", "partial", "arg", "schema"};
        return true;
    }
    if (n == "pragma_index_info" || n == "pragma_index_xinfo") {
        *cols = {"seqno", "cid", "name", "desc", "coll", "key", "arg", "schema"};
        return true;
    }
    if (n == "pragma_foreign_key_list") {
        *cols = {"id", "seq", "table", "from", "to", "on_update", "on_delete", "match", "arg", "schema"};
        return true;
    }
    if (n == "pragma_function_list") {
        *cols = {"name", "builtin", "type", "enc", "narg", "flags"};
        return true;
    }
    return false;
}

namespace {

/** @brief A virtual table's columns, from its module arguments, when the module is one whose arguments are columns. */
bool VtabColumns(const SqlParse &p, const SqlStmt &st, std::vector<SqlLspColumn> *cols) {
    const std::string m = SqlLower(st.module.text);
    if (m != "fts3" && m != "fts4" && m != "fts5" && m != "rtree" && m != "rtree_i32" && m != "geopoly") return false;
    std::vector<std::string> option_hidden;  // fts4's `languageid=NAME`
    bool external_content = false;           // fts4's `content=TABLE`: the columns may come from TABLE
    for (const auto &arg : st.module_args) {
        if (arg.first < 0) continue;
        int eq = -1;
        for (int t = arg.first; t <= arg.second; t++) {
            if (eq < 0 && p.tokens[static_cast<size_t>(t)].type == SqlTok::Eq) eq = t;
        }
        if (eq >= 0) {
            const std::string key = SqlLower(SqlDequote(p.Text(arg.first)));
            const std::string value = eq < arg.second ? SqlDequote(p.Text(eq + 1)) : std::string();
            // fts3 knows only `tokenize=`: any other `name=value` is a column called name
            if (m != "fts3" || key == "tokenize") {
                if (m == "fts4" && key == "languageid" && !value.empty()) option_hidden.push_back(value);
                if (m == "fts4" && key == "content" && !value.empty()) external_content = true;
                continue;  // fts option (`tokenize=porter`, `content=''`)
            }
        }
        int name_tok = arg.first;
        // rtree's auxiliary columns are written `+name`
        if (p.tokens[static_cast<size_t>(name_tok)].type == SqlTok::Plus && name_tok < arg.second) name_tok++;
        const SqlToken &first = p.tokens[static_cast<size_t>(name_tok)];
        if (first.type != SqlTok::Id && !SqlIsKeyword(first.type) && first.type != SqlTok::String) continue;
        // fts3/fts4's old option form: `tokenize porter`
        if ((m == "fts3" || m == "fts4") && SqlLower(std::string(p.Text(name_tok))) == "tokenize") continue;
        SqlLspColumn c;
        c.name = SqlDequote(p.Text(name_tok));
        c.decl_tok = name_tok;
        cols->push_back(c);
    }
    auto hidden = [cols](const std::string &n) {
        SqlLspColumn c;
        c.name = n;
        c.hidden = true;
        cols->push_back(c);
    };
    if (external_content && cols->empty()) return false;  // columns of the content table
    if (m == "fts3" || m == "fts4") {
        if (cols->empty()) {
            SqlLspColumn c;
            c.name = "content";  // an fts table declared without columns has this one
            cols->push_back(c);
        }
        for (const std::string &h : option_hidden) hidden(h);
        hidden(st.name.text);
        hidden("docid");
        hidden("langid");
    } else if (m == "fts5") {
        hidden(st.name.text);
        hidden("rank");
    } else if (m == "geopoly") {
        SqlLspColumn c;
        c.name = "_shape";
        cols->insert(cols->begin(), c);
    }
    return true;
}

std::string ResultName(const SqlParse &p, const SqlResultCol &c) {
    if (c.alias.ok()) return c.alias.text;
    if (c.expr && c.expr->kind == SqlExpr::Column) return c.expr->column.text;
    if (c.first < 0) return std::string();
    const SqlToken &a = p.tokens[static_cast<size_t>(c.first)];
    const SqlToken &b = p.tokens[static_cast<size_t>(c.last)];
    return p.text.substr(static_cast<size_t>(a.start), static_cast<size_t>(b.start + b.len - a.start));
}

}  // namespace

std::vector<SqlLspObject> SqlObjectsOf(const SqlParse &p, const SqlLineIndex &li, const std::string &origin,
                                       const std::string &uri) {
    std::vector<SqlLspObject> out;
    auto find = [&out](const std::string &name) -> SqlLspObject * {
        const std::string l = SqlLower(name);
        for (SqlLspObject &o : out) {
            if (SqlLower(o.name) == l) return &o;
        }
        return nullptr;
    };
    auto range_of = [&](int tok, SqlLspRange *r) {
        const SqlToken &t = p.tokens[static_cast<size_t>(tok)];
        r->line = li.Line(t.start);
        r->col = li.Col(t.start);
        r->end_line = li.Line(t.start + t.len);
        r->end_col = li.Col(t.start + t.len);
    };
    auto stmt_sql = [&p](const SqlStmt &st) {
        const SqlToken &a = p.tokens[static_cast<size_t>(st.first)];
        int last = st.last;
        if (p.tokens[static_cast<size_t>(last)].type == SqlTok::Semi && last > st.first) last--;
        const SqlToken &b = p.tokens[static_cast<size_t>(last)];
        return p.text.substr(static_cast<size_t>(a.start), static_cast<size_t>(b.start + b.len - a.start));
    };
    auto add = [&](SqlLspObject obj) {
        if (SqlLspObject *prev = find(obj.name)) {
            // Re-created (DROP + CREATE in one script): keep the first declaration, union the columns, so that
            // statements written against either version resolve.
            for (SqlLspColumn &c : obj.columns) {
                bool have = false;
                for (const SqlLspColumn &d : prev->columns) {
                    if (SqlLower(d.name) == SqlLower(c.name)) have = true;
                }
                if (!have) prev->columns.push_back(c);
            }
            prev->columns_known = prev->columns_known && obj.columns_known;
            prev->redeclared = true;
            return;
        }
        out.push_back(std::move(obj));
    };
    auto column = [&](const SqlColumnDef &cd) {
        SqlLspColumn c;
        c.name = cd.name.text;
        c.type = cd.type;
        c.primary_key = cd.primary_key;
        c.not_null = cd.not_null;
        c.generated = cd.generated;  // not insertable, so INSERT's value count leaves it out
        c.uri = uri;
        c.decl_tok = origin.empty() ? cd.name.tok : -1;
        range_of(cd.name.tok, &c.range);
        c.has_range = true;
        return c;
    };
    for (const SqlStmt &st : p.stmts) {
        if (st.broken) {
            // A CREATE cut short by a syntax error still declares its name (with the columns read so far, not a
            // complete list): later statements using it should not also be told it does not exist.
            if ((st.kind == SqlStmt::CreateTable || st.kind == SqlStmt::CreateView ||
                 st.kind == SqlStmt::CreateVirtualTable) &&
                st.name.ok() && !find(st.name.text)) {
                SqlLspObject obj;
                obj.kind = st.kind == SqlStmt::CreateView ? SqlLspObject::View
                           : st.kind == SqlStmt::CreateVirtualTable ? SqlLspObject::VirtualTable
                                                                    : SqlLspObject::Table;
                obj.name = st.name.text;
                obj.origin = origin;
                obj.uri = uri;
                obj.decl_tok = origin.empty() ? st.name.tok : -1;
                range_of(st.name.tok, &obj.range);
                obj.has_range = true;
                obj.columns_known = false;
                for (const SqlColumnDef &cd : st.coldefs) {
                    if (cd.name.ok()) obj.columns.push_back(column(cd));
                }
                out.push_back(std::move(obj));
            }
            continue;
        }
        SqlLspObject obj;
        obj.origin = origin;
        obj.uri = uri;
        if (st.name.ok()) {
            obj.name = st.name.text;
            obj.decl_tok = origin.empty() ? st.name.tok : -1;
            range_of(st.name.tok, &obj.range);
            obj.has_range = true;
        }
        switch (st.kind) {
            case SqlStmt::CreateTable:
                obj.kind = SqlLspObject::Table;
                obj.sql = stmt_sql(st);
                obj.without_rowid = st.without_rowid;
                if (st.select) {
                    // CREATE TABLE ... AS SELECT: the result columns' names.
                    const SqlCore &core = st.select->cores.front();
                    for (const SqlResultCol &rc : core.cols) {
                        if (rc.star) {
                            obj.columns_known = false;
                            continue;
                        }
                        SqlLspColumn c;
                        c.name = ResultName(p, rc);
                        obj.columns.push_back(c);
                    }
                    if (core.is_values) obj.columns_known = false;
                } else {
                    for (const SqlColumnDef &cd : st.coldefs) obj.columns.push_back(column(cd));
                }
                add(std::move(obj));
                break;
            case SqlStmt::CreateVirtualTable:
                obj.kind = SqlLspObject::VirtualTable;
                obj.module = st.module.text;
                obj.sql = stmt_sql(st);
                obj.columns_known = VtabColumns(p, st, &obj.columns);
                for (SqlLspColumn &c : obj.columns) {
                    c.uri = uri;
                    if (c.decl_tok >= 0) {
                        range_of(c.decl_tok, &c.range);
                        c.has_range = true;
                        if (!origin.empty()) c.decl_tok = -1;
                    }
                }
                add(std::move(obj));
                break;
            case SqlStmt::CreateView: {
                obj.kind = SqlLspObject::View;
                obj.sql = stmt_sql(st);
                if (!st.columns.empty()) {
                    for (const SqlName &n : st.columns) {
                        SqlLspColumn c;
                        c.name = n.text;
                        c.uri = uri;
                        c.decl_tok = origin.empty() ? n.tok : -1;
                        range_of(n.tok, &c.range);
                        c.has_range = true;
                        obj.columns.push_back(c);
                    }
                } else if (st.select) {
                    const SqlCore &core = st.select->cores.front();
                    if (core.is_values) obj.columns_known = false;
                    for (const SqlResultCol &rc : core.cols) {
                        if (rc.star) {
                            // Expand * over the sources' known columns (tables declared above).
                            bool ok = true;
                            for (const SqlFromItem &fi : core.from) {
                                if (rc.star_table.ok()) {
                                    const std::string q = SqlLower(rc.star_table.text);
                                    if (SqlLower(fi.alias.ok() ? fi.alias.text : fi.name.text) != q) continue;
                                }
                                const SqlLspObject *src = fi.kind == SqlFromItem::Table ? find(fi.name.text) : nullptr;
                                if (!src || !src->columns_known) {
                                    ok = false;
                                    continue;
                                }
                                for (const SqlLspColumn &c : src->columns) {
                                    SqlLspColumn copy = c;
                                    copy.decl_tok = -1;
                                    obj.columns.push_back(copy);
                                }
                            }
                            if (!ok || core.from.empty()) obj.columns_known = false;
                            continue;
                        }
                        SqlLspColumn c;
                        c.name = ResultName(p, rc);
                        c.uri = uri;
                        const int tok = rc.alias.ok() ? rc.alias.tok
                                        : (rc.expr && rc.expr->kind == SqlExpr::Column) ? rc.expr->column.tok
                                                                                         : -1;
                        if (tok >= 0) {
                            c.decl_tok = origin.empty() ? tok : -1;
                            range_of(tok, &c.range);
                            c.has_range = true;
                        }
                        obj.columns.push_back(c);
                    }
                }
                add(std::move(obj));
                break;
            }
            case SqlStmt::CreateIndex:
                obj.kind = SqlLspObject::Index;
                obj.table = st.on_table.text;
                obj.sql = stmt_sql(st);
                add(std::move(obj));
                break;
            case SqlStmt::CreateTrigger:
                obj.kind = SqlLspObject::Trigger;
                obj.table = st.on_table.text;
                obj.sql = stmt_sql(st);
                add(std::move(obj));
                break;
            case SqlStmt::AlterTable: {
                SqlLspObject *t = find(st.name.text);
                if (!t) break;
                if (st.alter == SqlStmt::RenameTable && st.new_name.ok()) {
                    // Keep the old name findable too: statements above the rename use it.
                    SqlLspObject renamed = *t;
                    renamed.name = st.new_name.text;
                    renamed.decl_tok = origin.empty() ? st.new_name.tok : -1;
                    range_of(st.new_name.tok, &renamed.range);
                    if (!find(renamed.name)) out.push_back(std::move(renamed));
                } else if (st.alter == SqlStmt::AddColumn && st.add_column) {
                    bool have = false;
                    for (const SqlLspColumn &c : t->columns) {
                        if (SqlLower(c.name) == SqlLower(st.add_column->name.text)) have = true;
                    }
                    if (!have) {
                        t->columns.push_back(column(*st.add_column));
                        t->columns.back().added_at = st.first;
                    }
                } else if (st.alter == SqlStmt::DropColumn && st.column.ok()) {
                    for (SqlLspColumn &c : t->columns) {
                        if (SqlLower(c.name) == SqlLower(st.column.text) && c.dropped_at < 0) c.dropped_at = st.first;
                    }
                } else if (st.alter == SqlStmt::RenameColumn && st.new_name.ok()) {
                    bool have = false;
                    for (const SqlLspColumn &c : t->columns) {
                        if (SqlLower(c.name) == SqlLower(st.new_name.text)) have = true;
                    }
                    if (!have) {
                        SqlLspColumn c;
                        c.name = st.new_name.text;
                        c.uri = uri;
                        c.decl_tok = origin.empty() ? st.new_name.tok : -1;
                        range_of(st.new_name.tok, &c.range);
                        c.has_range = true;
                        c.added_at = st.first;
                        for (SqlLspColumn &old : t->columns) {
                            if (SqlLower(old.name) != SqlLower(st.column.text)) continue;
                            c.type = old.type;
                            c.primary_key = old.primary_key;
                            c.not_null = old.not_null;
                            if (old.dropped_at < 0) old.dropped_at = st.first;
                        }
                        t->columns.push_back(c);
                    }
                }
                break;
            }
            default:
                break;
        }
    }
    return out;
}

namespace {

struct Col {
    std::string name;
    std::string lname;
    int target = -1;
    std::string type;
    bool star = true;  // part of `*` (not a virtual table's hidden column)
};

struct Source {
    std::string name;  // what qualifies its columns: the alias, else the table's name (lower case)
    std::string display;
    std::string table;  // the table / CTE it reads
    int qual_target = -1;  // what a qualifier `x.` names: the alias, else the object / CTE
    std::vector<Col> cols;
    bool known = true;
    bool rowid = true;
    std::set<std::string> merged;  // columns a USING / NATURAL join folded into the left side
};

struct Scope {
    Scope *parent = nullptr;
    std::vector<Source> sources;
    std::vector<std::pair<std::string, int>> ctes;  // name (lower) -> index into ctes_
    std::vector<std::pair<std::string, int>> aliases;  // result-column aliases -> target
    std::vector<std::pair<std::string, int>> windows;  // named windows -> target
    bool ddl = false;  // CHECK/DEFAULT/index contexts: unknown qualifiers are not reported
};

struct CteInfo {
    std::string name;
    int target = -1;
    int tok = -1;
    std::vector<Col> cols;
    bool known = true;
    int refs = 0;
    bool recursive_self = false;
    std::string count_problem;  // reported only if the CTE is used: SQLite ignores an unused one
};

enum class Ctx { Normal, Where, OrderBy };

class Resolver {
public:
    Resolver(SqlIndex *idx, const SqlLspContext &ctx) : x_(idx), ctx_(ctx) {}

    void Run() {
        schema_known_ = ctx_.database_loaded;
        for (const SqlLspObject &o : x_->local) {
            if (o.kind == SqlLspObject::Table || o.kind == SqlLspObject::View || o.kind == SqlLspObject::VirtualTable) {
                schema_known_ = true;
            }
        }
        for (size_t i = 0; i < x_->parse.stmts.size(); i++) {
            const SqlStmt &st = x_->parse.stmts[i];
            if (st.broken) continue;
            stmt_first_ = st.first;
            Statement(st, nullptr);
        }
    }

private:
    SqlIndex *x_;
    const SqlLspContext &ctx_;
    bool schema_known_ = false;
    std::vector<CteInfo> ctes_;
    std::map<std::string, int> keyed_;  // target key -> index
    int scope_counter_ = 0;
    int stmt_first_ = -1;  // the top-level statement being resolved

    const SqlParse &P() const { return x_->parse; }
    std::string Text(int tok) const { return std::string(P().Text(tok)); }

    SqlLspSeverity SchemaSeverity() const {
        return ctx_.database_loaded ? SqlLspSeverity::Error : SqlLspSeverity::Warning;
    }

    void Diag(int first, int last, SqlLspSeverity sev, const std::string &code, const std::string &msg) {
        if (first < 0) return;
        if (last < first) last = first;
        SqlLspDiagnostic d;
        const SqlLspRange a = x_->RangeOf(first);
        const SqlLspRange b = x_->RangeOf(last);
        d.range.line = a.line;
        d.range.col = a.col;
        d.range.end_line = b.end_line;
        d.range.end_col = b.end_col;
        d.severity = sev;
        d.code = code;
        d.message = msg;
        x_->diags.push_back(std::move(d));
    }

    int Target(const std::string &key, SqlIdxTarget proto) {
        const auto it = keyed_.find(key);
        if (it != keyed_.end()) return it->second;
        proto.key = key;
        x_->targets.push_back(std::move(proto));
        const int id = static_cast<int>(x_->targets.size()) - 1;
        keyed_[key] = id;
        return id;
    }

    void Occ(int tok, int target, bool def = false, bool write = false) {
        if (tok < 0 || target < 0) return;
        for (const SqlIdxOcc &o : x_->occs) {
            if (o.tok == tok) return;  // resolved twice (a view's column list, ...): keep the first
        }
        SqlIdxOcc o;
        o.tok = tok;
        o.target = target;
        o.is_def = def;
        o.is_write = write;
        x_->occs.push_back(o);
    }

    // --- schema lookup ----------------------------------------------------

    const SqlLspObject *FindObject(const std::string &name) const {
        const std::string l = SqlLower(name);
        for (const SqlLspObject &o : x_->local) {
            if (SqlLower(o.name) == l) return &o;
        }
        for (const SqlLspObject &o : ctx_.database) {
            if (SqlLower(o.name) == l) return &o;
        }
        for (const SqlLspObject &o : ctx_.workspace) {
            if (SqlLower(o.name) == l) return &o;
        }
        return nullptr;
    }

    /** @brief Whether a name is one of the shadow tables a virtual table keeps its data in (`docs_content`). */
    bool IsShadowTable(const std::string &l) const {
        static const char *const kSuffixes[] = {"content", "segments", "segdir", "docsize", "stat", "data",
                                                "idx",     "config",   "node",   "parent",  "rowid"};
        const std::vector<SqlLspObject> *lists[] = {&x_->local, &ctx_.database, &ctx_.workspace};
        for (const auto *list : lists) {
            for (const SqlLspObject &o : *list) {
                if (o.kind != SqlLspObject::VirtualTable) continue;
                const std::string base = SqlLower(o.name) + "_";
                if (l.size() <= base.size() || l.compare(0, base.size(), base) != 0) continue;
                const std::string suffix = l.substr(base.size());
                for (const char *s : kSuffixes) {
                    if (suffix == s) return true;
                }
            }
        }
        return false;
    }

    int ObjectTarget(const SqlLspObject *o) {
        SqlIdxTarget t;
        t.kind = SqlIdxTarget::Object;
        t.name = o->name;
        t.object = o;
        t.local = o->origin.empty() && o->decl_tok >= 0;
        t.decl_tok = o->origin.empty() ? o->decl_tok : -1;
        for (const SqlLspColumn &c : o->columns) t.columns.push_back(c.name);
        return Target("obj:" + SqlLower(o->name), t);
    }

    int ColumnTarget(const SqlLspObject *o, int ci) {
        const SqlLspColumn &c = o->columns[static_cast<size_t>(ci)];
        SqlIdxTarget t;
        t.kind = SqlIdxTarget::Column;
        t.name = c.name;
        t.type = c.type;
        t.table = o->name;
        t.object = o;
        t.column = ci;
        t.local = o->origin.empty() && c.decl_tok >= 0;
        t.decl_tok = o->origin.empty() ? c.decl_tok : -1;
        return Target("col:" + SqlLower(o->name) + "." + SqlLower(c.name), t);
    }

    /** @brief Whether a column of the document's own table exists at the statement being resolved. */
    bool ColumnVisible(const SqlLspObject *o, const SqlLspColumn &c) const {
        if (!o->origin.empty() || stmt_first_ < 0) return true;
        if (c.added_at >= 0 && stmt_first_ <= c.added_at) return false;
        if (c.dropped_at >= 0 && stmt_first_ > c.dropped_at) return false;
        return true;
    }

    Source ObjectSource(const SqlLspObject *o, const SqlName &alias, int name_tok) {
        Source s;
        s.table = o->name;
        s.display = alias.ok() ? alias.text : o->name;
        s.name = SqlLower(s.display);
        s.known = o->columns_known && !o->redeclared;
        s.rowid = !o->without_rowid && o->kind != SqlLspObject::View;
        for (size_t i = 0; i < o->columns.size(); i++) {
            const SqlLspColumn &oc = o->columns[i];
            Col c;
            c.name = oc.name;
            c.lname = SqlLower(c.name);
            c.type = oc.type;
            c.star = !oc.hidden;
            c.target = ColumnTarget(o, static_cast<int>(i));
            if (!ColumnVisible(o, oc)) {
                // Renamed away or not added yet: still resolvable for navigation, but not a column `*` gives.
                c.star = false;
            }
            s.cols.push_back(c);
        }
        const int ot = ObjectTarget(o);
        Occ(name_tok, ot);
        s.qual_target = ot;
        if (alias.ok()) s.qual_target = AliasTarget(alias, o->name, s);
        return s;
    }

    int AliasTarget(const SqlName &alias, const std::string &table, const Source &s) {
        SqlIdxTarget t;
        t.kind = SqlIdxTarget::Alias;
        t.name = alias.text;
        t.table = table;
        t.local = true;
        t.decl_tok = alias.tok;
        for (const Col &c : s.cols) t.columns.push_back(c.name);
        const int id = Target("alias:" + std::to_string(alias.tok), t);
        Occ(alias.tok, id, true);
        return id;
    }

    // --- scopes ------------------------------------------------------------

    int FindCte(Scope *sc, const std::string &lname) {
        for (Scope *s = sc; s; s = s->parent) {
            for (const auto &c : s->ctes) {
                if (c.first == lname) return c.second;
            }
        }
        return -1;
    }

    /** @brief Resolves a table name used as a source or a target (FROM, INTO, UPDATE, ON ...). */
    bool TableSource(Scope *sc, const SqlName &db, const SqlName &name, const SqlName &alias, Source *out,
                     bool report = true) {
        const std::string l = SqlLower(name.text);
        if (!db.ok()) {
            const int ci = FindCte(sc, l);
            if (ci >= 0) {
                CteInfo &cte = ctes_[static_cast<size_t>(ci)];
                cte.refs++;
                Occ(name.tok, cte.target);
                out->table = cte.name;
                out->display = alias.ok() ? alias.text : cte.name;
                out->name = SqlLower(out->display);
                out->cols = cte.cols;
                out->known = cte.known;
                out->rowid = false;
                out->qual_target = cte.target;
                if (alias.ok()) out->qual_target = AliasTarget(alias, cte.name, *out);
                return true;
            }
        }
        const std::string dbl = SqlLower(db.text);
        const bool other_db = db.ok() && dbl != "main" && dbl != "temp";
        const SqlLspObject *o = other_db ? nullptr : FindObject(name.text);
        if (o) {
            *out = ObjectSource(o, alias, name.tok);
            return true;
        }
        out->table = name.text;
        out->display = alias.ok() ? alias.text : name.text;
        out->name = SqlLower(out->display);
        out->known = false;
        if (alias.ok()) {
            out->qual_target = AliasTarget(alias, name.text, *out);
        }
        if (report && !other_db && schema_known_ && !SqlIsSystemTable(l) && l.rfind("pragma_", 0) != 0 &&
            !IsShadowTable(l)) {
            Diag(name.tok, name.tok, SchemaSeverity(), "unknown-table", "no such table: " + name.text);
        }
        if (SqlIsSystemTable(l) && (l == "sqlite_schema" || l == "sqlite_master" || l == "sqlite_temp_schema" ||
                                    l == "sqlite_temp_master")) {
            out->known = true;
            for (const char *c : {"type", "name", "tbl_name", "rootpage", "sql"}) {
                Col col;
                col.name = c;
                col.lname = c;
                out->cols.push_back(col);
            }
        }
        return false;
    }

    static bool AllKnown(const Scope *sc) {
        for (const Scope *s = sc; s; s = s->parent) {
            for (const Source &src : s->sources) {
                if (!src.known) return false;
            }
        }
        return true;
    }

    static bool IsRowid(const std::string &l) { return l == "rowid" || l == "oid" || l == "_rowid_"; }

    void ColumnRef(const SqlExpr &e, Scope *sc, Ctx ctx) {
        const std::string cname = SqlLower(e.column.text);
        if (e.table.ok()) {
            const std::string q = SqlLower(e.table.text);
            for (Scope *s = sc; s; s = s->parent) {
                for (Source &src : s->sources) {
                    if (src.name != q) continue;
                    Occ(e.table.tok, src.qual_target);
                    for (const Col &c : src.cols) {
                        if (c.lname == cname) {
                            Occ(e.column.tok, c.target);
                            return;
                        }
                    }
                    if (IsRowid(cname) && src.rowid) return;
                    if (src.known) {
                        Diag(e.table.tok, e.column.tok, SchemaSeverity(), "unknown-column",
                             "no such column: " + e.table.text + "." + e.column.text);
                    }
                    return;
                }
            }
            // A qualifier no FROM source has. (Inside CREATE TABLE/INDEX checks the table's own name is fine.)
            bool any_source = false;
            for (Scope *s = sc; s; s = s->parent) any_source = any_source || !s->sources.empty();
            if (!sc->ddl && any_source && AllKnown(sc) && !e.db.ok()) {
                Diag(e.table.tok, e.column.tok, SchemaSeverity(), "unknown-column",
                     "no such column: " + e.table.text + "." + e.column.text);
            }
            return;
        }
        if (ctx == Ctx::OrderBy) {
            for (const auto &a : sc->aliases) {
                if (a.first == cname) {
                    Occ(e.column.tok, a.second);
                    return;
                }
            }
        }
        for (Scope *s = sc; s; s = s->parent) {
            const Col *hit = nullptr;
            const Source *hit_src = nullptr;
            int hits = 0;
            bool rowid_hit = false;
            for (const Source &src : s->sources) {
                for (const Col &c : src.cols) {
                    if (c.lname != cname) continue;
                    if (src.merged.count(cname)) continue;
                    if (hit && hit->target == c.target && c.target >= 0) continue;
                    hit = &c;
                    hit_src = &src;
                    hits++;
                    break;
                }
                if (IsRowid(cname) && src.rowid) rowid_hit = true;
            }
            if (hits > 1) {
                Diag(e.column.tok, e.column.tok, SqlLspSeverity::Error, "ambiguous-column",
                     "ambiguous column name: " + e.column.text);
                return;
            }
            if (hit) {
                (void)hit_src;
                Occ(e.column.tok, hit->target);
                return;
            }
            if (rowid_hit) return;
        }
        for (const auto &a : sc->aliases) {
            if (a.first == cname) {
                Occ(e.column.tok, a.second);
                return;
            }
        }
        if (cname == "true" || cname == "false") return;  // SQLite's TRUE/FALSE are identifiers that fall back
        bool any_source = false;
        for (Scope *s = sc; s; s = s->parent) any_source = any_source || !s->sources.empty();
        if (!any_source || !AllKnown(sc)) return;
        const std::string_view raw = P().Text(e.column.tok);
        if (!raw.empty() && raw[0] == '"') {
            Diag(e.column.tok, e.column.tok, SqlLspSeverity::Warning, "double-quoted-string",
                 "no such column: " + e.column.text +
                     " (double quotes name an identifier; a string literal takes single quotes)");
            return;
        }
        Diag(e.column.tok, e.column.tok, SchemaSeverity(), "unknown-column", "no such column: " + e.column.text);
    }

    // --- expressions -------------------------------------------------------

    static bool IsNullLiteral(const SqlParse &p, const SqlExpr &e) {
        return e.kind == SqlExpr::Literal && p.tokens[static_cast<size_t>(e.op)].type == SqlTok::Null;
    }

    bool IsAggregateCall(const SqlExpr &e) const {
        if (e.kind != SqlExpr::Func || e.over || e.over_name.ok()) return false;
        const SqlLspFunction *f = SqlLspFindFunction(e.func.text);
        if (!f) return false;
        const std::string n = SqlLower(e.func.text);
        if (n == "min" || n == "max") return e.args.size() == 1;
        const std::string kind = f->kind;
        if (kind == "aggregate" || kind == "window") return true;
        return n == "json_group_array" || n == "json_group_object" || n == "jsonb_group_array" ||
               n == "jsonb_group_object";
    }

    /** @brief Reports an aggregate in a WHERE clause (not looking inside subqueries, which have their own). */
    void NoAggregates(const SqlExpr *e, const char *clause, const Scope *sc = nullptr) {
        if (!e) return;
        // In a subquery an aggregate may belong to the enclosing query (`WHERE x = max(outer.y)`).
        for (const Scope *s = sc ? sc->parent : nullptr; s; s = s->parent) {
            if (!s->sources.empty()) return;
        }
        if (e->kind == SqlExpr::Func && IsAggregateCall(*e)) {
            const SqlLspFunction *f = SqlLspFindFunction(e->func.text);
            const bool window_only = f && std::string(f->kind) == "window";
            Diag(e->func.tok, e->func.tok, SqlLspSeverity::Error, "misused-aggregate",
                 std::string(window_only ? "misuse of window function " : "misuse of aggregate: ") + e->func.text +
                     "()" + (window_only ? "" : std::string(" (aggregates are not allowed in ") + clause + ")"));
            return;
        }
        for (const SqlExprP &a : e->args) NoAggregates(a.get(), clause, sc);
    }

    void Expr(const SqlExpr *e, Scope *sc, Ctx ctx = Ctx::Normal) {
        if (!e) return;
        switch (e->kind) {
            case SqlExpr::Column:
                ColumnRef(*e, sc, ctx);
                return;
            case SqlExpr::Func: {
                SqlIdxCall call;
                call.tok = e->func.tok;
                call.name = e->func.text;
                call.argc = e->star ? 0 : static_cast<int>(e->args.size());
                x_->calls.push_back(call);
                const SqlLspFunction *f = SqlLspFindFunction(e->func.text);
                if (!f) {
                    Diag(e->func.tok, e->func.tok, SqlLspSeverity::Warning, "unknown-function",
                         "no such function: " + e->func.text);
                } else if (!SqlLspArityOk(*f, call.argc)) {  // f(*) is f() to SQLite
                    Diag(e->func.tok, e->last, SqlLspSeverity::Error, "wrong-arity",
                         "wrong number of arguments to function " + e->func.text + "()");
                }
                for (const SqlExprP &a : e->args) Expr(a.get(), sc);
                for (const SqlOrderTerm &o : e->order) Expr(o.expr.get(), sc);
                if (e->filter) Expr(e->filter.get(), sc);
                if (e->over) Window(*e->over, sc);
                if (e->over_name.ok()) WindowRef(e->over_name, sc);
                return;
            }
            case SqlExpr::Binary: {
                const SqlTok op = P().tokens[static_cast<size_t>(e->op)].type;
                if ((op == SqlTok::Eq || op == SqlTok::Ne) &&
                    (IsNullLiteral(P(), *e->args[0]) || IsNullLiteral(P(), *e->args[1]))) {
                    Diag(e->first, e->last, SqlLspSeverity::Warning, "compare-null",
                         std::string("comparison with NULL is always NULL: use ") +
                             (op == SqlTok::Eq ? "IS NULL" : "IS NOT NULL"));
                }
                break;
            }
            case SqlExpr::In:
                if (e->in_table.ok()) {
                    Source dummy;
                    const std::string l = SqlLower(e->in_table.text);
                    std::vector<std::string> cols;
                    if (!SqlTableFunctionColumns(l, &cols)) TableSource(sc, e->in_db, e->in_table, SqlName(), &dummy);
                }
                break;
            default:
                break;
        }
        for (const SqlExprP &a : e->args) Expr(a.get(), sc, ctx == Ctx::OrderBy ? Ctx::Normal : ctx);
        if (e->select) Select(*e->select, sc, nullptr, nullptr);
    }

    void WindowRef(const SqlName &n, Scope *sc) {
        const std::string l = SqlLower(n.text);
        for (Scope *s = sc; s; s = s->parent) {
            for (const auto &w : s->windows) {
                if (w.first == l) {
                    Occ(n.tok, w.second);
                    return;
                }
            }
        }
        // SQLite: "no such window: w"
        Diag(n.tok, n.tok, SqlLspSeverity::Error, "unknown-window", "no such window: " + n.text);
    }

    void Window(const SqlWindowSpec &w, Scope *sc) {
        if (w.base.ok()) WindowRef(w.base, sc);
        for (const SqlExprP &e : w.partition) Expr(e.get(), sc);
        for (const SqlOrderTerm &o : w.order) Expr(o.expr.get(), sc);
        for (const SqlExprP &e : w.frame) Expr(e.get(), sc);
    }

    // --- SELECT ------------------------------------------------------------

    void With(const SqlWith &with, Scope *sc) {
        std::set<std::string> seen;
        for (const SqlCte &cte : with.ctes) {
            const std::string l = SqlLower(cte.name.text);
            if (!seen.insert(l).second) {
                Diag(cte.name.tok, cte.name.tok, SqlLspSeverity::Error, "duplicate-cte",
                     "duplicate WITH table name: " + cte.name.text);
            }
            CteInfo info;
            info.name = cte.name.text;
            info.tok = cte.name.tok;
            SqlIdxTarget t;
            t.kind = SqlIdxTarget::Cte;
            t.name = cte.name.text;
            t.local = true;
            t.decl_tok = cte.name.tok;
            info.target = Target("cte:" + std::to_string(cte.name.tok), t);
            Occ(cte.name.tok, info.target, true);
            for (const SqlName &n : cte.columns) {
                Col c;
                c.name = n.text;
                c.lname = SqlLower(n.text);
                SqlIdxTarget ct;
                ct.kind = SqlIdxTarget::CteColumn;
                ct.name = n.text;
                ct.table = cte.name.text;
                ct.local = true;
                ct.decl_tok = n.tok;
                c.target = Target("ctecol:" + std::to_string(n.tok), ct);
                Occ(n.tok, c.target, true);
                info.cols.push_back(c);
            }
            // A recursive CTE sees itself: with its declared columns, else (until resolved) unknown ones.
            info.known = !cte.columns.empty();
            ctes_.push_back(info);
            const int ci = static_cast<int>(ctes_.size()) - 1;
            sc->ctes.emplace_back(l, ci);
            const int refs_before = ctes_[static_cast<size_t>(ci)].refs;
            std::vector<Col> result;
            bool known = true;
            std::vector<int> result_toks;
            Select(*cte.select, sc, &result, &known, &result_toks);
            CteInfo &now = ctes_[static_cast<size_t>(ci)];
            now.recursive_self = now.refs > refs_before;
            now.refs = refs_before;  // a recursive CTE's reference to itself is not a use
            if (cte.columns.empty()) {
                now.cols.clear();
                for (size_t i = 0; i < result.size(); i++) {
                    Col c = result[i];
                    SqlIdxTarget ct;
                    ct.kind = SqlIdxTarget::CteColumn;
                    ct.name = c.name;
                    ct.table = cte.name.text;
                    ct.local = true;
                    ct.decl_tok = i < result_toks.size() ? result_toks[i] : -1;
                    if (ct.decl_tok >= 0) {
                        c.target = Target("ctecol:" + std::to_string(ct.decl_tok), ct);
                    }
                    now.cols.push_back(c);
                }
                now.known = known;
            } else if (known && result.size() != cte.columns.size()) {
                now.count_problem = "table " + cte.name.text + " has " + std::to_string(result.size()) + " values for " +
                                    std::to_string(cte.columns.size()) + " columns";
            }
            std::vector<std::string> names;
            for (const Col &c : now.cols) names.push_back(c.name);
            x_->targets[static_cast<size_t>(now.target)].columns = names;
        }
    }

    void FinishWith(Scope *sc) {
        for (const auto &c : sc->ctes) {
            const CteInfo &info = ctes_[static_cast<size_t>(c.second)];
            if (info.refs == 0) {
                Diag(info.tok, info.tok, SqlLspSeverity::Warning, "unused-cte",
                     "CTE " + info.name + " is never used");
            } else if (!info.count_problem.empty()) {
                Diag(info.tok, info.tok, SqlLspSeverity::Error, "cte-column-count", info.count_problem);
            }
        }
    }

    void FromItems(const std::vector<SqlFromItem> &items, Scope *core_scope, Scope *outer) {
        for (const SqlFromItem &fi : items) {
            Source src;
            switch (fi.kind) {
                case SqlFromItem::Table:
                    TableSource(core_scope, fi.db, fi.name, fi.alias, &src);
                    break;
                case SqlFromItem::TableFunc: {
                    std::vector<std::string> cols;
                    src.table = fi.name.text;
                    src.display = fi.alias.ok() ? fi.alias.text : fi.name.text;
                    src.name = SqlLower(src.display);
                    src.rowid = false;
                    if (SqlTableFunctionColumns(fi.name.text, &cols)) {
                        for (const std::string &c : cols) {
                            Col col;
                            col.name = c;
                            col.lname = c;
                            src.cols.push_back(col);
                        }
                    } else {
                        // An eponymous virtual table (or an app's): columns unknown.
                        src.known = false;
                        const SqlLspObject *o = FindObject(fi.name.text);
                        if (o) src = ObjectSource(o, fi.alias, fi.name.tok);
                    }
                    if (fi.alias.ok() && src.qual_target < 0) src.qual_target = AliasTarget(fi.alias, fi.name.text, src);
                    for (const SqlExprP &a : fi.args) Expr(a.get(), outer ? outer : core_scope);
                    break;
                }
                case SqlFromItem::Subquery: {
                    std::vector<Col> cols;
                    bool known = true;
                    Select(*fi.select, core_scope->parent, &cols, &known);
                    src.cols = cols;
                    src.known = known;
                    src.rowid = false;
                    src.display = fi.alias.ok() ? fi.alias.text : std::string();
                    src.name = SqlLower(src.display);
                    if (fi.alias.ok()) src.qual_target = AliasTarget(fi.alias, "(subquery)", src);
                    break;
                }
                case SqlFromItem::Group:
                    FromItems(fi.group, core_scope, outer);
                    continue;
            }
            // USING / NATURAL: the right side's copies of the shared columns stop being separately visible.
            if (!core_scope->sources.empty()) {
                std::vector<std::string> shared;
                for (const SqlName &u : fi.using_cols) shared.push_back(SqlLower(u.text));
                if (fi.natural) {
                    for (const Col &c : src.cols) {
                        for (const Source &left : core_scope->sources) {
                            for (const Col &lc : left.cols) {
                                if (lc.lname == c.lname) shared.push_back(c.lname);
                            }
                        }
                    }
                }
                for (const std::string &s : shared) src.merged.insert(s);
                for (size_t ui = 0; ui < fi.using_cols.size(); ui++) {
                    const SqlName &u = fi.using_cols[ui];
                    const std::string l = SqlLower(u.text);
                    bool right = false;
                    for (const Col &c : src.cols) {
                        if (c.lname == l) {
                            right = true;
                            Occ(u.tok, c.target);
                        }
                    }
                    bool left = false;
                    for (const Source &ls : core_scope->sources) {
                        for (const Col &c : ls.cols) {
                            if (c.lname == l) left = true;
                        }
                    }
                    bool left_known = true;
                    for (const Source &ls : core_scope->sources) left_known = left_known && ls.known;
                    if ((!right && src.known) || (!left && left_known)) {
                        Diag(u.tok, u.tok, SchemaSeverity(), "unknown-column",
                             "cannot join using column " + u.text + " - column not present in both tables");
                    }
                }
            }
            for (const Source &other : core_scope->sources) {
                if (!src.name.empty() && other.name == src.name) {
                    Diag(fi.alias.ok() ? fi.alias.tok : fi.name.tok, fi.alias.ok() ? fi.alias.tok : fi.name.tok,
                         SqlLspSeverity::Warning, "duplicate-alias",
                         "the FROM clause names " + src.display + " twice: qualified references are ambiguous");
                    break;
                }
            }
            core_scope->sources.push_back(std::move(src));
        }
        for (const SqlFromItem &fi : items) {
            if (fi.on) Expr(fi.on.get(), core_scope);
        }
    }

    /**
     * @brief Resolves a SELECT; returns its result columns (names, and the column they come from) and whether they
     *        are all known.
     */
    void Select(const SqlSelect &sel, Scope *outer, std::vector<Col> *out, bool *known,
                std::vector<int> *out_toks = nullptr) {
        Scope with_scope;
        with_scope.parent = outer;
        Scope *base = outer;
        if (sel.with) {
            With(*sel.with, &with_scope);
            base = &with_scope;
        }
        size_t first_count = 0;
        std::vector<Col> first_cols;
        bool first_known = true;
        for (size_t ci = 0; ci < sel.cores.size(); ci++) {
            const SqlCore &core = sel.cores[ci];
            std::vector<Col> cols;
            bool k = true;
            std::vector<int> toks;
            Core(core, base, &cols, &k, &toks, sel.cores.size() > 1, ci == 0 ? nullptr : &first_cols);
            if (ci == 0) first_cols = cols;
            if (ci == 0) {
                first_count = cols.size();
                first_known = k;
                if (out) *out = cols;
                if (known) *known = k;
                if (out_toks) *out_toks = toks;
            } else if (k && first_known && cols.size() != first_count && core.compound_op >= 0) {
                Diag(core.compound_op, core.compound_op, SqlLspSeverity::Error, "compound-column-count",
                     "SELECTs to the left and right of " + P().Upper(core.compound_op) +
                         " do not have the same number of result columns");
            }
        }
        if (sel.with) FinishWith(&with_scope);
    }

    void Core(const SqlCore &core, Scope *outer, std::vector<Col> *out, bool *known, std::vector<int> *toks,
              bool compound, const std::vector<Col> *first_cols = nullptr) {
        Scope sc;
        sc.parent = outer;
        scope_counter_++;
        if (core.is_values) {
            size_t width = 0;
            for (size_t r = 0; r < core.rows.size(); r++) {
                for (const SqlExprP &e : core.rows[r]) Expr(e.get(), &sc);
                if (r == 0) {
                    width = core.rows[r].size();
                } else if (core.rows[r].size() != width) {
                    Diag(core.rows[r].front()->first, core.rows[r].back()->last, SqlLspSeverity::Error,
                         "values-count", "all VALUES must have the same number of terms");
                }
            }
            for (size_t i = 0; i < width; i++) {
                Col c;
                c.name = "column" + std::to_string(i + 1);
                c.lname = c.name;
                out->push_back(c);
                toks->push_back(-1);
            }
            *known = true;
            return;
        }
        FromItems(core.from, &sc, outer);
        // Named windows first: result columns refer to them.
        for (const SqlWindowDef &w : core.windows) {
            SqlIdxTarget t;
            t.kind = SqlIdxTarget::Window;
            t.name = w.name.text;
            t.local = true;
            t.decl_tok = w.name.tok;
            const int id = Target("win:" + std::to_string(w.name.tok), t);
            Occ(w.name.tok, id, true);
            sc.windows.emplace_back(SqlLower(w.name.text), id);
        }
        for (const SqlWindowDef &w : core.windows) Window(w.spec, &sc);
        *known = true;
        for (const SqlResultCol &rc : core.cols) {
            if (rc.star) {
                bool matched = false;
                for (const Source &src : sc.sources) {
                    if (rc.star_table.ok() && src.name != SqlLower(rc.star_table.text)) continue;
                    if (rc.star_table.ok()) {
                        Occ(rc.star_table.tok, src.qual_target);
                    }
                    matched = true;
                    if (!src.known) *known = false;
                    for (const Col &c : src.cols) {
                        if (!c.star) continue;
                        if (!rc.star_table.ok() && src.merged.count(c.lname)) continue;
                        out->push_back(c);
                        toks->push_back(-1);
                    }
                }
                if (rc.star_table.ok() && !matched && AllKnown(&sc) && !sc.sources.empty()) {
                    Diag(rc.star_table.tok, rc.star_table.tok, SchemaSeverity(), "unknown-table",
                         "no such table: " + rc.star_table.text);
                }
                if (!rc.star_table.ok() && sc.sources.empty()) {
                    Diag(rc.first, rc.first, SqlLspSeverity::Error, "star-without-from", "no tables specified");
                }
                continue;
            }
            Expr(rc.expr.get(), &sc);
            Col c;
            c.name = ResultName(P(), rc);
            c.lname = SqlLower(c.name);
            if (rc.expr && rc.expr->kind == SqlExpr::Column) {
                for (const SqlIdxOcc &o : x_->occs) {
                    if (o.tok == rc.expr->column.tok) {
                        c.target = o.target;
                        c.type = x_->targets[static_cast<size_t>(o.target)].type;
                    }
                }
            }
            int tok = rc.alias.ok() ? rc.alias.tok : (rc.expr && rc.expr->kind == SqlExpr::Column ? rc.expr->column.tok : -1);
            if (rc.alias.ok()) {
                SqlIdxTarget t;
                t.kind = SqlIdxTarget::ResultAlias;
                t.name = rc.alias.text;
                t.local = true;
                t.decl_tok = rc.alias.tok;
                const int id = Target("ralias:" + std::to_string(rc.alias.tok), t);
                Occ(rc.alias.tok, id, true);
                sc.aliases.emplace_back(SqlLower(rc.alias.text), id);
                c.target = id;
            }
            out->push_back(c);
            toks->push_back(tok);
        }
        Expr(core.where.get(), &sc, Ctx::Where);
        NoAggregates(core.where.get(), "WHERE", &sc);
        for (const SqlExprP &g : core.group) Expr(g.get(), &sc, Ctx::OrderBy);
        Expr(core.having.get(), &sc, Ctx::OrderBy);
        for (const SqlOrderTerm &o : core.order) {
            // A compound's ORDER BY names result columns (positions, or the first SELECT's names).
            if (compound && o.expr && o.expr->kind == SqlExpr::Column && !o.expr->table.ok()) {
                bool named = false;
                for (const Col &c : *out) named = named || c.lname == SqlLower(o.expr->column.text);
                if (first_cols) {
                    for (const Col &c : *first_cols) named = named || c.lname == SqlLower(o.expr->column.text);
                }
                if (named) continue;
            }
            Expr(o.expr.get(), &sc, Ctx::OrderBy);
        }
        Expr(core.limit.get(), &sc);
        Expr(core.offset.get(), &sc);
    }

    // --- statements --------------------------------------------------------

    /** @brief A pseudo-source with a table's columns under another name (NEW, OLD, excluded). */
    Source Renamed(const Source &s, const std::string &name) {
        Source r = s;
        r.name = name;
        r.display = name;
        r.merged.clear();
        return r;
    }

    void Returning(const std::vector<SqlResultCol> &cols, Scope *sc) {
        for (const SqlResultCol &rc : cols) {
            if (rc.expr) Expr(rc.expr.get(), sc);
        }
    }

    /** @brief The columns an INSERT without a column list fills at statement `at`, or -1 when not knowable. */
    int CountInsertable(const Source &s, const SqlLspObject *o, int at) {
        if (!o) return static_cast<int>(s.cols.size());
        if (o->redeclared || o->kind != SqlLspObject::Table) return -1;
        int n = 0;
        // Generated columns cannot be inserted into: SQLite leaves them out of the count; a column a later
        // ALTER TABLE adds is not there yet.
        (void)at;
        for (const SqlLspColumn &c : o->columns) {
            if (c.hidden || c.generated || !ColumnVisible(o, c)) continue;
            n++;
        }
        return n;
    }

    void Statement(const SqlStmt &st, Scope *outer) {
        Scope sc;
        sc.parent = outer;
        Scope *with_scope = &sc;
        Scope ws;
        if (st.with) {
            ws.parent = outer;
            With(*st.with, &ws);
            sc.parent = &ws;
            with_scope = &ws;
        }
        switch (st.kind) {
            case SqlStmt::Select:
                if (st.select) Select(*st.select, outer, nullptr, nullptr);
                break;
            case SqlStmt::Insert: {
                Source target;
                const bool found = TableSource(&sc, st.db, st.name, st.alias, &target);
                const SqlLspObject *obj = found ? FindObject(st.name.text) : nullptr;
                for (const SqlName &c : st.columns) {
                    bool hit = false;
                    for (const Col &col : target.cols) {
                        if (col.lname == SqlLower(c.text)) {
                            Occ(c.tok, col.target, false, true);
                            hit = true;
                        }
                    }
                    if (!hit && !IsRowid(SqlLower(c.text)) && target.known && found) {
                        Diag(c.tok, c.tok, SchemaSeverity(), "unknown-column",
                             "table " + st.name.text + " has no column named " + c.text);
                    }
                }
                if (st.select) {
                    std::vector<Col> cols;
                    bool known = true;
                    Select(*st.select, with_scope == &sc ? outer : with_scope, &cols, &known);
                    const SqlCore &first = st.select->cores.front();
                    const int want = st.columns.empty() ? (target.known && found ? CountInsertable(target, obj, st.first) : -1)
                                                        : static_cast<int>(st.columns.size());
                    if (want >= 0 && known && static_cast<int>(cols.size()) != want && st.select->cores.size() == 1) {
                        const int at = first.is_values && !first.rows.empty() ? first.rows.front().front()->first
                                                                              : first.first;
                        const int to = first.is_values && !first.rows.empty() ? first.rows.front().back()->last : at;
                        if (st.columns.empty()) {
                            Diag(at, to, SqlLspSeverity::Error, "insert-count",
                                 "table " + st.name.text + " has " + std::to_string(want) + " columns but " +
                                     std::to_string(cols.size()) + " values were supplied");
                        } else {
                            Diag(at, to, SqlLspSeverity::Error, "insert-count",
                                 std::to_string(cols.size()) + " values for " + std::to_string(want) + " columns");
                        }
                    }
                }
                sc.sources.push_back(target);
                for (const SqlUpsert &up : st.upserts) {
                    Scope us;
                    us.parent = with_scope == &sc ? outer : with_scope;
                    us.sources.push_back(target);
                    for (const SqlOrderTerm &o : up.target) Expr(o.expr.get(), &us);
                    Expr(up.target_where.get(), &us);
                    us.sources.push_back(Renamed(target, "excluded"));
                    for (const SqlSetItem &si : up.set) {
                        SetTargets(si, target);
                        Expr(si.value.get(), &us);
                    }
                    Expr(up.where.get(), &us);
                }
                Returning(st.returning, &sc);
                break;
            }
            case SqlStmt::Update: {
                Source target;
                TableSource(&sc, st.db, st.name, st.alias, &target);
                sc.sources.push_back(target);
                FromItems(st.from, &sc, sc.parent);
                for (const SqlSetItem &si : st.set) {
                    SetTargets(si, target);
                    Expr(si.value.get(), &sc);
                }
                Expr(st.where.get(), &sc, Ctx::Where);
                NoAggregates(st.where.get(), "WHERE", &sc);
                Returning(st.returning, &sc);
                for (const SqlOrderTerm &o : st.order) Expr(o.expr.get(), &sc);
                Expr(st.limit.get(), &sc);
                break;
            }
            case SqlStmt::Delete: {
                Source target;
                TableSource(&sc, st.db, st.name, st.alias, &target);
                sc.sources.push_back(target);
                Expr(st.where.get(), &sc, Ctx::Where);
                NoAggregates(st.where.get(), "WHERE", &sc);
                Returning(st.returning, &sc);
                for (const SqlOrderTerm &o : st.order) Expr(o.expr.get(), &sc);
                Expr(st.limit.get(), &sc);
                break;
            }
            case SqlStmt::CreateTable: {
                const SqlLspObject *o = FindObject(st.name.text);
                if (o && o->origin.empty()) {
                    Occ(st.name.tok, ObjectTarget(o), true);
                    std::set<std::string> seen;
                    Source self = ObjectSource(o, SqlName(), -1);
                    for (const SqlColumnDef &cd : st.coldefs) {
                        const std::string l = SqlLower(cd.name.text);
                        if (!seen.insert(l).second) {
                            Diag(cd.name.tok, cd.name.tok, SqlLspSeverity::Error, "duplicate-column",
                                 "duplicate column name: " + cd.name.text);
                        }
                        for (const Col &c : self.cols) {
                            if (c.lname == l && c.target >= 0 &&
                                x_->targets[static_cast<size_t>(c.target)].decl_tok == cd.name.tok) {
                                Occ(cd.name.tok, c.target, true);
                            }
                        }
                    }
                    Scope ts;
                    ts.ddl = true;
                    ts.sources.push_back(self);
                    for (const SqlColumnDef &cd : st.coldefs) {
                        for (const SqlExprP &e : cd.exprs) Expr(e.get(), &ts);
                        References(cd.ref_table, cd.ref_cols);
                    }
                    for (const SqlTableConstraint &tc : st.constraints) {
                        for (const SqlOrderTerm &t : tc.terms) Expr(t.expr.get(), &ts);
                        Expr(tc.check.get(), &ts);
                        for (const SqlName &c : tc.cols) {
                            for (const Col &col : self.cols) {
                                if (col.lname == SqlLower(c.text)) Occ(c.tok, col.target);
                            }
                        }
                        References(tc.ref_table, tc.ref_cols);
                    }
                }
                if (st.select) Select(*st.select, outer, nullptr, nullptr);
                break;
            }
            case SqlStmt::CreateView: {
                const SqlLspObject *o = FindObject(st.name.text);
                if (o && o->origin.empty()) {
                    Occ(st.name.tok, ObjectTarget(o), true);
                    for (size_t i = 0; i < o->columns.size(); i++) {
                        if (o->columns[i].decl_tok >= 0) Occ(o->columns[i].decl_tok, ColumnTarget(o, static_cast<int>(i)), !st.columns.empty());
                    }
                }
                std::vector<Col> cols;
                bool known = true;
                if (st.select) Select(*st.select, outer, &cols, &known);
                if (!st.columns.empty() && known && cols.size() != st.columns.size()) {
                    Diag(st.name.tok, st.name.tok, SqlLspSeverity::Error, "view-column-count",
                         "expected " + std::to_string(st.columns.size()) + " columns for '" + st.name.text +
                             "' but got " + std::to_string(cols.size()));
                }
                break;
            }
            case SqlStmt::CreateVirtualTable: {
                const SqlLspObject *o = FindObject(st.name.text);
                if (o && o->origin.empty()) {
                    Occ(st.name.tok, ObjectTarget(o), true);
                    for (size_t i = 0; i < o->columns.size(); i++) {
                        if (o->columns[i].decl_tok >= 0) Occ(o->columns[i].decl_tok, ColumnTarget(o, static_cast<int>(i)), true);
                    }
                }
                break;
            }
            case SqlStmt::CreateIndex: {
                const SqlLspObject *o = FindObject(st.name.text);
                if (o && o->origin.empty()) Occ(st.name.tok, ObjectTarget(o), true);
                Source target;
                TableSource(&sc, SqlName(), st.on_table, SqlName(), &target);
                Scope ts;
                ts.ddl = true;
                ts.sources.push_back(target);
                for (const SqlOrderTerm &t : st.index_terms) Expr(t.expr.get(), &ts);
                Expr(st.where.get(), &ts);
                break;
            }
            case SqlStmt::CreateTrigger: {
                const SqlLspObject *o = FindObject(st.name.text);
                if (o && o->origin.empty()) Occ(st.name.tok, ObjectTarget(o), true);
                Source target;
                TableSource(&sc, st.on_db, st.on_table, SqlName(), &target);
                for (const SqlName &c : st.columns) {
                    bool hit = false;
                    for (const Col &col : target.cols) {
                        if (col.lname == SqlLower(c.text)) {
                            Occ(c.tok, col.target);
                            hit = true;
                        }
                    }
                    if (!hit && target.known && !target.cols.empty()) {
                        Diag(c.tok, c.tok, SchemaSeverity(), "unknown-column", "no such column: " + c.text);
                    }
                }
                Scope ts;
                ts.parent = outer;
                ts.ddl = true;
                ts.sources.push_back(Renamed(target, "new"));
                ts.sources.push_back(Renamed(target, "old"));
                // NEW/OLD name the table's row: an unqualified column is not one of them (SQLite needs NEW.x).
                Scope qual;
                qual.parent = outer;
                qual.ddl = true;
                for (Source s : ts.sources) {
                    qual.sources.push_back(s);
                }
                Expr(st.when.get(), &qual);
                for (const SqlStmt &body : st.body) Statement(body, &qual);
                break;
            }
            case SqlStmt::DropTable:
            case SqlStmt::DropView:
            case SqlStmt::DropIndex:
            case SqlStmt::DropTrigger: {
                const SqlLspObject *o = FindObject(st.name.text);
                if (o) {
                    Occ(st.name.tok, ObjectTarget(o));
                } else if (!st.if_exists && ctx_.database_loaded && !st.db.ok()) {
                    const char *what = st.kind == SqlStmt::DropTable ? "table" : st.kind == SqlStmt::DropView ? "view"
                                       : st.kind == SqlStmt::DropIndex ? "index" : "trigger";
                    Diag(st.name.tok, st.name.tok, SqlLspSeverity::Error, "unknown-table",
                         std::string("no such ") + what + ": " + st.name.text);
                }
                break;
            }
            case SqlStmt::AlterTable: {
                Source target;
                TableSource(&sc, st.db, st.name, SqlName(), &target);
                if ((st.alter == SqlStmt::RenameColumn || st.alter == SqlStmt::DropColumn ||
                     st.alter == SqlStmt::OtherAlter) && st.column.ok()) {
                    bool hit = false;
                    for (const Col &col : target.cols) {
                        if (col.lname == SqlLower(st.column.text)) {
                            Occ(st.column.tok, col.target);
                            hit = true;
                        }
                    }
                    if (!hit && target.known && !target.cols.empty()) {
                        Diag(st.column.tok, st.column.tok, SchemaSeverity(), "unknown-column",
                             "no such column: \"" + st.column.text + "\"");
                    }
                }
                if (st.alter == SqlStmt::AddColumn && st.add_column) {
                    const SqlLspObject *o = FindObject(st.name.text);
                    if (o && o->origin.empty()) {
                        for (size_t i = 0; i < o->columns.size(); i++) {
                            if (o->columns[i].decl_tok == st.add_column->name.tok) {
                                Occ(st.add_column->name.tok, ColumnTarget(o, static_cast<int>(i)), true);
                            }
                        }
                    }
                }
                break;
            }
            case SqlStmt::Pragma: {
                if (st.name.ok() && !SqlLspFindPragma(SqlLower(st.name.text))) {
                    Diag(st.name.tok, st.name.tok, SqlLspSeverity::Warning, "unknown-pragma",
                         "unknown pragma: " + st.name.text + " (SQLite ignores it)");
                }
                const std::string l = SqlLower(st.name.text);
                if (st.value_first >= 0 && st.value_first == st.value_last &&
                    (l == "table_info" || l == "table_xinfo" || l == "index_list" || l == "foreign_key_list")) {
                    SqlName n;
                    n.tok = st.value_first;
                    n.text = SqlDequote(P().Text(st.value_first));
                    if (const SqlLspObject *o = FindObject(n.text)) Occ(n.tok, ObjectTarget(o));
                }
                break;
            }
            case SqlStmt::Attach:
            case SqlStmt::Detach:
            case SqlStmt::Vacuum:
                break;
            default:
                break;
        }
        if (st.with) FinishWith(&ws);
    }

    void SetTargets(const SqlSetItem &si, const Source &target) {
        for (const SqlName &c : si.cols) {
            bool hit = false;
            for (const Col &col : target.cols) {
                if (col.lname == SqlLower(c.text)) {
                    Occ(c.tok, col.target, false, true);
                    hit = true;
                }
            }
            if (!hit && target.known && !IsRowid(SqlLower(c.text)) && !target.table.empty()) {
                Diag(c.tok, c.tok, SchemaSeverity(), "unknown-column", "no such column: " + c.text);
            }
        }
    }

    void References(const SqlName &table, const std::vector<SqlName> &cols) {
        if (!table.ok()) return;
        // A foreign key may name a table created later (or never): link it when known, never report it.
        const SqlLspObject *o = FindObject(table.text);
        if (!o) return;
        Occ(table.tok, ObjectTarget(o));
        for (const SqlName &c : cols) {
            for (size_t i = 0; i < o->columns.size(); i++) {
                if (SqlLower(o->columns[i].name) == SqlLower(c.text)) Occ(c.tok, ColumnTarget(o, static_cast<int>(i)));
            }
        }
    }
};

}  // namespace

SqlIndex SqlBuildIndex(const std::vector<std::string> &lines, const SqlLspContext &ctx) {
    SqlIndex idx;
    idx.parse = SqlParseScript(SqlBlankDotCommands(SqlJoinLines(lines)));
    idx.lines = SqlLineIndex(idx.parse.text);
    idx.local = SqlObjectsOf(idx.parse, idx.lines, "", "");
    Resolver r(&idx, ctx);
    r.Run();
    return idx;
}

std::vector<SqlLspObject> SqlLspScriptObjects(const std::string &text, const std::string &uri) {
    SqlParse p = SqlParseScript(SqlBlankDotCommands(text));
    SqlLineIndex li(p.text);
    return SqlObjectsOf(p, li, uri.empty() ? "workspace" : uri, uri);
}

std::vector<SqlLspObject> SqlLspSchemaObjects(const std::vector<std::vector<std::string>> &rows,
                                              const std::string &origin) {
    std::string script;
    for (const auto &row : rows) {
        if (row.size() < 4 || row[3].empty()) continue;
        script += row[3];
        script += ";\n";
    }
    SqlParse p = SqlParseScript(script);
    SqlLineIndex li(p.text);
    std::vector<SqlLspObject> objs = SqlObjectsOf(p, li, origin.empty() ? "database" : origin, "");
    for (SqlLspObject &o : objs) {
        o.has_range = false;
        for (SqlLspColumn &c : o.columns) c.has_range = false;
    }
    return objs;
}

SqlLspDatabaseHeader SqlLspFindDatabaseHeader(const std::vector<std::string> &lines) {
    SqlLspDatabaseHeader h;
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string &l = lines[i];
        size_t p = 0;
        while (p < l.size() && (l[p] == ' ' || l[p] == '\t' || l[p] == '\r')) p++;
        if (p == l.size()) continue;  // blank lines are part of the header block
        if (l.compare(p, 2, "--") != 0) break;
        p += 2;
        while (p < l.size() && (l[p] == ' ' || l[p] == '\t')) p++;
        if (SqlLower(l.substr(p, 9)) != "database:") continue;
        p += 9;
        while (p < l.size() && (l[p] == ' ' || l[p] == '\t')) p++;
        size_t e = l.size();
        while (e > p && (l[e - 1] == ' ' || l[e - 1] == '\t' || l[e - 1] == '\r')) e--;
        if (e == p) continue;
        h.found = true;
        h.path = l.substr(p, e - p);
        h.line = static_cast<int>(i);
        h.col = static_cast<int>(p);
        h.end_col = static_cast<int>(e);
        return h;
    }
    return h;
}
