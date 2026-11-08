#pragma once

// The printer state shared by go_printer.cpp (printer.go) and
// go_printer_nodes.cpp (nodes.go). Internal to those two files.

#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "go_ast.h"
#include "go_printer.h"

namespace goprint {

constexpr int kMaxNewlines = 2;
constexpr int kInfinity = 1 << 30;

// whiteSpace
enum class WS : unsigned char {
    ignore = 0,
    blank = ' ',
    vtab = '\v',
    newline = '\n',
    formfeed = '\f',
    indent = '>',
    unindent = '<',
};

// pmode
enum class PM : int { none = 0, noExtraBlank = 1, noExtraLinebreak = 2, both = 3 };
constexpr int kNoExtraBlank = 1;
constexpr int kNoExtraLinebreak = 2;

class Printer {
public:
    struct TooDeep {};

    Printer(const Config &cfg, const LineTable &lt, std::unordered_map<const goast::Node *, int> *node_sizes,
            int depth);
    void PrintNode(goast::Node *node);
    /** @brief Flushes the rest and returns the text as the tabwriter and trimmer leave it. */
    std::string Finish();

private:
    // Recursion guard: expr1/stmt/nodeSize nest once per syntactic level.
    static constexpr int kMaxDepth = 4000;
    struct DepthGuard {
        Printer *p;
        explicit DepthGuard(Printer *pp) : p(pp) { p->Enter(); }
        ~DepthGuard() { p->depth_--; }
        DepthGuard(const DepthGuard &) = delete;
        DepthGuard &operator=(const DepthGuard &) = delete;
    };
    void Enter();

    struct CommentInfo {
        size_t cindex = 0;
        goast::CommentGroup *comment = nullptr;
        int comment_offset = 0;
        bool comment_newline = false;
    };

    // --- printer.go ---
    Position PosFor(goast::Pos p) const { return lt_.PositionFor(p); }
    int LineFor(goast::Pos p) const { return lt_.Line(p); }
    bool CommentsHaveNewline(const std::vector<goast::Comment *> &list);
    void NextComment();
    bool CommentBefore(const Position &next) const;
    int CommentSizeBefore(const Position &next);
    void RecordLine(int *line_ptr) { line_ptr_ = line_ptr; }
    int LinesFrom(int line) const { return out_.line - line; }
    void WriteIndent();
    void WriteByte(char ch, int n);
    void WriteString(const Position &pos, std::string_view s, bool is_lit);
    void WriteCommentPrefix(const Position &pos, const Position &next, const goast::Comment *prev, goast::Tok tok);
    void WriteComment(const goast::Comment *comment);
    std::pair<bool, bool> WriteCommentSuffix(bool needs_linebreak);
    bool ContainsLinebreak() const;
    std::pair<bool, bool> IntersperseComments(const Position &next, goast::Tok tok);
    void WriteWhitespace(size_t n);
    void SetPos(goast::Pos pos) {
        if (goast::IsValid(pos)) pos_ = PosFor(pos);
    }
    std::pair<bool, bool> Flush(const Position &next, goast::Tok tok);
    std::vector<goast::Comment *> FormatDocComment(const std::vector<goast::Comment *> &list);

    void RecordPrevOpen();
    void Item(PM m);
    void Item(WS x);
    void Item(const goast::Ident *x);
    void Item(const goast::BasicLit *x);
    void Item(goast::Tok x);
    void Item(std::string_view s);
    void Emit(std::string_view data, bool is_lit, bool implied_semi);
    template <class... A> void Print(const A &...a) { (Item(a), ...); }

    // --- nodes.go ---
    int Linebreak(int line, int min, WS ws, bool new_section);
    void SetComment(goast::CommentGroup *g);
    void IdentList(const std::vector<goast::Ident *> &list, bool indent);
    void ExprList(goast::Pos prev0, const std::vector<goast::Node *> &list, int depth, unsigned mode,
                  goast::Pos next0, bool is_incomplete);
    void Parameters(goast::FieldList *fields, int mode);
    void Signature(goast::FuncType *sig);
    bool IsOneLineFieldList(const std::vector<goast::Field *> &list);
    void SetLineComment(const std::string &text);
    void FieldListNode(goast::FieldList *fields, bool is_struct, bool is_incomplete);
    void BinaryExpr(goast::BinaryExpr *x, int prec1, int cutoff, int depth);
    void Expr1(goast::Node *expr, int prec1, int depth);
    bool PossibleSelectorExpr(goast::Node *expr, int prec1, int depth);
    bool SelectorExpr(goast::SelectorExpr *x, int depth, bool is_method);
    void Expr0(goast::Node *x, int depth) { Expr1(x, goast::LowestPrec, depth); }
    void Expr(goast::Node *x) { Expr1(x, goast::LowestPrec, 1); }
    void StmtList(const std::vector<goast::Node *> &list, int nindent, bool next_is_rbrace);
    void Block(goast::BlockStmt *b, int nindent);
    void ControlClause(bool is_for, goast::Node *init, goast::Node *expr, goast::Node *post);
    bool IndentList(const std::vector<goast::Node *> &list);
    void Stmt(goast::Node *stmt, bool next_is_rbrace);
    void ValueSpec(goast::ValueSpec *s, bool keep_type);
    void Spec(goast::Node *spec, int n, bool do_indent);
    void GenDecl(goast::GenDecl *d);
    int NodeSize(goast::Node *n, int max_size);
    int NumLines(goast::Node *n);
    int BodySize(goast::BlockStmt *b, int max_size);
    void FuncBody(int header_size, WS sep, goast::BlockStmt *b);
    int DistanceFrom(goast::Pos start_pos, int start_out_col);
    void FuncDecl(goast::FuncDecl *d);
    void Decl(goast::Node *decl);
    void DeclList(const std::vector<goast::Node *> &list);
    void FileNode(goast::File *src);
    goast::Node *StripParens(goast::Node *x);
    goast::BasicLit *SanitizeImportPath(goast::BasicLit *lit);

    Config cfg_;
    const LineTable &lt_;
    std::string output_;
    int indent_ = 0;
    int level_ = 0;
    int mode_ = 0;
    bool end_alignment_ = false;
    bool implied_semi_ = false;
    goast::Tok last_tok_ = goast::Tok::ILLEGAL;
    goast::Tok prev_open_ = goast::Tok::ILLEGAL;
    static constexpr size_t kWsCap = 16;
    std::vector<WS> wsbuf_;
    std::vector<int> go_build_, plus_build_;
    Position pos_, out_, last_;
    int *line_ptr_ = nullptr;
    std::vector<goast::CommentGroup *> comments_;
    bool use_node_comments_ = false;
    CommentInfo ci_;
    std::unordered_map<const goast::Node *, int> *node_sizes_;
    int depth_ = 0;
    goast::Arena scratch_;  // comments and literals the printer makes up
};

}  // namespace goprint
