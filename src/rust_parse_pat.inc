// rust_parse_pat.inc -- Parser members defined in rust_parse_pat.cpp
// (rustc_parse/src/parser/pat.rs), rust_parse_ty.cpp (ty.rs),
// rust_parse_path.cpp (path.rs) and rust_parse_generics.cpp (generics.rs),
// plus the parts of diagnostics.rs they use. Included inside `class Parser`.
//
// ---- used by other files: keep these signatures ----
// path.rs
Path* parse_path(PathStyle style);
PathSegment parse_path_segment(PathStyle style, const Generics* ty_generics);
std::pair<QSelf*, Path*> parse_qpath(PathStyle style);
Path* parse_path_inner(PathStyle style, const Generics* ty_generics);
void parse_path_segments(std::vector<PathSegment>& segments, PathStyle style, const Generics* ty_generics);
Ident parse_path_segment_ident();
bool eat_path_sep();
AnonConst parse_const_arg();
// pat.rs
void inclusive_range_with_incorrect_end();
Pat* parse_pat_allow_top_guard(Expected expected, RecoverComma rc, RecoverColon ra, CommaRecoveryMode rt);
Pat* parse_pat_no_top_alt(Expected expected, PatternLocation syntax_loc);
Pat* parse_pat_no_top_guard(Expected expected, RecoverComma rc, RecoverColon ra, CommaRecoveryMode rt);
/// parse_pat_before_ty: (pat, trailing_vert)
std::pair<Pat*, bool> parse_pat_before_ty(Expected expected, RecoverComma rc, PatternLocation syntax_loc);
// ty.rs
Lifetime expect_lifetime();
bool check_lifetime();
Ty* parse_ty();
Ty* parse_as_cast_ty();
Ty* parse_ty_no_question_mark_recover();
Ty* parse_ty_with_generics_recovery(const Generics& ty_params);
Ty* parse_ty_for_where_clause();
/// parse_higher_ranked_binder: (params, span of `for<..>` if present)
std::pair<std::vector<GenericParam*>, OptSpan> parse_higher_ranked_binder();
std::pair<Pinnedness, Mutability> parse_pin_and_mut();
FnRetTy parse_ret_ty(AllowPlus allow_plus, RecoverQPath recover_qpath, RecoverReturnSign recover_return_sign);
GenericBounds parse_generic_bounds();
// generics.rs
Generics parse_generics();
std::vector<GenericParam*> parse_generic_params();
WhereClause parse_where_clause();
/// parse_struct_where_clause: (where clause, tuple-struct fields recovered from `struct S where T: X (..)`)
std::pair<WhereClause, std::optional<std::vector<FieldDef*>>> parse_struct_where_clause(Ident struct_name, Span body_insertion_point);
bool choose_generics_over_qpath(size_t start) const;
FnContract* parse_contract();
// diagnostics.rs pieces owned by these files and used elsewhere:
/// check_trailing_angle_brackets: true when it emitted (Some(guar)).
bool check_trailing_angle_brackets(const PathSegment& segment, std::initializer_list<ExpTok> end);
/// check_mistyped_turbofish_with_multiple_type_params(e, expr): recovers (possibly replacing `expr`) or throws.
void check_mistyped_turbofish_with_multiple_type_params(PErr e, Expr*& expr);

// ---- private helpers of rust_parse_pat.cpp / rust_parse_ty.cpp / rust_parse_path.cpp / rust_parse_generics.cpp ----
// path.rs
static Span generic_arg_span(const GenericArg& a);
static Span angle_arg_span(const AngleBracketedArg& a);
static Span path_segment_span(const PathSegment& s);
bool recover_colon_before_qpath_proj();
Ty* parse_path_paren_arg();
void recover_fn_call_leading_path_sep(PathStyle style);
std::vector<AngleBracketedArg> parse_angle_args_with_leading_angle_bracket_recovery(PathStyle style, Span lo,
                                                                                 const Generics* ty_generics);
std::vector<AngleBracketedArg> parse_angle_args(const Generics* ty_generics);
/// parse_angle_arg: Ok(None) -> false.
bool parse_angle_arg(const Generics* ty_generics, AngleBracketedArg& out);
void parse_assoc_equality_term(Ident ident, GenericArgs* gen_args, AssocItemConstraint* c);
bool expr_is_valid_const_arg(const Expr* expr) const;
AnonConst parse_unambiguous_unbraced_const_arg();
std::optional<GenericArg> parse_generic_arg(const Generics* ty_generics);
bool get_ident_from_generic_arg(const GenericArg& gen_arg, bool& binder, Ident& ident, GenericArgs*& args);
// diagnostics.rs (const arguments, trailing angle brackets)
bool handle_ambiguous_unbraced_const_arg(std::vector<AngleBracketedArg>& args);
std::optional<GenericArg> recover_const_param_decl(const Generics* ty_generics);
std::optional<GenericArg> recover_const_param_declaration(const Generics* ty_generics);
bool assoc_op_from_token_is_op(const Token& t);
GenericArg recover_const_arg(Span start, PErr err);
Expr* recover_unbraced_const_arg_that_can_begin_ty(const ParserState& snapshot);
GenericArg dummy_const_arg_needs_braces(PErr err, Span span);
// generics.rs
GenericBounds parse_lt_param_bounds();
GenericParam* parse_ty_param(AttrVec preceding_attrs);
GenericParam* parse_const_param(AttrVec preceding_attrs);
GenericParam* recover_const_param_with_mistyped_const(AttrVec preceding_attrs, Ident mistyped_const_ident);
void expect_gt_or_maybe_suggest_closing_generics(const std::vector<GenericParam*>& params);
std::pair<WhereClause, std::optional<std::vector<FieldDef*>>> parse_where_clause_common(bool has_struct, Ident struct_name,
                                                                                       Span body_insertion_point);
WherePredicate* parse_ty_where_predicate_kind_or_recover_tuple_struct_body(bool has_struct, Ident struct_name,
                                                                           Span body_insertion_point, Span pred_lo,
                                                                           Span where_sp,
                                                                           std::optional<std::vector<FieldDef*>>& body);
WherePredicate* parse_ty_where_predicate_kind();
// ty.rs (each TyKind-returning rustc function returns a Ty* holding the kind;
// parse_ty_common sets its span)
bool can_begin_dyn_bound_in_edition_2015(const Token& t) const;
/// parse_ty_for_param: allows a top-level `...` (used by item.rs's parse_param_general).
Ty* parse_ty_for_param();
/// parse_ty_no_plus: `&'a T`, `x as T` contexts.
Ty* parse_ty_no_plus();
Ty* parse_ty_common(AllowPlus allow_plus, AllowCVariadic allow_c_variadic, RecoverQPath recover_qpath,
                    RecoverReturnSign recover_return_sign, const Generics* ty_generics, bool recover_question_mark);
Ty* parse_unsafe_binder_ty();
Ty* parse_ty_tuple_or_parens(Span lo, AllowPlus allow_plus);
Ty* parse_bare_trait_object(Span lo, AllowPlus allow_plus);
Ty* maybe_recover_ref_ty_no_leading_ampersand(Lifetime lt, Span lo, const PErr& err);
Ty* parse_remaining_bounds_path(std::vector<GenericParam*> generic_params, Path* path, Span lo, bool parse_plus,
                                bool parens);
Ty* parse_remaining_bounds(GenericBounds bounds, bool plus);
Ty* parse_ty_c_style_pointer();
Ty* parse_ty_ptr();
Ty* parse_array_or_slice_ty();
Ty* maybe_recover_array_ty_without_semi(Ty* elt_ty);
Ty* parse_borrowed_pointee();
Ty* parse_typeof_ty(Span lo);
Ty* parse_builtin_ty();
Ty* parse_ty_field_of(Span lo);
Ty* parse_ty_fn_ptr(Span lo, std::vector<GenericParam*> params, OptSpan param_insertion_point,
                    RecoverReturnSign recover_return_sign);
void recover_fn_ptr_with_generics(Span lo, std::vector<GenericParam*>& params, OptSpan param_insertion_point);
Ty* parse_impl_ty(bool& impl_dyn_multi);
GenericBound parse_use_bound(Span lo, bool parens);
bool is_explicit_dyn_type();
Ty* parse_dyn_ty(bool& impl_dyn_multi);
Ty* parse_path_start_ty(Span lo, AllowPlus allow_plus, const Generics* ty_generics);
GenericBounds parse_generic_bounds_common(AllowPlus allow_plus);
bool can_begin_bound();
bool can_begin_maybe_const_bound();
GenericBound parse_generic_bound();
GenericBound parse_lifetime_bound(Span lo, bool parens);
void report_parenthesized_bound(Span lo, Span hi, const char* kind);
void error_lt_bound_with_modifiers(const TraitBoundModifiers& modifiers, OptSpan binder_span);
TraitBoundModifiers parse_trait_bound_modifiers();
void parse_bound_constness(TraitBoundModifiers& m);
GenericBound parse_trait_bound(Span lo, bool parens, const Token& leading_token);
Path* recover_path_from_fn();
void recover_fn_trait_with_lifetime_params(Path* fn_path, std::vector<GenericParam*>& lifetime_defs);
// diagnostics.rs (types)
void maybe_report_ambiguous_plus(bool impl_dyn_multi, const Ty* ty);
Ty* maybe_recover_from_question_mark(Ty* ty);
void maybe_recover_from_bad_type_plus(const Ty* ty);
/// maybe_recover_from_bad_qpath for a type (`Vec<T>::x` -> `<Vec<T>>::x`).
Ty* maybe_recover_from_bad_qpath_ty(Ty* ty);
/// maybe_recover_from_bad_qpath_stage_2: parses the `::seg..` tail of `ty` and
/// returns the (qself, path) of `<ty>::seg..` (expr.rs builds its ExprKind::Path from it).
std::pair<QSelf*, Path*> maybe_recover_from_bad_qpath_stage_2(Span ty_span, Ty* ty, PathStyle style);
void maybe_recover_bounds_doubled_colon(const Ty* ty);
// pat.rs
enum class EatOrResult : uint8_t { TrailingVert, AteOr, None };
/// Spans of the "found an expression" errors stashed in late_ (StashKey::ExprInPat).
std::vector<Span> expr_in_pat_stash_;
std::pair<Pat*, bool> parse_pat_no_top_guard_inner(Expected expected, RecoverComma rc, RecoverColon ra,
                                                   CommaRecoveryMode rt, PatternLocation syntax_loc);
/// parse_fn_param_pat_colon: (pat, ate a `:`) -- used by item.rs's parse_param_general.
std::pair<Pat*, bool> parse_fn_param_pat_colon();
EatOrResult eat_or_separator(OptSpan lo);
bool recover_trailing_vert(OptSpan lo);
OptSpan maybe_recover_trailing_expr(Span pat_span, bool is_end_bound);
/// Called by stmt.rs's parse_stmt_without_recovery on each statement it returns.
void maybe_augment_stashed_expr_in_pats_with_suggestions(const Stmt* stmt);
Pat* parse_pat_with_range_pat(bool allow_range_pat, Expected expected, PatternLocation syntax_loc);
Pat* recover_dotdotdot_rest_pat(Span lo, Expected expected);
Pat* recover_intersection_pat(Pat* lhs);
void ban_pat_range_if_ambiguous(const Pat* pat);
Pat* parse_pat_deref(Expected expected);
Pat* parse_pat_tuple_or_parens();
Pat* parse_pat_ident_mut();
bool make_all_value_bindings_mutable(Pat* pat);
void ban_mut_general_pat(Span lo, const Pat* pat, bool changed_any_binding);
void recover_additional_muts();
Pat* parse_pat_mac_invoc(Path* path);
[[noreturn]] void fatal_unexpected_non_pat(Expected expected);
std::optional<std::pair<RangeEndKind, Span>> parse_range_end();
Pat* parse_pat_range_begin_with(Expr* begin, std::pair<RangeEndKind, Span> re);
Pat* parse_pat_range_to(std::pair<RangeEndKind, Span> re);
bool is_pat_range_end_start(size_t dist) const;
Expr* parse_pat_range_end();
bool is_start_of_pat_with_path();
bool can_be_ident_pat();
Pat* parse_pat_ident(ByRef by_ref, Mutability mutbl, PatternLocation syntax_loc);
Pat* parse_pat_struct(QSelf* qself, Path* path);
Pat* parse_pat_tuple_struct(QSelf* qself, Path* path);
bool isnt_pattern_start() const;
Pat* parse_pat_builtin();
Pat* parse_pat_box();
void parse_pat_fields(Pat* out);
PErr report_misplaced_at_in_struct_pat(Ident prev_field);
void recover_bad_dot_dot();
PatField parse_pat_field(Span lo, AttrVec attrs);
static bool pat_could_be_never_pattern(Pat* pat);
Ty* pat_to_ty(const Pat* pat);
// diagnostics.rs (patterns)
/// maybe_recover_from_bad_qpath for a pattern.
Pat* maybe_recover_from_bad_qpath_pat(Pat* pat);
Pat* recover_colon_colon_in_pat_typo(Pat* first_pat, Expected expected);
void maybe_recover_unexpected_comma(Span lo, CommaRecoveryMode rt);
[[noreturn]] void recover_unexpected_comma(Span lo, CommaRecoveryMode rt);
void skip_pat_list();
