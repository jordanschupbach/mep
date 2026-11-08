// rust_parse_path.cpp -- paths and generic arguments
// (rustc_parse/src/parser/path.rs) plus the const-argument recovery of
// diagnostics.rs.
//
// Ported from rustc_parse/rustc_lexer (rust-lang/rust 1.97.1, MIT OR
// Apache-2.0).

#include "rust_parse_internal.h"

namespace rust {
namespace detail {

namespace {

bool path_style_has_generic_ambiguity(PathStyle s) { return s == PathStyle::Expr || s == PathStyle::Pat; }

/// TokenKind::should_end_const_arg
bool should_end_const_arg(Tok k) { return k == Tok::Gt || k == Tok::Ge || k == Tok::Shr || k == Tok::ShrEq; }

}  // namespace

// ---------------------------------------------------------------------------
// Span helpers for the generic-argument nodes.

Span Parser::generic_arg_span(const GenericArg& a) {
    switch (a.kind) {
    case GenericArg::Lifetime: return a.lifetime.ident.span;
    case GenericArg::Type: return a.ty ? a.ty->span : Span();
    case GenericArg::Const: return a.konst.value ? a.konst.value->span : Span();
    }
    return Span();
}

Span Parser::angle_arg_span(const AngleBracketedArg& a) {
    if (a.is_constraint) return a.constraint ? a.constraint->span : Span();
    return generic_arg_span(a.arg);
}

Span Parser::path_segment_span(const PathSegment& s) {
    if (s.args) return s.ident.span.to(s.args->span);
    return s.ident.span;
}

// ---------------------------------------------------------------------------
// path.rs

std::pair<QSelf*, Path*> Parser::parse_qpath(PathStyle style) {
    Span lo = prev_token.span;
    Ty* ty = parse_ty();
    Path* path;
    Span path_span;
    if (eat_keyword(KW(As))) {
        Span path_lo = token.span;
        path = parse_path(PathStyle::Type);
        path_span = path_lo.to(prev_token.span);
    } else {
        path_span = token.span.to(token.span);
        path = mk_path(path_span);
    }
    expect(EXP(Gt));
    if (st_.unmatched_angle_bracket_count > 0) --st_.unmatched_angle_bracket_count;

    bool coupler = is_import_coupler();
    if (!coupler && !recover_colon_before_qpath_proj()) expect(EXP(PathSep));

    QSelf* qself = mk<QSelf>();
    qself->ty = ty;
    qself->path_span = path_span;
    qself->position = path->segments.size();

    if (!coupler) parse_path_segments(path->segments, style, nullptr);
    path->span = lo.to(prev_token.span);
    return {qself, path};
}

bool Parser::recover_colon_before_qpath_proj() {
    if (!check_noexpect(Tok::Colon) || look_ahead(1, [this](const Token& t) { return !tok_is_non_reserved_ident(t); }))
        return false;
    bump();  // colon
    emit("found single colon before projection in qualified path", prev_token.span);
    return true;
}

Path* Parser::parse_path(PathStyle style) { return parse_path_inner(style, nullptr); }

Path* Parser::parse_path_inner(PathStyle style, const Generics* ty_generics) {
    // (The `$path` / `$ty` metavariable cases only arise inside macro expansion.)
    Span lo = token.span;
    Path* path = mk_path(lo);
    if (eat_path_sep()) {
        PathSegment root;
        root.ident.name = "{{root}}";
        root.ident.span = lo.shrink_to_lo();
        path->segments.push_back(root);
    }
    parse_path_segments(path->segments, style, ty_generics);
    path->span = lo.to(prev_token.span);
    return path;
}

void Parser::parse_path_segments(std::vector<PathSegment>& segments, PathStyle style, const Generics* ty_generics) {
    for (;;) {
        PathSegment segment = parse_path_segment(style, ty_generics);
        if (path_style_has_generic_ambiguity(style)) {
            // Detect and recover from `Foo::<Bar>>::baz` (extra `>`).
            check_trailing_angle_brackets(segment, {EXP(PathSep)});
        }
        segments.push_back(segment);

        if (is_import_coupler() || !eat_path_sep()) {
            if (may_recover() && style == PathStyle::Expr && token.kind == Tok::Colon &&
                look_ahead(1, [this](const Token& t) { return tok_is_non_reserved_ident(t); })) {
                // Emit a special error for `a::b:c` instead of `a::b::c`.
                if (token.span.lo == prev_token.span.hi &&
                    look_ahead(1, [this](const Token& t) { return token.span.hi == t.span.lo; })) {
                    bump();  // bump past the colon
                    emit("path separator must be a double colon", prev_token.span);
                }
                continue;
            }
            return;
        }
    }
}

bool Parser::eat_path_sep() {
    bool result = eat(EXP(PathSep));
    if (result && may_recover()) {
        if (eat_noexpect(Tok::Colon)) emit("path separator must be a double colon", prev_token.span);
    }
    return result;
}

PathSegment Parser::parse_path_segment(PathStyle style, const Generics* ty_generics) {
    Ident ident = parse_path_segment_ident();
    auto is_args_start = [](const Token& t) {
        return t.kind == Tok::Lt || t.kind == Tok::Shl || t.kind == Tok::OpenParen || t.kind == Tok::LArrow;
    };
    auto check_args_start = [&]() {
        st_.expected_token_types.insert(TokenType::Lt);
        st_.expected_token_types.insert(TokenType::OpenParen);
        return is_args_start(token);
    };

    PathSegment seg;
    seg.ident = ident;
    if (!((style == PathStyle::Type && check_args_start()) ||
          (style != PathStyle::Mod && check_path_sep_and_look_ahead(is_args_start))))
        return seg;

    // We use `style == PathStyle::Expr` to check if this is in a recursion or not. If
    // it isn't, then we reset the unmatched angle bracket count as we're about to start
    // parsing a new path.
    if (style == PathStyle::Expr) st_.unmatched_angle_bracket_count = 0;

    // Generic arguments are found - `<`, `(`, `::<` or `::(`.
    (void)eat_path_sep();
    Span lo = token.span;
    GenericArgs* args = mk<GenericArgs>();
    if (eat_lt()) {
        // `<'a, T, A = U>`
        std::vector<AngleBracketedArg> a = parse_angle_args_with_leading_angle_bracket_recovery(style, lo, ty_generics);
        try {
            expect_gt();
        } catch (PErr& err) {
            if (token.kind == Tok::Colon && look_ahead(1, [this](const Token& t) { return tok_is_non_reserved_ident(t); }))
                throw PErr{"path separator must be a double colon", token.span};
            throw;
        }
        args->kind = GenericArgs::AngleBracketed;
        args->args = std::move(a);
        args->span = lo.to(prev_token.span);
    } else if (token.kind == Tok::OpenParen && look_ahead(1, [](const Token& t) { return t.kind == Tok::DotDot; })) {
        bump();  // (
        bump();  // ..
        expect(EXP(CloseParen));
        Span span = lo.to(prev_token.span);
        Span prev_lo = prev_token.span.shrink_to_hi();
        (void)prev_lo;
        if (eat_noexpect(Tok::RArrow)) {
            Span rlo = prev_token.span;
            Ty* ty = parse_ty();
            emit("return type not allowed with return type notation", rlo.to(ty->span));
        }
        args->kind = GenericArgs::ParenthesizedElided;
        args->span = span;
    } else {
        // `(T, U) -> R`
        Token prev_token_before_parsing = prev_token;
        bool have_snapshot = false;
        ParserState snapshot;
        if (may_recover() && prev_token_before_parsing.kind == Tok::PathSep &&
            ((style == PathStyle::Expr && tok_can_begin_expr(token)) ||
             (style == PathStyle::Pat && tok_can_begin_pattern(token, false)))) {
            snapshot = create_snapshot_for_diagnostic();
            have_snapshot = true;
        }
        std::vector<Ty*> inputs;
        try {
            inputs = parse_paren_comma_seq<Ty*>([this]() { return parse_path_paren_arg(); });
        } catch (PErr& error) {
            if (prev_token_before_parsing.kind == Tok::PathSep && have_snapshot) {
                // recover_fn_call_leading_path_sep only adds a suggestion, but it
                // parses on the snapshot (whose own errors are reported).
                ParserState cur = st_;
                st_ = snapshot;
                recover_fn_call_leading_path_sep(style);
                st_ = cur;
            }
            throw error;
        }
        Span inputs_span = lo.to(prev_token.span);
        FnRetTy output = parse_ret_ty(AllowPlus::No, RecoverQPath::No, RecoverReturnSign::No);
        args->kind = GenericArgs::Parenthesized;
        args->span = ident.span.to(prev_token.span);
        args->inputs = std::move(inputs);
        args->inputs_span = inputs_span;
        args->output = output;
    }
    seg.args = args;
    return seg;
}

/// The parenthesized-args closure of parse_path_segment:
/// parse_param_general(&FnParseMode { req_name: |_, _| false, .. }, false, false)
/// and then the checks on the parameter.
Ty* Parser::parse_path_paren_arg() {
    FnParseMode mode;
    mode.req_name = ReqName::Never;
    mode.context = FnContext::Free;
    mode.req_body = false;
    Param* param = parse_param_general(mode, false, false);
    if (param->pat && param->pat->kind != PatKind::Missing)
        emit("`Trait(...)` syntax does not support named parameters", param->pat->span);
    if (param->ty && param->ty->kind == TyKind::CVarArgs)
        emit("`Trait(...)` syntax does not support c_variadic parameters", param->pat ? param->pat->span : param->ty->span);
    if (!param->attrs.empty()) emit("`Trait(...)` syntax does not support attributes in parameters", param->attrs[0].span);
    return param->ty;
}

Ident Parser::parse_path_segment_ident() {
    if (token.kind == Tok::Ident && !token.raw && tok_is_path_segment_keyword(token)) {
        Ident id = tok_ident(token);
        bump();
        return id;
    }
    return parse_ident();
}

void Parser::recover_fn_call_leading_path_sep(PathStyle style) {
    // Only a suggestion; the speculative parse still runs (and reports).
    try {
        if (style == PathStyle::Expr)
            (void)parse_paren_comma_seq<Expr*>([this]() { return parse_expr(); });
        else if (style == PathStyle::Pat)
            (void)parse_paren_comma_seq<Pat*>([this]() {
                return parse_pat_allow_top_guard(Expected::None, RecoverComma::No, RecoverColon::No,
                                                 CommaRecoveryMode::LikelyTuple);
            });
    } catch (PErr&) {
    }
}

std::vector<AngleBracketedArg> Parser::parse_angle_args_with_leading_angle_bracket_recovery(PathStyle style, Span lo,
                                                                                         const Generics* ty_generics) {
    // We need to detect whether there are extra leading left angle brackets and produce an
    // appropriate error and suggestion.
    bool is_first_invocation = style == PathStyle::Expr;
    ParserState snapshot;
    if (is_first_invocation) snapshot = st_;
    ++st_.angle_bracket_nesting;
    try {
        std::vector<AngleBracketedArg> args = parse_angle_args(ty_generics);
        --st_.angle_bracket_nesting;
        return args;
    } catch (PErr& e) {
        if (st_.angle_bracket_nesting > 10) {
            --st_.angle_bracket_nesting;
            emit(e);
            throw FatalAbort{};
        }
        if (is_first_invocation && st_.unmatched_angle_bracket_count > 0) {
            --st_.angle_bracket_nesting;
            // Swap `self` with our backup of the parser state before attempting to parse
            // generic arguments.
            ParserState after = st_;
            st_ = snapshot;
            uint16_t count = after.unmatched_angle_bracket_count;
            // Eat the unmatched angle brackets.
            bool all_angle_brackets = true;
            for (uint16_t i = 0; i < count; ++i) all_angle_brackets = all_angle_brackets && eat_lt();
            if (!all_angle_brackets) {
                // If there are other tokens in between the extraneous `<`s, we cannot simply
                // suggest to remove them.
                st_ = after;
                throw;
            }
            Span span = lo.with_hi(lo.lo + count);
            emit(count > 1 ? "unmatched angle brackets" : "unmatched angle bracket", span);
            // Try again without unmatched angle bracket characters.
            return parse_angle_args(ty_generics);
        }
        --st_.angle_bracket_nesting;
        throw;
    }
}

std::vector<AngleBracketedArg> Parser::parse_angle_args(const Generics* ty_generics) {
    std::vector<AngleBracketedArg> args;
    AngleBracketedArg arg;
    while (parse_angle_arg(ty_generics, arg)) {
        args.push_back(arg);
        if (!eat(EXP(Comma))) {
            if (check_noexpect(Tok::Semi) &&
                look_ahead(1, [this](const Token& t) { return tok_is_ident(t) || tok_is_lifetime(t); })) {
                // Add `>` to the list of expected tokens.
                check(EXP(Gt));
                // Handle `,` to `;` substitution
                PErr err;
                try {
                    unexpected();
                } catch (PErr& e) {
                    err = e;
                }
                bump();
                emit(err);
                continue;
            }
            if (!should_end_const_arg(token.kind) && handle_ambiguous_unbraced_const_arg(args)) {
                // We've managed to (partially) recover, so continue trying to parse arguments.
                continue;
            }
            break;
        }
    }
    return args;
}

bool Parser::parse_angle_arg(const Generics* ty_generics, AngleBracketedArg& out) {
    Span lo = token.span;
    std::optional<GenericArg> arg = parse_generic_arg(ty_generics);
    if (!arg) return false;
    out = AngleBracketedArg();
    // we are using noexpect here because we first want to find out if either `=` or `:`
    // is present and then use that info to push the other token onto the tokens list
    bool separated = check_noexpect(Tok::Colon) || check_noexpect(Tok::Eq);
    // `separated && (check(:) | check(=))`: both checks run, but only when separated.
    bool checked = false;
    if (separated) {
        bool c1 = check(EXP(Colon));
        bool c2 = check(EXP(Eq));
        checked = c1 || c2;
    }
    if (checked) {
        Span arg_span = generic_arg_span(*arg);
        bool binder = false;
        Ident ident;
        GenericArgs* gen_args = nullptr;
        if (!get_ident_from_generic_arg(*arg, binder, ident, gen_args)) {
            out.arg = *arg;
            return true;
        }
        if (binder) {
            // FIXME(compiler-errors): this could be improved by suggesting lifting
            // this up to the trait, at least before this becomes real syntax.
            // e.g. `Trait<for<'a> Assoc = Ty>` -> `for<'a> Trait<Assoc = Ty>`
            fail("`for<...>` is not allowed on associated type bounds", arg_span);
        }
        AssocItemConstraint* c = mk<AssocItemConstraint>();
        c->ident = ident;
        c->gen_args = gen_args;
        if (eat(EXP(Colon))) {
            c->is_equality = false;
            c->bounds = mk<GenericBounds>();
            *c->bounds = parse_generic_bounds();
        } else if (check(EXP(Eq))) {
            c->is_equality = true;
            parse_assoc_equality_term(ident, gen_args, c);
        }
        c->span = lo.to(prev_token.span);
        out.is_constraint = true;
        out.constraint = c;
        return true;
    }
    if (tok_is_ident(prev_token) &&
        (tok_is_ident(token) || look_ahead(1, [this](const Token& t) { return tok_is_ident(t); }))) {
        // We are probably inside of a `Path<>` and have a typo: `Path<a b>`; add `:` and `=`
        // to the list of expected tokens.
        check(EXP(Colon));
        check(EXP(Eq));
    }
    out.arg = *arg;
    return true;
}

void Parser::parse_assoc_equality_term(Ident ident, GenericArgs* gen_args, AssocItemConstraint* c) {
    (void)gen_args;
    Span eq_span = token.span;
    expect(EXP(Eq));
    std::optional<GenericArg> arg = parse_generic_arg(nullptr);
    if (!arg) {
        Span after_eq = eq_span.shrink_to_hi();
        Span before_next = token.span.shrink_to_lo();
        fail("missing type to the right of `=`", after_eq.to(before_next));
    }
    Span span = ident.span.to(prev_token.span);
    (void)span;
    switch (arg->kind) {
    case GenericArg::Type: c->term_ty = arg->ty; break;
    case GenericArg::Const: c->term_const = arg->konst; break;
    case GenericArg::Lifetime:
        emit("lifetimes are not permitted in this context", arg->lifetime.ident.span);
        c->term_ty = mk_ty(arg->lifetime.ident.span, TyKind::Err);
        break;
    }
}

bool Parser::expr_is_valid_const_arg(const Expr* expr) const {
    switch (expr->kind) {
    case ExprKind::Block:
    case ExprKind::Lit:
    case ExprKind::IncludedBytes: return true;
    case ExprKind::Unary: return expr->unop == UnOp::Neg && expr->lhs && expr->lhs->kind == ExprKind::Lit;
    case ExprKind::Path:
        // A single-segment path.
        return expr->qself == nullptr && expr->path && expr->path->segments.size() == 1 &&
               expr->path->segments[0].args == nullptr;
    case ExprKind::ConstBlock: return true;
    default: return false;
    }
}

AnonConst Parser::parse_const_arg() {
    AnonConst c;
    if (token.kind == Tok::OpenBrace) {
        c.value = parse_expr_block(std::nullopt, token.span, BlockCheckMode::Default);
    } else {
        c = parse_unambiguous_unbraced_const_arg();
    }
    c.direct = true;
    return c;
}

AnonConst Parser::parse_unambiguous_unbraced_const_arg() {
    AttrWrapper attrs = parse_outer_attributes();
    Expr* expr = parse_expr_res(R_CONST_EXPR, attrs).first;
    if (!expr_is_valid_const_arg(expr))
        emit("expressions must be enclosed in braces to be used as const generic arguments", expr->span);
    AnonConst c;
    c.value = expr;
    c.direct = true;
    return c;
}

std::optional<GenericArg> Parser::parse_generic_arg(const Generics* ty_generics) {
    OptSpan attr_span;
    if (token.kind == Tok::Pound && look_ahead(1, [](const Token& t) { return t.kind == Tok::OpenBracket; })) {
        AttrWrapper w = parse_outer_attributes();
        if (!w.attrs.empty()) attr_span = OptSpan(w.attrs.front().span.to(w.attrs.back().span));
    }
    Span start = token.span;
    GenericArg arg;
    if (check_lifetime() && look_ahead(1, [this](const Token& t) { return !tok_is_like_plus(t); })) {
        // Parse lifetime argument.
        arg.kind = GenericArg::Lifetime;
        arg.lifetime = expect_lifetime();
    } else if (check_const_arg()) {
        // Parse const argument.
        arg.kind = GenericArg::Const;
        arg.konst = parse_const_arg();
    } else if (check_type()) {
        bool have_snapshot = false;
        ParserState snapshot;
        if (may_recover() && tok_can_begin_expr(token)) {
            snapshot = create_snapshot_for_diagnostic();
            have_snapshot = true;
        }
        Ty* ty = nullptr;
        try {
            ty = parse_ty();
        } catch (PErr& err) {
            bool stopped_at_doc_comment = token.kind == Tok::DocComment;
            if (!stopped_at_doc_comment && have_snapshot) {
                if (Expr* expr = recover_unbraced_const_arg_that_can_begin_ty(snapshot))
                    return dummy_const_arg_needs_braces(err, expr->span);
            }
            // Try to recover from possible `const` arg without braces.
            return recover_const_arg(start, err);
        }
        // Since the type parser recovers from some malformed slice and array types and
        // successfully returns a type, we need to look for `TyKind::Err`s in the
        // type to determine if error recovery has occurred and if the input is not a
        // syntactically valid type after all.
        if ((ty->kind == TyKind::Slice || ty->kind == TyKind::Array) && ty->inner && ty->inner->kind == TyKind::Err &&
            have_snapshot) {
            if (Expr* expr = recover_unbraced_const_arg_that_can_begin_ty(snapshot))
                return dummy_const_arg_needs_braces(PErr{"invalid const generic expression", expr->span}, expr->span);
        }
        arg.kind = GenericArg::Type;
        arg.ty = ty;
    } else if (tok_is_keyword(token, Kw::Const)) {
        return recover_const_param_declaration(ty_generics);
    } else if (attr_span.has) {
        fail("attributes cannot be applied here", attr_span.span);
    } else {
        // Fall back by trying to parse a const-expr expression. If we successfully do so,
        // then we should report an error that it needs to be wrapped in braces.
        ParserState snapshot = create_snapshot_for_diagnostic();
        try {
            AttrWrapper attrs = parse_outer_attributes();
            Expr* expr = parse_expr_res(R_CONST_EXPR, attrs).first;
            return dummy_const_arg_needs_braces(PErr{"invalid const generic expression", expr->span}, expr->span);
        } catch (PErr&) {
            restore_snapshot(snapshot);
            return std::nullopt;
        }
    }

    if (attr_span.has) {
        emit("attributes cannot be applied to generic arguments", attr_span.span);
        GenericArg r = arg;
        if (arg.kind == GenericArg::Type) {
            r.ty = mk_ty(attr_span.span, TyKind::Err);
        } else if (arg.kind == GenericArg::Const) {
            r.konst = AnonConst();
            r.konst.value = mk_expr_err(attr_span.span);
            r.konst.direct = true;
        }
        return r;
    }
    return arg;
}

bool Parser::get_ident_from_generic_arg(const GenericArg& gen_arg, bool& binder, Ident& ident, GenericArgs*& args) {
    if (gen_arg.kind != GenericArg::Type || !gen_arg.ty) return false;
    const Ty* ty = gen_arg.ty;
    if (ty->kind == TyKind::Path && ty->qself == nullptr && ty->path && ty->path->segments.size() == 1) {
        binder = false;
        ident = ty->path->segments[0].ident;
        args = ty->path->segments[0].args;
        return true;
    }
    if (ty->kind == TyKind::TraitObject && !ty->dyn_syntax && ty->bounds.size() == 1 &&
        ty->bounds[0].kind == GenericBound::Trait) {
        const PolyTraitRef& tr = ty->bounds[0].trait;
        const TraitBoundModifiers& m = tr.modifiers;
        if (m.constness == BoundConstnessKind::Never && !m.async_ && m.polarity == BoundPolarityKind::Positive &&
            tr.trait_ref && tr.trait_ref->segments.size() == 1) {
            binder = true;
            ident = tr.trait_ref->segments[0].ident;
            args = tr.trait_ref->segments[0].args;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// diagnostics.rs: trailing angle brackets, turbofish, const arguments.

bool Parser::check_trailing_angle_brackets(const PathSegment& segment, std::initializer_list<ExpTok> end) {
    if (!may_recover()) return false;
    // This function is intended to be invoked after parsing a path segment where there are two
    // cases:
    //
    // 1. A specific token is expected after the path segment.
    //    eg. `x.foo(`, `x.foo::<u32>(` (parenthesis - method call),
    //        `Foo::`, or `Foo::<Bar>::` (mod sep - continued path).
    // 2. No specific token is expected after the path segment.
    //    eg. `x.foo` (field access)
    //
    // This function is called after parsing `.foo` and before parsing the token `end` (if
    // present). This includes any angle bracket arguments, such as `.foo::<u32>` or
    // `Foo::<Bar>`.
    bool parsed_angle_bracket_args = segment.args && segment.args->kind == GenericArgs::AngleBracketed;
    if (!parsed_angle_bracket_args) return false;

    // Keep the span at the start so we can highlight the sequence of `>` characters to be
    // removed.
    Span lo = token.span;
    size_t position = 0;
    uint32_t number_of_shr = 0;
    uint32_t number_of_gt = 0;
    for (;;) {
        const Token& t = look_ahead_tok(position);
        if (t.kind == Tok::Shr)
            ++number_of_shr;
        else if (t.kind == Tok::Gt)
            ++number_of_gt;
        else
            break;
        ++position;
    }
    if (number_of_gt < 1 && number_of_shr < 1) return false;

    // Finally, double check that we have our end token as otherwise this is the
    // second case.
    const Token& after = look_ahead_tok(position);
    bool at_end = false;
    for (const ExpTok& e : end)
        if (e.tok == after.kind) at_end = true;
    if (at_end) {
        // Eat from where we started until the end token so that parsing can continue
        // as if we didn't have those extra angle brackets.
        eat_to_tokens(end);
        Span span = lo.to(prev_token.span);
        uint32_t num_extra_brackets = number_of_gt + number_of_shr * 2;
        emit(num_extra_brackets == 1 ? "unmatched angle bracket" : "unmatched angle brackets", span);
        return true;
    }
    return false;
}

void Parser::check_mistyped_turbofish_with_multiple_type_params(PErr e, Expr*& expr) {
    if (expr->kind == ExprKind::Binary && expr->binop == BinOpKind::Lt && eat(EXP(Comma))) {
        // Recover from bad turbofish: `foo < bar, baz > ()`.
        try {
            bool recovered = false;
            (void)parse_seq_to_before_end<GenericArg>(
                EXP(Gt), SeqSep::trailing_allowed(EXP(Comma)),
                [this]() {
                    std::optional<GenericArg> a = parse_generic_arg(nullptr);
                    if (!a) unexpected_any();
                    return *a;
                },
                nullptr, &recovered);
            if (!recovered && eat(EXP(Gt))) {
                try {
                    (void)parse_expr();
                    emit(e);
                    expr = mk_expr_err(expr->span.to(prev_token.span));
                    return;
                } catch (PErr&) {
                }
            }
        } catch (PErr&) {
        }
    }
    throw e;
}

bool Parser::handle_ambiguous_unbraced_const_arg(std::vector<AngleBracketedArg>& args) {
    // If we haven't encountered a closing `>`, then the argument is malformed.
    // It's likely that the user has written a const expression without enclosing it
    // in braces, so we try to parse it as a const expression and report an error
    // suggesting braces.
    AngleBracketedArg arg = args.back();
    args.pop_back();
    PErr err{"expected one of `,` or `>`, found " + token_descr(token), token.span};
    try {
        GenericArg a = recover_const_arg(angle_arg_span(arg), err);
        AngleBracketedArg aa;
        aa.arg = a;
        args.push_back(aa);
        if (eat(EXP(Comma))) return true;  // Continue
    } catch (PErr&) {
        args.push_back(arg);
        // (delay_as_bug: not reported)
    }
    return false;  // Don't continue.
}

std::optional<GenericArg> Parser::recover_const_param_decl(const Generics* ty_generics) {
    (void)ty_generics;
    ParserState snapshot = create_snapshot_for_diagnostic();
    GenericParam* param = nullptr;
    try {
        param = parse_const_param(AttrVec());
    } catch (PErr&) {
        restore_snapshot(snapshot);
        return std::nullopt;
    }
    Span ps = param->span();
    emit("unexpected `const` parameter declaration", ps);
    GenericArg a;
    a.kind = GenericArg::Const;
    a.konst.value = mk_expr_err(ps);
    a.konst.direct = true;
    return a;
}

std::optional<GenericArg> Parser::recover_const_param_declaration(const Generics* ty_generics) {
    // We have to check for a few different cases.
    if (std::optional<GenericArg> arg = recover_const_param_decl(ty_generics)) return arg;

    // We haven't consumed `const` yet.
    Span start = token.span;
    bump();  // `const`

    // Detect and recover from the old, pre-RFC2000 syntax for const generics.
    PErr err{"expected lifetime, type, or constant, found keyword `const`", start};
    if (check_const_arg()) {
        emit(err);
        GenericArg a;
        a.kind = GenericArg::Const;
        a.konst = parse_const_arg();
        return a;
    }
    Span after_kw_const = token.span;
    return recover_const_arg(after_kw_const, err);
}

bool Parser::assoc_op_from_token_is_op(const Token& t) {
    // AssocOp::from_token, minus Gt / Lt / Shr / Ge and the assignments.
    switch (t.kind) {
    case Tok::Plus: case Tok::Minus: case Tok::Star: case Tok::Slash: case Tok::Percent: case Tok::Caret:
    case Tok::And: case Tok::Or: case Tok::Shl: case Tok::Le: case Tok::EqEq: case Tok::Ne: case Tok::AndAnd:
    case Tok::OrOr: case Tok::DotDot: case Tok::DotDotEq:
        return true;
    case Tok::Ident: return tok_is_keyword(t, Kw::As);
    default: return false;
    }
}

GenericArg Parser::recover_const_arg(Span start, PErr err) {
    bool is_op_or_dot = assoc_op_from_token_is_op(token) || token.kind == Tok::Dot;
    bool was_op = prev_token.kind == Tok::Plus || prev_token.kind == Tok::Shr || prev_token.kind == Tok::Gt;
    if (!is_op_or_dot && !was_op) {
        // Only try to parse if it is an operator or `.` / we've parsed an operator.
        throw err;
    }
    ParserState snapshot = create_snapshot_for_diagnostic();
    if (is_op_or_dot) bump();
    try {
        AttrWrapper attrs = parse_outer_attributes();
        Expr* expr = parse_expr_res(R_CONST_EXPR, attrs).first;
        if (snapshot.token.kind == Tok::EqEq) {
            emit(err);
            GenericArg a;
            a.kind = GenericArg::Const;
            a.konst.value = mk_expr_err(start.to(expr->span));
            a.konst.direct = true;
            return a;
        } else if (snapshot.token.kind == Tok::Colon && expr->span.lo == snapshot.token.span.hi &&
                   expr->kind == ExprKind::Path) {
            // Find a mistake like "foo::var:A".
            emit(err);
            GenericArg a;
            a.kind = GenericArg::Type;
            a.ty = mk_ty(start.to(expr->span), TyKind::Err);
            return a;
        } else if (token.kind == Tok::Comma || should_end_const_arg(token.kind)) {
            // Avoid the following output by checking that we consumed a full const arg:
            // help: expressions must be enclosed in braces to be used as const generic
            //       arguments
            //    |
            // LL |     let sr: Vec<{ (u32, _, _) = vec![] };
            //    |                 ^                      ^
            return dummy_const_arg_needs_braces(err, start.to(expr->span));
        }
    } catch (PErr&) {
    }
    restore_snapshot(snapshot);
    throw err;
}

Expr* Parser::recover_unbraced_const_arg_that_can_begin_ty(const ParserState& snapshot) {
    // rustc parses the attributes on `self` and the expression on the snapshot.
    ParserState cur;
    try {
        AttrWrapper attrs = parse_outer_attributes();
        cur = st_;
        st_ = snapshot;
        Expr* expr = nullptr;
        try {
            expr = parse_expr_res(R_CONST_EXPR, attrs).first;
        } catch (...) {
            st_ = cur;
            throw;
        }
        if (token.kind == Tok::Comma || token.kind == Tok::Gt) return expr;  // restore_snapshot(snapshot)
        st_ = cur;
        return nullptr;
    } catch (PErr&) {
        return nullptr;
    }
}

GenericArg Parser::dummy_const_arg_needs_braces(PErr err, Span span) {
    emit(err);
    GenericArg a;
    a.kind = GenericArg::Const;
    a.konst.value = mk_expr_err(span);
    a.konst.direct = true;
    return a;
}

}  // namespace detail
}  // namespace rust
