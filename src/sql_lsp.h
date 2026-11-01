#ifndef MEP_SQL_LSP_H
#define MEP_SQL_LSP_H

#include <cstddef>
#include <string>
#include <vector>

#include "sql_ast.h"

// The analysis half of mep's own SQL language server (the wire half is
// sql_lsp_server.cpp, the `mep-sql-lsp` binary; kBuiltinLsp's `sql_ls`
// registry entry in main.cpp spawns it). The dialect is SQLite 3's, read by
// sql_ast.h's parser, whose syntax errors are SQLite's own (checked against
// sqlite3_prepare by tools/sql_parse_check.py).
//
// Everything here is a pure function over the document's lines plus an
// SqlLspContext: the schema objects the server found elsewhere -- the
// workspace's other .sql files, and the database a `-- database: PATH`
// header names (which the server reads by running `sqlite3 -readonly`;
// this half never touches a file). So the whole server is testable without
// a client and without SQLite (sql_lsp_test.cpp).
//
// The schema model: tables, views, indexes, triggers and virtual tables
// from the document's DDL (ALTER TABLE applied in order), the context's
// objects, then per statement the CTEs and FROM-clause aliases in scope.
// Names compare case-insensitively, as in SQLite.
//
// Positions are 0-based lines and 0-based byte columns (`positionEncoding:
// "utf-8"`), as everywhere in mep's own servers.

struct SqlLspRange {
    int line = 0, col = 0, end_line = 0, end_col = 0;
};

// --- Schema ----------------------------------------------------------------

struct SqlLspColumn {
    std::string name;
    std::string type;
    bool primary_key = false, not_null = false;
    bool hidden = false;     // a virtual table's hidden column (fts's docid, rank, ...): not in `*`, not inserted
    bool generated = false;  // GENERATED ALWAYS AS: in `*`, but an INSERT gives it no value
    std::string uri;  // where it is declared ("" = the document itself); empty range when unknown
    SqlLspRange range;
    bool has_range = false;
    int decl_tok = -1;  // (internal) the declaring token, when declared in the document being analysed
    int added_at = -1;    // (internal) the ALTER TABLE that added it (ADD / RENAME COLUMN: its first token)
    int dropped_at = -1;  // (internal) the ALTER TABLE that removed it (DROP / RENAME COLUMN)
};

struct SqlLspObject {
    enum Kind { Table, View, Index, Trigger, VirtualTable } kind = Table;
    std::string name;
    std::string table;   // an index's or trigger's table
    std::string module;  // a virtual table's module
    std::vector<SqlLspColumn> columns;
    bool columns_known = true;  // false: a virtual table or view whose columns could not be worked out
    bool without_rowid = false;
    std::string sql;     // the CREATE statement
    std::string origin;  // "" this document; otherwise the file (or database) it came from
    std::string uri;     // for go-to-definition ("" when it has no source position, e.g. a database)
    SqlLspRange range;   // the name in its CREATE statement
    bool has_range = false;
    int decl_tok = -1;   // (internal) as SqlLspColumn::decl_tok
    bool redeclared = false;  // created more than once (DROP + CREATE): `columns` is the union
};

struct SqlLspContext {
    std::vector<SqlLspObject> workspace;  // from the workspace's other .sql files
    std::vector<SqlLspObject> database;   // from the `-- database:` file
    bool database_loaded = false;         // a database's schema is known: unknown names are errors
    std::string database_path;            // as resolved, for messages
    std::string database_problem;         // why a named database could not be read ("" if none)
};

/** @brief The schema objects a script's own DDL declares (what the server indexes from a workspace file). */
std::vector<SqlLspObject> SqlLspScriptObjects(const std::string &text, const std::string &uri);

/**
 * @brief Reads the objects of `SELECT type, name, tbl_name, sql FROM sqlite_schema` rows (what the server gets
 *        back from sqlite3) by parsing each `sql` with this server's parser.
 */
std::vector<SqlLspObject> SqlLspSchemaObjects(const std::vector<std::vector<std::string>> &rows,
                                              const std::string &origin);

struct SqlLspDatabaseHeader {
    bool found = false;
    std::string path;  // as written
    int line = 0, col = 0, end_col = 0;  // the path's position
};

/** @brief The `-- database: PATH` line among a script's leading comment lines. */
SqlLspDatabaseHeader SqlLspFindDatabaseHeader(const std::vector<std::string> &lines);

// --- Diagnostics -----------------------------------------------------------

enum class SqlLspSeverity { Error = 1, Warning = 2, Information = 3, Hint = 4 };

struct SqlLspDiagnostic {
    SqlLspRange range;
    SqlLspSeverity severity = SqlLspSeverity::Error;
    std::string code;  // stable id ("syntax-error", "unknown-column", ...): tests assert on it
    std::string message;
};

/** @brief SQLite's syntax errors, then the checks listed in help/sql-lsp.org. */
std::vector<SqlLspDiagnostic> SqlLspDiagnostics(const std::vector<std::string> &lines,
                                                const SqlLspContext &ctx = SqlLspContext());

/** @brief `path:line:col: severity: message [code]`, 1-based, as `mep-sql-lsp --lint` prints it. */
std::string SqlLspFormatDiagnostic(const std::string &path, const SqlLspDiagnostic &d);

/** @brief Whether a diagnostic makes `--lint` exit non-zero (an error or a warning). */
bool SqlLspDiagnosticFails(const SqlLspDiagnostic &d);

// --- Completion ------------------------------------------------------------

enum class SqlLspKind {
    Text = 1, Method = 2, Function = 3, Field = 5, Variable = 6, Class = 7, Interface = 8, Module = 9,
    Property = 10, Keyword = 14, Snippet = 15, Reference = 18, Struct = 22, Event = 23, Operator = 24,
};

struct SqlLspCompletionItem {
    std::string label;
    std::string insert_text;
    SqlLspKind kind = SqlLspKind::Text;
    std::string detail;
    std::string documentation;
    int replace_start = 0, replace_end = 0;  // byte columns on the request's line
};

/**
 * @brief Completions at a position: `alias.`/`table.` members, table names after FROM/JOIN/INTO/UPDATE/...,
 *        PRAGMA names after PRAGMA, statement keywords at a statement's start, and in an expression the columns of
 *        the tables in the FROM clause, SQLite's functions and keywords.
 */
std::vector<SqlLspCompletionItem> SqlLspCompletions(const std::vector<std::string> &lines, int line, int col,
                                                    const SqlLspContext &ctx = SqlLspContext());

// --- Hover / signature help -------------------------------------------------

struct SqlLspHoverInfo {
    bool found = false;
    std::string text;  // markdown
    SqlLspRange range;
};

/** @brief A function's signatures and docs; a table's or view's columns; a column's table and type; a keyword. */
SqlLspHoverInfo SqlLspHover(const std::vector<std::string> &lines, int line, int col,
                            const SqlLspContext &ctx = SqlLspContext());

struct SqlLspSignature {
    std::string label;
    std::vector<std::string> params;  // substrings of label
};

struct SqlLspSignatureHelp {
    bool found = false;
    std::vector<SqlLspSignature> signatures;
    std::string documentation;
    int active_signature = 0;
    int active_param = 0;
};

/** @brief The function call the cursor is inside: every documented form, the active one and argument. */
SqlLspSignatureHelp SqlLspSignatureAt(const std::vector<std::string> &lines, int line, int col);

// --- Navigation -------------------------------------------------------------

struct SqlLspLocation {
    std::string uri;  // "" = this document
    SqlLspRange range;
};

/** @brief Where the table, view, column, CTE, alias or window under the cursor is declared. */
std::vector<SqlLspLocation> SqlLspDefinition(const std::vector<std::string> &lines, int line, int col,
                                             const SqlLspContext &ctx = SqlLspContext());

struct SqlLspReference {
    SqlLspRange range;
    bool is_definition = false;
    bool is_write = false;
};

/** @brief Every occurrence in the document of the name under the cursor (its declaration included). */
std::vector<SqlLspReference> SqlLspReferences(const std::vector<std::string> &lines, int line, int col,
                                              const SqlLspContext &ctx = SqlLspContext());

struct SqlLspTextEdit {
    SqlLspRange range;
    std::string new_text;
};

struct SqlLspRenameTarget {
    bool found = false;
    SqlLspRange range;
    std::string placeholder;
};

/** @brief textDocument/prepareRename: tables, columns, CTEs, aliases and windows the document declares. */
SqlLspRenameTarget SqlLspPrepareRename(const std::vector<std::string> &lines, int line, int col,
                                       const SqlLspContext &ctx = SqlLspContext());

/** @brief The edits renaming the name under the cursor; `error` says why not (a name declared elsewhere, ...). */
std::vector<SqlLspTextEdit> SqlLspRename(const std::vector<std::string> &lines, int line, int col,
                                         const std::string &new_name, std::string *error,
                                         const SqlLspContext &ctx = SqlLspContext());

// --- Outline / folding ------------------------------------------------------

enum class SqlLspSymbolKind {
    Module = 2, Namespace = 3, Class = 5, Method = 6, Field = 8, Interface = 11, Function = 12, Variable = 13,
    Key = 20, Object = 19, Struct = 23, Event = 24,
};

struct SqlLspSymbol {
    std::string name;
    std::string detail;
    SqlLspSymbolKind kind = SqlLspSymbolKind::Object;
    SqlLspRange range;
    SqlLspRange sel_range;
    int parent = -1;
};

/** @brief The outline: one entry per statement, CREATE TABLE's columns and WITH's CTEs under theirs. */
std::vector<SqlLspSymbol> SqlLspSymbols(const std::vector<std::string> &lines);

struct SqlLspFold {
    int start_line = 0, end_line = 0;
    std::string kind;  // "" or "comment"
};

/** @brief Multi-line statements, parenthesized lists and subqueries, CREATE TRIGGER bodies, comment runs. */
std::vector<SqlLspFold> SqlLspFolds(const std::vector<std::string> &lines);

// --- Formatting -------------------------------------------------------------

/** @brief mep's SQL formatter (sql_format.h) as one edit over the changed lines; none when already formatted. */
std::vector<SqlLspTextEdit> SqlLspFormat(const std::vector<std::string> &lines, int indent_width, bool tabs,
                                         std::string *error);

// --- Vocabulary (sql_lsp_names.cpp is generated; sql_lsp_vocab.cpp hand-written) ---

struct SqlLspFunction {
    const char *name;        // lower case
    const char *kind;        // scalar, aggregate, window, math, date, json, extension
    const char *signatures;  // one per line
    const char *doc;         // SQLite's documentation (public domain), "" when it has none
    int min_args;
    int max_args;            // -1: no upper bound
    const char *counts;      // the accepted counts when bounded ("1,3": not 2), "" otherwise
};

struct SqlLspPragma {
    const char *name;
    const char *signatures;
    const char *doc;
};

struct SqlLspKeywordDoc {
    const char *name;      // "SELECT", "CREATE TABLE", "ON CONFLICT", ...
    const char *synopsis;  // lines
    const char *doc;
};

const SqlLspFunction *SqlLspFunctionTable(size_t *count);
const SqlLspPragma *SqlLspPragmaTable(size_t *count);
const SqlLspKeywordDoc *SqlLspKeywordDocTable(size_t *count);

/** @brief A function by name (case-insensitive), or null. */
const SqlLspFunction *SqlLspFindFunction(const std::string &name);
/** @brief A PRAGMA by name, or null. */
const SqlLspPragma *SqlLspFindPragma(const std::string &name);
/** @brief A statement's or keyword's documentation ("select", "create table", "join"), or null. */
const SqlLspKeywordDoc *SqlLspFindKeywordDoc(const std::string &name);
/** @brief Whether a function accepts `n` arguments. */
bool SqlLspArityOk(const SqlLspFunction &f, int n);

/**
 * @brief `mep-sql-lsp --describe NAME`: plain text for a function, PRAGMA or statement/keyword -- signature lines,
 *        a blank line, the description wrapped at 78 columns -- or "" when NAME is unknown. With objects (a
 *        database's), also a table/view ("name": its columns and CREATE statement) or a column ("table.column",
 *        or a column name some table has).
 */
std::string SqlLspDescribe(const std::string &name, const std::vector<SqlLspObject> &objects = {});

/** @brief Wraps prose at `width` columns, keeping paragraph breaks. */
std::string SqlLspWrap(const std::string &text, size_t width);

#endif  // MEP_SQL_LSP_H
