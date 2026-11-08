// rust_parse_pat.cpp -- patterns (rustc_parse/src/parser/pat.rs) plus the
// pattern recovery of diagnostics.rs.
//
// Ported from rustc_parse/rustc_lexer (rust-lang/rust 1.97.1, MIT OR
// Apache-2.0).

#include <algorithm>

#include "rust_parse_internal.h"

namespace rust {
namespace detail {

namespace {

const char* expected_to_string_or_fallback(Expected e) {
    switch (e) {
    case Expected::ParameterName: return "parameter name";
    case Expected::ArgumentName: return "argument name";
    case Expected::Identifier: return "identifier";
    case Expected::BindingPattern: return "binding pattern";
    case Expected::None: return "pattern";
    }
    return "pattern";
}

/// Pat::walk: calls `f` on `p` and, while `f` returns true, on its subpatterns.
template <class F>
void walk_pat(Pat* p, F& f) {
    if (!p || !f(p)) return;
    switch (p->kind) {
    case PatKind::Ident: walk_pat(p->sub, f); break;
    case PatKind::Struct:
        for (PatField& fp : p->fields) walk_pat(fp.pat, f);
        break;
    case PatKind::TupleStruct:
    case PatKind::Or:
    case PatKind::Tuple:
    case PatKind::Slice:
        for (Pat* q : p->elems) walk_pat(q, f);
        break;
    case PatKind::Box:
    case PatKind::Deref:
    case PatKind::Ref:
    case PatKind::Paren:
    case PatKind::Guard: walk_pat(p->sub, f); break;
    default: break;
    }
}

/// A float literal symbol that break_up_float splits as `a.b` (DestructuredFloat::MiddleDot).
bool float_is_middle_dot(std::string_view sym) {
    // break_up_float: components are runs of identifier characters separated by
    // `.`, `+` or `-` punctuation.
    std::vector<char> comps;  // 'i' ident run, or the punct char
    bool in_ident = false;
    for (char c : sym) {
        if (c == '.' || c == '+' || c == '-') {
            comps.push_back(c);
            in_ident = false;
        } else if (!in_ident) {
            comps.push_back('i');
            in_ident = true;
        }
    }
    return comps.size() == 3 && comps[0] == 'i' && comps[1] == '.' && comps[2] == 'i';
}

/// expr.rs could_be_unclosed_char_literal: `'x` where `x` unescapes as a char.
bool could_be_unclosed_char_literal(std::string_view lifetime_name) {
    if (lifetime_name.size() < 2 || lifetime_name[0] != '\'') return false;
    std::string_view rest = lifetime_name.substr(1);
    size_t len = 1;
    decode_utf8(rest, 0, len);
    return len == rest.size();
}

}  // namespace

// ---------------------------------------------------------------------------
// pat.rs

/// Parses a pattern.
///
/// Corresponds to `Pattern` in RFC 3637 and admits guard patterns at the top level.
/// Used when parsing patterns in all cases where neither `PatternNoTopGuard` nor
/// `PatternNoTopAlt` (see below) are used.
Pat* Parser::parse_pat_allow_top_guard(Expected expected, RecoverComma rc, RecoverColon ra, CommaRecoveryMode rt) {
    Pat* pat = parse_pat_no_top_guard(expected, rc, ra, rt);
    if (eat_keyword(KW(If))) {
        Span leading_if_span = prev_token.span;
        Expr* cond = parse_expr();
        Guard* guard = mk<Guard>();
        guard->cond = cond;
        guard->span_with_leading_if = leading_if_span.to(cond->span);
        Pat* g = mk_pat(pat->span.to(guard->span_with_leading_if), PatKind::Guard);
        g->sub = pat;
        g->guard = guard;
        return g;
    }
    return pat;
}

/// Parses a pattern.
///
/// Corresponds to `PatternNoTopAlt` in RFC 3637 and does not admit or-patterns
/// or guard patterns at the top level. Used when parsing the parameters of lambda
/// expressions, functions, function pointers, and `pat_param` macro fragments.
Pat* Parser::parse_pat_no_top_alt(Expected expected, PatternLocation syntax_loc) {
    return parse_pat_with_range_pat(true, expected, syntax_loc);
}

/// Parses a pattern.
///
/// Corresponds to `PatternNoTopGuard` in RFC 3637 and allows or-patterns, but not
/// guard patterns, at the top level. Used for parsing patterns in `pat` fragments (until
/// the next edition) and `let`, `if let`, and `while let` expressions.
///
/// Note that after the FCP in <https://github.com/rust-lang/rust/issues/81415>,
/// a leading vert is allowed in nested or-patterns, too. This allows us to
/// simplify the grammar somewhat.
Pat* Parser::parse_pat_no_top_guard(Expected expected, RecoverComma rc, RecoverColon ra, CommaRecoveryMode rt) {
    return parse_pat_no_top_guard_inner(expected, rc, ra, rt, PatternLocation::None).first;
}

/// Returns the pattern and a bool indicating whether we recovered from a trailing vert (true =
/// recovered).
std::pair<Pat*, bool> Parser::parse_pat_no_top_guard_inner(Expected expected, RecoverComma rc, RecoverColon ra,
                                                            CommaRecoveryMode rt, PatternLocation syntax_loc) {
    // We use `parse_pat_allow_top_alt` regardless of whether we actually want top-level
    // or-patterns so that we can detect when a user tries to use it. This allows us to print a
    // better error message.
    OptSpan leading_vert_span;
    bool trailing_vert = false;
    switch (eat_or_separator(OptSpan())) {
    case EatOrResult::AteOr: leading_vert_span = OptSpan(prev_token.span); break;
    case EatOrResult::TrailingVert: trailing_vert = true; break;
    case EatOrResult::None: break;
    }

    // Parse the first pattern (`p_0`).
    Pat* first_pat = nullptr;
    try {
        first_pat = parse_pat_no_top_alt(expected, syntax_loc);
    } catch (PErr& err) {
        if (tok_is_reserved_ident(token) && !tok_is_keyword(token, Kw::In) && !tok_is_keyword(token, Kw::If)) {
            emit(err);
            bump();
            first_pat = mk_pat(token.span, PatKind::Wild);
        } else {
            throw;
        }
    }
    if (rc == RecoverComma::Yes && !pat_could_be_never_pattern(first_pat)) maybe_recover_unexpected_comma(first_pat->span, rt);

    // If the next token is not a `|`,
    // this is not an or-pattern and we should exit here.
    if (!check(EXP(Or)) && token.kind != Tok::OrOr) {
        // If we parsed a leading `|` which should be gated,
        // then we should really gate the leading `|`.
        // This complicated procedure is done purely for diagnostics UX.

        // Check if the user wrote `foo:bar` instead of `foo::bar`.
        if (ra == RecoverColon::Yes && token.kind == Tok::Colon) first_pat = recover_colon_colon_in_pat_typo(first_pat, expected);

        if (leading_vert_span.has) {
            // If there was a leading vert, treat this as an or-pattern. This improves
            // diagnostics.
            Span span = leading_vert_span.span.to(prev_token.span);
            Pat* p = mk_pat(span, PatKind::Or);
            p->elems.push_back(first_pat);
            return {p, trailing_vert};
        }
        return {first_pat, trailing_vert};
    }

    // Parse the patterns `p_1 | ... | p_n` where `n > 0`.
    Span lo = leading_vert_span.has ? leading_vert_span.span : first_pat->span;
    std::vector<Pat*> pats{first_pat};
    for (;;) {
        EatOrResult r = eat_or_separator(OptSpan(lo));
        if (r == EatOrResult::None) break;
        if (r == EatOrResult::TrailingVert) {
            trailing_vert = true;
            break;
        }
        Pat* pat = parse_pat_no_top_alt(expected, syntax_loc);
        if (rc == RecoverComma::Yes && !pat_could_be_never_pattern(pat)) maybe_recover_unexpected_comma(pat->span, rt);
        pats.push_back(pat);
    }
    Span or_pattern_span = lo.to(prev_token.span);
    Pat* p = mk_pat(or_pattern_span, PatKind::Or);
    p->elems = std::move(pats);
    return {p, trailing_vert};
}

/// Parse a pattern and (maybe) a `Colon` in positions where a pattern may be followed by a type
/// annotation (e.g. for `let` bindings or `fn` params).
///
/// Generally, this corresponds to `pat_no_top_alt` followed by an optional `Colon`. It will
/// eat the `Colon` token if one is present.
///
/// The return value represents the parsed pattern and `true` if a `Colon` was parsed (`false`
/// otherwise).
std::pair<Pat*, bool> Parser::parse_pat_before_ty(Expected expected, RecoverComma rc, PatternLocation syntax_loc) {
    // We use `parse_pat_allow_top_alt` regardless of whether we actually want top-level
    // or-patterns so that we can detect when a user tries to use it. This allows us to print a
    // better error message.
    std::pair<Pat*, bool> r =
        parse_pat_no_top_guard_inner(expected, rc, RecoverColon::No, CommaRecoveryMode::LikelyTuple, syntax_loc);
    Pat* pat = r.first;
    bool trailing_vert = r.second;
    bool colon = eat(EXP(Colon));

    if (pat->kind == PatKind::Or) {
        const char* msg = syntax_loc == PatternLocation::FunctionParameter
                              ? "function parameters require top-level or-patterns in parentheses"
                              : "`let` bindings require top-level or-patterns in parentheses";
        // a trailing vert has already been reported: the error is a delayed bug then
        if (!trailing_vert) emit(msg, pat->span);
    }
    return {pat, colon};
}

/// Parse the pattern for a function or function pointer parameter, followed by a colon.
///
/// The return value represents the parsed pattern and `true` if a `Colon` was parsed (`false`
/// otherwise).
std::pair<Pat*, bool> Parser::parse_fn_param_pat_colon() {
    // In order to get good UX, we first recover in the case of a leading vert for an illegal
    // top-level or-pat. Normally, this means recovering both `|` and `||`, but in this case,
    // a leading `||` probably doesn't indicate an or-pattern attempt, so we handle that
    // separately.
    if (token.kind == Tok::OrOr) {
        emit("unexpected `||` before function parameter", token.span);
        bump();
    }
    return parse_pat_before_ty(Expected::ParameterName, RecoverComma::No, PatternLocation::FunctionParameter);
}

/// Eat the or-pattern `|` separator.
/// If instead a `||` token is encountered, recover and pretend we parsed `|`.
Parser::EatOrResult Parser::eat_or_separator(OptSpan lo) {
    if (recover_trailing_vert(lo)) return EatOrResult::TrailingVert;
    if (token.kind == Tok::OrOr) {
        // Found `||`; Recover and pretend we parsed `|`.
        emit("unexpected token `||` in pattern", token.span);
        bump();
        return EatOrResult::AteOr;
    }
    if (eat(EXP(Or))) return EatOrResult::AteOr;
    return EatOrResult::None;
}

/// Recover if `|` or `||` is the current token and we have one of the
/// tokens `=>`, `if`, `=`, `:`, `;`, `,`, `]`, `)`, or `}` ahead of us.
///
/// These tokens all indicate that we reached the end of the or-pattern
/// list and can now reliably say that the `|` was an illegal trailing vert.
/// Note that there are more tokens such as `@` for which we know that the `|`
/// is an illegal parse. However, the user's intent is less clear in that case.
bool Parser::recover_trailing_vert(OptSpan lo) {
    (void)lo;
    const Token& t = look_ahead_tok(1);
    bool is_end_ahead = t.kind == Tok::FatArrow || (t.kind == Tok::Ident && !t.raw && t.kw == Kw::If) ||
                        t.kind == Tok::Eq || t.kind == Tok::Semi || t.kind == Tok::Colon || t.kind == Tok::Comma ||
                        t.kind == Tok::CloseBracket || t.kind == Tok::CloseParen || t.kind == Tok::CloseBrace;
    if (is_end_ahead && (token.kind == Tok::Or || token.kind == Tok::OrOr)) {
        emit("a trailing `" + token_str(token) + "` is not allowed in an or-pattern", token.span);
        bump();
        return true;
    }
    return false;
}

/// Ensures that the last parsed pattern (or pattern range bound) is not followed by an
/// expression.
///
/// `is_end_bound` indicates whether the last parsed thing was the end bound of a range
/// pattern (see [issue #47483]), as in that case the "found an expression" error is
/// reported as "expected a pattern range bound".
///
/// Returns the expression's span when one was found and an error stashed for it:
/// the pattern must then be discarded as `PatKind::Err`.
OptSpan Parser::maybe_recover_trailing_expr(Span pat_span, bool is_end_bound) {
    if (tok_is_keyword(prev_token, Kw::Underscore) || !may_recover()) {
        // Don't recover anything after an `_` or if recovery is disabled.
        return OptSpan();
    }

    // Returns `true` iff `token` is an unsuffixed integer.
    auto is_one_tuple_index = [](const Token& t) {
        return t.kind == Tok::Literal && t.lit == LitKind::Integer && t.suffix.empty();
    };
    // Returns `true` iff `token` is an unsuffixed `x.y` float.
    auto is_two_tuple_indexes = [](const Token& t) {
        return t.kind == Tok::Literal && t.lit == LitKind::Float && t.suffix.empty() && float_is_middle_dot(t.sym);
    };

    // Check for `.hello` or `.0`.
    bool has_dot_expr = check_noexpect(Tok::Dot) && look_ahead(1, [&](const Token& t) {
                            return tok_is_ident(t) || is_one_tuple_index(t) || is_two_tuple_indexes(t);
                        });

    // Check for operators.
    // `|` is excluded as it is used in pattern alternatives and lambdas,
    // `?` is included for error propagation,
    // `[` is included for indexing operations,
    // `[]` is excluded as `a[]` isn't an expression and should be recovered as `a, []` (cf. `tests/ui/parser/pat-lt-bracket-7.rs`),
    // `as` is included for type casts
    Tok k = token.kind;
    bool has_trailing_operator =
        k == Tok::Plus || k == Tok::Minus || k == Tok::Star || k == Tok::Slash || k == Tok::Percent ||
        k == Tok::Caret || k == Tok::And || k == Tok::Shl || k == Tok::Shr || k == Tok::Question ||
        (k == Tok::OpenBracket && look_ahead(1, [](const Token& t) { return t.kind != Tok::CloseBracket; })) ||
        tok_is_keyword(token, Kw::As);

    if (!has_dot_expr && !has_trailing_operator) {
        // Nothing to recover here.
        return OptSpan();
    }

    // Let's try to parse an expression to emit a better diagnostic.
    ParserState snapshot = create_snapshot_for_diagnostic();
    st_.restrictions = static_cast<Restrictions>(st_.restrictions | R_IS_PAT);
    Expr* expr = nullptr;
    try {
        // Parse `?`, `.f`, `(arg0, arg1, ...)` or `[expr]` until they've all been eaten.
        expr = parse_expr_dot_or_call_with(AttrVec(), mk_expr(pat_span, ExprKind::Dummy), pat_span);
        // Contonue by trying to parse the `(` or `.` that was not part of an expression.
        expr = parse_expr_assoc_rest_with(PrecBound::unbounded(), false, expr).first;
    } catch (PErr&) {
        restore_snapshot(snapshot);
        return OptSpan();
    }
    st_.restrictions = static_cast<Restrictions>(st_.restrictions & ~R_IS_PAT);

    // Check if the expression is the start or end bound of a range pattern.
    bool is_bound = is_end_bound || tok_is_range_separator(token) ||
                    (token.kind == Tok::CloseParen && look_ahead(1, [this](const Token& t) { return tok_is_range_separator(t); }));

    Span span = expr->span;
    // Stashed (StashKey::ExprInPat): emitted by
    // maybe_augment_stashed_expr_in_pats_with_suggestions or at the end.
    late_.push_back(PErr{is_bound ? "expected a pattern range bound, found an expression"
                                  : "expected a pattern, found an expression",
                         span});
    expr_in_pat_stash_.push_back(span);
    return OptSpan(span);
}

/// Called by [`Parser::parse_stmt_without_recovery`], used to add statement-aware subdiagnostics to the errors stashed
/// by [`Parser::maybe_recover_trailing_expr`].
void Parser::maybe_augment_stashed_expr_in_pats_with_suggestions(const Stmt* stmt) {
    // rustc walks the statement's patterns and steals each stashed error at the
    // pattern's span (only to add suggestions); stealing emits it now.
    if (!stmt) return;
    std::vector<Span> mine;
    std::vector<Span> rest;
    for (Span sp : expr_in_pat_stash_) (stmt->span.contains(sp) ? mine : rest).push_back(sp);
    std::stable_sort(mine.begin(), mine.end(), [](Span a, Span b) { return a.lo < b.lo; });
    for (Span sp : mine) (void)steal_late_error(sp);
    expr_in_pat_stash_ = std::move(rest);
}

/// Parses a pattern, with a setting whether modern range patterns (e.g., `a..=b`, `a..b` are
/// allowed).
Pat* Parser::parse_pat_with_range_pat(bool allow_range_pat, Expected expected, PatternLocation syntax_loc) {
    DepthGuard guard(*this);
    // (maybe_recover_from_interpolated_ty_qpath / eat_metavar_pat: macro expansion only.)

    Span lo = token.span;

    if (tok_is_keyword(token, Kw::Let) && look_ahead(1, [this](const Token& t) { return tok_can_begin_pattern(t, false); })) {
        bump();
        // Trim extra space after the `let`
        emit("expected pattern, found `let`", lo);
        lo = token.span;
    }

    Pat* pat = nullptr;
    std::optional<std::pair<RangeEndKind, Span>> form;
    if (check(EXP(And)) || token.kind == Tok::AndAnd) {
        pat = parse_pat_deref(expected);
    } else if (check(EXP(OpenParen))) {
        pat = parse_pat_tuple_or_parens();
    } else if (check(EXP(OpenBracket))) {
        // Parse `[pat, pat,...]` as a slice pattern.
        std::vector<Pat*> pats = parse_delim_comma_seq<Pat*>(EXP(OpenBracket), EXP(CloseBracket), [this]() {
            return parse_pat_allow_top_guard(Expected::None, RecoverComma::No, RecoverColon::No,
                                             CommaRecoveryMode::EitherTupleOrPipe);
        });
        pat = mk_pat(Span(), PatKind::Slice);
        pat->elems = std::move(pats);
    } else if (check(EXP(DotDot)) && !is_pat_range_end_start(1)) {
        // A rest pattern `..`.
        bump();  // `..`
        pat = mk_pat(Span(), PatKind::Rest);
    } else if (check(EXP(DotDotDot)) && !is_pat_range_end_start(1)) {
        pat = recover_dotdotdot_rest_pat(lo, expected);
    } else if ((form = parse_range_end())) {
        pat = parse_pat_range_to(*form);  // `..=X`, `...X`, or `..X`.
    } else if (eat(EXP(Bang))) {
        pat = mk_pat(Span(), PatKind::Never);
    } else if (eat_keyword(KW(Underscore))) {
        // Parse `_`
        pat = mk_pat(Span(), PatKind::Wild);
    } else if (eat_keyword(KW(Mut))) {
        pat = parse_pat_ident_mut();
    } else if (eat_keyword(KW(Ref))) {
        if (check_keyword(KW(Box))) {
            // Suggest `box ref`.
            Span span = prev_token.span.to(token.span);
            bump();
            emit("switch the order of `ref` and `box`", span);
        }
        // Parse ref ident @ pat / ref mut ident @ pat / ref pin const|mut ident @ pat
        std::pair<Pinnedness, Mutability> pm = parse_pin_and_mut();
        ByRef br;
        br.yes = true;
        br.pinned = pm.first;
        br.mutbl = pm.second;
        pat = parse_pat_ident(br, Mutability::Not, syntax_loc);
    } else if (eat_keyword(KW(Box))) {
        pat = parse_pat_box();
    } else if (check_inline_const(0)) {
        // Parse `const pat`
        Expr* const_expr = parse_const_block(lo.to(token.span), true);
        if ((form = parse_range_end())) {
            pat = parse_pat_range_begin_with(const_expr, *form);
        } else {
            pat = mk_pat(Span(), PatKind::Expr);
            pat->expr = const_expr;
        }
    } else if (is_builtin()) {
        pat = parse_pat_builtin();
    } else if (can_be_ident_pat() || (is_lit_bad_ident().has_value() && may_recover())) {
        // Parse `ident @ pat`
        // This can give false positives and parse nullary enums,
        // they are dealt with later in resolve.
        pat = parse_pat_ident(ByRef(), Mutability::Not, syntax_loc);
    } else if (is_start_of_pat_with_path()) {
        // Parse pattern starting with a path
        QSelf* qself = nullptr;
        Path* path = nullptr;
        if (eat_lt()) {
            // Parse a qualified path
            std::pair<QSelf*, Path*> qp = parse_qpath(PathStyle::Pat);
            qself = qp.first;
            path = qp.second;
        } else {
            // Parse an unqualified path
            path = parse_path(PathStyle::Pat);
        }
        Span span = lo.to(prev_token.span);

        if (qself == nullptr && check(EXP(Bang))) {
            pat = parse_pat_mac_invoc(path);
        } else if ((form = parse_range_end())) {
            Expr* begin = mk_expr(span, ExprKind::Path);
            begin->qself = qself;
            begin->path = path;
            pat = parse_pat_range_begin_with(begin, *form);
        } else if (check(EXP(OpenBrace))) {
            pat = parse_pat_struct(qself, path);
        } else if (check(EXP(OpenParen))) {
            pat = parse_pat_tuple_struct(qself, path);
        } else {
            if (maybe_recover_trailing_expr(span, false).has) {
                pat = mk_pat(Span(), PatKind::Err);
            } else {
                pat = mk_pat(Span(), PatKind::Path);
                pat->qself = qself;
                pat->path = path;
            }
        }
    } else if (tok_is_lifetime(token) && !token.raw && could_be_unclosed_char_literal(token.sym) &&
               !look_ahead(1, [](const Token& t) { return t.kind == Tok::Colon; })) {
        // Recover a `'a` as a `'a'` literal
        Lifetime lt = expect_lifetime();
        std::string msg = std::string("expected ") + expected_to_string_or_fallback(expected) + ", found " +
                          token_descr(token);
        Expr* lit = recover_unclosed_char_expr(lt.ident, msg, token.span);
        Expr* e = mk_expr(lo, ExprKind::Lit);
        e->lit = lit->lit;
        pat = mk_pat(Span(), PatKind::Expr);
        pat->expr = e;
    } else {
        // Try to parse everything else as literal with optional minus
        Expr* begin = nullptr;
        try {
            begin = parse_literal_maybe_minus();
        } catch (PErr&) {
            fatal_unexpected_non_pat(expected);
        }
        OptSpan rec = maybe_recover_trailing_expr(begin->span, false);
        if (rec.has) begin = mk_expr_err(rec.span);
        if ((form = parse_range_end())) {
            pat = parse_pat_range_begin_with(begin, *form);
        } else {
            pat = mk_pat(Span(), PatKind::Expr);
            pat->expr = begin;
        }
    }

    pat->span = lo.to(prev_token.span);
    pat = maybe_recover_from_bad_qpath_pat(pat);
    if (eat_noexpect(Tok::At)) pat = recover_intersection_pat(pat);

    if (!allow_range_pat) ban_pat_range_if_ambiguous(pat);

    return pat;
}

/// Recover from a typoed `...` pattern that was encountered
/// Ref: Issue #70388
Pat* Parser::recover_dotdotdot_rest_pat(Span lo, Expected expected) {
    // A typoed rest pattern `...`.
    bump();  // `...`
    emit("unexpected `...`", lo);
    if (expected == Expected::ParameterName) {
        // We have `...` in a closure argument, likely meant to be var-arg, which aren't
        // supported in closures (#146489).
        return mk_pat(Span(), PatKind::Err);
    }
    return mk_pat(Span(), PatKind::Rest);
}

/// Try to recover the more general form `intersect ::= $pat_lhs @ $pat_rhs`.
///
/// Allowed binding patterns generated by `binding ::= ref? mut? $ident @ $pat_rhs`
/// should already have been parsed by now at this point,
/// if the next token is `@` then we can try to parse the more general form.
///
/// Consult `parse_pat_ident` for the `binding` grammar.
///
/// The notion of intersection patterns are found in
/// e.g. [F#][and] where they are called AND-patterns.
///
/// [and]: https://docs.microsoft.com/en-us/dotnet/fsharp/language-reference/pattern-matching
Pat* Parser::recover_intersection_pat(Pat* lhs) {
    Pat* rhs = parse_pat_no_top_alt(Expected::None, PatternLocation::None);
    Span whole_span = lhs->span.to(rhs->span);

    if (rhs->kind == PatKind::Ident && rhs->sub == nullptr) {
        // The user inverted the order, so help them fix that.
        rhs->sub = lhs;
        emit("pattern on wrong side of `@`", whole_span);
    } else {
        // The special case above doesn't apply so we may have e.g. `A(x) @ B(y)`.
        rhs = mk_pat(whole_span, PatKind::Wild);
        emit("left-hand side of `@` must be a binding", whole_span);
    }
    rhs->span = whole_span;
    return rhs;
}

/// Ban a range pattern if it has an ambiguous interpretation.
void Parser::ban_pat_range_if_ambiguous(const Pat* pat) {
    if (pat->kind != PatKind::Range) return;
    if (pat->range_end == RangeEndKind::IncludedDotDotDot) return;
    emit("the range pattern here has ambiguous interpretation", pat->span);
}

/// Parse `&pat` / `&mut pat` / `&pin const pat` / `&pin mut pat`.
Pat* Parser::parse_pat_deref(Expected expected) {
    expect_and();
    if (tok_is_lifetime(token)) {
        std::string name(token.sym);
        bump();  // `'a`
        emit("unexpected lifetime `" + name + "` in pattern", prev_token.span);
    }

    std::pair<Pinnedness, Mutability> pm = parse_pin_and_mut();
    Pat* subpat = parse_pat_with_range_pat(false, expected, PatternLocation::None);
    Pat* p = mk_pat(Span(), PatKind::Ref);
    p->sub = subpat;
    p->pinned = pm.first;
    p->mutbl = pm.second;
    return p;
}

/// Parse a tuple or parenthesis pattern.
Pat* Parser::parse_pat_tuple_or_parens() {
    Span open_paren = token.span;
    Trailing trailing_comma = Trailing::No;
    std::vector<Pat*> fields = parse_paren_comma_seq<Pat*>(
        [this]() {
            return parse_pat_allow_top_guard(Expected::None, RecoverComma::No, RecoverColon::No,
                                             CommaRecoveryMode::LikelyTuple);
        },
        &trailing_comma);

    // Here, `(pat,)` is a tuple pattern.
    // For backward compatibility, `(..)` is a tuple pattern as well.
    bool paren_pattern = fields.size() == 1 && !(trailing_comma == Trailing::Yes || fields[0]->kind == PatKind::Rest);

    Pat* pat = nullptr;
    if (paren_pattern) {
        Pat* inner = fields[0];
        Span close_paren = prev_token.span;
        std::optional<std::pair<RangeEndKind, Span>> form;
        if (inner->kind == PatKind::Expr && may_recover() && (form = parse_range_end())) {
            // (pat) with optional parentheses are _not_ ambiguous: `(pat)..` / `(pat)..=`
            emit("range pattern bounds cannot have parentheses", open_paren);
            (void)close_paren;
            pat = parse_pat_range_begin_with(inner->expr, *form);
        } else if (inner->kind == PatKind::Err && may_recover() && (form = parse_range_end())) {
            // (pat) with optional parentheses are _not_ ambiguous: `(pat)..` / `(pat)..=`
            emit("range pattern bounds cannot have parentheses", open_paren);
            pat = parse_pat_range_begin_with(mk_expr_err(inner->span), *form);
        } else {
            pat = mk_pat(Span(), PatKind::Paren);
            pat->sub = inner;
        }
    } else {
        pat = mk_pat(Span(), PatKind::Tuple);
        pat->elems = std::move(fields);
    }

    if (maybe_recover_trailing_expr(open_paren.to(prev_token.span), false).has) return mk_pat(Span(), PatKind::Err);
    return pat;
}

/// Parse a mutable binding with the `mut` token already eaten.
Pat* Parser::parse_pat_ident_mut() {
    Span mut_span = prev_token.span;

    recover_additional_muts();

    ByRef byref = parse_byref();

    recover_additional_muts();

    // Parse the pattern we hope to be an identifier.
    Pat* pat = parse_pat_no_top_alt(Expected::Identifier, PatternLocation::None);

    // If we don't have `mut $ident (@ pat)?`, error.
    if (pat->kind == PatKind::Ident && !pat->by_ref && pat->mutbl == Mutability::Not) {
        // Mutable binding: `mut ident` / `mut ref ident`
        pat->by_ref = byref.yes;
        pat->by_ref_mutbl = byref.mutbl;
        pat->pinned = byref.pinned;
        pat->mutbl = Mutability::Mut;
    } else {
        // Add `mut` to any binding in the parsed pattern.
        bool changed_any_binding = make_all_value_bindings_mutable(pat);
        ban_mut_general_pat(mut_span, pat, changed_any_binding);
    }
    return pat;
}

/// Turn all by-value immutable bindings in a pattern into mutable bindings.
/// Returns `true` if any change was made.
bool Parser::make_all_value_bindings_mutable(Pat* pat) {
    bool changed = false;
    auto f = [&changed](Pat* p) {
        if (p->kind == PatKind::Ident && !p->by_ref && p->mutbl == Mutability::Not) {
            changed = true;
            p->mutbl = Mutability::Mut;
        }
        return true;
    };
    walk_pat(pat, f);
    return changed;
}

/// Error on `mut $pat` where `$pat` is not an ident.
void Parser::ban_mut_general_pat(Span lo, const Pat* pat, bool changed_any_binding) {
    if (changed_any_binding)
        emit("`mut` must be attached to each individual binding", lo.to(pat->span));
    else
        emit("`mut` must be followed by a named binding", lo.until(pat->span));
}

/// Eat any extraneous `mut`s and error + recover if we ate any.
void Parser::recover_additional_muts() {
    Span lo = token.span;
    while (eat_keyword(KW(Mut))) {
    }
    if (lo == token.span) return;
    Span span = lo.to(prev_token.span);
    emit("`mut` on a binding may not be repeated", span);
}

/// Parse macro invocation
Pat* Parser::parse_pat_mac_invoc(Path* path) {
    bump();
    MacCall* mac = mk<MacCall>();
    mac->path = path;
    mac->args = parse_delim_args();
    Pat* p = mk_pat(Span(), PatKind::MacCall);
    p->mac = mac;
    return p;
}

void Parser::fatal_unexpected_non_pat(Expected expected) {
    std::string msg = std::string("expected ") + expected_to_string_or_fallback(expected) + ", found " + token_descr(token);
    fail(msg, token.span);
}

/// Parses the range pattern end form `".." | "..." | "..=" ;`.
std::optional<std::pair<RangeEndKind, Span>> Parser::parse_range_end() {
    RangeEndKind re;
    if (eat(EXP(DotDotDot)))
        re = RangeEndKind::IncludedDotDotDot;
    else if (eat(EXP(DotDotEq)))
        re = RangeEndKind::IncludedDotDotEq;
    else if (eat(EXP(DotDot)))
        re = RangeEndKind::Excluded;
    else
        return std::nullopt;
    return std::make_pair(re, prev_token.span);
}

/// Parse a range pattern `$begin $form $end?` where `$form = ".." | "..." | "..=" ;`.
/// `$begin $form` has already been parsed.
Pat* Parser::parse_pat_range_begin_with(Expr* begin, std::pair<RangeEndKind, Span> re) {
    Expr* end = nullptr;
    if (is_pat_range_end_start(0)) {
        // Parsing e.g. `X..=Y`.
        end = parse_pat_range_end();
    } else {
        // Parsing e.g. `X..`.
        if (re.first != RangeEndKind::Excluded) {
            // FIXME(Centril): Consider semantic errors instead in `ast_validation`.
            inclusive_range_with_incorrect_end();
        }
    }
    Pat* p = mk_pat(Span(), PatKind::Range);
    p->range_lo = begin;
    p->range_hi = end;
    p->range_end = re.first;
    p->range_end_span = re.second;
    return p;
}

void Parser::inclusive_range_with_incorrect_end() {
    Span span = prev_token.span;
    // If the user typed "..==" instead of "..=", we want to give them
    // a specific error message telling them to use "..=".
    // If they typed "..=>", suggest they use ".. =>".
    // Otherwise, we assume that they meant to type a half open exclusive
    // range and give them an error telling them to do that instead.
    bool no_space = token.span.lo == span.hi;
    if (token.kind == Tok::Eq && no_space) {
        Span span_with_eq = span.to(token.span);

        // Ensure the user doesn't receive unhelpful unexpected token errors
        bump();
        if (is_pat_range_end_start(0)) {
            try {
                (void)parse_pat_range_end();
            } catch (PErr&) {
            }
        }
        emit("unexpected `=` after inclusive range", span_with_eq);
    } else if (token.kind == Tok::Gt && prev_token.kind == Tok::DotDotEq && no_space) {
        emit("unexpected `>` after inclusive range", token.span);
    } else {
        emit("inclusive range with no end", span);
    }
}

/// Parse a range-to pattern, `..X` or `..=X` where `X` remains to be parsed.
///
/// The form `...X` is prohibited to reduce confusion with the potential
/// expression syntax `...expr` for splatting in expressions.
Pat* Parser::parse_pat_range_to(std::pair<RangeEndKind, Span> re) {
    Expr* end = parse_pat_range_end();
    if (re.first == RangeEndKind::IncludedDotDotDot) {
        re.first = RangeEndKind::IncludedDotDotEq;
        emit("range-to patterns with `...` are not allowed", re.second);
    }
    Pat* p = mk_pat(Span(), PatKind::Range);
    p->range_hi = end;
    p->range_end = re.first;
    p->range_end_span = re.second;
    return p;
}

/// Is the token `dist` away from the current suitable as the start of a range patterns end?
bool Parser::is_pat_range_end_start(size_t dist) const {
    if (check_inline_const(dist)) return true;
    const Token& t = look_ahead_tok(dist);
    return tok_is_path_start(t)  // e.g. `MY_CONST`;
           || t.kind == Tok::Dot  // e.g. `.5` for recovery;
           || t.kind == Tok::Literal || t.kind == Tok::Minus || tok_is_bool_lit(t) ||
           tok_is_lifetime(t)  // recover `'a` instead of `'a'`
           || (may_recover()  // recover leading `(`
               && t.kind == Tok::OpenParen && look_ahead_tok(dist + 1).kind != Tok::OpenParen &&
               is_pat_range_end_start(dist + 1));
}

/// Parse a range pattern end bound
Expr* Parser::parse_pat_range_end() {
    // recover leading `(`
    OptSpan open_paren;
    if (may_recover() && eat_noexpect(Tok::OpenParen)) open_paren = OptSpan(prev_token.span);

    Expr* bound = nullptr;
    if (check_inline_const(0)) {
        bound = parse_const_block(token.span, true);
    } else if (check_path()) {
        Span lo = token.span;
        QSelf* qself = nullptr;
        Path* path = nullptr;
        if (eat_lt()) {
            // Parse a qualified path
            std::pair<QSelf*, Path*> qp = parse_qpath(PathStyle::Pat);
            qself = qp.first;
            path = qp.second;
        } else {
            // Parse an unqualified path
            path = parse_path(PathStyle::Pat);
        }
        Span hi = prev_token.span;
        bound = mk_expr(lo.to(hi), ExprKind::Path);
        bound->qself = qself;
        bound->path = path;
    } else {
        bound = parse_literal_maybe_minus();
    }

    OptSpan recovered = maybe_recover_trailing_expr(bound->span, true);

    // recover trailing `)`
    if (open_paren.has) {
        expect(EXP(CloseParen));
        emit("range pattern bounds cannot have parentheses", open_paren.span);
    }

    return recovered.has ? mk_expr_err(recovered.span) : bound;
}

/// Is this the start of a pattern beginning with a path?
bool Parser::is_start_of_pat_with_path() {
    return check_path()
           // Just for recovery (see `can_be_ident`).
           || (tok_is_ident(token) && !tok_is_bool_lit(token) && !tok_is_keyword(token, Kw::In));
}

/// Would `parse_pat_ident` be appropriate here?
bool Parser::can_be_ident_pat() {
    if (!check_ident()) return false;
    if (tok_is_bool_lit(token)) return false;            // Avoid `true` or `false` as a binding as it is a literal.
    if (tok_is_path_segment_keyword(token)) return false;  // Avoid e.g. `Self` as it is a path.
    // Avoid `in`. Due to recovery in the list parser this messes with `for ( $pat in $expr )`.
    if (tok_is_keyword(token, Kw::In)) return false;
    // Try to do something more complex?
    Tok k = look_ahead_tok(1).kind;
    return !(k == Tok::OpenParen     // A tuple struct pattern.
             || k == Tok::OpenBrace  // A struct pattern.
             || k == Tok::DotDotDot || k == Tok::DotDotEq || k == Tok::DotDot  // A range pattern.
             || k == Tok::PathSep  // A tuple / struct variant pattern.
             || k == Tok::Bang);   // A macro expanding to a pattern.
}

/// Parses `ident` or `ident @ pat`.
/// Used by the copy foo and ref foo patterns to give a good
/// error message when parsing mistakes like `ref foo(a, b)`.
Pat* Parser::parse_pat_ident(ByRef by_ref, Mutability mutbl, PatternLocation syntax_loc) {
    Ident ident = parse_ident_common(false);

    if (may_recover() && syntax_loc != PatternLocation::FunctionParameter && check_noexpect(Tok::Lt) &&
        look_ahead(1, [this](const Token& t) { return tok_can_begin_type(t); })) {
        fail("generic args in patterns require the turbofish syntax", token.span);
    }

    Pat* sub = nullptr;
    if (eat(EXP(At))) sub = parse_pat_no_top_alt(Expected::BindingPattern, PatternLocation::None);

    // Just to be friendly, if they write something like `ref Some(i)`,
    // we end up here with `(` as the current token.
    // This shortly leads to a parse error. Note that if there is no explicit
    // binding mode then we do not end up here, because the lookahead
    // will direct us over to `parse_enum_variant()`.
    if (token.kind == Tok::OpenParen) fail("expected identifier, found enum pattern", prev_token.span);

    // Check for method calls after the `ident`,
    // but not `ident @ subpat` as `subpat` was already checked and `ident` continues with `@`.
    if (sub == nullptr && maybe_recover_trailing_expr(ident.span, false).has) return mk_pat(Span(), PatKind::Err);

    Pat* p = mk_pat(Span(), PatKind::Ident);
    p->by_ref = by_ref.yes;
    p->by_ref_mutbl = by_ref.mutbl;
    p->pinned = by_ref.pinned;
    p->mutbl = mutbl;
    p->ident = ident;
    p->sub = sub;
    return p;
}

/// Parse a struct ("record") pattern (e.g. `Foo { ... }` or `Foo::Bar { ... }`).
Pat* Parser::parse_pat_struct(QSelf* qself, Path* path) {
    bump();
    Pat* p = mk_pat(Span(), PatKind::Struct);
    p->qself = qself;
    p->path = path;
    try {
        parse_pat_fields(p);
    } catch (PErr& e) {
        emit(e);
        recover_stmt();
        p->fields.clear();
        p->rest = Pat::Rest::Recovered;
    }
    bump();
    return p;
}

/// Parse tuple struct or tuple variant pattern (e.g. `Foo(...)` or `Foo::Bar(...)`).
Pat* Parser::parse_pat_tuple_struct(QSelf* qself, Path* path) {
    std::vector<Pat*> fields = parse_paren_comma_seq<Pat*>([this]() {
        return parse_pat_allow_top_guard(Expected::None, RecoverComma::No, RecoverColon::No,
                                         CommaRecoveryMode::EitherTupleOrPipe);
    });
    Pat* p = mk_pat(Span(), PatKind::TupleStruct);
    p->qself = qself;
    p->path = path;
    p->elems = std::move(fields);
    return p;
}

/// Are we sure this could not possibly be the start of a pattern?
///
/// Currently, this only accounts for tokens that can follow identifiers
/// in patterns, but this can be extended as necessary.
bool Parser::isnt_pattern_start() const {
    Tok k = token.kind;
    return k == Tok::Eq || k == Tok::Colon || k == Tok::Comma || k == Tok::Semi || k == Tok::At ||
           k == Tok::OpenBrace || k == Tok::CloseBrace || k == Tok::CloseParen;
}

Pat* Parser::parse_pat_builtin() {
    Pat* res = nullptr;
    parse_builtin([this, &res](Span lo, Ident ident) {
        (void)lo;
        if (ident.name == "deref") {
            Pat* sub = parse_pat_allow_top_guard(Expected::None, RecoverComma::Yes, RecoverColon::Yes,
                                                 CommaRecoveryMode::LikelyTuple);
            res = mk_pat(Span(), PatKind::Deref);
            res->sub = sub;
            return true;
        }
        return false;
    });
    return res;
}

/// Parses `box pat`
Pat* Parser::parse_pat_box() {
    Span box_span = prev_token.span;

    if (isnt_pattern_start()) {
        emit("expected pattern, found " + token_descr(token), token.span);

        // We cannot use `parse_pat_ident()` since it will complain `box`
        // is not an identifier.
        Pat* sub = nullptr;
        if (eat(EXP(At))) sub = parse_pat_no_top_alt(Expected::BindingPattern, PatternLocation::None);

        Pat* p = mk_pat(Span(), PatKind::Ident);
        p->ident.name = "box";
        p->ident.span = box_span;
        p->sub = sub;
        return p;
    }
    Pat* pat = parse_pat_with_range_pat(false, Expected::None, PatternLocation::None);
    Pat* p = mk_pat(Span(), PatKind::Box);
    p->sub = pat;
    return p;
}

/// Parses the fields of a struct-like pattern.
void Parser::parse_pat_fields(Pat* out) {
    std::vector<PatField> fields;
    Pat::Rest etc = Pat::Rest::None;
    Span etc_span;
    bool ate_comma = true;
    std::optional<PErr> delayed_err;

    while (token.kind != Tok::CloseBrace) {
        // check that a comma comes after every field
        if (!ate_comma) {
            PErr err;
            if (token.kind == Tok::At) {
                Ident prev_field = fields.back().ident;
                err = report_misplaced_at_in_struct_pat(prev_field);
            } else {
                err = PErr{"expected `,`", token.span};
            }
            if (delayed_err) emit(*delayed_err);
            throw err;
        }
        ate_comma = false;

        if (check(EXP(DotDot)) || check_noexpect(Tok::DotDotDot) || check_keyword(KW(Underscore))) {
            etc = Pat::Rest::Rest;
            etc_span = token.span;

            recover_bad_dot_dot();
            bump();  // `..` || `...` || `_`

            if (token.kind == Tok::CloseBrace) break;
            PErr err{"expected `}`, found " + token_descr(token), token.span};
            if (token.kind == Tok::Comma) {
                bump();
                ate_comma = true;
            }

            if (token.kind == Tok::CloseBrace) {
                // If the struct looks otherwise well formed, recover and continue.
                emit(err);
                break;
            } else if (tok_is_ident(token) && ate_comma) {
                // Accept fields coming after `..,`.
                // This way we avoid "pattern missing fields" errors afterwards.
                // We delay this error until the end in order to have a span for a
                // suggested fix.
                if (delayed_err) {
                    emit(*delayed_err);
                    throw err;
                }
                delayed_err = err;
            } else {
                if (delayed_err) emit(*delayed_err);
                throw err;
            }
        }

        AttrWrapper attrs;
        try {
            attrs = parse_outer_attributes();
        } catch (PErr&) {
            if (delayed_err) emit(*delayed_err);
            throw;
        }
        Span lo = token.span;
        PatField field;
        try {
            field = parse_pat_field(lo, std::move(attrs.attrs));
        } catch (PErr&) {
            if (delayed_err) {
                emit(*delayed_err);
                delayed_err.reset();
            }
            throw;
        }
        ate_comma = eat(EXP(Comma));
        fields.push_back(field);
    }

    if (delayed_err) emit(*delayed_err);
    out->fields = std::move(fields);
    out->rest = etc;
    out->rest_span = etc_span;
}

PErr Parser::report_misplaced_at_in_struct_pat(Ident prev_field) {
    Span span = prev_field.span.to(token.span);
    const Token& t = look_ahead_tok(1);
    if (t.kind == Tok::DotDot) return PErr{"`@ ..` is not supported in struct patterns", span.to(t.span)};
    return PErr{"unexpected `@` in struct pattern", span};
}

/// Recover on `...` or `_` as if it were `..` to avoid further errors.
/// See issue #46718.
void Parser::recover_bad_dot_dot() {
    if (token.kind == Tok::DotDot) return;
    emit("expected field pattern, found `" + token_str(token) + "`", token.span);
}

PatField Parser::parse_pat_field(Span lo, AttrVec attrs) {
    // Check if a colon exists one ahead. This means we're parsing a fieldname.
    Span hi;
    PatField f;
    if (look_ahead(1, [](const Token& t) { return t.kind == Tok::Colon; })) {
        // Parsing a pattern of the form `fieldname: pat`.
        Ident fieldname = parse_field_name();
        bump();
        Pat* pat = parse_pat_allow_top_guard(Expected::None, RecoverComma::No, RecoverColon::No,
                                             CommaRecoveryMode::EitherTupleOrPipe);
        hi = pat->span;
        f.ident = fieldname;
        f.pat = pat;
        f.is_shorthand = false;
    } else {
        // Parsing a pattern of the form `(box) (ref) (mut) fieldname`.
        bool is_box = eat_keyword(KW(Box));
        Span boxed_span = token.span;
        Mutability mutability = parse_mutability();
        ByRef by_ref = parse_byref();

        Ident fieldname = parse_ident_common(false);
        hi = prev_token.span;
        Pat* fieldpat = mk_pat(boxed_span.to(hi), PatKind::Ident);
        fieldpat->by_ref = by_ref.yes;
        fieldpat->by_ref_mutbl = by_ref.mutbl;
        fieldpat->pinned = by_ref.pinned;
        fieldpat->mutbl = mutability;
        fieldpat->ident = fieldname;
        Pat* subpat = fieldpat;
        if (is_box) {
            subpat = mk_pat(lo.to(hi), PatKind::Box);
            subpat->sub = fieldpat;
        }
        f.ident = fieldname;
        f.pat = subpat;
        f.is_shorthand = true;
    }
    f.attrs = std::move(attrs);
    f.span = lo.to(hi);
    return f;
}

// ---------------------------------------------------------------------------
// ast.rs helpers on patterns.

/// Pat::could_be_never_pattern
bool Parser::pat_could_be_never_pattern(Pat* pat) {
    bool could_be = false;
    auto visit = [&could_be](Pat* p) -> bool {
        if (p->kind == PatKind::Never || p->kind == PatKind::MacCall) {
            could_be = true;
            return false;
        }
        if (p->kind == PatKind::Or) {
            bool all = true;
            for (Pat* q : p->elems)
                if (!pat_could_be_never_pattern(q)) all = false;
            could_be = all;
            return false;
        }
        return true;
    };
    walk_pat(pat, visit);
    return could_be;
}

/// Pat::to_ty: attempt reparsing the pattern as a type.
Ty* Parser::pat_to_ty(const Pat* pat) {
    Ty* t = nullptr;
    switch (pat->kind) {
    case PatKind::Wild: t = mk_ty(pat->span, TyKind::Infer); break;
    case PatKind::Ident:
        if (pat->by_ref || pat->mutbl != Mutability::Not || pat->sub) return nullptr;
        t = mk_ty(pat->span, TyKind::Path);
        t->path = mk_path(pat->ident.span);
        {
            PathSegment s;
            s.ident = pat->ident;
            t->path->segments.push_back(s);
        }
        break;
    case PatKind::Path:
        t = mk_ty(pat->span, TyKind::Path);
        t->qself = pat->qself;
        t->path = pat->path;
        break;
    case PatKind::MacCall:
        t = mk_ty(pat->span, TyKind::MacCall);
        t->mac = pat->mac;
        break;
    case PatKind::Ref: {
        Ty* inner = pat->sub ? pat_to_ty(pat->sub) : nullptr;
        if (!inner) return nullptr;
        t = mk_ty(pat->span, pat->pinned == Pinnedness::Pinned ? TyKind::PinnedRef : TyKind::Ref);
        t->inner = inner;
        t->mutbl = pat->mutbl;
        break;
    }
    case PatKind::Slice: {
        if (pat->elems.size() != 1) return nullptr;
        Ty* inner = pat_to_ty(pat->elems[0]);
        if (!inner) return nullptr;
        t = mk_ty(pat->span, TyKind::Slice);
        t->inner = inner;
        break;
    }
    case PatKind::Tuple: {
        std::vector<Ty*> tys;
        for (Pat* q : pat->elems) {
            Ty* e = pat_to_ty(q);
            if (!e) return nullptr;
            tys.push_back(e);
        }
        t = mk_ty(pat->span, TyKind::Tup);
        t->elems = std::move(tys);
        break;
    }
    default: return nullptr;
    }
    return t;
}

// ---------------------------------------------------------------------------
// diagnostics.rs: pattern recovery.

Pat* Parser::maybe_recover_from_bad_qpath_pat(Pat* pat) {
    if (may_recover() && token.kind == Tok::PathSep) {
        if (Ty* ty = pat_to_ty(pat)) {
            std::pair<QSelf*, Path*> qp = maybe_recover_from_bad_qpath_stage_2(ty->span, ty, PathStyle::Pat);
            Pat* p = mk_pat(qp.second->span, PatKind::Path);
            p->qself = qp.first;
            p->path = qp.second;
            return p;
        }
    }
    return pat;
}

Pat* Parser::recover_colon_colon_in_pat_typo(Pat* first_pat, Expected expected) {
    if (token.kind != Tok::Colon) return first_pat;
    // Look for `Foo:bar` and recover as `Foo::bar`.
    ParserState snapshot_pat = create_snapshot_for_diagnostic();
    ParserState snapshot_type = create_snapshot_for_diagnostic();
    PErr err;
    try {
        expected_one_of_not_found({}, {});
        return first_pat;  // Ok(_): nothing to recover
    } catch (PErr& e) {
        err = e;
    }
    ParserState cur = st_;
    ParserState final_state = cur;

    // Try parsing the pattern after the colon on the pattern snapshot.
    st_ = snapshot_pat;
    bump();
    Pat* pat = nullptr;
    try {
        pat = parse_pat_no_top_alt(expected, PatternLocation::None);
    } catch (PErr&) {
        pat = nullptr;
    }
    if (pat) {
        Span new_span = first_pat->span.to(pat->span);
        bool show_sugg = false;
        if ((pat->kind == PatKind::Struct || pat->kind == PatKind::TupleStruct || pat->kind == PatKind::Path) &&
            pat->qself == nullptr) {
            if (first_pat->kind == PatKind::Ident) {
                PathSegment s;
                s.ident = first_pat->ident;
                pat->path->segments.insert(pat->path->segments.begin(), s);
                pat->path->span = new_span;
                show_sugg = true;
                first_pat = pat;
            } else if (first_pat->kind == PatKind::Path) {
                std::vector<PathSegment> segs = first_pat->path->segments;
                segs.insert(segs.end(), pat->path->segments.begin(), pat->path->segments.end());
                pat->path->segments = std::move(segs);
                pat->path->span = new_span;
                pat->qself = first_pat->qself;
                first_pat = pat;
                show_sugg = true;
            }
        } else if (pat->kind == PatKind::Ident && !pat->by_ref && pat->mutbl == Mutability::Not && pat->sub == nullptr) {
            if (first_pat->kind == PatKind::Ident) {
                Pat* p = mk_pat(new_span, PatKind::Path);
                p->path = mk_path(new_span);
                PathSegment a, b;
                a.ident = first_pat->ident;
                b.ident = pat->ident;
                p->path->segments.push_back(a);
                p->path->segments.push_back(b);
                first_pat = p;
                show_sugg = true;
            } else if (first_pat->kind == PatKind::Path) {
                Pat* p = mk_pat(new_span, PatKind::Path);
                p->qself = first_pat->qself;
                p->path = mk_path(new_span);
                p->path->segments = first_pat->path->segments;
                PathSegment b;
                b.ident = pat->ident;
                p->path->segments.push_back(b);
                first_pat = p;
                show_sugg = true;
            }
        }
        if (!show_sugg) {
            // Carry on as if we had not done anything. This should be unreachable.
            first_pat = mk_pat(new_span, PatKind::Err);
        }
        final_state = st_;  // restore_snapshot(snapshot_pat)
    }

    // Try parsing a type after the colon on the type snapshot.
    st_ = snapshot_type;
    bump();
    Ty* ty = nullptr;
    try {
        ty = parse_ty();
    } catch (PErr&) {
        ty = nullptr;
    }
    if (ty) {
        final_state = st_;  // restore_snapshot(snapshot_type)
        Span new_span = first_pat->span.to(ty->span);
        first_pat = mk_pat(new_span, PatKind::Err);
    }
    st_ = final_state;
    emit(err);
    return first_pat;
}

/// Some special error handling for the "top-level" patterns in a match arm,
/// `for` loop, `let`, &c. (in contrast to subpatterns within such).
void Parser::maybe_recover_unexpected_comma(Span lo, CommaRecoveryMode rt) {
    if (token.kind != Tok::Comma) return;
    recover_unexpected_comma(lo, rt);
}

void Parser::recover_unexpected_comma(Span lo, CommaRecoveryMode rt) {
    (void)lo;
    (void)rt;
    // An unexpected comma after a top-level pattern is a clue that the
    // user (perhaps more accustomed to some other language) forgot the
    // parentheses in what should have been a tuple pattern; return a
    // suggestion-enhanced error here rather than choking on the comma later.
    Span comma_span = token.span;
    bump();
    try {
        skip_pat_list();
    } catch (PErr&) {
        // We didn't expect this to work anyway; we just wanted to advance to the
        // end of the comma-sequence so we know the span to suggest parenthesizing.
    }
    fail("unexpected `,` in pattern", comma_span);
}

void Parser::skip_pat_list() {
    while (!check(EXP(CloseParen))) {
        (void)parse_pat_no_top_alt(Expected::None, PatternLocation::None);
        if (!eat(EXP(Comma))) return;
    }
}

}  // namespace detail
}  // namespace rust
