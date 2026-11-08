// rust_parse_item.inc -- Parser members defined in rust_parse_item.cpp
// (rustc_parse/src/parser/item.rs), rust_parse_stmt.cpp (stmt.rs) and
// rust_parse_attr.cpp (attr.rs), plus the parts of diagnostics.rs they use.
// Included inside `class Parser`.
//
// ---- used by other files: keep these signatures ----
// attr.rs
AttrVec parse_inner_attributes();
AttrWrapper parse_outer_attributes();
void error_on_forbidden_inner_attr(Span attr_sp, InnerAttrPolicy policy, bool suggest_to_outer);
// stmt.rs
Block* parse_block();
/// parse_block_common: (inner attrs, block)
std::pair<AttrVec, Block*> parse_block_common(Span lo, BlockCheckMode blk_mode, OptSpan loop_header);
std::pair<AttrVec, Block*> parse_inner_attrs_and_block(OptSpan loop_header);
/// nullptr = Ok(None)
Stmt* parse_stmt_without_recovery(bool capture_semi, ForceCollect force_collect, bool force_full_expr);
Stmt* parse_full_stmt(AttemptLocalParseRecovery recover);
// item.rs
Crate* parse_crate_mod();
/// parse_mod(term): (inner attrs, items, inner span)
struct ModParse {
    AttrVec attrs;
    std::vector<Item*> items;
    Span inner_span;
};
ModParse parse_mod(ExpTok term);
/// nullptr = Ok(None)
Item* parse_item(ForceCollect force_collect, AllowConstBlockItems allow_const_block_items);
Item* parse_item_common(AttrWrapper attrs, bool mac_allowed, bool attrs_allowed, FnParseMode fn_parse_mode,
                        ForceCollect force_collect, AllowConstBlockItems allow_const_block_items);
bool is_path_start_item();
bool check_fn_front_matter(bool check_pub, Case c);
FnHeader parse_fn_front_matter(const Visibility& orig_vis, Case c, FrontMatterParsingMode parsing_mode);
FnDecl* parse_fn_decl(const FnParseMode& fn_parse_mode, AllowPlus ret_allow_plus, RecoverReturnSign recover_return_sign);
std::vector<Param*> parse_fn_params(const FnParseMode& fn_parse_mode);
// diagnostics.rs pieces owned by item/stmt and used elsewhere:
void expect_semi();
bool recover_colon_as_semi();

// ---- private helpers of rust_parse_item.cpp / rust_parse_stmt.cpp / rust_parse_attr.cpp ----
// attr.rs
Attribute mk_doc_comment_attr(const Token& doc_tok, AttrStyle style);
Attribute parse_attribute(InnerAttrPolicy inner_parse_policy);
void parse_attr_item(Attribute& out);
// stmt.rs
Stmt* parse_stmt(ForceCollect force_collect);
Stmt* parse_stmt_path_start(Span lo, AttrWrapper attrs);
Stmt* parse_stmt_mac(Span lo, AttrVec attrs, Path* path);
Expr* maybe_recover_from_bad_qpath_mac_expr(Expr* base);
void error_outer_attrs(const AttrWrapper& attrs);
Stmt* recover_stmt_local_after_let(Span lo, AttrWrapper attrs);
Local* parse_local(OptSpan super_, AttrVec attrs);
void check_let_else_init_bool_expr(const Expr* init);
void check_let_else_init_trailing_brace(const Expr* init);
Expr* parse_initializer(bool eq_optional);
bool is_line_before_span_empty(Span sp) const;
PErr error_block_no_opening_brace_msg(std::string msg);
[[noreturn]] void error_block_no_opening_brace();
bool maybe_suggest_struct_literal(Span lo, BlockCheckMode s, const Token& maybe_struct_name, Block*& tail);
Block* parse_block_tail(Span lo, BlockCheckMode s, AttemptLocalParseRecovery recover);
void recover_vcs_conflict_marker_in(ParserState& snapshot);
void recover_missing_let_else(const Pat* pat);
bool try_recover_let_missing_semi(Local* local);
void attr_on_non_tail_expr(const Expr* expr);
Stmt* mk_stmt_err(Span span);
// item.rs
struct TreeAhead {
    bool delimited = false;  // TokenTree::Delimited (tok is its open delimiter)
    Token tok;
};
bool tree_look_ahead(size_t dist, TreeAhead& out) const;
bool tok_is_keyword_case(const Token& t, Kw k, Case c) const;
// eat_to_tokens / consume_block: declared and defined by rust_parse_expr (identical ports).
bool maybe_consume_incorrect_semicolon(const Item* previous_item);
Param* dummy_arg(Ident ident);
void eat_incorrect_doc_comment_for_param_type();
bool parameter_without_type(Pat* pat, bool require_name, bool first_param, Ident& out);
std::pair<Pat*, Ty*> recover_arg_parse();
Param* recover_bad_self_param(Param* param);
void deduplicate_recovered_params_names(std::vector<Param*>& fn_inputs);
Span guess_head_span(Span sp) const;
Item* parse_item_mod(AttrVec& attrs);
Item* parse_item_(FnParseMode fn_parse_mode, ForceCollect force_collect, AllowConstBlockItems const_block_items_allowed);
void error_on_unconsumed_default(Defaultness def, ItemKind kind);
Item* parse_item_kind(AttrVec& attrs, bool macros_allowed, AllowConstBlockItems allow_const_block_items, Span lo,
                      const Visibility& vis, Defaultness& def, FnParseMode fn_parse_mode, Case c);
Item* recover_import_as_use();
Item* parse_use_item();
int is_reuse_item();
bool isnt_macro_invocation();
void recover_missing_kw_before_item();
MacCall* parse_item_macro(const Visibility& vis);
void recover_attrs_no_item(const AttrVec& attrs);
bool is_async_fn() const;
bool parse_polarity(Span& neg_span);
Item* parse_item_impl(AttrVec& attrs, Defaultness defaultness, bool is_reuse);
Item* parse_item_delegation(AttrVec& attrs, Defaultness defaultness, int kind);
Block* parse_delegation_body();
Item* parse_impl_delegation(Span span, AttrVec& attrs, Defaultness defaultness);
Item* parse_path_like_delegation();
std::vector<Item*> parse_item_list(AttrVec& attrs, ItemContext which);
bool recover_doc_comment_before_brace();
Defaultness parse_defaultness();
bool check_trait_front_matter();
Item* parse_item_trait(AttrVec& attrs, Span lo);
/// parse_{impl,trait,foreign}_item: Ok(None) -> {false, null}; Ok(Some(None)) -> {true, null}.
std::pair<bool, Item*> parse_impl_item(ForceCollect force_collect);
std::pair<bool, Item*> parse_trait_item(ForceCollect force_collect);
std::pair<bool, Item*> parse_assoc_item(FnParseMode fn_parse_mode, ForceCollect force_collect, ItemContext ctx);
std::pair<bool, Item*> parse_foreign_item(ForceCollect force_collect);
Item* parse_type_alias(Defaultness defaultness);
UseTree* parse_use_tree();
void parse_use_tree_glob_or_nested(UseTree* tree);
std::vector<UseTree*> parse_use_tree_list();
bool parse_rename(Ident& out);
Ident parse_ident_or_underscore();
Item* parse_item_extern_crate();
Ident parse_crate_name_with_dashes();
Item* parse_item_foreign_mod(AttrVec& attrs, Safety safety);
Item* error_bad_item_kind(Span span, ItemKind kind, const char* ctx);
bool is_use_closure() const;
bool is_unsafe_foreign_mod() const;
std::optional<Safety> parse_global_static_front_matter(Case c);
void recover_const_mut(Span const_span);
Item* parse_const_block_item();
Item* parse_static_item(Safety safety, Mutability mutability);
struct ConstParts {
    Ident ident;
    Generics generics;
    Ty* ty = nullptr;
    Expr* rhs = nullptr;
    bool type_const = false;
};
ConstParts parse_const_item(bool const_arg, Span const_span);
Ty* recover_missing_global_item_type(bool colon_present, const char* kind);
Item* parse_item_enum();
Variant* parse_enum_variant(Span span);
Item* parse_item_struct();
Item* parse_item_union();
std::pair<std::vector<FieldDef*>, bool> parse_record_struct_body(const char* adt_ty, Span ident_span, bool parsed_where);
Safety parse_unsafe_field();
std::vector<FieldDef*> parse_tuple_struct_body();
FieldDef* parse_field_def(const char* adt_ty, Span ident_span);
FieldDef* parse_single_struct_field(const char* adt_ty, Span lo, Visibility vis, Safety safety, AttrVec attrs,
                                    Span ident_span);
void expect_field_ty_separator();
FieldDef* parse_name_and_ty(const char* adt_ty, Span lo, Visibility vis, Safety safety, AttrVec attrs);
Ident parse_field_ident(const char* adt_ty, Span lo);
Item* parse_item_decl_macro(Span lo);
int is_macro_rules_item();
Item* parse_item_macro_rules(const Visibility& vis, bool has_bang);
void complain_if_pub_macro(const Visibility& vis, bool macro_rules);
void eat_semi_for_macro_if_needed(const DelimArgs& args);
bool recover_nested_adt_item(Kw keyword);
struct FnParts {
    Ident ident;
    FnSig sig;
    Generics generics;
    FnContract* contract = nullptr;
    Block* body = nullptr;
};
FnParts parse_fn(AttrVec& attrs, FnParseMode fn_parse_mode, Span sig_lo, const Visibility& vis, Case c);
void error_fn_body_not_found(Span ident_span, bool req_body, OptSpan fn_params_end);
Block* parse_fn_body(AttrVec& attrs, const Ident& ident, Span& sig_hi, bool req_body, OptSpan fn_params_end);
bool check_impl_frontmatter(size_t look_ahead_n);
Param* parse_param_general(const FnParseMode& fn_parse_mode, bool first_param, bool recover_arg_parse_);
Param* parse_self_param();
bool is_named_param() const;
bool recover_self_param();
