// rust_format_stubs.cpp -- weak placeholder definitions of every function in
// rust_format_internal.h, so the formatter links while its modules are being
// ported: a module's real (strong) definition replaces the weak one. Remove a
// stub once its module defines it; the file goes away when every module is done.
// Ported from rustfmt (rust-lang/rust 1.97.1 src/tools/rustfmt, MIT OR Apache-2.0).
#include "rust_format_internal.h"

// NOLINTBEGIN
namespace rustfmt {

__attribute__((weak)) CommentStyle comment_style(std::string_view, bool) { return {}; }
__attribute__((weak)) bool fcck_is_comment(FullCodeCharKind) { return false; }
__attribute__((weak)) bool fcck_inside_comment(FullCodeCharKind) { return false; }
__attribute__((weak)) bool fcck_is_string(FullCodeCharKind) { return false; }
__attribute__((weak)) bool fcck_is_commented_string(FullCodeCharKind) { return false; }
__attribute__((weak)) CodeCharKind fcck_to_codecharkind(FullCodeCharKind) { return {}; }
__attribute__((weak)) std::vector<ClassifiedChar> char_classes(std::string_view) { return {}; }
__attribute__((weak)) std::vector<std::pair<FullCodeCharKind, std::string>> line_classes(std::string_view) { return {}; }
__attribute__((weak)) std::vector<std::tuple<CodeCharKind, size_t, std::string_view>> comment_code_slices(std::string_view) { return {}; }
__attribute__((weak)) std::vector<std::tuple<CodeCharKind, size_t, std::string_view>> ungrouped_comment_code_slices(std::string_view) { return {}; }
__attribute__((weak)) std::optional<size_t> find_uncommented(std::string_view, std::string_view) { return {}; }
__attribute__((weak)) bool is_last_comment_block(std::string_view) { return false; }
__attribute__((weak)) RewriteResult combine_strs_with_missing_comments(const RewriteContext&, std::string_view, std::string_view, Span, Shape, bool) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_doc_comment(std::string_view, Shape, const Config&) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_comment(std::string_view, bool, Shape, const Config&) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_missing_comment(Span, Shape, const RewriteContext&) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult recover_missing_comment_in_span(Span, Shape, const RewriteContext&, size_t) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) std::optional<size_t> find_comment_end(std::string_view) { return {}; }
__attribute__((weak)) bool contains_comment(std::string_view) { return false; }
__attribute__((weak)) std::string recover_comment_removed(std::string, Span, const RewriteContext&) { return {}; }
__attribute__((weak)) std::string filter_normal_code(std::string_view) { return {}; }
__attribute__((weak)) Opt rewrite_string(std::string_view, const StringFormat&, size_t) { return {}; }
__attribute__((weak)) MetaItem* parse_meta(const RewriteContext&, const rust::Attribute&) { return {}; }
__attribute__((weak)) Span spanned(const MetaItemInner*) { return {}; }
__attribute__((weak)) const rust::AttrVec* get_attrs_from_stmt(const rust::Stmt*) { return {}; }
__attribute__((weak)) Span get_span_without_attrs(const rust::Stmt*) { return {}; }
__attribute__((weak)) rust::AttrVec filter_inline_attrs(const rust::AttrVec&, Span) { return {}; }
__attribute__((weak)) RewriteResult rewrite_result(const RewriteContext&, const MetaItemInner*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_result(const RewriteContext&, const MetaItem*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_result(const RewriteContext&, const rust::Attribute*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_attrs(const RewriteContext&, const rust::AttrVec&, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_attrs(const RewriteContext&, const rust::Attribute*, size_t, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) SeparatorPlace separator_place_from_tactic(SeparatorPlace, DefinitiveListTactic, std::string_view) { return {}; }
__attribute__((weak)) DefinitiveListTactic definitive_tactic(const std::vector<ListItem>&, ListTactic, Separator, size_t) { return {}; }
__attribute__((weak)) RewriteResult write_list(const std::vector<ListItem>&, const ListFormatting&) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) std::pair<Opt, ListItemCommentStyle> extract_pre_comment(std::string_view) { return {}; }
__attribute__((weak)) Opt extract_post_comment(std::string_view, size_t, std::string_view, bool) { return {}; }
__attribute__((weak)) size_t get_comment_end(std::string_view, std::string_view, std::string_view, bool) { return 0; }
__attribute__((weak)) bool has_extra_newline(std::string_view, size_t) { return false; }
__attribute__((weak)) std::vector<ListItem> itemize_list(const SnippetProvider&, size_t, std::string_view, std::string_view, const std::function<uint32_t(size_t)>&, const std::function<uint32_t(size_t)>&, const std::function<RewriteResult(size_t)>&, uint32_t, uint32_t, bool) { return {}; }
__attribute__((weak)) size_t total_item_width(const ListItem&) { return 0; }
__attribute__((weak)) std::optional<std::pair<std::optional<Shape>, Shape>> struct_lit_shape(Shape, const RewriteContext&, size_t, size_t) { return {}; }
__attribute__((weak)) DefinitiveListTactic struct_lit_tactic(std::optional<Shape>, const RewriteContext&, const std::vector<ListItem>&) { return {}; }
__attribute__((weak)) Shape shape_for_tactic(DefinitiveListTactic, std::optional<Shape>, Shape) { return {}; }
__attribute__((weak)) ListFormatting struct_lit_formatting(Shape, DefinitiveListTactic, const RewriteContext&, bool) { return ListFormatting(Shape(), Config()); }
__attribute__((weak)) RewriteResult rewrite_result(const RewriteContext&, const RwNode&, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) Span spanned(const RwNode&) { return {}; }
__attribute__((weak)) bool overflowable_is_simple(const OverflowableItem&) { return false; }
__attribute__((weak)) bool overflowable_is_expr(const OverflowableItem&) { return false; }
__attribute__((weak)) bool overflowable_is_nested_call(const OverflowableItem&) { return false; }
__attribute__((weak)) const rust::Expr* overflowable_to_expr(const OverflowableItem&) { return {}; }
__attribute__((weak)) bool overflowable_can_be_overflowed(const OverflowableItem&, const RewriteContext&, size_t) { return false; }
__attribute__((weak)) RewriteResult rewrite_with_parens(const RewriteContext&, std::string_view, const std::vector<OverflowableItem>&, Shape, Span, size_t, std::optional<SeparatorTactic>) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_with_angle_brackets(const RewriteContext&, std::string_view, const std::vector<OverflowableItem>&, Shape, Span) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_with_square_brackets(const RewriteContext&, std::string_view, const std::vector<OverflowableItem>&, Shape, Span, std::optional<SeparatorTactic>, std::optional<Delimiter>) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) std::optional<std::pair<bool, size_t>> maybe_get_args_offset(std::string_view, const std::vector<OverflowableItem>&, const Config&) { return {}; }
__attribute__((weak)) RewriteResult rewrite_all_pairs(const rust::Expr*, Shape, const RewriteContext&) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_pair(const RwNode&, const RwNode&, PairParts, const RewriteContext&, Shape, SeparatorPlace) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) bool lit_ends_in_dot(const rust::Lit&, const RewriteContext&) { return false; }
__attribute__((weak)) bool float_lit_ends_in_dot(std::string_view, std::string_view) { return false; }
__attribute__((weak)) RewriteResult format_expr(const rust::Expr*, ExprType, const RewriteContext&, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_array(std::string_view, const std::vector<OverflowableItem>&, Span, const RewriteContext&, Shape, std::optional<SeparatorTactic>, std::optional<Delimiter>) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_block_with_visitor(const RewriteContext&, std::string_view, const rust::Block*, const rust::AttrVec*, const rust::Label*, Shape, bool) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_let_else_block(const rust::Block*, bool, const RewriteContext&, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) Opt rewrite_cond(const RewriteContext&, const rust::Expr*, Shape) { return {}; }
__attribute__((weak)) std::string rewrite_else_kw_with_comments(bool, bool, const RewriteContext&, Span, Shape) { return {}; }
__attribute__((weak)) bool block_contains_comment(const RewriteContext&, const rust::Block*) { return false; }
__attribute__((weak)) bool is_simple_block(const RewriteContext&, const rust::Block*, const rust::AttrVec*) { return false; }
__attribute__((weak)) bool is_simple_block_stmt(const RewriteContext&, const rust::Block*, const rust::AttrVec*) { return false; }
__attribute__((weak)) bool is_empty_block(const RewriteContext&, const rust::Block*, const rust::AttrVec*) { return false; }
__attribute__((weak)) bool stmt_is_expr(const rust::Stmt*) { return false; }
__attribute__((weak)) bool is_unsafe_block(const rust::Block*) { return false; }
__attribute__((weak)) RewriteResult rewrite_literal(const RewriteContext&, const rust::Lit&, Span, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_call(const RewriteContext&, std::string_view, const std::vector<rust::Expr*>&, Span, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) bool is_simple_expr(const rust::Expr*) { return false; }
__attribute__((weak)) bool is_every_expr_simple(const std::vector<OverflowableItem>&) { return false; }
__attribute__((weak)) bool can_be_overflowed_expr(const RewriteContext&, const rust::Expr*, size_t) { return false; }
__attribute__((weak)) bool is_nested_call(const rust::Expr*) { return false; }
__attribute__((weak)) bool span_ends_with_comma(const RewriteContext&, Span) { return false; }
__attribute__((weak)) RewriteResult rewrite_paren(const RewriteContext&, const rust::Expr*, Shape, Span) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult wrap_struct_field(const RewriteContext&, const rust::AttrVec&, std::string_view, Shape, Shape, size_t) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) const char* struct_lit_field_separator(const Config&) { return ""; }
__attribute__((weak)) RewriteResult rewrite_field(const RewriteContext&, const rust::ExprField*, Shape, size_t) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_tuple(const RewriteContext&, const std::vector<OverflowableItem>&, Span, Shape, bool) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_unary_prefix(const RewriteContext&, std::string_view, const RwNode&, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_unary_suffix(const RewriteContext&, std::string_view, const RwNode&, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_assign_rhs(const RewriteContext&, std::string, const RwNode&, const RhsAssignKind&, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_assign_rhs_expr(const RewriteContext&, std::string_view, const RwNode&, Shape, const RhsAssignKind&, RhsTactics) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_assign_rhs_with(const RewriteContext&, std::string, const RwNode&, Shape, const RhsAssignKind&, RhsTactics) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_assign_rhs_with_comments(const RewriteContext&, std::string, const RwNode&, Shape, const RhsAssignKind&, RhsTactics, Span, bool) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) bool prefer_next_line(std::string_view, std::string_view, RhsTactics) { return false; }
__attribute__((weak)) bool is_method_call(const rust::Expr*) { return false; }
__attribute__((weak)) RewriteResult rewrite_result(const RewriteContext&, const rust::Expr*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_result(const RewriteContext&, const rust::Block*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_result(const RewriteContext&, const rust::ExprField*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) Span spanned(const StmtW&) { return {}; }
__attribute__((weak)) RewriteResult rewrite_result(const RewriteContext&, const StmtW&, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_result(const RewriteContext&, const rust::Local*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_closure(const rust::Closure*, Span, const RewriteContext&, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_last_closure(const RewriteContext&, const rust::Expr*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) bool args_have_many_closure(const std::vector<OverflowableItem>&) { return false; }
__attribute__((weak)) RewriteResult rewrite_match(const RewriteContext&, const rust::Expr*, const std::vector<rust::Arm>&, Shape, Span, const rust::AttrVec&, rust::MatchKind) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_chain(const rust::Expr*, const RewriteContext&, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) Span spanned(const TuplePatField*) { return {}; }
__attribute__((weak)) Span spanned(const RangeOperand*) { return {}; }
__attribute__((weak)) bool is_short_pattern(const RewriteContext&, const rust::Pat*, std::string_view) { return false; }
__attribute__((weak)) RewriteResult rewrite_range_pat(const RewriteContext&, Shape, const rust::Expr*, const rust::Expr*, rust::RangeEndKind, Span, Span) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) bool can_be_overflowed_pat(const RewriteContext&, const TuplePatField*, size_t) { return false; }
__attribute__((weak)) RewriteResult rewrite_result(const RewriteContext&, const rust::Pat*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_result(const RewriteContext&, const rust::PatField*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_result(const RewriteContext&, const TuplePatField*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_result(const RewriteContext&, const RangeOperand*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) Span spanned(const SegmentParam*) { return {}; }
__attribute__((weak)) RewriteResult rewrite_path(const RewriteContext&, PathContext, const rust::QSelf*, const rust::Path*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) const rust::GenericBounds* opaque_ty(const rust::Ty*) { return {}; }
__attribute__((weak)) bool can_be_overflowed_type(const RewriteContext&, const rust::Ty*, size_t) { return false; }
__attribute__((weak)) Opt rewrite_bound_params(const RewriteContext&, Shape, const std::vector<rust::GenericParam*>&) { return {}; }
__attribute__((weak)) RewriteResult rewrite_result(const RewriteContext&, const SegmentParam*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_result(const RewriteContext&, const rust::PreciseCapturingArg*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_result(const RewriteContext&, const rust::AssocItemConstraint*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_result(const RewriteContext&, const rust::WherePredicate*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_result(const RewriteContext&, const rust::GenericArg*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_result(const RewriteContext&, const rust::AnonConst*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_result(const RewriteContext&, const rust::Lifetime*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_result(const RewriteContext&, const rust::GenericBound*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_result(const RewriteContext&, const rust::GenericBounds*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_result(const RewriteContext&, const rust::GenericParam*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_result(const RewriteContext&, const rust::PolyTraitRef*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_trait_ref(const RewriteContext&, const rust::Path*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_result(const RewriteContext&, const rust::Ty*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_result(const RewriteContext&, const rust::TyPat*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult format_impl(const RewriteContext&, const rust::Item*, Indent) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult format_trait(const RewriteContext&, const rust::Item*, Indent) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult format_trait_alias(const RewriteContext&, const rust::Item*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_type_alias(const rust::Item*, const RewriteContext&, Indent, ItemVisitorKind, Span) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_struct_field_prefix(const RewriteContext&, const rust::FieldDef*) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_struct_field(const RewriteContext&, const rust::FieldDef*, Shape, size_t) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) uint32_t span_lo_for_param(const rust::Param*) { return 0; }
__attribute__((weak)) uint32_t span_hi_for_param(const RewriteContext&, const rust::Param*) { return 0; }
__attribute__((weak)) bool is_named_param(const rust::Param*) { return false; }
__attribute__((weak)) RewriteResult rewrite_mod(const RewriteContext&, const rust::Item*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_extern_crate(const RewriteContext&, const rust::Item*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) bool is_mod_decl(const rust::Item*) { return false; }
__attribute__((weak)) bool is_use_item(const rust::Item*) { return false; }
__attribute__((weak)) bool is_extern_crate(const rust::Item*) { return false; }
__attribute__((weak)) RewriteResult rewrite_result(const RewriteContext&, const rust::FieldDef*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_result(const RewriteContext&, const rust::FnRetTy*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_result(const RewriteContext&, const rust::Param*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) void visit_static(FmtVisitor&, const rust::Item*) {  }
__attribute__((weak)) void visit_struct(FmtVisitor&, const rust::Item*) {  }
__attribute__((weak)) void visit_enum(FmtVisitor&, const rust::Item*) {  }
__attribute__((weak)) void format_foreign_mod(FmtVisitor&, const rust::Item*) {  }
__attribute__((weak)) std::optional<std::pair<std::string, FnBraceStyle>> rewrite_fn_before_block(FmtVisitor&, Indent, const rust::Item*, Span) { return {}; }
__attribute__((weak)) RewriteResult rewrite_required_fn(FmtVisitor&, Indent, const rust::Item*, Span) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) Opt single_line_fn(const FmtVisitor&, std::string_view, const rust::Block*, const rust::AttrVec*) { return {}; }
__attribute__((weak)) void format_import(FmtVisitor&, const rust::Item*) {  }
__attribute__((weak)) void visit_items_with_reordering(FmtVisitor&, const std::vector<const rust::Item*>&) {  }
__attribute__((weak)) int version_sort(std::string_view, std::string_view) { return 0; }
__attribute__((weak)) Span spanned(const MacroArg*) { return {}; }
__attribute__((weak)) RewriteResult rewrite_result(const RewriteContext&, const MacroArg*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_macro(const rust::MacCall*, const RewriteContext&, Shape, MacroPosition, Span) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) RewriteResult rewrite_macro_def(const RewriteContext&, Shape, Indent, const rust::Item*, Span) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) const rust::Expr* convert_try_mac(const rust::MacCall*, const RewriteContext&) { return {}; }
__attribute__((weak)) Delimiter macro_style(const rust::MacCall*, const RewriteContext&) { return {}; }
__attribute__((weak)) RewriteResult rewrite_result(const RewriteContext&, const rust::Item*, Shape) { return RewriteResult(RewriteError::unknown()); }
__attribute__((weak)) std::optional<FormattedSnippet> format_snippet(std::string_view, const Config&, bool, FormatState&) { return {}; }
__attribute__((weak)) std::optional<FormattedSnippet> format_code_block(std::string_view, const Config&, bool, FormatState&) { return {}; }

// Member functions.
__attribute__((weak)) bool CommentStyle::is_line_comment() const { return false; }
__attribute__((weak)) bool CommentStyle::is_block_comment() const { return false; }
__attribute__((weak)) bool CommentStyle::is_doc_comment() const { return false; }
__attribute__((weak)) std::string_view CommentStyle::opener() const { return {}; }
__attribute__((weak)) std::string_view CommentStyle::closer() const { return {}; }
__attribute__((weak)) std::string_view CommentStyle::line_start() const { return {}; }
__attribute__((weak)) bool ListFormatting::needs_trailing_separator() const { return false; }
__attribute__((weak)) ListItem ListItem::from_item(RewriteResult item) { ListItem l; l.item = std::move(item); return l; }
__attribute__((weak)) ListItem ListItem::from_str(std::string s) { ListItem l; l.item = RewriteResult(std::move(s)); return l; }
__attribute__((weak)) bool ListItem::is_different_group() const { return false; }
__attribute__((weak)) bool ListItem::is_multiline() const { return false; }
__attribute__((weak)) bool ListItem::has_single_line_comment() const { return false; }
__attribute__((weak)) bool ListItem::has_comment() const { return false; }
__attribute__((weak)) bool ListItem::is_substantial() const { return false; }
__attribute__((weak)) const rust::Item* StmtW::to_item() const { return nullptr; }
__attribute__((weak)) bool StmtW::is_empty() const { return false; }
__attribute__((weak)) bool StmtW::is_last_expr() const { return false; }
__attribute__((weak)) std::optional<StmtW> StmtW::from_simple_block(const RewriteContext&, const rust::Block*, const rust::AttrVec*) { return {}; }
__attribute__((weak)) std::vector<StmtW> StmtW::from_ast_nodes(const std::vector<rust::Stmt*>&) { return {}; }
__attribute__((weak)) SegmentParam SegmentParam::from_generic_arg(const rust::AngleBracketedArg&) { return {}; }
__attribute__((weak)) FmtVisitor FmtVisitor::from_context(const RewriteContext&) { return FmtVisitor(); }
__attribute__((weak)) FmtVisitor FmtVisitor::from_parse(rust::ParseResult*, const Config&, const SnippetProvider&, FormatState&) { return FmtVisitor(); }
__attribute__((weak)) FmtVisitor::~FmtVisitor() {}
__attribute__((weak)) std::string_view FmtVisitor::snippet(Span) const { return {}; }
__attribute__((weak)) RewriteContext FmtVisitor::get_context() const { return RewriteContext(); }
__attribute__((weak)) void FmtVisitor::visit_block(const rust::Block*, const rust::AttrVec*, bool) {}
__attribute__((weak)) void FmtVisitor::visit_fn(const rust::Item*, Span, const rust::AttrVec*) {}
__attribute__((weak)) void FmtVisitor::visit_item(const rust::Item*) {}
__attribute__((weak)) void FmtVisitor::visit_trait_item(const rust::Item*) {}
__attribute__((weak)) void FmtVisitor::visit_impl_item(const rust::Item*) {}
__attribute__((weak)) void FmtVisitor::visit_foreign_item(const rust::Item*) {}
__attribute__((weak)) void FmtVisitor::push_str(std::string_view) {}
__attribute__((weak)) void FmtVisitor::push_rewrite(Span, Opt) {}
__attribute__((weak)) void FmtVisitor::push_skipped_with_span(const rust::AttrVec&, Span, Span) {}
__attribute__((weak)) bool FmtVisitor::visit_attrs(const rust::AttrVec&, rust::AttrStyle) { return false; }
__attribute__((weak)) void FmtVisitor::skip_empty_lines(uint32_t) {}
__attribute__((weak)) void FmtVisitor::walk_mod_items(const std::vector<rust::Item*>&) {}
__attribute__((weak)) void FmtVisitor::format_separate_mod(const rust::Crate*, uint32_t) {}
__attribute__((weak)) void FmtVisitor::format_missing(uint32_t) {}
__attribute__((weak)) void FmtVisitor::format_missing_with_indent(uint32_t) {}
__attribute__((weak)) void FmtVisitor::format_missing_no_indent(uint32_t) {}
__attribute__((weak)) DefinitiveListTactic definitive_tactic(const ListItem*, const ListItem*, ListTactic, Separator, size_t) { return {}; }

__attribute__((weak)) std::optional<size_t> find_last_uncommented(std::string_view, std::string_view) { return {}; }
}  // namespace rustfmt
// NOLINTEND
