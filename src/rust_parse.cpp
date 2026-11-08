// rust_parse.cpp -- the parser core (rustc_parse/src/parser/mod.rs,
// token_type.rs and the generic parts of diagnostics.rs) and the public
// entry points of rust_parse.h.
//
// Ported from rustc_parse/rustc_lexer (rust-lang/rust 1.97.1, MIT OR
// Apache-2.0).

#include <algorithm>

#include "rust_parse_internal.h"

namespace rust {
namespace detail {

std::string token_type_to_string(TokenType t) {
    switch (t) {
    case TokenType::Eq: return "`=`";
    case TokenType::Lt: return "`<`";
    case TokenType::Le: return "`<=`";
    case TokenType::EqEq: return "`==`";
    case TokenType::Gt: return "`>`";
    case TokenType::AndAnd: return "`&&`";
    case TokenType::OrOr: return "`||`";
    case TokenType::Bang: return "`!`";
    case TokenType::Tilde: return "`~`";
    case TokenType::Plus: return "`+`";
    case TokenType::Minus: return "`-`";
    case TokenType::Star: return "`*`";
    case TokenType::And: return "`&`";
    case TokenType::Or: return "`|`";
    case TokenType::At: return "`@`";
    case TokenType::Dot: return "`.`";
    case TokenType::DotDot: return "`..`";
    case TokenType::DotDotDot: return "`...`";
    case TokenType::DotDotEq: return "`..=`";
    case TokenType::Comma: return "`,`";
    case TokenType::Semi: return "`;`";
    case TokenType::Colon: return "`:`";
    case TokenType::PathSep: return "`::`";
    case TokenType::RArrow: return "`->`";
    case TokenType::FatArrow: return "`=>`";
    case TokenType::Pound: return "`#`";
    case TokenType::Question: return "`?`";
    case TokenType::OpenParen: return "`(`";
    case TokenType::CloseParen: return "`)`";
    case TokenType::OpenBrace: return "`{`";
    case TokenType::CloseBrace: return "`}`";
    case TokenType::OpenBracket: return "`[`";
    case TokenType::CloseBracket: return "`]`";
    case TokenType::Eof: return "<eof>";
    case TokenType::Operator: return "an operator";
    case TokenType::Ident: return "identifier";
    case TokenType::Lifetime: return "lifetime";
    case TokenType::Path: return "path";
    case TokenType::Type: return "type";
    case TokenType::Const: return "a const expression";
    case TokenType::KwAs: return "`as`";
    case TokenType::KwAsync: return "`async`";
    case TokenType::KwAuto: return "`auto`";
    case TokenType::KwAwait: return "`await`";
    case TokenType::KwBecome: return "`become`";
    case TokenType::KwBox: return "`box`";
    case TokenType::KwBreak: return "`break`";
    case TokenType::KwCatch: return "`catch`";
    case TokenType::KwConst: return "`const`";
    case TokenType::KwContinue: return "`continue`";
    case TokenType::KwContractEnsures: return "`contract_ensures`";
    case TokenType::KwContractRequires: return "`contract_requires`";
    case TokenType::KwCrate: return "`crate`";
    case TokenType::KwDefault: return "`default`";
    case TokenType::KwDyn: return "`dyn`";
    case TokenType::KwElse: return "`else`";
    case TokenType::KwEnum: return "`enum`";
    case TokenType::KwExtern: return "`extern`";
    case TokenType::KwFinal: return "`final`";
    case TokenType::KwFn: return "`fn`";
    case TokenType::KwFor: return "`for`";
    case TokenType::KwGen: return "`gen`";
    case TokenType::KwIf: return "`if`";
    case TokenType::KwImpl: return "`impl`";
    case TokenType::KwIn: return "`in`";
    case TokenType::KwLet: return "`let`";
    case TokenType::KwLoop: return "`loop`";
    case TokenType::KwMacro: return "`macro`";
    case TokenType::KwMacroRules: return "`macro_rules`";
    case TokenType::KwMatch: return "`match`";
    case TokenType::KwMod: return "`mod`";
    case TokenType::KwMove: return "`move`";
    case TokenType::KwMut: return "`mut`";
    case TokenType::KwPub: return "`pub`";
    case TokenType::KwRaw: return "`raw`";
    case TokenType::KwRef: return "`ref`";
    case TokenType::KwReturn: return "`return`";
    case TokenType::KwReuse: return "`reuse`";
    case TokenType::KwSafe: return "`safe`";
    case TokenType::KwSelfUpper: return "`Self`";
    case TokenType::KwStatic: return "`static`";
    case TokenType::KwStruct: return "`struct`";
    case TokenType::KwSuper: return "`super`";
    case TokenType::KwTrait: return "`trait`";
    case TokenType::KwTry: return "`try`";
    case TokenType::KwType: return "`type`";
    case TokenType::KwUnderscore: return "`_`";
    case TokenType::KwUnsafe: return "`unsafe`";
    case TokenType::KwUse: return "`use`";
    case TokenType::KwWhere: return "`where`";
    case TokenType::KwWhile: return "`while`";
    case TokenType::KwYield: return "`yield`";
    case TokenType::SymAttSyntax: return "`att_syntax`";
    case TokenType::SymBikeshed: return "`bikeshed`";
    case TokenType::SymClobberAbi: return "`clobber_abi`";
    case TokenType::SymInlateout: return "`inlateout`";
    case TokenType::SymInout: return "`inout`";
    case TokenType::SymIs: return "`is`";
    case TokenType::SymLabel: return "`label`";
    case TokenType::SymLateout: return "`lateout`";
    case TokenType::SymMayUnwind: return "`may_unwind`";
    case TokenType::SymNomem: return "`nomem`";
    case TokenType::SymNoreturn: return "`noreturn`";
    case TokenType::SymNostack: return "`nostack`";
    case TokenType::SymNull: return "`null`";
    case TokenType::SymOptions: return "`options`";
    case TokenType::SymOut: return "`out`";
    case TokenType::SymPin: return "`pin`";
    case TokenType::SymPreservesFlags: return "`preserves_flags`";
    case TokenType::SymPure: return "`pure`";
    case TokenType::SymReadonly: return "`readonly`";
    case TokenType::SymSym: return "`sym`";
    case TokenType::Count_: break;
    }
    return "";
}

// ---------------------------------------------------------------------------
// Construction and the token cursor.

Parser::Parser(ParseResult& file, uint32_t tok_lo, uint32_t tok_hi, const char* subparser_name)
    : file_(file),
      src_(*file.source),
      toks_(file.tokens.data()),
      partner_(file.partner.data()),
      tok_lo_(tok_lo),
      tok_hi_(tok_hi),
      edition_(file.edition),
      subparser_name_(subparser_name),
      arena_(*file.arena),
      token(st_.token),
      prev_token(st_.prev_token),
      max_depth_(file.options.max_depth ? file.options.max_depth : 256) {
    uint64_t n = tok_hi >= tok_lo ? tok_hi - tok_lo : 0;
    work_budget_ = file.options.work_budget ? file.options.work_budget : 64 * n + 1000000;
    st_.pos = tok_lo;
    st_.token.span = Span(0, 0);
    st_.prev_token.span = Span(0, 0);
    bump();
    st_.num_bump_calls = 0;
    // A fresh parser's prev_token is the dummy (`?` at DUMMY_SP).
    st_.prev_token = Token();
    st_.prev_token.kind = Tok::Question;
    st_.prev_token.span = Span(0, 0);
}

Token Parser::next_token_from_vec(uint32_t idx) const {
    if (idx < tok_hi_) return toks_[idx];
    Token eof;
    eof.kind = Tok::Eof;
    if (tok_hi_ < file_.tokens.size()) {
        const Token& t = toks_[tok_hi_];
        eof.span = t.kind == Tok::Eof ? t.span : Span(t.span.lo, t.span.lo);
    } else {
        uint32_t end = static_cast<uint32_t>(src_.size());
        eof.span = Span(end, end);
    }
    return eof;
}

void Parser::bump() {
    Token next = next_token_from_vec(st_.pos);
    // rustc's Eof comes with DUMMY_SP, which bump() replaces by the span of
    // the token before it ("better diagnostics"): errors at end of input
    // point at the last token.
    if (next.kind == Tok::Eof && st_.pos > tok_lo_) next.span = st_.token.span;
    if (st_.pos < tok_hi_) ++st_.pos;
    ++st_.num_bump_calls;
    st_.break_last_token = 0;
    if (++work_ > work_budget_) {
        work_ = 0;
        throw PErr{"parser work budget exceeded", token.span};
    }
    st_.prev_token = st_.token;
    st_.token = next;
    st_.token_spacing = next.spacing;
    st_.expected_token_types.clear();
}

void Parser::bump_with(const Token& next, Spacing spacing) {
    st_.prev_token = st_.token;
    st_.token = next;
    st_.token_spacing = spacing;
    st_.expected_token_types.clear();
}

const Token& Parser::look_ahead_tok(size_t dist) const {
    if (dist == 0) return token;
    size_t idx = st_.pos + dist - 1;
    if (idx < tok_hi_) return toks_[idx];
    static thread_local Token eof;
    eof = next_token_from_vec(tok_hi_);
    return eof;
}

bool Parser::is_keyword_ahead(size_t dist, std::initializer_list<Kw> kws) const {
    const Token& t = look_ahead_tok(dist);
    for (Kw k : kws)
        if (tok_is_keyword(t, k)) return true;
    return false;
}

bool Parser::steal_late_error(Span sp) {
    for (size_t i = 0; i < late_.size(); ++i) {
        if (late_[i].span == sp) {
            emit(late_[i]);
            late_.erase(late_.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

void Parser::finish() {
    for (const PErr& e : late_) emit(e);
    late_.clear();
}

// ---------------------------------------------------------------------------
// check / eat / expect.

void Parser::unexpected_any() {
    expect_one_of({}, {});
    throw FatalAbort{};
}

bool Parser::expect(ExpTok exp) {
    if (st_.expected_token_types.is_empty()) {
        if (token.kind == exp.tok) {
            bump();
            return false;
        }
        throw unexpected_err(exp.tok);
    }
    return expect_one_of({exp}, {});
}

bool Parser::expect_one_of(std::initializer_list<ExpTok> edible, std::initializer_list<ExpTok> inedible) {
    for (const ExpTok& e : edible)
        if (e.tok == token.kind) {
            bump();
            return false;
        }
    for (const ExpTok& e : inedible)
        if (e.tok == token.kind) return false;
    if (token.kind != Tok::Eof && st_.last_unexpected_token_span.has && st_.last_unexpected_token_span.span == token.span)
        throw FatalAbort{};
    expected_one_of_not_found(edible, inedible);
    return true;
}

Ident Parser::parse_ident() { return parse_ident_common(may_recover()); }

Ident Parser::parse_ident_common(bool recover) {
    std::pair<Ident, bool> r = ident_or_err(recover);
    if (!r.second && ident_is_reserved(r.first)) {
        PErr err = expected_ident_found_err();
        if (recover)
            emit(err);
        else
            throw err;
    }
    bump();
    return r.first;
}

std::pair<Ident, bool> Parser::ident_or_err(bool recover) {
    if (token.kind == Tok::Ident) return {tok_ident(token), token.raw};
    return expected_ident_found(recover);
}

bool Parser::check(ExpTok exp) {
    bool present = token.kind == exp.tok;
    if (!present) st_.expected_token_types.insert(exp.tt);
    return present;
}

bool Parser::check_noexpect_past_close_delim(Tok t) const {
    // The token after the enclosing delimited group's close delimiter.
    // Only reachable for interpolated (macro) tokens in rustc; approximate.
    (void)t;
    return false;
}

bool Parser::check_keyword(ExpKw exp) {
    bool is_kw = tok_is_keyword(token, exp.kw);
    if (!is_kw) st_.expected_token_types.insert(exp.tt);
    return is_kw;
}

static bool eq_ignore_ascii_case(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

bool Parser::check_keyword_case(ExpKw exp, Case c) {
    if (check_keyword(exp)) return true;
    return c == Case::Insensitive && token.kind == Tok::Ident && !token.raw && eq_ignore_ascii_case(token.sym, kw_str(exp.kw));
}

bool Parser::eat_keyword(ExpKw exp) {
    bool is_kw = check_keyword(exp);
    if (is_kw) bump();
    return is_kw;
}

bool Parser::eat_keyword_case(ExpKw exp, Case c) {
    if (eat_keyword(exp)) return true;
    if (c == Case::Insensitive && token.kind == Tok::Ident && !token.raw && eq_ignore_ascii_case(token.sym, kw_str(exp.kw))) {
        emit("keyword `" + std::string(kw_str(exp.kw)) + "` is written in the wrong case", token.span);
        bump();
        return true;
    }
    return false;
}

bool Parser::eat_keyword_noexpect(Kw k) {
    bool is_kw = tok_is_keyword(token, k);
    if (is_kw) bump();
    return is_kw;
}

void Parser::expect_keyword(ExpKw exp) {
    if (!eat_keyword(exp)) unexpected();
}

bool Parser::is_kw_followed_by_ident(Kw k) const {
    return tok_is_keyword(token, k) && tok_is_non_reserved_ident(look_ahead_tok(1));
}

bool Parser::check_const_arg() {
    bool is_mcg_arg = check_or_expected(tok_can_begin_const_arg(token), TokenType::Const);
    bool is_mgca_arg = is_keyword_ahead(0, {Kw::Const}) && look_ahead_tok(1).kind == Tok::OpenBrace;
    return is_mcg_arg || is_mgca_arg;
}

bool Parser::check_const_closure() const {
    if (!is_keyword_ahead(0, {Kw::Const})) return false;
    const Token& t = look_ahead_tok(1);
    if (t.kind == Tok::Ident && !t.raw && (t.kw == Kw::Move || t.kw == Kw::Use || t.kw == Kw::Static)) return true;
    return t.kind == Tok::OrOr || t.kind == Tok::Or;
}

bool Parser::check_inline_const(size_t dist) const {
    return is_keyword_ahead(dist, {Kw::Const}) && look_ahead_tok(dist + 1).kind == Tok::OpenBrace;
}

// TokenKind::break_two_token_op(1)
static bool break_two_token_op(Tok k, Tok& first, Tok& second) {
    switch (k) {
    case Tok::Le: first = Tok::Lt; second = Tok::Eq; return true;
    case Tok::EqEq: first = Tok::Eq; second = Tok::Eq; return true;
    case Tok::Ne: first = Tok::Bang; second = Tok::Eq; return true;
    case Tok::Ge: first = Tok::Gt; second = Tok::Eq; return true;
    case Tok::AndAnd: first = Tok::And; second = Tok::And; return true;
    case Tok::OrOr: first = Tok::Or; second = Tok::Or; return true;
    case Tok::Shl: first = Tok::Lt; second = Tok::Lt; return true;
    case Tok::Shr: first = Tok::Gt; second = Tok::Gt; return true;
    case Tok::PlusEq: first = Tok::Plus; second = Tok::Eq; return true;
    case Tok::MinusEq: first = Tok::Minus; second = Tok::Eq; return true;
    case Tok::StarEq: first = Tok::Star; second = Tok::Eq; return true;
    case Tok::SlashEq: first = Tok::Slash; second = Tok::Eq; return true;
    case Tok::PercentEq: first = Tok::Percent; second = Tok::Eq; return true;
    case Tok::CaretEq: first = Tok::Caret; second = Tok::Eq; return true;
    case Tok::AndEq: first = Tok::And; second = Tok::Eq; return true;
    case Tok::OrEq: first = Tok::Or; second = Tok::Eq; return true;
    case Tok::ShlEq: first = Tok::Lt; second = Tok::Le; return true;
    case Tok::ShrEq: first = Tok::Gt; second = Tok::Ge; return true;
    case Tok::DotDot: first = Tok::Dot; second = Tok::Dot; return true;
    case Tok::DotDotDot: first = Tok::Dot; second = Tok::DotDot; return true;
    case Tok::PathSep: first = Tok::Colon; second = Tok::Colon; return true;
    case Tok::RArrow: first = Tok::Minus; second = Tok::Gt; return true;
    case Tok::LArrow: first = Tok::Lt; second = Tok::Minus; return true;
    case Tok::FatArrow: first = Tok::Eq; second = Tok::Gt; return true;
    default: return false;
    }
}

bool Parser::break_and_eat(ExpTok exp) {
    if (token.kind == exp.tok) {
        bump();
        return true;
    }
    Tok first, second;
    if (break_two_token_op(token.kind, first, second) && first == exp.tok) {
        Span first_span = start_point(token.span);
        Span second_span = token.span.with_lo(first_span.hi);
        Token t1 = token;
        t1.kind = first;
        t1.span = first_span;
        st_.token = t1;
        ++st_.break_last_token;
        Token t2 = t1;
        t2.kind = second;
        t2.span = second_span;
        bump_with(t2, st_.token_spacing);
        return true;
    }
    st_.expected_token_types.insert(exp.tt);
    return false;
}

bool Parser::eat_lt() {
    bool ate = break_and_eat(EXP(Lt));
    if (ate) ++st_.unmatched_angle_bracket_count;
    return ate;
}

void Parser::expect_gt() {
    if (break_and_eat(EXP(Gt))) {
        if (st_.unmatched_angle_bracket_count > 0) --st_.unmatched_angle_bracket_count;
        return;
    }
    unexpected();
}

bool Parser::expect_any_with_type(std::initializer_list<ExpTok> closes_expected, std::initializer_list<Tok> closes_not_expected) {
    for (const ExpTok& c : closes_expected)
        if (check(c)) return true;
    for (Tok k : closes_not_expected)
        if (check_noexpect(k)) return true;
    return false;
}

void Parser::recover_missing_braces_around_closure_body(ClosureSpans closure_spans, PErr expect_err) {
    while (eat(EXP(Semi))) {
        try {
            (void)parse_stmt_without_recovery(false, ForceCollect::No, false);
        } catch (PErr&) {
        }
    }
    // The error becomes "closure bodies that contain statements must be
    // surrounded by braces", spanning the closing pipe and the next token.
    Span preceding_pipe_span = closure_spans.closing_pipe;
    emit(PErr{"closure bodies that contain statements must be surrounded by braces",
              preceding_pipe_span.lo < token.span.lo ? preceding_pipe_span : token.span});
    (void)expect_err;
}

// ---------------------------------------------------------------------------
// Qualifiers.

CoroutineKind Parser::parse_coroutine_kind(Case c) {
    Span span = token.span;
    CoroutineKind k;
    if (eat_keyword_case(KW(Async), c)) {
        if (edition_ >= Edition::E2024 && eat_keyword_case(KW(Gen), c)) {
            k.kind = CoroutineKind::AsyncGen;
            k.span = span.to(prev_token.span);
        } else {
            k.kind = CoroutineKind::Async;
            k.span = span;
        }
    } else if (edition_ >= Edition::E2024 && eat_keyword_case(KW(Gen), c)) {
        k.kind = CoroutineKind::Gen;
        k.span = span;
    }
    return k;
}

Safety Parser::parse_safety(Case c) {
    Safety s;
    if (eat_keyword_case(KW(Unsafe), c)) {
        s.kind = Safety::Unsafe;
        s.span = prev_token.span;
    } else if (eat_keyword_case(KW(Safe), c)) {
        s.kind = Safety::Safe;
        s.span = prev_token.span;
    }
    return s;
}

Constness Parser::parse_constness_(Case c, bool is_closure) {
    Constness k;
    if (check_const_closure() == is_closure && look_ahead_tok(1).kind != Tok::OpenBrace && eat_keyword_case(KW(Const), c)) {
        k.yes = true;
        k.span = prev_token.span;
    }
    return k;
}

Expr* Parser::parse_const_block(Span span, bool pat) {
    expect_keyword(KW(Const));
    std::pair<AttrVec, Block*> r = parse_inner_attrs_and_block(OptSpan());
    Expr* value = mk_expr(r.second->span, ExprKind::Block);
    value->block = r.second;
    Span blk_span = value->span;
    Expr* e;
    if (pat) {
        emit("const blocks cannot be used as patterns", blk_span);
        e = mk_expr(span.to(blk_span), ExprKind::Err);
    } else {
        e = mk_expr(span.to(blk_span), ExprKind::ConstBlock);
        e->anon_const.value = value;
    }
    e->attrs = std::move(r.first);
    return e;
}

Parser::ByRef Parser::parse_byref() {
    ByRef b;
    if (eat_keyword(KW(Ref))) {
        std::pair<Pinnedness, Mutability> pm = parse_pin_and_mut();
        b.yes = true;
        b.pinned = pm.first;
        b.mutbl = pm.second;
    }
    return b;
}

std::optional<Mutability> Parser::parse_mut_or_const() {
    if (eat_keyword(KW(Mut))) return Mutability::Mut;
    if (eat_keyword(KW(Const))) return Mutability::Not;
    return std::nullopt;
}

Ident Parser::parse_field_name() {
    if (token.kind == Tok::Literal && token.lit == LitKind::Integer) {
        if (!token.suffix.empty()) emit("suffixes on a tuple index are invalid", token.span);
        bump();
        Ident id;
        id.name = prev_token.sym;
        id.span = prev_token.span;
        return id;
    }
    return parse_ident_common(true);
}

DelimArgs Parser::parse_delim_args() {
    std::optional<DelimArgs> a = parse_delim_args_inner();
    if (!a) unexpected_any();
    return *a;
}

AttrArgs Parser::parse_attr_args() {
    AttrArgs a;
    if (std::optional<DelimArgs> d = parse_delim_args_inner()) {
        a.kind = AttrArgs::Delimited;
        a.delim = *d;
    } else if (eat(EXP(Eq))) {
        a.kind = AttrArgs::Eq;
        a.eq_span = prev_token.span;
        a.expr = parse_expr_force_collect();
    }
    return a;
}

std::optional<DelimArgs> Parser::parse_delim_args_inner() {
    bool delimited = check(EXP(OpenParen)) || check(EXP(OpenBracket)) || check(EXP(OpenBrace));
    if (!delimited) return std::nullopt;
    DelimArgs d;
    d.delim = token.kind == Tok::OpenParen ? DelimKind::Paren : token.kind == Tok::OpenBracket ? DelimKind::Bracket : DelimKind::Brace;
    d.open = token.span;
    uint32_t open_idx = cur_index();
    uint32_t close_idx = partner_[open_idx];
    d.tok_lo = open_idx + 1;
    d.tok_hi = close_idx;
    (void)parse_token_tree();
    d.close = prev_token.span;
    return d;
}

Span Parser::parse_token_tree() {
    Span lo = token.span;
    if (tok_is_open_delim(token)) {
        uint32_t open_idx = cur_index();
        uint32_t close_idx = partner_[open_idx];
        if (close_idx == UINT32_MAX || close_idx >= tok_hi_) close_idx = tok_hi_;
        // Jump to the close delimiter, then past it.
        st_.pos = close_idx;
        bump();  // token = close delimiter
        bump();  // past it
        return lo.to(prev_token.span);
    }
    bump();
    return lo;
}

Visibility Parser::parse_visibility(FollowedByType fbt) {
    Visibility v;
    if (!eat_keyword(KW(Pub))) {
        v.kind = Visibility::Inherited;
        v.span = token.span.shrink_to_lo();
        return v;
    }
    Span lo = prev_token.span;
    if (check(EXP(OpenParen))) {
        if (is_keyword_ahead(1, {Kw::In})) {
            bump();  // (
            bump();  // in
            Path* path = parse_path(PathStyle::Mod);
            expect(EXP(CloseParen));
            v.kind = Visibility::Restricted;
            v.path = path;
            v.shorthand = false;
            v.span = lo.to(prev_token.span);
            return v;
        } else if (look_ahead_tok(2).kind == Tok::CloseParen && is_keyword_ahead(1, {Kw::Crate, Kw::Super, Kw::SelfLower})) {
            bump();  // (
            Path* path = parse_path(PathStyle::Mod);
            expect(EXP(CloseParen));
            v.kind = Visibility::Restricted;
            v.path = path;
            v.shorthand = true;
            v.span = lo.to(prev_token.span);
            return v;
        } else if (fbt == FollowedByType::No) {
            recover_incorrect_vis_restriction();
        }
    }
    v.kind = Visibility::Public;
    v.span = lo;
    return v;
}

void Parser::recover_incorrect_vis_restriction() {
    bump();  // (
    Path* path = parse_path(PathStyle::Mod);
    expect(EXP(CloseParen));
    emit("incorrect visibility restriction", path->span);
}

ImplRestriction Parser::parse_impl_restriction() {
    ImplRestriction r;
    if (eat_keyword(KW(Impl))) {
        Span lo = prev_token.span;
        expect(EXP(OpenParen));
        if (eat_keyword(KW(In))) {
            Path* path = parse_path(PathStyle::Mod);
            expect(EXP(CloseParen));
            r.restricted = true;
            r.path = path;
            r.span = lo.to(prev_token.span);
            return r;
        } else if (look_ahead_tok(1).kind == Tok::CloseParen && is_keyword_ahead(0, {Kw::Crate, Kw::Super, Kw::SelfLower})) {
            Path* path = parse_path(PathStyle::Mod);
            expect(EXP(CloseParen));
            r.restricted = true;
            r.shorthand = true;
            r.path = path;
            r.span = lo.to(prev_token.span);
            return r;
        } else {
            Path* path = parse_path(PathStyle::Mod);
            expect(EXP(CloseParen));
            emit("incorrect `impl` restriction", path->span);
        }
    }
    r.span = token.span.shrink_to_lo();
    return r;
}

Extern Parser::parse_extern(Case c) {
    Extern e;
    if (eat_keyword_case(KW(Extern), c)) {
        Span extern_span = prev_token.span;
        std::optional<StrLit> abi = parse_abi();
        if (abi) {
            extern_span = extern_span.to(abi->span);
            e.kind = Extern::Explicit;
            e.abi = *abi;
        } else {
            e.kind = Extern::Implicit;
        }
        e.span = extern_span;
    }
    return e;
}

std::optional<StrLit> Parser::parse_abi() {
    StrLit s;
    Lit non_str;
    bool had_lit = false;
    if (parse_str_lit(s, &non_str, &had_lit)) return s;
    if (had_lit && non_str.kind != LitKind::Err) emit("non-string ABI literal", non_str.span);
    return std::nullopt;
}

bool Parser::check_path_sep_and_look_ahead(bool (*looker)(const Token&)) {
    if (check(EXP(PathSep))) {
        if (may_recover() && look_ahead_tok(1).kind == Tok::Colon) return looker(look_ahead_tok(2));
        return looker(look_ahead_tok(1));
    }
    return false;
}

bool Parser::is_import_coupler() {
    return check_path_sep_and_look_ahead([](const Token& t) { return t.kind == Tok::OpenBrace || t.kind == Tok::Star; });
}

// classify::expr_requires_semi_to_be_stmt
static bool expr_requires_semi_to_be_stmt(const Expr* e) {
    switch (e->kind) {
    case ExprKind::If: case ExprKind::Match: case ExprKind::Block: case ExprKind::While: case ExprKind::Loop:
    case ExprKind::ForLoop: case ExprKind::TryBlock: case ExprKind::ConstBlock:
        return false;
    case ExprKind::MacCall:
        return e->mac == nullptr || e->mac->args.delim != DelimKind::Brace;
    default:
        return true;
    }
}

std::optional<Span> Parser::missing_semi_from_binop(const char* kind_desc, const Expr* expr, OptSpan decl_lo) {
    (void)decl_lo;
    if (token.kind == Tok::Semi) return std::nullopt;
    if (!may_recover()) return std::nullopt;
    if (expr->kind == ExprKind::Binary && expr->lhs && expr->rhs &&
        is_multiline(expr->lhs->span.shrink_to_hi().until(expr->rhs->span.shrink_to_lo())) &&
        (expr->binop == BinOpKind::Mul || expr->binop == BinOpKind::BitAnd) && expr_requires_semi_to_be_stmt(expr->rhs)) {
        Span lhs_end_span = expr->lhs->span.shrink_to_hi();
        emit("expected `;`, found " + token_descr(token), lhs_end_span);
        (void)kind_desc;
        return expr->lhs->span;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Token predicates.

bool Parser::ident_can_begin_expr(const Token& t) const {
    if (t.raw || !kw_is_reserved(t.kw, edition_) || kw_is_path_segment_keyword(t.kw)) return true;
    switch (t.kw) {
    case Kw::Async: case Kw::Do: case Kw::Box: case Kw::Break: case Kw::Const: case Kw::Continue: case Kw::False:
    case Kw::For: case Kw::Gen: case Kw::If: case Kw::Let: case Kw::Loop: case Kw::Match: case Kw::Move:
    case Kw::Return: case Kw::True: case Kw::Try: case Kw::Unsafe: case Kw::While: case Kw::Yield: case Kw::Safe:
    case Kw::Static:
        return true;
    default:
        return false;
    }
}

bool Parser::ident_can_begin_type(const Token& t) const {
    if (t.raw || !kw_is_reserved(t.kw, edition_) || kw_is_path_segment_keyword(t.kw)) return true;
    switch (t.kw) {
    case Kw::Underscore: case Kw::For: case Kw::Impl: case Kw::Fn: case Kw::Unsafe: case Kw::Extern: case Kw::Typeof:
    case Kw::Dyn:
        return true;
    default:
        return false;
    }
}

bool Parser::tok_can_begin_expr(const Token& t) const {
    switch (t.kind) {
    case Tok::Ident: return ident_can_begin_expr(t);
    case Tok::OpenParen: case Tok::OpenBrace: case Tok::OpenBracket: case Tok::Literal: case Tok::Bang:
    case Tok::Minus: case Tok::Star: case Tok::Or: case Tok::OrOr: case Tok::And: case Tok::AndAnd:
    case Tok::DotDot: case Tok::DotDotDot: case Tok::DotDotEq: case Tok::Lt: case Tok::Shl: case Tok::PathSep:
    case Tok::Lifetime: case Tok::Pound:
        return true;
    default:
        return false;
    }
}

bool Parser::tok_can_begin_pattern(const Token& t, bool pat_with_or) const {
    switch (t.kind) {
    case Tok::Ident: case Tok::OpenParen: case Tok::OpenBracket: case Tok::And: case Tok::Minus: case Tok::AndAnd:
    case Tok::Literal: case Tok::DotDot: case Tok::DotDotDot: case Tok::PathSep: case Tok::Lt: case Tok::Shl:
        return true;
    case Tok::Or: return pat_with_or;
    default: return false;
    }
}

bool Parser::tok_can_begin_type(const Token& t) const {
    switch (t.kind) {
    case Tok::Ident: return ident_can_begin_type(t);
    case Tok::OpenParen: case Tok::OpenBracket: case Tok::Bang: case Tok::Star: case Tok::And: case Tok::AndAnd:
    case Tok::Question: case Tok::Lifetime: case Tok::Lt: case Tok::Shl: case Tok::PathSep:
        return true;
    default:
        return false;
    }
}

bool Parser::tok_can_begin_const_arg(const Token& t) const {
    switch (t.kind) {
    case Tok::OpenBrace: case Tok::Literal: case Tok::Minus: return true;
    case Tok::Ident: return !t.raw && (t.kw == Kw::True || t.kw == Kw::False);
    default: return false;
    }
}

bool Parser::tok_can_begin_item(const Token& t) const {
    if (t.kind != Tok::Ident) return false;
    switch (t.kw) {
    case Kw::Fn: case Kw::Use: case Kw::Struct: case Kw::Enum: case Kw::Pub: case Kw::Trait: case Kw::Extern:
    case Kw::Impl: case Kw::Unsafe: case Kw::Const: case Kw::Safe: case Kw::Static: case Kw::Union: case Kw::Macro:
    case Kw::Mod: case Kw::Type:
        return true;
    default:
        return false;
    }
}

bool Parser::tok_can_begin_literal_maybe_minus(const Token& t) const {
    switch (t.kind) {
    case Tok::Literal: case Tok::Minus: return true;
    case Tok::Ident: return !t.raw && (t.kw == Kw::True || t.kw == Kw::False);
    default: return false;
    }
}

bool Parser::ident_is_raw_guess(const Ident& id) const {
    Kw k = kw_lookup(id.name);
    bool can_be_raw = !id.name.empty() && k != Kw::Underscore && !kw_is_path_segment_keyword(k);
    return can_be_raw && kw_is_reserved(k, edition_);
}

bool Parser::is_multiline(Span s) const {
    return file_.lines->at(s.lo).line != file_.lines->at(s.hi).line;
}

std::string Parser::token_descr(const Token& t) const {
    std::string s = token_str(t);
    if (tok_is_special_ident(t)) return "reserved identifier `" + s + "`";
    if (tok_is_used_keyword(t)) return "keyword `" + s + "`";
    if (tok_is_unused_keyword(t)) return "reserved keyword `" + s + "`";
    if (t.kind == Tok::DocComment) return "doc comment `" + s + "`";
    return "`" + s + "`";
}

Span Parser::start_point(Span s) const {
    if (s.lo >= s.hi) return s;
    size_t len = 1;
    decode_utf8(src_, s.lo, len);
    uint32_t hi = s.lo + static_cast<uint32_t>(len);
    if (hi > s.hi) hi = s.hi;
    return Span(s.lo, hi);
}

Expr* Parser::mk_closure_expr(Span sp, Expr* body) {
    Expr* e = mk_expr(sp, ExprKind::Closure);
    Closure* c = mk<Closure>();
    c->fn_decl = mk<FnDecl>();
    c->fn_decl->output.span = sp;
    c->fn_arg_span = sp;
    c->fn_decl_span = sp;
    c->body = body;
    e->closure = c;
    return e;
}

// ---------------------------------------------------------------------------
// diagnostics.rs (generic part).

PErr Parser::expected_ident_found_err() {
    try {
        expected_ident_found(false);
    } catch (PErr& e) {
        return e;
    }
    return PErr{"expected identifier", token.span};
}

std::optional<std::pair<size_t, std::string_view>> Parser::is_lit_bad_ident() {
    if (token.kind == Tok::Literal && (token.lit == LitKind::Integer || token.lit == LitKind::Float) &&
        !token.suffix.empty()) {
        // MetaItemLit::from_token(..).is_none(): the suffix is not a valid one.
        std::string_view suf = token.suffix;
        static const char* const ints[] = {"i8", "i16", "i32", "i64", "i128", "isize",
                                           "u8", "u16", "u32", "u64", "u128", "usize"};
        static const char* const floats[] = {"f16", "f32", "f64", "f128"};
        bool valid = false;
        for (const char* s : ints)
            if (suf == s) valid = true;
        for (const char* s : floats)
            if (suf == s) valid = true;
        // Floats with an integer suffix are invalid; ints with a float suffix are fine
        // unless the literal is not decimal.
        if (valid && token.lit == LitKind::Float) {
            valid = false;
            for (const char* s : floats)
                if (suf == s) valid = true;
        }
        if (valid && token.lit == LitKind::Integer) {
            bool is_float_suffix = suf == "f16" || suf == "f32" || suf == "f64" || suf == "f128";
            std::string_view s = token.sym;
            bool non_decimal = s.size() > 1 && s[0] == '0' && (s[1] == 'x' || s[1] == 'o' || s[1] == 'b');
            if (is_float_suffix && non_decimal) valid = false;
        }
        if (!valid) return std::make_pair(token.sym.size(), token.suffix);
    }
    return std::nullopt;
}

std::pair<Ident, bool> Parser::expected_ident_found(bool recover) {
    auto valid_follow = [](Tok k) {
        switch (k) {
        case Tok::Eq: case Tok::Colon: case Tok::Comma: case Tok::Semi: case Tok::PathSep: case Tok::OpenBrace:
        case Tok::OpenParen: case Tok::CloseBrace: case Tok::CloseParen:
            return true;
        default:
            return false;
        }
    };
    if (prev_token.kind == Tok::DocComment && valid_follow(token.kind))
        throw PErr{"found a documentation comment that doesn't document anything", prev_token.span};

    bool have_recovered = false;
    std::pair<Ident, bool> recovered_ident;
    Token bad_token = token;
    if (token.kind == Tok::Ident && !token.raw && ident_is_raw_guess(tok_ident(token)) &&
        valid_follow(look_ahead_tok(1).kind)) {
        have_recovered = true;
        recovered_ident = {tok_ident(token), true};
    }
    if (token.kind == Tok::Comma && look_ahead_tok(1).kind == Tok::Ident) {
        if (recover) {
            bump();
            try {
                recovered_ident = ident_or_err(false);
                have_recovered = true;
            } catch (PErr&) {
                have_recovered = false;
            }
        }
    }
    if (auto bad = is_lit_bad_ident()) {
        Ident id;
        id.name = bad->second;
        id.span = Span(token.span.lo + static_cast<uint32_t>(bad->first), token.span.hi);
        recovered_ident = {id, false};
        have_recovered = true;
    }
    PErr err{"expected identifier, found " + token_descr(bad_token), bad_token.span};
    if (token.kind == Tok::Lt) {
        Token maybe_keyword = prev_token;
        if (tok_is_keyword(maybe_keyword, Kw::Fn) || tok_is_keyword(maybe_keyword, Kw::Type) ||
            tok_is_keyword(maybe_keyword, Kw::Struct) || tok_is_keyword(maybe_keyword, Kw::Enum) ||
            tok_is_keyword(maybe_keyword, Kw::Union) || tok_is_keyword(maybe_keyword, Kw::Trait)) {
            try {
                (void)parse_generics();
            } catch (PErr&) {
            }
        }
    }
    if (have_recovered && recover) {
        emit(err);
        return recovered_ident;
    }
    throw err;
}

void Parser::expected_one_of_not_found(std::initializer_list<ExpTok> edible, std::initializer_list<ExpTok> inedible) {
    for (const ExpTok& e : edible) st_.expected_token_types.insert(e.tt);
    for (const ExpTok& e : inedible) st_.expected_token_types.insert(e.tt);
    std::vector<TokenType> expected = st_.expected_token_types.items();
    std::vector<std::pair<std::string, TokenType>> keyed;
    keyed.reserve(expected.size());
    for (TokenType t : expected) keyed.emplace_back(token_type_to_string(t), t);
    std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    // dedup on equal neighbours
    std::vector<std::pair<std::string, TokenType>> uniq;
    for (auto& k : keyed)
        if (uniq.empty() || uniq.back().second != k.second) uniq.push_back(k);
    auto contains = [&](TokenType t) {
        for (auto& k : uniq)
            if (k.second == t) return true;
        return false;
    };

    if (contains(TokenType::Semi)) {
        if (prev_token.kind == Tok::Question) maybe_recover_from_ternary_operator(OptSpan());
        bool dummy = token.span == Span(0, 0) || prev_token.span == Span(0, 0);
        if (dummy) {
        } else if (!is_multiline(prev_token.span.until(token.span))) {
        } else if ((token.kind == Tok::Comma || token.kind == Tok::Colon) && prev_token.kind == Tok::CloseParen) {
        } else if ((look_ahead_tok(1).kind == Tok::CloseBrace ||
                    (tok_can_begin_expr(look_ahead_tok(1)) && look_ahead_tok(1).kind != Tok::Colon)) &&
                   (token.kind == Tok::Comma || token.kind == Tok::Colon)) {
            emit("expected `;`, found " + token_descr(token), token.span);
            bump();
            return;
        } else {
            const Token& t = token;
            bool cond = t.kind == Tok::CloseBrace ||
                        ((tok_can_begin_expr(t) || tok_can_begin_item(t)) && t.kind != Tok::Semi && t.kind != Tok::Pound) ||
                        (is_multiline(prev_token.span.shrink_to_hi().until(token.span.shrink_to_lo())) && t.kind == Tok::Pound);
            if (cond && !contains(TokenType::Comma)) {
                emit("expected `;`, found " + token_descr(token), prev_token.span.shrink_to_hi());
                return;
            }
        }
    }
    if (token.kind == Tok::EqEq && prev_token.kind == Tok::Ident && contains(TokenType::Eq))
        throw PErr{"unexpected `==`", token.span};
    if ((tok_is_keyword(token, Kw::Move) || tok_is_keyword(token, Kw::Use)) && tok_is_keyword(prev_token, Kw::Async)) {
        Span span = prev_token.span.to(token.span);
        if (tok_is_keyword(token, Kw::Move)) throw PErr{"`async move` blocks are only allowed in Rust 2018 or later", span};
        throw PErr{"`async use` blocks are only allowed in Rust 2018 or later", span};
    }
    std::string expect;
    for (size_t i = 0; i < uniq.size(); ++i) {
        if (i > 0) {
            if (uniq.size() > 2 && i == uniq.size() - 1)
                expect += ", or ";
            else if (uniq.size() == 2 && i == 1)
                expect += " or ";
            else
                expect += ", ";
        }
        expect += uniq[i].first;
    }
    std::string actual = token_descr(token);
    std::string msg;
    if (uniq.size() > 1)
        msg = "expected one of " + expect + ", found " + actual;
    else if (uniq.empty())
        msg = "unexpected token: " + actual;
    else
        msg = "expected " + expect + ", found " + actual;
    st_.last_unexpected_token_span = OptSpan(token.span);
    PErr err{msg, token.span};
    if (check_too_many_raw_str_terminators(err)) {
        if (contains(TokenType::Semi) && eat(EXP(Semi))) {
            emit(err);
            return;
        }
        throw err;
    }
    throw err;
}

bool Parser::check_too_many_raw_str_terminators(PErr& err) {
    if (prev_token.kind == Tok::Literal && (prev_token.lit == LitKind::StrRaw || prev_token.lit == LitKind::ByteStrRaw) &&
        token.kind == Tok::Pound && !is_multiline(prev_token.span.shrink_to_hi().until(token.span.shrink_to_lo()))) {
        err.msg = "too many `#` when terminating raw string";
        Span span = token.span;
        while (token.kind == Tok::Pound && !is_multiline(span.shrink_to_hi().until(token.span.shrink_to_lo()))) {
            span = span.with_hi(token.span.hi);
            bump();
        }
        err.span = span;
        return true;
    }
    return false;
}

PErr Parser::unexpected_err(Tok t) {
    std::string token_s = tok_str(t);
    std::string this_token_str = token_descr(token);
    Span sp;
    if (token.kind == Tok::Eof && subparser_name_) {
        sp = prev_token.span.shrink_to_hi();
    } else {
        sp = token.span;
    }
    std::string found = (token.kind == Tok::Eof && subparser_name_) ? std::string("end of ") + subparser_name_ : this_token_str;
    return PErr{"expected `" + token_s + "`, found " + found, sp};
}

Crate* Parser::parse_crate_root() { return parse_crate_mod(); }

}  // namespace detail

// ---------------------------------------------------------------------------
// Public entry points.

using detail::FatalAbort;
using detail::Parser;
using detail::PErr;

/// Byte offset of the first byte that is not part of valid UTF-8, or npos.
static size_t first_invalid_utf8(const std::string& s) {
    size_t i = 0, n = s.size();
    auto cont = [&](size_t k) { return k < n && (static_cast<unsigned char>(s[k]) & 0xC0) == 0x80; };
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        size_t len = 0;
        uint32_t cp = 0;
        if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
            cp = c & 0x1Fu;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
            cp = c & 0x0Fu;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
            cp = c & 0x07u;
        } else {
            return i;
        }
        for (size_t k = 1; k < len; ++k) {
            if (!cont(i + k)) return i;
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
        }
        if ((len == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)))
            return i;
        i += len;
    }
    return std::string::npos;
}

static void init_result(ParseResult& r, std::string text, const ParseOptions& opts) {
    r.source = std::make_unique<const std::string>(std::move(text));
    r.edition = opts.edition;
    r.options = opts;
    r.arena = std::make_unique<Arena>();
    r.lines = std::make_unique<LineIndex>(*r.source);
}

static Diagnostic to_diag(const ParseResult& r, const std::string& msg, Span sp, bool after_fatal) {
    Diagnostic d;
    d.message = msg;
    d.span = sp;
    LineCol lc = r.lines->at(sp.lo);
    d.line = lc.line;
    d.col = lc.col;
    d.after_fatal = after_fatal;
    return d;
}

std::unique_ptr<ParseResult> parse_file(std::string text, const ParseOptions& opts) {
    auto r = std::make_unique<ParseResult>();
    init_result(*r, std::move(text), opts);
    LexOptions lo;
    lo.edition = opts.edition;
    lo.strip_shebang_frontmatter = opts.strip_shebang_frontmatter;
    LexResult lx = lex(*r->source, lo);
    r->tokens = std::move(lx.tokens);
    r->partner = std::move(lx.partner);
    r->comments = std::move(lx.comments);
    r->shebang = lx.shebang;
    r->frontmatter = lx.frontmatter;
    r->interned = std::move(lx.interned);
    // Lexer errors: everything before (and including) the first fatal one is
    // what rustc prints; after a fatal error rustc stops, mep goes on.
    bool fatal = lx.fatal;
    // rustc will not even read a file that is not UTF-8.
    bool bad_utf8 = false;
    if (size_t bad = first_invalid_utf8(*r->source); bad != std::string::npos) {
        r->diagnostics.push_back(to_diag(*r, "stream did not contain valid UTF-8",
                                         Span(static_cast<uint32_t>(bad), static_cast<uint32_t>(bad)), false));
        bad_utf8 = true;
        fatal = true;
    }
    for (size_t i = 0; i < lx.errors.size(); ++i) {
        bool after = bad_utf8 || (fatal && lx.fatal_index >= 0 && i > static_cast<size_t>(lx.fatal_index));
        r->diagnostics.push_back(to_diag(*r, lx.errors[i].message, lx.errors[i].span, after));
    }
    Parser p(*r, 0, static_cast<uint32_t>(r->tokens.size() - 1), nullptr);
    for (const LexDiag& d : lx.late_errors) p.late_.push_back(PErr{d.message, d.span});
    Crate* crate = nullptr;
    try {
        crate = p.parse_crate_root();
    } catch (PErr& e) {
        p.emit(e);
    } catch (FatalAbort&) {
    }
    p.finish();
    if (!crate) {
        crate = r->arena->make<Crate>();
        crate->inner_span = Span(0, static_cast<uint32_t>(r->source->size()));
    }
    r->crate = crate;
    for (const PErr& e : p.diags) r->diagnostics.push_back(to_diag(*r, e.msg, e.span, fatal));
    return r;
}

Fragment parse_fragment(ParseResult& file, uint32_t tok_lo, uint32_t tok_hi, FragmentKind kind, bool prefix) {
    Fragment f;
    f.kind = kind;
    if (tok_hi > file.tokens.size() - 1) tok_hi = static_cast<uint32_t>(file.tokens.size() - 1);
    Parser p(file, tok_lo, tok_hi, "macro arguments");
    try {
        switch (kind) {
        case FragmentKind::Expr: f.expr = p.parse_expr(); break;
        case FragmentKind::Ty: f.ty = p.parse_ty(); break;
        case FragmentKind::Pat:
            f.pat = p.parse_pat_allow_top_guard(detail::Expected::None, detail::RecoverComma::No,
                                                detail::RecoverColon::No, detail::CommaRecoveryMode::EitherTupleOrPipe);
            break;
        case FragmentKind::Path: f.path = p.parse_path(detail::PathStyle::Type); break;
        case FragmentKind::Block: f.block = p.parse_block(); break;
        case FragmentKind::Items:
            while (p.token.kind != Tok::Eof) {
                Item* it = p.parse_item(detail::ForceCollect::No, detail::AllowConstBlockItems::Yes);
                if (!it) {
                    p.emit("expected item, found " + p.token_descr(p.token), p.token.span);
                    break;
                }
                f.items.push_back(it);
            }
            break;
        case FragmentKind::Stmts:
            while (p.token.kind != Tok::Eof) {
                Stmt* s = p.parse_full_stmt(detail::AttemptLocalParseRecovery::No);
                if (!s) {
                    p.emit("expected statement, found " + p.token_descr(p.token), p.token.span);
                    break;
                }
                f.stmts.push_back(s);
            }
            break;
        case FragmentKind::ExprList:
            while (p.token.kind != Tok::Eof) {
                f.exprs.push_back(p.parse_expr());
                if (!p.eat(EXP(Comma))) break;
            }
            break;
        }
    } catch (PErr& e) {
        p.emit(e);
    } catch (FatalAbort&) {
    }
    f.end_tok = p.token.kind == Tok::Eof ? tok_hi : p.cur_index();
    for (const PErr& e : p.diags) f.diagnostics.push_back(to_diag(file, e.msg, e.span, false));
    bool consumed = p.token.kind == Tok::Eof;
    if (!prefix && !consumed && f.diagnostics.empty())
        f.diagnostics.push_back(to_diag(file, "unexpected token: " + p.token_descr(p.token), p.token.span, false));
    f.ok = f.diagnostics.empty() && (prefix || consumed);
    return f;
}

std::unique_ptr<ParseResult> parse_string_fragment(std::string text, FragmentKind kind, Fragment& out,
                                                   const ParseOptions& opts) {
    auto r = std::make_unique<ParseResult>();
    init_result(*r, std::move(text), opts);
    LexOptions lo;
    lo.edition = opts.edition;
    lo.strip_shebang_frontmatter = false;
    LexResult lx = lex(*r->source, lo);
    r->tokens = std::move(lx.tokens);
    r->partner = std::move(lx.partner);
    r->comments = std::move(lx.comments);
    r->interned = std::move(lx.interned);
    for (const LexDiag& d : lx.errors) r->diagnostics.push_back(to_diag(*r, d.message, d.span, false));
    r->crate = r->arena->make<Crate>();
    out = parse_fragment(*r, 0, static_cast<uint32_t>(r->tokens.size() - 1), kind);
    if (!r->diagnostics.empty()) {
        out.diagnostics.insert(out.diagnostics.begin(), r->diagnostics.begin(), r->diagnostics.end());
        out.ok = false;
    }
    return r;
}

}  // namespace rust
