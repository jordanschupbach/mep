// Windowless test for sql_lex.cpp, sql_parse.cpp and sql_lsp*.cpp -- the
// parser and analysis halves of mep's own SQL language server
// (`mep-sql-lsp`, src/sql_lsp_server.cpp). Drives the pure functions
// directly: no process, no JSON-RPC client, no SQLite. CHECK(), never
// assert(): the Release build strips assert().
//
// Conventions (bash_lsp_test.cpp's):
//   - Diagnostics are asserted by code; the syntax errors also by message,
//     because those are SQLite's own wording (what sqlite3 itself prints).
//   - Every lint is asserted absent on the idiomatic forms it must stay
//     quiet about, as deliberately as it is asserted present.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "sql_ast.h"
#include "sql_format.h"
#include "sql_lsp.h"

namespace {
int g_checks = 0;

void Check(bool ok, const char *expr, int line) {
    g_checks++;
    if (ok) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expr, __FILE__, line);
    std::abort();
}
#define CHECK(c) Check((c), #c, __LINE__)

using Lines = std::vector<std::string>;

Lines L(const std::string &text) {
    Lines out;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

std::vector<SqlLspDiagnostic> Diags(const std::string &text, const SqlLspContext &ctx = SqlLspContext()) {
    return SqlLspDiagnostics(L(text), ctx);
}

int Count(const std::vector<SqlLspDiagnostic> &ds, const std::string &code) {
    int n = 0;
    for (const SqlLspDiagnostic &d : ds) n += d.code == code ? 1 : 0;
    return n;
}

bool Has(const std::string &text, const std::string &code, const SqlLspContext &ctx = SqlLspContext()) {
    return Count(Diags(text, ctx), code) > 0;
}

void Dump(const std::string &text, const SqlLspContext &ctx = SqlLspContext()) {
    for (const SqlLspDiagnostic &d : Diags(text, ctx)) {
        std::fprintf(stderr, "  %d:%d %s %s\n", d.range.line, d.range.col, d.code.c_str(), d.message.c_str());
    }
}

bool Quiet(const std::string &text, const SqlLspContext &ctx = SqlLspContext()) {
    const auto ds = Diags(text, ctx);
    if (!ds.empty()) {
        std::fprintf(stderr, "expected no diagnostics for:\n%s\n", text.c_str());
        Dump(text, ctx);
    }
    return ds.empty();
}

bool HasLabel(const std::vector<SqlLspCompletionItem> &items, const std::string &label) {
    for (const auto &it : items) {
        if (it.label == label) return true;
    }
    return false;
}

const char *kSchema =
    "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT NULL, email TEXT);\n"
    "CREATE TABLE orders (id INTEGER PRIMARY KEY, user_id INTEGER REFERENCES users(id), total REAL);\n";

void TestParser() {
    // The three syntax errors, worded and placed as SQLite does.
    {
        SqlParse p = SqlParseScript("SELECT * FROM;");
        CHECK(p.errors.size() == 1);
        CHECK(p.errors[0].message == "near \";\": syntax error");
        CHECK(p.errors[0].offset == 13);
    }
    {
        SqlParse p = SqlParseScript("SELECT (1");
        CHECK(p.errors.size() == 1);
        CHECK(p.errors[0].message == "incomplete input");
        CHECK(p.errors[0].offset == -1);
    }
    {
        SqlParse p = SqlParseScript("SELECT 1 ^ 2;");
        CHECK(p.errors.size() == 1);
        CHECK(p.errors[0].message == "unrecognized token: \"^\"");
    }
    // One error per statement; the next statement starts fresh.
    {
        SqlParse p = SqlParseScript("SELECT FROM;\nSELECT 1;\nSELECT 2 2;\n");
        CHECK(p.chunks.size() == 3);
        CHECK(p.errors.size() == 2);
        CHECK(p.errors[1].chunk == 2);
    }
    // Fallback keywords are identifiers where an identifier fits.
    CHECK(SqlParseScript("CREATE TABLE key (replace TEXT, action INT); SELECT replace FROM key;").errors.empty());
    // ... and not where the keyword itself has a meaning.
    CHECK(!SqlParseScript("SELECT raise FROM t;").errors.empty());
    // A trigger's `;` does not end the statement.
    {
        SqlParse p = SqlParseScript("CREATE TRIGGER t AFTER INSERT ON a BEGIN SELECT 1; SELECT 2; END; SELECT 3;");
        CHECK(p.errors.empty());
        CHECK(p.chunks.size() == 2);
        CHECK(p.stmts[0].kind == SqlStmt::CreateTrigger);
        CHECK(p.stmts[0].body.size() == 2);
    }
    // WINDOW / OVER / FILTER are keywords only in their places.
    CHECK(SqlParseScript("SELECT window, over, filter FROM t;").errors.empty());
    CHECK(SqlParseScript("SELECT sum(x) FILTER (WHERE x > 0) OVER w FROM t WINDOW w AS (ORDER BY y);").errors.empty());
    // The shape of what it read.
    {
        SqlParse p = SqlParseScript("WITH c(x) AS (SELECT 1) SELECT a.x AS y FROM c AS a JOIN t USING (x) WHERE y > 0;");
        CHECK(p.errors.empty());
        const SqlStmt &st = p.stmts[0];
        CHECK(st.kind == SqlStmt::Select);
        CHECK(st.select->with->ctes.size() == 1);
        CHECK(st.select->with->ctes[0].columns.size() == 1);
        const SqlCore &core = st.select->cores[0];
        CHECK(core.from.size() == 2);
        CHECK(core.from[0].alias.text == "a");
        CHECK(core.from[1].using_cols.size() == 1);
        CHECK(core.cols[0].alias.text == "y");
        CHECK(core.cols[0].expr->kind == SqlExpr::Column);
        CHECK(core.cols[0].expr->table.text == "a");
    }
    // Dot-commands are blanked where no statement is pending, kept inside one.
    {
        const std::string s = SqlBlankDotCommands(".headers on\nSELECT 1;\n.mode column\n");
        CHECK(s == "           \nSELECT 1;\n            \n");
        const std::string inside = SqlBlankDotCommands("SELECT 1\n.5;\n");
        CHECK(inside == "SELECT 1\n.5;\n");
    }
}

void TestDiagnostics() {
    // Syntax errors carry SQLite's message.
    {
        const auto ds = Diags("SELECT * FROM;\n");
        CHECK(ds.size() == 1);
        CHECK(ds[0].code == "syntax-error");
        CHECK(ds[0].message == "near \";\": syntax error");
        CHECK(ds[0].range.col == 13);
    }
    CHECK(Has("SELECT 1 ^ 2;", "syntax-error"));
    // Idiomatic SQL is quiet.
    CHECK(Quiet(std::string(kSchema) +
                "SELECT u.name, count(*) AS n FROM users u JOIN orders o ON o.user_id = u.id GROUP BY u.name "
                "HAVING n > 1 ORDER BY n DESC;\n"
                "INSERT INTO users (name, email) VALUES ('a', 'b');\n"
                "INSERT INTO users VALUES (1, 'a', 'b');\n"
                "UPDATE orders SET total = total * 2 WHERE user_id IN (SELECT id FROM users WHERE name LIKE 'a%');\n"
                "DELETE FROM orders WHERE rowid = 3;\n"
                "WITH big AS (SELECT * FROM orders WHERE total > 10) SELECT big.id FROM big;\n"
                "SELECT x.value FROM json_each('[1,2]') AS x;\n"
                "SELECT name FROM sqlite_schema WHERE type = 'table';\n"
                "SELECT id FROM users WHERE email IS NULL;\n"
                "WITH RECURSIVE cnt(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM cnt WHERE x < 10) SELECT x FROM cnt;\n"
                "SELECT * FROM users NATURAL JOIN orders;\n"
                "SELECT id FROM users JOIN orders USING (id);\n"
                "INSERT INTO users (id, name) VALUES (1, 'x') ON CONFLICT (id) DO UPDATE SET name = excluded.name;\n"
                "CREATE TRIGGER trg AFTER UPDATE OF name ON users BEGIN UPDATE orders SET total = 0 "
                "WHERE user_id = new.id; END;\n"
                "SELECT sum(total) OVER w FROM orders WINDOW w AS (PARTITION BY user_id);\n"
                "PRAGMA foreign_keys = ON;\n"
                "SELECT TRUE, FALSE, CURRENT_TIMESTAMP;\n"));
    // Unknown names, once the document declares tables of its own.
    CHECK(Has(std::string(kSchema) + "SELECT * FROM userz;", "unknown-table"));
    CHECK(Has(std::string(kSchema) + "SELECT nme FROM users;", "unknown-column"));
    CHECK(Has(std::string(kSchema) + "SELECT u.nme FROM users u;", "unknown-column"));
    CHECK(Has(std::string(kSchema) + "INSERT INTO users (nme) VALUES (1);", "unknown-column"));
    CHECK(Has(std::string(kSchema) + "UPDATE users SET nme = 1;", "unknown-column"));
    // ... but not without any schema to go on (a file of queries against a database it does not name).
    CHECK(Quiet("SELECT a, b FROM somewhere WHERE c = 1;"));
    // ... nor through a table whose columns are unknown.
    CHECK(Quiet(std::string(kSchema) + "CREATE VIRTUAL TABLE v USING mymodule(x);\nSELECT anything FROM v;"));
    CHECK(Has(std::string(kSchema) + "SELECT id FROM users, orders;", "ambiguous-column"));
    CHECK(!Has(std::string(kSchema) + "SELECT id FROM users JOIN orders USING (id);", "ambiguous-column"));
    CHECK(Has("SELECT nosuchfn(1);", "unknown-function"));
    CHECK(Has("SELECT substr();", "wrong-arity"));
    CHECK(Has("SELECT abs(1, 2);", "wrong-arity"));
    CHECK(!Has("SELECT substr('abc', 2), substr('abc', 1, 2), count(*), max(1, 2, 3), printf('%d', 1, 2);",
               "wrong-arity"));
    CHECK(Has("SELECT 1 WHERE 1 = NULL;", "compare-null"));
    CHECK(Has("SELECT 1 WHERE NULL <> 1;", "compare-null"));
    CHECK(!Has("SELECT 1 WHERE 1 IS NULL;", "compare-null"));
    CHECK(Has("WITH a AS (SELECT 1) SELECT 2;", "unused-cte"));
    CHECK(Has("WITH a AS (SELECT 1), a AS (SELECT 2) SELECT * FROM a;", "duplicate-cte"));
    CHECK(Has(std::string(kSchema) + "SELECT 1 FROM users u JOIN orders u;", "duplicate-alias"));
    CHECK(Has(std::string(kSchema) + "INSERT INTO users VALUES (1, 'a');", "insert-count"));
    CHECK(Has(std::string(kSchema) + "INSERT INTO users (id, name) VALUES (1);", "insert-count"));
    CHECK(!Has(std::string(kSchema) + "INSERT INTO users (id, name) SELECT id, name FROM users;", "insert-count"));
    CHECK(Has("VALUES (1, 2), (3);", "values-count"));
    CHECK(Has("SELECT 1, 2 UNION SELECT 3;", "compound-column-count"));
    CHECK(Has("WITH c(a, b) AS (SELECT 1) SELECT * FROM c;", "cte-column-count"));
    CHECK(Has("CREATE TABLE t (a, b, a);", "duplicate-column"));
    CHECK(Has(std::string(kSchema) + "SELECT id FROM orders WHERE count(*) > 1;", "misused-aggregate"));
    CHECK(!Has(std::string(kSchema) + "SELECT id FROM orders WHERE total > (SELECT max(total) FROM orders);",
               "misused-aggregate"));
    CHECK(!Has(std::string(kSchema) + "SELECT id FROM orders WHERE max(total, 1) > 2;", "misused-aggregate"));
    CHECK(Has(std::string(kSchema) + "SELECT id FROM users WHERE name = \"bob\";", "double-quoted-string"));
    CHECK(Has("PRAGMA no_such_pragma;", "unknown-pragma"));
    CHECK(!Has("PRAGMA journal_mode = WAL; PRAGMA main.table_info(t);", "unknown-pragma"));
    CHECK(Has("SELECT sum(x) OVER nowin FROM (SELECT 1 AS x);", "unknown-window"));
    // Calibrated against SQLite on its own test suite (help/sql-lsp.org): each of these prepares fine there.
    CHECK(Quiet("CREATE TABLE t2 (a INTEGER);\nINSERT INTO t2 VALUES (1);\nALTER TABLE t2 ADD COLUMN b;\n"
                "INSERT INTO t2 VALUES (1, 2);"));
    CHECK(Quiet("CREATE TABLE t (a);\nINSERT INTO t VALUES (1);\nDROP TABLE t;\nCREATE TABLE t (a, b);\n"
                "INSERT INTO t VALUES (1, 2);"));
    CHECK(Quiet("SELECT sqlite_version(*);"));
    CHECK(Quiet("CREATE TABLE t1 (a, b);\nINSERT INTO t1 VALUES (1, 2);\nINSERT INTO t1 SELECT * FROM t1;\n"
                "ALTER TABLE t1 ADD COLUMN c;\nINSERT INTO t1 VALUES (1, 2, 3);"));
    CHECK(Quiet("CREATE TABLE t1 (a, b);\nINSERT INTO t1 VALUES (1, 2);\nALTER TABLE t1 RENAME COLUMN a TO c;\n"
                "INSERT INTO t1 VALUES (1, 2);\nALTER TABLE t1 DROP COLUMN b;\nINSERT INTO t1 VALUES (1);"));
    CHECK(Quiet("CREATE TABLE t1 (x AS (typeof(y)), y);\nINSERT INTO t1 VALUES (1);"));
    CHECK(Quiet("CREATE VIRTUAL TABLE fts USING fts3(tags TEXT);\nINSERT INTO fts (docid, tags) VALUES (1, 'x');\n"
                "SELECT count(*) FROM fts_segdir;\nSELECT fts FROM fts WHERE fts MATCH 'x';\n"
                "CREATE VIRTUAL TABLE t0 USING fts4(order=desc);\nINSERT INTO t0(rowid, content) VALUES (1, 'abc');\n"
                "CREATE VIRTUAL TABLE r USING rtree(id, x0, x1, +label);\nSELECT label FROM r;"));
    CHECK(Has("CREATE VIRTUAL TABLE fts USING fts5(a, b);\nSELECT c FROM fts;", "unknown-column"));
    CHECK(Quiet("CREATE VIRTUAL TABLE t2 USING fts3(xyz=abc);\nSELECT xyz FROM t2;\n"
                "CREATE VIRTUAL TABLE t4 USING fts4(a, languageid=l);\nSELECT l FROM t4;\nINSERT INTO t4 VALUES ('x');\n"
                "CREATE TABLE e (a, b);\nCREATE VIRTUAL TABLE t5 USING fts4(content=e);\nSELECT a FROM t5;"
                "CREATE VIRTUAL TABLE t7 USING fts4(languageid=lid);\nINSERT INTO t7(content, lid) VALUES ('a', 1);"));
    CHECK(Has("CREATE VIRTUAL TABLE t6 USING fts3(tokenize=simple);\nSELECT tokenize FROM t6;", "unknown-column"));
    CHECK(!Has("WITH x(a) AS (SELECT 1, 2) SELECT 10;", "cte-column-count"));
    CHECK(Quiet("CREATE TABLE t1 (a, b, c);\nSELECT a, b, a AS name FROM t1 INTERSECT SELECT a, b, c FROM t1 "
                "ORDER BY name;"));
    CHECK(Quiet("CREATE TABLE t1 (id1, value1);\nCREATE TABLE t2 (value2);\n"
                "SELECT max(value1), (SELECT count(*) FROM t2 WHERE value2 = max(value1)) FROM t1 GROUP BY id1;"));
    // A table created further down is still a table (scripts are not executed top to bottom here)...
    CHECK(Quiet("SELECT a FROM later;\nCREATE TABLE later (a);"));
    // ... and a CREATE cut short by a syntax error still declares its name.
    CHECK(!Has("CREATE TABLE t (a INTEGER,);\nCREATE TABLE u (b);\nSELECT * FROM t;", "unknown-table"));
    // Dot-commands of the sqlite3 shell are not SQL.
    CHECK(Quiet(".headers on\n.mode column\nSELECT 1;\n"));
}

void TestDatabaseContext() {
    // A database's schema (what the server reads with sqlite3) makes unknown names errors.
    std::vector<std::vector<std::string>> rows = {
        {"table", "items", "items", "CREATE TABLE items (id INTEGER PRIMARY KEY, label TEXT, price REAL)"},
        {"index", "items_label", "items", "CREATE INDEX items_label ON items(label)"},
        {"index", "sqlite_autoindex_x_1", "x", ""},
        {"view", "cheap", "cheap", "CREATE VIEW cheap AS SELECT id, label FROM items WHERE price < 1"},
    };
    SqlLspContext ctx;
    ctx.database = SqlLspSchemaObjects(rows, "shop.db");
    ctx.database_loaded = true;
    CHECK(ctx.database.size() == 3);
    CHECK(Quiet("-- database: shop.db\nSELECT label, price FROM items;\nSELECT label FROM cheap;\n", ctx));
    {
        const auto ds = Diags("SELECT colour FROM items;\nSELECT * FROM nowhere;\n", ctx);
        CHECK(Count(ds, "unknown-column") == 1);
        CHECK(Count(ds, "unknown-table") == 1);
        for (const auto &d : ds) CHECK(d.severity == SqlLspSeverity::Error);
    }
    // Without the database, the same mistakes are warnings (the document's own DDL might be incomplete).
    {
        const auto ds = Diags("CREATE TABLE items (id, label);\nSELECT colour FROM items;\n");
        CHECK(Count(ds, "unknown-column") == 1);
        CHECK(ds[0].severity == SqlLspSeverity::Warning);
    }
    // A header naming an unreadable database: an Information on the header, nothing else.
    {
        SqlLspContext bad;
        bad.database_problem = "database x.db does not exist";
        const auto ds = Diags("-- database: x.db\nSELECT a FROM b;\n", bad);
        CHECK(ds.size() == 1);
        CHECK(ds[0].code == "database-unavailable");
        CHECK(ds[0].severity == SqlLspSeverity::Information);
        CHECK(ds[0].range.line == 0);
    }
    {
        const SqlLspDatabaseHeader h = SqlLspFindDatabaseHeader(L("-- A script\n\n--  database:  ../data/app.db \nSELECT 1;"));
        CHECK(h.found);
        CHECK(h.path == "../data/app.db");
        CHECK(h.line == 2);
        CHECK(!SqlLspFindDatabaseHeader(L("SELECT 1;\n-- database: late.db")).found);
    }
    // Workspace files' objects resolve too.
    {
        SqlLspContext ws;
        ws.workspace = SqlLspScriptObjects("CREATE TABLE remote (a, b);", "file:///w/schema.sql");
        CHECK(Quiet("SELECT a, b FROM remote;", ws));
        const auto defs = SqlLspDefinition(L("SELECT a FROM remote;"), 0, 15, ws);
        CHECK(defs.size() == 1);
        CHECK(defs[0].uri == "file:///w/schema.sql");
        CHECK(defs[0].range.col == 13);
    }
}

void TestCompletion() {
    const std::string doc = std::string(kSchema) + "SELECT u. FROM users u;\nSELECT  FROM orders;\nSELECT * FROM ";
    const Lines lines = L(doc);
    {
        const auto items = SqlLspCompletions(lines, 2, 9);
        CHECK(HasLabel(items, "name"));
        CHECK(HasLabel(items, "email"));
        CHECK(!HasLabel(items, "total"));
        CHECK(!HasLabel(items, "SELECT"));
    }
    {
        const auto items = SqlLspCompletions(lines, 3, 7);
        CHECK(HasLabel(items, "total"));
        CHECK(HasLabel(items, "user_id"));
        CHECK(HasLabel(items, "count"));
        CHECK(HasLabel(items, "FROM"));
    }
    {
        const auto items = SqlLspCompletions(lines, 4, 14);
        CHECK(HasLabel(items, "users"));
        CHECK(HasLabel(items, "orders"));
        CHECK(!HasLabel(items, "count"));
    }
    {
        const auto items = SqlLspCompletions(L("sel"), 0, 3);
        CHECK(HasLabel(items, "SELECT"));
        CHECK(items[0].replace_start == 0);
    }
    {
        const auto items = SqlLspCompletions(L("PRAGMA fore"), 0, 11);
        CHECK(HasLabel(items, "foreign_keys"));
        CHECK(!HasLabel(items, "journal_mode"));
    }
    {
        const auto items = SqlLspCompletions(L(std::string(kSchema) + "INSERT INTO orders ("), 2, 20);
        CHECK(HasLabel(items, "total"));
    }
    // Nothing inside a comment or string.
    CHECK(SqlLspCompletions(L("-- sel"), 0, 6).empty());
}

void TestHoverAndSignature() {
    const Lines lines = L(std::string(kSchema) + "SELECT substr(name, 1) FROM users;");
    {
        const auto h = SqlLspHover(lines, 2, 8);
        CHECK(h.found);
        CHECK(h.text.find("substr(X,Y,Z)") != std::string::npos);
    }
    {
        const auto h = SqlLspHover(lines, 2, 15);
        CHECK(h.found);
        CHECK(h.text.find("users.name") != std::string::npos);
        CHECK(h.text.find("TEXT") != std::string::npos);
    }
    {
        const auto h = SqlLspHover(lines, 2, 30);
        CHECK(h.found);
        CHECK(h.text.find("CREATE TABLE users") != std::string::npos);
    }
    {
        const auto h = SqlLspHover(lines, 2, 1);
        CHECK(h.found);
        CHECK(h.text.find("SELECT") != std::string::npos);
    }
    {
        const auto h = SqlLspHover(L("PRAGMA journal_mode;"), 0, 9);
        CHECK(h.found);
        CHECK(h.text.find("journal_mode") != std::string::npos);
    }
    {
        const auto s = SqlLspSignatureAt(L("SELECT substr('abc', "), 0, 21);
        CHECK(s.found);
        CHECK(s.active_param == 1);
        CHECK(!s.signatures.empty());
        CHECK(s.signatures[static_cast<size_t>(s.active_signature)].params.size() >= 2);
    }
    {
        const auto s = SqlLspSignatureAt(L("SELECT round(abs(x), "), 0, 21);
        CHECK(s.found);
        CHECK(s.signatures[0].label.rfind("round", 0) == 0);
        CHECK(s.active_param == 1);
    }
    CHECK(!SqlLspSignatureAt(L("SELECT (1, "), 0, 11).found);
}

// The hover text over the first occurrence of `word` (after `from`) in a one-line script.
std::string HoverOn(const std::string &script, const std::string &word, size_t from = 0) {
    const size_t at = script.find(word, from);
    CHECK(at != std::string::npos);
    const auto h = SqlLspHover(L(script), 0, static_cast<int>(at));
    return h.found ? h.text : std::string();
}

bool Mentions(const std::string &text, const std::string &needle) { return text.find(needle) != std::string::npos; }

void TestKeywordAndTypeHover() {
    const std::string t =
        "CREATE TABLE IF NOT EXISTS t (id INTEGER PRIMARY KEY AUTOINCREMENT, name VARCHAR(40) NOT NULL UNIQUE, "
        "score DOUBLE PRECISION DEFAULT NULL, w BLOB, n DECIMAL(10, 2) CHECK (n > 0), "
        "p INT REFERENCES t (id) ON DELETE CASCADE) STRICT;";
    // Types: the affinity rules, multi-word names and sizes, the rowid alias.
    CHECK(Mentions(HoverOn(t, "INTEGER"), "INTEGER affinity"));
    CHECK(Mentions(HoverOn(t, "INTEGER"), "alias for the rowid"));
    CHECK(Mentions(HoverOn(t, "VARCHAR"), "VARCHAR(40)"));
    CHECK(Mentions(HoverOn(t, "VARCHAR"), "TEXT affinity"));
    CHECK(Mentions(HoverOn(t, "PRECISION"), "DOUBLE PRECISION"));
    CHECK(Mentions(HoverOn(t, "PRECISION"), "REAL affinity"));
    CHECK(Mentions(HoverOn(t, "BLOB"), "BLOB affinity"));
    CHECK(Mentions(HoverOn(t, "DECIMAL"), "NUMERIC affinity"));
    CHECK(Mentions(HoverOn(t, "INT "), "INTEGER affinity"));
    CHECK(!Mentions(HoverOn(t, "INT "), "alias for the rowid"));
    CHECK(Mentions(HoverOn("SELECT CAST(x AS TEXT) FROM t;", "TEXT"), "TEXT affinity"));
    // Keywords, alone and in phrases, from either word of the phrase.
    CHECK(Mentions(HoverOn(t, "IF"), "IF NOT EXISTS"));
    CHECK(Mentions(HoverOn(t, "NOT"), "IF NOT EXISTS"));
    CHECK(Mentions(HoverOn(t, "EXISTS"), "IF NOT EXISTS"));
    CHECK(Mentions(HoverOn(t, "TABLE"), "Creates a table"));
    CHECK(Mentions(HoverOn(t, "PRIMARY"), "identify a row"));
    CHECK(Mentions(HoverOn(t, "KEY"), "identify a row"));
    CHECK(Mentions(HoverOn(t, "AUTOINCREMENT"), "sqlite_sequence"));
    const size_t nn = t.find("NOT NULL");
    CHECK(Mentions(HoverOn(t, "NOT", nn), "may never hold NULL"));
    CHECK(Mentions(HoverOn(t, "NULL", nn), "may never hold NULL"));
    CHECK(Mentions(HoverOn(t, "UNIQUE"), "same value"));
    CHECK(Mentions(HoverOn(t, "DEFAULT"), "INSERT does not give one"));
    CHECK(Mentions(HoverOn(t, "NULL,"), "missing value"));
    CHECK(Mentions(HoverOn(t, "CHECK"), "CHECK constraint"));
    CHECK(Mentions(HoverOn(t, "REFERENCES"), "foreign key"));
    CHECK(Mentions(HoverOn(t, "CASCADE"), "foreign key"));
    CHECK(Mentions(HoverOn(t, "STRICT"), "enforce its column types"));
    const std::string wr = "CREATE TABLE k (a TEXT PRIMARY KEY) WITHOUT ROWID;";
    CHECK(Mentions(HoverOn(wr, "WITHOUT"), "B-tree keyed"));
    CHECK(Mentions(HoverOn(wr, "ROWID"), "B-tree keyed"));
    CHECK(Mentions(HoverOn("SELECT CURRENT_TIMESTAMP;", "CURRENT_TIMESTAMP"), "current UTC time"));
    const std::string g = "CREATE TABLE b (c INT, p REAL GENERATED ALWAYS AS (c / 100.0) VIRTUAL);";
    CHECK(Mentions(HoverOn(g, "VIRTUAL"), "generated column"));
    CHECK(Mentions(HoverOn("CREATE VIRTUAL TABLE f USING fts5(x);", "VIRTUAL"), "implemented by a module"));
    CHECK(Mentions(HoverOn("INSERT INTO t VALUES (1) ON CONFLICT (a) DO UPDATE SET a = 2;", "SET"), "upsert"));
    CHECK(Mentions(HoverOn("UPDATE t SET a = 1;", "SET"), "Changes the columns"));
    const std::string q = "SELECT a FROM t WHERE a IS NOT NULL AND b NOT IN (1) OR c LIKE 'x%' "
                          "AND d BETWEEN 1 AND 2 AND NOT e;";
    CHECK(Mentions(HoverOn(q, "IS"), "Tests whether a value is NULL"));
    CHECK(Mentions(HoverOn(q, "NOT"), "Tests whether a value is NULL"));
    CHECK(Mentions(HoverOn(q, "NOT", q.find("b NOT")), "equals one of"));
    CHECK(Mentions(HoverOn(q, "IN "), "equals one of"));
    CHECK(Mentions(HoverOn(q, "AND"), "Logical operators"));
    CHECK(Mentions(HoverOn(q, "OR"), "Logical operators"));
    CHECK(Mentions(HoverOn(q, "LIKE"), "Pattern matching"));
    CHECK(Mentions(HoverOn(q, "BETWEEN"), "both ends"));
    CHECK(Mentions(HoverOn(q, "NOT e"), "Logical negation"));
    CHECK(Mentions(HoverOn(q, "FROM"), "reads rows from"));
    CHECK(Mentions(HoverOn("SELECT * FROM a LEFT JOIN b USING (id);", "LEFT"), "Combines the rows"));
    CHECK(Mentions(HoverOn("SELECT * FROM a WHERE NOT EXISTS (SELECT 1);", "EXISTS"), "at least one row"));
    // END closes a CASE here, and BEGIN/END a trigger body, not a transaction.
    CHECK(Mentions(HoverOn("SELECT CASE WHEN a THEN 1 END FROM t;", "END"), "conditional expression"));
    CHECK(Mentions(HoverOn("CREATE TRIGGER g AFTER INSERT ON t BEGIN SELECT 1; END;", "END"), "Creates a trigger"));
    CHECK(Mentions(HoverOn("END;", "END"), "Ends the current transaction"));
    CHECK(Mentions(HoverOn("INSERT OR REPLACE INTO t VALUES (1);", "REPLACE"), "conflict resolution"));
    // The doc table is binary-searched, so it has to stay sorted.
    size_t n = 0;
    const SqlLspKeywordDoc *docs = SqlLspKeywordDocTable(&n);
    for (size_t i = 1; i < n; i++) CHECK(std::string(docs[i - 1].name) < std::string(docs[i].name));
}

void TestNavigation() {
    const std::string doc = std::string(kSchema) +
                            "SELECT u.name, o.total FROM users u JOIN orders o ON o.user_id = u.id WHERE u.name = 'x';\n"
                            "WITH big AS (SELECT * FROM orders) SELECT big.total FROM big;";
    const Lines lines = L(doc);
    // Definition of a column, a table, an alias, a CTE.
    {
        const auto d = SqlLspDefinition(lines, 2, 9);  // u.name -> users.name
        CHECK(d.size() == 1);
        CHECK(d[0].range.line == 0);
        CHECK(d[0].range.col == 44);
    }
    {
        const auto d = SqlLspDefinition(lines, 2, 30);  // users
        CHECK(d.size() == 1);
        CHECK(d[0].range.line == 0);
        CHECK(d[0].range.col == 13);
    }
    {
        const auto d = SqlLspDefinition(lines, 2, 7);  // u -> the alias
        CHECK(d.size() == 1);
        CHECK(d[0].range.line == 2);
        CHECK(d[0].range.col == 34);
    }
    {
        const auto d = SqlLspDefinition(lines, 3, 57);  // big -> the CTE
        CHECK(d.size() == 1);
        CHECK(d[0].range.line == 3);
        CHECK(d[0].range.col == 5);
    }
    // References: the alias u, everywhere in its statement.
    {
        const auto refs = SqlLspReferences(lines, 2, 34);
        CHECK(refs.size() == 4);
    }
    // Rename a column: its declaration and every use, nothing else.
    {
        std::string err;
        const auto edits = SqlLspRename(lines, 2, 9, "full_name", &err);
        CHECK(err.empty());
        CHECK(edits.size() == 3);  // the declaration, u.name, u.name in WHERE
        for (const auto &e : edits) CHECK(e.new_text == "full_name");
    }
    // Rename to a keyword quotes it.
    {
        std::string err;
        const auto edits = SqlLspRename(lines, 2, 7, "order", &err);
        CHECK(err.empty());
        CHECK(!edits.empty());
        CHECK(edits[0].new_text == "\"order\"");
    }
    // A table declared elsewhere cannot be renamed from here.
    {
        SqlLspContext ws;
        ws.workspace = SqlLspScriptObjects("CREATE TABLE remote (a);", "file:///w/s.sql");
        std::string err;
        CHECK(!SqlLspPrepareRename(L("SELECT a FROM remote;"), 0, 15, ws).found);
        CHECK(SqlLspRename(L("SELECT a FROM remote;"), 0, 15, "x", &err, ws).empty());
        CHECK(!err.empty());
    }
    CHECK(SqlLspPrepareRename(lines, 2, 30).found);
}

void TestSymbolsAndFolds() {
    const Lines lines = L(std::string(kSchema) +
                          "-- one\n-- two\nCREATE VIEW v AS\n  SELECT id\n  FROM users;\n"
                          "CREATE INDEX i ON users (name);\nSELECT 1;\n");
    const auto syms = SqlLspSymbols(lines);
    int tables = 0, columns = 0;
    bool view = false, index = false;
    for (const auto &s : syms) {
        if (s.kind == SqlLspSymbolKind::Struct) tables++;
        if (s.kind == SqlLspSymbolKind::Field) columns++;
        if (s.name == "v") view = true;
        if (s.name == "i") index = true;
    }
    CHECK(tables == 2);
    CHECK(columns == 6);
    CHECK(view);
    CHECK(index);
    const auto folds = SqlLspFolds(lines);
    bool comment = false, stmt = false;
    for (const auto &f : folds) {
        if (f.kind == "comment" && f.start_line == 2 && f.end_line == 3) comment = true;
        if (f.kind.empty() && f.start_line == 4 && f.end_line == 6) stmt = true;
    }
    CHECK(comment);
    CHECK(stmt);
}

void TestDescribe() {
    {
        const std::string d = SqlLspDescribe("substr");
        CHECK(d.rfind("substr(X,Y,Z)", 0) == 0);
        CHECK(d.find("\n\n") != std::string::npos);
        size_t s = 0;
        while (s < d.size()) {
            size_t e = d.find('\n', s);
            if (e == std::string::npos) e = d.size();
            CHECK(e - s <= 78);
            s = e + 1;
        }
    }
    CHECK(!SqlLspDescribe("journal_mode").empty());
    CHECK(SqlLspDescribe("PRAGMA journal_mode") == SqlLspDescribe("journal_mode"));
    CHECK(SqlLspDescribe("SELECT").find("[WITH") != std::string::npos);
    CHECK(SqlLspDescribe("create table").rfind("CREATE", 0) == 0);
    CHECK(SqlLspDescribe("no_such_thing_at_all").empty());
    {
        std::vector<SqlLspObject> objs =
            SqlLspSchemaObjects({{"table", "items", "items", "CREATE TABLE items (id INTEGER PRIMARY KEY, label TEXT)"}},
                                "shop.db");
        CHECK(SqlLspDescribe("items", objs).rfind("table items", 0) == 0);
        CHECK(SqlLspDescribe("items.label", objs).find("TEXT") != std::string::npos);
        CHECK(SqlLspDescribe("label", objs).find("items.label") != std::string::npos);
    }
    CHECK(SqlLspWrap("aaa bbb ccc", 7) == "aaa bbb\nccc");
}

void TestFormatAdapter() {
    std::string err;
    const auto edits = SqlLspFormat(L("select a from t;"), 4, false, &err);
    CHECK(err.empty());
    CHECK(edits.size() == 1);
    CHECK(edits[0].new_text == "SELECT a\nFROM t;");
    CHECK(SqlLspFormat(L("SELECT a\nFROM t;"), 4, false, &err).empty());
    CHECK(SqlLspFormat(L("select from;"), 4, false, &err).empty());
    CHECK(err.find("syntax error") != std::string::npos);
}

void TestLintFormat() {
    SqlLspDiagnostic d;
    d.range.line = 2;
    d.range.col = 4;
    d.severity = SqlLspSeverity::Warning;
    d.code = "unknown-table";
    d.message = "no such table: t";
    CHECK(SqlLspFormatDiagnostic("a.sql", d) == "a.sql:3:5: warning: no such table: t [unknown-table]");
    CHECK(SqlLspDiagnosticFails(d));
    d.severity = SqlLspSeverity::Information;
    CHECK(!SqlLspDiagnosticFails(d));
}

}  // namespace

int main() {
    TestParser();
    TestDiagnostics();
    TestDatabaseContext();
    TestCompletion();
    TestHoverAndSignature();
    TestKeywordAndTypeHover();
    TestNavigation();
    TestSymbolsAndFolds();
    TestDescribe();
    TestFormatAdapter();
    TestLintFormat();
    std::printf("sql lsp tests passed (%d checks)\n", g_checks);
    return 0;
}
