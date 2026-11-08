// rust_ast.cpp -- helpers for the Rust syntax tree and an S-expression dump
// (for tests and debugging).
//
// Ported from rustc_parse/rustc_lexer (rust-lang/rust 1.97.1, MIT OR
// Apache-2.0); node shapes follow rustc_ast/src/ast.rs.

#include "rust_ast.h"

#include <string>

#include "rust_parse.h"

namespace rust {

const char* binop_str(BinOpKind k) {
    switch (k) {
    case BinOpKind::Add: return "+";
    case BinOpKind::Sub: return "-";
    case BinOpKind::Mul: return "*";
    case BinOpKind::Div: return "/";
    case BinOpKind::Rem: return "%";
    case BinOpKind::And: return "&&";
    case BinOpKind::Or: return "||";
    case BinOpKind::BitXor: return "^";
    case BinOpKind::BitAnd: return "&";
    case BinOpKind::BitOr: return "|";
    case BinOpKind::Shl: return "<<";
    case BinOpKind::Shr: return ">>";
    case BinOpKind::Eq: return "==";
    case BinOpKind::Lt: return "<";
    case BinOpKind::Le: return "<=";
    case BinOpKind::Ne: return "!=";
    case BinOpKind::Ge: return ">=";
    case BinOpKind::Gt: return ">";
    }
    return "?";
}

const char* assignop_str(AssignOpKind k) {
    switch (k) {
    case AssignOpKind::AddAssign: return "+=";
    case AssignOpKind::SubAssign: return "-=";
    case AssignOpKind::MulAssign: return "*=";
    case AssignOpKind::DivAssign: return "/=";
    case AssignOpKind::RemAssign: return "%=";
    case AssignOpKind::BitXorAssign: return "^=";
    case AssignOpKind::BitAndAssign: return "&=";
    case AssignOpKind::BitOrAssign: return "|=";
    case AssignOpKind::ShlAssign: return "<<=";
    case AssignOpKind::ShrAssign: return ">>=";
    }
    return "?";
}

const char* unop_str(UnOp k) {
    switch (k) {
    case UnOp::Deref: return "*";
    case UnOp::Not: return "!";
    case UnOp::Neg: return "-";
    }
    return "?";
}

const char* expr_kind_name(ExprKind k) {
    switch (k) {
    case ExprKind::Array: return "Array";
    case ExprKind::ConstBlock: return "ConstBlock";
    case ExprKind::Call: return "Call";
    case ExprKind::MethodCall: return "MethodCall";
    case ExprKind::Tup: return "Tup";
    case ExprKind::Binary: return "Binary";
    case ExprKind::Unary: return "Unary";
    case ExprKind::Move: return "Move";
    case ExprKind::Lit: return "Lit";
    case ExprKind::Cast: return "Cast";
    case ExprKind::Type: return "Type";
    case ExprKind::Let: return "Let";
    case ExprKind::If: return "If";
    case ExprKind::While: return "While";
    case ExprKind::ForLoop: return "ForLoop";
    case ExprKind::Loop: return "Loop";
    case ExprKind::Match: return "Match";
    case ExprKind::Closure: return "Closure";
    case ExprKind::Block: return "Block";
    case ExprKind::Gen: return "Gen";
    case ExprKind::Await: return "Await";
    case ExprKind::Use: return "Use";
    case ExprKind::TryBlock: return "TryBlock";
    case ExprKind::Assign: return "Assign";
    case ExprKind::AssignOp: return "AssignOp";
    case ExprKind::Field: return "Field";
    case ExprKind::Index: return "Index";
    case ExprKind::Range: return "Range";
    case ExprKind::Underscore: return "Underscore";
    case ExprKind::Path: return "Path";
    case ExprKind::AddrOf: return "AddrOf";
    case ExprKind::Break: return "Break";
    case ExprKind::Continue: return "Continue";
    case ExprKind::Ret: return "Ret";
    case ExprKind::InlineAsm: return "InlineAsm";
    case ExprKind::OffsetOf: return "OffsetOf";
    case ExprKind::MacCall: return "MacCall";
    case ExprKind::Struct: return "Struct";
    case ExprKind::Repeat: return "Repeat";
    case ExprKind::Paren: return "Paren";
    case ExprKind::Try: return "Try";
    case ExprKind::Yield: return "Yield";
    case ExprKind::Yeet: return "Yeet";
    case ExprKind::Become: return "Become";
    case ExprKind::IncludedBytes: return "IncludedBytes";
    case ExprKind::FormatArgs: return "FormatArgs";
    case ExprKind::UnsafeBinderCast: return "UnsafeBinderCast";
    case ExprKind::Err: return "Err";
    case ExprKind::Dummy: return "Dummy";
    }
    return "?";
}

const char* pat_kind_name(PatKind k) {
    switch (k) {
    case PatKind::Missing: return "Missing";
    case PatKind::Wild: return "Wild";
    case PatKind::Ident: return "Ident";
    case PatKind::Struct: return "Struct";
    case PatKind::TupleStruct: return "TupleStruct";
    case PatKind::Or: return "Or";
    case PatKind::Path: return "Path";
    case PatKind::Tuple: return "Tuple";
    case PatKind::Box: return "Box";
    case PatKind::Deref: return "Deref";
    case PatKind::Ref: return "Ref";
    case PatKind::Expr: return "Expr";
    case PatKind::Range: return "Range";
    case PatKind::Slice: return "Slice";
    case PatKind::Rest: return "Rest";
    case PatKind::Never: return "Never";
    case PatKind::Guard: return "Guard";
    case PatKind::Paren: return "Paren";
    case PatKind::MacCall: return "MacCall";
    case PatKind::Err: return "Err";
    }
    return "?";
}

const char* ty_kind_name(TyKind k) {
    switch (k) {
    case TyKind::Slice: return "Slice";
    case TyKind::Array: return "Array";
    case TyKind::Ptr: return "Ptr";
    case TyKind::Ref: return "Ref";
    case TyKind::PinnedRef: return "PinnedRef";
    case TyKind::FnPtr: return "FnPtr";
    case TyKind::UnsafeBinder: return "UnsafeBinder";
    case TyKind::Never: return "Never";
    case TyKind::Tup: return "Tup";
    case TyKind::Path: return "Path";
    case TyKind::TraitObject: return "TraitObject";
    case TyKind::ImplTrait: return "ImplTrait";
    case TyKind::Paren: return "Paren";
    case TyKind::Typeof: return "Typeof";
    case TyKind::Infer: return "Infer";
    case TyKind::ImplicitSelf: return "ImplicitSelf";
    case TyKind::MacCall: return "MacCall";
    case TyKind::CVarArgs: return "CVarArgs";
    case TyKind::Pat: return "Pat";
    case TyKind::FieldOf: return "FieldOf";
    case TyKind::Err: return "Err";
    case TyKind::Dummy: return "Dummy";
    }
    return "?";
}

const char* item_kind_name(ItemKind k) {
    switch (k) {
    case ItemKind::ExternCrate: return "ExternCrate";
    case ItemKind::Use: return "Use";
    case ItemKind::Static: return "Static";
    case ItemKind::Const: return "Const";
    case ItemKind::ConstBlock: return "ConstBlock";
    case ItemKind::Fn: return "Fn";
    case ItemKind::Mod: return "Mod";
    case ItemKind::ForeignMod: return "ForeignMod";
    case ItemKind::GlobalAsm: return "GlobalAsm";
    case ItemKind::TyAlias: return "TyAlias";
    case ItemKind::Enum: return "Enum";
    case ItemKind::Struct: return "Struct";
    case ItemKind::Union: return "Union";
    case ItemKind::Trait: return "Trait";
    case ItemKind::TraitAlias: return "TraitAlias";
    case ItemKind::Impl: return "Impl";
    case ItemKind::MacCall: return "MacCall";
    case ItemKind::MacroDef: return "MacroDef";
    case ItemKind::Delegation: return "Delegation";
    case ItemKind::DelegationMac: return "DelegationMac";
    }
    return "?";
}

Span GenericParam::span() const {
    Span s = ident.span;
    for (const GenericBound& b : bounds) s = s.to(b.span());
    if (default_ty) s = s.to(default_ty->span);
    if (kind == Const) {
        s = const_span;
        if (const_default && const_default->value) s = s.to(const_default->value->span);
    }
    return s;
}

bool Param::is_self() const {
    return pat && pat->kind == PatKind::Ident && pat->ident.name == "self" && !pat->ident.raw;
}

bool FnDecl::c_variadic() const {
    return !inputs.empty() && inputs.back()->ty && inputs.back()->ty->kind == TyKind::CVarArgs;
}

// ---------------------------------------------------------------------------
// S-expression dump.

namespace {

class Dumper {
public:
    explicit Dumper(const ParseResult& r) : r_(r) {}
    std::string out;

    void sp(Span s) { out += "@" + std::to_string(s.lo) + ".." + std::to_string(s.hi); }
    void open(const char* tag, Span s) {
        out += '(';
        out += tag;
        out += ' ';
        sp(s);
    }
    void close() { out += ')'; }
    void word(std::string_view w) {
        out += ' ';
        out += w;
    }
    void quoted(std::string_view w) {
        out += " \"";
        for (char c : w) {
            if (c == '"' || c == '\\') out += '\\';
            if (c == '\n') {
                out += "\\n";
                continue;
            }
            out += c;
        }
        out += '"';
    }
    void ident(const Ident& id) {
        out += ' ';
        if (id.raw) out += "r#";
        out += id.name.empty() ? std::string_view("<none>") : id.name;
    }

    void attrs(const AttrVec& as) {
        for (const Attribute& a : as) {
            open(a.is_doc_comment ? "doc" : "attr", a.span);
            if (a.style == AttrStyle::Inner) word("inner");
            if (a.is_doc_comment) {
                quoted(a.doc);
            } else {
                if (a.unsafety.kind == Safety::Unsafe) word("unsafe");
                if (a.path) path(a.path);
                if (a.args.kind == AttrArgs::Delimited) delim(a.args.delim);
                if (a.args.kind == AttrArgs::Eq && a.args.expr) {
                    word("=");
                    expr(a.args.expr);
                }
            }
            close();
        }
    }

    void delim(const DelimArgs& d) {
        open("tokens", d.span());
        out += " " + std::to_string(d.tok_hi - d.tok_lo);
        close();
    }

    void mac(const MacCall* m, Span s) {
        if (!m) return;
        open("mac", s);
        path(m->path);
        delim(m->args);
        close();
    }

    void path(const Path* p) {
        if (!p) return;
        open("path", p->span);
        for (const PathSegment& seg : p->segments) {
            ident(seg.ident);
            if (seg.args) generic_args(seg.args);
        }
        close();
    }

    void qpath(const QSelf* q, const Path* p) {
        if (q) {
            open("qself", q->path_span);
            out += " " + std::to_string(q->position);
            ty(q->ty);
            close();
        }
        path(p);
    }

    void anon(const AnonConst& c) {
        if (c.value) expr(c.value);
    }

    void generic_args(const GenericArgs* a) {
        if (a->kind == GenericArgs::AngleBracketed) {
            open("args", a->span);
            for (const AngleBracketedArg& arg : a->args) {
                if (arg.is_constraint && arg.constraint) {
                    const AssocItemConstraint* c = arg.constraint;
                    open("constraint", c->span);
                    ident(c->ident);
                    if (c->gen_args) generic_args(c->gen_args);
                    if (c->is_equality) {
                        if (c->term_ty)
                            ty(c->term_ty);
                        else
                            anon(c->term_const);
                    } else if (c->bounds) {
                        bounds(*c->bounds);
                    }
                    close();
                } else if (arg.arg.kind == GenericArg::Lifetime) {
                    ident(arg.arg.lifetime.ident);
                } else if (arg.arg.kind == GenericArg::Type) {
                    ty(arg.arg.ty);
                } else {
                    out += " (const";
                    anon(arg.arg.konst);
                    out += ")";
                }
            }
            close();
        } else {
            open(a->kind == GenericArgs::Parenthesized ? "paren-args" : "paren-args-elided", a->span);
            for (const Ty* t : a->inputs) ty(t);
            if (a->output.ty) {
                word("->");
                ty(a->output.ty);
            }
            close();
        }
    }

    void bounds(const GenericBounds& bs) {
        for (const GenericBound& b : bs) {
            if (b.kind == GenericBound::Trait) {
                open("bound", b.trait.span);
                const TraitBoundModifiers& m = b.trait.modifiers;
                if (m.constness == BoundConstnessKind::Always) word("const");
                if (m.constness == BoundConstnessKind::Maybe) word("[const]");
                if (m.async_) word("async");
                if (m.polarity == BoundPolarityKind::Maybe) word("?");
                if (m.polarity == BoundPolarityKind::Negative) word("!");
                if (b.trait.parens) word("parens");
                generic_params(b.trait.bound_generic_params);
                path(b.trait.trait_ref);
                close();
            } else if (b.kind == GenericBound::Outlives) {
                ident(b.lifetime.ident);
            } else {
                open("use", b.use_span);
                for (const PreciseCapturingArg& a : b.use_args) {
                    if (a.is_lifetime)
                        ident(a.lifetime.ident);
                    else
                        path(a.path);
                }
                close();
            }
        }
    }

    void generic_params(const std::vector<GenericParam*>& ps) {
        for (const GenericParam* p : ps) {
            const char* tag = p->kind == GenericParam::Lifetime ? "lifetime-param"
                              : p->kind == GenericParam::Type   ? "type-param"
                                                                : "const-param";
            open(tag, p->span());
            attrs(p->attrs);
            ident(p->ident);
            bounds(p->bounds);
            if (p->default_ty) ty(p->default_ty);
            if (p->const_ty) ty(p->const_ty);
            if (p->const_default) anon(*p->const_default);
            close();
        }
    }

    void generics(const Generics& g) {
        if (g.params.empty() && g.where_clause.predicates.empty() && !g.where_clause.has_where_token) return;
        open("generics", g.span);
        generic_params(g.params);
        where(g.where_clause);
        close();
    }

    void where(const WhereClause& w) {
        if (!w.has_where_token) return;
        open("where", w.span);
        for (const WherePredicate* p : w.predicates) {
            open("pred", p->span);
            attrs(p->attrs);
            generic_params(p->bound_generic_params);
            if (p->kind == WherePredicate::Bound) ty(p->bounded_ty);
            if (p->kind == WherePredicate::Region) ident(p->lifetime.ident);
            if (p->kind == WherePredicate::Eq) {
                ty(p->lhs_ty);
                ty(p->rhs_ty);
            }
            bounds(p->bounds);
            close();
        }
        close();
    }

    void ty(const Ty* t) {
        if (!t) return;
        open(ty_kind_name(t->kind), t->span);
        switch (t->kind) {
        case TyKind::Ptr:
        case TyKind::Ref:
        case TyKind::PinnedRef:
            if (t->has_lifetime) ident(t->lifetime.ident);
            if (t->mutbl == Mutability::Mut) word("mut");
            break;
        case TyKind::TraitObject:
            if (t->dyn_syntax) word("dyn");
            break;
        default:
            break;
        }
        if (t->fn_ptr) {
            const FnPtrTy* f = t->fn_ptr;
            if (f->safety.kind == Safety::Unsafe) word("unsafe");
            if (f->ext.kind != Extern::None) word("extern");
            generic_params(f->generic_params);
            fn_decl(f->decl);
        }
        generic_params(t->generic_params);
        ty(t->inner);
        anon(t->len);
        for (const Ty* e : t->elems) ty(e);
        if (t->path) qpath(t->qself, t->path);
        bounds(t->bounds);
        if (t->mac) mac(t->mac, t->span);
        if (t->pat) ty_pat(t->pat);
        if (t->kind == TyKind::FieldOf) {
            if (t->has_variant) ident(t->variant);
            ident(t->field);
        }
        close();
    }

    void ty_pat(const TyPat* p) {
        if (!p) return;
        open("ty-pat", p->span);
        if (p->start) anon(*p->start);
        if (p->kind == TyPat::Range) word(p->range_end == RangeEndKind::Excluded ? ".." : "..=");
        if (p->end) anon(*p->end);
        if (p->kind == TyPat::NotNull) word("!null");
        for (const TyPat* a : p->alts) ty_pat(a);
        close();
    }

    void pat(const Pat* p) {
        if (!p) return;
        open(pat_kind_name(p->kind), p->span);
        if (p->kind == PatKind::Ident) {
            if (p->by_ref) word(p->by_ref_mutbl == Mutability::Mut ? "ref-mut" : "ref");
            if (p->mutbl == Mutability::Mut) word("mut");
            ident(p->ident);
        }
        if (p->kind == PatKind::Ref && p->mutbl == Mutability::Mut) word("mut");
        if (p->path) qpath(p->qself, p->path);
        for (const PatField& f : p->fields) {
            open("field", f.span);
            attrs(f.attrs);
            ident(f.ident);
            if (f.is_shorthand) word("shorthand");
            pat(f.pat);
            close();
        }
        if (p->rest != Pat::Rest::None) word("..");
        for (const Pat* e : p->elems) pat(e);
        if (p->expr) expr(p->expr);
        if (p->kind == PatKind::Range) {
            if (p->range_lo) expr(p->range_lo);
            word(p->range_end == RangeEndKind::Excluded ? ".." : p->range_end == RangeEndKind::IncludedDotDotDot ? "..." : "..=");
            if (p->range_hi) expr(p->range_hi);
        }
        pat(p->sub);
        if (p->guard && p->guard->cond) expr(p->guard->cond);
        if (p->mac) mac(p->mac, p->span);
        close();
    }

    void lit(const Lit& l) {
        out += ' ';
        out += r_.text(l.span);
    }

    void label(bool has, const Label& l) {
        if (has) ident(l.ident);
    }

    void expr(const Expr* e) {
        if (!e) {
            out += " <null>";
            return;
        }
        open(expr_kind_name(e->kind), e->span);
        attrs(e->attrs);
        switch (e->kind) {
        case ExprKind::Binary: word(binop_str(e->binop)); break;
        case ExprKind::AssignOp: word(assignop_str(e->assignop)); break;
        case ExprKind::Unary: word(unop_str(e->unop)); break;
        case ExprKind::Lit: lit(e->lit); break;
        case ExprKind::Range: word(e->limits == RangeLimits::Closed ? "..=" : ".."); break;
        case ExprKind::AddrOf:
            if (e->borrow == BorrowKind::Raw) word("raw");
            if (e->borrow == BorrowKind::Pin) word("pin");
            if (e->mutbl == Mutability::Mut) word("mut");
            break;
        case ExprKind::MethodCall:
            ident(e->seg.ident);
            if (e->seg.args) generic_args(e->seg.args);
            break;
        case ExprKind::Field: ident(e->ident); break;
        case ExprKind::Gen:
            if (e->capture == CaptureBy::Value) word("move");
            break;
        case ExprKind::ForLoop:
            if (e->for_kind == ForLoopKind::ForAwait) word("await");
            break;
        case ExprKind::Match:
            if (e->match_kind == MatchKind::Postfix) word("postfix");
            break;
        case ExprKind::Block:
            if (e->block && e->block->rules == BlockCheckMode::Unsafe) word("unsafe");
            break;
        default: break;
        }
        label(e->has_label, e->label);
        if (e->path) qpath(e->qself, e->path);
        if (e->pat) pat(e->pat);
        if (e->lhs) expr(e->lhs);
        for (const Expr* x : e->exprs) expr(x);
        if (e->ty) ty(e->ty);
        for (const Ident& f : e->fields) ident(f);
        if (e->closure) closure(e->closure);
        if (e->block) block(e->block);
        for (const Arm& a : e->arms) {
            open("arm", a.span);
            attrs(a.attrs);
            pat(a.pat);
            if (a.guard) {
                word("if");
                expr(a.guard->cond);
            }
            if (a.body) expr(a.body);
            close();
        }
        for (const ExprField& f : e->struct_fields) {
            open("field", f.span);
            attrs(f.attrs);
            ident(f.ident);
            if (f.is_shorthand) word("shorthand");
            expr(f.expr);
            close();
        }
        if (e->rest == StructRestKind::Rest) word("..");
        if (e->rest == StructRestKind::Base) word("..base");
        if (e->rhs) expr(e->rhs);
        anon(e->anon_const);
        if (e->mac) mac(e->mac, e->span);
        close();
    }

    void closure(const Closure* c) {
        if (c->binder.present) generic_params(c->binder.generic_params);
        if (c->constness.yes) word("const");
        if (c->coroutine_kind.kind == CoroutineKind::Async) word("async");
        if (c->movability == Movability::Static) word("static");
        if (c->capture == CaptureBy::Value) word("move");
        if (c->capture == CaptureBy::Use) word("use");
        fn_decl(c->fn_decl);
        expr(c->body);
    }

    void fn_decl(const FnDecl* d) {
        if (!d) return;
        out += " (params";
        for (const Param* p : d->inputs) {
            open("param", p->span);
            attrs(p->attrs);
            pat(p->pat);
            ty(p->ty);
            close();
        }
        out += ')';
        if (d->output.ty) {
            word("->");
            ty(d->output.ty);
        }
    }

    void block(const Block* b) {
        if (!b) return;
        open("block", b->span);
        for (const Stmt* s : b->stmts) stmt(s);
        close();
    }

    void stmt(const Stmt* s) {
        switch (s->kind) {
        case StmtKind::Let: {
            const Local* l = s->local;
            open("let", s->span);
            attrs(l->attrs);
            if (l->is_super) word("super");
            pat(l->pat);
            ty(l->ty);
            if (l->init) expr(l->init);
            if (l->els) {
                word("else");
                block(l->els);
            }
            close();
            break;
        }
        case StmtKind::Item: item(s->item); break;
        case StmtKind::Expr:
            open("expr-stmt", s->span);
            expr(s->expr);
            close();
            break;
        case StmtKind::Semi:
            open("semi", s->span);
            expr(s->expr);
            close();
            break;
        case StmtKind::Empty:
            open("empty", s->span);
            close();
            break;
        case StmtKind::MacCall:
            open("mac-stmt", s->span);
            attrs(s->mac->attrs);
            word(s->mac->style == MacStmtStyle::Semicolon ? "semi" : s->mac->style == MacStmtStyle::Braces ? "braces" : "nobraces");
            mac(s->mac->mac, s->span);
            close();
            break;
        }
    }

    void vis(const Visibility& v) {
        if (v.kind == Visibility::Inherited) return;
        open("vis", v.span);
        if (v.path) path(v.path);
        close();
    }

    void field_def(const FieldDef* f) {
        open("field", f->span);
        attrs(f->attrs);
        vis(f->vis);
        if (f->has_ident) ident(f->ident);
        ty(f->ty);
        if (f->default_) anon(*f->default_);
        close();
    }

    void variant_data(const VariantData& d) {
        if (d.kind == VariantData::Unit) return;
        out += d.kind == VariantData::Struct ? " (struct-fields" : " (tuple-fields";
        for (const FieldDef* f : d.fields) field_def(f);
        out += ')';
    }

    void use_tree(const UseTree* t) {
        if (!t) return;
        open("use-tree", t->span);
        path(t->prefix);
        if (t->kind == UseTree::Glob) word("*");
        if (t->has_rename) {
            word("as");
            ident(t->rename);
        }
        for (const UseTree* u : t->items) use_tree(u);
        close();
    }

    void item(const Item* it) {
        if (!it) return;
        open(item_kind_name(it->kind), it->span);
        attrs(it->attrs);
        vis(it->vis);
        if (it->defaultness.kind == Defaultness::Default) word("default");
        if (it->safety.kind == Safety::Unsafe) word("unsafe");
        if (it->safety.kind == Safety::Safe) word("safe");
        if (it->constness.yes) word("const");
        if (it->is_auto) word("auto");
        if (it->mutbl == Mutability::Mut) word("mut");
        if (it->kind == ItemKind::Fn) {
            const FnHeader& h = it->sig.header;
            if (h.constness.yes) word("const");
            if (h.coroutine_kind.kind != CoroutineKind::None) word("async/gen");
            if (h.safety.kind == Safety::Unsafe) word("unsafe");
            if (h.safety.kind == Safety::Safe) word("safe");
            if (h.ext.kind != Extern::None) word("extern");
        }
        if (it->has_orig_name) ident(it->orig_name);
        if (!it->ident.empty()) ident(it->ident);
        generics(it->generics);
        if (it->kind == ItemKind::Fn) fn_decl(it->sig.decl);
        if (it->negative) word("!");
        if (it->trait_ref) path(it->trait_ref);
        if (it->kind == ItemKind::Delegation || it->kind == ItemKind::DelegationMac) qpath(it->qself, it->path);
        bounds(it->bounds);
        ty(it->ty);
        if (it->expr) expr(it->expr);
        where(it->after_where_clause);
        use_tree(it->use_tree);
        variant_data(it->data);
        for (const Variant* v : it->variants) {
            open("variant", v->span);
            attrs(v->attrs);
            ident(v->ident);
            variant_data(v->data);
            if (v->disr_expr) anon(*v->disr_expr);
            close();
        }
        if (it->kind == ItemKind::MacCall) mac(it->mac, it->span);
        if (it->kind == ItemKind::MacroDef) delim(it->macro_body);
        block(it->body);
        for (const Item* sub : it->items) item(sub);
        close();
    }

private:
    const ParseResult& r_;
};

}  // namespace

std::string dump_crate(const ParseResult& r) {
    Dumper d(r);
    if (!r.crate) return "(crate)";
    d.open("crate", r.crate->inner_span);
    d.attrs(r.crate->attrs);
    for (const Item* it : r.crate->items) {
        d.out += "\n ";
        d.item(it);
    }
    d.close();
    d.out += '\n';
    return d.out;
}

std::string dump_expr(const ParseResult& r, const Expr* e) {
    Dumper d(r);
    d.expr(e);
    if (!d.out.empty() && d.out[0] == ' ') d.out.erase(0, 1);
    return d.out;
}

}  // namespace rust
