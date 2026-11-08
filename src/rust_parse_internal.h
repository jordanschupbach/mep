// rust_parse_internal.h -- the Parser class shared by rust_parse*.cpp.
//
// Ported from rustc_parse/src/parser (rust-lang/rust 1.97.1, MIT OR
// Apache-2.0). Conventions of the port:
//
//  - `PResult<'a, T>` is a C++ return value plus exceptions: `return Err(e)`
//    / `?` is `throw PErr{...}` (propagates by itself); `e.emit()` is
//    `emit(e)`; `e.cancel()` is just catching and dropping it.
//    `FatalError.raise()` is `throw FatalAbort{}`.
//  - Options become nullable pointers (Box<T>) or std::optional; tuples
//    become std::pair / small structs.
//  - Snapshots (`create_snapshot_for_diagnostic` / `restore_snapshot`) copy
//    and restore ParserState; already-emitted diagnostics are not rolled
//    back (rustc does not roll them back either).
//  - `collect_tokens(...)` wrappers do nothing: call the body directly with
//    the outer attributes.
//  - Every `check`/`eat`/`expect` that records an expected token type in
//    rustc does so here too: that is what makes "expected one of ..."
//    messages match.
//  - Diagnostics keep only the primary message and span: suggestions, notes
//    and labels are dropped.
//
// The class is split across files; each file owns the declarations in its
// rust_parse_<file>.inc, included inside the class body below.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "rust_ast.h"
#include "rust_lex.h"
#include "rust_parse.h"

namespace rust {
namespace detail {

// ---------------------------------------------------------------------------
// token_type.rs

enum class TokenType : uint8_t {
    Eq, Lt, Le, EqEq, Gt, AndAnd, OrOr, Bang, Tilde, Plus, Minus, Star, And, Or, At, Dot, DotDot, DotDotDot,
    DotDotEq, Comma, Semi, Colon, PathSep, RArrow, FatArrow, Pound, Question, OpenParen, CloseParen, OpenBrace,
    CloseBrace, OpenBracket, CloseBracket, Eof, Operator, Ident, Lifetime, Path, Type, Const,
    KwAs, KwAsync, KwAuto, KwAwait, KwBecome, KwBox, KwBreak, KwCatch, KwConst, KwContinue, KwContractEnsures,
    KwContractRequires, KwCrate, KwDefault, KwDyn, KwElse, KwEnum, KwExtern, KwFinal, KwFn, KwFor, KwGen, KwIf,
    KwImpl, KwIn, KwLet, KwLoop, KwMacro, KwMacroRules, KwMatch, KwMod, KwMove, KwMut, KwPub, KwRaw, KwRef,
    KwReturn, KwReuse, KwSafe, KwSelfUpper, KwStatic, KwStruct, KwSuper, KwTrait, KwTry, KwType, KwUnderscore,
    KwUnsafe, KwUse, KwWhere, KwWhile, KwYield,
    SymAttSyntax, SymBikeshed, SymClobberAbi, SymInlateout, SymInout, SymIs, SymLabel, SymLateout, SymMayUnwind,
    SymNomem, SymNoreturn, SymNostack, SymNull, SymOptions, SymOut, SymPin, SymPreservesFlags, SymPure,
    SymReadonly, SymSym,
    Count_
};
std::string token_type_to_string(TokenType t);

struct ExpTok {
    Tok tok;
    TokenType tt;
};
struct ExpKw {
    Kw kw;
    TokenType tt;
    const char* name;  // for keyword-like symbols without a Kw id (bikeshed ...): compared by text
};

// exp!(Comma) -> EXP(Comma); exp!(Fn) -> KW(Fn); exp!(Is) -> SYM(Is)
#define EXP(name) (::rust::detail::ExpTok{::rust::Tok::name, ::rust::detail::TokenType::name})
#define KW(name) (::rust::detail::ExpKw{::rust::Kw::name, ::rust::detail::TokenType::Kw##name, nullptr})
#define SYM(name) (::rust::detail::ExpKw{::rust::Kw::Sym##name, ::rust::detail::TokenType::Sym##name, nullptr})

class TokenTypeSet {
public:
    bool is_empty() const { return lo_ == 0 && hi_ == 0; }
    void insert(TokenType t) {
        unsigned i = static_cast<unsigned>(t);
        if (i < 64)
            lo_ |= uint64_t{1} << i;
        else
            hi_ |= uint64_t{1} << (i - 64);
    }
    void clear() { lo_ = hi_ = 0; }
    bool contains(TokenType t) const {
        unsigned i = static_cast<unsigned>(t);
        return i < 64 ? (lo_ >> i) & 1u : (hi_ >> (i - 64)) & 1u;
    }
    std::vector<TokenType> items() const {
        std::vector<TokenType> v;
        for (unsigned i = 0; i < static_cast<unsigned>(TokenType::Count_); ++i)
            if (contains(static_cast<TokenType>(i))) v.push_back(static_cast<TokenType>(i));
        return v;
    }

private:
    uint64_t lo_ = 0, hi_ = 0;
};

// ---------------------------------------------------------------------------
// Errors.

/// A diagnostic created but not yet emitted (rustc's `Diag`).
struct PErr {
    std::string msg;
    Span span;
};
/// FatalError.raise(): abandon the parse (what was emitted so far stands).
struct FatalAbort {};

// ---------------------------------------------------------------------------
// Parser enums (mod.rs, path.rs, pat.rs, ty.rs, item.rs, attr.rs, ...).

using Restrictions = uint8_t;
constexpr Restrictions R_EMPTY = 0;
constexpr Restrictions R_STMT_EXPR = 1 << 0;
constexpr Restrictions R_NO_STRUCT_LITERAL = 1 << 1;
constexpr Restrictions R_CONST_EXPR = 1 << 2;
constexpr Restrictions R_ALLOW_LET = 1 << 3;
constexpr Restrictions R_IN_IF_GUARD = 1 << 4;
constexpr Restrictions R_IS_PAT = 1 << 5;

enum class SemiColonMode : uint8_t { Break, Ignore, Comma };
enum class BlockMode : uint8_t { Break, Ignore };
enum class ForceCollect : uint8_t { Yes, No };
enum class AllowConstBlockItems : uint8_t { Yes, No, DoesNotMatter };
enum class Recovery : uint8_t { Allowed, Forbidden };
enum class FollowedByType : uint8_t { Yes, No };
enum class Trailing : uint8_t { No, Yes };
enum class Case : uint8_t { Sensitive, Insensitive };
enum class PathStyle : uint8_t { Expr, Pat, Type, Mod };
/// pat.rs `Option<Expected>` (None = no expectation).
enum class Expected : uint8_t { None, ParameterName, ArgumentName, Identifier, BindingPattern };
enum class RecoverComma : uint8_t { Yes, No };
enum class RecoverColon : uint8_t { Yes, No };
enum class CommaRecoveryMode : uint8_t { LikelyTuple, EitherTupleOrPipe };
/// pat.rs `Option<PatternLocation>` (None = no location).
enum class PatternLocation : uint8_t { None, LetBinding, FunctionParameter };
enum class AllowPlus : uint8_t { Yes, No };
enum class RecoverQPath : uint8_t { Yes, No };
enum class RecoverReturnSign : uint8_t { Yes, OnlyFatArrow, No };
enum class AllowCVariadic : uint8_t { Yes, No };
enum class FnContext : uint8_t { Free, Trait, Impl };
enum class IsDotDotDot : uint8_t { Yes, No };
/// item.rs `ReqName = fn(Edition, IsDotDotDot) -> bool`, as the closures rustc passes.
enum class ReqName : uint8_t { Always, Never, Edition2018, NotDotDotDot };
struct FnParseMode {
    ReqName req_name = ReqName::Always;
    FnContext context = FnContext::Free;
    bool req_body = true;
};
enum class FrontMatterParsingMode : uint8_t { Function, FunctionPtrType };
enum class InnerAttrForbiddenReasonKind : uint8_t { InCodeBlock, AfterOuterDocComment, AfterOuterAttribute };
struct InnerAttrPolicy {
    bool permitted = true;
    bool has_reason = false;
    InnerAttrForbiddenReasonKind reason = InnerAttrForbiddenReasonKind::InCodeBlock;
    Span prev_span;
    static InnerAttrPolicy Permitted() { return InnerAttrPolicy(); }
    static InnerAttrPolicy Forbidden() {
        InnerAttrPolicy p;
        p.permitted = false;
        return p;
    }
    static InnerAttrPolicy Forbidden(InnerAttrForbiddenReasonKind r, Span sp = Span()) {
        InnerAttrPolicy p;
        p.permitted = false;
        p.has_reason = true;
        p.reason = r;
        p.prev_span = sp;
        return p;
    }
};
enum class AttemptLocalParseRecovery : uint8_t { Yes, No };
enum class LetChainsPolicy : uint8_t { AlwaysAllowed, EditionDependent };
enum class UsePreAttrPos : uint8_t { No, Yes };

/// rustc_ast::util::parser::ExprPrecedence
enum class ExprPrecedence : uint8_t {
    Jump, Assign, Range, LOr, LAnd, Compare, BitOr, BitXor, BitAnd, Shift, Sum, Product, Cast, Prefix, Unambiguous
};
/// std::ops::Bound<ExprPrecedence>
struct PrecBound {
    enum Kind : uint8_t { Unbounded, Included, Excluded } kind = Unbounded;
    ExprPrecedence prec = ExprPrecedence::Jump;
    static PrecBound unbounded() { return PrecBound(); }
    static PrecBound included(ExprPrecedence p) {
        PrecBound b;
        b.kind = Included;
        b.prec = p;
        return b;
    }
    static PrecBound excluded(ExprPrecedence p) {
        PrecBound b;
        b.kind = Excluded;
        b.prec = p;
        return b;
    }
};

/// rustc_ast::util::parser::AssocOp (with the operator's span when checked).
struct AssocOp {
    enum Kind : uint8_t { Binary, AssignOp, Assign, Cast, Range } kind = Binary;
    BinOpKind bin = BinOpKind::Add;
    AssignOpKind assign = AssignOpKind::AddAssign;
    RangeLimits range = RangeLimits::HalfOpen;
    Span span;
};

/// An optional span.
struct OptSpan {
    bool has = false;
    Span span;
    OptSpan() = default;
    explicit OptSpan(Span s) : has(true), span(s) {}
};

/// attr_wrapper.rs AttrWrapper: the outer attributes of the next node.
struct AttrWrapper {
    AttrVec attrs;
    uint32_t start_pos = 0;
    bool has_start_pos = false;
    bool is_empty() const { return attrs.empty(); }
    static AttrWrapper empty() { return AttrWrapper(); }
};

struct SeqSep {
    bool has_sep = false;
    ExpTok sep{Tok::Comma, TokenType::Comma};
    bool trailing_sep_allowed = false;
    static SeqSep trailing_allowed(ExpTok s) {
        SeqSep r;
        r.has_sep = true;
        r.sep = s;
        r.trailing_sep_allowed = true;
        return r;
    }
    static SeqSep none() { return SeqSep(); }
};

struct ClosureSpans {
    Span whole_closure;
    Span closing_pipe;
    Span body;
};

/// The parser's mutable state, copied by snapshots.
struct ParserState {
    Token token;
    Spacing token_spacing = Spacing::Alone;
    Token prev_token;
    Restrictions restrictions = R_EMPTY;
    TokenTypeSet expected_token_types;
    uint32_t pos = 0;  // index of the next token to read
    uint32_t num_bump_calls = 0;
    uint32_t break_last_token = 0;
    uint16_t unmatched_angle_bracket_count = 0;
    uint16_t angle_bracket_nesting = 0;
    bool parsing_generics = false;
    OptSpan last_unexpected_token_span;
    bool has_current_closure = false;
    ClosureSpans current_closure;
    Recovery recovery = Recovery::Allowed;
    bool in_fn_body = false;
    bool fn_body_missing_semi_guar = false;
};

// ---------------------------------------------------------------------------
// The parser.

class Parser {
public:
    Parser(ParseResult& file, uint32_t tok_lo, uint32_t tok_hi, const char* subparser_name);

    // Entry points (rust_parse.cpp).
    Crate* parse_crate_root();
    void finish();  // flush late (stashed) errors

    /// Diagnostics emitted so far, in order.
    std::vector<PErr> diags;

    // ===== mod.rs: the core (rust_parse.cpp) ===============================

    ParseResult& file_;
    std::string_view src_;
    const Token* toks_;
    const uint32_t* partner_;
    uint32_t tok_lo_;
    uint32_t tok_hi_;   // index of the token that acts as Eof
    Edition edition_;
    const char* subparser_name_;
    Arena& arena_;
    ParserState st_;
    // Shortcuts to the hot state (references into st_).
    Token& token;
    Token& prev_token;
    uint32_t depth_ = 0;
    uint32_t max_depth_;
    uint64_t work_ = 0;
    uint64_t work_budget_;
    std::vector<PErr> late_;  // stashed lexer errors (LifetimeIsChar ...)

    template <class T>
    T* mk() { return arena_.make<T>(); }

    // -- errors
    [[noreturn]] void fail(std::string msg, Span sp) { throw PErr{std::move(msg), sp}; }
    void emit(std::string msg, Span sp) { diags.push_back(PErr{std::move(msg), sp}); }
    void emit(const PErr& e) { diags.push_back(e); }
    PErr make_err(std::string msg, Span sp) const { return PErr{std::move(msg), sp}; }
    /// try_steal_modify_and_emit_err: emit the stashed error at `sp` now, if any.
    bool steal_late_error(Span sp);

    // -- snapshots
    ParserState create_snapshot_for_diagnostic() const { return st_; }
    void restore_snapshot(const ParserState& s) { st_ = s; }

    /// Depth guard for the recursive productions.
    struct DepthGuard {
        Parser& p;
        explicit DepthGuard(Parser& pp) : p(pp) {
            if (++p.depth_ > p.max_depth_) {
                --p.depth_;
                p.fail("nesting too deep for mep's parser", p.token.span);
            }
        }
        ~DepthGuard() { --p.depth_; }
        DepthGuard(const DepthGuard&) = delete;
        DepthGuard& operator=(const DepthGuard&) = delete;
    };

    // -- tokens
    std::string_view text(Span s) const { return src_.substr(s.lo, s.hi - s.lo); }
    Token next_token_from_vec(uint32_t idx) const;  // toks_[idx], or Eof past the end
    void bump();
    void bump_with(const Token& next, Spacing spacing);
    /// look_ahead(dist, |t| ...): the token `dist` ahead (0 = current).
    const Token& look_ahead_tok(size_t dist) const;
    template <class F>
    auto look_ahead(size_t dist, F f) const { return f(look_ahead_tok(dist)); }
    bool is_keyword_ahead(size_t dist, std::initializer_list<Kw> kws) const;
    /// Index of the current token in toks_ (meaningless for split tokens).
    uint32_t cur_index() const { return st_.pos - 1; }

    bool may_recover() const { return st_.recovery == Recovery::Allowed; }
    template <class F>
    auto with_recovery(Recovery r, F f) {
        Recovery old = st_.recovery;
        st_.recovery = r;
        struct Restore {
            Parser& p;
            Recovery old;
            ~Restore() { p.st_.recovery = old; }
        } restore{*this, old};
        return f();
    }
    template <class F>
    auto with_res(Restrictions r, F f) {
        Restrictions old = st_.restrictions;
        st_.restrictions = r;
        struct Restore {
            Parser& p;
            Restrictions old;
            ~Restore() { p.st_.restrictions = old; }
        } restore{*this, old};
        return f();
    }

    [[noreturn]] void unexpected_any();
    [[noreturn]] void unexpected() { unexpected_any(); }
    /// Ok(Recovered::No) -> false, Ok(Recovered::Yes) -> true.
    bool expect(ExpTok exp);
    bool expect_one_of(std::initializer_list<ExpTok> edible, std::initializer_list<ExpTok> inedible);
    Ident parse_ident();
    Ident parse_ident_common(bool recover);
    /// ident_or_err: the current token's ident (and raw flag) or the recovery.
    std::pair<Ident, bool> ident_or_err(bool recover);
    bool check(ExpTok exp);
    bool check_noexpect(Tok t) const { return token.kind == t; }
    bool check_noexpect_past_close_delim(Tok t) const;
    bool eat_noexpect(Tok t) {
        bool p = check_noexpect(t);
        if (p) bump();
        return p;
    }
    bool eat(ExpTok exp) {
        bool p = check(exp);
        if (p) bump();
        return p;
    }
    bool check_keyword(ExpKw exp);
    bool check_keyword_case(ExpKw exp, Case c);
    bool eat_keyword(ExpKw exp);
    bool eat_keyword_case(ExpKw exp, Case c);
    bool eat_keyword_noexpect(Kw k);
    void expect_keyword(ExpKw exp);
    bool is_kw_followed_by_ident(Kw k) const;
    bool check_or_expected(bool ok, TokenType tt) {
        if (!ok) st_.expected_token_types.insert(tt);
        return ok;
    }
    bool check_ident() { return check_or_expected(tok_is_ident(token), TokenType::Ident); }
    bool check_path() { return check_or_expected(tok_is_path_start(token), TokenType::Path); }
    bool check_type() { return check_or_expected(tok_can_begin_type(token), TokenType::Type); }
    bool check_const_arg();
    bool check_const_closure() const;
    bool check_inline_const(size_t dist) const;
    bool check_plus() { return check_or_expected(token.kind == Tok::Plus || token.kind == Tok::PlusEq, TokenType::Plus); }
    bool break_and_eat(ExpTok exp);
    bool eat_plus() { return break_and_eat(EXP(Plus)); }
    void expect_and() {
        if (!break_and_eat(EXP(And))) unexpected();
    }
    void expect_or() {
        if (!break_and_eat(EXP(Or))) unexpected();
    }
    bool eat_lt();
    void expect_lt() {
        if (!eat_lt()) unexpected();
    }
    void expect_gt();
    bool expect_any_with_type(std::initializer_list<ExpTok> closes_expected, std::initializer_list<Tok> closes_not_expected);

    /// parse_seq_to_before_tokens. `f` parses one element and returns it.
    template <class T, class F>
    std::vector<T> parse_seq_to_before_tokens(std::initializer_list<ExpTok> closes_expected,
                                              std::initializer_list<Tok> closes_not_expected, SeqSep sep, F f,
                                              Trailing* trailing_out = nullptr, bool* recovered_out = nullptr);
    template <class T, class F>
    std::vector<T> parse_seq_to_before_end(ExpTok close, SeqSep sep, F f, Trailing* trailing = nullptr,
                                           bool* recovered = nullptr) {
        return parse_seq_to_before_tokens<T>({close}, {}, sep, f, trailing, recovered);
    }
    template <class T, class F>
    std::vector<T> parse_seq_to_end(ExpTok close, SeqSep sep, F f, Trailing* trailing = nullptr) {
        bool recovered = false;
        std::vector<T> v = parse_seq_to_before_end<T>(close, sep, f, trailing, &recovered);
        if (!recovered) (void)eat(close);
        return v;
    }
    template <class T, class F>
    std::vector<T> parse_unspanned_seq(ExpTok open, ExpTok close, SeqSep sep, F f, Trailing* trailing = nullptr) {
        expect(open);
        return parse_seq_to_end<T>(close, sep, f, trailing);
    }
    template <class T, class F>
    std::vector<T> parse_delim_comma_seq(ExpTok open, ExpTok close, F f, Trailing* trailing = nullptr) {
        return parse_unspanned_seq<T>(open, close, SeqSep::trailing_allowed(EXP(Comma)), f, trailing);
    }
    template <class T, class F>
    std::vector<T> parse_paren_comma_seq(F f, Trailing* trailing = nullptr) {
        return parse_delim_comma_seq<T>(EXP(OpenParen), EXP(CloseParen), f, trailing);
    }
    void recover_missing_braces_around_closure_body(ClosureSpans closure_spans, PErr expect_err);

    CoroutineKind parse_coroutine_kind(Case c);
    Safety parse_safety(Case c);
    Constness parse_constness(Case c) { return parse_constness_(c, false); }
    Constness parse_closure_constness() { return parse_constness_(Case::Sensitive, true); }
    Constness parse_constness_(Case c, bool is_closure);
    Expr* parse_const_block(Span span, bool pat);
    Mutability parse_mutability() { return eat_keyword(KW(Mut)) ? Mutability::Mut : Mutability::Not; }
    /// parse_byref: ByRef::Yes(pinnedness, mutability) or No.
    struct ByRef {
        bool yes = false;
        Pinnedness pinned = Pinnedness::Not;
        Mutability mutbl = Mutability::Not;
    };
    ByRef parse_byref();
    /// parse_mut_or_const: None when neither.
    std::optional<Mutability> parse_mut_or_const();
    Ident parse_field_name();
    DelimArgs parse_delim_args();
    AttrArgs parse_attr_args();
    std::optional<DelimArgs> parse_delim_args_inner();
    /// parse_token_tree: skips one token tree; returns [first, last] token span.
    Span parse_token_tree();
    Visibility parse_visibility(FollowedByType fbt);
    void recover_incorrect_vis_restriction();
    ImplRestriction parse_impl_restriction();
    Extern parse_extern(Case c);
    std::optional<StrLit> parse_abi();
    bool check_path_sep_and_look_ahead(bool (*looker)(const Token&));
    bool is_import_coupler();
    Span token_uninterpolated_span() const { return token.span; }
    Span prev_token_uninterpolated_span() const { return prev_token.span; }
    /// missing_semi_from_binop: Some(lhs span) when it emitted the error.
    std::optional<Span> missing_semi_from_binop(const char* kind_desc, const Expr* expr, OptSpan decl_lo);

    // -- token predicates (rustc_ast::token::Token methods; edition-aware)
    bool tok_is_ident(const Token& t) const { return t.kind == Tok::Ident; }
    bool tok_is_lifetime(const Token& t) const { return t.kind == Tok::Lifetime; }
    bool tok_is_keyword(const Token& t, Kw k) const { return t.kind == Tok::Ident && !t.raw && t.kw == k; }
    bool tok_is_ident_named(const Token& t, std::string_view name) const { return t.kind == Tok::Ident && t.sym == name; }
    bool tok_is_reserved_ident(const Token& t) const {
        return t.kind == Tok::Ident && !t.raw && kw_is_reserved(t.kw, edition_);
    }
    bool tok_is_non_reserved_ident(const Token& t) const {
        return t.kind == Tok::Ident && (t.raw || !kw_is_reserved(t.kw, edition_));
    }
    bool tok_is_used_keyword(const Token& t) const {
        return t.kind == Tok::Ident && !t.raw && kw_is_used_keyword(t.kw, edition_);
    }
    bool tok_is_unused_keyword(const Token& t) const {
        return t.kind == Tok::Ident && !t.raw && kw_is_unused_keyword(t.kw, edition_);
    }
    bool tok_is_special_ident(const Token& t) const { return t.kind == Tok::Ident && !t.raw && kw_is_special(t.kw); }
    bool tok_is_path_segment_keyword(const Token& t) const {
        return t.kind == Tok::Ident && !t.raw && kw_is_path_segment_keyword(t.kw);
    }
    bool tok_is_bool_lit(const Token& t) const {
        return t.kind == Tok::Ident && !t.raw && (t.kw == Kw::True || t.kw == Kw::False);
    }
    bool tok_is_lit(const Token& t) const { return t.kind == Tok::Literal; }
    bool tok_is_numeric_lit(const Token& t) const {
        return t.kind == Tok::Literal && (t.lit == LitKind::Integer || t.lit == LitKind::Float);
    }
    bool tok_is_integer_lit(const Token& t) const { return t.kind == Tok::Literal && t.lit == LitKind::Integer; }
    bool tok_is_qpath_start(const Token& t) const { return t.kind == Tok::Lt || t.kind == Tok::Shl; }
    bool tok_is_path_start(const Token& t) const {
        return t.kind == Tok::PathSep || tok_is_qpath_start(t) || tok_is_path_segment_keyword(t) ||
               tok_is_non_reserved_ident(t);
    }
    bool tok_is_mutability(const Token& t) const { return tok_is_keyword(t, Kw::Mut) || tok_is_keyword(t, Kw::Const); }
    bool tok_is_range_separator(const Token& t) const {
        return t.kind == Tok::DotDot || t.kind == Tok::DotDotDot || t.kind == Tok::DotDotEq;
    }
    bool tok_is_like_plus(const Token& t) const { return t.kind == Tok::Plus || t.kind == Tok::PlusEq; }
    bool tok_is_punct(const Token& t) const { return ::rust::tok_is_punct(t.kind); }
    bool tok_is_close_delim_or_eof(const Token& t) const {
        return t.kind == Tok::CloseParen || t.kind == Tok::CloseBrace || t.kind == Tok::CloseBracket || t.kind == Tok::Eof;
    }
    bool tok_is_open_delim(const Token& t) const {
        return t.kind == Tok::OpenParen || t.kind == Tok::OpenBrace || t.kind == Tok::OpenBracket;
    }
    bool ident_can_begin_expr(const Token& t) const;
    bool ident_can_begin_type(const Token& t) const;
    bool tok_can_begin_expr(const Token& t) const;
    bool tok_can_begin_pattern(const Token& t, bool pat_with_or) const;
    bool tok_can_begin_type(const Token& t) const;
    bool tok_can_begin_const_arg(const Token& t) const;
    bool tok_can_begin_item(const Token& t) const;
    bool tok_can_begin_literal_maybe_minus(const Token& t) const;
    bool tok_can_begin_string_literal(const Token& t) const { return t.kind == Tok::Literal; }
    /// Token::ident() as an AST Ident.
    Ident tok_ident(const Token& t) const {
        Ident id;
        id.name = t.sym;
        id.span = t.span;
        id.raw = t.raw;
        return id;
    }
    /// Ident::is_reserved on an AST ident (raw idents are never reserved).
    bool ident_is_reserved(const Ident& id) const {
        return !id.raw && kw_is_reserved(kw_lookup(id.name), edition_);
    }
    bool ident_is_raw_guess(const Ident& id) const;
    /// Is the source between two positions multi-line (SourceMap::is_multiline)?
    bool is_multiline(Span s) const;
    /// pprust token_to_string / token_descr.
    std::string token_str(const Token& t) const { return token_to_string(t, src_); }
    std::string token_descr(const Token& t) const;
    /// SourceMap::start_point: the first character of a span.
    Span start_point(Span s) const;

    // -- diagnostics.rs (core part, rust_parse.cpp)
    PErr expected_ident_found_err();
    std::pair<Ident, bool> expected_ident_found(bool recover);
    std::optional<std::pair<size_t, std::string_view>> is_lit_bad_ident();
    /// expected_one_of_not_found: Ok(guar) -> returns (an error was emitted, parsing goes on).
    void expected_one_of_not_found(std::initializer_list<ExpTok> edible, std::initializer_list<ExpTok> inedible);
    PErr unexpected_err(Tok t);
    bool check_too_many_raw_str_terminators(PErr& err);
    void label_expected_raw_ref() {}

    // -- AST builders shared by every file
    Expr* mk_expr(Span sp, ExprKind k) {
        Expr* e = mk<Expr>();
        e->span = sp;
        e->kind = k;
        return e;
    }
    Expr* mk_expr_with_attrs(Span sp, ExprKind k, AttrVec attrs) {
        Expr* e = mk_expr(sp, k);
        e->attrs = std::move(attrs);
        return e;
    }
    Expr* mk_expr_err(Span sp) { return mk_expr(sp, ExprKind::Err); }
    Expr* mk_unit_expr(Span sp) { return mk_expr(sp, ExprKind::Tup); }
    Expr* mk_closure_expr(Span sp, Expr* body);
    Pat* mk_pat(Span sp, PatKind k) {
        Pat* p = mk<Pat>();
        p->span = sp;
        p->kind = k;
        return p;
    }
    Ty* mk_ty(Span sp, TyKind k) {
        Ty* t = mk<Ty>();
        t->span = sp;
        t->kind = k;
        return t;
    }
    Stmt* mk_stmt(Span sp, StmtKind k) {
        Stmt* s = mk<Stmt>();
        s->span = sp;
        s->kind = k;
        return s;
    }
    Block* mk_block(std::vector<Stmt*> stmts, BlockCheckMode rules, Span sp) {
        Block* b = mk<Block>();
        b->stmts = std::move(stmts);
        b->rules = rules;
        b->span = sp;
        return b;
    }
    Block* mk_block_err(Span sp) {
        Stmt* s = mk_stmt(sp, StmtKind::Expr);
        s->expr = mk_expr_err(sp);
        std::vector<Stmt*> v{s};
        return mk_block(std::move(v), BlockCheckMode::Default, sp);
    }
    Path* mk_path(Span sp) {
        Path* p = mk<Path>();
        p->span = sp;
        return p;
    }

    /// parse_builtin(|this, lo, ident| ...): `parse` returns true and fills
    /// its result when it knows the construct, false for "unknown".
    template <class F>
    void parse_builtin(F parse);
    bool is_builtin() const { return tok_is_keyword(token, Kw::Builtin) && look_ahead(1, [](const Token& t) { return t.kind == Tok::Pound; }); }

    // ===== per-file declarations ===========================================
#include "rust_parse_expr.inc"
#include "rust_parse_item.inc"
#include "rust_parse_pat.inc"
};

// ---------------------------------------------------------------------------
// Template definitions.

template <class F>
void Parser::parse_builtin(F parse) {
    Span lo = token.span;
    bump();  // `builtin`
    bump();  // `#`
    if (token.kind != Tok::Ident || token.raw) fail("expected identifier after `builtin #`", token.span);
    Ident ident = tok_ident(token);
    bump();
    expect(EXP(OpenParen));
    if (!parse(lo, ident)) fail("unknown `builtin #` construct `" + std::string(ident.name) + "`", lo.to(ident.span));
    expect(EXP(CloseParen));
}

template <class T, class F>
std::vector<T> Parser::parse_seq_to_before_tokens(std::initializer_list<ExpTok> closes_expected,
                                                  std::initializer_list<Tok> closes_not_expected, SeqSep sep, F f,
                                                  Trailing* trailing_out, bool* recovered_out) {
    bool first = true;
    bool recovered = false;
    Trailing trailing = Trailing::No;
    std::vector<T> v;
    while (!expect_any_with_type(closes_expected, closes_not_expected)) {
        if (tok_is_close_delim_or_eof(token)) break;
        if (sep.has_sep) {
            if (first) {
                first = false;
            } else {
                bool rec = false;
                bool have_err = false;
                PErr expect_err;
                try {
                    rec = expect(sep.sep);
                } catch (PErr& e) {
                    have_err = true;
                    expect_err = e;
                }
                if (!have_err) {
                    st_.has_current_closure = false;
                    if (rec) {
                        recovered = true;
                        break;
                    }
                } else {
                    bool had_closure = st_.has_current_closure;
                    ClosureSpans closure_spans = st_.current_closure;
                    st_.has_current_closure = false;
                    if (had_closure && token.kind == Tok::Semi) {
                        recover_missing_braces_around_closure_body(closure_spans, expect_err);
                        continue;
                    }
                    // similar_tokens: Comma -> [Dot, Lt, Semi], Semi -> [Colon, Comma], Colon -> [Semi], FatArrow -> [Eq, RArrow, Ge, Gt]
                    Tok s = sep.sep.tok;
                    Tok k = token.kind;
                    bool similar = (s == Tok::Comma && (k == Tok::Dot || k == Tok::Lt || k == Tok::Semi)) ||
                                   (s == Tok::Semi && (k == Tok::Colon || k == Tok::Comma)) ||
                                   (s == Tok::Colon && k == Tok::Semi) ||
                                   (s == Tok::FatArrow && (k == Tok::Eq || k == Tok::RArrow || k == Tok::Ge || k == Tok::Gt));
                    if (similar) bump();
                    if (prev_token.kind == Tok::Ident && token.kind == Tok::DotDot) {
                        emit(expect_err);
                        break;
                    }
                    st_.last_unexpected_token_span = OptSpan();
                    bool ok = false;
                    T t{};
                    try {
                        t = f();
                        ok = true;
                    } catch (PErr&) {
                    }
                    if (ok) {
                        emit(expect_err);
                        v.push_back(t);
                        continue;
                    }
                    if (token.kind == Tok::Colon) throw expect_err;
                    if (closes_expected.size() == 1 && closes_expected.begin()->tt == TokenType::CloseParen)
                        throw expect_err;
                    emit(expect_err);
                    break;
                }
            }
        }
        if (sep.trailing_sep_allowed && expect_any_with_type(closes_expected, closes_not_expected)) {
            trailing = Trailing::Yes;
            break;
        }
        v.push_back(f());
    }
    if (trailing_out) *trailing_out = trailing;
    if (recovered_out) *recovered_out = recovered;
    return v;
}

}  // namespace detail
}  // namespace rust
