// rust_parse_item.cpp -- items: modules, functions, ADTs, traits, impls,
// extern blocks, macros, `use` trees, delegation.
//
// Ported from rustc_parse/rustc_lexer (rust-lang/rust 1.97.1, MIT OR
// Apache-2.0): rustc_parse/src/parser/item.rs, plus the parts of
// diagnostics.rs it uses (expect_semi, recover_colon_as_semi, consume_block,
// eat_to_tokens, maybe_consume_incorrect_semicolon, parameter_without_type,
// recover_arg_parse, recover_bad_self_param,
// eat_incorrect_doc_comment_for_param_type, deduplicate_recovered_params_names,
// dummy_arg). Diagnostics keep their primary message and span;
// suggestions, notes, labels, lints and feature gates are dropped.
#include <cctype>

#include "rust_parse_internal.h"

namespace rust {
namespace detail {

namespace {

bool ascii_eq_ignore_case(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        unsigned char x = static_cast<unsigned char>(a[i]);
        unsigned char y = static_cast<unsigned char>(b[i]);
        if (std::tolower(x) != std::tolower(y)) return false;
    }
    return true;
}

// ItemKind::descr
const char* item_descr(ItemKind k) {
    switch (k) {
    case ItemKind::ExternCrate: return "extern crate";
    case ItemKind::Use: return "`use` import";
    case ItemKind::Static: return "static item";
    case ItemKind::Const: return "constant item";
    case ItemKind::ConstBlock: return "const block";
    case ItemKind::Fn: return "function";
    case ItemKind::Mod: return "module";
    case ItemKind::ForeignMod: return "extern block";
    case ItemKind::GlobalAsm: return "global asm item";
    case ItemKind::TyAlias: return "type alias";
    case ItemKind::Enum: return "enum";
    case ItemKind::Struct: return "struct";
    case ItemKind::Union: return "union";
    case ItemKind::Trait: return "trait";
    case ItemKind::TraitAlias: return "trait alias";
    case ItemKind::MacCall: return "item macro invocation";
    case ItemKind::MacroDef: return "macro definition";
    case ItemKind::Impl: return "implementation";
    case ItemKind::Delegation: return "delegated function";
    case ItemKind::DelegationMac: return "delegation";
    }
    return "item";
}

// ItemKind::article
const char* item_article(ItemKind k) {
    switch (k) {
    case ItemKind::ExternCrate:
    case ItemKind::ForeignMod:
    case ItemKind::MacCall:
    case ItemKind::Enum:
    case ItemKind::Impl: return "an";
    default: return "a";
    }
}

std::string path_to_string(const Path* p) {
    std::string s;
    if (p == nullptr) return s;
    bool first = true;
    for (const PathSegment& seg : p->segments) {
        if (seg.ident.name == "{{root}}") {
            first = false;
            continue;
        }
        if (!first) s += "::";
        first = false;
        if (seg.ident.raw) s += "r#";
        s += std::string(seg.ident.name);
    }
    return s;
}

// pprust::vis_to_string(..).trim_end()
std::string vis_to_string(const Visibility& vis) {
    switch (vis.kind) {
    case Visibility::Public: return "pub";
    case Visibility::Restricted:
        return std::string("pub(") + (vis.shorthand ? "" : "in ") + path_to_string(vis.path) + ")";
    case Visibility::Inherited: return "";
    }
    return "";
}

}  // namespace

// ---------------------------------------------------------------------------
// Token helpers.

bool Parser::tok_is_keyword_case(const Token& t, Kw k, Case c) const {
    if (tok_is_keyword(t, k)) return true;
    return c == Case::Insensitive && t.kind == Tok::Ident && !t.raw && ascii_eq_ignore_case(t.sym, kw_str(k));
}

// Parser::tree_look_ahead(dist, looker): the token tree `dist` trees after the
// current token. Returns false (None) past the end of the enclosing
// delimited group; otherwise sets `out`.
bool Parser::tree_look_ahead(size_t dist, TreeAhead& out) const {
    uint32_t idx = st_.pos;  // the token after the current one
    for (size_t k = 1; k < dist; ++k) {
        if (idx >= tok_hi_) return false;
        Tok kind = toks_[idx].kind;
        if (kind == Tok::CloseParen || kind == Tok::CloseBrace || kind == Tok::CloseBracket || kind == Tok::Eof)
            return false;
        if (kind == Tok::OpenParen || kind == Tok::OpenBrace || kind == Tok::OpenBracket) {
            uint32_t p = partner_[idx];
            if (p == UINT32_MAX || p >= tok_hi_) return false;
            idx = p + 1;
        } else {
            ++idx;
        }
    }
    if (idx >= tok_hi_) return false;
    const Token& t = toks_[idx];
    if (t.kind == Tok::CloseParen || t.kind == Tok::CloseBrace || t.kind == Tok::CloseBracket || t.kind == Tok::Eof)
        return false;
    out.delimited = t.kind == Tok::OpenParen || t.kind == Tok::OpenBrace || t.kind == Tok::OpenBracket;
    out.tok = t;
    return true;
}

// ---------------------------------------------------------------------------
// diagnostics.rs pieces.

void Parser::expect_semi() {
    if (eat(EXP(Semi)) || recover_colon_as_semi()) return;
    expect(EXP(Semi));  // Error unconditionally
}

/// Consumes a colon in place of a semicolon when the next token starts a new line.
bool Parser::recover_colon_as_semi() {
    if (may_recover() && token.kind == Tok::Colon) {
        Span colon = token.span;
        bool next_line = look_ahead(1, [&](const Token& next) {
            return next.span.lo > colon.lo && is_multiline(Span(colon.lo, next.span.lo));
        });
        if (next_line) {
            emit("statements are terminated with a semicolon", token.span);
            bump();
            return true;
        }
    }
    return false;
}

bool Parser::maybe_consume_incorrect_semicolon(const Item* previous_item) {
    (void)previous_item;  // only changes a help message
    if (token.kind != Tok::Semi) return false;
    emit("expected item, found `;`", token.span);
    bump();
    return true;
}

// dummy_arg: a placeholder argument.
Param* Parser::dummy_arg(Ident ident) {
    Pat* pat = mk_pat(ident.span, PatKind::Ident);
    pat->ident = ident;
    Param* p = mk<Param>();
    p->pat = pat;
    p->ty = mk_ty(ident.span, TyKind::Err);
    p->span = ident.span;
    return p;
}

void Parser::eat_incorrect_doc_comment_for_param_type() {
    if (token.kind == Tok::DocComment) {
        emit("documentation comments cannot be applied to a function parameter's type", token.span);
        bump();
    } else if (token.kind == Tok::Pound && look_ahead(1, [](const Token& t) { return t.kind == Tok::OpenBracket; })) {
        Span lo = token.span;
        // Skip every token until next possible arg.
        while (token.kind != Tok::CloseBracket && token.kind != Tok::Eof) bump();
        Span sp = lo.to(token.span);
        bump();
        emit("attributes cannot be applied to a function parameter's type", sp);
    }
}

// parameter_without_type: Some(ident) -> true and `out` set.
bool Parser::parameter_without_type(Pat* pat, bool require_name, bool first_param, Ident& out) {
    (void)first_param;
    // If we find a pattern followed by an identifier, it could be an (incorrect)
    // C-style parameter declaration.
    if (check_ident() &&
        look_ahead(1, [](const Token& t) { return t.kind == Tok::Comma || t.kind == Tok::CloseParen; })) {
        // `fn foo(String s) {}`
        out = parse_ident_common(true);
        return true;
    }
    if (require_name &&
        (token.kind == Tok::Comma || token.kind == Tok::Lt || token.kind == Tok::CloseParen)) {
        Ident ident;
        if (pat->kind == PatKind::Ident) {
            ident = pat->ident;
        } else if (pat->kind == PatKind::Ref && pat->sub != nullptr && pat->sub->kind == PatKind::Ref) {
            // `fn foo(&&self)`-like: only a suggestion when it ends in a `self` path.
            const Pat* inner = pat;
            while (inner->kind == PatKind::Ref && inner->sub != nullptr) inner = inner->sub;
            if (inner->kind == PatKind::Path && inner->path != nullptr && !inner->path->segments.empty() &&
                inner->path->segments[0].ident.name == "self" && !inner->path->segments[0].ident.raw)
                return false;
            return false;
        } else if (pat->kind == PatKind::Ref && pat->sub != nullptr && pat->sub->kind == PatKind::Ident) {
            ident = pat->sub->ident;
        } else {
            return false;
        }
        if (token.kind == Tok::Lt) return false;
        out = ident;
        return true;
    }
    return false;
}

std::pair<Pat*, Ty*> Parser::recover_arg_parse() {
    Pat* pat = parse_pat_no_top_alt(Expected::ArgumentName, PatternLocation::None);
    expect(EXP(Colon));
    Ty* ty = parse_ty();
    emit("patterns aren't allowed in methods without bodies", pat->span);
    // Pretend the pattern is `_`, to avoid duplicate errors from AST validation.
    Pat* wild = mk_pat(pat->span, PatKind::Wild);
    return {wild, ty};
}

Param* Parser::recover_bad_self_param(Param* param) {
    Span span = param->pat->span;
    emit("unexpected `self` parameter in function", span);
    param->ty->kind = TyKind::Err;
    return param;
}

void Parser::deduplicate_recovered_params_names(std::vector<Param*>& fn_inputs) {
    std::vector<std::string_view> seen;
    for (Param* input : fn_inputs) {
        if (input->pat->kind == PatKind::Ident && input->ty->kind == TyKind::Err) {
            std::string_view name = input->pat->ident.name;
            bool dup = false;
            for (std::string_view s : seen)
                if (s == name) dup = true;
            if (dup) input->pat->kind = PatKind::Wild;
            seen.push_back(name);
        }
    }
}

// SourceMap::guess_head_span: the span up to the first `{`.
Span Parser::guess_head_span(Span sp) const {
    std::string_view t = text(sp);
    size_t brace = t.find('{');
    if (brace == std::string_view::npos) return sp;
    size_t end = brace;
    while (end > 0 && (t[end - 1] == ' ' || t[end - 1] == '\t' || t[end - 1] == '\n' || t[end - 1] == '\r')) --end;
    return Span(sp.lo, sp.lo + static_cast<uint32_t>(end));
}

// ---------------------------------------------------------------------------
// Modules.

/// Parses a source module as a crate. This is the main entry point for the parser.
Crate* Parser::parse_crate_mod() {
    ModParse m = parse_mod(EXP(Eof));
    Crate* c = mk<Crate>();
    c->attrs = std::move(m.attrs);
    c->items = std::move(m.items);
    c->inner_span = m.inner_span;
    return c;
}

/// Parses a `mod <foo> { ... }` or `mod <foo>;` item.
Item* Parser::parse_item_mod(AttrVec& attrs) {
    Safety safety = parse_safety(Case::Sensitive);
    expect_keyword(KW(Mod));
    Ident ident = parse_ident();
    Item* item = mk<Item>();
    item->kind = ItemKind::Mod;
    item->safety = safety;
    item->ident = ident;
    if (eat(EXP(Semi))) {
        item->mod_inline = false;
    } else {
        expect(EXP(OpenBrace));
        ModParse m = parse_mod(EXP(CloseBrace));
        for (Attribute& a : m.attrs) attrs.push_back(a);
        item->mod_inline = true;
        item->items = std::move(m.items);
        item->mod_inner_span = m.inner_span;
    }
    return item;
}

/// Parses the contents of a module (inner attributes followed by module items).
/// We exit once we hit `term` which can be either
/// - EOF (for files)
/// - `}` for mod items
Parser::ModParse Parser::parse_mod(ExpTok term) {
    Span lo = token.span;
    ModParse m;
    m.attrs = parse_inner_attributes();

    for (;;) {
        while (maybe_consume_incorrect_semicolon(m.items.empty() ? nullptr : m.items.back())) {
        }  // Eat all bad semicolons
        Item* item = parse_item(ForceCollect::No, AllowConstBlockItems::Yes);
        if (item == nullptr) break;
        m.items.push_back(item);
    }

    if (!eat(term)) {
        std::string token_str = token_descr(token);
        if (!maybe_consume_incorrect_semicolon(m.items.empty() ? nullptr : m.items.back())) {
            fail("expected item, found " + token_str, token.span);
        }
    }

    m.inner_span = lo.to(prev_token.span);
    return m;
}

Item* Parser::parse_item(ForceCollect force_collect, AllowConstBlockItems allow_const_block_items) {
    FnParseMode fn_parse_mode{ReqName::Always, FnContext::Free, true};
    return parse_item_(fn_parse_mode, force_collect, allow_const_block_items);
}

Item* Parser::parse_item_(FnParseMode fn_parse_mode, ForceCollect force_collect,
                          AllowConstBlockItems const_block_items_allowed) {
    recover_vcs_conflict_marker();
    AttrWrapper attrs = parse_outer_attributes();
    recover_vcs_conflict_marker();
    return parse_item_common(attrs, true, false, fn_parse_mode, force_collect, const_block_items_allowed);
}

Item* Parser::parse_item_common(AttrWrapper attrs_w, bool mac_allowed, bool attrs_allowed, FnParseMode fn_parse_mode,
                                ForceCollect force_collect, AllowConstBlockItems allow_const_block_items) {
    (void)force_collect;
    DepthGuard guard(*this);
    AttrVec attrs = std::move(attrs_w.attrs);
    Span lo = token.span;
    Visibility vis = parse_visibility(FollowedByType::No);
    Defaultness def = parse_defaultness();
    Item* item = parse_item_kind(attrs, mac_allowed, allow_const_block_items, lo, vis, def, fn_parse_mode,
                                 Case::Sensitive);
    if (item != nullptr) {
        error_on_unconsumed_default(def, item->kind);
        item->span = lo.to(prev_token.span);
        item->attrs = std::move(attrs);
        item->vis = vis;
        return item;
    }

    // At this point, we have failed to parse an item.
    if (vis.kind != Visibility::Inherited) {
        // MisspelledKw is a subdiagnostic.
        emit("visibility `" + vis_to_string(vis) + "` is not followed by an item", vis.span);
    }

    if (def.kind == Defaultness::Default) {
        emit("`default` is not followed by an item", def.span);
    } else if (def.kind == Defaultness::Final) {
        emit("`final` is not followed by an item", def.span);
    }

    if (!attrs_allowed) recover_attrs_no_item(attrs);
    return nullptr;
}

void Parser::error_on_unconsumed_default(Defaultness def, ItemKind kind) {
    if (def.kind == Defaultness::Default) {
        emit(std::string(item_article(kind)) + " " + item_descr(kind) + " cannot be `default`", def.span);
    } else if (def.kind == Defaultness::Final) {
        emit(std::string(item_article(kind)) + " " + item_descr(kind) + " cannot be `final`", def.span);
    }
}

/// Parses one of the items allowed by the flags.
Item* Parser::parse_item_kind(AttrVec& attrs, bool macros_allowed, AllowConstBlockItems allow_const_block_items,
                              Span lo, const Visibility& vis, Defaultness& def, FnParseMode fn_parse_mode, Case c) {
    bool check_pub = def.kind == Defaultness::Implicit;
    auto def_ = [&def] {
        Defaultness d = def;
        def = Defaultness();
        return d;
    };

    Item* info = nullptr;
    Constness const_kw;
    std::optional<Safety> static_safety;
    int reuse = 0;
    int macro_rules = 0;
    if (!is_use_closure() && eat_keyword_case(KW(Use), c)) {
        // USE ITEM
        info = parse_use_item();
    } else if (check_fn_front_matter(check_pub, c)) {
        // FUNCTION ITEM
        Defaultness defaultness = def_();
        FnParts f = parse_fn(attrs, fn_parse_mode, lo, vis, c);
        info = mk<Item>();
        info->kind = ItemKind::Fn;
        info->defaultness = defaultness;
        info->ident = f.ident;
        info->sig = f.sig;
        info->generics = std::move(f.generics);
        info->contract = f.contract;
        info->body = f.body;
    } else if (eat_keyword_case(KW(Extern), c)) {
        if (eat_keyword_case(KW(Crate), c)) {
            // EXTERN CRATE
            info = parse_item_extern_crate();
        } else {
            // EXTERN BLOCK
            info = parse_item_foreign_mod(attrs, Safety());
        }
    } else if (is_unsafe_foreign_mod()) {
        // EXTERN BLOCK
        Safety safety = parse_safety(Case::Sensitive);
        expect_keyword(KW(Extern));
        info = parse_item_foreign_mod(attrs, safety);
    } else if ((static_safety = parse_global_static_front_matter(c))) {
        // STATIC ITEM
        Mutability mutability = parse_mutability();
        info = parse_static_item(*static_safety, mutability);
    } else if (check_keyword_case(KW(Trait), c) || check_trait_front_matter()) {
        // TRAIT ITEM
        info = parse_item_trait(attrs, lo);
    } else if (check_impl_frontmatter(0)) {
        // IMPL ITEM
        info = parse_item_impl(attrs, def_(), false);
    } else if ((allow_const_block_items == AllowConstBlockItems::Yes ||
                allow_const_block_items == AllowConstBlockItems::DoesNotMatter) &&
               check_inline_const(0)) {
        // CONST BLOCK ITEM
        info = parse_const_block_item();
    } else if ((const_kw = parse_constness(c)).yes) {
        // CONST ITEM
        recover_const_mut(const_kw.span);
        recover_missing_kw_before_item();
        ConstParts cp = parse_const_item(false, const_kw.span);
        info = mk<Item>();
        info->kind = ItemKind::Const;
        info->defaultness = def_();
        info->ident = cp.ident;
        info->generics = std::move(cp.generics);
        info->ty = cp.ty;
        info->expr = cp.rhs;
    } else if ((reuse = is_reuse_item()) != 0) {
        info = parse_item_delegation(attrs, def_(), reuse);
    } else if (check_keyword_case(KW(Mod), c) || (check_keyword_case(KW(Unsafe), c) && is_keyword_ahead(1, {Kw::Mod}))) {
        // MODULE ITEM
        info = parse_item_mod(attrs);
    } else if (eat_keyword_case(KW(Type), c)) {
        Constness tc = parse_constness(c);
        if (tc.yes) {
            // TYPE CONST (mgca)
            recover_const_mut(tc.span);
            recover_missing_kw_before_item();
            ConstParts cp = parse_const_item(true, tc.span);
            info = mk<Item>();
            info->kind = ItemKind::Const;
            info->defaultness = def_();
            info->ident = cp.ident;
            info->generics = std::move(cp.generics);
            info->ty = cp.ty;
            info->expr = cp.rhs;
            info->type_const = cp.type_const;
        } else {
            // TYPE ITEM
            info = parse_type_alias(def_());
        }
    } else if (eat_keyword_case(KW(Enum), c)) {
        // ENUM ITEM
        info = parse_item_enum();
    } else if (eat_keyword_case(KW(Struct), c)) {
        // STRUCT ITEM
        info = parse_item_struct();
    } else if (is_kw_followed_by_ident(Kw::Union)) {
        // UNION ITEM
        bump();  // `union`
        info = parse_item_union();
    } else if (is_builtin()) {
        // parse_item_builtin: no builtin items are recognised.
        return nullptr;
    } else if (eat_keyword_case(KW(Macro), c)) {
        // MACROS 2.0 ITEM
        info = parse_item_decl_macro(lo);
    } else if ((macro_rules = is_macro_rules_item()) != 0) {
        // MACRO_RULES ITEM
        info = parse_item_macro_rules(vis, macro_rules == 1);
    } else if (isnt_macro_invocation() &&
               (tok_is_ident_named(token, "import") || tok_is_ident_named(token, "using") ||
                tok_is_ident_named(token, "include") || tok_is_ident_named(token, "require"))) {
        return recover_import_as_use();
    } else if (isnt_macro_invocation() && vis.kind == Visibility::Public) {
        recover_missing_kw_before_item();
        return nullptr;
    } else if (isnt_macro_invocation() && c == Case::Sensitive) {
        // Recover wrong cased keywords
        return parse_item_kind(attrs, macros_allowed, allow_const_block_items, lo, vis, def, fn_parse_mode,
                               Case::Insensitive);
    } else if (macros_allowed && check_path()) {
        if (isnt_macro_invocation()) recover_missing_kw_before_item();
        // MACRO INVOCATION ITEM
        MacCall* mac = parse_item_macro(vis);
        info = mk<Item>();
        info->kind = ItemKind::MacCall;
        info->mac = mac;
    } else {
        return nullptr;
    }
    return info;
}

Item* Parser::recover_import_as_use() {
    Span span = token.span;
    std::string token_name = token_descr(token);
    ParserState snapshot = create_snapshot_for_diagnostic();
    bump();
    try {
        Item* u = parse_use_item();
        emit("expected item, found " + token_name, span);
        return u;
    } catch (PErr&) {
        restore_snapshot(snapshot);
        return nullptr;
    }
}

Item* Parser::parse_use_item() {
    UseTree* tree = parse_use_tree();
    // expect_semi's error only gains a note on failure.
    expect_semi();
    Item* item = mk<Item>();
    item->kind = ItemKind::Use;
    item->use_tree = tree;
    return item;
}

/// When parsing a statement, would the start of a path be an item?
bool Parser::is_path_start_item() {
    return is_kw_followed_by_ident(Kw::Union)  // no: `union::b`, yes: `union U { .. }`
           || is_reuse_item() != 0              // yes: `reuse impl Trait for Struct { self.0 }`, yes: `reuse some_path::foo;`
           || check_trait_front_matter()       // no: `auto::b`, yes: `auto trait X { .. }`
           || is_async_fn()                    // no(2015): `async::b`, yes: `async fn`
           || is_macro_rules_item() != 0;      // no: `macro_rules::b`, yes: `macro_rules! mac`
}

// 0 = None, 1 = ReuseKind::Path, 2 = ReuseKind::Impl
int Parser::is_reuse_item() {
    if (!tok_is_keyword(token, Kw::Reuse)) return 0;
    // no: `reuse ::path` for compatibility reasons with macro invocations
    if (look_ahead(1, [this](const Token& t) { return tok_is_path_start(t) && t.kind != Tok::PathSep; })) return 1;
    if (check_impl_frontmatter(1)) return 2;
    return 0;
}

/// Are we sure this could not possibly be a macro invocation?
bool Parser::isnt_macro_invocation() {
    return check_ident() && look_ahead(1, [](const Token& t) { return t.kind != Tok::Bang && t.kind != Tok::PathSep; });
}

/// Recover on encountering a struct, enum, or method definition where the user
/// forgot to add the `struct`, `enum`, or `fn` keyword.
void Parser::recover_missing_kw_before_item() {
    bool is_pub = tok_is_keyword(prev_token, Kw::Pub);
    bool is_const = tok_is_keyword(prev_token, Kw::Const);
    Span ident_span = token.span;
    Span span = is_pub ? prev_token.span.to(ident_span) : ident_span;

    if (!(tok_is_ident(token) &&
          (!is_const || look_ahead(1, [](const Token& t) { return t.kind == Tok::OpenParen; })) &&
          look_ahead(1, [](const Token& t) {
              return t.kind == Tok::Lt || t.kind == Tok::OpenBrace || t.kind == Tok::OpenParen;
          })))
        return;
    (void)parse_ident_common(true);

    bool found_generics = false;
    if (check(EXP(Lt))) {
        found_generics = true;
        eat_to_tokens({EXP(Gt)});
        bump();  // `>`
    }

    const char* err = nullptr;
    if (check(EXP(OpenBrace))) {
        // possible struct or enum definition where `struct` or `enum` was forgotten
        if (look_ahead(1, [](const Token& t) { return t.kind == Tok::CloseBrace; })) {
            // `S {}` could be unit enum or struct
            err = "missing `enum` or `struct` for enum or struct definition";
        } else if (look_ahead(2, [](const Token& t) { return t.kind == Tok::Colon; }) ||
                   look_ahead(3, [](const Token& t) { return t.kind == Tok::Colon; })) {
            // `S { f:` or `S { pub f:`
            err = "missing `struct` for struct definition";
        } else {
            err = "missing `enum` for enum definition";
        }
    } else if (check(EXP(OpenParen))) {
        // possible function or tuple struct definition where `fn` or `struct` was forgotten
        bump();  // `(`
        bool is_method = recover_self_param();

        consume_block(EXP(OpenParen), EXP(CloseParen), true);

        if (check(EXP(RArrow)) || check(EXP(OpenBrace))) {
            eat_to_tokens({EXP(OpenBrace)});
            bump();  // `{`
            consume_block(EXP(OpenBrace), EXP(CloseBrace), true);
            err = is_method ? "missing `fn` for method definition" : "missing `fn` for function definition";
        } else if (is_pub && check(EXP(Semi))) {
            err = "missing `struct` for struct definition";
        } else {
            err = "missing `fn` or `struct` for function or struct definition";
        }
    } else if (found_generics) {
        err = "missing `fn` or `struct` for function or struct definition";
    }
    if (err != nullptr) fail(err, span);
}

/// Parses an item macro, e.g., `item!();`.
MacCall* Parser::parse_item_macro(const Visibility& vis) {
    Path* path = parse_path(PathStyle::Mod);  // `foo::bar`
    expect(EXP(Bang));                        // `!`
    DelimArgs args = parse_delim_args();      // its error only gains a suggestion
    eat_semi_for_macro_if_needed(args);
    complain_if_pub_macro(vis, false);
    MacCall* m = mk<MacCall>();
    m->path = path;
    m->args = args;
    return m;
}

/// Recover if we parsed attributes and expected an item but there was none.
void Parser::recover_attrs_no_item(const AttrVec& attrs) {
    if (attrs.empty()) return;
    const Attribute& end = attrs.back();
    const char* msg = end.is_doc_comment ? "expected item after doc comment" : "expected item after attributes";
    fail(msg, end.span);
}

bool Parser::is_async_fn() const { return tok_is_keyword(token, Kw::Async) && is_keyword_ahead(1, {Kw::Fn}); }

// parse_polarity: true for `!` (negative), with its span.
bool Parser::parse_polarity(Span& neg_span) {
    // Disambiguate `impl !Trait for Type { ... }` and `impl ! { ... }` for the never type.
    if (check(EXP(Bang)) && look_ahead(1, [this](const Token& t) { return tok_can_begin_type(t); })) {
        bump();  // `!`
        neg_span = prev_token.span;
        return true;
    }
    return false;
}

/// Parses an implementation item.
Item* Parser::parse_item_impl(AttrVec& attrs, Defaultness defaultness, bool is_reuse) {
    Constness constness = parse_constness(Case::Sensitive);
    Safety safety = parse_safety(Case::Sensitive);
    expect_keyword(KW(Impl));

    // First, parse generic parameters if necessary.
    Generics generics;
    if (choose_generics_over_qpath(0)) {
        generics = parse_generics();
    } else {
        generics.span = prev_token.span.shrink_to_hi();
    }

    if (!constness.yes) constness = parse_constness(Case::Sensitive);

    if ((edition_ >= Edition::E2018 && tok_is_keyword(token, Kw::Async)) || is_kw_followed_by_ident(Kw::Async)) {
        bump();
        emit("`async` trait implementations are unsupported", prev_token.span);
    }

    Span neg_span;
    bool negative = parse_polarity(neg_span);

    // Parse both types and traits as a type, then reinterpret if necessary.
    Ty* ty_first = nullptr;
    if (tok_is_keyword(token, Kw::For) && look_ahead(1, [](const Token& t) { return t.kind != Tok::Lt; })) {
        Span span = prev_token.span.between(token.span);
        fail("missing trait in a trait impl", span);
    } else {
        ty_first = parse_ty_with_generics_recovery(generics);
    }

    // If `for` is missing we try to recover.
    bool has_for = eat_keyword(KW(For));
    Span missing_for_span = prev_token.span.between(token.span);

    Ty* ty_second = nullptr;
    if (token.kind == Tok::DotDot) {
        // We need to report this error after `cfg` expansion for compatibility reasons
        bump();  // `..`, do not add it to expected tokens
        ty_second = mk_ty(prev_token.span, TyKind::Dummy);
    } else if (has_for || tok_can_begin_type(token)) {
        ty_second = parse_ty();
    }

    generics.where_clause = parse_where_clause();

    std::vector<Item*> impl_items;
    if (!is_reuse) impl_items = parse_item_list(attrs, ItemContext::Impl);

    Item* item = mk<Item>();
    item->kind = ItemKind::Impl;
    item->constness = constness;
    if (ty_second != nullptr) {
        // impl Trait for Type
        if (!has_for) emit("missing `for` in a trait impl", missing_for_span);
        Path* path = nullptr;
        if (ty_first->kind == TyKind::Path && ty_first->qself == nullptr) {
            path = ty_first->path;
        } else if (ty_first->kind == TyKind::ImplTrait && ty_first->bounds.size() == 1 &&
                   ty_first->bounds[0].kind == GenericBound::Trait) {
            const GenericBound& bound = ty_first->bounds[0];
            Span extra_impl_kw = ty_first->span.until(bound.span());
            emit("unexpected `impl` keyword", extra_impl_kw);
            path = bound.trait.trait_ref;
        } else {
            fail("expected a trait, found type", ty_first->span);
        }
        item->of_trait = true;
        item->defaultness = defaultness;
        item->safety = safety;
        item->negative = negative;
        item->negative_span = neg_span;
        item->trait_ref = path;
        item->ty = ty_second;
    } else {
        // impl Type
        Ty* self_ty = ty_first;
        if (safety.kind == Safety::Unsafe) emit("inherent impls cannot be unsafe", self_ty->span);
        if (negative) emit("inherent impls cannot be negative", self_ty->span);
        if (defaultness.kind == Defaultness::Default) emit("inherent impls cannot be default", self_ty->span);
        item->ty = self_ty;
    }
    item->generics = std::move(generics);
    item->items = std::move(impl_items);
    return item;
}

Item* Parser::parse_item_delegation(AttrVec& attrs, Defaultness defaultness, int kind) {
    Span span = token.span;
    expect_keyword(KW(Reuse));
    Item* item = kind == 1 ? parse_path_like_delegation() : parse_impl_delegation(span, attrs, defaultness);
    return item;
}

Block* Parser::parse_delegation_body() {
    if (check(EXP(OpenBrace))) return parse_block();
    expect(EXP(Semi));
    return nullptr;
}

Item* Parser::parse_impl_delegation(Span span, AttrVec& attrs, Defaultness defaultness) {
    Item* impl_item = parse_item_impl(attrs, defaultness, true);
    Span until_expr_span = span.to(prev_token.span);
    if (!impl_item->of_trait) fail("only trait impls can be reused", until_expr_span);

    Block* body = parse_delegation_body();
    Span whole_reuse_span = span.to(prev_token.span);

    Item* d = mk<Item>();
    d->kind = ItemKind::DelegationMac;
    d->ctx = ItemContext::Impl;
    d->span = whole_reuse_span;
    d->vis.kind = Visibility::Inherited;
    d->vis.span = whole_reuse_span;
    d->path = impl_item->trait_ref;
    d->glob = true;
    d->glob_span = whole_reuse_span;
    d->body = body;
    impl_item->items.push_back(d);
    return impl_item;
}

Item* Parser::parse_path_like_delegation() {
    QSelf* qself = nullptr;
    Path* path;
    if (eat_lt()) {
        std::pair<QSelf*, Path*> qp = parse_qpath(PathStyle::Expr);
        qself = qp.first;
        path = qp.second;
    } else {
        path = parse_path(PathStyle::Expr);
    }

    auto rename = [this](Ident& out) {
        if (eat_keyword(KW(As))) {
            out = parse_ident();
            return true;
        }
        return false;
    };

    Item* item = mk<Item>();
    item->qself = qself;
    item->path = path;
    if (eat_path_sep()) {
        item->kind = ItemKind::DelegationMac;
        if (eat(EXP(Star))) {
            item->glob = true;
            item->glob_span = prev_token.span;
        } else {
            item->suffixes = parse_delim_comma_seq<DelegationSuffix>(EXP(OpenBrace), EXP(CloseBrace), [&] {
                DelegationSuffix s;
                s.ident = parse_path_segment_ident();
                s.has_rename = rename(s.rename);
                return s;
            });
        }
        item->body = parse_delegation_body();
    } else {
        item->kind = ItemKind::Delegation;
        Ident r;
        bool has_rename = rename(r);
        item->has_rename = has_rename;
        item->rename = r;
        if (has_rename)
            item->ident = r;
        else if (!path->segments.empty())
            item->ident = path->segments.back().ident;
        item->body = parse_delegation_body();
    }
    return item;
}

// parse_item_list(attrs, |p| p.parse_{impl,trait,foreign}_item(ForceCollect::No))
std::vector<Item*> Parser::parse_item_list(AttrVec& attrs, ItemContext which) {
    std::vector<Item*> items;
    if (token.kind == Tok::Semi) {
        emit("expected `{}`, found `;`", token.span);
        bump();
        return items;
    }

    expect(EXP(OpenBrace));
    AttrVec inner = parse_inner_attributes();
    for (Attribute& a : inner) attrs.push_back(a);

    while (!eat(EXP(CloseBrace))) {
        if (recover_doc_comment_before_brace()) continue;
        recover_vcs_conflict_marker();
        std::pair<bool, Item*> r;
        try {
            switch (which) {
            case ItemContext::Impl: r = parse_impl_item(ForceCollect::No); break;
            case ItemContext::Trait: r = parse_trait_item(ForceCollect::No); break;
            default: r = parse_foreign_item(ForceCollect::No); break;
            }
        } catch (PErr& err) {
            consume_block(EXP(OpenBrace), EXP(CloseBrace), true);
            emit(err);
            break;
        }
        if (!r.first) {
            Span non_item_span = token.span;
            consume_block(EXP(OpenBrace), EXP(CloseBrace), true);
            emit("non-item in item list", non_item_span);
            break;
        }
        if (r.second != nullptr) items.push_back(r.second);
    }
    return items;
}

/// Recover on a doc comment before `}`.
bool Parser::recover_doc_comment_before_brace() {
    if (token.kind == Tok::DocComment) {
        if (look_ahead(1, [](const Token& t) { return t.kind == Tok::CloseBrace; })) {
            // FIXME: merge with `DocCommentDoesNotDocumentAnything` (E0585)
            emit("found a documentation comment that doesn't document anything", token.span);
            bump();
            return true;
        }
    }
    return false;
}

/// Parses defaultness (i.e., `default` or nothing).
Defaultness Parser::parse_defaultness() {
    Defaultness d;
    // We are interested in `default` followed by another identifier.
    // However, we must avoid keywords that occur as binary operators.
    // Currently, the only applicable keyword is `as` (`default as Ty`).
    if (check_keyword(KW(Default)) &&
        look_ahead(1, [](const Token& t) { return t.kind == Tok::Ident && !t.raw && t.sym != "as"; })) {
        bump();  // `default`
        d.kind = Defaultness::Default;
        d.span = prev_token_uninterpolated_span();
    } else if (eat_keyword(KW(Final))) {
        d.kind = Defaultness::Final;
        d.span = prev_token_uninterpolated_span();
    }
    return d;
}

/// Is this an `(const unsafe? auto? | unsafe auto? | auto) trait` item?
bool Parser::check_trait_front_matter() {
    static const std::vector<std::vector<Kw>> SUFFIXES = {
        {Kw::Trait},
        {Kw::Auto, Kw::Trait},
        {Kw::Unsafe, Kw::Trait},
        {Kw::Unsafe, Kw::Auto, Kw::Trait},
        {Kw::Const, Kw::Trait},
        {Kw::Const, Kw::Auto, Kw::Trait},
        {Kw::Const, Kw::Unsafe, Kw::Trait},
        {Kw::Const, Kw::Unsafe, Kw::Auto, Kw::Trait},
    };
    // `impl(`
    if (check_keyword(KW(Impl)) && look_ahead(1, [](const Token& t) { return t.kind == Tok::OpenParen; })) {
        // `impl(in` unambiguously introduces an `impl` restriction
        if (is_keyword_ahead(2, {Kw::In})) return true;
        // `impl(crate | self | super)` + SUFFIX
        if (is_keyword_ahead(2, {Kw::Crate, Kw::SelfLower, Kw::Super}) &&
            look_ahead(3, [](const Token& t) { return t.kind == Tok::CloseParen; })) {
            for (const std::vector<Kw>& suffix : SUFFIXES) {
                bool all = true;
                for (size_t i = 0; i < suffix.size() && all; ++i) all = is_keyword_ahead(i + 4, {suffix[i]});
                if (all) return true;
            }
        }
        // Recover cases like `impl(path::to::module)` + SUFFIX to suggest inserting `in`.
        for (const std::vector<Kw>& suffix : SUFFIXES) {
            bool all = true;
            for (size_t i = 0; i < suffix.size() && all; ++i) {
                TreeAhead tt;
                all = tree_look_ahead(i + 2, tt) && !tt.delimited && tok_is_keyword(tt.tok, suffix[i]);
            }
            if (all) return true;
        }
        return false;
    }
    for (const std::vector<Kw>& suffix : SUFFIXES) {
        bool all = true;
        for (size_t i = 0; i < suffix.size() && all; ++i) {
            if (i == 0) {
                switch (suffix[i]) {
                case Kw::Const: all = check_keyword(KW(Const)); break;
                case Kw::Unsafe: all = check_keyword(KW(Unsafe)); break;
                case Kw::Auto: all = check_keyword(KW(Auto)); break;
                default: all = check_keyword(KW(Trait)); break;
                }
            } else {
                all = is_keyword_ahead(i, {suffix[i]});
            }
        }
        if (all) return true;
    }
    return false;
}

/// Parses `[impl(in path)]? unsafe? auto? trait Foo { ... }` or `trait Foo = Bar;`.
Item* Parser::parse_item_trait(AttrVec& attrs, Span lo) {
    ImplRestriction impl_restriction = parse_impl_restriction();
    Constness constness = parse_constness(Case::Sensitive);
    Safety safety = parse_safety(Case::Sensitive);
    // Parse optional `auto` prefix.
    bool is_auto = eat_keyword(KW(Auto));

    expect_keyword(KW(Trait));
    Ident ident = parse_ident();
    Generics generics = parse_generics();

    // Parse optional colon and supertrait bounds.
    bool had_colon = eat(EXP(Colon));
    Span span_at_colon = prev_token.span;
    GenericBounds bounds;
    if (had_colon) bounds = parse_generic_bounds();

    Span span_before_eq = prev_token.span;
    Item* item = mk<Item>();
    item->ident = ident;
    item->constness = constness;
    if (eat(EXP(Eq))) {
        // It's a trait alias.
        if (had_colon) emit("bounds are not allowed on trait aliases", span_at_colon.to(span_before_eq));

        GenericBounds alias_bounds = parse_generic_bounds();
        generics.where_clause = parse_where_clause();
        expect_semi();

        Span whole_span = lo.to(prev_token.span);
        if (is_auto) emit("trait aliases cannot be `auto`", whole_span);
        if (safety.kind == Safety::Unsafe) emit("trait aliases cannot be `unsafe`", whole_span);
        if (impl_restriction.restricted) emit("trait aliases cannot be `impl`-restricted", whole_span);

        item->kind = ItemKind::TraitAlias;
        item->generics = std::move(generics);
        item->bounds = std::move(alias_bounds);
    } else {
        // It's a normal trait.
        generics.where_clause = parse_where_clause();
        std::vector<Item*> items = parse_item_list(attrs, ItemContext::Trait);
        item->kind = ItemKind::Trait;
        item->impl_restriction = impl_restriction;
        item->is_auto = is_auto;
        item->safety = safety;
        item->generics = std::move(generics);
        item->bounds = std::move(bounds);
        item->items = std::move(items);
    }
    return item;
}

std::pair<bool, Item*> Parser::parse_impl_item(ForceCollect force_collect) {
    FnParseMode fn_parse_mode{ReqName::Always, FnContext::Impl, true};
    return parse_assoc_item(fn_parse_mode, force_collect, ItemContext::Impl);
}

std::pair<bool, Item*> Parser::parse_trait_item(ForceCollect force_collect) {
    FnParseMode fn_parse_mode{ReqName::Edition2018, FnContext::Trait, false};
    return parse_assoc_item(fn_parse_mode, force_collect, ItemContext::Trait);
}

/// Parses associated items.
std::pair<bool, Item*> Parser::parse_assoc_item(FnParseMode fn_parse_mode, ForceCollect force_collect,
                                                 ItemContext ctx) {
    Item* item = parse_item_(fn_parse_mode, force_collect, AllowConstBlockItems::DoesNotMatter);
    if (item == nullptr) return {false, nullptr};
    switch (item->kind) {
    case ItemKind::Const:
    case ItemKind::Fn:
    case ItemKind::TyAlias:
    case ItemKind::MacCall:
    case ItemKind::Delegation:
    case ItemKind::DelegationMac: break;
    case ItemKind::Static:
        emit("associated `static` items are not allowed", item->span);
        item->kind = ItemKind::Const;
        item->defaultness = Defaultness();
        item->generics = Generics();
        item->safety = Safety();
        item->mutbl = Mutability::Not;
        break;
    default: return {true, error_bad_item_kind(item->span, item->kind, "`trait`s or `impl`s")};
    }
    item->ctx = ctx;
    return {true, item};
}

/// Parses a `type` alias with the following grammar:
/// ```ebnf
/// TypeAlias = "type" Ident Generics (":" GenericBounds)? WhereClause ("=" Ty)? WhereClause ";" ;
/// ```
/// The `"type"` has already been eaten.
Item* Parser::parse_type_alias(Defaultness defaultness) {
    Ident ident = parse_ident();
    Generics generics = parse_generics();

    // Parse optional colon and param bounds.
    GenericBounds bounds;
    if (eat(EXP(Colon))) bounds = parse_generic_bounds();
    generics.where_clause = parse_where_clause();

    Ty* ty = nullptr;
    if (eat(EXP(Eq))) ty = parse_ty();

    WhereClause after_where_clause = parse_where_clause();

    expect_semi();

    Item* item = mk<Item>();
    item->kind = ItemKind::TyAlias;
    item->defaultness = defaultness;
    item->ident = ident;
    item->generics = std::move(generics);
    item->after_where_clause = std::move(after_where_clause);
    item->bounds = std::move(bounds);
    item->ty = ty;
    return item;
}

/// Parses a `UseTree`.
///
/// ```text
/// USE_TREE = [`::`] `*` |
///            [`::`] `{` USE_TREE_LIST `}` |
///            PATH `::` `*` |
///            PATH `::` `{` USE_TREE_LIST `}` |
///            PATH [`as` IDENT]
/// ```
UseTree* Parser::parse_use_tree() {
    Span lo = token.span;

    UseTree* tree = mk<UseTree>();
    Path* prefix = mk_path(lo.shrink_to_lo());
    if (check(EXP(OpenBrace)) || check(EXP(Star)) || is_import_coupler()) {
        // `use *;` or `use ::*;` or `use {...};` or `use ::{...};`
        if (eat_path_sep()) {
            PathSegment root;
            root.ident.name = "{{root}}";
            root.ident.span = lo.shrink_to_lo();
            prefix->segments.push_back(root);
        }
        parse_use_tree_glob_or_nested(tree);
    } else {
        // `use path::*;` or `use path::{...};` or `use path;` or `use path as bar;`
        prefix = parse_path(PathStyle::Mod);

        if (eat_path_sep()) {
            parse_use_tree_glob_or_nested(tree);
        } else {
            // Recover from using a colon as path separator.
            while (eat_noexpect(Tok::Colon)) {
                emit("expected `::`, found `:`", prev_token.span);

                // We parse the rest of the path and append it to the original prefix.
                parse_path_segments(prefix->segments, PathStyle::Mod, nullptr);
                prefix->span = lo.to(prev_token.span);
            }

            tree->kind = UseTree::Simple;
            tree->has_rename = parse_rename(tree->rename);
        }
    }
    tree->prefix = prefix;
    tree->span = lo.to(prev_token.span);
    return tree;
}

/// Parses `*` or `{...}`.
void Parser::parse_use_tree_glob_or_nested(UseTree* tree) {
    if (eat(EXP(Star))) {
        tree->kind = UseTree::Glob;
        tree->glob_span = prev_token.span;
    } else {
        Span lo = token.span;
        tree->kind = UseTree::Nested;
        tree->items = parse_use_tree_list();
        tree->nested_span = lo.to(prev_token.span);
    }
}

/// Parses a `UseTreeKind::Nested(list)`.
///
/// ```text
/// USE_TREE_LIST = ∅ | (USE_TREE `,`)* USE_TREE [`,`]
/// ```
std::vector<UseTree*> Parser::parse_use_tree_list() {
    return parse_delim_comma_seq<UseTree*>(EXP(OpenBrace), EXP(CloseBrace), [this] {
        recover_vcs_conflict_marker();
        return parse_use_tree();
    });
}

bool Parser::parse_rename(Ident& out) {
    if (eat_keyword(KW(As))) {
        out = parse_ident_or_underscore();
        return true;
    }
    return false;
}

Ident Parser::parse_ident_or_underscore() {
    if (token.kind == Tok::Ident && !token.raw && token.kw == Kw::Underscore) {
        Ident ident = tok_ident(token);
        bump();
        return ident;
    }
    return parse_ident();
}

/// Parses `extern crate` links.
///
/// # Examples
///
/// ```ignore (illustrative)
/// extern crate foo;
/// extern crate bar as foo;
/// ```
Item* Parser::parse_item_extern_crate() {
    // Accept `extern crate name-like-this` for better diagnostics
    Ident orig_ident = parse_crate_name_with_dashes();
    Item* item = mk<Item>();
    item->kind = ItemKind::ExternCrate;
    Ident rename;
    if (parse_rename(rename)) {
        item->has_orig_name = true;
        item->orig_name = orig_ident;
        item->ident = rename;
    } else {
        item->ident = orig_ident;
    }
    expect_semi();
    return item;
}

Ident Parser::parse_crate_name_with_dashes() {
    Ident ident = tok_is_keyword(token, Kw::SelfLower) ? parse_path_segment_ident() : parse_ident();

    if (token.kind != Tok::Minus) return ident;

    // Accept `extern crate name-like-this` for better diagnostics.
    std::vector<Ident> idents;
    while (eat(EXP(Minus))) idents.push_back(parse_ident());
    Span fixed_name_sp = ident.span.to(idents.back().span);
    std::string fixed_name(ident.name);
    for (const Ident& part : idents) fixed_name += "_" + std::string(part.name);

    emit("crate name using dashes are not valid in `extern crate` statements", fixed_name_sp);

    file_.interned.push_back(fixed_name);
    Ident r;
    r.name = file_.interned.back();
    r.span = fixed_name_sp;
    return r;
}

/// Parses `extern` for foreign ABIs modules.
///
/// `extern` is expected to have been consumed before calling this method.
///
/// # Examples
///
/// ```ignore (only-for-syntax-highlight)
/// extern "C" {}
/// extern {}
/// ```
Item* Parser::parse_item_foreign_mod(AttrVec& attrs, Safety safety) {
    Span extern_span = prev_token_uninterpolated_span();
    std::optional<StrLit> abi = parse_abi();  // ABI?
    // FIXME: This recovery should be tested better.
    if (safety.kind == Safety::Default && tok_is_keyword(token, Kw::Unsafe) &&
        look_ahead(1, [](const Token& t) { return t.kind == Tok::OpenBrace; })) {
        try {
            expect(EXP(OpenBrace));
        } catch (PErr& e) {
            emit(e);
        }
        safety.kind = Safety::Unsafe;
        safety.span = token.span;
        (void)eat_keyword(KW(Unsafe));
    }
    Item* item = mk<Item>();
    item->kind = ItemKind::ForeignMod;
    item->extern_span = extern_span;
    item->safety = safety;
    item->has_abi = abi.has_value();
    if (abi) item->abi = *abi;
    item->items = parse_item_list(attrs, ItemContext::Foreign);
    return item;
}

/// Parses a foreign item (one in an `extern { ... }` block).
std::pair<bool, Item*> Parser::parse_foreign_item(ForceCollect force_collect) {
    FnParseMode fn_parse_mode{ReqName::NotDotDotDot, FnContext::Free, false};
    Item* item = parse_item_(fn_parse_mode, force_collect, AllowConstBlockItems::DoesNotMatter);
    if (item == nullptr) return {false, nullptr};
    switch (item->kind) {
    case ItemKind::Static:
    case ItemKind::Fn:
    case ItemKind::TyAlias:
    case ItemKind::MacCall: break;
    case ItemKind::Const:
        emit("extern items cannot be `const`", item->ident.span);
        item->kind = ItemKind::Static;
        item->mutbl = Mutability::Not;
        item->safety = Safety();
        item->defaultness = Defaultness();
        item->generics = Generics();
        item->type_const = false;
        break;
    default: return {true, error_bad_item_kind(item->span, item->kind, "`extern` blocks")};
    }
    item->ctx = ItemContext::Foreign;
    return {true, item};
}

Item* Parser::error_bad_item_kind(Span span, ItemKind kind, const char* ctx) {
    Span sp = guess_head_span(span);
    emit(std::string(item_descr(kind)) + " is not supported in " + ctx, sp);
    return nullptr;
}

bool Parser::is_use_closure() const {
    if (tok_is_keyword(token, Kw::Use)) {
        // Check if this could be a closure.
        size_t dist = look_ahead(1, [this](const Token& t) {
            return tok_is_keyword(t, Kw::Move) || tok_is_keyword(t, Kw::Async);
        }) ? 2
           : 1;
        return look_ahead(dist, [](const Token& t) { return t.kind == Tok::Or || t.kind == Tok::OrOr; });
    }
    return false;
}

bool Parser::is_unsafe_foreign_mod() const {
    // Look for `unsafe`.
    if (!tok_is_keyword(token, Kw::Unsafe)) return false;
    // Look for `extern`.
    if (!is_keyword_ahead(1, {Kw::Extern})) return false;

    // Look for the optional ABI string literal.
    size_t n = look_ahead(2, [this](const Token& t) { return tok_can_begin_string_literal(t); }) ? 3 : 2;

    // Look for the `{`. Use `tree_look_ahead` because the ABI (if present)
    // might be a metavariable i.e. an invisible-delimited sequence, and
    // `tree_look_ahead` will consider that a single element when looking
    // ahead.
    TreeAhead tt;
    return tree_look_ahead(n, tt) && tt.delimited && tt.tok.kind == Tok::OpenBrace;
}

std::optional<Safety> Parser::parse_global_static_front_matter(Case c) {
    bool is_global_static;
    if (check_keyword_case(KW(Static), c)) {
        // Check if this could be a closure.
        is_global_static = !look_ahead(1, [&](const Token& t) {
            if (tok_is_keyword_case(t, Kw::Move, c) || tok_is_keyword_case(t, Kw::Use, c)) return true;
            return t.kind == Tok::Or || t.kind == Tok::OrOr;
        });
    } else {
        // `$qual static`
        is_global_static = (check_keyword_case(KW(Unsafe), c) || check_keyword_case(KW(Safe), c)) &&
                           look_ahead(1, [&](const Token& t) { return tok_is_keyword_case(t, Kw::Static, c); });
    }

    if (is_global_static) {
        Safety safety = parse_safety(c);
        (void)eat_keyword_case(KW(Static), c);
        return safety;
    }
    return std::nullopt;
}

/// Recover on `const mut` with `const` already eaten.
void Parser::recover_const_mut(Span const_span) {
    if (eat_keyword(KW(Mut))) {
        Span span = prev_token.span;
        emit("const globals cannot be mutable", span);
    } else if (eat_keyword(KW(Let))) {
        Span span = prev_token.span;
        emit("`const` and `let` are mutually exclusive", const_span.to(span));
    }
}

Item* Parser::parse_const_block_item() {
    expect_keyword(KW(Const));
    Block* block = parse_block();
    Item* item = mk<Item>();
    item->kind = ItemKind::ConstBlock;
    item->body = block;
    return item;
}

/// Parse a static item with the prefix `"static" "mut"?` already parsed and stored in
/// `mutability`.
///
/// ```ebnf
/// Static = "static" "mut"? $ident ":" $ty (= $expr)? ";" ;
/// ```
Item* Parser::parse_static_item(Safety safety, Mutability mutability) {
    Ident ident = parse_ident();

    if (token.kind == Tok::Lt && may_recover()) {
        Generics generics = parse_generics();
        emit("static items may not have generic parameters", generics.span);
    }

    // Parse the type of a static item. That is, the `":" $ty` fragment.
    // FIXME: This could maybe benefit from `.may_recover()`?
    bool colon = eat(EXP(Colon));
    bool eq = check(EXP(Eq));
    bool semi = check(EXP(Semi));
    Ty* ty;
    if (colon && !(eq || semi))
        ty = parse_ty();
    else
        // If there wasn't a `:` or the colon was followed by a `=` or `;`, recover a missing type.
        ty = recover_missing_global_item_type(colon, mutability == Mutability::Mut ? "static mut" : "static");

    Expr* expr = nullptr;
    if (eat(EXP(Eq))) expr = parse_expr();

    expect_semi();

    Item* item = mk<Item>();
    item->kind = ItemKind::Static;
    item->ident = ident;
    item->ty = ty;
    item->safety = safety;
    item->mutbl = mutability;
    item->expr = expr;
    return item;
}

/// Parse a constant item with the prefix `"const"` already parsed.
///
/// If `const_arg` is true, any expression assigned to the const will be parsed
/// as a const_arg instead of a body expression.
///
/// ```ebnf
/// Const = "const" ($ident | "_") Generics ":" $ty (= $expr)? WhereClause ";" ;
/// ```
Parser::ConstParts Parser::parse_const_item(bool const_arg, Span const_span) {
    ConstParts cp;
    cp.ident = parse_ident_or_underscore();

    cp.generics = parse_generics();

    // Parse the type of a constant item. That is, the `":" $ty` fragment.
    // FIXME: This could maybe benefit from `.may_recover()`?
    bool colon = eat(EXP(Colon));
    bool eq = check(EXP(Eq));
    bool semi = check(EXP(Semi));
    bool where_ = check_keyword(KW(Where));
    if (colon && !(eq || semi || where_))
        cp.ty = parse_ty();
    else
        cp.ty = recover_missing_global_item_type(colon, "const");

    WhereClause before_where_clause;
    if (may_recover()) before_where_clause = parse_where_clause();

    bool has_rhs = false;
    if (eat(EXP(Eq))) {
        has_rhs = true;
        if (const_arg)
            cp.rhs = parse_expr_anon_const(true).value;
        else
            cp.rhs = parse_expr();
    }
    cp.type_const = const_arg;

    WhereClause after_where_clause = parse_where_clause();

    // Provide a nice error message if the user placed a where-clause before the item body.
    // Users may be tempted to write such code if they are still used to the deprecated
    // where-clause location on type aliases and associated types. See also #89122.
    if (before_where_clause.has_where_token && has_rhs && cp.rhs != nullptr)
        emit("where clauses are not allowed before const item bodies", before_where_clause.span);

    // Merge the predicates of both where-clauses since either one can be relevant.
    // If we didn't parse a body (which is valid for associated consts in traits) and we were
    // allowed to recover, `before_where_clause` contains the predicates, otherwise they are
    // in `after_where_clause`. Further, both of them might contain predicates iff two
    // where-clauses were provided which is syntactically ill-formed but we want to recover from
    // it and treat them as one large where-clause.
    WhereClause where_clause;
    where_clause.has_where_token = before_where_clause.has_where_token || after_where_clause.has_where_token;
    where_clause.predicates = std::move(before_where_clause.predicates);
    for (WherePredicate* p : after_where_clause.predicates) where_clause.predicates.push_back(p);
    where_clause.span = after_where_clause.has_where_token ? after_where_clause.span : before_where_clause.span;
    cp.generics.where_clause = std::move(where_clause);

    // try_recover_const_missing_semi
    if (token.kind != Tok::Semi && has_rhs && !const_arg && cp.rhs != nullptr && st_.in_fn_body && may_recover()) {
        std::optional<Span> sp = missing_semi_from_binop("const", cp.rhs, OptSpan(const_span.shrink_to_lo()));
        if (sp) {
            st_.fn_body_missing_semi_guar = true;
            cp.rhs = mk_expr_err(*sp);
            cp.type_const = false;
            return cp;
        }
    }

    expect_semi();
    return cp;
}

/// We were supposed to parse `":" $ty` but the `:` or the type was missing.
/// This means that the type is missing.
Ty* Parser::recover_missing_global_item_type(bool colon_present, const char* kind) {
    // Construct the error and stash it away with the hope
    // that typeck will later enrich the error with a type.
    (void)colon_present;
    Span span = prev_token.span.shrink_to_hi();
    late_.push_back(PErr{std::string("missing type for `") + kind + "` item", span});

    // The user intended that the type be inferred,
    // so treat this as if the user wrote e.g. `const A: _ = expr;`.
    return mk_ty(span, TyKind::Infer);
}

/// Parses an enum declaration.
Item* Parser::parse_item_enum() {
    if (tok_is_keyword(token, Kw::Struct)) {
        Span span = prev_token.span.to(token.span);
        if (look_ahead(1, [this](const Token& t) { return tok_is_ident(t); })) {
            bump();
            emit("`enum` and `struct` are mutually exclusive", span);
        } else {
            fail("`enum` and `struct` are mutually exclusive", span);
        }
    }

    Ident ident = parse_ident();
    Generics generics = parse_generics();
    generics.where_clause = parse_where_clause();

    // Possibly recover `enum Foo;` instead of `enum Foo {}`
    std::vector<Variant*> variants;
    if (token.kind == Tok::Semi) {
        emit("expected `{}`, found `;`", token.span);
        bump();
    } else {
        try {
            variants = parse_delim_comma_seq<Variant*>(EXP(OpenBrace), EXP(CloseBrace),
                                                       [&] { return parse_enum_variant(ident.span); });
        } catch (PErr&) {
            if (tok_is_non_reserved_ident(prev_token) && token.kind == Tok::Colon) {
                ParserState snapshot = create_snapshot_for_diagnostic();
                bump();
                try {
                    (void)parse_ty();
                } catch (PErr&) {
                }
                restore_snapshot(snapshot);
            }
            eat_to_tokens({EXP(CloseBrace)});
            bump();  // }
            throw;
        }
    }

    Item* item = mk<Item>();
    item->kind = ItemKind::Enum;
    item->ident = ident;
    item->generics = std::move(generics);
    for (Variant* v : variants)
        if (v != nullptr) item->variants.push_back(v);
    return item;
}

Variant* Parser::parse_enum_variant(Span span) {
    (void)span;
    recover_vcs_conflict_marker();
    AttrWrapper variant_attrs = parse_outer_attributes();
    recover_vcs_conflict_marker();
    Span vlo = token.span;

    Visibility vis = parse_visibility(FollowedByType::No);
    if (!recover_nested_adt_item(Kw::Enum)) return nullptr;
    Ident ident = parse_field_ident("enum", vlo);

    if (token.kind == Tok::Bang) {
        try {
            unexpected();
        } catch (PErr& err) {
            emit(err);
        }
        bump();
        (void)parse_delim_args();
        return nullptr;
    }

    VariantData struct_def;
    if (check(EXP(OpenBrace))) {
        // Parse a struct variant.
        try {
            std::pair<std::vector<FieldDef*>, bool> r = parse_record_struct_body("struct", ident.span, false);
            struct_def.fields = std::move(r.first);
            struct_def.recovered = r.second;
        } catch (PErr& err) {
            if (token.kind == Tok::Colon) throw;
            eat_to_tokens({EXP(CloseBrace)});
            bump();  // }
            emit(err);
            struct_def.fields.clear();
            struct_def.recovered = true;
        }
        struct_def.kind = VariantData::Struct;
    } else if (check(EXP(OpenParen))) {
        try {
            struct_def.fields = parse_tuple_struct_body();
        } catch (PErr& err) {
            if (token.kind == Tok::Colon) throw;
            eat_to_tokens({EXP(CloseParen)});
            bump();  // )
            emit(err);
            struct_def.fields.clear();
        }
        struct_def.kind = VariantData::Tuple;
    } else {
        struct_def.kind = VariantData::Unit;
    }

    AnonConst* disr_expr = nullptr;
    if (eat(EXP(Eq))) {
        disr_expr = mk<AnonConst>();
        *disr_expr = parse_expr_anon_const(false);
    }

    Variant* vr = mk<Variant>();
    vr->ident = ident;
    vr->vis = vis;
    vr->attrs = std::move(variant_attrs.attrs);
    vr->data = std::move(struct_def);
    vr->disr_expr = disr_expr;
    vr->span = vlo.to(prev_token.span);
    return vr;
}

/// Parses `struct Foo { ... }`.
Item* Parser::parse_item_struct() {
    Ident ident = parse_ident();

    Generics generics = parse_generics();

    // There is a `where` clause.
    VariantData vdata;
    if (tok_is_keyword(token, Kw::Where)) {
        std::pair<WhereClause, std::optional<std::vector<FieldDef*>>> wc =
            parse_struct_where_clause(ident, generics.span);
        generics.where_clause = std::move(wc.first);

        if (wc.second) {
            // If we see a misplaced tuple struct body: `struct Foo<T> where T: Copy, (T);`
            vdata.kind = VariantData::Tuple;
            vdata.fields = std::move(*wc.second);
            expect_semi();
        } else if (eat(EXP(Semi))) {
            // If we see a: `struct Foo<T> where T: Copy;` style decl.
            vdata.kind = VariantData::Unit;
        } else {
            // If we see: `struct Foo<T> where T: Copy { ... }`
            std::pair<std::vector<FieldDef*>, bool> r =
                parse_record_struct_body("struct", ident.span, generics.where_clause.has_where_token);
            vdata.kind = VariantData::Struct;
            vdata.fields = std::move(r.first);
            vdata.recovered = r.second;
        }
    } else if (eat(EXP(Semi))) {
        // No `where` so: `struct Foo<T>;`
        vdata.kind = VariantData::Unit;
    } else if (token.kind == Tok::OpenBrace) {
        // Record-style struct definition
        std::pair<std::vector<FieldDef*>, bool> r =
            parse_record_struct_body("struct", ident.span, generics.where_clause.has_where_token);
        vdata.kind = VariantData::Struct;
        vdata.fields = std::move(r.first);
        vdata.recovered = r.second;
    } else if (token.kind == Tok::OpenParen) {
        // Tuple-style struct definition with optional where-clause.
        vdata.kind = VariantData::Tuple;
        vdata.fields = parse_tuple_struct_body();
        generics.where_clause = parse_where_clause();
        expect_semi();
    } else {
        fail("expected `where`, `{`, `(`, or `;` after struct name, found " + token_descr(token), token.span);
    }

    Item* item = mk<Item>();
    item->kind = ItemKind::Struct;
    item->ident = ident;
    item->generics = std::move(generics);
    item->data = std::move(vdata);
    return item;
}

/// Parses `union Foo { ... }`.
Item* Parser::parse_item_union() {
    Ident ident = parse_ident();

    Generics generics = parse_generics();

    VariantData vdata;
    vdata.kind = VariantData::Struct;
    if (tok_is_keyword(token, Kw::Where)) {
        generics.where_clause = parse_where_clause();
        std::pair<std::vector<FieldDef*>, bool> r =
            parse_record_struct_body("union", ident.span, generics.where_clause.has_where_token);
        vdata.fields = std::move(r.first);
        vdata.recovered = r.second;
    } else if (token.kind == Tok::OpenBrace) {
        std::pair<std::vector<FieldDef*>, bool> r =
            parse_record_struct_body("union", ident.span, generics.where_clause.has_where_token);
        vdata.fields = std::move(r.first);
        vdata.recovered = r.second;
    } else {
        fail("expected `where` or `{` after union name, found " + token_descr(token), token.span);
    }

    Item* item = mk<Item>();
    item->kind = ItemKind::Union;
    item->ident = ident;
    item->generics = std::move(generics);
    item->data = std::move(vdata);
    return item;
}

/// This function parses the fields of record structs:
///
///   - `struct S { ... }`
///   - `enum E { Variant { ... } }`
std::pair<std::vector<FieldDef*>, bool> Parser::parse_record_struct_body(const char* adt_ty, Span ident_span,
                                                                         bool parsed_where) {
    std::vector<FieldDef*> fields;
    bool recovered = false;
    if (eat(EXP(OpenBrace))) {
        while (token.kind != Tok::CloseBrace) {
            try {
                fields.push_back(parse_field_def(adt_ty, ident_span));
            } catch (PErr& err) {
                consume_block(EXP(OpenBrace), EXP(CloseBrace), false);
                emit(err);
                recovered = true;
                break;
            }
        }
        expect(EXP(CloseBrace));
    } else {
        std::string where_str = parsed_where ? "" : "`where`, or ";
        fail("expected " + where_str + "`{` after struct name, found " + token_descr(token), token.span);
    }
    return {std::move(fields), recovered};
}

Safety Parser::parse_unsafe_field() {
    Safety s;
    if (eat_keyword(KW(Unsafe))) {
        s.kind = Safety::Unsafe;
        s.span = prev_token.span;
    }
    return s;
}

std::vector<FieldDef*> Parser::parse_tuple_struct_body() {
    // This is the case where we find `struct Foo<T>(T) where T: Copy;`
    // Unit like structs are handled in parse_item_struct function
    return parse_paren_comma_seq<FieldDef*>([this] {
        AttrWrapper attrs = parse_outer_attributes();
        bool have_snapshot = false;
        ParserState snapshot;
        if (is_vcs_conflict_marker(Tok::Shl, Tok::Lt)) {
            // Account for `<<<<<<<` diff markers. We can't proactively error here because
            // that can be a valid type start, so we snapshot and reparse only we've
            // encountered another parse error.
            snapshot = create_snapshot_for_diagnostic();
            have_snapshot = true;
        }
        Span lo = token.span;
        Visibility vis;
        Ty* ty = nullptr;
        try {
            vis = parse_visibility(FollowedByType::Yes);
            ty = parse_ty();
        } catch (PErr&) {
            if (have_snapshot) recover_vcs_conflict_marker_in(snapshot);
            throw;
        }
        AnonConst* default_ = nullptr;
        if (token.kind == Tok::Eq) {
            ParserState before = create_snapshot_for_diagnostic();
            bump();
            try {
                AnonConst c = parse_expr_anon_const(false);
                default_ = mk<AnonConst>();
                *default_ = c;
            } catch (PErr&) {
                restore_snapshot(before);
            }
        }
        FieldDef* f = mk<FieldDef>();
        f->span = lo.to(ty->span);
        f->vis = vis;
        f->ty = ty;
        f->default_ = default_;
        f->attrs = std::move(attrs.attrs);
        return f;
    });
}

/// Parses an element of a struct declaration.
FieldDef* Parser::parse_field_def(const char* adt_ty, Span ident_span) {
    recover_vcs_conflict_marker();
    AttrWrapper attrs = parse_outer_attributes();
    recover_vcs_conflict_marker();
    Span lo = token.span;
    Visibility vis = parse_visibility(FollowedByType::No);
    Safety safety = parse_unsafe_field();
    return parse_single_struct_field(adt_ty, lo, vis, safety, std::move(attrs.attrs), ident_span);
}

/// Parses a structure field declaration.
FieldDef* Parser::parse_single_struct_field(const char* adt_ty, Span lo, Visibility vis, Safety safety, AttrVec attrs,
                                            Span ident_span) {
    (void)ident_span;
    FieldDef* a_var = parse_name_and_ty(adt_ty, lo, vis, safety, std::move(attrs));
    switch (token.kind) {
    case Tok::Comma: bump(); break;
    case Tok::Semi: {
        bump();
        Span sp = prev_token.span;
        emit(std::string(adt_ty) + " fields are separated by `,`", sp);
        break;
    }
    case Tok::CloseBrace: break;
    case Tok::DocComment: {
        Span err_span = token.span;
        bump();  // consume the doc comment
        if (eat(EXP(Comma)) || token.kind == Tok::CloseBrace) {
            emit("found a documentation comment that doesn't document anything", err_span);
        } else {
            fail("found a documentation comment that doesn't document anything", err_span);
        }
        break;
    }
    default: {
        Span sp = prev_token.span.shrink_to_hi();
        std::string msg = "expected `,`, or `}`, found " + token_descr(token);

        // Try to recover extra trailing angle brackets
        if (a_var->ty->kind == TyKind::Path && a_var->ty->path != nullptr && !a_var->ty->path->segments.empty()) {
            const PathSegment& last_segment = a_var->ty->path->segments.back();
            if (check_trailing_angle_brackets(last_segment, {EXP(Comma), EXP(CloseBrace)})) {
                // Handle a case like `Vec<u8>>,` where we can continue parsing fields
                // after the comma
                (void)eat(EXP(Comma));

                // `check_trailing_angle_brackets` already emitted a nicer error, as
                // proven by the presence of `_guar`. We can continue parsing.
                return a_var;
            }
        }

        if (tok_is_ident(token) ||
            (token.kind == Tok::Pound && look_ahead(1, [](const Token& t) { return t.kind == Tok::OpenBracket; }))) {
            // This is likely another field, TokenKind::Pound is used for `#[..]`
            // attribute for next field. Emit the diagnostic and continue parsing.
            emit(msg, sp);
        } else {
            fail(msg, sp);
        }
        break;
    }
    }
    return a_var;
}

void Parser::expect_field_ty_separator() {
    try {
        expect(EXP(Colon));
    } catch (PErr& err) {
        bool eq_typo =
            token.kind == Tok::Eq && look_ahead(1, [this](const Token& t) { return tok_is_path_start(t); });
        // Try to recover from typos like `x; usize` and `x = usize`.
        Span semi_hi = token.span;
        bool semi_typo = token.kind == Tok::Semi && look_ahead(1, [&](const Token& t) {
                             return tok_is_path_start(t) &&
                                    // We check that the type is defined on the same line as the field.
                                    !(t.span.lo > semi_hi.hi && is_multiline(Span(semi_hi.hi, t.span.lo)));
                         });
        if (eq_typo || semi_typo) {
            bump();
            // Gracefully handle small typos.
            emit(err);
        } else {
            throw;
        }
    }
}

/// Parses a structure field.
FieldDef* Parser::parse_name_and_ty(const char* adt_ty, Span lo, Visibility vis, Safety safety, AttrVec attrs) {
    Ident name = parse_field_ident(adt_ty, lo);
    if (token.kind == Tok::Bang) unexpected();  // "macros cannot expand to ... fields" is a note
    expect_field_ty_separator();
    Ty* ty = parse_ty();
    if (token.kind == Tok::Colon && look_ahead(1, [](const Token& t) { return t.kind != Tok::Colon; })) {
        emit("found single colon in a struct field type path", token.span);
    }
    AnonConst* default_ = nullptr;
    if (token.kind == Tok::Eq) {
        bump();
        default_ = mk<AnonConst>();
        *default_ = parse_expr_anon_const(false);
    }
    FieldDef* f = mk<FieldDef>();
    f->span = lo.to(prev_token.span);
    f->has_ident = true;
    f->ident = name;
    f->vis = vis;
    f->safety = safety;
    f->ty = ty;
    f->default_ = default_;
    f->attrs = std::move(attrs);
    return f;
}

/// Parses a field identifier. Specialized version of `parse_ident_common`
/// for better diagnostics and suggestions.
Ident Parser::parse_field_ident(const char* adt_ty, Span lo) {
    std::pair<Ident, bool> ir = ident_or_err(true);
    Ident ident = ir.first;
    bool is_raw = ir.second;
    if (!is_raw && ident_is_reserved(ident)) {
        ParserState snapshot = create_snapshot_for_diagnostic();
        PErr err;
        if (check_fn_front_matter(false, Case::Sensitive)) {
            Visibility inherited_vis;
            FnParseMode fn_parse_mode{ReqName::Always, FnContext::Free, true};
            // We use `parse_fn` to get a span for the function
            AttrVec dummy_attrs;
            try {
                (void)parse_fn(dummy_attrs, fn_parse_mode, lo, inherited_vis, Case::Insensitive);
                err = make_err(std::string("functions are not allowed in ") + adt_ty + " definitions",
                               lo.to(prev_token.span));
            } catch (PErr&) {
                restore_snapshot(snapshot);
                err = expected_ident_found_err();
            }
        } else if (eat_keyword(KW(Struct))) {
            try {
                Item* item = parse_item_struct();
                err = make_err(std::string("structs are not allowed in ") + adt_ty + " definitions",
                               lo.with_hi(item->ident.span.hi));
            } catch (PErr&) {
                restore_snapshot(snapshot);
                err = expected_ident_found_err();
            }
        } else {
            err = expected_ident_found_err();
            bool recovered = false;
            Ident let_ident;
            if (eat_keyword_noexpect(Kw::Let)) {
                bool ok = false;
                try {
                    let_ident = parse_ident_common(false);
                    ok = true;
                } catch (PErr&) {
                }
                if (ok && token.kind == Tok::Colon) recovered = true;
            }
            if (recovered) {
                emit(err);
                return let_ident;
            }
            restore_snapshot(snapshot);
        }
        throw err;
    }
    bump();
    return ident;
}

/// Parses a declarative macro 2.0 definition.
/// The `macro` keyword has already been parsed.
/// ```ebnf
/// MacBody = "{" TOKEN_STREAM "}" ;
/// MacParams = "(" TOKEN_STREAM ")" ;
/// DeclMac = "macro" Ident MacParams? MacBody ;
/// ```
Item* Parser::parse_item_decl_macro(Span lo) {
    (void)lo;
    Ident ident = parse_ident();
    DelimArgs body;
    if (check(EXP(OpenBrace))) {
        body = parse_delim_args();  // `MacBody`
    } else if (check(EXP(OpenParen))) {
        uint32_t params_lo = cur_index();
        Span pspan = parse_token_tree();  // `MacParams`
        if (!check(EXP(OpenBrace))) unexpected();
        Span bspan = parse_token_tree();  // `MacBody`
        uint32_t body_hi = cur_index();   // the token after the body's `}`
        // Convert `MacParams MacBody` into `{ MacParams => MacBody }`: the
        // token range covers `(..) {..}` without its outermost two delimiters.
        body.delim = DelimKind::Brace;
        body.open = pspan.shrink_to_lo();
        body.close = bspan.shrink_to_hi();
        body.tok_lo = params_lo + 1;
        body.tok_hi = body_hi > 0 ? body_hi - 1 : body_hi;
    } else {
        unexpected_any();
    }

    Item* item = mk<Item>();
    item->kind = ItemKind::MacroDef;
    item->ident = ident;
    item->macro_body = body;
    item->macro_rules = false;
    return item;
}

/// Is this a possibly malformed start of a `macro_rules! foo` item definition?
/// 0 = No, 1 = Yes with `!`, 2 = Yes without `!`.
int Parser::is_macro_rules_item() {
    if (check_keyword(KW(MacroRules))) {
        Span macro_rules_span = token.span;

        if (look_ahead(1, [](const Token& t) { return t.kind == Tok::Bang; }) &&
            look_ahead(2, [this](const Token& t) { return tok_is_ident(t); })) {
            return 1;
        }
        if (look_ahead(1, [this](const Token& t) { return tok_is_ident(t); })) {
            // macro_rules foo
            emit("expected `!` after `macro_rules`", macro_rules_span);
            return 2;
        }
    }
    return 0;
}

/// Parses a `macro_rules! foo { ... }` declarative macro.
Item* Parser::parse_item_macro_rules(const Visibility& vis, bool has_bang) {
    expect_keyword(KW(MacroRules));  // `macro_rules`

    if (has_bang) expect(EXP(Bang));  // `!`
    Ident ident = parse_ident();

    if (eat(EXP(Bang))) {
        // Handle macro_rules! foo!
        Span span = prev_token.span;
        emit("macro names aren't followed by a `!`", span);
    }

    DelimArgs body = parse_delim_args();
    eat_semi_for_macro_if_needed(body);
    complain_if_pub_macro(vis, true);

    Item* item = mk<Item>();
    item->kind = ItemKind::MacroDef;
    item->ident = ident;
    item->macro_body = body;
    item->macro_rules = true;
    return item;
}

/// Item macro invocations or `macro_rules!` definitions need inherited visibility.
/// If that's not the case, emit an error.
void Parser::complain_if_pub_macro(const Visibility& vis, bool macro_rules) {
    if (vis.kind == Visibility::Inherited) return;

    std::string vstr = vis_to_string(vis);
    if (macro_rules) {
        emit("can't qualify macro_rules invocation with `" + vstr + "`", vis.span);
    } else {
        emit("can't qualify macro invocation with `pub`", vis.span);
    }
}

void Parser::eat_semi_for_macro_if_needed(const DelimArgs& args) {
    // DelimArgs::need_semicolon
    if (args.delim != DelimKind::Brace && !eat(EXP(Semi))) {
        // report_invalid_macro_expansion_item
        emit("macros that expand to items must be delimited with braces or followed by a semicolon", args.span());
    }
}

/// Checks if current token is one of tokens which cannot be nested like `kw::Enum`. In case
/// it is, we try to parse the item and report error about nested types.
bool Parser::recover_nested_adt_item(Kw keyword) {
    if ((tok_is_keyword(token, Kw::Enum) || tok_is_keyword(token, Kw::Struct) || tok_is_keyword(token, Kw::Union)) &&
        look_ahead(1, [this](const Token& t) { return tok_is_ident(t); })) {
        Token kw_token = token;
        std::string kw_text = token_str(kw_token);
        Item* item = parse_item(ForceCollect::No, AllowConstBlockItems::DoesNotMatter);
        if (item != nullptr) {
            Span item_span = item->span;
            if (token.kind == Tok::Comma) item_span = item_span.to(token.span);
            (void)item_span;
        }
        emit("`" + kw_text + "` definition cannot be nested inside `" + std::string(kw_str(keyword)) + "`", kw_token.span);
        // We successfully parsed the item but we must inform the caller about nested problem.
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Functions.

/// Parse a function starting from the front matter (`const ...`) to the body `{ ... }` or `;`.
Parser::FnParts Parser::parse_fn(AttrVec& attrs, FnParseMode fn_parse_mode, Span sig_lo, const Visibility& vis, Case c) {
    Span fn_span = token.span;
    FnHeader header = parse_fn_front_matter(vis, c, FrontMatterParsingMode::Function);  // `const ... fn`
    Ident ident = parse_ident();                                                         // `foo`
    Generics generics = parse_generics();                                                // `<'a, T, ...>`
    FnDecl* decl = nullptr;
    try {
        decl = parse_fn_decl(fn_parse_mode, AllowPlus::Yes, RecoverReturnSign::Yes);
    } catch (PErr&) {
        // If we see `for Ty ...` then user probably meant `impl` item.
        if (tok_is_keyword(token, Kw::For)) fail("you might have meant to write `impl` instead of `fn`", fn_span);
        throw;
    }

    // Store the end of function parameters to give better diagnostics
    // inside `parse_fn_body()`.
    Span fn_params_end_sp = prev_token.span.shrink_to_hi();

    FnContract* contract = parse_contract();

    generics.where_clause = parse_where_clause();  // `where T: Ord`

    // `fn_params_end` is needed only when it's followed by a where clause.
    OptSpan fn_params_end;
    if (generics.where_clause.has_where_token) fn_params_end = OptSpan(fn_params_end_sp);

    Span sig_hi = prev_token.span;

    // Either `;` or `{ ... }`.
    Block* body = parse_fn_body(attrs, ident, sig_hi, fn_parse_mode.req_body, fn_params_end);
    FnParts f;
    f.ident = ident;
    f.sig.header = header;
    f.sig.decl = decl;
    f.sig.span = sig_lo.to(sig_hi);
    f.generics = std::move(generics);
    f.contract = contract;
    f.body = body;
    return f;
}

/// Provide diagnostics when function body is not found
void Parser::error_fn_body_not_found(Span ident_span, bool req_body, OptSpan fn_params_end) {
    (void)ident_span;
    try {
        if (req_body)
            expected_one_of_not_found({}, {EXP(OpenBrace)});
        else
            expected_one_of_not_found({}, {EXP(Semi), EXP(OpenBrace)});
        return;
    } catch (PErr& err) {
        if (token.kind == Tok::CloseBrace) {
            // The enclosing `mod`, `trait` or `impl` is being closed, so keep the `fn` in
            // the AST for typechecking.
            emit(err);
            return;
        }
        if (token.kind == Tok::RArrow && fn_params_end.has) {
            // Instead of a function body, the parser has encountered a right arrow
            // preceded by a where clause.
            static const char* const kFnTraits[] = {"FnOnce", "FnMut", "Fn"};
            bool fn_trait = false;
            for (const char* name : kFnTraits)
                if (tok_is_ident_named(prev_token, name)) fn_trait = true;
            try {
                (void)parse_ret_ty(AllowPlus::Yes, RecoverQPath::Yes, RecoverReturnSign::Yes);
            } catch (PErr&) {
                throw err;
            }
            PErr e = err;
            if (!fn_trait) e.msg = "return type should be specified after the function parameters";
            throw e;
        }
        throw;
    }
}

/// Parse the "body" of a function.
/// This can either be `;` when there's no body,
/// or e.g. a block when the function is a provided one.
Block* Parser::parse_fn_body(AttrVec& attrs, const Ident& ident, Span& sig_hi, bool req_body, OptSpan fn_params_end) {
    bool has_semi = req_body ? token.kind == Tok::Semi
                             // Only include `;` in list of expected tokens if body is not required
                             : check(EXP(Semi));
    AttrVec inner_attrs;
    Block* body = nullptr;
    if (has_semi) {
        // Include the trailing semicolon in the span of the signature
        expect_semi();
        sig_hi = prev_token.span;
    } else if (check(EXP(OpenBrace))) {
        bool prev_in_fn_body = st_.in_fn_body;
        st_.in_fn_body = true;
        try {
            std::pair<AttrVec, Block*> r = parse_block_common(token.span, BlockCheckMode::Default, OptSpan());
            if (st_.fn_body_missing_semi_guar) {
                st_.fn_body_missing_semi_guar = false;
                Stmt* s = mk_stmt(r.second->span, StmtKind::Expr);
                s->expr = mk_expr_err(r.second->span);
                r.second->stmts.push_back(s);
            }
            inner_attrs = std::move(r.first);
            body = r.second;
        } catch (...) {
            st_.in_fn_body = prev_in_fn_body;
            throw;
        }
        st_.in_fn_body = prev_in_fn_body;
    } else if (token.kind == Tok::Eq) {
        // Recover `fn foo() = $expr;`.
        bump();  // `=`
        Span eq_sp = prev_token.span;
        (void)parse_expr();
        expect_semi();  // `;`
        Span span = eq_sp.to(prev_token.span);
        emit("function body cannot be `= expression;`", span);
        body = mk_block_err(span);
    } else {
        error_fn_body_not_found(ident.span, req_body, fn_params_end);
    }
    for (Attribute& a : inner_attrs) attrs.push_back(a);
    return body;
}

/// Is the current token the start of an `FnHeader` / not a valid parse?
bool Parser::check_impl_frontmatter(size_t look_ahead_n) {
    static const Kw ALL_QUALS[] = {Kw::Const, Kw::Unsafe};
    // In most cases, we only need to check the next token, but `impl` can come after
    // qualifiers.
    if (check_keyword(KW(Impl))) return true;
    size_t i = 0;
    while (i < 2) {
        int action = look_ahead(i + look_ahead_n, [this](const Token& t) {
            if (tok_is_keyword(t, Kw::Impl)) return 1;
            for (Kw q : ALL_QUALS)
                if (tok_is_keyword(t, q)) return 0;
            return -1;
        });
        if (action == 1) return true;
        if (action == -1) return false;
        ++i;
    }
    return is_keyword_ahead(i, {Kw::Impl});
}

/// Is the current token the start of an `FnHeader` / not a valid parse?
///
/// `check_pub` adds additional `pub` to the checks in case users place it
/// wrongly, can be used to ensure `pub` never comes after `default`.
bool Parser::check_fn_front_matter(bool check_pub, Case c) {
    // We use an over-approximation here.
    // `const const`, `fn const` won't parse, but we're not stepping over other syntax either.
    // `pub` is added in case users got confused with the ordering like `async pub fn`,
    // only if it wasn't preceded by `default` as `default pub` is invalid.
    static const ExpKw ALL_QUALS[] = {KW(Pub), KW(Gen), KW(Const), KW(Async), KW(Unsafe), KW(Safe), KW(Extern)};
    const ExpKw* quals = check_pub ? ALL_QUALS : ALL_QUALS + 1;
    size_t nquals = check_pub ? 7 : 6;

    // Rationale for checking for "Definitely an `fn`" first.
    if (check_keyword_case(KW(Fn), c)) return true;  // Definitely an `fn`.

    bool any = false;
    for (size_t i = 0; i < nquals && !any; ++i) any = check_keyword_case(quals[i], c);
    if (any) {
        bool la = look_ahead(1, [&](const Token& t) {
            // `$qual fn` or `$qual $qual`:
            if (tok_is_keyword_case(t, Kw::Fn, c)) return true;
            bool qual_next = false;
            if (t.kind == Tok::Ident && !t.raw) {
                // `$qual $qual` where the second is reserved
                for (size_t i = 0; i < nquals; ++i)
                    if (t.kw == quals[i].kw && kw_is_reserved(t.kw, edition_)) qual_next = true;
                if (!qual_next && c == Case::Insensitive) {
                    std::string lower(t.sym);
                    for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    for (size_t i = 0; i < nquals; ++i)
                        if (lower == kw_str(quals[i].kw)) qual_next = true;
                }
            }
            return qual_next
                   // Rule out 2015 `const async: T = val`.
                   && !is_unsafe_foreign_mod()
                   // Rule out `async gen {` and `async gen move {`
                   && !is_async_gen_block()
                   // Rule out `const unsafe auto` and `const unsafe trait` and `const unsafe impl`.
                   && !is_keyword_ahead(2, {Kw::Auto, Kw::Trait, Kw::Impl});
        });
        if (la) return true;
    }

    // `extern ABI fn`
    if (check_keyword_case(KW(Extern), c) &&
        look_ahead(1, [this](const Token& t) { return tok_can_begin_string_literal(t); })) {
        TreeAhead t2;
        if (tree_look_ahead(2, t2) && !t2.delimited && tok_is_keyword_case(t2.tok, Kw::Fn, c)) return true;
        // This branch is only for better diagnostics; `pub`, `unsafe`, etc. are not
        // allowed here.
        if (may_recover() && tree_look_ahead(2, t2) && !t2.delimited) {
            bool is_qual = false;
            for (const ExpKw& q : ALL_QUALS)
                if (tok_is_keyword(t2.tok, q.kw)) is_qual = true;
            TreeAhead t3;
            if (is_qual && tree_look_ahead(3, t3) && !t3.delimited && tok_is_keyword_case(t3.tok, Kw::Fn, c))
                return true;
        }
    }
    return false;
}

/// Parses all the "front matter" (or "qualifiers") for a `fn` declaration,
/// up to and including the `fn` keyword. The formal grammar is:
///
/// ```text
/// Extern = "extern" StringLit? ;
/// FnQual = "const"? "async"? "unsafe"? Extern? ;
/// FnFrontMatter = FnQual "fn" ;
/// ```
///
/// `vis` represents the visibility that was already parsed, if any. Use
/// `Visibility::Inherited` when no visibility is known.
///
/// If `parsing_mode` is `FrontMatterParsingMode::FunctionPtrType`, we error on `const` and `async` qualifiers,
/// which are not allowed in function pointer types.
FnHeader Parser::parse_fn_front_matter(const Visibility& orig_vis, Case c, FrontMatterParsingMode parsing_mode) {
    (void)orig_vis;  // only steers suggestions
    Span sp_start = token.span;
    Constness constness = parse_constness(c);
    if (parsing_mode == FrontMatterParsingMode::FunctionPtrType && constness.yes) {
        emit("an `fn` pointer type cannot be `const`", constness.span);
    }

    Span async_start_sp = token.span;
    CoroutineKind coroutine_kind = parse_coroutine_kind(c);
    if (parsing_mode == FrontMatterParsingMode::FunctionPtrType && coroutine_kind.kind == CoroutineKind::Async) {
        emit("an `fn` pointer type cannot be `async`", coroutine_kind.span);
    }
    // FIXME(gen_blocks): emit a similar error if the user tries to use `gen fn` or `async gen fn`.

    Span unsafe_start_sp = token.span;
    Safety safety = parse_safety(c);

    Span ext_start_sp = token.span;
    Extern ext = parse_extern(c);

    if (coroutine_kind.kind == CoroutineKind::Async && edition_ == Edition::E2015) {
        emit("`async fn` is not permitted in Rust 2015", coroutine_kind.span);
    }

    FnHeader header;
    header.constness = constness;
    header.coroutine_kind = coroutine_kind;
    header.safety = safety;
    header.ext = ext;

    if (!eat_keyword_case(KW(Fn), c)) {
        // It is possible for `expect_one_of` to recover given the contents of
        // `self.expected_token_types`, therefore, do not use `self.unexpected()` which doesn't
        // account for this.
        bool ok = false;
        PErr err;
        try {
            (void)expect_one_of({}, {});
            ok = true;
        } catch (PErr& e) {
            err = e;
        }
        if (ok) return header;

        // Qualifier keywords ordering check
        enum class WrongKw { None, Duplicated, Misplaced, MisplacedDisallowedQualifier };
        Constness recover_constness = constness;
        CoroutineKind recover_coroutine_kind = coroutine_kind;
        Safety recover_safety = safety;
        WrongKw wrong_kw = WrongKw::None;
        // This will allow the machine fix to directly place the keyword in the correct place or to indicate
        // that the keyword is already present and the second instance should be removed.
        if (check_keyword(KW(Const))) {
            if (constness.yes) {
                wrong_kw = WrongKw::Duplicated;
            } else {
                recover_constness.yes = true;
                recover_constness.span = token.span;
                if (parsing_mode == FrontMatterParsingMode::Function) {
                    wrong_kw = WrongKw::Misplaced;
                    (void)async_start_sp;
                } else {
                    emit("an `fn` pointer type cannot be `const`", token.span);
                    wrong_kw = WrongKw::MisplacedDisallowedQualifier;
                }
            }
        } else if (check_keyword(KW(Async))) {
            if (coroutine_kind.kind == CoroutineKind::Async || coroutine_kind.kind == CoroutineKind::AsyncGen) {
                wrong_kw = WrongKw::Duplicated;
            } else if (coroutine_kind.kind == CoroutineKind::Gen) {
                recover_coroutine_kind.kind = CoroutineKind::AsyncGen;
                recover_coroutine_kind.span = token.span;
                wrong_kw = WrongKw::Misplaced;
                (void)unsafe_start_sp;
            } else {
                recover_coroutine_kind.kind = CoroutineKind::Async;
                recover_coroutine_kind.span = token.span;
                if (parsing_mode == FrontMatterParsingMode::Function) {
                    wrong_kw = WrongKw::Misplaced;
                } else {
                    emit("an `fn` pointer type cannot be `async`", token.span);
                    wrong_kw = WrongKw::MisplacedDisallowedQualifier;
                }
            }
        } else if (check_keyword(KW(Unsafe))) {
            if (safety.kind == Safety::Unsafe) {
                wrong_kw = WrongKw::Duplicated;
            } else {
                recover_safety.kind = Safety::Unsafe;
                recover_safety.span = token.span;
                wrong_kw = WrongKw::Misplaced;
                (void)ext_start_sp;
            }
        } else if (check_keyword(KW(Safe))) {
            if (safety.kind == Safety::Safe) {
                wrong_kw = WrongKw::Duplicated;
            } else {
                recover_safety.kind = Safety::Safe;
                recover_safety.span = token.span;
                wrong_kw = WrongKw::Misplaced;
            }
        }

        if (wrong_kw == WrongKw::None && check_keyword(KW(Pub))) {
            // Recover incorrect visibility order such as `async pub`
            (void)sp_start;
            try {
                (void)parse_visibility(FollowedByType::No);
            } catch (PErr&) {
                throw err;
            }
            // Only suggestions follow.
        }

        // FIXME(gen_blocks): add keyword recovery logic for genness

        if (wrong_kw != WrongKw::None && may_recover() &&
            look_ahead(1, [&](const Token& t) { return tok_is_keyword_case(t, Kw::Fn, c); })) {
            // Advance past the misplaced keyword and `fn`
            bump();
            bump();
            // When we recover from a `MisplacedDisallowedQualifier`, we already emitted an error for the disallowed qualifier
            // So we don't need to emit another error here
            if (wrong_kw != WrongKw::MisplacedDisallowedQualifier) emit(err);
            header.constness = recover_constness;
            header.safety = recover_safety;
            header.coroutine_kind = recover_coroutine_kind;
            return header;
        }

        throw err;
    }

    return header;
}

/// Parses the parameter list and result type of a function declaration.
FnDecl* Parser::parse_fn_decl(const FnParseMode& fn_parse_mode, AllowPlus ret_allow_plus,
                              RecoverReturnSign recover_return_sign) {
    FnDecl* d = mk<FnDecl>();
    d->inputs = parse_fn_params(fn_parse_mode);
    d->output = parse_ret_ty(ret_allow_plus, RecoverQPath::Yes, recover_return_sign);
    return d;
}

/// Parses the parameter list of a function, including the `(` and `)` delimiters.
std::vector<Param*> Parser::parse_fn_params(const FnParseMode& fn_parse_mode) {
    bool first_param = true;
    // Parse the arguments, starting out with `self` being allowed...
    if (token.kind != Tok::OpenParen
        // might be typo'd trait impl, handled elsewhere
        && !tok_is_keyword(token, Kw::For)) {
        // recover from missing argument list, e.g. `fn main -> () {}`
        emit("missing parameters for function definition", prev_token.span.shrink_to_hi());
        return {};
    }

    std::vector<Param*> params = parse_paren_comma_seq<Param*>([&]() -> Param* {
        recover_vcs_conflict_marker();
        ParserState snapshot = create_snapshot_for_diagnostic();
        Param* param;
        try {
            param = parse_param_general(fn_parse_mode, first_param, true);
        } catch (PErr& e) {
            emit(e);
            Span lo = prev_token.kind == Tok::OpenParen ? prev_token.span.shrink_to_hi() : prev_token.span;
            restore_snapshot(snapshot);
            // Skip every token until next possible arg or end.
            eat_to_tokens({EXP(Comma), EXP(CloseParen)});
            // Create a placeholder argument for proper arg count (issue #34264).
            Ident dummy;
            dummy.name = "dummy";
            dummy.span = lo.to(prev_token.span);
            param = dummy_arg(dummy);
        }
        // ...now that we've parsed the first argument, `self` is no longer allowed.
        first_param = false;
        return param;
    });
    // Replace duplicated recovered params with `_` pattern to avoid unnecessary errors.
    deduplicate_recovered_params_names(params);
    return params;
}

/// Parses a single function parameter.
///
/// - `self` is syntactically allowed when `first_param` holds.
/// - `recover_arg_parse` is used to recover from a failed argument parse.
Param* Parser::parse_param_general(const FnParseMode& fn_parse_mode, bool first_param, bool recover_arg_parse_) {
    DepthGuard guard(*this);
    Span lo = token.span;
    AttrWrapper attrs = parse_outer_attributes();

    // Possibly parse `self`. Recover if we parsed it and it wasn't allowed here.
    if (Param* param = parse_self_param()) {
        param->attrs = std::move(attrs.attrs);
        return first_param ? param : recover_bad_self_param(param);
    }

    bool is_dot_dot_dot = token.kind == Tok::DotDotDot;
    bool is_name_required;
    switch (fn_parse_mode.req_name) {
    case ReqName::Always: is_name_required = true; break;
    case ReqName::Never: is_name_required = false; break;
    case ReqName::Edition2018: is_name_required = edition_ >= Edition::E2018; break;
    case ReqName::NotDotDotDot: is_name_required = !is_dot_dot_dot; break;
    default: is_name_required = true; break;
    }
    // VARARGS_WITHOUT_PATTERN is a lint.
    if (is_name_required && is_dot_dot_dot) is_name_required = false;

    Pat* pat;
    Ty* ty;
    if (is_name_required || is_named_param()) {
        std::pair<Pat*, bool> pc = parse_fn_param_pat_colon();
        pat = pc.first;
        if (!pc.second) {
            PErr err;
            try {
                unexpected();
            } catch (PErr& e) {
                err = e;
            }
            Ident ident;
            if (parameter_without_type(pat, is_name_required, first_param, ident)) {
                emit(err);
                return dummy_arg(ident);
            }
            throw err;
        }

        eat_incorrect_doc_comment_for_param_type();
        ty = parse_ty_for_param();
    } else {
        ParserState parser_snapshot_before_ty = create_snapshot_for_diagnostic();
        eat_incorrect_doc_comment_for_param_type();
        Ty* t = nullptr;
        bool ok = false;
        PErr err;
        try {
            t = parse_ty_for_param();
            ok = true;
        } catch (PErr& e) {
            err = e;
        }
        if (ok) {
            if (t->kind == TyKind::Path && t->path != nullptr && !t->path->segments.empty()) {
                const PathSegment& segment = t->path->segments.back();
                if (check_trailing_angle_brackets(segment, {EXP(CloseParen)})) return dummy_arg(segment.ident);
            }
            if (token.kind != Tok::Comma && token.kind != Tok::CloseParen) {
                // This wasn't actually a type, but a pattern looking like a type,
                // so we are going to rollback and re-parse for recovery.
                try {
                    unexpected_any();
                } catch (PErr& e) {
                    err = e;
                    ok = false;
                }
            }
        }
        if (ok) {
            pat = mk_pat(t->span, PatKind::Missing);
            ty = t;
        } else if (token.kind == Tok::DotDotDot) {
            throw err;
        } else if (st_.unmatched_angle_bracket_count > 0) {
            // Recovering here would lead to a parse error on the angle bracket.
            throw err;
        } else if (recover_arg_parse_) {
            // Recover from attempting to parse the argument as a type without pattern.
            restore_snapshot(parser_snapshot_before_ty);
            std::pair<Pat*, Ty*> r = recover_arg_parse();
            pat = r.first;
            ty = r.second;
        } else {
            throw err;
        }
    }

    Span span = lo.to(prev_token.span);
    Param* p = mk<Param>();
    p->attrs = std::move(attrs.attrs);
    p->pat = pat;
    p->span = span;
    p->ty = ty;
    return p;
}

/// Returns the parsed optional self parameter and whether a self shortcut was used.
Param* Parser::parse_self_param() {
    // Extract an identifier *after* having confirmed that the token is one.
    auto expect_self_ident = [this]() {
        Ident ident = tok_ident(token);
        bump();
        return ident;
    };
    // is `self` is `n` tokens ahead?
    auto is_isolated_self = [this](size_t n) {
        return is_keyword_ahead(n, {Kw::SelfLower}) &&
               look_ahead(n + 1, [](const Token& t) { return t.kind != Tok::PathSep; });
    };
    // is `pin const self` `n` tokens ahead?
    auto is_isolated_pin_const_self = [&](size_t n) {
        return look_ahead(n, [this](const Token& t) { return tok_is_ident_named(t, "pin") && !t.raw; }) &&
               is_keyword_ahead(n + 1, {Kw::Const}) && is_isolated_self(n + 2);
    };
    // is `mut self` `n` tokens ahead?
    auto is_isolated_mut_self = [&](size_t n) { return is_keyword_ahead(n, {Kw::Mut}) && is_isolated_self(n + 1); };
    // is `pin mut self` `n` tokens ahead?
    auto is_isolated_pin_mut_self = [&](size_t n) {
        return look_ahead(n, [this](const Token& t) { return tok_is_ident_named(t, "pin") && !t.raw; }) &&
               is_isolated_mut_self(n + 1);
    };

    // SelfKind
    enum class SK { Value, Region, Pinned, Explicit };
    SK sk = SK::Value;
    Mutability m = Mutability::Not;
    bool has_lt = false;
    Lifetime lt;
    Ty* explicit_ty = nullptr;
    Ident eself_ident;
    Span eself_hi;

    // Parse `self` or `self: TYPE`. We already know the current token is `self`.
    auto parse_self_possibly_typed = [&](Mutability mm) {
        eself_ident = expect_self_ident();
        eself_hi = prev_token.span;
        m = mm;
        if (eat(EXP(Colon))) {
            sk = SK::Explicit;
            explicit_ty = parse_ty();
        } else {
            sk = SK::Value;
        }
    };
    auto expect_self_ident_not_typed = [&]() {
        Ident ident = expect_self_ident();
        // Recover `: Type` after a qualified self
        if (may_recover() && eat_noexpect(Tok::Colon)) {
            ParserState snap = create_snapshot_for_diagnostic();
            try {
                Ty* ty = parse_ty();
                emit("type not allowed for shorthand `self` parameter", ty->span);
            } catch (PErr&) {
                restore_snapshot(snap);
            }
        }
        return ident;
    };
    // Recover for the grammar `*self`, `*const self`, and `*mut self`.
    auto recover_self_ptr = [&]() {
        emit("cannot pass `self` by raw pointer", token.span);
        sk = SK::Value;
        m = Mutability::Not;
        eself_ident = expect_self_ident();
        eself_hi = prev_token.span;
    };

    // Parse optional `self` parameter of a method.
    // Only a limited set of initial token sequences is considered `self` parameters; anything
    // else is parsed as a normal function parameter list, so some lookahead is required.
    Span eself_lo = token.span;
    if (token.kind == Tok::And) {
        has_lt = look_ahead(1, [this](const Token& t) { return tok_is_lifetime(t); });
        size_t skip_lifetime_count = has_lt ? 1 : 0;
        if (is_isolated_self(skip_lifetime_count + 1)) {
            // `&self`
            // `&'lt self`
            bump();  // &
            if (has_lt) lt = expect_lifetime();
            sk = SK::Region;
            m = Mutability::Not;
        } else if (is_isolated_mut_self(skip_lifetime_count + 1)) {
            // `&mut self`
            // `&'lt mut self`
            bump();  // &
            if (has_lt) lt = expect_lifetime();
            bump();  // mut
            sk = SK::Region;
            m = Mutability::Mut;
        } else if (is_isolated_pin_const_self(skip_lifetime_count + 1)) {
            // `&pin const self`
            bump();  // &
            if (has_lt) lt = expect_lifetime();
            bump();  // pin
            bump();  // const
            sk = SK::Pinned;
            m = Mutability::Not;
        } else if (is_isolated_pin_mut_self(skip_lifetime_count + 1)) {
            // `&pin mut self`
            bump();  // &
            if (has_lt) lt = expect_lifetime();
            bump();  // pin
            bump();  // mut
            sk = SK::Pinned;
            m = Mutability::Mut;
        } else {
            // `&not_self`
            return nullptr;
        }
        Span hi = token.span;
        eself_ident = expect_self_ident_not_typed();
        eself_hi = hi;
    } else if (token.kind == Tok::Star && is_isolated_self(1)) {
        // `*self`
        bump();
        recover_self_ptr();
    } else if (token.kind == Tok::Star && look_ahead(1, [this](const Token& t) { return tok_is_mutability(t); }) &&
               is_isolated_self(2)) {
        // `*mut self` and `*const self`
        bump();
        bump();
        recover_self_ptr();
    } else if (token.kind == Tok::Ident && is_isolated_self(0)) {
        // `self` and `self: TYPE`
        parse_self_possibly_typed(Mutability::Not);
    } else if (token.kind == Tok::Ident && is_isolated_mut_self(0)) {
        // `mut self` and `mut self: TYPE`
        bump();
        parse_self_possibly_typed(Mutability::Mut);
    } else {
        return nullptr;
    }

    // Param::from_self
    Span eself_span = eself_lo.to(eself_hi);
    Span span = eself_span.to(eself_ident.span);
    Ty* infer_ty = mk_ty(eself_ident.span, TyKind::ImplicitSelf);
    Mutability pat_mutbl = m;
    Ty* ty = nullptr;
    switch (sk) {
    case SK::Explicit: ty = explicit_ty; break;
    case SK::Value: ty = infer_ty; break;
    case SK::Region:
        pat_mutbl = Mutability::Not;
        ty = mk_ty(span, TyKind::Ref);
        ty->inner = infer_ty;
        ty->mutbl = m;
        ty->has_lifetime = has_lt;
        ty->lifetime = lt;
        break;
    case SK::Pinned:
        ty = mk_ty(span, TyKind::PinnedRef);
        ty->inner = infer_ty;
        ty->mutbl = m;
        ty->has_lifetime = has_lt;
        ty->lifetime = lt;
        break;
    }
    Pat* pat = mk_pat(span, PatKind::Ident);
    pat->mutbl = pat_mutbl;
    pat->ident = eself_ident;
    Param* p = mk<Param>();
    p->pat = pat;
    p->span = span;
    p->ty = ty;
    return p;
}

bool Parser::is_named_param() const {
    size_t offset = 0;
    if (token.kind == Tok::And || token.kind == Tok::AndAnd)
        offset = 1;
    else if (tok_is_keyword(token, Kw::Mut))
        offset = 1;
    return look_ahead(offset, [this](const Token& t) { return tok_is_ident(t); }) &&
           look_ahead(offset + 1, [](const Token& t) { return t.kind == Tok::Colon; });
}

bool Parser::recover_self_param() {
    try {
        (void)parse_outer_attributes();
        return parse_self_param() != nullptr;
    } catch (PErr&) {
        return false;
    }
}

}  // namespace detail
}  // namespace rust
