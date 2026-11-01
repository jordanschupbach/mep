// mep-sql-lsp's hand-written vocabulary: statement and keyword docs, the
// lookups over the generated function/PRAGMA tables (sql_lsp_names.cpp), and
// `--describe`.
#include <algorithm>
#include <cstring>

#include "sql_lsp.h"
#include "sql_lsp_index.h"

namespace {

// Sorted by name (lower case, words separated by one space).
const SqlLspKeywordDoc kKeywordDocs[] = {
    {"alter table",
     "ALTER TABLE [schema.]table RENAME TO new_table\nALTER TABLE [schema.]table RENAME [COLUMN] column TO "
     "new_column\nALTER TABLE [schema.]table ADD [COLUMN] column-def\nALTER TABLE [schema.]table DROP [COLUMN] column",
     "Changes an existing table: renames it, renames or drops one of its columns, or adds a column at the end. "
     "SQLite does not support other alterations; rebuild the table (create a new one, copy the rows, drop the old "
     "one, rename) for those."},
    {"analyze", "ANALYZE [schema | table | index]",
     "Gathers statistics about tables and indexes into the sqlite_stat tables, which the query planner uses to "
     "choose between indexes."},
    {"attach", "ATTACH [DATABASE] expr AS schema",
     "Opens another database file on the same connection under the name schema; its tables are then reachable as "
     "schema.table."},
    {"begin", "BEGIN [DEFERRED | IMMEDIATE | EXCLUSIVE] [TRANSACTION]",
     "Starts a transaction, which lasts until COMMIT or ROLLBACK. DEFERRED (the default) takes locks when the "
     "database is first read or written; IMMEDIATE takes the write lock at once; EXCLUSIVE also keeps readers out "
     "in rollback-journal mode."},
    {"case", "CASE [base] WHEN expr THEN result [WHEN ...] [ELSE result] END",
     "A conditional expression: the result of the first WHEN that is true (or equal to base), else the ELSE result, "
     "else NULL."},
    {"cast", "CAST(expr AS type-name)",
     "Converts a value to the storage class the type name's affinity implies (INTEGER, REAL, TEXT, BLOB, NUMERIC)."},
    {"commit", "COMMIT [TRANSACTION]\nEND [TRANSACTION]", "Ends the current transaction, making its changes durable."},
    {"create index",
     "CREATE [UNIQUE] INDEX [IF NOT EXISTS] [schema.]index ON table (indexed-column, ...) [WHERE expr]",
     "Creates an index on columns or expressions of a table. A UNIQUE index rejects rows that duplicate a key; a "
     "WHERE clause makes a partial index over the rows it matches."},
    {"create table",
     "CREATE [TEMP] TABLE [IF NOT EXISTS] [schema.]table (column-def, ... [, table-constraint, ...]) "
     "[WITHOUT ROWID] [, STRICT]\nCREATE [TEMP] TABLE [IF NOT EXISTS] [schema.]table AS select",
     "Creates a table. Each column definition is a name, an optional type name and constraints (PRIMARY KEY, NOT "
     "NULL, UNIQUE, CHECK, DEFAULT, COLLATE, REFERENCES, GENERATED ALWAYS AS). STRICT tables enforce the declared "
     "types; WITHOUT ROWID tables are clustered on their primary key."},
    {"create trigger",
     "CREATE [TEMP] TRIGGER [IF NOT EXISTS] [schema.]trigger [BEFORE | AFTER | INSTEAD OF] {DELETE | INSERT | "
     "UPDATE [OF column, ...]} ON table [FOR EACH ROW] [WHEN expr] BEGIN stmt; ... END",
     "Creates a trigger: statements run whenever rows of the table are deleted, inserted or updated. Inside the "
     "body NEW.column and OLD.column name the row's values after and before the change."},
    {"create view", "CREATE [TEMP] VIEW [IF NOT EXISTS] [schema.]view [(column, ...)] AS select",
     "Creates a view: a named SELECT that can be queried like a table. A view is read-only unless INSTEAD OF "
     "triggers handle writes to it."},
    {"create virtual table", "CREATE VIRTUAL TABLE [IF NOT EXISTS] [schema.]table USING module [(argument, ...)]",
     "Creates a table implemented by a module (fts5, rtree, ...): the arguments are the module's to interpret, "
     "usually column names and options."},
    {"delete", "[WITH ...] DELETE FROM [schema.]table [WHERE expr] [RETURNING ...]",
     "Removes the rows of a table the WHERE clause matches (all of them without one)."},
    {"detach", "DETACH [DATABASE] schema", "Closes a database that ATTACH opened."},
    {"drop index", "DROP INDEX [IF EXISTS] [schema.]index", "Removes an index."},
    {"drop table", "DROP TABLE [IF EXISTS] [schema.]table",
     "Removes a table, with its indexes and triggers. IF EXISTS makes a missing table no error."},
    {"drop trigger", "DROP TRIGGER [IF EXISTS] [schema.]trigger", "Removes a trigger."},
    {"drop view", "DROP VIEW [IF EXISTS] [schema.]view", "Removes a view (not the tables it reads)."},
    {"explain", "EXPLAIN statement\nEXPLAIN QUERY PLAN statement",
     "Shows how SQLite would run a statement instead of running it: the virtual-machine program, or with QUERY "
     "PLAN the tables and indexes it would use."},
    {"group by", "SELECT ... GROUP BY expr, ... [HAVING expr]",
     "Collapses the rows that share the values of the GROUP BY expressions into one, over which aggregate functions "
     "(count, sum, ...) run; HAVING then filters the groups."},
    {"insert",
     "[WITH ...] INSERT [OR {ABORT | FAIL | IGNORE | REPLACE | ROLLBACK}] INTO [schema.]table [AS alias] [(column, "
     "...)] {VALUES (expr, ...), ... | select | DEFAULT VALUES} [upsert-clause] [RETURNING ...]\nREPLACE INTO ...",
     "Adds rows to a table. Without a column list the values go to the table's columns in order. An ON CONFLICT "
     "clause (an upsert) turns a uniqueness violation into an update or nothing."},
    {"join", "FROM a [NATURAL] {[INNER] | LEFT [OUTER] | RIGHT [OUTER] | FULL [OUTER] | CROSS} JOIN b [ON expr | "
             "USING (column, ...)]",
     "Combines the rows of two tables. ON keeps the pairs a condition holds for; USING and NATURAL join on equal "
     "columns of the same name; LEFT/RIGHT/FULL also keep unmatched rows, with NULLs for the other side."},
    {"on conflict", "INSERT ... ON CONFLICT [(column, ...) [WHERE expr]] DO {NOTHING | UPDATE SET column = expr, ... "
                    "[WHERE expr]}",
     "An upsert: when the inserted row would violate a uniqueness constraint, do nothing or update the existing "
     "row instead; `excluded.column` names the value that was being inserted."},
    {"order by", "... ORDER BY expr [COLLATE name] [ASC | DESC] [NULLS {FIRST | LAST}], ...",
     "Sorts the result rows. An expression may also be a result column's alias or its 1-based position."},
    {"pragma", "PRAGMA [schema.]name\nPRAGMA [schema.]name = value\nPRAGMA [schema.]name(value)",
     "Queries or changes SQLite's settings and internal state. A pragma SQLite does not know is silently ignored."},
    {"reindex", "REINDEX [collation | [schema.]table | [schema.]index]",
     "Rebuilds indexes from scratch (after a collation's definition changed, for example)."},
    {"release", "RELEASE [SAVEPOINT] name",
     "Ends a savepoint, keeping its changes as part of the enclosing transaction."},
    {"returning", "INSERT / UPDATE / DELETE ... RETURNING expr [AS alias], ... | *",
     "Makes a data-changing statement return a row for each row it changed, with the given expressions over the "
     "changed row."},
    {"rollback", "ROLLBACK [TRANSACTION] [TO [SAVEPOINT] name]",
     "Undoes the current transaction, or with TO the changes since a savepoint."},
    {"savepoint", "SAVEPOINT name",
     "Starts a named nested transaction that ROLLBACK TO can undo and RELEASE can end."},
    {"select",
     "[WITH [RECURSIVE] cte, ...] SELECT [DISTINCT | ALL] result-column, ... [FROM table-or-subquery [join ...]] "
     "[WHERE expr] [GROUP BY expr, ... [HAVING expr]] [WINDOW name AS (window-defn), ...] [{UNION [ALL] | "
     "INTERSECT | EXCEPT} select] [ORDER BY ordering-term, ...] [LIMIT expr [OFFSET expr]]\nVALUES (expr, ...), ...",
     "Queries the database: the result columns computed for each row the FROM clause produces and the WHERE clause "
     "keeps, optionally grouped, combined with other SELECTs, sorted and limited."},
    {"update",
     "[WITH ...] UPDATE [OR {ABORT | FAIL | IGNORE | REPLACE | ROLLBACK}] [schema.]table [AS alias] SET column = "
     "expr, ... [FROM ...] [WHERE expr] [RETURNING ...]",
     "Changes the columns of the rows the WHERE clause matches (all rows without one). A FROM clause joins other "
     "tables whose columns the SET and WHERE expressions may use."},
    {"vacuum", "VACUUM [schema] [INTO filename]",
     "Rebuilds the database file, reclaiming free pages; with INTO, writes a compacted copy to a new file instead."},
    {"values", "VALUES (expr, ...), ...",
     "A literal table: one row per parenthesized list. All rows must have the same number of terms; the columns "
     "are named column1, column2, ..."},
    {"where", "... WHERE expr", "Keeps the rows for which the expression is true (not false and not NULL)."},
    {"window", "SELECT ... WINDOW name AS ([base] [PARTITION BY ...] [ORDER BY ...] [frame]), ...",
     "Names a window definition that window functions' OVER clauses can refer to."},
    {"with", "WITH [RECURSIVE] name [(column, ...)] AS [[NOT] MATERIALIZED] (select), ... statement",
     "Common table expressions: named subqueries the statement can read like tables. A recursive CTE refers to "
     "itself, typically as `initial-select UNION ALL recursive-select`."},
};

int Cmp(const char *a, const std::string &b) { return std::strcmp(a, b.c_str()); }

std::string Trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
    return s.substr(a, b - a);
}

}  // namespace

const SqlLspKeywordDoc *SqlLspKeywordDocTable(size_t *count) {
    *count = sizeof(kKeywordDocs) / sizeof(kKeywordDocs[0]);
    return kKeywordDocs;
}

const SqlLspFunction *SqlLspFindFunction(const std::string &name) {
    const std::string l = SqlLower(name);
    size_t n = 0;
    const SqlLspFunction *t = SqlLspFunctionTable(&n);
    const SqlLspFunction *e = t + n;
    const SqlLspFunction *it =
        std::lower_bound(t, e, l, [](const SqlLspFunction &f, const std::string &k) { return Cmp(f.name, k) < 0; });
    return it != e && Cmp(it->name, l) == 0 ? it : nullptr;
}

const SqlLspPragma *SqlLspFindPragma(const std::string &name) {
    const std::string l = SqlLower(name);
    size_t n = 0;
    const SqlLspPragma *t = SqlLspPragmaTable(&n);
    const SqlLspPragma *e = t + n;
    const SqlLspPragma *it =
        std::lower_bound(t, e, l, [](const SqlLspPragma &f, const std::string &k) { return Cmp(f.name, k) < 0; });
    return it != e && Cmp(it->name, l) == 0 ? it : nullptr;
}

const SqlLspKeywordDoc *SqlLspFindKeywordDoc(const std::string &name) {
    // Normalize: lower case, runs of blanks to one space.
    std::string l;
    for (char c : SqlLower(Trim(name))) {
        if (c == ' ' || c == '\t' || c == '_') {
            if (!l.empty() && l.back() != ' ') l += ' ';
        } else {
            l += c;
        }
    }
    if (l == "end") l = "commit";
    if (l == "replace" || l == "replace into" || l == "insert into") l = "insert";
    if (l == "upsert") l = "on conflict";
    if (l == "delete from") l = "delete";
    size_t n = 0;
    const SqlLspKeywordDoc *t = SqlLspKeywordDocTable(&n);
    const SqlLspKeywordDoc *e = t + n;
    const SqlLspKeywordDoc *it =
        std::lower_bound(t, e, l, [](const SqlLspKeywordDoc &f, const std::string &k) { return Cmp(f.name, k) < 0; });
    return it != e && Cmp(it->name, l) == 0 ? it : nullptr;
}

bool SqlLspArityOk(const SqlLspFunction &f, int n) {
    if (n < f.min_args) return false;
    if (f.max_args < 0) return true;
    if (n > f.max_args) return false;
    if (!f.counts || !*f.counts) return true;
    const std::string want = std::to_string(n);
    const std::string counts = std::string(",") + f.counts + ",";
    return counts.find("," + want + ",") != std::string::npos;
}

std::string SqlLspWrap(const std::string &text, size_t width) {
    std::string out;
    size_t i = 0;
    while (i <= text.size()) {
        size_t j = text.find("\n\n", i);
        if (j == std::string::npos) j = text.size();
        const std::string para = text.substr(i, j - i);
        if (!out.empty()) out += "\n\n";
        size_t col = 0;
        size_t k = 0;
        while (k < para.size()) {
            while (k < para.size() && (para[k] == ' ' || para[k] == '\n')) k++;
            size_t w = k;
            while (w < para.size() && para[w] != ' ' && para[w] != '\n') w++;
            if (w == k) break;
            const size_t len = w - k;
            if (col > 0 && col + 1 + len > width) {
                out += '\n';
                col = 0;
            } else if (col > 0) {
                out += ' ';
                col++;
            }
            out.append(para, k, len);
            col += len;
            k = w;
        }
        i = j + 2;
    }
    return out;
}

namespace {

std::string ObjectKindName(const SqlLspObject &o) {
    switch (o.kind) {
        case SqlLspObject::Table: return "table";
        case SqlLspObject::View: return "view";
        case SqlLspObject::Index: return "index";
        case SqlLspObject::Trigger: return "trigger";
        case SqlLspObject::VirtualTable: return "virtual table";
    }
    return "table";
}

std::string DescribeObject(const SqlLspObject &o) {
    std::string out = ObjectKindName(o) + " " + o.name;
    if (!o.table.empty()) out += " ON " + o.table;
    out += "\n";
    for (const SqlLspColumn &c : o.columns) {
        out += "  " + c.name;
        if (!c.type.empty()) out += " " + c.type;
        if (c.primary_key) out += " PRIMARY KEY";
        if (c.not_null) out += " NOT NULL";
        out += "\n";
    }
    if (!o.sql.empty()) out += "\n" + o.sql + "\n";
    return out;
}

}  // namespace

std::string SqlLspDescribe(const std::string &raw, const std::vector<SqlLspObject> &objects) {
    std::string name = Trim(raw);
    while (!name.empty() && (name.back() == '(' || name.back() == ')')) name.pop_back();
    if (name.empty()) return std::string();
    if (const SqlLspFunction *f = SqlLspFindFunction(name)) {
        std::string out = f->signatures;
        out += "\n\n";
        std::string doc = f->doc;
        if (doc.empty()) doc = std::string("A built-in ") + f->kind + " function of SQLite.";
        out += SqlLspWrap(doc, 78) + "\n";
        return out;
    }
    std::string l = SqlLower(name);
    if (l.rfind("pragma ", 0) == 0) l = Trim(l.substr(7));
    if (const SqlLspPragma *p = SqlLspFindPragma(l)) {
        std::string out = p->signatures;
        out += "\n\n";
        out += SqlLspWrap(*p->doc ? p->doc : "A PRAGMA of SQLite.", 78) + "\n";
        return out;
    }
    if (const SqlLspKeywordDoc *k = SqlLspFindKeywordDoc(name)) {
        std::string out;
        // Signature lines wrap too: some are long.
        std::string syn = k->synopsis;
        size_t i = 0;
        while (i <= syn.size()) {
            size_t j = syn.find('\n', i);
            if (j == std::string::npos) j = syn.size();
            out += SqlLspWrap(syn.substr(i, j - i), 78) + "\n";
            i = j + 1;
        }
        out += "\n" + SqlLspWrap(k->doc, 78) + "\n";
        return out;
    }
    // Schema objects: "table", "table.column", or a column name some table has.
    const size_t dot = l.find('.');
    for (const SqlLspObject &o : objects) {
        if (dot == std::string::npos && SqlLower(o.name) == l) return DescribeObject(o);
    }
    std::string table = dot == std::string::npos ? std::string() : l.substr(0, dot);
    std::string column = dot == std::string::npos ? l : l.substr(dot + 1);
    std::string out;
    for (const SqlLspObject &o : objects) {
        if (!table.empty() && SqlLower(o.name) != table) continue;
        for (const SqlLspColumn &c : o.columns) {
            if (SqlLower(c.name) != column) continue;
            out += "column " + o.name + "." + c.name + (c.type.empty() ? "" : " " + c.type) +
                   (c.primary_key ? " PRIMARY KEY" : "") + (c.not_null ? " NOT NULL" : "") + "\n";
        }
    }
    if (!out.empty()) {
        out += "\nA column of the database's schema.\n";
        return out;
    }
    // Keywords without a page of their own: say what kind of word it is.
    if (SqlKeywordType(name) != SqlTok::Id) {
        std::string up = name;
        for (char &c : up) {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        }
        return up + "\n\nA keyword of SQLite's SQL.\n";
    }
    return std::string();
}
