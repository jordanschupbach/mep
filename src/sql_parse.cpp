// SQLite's grammar (src/parse.y) as a recursive-descent parser: see
// sql_ast.h. Each function names the parse.y nonterminal it reads.
//
// The one rule that makes a hand-written reading of a Lemon grammar agree
// with Lemon on the error token is %fallback: a keyword in parse.y's
// fallback list is read as ID exactly where the parser state could shift
// an ID and has no action for the keyword itself. So every decision below
// tests the keywords that are explicit at that point *first*, and only
// then accepts a fallback keyword as a name (AtNm, AtIds, ...). That is
// why `a INTEGER GENERATED ALWAYS AS (1)` reads GENERATED ALWAYS as more
// type words (SQLite's own sqlite3AddColumn strips them back off), and
// why `SELECT raise FROM t` is an error near "FROM" while `SELECT key
// FROM t` is fine.
#include <cstring>
#include <stdexcept>
#include <utility>

#include "sql_ast.h"

namespace {

struct ParseFail {};

// Binding powers, parse.y's %left/%right lines from loosest to tightest.
enum Prec {
    kOr = 1,
    kAnd = 2,
    kNot = 3,
    kEq = 4,  // IS MATCH LIKE_KW BETWEEN IN ISNULL NOTNULL NE EQ
    kCmp = 5,  // GT LE LT GE
    kEscape = 6,
    kBit = 7,
    kAdd = 8,
    kMul = 9,
    kConcat = 10,
    kCollate = 11,
    kUnary = 12,
};

class Parser {
public:
    Parser(SqlParse *p, const SqlChunk &chunk, int index) : p_(p), chunk_index_(index) {
        for (int i = chunk.first_tok; i < chunk.end_tok; i++) {
            if (p_->tokens[static_cast<size_t>(i)].type != SqlTok::Comment &&
                p_->tokens[static_cast<size_t>(i)].type != SqlTok::Eof) {
                sig_.push_back(i);
            }
        }
        eof_tok_ = chunk.end_tok < static_cast<int>(p_->tokens.size()) ? chunk.end_tok : chunk.end_tok - 1;
    }

    /** @brief Parses every statement in the chunk; on a syntax error records it and stops. */
    void Run() {
        try {
            while (Cur() != SqlTok::Eof) {
                if (Cur() == SqlTok::Semi) {
                    pos_++;
                    continue;
                }
                partial_ = std::make_unique<SqlStmt>();
                ParseEcmd(partial_.get());
                p_->stmts.push_back(std::move(*partial_));
                partial_.reset();
            }
        } catch (const ParseFail &) {
            error_.chunk = chunk_index_;
            p_->errors.push_back(error_);
            if (partial_) {
                partial_->last = LastTok();
                partial_->broken = true;
                p_->stmts.push_back(std::move(*partial_));
            }
        }
    }

private:
    SqlParse *p_;
    int chunk_index_;
    std::vector<int> sig_;
    size_t pos_ = 0;
    int eof_tok_ = 0;
    SqlSyntaxError error_;
    std::unique_ptr<SqlStmt> partial_;

    // --- token access ---------------------------------------------------

    SqlTok TypeAt(size_t i) const {
        if (i >= sig_.size()) return SqlTok::Eof;
        return p_->tokens[static_cast<size_t>(sig_[i])].type;
    }
    SqlTok Cur() const { return TypeAt(pos_); }
    SqlTok Peek(size_t n = 1) const { return TypeAt(pos_ + n); }
    int Tok() const { return pos_ < sig_.size() ? sig_[pos_] : eof_tok_; }
    int LastTok() const { return pos_ > 0 ? sig_[pos_ - 1] : (sig_.empty() ? eof_tok_ : sig_[0]); }
    int Advance() {
        const int t = Tok();
        pos_++;
        return t;
    }
    bool Accept(SqlTok t) {
        if (Cur() != t) return false;
        pos_++;
        return true;
    }

    [[noreturn]] void Fail() {
        error_ = SqlSyntaxError();
        if (pos_ >= sig_.size()) {
            error_.message = "incomplete input";
            error_.tok = eof_tok_;
            error_.offset = -1;
        } else {
            const int t = sig_[pos_];
            const std::string text(p_->Text(t));
            error_.tok = t;
            error_.offset = p_->tokens[static_cast<size_t>(t)].start;
            if (p_->tokens[static_cast<size_t>(t)].type == SqlTok::Illegal) {
                error_.message = "unrecognized token: \"" + text + "\"";
            } else {
                error_.message = "near \"" + text + "\": syntax error";
            }
        }
        throw ParseFail();
    }
    [[noreturn]] void FailAction(int tok, const std::string &message) {
        error_ = SqlSyntaxError();
        error_.tok = tok;
        error_.offset = tok >= 0 ? p_->tokens[static_cast<size_t>(tok)].start : -1;
        error_.message = message;
        // sqlite3DequoteNumber's complaint is one of the three syntax messages, raised from an action.
        error_.syntax = message.rfind("unrecognized token", 0) == 0;
        if (error_.syntax) error_.offset = -1;
        throw ParseFail();
    }
    int Expect(SqlTok t) {
        if (Cur() != t) Fail();
        return Advance();
    }

    // --- token classes (parse.y's %token_class, plus %fallback) ---------

    static bool IsIdOrFallback(SqlTok t) { return t == SqlTok::Id || SqlIsFallback(t); }
    // id: ID|INDEXED
    bool AtId() const { return IsIdOrFallback(Cur()) || Cur() == SqlTok::Indexed; }
    // ids: ID|STRING
    bool AtIds() const { return IsIdOrFallback(Cur()) || Cur() == SqlTok::String; }
    // idj: ID|INDEXED|JOIN_KW
    static bool IsIdj(SqlTok t) { return IsIdOrFallback(t) || t == SqlTok::Indexed || t == SqlTok::JoinKw; }
    bool AtIdj() const { return IsIdj(Cur()); }
    // nm: idj|STRING
    static bool IsNm(SqlTok t) { return IsIdj(t) || t == SqlTok::String; }
    bool AtNm() const { return IsNm(Cur()); }

    SqlName NameAt(int tok) const {
        SqlName n;
        n.tok = tok;
        n.text = SqlDequote(p_->Text(tok));
        return n;
    }
    SqlName Nm() {
        if (!AtNm()) Fail();
        return NameAt(Advance());
    }
    SqlName Ids() {
        if (!AtIds()) Fail();
        return NameAt(Advance());
    }

    /** @brief dbnm after a nm: `nm [. nm]`, returning (db, name). */
    void FullName(SqlName *db, SqlName *name) {
        SqlName a = Nm();
        if (Accept(SqlTok::Dot)) {
            *db = std::move(a);
            *name = Nm();
        } else {
            *name = std::move(a);
        }
    }

    // --- statements -------------------------------------------------------

    /** @brief ecmd ::= [explain] cmdx SEMI. */
    void ParseEcmd(SqlStmt *st) {
        st->first = Tok();
        if (Cur() == SqlTok::Explain) {
            Advance();
            st->explain = 1;
            if (Cur() == SqlTok::Query) {
                Advance();
                Expect(SqlTok::Plan);
                st->explain = 2;
            }
        }
        ParseCmd(st);
        st->last = LastTok();
        if (Cur() == SqlTok::Semi) {
            st->last = Advance();
        } else if (Cur() != SqlTok::Eof) {
            Fail();
        }
    }

    void ParseCmd(SqlStmt *st) {
        switch (Cur()) {
            case SqlTok::Begin: {
                Advance();
                st->kind = SqlStmt::Begin;
                if (Cur() == SqlTok::Deferred || Cur() == SqlTok::Immediate || Cur() == SqlTok::Exclusive) Advance();
                TransOpt();
                return;
            }
            case SqlTok::Commit:
            case SqlTok::End:
                Advance();
                st->kind = SqlStmt::Commit;
                TransOpt();
                return;
            case SqlTok::Rollback:
                Advance();
                st->kind = SqlStmt::Rollback;
                TransOpt();
                if (Accept(SqlTok::To)) {
                    Accept(SqlTok::Savepoint);
                    st->name = Nm();
                }
                return;
            case SqlTok::Savepoint:
                Advance();
                st->kind = SqlStmt::Savepoint;
                st->name = Nm();
                return;
            case SqlTok::Release:
                Advance();
                st->kind = SqlStmt::Release;
                Accept(SqlTok::Savepoint);
                st->name = Nm();
                return;
            case SqlTok::Create:
                Advance();
                ParseCreate(st);
                return;
            case SqlTok::Drop: {
                Advance();
                switch (Cur()) {
                    case SqlTok::Table: st->kind = SqlStmt::DropTable; break;
                    case SqlTok::View: st->kind = SqlStmt::DropView; break;
                    case SqlTok::Index: st->kind = SqlStmt::DropIndex; break;
                    case SqlTok::Trigger: st->kind = SqlStmt::DropTrigger; break;
                    default: Fail();
                }
                Advance();
                if (Cur() == SqlTok::If) {
                    Advance();
                    Expect(SqlTok::Exists);
                    st->if_exists = true;
                }
                FullName(&st->db, &st->name);
                return;
            }
            case SqlTok::Select:
            case SqlTok::Values:
                st->kind = SqlStmt::Select;
                st->select = ParseSelect();
                return;
            case SqlTok::With: {
                std::unique_ptr<SqlWith> with = ParseWith();
                if (Cur() == SqlTok::Select || Cur() == SqlTok::Values) {
                    st->kind = SqlStmt::Select;
                    st->select = ParseSelectNoWith();
                    st->select->with = std::move(with);
                    st->select->first = st->select->with->first;
                    return;
                }
                st->with = std::move(with);
                ParseDml(st, false);
                return;
            }
            case SqlTok::Delete:
            case SqlTok::Update:
            case SqlTok::Insert:
            case SqlTok::Replace:
                ParseDml(st, false);
                return;
            case SqlTok::Vacuum:
                Advance();
                st->kind = SqlStmt::Vacuum;
                if (Cur() != SqlTok::Into && AtNm()) st->name = Nm();
                if (Accept(SqlTok::Into)) st->exprs.push_back(ParseExpr(kOr));
                return;
            case SqlTok::Pragma: {
                Advance();
                st->kind = SqlStmt::Pragma;
                FullName(&st->db, &st->name);
                if (Accept(SqlTok::Eq)) {
                    PragmaValue(st);
                } else if (Accept(SqlTok::LP)) {
                    PragmaValue(st);
                    Expect(SqlTok::RP);
                }
                return;
            }
            case SqlTok::Reindex:
            case SqlTok::Analyze:
                st->kind = Cur() == SqlTok::Reindex ? SqlStmt::Reindex : SqlStmt::Analyze;
                Advance();
                if (AtNm()) FullName(&st->db, &st->name);
                return;
            case SqlTok::Attach:
                Advance();
                st->kind = SqlStmt::Attach;
                Accept(SqlTok::Database);
                st->exprs.push_back(ParseExpr(kOr));
                Expect(SqlTok::As);
                st->exprs.push_back(ParseExpr(kOr));
                if (Accept(SqlTok::Key)) st->exprs.push_back(ParseExpr(kOr));
                return;
            case SqlTok::Detach:
                Advance();
                st->kind = SqlStmt::Detach;
                Accept(SqlTok::Database);
                st->exprs.push_back(ParseExpr(kOr));
                return;
            case SqlTok::Alter:
                Advance();
                ParseAlter(st);
                return;
            default:
                Fail();
        }
    }

    /** @brief trans_opt ::= [TRANSACTION [nm]]. */
    void TransOpt() {
        if (Accept(SqlTok::Transaction)) {
            if (AtNm()) Advance();
        }
    }

    /** @brief nmnum | minus_num, a PRAGMA's value. */
    void PragmaValue(SqlStmt *st) {
        st->value_first = Tok();
        if (Cur() == SqlTok::Minus || Cur() == SqlTok::Plus) {
            Advance();
            if (Cur() != SqlTok::Integer && Cur() != SqlTok::Float) Fail();
            st->value_last = Advance();
            return;
        }
        if (Cur() == SqlTok::Integer || Cur() == SqlTok::Float || Cur() == SqlTok::On || Cur() == SqlTok::Delete ||
            Cur() == SqlTok::Default || AtNm()) {
            st->value_last = Advance();
            return;
        }
        Fail();
    }

    void IfNotExists(SqlStmt *st) {
        if (Cur() == SqlTok::If) {
            Advance();
            Expect(SqlTok::Not);
            Expect(SqlTok::Exists);
            st->if_not_exists = true;
        }
    }

    void ParseCreate(SqlStmt *st) {
        if (Cur() == SqlTok::Temp) {
            Advance();
            st->temp = true;
            if (Cur() == SqlTok::Table) return CreateTable(st);
            if (Cur() == SqlTok::View) return CreateView(st);
            if (Cur() == SqlTok::Trigger) return CreateTrigger(st);
            Fail();
        }
        switch (Cur()) {
            case SqlTok::Table: return CreateTable(st);
            case SqlTok::View: return CreateView(st);
            case SqlTok::Trigger: return CreateTrigger(st);
            case SqlTok::Unique:
                Advance();
                st->unique = true;
                if (Cur() != SqlTok::Index) Fail();
                return CreateIndex(st);
            case SqlTok::Index: return CreateIndex(st);
            case SqlTok::Virtual: {
                Advance();
                Expect(SqlTok::Table);
                st->kind = SqlStmt::CreateVirtualTable;
                IfNotExists(st);
                FullName(&st->db, &st->name);
                Expect(SqlTok::Using);
                st->module = Nm();
                if (Accept(SqlTok::LP)) {
                    // vtabarglist: any tokens, parentheses balanced, split at top-level commas.
                    int depth = 0;
                    int arg_first = -1;
                    int arg_last = -1;
                    while (true) {
                        const SqlTok t = Cur();
                        if (t == SqlTok::Eof || t == SqlTok::Illegal) Fail();
                        if (depth == 0 && (t == SqlTok::RP || t == SqlTok::Comma)) {
                            st->module_args.emplace_back(arg_first, arg_last);
                            arg_first = arg_last = -1;
                            Advance();
                            if (t == SqlTok::RP) break;
                            continue;
                        }
                        if (t == SqlTok::LP) depth++;
                        if (t == SqlTok::RP) depth--;
                        const int tok = Advance();
                        if (arg_first < 0) arg_first = tok;
                        arg_last = tok;
                    }
                }
                return;
            }
            default:
                Fail();
        }
    }

    void CreateTable(SqlStmt *st) {
        Advance();  // TABLE
        st->kind = SqlStmt::CreateTable;
        IfNotExists(st);
        FullName(&st->db, &st->name);
        if (Accept(SqlTok::As)) {
            st->select = ParseSelect();
            return;
        }
        Expect(SqlTok::LP);
        // columnlist
        while (true) {
            SqlColumnDef col;
            ColumnName(&col);
            CargList(&col);
            col.last = LastTok();
            st->coldefs.push_back(std::move(col));
            if (Cur() != SqlTok::Comma) break;
            const SqlTok next = Peek();
            if (next == SqlTok::Constraint || next == SqlTok::Primary || next == SqlTok::Unique ||
                next == SqlTok::Check || next == SqlTok::Foreign) {
                Advance();  // the COMMA of conslist_opt
                ConsList(st);
                break;
            }
            Advance();
        }
        Expect(SqlTok::RP);
        // table_option_set
        if (Cur() == SqlTok::Without || AtNm()) {
            while (true) {
                // The action's "unknown table option" runs when the next token arrives; when that token is
                // itself a syntax error, Lemon reports the syntax error instead.
                auto unknown = [this](int tok, const SqlName &n) {
                    if (Cur() != SqlTok::Comma && Cur() != SqlTok::Semi && Cur() != SqlTok::Eof) Fail();
                    FailAction(tok, "unknown table option: " + std::string(p_->Text(n.tok)));
                };
                if (Cur() == SqlTok::Without) {
                    const int kw = Advance();
                    SqlName n = Nm();
                    if (Lower(n.text) == "rowid") {
                        st->without_rowid = true;
                    } else {
                        unknown(kw, n);
                    }
                } else {
                    SqlName n = Nm();
                    if (Lower(n.text) == "strict") {
                        st->strict = true;
                    } else {
                        unknown(n.tok, n);
                    }
                }
                if (!Accept(SqlTok::Comma)) break;
            }
        }
    }

    static std::string Lower(std::string s) {
        for (char &c : s) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return s;
    }

    /** @brief columnname ::= nm typetoken. */
    void ColumnName(SqlColumnDef *col) {
        col->first = Tok();
        col->name = Nm();
        TypeToken(&col->type, &col->type_first, &col->type_last);
        // sqlite3AddColumn: a type ending in GENERATED ALWAYS (the fallback read those words as type words)
        // is a generated column's.
        const std::string lower = Lower(col->type);
        const std::string suffix = "generated always";
        if (lower.size() >= suffix.size() && lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0) {
            col->type.resize(lower.size() - suffix.size());
            while (!col->type.empty() && col->type.back() == ' ') col->type.pop_back();
        }
    }

    /** @brief typetoken ::= [typename [LP signed RP | LP signed COMMA signed RP]]. */
    void TypeToken(std::string *type, int *first, int *last) {
        if (!AtIds()) return;
        *first = Tok();
        while (AtIds()) {
            if (!type->empty()) *type += ' ';
            *type += p_->Text(Advance());
        }
        if (Accept(SqlTok::LP)) {
            Signed();
            if (Accept(SqlTok::Comma)) Signed();
            Expect(SqlTok::RP);
        }
        *last = LastTok();
    }

    /** @brief signed ::= plus_num | minus_num. */
    void Signed() {
        if (Cur() == SqlTok::Plus || Cur() == SqlTok::Minus) Advance();
        if (Cur() != SqlTok::Integer && Cur() != SqlTok::Float) Fail();
        Advance();
    }

    void OnConf() {
        if (Cur() == SqlTok::On) {
            Advance();
            Expect(SqlTok::Conflict);
            ResolveType();
        }
    }
    void ResolveType() {
        switch (Cur()) {
            case SqlTok::Rollback: case SqlTok::Abort: case SqlTok::Fail: case SqlTok::Ignore: case SqlTok::Replace:
                Advance();
                return;
            default:
                Fail();
        }
    }
    void SortOrder() {
        if (Cur() == SqlTok::Asc || Cur() == SqlTok::Desc) Advance();
    }

    /** @brief carglist ::= ccons*. */
    void CargList(SqlColumnDef *col) {
        while (true) {
            switch (Cur()) {
                case SqlTok::Constraint:
                    Advance();
                    Nm();
                    break;
                case SqlTok::Default:
                    Advance();
                    if (Cur() == SqlTok::LP) {
                        Advance();
                        col->exprs.push_back(ParseExpr(kOr));
                        Expect(SqlTok::RP);
                    } else if (Cur() == SqlTok::Plus || Cur() == SqlTok::Minus) {
                        Advance();
                        Term();
                    } else if (IsTerm(Cur())) {
                        Term();
                    } else if (AtId()) {
                        Advance();
                    } else {
                        Fail();
                    }
                    break;
                case SqlTok::Null:
                    Advance();
                    OnConf();
                    break;
                case SqlTok::Not:
                    Advance();
                    if (Cur() == SqlTok::Null) {
                        Advance();
                        col->not_null = true;
                        OnConf();
                    } else if (Cur() == SqlTok::Deferrable) {
                        Advance();
                        InitDeferred();
                    } else {
                        Fail();
                    }
                    break;
                case SqlTok::Primary:
                    Advance();
                    Expect(SqlTok::Key);
                    col->primary_key = true;
                    SortOrder();
                    OnConf();
                    if (Accept(SqlTok::Autoincr)) col->autoincrement = true;
                    break;
                case SqlTok::Unique:
                    Advance();
                    col->unique = true;
                    OnConf();
                    break;
                case SqlTok::Check:
                    Advance();
                    Expect(SqlTok::LP);
                    col->exprs.push_back(ParseExpr(kOr));
                    Expect(SqlTok::RP);
                    break;
                case SqlTok::References:
                    Advance();
                    col->ref_table = Nm();
                    if (Cur() == SqlTok::LP) EidList(&col->ref_cols);
                    RefArgs();
                    break;
                case SqlTok::Deferrable:
                    Advance();
                    InitDeferred();
                    break;
                case SqlTok::Collate:
                    Advance();
                    col->collation = Ids();
                    break;
                case SqlTok::Generated:
                    Advance();
                    Expect(SqlTok::Always);
                    Expect(SqlTok::As);
                    Generated(col);
                    break;
                case SqlTok::As:
                    Advance();
                    Generated(col);
                    break;
                default:
                    return;
            }
        }
    }

    void Generated(SqlColumnDef *col) {
        Expect(SqlTok::LP);
        col->exprs.push_back(ParseExpr(kOr));
        Expect(SqlTok::RP);
        col->generated = true;
        if (IsIdOrFallback(Cur())) Advance();  // STORED / VIRTUAL
    }

    void InitDeferred() {
        if (Accept(SqlTok::Initially)) {
            if (Cur() != SqlTok::Deferred && Cur() != SqlTok::Immediate) Fail();
            Advance();
        }
    }

    /** @brief refargs ::= (MATCH nm | ON INSERT|DELETE|UPDATE refact)*. */
    void RefArgs() {
        while (true) {
            if (Cur() == SqlTok::Match) {
                Advance();
                Nm();
            } else if (Cur() == SqlTok::On) {
                Advance();
                if (Cur() != SqlTok::Insert && Cur() != SqlTok::Delete && Cur() != SqlTok::Update) Fail();
                Advance();
                // refact
                if (Cur() == SqlTok::Set) {
                    Advance();
                    if (Cur() != SqlTok::Null && Cur() != SqlTok::Default) Fail();
                    Advance();
                } else if (Cur() == SqlTok::Cascade || Cur() == SqlTok::Restrict) {
                    Advance();
                } else if (Cur() == SqlTok::No) {
                    Advance();
                    Expect(SqlTok::Action);
                } else {
                    Fail();
                }
            } else {
                return;
            }
        }
    }

    /** @brief conslist ::= tcons (tconscomma tcons)*. */
    void ConsList(SqlStmt *st) {
        while (true) {
            SqlTableConstraint tc;
            tc.first = Tok();
            switch (Cur()) {
                case SqlTok::Constraint:
                    Advance();
                    tc.kind = SqlTableConstraint::Name;
                    tc.name = Nm();
                    break;
                case SqlTok::Primary:
                    Advance();
                    Expect(SqlTok::Key);
                    Expect(SqlTok::LP);
                    tc.kind = SqlTableConstraint::PrimaryKey;
                    SortList(&tc.terms);
                    Accept(SqlTok::Autoincr);
                    Expect(SqlTok::RP);
                    OnConf();
                    break;
                case SqlTok::Unique:
                    Advance();
                    Expect(SqlTok::LP);
                    tc.kind = SqlTableConstraint::Unique;
                    SortList(&tc.terms);
                    Expect(SqlTok::RP);
                    OnConf();
                    break;
                case SqlTok::Check:
                    Advance();
                    Expect(SqlTok::LP);
                    tc.kind = SqlTableConstraint::Check;
                    tc.check = ParseExpr(kOr);
                    Expect(SqlTok::RP);
                    OnConf();
                    break;
                case SqlTok::Foreign:
                    Advance();
                    Expect(SqlTok::Key);
                    tc.kind = SqlTableConstraint::ForeignKey;
                    if (Cur() != SqlTok::LP) Fail();
                    EidList(&tc.cols);
                    Expect(SqlTok::References);
                    tc.ref_table = Nm();
                    if (Cur() == SqlTok::LP) EidList(&tc.ref_cols);
                    RefArgs();
                    // defer_subclause_opt
                    if (Cur() == SqlTok::Not) {
                        Advance();
                        Expect(SqlTok::Deferrable);
                        InitDeferred();
                    } else if (Cur() == SqlTok::Deferrable) {
                        Advance();
                        InitDeferred();
                    }
                    break;
                default:
                    Fail();
            }
            tc.last = LastTok();
            st->constraints.push_back(std::move(tc));
            if (Cur() == SqlTok::Comma) {
                Advance();
                continue;
            }
            const SqlTok t = Cur();
            if (t == SqlTok::Constraint || t == SqlTok::Primary || t == SqlTok::Unique || t == SqlTok::Check ||
                t == SqlTok::Foreign) {
                continue;
            }
            return;
        }
    }

    /** @brief eidlist_opt's LP eidlist RP: nm [COLLATE ids] [ASC|DESC], comma separated. */
    void EidList(std::vector<SqlName> *out) {
        Expect(SqlTok::LP);
        while (true) {
            out->push_back(Nm());
            if (Accept(SqlTok::Collate)) Ids();
            SortOrder();
            if (!Accept(SqlTok::Comma)) break;
        }
        Expect(SqlTok::RP);
    }

    /** @brief idlist ::= nm (COMMA nm)*. */
    void IdList(std::vector<SqlName> *out) {
        while (true) {
            out->push_back(Nm());
            if (!Accept(SqlTok::Comma)) break;
        }
    }

    void CreateView(SqlStmt *st) {
        Advance();
        st->kind = SqlStmt::CreateView;
        IfNotExists(st);
        FullName(&st->db, &st->name);
        if (Cur() == SqlTok::LP) EidList(&st->columns);
        Expect(SqlTok::As);
        st->select = ParseSelect();
    }

    void CreateIndex(SqlStmt *st) {
        Advance();  // INDEX
        st->kind = SqlStmt::CreateIndex;
        IfNotExists(st);
        FullName(&st->db, &st->name);
        Expect(SqlTok::On);
        st->on_table = Nm();
        Expect(SqlTok::LP);
        SortList(&st->index_terms);
        Expect(SqlTok::RP);
        if (Accept(SqlTok::Where)) st->where = ParseExpr(kOr);
    }

    void CreateTrigger(SqlStmt *st) {
        Advance();  // TRIGGER
        st->kind = SqlStmt::CreateTrigger;
        IfNotExists(st);
        FullName(&st->db, &st->name);
        // trigger_time
        if (Cur() == SqlTok::Before || Cur() == SqlTok::After) {
            st->trigger_time = Advance();
        } else if (Cur() == SqlTok::Instead) {
            st->trigger_time = Advance();
            Expect(SqlTok::Of);
        }
        // trigger_event
        if (Cur() == SqlTok::Delete || Cur() == SqlTok::Insert) {
            st->trigger_event = Advance();
        } else if (Cur() == SqlTok::Update) {
            st->trigger_event = Advance();
            if (Accept(SqlTok::Of)) IdList(&st->columns);
        } else {
            Fail();
        }
        Expect(SqlTok::On);
        FullName(&st->on_db, &st->on_table);
        if (Cur() == SqlTok::For) {
            Advance();
            Expect(SqlTok::Each);
            Expect(SqlTok::Row);
        }
        if (Accept(SqlTok::When)) st->when = ParseExpr(kOr);
        st->begin_tok = Expect(SqlTok::Begin);
        // trigger_cmd_list ::= (trigger_cmd SEMI)+
        do {
            SqlStmt cmd;
            cmd.first = Tok();
            TriggerCmd(&cmd);
            cmd.last = Expect(SqlTok::Semi);
            st->body.push_back(std::move(cmd));
        } while (Cur() != SqlTok::End);
        Advance();  // END
    }

    void TriggerCmd(SqlStmt *st) {
        switch (Cur()) {
            case SqlTok::Update:
            case SqlTok::Delete:
            case SqlTok::Insert:
            case SqlTok::Replace:
                ParseDml(st, true);
                return;
            case SqlTok::Select:
            case SqlTok::Values:
            case SqlTok::With:
                st->kind = SqlStmt::Select;
                st->select = ParseSelect();
                return;
            default:
                Fail();
        }
    }

    void ParseAlter(SqlStmt *st) {
        Expect(SqlTok::Table);
        st->kind = SqlStmt::AlterTable;
        FullName(&st->db, &st->name);
        switch (Cur()) {
            case SqlTok::Rename:
                Advance();
                if (Accept(SqlTok::To)) {
                    st->alter = SqlStmt::RenameTable;
                    st->new_name = Nm();
                    return;
                }
                Accept(SqlTok::ColumnKw);
                st->alter = SqlStmt::RenameColumn;
                st->column = Nm();
                Expect(SqlTok::To);
                st->new_name = Nm();
                return;
            case SqlTok::Add:
                Advance();
                if (Cur() == SqlTok::Constraint) {
                    Advance();
                    Nm();
                    Expect(SqlTok::Check);
                    AlterCheck(st);
                    return;
                }
                if (Cur() == SqlTok::Check) {
                    Advance();
                    AlterCheck(st);
                    return;
                }
                Accept(SqlTok::ColumnKw);
                st->alter = SqlStmt::AddColumn;
                st->add_column = std::make_unique<SqlColumnDef>();
                ColumnName(st->add_column.get());
                CargList(st->add_column.get());
                st->add_column->last = LastTok();
                st->column = st->add_column->name;
                return;
            case SqlTok::Drop:
                Advance();
                if (Cur() == SqlTok::Constraint) {
                    Advance();
                    st->alter = SqlStmt::OtherAlter;
                    Nm();
                    return;
                }
                Accept(SqlTok::ColumnKw);
                st->alter = SqlStmt::DropColumn;
                st->column = Nm();
                return;
            case SqlTok::Alter:
                Advance();
                Accept(SqlTok::ColumnKw);
                st->alter = SqlStmt::OtherAlter;
                st->column = Nm();
                if (Accept(SqlTok::Drop)) {
                    Expect(SqlTok::Not);
                    Expect(SqlTok::Null);
                    return;
                }
                Expect(SqlTok::Set);
                Expect(SqlTok::Not);
                Expect(SqlTok::Null);
                OnConf();
                return;
            default:
                Fail();
        }
    }

    void AlterCheck(SqlStmt *st) {
        st->alter = SqlStmt::OtherAlter;
        Expect(SqlTok::LP);
        st->exprs.push_back(ParseExpr(kOr));
        Expect(SqlTok::RP);
        OnConf();
    }

    /** @brief xfullname ::= nm [DOT nm] [AS nm]. */
    void XFullName(SqlStmt *st) {
        FullName(&st->db, &st->name);
        if (Accept(SqlTok::As)) st->alias = Nm();
    }

    /** @brief indexed_opt / tridxby. */
    void IndexedOpt() {
        if (Cur() == SqlTok::Indexed) {
            Advance();
            Expect(SqlTok::By);
            Nm();
        } else if (Cur() == SqlTok::Not) {
            Advance();
            Expect(SqlTok::Indexed);
        }
    }

    /** @brief The DELETE / UPDATE / INSERT commands, at top level (`with` prefix allowed) or in a trigger body. */
    void ParseDml(SqlStmt *st, bool in_trigger) {
        // Mark the statement as partial so a later error still leaves its name for the outline.
        switch (Cur()) {
            case SqlTok::Delete: {
                Advance();
                st->kind = SqlStmt::Delete;
                Expect(SqlTok::From);
                XFullName(st);
                IndexedOpt();
                if (in_trigger) {
                    if (Accept(SqlTok::Where)) st->where = ParseExpr(kOr);
                } else {
                    WhereOptRet(st);
                    OrderLimit(st);
                }
                return;
            }
            case SqlTok::Update: {
                Advance();
                st->kind = SqlStmt::Update;
                if (Cur() == SqlTok::Or) {
                    Advance();
                    st->insert_or = Tok();
                    ResolveType();
                }
                XFullName(st);
                IndexedOpt();
                Expect(SqlTok::Set);
                SetList(&st->set);
                if (Accept(SqlTok::From)) SelTabList(&st->from);
                if (in_trigger) {
                    if (Accept(SqlTok::Where)) st->where = ParseExpr(kOr);
                } else {
                    WhereOptRet(st);
                    OrderLimit(st);
                }
                return;
            }
            case SqlTok::Insert:
            case SqlTok::Replace: {
                st->kind = SqlStmt::Insert;
                if (Cur() == SqlTok::Replace) {
                    Advance();
                    st->replace = true;
                } else {
                    Advance();
                    if (Cur() == SqlTok::Or) {
                        Advance();
                        st->insert_or = Tok();
                        ResolveType();
                    }
                }
                Expect(SqlTok::Into);
                XFullName(st);
                if (Cur() == SqlTok::LP) {
                    Advance();
                    IdList(&st->columns);
                    Expect(SqlTok::RP);
                }
                if (!in_trigger && Cur() == SqlTok::Default) {
                    Advance();
                    Expect(SqlTok::Values);
                    st->default_values = true;
                    if (Accept(SqlTok::Returning)) SelColList(&st->returning);
                    return;
                }
                if (Cur() != SqlTok::Select && Cur() != SqlTok::Values && Cur() != SqlTok::With) Fail();
                st->select = ParseSelect();
                Upsert(st);
                return;
            }
            default:
                Fail();
        }
    }

    void WhereOptRet(SqlStmt *st) {
        if (Accept(SqlTok::Where)) st->where = ParseExpr(kOr);
        if (Accept(SqlTok::Returning)) SelColList(&st->returning);
    }

    void OrderLimit(SqlStmt *st) {
        if (Cur() == SqlTok::Order) {
            Advance();
            Expect(SqlTok::By);
            SortList(&st->order);
        }
        if (Accept(SqlTok::Limit)) {
            st->limit = ParseExpr(kOr);
            if (Accept(SqlTok::Offset) || Accept(SqlTok::Comma)) st->offset = ParseExpr(kOr);
        }
    }

    /** @brief setlist ::= (nm | LP idlist RP) EQ expr, comma separated. */
    void SetList(std::vector<SqlSetItem> *out) {
        while (true) {
            SqlSetItem item;
            if (Accept(SqlTok::LP)) {
                IdList(&item.cols);
                Expect(SqlTok::RP);
            } else {
                item.cols.push_back(Nm());
            }
            Expect(SqlTok::Eq);
            item.value = ParseExpr(kOr);
            out->push_back(std::move(item));
            if (!Accept(SqlTok::Comma)) break;
        }
    }

    /** @brief upsert, after INSERT's select. */
    void Upsert(SqlStmt *st) {
        while (true) {
            if (Accept(SqlTok::Returning)) {
                SelColList(&st->returning);
                return;
            }
            if (Cur() != SqlTok::On) return;
            SqlUpsert up;
            up.first = Advance();
            Expect(SqlTok::Conflict);
            bool targeted = false;
            if (Accept(SqlTok::LP)) {
                targeted = true;
                SortList(&up.target);
                Expect(SqlTok::RP);
                if (Accept(SqlTok::Where)) up.target_where = ParseExpr(kOr);
            }
            Expect(SqlTok::Do);
            if (Accept(SqlTok::Nothing)) {
            } else {
                Expect(SqlTok::Update);
                Expect(SqlTok::Set);
                up.do_update = true;
                SetList(&up.set);
                if (Accept(SqlTok::Where)) up.where = ParseExpr(kOr);
            }
            up.last = LastTok();
            st->upserts.push_back(std::move(up));
            if (!targeted) {
                // The last ON CONFLICT clause: only RETURNING may follow.
                if (Accept(SqlTok::Returning)) SelColList(&st->returning);
                return;
            }
        }
    }

    // --- SELECT -----------------------------------------------------------

    std::unique_ptr<SqlWith> ParseWith() {
        auto with = std::make_unique<SqlWith>();
        with->first = Expect(SqlTok::With);
        if (Accept(SqlTok::Recursive)) with->recursive = true;
        while (true) {
            SqlCte cte;
            cte.first = Tok();
            cte.name = Nm();
            if (Cur() == SqlTok::LP) EidList(&cte.columns);
            Expect(SqlTok::As);
            if (Cur() == SqlTok::Materialized) {
                Advance();
            } else if (Cur() == SqlTok::Not) {
                Advance();
                Expect(SqlTok::Materialized);
            }
            Expect(SqlTok::LP);
            cte.select = ParseSelect();
            Expect(SqlTok::RP);
            cte.last = LastTok();
            with->ctes.push_back(std::move(cte));
            if (!Accept(SqlTok::Comma)) break;
        }
        with->last = LastTok();
        return with;
    }

    /** @brief select ::= [WITH [RECURSIVE] wqlist] selectnowith. */
    SqlSelectP ParseSelect() {
        if (Cur() == SqlTok::With) {
            const int first = Tok();
            std::unique_ptr<SqlWith> with = ParseWith();
            SqlSelectP sel = ParseSelectNoWith();
            sel->with = std::move(with);
            sel->first = first;
            return sel;
        }
        return ParseSelectNoWith();
    }

    /** @brief selectnowith ::= oneselect (multiselect_op oneselect)*. */
    SqlSelectP ParseSelectNoWith() {
        auto sel = std::make_unique<SqlSelect>();
        sel->first = Tok();
        int op = -1;
        std::string pending;  // the multiselect action's error, raised once the next oneselect is read
        int pending_tok = -1;
        while (true) {
            SqlCore core;
            core.compound_op = op;
            OneSelect(&core);
            if (!pending.empty()) FailAction(pending_tok, pending);
            const bool order = !core.order.empty();
            const bool has_order_limit = order || core.limit;
            sel->cores.push_back(std::move(core));
            if (Cur() != SqlTok::Union && Cur() != SqlTok::Except && Cur() != SqlTok::Intersect) break;
            op = Advance();
            std::string op_name = p_->Upper(op);
            if (p_->tokens[static_cast<size_t>(op)].type == SqlTok::Union && Accept(SqlTok::All)) op_name += " ALL";
            if (has_order_limit) {
                pending = std::string(order ? "ORDER BY" : "LIMIT") + " clause should come after " + op_name +
                          " not before";
                pending_tok = op;
            }
        }
        sel->last = LastTok();
        return sel;
    }

    void OneSelect(SqlCore *core) {
        core->first = Tok();
        if (Cur() == SqlTok::Values) {
            core->is_values = true;
            Advance();
            while (true) {
                Expect(SqlTok::LP);
                std::vector<SqlExprP> row;
                NExprList(&row);
                Expect(SqlTok::RP);
                core->rows.push_back(std::move(row));
                if (!Accept(SqlTok::Comma)) break;
            }
            core->last = LastTok();
            return;
        }
        Expect(SqlTok::Select);
        if (Cur() == SqlTok::Distinct) {
            Advance();
            core->distinct = true;
        } else if (Cur() == SqlTok::All) {
            Advance();
        }
        SelColList(&core->cols);
        if (Accept(SqlTok::From)) {
            core->has_from = true;
            SelTabList(&core->from);
        }
        if (Accept(SqlTok::Where)) core->where = ParseExpr(kOr);
        if (Cur() == SqlTok::Group) {
            Advance();
            Expect(SqlTok::By);
            NExprList(&core->group);
        }
        if (Accept(SqlTok::Having)) core->having = ParseExpr(kOr);
        if (Accept(SqlTok::Window)) {
            while (true) {
                SqlWindowDef def;
                def.name = Nm();
                Expect(SqlTok::As);
                Expect(SqlTok::LP);
                Window(&def.spec);
                Expect(SqlTok::RP);
                core->windows.push_back(std::move(def));
                if (!Accept(SqlTok::Comma)) break;
            }
        }
        if (Cur() == SqlTok::Order) {
            Advance();
            Expect(SqlTok::By);
            SortList(&core->order);
        }
        if (Accept(SqlTok::Limit)) {
            core->limit = ParseExpr(kOr);
            if (Accept(SqlTok::Offset) || Accept(SqlTok::Comma)) core->offset = ParseExpr(kOr);
        }
        core->last = LastTok();
    }

    /** @brief selcollist ::= (expr [as] | STAR | nm DOT STAR), comma separated. */
    void SelColList(std::vector<SqlResultCol> *out) {
        while (true) {
            SqlResultCol col;
            col.first = Tok();
            if (Cur() == SqlTok::Star) {
                Advance();
                col.star = true;
            } else if (AtNm() && Peek() == SqlTok::Dot && Peek(2) == SqlTok::Star) {
                col.star_table = Nm();
                Advance();
                Advance();
                col.star = true;
            } else {
                col.expr = ParseExpr(kOr);
                As(&col.alias);
            }
            col.last = LastTok();
            out->push_back(std::move(col));
            if (!Accept(SqlTok::Comma)) break;
        }
    }

    /** @brief as ::= AS nm | ids | (nothing). */
    void As(SqlName *alias) {
        if (Accept(SqlTok::As)) {
            *alias = Nm();
        } else if (AtIds()) {
            *alias = NameAt(Advance());
        }
    }

    /** @brief seltablist: items joined by joinop. */
    void SelTabList(std::vector<SqlFromItem> *out) {
        int join_first = -1;
        int join_last = -1;
        bool comma = false;
        bool natural = false;
        bool outer_side = false;
        while (true) {
            SqlFromItem item;
            item.join_first = join_first;
            item.join_last = join_last;
            item.comma_join = comma;
            item.natural = natural;
            item.left_or_right = outer_side;
            SelTabItem(&item);
            out->push_back(std::move(item));
            // joinop
            comma = natural = outer_side = false;
            if (Cur() == SqlTok::Comma) {
                comma = true;
                join_first = join_last = Advance();
            } else if (Cur() == SqlTok::Join) {
                join_first = join_last = Advance();
            } else if (Cur() == SqlTok::JoinKw) {
                join_first = Tok();
                auto note = [&](int t) {
                    const std::string w = p_->Upper(t);
                    if (w == "NATURAL") natural = true;
                    if (w == "LEFT" || w == "RIGHT" || w == "FULL") outer_side = true;
                };
                note(Advance());
                if (Cur() != SqlTok::Join) {
                    if (!AtNm()) Fail();
                    note(Advance());
                    if (Cur() != SqlTok::Join) {
                        if (!AtNm()) Fail();
                        note(Advance());
                    }
                }
                join_last = Expect(SqlTok::Join);
                JoinType(join_first, join_last);
            } else {
                return;
            }
        }
    }

    /** @brief sqlite3JoinType's check of `JOIN_KW [nm [nm]] JOIN`, raised (as Lemon does) when the next token arrives. */
    void JoinType(int first, int join) {
        enum { kInner = 1, kCross = 2, kNatural = 4, kLeft = 8, kRight = 16, kOuter = 32, kError = 64 };
        int type = 0;
        std::vector<int> words;
        for (int t = first; t < join; t++) {
            const SqlTok ty = p_->tokens[static_cast<size_t>(t)].type;
            if (ty == SqlTok::Comment) continue;
            words.push_back(t);
            const std::string w = p_->Upper(t);
            if (w == "NATURAL") type |= kNatural;
            else if (w == "LEFT") type |= kLeft | kOuter;
            else if (w == "OUTER") type |= kOuter;
            else if (w == "RIGHT") type |= kRight | kOuter;
            else if (w == "FULL") type |= kLeft | kRight | kOuter;
            else if (w == "INNER") type |= kInner;
            else if (w == "CROSS") type |= kInner | kCross;
            else type |= kError;
        }
        const bool bad = (type & (kInner | kOuter)) == (kInner | kOuter) || (type & kError) != 0 ||
                         (type & (kOuter | kLeft | kRight)) == kOuter;
        if (!bad) return;
        if (Cur() != SqlTok::LP && !AtNm()) Fail();
        std::string msg = "unknown join type:";
        for (int t : words) msg += " " + std::string(p_->Text(t));
        FailAction(first, msg);
    }

    void OnUsing(SqlFromItem *item) {
        if (Accept(SqlTok::On)) {
            item->on = ParseExpr(kOr);
        } else if (Accept(SqlTok::Using)) {
            Expect(SqlTok::LP);
            IdList(&item->using_cols);
            Expect(SqlTok::RP);
        }
    }

    void SelTabItem(SqlFromItem *item) {
        item->first = Tok();
        if (Cur() == SqlTok::LP) {
            Advance();
            if (Cur() == SqlTok::Select || Cur() == SqlTok::Values || Cur() == SqlTok::With) {
                item->kind = SqlFromItem::Subquery;
                item->select = ParseSelect();
            } else {
                item->kind = SqlFromItem::Group;
                SelTabList(&item->group);
            }
            Expect(SqlTok::RP);
            As(&item->alias);
            OnUsing(item);
            item->last = LastTok();
            return;
        }
        FullName(&item->db, &item->name);
        if (Cur() == SqlTok::LP) {
            Advance();
            item->kind = SqlFromItem::TableFunc;
            if (Cur() != SqlTok::RP) NExprList(&item->args);
            Expect(SqlTok::RP);
            As(&item->alias);
            OnUsing(item);
            item->last = LastTok();
            return;
        }
        As(&item->alias);
        if (Cur() == SqlTok::Indexed) {
            Advance();
            Expect(SqlTok::By);
            item->indexed = Nm();
        } else if (Cur() == SqlTok::Not) {
            Advance();
            Expect(SqlTok::Indexed);
        }
        OnUsing(item);
        item->last = LastTok();
    }

    /** @brief sortlist ::= expr [ASC|DESC] [NULLS FIRST|LAST], comma separated. */
    void SortList(std::vector<SqlOrderTerm> *out) {
        while (true) {
            SqlOrderTerm term;
            term.expr = ParseExpr(kOr);
            SortOrder();
            if (Accept(SqlTok::Nulls)) {
                if (Cur() != SqlTok::First && Cur() != SqlTok::Last) Fail();
                Advance();
            }
            out->push_back(std::move(term));
            if (!Accept(SqlTok::Comma)) break;
        }
    }

    /** @brief nexprlist ::= expr (COMMA expr)*. */
    void NExprList(std::vector<SqlExprP> *out) {
        while (true) {
            out->push_back(ParseExpr(kOr));
            if (!Accept(SqlTok::Comma)) break;
        }
    }

    /** @brief window ::= [nm] [PARTITION BY nexprlist] [ORDER BY sortlist] frame_opt. */
    void Window(SqlWindowSpec *w) {
        w->first = Tok();
        const SqlTok t = Cur();
        if (t != SqlTok::Partition && t != SqlTok::Order && t != SqlTok::Range && t != SqlTok::Rows &&
            t != SqlTok::Groups && AtNm()) {
            w->base = Nm();
        }
        if (Cur() == SqlTok::Partition) {
            Advance();
            Expect(SqlTok::By);
            NExprList(&w->partition);
            if (Cur() == SqlTok::Order) {
                Advance();
                Expect(SqlTok::By);
                SortList(&w->order);
            }
        } else if (Cur() == SqlTok::Order) {
            Advance();
            Expect(SqlTok::By);
            SortList(&w->order);
        }
        // frame_opt
        if (Cur() == SqlTok::Range || Cur() == SqlTok::Rows || Cur() == SqlTok::Groups) {
            Advance();
            if (Accept(SqlTok::Between)) {
                FrameBound(w, true);
                Expect(SqlTok::And);
                FrameBound(w, false);
            } else {
                FrameBound(w, true);
            }
            if (Accept(SqlTok::Exclude)) {
                if (Cur() == SqlTok::No) {
                    Advance();
                    Expect(SqlTok::Others);
                } else if (Cur() == SqlTok::Current) {
                    Advance();
                    Expect(SqlTok::Row);
                } else if (Cur() == SqlTok::Group || Cur() == SqlTok::Ties) {
                    Advance();
                } else {
                    Fail();
                }
            }
        }
        w->last = LastTok();
    }

    void FrameBound(SqlWindowSpec *w, bool start) {
        if (Cur() == SqlTok::Unbounded) {
            Advance();
            Expect(start ? SqlTok::Preceding : SqlTok::Following);
            return;
        }
        if (Cur() == SqlTok::Current) {
            Advance();
            Expect(SqlTok::Row);
            return;
        }
        w->frame.push_back(ParseExpr(kOr));
        if (Cur() != SqlTok::Preceding && Cur() != SqlTok::Following) Fail();
        Advance();
    }

    // --- expressions ------------------------------------------------------

    static bool IsTerm(SqlTok t) {
        return t == SqlTok::Null || t == SqlTok::Float || t == SqlTok::Blob || t == SqlTok::String ||
               t == SqlTok::Integer || t == SqlTok::CtimeKw || t == SqlTok::QNumber;
    }

    void CheckQNumber(int tok) {
        // sqlite3DequoteNumber: every `_` must sit between two digits.
        // It dequotes in place, so the message shows the buffer half rewritten, as of the last bad `_`;
        // reproduced byte for byte.
        std::string buf(p_->Text(tok));
        buf.push_back('\0');
        const bool hex = buf.size() > 2 && buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X');
        auto digit = [hex](char c) {
            if (c >= '0' && c <= '9') return true;
            return hex && ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'));
        };
        std::string message;
        size_t out = 0;
        for (size_t in = 0; in < buf.size(); in++) {
            if (buf[in] != '_') {
                buf[out++] = buf[in];
                continue;
            }
            if (in == 0 || !digit(buf[in - 1]) || !digit(buf[in + 1])) {
                message = "unrecognized token: \"" + std::string(buf.c_str()) + "\"";
            }
        }
        if (!message.empty()) FailAction(tok, message);  // (one of the syntax messages: see FailAction)
    }

    SqlExprP Term() {
        auto e = std::make_unique<SqlExpr>();
        e->kind = SqlExpr::Literal;
        if (Cur() == SqlTok::QNumber) CheckQNumber(Tok());
        e->op = e->first = e->last = Advance();
        return e;
    }

    SqlExprP Make(SqlExpr::Kind kind, int first) {
        auto e = std::make_unique<SqlExpr>();
        e->kind = kind;
        e->first = first;
        return e;
    }

    SqlExprP Binary(SqlExprP lhs, int op, SqlExprP rhs) {
        auto e = Make(SqlExpr::Binary, lhs->first);
        e->op = op;
        e->last = rhs->last;
        e->args.push_back(std::move(lhs));
        e->args.push_back(std::move(rhs));
        return e;
    }

    /**
     * @brief expr, read Pratt-style with parse.y's precedences.
     * @param min_prec the loosest operator this call may consume
     * @param no_top_and BETWEEN's middle operand: a top-level AND belongs to the BETWEEN
     */
    SqlExprP ParseExpr(int min_prec, bool no_top_and = false) {
        SqlExprP lhs = ParsePrefix();
        while (true) {
            const SqlTok t = Cur();
            if (t == SqlTok::Or && min_prec <= kOr) {
                const int op = Advance();
                lhs = Binary(std::move(lhs), op, ParseExpr(kAnd));
            } else if (t == SqlTok::And && min_prec <= kAnd && !no_top_and) {
                const int op = Advance();
                lhs = Binary(std::move(lhs), op, ParseExpr(kNot));
            } else if (min_prec <= kEq &&
                       (t == SqlTok::Eq || t == SqlTok::Ne || t == SqlTok::Is || t == SqlTok::LikeKw ||
                        t == SqlTok::Match || t == SqlTok::Not || t == SqlTok::Between || t == SqlTok::In ||
                        t == SqlTok::IsNull || t == SqlTok::NotNull)) {
                lhs = ParseEqLevel(std::move(lhs));
            } else if (min_prec <= kCmp && (t == SqlTok::Lt || t == SqlTok::Le || t == SqlTok::Gt || t == SqlTok::Ge)) {
                const int op = Advance();
                lhs = Binary(std::move(lhs), op, ParseExpr(kCmp + 1));
            } else if (min_prec <= kBit &&
                       (t == SqlTok::BitAnd || t == SqlTok::BitOr || t == SqlTok::LShift || t == SqlTok::RShift)) {
                const int op = Advance();
                lhs = Binary(std::move(lhs), op, ParseExpr(kBit + 1));
            } else if (min_prec <= kAdd && (t == SqlTok::Plus || t == SqlTok::Minus)) {
                const int op = Advance();
                lhs = Binary(std::move(lhs), op, ParseExpr(kAdd + 1));
            } else if (min_prec <= kMul && (t == SqlTok::Star || t == SqlTok::Slash || t == SqlTok::Rem)) {
                const int op = Advance();
                lhs = Binary(std::move(lhs), op, ParseExpr(kMul + 1));
            } else if (min_prec <= kConcat && (t == SqlTok::Concat || t == SqlTok::Ptr)) {
                const int op = Advance();
                lhs = Binary(std::move(lhs), op, ParseExpr(kConcat + 1));
            } else if (min_prec <= kCollate && t == SqlTok::Collate) {
                auto e = Make(SqlExpr::Collate, lhs->first);
                e->op = Advance();
                e->collation = Ids();
                e->last = LastTok();
                e->args.push_back(std::move(lhs));
                lhs = std::move(e);
            } else {
                return lhs;
            }
        }
    }

    /** @brief The operators that share EQ's precedence: = != IS [NOT] LIKE/MATCH BETWEEN IN ISNULL NOTNULL NOT NULL. */
    SqlExprP ParseEqLevel(SqlExprP lhs) {
        const int first = lhs->first;
        SqlTok t = Cur();
        if (t == SqlTok::Eq || t == SqlTok::Ne) {
            const int op = Advance();
            return Binary(std::move(lhs), op, ParseExpr(kEq + 1));
        }
        if (t == SqlTok::IsNull || t == SqlTok::NotNull) {
            auto e = Make(SqlExpr::Postfix, first);
            e->op = Advance();
            e->last = e->op;
            e->args.push_back(std::move(lhs));
            return e;
        }
        if (t == SqlTok::Is) {
            auto e = Make(SqlExpr::Is, first);
            e->op = Advance();
            if (Accept(SqlTok::Not)) e->negated = true;
            if (Cur() == SqlTok::Distinct) {
                Advance();
                Expect(SqlTok::From);
                e->negated = !e->negated;  // IS DISTINCT FROM is IS NOT
            }
            e->args.push_back(std::move(lhs));
            e->args.push_back(ParseExpr(kEq + 1));
            e->last = e->args.back()->last;
            return e;
        }
        bool negated = false;
        int not_tok = -1;
        if (t == SqlTok::Not) {
            not_tok = Advance();
            t = Cur();
            if (t == SqlTok::Null) {
                auto e = Make(SqlExpr::Postfix, first);
                e->op = not_tok;
                e->last = Advance();
                e->args.push_back(std::move(lhs));
                return e;
            }
            if (t != SqlTok::LikeKw && t != SqlTok::Match && t != SqlTok::Between && t != SqlTok::In) Fail();
            negated = true;
        }
        if (t == SqlTok::LikeKw || t == SqlTok::Match) {
            auto e = Make(SqlExpr::Like, first);
            e->op = Advance();
            e->negated = negated;
            e->args.push_back(std::move(lhs));
            e->args.push_back(ParseExpr(kEq + 1));
            if (Accept(SqlTok::Escape)) e->args.push_back(ParseExpr(kEq + 1));
            e->last = e->args.back()->last;
            return e;
        }
        if (t == SqlTok::Between) {
            auto e = Make(SqlExpr::Between, first);
            e->op = Advance();
            e->negated = negated;
            e->args.push_back(std::move(lhs));
            e->args.push_back(ParseExpr(kOr, true));
            Expect(SqlTok::And);
            e->args.push_back(ParseExpr(kEq + 1));
            e->last = e->args.back()->last;
            return e;
        }
        // IN
        auto e = Make(SqlExpr::In, first);
        e->op = Expect(SqlTok::In);
        e->negated = negated;
        e->args.push_back(std::move(lhs));
        if (Accept(SqlTok::LP)) {
            if (Cur() == SqlTok::Select || Cur() == SqlTok::Values || Cur() == SqlTok::With) {
                e->select = ParseSelect();
            } else if (Cur() != SqlTok::RP) {
                NExprList(&e->args);
            }
            e->last = Expect(SqlTok::RP);
            return e;
        }
        SqlName a = Nm();
        if (Accept(SqlTok::Dot)) {
            e->in_db = std::move(a);
            e->in_table = Nm();
        } else {
            e->in_table = std::move(a);
        }
        if (Accept(SqlTok::LP)) {
            if (Cur() != SqlTok::RP) NExprList(&e->args);
            Expect(SqlTok::RP);
        }
        e->last = LastTok();
        return e;
    }

    SqlExprP ParsePrefix() {
        const SqlTok t = Cur();
        const int first = Tok();
        switch (t) {
            case SqlTok::Not: {
                auto e = Make(SqlExpr::Unary, first);
                e->op = Advance();
                e->args.push_back(ParseExpr(kNot + 1));
                e->last = e->args.back()->last;
                return e;
            }
            case SqlTok::BitNot:
            case SqlTok::Plus:
            case SqlTok::Minus: {
                auto e = Make(SqlExpr::Unary, first);
                e->op = Advance();
                e->args.push_back(ParseExpr(kUnary + 1));
                e->last = e->args.back()->last;
                return e;
            }
            case SqlTok::Null:
            case SqlTok::Float:
            case SqlTok::Blob:
            case SqlTok::Integer:
            case SqlTok::QNumber:
            case SqlTok::CtimeKw:
                return Term();
            case SqlTok::String:
                if (Peek() == SqlTok::Dot) return ColumnRef();
                return Term();
            case SqlTok::Variable: {
                // parse.y: `#N` names a register, legal only in SQLite's own nested parses.
                const std::string_view v = p_->Text(Tok());
                if (v.size() > 1 && v[0] == '#' && v[1] >= '0' && v[1] <= '9') {
                    error_ = SqlSyntaxError();
                    error_.tok = Tok();
                    // (sqlite3_error_offset() reports it inside CREATE/ALTER only: a query's resolver resets it.)
                    const SqlTok head = partial_ ? p_->tokens[static_cast<size_t>(partial_->first)].type : SqlTok::Eof;
                    error_.offset = head == SqlTok::Create || head == SqlTok::Alter
                                        ? p_->tokens[static_cast<size_t>(Tok())].start
                                        : -1;
                    error_.message = "near \"" + std::string(v) + "\": syntax error";
                    throw ParseFail();
                }
                auto e = Make(SqlExpr::Variable, first);
                e->op = e->last = Advance();
                return e;
            }
            case SqlTok::LP: {
                Advance();
                if (Cur() == SqlTok::Select || Cur() == SqlTok::Values || Cur() == SqlTok::With) {
                    auto e = Make(SqlExpr::Subquery, first);
                    e->select = ParseSelect();
                    e->last = Expect(SqlTok::RP);
                    return e;
                }
                auto e = Make(SqlExpr::Paren, first);
                NExprList(&e->args);
                if (e->args.size() > 1) e->kind = SqlExpr::Row;
                e->last = Expect(SqlTok::RP);
                return e;
            }
            case SqlTok::Cast: {
                auto e = Make(SqlExpr::Cast, first);
                e->op = Advance();
                Expect(SqlTok::LP);
                e->args.push_back(ParseExpr(kOr));
                Expect(SqlTok::As);
                int tf = -1;
                int tl = -1;
                TypeToken(&e->type, &tf, &tl);
                e->last = Expect(SqlTok::RP);
                return e;
            }
            case SqlTok::Case: {
                auto e = Make(SqlExpr::Case, first);
                e->op = Advance();
                if (Cur() != SqlTok::When) {
                    e->has_operand = true;
                    e->args.push_back(ParseExpr(kOr));
                }
                Expect(SqlTok::When);
                do {
                    e->args.push_back(ParseExpr(kOr));
                    Expect(SqlTok::Then);
                    e->args.push_back(ParseExpr(kOr));
                } while (Accept(SqlTok::When));
                if (Accept(SqlTok::Else)) {
                    e->has_else = true;
                    e->args.push_back(ParseExpr(kOr));
                }
                e->last = Expect(SqlTok::End);
                return e;
            }
            case SqlTok::Exists: {
                auto e = Make(SqlExpr::Exists, first);
                e->op = Advance();
                Expect(SqlTok::LP);
                e->select = ParseSelect();
                e->last = Expect(SqlTok::RP);
                return e;
            }
            case SqlTok::Raise: {
                auto e = Make(SqlExpr::Raise, first);
                e->op = Advance();
                Expect(SqlTok::LP);
                if (!Accept(SqlTok::Ignore)) {
                    if (Cur() != SqlTok::Rollback && Cur() != SqlTok::Abort && Cur() != SqlTok::Fail) Fail();
                    Advance();
                    Expect(SqlTok::Comma);
                    e->args.push_back(ParseExpr(kOr));
                }
                e->last = Expect(SqlTok::RP);
                return e;
            }
            default:
                break;
        }
        if (!AtIdj()) Fail();
        if (Peek() == SqlTok::LP) return FunctionCall();
        if (Peek() == SqlTok::Dot) return ColumnRef();
        auto e = Make(SqlExpr::Column, first);
        e->column = NameAt(Advance());
        e->last = e->first;
        return e;
    }

    /** @brief nm DOT nm [DOT nm]. */
    SqlExprP ColumnRef() {
        auto e = Make(SqlExpr::Column, Tok());
        SqlName a = Nm();
        Expect(SqlTok::Dot);
        SqlName b = Nm();
        if (Accept(SqlTok::Dot)) {
            e->db = std::move(a);
            e->table = std::move(b);
            e->column = Nm();
        } else {
            e->table = std::move(a);
            e->column = std::move(b);
        }
        e->last = LastTok();
        return e;
    }

    SqlExprP FunctionCall() {
        auto e = Make(SqlExpr::Func, Tok());
        e->func = NameAt(Advance());
        Expect(SqlTok::LP);
        if (Cur() == SqlTok::Star) {
            Advance();
            e->star = true;
            e->last = Expect(SqlTok::RP);
        } else {
            if (Cur() == SqlTok::Distinct) {
                Advance();
                e->distinct = true;
            } else if (Cur() == SqlTok::All) {
                Advance();
            }
            if (Cur() != SqlTok::RP && Cur() != SqlTok::Order) NExprList(&e->args);
            if (Cur() == SqlTok::Order) {
                // exprlist may be empty, so `f(ORDER BY x)` parses, as in parse.y.
                Advance();
                Expect(SqlTok::By);
                SortList(&e->order);
            }
            e->last = Expect(SqlTok::RP);
        }
        // filter_over
        if (Cur() == SqlTok::Filter) {
            Advance();
            Expect(SqlTok::LP);
            Expect(SqlTok::Where);
            e->filter = ParseExpr(kOr);
            e->last = Expect(SqlTok::RP);
        }
        if (Cur() == SqlTok::Over) {
            Advance();
            if (Accept(SqlTok::LP)) {
                e->over = std::make_unique<SqlWindowSpec>();
                Window(e->over.get());
                e->last = Expect(SqlTok::RP);
            } else {
                e->over_name = Nm();
                e->last = LastTok();
            }
        }
        return e;
    }
};

/** @brief sqlite3_complete()'s state machine over the token stream: the statement boundaries a script has. */
std::vector<SqlChunk> Chunks(const SqlParse &p) {
    enum { kSemi, kWs, kOther, kExplain, kCreate, kTemp, kTrigger, kEnd };
    static const unsigned char trans[8][8] = {
        {1, 0, 2, 3, 4, 2, 2, 2}, {1, 1, 2, 3, 4, 2, 2, 2}, {1, 2, 2, 2, 2, 2, 2, 2}, {1, 3, 3, 2, 4, 2, 2, 2},
        {1, 4, 2, 2, 2, 4, 5, 2}, {6, 5, 5, 5, 5, 5, 5, 5}, {6, 6, 5, 5, 5, 5, 5, 7}, {1, 7, 5, 5, 5, 5, 5, 5},
    };
    std::vector<SqlChunk> out;
    int state = 0;
    int first = 0;
    const int n = static_cast<int>(p.tokens.size()) - 1;  // without Eof
    for (int i = 0; i < n; i++) {
        const SqlToken &t = p.tokens[static_cast<size_t>(i)];
        int kind = kOther;
        if (t.type == SqlTok::Semi) {
            kind = kSemi;
        } else if (t.type == SqlTok::Comment) {
            kind = kWs;
        } else if (t.type == SqlTok::Id || SqlIsKeyword(t.type)) {
            const std::string w = p.Upper(i);
            if (w == "CREATE") kind = kCreate;
            else if (w == "TRIGGER") kind = kTrigger;
            else if (w == "TEMP" || w == "TEMPORARY") kind = kTemp;
            else if (w == "END") kind = kEnd;
            else if (w == "EXPLAIN") kind = kExplain;
        }
        state = trans[state][kind];
        if (state == 1 && kind == kSemi) {
            SqlChunk c;
            c.first_tok = first;
            c.end_tok = i + 1;
            out.push_back(c);
            first = i + 1;
        }
    }
    if (first < n || out.empty()) {
        SqlChunk c;
        c.first_tok = first;
        c.end_tok = n + 1;  // includes the Eof token
        out.push_back(c);
    }
    int prev_end = 0;
    for (SqlChunk &c : out) {
        c.start = prev_end;  // a statement owns the whitespace before it, as sqlite3_complete() cuts
        if (c.first_tok == c.end_tok) {
            c.end = prev_end;
            continue;
        }
        const SqlToken &last = p.tokens[static_cast<size_t>(c.end_tok - 1)];
        c.end = last.start + last.len;
        prev_end = c.end;
    }
    return out;
}

}  // namespace

SqlParse SqlParseScript(std::string_view text) {
    SqlParse p;
    p.text = std::string(text);
    p.tokens = SqlTokenize(p.text);
    p.chunks = Chunks(p);
    for (size_t i = 0; i < p.chunks.size(); i++) {
        Parser parser(&p, p.chunks[i], static_cast<int>(i));
        parser.Run();
    }
    return p;
}

bool SqlIsDotCommandLine(std::string_view line) { return !line.empty() && line[0] == '.'; }

bool SqlSettled(std::string_view text) {
    const SqlParse p = SqlParseScript(text);
    int last = -1;
    for (size_t i = 0; i + 1 < p.tokens.size(); i++) {
        if (p.tokens[i].type != SqlTok::Comment) last = static_cast<int>(i);
    }
    if (last < 0) return true;
    if (p.tokens[static_cast<size_t>(last)].type != SqlTok::Semi) return false;
    for (const SqlSyntaxError &e : p.errors) {
        if (e.message == "incomplete input") return false;
    }
    return true;
}

std::string SqlBlankDotCommands(std::string_view text) {
    std::string out(text);
    size_t pending = 0;  // where the SQL since the last dot-command starts
    size_t s = 0;
    while (s <= text.size()) {
        size_t e = text.find('\n', s);
        if (e == std::string_view::npos) e = text.size();
        if (SqlIsDotCommandLine(text.substr(s, e - s)) && SqlSettled(text.substr(pending, s - pending))) {
            for (size_t i = s; i < e; i++) out[i] = ' ';
            pending = e;
        }
        if (e == text.size()) break;
        s = e + 1;
    }
    return out;
}
