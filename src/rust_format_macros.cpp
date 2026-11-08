// rust_format_macros.cpp -- rustfmt's macros.rs, plus parse/macros/mod.rs and
// parse/macros/lazy_static.rs (the macro-argument re-parsing it depends on).
// Ported from rustfmt (rust-lang/rust 1.97.1 src/tools/rustfmt, MIT OR Apache-2.0).
// See rust_format_internal.h for the conventions.
//
// Not ported:
//  - MacroArgParser / MacroArgKind / ParsedMacroArg / wrap_macro_args /
//    delim_token_to_str / next_space / force_space_before / ident_like:
//    only reachable with format_macro_matchers = true (non-default).
//    format_macro_args returns the matcher's snippet verbatim by default.
//  - parse/macros/cfg_if.rs, cfg_match.rs: used only by modules.rs to find
//    out-of-line modules, never for formatting. asm.rs is dead code upstream.
#include "rust_format_internal.h"

#include <map>
#include <memory>

#include "rust_parse_internal.h"

namespace rustfmt {

namespace {

using rust::Tok;
using rust::Token;

constexpr const char* FORCED_BRACKET_MACROS[] = {"vec!"};

// MacCall::span(): path.span.to(args.dspan.entire()).
Span mac_span(const rust::MacCall* mac) {
    Span s = mac->args.span();
    if (mac->path) s = mac->path->span.to(s);
    return s;
}

Delimiter delim_of_tok(Tok k) {
    switch (k) {
    case Tok::OpenParen: return Delimiter::Parenthesis;
    case Tok::OpenBracket: return Delimiter::Bracket;
    case Tok::OpenBrace: return Delimiter::Brace;
    default: break;
    }
    return Delimiter::Invisible;
}

bool is_open_delim(Tok k) { return k == Tok::OpenParen || k == Tok::OpenBracket || k == Tok::OpenBrace; }

RewriteResult macro_error(Opt o, MacroErrorKind kind, Span span) {
    if (o) return RewriteResult(std::move(*o));
    return Err(RewriteError::macro_failure(kind, span));
}

// ===========================================================================
// parse/macros/mod.rs. rustfmt drives rustc's own Parser over the macro's
// tokens (build_stream_parser: Recovery::Forbidden); so does this port, with
// mep's rustc_parse port (rust_parse_internal.h). Cloning the parser for an
// attempt is a ParserState snapshot; `dcx().reset_err_count()` drops the
// diagnostics the attempt emitted.

using RParser = rust::detail::Parser;

// build_parser / build_stream_parser
std::unique_ptr<RParser> build_parser(const RewriteContext& ctx, uint32_t tok_lo, uint32_t tok_hi) {
    rust::ParseResult& pr = *ctx.pr;
    if (tok_hi > pr.tokens.size() - 1) tok_hi = static_cast<uint32_t>(pr.tokens.size() - 1);
    auto p = std::make_unique<RParser>(pr, tok_lo, tok_hi, "macro arguments");
    p->st_.recovery = rust::detail::Recovery::Forbidden;
    return p;
}

bool tok_is_reserved_ident(const Token* t, rust::Edition e) {
    return t && t->kind == Tok::Ident && !t->raw && rust::kw_is_reserved(t->kw, e);
}

bool tok_is_path_segment_keyword(const Token* t) {
    return t && t->kind == Tok::Ident && !t->raw && rust::kw_is_path_segment_keyword(t->kw);
}

bool tok_is_keyword(const Token* t, rust::Kw k) { return t && t->kind == Tok::Ident && !t->raw && t->kw == k; }

// token.rs: ident_can_begin_expr
bool ident_can_begin_expr(const Token* t, rust::Edition e) {
    using rust::Kw;
    if (!tok_is_reserved_ident(t, e) || tok_is_path_segment_keyword(t)) return true;
    switch (t->kw) {
    case Kw::Async: case Kw::Do: case Kw::Box: case Kw::Break: case Kw::Const: case Kw::Continue:
    case Kw::False: case Kw::For: case Kw::Gen: case Kw::If: case Kw::Let: case Kw::Loop: case Kw::Match:
    case Kw::Move: case Kw::Return: case Kw::True: case Kw::Try: case Kw::Unsafe: case Kw::While:
    case Kw::Yield: case Kw::Safe: case Kw::Static:
        return true;
    default: return false;
    }
}

// token.rs: ident_can_begin_type
bool ident_can_begin_type(const Token* t, rust::Edition e) {
    using rust::Kw;
    if (!tok_is_reserved_ident(t, e) || tok_is_path_segment_keyword(t)) return true;
    switch (t->kw) {
    case Kw::Underscore: case Kw::For: case Kw::Impl: case Kw::Fn: case Kw::Unsafe: case Kw::Extern:
    case Kw::Typeof: case Kw::Dyn:
        return true;
    default: return false;
    }
}

// token.rs: Token::can_begin_expr
bool can_begin_expr(const Token* t, rust::Edition e) {
    if (!t) return false;
    switch (t->kind) {
    case Tok::Ident: return ident_can_begin_expr(t, e);
    case Tok::OpenParen: case Tok::OpenBrace: case Tok::OpenBracket: case Tok::Literal: case Tok::Bang:
    case Tok::Minus: case Tok::Star: case Tok::Or: case Tok::OrOr: case Tok::And: case Tok::AndAnd:
    case Tok::DotDot: case Tok::DotDotDot: case Tok::DotDotEq: case Tok::Lt: case Tok::Shl: case Tok::PathSep:
    case Tok::Lifetime: case Tok::Pound:
        return true;
    default: return false;
    }
}

// token.rs: Token::can_begin_type
bool can_begin_type(const Token* t, rust::Edition e) {
    if (!t) return false;
    switch (t->kind) {
    case Tok::Ident: return ident_can_begin_type(t, e);
    case Tok::OpenParen: case Tok::OpenBracket: case Tok::Bang: case Tok::Star: case Tok::And: case Tok::AndAnd:
    case Tok::Question: case Tok::Lifetime: case Tok::Lt: case Tok::Shl: case Tok::PathSep:
        return true;
    default: return false;
    }
}

// token.rs: Token::can_begin_pattern(PatParam { inferred: false })
bool can_begin_pattern_param(const Token* t) {
    if (!t) return false;
    switch (t->kind) {
    case Tok::Ident: case Tok::OpenParen: case Tok::OpenBracket: case Tok::And: case Tok::Minus:
    case Tok::AndAnd: case Tok::Literal: case Tok::DotDot: case Tok::DotDotDot: case Tok::PathSep:
    case Tok::Lt: case Tok::Shl:
        return true;
    default: return false;  // `|` only for PatWithOr
    }
}

enum class NtKind : uint8_t { Expr, Ty, Pat, Item };

// Parser::nonterminal_may_begin_with for the kinds parse_macro_arg tries.
bool nonterminal_may_begin_with(NtKind kind, const Token* t, rust::Edition e) {
    switch (kind) {
    case NtKind::Expr:
        // NonterminalKind::Expr(Expr): `_` allowed, `let` not.
        return (can_begin_expr(t, e) || tok_is_keyword(t, rust::Kw::Underscore)) && !tok_is_keyword(t, rust::Kw::Let);
    case NtKind::Ty: return can_begin_type(t, e);
    case NtKind::Pat: return can_begin_pattern_param(t);
    case NtKind::Item: {
        // close_delim().is_none()
        Tok k = t ? t->kind : Tok::Eof;
        return k != Tok::CloseParen && k != Tok::CloseBrace && k != Tok::CloseBracket;
    }
    }
    return false;
}

// parse_macro_arg: try Expr, Ty, Pat, Item in order on a cloned parser; the
// first that parses without errors wins and the parser moves past it.
MacroArg* parse_macro_arg(const RewriteContext& ctx, RParser& parser) {
    rust::Edition ed = parser.edition_;
    for (NtKind kind : {NtKind::Expr, NtKind::Ty, NtKind::Pat, NtKind::Item}) {
        rust::detail::ParserState snapshot = parser.create_snapshot_for_diagnostic();
        size_t n_diags = parser.diags.size();
        if (!nonterminal_may_begin_with(kind, &parser.token, ed)) continue;
        MacroArg* arg = ctx.arena().make<MacroArg>();
        bool none = false;
        try {
            switch (kind) {
            case NtKind::Expr:
                arg->kind = MacroArg::Expr;
                arg->expr = parser.parse_expr();
                break;
            case NtKind::Ty:
                arg->kind = MacroArg::Ty;
                arg->ty = parser.parse_ty();
                break;
            case NtKind::Pat:
                arg->kind = MacroArg::Pat;
                arg->pat = parser.parse_pat_no_top_alt(rust::detail::Expected::None,
                                                       rust::detail::PatternLocation::None);
                break;
            case NtKind::Item:
                arg->kind = MacroArg::Item;
                arg->item = parser.parse_item(rust::detail::ForceCollect::No, rust::detail::AllowConstBlockItems::Yes);
                none = arg->item == nullptr;
                break;
            }
            if (parser.diags.size() > n_diags) {
                // has_errors(): reset_err_count, and the clone is dropped.
                parser.diags.resize(n_diags);
                parser.restore_snapshot(snapshot);
            } else {
                // Parsing succeeded. (`$then(x)?`: an Ok(None) item is None.)
                if (none) return nullptr;
                return arg;
            }
        } catch (const rust::detail::PErr&) {
            parser.diags.resize(n_diags);
            parser.restore_snapshot(snapshot);
        } catch (const rust::detail::FatalAbort&) {
            parser.diags.resize(n_diags);
            parser.restore_snapshot(snapshot);
        }
    }
    return nullptr;
}

struct ParsedMacroArgs {
    bool vec_with_semi = false;
    bool trailing_comma = false;
    std::vector<MacroArg*> args;
};

MacroArg* check_keyword(const RewriteContext& ctx, RParser& parser) {
    Tok next = parser.look_ahead_tok(1).kind;
    if (tok_is_reserved_ident(&parser.token, parser.edition_) && (next == Tok::Eof || next == Tok::Comma)) {
        MacroArg* a = ctx.arena().make<MacroArg>();
        a->kind = MacroArg::Keyword;
        a->keyword.name = parser.token.sym;
        parser.bump();
        a->span = parser.prev_token.span;
        return a;
    }
    return nullptr;
}

std::optional<ParsedMacroArgs> parse_macro_args_inner(const RewriteContext& ctx, RParser& parser, Delimiter style,
                                                      bool forced_bracket) {
    ParsedMacroArgs out;

    if (style != Delimiter::Brace) {
        for (;;) {
            if (MacroArg* arg = check_keyword(ctx, parser)) {
                out.args.push_back(arg);
            } else if (MacroArg* arg2 = parse_macro_arg(ctx, parser)) {
                out.args.push_back(arg2);
            } else {
                return std::nullopt;
            }

            Tok k = parser.token.kind;
            if (k == Tok::Eof) break;
            if (k == Tok::Comma) {
                // bumped below
            } else if (k == Tok::Semi) {
                // Try to parse `vec![expr; expr]`
                if (forced_bracket) {
                    parser.bump();
                    if (parser.token.kind != Tok::Eof) {
                        MacroArg* arg = parse_macro_arg(ctx, parser);
                        if (!arg) return std::nullopt;
                        out.args.push_back(arg);
                        parser.bump();
                        if (parser.token.kind == Tok::Eof && out.args.size() == 2) {
                            out.vec_with_semi = true;
                            break;
                        }
                    }
                }
                return std::nullopt;
            } else if (!out.args.empty() && out.args.back()->is_item()) {
                continue;
            } else {
                return std::nullopt;
            }

            parser.bump();

            if (parser.token.kind == Tok::Eof) {
                out.trailing_comma = true;
                break;
            }
        }
    }
    return out;
}

std::optional<ParsedMacroArgs> parse_macro_args(const RewriteContext& ctx, uint32_t tok_lo, uint32_t tok_hi,
                                                Delimiter style, bool forced_bracket) {
    std::unique_ptr<RParser> parser = build_parser(ctx, tok_lo, tok_hi);
    try {
        return parse_macro_args_inner(ctx, *parser, style, forced_bracket);
    } catch (const rust::detail::PErr&) {  // the parser's work budget (bump)
        return std::nullopt;
    } catch (const rust::detail::FatalAbort&) {
        return std::nullopt;
    }
}

/// parse/macros/mod.rs parse_expr: `parser.parse_expr().ok()`.
rust::Expr* parse_expr(const RewriteContext& ctx, uint32_t tok_lo, uint32_t tok_hi) {
    std::unique_ptr<RParser> parser = build_parser(ctx, tok_lo, tok_hi);
    try {
        return parser->parse_expr();
    } catch (const rust::detail::PErr&) {
        return nullptr;
    } catch (const rust::detail::FatalAbort&) {
        return nullptr;
    }
}

// ===========================================================================
// parse/macros/lazy_static.rs

struct LazyStaticElem {
    rust::Visibility vis;
    std::string id;
    rust::Ty* ty = nullptr;
    rust::Expr* expr = nullptr;
};

std::optional<std::vector<LazyStaticElem>> parse_lazy_static_inner(RParser& parser) {
    std::vector<LazyStaticElem> result;
    // parse_or!: Ok but errors emitted, or Err: None.
    auto parse_or = [&](auto f) -> bool {
        size_t n = parser.diags.size();
        try {
            f();
        } catch (const rust::detail::PErr&) {
            parser.diags.resize(n);
            return false;
        } catch (const rust::detail::FatalAbort&) {
            parser.diags.resize(n);
            return false;
        }
        if (parser.diags.size() > n) {
            parser.diags.resize(n);
            return false;
        }
        return true;
    };
    while (parser.token.kind != Tok::Eof) {
        // Parse a `lazy_static!` item.
        // FIXME: These `eat_*` calls should be converted to `parse_or` to avoid
        // silently formatting malformed lazy-statics.
        LazyStaticElem el;
        if (!parse_or([&] { el.vis = parser.parse_visibility(rust::detail::FollowedByType::No); })) return std::nullopt;
        parser.eat_keyword(KW(Static));
        parser.eat_keyword(KW(Ref));
        rust::Ident id;
        if (!parse_or([&] { id = parser.parse_ident(); })) return std::nullopt;
        el.id = std::string(id.raw ? "r#" : "") + std::string(id.name);  // Ident's Display
        parser.eat(EXP(Colon));
        if (!parse_or([&] { el.ty = parser.parse_ty(); })) return std::nullopt;
        parser.eat(EXP(Eq));
        if (!parse_or([&] { el.expr = parser.parse_expr(); })) return std::nullopt;
        parser.eat(EXP(Semi));
        result.push_back(std::move(el));
    }
    return result;
}

std::optional<std::vector<LazyStaticElem>> parse_lazy_static(const RewriteContext& ctx, uint32_t tok_lo,
                                                             uint32_t tok_hi) {
    std::unique_ptr<RParser> parser = build_parser(ctx, tok_lo, tok_hi);
    try {
        return parse_lazy_static_inner(*parser);
    } catch (const rust::detail::PErr&) {
        return std::nullopt;
    } catch (const rust::detail::FatalAbort&) {
        return std::nullopt;
    }
}

// ===========================================================================
// macros.rs

/// Rewrite macro name without using pretty-printer if possible.
std::string rewrite_macro_name(const RewriteContext& ctx, const rust::Path* path) {
    if (path->segments.size() == 1) {
        // Avoid using pretty-printer in the common case.
        return std::string(rewrite_ident(ctx, path->segments[0].ident)) + "!";
    }
    return path_to_string(ctx, path) + "!";
}

// Use this on failing to format the macro call.
RewriteResult return_macro_parse_failure_fallback(const RewriteContext& ctx, Indent indent, MacroPosition position,
                                                  Span span) {
    // Mark this as a failure however we format it
    ctx.macro_rewrite_failure = true;

    // Heuristically determine whether the last line of the macro uses "Block" style
    // rather than using "Visual" style, or another indentation style.
    bool is_like_block_indent_style = false;
    {
        std::vector<std::string_view> ls = lines(ctx.snippet(span));
        if (!ls.empty()) {
            std::string_view closing_line = trim(ls.back());
            is_like_block_indent_style = true;
            for (char c : closing_line) {
                // chars().all(..): any non-ASCII char is not one of the three.
                if (c != '}' && c != ')' && c != ']') {
                    is_like_block_indent_style = false;
                    break;
                }
            }
        }
    }
    if (is_like_block_indent_style) {
        return macro_error(trim_left_preserve_layout(ctx.snippet(span), indent, *ctx.config), MacroErrorKind::Unknown,
                           span);
    }

    ctx.skipped_range->emplace_back(static_cast<size_t>(ctx.pr->line_col(span.lo).line),
                                    static_cast<size_t>(ctx.pr->line_col(span.hi).line));

    // Return the snippet unmodified if the macro is not block-like
    std::string snippet(ctx.snippet(span));
    if (position == MacroPosition::Item) snippet.push_back(';');
    return snippet;
}

std::vector<OverflowableItem> to_items(const std::vector<MacroArg*>& args) {
    std::vector<OverflowableItem> v;
    v.reserve(args.size());
    for (const MacroArg* a : args) v.push_back(RwNode::of(a));
    return v;
}

RewriteResult handle_vec_semi(const RewriteContext& ctx, Shape shape, const std::vector<MacroArg*>& arg_vec,
                              const std::string& macro_name, Delimiter delim_token, Span span) {
    const char* left;
    const char* right;
    switch (delim_token) {
    case Delimiter::Parenthesis: left = "("; right = ")"; break;
    case Delimiter::Bracket: left = "["; right = "]"; break;
    default: panic("unreachable: handle_vec_semi delimiter", span.lo);
    }

    // Should we return MaxWidthError, Or Macro failure
    std::optional<Shape> mac_shape = shape.offset_left(macro_name.size());
    if (!mac_shape) return Err(RewriteError::exceeds(shape.width, span));
    // 8 = `vec![]` + `; ` or `vec!()` + `; `
    size_t total_overhead = 8;
    Shape nested_shape = mac_shape->block_indent(ctx.config->tab_spaces());
    RewriteResult lhs = rewrite_result(ctx, arg_vec[0], nested_shape);
    if (!lhs) return lhs;
    RewriteResult rhs = rewrite_result(ctx, arg_vec[1], nested_shape);
    if (!rhs) return rhs;
    if (!contains(*lhs, '\n') && !contains(*rhs, '\n') && lhs->size() + rhs->size() + total_overhead <= shape.width) {
        // macro_name(lhs; rhs) or macro_name[lhs; rhs]
        return macro_name + left + *lhs + "; " + *rhs + right;
    }
    // macro_name(\nlhs;\nrhs\n) or macro_name[\nlhs;\nrhs\n]
    return macro_name + left + nested_shape.indent.to_string_with_newline(*ctx.config) + *lhs + ";" +
           nested_shape.indent.to_string_with_newline(*ctx.config) + *rhs +
           shape.indent.to_string_with_newline(*ctx.config) + right;
}

/// Format `lazy_static!` and `lazy_static::lazy_static!`
/// from <https://crates.io/crates/lazy_static>.
RewriteResult format_lazy_static(const RewriteContext& ctx, Shape shape, uint32_t tok_lo, uint32_t tok_hi, Span span,
                                 const std::string& macro_name) {
    std::string result;
    result.reserve(1024);
    Shape nested_shape = shape.block_indent(ctx.config->tab_spaces()).with_max_width(*ctx.config);

    result += macro_name;
    result += " {";
    result += nested_shape.indent.to_string_with_newline(*ctx.config);

    std::optional<std::vector<LazyStaticElem>> parsed_elems = parse_lazy_static(ctx, tok_lo, tok_hi);
    if (!parsed_elems) return Err(RewriteError::macro_failure(MacroErrorKind::ParseFailure, span));
    size_t last = parsed_elems->size() - 1;
    for (size_t i = 0; i < parsed_elems->size(); i++) {
        const LazyStaticElem& el = (*parsed_elems)[i];
        // Rewrite as a static item.
        std::string vis = format_visibility(ctx, el.vis);
        RewriteResult ty = rewrite_result(ctx, el.ty, nested_shape);
        if (!ty) return ty;
        std::string stmt = vis + "static ref " + el.id + ": " + *ty + " =";
        std::optional<Shape> rhs_shape = nested_shape.sub_width(1);
        if (!rhs_shape) return Err(RewriteError::exceeds(nested_shape.width, el.expr->span));
        RewriteResult rhs =
            rewrite_assign_rhs(ctx, std::move(stmt), RwNode::of(el.expr), RhsAssignKind::of_expr(el.expr), *rhs_shape);
        if (!rhs) return rhs;
        result += *rhs;
        result.push_back(';');
        if (i != last) result += nested_shape.indent.to_string_with_newline(*ctx.config);
    }

    result += shape.indent.to_string_with_newline(*ctx.config);
    result.push_back('}');
    return result;
}

RewriteResult rewrite_macro_with_items(const RewriteContext& ctx, const std::vector<MacroArg*>& items,
                                       const std::string& macro_name, Shape shape, Delimiter style,
                                       Delimiter original_style, MacroPosition position, Span span) {
    auto style_to_delims = [](Delimiter s, const char*& open, const char*& close) {
        switch (s) {
        case Delimiter::Parenthesis: open = "("; close = ")"; return true;
        case Delimiter::Bracket: open = "["; close = "]"; return true;
        case Delimiter::Brace: open = " {"; close = "}"; return true;
        default: return false;
        }
    };

    const char* opener = "";
    const char* closer = "";
    if (!style_to_delims(style, opener, closer)) return Err(RewriteError::unknown());
    const char* original_opener = "";
    const char* original_closer = "";
    if (!style_to_delims(original_style, original_opener, original_closer)) return Err(RewriteError::unknown());
    const char* trailing_semicolon =
        ((style == Delimiter::Parenthesis || style == Delimiter::Bracket) && position == MacroPosition::Item) ? ";"
                                                                                                             : "";

    FmtVisitor visitor = FmtVisitor::from_context(ctx);
    visitor.block_indent = shape.indent.block_indent_(*ctx.config);

    // The current opener may be different from the original opener. This can happen
    // if our macro is a forced bracket macro originally written with non-bracket
    // delimiters. We need to use the original opener to locate the span after it.
    visitor.last_pos = ctx.snippet_provider->span_after(span, trim(original_opener));
    for (const MacroArg* item : items) {
        if (item->kind != MacroArg::Item) return Err(RewriteError::unknown());
        visitor.visit_item(item->item);
    }

    std::string result;
    result.reserve(256);
    result += macro_name;
    result += opener;
    result += visitor.block_indent.to_string_with_newline(*ctx.config);
    result += trim(visitor.buffer);
    result += shape.indent.to_string_with_newline(*ctx.config);
    result += closer;
    result += trailing_semicolon;
    return result;
}

RewriteResult rewrite_macro_inner(const rust::MacCall* mac, const RewriteContext& ctx, Shape shape,
                                  MacroPosition position, bool is_nested_macro) {
    // non-default config: use_try_shorthand (convert_try_mac + rewrite as `e?`)

    Span mspan = mac_span(mac);
    Delimiter original_style = macro_style(mac, ctx);

    std::string macro_name = rewrite_macro_name(ctx, mac->path);
    bool is_forced_bracket = false;
    for (const char* m : FORCED_BRACKET_MACROS)
        if (macro_name == m) is_forced_bracket = true;

    Delimiter style = (is_forced_bracket && !is_nested_macro) ? Delimiter::Bracket : original_style;

    uint32_t ts_lo = mac->args.tok_lo;
    uint32_t ts_hi = mac->args.tok_hi;
    bool has_comment = contains_comment(ctx.snippet(mspan));
    if (ts_lo >= ts_hi && !has_comment) {
        switch (style) {
        case Delimiter::Parenthesis:
            if (position == MacroPosition::Item) return macro_name + "();";
            return macro_name + "()";
        case Delimiter::Bracket:
            if (position == MacroPosition::Item) return macro_name + "[];";
            return macro_name + "[]";
        case Delimiter::Brace: return macro_name + " {}";
        default: panic("unreachable: macro delimiter", mspan.lo);
        }
    }
    // Format well-known macros which cannot be parsed as a valid AST.
    // (style_edition 2027 also accepts `lazy_static::lazy_static!`: never taken.)
    if (macro_name == "lazy_static!" && !has_comment) {
        RewriteResult rw = format_lazy_static(ctx, shape, ts_lo, ts_hi, mspan, macro_name);
        if (rw) return rw;
        const RewriteError& err = rw.error();
        // We will move on to parsing macro args just like other macros
        // if we could not parse lazy_static! with known syntax
        if (!(err.kind == RewriteError::MacroFailure && err.macro_kind == MacroErrorKind::ParseFailure)) {
            // If formatting fails even though parsing succeeds, return the err early
            return rw;
        }
    }

    std::optional<ParsedMacroArgs> parsed = parse_macro_args(ctx, ts_lo, ts_hi, style, is_forced_bracket);
    if (!parsed) return return_macro_parse_failure_fallback(ctx, shape.indent, position, mspan);
    std::vector<MacroArg*>& arg_vec = parsed->args;
    bool vec_with_semi = parsed->vec_with_semi;
    bool trailing_comma = parsed->trailing_comma;

    if (!arg_vec.empty()) {
        bool all_items = true;
        for (const MacroArg* a : arg_vec)
            if (!a->is_item()) all_items = false;
        if (all_items)
            return rewrite_macro_with_items(ctx, arg_vec, macro_name, shape, style, original_style, position, mspan);
    }

    switch (style) {
    case Delimiter::Parenthesis: {
        // Handle special case: `vec!(expr; expr)`
        if (vec_with_semi) return handle_vec_semi(ctx, shape, arg_vec, macro_name, style, mspan);
        // Format macro invocation as function call, preserve the trailing
        // comma because not all macros support them.
        RewriteResult rw = rewrite_with_parens(ctx, macro_name, to_items(arg_vec), shape, mspan,
                                               ctx.config->fn_call_width(),
                                               trailing_comma ? SeparatorTactic::Always : SeparatorTactic::Never);
        if (!rw) return rw;
        if (position == MacroPosition::Item) return *rw + ";";
        return rw;
    }
    case Delimiter::Bracket: {
        // Handle special case: `vec![expr; expr]`
        if (vec_with_semi) return handle_vec_semi(ctx, shape, arg_vec, macro_name, style, mspan);
        // If we are rewriting `vec!` macro or other special macros,
        // then we can rewrite this as a usual array literal.
        // Otherwise, we must preserve the original existence of trailing comma.
        std::optional<SeparatorTactic> force_trailing_comma =
            trailing_comma ? SeparatorTactic::Always : SeparatorTactic::Never;
        if (is_forced_bracket && !is_nested_macro) {
            ctx.leave_macro();
            if (ctx.use_block_indent()) force_trailing_comma = SeparatorTactic::Vertical;
        }
        RewriteResult rewrite =
            rewrite_array(macro_name, to_items(arg_vec), mspan, ctx, shape, force_trailing_comma, original_style);
        if (!rewrite) return rewrite;
        const char* comma = position == MacroPosition::Item ? ";" : "";
        return *rewrite + comma;
    }
    case Delimiter::Brace: {
        // For macro invocations with braces, always put a space between
        // the `macro_name!` and `{ /* macro_body */ }` but skip modifying
        // anything in between the braces (for now).
        std::string_view snippet = ctx.snippet(mspan);
        size_t b = snippet.find('{');
        snippet = b == std::string_view::npos ? std::string_view() : snippet.substr(b);
        Opt macro_body = trim_left_preserve_layout(snippet, shape.indent, *ctx.config);
        if (macro_body) return macro_name + " " + *macro_body;
        return macro_name + " " + std::string(snippet);
    }
    default: panic("unreachable: macro style", mspan.lo);
    }
}

RewriteResult rewrite_empty_macro_def_body(const RewriteContext& ctx, Span span, Shape shape) {
    // Create an empty, dummy `ast::Block` representing an empty macro body
    rust::Block* block = ctx.arena().make<rust::Block>();
    block->rules = rust::BlockCheckMode::Default;
    block->span = span;
    return rewrite_result(ctx, block, shape);
}

void register_metavariable(std::map<std::string, std::string>& map, std::string& result, const std::string& name,
                           size_t dollar_count) {
    std::string new_name = repeat("$", dollar_count - 1);
    std::string old_name = repeat("$", dollar_count);

    new_name.push_back('z');
    new_name += name;
    old_name += name;

    result += new_name;
    map[old_name] = new_name;
}

// char::is_alphanumeric (approximated outside ASCII by XID_Continue minus `_`).
bool char_is_alphanumeric(uint32_t c) {
    if (c < 0x80) return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    return rust::is_id_continue(c);
}

// Replaces `$foo` with `zfoo`. We must check for name overlap to ensure we
// aren't causing problems.
// This should also work for escaped `$` variables, where we leave earlier `$`s.
std::optional<std::pair<std::string, std::map<std::string, std::string>>> replace_names(std::string_view input) {
    // Each substitution will require five or six extra bytes.
    std::string result;
    result.reserve(input.size() + 64);
    std::map<std::string, std::string> substs;
    size_t dollar_count = 0;
    std::string cur_name;

    for (const ClassifiedChar& cc : char_classes(input)) {
        std::string_view ch = input.substr(cc.pos, cc.len);
        uint32_t c = cc.ch;
        if (cc.kind != FullCodeCharKind::Normal) {
            result += ch;
        } else if (c == '$') {
            dollar_count += 1;
        } else if (dollar_count == 0) {
            result += ch;
        } else if (!char_is_alphanumeric(c) && !cur_name.empty()) {
            // Terminates a name following one or more dollars.
            register_metavariable(substs, result, cur_name, dollar_count);

            result += ch;
            dollar_count = 0;
            cur_name.clear();
        } else if (c == '(' && cur_name.empty()) {
            // FIXME: Support macro def with repeat.
            return std::nullopt;
        } else if (char_is_alphanumeric(c) || c == '_') {
            cur_name += ch;
        }
    }

    if (!cur_name.empty()) register_metavariable(substs, result, cur_name, dollar_count);

    return std::make_pair(std::move(result), std::move(substs));
}

// A token tree of a macro body (rustc's TokenTree): one token, or a
// delimited group [open, close]. `fat_arrow` is the synthetic `=>` rustc puts
// between `macro m(..)`'s params and body.
struct TT {
    bool delimited = false;
    bool fat_arrow = false;
    uint32_t open = 0;   // token index (Token: the token)
    uint32_t close = 0;  // Delimited: the closing delimiter's index
    Span entire;         // Delimited: open.lo .. close.hi; Token: its span
};

std::vector<TT> token_trees(const rust::ParseResult& pr, uint32_t lo, uint32_t hi) {
    std::vector<TT> out;
    uint32_t i = lo;
    while (i < hi) {
        const Token& t = pr.tokens[i];
        TT tt;
        if (is_open_delim(t.kind) && pr.partner[i] != UINT32_MAX && pr.partner[i] < hi) {
            tt.delimited = true;
            tt.open = i;
            tt.close = pr.partner[i];
            tt.entire = Span(t.span.lo, pr.tokens[tt.close].span.hi);
            i = tt.close + 1;
        } else {
            tt.open = i;
            tt.entire = t.span;
            i++;
        }
        out.push_back(tt);
    }
    return out;
}

/// The body token trees of a macro definition.
std::vector<TT> macro_def_body_trees(const rust::ParseResult& pr, const rust::Item* def) {
    const rust::DelimArgs& body = def->macro_body;
    // `macro m(params) {body}`: rustc builds `(params) => {body}`; mep keeps
    // the token range from inside `(` to before the final `}`.
    if (!def->macro_rules && body.open.is_empty() && body.tok_lo > 0 &&
        pr.tokens[body.tok_lo - 1].kind == Tok::OpenParen) {
        uint32_t p_open = body.tok_lo - 1;
        uint32_t p_close = pr.partner[p_open];
        if (p_close != UINT32_MAX && p_close + 1 < pr.tokens.size()) {
            uint32_t b_open = p_close + 1;
            uint32_t b_close = pr.partner[b_open];
            if (b_close != UINT32_MAX) {
                std::vector<TT> out(3);
                out[0].delimited = true;
                out[0].open = p_open;
                out[0].close = p_close;
                out[0].entire = Span(pr.tokens[p_open].span.lo, pr.tokens[p_close].span.hi);
                out[1].fat_arrow = true;
                out[1].entire = Span(pr.tokens[p_close].span.hi, pr.tokens[b_open].span.lo);
                out[2].delimited = true;
                out[2].open = b_open;
                out[2].close = b_close;
                out[2].entire = Span(pr.tokens[b_open].span.lo, pr.tokens[b_close].span.hi);
                return out;
            }
        }
    }
    return token_trees(pr, body.tok_lo, body.tok_hi);
}

struct MacroBranch {
    Span span;
    Delimiter args_paren_kind = Delimiter::Parenthesis;
    Span args;  // the matcher token tree's entire span (span_for_token_stream)
    Span body;
    Span whole_body;
};

// MacroParser::parse: (`(` ... `)` `=>` `{` ... `}`)*
std::optional<std::vector<MacroBranch>> macro_parser_parse(const rust::ParseResult& pr, const std::vector<TT>& tts) {
    std::vector<MacroBranch> branches;
    size_t i = 0;
    while (i < tts.size()) {
        // parse_branch: `(` ... `)` `=>` `{` ... `}`
        const TT& tok = tts[i++];
        if (!tok.delimited) return std::nullopt;
        MacroBranch br;
        uint32_t lo = tok.entire.lo;
        br.args_paren_kind = delim_of_tok(pr.tokens[tok.open].kind);
        br.args = tok.entire;
        if (i >= tts.size()) return std::nullopt;
        const TT& arrow = tts[i++];
        if (!(arrow.fat_arrow || (!arrow.delimited && pr.tokens[arrow.open].kind == Tok::FatArrow)))
            return std::nullopt;
        if (i >= tts.size()) return std::nullopt;
        const TT& b = tts[i++];
        if (!b.delimited) return std::nullopt;
        uint32_t hi = b.entire.hi;
        br.body = Span(b.entire.lo + 1, b.entire.hi - 1);
        br.whole_body = b.entire;
        if (i < tts.size() && !tts[i].delimited && !tts[i].fat_arrow && pr.tokens[tts[i].open].kind == Tok::Semi) {
            hi = tts[i].entire.hi;
            i++;
        }
        br.span = mk_sp(lo, hi);
        branches.push_back(br);
    }
    return branches;
}

// FormattedSnippet::is_line_non_formatted. non_formatted_ranges holds the
// (low, high) pairs flattened: [lo0, hi0, lo1, hi1, ...].
bool is_line_non_formatted(const FormattedSnippet& fs, size_t n) {
    const std::vector<size_t>& r = fs.non_formatted_ranges;
    for (size_t i = 0; i + 1 < r.size(); i += 2)
        if (r[i] <= n && n <= r[i + 1]) return true;
    return false;
}

// utils.rs indent_next_line
bool indent_next_line(FullCodeCharKind kind, std::string_view line, const Config& config) {
    if (fcck_is_string(kind)) {
        // If the string ends with '\', the string has been wrapped over
        // multiple lines. If `format_strings = true`, then the indentation of
        // strings wrapped over multiple lines will have been adjusted while
        // formatting the code block, therefore the string's indentation needs
        // to be adjusted for the code surrounding the code block.
        return config.format_strings() && ends_with(line, "\\");
    }
    if (config.style_edition() >= StyleEdition::E2024) return !fcck_is_commented_string(kind);
    return true;
}

// format_macro_args with format_macro_matchers = false (the default): the
// matcher as written. (The MacroArgParser path is non-default config.)
RewriteResult format_macro_args(const RewriteContext& ctx, Span args_span, Shape /*shape*/) {
    // non-default config: format_macro_matchers
    return std::string(ctx.snippet(args_span));
}

std::string replace_all(std::string_view s, std::string_view from, std::string_view to) {
    std::string out;
    if (from.empty()) return std::string(s);
    size_t i = 0;
    for (;;) {
        size_t j = s.find(from, i);
        if (j == std::string_view::npos) {
            out += s.substr(i);
            break;
        }
        out += s.substr(i, j - i);
        out += to;
        i = j + from.size();
    }
    return out;
}

RewriteResult macro_branch_rewrite(const MacroBranch& br, const RewriteContext& ctx, Shape shape,
                                   bool multi_branch_style) {
    // Only attempt to format function-like macros.
    if (br.args_paren_kind != Delimiter::Parenthesis) {
        // FIXME(#1539): implement for non-sugared macros.
        return Err(RewriteError::macro_failure(MacroErrorKind::Unknown, br.span));
    }

    std::string_view old_body = trim(ctx.snippet(br.body));
    bool has_block_body = starts_with(old_body, "{");
    size_t prefix_width = 5;  // 5 = " => {"
    if (ctx.config->style_edition() >= StyleEdition::E2024) {
        if (has_block_body) prefix_width = 6;  // 6 = " => {{"
    }
    std::optional<Shape> args_shape = shape.sub_width(prefix_width);
    if (!args_shape) return Err(RewriteError::exceeds(shape.width, br.span));
    RewriteResult args = format_macro_args(ctx, br.args, *args_shape);
    if (!args) return args;
    std::string result = std::move(*args);

    if (multi_branch_style) result += " =>";

    // non-default config: format_macro_bodies = false (append the whole body as written)

    // The macro body is the most interesting part. It might end up as various
    // AST nodes, but also has special variables (e.g, `$foo`) which can't be
    // parsed as regular Rust code (and note that these can be escaped using
    // `$$`). We'll try and format like an AST node, but we'll substitute
    // variables for new names with the same length first.
    auto replaced = replace_names(old_body);
    if (!replaced) return Err(RewriteError::macro_failure(MacroErrorKind::ReplaceMacroVariable, br.span));
    const std::string& body_str = replaced->first;
    const std::map<std::string, std::string>& substs = replaced->second;

    Config config = *ctx.config;
    // config.set().show_parse_errors(false): mep's format_snippet reports nothing.

    result += " {";

    Indent body_indent = has_block_body ? shape.indent : shape.indent.block_indent_(config);
    size_t new_width = config.max_width() - body_indent.width();
    config.max_width_ = new_width;

    // First try to format as items, then as statements.
    std::optional<FormattedSnippet> new_body_snippet = format_snippet(body_str, config, true, *ctx.state);
    if (!new_body_snippet) {
        size_t new_width2 = new_width + config.tab_spaces();
        config.max_width_ = new_width2;
        new_body_snippet = format_code_block(body_str, config, true, *ctx.state);
        if (!new_body_snippet) return Err(RewriteError::macro_failure(MacroErrorKind::Unknown, br.span));
    }

    if (!filtered_str_fits(new_body_snippet->snippet, config.max_width(), shape))
        return Err(RewriteError::exceeds(shape.width, br.span));

    // Indent the body since it is in a block.
    std::string indent_str = body_indent.to_string(config);
    std::string new_body;
    {
        bool need_indent = true;
        size_t i = 0;
        for (const auto& kl : line_classes(trim_end(new_body_snippet->snippet))) {
            const std::string& l = kl.second;
            if (!is_empty_line(l) && need_indent && !is_line_non_formatted(*new_body_snippet, i + 1))
                new_body += indent_str;
            new_body += l;
            new_body += "\n";
            need_indent = indent_next_line(kl.first, l, config);
            i++;
        }
    }

    // Undo our replacement of macro variables.
    // FIXME: this could be *much* more efficient.
    for (const auto& on : substs) {
        const std::string& old_ = on.first;
        const std::string& new_ = on.second;
        if (contains(old_body, new_)) {
            return Err(RewriteError::macro_failure(MacroErrorKind::ReplaceMacroVariable, br.span));
        }
        new_body = replace_all(new_body, new_, old_);
    }

    if (has_block_body) {
        result += trim(new_body);
    } else if (!new_body.empty()) {
        result += "\n";
        result += new_body;
        result += shape.indent.to_string(config);
    }

    result += "}";

    return result;
}

}  // namespace

// ===========================================================================
// Exported.

Span spanned(const MacroArg* x) {
    switch (x->kind) {
    case MacroArg::Expr: return spanned(x->expr);
    case MacroArg::Ty: return spanned(x->ty);
    case MacroArg::Pat: return spanned(x->pat);
    case MacroArg::Item: return spanned(x->item);
    case MacroArg::Keyword: return x->span;
    }
    return Span();
}

// impl Rewrite for ast::Item
RewriteResult rewrite_result(const RewriteContext& ctx, const rust::Item* x, Shape shape) {
    FmtVisitor visitor = FmtVisitor::from_context(ctx);
    visitor.block_indent = shape.indent;
    visitor.last_pos = spanned(x).lo;
    visitor.visit_item(x);
    return visitor.buffer;
}

// impl Rewrite for MacroArg
RewriteResult rewrite_result(const RewriteContext& ctx, const MacroArg* x, Shape shape) {
    switch (x->kind) {
    case MacroArg::Expr: return rewrite_result(ctx, static_cast<const rust::Expr*>(x->expr), shape);
    case MacroArg::Ty: return rewrite_result(ctx, static_cast<const rust::Ty*>(x->ty), shape);
    case MacroArg::Pat: return rewrite_result(ctx, static_cast<const rust::Pat*>(x->pat), shape);
    case MacroArg::Item: return rewrite_result(ctx, static_cast<const rust::Item*>(x->item), shape);
    case MacroArg::Keyword: return std::string(x->keyword.name);
    }
    return Err(RewriteError::unknown());
}

// `span` is the enclosing node's span; rustfmt uses mac.span() (the path to
// the closing delimiter), which is what is used here.
RewriteResult rewrite_macro(const rust::MacCall* mac, const RewriteContext& ctx, Shape shape, MacroPosition position,
                            Span /*span*/) {
    bool should_skip = ctx.skip_context.macros.skip(ctx.snippet(mac->path->span));
    if (should_skip) return Err(RewriteError::skip());

    DepthGuard g(*ctx.state);
    InsideMacroGuard guard(ctx);
    try {
        RewriteResult rw = rewrite_macro_inner(mac, ctx, shape, position, guard.is_nested());
        if (!rw) ctx.macro_rewrite_failure = true;
        return rw;
    } catch (const FormatPanic& e) {
        // The depth cap and the work budget are mep's own limits: let them through.
        std::string_view what = e.what();
        if (what == "nesting too deep" || what == "formatter work budget exceeded") throw;
        // catch_unwind: a panic inside the macro rewrite.
        ctx.macro_rewrite_failure = true;
        return Err(RewriteError::macro_failure(MacroErrorKind::Unknown, mac_span(mac)));
    }
}

RewriteResult rewrite_macro_def(const RewriteContext& ctx, Shape shape, Indent indent, const rust::Item* def,
                                Span span) {
    RewriteResult snippet = remove_trailing_white_spaces(ctx.snippet(span));
    if (ends_with(*snippet, ";")) return snippet;

    std::vector<TT> tts = macro_def_body_trees(*ctx.pr, def);
    std::optional<std::vector<MacroBranch>> parsed_def = macro_parser_parse(*ctx.pr, tts);
    if (!parsed_def) return snippet;
    const std::vector<MacroBranch>& branches = *parsed_def;

    std::string result = def->macro_rules ? std::string("macro_rules!") : format_visibility(ctx, def->vis) + "macro";

    result += " ";
    result += rewrite_ident(ctx, def->ident);

    bool multi_branch_style = def->macro_rules || branches.size() != 1;

    Shape arm_shape =
        multi_branch_style ? shape.block_indent(ctx.config->tab_spaces()).with_max_width(*ctx.config) : shape;

    if (branches.empty()) {
        uint32_t lo = ctx.snippet_provider->span_before(span, "{");
        result += " ";
        RewriteResult body = rewrite_empty_macro_def_body(ctx, span.with_lo(lo), shape);
        if (!body) return body;
        result += *body;
        return result;
    }

    std::vector<ListItem> branch_items = itemize_list(
        *ctx.snippet_provider, branches.size(), "}", ";", [&](size_t i) { return branches[i].span.lo; },
        [&](size_t i) { return branches[i].span.hi; },
        [&](size_t i) -> RewriteResult {
            RewriteResult v = macro_branch_rewrite(branches[i], ctx, arm_shape, multi_branch_style);
            if (v) return v;
            // if the rewrite returned None because a macro could not be rewritten, then return the
            // original body
            if (ctx.macro_rewrite_failure) return std::string(trim(ctx.snippet(branches[i].body)));
            return v;
        },
        ctx.snippet_provider->span_after(span, "{"), span.hi, false);

    ListFormatting fmt(arm_shape, *ctx.config);
    fmt.separator(def->macro_rules ? ";" : "").trailing_separator(SeparatorTactic::Always).preserve_newline(true);

    if (multi_branch_style) {
        result += " {";
        result += arm_shape.indent.to_string_with_newline(*ctx.config);
    }

    RewriteResult list = write_list(branch_items, fmt);
    if (!list) return snippet;
    result += *list;

    if (multi_branch_style) {
        result += indent.to_string_with_newline(*ctx.config);
        result += "}";
    }

    return result;
}

/// Tries to convert a macro use into a short hand try expression. Returns null
/// when the macro is not an instance of `try!` (or parsing the inner expression
/// failed).
const rust::Expr* convert_try_mac(const rust::MacCall* mac, const RewriteContext& ctx) {
    std::string path = path_to_string(ctx, mac->path);
    if (path == "try" || path == "r#try") {
        rust::Expr* inner = parse_expr(ctx, mac->args.tok_lo, mac->args.tok_hi);
        if (!inner) return nullptr;
        rust::Expr* e = ctx.arena().make<rust::Expr>();
        e->kind = rust::ExprKind::Try;
        e->lhs = inner;
        e->span = mac_span(mac);  // incorrect span, but shouldn't matter too much
        return e;
    }
    return nullptr;
}

Delimiter macro_style(const rust::MacCall* mac, const RewriteContext& ctx) {
    std::string_view snippet = ctx.snippet(mac_span(mac));
    size_t paren_pos = find_uncommented(snippet, "(").value_or(SIZE_MAX);
    size_t bracket_pos = find_uncommented(snippet, "[").value_or(SIZE_MAX);
    size_t brace_pos = find_uncommented(snippet, "{").value_or(SIZE_MAX);

    if (paren_pos < bracket_pos && paren_pos < brace_pos) return Delimiter::Parenthesis;
    if (bracket_pos < brace_pos) return Delimiter::Bracket;
    return Delimiter::Brace;
}

}  // namespace rustfmt
