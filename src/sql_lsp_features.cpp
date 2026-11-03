// mep-sql-lsp's features over the semantic index (sql_lsp_analysis.cpp):
// diagnostics, completion, hover, signature help, navigation, rename,
// outline, folding and the formatting adapter. See sql_lsp.h.
#include <algorithm>
#include <cctype>
#include <map>
#include <set>

#include "sql_format.h"
#include "sql_lsp.h"
#include "sql_lsp_index.h"

namespace {

bool IsIdent(SqlTok t) { return t == SqlTok::Id || SqlIsKeyword(t); }

bool IsNameTok(const SqlParse &p, int tok) {
    if (tok < 0) return false;
    const SqlTok t = p.tokens[static_cast<size_t>(tok)].type;
    return t == SqlTok::Id || t == SqlTok::String || SqlIsKeyword(t);
}

/** @brief A name as SQL text: bare when it is a plain identifier that is not a keyword, else "double-quoted". */
std::string QuoteName(const std::string &n) {
    bool plain = !n.empty() && !(n[0] >= '0' && n[0] <= '9');
    for (char c : n) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!(u == '_' || u >= 0x80 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            plain = false;
        }
    }
    if (plain && SqlKeywordType(n) == SqlTok::Id) return n;
    std::string out = "\"";
    for (char c : n) {
        if (c == '"') out += '"';
        out += c;
    }
    return out + "\"";
}

std::vector<SqlLspObject> AllObjects(const SqlIndex &idx, const SqlLspContext &ctx) {
    std::vector<SqlLspObject> out = idx.local;
    std::set<std::string> seen;
    for (const SqlLspObject &o : out) seen.insert(SqlLower(o.name));
    for (const auto *list : {&ctx.database, &ctx.workspace}) {
        for (const SqlLspObject &o : *list) {
            if (seen.insert(SqlLower(o.name)).second) out.push_back(o);
        }
    }
    return out;
}

const char *KindName(SqlLspObject::Kind k) {
    switch (k) {
        case SqlLspObject::Table: return "table";
        case SqlLspObject::View: return "view";
        case SqlLspObject::Index: return "index";
        case SqlLspObject::Trigger: return "trigger";
        case SqlLspObject::VirtualTable: return "virtual table";
    }
    return "table";
}

std::string ColumnsBlock(const SqlLspObject &o) {
    std::string out;
    for (const SqlLspColumn &c : o.columns) {
        out += "    " + c.name;
        if (!c.type.empty()) out += " " + c.type;
        if (c.primary_key) out += " PRIMARY KEY";
        if (c.not_null) out += " NOT NULL";
        out += "\n";
    }
    return out;
}

std::string ObjectHover(const SqlLspObject &o) {
    std::string out = "**" + std::string(KindName(o.kind)) + "** `" + o.name + "`";
    if (!o.table.empty()) out += " on `" + o.table + "`";
    if (!o.origin.empty()) out += " (from " + o.origin + ")";
    out += "\n\n";
    if (!o.sql.empty()) {
        out += "```sql\n" + o.sql + "\n```\n";
    } else if (!o.columns.empty()) {
        out += "```\n" + ColumnsBlock(o) + "```\n";
    }
    return out;
}

// SQLite's column affinity for a declared type name (datatype3.html, section 3.1): the first rule that
// matches wins.
std::string SqlUpper(std::string s) {
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

const char *TypeAffinity(const std::string &upper) {
    if (upper.find("INT") != std::string::npos) return "INTEGER";
    if (upper.find("CHAR") != std::string::npos || upper.find("CLOB") != std::string::npos ||
        upper.find("TEXT") != std::string::npos)
        return "TEXT";
    if (upper.empty() || upper.find("BLOB") != std::string::npos) return "BLOB";
    if (upper.find("REAL") != std::string::npos || upper.find("FLOA") != std::string::npos ||
        upper.find("DOUB") != std::string::npos)
        return "REAL";
    return "NUMERIC";
}

std::string TypeHover(const SqlParse &p, int first, int last) {
    std::string name, upper;
    for (int t = first; t <= last; t++) {
        const SqlTok tt = p.tokens[static_cast<size_t>(t)].type;
        if (tt == SqlTok::Comment) continue;
        const std::string text(p.Text(t));
        if (!name.empty() && tt != SqlTok::LP && tt != SqlTok::RP && tt != SqlTok::Comma &&
            p.tokens[static_cast<size_t>(t - 1)].type != SqlTok::LP && p.tokens[static_cast<size_t>(t - 1)].type != SqlTok::Comma)
            name += ' ';
        name += text;
        if (tt != SqlTok::LP && tt != SqlTok::RP && tt != SqlTok::Comma) upper += SqlUpper(SqlDequote(text)) + " ";
    }
    const std::string affinity = TypeAffinity(upper);
    std::string out = "```sql\n" + name + "\n```\n\n**type** with " + affinity + " affinity. ";
    static const struct {
        const char *affinity, *doc;
    } kDocs[] = {
        {"INTEGER", "Values that look like numbers are stored as integers when that loses nothing, else as REAL; "
                    "other text and blobs are stored as they are."},
        {"TEXT", "Numbers are converted to text before being stored; NULL and blobs are stored as they are."},
        {"BLOB", "Values are stored exactly as given, with no conversion."},
        {"REAL", "Numbers, and text that looks like one, are stored as floating point."},
        {"NUMERIC", "Text that looks like a number is converted to INTEGER or REAL, whichever keeps it exactly; "
                    "anything else is stored as it is."},
    };
    for (const auto &k : kDocs) {
        if (std::string(k.affinity) == affinity) out += k.doc;
    }
    out += "\n\nSQLite does not enforce a declared type: it picks the affinity from the name (INT anywhere "
           "gives INTEGER; else CHAR, CLOB or TEXT gives TEXT; BLOB or no type gives BLOB; REAL, FLOA or DOUB "
           "gives REAL; anything else NUMERIC), and any column can still hold any value -- unless the table is "
           "STRICT, where only INT, INTEGER, REAL, TEXT, BLOB and ANY are allowed and are enforced.";
    for (const SqlStmt &st : p.stmts) {
        for (const SqlColumnDef &c : st.coldefs) {
            if (c.type_first == first && c.primary_key && SqlUpper(c.type) == "INTEGER")
                out += "\n\nDeclared exactly INTEGER PRIMARY KEY, this column is an alias for the rowid.";
        }
    }
    return out;
}

std::string FunctionHover(const SqlLspFunction &f) {
    std::string out = "```sql\n" + std::string(f.signatures) + "\n```\n\n";
    out += std::string("*") + f.kind + " function*";
    if (*f.doc) out += "\n\n" + std::string(f.doc);
    return out;
}

}  // namespace

// --- Diagnostics -------------------------------------------------------------

std::vector<SqlLspDiagnostic> SqlLspDiagnostics(const std::vector<std::string> &lines, const SqlLspContext &ctx) {
    SqlIndex idx = SqlBuildIndex(lines, ctx);
    std::vector<SqlLspDiagnostic> out;
    for (const SqlSyntaxError &e : idx.parse.errors) {
        SqlLspDiagnostic d;
        int tok = e.tok;
        if (tok >= 0 && idx.parse.tokens[static_cast<size_t>(tok)].type == SqlTok::Eof) {
            // incomplete input: point at the statement's last token
            int t = tok - 1;
            while (t >= 0 && idx.parse.tokens[static_cast<size_t>(t)].type == SqlTok::Comment) t--;
            if (t >= 0) tok = t;
        }
        d.range = idx.RangeOf(tok);
        if (d.range.end_line == d.range.line && d.range.end_col == d.range.col) d.range.end_col++;
        d.severity = SqlLspSeverity::Error;
        d.code = e.syntax ? "syntax-error" : "invalid-clause";
        d.message = e.message;
        out.push_back(d);
    }
    for (const SqlLspDiagnostic &d : idx.diags) out.push_back(d);
    if (!ctx.database_problem.empty()) {
        const SqlLspDatabaseHeader h = SqlLspFindDatabaseHeader(lines);
        if (h.found) {
            SqlLspDiagnostic d;
            d.range.line = d.range.end_line = h.line;
            d.range.col = h.col;
            d.range.end_col = h.end_col;
            d.severity = SqlLspSeverity::Information;
            d.code = "database-unavailable";
            d.message = ctx.database_problem + ": schema checks use the document's own DDL only";
            out.push_back(d);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const SqlLspDiagnostic &a, const SqlLspDiagnostic &b) {
        if (a.range.line != b.range.line) return a.range.line < b.range.line;
        return a.range.col < b.range.col;
    });
    std::vector<SqlLspDiagnostic> uniq;
    for (const SqlLspDiagnostic &d : out) {
        bool dup = false;
        for (const SqlLspDiagnostic &u : uniq) {
            dup = dup || (u.range.line == d.range.line && u.range.col == d.range.col && u.message == d.message);
        }
        if (!dup) uniq.push_back(d);
    }
    return uniq;
}

std::string SqlLspFormatDiagnostic(const std::string &path, const SqlLspDiagnostic &d) {
    const char *sev = d.severity == SqlLspSeverity::Error     ? "error"
                      : d.severity == SqlLspSeverity::Warning ? "warning"
                      : d.severity == SqlLspSeverity::Information ? "info"
                                                                  : "hint";
    return path + ":" + std::to_string(d.range.line + 1) + ":" + std::to_string(d.range.col + 1) + ": " + sev + ": " +
           d.message + " [" + d.code + "]";
}

bool SqlLspDiagnosticFails(const SqlLspDiagnostic &d) {
    return d.severity == SqlLspSeverity::Error || d.severity == SqlLspSeverity::Warning;
}

// --- Completion --------------------------------------------------------------

namespace {

struct CompletionScope {
    // FROM-clause sources of the statement around the cursor: (qualifier, table/CTE name)
    std::vector<std::pair<std::string, std::string>> sources;
    // CTE names and the columns they declare (or select)
    std::vector<std::pair<std::string, std::vector<std::string>>> ctes;
};

/** @brief The chunk (statement) containing a byte offset. */
int ChunkAt(const SqlParse &p, int off) {
    for (size_t i = 0; i < p.chunks.size(); i++) {
        if (off >= p.chunks[i].start && off <= p.chunks[i].end) return static_cast<int>(i);
    }
    return p.chunks.empty() ? -1 : static_cast<int>(p.chunks.size()) - 1;
}

/** @brief Token-level scan of a statement for its tables and aliases (works on half-typed statements too). */
CompletionScope ScanScope(const SqlParse &p, int first, int end) {
    CompletionScope sc;
    auto type = [&](int i) { return i < end ? p.tokens[static_cast<size_t>(i)].type : SqlTok::Eof; };
    auto next_sig = [&](int i) {
        i++;
        while (i < end && type(i) == SqlTok::Comment) i++;
        return i;
    };
    for (int i = first; i < end; i = next_sig(i)) {
        const SqlTok t = type(i);
        if (t == SqlTok::With || (t == SqlTok::Comma && !sc.ctes.empty())) {
            int n = next_sig(i);
            if (type(n) == SqlTok::Recursive) n = next_sig(n);
            if (!IsNameTok(p, n)) continue;
            int k = next_sig(n);
            std::vector<std::string> cols;
            if (type(k) == SqlTok::LP) {
                k = next_sig(k);
                while (k < end && type(k) != SqlTok::RP) {
                    if (IsNameTok(p, k)) cols.push_back(SqlDequote(p.Text(k)));
                    k = next_sig(k);
                }
                k = next_sig(k);
            }
            if (type(k) == SqlTok::As) sc.ctes.emplace_back(SqlDequote(p.Text(n)), cols);
            continue;
        }
        if (t == SqlTok::From || t == SqlTok::Join || t == SqlTok::Comma || t == SqlTok::Update ||
            t == SqlTok::Into) {
            if (t == SqlTok::Comma) {
                // only a comma inside a FROM clause: look back for FROM at depth 0 before any other clause keyword
                bool in_from = false;
                int depth = 0;
                for (int b = i - 1; b >= first; b--) {
                    const SqlTok bt = type(b);
                    if (bt == SqlTok::RP) depth++;
                    if (bt == SqlTok::LP) {
                        if (depth == 0) break;
                        depth--;
                    }
                    if (depth) continue;
                    if (bt == SqlTok::From) {
                        in_from = true;
                        break;
                    }
                    if (bt == SqlTok::Select || bt == SqlTok::Where || bt == SqlTok::Group || bt == SqlTok::Order ||
                        bt == SqlTok::Set || bt == SqlTok::Values || bt == SqlTok::On)
                        break;
                }
                if (!in_from) continue;
            }
            int n = next_sig(i);
            if (!IsNameTok(p, n) || type(n) == SqlTok::Select || type(n) == SqlTok::LP) continue;
            if (type(n) == SqlTok::Or) continue;  // UPDATE OR ...
            std::string name = SqlDequote(p.Text(n));
            int k = next_sig(n);
            if (type(k) == SqlTok::Dot) {
                k = next_sig(k);
                if (!IsNameTok(p, k)) continue;
                name = SqlDequote(p.Text(k));
                k = next_sig(k);
            }
            std::string alias = name;
            if (type(k) == SqlTok::As) k = next_sig(k);
            if (k < end && type(k) == SqlTok::Id) alias = SqlDequote(p.Text(k));
            sc.sources.emplace_back(SqlLower(alias), name);
        }
    }
    return sc;
}

const char *const kStatementStarts[] = {
    "SELECT", "INSERT INTO", "UPDATE", "DELETE FROM", "WITH", "CREATE TABLE", "CREATE VIEW", "CREATE INDEX",
    "CREATE UNIQUE INDEX", "CREATE TRIGGER", "CREATE VIRTUAL TABLE", "DROP TABLE", "DROP VIEW", "DROP INDEX",
    "DROP TRIGGER", "ALTER TABLE", "PRAGMA", "BEGIN", "COMMIT", "ROLLBACK", "SAVEPOINT", "RELEASE", "ATTACH",
    "DETACH", "VACUUM", "REINDEX", "ANALYZE", "EXPLAIN", "EXPLAIN QUERY PLAN", "REPLACE INTO", "VALUES",
};

const char *const kClauseKeywords[] = {
    "FROM", "WHERE", "GROUP BY", "HAVING", "ORDER BY", "LIMIT", "OFFSET", "JOIN", "LEFT JOIN", "INNER JOIN",
    "CROSS JOIN", "NATURAL JOIN", "ON", "USING", "AS", "AND", "OR", "NOT", "IN", "IS", "IS NOT", "NULL", "LIKE",
    "GLOB", "BETWEEN", "EXISTS", "CASE", "WHEN", "THEN", "ELSE", "END", "DISTINCT", "UNION", "UNION ALL",
    "INTERSECT", "EXCEPT", "ASC", "DESC", "NULLS FIRST", "NULLS LAST", "CAST", "COLLATE", "ESCAPE", "OVER",
    "PARTITION BY", "FILTER", "WINDOW", "RETURNING", "SET", "VALUES", "DEFAULT VALUES", "ON CONFLICT", "DO NOTHING",
    "DO UPDATE SET", "TRUE", "FALSE", "CURRENT_DATE", "CURRENT_TIME", "CURRENT_TIMESTAMP",
};

const char *const kDdlKeywords[] = {
    "INTEGER", "TEXT", "REAL", "BLOB", "NUMERIC", "ANY", "PRIMARY KEY", "AUTOINCREMENT", "NOT NULL", "UNIQUE",
    "CHECK", "DEFAULT", "COLLATE", "REFERENCES", "FOREIGN KEY", "ON DELETE", "ON UPDATE", "CASCADE", "SET NULL",
    "RESTRICT", "NO ACTION", "CONSTRAINT", "GENERATED ALWAYS AS", "STORED", "VIRTUAL", "WITHOUT ROWID", "STRICT",
    "IF NOT EXISTS",
};

}  // namespace

std::vector<SqlLspCompletionItem> SqlLspCompletions(const std::vector<std::string> &lines, int line, int col,
                                                    const SqlLspContext &ctx) {
    std::vector<SqlLspCompletionItem> out;
    if (line < 0 || static_cast<size_t>(line) >= lines.size()) return out;
    const std::string &l = lines[static_cast<size_t>(line)];
    if (col > static_cast<int>(l.size())) col = static_cast<int>(l.size());
    int start = col;
    auto word_char = [](char c) {
        const unsigned char u = static_cast<unsigned char>(c);
        return u >= 0x80 || u == '_' || u == '$' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9');
    };
    while (start > 0 && word_char(l[static_cast<size_t>(start - 1)])) start--;
    const std::string prefix = SqlLower(l.substr(static_cast<size_t>(start), static_cast<size_t>(col - start)));

    SqlIndex idx = SqlBuildIndex(lines, ctx);
    const SqlParse &p = idx.parse;
    const int off = idx.lines.Offset(line, start);
    // Inside a comment or string: nothing.
    for (const SqlToken &t : p.tokens) {
        if (t.start < off && off < t.start + t.len && (t.type == SqlTok::Comment || t.type == SqlTok::String)) {
            return out;
        }
    }
    const int chunk = ChunkAt(p, off);
    int first = 0, end = static_cast<int>(p.tokens.size()) - 1;
    if (chunk >= 0) {
        first = p.chunks[static_cast<size_t>(chunk)].first_tok;
        end = p.chunks[static_cast<size_t>(chunk)].end_tok;
    }
    // The significant tokens before the word being typed.
    std::vector<int> before;
    for (int i = first; i < end; i++) {
        const SqlToken &t = p.tokens[static_cast<size_t>(i)];
        if (t.start >= off) break;
        if (t.type == SqlTok::Comment || t.type == SqlTok::Eof) continue;
        before.push_back(i);
    }
    auto tt = [&](size_t back) {
        return back < before.size() ? p.tokens[static_cast<size_t>(before[before.size() - 1 - back])].type
                                    : SqlTok::Eof;
    };
    const std::vector<SqlLspObject> objects = AllObjects(idx, ctx);
    const CompletionScope scope = ScanScope(p, first, end);
    std::set<std::string> seen;
    auto add = [&](const std::string &label, SqlLspKind kind, const std::string &detail, const std::string &doc,
                   const std::string &insert = std::string()) {
        if (!prefix.empty() && SqlLower(label).rfind(prefix, 0) != 0) return;
        if (!seen.insert(label + "\x01" + std::to_string(static_cast<int>(kind))).second) return;
        SqlLspCompletionItem it;
        it.label = label;
        it.insert_text = insert.empty() ? label : insert;
        it.kind = kind;
        it.detail = detail;
        it.documentation = doc;
        it.replace_start = start;
        it.replace_end = col;
        out.push_back(it);
    };
    auto table_columns = [&](const std::string &name, std::vector<std::pair<std::string, std::string>> *cols) {
        const std::string ln = SqlLower(name);
        for (const auto &c : scope.ctes) {
            if (SqlLower(c.first) == ln) {
                for (const std::string &n : c.second) cols->emplace_back(n, "");
                if (!c.second.empty()) return true;
            }
        }
        for (const SqlLspObject &o : objects) {
            if (SqlLower(o.name) != ln) continue;
            for (const SqlLspColumn &c : o.columns) cols->emplace_back(c.name, c.type);
            return true;
        }
        std::vector<std::string> fcols;
        if (SqlTableFunctionColumns(ln, &fcols)) {
            for (const std::string &n : fcols) cols->emplace_back(n, "");
            return true;
        }
        return false;
    };
    auto add_tables = [&]() {
        for (const auto &c : scope.ctes) add(c.first, SqlLspKind::Struct, "CTE", "");
        for (const SqlLspObject &o : objects) {
            if (o.kind == SqlLspObject::Index || o.kind == SqlLspObject::Trigger) continue;
            add(o.name, o.kind == SqlLspObject::View ? SqlLspKind::Interface : SqlLspKind::Class, KindName(o.kind),
                ColumnsBlock(o), QuoteName(o.name));
        }
    };

    // `qualifier.` : the columns of that alias / table / CTE.
    if (tt(0) == SqlTok::Dot && before.size() >= 2 && IsNameTok(p, before[before.size() - 2])) {
        const std::string q = SqlLower(SqlDequote(p.Text(before[before.size() - 2])));
        std::string table = q;
        for (const auto &s : scope.sources) {
            if (s.first == q) table = s.second;
        }
        if (q == "new" || q == "old" || q == "excluded") {
            for (int i = first; i < end; i++) {
                const SqlTok t = p.tokens[static_cast<size_t>(i)].type;
                if ((t == SqlTok::On || t == SqlTok::Into) && i + 1 < end && IsNameTok(p, i + 1)) {
                    table = SqlDequote(p.Text(i + 1));
                    break;
                }
            }
        }
        std::vector<std::pair<std::string, std::string>> cols;
        table_columns(table, &cols);
        for (const auto &c : cols) add(c.first, SqlLspKind::Field, c.second.empty() ? table : c.second + " — " + table, "", QuoteName(c.first));
        return out;
    }
    const SqlTok prev = tt(0);
    // Table positions.
    if (prev == SqlTok::From || prev == SqlTok::Join || prev == SqlTok::Into || prev == SqlTok::Update ||
        (prev == SqlTok::Table && tt(1) != SqlTok::Create && tt(1) != SqlTok::Temp && tt(1) != SqlTok::Virtual) ||
        (prev == SqlTok::View && tt(1) == SqlTok::Drop) || (prev == SqlTok::On && (tt(2) == SqlTok::Index || tt(3) == SqlTok::Index ||
                                                                                     tt(1) == SqlTok::Insert || tt(1) == SqlTok::Update ||
                                                                                     tt(1) == SqlTok::Delete || tt(1) == SqlTok::Of)) ||
        prev == SqlTok::References || (prev == SqlTok::Comma && [&] {
            for (size_t b = 0; b < before.size(); b++) {
                const SqlTok bt = tt(b);
                if (bt == SqlTok::From) return true;
                if (bt == SqlTok::Select || bt == SqlTok::Where || bt == SqlTok::LP || bt == SqlTok::RP ||
                    bt == SqlTok::Set || bt == SqlTok::On || bt == SqlTok::Group || bt == SqlTok::Order)
                    return false;
            }
            return false;
        }()) ||
        (prev == SqlTok::Exists && tt(1) == SqlTok::If && (tt(2) == SqlTok::Table || tt(2) == SqlTok::View))) {
        add_tables();
        if (prev == SqlTok::From || prev == SqlTok::Join || prev == SqlTok::Comma) {
            size_t n = 0;
            const SqlLspFunction *fs = SqlLspFunctionTable(&n);
            (void)fs;
            for (const char *f : {"json_each", "json_tree", "generate_series", "pragma_table_info"}) {
                add(f, SqlLspKind::Function, "table-valued function", "", std::string(f) + "(");
            }
        }
        return out;
    }
    if ((prev == SqlTok::Index || prev == SqlTok::Trigger || prev == SqlTok::View) && tt(1) == SqlTok::Drop) {
        for (const SqlLspObject &o : objects) {
            const bool want = (prev == SqlTok::Index && o.kind == SqlLspObject::Index) ||
                              (prev == SqlTok::Trigger && o.kind == SqlLspObject::Trigger) ||
                              (prev == SqlTok::View && o.kind == SqlLspObject::View);
            if (want) add(o.name, SqlLspKind::Reference, KindName(o.kind), "", QuoteName(o.name));
        }
        return out;
    }
    if (prev == SqlTok::Pragma || (prev == SqlTok::Dot && tt(2) == SqlTok::Pragma)) {
        size_t n = 0;
        const SqlLspPragma *ps = SqlLspPragmaTable(&n);
        for (size_t i = 0; i < n; i++) add(ps[i].name, SqlLspKind::Property, "PRAGMA", ps[i].doc);
        return out;
    }
    // A statement's start.
    if (before.empty() || prev == SqlTok::Semi || prev == SqlTok::Begin || prev == SqlTok::Explain ||
        prev == SqlTok::Plan) {
        for (const char *k : kStatementStarts) {
            const SqlLspKeywordDoc *d = SqlLspFindKeywordDoc(k);
            add(k, SqlLspKind::Keyword, "statement", d ? d->doc : "");
        }
        return out;
    }
    // INSERT INTO t ( ... : the target's columns. UPDATE t SET ...: likewise.
    {
        int depth = 0;
        for (size_t b = 0; b < before.size(); b++) {
            const SqlTok bt = tt(b);
            if (bt == SqlTok::RP) depth++;
            if (bt == SqlTok::LP) {
                if (depth == 0 && b + 1 < before.size() && IsNameTok(p, before[before.size() - 2 - b]) &&
                    b + 2 < before.size() && tt(b + 2) == SqlTok::Into) {
                    std::vector<std::pair<std::string, std::string>> cols;
                    const std::string table = SqlDequote(p.Text(before[before.size() - 2 - b]));
                    table_columns(table, &cols);
                    for (const auto &c : cols) add(c.first, SqlLspKind::Field, c.second, "", QuoteName(c.first));
                    return out;
                }
                if (depth > 0) depth--;
            }
        }
    }
    bool ddl = false;
    for (size_t b = 0; b < before.size(); b++) {
        if (tt(b) == SqlTok::Create && b + 1 <= before.size()) {
            ddl = tt(b == 0 ? 0 : b - 1) == SqlTok::Table || (b >= 2 && tt(b - 2) == SqlTok::Table);
        }
    }
    // An expression: the sources' columns, then functions and keywords.
    for (const auto &s : scope.sources) {
        std::vector<std::pair<std::string, std::string>> cols;
        table_columns(s.second, &cols);
        for (const auto &c : cols) {
            add(c.first, SqlLspKind::Field, (c.second.empty() ? "" : c.second + " — ") + s.second, "",
                QuoteName(c.first));
        }
        if (s.first != SqlLower(s.second)) add(s.first, SqlLspKind::Variable, "alias of " + s.second, "");
    }
    if (ddl) {
        for (const char *k : kDdlKeywords) add(k, SqlLspKind::Keyword, "", "");
    }
    size_t n = 0;
    const SqlLspFunction *fs = SqlLspFunctionTable(&n);
    for (size_t i = 0; i < n; i++) {
        std::string sig = fs[i].signatures;
        const size_t nl = sig.find('\n');
        if (nl != std::string::npos) sig = sig.substr(0, nl);
        add(fs[i].name, SqlLspKind::Function, sig, fs[i].doc, std::string(fs[i].name) + "(");
    }
    for (const char *k : kClauseKeywords) add(k, SqlLspKind::Keyword, "", "");
    if (scope.sources.empty()) add_tables();
    return out;
}

// --- Hover -------------------------------------------------------------------

SqlLspHoverInfo SqlLspHover(const std::vector<std::string> &lines, int line, int col, const SqlLspContext &ctx) {
    SqlLspHoverInfo h;
    SqlIndex idx = SqlBuildIndex(lines, ctx);
    const SqlParse &p = idx.parse;
    const int tok = idx.TokAt(line, col);
    if (tok < 0) return h;
    const SqlTok type = p.tokens[static_cast<size_t>(tok)].type;
    if (type == SqlTok::Comment || type == SqlTok::Eof) return h;
    h.range = idx.RangeOf(tok);
    for (const SqlIdxOcc &o : idx.occs) {
        if (o.tok != tok) continue;
        const SqlIdxTarget &t = idx.targets[static_cast<size_t>(o.target)];
        switch (t.kind) {
            case SqlIdxTarget::Object:
                h.text = ObjectHover(*t.object);
                break;
            case SqlIdxTarget::Column: {
                const SqlLspColumn &c = t.object->columns[static_cast<size_t>(t.column)];
                h.text = "**column** `" + t.table + "." + c.name + "`" + (c.type.empty() ? "" : " " + c.type) +
                         (c.primary_key ? " PRIMARY KEY" : "") + (c.not_null ? " NOT NULL" : "");
                if (!t.object->origin.empty()) h.text += "\n\nfrom " + t.object->origin;
                break;
            }
            case SqlIdxTarget::Cte:
            case SqlIdxTarget::Alias: {
                h.text = (t.kind == SqlIdxTarget::Cte ? "**CTE** `" : "**alias** `") + t.name + "`";
                if (t.kind == SqlIdxTarget::Alias) h.text += " for `" + t.table + "`";
                if (!t.columns.empty()) {
                    h.text += "\n\ncolumns: ";
                    for (size_t i = 0; i < t.columns.size(); i++) h.text += (i ? ", " : "") + t.columns[i];
                }
                break;
            }
            case SqlIdxTarget::CteColumn:
                h.text = "**column** `" + t.name + "` of CTE `" + t.table + "`";
                break;
            case SqlIdxTarget::ResultAlias:
                h.text = "**result column** `" + t.name + "`";
                break;
            case SqlIdxTarget::Window:
                h.text = "**window** `" + t.name + "`";
                break;
        }
        h.found = !h.text.empty();
        return h;
    }
    // A type name (column definition or CAST): SQLite's affinity rules. Type words are plain identifiers,
    // and one with a size looks like a function call (DECIMAL(10, 2)), so this comes first.
    if (type == SqlTok::Id || type == SqlTok::String) {
        for (const auto &span : p.type_spans) {
            if (tok < span.first || tok > span.second) continue;
            h.text = TypeHover(p, span.first, span.second);
            h.found = true;
            return h;
        }
    }
    // STRICT and the ROWID of WITHOUT ROWID are table options SQLite matches by name, not keywords.
    if (type == SqlTok::Id) {
        const std::string w = SqlLower(std::string(p.Text(tok)));
        for (const SqlStmt &st : p.stmts) {
            if (st.kind != SqlStmt::CreateTable || tok < st.first || tok > st.last) continue;
            int body_end = st.first;
            if (!st.coldefs.empty()) body_end = std::max(body_end, st.coldefs.back().last);
            if (!st.constraints.empty()) body_end = std::max(body_end, st.constraints.back().last);
            const SqlLspKeywordDoc *d = nullptr;
            if (tok > body_end && w == "strict" && st.strict) d = SqlLspFindKeywordDoc("strict");
            if (tok > body_end && w == "rowid" && st.without_rowid) d = SqlLspFindKeywordDoc("without rowid");
            if (d) {
                h.text = "```sql\n" + std::string(d->synopsis) + "\n```\n\n" + d->doc;
                h.found = true;
                return h;
            }
            break;
        }
    }
    // A function call?
    int next = tok + 1;
    while (static_cast<size_t>(next) < p.tokens.size() && p.tokens[static_cast<size_t>(next)].type == SqlTok::Comment) next++;
    const std::string word(p.Text(tok));
    if (IsIdent(type) && static_cast<size_t>(next) < p.tokens.size() &&
        p.tokens[static_cast<size_t>(next)].type == SqlTok::LP) {
        if (const SqlLspFunction *f = SqlLspFindFunction(word)) {
            h.text = FunctionHover(*f);
            h.found = true;
            return h;
        }
    }
    int prev = tok - 1;
    while (prev >= 0 && p.tokens[static_cast<size_t>(prev)].type == SqlTok::Comment) prev--;
    if (prev >= 0 && p.tokens[static_cast<size_t>(prev)].type == SqlTok::Pragma && IsIdent(type)) {
        if (const SqlLspPragma *pr = SqlLspFindPragma(SqlLower(word))) {
            h.text = "```sql\n" + std::string(pr->signatures) + "\n```\n\n" + pr->doc;
            h.found = true;
            return h;
        }
    }
    if (SqlIsKeyword(type)) {
        auto skip = [&](int t, int step) {
            while (t >= 0 && static_cast<size_t>(t) < p.tokens.size() &&
                   p.tokens[static_cast<size_t>(t)].type == SqlTok::Comment)
                t += step;
            return t;
        };
        auto word_at = [&](int t) {
            return t >= 0 && static_cast<size_t>(t) < p.tokens.size() && SqlIsKeyword(p.tokens[static_cast<size_t>(t)].type)
                       ? SqlLower(std::string(p.Text(t)))
                       : std::string();
        };
        const std::string w = SqlLower(word);
        const int prev2 = prev >= 0 ? skip(prev - 1, -1) : -1;
        const int next2 = skip(next + 1, 1);
        const std::string before2 = word_at(prev2), before = word_at(prev), after = word_at(next),
                          after2 = word_at(next2);
        const SqlLspKeywordDoc *d = nullptr;
        // BEGIN/END of a trigger body and END of a CASE are not the transaction statements.
        if (w == "begin" || w == "end") {
            for (const SqlStmt &st : p.stmts) {
                if (tok < st.first || tok > st.last) continue;
                if (st.kind == SqlStmt::CreateTrigger) d = SqlLspFindKeywordDoc("create trigger");
                if (w == "end") {
                    int depth = 0;
                    for (int t = tok - 1; t >= st.first && !d; t--) {
                        const SqlTok tt = p.tokens[static_cast<size_t>(t)].type;
                        if (tt == SqlTok::End) depth++;
                        else if (tt == SqlTok::Case && depth-- == 0) d = SqlLspFindKeywordDoc("case");
                    }
                }
                break;
            }
        }
        // The longest phrase around the word that has an entry: three words, then two, then the word
        // alone ("IF NOT EXISTS", "NOT NULL", "PRIMARY KEY", "GROUP BY", ...). A phrase is only tried
        // when every word of it is a keyword.
        auto phrase = [&](std::initializer_list<const std::string *> words) -> const SqlLspKeywordDoc * {
            std::string s;
            for (const std::string *x : words) {
                if (x->empty()) return nullptr;
                s += (s.empty() ? "" : " ") + *x;
            }
            return SqlLspFindKeywordDoc(s);
        };
        if (!d) d = phrase({&before2, &before, &w});
        if (!d) d = phrase({&before, &w, &after});
        if (!d) d = phrase({&w, &after, &after2});
        if (!d) d = phrase({&before, &w});
        if (!d) d = phrase({&w, &after});
        if (!d) d = SqlLspFindKeywordDoc(w);
        if (d) {
            h.text = "```sql\n" + std::string(d->synopsis) + "\n```\n\n" + d->doc;
            h.found = true;
            return h;
        }
    }
    if (IsIdent(type)) {
        // A table name in a position the resolver did not see (a broken statement): look it up.
        const std::string ln = SqlLower(SqlDequote(word));
        for (const SqlLspObject &o : AllObjects(idx, ctx)) {
            if (SqlLower(o.name) == ln) {
                h.text = ObjectHover(o);
                h.found = true;
                return h;
            }
        }
    }
    return h;
}

// --- Signature help ------------------------------------------------------------

namespace {

std::vector<std::string> ParamsOf(const std::string &label) {
    std::vector<std::string> out;
    const size_t lp = label.find('(');
    const size_t rp = label.rfind(')');
    if (lp == std::string::npos || rp == std::string::npos || rp <= lp + 1) return out;
    int depth = 0;
    size_t s = lp + 1;
    for (size_t i = lp + 1; i <= rp; i++) {
        const char c = label[i];
        if (c == '(' || c == '[') depth++;
        if ((c == ')' || c == ']') && i < rp) depth--;
        if ((c == ',' && depth == 0) || i == rp) {
            size_t a = s, b = i;
            while (a < b && label[a] == ' ') a++;
            while (b > a && label[b - 1] == ' ') b--;
            if (b > a) out.push_back(label.substr(a, b - a));
            s = i + 1;
        }
    }
    return out;
}

}  // namespace

SqlLspSignatureHelp SqlLspSignatureAt(const std::vector<std::string> &lines, int line, int col) {
    SqlLspSignatureHelp sh;
    const std::string text = SqlJoinLines(lines);
    SqlLineIndex li(text);
    const int off = li.Offset(line, col);
    std::vector<SqlToken> toks = SqlTokenize(std::string_view(text).substr(0, static_cast<size_t>(off)));
    std::vector<SqlToken> sig;
    for (const SqlToken &t : toks) {
        if (t.type != SqlTok::Space && t.type != SqlTok::Comment && t.type != SqlTok::Eof) sig.push_back(t);
    }
    int depth = 0, commas = 0;
    for (int i = static_cast<int>(sig.size()) - 1; i >= 0; i--) {
        const SqlTok t = sig[static_cast<size_t>(i)].type;
        if (t == SqlTok::Semi) return sh;
        if (t == SqlTok::RP) depth++;
        if (t == SqlTok::Comma && depth == 0) commas++;
        if (t == SqlTok::LP) {
            if (depth > 0) {
                depth--;
                continue;
            }
            if (i == 0 || !IsIdent(sig[static_cast<size_t>(i - 1)].type)) {
                commas = 0;  // a parenthesized list, not a call: keep looking outwards
                continue;
            }
            const SqlToken &n = sig[static_cast<size_t>(i - 1)];
            const std::string name = text.substr(static_cast<size_t>(n.start), static_cast<size_t>(n.len));
            const SqlLspFunction *f = SqlLspFindFunction(name);
            if (!f) {
                commas = 0;
                continue;
            }
            std::string sigs = f->signatures;
            size_t s = 0;
            while (s <= sigs.size()) {
                size_t e = sigs.find('\n', s);
                if (e == std::string::npos) e = sigs.size();
                SqlLspSignature one;
                one.label = sigs.substr(s, e - s);
                one.params = ParamsOf(one.label);
                if (!one.label.empty()) sh.signatures.push_back(one);
                s = e + 1;
            }
            sh.documentation = f->doc;
            sh.active_param = commas;
            sh.active_signature = 0;
            for (size_t k = 0; k < sh.signatures.size(); k++) {
                const auto &ps = sh.signatures[k].params;
                const bool variadic = !ps.empty() && ps.back().find("...") != std::string::npos;
                if (static_cast<int>(ps.size()) > commas || variadic) {
                    sh.active_signature = static_cast<int>(k);
                    break;
                }
            }
            sh.found = !sh.signatures.empty();
            return sh;
        }
    }
    return sh;
}

// --- Navigation --------------------------------------------------------------

namespace {

const SqlIdxTarget *TargetAt(const SqlIndex &idx, int line, int col) {
    const int o = idx.OccAt(line, col);
    if (o < 0) return nullptr;
    return &idx.targets[static_cast<size_t>(idx.occs[static_cast<size_t>(o)].target)];
}

}  // namespace

std::vector<SqlLspLocation> SqlLspDefinition(const std::vector<std::string> &lines, int line, int col,
                                             const SqlLspContext &ctx) {
    std::vector<SqlLspLocation> out;
    SqlIndex idx = SqlBuildIndex(lines, ctx);
    const SqlIdxTarget *t = TargetAt(idx, line, col);
    if (!t) return out;
    if (t->decl_tok >= 0) {
        SqlLspLocation loc;
        loc.range = idx.RangeOf(t->decl_tok);
        out.push_back(loc);
        return out;
    }
    if (t->kind == SqlIdxTarget::Object && t->object && t->object->has_range && !t->object->uri.empty()) {
        out.push_back({t->object->uri, t->object->range});
    } else if (t->kind == SqlIdxTarget::Column && t->object) {
        const SqlLspColumn &c = t->object->columns[static_cast<size_t>(t->column)];
        if (c.has_range && !c.uri.empty()) {
            out.push_back({c.uri, c.range});
        } else if (t->object->has_range && !t->object->uri.empty()) {
            out.push_back({t->object->uri, t->object->range});
        }
    }
    return out;
}

std::vector<SqlLspReference> SqlLspReferences(const std::vector<std::string> &lines, int line, int col,
                                              const SqlLspContext &ctx) {
    std::vector<SqlLspReference> out;
    SqlIndex idx = SqlBuildIndex(lines, ctx);
    const int o = idx.OccAt(line, col);
    if (o < 0) return out;
    const int target = idx.occs[static_cast<size_t>(o)].target;
    const std::string &key = idx.targets[static_cast<size_t>(target)].key;
    std::set<int> toks;
    for (const SqlIdxOcc &oc : idx.occs) {
        if (idx.targets[static_cast<size_t>(oc.target)].key != key || !toks.insert(oc.tok).second) continue;
        SqlLspReference r;
        r.range = idx.RangeOf(oc.tok);
        r.is_definition = oc.is_def || oc.tok == idx.targets[static_cast<size_t>(target)].decl_tok;
        r.is_write = oc.is_write;
        out.push_back(r);
    }
    const int decl = idx.targets[static_cast<size_t>(target)].decl_tok;
    if (decl >= 0 && !toks.count(decl)) {
        SqlLspReference r;
        r.range = idx.RangeOf(decl);
        r.is_definition = true;
        out.push_back(r);
    }
    std::sort(out.begin(), out.end(), [](const SqlLspReference &a, const SqlLspReference &b) {
        return a.range.line != b.range.line ? a.range.line < b.range.line : a.range.col < b.range.col;
    });
    return out;
}

namespace {

/** @brief Why the target cannot be renamed here ("" when it can). */
std::string RenameProblem(const SqlIndex &idx, const SqlIdxTarget &t) {
    if (t.decl_tok < 0) {
        if (t.object && !t.object->origin.empty()) return "`" + t.name + "` is declared in " + t.object->origin;
        return "`" + t.name + "` is not declared in this document";
    }
    if (t.kind == SqlIdxTarget::CteColumn) {
        for (const SqlIdxOcc &o : idx.occs) {
            if (o.tok == t.decl_tok && idx.targets[static_cast<size_t>(o.target)].key != t.key &&
                idx.targets[static_cast<size_t>(o.target)].kind != SqlIdxTarget::ResultAlias) {
                return "`" + t.name + "` is a column the CTE selects from `" +
                       idx.targets[static_cast<size_t>(o.target)].table + "`: give it an alias to rename it";
            }
        }
    }
    return std::string();
}

}  // namespace

SqlLspRenameTarget SqlLspPrepareRename(const std::vector<std::string> &lines, int line, int col,
                                       const SqlLspContext &ctx) {
    SqlLspRenameTarget r;
    SqlIndex idx = SqlBuildIndex(lines, ctx);
    const int o = idx.OccAt(line, col);
    if (o < 0) return r;
    const SqlIdxTarget &t = idx.targets[static_cast<size_t>(idx.occs[static_cast<size_t>(o)].target)];
    if (!RenameProblem(idx, t).empty()) return r;
    r.found = true;
    r.range = idx.RangeOf(idx.occs[static_cast<size_t>(o)].tok);
    r.placeholder = t.name;
    return r;
}

std::vector<SqlLspTextEdit> SqlLspRename(const std::vector<std::string> &lines, int line, int col,
                                         const std::string &new_name, std::string *error, const SqlLspContext &ctx) {
    std::vector<SqlLspTextEdit> edits;
    SqlIndex idx = SqlBuildIndex(lines, ctx);
    const int o = idx.OccAt(line, col);
    if (o < 0) {
        if (error) *error = "no table, column, CTE, alias or window here";
        return edits;
    }
    const int target = idx.occs[static_cast<size_t>(o)].target;
    const SqlIdxTarget &t = idx.targets[static_cast<size_t>(target)];
    const std::string problem = RenameProblem(idx, t);
    if (!problem.empty()) {
        if (error) *error = problem;
        return edits;
    }
    if (new_name.empty()) {
        if (error) *error = "the new name is empty";
        return edits;
    }
    const std::string text = QuoteName(new_name);
    std::set<int> toks;
    for (const SqlIdxOcc &oc : idx.occs) {
        if (idx.targets[static_cast<size_t>(oc.target)].key == t.key) toks.insert(oc.tok);
    }
    toks.insert(t.decl_tok);
    for (int tok : toks) {
        SqlLspTextEdit e;
        e.range = idx.RangeOf(tok);
        e.new_text = text;
        edits.push_back(e);
    }
    return edits;
}

// --- Outline / folding --------------------------------------------------------

std::vector<SqlLspSymbol> SqlLspSymbols(const std::vector<std::string> &lines) {
    std::vector<SqlLspSymbol> out;
    SqlParse p = SqlParseScript(SqlBlankDotCommands(SqlJoinLines(lines)));
    SqlLineIndex li(p.text);
    auto range = [&](int a, int b) {
        SqlLspRange r;
        const SqlToken &ta = p.tokens[static_cast<size_t>(a)];
        const SqlToken &tb = p.tokens[static_cast<size_t>(b)];
        r.line = li.Line(ta.start);
        r.col = li.Col(ta.start);
        r.end_line = li.Line(tb.start + tb.len);
        r.end_col = li.Col(tb.start + tb.len);
        return r;
    };
    for (const SqlStmt &st : p.stmts) {
        if (st.kind == SqlStmt::Empty || st.first < 0) continue;
        SqlLspSymbol s;
        s.range = range(st.first, std::max(st.first, st.last));
        int sel = st.name.ok() ? st.name.tok : st.first;
        switch (st.kind) {
            case SqlStmt::CreateTable: s.kind = SqlLspSymbolKind::Struct; s.detail = "table"; break;
            case SqlStmt::CreateVirtualTable:
                s.kind = SqlLspSymbolKind::Struct;
                s.detail = "virtual table using " + st.module.text;
                break;
            case SqlStmt::CreateView: s.kind = SqlLspSymbolKind::Interface; s.detail = "view"; break;
            case SqlStmt::CreateIndex: s.kind = SqlLspSymbolKind::Key; s.detail = "index on " + st.on_table.text; break;
            case SqlStmt::CreateTrigger: s.kind = SqlLspSymbolKind::Event; s.detail = "trigger on " + st.on_table.text; break;
            case SqlStmt::Select: s.kind = SqlLspSymbolKind::Function; break;
            case SqlStmt::Insert: case SqlStmt::Update: case SqlStmt::Delete: s.kind = SqlLspSymbolKind::Method; break;
            default: s.kind = SqlLspSymbolKind::Key; break;
        }
        const bool ddl = st.kind == SqlStmt::CreateTable || st.kind == SqlStmt::CreateVirtualTable ||
                         st.kind == SqlStmt::CreateView || st.kind == SqlStmt::CreateIndex ||
                         st.kind == SqlStmt::CreateTrigger;
        if (ddl && st.name.ok()) {
            s.name = st.name.text;
        } else {
            // "SELECT ... FROM t", "INSERT INTO t", "PRAGMA foreign_keys", "DROP TABLE t" ...
            std::string head;
            int words = 0;
            for (int t = st.first; t <= st.last && words < 4; t++) {
                const SqlTok ty = p.tokens[static_cast<size_t>(t)].type;
                if (ty == SqlTok::Comment) continue;
                if (ty == SqlTok::Semi || ty == SqlTok::LP) break;
                if (!head.empty()) head += ' ';
                head += SqlIsKeyword(ty) ? p.Upper(t) : std::string(p.Text(t));
                words++;
                if (st.kind == SqlStmt::Select || st.kind == SqlStmt::Insert) break;
            }
            s.name = head;
            if (st.kind == SqlStmt::Select && st.select && !st.select->cores.empty() &&
                !st.select->cores.front().from.empty()) {
                s.detail = "from " + st.select->cores.front().from.front().name.text;
            }
            if ((st.kind == SqlStmt::Insert || st.kind == SqlStmt::Update || st.kind == SqlStmt::Delete) &&
                st.name.ok()) {
                s.name = head + " " + st.name.text;
            }
            sel = st.first;
        }
        if (s.name.empty()) continue;
        s.sel_range = range(sel, sel);
        out.push_back(s);
        const int parent = static_cast<int>(out.size()) - 1;
        if (st.kind == SqlStmt::CreateTable) {
            for (const SqlColumnDef &cd : st.coldefs) {
                SqlLspSymbol c;
                c.name = cd.name.text;
                c.detail = cd.type;
                c.kind = SqlLspSymbolKind::Field;
                c.range = range(cd.first >= 0 ? cd.first : cd.name.tok, cd.last >= 0 ? cd.last : cd.name.tok);
                c.sel_range = range(cd.name.tok, cd.name.tok);
                c.parent = parent;
                out.push_back(c);
            }
        }
        const SqlWith *with = st.with ? st.with.get() : (st.select && st.select->with ? st.select->with.get() : nullptr);
        if (with) {
            for (const SqlCte &cte : with->ctes) {
                SqlLspSymbol c;
                c.name = cte.name.text;
                c.detail = "CTE";
                c.kind = SqlLspSymbolKind::Variable;
                c.range = range(cte.first >= 0 ? cte.first : cte.name.tok, cte.last >= 0 ? cte.last : cte.name.tok);
                c.sel_range = range(cte.name.tok, cte.name.tok);
                c.parent = parent;
                out.push_back(c);
            }
        }
    }
    return out;
}

std::vector<SqlLspFold> SqlLspFolds(const std::vector<std::string> &lines) {
    std::vector<SqlLspFold> out;
    SqlParse p = SqlParseScript(SqlBlankDotCommands(SqlJoinLines(lines)));
    SqlLineIndex li(p.text);
    auto line_of = [&](int tok, bool end) {
        const SqlToken &t = p.tokens[static_cast<size_t>(tok)];
        return li.Line(end ? t.start + t.len : t.start);
    };
    std::map<int, int> best;  // start line -> end line (the largest)
    std::map<int, std::string> kind;
    auto add = [&](int a, int b, const std::string &k) {
        if (b <= a) return;
        auto it = best.find(a);
        if (it == best.end() || it->second < b) {
            best[a] = b;
            kind[a] = k;
        }
    };
    for (const SqlChunk &c : p.chunks) {
        int first = c.first_tok, last = -1;
        for (int t = c.first_tok; t < c.end_tok; t++) {
            if (p.tokens[static_cast<size_t>(t)].type == SqlTok::Comment) {
                if (last < 0) first = t + 1;
                continue;
            }
            if (p.tokens[static_cast<size_t>(t)].type == SqlTok::Eof) break;
            last = t;
        }
        if (last >= first && first < c.end_tok) add(line_of(first, false), line_of(last, true), "");
        std::vector<int> stack;
        for (int t = c.first_tok; t < c.end_tok; t++) {
            const SqlTok ty = p.tokens[static_cast<size_t>(t)].type;
            if (ty == SqlTok::LP || ty == SqlTok::Begin) stack.push_back(t);
            if ((ty == SqlTok::RP || (ty == SqlTok::End && !stack.empty() &&
                                      p.tokens[static_cast<size_t>(stack.back())].type == SqlTok::Begin)) &&
                !stack.empty()) {
                add(line_of(stack.back(), false), line_of(t, true) - (ty == SqlTok::End ? 1 : 0), "");
                stack.pop_back();
            }
        }
    }
    // Comment runs: a multi-line block comment, or two or more line comments in a row.
    int run_start = -1, run_end = -1;
    for (const SqlToken &t : p.tokens) {
        if (t.type != SqlTok::Comment) continue;
        const int a = li.Line(t.start), b = li.Line(t.start + t.len - 1);
        if (p.text.compare(static_cast<size_t>(t.start), 2, "/*") == 0) {
            add(a, b, "comment");
            continue;
        }
        if (run_start >= 0 && a == run_end + 1) {
            run_end = a;
        } else {
            if (run_start >= 0) add(run_start, run_end, "comment");
            run_start = run_end = a;
        }
    }
    if (run_start >= 0) add(run_start, run_end, "comment");
    for (const auto &b : best) {
        SqlLspFold f;
        f.start_line = b.first;
        f.end_line = b.second;
        f.kind = kind[b.first];
        out.push_back(f);
    }
    return out;
}

// --- Formatting ----------------------------------------------------------------

std::vector<SqlLspTextEdit> SqlLspFormat(const std::vector<std::string> &lines, int indent_width, bool tabs,
                                         std::string *error) {
    std::vector<SqlLspTextEdit> edits;
    sqlfmt::Options opts;
    opts.indent_width = indent_width > 0 ? indent_width : 4;
    opts.use_tabs = tabs;
    const sqlfmt::Result r = sqlfmt::Format(SqlJoinLines(lines), opts);
    if (!r.ok) {
        if (error) {
            *error = r.error_line >= 0 ? "line " + std::to_string(r.error_line + 1) + ": " + r.error : r.error;
        }
        return edits;
    }
    std::vector<std::string> now;
    size_t s = 0;
    while (true) {
        const size_t e = r.text.find('\n', s);
        if (e == std::string::npos) {
            now.push_back(r.text.substr(s));
            break;
        }
        now.push_back(r.text.substr(s, e - s));
        s = e + 1;
    }
    // The changed span only: common leading and trailing lines stay.
    size_t a = 0;
    while (a < lines.size() && a < now.size() && lines[a] == now[a]) a++;
    if (a == lines.size() && a == now.size()) return edits;
    size_t bl = lines.size(), bn = now.size();
    while (bl > a && bn > a && lines[bl - 1] == now[bn - 1]) {
        bl--;
        bn--;
    }
    SqlLspTextEdit e;
    e.range.line = static_cast<int>(a);
    e.range.col = 0;
    std::string text;
    for (size_t i = a; i < bn; i++) text += now[i] + "\n";
    if (bl < lines.size()) {
        e.range.end_line = static_cast<int>(bl);
        e.range.end_col = 0;
    } else {
        // to the end of the document
        e.range.end_line = static_cast<int>(lines.size()) - 1;
        e.range.end_col = static_cast<int>(lines.back().size());
        if (!text.empty()) text.pop_back();
        if (bn == a && a > 0) {
            // only lines removed at the end: delete from the end of the last kept line
            e.range.line = static_cast<int>(a) - 1;
            e.range.col = static_cast<int>(lines[a - 1].size());
            text.clear();
        } else if (a > 0 && a == lines.size()) {
            // appending lines after the last one
            e.range.line = static_cast<int>(lines.size()) - 1;
            e.range.col = static_cast<int>(lines.back().size());
            text = "\n" + text;
        }
    }
    e.new_text = text;
    edits.push_back(e);
    return edits;
}
