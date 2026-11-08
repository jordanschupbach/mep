#ifndef MEP_FORTRAN_PARSE_H
#define MEP_FORTRAN_PARSE_H

// A Fortran reader for mep's Fortran language server (fortran_lsp.cpp):
// source form (free and fixed), the C preprocessor gfortran runs on .F90
// files (modelled, not executed: #if/#ifdef/#elif/#else/#endif evaluated
// against the file's own #defines plus gfortran's predefines, object- and
// function-like macros expanded), statements and their tokens, the
// construct structure (program units, CONTAINS, interface blocks, derived
// types, IF/DO/SELECT/... blocks) and every statement's declarations and
// name references.
//
// Errors are reported where gfortran reports them, worded the way gfortran
// words them ("Syntax error in CALL statement", "Unclassifiable statement",
// "Expecting END DO statement", "Unexpected ELSE statement", ...), and only
// for what gfortran -fsyntax-only rejects as syntax -- names, types and
// interfaces are the language server's business, not the reader's.
// tools/fortran_parse_check.py measures the verdicts against gfortran.
// Written from the Fortran 2018 standard and gfortran's observable
// behaviour; no gfortran code is used.
//
// Positions are 0-based lines and 0-based byte columns.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fortran {

enum class Form { Free, Fixed };

/** @brief The source form a file name implies: .f/.for/.ftn/.fpp (any case) are fixed, the rest free. */
Form FormForPath(std::string_view path);
/** @brief Whether gfortran runs the preprocessor on a file of this name (upper-case suffix, .fpp). */
bool PreprocessedPath(std::string_view path);

struct Pos {
    int line = 0, col = 0;
};

inline bool operator<(const Pos &a, const Pos &b) { return a.line != b.line ? a.line < b.line : a.col < b.col; }
inline bool operator==(const Pos &a, const Pos &b) { return a.line == b.line && a.col == b.col; }

enum class TokKind : uint8_t {
    Name,
    Int,        // 42, 42_8, 42_ik
    Real,       // 1.0, 1e3, 1.0d0, .5_dp
    String,     // 'x', "x", c_char_'x' (text is the whole spelling)
    Boz,        // b'101', z"ff", 'ff'x
    Hollerith,  // 5Hhello
    DotOp,      // .and. .eq. .myop. (lower = "and", "eq", "myop")
    Logical,    // .true. .false._lk
    Op,         // ** // == /= <= >= < > => :: (/ /) ( ) [ ] , : = + - * / % ? @ ;
};

struct Token {
    TokKind kind = TokKind::Name;
    std::string text;   // as written (continuations removed)
    std::string lower;  // names and dot-operators: lower case; else = text
    Pos pos, end;       // end is exclusive
};

struct Comment {
    Pos pos, end;
    std::string text;
};

enum class StmtKind : uint8_t {
    Unknown,
    // program units
    Program, Module, Submodule, Subroutine, Function, BlockData, ModuleProcedureUnit, End,
    // specification
    Use, Import, Implicit, TypeDecl, DerivedType, EndType, Procedure, Generic, Final, Contains,
    Interface, EndInterface, ModuleProcedureList, Enum, Enumerator, EndEnum, Parameter, Data, Common,
    Equivalence, Namelist, Attribute, Format, Entry, Include, StatementFunction, Sequence, PrivateComponents,
    // executable
    Assignment, PointerAssignment, Call, If, LogicalIf, ArithmeticIf, ElseIf, Else, EndIf, Do, EndDo, Cycle,
    Exit, Continue, Goto, ComputedGoto, AssignedGoto, Assign, SelectCase, Case, EndSelect, SelectType, TypeGuard,
    SelectRank, RankGuard, Where, WhereStmt, ElseWhere, EndWhere, Forall, ForallStmt, EndForall, Associate,
    EndAssociate, Block, EndBlock, Critical, EndCritical, ChangeTeam, EndTeam, Stop, ErrorStop, Return, Allocate,
    Deallocate, Nullify, Io, Pause, Sync, Lock, Event, FailImage, FormTeam, Print,
};

/** @brief The construct a block statement opens or closes, for messages ("END DO", "IF"). */
const char *StmtKindName(StmtKind k);

enum class RefCtx : uint8_t {
    Read,       // used in an expression
    Write,      // assigned (left of =, DO variable, READ item, ALLOCATE object, ...)
    Call,       // CALL name / name(...) known to be a call
    Keyword,    // name= inside an argument list (the dummy's name)
    Component,  // a%name
    Type,       // TYPE(name), CLASS(name), EXTENDS(name)
    Label,      // a statement label reference (GOTO 10)
    Module,     // USE name
    Construct,  // EXIT name, CYCLE name, END DO name
    Generic,    // a binding / generic spec name
};

struct NameRef {
    std::string name;  // lower case
    Pos pos, end;
    RefCtx ctx = RefCtx::Read;
    bool has_args = false;  // followed by (...)
    int args = -1;          // number of actual arguments when has_args
    int base = -1;          // for Component: index of the reference it hangs off
};

// Attributes of a declaration (bit set).
enum Attr : uint32_t {
    kAttrParameter = 1u << 0, kAttrAllocatable = 1u << 1, kAttrPointer = 1u << 2, kAttrTarget = 1u << 3,
    kAttrDimension = 1u << 4, kAttrIntentIn = 1u << 5, kAttrIntentOut = 1u << 6, kAttrOptional = 1u << 7,
    kAttrSave = 1u << 8, kAttrExternal = 1u << 9, kAttrIntrinsic = 1u << 10, kAttrPublic = 1u << 11,
    kAttrPrivate = 1u << 12, kAttrValue = 1u << 13, kAttrVolatile = 1u << 14, kAttrProtected = 1u << 15,
    kAttrBind = 1u << 16, kAttrContiguous = 1u << 17, kAttrAsynchronous = 1u << 18, kAttrCodimension = 1u << 19,
};

struct Entity {
    std::string name;  // lower case
    std::string spelling;
    Pos pos, end;
    bool array = false;  // has an array spec
    bool init = false;   // = or => initialiser
    std::string rename;  // USE: the module's name for it (local => rename)
};

struct UnitInfo {
    std::string name, spelling;
    Pos name_pos, name_end;
    std::vector<Entity> dummies;  // argument names ("*" for an alternate return)
    std::string result;           // RESULT(name), or the function name
    std::string type;             // function prefix type ("real(dp)"), or ""
    std::string prefix;           // pure/elemental/recursive/module ...
    std::string parent;           // SUBMODULE (parent)
    bool has_args = false;
};

struct Stmt {
    StmtKind kind = StmtKind::Unknown;
    std::vector<Token> toks;
    std::string label;  // statement label, or ""
    Pos label_pos;
    std::string construct;  // `name:` construct name, lower case, or ""
    Pos construct_pos;
    Pos pos, end;  // span of the statement's own text (after label)
    size_t first = 0;  // index of the first token after label and construct name

    // What the statement declares and refers to (filled by the parser).
    UnitInfo unit;                  // program units, interface/type names, END names
    std::string end_of;             // End: "subroutine", "do", ... ("" for a bare END)
    std::string type;               // TypeDecl/Procedure: "integer", "real(dp)", "type(foo)", "procedure(iface)"
    uint32_t attrs = 0;             // TypeDecl/Attribute
    std::vector<Entity> entities;   // declared names; USE only-list; COMMON/NAMELIST members
    std::string group;              // COMMON block / NAMELIST group / USE module (lower)
    bool use_only = false;          // USE ..., ONLY:
    bool use_intrinsic = false;     // USE, INTRINSIC
    std::vector<NameRef> refs;      // every name used, in order
    std::vector<std::string> label_refs;  // labels referenced (GOTO 10, DO 10, ERR=20, FMT=100)
    std::vector<Pos> label_ref_pos;
    std::string do_label;           // DO 10 I=...: the terminating label
    bool implicit_none = false;     // IMPLICIT NONE
    bool implicit_none_external = false;
    bool abstract = false;          // ABSTRACT INTERFACE
    bool generic_spec = false;      // INTERFACE OPERATOR(...) etc.
};

struct Diag {
    Pos pos, end;
    std::string message;
    std::string code;  // "syntax-error", ...
    int severity = 1;  // LSP: 1 error, 2 warning, 3 information, 4 hint
};

// Construct tree: a program unit, an interface block, a derived type, or
// an executable construct, with the statements it spans.
struct Construct {
    StmtKind kind = StmtKind::Unknown;  // the opening statement's kind
    int open = -1, close = -1;          // statement indices (close -1 if unterminated)
    int contains = -1;                  // CONTAINS statement index, or -1
    int parent = -1;
    std::vector<int> children;
};

struct Directive {
    int line = 0;
    std::string name;  // define, include, if, ...
    std::string text;
    bool active = true;
};

struct ReadOptions {
    Form form = Form::Free;
    bool preprocess = false;    // run the preprocessor model (else '#' lines are still skipped)
    int fixed_line_length = 72;  // fixed form's statement field ends here (0 = unlimited)
    int free_line_length = 0;    // free form: report lines longer than this (0 = unlimited)
};

struct Source {
    Form form = Form::Free;
    std::vector<Stmt> stmts;
    std::vector<Comment> comments;
    std::vector<Diag> diags;
    std::vector<Directive> directives;
    std::vector<Construct> constructs;  // [0] is the file
    std::vector<int> construct_of;      // per statement: innermost construct it belongs to
    std::map<std::string, std::string> macros;  // the file's #defines (name -> body), active branches
    bool unresolved_include = false;     // an INCLUDE line or #include (the reader doesn't open files)
    std::vector<int> inactive_lines;     // lines skipped by #if (0-based)
};

/** @brief Reads @p lines (no newlines): statements, structure, references and syntax errors. */
std::shared_ptr<Source> Read(const std::vector<std::string> &lines, const ReadOptions &opts);

/** @brief File bytes as UTF-8 text, the way gfortran reads them: a UTF-8 byte-order mark
 *  is dropped, and UTF-16/UTF-32 text (with its mark) is converted. */
std::string DecodeSource(std::string_view bytes);

/** @brief Splits text into lines (\n, \r\n). */
std::vector<std::string> SplitLines(std::string_view text);

constexpr int kMaxNesting = 400;  // parentheses / constructs deeper than this are refused

}  // namespace fortran

#endif  // MEP_FORTRAN_PARSE_H
