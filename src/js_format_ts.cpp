// Prettier 3.6.2's TypeScript printers: language-js/print/{typescript,
// type-annotation,type-parameters,interface,enum,mapped-type,
// cast-expression,decorators}.js, over js_parse's typescript-estree trees.

#include <cctype>

#include "js_format_internal.h"

namespace jsfmt {

// ---------------------------------------------------------------- utils/index.js (types)

bool IsTsKeywordType(const Node* n) {
    if (!n) return false;
    switch (n->type) {
        case NT::TSAnyKeyword:
        case NT::TSUnknownKeyword:
        case NT::TSNumberKeyword:
        case NT::TSObjectKeyword:
        case NT::TSBooleanKeyword:
        case NT::TSBigIntKeyword:
        case NT::TSStringKeyword:
        case NT::TSSymbolKeyword:
        case NT::TSVoidKeyword:
        case NT::TSUndefinedKeyword:
        case NT::TSNullKeyword:
        case NT::TSNeverKeyword:
        case NT::TSIntrinsicKeyword:
            return true;
        default:
            return false;
    }
}

bool IsSimpleType(const Node* n) {
    if (!n) return false;
    if (IsTsKeywordType(n)) return true;
    switch (n->type) {
        case NT::TSThisType:
        case NT::TSLiteralType:
        case NT::TSTemplateLiteralType:
            return true;
        case NT::TSTypeReference:
            return !n->typeArguments();
        default:
            return false;
    }
}

bool IsObjectType(const Node* n) { return n && (n->type == NT::TSTypeLiteral || n->type == NT::TSMappedType); }
bool IsUnionType(const Node* n) { return n && n->type == NT::TSUnionType; }
bool IsBinaryCastExpression(const Node* n) {
    return n && (n->type == NT::TSAsExpression || n->type == NT::TSSatisfiesExpression);
}

// type-annotation.js shouldHugType
bool Printer::ShouldHugType(const Node* n) {
    if (IsSimpleType(n) || IsObjectType(n)) return true;
    if (!IsUnionType(n)) return false;
    // shouldHugUnionType
    const Node* object_type = nullptr;
    for (Node* t : n->types()) {
        if (HasComment(t)) return false;
    }
    for (Node* t : n->types()) {
        if (t->type == NT::TSTypeLiteral || t->type == NT::TSTypeReference) {
            object_type = t;
            break;
        }
    }
    if (!object_type) return false;
    for (Node* t : n->types()) {
        if (t != object_type && t->type != NT::TSVoidKeyword && t->type != NT::TSNullKeyword) return false;
    }
    return true;
}

// ---------------------------------------------------------------- type-annotation.js

std::string_view Printer::TypeAnnotationFirstToken() {
    if (Match({[](Node* x, Key) { return x->type == NT::TSTypeAnnotation; },
               [](Node* x, Key k) {
                   return (k == Key::returnType || k == Key::typeAnnotation) &&
                          (x->type == NT::TSFunctionType || x->type == NT::TSConstructorType);
               }})) {
        return "=>";
    }
    if (Match({[](Node* x, Key) { return x->type == NT::TSTypeAnnotation; },
               [](Node* x, Key k) { return k == Key::typeAnnotation && x->type == NT::TSTypePredicate; }})) {
        return "";
    }
    return ":";
}

Doc* Printer::PrintTypeAnnotationProperty(Key k) {
    Node* ta = Child(node(), k);
    if (!ta) return nullptr;
    bool space = false;
    if (ta->type == NT::TSTypeAnnotation) {
        const std::string_view first = Call(k, [&] { return TypeAnnotationFirstToken(); });
        if (first == "=>" || (first == ":" && HasComment(ta, kCLeading))) space = true;
    }
    return space ? Cat({S(" "), Print(k)}) : Print(k);
}

Doc* Printer::PrintTypeAnnotation() {
    const std::string_view token = TypeAnnotationFirstToken();
    if (token.empty()) return Print(Key::typeAnnotation);
    return Cat({S(token), S(" "), Print(Key::typeAnnotation)});
}

Doc* Printer::PrintTypeAlias() {
    Parts parts{PrintDeclareToken(), S("type "), Print(Key::id), Print(Key::typeParameters)};
    return Cat({PrintAssignment(Cat(parts), S(" ="), Key::typeAnnotation), opts_.semi ? S(";") : nullptr});
}

Doc* Printer::PrintIntersectionType() {
    bool was_indented = false;
    Parts out;
    const NodeList& types = node()->types();
    Each(Key::types, [&](int index) {
        Doc* doc = Print();
        if (index == 0) {
            out.push_back(doc);
            return;
        }
        Node* cur = node();
        Node* prev = types[static_cast<size_t>(index) - 1];
        const bool cur_obj = IsObjectType(cur);
        const bool prev_obj = IsObjectType(prev);
        if (prev_obj && cur_obj) {
            out.push_back(Cat({S(" & "), was_indented ? Indent(doc) : doc}));
            return;
        }
        if ((!prev_obj && !cur_obj) || HasLeadingOwnLineComment(cur)) {
            out.push_back(Indent(Cat({S(" &"), line(), doc})));
            return;
        }
        if (index > 1) was_indented = true;
        out.push_back(Cat({S(" & "), index > 1 ? Indent(doc) : doc}));
    });
    return Group(Cat(out));
}

Doc* Printer::PrintUnionType() {
    Node* n = node();
    Node* p = parent();
    const bool should_indent =
        p->type != NT::TSTypeParameterInstantiation && p->type != NT::TSTypeReference &&
        p->type != NT::TSTypeAssertion && p->type != NT::TSTupleType &&
        !((p->type == NT::VariableDeclarator || p->type == NT::TSTypeAliasDeclaration) && HasLeadingOwnLineComment(n));
    const bool should_hug = ShouldHugType(n);
    Parts printed;
    Each(Key::types, [&](int) {
        Doc* d = Print();
        if (!should_hug) d = a_.Align(2, d);
        printed.push_back(PrintComments(d));
    });
    if (should_hug) return a_.Join(S(" | "), printed);
    const bool should_add_start_line = should_indent && !HasLeadingOwnLineComment(n);
    Doc* code = Cat({IfBreak(Cat({should_add_start_line ? line() : nullptr, S("| ")})),
                     a_.Join(Cat({line(), S("| ")}), printed)});
    if (NeedsParens()) return Group(Cat({Indent(code), softline()}));
    if (p->type == NT::TSTupleType && p->elementTypes().size() > 1) {
        return Group(Cat({Indent(Cat({IfBreak(Cat({S("("), softline()})), code})), softline(), IfBreak(S(")"))}));
    }
    return Group(should_indent ? Indent(code) : code);
}

Doc* Printer::PrintFunctionType() {
    Node* n = node();
    Parts parts{PrintAbstractToken()};
    if (n->type == NT::TSConstructorType || n->type == NT::TSConstructSignatureDeclaration) parts.push_back(S("new "));
    Doc* params = PrintFunctionParameters(false, true);
    Doc* return_type = PrintTypeAnnotationProperty(n->returnType() ? Key::returnType : Key::typeAnnotation);
    if (ShouldGroupFunctionParameters(n, return_type)) params = Group(params);
    parts.push_back(params);
    parts.push_back(return_type);
    return Group(Cat(parts));
}

Doc* Printer::PrintTypePredicate() {
    Node* n = node();
    return Cat({n->HasTs(jsparse::kTsAsserts) ? S("asserts ") : nullptr, Print(Key::parameterName),
                n->typeAnnotation() ? Cat({S(" is "), PrintTypeAnnotationProperty()}) : nullptr});
}

// ---------------------------------------------------------------- type-parameters.js

Doc* Printer::PrintDanglingCommentsForInline() {
    Node* n = node();
    if (!HasComment(n, kCDangling)) return nullptr;
    const bool only_block = !HasComment(n, kCLine);
    Doc* printed = PrintDanglingComments(!only_block);
    if (only_block) return printed;
    return Cat({printed, hardline()});
}

Doc* Printer::PrintTypeParameters(Key params_key) {
    Node* n = node();
    const NodeList* params = ListOf(n, params_key);
    if (!params) return Print(params_key);
    const bool in_test_call = IsTestCall(grandparent());
    // isArrowFunctionVariable: path.match(not one object-type param, _, key typeAnnotation, Identifier,
    // isArrowFunctionVariableDeclarator)
    const bool arrow_var = Match({[&](Node* x, Key) {
                                      const NodeList* ps = ListOf(x, params_key);
                                      return !(ps && ps->size() == 1 && IsObjectType((*ps)[0]));
                                  },
                                  nullptr, [](Node*, Key k) { return k == Key::typeAnnotation; },
                                  [](Node* x, Key) { return x->type == NT::Identifier; },
                                  [](Node* x, Key) {
                                      return x->type == NT::VariableDeclarator &&
                                             Is(x->init(), NT::ArrowFunctionExpression);
                                  }});
    const bool should_inline =
        params->empty() || (!arrow_var && (in_test_call || (params->size() == 1 && ShouldHugType((*params)[0]))));
    if (should_inline) {
        return Cat({S("<"), a_.Join(S(", "), PrintAll(params_key)), PrintDanglingCommentsForInline(), S(">")});
    }
    Doc* trailing_comma;
    if (n->type == NT::TSTypeParameterInstantiation) {
        trailing_comma = nullptr;
    } else if (params->size() == 1 && !(*params)[0]->constraint() && parent()->type == NT::ArrowFunctionExpression &&
               !(opts_.typescript && !opts_.jsx)) {
        // shouldForceTrailingComma: not a .ts file (.tsx keeps `<T,>`)
        trailing_comma = S(",");
    } else {
        trailing_comma = IfBreak(S(","));
    }
    return Group(Cat({S("<"), Indent(Cat({softline(), a_.Join(Cat({S(","), line()}), PrintAll(params_key))})),
                      trailing_comma, softline(), S(">")}),
                 false, TypeParametersGroupId(n));
}

Doc* Printer::PrintTypeParameter() {
    Node* n = node();
    Parts parts{n->HasTs(jsparse::kTsConst) ? S("const ") : nullptr};
    if (n->HasTs(jsparse::kTsIn)) parts.push_back(S("in "));
    if (n->HasTs(jsparse::kTsOut)) parts.push_back(S("out "));
    parts.push_back(Print(Key::name));
    if (n->constraint()) {
        const GroupId id = a_.NewId();
        parts.push_back(S(" extends"));
        parts.push_back(Group(Indent(line()), false, id));
        parts.push_back(a_.line_suffix_boundary);
        parts.push_back(a_.IndentIfBreak(Print(Key::constraint), id));
    }
    if (n->defaultType()) {
        parts.push_back(S(" = "));
        parts.push_back(Print(Key::defaultType));
    }
    return Group(Cat(parts));
}

// ---------------------------------------------------------------- interface.js

Doc* Printer::PrintInterface() {
    Node* n = node();
    Parts parts{PrintDeclareToken(), S("interface")};
    Parts parts_group, extends_parts;
    parts_group.push_back(S(" "));
    parts_group.push_back(Print(Key::id));
    parts_group.push_back(Print(Key::typeParameters));
    const bool indent_only_heritage =
        n->typeParameters() && !HasComment(n->typeParameters(), kCTrailing | kCLine);
    const NodeList& ext = n->extends_list();
    if (!ext.empty()) {
        Doc* joined = a_.Join(Cat({S(","), line()}), PrintAll(Key::extends_));
        extends_parts.push_back(indent_only_heritage
                                    ? IfBreak(S(" "), line(), TypeParametersGroupId(n->typeParameters()))
                                    : line());
        extends_parts.push_back(S("extends "));
        extends_parts.push_back(ext.size() == 1 ? joined : Indent(joined));
    }
    if (HasComment(n->id(), kCTrailing) || !ext.empty()) {
        if (indent_only_heritage) {
            Parts g = parts_group;
            g.push_back(Indent(Cat(extends_parts)));
            parts.push_back(Group(Cat(g)));
        } else {
            Parts g = parts_group;
            g.insert(g.end(), extends_parts.begin(), extends_parts.end());
            parts.push_back(Group(Indent(Cat(g))));
        }
    } else {
        parts.insert(parts.end(), parts_group.begin(), parts_group.end());
        parts.insert(parts.end(), extends_parts.begin(), extends_parts.end());
    }
    parts.push_back(S(" "));
    parts.push_back(Print(Key::body));
    return Group(Cat(parts));
}

// ---------------------------------------------------------------- mapped-type.js

Doc* Printer::PrintMappedType() {
    Node* n = node();
    // objectWrap "preserve": break after `{` when the source does.
    bool should_break = false;
    if (n->key()) {
        const uint32_t start = n->start;
        const std::string after = GetTextWithoutComments(start + 1, n->key()->start);
        size_t k = 0;
        while (k < after.size() && (after[k] == ' ' || after[k] == '\t' || after[k] == '\n' || after[k] == '\r' ||
                                    after[k] == '\v' || after[k] == '\f')) {
            k++;
        }
        const int64_t next_token = k < after.size() ? static_cast<int64_t>(start) + 1 + static_cast<int64_t>(k)
                                                    : static_cast<int64_t>(start);  // search() = -1
        if (HasNewlineInRange(start, next_token)) should_break = true;
    }
    auto modifier = [&](uint32_t plus, uint32_t minus, std::string_view keyword) -> Doc* {
        if (n->HasTs(plus)) return Cat({S("+"), S(keyword)});
        if (n->HasTs(minus)) return Cat({S("-"), S(keyword)});
        return S(keyword);
    };
    Doc* bl = opts_.bracket_spacing ? line() : softline();
    Doc* inner = Group(Cat({n->HasTs(jsparse::kTsReadonly)
                                ? Cat({modifier(jsparse::kTsReadonlyPlus, jsparse::kTsReadonlyMinus, "readonly"),
                                       S(" ")})
                                : nullptr,
                            S("["), Print(Key::key), S(" in "), Print(Key::constraint),
                            n->nameType() ? Cat({S(" as "), Print(Key::nameType)}) : nullptr, S("]"),
                            n->HasTs(jsparse::kTsOptional)
                                ? modifier(jsparse::kTsMappedPlus, jsparse::kTsMappedMinus, "?")
                                : nullptr,
                            n->typeAnnotation() ? S(": ") : nullptr, Print(Key::typeAnnotation)}));
    return Group(Cat({S("{"),
                      Indent(Cat({bl,
                                  HasComment(n, kCDangling) ? Group(Cat({PrintDanglingComments(), hardline()}))
                                                            : nullptr,
                                  inner, opts_.semi ? IfBreak(S(";")) : nullptr})),
                      bl, S("}")}),
                 should_break);
}

// ---------------------------------------------------------------- cast-expression.js

Doc* Printer::PrintBinaryCastExpression() {
    Node* n = node();
    Node* p = parent();
    const Key k = key();
    Parts parts{Print(Key::expression), S(n->type == NT::TSAsExpression ? " as " : " satisfies "),
                Print(Key::typeAnnotation)};
    if ((k == Key::callee && IsCallExpression(p)) || (k == Key::object && IsMemberExpression(p))) {
        Parts inner{softline()};
        inner.insert(inner.end(), parts.begin(), parts.end());
        return Group(Cat({Indent(Cat(inner)), softline()}));
    }
    return Cat(parts);
}

// ---------------------------------------------------------------- decorators.js

bool Printer::HasNewlineBetweenOrAfterDecorators(const Node* n) {
    for (Node* d : n->decorators()) {
        if (HasNewline(d->end)) return true;
    }
    return false;
}

static bool HasDecoratorsBeforeExport(const Node* n) {
    if (!n || (n->type != NT::ExportDefaultDeclaration && n->type != NT::ExportNamedDeclaration)) return false;
    const Node* d = n->declaration();
    return d && !d->decorators().empty() && d->decorators()[0]->start <= n->start;  // locStart: min with decorators
}

Doc* Printer::PrintClassMemberDecorators() {
    Node* n = node();
    return Group(Cat({a_.Join(line(), PrintAll(Key::decorators)),
                      HasNewlineBetweenOrAfterDecorators(n) ? hardline() : line()}));
}

Doc* Printer::PrintDecoratorsBeforeExport() {
    Node* n = node();
    if (!HasDecoratorsBeforeExport(n)) return nullptr;
    Parts printed = Call(Key::declaration, [&] { return PrintAll(Key::decorators); });
    return Cat({a_.Join(hardline(), printed), hardline()});
}

Doc* Printer::PrintDecorators() {
    Node* n = node();
    Node* p = parent();
    if (n->decorators().empty() || HasDecoratorsBeforeExport(p) || IsIgnored()) return nullptr;
    const bool should_break = n->type == NT::ClassExpression || n->type == NT::ClassDeclaration ||
                              HasNewlineBetweenOrAfterDecorators(n);
    const bool export_decl = key() == Key::declaration && p &&
                             (p->type == NT::ExportDefaultDeclaration || p->type == NT::ExportNamedDeclaration ||
                              p->type == NT::ExportAllDeclaration);
    return Cat({export_decl ? hardline() : should_break ? a_.break_parent : nullptr,
                a_.Join(line(), PrintAll(Key::decorators)), line()});
}

// ---------------------------------------------------------------- typescript.js

Doc* Printer::PrintTypescript(const Args* a) {
    Node* n = node();
    if (IsTsKeywordType(n)) {
        std::string_view name = jsparse::NodeTypeName(n->type);
        name = name.substr(2, name.size() - 9);
        std::string lower(name);
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return a_.Own(std::move(lower));
    }
    Doc* semi = opts_.semi ? S(";") : nullptr;
    switch (n->type) {
        case NT::TSThisType:
            return S("this");
        case NT::TSTypeAssertion: {
            const bool should_break_after_cast = !(IsArrayExpression(n->expression()) || IsObjectExpression(n->expression()));
            Doc* cast_group = Group(Cat({S("<"), Indent(Cat({softline(), Print(Key::typeAnnotation)})), softline(), S(">")}));
            if (should_break_after_cast) {
                Doc* expr_contents = Cat({IfBreak(S("(")), Indent(Cat({softline(), Print(Key::expression)})), softline(),
                                          IfBreak(S(")"))});
                return a_.ConditionalGroup({Cat({cast_group, Print(Key::expression)}),
                                            Cat({cast_group, Group(expr_contents, true)}),
                                            Cat({cast_group, Print(Key::expression)})});
            }
            return Group(Cat({cast_group, Print(Key::expression)}));
        }
        case NT::TSDeclareFunction:
            return PrintFunction(a);
        case NT::TSExportAssignment:
            return Cat({S("export = "), Print(Key::expression), semi});
        case NT::TSModuleBlock:
            return PrintBlock();
        case NT::TSInterfaceBody:
        case NT::TSTypeLiteral:
            return PrintObject();
        case NT::TSTypeAliasDeclaration:
            return PrintTypeAlias();
        case NT::TSQualifiedName:
            return Cat({Print(Key::left), S("."), Print(Key::right)});
        case NT::TSAbstractMethodDefinition:
            return PrintClassMethod();
        case NT::TSAbstractAccessorProperty:
        case NT::TSAbstractPropertyDefinition:
            return PrintClassProperty();
        case NT::TSInterfaceHeritage:
        case NT::TSClassImplements:
        case NT::TSInstantiationExpression:
            return Cat({Print(Key::expression), Print(Key::typeArguments)});
        case NT::TSTemplateLiteralType:
            return PrintTemplateLiteral();
        case NT::TSNamedTupleMember:
            return Cat({Print(Key::label), n->HasTs(jsparse::kTsOptional) ? S("?") : nullptr, S(": "),
                        Print(Key::elementType)});
        case NT::TSRestType:
            return Cat({S("..."), Print(Key::typeAnnotation)});
        case NT::TSOptionalType:
            return Cat({Print(Key::typeAnnotation), S("?")});
        // JSDoc types (js_parse.h): printed back as written, like prettier's
        // printJSDocType (the inner type is typeAnnotation.typeAnnotation)
        case NT::TSJSDocAllType:
            return S("*");
        case NT::TSJSDocUnknownType:
            return S("?");
        case NT::TSJSDocNullableType:
        case NT::TSJSDocNonNullableType:
        case NT::TSJSDocOptionalType: {
            Doc* inner = Call(Key::typeAnnotation, [&] { return Print(Key::typeAnnotation); });
            Doc* mark = S(n->type == NT::TSJSDocNullableType ? "?" : n->type == NT::TSJSDocOptionalType ? "=" : "!");
            if (n->type == NT::TSJSDocOptionalType || n->HasTs(jsparse::kTsPostfix)) return Cat({inner, mark});
            return Cat({mark, inner});
        }
        case NT::TSJSDocFunctionType:
            return Cat({S("function("), a_.Join(S(", "), PrintAll(Key::params)), S(")"),
                        PrintTypeAnnotationProperty(Key::returnType)});
        case NT::TSInterfaceDeclaration:
            return PrintInterface();
        case NT::TSTypeParameterDeclaration:
        case NT::TSTypeParameterInstantiation:
            return PrintTypeParameters(Key::params);
        case NT::TSTypeParameter:
            return PrintTypeParameter();
        case NT::TSAsExpression:
        case NT::TSSatisfiesExpression:
            return PrintBinaryCastExpression();
        case NT::TSArrayType:
            return Cat({Print(Key::elementType), S("[]")});
        case NT::TSPropertySignature:
            return Cat({n->HasTs(jsparse::kTsReadonly) ? S("readonly ") : nullptr, PrintPropertyKey(),
                        PrintOptionalToken(), PrintTypeAnnotationProperty()});
        case NT::TSParameterProperty:
            return Cat({PrintAccessibilityToken(), n->HasTs(jsparse::kTsStatic) ? S("static ") : nullptr,
                        n->HasTs(jsparse::kTsOverride) ? S("override ") : nullptr,
                        n->HasTs(jsparse::kTsReadonly) ? S("readonly ") : nullptr, Print(Key::parameter)});
        case NT::TSTypeQuery:
            return Cat({S("typeof "), Print(Key::exprName), Print(Key::typeArguments)});
        case NT::TSIndexSignature: {
            Doc* trailing = n->parameters().size() > 1 ? IfBreak(S(",")) : nullptr;
            Doc* params_group =
                Group(Cat({Indent(Cat({softline(), a_.Join(Cat({S(", "), softline()}), PrintAll(Key::parameters))})),
                           trailing, softline()}));
            const bool is_class_member = parent()->type == NT::ClassBody && key() == Key::body;
            return Cat({is_class_member && n->HasTs(jsparse::kTsStatic) ? S("static ") : nullptr,
                        n->HasTs(jsparse::kTsReadonly) ? S("readonly ") : nullptr, S("["), params_group, S("]"),
                        PrintTypeAnnotationProperty(), is_class_member ? semi : nullptr});
        }
        case NT::TSTypePredicate:
            return PrintTypePredicate();
        case NT::TSNonNullExpression:
            return Cat({Print(Key::expression), S("!")});
        case NT::TSImportType:
            return Cat({S("import"), PrintCallArguments(),
                        n->qualifier() ? Cat({S("."), Print(Key::qualifier)}) : nullptr, Print(Key::typeArguments)});
        case NT::TSLiteralType:
            return Print(Key::literal);
        case NT::TSIndexedAccessType:
            return Cat({Print(Key::objectType), S("["), Print(Key::indexType), S("]")});
        case NT::TSTypeOperator:
            return Cat({S(n->op), S(" "), Print(Key::typeAnnotation)});
        case NT::TSMappedType:
            return PrintMappedType();
        case NT::TSMethodSignature: {
            Parts parts{PrintAccessibilityToken(),
                        !n->kind.empty() && n->kind != "method" ? Cat({S(n->kind), S(" ")}) : nullptr,
                        n->Has(jsparse::kFlagComputed) ? S("[") : nullptr, Print(Key::key),
                        n->Has(jsparse::kFlagComputed) ? S("]") : nullptr, PrintOptionalToken()};
            Doc* params = PrintFunctionParameters(false, true);
            const Key rk = n->returnType() ? Key::returnType : Key::typeAnnotation;
            Doc* return_type = Child(n, rk) ? PrintTypeAnnotationProperty(rk) : nullptr;
            const bool group_params = ShouldGroupFunctionParameters(n, return_type);
            parts.push_back(group_params ? Group(params) : params);
            if (Child(n, rk)) parts.push_back(Group(return_type));
            return Group(Cat(parts));
        }
        case NT::TSNamespaceExportDeclaration:
            return Cat({S("export as namespace "), Print(Key::id), semi});
        case NT::TSEnumDeclaration:
            return Cat({PrintDeclareToken(), n->HasTs(jsparse::kTsConst) ? S("const ") : nullptr, S("enum "),
                        Print(Key::id), S(" "), Print(Key::body)});
        case NT::TSEnumBody:
            return PrintObject();
        case NT::TSEnumMember: {
            Doc* id = Print(Key::id);
            if (n->Has(jsparse::kFlagComputed)) id = Cat({S("["), id, S("]")});
            if (!n->initializer()) return id;
            return Cat({id, S(" = "), Print(Key::initializer)});
        }
        case NT::TSImportEqualsDeclaration:
            return Cat({S("import "), n->HasTs(jsparse::kTsTypeOnly) ? S("type ") : nullptr, Print(Key::id), S(" = "),
                        Print(Key::moduleReference), semi});
        case NT::TSExternalModuleReference:
            return Cat({S("require("), Print(Key::expression), S(")")});
        case NT::TSModuleDeclaration: {
            Parts parts;
            Node* p = parent();
            const bool parent_is_decl = p->type == NT::TSModuleDeclaration;
            const bool body_is_decl = Is(n->body(), NT::TSModuleDeclaration);
            if (parent_is_decl) {
                parts.push_back(S("."));
            } else {
                parts.push_back(PrintDeclareToken());
                if (n->kind != "global") {
                    parts.push_back(S(n->kind));
                    parts.push_back(S(" "));
                }
            }
            parts.push_back(Print(Key::id));
            if (body_is_decl) {
                parts.push_back(Print(Key::body));
            } else if (n->body()) {
                parts.push_back(S(" "));
                parts.push_back(Group(Print(Key::body)));
            } else {
                parts.push_back(semi);
            }
            return Cat(parts);
        }
        case NT::TSConditionalType:
            return PrintTernary();
        case NT::TSInferType:
            return Cat({S("infer "), Print(Key::typeParameter)});
        case NT::TSIntersectionType:
            return PrintIntersectionType();
        case NT::TSUnionType:
            return PrintUnionType();
        case NT::TSFunctionType:
        case NT::TSCallSignatureDeclaration:
        case NT::TSConstructorType:
        case NT::TSConstructSignatureDeclaration:
            return PrintFunctionType();
        case NT::TSTupleType:
            return PrintArray();
        case NT::TSTypeReference:
            return Cat({Print(Key::typeName), Print(Key::typeArguments)});
        case NT::TSTypeAnnotation:
            return PrintTypeAnnotation();
        case NT::TSEmptyBodyFunctionExpression:
            return PrintMethodValue();
        default:
            throw UnsupportedNode{n};
    }
}

}  // namespace jsfmt
