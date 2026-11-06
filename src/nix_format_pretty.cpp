// The printer half of mep's Nix formatter: a port of nixfmt's Pretty
// instances (Nixfmt/Pretty.hs), which turn the tree into a layout
// document. See nix_format_internal.h. Function names follow the Haskell
// ones (prettyApp -> PrettyApp, absorbRHS -> AbsorbRHS, ...); `pretty x`
// for an expression is PExpr, for a term PTerm (which groups a list), and
// prettyTerm (which does not) PrettyTerm.

#include "nix_format_internal.h"

#include <algorithm>
#include <utility>

namespace nixfmt::detail {

namespace {

// ---- document building blocks (Nixfmt/Predoc.hs) ----

Doc operator+(Doc a, Doc b) {
    a.insert(a.end(), std::make_move_iterator(b.begin()), std::make_move_iterator(b.end()));
    return a;
}
Doc &operator+=(Doc &a, Doc b) {
    a.insert(a.end(), std::make_move_iterator(b.begin()), std::make_move_iterator(b.end()));
    return a;
}

Doc TextA(TAnn ann, std::string t) {
    if (t.empty()) return {};
    DocE e;
    e.kind = DocE::Text;
    e.tann = ann;
    e.text = std::move(t);
    return {std::move(e)};
}
Doc Text(std::string t) { return TextA(TAnn::Regular, std::move(t)); }
Doc CommentD(std::string t) { return TextA(TAnn::Comment, std::move(t)); }
Doc Spc(Sp s, int n = 0) {
    DocE e;
    e.kind = DocE::Spacing;
    e.sp = s;
    e.n = n;
    return {std::move(e)};
}
Doc Softbreak() { return Spc(Sp::Softbreak); }  // softline'
Doc Break() { return Spc(Sp::Break); }          // line'
Doc Softspace() { return Spc(Sp::Softspace); }  // softline
Doc Line() { return Spc(Sp::Space); }           // line
Doc Hardspace() { return Spc(Sp::Hardspace); }
Doc Hardline() { return Spc(Sp::Hardline); }
Doc Emptyline() { return Spc(Sp::Emptyline); }
Doc Newline() { return Spc(Sp::Newlines, 1); }

Doc GroupA(GAnn g, Doc d) {
    DocE e;
    e.kind = DocE::Group;
    e.g = g;
    e.kids = std::move(d);
    return {std::move(e)};
}
Doc Group(Doc d) { return GroupA(GAnn::Regular, std::move(d)); }

void NestIn(Doc &d, int dn, int doff) {
    for (auto &e : d) {
        if (e.kind == DocE::Text) {
            e.nest += dn;
            e.off += doff;
        } else if (e.kind == DocE::Group) {
            NestIn(e.kids, dn, doff);
        }
    }
}
Doc Nest(Doc d) {
    NestIn(d, 1, 0);
    return d;
}
Doc Offset(int level, Doc d) {
    NestIn(d, 0, level);
    return d;
}
Doc SepBy(const Doc &sep, std::vector<Doc> items) {
    Doc out;
    for (size_t i = 0; i < items.size(); i++) {
        if (i) out += sep;
        out += std::move(items[i]);
    }
    return out;
}

// ---- trivia ----

bool AllSpaces(const std::string &s) {
    return std::all_of(s.begin(), s.end(), [](char c) { return c == ' '; });
}

Trivium LineCommentT(std::string text) {
    Trivium t;
    t.kind = Trivium::LineComment;
    t.text = std::move(text);
    return t;
}

void MoveTrailUp(AnnHdr &h) {
    if (!h.trail) return;
    h.pre.push_back(LineCommentT(" " + *h.trail));
    h.trail.reset();
}
Leaf MoveTrailUp(Leaf l) {
    MoveTrailUp(static_cast<AnnHdr &>(l));
    return l;
}
Leaf NoTrail(Leaf l) {
    l.trail.reset();
    return l;
}
Leaf NoPre(Leaf l) {
    l.pre.clear();
    return l;
}
ExprP Prepend(const ExprP &e, const Trivia &extra) {
    if (extra.empty()) return e;
    return MapFirst(e, [&](AnnHdr &h) { h.pre.insert(h.pre.begin(), extra.begin(), extra.end()); });
}

Doc PTrivium(const Trivium &t) {
    switch (t.kind) {
    case Trivium::EmptyLine: return Emptyline();
    case Trivium::LineComment: return CommentD("#" + t.text) + Hardline();
    case Trivium::LangAnnotation: return CommentD("/* " + t.text + " */") + Hardspace();
    case Trivium::BlockComment: {
        Doc body;
        for (const auto &l : t.lines) {
            if (l.empty()) body += Emptyline();
            else body += CommentD(l) + Hardline();
        }
        return CommentD(t.doc ? "/**" : "/*") + Hardline() + Offset(2, std::move(body)) + CommentD("*/") + Hardline();
    }
    }
    return {};
}
Doc PTrivia(const Trivia &t) {
    if (t.empty()) return {};
    if (t.size() == 1 && t[0].kind == Trivium::LangAnnotation) return PTrivium(t[0]);
    Doc out = Hardline();
    for (const auto &x : t) out += PTrivium(x);
    return out;
}
Doc PTrail(const std::optional<std::string> &c) {
    if (!c) return {};
    return Hardspace() + TextA(TAnn::TrailingComment, "# " + *c) + Hardline();
}
Doc PHdr(const AnnHdr &h, Doc value) { return PTrivia(h.pre) + std::move(value) + PTrail(h.trail); }
Doc PLeaf(const Leaf &l) { return PHdr(l, Text(TokenText(l))); }
Doc PLeaf(const std::optional<Leaf> &l) { return l ? PLeaf(*l) : Doc{}; }

// ---- predicates ----

Doc PExpr(const ExprP &e);
Doc PTerm(const TermP &t);
Doc PrettyTerm(const Term &t);
bool IsAbsorbable(const Term &t);
bool IsSimpleT(const Term &t);
bool IsSimple(const Expr &e);

template <class T>
bool HasOnlyComments(const std::vector<Item<T>> &items) {
    return !items.empty() && std::all_of(items.begin(), items.end(), [](const Item<T> &i) { return i.comments; });
}

bool IsSimpleT(const Term &t) {
    switch (t.kind) {
    case Term::SimpleString:
    case Term::IndentedString: return !t.str.HasTrivia();
    case Term::Path: return !t.path.HasTrivia();
    case Term::Token:
        return !t.leaf.HasTrivia() && (t.leaf.tok == Tok::Identifier || t.leaf.tok == Tok::Integer ||
                                       t.leaf.tok == Tok::Float || t.leaf.tok == Tok::EnvPath);
    case Term::Selection:
        if (!IsSimpleT(*t.sel_term) || t.or_kw) return false;
        return std::all_of(t.sels.begin(), t.sels.end(), [](const Selector &s) { return s.sel.kind == SimpleSel::Id; });
    case Term::Parenthesized:
        if (t.open.HasTrivia() || t.close.HasTrivia()) return false;
        return IsSimple(*t.paren);
    default: return false;
    }
}
bool IsSimple(const Expr &e) {
    if (e.kind == Expr::Term) return IsSimpleT(*e.term);
    if (e.kind == Expr::Application) {
        if (e.a->kind == Expr::Application && e.a->a->kind == Expr::Application) return false;
        return IsSimple(*e.a) && IsSimple(*e.b);
    }
    return false;
}

bool IsAbsorbable(const Term &t) {
    switch (t.kind) {
    case Term::IndentedString:
        if (t.str.lines.size() >= 2) return true;
        break;
    case Term::Path: return false;
    case Term::Set:
        if (!t.set_items.empty()) return true;
        if (t.open.line != t.close.line) return true;
        if (t.open.HasTrivia() || HasOnlyComments(t.set_items)) return true;
        break;
    case Term::List:
        if (!t.list_items.empty()) return true;
        if (t.open.line != t.close.line) return true;
        if (t.open.HasTrivia() || HasOnlyComments(t.list_items)) return true;
        break;
    case Term::Parenthesized:
        if (!t.open.HasTrivia() && t.paren->kind == Expr::Term) {
            const Term &inner = *t.paren->term;
            const Trivia &pre = FirstTok(inner).pre;
            const bool lang = std::all_of(pre.begin(), pre.end(), [](const Trivium &x) { return x.kind == Trivium::LangAnnotation; });
            return lang && IsAbsorbable(inner);
        }
        break;
    default: break;
    }
    return false;
}

bool IsSimpleSelector(const Selector &s) { return s.sel.kind == SimpleSel::Id; }

bool ParamHasTrivia(const ParamAttr &a) {
    if (a.ellipsis) return a.name.HasTrivia();
    return a.name.HasTrivia() || (a.comma && a.comma->HasTrivia());
}

bool CanFlattenAttrs(const Leaf &open, const std::vector<ParamAttr> &attrs, const Leaf &close) {
    if (open.line != close.line) return false;
    if (close.HasTrivia()) return false;
    if (std::any_of(attrs.begin(), attrs.end(), ParamHasTrivia)) return false;
    auto plain = [](const ParamAttr &a) { return !a.ellipsis && !a.qmark; };
    switch (attrs.size()) {
    case 0: return true;
    case 1: return attrs[0].ellipsis || plain(attrs[0]);
    case 2: return plain(attrs[0]) && (attrs[1].ellipsis || plain(attrs[1]));
    case 3: return plain(attrs[0]) && plain(attrs[1]) && attrs[2].ellipsis;
    default: return false;
    }
}

bool IsAbsorbableExpr(const Expr &e) {
    switch (e.kind) {
    case Expr::Term: return IsAbsorbable(*e.term);
    case Expr::With: return e.b->kind == Expr::Term && IsAbsorbable(*e.b->term);
    case Expr::Abstraction: {
        const Param &p = *e.param;
        if (p.kind == Param::Id) {
            if (e.a->kind == Expr::Term && IsAbsorbable(*e.a->term)) return true;
            if (e.a->kind == Expr::Abstraction) return IsAbsorbableExpr(*e.a);
            return false;
        }
        if (p.kind == Param::Set && CanFlattenAttrs(p.open, p.attrs, p.close) && p.open.pre.empty() && !p.open.trail &&
            e.k1.line == FirstTok(*e.a).line)
            return IsAbsorbableExpr(*e.a);
        return false;
    }
    default: return false;
    }
}

bool IsUpdateConcatPlus(Tok t) { return t == Tok::Update || t == Tok::Concat || t == Tok::Plus; }

// ---- strings ----

Doc PWhole(const Whole &w) { return Group(PExpr(w.e) + PTrivia(w.trivia)); }
Doc AbsorbExpr(bool wide, const ExprP &e);
Doc PrettyApp(bool indent_function, const Doc &pre, bool has_post, const ExprP &f, const ExprP &a);

// What goes inside `( )` or a `${ }` alone on its string line.
Doc ParenInner(const ExprP &e) {
    if (IsAbsorbableExpr(*e)) return Group(AbsorbExpr(false, e));
    if (e->kind == Expr::Application) return PrettyApp(true, {}, true, e->a, e->b);
    if (e->kind == Expr::Term && e->term->kind == Term::Selection) {
        if (IsAbsorbable(*e->term->sel_term)) return Break() + Group(PExpr(e)) + Break();
        return Group(PExpr(e)) + Break();
    }
    return Break() + Group(PExpr(e)) + Break();
}

Doc PStringPart(const StringPart &p) {
    if (!p.interp) return Text(p.text);
    const Whole &w = *p.whole;
    if (w.trivia.empty() && w.e->kind == Expr::Term && IsAbsorbable(*w.e->term)) {
        return Group(Text("${") + PrettyTerm(*w.e->term) + Text("}"));
    }
    if (w.trivia.empty() && IsSimple(*w.e)) {
        Doc d = PExpr(w.e);
        std::optional<Doc> flat = UnexpandSpacing(std::nullopt, d);
        return Text("${") + (flat ? std::move(*flat) : std::move(d)) + Text("}");
    }
    Doc whole = PWhole(w);
    std::optional<Doc> flat = UnexpandSpacing(30, whole);
    Doc inner = flat ? std::move(*flat) : Break() + Nest(std::move(whole)) + Break();
    return Group(Text("${") + std::move(inner) + Text("}"));
}

Doc PStrLine(const StrLine &parts) {
    // An interpolation alone on its line (after nothing but spaces).
    const StringPart *lead = nullptr;
    const StringPart *interp = nullptr;
    if (parts.size() == 1 && parts[0].interp) interp = &parts[0];
    if (parts.size() == 2 && !parts[0].interp && parts[1].interp && AllSpaces(parts[0].text)) {
        lead = &parts[0];
        interp = &parts[1];
    }
    if (interp && interp->whole->trivia.empty()) {
        const std::string pre = lead ? lead->text : "";
        return Text(pre) + Offset(TextWidth(pre), Group(Text("${") + Nest(ParenInner(interp->whole->e)) + Text("}")));
    }
    if (parts.size() == 1 && parts[0].interp) {
        return Group(Text("${") + Break() + Nest(PWhole(*parts[0].whole)) + Break() + Text("}"));
    }
    if (!parts.empty() && !parts[0].interp) {
        const std::string &t = parts[0].text;
        size_t k = 0;
        while (const size_t n = SpaceAt(t, k)) k += n;
        Doc rest;
        for (size_t i = 1; i < parts.size(); i++) rest += PStringPart(parts[i]);
        return Text(t) + Offset(TextWidth(t.substr(0, k)), std::move(rest));
    }
    Doc out;
    for (const auto &p : parts) out += PStringPart(p);
    return out;
}

Doc PrettySimpleString(const std::vector<StrLine> &lines) {
    std::vector<Doc> ls;
    for (const auto &l : lines) ls.push_back(PStrLine(l));
    return Group(Text("\"") + SepBy(Text("\n"), std::move(ls)) + Text("\""));
}
Doc PrettyIndentedString(const std::vector<StrLine> &lines) {
    std::vector<Doc> ls;
    for (const auto &l : lines) ls.push_back(PStrLine(l));
    return Group(Text("''") + (lines.size() >= 2 ? Break() : Doc{}) + Nest(SepBy(Newline(), std::move(ls))) + Text("''"));
}

// ---- selectors, items, binders ----

Doc PSimpleSel(const SimpleSel &s) {
    switch (s.kind) {
    case SimpleSel::Id: return PLeaf(s.id);
    case SimpleSel::Interp: return PHdr(s.interp, PStringPart(s.interp.part));
    case SimpleSel::Str: return PHdr(s.str, PrettySimpleString(s.str.lines));
    }
    return {};
}
Doc PSelector(const Selector &s) { return PLeaf(s.dot) + PSimpleSel(s.sel); }
Doc HcatSels(const std::vector<Selector> &sels) {
    Doc out;
    for (const auto &s : sels) out += PSelector(s);
    return out;
}

template <class T, class F>
Doc RenderItems(const Doc &sep, const std::vector<Item<T>> &items, F &&pvalue) {
    auto pitem = [&](const Item<T> &i) { return i.comments ? PTrivia(i.trivia) : Group(pvalue(i.value)); };
    Doc out;
    size_t i = 0;
    const size_t n = items.size();
    while (i < n) {
        if (i + 1 == n) {
            out += pitem(items[i]);
            break;
        }
        if (items[i].comments && !items[i + 1].comments && !items[i].trivia.empty() &&
            items[i].trivia.back().kind == Trivium::LangAnnotation) {
            Trivia preceding(items[i].trivia.begin(), items[i].trivia.end() - 1);
            out += PTrivia(preceding) + PTrivium(items[i].trivia.back()) + Hardspace() + Group(pvalue(items[i + 1].value));
            i += 2;
            if (i < n) out += sep;
            continue;
        }
        out += pitem(items[i]) + sep;
        i++;
    }
    return out;
}

Doc AbsorbRHS(const ExprP &e);

Doc PBinder(const Binder &b) {
    if (b.kind == Binder::Inherit) {
        const bool compact = b.kw.line == b.semi.line && b.ids.size() < 4;
        const Doc sep = compact ? Line() : Hardline();
        const Doc nosep = compact ? Break() : Hardline();
        std::vector<Doc> ids;
        for (const auto &s : b.ids) ids.push_back(PSimpleSel(s));
        if (!b.from) return Group(PLeaf(b.kw) + sep + Nest(SepBy(sep, std::move(ids)) + nosep + PLeaf(b.semi)));
        return Group(PLeaf(b.kw) +
                     Nest(GroupA(GAnn::Regular, Line() + PTerm(b.from)) + sep + SepBy(sep, std::move(ids)) + nosep + PLeaf(b.semi)));
    }
    const bool simple = b.sels.size() <= 4 && std::all_of(b.sels.begin(), b.sels.end(), IsSimpleSelector);
    Doc rhs = simple ? AbsorbRHS(b.rhs) : Break() + GroupA(GAnn::Priority, AbsorbRHS(b.rhs));
    return Group(HcatSels(b.sels) + Nest(Hardspace() + PLeaf(b.eq) + std::move(rhs)) + PLeaf(b.semi));
}

Doc PrettySet(bool wide, const std::optional<Leaf> &rec, const Leaf &open, const std::vector<Item<Binder>> &items,
              const Leaf &close) {
    Doc krec = rec ? PLeaf(*rec) + Hardspace() : Doc{};
    if (!open.HasTrivia() && items.empty() && close.pre.empty()) {
        return krec + PLeaf(open) + (open.line != close.line ? Hardline() : Hardspace()) + PLeaf(close);
    }
    const Doc sep = ((wide && !items.empty()) || open.line != close.line) ? Hardline() : Line();
    Doc body = Nest(PTrail(open.trail) + RenderItems(Hardline(), items, PBinder));
    return krec + PLeaf(NoTrail(open)) + sep + std::move(body) + sep + PLeaf(close);
}

Doc PrettyTermWide(const Term &t) {
    if (t.kind == Term::Set) return PrettySet(true, t.rec, t.open, t.set_items, t.close);
    return PrettyTerm(t);
}

Doc RenderList(const Doc &item_sep, const Leaf &open, const std::vector<Item<TermP>> &items, const Leaf &close) {
    Doc sur;
    if (open.line != close.line || HasOnlyComments(items) || (open.HasTrivia() && items.empty())) sur = Hardline();
    else if (items.empty()) sur = Hardspace();
    else sur = Line();
    Doc body = Nest(PTrail(open.trail) + RenderItems(item_sep, items, [](const TermP &t) { return PTerm(t); }));
    return PLeaf(NoTrail(open)) + sur + std::move(body) + sur + PLeaf(close);
}

Doc PrettyTerm(const Term &t) {
    switch (t.kind) {
    case Term::Token: return PLeaf(t.leaf);
    case Term::SimpleString: return PHdr(t.str, PrettySimpleString(t.str.lines));
    case Term::IndentedString: return PHdr(t.str, PrettyIndentedString(t.str.lines));
    case Term::Path: return PHdr(t.path, PStrLine(t.path.parts));
    case Term::Selection: {
        const Term &inner = *t.sel_term;
        Doc sep;
        if (inner.kind == Term::Token) {
            if (inner.leaf.tok == Tok::Integer || inner.leaf.tok == Tok::Float) sep = Hardspace();
        } else if (inner.kind == Term::Parenthesized) {
            sep = Softbreak();
        } else {
            sep = Break();
        }
        Doc out = PTerm(t.sel_term) + std::move(sep) + HcatSels(t.sels);
        if (t.or_kw) out += Softspace() + Nest(PLeaf(*t.or_kw) + Hardspace() + PTerm(t.or_def));
        return out;
    }
    case Term::List:
        if (!t.open.trail && t.list_items.empty() && t.close.pre.empty()) {
            return PLeaf(t.open) + (t.open.line != t.close.line ? Hardline() : Hardspace()) + PLeaf(t.close);
        }
        return RenderList(Hardline(), t.open, t.list_items, t.close);
    case Term::Set: return PrettySet(false, t.rec, t.open, t.set_items, t.close);
    case Term::Parenthesized:
        return Group(PLeaf(MoveTrailUp(t.open)) + Nest(ParenInner(t.paren) + PTrivia(t.close.pre)) + PLeaf(NoPre(t.close)));
    }
    return {};
}

Doc PTerm(const TermP &t) {
    if (t->kind == Term::List) return Group(PrettyTerm(*t));
    return PrettyTerm(*t);
}

// ---- parameters ----

Doc PParamAttr(const ParamAttr &a) {
    if (a.ellipsis) return PLeaf(a.name);
    if (!a.qmark) return PLeaf(a.name) + PLeaf(a.comma);
    return Group(PLeaf(a.name) + Hardspace() + Nest(PLeaf(*a.qmark) + AbsorbRHS(a.def)) + PLeaf(a.comma));
}

bool LoneLeaf(const Leaf &l) { return !l.HasTrivia(); }

// `, name # foo` -> `name, # foo`
ParamAttr MoveParamAttrComment(ParamAttr a) {
    if (a.ellipsis || !a.comma || !LoneLeaf(*a.comma)) return a;
    if (!a.qmark) {
        if (a.name.trail) {
            a.comma->trail = std::move(a.name.trail);
            a.name.trail.reset();
        }
        return a;
    }
    std::optional<std::string> c = LastTok(*a.def).trail;
    if (c) a.def = MapLast(a.def, [](AnnHdr &h) { h.trail.reset(); });
    a.comma->trail = std::move(c);
    return a;
}

// Comments before a `, name` belong to that name, not to the comma before
// it; and the last attribute gets a comma.
std::vector<ParamAttr> MoveParamsComments(std::vector<ParamAttr> v) {
    const size_t n = v.size();
    for (size_t i = 0; i < n; i++) {
        ParamAttr &x = v[i];
        if (x.ellipsis) continue;
        if (x.comma && !x.comma->trail && i + 1 < n && !v[i + 1].ellipsis) {
            Trivia &dst = v[i + 1].name.pre;
            dst.insert(dst.begin(), x.comma->pre.begin(), x.comma->pre.end());
            x.comma->pre.clear();
            continue;
        }
        if (x.comma && !x.comma->trail && i + 2 == n && v[i + 1].ellipsis) {
            Trivia &dst = v[i + 1].name.pre;
            dst.insert(dst.begin(), x.comma->pre.begin(), x.comma->pre.end());
            x.comma->pre.clear();
            break;
        }
        if (!x.comma && i + 1 == n) {
            Leaf comma;
            comma.tok = Tok::Comma;
            comma.line = x.name.line;
            x.comma = std::move(comma);
        }
    }
    return v;
}

Doc PParam(const Param &p) {
    switch (p.kind) {
    case Param::Id: return PLeaf(p.name);
    case Param::Context: return PParam(*p.first) + PLeaf(p.at) + PParam(*p.second);
    case Param::Set: {
        if (p.attrs.empty()) {
            return Group(PLeaf(MoveTrailUp(p.open)) + (p.open.line != p.close.line ? Hardline() : Hardspace()) + PLeaf(p.close));
        }
        std::vector<ParamAttr> attrs = MoveParamsComments(p.attrs);
        std::vector<Doc> docs;
        for (size_t i = 0; i < attrs.size(); i++) {
            ParamAttr a = MoveParamAttrComment(std::move(attrs[i]));
            if (i + 1 == attrs.size() && !a.ellipsis && a.comma && LoneLeaf(*a.comma) && a.comma->tok == Tok::Comma) {
                a.comma.reset();
                docs.push_back(PParamAttr(a) + TextA(TAnn::Trailing, ","));
            } else {
                docs.push_back(PParamAttr(a));
            }
        }
        const Doc sep = CanFlattenAttrs(p.open, p.attrs, p.close) ? Line() : Hardline();
        return Group(PLeaf(MoveTrailUp(p.open)) + sep + Nest(SepBy(sep, std::move(docs))) + sep + Nest(PTrivia(p.close.pre)) +
                     PLeaf(NoPre(p.close)));
    }
    }
    return {};
}

// ---- applications ----

Doc AbsorbParen(const Leaf &open, const ExprP &expr, const Leaf &close) {
    Trivia moved;
    if (open.trail) moved.push_back(LineCommentT(" " + *open.trail));
    Doc inner = GroupA(GAnn::Regular, Nest(PExpr(Prepend(expr, moved)) + PTrivia(close.pre)));
    return GroupA(GAnn::Priority, Nest(PLeaf(NoTrail(open)) + Break() + std::move(inner) + Break() + PLeaf(NoPre(close))));
}

struct AppCtx {
    bool indent_function;
    bool comment_empty;

    Doc AbsorbInner(const ExprP &e) const {
        if (e->kind == Expr::Term && e->term->kind == Term::List) {
            const Term &t = *e->term;
            const bool simple = t.list_items.size() <= 6 &&
                                std::all_of(t.list_items.begin(), t.list_items.end(),
                                            [](const Item<TermP> &i) { return i.comments || IsSimpleT(*i.value); });
            if (simple) return RenderList(Line(), t.open, t.list_items, t.close);
        }
        return PExpr(e);
    }
    static bool IsTermKind(const ExprP &e, Term::Kind k) { return e->kind == Expr::Term && e->term->kind == k; }

    Doc AbsorbApp(const ExprP &e) const {
        if (e->kind == Expr::Application) {
            const ExprP &f2 = e->a;
            const ExprP &a2 = e->b;
            if (IsTermKind(a2, Term::Selection)) {
                return GroupA(GAnn::Transparent, AbsorbApp(f2)) + Line() + Nest(GroupA(GAnn::Regular, AbsorbInner(a2)));
            }
            if (f2->kind == Expr::Application && IsTermKind(f2->b, Term::List) && IsTermKind(a2, Term::List)) {
                return GroupA(GAnn::Transparent,
                              GroupA(GAnn::Transparent, AbsorbApp(f2->a)) +
                                  Nest(GroupA(GAnn::Regular, Line() + Group(AbsorbInner(f2->b)) + Line() + Group(AbsorbInner(a2)))));
            }
            return GroupA(GAnn::Transparent, AbsorbApp(f2)) + Line() + Nest(GroupA(GAnn::Priority, AbsorbInner(a2)));
        }
        if (indent_function && comment_empty) return Nest(GroupA(GAnn::Regular, Break() + PExpr(e)));
        return PExpr(e);
    }

    static Doc AbsorbLast(const ExprP &a) {
        if (a->kind == Expr::Term) {
            const Term &t = *a->term;
            if (IsAbsorbable(t)) return GroupA(GAnn::Priority, Nest(PrettyTerm(t)));
            if (t.kind == Term::Parenthesized) {
                const Expr &in = *t.paren;
                if (in.kind == Expr::Abstraction && in.param->kind == Param::Id && in.a->kind == Expr::Term &&
                    IsAbsorbable(*in.a->term) && !t.open.HasTrivia() && !in.param->name.HasTrivia() && !in.k1.HasTrivia()) {
                    return GroupA(GAnn::Priority, Nest(PLeaf(t.open) + PLeaf(in.param->name) + PLeaf(in.k1) + Hardspace() +
                                                       PrettyTermWide(*in.a->term) + PLeaf(t.close)));
                }
                if (in.kind == Expr::Application && IsTermKind(in.a, Term::Token) && in.a->term->leaf.tok == Tok::Identifier &&
                    in.b->kind == Expr::Term && IsAbsorbable(*in.b->term) && !t.open.HasTrivia() &&
                    !in.a->term->leaf.HasTrivia() && !t.close.HasTrivia()) {
                    return GroupA(GAnn::Priority, Nest(PLeaf(t.open) + Text(in.a->term->leaf.text) + Hardspace() +
                                                       PrettyTermWide(*in.b->term) + PLeaf(t.close)));
                }
                return AbsorbParen(t.open, t.paren, t.close);
            }
        }
        return GroupA(GAnn::Regular, Nest(PExpr(a)));
    }
};

Doc PrettyApp(bool indent_function, const Doc &pre, bool has_post, const ExprP &f, const ExprP &a) {
    Trivia comment;
    const ExprP fw = MapFirst(f, [&](AnnHdr &h) {
        MoveTrailUp(h);
        comment = std::move(h.pre);
        h.pre.clear();
    });
    const AppCtx ctx{indent_function, comment.empty()};
    const Doc post = has_post ? Break() : Doc{};
    Doc out = PTrivia(comment);
    if (fw->kind == Expr::Application && AppCtx::IsTermKind(fw->b, Term::List) && AppCtx::IsTermKind(a, Term::List)) {
        out += GroupA(GAnn::Regular, pre + GroupA(GAnn::Transparent, ctx.AbsorbApp(fw->a)) + Line() +
                                         Nest(Group(ctx.AbsorbInner(fw->b))) + Line() + Nest(Group(ctx.AbsorbInner(a))) + post);
    } else {
        Doc rendered = pre + GroupA(GAnn::Transparent, ctx.AbsorbApp(fw));
        const bool simple = (f->kind != Expr::Application || f->a->kind != Expr::Application) && IsSimple(*f) && IsSimple(*a);
        std::optional<Doc> flat = simple ? UnexpandSpacing(std::nullopt, rendered) : std::nullopt;
        if (flat) out += GroupA(GAnn::Regular, std::move(*flat) + Hardspace() + AppCtx::AbsorbLast(a));
        else out += GroupA(GAnn::Regular, std::move(rendered) + Line() + AppCtx::AbsorbLast(a) + post);
    }
    if (has_post && !comment.empty()) out += Hardline();
    return out;
}

// ---- operators ----

void Flatten(const Leaf *op_l, const ExprP &e, const Leaf &op, std::vector<std::pair<const Leaf *, ExprP>> &out) {
    if (e->kind == Expr::Operation && e->k1.tok == op.tok) {
        Flatten(op_l, e->a, op, out);
        Flatten(&e->k1, e->b, op, out);
        return;
    }
    out.emplace_back(op_l, e);
}

Doc AbsorbOperation(const ExprP &e) {
    if (e->kind == Expr::Term && IsAbsorbable(*e->term)) return Hardspace() + PTerm(e->term);
    if (e->kind == Expr::Operation) return GroupA(GAnn::Regular, Line() + PExpr(e));
    if (e->kind == Expr::Application) return Group(Hardspace() + PrettyApp(false, Line(), false, e->a, e->b));
    return Hardspace() + PExpr(e);
}

Doc PrettyOp(bool force_first_wide, const ExprP &operation, const Leaf &op) {
    std::vector<std::pair<const Leaf *, ExprP>> parts;
    Flatten(nullptr, operation, op, parts);
    const bool pipe = op.tok == Tok::PipeForward || op.tok == Tok::PipeBackward;
    const Doc sep = pipe && parts.size() >= 4 ? Hardline() : Line();
    Doc out;
    for (const auto &[op_l, e] : parts) {
        if (!op_l) {
            if (e->kind == Expr::Term && IsAbsorbable(*e->term) && force_first_wide) out += PrettyTermWide(*e->term);
            else out += PExpr(e);
        } else {
            out += sep + PLeaf(MoveTrailUp(*op_l)) + Nest(AbsorbOperation(e));
        }
    }
    return GroupA(GAnn::Regular, std::move(out));
}

Doc PrettyWith(bool absorb, const ExprP &e) {
    if (absorb && e->b->kind == Expr::Term) {
        return GroupA(GAnn::Regular, Break() + PLeaf(e->k1) + Hardspace() + Nest(Group(PExpr(e->a))) + PLeaf(e->k2) + Hardspace() +
                                         GroupA(GAnn::Priority, PrettyTermWide(*e->b->term)));
    }
    return Group(PLeaf(e->k1) + Hardspace() + Nest(Group(PExpr(e->a))) + PLeaf(e->k2)) + Line() + PExpr(e->b);
}

Doc AbsorbExpr(bool wide, const ExprP &e) {
    if (e->kind == Expr::Term && IsAbsorbable(*e->term)) return wide ? PrettyTermWide(*e->term) : PrettyTerm(*e->term);
    if (e->kind == Expr::With && e->b->kind == Expr::Term && IsAbsorbable(*e->b->term)) return PrettyWith(true, e);
    return PExpr(e);
}

Doc AbsorbRHS(const ExprP &e) {
    if (e->kind == Expr::Term && e->term->kind == Term::Set) {
        const auto &items = e->term->set_items;
        if (items.size() == 1 && !items[0].comments && items[0].value.kind == Binder::Inherit) {
            return Nest(Hardspace() + Group(AbsorbExpr(false, e)));
        }
    }
    if (IsAbsorbableExpr(*e)) return Nest(Hardspace() + Group(AbsorbExpr(true, e)));
    if (e->kind == Expr::Term) {
        const Term &t = *e->term;
        if (t.kind == Term::Parenthesized) return Nest(Hardspace() + AbsorbParen(t.open, t.paren, t.close));
        if (t.kind == Term::SimpleString || t.kind == Term::IndentedString || t.kind == Term::Path) {
            return Nest(Hardspace() + Group(PExpr(e)));
        }
        return Nest(GroupA(GAnn::Regular, Line() + PExpr(e)));
    }
    if (e->kind == Expr::Application) return Nest(PrettyApp(false, Line(), false, e->a, e->b));
    if (e->kind == Expr::With) return Nest(GroupA(GAnn::Regular, Line() + PExpr(e)));
    if (e->kind == Expr::Operation && IsUpdateConcatPlus(e->k1.tok)) {
        if (e->a->kind == Expr::Term && IsAbsorbable(*e->a->term) && FirstTok(*e->a->term).pre.empty()) {
            return Hardspace() + PrettyOp(true, e, e->k1);
        }
        if (!e->k1.HasTrivia() && e->b->kind == Expr::Term && IsAbsorbable(*e->b->term)) {
            return Nest(GroupA(GAnn::Regular, Line() + PExpr(e->a) + Line() +
                                                  GroupA(GAnn::Transparent, Text(TokenText(e->k1)) + Hardspace() +
                                                                                GroupA(GAnn::Priority, PrettyTermWide(*e->b->term)))));
        }
    }
    return Nest(Line() + Group(PExpr(e)));
}

// ---- expressions ----

Doc PrettyIf(const Doc &sep, const ExprP &e) {
    if (e->kind != Expr::If) return Line() + Nest(Group(PExpr(e)));
    return Group(PLeaf(e->k1) + Line() + Nest(PExpr(e->a)) + Line() + PLeaf(e->k2)) + sep + Nest(Group(PExpr(e->b))) + sep +
           PLeaf(MoveTrailUp(e->k3)) + Hardspace() + PrettyIf(Hardline(), e->c);
}

Doc AbsorbAbs(int depth, const ExprP &e) {
    if (e->kind == Expr::Abstraction && e->param->kind == Param::Id) {
        return Hardspace() + PLeaf(e->param->name) + PLeaf(e->k1) + AbsorbAbs(depth + 1, e->a);
    }
    if (IsAbsorbableExpr(*e)) return Hardspace() + GroupA(GAnn::Priority, AbsorbExpr(false, e));
    return (depth <= 2 ? Line() : Hardline()) + PExpr(e);
}

ExprP TokenExpr(const Leaf &l) {
    auto t = std::make_shared<Term>();
    t->kind = Term::Token;
    t->leaf = l;
    auto e = std::make_shared<Expr>();
    e->kind = Expr::Term;
    e->term = std::move(t);
    return e;
}
ExprP App(ExprP f, ExprP a) {
    auto e = std::make_shared<Expr>();
    e->kind = Expr::Application;
    e->a = std::move(f);
    e->b = std::move(a);
    return e;
}
// insertIntoApp: `assert` put in front of the condition's function call.
std::pair<ExprP, ExprP> InsertIntoApp(const ExprP &insert, const ExprP &e) {
    if (e->kind == Expr::Application) {
        auto [f, a] = InsertIntoApp(insert, e->a);
        return {App(std::move(f), std::move(a)), e->b};
    }
    return {insert, e};
}

Doc PExpr(const ExprP &e) {
    switch (e->kind) {
    case Expr::Term: return PTerm(e->term);
    case Expr::With: return PrettyWith(false, e);
    case Expr::Let: {
        Trivia moved = e->k2.pre;
        if (e->k2.trail) moved.push_back(LineCommentT(" " + *e->k2.trail));
        const ExprP body = Prepend(e->a, moved);
        Doc let_part = Group(PLeaf(e->k1) + Hardline() + Nest(RenderItems(Hardline(), e->binders, PBinder)));
        Doc in_part = Group(Text("in") + Hardline() + PExpr(body));
        return std::move(let_part) + Hardline() + std::move(in_part);
    }
    case Expr::Assert: {
        auto [f, a] = InsertIntoApp(TokenExpr(e->k1), e->a);
        return Group(PrettyApp(false, {}, false, f, a) + PLeaf(e->k2) + Hardline() + PExpr(e->b));
    }
    case Expr::If: return GroupA(GAnn::Regular, PrettyIf(Line(), MapFirst(e, [](AnnHdr &h) { MoveTrailUp(h); })));
    case Expr::Abstraction: {
        const Param &p = *e->param;
        if (p.kind == Param::Id) return GroupA(GAnn::Regular, Break() + PLeaf(p.name) + PLeaf(e->k1) + AbsorbAbs(1, e->a));
        if (p.kind == Param::Set) {
            const int body_line = FirstTok(*e->a).line;
            if (CanFlattenAttrs(p.open, p.attrs, p.close) && e->k1.line == body_line && IsAbsorbableExpr(*e->a)) {
                return PParam(p) + PLeaf(e->k1) + Hardspace() + GroupA(GAnn::Priority, PExpr(e->a));
            }
            if (e->k1.line != body_line) return PParam(p) + PLeaf(e->k1) + Hardline() + PExpr(e->a);
        }
        if (e->a->kind == Expr::Term && IsAbsorbable(*e->a->term)) {
            return PParam(p) + PLeaf(e->k1) + Line() + Group(PrettyTermWide(*e->a->term));
        }
        return PParam(p) + PLeaf(e->k1) + Line() + PExpr(e->a);
    }
    case Expr::Application: return PrettyApp(false, {}, false, e->a, e->b);
    case Expr::Operation: {
        const Tok t = e->k1.tok;
        if (t == Tok::Less || t == Tok::Greater || t == Tok::LessEqual || t == Tok::GreaterEqual || t == Tok::Equal ||
            t == Tok::Unequal) {
            return PExpr(e->a) + Softspace() + PLeaf(e->k1) + Hardspace() + PExpr(e->b);
        }
        return PrettyOp(false, e, e->k1);
    }
    case Expr::MemberCheck: return PExpr(e->a) + Softspace() + PLeaf(e->k1) + Hardspace() + HcatSels(e->sels);
    case Expr::Negation:
    case Expr::Inversion: return PLeaf(e->k1) + PExpr(e->a);
    }
    return {};
}

}  // namespace

Doc PrettyFile(const Whole &w) { return PWhole(w); }

}  // namespace nixfmt::detail
