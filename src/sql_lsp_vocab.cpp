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
    {"and",
     "expr AND expr\nexpr OR expr",
     "Logical operators. AND is true when both sides are true, OR when either is; with a NULL operand the result is "
     "NULL unless the other side settles it (NULL AND 0 is 0, NULL OR 1 is 1). AND binds tighter than OR."},
    {"as",
     "expr AS alias\ntable AS alias\nname AS (select)\nCAST(expr AS type)",
     "Gives a result column or a table a new name (the AS itself is optional there), names a CTE's or a view's query, "
     "or introduces the target type of a CAST."},
    {"attach", "ATTACH [DATABASE] expr AS schema",
     "Opens another database file on the same connection under the name schema; its tables are then reachable as "
     "schema.table."},
    {"autoincrement",
     "column INTEGER PRIMARY KEY AUTOINCREMENT",
     "Makes the rowid of an INTEGER PRIMARY KEY column strictly increasing: a value is never reused, even after the "
     "row holding the largest one is deleted. SQLite records the largest rowid handed out in the sqlite_sequence "
     "table. Without AUTOINCREMENT a new row gets one more than the current largest rowid, which is usually enough."},
    {"begin", "BEGIN [DEFERRED | IMMEDIATE | EXCLUSIVE] [TRANSACTION]",
     "Starts a transaction, which lasts until COMMIT or ROLLBACK. DEFERRED (the default) takes locks when the "
     "database is first read or written; IMMEDIATE takes the write lock at once; EXCLUSIVE also keeps readers out "
     "in rollback-journal mode."},
    {"between",
     "expr [NOT] BETWEEN low AND high",
     "True when expr >= low AND expr <= high: both ends are included. NOT BETWEEN is the opposite."},
    {"case", "CASE [base] WHEN expr THEN result [WHEN ...] [ELSE result] END",
     "A conditional expression: the result of the first WHEN that is true (or equal to base), else the ELSE result, "
     "else NULL."},
    {"cast", "CAST(expr AS type-name)",
     "Converts a value to the storage class the type name's affinity implies (INTEGER, REAL, TEXT, BLOB, NUMERIC)."},
    {"check",
     "CHECK (expr)  -- column or table constraint",
     "A constraint: every row inserted or updated must make the expression true or NULL, else the statement fails "
     "with a CHECK constraint error."},
    {"collate",
     "expr COLLATE name\ncolumn type COLLATE name",
     "Chooses how text is compared and sorted: BINARY (the default, byte by byte), NOCASE (ASCII letters "
     "case-insensitive) or RTRIM (trailing spaces ignored). On a column definition it sets the column's default "
     "collation."},
    {"commit", "COMMIT [TRANSACTION]\nEND [TRANSACTION]", "Ends the current transaction, making its changes durable."},
    {"constraint",
     "CONSTRAINT name constraint",
     "Names the constraint that follows (PRIMARY KEY, UNIQUE, CHECK, FOREIGN KEY, ...). The name appears in error "
     "messages; SQLite has no statement that drops a constraint by name."},
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
    {"current timestamp",
     "CURRENT_TIME\nCURRENT_DATE\nCURRENT_TIMESTAMP",
     "The current UTC time ('HH:MM:SS'), date ('YYYY-MM-DD') or both ('YYYY-MM-DD HH:MM:SS') as text, the same for "
     "every row of one statement. Often used as a column DEFAULT."},
    {"default",
     "column type DEFAULT {literal | (expr) | CURRENT_TIMESTAMP ...}\nINSERT INTO table DEFAULT VALUES",
     "The value a column takes when an INSERT does not give one; without a DEFAULT it is NULL. DEFAULT VALUES inserts "
     "one row made only of defaults."},
    {"delete", "[WITH ...] DELETE FROM [schema.]table [WHERE expr] [RETURNING ...]",
     "Removes the rows of a table the WHERE clause matches (all of them without one)."},
    {"detach", "DETACH [DATABASE] schema", "Closes a database that ATTACH opened."},
    {"distinct",
     "SELECT [DISTINCT | ALL] ...\naggregate(DISTINCT expr)",
     "DISTINCT removes duplicate result rows (NULLs count as equal to each other here); ALL, the default, keeps them. "
     "Inside an aggregate, DISTINCT makes it see each value once: count(DISTINCT x)."},
    {"drop index", "DROP INDEX [IF EXISTS] [schema.]index", "Removes an index."},
    {"drop table", "DROP TABLE [IF EXISTS] [schema.]table",
     "Removes a table, with its indexes and triggers. IF EXISTS makes a missing table no error."},
    {"drop trigger", "DROP TRIGGER [IF EXISTS] [schema.]trigger", "Removes a trigger."},
    {"drop view", "DROP VIEW [IF EXISTS] [schema.]view", "Removes a view (not the tables it reads)."},
    {"exists",
     "[NOT] EXISTS (select)",
     "True when the subquery returns at least one row, false when it returns none; the subquery's columns do not "
     "matter."},
    {"explain", "EXPLAIN statement\nEXPLAIN QUERY PLAN statement",
     "Shows how SQLite would run a statement instead of running it: the virtual-machine program, or with QUERY "
     "PLAN the tables and indexes it would use."},
    {"from",
     "SELECT ... FROM table-or-subquery [[AS] alias] [join ...]\nDELETE FROM table\nUPDATE ... FROM ...",
     "Names the tables, views, subqueries and table-valued functions a statement reads rows from. Several items "
     "separated by commas are a cross join."},
    {"generated always",
     "column type [GENERATED ALWAYS] AS (expr) [VIRTUAL | STORED]",
     "A generated column: its value is computed from the other columns of the same row and cannot be written. VIRTUAL "
     "(the default) computes it when read; STORED keeps it in the table, and only STORED columns cannot be added by "
     "ALTER TABLE."},
    {"glob",
     "expr [NOT] GLOB pattern",
     "Unix-style pattern matching, case-sensitive: * matches any run of characters, ? one character, [...] one of a "
     "set."},
    {"group by", "SELECT ... GROUP BY expr, ... [HAVING expr]",
     "Collapses the rows that share the values of the GROUP BY expressions into one, over which aggregate functions "
     "(count, sum, ...) run; HAVING then filters the groups."},
    {"if exists",
     "DROP {TABLE | VIEW | INDEX | TRIGGER} IF EXISTS name",
     "Makes the statement do nothing, instead of failing, when the object does not exist."},
    {"if not exists",
     "CREATE {TABLE | VIEW | INDEX | TRIGGER} IF NOT EXISTS name ...",
     "Makes the statement do nothing, instead of failing, when an object of that name already exists. The existing "
     "object is left as it is, even if its definition differs."},
    {"in",
     "expr [NOT] IN (value, ...)\nexpr [NOT] IN (select)\nexpr [NOT] IN table",
     "True when expr equals one of the values or one of the rows the subquery returns. With a NULL in the list and no "
     "match the result is NULL, not false -- which makes NOT IN with a NULL in the list never true."},
    {"insert",
     "[WITH ...] INSERT [OR {ABORT | FAIL | IGNORE | REPLACE | ROLLBACK}] INTO [schema.]table [AS alias] [(column, "
     "...)] {VALUES (expr, ...), ... | select | DEFAULT VALUES} [upsert-clause] [RETURNING ...]\nREPLACE INTO ...",
     "Adds rows to a table. Without a column list the values go to the table's columns in order. An ON CONFLICT "
     "clause (an upsert) turns a uniqueness violation into an update or nothing."},
    {"is",
     "expr IS [NOT] expr\nexpr IS [NOT] DISTINCT FROM expr",
     "Equality that treats NULL as a value: x IS y is true when both are NULL, and never NULL itself, where x = y "
     "would be NULL. IS NOT is its opposite; IS NOT DISTINCT FROM is a synonym of IS."},
    {"is null",
     "expr IS NULL\nexpr IS NOT NULL\nexpr ISNULL\nexpr NOTNULL",
     "Tests whether a value is NULL. Use these rather than = NULL, which is never true: any comparison with NULL is "
     "NULL."},
    {"join", "FROM a [NATURAL] {[INNER] | LEFT [OUTER] | RIGHT [OUTER] | FULL [OUTER] | CROSS} JOIN b [ON expr | "
             "USING (column, ...)]",
     "Combines the rows of two tables. ON keeps the pairs a condition holds for; USING and NATURAL join on equal "
     "columns of the same name; LEFT/RIGHT/FULL also keep unmatched rows, with NULLs for the other side."},
    {"like",
     "expr [NOT] LIKE pattern [ESCAPE char]",
     "Pattern matching: % matches any run of characters, _ one character. Case-insensitive for ASCII letters by "
     "default (PRAGMA case_sensitive_like changes that); ESCAPE names a character that makes the next % or _ literal."},
    {"limit",
     "... LIMIT count [OFFSET skip]\n... LIMIT skip, count",
     "Returns at most count rows (a negative count means no limit), after skipping the first skip rows. Without ORDER "
     "BY which rows those are is not defined. Note the reversed order of the comma form."},
    {"match",
     "expr MATCH pattern",
     "Full-text search on an FTS3/FTS4/FTS5 virtual table (and a few other modules). On an ordinary table there is no "
     "built-in match() function and it is an error."},
    {"not",
     "NOT expr",
     "Logical negation: true becomes false and false becomes true; NOT NULL is NULL. Also combines with IN, LIKE, "
     "GLOB, BETWEEN, EXISTS and IS (x NOT IN ..., x IS NOT NULL)."},
    {"not null",
     "column type NOT NULL [ON CONFLICT ...]",
     "A column constraint: the column may never hold NULL, so an INSERT or UPDATE that would put one there fails (or, "
     "with ON CONFLICT, resolves as that says). A column without it accepts NULL, even one that is part of a PRIMARY "
     "KEY, except INTEGER PRIMARY KEY and in WITHOUT ROWID or STRICT tables."},
    {"null",
     "NULL",
     "The missing value. Any arithmetic or comparison involving NULL gives NULL, so test for it with IS NULL / IS NOT "
     "NULL; aggregates other than count(*) skip NULLs. As a column constraint, NULL is a no-op that only states the "
     "column may hold it."},
    {"on",
     "... JOIN table ON expr\nCREATE INDEX name ON table (...)\nCREATE TRIGGER ... ON table",
     "In a join, the condition a pair of rows must satisfy to be combined. In CREATE INDEX and CREATE TRIGGER it "
     "names the table the index or trigger belongs to."},
    {"on conflict", "INSERT ... ON CONFLICT [(column, ...) [WHERE expr]] DO {NOTHING | UPDATE SET column = expr, ... "
                    "[WHERE expr]}",
     "An upsert: when the inserted row would violate a uniqueness constraint, do nothing or update the existing "
     "row instead; `excluded.column` names the value that was being inserted."},
    {"or ignore",
     "INSERT OR {ABORT | FAIL | IGNORE | REPLACE | ROLLBACK} ...\nUPDATE OR {...} ...",
     "The conflict resolution algorithm for a constraint violation. ABORT (the default) undoes the statement; FAIL "
     "stops it but keeps the rows already changed; IGNORE skips the offending row; REPLACE deletes the rows in the "
     "way and goes on (not for NOT NULL without a default, nor CHECK); ROLLBACK undoes the whole transaction."},
    {"order by", "... ORDER BY expr [COLLATE name] [ASC | DESC] [NULLS {FIRST | LAST}], ...",
     "Sorts the result rows. An expression may also be a result column's alias or its 1-based position."},
    {"over",
     "func(args) [FILTER (WHERE expr)] OVER ([window] [PARTITION BY expr, ...] [ORDER BY ...] [frame])\nframe: {ROWS "
     "| RANGE | GROUPS} BETWEEN start AND end [EXCLUDE ...]",
     "Makes a function a window function: it is computed for each row over a window of related rows instead of "
     "collapsing them into one. PARTITION BY splits the rows into groups, ORDER BY orders each group, and the frame "
     "picks which rows around the current one count (UNBOUNDED PRECEDING, n PRECEDING, CURRENT ROW, n FOLLOWING, "
     "UNBOUNDED FOLLOWING). FILTER limits the rows an aggregate sees."},
    {"pragma", "PRAGMA [schema.]name\nPRAGMA [schema.]name = value\nPRAGMA [schema.]name(value)",
     "Queries or changes SQLite's settings and internal state. A pragma SQLite does not know is silently ignored."},
    {"primary key",
     "column type PRIMARY KEY [ASC | DESC] [ON CONFLICT ...] [AUTOINCREMENT]\nPRIMARY KEY (column, ...)  -- table "
     "constraint",
     "The column or columns that identify a row: no two rows may share them. A column declared exactly INTEGER "
     "PRIMARY KEY becomes an alias for the table's rowid. Unlike standard SQL, SQLite lets PRIMARY KEY columns hold "
     "NULL unless they are also NOT NULL, the table is WITHOUT ROWID or STRICT, or the column is INTEGER PRIMARY KEY."},
    {"raise",
     "RAISE(IGNORE)\nRAISE({ROLLBACK | ABORT | FAIL}, message)",
     "Only inside a trigger body: IGNORE abandons the current row's change silently; the others make the statement "
     "fail with the message, resolved as ROLLBACK, ABORT or FAIL."},
    {"references",
     "column type REFERENCES table [(column, ...)] [ON {DELETE | UPDATE} action] [[NOT] DEFERRABLE ...]\nFOREIGN KEY "
     "(column, ...) REFERENCES table [(column, ...)] ...\naction: SET NULL | SET DEFAULT | CASCADE | RESTRICT | NO "
     "ACTION",
     "A foreign key: the column's values must match a row of the parent table (its primary key, or the named "
     "columns). The ON DELETE / ON UPDATE action says what happens to child rows when the parent row goes or changes. "
     "SQLite only enforces foreign keys while PRAGMA foreign_keys = ON, which is off by default on each connection."},
    {"regexp",
     "expr [NOT] REGEXP pattern",
     "Calls the application-defined regexp(pattern, expr) function. SQLite has none built in, so it is an error "
     "unless one is loaded (the sqlite3 shell provides it)."},
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
    {"strict",
     "CREATE TABLE ... (...) STRICT",
     "Makes the table enforce its column types: every column must be declared INT, INTEGER, REAL, TEXT, BLOB or ANY, "
     "and a value that cannot be converted losslessly to the declared type is an error. Ordinary tables accept any "
     "value in any column."},
    {"temp",
     "CREATE {TEMP | TEMPORARY} {TABLE | VIEW | TRIGGER | INDEX} ...",
     "Creates the object in the temp schema: visible only to this connection and dropped automatically when it closes."},
    {"true",
     "TRUE\nFALSE",
     "Aliases for the integers 1 and 0; SQLite has no separate boolean type. Comparisons and logical operators also "
     "give 1, 0 or NULL."},
    {"union",
     "select UNION [ALL] select\nselect INTERSECT select\nselect EXCEPT select",
     "Compound SELECTs. UNION returns the rows of either query with duplicates removed (UNION ALL keeps them); "
     "INTERSECT the rows in both; EXCEPT the rows of the first that are not in the second. The queries must have the "
     "same number of columns; ORDER BY and LIMIT apply to the whole compound."},
    {"unique",
     "column type UNIQUE [ON CONFLICT ...]\nUNIQUE (column, ...)  -- table constraint",
     "No two rows may have the same value (or combination of values) in these columns. NULLs are all considered "
     "distinct here, so any number of rows may hold NULL. SQLite enforces it with an automatic index."},
    {"update",
     "[WITH ...] UPDATE [OR {ABORT | FAIL | IGNORE | REPLACE | ROLLBACK}] [schema.]table [AS alias] SET column = "
     "expr, ... [FROM ...] [WHERE expr] [RETURNING ...]",
     "Changes the columns of the rows the WHERE clause matches (all rows without one). A FROM clause joins other "
     "tables whose columns the SET and WHERE expressions may use."},
    {"using",
     "... JOIN table USING (column, ...)",
     "Joins on equality of the listed columns, which must exist under the same name in both tables; each appears once "
     "in SELECT *."},
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
    {"without rowid",
     "CREATE TABLE ... (..., PRIMARY KEY (...)) WITHOUT ROWID",
     "Stores the table as a B-tree keyed on its PRIMARY KEY instead of on a hidden rowid. It needs an explicit "
     "PRIMARY KEY, whose columns become NOT NULL; it can save space and lookups for tables with a non-integer or "
     "composite key, and has no rowid, AUTOINCREMENT or sqlite3_last_insert_rowid()."},
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
    // Words and phrases documented under another entry.
    static const char *const kAliases[][2] = {
        {"end", "commit"}, {"end transaction", "commit"}, {"commit transaction", "commit"},
        {"rollback transaction", "rollback"}, {"rollback to", "rollback"},
        {"begin transaction", "begin"}, {"deferred", "begin"}, {"immediate", "begin"}, {"exclusive", "begin"},
        {"replace", "insert"}, {"replace into", "insert"}, {"insert into", "insert"}, {"delete from", "delete"},
        {"upsert", "on conflict"}, {"do nothing", "on conflict"}, {"do update", "on conflict"},
        {"do update set", "on conflict"}, {"set", "update"},
        {"explain query plan", "explain"}, {"query plan", "explain"},
        {"create unique index", "create index"}, {"create temp table", "create table"},
        {"create temporary table", "create table"}, {"create temp view", "create view"},
        {"create temporary view", "create view"}, {"create temp trigger", "create trigger"},
        {"create temporary trigger", "create trigger"}, {"temporary", "temp"},
        {"before", "create trigger"}, {"after", "create trigger"}, {"instead of", "create trigger"},
        {"for each row", "create trigger"},
        {"or", "and"}, {"current date", "current timestamp"}, {"current time", "current timestamp"},
        {"false", "true"}, {"all", "distinct"}, {"select distinct", "distinct"}, {"select all", "distinct"},
        {"not in", "in"}, {"not like", "like"}, {"not glob", "glob"}, {"not regexp", "regexp"},
        {"not match", "match"}, {"not between", "between"}, {"not exists", "exists"}, {"escape", "like"},
        {"is not", "is"}, {"is distinct from", "is"}, {"is not distinct from", "is"},
        {"is not null", "is null"}, {"isnull", "is null"}, {"notnull", "is null"},
        {"left join", "join"}, {"right join", "join"}, {"full join", "join"},
        {"inner join", "join"}, {"cross join", "join"}, {"natural join", "join"}, {"outer join", "join"},
        {"left outer join", "join"}, {"left", "join"}, {"right", "join"}, {"full", "join"}, {"inner", "join"},
        {"outer", "join"}, {"cross", "join"}, {"natural", "join"},
        {"union all", "union"}, {"intersect", "union"}, {"except", "union"},
        {"asc", "order by"}, {"desc", "order by"}, {"nulls first", "order by"}, {"nulls last", "order by"},
        {"having", "group by"}, {"offset", "limit"}, {"recursive", "with"}, {"with recursive", "with"},
        {"materialized", "with"}, {"not materialized", "with"},
        {"partition by", "over"}, {"filter", "over"}, {"rows", "over"}, {"range", "over"}, {"groups", "over"},
        {"unbounded", "over"}, {"preceding", "over"}, {"following", "over"}, {"current row", "over"},
        {"exclude", "over"},
        {"primary", "primary key"}, {"foreign key", "references"}, {"on delete", "references"},
        {"on update", "references"}, {"on delete cascade", "references"}, {"on update cascade", "references"},
        {"cascade", "references"}, {"restrict", "references"}, {"no action", "references"},
        {"set null", "references"}, {"set default", "references"}, {"deferrable", "references"},
        {"not deferrable", "references"}, {"initially deferred", "references"},
        {"generated", "generated always"}, {"generated always as", "generated always"},
        {"stored", "generated always"}, {"virtual", "generated always"}, {"without", "without rowid"},
        {"or abort", "or ignore"}, {"or fail", "or ignore"}, {"or replace", "or ignore"},
        {"or rollback", "or ignore"}, {"on conflict abort", "or ignore"}, {"on conflict fail", "or ignore"},
        {"on conflict ignore", "or ignore"}, {"on conflict replace", "or ignore"},
        {"on conflict rollback", "or ignore"},
    };
    for (const auto &a : kAliases) {
        if (l == a[0]) {
            l = a[1];
            break;
        }
    }
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
