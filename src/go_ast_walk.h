// ForEachChild for go_ast.h: go/ast's Walk order (walk.go), one level.
// Included from go_ast.h only.

namespace goast {

template <class F> void ForEachChild(Node *n, F &&f) {
    auto one = [&](Node *c) {
        if (c) f(c);
    };
    auto list = [&](auto &v) {
        for (auto *c : v) one(c);
    };
    if (!n) return;
    switch (n->kind) {
    case K::Comment: case K::BadExpr: case K::Ident: case K::BasicLit: case K::BadStmt: case K::EmptyStmt:
    case K::BadDecl:
        break;
    case K::CommentGroup: list(static_cast<CommentGroup *>(n)->List); break;
    case K::Field: {
        auto *x = static_cast<Field *>(n);
        one(x->Doc); list(x->Names); one(x->Type); one(x->Tag); one(x->Comment);
        break;
    }
    case K::FieldList: list(static_cast<FieldList *>(n)->List); break;
    case K::Ellipsis: one(static_cast<Ellipsis *>(n)->Elt); break;
    case K::FuncLit: {
        auto *x = static_cast<FuncLit *>(n);
        one(x->Type); one(x->Body);
        break;
    }
    case K::CompositeLit: {
        auto *x = static_cast<CompositeLit *>(n);
        one(x->Type); list(x->Elts);
        break;
    }
    case K::ParenExpr: one(static_cast<ParenExpr *>(n)->X); break;
    case K::SelectorExpr: {
        auto *x = static_cast<SelectorExpr *>(n);
        one(x->X); one(x->Sel);
        break;
    }
    case K::IndexExpr: {
        auto *x = static_cast<IndexExpr *>(n);
        one(x->X); one(x->Index);
        break;
    }
    case K::IndexListExpr: {
        auto *x = static_cast<IndexListExpr *>(n);
        one(x->X); list(x->Indices);
        break;
    }
    case K::SliceExpr: {
        auto *x = static_cast<SliceExpr *>(n);
        one(x->X); one(x->Low); one(x->High); one(x->Max);
        break;
    }
    case K::TypeAssertExpr: {
        auto *x = static_cast<TypeAssertExpr *>(n);
        one(x->X); one(x->Type);
        break;
    }
    case K::CallExpr: {
        auto *x = static_cast<CallExpr *>(n);
        one(x->Fun); list(x->Args);
        break;
    }
    case K::StarExpr: one(static_cast<StarExpr *>(n)->X); break;
    case K::UnaryExpr: one(static_cast<UnaryExpr *>(n)->X); break;
    case K::BinaryExpr: {
        auto *x = static_cast<BinaryExpr *>(n);
        one(x->X); one(x->Y);
        break;
    }
    case K::KeyValueExpr: {
        auto *x = static_cast<KeyValueExpr *>(n);
        one(x->Key); one(x->Value);
        break;
    }
    case K::ArrayType: {
        auto *x = static_cast<ArrayType *>(n);
        one(x->Len); one(x->Elt);
        break;
    }
    case K::StructType: one(static_cast<StructType *>(n)->Fields); break;
    case K::FuncType: {
        auto *x = static_cast<FuncType *>(n);
        one(x->TypeParams); one(x->Params); one(x->Results);
        break;
    }
    case K::InterfaceType: one(static_cast<InterfaceType *>(n)->Methods); break;
    case K::MapType: {
        auto *x = static_cast<MapType *>(n);
        one(x->Key); one(x->Value);
        break;
    }
    case K::ChanType: one(static_cast<ChanType *>(n)->Value); break;
    case K::DeclStmt: one(static_cast<DeclStmt *>(n)->Decl); break;
    case K::LabeledStmt: {
        auto *x = static_cast<LabeledStmt *>(n);
        one(x->Label); one(x->Stmt);
        break;
    }
    case K::ExprStmt: one(static_cast<ExprStmt *>(n)->X); break;
    case K::SendStmt: {
        auto *x = static_cast<SendStmt *>(n);
        one(x->Chan); one(x->Value);
        break;
    }
    case K::IncDecStmt: one(static_cast<IncDecStmt *>(n)->X); break;
    case K::AssignStmt: {
        auto *x = static_cast<AssignStmt *>(n);
        list(x->Lhs); list(x->Rhs);
        break;
    }
    case K::GoStmt: one(static_cast<GoStmt *>(n)->Call); break;
    case K::DeferStmt: one(static_cast<DeferStmt *>(n)->Call); break;
    case K::ReturnStmt: list(static_cast<ReturnStmt *>(n)->Results); break;
    case K::BranchStmt: one(static_cast<BranchStmt *>(n)->Label); break;
    case K::BlockStmt: list(static_cast<BlockStmt *>(n)->List); break;
    case K::IfStmt: {
        auto *x = static_cast<IfStmt *>(n);
        one(x->Init); one(x->Cond); one(x->Body); one(x->Else);
        break;
    }
    case K::CaseClause: {
        auto *x = static_cast<CaseClause *>(n);
        list(x->List); list(x->Body);
        break;
    }
    case K::SwitchStmt: {
        auto *x = static_cast<SwitchStmt *>(n);
        one(x->Init); one(x->Tag); one(x->Body);
        break;
    }
    case K::TypeSwitchStmt: {
        auto *x = static_cast<TypeSwitchStmt *>(n);
        one(x->Init); one(x->Assign); one(x->Body);
        break;
    }
    case K::CommClause: {
        auto *x = static_cast<CommClause *>(n);
        one(x->Comm); list(x->Body);
        break;
    }
    case K::SelectStmt: one(static_cast<SelectStmt *>(n)->Body); break;
    case K::ForStmt: {
        auto *x = static_cast<ForStmt *>(n);
        one(x->Init); one(x->Cond); one(x->Post); one(x->Body);
        break;
    }
    case K::RangeStmt: {
        auto *x = static_cast<RangeStmt *>(n);
        one(x->Key); one(x->Value); one(x->X); one(x->Body);
        break;
    }
    case K::ImportSpec: {
        auto *x = static_cast<ImportSpec *>(n);
        one(x->Doc); one(x->Name); one(x->Path); one(x->Comment);
        break;
    }
    case K::ValueSpec: {
        auto *x = static_cast<ValueSpec *>(n);
        one(x->Doc); list(x->Names); one(x->Type); list(x->Values); one(x->Comment);
        break;
    }
    case K::TypeSpec: {
        auto *x = static_cast<TypeSpec *>(n);
        one(x->Doc); one(x->Name); one(x->TypeParams); one(x->Type); one(x->Comment);
        break;
    }
    case K::GenDecl: {
        auto *x = static_cast<GenDecl *>(n);
        one(x->Doc); list(x->Specs);
        break;
    }
    case K::FuncDecl: {
        auto *x = static_cast<FuncDecl *>(n);
        one(x->Doc); one(x->Recv); one(x->Name); one(x->Type); one(x->Body);
        break;
    }
    case K::File: {
        auto *x = static_cast<File *>(n);
        one(x->Doc); one(x->Name); list(x->Decls);
        break;
    }
    }
}

}  // namespace goast
