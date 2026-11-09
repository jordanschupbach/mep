// Prettier 3.6.2's estree printer: language-js/print/{index,estree,
// statement,block,expression-statement,misc,literal,class,module,property,
// function}.js and main/ast-to-doc.js (mainPrint).

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "js_format_internal.h"

namespace jsfmt {

Printer::Printer(std::string_view text, const Options& opts, DocArena& arena)
    : text_(text), a_(arena), opts_(opts) {}

// ---------------------------------------------------------------- mainPrint

Doc* Printer::Print() { return MainPrint(nullptr); }

Doc* Printer::Print(Key k) {
    return Call(k, [&] { return MainPrint(nullptr); });
}

Doc* Printer::PrintArgs(Key k, const Args& a) {
    return Call(k, [&] { return MainPrint(&a); });
}

Doc* Printer::PrintAt(Key k, int i) {
    return CallAt(k, i, [&] { return MainPrint(nullptr); });
}

Parts Printer::PrintAll(Key k) {
    Parts out;
    Each(k, [&](int) { out.push_back(MainPrint(nullptr)); });
    return out;
}

Doc* Printer::MainPrint(const Args* a) {
    Node* n = node();
    if (!n) return nullptr;
    if (++print_calls > print_limit) throw BudgetExceeded();
    if (!a) {
        auto it = cache_.find(n);
        if (it != cache_.end()) return it->second;
    }
    Doc* d = CallPluginPrint(a);
    if (!a) cache_[n] = d;
    return d;
}

Doc* Printer::CallPluginPrint(const Args* a) {
    Doc* d = PrintNode(a);
    if (!WillPrintOwnComments()) d = PrintComments(d);
    return d;
}

Doc* Printer::InheritLabel(Doc* doc, const std::function<Doc*(Doc*)>& fn) {
    if (doc && doc->kind == DocKind::Label) return a_.Label(doc->label, fn(doc->contents));
    return fn(doc);
}

static bool IsJsxType(NT t) {
    switch (t) {
        case NT::JSXElement:
        case NT::JSXFragment:
        case NT::JSXOpeningElement:
        case NT::JSXClosingElement:
        case NT::JSXOpeningFragment:
        case NT::JSXClosingFragment:
        case NT::JSXAttribute:
        case NT::JSXSpreadAttribute:
        case NT::JSXSpreadChild:
        case NT::JSXIdentifier:
        case NT::JSXNamespacedName:
        case NT::JSXMemberExpression:
        case NT::JSXExpressionContainer:
        case NT::JSXEmptyExpression:
        case NT::JSXText:
            return true;
        default:
            return false;
    }
}

static bool ShouldPrintDirectly(NT t) {
    switch (t) {
        case NT::AccessorProperty:
        case NT::TSAbstractAccessorProperty:
        case NT::PropertyDefinition:
        case NT::TSAbstractPropertyDefinition:
        case NT::MethodDefinition:
        case NT::TSAbstractMethodDefinition:
            return true;
        default:
            return false;
    }
}

Doc* Printer::PrintNode(const Args* a) {
    Node* n = node();
    Doc* d;
    if (IsIgnored()) {
        d = PrintIgnored();
    } else if (IsJsxType(n->type)) {
        d = PrintJsx();
    } else {
        d = PrintEstree(a);
    }
    if (!d) return nullptr;
    if (ShouldPrintDirectly(n->type)) return d;
    const bool has_decorators = !n->decorators().empty();
    Doc* decorators = PrintDecorators();
    const bool is_class_expression = n->type == NT::ClassExpression;
    if (has_decorators && !is_class_expression) {
        return InheritLabel(d, [&](Doc* x) { return Group(Cat({decorators, x})); });
    }
    const bool parens = NeedsParens();
    const bool semi = ShouldPrintLeadingSemicolon();
    if (!decorators && !parens && !semi) return d;
    return InheritLabel(d, [&](Doc* x) {
        return Cat({semi ? S(";") : nullptr, parens ? S("(") : nullptr,
                    parens && is_class_expression && has_decorators ? Cat({Indent(Cat({line(), decorators, x})), line()})
                                                                    : Cat({decorators, x}),
                    parens ? S(")") : nullptr});
    });
}

// ---------------------------------------------------------------- semicolon.js

bool Printer::ShouldPrintLeadingSemicolon() {
    if (opts_.semi) return false;
    Node* n = node();
    Node* p = parent();
    const Key k = key();
    if (n->type == NT::ExpressionStatement && p &&
        ((k == Key::body && (p->type == NT::Program || p->type == NT::BlockStatement || p->type == NT::StaticBlock ||
                             p->type == NT::TSModuleBlock)) ||
         (k == Key::consequent && p->type == NT::SwitchCase))) {
        return Call(Key::expression, [&] { return ExpressionNeedsAsiProtection(); });
    }
    return false;
}

bool Printer::ExpressionNeedsAsiProtection() {
    Node* n = node();
    switch (n->type) {
        case NT::ParenthesizedExpression:
        case NT::ArrayExpression:
        case NT::ArrayPattern:
        case NT::TemplateLiteral:
        case NT::TemplateElement:
            return true;
        case NT::ArrowFunctionExpression:
            if (!(!opts_.arrow_parens_always && CanPrintParamsWithoutParens(n))) return true;
            break;
        case NT::UnaryExpression:
            if (n->Has(jsparse::kFlagPrefix) && (n->op == "+" || n->op == "-")) return true;
            break;
        case NT::Literal:
            if (IsRegExpLiteral(n)) return true;
            break;
        default:
            if (IsJsxElement(n)) return true;
    }
    if (NeedsParens()) return true;
    if (!HasNakedLeftSide(n)) return false;
    int idx;
    const Key k = GetLeftSideKey(n, &idx);
    if (k == Key::None) return false;
    if (idx >= 0) return CallAt(k, idx, [&] { return ExpressionNeedsAsiProtection(); });
    return Call(k, [&] { return ExpressionNeedsAsiProtection(); });
}

// ---------------------------------------------------------------- misc.js

Doc* Printer::PrintOptionalToken() {
    Node* n = node();
    const bool optional = (n->type == NT::CallExpression || n->type == NT::MemberExpression)
                              ? n->Has(jsparse::kFlagOptional)
                              : n->HasTs(jsparse::kTsOptional);
    if (!optional) return nullptr;
    if (n->type == NT::Identifier && key() == Key::key) return nullptr;
    if (IsCallExpression(n) || (IsMemberExpression(n) && n->Has(jsparse::kFlagComputed))) return S("?.");
    return S("?");
}

Doc* Printer::PrintDefiniteToken() {
    Node* n = node();
    if (n->HasTs(jsparse::kTsDefinite)) return S("!");
    Node* p = parent();
    if (key() == Key::id && Is(p, NT::VariableDeclarator) && p->HasTs(jsparse::kTsDefinite)) return S("!");
    return nullptr;
}

Doc* Printer::PrintDeclareToken() { return node()->HasTs(jsparse::kTsDeclare) ? S("declare ") : nullptr; }

Doc* Printer::PrintAbstractToken() {
    Node* n = node();
    return n->HasTs(jsparse::kTsAbstract) || n->type == NT::TSAbstractMethodDefinition ||
                   n->type == NT::TSAbstractPropertyDefinition || n->type == NT::TSAbstractAccessorProperty
               ? S("abstract ")
               : nullptr;
}

Doc* Printer::PrintAccessibilityToken() {
    Node* n = node();
    return n->accessibility.empty() ? nullptr : Cat({S(n->accessibility), S(" ")});
}

Doc* Printer::PrintFunctionTypeParameters() {
    Node* n = node();
    if (n->typeArguments()) return Print(Key::typeArguments);
    if (n->typeParameters()) return Print(Key::typeParameters);
    return nullptr;
}

Doc* Printer::AdjustClause(const Node* n, Doc* clause, bool force_space) {
    if (n->type == NT::EmptyStatement) return S(";");
    if (n->type == NT::BlockStatement || force_space) return Cat({S(" "), clause});
    return Indent(Cat({line(), clause}));
}

// ---------------------------------------------------------------- literal.js

static std::string PreferredQuote(std::string_view text, bool prefer_single) {
    const char preferred = prefer_single ? '\'' : '"';
    const char alternate = prefer_single ? '"' : '\'';
    size_t pc = 0, ac = 0;
    for (char c : text) {
        if (c == preferred) {
            pc++;
        } else if (c == alternate) {
            ac++;
        }
    }
    return std::string(1, pc > ac ? alternate : preferred);
}

static std::string MakeString(std::string_view raw, char enclosing) {
    const char other = enclosing == '"' ? '\'' : '"';
    std::string out;
    out.reserve(raw.size() + 2);
    out += enclosing;
    for (size_t i = 0; i < raw.size(); i++) {
        const char c = raw[i];
        if (c == '\\' && i + 1 < raw.size()) {
            const char e = raw[i + 1];
            if (e == other) {
                out += e;
            } else {
                out += '\\';
                out += e;
            }
            i++;
            continue;
        }
        if (c == enclosing) {
            out += '\\';
            out += c;
            continue;
        }
        out += c;
    }
    out += enclosing;
    return out;
}

std::string Printer::PrintString(std::string_view raw) const {
    const std::string_view content = raw.substr(1, raw.size() - 2);
    const std::string enclosing = PreferredQuote(content, opts_.single_quote);
    if (raw[0] == enclosing[0]) return std::string(raw);
    return MakeString(content, enclosing[0]);
}

bool Printer::IsDirectiveLiteral() const {
    if (key() != Key::expression) return false;
    Node* p = parent();
    return p && p->type == NT::ExpressionStatement && p->Has(jsparse::kFlagHasDirective);
}

Doc* Printer::PrintLiteral() {
    Node* n = node();
    switch (n->lit) {
        case jsparse::LitKind::RegExp: {
            std::string flags(n->str3);
            std::sort(flags.begin(), flags.end());
            return a_.Own("/" + std::string(n->str2) + "/" + flags);
        }
        case jsparse::LitKind::BigInt: {
            std::string s(Raw(n));
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return a_.Own(std::move(s));
        }
        case jsparse::LitKind::Number:
            return a_.Own(PrintNumber(Raw(n)));
        case jsparse::LitKind::String: {
            const std::string_view raw = Raw(n);
            if (IsDirectiveLiteral()) {
                const std::string_view content = raw.substr(1, raw.size() - 2);
                if (content.find('"') != std::string_view::npos || content.find('\'') != std::string_view::npos) {
                    return S(raw);
                }
                const char q = opts_.single_quote ? '\'' : '"';
                return a_.Own(std::string(1, q) + std::string(content) + std::string(1, q));
            }
            std::string s = PrintString(raw);
            if (s == raw) return a_.ReplaceEndOfLine(raw);
            return a_.ReplaceEndOfLine(a_.Own(std::move(s))->str);
        }
        case jsparse::LitKind::Null:
            return S("null");
        case jsparse::LitKind::Bool:
            return S(n->Has(jsparse::kFlagBoolValue) ? "true" : "false");
        default:
            return S(Raw(n));
    }
}

// ---------------------------------------------------------------- statement.js, block.js

Doc* Printer::PrintStatementSequence(Key k) {
    const NodeList* list = ListOf(node(), k);
    Parts parts;
    if (!list) return Cat(parts);
    Node* last = nullptr;
    for (uint32_t i = list->size(); i-- > 0;) {
        if ((*list)[i]->type != NT::EmptyStatement) {
            last = (*list)[i];
            break;
        }
    }
    Each(k, [&](int) {
        Node* s = node();
        if (s->type == NT::EmptyStatement) return;
        parts.push_back(Print());
        if (s != last) {
            parts.push_back(hardline());
            if (IsNextLineEmpty(s)) parts.push_back(hardline());
        }
    });
    return Cat(parts);
}

Doc* Printer::PrintBlockBody() {
    Node* n = node();
    bool has_body = false;
    for (Node* s : n->body_list()) {
        if (s->type != NT::EmptyStatement) {
            has_body = true;
            break;
        }
    }
    const bool has_dangling = HasComment(n, kCDangling);
    if (!has_body && !has_dangling) return nullptr;
    Parts parts;
    if (has_body) parts.push_back(PrintStatementSequence(Key::body));
    if (has_dangling) parts.push_back(PrintDanglingComments());
    return Cat(parts);
}

Doc* Printer::PrintBlock() {
    Node* n = node();
    Doc* body = PrintBlockBody();
    if (n->type == NT::Program) return body ? Cat({body, hardline()}) : nullptr;
    Parts parts;
    if (n->type == NT::StaticBlock) parts.push_back(S("static "));
    parts.push_back(S("{"));
    if (body) {
        parts.push_back(Indent(Cat({hardline(), body})));
        parts.push_back(hardline());
    } else {
        Node* p = parent();
        Node* gp = grandparent();
        if (!(p->type == NT::ArrowFunctionExpression || p->type == NT::FunctionExpression ||
              p->type == NT::FunctionDeclaration || p->type == NT::ForStatement || p->type == NT::WhileStatement ||
              p->type == NT::DoWhileStatement || (p->type == NT::CatchClause && !gp->finalizer()) ||
              p->type == NT::TSModuleDeclaration || n->type == NT::StaticBlock)) {
            parts.push_back(hardline());
        }
    }
    parts.push_back(S("}"));
    return Cat(parts);
}

Doc* Printer::PrintExpressionStatement() {
    Doc* e = Print(Key::expression);
    return opts_.semi ? Cat({e, S(";")}) : Cat({e});
}

// ---------------------------------------------------------------- estree.js

Doc* Printer::PrintEstree(const Args* a) {
    Node* n = node();
    if (n->type == NT::Literal) return PrintLiteral();
    Doc* semi = opts_.semi ? S(";") : nullptr;
    switch (n->type) {
        case NT::EmptyStatement:
            return nullptr;
        case NT::ExpressionStatement:
            return PrintExpressionStatement();
        case NT::ChainExpression:
            return Print(Key::expression);
        case NT::ParenthesizedExpression: {
            Node* e = n->expression();
            if (!HasComment(e) && (IsObjectExpression(e) || IsArrayExpression(e))) {
                return Cat({S("("), Print(Key::expression), S(")")});
            }
            return Group(Cat({S("("), Indent(Cat({softline(), Print(Key::expression)})), softline(), S(")")}));
        }
        case NT::AssignmentExpression:
            return PrintAssignmentExpression();
        case NT::VariableDeclarator:
            return PrintVariableDeclarator();
        case NT::BinaryExpression:
        case NT::LogicalExpression:
            return PrintBinaryish();
        case NT::AssignmentPattern:
            return Cat({Print(Key::left), S(" = "), Print(Key::right)});
        case NT::MemberExpression:
            return PrintMemberExpression();
        case NT::MetaProperty:
            return Cat({Print(Key::meta), S("."), Print(Key::property)});
        case NT::Identifier:
            return Cat({S(n->str), PrintOptionalToken(), PrintDefiniteToken(), PrintTypeAnnotationProperty()});
        case NT::SpreadElement:
        case NT::RestElement:
            return Cat({S("..."), Print(Key::argument), PrintTypeAnnotationProperty()});
        case NT::Decorator:
            return Cat({S("@"), Print(Key::expression)});
        case NT::AccessorProperty:
            return PrintClassProperty();
        case NT::FunctionDeclaration:
        case NT::FunctionExpression:
            return PrintFunction(a);
        case NT::ArrowFunctionExpression:
            return PrintArrowFunction(a);
        case NT::YieldExpression: {
            Parts parts{S("yield")};
            if (n->Has(jsparse::kFlagDelegate)) parts.push_back(S("*"));
            if (n->argument()) {
                parts.push_back(S(" "));
                parts.push_back(Print(Key::argument));
            }
            return Cat(parts);
        }
        case NT::AwaitExpression:
            return PrintAwait();
        case NT::ExportDefaultDeclaration:
        case NT::ExportNamedDeclaration:
        case NT::ExportAllDeclaration:
            return PrintExportDeclaration();
        case NT::ImportDeclaration:
            return PrintImportDeclaration();
        case NT::ImportSpecifier:
        case NT::ExportSpecifier:
        case NT::ImportNamespaceSpecifier:
        case NT::ImportDefaultSpecifier:
            return PrintModuleSpecifier();
        case NT::ImportAttribute:
            return PrintProperty();
        case NT::Program:
        case NT::BlockStatement:
        case NT::StaticBlock:
            return PrintBlock();
        case NT::ClassBody:
            return PrintClassBody();
        case NT::ThrowStatement:
            return PrintReturnOrThrow("throw");
        case NT::ReturnStatement:
            return PrintReturnOrThrow("return");
        case NT::NewExpression:
        case NT::ImportExpression:
        case NT::CallExpression:
            return PrintCallExpression();
        case NT::ObjectExpression:
        case NT::ObjectPattern:
            return PrintObject();
        case NT::Property:
            if (IsMethod(n)) return PrintMethod();
            return PrintProperty();
        case NT::ArrayExpression:
        case NT::ArrayPattern:
            return PrintArray();
        case NT::SequenceExpression:
            return PrintSequence();
        case NT::ThisExpression:
            return S("this");
        case NT::Super:
            return S("super");
        case NT::UnaryExpression:
            return PrintUnary();
        case NT::UpdateExpression:
            if (n->Has(jsparse::kFlagPrefix)) return Cat({S(n->op), Print(Key::argument)});
            return Cat({Print(Key::argument), S(n->op)});
        case NT::ConditionalExpression:
            return PrintTernary();
        case NT::VariableDeclaration:
            return PrintVariableDeclaration();
        case NT::WithStatement:
            return Group(
                Cat({S("with ("), Print(Key::object), S(")"), AdjustClause(n->body(), Print(Key::body))}));
        case NT::IfStatement:
            return PrintIf();
        case NT::ForStatement:
            return PrintFor();
        case NT::WhileStatement:
            return Group(Cat({S("while ("), Group(Cat({Indent(Cat({softline(), Print(Key::test)})), softline()})),
                              S(")"), AdjustClause(n->body(), Print(Key::body))}));
        case NT::ForInStatement:
            return Group(Cat({S("for ("), Print(Key::left), S(" in "), Print(Key::right), S(")"),
                              AdjustClause(n->body(), Print(Key::body))}));
        case NT::ForOfStatement:
            return Group(Cat({S("for"), n->Has(jsparse::kFlagAwait) ? S(" await") : nullptr, S(" ("),
                              Print(Key::left), S(" of "), Print(Key::right), S(")"),
                              AdjustClause(n->body(), Print(Key::body))}));
        case NT::DoWhileStatement: {
            Doc* clause = AdjustClause(n->body(), Print(Key::body));
            Parts parts{Group(Cat({S("do"), clause}))};
            parts.push_back(n->body()->type == NT::BlockStatement ? S(" ") : hardline());
            parts.push_back(S("while ("));
            parts.push_back(Group(Cat({Indent(Cat({softline(), Print(Key::test)})), softline()})));
            parts.push_back(S(")"));
            parts.push_back(semi);
            return Cat(parts);
        }
        case NT::BreakStatement:
        case NT::ContinueStatement: {
            Parts parts{S(n->type == NT::BreakStatement ? "break" : "continue")};
            if (n->label()) {
                parts.push_back(S(" "));
                parts.push_back(Print(Key::label));
            }
            parts.push_back(semi);
            return Cat(parts);
        }
        case NT::LabeledStatement:
            if (n->body()->type == NT::EmptyStatement) return Cat({Print(Key::label), S(":;")});
            return Cat({Print(Key::label), S(": "), Print(Key::body)});
        case NT::TryStatement:
            return Cat({S("try "), Print(Key::block),
                        n->handler() ? Cat({S(" "), Print(Key::handler)}) : nullptr,
                        n->finalizer() ? Cat({S(" finally "), Print(Key::finalizer)}) : nullptr});
        case NT::CatchClause:
            return PrintCatch();
        case NT::SwitchStatement:
            return PrintSwitch();
        case NT::SwitchCase:
            return PrintSwitchCase();
        case NT::DebuggerStatement:
            return Cat({S("debugger"), semi});
        case NT::ClassDeclaration:
        case NT::ClassExpression:
            return PrintClass();
        case NT::MethodDefinition:
            return PrintClassMethod();
        case NT::PropertyDefinition:
            return PrintClassProperty();
        case NT::TemplateElement:
            return a_.ReplaceEndOfLine(Raw(n));
        case NT::TemplateLiteral:
            return PrintTemplateLiteral();
        case NT::TaggedTemplateExpression:
            return PrintTaggedTemplateLiteral();
        case NT::PrivateIdentifier:
            return Cat({S("#"), S(n->str)});
        default:
            if (n->type >= NT::Decorator) return PrintTypescript(a);
            throw UnsupportedNode{n};
    }
}

Doc* Printer::PrintAwait() {
    Node* n = node();
    Parts parts{S("await")};
    if (n->argument()) {
        parts.push_back(S(" "));
        parts.push_back(Print(Key::argument));
        Node* p = parent();
        if ((IsCallExpression(p) && p->callee() == n) || (IsMemberExpression(p) && p->object() == n)) {
            Parts inner{softline()};
            inner.insert(inner.end(), parts.begin(), parts.end());
            Parts wrapped{Indent(Cat(inner)), softline()};
            Node* anc = FindAncestor(
                [](Node* x) { return x->type == NT::AwaitExpression || x->type == NT::BlockStatement; });
            if (!Is(anc, NT::AwaitExpression) ||
                !StartsWithNoLookaheadToken(anc->argument(), [&](const Node* lm) { return lm == n; })) {
                return Group(Cat(wrapped));
            }
            return Cat(wrapped);
        }
    }
    return Cat(parts);
}

Doc* Printer::PrintSequence() {
    Node* p = parent();
    if (p->type == NT::ExpressionStatement || p->type == NT::ForStatement) {
        Parts parts;
        Each(Key::expressions, [&](int i) {
            if (i == 0) {
                parts.push_back(Print());
            } else {
                parts.push_back(S(","));
                parts.push_back(Indent(Cat({line(), Print()})));
            }
        });
        return Group(Cat(parts));
    }
    Doc* parts = a_.Join(Cat({S(","), line()}), PrintAll(Key::expressions));
    const Key k = key();
    if (((p->type == NT::ReturnStatement || p->type == NT::ThrowStatement) && k == Key::argument) ||
        (p->type == NT::ArrowFunctionExpression && k == Key::body)) {
        return Group(IfBreak(Cat({Indent(Cat({softline(), parts})), softline()}), parts));
    }
    return Group(parts);
}

Doc* Printer::PrintUnary() {
    Node* n = node();
    Parts parts{S(n->op)};
    if (!n->op.empty() && n->op.back() >= 'a' && n->op.back() <= 'z') parts.push_back(S(" "));
    if (HasComment(n->argument())) {
        parts.push_back(Group(Cat({S("("), Indent(Cat({softline(), Print(Key::argument)})), softline(), S(")")})));
    } else {
        parts.push_back(Print(Key::argument));
    }
    return Cat(parts);
}

Doc* Printer::PrintVariableDeclaration() {
    Node* n = node();
    Parts printed = PrintAll(Key::declarations);
    Node* p = parent();
    const bool parent_for =
        p->type == NT::ForStatement || p->type == NT::ForInStatement || p->type == NT::ForOfStatement;
    bool has_value = false;
    for (Node* d : n->declarations()) {
        if (d->init()) has_value = true;
    }
    Doc* first = nullptr;
    bool have_first = false;
    if (printed.size() == 1 && !HasComment(n->declarations()[0])) {
        first = printed[0];
        have_first = true;
    } else if (!printed.empty()) {
        first = Indent(printed[0]);
        have_first = true;
    }
    Parts rest;
    for (size_t i = 1; i < printed.size(); i++) {
        rest.push_back(Cat({S(","), has_value && !parent_for ? hardline() : line(), printed[i]}));
    }
    Parts parts{PrintDeclareToken(), S(n->kind), have_first ? Cat({S(" "), first}) : nullptr, Indent(Cat(rest))};
    if (!(parent_for && p->body() != n)) parts.push_back(opts_.semi ? S(";") : nullptr);
    return Group(Cat(parts));
}

Doc* Printer::PrintIf() {
    Node* n = node();
    Doc* consequent = AdjustClause(n->consequent(), Print(Key::consequent));
    Doc* opening = Group(Cat({S("if ("), Group(Cat({Indent(Cat({softline(), Print(Key::test)})), softline()})),
                              S(")"), consequent}));
    Parts parts{opening};
    if (n->alternate()) {
        const bool comment_on_own_line =
            HasComment(n->consequent(), kCTrailing | kCLine) || NeedsHardlineAfterDanglingComment(n);
        const bool else_same_line = n->consequent()->type == NT::BlockStatement && !comment_on_own_line;
        parts.push_back(else_same_line ? S(" ") : hardline());
        if (HasComment(n, kCDangling)) {
            parts.push_back(PrintDanglingComments());
            parts.push_back(comment_on_own_line ? hardline() : S(" "));
        }
        parts.push_back(S("else"));
        parts.push_back(Group(AdjustClause(n->alternate(), Print(Key::alternate),
                                           n->alternate()->type == NT::IfStatement)));
    }
    return Cat(parts);
}

Doc* Printer::PrintFor() {
    Node* n = node();
    Doc* body = AdjustClause(n->body(), Print(Key::body));
    Doc* dangling = PrintDanglingComments();
    Doc* printed_comments = dangling ? Cat({dangling, softline()}) : nullptr;
    if (!n->init() && !n->test() && !n->update()) return Cat({printed_comments, Group(Cat({S("for (;;)"), body}))});
    return Cat({printed_comments,
                Group(Cat({S("for ("),
                           Group(Cat({Indent(Cat({softline(), Print(Key::init), S(";"), line(), Print(Key::test),
                                                  S(";"), line(), Print(Key::update)})),
                                      softline()})),
                           S(")"), body}))});
}

Doc* Printer::PrintCatch() {
    Node* n = node();
    if (n->param()) {
        const bool param_has_comments = HasComment(n->param(), 0, [&](CommentRec* c) {
            return !IsBlockComment(c) || (c->leading && HasNewline(c->end)) ||
                   (c->trailing && HasNewline(c->start, true));
        });
        Doc* param = Print(Key::param);
        return Cat({S("catch "),
                    param_has_comments ? Cat({S("("), Indent(Cat({softline(), param})), softline(), S(") ")})
                                       : Cat({S("("), param, S(") ")}),
                    Print(Key::body)});
    }
    return Cat({S("catch "), Print(Key::body)});
}

Doc* Printer::PrintSwitch() {
    Node* n = node();
    Parts cases;
    Each(Key::cases, [&](int) {
        Node* c = node();
        cases.push_back(Cat({Print(), !IsLast() && IsNextLineEmpty(c) ? hardline() : nullptr}));
    });
    return Cat({Group(Cat({S("switch ("), Indent(Cat({softline(), Print(Key::discriminant)})), softline(), S(")")})),
                S(" {"), n->cases().empty() ? nullptr : Indent(Cat({hardline(), a_.Join(hardline(), cases)})),
                hardline(), S("}")});
}

Doc* Printer::PrintSwitchCase() {
    Node* n = node();
    Parts parts;
    if (n->test()) {
        parts.push_back(S("case "));
        parts.push_back(Print(Key::test));
        parts.push_back(S(":"));
    } else {
        parts.push_back(S("default:"));
    }
    if (HasComment(n, kCDangling)) {
        parts.push_back(S(" "));
        parts.push_back(PrintDanglingComments());
    }
    std::vector<Node*> consequent;
    for (Node* s : n->consequent_list()) {
        if (s->type != NT::EmptyStatement) consequent.push_back(s);
    }
    if (!consequent.empty()) {
        Doc* cons = PrintStatementSequence(Key::consequent);
        parts.push_back(consequent.size() == 1 && consequent[0]->type == NT::BlockStatement
                            ? Cat({S(" "), cons})
                            : Indent(Cat({hardline(), cons})));
    }
    return Cat(parts);
}

// ---------------------------------------------------------------- function.js

bool Printer::ReturnArgumentHasLeadingComment(const Node* arg) {
    if (HasLeadingOwnLineComment(arg) ||
        (HasComment(arg, kCLeading, [&](CommentRec* c) { return HasNewlineInRange(c->start, c->end); }) &&
         !IsJsxElement(arg))) {
        return true;
    }
    if (HasNakedLeftSide(arg)) {
        const Node* left_most = arg;
        const Node* next;
        while ((next = GetLeftSide(left_most))) {
            left_most = next;
            if (HasLeadingOwnLineComment(left_most)) return true;
        }
    }
    return false;
}

Doc* Printer::PrintReturnOrThrow(std::string_view keyword) {
    Node* n = node();
    Doc* semi = opts_.semi ? S(";") : nullptr;
    Parts parts{S(keyword)};
    if (n->argument()) {
        Doc* arg = Print(Key::argument);
        if (ReturnArgumentHasLeadingComment(n->argument())) {
            arg = Cat({S("("), Indent(Cat({hardline(), arg})), hardline(), S(")")});
        } else if (IsBinaryish(n->argument())) {
            arg = Group(Cat({IfBreak(S("(")), Indent(Cat({softline(), arg})), softline(), IfBreak(S(")"))}));
        }
        parts.push_back(S(" "));
        parts.push_back(arg);
    }
    const bool has_dangling = HasComment(n, kCDangling);
    const bool semi_before = semi && has_dangling && HasComment(n, kCLast | kCLine);
    if (semi_before) parts.push_back(semi);
    if (has_dangling) {
        parts.push_back(S(" "));
        parts.push_back(PrintDanglingComments());
    }
    if (!semi_before) parts.push_back(semi);
    return Cat(parts);
}

static bool IsMethodValuePath(const Node* n, Key k, const Node* p) {
    return k == Key::value && n->type == NT::FunctionExpression &&
           (p->type == NT::MethodDefinition || p->type == NT::TSAbstractMethodDefinition ||
            (p->type == NT::Property && IsMethod(p)));
}

Doc* Printer::PrintReturnType() { return PrintTypeAnnotationProperty(Key::returnType); }

Doc* Printer::PrintFunction(const Args* a) {
    Node* n = node();
    if (IsMethodValuePath(n, key(), parent())) return PrintMethodValue();
    bool expand_arg = false;
    if ((n->type == NT::FunctionDeclaration || n->type == NT::FunctionExpression) && a && a->expand_last) {
        Node* p = parent();
        if (IsCallExpression(p)) {
            bool all_ident = true;
            for (Node* q : n->params()) {
                if (q->type != NT::Identifier || q->typeAnnotation()) all_ident = false;
            }
            if (GetCallArguments(p).size() > 1 || all_ident) expand_arg = true;
        }
    }
    Parts parts{PrintDeclareToken(), n->Has(jsparse::kFlagAsync) ? S("async ") : nullptr,
                S(n->Has(jsparse::kFlagGenerator) ? "function* " : "function "),
                n->id() ? Print(Key::id) : nullptr};
    Doc* params = PrintFunctionParameters(expand_arg, false);
    Doc* return_type = PrintReturnType();
    const bool group_params = ShouldGroupFunctionParameters(n, return_type);
    parts.push_back(PrintFunctionTypeParameters());
    parts.push_back(Group(Cat({group_params ? Group(params) : params, return_type})));
    parts.push_back(n->body() ? S(" ") : nullptr);
    parts.push_back(Print(Key::body));
    if (opts_.semi && (n->HasTs(jsparse::kTsDeclare) || !n->body())) parts.push_back(S(";"));
    return Cat(parts);
}

Doc* Printer::PrintMethod() {
    Node* n = node();
    Node* value = n->value() ? n->value() : n;
    Parts parts;
    const std::string_view kind = n->kind;
    if (kind.empty() || kind == "init" || kind == "method" || kind == "constructor") {
        if (value->Has(jsparse::kFlagAsync)) parts.push_back(S("async "));
    } else {
        parts.push_back(S(kind));
        parts.push_back(S(" "));
    }
    if (value->Has(jsparse::kFlagGenerator)) parts.push_back(S("*"));
    parts.push_back(PrintPropertyKey());
    parts.push_back(n->HasTs(jsparse::kTsOptional) ? S("?") : nullptr);
    parts.push_back(n == value ? PrintMethodValue() : Print(Key::value));
    return Cat(parts);
}

Doc* Printer::PrintMethodValue() {
    Node* n = node();
    Doc* params = PrintFunctionParameters(false, false);
    Doc* return_type = PrintReturnType();
    const bool should_break = ShouldBreakFunctionParameters(n);
    const bool group_params = ShouldGroupFunctionParameters(n, return_type);
    Parts parts{PrintFunctionTypeParameters(),
                Group(Cat({should_break ? Group(params, true) : group_params ? Group(params) : params, return_type}))};
    if (n->body()) {
        parts.push_back(S(" "));
        parts.push_back(Print(Key::body));
    } else {
        parts.push_back(opts_.semi ? S(";") : nullptr);
    }
    return Cat(parts);
}

// ---------------------------------------------------------------- property.js

static bool IsSimpleNumber(std::string_view s) {
    // ^(?:\d+|\d+\.\d+)$
    size_t i = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
    if (i == 0) return false;
    if (i == s.size()) return true;
    if (s[i] != '.') return false;
    size_t j = i + 1;
    while (j < s.size() && s[j] >= '0' && s[j] <= '9') j++;
    return j > i + 1 && j == s.size();
}

static bool IsEs5IdentifierName(std::string_view s) {
    if (s.empty()) return false;
    for (size_t i = 0; i < s.size(); i++) {
        const auto c = static_cast<unsigned char>(s[i]);
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '$' || c == '_' ||
                        (i > 0 && c >= '0' && c <= '9');
        if (!ok) return false;
    }
    return true;
}

// JavaScript's String(Number(s)) for a simple decimal string.
static std::string JsNumberString(std::string_view s) {
    double v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc()) return "";
    if (std::isinf(v)) return "Infinity";
    if (v == 0) return "0";
    char buf[64];
    // shortest round-trip digits in scientific form: d.ddde[+-]x
    const auto res = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::scientific);
    std::string sci(buf, res.ptr);
    const size_t e = sci.find('e');
    std::string digits;
    for (size_t i = 0; i < e; i++) {
        if (sci[i] != '.') digits += sci[i];
    }
    const int exp = std::atoi(sci.c_str() + e + 1);
    const int k = static_cast<int>(digits.size());
    const int n = exp + 1;
    std::string out;
    if (k <= n && n <= 21) {
        out = digits + std::string(static_cast<size_t>(n - k), '0');
    } else if (0 < n && n <= 21) {
        out = digits.substr(0, static_cast<size_t>(n)) + "." + digits.substr(static_cast<size_t>(n));
    } else if (-6 < n && n <= 0) {
        out = "0." + std::string(static_cast<size_t>(-n), '0') + digits;
    } else {
        out = digits.substr(0, 1);
        if (k > 1) out += "." + digits.substr(1);
        out += "e";
        out += (n - 1 >= 0) ? "+" : "-";
        out += std::to_string(std::abs(n - 1));
    }
    return out;
}

bool Printer::IsStringKeySafeToUnquote(const Node* prop) const {
    const Node* k = prop->key();
    if (!IsStringLiteral(k)) return false;
    const std::string printed = PrintString(Raw(k));
    if (std::string_view(printed).substr(1, printed.size() - 2) != k->str2) return false;
    if (IsEs5IdentifierName(k->str2) && !(opts_.typescript && prop->type == NT::PropertyDefinition)) return true;
    if (IsSimpleNumber(k->str2) && JsNumberString(k->str2) == k->str2 && prop->type != NT::ImportAttribute &&
        !opts_.typescript) {
        return true;
    }
    return false;
}

Doc* Printer::PrintPropertyKey() {
    Node* n = node();
    if (n->Has(jsparse::kFlagComputed)) return Cat({S("["), Print(Key::key), S("]")});
    Node* k = n->key();
    if (IsStringKeySafeToUnquote(n)) {
        const std::string_view v = k->str2;
        Doc* d = (!v.empty() && v[0] >= '0' && v[0] <= '9') ? a_.Own(PrintNumber(v)) : S(v);
        return Call(Key::key, [&] { return PrintComments(d); });
    }
    return Print(Key::key);
}

Doc* Printer::PrintProperty() {
    Node* n = node();
    if (n->Has(jsparse::kFlagShorthand)) return Print(Key::value);
    return PrintAssignment(PrintPropertyKey(), S(":"), Key::value);
}

// ---------------------------------------------------------------- class.js

static size_t HeritageCount(const Node* n) { return (n->superClass() ? 1 : 0) + n->implements().size(); }

bool Printer::ShouldIndentOnlyHeritageClauses(const Node* n) {
    return n->typeParameters() && !HasComment(n->typeParameters(), kCTrailing | kCLine) && HeritageCount(n) <= 1;
}

Doc* Printer::PrintHeritageClauses(Key list_key, std::string_view list_name) {
    Node* n = node();
    const NodeList* list = ListOf(n, list_key);
    if (!list || list->empty()) return nullptr;
    Doc* comments = PrintDanglingComments(false, nullptr, list_key);
    Parts printed = PrintAll(list_key);
    return Cat({ShouldIndentOnlyHeritageClauses(n) ? IfBreak(S(" "), line(), TypeParametersGroupId(n->typeParameters()))
                                                   : line(),
                comments, comments ? hardline() : nullptr, S(list_name),
                Group(Indent(Cat({line(), a_.Join(Cat({S(","), line()}), printed)})))});
}

Doc* Printer::PrintClass() {
    Node* n = node();
    Parts parts{PrintDeclareToken(), PrintAbstractToken(), S("class")};
    const bool group_mode = HasComment(n->id(), kCTrailing) || HasComment(n->typeParameters(), kCTrailing) ||
                            HasComment(n->superClass()) || !n->implements().empty();
    Parts parts_group, extends_parts;
    if (n->id()) {
        parts_group.push_back(S(" "));
        parts_group.push_back(Print(Key::id));
    }
    parts_group.push_back(Print(Key::typeParameters));
    if (n->superClass()) {
        Doc* printed = Print(Key::superClass);
        // printSuperClass
        if (parent()->type == NT::AssignmentExpression) {
            printed = Group(IfBreak(Cat({S("("), Indent(Cat({softline(), printed})), softline(), S(")")}), printed));
        }
        printed = Cat({printed, Print(Key::typeArguments)});  // superTypeArguments
        Doc* with_comments = Call(Key::superClass, [&] { return Cat({S("extends "), PrintComments(printed)}); });
        if (group_mode) {
            extends_parts.push_back(line());
            extends_parts.push_back(Group(with_comments));
        } else {
            extends_parts.push_back(S(" "));
            extends_parts.push_back(with_comments);
        }
    }
    extends_parts.push_back(PrintHeritageClauses(Key::implements, "implements"));
    GroupId heritage_id = 0;
    if (group_mode) {
        Doc* printed_group;
        if (ShouldIndentOnlyHeritageClauses(n)) {
            Parts g = parts_group;
            g.push_back(Indent(Cat(extends_parts)));
            printed_group = Cat(g);
        } else {
            Parts g = parts_group;
            g.push_back(Cat(extends_parts));
            printed_group = Indent(Cat(g));
        }
        heritage_id = HeritageGroupId(n);
        parts.push_back(Group(printed_group, false, heritage_id));
    } else {
        parts.insert(parts.end(), parts_group.begin(), parts_group.end());
        parts.insert(parts.end(), extends_parts.begin(), extends_parts.end());
    }
    if (group_mode && !n->body()->body_list().empty()) {
        parts.push_back(IfBreak(hardline(), S(" "), heritage_id));
    } else {
        parts.push_back(S(" "));
    }
    parts.push_back(Print(Key::body));
    return Cat(parts);
}

GroupId Printer::HeritageGroupId(const Node* n) {
    auto it = heritage_ids_.find(n);
    if (it != heritage_ids_.end()) return it->second;
    return heritage_ids_[n] = a_.NewId();
}

GroupId Printer::TypeParametersGroupId(const Node* n) {
    auto it = type_params_ids_.find(n);
    if (it != type_params_ids_.end()) return it->second;
    return type_params_ids_[n] = a_.NewId();
}

Doc* Printer::PrintClassMethod() {
    Node* n = node();
    Parts parts;
    if (!n->decorators().empty()) parts.push_back(PrintClassMemberDecorators());
    parts.push_back(PrintAccessibilityToken());
    if (n->Has(jsparse::kFlagStatic)) parts.push_back(S("static "));
    parts.push_back(PrintAbstractToken());
    if (n->HasTs(jsparse::kTsOverride)) parts.push_back(S("override "));
    parts.push_back(PrintMethod());
    return Cat(parts);
}

static bool IsAbstractProperty(const Node* n) {
    return n->type == NT::TSAbstractPropertyDefinition || n->type == NT::TSAbstractAccessorProperty;
}

Doc* Printer::PrintClassProperty() {
    Node* n = node();
    Parts parts;
    if (!n->decorators().empty()) parts.push_back(PrintClassMemberDecorators());
    parts.push_back(PrintDeclareToken());
    parts.push_back(PrintAccessibilityToken());
    if (n->Has(jsparse::kFlagStatic)) parts.push_back(S("static "));
    parts.push_back(PrintAbstractToken());
    if (n->HasTs(jsparse::kTsOverride)) parts.push_back(S("override "));
    if (n->HasTs(jsparse::kTsReadonly)) parts.push_back(S("readonly "));
    if (n->type == NT::AccessorProperty || n->type == NT::TSAbstractAccessorProperty) parts.push_back(S("accessor "));
    parts.push_back(PrintPropertyKey());
    parts.push_back(PrintOptionalToken());
    parts.push_back(PrintDefiniteToken());
    parts.push_back(PrintTypeAnnotationProperty());
    return Cat({PrintAssignment(Cat(parts), S(" ="), IsAbstractProperty(n) ? Key::None : Key::value),
                opts_.semi ? S(";") : nullptr});
}

static bool IsClassPropertyNode(const Node* n) {
    return n->type == NT::PropertyDefinition || n->type == NT::AccessorProperty ||
           n->type == NT::TSAbstractPropertyDefinition || n->type == NT::TSAbstractAccessorProperty;
}

static bool ShouldPrintSemicolonAfterClassProperty(const Node* n, const Node* next) {
    const Node* k = n->key();
    if (!n->Has(jsparse::kFlagComputed) && Is(k, NT::Identifier) &&
        (k->str == "static" || k->str == "get" || k->str == "set") && !n->value() && !n->typeAnnotation()) {
        return true;
    }
    if (!next) return false;
    if (next->Has(jsparse::kFlagStatic) || !next->accessibility.empty() || next->HasTs(jsparse::kTsReadonly)) {
        return false;
    }
    if (!next->Has(jsparse::kFlagComputed)) {
        const Node* nk = next->key();
        if (Is(nk, NT::Identifier) && (nk->str == "in" || nk->str == "instanceof")) return true;
    }
    switch (next->type) {
        case NT::PropertyDefinition:
        case NT::TSAbstractPropertyDefinition:
            return next->Has(jsparse::kFlagComputed);
        case NT::MethodDefinition:
        case NT::TSAbstractMethodDefinition: {
            const Node* v = next->value();
            if ((v && v->Has(jsparse::kFlagAsync)) || next->kind == "get" || next->kind == "set") return false;
            return next->Has(jsparse::kFlagComputed) || (v && v->Has(jsparse::kFlagGenerator));
        }
        case NT::TSIndexSignature:
            return true;
        default:
            return false;
    }
}

Doc* Printer::PrintClassBody() {
    Node* n = node();
    Parts parts;
    Each(Key::body, [&](int) {
        Node* m = node();
        parts.push_back(Print());
        if (!opts_.semi && IsClassPropertyNode(m) && ShouldPrintSemicolonAfterClassProperty(m, Next())) {
            parts.push_back(S(";"));
        }
        if (!IsLast()) {
            parts.push_back(hardline());
            if (IsNextLineEmpty(m)) parts.push_back(hardline());
        }
    });
    if (HasComment(n, kCDangling)) parts.push_back(PrintDanglingComments());
    return Cat({S("{"), parts.empty() ? nullptr : Cat({Indent(Cat({hardline(), Cat(parts)})), hardline()}), S("}")});
}

// ---------------------------------------------------------------- module.js

bool Printer::ShouldPrintSpecifiers(const Node* n) const {
    if (n->type != NT::ImportDeclaration || !n->specifiers().empty() || n->HasTs(jsparse::kTsTypeOnly)) return true;
    const std::string t = GetTextWithoutComments(n->start, n->source()->start);
    std::string_view tv = t;
    while (!tv.empty() && (tv.back() == ' ' || tv.back() == '\n' || tv.back() == '\t' || tv.back() == '\r')) {
        tv.remove_suffix(1);
    }
    return tv.size() >= 4 && tv.substr(tv.size() - 4) == "from";
}

Doc* Printer::PrintModuleSource() {
    Node* n = node();
    if (!n->source()) return nullptr;
    Parts parts;
    if (ShouldPrintSpecifiers(n)) parts.push_back(S(" from"));
    parts.push_back(S(" "));
    parts.push_back(Print(Key::source));
    return Cat(parts);
}

Doc* Printer::PrintModuleSpecifiers() {
    Node* n = node();
    if (!ShouldPrintSpecifiers(n)) return nullptr;
    Parts parts{S(" ")};
    if (!n->specifiers().empty()) {
        Parts standalone, grouped;
        Each(Key::specifiers, [&](int) {
            const NT t = node()->type;
            if (t == NT::ImportNamespaceSpecifier || t == NT::ImportDefaultSpecifier) {
                standalone.push_back(Print());
            } else {
                grouped.push_back(Print());
            }
        });
        parts.push_back(a_.Join(S(", "), standalone));
        if (!grouped.empty()) {
            if (!standalone.empty()) parts.push_back(S(", "));
            bool any_comment = false;
            for (Node* s : n->specifiers()) {
                if (HasComment(s)) any_comment = true;
            }
            const bool can_break = grouped.size() > 1 || !standalone.empty() || any_comment;
            Doc* bl = opts_.bracket_spacing ? line() : softline();
            if (can_break) {
                parts.push_back(Group(Cat({S("{"), Indent(Cat({bl, a_.Join(Cat({S(","), line()}), grouped)})),
                                           IfBreak(S(",")), bl, S("}")})));
            } else {
                Parts g{S("{"), opts_.bracket_spacing ? S(" ") : nullptr};
                g.insert(g.end(), grouped.begin(), grouped.end());
                g.push_back(opts_.bracket_spacing ? S(" ") : nullptr);
                g.push_back(S("}"));
                parts.push_back(Cat(g));
            }
        }
    } else {
        parts.push_back(S("{}"));
    }
    return Cat(parts);
}

Doc* Printer::PrintImportAttributes() {
    Node* n = node();
    if (!n->source()) return nullptr;
    const NodeList& attrs = n->attributes();
    const std::string between = GetTextWithoutComments(n->source()->end, attrs.empty() ? n->end : attrs[0]->start);
    const std::string_view t = TrimJs(between);
    std::string_view keyword;
    if (t.substr(0, 6) == "assert") {
        keyword = "assert";
    } else if (t.substr(0, 4) == "with") {
        keyword = "with";
    } else if (!attrs.empty()) {
        keyword = "with";
    }
    if (keyword.empty()) return nullptr;
    Parts parts{S(" "), S(keyword), S(" {")};
    if (!attrs.empty()) {
        if (opts_.bracket_spacing) parts.push_back(S(" "));
        parts.push_back(a_.Join(S(", "), PrintAll(Key::attributes)));
        if (opts_.bracket_spacing) parts.push_back(S(" "));
    }
    parts.push_back(S("}"));
    return Cat(parts);
}

Doc* Printer::PrintImportDeclaration() {
    Node* n = node();
    return Cat({S("import"), n->kind.empty() ? nullptr : Cat({S(" "), S(n->kind)}),
                n->HasTs(jsparse::kTsTypeOnly) ? S(" type") : nullptr, PrintModuleSpecifiers(), PrintModuleSource(),
                PrintImportAttributes(), opts_.semi ? S(";") : nullptr});
}

Doc* Printer::PrintExportDeclaration() {
    Node* n = node();
    const bool is_default = n->type == NT::ExportDefaultDeclaration;
    Parts parts{PrintDecoratorsBeforeExport(), PrintDeclareToken(), S("export"), is_default ? S(" default") : nullptr};
    if (HasComment(n, kCDangling)) {
        parts.push_back(S(" "));
        parts.push_back(PrintDanglingComments());
        if (NeedsHardlineAfterDanglingComment(n)) parts.push_back(hardline());
    }
    if (n->declaration()) {
        parts.push_back(S(" "));
        parts.push_back(Print(Key::declaration));
    } else {
        if (n->HasTs(jsparse::kTsTypeOnly)) parts.push_back(S(" type"));
        if (n->type == NT::ExportAllDeclaration) {
            parts.push_back(S(" *"));
            if (n->exported()) {
                parts.push_back(S(" as "));
                parts.push_back(Print(Key::exported));
            }
        } else {
            parts.push_back(PrintModuleSpecifiers());
        }
        parts.push_back(PrintModuleSource());
        parts.push_back(PrintImportAttributes());
    }
    if (opts_.semi) {
        const Node* d = n->declaration();
        if (!d || (is_default && d->type != NT::ClassDeclaration && d->type != NT::FunctionDeclaration &&
                   d->type != NT::TSInterfaceDeclaration && d->type != NT::TSDeclareFunction)) {
            parts.push_back(S(";"));
        }
    }
    return Cat(parts);
}

static bool HasSameLoc(const Node* a, const Node* b) { return a->start == b->start && a->end == b->end; }

Doc* Printer::PrintModuleSpecifier() {
    Node* n = node();
    const bool is_import = n->type == NT::ImportSpecifier || n->type == NT::ImportDefaultSpecifier ||
                           n->type == NT::ImportNamespaceSpecifier;
    const Key left_key = is_import ? Key::imported : Key::local;
    const Key right_key = is_import ? Key::local : Key::exported;
    Node* left_node = Child(n, left_key);
    Node* right_node = Child(n, right_key);
    Doc* left = nullptr;
    Doc* right = nullptr;
    if (n->type == NT::ImportNamespaceSpecifier) {
        left = S("*");
    } else if (left_node) {
        left = Print(left_key);
    }
    bool shorthand = false;
    if ((n->type == NT::ImportSpecifier || n->type == NT::ExportSpecifier) && left_node && right_node) {
        const Node* local = n->local();
        const Node* other = n->type == NT::ImportSpecifier ? n->imported() : n->exported();
        if (local->type == other->type && HasSameLoc(local, other)) {
            if (IsStringLiteral(local)) {
                shorthand = local->str2 == other->str2 && Raw(local) == Raw(other);
            } else if (local->type == NT::Identifier) {
                shorthand = local->str == other->str;
            }
        }
    }
    if (right_node && !shorthand) right = Print(right_key);
    const bool kind = (n->type == NT::ImportSpecifier || n->type == NT::ExportSpecifier) && n->HasTs(jsparse::kTsTypeOnly);
    return Cat({kind ? S("type ") : nullptr, left, left && right ? S(" as ") : nullptr, right});
}

}  // namespace jsfmt
