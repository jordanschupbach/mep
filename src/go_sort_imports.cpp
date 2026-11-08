// go/ast's SortImports (import.go), which cmd/gofmt applies before
// printing. Copyright 2011 The Go Authors. All rights reserved.
// BSD-style license; the full notice is in go_parse.cpp.

#include <algorithm>
#include <map>

#include "go_parse.h"
#include "go_printer.h"

namespace goprint {

using namespace goast;

namespace {

std::string ImportPath(Node *s) {
    std::string t;
    if (goparse::Unquote(static_cast<ImportSpec *>(s)->Path->Value, &t)) return t;
    return "";
}

std::string ImportName(Node *s) {
    Ident *n = static_cast<ImportSpec *>(s)->Name;
    return n ? n->Name : "";
}

std::string ImportComment(Node *s) {
    CommentGroup *c = static_cast<ImportSpec *>(s)->Comment;
    return c ? CommentGroupText(c) : "";
}

bool Collapse(Node *prev, Node *next) {
    if (ImportPath(next) != ImportPath(prev) || ImportName(next) != ImportName(prev)) return false;
    return static_cast<ImportSpec *>(prev)->Comment == nullptr;
}

void UpdateBasicLitPos(BasicLit *lit, Pos pos) {
    const int len = NodeEnd(lit) - NodePos(lit);
    lit->ValuePos = pos;
    if (IsValid(lit->ValueEnd)) lit->ValueEnd = pos + len;
}

struct CgPos {
    bool left;
    CommentGroup *cg;
};

std::vector<Node *> SortSpecs(LineTable &lt, File *f, GenDecl *d, std::vector<Node *> specs) {
    if (specs.size() <= 1) return specs;
    struct Span {
        Pos start, end;
    };
    std::vector<Span> pos;
    for (Node *s : specs) pos.push_back({NodePos(s), NodeEnd(s)});

    const Pos beg_specs = pos[0].start;
    const Pos end_specs = pos.back().end;
    const Pos beg = lt.LineStart(lt.Line(beg_specs));
    const int end_line = lt.Line(end_specs);
    Pos end;
    if (end_line == lt.LineCount()) end = end_specs;
    else end = lt.LineStart(end_line + 1);
    size_t first = f->Comments.size();
    long last = -1;
    for (size_t i = 0; i < f->Comments.size(); i++) {
        CommentGroup *g = f->Comments[i];
        if (NodeEnd(g) >= end) break;
        if (beg <= NodePos(g)) {
            first = std::min(first, i);
            last = std::max(last, static_cast<long>(i));
        }
    }
    auto cbegin = f->Comments.begin(), cend = f->Comments.begin();
    if (last >= 0) {
        cbegin = f->Comments.begin() + static_cast<long>(first);
        cend = f->Comments.begin() + last + 1;
    }

    std::map<ImportSpec *, std::vector<CgPos>> import_comments;
    size_t spec_index = 0;
    for (auto it = cbegin; it != cend; ++it) {
        CommentGroup *g = *it;
        while (spec_index + 1 < specs.size() && pos[spec_index + 1].start <= NodePos(g)) spec_index++;
        bool left = false;
        if (spec_index == 0 && pos[spec_index].start > NodePos(g)) {
            left = true;
        } else if (spec_index + 1 < specs.size() && lt.Line(pos[spec_index].start) + 1 == lt.Line(NodePos(g))) {
            spec_index++;
            left = true;
        }
        import_comments[static_cast<ImportSpec *>(specs[spec_index])].push_back({left, g});
    }

    std::stable_sort(specs.begin(), specs.end(), [](Node *a, Node *b) {
        const std::string ip = ImportPath(a), jp = ImportPath(b);
        if (ip != jp) return ip < jp;
        const std::string in = ImportName(a), jn = ImportName(b);
        if (in != jn) return in < jn;
        return ImportComment(a) < ImportComment(b);
    });

    std::vector<Node *> deduped;
    for (size_t i = 0; i < specs.size(); i++) {
        Node *s = specs[i];
        if (i == specs.size() - 1 || !Collapse(s, specs[i + 1])) {
            deduped.push_back(s);
        } else {
            const Pos p = NodePos(s);
            if (const int l = lt.Line(p); l != lt.Line(d->Rparen)) lt.MergeLine(l);
        }
    }
    specs = deduped;

    for (size_t i = 0; i < specs.size(); i++) {
        auto *s = static_cast<ImportSpec *>(specs[i]);
        if (s->Name) s->Name->NamePos = pos[i].start;
        UpdateBasicLitPos(s->Path, pos[i].start);
        s->EndPos = pos[i].end;
        auto found = import_comments.find(s);
        if (found == import_comments.end()) continue;
        for (const CgPos &g : found->second) {
            for (Comment *c : g.cg->List) c->Slash = g.left ? pos[i].start - 1 : pos[i].end;
        }
    }
    std::stable_sort(cbegin, cend, [](CommentGroup *a, CommentGroup *b) { return NodePos(a) < NodePos(b); });
    return specs;
}

}  // namespace

void SortImports(LineTable &lt, File *f) {
    for (Node *dn : f->Decls) {
        auto *d = As<GenDecl>(dn);
        if (!d || d->Tok_ != Tok::IMPORT) break;
        if (!IsValid(d->Lparen)) continue;
        size_t i = 0;
        std::vector<Node *> specs;
        for (size_t j = 0; j < d->Specs.size(); j++) {
            if (j > i && lt.Line(NodePos(d->Specs[j])) > 1 + lt.Line(NodeEnd(d->Specs[j - 1]))) {
                auto run = SortSpecs(lt, f, d, std::vector<Node *>(d->Specs.begin() + static_cast<long>(i),
                                                                   d->Specs.begin() + static_cast<long>(j)));
                specs.insert(specs.end(), run.begin(), run.end());
                i = j;
            }
        }
        auto run = SortSpecs(lt, f, d, std::vector<Node *>(d->Specs.begin() + static_cast<long>(i), d->Specs.end()));
        specs.insert(specs.end(), run.begin(), run.end());
        d->Specs = specs;
        if (!d->Specs.empty()) {
            Node *last_spec = d->Specs.back();
            const int last_line = lt.Line(NodePos(last_spec));
            int rparen_line = lt.Line(d->Rparen);
            while (rparen_line > last_line + 1) {
                rparen_line--;
                lt.MergeLine(rparen_line);
            }
        }
    }
    f->Imports.clear();
    for (Node *decl : f->Decls) {
        if (auto *d = As<GenDecl>(decl); d && d->Tok_ == Tok::IMPORT) {
            for (Node *s : d->Specs) f->Imports.push_back(static_cast<ImportSpec *>(s));
        }
    }
}

}  // namespace goprint
