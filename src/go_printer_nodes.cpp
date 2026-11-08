// go/printer's nodes.go: printing of expressions, statements,
// declarations and files over the machinery in go_printer.cpp.
// Copyright 2009 The Go Authors. All rights reserved. BSD-style license;
// the full notice is in go_parse.cpp.

#include <cmath>

#include "go_parse.h"
#include "go_printer_impl.h"

namespace goprint {

using namespace goast;

namespace {

constexpr unsigned kCommaTerm = 1;
constexpr unsigned kNoIndent = 2;

constexpr int kFuncParam = 0;
constexpr int kFuncTParam = 1;
constexpr int kTypeTParam = 2;

const char *const kFilteredMsg = "contains filtered or unexported fields";

int IdentListSize(const std::vector<Ident *> &list, int max_size) {
    int size = 0;
    for (size_t i = 0; i < list.size(); i++) {
        if (i > 0) size += 2;
        size += RuneCount(list[i]->Name);
        if (size >= max_size) break;
    }
    return size;
}

Node *StripParensAlways(Node *x) {
    while (auto *p = As<ParenExpr>(x)) x = p->X;
    return x;
}

bool IsTypeElem(Node *x) {
    switch (x->kind) {
    case K::ArrayType: case K::StructType: case K::FuncType: case K::InterfaceType: case K::MapType:
    case K::ChanType:
        return true;
    case K::UnaryExpr: return static_cast<UnaryExpr *>(x)->Op == Tok::TILDE;
    case K::BinaryExpr: {
        auto *b = static_cast<goast::BinaryExpr *>(x);
        return IsTypeElem(b->X) || IsTypeElem(b->Y);
    }
    case K::ParenExpr: return IsTypeElem(static_cast<ParenExpr *>(x)->X);
    default: return false;
    }
}

bool CombinesWithName(Node *x) {
    switch (x->kind) {
    case K::StarExpr: return !IsTypeElem(static_cast<StarExpr *>(x)->X);
    case K::BinaryExpr: {
        auto *b = static_cast<goast::BinaryExpr *>(x);
        return CombinesWithName(b->X) && !IsTypeElem(b->Y);
    }
    case K::ParenExpr: return !IsTypeElem(static_cast<ParenExpr *>(x)->X);
    default: return false;
    }
}

void WalkBinary(goast::BinaryExpr *e, bool *has4, bool *has5, int *max_problem) {
    *has4 = *has5 = false;
    *max_problem = 0;
    switch (Precedence(e->Op)) {
    case 4: *has4 = true; break;
    case 5: *has5 = true; break;
    default: break;
    }
    if (auto *l = As<goast::BinaryExpr>(e->X)) {
        if (!(Precedence(l->Op) < Precedence(e->Op))) {
            bool h4, h5;
            int mp;
            WalkBinary(l, &h4, &h5, &mp);
            *has4 = *has4 || h4;
            *has5 = *has5 || h5;
            *max_problem = std::max(*max_problem, mp);
        }
    }
    if (auto *r = As<goast::BinaryExpr>(e->Y)) {
        if (!(Precedence(r->Op) <= Precedence(e->Op))) {
            bool h4, h5;
            int mp;
            WalkBinary(r, &h4, &h5, &mp);
            *has4 = *has4 || h4;
            *has5 = *has5 || h5;
            *max_problem = std::max(*max_problem, mp);
        }
    } else if (e->Y->kind == K::StarExpr) {
        if (e->Op == Tok::QUO) *max_problem = 5;
    } else if (auto *u = As<UnaryExpr>(e->Y)) {
        const std::string s = std::string(TokString(e->Op)) + TokString(u->Op);
        if (s == "/*" || s == "&&" || s == "&^") *max_problem = 5;
        else if (s == "++" || s == "--") *max_problem = std::max(*max_problem, 4);
    }
}

int Cutoff(goast::BinaryExpr *e, int depth) {
    bool has4, has5;
    int max_problem;
    WalkBinary(e, &has4, &has5, &max_problem);
    if (max_problem > 0) return max_problem + 1;
    if (has4 && has5) return depth == 1 ? 5 : 4;
    return depth == 1 ? 6 : 4;
}

int DiffPrec(Node *expr, int prec) {
    auto *x = As<goast::BinaryExpr>(expr);
    if (!x || prec != Precedence(x->Op)) return 1;
    return 0;
}

int ReduceDepth(int depth) {
    depth--;
    return depth < 1 ? 1 : depth;
}

bool IsTypeName(Node *x) {
    if (!x) return false;
    if (x->kind == K::Ident) return true;
    if (auto *s = As<goast::SelectorExpr>(x)) return IsTypeName(s->X);
    return false;
}

template <class F> void Inspect(Node *n, F &f) {
    if (!n || !f(n)) return;
    ForEachChild(n, [&](Node *c) { Inspect(c, f); });
}

// normalizedNumber; returns false when lit is already canonical.
bool NormalizedNumber(const BasicLit *lit, std::string *out) {
    if (lit->Kind != Tok::INT && lit->Kind != Tok::FLOAT && lit->Kind != Tok::IMAG) return false;
    if (lit->Value.size() < 2) return false;
    std::string x = lit->Value;
    const std::string p = x.substr(0, 2);
    if (p == "0X") {
        x = "0x" + x.substr(2);
        if (size_t i = x.rfind('P'); i != std::string::npos) x[i] = 'p';
    } else if (p == "0x") {
        size_t i = x.rfind('P');
        if (i == std::string::npos) return false;
        x[i] = 'p';
    } else if (p == "0O") {
        x = "0o" + x.substr(2);
    } else if (p == "0o") {
        return false;
    } else if (p == "0B") {
        x = "0b" + x.substr(2);
    } else if (p == "0b") {
        return false;
    } else {
        if (size_t i = x.rfind('E'); i != std::string::npos) {
            x[i] = 'e';
        } else if (x.back() == 'i' && x.find_first_of(".e") == std::string::npos) {
            size_t k = x.find_first_not_of("0_");
            x = k == std::string::npos ? std::string() : x.substr(k);
            if (x == "i") x = "0i";
        }
    }
    *out = x;
    return true;
}

std::vector<bool> KeepTypeColumn(const std::vector<Node *> &specs) {
    std::vector<bool> m(specs.size(), false);
    auto populate = [&](size_t i, size_t j, bool keep) {
        if (keep)
            for (; i < j; i++) m[i] = true;
    };
    long i0 = -1;
    bool keep_type = false;
    for (size_t i = 0; i < specs.size(); i++) {
        auto *t = static_cast<goast::ValueSpec *>(specs[i]);
        if (!t->Values.empty()) {
            if (i0 < 0) {
                i0 = static_cast<long>(i);
                keep_type = false;
            }
        } else if (i0 >= 0) {
            populate(static_cast<size_t>(i0), i, keep_type);
            i0 = -1;
        }
        if (t->Type) keep_type = true;
    }
    if (i0 >= 0) populate(static_cast<size_t>(i0), specs.size(), keep_type);
    return m;
}

Tok DeclToken(Node *d) {
    if (auto *g = As<goast::GenDecl>(d)) return g->Tok_;
    if (d->kind == K::FuncDecl) return Tok::FUNC;
    return Tok::ILLEGAL;
}

CommentGroup *GetDoc(Node *n) {
    switch (n->kind) {
    case K::Field: return static_cast<Field *>(n)->Doc;
    case K::ImportSpec: return static_cast<ImportSpec *>(n)->Doc;
    case K::ValueSpec: return static_cast<goast::ValueSpec *>(n)->Doc;
    case K::TypeSpec: return static_cast<TypeSpec *>(n)->Doc;
    case K::GenDecl: return static_cast<goast::GenDecl *>(n)->Doc;
    case K::FuncDecl: return static_cast<goast::FuncDecl *>(n)->Doc;
    case K::File: return static_cast<File *>(n)->Doc;
    default: return nullptr;
    }
}

}  // namespace

int Printer::Linebreak(int line, int min, WS ws, bool new_section) {
    int n = std::max(std::min(line - pos_.line, kMaxNewlines), min);
    int nbreaks = 0;
    if (n > 0) {
        Print(ws);
        if (new_section) {
            Print(WS::formfeed);
            n--;
            nbreaks = 2;
        }
        nbreaks += n;
        for (; n > 0; n--) Print(WS::newline);
    }
    return nbreaks;
}

void Printer::SetComment(CommentGroup *g) {
    if (!g || !use_node_comments_) return;
    if (comments_.empty()) {
        comments_.resize(1);
    } else if (ci_.cindex < comments_.size()) {
        Flush(PosFor(g->List[0]->Slash), Tok::ILLEGAL);
        comments_.resize(1);
    }
    comments_[0] = g;
    ci_.cindex = 0;
    if (ci_.comment_offset == kInfinity) NextComment();
}

void Printer::IdentList(const std::vector<Ident *> &list, bool indent) {
    std::vector<Node *> xlist(list.begin(), list.end());
    ExprList(NoPos, xlist, 1, indent ? 0 : kNoIndent, NoPos, false);
}

void Printer::ExprList(Pos prev0, const std::vector<Node *> &list, int depth, unsigned mode, Pos next0,
                       bool is_incomplete) {
    if (list.empty()) {
        if (is_incomplete) {
            const Position prev = PosFor(prev0), next = PosFor(next0);
            if (prev.IsValid() && prev.line == next.line) {
                Print(std::string("/* ") + kFilteredMsg + " */");
            } else {
                Print(WS::newline);
                Print(WS::indent, std::string("// ") + kFilteredMsg, WS::unindent, WS::newline);
            }
        }
        return;
    }
    const Position prev = PosFor(prev0), next = PosFor(next0);
    int line = LineFor(NodePos(list[0]));
    const int end_line = LineFor(NodeEnd(list.back()));
    if (prev.IsValid() && prev.line == line && line == end_line) {
        for (size_t i = 0; i < list.size(); i++) {
            if (i > 0) {
                SetPos(NodePos(list[i]));
                Print(Tok::COMMA, WS::blank);
            }
            Expr0(list[i], depth);
        }
        if (is_incomplete) Print(Tok::COMMA, WS::blank, std::string("/* ") + kFilteredMsg + " */");
        return;
    }
    WS ws = WS::ignore;
    if ((mode & kNoIndent) == 0) ws = WS::indent;
    long prev_break = -1;
    if (prev.IsValid() && prev.line < line && Linebreak(line, 0, ws, true) > 0) {
        ws = WS::ignore;
        prev_break = 0;
    }
    int size = 0;
    double lnsum = 0.0;
    int count = 0;
    int prev_line = prev.line;
    for (size_t i = 0; i < list.size(); i++) {
        Node *x = list[i];
        line = LineFor(NodePos(x));
        bool use_ff = true;
        const int prev_size = size;
        constexpr int kInf = 1000000;
        size = NodeSize(x, kInf);
        auto *pair = As<KeyValueExpr>(x);
        if (size <= kInf && prev.IsValid() && next.IsValid()) {
            if (pair) size = NodeSize(pair->Key, kInf);
        } else {
            size = 0;
        }
        if (prev_size > 0 && size > 0) {
            constexpr int kSmallSize = 40;
            if (count == 0 || (prev_size <= kSmallSize && size <= kSmallSize)) {
                use_ff = false;
            } else {
                constexpr double r = 2.5;
                const double geomean = std::exp(lnsum / static_cast<double>(count));
                const double ratio = static_cast<double>(size) / geomean;
                use_ff = r * ratio <= 1 || r <= ratio;
            }
        }
        const bool needs_linebreak = 0 < prev_line && prev_line < line;
        if (i > 0) {
            if (!needs_linebreak) SetPos(NodePos(x));
            Print(Tok::COMMA);
            bool needs_blank = true;
            if (needs_linebreak) {
                const int nbreaks = Linebreak(line, 0, ws, use_ff || prev_break + 1 < static_cast<long>(i));
                if (nbreaks > 0) {
                    ws = WS::ignore;
                    prev_break = static_cast<long>(i);
                    needs_blank = false;
                }
                if (nbreaks > 1) {
                    lnsum = 0;
                    count = 0;
                }
            }
            if (needs_blank) Print(WS::blank);
        }
        if (list.size() > 1 && pair && size > 0 && needs_linebreak) {
            Expr(pair->Key);
            SetPos(pair->Colon);
            Print(Tok::COLON, WS::vtab);
            Expr(pair->Value);
        } else {
            Expr0(x, depth);
        }
        if (size > 0) {
            lnsum += std::log(static_cast<double>(size));
            count++;
        }
        prev_line = line;
    }
    if ((mode & kCommaTerm) && next.IsValid() && pos_.line < next.line) {
        Print(Tok::COMMA);
        if (is_incomplete) {
            Print(WS::newline);
            Print(std::string("// ") + kFilteredMsg);
        }
        if (ws == WS::ignore && (mode & kNoIndent) == 0) Print(WS::unindent);
        Print(WS::formfeed);
        return;
    }
    if (is_incomplete) {
        Print(Tok::COMMA, WS::newline);
        Print(std::string("// ") + kFilteredMsg, WS::newline);
    }
    if (ws == WS::ignore && (mode & kNoIndent) == 0) Print(WS::unindent);
}

void Printer::Parameters(FieldList *fields, int mode) {
    Tok open_tok = Tok::LPAREN, close_tok = Tok::RPAREN;
    if (mode != kFuncParam) {
        open_tok = Tok::LBRACK;
        close_tok = Tok::RBRACK;
    }
    SetPos(fields->Opening);
    Print(open_tok);
    if (!fields->List.empty()) {
        int prev_line = LineFor(fields->Opening);
        WS ws = WS::indent;
        for (size_t i = 0; i < fields->List.size(); i++) {
            Field *par = fields->List[i];
            const int par_line_beg = LineFor(NodePos(par));
            const int par_line_end = LineFor(NodeEnd(par));
            const bool needs_linebreak = 0 < prev_line && prev_line < par_line_beg;
            if (i > 0) {
                if (!needs_linebreak) SetPos(NodePos(par));
                Print(Tok::COMMA);
            }
            if (needs_linebreak && Linebreak(par_line_beg, 0, ws, true) > 0) {
                ws = WS::ignore;
            } else if (i > 0) {
                Print(WS::blank);
            }
            if (!par->Names.empty()) {
                IdentList(par->Names, ws == WS::indent);
                Print(WS::blank);
            }
            Expr(StripParensAlways(par->Type));
            prev_line = par_line_end;
        }
        if (const int closing = LineFor(fields->Closing); 0 < prev_line && prev_line < closing) {
            Print(Tok::COMMA);
            Linebreak(closing, 0, WS::ignore, true);
        } else if (mode == kTypeTParam && NumFields(fields) == 1 &&
                   CombinesWithName(StripParensAlways(fields->List[0]->Type))) {
            Print(Tok::COMMA);
        }
        if (ws == WS::ignore) Print(WS::unindent);
    }
    SetPos(fields->Closing);
    Print(close_tok);
}

void Printer::Signature(FuncType *sig) {
    if (sig->TypeParams) Parameters(sig->TypeParams, kFuncTParam);
    if (sig->Params) {
        Parameters(sig->Params, kFuncParam);
    } else {
        Print(Tok::LPAREN, Tok::RPAREN);
    }
    FieldList *res = sig->Results;
    const int n = NumFields(res);
    if (n > 0) {
        Print(WS::blank);
        if (n == 1 && res->List[0]->Names.empty()) {
            Expr(StripParensAlways(res->List[0]->Type));
            return;
        }
        Parameters(res, kFuncParam);
    }
}

bool Printer::IsOneLineFieldList(const std::vector<Field *> &list) {
    if (list.size() != 1) return false;
    Field *f = list[0];
    if (f->Tag || f->Comment) return false;
    constexpr int kMaxSize = 30;
    int names_size = IdentListSize(f->Names, kMaxSize);
    if (names_size > 0) names_size = 1;
    const int type_size = NodeSize(f->Type, kMaxSize);
    return names_size + type_size <= kMaxSize;
}

void Printer::SetLineComment(const std::string &text) {
    auto *g = scratch_.New<CommentGroup>();
    auto *c = scratch_.New<Comment>();
    c->Slash = NoPos;
    c->Text = text;
    g->List.push_back(c);
    SetComment(g);
}

void Printer::FieldListNode(FieldList *fields, bool is_struct, bool is_incomplete) {
    const Pos lbrace = fields->Opening;
    const std::vector<Field *> &list = fields->List;
    const Pos rbrace = fields->Closing;
    const bool has_comments = is_incomplete || CommentBefore(PosFor(rbrace));
    const bool src_is_one_line = IsValid(lbrace) && IsValid(rbrace) && LineFor(lbrace) == LineFor(rbrace);
    if (!has_comments && src_is_one_line) {
        if (list.empty()) {
            SetPos(lbrace);
            Print(Tok::LBRACE);
            SetPos(rbrace);
            Print(Tok::RBRACE);
            return;
        } else if (IsOneLineFieldList(list)) {
            SetPos(lbrace);
            Print(Tok::LBRACE, WS::blank);
            Field *f = list[0];
            if (is_struct) {
                for (size_t i = 0; i < f->Names.size(); i++) {
                    if (i > 0) Print(Tok::COMMA, WS::blank);
                    Expr(f->Names[i]);
                }
                if (!f->Names.empty()) Print(WS::blank);
                Expr(f->Type);
            } else {
                if (!f->Names.empty()) {
                    Expr(f->Names[0]);
                    Signature(static_cast<FuncType *>(f->Type));
                } else {
                    Expr(f->Type);
                }
            }
            Print(WS::blank);
            SetPos(rbrace);
            Print(Tok::RBRACE);
            return;
        }
    }
    Print(WS::blank);
    SetPos(lbrace);
    Print(Tok::LBRACE, WS::indent);
    if (has_comments || !list.empty()) Print(WS::formfeed);
    if (is_struct) {
        const WS sep = list.size() == 1 ? WS::blank : WS::vtab;
        int line = 0;
        for (size_t i = 0; i < list.size(); i++) {
            Field *f = list[i];
            if (i > 0) Linebreak(LineFor(NodePos(f)), 1, WS::ignore, LinesFrom(line) > 0);
            int extra_tabs = 0;
            SetComment(f->Doc);
            RecordLine(&line);
            if (!f->Names.empty()) {
                IdentList(f->Names, false);
                Print(sep);
                Expr(f->Type);
                extra_tabs = 1;
            } else {
                Expr(f->Type);
                extra_tabs = 2;
            }
            if (f->Tag) {
                if (!f->Names.empty() && sep == WS::vtab) Print(sep);
                Print(sep);
                Expr(f->Tag);
                extra_tabs = 0;
            }
            if (f->Comment) {
                for (; extra_tabs > 0; extra_tabs--) Print(sep);
                SetComment(f->Comment);
            }
        }
        if (is_incomplete) {
            if (!list.empty()) Print(WS::formfeed);
            Flush(PosFor(rbrace), Tok::RBRACE);
            SetLineComment(std::string("// ") + kFilteredMsg);
        }
    } else {
        int line = 0;
        for (size_t i = 0; i < list.size(); i++) {
            Field *f = list[i];
            Ident *name = f->Names.empty() ? nullptr : f->Names[0];
            if (i > 0) Linebreak(LineFor(NodePos(f)), 1, WS::ignore, LinesFrom(line) > 0);
            SetComment(f->Doc);
            RecordLine(&line);
            if (name) {
                Expr(name);
                Signature(static_cast<FuncType *>(f->Type));
            } else {
                Expr(f->Type);
            }
            SetComment(f->Comment);
        }
        if (is_incomplete) {
            if (!list.empty()) Print(WS::formfeed);
            Flush(PosFor(rbrace), Tok::RBRACE);
            SetLineComment("// contains filtered or unexported methods");
        }
    }
    Print(WS::unindent, WS::formfeed);
    SetPos(rbrace);
    Print(Tok::RBRACE);
}

void Printer::BinaryExpr(goast::BinaryExpr *x, int prec1, int cutoff, int depth) {
    const int prec = Precedence(x->Op);
    if (prec < prec1) {
        Print(Tok::LPAREN);
        Expr0(x, ReduceDepth(depth));
        Print(Tok::RPAREN);
        return;
    }
    bool print_blank = prec < cutoff;
    WS ws = WS::indent;
    Expr1(x->X, prec, depth + DiffPrec(x->X, prec));
    if (print_blank) Print(WS::blank);
    const int xline = pos_.line;
    const int yline = LineFor(NodePos(x->Y));
    SetPos(x->OpPos);
    Print(x->Op);
    if (xline != yline && xline > 0 && yline > 0) {
        if (Linebreak(yline, 1, ws, true) > 0) {
            ws = WS::ignore;
            print_blank = false;
        }
    }
    if (print_blank) Print(WS::blank);
    Expr1(x->Y, prec + 1, depth + 1);
    if (ws == WS::ignore) Print(WS::unindent);
}

void Printer::Expr1(Node *expr, int prec1, int depth) {
    DepthGuard guard(this);
    SetPos(NodePos(expr));
    switch (expr->kind) {
    case K::BadExpr: Print(std::string_view("BadExpr")); break;
    case K::Ident: Print(static_cast<const Ident *>(expr)); break;
    case K::BinaryExpr: {
        auto *x = static_cast<goast::BinaryExpr *>(expr);
        if (depth < 1) depth = 1;
        BinaryExpr(x, prec1, Cutoff(x, depth), depth);
        break;
    }
    case K::KeyValueExpr: {
        auto *x = static_cast<KeyValueExpr *>(expr);
        Expr(x->Key);
        SetPos(x->Colon);
        Print(Tok::COLON, WS::blank);
        Expr(x->Value);
        break;
    }
    case K::StarExpr: {
        auto *x = static_cast<StarExpr *>(expr);
        if (UnaryPrec < prec1) {
            Print(Tok::LPAREN);
            Print(Tok::MUL);
            Expr(x->X);
            Print(Tok::RPAREN);
        } else {
            Print(Tok::MUL);
            Expr(x->X);
        }
        break;
    }
    case K::UnaryExpr: {
        auto *x = static_cast<UnaryExpr *>(expr);
        if (UnaryPrec < prec1) {
            Print(Tok::LPAREN);
            Expr(x);
            Print(Tok::RPAREN);
        } else {
            Print(x->Op);
            if (x->Op == Tok::RANGE) Print(WS::blank);
            Expr1(x->X, UnaryPrec, depth);
        }
        break;
    }
    case K::BasicLit: {
        auto *x = static_cast<BasicLit *>(expr);
        std::string norm;
        if ((cfg_.mode & NormalizeNumbers) && NormalizedNumber(x, &norm)) {
            BasicLit tmp;
            tmp.ValuePos = x->ValuePos;
            tmp.Kind = x->Kind;
            tmp.Value = norm;
            Print(static_cast<const BasicLit *>(&tmp));
        } else {
            Print(static_cast<const BasicLit *>(x));
        }
        break;
    }
    case K::FuncLit: {
        auto *x = static_cast<FuncLit *>(expr);
        SetPos(NodePos(x->Type));
        Print(Tok::FUNC);
        const int start_col = out_.column - 4;
        Signature(x->Type);
        FuncBody(DistanceFrom(NodePos(x->Type), start_col), WS::blank, x->Body);
        break;
    }
    case K::ParenExpr: {
        auto *x = static_cast<ParenExpr *>(expr);
        if (x->X->kind == K::ParenExpr) {
            Expr0(x->X, depth);
        } else {
            Print(Tok::LPAREN);
            Expr0(x->X, ReduceDepth(depth));
            SetPos(x->Rparen);
            Print(Tok::RPAREN);
        }
        break;
    }
    case K::SelectorExpr: SelectorExpr(static_cast<goast::SelectorExpr *>(expr), depth, false); break;
    case K::TypeAssertExpr: {
        auto *x = static_cast<TypeAssertExpr *>(expr);
        Expr1(x->X, HighestPrec, depth);
        Print(Tok::PERIOD);
        SetPos(x->Lparen);
        Print(Tok::LPAREN);
        if (x->Type) {
            Expr(x->Type);
        } else {
            Print(Tok::TYPE);
        }
        SetPos(x->Rparen);
        Print(Tok::RPAREN);
        break;
    }
    case K::IndexExpr: {
        auto *x = static_cast<IndexExpr *>(expr);
        Expr1(x->X, HighestPrec, 1);
        SetPos(x->Lbrack);
        Print(Tok::LBRACK);
        Expr0(x->Index, depth + 1);
        SetPos(x->Rbrack);
        Print(Tok::RBRACK);
        break;
    }
    case K::IndexListExpr: {
        auto *x = static_cast<IndexListExpr *>(expr);
        Expr1(x->X, HighestPrec, 1);
        SetPos(x->Lbrack);
        Print(Tok::LBRACK);
        ExprList(x->Lbrack, x->Indices, depth + 1, kCommaTerm, x->Rbrack, false);
        SetPos(x->Rbrack);
        Print(Tok::RBRACK);
        break;
    }
    case K::SliceExpr: {
        auto *x = static_cast<SliceExpr *>(expr);
        Expr1(x->X, HighestPrec, 1);
        SetPos(x->Lbrack);
        Print(Tok::LBRACK);
        Node *indices[3] = {x->Low, x->High, x->Max};
        const size_t nidx = x->Max ? 3 : 2;
        bool needs_blanks = false;
        if (depth <= 1) {
            int index_count = 0;
            bool has_binaries = false;
            for (size_t i = 0; i < nidx; i++) {
                if (indices[i]) {
                    index_count++;
                    if (indices[i]->kind == K::BinaryExpr) has_binaries = true;
                }
            }
            if (index_count > 1 && has_binaries) needs_blanks = true;
        }
        for (size_t i = 0; i < nidx; i++) {
            if (i > 0) {
                if (indices[i - 1] && needs_blanks) Print(WS::blank);
                Print(Tok::COLON);
                if (indices[i] && needs_blanks) Print(WS::blank);
            }
            if (indices[i]) Expr0(indices[i], depth + 1);
        }
        SetPos(x->Rbrack);
        Print(Tok::RBRACK);
        break;
    }
    case K::CallExpr: {
        auto *x = static_cast<CallExpr *>(expr);
        if (x->Args.size() > 1) depth++;
        bool paren = false;
        if (x->Fun->kind == K::FuncType) paren = true;
        else if (auto *t = As<ChanType>(x->Fun)) paren = t->Dir == RECV;
        if (paren) Print(Tok::LPAREN);
        const bool was_indented = PossibleSelectorExpr(x->Fun, HighestPrec, depth);
        if (paren) Print(Tok::RPAREN);
        SetPos(x->Lparen);
        Print(Tok::LPAREN);
        if (IsValid(x->Ellipsis)) {
            ExprList(x->Lparen, x->Args, depth, 0, x->Ellipsis, false);
            SetPos(x->Ellipsis);
            Print(Tok::ELLIPSIS);
            if (IsValid(x->Rparen) && LineFor(x->Ellipsis) < LineFor(x->Rparen)) Print(Tok::COMMA, WS::formfeed);
        } else {
            ExprList(x->Lparen, x->Args, depth, kCommaTerm, x->Rparen, false);
        }
        SetPos(x->Rparen);
        Print(Tok::RPAREN);
        if (was_indented) Print(WS::unindent);
        break;
    }
    case K::CompositeLit: {
        auto *x = static_cast<CompositeLit *>(expr);
        if (x->Type) Expr1(x->Type, HighestPrec, depth);
        level_++;
        SetPos(x->Lbrace);
        Print(Tok::LBRACE);
        ExprList(x->Lbrace, x->Elts, 1, kCommaTerm, x->Rbrace, x->Incomplete);
        int mode = kNoExtraLinebreak;
        if (!x->Elts.empty()) mode |= kNoExtraBlank;
        Print(WS::indent, WS::unindent, static_cast<PM>(mode));
        SetPos(x->Rbrace);
        Print(Tok::RBRACE, static_cast<PM>(mode));
        level_--;
        break;
    }
    case K::Ellipsis: {
        auto *x = static_cast<goast::Ellipsis *>(expr);
        Print(Tok::ELLIPSIS);
        if (x->Elt) Expr(x->Elt);
        break;
    }
    case K::ArrayType: {
        auto *x = static_cast<ArrayType *>(expr);
        Print(Tok::LBRACK);
        if (x->Len) Expr(x->Len);
        Print(Tok::RBRACK);
        Expr(x->Elt);
        break;
    }
    case K::StructType: {
        auto *x = static_cast<StructType *>(expr);
        Print(Tok::STRUCT);
        FieldListNode(x->Fields, true, x->Incomplete);
        break;
    }
    case K::FuncType:
        Print(Tok::FUNC);
        Signature(static_cast<FuncType *>(expr));
        break;
    case K::InterfaceType: {
        auto *x = static_cast<InterfaceType *>(expr);
        Print(Tok::INTERFACE);
        FieldListNode(x->Methods, false, x->Incomplete);
        break;
    }
    case K::MapType: {
        auto *x = static_cast<MapType *>(expr);
        Print(Tok::MAP, Tok::LBRACK);
        Expr(x->Key);
        Print(Tok::RBRACK);
        Expr(x->Value);
        break;
    }
    case K::ChanType: {
        auto *x = static_cast<ChanType *>(expr);
        switch (x->Dir) {
        case SEND | RECV: Print(Tok::CHAN); break;
        case RECV: Print(Tok::ARROW, Tok::CHAN); break;
        case SEND:
            Print(Tok::CHAN);
            SetPos(x->Arrow);
            Print(Tok::ARROW);
            break;
        default: break;
        }
        Print(WS::blank);
        Expr(x->Value);
        break;
    }
    default: break;
    }
}

bool Printer::PossibleSelectorExpr(Node *expr, int prec1, int depth) {
    if (auto *x = As<goast::SelectorExpr>(expr)) return SelectorExpr(x, depth, true);
    Expr1(expr, prec1, depth);
    return false;
}

bool Printer::SelectorExpr(goast::SelectorExpr *x, int depth, bool is_method) {
    Expr1(x->X, HighestPrec, depth);
    Print(Tok::PERIOD);
    if (const int line = LineFor(NodePos(x->Sel)); pos_.IsValid() && pos_.line < line) {
        Print(WS::indent, WS::newline);
        SetPos(NodePos(x->Sel));
        Print(static_cast<const Ident *>(x->Sel));
        if (!is_method) Print(WS::unindent);
        return true;
    }
    SetPos(NodePos(x->Sel));
    Print(static_cast<const Ident *>(x->Sel));
    return false;
}

void Printer::StmtList(const std::vector<Node *> &list, int nindent, bool next_is_rbrace) {
    if (nindent > 0) Print(WS::indent);
    int line = 0;
    size_t i = 0;
    for (Node *s : list) {
        if (s->kind == K::EmptyStmt) continue;
        if (!output_.empty()) {
            Linebreak(LineFor(NodePos(s)), 1, WS::ignore, i == 0 || nindent == 0 || LinesFrom(line) > 0);
        }
        RecordLine(&line);
        Stmt(s, next_is_rbrace && i == list.size() - 1);
        for (Node *t = s;;) {
            auto *lt = As<LabeledStmt>(t);
            if (!lt) break;
            line++;
            t = lt->Stmt;
        }
        i++;
    }
    if (nindent > 0) Print(WS::unindent);
}

void Printer::Block(BlockStmt *b, int nindent) {
    SetPos(b->Lbrace);
    Print(Tok::LBRACE);
    StmtList(b->List, nindent, true);
    Linebreak(LineFor(b->Rbrace), 1, WS::ignore, true);
    SetPos(b->Rbrace);
    Print(Tok::RBRACE);
}

Node *Printer::StripParens(Node *x) {
    if (auto *px = As<ParenExpr>(x)) {
        bool strip = true;
        auto visit = [&](Node *node) {
            if (node->kind == K::ParenExpr) return false;
            if (auto *cl = As<CompositeLit>(node)) {
                if (IsTypeName(cl->Type)) strip = false;
                return false;
            }
            return true;
        };
        Inspect(px->X, visit);
        if (strip) return StripParens(px->X);
    }
    return x;
}

void Printer::ControlClause(bool is_for, Node *init, Node *expr, Node *post) {
    Print(WS::blank);
    bool needs_blank = false;
    if (!init && !post) {
        if (expr) {
            Expr(StripParens(expr));
            needs_blank = true;
        }
    } else {
        if (init) Stmt(init, false);
        Print(Tok::SEMICOLON, WS::blank);
        if (expr) {
            Expr(StripParens(expr));
            needs_blank = true;
        }
        if (is_for) {
            Print(Tok::SEMICOLON, WS::blank);
            needs_blank = false;
            if (post) {
                Stmt(post, false);
                needs_blank = true;
            }
        }
    }
    if (needs_blank) Print(WS::blank);
}

bool Printer::IndentList(const std::vector<Node *> &list) {
    if (list.size() >= 2) {
        const int b = LineFor(NodePos(list[0]));
        const int e = LineFor(NodeEnd(list.back()));
        if (0 < b && b < e) {
            int n = 0;
            int line = b;
            for (Node *x : list) {
                const int xb = LineFor(NodePos(x));
                const int xe = LineFor(NodeEnd(x));
                if (line < xb) return true;
                if (xb < xe) n++;
                line = xe;
            }
            return n > 1;
        }
    }
    return false;
}

void Printer::Stmt(Node *stmt, bool next_is_rbrace) {
    DepthGuard guard(this);
    SetPos(NodePos(stmt));
    switch (stmt->kind) {
    case K::BadStmt: Print(std::string_view("BadStmt")); break;
    case K::DeclStmt: Decl(static_cast<DeclStmt *>(stmt)->Decl); break;
    case K::EmptyStmt: break;
    case K::LabeledStmt: {
        auto *s = static_cast<LabeledStmt *>(stmt);
        Print(WS::unindent);
        Expr(s->Label);
        SetPos(s->Colon);
        Print(Tok::COLON, WS::indent);
        if (auto *e = As<EmptyStmt>(s->Stmt)) {
            if (!next_is_rbrace) {
                Print(WS::newline);
                SetPos(NodePos(e));
                Print(Tok::SEMICOLON);
                break;
            }
        } else {
            Linebreak(LineFor(NodePos(s->Stmt)), 1, WS::ignore, true);
        }
        Stmt(s->Stmt, next_is_rbrace);
        break;
    }
    case K::ExprStmt: Expr0(static_cast<ExprStmt *>(stmt)->X, 1); break;
    case K::SendStmt: {
        auto *s = static_cast<SendStmt *>(stmt);
        Expr0(s->Chan, 1);
        Print(WS::blank);
        SetPos(s->Arrow);
        Print(Tok::ARROW, WS::blank);
        Expr0(s->Value, 1);
        break;
    }
    case K::IncDecStmt: {
        auto *s = static_cast<IncDecStmt *>(stmt);
        Expr0(s->X, 2);
        SetPos(s->TokPos);
        Print(s->Tok_);
        break;
    }
    case K::AssignStmt: {
        auto *s = static_cast<AssignStmt *>(stmt);
        int depth = 1;
        if (s->Lhs.size() > 1 && s->Rhs.size() > 1) depth++;
        ExprList(NodePos(s), s->Lhs, depth, 0, s->TokPos, false);
        Print(WS::blank);
        SetPos(s->TokPos);
        Print(s->Tok_, WS::blank);
        ExprList(s->TokPos, s->Rhs, depth, 0, NoPos, false);
        break;
    }
    case K::GoStmt:
        Print(Tok::GO, WS::blank);
        Expr(static_cast<GoStmt *>(stmt)->Call);
        break;
    case K::DeferStmt:
        Print(Tok::DEFER, WS::blank);
        Expr(static_cast<DeferStmt *>(stmt)->Call);
        break;
    case K::ReturnStmt: {
        auto *s = static_cast<ReturnStmt *>(stmt);
        Print(Tok::RETURN);
        if (!s->Results.empty()) {
            Print(WS::blank);
            if (IndentList(s->Results)) {
                Print(WS::indent);
                ExprList(NoPos, s->Results, 1, kNoIndent, NoPos, false);
                Print(WS::unindent);
            } else {
                ExprList(NoPos, s->Results, 1, 0, NoPos, false);
            }
        }
        break;
    }
    case K::BranchStmt: {
        auto *s = static_cast<BranchStmt *>(stmt);
        Print(s->Tok_);
        if (s->Label) {
            Print(WS::blank);
            Expr(s->Label);
        }
        break;
    }
    case K::BlockStmt: Block(static_cast<BlockStmt *>(stmt), 1); break;
    case K::IfStmt: {
        auto *s = static_cast<IfStmt *>(stmt);
        Print(Tok::IF);
        ControlClause(false, s->Init, s->Cond, nullptr);
        Block(s->Body, 1);
        if (s->Else) {
            Print(WS::blank, Tok::ELSE, WS::blank);
            if (s->Else->kind == K::BlockStmt || s->Else->kind == K::IfStmt) {
                Stmt(s->Else, next_is_rbrace);
            } else {
                Print(Tok::LBRACE, WS::indent, WS::formfeed);
                Stmt(s->Else, true);
                Print(WS::unindent, WS::formfeed, Tok::RBRACE);
            }
        }
        break;
    }
    case K::CaseClause: {
        auto *s = static_cast<CaseClause *>(stmt);
        if (!s->List.empty()) {
            Print(Tok::CASE, WS::blank);
            ExprList(NodePos(s), s->List, 1, 0, s->Colon, false);
        } else {
            Print(Tok::DEFAULT);
        }
        SetPos(s->Colon);
        Print(Tok::COLON);
        StmtList(s->Body, 1, next_is_rbrace);
        break;
    }
    case K::SwitchStmt: {
        auto *s = static_cast<SwitchStmt *>(stmt);
        Print(Tok::SWITCH);
        ControlClause(false, s->Init, s->Tag, nullptr);
        Block(s->Body, 0);
        break;
    }
    case K::TypeSwitchStmt: {
        auto *s = static_cast<TypeSwitchStmt *>(stmt);
        Print(Tok::SWITCH);
        if (s->Init) {
            Print(WS::blank);
            Stmt(s->Init, false);
            Print(Tok::SEMICOLON);
        }
        Print(WS::blank);
        Stmt(s->Assign, false);
        Print(WS::blank);
        Block(s->Body, 0);
        break;
    }
    case K::CommClause: {
        auto *s = static_cast<CommClause *>(stmt);
        if (s->Comm) {
            Print(Tok::CASE, WS::blank);
            Stmt(s->Comm, false);
        } else {
            Print(Tok::DEFAULT);
        }
        SetPos(s->Colon);
        Print(Tok::COLON);
        StmtList(s->Body, 1, next_is_rbrace);
        break;
    }
    case K::SelectStmt: {
        auto *s = static_cast<SelectStmt *>(stmt);
        Print(Tok::SELECT, WS::blank);
        BlockStmt *body = s->Body;
        if (body->List.empty() && !CommentBefore(PosFor(body->Rbrace))) {
            SetPos(body->Lbrace);
            Print(Tok::LBRACE);
            SetPos(body->Rbrace);
            Print(Tok::RBRACE);
        } else {
            Block(body, 0);
        }
        break;
    }
    case K::ForStmt: {
        auto *s = static_cast<ForStmt *>(stmt);
        Print(Tok::FOR);
        ControlClause(true, s->Init, s->Cond, s->Post);
        Block(s->Body, 1);
        break;
    }
    case K::RangeStmt: {
        auto *s = static_cast<RangeStmt *>(stmt);
        Print(Tok::FOR, WS::blank);
        if (s->Key) {
            Expr(s->Key);
            if (s->Value) {
                SetPos(NodePos(s->Value));
                Print(Tok::COMMA, WS::blank);
                Expr(s->Value);
            }
            Print(WS::blank);
            SetPos(s->TokPos);
            Print(s->Tok_, WS::blank);
        }
        Print(Tok::RANGE, WS::blank);
        Expr(StripParens(s->X));
        Print(WS::blank);
        Block(s->Body, 1);
        break;
    }
    default: break;
    }
}

void Printer::ValueSpec(goast::ValueSpec *s, bool keep_type) {
    SetComment(s->Doc);
    IdentList(s->Names, false);
    int extra_tabs = 3;
    if (s->Type || keep_type) {
        Print(WS::vtab);
        extra_tabs--;
    }
    if (s->Type) Expr(s->Type);
    if (!s->Values.empty()) {
        Print(WS::vtab, Tok::ASSIGN, WS::blank);
        ExprList(NoPos, s->Values, 1, 0, NoPos, false);
        extra_tabs--;
    }
    if (s->Comment) {
        for (; extra_tabs > 0; extra_tabs--) Print(WS::vtab);
        SetComment(s->Comment);
    }
}

BasicLit *Printer::SanitizeImportPath(BasicLit *lit) {
    if (lit->Kind != Tok::STRING) return lit;
    std::string s;
    if (!goparse::Unquote(lit->Value, &s)) return lit;
    if (s.empty()) return lit;
    static const char32_t kIllegal[] = U"!\"#$%&'()*,:;<=>?[\\]^{|}`�";
    size_t i = 0;
    while (i < s.size()) {
        int w = 0;
        const uint32_t r = DecodeRune(s, i, &w);
        // !IsGraphic(r) || IsSpace(r): the graphic runes that are not printable are
        // the Zs spaces, which IsSpace rejects anyway.
        if (!UnicodeIsPrint(r) || UnicodeIsSpace(r)) return lit;
        for (const char32_t *p = kIllegal; *p; p++)
            if (static_cast<uint32_t>(*p) == r) return lit;
        i += static_cast<size_t>(w);
    }
    s = "\"" + s + "\"";
    if (s == lit->Value) return lit;
    auto *n = scratch_.New<BasicLit>();
    n->ValuePos = lit->ValuePos;
    n->Kind = Tok::STRING;
    n->Value = s;
    return n;
}

void Printer::Spec(Node *spec, int n, bool do_indent) {
    switch (spec->kind) {
    case K::ImportSpec: {
        auto *s = static_cast<ImportSpec *>(spec);
        SetComment(s->Doc);
        if (s->Name) {
            Expr(s->Name);
            Print(WS::blank);
        }
        Expr(SanitizeImportPath(s->Path));
        SetComment(s->Comment);
        SetPos(s->EndPos);
        break;
    }
    case K::ValueSpec: {
        auto *s = static_cast<goast::ValueSpec *>(spec);
        SetComment(s->Doc);
        IdentList(s->Names, do_indent);
        if (s->Type) {
            Print(WS::blank);
            Expr(s->Type);
        }
        if (!s->Values.empty()) {
            Print(WS::blank, Tok::ASSIGN, WS::blank);
            ExprList(NoPos, s->Values, 1, 0, NoPos, false);
        }
        SetComment(s->Comment);
        break;
    }
    case K::TypeSpec: {
        auto *s = static_cast<TypeSpec *>(spec);
        SetComment(s->Doc);
        Expr(s->Name);
        if (s->TypeParams) Parameters(s->TypeParams, kTypeTParam);
        if (n == 1) Print(WS::blank);
        else Print(WS::vtab);
        if (IsValid(s->Assign)) Print(Tok::ASSIGN, WS::blank);
        Expr(s->Type);
        SetComment(s->Comment);
        break;
    }
    default: break;
    }
}

void Printer::GenDecl(goast::GenDecl *d) {
    SetComment(d->Doc);
    SetPos(NodePos(d));
    Print(d->Tok_, WS::blank);
    if (IsValid(d->Lparen) || d->Specs.size() != 1) {
        SetPos(d->Lparen);
        Print(Tok::LPAREN);
        if (const size_t n = d->Specs.size(); n > 0) {
            Print(WS::indent, WS::formfeed);
            if (n > 1 && (d->Tok_ == Tok::CONST || d->Tok_ == Tok::VAR)) {
                const std::vector<bool> keep_type = KeepTypeColumn(d->Specs);
                int line = 0;
                for (size_t i = 0; i < n; i++) {
                    Node *s = d->Specs[i];
                    if (i > 0) Linebreak(LineFor(NodePos(s)), 1, WS::ignore, LinesFrom(line) > 0);
                    RecordLine(&line);
                    ValueSpec(static_cast<goast::ValueSpec *>(s), keep_type[i]);
                }
            } else {
                int line = 0;
                for (size_t i = 0; i < n; i++) {
                    Node *s = d->Specs[i];
                    if (i > 0) Linebreak(LineFor(NodePos(s)), 1, WS::ignore, LinesFrom(line) > 0);
                    RecordLine(&line);
                    Spec(s, static_cast<int>(n), false);
                }
            }
            Print(WS::unindent, WS::formfeed);
        }
        SetPos(d->Rparen);
        Print(Tok::RPAREN);
    } else if (!d->Specs.empty()) {
        Spec(d->Specs[0], 1, true);
    }
}

int Printer::NumLines(Node *n) {
    if (const Pos from = NodePos(n); IsValid(from)) {
        if (const Pos to = NodeEnd(n); IsValid(to)) return LineFor(to) - LineFor(from) + 1;
    }
    return kInfinity;
}

int Printer::BodySize(BlockStmt *b, int max_size) {
    const Pos pos1 = NodePos(b);
    const Pos pos2 = b->Rbrace;
    if (IsValid(pos1) && IsValid(pos2) && LineFor(pos1) != LineFor(pos2)) return max_size + 1;
    if (b->List.size() > 5) return max_size + 1;
    int body_size = CommentSizeBefore(PosFor(pos2));
    for (size_t i = 0; i < b->List.size(); i++) {
        if (body_size > max_size) break;
        if (i > 0) body_size += 2;
        body_size += NodeSize(b->List[i], max_size);
    }
    return body_size;
}

void Printer::FuncBody(int header_size, WS sep, BlockStmt *b) {
    if (!b) return;
    const int saved_level = level_;
    level_ = 0;
    constexpr int kMaxSize = 100;
    if (header_size + BodySize(b, kMaxSize) <= kMaxSize) {
        Print(sep);
        SetPos(b->Lbrace);
        Print(Tok::LBRACE);
        if (!b->List.empty()) {
            Print(WS::blank);
            for (size_t i = 0; i < b->List.size(); i++) {
                if (i > 0) Print(Tok::SEMICOLON, WS::blank);
                Stmt(b->List[i], i == b->List.size() - 1);
            }
            Print(WS::blank);
        }
        Print(PM::noExtraLinebreak);
        SetPos(b->Rbrace);
        Print(Tok::RBRACE, PM::noExtraLinebreak);
        level_ = saved_level;
        return;
    }
    if (sep != WS::ignore) Print(WS::blank);
    Block(b, 1);
    level_ = saved_level;
}

int Printer::DistanceFrom(Pos start_pos, int start_out_col) {
    if (IsValid(start_pos) && pos_.IsValid() && PosFor(start_pos).line == pos_.line)
        return out_.column - start_out_col;
    return kInfinity;
}

void Printer::FuncDecl(goast::FuncDecl *d) {
    SetComment(d->Doc);
    SetPos(NodePos(d));
    Print(Tok::FUNC, WS::blank);
    const int start_col = out_.column - 5;
    if (d->Recv) {
        Parameters(d->Recv, kFuncParam);
        Print(WS::blank);
    }
    Expr(d->Name);
    Signature(d->Type);
    FuncBody(DistanceFrom(NodePos(d), start_col), WS::vtab, d->Body);
}

void Printer::Decl(Node *decl) {
    switch (decl->kind) {
    case K::BadDecl:
        SetPos(NodePos(decl));
        Print(std::string_view("BadDecl"));
        break;
    case K::GenDecl: GenDecl(static_cast<goast::GenDecl *>(decl)); break;
    case K::FuncDecl: FuncDecl(static_cast<goast::FuncDecl *>(decl)); break;
    default: break;
    }
}

void Printer::DeclList(const std::vector<Node *> &list) {
    Tok tok = Tok::ILLEGAL;
    for (Node *d : list) {
        const Tok prev = tok;
        tok = DeclToken(d);
        if (!output_.empty()) {
            int min = 1;
            if (prev != tok || GetDoc(d) != nullptr) min = 2;
            Linebreak(LineFor(NodePos(d)), min, WS::ignore, tok == Tok::FUNC && NumLines(d) > 1);
        }
        Decl(d);
    }
}

void Printer::FileNode(File *src) {
    SetComment(src->Doc);
    SetPos(NodePos(src));
    Print(Tok::PACKAGE, WS::blank);
    Expr(src->Name);
    DeclList(src->Decls);
    Print(WS::newline);
}

}  // namespace goprint
