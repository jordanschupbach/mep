// rust_format_check.cpp -- the Rust formatter's self-check: the output must
// lex and parse, and its tokens must be the input's, up to exactly the token
// changes rustfmt makes. Both texts are reduced to a canonical token list
// derived from their own parse trees, independently of the formatter:
//
//  - punctuation is split into single characters (`||` == `| |`, `>>` == `> >`);
//  - a `,` right before a closing `)` `]` `}` `>` `|` is dropped, except the
//    `,` of a one-element tuple `(x,)` (a paren group whose opener does not
//    follow a name / `)` / `]` / `>` / `!`), which is significant;
//  - the trailing `,` of a where clause, and the `;` closing a macro_rules!
//    body's last rule;
//  - match arms: the `,` after an arm, and the braces of an arm body that is a
//    plain block holding one tail expression (rustfmt adds and removes both);
//  - closures without a return type: the braces of such a body;
//  - `;` of empty statements and of `while`/`loop`/`for`/`return`/`break`/
//    `continue` statements (rustfmt drops the first, adds the second);
//  - `((e))` -> `(e)` (remove_nested_parens);
//  - `extern` without an ABI string gets `"C"` (force_explicit_abi);
//  - `pub(in crate|self|super)` -> `pub(crate|self|super)`;
//  - `#[derive(A, B)]` == `#[derive(A)] #[derive(B)]` (merge_derives);
//  - a run of consecutive `use` items (also `extern crate`, `mod x;`) is
//    compared as the sorted multiset of its flattened import paths, each with
//    its attributes and visibility (reorder_imports / reorder_modules and
//    rustfmt's use-tree normalisation);
//  - comments (doc comments included) are compared as a multiset of their
//    texts with whitespace runs collapsed.
// Macro arguments in `(..)` / `[..]` that parse as an expression list are
// canonicalised like code, since rustfmt formats them as code.
// Ported from rustfmt (rust-lang/rust 1.97.1 src/tools/rustfmt, MIT OR Apache-2.0)
// in the sense that the allowed changes are rustfmt's; the check itself is mep's.
#include <algorithm>
#include <map>
#include <set>

#include "rust_format_internal.h"

namespace rustfmt {

namespace {

using rust::Tok;
using rust::Token;

struct Canon {
    const rust::ParseResult& pr;
    std::vector<bool> drop;                              // per token
    std::map<uint32_t, std::vector<std::string>> insert_after;  // token index -> extra tokens
    std::map<uint32_t, std::pair<uint32_t, std::vector<std::string>>> replace;  // [lo, hi) -> tokens
    uint32_t depth = 0;

    explicit Canon(const rust::ParseResult& p) : pr(p), drop(p.tokens.size(), false) {}

    std::string_view src() const { return *pr.source; }

    /// Index of the first token whose span starts at or after `pos`.
    uint32_t tok_at(uint32_t pos) const {
        auto it = std::lower_bound(pr.tokens.begin(), pr.tokens.end(), pos,
                                   [](const Token& t, uint32_t p) { return t.span.lo < p; });
        return static_cast<uint32_t>(it - pr.tokens.begin());
    }
    /// Index of the last token ending at or before `pos` (UINT32_MAX if none).
    uint32_t tok_ending_at(uint32_t pos) const {
        uint32_t i = tok_at(pos);
        while (i > 0) {
            i--;
            if (pr.tokens[i].span.hi <= pos && !pr.tokens[i].synthetic) return i;
        }
        return UINT32_MAX;
    }
    bool is(uint32_t i, Tok k) const { return i < pr.tokens.size() && pr.tokens[i].kind == k; }

    void drop_braces_of(const rust::Block* b) {
        uint32_t o = tok_at(b->span.lo);
        uint32_t c = tok_ending_at(b->span.hi);
        if (is(o, Tok::OpenBrace) && c != UINT32_MAX && is(c, Tok::CloseBrace)) {
            drop[o] = true;
            drop[c] = true;
        }
    }

    // A plain block holding exactly one tail expression (no semicolon).
    static bool single_expr_block(const rust::Expr* e) {
        if (!e || e->kind != rust::ExprKind::Block || e->has_label || !e->attrs.empty()) return false;
        const rust::Block* b = e->block;
        return b && b->rules == rust::BlockCheckMode::Default && b->stmts.size() == 1 &&
               b->stmts[0]->kind == rust::StmtKind::Expr;
    }

    // ------------------------------------------------------------------ walk

    void attrs(const rust::AttrVec&) {}

    void item(const rust::Item* it) {
        if (!it || ++depth > 400) return;
        where(it->generics.where_clause);
        where(it->after_where_clause);
        for (rust::GenericParam* gp : it->generics.params) generic_param(gp);
        vis(it->vis);
        if (it->ty) ty(it->ty);
        if (it->expr) expr(it->expr);
        if (it->sig.decl) fn_decl(it->sig.decl);
        if (it->body) block(it->body);
        item_list(it->items);
        for (const rust::Variant* v : it->variants) {
            if (v->disr_expr) expr(v->disr_expr->value);
            for (const rust::FieldDef* f : v->data.fields) field(f);
        }
        for (const rust::FieldDef* f : it->data.fields) field(f);
        for (const rust::GenericBound& b : it->bounds) bound(b);
        if (it->mac) mac(it->mac);
        if (it->kind == rust::ItemKind::MacroDef && it->macro_rules) {
            // rustfmt ends the last rule with `;`.
            uint32_t c = it->macro_body.tok_hi;
            if (c > it->macro_body.tok_lo && is(c - 1, Tok::Semi)) drop[c - 1] = true;
        }
        depth--;
    }

    void vis(const rust::Visibility& v) {
        if (v.kind != rust::Visibility::Restricted || v.shorthand || !v.path) return;
        // `pub(in crate)` -> `pub(crate)`
        if (v.path->segments.size() == 1) {
            std::string_view n = v.path->segments[0].ident.name;
            if (n == "crate" || n == "self" || n == "super") {
                uint32_t i = tok_at(v.span.lo);
                for (; i < pr.tokens.size() && pr.tokens[i].span.lo < v.span.hi; i++) {
                    if (pr.tokens[i].kind == Tok::Ident && pr.tokens[i].kw == rust::Kw::In) {
                        drop[i] = true;
                        break;
                    }
                }
            }
        }
    }

    void field(const rust::FieldDef* f) {
        if (f->ty) ty(f->ty);
        if (f->default_) expr(f->default_->value);
    }

    void where(const rust::WhereClause& w) {
        if (!w.has_where_token || w.predicates.empty()) return;
        uint32_t last = tok_ending_at(w.span.hi);
        if (last != UINT32_MAX && is(last, Tok::Comma) && pr.tokens[last].span.lo >= w.span.lo) drop[last] = true;
        for (const rust::WherePredicate* p : w.predicates) {
            if (p->bounded_ty) ty(p->bounded_ty);
            if (p->lhs_ty) ty(p->lhs_ty);
            if (p->rhs_ty) ty(p->rhs_ty);
            for (const rust::GenericBound& b : p->bounds) bound(b);
        }
    }

    void generic_param(const rust::GenericParam* gp) {
        for (const rust::GenericBound& b : gp->bounds) bound(b);
        if (gp->default_ty) ty(gp->default_ty);
        if (gp->const_ty) ty(gp->const_ty);
        if (gp->const_default) expr(gp->const_default->value);
    }

    void bound(const rust::GenericBound& b) {
        if (b.kind == rust::GenericBound::Trait && b.trait.trait_ref) path(b.trait.trait_ref);
    }

    void path(const rust::Path* p) {
        if (!p) return;
        for (const rust::PathSegment& s : p->segments) {
            if (!s.args) continue;
            for (const rust::AngleBracketedArg& a : s.args->args) {
                if (a.is_constraint) {
                    if (a.constraint && a.constraint->term_ty) ty(a.constraint->term_ty);
                    if (a.constraint && a.constraint->term_const.value) expr(a.constraint->term_const.value);
                } else if (a.arg.kind == rust::GenericArg::Type) {
                    ty(a.arg.ty);
                } else if (a.arg.kind == rust::GenericArg::Const) {
                    expr(a.arg.konst.value);
                }
            }
            for (const rust::Ty* t : s.args->inputs) ty(t);
            if (s.args->output.ty) ty(s.args->output.ty);
        }
    }

    void fn_decl(const rust::FnDecl* d) {
        for (const rust::Param* p : d->inputs) {
            if (p->ty) ty(p->ty);
            if (p->pat) pat(p->pat);
        }
        if (d->output.ty) ty(d->output.ty);
    }

    void ty(const rust::Ty* t) {
        if (!t || ++depth > 400) return;
        if (t->inner) ty(t->inner);
        if (t->len.value) expr(t->len.value);
        for (const rust::Ty* e : t->elems) ty(e);
        if (t->path) path(t->path);
        if (t->qself && t->qself->ty) ty(t->qself->ty);
        for (const rust::GenericBound& b : t->bounds) bound(b);
        if (t->fn_ptr && t->fn_ptr->decl) fn_decl(t->fn_ptr->decl);
        if (t->mac) mac(t->mac);
        depth--;
    }

    void pat(const rust::Pat* p) {
        if (!p || ++depth > 400) return;
        if (p->sub) pat(p->sub);
        for (const rust::Pat* e : p->elems) pat(e);
        for (const rust::PatField& f : p->fields) pat(f.pat);
        if (p->expr) expr(p->expr);
        if (p->range_lo) expr(p->range_lo);
        if (p->range_hi) expr(p->range_hi);
        if (p->path) path(p->path);
        if (p->mac) mac(p->mac);
        if (p->guard && p->guard->cond) expr(p->guard->cond);
        depth--;
    }

    void mac(const rust::MacCall* m) {
        if (!m || m->args.delim == rust::DelimKind::Brace) return;
        if (m->args.tok_hi <= m->args.tok_lo) return;
        rust::ParseResult& mpr = const_cast<rust::ParseResult&>(pr);
        rust::Fragment f = rust::parse_fragment(mpr, m->args.tok_lo, m->args.tok_hi, rust::FragmentKind::ExprList);
        if (!f.ok) return;
        for (const rust::Expr* e : f.exprs) expr(e);
    }

    void block(const rust::Block* b) {
        if (!b || ++depth > 400) return;
        stmts(b->stmts);
        depth--;
    }

    void stmts(const std::vector<rust::Stmt*>& ss) {
        std::vector<const rust::Item*> run;
        auto flush = [&]() {
            item_runs(run);
            run.clear();
        };
        for (const rust::Stmt* s : ss) {
            if (s->kind == rust::StmtKind::Item) {
                run.push_back(s->item);
            } else {
                flush();
            }
            switch (s->kind) {
            case rust::StmtKind::Let:
                if (s->local->pat) pat(s->local->pat);
                if (s->local->ty) ty(s->local->ty);
                if (s->local->init) expr(s->local->init);
                if (s->local->els) block(s->local->els);
                break;
            case rust::StmtKind::Item: item(s->item); break;
            case rust::StmtKind::Expr: expr(s->expr); break;
            case rust::StmtKind::Semi: {
                expr(s->expr);
                switch (s->expr->kind) {
                case rust::ExprKind::While:
                case rust::ExprKind::Loop:
                case rust::ExprKind::ForLoop:
                case rust::ExprKind::Ret:
                case rust::ExprKind::Break:
                case rust::ExprKind::Continue: {
                    uint32_t c = tok_ending_at(s->span.hi);
                    if (c != UINT32_MAX && is(c, Tok::Semi)) drop[c] = true;
                    break;
                }
                default: break;
                }
                break;
            }
            case rust::StmtKind::Empty: {
                uint32_t c = tok_at(s->span.lo);
                if (is(c, Tok::Semi)) drop[c] = true;
                break;
            }
            case rust::StmtKind::MacCall: mac(s->mac->mac); break;
            }
        }
        flush();
    }

    void item_list(const std::vector<rust::Item*>& items) {
        std::vector<const rust::Item*> run;
        for (const rust::Item* it : items) run.push_back(it);
        item_runs(run);
        for (const rust::Item* it : items) item(it);
    }

    // ------------------------------------------------------------ use runs

    enum class RunKind { None, Use, ExternCrate, ModDecl };
    static RunKind run_kind(const rust::Item* it) {
        if (it->kind == rust::ItemKind::Use) return RunKind::Use;
        if (it->kind == rust::ItemKind::ExternCrate) return RunKind::ExternCrate;
        if (it->kind == rust::ItemKind::Mod && !it->mod_inline) return RunKind::ModDecl;
        return RunKind::None;
    }

    std::string tokens_text(uint32_t lo_pos, uint32_t hi_pos) {
        std::string out;
        for (uint32_t i = tok_at(lo_pos); i < pr.tokens.size() && pr.tokens[i].span.hi <= hi_pos; i++) {
            if (pr.tokens[i].kind == Tok::Eof) break;
            out += token_text(i);
            out += ' ';
        }
        return out;
    }

    std::string token_text(uint32_t i) const {
        const Token& t = pr.tokens[i];
        if (t.kind == Tok::DocComment) return "doc";
        return std::string(src().substr(t.span.lo, t.span.hi - t.span.lo));
    }

    void flatten_use(const rust::UseTree* t, std::string prefix, std::vector<std::string>& out) {
        if (!t) return;
        std::string p = prefix;
        if (t->prefix) {
            for (const rust::PathSegment& s : t->prefix->segments) {
                if (!p.empty() && p.back() != ':') p += "::";
                std::string_view n = s.ident.name;
                p += (s.ident.raw ? "r#" : "");
                p += (n == "{{root}}" ? "" : n);
                if (n == "{{root}}") p += "::";
            }
        }
        switch (t->kind) {
        case rust::UseTree::Simple: {
            std::string e = p;
            // `a::{self}` == `a`
            if (ends_with(e, "::self")) e.resize(e.size() - 6);
            if (t->has_rename) e += " as " + std::string(t->rename.name);
            out.push_back(e);
            break;
        }
        case rust::UseTree::Glob: out.push_back(p + (p.empty() || p.back() == ':' ? "*" : "::*")); break;
        case rust::UseTree::Nested:
            if (t->items.empty()) out.push_back(p + "::{}");
            for (const rust::UseTree* c : t->items) flatten_use(c, p, out);
            break;
        }
    }

    void item_runs(const std::vector<const rust::Item*>& items) {
        size_t i = 0;
        while (i < items.size()) {
            RunKind k = run_kind(items[i]);
            if (k == RunKind::None) {
                i++;
                continue;
            }
            size_t j = i;
            while (j < items.size() && run_kind(items[j]) == k) j++;
            std::vector<std::string> entries;
            for (size_t m = i; m < j; m++) {
                const rust::Item* it = items[m];
                std::string head;
                for (const rust::Attribute& a : it->attrs)
                    head += a.is_doc_comment ? "doc " : tokens_text(a.span.lo, a.span.hi);
                head += "|" + tokens_text(it->vis.span.lo, it->vis.span.hi) + "|";
                if (k == RunKind::Use) {
                    std::vector<std::string> leaves;
                    flatten_use(it->use_tree, "", leaves);
                    for (const std::string& l : leaves) entries.push_back(head + l);
                } else {
                    entries.push_back(head + tokens_text(it->span.lo, it->span.hi));
                }
            }
            std::sort(entries.begin(), entries.end());
            uint32_t lo = items[i]->span_with_attributes().lo;
            uint32_t hi = items[j - 1]->span.hi;
            uint32_t tlo = tok_at(lo);
            uint32_t thi = tok_at(hi);
            replace[tlo] = {thi, entries};
            i = j;
        }
    }

    // ----------------------------------------------------------- expressions

    void expr(const rust::Expr* e) {
        if (!e || ++depth > 400) return;
        using K = rust::ExprKind;
        if (e->lhs) expr(e->lhs);
        if (e->rhs) expr(e->rhs);
        for (const rust::Expr* x : e->exprs) expr(x);
        if (e->ty) ty(e->ty);
        if (e->pat) pat(e->pat);
        if (e->block) block(e->block);
        if (e->path) path(e->path);
        if (e->qself && e->qself->ty) ty(e->qself->ty);
        for (const rust::ExprField& f : e->struct_fields) expr(f.expr);
        if (e->anon_const.value) expr(e->anon_const.value);
        if (e->mac) mac(e->mac);
        if (e->seg.args) {
            rust::Path tmp;
            tmp.segments.push_back(e->seg);
            path(&tmp);
        }
        switch (e->kind) {
        case K::Match:
            for (const rust::Arm& a : e->arms) {
                if (a.pat) {
                    // match_arm_leading_pipes = Never: a leading `|` goes.
                    uint32_t v = tok_ending_at(a.pat->span.lo);
                    if (v != UINT32_MAX && is(v, Tok::Or) && pr.tokens[v].span.hi <= a.pat->span.lo) {
                        uint32_t before = v;
                        bool leading = before == 0 || is(before - 1, Tok::OpenBrace) || is(before - 1, Tok::Comma) ||
                                       is(before - 1, Tok::CloseBrace) || is(before - 1, Tok::CloseBracket);
                        if (leading) drop[v] = true;
                    }
                    uint32_t f = tok_at(a.pat->span.lo);  // the span may start at the `|`
                    if (is(f, Tok::Or)) drop[f] = true;
                    pat(a.pat);
                }
                if (a.guard && a.guard->cond) expr(a.guard->cond);
                if (a.body) {
                    expr(a.body);
                    if (single_expr_block(a.body)) drop_braces_of(a.body->block);
                    if (a.has_comma) {
                        uint32_t c = tok_at(a.body->span.hi);
                        if (is(c, Tok::Comma)) drop[c] = true;
                    }
                } else if (a.has_comma) {
                    uint32_t c = tok_at(a.pat->span.hi);
                    if (is(c, Tok::Comma)) drop[c] = true;
                }
            }
            break;
        case K::Closure:
            if (e->closure) {
                if (e->closure->fn_decl) fn_decl(e->closure->fn_decl);
                expr(e->closure->body);
                if (e->closure->fn_decl && e->closure->fn_decl->output.is_default() &&
                    single_expr_block(e->closure->body))
                    drop_braces_of(e->closure->body->block);
            }
            break;
        case K::Paren:
            if (e->lhs && e->lhs->kind == K::Paren && e->attrs.empty()) {
                uint32_t o = tok_at(e->span.lo);
                uint32_t c = tok_ending_at(e->span.hi);
                if (is(o, Tok::OpenParen) && c != UINT32_MAX && is(c, Tok::CloseParen)) {
                    drop[o] = true;
                    drop[c] = true;
                }
            }
            break;
        default: break;
        }
        depth--;
    }

    // -------------------------------------------------------- token-level rules

    void token_rules() {
        const auto& toks = pr.tokens;
        for (uint32_t i = 0; i + 1 < toks.size(); i++) {
            if (toks[i].kind != Tok::Comma) continue;
            uint32_t n = i + 1;
            Tok nk = toks[n].kind;
            if (nk == Tok::CloseBracket || nk == Tok::CloseBrace || nk == Tok::Gt || nk == Tok::Shr ||
                nk == Tok::Ge || nk == Tok::ShrEq || nk == Tok::Or) {
                drop[i] = true;
            } else if (nk == Tok::CloseParen) {
                // Keep the comma of a one-element tuple.
                uint32_t open = pr.partner[n];
                bool other_comma = false;
                for (uint32_t k = open + 1; k < i; k++) {
                    if (toks[k].kind == Tok::Comma) {
                        other_comma = true;
                        break;
                    }
                    if (pr.partner[k] != UINT32_MAX && pr.partner[k] > k) k = pr.partner[k];
                }
                bool call_like = false;
                if (open > 0 && open != UINT32_MAX) {
                    const Token& p = toks[open - 1];
                    call_like = (p.kind == Tok::Ident && (p.kw == rust::Kw::None || p.kw == rust::Kw::SelfUpper ||
                                                         p.kw == rust::Kw::SelfLower)) ||
                                p.kind == Tok::CloseParen || p.kind == Tok::CloseBracket || p.kind == Tok::Gt ||
                                p.kind == Tok::Shr ||
                                p.kind == Tok::Bang;
                }
                if (other_comma || call_like) drop[i] = true;
            }
        }
        // extern without an ABI string -> extern "C"
        for (uint32_t i = 0; i + 1 < toks.size(); i++) {
            if (toks[i].kind == Tok::Ident && toks[i].kw == rust::Kw::Extern && !toks[i].raw) {
                const Token& n = toks[i + 1];
                if (n.kind == Tok::Literal) continue;
                if (n.kind == Tok::Ident && n.kw == rust::Kw::Crate) continue;
                insert_after[i].push_back("\"C\"");
            }
        }
    }

    // Derives: `#[derive(A, B)]` -> `#[derive(A)] #[derive(B)]`.
    void derive_rules() {
        const auto& toks = pr.tokens;
        for (uint32_t i = 0; i + 5 < toks.size(); i++) {
            if (toks[i].kind != Tok::Pound || toks[i + 1].kind != Tok::OpenBracket) continue;
            if (toks[i + 2].kind != Tok::Ident || toks[i + 2].sym != "derive" || toks[i + 2].raw) continue;
            if (toks[i + 3].kind != Tok::OpenParen) continue;
            uint32_t close = pr.partner[i + 3];
            if (close == UINT32_MAX || close + 1 >= toks.size() || toks[close + 1].kind != Tok::CloseBracket) continue;
            if (pr.partner[i + 1] != close + 1) continue;
            std::vector<std::string> out;
            std::string cur;
            auto flush = [&]() {
                if (!cur.empty()) {
                    out.push_back("#");
                    out.push_back("[");
                    out.push_back("derive");
                    out.push_back("(");
                    out.push_back(cur);
                    out.push_back(")");
                    out.push_back("]");
                }
                cur.clear();
            };
            for (uint32_t k = i + 4; k < close; k++) {
                if (toks[k].kind == Tok::Comma) {
                    flush();
                } else {
                    cur += token_text(k);
                }
            }
            flush();
            if (!replace.count(i)) replace[i] = {close + 2, out};
            i = close + 1;
        }
    }

    static void push_split(std::vector<std::string>& out, const std::string& s, bool punct) {
        if (!punct) {
            out.push_back(s);
            return;
        }
        for (char c : s) out.emplace_back(1, c);
    }

    std::vector<std::pair<std::string, uint32_t>> canonical() {
        std::vector<std::pair<std::string, uint32_t>> out;
        const auto& toks = pr.tokens;
        for (uint32_t i = 0; i < toks.size();) {
            const Token& t = toks[i];
            if (t.kind == Tok::Eof) break;
            auto r = replace.find(i);
            if (r != replace.end() && r->second.first > i) {
                for (const std::string& s : r->second.second) out.push_back({s, t.span.lo});
                i = r->second.first;
                continue;
            }
            if (!drop[i] && !t.synthetic) {
                if (t.kind == Tok::DocComment) {
                    out.push_back({"<doc>", t.span.lo});
                } else if (t.kind == Tok::Literal && t.lit == rust::LitKind::Float && t.suffix.empty() && i > 0 &&
                           toks[i - 1].kind == Tok::Dot) {
                    // `x.0.1` lexes `0.1` as one float; rustfmt may print `x.0 .1`.
                    std::string_view f = src().substr(t.span.lo, t.span.hi - t.span.lo);
                    size_t dot = f.find('.');
                    out.push_back({std::string(f.substr(0, dot)), t.span.lo});
                    if (dot != std::string_view::npos) {
                        out.push_back({".", t.span.lo});
                        if (dot + 1 < f.size()) out.push_back({std::string(f.substr(dot + 1)), t.span.lo});
                    }
                } else {
                    std::vector<std::string> parts;
                    push_split(parts, token_text(i), rust::tok_is_punct(t.kind));
                    for (std::string& s : parts) out.push_back({std::move(s), t.span.lo});
                }
            }
            auto ins = insert_after.find(i);
            if (ins != insert_after.end())
                for (const std::string& s : ins->second) out.push_back({s, t.span.hi});
            i++;
        }
        return out;
    }
};

std::string CollapseWs(std::string_view s) {
    std::string out;
    bool ws = false;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
            ws = true;
            continue;
        }
        if (ws && !out.empty()) out += ' ';
        ws = false;
        out += c;
    }
    return out;
}

std::vector<std::pair<std::string, uint32_t>> CanonicalTokens(const rust::ParseResult& pr) {
    Canon c(pr);
    c.token_rules();
    c.derive_rules();
    for (const rust::Item* it : pr.crate->items) c.item(it);
    {
        std::vector<const rust::Item*> run(pr.crate->items.begin(), pr.crate->items.end());
        c.item_runs(run);
    }
    return c.canonical();
}

}  // namespace

std::optional<std::pair<std::string, uint32_t>> CheckTokens(const rust::ParseResult& in, std::string_view out_text,
                                                            rust::Edition edition) {
    rust::ParseOptions po;
    po.edition = edition;
    std::unique_ptr<rust::ParseResult> out = rust::parse_file(std::string(out_text), po);
    if (!out) return std::make_pair(std::string("the output could not be parsed"), 0u);
    if (!out->ok()) {
        const auto& d = out->diagnostics[0];
        return std::make_pair("the output does not parse (line " + std::to_string(d.line) + " of the output: " +
                                  d.message + ")",
                              0u);
    }
    // Comments: a multiset of whitespace-collapsed texts.
    std::multiset<std::string> cin, cout;
    for (const rust::Comment& c : in.comments) cin.insert(CollapseWs(in.text(c.span)));
    for (const rust::Comment& c : out->comments) cout.insert(CollapseWs(out->text(c.span)));
    if (cin != cout) {
        for (const rust::Comment& c : in.comments) {
            std::string t = CollapseWs(in.text(c.span));
            auto it = cout.find(t);
            if (it == cout.end()) return std::make_pair("a comment was lost or changed", c.span.lo);
            cout.erase(it);
        }
        return std::make_pair(std::string("a comment was added"), 0u);
    }
    auto a = CanonicalTokens(in);
    auto b = CanonicalTokens(*out);
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; i++) {
        if (a[i].first != b[i].first)
            return std::make_pair("token `" + a[i].first + "` became `" + b[i].first + "`", a[i].second);
    }
    if (a.size() != b.size()) {
        if (a.size() > b.size()) return std::make_pair("token `" + a[n].first + "` was lost", a[n].second);
        return std::make_pair("token `" + b[n].first + "` was added", a.empty() ? 0u : a.back().second);
    }
    return std::nullopt;
}

}  // namespace rustfmt
