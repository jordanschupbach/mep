#ifndef MEP_SQL_AST_H
#define MEP_SQL_AST_H

// SQLite's SQL, read the way SQLite reads it: the tokenizer is a port of
// src/tokenize.c (character classes, keyword table, the context-sensitive
// WINDOW/OVER/FILTER words, digit separators) and the parser
// (sql_parse.cpp) is a hand-written recursive-descent reading of
// src/parse.y -- the same productions, the same %fallback list (a keyword
// such as KEY, ROWS or REPLACE is an identifier wherever an identifier can
// be shifted and the keyword itself cannot), and the same three syntax
// error messages on the same token:
//
//   near "X": syntax error      the first token no statement can continue with
//   incomplete input            the input ended inside a statement
//   unrecognized token: "X"     a token the tokenizer cannot read
//
// Like the sqlite3 shell, a script is cut into statements the way
// sqlite3_complete() cuts it (src/complete.c: a `;` ends a statement except
// inside CREATE TRIGGER ... END), each statement is parsed on its own, and
// one syntax error is reported per statement; the next statement starts
// fresh. tools/sql_parse_check.py measures all of this against SQLite
// itself (sqlite3_prepare_v2 + sqlite3_error_offset).
//
// No libsqlite, no tree-sitter, nothing spawned: pure functions over text.
// Positions are byte offsets into the text; SqlLineIndex maps them to the
// 0-based line / byte column the LSP speaks.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// --- Tokens -------------------------------------------------------------

enum class SqlTok : uint8_t {
    Eof,
    Space,
    Comment,
    Illegal,
    // punctuation / operators
    Semi, LP, RP, Comma, Dot, Plus, Minus, Star, Slash, Rem, Concat, Ptr,
    Eq, Ne, Lt, Le, Gt, Ge, BitAnd, BitOr, BitNot, LShift, RShift,
    // literals and names
    Id, String, Integer, Float, QNumber, Blob, Variable,
    // keywords (TK_ names; several spellings share one: JOIN_KW, LIKE_KW, CTIME_KW, TEMP)
    Abort, Action, Add, After, All, Alter, Always, Analyze, And, As, Asc, Attach, Autoincr,
    Before, Begin, Between, By, Cascade, Case, Cast, Check, Collate, ColumnKw, Commit,
    Conflict, Constraint, Create, CtimeKw, Current, Database, Default, Deferrable, Deferred,
    Delete, Desc, Detach, Distinct, Do, Drop, Each, Else, End, Escape, Except, Exclude,
    Exclusive, Exists, Explain, Fail, Filter, First, Following, For, Foreign, From,
    Generated, Group, Groups, Having, If, Ignore, Immediate, In, Index, Indexed,
    Initially, Insert, Instead, Intersect, Into, Is, IsNull, Join, JoinKw, Key, Last, LikeKw,
    Limit, Match, Materialized, No, Not, Nothing, NotNull, Null, Nulls, Of, Offset, On, Or,
    Order, Others, Over, Partition, Plan, Pragma, Preceding, Primary, Query, Raise, Range,
    Recursive, References, Reindex, Release, Rename, Replace, Restrict, Returning, Rollback,
    Row, Rows, Savepoint, Select, Set, Table, Temp, Then, Ties, To, Transaction, Trigger,
    Unbounded, Union, Unique, Update, Using, Vacuum, Values, View, Virtual, When, Where,
    Window, With, Without,
};

struct SqlToken {
    SqlTok type = SqlTok::Eof;
    int start = 0;  // byte offset
    int len = 0;
};

/** @brief Whether a token type is one of SQLite's keywords (anything from Abort on). */
inline bool SqlIsKeyword(SqlTok t) { return t >= SqlTok::Abort; }

/** @brief Whether a keyword falls back to an identifier (parse.y's %fallback ID list). */
bool SqlIsFallback(SqlTok t);

/** @brief The keyword a word spells (case-insensitively), or SqlTok::Id. */
SqlTok SqlKeywordType(std::string_view word);

/** @brief Every keyword spelling SQLite knows, upper case, sorted. */
const std::vector<std::string> &SqlKeywordList();

/**
 * @brief SQLite's tokenizer over a whole text: every token, comments and whitespace included, ending with an Eof
 *        token at text.size(). WINDOW, OVER and FILTER are already resolved (they are identifiers unless followed
 *        by what makes them keywords, exactly as tokenize.c decides).
 */
std::vector<SqlToken> SqlTokenize(std::string_view text);

/** @brief An identifier's or string's value: quotes removed, doubled quotes collapsed. */
std::string SqlDequote(std::string_view raw);

/** @brief Maps byte offsets to 0-based (line, byte column) pairs. */
class SqlLineIndex {
public:
    explicit SqlLineIndex(std::string_view text);
    int Line(int offset) const;
    int Col(int offset) const;
    int Offset(int line, int col) const;
    int LineCount() const { return static_cast<int>(starts_.size()); }
    int LineStart(int line) const;

private:
    std::vector<int> starts_;
    int size_ = 0;
};

// --- AST ----------------------------------------------------------------
//
// Token references are indices into SqlParse::tokens (which holds every
// token, comments included); -1 means absent.

struct SqlName {
    int tok = -1;
    std::string text;  // dequoted
    bool ok() const { return tok >= 0; }
};

struct SqlExpr;
struct SqlSelect;
using SqlExprP = std::unique_ptr<SqlExpr>;
using SqlSelectP = std::unique_ptr<SqlSelect>;

struct SqlOrderTerm {
    SqlExprP expr;
};

struct SqlWindowSpec {
    SqlName base;  // `OVER (w ORDER BY ...)`: the window it extends
    std::vector<SqlExprP> partition;
    std::vector<SqlOrderTerm> order;
    std::vector<SqlExprP> frame;  // the frame bounds' expressions
    int first = -1, last = -1;
};

struct SqlWindowDef {
    SqlName name;
    SqlWindowSpec spec;
};

struct SqlExpr {
    enum Kind {
        Literal,    // op = the token
        Variable,   // op = the token
        Column,     // db.table.column, any prefix absent
        Func,       // func(args) [FILTER (WHERE filter)] [OVER over | OVER over_name]
        Unary,      // op args[0]
        Binary,     // args[0] op args[1]
        Between,    // args[0] [NOT] BETWEEN args[1] AND args[2]
        Like,       // args[0] [NOT] LIKE args[1] [ESCAPE args[2]]
        In,         // args[0] [NOT] IN (args[1..]) | (select) | in_table
        Is,         // args[0] IS [NOT] [DISTINCT FROM] args[1]
        Postfix,    // args[0] ISNULL | NOTNULL | NOT NULL
        Collate,    // args[0] COLLATE collation
        Cast,       // CAST(args[0] AS type)
        Case,       // CASE [args[0] if has_operand] WHEN/THEN pairs [ELSE last]
        Exists,     // EXISTS (select)
        Subquery,   // (select)
        Row,        // (args...)
        Paren,      // (args[0])
        Raise,      // RAISE(IGNORE | kind, args[0])
    } kind = Literal;
    int first = -1, last = -1;  // token range, inclusive
    int op = -1;
    bool negated = false;  // NOT IN / NOT LIKE / IS NOT / NOT BETWEEN
    SqlName db, table, column;
    SqlName func;
    bool star = false, distinct = false;
    bool has_operand = false, has_else = false;
    std::vector<SqlExprP> args;
    SqlSelectP select;
    SqlName in_db, in_table;
    std::vector<SqlOrderTerm> order;  // aggregate ORDER BY
    SqlExprP filter;
    std::unique_ptr<SqlWindowSpec> over;
    SqlName over_name;
    SqlName collation;
    std::string type;  // CAST's type, words joined by one space
};

struct SqlCte {
    SqlName name;
    std::vector<SqlName> columns;
    SqlSelectP select;
    int first = -1, last = -1;
};

struct SqlWith {
    bool recursive = false;
    std::vector<SqlCte> ctes;
    int first = -1, last = -1;
};

struct SqlResultCol {
    SqlExprP expr;   // null for * and t.*
    SqlName alias;
    bool star = false;
    SqlName star_table;  // t.*
    int first = -1, last = -1;
};

struct SqlFromItem {
    enum Kind { Table, TableFunc, Subquery, Group } kind = Table;
    SqlName db, name, alias;
    std::vector<SqlExprP> args;  // table-valued function arguments
    SqlSelectP select;
    std::vector<SqlFromItem> group;  // `(a JOIN b)`
    int join_first = -1, join_last = -1;  // the join operator before this item (-1: first item)
    bool comma_join = false, natural = false, left_or_right = false;
    SqlExprP on;
    std::vector<SqlName> using_cols;
    SqlName indexed;  // INDEXED BY name
    int first = -1, last = -1;
};

struct SqlCore {
    bool is_values = false;
    std::vector<std::vector<SqlExprP>> rows;  // VALUES
    int compound_op = -1;  // the UNION/EXCEPT/INTERSECT token before this core (-1: first)
    bool distinct = false;
    std::vector<SqlResultCol> cols;
    std::vector<SqlFromItem> from;
    bool has_from = false;
    SqlExprP where;
    std::vector<SqlExprP> group;
    SqlExprP having;
    std::vector<SqlWindowDef> windows;
    std::vector<SqlOrderTerm> order;
    SqlExprP limit, offset;
    int first = -1, last = -1;
};

struct SqlSelect {
    std::unique_ptr<SqlWith> with;
    std::vector<SqlCore> cores;
    int first = -1, last = -1;
};

struct SqlColumnDef {
    SqlName name;
    std::string type;  // declared type words joined by one space ("" when absent)
    int type_first = -1, type_last = -1;
    bool primary_key = false, not_null = false, unique = false, autoincrement = false, generated = false;
    std::vector<SqlExprP> exprs;  // DEFAULT (...), CHECK (...), AS (...)
    SqlName ref_table;
    std::vector<SqlName> ref_cols;
    SqlName collation;
    int first = -1, last = -1;
};

struct SqlTableConstraint {
    enum Kind { PrimaryKey, Unique, Check, ForeignKey, Name } kind = Check;
    SqlName name;
    std::vector<SqlOrderTerm> terms;  // PRIMARY KEY / UNIQUE columns
    SqlExprP check;
    std::vector<SqlName> cols;  // FOREIGN KEY (cols)
    SqlName ref_table;
    std::vector<SqlName> ref_cols;
    int first = -1, last = -1;
};

struct SqlSetItem {
    std::vector<SqlName> cols;
    SqlExprP value;
};

struct SqlUpsert {
    std::vector<SqlOrderTerm> target;
    SqlExprP target_where;
    bool do_update = false;
    std::vector<SqlSetItem> set;
    SqlExprP where;
    int first = -1, last = -1;
};

struct SqlStmt {
    enum Kind {
        Empty, Select, Insert, Update, Delete, CreateTable, CreateView, CreateIndex, CreateTrigger,
        CreateVirtualTable, DropTable, DropView, DropIndex, DropTrigger, AlterTable, Pragma, Begin, Commit,
        Rollback, Savepoint, Release, Attach, Detach, Vacuum, Reindex, Analyze,
    } kind = Empty;
    int first = -1, last = -1;  // tokens, inclusive; `last` is the `;` when there is one
    int explain = 0;            // 1 EXPLAIN, 2 EXPLAIN QUERY PLAN
    bool broken = false;        // a syntax error cut it short: what was read before it
    bool temp = false, if_not_exists = false, if_exists = false, unique = false;
    std::unique_ptr<SqlWith> with;
    SqlSelectP select;  // SELECT; INSERT ... SELECT; CREATE VIEW/TABLE ... AS; a trigger's SELECT
    SqlName db, name, alias;  // the table/view/index/trigger the statement is about
    std::vector<SqlName> columns;  // INSERT (cols) / CREATE VIEW v(cols) / UPDATE OF cols
    // CREATE TABLE
    std::vector<SqlColumnDef> coldefs;
    std::vector<SqlTableConstraint> constraints;
    bool without_rowid = false, strict = false;
    // INSERT / UPDATE / DELETE
    int insert_or = -1;  // INSERT OR <x> / UPDATE OR <x> token
    bool replace = false, default_values = false;
    std::vector<SqlSetItem> set;
    std::vector<SqlFromItem> from;
    SqlExprP where;
    std::vector<SqlResultCol> returning;
    std::vector<SqlUpsert> upserts;
    std::vector<SqlOrderTerm> order;
    SqlExprP limit, offset;
    // CREATE INDEX: name ON on_table(index_terms) WHERE where
    SqlName on_db, on_table;
    std::vector<SqlOrderTerm> index_terms;
    // CREATE TRIGGER
    int trigger_time = -1, trigger_event = -1;  // tokens
    std::vector<SqlStmt> body;
    SqlExprP when;
    int begin_tok = -1;
    // CREATE VIRTUAL TABLE name USING module(args)
    SqlName module;
    std::vector<std::pair<int, int>> module_args;  // token ranges, inclusive
    // ALTER TABLE
    enum AlterOp { AlterNone, RenameTable, RenameColumn, AddColumn, DropColumn, OtherAlter } alter = AlterNone;
    SqlName column, new_name;
    std::unique_ptr<SqlColumnDef> add_column;
    // PRAGMA name [= value | (value)]
    int value_first = -1, value_last = -1;
    // ATTACH expr AS expr / DETACH expr / VACUUM INTO expr / generic expressions
    std::vector<SqlExprP> exprs;
};

struct SqlSyntaxError {
    std::string message;  // SQLite's wording
    int tok = -1;         // the token it is "near" (the Eof token for incomplete input)
    int offset = -1;      // byte offset of that token (-1 for incomplete input, as sqlite3_error_offset)
    bool syntax = true;   // false: a parser-action error ("unknown table option: x"), not one of the three
    int chunk = -1;       // index into SqlParse::chunks
};

// One statement as sqlite3_complete() cuts the script.
struct SqlChunk {
    int first_tok = 0, end_tok = 0;  // [first, end) over tokens
    int start = 0, end = 0;          // byte range
};

struct SqlParse {
    std::string text;
    std::vector<SqlToken> tokens;  // no Space tokens; ends with Eof
    std::vector<SqlChunk> chunks;
    std::vector<SqlStmt> stmts;
    std::vector<SqlSyntaxError> errors;
    // The token text.
    std::string_view Text(int tok) const;
    std::string Upper(int tok) const;
};

/** @brief Tokenizes, cuts into statements and parses a whole script. */
SqlParse SqlParseScript(std::string_view text);

/**
 * @brief The text with the sqlite3 shell's dot-command lines (`.headers on`: a line starting with `.` where no
 *        statement is pending) blanked to spaces, so offsets stay put and the parser never sees them.
 */
std::string SqlBlankDotCommands(std::string_view text);

/** @brief Whether a line is one the sqlite3 shell would read as a dot-command, were no statement pending. */
bool SqlIsDotCommandLine(std::string_view line);

/** @brief Whether SQL text leaves no statement pending (nothing but complete statements and comments). */
bool SqlSettled(std::string_view text);

#endif  // MEP_SQL_AST_H
