// Prettier 3.6.2's expression printers: language-js/print/{member,
// member-chain,call-expression,call-arguments,assignment,binaryish,
// ternary-old,arrow-function,function-parameters,object,array,
// template-literal}.js.

#include <algorithm>

#include "js_format_internal.h"

namespace jsfmt {

// ---------------------------------------------------------------- member.js

Doc* Printer::PrintMemberLookup() {
    Node* n = node();
    Doc* property = Print(Key::property);
    Doc* optional = PrintOptionalToken();
    if (!n->Has(jsparse::kFlagComputed)) return Cat({optional, S("."), property});
    if (!n->property() || IsNumericLiteral(n->property())) return Cat({optional, S("["), property, S("]")});
    return Group(Cat({optional, S("["), Indent(Cat({softline(), property})), softline(), S("]")}));
}

static bool IsCallExpressionWithArguments(const Printer& p, const Node* n) {
    if (Is(n, NT::ChainExpression) || Is(n, NT::TSNonNullExpression)) n = n->expression();
    return p.IsCallExpression(n) && !p.GetCallArguments(n).empty();
}

Doc* Printer::PrintMemberExpression() {
    Node* n = node();
    Doc* object_doc = Print(Key::object);
    Doc* lookup = PrintMemberLookup();
    Node* first_non_member =
        FindAncestor([&](Node* x) { return !(IsMemberExpression(x) || x->type == NT::TSNonNullExpression); });
    Node* first_non_chain = FindAncestor(
        [](Node* x) { return x->type != NT::ChainExpression && x->type != NT::TSNonNullExpression; });
    const bool should_inline =
        (first_non_member && (first_non_member->type == NT::NewExpression ||
                              (first_non_member->type == NT::AssignmentExpression &&
                               first_non_member->left()->type != NT::Identifier))) ||
        n->Has(jsparse::kFlagComputed) ||
        (n->object()->type == NT::Identifier && n->property()->type == NT::Identifier &&
         !IsMemberExpression(first_non_chain)) ||
        ((Is(first_non_chain, NT::AssignmentExpression) || Is(first_non_chain, NT::VariableDeclarator)) &&
         (IsCallExpressionWithArguments(*this, n->object()) || HasLabel(object_doc, kLabelMemberChain)));
    const uint32_t lbl = object_doc && object_doc->kind == DocKind::Label ? object_doc->label : 0;
    return a_.Label(lbl, Cat({object_doc, should_inline ? lookup : Group(Indent(Cat({softline(), lookup})))}));
}

// ---------------------------------------------------------------- call-expression.js

bool Printer::IsCommonsJsOrAmdModuleDefinition() {
    Node* n = node();
    if (n->type != NT::CallExpression || n->Has(jsparse::kFlagOptional)) return false;
    if (!Is(n->callee(), NT::Identifier)) return false;
    const auto args = GetCallArguments(n);
    if (n->callee()->str == "require") return (args.size() == 1 && IsStringLiteral(args[0])) || args.size() > 1;
    if (n->callee()->str == "define" && parent()->type == NT::ExpressionStatement) {
        return args.size() == 1 || (args.size() == 2 && args[0]->type == NT::ArrayExpression) ||
               (args.size() == 3 && IsStringLiteral(args[0]) && args[1]->type == NT::ArrayExpression);
    }
    return false;
}

Doc* Printer::PrintCallee() {
    Node* n = node();
    if (n->type == NT::ImportExpression) return S("import");
    return Print(Key::callee);
}

Doc* Printer::PrintCallExpression() {
    Node* n = node();
    const bool is_new = n->type == NT::NewExpression;
    const bool is_import = n->type == NT::ImportExpression;
    Doc* optional = PrintOptionalToken();
    const auto args = GetCallArguments(n);
    const bool template_single = args.size() == 1 && IsTemplateOnItsOwnLine(args[0]);
    if (template_single || IsCommonsJsOrAmdModuleDefinition() || IsTestCall(n, parent())) {
        Parts printed;
        IterateCallArgumentsPath([&](int) { printed.push_back(MainPrint(nullptr)); });
        return Cat({is_new ? S("new ") : nullptr, PrintCallee(), optional, PrintFunctionTypeParameters(), S("("),
                    a_.Join(S(", "), printed), S(")")});
    }
    if (!is_import && !is_new && IsMemberish(n->callee())) {
        const bool callee_parens = Call(Key::callee, [&] {
            if (node()->type == NT::ChainExpression) return Call(Key::expression, [&] { return NeedsParens(); });
            return NeedsParens();
        });
        if (!callee_parens) return PrintMemberChain();
    }
    Parts contents{is_new ? S("new ") : nullptr, PrintCallee(), optional, PrintFunctionTypeParameters(),
                   PrintCallArguments()};
    if (is_import || IsCallExpression(n->callee())) return Group(Cat(contents));
    return Cat(contents);
}

// ---------------------------------------------------------------- member-chain.js

namespace {

struct PrintedNode {
    Node* node;
    Doc* printed;
    bool needs_parens = false;
    bool has_trailing_empty_line = false;
};

bool IsFactory(std::string_view name) {
    if (name.empty()) return false;
    if (name[0] >= 'A' && name[0] <= 'Z') return true;
    for (char c : name) {
        if (c != '$' && c != '_') return false;
    }
    return true;
}

}  // namespace

Doc* Printer::PrintMemberChain() {
    Node* top = node();
    if (top->type == NT::ChainExpression) return Call(Key::expression, [&] { return PrintMemberChain(); });
    Node* p = parent();
    const bool is_expression_statement =
        (p->type == NT::ChainExpression ? grandparent() : p)->type == NT::ExpressionStatement;

    std::vector<PrintedNode> printed_nodes;  // built in reverse, flipped at the end

    auto should_insert_empty_line_after = [&](const Node* n) {
        const int64_t next_idx = GetNextNonSpaceNonCommentCharacterIndex(n->end);
        if (CharAt(next_idx) == ')') return next_idx != kNoIndex && IsNextLineEmptyAfterIndex(next_idx + 1);
        return IsNextLineEmpty(n);
    };

    std::function<void()> rec = [&]() {
        Node* n = node();
        if (n->type == NT::ChainExpression) {
            Call(Key::expression, [&] {
                rec();
                return 0;
            });
            return;
        }
        if (IsCallExpression(n) && (IsMemberish(n->callee()) || IsCallExpression(n->callee()))) {
            const bool trailing_empty = should_insert_empty_line_after(n);
            Doc* d = PrintComments(Cat({PrintOptionalToken(), PrintFunctionTypeParameters(), PrintCallArguments()}));
            printed_nodes.push_back({n, Cat({d, trailing_empty ? hardline() : nullptr}), false, trailing_empty});
            Call(Key::callee, [&] {
                rec();
                return 0;
            });
        } else if (IsMemberish(n)) {
            const bool np = NeedsParens();
            printed_nodes.push_back({n, PrintComments(PrintMemberLookup()), np, false});
            Call(Key::object, [&] {
                rec();
                return 0;
            });
        } else if (n->type == NT::TSNonNullExpression) {
            printed_nodes.push_back({n, PrintComments(S("!")), false, false});
            Call(Key::expression, [&] {
                rec();
                return 0;
            });
        } else {
            printed_nodes.push_back({n, Print(), false, false});
        }
    };

    // Prettier unshifts the outer call, then the recursion unshifts ever
    // deeper nodes: [innermost, ..., outer call]. Push and reverse.
    Node* n = top;
    printed_nodes.push_back(
        {n, Cat({PrintOptionalToken(), PrintFunctionTypeParameters(), PrintCallArguments()}), false, false});
    if (n->callee()) {
        Call(Key::callee, [&] {
            rec();
            return 0;
        });
    }
    std::reverse(printed_nodes.begin(), printed_nodes.end());

    std::vector<std::vector<PrintedNode>> groups;
    std::vector<PrintedNode> current{printed_nodes[0]};
    size_t i = 1;
    for (; i < printed_nodes.size(); ++i) {
        const Node* x = printed_nodes[i].node;
        if (x->type == NT::TSNonNullExpression || IsCallExpression(x) ||
            (IsMemberExpression(x) && x->Has(jsparse::kFlagComputed) && IsNumericLiteral(x->property()))) {
            current.push_back(printed_nodes[i]);
        } else {
            break;
        }
    }
    if (!IsCallExpression(printed_nodes[0].node)) {
        for (; i + 1 < printed_nodes.size(); ++i) {
            if (IsMemberish(printed_nodes[i].node) && IsMemberish(printed_nodes[i + 1].node)) {
                current.push_back(printed_nodes[i]);
            } else {
                break;
            }
        }
    }
    groups.push_back(current);
    current.clear();
    bool has_seen_call = false;
    for (; i < printed_nodes.size(); ++i) {
        const Node* x = printed_nodes[i].node;
        if (has_seen_call && IsMemberish(x)) {
            if (x->Has(jsparse::kFlagComputed) && IsNumericLiteral(x->property())) {
                current.push_back(printed_nodes[i]);
                continue;
            }
            groups.push_back(current);
            current.clear();
            has_seen_call = false;
        }
        if (IsCallExpression(x) || x->type == NT::ImportExpression) has_seen_call = true;
        current.push_back(printed_nodes[i]);
        if (HasComment(x, kCTrailing)) {
            groups.push_back(current);
            current.clear();
            has_seen_call = false;
        }
    }
    if (!current.empty()) groups.push_back(current);

    auto should_not_wrap = [&]() {
        const bool has_computed =
            !groups[1].empty() && groups[1][0].node->Has(jsparse::kFlagComputed) &&
            groups[1][0].node->type == NT::MemberExpression;
        if (groups[0].size() == 1) {
            const Node* first = groups[0][0].node;
            return first->type == NT::ThisExpression ||
                   (first->type == NT::Identifier &&
                    (IsFactory(first->str) ||
                     (is_expression_statement &&
                      Utf16Length(first->str) <= static_cast<size_t>(opts_.indent)) ||
                     has_computed));
        }
        const Node* last = groups[0].back().node;
        return IsMemberExpression(last) && last->property()->type == NT::Identifier &&
               (IsFactory(last->property()->str) || has_computed);
    };
    const bool should_merge = groups.size() >= 2 && !HasComment(groups[1][0].node) && should_not_wrap();

    auto print_group = [&](const std::vector<PrintedNode>& g) -> Doc* {
        Parts ps;
        for (const auto& t : g) ps.push_back(t.printed);
        if (!g.empty() && g.back().needs_parens) {
            Parts wrapped{S("(")};
            wrapped.insert(wrapped.end(), ps.begin(), ps.end());
            wrapped.push_back(S(")"));
            return Cat(wrapped);
        }
        return Cat(ps);
    };
    auto print_indented_group = [&](size_t from) -> Doc* {
        if (from >= groups.size()) return nullptr;
        Parts gs;
        for (size_t k = from; k < groups.size(); k++) gs.push_back(print_group(groups[k]));
        return Indent(Cat({hardline(), a_.Join(hardline(), gs)}));
    };

    Parts printed_groups;
    for (const auto& g : groups) printed_groups.push_back(print_group(g));
    Doc* one_line = Cat(printed_groups);

    const size_t cutoff = should_merge ? 3 : 2;
    std::vector<PrintedNode> flat;
    for (const auto& g : groups) flat.insert(flat.end(), g.begin(), g.end());
    bool node_has_comment = false;
    for (size_t k = 1; k + 1 < flat.size(); k++) {
        if (HasComment(flat[k].node, kCLeading)) node_has_comment = true;
    }
    for (size_t k = 0; k + 1 < flat.size(); k++) {
        if (HasComment(flat[k].node, kCTrailing)) node_has_comment = true;
    }
    if (groups.size() > cutoff && !groups[cutoff].empty() && HasComment(groups[cutoff][0].node, kCLeading)) {
        node_has_comment = true;
    }

    bool any_trailing_empty = false;
    for (const auto& g : groups) {
        if (!g.empty() && g.back().has_trailing_empty_line) any_trailing_empty = true;
    }
    if (groups.size() <= cutoff && !node_has_comment && !any_trailing_empty) {
        if (IsLongCurriedCallExpression()) return one_line;
        return Group(one_line);
    }

    const Node* last_before_indent = groups[should_merge ? 1 : 0].back().node;
    const bool empty_line_before_indent =
        !IsCallExpression(last_before_indent) && should_insert_empty_line_after(last_before_indent);

    Doc* expanded = Cat({print_group(groups[0]), should_merge ? print_group(groups[1]) : nullptr,
                         empty_line_before_indent ? hardline() : nullptr, print_indented_group(should_merge ? 2 : 1)});

    std::vector<Node*> call_expressions;
    for (const auto& t : printed_nodes) {
        if (IsCallExpression(t.node)) call_expressions.push_back(t.node);
    }

    auto last_group_will_break_and_other_calls_have_fn_args = [&]() {
        const Node* last_node = groups.back().back().node;
        Doc* last_doc = printed_groups.back();
        if (!IsCallExpression(last_node) || !WillBreak(last_doc)) return false;
        for (size_t k = 0; k + 1 < call_expressions.size(); k++) {
            for (Node* arg : GetCallArguments(call_expressions[k])) {
                if (IsFunctionOrArrow(arg)) return true;
            }
        }
        return false;
    };

    bool complex_args = false;
    if (call_expressions.size() > 2) {
        for (Node* c : call_expressions) {
            for (Node* arg : GetCallArguments(c)) {
                if (!IsSimpleCallArgument(arg)) {
                    complex_args = true;
                    break;
                }
            }
            if (complex_args) break;
        }
    }
    bool head_breaks = false;
    for (size_t k = 0; k + 1 < printed_groups.size(); k++) {
        if (WillBreak(printed_groups[k])) head_breaks = true;
    }

    Doc* result;
    if (node_has_comment || complex_args || head_breaks || last_group_will_break_and_other_calls_have_fn_args()) {
        result = Group(expanded);
    } else {
        result = Cat({WillBreak(one_line) || empty_line_before_indent ? a_.break_parent : nullptr,
                      a_.ConditionalGroup({one_line, expanded})});
    }
    return a_.Label(kLabelMemberChain, result);
}

// ---------------------------------------------------------------- call-arguments.js

bool Printer::CouldExpandArg(const Node* arg, bool arrow_chain_recursion) {
    if (IsObjectExpression(arg) && (!arg->properties().empty() || HasComment(arg))) return true;
    if (IsArrayExpression(arg) && (!arg->elements().empty() || HasComment(arg))) return true;
    if ((arg->type == NT::TSTypeAssertion || IsBinaryCastExpression(arg)) && CouldExpandArg(arg->expression())) {
        return true;
    }
    if (arg->type == NT::FunctionExpression) return true;
    if (arg->type == NT::ArrowFunctionExpression) {
        const Node* b = arg->body();
        const Node* rt = arg->returnType();
        if (!(!rt || !rt->typeAnnotation() || rt->typeAnnotation()->type != NT::TSTypeReference ||
              (b->type == NT::BlockStatement &&
               (std::any_of(b->body_list().begin(), b->body_list().end(),
                            [](Node* x) { return x->type != NT::EmptyStatement; }) ||
                HasComment(b, kCDangling))))) {
            return false;
        }
        return b->type == NT::BlockStatement ||
               (b->type == NT::ArrowFunctionExpression && CouldExpandArg(b, true)) || IsObjectExpression(b) ||
               IsArrayExpression(b) ||
               (!arrow_chain_recursion && (IsCallExpression(b) || b->type == NT::ConditionalExpression)) ||
               IsJsxElement(b);
    }
    return false;
}

bool Printer::IsConciselyPrintedArray(const Node* n) {
    if (!IsArrayExpression(n) || n->elements().size() <= 1) return false;
    for (Node* e : n->elements()) {
        if (!e) return false;
        if (!(IsNumericLiteral(e) || (IsSignedNumericLiteral(e) && !HasComment(e->argument())))) return false;
        if (HasComment(e, kCTrailing | kCLine, [&](CommentRec* c) { return !HasNewline(c->start, true); })) {
            return false;
        }
    }
    return true;
}

bool Printer::ShouldExpandLastArg(const std::vector<Node*>& args, const Parts&) {
    const Node* last = args.back();
    const Node* pen = args.size() >= 2 ? args[args.size() - 2] : nullptr;
    return !HasComment(last, kCLeading) && !HasComment(last, kCTrailing) && CouldExpandArg(last) &&
           (!pen || pen->type != last->type) &&
           (args.size() != 2 || pen->type != NT::ArrowFunctionExpression || !IsArrayExpression(last)) &&
           !(args.size() > 1 && IsConciselyPrintedArray(last));
}

bool Printer::IsHopefullyShortCallArgument(const Node* n) {
    if (n->type == NT::ParenthesizedExpression) return IsHopefullyShortCallArgument(n->expression());
    if (IsBinaryCastExpression(n)) {
        const Node* t = n->typeAnnotation();
        if (t->type == NT::TSArrayType) {
            t = t->elementType();
            if (t->type == NT::TSArrayType) t = t->elementType();
        }
        if (t->type == NT::TSTypeReference && t->typeArguments() && t->typeArguments()->params().size() == 1) {
            t = t->typeArguments()->params()[0];
        }
        return IsSimpleType(t) && IsSimpleCallArgument(n->expression(), 1);
    }
    if (IsCallLikeExpression(n) && GetCallArguments(n).size() > 1) return false;
    if (IsBinaryish(n)) return IsSimpleCallArgument(n->left(), 1) && IsSimpleCallArgument(n->right(), 1);
    return IsRegExpLiteral(n) || IsSimpleCallArgument(n);
}

bool Printer::ShouldExpandFirstArg(const std::vector<Node*>& args) {
    if (args.size() != 2) return false;
    const Node* first = args[0];
    const Node* second = args[1];
    return !HasComment(first) &&
           (first->type == NT::FunctionExpression ||
            (first->type == NT::ArrowFunctionExpression && first->body()->type == NT::BlockStatement)) &&
           second->type != NT::FunctionExpression && second->type != NT::ArrowFunctionExpression &&
           second->type != NT::ConditionalExpression && IsHopefullyShortCallArgument(second) &&
           !CouldExpandArg(second);
}

bool Printer::IsReactHookCallWithDepsArray(const std::vector<Node*>& args) {
    auto valid = [&](size_t base) {
        const Node* fn = args[base];
        const Node* deps = args[base + 1];
        if (!(fn->type == NT::ArrowFunctionExpression && fn->params().empty() &&
              fn->body()->type == NT::BlockStatement && deps->type == NT::ArrayExpression)) {
            return false;
        }
        for (Node* a : args) {
            if (HasComment(a)) return false;
        }
        return true;
    };
    if (args.size() == 2) return valid(0);
    if (args.size() == 3) return args[0]->type == NT::Identifier && valid(1);
    return false;
}

Doc* Printer::PrintCallArguments() {
    Node* n = node();
    const auto args = GetCallArguments(n);
    if (args.empty()) return Cat({S("("), PrintDanglingComments(), S(")")});
    const size_t last_index = args.size() - 1;

    if (IsReactHookCallWithDepsArray(args)) {
        Parts parts{S("(")};
        IterateCallArgumentsPath([&](int i) {
            parts.push_back(MainPrint(nullptr));
            if (static_cast<size_t>(i) != last_index) parts.push_back(S(", "));
        });
        parts.push_back(S(")"));
        return Cat(parts);
    }

    bool any_arg_empty_line = false;
    Parts printed_args;
    IterateCallArgumentsPath([&](int i) {
        Node* arg = node();
        Doc* d = MainPrint(nullptr);
        if (static_cast<size_t>(i) == last_index) {
        } else if (IsNextLineEmpty(arg)) {
            any_arg_empty_line = true;
            d = Cat({d, S(","), hardline(), hardline()});
        } else {
            d = Cat({d, S(","), line()});
        }
        printed_args.push_back(d);
    });

    Doc* trailing_comma =
        n->type != NT::ImportExpression && n->type != NT::TSImportType && opts_.trailing_comma_all ? S(",") : nullptr;
    if (n->type == NT::TSImportType && args.size() == 1 && IsStringLiteral(args[0]) && !HasComment(args[0])) {
        Parts g{S("(")};
        g.insert(g.end(), printed_args.begin(), printed_args.end());
        g.push_back(IfBreak(trailing_comma));
        g.push_back(S(")"));
        return Group(Cat(g));
    }

    auto all_args_broken_out = [&]() {
        Parts inner{line()};
        inner.insert(inner.end(), printed_args.begin(), printed_args.end());
        return Group(Cat({S("("), Indent(Cat(inner)), trailing_comma, line(), S(")")}), true);
    };

    if (any_arg_empty_line || (parent()->type != NT::Decorator && IsFunctionCompositionArgs(args))) {
        return all_args_broken_out();
    }

    if (ShouldExpandFirstArg(args)) {
        Parts tail(printed_args.begin() + 1, printed_args.end());
        for (Doc* d : tail) {
            if (WillBreak(d)) return all_args_broken_out();
        }
        Doc* first;
        try {
            Args a;
            a.expand_first = true;
            first = PrintCallArgument(0, &a);
        } catch (const ArgExpansionBailout&) {
            return all_args_broken_out();
        }
        auto state = [&](Doc* f) {
            Parts s{S("("), f, S(", ")};
            s.insert(s.end(), tail.begin(), tail.end());
            s.push_back(S(")"));
            return Cat(s);
        };
        if (WillBreak(first)) {
            return Cat({a_.break_parent, a_.ConditionalGroup({state(Group(first, true)), all_args_broken_out()})});
        }
        return a_.ConditionalGroup({state(first), state(Group(first, true)), all_args_broken_out()});
    }

    if (ShouldExpandLastArg(args, printed_args)) {
        Parts head(printed_args.begin(), printed_args.end() - 1);
        for (Doc* d : head) {
            if (WillBreak(d)) return all_args_broken_out();
        }
        Doc* last;
        try {
            Args a;
            a.expand_last = true;
            last = PrintCallArgument(-1, &a);
        } catch (const ArgExpansionBailout&) {
            return all_args_broken_out();
        }
        auto state = [&](Doc* l) {
            Parts s{S("(")};
            s.insert(s.end(), head.begin(), head.end());
            s.push_back(l);
            s.push_back(S(")"));
            return Cat(s);
        };
        if (WillBreak(last)) {
            return Cat({a_.break_parent, a_.ConditionalGroup({state(Group(last, true)), all_args_broken_out()})});
        }
        return a_.ConditionalGroup({state(last), state(Group(last, true)), all_args_broken_out()});
    }

    Parts inner{softline()};
    inner.insert(inner.end(), printed_args.begin(), printed_args.end());
    Doc* contents = Cat({S("("), Indent(Cat(inner)), IfBreak(trailing_comma), softline(), S(")")});
    if (IsLongCurriedCallExpression()) return contents;
    bool any_break = any_arg_empty_line;
    for (Doc* d : printed_args) {
        if (WillBreak(d)) any_break = true;
    }
    return Group(contents, any_break);
}

// ---------------------------------------------------------------- assignment.js

static bool IsAssignment(const Node* n) { return Is(n, NT::AssignmentExpression); }
static bool IsAssignmentOrVariableDeclarator(const Node* n) {
    return IsAssignment(n) || Is(n, NT::VariableDeclarator);
}

bool Printer::IsComplexDestructuring(const Node* n) const {
    if (!IsAssignmentOrVariableDeclarator(n)) return false;
    const Node* left = n->kid[0];  // left / id
    if (!Is(left, NT::ObjectPattern) || left->properties().size() <= 2) return false;
    for (Node* p : left->properties()) {
        if (IsObjectProperty(p) &&
            (!p->Has(jsparse::kFlagShorthand) || Is(p->value(), NT::AssignmentPattern))) {
            return true;
        }
    }
    return false;
}

bool Printer::IsObjectPropertyWithShortKey(const Node* n, Doc* key_doc) {
    if (!IsObjectProperty(n)) return false;
    Doc* d = a_.CleanDoc(key_doc);
    if (d && d->kind != DocKind::String) return false;
    const int w = d ? StringWidth(d->str) : 0;
    return w < opts_.indent + 3;
}

static const NodeList* TypeArgsOf(const Node* n) {
    const Node* t = n->typeArguments() ? n->typeArguments() : n->typeParameters();
    return t ? &t->params() : nullptr;
}

bool Printer::IsCallExpressionWithComplexTypeArguments(const Node* n) {
    const NodeList* args = n->typeArguments() ? &n->typeArguments()->params() : nullptr;
    if (!args || args->empty()) return false;
    if (args->size() > 1) return true;
    const Node* first = (*args)[0];
    if (IsUnionType(first) || first->type == NT::TSIntersectionType || first->type == NT::TSTypeLiteral) return true;
    return WillBreak(Print(Key::typeArguments));
}

bool Printer::IsPoorlyBreakableMemberOrCallChain(bool deep) {
    Node* n = node();
    auto deeper = [&] { return IsPoorlyBreakableMemberOrCallChain(true); };
    if (n->type == NT::ChainExpression || n->type == NT::TSNonNullExpression) return Call(Key::expression, deeper);
    if (IsCallExpression(n)) {
        Doc* doc = PrintCallExpression();
        if (HasLabel(doc, kLabelMemberChain)) return false;
        const auto args = GetCallArguments(n);
        const bool poorly = args.empty() || (args.size() == 1 && IsLoneShortArgument(args[0]));
        if (!poorly) return false;
        if (IsCallExpressionWithComplexTypeArguments(n)) return false;
        return Call(Key::callee, deeper);
    }
    if (IsMemberExpression(n)) return Call(Key::object, deeper);
    return deep && (n->type == NT::Identifier || n->type == NT::ThisExpression);
}

static bool IsGenericType(const Node* t) {
    switch (t->type) {
        case NT::TSFunctionType:
            return t->typeParameters() != nullptr;
        case NT::TSTypeReference:
            return t->typeArguments() != nullptr;
        default:
            return false;
    }
}

bool Printer::ShouldBreakAfterOperator(bool has_short_key) {
    Node* right = node();
    if (IsBinaryish(right) && !ShouldInlineLogicalExpression(right)) return true;
    switch (right->type) {
        case NT::SequenceExpression:
            return true;
        case NT::TSConditionalType:
            if (IsGenericType(right->checkType()) || IsGenericType(right->extendsType())) return true;
            break;
        case NT::ConditionalExpression: {
            const Node* test = right->test();
            return IsBinaryish(test) && !ShouldInlineLogicalExpression(test);
        }
        case NT::ClassExpression:
            return !right->decorators().empty();
        default:
            break;
    }
    if (has_short_key) return false;
    std::vector<Key> keys;
    Node* n = right;
    while (true) {
        if (n->type == NT::UnaryExpression || n->type == NT::AwaitExpression ||
            (n->type == NT::YieldExpression && n->argument())) {
            n = n->argument();
            keys.push_back(Key::argument);
        } else if (n->type == NT::TSNonNullExpression) {
            n = n->expression();
            keys.push_back(Key::expression);
        } else {
            break;
        }
    }
    if (IsStringLiteral(n)) return true;
    std::function<bool(size_t)> go = [&](size_t d) -> bool {
        if (d == keys.size()) return IsPoorlyBreakableMemberOrCallChain();
        return Call(keys[d], [&] { return go(d + 1); });
    };
    return go(0);
}

static bool IsComplexTypeAliasParams(const Node* n) {
    if (n->type != NT::TSTypeAliasDeclaration || !n->typeParameters()) return false;
    const auto& ps = n->typeParameters()->params();
    if (ps.size() <= 1) return false;
    for (Node* q : ps) {
        if (q->constraint() || q->defaultType()) return true;
    }
    return false;
}

static bool HasComplexTypeAnnotation(const Node* n) {
    if (n->type != NT::VariableDeclarator) return false;
    const Node* ta = n->id()->typeAnnotation();
    if (!ta || !ta->typeAnnotation()) return false;
    const Node* t = ta->typeAnnotation();
    if (t->type != NT::TSTypeReference) return false;
    const NodeList* ps = TypeArgsOf(t);
    if (!ps || ps->size() <= 1) return false;
    for (Node* q : *ps) {
        if (q->type == NT::TSConditionalType) return true;
        if (q->type == NT::TSTypeReference) {
            const NodeList* qs = TypeArgsOf(q);
            if (qs && !qs->empty()) return true;
        }
    }
    return false;
}

Layout Printer::ChooseLayout(Doc* left_doc, Key right_key) {
    Node* n = node();
    Node* right = Child(n, right_key);
    if (!right) return Layout::OnlyLeft;
    const bool is_tail = !IsAssignment(right);
    const bool chain = Match({[](Node* x, Key) { return IsAssignment(x); },
                              [](Node* x, Key) { return IsAssignmentOrVariableDeclarator(x); },
                              [&](Node* x, Key) {
                                  return !is_tail ||
                                         (x->type != NT::ExpressionStatement && x->type != NT::VariableDeclaration);
                              }});
    if (chain) {
        if (!is_tail) return Layout::Chain;
        return right->type == NT::ArrowFunctionExpression && right->body()->type == NT::ArrowFunctionExpression
                   ? Layout::ChainTailArrowChain
                   : Layout::ChainTail;
    }
    const bool head_of_long_chain = !is_tail && IsAssignment(right->right());
    if (head_of_long_chain || HasLeadingOwnLineComment(right)) return Layout::BreakAfterOperator;
    if (n->type == NT::ImportAttribute ||
        (right->type == NT::CallExpression && Is(right->callee(), NT::Identifier) && right->callee()->str == "require")) {
        return Layout::NeverBreakAfterOperator;
    }
    const bool can_break_left = a_.CanBreak(left_doc);
    if (IsComplexDestructuring(n) || HasComplexTypeAnnotation(n) ||
        (n->type == NT::VariableDeclarator && Is(n->init(), NT::ArrowFunctionExpression) && can_break_left)) {
        return Layout::BreakLhs;
    }
    const bool has_short_key = IsObjectPropertyWithShortKey(n, left_doc);
    if (Call(right_key, [&] { return ShouldBreakAfterOperator(has_short_key); })) return Layout::BreakAfterOperator;
    if (IsComplexTypeAliasParams(n)) return Layout::BreakLhs;
    if (!can_break_left &&
        (has_short_key || right->type == NT::TemplateLiteral || right->type == NT::TaggedTemplateExpression ||
         IsBooleanLiteral(right) || IsNumericLiteral(right) || right->type == NT::ClassExpression)) {
        return Layout::NeverBreakAfterOperator;
    }
    return Layout::Fluid;
}

Doc* Printer::PrintAssignment(Doc* left, Doc* op, Key right_key) {
    const Layout layout = ChooseLayout(left, right_key);
    Doc* right = nullptr;
    if (Child(node(), right_key)) {
        Args a;
        a.layout = layout;
        right = PrintArgs(right_key, a);
    }
    switch (layout) {
        case Layout::BreakAfterOperator:
            return Group(Cat({Group(left), op, Group(Indent(Cat({line(), right})))}));
        case Layout::NeverBreakAfterOperator:
            return Group(Cat({Group(left), op, S(" "), right}));
        case Layout::Fluid: {
            const GroupId id = a_.NewId();
            return Group(Cat({Group(left), op, Group(Indent(line()), false, id), a_.line_suffix_boundary,
                              a_.IndentIfBreak(right, id)}));
        }
        case Layout::BreakLhs:
            return Group(Cat({left, op, S(" "), Group(right)}));
        case Layout::Chain:
            return Cat({Group(left), op, line(), right});
        case Layout::ChainTail:
            return Cat({Group(left), op, Indent(Cat({line(), right}))});
        case Layout::ChainTailArrowChain:
            return Cat({Group(left), op, right});
        case Layout::OnlyLeft:
        default:
            return left;
    }
}

Doc* Printer::PrintAssignmentExpression() {
    Node* n = node();
    return PrintAssignment(Print(Key::left), Cat({S(" "), S(n->op)}), Key::right);
}

Doc* Printer::PrintVariableDeclarator() { return PrintAssignment(Print(Key::id), S(" ="), Key::init); }

// ---------------------------------------------------------------- binaryish.js

bool Printer::ShouldInlineLogicalExpression(const Node* n) const {
    if (n->type != NT::LogicalExpression) return false;
    const Node* r = n->right();
    if (IsObjectExpression(r) && !r->properties().empty()) return true;
    if (IsArrayExpression(r) && !r->elements().empty()) return true;
    return IsJsxElement(r);
}

Parts Printer::PrintBinaryishExpressions(bool is_nested, bool is_inside_parens) {
    Node* n = node();
    if (!IsBinaryish(n)) return Parts{Group(Print())};
    Parts parts;
    if (IsBinaryish(n->left()) && ShouldFlatten(n->op, n->left()->op)) {
        parts = Call(Key::left, [&] { return PrintBinaryishExpressions(true, is_inside_parens); });
    } else {
        parts.push_back(Group(Print(Key::left)));
    }
    const bool should_inline = ShouldInlineLogicalExpression(n);
    Doc* right;
    if (should_inline) {
        right = Cat({S(n->op), HasLeadingOwnLineComment(n->right()) ? Indent(Cat({line(), Print(Key::right)}))
                                                                    : Cat({S(" "), Print(Key::right)})});
    } else {
        right = Cat({S(n->op), line(), Print(Key::right)});
    }
    Node* p = parent();
    const bool should_break = HasComment(n->left(), kCTrailing | kCLine);
    const bool should_group = should_break || (!(is_inside_parens && n->type == NT::LogicalExpression) &&
                                               p->type != n->type && n->left()->type != n->type &&
                                               n->right()->type != n->type);
    if (should_group) right = Group(right, should_break);
    parts.push_back(S(" "));
    parts.push_back(right);
    if (is_nested && HasComment(n)) {
        Doc* printed = a_.CleanDoc(PrintComments(Cat(parts)));
        if (printed && (printed->kind == DocKind::Array || printed->kind == DocKind::Fill)) {
            return Parts(printed->parts, printed->parts + printed->n);
        }
        return Parts{printed};
    }
    return parts;
}

Doc* Printer::PrintBinaryish() {
    Node* n = node();
    Node* p = parent();
    Node* gp = grandparent();
    const Key k = key();
    const bool inside_parens =
        k != Key::body && (p->type == NT::IfStatement || p->type == NT::WhileStatement ||
                           p->type == NT::SwitchStatement || p->type == NT::DoWhileStatement);
    Parts parts = PrintBinaryishExpressions(false, inside_parens);
    if (inside_parens) return Cat(parts);
    if ((IsCallExpression(p) && p->callee() == n) || p->type == NT::UnaryExpression ||
        (IsMemberExpression(p) && !p->Has(jsparse::kFlagComputed))) {
        Parts inner{softline()};
        inner.insert(inner.end(), parts.begin(), parts.end());
        return Group(Cat({Indent(Cat(inner)), softline()}));
    }
    const bool should_not_indent =
        p->type == NT::ReturnStatement || p->type == NT::ThrowStatement ||
        (p->type == NT::JSXExpressionContainer && gp->type == NT::JSXAttribute) ||
        (n == p->body() && p->type == NT::ArrowFunctionExpression) ||
        (n != p->body() && p->type == NT::ForStatement) ||
        (p->type == NT::ConditionalExpression && gp->type != NT::ReturnStatement &&
         gp->type != NT::ThrowStatement && !IsCallExpression(gp)) ||
        p->type == NT::TemplateLiteral;
    const bool should_indent_if_inlining = p->type == NT::AssignmentExpression ||
                                           p->type == NT::VariableDeclarator || p->type == NT::PropertyDefinition ||
                                           p->type == NT::TSAbstractPropertyDefinition || IsObjectProperty(p);
    const bool same_precedence_sub =
        IsBinaryish(n->left()) && ShouldFlatten(n->op, n->left()->op);
    if (should_not_indent || (ShouldInlineLogicalExpression(n) && !same_precedence_sub) ||
        (!ShouldInlineLogicalExpression(n) && should_indent_if_inlining)) {
        return Group(Cat(parts));
    }
    if (parts.empty()) return nullptr;
    const bool has_jsx = IsJsxElement(n->right());
    size_t first_group = parts.size();
    for (size_t i = 0; i < parts.size(); i++) {
        if (parts[i] && parts[i]->kind == DocKind::Group) {
            first_group = i;
            break;
        }
    }
    const size_t head_len = first_group == parts.size() ? 1 : first_group + 1;
    Parts head(parts.begin(), parts.begin() + static_cast<std::ptrdiff_t>(std::min(head_len, parts.size())));
    const size_t rest_end = has_jsx ? (parts.size() >= 1 ? parts.size() - 1 : 0) : parts.size();
    Parts rest;
    for (size_t i = head.size(); i < rest_end; i++) rest.push_back(parts[i]);
    const GroupId id = a_.NewId();
    head.push_back(Indent(Cat(rest)));
    Doc* chain = Group(Cat(head), false, id);
    if (!has_jsx) return chain;
    return Group(Cat({chain, a_.IndentIfBreak(parts.back(), id)}));
}

// ---------------------------------------------------------------- ternary-old.js

static bool ConditionalChainContainsJsx(const Node* n) {
    std::vector<const Node*> list{n};
    for (size_t i = 0; i < list.size(); i++) {
        for (const Node* c : {list[i]->test(), list[i]->consequent(), list[i]->alternate()}) {
            if (IsJsxElement(c)) return true;
            if (Is(c, NT::ConditionalExpression)) list.push_back(c);
        }
    }
    return false;
}

bool Printer::ShouldExtraIndentForConditionalExpression() {
    Node* n = node();
    if (n->type != NT::ConditionalExpression) return false;
    Node* p = nullptr;
    Node* child = n;
    for (size_t count = 0; !p; count++) {
        Node* x = GetParentNode(count);
        if (!x) return false;
        if ((x->type == NT::ChainExpression && x->expression() == child) ||
            (IsCallExpression(x) && x->callee() == child) || (IsMemberExpression(x) && x->object() == child) ||
            (x->type == NT::TSNonNullExpression && x->expression() == child)) {
            child = x;
            continue;
        }
        if ((x->type == NT::NewExpression && x->callee() == child) ||
            (IsBinaryCastExpression(x) && x->expression() == child)) {
            p = GetParentNode(count + 1);
            child = x;
            if (!p) return false;
        } else {
            p = x;
        }
    }
    if (child == n) return false;
    switch (p->type) {
        case NT::AssignmentExpression:
            return p->right() == child;
        case NT::VariableDeclarator:
            return p->init() == child;
        case NT::ReturnStatement:
        case NT::ThrowStatement:
        case NT::UnaryExpression:
        case NT::YieldExpression:
        case NT::AwaitExpression:
            return p->argument() == child;
        default:
            return false;
    }
}

Doc* Printer::PrintTernary() {
    Node* n = node();
    const bool is_cond = n->type == NT::ConditionalExpression;
    const Key cons_key = is_cond ? Key::consequent : Key::trueType;
    const Key alt_key = is_cond ? Key::alternate : Key::falseType;
    Node* consequent = Child(n, cons_key);
    Node* alternate = Child(n, alt_key);
    auto is_test_of = [&](const Node* parent_node, const Node* x) {
        return is_cond ? parent_node->test() == x : (parent_node->checkType() == x || parent_node->extendsType() == x);
    };
    Parts parts;
    bool jsx_mode = false;
    Node* p = parent();
    const bool is_parent_test = p->type == n->type && is_test_of(p, n);
    bool force_no_indent = p->type == n->type && !is_parent_test;

    Node* current = nullptr;
    Node* previous = nullptr;
    size_t i = 0;
    do {
        previous = current ? current : n;
        current = GetParentNode(i);
        i++;
    } while (current && current->type == n->type && !is_test_of(current, previous));
    Node* first_non_conditional_parent = current ? current : p;
    Node* last_conditional_parent = previous;

    if (is_cond && (IsJsxElement(n->test()) || IsJsxElement(consequent) || IsJsxElement(alternate) ||
                    ConditionalChainContainsJsx(last_conditional_parent))) {
        jsx_mode = true;
        force_no_indent = true;
        auto wrap = [&](Doc* d) {
            return Cat({IfBreak(S("(")), Indent(Cat({softline(), d})), softline(), IfBreak(S(")"))});
        };
        auto is_nil = [](const Node* x) {
            return (x->type == NT::Literal && x->lit == jsparse::LitKind::Null) ||
                   (x->type == NT::Identifier && x->str == "undefined");
        };
        parts.push_back(S(" ? "));
        parts.push_back(is_nil(consequent) ? Print(cons_key) : wrap(Print(cons_key)));
        parts.push_back(S(" : "));
        parts.push_back(alternate->type == n->type || is_nil(alternate) ? Print(alt_key) : wrap(Print(alt_key)));
    } else {
        auto print_branch = [&](Key k) { return opts_.use_tabs ? Indent(Print(k)) : a_.Align(2, Print(k)); };
        const bool cons_cond = consequent->type == n->type;
        Doc* part = Cat({line(), S("? "), cons_cond ? IfBreak(nullptr, S("(")) : nullptr, print_branch(cons_key),
                         cons_cond ? IfBreak(nullptr, S(")")) : nullptr, line(), S(": "), print_branch(alt_key)});
        parts.push_back(p->type != n->type || Child(p, alt_key) == n || is_parent_test
                            ? part
                            : (opts_.use_tabs ? a_.Dedent(Indent(part))
                                              : a_.Align(std::max(0, opts_.indent - 2), part)));
    }

    bool should_break = false;
    auto check = [&](const Node* x) {
        if (HasComment(x, 0, [&](CommentRec* c) { return IsBlockComment(c) && HasNewlineInRange(c->start, c->end); })) {
            should_break = true;
        }
    };
    check(consequent);
    check(alternate);
    if (is_cond) {
        check(n->test());
    } else {
        check(n->checkType());
        check(n->extendsType());
    }
    auto maybe_group = [&](Doc* d) -> Doc* {
        if (p == first_non_conditional_parent) return Group(d, should_break);
        return should_break ? Cat({d, a_.break_parent}) : d;
    };
    const bool break_closing_paren = !jsx_mode && IsMemberExpression(p) && !p->Has(jsparse::kFlagComputed);
    const bool extra_indent = ShouldExtraIndentForConditionalExpression();
    // printTernaryTest
    Doc* test = is_cond ? Print(Key::test)
                        : Cat({Print(Key::checkType), S(" "), S("extends"), S(" "), Print(Key::extendsType)});
    if (p->type == n->type && Child(p, alt_key) == n) test = a_.Align(2, test);
    Doc* result = maybe_group(Cat({test, force_no_indent ? Cat(parts) : Indent(Cat(parts)),
                                   is_cond && break_closing_paren && !extra_indent ? softline() : nullptr}));
    return is_parent_test || extra_indent ? Group(Cat({Indent(Cat({softline(), result})), softline()})) : result;
}

// ---------------------------------------------------------------- function-parameters.js

bool Printer::ShouldHugTheOnlyFunctionParameter(const Node* n) {
    if (!n) return false;
    const auto& ps = n->params();
    if (ps.size() != 1) return false;
    const Node* q = ps[0];
    return !HasComment(q) &&
           (q->type == NT::ObjectPattern || q->type == NT::ArrayPattern ||
            (q->type == NT::Identifier && q->typeAnnotation() && q->typeAnnotation()->type == NT::TSTypeAnnotation &&
             IsObjectType(q->typeAnnotation()->typeAnnotation())) ||
            (q->type == NT::AssignmentPattern &&
             (q->left()->type == NT::ObjectPattern || q->left()->type == NT::ArrayPattern) &&
             (q->right()->type == NT::Identifier ||
              (IsObjectExpression(q->right()) && q->right()->properties().empty()) ||
              (IsArrayExpression(q->right()) && q->right()->elements().empty()))));
}

static const Node* GetReturnTypeNode(const Node* fn) {
    if (fn->returnType()) {
        const Node* r = fn->returnType();
        if (r->typeAnnotation()) r = r->typeAnnotation();
        return r;
    }
    // functionNode.typeAnnotation (TSMethodSignature/TSFunctionType in older ASTs)
    if (fn->type != NT::Identifier && fn->typeAnnotation()) return fn->typeAnnotation();
    return nullptr;
}

bool Printer::ShouldGroupFunctionParameters(const Node* fn, Doc* return_type_doc) {
    const Node* rt = GetReturnTypeNode(fn);
    if (!rt) return false;
    if (const Node* tp = fn->typeParameters()) {
        const auto& ps = tp->params();
        if (ps.size() > 1) return false;
        if (ps.size() == 1 && (ps[0]->constraint() || ps[0]->defaultType())) return false;
    }
    return fn->params().size() == 1 && (IsObjectType(rt) || WillBreak(return_type_doc));
}

bool Printer::ShouldBreakFunctionParameters(const Node* fn) const {
    const auto& ps = fn->params();
    if (ps.size() <= 1) return false;
    for (Node* q : ps) {
        if (q->type == NT::TSParameterProperty) return true;
    }
    return false;
}

bool Printer::IsDecoratedFunction() const {
    return Match({[](Node* x, Key) {
                      return x->type == NT::ArrowFunctionExpression && x->body()->type == NT::BlockStatement;
                  },
                  [](Node* x, Key k) {
                      if (x->type == NT::CallExpression && k == Key::arguments && x->arguments().size() == 1 &&
                          x->callee()->type == NT::CallExpression) {
                          const Node* d = x->callee()->callee();
                          return d->type == NT::Identifier ||
                                 (d->type == NT::MemberExpression && !d->Has(jsparse::kFlagComputed) &&
                                  d->object()->type == NT::Identifier && d->property()->type == NT::Identifier);
                      }
                      return false;
                  },
                  [](Node* x, Key k) {
                      return (x->type == NT::VariableDeclarator && k == Key::init) ||
                             (x->type == NT::ExportDefaultDeclaration && k == Key::declaration) ||
                             (x->type == NT::TSExportAssignment && k == Key::expression) ||
                             (x->type == NT::AssignmentExpression && k == Key::right &&
                              x->left()->type == NT::MemberExpression &&
                              x->left()->object()->type == NT::Identifier && x->left()->object()->str == "module" &&
                              x->left()->property()->type == NT::Identifier &&
                              x->left()->property()->str == "exports");
                  },
                  [](Node* x, Key) {
                      return x->type != NT::VariableDeclaration ||
                             (x->kind == "const" && x->declarations().size() == 1);
                  }});
}

Doc* Printer::PrintFunctionParameters(bool expand_arg, bool print_type_params) {
    Node* fn = node();
    const auto& params = fn->params();
    Doc* type_params = print_type_params ? PrintFunctionTypeParameters() : nullptr;
    if (params.empty()) {
        return Cat({type_params, S("("), PrintDanglingComments(false, [&](CommentRec* c) {
                        return GetNextNonSpaceNonCommentCharacter(c->end) == ')';
                    }),
                    S(")")});
    }
    const bool in_test_call = IsTestCall(parent());
    const bool hug = ShouldHugTheOnlyFunctionParameter(fn);
    Parts printed;
    Each(Key::params, [&](int i) {
        printed.push_back(Print());
        if (static_cast<size_t>(i) == params.size() - 1) return;
        printed.push_back(S(","));
        if (in_test_call || hug) {
            printed.push_back(S(" "));
        } else if (IsNextLineEmpty(params[static_cast<size_t>(i)])) {
            printed.push_back(hardline());
            printed.push_back(hardline());
        } else {
            printed.push_back(line());
        }
    });
    if (expand_arg && !IsDecoratedFunction()) {
        if (WillBreak(type_params) || WillBreak(Cat(printed))) throw ArgExpansionBailout();
        return Group(Cat({a_.RemoveLines(type_params), S("("), a_.RemoveLines(Cat(printed)), S(")")}));
    }
    bool has_not_decorator = true;
    for (Node* q : params) {
        if (!q->decorators().empty()) has_not_decorator = false;
    }
    if ((hug && has_not_decorator) || in_test_call) {
        Parts out{type_params, S("(")};
        out.insert(out.end(), printed.begin(), printed.end());
        out.push_back(S(")"));
        return Cat(out);
    }
    Parts inner{softline()};
    inner.insert(inner.end(), printed.begin(), printed.end());
    return Cat({type_params, S("("), Indent(Cat(inner)),
                IfBreak(!HasRestParameter(fn) && opts_.trailing_comma_all ? S(",") : nullptr), softline(), S(")")});
}

bool Printer::CanPrintParamsWithoutParens(const Node* n) {
    const auto& ps = n->params();
    return ps.size() == 1 && !n->typeParameters() && !HasComment(n, kCDangling) && ps[0]->type == NT::Identifier &&
           !ps[0]->typeAnnotation() && !HasComment(ps[0]) && !ps[0]->HasTs(jsparse::kTsOptional) && !n->returnType();
}

// ---------------------------------------------------------------- arrow-function.js

bool Printer::ShouldAddParensIfNotBreak(const Node* n) {
    return n->type == NT::ConditionalExpression &&
           !StartsWithNoLookaheadToken(n, [](const Node* x) { return x->type == NT::ObjectExpression; });
}

bool Printer::MayBreakAfterShortPrefix(const Node* body, Doc*) {
    return IsArrayExpression(body) || IsObjectExpression(body) || body->type == NT::ArrowFunctionExpression ||
           body->type == NT::BlockStatement || IsJsxElement(body) || IsTemplateOnItsOwnLine(body);
}

Doc* Printer::PrintArrowFunctionSignature(const Args* a) {
    Node* n = node();
    Parts parts;
    if (n->Has(jsparse::kFlagAsync)) parts.push_back(S("async "));
    if (!opts_.arrow_parens_always && CanPrintParamsWithoutParens(n)) {
        parts.push_back(PrintAt(Key::params, 0));
    } else {
        const bool expand = a && (a->expand_last || a->expand_first);
        Doc* return_type = PrintReturnType();
        if (expand) {
            if (WillBreak(return_type)) throw ArgExpansionBailout();
            return_type = Group(a_.RemoveLines(return_type));
        }
        parts.push_back(Group(Cat({PrintFunctionParameters(expand, true), return_type})));
    }
    Doc* dangling = PrintDanglingComments(false, [&](CommentRec* c) {
        const int64_t next = GetNextNonSpaceNonCommentCharacterIndex(c->end);
        return next != kNoIndex && text_.substr(static_cast<size_t>(next), 2) == "=>";
    });
    if (dangling) {
        parts.push_back(S(" "));
        parts.push_back(dangling);
    }
    return Cat(parts);
}

Doc* Printer::PrintArrowFunction(const Args* args_in) {
    Args empty;
    const Args* a = args_in ? args_in : &empty;
    Parts signature_docs;
    Doc* body_doc = nullptr;
    Parts body_comments;
    bool should_break_chain = false;
    const bool print_as_chain = !a->expand_last && node()->body()->type == NT::ArrowFunctionExpression;
    Node* function_body = nullptr;

    std::function<void()> rec = [&]() {
        Node* n = node();
        Doc* sig = PrintArrowFunctionSignature(a);
        if (signature_docs.empty()) {
            signature_docs.push_back(sig);
        } else {
            Separate s = PrintCommentsSeparately();
            signature_docs.push_back(Cat({s.leading, sig}));
            body_comments.insert(body_comments.begin(), s.trailing);
        }
        if (print_as_chain) {
            if ((n->returnType() && !n->params().empty()) || n->typeParameters()) should_break_chain = true;
            for (Node* q : n->params()) {
                if (q->type != NT::Identifier) should_break_chain = true;
            }
        }
        if (!print_as_chain || n->body()->type != NT::ArrowFunctionExpression) {
            body_doc = PrintArgs(Key::body, *a);
            function_body = n->body();
        } else {
            Call(Key::body, [&] {
                rec();
                return 0;
            });
        }
    };
    rec();

    const bool body_same_line = !HasLeadingOwnLineComment(function_body) &&
                                (function_body->type == NT::SequenceExpression ||
                                 MayBreakAfterShortPrefix(function_body, body_doc) ||
                                 (!should_break_chain && ShouldAddParensIfNotBreak(function_body)));
    Node* p = parent();
    const bool is_callee = key() == Key::callee && IsCallLikeExpression(p);
    const GroupId chain_id = a_.NewId();

    // printArrowFunctionSignatures
    Doc* signatures;
    if (signature_docs.size() == 1) {
        signatures = signature_docs[0];
    } else if ((key() != Key::callee && IsCallLikeExpression(p)) || IsBinaryish(p)) {
        Parts rest(signature_docs.begin() + 1, signature_docs.end());
        signatures = Group(Cat({signature_docs[0], S(" =>"),
                                Indent(Cat({line(), a_.Join(Cat({S(" =>"), line()}), rest)}))}),
                           should_break_chain);
    } else if ((key() == Key::callee && IsCallLikeExpression(p)) || a->layout != Layout::None) {
        signatures = Group(a_.Join(Cat({S(" =>"), line()}), signature_docs), should_break_chain);
    } else {
        signatures = Group(Indent(a_.Join(Cat({S(" =>"), line()}), signature_docs)), should_break_chain);
    }

    bool should_break_signatures = false, should_indent_signatures = false, softline_in_indent = false;
    if (print_as_chain && (is_callee || a->layout != Layout::None)) {
        should_indent_signatures = true;
        softline_in_indent = !HasComment(node(), 0);
        should_break_signatures = a->layout == Layout::ChainTailArrowChain || (is_callee && !body_same_line);
    }

    // printArrowFunctionBody
    Doc* trailing_comma = a->expand_last && opts_.trailing_comma_all ? IfBreak(S(",")) : nullptr;
    Doc* trailing_space =
        (a->expand_last || p->type == NT::JSXExpressionContainer) && !HasComment(node()) ? softline() : nullptr;
    Doc* comments = Cat(body_comments);
    Doc* body;
    if (body_same_line && ShouldAddParensIfNotBreak(function_body)) {
        body = Cat({S(" "),
                    Group(Cat({IfBreak(nullptr, S("(")), Indent(Cat({softline(), body_doc})), IfBreak(nullptr, S(")")),
                               trailing_comma, trailing_space})),
                    comments});
    } else if (body_same_line) {
        body = Cat({S(" "), body_doc, comments});
    } else {
        body = Cat({Indent(Cat({line(), body_doc, comments})), trailing_comma, trailing_space});
    }

    return Group(Cat({Group(should_indent_signatures
                                ? Indent(Cat({softline_in_indent ? softline() : nullptr, signatures}))
                                : signatures,
                            should_break_signatures, chain_id),
                      S(" =>"), print_as_chain ? a_.IndentIfBreak(body, chain_id) : Group(body),
                      print_as_chain && is_callee ? IfBreak(softline(), nullptr, chain_id) : nullptr}));
}

// ---------------------------------------------------------------- object.js

static bool IsTsSignatureMember(const Node* n) {
    return n->type == NT::TSPropertySignature || n->type == NT::TSMethodSignature ||
           n->type == NT::TSConstructSignatureDeclaration || n->type == NT::TSCallSignatureDeclaration;
}

Doc* Printer::PrintObject() {
    Node* n = node();
    const bool is_enum_body = n->type == NT::TSEnumBody;
    const Key field = n->type == NT::TSTypeLiteral || is_enum_body ? Key::members
                      : n->type == NT::TSInterfaceBody            ? Key::body
                                                                  : Key::properties;
    struct Prop {
        Node* node;
        Doc* printed;
    };
    std::vector<Prop> props_loc;
    Each(field, [&](int) { props_loc.push_back({node(), Print()}); });
    Node* p = parent();
    const bool should_break =
        n->type == NT::TSInterfaceBody || is_enum_body ||
        (n->type == NT::ObjectPattern && p->type != NT::FunctionDeclaration && p->type != NT::FunctionExpression &&
         p->type != NT::ArrowFunctionExpression && p->type != NT::AssignmentPattern &&
         p->type != NT::CatchClause &&
         std::any_of(n->properties().begin(), n->properties().end(),
                     [](Node* q) {
                         return q->type == NT::Property && q->value() &&
                                (q->value()->type == NT::ObjectPattern || q->value()->type == NT::ArrayPattern);
                     })) ||
        (n->type != NT::ObjectPattern && !props_loc.empty() &&
         HasNewlineInRange(n->start, props_loc[0].node->start));

    Doc* semi = opts_.semi ? S(";") : nullptr;
    Doc* separator = n->type == NT::TSInterfaceBody || n->type == NT::TSTypeLiteral ? IfBreak(semi, S(";")) : S(",");
    Parts separator_parts;
    Parts props;
    for (const Prop& q : props_loc) {
        Parts r = separator_parts;
        r.push_back(Group(q.printed));
        props.push_back(Cat(r));
        separator_parts = {separator, line()};
        if (IsTsSignatureMember(q.node) && HasComment(q.node, kCPrettierIgnore)) separator_parts.erase(separator_parts.begin());
        if (IsNextLineEmpty(q.node)) separator_parts.push_back(hardline());
    }
    Node* last = props_loc.empty() ? nullptr : props_loc.back().node;
    const bool can_trailing =
        !(last && (last->type == NT::RestElement ||
                   ((IsTsSignatureMember(last) || last->type == NT::TSIndexSignature) &&
                    HasComment(last, kCPrettierIgnore)))) &&
        !Match({nullptr, [](Node* x, Key k) { return x->type == NT::TSImportType && k == Key::options; }});

    Doc* content;
    if (props.empty()) {
        if (!HasComment(n, kCDangling)) return Cat({S("{"), S("}"), PrintTypeAnnotationProperty()});
        content = Group(Cat({S("{"), PrintDanglingComments(true), softline(), S("}"), PrintOptionalToken(),
                             PrintTypeAnnotationProperty()}));
    } else {
        Doc* bl = opts_.bracket_spacing ? line() : softline();
        Parts inner{bl};
        inner.insert(inner.end(), props.begin(), props.end());
        Doc* trailing = nullptr;
        if (can_trailing) {
            const bool is_comma = !(n->type == NT::TSInterfaceBody || n->type == NT::TSTypeLiteral);
            // shouldPrintComma(options) is es5-level: true for "es5" and "all"
            trailing = is_comma ? S(",") : separator;
        }
        content = Cat({S("{"), Indent(Cat(inner)), IfBreak(trailing), bl, S("}"), PrintOptionalToken(),
                       PrintTypeAnnotationProperty()});
    }

    auto hug_param = [&](Node* x, Key k) { return k == Key::params && ShouldHugTheOnlyFunctionParameter(x); };
    if (Match({[](Node* x, Key) { return x->type == NT::ObjectPattern && x->decorators().empty(); }, hug_param}) ||
        (IsObjectType(n) && Match({nullptr, [](Node*, Key k) { return k == Key::typeAnnotation; },
                                   [](Node*, Key k) { return k == Key::typeAnnotation; }, hug_param})) ||
        (!should_break && Match({[](Node* x, Key) { return x->type == NT::ObjectPattern; },
                                 [](Node* x, Key) {
                                     return x->type == NT::AssignmentExpression || x->type == NT::VariableDeclarator;
                                 }}))) {
        return content;
    }
    return Group(content, should_break);
}

// ---------------------------------------------------------------- array.js

Doc* Printer::PrintArrayElements() {
    Parts parts;
    Each(Key::elements, [&](int) {
        Node* e = node();
        parts.push_back(e ? Group(Print()) : nullptr);
        if (!IsLast()) parts.push_back(Cat({S(","), line(), e && IsLineAfterElementEmpty(e) ? softline() : nullptr}));
    });
    return Cat(parts);
}

Doc* Printer::PrintArrayElementsConcisely(Doc* trailing_comma) {
    Parts parts;
    Each(Key::elements, [&](int) {
        Node* e = node();
        const bool last = IsLast();
        parts.push_back(Cat({Print(), last ? trailing_comma : S(",")}));
        if (!last) {
            parts.push_back(IsLineAfterElementEmpty(e)        ? Cat({hardline(), hardline()})
                            : HasComment(Next(), kCLeading | kCLine) ? hardline()
                                                                  : line());
        }
    });
    return a_.Fill(parts);
}

Doc* Printer::PrintArray() {
    Node* n = node();
    const auto& elements = n->elements();
    if (elements.empty()) {
        if (!HasComment(n, kCDangling)) return Cat({S("[]"), PrintOptionalToken(), PrintTypeAnnotationProperty()});
        return Cat({Group(Cat({S("["), PrintDanglingComments(true), softline(), S("]")})), PrintOptionalToken(),
                    PrintTypeAnnotationProperty()});
    }
    Node* last = elements[elements.size() - 1];
    const bool can_trailing = !Is(last, NT::RestElement);
    const bool forced_trailing = last == nullptr;
    const GroupId id = a_.NewId();
    bool should_break = !in_jest_each_ && elements.size() > 1;
    if (should_break) {
        for (size_t i = 0; i < elements.size(); i++) {
            Node* e = elements[i];
            if (!IsArrayExpression(e) && !IsObjectExpression(e)) {
                should_break = false;
                break;
            }
            Node* next = i + 1 < elements.size() ? elements[i + 1] : nullptr;
            if (next && e->type != next->type) {
                should_break = false;
                break;
            }
            const size_t count = IsArrayExpression(e) ? e->elements().size() : e->properties().size();
            if (count <= 1) {
                should_break = false;
                break;
            }
        }
    }
    const bool concise = IsConciselyPrintedArray(n);
    Doc* trailing_comma = !can_trailing     ? nullptr
                          : forced_trailing ? S(",")
                          : concise         ? IfBreak(S(","), nullptr, id)
                                            : IfBreak(S(","));
    Doc* body = concise ? PrintArrayElementsConcisely(trailing_comma) : Cat({PrintArrayElements(), trailing_comma});
    return Cat({Group(Cat({S("["), Indent(Cat({softline(), body, PrintDanglingComments()})), softline(), S("]")}),
                      should_break, id),
                PrintOptionalToken(), PrintTypeAnnotationProperty()});
}

// ---------------------------------------------------------------- template-literal.js

static int GetAlignmentSize(std::string_view text, int tab_width) {
    int size = 0;
    for (char c : text) {
        if (c == '\t') {
            size = size + tab_width - (size % tab_width);
        } else {
            size++;
        }
    }
    return size;
}

static int GetIndentSize(std::string_view value, int tab_width) {
    const size_t nl = value.rfind('\n');
    if (nl == std::string_view::npos) return 0;
    std::string_view rest = value.substr(nl + 1);
    size_t k = 0;
    while (k < rest.size() && (rest[k] == ' ' || rest[k] == '\t')) k++;
    return GetAlignmentSize(rest.substr(0, k), tab_width);
}

bool Printer::IsJestEachTemplateLiteral() {
    Node* p = parent();
    auto trigger = [](std::string_view name) {
        if (!name.empty() && (name[0] == 'f' || name[0] == 'x')) {
            const std::string_view rest = name.substr(1);
            if (rest == "describe" || rest == "it" || rest == "test") return true;
        }
        return name == "describe" || name == "it" || name == "test";
    };
    if (!Is(p, NT::TaggedTemplateExpression) || p->quasi() != node()) return false;
    const Node* tag = p->tag();
    if (!Is(tag, NT::MemberExpression) || tag->Has(jsparse::kFlagComputed) || !Is(tag->property(), NT::Identifier) ||
        tag->property()->str != "each") {
        return false;
    }
    const Node* o = tag->object();
    if (Is(o, NT::Identifier)) return trigger(o->str);
    return Is(o, NT::MemberExpression) && !o->Has(jsparse::kFlagComputed) && Is(o->property(), NT::Identifier) &&
           (o->property()->str == "only" || o->property()->str == "skip") && Is(o->object(), NT::Identifier) &&
           trigger(o->object()->str);
}

static std::vector<std::string_view> SplitHeader(std::string_view s) {
    // trim().split(/\s*\|\s*/)
    s = TrimJs(s);
    std::vector<std::string_view> out;
    size_t start = 0;
    while (true) {
        const size_t bar = s.find('|', start);
        std::string_view part = s.substr(start, bar == std::string_view::npos ? std::string_view::npos : bar - start);
        out.push_back(TrimJs(part));
        if (bar == std::string_view::npos) break;
        start = bar + 1;
    }
    return out;
}

Doc* Printer::PrintJestEachTemplateLiteral() {
    Node* n = node();
    const auto header = SplitHeader(Raw(n->quasis()[0]));
    bool any = header.size() > 1;
    for (auto h : header) {
        if (!h.empty()) any = true;
    }
    if (!any) return nullptr;
    in_jest_each_ = true;
    Parts expressions;
    try {
        expressions = PrintAll(Key::expressions);
    } catch (...) {
        in_jest_each_ = false;
        throw;
    }
    in_jest_each_ = false;
    std::vector<std::string> cells_text;
    for (Doc* d : expressions) {
        PrintOptions po;
        po.width = -1;
        po.tab_width = opts_.indent;
        po.use_tabs = opts_.use_tabs;
        cells_text.push_back("${" + PrintDocToString(a_, d, po) + "}");
    }
    struct Row {
        bool has_line_break = false;
        std::vector<std::string> cells;
    };
    std::vector<Row> body(1);
    for (size_t i = 1; i < n->quasis().size(); i++) {
        Row& row = body.back();
        const std::string& cell = cells_text[i - 1];
        row.cells.push_back(cell);
        if (cell.find('\n') != std::string::npos) row.has_line_break = true;
        if (Raw(n->quasis()[i]).find('\n') != std::string_view::npos) body.push_back(Row{});
    }
    size_t max_cols = header.size();
    for (const Row& r : body) max_cols = std::max(max_cols, r.cells.size());
    std::vector<int> widths(max_cols, 0);
    std::vector<Row> table;
    Row head;
    for (auto h : header) head.cells.emplace_back(h);
    table.push_back(head);
    for (const Row& r : body) {
        if (!r.cells.empty()) table.push_back(r);
    }
    for (const Row& r : table) {
        if (r.has_line_break) continue;
        for (size_t k = 0; k < r.cells.size(); k++) widths[k] = std::max(widths[k], StringWidth(r.cells[k]));
    }
    Parts rows;
    for (const Row& r : table) {
        Parts cells;
        for (size_t k = 0; k < r.cells.size(); k++) {
            std::string c = r.cells[k];
            if (!r.has_line_break) c += std::string(static_cast<size_t>(widths[k] - StringWidth(r.cells[k])), ' ');
            cells.push_back(a_.Own(std::move(c)));
        }
        rows.push_back(a_.Join(S(" | "), cells));
    }
    return Cat({a_.line_suffix_boundary, S("`"), Indent(Cat({hardline(), a_.Join(hardline(), rows)})), hardline(),
                S("`")});
}

Doc* Printer::PrintTemplateLiteral() {
    Node* n = node();
    if (n->type == NT::TemplateLiteral && IsJestEachTemplateLiteral()) {
        Doc* d = PrintJestEachTemplateLiteral();
        if (d) return d;
    }
    Parts expression_docs = PrintAll(Key::expressions);
    Parts parts{a_.line_suffix_boundary, S("`")};
    int previous_indent = 0;
    Each(Key::quasis, [&](int index) {
        Node* quasi = node();
        parts.push_back(Print());
        if (quasi->Has(jsparse::kFlagTail)) return;
        const std::string_view text = Raw(quasi);
        const int indent_size = text.find('\n') != std::string_view::npos ? GetIndentSize(text, opts_.indent)
                                                                          : previous_indent;
        previous_indent = indent_size;
        Doc* expr_doc = expression_docs[static_cast<size_t>(index)];
        Node* expr = n->expressions()[static_cast<size_t>(index)];
        bool has_newline = HasNewlineInRange(quasi->end, n->quasis()[static_cast<size_t>(index) + 1]->start);
        if (!has_newline) {
            PrintOptions po;
            po.width = -1;
            po.tab_width = opts_.indent;
            po.use_tabs = opts_.use_tabs;
            std::string rendered = PrintDocToString(a_, expr_doc, po);
            if (rendered.find('\n') != std::string::npos) {
                has_newline = true;
            } else {
                expr_doc = a_.Own(std::move(rendered));
            }
        }
        if (has_newline && (HasComment(expr) || expr->type == NT::Identifier || IsMemberExpression(expr) ||
                            expr->type == NT::ConditionalExpression || expr->type == NT::SequenceExpression ||
                            IsBinaryCastExpression(expr) || IsBinaryish(expr))) {
            expr_doc = Cat({Indent(Cat({softline(), expr_doc})), softline()});
        }
        Doc* aligned = indent_size == 0 && !text.empty() && text.back() == '\n'
                           ? a_.DedentToRoot(expr_doc)
                           : a_.AddAlignmentToDoc(expr_doc, indent_size, opts_.indent);
        parts.push_back(Group(Cat({S("${"), aligned, a_.line_suffix_boundary, S("}")})));
    });
    parts.push_back(S("`"));
    return Cat(parts);
}

Doc* Printer::PrintTaggedTemplateLiteral() {
    Node* n = node();
    Doc* quasi = Print(Key::quasi);
    Doc* space = nullptr;
    auto leading = GetComments(n->quasi(), kCLeading);
    if (!leading.empty()) {
        const Node* before = n->typeArguments() ? n->typeArguments() : n->tag();
        space = HasNewlineInRange(before->end, leading[0]->start) ? softline() : S(" ");
    }
    return Cat({Print(Key::tag), Print(Key::typeArguments), space, a_.line_suffix_boundary, quasi});
}

}  // namespace jsfmt
