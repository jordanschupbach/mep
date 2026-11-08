// rust_parse_generics.cpp -- generic parameters, where clauses and
// contracts (rustc_parse/src/parser/generics.rs).
//
// Ported from rustc_parse/rustc_lexer (rust-lang/rust 1.97.1, MIT OR
// Apache-2.0).

#include "rust_parse_internal.h"

namespace rust {
namespace detail {

/// Parses bounds of a lifetime parameter `BOUND + BOUND + BOUND`, possibly with trailing `+`.
///
/// ```text
/// BOUND = LT_BOUND (e.g., `'a`)
/// ```
GenericBounds Parser::parse_lt_param_bounds() {
    GenericBounds lifetimes;
    while (check_lifetime()) {
        GenericBound b;
        b.kind = GenericBound::Outlives;
        b.lifetime = expect_lifetime();
        lifetimes.push_back(b);
        if (!eat_plus()) break;
    }
    return lifetimes;
}

/// Matches `typaram = IDENT (`?` unbound)? optbounds ( EQ ty )?`.
GenericParam* Parser::parse_ty_param(AttrVec preceding_attrs) {
    Ident ident = parse_ident();

    if (may_recover()) {
        std::string lower(ident.name);
        for (char& ch : lower)
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        if (lower == "const" && check_ident()) return recover_const_param_with_mistyped_const(std::move(preceding_attrs), ident);
    }

    // Parse optional colon and param bounds.
    GenericParam* p = mk<GenericParam>();
    p->kind = GenericParam::Type;
    p->ident = ident;
    if (eat(EXP(Colon))) {
        p->has_colon = true;
        p->colon_span = prev_token.span;
        // recover from `impl Trait` in type param bound
        if (tok_is_keyword(token, Kw::Impl)) {
            Span impl_span = token.span;
            ParserState snapshot = create_snapshot_for_diagnostic();
            Ty* t = nullptr;
            try {
                t = parse_ty();
            } catch (PErr&) {
            }
            if (t && t->kind == TyKind::ImplTrait)
                fail("expected trait bound, found `impl Trait` type", impl_span.to(token.span.shrink_to_lo()));
            restore_snapshot(snapshot);
        }
        p->bounds = parse_generic_bounds();
    }
    if (eat(EXP(Eq))) p->default_ty = parse_ty();
    p->attrs = std::move(preceding_attrs);
    return p;
}

GenericParam* Parser::parse_const_param(AttrVec preceding_attrs) {
    Span const_span = token.span;

    expect_keyword(KW(Const));
    Ident ident = parse_ident();
    GenericParam* p = mk<GenericParam>();
    p->kind = GenericParam::Const;
    p->ident = ident;
    p->attrs = std::move(preceding_attrs);
    try {
        expect(EXP(Colon));
    } catch (PErr& err) {
        if (token.kind == Tok::Comma || token.kind == Tok::Gt) {
            // Recover from missing type: `const N >` / `const N,`.
            Span span = const_span.to(ident.span);
            emit(err);
            p->const_ty = mk_ty(span, TyKind::Err);
            p->const_span = span;
            return p;
        }
        throw;
    }
    Ty* ty = parse_ty();

    // Parse optional const generics default value.
    AnonConst* def = nullptr;
    if (eat(EXP(Eq))) {
        def = mk<AnonConst>();
        *def = parse_const_arg();
    }
    p->const_ty = ty;
    p->const_default = def;
    p->const_span = def ? const_span.to(def->value->span) : const_span.to(ty->span);
    return p;
}

GenericParam* Parser::recover_const_param_with_mistyped_const(AttrVec preceding_attrs, Ident mistyped_const_ident) {
    Ident ident = parse_ident();
    expect(EXP(Colon));
    Ty* ty = parse_ty();

    // Parse optional const generics default value.
    AnonConst* def = nullptr;
    if (eat(EXP(Eq))) {
        def = mk<AnonConst>();
        *def = parse_const_arg();
    }
    Span span = def ? mistyped_const_ident.span.to(def->value->span) : mistyped_const_ident.span.to(ty->span);

    emit("`const` keyword was mistyped as `" + std::string(mistyped_const_ident.name) + "`", mistyped_const_ident.span);

    GenericParam* p = mk<GenericParam>();
    p->kind = GenericParam::Const;
    p->ident = ident;
    p->attrs = std::move(preceding_attrs);
    p->const_ty = ty;
    p->const_default = def;
    p->const_span = span;
    return p;
}

/// Parses a (possibly empty) list of lifetime and type parameters, possibly including
/// a trailing comma and erroneous trailing attributes.
std::vector<GenericParam*> Parser::parse_generic_params() {
    std::vector<GenericParam*> params;
    bool done = false;
    bool prev = st_.parsing_generics;
    st_.parsing_generics = true;
    while (!done) {
        AttrWrapper attrs = parse_outer_attributes();
        GenericParam* param = nullptr;
        bool stop = false;
        try {
            if (eat_keyword_noexpect(Kw::SelfUpper)) {
                // `Self` as a generic param is invalid. Here we emit the diagnostic and continue
                // parsing the next param.
                emit("unexpected keyword `Self` in generic parameters", prev_token.span);
                // Eat a trailing comma, if it exists.
                (void)eat(EXP(Comma));
            }

            if (check_lifetime()) {
                Lifetime lifetime = expect_lifetime();
                param = mk<GenericParam>();
                param->kind = GenericParam::Lifetime;
                param->ident = lifetime.ident;
                // Parse lifetime parameter.
                if (eat(EXP(Colon))) {
                    param->has_colon = true;
                    param->colon_span = prev_token.span;
                    param->bounds = parse_lt_param_bounds();
                }
                if (check_noexpect(Tok::Eq) && look_ahead(1, [this](const Token& t) { return tok_is_lifetime(t); })) {
                    Span lo = token.span;
                    // Parse `= 'lifetime`.
                    bump();  // `=`
                    bump();  // `'lifetime`
                    emit("unexpected default lifetime parameter", lo.to(prev_token.span));
                }
                param->attrs = std::move(attrs.attrs);
            } else if (check_keyword(KW(Const))) {
                // Parse const parameter.
                param = parse_const_param(std::move(attrs.attrs));
            } else if (check_ident()) {
                // Parse type parameter.
                param = parse_ty_param(std::move(attrs.attrs));
            } else if (tok_can_begin_type(token)) {
                // Trying to write an associated type bound? (#26271)
                ParserState snapshot = create_snapshot_for_diagnostic();
                Span lo = token.span;
                try {
                    parse_ty_where_predicate_kind();
                    emit("bounds on associated types do not belong here", lo.to(prev_token.span));
                } catch (PErr&) {
                    restore_snapshot(snapshot);
                }
                stop = true;
            } else {
                // Check for trailing attributes and stop parsing.
                if (!attrs.attrs.empty()) {
                    if (!params.empty())
                        emit("trailing attribute after generic parameter", attrs.attrs[0].span);
                    else
                        emit("attribute without generic parameters", attrs.attrs[0].span);
                }
                stop = true;
            }
            if (!stop && !eat(EXP(Comma))) done = true;
        } catch (...) {
            st_.parsing_generics = prev;
            throw;
        }
        if (stop || !param) break;
        params.push_back(param);
    }
    st_.parsing_generics = prev;
    return params;
}

/// Parses a set of optional generic type parameter declarations. Where
/// clauses are not parsed here, and must be added later via
/// `parse_where_clause()`.
///
/// matches generics = ( ) | ( < > ) | ( < typaramseq ( , )? > ) | ( < lifetimes ( , )? > )
///                  | ( < lifetimes , typaramseq ( , )? > )
/// where   typaramseq = ( typaram ) | ( typaram , typaramseq )
Generics Parser::parse_generics() {
    // invalid path separator `::` in function definition
    // for example `fn invalid_path_separator::<T>() {}`
    if (eat_noexpect(Tok::PathSep)) emit("invalid path separator in function definition", prev_token.span);

    Span span_lo = token.span;
    Generics g;
    if (eat_lt()) {
        g.params = parse_generic_params();
        expect_gt_or_maybe_suggest_closing_generics(g.params);
        g.span = span_lo.to(prev_token.span);
    } else {
        g.span = prev_token.span.shrink_to_hi();
    }
    g.where_clause.has_where_token = false;
    g.where_clause.span = prev_token.span.shrink_to_hi();
    return g;
}

void Parser::expect_gt_or_maybe_suggest_closing_generics(const std::vector<GenericParam*>& params) {
    (void)params;  // only feeds a suggestion
    expect_gt();
}

/// Parses an experimental fn contract
/// (`contract_requires(WWW) contract_ensures(ZZZ)`)
FnContract* Parser::parse_contract() {
    Expr* requires_ = nullptr;
    if (eat_keyword_noexpect(Kw::ContractRequires)) {
        Block* decls_and_precond = parse_block();
        Expr* precond = nullptr;
        if (!decls_and_precond->stmts.empty()) {
            Stmt* last = decls_and_precond->stmts.back();
            decls_and_precond->stmts.pop_back();
            precond = last->kind == StmtKind::Expr ? last->expr : mk_unit_expr(decls_and_precond->span);
        } else {
            precond = mk_unit_expr(decls_and_precond->span);
        }
        requires_ = mk_closure_expr(precond->span, precond);
    }
    Expr* ensures = nullptr;
    if (eat_keyword_noexpect(Kw::ContractEnsures)) ensures = parse_expr();
    if (!requires_ && !ensures) return nullptr;
    FnContract* c = mk<FnContract>();
    c->requires_ = requires_;
    c->ensures = ensures;
    return c;
}

/// Parses an optional where-clause.
///
/// ```ignore (only-for-syntax-highlight)
/// where T : Trait<U, V> + 'b, 'a : 'b
/// ```
WhereClause Parser::parse_where_clause() { return parse_where_clause_common(false, Ident(), Span()).first; }

std::pair<WhereClause, std::optional<std::vector<FieldDef*>>> Parser::parse_struct_where_clause(Ident struct_name,
                                                                                               Span body_insertion_point) {
    return parse_where_clause_common(true, struct_name, body_insertion_point);
}

std::pair<WhereClause, std::optional<std::vector<FieldDef*>>> Parser::parse_where_clause_common(
    bool has_struct, Ident struct_name, Span body_insertion_point) {
    WhereClause where_clause;
    where_clause.has_where_token = false;
    where_clause.span = prev_token.span.shrink_to_hi();
    std::optional<std::vector<FieldDef*>> tuple_struct_body;

    if (!eat_keyword(KW(Where))) return {where_clause, std::nullopt};

    if (eat_noexpect(Tok::Colon)) emit("unexpected colon after `where`", prev_token.span);

    where_clause.has_where_token = true;
    Span where_lo = prev_token.span;

    if (choose_generics_over_qpath(0)) {
        Generics generics = parse_generics();
        emit("generic parameters on `where` clauses are reserved for future use", generics.span);
    }

    for (;;) {
        Span where_sp = where_lo.to(prev_token.span);
        AttrWrapper attrs = parse_outer_attributes();
        Span pred_lo = token.span;
        WherePredicate* pred = nullptr;
        if (check_lifetime() && look_ahead(1, [this](const Token& t) { return !tok_is_like_plus(t); })) {
            Lifetime lifetime = expect_lifetime();
            // Bounds starting with a colon are mandatory, but possibly empty.
            expect(EXP(Colon));
            GenericBounds bounds = parse_lt_param_bounds();
            pred = mk<WherePredicate>();
            pred->kind = WherePredicate::Region;
            pred->lifetime = lifetime;
            pred->bounds = std::move(bounds);
        } else if (check_type()) {
            std::optional<std::vector<FieldDef*>> body;
            pred = parse_ty_where_predicate_kind_or_recover_tuple_struct_body(has_struct, struct_name,
                                                                              body_insertion_point, pred_lo, where_sp,
                                                                              body);
            if (!pred) tuple_struct_body = std::move(body);
        } else {
            if (!attrs.attrs.empty()) {
                const Attribute& last = attrs.attrs.back();
                if (last.is_doc_comment)
                    emit("found a documentation comment that doesn't document anything", last.span);
                else
                    emit("attribute without where predicates", last.span);
            }
        }
        if (!pred) break;
        pred->attrs = std::move(attrs.attrs);
        pred->span = pred_lo.to(prev_token.span);
        where_clause.predicates.push_back(pred);

        Span prev_tok = prev_token.span;
        bool ate_comma = eat(EXP(Comma));

        if (eat_keyword_noexpect(Kw::Where)) {
            emit("cannot define duplicate `where` clauses on an item", token.span);
            (void)prev_tok;
        } else if (!ate_comma) {
            break;
        }
    }

    where_clause.span = where_lo.to(prev_token.span);
    return {where_clause, std::move(tuple_struct_body)};
}

WherePredicate* Parser::parse_ty_where_predicate_kind_or_recover_tuple_struct_body(
    bool has_struct, Ident struct_name, Span body_insertion_point, Span pred_lo, Span where_sp,
    std::optional<std::vector<FieldDef*>>& body) {
    (void)struct_name;
    (void)body_insertion_point;
    (void)pred_lo;
    (void)where_sp;
    (void)body;
    // rustc snapshots here (when `has_struct` and the token is `(`) and, if the
    // predicate fails to parse, tries `parse_tuple_struct_body` on the snapshot
    // to report "where clauses are not allowed before tuple struct bodies".
    // item.rs's parse_tuple_struct_body is not exposed to this file, so the
    // type error propagates instead.
    (void)has_struct;
    return parse_ty_where_predicate_kind();
}

WherePredicate* Parser::parse_ty_where_predicate_kind() {
    // Parse optional `for<'a, 'b>`.
    // This `for` is parsed greedily and applies to the whole predicate,
    // the bounded type can have its own `for` applying only to it.
    // Examples:
    // * `for<'a> Trait1<'a>: Trait2<'a /* ok */>`
    // * `(for<'a> Trait1<'a>): Trait2<'a /* not ok */>`
    // * `for<'a> for<'b> Trait1<'a, 'b>: Trait2<'a /* ok */, 'b /* not ok */>`
    std::vector<GenericParam*> bound_vars = parse_higher_ranked_binder().first;

    // Parse type with mandatory colon and (possibly empty) bounds,
    // or with mandatory equality sign and the second type.
    Ty* ty = parse_ty_for_where_clause();
    WherePredicate* pred = mk<WherePredicate>();
    if (eat(EXP(Colon))) {
        pred->kind = WherePredicate::Bound;
        pred->bound_generic_params = std::move(bound_vars);
        pred->bounded_ty = ty;
        pred->bounds = parse_generic_bounds();
        return pred;
    }
    // FIXME: Decide what should be used here, `=` or `==`.
    // FIXME: We are just dropping the binders in lifetime_defs on the floor here.
    bool eq = eat(EXP(Eq));
    if (!eq) eq = eat(EXP(EqEq));
    if (eq) {
        pred->kind = WherePredicate::Eq;
        pred->lhs_ty = ty;
        pred->rhs_ty = parse_ty();
        return pred;
    }
    maybe_recover_bounds_doubled_colon(ty);
    unexpected_any();
}

bool Parser::choose_generics_over_qpath(size_t start) const {
    // There's an ambiguity between generic parameters and qualified paths in impls.
    // If we see `<` it may start both, so we have to inspect some following tokens.
    // The following combinations can only start generics,
    // but not qualified paths (with one exception):
    //     `<` `>` - empty generic parameters
    //     `<` `#` - generic parameters with attributes
    //     `<` (LIFETIME|IDENT) `>` - single generic parameter
    //     `<` (LIFETIME|IDENT) `,` - first generic parameter in a list
    //     `<` (LIFETIME|IDENT) `:` - generic parameter with bounds
    //     `<` (LIFETIME|IDENT) `=` - generic parameter with a default
    //     `<` const                - generic const parameter
    //     `<` IDENT `?`            - RECOVERY `impl<T ?Bound` missing a `:`, meant to avoid that
    //                                `T` and `?` get parsed as qpath
    // The only truly ambiguous case is
    //     `<` IDENT `>` `::` IDENT ...
    // we disambiguate it in favor of generics (`impl<T> ::absolute::Path<T> { ... }`)
    // because this is what almost always expected in practice, qualified paths in impls
    // (`impl <Type>::AssocTy { ... }`) aren't even allowed by type checker at the moment.
    if (look_ahead_tok(start).kind != Tok::Lt) return false;
    const Token& t1 = look_ahead_tok(start + 1);
    if (t1.kind == Tok::Pound || t1.kind == Tok::Gt) return true;
    if (tok_is_lifetime(t1) || tok_is_ident(t1)) {
        Tok k2 = look_ahead_tok(start + 2).kind;
        if (k2 == Tok::Gt || k2 == Tok::Comma || k2 == Tok::Colon || k2 == Tok::Eq || k2 == Tok::Question) return true;
    }
    return is_keyword_ahead(start + 1, {Kw::Const});
}

}  // namespace detail
}  // namespace rust
