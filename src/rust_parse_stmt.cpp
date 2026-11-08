// rust_parse_stmt.cpp -- statements, `let`, blocks and block recovery.
//
// Ported from rustc_parse/rustc_lexer (rust-lang/rust 1.97.1, MIT OR
// Apache-2.0): rustc_parse/src/parser/stmt.rs, plus the parts of
// diagnostics.rs it uses (attr_on_non_tail_expr, maybe_suggest_struct_literal,
// suggest_add_missing_let_for_stmt, maybe_recover_from_bad_qpath for macro
// statements) and rustc_ast::util::classify. Diagnostics keep their primary
// message and span; suggestions, notes and labels are dropped.
#include "rust_parse_internal.h"

namespace rust {
namespace detail {

namespace {

// rustc_ast::util::classify::expr_is_complete
bool expr_is_complete(const Expr* e) {
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

// classify::expr_requires_semi_to_be_stmt
bool expr_requires_semi_to_be_stmt(const Expr* e) {
    if (e->kind == ExprKind::MacCall) return e->mac == nullptr || e->mac->args.delim != DelimKind::Brace;
    return !expr_is_complete(e);
}

// classify::path_return_type
const Ty* path_return_type(const Path* path) {
    if (path == nullptr || path->segments.empty()) return nullptr;
    const GenericArgs* args = path->segments.back().args;
    if (args == nullptr) return nullptr;
    if (args->kind != GenericArgs::Parenthesized) return nullptr;
    return args->output.ty;
}

// classify::type_trailing_braced_mac_call: the span of the braced macro call, if any.
bool type_trailing_braced_mac_call(const Ty* ty, Span& out) {
    for (;;) {
        if (ty == nullptr) return false;
        switch (ty->kind) {
        case TyKind::MacCall:
            if (ty->mac != nullptr && ty->mac->args.delim == DelimKind::Brace) {
                out = ty->span;
                return true;
            }
            return false;
        case TyKind::Ptr:
        case TyKind::Ref:
        case TyKind::PinnedRef:
        case TyKind::UnsafeBinder: ty = ty->inner; break;
        case TyKind::FnPtr:
            if (ty->fn_ptr == nullptr || ty->fn_ptr->decl == nullptr || ty->fn_ptr->decl->output.ty == nullptr)
                return false;
            ty = ty->fn_ptr->decl->output.ty;
            break;
        case TyKind::Path: {
            const Ty* t = path_return_type(ty->path);
            if (t == nullptr) return false;
            ty = t;
            break;
        }
        case TyKind::TraitObject:
        case TyKind::ImplTrait: {
            if (ty->bounds.empty() || ty->bounds.back().kind != GenericBound::Trait) return false;
            const Ty* t = path_return_type(ty->bounds.back().trait.trait_ref);
            if (t == nullptr) return false;
            ty = t;
            break;
        }
        default: return false;
        }
    }
}

// classify::expr_trailing_brace: the span of the node ending in `}`, if any.
bool expr_trailing_brace(const Expr* expr, Span& out) {
    for (;;) {
        if (expr == nullptr) return false;
        switch (expr->kind) {
        case ExprKind::AddrOf:
        case ExprKind::Unary:
        case ExprKind::Move:
        case ExprKind::Become:
        case ExprKind::Let: expr = expr->lhs; break;
        case ExprKind::Assign:
        case ExprKind::AssignOp:
        case ExprKind::Binary: expr = expr->rhs; break;
        case ExprKind::Break:
        case ExprKind::Ret:
        case ExprKind::Yeet:
        case ExprKind::Yield:
            if (expr->lhs == nullptr) return false;
            expr = expr->lhs;
            break;
        case ExprKind::Range:
            if (expr->rhs == nullptr) return false;
            expr = expr->rhs;
            break;
        case ExprKind::Closure:
            if (expr->closure == nullptr) return false;
            expr = expr->closure->body;
            break;
        case ExprKind::Gen:
        case ExprKind::Block:
        case ExprKind::ForLoop:
        case ExprKind::If:
        case ExprKind::Loop:
        case ExprKind::Match:
        case ExprKind::Struct:
        case ExprKind::TryBlock:
        case ExprKind::While:
        case ExprKind::ConstBlock: out = expr->span; return true;
        case ExprKind::Cast: return type_trailing_braced_mac_call(expr->ty, out);
        case ExprKind::MacCall:
            if (expr->mac != nullptr && expr->mac->args.delim == DelimKind::Brace) {
                out = expr->span;
                return true;
            }
            return false;
        default: return false;
        }
    }
}

}  // namespace

// stmt.rs parse_stmt: parses a statement, recovering from errors.
Stmt* Parser::parse_stmt(ForceCollect force_collect) {
    try {
        return parse_stmt_without_recovery(false, force_collect, false);
    } catch (PErr& e) {
        emit(e);
        recover_stmt_(SemiColonMode::Break, BlockMode::Ignore);
        return nullptr;
    }
}

/// If `force_collect` is `ForceCollect::Yes`, forces collection of tokens regardless of whether
/// or not we have attributes.
Stmt* Parser::parse_stmt_without_recovery(bool capture_semi, ForceCollect force_collect, bool force_full_expr) {
    (void)capture_semi;
    AttrWrapper attrs = parse_outer_attributes();
    Span lo = token.span;

    if (tok_is_keyword(token, Kw::Mut) && is_keyword_ahead(1, {Kw::Let})) {
        bump();
        Span mut_let_span = lo.to(token.span);
        emit("invalid variable declaration", mut_let_span);
    }

    Stmt* stmt = nullptr;
    if (tok_is_keyword(token, Kw::Super) && is_keyword_ahead(1, {Kw::Let})) {
        Span super_span = token.span;
        expect_keyword(KW(Super));
        expect_keyword(KW(Let));
        Local* local = parse_local(OptSpan(super_span), attrs.attrs);
        stmt = mk_stmt(lo.to(prev_token.span), StmtKind::Let);
        stmt->local = local;
    } else if (tok_is_keyword(token, Kw::Let)) {
        expect_keyword(KW(Let));
        Local* local = parse_local(OptSpan(), attrs.attrs);
        stmt = mk_stmt(lo.to(prev_token.span), StmtKind::Let);
        stmt->local = local;
    } else if (is_kw_followed_by_ident(Kw::Mut) && may_recover()) {
        stmt = recover_stmt_local_after_let(lo, attrs);
    } else if (is_kw_followed_by_ident(Kw::Auto) && may_recover()) {
        bump();  // `auto`
        stmt = recover_stmt_local_after_let(lo, attrs);
    } else if (tok_is_ident_named(token, "var") && !token.raw &&
               look_ahead(1, [this](const Token& t) { return tok_is_non_reserved_ident(t); }) && may_recover()) {
        bump();  // `var`
        stmt = recover_stmt_local_after_let(lo, attrs);
    } else if (check_path() && !tok_is_qpath_start(token) && !is_path_start_item() && !is_builtin()) {
        // We have avoided contextual keywords like `union`, items with `crate` visibility,
        // or `auto trait` items. We aim to parse an arbitrary path `a::b` but not something
        // that starts like a path (1 token), but it fact not a path.
        // Also, we avoid stealing syntax from `parse_item_`.
        //
        // `UsePreAttrPos::Yes` here means the attribute belongs unconditionally to the
        // expression, not the statement.
        // suggest_add_missing_let_for_stmt only adds a suggestion (and restores its snapshot).
        stmt = parse_stmt_path_start(lo, attrs);
    } else {
        Item* item = parse_item_common(attrs, false, true, FnParseMode{ReqName::Always, FnContext::Free, true},
                                       force_collect, AllowConstBlockItems::No);
        if (item != nullptr) {
            // FIXME: Bad copy of attrs
            stmt = mk_stmt(lo.to(item->span), StmtKind::Item);
            stmt->item = item;
        } else if (eat(EXP(Semi))) {
            // Do not attempt to parse an expression if we're done here.
            error_outer_attrs(attrs);
            stmt = mk_stmt(lo, StmtKind::Empty);
        } else if (token.kind != Tok::CloseBrace) {
            // Remainder are line-expr stmts. This is similar to the `parse_stmt_path_start` case
            // above.
            Restrictions restrictions = force_full_expr ? R_EMPTY : R_STMT_EXPR;
            Expr* e = parse_expr_res(restrictions, attrs).first;
            if (e->kind == ExprKind::Assign && eat_keyword(KW(Else))) {
                Block* bl = parse_block();
                // Not `emit_err`: `<assignment> ... else { ... }` is not allowed.
                emit("<assignment> ... else { ... } is not allowed", e->span.to(bl->span));
            }
            stmt = mk_stmt(lo.to(e->span), StmtKind::Expr);
            stmt->expr = e;
        } else {
            error_outer_attrs(attrs);
            return nullptr;
        }
    }
    // Steals (and so emits now) the expression-in-pattern errors stashed
    // while parsing this statement's patterns.
    maybe_augment_stashed_expr_in_pats_with_suggestions(stmt);
    return stmt;
}

Stmt* Parser::parse_stmt_path_start(Span lo, AttrWrapper attrs) {
    Path* path = parse_path(PathStyle::Expr);

    if (eat(EXP(Bang))) return parse_stmt_mac(lo, attrs.attrs, path);

    Expr* expr;
    if (eat(EXP(OpenBrace))) {
        expr = parse_expr_struct(nullptr, path, true);
    } else {
        Span hi = prev_token.span;
        expr = mk_expr(lo.to(hi), ExprKind::Path);
        expr->path = path;
    }

    expr = with_res(R_STMT_EXPR, [&] { return parse_expr_dot_or_call_with(attrs.attrs, expr, lo); });

    // `parse_expr_assoc_rest_with` doesn't take `attrs` (they're attached to the lhs).
    Expr* e = with_res(R_STMT_EXPR, [&] { return parse_expr_assoc_rest_with(PrecBound::unbounded(), true, expr); }).first;
    Stmt* s = mk_stmt(lo.to(prev_token.span), StmtKind::Expr);
    s->expr = e;
    return s;
}

/// Parses a statement macro `mac!(args)` provided a `path` representing `mac`.
/// At this point, the `!` token after the path has already been eaten.
Stmt* Parser::parse_stmt_mac(Span lo, AttrVec attrs, Path* path) {
    DelimArgs args = parse_delim_args();
    Span hi = prev_token.span;

    MacStmtStyle style = args.delim == DelimKind::Brace ? MacStmtStyle::Braces : MacStmtStyle::NoBraces;

    MacCall* mac = mk<MacCall>();
    mac->path = path;
    mac->args = args;

    Stmt* s;
    if ((style == MacStmtStyle::Braces && token.kind != Tok::Dot && token.kind != Tok::Question) ||
        token.kind == Tok::Semi || token.kind == Tok::Eof) {
        s = mk_stmt(lo.to(hi), StmtKind::MacCall);
        MacCallStmt* m = mk<MacCallStmt>();
        m->mac = mac;
        m->style = style;
        m->attrs = std::move(attrs);
        s->mac = m;
    } else {
        // Since none of the above applied, this is an expression statement macro.
        Expr* e = mk_expr(lo.to(hi), ExprKind::MacCall);
        e->mac = mac;
        e = maybe_recover_from_bad_qpath_mac_expr(e);
        e = parse_expr_dot_or_call_with(std::move(attrs), e, lo);
        e = parse_expr_assoc_rest_with(PrecBound::unbounded(), false, e).first;
        s = mk_stmt(lo.to(hi), StmtKind::Expr);
        s->expr = e;
    }
    return s;
}

// diagnostics.rs maybe_recover_from_bad_qpath for an expression whose
// to_ty() is Some (a macro call): `mac!()::x`.
Expr* Parser::maybe_recover_from_bad_qpath_mac_expr(Expr* base) {
    if (!(may_recover() && token.kind == Tok::PathSep)) return base;
    // recover_from_bad_qpath -> maybe_recover_from_bad_qpath_stage_2
    Span ty_span = base->span;
    Ty* ty = mk_ty(ty_span, TyKind::MacCall);
    ty->mac = base->mac;
    expect(EXP(PathSep));
    Path* path = mk_path(Span());
    parse_path_segments(path->segments, PathStyle::Expr, nullptr);
    path->span = ty_span.to(prev_token.span);
    emit("missing angle brackets in associated item path", ty_span);
    QSelf* qself = mk<QSelf>();
    qself->ty = ty;
    qself->path_span = ty_span.shrink_to_hi();
    qself->position = 0;
    Expr* e = mk_expr(path->span, ExprKind::Path);
    e->qself = qself;
    e->path = path;
    return e;
}

/// Error on outer attributes in this context.
/// Also error if the previous token was a doc comment.
void Parser::error_outer_attrs(const AttrWrapper& attrs) {
    if (attrs.is_empty()) return;
    const Attribute& last = attrs.attrs.back();
    if (last.is_doc_comment) {
        emit("found a documentation comment that doesn't document anything", last.span);
    } else {
        for (const Attribute& a : attrs.attrs) {
            if (a.style == AttrStyle::Outer) {
                emit("expected statement after outer attribute", last.span);
                break;
            }
        }
    }
}

Stmt* Parser::recover_stmt_local_after_let(Span lo, AttrWrapper attrs) {
    Local* local = parse_local(OptSpan(), attrs.attrs);
    Stmt* stmt = mk_stmt(lo.to(prev_token.span), StmtKind::Let);
    stmt->local = local;
    emit("invalid variable declaration", lo);
    return stmt;
}

/// Parses a local variable declaration.
Local* Parser::parse_local(OptSpan super_, AttrVec attrs) {
    Span lo = super_.has ? super_.span : prev_token.span;

    if (tok_is_keyword(token, Kw::Const) && look_ahead(1, [this](const Token& t) { return tok_is_ident(t); })) {
        emit("`const` and `let` are mutually exclusive", lo.to(token.span));
        bump();
    }

    std::pair<Pat*, bool> pc = parse_pat_before_ty(Expected::None, RecoverComma::Yes, PatternLocation::LetBinding);
    Pat* pat = pc.first;
    bool colon = pc.second;

    // (err, ty, colon_sp): err = (snapshot after the type, colon span, type error)
    bool have_err = false;
    ParserState parser_snapshot_after_type;
    PErr ty_err;
    Ty* ty = nullptr;
    bool has_colon_sp = false;
    Span colon_sp;
    if (colon) {
        // Save the state of the parser before parsing type normally, in case there is a `:`
        // instead of an `=` typo.
        ParserState parser_snapshot_before_type = create_snapshot_for_diagnostic();
        colon_sp = prev_token.span;
        has_colon_sp = true;
        try {
            ty = parse_ty();
        } catch (PErr& err) {
            // we use noexpect here because we don't actually expect Eq to be here
            // but we are still checking for it in order to be able to handle it if
            // it is there
            if (check_noexpect(Tok::Eq)) {
                emit(err);
            } else {
                // Rewind to before attempting to parse the type and continue parsing.
                parser_snapshot_after_type = create_snapshot_for_diagnostic();
                restore_snapshot(parser_snapshot_before_type);
                have_err = true;
                ty_err = err;
            }
        }
    }

    Expr* init = nullptr;
    try {
        init = parse_initializer(have_err);
    } catch (PErr&) {
        if (have_err) {
            // init parsed, ty error: could not parse the init.
            restore_snapshot(parser_snapshot_after_type);
            throw ty_err;
        }
        throw;
    }
    if (have_err) {
        // init parsed, ty error: could not parse the type, but `=` might have been a typo for `:`.
        emit(ty_err);
    }

    LocalKind kind = LocalKind::Decl;
    Block* els = nullptr;
    if (init != nullptr) {
        if (eat_keyword(KW(Else))) {
            if (tok_is_keyword(token, Kw::If)) {
                // `let...else if`. Emit the same error that `parse_block()` would,
                // but explicitly point out that this pattern is not allowed.
                throw error_block_no_opening_brace_msg("conditional `else if` is not supported for `let...else`");
            }
            els = parse_block();
            check_let_else_init_bool_expr(init);
            check_let_else_init_trailing_brace(init);
            kind = LocalKind::InitElse;
        } else {
            kind = LocalKind::Init;
        }
    }
    Span hi = token.kind == Tok::Semi ? token.span : prev_token.span;
    Local* local = mk<Local>();
    local->is_super = super_.has;
    local->super_span = super_.span;
    local->ty = ty;
    local->pat = pat;
    local->kind = kind;
    local->init = init;
    local->els = els;
    local->span = lo.to(hi);
    local->has_colon = has_colon_sp;
    local->colon_span = colon_sp;
    local->attrs = std::move(attrs);
    return local;
}

void Parser::check_let_else_init_bool_expr(const Expr* init) {
    if (init->kind == ExprKind::Binary && (init->binop == BinOpKind::And || init->binop == BinOpKind::Or)) {
        std::string op = init->binop == BinOpKind::And ? "&&" : "||";
        emit("a `" + op + "` expression cannot be directly assigned in `let...else`", init->span);
    }
}

void Parser::check_let_else_init_trailing_brace(const Expr* init) {
    Span span;
    if (expr_trailing_brace(init, span)) {
        uint32_t hi = span.hi;
        emit("right curly brace `}` before `else` in a `let...else` statement not allowed",
             span.with_lo(hi > 0 ? hi - 1 : hi));
    }
}

/// Parses the RHS of a local variable declaration (e.g., `= 14;`).
Expr* Parser::parse_initializer(bool eq_optional) {
    bool eq_consumed;
    switch (token.kind) {
    case Tok::PlusEq:
    case Tok::MinusEq:
    case Tok::StarEq:
    case Tok::SlashEq:
    case Tok::PercentEq:
    case Tok::CaretEq:
    case Tok::AndEq:
    case Tok::OrEq:
    case Tok::ShlEq:
    case Tok::ShrEq:
        // Recover `let x <op>= 1` as `let x = 1` We must not use `+ BytePos(1)` here because `ne`
        // will always be `=` and the extra op will be handled by `start_point`.
        emit("can't reassign to an uninitialized variable", token.span);
        bump();
        eq_consumed = true;
        break;
    default: eq_consumed = eat(EXP(Eq)); break;
    }
    if (eq_consumed || eq_optional) return parse_expr();
    return nullptr;
}

/// Parses a block. No inner attributes are allowed.
Block* Parser::parse_block() {
    std::pair<AttrVec, Block*> r = parse_inner_attrs_and_block(OptSpan());
    if (!r.first.empty()) {
        const Attribute& last = r.first.back();
        error_on_forbidden_inner_attr(last.span, InnerAttrPolicy::Forbidden(InnerAttrForbiddenReasonKind::InCodeBlock),
                                      !last.is_doc_comment);
    }
    return r.second;
}

// SourceMap::is_line_before_span_empty
bool Parser::is_line_before_span_empty(Span sp) const {
    uint32_t i = sp.lo;
    while (i > 0) {
        char c = src_[i - 1];
        if (c == '\n') return true;
        if (c != ' ' && c != '\t' && c != '\r') return false;
        --i;
    }
    return true;
}

PErr Parser::error_block_no_opening_brace_msg(std::string msg) {
    Span sp = token.span;
    // label_expected_raw_ref: label only.
    bool do_not_suggest_help = tok_is_keyword(token, Kw::In) || token.kind == Tok::Colon ||
                               tok_is_keyword(prev_token, Kw::Raw);

    // Check to see if the user has written something like
    //
    //    if (cond)
    //      bar;
    //
    // which is valid in other languages, but not Rust.
    try {
        Stmt* stmt = parse_stmt_without_recovery(false, ForceCollect::No, false);
        if (stmt != nullptr) {
            bool first = (!tok_is_keyword(token, Kw::Else) &&
                          look_ahead(1, [](const Token& t) { return t.kind == Tok::OpenBrace; })) ||
                         do_not_suggest_help;
            if (!first && stmt->kind != StmtKind::Empty) {
                bool stmt_own_line = is_line_before_span_empty(sp);
                if (stmt_own_line) (void)eat(EXP(Semi));
                // suggest_fixes_misparsed_for_loop_head: suggestions only.
            }
        }
    } catch (PErr&) {
        // e.delay_as_bug()
    }
    return PErr{std::move(msg), sp};
}

[[noreturn]] void Parser::error_block_no_opening_brace() {
    std::string msg = "expected `{`, found " + token_descr(token);
    throw error_block_no_opening_brace_msg(std::move(msg));
}

/// Parses a block. Inner attributes are allowed, block labels are not.
///
/// If `loop_header` is `Some` and an unexpected block label is encountered,
/// it is suggested to be moved just before `loop_header`, else it is suggested to be removed.
std::pair<AttrVec, Block*> Parser::parse_inner_attrs_and_block(OptSpan loop_header) {
    return parse_block_common(token.span, BlockCheckMode::Default, loop_header);
}

/// Parses a block. Inner attributes are allowed, block labels are not.
///
/// If `loop_header` is `Some` and an unexpected block label is encountered,
/// it is suggested to be moved just before `loop_header`, else it is suggested to be removed.
std::pair<AttrVec, Block*> Parser::parse_block_common(Span lo, BlockCheckMode blk_mode, OptSpan loop_header) {
    DepthGuard guard(*this);
    Token maybe_ident = prev_token;
    (void)try_recover_unexpected_block_label(loop_header);
    if (!eat(EXP(OpenBrace))) error_block_no_opening_brace();

    AttrVec attrs = parse_inner_attributes();
    Block* tail = nullptr;
    if (!maybe_suggest_struct_literal(lo, blk_mode, maybe_ident, tail))
        tail = parse_block_tail(lo, blk_mode, AttemptLocalParseRecovery::Yes);
    return {std::move(attrs), tail};
}

// diagnostics.rs maybe_suggest_struct_literal: true when it handled the block (tail set or thrown).
bool Parser::maybe_suggest_struct_literal(Span lo, BlockCheckMode s, const Token& maybe_struct_name, Block*& tail) {
    (void)maybe_struct_name;
    if (!(tok_is_ident(token) && look_ahead(1, [](const Token& t) { return t.kind == Tok::Colon; }))) return false;
    // We might be having a struct literal where people forgot to include the path:
    // fn foo() -> Foo {
    //     field: value,
    // }
    ParserState before = create_snapshot_for_diagnostic();
    Path* path = mk_path(prev_token.span.shrink_to_lo());
    bool struct_ok = false;
    Expr* struct_expr = nullptr;
    try {
        struct_expr = parse_expr_struct(nullptr, path, false);
        struct_ok = true;
    } catch (PErr&) {
    }
    ParserState snapshot = create_snapshot_for_diagnostic();  // the snapshot parser after the struct
    restore_snapshot(before);

    bool tail_ok = false;
    PErr tail_err;
    Block* block_tail = nullptr;
    try {
        block_tail = parse_block_tail(lo, s, AttemptLocalParseRecovery::No);
        tail_ok = true;
    } catch (PErr& e) {
        tail_err = e;
    }
    if (struct_ok && !tail_ok) {
        // We have encountered the following:
        // fn foo() -> Foo {
        //     field: value,
        // }
        // Suggest:
        // fn foo() -> Foo { Path {
        //     field: value,
        // } }
        restore_snapshot(snapshot);
        emit("struct literal body without path", struct_expr->span);
        std::vector<Stmt*> stmts{mk_stmt_err(struct_expr->span)};
        tail = mk_block(std::move(stmts), s, lo.to(prev_token.span));
        return true;
    }
    if (!struct_ok && tail_ok) {
        tail = block_tail;
        return true;
    }
    if (!struct_ok && !tail_ok) {
        consume_block(EXP(OpenBrace), EXP(CloseBrace), true);
        throw tail_err;
    }
    tail = block_tail;
    return true;
}

/// Parses the rest of a block expression or function body.
/// Precondition: already parsed the '{'.
Block* Parser::parse_block_tail(Span lo, BlockCheckMode s, AttemptLocalParseRecovery recover) {
    std::vector<Stmt*> stmts;
    bool have_snapshot = false;
    ParserState snapshot;
    while (!eat(EXP(CloseBrace))) {
        if (token.kind == Tok::Eof) break;
        if (is_vcs_conflict_marker(Tok::Shl, Tok::Lt)) {
            // Account for `<<<<<<<` diff markers. We can't proactively error here because
            // that can be a valid path start, so we snapshot and reparse only we've
            // encountered another parse error.
            snapshot = create_snapshot_for_diagnostic();
            have_snapshot = true;
        }
        Stmt* stmt = nullptr;
        try {
            stmt = parse_full_stmt(recover);
        } catch (PErr& err) {
            if (recover != AttemptLocalParseRecovery::Yes) throw;
            if (have_snapshot) recover_vcs_conflict_marker_in(snapshot);
            if (token.kind == Tok::Colon) {
                // if a previous and next token of the current one is
                // integer literal (e.g. `1:42`), it's likely a range
                // expression for Pythonistas and we can suggest so.
                if (tok_is_integer_lit(prev_token) && may_recover() &&
                    look_ahead(1, [this](const Token& t) { return tok_is_integer_lit(t); })) {
                    // suggestion only
                } else {
                    // if next token is following a colon, it's likely a path
                    // and we can suggest a path separator
                    bump();
                }
            }
            emit(err);
            recover_stmt_(SemiColonMode::Ignore, BlockMode::Ignore);
            stmt = mk_stmt_err(token.span);
        }
        if (stmt != nullptr) stmts.push_back(stmt);
    }
    return mk_block(std::move(stmts), s, lo.to(prev_token.span));
}

// Runs recover_vcs_conflict_marker on a snapshot parser (`snapshot.recover_vcs_conflict_marker()`).
void Parser::recover_vcs_conflict_marker_in(ParserState& snapshot) {
    ParserState cur = create_snapshot_for_diagnostic();
    restore_snapshot(snapshot);
    try {
        recover_vcs_conflict_marker();
    } catch (...) {
        snapshot = create_snapshot_for_diagnostic();
        restore_snapshot(cur);
        throw;
    }
    snapshot = create_snapshot_for_diagnostic();
    restore_snapshot(cur);
}

// stmt.rs recover_missing_let_else: only suggestions, but the speculative
// block parse is kept when it succeeds (rustc does not restore then).
void Parser::recover_missing_let_else(const Pat* pat) {
    if (token.kind != Tok::OpenBrace) return;
    switch (pat->kind) {
    case PatKind::Ident:
    case PatKind::Missing:
    case PatKind::Wild: return;
    default: break;
    }
    ParserState snapshot = create_snapshot_for_diagnostic();
    try {
        (void)parse_block();
    } catch (PErr&) {
        restore_snapshot(snapshot);
    }
}

/// Parses a statement, including the trailing semicolon.
Stmt* Parser::parse_full_stmt(AttemptLocalParseRecovery recover) {
    Stmt* stmt = parse_stmt_without_recovery(true, ForceCollect::No, false);
    if (stmt == nullptr) return nullptr;

    bool eat_semi = true;
    bool add_semi_to_stmt = false;

    switch (stmt->kind) {
    case StmtKind::Expr: {
        Expr*& expr = stmt->expr;
        // Expression without semicolon.
        if (expr_requires_semi_to_be_stmt(expr) && !expr->attrs.empty() && token.kind != Tok::Eof &&
            token.kind != Tok::Semi && token.kind != Tok::CloseBrace) {
            // Just check for errors and recover; do not eat semicolon yet.
            attr_on_non_tail_expr(expr);
            Span sp = expr->span.to(prev_token.span);
            expr = mk_expr_err(sp);
        } else if (token.kind != Tok::Eof && expr_requires_semi_to_be_stmt(expr)) {
            // Just check for errors and recover; do not eat semicolon yet.
            bool ok = false;
            bool recovered = false;
            PErr e;
            try {
                maybe_recover_from_ternary_operator(OptSpan(expr->span));
                recovered = expect_one_of({}, {EXP(Semi), EXP(CloseBrace)});
                ok = true;
            } catch (PErr& err) {
                e = err;
            }
            bool replace_with_err = false;
            if (ok) {
                replace_with_err = recovered;
            } else {
                bool done = false;
                if (recover_colon_as_semi()) {
                    // recover_colon_as_semi has already emitted a nicer error.
                    add_semi_to_stmt = true;
                    eat_semi = false;
                    done = true;
                }
                if (!done && expr->kind == ExprKind::Path && expr->qself == nullptr && expr->path != nullptr &&
                    expr->path->segments.size() == 1) {
                    const PathSegment& segment = expr->path->segments[0];
                    if (token.kind == Tok::Colon && look_ahead(1, [](const Token& t) {
                            return t.kind == Tok::OpenBrace ||
                                   (t.kind == Tok::Ident && !t.raw &&
                                    (t.kw == Kw::For || t.kw == Kw::Loop || t.kw == Kw::While));
                        })) {
                        ParserState snapshot = create_snapshot_for_diagnostic();
                        file_.interned.push_back("'" + std::string(segment.ident.name));
                        Label label;
                        label.ident.name = file_.interned.back();
                        label.ident.span = segment.ident.span;
                        try {
                            Expr* labeled_expr = parse_expr_labeled(label, false);
                            emit("malformed loop label", label.ident.span);
                            expr = labeled_expr;
                            done = true;
                        } catch (PErr&) {
                            restore_snapshot(snapshot);
                        }
                    }
                }
                if (!done) {
                    // Continue the statement: `check_mistyped_turbofish_with_multiple_type_params`
                    // either recovers (Ok(guar)) or hands the error back.
                    if (recover == AttemptLocalParseRecovery::No) {
                        check_mistyped_turbofish_with_multiple_type_params(e, expr);
                    } else {
                        try {
                            check_mistyped_turbofish_with_multiple_type_params(e, expr);
                        } catch (PErr& e2) {
                            // recover_missing_dot: suggestions only.
                            emit(e2);
                            recover_stmt();
                        }
                    }
                    replace_with_err = true;
                }
            }
            if (replace_with_err) {
                Span sp = expr->span.to(prev_token.span);
                expr = mk_expr_err(sp);
            }
        }
        break;
    }
    case StmtKind::MacCall: break;
    case StmtKind::Let: {
        Local* local = stmt->local;
        if (try_recover_let_missing_semi(local)) {
            // `try_recover_let_missing_semi` recovers the whole statement.
            return stmt;
        }
        try {
            expect_semi();
        } catch (PErr& e) {
            if (local->kind == LocalKind::Init || local->kind == LocalKind::InitElse) {
                try {
                    check_mistyped_turbofish_with_multiple_type_params(e, local->init);
                } catch (PErr&) {
                    // recover_missing_dot: suggestions only.
                    recover_missing_let_else(local->pat);
                    throw;
                }
                expect_semi();
            } else {
                if (local->has_colon) {
                    // `suggest_eq` probes `.` after the type, bumping it first.
                    if (token.kind == Tok::Dot) bump();
                }
                throw;
            }
        }
        eat_semi = false;
        break;
    }
    case StmtKind::Empty:
    case StmtKind::Item:
    case StmtKind::Semi: eat_semi = false; break;
    }

    if (add_semi_to_stmt || (eat_semi && eat(EXP(Semi)))) {
        // add_trailing_semicolon
        if (stmt->kind == StmtKind::Expr)
            stmt->kind = StmtKind::Semi;
        else if (stmt->kind == StmtKind::MacCall)
            stmt->mac->style = MacStmtStyle::Semicolon;
    }

    stmt->span = stmt->span.to(prev_token.span);
    return stmt;
}

bool Parser::try_recover_let_missing_semi(Local* local) {
    if (local->kind == LocalKind::Decl) return false;
    std::optional<Span> sp = missing_semi_from_binop("`let` binding", local->init, OptSpan(local->span.shrink_to_lo()));
    if (sp) {
        st_.fn_body_missing_semi_guar = true;
        local->init = mk_expr_err(*sp);
        return true;
    }
    return false;
}

// diagnostics.rs attr_on_non_tail_expr: the `ExpectedSemi` error.
void Parser::attr_on_non_tail_expr(const Expr* expr) {
    (void)expr;
    Span span = prev_token.span.shrink_to_hi();
    emit("expected `;`, found " + token_descr(token), span);
}

Stmt* Parser::mk_stmt_err(Span span) {
    Stmt* s = mk_stmt(span, StmtKind::Expr);
    s->expr = mk_expr_err(span);
    return s;
}

}  // namespace detail
}  // namespace rust
