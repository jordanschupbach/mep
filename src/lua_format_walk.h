#pragma once

// Token walks over the Lua formatter's CST (lua_format_cst.h).
//
// Walk(node, f, node_order) calls f(const TokenRef &) for every token of the
// node and stops early when f returns true. Display order (node_order false)
// is the order the tokens print in. Node order is full-moon's Node::tokens()
// order, which StyLua's width tests (test_over_budget) see: a bracketed
// span's two tokens come before its contents, and a `local` statement's names
// come before their attributes.
//
// FirstTok / LastTok give the token a node's leading / trailing trivia
// belongs to (StyLua's update_leading_trivia / update_trailing_trivia
// targets): the first and last tokens, except that a Punctuated list's
// trailing trivia is its last value's (never a trailing separator).

#include "lua_format_cst.h"

namespace luafmt::cst {

template <class F>
bool Walk(const TokenRef &t, F &f, bool) {
    return f(t);
}
template <class F>
bool Walk(const Expr &e, F &f, bool node);
template <class F>
bool Walk(const Block &b, F &f, bool node);
template <class F>
bool Walk(const TableConstructor &t, F &f, bool node);
template <class F>
bool Walk(const FunctionCall &c, F &f, bool node);
template <class F>
bool Walk(const FunctionBody &b, F &f, bool node);

template <class T, class F>
bool Walk(const Punctuated<T> &p, F &f, bool node) {
    for (const auto &pr : p.pairs) {
        if (Walk(pr.value, f, node)) return true;
        if (pr.punct && f(*pr.punct)) return true;
    }
    return false;
}

template <class F>
bool Walk(const ContainedSpan &c, F &f, bool) {
    return f(c.open) || f(c.close);
}

template <class F>
bool Walk(const Prefix &p, F &f, bool node) {
    return p.is_name ? f(p.name) : Walk(*p.expr, f, node);
}

template <class F>
bool Walk(const FunctionArgs &a, F &f, bool node) {
    switch (a.kind) {
    case FunctionArgs::Parens:
        if (node) return f(a.parens.open) || f(a.parens.close) || Walk(*a.args, f, node);
        return f(a.parens.open) || Walk(*a.args, f, node) || f(a.parens.close);
    case FunctionArgs::String:
        return f(a.str);
    case FunctionArgs::Table:
        return Walk(*a.table, f, node);
    }
    return false;
}

template <class F>
bool Walk(const Index &i, F &f, bool node) {
    if (i.is_dot) return f(i.dot) || f(i.name);
    if (node) return f(i.brackets.open) || f(i.brackets.close) || Walk(*i.expr, f, node);
    return f(i.brackets.open) || Walk(*i.expr, f, node) || f(i.brackets.close);
}

template <class F>
bool Walk(const Suffix &s, F &f, bool node) {
    switch (s.kind) {
    case Suffix::kAnon:
        return Walk(s.args, f, node);
    case Suffix::kMethod:
        return f(s.colon) || f(s.method_name) || Walk(s.args, f, node);
    case Suffix::kIndex:
        return Walk(s.index, f, node);
    }
    return false;
}

template <class F>
bool Walk(const FunctionCall &c, F &f, bool node) {
    if (Walk(c.prefix, f, node)) return true;
    for (const auto &s : c.suffixes)
        if (Walk(s, f, node)) return true;
    return false;
}

template <class F>
bool Walk(const Var &v, F &f, bool node) {
    return v.is_name ? f(v.name) : Walk(*v.expr, f, node);
}

template <class F>
bool Walk(const Field &fd, F &f, bool node) {
    switch (fd.kind) {
    case Field::ExprKey:
        if (node)
            return f(fd.brackets.open) || f(fd.brackets.close) || Walk(*fd.key, f, node) || f(fd.equal) ||
                   Walk(*fd.value, f, node);
        return f(fd.brackets.open) || Walk(*fd.key, f, node) || f(fd.brackets.close) || f(fd.equal) ||
               Walk(*fd.value, f, node);
    case Field::NameKey:
        return f(fd.name) || f(fd.equal) || Walk(*fd.value, f, node);
    case Field::NoKey:
        return Walk(*fd.value, f, node);
    }
    return false;
}

template <class F>
bool Walk(const TableConstructor &t, F &f, bool node) {
    if (node) return f(t.braces.open) || f(t.braces.close) || Walk(t.fields, f, node);
    return f(t.braces.open) || Walk(t.fields, f, node) || f(t.braces.close);
}

template <class F>
bool Walk(const FunctionBody &b, F &f, bool node) {
    if (node) return f(b.parens.open) || f(b.parens.close) || Walk(b.params, f, node) || Walk(b.block, f, node) || f(b.end_token);
    return f(b.parens.open) || Walk(b.params, f, node) || f(b.parens.close) || Walk(b.block, f, node) || f(b.end_token);
}

template <class F>
bool Walk(const Expr &e, F &f, bool node) {
    switch (e.kind) {
    case EK::Binary:
        return Walk(*e.lhs, f, node) || f(e.tok) || Walk(*e.rhs, f, node);
    case EK::Paren:
        if (node) return f(e.contained.open) || f(e.contained.close) || Walk(*e.rhs, f, node);
        return f(e.contained.open) || Walk(*e.rhs, f, node) || f(e.contained.close);
    case EK::Unary:
        return f(e.tok) || Walk(*e.rhs, f, node);
    case EK::Function:
        return f(e.func->function_token) || Walk(*e.func->body, f, node);
    case EK::Call:
        return Walk(*e.call, f, node);
    case EK::Table:
        return Walk(*e.table, f, node);
    case EK::Number:
    case EK::String:
    case EK::Symbol:
        return f(e.tok);
    case EK::Var:
        return Walk(*e.var, f, node);
    }
    return false;
}

template <class F>
bool Walk(const Attribute &a, F &f, bool node) {
    if (node) return f(a.brackets.open) || f(a.brackets.close) || f(a.name);
    return f(a.brackets.open) || f(a.name) || f(a.brackets.close);
}

template <class F>
bool Walk(const FunctionName &n, F &f, bool node) {
    if (Walk(n.names, f, node)) return true;
    if (n.method) return f(n.method->first) || f(n.method->second);
    return false;
}

template <class F>
bool Walk(const LastStmt &s, F &f, bool node) {
    return f(s.token) || Walk(s.returns, f, node);
}

template <class F>
bool Walk(const Stmt &s, F &f, bool node) {
    switch (s.kind) {
    case SK::Assignment:
        return Walk(s.vars, f, node) || f(*s.equal) || Walk(s.exprs, f, node);
    case SK::Local: {
        if (f(s.local_token)) return true;
        if (node) {
            if (Walk(s.names, f, node)) return true;
            for (const auto &a : s.attributes)
                if (a && Walk(*a, f, node)) return true;
        } else {
            for (size_t i = 0; i < s.names.pairs.size(); i++) {
                if (f(s.names.pairs[i].value)) return true;
                if (i < s.attributes.size() && s.attributes[i] && Walk(*s.attributes[i], f, node)) return true;
                if (s.names.pairs[i].punct && f(*s.names.pairs[i].punct)) return true;
            }
        }
        if (s.equal && f(*s.equal)) return true;
        return Walk(s.exprs, f, node);
    }
    case SK::Call:
        return Walk(*s.call, f, node);
    case SK::Do:
        return f(s.first) || Walk(*s.block, f, node) || f(s.end_token);
    case SK::While:
        return f(s.first) || Walk(*s.cond, f, node) || f(s.do_token) || Walk(*s.block, f, node) || f(s.end_token);
    case SK::Repeat:
        return f(s.first) || Walk(*s.block, f, node) || f(s.end_token) || Walk(*s.cond, f, node);
    case SK::If: {
        if (f(s.first) || Walk(*s.cond, f, node) || f(s.do_token) || Walk(*s.block, f, node)) return true;
        for (const auto &ei : s.elseifs)
            if (f(ei.elseif_token) || Walk(ei.cond, f, node) || f(ei.then_token) || Walk(ei.block, f, node)) return true;
        if (s.else_token && (f(*s.else_token) || Walk(*s.else_block, f, node))) return true;
        return f(s.end_token);
    }
    case SK::NumericFor:
        if (f(s.first) || f(s.index_var) || f(*s.equal) || Walk(*s.cond, f, node) || f(s.comma1) || Walk(*s.limit, f, node))
            return true;
        if (s.comma2 && (f(*s.comma2) || Walk(*s.step, f, node))) return true;
        return f(s.do_token) || Walk(*s.block, f, node) || f(s.end_token);
    case SK::GenericFor:
        return f(s.first) || Walk(s.names, f, node) || f(s.in_token) || Walk(s.exprs, f, node) || f(s.do_token) ||
               Walk(*s.block, f, node) || f(s.end_token);
    case SK::FunctionDecl:
        return f(s.first) || Walk(s.fname, f, node) || Walk(*s.body, f, node);
    case SK::LocalFunction:
        return f(s.local_token) || f(s.function_token) || f(s.name) || Walk(*s.body, f, node);
    case SK::Goto:
        return f(s.first) || f(s.name);
    case SK::Label:
        return f(s.first) || f(s.name) || f(s.end_token);
    }
    return false;
}

template <class F>
bool Walk(const Block &b, F &f, bool node) {
    for (const auto &ss : b.stmts) {
        if (Walk(ss.stmt, f, node)) return true;
        if (ss.semi && f(*ss.semi)) return true;
    }
    if (b.last) {
        if (Walk(b.last->first, f, node)) return true;
        if (b.last->second && f(*b.last->second)) return true;
    }
    return false;
}

// ---- first / last tokens ----------------------------------------------------

TokenRef *FirstTok(Expr &e);
TokenRef *LastTok(Expr &e);
TokenRef *FirstTok(Prefix &p);
TokenRef *LastTok(Prefix &p);
TokenRef *FirstTok(Suffix &s);
TokenRef *LastTok(Suffix &s);
TokenRef *FirstTok(FunctionArgs &a);
TokenRef *LastTok(FunctionArgs &a);
TokenRef *FirstTok(FunctionCall &c);
TokenRef *LastTok(FunctionCall &c);
TokenRef *FirstTok(Var &v);
TokenRef *LastTok(Var &v);
TokenRef *FirstTok(Field &f);
TokenRef *LastTok(Field &f);
TokenRef *FirstTok(TableConstructor &t);
TokenRef *LastTok(TableConstructor &t);
TokenRef *FirstTok(Stmt &s);
TokenRef *LastTok(Stmt &s);
TokenRef *FirstTok(LastStmt &s);
TokenRef *LastTok(LastStmt &s);
TokenRef *FirstTok(FunctionBody &b);
TokenRef *LastTok(FunctionBody &b);
TokenRef *FirstTok(Attribute &a);
TokenRef *LastTok(Attribute &a);
TokenRef *FirstTok(FunctionName &n);
TokenRef *LastTok(FunctionName &n);
inline TokenRef *FirstTok(TokenRef &t) { return &t; }
inline TokenRef *LastTok(TokenRef &t) { return &t; }
inline TokenRef *FirstTok(ContainedSpan &c) { return &c.open; }
inline TokenRef *LastTok(ContainedSpan &c) { return &c.close; }
template <class T>
TokenRef *FirstTok(Punctuated<T> &p) {
    return p.pairs.empty() ? nullptr : FirstTok(p.pairs.front().value);
}
template <class T>
TokenRef *LastTok(Punctuated<T> &p) {
    return p.pairs.empty() ? nullptr : LastTok(p.pairs.back().value);
}

}  // namespace luafmt::cst
