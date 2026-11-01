#ifndef MEP_SQL_LSP_INDEX_H
#define MEP_SQL_LSP_INDEX_H

// The semantic index mep-sql-lsp's features read (internal to sql_lsp*.cpp):
// every name occurrence in the document, each tied to the thing it names --
// a schema object, one of its columns, a CTE, a CTE column, a FROM alias, a
// result-column alias or a named window -- plus the diagnostics the
// resolution found along the way. Built by sql_lsp_analysis.cpp.

#include <map>
#include <string>
#include <vector>

#include "sql_ast.h"
#include "sql_lsp.h"

struct SqlIdxTarget {
    enum Kind { Object, Column, Cte, CteColumn, Alias, ResultAlias, Window } kind = Object;
    std::string key;   // identity: two occurrences name the same thing iff their targets' keys match
    std::string name;  // as declared
    std::string type;  // a column's declared type
    std::string table; // a column's table (or CTE), an alias's table
    const SqlLspObject *object = nullptr;  // Object / Column: the schema object
    int column = -1;                       // Column: index into object->columns
    bool local = false;  // declared in this document (renamable)
    int decl_tok = -1;   // the declaration's token in this document
    std::vector<std::string> columns;  // a CTE's / alias's columns, for hover
};

struct SqlIdxOcc {
    int tok = -1;
    int target = -1;
    bool is_def = false;
    bool is_write = false;
};

struct SqlIdxCall {
    int tok = -1;  // the function name
    std::string name;
    int argc = 0;
};

struct SqlIndex {
    SqlParse parse;
    SqlLineIndex lines{std::string_view()};
    std::vector<SqlLspObject> local;  // the document's own DDL
    std::vector<SqlIdxTarget> targets;
    std::vector<SqlIdxOcc> occs;
    std::vector<SqlIdxCall> calls;
    std::vector<SqlLspDiagnostic> diags;  // semantic diagnostics (syntax errors are added by the caller)

    SqlLspRange RangeOf(int tok) const;
    /** @brief The occurrence whose token covers (line, col), or -1. */
    int OccAt(int line, int col) const;
    /** @brief The token covering (line, col) (a name's end counts), or -1. */
    int TokAt(int line, int col) const;
};

/** @brief Joins lines with '\n'. */
std::string SqlJoinLines(const std::vector<std::string> &lines);

/** @brief Parses the document and resolves every name in it. */
SqlIndex SqlBuildIndex(const std::vector<std::string> &lines, const SqlLspContext &ctx);

/** @brief Lower-cases ASCII. */
std::string SqlLower(const std::string &s);

/** @brief The schema objects of a parsed script (CREATE / ALTER applied in order; a re-created name unions). */
std::vector<SqlLspObject> SqlObjectsOf(const SqlParse &p, const SqlLineIndex &li, const std::string &origin,
                                       const std::string &uri);

/** @brief The columns a table-valued function in FROM yields (json_each, generate_series, pragma_*), if known. */
bool SqlTableFunctionColumns(const std::string &name, std::vector<std::string> *cols);

/** @brief Whether a table name is one SQLite itself provides (sqlite_schema, sqlite_sequence, ...). */
bool SqlIsSystemTable(const std::string &lower_name);

#endif  // MEP_SQL_LSP_INDEX_H
