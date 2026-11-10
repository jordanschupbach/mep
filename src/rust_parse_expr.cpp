// rust_parse_expr.cpp -- expression parsing: rustc_parse/src/parser/expr.rs
// plus the expression-related parts of diagnostics.rs.
//
// Ported from rustc_parse/rustc_lexer (rust-lang/rust 1.97.1, MIT OR
// Apache-2.0). The port follows rustc decision for decision; see the
// conventions in rust_parse_internal.h. Diagnostics keep only the primary
// message and span; feature gates, lints, notes and suggestions are dropped.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rust_parse_internal.h"
#include "uint128.h"

namespace rust {
namespace detail {

namespace {

// ---------------------------------------------------------------------------
// rustc_ast::util::parser::AssocOp

std::optional<AssocOp> assoc_op_from_token(const Token& t) {
    AssocOp op;
    op.span = t.span;
    auto bin = [&](BinOpKind k) {
        op.kind = AssocOp::Binary;
        op.bin = k;
        return std::optional<AssocOp>(op);
    };
    auto asg = [&](AssignOpKind k) {
        op.kind = AssocOp::AssignOp;
        op.assign = k;
        return std::optional<AssocOp>(op);
    };
    switch (t.kind) {
    case Tok::Eq: op.kind = AssocOp::Assign; return op;
    case Tok::Plus: return bin(BinOpKind::Add);
    case Tok::Minus: return bin(BinOpKind::Sub);
    case Tok::Star: return bin(BinOpKind::Mul);
    case Tok::Slash: return bin(BinOpKind::Div);
    case Tok::Percent: return bin(BinOpKind::Rem);
    case Tok::Caret: return bin(BinOpKind::BitXor);
    case Tok::And: return bin(BinOpKind::BitAnd);
    case Tok::Or: return bin(BinOpKind::BitOr);
    case Tok::Shl: return bin(BinOpKind::Shl);
    case Tok::Shr: return bin(BinOpKind::Shr);
    case Tok::PlusEq: return asg(AssignOpKind::AddAssign);
    case Tok::MinusEq: return asg(AssignOpKind::SubAssign);
    case Tok::StarEq: return asg(AssignOpKind::MulAssign);
    case Tok::SlashEq: return asg(AssignOpKind::DivAssign);
    case Tok::PercentEq: return asg(AssignOpKind::RemAssign);
    case Tok::CaretEq: return asg(AssignOpKind::BitXorAssign);
    case Tok::AndEq: return asg(AssignOpKind::BitAndAssign);
    case Tok::OrEq: return asg(AssignOpKind::BitOrAssign);
    case Tok::ShlEq: return asg(AssignOpKind::ShlAssign);
    case Tok::ShrEq: return asg(AssignOpKind::ShrAssign);
    case Tok::Lt: return bin(BinOpKind::Lt);
    case Tok::Le: return bin(BinOpKind::Le);
    case Tok::Ge: return bin(BinOpKind::Ge);
    case Tok::Gt: return bin(BinOpKind::Gt);
    case Tok::EqEq: return bin(BinOpKind::Eq);
    case Tok::Ne: return bin(BinOpKind::Ne);
    case Tok::AndAnd: return bin(BinOpKind::And);
    case Tok::OrOr: return bin(BinOpKind::Or);
    case Tok::DotDot:
        op.kind = AssocOp::Range;
        op.range = RangeLimits::HalfOpen;
        return op;
    case Tok::DotDotEq:
    case Tok::DotDotDot:
        op.kind = AssocOp::Range;
        op.range = RangeLimits::Closed;
        return op;
    case Tok::LArrow: return bin(BinOpKind::Lt);
    case Tok::Ident:
        if (!t.raw && t.kw == Kw::As) {
            op.kind = AssocOp::Cast;
            return op;
        }
        return std::nullopt;
    default: return std::nullopt;
    }
}

ExprPrecedence binop_precedence(BinOpKind k) {
    switch (k) {
    case BinOpKind::Mul:
    case BinOpKind::Div:
    case BinOpKind::Rem: return ExprPrecedence::Product;
    case BinOpKind::Add:
    case BinOpKind::Sub: return ExprPrecedence::Sum;
    case BinOpKind::Shl:
    case BinOpKind::Shr: return ExprPrecedence::Shift;
    case BinOpKind::BitAnd: return ExprPrecedence::BitAnd;
    case BinOpKind::BitXor: return ExprPrecedence::BitXor;
    case BinOpKind::BitOr: return ExprPrecedence::BitOr;
    case BinOpKind::Lt:
    case BinOpKind::Gt:
    case BinOpKind::Le:
    case BinOpKind::Ge:
    case BinOpKind::Eq:
    case BinOpKind::Ne: return ExprPrecedence::Compare;
    case BinOpKind::And: return ExprPrecedence::LAnd;
    case BinOpKind::Or: return ExprPrecedence::LOr;
    }
    return ExprPrecedence::Compare;
}

ExprPrecedence op_precedence(const AssocOp& op) {
    switch (op.kind) {
    case AssocOp::Binary: return binop_precedence(op.bin);
    case AssocOp::Cast: return ExprPrecedence::Cast;
    case AssocOp::Assign:
    case AssocOp::AssignOp: return ExprPrecedence::Assign;
    case AssocOp::Range: return ExprPrecedence::Range;
    }
    return ExprPrecedence::Assign;
}

bool binop_is_comparison(BinOpKind k) {
    return k == BinOpKind::Eq || k == BinOpKind::Lt || k == BinOpKind::Le || k == BinOpKind::Ne ||
           k == BinOpKind::Gt || k == BinOpKind::Ge;
}

bool op_is_comparison(const AssocOp& op) { return op.kind == AssocOp::Binary && binop_is_comparison(op.bin); }
bool op_is_assign_like(const AssocOp& op) { return op.kind == AssocOp::Assign || op.kind == AssocOp::AssignOp; }
bool op_is_binary(const AssocOp& op, BinOpKind k) { return op.kind == AssocOp::Binary && op.bin == k; }

enum class Fixity { Left, Right, None };
Fixity op_fixity(const AssocOp& op) {
    switch (op.kind) {
    case AssocOp::Assign:
    case AssocOp::AssignOp: return Fixity::Right;
    case AssocOp::Binary: return binop_is_comparison(op.bin) ? Fixity::None : Fixity::Left;
    case AssocOp::Cast: return Fixity::Left;
    case AssocOp::Range: return Fixity::None;
    }
    return Fixity::Left;
}

bool op_can_continue_expr_unambiguously(const AssocOp& op) {
    switch (op.kind) {
    case AssocOp::Assign:
    case AssocOp::AssignOp:
    case AssocOp::Cast: return true;
    case AssocOp::Binary:
        return op.bin == BinOpKind::BitXor || op.bin == BinOpKind::Div || op.bin == BinOpKind::Rem ||
               op.bin == BinOpKind::Shr || op.bin == BinOpKind::Le || op.bin == BinOpKind::Gt ||
               op.bin == BinOpKind::Ge;
    case AssocOp::Range: return false;
    }
    return false;
}

Restrictions res_minus(Restrictions r, Restrictions x) { return static_cast<Restrictions>(r & ~x); }
bool res_contains(Restrictions r, Restrictions x) { return (r & x) == x; }

/// classify::expr_is_complete
bool classify_expr_is_complete(const Expr* e) {
    switch (e->kind) {
    case ExprKind::If:
    case ExprKind::Match:
    case ExprKind::Block:
    case ExprKind::While:
    case ExprKind::Loop:
    case ExprKind::ForLoop:
    case ExprKind::TryBlock:
    case ExprKind::ConstBlock: return true;
    default: return false;
    }
}

/// classify::expr_requires_semi_to_be_stmt
bool expr_requires_semi_to_be_stmt(const Expr* e) {
    if (e->kind == ExprKind::MacCall) return e->mac == nullptr || e->mac->args.delim != DelimKind::Brace;
    return !classify_expr_is_complete(e);
}

/// prec_let_scrutinee_needs_par
constexpr ExprPrecedence kPrecLetScrutineeNeedsPar = ExprPrecedence::LAnd;

/// Pat::could_be_never_pattern
bool pat_could_be_never_pattern(const Pat* p);
void pat_walk_never(const Pat* p, bool& out) {
    if (p == nullptr) return;
    switch (p->kind) {
    case PatKind::Never:
    case PatKind::MacCall: out = true; return;
    case PatKind::Or: {
        bool all = true;
        for (const Pat* q : p->elems)
            if (!pat_could_be_never_pattern(q)) all = false;
        out = all;
        return;
    }
    case PatKind::Ident: pat_walk_never(p->sub, out); return;
    case PatKind::Struct:
        for (const PatField& f : p->fields) pat_walk_never(f.pat, out);
        return;
    case PatKind::TupleStruct:
    case PatKind::Tuple:
    case PatKind::Slice:
        for (const Pat* q : p->elems) pat_walk_never(q, out);
        return;
    case PatKind::Box:
    case PatKind::Deref:
    case PatKind::Ref:
    case PatKind::Paren:
    case PatKind::Guard: pat_walk_never(p->sub, out); return;
    default: return;
    }
}
bool pat_could_be_never_pattern(const Pat* p) {
    bool out = false;
    pat_walk_never(p, out);
    return out;
}

/// rustc_literal_escaper::unescape_char(..).is_ok() for a lifetime's name:
/// a lifetime name never holds a backslash or a quote, so this is "exactly
/// one character".
bool unescape_char_ok(std::string_view s) {
    if (s.empty()) return false;
    size_t len = 0;
    uint32_t c = decode_utf8(s, 0, len);
    if (c == '\\') return false;  // escapes never appear in a lifetime name
    if (c == '\n' || c == '\t' || c == '\'' || c == '\r') return false;
    return len == s.size();
}

bool could_be_unclosed_char_literal(const Ident& ident) {
    return !ident.name.empty() && ident.name[0] == '\'' && unescape_char_ok(ident.name.substr(1));
}

bool path_is_single_kw(const Path* p, std::string_view name) {
    return p != nullptr && p->segments.size() == 1 && p->segments[0].ident.name == name;
}

/// LitKind::descr
const char* lit_kind_descr(LitKind k) {
    switch (k) {
    case LitKind::Bool: return "boolean";
    case LitKind::Byte: return "byte";
    case LitKind::Char: return "char";
    case LitKind::Integer: return "integer";
    case LitKind::Float: return "float";
    case LitKind::Str:
    case LitKind::StrRaw: return "string";
    case LitKind::ByteStr:
    case LitKind::ByteStrRaw: return "byte string";
    case LitKind::CStr:
    case LitKind::CStrRaw: return "C string";
    case LitKind::Err: return "error";
    }
    return "error";
}

bool is_int_suffix(std::string_view s) {
    return s == "isize" || s == "i8" || s == "i16" || s == "i32" || s == "i64" || s == "i128" || s == "usize" ||
           s == "u8" || s == "u16" || s == "u32" || s == "u64" || s == "u128";
}
bool is_float_suffix(std::string_view s) { return s == "f16" || s == "f32" || s == "f64" || s == "f128"; }

bool looks_like_width_suffix(std::string_view first_chars, std::string_view s) {
    if (s.size() <= 1 || first_chars.find(s[0]) == std::string_view::npos) return false;
    for (size_t i = 1; i < s.size(); ++i)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

/// Does the integer literal text overflow u128?
bool int_lit_too_large(std::string_view sym) {
    unsigned base = 10;
    size_t i = 0;
    if (sym.size() >= 2 && sym[0] == '0') {
        if (sym[1] == 'x') base = 16, i = 2;
        else if (sym[1] == 'o') base = 8, i = 2;
        else if (sym[1] == 'b') base = 2, i = 2;
    }
    mep::u128 v = 0;
    mep::u128 max = ~static_cast<mep::u128>(0);
    for (; i < sym.size(); ++i) {
        char c = sym[i];
        if (c == '_') continue;
        unsigned d;
        if (c >= '0' && c <= '9')
            d = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f')
            d = static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            d = static_cast<unsigned>(c - 'A' + 10);
        else
            return false;
        if (d >= base) return false;
        if (v > (max - d) / base) return true;
        v = v * base + d;
    }
    return false;
}

/// MetaItemLit::from_token_lit failing -> the message report_lit_error prints.
std::optional<std::string> meta_lit_error(const Lit& lit) {
    std::string_view suf = lit.suffix;
    switch (lit.kind) {
    case LitKind::Err: return std::nullopt;
    case LitKind::Integer: {
        unsigned base = 10;
        if (lit.symbol.size() >= 2 && lit.symbol[0] == '0') {
            if (lit.symbol[1] == 'x') base = 16;
            else if (lit.symbol[1] == 'o') base = 8;
            else if (lit.symbol[1] == 'b') base = 2;
        }
        if (!suf.empty()) {
            if (is_float_suffix(suf)) {
                if (base != 10) {
                    if (base == 16) return std::string("hexadecimal float literal is not supported");
                    if (base == 8) return std::string("octal float literal is not supported");
                    return std::string("binary float literal is not supported");
                }
                return std::nullopt;
            }
            if (!is_int_suffix(suf)) {
                std::string s(suf);
                if (looks_like_width_suffix("iu", s)) return "invalid width `" + s.substr(1) + "` for integer literal";
                if (lit.symbol == "0" && (s[0] == 'B' || s[0] == 'O' || s[0] == 'X')) {
                    unsigned b = s[0] == 'B' ? 2 : s[0] == 'O' ? 8 : 16;
                    bool valid = true;
                    for (size_t i = 1; i < s.size(); ++i) {
                        char c = s[i];
                        if (c == '_') continue;
                        if (c == 'i' || c == 'u') break;
                        unsigned d = (c >= '0' && c <= '9')   ? static_cast<unsigned>(c - '0')
                                     : (c >= 'a' && c <= 'f') ? static_cast<unsigned>(c - 'a' + 10)
                                     : (c >= 'A' && c <= 'F') ? static_cast<unsigned>(c - 'A' + 10)
                                                              : 99u;
                        if (d >= b) {
                            valid = false;
                            break;
                        }
                    }
                    if (valid) return std::string("invalid base prefix for number literal");
                }
                return "invalid suffix `" + s + "` for number literal";
            }
        }
        if (int_lit_too_large(lit.symbol)) return std::string("integer literal is too large");
        return std::nullopt;
    }
    case LitKind::Float:
        if (!suf.empty() && !is_float_suffix(suf)) {
            std::string s(suf);
            if (looks_like_width_suffix("f", s)) return "invalid width `" + s.substr(1) + "` for float literal";
            return "invalid suffix `" + s + "` for float literal";
        }
        return std::nullopt;
    default:
        if (!suf.empty()) return std::string("suffixes on ") + lit_kind_descr(lit.kind) + " literals are invalid";
        return std::nullopt;
    }
}

Lit lit_from_token(const Token& t) {
    Lit l;
    l.kind = t.lit;
    l.symbol = t.sym;
    l.suffix = t.suffix;
    l.n_hashes = t.n_hashes;
    l.span = t.span;
    return l;
}

Path* path_from_ident(Arena& arena, Ident ident) {
    Path* p = arena.make<Path>();
    p->span = ident.span;
    PathSegment seg;
    seg.ident = ident;
    p->segments.push_back(seg);
    return p;
}

// ---------------------------------------------------------------------------
// CondChecker: the let-chain checks of parse_expr_cond / match guards.

enum class ForbidLet : uint8_t { None, NotSupportedOr, NotSupportedParentheses, OtherForbidden };

struct CondChecker {
    Parser& parser;
    LetChainsPolicy policy;
    uint32_t depth = 0;
    ForbidLet forbid = ForbidLet::None;
    Span or_span;
    bool missing_let = false;
    bool found_incorrect_let_chain = false;

    CondChecker(Parser& p, LetChainsPolicy pol) : parser(p), policy(pol) {}

    void walk(Expr* e) {
        // mut_visit::walk_expr over the sub-expressions a `let` can reach.
        if (e->lhs) visit(e->lhs);
        for (Expr* x : e->exprs) visit(x);
        if (e->rhs) visit(e->rhs);
    }

    void with_forbid(ForbidLet f, Expr* e, bool whole) {
        ForbidLet saved = forbid;
        forbid = f;
        if (whole)
            walk(e);
        else if (e->lhs)
            visit(e->lhs);
        forbid = saved;
    }

    static bool find_let_some(const Expr* e) {
        if (e->kind == ExprKind::Let) return true;
        if (e->kind == ExprKind::Binary && e->binop == BinOpKind::And)
            return find_let_some(e->lhs) || find_let_some(e->rhs);
        return false;
    }
    static const Expr* find_let_some_expr(const Expr* e) {
        if (e->kind == ExprKind::Let) return e;
        if (e->kind == ExprKind::Binary && e->binop == BinOpKind::And) {
            const Expr* l = find_let_some_expr(e->lhs);
            return l ? l : find_let_some_expr(e->rhs);
        }
        return nullptr;
    }

    void visit(Expr* e) {
        Parser::DepthGuard g(parser);
        ++depth;
        Span span = e->span;
        switch (e->kind) {
        case ExprKind::Let:
            if (e->recovered) break;
            if (forbid != ForbidLet::None) {
                if (forbid == ForbidLet::NotSupportedOr) {
                    parser.emit("`||` operators are not supported in let chain conditions", or_span);
                } else {
                    parser.emit("expected expression, found `let` statement", span);
                    if (missing_let) found_incorrect_let_chain = true;
                }
                e->recovered = true;
            } else if (depth > 1) {
                if (policy == LetChainsPolicy::EditionDependent && parser.edition_ < Edition::E2024)
                    parser.emit("let chains are only allowed in Rust 2024 or later", span);
            }
            break;
        case ExprKind::Binary:
            if (e->binop == BinOpKind::And) {
                walk(e);
            } else if (e->binop == BinOpKind::Or &&
                       (forbid == ForbidLet::None || forbid == ForbidLet::NotSupportedOr)) {
                ForbidLet saved = forbid;
                Span saved_or = or_span;
                forbid = ForbidLet::NotSupportedOr;
                or_span = e->op_span;
                walk(e);
                forbid = saved;
                or_span = saved_or;
            } else {
                with_forbid(ForbidLet::OtherForbidden, e, true);
            }
            break;
        case ExprKind::Paren:
            if (forbid == ForbidLet::None || forbid == ForbidLet::NotSupportedParentheses)
                with_forbid(ForbidLet::NotSupportedParentheses, e, true);
            else
                with_forbid(ForbidLet::OtherForbidden, e, true);
            break;
        case ExprKind::Assign: {
            Expr* lhs = e->lhs;
            Expr* rhs = e->rhs;
            if (lhs->kind == ExprKind::Call) {
                // get_path_from_rhs
                uint32_t d = 0;
                const Expr* cur = rhs;
                const Path* path = nullptr;
                while (true) {
                    if (cur->kind == ExprKind::Binary) {
                        cur = cur->lhs;
                        ++d;
                    } else if (cur->kind == ExprKind::Path) {
                        path = cur->path;
                        break;
                    } else {
                        break;
                    }
                }
                if (path != nullptr) {
                    if (find_let_some_expr(rhs) != nullptr && d > 0) {
                        parser.emit("let-chain with missing `let`", lhs->span);
                        found_incorrect_let_chain = true;
                    }
                }
            }
            ForbidLet saved = forbid;
            forbid = ForbidLet::OtherForbidden;
            bool saved_missing = missing_let;
            if (lhs->kind == ExprKind::Binary) {
                ExprKind rk = lhs->rhs->kind;
                if (rk == ExprKind::Path || rk == ExprKind::Struct || rk == ExprKind::Call || rk == ExprKind::Array)
                    missing_let = true;
            }
            walk(e);
            forbid = saved;
            missing_let = saved_missing;
            break;
        }
        case ExprKind::Unary:
        case ExprKind::Await:
        case ExprKind::Move:
        case ExprKind::Use:
        case ExprKind::AssignOp:
        case ExprKind::Range:
        case ExprKind::Try:
        case ExprKind::AddrOf:
        case ExprKind::Field:
        case ExprKind::Index:
        case ExprKind::Call:
        case ExprKind::MethodCall:
        case ExprKind::Tup: with_forbid(ForbidLet::OtherForbidden, e, true); break;
        case ExprKind::Cast:
        case ExprKind::Type:
        case ExprKind::UnsafeBinderCast: with_forbid(ForbidLet::OtherForbidden, e, false); break;
        default: break;
        }
        --depth;
    }
};

}  // namespace

// ===========================================================================
// expr.rs

Expr* Parser::parse_expr() {
    st_.has_current_closure = false;
    AttrWrapper attrs = parse_outer_attributes();
    return parse_expr_res(R_EMPTY, std::move(attrs)).first;
}

Expr* Parser::parse_expr_force_collect() {
    st_.has_current_closure = false;
    AttrWrapper attrs = parse_outer_attributes();
    return parse_expr_res(R_EMPTY, std::move(attrs)).first;
}

AnonConst Parser::parse_expr_anon_const(bool direct) {
    AnonConst c;
    c.value = parse_expr();
    c.direct = direct;
    return c;
}

Expr* Parser::parse_expr_catch_underscore(Restrictions restrictions) {
    AttrWrapper attrs = parse_outer_attributes();
    try {
        return parse_expr_res(restrictions, std::move(attrs)).first;
    } catch (PErr& err) {
        if (token.kind == Tok::Ident && !token.raw && token.kw == Kw::Underscore && may_recover() &&
            look_ahead_tok(1).kind == Tok::Comma) {
            emit(err);
            bump();
            return mk_expr_err(prev_token.span);
        }
        throw;
    }
}

std::vector<Expr*> Parser::parse_expr_paren_seq() {
    return parse_paren_comma_seq<Expr*>([this] { return parse_expr_catch_underscore(R_EMPTY); });
}

std::pair<Expr*, bool> Parser::parse_expr_res(Restrictions r, AttrWrapper attrs) {
    return with_res(r, [&] { return parse_expr_assoc_with(PrecBound::unbounded(), std::move(attrs)); });
}

std::pair<Expr*, bool> Parser::parse_expr_assoc_with(PrecBound min_prec, AttrWrapper attrs) {
    DepthGuard g(*this);
    Expr* lhs;
    if (tok_is_range_separator(token)) {
        return {parse_expr_prefix_range(std::move(attrs)), false};
    } else {
        lhs = parse_expr_prefix(std::move(attrs));
    }
    return parse_expr_assoc_rest_with(min_prec, false, lhs);
}

std::pair<Expr*, bool> Parser::parse_expr_assoc_rest_with(PrecBound min_prec, bool starts_stmt, Expr* lhs) {
    bool parsed_something = false;
    if (!should_continue_as_assoc_expr(lhs)) return {lhs, parsed_something};
    st_.expected_token_types.insert(TokenType::Operator);
    while (true) {
        std::optional<AssocOp> opt = check_assoc_op();
        if (!opt) break;
        AssocOp op = *opt;
        Span lhs_span = lhs->span;
        Span cur_op_span = token.span;
        Restrictions restrictions =
            op_is_assign_like(op) ? static_cast<Restrictions>(st_.restrictions & R_NO_STRUCT_LITERAL) : st_.restrictions;
        ExprPrecedence prec = op_precedence(op);
        bool stop = false;
        switch (min_prec.kind) {
        case PrecBound::Included: stop = prec < min_prec.prec; break;
        case PrecBound::Excluded: stop = prec <= min_prec.prec; break;
        case PrecBound::Unbounded: stop = false; break;
        }
        if (stop) break;
        if (token.kind == Tok::DotDotDot && op.kind == AssocOp::Range && op.range == RangeLimits::Closed)
            err_dotdotdot_syntax(token.span);
        if (token.kind == Tok::LArrow) err_larrow_operator(token.span);
        parsed_something = true;
        bump();
        if (op_is_comparison(op)) {
            if (Expr* e = check_no_chained_comparison(lhs, op)) return {e, parsed_something};
        }
        if (op.kind == AssocOp::Binary && (op.bin == BinOpKind::Eq || op.bin == BinOpKind::Ne) &&
            token.kind == Tok::Eq && prev_token.span.hi == token.span.lo) {
            Span sp = op.span.to(token.span);
            emit(std::string("invalid comparison operator `") + binop_str(op.bin) + "=`", sp);
            bump();
        }
        if (op_is_binary(op, BinOpKind::Lt) && token.kind == Tok::Gt && prev_token.span.hi == token.span.lo) {
            Span sp = op.span.to(token.span);
            emit("invalid comparison operator `<>`", sp);
            bump();
        }
        if (op_is_binary(op, BinOpKind::Le) && token.kind == Tok::Gt && prev_token.span.hi == token.span.lo) {
            Span sp = op.span.to(token.span);
            emit("invalid comparison operator `<=>`", sp);
            bump();
        }
        if (prev_token.kind == Tok::Plus && token.kind == Tok::Plus && prev_token.span.between(token.span).is_empty()) {
            Span op_span = prev_token.span.to(token.span);
            bump();
            lhs = recover_from_inc_dec(lhs, true, false, op_span, starts_stmt);
            continue;
        }
        if (prev_token.kind == Tok::Minus && token.kind == Tok::Minus &&
            prev_token.span.between(token.span).is_empty() && !tok_can_begin_expr(look_ahead_tok(1))) {
            Span op_span = prev_token.span.to(token.span);
            bump();
            lhs = recover_from_inc_dec(lhs, false, false, op_span, starts_stmt);
            continue;
        }
        Span op_span = op.span;
        if (op.kind == AssocOp::Cast) {
            lhs = parse_assoc_op_cast(lhs, lhs_span, op_span, ExprKind::Cast);
            continue;
        } else if (op.kind == AssocOp::Range) {
            lhs = parse_expr_range(prec, lhs, op.range, cur_op_span);
            break;
        }
        PrecBound rhs_min =
            op_fixity(op) == Fixity::Right ? PrecBound::included(prec) : PrecBound::excluded(prec);
        Expr* rhs = with_res(res_minus(restrictions, R_STMT_EXPR), [&] {
                        AttrWrapper attrs = parse_outer_attributes();
                        return parse_expr_assoc_with(rhs_min, std::move(attrs));
                    }).first;
        Span span = mk_expr_sp(lhs, lhs_span, op_span, rhs->span);
        Expr* e;
        if (op.kind == AssocOp::Binary) {
            e = mk_expr(span, ExprKind::Binary);
            e->binop = op.bin;
        } else if (op.kind == AssocOp::Assign) {
            e = mk_expr(span, ExprKind::Assign);
        } else {
            e = mk_expr(span, ExprKind::AssignOp);
            e->assignop = op.assign;
        }
        e->op_span = cur_op_span;
        e->lhs = lhs;
        e->rhs = rhs;
        lhs = e;
    }
    return {lhs, parsed_something};
}

bool Parser::should_continue_as_assoc_expr(Expr* lhs) {
    bool complete = expr_is_complete(lhs);
    std::optional<AssocOp> op = assoc_op_from_token(token);
    if (complete && !op) return false;
    if (!complete) return true;
    if (op->kind == AssocOp::Binary &&
        (op->bin == BinOpKind::Mul || op->bin == BinOpKind::Sub || op->bin == BinOpKind::Add ||
         op->bin == BinOpKind::And || op->bin == BinOpKind::Or || op->bin == BinOpKind::BitOr)) {
        // ambiguous_block_expr_parse only feeds suggestions.
        return false;
    }
    if (!op_can_continue_expr_unambiguously(*op)) return false;
    error_found_expr_would_be_stmt(lhs);
    return true;
}

void Parser::error_found_expr_would_be_stmt(Expr*) {
    emit("expected expression, found `" + token_str(token) + "`", token.span);
}

std::optional<AssocOp> Parser::check_assoc_op() {
    std::optional<AssocOp> op = assoc_op_from_token(token);
    if (op) {
        if (res_contains(st_.restrictions, R_CONST_EXPR) &&
            ((op->kind == AssocOp::Binary &&
              (op->bin == BinOpKind::Shr || op->bin == BinOpKind::Gt || op->bin == BinOpKind::Ge)) ||
             (op->kind == AssocOp::AssignOp && op->assign == AssignOpKind::ShrAssign)))
            return std::nullopt;
        if (res_contains(st_.restrictions, R_IS_PAT) &&
            (op->kind == AssocOp::Assign || op->kind == AssocOp::AssignOp || op_is_binary(*op, BinOpKind::BitOr) ||
             op->kind == AssocOp::Range))
            return std::nullopt;
        op->span = token.span;
        return op;
    }
    if (token.kind == Tok::Ident && !token.raw && may_recover()) {
        if (token.sym == "and") {
            emit("`and` is not a logical operator", token.span);
            AssocOp a;
            a.kind = AssocOp::Binary;
            a.bin = BinOpKind::And;
            a.span = token.span;
            return a;
        }
        if (token.sym == "or") {
            emit("`or` is not a logical operator", token.span);
            AssocOp a;
            a.kind = AssocOp::Binary;
            a.bin = BinOpKind::Or;
            a.span = token.span;
            return a;
        }
    }
    return std::nullopt;
}

bool Parser::expr_is_complete(const Expr* e) const {
    return res_contains(st_.restrictions, R_STMT_EXPR) && classify_expr_is_complete(e);
}

Expr* Parser::parse_expr_range(ExprPrecedence prec, Expr* lhs, RangeLimits limits, Span cur_op_span) {
    Expr* rhs = nullptr;
    if (is_at_start_of_range_notation_rhs()) {
        AttrWrapper attrs = parse_outer_attributes();
        // maybe_err_dotdotlt_syntax only adds a suggestion.
        rhs = parse_expr_assoc_with(PrecBound::excluded(prec), std::move(attrs)).first;
    }
    Span rhs_span = rhs ? rhs->span : cur_op_span;
    Span span = mk_expr_sp(lhs, lhs->span, cur_op_span, rhs_span);
    return mk_range(span, lhs, rhs, limits);
}

bool Parser::is_at_start_of_range_notation_rhs() const {
    if (tok_can_begin_expr(token)) {
        if (token.kind == Tok::OpenBrace) return !res_contains(st_.restrictions, R_NO_STRUCT_LITERAL);
        return true;
    }
    return false;
}

Expr* Parser::parse_expr_prefix_range(AttrWrapper attrs) {
    if (!attrs.is_empty()) emit("attributes are not allowed on range expressions starting with `..`", token.span);
    if (token.kind == Tok::DotDotDot) err_dotdotdot_syntax(token.span);
    RangeLimits limits = token.kind == Tok::DotDot ? RangeLimits::HalfOpen : RangeLimits::Closed;
    std::optional<AssocOp> op = assoc_op_from_token(token);
    AttrWrapper attrs2 = parse_outer_attributes();
    Span lo = token.span;
    bump();
    Span span = lo;
    Expr* opt_end = nullptr;
    if (is_at_start_of_range_notation_rhs()) {
        AttrWrapper a = parse_outer_attributes();
        Expr* x = parse_expr_assoc_with(PrecBound::excluded(op_precedence(*op)), std::move(a)).first;
        span = lo.to(x->span);
        opt_end = x;
    }
    Expr* e = mk_range(span, nullptr, opt_end, limits);
    e->attrs = std::move(attrs2.attrs);
    return e;
}

Expr* Parser::parse_expr_prefix(AttrWrapper attrs) {
    DepthGuard g(*this);
    Span lo = token.span;
    auto make_it = [&](Expr* e) {
        // make_it!: the span is lo.to(hi); the helpers already built it.
        e->attrs = std::move(attrs.attrs);
        return e;
    };
    switch (token.kind) {
    case Tok::Bang: return make_it(parse_expr_unary(lo, UnOp::Not));
    case Tok::Tilde: return make_it(recover_tilde_expr(lo));
    case Tok::Minus: return make_it(parse_expr_unary(lo, UnOp::Neg));
    case Tok::Star: return make_it(parse_expr_unary(lo, UnOp::Deref));
    case Tok::And:
    case Tok::AndAnd: return make_it(parse_expr_borrow(lo));
    case Tok::Plus:
        if (tok_is_numeric_lit(look_ahead_tok(1))) {
            emit("leading `+` is not supported", lo);
            bump();
            AttrWrapper a = parse_outer_attributes();
            return parse_expr_prefix(std::move(a));
        }
        if (look_ahead_tok(1).kind == Tok::Plus) {
            bool starts_stmt = prev_token.kind == Tok::Semi || prev_token.kind == Tok::CloseBrace;
            Span pre_span = token.span.to(look_ahead_tok(1).span);
            bump();
            bump();
            Expr* operand_expr = parse_expr_dot_or_call(std::move(attrs));
            return recover_from_inc_dec(operand_expr, true, true, pre_span, starts_stmt);
        }
        break;
    case Tok::Ident:
        if (tok_is_keyword(token, Kw::Box)) return make_it(parse_expr_box(lo));
        if (tok_is_keyword(token, Kw::Move) && look_ahead_tok(1).kind == Tok::OpenParen)
            return make_it(parse_expr_move(lo));
        if (may_recover() && is_mistaken_not_ident_negation()) return make_it(recover_not_expr(lo));
        break;
    default: break;
    }
    return parse_expr_dot_or_call(std::move(attrs));
}

std::pair<Span, Expr*> Parser::parse_expr_prefix_common(Span lo) {
    bump();
    AttrWrapper attrs = parse_outer_attributes();
    Expr* expr = tok_is_range_separator(token) ? parse_expr_prefix_range(std::move(attrs))
                                               : parse_expr_prefix(std::move(attrs));
    return {lo.to(expr->span), expr};
}

Expr* Parser::parse_expr_unary(Span lo, UnOp op) {
    std::pair<Span, Expr*> r = parse_expr_prefix_common(lo);
    Expr* e = mk_expr(lo.to(r.first), ExprKind::Unary);
    e->unop = op;
    e->lhs = r.second;
    return e;
}

Expr* Parser::recover_tilde_expr(Span lo) {
    emit("`~` cannot be used as a unary operator", lo);
    return parse_expr_unary(lo, UnOp::Not);
}

Expr* Parser::parse_expr_box(Span box_kw) {
    std::pair<Span, Expr*> r = parse_expr_prefix_common(box_kw);
    emit("`box_syntax` has been removed", r.first);
    return mk_expr_err(box_kw.to(r.first));
}

Expr* Parser::parse_expr_move(Span move_kw) {
    bump();
    expect(EXP(OpenParen));
    Expr* expr = parse_expr();
    expect(EXP(CloseParen));
    Span span = move_kw.to(prev_token.span);
    Expr* e = mk_expr(span, ExprKind::Move);
    e->lhs = expr;
    e->kw_span = move_kw;
    return e;
}

bool Parser::is_mistaken_not_ident_negation() const {
    auto token_cannot_continue_expr = [this](const Token& t) {
        if (t.kind == Tok::Ident) return ident_can_begin_expr(t);
        return t.kind == Tok::Literal || t.kind == Tok::Pound;
    };
    return tok_is_ident_named(token, "not") && token_cannot_continue_expr(look_ahead_tok(1));
}

Expr* Parser::recover_not_expr(Span lo) {
    Token negated = look_ahead_tok(1);
    emit("unexpected " + token_descr(negated) + " after identifier", negated.span);
    return parse_expr_unary(lo, UnOp::Not);
}

Expr* Parser::parse_assoc_op_cast(Expr* lhs, Span lhs_span, Span op_span, ExprKind expr_kind) {
    auto mk_cast = [&](Expr* l, Ty* rhs) {
        Expr* e = mk_expr(mk_expr_sp(l, lhs_span, op_span, rhs->span), expr_kind);
        e->lhs = l;
        e->ty = rhs;
        return e;
    };
    ParserState before_type = create_snapshot_for_diagnostic();
    Expr* cast_expr = nullptr;
    try {
        Ty* rhs = parse_as_cast_ty();
        cast_expr = mk_cast(lhs, rhs);
    } catch (PErr& type_err) {
        if (!may_recover()) throw;
        ParserState after_type = create_snapshot_for_diagnostic();
        restore_snapshot(before_type);
        if (lhs->kind == ExprKind::Path && lhs->qself == nullptr && lhs->path != nullptr &&
            lhs->path->segments.size() == 1 && token.kind == Tok::Ident && !token.raw &&
            (token.kw == Kw::For || token.kw == Kw::Loop || token.kw == Kw::While)) {
            ParserState snapshot = create_snapshot_for_diagnostic();
            const PathSegment& segment = lhs->path->segments[0];
            Label label;
            file_.interned.emplace_back("'" + std::string(segment.ident.name));
            label.ident.name = file_.interned.back();
            label.ident.span = segment.ident.span;
            try {
                Expr* expr = parse_expr_labeled(label, false);
                emit("malformed loop label", label.ident.span);
                return expr;
            } catch (PErr&) {
                restore_snapshot(snapshot);
            }
        }
        Path* path = nullptr;
        try {
            path = parse_path(PathStyle::Expr);
        } catch (PErr&) {
            restore_snapshot(after_type);
            throw type_err;
        }
        Ty* ty = mk_ty(path->span, TyKind::Path);
        ty->path = path;
        Expr* expr = mk_cast(lhs, ty);
        if (token.kind == Tok::Lt) {
            emit("`<` is interpreted as a start of generic arguments for `" + path_to_string(path) +
                     "`, not a comparison",
                 token.span);
        } else if (token.kind == Tok::Shl) {
            emit("`<<` is interpreted as a start of generic arguments for `" + path_to_string(path) +
                     "`, not a shift",
                 token.span);
        } else {
            restore_snapshot(after_type);
            throw type_err;
        }
        cast_expr = expr;
    }
    Span span = cast_expr->span;
    Expr* with_postfix = parse_expr_dot_or_call_with(AttrVec(), cast_expr, span);
    if (with_postfix->kind != ExprKind::Cast) {
        const char* what = nullptr;
        switch (with_postfix->kind) {
        case ExprKind::Index: what = "indexing"; break;
        case ExprKind::Try: what = "`?`"; break;
        case ExprKind::Field: what = "a field access"; break;
        case ExprKind::MethodCall: what = "a method call"; break;
        case ExprKind::Call: what = "a function call"; break;
        case ExprKind::Await: what = "`.await`"; break;
        case ExprKind::Use: what = "`.use`"; break;
        case ExprKind::Yield: what = "`.yield`"; break;
        case ExprKind::Match: what = "a postfix match"; break;
        case ExprKind::Err: return with_postfix;
        default: what = nullptr; break;
        }
        if (what != nullptr) emit(std::string("cast cannot be followed by ") + what, span);
    }
    return with_postfix;
}

std::string Parser::path_to_string(const Path* path) const {
    // pprust::path_to_string for the error message: the source text.
    return std::string(text(path->span));
}

Expr* Parser::parse_expr_borrow(Span lo) {
    expect_and();
    bool has_lifetime = tok_is_lifetime(token) && look_ahead_tok(1).kind != Tok::Colon;
    std::optional<Lifetime> lifetime;
    if (has_lifetime) lifetime = expect_lifetime();
    std::pair<BorrowKind, Mutability> bm = parse_borrow_modifiers();
    AttrWrapper attrs = parse_outer_attributes();
    Expr* expr = tok_is_range_separator(token) ? parse_expr_prefix_range(std::move(attrs))
                                               : parse_expr_prefix(std::move(attrs));
    Span span = lo.to(expr->span);
    if (lifetime) emit("borrow expressions cannot be annotated with lifetimes", span);
    if (bm.first == BorrowKind::Ref && bm.second == Mutability::Not && expr->kind == ExprKind::Path &&
        expr->qself == nullptr && path_is_single_kw(expr->path, "raw") && expr->path->segments[0].args == nullptr) {
        st_.expected_token_types.insert(TokenType::KwMut);
        st_.expected_token_types.insert(TokenType::KwConst);
    }
    Expr* e = mk_expr(span, ExprKind::AddrOf);
    e->borrow = bm.first;
    e->mutbl = bm.second;
    e->lhs = expr;
    return e;
}

std::pair<BorrowKind, Mutability> Parser::parse_borrow_modifiers() {
    if (check_keyword(KW(Raw)) && tok_is_mutability(look_ahead_tok(1))) {
        eat_keyword(KW(Raw));
        std::optional<Mutability> m = parse_mut_or_const();
        return {BorrowKind::Raw, m ? *m : Mutability::Not};
    }
    std::pair<Pinnedness, Mutability> pm = parse_pin_and_mut();
    if (pm.first == Pinnedness::Not) return {BorrowKind::Ref, pm.second};
    return {BorrowKind::Pin, pm.second};
}

Expr* Parser::parse_expr_dot_or_call(AttrWrapper attrs) {
    Expr* base = parse_expr_bottom();
    Span span = base->span;
    return parse_expr_dot_or_call_with(std::move(attrs.attrs), base, span);
}

Expr* Parser::parse_expr_dot_or_call_with(AttrVec attrs, Expr* e, Span lo) {
    DepthGuard g(*this);
    while (true) {
        bool has_question = tok_is_keyword(prev_token, Kw::Return) ? eat_noexpect(Tok::Question) : eat(EXP(Question));
        if (has_question) {
            Expr* t = mk_expr(lo.to(prev_token.span), ExprKind::Try);
            t->lhs = e;
            e = t;
            continue;
        }
        bool has_dot;
        if (tok_is_keyword(prev_token, Kw::Return)) {
            has_dot = eat_noexpect(Tok::Dot);
        } else if (token.kind == Tok::RArrow && may_recover()) {
            bump();
            emit("`->` is not valid syntax for field accesses and method calls", prev_token.span);
            has_dot = true;
        } else {
            has_dot = eat(EXP(Dot));
        }
        if (has_dot) {
            e = parse_dot_suffix_expr(lo, e);
            continue;
        }
        if (expr_is_complete(e)) break;
        if (token.kind == Tok::OpenParen)
            e = parse_expr_fn_call(lo, e);
        else if (token.kind == Tok::OpenBracket)
            e = parse_expr_index(lo, e);
        else
            break;
    }
    if (!attrs.empty()) {
        for (Attribute& a : e->attrs) attrs.push_back(std::move(a));
        e->attrs = std::move(attrs);
    }
    return e;
}

Expr* Parser::parse_dot_suffix_expr(Span lo, Expr* base) {
    if (token.kind == Tok::Ident) return parse_dot_suffix(base, lo);
    if (token.kind == Tok::Literal && token.lit == LitKind::Integer) {
        Span ident_span = token.span;
        std::string_view sym = token.sym, suffix = token.suffix;
        bump();
        return mk_expr_tuple_field_access(lo, ident_span, base, sym, suffix);
    }
    if (token.kind == Tok::Literal && token.lit == LitKind::Float) {
        std::string_view suffix = token.suffix;
        DestructuredFloat df = break_up_float(token.sym, token.span);
        switch (df.kind) {
        case DestructuredFloat::Single: {
            Span ident_span = token.span;
            bump();
            return mk_expr_tuple_field_access(lo, ident_span, base, df.sym1, suffix);
        }
        case DestructuredFloat::TrailingDot: {
            Token id;
            id.kind = Tok::Ident;
            id.sym = df.sym1;
            id.kw = kw_lookup(df.sym1);
            id.span = df.span1;
            st_.token = id;
            Token dot;
            dot.kind = Tok::Dot;
            dot.span = df.dot_span;
            bump_with(dot, st_.token_spacing);
            return mk_expr_tuple_field_access(lo, df.span1, base, df.sym1, std::string_view());
        }
        case DestructuredFloat::MiddleDot: {
            Token next2;
            next2.kind = Tok::Ident;
            next2.sym = df.sym2;
            next2.kw = kw_lookup(df.sym2);
            next2.span = df.span2;
            bump_with(next2, st_.token_spacing);
            bump();
            Expr* base1 = mk_expr_tuple_field_access(lo, df.span1, base, df.sym1, std::string_view());
            return mk_expr_tuple_field_access(lo, df.span2, base1, df.sym2, suffix);
        }
        case DestructuredFloat::Error: return base;
        }
        return base;
    }
    error_unexpected_after_dot();
    return base;
}

void Parser::error_unexpected_after_dot() {
    std::string actual = token_descr(token);
    Span span = token.span;
    if (token.kind == Tok::Eof && subparser_name_ != nullptr && span.hi < src_.size()) {
        size_t len = 0;
        decode_utf8(src_, span.hi, len);
        actual = "`" + std::string(src_.substr(span.hi, len)) + "`";
        span = span.shrink_to_hi();
    }
    emit("unexpected token: " + actual, span);
}

Parser::DestructuredFloat Parser::break_up_float(std::string_view float_str, Span span) {
    struct Comp {
        bool ident;
        std::string_view text;
        char punct;
    };
    std::vector<Comp> comps;
    size_t start = 0;
    bool in_ident = false;
    for (size_t i = 0; i < float_str.size(); ++i) {
        char c = float_str[i];
        bool alnum = c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (alnum) {
            if (!in_ident) {
                start = i;
                in_ident = true;
            }
        } else if (c == '.' || c == '+' || c == '-') {
            if (in_ident) comps.push_back({true, float_str.substr(start, i - start), 0});
            in_ident = false;
            comps.push_back({false, std::string_view(), c});
        } else {
            error_unexpected_after_dot();
            return DestructuredFloat();
        }
    }
    if (in_ident) comps.push_back({true, float_str.substr(start), 0});
    bool can_take_span_apart = text(span) == float_str;
    DestructuredFloat d;
    auto P = [&](size_t i, char c) { return i < comps.size() && !comps[i].ident && comps[i].punct == c; };
    auto PM = [&](size_t i) { return P(i, '+') || P(i, '-'); };
    auto I = [&](size_t i) { return i < comps.size() && comps[i].ident; };
    size_t n = comps.size();
    if (n == 1 && I(0)) {
        d.kind = DestructuredFloat::Single;
        d.sym1 = comps[0].text;
        d.span1 = span;
        return d;
    }
    if (n == 2 && I(0) && P(1, '.')) {
        d.kind = DestructuredFloat::TrailingDot;
        d.sym1 = comps[0].text;
        if (can_take_span_apart) {
            d.span1 = span.with_hi(span.lo + static_cast<uint32_t>(comps[0].text.size()));
            d.dot_span = span.with_lo(d.span1.hi);
        } else {
            d.span1 = d.dot_span = span;
        }
        return d;
    }
    if (n == 3 && I(0) && P(1, '.') && I(2)) {
        d.kind = DestructuredFloat::MiddleDot;
        d.sym1 = comps[0].text;
        d.sym2 = comps[2].text;
        if (can_take_span_apart) {
            d.span1 = span.with_hi(span.lo + static_cast<uint32_t>(comps[0].text.size()));
            d.dot_span = Span(d.span1.hi, d.span1.hi + 1);
            d.span2 = span.with_lo(d.dot_span.hi);
        } else {
            d.span1 = d.dot_span = d.span2 = span;
        }
        return d;
    }
    if ((n == 2 && I(0) && PM(1)) || (n == 3 && I(0) && PM(1) && I(2)) ||
        (n == 4 && I(0) && P(1, '.') && I(2) && PM(3)) || (n == 5 && I(0) && P(1, '.') && I(2) && PM(3) && I(4))) {
        error_unexpected_after_dot();
        return DestructuredFloat();
    }
    // rustc panics on anything else; the lexer never produces it.
    error_unexpected_after_dot();
    return DestructuredFloat();
}

std::vector<Ident> Parser::parse_floating_field_access() {
    std::vector<Ident> fields;
    OptSpan trailing_dot;
    while (true) {
        Expr* expr = parse_expr();
        const Expr* current = expr;
        size_t start_idx = fields.size();
        auto ins = [&](Ident id) { fields.insert(fields.begin() + static_cast<std::ptrdiff_t>(start_idx), id); };
        while (true) {
            if (current->kind == ExprKind::Field) {
                ins(current->ident);
                trailing_dot = OptSpan();
                current = current->lhs;
            } else if (current->kind == ExprKind::Index) {
                emit("array indexing not supported in offset_of", current->op_span);
                current = current->lhs;
            } else if (current->kind == ExprKind::Lit &&
                       (current->lit.kind == LitKind::Float || current->lit.kind == LitKind::Integer)) {
                std::string_view symbol = current->lit.symbol;
                if (!current->lit.suffix.empty()) emit("suffixes on a tuple index are invalid", current->span);
                DestructuredFloat df = break_up_float(symbol, current->span);
                Ident id;
                switch (df.kind) {
                case DestructuredFloat::Single:
                    trailing_dot = OptSpan();
                    id.name = df.sym1;
                    id.span = df.span1;
                    ins(id);
                    break;
                case DestructuredFloat::TrailingDot:
                    trailing_dot = OptSpan(df.dot_span);
                    id.name = df.sym1;
                    id.span = df.span1;
                    ins(id);
                    break;
                case DestructuredFloat::MiddleDot: {
                    trailing_dot = OptSpan();
                    Ident id2;
                    id2.name = df.sym2;
                    id2.span = df.span2;
                    ins(id2);
                    id.name = df.sym1;
                    id.span = df.span1;
                    ins(id);
                    break;
                }
                case DestructuredFloat::Error:
                    trailing_dot = OptSpan();
                    id.name = symbol;
                    id.span = prev_token.span;
                    ins(id);
                    break;
                }
                break;
            } else if (current->kind == ExprKind::Path && current->qself == nullptr) {
                if (current->path->segments.size() == 1 && current->path->segments[0].args == nullptr) {
                    trailing_dot = OptSpan();
                    ins(current->path->segments[0].ident);
                } else {
                    emit("offset_of expects dot-separated field and variant names", current->span);
                }
                break;
            } else {
                emit("offset_of expects dot-separated field and variant names", current->span);
                break;
            }
        }
        if (token.kind == Tok::CloseParen || token.kind == Tok::CloseBrace || token.kind == Tok::CloseBracket ||
            token.kind == Tok::Comma) {
            break;
        } else if (!trailing_dot.has) {
            emit("offset_of expects dot-separated field and variant names", token.span);
            break;
        }
    }
    if (trailing_dot.has) emit("offset_of expects dot-separated field and variant names", trailing_dot.span);
    return fields;
}

Expr* Parser::mk_expr_tuple_field_access(Span lo, Span ident_span, Expr* base, std::string_view field,
                                         std::string_view suffix) {
    if (!suffix.empty()) emit("suffixes on a tuple index are invalid", ident_span);
    Expr* e = mk_expr(lo.to(ident_span), ExprKind::Field);
    e->lhs = base;
    e->ident.name = field;
    e->ident.span = ident_span;
    return e;
}

Expr* Parser::parse_expr_fn_call(Span lo, Expr* fun) {
    bool have_snapshot = token.kind == Tok::OpenParen;
    ParserState snapshot;
    if (have_snapshot) snapshot = create_snapshot_for_diagnostic();
    try {
        std::vector<Expr*> args = parse_expr_paren_seq();
        Expr* e = mk_expr(lo.to(prev_token.span), ExprKind::Call);
        e->lhs = fun;
        e->exprs = std::move(args);
        return e;
    } catch (PErr& err) {
        // maybe_recover_struct_lit_bad_delims
        if (may_recover() && have_snapshot && fun->kind == ExprKind::Path && fun->qself == nullptr) {
            ParserState after = create_snapshot_for_diagnostic();
            restore_snapshot(snapshot);
            bump();  // `(`
            bool ok = false;
            StructFields sf;
            try {
                sf = parse_struct_fields(fun->path, false, EXP(CloseParen));
                ok = true;
            } catch (PErr&) {
            }
            if (ok && eat(EXP(CloseParen))) {
                Span close_paren = prev_token.span;
                Span span = lo.to(close_paren);
                size_t non_shorthand = 0;
                for (const ExprField& f : sf.fields)
                    if (!f.is_shorthand) ++non_shorthand;
                if (non_shorthand > 0 && text(close_paren) == ")")
                    emit("invalid `struct` delimiters or `fn` call arguments", span);
                else
                    emit(err);
                return mk_expr_err(span);
            }
            restore_snapshot(after);
        }
        return recover_seq_parse_error(EXP(OpenParen), EXP(CloseParen), lo, err);
    }
}

Expr* Parser::parse_expr_index(Span lo, Expr* base) {
    Span prev_span = prev_token.span;
    Span open_delim_span = token.span;
    bump();  // `[`
    Expr* index = parse_expr();
    suggest_missing_semicolon_before_array(prev_span, open_delim_span);
    expect(EXP(CloseBracket));
    Expr* e = mk_expr(lo.to(prev_token.span), ExprKind::Index);
    e->lhs = base;
    e->rhs = index;
    e->op_span = open_delim_span.to(prev_token.span);
    return e;
}

Expr* Parser::parse_dot_suffix(Expr* self_arg, Span lo) {
    if (edition_ >= Edition::E2018 && eat_keyword(KW(Await))) return mk_await_expr(self_arg, lo);
    if (eat_keyword(KW(Use))) return mk_use_expr(self_arg, lo);
    if (eat_keyword(KW(Match))) {
        Span match_span = prev_token.span;
        return parse_match_block(lo, match_span, self_arg, MatchKind::Postfix);
    }
    if (eat_keyword(KW(Yield))) {
        Span yield_span = prev_token.span;
        Expr* e = mk_expr(lo.to(yield_span), ExprKind::Yield);
        e->yield_kind = YieldKind::Postfix;
        e->lhs = self_arg;
        return e;
    }
    Span fn_span_lo = token.span;
    PathSegment seg = parse_path_segment(PathStyle::Expr, nullptr);
    check_trailing_angle_brackets(seg, {EXP(OpenParen)});
    check_turbofish_missing_angle_brackets(seg);
    if (check(EXP(OpenParen))) {
        std::vector<Expr*> args = parse_expr_paren_seq();
        Span fn_span = fn_span_lo.to(prev_token.span);
        Span span = lo.to(prev_token.span);
        Expr* e = mk_expr(span, ExprKind::MethodCall);
        e->seg = seg;
        e->lhs = self_arg;
        e->exprs = std::move(args);
        e->method_span = fn_span;
        return e;
    }
    Span span = lo.to(prev_token.span);
    // FieldExpressionWithGeneric is stashed (StashKey::GenericInFieldExpr):
    // nothing steals it during parsing, so it is reported late.
    if (seg.args != nullptr) late_.push_back(PErr{"field expressions cannot have generic arguments", seg.args->span});
    Expr* e = mk_expr(span, ExprKind::Field);
    e->lhs = self_arg;
    e->ident = seg.ident;
    return e;
}

Expr* Parser::parse_expr_bottom() {
    DepthGuard g(*this);
    // maybe_recover_from_interpolated_ty_qpath! and the metavariable cases
    // only apply inside macro expansion.
    Restrictions restrictions = st_.restrictions;
    return with_res(res_minus(restrictions, R_ALLOW_LET), [&]() -> Expr* {
        Span lo = token.span;
        if (token.kind == Tok::Literal) return parse_expr_lit();
        if (check(EXP(OpenParen))) return parse_expr_tuple_parens(restrictions);
        if (check(EXP(OpenBrace))) {
            if (Expr* e = maybe_recover_bad_struct_literal_path(false)) return e;
            return parse_expr_block(std::nullopt, lo, BlockCheckMode::Default);
        }
        if (check(EXP(Or)) || check(EXP(OrOr))) return parse_expr_closure();
        if (check(EXP(OpenBracket))) return parse_expr_array_or_repeat(EXP(CloseBracket));
        if (is_builtin()) return parse_expr_builtin();
        if (check_path()) return parse_expr_path_start();
        if (check_keyword(KW(Move)) || check_keyword(KW(Use)) || check_keyword(KW(Static)) || check_const_closure())
            return parse_expr_closure();
        if (eat_keyword(KW(If))) return parse_expr_if();
        if (check_keyword(KW(For))) {
            if (choose_generics_over_qpath(1)) return parse_expr_closure();
            eat_keyword(KW(For));
            return parse_expr_for(std::nullopt, lo);
        }
        if (eat_keyword(KW(While))) return parse_expr_while(std::nullopt, lo);
        if (std::optional<Label> label = eat_label()) return parse_expr_labeled(*label, true);
        if (eat_keyword(KW(Loop))) return parse_expr_loop(std::nullopt, lo);
        if (eat_keyword(KW(Match))) return parse_expr_match();
        if (eat_keyword(KW(Unsafe))) return parse_expr_block(std::nullopt, lo, BlockCheckMode::Unsafe);
        if (check_inline_const(0)) return parse_const_block(lo, false);
        if (may_recover() && is_do_catch_block()) return recover_do_catch();
        if (is_try_block()) {
            expect_keyword(KW(Try));
            return parse_try_block(lo);
        }
        if (eat_keyword(KW(Return))) return parse_expr_return();
        if (eat_keyword(KW(Continue))) return parse_expr_continue(lo);
        if (eat_keyword(KW(Break))) return parse_expr_break();
        if (eat_keyword(KW(Yield))) return parse_expr_yield();
        if (is_do_yeet()) return parse_expr_yeet();
        if (eat_keyword(KW(Become))) return parse_expr_become();
        if (check_keyword(KW(Let))) return parse_expr_let(restrictions);
        if (eat_keyword(KW(Underscore))) {
            if (Expr* e = maybe_recover_bad_struct_literal_path(true)) return e;
            return mk_expr(prev_token.span, ExprKind::Underscore);
        }
        if (edition_ >= Edition::E2018) {
            bool at_async = check_keyword(KW(Async));
            if (edition_ >= Edition::E2024 && is_gen_block(Kw::Gen, at_async ? 1 : 0)) return parse_gen_block();
            if (is_gen_block(Kw::Async, 0)) return parse_gen_block();
            if (at_async) return parse_expr_closure();
            if (eat_keyword_noexpect(Kw::Await)) return recover_incorrect_await_syntax(lo);
            return parse_expr_lit();
        }
        return parse_expr_lit();
    });
}

Expr* Parser::parse_expr_lit() {
    Span lo = token.span;
    Lit lit;
    if (parse_opt_token_lit(lit)) {
        Expr* e = mk_expr(lo.to(prev_token.span), ExprKind::Lit);
        e->lit = lit;
        return maybe_recover_from_bad_qpath(e);
    }
    return try_macro_suggestion();
}

Expr* Parser::parse_expr_tuple_parens(Restrictions restrictions) {
    Span lo = token.span;
    expect(EXP(OpenParen));
    std::vector<Expr*> es;
    Trailing trailing_comma = Trailing::No;
    try {
        es = parse_seq_to_end<Expr*>(
            EXP(CloseParen), SeqSep::trailing_allowed(EXP(Comma)),
            [&] { return parse_expr_catch_underscore(static_cast<Restrictions>(restrictions & R_ALLOW_LET)); },
            &trailing_comma);
    } catch (PErr& err) {
        return recover_seq_parse_error(EXP(OpenParen), EXP(CloseParen), lo, err);
    }
    Expr* e;
    if (es.size() == 1 && trailing_comma == Trailing::No) {
        e = mk_expr(lo.to(prev_token.span), ExprKind::Paren);
        e->lhs = es[0];
    } else {
        e = mk_expr(lo.to(prev_token.span), ExprKind::Tup);
        e->exprs = std::move(es);
    }
    return maybe_recover_from_bad_qpath(e);
}

Expr* Parser::parse_expr_array_or_repeat(ExpTok close) {
    Span lo = token.span;
    bump();  // `[` or other open delim
    Expr* e = nullptr;
    if (eat(close)) {
        e = mk_expr(Span(), ExprKind::Array);
    } else {
        Expr* first_expr = parse_expr();
        if (eat(EXP(Semi))) {
            AnonConst count = parse_expr_anon_const(true);
            expect(close);
            e = mk_expr(Span(), ExprKind::Repeat);
            e->lhs = first_expr;
            e->anon_const = count;
        } else if (eat(EXP(Comma))) {
            std::vector<Expr*> exprs =
                parse_seq_to_end<Expr*>(close, SeqSep::trailing_allowed(EXP(Comma)), [this] { return parse_expr(); });
            exprs.insert(exprs.begin(), first_expr);
            e = mk_expr(Span(), ExprKind::Array);
            e->exprs = std::move(exprs);
        } else {
            expect(close);
            e = mk_expr(Span(), ExprKind::Array);
            e->exprs.push_back(first_expr);
        }
    }
    e->span = lo.to(prev_token.span);
    return maybe_recover_from_bad_qpath(e);
}

Expr* Parser::parse_expr_path_start() {
    QSelf* qself = nullptr;
    Path* path = nullptr;
    if (eat_lt()) {
        std::pair<QSelf*, Path*> qp = parse_qpath(PathStyle::Expr);
        qself = qp.first;
        path = qp.second;
    } else {
        path = parse_path(PathStyle::Expr);
    }
    Expr* e;
    if (eat(EXP(Bang))) {
        if (qself != nullptr) emit("macros cannot use qualified paths", path->span);
        Span lo = path->span;
        MacCall* mac = mk<MacCall>();
        mac->path = path;
        mac->args = parse_delim_args();
        e = mk_expr(lo.to(prev_token.span), ExprKind::MacCall);
        e->mac = mac;
    } else {
        if (check(EXP(OpenBrace))) {
            bool some = false;
            Expr* se = maybe_parse_struct_expr(qself, path, some);
            if (some) return se;
        }
        e = mk_expr(path->span, ExprKind::Path);
        e->qself = qself;
        e->path = path;
    }
    return maybe_recover_from_bad_qpath(e);
}

namespace {
struct FindLabeledBreaks {
    static bool visit(const Expr* e) {
        if (e == nullptr) return false;
        if (e->kind == ExprKind::Break && e->has_label) return true;
        if (visit(e->lhs) || visit(e->rhs)) return true;
        for (const Expr* x : e->exprs)
            if (visit(x)) return true;
        if (e->block)
            for (const Stmt* s : e->block->stmts)
                if (s->expr && visit(s->expr)) return true;
        for (const Arm& a : e->arms)
            if (visit(a.body) || (a.guard && visit(a.guard->cond))) return true;
        if (e->closure && visit(e->closure->body)) return true;
        for (const ExprField& f : e->struct_fields)
            if (visit(f.expr)) return true;
        if (e->anon_const.value && visit(e->anon_const.value)) return true;
        return false;
    }
};
}  // namespace

Expr* Parser::parse_expr_labeled(Label label_, bool consume_colon) {
    Span lo = label_.ident.span;
    std::optional<Label> label = label_;
    bool ate_colon = eat(EXP(Colon));
    Expr* expr;
    if (eat_keyword(KW(While))) {
        expr = parse_expr_while(label, lo);
    } else if (eat_keyword(KW(For))) {
        expr = parse_expr_for(label, lo);
    } else if (eat_keyword(KW(Loop))) {
        expr = parse_expr_loop(label, lo);
    } else if (check_noexpect(Tok::OpenBrace)) {
        expr = parse_expr_block(label, lo, BlockCheckMode::Default);
    } else if (!ate_colon && may_recover() &&
               (token.kind == Tok::CloseParen || token.kind == Tok::CloseBrace || token.kind == Tok::CloseBracket ||
                tok_is_punct(token)) &&
               could_be_unclosed_char_literal(label_.ident)) {
        expr = recover_unclosed_char_expr(label_.ident, "expected `while`, `for`, `loop` or `{` after a label",
                                          token.span);
        consume_colon = false;
    } else if (!ate_colon && (check_noexpect(Tok::Comma) || check_noexpect(Tok::Gt))) {
        emit("expected `while`, `for`, `loop` or `{` after a label", token.span);
        consume_colon = false;
        expr = mk_expr_err(lo);
    } else {
        Span err_span = token.span;
        try {
            Expr* e = parse_expr();
            Span span = e->span;
            if (FindLabeledBreaks::visit(e)) {
                Stmt* stmt = mk_stmt(span, StmtKind::Expr);
                stmt->expr = e;
                std::vector<Stmt*> stmts{stmt};
                Block* blk = mk_block(std::move(stmts), BlockCheckMode::Default, span);
                e = mk_expr(span, ExprKind::Block);
                e->block = blk;
                e->has_label = true;
                e->label = *label;
            }
            expr = e;
        } catch (PErr&) {
            emit("expected `while`, `for`, `loop` or `{` after a label", err_span);
            throw;
        }
        emit("expected `while`, `for`, `loop` or `{` after a label", err_span);
    }
    if (!ate_colon && consume_colon) emit("labeled expression must be followed by `:`", expr->span);
    return expr;
}

Expr* Parser::recover_unclosed_char_expr(Ident lifetime_ident, const std::string& err_msg, Span err_span) {
    if (!steal_late_error(lifetime_ident.span)) emit(err_msg, err_span);
    Expr* e = mk_expr(lifetime_ident.span, ExprKind::Lit);
    e->lit = mk_token_lit_char(lifetime_ident);
    return e;
}

Lit Parser::mk_token_lit_char(Ident lifetime_ident) {
    Lit l;
    l.kind = LitKind::Char;
    l.symbol = lifetime_ident.name.substr(1);  // without_first_quote
    l.span = lifetime_ident.span;
    return l;
}

Expr* Parser::recover_do_catch() {
    Span lo = token.span;
    bump();  // `do`
    bump();  // `catch`
    Span span = lo.to(prev_token.span);
    emit("found removed `do catch` syntax", span);
    return parse_try_block(lo);
}

Expr* Parser::parse_expr_opt() { return tok_can_begin_expr(token) ? parse_expr() : nullptr; }

Expr* Parser::parse_expr_return() {
    Span lo = prev_token.span;
    Expr* inner = parse_expr_opt();
    Expr* e = mk_expr(lo.to(prev_token.span), ExprKind::Ret);
    e->lhs = inner;
    return maybe_recover_from_bad_qpath(e);
}

Expr* Parser::parse_expr_yeet() {
    Span lo = token.span;
    bump();  // `do`
    bump();  // `yeet`
    Expr* inner = parse_expr_opt();
    Expr* e = mk_expr(lo.to(prev_token.span), ExprKind::Yeet);
    e->lhs = inner;
    return maybe_recover_from_bad_qpath(e);
}

Expr* Parser::parse_expr_become() {
    Span lo = prev_token.span;
    Expr* inner = parse_expr();
    Expr* e = mk_expr(lo.to(prev_token.span), ExprKind::Become);
    e->lhs = inner;
    return maybe_recover_from_bad_qpath(e);
}

Expr* Parser::parse_expr_break() {
    Span lo = prev_token.span;
    std::optional<Label> label = eat_label();
    Expr* kind_expr = nullptr;
    if (token.kind == Tok::Colon && label) {
        Label l = *label;
        label.reset();
        Expr* lexpr = parse_expr_labeled(l, true);
        emit("parentheses are required around this expression to avoid confusion with a labeled break expression",
             lexpr->span);
        kind_expr = lexpr;
    } else if (token.kind != Tok::OpenBrace || !res_contains(st_.restrictions, R_NO_STRUCT_LITERAL)) {
        Expr* expr = parse_expr_opt();
        if (expr != nullptr) {
            // BREAK_WITH_LABEL_AND_LOOP is a lint: not reported by the parser.
            if (may_recover() && expr->kind == ExprKind::Path && expr->qself == nullptr &&
                expr->path->segments.size() == 1 && expr->path->segments[0].args == nullptr) {
                Ident ident = expr->path->segments[0].ident;
                if (Expr* next = parse_expr_opt()) {
                    label = recover_ident_into_label(ident);
                    expr = next;
                }
            }
        }
        kind_expr = expr;
    }
    Expr* e = mk_expr(lo.to(prev_token.span), ExprKind::Break);
    if (label) {
        e->has_label = true;
        e->label = *label;
    }
    e->lhs = kind_expr;
    return maybe_recover_from_bad_qpath(e);
}

Expr* Parser::parse_expr_continue(Span lo) {
    std::optional<Label> label = eat_label();
    if (may_recover() && !label && token.kind == Tok::Ident) {
        Ident ident = tok_ident(token);
        bump();
        label = recover_ident_into_label(ident);
    }
    Expr* e = mk_expr(lo.to(prev_token.span), ExprKind::Continue);
    if (label) {
        e->has_label = true;
        e->label = *label;
    }
    return e;
}

Expr* Parser::parse_expr_yield() {
    Span lo = prev_token.span;
    Expr* inner = parse_expr_opt();
    Expr* e = mk_expr(lo.to(prev_token.span), ExprKind::Yield);
    e->yield_kind = YieldKind::Prefix;
    e->lhs = inner;
    return maybe_recover_from_bad_qpath(e);
}

Expr* Parser::parse_expr_builtin() {
    Expr* result = nullptr;
    parse_builtin([&](Span lo, Ident ident) {
        if (ident.name == "offset_of") {
            result = parse_expr_offset_of(lo);
            return true;
        }
        if (ident.name == "type_ascribe") {
            result = parse_expr_type_ascribe(lo);
            return true;
        }
        if (ident.name == "wrap_binder") {
            result = parse_expr_unsafe_binder_cast(lo, UnsafeBinderCastKind::Wrap);
            return true;
        }
        if (ident.name == "unwrap_binder") {
            result = parse_expr_unsafe_binder_cast(lo, UnsafeBinderCastKind::Unwrap);
            return true;
        }
        return false;
    });
    return result;
}

Expr* Parser::parse_expr_offset_of(Span lo) {
    Ty* container = parse_ty();
    expect(EXP(Comma));
    std::vector<Ident> fields = parse_floating_field_access();
    (void)eat_noexpect(Tok::Comma);
    try {
        expect_one_of({}, {EXP(CloseParen)});
    } catch (PErr& e) {
        emit(e);
    }
    if (may_recover()) {
        while (!tok_is_close_delim_or_eof(token)) bump();
    }
    Span span = lo.to(token.span);
    Expr* e = mk_expr(span, ExprKind::OffsetOf);
    e->ty = container;
    e->fields = std::move(fields);
    return e;
}

Expr* Parser::parse_expr_type_ascribe(Span lo) {
    Expr* expr = parse_expr();
    expect(EXP(Comma));
    Ty* ty = parse_ty();
    Span span = lo.to(token.span);
    Expr* e = mk_expr(span, ExprKind::Type);
    e->lhs = expr;
    e->ty = ty;
    return e;
}

Expr* Parser::parse_expr_unsafe_binder_cast(Span lo, UnsafeBinderCastKind kind) {
    Expr* expr = parse_expr();
    Ty* ty = eat(EXP(Comma)) ? parse_ty() : nullptr;
    Span span = lo.to(token.span);
    Expr* e = mk_expr(span, ExprKind::UnsafeBinderCast);
    e->binder_cast = kind;
    e->lhs = expr;
    e->ty = ty;
    return e;
}

bool Parser::parse_str_lit(StrLit& out, Lit* non_str_lit, bool* had_lit) {
    if (had_lit) *had_lit = false;
    Lit lit;
    if (!parse_opt_meta_item_lit(lit)) return false;
    if (lit.kind == LitKind::Str || lit.kind == LitKind::StrRaw) {
        out.symbol = lit.symbol;
        out.suffix = lit.suffix;
        out.style = lit.kind == LitKind::Str ? StrStyle::Cooked : StrStyle::Raw;
        out.n_hashes = lit.n_hashes;
        out.span = lit.span;
        return true;
    }
    if (had_lit) *had_lit = true;
    if (non_str_lit) *non_str_lit = lit;
    return false;
}

Lit Parser::handle_missing_lit() {
    Token tok = token;
    std::string msg = "unexpected token: " + token_descr(tok);
    if (token.kind == Tok::Lifetime && !token.raw && could_be_unclosed_char_literal(tok_ident(token))) {
        Lifetime lt = expect_lifetime();
        if (!steal_late_error(lt.ident.span)) emit(msg, tok.span);
        return mk_token_lit_char(lt.ident);
    }
    fail(msg, tok.span);
}

Lit Parser::parse_token_lit() {
    Lit lit;
    if (parse_opt_token_lit(lit)) return lit;
    return handle_missing_lit();
}

Lit Parser::parse_meta_item_lit() {
    Lit lit;
    if (parse_opt_meta_item_lit(lit)) return lit;
    return handle_missing_lit();
}

void Parser::recover_after_dot() {
    if (token.kind != Tok::Dot) return;
    const Token& next = look_ahead_tok(1);
    if (next.kind == Tok::Literal && next.lit == LitKind::Integer &&
        (next.suffix.empty() || next.suffix == "f32" || next.suffix == "f64") && token.span.hi == next.span.lo) {
        bool all = true;
        for (char c : next.sym)
            if (!((c >= '0' && c <= '9') || c == '_')) all = false;
        if (!all) return;
        Token recovered = next;
        file_.interned.emplace_back("0." + std::string(next.sym));
        recovered.lit = LitKind::Float;
        recovered.sym = file_.interned.back();
        recovered.span = token.span.to(next.span);
        emit("float literals must have an integer part", recovered.span);
        bump();
        st_.token = recovered;
    }
}

bool Parser::eat_token_lit(Lit& out) {
    if (token.kind == Tok::Ident && !token.raw && tok_is_bool_lit(token)) {
        out = Lit();
        out.kind = LitKind::Bool;
        out.symbol = token.sym;
        out.span = token.span;
        bump();
        return true;
    }
    if (token.kind == Tok::Literal) {
        out = lit_from_token(token);
        bump();
        return true;
    }
    return false;
}

bool Parser::parse_opt_token_lit(Lit& out) {
    recover_after_dot();
    Span span = token.span;
    if (!eat_token_lit(out)) return false;
    out.span = span;
    return true;
}

bool Parser::parse_opt_meta_item_lit(Lit& out) {
    recover_after_dot();
    Span span = token.span;
    if (!eat_token_lit(out)) return false;
    out.span = span;
    if (std::optional<std::string> err = meta_lit_error(out)) {
        emit(*err, span);
        out.kind = LitKind::Err;
    }
    return true;
}

Expr* Parser::parse_literal_maybe_minus() {
    Span lo = token.span;
    bool minus_present = eat(EXP(Minus));
    Lit lit = parse_token_lit();
    Expr* expr = mk_expr(lit.span, ExprKind::Lit);
    expr->lit = lit;
    if (minus_present) {
        Expr* e = mk_expr(lo.to(prev_token.span), ExprKind::Unary);
        e->unop = UnOp::Neg;
        e->lhs = expr;
        return e;
    }
    return expr;
}

bool Parser::is_array_like_block() {
    return token.kind == Tok::OpenBrace &&
           (look_ahead_tok(1).kind == Tok::Ident || look_ahead_tok(1).kind == Tok::Literal) &&
           look_ahead_tok(2).kind == Tok::Comma && tok_can_begin_expr(look_ahead_tok(3));
}

Expr* Parser::maybe_suggest_brackets_instead_of_braces(Span) {
    ParserState before = create_snapshot_for_diagnostic();
    try {
        Expr* arr = parse_expr_array_or_repeat(EXP(CloseBrace));
        emit("this is a block expression, not an array", arr->span);
        return mk_expr_err(arr->span);
    } catch (PErr&) {
        restore_snapshot(before);
        return nullptr;
    }
}

void Parser::suggest_missing_semicolon_before_array(Span prev_span, Span open_delim_span) {
    if (!may_recover()) return;
    if (token.kind == Tok::Comma) {
        if (!is_multiline(prev_span.until(token.span))) return;
        ParserState before = create_snapshot_for_diagnostic();
        bump();
        bool found = false;
        try {
            parse_seq_to_before_end<Expr*>(EXP(CloseBracket), SeqSep::trailing_allowed(EXP(Comma)),
                                          [this] { return parse_expr(); });
            found = text(token.span) == "]";
        } catch (PErr&) {
        }
        restore_snapshot(before);
        if (found) fail("expected `;`, found `[`", open_delim_span);
    }
}

Expr* Parser::parse_expr_block(std::optional<Label> opt_label, Span lo, BlockCheckMode blk_mode) {
    if (may_recover() && is_array_like_block()) {
        if (Expr* arr = maybe_suggest_brackets_instead_of_braces(lo)) return arr;
    }
    std::pair<AttrVec, Block*> r = parse_block_common(lo, blk_mode, OptSpan());
    Expr* e = mk_expr_with_attrs(r.second->span, ExprKind::Block, std::move(r.first));
    e->block = r.second;
    if (opt_label) {
        e->has_label = true;
        e->label = *opt_label;
    }
    return e;
}

Expr* Parser::parse_simple_block() {
    Block* blk = parse_block();
    Expr* e = mk_expr(blk->span, ExprKind::Block);
    e->block = blk;
    return e;
}

bool Parser::enclosing_delim_is_paren() const {
    // token_cursor.stack.last().curr() is a parenthesized group.
    uint32_t i = cur_index();
    while (i > tok_lo_) {
        --i;
        Tok k = toks_[i].kind;
        if (k == Tok::CloseParen || k == Tok::CloseBrace || k == Tok::CloseBracket) {
            uint32_t p = partner_[i];
            if (p == UINT32_MAX || p >= i) return false;
            i = p;
            continue;
        }
        if (k == Tok::OpenParen) return true;
        if (k == Tok::OpenBrace || k == Tok::OpenBracket) return false;
    }
    return false;
}

Expr* Parser::parse_expr_closure() {
    Span lo = token.span;
    Token before = prev_token;
    ClosureBinder binder;
    if (check_keyword(KW(For))) {
        Span blo = token.span;
        std::pair<std::vector<GenericParam*>, OptSpan> hb = parse_higher_ranked_binder();
        binder.present = true;
        binder.span = blo.to(prev_token.span);
        binder.generic_params = std::move(hb.first);
    }
    Constness constness = parse_closure_constness();
    Movability movability = eat_keyword(KW(Static)) ? Movability::Static : Movability::Movable;
    CoroutineKind coroutine_kind;
    if (edition_ >= Edition::E2018) coroutine_kind = parse_coroutine_kind(Case::Sensitive);
    if (!binder.present && coroutine_kind.kind != CoroutineKind::None)
        st_.expected_token_types.insert(TokenType::OpenBrace);
    Span capture_kw;
    CaptureBy capture = parse_capture_clause(capture_kw);
    Span fn_arg_span;
    FnDecl* fn_decl = parse_fn_block_decl(fn_arg_span);
    Span decl_hi = prev_token.span;
    Expr* body;
    if (fn_decl->output.is_default()) {
        Restrictions restrictions = res_minus(res_minus(st_.restrictions, R_STMT_EXPR), R_ALLOW_LET);
        Token prev = prev_token;
        Token tok = token;
        AttrWrapper attrs = parse_outer_attributes();
        try {
            body = parse_expr_res(restrictions, std::move(attrs)).first;
        } catch (PErr& err) {
            body = recover_closure_body(err, before, prev, tok, lo, decl_hi);
        }
    } else {
        body = parse_closure_block_body(fn_decl->output.ty->span);
    }
    if (token.kind == Tok::Semi && enclosing_delim_is_paren() && may_recover()) body = mk_expr_err(body->span);
    Span body_span = body->span;
    Closure* c = mk<Closure>();
    c->binder = std::move(binder);
    c->capture = capture;
    c->capture_kw = capture_kw;
    c->constness = constness;
    c->coroutine_kind = coroutine_kind;
    c->movability = movability;
    c->fn_decl = fn_decl;
    c->body = body;
    c->fn_decl_span = lo.to(decl_hi);
    c->fn_arg_span = fn_arg_span;
    Expr* closure = mk_expr(lo.to(body->span), ExprKind::Closure);
    closure->closure = c;
    st_.has_current_closure = true;
    st_.current_closure.whole_closure = closure->span;
    st_.current_closure.closing_pipe = decl_hi;
    st_.current_closure.body = body_span;
    return closure;
}

Expr* Parser::parse_closure_block_body(Span) {
    if (may_recover() && tok_can_begin_expr(token) && token.kind != Tok::OpenBrace) {
        ParserState snapshot = create_snapshot_for_diagnostic();
        Restrictions restrictions = res_minus(res_minus(st_.restrictions, R_STMT_EXPR), R_ALLOW_LET);
        Token tok = token;
        try {
            Expr* expr = parse_expr_res(restrictions, AttrWrapper::empty()).first;
            emit("expected `{`, found " + token_descr(tok), tok.span);
            return expr;
        } catch (PErr&) {
            restore_snapshot(snapshot);
        }
    }
    Span body_lo = token.span;
    return parse_expr_block(std::nullopt, body_lo, BlockCheckMode::Default);
}

CaptureBy Parser::parse_capture_clause(Span& kw_span) {
    if (eat_keyword(KW(Move))) {
        kw_span = prev_token.span;
        if (check_keyword(KW(Async)))
            fail("the order of `move` and `async` is incorrect", token.span.with_lo(prev_token.span.lo));
        return CaptureBy::Value;
    }
    if (eat_keyword(KW(Use))) {
        kw_span = prev_token.span;
        if (check_keyword(KW(Async)))
            fail("the order of `use` and `async` is incorrect", token.span.with_lo(prev_token.span.lo));
        return CaptureBy::Use;
    }
    return CaptureBy::Ref;
}

FnDecl* Parser::parse_fn_block_decl(Span& arg_span) {
    uint32_t arg_start = token.span.lo;
    std::vector<Param*> inputs;
    if (!eat(EXP(OrOr))) {
        expect(EXP(Or));
        inputs = parse_seq_to_before_tokens<Param*>({EXP(Or)}, {Tok::OrOr}, SeqSep::trailing_allowed(EXP(Comma)),
                                                    [this] { return parse_fn_block_param(); });
        expect_or();
    }
    arg_span = prev_token.span.with_lo(arg_start);
    FnRetTy output = parse_ret_ty(AllowPlus::Yes, RecoverQPath::Yes, RecoverReturnSign::Yes);
    FnDecl* d = mk<FnDecl>();
    d->inputs = std::move(inputs);
    d->output = output;
    return d;
}

Param* Parser::parse_fn_block_param() {
    Span lo = token.span;
    AttrWrapper attrs = parse_outer_attributes();
    Pat* pat = parse_pat_no_top_alt(Expected::ParameterName, PatternLocation::None);
    Ty* ty = eat(EXP(Colon)) ? parse_ty() : mk_ty(pat->span, TyKind::Infer);
    Param* p = mk<Param>();
    p->attrs = std::move(attrs.attrs);
    p->ty = ty;
    p->pat = pat;
    p->span = lo.to(prev_token.span);
    return p;
}

Expr* Parser::parse_expr_if() {
    Span lo = prev_token.span;
    Expr* cond = parse_expr_cond(LetChainsPolicy::EditionDependent);
    return parse_if_after_cond(lo, cond);
}

Expr* Parser::parse_if_after_cond(Span lo, Expr* cond) {
    Span cond_span = cond->span;
    // recover_block_from_condition
    auto recover_block_from_condition = [&]() -> Block* {
        Expr* blk_expr = nullptr;
        if (cond->kind == ExprKind::Binary && cond->rhs->kind == ExprKind::Block && !cond->rhs->has_label) {
            Span binop_span = cond->op_span;
            emit("this `if` expression is missing a block after the condition", lo);
            blk_expr = cond->rhs;
            cond->rhs = mk_expr_err(binop_span.shrink_to_hi());
        } else if (cond->kind == ExprKind::Block && !cond->has_label) {
            emit("missing condition for `if` expression", lo.shrink_to_hi());
            blk_expr = cond;
            cond = mk_expr_err(cond_span.shrink_to_hi());
        } else {
            return nullptr;
        }
        return blk_expr->block;
    };
    Block* thn;
    if (tok_is_keyword(token, Kw::Else)) {
        if (Block* b = recover_block_from_condition()) {
            thn = b;
        } else {
            emit("this `if` expression is missing a block after the condition", lo);
            thn = mk_block_err(cond_span.shrink_to_hi());
        }
    } else {
        AttrWrapper attrs = parse_outer_attributes();  // For recovery.
        if (check(EXP(OpenBrace))) {
            thn = parse_block();
        } else if (Block* b = recover_block_from_condition()) {
            thn = b;
        } else {
            error_on_extra_if(cond);
            thn = parse_block();
        }
        error_on_if_block_attrs(lo, false, thn->span, attrs);
    }
    Expr* els = nullptr;
    if (eat_keyword(KW(Else))) els = parse_expr_else();
    Expr* e = mk_expr(lo.to(prev_token.span), ExprKind::If);
    e->lhs = cond;
    e->block = thn;
    e->rhs = els;
    return e;
}

Expr* Parser::parse_expr_cond(LetChainsPolicy let_chains_policy) {
    AttrWrapper attrs = parse_outer_attributes();
    Expr* cond = parse_expr_res(static_cast<Restrictions>(R_NO_STRUCT_LITERAL | R_ALLOW_LET), std::move(attrs)).first;
    CondChecker checker(*this, let_chains_policy);
    checker.visit(cond);
    if (checker.found_incorrect_let_chain) return mk_expr_err(cond->span);
    return cond;
}

Expr* Parser::parse_expr_let(Restrictions restrictions) {
    bool recovered = false;
    if (!res_contains(restrictions, R_ALLOW_LET)) {
        if (prev_token.kind == Tok::Or) fail("expected expression, found `let` statement", token.span);
        emit("expected expression, found `let` statement", token.span);
        recovered = true;
    }
    bump();  // `let`
    Span lo = prev_token.span;
    Pat* pat = parse_pat_no_top_guard(Expected::None, RecoverComma::Yes, RecoverColon::Yes,
                                      CommaRecoveryMode::LikelyTuple);
    if (token.kind == Tok::EqEq) {
        emit("expected `=`, found `==`", token.span);
        bump();
    } else {
        expect(EXP(Eq));
    }
    AttrWrapper attrs = parse_outer_attributes();
    Expr* expr = parse_expr_assoc_with(PrecBound::excluded(kPrecLetScrutineeNeedsPar), std::move(attrs)).first;
    Span span = lo.to(expr->span);
    Expr* e = mk_expr(span, ExprKind::Let);
    e->pat = pat;
    e->lhs = expr;
    e->op_span = lo;
    e->recovered = recovered;
    return e;
}

Expr* Parser::parse_expr_else() {
    Span else_span = prev_token.span;
    AttrWrapper attrs = parse_outer_attributes();  // For recovery.
    Expr* expr;
    if (eat_keyword(KW(If))) {
        expr = parse_expr_if();
    } else if (check(EXP(OpenBrace))) {
        expr = parse_simple_block();
    } else {
        ParserState snapshot = create_snapshot_for_diagnostic();
        std::string first_tok = token_descr(token);
        Span first_tok_span = token.span;
        Expr* cond = nullptr;
        try {
            cond = parse_expr();
        } catch (PErr&) {
            cond = nullptr;
        }
        if (cond != nullptr && check(EXP(OpenBrace)) &&
            (expr_requires_semi_to_be_stmt(cond) || cond->kind == ExprKind::MacCall)) {
            emit("expected `{`, found " + first_tok, first_tok_span);
            expr = parse_if_after_cond(cond->span.shrink_to_lo(), cond);
        } else {
            restore_snapshot(snapshot);
            expr = parse_simple_block();
        }
    }
    error_on_if_block_attrs(else_span, true, expr->span, attrs);
    return expr;
}

void Parser::error_on_if_block_attrs(Span, bool, Span, const AttrWrapper& attrs) {
    if (!attrs.is_empty()) emit("outer attributes are not allowed on `if` and `else` branches", attrs.attrs.back().span);
}

void Parser::error_on_extra_if(const Expr* cond) {
    if (cond->kind == ExprKind::Binary && cond->binop == BinOpKind::And && cond->rhs->kind == ExprKind::If)
        fail("unexpected `if` in the condition expression", cond->op_span.shrink_to_hi().to(cond->rhs->lhs->span.shrink_to_lo()));
}

std::pair<Pat*, Expr*> Parser::parse_for_head() {
    bool begin_paren = token.kind == Tok::OpenParen;
    Span start_span = token.span;
    Pat* pat = nullptr;
    try {
        pat = parse_pat_allow_top_guard(Expected::None, RecoverComma::Yes, RecoverColon::Yes,
                                        CommaRecoveryMode::LikelyTuple);
    } catch (PErr& err) {
        if (begin_paren && eat_keyword(KW(In))) {
            AttrWrapper attrs = parse_outer_attributes();
            Expr* expr = nullptr;
            try {
                expr = parse_expr_res(R_NO_STRUCT_LITERAL, std::move(attrs)).first;
            } catch (PErr&) {
                throw err;
            }
            if (token.kind == Tok::CloseParen) {
                Span right = prev_token.span.between(look_ahead_tok(1).span);
                Span close = token.span;
                bump();  // )
                emit("unexpected parentheses surrounding `for` loop head", start_span);
                (void)close;
                return {mk_pat(start_span.to(right), PatKind::Wild), expr};
            }
            throw err;
        }
        throw;
    }
    if (!eat_keyword(KW(In))) error_missing_in_for_loop();
    check_for_for_in_in_typo(prev_token.span);
    AttrWrapper attrs = parse_outer_attributes();
    Expr* expr = parse_expr_res(R_NO_STRUCT_LITERAL, std::move(attrs)).first;
    return {pat, expr};
}

Expr* Parser::parse_expr_for(std::optional<Label> opt_label, Span lo) {
    bool is_await = edition_ >= Edition::E2018 && eat_keyword(KW(Await));
    ForLoopKind kind = is_await ? ForLoopKind::ForAwait : ForLoopKind::For;
    std::pair<Pat*, Expr*> head = parse_for_head();
    Pat* pat = head.first;
    Expr* expr = head.second;
    auto mk_for = [&](Span sp, Expr* iter, Block* body) {
        Expr* e = mk_expr(sp, ExprKind::ForLoop);
        e->pat = pat;
        e->lhs = iter;
        e->block = body;
        e->for_kind = kind;
        if (opt_label) {
            e->has_label = true;
            e->label = *opt_label;
        }
        return e;
    };
    if (expr->kind == ExprKind::Block && token.kind != Tok::OpenBrace && may_recover()) {
        emit("missing expression to iterate on in `for` loop", expr->span.shrink_to_lo());
        Expr* err_expr = mk_expr_err(expr->span);
        Block* block = mk_block({}, BlockCheckMode::Default, prev_token.span);
        return mk_for(lo.to(prev_token.span), err_expr, block);
    }
    std::pair<AttrVec, Block*> r = parse_inner_attrs_and_block(opt_label ? OptSpan() : OptSpan(lo));
    Expr* e = mk_for(Span(), expr, r.second);
    recover_loop_else("for", lo);
    e->span = lo.to(prev_token.span);
    e->attrs = std::move(r.first);
    return e;
}

void Parser::recover_loop_else(const char* loop_kind, Span) {
    if (tok_is_keyword(token, Kw::Else) && may_recover()) {
        Span else_span = token.span;
        bump();
        Expr* else_clause = parse_expr_else();
        emit(std::string("`") + loop_kind + "...else` loops are not supported", else_span.to(else_clause->span));
    }
}

void Parser::error_missing_in_for_loop() {
    Span span;
    if (tok_is_ident_named(token, "of")) {
        span = token.span;
        bump();
    } else if (eat(EXP(Eq))) {
        span = prev_token.span;
    } else {
        span = prev_token.span.between(token.span);
    }
    emit("missing `in` in `for` loop", span);
}

Expr* Parser::parse_expr_while(std::optional<Label> opt_label, Span lo) {
    Expr* cond = parse_expr_cond(LetChainsPolicy::EditionDependent);
    std::pair<AttrVec, Block*> r = parse_inner_attrs_and_block(opt_label ? OptSpan() : OptSpan(lo));
    recover_loop_else("while", lo);
    Expr* e = mk_expr_with_attrs(lo.to(prev_token.span), ExprKind::While, std::move(r.first));
    e->lhs = cond;
    e->block = r.second;
    if (opt_label) {
        e->has_label = true;
        e->label = *opt_label;
    }
    return e;
}

Expr* Parser::parse_expr_loop(std::optional<Label> opt_label, Span lo) {
    Span loop_span = prev_token.span;
    std::pair<AttrVec, Block*> r = parse_inner_attrs_and_block(opt_label ? OptSpan() : OptSpan(lo));
    recover_loop_else("loop", lo);
    Expr* e = mk_expr_with_attrs(lo.to(prev_token.span), ExprKind::Loop, std::move(r.first));
    e->block = r.second;
    e->kw_span = loop_span;
    if (opt_label) {
        e->has_label = true;
        e->label = *opt_label;
    }
    return e;
}

std::optional<Label> Parser::eat_label() {
    if (token.kind != Tok::Lifetime) return std::nullopt;
    Ident ident = tok_ident(token);
    if (!token.raw) {
        Ident bare = ident;
        bare.name = ident.name.substr(1);
        if (ident_is_reserved(bare)) emit("labels cannot use keyword names", ident.span);
    }
    bump();
    Label l;
    l.ident = ident;
    return l;
}

Expr* Parser::parse_expr_match() {
    Span match_span = prev_token.span;
    AttrWrapper attrs = parse_outer_attributes();
    Expr* scrutinee = parse_expr_res(R_NO_STRUCT_LITERAL, std::move(attrs)).first;
    return parse_match_block(match_span, match_span, scrutinee, MatchKind::Prefix);
}

Expr* Parser::parse_match_block(Span lo, Span, Expr* scrutinee, MatchKind match_kind) {
    try {
        expect(EXP(OpenBrace));
    } catch (PErr&) {
        if (try_recover_unexpected_block_label(OptSpan())) {
            bump();
        } else {
            throw;
        }
    }
    AttrVec attrs = parse_inner_attributes();
    std::vector<Arm> arms;
    auto mk_match = [&](Span sp) {
        Expr* e = mk_expr_with_attrs(sp, ExprKind::Match, std::move(attrs));
        e->lhs = scrutinee;
        e->arms = std::move(arms);
        e->match_kind = match_kind;
        return e;
    };
    while (token.kind != Tok::CloseBrace) {
        try {
            arms.push_back(parse_arm());
        } catch (PErr& e) {
            emit(e);
            recover_stmt();
            Span span = lo.to(token.span);
            if (token.kind == Tok::CloseBrace) bump();
            Arm arm;
            arm.pat = mk_pat(span, PatKind::Err);
            arm.body = mk_expr_err(span);
            arm.span = span;
            arms.push_back(std::move(arm));
            return mk_match(span);
        }
    }
    Span hi = token.span;
    bump();
    return mk_match(lo.to(hi));
}

std::optional<Span> Parser::parse_arm_body_missing_braces(Expr* first_expr, Span) {
    if (token.kind != Tok::Semi) return std::nullopt;
    ParserState start_snapshot = create_snapshot_for_diagnostic();
    bump();  // `;`
    std::vector<Span> stmts{first_expr->span};
    auto err = [&]() {
        Span span = stmts.front().to(stmts.back());
        emit("`match` arm body without braces", span);
        return span;
    };
    while (true) {
        if (token.kind == Tok::CloseBrace) return err();
        if (token.kind == Tok::Comma) {
            restore_snapshot(start_snapshot);
            return std::nullopt;
        }
        ParserState pre_pat_snapshot = create_snapshot_for_diagnostic();
        try {
            parse_pat_no_top_alt(Expected::None, PatternLocation::None);
            if (token.kind == Tok::FatArrow) {
                restore_snapshot(pre_pat_snapshot);
                return err();
            }
        } catch (PErr&) {
        }
        restore_snapshot(pre_pat_snapshot);
        Stmt* stmt = nullptr;
        try {
            stmt = parse_stmt_without_recovery(true, ForceCollect::No, false);
        } catch (PErr&) {
            restore_snapshot(start_snapshot);
            break;
        }
        if (stmt == nullptr) {
            restore_snapshot(start_snapshot);
            break;
        }
        stmts.push_back(stmt->span);
    }
    return std::nullopt;
}

Arm Parser::parse_arm() {
    AttrWrapper attrs = parse_outer_attributes();
    Span lo = token.span;
    std::pair<Pat*, Guard*> pg = parse_match_arm_pat_and_guard();
    Pat* pat = pg.first;
    Span span_before_body = prev_token.span;
    Expr* arm_body = nullptr;
    bool is_fat_arrow = check(EXP(FatArrow));
    Tok k = token.kind;
    bool is_almost_fat_arrow = k == Tok::Eq || k == Tok::RArrow || k == Tok::Ge || k == Tok::Gt;
    bool armless = (!is_fat_arrow && !is_almost_fat_arrow && pat_could_be_never_pattern(pat)) ||
                   token.kind == Tok::Comma || token.kind == Tok::CloseBrace;
    std::optional<PErr> result;
    if (armless) {
        try {
            expect_one_of({EXP(Comma)}, {EXP(CloseBrace)});
        } catch (PErr& e) {
            result = e;
        }
    } else {
        try {
            expect(EXP(FatArrow));
        } catch (PErr& err) {
            if (!is_almost_fat_arrow) throw;
            if (!(prev_token.kind == Tok::DotDotEq && token.kind == Tok::Gt)) emit(err);
            bump();
        }
        Span arrow_span = prev_token.span;
        AttrWrapper battrs = parse_outer_attributes();
        Expr* expr = parse_expr_res(R_STMT_EXPR, std::move(battrs)).first;
        bool require_comma = !classify_expr_is_complete(expr) && token.kind != Tok::CloseBrace;
        if (!require_comma) {
            arm_body = expr;
            (void)eat(EXP(Comma));
        } else if (std::optional<Span> sp = parse_arm_body_missing_braces(expr, arrow_span)) {
            arm_body = mk_expr_err(*sp);
        } else {
            arm_body = expr;
            try {
                expect_one_of({EXP(Comma)}, {EXP(CloseBrace)});
            } catch (PErr& e) {
                result = e;
            }
        }
    }
    Span hi_span = arm_body ? arm_body->span : span_before_body;
    Span arm_span = lo.to(hi_span);
    bool recover_missing_comma = arm_body != nullptr || pat_could_be_never_pattern(pat);
    if (result && recover_missing_comma) {
        ParserState saved = create_snapshot_for_diagnostic();
        bool pattern_follows = false;
        try {
            parse_pat_no_top_guard(Expected::None, RecoverComma::Yes, RecoverColon::Yes,
                                   CommaRecoveryMode::EitherTupleOrPipe);
            pattern_follows = true;
        } catch (PErr&) {
        }
        bool fat = pattern_follows && check(EXP(FatArrow));
        restore_snapshot(saved);
        if (fat) {
            emit("expected `,` following `match` arm", arm_span.shrink_to_hi());
            result.reset();
        }
    }
    if (result) throw *result;
    Arm arm;
    arm.attrs = std::move(attrs.attrs);
    arm.pat = pat;
    arm.guard = pg.second;
    arm.body = arm_body;
    arm.span = arm_span;
    arm.has_comma = prev_token.kind == Tok::Comma && prev_token.span.lo >= arm_span.hi;
    return arm;
}

Guard* Parser::parse_match_arm_guard() {
    if (Guard* g = eat_metavar_guard()) return g;
    if (!eat_keyword(KW(If))) return nullptr;
    return expect_match_arm_guard_cond();
}

Guard* Parser::expect_match_arm_guard_cond() {
    Span leading_if_span = prev_token.span;
    Expr* cond = parse_match_guard_condition();
    Span cond_span = cond->span;
    CondChecker checker(*this, LetChainsPolicy::AlwaysAllowed);
    checker.visit(cond);
    Guard* g = mk<Guard>();
    g->cond = cond;
    g->span_with_leading_if = leading_if_span.to(cond_span);
    return g;
}

std::pair<Pat*, Guard*> Parser::parse_match_arm_pat_and_guard() {
    if (token.kind == Tok::OpenParen) {
        Span left = token.span;
        Pat* pat = parse_pat_no_top_guard(Expected::None, RecoverComma::Yes, RecoverColon::Yes,
                                          CommaRecoveryMode::EitherTupleOrPipe);
        if (pat->kind == PatKind::Paren && pat->sub != nullptr && pat->sub->kind == PatKind::Guard) {
            Span span = pat->span;
            Guard* guard = pat->sub->guard;
            CondChecker checker(*this, LetChainsPolicy::AlwaysAllowed);
            checker.visit(guard->cond);
            emit("unexpected parentheses surrounding `match` arm pattern", left);
            if (checker.found_incorrect_let_chain) guard->cond = mk_expr_err(guard->span_with_leading_if);
            return {mk_pat(span, PatKind::Wild), guard};
        }
        return {pat, parse_match_arm_guard()};
    }
    Pat* pat = parse_pat_no_top_guard(Expected::None, RecoverComma::Yes, RecoverColon::Yes,
                                      CommaRecoveryMode::EitherTupleOrPipe);
    return {pat, parse_match_arm_guard()};
}

Expr* Parser::parse_match_guard_condition() {
    AttrWrapper attrs = parse_outer_attributes();
    try {
        return parse_expr_res(static_cast<Restrictions>(R_ALLOW_LET | R_IN_IF_GUARD), std::move(attrs)).first;
    } catch (PErr&) {
        if (prev_token.kind == Tok::OpenBrace) {
            recover_stmt_(SemiColonMode::Ignore, BlockMode::Ignore);
            (void)eat(EXP(CloseBrace));
        }
        throw;
    }
}

Expr* Parser::parse_try_block(Span span_lo) {
    Ty* annotation = eat_keyword(SYM(Bikeshed)) ? parse_ty() : nullptr;
    std::pair<AttrVec, Block*> r = parse_inner_attrs_and_block(OptSpan());
    if (eat_keyword(KW(Catch))) fail("keyword `catch` cannot follow a `try` block", prev_token.span);
    Span span = span_lo.to(r.second->span);
    Expr* e = mk_expr_with_attrs(span, ExprKind::TryBlock, std::move(r.first));
    e->block = r.second;
    e->ty = annotation;
    return e;
}

bool Parser::is_do_catch_block() const {
    return tok_is_keyword(token, Kw::Do) && is_keyword_ahead(1, {Kw::Catch}) &&
           look_ahead_tok(2).kind == Tok::OpenBrace && !res_contains(st_.restrictions, R_NO_STRUCT_LITERAL);
}

bool Parser::is_do_yeet() const { return tok_is_keyword(token, Kw::Do) && is_keyword_ahead(1, {Kw::Yeet}); }

bool Parser::is_try_block() const {
    if (!tok_is_keyword(token, Kw::Try)) return false;
    const Token& t = look_ahead_tok(1);
    return (t.kind == Tok::OpenBrace || (t.kind == Tok::Ident && !t.raw && t.sym == "bikeshed")) &&
           edition_ >= Edition::E2018;
}

Expr* Parser::parse_gen_block() {
    Span lo = token.span;
    GenBlockKind kind;
    if (eat_keyword(KW(Async))) {
        kind = eat_keyword(KW(Gen)) ? GenBlockKind::AsyncGen : GenBlockKind::Async;
    } else {
        eat_keyword(KW(Gen));
        kind = GenBlockKind::Gen;
    }
    Span kw;
    CaptureBy capture = parse_capture_clause(kw);
    Span decl_span = lo.to(prev_token.span);
    std::pair<AttrVec, Block*> r = parse_inner_attrs_and_block(OptSpan());
    Expr* e = mk_expr_with_attrs(lo.to(prev_token.span), ExprKind::Gen, std::move(r.first));
    e->capture = capture;
    e->block = r.second;
    e->gen_kind = kind;
    e->kw_span = decl_span;
    return e;
}

bool Parser::is_gen_block(Kw kw, size_t lookahead) const {
    if (!is_keyword_ahead(lookahead, {kw})) return false;
    return (is_keyword_ahead(lookahead + 1, {Kw::Move, Kw::Use}) &&
            look_ahead_tok(lookahead + 2).kind == Tok::OpenBrace) ||
           look_ahead_tok(lookahead + 1).kind == Tok::OpenBrace;
}

bool Parser::is_async_gen_block() const { return tok_is_keyword(token, Kw::Async) && is_gen_block(Kw::Gen, 1); }

bool Parser::is_likely_struct_lit() const {
    return look_ahead_tok(1).kind == Tok::Ident &&
           (look_ahead_tok(2).kind == Tok::Comma || look_ahead_tok(2).kind == Tok::Colon);
}

Expr* Parser::maybe_parse_struct_expr(QSelf* qself, Path* path, bool& some) {
    bool struct_allowed = !res_contains(st_.restrictions, R_NO_STRUCT_LITERAL);
    bool likely = is_likely_struct_lit();
    some = false;
    if (!struct_allowed && !likely) return nullptr;
    if (struct_allowed) {
        some = true;
        expect(EXP(OpenBrace));
        return parse_expr_struct(qself, path, true);
    }
    ParserState snapshot = create_snapshot_for_diagnostic();
    some = true;
    expect(EXP(OpenBrace));
    try {
        Expr* expr = parse_expr_struct(qself, path, false);
        emit("struct literals are not allowed here", expr->span);
        return expr;
    } catch (PErr&) {
        restore_snapshot(snapshot);
        some = false;
        return nullptr;
    }
}

Expr* Parser::maybe_recover_bad_struct_literal_path(bool is_underscore_entry_point) {
    if (may_recover() && check_noexpect(Tok::OpenBrace) && !res_contains(st_.restrictions, R_NO_STRUCT_LITERAL) &&
        is_likely_struct_lit()) {
        Span span = is_underscore_entry_point ? prev_token.span : token.span.shrink_to_lo();
        bump();  // {
        Ident us;
        us.name = "_";
        us.span = span;
        Expr* expr = parse_expr_struct(nullptr, path_from_ident(arena_, us), false);
        if (is_underscore_entry_point)
            emit("placeholder `_` is not allowed for the path in struct literals", span);
        else
            emit("struct literal body without path", expr->span);
        return mk_expr_err(expr->span);
    }
    return nullptr;
}

Parser::StructFields Parser::parse_struct_fields(Path* pth, bool recover, ExpTok close) {
    StructFields out;
    bool in_if_guard = res_contains(st_.restrictions, R_IN_IF_GUARD);
    bool is_async = path_is_single_kw(pth, "async");
    while (token.kind != close.tok) {
        if (eat(EXP(DotDot)) || recover_struct_field_dots(close.tok)) {
            Span exp_span = prev_token.span;
            if (check(close)) {
                out.rest = StructRestKind::Rest;
                out.rest_span = prev_token.span;
                break;
            }
            try {
                Expr* e = parse_expr();
                out.rest = StructRestKind::Base;
                out.base = e;
                out.rest_span = exp_span;
            } catch (PErr& e) {
                if (!recover) throw;
                emit(e);
                recover_stmt();
            }
            recover_struct_comma_after_dotdot(exp_span);
            break;
        }
        std::optional<Ident> peek;
        if (token.kind == Tok::Ident) {
            Ident id = tok_ident(token);
            if ((!ident_is_reserved(id) || token.raw) && look_ahead_tok(1).kind == Tok::Colon) peek = id;
        }
        auto field_ident = [&]() -> std::optional<ExprField> {
            if (!peek) return std::nullopt;
            ExprField f;
            f.ident = *peek;
            f.span = peek->span;
            f.expr = mk_expr_err(peek->span);
            return f;
        };
        std::optional<ExprField> parsed_field;
        bool parse_failed = false;
        try {
            parsed_field = parse_expr_field();
        } catch (PErr& e) {
            if (in_if_guard && close.tt == TokenType::CloseBrace) throw;
            if (!recover) throw;
            emit(e);
            parse_failed = true;
            if (is_async) out.recovered_async = true;
            out.rest = StructRestKind::NoneWithError;
            if (token.kind != Tok::Comma) {
                recover_stmt_(SemiColonMode::Comma, BlockMode::Ignore);
                if (token.kind != Tok::Comma) break;
            }
        }
        bool is_shorthand = parsed_field && parsed_field->is_shorthand;
        check_or_expected(!is_shorthand, TokenType::Colon);
        try {
            expect_one_of({EXP(Comma)}, {close});
            if (parsed_field) {
                out.fields.push_back(std::move(*parsed_field));
            } else if (parse_failed) {
                if (std::optional<ExprField> f = field_ident()) out.fields.push_back(std::move(*f));
            }
        } catch (PErr& e) {
            if (!recover) throw;
            emit(e);
            if (is_async) {
                out.recovered_async = true;
            } else if (std::optional<ExprField> f = field_ident()) {
                out.fields.push_back(std::move(*f));
            }
            out.rest = StructRestKind::NoneWithError;
            recover_stmt_(SemiColonMode::Comma, BlockMode::Ignore);
            (void)eat(EXP(Comma));
        }
    }
    return out;
}

Expr* Parser::parse_expr_struct(QSelf* qself, Path* pth, bool recover) {
    Span lo = pth->span;
    StructFields sf = parse_struct_fields(pth, recover, EXP(CloseBrace));
    Span span = lo.to(token.span);
    expect(EXP(CloseBrace));
    if (sf.recovered_async) return mk_expr_err(span);
    Expr* e = mk_expr(span, ExprKind::Struct);
    e->qself = qself;
    e->path = pth;
    e->struct_fields = std::move(sf.fields);
    e->rest = sf.rest;
    e->rest_span = sf.rest_span;
    e->rhs = sf.base;
    return e;
}

void Parser::recover_struct_comma_after_dotdot(Span span) {
    if (token.kind != Tok::Comma) return;
    emit("cannot use a comma after the base struct", span.to(prev_token.span));
    recover_stmt();
}

bool Parser::recover_struct_field_dots(Tok close) {
    if (look_ahead_tok(1).kind != close && eat(EXP(DotDotDot))) {
        emit("expected `..`, found `...`", prev_token.span);
        return true;
    }
    return false;
}

Label Parser::recover_ident_into_label(Ident ident) {
    file_.interned.emplace_back("'" + std::string(ident.name));
    Label l;
    l.ident.name = file_.interned.back();
    l.ident.span = ident.span;
    emit("expected a label, found an identifier", ident.span);
    return l;
}

ExprField Parser::parse_expr_field() {
    AttrWrapper attrs = parse_outer_attributes();
    recover_vcs_conflict_marker();
    Span lo = token.span;
    Tok n1 = look_ahead_tok(1).kind;
    bool is_shorthand = !(n1 == Tok::Colon || n1 == Tok::Eq);
    bool is_wrong = tok_is_non_reserved_ident(token) &&
                    !(n1 == Tok::Colon || n1 == Tok::Eq || n1 == Tok::Comma || n1 == Tok::CloseBrace ||
                      n1 == Tok::CloseParen);
    if (is_wrong) {
        const Token& t1 = look_ahead_tok(1);
        fail("expected one of `,`, `:`, or `}`, found `" + token_str(t1) + "`", t1.span);
    }
    ExprField f;
    if (is_shorthand) {
        Ident ident = parse_ident_common(false);
        f.ident = ident;
        f.expr = mk_expr(ident.span, ExprKind::Path);
        f.expr->path = path_from_ident(arena_, ident);
    } else {
        Ident ident = parse_field_name();
        error_on_eq_field_init(ident);
        bump();  // `:`
        f.ident = ident;
        f.expr = parse_expr();
    }
    f.span = lo.to(f.expr->span);
    f.is_shorthand = is_shorthand;
    f.attrs = std::move(attrs.attrs);
    return f;
}

void Parser::error_on_eq_field_init(Ident) {
    if (token.kind != Tok::Eq) return;
    emit("expected `:`, found `=`", token.span);
}

void Parser::err_dotdotdot_syntax(Span span) { emit("unexpected token: `...`", span); }

void Parser::err_larrow_operator(Span span) { emit("unexpected token: `<-`", span); }

Expr* Parser::mk_range(Span span, Expr* start, Expr* end, RangeLimits limits) {
    if (end == nullptr && limits == RangeLimits::Closed) {
        inclusive_range_with_incorrect_end();
        return mk_expr_err(span);
    }
    Expr* e = mk_expr(span, ExprKind::Range);
    e->lhs = start;
    e->rhs = end;
    e->limits = limits;
    return e;
}

Expr* Parser::mk_await_expr(Expr* self_arg, Span lo) {
    Span span = lo.to(prev_token.span);
    Expr* e = mk_expr(span, ExprKind::Await);
    e->lhs = self_arg;
    e->kw_span = prev_token.span;
    recover_from_await_method_call();
    return e;
}

Expr* Parser::mk_use_expr(Expr* self_arg, Span lo) {
    Span span = lo.to(prev_token.span);
    Expr* e = mk_expr(span, ExprKind::Use);
    e->lhs = self_arg;
    e->kw_span = prev_token.span;
    recover_from_use();
    return e;
}

Span Parser::mk_expr_sp(const Expr* lhs, Span lhs_span, Span op_span, Span rhs_span) const {
    Span s = lhs_span;
    for (const Attribute& a : lhs->attrs) {
        if (a.style == AttrStyle::Outer) {
            s = a.span;
            break;
        }
    }
    return s.to(op_span).to(rhs_span);
}

// ===========================================================================
// diagnostics.rs (the expression parts)

PErr Parser::expected_expression_found() {
    if (token.kind == Tok::Eof && subparser_name_ != nullptr)
        return make_err(std::string("expected expression, found end of ") + subparser_name_,
                        prev_token.span.shrink_to_hi());
    return make_err("expected expression, found " + token_descr(token), token.span);
}

void Parser::maybe_recover_unexpected_block_label() { (void)try_recover_unexpected_block_label(OptSpan()); }

bool Parser::try_recover_unexpected_block_label(OptSpan) {
    if (!(check_lifetime() && look_ahead_tok(1).kind == Tok::Colon && look_ahead_tok(2).kind == Tok::OpenBrace))
        return false;
    std::optional<Label> label = eat_label();
    bump();  // `:`
    Span span = label->ident.span.to(prev_token.span);
    emit("block label not supported here", span);
    return true;
}

void Parser::maybe_recover_from_ternary_operator(OptSpan cond) {
    if (prev_token.kind != Tok::Question) return;
    Span question = prev_token.span;
    uint32_t lo = cond.has ? cond.span.lo : question.lo;
    ParserState snapshot = create_snapshot_for_diagnostic();
    bool go;
    try {
        parse_expr();
        go = true;
    } catch (PErr&) {
        go = token.kind == Tok::Colon;
    }
    if (go && eat_noexpect(Tok::Colon)) {
        bool ok = false;
        try {
            parse_expr();
            ok = true;
        } catch (PErr&) {
        }
        if (ok) fail("Rust has no ternary operator", prev_token.span.with_lo(lo));
    }
    restore_snapshot(snapshot);
}

bool Parser::is_vcs_conflict_marker(Tok long_kind, Tok short_kind) const {
    if (long_kind == short_kind) {
        for (size_t i = 0; i < 7; ++i)
            if (look_ahead_tok(i).kind != long_kind) return false;
        return true;
    }
    for (size_t i = 0; i < 3; ++i)
        if (look_ahead_tok(i).kind != long_kind) return false;
    Tok k = look_ahead_tok(3).kind;
    return k == short_kind || k == long_kind;
}

bool Parser::conflict_marker(Tok long_kind, Tok short_kind, size_t expected, Span& out_span, size_t& out_len) {
    if (!is_vcs_conflict_marker(long_kind, short_kind)) return false;
    Span lo = token.span;
    // span_to_margin(lo) != Some(0): the indentation of lo's line.
    size_t line_start = lo.lo;
    while (line_start > 0 && src_[line_start - 1] != '\n') --line_start;
    size_t indent = 0;
    for (size_t i = line_start; i < lo.lo; ++i) {
        char c = src_[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            ++indent;
        } else {
            indent = 0;
            break;
        }
    }
    if (indent != 0) return false;
    size_t len = 0;
    while (token.kind == long_kind || token.kind == short_kind) {
        Tok k = token.kind;
        bool two = k == Tok::Shl || k == Tok::Shr || k == Tok::OrOr || k == Tok::EqEq;
        len += two ? 2 : 1;
        bump();
        if (expected != 0 && expected == len) break;
    }
    if (expected != 0 && expected != len) return false;
    out_span = lo.to(prev_token.span);
    out_len = len;
    return true;
}

void Parser::recover_vcs_conflict_marker() {
    Span start;
    size_t len = 0;
    if (!conflict_marker(Tok::Shl, Tok::Lt, 0, start, len)) return;
    while (token.kind != Tok::Eof) {
        Span sp;
        size_t l = 0;
        (void)conflict_marker(Tok::OrOr, Tok::Or, len, sp, l);
        (void)conflict_marker(Tok::EqEq, Tok::Eq, len, sp, l);
        if (conflict_marker(Tok::Shr, Tok::Gt, len, sp, l)) break;
        bump();
    }
    emit("encountered diff marker", start);
    throw FatalAbort{};
}

void Parser::recover_stmt() { recover_stmt_(SemiColonMode::Ignore, BlockMode::Ignore); }

void Parser::recover_stmt_(SemiColonMode break_on_semi, BlockMode break_on_block) {
    int64_t brace_depth = 0;
    int64_t bracket_depth = 0;
    bool in_block = false;
    while (true) {
        switch (token.kind) {
        case Tok::OpenBrace:
            ++brace_depth;
            bump();
            if (break_on_block == BlockMode::Break && brace_depth == 1 && bracket_depth == 0) in_block = true;
            break;
        case Tok::OpenBracket:
            ++bracket_depth;
            bump();
            break;
        case Tok::CloseBrace:
            if (brace_depth == 0) return;
            --brace_depth;
            bump();
            if (in_block && bracket_depth == 0 && brace_depth == 0) return;
            break;
        case Tok::CloseBracket:
            --bracket_depth;
            if (bracket_depth < 0) bracket_depth = 0;
            bump();
            break;
        case Tok::Eof: return;
        case Tok::Semi:
            bump();
            if (break_on_semi == SemiColonMode::Break && brace_depth == 0 && bracket_depth == 0) return;
            break;
        case Tok::Comma:
            if (break_on_semi == SemiColonMode::Comma && brace_depth == 0 && bracket_depth == 0) return;
            bump();
            break;
        default: bump(); break;
        }
    }
}

Expr* Parser::check_no_chained_comparison(Expr* inner_op, const AssocOp& outer_op) {
    if (!(inner_op->kind == ExprKind::Binary && binop_is_comparison(inner_op->binop))) return nullptr;
    BinOpKind op = inner_op->binop;
    Span op_span = inner_op->op_span;
    const char* msg = "comparison operators cannot be chained";
    auto mk_err_expr = [&]() { return mk_expr_err(inner_op->span.to(prev_token.span)); };
    if ((op == BinOpKind::Lt && op_is_binary(outer_op, BinOpKind::Lt)) || op_is_binary(outer_op, BinOpKind::Gt)) {
        if (op_is_binary(outer_op, BinOpKind::Lt)) {
            ParserState snapshot = create_snapshot_for_diagnostic();
            bump();
            consume_tts(1, {{Tok::Lt, 1}, {Tok::Gt, -1}, {Tok::Shr, -2}});
            if (!(token.kind == Tok::OpenParen || token.kind == Tok::PathSep)) restore_snapshot(snapshot);
        }
        if (token.kind == Tok::PathSep) {
            ParserState snapshot = create_snapshot_for_diagnostic();
            bump();  // `::`
            bool ok = false;
            try {
                parse_expr();
                ok = true;
            } catch (PErr&) {
            }
            if (ok) {
                emit(msg, op_span);
                return mk_err_expr();
            }
            restore_snapshot(snapshot);
            fail(msg, op_span);
        } else if (token.kind == Tok::OpenParen) {
            if (!consume_fn_args()) fail(msg, op_span);
            emit(msg, op_span);
            return mk_err_expr();
        } else {
            bool recovered = attempt_chained_comparison_suggestion(inner_op, outer_op);
            if (recovered) {
                emit(msg, op_span);
                return mk_err_expr();
            }
            fail(msg, op_span);
        }
    }
    bool recovered = attempt_chained_comparison_suggestion(inner_op, outer_op);
    emit(msg, op_span);
    if (recovered) return mk_err_expr();
    return nullptr;
}

bool Parser::attempt_chained_comparison_suggestion(Expr* inner_op, const AssocOp& outer_op) {
    if (inner_op->kind != ExprKind::Binary) return false;
    const Expr* l1 = inner_op->lhs;
    const Expr* r1 = inner_op->rhs;
    if (l1->kind == ExprKind::Field && r1->kind != ExprKind::Lit) {
        bool numeric = !l1->ident.name.empty();
        for (char c : l1->ident.name)
            if (c < '0' || c > '9') numeric = false;
        if (!numeric) return false;
    }
    BinOpKind op = inner_op->binop;
    auto outer_is = [&](std::initializer_list<BinOpKind> ks) {
        if (outer_op.kind != AssocOp::Binary) return false;
        for (BinOpKind k : ks)
            if (outer_op.bin == k) return true;
        return false;
    };
    if ((op == BinOpKind::Eq && outer_is({BinOpKind::Eq})) ||
        ((op == BinOpKind::Lt || op == BinOpKind::Le) && outer_is({BinOpKind::Lt, BinOpKind::Le})) ||
        ((op == BinOpKind::Gt || op == BinOpKind::Ge) && outer_is({BinOpKind::Gt, BinOpKind::Ge})))
        return false;
    if (op == BinOpKind::Eq && outer_is({BinOpKind::Lt, BinOpKind::Le, BinOpKind::Gt, BinOpKind::Ge})) {
        ParserState snapshot = create_snapshot_for_diagnostic();
        try {
            parse_expr();
        } catch (PErr&) {
            restore_snapshot(snapshot);
        }
        return true;
    }
    if ((op == BinOpKind::Lt || op == BinOpKind::Le || op == BinOpKind::Gt || op == BinOpKind::Ge) &&
        outer_is({BinOpKind::Eq})) {
        ParserState snapshot = create_snapshot_for_diagnostic();
        try {
            parse_expr();
            return true;
        } catch (PErr&) {
            restore_snapshot(snapshot);
            return false;
        }
    }
    return false;
}

void Parser::consume_tts(int64_t acc, std::initializer_list<std::pair<Tok, int64_t>> modifier) {
    while (acc > 0) {
        for (const std::pair<Tok, int64_t>& m : modifier) {
            if (token.kind == m.first) {
                acc += m.second;
                break;
            }
        }
        if (token.kind == Tok::Eof) break;
        bump();
    }
}

bool Parser::consume_fn_args() {
    ParserState snapshot = create_snapshot_for_diagnostic();
    bump();  // `(`
    consume_tts(1, {{Tok::OpenParen, 1}, {Tok::CloseParen, -1}});
    if (token.kind == Tok::Eof) {
        restore_snapshot(snapshot);
        return false;
    }
    return true;
}

void Parser::check_turbofish_missing_angle_brackets(PathSegment& segment) {
    if (!may_recover()) return;
    if (token.kind == Tok::PathSep && segment.args == nullptr) {
        ParserState snapshot = create_snapshot_for_diagnostic();
        bump();
        Span lo = token.span;
        // parse_angle_args(None) lives in path.rs and is not shared; this is
        // the same loop over lifetimes / const args / types without the
        // associated-item-constraint forms.
        std::vector<AngleBracketedArg> args;
        bool ok = true;
        try {
            while (true) {
                AngleBracketedArg a;
                if (check_lifetime()) {
                    a.arg.kind = GenericArg::Lifetime;
                    a.arg.lifetime = expect_lifetime();
                } else if (check_const_arg()) {
                    a.arg.kind = GenericArg::Const;
                    a.arg.konst = parse_const_arg();
                } else if (check_type()) {
                    a.arg.kind = GenericArg::Type;
                    a.arg.ty = parse_ty();
                } else {
                    break;
                }
                args.push_back(a);
                if (!eat(EXP(Comma))) break;
            }
        } catch (PErr&) {
            ok = false;
        }
        if (!ok) {
            restore_snapshot(snapshot);
            return;
        }
        Span span = lo.to(prev_token.span);
        while (token.kind == Tok::Shr || token.kind == Tok::Gt) bump();
        if (token.kind == Tok::OpenParen) {
            GenericArgs* ga = mk<GenericArgs>();
            ga->kind = GenericArgs::AngleBracketed;
            ga->span = span;
            ga->args = std::move(args);
            segment.args = ga;
            emit("generic parameters without surrounding angle brackets", span);
        } else {
            restore_snapshot(snapshot);
        }
    }
}

void Parser::check_for_for_in_in_typo(Span) {
    if (eat_keyword(KW(In))) emit("expected iterable, found keyword `in`", prev_token.span);
}

Expr* Parser::recover_closure_body(const PErr& err, const Token& before, const Token&, const Token& tok, Span lo,
                                   Span) {
    if (before.kind == Tok::OpenBrace && tok.kind != Tok::OpenBrace) {
        emit(err);
        eat_to_tokens({EXP(CloseBrace)});
    } else if (before.kind == Tok::OpenParen && tok.kind != Tok::OpenBrace) {
        eat_to_tokens({EXP(CloseParen), EXP(Comma)});
        emit(err);
    } else {
        throw err;
    }
    return mk_expr_err(lo.to(token.span));
}

void Parser::eat_to_tokens(std::initializer_list<ExpTok> closes) {
    try {
        parse_seq_to_before_tokens<Span>(closes, {}, SeqSep::none(), [this] { return parse_token_tree(); });
    } catch (PErr&) {
    }
}

void Parser::recover_from_await_method_call() {
    if (token.kind == Tok::OpenParen && look_ahead_tok(1).kind == Tok::CloseParen) {
        Span lo = token.span;
        bump();  // (
        Span span = lo.to(token.span);
        bump();  // )
        emit("incorrect use of `await`", span);
    }
}

void Parser::recover_from_use() {
    if (token.kind == Tok::OpenParen && look_ahead_tok(1).kind == Tok::CloseParen) {
        Span lo = token.span;
        bump();  // (
        Span span = lo.to(token.span);
        bump();  // )
        emit("incorrect use of `use`", span);
    }
}

[[noreturn]] Expr* Parser::recover_from_inc_dec(Expr*, bool is_inc, bool is_pre, Span op_span, bool) {
    fail(std::string("Rust has no ") + (is_pre ? "prefix" : "postfix") + " " + (is_inc ? "increment" : "decrement") +
             " operator",
         op_span);
}

Expr* Parser::recover_seq_parse_error(ExpTok open, ExpTok close, Span lo, const PErr& err) {
    emit(err);
    consume_block(open, close, true);
    return mk_expr_err(lo.to(prev_token.span));
}

void Parser::consume_block(ExpTok open, ExpTok close, bool consume_close) {
    int64_t brace_depth = 0;
    while (true) {
        if (eat(open)) {
            ++brace_depth;
        } else if (check(close)) {
            if (brace_depth == 0) {
                if (consume_close) bump();
                return;
            }
            bump();
            --brace_depth;
        } else if (token.kind == Tok::Eof) {
            return;
        } else {
            bump();
        }
    }
}

Expr* Parser::try_macro_suggestion() {
    bool is_try = tok_is_keyword(token, Kw::Try);
    bool is_questionmark = look_ahead_tok(1).kind == Tok::Bang;
    bool is_open = look_ahead_tok(2).kind == Tok::OpenParen;
    if (is_try && is_questionmark && is_open) {
        Span lo = token.span;
        bump();  // try
        bump();  // !
        bump();  // (
        consume_block(EXP(OpenParen), EXP(CloseParen), false);
        Span hi = token.span;
        bump();  // )
        emit("use of deprecated `try` macro", lo.to(hi));
        return mk_expr_err(lo.to(hi));
    }
    throw expected_expression_found();
}

Expr* Parser::recover_incorrect_await_syntax(Span await_sp) {
    Span hi;
    if (token.kind == Tok::Bang) {
        expect(EXP(Bang));
        expect(EXP(OpenParen));
        parse_expr();
        expect(EXP(CloseParen));
        hi = prev_token.span;
    } else {
        (void)eat(EXP(Question));
        Expr* expr = token.kind == Tok::OpenBrace ? parse_expr_block(std::nullopt, token.span, BlockCheckMode::Default)
                                                  : parse_expr();
        hi = expr->span;
    }
    Span sp = await_sp.to(hi);
    emit("incorrect use of `await`", sp);
    Expr* e = mk_expr_err(await_sp.to(sp));
    return maybe_recover_from_bad_qpath(e);
}

Ty* Parser::expr_to_ty(const Expr* e) {
    Ty* t = nullptr;
    switch (e->kind) {
    case ExprKind::Path:
        t = mk_ty(e->span, TyKind::Path);
        t->qself = e->qself;
        t->path = e->path;
        return t;
    case ExprKind::MacCall:
        t = mk_ty(e->span, TyKind::MacCall);
        t->mac = e->mac;
        return t;
    case ExprKind::Paren: {
        Ty* inner = expr_to_ty(e->lhs);
        if (!inner) return nullptr;
        t = mk_ty(e->span, TyKind::Paren);
        t->inner = inner;
        return t;
    }
    case ExprKind::AddrOf: {
        if (e->borrow != BorrowKind::Ref) return nullptr;
        Ty* inner = expr_to_ty(e->lhs);
        if (!inner) return nullptr;
        t = mk_ty(e->span, TyKind::Ref);
        t->inner = inner;
        t->mutbl = e->mutbl;
        return t;
    }
    case ExprKind::Repeat: {
        Ty* inner = expr_to_ty(e->lhs);
        if (!inner) return nullptr;
        t = mk_ty(e->span, TyKind::Array);
        t->inner = inner;
        t->len = e->anon_const;
        return t;
    }
    case ExprKind::Array: {
        if (e->exprs.size() != 1) return nullptr;
        Ty* inner = expr_to_ty(e->exprs[0]);
        if (!inner) return nullptr;
        t = mk_ty(e->span, TyKind::Slice);
        t->inner = inner;
        return t;
    }
    case ExprKind::Tup: {
        std::vector<Ty*> tys;
        for (const Expr* x : e->exprs) {
            Ty* ty = expr_to_ty(x);
            if (!ty) return nullptr;
            tys.push_back(ty);
        }
        t = mk_ty(e->span, TyKind::Tup);
        t->elems = std::move(tys);
        return t;
    }
    case ExprKind::Binary: {
        if (e->binop != BinOpKind::Add) return nullptr;
        auto to_bound = [](const Expr* x, GenericBound& b) {
            if (x->kind != ExprKind::Path || x->qself != nullptr) return false;
            b.kind = GenericBound::Trait;
            b.trait.trait_ref = x->path;
            b.trait.span = x->span;
            return true;
        };
        GenericBound l, r;
        if (!to_bound(e->lhs, l) || !to_bound(e->rhs, r)) return nullptr;
        t = mk_ty(e->span, TyKind::TraitObject);
        t->bounds.push_back(l);
        t->bounds.push_back(r);
        return t;
    }
    case ExprKind::Underscore: return mk_ty(e->span, TyKind::Infer);
    default: return nullptr;
    }
}

Expr* Parser::maybe_recover_from_bad_qpath(Expr* base) {
    if (may_recover() && token.kind == Tok::PathSep) {
        // recover_from_bad_qpath
        Ty* ty = expr_to_ty(base);
        if (ty == nullptr) return base;
        Span ty_span = ty->span;
        expect(EXP(PathSep));
        Path* path = mk_path(Span());
        parse_path_segments(path->segments, PathStyle::Expr, nullptr);
        path->span = ty_span.to(prev_token.span);
        emit("missing angle brackets in associated item path", ty_span);
        QSelf* q = mk<QSelf>();
        q->ty = ty;
        q->path_span = ty_span.shrink_to_hi();
        q->position = 0;
        Expr* e = mk_expr(path->span, ExprKind::Path);
        e->qself = q;
        e->path = path;
        return e;
    }
    return base;
}

}  // namespace detail
}  // namespace rust
