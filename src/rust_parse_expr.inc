// rust_parse_expr.inc -- Parser members defined in rust_parse_expr.cpp
// (rustc_parse/src/parser/expr.rs, plus the expression-related parts of
// diagnostics.rs). Included inside `class Parser`.
//
// ---- used by other files: keep these signatures ----
/// check_assoc_op: the operator at the current token, if any.
std::optional<AssocOp> check_assoc_op();
Expr* parse_expr();
Expr* parse_expr_force_collect();
/// parse_expr_anon_const(|_, _| MgcaDisambiguation::Direct / AnonConst)
AnonConst parse_expr_anon_const(bool direct);
/// parse_expr_res: (expr, is_assoc_op_parsed)
std::pair<Expr*, bool> parse_expr_res(Restrictions r, AttrWrapper attrs);
std::pair<Expr*, bool> parse_expr_assoc_rest_with(PrecBound min_prec, bool starts_stmt, Expr* lhs);
Expr* parse_expr_dot_or_call_with(AttrVec attrs, Expr* e, Span lo);
Expr* parse_expr_labeled(Label label, bool consume_colon);
Expr* parse_expr_block(std::optional<Label> opt_label, Span lo, BlockCheckMode blk_mode);
bool is_async_gen_block() const;
Expr* parse_literal_maybe_minus();
/// recover_unclosed_char(ident, mk_lit_char, err): emits the error (or the
/// stolen stashed one) and returns the char literal expression it recovers to.
Expr* recover_unclosed_char_expr(Ident lifetime_ident, const std::string& err_msg, Span err_span);
std::vector<Ident> parse_floating_field_access();
/// parse_str_lit: Ok(StrLit) -> true and `out` filled; Err(Some(lit)) -> false and *non_str set; Err(None) -> false.
bool parse_str_lit(StrLit& out, Lit* non_str_lit, bool* had_lit);
/// parse_meta_item_lit (for attributes): the literal expression.
Lit parse_meta_item_lit();
/// eat_metavar_guard: always None outside macro expansion.
Guard* eat_metavar_guard() { return nullptr; }
// diagnostics.rs pieces owned by expr.cpp and used elsewhere:
PErr expected_expression_found();
void maybe_recover_unexpected_block_label();
void maybe_recover_from_ternary_operator(OptSpan cond);
bool is_vcs_conflict_marker(Tok long_kind, Tok short_kind) const;
void recover_vcs_conflict_marker();
void recover_stmt();
void recover_stmt_(SemiColonMode break_on_semi, BlockMode break_on_block);

// ---- private helpers of rust_parse_expr.cpp ----
/// expr.rs DestructuredFloat (break_up_float's result).
struct DestructuredFloat {
    enum Kind { Single, TrailingDot, MiddleDot, Error } kind = Error;
    std::string_view sym1, sym2;
    Span span1, dot_span, span2;
};
/// parse_struct_fields: (fields, StructRest, recovered async block in 2015).
struct StructFields {
    std::vector<ExprField> fields;
    StructRestKind rest = StructRestKind::None;
    Expr* base = nullptr;   // StructRest::Base
    Span rest_span;         // StructRest::Rest (the `..`), Base (the `..` too)
    bool recovered_async = false;
};
// expr.rs
Expr* parse_expr_catch_underscore(Restrictions restrictions);
std::vector<Expr*> parse_expr_paren_seq();
std::pair<Expr*, bool> parse_expr_assoc_with(PrecBound min_prec, AttrWrapper attrs);
bool should_continue_as_assoc_expr(Expr* lhs);
void error_found_expr_would_be_stmt(Expr* lhs);
bool expr_is_complete(const Expr* e) const;
Expr* parse_expr_range(ExprPrecedence prec, Expr* lhs, RangeLimits limits, Span cur_op_span);
bool is_at_start_of_range_notation_rhs() const;
Expr* parse_expr_prefix_range(AttrWrapper attrs);
Expr* parse_expr_prefix(AttrWrapper attrs);
std::pair<Span, Expr*> parse_expr_prefix_common(Span lo);
Expr* parse_expr_unary(Span lo, UnOp op);
Expr* recover_tilde_expr(Span lo);
Expr* parse_expr_box(Span box_kw);
Expr* parse_expr_move(Span move_kw);
bool is_mistaken_not_ident_negation() const;
Expr* recover_not_expr(Span lo);
Expr* parse_assoc_op_cast(Expr* lhs, Span lhs_span, Span op_span, ExprKind expr_kind);
std::string path_to_string(const Path* path) const;
Expr* parse_expr_borrow(Span lo);
std::pair<BorrowKind, Mutability> parse_borrow_modifiers();
Expr* parse_expr_dot_or_call(AttrWrapper attrs);
/// parse_dot_suffix_expr: after the `.` (stmt.rs uses it too).
Expr* parse_dot_suffix_expr(Span lo, Expr* base);
void error_unexpected_after_dot();
DestructuredFloat break_up_float(std::string_view float_str, Span span);
Expr* mk_expr_tuple_field_access(Span lo, Span ident_span, Expr* base, std::string_view field, std::string_view suffix);
Expr* parse_expr_fn_call(Span lo, Expr* fun);
Expr* parse_expr_index(Span lo, Expr* base);
Expr* parse_dot_suffix(Expr* self_arg, Span lo);
Expr* parse_expr_bottom();
Expr* parse_expr_lit();
Expr* parse_expr_tuple_parens(Restrictions restrictions);
Expr* parse_expr_array_or_repeat(ExpTok close);
Expr* parse_expr_path_start();
/// mk_token_lit_char(name, span) for a `'a` that was meant as a char literal.
Lit mk_token_lit_char(Ident lifetime_ident);
Expr* recover_do_catch();
Expr* parse_expr_opt();
Expr* parse_expr_return();
Expr* parse_expr_yeet();
Expr* parse_expr_become();
Expr* parse_expr_break();
Expr* parse_expr_continue(Span lo);
Expr* parse_expr_yield();
Expr* parse_expr_builtin();
Expr* parse_expr_offset_of(Span lo);
Expr* parse_expr_type_ascribe(Span lo);
Expr* parse_expr_unsafe_binder_cast(Span lo, UnsafeBinderCastKind kind);
Lit handle_missing_lit();
Lit parse_token_lit();
void recover_after_dot();
bool eat_token_lit(Lit& out);
bool parse_opt_token_lit(Lit& out);
bool parse_opt_meta_item_lit(Lit& out);
bool is_array_like_block();
Expr* maybe_suggest_brackets_instead_of_braces(Span lo);
void suggest_missing_semicolon_before_array(Span prev_span, Span open_delim_span);
Expr* parse_simple_block();
bool enclosing_delim_is_paren() const;
Expr* parse_expr_closure();
Expr* parse_closure_block_body(Span ret_span);
CaptureBy parse_capture_clause(Span& kw_span);
FnDecl* parse_fn_block_decl(Span& arg_span);
Param* parse_fn_block_param();
Expr* parse_expr_if();
Expr* parse_if_after_cond(Span lo, Expr* cond);
Expr* parse_expr_cond(LetChainsPolicy let_chains_policy);
Expr* parse_expr_let(Restrictions restrictions);
Expr* parse_expr_else();
void error_on_if_block_attrs(Span ctx_span, bool is_ctx_else, Span branch_span, const AttrWrapper& attrs);
void error_on_extra_if(const Expr* cond);
std::pair<Pat*, Expr*> parse_for_head();
Expr* parse_expr_for(std::optional<Label> opt_label, Span lo);
void recover_loop_else(const char* loop_kind, Span loop_kw);
void error_missing_in_for_loop();
Expr* parse_expr_while(std::optional<Label> opt_label, Span lo);
Expr* parse_expr_loop(std::optional<Label> opt_label, Span lo);
std::optional<Label> eat_label();
Expr* parse_expr_match();
Expr* parse_match_block(Span lo, Span match_span, Expr* scrutinee, MatchKind match_kind);
std::optional<Span> parse_arm_body_missing_braces(Expr* first_expr, Span arrow_span);
Arm parse_arm();
Guard* parse_match_arm_guard();
Guard* expect_match_arm_guard_cond();
std::pair<Pat*, Guard*> parse_match_arm_pat_and_guard();
Expr* parse_match_guard_condition();
Expr* parse_try_block(Span span_lo);
bool is_do_catch_block() const;
bool is_do_yeet() const;
bool is_try_block() const;
Expr* parse_gen_block();
bool is_gen_block(Kw kw, size_t lookahead) const;
bool is_likely_struct_lit() const;
/// maybe_parse_struct_expr: `some` = Some(..) (the result, or a thrown error), else None.
Expr* maybe_parse_struct_expr(QSelf* qself, Path* path, bool& some);
Expr* maybe_recover_bad_struct_literal_path(bool is_underscore_entry_point);
StructFields parse_struct_fields(Path* pth, bool recover, ExpTok close);
Expr* parse_expr_struct(QSelf* qself, Path* pth, bool recover);
void recover_struct_comma_after_dotdot(Span span);
bool recover_struct_field_dots(Tok close);
Label recover_ident_into_label(Ident ident);
ExprField parse_expr_field();
void error_on_eq_field_init(Ident field_name);
void err_dotdotdot_syntax(Span span);
void err_larrow_operator(Span span);
/// mk_range + mk_expr: an Err expression for `a..=` without an end.
Expr* mk_range(Span span, Expr* start, Expr* end, RangeLimits limits);
Expr* mk_await_expr(Expr* self_arg, Span lo);
Expr* mk_use_expr(Expr* self_arg, Span lo);
Span mk_expr_sp(const Expr* lhs, Span lhs_span, Span op_span, Span rhs_span) const;
// diagnostics.rs
/// maybe_recover_unexpected_block_label(loop_header) -> bool (the public void
/// overload above drops the result).
bool try_recover_unexpected_block_label(OptSpan loop_header);
/// conflict_marker(long, short, expected): expected 0 = None.
bool conflict_marker(Tok long_kind, Tok short_kind, size_t expected, Span& out_span, size_t& out_len);
/// check_no_chained_comparison: nullptr = Ok(None).
Expr* check_no_chained_comparison(Expr* inner_op, const AssocOp& outer_op);
bool attempt_chained_comparison_suggestion(Expr* inner_op, const AssocOp& outer_op);
void consume_tts(int64_t acc, std::initializer_list<std::pair<Tok, int64_t>> modifier);
bool consume_fn_args();
void check_turbofish_missing_angle_brackets(PathSegment& segment);
void check_for_for_in_in_typo(Span in_span);
Expr* recover_closure_body(const PErr& err, const Token& before, const Token& prev, const Token& tok, Span lo, Span decl_hi);
void eat_to_tokens(std::initializer_list<ExpTok> closes);
void recover_from_await_method_call();
void recover_from_use();
/// recover_from_postfix_increment / _decrement / recover_from_prefix_increment: always throws.
[[noreturn]] Expr* recover_from_inc_dec(Expr* base, bool is_inc, bool is_pre, Span op_span, bool starts_stmt);
Expr* recover_seq_parse_error(ExpTok open, ExpTok close, Span lo, const PErr& err);
/// consume_block(open, close, ConsumeClosingDelim::Yes -> true)
void consume_block(ExpTok open, ExpTok close, bool consume_close);
Expr* try_macro_suggestion();
Expr* recover_incorrect_await_syntax(Span await_sp);
/// Expr::to_ty (for the bad-qpath recovery); nullptr = None.
Ty* expr_to_ty(const Expr* e);
Expr* maybe_recover_from_bad_qpath(Expr* base);
