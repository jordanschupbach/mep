// rust_parse_ty.cpp -- types and bounds (rustc_parse/src/parser/ty.rs)
// plus the type recovery of diagnostics.rs.
//
// Ported from rustc_parse/rustc_lexer (rust-lang/rust 1.97.1, MIT OR
// Apache-2.0).

#include "rust_parse_internal.h"

namespace rust {
namespace detail {

namespace {

bool can_recover_return_sign(RecoverReturnSign r, Tok k) {
    switch (r) {
    case RecoverReturnSign::Yes: return k == Tok::FatArrow || k == Tok::Colon;
    case RecoverReturnSign::OnlyFatArrow: return k == Tok::FatArrow;
    case RecoverReturnSign::No: return false;
    }
    return false;
}

const char* constness_str(BoundConstnessKind k) {
    switch (k) {
    case BoundConstnessKind::Never: return "";
    case BoundConstnessKind::Always: return "const";
    case BoundConstnessKind::Maybe: return "[const]";
    }
    return "";
}

const char* polarity_str(BoundPolarityKind k) {
    switch (k) {
    case BoundPolarityKind::Positive: return "";
    case BoundPolarityKind::Negative: return "!";
    case BoundPolarityKind::Maybe: return "?";
    }
    return "";
}

}  // namespace

bool Parser::can_begin_dyn_bound_in_edition_2015(const Token& t) const {
    if (tok_is_path_start(t)) {
        // In `dyn::x`, `dyn<X>` and `dyn<<X>::Y>`, `dyn` should (continue to) denote a regular path
        // segment for backward compatibility. We make an exception for `dyn(X)` which used to be
        // interpreted as a path with parenthesized generic arguments which was semantically
        // ill-formed and is now interpreted as a trait object type.
        return t.kind != Tok::PathSep && t.kind != Tok::Lt && t.kind != Tok::Shl;
    }
    return t.kind == Tok::OpenParen || t.kind == Tok::Question || tok_is_lifetime(t) || tok_is_keyword(t, Kw::For);
}

/// Parses a type.
Ty* Parser::parse_ty() {
    if (token.kind == Tok::DotDotDot) {
        // Make sure to recover from a C-variadic type nested in another type.
        Span span = token.span;
        bump();
        emit("unexpected `...`", span);
        return mk_ty(span, TyKind::Err);
    }
    return parse_ty_common(AllowPlus::Yes, AllowCVariadic::No, RecoverQPath::Yes, RecoverReturnSign::Yes, nullptr, true);
}

Ty* Parser::parse_ty_with_generics_recovery(const Generics& ty_params) {
    return parse_ty_common(AllowPlus::Yes, AllowCVariadic::No, RecoverQPath::Yes, RecoverReturnSign::Yes, &ty_params, true);
}

/// Parse a type suitable for a function or function pointer parameter.
/// The difference from `parse_ty` is that this version allows `...`
/// (`CVarArgs`) at the top level of the type.
Ty* Parser::parse_ty_for_param() {
    Ty* ty = parse_ty_common(AllowPlus::Yes, AllowCVariadic::Yes, RecoverQPath::Yes, RecoverReturnSign::Yes, nullptr, true);
    // Recover a trailing `= EXPR` if present.
    if (may_recover() && check_noexpect(Tok::Eq) && look_ahead(1, [this](const Token& t) { return tok_can_begin_expr(t); })) {
        ParserState snapshot = create_snapshot_for_diagnostic();
        bump();
        Span eq_span = prev_token.span;
        try {
            Expr* e = parse_expr();
            emit("parameter defaults are not supported", eq_span.to(e->span));
        } catch (PErr&) {
            restore_snapshot(snapshot);
        }
    }
    return ty;
}

/// Parses a type in restricted contexts where `+` is not permitted.
///
/// Example 1: `&'a TYPE`
///     `+` is prohibited to maintain operator priority (P(+) < P(&)).
/// Example 2: `value1 as TYPE + value2`
///     `+` is prohibited to avoid interactions with expression grammar.
Ty* Parser::parse_ty_no_plus() {
    return parse_ty_common(AllowPlus::No, AllowCVariadic::No, RecoverQPath::Yes, RecoverReturnSign::Yes, nullptr, true);
}

/// Parses a type following an `as` cast. Similar to `parse_ty_no_plus`, but signaling origin
/// for better diagnostics involving `?`.
Ty* Parser::parse_as_cast_ty() {
    return parse_ty_common(AllowPlus::No, AllowCVariadic::No, RecoverQPath::Yes, RecoverReturnSign::Yes, nullptr, false);
}

Ty* Parser::parse_ty_no_question_mark_recover() {
    return parse_ty_common(AllowPlus::Yes, AllowCVariadic::No, RecoverQPath::Yes, RecoverReturnSign::Yes, nullptr, false);
}

/// Parse a type without recovering `:` as `->` to avoid breaking code such
/// as `where fn() : for<'a>`.
Ty* Parser::parse_ty_for_where_clause() {
    return parse_ty_common(AllowPlus::Yes, AllowCVariadic::No, RecoverQPath::Yes, RecoverReturnSign::OnlyFatArrow,
                           nullptr, true);
}

/// Parses an optional return type `[ -> TY ]` in a function declaration.
FnRetTy Parser::parse_ret_ty(AllowPlus allow_plus, RecoverQPath recover_qpath, RecoverReturnSign recover_return_sign) {
    FnRetTy r;
    if (eat(EXP(RArrow))) {
        // FIXME(Centril): Can we unconditionally `allow_plus`?
        r.ty = parse_ty_common(allow_plus, AllowCVariadic::No, recover_qpath, recover_return_sign, nullptr, true);
        r.span = r.ty->span;
    } else if (can_recover_return_sign(recover_return_sign, token.kind)) {
        // Don't `eat` to prevent `=>` from being added as an expected token which isn't
        // actually expected and could only confuse users
        bump();
        emit("return types are denoted using `->`", prev_token.span);
        r.ty = parse_ty_common(allow_plus, AllowCVariadic::No, recover_qpath, recover_return_sign, nullptr, true);
        r.span = r.ty->span;
    } else {
        r.span = prev_token.span.shrink_to_hi();
    }
    return r;
}

Ty* Parser::parse_ty_common(AllowPlus allow_plus, AllowCVariadic allow_c_variadic, RecoverQPath recover_qpath,
                            RecoverReturnSign recover_return_sign, const Generics* ty_generics,
                            bool recover_question_mark) {
    DepthGuard guard(*this);
    bool allow_qpath_recovery = recover_qpath == RecoverQPath::Yes;
    // (maybe_recover_from_interpolated_ty_qpath: macro expansion only.)

    if (token.kind == Tok::Pound && look_ahead(1, [](const Token& t) { return t.kind == Tok::OpenBracket; })) {
        AttrWrapper w = parse_outer_attributes();
        Span attr_span = w.attrs.empty() ? token.span : w.attrs.front().span.to(w.attrs.back().span);
        try {
            (void)parse_ty();
            emit("attributes cannot be applied to types", attr_span);
        } catch (PErr&) {
            emit("attributes cannot be applied here", attr_span);
        }
        return mk_ty(attr_span, TyKind::Err);
    }

    Span lo = token.span;
    bool impl_dyn_multi = false;
    Ty* kind = nullptr;
    if (check(EXP(OpenParen))) {
        kind = parse_ty_tuple_or_parens(lo, allow_plus);
    } else if (eat(EXP(Bang))) {
        // Never type `!`
        kind = mk_ty(lo, TyKind::Never);
    } else if (eat(EXP(Star))) {
        kind = parse_ty_ptr();
    } else if (eat(EXP(OpenBracket))) {
        kind = parse_array_or_slice_ty();
    } else if (check(EXP(And)) || check(EXP(AndAnd))) {
        // Reference
        expect_and();
        kind = parse_borrowed_pointee();
    } else if (eat_keyword_noexpect(Kw::Typeof)) {
        kind = parse_typeof_ty(lo);
    } else if (is_builtin()) {
        kind = parse_builtin_ty();
    } else if (eat_keyword(KW(Underscore))) {
        // A type to be inferred `_`
        kind = mk_ty(lo, TyKind::Infer);
    } else if (check_fn_front_matter(false, Case::Sensitive)) {
        // Function pointer type
        kind = parse_ty_fn_ptr(lo, {}, OptSpan(), recover_return_sign);
    } else if (check_keyword(KW(For))) {
        // Function pointer type or bound list (trait object type) starting with a poly-trait.
        //   `for<'lt> [unsafe] [extern "ABI"] fn (&'lt S) -> T`
        //   `for<'lt> Trait1<'lt> + Trait2 + 'a`
        std::vector<GenericParam*> bound_vars = parse_higher_ranked_binder().first;
        if (check_fn_front_matter(false, Case::Sensitive)) {
            kind = parse_ty_fn_ptr(lo, std::move(bound_vars), OptSpan(prev_token.span.shrink_to_lo()),
                                   recover_return_sign);
        } else {
            // Try to recover `for<'a> dyn Trait` or `for<'a> impl Trait`.
            if (may_recover() && (eat_keyword_noexpect(Kw::Impl) || eat_keyword_noexpect(Kw::Dyn))) {
                Ident kw = tok_ident(prev_token);
                bool is_dyn = prev_token.kw == Kw::Dyn;
                Path* path = parse_path(PathStyle::Type);
                bool parse_plus = allow_plus == AllowPlus::Yes && check_plus();
                kind = parse_remaining_bounds_path(std::move(bound_vars), path, lo, parse_plus, false);
                emit("`for<...>` expected after `" + std::string(kw.name) + "`, not before", kw.span);
                if (is_dyn) {
                    kind->dyn_syntax = true;
                } else {
                    kind->kind = TyKind::ImplTrait;
                }
            } else {
                Path* path = parse_path(PathStyle::Type);
                bool parse_plus = allow_plus == AllowPlus::Yes && check_plus();
                kind = parse_remaining_bounds_path(std::move(bound_vars), path, lo, parse_plus, false);
            }
        }
    } else if (eat_keyword(KW(Impl))) {
        kind = parse_impl_ty(impl_dyn_multi);
    } else if (is_explicit_dyn_type()) {
        kind = parse_dyn_ty(impl_dyn_multi);
    } else if (eat_lt()) {
        // Qualified path
        std::pair<QSelf*, Path*> qp = parse_qpath(PathStyle::Type);
        kind = mk_ty(lo, TyKind::Path);
        kind->qself = qp.first;
        kind->path = qp.second;
    } else if ((tok_is_keyword(token, Kw::Const) || tok_is_keyword(token, Kw::Mut)) &&
               look_ahead(1, [](const Token& t) { return t.kind == Tok::Star; })) {
        kind = parse_ty_c_style_pointer();
    } else if (check_path()) {
        kind = parse_path_start_ty(lo, allow_plus, ty_generics);
    } else if (can_begin_bound()) {
        kind = parse_bare_trait_object(lo, allow_plus);
    } else if (eat(EXP(DotDotDot))) {
        if (allow_c_variadic == AllowCVariadic::Yes) {
            kind = mk_ty(lo, TyKind::CVarArgs);
        } else {
            // FIXME(c_variadic): Should we just allow `...` syntactically
            // anywhere in a type and use semantic restrictions instead?
            // NOTE: This may regress certain MBE calls if done incorrectly.
            emit("C-variadic type `...` may not be nested inside another type", lo);
            kind = mk_ty(lo, TyKind::Err);
        }
    } else if (check_keyword(KW(Unsafe)) && look_ahead(1, [](const Token& t) { return t.kind == Tok::Lt; })) {
        kind = parse_unsafe_binder_ty();
    } else {
        fail("expected type, found " + token_descr(token), lo);
    }

    Ty* ty = kind;
    ty->span = lo.to(prev_token.span);

    // Try to recover from use of `+` with incorrect priority.
    if (allow_plus == AllowPlus::Yes)
        maybe_recover_from_bad_type_plus(ty);
    else
        maybe_report_ambiguous_plus(impl_dyn_multi, ty);
    if (recover_question_mark) ty = maybe_recover_from_question_mark(ty);
    return allow_qpath_recovery ? maybe_recover_from_bad_qpath_ty(ty) : ty;
}

Ty* Parser::parse_unsafe_binder_ty() {
    (void)eat_keyword(KW(Unsafe));
    expect_lt();
    std::vector<GenericParam*> generic_params = parse_generic_params();
    expect_gt();
    Ty* inner_ty = parse_ty();
    Ty* k = mk_ty(Span(), TyKind::UnsafeBinder);
    k->generic_params = std::move(generic_params);
    k->inner = inner_ty;
    return k;
}

/// Parses either:
/// - `(TYPE)`, a parenthesized type.
/// - `(TYPE,)`, a tuple with a single field of type TYPE.
Ty* Parser::parse_ty_tuple_or_parens(Span lo, AllowPlus allow_plus) {
    bool trailing_plus = false;
    Trailing trailing = Trailing::No;
    std::vector<Ty*> ts = parse_paren_comma_seq<Ty*>(
        [this, &trailing_plus]() {
            Ty* ty = parse_ty();
            trailing_plus = prev_token.kind == Tok::Plus;
            return ty;
        },
        &trailing);

    if (ts.size() == 1 && trailing == Trailing::No) {
        Ty* ty = ts[0];
        bool maybe_bounds = allow_plus == AllowPlus::Yes && tok_is_like_plus(token);
        if (ty->kind == TyKind::Path && ty->qself == nullptr && maybe_bounds)
            return parse_remaining_bounds_path({}, ty->path, lo, true, true);
        if (ty->kind == TyKind::TraitObject && !ty->dyn_syntax && maybe_bounds && ty->bounds.size() == 1 &&
            !trailing_plus)
            return parse_remaining_bounds(ty->bounds, true);
        Ty* k = mk_ty(Span(), TyKind::Paren);
        k->inner = ty;
        return k;
    }
    Ty* k = mk_ty(Span(), TyKind::Tup);
    k->elems = std::move(ts);
    return k;
}

Ty* Parser::parse_bare_trait_object(Span lo, AllowPlus allow_plus) {
    // A lifetime only begins a bare trait object type if it is followed by `+`!
    if (tok_is_lifetime(token) && !look_ahead(1, [this](const Token& t) { return tok_is_like_plus(t); })) {
        // In Rust 2021 and beyond, we assume that the user didn't intend to write a bare trait
        // object type with a leading lifetime bound since that seems very unlikely given the
        // fact that `dyn`-less trait objects are *semantic* errors in Rust 2021 (non-fatal in
        // Rust 2015-2018).
        if (edition_ >= Edition::E2021) {
            Lifetime lt = expect_lifetime();
            PErr err{"expected type, found lifetime", lo};
            if (Ty* ref_ty = maybe_recover_ref_ty_no_leading_ampersand(lt, lo, err)) return ref_ty;
            emit(err);
            return mk_ty(Span(), TyKind::Err);
        }
        emit("lifetimes must be followed by `+` to form a trait object type", lo);
    }
    Ty* k = mk_ty(Span(), TyKind::TraitObject);
    k->bounds = parse_generic_bounds_common(allow_plus);
    return k;
}

Ty* Parser::maybe_recover_ref_ty_no_leading_ampersand(Lifetime lt, Span lo, const PErr& err) {
    (void)lo;
    if (!may_recover()) return nullptr;
    ParserState snapshot = create_snapshot_for_diagnostic();
    Mutability mutbl = parse_mutability();
    try {
        Ty* ty = parse_ty_no_plus();
        emit(err);
        Ty* k = mk_ty(Span(), TyKind::Ref);
        k->has_lifetime = true;
        k->lifetime = lt;
        k->mutbl = mutbl;
        k->inner = ty;
        return k;
    } catch (PErr&) {
        restore_snapshot(snapshot);
        return nullptr;
    }
}

Ty* Parser::parse_remaining_bounds_path(std::vector<GenericParam*> generic_params, Path* path, Span lo, bool parse_plus,
                                        bool parens) {
    GenericBound b;
    b.kind = GenericBound::Trait;
    b.trait.bound_generic_params = std::move(generic_params);
    b.trait.trait_ref = path;
    b.trait.span = lo.to(prev_token.span);
    b.trait.parens = parens;
    GenericBounds bounds{b};
    return parse_remaining_bounds(std::move(bounds), parse_plus);
}

/// Parse the remainder of a bare trait object type given an already parsed list.
Ty* Parser::parse_remaining_bounds(GenericBounds bounds, bool plus) {
    if (plus) {
        (void)eat_plus();  // `+`, or `+=` gets split and `+` is discarded
        GenericBounds more = parse_generic_bounds();
        bounds.insert(bounds.end(), more.begin(), more.end());
    }
    Ty* k = mk_ty(Span(), TyKind::TraitObject);
    k->bounds = std::move(bounds);
    return k;
}

/// Parses a raw pointer with a C-style typo
Ty* Parser::parse_ty_c_style_pointer() {
    Span kw_span = token.span;
    std::optional<Mutability> mutbl = parse_mut_or_const();
    if (mutbl && eat(EXP(Star))) {
        const char* mutability = *mutbl == Mutability::Not ? "const" : "mut";
        Ty* ty = parse_ty_no_question_mark_recover();
        emit(std::string("raw pointer types must be written as `*") + mutability + " T`", kw_span);
        Ty* k = mk_ty(Span(), TyKind::Ptr);
        k->mutbl = *mutbl;
        k->inner = ty;
        return k;
    }
    // This is unreachable because we always get into if above and return from it
    fail("this could never happen", kw_span);
}

/// Parses a raw pointer type: `*[const | mut] $type`.
Ty* Parser::parse_ty_ptr() {
    std::optional<Mutability> m = parse_mut_or_const();
    Mutability mutbl = Mutability::Not;
    if (m) {
        mutbl = *m;
    } else {
        emit("expected `mut` or `const` keyword in raw pointer type", prev_token.span);
    }
    Ty* ty = parse_ty_no_plus();
    Ty* k = mk_ty(Span(), TyKind::Ptr);
    k->mutbl = mutbl;
    k->inner = ty;
    return k;
}

/// Parses an array (`[TYPE; EXPR]`) or slice (`[TYPE]`) type.
/// The opening `[` bracket is already eaten.
Ty* Parser::parse_array_or_slice_ty() {
    Ty* elt_ty = nullptr;
    try {
        elt_ty = parse_ty();
    } catch (PErr& err) {
        bool a = look_ahead(1, [](const Token& t) { return t.kind == Tok::CloseBracket; });
        bool b = look_ahead(1, [](const Token& t) { return t.kind == Tok::Semi; });
        if (!(a || b)) throw;
        // Recover from `[LIT; EXPR]` and `[LIT]`
        bump();
        emit(err);
        elt_ty = mk_ty(prev_token.span, TyKind::Err);
    }

    if (eat(EXP(Semi))) {
        AnonConst length = parse_expr_anon_const(true);
        try {
            expect(EXP(CloseBracket));
        } catch (PErr& e) {
            // Try to recover from `X<Y, ...>` when `X::<Y, ...>` works
            check_mistyped_turbofish_with_multiple_type_params(e, length.value);
            expect(EXP(CloseBracket));
        }
        Ty* k = mk_ty(Span(), TyKind::Array);
        k->inner = elt_ty;
        k->len = length;
        return k;
    }
    if (eat(EXP(CloseBracket))) {
        Ty* k = mk_ty(Span(), TyKind::Slice);
        k->inner = elt_ty;
        return k;
    }
    return maybe_recover_array_ty_without_semi(elt_ty);
}

Ty* Parser::maybe_recover_array_ty_without_semi(Ty* elt_ty) {
    Span span = token.span;
    PErr err{"expected `;` or `]`, found " + token_descr(token), span};
    if (!may_recover()) throw err;

    ParserState snapshot = create_snapshot_for_diagnostic();
    // Consume common erroneous separators.
    if (!eat(EXP(Comma)))
        if (!eat(EXP(Colon))) (void)eat(EXP(Star));
    AnonConst length;
    try {
        length = parse_expr_anon_const(true);
    } catch (PErr&) {
        restore_snapshot(snapshot);
        throw err;
    }
    try {
        expect(EXP(CloseBracket));
    } catch (PErr&) {
        restore_snapshot(snapshot);
        throw err;
    }
    emit(err);
    Ty* k = mk_ty(Span(), TyKind::Array);
    k->inner = elt_ty;
    k->len = length;
    return k;
}

Ty* Parser::parse_borrowed_pointee() {
    Span and_span = prev_token.span;
    bool has_lifetime = false;
    Lifetime opt_lifetime;
    if (check_lifetime()) {
        opt_lifetime = expect_lifetime();
        has_lifetime = true;
    }
    std::pair<Pinnedness, Mutability> pm = parse_pin_and_mut();
    Mutability mutbl = pm.second;
    if (tok_is_lifetime(token) && mutbl == Mutability::Mut && !has_lifetime) {
        // A lifetime is invalid here: it would be part of a bare trait bound, which requires
        // it to be followed by a plus, but we disallow plus in the pointee type.
        // So we can handle this case as an error here, and suggest `'a mut`.
        // If there *is* a plus next though, handling the error later provides better suggestions
        // (like adding parentheses)
        if (!look_ahead(1, [this](const Token& t) { return tok_is_like_plus(t); })) {
            Span lifetime_span = token.span;
            emit("lifetime must precede `mut`", and_span.to(lifetime_span));
            opt_lifetime = expect_lifetime();
            has_lifetime = true;
        }
    } else if (tok_is_keyword(token, Kw::Dyn) && mutbl == Mutability::Not &&
               look_ahead(1, [this](const Token& t) { return tok_is_keyword(t, Kw::Mut); })) {
        // We have `&dyn mut ...`, which is invalid and should be `&mut dyn ...`.
        Span span = and_span.to(look_ahead_tok(1).span);
        emit("`mut` must precede `dyn`", span);
        // Recovery
        mutbl = Mutability::Mut;
        Token dyn_tok = token;
        Spacing dyn_sp = st_.token_spacing;
        bump();
        bump_with(dyn_tok, dyn_sp);
    }
    Ty* ty = parse_ty_no_plus();
    if (token.kind == Tok::Dot && look_ahead(1, [](const Token& t) { return t.kind == Tok::OpenBrace; })) {
        // & [mut] (pin) Type.{fields} -- view types (unstable).
        bump();
        (void)parse_delim_comma_seq<Ident>(EXP(OpenBrace), EXP(CloseBrace), [this]() { return parse_ident(); });
    }
    Ty* k = mk_ty(Span(), pm.first == Pinnedness::Pinned ? TyKind::PinnedRef : TyKind::Ref);
    k->has_lifetime = has_lifetime;
    k->lifetime = opt_lifetime;
    k->mutbl = mutbl;
    k->inner = ty;
    return k;
}

/// Parses `pin` and `mut` annotations on references, patterns, or borrow modifiers.
///
/// It must be either `pin const`, `pin mut`, `mut`, or nothing (immutable).
std::pair<Pinnedness, Mutability> Parser::parse_pin_and_mut() {
    if (tok_is_ident_named(token, "pin") && look_ahead(1, [this](const Token& t) { return tok_is_mutability(t); })) {
        if (!eat_keyword(SYM(Pin))) bump();
        std::optional<Mutability> m = parse_mut_or_const();
        return {Pinnedness::Pinned, m ? *m : Mutability::Not};
    }
    return {Pinnedness::Not, parse_mutability()};
}

// Parses the `typeof(EXPR)`.
// To avoid ambiguity, the type is surrounded by parentheses.
Ty* Parser::parse_typeof_ty(Span lo) {
    expect(EXP(OpenParen));
    (void)parse_expr_anon_const(false);
    expect(EXP(CloseParen));
    Span span = lo.to(prev_token.span);
    emit("`typeof` is a reserved keyword but unimplemented", span);
    return mk_ty(Span(), TyKind::Err);
}

Ty* Parser::parse_builtin_ty() {
    Ty* res = nullptr;
    parse_builtin([this, &res](Span lo, Ident ident) {
        if (ident.name == "field_of") {
            res = parse_ty_field_of(lo);
            return true;
        }
        return false;
    });
    return res;
}

/// Built-in macro for `field_of!` expressions.
Ty* Parser::parse_ty_field_of(Span lo) {
    (void)lo;
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

    if (fields.empty()) fail("`field_of!` expects dot-separated field and variant names", token.span);
    if (fields.size() > 2) fail("`field_of!` only supports a single field or a variant with a field", fields[0].span);
    Ty* k = mk_ty(Span(), TyKind::FieldOf);
    k->inner = container;
    if (fields.size() == 1) {
        k->field = fields[0];
    } else {
        k->has_variant = true;
        k->variant = fields[0];
        k->field = fields[1];
    }
    return k;
}

/// Parses a function pointer type (`TyKind::FnPtr`).
/// ```ignore (illustrative)
///    [unsafe] [extern "ABI"] fn (S) -> T
/// //  ^~~~~^          ^~~~^     ^~^    ^
/// //    |               |        |     |
/// //    |               |        |   Return type
/// // Function Style    ABI  Parameter types
/// ```
/// We actually parse `FnHeader FnDecl`, but we error on `const` and `async` qualifiers.
Ty* Parser::parse_ty_fn_ptr(Span lo, std::vector<GenericParam*> params, OptSpan param_insertion_point,
                            RecoverReturnSign recover_return_sign) {
    Visibility inherited_vis;
    Span span_start = token.span;
    FnHeader header = parse_fn_front_matter(inherited_vis, Case::Sensitive, FrontMatterParsingMode::FunctionPtrType);
    if (may_recover() && token.kind == Tok::Lt) recover_fn_ptr_with_generics(lo, params, param_insertion_point);
    FnParseMode mode;
    mode.req_name = ReqName::Never;
    mode.context = FnContext::Free;
    mode.req_body = false;
    FnDecl* decl = parse_fn_decl(mode, AllowPlus::No, recover_return_sign);
    Span decl_span = span_start.to(prev_token.span);
    FnPtrTy* f = mk<FnPtrTy>();
    f->ext = header.ext;
    f->safety = header.safety;
    f->generic_params = std::move(params);
    f->decl = decl;
    f->decl_span = decl_span;
    Ty* k = mk_ty(Span(), TyKind::FnPtr);
    k->fn_ptr = f;
    return k;
}

/// Recover from function pointer types with a generic parameter list (e.g. `fn<'a>(&'a str)`).
void Parser::recover_fn_ptr_with_generics(Span lo, std::vector<GenericParam*>& params, OptSpan param_insertion_point) {
    (void)lo;
    (void)param_insertion_point;
    Generics generics = parse_generics();
    emit("function pointer types may not have generic parameters", generics.span);
    for (GenericParam* p : generics.params)
        if (p->kind == GenericParam::Lifetime) params.push_back(p);
}

/// Parses an `impl B0 + ... + Bn` type.
Ty* Parser::parse_impl_ty(bool& impl_dyn_multi) {
    if (tok_is_lifetime(token)) {
        const Token& t = look_ahead_tok(1);
        if (t.kind == Tok::Ident) {
            // Ident's Display: `r#` for raw identifiers and for reserved names.
            Ident id = tok_ident(t);
            std::string name = (t.raw || ident_is_raw_guess(id) ? "r#" : "") + std::string(t.sym);
            emit("expected `+` between lifetime and " + name, token.span);
        }
    }
    // Always parse bounds greedily for better error recovery.
    GenericBounds bounds = parse_generic_bounds();
    impl_dyn_multi = bounds.size() > 1 || prev_token.kind == Tok::Plus;
    Ty* k = mk_ty(Span(), TyKind::ImplTrait);
    k->bounds = std::move(bounds);
    return k;
}

GenericBound Parser::parse_use_bound(Span lo, bool parens) {
    expect_lt();
    std::vector<PreciseCapturingArg> args = parse_seq_to_before_tokens<PreciseCapturingArg>(
        {EXP(Gt)}, {Tok::Ge, Tok::Shr, Tok::Shr}, SeqSep::trailing_allowed(EXP(Comma)), [this]() {
            PreciseCapturingArg a;
            if (check_keyword(KW(SelfUpper))) {
                bump();
                Ident id = tok_ident(prev_token);
                a.path = mk_path(id.span);
                PathSegment s;
                s.ident = id;
                a.path->segments.push_back(s);
            } else if (check_ident()) {
                Ident id = parse_ident();
                a.path = mk_path(id.span);
                PathSegment s;
                s.ident = id;
                a.path->segments.push_back(s);
            } else if (check_lifetime()) {
                a.is_lifetime = true;
                a.lifetime = expect_lifetime();
            } else {
                unexpected_any();
            }
            return a;
        });
    expect_gt();

    if (parens) {
        expect(EXP(CloseParen));
        report_parenthesized_bound(lo, prev_token.span, "precise capturing lists");
    }
    GenericBound b;
    b.kind = GenericBound::Use;
    b.use_args = std::move(args);
    b.use_span = lo.to(prev_token.span);
    return b;
}

/// Is a `dyn B0 + ... + Bn` type allowed here?
bool Parser::is_explicit_dyn_type() {
    return check_keyword(KW(Dyn)) &&
           (edition_ >= Edition::E2018 ||
            look_ahead(1, [this](const Token& t) { return can_begin_dyn_bound_in_edition_2015(t); }));
}

/// Parses a `dyn B0 + ... + Bn` type.
///
/// Note that this does *not* parse bare trait objects.
Ty* Parser::parse_dyn_ty(bool& impl_dyn_multi) {
    bump();  // `dyn`
    // Always parse bounds greedily for better error recovery.
    GenericBounds bounds = parse_generic_bounds();
    impl_dyn_multi = bounds.size() > 1 || prev_token.kind == Tok::Plus;
    Ty* k = mk_ty(Span(), TyKind::TraitObject);
    k->bounds = std::move(bounds);
    k->dyn_syntax = true;
    return k;
}

/// Parses a type starting with a path.
///
/// This can be:
/// 1. a type macro, `mac!(...)`,
/// 2. a bare trait object, `B0 + ... + Bn`,
/// 3. or a path, `path::to::MyType`.
Ty* Parser::parse_path_start_ty(Span lo, AllowPlus allow_plus, const Generics* ty_generics) {
    // Simple path
    Path* path = parse_path_inner(PathStyle::Type, ty_generics);
    if (eat(EXP(Bang))) {
        // Macro invocation in type position
        MacCall* mac = mk<MacCall>();
        mac->path = path;
        mac->args = parse_delim_args();
        Ty* k = mk_ty(Span(), TyKind::MacCall);
        k->mac = mac;
        return k;
    }
    if (allow_plus == AllowPlus::Yes && check_plus()) {
        // `Trait1 + Trait2 + 'a`
        return parse_remaining_bounds_path({}, path, lo, true, false);
    }
    // Just a type path.
    Ty* k = mk_ty(Span(), TyKind::Path);
    k->path = path;
    return k;
}

GenericBounds Parser::parse_generic_bounds() { return parse_generic_bounds_common(AllowPlus::Yes); }

/// Parses bounds of a type parameter `BOUND + BOUND + ...`, possibly with trailing `+`.
///
/// See `parse_generic_bound` for the `BOUND` grammar.
GenericBounds Parser::parse_generic_bounds_common(AllowPlus allow_plus) {
    GenericBounds bounds;

    // In addition to looping while we find generic bounds:
    // We continue even if we find a keyword. This is necessary for error recovery on,
    // for example, `impl fn()`. The only keyword that can go after generic bounds is
    // `where`, so stop if it's it.
    // We also continue if we find types (not traits), again for error recovery.
    for (;;) {
        bool go = can_begin_bound();
        if (!go && may_recover())
            go = tok_can_begin_type(token) || (tok_is_reserved_ident(token) && !tok_is_keyword(token, Kw::Where));
        if (!go) break;
        if (tok_is_keyword(token, Kw::Dyn)) {
            // Account for `&dyn Trait + dyn Other`.
            bump();
            emit("invalid `dyn` keyword", prev_token.span);
        }
        bounds.push_back(parse_generic_bound());
        if (allow_plus == AllowPlus::No || !eat_plus()) break;
    }
    return bounds;
}

/// Can the current token begin a bound?
bool Parser::can_begin_bound() {
    return check_path() || check_lifetime() || check(EXP(Bang)) || check(EXP(Question)) || check(EXP(Tilde)) ||
           check_keyword(KW(For)) || check(EXP(OpenParen)) || can_begin_maybe_const_bound() ||
           check_keyword(KW(Const)) || check_keyword(KW(Async)) || check_keyword(KW(Use));
}

bool Parser::can_begin_maybe_const_bound() {
    return check(EXP(OpenBracket)) && look_ahead(1, [this](const Token& t) { return tok_is_keyword(t, Kw::Const); }) &&
           look_ahead(2, [](const Token& t) { return t.kind == Tok::CloseBracket; });
}

/// Parse a bound.
///
/// ```ebnf
/// BOUND = TY_BOUND | LT_BOUND
/// ```
GenericBound Parser::parse_generic_bound() {
    Token leading_token = prev_token;
    Span lo = token.span;

    // We only admit parenthesized *trait* bounds. However, we want to gracefully recover from
    // other kinds of parenthesized bounds, so parse the opening parenthesis *here*.
    //
    // In the future we might want to lift this syntactic restriction and
    // introduce "`GenericBound::Paren(Box<GenericBound>)`".
    bool parens = eat(EXP(OpenParen));

    if (tok_is_lifetime(token)) return parse_lifetime_bound(lo, parens);
    if (eat_keyword(KW(Use))) return parse_use_bound(lo, parens);
    return parse_trait_bound(lo, parens, leading_token);
}

/// Parse a lifetime-bound aka outlives-bound.
///
/// ```ebnf
/// LT_BOUND = LIFETIME
/// ```
GenericBound Parser::parse_lifetime_bound(Span lo, bool parens) {
    Lifetime lt = expect_lifetime();
    if (parens) {
        expect(EXP(CloseParen));
        report_parenthesized_bound(lo, prev_token.span, "lifetime bounds");
    }
    GenericBound b;
    b.kind = GenericBound::Outlives;
    b.lifetime = lt;
    return b;
}

void Parser::report_parenthesized_bound(Span lo, Span hi, const char* kind) {
    emit(std::string(kind) + " may not be parenthesized", lo.to(hi));
}

/// Emits an error if any trait bound modifiers were present.
void Parser::error_lt_bound_with_modifiers(const TraitBoundModifiers& modifiers, OptSpan binder_span) {
    auto modifier_lifetime = [this](Span sp, const std::string& m) {
        emit("`" + m + "` may only modify trait bounds, not lifetime bounds", sp);
    };
    if (modifiers.constness != BoundConstnessKind::Never) {
        modifier_lifetime(modifiers.constness_span, constness_str(modifiers.constness));
        return;
    }
    if (modifiers.polarity != BoundPolarityKind::Positive) {
        modifier_lifetime(modifiers.polarity_span, polarity_str(modifiers.polarity));
        return;
    }
    if (modifiers.async_) {
        modifier_lifetime(modifiers.async_span, "async");
        return;
    }
    if (binder_span.has) modifier_lifetime(binder_span.span, "for<...>");
}

/// Parses the modifiers that may precede a trait in a bound, e.g. `?Trait` or `[const] Trait`.
///
/// If no modifiers are present, this does not consume any tokens.
///
/// ```ebnf
/// CONSTNESS = [["["] "const" ["]"]]
/// ASYNCNESS = ["async"]
/// POLARITY = ["?" | "!"]
/// ```
///
/// See `parse_trait_bound` for more context.
TraitBoundModifiers Parser::parse_trait_bound_modifiers() {
    TraitBoundModifiers m;
    Span modifier_lo = token.span;
    parse_bound_constness(m);

    if (edition_ >= Edition::E2018 && eat_keyword(KW(Async))) {
        m.async_ = true;
        m.async_span = prev_token.span;
    } else if (may_recover() && edition_ == Edition::E2015 && is_kw_followed_by_ident(Kw::Async)) {
        bump();  // eat `async`
        emit("`async` trait bounds are only allowed in Rust 2018 or later", prev_token.span);
        m.async_ = true;
        m.async_span = prev_token.span;
    }
    Span modifier_hi = prev_token.span;
    (void)modifier_lo;
    (void)modifier_hi;

    if (eat(EXP(Question))) {
        m.polarity = BoundPolarityKind::Maybe;
        m.polarity_span = prev_token.span;
    } else if (eat(EXP(Bang))) {
        m.polarity = BoundPolarityKind::Negative;
        m.polarity_span = prev_token.span;
    }

    // Enforce the mutual-exclusivity of `const`/`async` and `?`/`!`.
    if (m.polarity != BoundPolarityKind::Positive && (m.async_ || m.constness != BoundConstnessKind::Never)) {
        std::string constness = constness_str(m.constness);
        std::string asyncness = m.async_ ? "async" : "";
        std::string glue = (!constness.empty() && !asyncness.empty()) ? " " : "";
        emit("`" + constness + glue + asyncness + "` trait not allowed with `" + polarity_str(m.polarity) +
                 "` trait polarity modifier",
             m.polarity_span);
    }
    return m;
}

void Parser::parse_bound_constness(TraitBoundModifiers& m) {
    // FIXME(const_trait_impl): remove `~const` parser support once bootstrap has the new syntax
    // in rustfmt
    if (eat(EXP(Tilde))) {
        Span tilde = prev_token.span;
        expect_keyword(KW(Const));
        m.constness = BoundConstnessKind::Maybe;
        m.constness_span = tilde.to(prev_token.span);
    } else if (can_begin_maybe_const_bound()) {
        Span start = token.span;
        bump();
        expect_keyword(KW(Const));
        bump();
        m.constness = BoundConstnessKind::Maybe;
        m.constness_span = start.to(prev_token.span);
    } else if (eat_keyword(KW(Const))) {
        m.constness = BoundConstnessKind::Always;
        m.constness_span = prev_token.span;
    }
}

/// Parses a type bound according to:
/// ```ebnf
/// TY_BOUND = TY_BOUND_NOPAREN | (TY_BOUND_NOPAREN)
/// TY_BOUND_NOPAREN = [for<GENERIC_PARAMS> CONSTNESS ASYNCNESS | POLARITY] SIMPLE_PATH
/// ```
///
/// For example, this grammar accepts `for<'a: 'b> [const] ?m::Trait<'a>`.
GenericBound Parser::parse_trait_bound(Span lo, bool parens, const Token& leading_token) {
    std::pair<std::vector<GenericParam*>, OptSpan> hb = parse_higher_ranked_binder();
    std::vector<GenericParam*> bound_vars = std::move(hb.first);
    OptSpan binder_span = hb.second;

    Span modifiers_lo = token.span;
    TraitBoundModifiers modifiers = parse_trait_bound_modifiers();
    Span modifiers_span = modifiers_lo.to(prev_token.span);
    (void)modifiers_span;

    if (binder_span.has && modifiers.polarity != BoundPolarityKind::Positive) {
        emit(std::string("`for<...>` binder not allowed with `") + polarity_str(modifiers.polarity) +
                 "` trait polarity modifier",
             modifiers.polarity_span);
    }

    // Recover erroneous lifetime bound with modifiers or binder.
    // e.g. `T: for<'a> 'a` or `T: [const] 'a`.
    if (tok_is_lifetime(token)) {
        error_lt_bound_with_modifiers(modifiers, binder_span);
        return parse_lifetime_bound(lo, parens);
    }

    std::pair<std::vector<GenericParam*>, OptSpan> more = parse_higher_ranked_binder();
    if (more.second.has) {
        bound_vars.insert(bound_vars.end(), more.first.begin(), more.first.end());
        emit("`for<...>` binder should be placed before trait bound modifiers", more.second.span);
    }

    Path* path = nullptr;
    if (tok_is_keyword(token, Kw::Fn) && look_ahead(1, [](const Token& t) { return t.kind == Tok::OpenParen; }))
        path = recover_path_from_fn();
    if (path) {
    } else if (!tok_is_path_start(token) && tok_can_begin_type(token)) {
        Ty* ty = parse_ty_no_plus();
        // Instead of finding a path (a trait), we found a type.
        PErr err{"expected a trait, found type", ty->span};
        // If we can recover, try to extract a path from the type. Note
        // that we do not use the try operator when parsing the type because
        // if it fails then we get a parser error which we don't want (we're trying
        // to recover from errors, not make more).
        if (!may_recover()) throw err;
        if (ty->kind == TyKind::Ptr || ty->kind == TyKind::Ref) {
            const Ty* peeled = ty;
            while ((peeled->kind == TyKind::Ref || peeled->kind == TyKind::Ptr) && peeled->inner) peeled = peeled->inner;
            if (peeled->kind != TyKind::Path) throw err;
            path = peeled->path;
        } else if (ty->kind == TyKind::ImplTrait && !ty->bounds.empty() && ty->bounds[0].kind == GenericBound::Trait) {
            path = ty->bounds[0].trait.trait_ref;
        } else {
            throw err;
        }
        emit(err);
    } else {
        path = parse_path(PathStyle::Type);
    }

    if (may_recover() && token.kind == Tok::OpenParen) recover_fn_trait_with_lifetime_params(path, bound_vars);

    if (parens) {
        // Someone has written something like `&dyn (Trait + Other)`. The correct code
        // would be `&(dyn Trait + Other)`
        if (tok_is_like_plus(token) && tok_is_keyword(leading_token, Kw::Dyn)) {
            (void)parse_remaining_bounds({}, true);
            expect(EXP(CloseParen));
            emit("incorrect parentheses around trait bounds", lo);
        } else {
            expect(EXP(CloseParen));
        }
    }

    GenericBound b;
    b.kind = GenericBound::Trait;
    b.trait.bound_generic_params = std::move(bound_vars);
    b.trait.modifiers = modifiers;
    b.trait.trait_ref = path;
    b.trait.span = lo.to(prev_token.span);
    b.trait.parens = parens;
    return b;
}

/// Recover from `Fn`-family traits (Fn, FnMut, FnOnce) with lifetime arguments
/// (e.g. `FnOnce<'a>(&'a str) -> bool`). Up to generic arguments have already
/// been eaten.
Path* Parser::recover_path_from_fn() {
    Span fn_token_span = token.span;
    bump();
    Span args_lo = token.span;
    ParserState snapshot = create_snapshot_for_diagnostic();
    FnParseMode mode;
    mode.req_name = ReqName::Never;
    mode.context = FnContext::Free;
    mode.req_body = false;
    try {
        FnDecl* decl = parse_fn_decl(mode, AllowPlus::No, RecoverReturnSign::OnlyFatArrow);
        emit("expected identifier, found keyword `fn`", fn_token_span);
        Path* path = mk_path(fn_token_span.to(prev_token.span));
        PathSegment seg;
        seg.ident.name = "Fn";
        seg.ident.span = fn_token_span;
        GenericArgs* args = mk<GenericArgs>();
        args->kind = GenericArgs::Parenthesized;
        args->span = args_lo.to(prev_token.span);
        for (Param* p : decl->inputs) args->inputs.push_back(p->ty);
        Span out_span = decl->output.ty ? decl->output.ty->span : decl->output.span;
        args->inputs_span = args_lo.until(out_span);
        args->output = decl->output;
        seg.args = args;
        path->segments.push_back(seg);
        return path;
    } catch (PErr&) {
        restore_snapshot(snapshot);
        return nullptr;
    }
}

/// Optionally parses `for<$generic_params>`.
std::pair<std::vector<GenericParam*>, OptSpan> Parser::parse_higher_ranked_binder() {
    if (eat_keyword(KW(For))) {
        Span lo = token.span;
        expect_lt();
        std::vector<GenericParam*> params = parse_generic_params();
        expect_gt();
        // We rely on AST validation to rule out invalid cases: There must not be
        // type or const parameters, and parameters must not have bounds.
        return {std::move(params), OptSpan(lo.to(prev_token.span))};
    }
    return {{}, OptSpan()};
}

/// Recover from `Fn`-family traits (Fn, FnMut, FnOnce) with lifetime arguments
/// (e.g. `FnOnce<'a>(&'a str) -> bool`). Up to generic arguments have already
/// been eaten.
void Parser::recover_fn_trait_with_lifetime_params(Path* fn_path, std::vector<GenericParam*>& lifetime_defs) {
    if (fn_path->segments.empty()) return;
    PathSegment& fn_path_segment = fn_path->segments.back();
    GenericArgs* generic_args = fn_path_segment.args;
    if (!generic_args) return;
    std::vector<Lifetime> lifetimes;
    if (generic_args->kind == GenericArgs::AngleBracketed)
        for (const AngleBracketedArg& a : generic_args->args)
            if (!a.is_constraint && a.arg.kind == GenericArg::Lifetime) lifetimes.push_back(a.arg.lifetime);
    if (lifetimes.empty()) return;

    bool have_snapshot = st_.parsing_generics;
    ParserState snapshot;
    if (have_snapshot) snapshot = create_snapshot_for_diagnostic();

    Span inputs_lo = token.span;
    FnParseMode mode;
    mode.req_name = ReqName::Never;
    mode.context = FnContext::Free;
    mode.req_body = false;
    std::vector<Param*> params;
    try {
        params = parse_fn_params(mode);
    } catch (PErr&) {
        if (have_snapshot) {
            restore_snapshot(snapshot);
            return;
        }
        throw;
    }
    std::vector<Ty*> inputs;
    for (Param* p : params) inputs.push_back(p->ty);
    Span inputs_span = inputs_lo.to(prev_token.span);
    FnRetTy output;
    try {
        output = parse_ret_ty(AllowPlus::No, RecoverQPath::No, RecoverReturnSign::No);
    } catch (PErr&) {
        if (have_snapshot) {
            restore_snapshot(snapshot);
            return;
        }
        throw;
    }
    GenericArgs* args = mk<GenericArgs>();
    args->kind = GenericArgs::Parenthesized;
    args->span = path_segment_span(fn_path_segment).to(prev_token.span);
    args->inputs = std::move(inputs);
    args->inputs_span = inputs_span;
    args->output = output;

    if (have_snapshot && token.kind != Tok::Comma && token.kind != Tok::Gt && token.kind != Tok::Plus) {
        // We would expect another bound or the end of type params by now. Most likely we've
        // encountered a `(` *not* representing `Trait()`, but rather the start of the trait's
        // where clause, e.g. `impl<T: Fn<'a>(u8)> Foo<T> where T: ...`.
        restore_snapshot(snapshot);
        return;
    }

    Span generic_args_span = generic_args->span;
    fn_path_segment.args = args;

    // Convert parsed `<'a>` in `Fn<'a>` into `for<'a>`.
    for (const Lifetime& lt : lifetimes) {
        GenericParam* p = mk<GenericParam>();
        p->kind = GenericParam::Lifetime;
        p->ident = lt.ident;
        lifetime_defs.push_back(p);
    }
    emit("`Fn` traits cannot take lifetime parameters", generic_args_span);
}

bool Parser::check_lifetime() {
    st_.expected_token_types.insert(TokenType::Lifetime);
    return tok_is_lifetime(token);
}

/// Parses a single lifetime `'a` or panics.
Lifetime Parser::expect_lifetime() {
    if (tok_is_lifetime(token)) {
        Lifetime lt;
        lt.ident = tok_ident(token);
        if (!token.raw && kw_is_reserved(token.kw, edition_) && token.kw != Kw::Underscore && token.kw != Kw::Static)
            emit("lifetimes cannot use keyword names", lt.ident.span);
        bump();
        return lt;
    }
    fail("not a lifetime", token.span);
}

// ---------------------------------------------------------------------------
// diagnostics.rs: type recovery.

void Parser::maybe_report_ambiguous_plus(bool impl_dyn_multi, const Ty* ty) {
    if (impl_dyn_multi) emit("ambiguous `+` in a type", ty->span);
}

/// Swift lets users write `Ty?` to mean `Option<Ty>`. Parse the construct and recover from it.
Ty* Parser::maybe_recover_from_question_mark(Ty* ty) {
    if (token.kind == Tok::Question) {
        bump();
        emit("invalid `?` in type", prev_token.span);
        return mk_ty(ty->span.to(prev_token.span), TyKind::Err);
    }
    return ty;
}

void Parser::maybe_recover_from_bad_type_plus(const Ty* ty) {
    // Do not add `+` to expected tokens.
    if (!tok_is_like_plus(token)) return;
    bump();  // `+`
    (void)parse_generic_bounds();
    emit("expected a path on the left-hand side of `+`", ty->span);
}

Ty* Parser::maybe_recover_from_bad_qpath_ty(Ty* ty) {
    if (may_recover() && token.kind == Tok::PathSep) {
        std::pair<QSelf*, Path*> qp = maybe_recover_from_bad_qpath_stage_2(ty->span, ty, PathStyle::Type);
        Ty* r = mk_ty(qp.second->span, TyKind::Path);
        r->qself = qp.first;
        r->path = qp.second;
        return r;
    }
    return ty;
}

/// Given an already parsed `Ty`, parses the `::AssocItem` tail and
/// combines them into a `<Ty>::AssocItem` expression/pattern/type.
std::pair<QSelf*, Path*> Parser::maybe_recover_from_bad_qpath_stage_2(Span ty_span, Ty* ty, PathStyle style) {
    expect(EXP(PathSep));
    Path* path = mk_path(Span());
    parse_path_segments(path->segments, style, nullptr);
    path->span = ty_span.to(prev_token.span);
    emit("missing angle brackets in associated item path", ty_span);
    QSelf* qself = mk<QSelf>();
    qself->ty = ty;
    qself->path_span = ty_span.shrink_to_hi();  // Use an empty path since `position == 0`.
    qself->position = 0;
    return {qself, path};
}

void Parser::maybe_recover_bounds_doubled_colon(const Ty* ty) {
    if (ty->kind != TyKind::Path || !ty->path) return;
    const std::vector<PathSegment>& segs = ty->path->segments;
    for (size_t i = 0; i + 1 < segs.size(); ++i) {
        if (ty->qself && i < ty->qself->position) continue;
        Span a_span = path_segment_span(segs[i]);
        Span b_span = path_segment_span(segs[i + 1]);
        Span between_span = a_span.shrink_to_hi().to(b_span.shrink_to_lo());
        if (text(between_span) == ":: ")
            fail("expected `:` followed by trait or lifetime", ty->path->span.shrink_to_hi());
    }
}

}  // namespace detail
}  // namespace rust
