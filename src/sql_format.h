#ifndef MEP_SQL_FORMAT_H
#define MEP_SQL_FORMAT_H

// mep's SQL formatter (`gf` in a .sql buffer: mep.format_sql, and
// mep-sql-lsp's textDocument/formatting). SQLite's dialect, read with
// sql_ast.h's tokenizer and parser.
//
// The layout is the common "one clause per line" style that sqlfluff's
// default rules and pgFormatter both produce:
//
//   - keywords upper case (Options::upper_keywords = false keeps their case);
//     identifiers, strings and comments are never changed;
//   - each clause on its own line at the statement's indent: SELECT, FROM,
//     each JOIN (its ON / USING on the same line when it fits, else indented
//     below), WHERE, GROUP BY, HAVING, WINDOW, ORDER BY, LIMIT; INSERT INTO,
//     VALUES, ON CONFLICT, RETURNING; UPDATE, SET; DELETE FROM;
//   - a clause's list stays on the clause's line when it fits in `width`,
//     else one item per line, indented one level, with trailing commas; a
//     WHERE that does not fit breaks before each top-level AND / OR;
//   - subqueries and CTE bodies are indented one level inside their
//     parentheses, the closing parenthesis back at the opening line's indent;
//   - CREATE TABLE puts one column definition or table constraint per line,
//     column types aligned; CREATE TRIGGER's body statements are formatted
//     as statements, indented between BEGIN and END;
//   - comments stay where they were relative to the tokens around them;
//     blank lines between statements are kept, at most one; a `.command`
//     line of the sqlite3 shell and the `-- database:` header pass through
//     untouched.
//
// Format never returns text it has not checked: the result is re-tokenized
// and must have the same tokens as the input (comments included) apart from
// whitespace and the case of keywords, and must parse with exactly the same
// errors. A script with a syntax error is refused (Result::ok false, with
// SQLite's message and its line) and the buffer is left alone.

#include <string>

namespace sqlfmt {

struct Options {
    int indent_width = 4;
    bool use_tabs = false;
    int width = 80;
    bool upper_keywords = true;
};

struct Result {
    bool ok = false;
    std::string text;
    std::string error;    // why it was refused
    int error_line = -1;  // 0-based line of the error, -1 when none applies
};

Result Format(const std::string &text, const Options &opts = Options());

}  // namespace sqlfmt

#endif  // MEP_SQL_FORMAT_H
