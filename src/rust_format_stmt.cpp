// rust_format_stmt.cpp -- rustfmt's stmt.rs
// Ported from rustfmt (rust-lang/rust 1.97.1 src/tools/rustfmt, MIT OR Apache-2.0).
// See rust_format_internal.h for the conventions.
#include "rust_format_internal.h"

namespace rustfmt {

// impl Spanned for Stmt
Span spanned(const StmtW& s) { return spanned(s.inner); }

const rust::Item* StmtW::to_item() const {
    if (inner->kind == rust::StmtKind::Item) return inner->item;
    return nullptr;
}

std::optional<StmtW> StmtW::from_simple_block(const RewriteContext& ctx, const rust::Block* block,
                                              const rust::AttrVec* attrs) {
    if (is_simple_block(ctx, block, attrs)) {
        const rust::Stmt* inner = block->stmts[0];
        // Simple blocks only contain one expr and no stmts
        bool is_last = true;
        return StmtW{inner, is_last};
    }
    return std::nullopt;
}

std::vector<StmtW> StmtW::from_ast_nodes(const std::vector<rust::Stmt*>& stmts) {
    std::vector<StmtW> result;
    result.reserve(stmts.size());
    for (size_t i = 0; i < stmts.size(); i++) result.push_back(StmtW{stmts[i], i + 1 == stmts.size()});
    return result;
}

bool StmtW::is_empty() const { return inner->kind == rust::StmtKind::Empty; }

bool StmtW::is_last_expr() const {
    if (!is_last) return false;

    if (as_ast_node()->kind == rust::StmtKind::Expr) {
        switch (as_ast_node()->expr->kind) {
        case rust::ExprKind::Ret:
        case rust::ExprKind::Continue:
        case rust::ExprKind::Break: return false;
        default: return true;
        }
    }
    return false;
}

static RewriteResult format_stmt(const RewriteContext& ctx, Shape shape, const rust::Stmt* stmt, ExprType expr_type,
                                 bool is_last_expr) {
    // skip_out_of_file_lines_range_err!: file_lines is the default (everything), never skips.

    RewriteResult result(RewriteError::unknown());
    switch (stmt->kind) {
    case rust::StmtKind::Let: result = rewrite_result(ctx, stmt->local, shape); break;
    case rust::StmtKind::Expr:
    case rust::StmtKind::Semi: {
        const rust::Expr* ex = stmt->expr;
        const char* suffix = semicolon_for_stmt(ctx, stmt, is_last_expr) ? ";" : "";

        std::optional<Shape> s = shape.sub_width(std::char_traits<char>::length(suffix));
        if (!s) return Err(RewriteError::exceeds(shape.width, spanned(ex)));
        result = format_expr(ex, expr_type, ctx, *s);
        if (result) *result += suffix;
        break;
    }
    case rust::StmtKind::MacCall:
    case rust::StmtKind::Item:
    case rust::StmtKind::Empty: result = Err(RewriteError::unknown()); break;
    }
    if (!result) return result;
    return recover_comment_removed(std::move(*result), spanned(stmt), ctx);
}

// impl Rewrite for Stmt
RewriteResult rewrite_result(const RewriteContext& ctx, const StmtW& s, Shape shape) {
    ExprType expr_type = ctx.config->style_edition() >= StyleEdition::E2024 && s.is_last_expr()
                             ? ExprType::SubExpression
                             : ExprType::Statement;
    return format_stmt(ctx, shape, s.as_ast_node(), expr_type, s.is_last_expr());
}

}  // namespace rustfmt
