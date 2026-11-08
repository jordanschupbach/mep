// go/token and go/ast helpers for go_ast.h. A translation of parts of Go's
// go/token (token.go) and go/ast (ast.go): Copyright 2009 The Go Authors.
// All rights reserved. BSD-style license; the full notice is in
// go_parse.cpp.

#include "go_ast.h"

#include <algorithm>

namespace goast {

namespace {

#include "go_unicode.inc"

template <size_t N> bool InTable(const uint32_t (&t)[N][2], uint32_t r) {
    size_t lo = 0, hi = N;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (r < t[mid][0]) hi = mid;
        else if (r > t[mid][1]) lo = mid + 1;
        else return true;
    }
    return false;
}

const char *const kTokStrings[] = {
    "ILLEGAL", "EOF", "COMMENT",
    "IDENT", "INT", "FLOAT", "IMAG", "CHAR", "STRING",
    "+", "-", "*", "/", "%",
    "&", "|", "^", "<<", ">>", "&^",
    "+=", "-=", "*=", "/=", "%=",
    "&=", "|=", "^=", "<<=", ">>=", "&^=",
    "&&", "||", "<-", "++", "--",
    "==", "<", ">", "=", "!",
    "!=", "<=", ">=", ":=", "...",
    "(", "[", "{", ",", ".",
    ")", "]", "}", ";", ":",
    "~",
    "break", "case", "chan", "const", "continue",
    "default", "defer", "else", "fallthrough", "for",
    "func", "go", "goto", "if", "import",
    "interface", "map", "package", "range", "return",
    "select", "struct", "switch", "type", "var",
};

}  // namespace

bool UnicodeIsLetter(uint32_t r) {
    if (r < 0x80) return (r | 0x20) >= 'a' && (r | 0x20) <= 'z';
    return InTable(kGoLetter, r);
}
bool UnicodeIsDigit(uint32_t r) {
    if (r < 0x80) return r >= '0' && r <= '9';
    return InTable(kGoDigit, r);
}
bool UnicodeIsSpace(uint32_t r) {
    if (r <= 0xFF) {
        return r == '\t' || r == '\n' || r == '\v' || r == '\f' || r == '\r' || r == ' ' || r == 0x85 || r == 0xA0;
    }
    return InTable(kGoSpace, r);
}
bool UnicodeIsUpper(uint32_t r) {
    if (r < 0x80) return r >= 'A' && r <= 'Z';
    return InTable(kGoUpper, r);
}
bool UnicodeIsLower(uint32_t r) {
    if (r < 0x80) return r >= 'a' && r <= 'z';
    return InTable(kGoLower, r);
}
bool UnicodeIsPunct(uint32_t r) { return InTable(kGoPunct, r); }
bool UnicodeIsPrint(uint32_t r) {
    if (r == ' ') return true;
    return InTable(kGoPrint, r);
}

uint32_t DecodeRune(std::string_view s, size_t i, int *width) {
    if (i >= s.size()) {
        *width = 0;
        return RuneError;
    }
    auto b = [&](size_t k) { return static_cast<uint32_t>(static_cast<unsigned char>(s[k])); };
    uint32_t c0 = b(i);
    if (c0 < 0x80) {
        *width = 1;
        return c0;
    }
    size_t n = s.size() - i;
    auto cont = [&](size_t k) { return k < s.size() && (b(k) & 0xC0) == 0x80; };
    if (c0 >= 0xC2 && c0 <= 0xDF) {
        if (n >= 2 && cont(i + 1)) {
            *width = 2;
            return ((c0 & 0x1F) << 6) | (b(i + 1) & 0x3F);
        }
    } else if (c0 >= 0xE0 && c0 <= 0xEF) {
        if (n >= 3 && cont(i + 1) && cont(i + 2)) {
            uint32_t c1 = b(i + 1);
            bool ok = !(c0 == 0xE0 && c1 < 0xA0) && !(c0 == 0xED && c1 > 0x9F);
            if (ok) {
                *width = 3;
                return ((c0 & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (b(i + 2) & 0x3F);
            }
        }
    } else if (c0 >= 0xF0 && c0 <= 0xF4) {
        if (n >= 4 && cont(i + 1) && cont(i + 2) && cont(i + 3)) {
            uint32_t c1 = b(i + 1);
            bool ok = !(c0 == 0xF0 && c1 < 0x90) && !(c0 == 0xF4 && c1 > 0x8F);
            if (ok) {
                *width = 4;
                return ((c0 & 0x07) << 18) | ((c1 & 0x3F) << 12) | ((b(i + 2) & 0x3F) << 6) | (b(i + 3) & 0x3F);
            }
        }
    }
    *width = 1;
    return RuneError;
}

uint32_t DecodeLastRune(std::string_view s, int *width) {
    if (s.empty()) {
        *width = 0;
        return RuneError;
    }
    size_t end = s.size();
    size_t start = end - 1;
    if (static_cast<unsigned char>(s[start]) < 0x80) {
        *width = 1;
        return static_cast<unsigned char>(s[start]);
    }
    size_t lim = end >= 4 ? end - 4 : 0;
    while (start > lim && (static_cast<unsigned char>(s[start]) & 0xC0) == 0x80) start--;
    int w = 0;
    uint32_t r = DecodeRune(s, start, &w);
    if (start + static_cast<size_t>(w) != end) {
        *width = 1;
        return RuneError;
    }
    *width = w;
    return r;
}

int RuneCount(std::string_view s) {
    int n = 0;
    size_t i = 0;
    while (i < s.size()) {
        int w = 0;
        DecodeRune(s, i, &w);
        i += static_cast<size_t>(w);
        n++;
    }
    return n;
}

void AppendRune(std::string &out, uint32_t r) {
    if (r > 0x10FFFF || (r >= 0xD800 && r < 0xE000)) r = RuneError;
    if (r < 0x80) {
        out += static_cast<char>(r);
    } else if (r < 0x800) {
        out += static_cast<char>(0xC0 | (r >> 6));
        out += static_cast<char>(0x80 | (r & 0x3F));
    } else if (r < 0x10000) {
        out += static_cast<char>(0xE0 | (r >> 12));
        out += static_cast<char>(0x80 | ((r >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (r & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (r >> 18));
        out += static_cast<char>(0x80 | ((r >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((r >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (r & 0x3F));
    }
}

const char *TokString(Tok t) {
    auto i = static_cast<size_t>(t);
    if (i < sizeof(kTokStrings) / sizeof(kTokStrings[0])) return kTokStrings[i];
    return "token(?)";
}

int Precedence(Tok t) {
    switch (t) {
    case Tok::LOR: return 1;
    case Tok::LAND: return 2;
    case Tok::EQL: case Tok::NEQ: case Tok::LSS: case Tok::LEQ: case Tok::GTR: case Tok::GEQ: return 3;
    case Tok::ADD: case Tok::SUB: case Tok::OR: case Tok::XOR: return 4;
    case Tok::MUL: case Tok::QUO: case Tok::REM: case Tok::SHL: case Tok::SHR: case Tok::AND: case Tok::AND_NOT:
        return 5;
    default: return LowestPrec;
    }
}

Tok Lookup(std::string_view s) {
    if (s.size() < 2 || s.size() > 11) return Tok::IDENT;
    for (int i = static_cast<int>(Tok::BREAK); i <= static_cast<int>(Tok::VAR); i++) {
        if (s == kTokStrings[i]) return static_cast<Tok>(i);
    }
    return Tok::IDENT;
}

bool IsKeywordName(std::string_view s) { return Lookup(s) != Tok::IDENT; }

bool IsIdentifier(std::string_view s) {
    if (s.empty() || IsKeywordName(s)) return false;
    size_t i = 0;
    bool first = true;
    while (i < s.size()) {
        int w = 0;
        uint32_t r = DecodeRune(s, i, &w);
        if (!(UnicodeIsLetter(r) || r == '_' || (!first && UnicodeIsDigit(r)))) return false;
        first = false;
        i += static_cast<size_t>(w);
    }
    return true;
}

bool IsExported(std::string_view s) {
    int w = 0;
    return UnicodeIsUpper(DecodeRune(s, 0, &w));
}

int NumFields(const FieldList *f) {
    int n = 0;
    if (f) {
        for (const Field *g : f->List) {
            int m = static_cast<int>(g->Names.size());
            if (m == 0) m = 1;
            n += m;
        }
    }
    return n;
}

Node *Unparen(Node *e) {
    while (auto *p = As<ParenExpr>(e)) e = p->X;
    return e;
}
const Node *Unparen(const Node *e) {
    while (auto *p = As<ParenExpr>(e)) e = p->X;
    return e;
}

namespace {

bool IsDirective(std::string_view c) {
    if (c.substr(0, 5) == "line " || c.substr(0, 7) == "extern " || c.substr(0, 7) == "export ") return true;
    size_t colon = c.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= c.size()) return false;
    for (size_t i = 0; i <= colon + 1; i++) {
        if (i == colon) continue;
        char b = c[i];
        if (!(('a' <= b && b <= 'z') || ('0' <= b && b <= '9'))) return false;
    }
    return true;
}

std::string_view StripTrailingWhitespace(std::string_view s) {
    size_t i = s.size();
    while (i > 0 && (s[i - 1] == ' ' || s[i - 1] == '\t' || s[i - 1] == '\n' || s[i - 1] == '\r')) i--;
    return s.substr(0, i);
}

}  // namespace

std::string CommentGroupText(const CommentGroup *g) {
    if (!g) return "";
    std::vector<std::string> lines;
    for (const Comment *cm : g->List) {
        std::string_view c = cm->Text;
        if (c.size() >= 2 && c[1] == '/') {
            c = c.substr(2);
            if (!c.empty()) {
                if (c[0] == ' ') c = c.substr(1);
                else if (IsDirective(c)) continue;
            }
        } else if (c.size() >= 4) {
            c = c.substr(2, c.size() - 4);
        }
        size_t start = 0;
        while (true) {
            size_t nl = c.find('\n', start);
            std::string_view l = c.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
            lines.emplace_back(StripTrailingWhitespace(l));
            if (nl == std::string_view::npos) break;
            start = nl + 1;
        }
    }
    size_t n = 0;
    for (size_t i = 0; i < lines.size(); i++) {
        if (!lines[i].empty() || (n > 0 && !lines[n - 1].empty())) lines[n++] = lines[i];
    }
    lines.resize(n);
    if (n > 0 && !lines[n - 1].empty()) lines.emplace_back();
    std::string out;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) out += '\n';
        out += lines[i];
    }
    return out;
}

Pos NodePos(const Node *n) {
    if (!n) return NoPos;
    switch (n->kind) {
    case K::BadExpr: return static_cast<const BadExpr *>(n)->From;
    case K::Ident: return static_cast<const Ident *>(n)->NamePos;
    case K::Ellipsis: return static_cast<const Ellipsis *>(n)->Ellipsis_;
    case K::BasicLit: return static_cast<const BasicLit *>(n)->ValuePos;
    case K::FuncLit: return NodePos(static_cast<const FuncLit *>(n)->Type);
    case K::CompositeLit: {
        auto *x = static_cast<const CompositeLit *>(n);
        return x->Type ? NodePos(x->Type) : x->Lbrace;
    }
    case K::ParenExpr: return static_cast<const ParenExpr *>(n)->Lparen;
    case K::SelectorExpr: return NodePos(static_cast<const SelectorExpr *>(n)->X);
    case K::IndexExpr: return NodePos(static_cast<const IndexExpr *>(n)->X);
    case K::IndexListExpr: return NodePos(static_cast<const IndexListExpr *>(n)->X);
    case K::SliceExpr: return NodePos(static_cast<const SliceExpr *>(n)->X);
    case K::TypeAssertExpr: return NodePos(static_cast<const TypeAssertExpr *>(n)->X);
    case K::CallExpr: return NodePos(static_cast<const CallExpr *>(n)->Fun);
    case K::StarExpr: return static_cast<const StarExpr *>(n)->Star;
    case K::UnaryExpr: return static_cast<const UnaryExpr *>(n)->OpPos;
    case K::BinaryExpr: return NodePos(static_cast<const BinaryExpr *>(n)->X);
    case K::KeyValueExpr: return NodePos(static_cast<const KeyValueExpr *>(n)->Key);
    case K::ArrayType: return static_cast<const ArrayType *>(n)->Lbrack;
    case K::StructType: return static_cast<const StructType *>(n)->Struct;
    case K::FuncType: {
        auto *x = static_cast<const FuncType *>(n);
        if (IsValid(x->Func) || !x->Params) return x->Func;
        return NodePos(x->Params);
    }
    case K::InterfaceType: return static_cast<const InterfaceType *>(n)->Interface;
    case K::MapType: return static_cast<const MapType *>(n)->Map;
    case K::ChanType: return static_cast<const ChanType *>(n)->Begin;
    case K::BadStmt: return static_cast<const BadStmt *>(n)->From;
    case K::DeclStmt: return NodePos(static_cast<const DeclStmt *>(n)->Decl);
    case K::EmptyStmt: return static_cast<const EmptyStmt *>(n)->Semicolon;
    case K::LabeledStmt: return NodePos(static_cast<const LabeledStmt *>(n)->Label);
    case K::ExprStmt: return NodePos(static_cast<const ExprStmt *>(n)->X);
    case K::SendStmt: return NodePos(static_cast<const SendStmt *>(n)->Chan);
    case K::IncDecStmt: return NodePos(static_cast<const IncDecStmt *>(n)->X);
    case K::AssignStmt: {
        auto *x = static_cast<const AssignStmt *>(n);
        return x->Lhs.empty() ? NoPos : NodePos(x->Lhs[0]);
    }
    case K::GoStmt: return static_cast<const GoStmt *>(n)->Go;
    case K::DeferStmt: return static_cast<const DeferStmt *>(n)->Defer;
    case K::ReturnStmt: return static_cast<const ReturnStmt *>(n)->Return;
    case K::BranchStmt: return static_cast<const BranchStmt *>(n)->TokPos;
    case K::BlockStmt: return static_cast<const BlockStmt *>(n)->Lbrace;
    case K::IfStmt: return static_cast<const IfStmt *>(n)->If;
    case K::CaseClause: return static_cast<const CaseClause *>(n)->Case;
    case K::SwitchStmt: return static_cast<const SwitchStmt *>(n)->Switch;
    case K::TypeSwitchStmt: return static_cast<const TypeSwitchStmt *>(n)->Switch;
    case K::CommClause: return static_cast<const CommClause *>(n)->Case;
    case K::SelectStmt: return static_cast<const SelectStmt *>(n)->Select;
    case K::ForStmt: return static_cast<const ForStmt *>(n)->For;
    case K::RangeStmt: return static_cast<const RangeStmt *>(n)->For;
    case K::ImportSpec: {
        auto *x = static_cast<const ImportSpec *>(n);
        return x->Name ? NodePos(x->Name) : NodePos(x->Path);
    }
    case K::ValueSpec: return NodePos(static_cast<const ValueSpec *>(n)->Names[0]);
    case K::TypeSpec: return NodePos(static_cast<const TypeSpec *>(n)->Name);
    case K::BadDecl: return static_cast<const BadDecl *>(n)->From;
    case K::GenDecl: return static_cast<const GenDecl *>(n)->TokPos;
    case K::FuncDecl: return NodePos(static_cast<const FuncDecl *>(n)->Type);
    case K::Comment: return static_cast<const Comment *>(n)->Slash;
    case K::CommentGroup: return NodePos(static_cast<const CommentGroup *>(n)->List.front());
    case K::Field: {
        auto *x = static_cast<const Field *>(n);
        if (!x->Names.empty()) return NodePos(x->Names[0]);
        if (x->Type) return NodePos(x->Type);
        return NoPos;
    }
    case K::FieldList: {
        auto *x = static_cast<const FieldList *>(n);
        if (IsValid(x->Opening)) return x->Opening;
        if (!x->List.empty()) return NodePos(x->List[0]);
        return NoPos;
    }
    case K::File: return static_cast<const File *>(n)->Package;
    }
    return NoPos;
}

Pos NodeEnd(const Node *n) {
    if (!n) return NoPos;
    switch (n->kind) {
    case K::BadExpr: return static_cast<const BadExpr *>(n)->To;
    case K::Ident: {
        auto *x = static_cast<const Ident *>(n);
        return x->NamePos + static_cast<int>(x->Name.size());
    }
    case K::Ellipsis: {
        auto *x = static_cast<const Ellipsis *>(n);
        return x->Elt ? NodeEnd(x->Elt) : x->Ellipsis_ + 3;
    }
    case K::BasicLit: {
        auto *x = static_cast<const BasicLit *>(n);
        if (!IsValid(x->ValueEnd)) return x->ValuePos + static_cast<int>(x->Value.size());
        return x->ValueEnd;
    }
    case K::FuncLit: return NodeEnd(static_cast<const FuncLit *>(n)->Body);
    case K::CompositeLit: return static_cast<const CompositeLit *>(n)->Rbrace + 1;
    case K::ParenExpr: return static_cast<const ParenExpr *>(n)->Rparen + 1;
    case K::SelectorExpr: return NodeEnd(static_cast<const SelectorExpr *>(n)->Sel);
    case K::IndexExpr: return static_cast<const IndexExpr *>(n)->Rbrack + 1;
    case K::IndexListExpr: return static_cast<const IndexListExpr *>(n)->Rbrack + 1;
    case K::SliceExpr: return static_cast<const SliceExpr *>(n)->Rbrack + 1;
    case K::TypeAssertExpr: return static_cast<const TypeAssertExpr *>(n)->Rparen + 1;
    case K::CallExpr: return static_cast<const CallExpr *>(n)->Rparen + 1;
    case K::StarExpr: return NodeEnd(static_cast<const StarExpr *>(n)->X);
    case K::UnaryExpr: return NodeEnd(static_cast<const UnaryExpr *>(n)->X);
    case K::BinaryExpr: return NodeEnd(static_cast<const BinaryExpr *>(n)->Y);
    case K::KeyValueExpr: return NodeEnd(static_cast<const KeyValueExpr *>(n)->Value);
    case K::ArrayType: return NodeEnd(static_cast<const ArrayType *>(n)->Elt);
    case K::StructType: return NodeEnd(static_cast<const StructType *>(n)->Fields);
    case K::FuncType: {
        auto *x = static_cast<const FuncType *>(n);
        if (x->Results) return NodeEnd(x->Results);
        return NodeEnd(x->Params);
    }
    case K::InterfaceType: return NodeEnd(static_cast<const InterfaceType *>(n)->Methods);
    case K::MapType: return NodeEnd(static_cast<const MapType *>(n)->Value);
    case K::ChanType: return NodeEnd(static_cast<const ChanType *>(n)->Value);
    case K::BadStmt: return static_cast<const BadStmt *>(n)->To;
    case K::DeclStmt: return NodeEnd(static_cast<const DeclStmt *>(n)->Decl);
    case K::EmptyStmt: {
        auto *x = static_cast<const EmptyStmt *>(n);
        return x->Implicit ? x->Semicolon : x->Semicolon + 1;
    }
    case K::LabeledStmt: return NodeEnd(static_cast<const LabeledStmt *>(n)->Stmt);
    case K::ExprStmt: return NodeEnd(static_cast<const ExprStmt *>(n)->X);
    case K::SendStmt: return NodeEnd(static_cast<const SendStmt *>(n)->Value);
    case K::IncDecStmt: return static_cast<const IncDecStmt *>(n)->TokPos + 2;
    case K::AssignStmt: {
        auto *x = static_cast<const AssignStmt *>(n);
        return x->Rhs.empty() ? NoPos : NodeEnd(x->Rhs.back());
    }
    case K::GoStmt: return NodeEnd(static_cast<const GoStmt *>(n)->Call);
    case K::DeferStmt: return NodeEnd(static_cast<const DeferStmt *>(n)->Call);
    case K::ReturnStmt: {
        auto *x = static_cast<const ReturnStmt *>(n);
        if (!x->Results.empty()) return NodeEnd(x->Results.back());
        return x->Return + 6;
    }
    case K::BranchStmt: {
        auto *x = static_cast<const BranchStmt *>(n);
        if (x->Label) return NodeEnd(x->Label);
        return x->TokPos + static_cast<int>(std::string_view(TokString(x->Tok_)).size());
    }
    case K::BlockStmt: {
        auto *x = static_cast<const BlockStmt *>(n);
        if (IsValid(x->Rbrace)) return x->Rbrace + 1;
        if (!x->List.empty()) return NodeEnd(x->List.back());
        return x->Lbrace + 1;
    }
    case K::IfStmt: {
        auto *x = static_cast<const IfStmt *>(n);
        return x->Else ? NodeEnd(x->Else) : NodeEnd(x->Body);
    }
    case K::CaseClause: {
        auto *x = static_cast<const CaseClause *>(n);
        return x->Body.empty() ? x->Colon + 1 : NodeEnd(x->Body.back());
    }
    case K::SwitchStmt: return NodeEnd(static_cast<const SwitchStmt *>(n)->Body);
    case K::TypeSwitchStmt: return NodeEnd(static_cast<const TypeSwitchStmt *>(n)->Body);
    case K::CommClause: {
        auto *x = static_cast<const CommClause *>(n);
        return x->Body.empty() ? x->Colon + 1 : NodeEnd(x->Body.back());
    }
    case K::SelectStmt: return NodeEnd(static_cast<const SelectStmt *>(n)->Body);
    case K::ForStmt: return NodeEnd(static_cast<const ForStmt *>(n)->Body);
    case K::RangeStmt: return NodeEnd(static_cast<const RangeStmt *>(n)->Body);
    case K::ImportSpec: {
        auto *x = static_cast<const ImportSpec *>(n);
        return x->EndPos != 0 ? x->EndPos : NodeEnd(x->Path);
    }
    case K::ValueSpec: {
        auto *x = static_cast<const ValueSpec *>(n);
        if (!x->Values.empty()) return NodeEnd(x->Values.back());
        if (x->Type) return NodeEnd(x->Type);
        return NodeEnd(x->Names.back());
    }
    case K::TypeSpec: return NodeEnd(static_cast<const TypeSpec *>(n)->Type);
    case K::BadDecl: return static_cast<const BadDecl *>(n)->To;
    case K::GenDecl: {
        auto *x = static_cast<const GenDecl *>(n);
        if (IsValid(x->Rparen)) return x->Rparen + 1;
        return x->Specs.empty() ? x->TokPos : NodeEnd(x->Specs[0]);
    }
    case K::FuncDecl: {
        auto *x = static_cast<const FuncDecl *>(n);
        return x->Body ? NodeEnd(x->Body) : NodeEnd(x->Type);
    }
    case K::Comment: {
        auto *x = static_cast<const Comment *>(n);
        return x->Slash + static_cast<int>(x->Text.size());
    }
    case K::CommentGroup: return NodeEnd(static_cast<const CommentGroup *>(n)->List.back());
    case K::Field: {
        auto *x = static_cast<const Field *>(n);
        if (x->Tag) return NodeEnd(x->Tag);
        if (x->Type) return NodeEnd(x->Type);
        if (!x->Names.empty()) return NodeEnd(x->Names.back());
        return NoPos;
    }
    case K::FieldList: {
        auto *x = static_cast<const FieldList *>(n);
        if (IsValid(x->Closing)) return x->Closing + 1;
        if (!x->List.empty()) return NodeEnd(x->List.back());
        return NoPos;
    }
    case K::File: {
        auto *x = static_cast<const File *>(n);
        if (!x->Decls.empty()) return NodeEnd(x->Decls.back());
        return NodeEnd(x->Name);
    }
    }
    return NoPos;
}

}  // namespace goast
