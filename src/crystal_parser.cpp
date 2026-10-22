// The Crystal parser: a port of src/compiler/crystal/syntax/parser.cr
// (Crystal 1.19). See crystal_parser.h. The functions follow the
// original's order; a comment marks each group.

#include "crystal_parser.h"

#include <algorithm>
#include <functional>
#include <memory>

namespace crystal {

// --- scoped helpers (the original's block-taking methods) ------------------------------

template <typename F>
auto Parser::with_isolated_var_scope(bool create_scope, F &&f) -> decltype(f()) {
    if (!create_scope) return f();
    var_scopes_.emplace_back();
    struct Pop {
        Parser *p;
        ~Pop() { p->var_scopes_.pop_back(); }
    } pop{this};
    return f();
}

template <typename F>
auto Parser::with_lexical_var_scope(F &&f) -> decltype(f()) {
    std::unordered_set<std::string> current = var_scopes_.back();
    var_scopes_.push_back(std::move(current));
    struct Pop {
        Parser *p;
        ~Pop() { p->var_scopes_.pop_back(); }
    } pop{this};
    return f();
}

template <typename F>
auto Parser::preserve_stop_on_do(bool new_value, F &&f) -> decltype(f()) {
    const bool old = stop_on_do_;
    stop_on_do_ = new_value;
    auto value = f();
    stop_on_do_ = old;
    return value;
}

template <typename F>
auto Parser::push_visibility(F &&f) -> decltype(f()) {
    const bool old_has = has_visibility_;
    const Visibility old = visibility_;
    has_visibility_ = false;
    auto value = f();
    has_visibility_ = old_has;
    visibility_ = old;
    return value;
}

template <typename F>
auto Parser::open(const std::string &symbol, Location location, F &&f) -> decltype(f()) {
    unclosed_stack_.push_back(Unclosed{symbol, location});
    struct Pop {
        Parser *p;
        ~Pop() { p->unclosed_stack_.pop_back(); }
    } pop{this};
    return f();
}

template <typename F>
Node *Parser::check_type_declaration(F &&f) {
    if (next_comes_colon_space()) {
        const std::string name = value_s();
        Node *var = at_end(at(var_node(name), token_.location()), token_end_location());
        next_token();
        if (token_.type != Tok::SPACE) {
            warn(token_.location(),
                 "space required before colon in type declaration (run `crystal tool format` to fix this)");
        }
        skip_space();
        check(Tok::OP_COLON);
        Node *type_declaration = parse_type_declaration(var);
        return set_visibility(type_declaration);
    }
    return f();
}

template <typename F>
Node *Parser::check_not_inside_def(const std::string &message, F &&f) {
    if (def_nest_ == 0 && fun_nest_ == 0) return f();
    const std::string suffix = def_nest_ > 0 ? " inside def" : " inside fun";
    raise(message + suffix, token_.line_number, token_.column_number);
}

template <typename F>
Node *Parser::parse_type_splat(F &&f) {
    const Location location = token_.location();
    bool splat = false;
    if (token_.type == Tok::OP_STAR) {
        next_token_skip_space_or_newline();
        splat = true;
    }
    Node *type = f();
    if (splat) type = at(unary(NodeKind::Splat, type), location);
    return type;
}

// --- entry points -------------------------------------------------------------------

Parser::Parser(std::string_view src, Ast &ast) : Lexer(src), ast_(ast) { var_scopes_.emplace_back(); }

ParseResult Parse(std::string_view src, Ast &ast, const std::string &filename) {
    ParseResult r;
    std::unique_ptr<Parser> p;
    try {
        // The lexer decodes the first character on construction, and
        // reports malformed UTF-8 there, as the original does.
        p = std::make_unique<Parser>(src, ast);
        p->filename = filename;
        r.root = p->parse();
        r.ok = true;
    } catch (const SyntaxError &e) {
        r.ok = false;
        r.error = e.what();
        r.error_line = e.line;
        r.error_column = e.column;
    }
    if (p) r.warnings = p->warnings;
    return r;
}

Node *Parser::parse() {
    next_token_skip_statement_end();
    Node *n = parse_expressions();
    check(Tok::EOF_);
    return n;
}

Node *Parser::parse(ParseMode mode) {
    switch (mode) {
        case ParseMode::Normal:
            return parse();
        case ParseMode::Lib:
            return parse_lib_body();
        case ParseMode::LibStructOrUnion:
            next_token_skip_statement_end();
            return expressions_from(parse_c_struct_or_union_body_expressions());
        case ParseMode::Enum:
            break;
    }
    next_token_skip_statement_end();
    return expressions_from(parse_enum_body_expressions());
}

Token &Parser::next_token() {
    Token &t = Lexer::next_token();
    if (t.type == Tok::NEWLINE && !consuming_heredocs_ && !heredocs.empty()) consume_heredocs();
    return token_;
}

// --- node helpers -------------------------------------------------------------------

Node *Parser::expressions_from(std::vector<Node *> v) {
    if (v.empty()) return nop();
    if (v.size() == 1) return v[0];
    Node *e = make(NodeKind::Expressions);
    e->exps = std::move(v);
    return e;
}

Node *Parser::new_expressions(std::vector<Node *> v) {
    Node *e = make(NodeKind::Expressions);
    e->exps = std::move(v);
    return e;
}

Node *Parser::var_node(const std::string &name) {
    Node *n = make(NodeKind::Var);
    n->name = name;
    return n;
}

Node *Parser::call_node(Node *obj, const std::string &name, std::vector<Node *> args, Node *block, Node *block_arg,
                        const std::vector<Node *> *named_args, bool global) {
    Node *n = make(NodeKind::Call);
    n->obj = obj;
    n->name = name;
    n->args = std::move(args);
    n->block = block;
    n->block_arg = block_arg;
    if (named_args) {
        n->named_args = *named_args;
        n->has_named_args = true;
    }
    n->global = global;
    return n;
}

Node *Parser::path_global(const std::string &name) {
    Node *n = make(NodeKind::Path);
    n->names = {name};
    n->global = true;
    return n;
}

Node *Parser::string_literal(const std::string &s) {
    Node *n = make(NodeKind::StringLiteral);
    n->value = s;
    return n;
}

Node *Parser::number_literal(long long v) {
    Node *n = make(NodeKind::NumberLiteral);
    n->value = std::to_string(v);
    n->number_kind = NumberKind::I32;
    return n;
}

Node *Parser::unary(NodeKind k, Node *exp) {
    Node *n = make(k);
    n->exp = exp;
    return n;
}

Node *Parser::concat_expressions(Node *x, Node *y) {
    if (y->is(NodeKind::Nop)) return x;
    if (x->is(NodeKind::Nop)) return y;
    if (x->is(NodeKind::Expressions) && x->keyword == ExpressionsKeyword::None) {
        if (y->is(NodeKind::Expressions) && y->keyword == ExpressionsKeyword::None) {
            x->exps.insert(x->exps.end(), y->exps.begin(), y->exps.end());
        } else {
            x->exps.push_back(y);
        }
        return at_end(x, EndLoc(y));
    } else if (y->is(NodeKind::Expressions) && y->keyword == ExpressionsKeyword::None) {
        y->exps.insert(y->exps.begin(), x);
        return at(y, Loc(x));
    }
    Node *e = new_expressions({x, y});
    return at_end(at(e, Loc(x)), EndLoc(y));
}

// --- expressions ----------------------------------------------------------------------

Node *Parser::parse_expressions() {
    return preserve_stop_on_do(false, [&] { return parse_expressions_internal(); });
}

Node *Parser::parse_expressions_internal() {
    if (end_token()) return nop();
    Node *exp = parse_multi_assign();
    slash_is_regex_on();
    skip_statement_end();
    if (end_token()) return exp;
    std::vector<Node *> exps;
    exps.push_back(exp);
    while (true) {
        exps.push_back(parse_multi_assign());
        skip_statement_end();
        if (end_token()) break;
    }
    return expressions_from(std::move(exps));
}

Node *Parser::parse_multi_assign() {
    const Location location = token_.location();
    bool has_lhs_splat = false;
    int lhs_splat_index = 0;
    Location lhs_splat_location;
    if (token_.type == Tok::OP_STAR) {
        has_lhs_splat = true;
        lhs_splat_index = 0;
        lhs_splat_location = token_.location();
        next_token_skip_space();
    }

    Node *last = parse_expression();
    skip_space();

    const bool last_is_target = multi_assign_target(last) || multi_assign_middle(last);

    switch (token_.type) {
        case Tok::OP_COMMA:
            if (!last_is_target) {
                if (has_lhs_splat) unexpected_token();
                if (last->is(NodeKind::Path)) raise("Multiple assignment is not allowed for constants");
                unexpected_token();
            }
            break;
        case Tok::NEWLINE:
        case Tok::OP_SEMICOLON:
            if (has_lhs_splat && !multi_assign_middle(last)) unexpected_token();
            if (!has_lhs_splat) return last;
            break;
        default:
            if (end_token()) {
                if (has_lhs_splat && !multi_assign_middle(last)) unexpected_token();
                if (!has_lhs_splat) return last;
            } else {
                unexpected_token();
            }
    }

    std::vector<Node *> exps;
    exps.push_back(last);
    int i = 0;
    int assign_index = -1;

    while (token_.type == Tok::OP_COMMA) {
        if (assign_index == -1 && multi_assign_middle(last)) assign_index = i;
        i++;
        next_token_skip_space_or_newline();
        if (token_.type == Tok::OP_STAR) {
            if (assign_index == -1) {
                if (has_lhs_splat) raise("splat assignment already specified");
                has_lhs_splat = true;
                lhs_splat_index = i;
                lhs_splat_location = token_.location();
            } else {
                unexpected_token();
            }
            next_token_skip_space();
        }
        last = parse_op_assign(false);
        if (assign_index == -1 && !multi_assign_target(last) && !multi_assign_middle(last)) unexpected_token();
        exps.push_back(last);
        skip_space();
    }

    if (assign_index == -1 && multi_assign_middle(last)) assign_index = i;
    if (assign_index == -1) unexpected_token();

    std::vector<Node *> targets;
    for (int k = 0; k < assign_index; k++) targets.push_back(multi_assign_left_hand(exps[static_cast<size_t>(k)]));

    Node *assign = exps[static_cast<size_t>(assign_index)];
    std::vector<Node *> values;
    if (assign->is(NodeKind::Assign)) {
        targets.push_back(multi_assign_left_hand(assign->target));
        values.push_back(assign->value_node);
    } else if (assign->is(NodeKind::Call)) {
        assign->name = assign->name.substr(0, assign->name.size() - 1);
        targets.push_back(assign);
        values.push_back(assign->args.back());
        assign->args.pop_back();
    } else {
        raise("BUG: multi_assign index expression can only be Assign or Call");
    }

    if (has_lhs_splat) {
        Node *t = targets[static_cast<size_t>(lhs_splat_index)];
        targets[static_cast<size_t>(lhs_splat_index)] = at(unary(NodeKind::Splat, t), lhs_splat_location);
    }

    for (size_t k = static_cast<size_t>(assign_index) + 1; k < exps.size(); k++) values.push_back(exps[k]);
    if (values.size() != 1) {
        if (has_lhs_splat) {
            if (targets.size() - 1 > values.size()) raise("Multiple assignment count mismatch", location);
        } else {
            if (targets.size() != values.size()) raise("Multiple assignment count mismatch", location);
        }
    }

    Node *multi = make(NodeKind::MultiAssign);
    multi->exps = std::move(targets);
    multi->exps2 = std::move(values);
    at(multi, location);
    return parse_expression_suffix(multi, location);
}

bool Parser::multi_assign_target(Node *exp) {
    switch (exp->kind) {
        case NodeKind::Underscore: case NodeKind::Var: case NodeKind::InstanceVar: case NodeKind::ClassVar:
        case NodeKind::Global:
            return true;
        case NodeKind::Call:
            return !exp->has_parentheses && !exp->block && (!exp->has_any_args() || exp->name == "[]");
        default:
            return false;
    }
}

bool Parser::multi_assign_middle(Node *exp) {
    if (exp->is(NodeKind::Assign)) return true;
    if (exp->is(NodeKind::Call)) return Lexer::is_setter(exp->name) || exp->name == "[]=";
    return false;
}

Node *Parser::multi_assign_left_hand(Node *exp) {
    if (exp->is(NodeKind::Path)) raise("can't assign to constant in multiple assignment", Loc(exp));
    if (exp->is(NodeKind::Call)) {
        Node *obj = exp->obj;
        if (!obj) {
            if (exp->args.empty()) exp = at(var_node(exp->name), exp);
        } else if (obj->is(NodeKind::Global)) {
            if (obj->name == "$~" && exp->name == "[]") raise("global match data cannot be assigned to", Loc(obj));
        }
    }
    if (exp->is(NodeKind::Var)) {
        if (exp->name == "self") raise("can't change the value of self", Loc(exp));
        push_var(exp);
    }
    return exp;
}

Node *Parser::parse_expression() {
    const Location location = token_.location();
    Node *atomic = parse_op_assign();
    return parse_expression_suffix(atomic, location);
}

Node *Parser::parse_expression_suffix(Node *atomic, Location location) {
    auto suffix = [&](auto make_node) {
        slash_is_regex_on();
        next_token_skip_statement_end();
        Node *exp = parse_op_assign_no_control();
        Node *n = make_node(exp);
        return at_end(at(n, location), exp);
    };
    while (true) {
        switch (token_.type) {
            case Tok::SPACE:
                next_token();
                break;
            case Tok::IDENT:
                if (value_is(Keyword::IF)) {
                    atomic = suffix([&](Node *exp) {
                        Node *n = make(NodeKind::If);
                        n->cond = exp;
                        n->then_ = atomic;
                        n->else_ = nop();
                        return n;
                    });
                } else if (value_is(Keyword::UNLESS)) {
                    atomic = suffix([&](Node *exp) {
                        Node *n = make(NodeKind::Unless);
                        n->cond = exp;
                        n->then_ = atomic;
                        n->else_ = nop();
                        return n;
                    });
                } else if (value_is(Keyword::WHILE)) {
                    raise_at_token("trailing `while` is not supported", token_);
                } else if (value_is(Keyword::UNTIL)) {
                    raise_at_token("trailing `until` is not supported", token_);
                } else if (value_is(Keyword::RESCUE)) {
                    const Location rescue_location = token_.location();
                    next_token_skip_space();
                    Node *rescue_body = parse_op_assign();
                    Node *r = make(NodeKind::Rescue);
                    r->body = rescue_body;
                    at_end(at(r, rescue_location), rescue_body);
                    Node *ex = make(NodeKind::ExceptionHandler);
                    ex->exps = {r};
                    ex->has_exps = true;
                    if (atomic->is(NodeKind::Assign)) {
                        ex->body = atomic->value_node;
                        atomic->value_node = ex;
                    } else {
                        ex->body = atomic;
                        atomic = ex;
                    }
                    at_end(at(ex, location), rescue_body);
                    ex->suffix_ = true;
                } else if (value_is(Keyword::ENSURE)) {
                    const Location ensure_location = token_.location();
                    next_token_skip_space();
                    Node *ensure_body = parse_op_assign();
                    Node *ex = make(NodeKind::ExceptionHandler);
                    ex->ensure_ = ensure_body;
                    if (atomic->is(NodeKind::Assign)) {
                        ex->body = atomic->value_node;
                        atomic->value_node = ex;
                    } else {
                        ex->body = atomic;
                        atomic = ex;
                    }
                    at_end(at(ex, location), ensure_body);
                    ex->ensure_location = ensure_location;
                    ex->suffix_ = true;
                } else {
                    return atomic;
                }
                break;
            case Tok::OP_RPAREN: case Tok::OP_COMMA: case Tok::OP_SEMICOLON: case Tok::OP_PERCENT_RCURLY:
            case Tok::NEWLINE: case Tok::EOF_:
                return atomic;
            default:
                if (end_token()) return atomic;
                unexpected_token();
        }
    }
}

Node *Parser::parse_op_assign_no_control(bool allow_ops, bool allow_suffix) {
    check_void_expression_keyword();
    return parse_op_assign(allow_ops, allow_suffix);
}

Node *Parser::parse_op_assign(bool allow_ops, bool allow_suffix) {
    const bool has_doc = token_.has_doc;
    const std::string doc = token_.doc;
    const Location location = token_.location();

    Node *atomic = parse_question_colon();

    while (true) {
        const Location name_location = token_.location();
        if (token_.type == Tok::SPACE) {
            next_token();
            continue;
        } else if (token_.type == Tok::IDENT) {
            if (!allow_suffix) unexpected_token();
            break;
        } else if (token_.type == Tok::OP_EQ) {
            slash_is_regex_on();
            if (!can_be_assigned(atomic)) break;

            if (atomic->is(NodeKind::Call) && atomic->name == "[]") {
                next_token_skip_space_or_newline();
                atomic->name = "[]=";
                atomic->name_size_ = 0;
                Node *arg = parse_op_assign_no_control();
                atomic->args.push_back(arg);
                atomic->end_location = EndLoc(arg);
            } else {
                if (atomic->is(NodeKind::Path) && (inside_def() || inside_fun() || is_constant_assignment_)) {
                    raise("dynamic constant assignment. Constants can only be declared at the top level or inside "
                          "other types.");
                }
                if (atomic->is(NodeKind::Path)) is_constant_assignment_ = true;
                if (atomic->is(NodeKind::Var) && atomic->name == "self") raise("can't change the value of self", location);
                if (atomic->is(NodeKind::Call) && !atomic->name.empty() &&
                    (atomic->name.back() == '?' || atomic->name.back() == '!')) {
                    // `start_token` aliases the live token in the original.
                    unexpected_token();
                }
                if (atomic->is(NodeKind::Call)) atomic = at(var_node(atomic->name), atomic);

                next_token_skip_space_or_newline();

                bool needs_new_scope = false;
                switch (atomic->kind) {
                    case NodeKind::Path: needs_new_scope = true; break;
                    case NodeKind::InstanceVar: needs_new_scope = def_nest_ == 0; break;
                    case NodeKind::ClassVar: needs_new_scope = def_nest_ == 0; break;
                    case NodeKind::Var:
                        if (atomic->special_var()) assigns_special_var_ = true;
                        break;
                    default: needs_new_scope = false;
                }

                bool returned_uninitialized = false;
                Node *atomic_value = with_isolated_var_scope(needs_new_scope, [&]() -> Node * {
                    if (tok_kw(Keyword::UNINITIALIZED) &&
                        (atomic->is(NodeKind::Var) || atomic->is(NodeKind::InstanceVar) ||
                         atomic->is(NodeKind::ClassVar) || atomic->is(NodeKind::Global))) {
                        push_var(atomic);
                        next_token_skip_space();
                        Node *type = parse_bare_proc_type();
                        Node *u = make(NodeKind::UninitializedVar);
                        u->var = atomic;
                        u->declared_type = type;
                        atomic = at_end(at(u, location), type);
                        returned_uninitialized = true;
                        return atomic;
                    }
                    if (atomic->is(NodeKind::Var) && !is_var(atomic->name)) {
                        assigned_vars_.push_back(atomic->name);
                        Node *value = parse_op_assign_no_control();
                        assigned_vars_.pop_back();
                        return value;
                    }
                    return parse_op_assign_no_control();
                });
                if (returned_uninitialized) return atomic;

                if (atomic->is(NodeKind::Path)) is_constant_assignment_ = false;
                push_var(atomic);

                Node *a = make(NodeKind::Assign);
                a->target = atomic;
                a->value_node = atomic_value;
                at(a, location);
                a->assign_doc(has_doc, doc);
                atomic = a;
            }
        } else if (IsAssignmentOperator(token_.type)) {
            if (!allow_ops) unexpected_token();
            if (!can_be_assigned(atomic)) break;
            if (atomic->is(NodeKind::Path)) raise("can't reassign to constant");
            if (atomic->is(NodeKind::Var) && atomic->name == "self") raise("can't change the value of self", location);
            if (atomic->is(NodeKind::Call) && atomic->name != "[]" && !var_in_scope(atomic->name)) {
                raise(std::string("'") + TokName(token_.type) + "' before definition of '" + atomic->name + "'");
            }
            push_var(atomic);
            std::string method = TokName(token_.type);
            const std::string tos = token_.to_s();
            method = method.substr(0, tos.size() - 1);
            next_token_skip_space_or_newline();
            Node *value = parse_op_assign_no_control();
            Node *op = make(NodeKind::OpAssign);
            op->target = atomic;
            op->value = method;
            op->value_node = value;
            at(op, location);
            op->name_location = name_location;
            atomic = op;
        } else {
            break;
        }
        allow_ops = true;
    }
    return atomic;
}

Node *Parser::parse_question_colon() {
    Node *cond = parse_range();
    while (token_.type == Tok::OP_QUESTION) {
        const Location location = token_.location();
        check_void_value(cond, location);
        next_token_skip_space_or_newline();
        no_type_declaration_++;
        Node *true_val = parse_question_colon();
        skip_space_or_newline();
        check(Tok::OP_COLON);
        next_token_skip_space_or_newline();
        Node *false_val = parse_question_colon();
        no_type_declaration_--;
        Node *n = make(NodeKind::If);
        n->cond = cond;
        n->then_ = true_val;
        n->else_ = false_val;
        n->ternary = true;
        cond = at_end(at(n, cond), false_val);
    }
    return cond;
}

Node *Parser::parse_range() {
    const Location location = token_.location();
    Node *exp;
    if (token_.type == Tok::OP_PERIOD_PERIOD || token_.type == Tok::OP_PERIOD_PERIOD_PERIOD) {
        exp = nop();
    } else {
        exp = parse_or();
    }
    while (true) {
        if (token_.type == Tok::OP_PERIOD_PERIOD) {
            exp = new_range(exp, location, false);
        } else if (token_.type == Tok::OP_PERIOD_PERIOD_PERIOD) {
            exp = new_range(exp, location, true);
        } else {
            return exp;
        }
    }
}

Node *Parser::new_range(Node *exp, Location location, bool exclusive) {
    Location end_location = token_end_location();
    check_void_value(exp, location);
    next_token_skip_space();
    check_void_expression_keyword();
    Node *right;
    if (end_token() || token_.type == Tok::OP_RPAREN || token_.type == Tok::OP_COMMA ||
        token_.type == Tok::OP_SEMICOLON || token_.type == Tok::OP_EQ_GT || token_.type == Tok::NEWLINE) {
        right = nop();
    } else {
        right = parse_or();
        end_location = EndLoc(right);
    }
    Node *r = make(NodeKind::RangeLiteral);
    r->left = exp;
    r->right = right;
    r->exclusive = exclusive;
    return at_end(at(r, location), end_location);
}

namespace {
bool TokIn(Tok t, std::initializer_list<Tok> ts) {
    for (Tok x : ts) {
        if (x == t) return true;
    }
    return false;
}
}  // namespace

// parse_operator: one binary-operator precedence level.
#define CRYSTAL_PARSE_OPERATOR(fname, next, kind, right_assoc, ...)                       \
    Node *Parser::fname() {                                                               \
        const Location location = token_.location();                                     \
        Node *left = next();                                                              \
        while (true) {                                                                    \
            if (token_.type == Tok::SPACE) {                                              \
                next_token();                                                             \
            } else if (TokIn(token_.type, {__VA_ARGS__})) {                               \
                check_void_value(left, location);                                         \
                const std::string method = TokName(token_.type);                         \
                const Location name_location = token_.location();                        \
                slash_is_regex_on();                                                      \
                next_token_skip_space_or_newline();                                       \
                Node *right = right_assoc ? fname() : next();                            \
                Node *n;                                                                  \
                if (kind == NodeKind::Call) {                                             \
                    n = call_node(left, method, {right});                                 \
                } else {                                                                  \
                    n = make(kind);                                                       \
                    n->left = left;                                                       \
                    n->right = right;                                                     \
                }                                                                         \
                left = at_end(at(n, location), right);                                    \
                if (left->is(NodeKind::Call)) left->name_location = name_location;        \
            } else {                                                                      \
                return left;                                                              \
            }                                                                             \
        }                                                                                 \
    }

CRYSTAL_PARSE_OPERATOR(parse_or, parse_and, NodeKind::Or, false, Tok::OP_BAR_BAR)
CRYSTAL_PARSE_OPERATOR(parse_and, parse_equality, NodeKind::And, false, Tok::OP_AMP_AMP)
CRYSTAL_PARSE_OPERATOR(parse_equality, parse_cmp, NodeKind::Call, false, Tok::OP_LT, Tok::OP_LT_EQ, Tok::OP_GT,
                       Tok::OP_GT_EQ, Tok::OP_LT_EQ_GT)
CRYSTAL_PARSE_OPERATOR(parse_cmp, parse_logical_or, NodeKind::Call, false, Tok::OP_EQ_EQ, Tok::OP_BANG_EQ,
                       Tok::OP_EQ_TILDE, Tok::OP_BANG_TILDE, Tok::OP_EQ_EQ_EQ)
CRYSTAL_PARSE_OPERATOR(parse_logical_or, parse_logical_and, NodeKind::Call, false, Tok::OP_BAR, Tok::OP_CARET)
CRYSTAL_PARSE_OPERATOR(parse_logical_and, parse_shift, NodeKind::Call, false, Tok::OP_AMP)
CRYSTAL_PARSE_OPERATOR(parse_shift, parse_add_or_sub, NodeKind::Call, false, Tok::OP_LT_LT, Tok::OP_GT_GT)
CRYSTAL_PARSE_OPERATOR(parse_mul_or_div, parse_pow, NodeKind::Call, false, Tok::OP_STAR, Tok::OP_SLASH,
                       Tok::OP_SLASH_SLASH, Tok::OP_PERCENT, Tok::OP_AMP_STAR)
CRYSTAL_PARSE_OPERATOR(parse_pow, parse_prefix, NodeKind::Call, true, Tok::OP_STAR_STAR, Tok::OP_AMP_STAR_STAR)

#undef CRYSTAL_PARSE_OPERATOR

Node *Parser::parse_add_or_sub() {
    const Location location = token_.location();
    Node *left = parse_mul_or_div();
    while (true) {
        if (token_.type == Tok::SPACE) {
            next_token();
        } else if (TokIn(token_.type, {Tok::OP_PLUS, Tok::OP_MINUS, Tok::OP_AMP_PLUS, Tok::OP_AMP_MINUS})) {
            check_void_value(left, location);
            const std::string method = TokName(token_.type);
            const Location name_location = token_.location();
            next_token_skip_space_or_newline();
            Node *right = parse_mul_or_div();
            left = at_end(at(call_node(left, method, {right}), location), right);
            left->name_location = name_location;
        } else if (token_.type == Tok::NUMBER) {
            const std::string v = token_.value_s();
            const char c = v.empty() ? '\0' : v[0];
            if (c == '+' || c == '-') {
                const std::string method(1, c);
                const Location name_location = token_.location();
                // Go back to the +/-, advance one char and continue from there.
                set_current_pos(token_.start + 1);
                next_token();
                Node *right = parse_mul_or_div();
                left = at_end(at(call_node(left, method, {right}), location), right);
                left->name_location = name_location;
            } else {
                return left;
            }
        } else {
            return left;
        }
    }
}

Node *Parser::parse_prefix() {
    const Location name_location = token_.location();
    const Tok token_type = token_.type;
    if (IsUnaryOperator(token_type)) {
        const Location location = token_.location();
        next_token_skip_space_or_newline();
        check_void_expression_keyword();
        Node *arg = parse_prefix();
        if (token_type == Tok::OP_BANG) return at_end(at(unary(NodeKind::Not, arg), location), arg);
        Node *call = at_end(at(call_node(arg, TokName(token_type)), location), arg);
        call->name_location = name_location;
        return call;
    }
    return parse_atomic_with_method();
}

namespace {
const std::initializer_list<Tok> kAtomicWithMethodCheck = {
    Tok::IDENT, Tok::CONST, Tok::OP_PLUS, Tok::OP_MINUS, Tok::OP_STAR, Tok::OP_SLASH, Tok::OP_SLASH_SLASH,
    Tok::OP_PERCENT, Tok::OP_BAR, Tok::OP_AMP, Tok::OP_CARET, Tok::OP_TILDE, Tok::OP_BANG, Tok::OP_STAR_STAR,
    Tok::OP_LT_LT, Tok::OP_LT, Tok::OP_LT_EQ, Tok::OP_EQ_EQ, Tok::OP_BANG_EQ, Tok::OP_EQ_TILDE, Tok::OP_BANG_TILDE,
    Tok::OP_GT_GT, Tok::OP_GT, Tok::OP_GT_EQ, Tok::OP_LT_EQ_GT, Tok::OP_EQ_EQ_EQ, Tok::OP_LSQUARE_RSQUARE,
    Tok::OP_LSQUARE_RSQUARE_EQ, Tok::OP_LSQUARE_RSQUARE_QUESTION, Tok::OP_LSQUARE, Tok::OP_AMP_PLUS,
    Tok::OP_AMP_MINUS, Tok::OP_AMP_STAR, Tok::OP_AMP_STAR_STAR,
};
}  // namespace

Node *Parser::parse_atomic_with_method() {
    const Location location = token_.location();
    Node *atomic = parse_atomic();
    return parse_atomic_method_suffix(atomic, location);
}

Node *Parser::parse_atomic_method_suffix(Node *atomic, Location location) {
    while (true) {
        switch (token_.type) {
            case Tok::SPACE:
                next_token();
                break;
            case Tok::NEWLINE: {
                if (atomic->is(NodeKind::ClassDef) || atomic->is(NodeKind::ModuleDef) || atomic->is(NodeKind::EnumDef) ||
                    atomic->is(NodeKind::FunDef) || atomic->is(NodeKind::Def)) {
                    return atomic;
                }
                // Allow '.' after newline for chaining calls.
                const bool ok = lookahead(
                    [&] {
                        next_token_skip_space_or_newline();
                        return token_.type == Tok::OP_PERIOD;
                    },
                    true);
                if (!ok) return atomic;
                break;
            }
            case Tok::OP_PERIOD: {
                check_void_value(atomic, location);
                wants_regex_ = false;
                wants_def_or_macro_name_begin();
                try {
                    next_token_skip_space_or_newline();
                } catch (...) {
                    wants_def_or_macro_name_end();
                    throw;
                }
                wants_def_or_macro_name_end();

                if (token_.type == Tok::INSTANCE_VAR) {
                    atomic = at(parse_read_instance_var(atomic), location);
                    break;
                }

                check(kAtomicWithMethodCheck);

                if (value_is(Keyword::IS_A_QUESTION)) {
                    atomic = at(parse_is_a(atomic), location);
                } else if (value_is(Keyword::AS)) {
                    atomic = at(parse_as(atomic), location);
                } else if (value_is(Keyword::AS_QUESTION)) {
                    atomic = at(parse_as(atomic, NodeKind::NilableCast), location);
                } else if (value_is(Keyword::RESPONDS_TO_QUESTION)) {
                    atomic = at(parse_responds_to(atomic), location);
                } else if (!in_macro_expression_ && value_is(Keyword::NIL_QUESTION)) {
                    atomic = at(parse_nil_q(atomic), location);
                } else if (token_.type == Tok::OP_BANG) {
                    atomic = at(parse_negation_suffix(atomic), location);
                    atomic = parse_atomic_method_suffix_special(atomic, location);
                } else if (token_.type == Tok::OP_LSQUARE) {
                    return parse_atomic_method_suffix(atomic, location);
                } else {
                    std::string name =
                        (token_.type == Tok::IDENT || token_.type == Tok::CONST) ? value_s() : TokName(token_.type);
                    const Location name_location = token_.location();
                    Location end_location = token_end_location();

                    wants_regex_ = false;
                    next_token();

                    bool space_consumed = false;
                    if (token_.type == Tok::SPACE) {
                        wants_regex_ = true;
                        next_token();
                        space_consumed = true;
                    }

                    if (token_.type == Tok::OP_EQ) {
                        Node *call = call_node(atomic, name);
                        if (!can_be_assigned(call)) unexpected_token();
                        next_token();
                        Node *arg;
                        if (token_.type == Tok::OP_LPAREN) {
                            if (current_char() == '*') {
                                next_token_skip_space();
                                arg = parse_single_arg();
                                check(Tok::OP_RPAREN);
                                end_location = token_end_location();
                                next_token();
                            } else {
                                arg = parse_op_assign_no_control();
                                end_location = EndLoc(arg);
                            }
                        } else {
                            skip_space_or_newline();
                            arg = parse_single_arg();
                            end_location = EndLoc(arg);
                        }
                        at_end(at(call, location), end_location);
                        call->name = name + "=";
                        call->args = {arg};
                        call->name_location = name_location;
                        atomic = call;
                        break;
                    } else if (IsAssignmentOperator(token_.type)) {
                        Node *call = call_node(atomic, name);
                        if (!can_be_assigned(call)) unexpected_token();
                        const Location op_name_location = token_.location();
                        std::string method = TokName(token_.type);
                        method = method.substr(0, method.size() - 1);
                        next_token_skip_space_or_newline();
                        Node *value = parse_op_assign();
                        at(call, location);
                        call->name_location = name_location;
                        Node *op = make(NodeKind::OpAssign);
                        op->target = call;
                        op->value = method;
                        op->value_node = value;
                        at(op, location);
                        op->name_location = op_name_location;
                        atomic = op;
                        break;
                    }

                    std::optional<CallArgs> call_args = preserve_stop_on_do(false, [&] {
                        return space_consumed ? parse_call_args_space_consumed() : parse_call_args();
                    });
                    std::vector<Node *> args;
                    Node *block = nullptr;
                    Node *block_arg = nullptr;
                    std::vector<Node *> named_args;
                    bool has_named = false;
                    bool has_parentheses = false;
                    if (call_args) {
                        args = call_args->args;
                        block = call_args->block;
                        block_arg = call_args->block_arg;
                        named_args = call_args->named_args;
                        has_named = call_args->has_named_args;
                        has_parentheses = call_args->has_parentheses;
                    }
                    block = parse_block(block, stop_on_do_);
                    Node *call = call_node(atomic, name, args, block, block_arg, has_named ? &named_args : nullptr);
                    call->has_parentheses = has_parentheses;
                    call->name_location = name_location;
                    if (block && EndLoc(block).known()) {
                        call->end_location = EndLoc(block);
                    } else if (call_args && call_args->end_location.known()) {
                        call->end_location = call_args->end_location;
                    } else {
                        call->end_location = end_location;
                    }
                    at(call, location);
                    atomic = call;
                }
                break;
            }
            case Tok::OP_LSQUARE_RSQUARE: {
                check_void_value(atomic, location);
                const Location name_location = token_.location();
                const Location end_location = token_end_location();
                wants_regex_ = false;
                next_token_skip_space();
                Node *call = at(call_node(atomic, "[]"), location);
                call->name_location = name_location;
                call->end_location = end_location;
                call->name_size_ = 0;
                atomic = call;
                break;
            }
            case Tok::OP_LSQUARE: {
                check_void_value(atomic, location);
                const Location name_location = token_.location();
                next_token_skip_space_or_newline();
                std::optional<CallArgs> call_args = preserve_stop_on_do(false, [&] {
                    return parse_call_args_space_consumed(false, true, true, Tok::OP_RSQUARE, true);
                });
                skip_space_or_newline();
                check(Tok::OP_RSQUARE);
                Location end_location = token_end_location();
                wants_regex_ = false;
                next_token();

                std::vector<Node *> args;
                Node *block = nullptr;
                Node *block_arg = nullptr;
                std::vector<Node *> named_args;
                bool has_named = false;
                if (call_args) {
                    args = call_args->args;
                    block = call_args->block;
                    block_arg = call_args->block_arg;
                    named_args = call_args->named_args;
                    has_named = call_args->has_named_args;
                }

                std::string method_name;
                if (token_.type == Tok::OP_QUESTION) {
                    method_name = "[]?";
                    end_location = token_end_location();
                    next_token_skip_space();
                } else {
                    method_name = "[]";
                    skip_space();
                }

                Node *call =
                    at(call_node(atomic, method_name, args, block, block_arg, has_named ? &named_args : nullptr), location);
                call->name_location = name_location;
                call->end_location = end_location;
                call->name_size_ = 0;
                call->args_in_brackets = true;
                atomic = call;
                break;
            }
            default:
                return atomic;
        }
    }
}

Node *Parser::parse_atomic_method_suffix_special(Node *call, Location location) {
    if (token_.type == Tok::OP_PERIOD || token_.type == Tok::OP_LSQUARE || token_.type == Tok::OP_LSQUARE_RSQUARE) {
        return parse_atomic_method_suffix(call, location);
    }
    return call;
}

Node *Parser::parse_single_arg() {
    if (token_.type == Tok::OP_STAR) {
        const Location location = token_.location();
        next_token_skip_space();
        Node *arg = parse_op_assign_no_control();
        return at_end(at(unary(NodeKind::Splat, arg), location), arg);
    }
    return parse_op_assign_no_control();
}

Node *Parser::parse_is_a(Node *atomic) {
    next_token_skip_space();
    Node *type;
    Location end_location;
    if (token_.type == Tok::OP_LPAREN) {
        next_token_skip_space_or_newline();
        type = parse_bare_proc_type();
        skip_space_or_newline();
        check(Tok::OP_RPAREN);
        end_location = token_end_location();
        next_token_skip_space();
    } else {
        type = parse_union_type();
        end_location = EndLoc(type);
    }
    Node *n = make(NodeKind::IsA);
    n->obj = atomic;
    n->right = type;
    return at_end(n, end_location);
}

Node *Parser::parse_as(Node *atomic, NodeKind klass) {
    next_token_skip_space();
    Node *type;
    Location end_location;
    if (token_.type == Tok::OP_LPAREN) {
        next_token_skip_space_or_newline();
        type = parse_bare_proc_type();
        skip_space_or_newline();
        check(Tok::OP_RPAREN);
        end_location = token_end_location();
        next_token_skip_space();
    } else {
        type = parse_union_type();
        end_location = token_end_location();
    }
    Node *n = make(klass);
    n->obj = atomic;
    n->right = type;
    return at_end(n, end_location);
}

Node *Parser::parse_responds_to(Node *atomic) {
    next_token();
    std::string name;
    Location end_location;
    if (token_.type == Tok::OP_LPAREN) {
        next_token_skip_space_or_newline();
        name = parse_responds_to_name();
        next_token_skip_space_or_newline();
        check(Tok::OP_RPAREN);
        end_location = token_end_location();
        next_token_skip_space();
    } else if (token_.type == Tok::SPACE) {
        next_token();
        name = parse_responds_to_name();
        end_location = token_end_location();
        next_token_skip_space();
    } else {
        unexpected_token("expected space or '('");
    }
    Node *n = make(NodeKind::RespondsTo);
    n->obj = atomic;
    n->name = name;
    return at_end(n, end_location);
}

std::string Parser::parse_responds_to_name() {
    if (token_.type != Tok::SYMBOL) unexpected_token("expected symbol");
    return value_s();
}

Node *Parser::parse_nil_q(Node *atomic) {
    Location end_location = token_end_location();
    next_token();
    if (token_.type == Tok::OP_LPAREN) {
        next_token_skip_space_or_newline();
        check(Tok::OP_RPAREN);
        end_location = token_end_location();
        next_token_skip_space();
    }
    Node *n = make(NodeKind::IsA);
    n->obj = atomic;
    n->right = path_global("Nil");
    n->nil_check = true;
    return at_end(n, end_location);
}

Node *Parser::parse_negation_suffix(Node *atomic) {
    Location end_location = token_end_location();
    next_token();
    if (token_.type == Tok::OP_LPAREN) {
        next_token_skip_space_or_newline();
        check(Tok::OP_RPAREN);
        end_location = token_end_location();
        next_token_skip_space();
    }
    return at_end(unary(NodeKind::Not, atomic), end_location);
}

Node *Parser::parse_read_instance_var(Node *atomic) {
    const std::string ivar_name = value_s();
    const Location end_location = token_end_location();
    next_token_skip_space();
    Node *n = make(NodeKind::ReadInstanceVar);
    n->obj = atomic;
    n->name = ivar_name;
    n->end_location = end_location;
    return n;
}

Node *Parser::parse_atomic() {
    const Location location = token_.location();
    Node *atomic = parse_atomic_without_location();
    if (!Loc(atomic).known()) atomic->location = location;
    return atomic;
}

Node *Parser::parse_atomic_without_location() {
    switch (token_.type) {
        case Tok::OP_LPAREN: return parse_parenthesized_expression();
        case Tok::OP_LSQUARE_RSQUARE: return parse_empty_array_literal();
        case Tok::OP_LSQUARE: return parse_array_literal();
        case Tok::OP_LCURLY: return parse_hash_or_tuple_literal();
        case Tok::OP_LCURLY_LCURLY: return parse_percent_macro_expression();
        case Tok::OP_LCURLY_PERCENT: return parse_percent_macro_control();
        case Tok::OP_COLON_COLON: return parse_generic_or_global_call();
        case Tok::OP_MINUS_GT: return parse_fun_literal();
        case Tok::OP_AT_LSQUARE: return parse_annotation();
        case Tok::NUMBER: {
            wants_regex_ = false;
            Node *n = make(NodeKind::NumberLiteral);
            n->value = value_s();
            n->number_kind = token_.number_kind;
            return node_and_next_token(n);
        }
        case Tok::CHAR: {
            Node *n = make(NodeKind::CharLiteral);
            n->ch = token_.ch;
            return node_and_next_token(n);
        }
        case Tok::STRING:
        case Tok::DELIMITER_START:
            return parse_delimiter();
        case Tok::STRING_ARRAY_START: return parse_string_or_symbol_array(NodeKind::StringLiteral, "String");
        case Tok::SYMBOL_ARRAY_START: return parse_string_or_symbol_array(NodeKind::SymbolLiteral, "Symbol");
        case Tok::SYMBOL: {
            Node *n = make(NodeKind::SymbolLiteral);
            n->value = value_s();
            return node_and_next_token(n);
        }
        case Tok::GLOBAL:
            raise("$global_variables are not supported, use @@class_variables instead");
        case Tok::OP_DOLLAR_TILDE:
        case Tok::OP_DOLLAR_QUESTION: {
            const Location location = token_.location();
            Node *var = at(var_node(token_.to_s()), location);
            bool eq = false;
            lookahead(
                [&] {
                    next_token_skip_space();
                    eq = token_.type == Tok::OP_EQ;
                    return false;
                },
                true);
            if (eq) {
                push_var(var);
                return node_and_next_token(var);
            }
            Node *g = make(NodeKind::Global);
            g->name = var->name;
            return node_and_next_token(at(g, location));
        }
        case Tok::GLOBAL_MATCH_DATA_INDEX: {
            bool eq = false;
            lookahead(
                [&] {
                    next_token_skip_space();
                    eq = token_.type == Tok::OP_EQ;
                    return false;
                },
                true);
            if (eq) raise("global match data cannot be assigned to");
            std::string value = value_s();
            std::string method;
            if (!value.empty() && value.back() == '?') {
                method = "[]?";
                value.pop_back();
            } else {
                method = "[]";
            }
            const Location location = token_.location();
            long long index = 0;
            bool fits = !value.empty();
            for (char c : value) {
                if (c < '0' || c > '9') {
                    fits = false;
                    break;
                }
                index = index * 10 + (c - '0');
                if (index > 2147483647LL) {
                    fits = false;
                    break;
                }
            }
            if (!fits) raise("Index $" + value + " doesn't fit in an Int32");
            Node *g = make(NodeKind::Global);
            g->name = "$~";
            at(g, location);
            return node_and_next_token(call_node(g, method, {number_literal(index)}));
        }
        case Tok::MAGIC_LINE:
            return node_and_next_token(number_literal(token_.location().line));
        case Tok::MAGIC_END_LINE:
            raise_at_token("__END_LINE__ can only be used in default parameter value", token_);
        case Tok::MAGIC_FILE:
            return node_and_next_token(string_literal(filename.empty() ? "?" : filename));
        case Tok::MAGIC_DIR: {
            std::string dir = "?";
            if (!filename.empty()) {
                const size_t slash = filename.find_last_of('/');
                dir = slash == std::string::npos ? "." : (slash == 0 ? "/" : filename.substr(0, slash));
            }
            return node_and_next_token(string_literal(dir));
        }
        case Tok::IDENT:
            if (token_.vkind == TokValue::Keyword) {
                switch (token_.kw) {
                    case Keyword::BEGIN: return check_type_declaration([&] { return parse_begin(); });
                    case Keyword::NIL:
                        return check_type_declaration([&] { return node_and_next_token(make(NodeKind::NilLiteral)); });
                    case Keyword::TRUE:
                        return check_type_declaration([&] {
                            Node *n = make(NodeKind::BoolLiteral);
                            n->bool_value = true;
                            return node_and_next_token(n);
                        });
                    case Keyword::FALSE:
                        return check_type_declaration([&] {
                            Node *n = make(NodeKind::BoolLiteral);
                            n->bool_value = false;
                            return node_and_next_token(n);
                        });
                    case Keyword::YIELD: return check_type_declaration([&] { return parse_yield(); });
                    case Keyword::WITH: return check_type_declaration([&] { return parse_yield_with_scope(); });
                    case Keyword::ABSTRACT:
                        return check_type_declaration([&] {
                            return check_not_inside_def("can't use abstract", [&]() -> Node * {
                                const Token doc_token = token_;
                                next_token_skip_space_or_newline();
                                if (value_is(Keyword::DEF)) return parse_def(true, &doc_token);
                                if (value_is(Keyword::CLASS)) return parse_class_def(true, false, &doc_token);
                                if (value_is(Keyword::STRUCT)) return parse_class_def(true, true, &doc_token);
                                unexpected_token();
                            });
                        });
                    case Keyword::DEF:
                        return check_type_declaration(
                            [&] { return check_not_inside_def("can't define def", [&] { return parse_def(); }); });
                    case Keyword::MACRO:
                        return check_type_declaration(
                            [&] { return check_not_inside_def("can't define macro", [&] { return parse_macro(); }); });
                    case Keyword::REQUIRE:
                        return check_type_declaration(
                            [&] { return check_not_inside_def("can't require", [&] { return parse_require(); }); });
                    case Keyword::CASE: return check_type_declaration([&] { return parse_case(); });
                    case Keyword::SELECT: return check_type_declaration([&] { return parse_select(); });
                    case Keyword::IF: return check_type_declaration([&] { return parse_if(); });
                    case Keyword::UNLESS: return check_type_declaration([&] { return parse_unless(); });
                    case Keyword::INCLUDE:
                        return check_type_declaration([&] {
                            return check_not_inside_def("can't include",
                                                        [&] { return parse_include_or_extend(NodeKind::Include); });
                        });
                    case Keyword::EXTEND:
                        return check_type_declaration([&] {
                            return check_not_inside_def("can't extend",
                                                        [&] { return parse_include_or_extend(NodeKind::Extend); });
                        });
                    case Keyword::CLASS:
                        return check_type_declaration([&] {
                            return check_not_inside_def("can't define class", [&] { return parse_class_def(); });
                        });
                    case Keyword::STRUCT:
                        return check_type_declaration([&] {
                            return check_not_inside_def("can't define struct",
                                                        [&] { return parse_class_def(false, true); });
                        });
                    case Keyword::MODULE:
                        return check_type_declaration([&] {
                            return check_not_inside_def("can't define module", [&] { return parse_module_def(); });
                        });
                    case Keyword::ENUM:
                        return check_type_declaration([&] {
                            return check_not_inside_def("can't define enum", [&] { return parse_enum_def(); });
                        });
                    case Keyword::WHILE:
                        return check_type_declaration([&] { return parse_while_or_until(NodeKind::While); });
                    case Keyword::UNTIL:
                        return check_type_declaration([&] { return parse_while_or_until(NodeKind::Until); });
                    case Keyword::RETURN:
                        return check_type_declaration([&] { return parse_control_expression(NodeKind::Return); });
                    case Keyword::NEXT:
                        return check_type_declaration([&] { return parse_control_expression(NodeKind::Next); });
                    case Keyword::BREAK:
                        return check_type_declaration([&] { return parse_control_expression(NodeKind::Break); });
                    case Keyword::LIB:
                        return check_type_declaration(
                            [&] { return check_not_inside_def("can't define lib", [&] { return parse_lib(); }); });
                    case Keyword::FUN:
                        return check_type_declaration([&] {
                            return check_not_inside_def("can't define fun", [&] { return parse_fun_def(true, true); });
                        });
                    case Keyword::ALIAS:
                        return check_type_declaration(
                            [&] { return check_not_inside_def("can't define alias", [&] { return parse_alias(); }); });
                    case Keyword::POINTEROF: return check_type_declaration([&] { return parse_pointerof(); });
                    case Keyword::SIZEOF:
                        return check_type_declaration([&] { return parse_sizeof(NodeKind::SizeOf); });
                    case Keyword::INSTANCE_SIZEOF:
                        return check_type_declaration([&] { return parse_sizeof(NodeKind::InstanceSizeOf); });
                    case Keyword::ALIGNOF:
                        return check_type_declaration([&] { return parse_sizeof(NodeKind::AlignOf); });
                    case Keyword::INSTANCE_ALIGNOF:
                        return check_type_declaration([&] { return parse_sizeof(NodeKind::InstanceAlignOf); });
                    case Keyword::OFFSETOF: return check_type_declaration([&] { return parse_offsetof(); });
                    case Keyword::TYPEOF: return check_type_declaration([&] { return parse_typeof(); });
                    case Keyword::PRIVATE:
                        return check_type_declaration([&] { return parse_visibility_modifier(Visibility::Private); });
                    case Keyword::PROTECTED:
                        return check_type_declaration([&] { return parse_visibility_modifier(Visibility::Protected); });
                    case Keyword::ASM: return check_type_declaration([&] { return parse_asm(); });
                    case Keyword::ANNOTATION:
                        return check_type_declaration([&] {
                            return check_not_inside_def("can't define annotation",
                                                        [&] { return parse_annotation_def(); });
                        });
                    default:
                        return set_visibility(parse_var_or_call());
                }
            }
            return set_visibility(parse_var_or_call());
        case Tok::CONST: return parse_generic_or_custom_literal();
        case Tok::INSTANCE_VAR:
            if (in_macro_expression_ && value_is_str("@type")) is_macro_def_ = true;
            return new_node_check_type_declaration(NodeKind::InstanceVar);
        case Tok::CLASS_VAR: return new_node_check_type_declaration(NodeKind::ClassVar);
        case Tok::UNDERSCORE: return node_and_next_token(make(NodeKind::Underscore));
        default: unexpected_token_in_atomic();
    }
}

Node *Parser::parse_type_declaration(Node *var) {
    next_token_skip_space_or_newline();
    Node *var_type = parse_bare_proc_type();
    skip_space();
    Node *value = nullptr;
    if (token_.type == Tok::OP_EQ) {
        next_token_skip_space_or_newline();
        value = parse_op_assign_no_control();
    }
    Node *n = make(NodeKind::TypeDeclaration);
    n->var = var;
    n->declared_type = var_type;
    n->value_node = value;
    at(n, var);
    return at_end(n, value ? value : var_type);
}

bool Parser::next_comes_colon_space() {
    if (no_type_declaration_ != 0) return false;
    const int pos = current_pos();
    auto ws = [](char32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; };
    while (ws(current_char())) next_char_no_column_increment();
    bool comes_colon_space = current_char() == ':';
    if (comes_colon_space) {
        next_char_no_column_increment();
        comes_colon_space = ws(current_char());
    }
    set_current_pos(pos);
    return comes_colon_space;
}

Node *Parser::new_node_check_type_declaration(NodeKind klass) {
    const std::string name = value_s();
    Node *var = make(klass);
    var->name = name;
    at(var, token_.location());
    var->end_location = token_end_location();
    wants_regex_ = false;
    next_token();
    const bool space_after_name = token_.type == Tok::SPACE;
    skip_space();
    if (no_type_declaration_ == 0 && token_.type == Tok::OP_COLON) {
        if (!space_after_name) {
            warn(token_.location(),
                 "space required before colon in type declaration (run `crystal tool format` to fix this)");
        }
        return parse_type_declaration(var);
    }
    return var;
}

Node *Parser::parse_generic_or_custom_literal() {
    Node *type = parse_generic(true);
    skip_space();
    return parse_custom_literal(type);
}

Node *Parser::parse_custom_literal(Node *type) {
    if (token_.type == Tok::OP_LCURLY) {
        Node *tuple_or_hash = parse_hash_or_tuple_literal(false);
        skip_space();
        if (tok_kw(Keyword::OF)) unexpected_token();
        if (tuple_or_hash->is(NodeKind::TupleLiteral)) {
            Node *ary = make(NodeKind::ArrayLiteral);
            ary->exps = tuple_or_hash->exps;
            ary->name_node = type;
            return at(ary, tuple_or_hash);
        }
        if (tuple_or_hash->is(NodeKind::HashLiteral)) {
            tuple_or_hash->name_node = type;
            return tuple_or_hash;
        }
        raise("BUG: tuple_or_hash should be tuple or hash");
    }
    return type;
}

Node *Parser::parse_annotation() {
    bool has_doc;
    const std::string doc = doc_or_empty(has_doc);
    const Location location = token_.location();
    next_token_skip_space();
    Node *name = parse_path();
    skip_space();

    std::vector<Node *> args;
    std::vector<Node *> named_args;
    bool has_named = false;

    if (token_.type == Tok::OP_LPAREN) {
        open("annotation", token_.location(), [&] {
            next_token_skip_space_or_newline();
            while (token_.type != Tok::OP_RPAREN) {
                if (token_.type == Tok::IDENT && current_char() == ':') {
                    named_args = parse_named_args(token_.location(), nullptr, true);
                    has_named = true;
                    check(Tok::OP_RPAREN);
                    break;
                } else {
                    args.push_back(parse_call_arg());
                }
                skip_space_or_newline();
                if (token_.type == Tok::OP_COMMA) next_token_skip_space_or_newline();
            }
            next_token_skip_space();
            return 0;
        });
    }
    check(Tok::OP_RSQUARE);
    const Location end_location = token_end_location();
    wants_regex_ = false;
    next_token_skip_space();

    Node *ann = make(NodeKind::Annotation);
    ann->name_node = name;
    ann->args = std::move(args);
    if (has_named) {
        ann->named_args = std::move(named_args);
        ann->has_named_args = true;
    }
    at_end(at(ann, location), end_location);
    ann->assign_doc(has_doc, doc);
    return ann;
}

Node *Parser::parse_begin() {
    const Location begin_location = token_.location();
    slash_is_regex_on();
    next_token_skip_statement_end();
    Node *exps = parse_expressions();
    auto [node, end_location] = parse_exception_handler(exps, false, begin_location);
    if (!node->is(NodeKind::ExceptionHandler) &&
        (!node->is(NodeKind::Expressions) || node->keyword != ExpressionsKeyword::None)) {
        node = new_expressions({node});
    }
    at_end(at(node, begin_location), end_location);
    if (node->is(NodeKind::Expressions)) node->keyword = ExpressionsKeyword::Begin;
    return node;
}

std::pair<Node *, Location> Parser::parse_exception_handler(Node *exp, bool implicit, Location begin_location) {
    bool has_rescues = false;
    std::vector<Node *> rescues;
    Node *a_else = nullptr;
    Node *a_ensure = nullptr;
    if (!begin_location.known()) begin_location = Loc(exp);
    Location else_location, ensure_location;

    if (tok_kw(Keyword::RESCUE)) {
        has_rescues = true;
        bool found_catch_all = false;
        while (true) {
            if (!begin_location.known()) begin_location = token_.location();
            const Location location = token_.location();
            Node *a_rescue = parse_rescue();
            if (a_rescue->has_exps) {
                if (found_catch_all) raise("specific rescue must come before catch-all rescue", location);
            } else {
                if (found_catch_all) raise("catch-all rescue can only be specified once", location);
                found_catch_all = true;
            }
            rescues.push_back(a_rescue);
            if (!tok_kw(Keyword::RESCUE)) break;
        }
    }

    if (tok_kw(Keyword::ELSE)) {
        if (!has_rescues) raise_at_token("'else' is useless without 'rescue'", token_, 4);
        else_location = token_.location();
        if (!begin_location.known()) begin_location = token_.location();
        next_token_skip_statement_end();
        a_else = parse_expressions();
        skip_statement_end();
    }

    if (tok_kw(Keyword::ENSURE)) {
        ensure_location = token_.location();
        if (!begin_location.known()) begin_location = token_.location();
        next_token_skip_statement_end();
        a_ensure = parse_expressions();
        skip_statement_end();
    }

    const Location end_location = token_end_location();
    check_ident(Keyword::END);
    slash_is_not_regex();
    next_token_skip_space();

    if (has_rescues || a_ensure) {
        Node *ex = make(NodeKind::ExceptionHandler);
        ex->body = exp;
        if (has_rescues) {
            ex->exps = std::move(rescues);
            ex->has_exps = true;
        }
        ex->else_ = a_else;
        ex->ensure_ = a_ensure;
        at_end(at(ex, begin_location), end_location);
        if (implicit) ex->implicit = true;
        ex->else_location = else_location;
        ex->ensure_location = ensure_location;
        return {ex, end_location};
    }
    return {exp, end_location};
}

Node *Parser::parse_rescue() {
    const Location location = token_.location();
    Location end_location = token_end_location();
    next_token_skip_space();

    bool has_name = false;
    std::string name;
    bool has_types = false;
    std::vector<Node *> types;

    if (token_.type == Tok::IDENT) {
        name = value_s();
        has_name = true;
        push_var_name(name);
        end_location = token_end_location();
        next_token_skip_space();
        if (token_.type == Tok::OP_COLON) {
            next_token_skip_space_or_newline();
            check({Tok::CONST, Tok::OP_COLON_COLON});
            types = parse_rescue_types();
            has_types = true;
            end_location = EndLoc(types.back());
        }
    } else if (token_.type == Tok::CONST || token_.type == Tok::OP_COLON_COLON) {
        types = parse_rescue_types();
        has_types = true;
        end_location = EndLoc(types.back());
    }

    check({Tok::OP_SEMICOLON, Tok::NEWLINE});
    next_token_skip_space_or_newline();

    Node *body = nullptr;
    if (!tok_kw(Keyword::END)) {
        body = parse_expressions();
        end_location = EndLoc(body);
        skip_statement_end();
    }

    Node *r = make(NodeKind::Rescue);
    r->body = body ? body : nop();
    if (has_types) {
        r->exps = std::move(types);
        r->has_exps = true;
    }
    if (has_name) {
        r->name = name;
        r->has_name = true;
    }
    return at_end(at(r, location), end_location);
}

std::vector<Node *> Parser::parse_rescue_types() {
    std::vector<Node *> types;
    while (true) {
        types.push_back(parse_generic());
        skip_space();
        if (token_.type == Tok::OP_BAR) {
            next_token_skip_space();
        } else {
            skip_space();
            break;
        }
    }
    return types;
}

Node *Parser::parse_while_or_until(NodeKind klass) {
    slash_is_regex_on();
    next_token_skip_space_or_newline();
    Node *cond = parse_op_assign_no_control(true, false);
    slash_is_regex_on();
    skip_statement_end();
    Node *body = parse_expressions();
    skip_statement_end();
    const Location end_location = token_end_location();
    check_ident(Keyword::END);
    next_token_skip_space();
    Node *n = make(klass);
    n->cond = cond;
    n->body = body;
    return at_end(n, end_location);
}

bool Parser::call_block_arg_follows() {
    const char32_t c = current_char();
    const bool ws = c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
    return token_.type == Tok::OP_AMP && !ws;
}

Parser::CallArgs Parser::parse_call_block_arg(std::vector<Node *> args, bool check_paren,
                                              const std::vector<Node *> *named_args) {
    const Location location = token_.location();
    next_token_skip_space();

    Node *block = nullptr;
    Node *block_arg = nullptr;
    Location end_location;

    if (token_.type == Tok::OP_PERIOD) {
        const std::string block_arg_name = temp_arg_name();
        Node *obj = var_node(block_arg_name);
        wants_regex_ = false;
        if (current_char() == '%') {
            next_char();
            token_.type = Tok::OP_PERCENT;
            token_.column_number += 1;
            skip_space();
        } else {
            next_token_skip_space();
        }
        Node *call = parse_call_block_arg_after_dot(obj);
        block = make(NodeKind::Block);
        block->exps = {var_node(block_arg_name)};
        block->body = call;
        at(block, location);
        end_location = EndLoc(call);
    } else {
        block_arg = parse_op_assign();
        end_location = EndLoc(block_arg);
    }

    if (check_paren) {
        skip_space_or_newline();
        check(Tok::OP_RPAREN);
        end_location = token_end_location();
        next_token_skip_space();
    } else {
        skip_space();
    }

    CallArgs ca;
    ca.args = std::move(args);
    ca.has_args = true;
    ca.block = block;
    ca.block_arg = block_arg;
    if (named_args) {
        ca.named_args = *named_args;
        ca.has_named_args = true;
    }
    ca.end_location = end_location;
    ca.has_parentheses = check_paren;
    return ca;
}

Node *Parser::parse_call_block_arg_after_dot(Node *obj) {
    const Location location = token_.location();

    if (token_.type == Tok::INSTANCE_VAR) {
        Node *call = at(parse_read_instance_var(obj), location);
        return parse_atomic_method_suffix_special(call, location);
    }

    check(kAtomicWithMethodCheck);

    Node *call;
    if (value_is(Keyword::IS_A_QUESTION)) {
        call = parse_atomic_method_suffix_special(at(parse_is_a(obj), location), location);
    } else if (value_is(Keyword::AS)) {
        call = parse_atomic_method_suffix_special(at(parse_as(obj), location), location);
    } else if (value_is(Keyword::AS_QUESTION)) {
        call = parse_atomic_method_suffix_special(at(parse_as(obj, NodeKind::NilableCast), location), location);
    } else if (value_is(Keyword::RESPONDS_TO_QUESTION)) {
        call = parse_atomic_method_suffix_special(at(parse_responds_to(obj), location), location);
    } else if (!in_macro_expression_ && value_is(Keyword::NIL_QUESTION)) {
        call = parse_atomic_method_suffix_special(at(parse_nil_q(obj), location), location);
    } else if (token_.type == Tok::OP_BANG) {
        call = parse_atomic_method_suffix_special(at(parse_negation_suffix(obj), location), location);
    } else if (token_.type == Tok::OP_LSQUARE) {
        call = parse_atomic_method_suffix(obj, location);
        if (token_.type == Tok::OP_EQ && call->is(NodeKind::Call) && can_be_assigned(call)) {
            next_token_skip_space();
            Node *exp = parse_op_assign();
            call->name = call->name + "=";
            call->args.push_back(exp);
        }
    } else {
        // At this point we want to attach the "do" to the next call.
        const bool old_stop_on_do = stop_on_do_;
        stop_on_do_ = false;
        call = at(parse_var_or_call(false, true), location);
        if (call->is(NodeKind::Call)) {
            call->obj = obj;
        } else {
            raise("BUG: should be a call");
        }
        if (token_.type == Tok::OP_EQ) {
            if (!can_be_assigned(call)) unexpected_token();
            next_token_skip_space();
            if (token_.type == Tok::OP_LPAREN) {
                next_token_skip_space();
                Node *exp = parse_op_assign();
                check(Tok::OP_RPAREN);
                next_token_skip_space();
                call->name = call->name + "=";
                call->args = {exp};
                call = parse_atomic_method_suffix(call, location);
            } else {
                Node *exp = parse_op_assign();
                call->name = call->name + "=";
                call->args = {exp};
            }
        } else {
            call = parse_atomic_method_suffix(call, location);
            if (token_.type == Tok::OP_EQ && call->is(NodeKind::Call) && can_be_assigned(call)) {
                next_token_skip_space();
                Node *exp = parse_op_assign();
                call->name = call->name + "=";
                call->args.push_back(exp);
            }
        }
        stop_on_do_ = old_stop_on_do;
    }
    return call;
}

}  // namespace crystal

// =====================================================================================
// Part 2: definitions, literals, case/select, macros, calls, types, lib, enum.
// =====================================================================================

namespace crystal {

namespace {
const std::initializer_list<Tok> kStatementEnd = {Tok::OP_SEMICOLON, Tok::NEWLINE, Tok::SPACE};
}  // namespace

Node *Parser::parse_class_def(bool is_abstract, bool is_struct, const Token *doc_token) {
    type_nest_++;
    bool has_doc = doc_token ? doc_token->has_doc : false;
    std::string doc = doc_token ? doc_token->doc : std::string();
    if (!has_doc) {
        has_doc = token_.has_doc;
        doc = token_.doc;
    }

    next_token_skip_space_or_newline();
    const Location name_location = token_.location();

    Node *name = parse_path();
    bool found_space = token_.type == Tok::SPACE;
    skip_space();

    bool has_type_vars = false;
    std::vector<std::string> type_vars;
    int splat_index = -1;
    parse_type_vars(&has_type_vars, &type_vars, &splat_index);

    if (has_type_vars) {
        found_space = token_.type == Tok::SPACE;
        skip_space();
    }

    Node *superclass = nullptr;
    if (token_.type == Tok::OP_LT) {
        next_token_skip_space_or_newline();
        if (tok_kw(Keyword::SELF)) {
            superclass = at(make(NodeKind::Self), token_.location());
            next_token();
        } else {
            superclass = parse_generic();
        }
    }

    if (superclass || !found_space) check(kStatementEnd);
    skip_statement_end();

    Node *body = push_visibility([&] { return parse_expressions(); });

    const Location end_location = token_end_location();
    check_ident(Keyword::END);
    next_token_skip_space();

    type_nest_--;

    Node *class_def = make(NodeKind::ClassDef);
    class_def->name_node = name;
    class_def->body = body ? body : nop();
    class_def->superclass = superclass;
    if (has_type_vars) {
        class_def->names = std::move(type_vars);
        class_def->has_names = true;
    }
    class_def->is_abstract = is_abstract;
    class_def->is_struct = is_struct;
    class_def->splat_index = splat_index;
    class_def->assign_doc(has_doc, doc);
    class_def->name_location = name_location;
    class_def->end_location = end_location;
    set_visibility(class_def);
    return class_def;
}

void Parser::parse_type_vars(bool *has, std::vector<std::string> *type_vars, int *splat_index) {
    *has = false;
    *splat_index = -1;
    if (token_.type == Tok::OP_LPAREN) {
        *has = true;
        next_token_skip_space_or_newline();
        int index = 0;
        while (token_.type != Tok::OP_RPAREN) {
            if (token_.type == Tok::OP_STAR) {
                if (*splat_index >= 0) raise_at_token("splat type parameter already specified", token_);
                *splat_index = index;
                next_token();
            }
            const std::string type_var_name = check_const();
            if (std::find(type_vars->begin(), type_vars->end(), type_var_name) != type_vars->end()) {
                raise_at_token("duplicated type parameter name: " + type_var_name, token_);
            }
            type_vars->push_back(type_var_name);
            next_token_skip_space();
            if (token_.type == Tok::OP_COMMA) {
                next_token_skip_space_or_newline();
            } else {
                skip_space_or_newline();
                check(Tok::OP_RPAREN);
            }
            index++;
        }
        if (type_vars->empty()) raise("must specify at least one type var");
        next_token();
    }
}

Node *Parser::parse_module_def() {
    type_nest_++;
    bool has_doc;
    const std::string doc = doc_or_empty(has_doc);
    next_token_skip_space_or_newline();
    const Location name_location = token_.location();
    Node *name = parse_path();
    const bool found_space = token_.type == Tok::SPACE;
    skip_space();

    bool has_type_vars = false;
    std::vector<std::string> type_vars;
    int splat_index = -1;
    parse_type_vars(&has_type_vars, &type_vars, &splat_index);

    if (has_type_vars || !found_space) check(kStatementEnd);
    skip_statement_end();

    Node *body = push_visibility([&] { return parse_expressions(); });

    const Location end_location = token_end_location();
    check_ident(Keyword::END);
    next_token_skip_space();

    type_nest_--;

    Node *m = make(NodeKind::ModuleDef);
    m->name_node = name;
    m->body = body ? body : nop();
    if (has_type_vars) {
        m->names = std::move(type_vars);
        m->has_names = true;
    }
    m->splat_index = splat_index;
    m->assign_doc(has_doc, doc);
    m->name_location = name_location;
    m->end_location = end_location;
    set_visibility(m);
    return m;
}

Node *Parser::parse_annotation_def() {
    bool has_doc;
    const std::string doc = doc_or_empty(has_doc);
    next_token_skip_space_or_newline();
    const Location name_location = token_.location();
    Node *name = parse_path();
    check(kStatementEnd);
    skip_statement_end();
    const Location end_location = token_end_location();
    check_ident(Keyword::END);
    next_token_skip_space();
    Node *a = make(NodeKind::AnnotationDef);
    a->name_node = name;
    a->assign_doc(has_doc, doc);
    a->name_location = name_location;
    a->end_location = end_location;
    return a;
}

Node *Parser::parse_parenthesized_expression() {
    const Location location = token_.location();
    slash_is_regex_on();
    next_token_skip_space_or_newline();

    if (token_.type == Tok::OP_RPAREN) {
        const Location end_location = token_end_location();
        Node *node = at_end(at(new_expressions({nop()}), location), end_location);
        node->keyword = ExpressionsKeyword::Paren;
        return node_and_next_token(node);
    }

    std::vector<Node *> exps;
    stop_on_do_ = false;
    Location end_location;

    while (true) {
        exps.push_back(parse_expression());
        if (token_.type == Tok::OP_RPAREN) {
            wants_regex_ = false;
            end_location = token_end_location();
            next_token_skip_space();
            break;
        } else if (token_.type == Tok::NEWLINE || token_.type == Tok::OP_SEMICOLON) {
            next_token_skip_statement_end();
            if (token_.type == Tok::OP_RPAREN) {
                wants_regex_ = false;
                end_location = token_end_location();
                next_token_skip_space();
                break;
            }
        } else {
            raise("unterminated parenthesized expression", location);
        }
    }

    if (token_.type == Tok::OP_LPAREN) unexpected_token();

    Node *node = at_end(at(new_expressions(std::move(exps)), location), end_location);
    node->keyword = ExpressionsKeyword::Paren;
    return node;
}

Node *Parser::parse_fun_literal() {
    const Location location = token_.location();
    next_token_skip_space_or_newline();

    if (token_.type == Tok::SYMBOL) {
        raise_at_token("a space is mandatory between ':' and return type", token_);
    } else if (token_.type == Tok::OP_LPAREN || token_.type == Tok::OP_LCURLY || token_.type == Tok::OP_COLON) {
        // do nothing
    } else {
        if (!tok_kw(Keyword::DO)) return parse_fun_pointer();
    }

    std::vector<Node *> params;
    if (token_.type == Tok::OP_LPAREN) {
        next_token_skip_space_or_newline();
        while (token_.type != Tok::OP_RPAREN) {
            Node *param = parse_fun_literal_param();
            for (Node *p : params) {
                if (p->name == param->name) {
                    raise("duplicated proc literal parameter name: " + param->name, Loc(param));
                }
            }
            params.push_back(param);
        }
        next_token_skip_space_or_newline();
    }

    Node *return_type = nullptr;
    if (token_.type == Tok::SYMBOL) {
        raise_at_token("a space is mandatory between ':' and return type", token_);
    } else if (token_.type == Tok::OP_COLON) {
        next_token_skip_space_or_newline();
        return_type = parse_bare_proc_type();
        skip_space_or_newline();
    }

    return with_lexical_var_scope([&]() -> Node * {
        for (Node *p : params) push_var(p);
        Location end_location;
        Node *body = nullptr;
        if (tok_kw(Keyword::DO)) {
            next_token_skip_statement_end();
            check_not_pipe_before_proc_literal_body();
            body = parse_expressions();
            auto r = parse_exception_handler(body, true);
            body = r.first;
            end_location = r.second;
        } else if (token_.type == Tok::OP_LCURLY) {
            next_token_skip_statement_end();
            check_not_pipe_before_proc_literal_body();
            body = preserve_stop_on_do(false, [&] { return parse_expressions(); });
            end_location = token_end_location();
            check(Tok::OP_RCURLY);
            next_token_skip_space();
        } else {
            unexpected_token();
        }
        Node *a_def = make(NodeKind::Def);
        a_def->name = "->";
        a_def->args = params;
        a_def->body = body ? body : nop();
        a_def->return_type = return_type;
        at_end(at(a_def, location), end_location);
        Node *proc = make(NodeKind::ProcLiteral);
        proc->def_ = a_def;
        return at_end(at(proc, location), end_location);
    });
}

void Parser::check_not_pipe_before_proc_literal_body() {
    if (token_.type == Tok::OP_BAR) {
        const Location location = token_.location();
        next_token_skip_space();
        std::string msg = "unexpected token: \"|\", proc literals specify their parameters like this: ->(";
        if (token_.type == Tok::IDENT) {
            msg += value_s() + " : Type";
            next_token_skip_space_or_newline();
            if (token_.type == Tok::OP_COMMA) msg += ", ...";
        } else {
            msg += "param : Type";
        }
        msg += ") { ... }";
        raise(msg, location);
    }
}

Node *Parser::parse_fun_literal_param() {
    const std::string name = check_ident();
    const Location location = token_.location();
    Location end_location = token_end_location();
    next_token_skip_space_or_newline();

    Node *type = nullptr;
    if (token_.type == Tok::OP_COLON) {
        next_token_skip_space_or_newline();
        type = parse_bare_proc_type();
        end_location = EndLoc(type);
    }

    if (token_.type == Tok::OP_COMMA) {
        next_token_skip_space_or_newline();
    } else {
        skip_space_or_newline();
        check(Tok::OP_RPAREN);
    }

    Node *arg = make(NodeKind::Arg);
    arg->name = name;
    arg->external_name = name;
    arg->restriction = type;
    return at_end(at(arg, location), end_location);
}

Node *Parser::parse_fun_pointer() {
    const Location location = token_.location();
    bool global = false;
    if (token_.type == Tok::OP_COLON_COLON) {
        next_token_skip_space_or_newline();
        global = true;
    }

    Node *obj = nullptr;
    std::string name;
    Location end_location;
    bool global_call = false;

    switch (token_.type) {
        case Tok::IDENT: {
            name = value_s();
            const Location var_location = token_.location();
            const Location var_end_location = token_end_location();
            global_call = global;
            auto [equals_sign, el] = consume_def_equals_sign_skip_space();
            end_location = el;
            if (equals_sign) {
                name += "=";
            } else if (token_.type == Tok::OP_PERIOD) {
                if (global) raise("ProcPointer of local variable cannot be global", location);
                if (name != "self" && !var_in_scope(name)) raise("undefined variable '" + name + "'", location);
                obj = at_end(at(var_node(name), var_location), var_end_location);
                name = consume_def_or_macro_name();
                auto [eq2, el2] = consume_def_equals_sign_skip_space();
                end_location = el2;
                if (eq2) name += "=";
            }
            break;
        }
        case Tok::CONST: {
            obj = parse_generic(global, location, false);
            skip_space();
            check(Tok::OP_PERIOD);
            name = consume_def_or_macro_name();
            auto [eq, el] = consume_def_equals_sign_skip_space();
            end_location = el;
            if (eq) name += "=";
            break;
        }
        case Tok::INSTANCE_VAR: {
            if (global) raise("ProcPointer of instance variable cannot be global", location);
            obj = make(NodeKind::InstanceVar);
            obj->name = value_s();
            at_end(at(obj, location), token_end_location());
            next_token_skip_space();
            check(Tok::OP_PERIOD);
            name = consume_def_or_macro_name();
            auto [eq, el] = consume_def_equals_sign_skip_space();
            end_location = el;
            if (eq) name += "=";
            break;
        }
        case Tok::CLASS_VAR: {
            if (global) raise("ProcPointer of class variable cannot be global", location);
            obj = make(NodeKind::ClassVar);
            obj->name = value_s();
            at_end(at(obj, location), token_end_location());
            next_token_skip_space();
            check(Tok::OP_PERIOD);
            name = consume_def_or_macro_name();
            auto [eq, el] = consume_def_equals_sign_skip_space();
            end_location = el;
            if (eq) name += "=";
            break;
        }
        default:
            unexpected_token();
    }

    if (token_.type == Tok::OP_PERIOD) unexpected_token();

    std::vector<Node *> types;
    if (token_.type == Tok::OP_LPAREN) {
        next_token_skip_space();
        types = parse_union_types(Tok::OP_RPAREN);
        check(Tok::OP_RPAREN);
        end_location = token_end_location();
        next_token_skip_space();
    }

    Node *pp = make(NodeKind::ProcPointer);
    pp->obj = obj;
    pp->name = name;
    pp->args = std::move(types);
    pp->global = global_call;
    return at_end(pp, end_location);
}

Node *Parser::parse_delimiter(bool want_skip_space) {
    if (token_.type == Tok::STRING) {
        return node_and_next_token(at(string_literal(value_s()), token_.location()));
    }

    const Location location = token_.location();
    DelimiterState delimiter_state = token_.delimiter_state;

    check(Tok::DELIMITER_START);

    if (delimiter_state.is_heredoc()) {
        if (inside_interpolation_) raise("heredoc cannot be used inside interpolation", location);
        Node *node = at(make(NodeKind::StringInterpolation), location);
        heredocs.push_back(HeredocEntry{delimiter_state, node});
        next_token();
        return node;
    }

    next_string_token(delimiter_state);
    delimiter_state = token_.delimiter_state;

    std::vector<Piece> pieces;
    bool has_interpolation = false;
    unsigned options = 0;
    Location end_location;

    consume_delimiter(pieces, delimiter_state, has_interpolation, options, end_location);

    if (want_skip_space && delimiter_state.kind == DelimiterKind::STRING) {
        while (true) {
            const bool passed_backslash_newline = token_.passed_backslash_newline;
            skip_space();
            if (passed_backslash_newline && token_.type == Tok::DELIMITER_START &&
                token_.delimiter_state.kind == DelimiterKind::STRING) {
                next_string_token(delimiter_state);
                delimiter_state = token_.delimiter_state;
                consume_delimiter(pieces, delimiter_state, has_interpolation, options, end_location);
            } else {
                break;
            }
        }
    }

    Node *result;
    if (has_interpolation) {
        std::vector<Node *> combined = combine_interpolation_pieces(pieces, delimiter_state);
        result = at(make(NodeKind::StringInterpolation), location);
        result->exps = std::move(combined);
    } else {
        result = string_literal(combine_pieces(pieces, delimiter_state));
    }

    if (delimiter_state.kind == DelimiterKind::COMMAND) {
        result = at(call_node(nullptr, "`", {result}), location);
    } else if (delimiter_state.kind == DelimiterKind::REGEX) {
        // The compiler compiles the regex here to report "invalid regex";
        // mep does not check regex syntax.
        Node *r = make(NodeKind::RegexLiteral);
        r->value_node = result;
        r->regex_options = options;
        result = at(r, location);
    }

    result->end_location = end_location;
    return result;
}

std::vector<Node *> Parser::combine_interpolation_pieces(std::vector<Piece> &pieces, const DelimiterState &ds) {
    if (needs_heredoc_indent_removed(ds)) return remove_heredoc_indent(pieces, ds.heredoc_indent);
    std::vector<Node *> out;
    for (Piece &p : pieces) out.push_back(p.is_string ? string_literal(p.str) : p.node);
    return out;
}

std::string Parser::combine_pieces(std::vector<Piece> &pieces, const DelimiterState &ds) {
    std::string out;
    if (needs_heredoc_indent_removed(ds)) {
        for (Node *n : remove_heredoc_indent(pieces, ds.heredoc_indent)) out += n->value;
        return out;
    }
    for (Piece &p : pieces) out += p.str;
    return out;
}

void Parser::consume_delimiter(std::vector<Piece> &pieces, DelimiterState &delimiter_state, bool &has_interpolation,
                               unsigned &options, Location &end_location) {
    options = 0;
    end_location = Location();
    while (true) {
        if (token_.type == Tok::STRING) {
            pieces.push_back(Piece{true, value_s(), nullptr, token_.line_number});
            next_string_token(delimiter_state);
            delimiter_state = token_.delimiter_state;
        } else if (token_.type == Tok::DELIMITER_END) {
            if (delimiter_state.kind == DelimiterKind::REGEX) options = consume_regex_options();
            end_location = token_end_location();
            next_token();
            break;
        } else if (token_.type == Tok::EOF_) {
            switch (delimiter_state.kind) {
                case DelimiterKind::COMMAND: raise("Unterminated command");
                case DelimiterKind::REGEX: raise("Unterminated regular expression");
                case DelimiterKind::HEREDOC: raise("Unterminated heredoc");
                default: raise("Unterminated string literal");
            }
        } else {
            const int line_number = token_.line_number;
            delimiter_state = token_.delimiter_state;
            next_token_skip_space_or_newline();
            const bool old_inside_interpolation = inside_interpolation_;
            inside_interpolation_ = true;
            Node *exp = preserve_stop_on_do(false, [&] { return parse_expression(); });

            if (exp->is(NodeKind::StringLiteral) && !delimiter_state.is_heredoc()) {
                pieces.push_back(Piece{true, exp->value, nullptr, line_number});
            } else {
                pieces.push_back(Piece{false, std::string(), exp, line_number});
                has_interpolation = true;
            }

            skip_space_or_newline();
            if (token_.type != Tok::OP_RCURLY) raise("Unterminated string interpolation");

            token_.delimiter_state = delimiter_state;
            next_string_token(delimiter_state);
            inside_interpolation_ = old_inside_interpolation;
            delimiter_state = token_.delimiter_state;
        }
    }
}

unsigned Parser::consume_regex_options() {
    unsigned options = 0;
    while (true) {
        const char32_t c = current_char();
        if (c == 'i') {
            options |= kRegexIgnoreCase;
            next_char();
        } else if (c == 'm') {
            options |= kRegexMultiline;
            next_char();
        } else if (c == 'x') {
            options |= kRegexExtended;
            next_char();
        } else {
            const char32_t lower = (c >= 'A' && c <= 'Z') ? c + 32 : c;
            if (lower >= 'a' && lower <= 'z') raise("unknown regex option: " + Utf8Encode(c));
            break;
        }
    }
    return options;
}

void Parser::consume_heredocs() {
    consuming_heredocs_ = true;
    std::reverse(heredocs.begin(), heredocs.end());
    while (!heredocs.empty()) {
        HeredocEntry h = heredocs.back();
        heredocs.pop_back();
        consume_heredoc(h.state, static_cast<Node *>(h.item));
    }
    consuming_heredocs_ = false;
}

void Parser::consume_heredoc(const DelimiterState &ds_in, Node *node) {
    next_string_token(ds_in);
    DelimiterState delimiter_state = token_.delimiter_state;
    std::vector<Piece> pieces;
    bool has_interpolation = false;
    unsigned options = 0;
    Location end_location;
    consume_delimiter(pieces, delimiter_state, has_interpolation, options, end_location);

    if (has_interpolation) {
        std::vector<Node *> combined = combine_interpolation_pieces(pieces, delimiter_state);
        node->exps.insert(node->exps.end(), combined.begin(), combined.end());
    } else {
        Node *s = string_literal(combine_pieces(pieces, delimiter_state));
        at(s, node);
        at_end(s, end_location);
        node->exps.push_back(s);
    }
    node->heredoc_indent = delimiter_state.heredoc_indent;
    node->end_location = end_location;
}

namespace {

bool AsciiWhitespace(char32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }

// Decodes the characters of a UTF-8 string (for heredoc indentation, which
// counts characters).
std::vector<std::pair<char32_t, size_t>> Chars(const std::string &s) {
    std::vector<std::pair<char32_t, size_t>> out;  // (char, byte offset)
    size_t i = 0;
    while (i < s.size()) {
        const auto b = static_cast<unsigned char>(s[i]);
        size_t w = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 1;
        if (i + w > s.size()) w = 1;
        char32_t c = b < 0x80 ? b : 0xFFFD;
        out.push_back({c, i});
        i += w;
    }
    return out;
}

// line[indent..-1] in characters.
std::string FromChar(const std::string &s, size_t indent) {
    std::vector<std::pair<char32_t, size_t>> cs = Chars(s);
    if (indent >= cs.size()) return std::string();
    return s.substr(cs[indent].second);
}

bool FirstCharsWhitespace(const std::string &s, size_t n) {
    std::vector<std::pair<char32_t, size_t>> cs = Chars(s);
    for (size_t k = 0; k < n && k < cs.size(); k++) {
        if (!AsciiWhitespace(cs[k].first)) return false;
    }
    return true;
}

}  // namespace

std::vector<Node *> Parser::remove_heredoc_indent(std::vector<Piece> &pieces, int indent_i) {
    const size_t indent = static_cast<size_t>(indent_i);
    std::string current_line;
    bool remove_indent = true;
    struct NP {
        bool is_string;
        std::string str;
        Node *node;
    };
    std::vector<NP> new_pieces;
    auto add_string = [&](const std::string &piece) {
        if (!new_pieces.empty() && new_pieces.back().is_string) {
            new_pieces.back().str += piece;
        } else {
            new_pieces.push_back(NP{true, piece, nullptr});
        }
    };
    int previous_line_number = 0;
    for (size_t i = 0; i < pieces.size(); i++) {
        Piece &piece = pieces[i];
        const int line_number = piece.line_number;
        const bool this_piece_is_in_new_line = line_number != previous_line_number;
        const bool next_piece_is_in_new_line = i == pieces.size() - 1 || pieces[i + 1].line_number != line_number;
        if (piece.is_string) {
            const std::string &value = piece.str;
            if (value == "\n" || value == "\r\n") {
                current_line += value;
                if (this_piece_is_in_new_line || next_piece_is_in_new_line) {
                    std::string line = current_line;
                    if (remove_indent) line = remove_heredoc_from_line(line, indent_i, line_number - 1);
                    add_string(line);
                    current_line.clear();
                    remove_indent = true;
                }
            } else if (!value.empty() && (value[0] == '\n' || value.rfind("\r\n", 0) == 0)) {
                const bool slash_n = value[0] == '\n';
                current_line += slash_n ? "\n" : "\r\n";
                std::string line = current_line;
                if (remove_indent) line = remove_heredoc_from_line(line, indent_i, line_number - 1);
                add_string(line);
                current_line.clear();
                remove_indent = true;
                current_line += value.substr(slash_n ? 1 : 2);
            } else {
                current_line += value;
            }
        } else {
            if (remove_indent) {
                std::string line = current_line;
                if (Chars(line).size() < indent || !FirstCharsWhitespace(line, indent)) {
                    raise("heredoc line must have an indent greater than or equal to " + std::to_string(indent_i),
                          line_number, 1);
                }
                line = FromChar(line, indent);
                if (!line.empty()) add_string(line);
                new_pieces.push_back(NP{false, std::string(), piece.node});
                remove_indent = false;
                current_line.clear();
            } else {
                if (!current_line.empty()) {
                    add_string(current_line);
                    current_line.clear();
                }
                new_pieces.push_back(NP{false, std::string(), piece.node});
            }
        }
        previous_line_number = line_number;
    }
    if (!current_line.empty()) {
        std::string line = current_line;
        if (remove_indent) line = remove_heredoc_from_line(line, indent_i, pieces.back().line_number);
        add_string(line);
    }
    std::vector<Node *> out;
    for (NP &p : new_pieces) out.push_back(p.is_string ? string_literal(p.str) : p.node);
    return out;
}

std::string Parser::remove_heredoc_from_line(const std::string &line, int indent, int line_number) {
    if (FirstCharsWhitespace(line, static_cast<size_t>(indent))) {
        if (static_cast<int>(Chars(line).size()) - 1 < indent) return "\n";
        return FromChar(line, static_cast<size_t>(indent));
    }
    raise("heredoc line must have an indent greater than or equal to " + std::to_string(indent), line_number, 1);
}

std::string Parser::parse_string_without_interpolation(const std::string &context, bool want_skip_space) {
    return parse_string_literal_without_interpolation(context, want_skip_space)->value;
}

Node *Parser::parse_string_literal_without_interpolation(const std::string &context, bool want_skip_space) {
    const Location location = token_.location();
    if (!string_literal_start()) raise("expected string literal for " + context + ", not " + token_.to_s());
    Node *string = parse_delimiter(want_skip_space);
    if (string->is(NodeKind::StringLiteral)) return string;
    raise("interpolation not allowed in " + context, location);
}

Node *Parser::parse_string_or_symbol_array(NodeKind klass, const char *elements_type) {
    std::vector<Node *> strings;
    Location end_location;
    while (true) {
        next_string_array_token();
        if (token_.type == Tok::STRING) {
            Node *s = make(klass);
            s->value = value_s();
            strings.push_back(at_end(at(s, token_.location()), token_end_location()));
        } else if (token_.type == Tok::STRING_ARRAY_END) {
            end_location = token_end_location();
            next_token();
            break;
        } else {
            std::string lower = elements_type;
            for (char &ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            raise("Unterminated " + lower + " array literal");
        }
    }
    Node *a = make(NodeKind::ArrayLiteral);
    a->exps = std::move(strings);
    a->of = path_global(elements_type);
    return at_end(a, end_location);
}

Node *Parser::parse_empty_array_literal() {
    const int line = line_number;
    const int column = token_.column_number;
    next_token_skip_space();
    if (tok_kw(Keyword::OF)) {
        next_token_skip_space_or_newline();
        Node *of = parse_bare_proc_type();
        Node *a = make(NodeKind::ArrayLiteral);
        a->of = of;
        return at_end(a, of);
    }
    raise("for empty arrays use '[] of ElementType'", line, column);
}

Node *Parser::parse_array_literal() {
    const int line = line_number;
    const int column = token_.column_number;
    slash_is_regex_on();
    std::vector<Node *> exps;
    Location end_location;

    open("array literal", token_.location(), [&] {
        next_token_skip_space_or_newline();
        while (token_.type != Tok::OP_RSQUARE) {
            const Location exp_location = token_.location();
            Node *exp;
            if (token_.type == Tok::OP_STAR) {
                next_token_skip_space_or_newline();
                exp = at(unary(NodeKind::Splat, parse_op_assign_no_control()), exp_location);
            } else {
                exp = parse_op_assign_no_control();
            }
            exps.push_back(exp);
            skip_space();
            if (token_.type == Tok::OP_COMMA) {
                slash_is_regex_on();
                next_token_skip_space_or_newline();
            } else {
                skip_space_or_newline();
                check(Tok::OP_RSQUARE);
                break;
            }
        }
        wants_regex_ = false;
        end_location = token_end_location();
        next_token_skip_space();
        return 0;
    });

    Node *of = nullptr;
    if (tok_kw(Keyword::OF)) {
        next_token_skip_space_or_newline();
        of = parse_bare_proc_type();
        end_location = EndLoc(of);
    } else if (exps.empty()) {
        raise("for empty arrays use '[] of ElementType'", line, column);
    }

    Node *a = make(NodeKind::ArrayLiteral);
    a->exps = std::move(exps);
    a->of = of;
    return at_end(a, end_location);
}

Node *Parser::parse_hash_or_tuple_literal(bool allow_of) {
    const Location location = token_.location();
    const int line = line_number;
    const int column = token_.column_number;

    slash_is_regex_on();
    next_token_skip_space_or_newline();

    if (token_.type == Tok::OP_RCURLY) {
        const Location end_location = token_end_location();
        next_token_skip_space();
        return new_hash_literal({}, line, column, end_location);
    }

    if (named_tuple_start()) {
        if (!allow_of) raise_at_token("can't use named tuple syntax for Hash-like literal, use '=>'", token_);
        return parse_named_tuple(location, value_s());
    }

    bool first_is_splat = false;
    if (token_.type == Tok::OP_STAR) {
        first_is_splat = true;
        next_token_skip_space_or_newline();
    }

    const Location key_location = token_.location();
    Node *first_key = parse_op_assign_no_control();
    if (first_is_splat) first_key = at(unary(NodeKind::Splat, first_key), location);

    switch (token_.type) {
        case Tok::OP_COLON:
            if (first_is_splat) unexpected_token();
            if (token_.column_number != EndLoc(first_key).column + 1) {
                raise("space not allowed between named argument name and ':'");
            }
            if (first_key->is(NodeKind::StringLiteral)) {
                if (!allow_of) raise_at_token("can't use named tuple syntax for Hash-like literal, use '=>'", token_);
                if (first_key->value.empty()) raise("named tuple name cannot be empty", key_location);
                return parse_named_tuple(location, first_key->value);
            }
            check(Tok::OP_EQ_GT);
            break;
        case Tok::OP_COMMA:
            slash_is_regex_on();
            next_token_skip_space_or_newline();
            return parse_tuple(first_key, location);
        case Tok::OP_RCURLY:
            return parse_tuple(first_key, location);
        case Tok::NEWLINE:
            next_token_skip_space();
            check(Tok::OP_RCURLY);
            return parse_tuple(first_key, location);
        default:
            if (first_is_splat) unexpected_token();
            check(Tok::OP_EQ_GT);
    }
    slash_is_regex_on();
    next_token_skip_space_or_newline();
    return parse_hash_literal(first_key, location, allow_of);
}

Node *Parser::parse_hash_literal(Node *first_key, Location location, bool allow_of) {
    const int line = line_number;
    const int column = token_.column_number;
    Location end_location;

    std::vector<std::pair<Node *, Node *>> entries;
    entries.push_back({first_key, parse_op_assign()});

    if (token_.type == Tok::NEWLINE) {
        next_token_skip_space_or_newline();
        check(Tok::OP_RCURLY);
        next_token_skip_space();
    } else {
        open("hash literal", location, [&] {
            skip_space_or_newline();
            if (token_.type == Tok::OP_COMMA) {
                slash_is_regex_on();
                next_token_skip_space_or_newline();
            } else {
                skip_space_or_newline();
                check(Tok::OP_RCURLY);
            }
            while (token_.type != Tok::OP_RCURLY) {
                const Location key_loc = token_.location();
                Node *key = parse_op_assign_no_control();
                skip_space_or_newline();
                if (token_.type == Tok::OP_COLON && key->is(NodeKind::StringLiteral)) {
                    raise("can't use 'key: value' syntax in a hash literal", key_loc);
                } else {
                    check(Tok::OP_EQ_GT);
                }
                slash_is_regex_on();
                next_token_skip_space_or_newline();
                entries.push_back({key, parse_op_assign()});
                skip_space();
                if (token_.type == Tok::OP_COMMA) {
                    slash_is_regex_on();
                    next_token_skip_space_or_newline();
                } else {
                    skip_space_or_newline();
                    check(Tok::OP_RCURLY);
                    break;
                }
            }
            end_location = token_end_location();
            next_token_skip_space();
            return 0;
        });
    }
    return new_hash_literal(std::move(entries), line, column, end_location, allow_of);
}

bool Parser::named_tuple_start() {
    return (token_.type == Tok::IDENT || token_.type == Tok::CONST) && current_char() == ':' && peek_next_char() != ':';
}

Node *Parser::parse_tuple(Node *first_exp, Location location) {
    std::vector<Node *> exps;
    Location end_location;
    open("tuple literal", location, [&] {
        exps.push_back(first_exp);
        while (token_.type != Tok::OP_RCURLY) {
            const Location exp_location = token_.location();
            Node *exp;
            if (token_.type == Tok::OP_STAR) {
                next_token_skip_space_or_newline();
                exp = at(unary(NodeKind::Splat, parse_op_assign_no_control()), exp_location);
            } else {
                exp = parse_op_assign_no_control();
            }
            exps.push_back(exp);
            skip_space();
            if (token_.type == Tok::OP_COMMA) {
                next_token_skip_space_or_newline();
            } else {
                skip_space_or_newline();
                check(Tok::OP_RCURLY);
                break;
            }
        }
        end_location = token_end_location();
        next_token_skip_space();
        return 0;
    });
    Node *t = make(NodeKind::TupleLiteral);
    t->exps = std::move(exps);
    return at_end(t, end_location);
}

Node *Parser::new_hash_literal(std::vector<std::pair<Node *, Node *>> entries, int line, int column,
                               Location end_location, bool allow_of) {
    Node *h = make(NodeKind::HashLiteral);
    if (allow_of) {
        if (tok_kw(Keyword::OF)) {
            next_token_skip_space_or_newline();
            Node *of_key = parse_bare_proc_type();
            check(Tok::OP_EQ_GT);
            next_token_skip_space_or_newline();
            Node *of_value = parse_bare_proc_type();
            h->of_key = of_key;
            h->of_value = of_value;
            h->has_of = true;
            end_location = EndLoc(of_value);
        }
        if (entries.empty() && !h->has_of) raise("for empty hashes use '{} of KeyType => ValueType'", line, column);
    }
    h->entries = std::move(entries);
    return at_end(h, end_location);
}

Node *Parser::parse_named_tuple(Location location, const std::string &first_key) {
    next_token_never_a_symbol();
    slash_is_regex_on();
    next_token_skip_space();

    Node *first_value = parse_op_assign();
    skip_space_or_newline();

    std::vector<std::pair<std::string, Node *>> entries;
    entries.push_back({first_key, first_value});

    if (token_.type == Tok::OP_COMMA) {
        next_token_skip_space_or_newline();
        while (token_.type != Tok::OP_RCURLY) {
            const Location key_location = token_.location();
            std::string key = value_s();
            if (named_tuple_start()) {
                next_token_never_a_symbol();
            } else if (string_literal_start()) {
                key = parse_string_without_interpolation("named tuple name", false);
            } else {
                raise_at_token("expected '}' or named tuple name, not " + token_.to_s(), token_);
            }
            if (key.empty()) raise("named tuple name cannot be empty", key_location);
            if (token_.type == Tok::SPACE) raise("space not allowed between named argument name and ':'");
            check(Tok::OP_COLON);
            for (auto &e : entries) {
                if (e.first == key) raise_at_token("duplicated key: " + key, token_);
            }
            slash_is_regex_on();
            next_token_skip_space();
            Node *value = parse_op_assign_no_control();
            skip_space();
            entries.push_back({key, value});
            if (token_.type == Tok::OP_COMMA) {
                next_token_skip_space_or_newline();
            } else {
                break;
            }
        }
    }

    skip_space_or_newline();
    check(Tok::OP_RCURLY);
    const Location end_location = token_end_location();
    next_token_skip_space();

    Node *n = make(NodeKind::NamedTupleLiteral);
    n->named_entries = std::move(entries);
    return at_end(at(n, location), end_location);
}

Node *Parser::parse_require() {
    if (type_nest_ > 0) raise_at_token("can't require inside type declarations", token_);
    next_token_skip_space();
    Node *string_literal_node = parse_string_literal_without_interpolation("require");
    skip_space();
    Node *r = make(NodeKind::Require);
    r->value = string_literal_node->value;
    return at_end(r, string_literal_node);
}

Node *Parser::parse_case() {
    slash_is_regex_on();
    next_token_skip_space_or_newline();
    while (token_.type == Tok::OP_SEMICOLON) next_token_skip_space();

    Node *cond = nullptr;
    if (!(value_is(Keyword::WHEN) || value_is(Keyword::ELSE) || value_is(Keyword::END))) {
        cond = parse_op_assign_no_control();
        skip_statement_end();
    }

    std::vector<Node *> whens;
    Node *a_else = nullptr;
    int exhaustive = -1;  // nil
    std::vector<Node *> when_exps;
    Location end_location;

    while (true) {
        if (value_is(Keyword::WHEN) || value_is(Keyword::IN)) {
            if (exhaustive == -1) {
                exhaustive = value_is(Keyword::IN) ? 1 : 0;
                if (exhaustive == 1 && !cond) {
                    raise("exhaustive case (case ... in) requires a case expression (case exp; in ..)");
                }
            } else if (exhaustive == 1 && value_is(Keyword::WHEN)) {
                raise("expected 'in', not 'when'");
            } else if (exhaustive == 0 && value_is(Keyword::IN)) {
                raise("expected 'when', not 'in'");
            }

            const Location location = token_.location();
            slash_is_regex_on();
            next_token_skip_space_or_newline();
            std::vector<Node *> when_conds;

            if (cond && cond->is(NodeKind::TupleLiteral)) {
                for (Node *e : cond->exps) {
                    if (e->is(NodeKind::Splat)) raise("splat is not allowed inside case expression");
                }
                while (true) {
                    if (token_.type == Tok::OP_LCURLY) {
                        const Location curly_location = token_.location();
                        next_token_skip_space_or_newline();
                        std::vector<Node *> tuple_elements;
                        while (true) {
                            Node *exp = parse_when_expression(cond, false, exhaustive == 1);
                            if (exhaustive == 1) check_valid_exhaustive_expression(exp);
                            tuple_elements.push_back(exp);
                            skip_space();
                            if (token_.type == Tok::OP_COMMA) {
                                next_token_skip_space_or_newline();
                            } else {
                                break;
                            }
                        }
                        if (tuple_elements.size() != cond->exps.size()) {
                            raise("wrong number of tuple elements (given " + std::to_string(tuple_elements.size()) +
                                      ", expected " + std::to_string(cond->exps.size()) + ")",
                                  curly_location);
                        }
                        Node *tuple = make(NodeKind::TupleLiteral);
                        tuple->exps = std::move(tuple_elements);
                        at(tuple, curly_location);
                        when_conds.push_back(tuple);
                        add_when_exp(when_exps, tuple);
                        check(Tok::OP_RCURLY);
                        next_token_skip_space();
                    } else {
                        Node *exp = parse_when_expression(cond, true, exhaustive == 1);
                        when_conds.push_back(exp);
                        add_when_exp(when_exps, exp);
                        skip_space();
                    }
                    if (when_expression_end()) break;
                }
            } else {
                while (true) {
                    Node *exp = parse_when_expression(cond, true, exhaustive == 1);
                    if (exhaustive == 1) check_valid_exhaustive_expression(exp);
                    when_conds.push_back(exp);
                    add_when_exp(when_exps, exp);
                    skip_space();
                    if (when_expression_end()) break;
                }
            }

            Node *when_body = parse_expressions();
            skip_space_or_newline();
            Node *w = make(NodeKind::When);
            w->exps = when_conds;
            w->body = when_body ? when_body : nop();
            at_end(at(w, location), EndLoc(when_conds.back()));
            whens.push_back(w);
        } else if (value_is(Keyword::ELSE)) {
            if (exhaustive == 1) raise("exhaustive case (case ... in) doesn't allow an 'else'");
            next_token_skip_statement_end();
            a_else = parse_expressions();
            skip_statement_end();
            check_ident(Keyword::END);
            end_location = token_end_location();
            next_token();
            break;
        } else if (value_is(Keyword::END)) {
            end_location = token_end_location();
            next_token();
            break;
        } else {
            unexpected_token("expecting when, else or end");
        }
    }

    Node *c = make(NodeKind::Case);
    c->cond = cond;
    c->exps = std::move(whens);
    c->else_ = a_else;
    c->exhaustive = exhaustive == 1;
    for (Node *w : c->exps) w->exhaustive = c->exhaustive;
    return at_end(c, end_location);
}

void Parser::check_valid_exhaustive_expression(Node *exp) {
    switch (exp->kind) {
        case NodeKind::NilLiteral: case NodeKind::BoolLiteral: case NodeKind::Path: case NodeKind::Generic:
        case NodeKind::Underscore:
            return;
        case NodeKind::Call:
            if (exp->obj && exp->obj->is(NodeKind::ImplicitObj) && !exp->name.empty() && exp->name.back() == '?' &&
                exp->args.empty() && !exp->has_named_args && !exp->block) {
                return;
            }
            if (exp->obj && (exp->obj->is(NodeKind::Path) || exp->obj->is(NodeKind::Generic)) && exp->name == "class" &&
                exp->args.empty() && !exp->has_named_args && !exp->block) {
                return;
            }
            break;
        default: break;
    }
    raise("expression of exhaustive case (case ... in) must be a constant (like `IO::Memory`), a generic (like "
          "`Array(Int32)`), a bool literal (true or false), a nil literal (nil) or a question method (like `.red?`)",
          Loc(exp));
}

namespace {
// ASTNode equality (def_equals_and_hash) for the constant expressions
// add_when_exp compares: literals, paths, and arrays/tuples/regexes/ranges
// of them.
bool NodeEquals(const Node *a, const Node *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
        case NodeKind::NilLiteral: case NodeKind::Nop: return true;
        case NodeKind::BoolLiteral: return a->bool_value == b->bool_value;
        case NodeKind::CharLiteral: return a->ch == b->ch;
        case NodeKind::NumberLiteral: return a->value == b->value && a->number_kind == b->number_kind;
        case NodeKind::StringLiteral: case NodeKind::SymbolLiteral: return a->value == b->value;
        case NodeKind::Path: return a->names == b->names && a->global == b->global;
        case NodeKind::ArrayLiteral:
            if (a->exps.size() != b->exps.size() || !NodeEquals(a->of, b->of) || !NodeEquals(a->name_node, b->name_node))
                return false;
            for (size_t i = 0; i < a->exps.size(); i++) {
                if (!NodeEquals(a->exps[i], b->exps[i])) return false;
            }
            return true;
        case NodeKind::TupleLiteral:
            if (a->exps.size() != b->exps.size()) return false;
            for (size_t i = 0; i < a->exps.size(); i++) {
                if (!NodeEquals(a->exps[i], b->exps[i])) return false;
            }
            return true;
        case NodeKind::RegexLiteral:
            return NodeEquals(a->value_node, b->value_node) && a->regex_options == b->regex_options;
        case NodeKind::RangeLiteral:
            return NodeEquals(a->left, b->left) && NodeEquals(a->right, b->right) && a->exclusive == b->exclusive;
        default: return false;
    }
}

// ASTNode#to_s for the constant when-expressions (the duplicate message).
std::string ConstToS(const Node *n) {
    switch (n->kind) {
        case NodeKind::NilLiteral: return "nil";
        case NodeKind::BoolLiteral: return n->bool_value ? "true" : "false";
        case NodeKind::CharLiteral: return CharInspect(n->ch);
        case NodeKind::NumberLiteral: {
            std::string s = n->value;
            if (n->number_kind != NumberKind::I32 && n->number_kind != NumberKind::F64) {
                s += std::string("_") + NumberKindName(n->number_kind);
            }
            return s;
        }
        case NodeKind::StringLiteral: return StringInspect(n->value);
        case NodeKind::SymbolLiteral: {
            const std::string &v = n->value;
            bool plain = Lexer::is_ident(v);
            for (size_t i = 0; plain && i < v.size(); i++) {
                const char c = v[i];
                if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || static_cast<unsigned char>(c) >= 0x80 ||
                      ((c == '?' || c == '!' || c == '=') && i + 1 == v.size()))) {
                    plain = false;
                }
            }
            return plain ? ":" + v : ":" + StringInspect(v);
        }
        case NodeKind::Path: {
            std::string s = n->global ? "::" : "";
            for (size_t i = 0; i < n->names.size(); i++) s += (i ? "::" : "") + n->names[i];
            return s;
        }
        case NodeKind::ArrayLiteral:
        case NodeKind::TupleLiteral: {
            std::string s = n->kind == NodeKind::TupleLiteral ? "{" : "[";
            for (size_t i = 0; i < n->exps.size(); i++) s += (i ? ", " : "") + ConstToS(n->exps[i]);
            s += n->kind == NodeKind::TupleLiteral ? "}" : "]";
            return s;
        }
        case NodeKind::RangeLiteral:
            return ConstToS(n->left) + (n->exclusive ? "..." : "..") + ConstToS(n->right);
        case NodeKind::RegexLiteral: {
            std::string s = "/";
            if (n->value_node && n->value_node->is(NodeKind::StringLiteral)) {
                for (char c : n->value_node->value) {
                    if (c == '/') s += '\\';
                    s += c;
                }
            }
            s += "/";
            if (n->regex_options & kRegexIgnoreCase) s += "i";
            if ((n->regex_options & kRegexMultiline) == kRegexMultiline) s += "m";
            if (n->regex_options & kRegexExtended) s += "x";
            return s;
        }
        case NodeKind::Nop: return "";
        default: return "?";
    }
}
}  // namespace

void Parser::add_when_exp(std::vector<Node *> &when_exps, Node *exp) {
    if (!when_exp_constant(exp)) return;
    for (Node *w : when_exps) {
        if (NodeEquals(w, exp)) raise("duplicate when " + ConstToS(exp) + " in case", Loc(exp));
    }
    when_exps.push_back(exp);
}

bool Parser::when_exp_constant(Node *exp) {
    switch (exp->kind) {
        case NodeKind::NilLiteral: case NodeKind::BoolLiteral: case NodeKind::CharLiteral: case NodeKind::NumberLiteral:
        case NodeKind::StringLiteral: case NodeKind::SymbolLiteral: case NodeKind::Path:
            return true;
        case NodeKind::ArrayLiteral:
        case NodeKind::TupleLiteral:
            for (Node *e : exp->exps) {
                if (!when_exp_constant(e)) return false;
            }
            return true;
        case NodeKind::RegexLiteral: return when_exp_constant(exp->value_node);
        case NodeKind::RangeLiteral: return when_exp_constant(exp->left) && when_exp_constant(exp->right);
        default: return false;
    }
}

bool Parser::when_expression_end() {
    slash_is_regex_on();
    if (tok_kw(Keyword::THEN)) {
        next_token_skip_space_or_newline();
        return true;
    }
    switch (token_.type) {
        case Tok::OP_COMMA: next_token_skip_space_or_newline(); break;
        case Tok::NEWLINE: skip_space_or_newline(); return true;
        case Tok::OP_SEMICOLON: skip_statement_end(); return true;
        default: unexpected_token("expecting ',', ';' or '\\n'");
    }
    return false;
}

Node *Parser::parse_when_expression(Node *cond, bool single, bool exhaustive) {
    if (cond && token_.type == Tok::OP_PERIOD) {
        const Location location = token_.location();
        next_token();
        Node *call = at(parse_var_or_call(false, true), location);
        switch (call->kind) {
            case NodeKind::Call: case NodeKind::RespondsTo: case NodeKind::IsA: case NodeKind::Cast:
            case NodeKind::NilableCast:
                call->obj = make(NodeKind::ImplicitObj);
                break;
            case NodeKind::Not: call->exp = make(NodeKind::ImplicitObj); break;
            default: raise("BUG: expected Call, RespondsTo, IsA, Cast or NilableCast");
        }
        return call;
    }
    if (single && token_.type == Tok::UNDERSCORE) {
        if (exhaustive) raise("'when _' is not supported");
        raise("'when _' is not supported, use 'else' block instead");
    }
    return parse_op_assign_no_control();
}

Node *Parser::parse_select() {
    slash_is_regex_on();
    next_token_skip_space();
    skip_statement_end();

    std::vector<Node *> whens;
    Node *a_else = nullptr;
    Location end_location;

    while (true) {
        if (value_is(Keyword::WHEN)) {
            slash_is_regex_on();
            next_token_skip_space_or_newline();
            const Location location = token_.location();
            Node *condition = parse_op_assign_no_control();
            if (!valid_select_when(condition)) {
                raise("invalid select when expression: must be an assignment or call", location);
            }
            skip_space();
            if (!when_expression_end()) unexpected_token("expecting then, ';' or newline");
            skip_statement_end();
            Node *body = parse_expressions();
            skip_space_or_newline();
            Node *w = make(NodeKind::When);
            w->exps = {condition};
            w->body = body ? body : nop();
            at_end(at(w, location), EndLoc(condition));
            whens.push_back(w);
        } else if (value_is(Keyword::ELSE)) {
            if (whens.empty()) unexpected_token("expecting when");
            slash_is_regex_on();
            next_token_skip_statement_end();
            a_else = parse_expressions();
            skip_statement_end();
            check_ident(Keyword::END);
            end_location = token_end_location();
            next_token();
            break;
        } else if (value_is(Keyword::END)) {
            if (whens.empty()) unexpected_token("expecting when, else or end");
            end_location = token_end_location();
            next_token();
            break;
        } else {
            unexpected_token("expecting when, else or end");
        }
    }

    Node *s = make(NodeKind::Select);
    s->exps = std::move(whens);
    s->else_ = a_else;
    return at_end(s, end_location);
}

bool Parser::valid_select_when(Node *node) {
    if (node->is(NodeKind::Assign)) return node->value_node->is(NodeKind::Call);
    return node->is(NodeKind::Call);
}

Node *Parser::parse_include_or_extend(NodeKind klass) {
    next_token_skip_space_or_newline();
    Node *name;
    if (tok_kw(Keyword::SELF)) {
        name = at(make(NodeKind::Self), token_.location());
        name->end_location = token_end_location();
        next_token_skip_space();
    } else {
        name = parse_generic();
        skip_space();
    }
    Node *n = make(klass);
    n->name_node = name;
    return n;
}

Node *Parser::parse_def(bool is_abstract, const Token *doc_token) {
    bool has_doc = doc_token ? doc_token->has_doc : false;
    std::string doc = doc_token ? doc_token->doc : std::string();
    if (!has_doc) {
        has_doc = token_.has_doc;
        doc = token_.doc;
    }

    prepare_parse_def();
    Node *a_def = with_isolated_var_scope(true, [&] { return parse_def_helper(is_abstract); });

    a_def->calls_super = calls_super_;
    a_def->calls_initialize = calls_initialize_;
    a_def->calls_previous_def = calls_previous_def_;
    a_def->uses_block_arg = uses_block_arg_;
    a_def->assigns_special_var = assigns_special_var_;
    a_def->assign_doc(has_doc, doc);
    calls_super_ = false;
    calls_initialize_ = false;
    calls_previous_def_ = false;
    uses_block_arg_ = false;
    assigns_special_var_ = false;
    has_block_arg_name_ = false;
    is_macro_def_ = false;
    return a_def;
}

void Parser::prepare_parse_def() {
    calls_super_ = false;
    calls_initialize_ = false;
    calls_previous_def_ = false;
    uses_block_arg_ = false;
    has_block_arg_name_ = false;
    assigns_special_var_ = false;
    is_macro_def_ = false;
}

Node *Parser::parse_macro() {
    bool has_doc;
    const std::string doc = doc_or_empty(has_doc);

    std::string name = consume_def_or_macro_name();

    return with_isolated_var_scope(true, [&]() -> Node * {
        const Location name_location = token_.location();

        if (token_.type == Tok::CONST) {
            raise("macro can't have a receiver");
        } else if (token_.type == Tok::IDENT) {
            check_valid_def_name();
            auto [equals_sign, el] = consume_def_equals_sign_skip_space();
            (void)el;
            if (equals_sign) name += "=";
        } else {
            check_valid_def_op_name();
            next_token_skip_space();
        }

        std::vector<Node *> params;
        bool found_default_value = false;
        bool found_splat = false;
        Node *found_double_splat = nullptr;
        int splat_index = -1;
        Node *double_splat = nullptr;
        Node *block_param = nullptr;
        int index = 0;

        switch (token_.type) {
            case Tok::OP_LPAREN:
                next_token_skip_space_or_newline();
                while (token_.type != Tok::OP_RPAREN) {
                    ArgExtras extras = parse_param(params, nullptr, true, found_default_value, found_splat,
                                                   found_double_splat, false);
                    if (!found_default_value && extras.default_value) found_default_value = true;
                    if (splat_index < 0 && extras.splat) {
                        splat_index = index;
                        found_splat = true;
                    }
                    if (extras.double_splat) {
                        double_splat = params.back();
                        params.pop_back();
                        found_double_splat = double_splat;
                    }
                    if (extras.block_arg) {
                        block_param = extras.block_arg;
                        check(Tok::OP_RPAREN);
                        break;
                    } else if (token_.type == Tok::OP_COMMA) {
                        next_token_skip_space_or_newline();
                    } else {
                        skip_space_or_newline();
                        check(Tok::OP_RPAREN);
                    }
                    index++;
                }
                if (splat_index >= 0 && splat_index == static_cast<int>(params.size()) - 1 &&
                    params.back()->name.empty()) {
                    raise("named parameters must follow bare *", Loc(params.back()));
                }
                next_token();
                break;
            case Tok::IDENT:
            case Tok::OP_STAR:
                if (tok_kw(Keyword::END)) unexpected_token("expected ';' or newline");
                unexpected_token("parentheses are mandatory for macro parameters");
            case Tok::OP_SEMICOLON:
            case Tok::NEWLINE:
                break;
            case Tok::OP_PERIOD:
                raise("macro can't have a receiver");
            default:
                unexpected_token();
        }

        Location end_location;
        Node *body;
        if (tok_kw(Keyword::END)) {
            end_location = token_end_location();
            body = new_expressions({});
            next_token_skip_space();
        } else {
            auto r = parse_macro_body(name_location);
            body = r.first;
            end_location = r.second;
        }

        Node *node = make(NodeKind::Macro);
        node->name = name;
        node->args = std::move(params);
        node->body = body;
        node->block_arg = block_param;
        node->splat_index = splat_index;
        node->double_splat = double_splat;
        node->name_location = name_location;
        node->assign_doc(has_doc, doc);
        node->end_location = end_location;
        set_visibility(node);
        return node;
    });
}

std::pair<Node *, Location> Parser::parse_macro_body(Location start_location, MacroState macro_state) {
    bool skip_whitespace = check_macro_skip_whitespace();
    slash_is_regex_on();

    std::vector<Node *> pieces;
    auto new_macro_expressions = [&](std::vector<Node *> &p) -> Node * {
        if (p.empty()) return new_expressions({});
        return expressions_from(p);
    };

    while (true) {
        next_macro_token(macro_state, skip_whitespace);
        macro_state = token_.macro_state;
        if (macro_state.yields) {
            if (block_arity_ < 0) block_arity_ = 0;
        }
        skip_whitespace = false;

        switch (token_.type) {
            case Tok::MACRO_LITERAL: {
                Node *l = make(NodeKind::MacroLiteral);
                l->value = value_s();
                pieces.push_back(at_end(at(l, token_.location()), token_end_location()));
                break;
            }
            case Tok::MACRO_EXPRESSION_START: {
                const Location location = token_.location();
                Node *e = make(NodeKind::MacroExpression);
                e->exp = parse_macro_expression();
                at(e, location);
                check_macro_expression_end();
                skip_whitespace = check_macro_skip_whitespace();
                pieces.push_back(at_end(e, token_end_location()));
                break;
            }
            case Tok::MACRO_CONTROL_START: {
                Node *macro_control = parse_macro_control(start_location, macro_state);
                if (macro_control) {
                    skip_space_or_newline();
                    check(Tok::OP_PERCENT_RCURLY);
                    pieces.push_back(macro_control);
                    skip_whitespace = check_macro_skip_whitespace();
                } else {
                    return {new_macro_expressions(pieces), Location()};
                }
                break;
            }
            case Tok::MACRO_VAR: {
                const std::string macro_var_name = value_s();
                const Location location = token_.location();
                const char32_t first = static_cast<unsigned char>(macro_var_name[0]);
                if (first >= 'A' && first <= 'Z') {
                    warn(token_.location(), "macro fresh variables with constant names are deprecated");
                }
                Node *mv = make(NodeKind::MacroVar);
                mv->name = macro_var_name;
                if (current_char() == '{') {
                    if (Utf8Length(macro_var_name) == 1) {
                        warn(token_.location(), "single-letter macro fresh variables with indices are deprecated");
                    }
                    mv->exps = parse_macro_var_exps();
                    mv->has_exps = true;
                }
                pieces.push_back(at_end(at(mv, location), token_end_location()));
                break;
            }
            case Tok::MACRO_END:
                goto done;
            case Tok::EOF_:
                raise("unterminated macro", start_location);
            default:
                unexpected_token();
        }
    }
done:
    const Location end_location = token_end_location();
    next_token();
    return {new_macro_expressions(pieces), end_location};
}

std::vector<Node *> Parser::parse_macro_var_exps() {
    next_token();  // '{'
    next_token();
    std::vector<Node *> exps;
    while (true) {
        exps.push_back(parse_expression_inside_macro());
        skip_space();
        if (token_.type == Tok::OP_COMMA) {
            next_token_skip_space();
            if (token_.type == Tok::OP_RCURLY) break;
        } else if (token_.type == Tok::OP_RCURLY) {
            break;
        } else {
            unexpected_token("expecting \",\" or \"}\"");
        }
    }
    return exps;
}

bool Parser::check_macro_skip_whitespace() {
    const char32_t p = peek_next_char();
    if (current_char() == '\\' && (p == ' ' || p == '\t' || p == '\n' || p == '\v' || p == '\f' || p == '\r')) {
        next_char();
        return true;
    }
    return false;
}

Node *Parser::parse_percent_macro_expression() {
    if (in_macro_expression_) raise_at_token("can't nest macro expressions", token_);
    slash_is_regex_on();
    const Location location = token_.location();
    Node *macro_exp = parse_macro_expression();
    check_macro_expression_end();
    const Location end_location = token_end_location();
    next_token();
    Node *e = make(NodeKind::MacroExpression);
    e->exp = macro_exp;
    return at_end(at(e, location), end_location);
}

Node *Parser::parse_macro_expression() {
    next_token_skip_space_or_newline();
    return parse_expression_inside_macro();
}

void Parser::check_macro_expression_end() {
    if (token_.type == Tok::OP_COMMA) {
        raise("expecting token ',', not '}'\n\nIf you are nesting tuples or hashes you must write them like this:\n\n"
              "    { {x, y}, {z, w} } # Note the space after the first curly brace\n\nbecause {{...}} is parsed as a "
              "macro expression.");
    }
    check(Tok::OP_RCURLY);
    next_token();
    check(Tok::OP_RCURLY);
}

Node *Parser::parse_percent_macro_control() {
    if (in_macro_expression_) raise_at_token("can't nest macro expressions", token_);
    Node *macro_control = parse_macro_control(token_.location());
    if (macro_control) {
        skip_space_or_newline();
        check(Tok::OP_PERCENT_RCURLY);
        next_token_skip_space();
        return macro_control;
    }
    unexpected_token_in_atomic();
}

Node *Parser::parse_macro_control(Location start_location, MacroState macro_state) {
    const Location location = token_.location();
    next_token_skip_space_or_newline();

    if (value_is(Keyword::FOR)) {
        next_token_skip_space();
        std::vector<Node *> vars;
        while (true) {
            std::string var;
            if (token_.type == Tok::UNDERSCORE) {
                var = "_";
            } else if (token_.type == Tok::IDENT) {
                var = value_s();
            } else {
                unexpected_token("expecting ident or underscore");
            }
            vars.push_back(at_end(at(var_node(var), token_.location()), token_end_location()));
            next_token_skip_space();
            if (token_.type == Tok::OP_COMMA) {
                next_token_skip_space();
            } else {
                break;
            }
        }
        check_ident(Keyword::IN);
        next_token_skip_space();
        Node *exp = parse_expression_inside_macro();
        check(Tok::OP_PERCENT_RCURLY);
        macro_state.control_nest++;
        auto [body, end_location] = parse_macro_body(start_location, macro_state);
        (void)end_location;
        macro_state.control_nest--;
        check_ident(Keyword::END);
        next_token_skip_space();
        check(Tok::OP_PERCENT_RCURLY);
        Node *f = make(NodeKind::MacroFor);
        f->exps = std::move(vars);
        f->exp = exp;
        f->body = body;
        return at_end(f, token_end_location());
    } else if (value_is(Keyword::IF)) {
        return at(parse_macro_if(start_location, macro_state), location);
    } else if (value_is(Keyword::UNLESS)) {
        return at(parse_macro_if(start_location, macro_state, true, true), location);
    } else if (value_is(Keyword::BEGIN)) {
        next_token_skip_space();
        check(Tok::OP_PERCENT_RCURLY);
        macro_state.control_nest++;
        auto [body, end_location] = parse_macro_body(start_location, macro_state);
        (void)end_location;
        macro_state.control_nest--;
        check_ident(Keyword::END);
        next_token_skip_space();
        check(Tok::OP_PERCENT_RCURLY);
        Node *mi = make(NodeKind::MacroIf);
        Node *t = make(NodeKind::BoolLiteral);
        t->bool_value = true;
        mi->cond = t;
        mi->then_ = body ? body : nop();
        mi->else_ = nop();
        return at_end(at(mi, location), token_end_location());
    } else if (value_is(Keyword::ELSE) || value_is(Keyword::ELSIF) || value_is(Keyword::END)) {
        return nullptr;
    } else if (value_is(Keyword::VERBATIM)) {
        next_token_skip_space();
        if (!tok_kw(Keyword::DO)) unexpected_token("expecting 'do'");
        next_token_skip_space();
        check(Tok::OP_PERCENT_RCURLY);
        macro_state.control_nest++;
        auto [body, end_location] = parse_macro_body(start_location, macro_state);
        (void)end_location;
        macro_state.control_nest--;
        check_ident(Keyword::END);
        next_token_skip_space();
        check(Tok::OP_PERCENT_RCURLY);
        return at_end(unary(NodeKind::MacroVerbatim, body), token_end_location());
    }

    in_macro_expression_ = true;
    Node *exps = parse_expressions();
    in_macro_expression_ = false;
    Node *e = make(NodeKind::MacroExpression);
    e->exp = exps;
    e->output = false;
    return at_end(at(e, location), token_end_location());
}

Node *Parser::parse_macro_if(Location start_location, MacroState macro_state, bool check_end, bool is_unless) {
    const Location location = token_.location();
    next_token_skip_space();

    in_macro_expression_ = true;
    Node *cond = parse_op_assign();
    in_macro_expression_ = false;

    if (token_.type != Tok::OP_PERCENT_RCURLY && check_end) {
        in_macro_expression_ = true;
        Node *node = is_unless ? parse_unless_after_condition(cond, location)
                               : parse_if_after_condition(cond, location, true);
        skip_statement_end();
        Node *exps = parse_expressions();
        in_macro_expression_ = false;
        check(Tok::OP_PERCENT_RCURLY);
        exps = concat_expressions(node, exps);
        Node *e = make(NodeKind::MacroExpression);
        e->exp = exps;
        e->output = false;
        return at_end(e, token_end_location());
    }

    check(Tok::OP_PERCENT_RCURLY);

    macro_state.control_nest++;
    auto r = parse_macro_body(start_location, macro_state);
    Node *a_then = r.first;
    macro_state.control_nest--;

    Node *a_else = nullptr;
    if (value_is(Keyword::ELSE)) {
        next_token_skip_space();
        check(Tok::OP_PERCENT_RCURLY);
        macro_state.control_nest++;
        auto r2 = parse_macro_body(start_location, macro_state);
        a_else = r2.first;
        macro_state.control_nest--;
        if (check_end) {
            check_ident(Keyword::END);
            next_token_skip_space();
            check(Tok::OP_PERCENT_RCURLY);
        }
    } else if (value_is(Keyword::ELSIF)) {
        if (is_unless) unexpected_token();
        const Location start_loc = token_.location();
        a_else = at(parse_macro_if(start_location, macro_state, false), start_loc);
        if (check_end) {
            check_ident(Keyword::END);
            next_token_skip_space();
            check(Tok::OP_PERCENT_RCURLY);
        }
    } else if (value_is(Keyword::END)) {
        if (check_end) {
            next_token_skip_space();
            check(Tok::OP_PERCENT_RCURLY);
        }
    } else {
        unexpected_token();
    }

    if (is_unless) std::swap(a_then, a_else);
    Node *mi = make(NodeKind::MacroIf);
    mi->cond = cond;
    mi->then_ = a_then ? a_then : nop();
    mi->else_ = a_else ? a_else : nop();
    mi->is_unless = is_unless;
    return at_end(mi, token_end_location());
}

Node *Parser::parse_expression_inside_macro() {
    in_macro_expression_ = true;
    Node *exp;
    if (token_.type == Tok::OP_STAR) {
        next_token_skip_space();
        exp = parse_expression();
        exp = at(unary(NodeKind::Splat, exp), exp);
    } else if (token_.type == Tok::OP_STAR_STAR) {
        next_token_skip_space();
        exp = parse_expression();
        exp = at(unary(NodeKind::DoubleSplat, exp), exp);
    } else {
        exp = parse_expression();
    }
    skip_space_or_newline();
    in_macro_expression_ = false;
    return exp;
}

namespace {
const std::initializer_list<Tok> kDefOrMacroCheck2 = {
    Tok::OP_LT_LT, Tok::OP_LT, Tok::OP_LT_EQ, Tok::OP_EQ_EQ, Tok::OP_EQ_EQ_EQ, Tok::OP_BANG_EQ, Tok::OP_EQ_TILDE,
    Tok::OP_BANG_TILDE, Tok::OP_GT_GT, Tok::OP_GT, Tok::OP_GT_EQ, Tok::OP_PLUS, Tok::OP_MINUS, Tok::OP_STAR,
    Tok::OP_SLASH, Tok::OP_SLASH_SLASH, Tok::OP_BANG, Tok::OP_TILDE, Tok::OP_PERCENT, Tok::OP_AMP, Tok::OP_BAR,
    Tok::OP_CARET, Tok::OP_STAR_STAR, Tok::OP_LSQUARE_RSQUARE, Tok::OP_LSQUARE_RSQUARE_EQ,
    Tok::OP_LSQUARE_RSQUARE_QUESTION, Tok::OP_LT_EQ_GT, Tok::OP_AMP_PLUS, Tok::OP_AMP_MINUS, Tok::OP_AMP_STAR,
    Tok::OP_AMP_STAR_STAR,
};
const std::initializer_list<Tok> kDefOrMacroCheck1 = {
    Tok::IDENT, Tok::CONST, Tok::OP_GRAVE, Tok::OP_LT_LT, Tok::OP_LT, Tok::OP_LT_EQ, Tok::OP_EQ_EQ,
    Tok::OP_EQ_EQ_EQ, Tok::OP_BANG_EQ, Tok::OP_EQ_TILDE, Tok::OP_BANG_TILDE, Tok::OP_GT_GT, Tok::OP_GT,
    Tok::OP_GT_EQ, Tok::OP_PLUS, Tok::OP_MINUS, Tok::OP_STAR, Tok::OP_SLASH, Tok::OP_SLASH_SLASH, Tok::OP_BANG,
    Tok::OP_TILDE, Tok::OP_PERCENT, Tok::OP_AMP, Tok::OP_BAR, Tok::OP_CARET, Tok::OP_STAR_STAR,
    Tok::OP_LSQUARE_RSQUARE, Tok::OP_LSQUARE_RSQUARE_EQ, Tok::OP_LSQUARE_RSQUARE_QUESTION, Tok::OP_LT_EQ_GT,
    Tok::OP_AMP_PLUS, Tok::OP_AMP_MINUS, Tok::OP_AMP_STAR, Tok::OP_AMP_STAR_STAR,
};
}  // namespace

Node *Parser::parse_def_helper(bool is_abstract) {
    doc_enabled = false;
    def_nest_++;
    stop_on_do_ = false;

    next_token();
    consume_def_or_macro_name();

    Node *receiver = nullptr;
    block_arity_ = -1;
    Location name_location = token_.location();
    const Location receiver_location = token_.location();
    Location end_location = token_end_location();
    std::string name;
    bool has_name = false;
    bool last_was_space = false;

    if (token_.type == Tok::CONST) {
        receiver = parse_path();
        skip_space();
        last_was_space = false;
    } else if (token_.type == Tok::IDENT) {
        check_valid_def_name();
        name = value_s();
        has_name = true;
        auto [equals_sign, el] = consume_def_equals_sign();
        (void)el;
        if (equals_sign) name += "=";
        last_was_space = token_.type == Tok::SPACE;
        skip_space();
    } else {
        check_valid_def_op_name();
        name = TokName(token_.type);
        has_name = true;
        next_token();
        last_was_space = token_.type == Tok::SPACE;
        skip_space();
    }

    std::vector<Node *> params;
    std::vector<Node *> extra_assigns;

    if (token_.type == Tok::OP_PERIOD) {
        if (!receiver) {
            if (has_name) {
                receiver = at_end(at(var_node(name), receiver_location), end_location);
            } else {
                raise("shouldn't reach this line");
            }
        }
        consume_def_or_macro_name();
        if (token_.type == Tok::IDENT) {
            check_valid_def_name();
            name = value_s();
            name_location = token_.location();
            auto [equals_sign, el] = consume_def_equals_sign();
            (void)el;
            if (equals_sign) name += "=";
            last_was_space = token_.type == Tok::SPACE;
            skip_space();
        } else {
            check(kDefOrMacroCheck2);
            check_valid_def_op_name();
            name = TokName(token_.type);
            name_location = token_.location();
            next_token();
            last_was_space = token_.type == Tok::SPACE;
            skip_space();
        }
    } else {
        if (receiver) {
            unexpected_token();
        } else if (!has_name) {
            raise("shouldn't reach this line");
        }
    }

    bool found_default_value = false;
    bool found_splat = false;
    Node *found_double_splat = nullptr;
    bool found_block = false;
    int index = 0;
    int splat_index = -1;
    Node *double_splat = nullptr;
    Node *block_param = nullptr;

    switch (token_.type) {
        case Tok::OP_LPAREN: {
            next_token_skip_space_or_newline();
            while (token_.type != Tok::OP_RPAREN) {
                ArgExtras extras = parse_param(params, &extra_assigns, true, found_default_value, found_splat,
                                               found_double_splat, true);
                if (!found_default_value && extras.default_value) found_default_value = true;
                if (splat_index < 0 && extras.splat) {
                    splat_index = index;
                    found_splat = true;
                }
                if (extras.double_splat) {
                    double_splat = params.back();
                    params.pop_back();
                    found_double_splat = double_splat;
                }
                if (extras.block_arg) {
                    block_param = extras.block_arg;
                    compute_block_arg_yields(block_param);
                    check(Tok::OP_RPAREN);
                    found_block = true;
                    break;
                } else if (token_.type == Tok::OP_COMMA) {
                    next_token_skip_space_or_newline();
                } else {
                    skip_space_or_newline();
                    check(Tok::OP_RPAREN);
                }
                index++;
            }

            end_location = token_end_location();

            if (Lexer::is_setter(name)) {
                if (params.size() > 1 || found_splat || found_double_splat) {
                    raise("setter method '" + name + "' cannot have more than one parameter");
                } else if (found_block) {
                    raise("setter method '" + name + "' cannot have a block");
                }
            }

            if (splat_index >= 0 && splat_index == static_cast<int>(params.size()) - 1 && params.back()->name.empty()) {
                raise("named parameters must follow bare *", Loc(params.back()));
            }

            next_token();
            last_was_space = token_.type == Tok::SPACE;
            skip_space();
            if (token_.type == Tok::SYMBOL) raise_at_token("a space is mandatory between ':' and return type", token_);
            break;
        }
        case Tok::IDENT: case Tok::INSTANCE_VAR: case Tok::CLASS_VAR: case Tok::OP_STAR: case Tok::OP_STAR_STAR:
            if (tok_kw(Keyword::END)) unexpected_token("expected \";\" or newline");
            unexpected_token("parentheses are mandatory for def parameters");
        case Tok::OP_SEMICOLON: case Tok::NEWLINE: case Tok::OP_COLON:
            break;
        case Tok::OP_AMP:
            unexpected_token("parentheses are mandatory for def parameters");
        case Tok::SYMBOL:
            raise_at_token("a space is mandatory between ':' and return type", token_);
        default:
            if (!(is_abstract && token_.type == Tok::EOF_)) unexpected_token();
    }

    Node *return_type = nullptr;
    if (token_.type == Tok::OP_COLON) {
        if (!last_was_space) {
            warn(token_.location(),
                 "space required before colon in return type restriction (run `crystal tool format` to fix this)");
        }
        next_token_skip_space();
        return_type = parse_bare_proc_type();
        end_location = EndLoc(return_type);
    }

    skip_space();
    bool has_free_vars = false;
    std::vector<std::string> free_vars;
    if (token_.type == Tok::IDENT && value_is_str("forall")) {
        next_token_skip_space();
        free_vars = parse_def_free_vars();
        has_free_vars = true;
    }

    Node *body;
    if (is_abstract) {
        body = nop();
    } else {
        slash_is_regex_on();
        skip_statement_end();
        end_location = token_end_location();
        if (tok_kw(Keyword::END)) {
            body = at(expressions_from(extra_assigns), token_.location());
            next_token_skip_space();
        } else {
            body = parse_expressions();
            if (!extra_assigns.empty()) {
                std::vector<Node *> exps = extra_assigns;
                if (body->is(NodeKind::Expressions)) {
                    exps.insert(exps.end(), body->exps.begin(), body->exps.end());
                } else {
                    exps.push_back(body);
                }
                body = at(expressions_from(exps), body);
            }
            auto r = parse_exception_handler(body, true);
            body = r.first;
            end_location = r.second;
        }
    }

    def_nest_--;
    doc_enabled = wants_doc_;

    Node *node = make(NodeKind::Def);
    node->name = name;
    node->args = std::move(params);
    node->body = body ? body : nop();
    node->receiver = receiver;
    node->block_arg = block_param;
    node->return_type = return_type;
    node->macro_def = is_macro_def_;
    node->block_arity = block_arity_;
    node->is_abstract = is_abstract;
    node->splat_index = splat_index;
    node->double_splat = double_splat;
    if (has_free_vars) {
        node->names = std::move(free_vars);
        node->has_names = true;
    }
    node->name_location = name_location;
    set_visibility(node);
    node->end_location = end_location;
    return node;
}

void Parser::check_valid_def_name() {
    if (value_is(Keyword::IS_A_QUESTION) || value_is(Keyword::AS) || value_is(Keyword::AS_QUESTION) ||
        value_is(Keyword::RESPONDS_TO_QUESTION) || value_is(Keyword::NIL_QUESTION)) {
        raise_at_token("'" + value_s() + "' is a pseudo-method and can't be redefined", token_);
    }
}

void Parser::check_valid_def_op_name() {
    if (token_.type == Tok::OP_BANG) raise_at_token("'!' is a pseudo-method and can't be redefined", token_);
}

std::vector<std::string> Parser::parse_def_free_vars() {
    std::vector<std::string> free_vars;
    while (true) {
        check(Tok::CONST);
        const std::string free_var = value_s();
        if (std::find(free_vars.begin(), free_vars.end(), free_var) != free_vars.end()) {
            raise_at_token("duplicated free variable name: " + free_var, token_);
        }
        free_vars.push_back(free_var);
        next_token_skip_space();
        if (token_.type == Tok::OP_COMMA) {
            next_token_skip_space();
            check(Tok::CONST);
        } else {
            break;
        }
    }
    return free_vars;
}

void Parser::compute_block_arg_yields(Node *block_arg) {
    Node *r = block_arg->restriction;
    if (r && r->is(NodeKind::ProcNotation)) {
        block_arity_ = r->has_exps ? static_cast<int>(r->exps.size()) : 0;
    } else {
        block_arity_ = 0;
    }
}

Parser::ArgExtras Parser::parse_param(std::vector<Node *> &params, std::vector<Node *> *extra_assigns, bool,
                                      bool found_default_value, bool found_splat, Node *found_double_splat,
                                      bool allow_restrictions) {
    bool has_annotations = false;
    std::vector<Node *> annotations;

    while (token_.type == Tok::OP_AT_LSQUARE) {
        has_annotations = true;
        annotations.push_back(parse_annotation());
        skip_space_or_newline();
    }

    if (token_.type == Tok::OP_AMP) {
        next_token();
        const bool space_after_amp = token_.type == Tok::SPACE;
        skip_space_or_newline();
        if (token_.type == Tok::OP_COLON && !space_after_amp) {
            warn(token_.location(), "space required before colon in type restriction (run `crystal tool format` to fix this)");
        }
        Node *block_param = parse_def_block_param(extra_assigns, has_annotations, annotations);
        skip_space_or_newline();
        if (!block_param->name.empty()) {
            bool conflict = false;
            for (Node *p : params) {
                if (p->name == block_param->name) conflict = true;
            }
            if (found_double_splat && found_double_splat->name == block_param->name) conflict = true;
            if (conflict) raise("duplicated def parameter name: " + block_param->name, Loc(block_param));
        }
        ArgExtras e;
        e.block_arg = block_param;
        return e;
    }

    if (found_double_splat) raise("only block parameter is allowed after double splat");

    bool splat = false;
    bool double_splat = false;
    Location param_location = token_.location();
    bool allow_external_name = true;

    if (token_.type == Tok::OP_STAR) {
        if (found_splat) unexpected_token();
        splat = true;
        allow_external_name = false;
        next_token_skip_space();
    } else if (token_.type == Tok::OP_STAR_STAR) {
        double_splat = true;
        allow_external_name = false;
        next_token_skip_space();
    }

    bool found_space = false;
    std::string param_name;
    bool has_external_name = false;
    std::string external_name;

    if (splat && (token_.type == Tok::OP_COMMA || token_.type == Tok::OP_RPAREN)) {
        param_name = "";
        allow_restrictions = false;
    } else {
        param_location = token_.location();
        ParamName pn = parse_param_name(param_location, extra_assigns, allow_external_name);
        param_name = pn.param_name;
        has_external_name = pn.has_external_name;
        external_name = pn.external_name;
        found_space = pn.found_space;

        for (Node *param : params) {
            if (param->name == param_name) raise("duplicated def parameter name: " + param_name, param_location);
            // `param.external_name == external_name`: external_name may be nil
            // (no external name given), which never equals a String.
            if (has_external_name && param->external_name == external_name) {
                raise("duplicated def parameter external name: " + external_name, param_location);
            }
        }

        if (token_.type == Tok::SYMBOL) raise_at_token("space required after colon in type restriction", token_);
    }

    Node *default_value = nullptr;
    Node *restriction = nullptr;
    bool found_colon = false;

    if (allow_restrictions && token_.type == Tok::OP_COLON) {
        if (!default_value && !found_space) raise_at_token("space required before colon in type restriction", token_);
        next_token_skip_space_or_newline();
        const Location location = token_.location();
        bool splat_restriction = false;
        if ((splat && token_.type == Tok::OP_STAR) || (double_splat && token_.type == Tok::OP_STAR_STAR)) {
            splat_restriction = true;
            next_token();
        }
        restriction = parse_bare_proc_type();
        if (splat_restriction) {
            restriction = unary(splat ? NodeKind::Splat : NodeKind::DoubleSplat, restriction);
            at(restriction, location);
        }
        found_colon = true;
    }

    if (token_.type == Tok::OP_EQ) {
        if (splat) raise_at_token("splat parameter can't have default value", token_);
        if (double_splat) raise_at_token("double splat parameter can't have default value", token_);
        slash_is_regex_on();
        next_token_skip_space_or_newline();
        if (IsMagic(token_.type)) {
            Node *m = make(NodeKind::MagicConstant);
            m->magic = token_.type;
            default_value = at(m, token_.location());
            next_token();
        } else {
            no_type_declaration_++;
            default_value = parse_op_assign();
            no_type_declaration_--;
        }
        skip_space();
    } else {
        if (found_default_value && !found_splat && !splat && !double_splat) {
            raise("parameter must have a default value", param_location);
        }
    }

    if (!found_colon) {
        if (token_.type == Tok::SYMBOL) {
            raise_at_token("the syntax for a parameter with a default value V and type T is `param : T = V`", token_);
        }
        if (allow_restrictions && token_.type == Tok::OP_COLON) {
            raise_at_token("the syntax for a parameter with a default value V and type T is `param : T = V`", token_);
        }
    }

    Node *param = make(NodeKind::Arg);
    param->name = param_name;
    param->external_name = has_external_name ? external_name : param_name;
    param->default_value = default_value;
    param->restriction = restriction;
    if (has_annotations) {
        param->annotations = annotations;
        param->has_annotations = true;
    }
    at(param, param_location);
    params.push_back(param);
    push_var(param);

    ArgExtras e;
    e.default_value = default_value != nullptr;
    e.splat = splat;
    e.double_splat = double_splat;
    return e;
}

Node *Parser::parse_def_block_param(std::vector<Node *> *extra_assigns, bool has_annotations,
                                    std::vector<Node *> annotations) {
    const Location name_location = token_.location();
    std::string param_name;
    bool found_space = false;
    if (token_.type == Tok::OP_RPAREN || token_.type == Tok::NEWLINE || token_.type == Tok::OP_COLON) {
        param_name = "";
    } else {
        ParamName pn = parse_param_name(name_location, extra_assigns, false);
        param_name = pn.param_name;
        found_space = pn.found_space;
        if (pn.uses_param) uses_block_arg_ = true;
    }

    Node *type_spec = nullptr;
    if (token_.type == Tok::OP_COLON) {
        if (!(param_name.empty() || found_space)) {
            warn(token_.location(), "space required before colon in type restriction (run `crystal tool format` to fix this)");
        }
        next_token_skip_space_or_newline();
        type_spec = parse_bare_proc_type();
    }

    Node *block_param = make(NodeKind::Arg);
    block_param->name = param_name;
    block_param->external_name = param_name;
    block_param->restriction = type_spec;
    if (has_annotations) {
        block_param->annotations = std::move(annotations);
        block_param->has_annotations = true;
    }
    at(block_param, name_location);
    push_var(block_param);
    has_block_arg_name_ = true;
    block_arg_name_ = block_param->name;
    return block_param;
}

Parser::ParamName Parser::parse_param_name(Location location, std::vector<Node *> *extra_assigns,
                                           bool allow_external_name) {
    bool do_next_token = true;
    bool found_string_literal = false;
    bool has_invalid_internal_name = false;
    Token invalid_internal_name;
    bool has_external_name_token = false;
    Token external_name_token;
    ParamName r;

    if (allow_external_name && (token_.type == Tok::IDENT || string_literal_start())) {
        const Location name_location = token_.location();
        external_name_token = token_;
        has_external_name_token = true;
        if (token_.type == Tok::IDENT) {
            if (token_.keyword() && invalid_internal_name_kw(token_.kw)) {
                invalid_internal_name = token_;
                has_invalid_internal_name = true;
            }
            r.external_name = value_s();
            r.has_external_name = true;
            next_token();
        } else {
            r.external_name = parse_string_without_interpolation("external name");
            r.has_external_name = true;
            found_string_literal = true;
        }
        if (r.external_name.empty()) raise("external parameter name cannot be empty", name_location);
        r.found_space = token_.type == Tok::SPACE || token_.type == Tok::NEWLINE;
        skip_space();
        do_next_token = false;
    }

    switch (token_.type) {
        case Tok::IDENT: {
            if (token_.keyword() && invalid_internal_name_kw(token_.kw)) {
                raise_at_token("cannot use '" + token_.to_s() + "' as a parameter name", token_);
            }
            r.param_name = value_s();
            if (r.has_external_name && r.param_name == r.external_name) {
                raise_at_token("when specified, external name must be different than internal name", token_);
            }
            check_valid_param_name(token_);
            r.uses_param = false;
            do_next_token = true;
            break;
        }
        case Tok::INSTANCE_VAR: {
            r.param_name = value_s().substr(1);
            if (r.has_external_name && r.param_name == r.external_name) {
                raise_at_token("when specified, external name must be different than internal name", token_);
            }
            if (invalid_internal_name_str(r.param_name)) {
                if (!r.has_external_name) {
                    r.external_name = r.param_name;
                    r.has_external_name = true;
                }
                r.param_name = temp_arg_name();
            }
            Node *ivar = make(NodeKind::InstanceVar);
            ivar->name = value_s();
            at(ivar, location);
            Node *var = at(var_node(r.param_name), location);
            Node *assign = make(NodeKind::Assign);
            assign->target = ivar;
            assign->value_node = var;
            at(assign, location);
            if (extra_assigns) {
                extra_assigns->push_back(assign);
            } else {
                raise("can't use @instance_variable here");
            }
            r.uses_param = true;
            do_next_token = true;
            break;
        }
        case Tok::CLASS_VAR: {
            r.param_name = value_s().substr(2);
            if (r.has_external_name && r.param_name == r.external_name) {
                raise_at_token("when specified, external name must be different than internal name", token_);
            }
            if (invalid_internal_name_str(r.param_name)) {
                if (!r.has_external_name) {
                    r.external_name = r.param_name;
                    r.has_external_name = true;
                }
                r.param_name = temp_arg_name();
            }
            Node *cvar = make(NodeKind::ClassVar);
            cvar->name = value_s();
            at(cvar, location);
            Node *var = at(var_node(r.param_name), location);
            Node *assign = make(NodeKind::Assign);
            assign->target = cvar;
            assign->value_node = var;
            at(assign, location);
            if (extra_assigns) {
                extra_assigns->push_back(assign);
            } else {
                raise("can't use @@class_var here");
            }
            r.uses_param = true;
            do_next_token = true;
            break;
        }
        default:
            if (r.has_external_name) {
                if (found_string_literal) unexpected_token("expected parameter internal name");
                if (has_invalid_internal_name) {
                    raise_at_token("cannot use '" + invalid_internal_name.to_s() + "' as a parameter name",
                                   invalid_internal_name);
                }
                r.param_name = r.external_name;
                if (!has_external_name_token) raise("missing external name token");
                check_valid_param_name(external_name_token);
            } else {
                unexpected_token();
            }
    }

    if (do_next_token) {
        next_token();
        r.found_space = token_.type == Tok::SPACE || token_.type == Tok::NEWLINE;
    }
    skip_space();
    return r;
}

bool Parser::invalid_internal_name_kw(Keyword k) {
    switch (k) {
        case Keyword::BEGIN: case Keyword::NIL: case Keyword::TRUE: case Keyword::FALSE: case Keyword::YIELD:
        case Keyword::WITH: case Keyword::ABSTRACT: case Keyword::DEF: case Keyword::MACRO: case Keyword::REQUIRE:
        case Keyword::CASE: case Keyword::SELECT: case Keyword::IF: case Keyword::UNLESS: case Keyword::INCLUDE:
        case Keyword::EXTEND: case Keyword::CLASS: case Keyword::STRUCT: case Keyword::MODULE: case Keyword::ENUM:
        case Keyword::WHILE: case Keyword::UNTIL: case Keyword::RETURN: case Keyword::NEXT: case Keyword::BREAK:
        case Keyword::LIB: case Keyword::FUN: case Keyword::ALIAS: case Keyword::POINTEROF: case Keyword::SIZEOF:
        case Keyword::OFFSETOF: case Keyword::INSTANCE_SIZEOF: case Keyword::TYPEOF: case Keyword::PRIVATE:
        case Keyword::PROTECTED: case Keyword::ASM: case Keyword::OUT: case Keyword::SELF: case Keyword::IN:
        case Keyword::END: case Keyword::ALIGNOF: case Keyword::INSTANCE_ALIGNOF:
            return true;
        default:
            return false;
    }
}

bool Parser::invalid_internal_name_str(const std::string &s) {
    static const char *const kNames[] = {
        "begin", "nil", "true", "false", "yield", "with", "abstract", "def", "macro", "require", "case", "select",
        "if", "unless", "include", "extend", "class", "struct", "module", "enum", "while", "until", "return",
        "next", "break", "lib", "fun", "alias", "pointerof", "sizeof", "offsetof", "instance_sizeof", "typeof",
        "private", "protected", "asm", "out", "self", "in", "end", "alignof", "instance_alignof",
    };
    for (const char *n : kNames) {
        if (s == n) return true;
    }
    return false;
}

void Parser::check_valid_param_name(const Token &token) {
    const std::string param_name = token.value_s();
    if (!param_name.empty() && (param_name.back() == '?' || param_name.back() == '!')) {
        warn(token.location(), "invalid parameter name: " + param_name);
    }
}

Node *Parser::parse_if(bool check_end) {
    const Location location = token_.location();
    slash_is_regex_on();
    next_token_skip_space_or_newline();
    Node *cond = parse_op_assign_no_control(true, false);
    return parse_if_after_condition(cond, location, check_end);
}

Node *Parser::parse_if_after_condition(Node *cond, Location location, bool check_end) {
    slash_is_regex_on();
    skip_statement_end();

    Node *a_then = parse_expressions();
    skip_statement_end();

    Location else_location;
    Node *a_else = nullptr;
    if (token_.type == Tok::IDENT) {
        if (value_is(Keyword::ELSE)) {
            else_location = token_.location();
            next_token_skip_statement_end();
            a_else = parse_expressions();
        } else if (value_is(Keyword::ELSIF)) {
            else_location = token_.location();
            a_else = parse_if(false);
        }
    }

    const Location end_location = token_end_location();
    if (check_end) {
        check_ident(Keyword::END);
        next_token_skip_space();
    }

    Node *node = make(NodeKind::If);
    node->cond = cond;
    node->then_ = a_then ? a_then : nop();
    node->else_ = a_else ? a_else : nop();
    at_end(at(node, location), end_location);
    node->else_location = else_location;
    return node;
}

Node *Parser::parse_unless() {
    const Location location = token_.location();
    slash_is_regex_on();
    next_token_skip_space_or_newline();
    Node *cond = parse_op_assign_no_control(true, false);
    return parse_unless_after_condition(cond, location);
}

Node *Parser::parse_unless_after_condition(Node *cond, Location location) {
    slash_is_regex_on();
    skip_statement_end();

    Node *a_then = parse_expressions();
    skip_statement_end();

    Node *a_else = nullptr;
    Location else_location;
    if (tok_kw(Keyword::ELSE)) {
        else_location = token_.location();
        next_token_skip_statement_end();
        a_else = parse_expressions();
    }

    check_ident(Keyword::END);
    const Location end_location = token_end_location();
    next_token_skip_space();

    Node *node = make(NodeKind::Unless);
    node->cond = cond;
    node->then_ = a_then ? a_then : nop();
    node->else_ = a_else ? a_else : nop();
    at_end(at(node, location), end_location);
    node->else_location = else_location;
    return node;
}

Node *Parser::set_visibility(Node *node) {
    if (has_visibility_) node->set_visibility(visibility_);
    return node;
}

Node *Parser::parse_var_or_call(bool global, bool force_call, std::optional<Location> location_opt) {
    const Location location = location_opt ? *location_opt : token_.location();
    Location end_location = token_end_location();
    const bool has_doc = token_.has_doc;
    const std::string doc = token_.doc;

    check(kAtomicWithMethodCheck);

    if (token_.type == Tok::OP_BANG) {
        Node *obj = at(var_node("self"), location);
        return parse_negation_suffix(obj);
    }

    if (value_is(Keyword::IS_A_QUESTION)) return parse_is_a(at(var_node("self"), location));
    if (value_is(Keyword::AS)) return parse_as(at(var_node("self"), location));
    if (value_is(Keyword::AS_QUESTION)) return parse_as(at(var_node("self"), location), NodeKind::NilableCast);
    if (value_is(Keyword::RESPONDS_TO_QUESTION)) return parse_responds_to(at(var_node("self"), location));
    if (value_is(Keyword::NIL_QUESTION) && !in_macro_expression_) return parse_nil_q(at(var_node("self"), location));

    std::string name = value_s();
    const Location name_location = token_.location();

    if (force_call && token_.vkind == TokValue::None) name = TokName(token_.type);

    const bool is_var_name = is_var(name);

    // If the name is a var and '+' or '-' follow, never treat the name as a call.
    if (is_var_name && next_comes_plus_or_minus()) {
        Node *var = var_node(name);
        var->assign_doc(has_doc, doc);
        var->location = name_location;
        var->end_location = end_location;
        next_token();
        return var;
    }

    wants_regex_ = false;
    next_token();

    const bool name_followed_by_space = token_.type == Tok::SPACE;
    if (name_followed_by_space) wants_regex_ = !is_var_name;

    if (name == "super") {
        calls_super_ = true;
    } else if (name == "initialize") {
        calls_initialize_ = true;
    } else if (name == "previous_def") {
        calls_previous_def_ = true;
    }

    std::optional<CallArgs> call_args =
        preserve_stop_on_do(stop_on_do_, [&] { return parse_call_args(stop_on_do_); });

    std::vector<Node *> args;
    bool has_args = false;
    Node *block = nullptr;
    Node *block_arg = nullptr;
    std::vector<Node *> named_args;
    bool has_named = false;
    bool has_parentheses = false;
    if (call_args) {
        args = call_args->args;
        has_args = call_args->has_args;
        block = call_args->block;
        block_arg = call_args->block_arg;
        named_args = call_args->named_args;
        has_named = call_args->has_named_args;
        has_parentheses = call_args->has_parentheses;
        force_call = force_call || has_parentheses || (has_args && !args.empty()) || (has_named && !named_args.empty());
    }

    if (call_args && call_args->stopped_on_do_after_space) {
        block = parse_curly_block(block);
    } else if (stop_on_do_ && call_args && call_args->has_parentheses) {
        block = parse_curly_block(block);
    } else {
        block = parse_block(block);
    }

    if (block && block_arg) raise("can't use captured and non-captured blocks together", location);

    Node *node;
    if (block || block_arg || global) {
        node = call_node(nullptr, name, args, block, block_arg, has_named ? &named_args : nullptr, global);
        node->name_location = name_location;
        node->has_parentheses = has_parentheses;
    } else if (has_args) {
        const bool maybe_var = !force_call && is_var_name;
        if (maybe_var) {
            node = var_node(name);
        } else {
            node = call_node(nullptr, name, args, nullptr, nullptr, has_named ? &named_args : nullptr, global);
            node->name_location = name_location;
            node->has_parentheses = has_parentheses;
        }
    } else {
        if (no_type_declaration_ == 0 && token_.type == Tok::OP_COLON) {
            if (!name_followed_by_space) {
                warn(token_.location(),
                     "space required before colon in type declaration (run `crystal tool format` to fix this)");
            }
            Node *declare_var = parse_type_declaration(at_end(at(var_node(name), location), end_location));
            end_location = EndLoc(declare_var);
            if (std::find(call_args_start_locations_.begin(), call_args_start_locations_.end(), location) ==
                call_args_start_locations_.end()) {
                push_var(declare_var);
            }
            node = declare_var;
        } else if (!force_call && is_var_name) {
            if (has_block_arg_name_ && !uses_block_arg_ && name == block_arg_name_) uses_block_arg_ = true;
            node = var_node(name);
        } else {
            if (!force_call && !has_named && !global &&
                std::find(assigned_vars_.begin(), assigned_vars_.end(), name) != assigned_vars_.end()) {
                raise("can't use variable name '" + name + "' inside assignment to variable '" + name + "'", location);
            }
            node = call_node(nullptr, name, {}, nullptr, nullptr, has_named ? &named_args : nullptr, global);
            node->name_location = name_location;
            node->has_parentheses = has_parentheses;
        }
    }
    node->assign_doc(has_doc, doc);
    node->location = location;
    if (block && EndLoc(block).known()) {
        node->end_location = EndLoc(block);
    } else if (call_args && call_args->end_location.known()) {
        node->end_location = call_args->end_location;
    } else {
        node->end_location = end_location;
    }
    return node;
}

bool Parser::next_comes_plus_or_minus() {
    const int pos = current_pos();
    auto ws = [](char32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; };
    while (ws(current_char())) next_char_no_column_increment();
    const bool comes = current_char() == '+' || current_char() == '-';
    set_current_pos(pos);
    return comes;
}

Node *Parser::parse_block(Node *block, bool stop_on_do) {
    if (tok_kw(Keyword::DO)) {
        if (stop_on_do) return block;
        if (block) raise("block already specified with &");
        return parse_block2([&](Node *body) { return parse_exception_handler(body, true); });
    }
    return parse_curly_block(block);
}

Node *Parser::parse_curly_block(Node *block) {
    if (token_.type == Tok::OP_LCURLY) {
        if (block) raise("block already specified with &");
        return parse_block2([&](Node *body) {
            check(Tok::OP_RCURLY);
            const Location end_location = token_end_location();
            slash_is_not_regex();
            next_token_skip_space();
            return std::pair<Node *, Location>{body, end_location};
        });
    }
    return block;
}

template <typename F>
Node *Parser::parse_block2(F &&f) {
    const Location location = token_.location();
    std::vector<Node *> block_params;
    int splat_index = -1;
    std::map<int, Node *> unpacks;
    bool has_unpacks = false;
    parse_block_params(location, block_params, splat_index, unpacks, has_unpacks);

    return with_lexical_var_scope([&]() -> Node * {
        for (Node *p : block_params) push_var(p);
        for (auto &kv : unpacks) push_block_vars(kv.second);
        Node *block_body = parse_expressions();
        auto [body, end_location] = f(block_body);
        Node *b = make(NodeKind::Block);
        b->exps = block_params;
        b->body = body ? body : nop();
        b->splat_index = splat_index;
        b->unpacks = unpacks;
        b->has_unpacks = has_unpacks;
        return at_end(at(b, location), end_location);
    });
}

void Parser::parse_block_params(Location, std::vector<Node *> &block_params, int &splat_index,
                                std::map<int, Node *> &unpacks, bool &has_unpacks) {
    std::vector<std::string> all_names;
    int param_index = 0;
    slash_is_regex_on();
    next_token_skip_space();
    if (token_.type == Tok::OP_BAR) {
        next_token_skip_space_or_newline();
        while (true) {
            BlockParam bp = parse_block_param(splat_index >= 0, all_names);
            if (splat_index < 0 && bp.found_splat) splat_index = param_index;
            block_params.push_back(bp.var);
            if (bp.has_unpack) {
                has_unpacks = true;
                unpacks[param_index] = new_expressions(bp.unpack_expressions);
            }
            next_token_skip_space_or_newline();
            if (token_.type == Tok::OP_COMMA) {
                next_token_skip_space_or_newline();
                if (token_.type == Tok::OP_BAR) break;
            } else if (token_.type == Tok::OP_BAR) {
                break;
            } else {
                raise_at_token("expecting ',' or '|', not " + token_.to_s(), token_);
            }
            param_index++;
        }
        next_token_skip_statement_end();
    } else {
        skip_statement_end();
    }
}

Parser::BlockParam Parser::parse_block_param(bool found_splat, std::vector<std::string> &all_names) {
    if (token_.type == Tok::OP_STAR) {
        if (found_splat) raise_at_token("splat block parameter already specified", token_);
        found_splat = true;
        next_token();
    }

    const Location location = token_.location();
    BlockParam r;
    std::string param_name;

    if (token_.type == Tok::IDENT) {
        if (token_.keyword() && invalid_internal_name_kw(token_.kw)) {
            raise_at_token("cannot use '" + token_.to_s() + "' as a block parameter name", token_);
        }
        param_name = value_s();
        check_valid_param_name(token_);
        if (std::find(all_names.begin(), all_names.end(), param_name) != all_names.end()) {
            raise_at_token("duplicated block parameter name: " + param_name, token_);
        }
        all_names.push_back(param_name);
    } else if (token_.type == Tok::UNDERSCORE) {
        param_name = "_";
    } else if (token_.type == Tok::OP_LPAREN) {
        next_token_skip_space_or_newline();
        std::vector<Node *> unpack_expressions;
        bool found_splat_in_nested_expression = false;
        while (true) {
            const Location sub_location = token_.location();
            BlockParam sub = parse_block_param(found_splat_in_nested_expression, all_names);
            Node *unpack_expression;
            if (sub.has_unpack) {
                unpack_expression = at(new_expressions(sub.unpack_expressions), sub_location);
            } else if (sub.var->name == "_") {
                unpack_expression = at(make(NodeKind::Underscore), sub_location);
            } else {
                unpack_expression = sub.var;
            }
            if (sub.found_splat && !found_splat_in_nested_expression) {
                unpack_expression = at(unary(NodeKind::Splat, unpack_expression), sub_location);
            }
            found_splat_in_nested_expression = sub.found_splat;
            unpack_expressions.push_back(unpack_expression);

            next_token_skip_space_or_newline();
            if (token_.type == Tok::OP_COMMA) {
                next_token_skip_space_or_newline();
                if (token_.type == Tok::OP_RPAREN) break;
            } else if (token_.type == Tok::OP_RPAREN) {
                break;
            } else {
                raise_at_token("expecting ',' or ')', not " + token_.to_s(), token_);
            }
        }
        param_name = "";
        r.has_unpack = true;
        r.unpack_expressions = std::move(unpack_expressions);
    } else {
        raise_at_token(std::string("expecting block parameter name, not ") + TokName(token_.type), token_);
    }

    r.var = at(var_node(param_name), location);
    r.found_splat = found_splat;
    return r;
}

void Parser::push_block_vars(Node *node) {
    switch (node->kind) {
        case NodeKind::Expressions:
            for (Node *e : node->exps) push_block_vars(e);
            break;
        case NodeKind::Var: push_var(node); break;
        case NodeKind::Underscore: break;
        case NodeKind::Splat: push_block_vars(node->exp); break;
        default: raise("BUG: unexpected block var");
    }
}

std::optional<Parser::CallArgs> Parser::parse_call_args(bool stop_on_do_after_space, bool allow_curly, bool control) {
    switch (token_.type) {
        case Tok::OP_LCURLY:
            return std::nullopt;
        case Tok::OP_LPAREN: {
            slash_is_regex_on();
            std::vector<Node *> args;
            Location end_location;
            std::optional<CallArgs> early;
            open("call", token_.location(), [&] {
                stop_on_do_ = false;
                bool found_double_splat = false;
                next_token_skip_space_or_newline();
                while (token_.type != Tok::OP_RPAREN) {
                    if (call_block_arg_follows()) {
                        early = parse_call_block_arg(args, true);
                        return 0;
                    }
                    if (named_tuple_start()) {
                        early = parse_call_args_named_args(token_.location(), args, nullptr, true);
                        return 0;
                    }
                    Node *arg = parse_call_arg(found_double_splat);
                    if (token_.type == Tok::OP_COLON && arg->is(NodeKind::StringLiteral)) {
                        early = parse_call_args_named_args(Loc(arg), args, &arg->value, true);
                        return 0;
                    }
                    args.push_back(arg);
                    found_double_splat = arg->is(NodeKind::DoubleSplat);
                    skip_space();
                    if (token_.type == Tok::OP_COMMA) {
                        slash_is_regex_on();
                        next_token_skip_space_or_newline();
                    } else {
                        skip_space_or_newline();
                        check(Tok::OP_RPAREN);
                        break;
                    }
                }
                end_location = token_end_location();
                wants_regex_ = false;
                next_token_skip_space();
                return 0;
            });
            if (early) return early;
            CallArgs ca;
            ca.args = std::move(args);
            ca.has_args = true;
            ca.end_location = end_location;
            ca.has_parentheses = true;
            return ca;
        }
        case Tok::SPACE: {
            slash_is_not_regex();
            const Location end_location = token_end_location();
            next_token();
            if (stop_on_do_after_space && tok_kw(Keyword::DO)) {
                CallArgs ca;
                ca.stopped_on_do_after_space = true;
                ca.end_location = end_location;
                return ca;
            }
            if (control && tok_kw(Keyword::DO)) unexpected_token();
            return parse_call_args_space_consumed(true, allow_curly, control);
        }
        default:
            return std::nullopt;
    }
}

std::optional<Parser::CallArgs> Parser::parse_call_args_space_consumed(bool check_plus_and_minus, bool allow_curly,
                                                                       bool control, Tok end_tok,
                                                                       bool allow_beginless_range) {
    if (tok_kw(Keyword::END) && !next_comes_colon_space()) return std::nullopt;

    auto ws = [](char32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; };

    switch (token_.type) {
        case Tok::OP_AMP:
            if (ws(current_char())) return std::nullopt;
            break;
        case Tok::OP_PLUS:
        case Tok::OP_MINUS:
            if (check_plus_and_minus && ws(current_char())) return std::nullopt;
            break;
        case Tok::OP_LCURLY:
            if (!allow_curly) return std::nullopt;
            break;
        case Tok::CHAR: case Tok::STRING: case Tok::DELIMITER_START: case Tok::STRING_ARRAY_START:
        case Tok::SYMBOL_ARRAY_START: case Tok::NUMBER: case Tok::IDENT: case Tok::SYMBOL: case Tok::INSTANCE_VAR:
        case Tok::CLASS_VAR: case Tok::CONST: case Tok::GLOBAL: case Tok::OP_DOLLAR_TILDE:
        case Tok::OP_DOLLAR_QUESTION: case Tok::GLOBAL_MATCH_DATA_INDEX: case Tok::OP_LPAREN: case Tok::OP_BANG:
        case Tok::OP_LSQUARE: case Tok::OP_LSQUARE_RSQUARE: case Tok::OP_TILDE: case Tok::OP_MINUS_GT:
        case Tok::OP_LCURLY_LCURLY: case Tok::MAGIC_DIR: case Tok::MAGIC_END_LINE: case Tok::MAGIC_FILE:
        case Tok::MAGIC_LINE: case Tok::UNDERSCORE:
            break;
        case Tok::OP_STAR:
        case Tok::OP_STAR_STAR:
            if (ws(current_char())) return std::nullopt;
            break;
        case Tok::OP_COLON_COLON:
            if (ws(current_char())) return std::nullopt;
            break;
        case Tok::OP_PERIOD_PERIOD:
        case Tok::OP_PERIOD_PERIOD_PERIOD:
            if (!allow_beginless_range) return std::nullopt;
            break;
        default:
            return std::nullopt;
    }

    if (value_is(Keyword::IF) || value_is(Keyword::UNLESS) || value_is(Keyword::WHILE) || value_is(Keyword::UNTIL) ||
        value_is(Keyword::RESCUE) || value_is(Keyword::ENSURE)) {
        if (!next_comes_colon_space()) return std::nullopt;
    } else if (value_is(Keyword::YIELD)) {
        if (stop_on_yield_ > 0 && !next_comes_colon_space()) return std::nullopt;
    }

    std::vector<Node *> args;
    Location end_location;

    if (!control) stop_on_do_ = true;

    bool found_double_splat = false;

    while (token_.type != Tok::NEWLINE && token_.type != Tok::OP_SEMICOLON && token_.type != Tok::EOF_ &&
           token_.type != end_tok && token_.type != Tok::OP_COLON && !end_token()) {
        if (call_block_arg_follows()) return parse_call_block_arg(args, false);

        if (token_.type == Tok::IDENT && current_char() == ':') {
            return parse_call_args_named_args(token_.location(), args, nullptr, false);
        }
        Node *arg = parse_call_arg(found_double_splat);
        if (token_.type == Tok::OP_COLON && arg->is(NodeKind::StringLiteral)) {
            return parse_call_args_named_args(Loc(arg), args, &arg->value, false);
        }
        args.push_back(arg);
        found_double_splat = arg->is(NodeKind::DoubleSplat);
        end_location = EndLoc(arg);

        skip_space();

        if (token_.type == Tok::OP_COMMA) {
            const Location location = token_.location();
            slash_is_regex_on();
            next_token_skip_space_or_newline();
            if ((tok_kw(Keyword::END) && !next_comes_colon_space()) || token_.type == Tok::EOF_) {
                raise("invalid trailing comma in call", location);
            }
        } else {
            break;
        }
    }

    CallArgs ca;
    ca.args = std::move(args);
    ca.has_args = true;
    ca.end_location = end_location;
    ca.has_parentheses = false;
    return ca;
}

Parser::CallArgs Parser::parse_call_args_named_args(Location location, std::vector<Node *> args,
                                                    const std::string *first_name, bool allow_newline) {
    std::vector<Node *> named_args = parse_named_args(location, first_name, allow_newline);

    if (call_block_arg_follows()) return parse_call_block_arg(args, allow_newline, &named_args);

    if (allow_newline) check(Tok::OP_RPAREN);

    Location end_location;
    if (!allow_newline && !named_args.empty()) end_location = EndLoc(named_args.back());
    if (!end_location.known()) end_location = token_end_location();

    if (allow_newline) {
        next_token_skip_space();
    } else {
        skip_space();
    }
    CallArgs ca;
    ca.args = std::move(args);
    ca.has_args = true;
    ca.named_args = std::move(named_args);
    ca.has_named_args = true;
    ca.end_location = end_location;
    ca.has_parentheses = allow_newline;
    return ca;
}

std::vector<Node *> Parser::parse_named_args(Location location, const std::string *first_name_in, bool allow_newline) {
    std::vector<Node *> named_args;
    bool has_first = first_name_in != nullptr;
    std::string first_name = first_name_in ? *first_name_in : std::string();
    while (true) {
        std::string name;
        if (has_first) {
            name = first_name;
            has_first = false;
        } else {
            if (named_tuple_start()) {
                name = value_s();
                next_token_never_a_symbol();
            } else if (string_literal_start()) {
                name = parse_string_without_interpolation("named argument");
            } else {
                raise("expected named argument, not " + token_.to_s(), location);
            }
        }

        if (name.empty()) raise("named argument cannot have an empty name", location);
        for (Node *a : named_args) {
            if (a->name == name) raise("duplicated named argument: " + name, location);
        }

        check(Tok::OP_COLON);
        slash_is_regex_on();
        next_token_skip_space_or_newline();

        Node *value;
        if (tok_kw(Keyword::OUT)) {
            value = parse_out();
        } else {
            call_args_start_locations_.push_back(token_.location());
            try {
                value = parse_op_assign();
            } catch (...) {
                call_args_start_locations_.pop_back();
                throw;
            }
            call_args_start_locations_.pop_back();
        }

        Node *na = make(NodeKind::NamedArgument);
        na->name = name;
        na->value_node = value;
        named_args.push_back(at(na, location));
        skip_space();
        if (token_.type == Tok::OP_COMMA) {
            next_token_skip_space_or_newline();
            if (token_.type == Tok::OP_RPAREN || token_.type == Tok::OP_AMP || token_.type == Tok::OP_RSQUARE) break;
        } else if (token_.type == Tok::NEWLINE && allow_newline) {
            skip_space_or_newline();
            break;
        } else {
            break;
        }
        location = token_.location();
    }
    return named_args;
}

Node *Parser::parse_call_arg(bool found_double_splat) {
    call_args_start_locations_.push_back(token_.location());
    struct Pop {
        Parser *p;
        ~Pop() { p->call_args_start_locations_.pop_back(); }
    } pop{this};

    if (tok_kw(Keyword::OUT)) {
        if (found_double_splat) raise("out argument not allowed after double splat");
        return parse_out();
    }

    const Location location = token_.location();
    int splat = 0;  // 1 single, 2 double
    auto ws = [](char32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; };
    if (token_.type == Tok::OP_STAR) {
        if (!ws(current_char())) {
            if (found_double_splat) raise("splat not allowed after double splat");
            splat = 1;
            next_token();
        }
    } else if (token_.type == Tok::OP_STAR_STAR) {
        if (!ws(current_char())) {
            splat = 2;
            next_token();
        }
    }

    Node *arg = parse_op_assign_no_control();

    if (found_double_splat && splat != 2) raise("argument not allowed after double splat", Loc(arg));

    if (splat == 1) {
        arg = at_end(at(unary(NodeKind::Splat, arg), location), arg);
    } else if (splat == 2) {
        arg = at_end(at(unary(NodeKind::DoubleSplat, arg), location), arg);
    }
    return arg;
}

Node *Parser::parse_out() {
    const Location location = token_.location();
    next_token_skip_space_or_newline();
    const std::string name = value_s();
    Node *var;
    switch (token_.type) {
        case Tok::IDENT:
            var = var_node(name);
            push_var(var);
            break;
        case Tok::INSTANCE_VAR:
            var = make(NodeKind::InstanceVar);
            var->name = name;
            break;
        case Tok::UNDERSCORE:
            var = make(NodeKind::Underscore);
            break;
        default:
            raise("expecting variable or instance variable after out");
    }
    at_end(at(var, token_.location()), token_end_location());
    next_token();
    return at_end(at(unary(NodeKind::Out, var), location), var);
}

Node *Parser::parse_generic_or_global_call() {
    const Location location = token_.location();
    next_token_skip_space_or_newline();
    if (token_.type == Tok::IDENT) {
        return set_visibility(parse_var_or_call(true, false, location));
    } else if (token_.type == Tok::CONST) {
        Node *ident = parse_generic(true, location, true);
        skip_space();
        return parse_custom_literal(ident);
    }
    unexpected_token();
}

Node *Parser::parse_bare_proc_type() {
    Node *type = parse_type_splat([&] { return parse_union_type(); });

    if (!(token_.type == Tok::OP_MINUS_GT || (token_.type == Tok::OP_COMMA && type_start(true)))) {
        if (type->is(NodeKind::Splat)) raise("invalid type splat", Loc(type));
        return type;
    }

    std::vector<Node *> input_types = {type};
    if (token_.type != Tok::OP_MINUS_GT) {
        while (true) {
            next_token_skip_space_or_newline();
            input_types.push_back(parse_type_splat([&] { return parse_union_type(); }));
            if (!(token_.type == Tok::OP_COMMA && type_start(true))) break;
        }
    }
    return parse_proc_type_output(&input_types, Loc(input_types.front()));
}

Node *Parser::parse_union_type() {
    Node *type = parse_atomic_type_with_suffix();
    if (token_.type != Tok::OP_BAR) return type;
    std::vector<Node *> types = {type};
    while (token_.type == Tok::OP_BAR) {
        next_token_skip_space_or_newline();
        types.push_back(parse_atomic_type_with_suffix());
    }
    Node *u = make(NodeKind::Union);
    Node *last = types.back();
    u->exps = std::move(types);
    at(u, type);
    return at_end(u, last);
}

Node *Parser::parse_atomic_type_with_suffix() {
    Node *type = parse_atomic_type();
    return parse_type_suffix(type);
}

Node *Parser::parse_atomic_type() {
    const Location location = token_.location();
    switch (token_.type) {
        case Tok::IDENT:
            if (value_is(Keyword::SELF)) {
                next_token_skip_space();
                return at(make(NodeKind::Self), location);
            } else if (value_is_str("self?")) {
                next_token_skip_space();
                return make_nilable_type(at(make(NodeKind::Self), location));
            } else if (value_is(Keyword::TYPEOF)) {
                return parse_typeof();
            }
            unexpected_token();
        case Tok::UNDERSCORE:
            next_token_skip_space();
            return at(make(NodeKind::Underscore), location);
        case Tok::CONST:
        case Tok::OP_COLON_COLON: {
            Node *type = parse_generic();
            skip_space();
            return type;
        }
        case Tok::OP_LCURLY: {
            next_token_skip_space_or_newline();
            Node *type;
            if (named_tuple_start() || token_.type == Tok::DELIMITER_START) {
                type = make_named_tuple_type(parse_named_type_args(Tok::OP_RCURLY));
            } else {
                type = make_tuple_type(parse_union_types(Tok::OP_RCURLY, true));
            }
            check(Tok::OP_RCURLY);
            const Location end_location = token_end_location();
            next_token_skip_space();
            return at_end(at(type, location), end_location);
        }
        case Tok::OP_MINUS_GT:
            return parse_proc_type_output(nullptr, location);
        case Tok::OP_LPAREN: {
            next_token_skip_space_or_newline();
            Node *type = parse_type_splat([&] { return parse_union_type(); });
            if (type->is(NodeKind::Union)) at_end(at(type, location), token_.location());
            if (token_.type == Tok::OP_RPAREN) {
                next_token_skip_space();
                if (token_.type == Tok::OP_MINUS_GT) {
                    std::vector<Node *> in = {type};
                    type = parse_proc_type_output(&in, location);
                } else if (type->is(NodeKind::Splat)) {
                    raise("invalid type splat", Loc(type));
                }
            } else {
                std::vector<Node *> input_types = {type};
                while (token_.type == Tok::OP_COMMA) {
                    next_token_skip_space_or_newline();
                    if (token_.type == Tok::OP_RPAREN) break;
                    input_types.push_back(parse_type_splat([&] { return parse_union_type(); }));
                }
                if (token_.type == Tok::OP_MINUS_GT) {
                    type = parse_proc_type_output(&input_types, Loc(input_types.front()));
                    check(Tok::OP_RPAREN);
                    next_token_skip_space();
                } else {
                    check(Tok::OP_RPAREN);
                    next_token_skip_space();
                    type = parse_proc_type_output(&input_types, location);
                }
            }
            return type;
        }
        default:
            unexpected_token();
    }
}

std::vector<Node *> Parser::parse_union_types(Tok end_tok, bool allow_splats) {
    auto one = [&] {
        return allow_splats ? parse_type_splat([&] { return parse_union_type(); }) : parse_union_type();
    };
    std::vector<Node *> types = {one()};
    while (token_.type == Tok::OP_COMMA) {
        next_token_skip_space_or_newline();
        if (token_.type == end_tok) break;
        types.push_back(one());
    }
    return types;
}

Node *Parser::parse_generic(bool expression) {
    const Location location = token_.location();
    bool global = false;
    if (token_.type == Tok::OP_COLON_COLON) {
        next_token_skip_space_or_newline();
        global = true;
    }
    return parse_generic(global, location, expression);
}

Node *Parser::parse_generic(bool global, Location location, bool expression) {
    Node *path = parse_path(global, location);
    Node *type = parse_type_args(path);
    while (expression && token_.type == Tok::OP_QUESTION) {
        const Location end_location = token_end_location();
        next_token();
        type = at_end(make_nilable_expression(type), end_location);
    }
    return type;
}

Node *Parser::parse_path() {
    const Location location = token_.location();
    bool global = false;
    if (token_.type == Tok::OP_COLON_COLON) {
        next_token_skip_space_or_newline();
        global = true;
    }
    return parse_path(global, location);
}

Node *Parser::parse_path(bool global, Location location) {
    std::vector<std::string> names = {check_const()};
    Location end_location = token_end_location();
    wants_regex_ = false;
    next_token();
    while (token_.type == Tok::OP_COLON_COLON) {
        next_token_skip_space_or_newline();
        names.push_back(check_const());
        end_location = token_end_location();
        wants_regex_ = false;
        next_token();
    }
    Node *p = make(NodeKind::Path);
    p->names = std::move(names);
    p->global = global;
    return at_end(at(p, location), end_location);
}

Node *Parser::parse_type_args(Node *name) {
    if (token_.type != Tok::OP_LPAREN) return name;

    next_token_skip_space_or_newline();
    std::vector<Node *> args;
    std::vector<Node *> named_args;
    bool has_named = false;
    if (named_tuple_start() || string_literal_start()) {
        named_args = parse_named_type_args(Tok::OP_RPAREN);
        has_named = true;
    } else if (token_.type != Tok::OP_RPAREN) {
        args.push_back(parse_type_splat([&] { return parse_type_arg(); }));
        while (token_.type == Tok::OP_COMMA) {
            next_token_skip_space_or_newline();
            if (token_.type == Tok::OP_RPAREN) break;
            args.push_back(parse_type_splat([&] { return parse_type_arg(); }));
        }
        bool has_int = false;
        for (Node *a : args) {
            if (a->is(NodeKind::NumberLiteral) || a->is(NodeKind::SizeOf) || a->is(NodeKind::InstanceSizeOf) ||
                a->is(NodeKind::AlignOf) || a->is(NodeKind::InstanceAlignOf) || a->is(NodeKind::OffsetOf)) {
                has_int = true;
            }
        }
        if (token_.type == Tok::OP_MINUS_GT && !has_int) {
            args = {parse_proc_type_output(&args, Loc(args.front()))};
        }
    }

    skip_space_or_newline();
    check(Tok::OP_RPAREN);
    const Location end_location = token_end_location();
    next_token();

    Node *g = make(NodeKind::Generic);
    g->name_node = name;
    g->exps = std::move(args);
    if (has_named) {
        g->named_args = std::move(named_args);
        g->has_named_args = true;
    }
    at(g, name);
    return at_end(g, end_location);
}

std::vector<Node *> Parser::parse_named_type_args(Tok end_tok) {
    std::vector<Node *> named_args;
    while (token_.type != end_tok) {
        const Location name_location = token_.location();
        std::string name;
        if (named_tuple_start()) {
            name = value_s();
            next_token();
        } else if (string_literal_start()) {
            name = parse_string_without_interpolation("named argument");
        } else {
            raise_at_token(std::string("expected '") + TokName(end_tok) + "' or named argument, not " + token_.to_s(),
                           token_);
        }
        if (name.empty()) raise("named argument cannot have an empty name", name_location);
        for (Node *a : named_args) {
            if (a->name == name) raise_at_token("duplicated key: " + name, token_);
        }
        check(Tok::OP_COLON);
        next_token_skip_space_or_newline();
        Node *type = parse_bare_proc_type();
        skip_space();
        Node *na = make(NodeKind::NamedArgument);
        na->name = name;
        na->value_node = type;
        named_args.push_back(at(na, name_location));
        if (token_.type == Tok::OP_COMMA) {
            next_token_skip_space_or_newline();
        } else {
            skip_space_or_newline();
            check(end_tok);
            break;
        }
    }
    return named_args;
}

Node *Parser::parse_type_arg() {
    if (token_.type == Tok::NUMBER) {
        Node *num = make(NodeKind::NumberLiteral);
        num->value = value_s();
        num->number_kind = token_.number_kind;
        at(num, token_.location());
        next_token_skip_space();
        return num;
    }
    if (tok_kw(Keyword::SIZEOF)) return parse_sizeof(NodeKind::SizeOf);
    if (tok_kw(Keyword::INSTANCE_SIZEOF)) return parse_sizeof(NodeKind::InstanceSizeOf);
    if (tok_kw(Keyword::ALIGNOF)) return parse_sizeof(NodeKind::AlignOf);
    if (tok_kw(Keyword::INSTANCE_ALIGNOF)) return parse_sizeof(NodeKind::InstanceAlignOf);
    if (tok_kw(Keyword::OFFSETOF)) return parse_offsetof();
    return parse_union_type();
}

Node *Parser::parse_type_suffix(Node *type) {
    while (true) {
        Location end_location = token_end_location();
        switch (token_.type) {
            case Tok::OP_PERIOD: {
                next_token_skip_space_or_newline();
                check_ident(Keyword::CLASS);
                end_location = token_end_location();
                next_token_skip_space();
                Node *m = make(NodeKind::Metaclass);
                m->name_node = type;
                type = at_end(at(m, type), end_location);
                break;
            }
            case Tok::OP_QUESTION:
                next_token_skip_space();
                type = at_end(make_nilable_type(type), end_location);
                break;
            case Tok::OP_STAR:
                next_token_skip_space();
                type = at_end(make_pointer_type(type), end_location);
                break;
            case Tok::OP_STAR_STAR:
                next_token_skip_space();
                type = at_end(make_pointer_type(make_pointer_type(type)), end_location);
                break;
            case Tok::OP_LSQUARE: {
                next_token_skip_space_or_newline();
                Node *size = parse_type_arg();
                skip_space_or_newline();
                check(Tok::OP_RSQUARE);
                end_location = token_end_location();
                next_token_skip_space();
                type = at_end(make_static_array_type(type, size), end_location);
                break;
            }
            default:
                return type;
        }
    }
}

Node *Parser::parse_proc_type_output(const std::vector<Node *> *input_types, Location location) {
    const bool has_output_type = type_start(false);
    check(Tok::OP_MINUS_GT);
    Location end_location = token_end_location();
    next_token_skip_space();
    Node *output_type = nullptr;
    if (has_output_type) {
        skip_space_or_newline();
        output_type = parse_union_type();
        end_location = EndLoc(output_type);
    }
    Node *p = make(NodeKind::ProcNotation);
    if (input_types) {
        p->exps = *input_types;
        p->has_exps = true;
    }
    p->right = output_type;
    return at_end(at(p, location), end_location);
}

Node *Parser::make_nilable_type(Node *type) {
    Node *nil = at(path_global("Nil"), type);
    Node *u = make(NodeKind::Union);
    u->exps = {type, nil};
    return at(u, type);
}

Node *Parser::make_nilable_expression(Node *type) {
    Node *g = make(NodeKind::Generic);
    g->name_node = at(path_global("Union"), type);
    g->exps = {type, at(path_global("Nil"), type)};
    at(g, type);
    g->suffix = GenericSuffix::Question;
    return g;
}

Node *Parser::make_pointer_type(Node *type) {
    Node *g = make(NodeKind::Generic);
    g->name_node = at(path_global("Pointer"), type);
    g->exps = {type};
    at(g, type);
    g->suffix = GenericSuffix::Asterisk;
    return g;
}

Node *Parser::make_static_array_type(Node *type, Node *size) {
    Node *g = make(NodeKind::Generic);
    g->name_node = at(path_global("StaticArray"), type);
    g->exps = {type, size};
    at(g, type);
    g->suffix = GenericSuffix::Bracket;
    return g;
}

Node *Parser::make_tuple_type(std::vector<Node *> types) {
    Node *g = make(NodeKind::Generic);
    g->name_node = path_global("Tuple");
    g->exps = std::move(types);
    return g;
}

Node *Parser::make_named_tuple_type(std::vector<Node *> named_args) {
    Node *g = make(NodeKind::Generic);
    g->name_node = path_global("NamedTuple");
    g->named_args = std::move(named_args);
    g->has_named_args = true;
    return g;
}

bool Parser::type_start(bool consume_newlines) {
    bool result = false;
    lookahead(
        [&] {
            try {
                if (consume_newlines) {
                    next_token_skip_space_or_newline();
                } else {
                    next_token_skip_space();
                }
                result = type_start_here();
            } catch (const SyntaxError &) {
                result = false;
            }
            return false;
        },
        true);
    return result;
}

bool Parser::type_start_here() {
    while (token_.type == Tok::OP_LPAREN || token_.type == Tok::OP_LCURLY) next_token_skip_space_or_newline();
    switch (token_.type) {
        case Tok::IDENT:
            if (named_tuple_start()) return false;
            if (value_is(Keyword::TYPEOF)) return true;
            if (value_is(Keyword::SELF) || value_is_str("self?")) {
                next_token_skip_space();
                return delimiter_or_type_suffix();
            }
            return false;
        case Tok::CONST:
            if (named_tuple_start()) return false;
            return type_path_start();
        case Tok::OP_COLON_COLON:
            next_token();
            return type_path_start();
        case Tok::UNDERSCORE:
        case Tok::OP_MINUS_GT:
            return true;
        case Tok::OP_STAR:
            next_token_skip_space_or_newline();
            return type_start_here();
        default:
            return false;
    }
}

bool Parser::type_path_start() {
    while (token_.type == Tok::CONST) {
        next_token();
        if (token_.type != Tok::OP_COLON_COLON) break;
        next_token_skip_space_or_newline();
    }
    skip_space();
    return delimiter_or_type_suffix();
}

bool Parser::delimiter_or_type_suffix() {
    switch (token_.type) {
        case Tok::OP_PERIOD:
            next_token_skip_space_or_newline();
            return tok_kw(Keyword::CLASS);
        case Tok::OP_QUESTION: case Tok::OP_STAR: case Tok::OP_STAR_STAR:
            next_token_skip_space();
            return delimiter_or_type_suffix();
        case Tok::OP_MINUS_GT: case Tok::OP_BAR: case Tok::OP_COMMA: case Tok::OP_EQ_GT: case Tok::NEWLINE:
        case Tok::EOF_: case Tok::OP_EQ: case Tok::OP_SEMICOLON: case Tok::OP_LPAREN: case Tok::OP_RPAREN:
        case Tok::OP_LSQUARE: case Tok::OP_RSQUARE:
            return true;
        default:
            return false;
    }
}

Node *Parser::parse_typeof() {
    const Location location = token_.location();
    next_token_skip_space();
    check(Tok::OP_LPAREN);
    next_token_skip_space_or_newline();
    if (token_.type == Tok::OP_RPAREN) raise("missing typeof argument");
    return with_lexical_var_scope([&]() -> Node * {
        std::vector<Node *> exps;
        while (token_.type != Tok::OP_RPAREN) {
            exps.push_back(parse_op_assign());
            if (token_.type == Tok::OP_COMMA) {
                next_token_skip_space_or_newline();
            } else {
                skip_space_or_newline();
                check(Tok::OP_RPAREN);
            }
        }
        const Location end_location = token_end_location();
        next_token_skip_space();
        Node *t = make(NodeKind::TypeOf);
        t->exps = std::move(exps);
        return at_end(at(t, location), end_location);
    });
}

Node *Parser::parse_visibility_modifier(Visibility modifier) {
    bool has_doc;
    const std::string doc = doc_or_empty(has_doc);
    const Location location = token_.location();
    next_token_skip_space();
    Node *exp = parse_op_assign();
    Node *m = make(NodeKind::VisibilityModifier);
    m->modifier = modifier;
    m->exp = exp;
    at(m, location);
    m->assign_doc(has_doc, doc);
    exp->assign_doc(has_doc, doc);
    return m;
}

Node *Parser::parse_asm() {
    next_token_skip_space();
    check(Tok::OP_LPAREN);
    next_token_skip_space_or_newline();
    const std::string text = parse_string_without_interpolation("asm");
    skip_space_or_newline();

    bool volatile_ = false, alignstack = false, intel = false, can_throw = false;
    std::vector<Node *> outputs, inputs;
    bool has_outputs = false, has_inputs = false, has_clobbers = false;
    std::vector<std::string> clobbers;

    int part_index = 0;
    while (token_.type != Tok::OP_RPAREN) {
        if (token_.type == Tok::OP_COLON_COLON) {
            next_token_skip_space_or_newline();
            part_index += 2;
        } else if (token_.type == Tok::OP_COLON) {
            next_token_skip_space_or_newline();
            part_index += 1;
        } else {
            unexpected_token();
        }
        bool stop = false;
        switch (part_index) {
            case 1:
                if (token_.type == Tok::DELIMITER_START) {
                    outputs = parse_asm_operands();
                    has_outputs = true;
                }
                break;
            case 2:
                if (token_.type == Tok::DELIMITER_START) {
                    inputs = parse_asm_operands();
                    has_inputs = true;
                }
                break;
            case 3:
                if (token_.type == Tok::DELIMITER_START) {
                    clobbers = parse_asm_clobbers();
                    has_clobbers = true;
                }
                break;
            case 4:
                if (token_.type == Tok::DELIMITER_START) parse_asm_options(volatile_, alignstack, intel, can_throw);
                break;
            default:
                stop = true;
        }
        if (stop) break;
    }

    check(Tok::OP_RPAREN);
    const Location end_location = token_end_location();
    next_token_skip_space();

    Node *a = make(NodeKind::Asm);
    a->value = text;
    if (has_outputs) {
        a->exps = std::move(outputs);
        a->has_exps = true;
    }
    if (has_inputs) {
        a->exps2 = std::move(inputs);
        a->has_exps2 = true;
    }
    if (has_clobbers) {
        a->names = std::move(clobbers);
        a->has_names = true;
    }
    a->volatile_ = volatile_;
    a->alignstack = alignstack;
    a->intel = intel;
    a->can_throw = can_throw;
    return at_end(a, end_location);
}

std::vector<Node *> Parser::parse_asm_operands() {
    std::vector<Node *> operands;
    while (true) {
        operands.push_back(parse_asm_operand());
        if (token_.type == Tok::OP_COMMA) next_token_skip_space_or_newline();
        if (token_.type != Tok::DELIMITER_START) break;
    }
    return operands;
}

Node *Parser::parse_asm_operand() {
    const std::string text = parse_string_without_interpolation("constraint");
    check(Tok::OP_LPAREN);
    next_token_skip_space_or_newline();
    Node *exp = parse_expression();
    check(Tok::OP_RPAREN);
    next_token_skip_space_or_newline();
    Node *o = make(NodeKind::AsmOperand);
    o->value = text;
    o->exp = exp;
    return o;
}

std::vector<std::string> Parser::parse_asm_clobbers() {
    std::vector<std::string> clobbers;
    while (true) {
        clobbers.push_back(parse_string_without_interpolation("asm clobber"));
        skip_space_or_newline();
        if (token_.type == Tok::OP_COMMA) next_token_skip_space_or_newline();
        if (token_.type != Tok::DELIMITER_START) break;
    }
    return clobbers;
}

void Parser::parse_asm_options(bool &volatile_, bool &alignstack, bool &intel, bool &can_throw) {
    while (true) {
        const Location location = token_.location();
        const std::string option = parse_string_without_interpolation("asm option");
        skip_space_or_newline();
        if (option == "volatile") {
            volatile_ = true;
        } else if (option == "alignstack") {
            alignstack = true;
        } else if (option == "intel") {
            intel = true;
        } else if (option == "unwind") {
            can_throw = true;
        } else {
            raise("unknown asm option: " + option, location);
        }
        if (token_.type == Tok::OP_COMMA) next_token_skip_space_or_newline();
        if (token_.type != Tok::DELIMITER_START) break;
    }
}

Node *Parser::parse_yield_with_scope() {
    const Location location = token_.location();
    next_token_skip_space();
    stop_on_yield_++;
    if (block_arity_ < 0) block_arity_ = 1;
    Node *scope = parse_op_assign();
    stop_on_yield_--;
    skip_space();
    check_ident(Keyword::YIELD);
    return parse_yield(scope, location);
}

Node *Parser::parse_yield(Node *scope, std::optional<Location> location_opt) {
    const Location location = location_opt ? *location_opt : token_.location();
    Location end_location = token_end_location();
    next_token();

    std::optional<CallArgs> call_args = preserve_stop_on_do(false, [&] { return parse_call_args(false, false, true); });
    std::vector<Node *> args;
    bool has_args = false;
    if (call_args) {
        args = call_args->args;
        has_args = call_args->has_args;
        end_location = Location();
    }

    if (block_arity_ < 0) block_arity_ = 0;
    if (has_args && static_cast<int>(args.size()) > block_arity_) block_arity_ = static_cast<int>(args.size());

    Node *y = make(NodeKind::Yield);
    y->exps = std::move(args);
    y->scope = scope;
    y->has_parentheses = call_args && call_args->has_parentheses;
    return at_end(at(y, location), end_location);
}

Node *Parser::parse_control_expression(NodeKind klass) {
    const Location end_location = token_end_location();
    next_token();

    std::optional<CallArgs> call_args = preserve_stop_on_do(false, [&] { return parse_call_args(false, true, true); });
    std::vector<Node *> args;
    if (call_args) args = call_args->args;

    if (!args.empty()) {
        if (args.size() == 1 && !args.front()->is(NodeKind::Splat)) return unary(klass, args.front());
        Node *tuple = make(NodeKind::TupleLiteral);
        Node *first = args.front();
        Node *last = args.back();
        tuple->exps = std::move(args);
        at(tuple, first);
        at_end(tuple, last);
        return unary(klass, tuple);
    }
    return at_end(unary(klass, nullptr), end_location);
}

Node *Parser::parse_lib() {
    bool has_doc;
    const std::string doc = doc_or_empty(has_doc);
    const Location location = token_.location();
    next_token_skip_space_or_newline();

    const Location name_location = token_.location();
    Node *name = parse_path();
    check(kStatementEnd);
    skip_statement_end();

    Node *body = push_visibility([&] { return expressions_from(parse_lib_body_expressions()); });

    check_ident(Keyword::END);
    const Location end_location = token_end_location();
    next_token_skip_space();

    Node *lib_def = make(NodeKind::LibDef);
    lib_def->name_node = name;
    lib_def->body = body;
    at_end(at(lib_def, location), end_location);
    lib_def->name_location = name_location;
    lib_def->assign_doc(has_doc, doc);
    return lib_def;
}

Node *Parser::parse_lib_body() {
    next_token_skip_statement_end();
    return expressions_from(parse_lib_body_expressions());
}

std::vector<Node *> Parser::parse_lib_body_expressions() {
    std::vector<Node *> expressions;
    while (true) {
        skip_statement_end();
        if (end_token()) break;
        expressions.push_back(parse_lib_body_exp());
    }
    return expressions;
}

Node *Parser::parse_lib_body_exp() {
    const Location location = token_.location();
    return at(parse_lib_body_exp_without_location(), location);
}

Node *Parser::parse_lib_body_exp_without_location() {
    switch (token_.type) {
        case Tok::OP_AT_LSQUARE: return parse_annotation();
        case Tok::IDENT:
            if (value_is(Keyword::ALIAS)) return parse_alias();
            if (value_is(Keyword::FUN)) return parse_fun_def(false);
            if (value_is(Keyword::TYPE)) return parse_type_def();
            if (value_is(Keyword::STRUCT)) {
                inside_c_struct_ = true;
                Node *node = parse_c_struct_or_union(false);
                inside_c_struct_ = false;
                return node;
            }
            if (value_is(Keyword::UNION)) return parse_c_struct_or_union(true);
            if (value_is(Keyword::ENUM)) return parse_enum_def();
            unexpected_token();
        case Tok::CONST: {
            Node *ident = parse_path(false, token_.location());
            skip_space();
            check(Tok::OP_EQ);
            next_token_skip_space_or_newline();
            Node *value = parse_expression();
            skip_statement_end();
            Node *a = make(NodeKind::Assign);
            a->target = ident;
            a->value_node = value;
            return a;
        }
        case Tok::GLOBAL: {
            bool has_doc;
            const std::string doc = doc_or_empty(has_doc);
            const Location location = token_.location();
            const std::string name = value_s().substr(1);
            next_token_skip_space_or_newline();
            bool has_real_name = false;
            std::string real_name;
            if (token_.type == Tok::OP_EQ) {
                next_token_skip_space();
                check({Tok::IDENT, Tok::CONST});
                real_name = value_s();
                has_real_name = true;
                next_token_skip_space();
            }
            check(Tok::OP_COLON);
            next_token_skip_space_or_newline();
            Node *type = parse_bare_proc_type();
            if (!name.empty() && name[0] >= 'A' && name[0] <= 'Z') {
                // `name.underscore` and `type.to_s`, for the common cases.
                std::string underscore;
                for (size_t i = 0; i < name.size(); i++) {
                    const char c = name[i];
                    if (c >= 'A' && c <= 'Z') {
                        if (i > 0 && name[i - 1] != '_' &&
                            ((name[i - 1] >= 'a' && name[i - 1] <= 'z') || (name[i - 1] >= '0' && name[i - 1] <= '9') ||
                             (i + 1 < name.size() && name[i + 1] >= 'a' && name[i + 1] <= 'z'))) {
                            underscore += '_';
                        }
                        underscore += static_cast<char>(c + 32);
                    } else {
                        underscore += c;
                    }
                }
                std::string type_s;
                if (type->is(NodeKind::Path)) {
                    for (size_t i = 0; i < type->names.size(); i++) type_s += (i ? "::" : "") + type->names[i];
                } else {
                    type_s = "?";
                }
                raise("external variables must start with lowercase, use for example `$" + underscore + " = " + name +
                          " : " + type_s + "`",
                      location);
            }
            skip_statement_end();
            Node *ev = make(NodeKind::ExternalVar);
            ev->name = name;
            ev->restriction = type;
            if (has_real_name) {
                ev->value = real_name;
                ev->has_real_name = true;
            }
            at(ev, location);
            ev->assign_doc(has_doc, doc);
            return ev;
        }
        case Tok::OP_LCURLY_LCURLY: return parse_percent_macro_expression();
        case Tok::OP_LCURLY_PERCENT: return parse_percent_macro_control();
        default: unexpected_token();
    }
}

Node *Parser::parse_fun_def(bool top_level, bool require_body) {
    const Location location = token_.location();
    bool has_doc;
    const std::string doc = doc_or_empty(has_doc);

    return with_isolated_var_scope(require_body, [&]() -> Node * {
        next_token_skip_space_or_newline();
        const Location name_location = token_.location();
        std::string name;
        if (top_level) {
            name = check_ident();
        } else {
            check({Tok::IDENT, Tok::CONST});
            name = value_s();
        }
        next_token_skip_space_or_newline();

        std::string real_name;
        if (token_.type == Tok::OP_EQ) {
            next_token_skip_space_or_newline();
            if (token_.type == Tok::IDENT || token_.type == Tok::CONST) {
                real_name = value_s();
                next_token_skip_space_or_newline();
            } else if (token_.type == Tok::DELIMITER_START) {
                real_name = parse_string_without_interpolation("fun name");
                skip_space();
            } else {
                unexpected_token();
            }
        } else {
            real_name = name;
        }

        std::vector<Node *> params;
        bool varargs = false;
        Location end_location;

        if (token_.type == Tok::OP_LPAREN) {
            next_token_skip_space_or_newline();
            while (token_.type != Tok::OP_RPAREN) {
                if (token_.type == Tok::OP_PERIOD_PERIOD_PERIOD) {
                    varargs = true;
                    next_token_skip_space_or_newline();
                    check(Tok::OP_RPAREN);
                    break;
                }
                if (token_.type == Tok::IDENT) {
                    const std::string param_name = value_s();
                    const Location param_location = token_.location();
                    next_token_skip_space_or_newline();
                    check(Tok::OP_COLON);
                    next_token_skip_space_or_newline();
                    Node *param_type = parse_bare_proc_type();
                    skip_space_or_newline();
                    for (Node *p : params) {
                        if (p->name == param_name) {
                            raise("duplicated fun parameter name: " + param_name, param_location);
                        }
                    }
                    Node *arg = make(NodeKind::Arg);
                    arg->name = param_name;
                    arg->external_name = param_name;
                    arg->restriction = param_type;
                    params.push_back(at(arg, param_location));
                    if (require_body) push_var_name(param_name);
                } else {
                    if (top_level) raise_at_token("top-level fun parameter must have a name", token_);
                    Node *param_type = parse_union_type();
                    Node *arg = make(NodeKind::Arg);
                    arg->restriction = param_type;
                    params.push_back(at(arg, param_type));
                }
                if (token_.type == Tok::OP_COMMA) {
                    next_token_skip_space_or_newline();
                } else {
                    skip_space_or_newline();
                    check(Tok::OP_RPAREN);
                    break;
                }
            }
            end_location = token_end_location();
            next_token_skip_statement_end();
        }

        Node *return_type = nullptr;
        if (token_.type == Tok::OP_COLON) {
            next_token_skip_space_or_newline();
            end_location = token_end_location();
            return_type = parse_bare_proc_type();
        }

        skip_statement_end();

        Node *body = nullptr;
        if (require_body) {
            fun_nest_++;
            if (tok_kw(Keyword::END)) {
                body = nop();
                end_location = token_end_location();
                next_token();
            } else {
                body = parse_expressions();
                auto r = parse_exception_handler(body, true);
                body = r.first;
                end_location = r.second;
            }
            fun_nest_--;
        }

        Node *fun_def = make(NodeKind::FunDef);
        fun_def->name = name;
        fun_def->args = std::move(params);
        fun_def->return_type = return_type;
        fun_def->varargs = varargs;
        fun_def->body = body;
        fun_def->value = real_name;
        fun_def->name_location = name_location;
        fun_def->assign_doc(has_doc, doc);
        return at_end(at(fun_def, location), end_location);
    });
}

Node *Parser::parse_alias() {
    bool has_doc;
    const std::string doc = doc_or_empty(has_doc);
    next_token_skip_space_or_newline();
    Node *name = parse_path();
    skip_space();
    check(Tok::OP_EQ);
    next_token_skip_space_or_newline();
    Node *value = parse_bare_proc_type();
    const Location end_location = EndLoc(value);
    skip_space();
    Node *a = make(NodeKind::Alias);
    a->name_node = name;
    a->value_node = value;
    at_end(a, end_location);
    a->assign_doc(has_doc, doc);
    return a;
}

Node *Parser::parse_pointerof() {
    next_token_skip_space();
    check(Tok::OP_LPAREN);
    next_token_skip_space_or_newline();
    if (tok_kw(Keyword::SELF)) raise("can't take address of self", token_.line_number, token_.column_number);
    Node *exp = parse_op_assign();
    skip_space_or_newline();
    const Location end_location = token_end_location();
    check(Tok::OP_RPAREN);
    next_token_skip_space();
    return at_end(unary(NodeKind::PointerOf, exp), end_location);
}

Node *Parser::parse_sizeof(NodeKind klass) {
    const Location sizeof_location = token_.location();
    next_token_skip_space();
    check(Tok::OP_LPAREN);
    next_token_skip_space_or_newline();
    const Location location = token_.location();
    Node *exp = at(parse_bare_proc_type(), location);
    skip_space_or_newline();
    const Location end_location = token_end_location();
    check(Tok::OP_RPAREN);
    next_token_skip_space();
    return at_end(at(unary(klass, exp), sizeof_location), end_location);
}

Node *Parser::parse_offsetof() {
    const Location offsetof_location = token_.location();
    next_token_skip_space();
    check(Tok::OP_LPAREN);
    next_token_skip_space_or_newline();
    const Location type_location = token_.location();
    Node *type = at(parse_bare_proc_type(), type_location);
    skip_space();
    check(Tok::OP_COMMA);
    next_token_skip_space_or_newline();
    Node *offset;
    if (token_.type == Tok::INSTANCE_VAR) {
        offset = make(NodeKind::InstanceVar);
        offset->name = value_s();
    } else if (token_.type == Tok::NUMBER) {
        if (token_.number_kind != NumberKind::I32) {
            raise_at_token("expecting an integer offset, not '" + token_.to_s() + "'", token_);
        }
        offset = make(NodeKind::NumberLiteral);
        offset->value = value_s();
        offset->number_kind = token_.number_kind;
    } else {
        raise_at_token("expecting an instance variable or a integer offset, not '" + token_.to_s() + "'", token_);
    }
    at(offset, token_.location());
    next_token_skip_space_or_newline();
    const Location end_location = token_end_location();
    check(Tok::OP_RPAREN);
    next_token_skip_space();
    Node *o = make(NodeKind::OffsetOf);
    o->left = type;
    o->right = offset;
    return at_end(at(o, offsetof_location), end_location);
}

Node *Parser::parse_type_def() {
    bool has_doc;
    const std::string doc = doc_or_empty(has_doc);
    next_token_skip_space_or_newline();
    const std::string name = check_const();
    const Location name_location = token_.location();
    next_token_skip_space_or_newline();
    check(Tok::OP_EQ);
    next_token_skip_space_or_newline();
    Node *type = parse_bare_proc_type();
    skip_space();
    Node *t = make(NodeKind::TypeDef);
    t->name = name;
    t->restriction = type;
    t->name_location = name_location;
    t->assign_doc(has_doc, doc);
    return t;
}

Node *Parser::parse_c_struct_or_union(bool is_union) {
    bool has_doc;
    const std::string doc = doc_or_empty(has_doc);
    const Location location = token_.location();
    next_token_skip_space_or_newline();
    const std::string name = check_const();
    next_token();
    check(kStatementEnd);
    skip_statement_end();
    std::vector<Node *> body = parse_c_struct_or_union_body_expressions();
    check_ident(Keyword::END);
    const Location end_location = token_end_location();
    next_token_skip_space();
    Node *c = make(NodeKind::CStructOrUnionDef);
    c->name = name;
    c->body = expressions_from(std::move(body));
    c->is_union = is_union;
    at_end(at(c, location), end_location);
    c->assign_doc(has_doc, doc);
    return c;
}

std::vector<Node *> Parser::parse_c_struct_or_union_body_expressions() {
    std::vector<Node *> exps;
    while (true) {
        if (token_.type == Tok::IDENT) {
            if (value_is(Keyword::INCLUDE)) {
                if (inside_c_struct_) {
                    const Location location = token_.location();
                    exps.push_back(at(parse_include_or_extend(NodeKind::Include), location));
                } else {
                    parse_c_struct_or_union_fields(exps);
                }
            } else if (value_is(Keyword::END)) {
                break;
            } else {
                parse_c_struct_or_union_fields(exps);
            }
        } else if (token_.type == Tok::OP_LCURLY_LCURLY) {
            exps.push_back(parse_percent_macro_expression());
        } else if (token_.type == Tok::OP_LCURLY_PERCENT) {
            exps.push_back(parse_percent_macro_control());
        } else if (token_.type == Tok::NEWLINE || token_.type == Tok::OP_SEMICOLON) {
            skip_statement_end();
        } else {
            break;
        }
    }
    return exps;
}

void Parser::parse_c_struct_or_union_fields(std::vector<Node *> &exps) {
    bool has_doc;
    const std::string doc = doc_or_empty(has_doc);
    std::vector<Node *> vars = {at_end(at(var_node(value_s()), token_.location()), token_end_location())};
    next_token_skip_space_or_newline();
    while (token_.type == Tok::OP_COMMA) {
        next_token_skip_space_or_newline();
        const std::string n = check_ident();
        vars.push_back(at_end(at(var_node(n), token_.location()), token_end_location()));
        next_token_skip_space_or_newline();
    }
    check(Tok::OP_COLON);
    next_token_skip_space_or_newline();
    Node *type = parse_bare_proc_type();
    skip_statement_end();
    for (Node *var : vars) {
        var->assign_doc(has_doc, doc);
        Node *td = make(NodeKind::TypeDeclaration);
        td->var = var;
        td->declared_type = type;
        at(td, var);
        exps.push_back(at_end(td, type));
    }
}

Node *Parser::parse_enum_def() {
    const Location location = token_.location();
    bool has_doc;
    const std::string doc = doc_or_empty(has_doc);
    next_token_skip_space_or_newline();
    Node *name = parse_path();
    skip_space();
    Node *base_type = nullptr;
    if (token_.type == Tok::OP_COLON) {
        next_token_skip_space_or_newline();
        base_type = parse_path();
        skip_space();
    }
    check({Tok::OP_SEMICOLON, Tok::NEWLINE});
    skip_statement_end();
    std::vector<Node *> members = parse_enum_body_expressions();
    check_ident(Keyword::END);
    const Location end_location = token_end_location();
    next_token_skip_space();
    Node *e = make(NodeKind::EnumDef);
    e->name_node = name;
    e->exps = std::move(members);
    e->restriction = base_type;
    e->assign_doc(has_doc, doc);
    return at_end(at(e, location), end_location);
}

std::vector<Node *> Parser::parse_enum_body_expressions() {
    std::vector<Node *> members;
    while (!end_token()) {
        const Location location = token_.location();
        switch (token_.type) {
            case Tok::CONST: {
                const std::string constant_name = value_s();
                const bool member_has_doc = token_.has_doc;
                const std::string member_doc = token_.doc;
                next_token_skip_space();
                Node *constant_value = nullptr;
                if (token_.type == Tok::OP_EQ) {
                    next_token_skip_space_or_newline();
                    constant_value = parse_logical_or();
                }
                skip_space();
                if (token_.type == Tok::NEWLINE || token_.type == Tok::OP_SEMICOLON || token_.type == Tok::EOF_) {
                    next_token_skip_statement_end();
                } else {
                    if (!tok_kw(Keyword::END)) raise("expecting ';', 'end' or newline after enum member", location);
                }
                Node *arg = make(NodeKind::Arg);
                arg->name = constant_name;
                arg->external_name = constant_name;
                arg->default_value = constant_value;
                at(arg, location);
                if (constant_value) {
                    at_end(arg, constant_value);
                } else {
                    at_end(arg, location);
                }
                arg->assign_doc(member_has_doc, member_doc);
                members.push_back(arg);
                break;
            }
            case Tok::IDENT: {
                bool has_vis = false;
                Visibility visibility = Visibility::Public;
                if (value_is(Keyword::PRIVATE)) {
                    has_vis = true;
                    visibility = Visibility::Private;
                    next_token_skip_space();
                } else if (value_is(Keyword::PROTECTED)) {
                    has_vis = true;
                    visibility = Visibility::Protected;
                    next_token_skip_space();
                }
                const Location def_location = token_.location();
                Node *member;
                if (value_is(Keyword::DEF)) {
                    member = at(parse_def(), def_location);
                } else if (value_is(Keyword::MACRO)) {
                    member = at(parse_macro(), def_location);
                } else {
                    unexpected_token();
                }
                if (has_vis) {
                    Node *m = make(NodeKind::VisibilityModifier);
                    m->modifier = visibility;
                    m->exp = member;
                    member = at(m, location);
                }
                members.push_back(member);
                break;
            }
            case Tok::CLASS_VAR: {
                Node *class_var = make(NodeKind::ClassVar);
                class_var->name = value_s();
                at(class_var, location);
                next_token_skip_space();
                check(Tok::OP_EQ);
                next_token_skip_space_or_newline();
                Node *value = parse_op_assign();
                Node *a = make(NodeKind::Assign);
                a->target = class_var;
                a->value_node = value;
                members.push_back(at(a, class_var));
                break;
            }
            case Tok::OP_LCURLY_LCURLY: members.push_back(parse_percent_macro_expression()); break;
            case Tok::OP_LCURLY_PERCENT: members.push_back(at(parse_percent_macro_control(), location)); break;
            case Tok::OP_AT_LSQUARE: members.push_back(parse_annotation()); break;
            case Tok::NEWLINE:
            case Tok::OP_SEMICOLON:
                skip_statement_end();
                break;
            default: unexpected_token();
        }
    }
    return members;
}

Node *Parser::node_and_next_token(Node *node) {
    node->end_location = token_end_location();
    next_token();
    return node;
}

bool Parser::end_token() {
    switch (token_.type) {
        case Tok::OP_RCURLY: case Tok::OP_RSQUARE: case Tok::OP_PERCENT_RCURLY: case Tok::EOF_: return true;
        default: break;
    }
    if (token_.vkind == TokValue::Keyword) {
        switch (token_.kw) {
            case Keyword::DO: case Keyword::END: case Keyword::ELSE: case Keyword::ELSIF: case Keyword::WHEN:
            case Keyword::IN: case Keyword::RESCUE: case Keyword::ENSURE: case Keyword::THEN:
                return !next_comes_colon_space();
            default: return false;
        }
    }
    return false;
}

bool Parser::can_be_assigned(Node *node) {
    switch (node->kind) {
        case NodeKind::Var: case NodeKind::InstanceVar: case NodeKind::ClassVar: case NodeKind::Path:
        case NodeKind::Global: case NodeKind::Underscore:
            return true;
        case NodeKind::Call: {
            if (node->has_parentheses) return false;
            const bool no_args = node->args.empty() && !node->has_named_args && !node->block;
            if (Lexer::is_ident(node->name) && no_args) return true;
            return node->name == "[]" && (node->args_in_brackets || no_args);
        }
        default: return false;
    }
}

std::string Parser::consume_def_or_macro_name() {
    wants_def_or_macro_name_begin();
    try {
        next_token_skip_space_or_newline();
        check(kDefOrMacroCheck1);
    } catch (...) {
        wants_def_or_macro_name_end();
        throw;
    }
    wants_def_or_macro_name_end();
    return token_.to_s();
}

std::pair<bool, Location> Parser::consume_def_equals_sign_skip_space() {
    auto r = consume_def_equals_sign();
    skip_space();
    return r;
}

std::pair<bool, Location> Parser::consume_def_equals_sign() {
    Location end_location = token_end_location();
    next_token();
    if (token_.type == Tok::OP_EQ) {
        end_location = token_end_location();
        next_token();
        return {true, end_location};
    }
    return {false, end_location};
}

void Parser::push_var(Node *var) {
    switch (var->kind) {
        case NodeKind::Var:
        case NodeKind::Arg:
            push_var_name(var->name);
            break;
        case NodeKind::TypeDeclaration: {
            Node *v = var->var;
            if (v->is(NodeKind::Var) || v->is(NodeKind::InstanceVar)) {
                push_var_name(v->name);
            } else {
                raise("can't happen");
            }
            break;
        }
        default: break;
    }
}

void Parser::check_void_value(Node *exp, Location location) {
    if (exp->is_control_expression()) raise("void value expression", location);
}

void Parser::check_void_expression_keyword() {
    if (value_is(Keyword::BREAK) || value_is(Keyword::NEXT) || value_is(Keyword::RETURN)) {
        if (!next_comes_colon_space()) {
            raise_at_token("void value expression", token_, static_cast<int>(Utf8Length(value_s())));
        }
    }
}

void Parser::check(std::initializer_list<Tok> types) {
    for (Tok t : types) {
        if (token_.type == t) return;
    }
    std::string list;
    bool first = true;
    for (Tok t : types) {
        if (!first) list += ", ";
        list += TokName(t);
        first = false;
    }
    raise_at_token("expecting any of these tokens: " + list + " (not '" + token_.to_s() + "')", token_);
}

void Parser::check(Tok type) {
    if (type != token_.type) {
        raise_at_token(std::string("expecting token '") + TokName(type) + "', not '" + token_.to_s() + "'", token_);
    }
}

void Parser::check_ident(Keyword k) {
    if (!tok_kw(k)) {
        raise_at_token(std::string("expecting identifier '") + KeywordName(k) + "', not '" + token_.to_s() + "'", token_);
    }
}

std::string Parser::check_ident() {
    check(Tok::IDENT);
    return value_s();
}

std::string Parser::check_const() {
    check(Tok::CONST);
    return value_s();
}

void Parser::unexpected_token(const std::string &msg, const Token *token) {
    const Token &t = token ? *token : token_;
    const std::string token_str = t.type == Tok::EOF_ ? "EOF" : StringInspect(t.to_s());
    if (!msg.empty()) raise_at_token("unexpected token: " + token_str + " (" + msg + ")", token_);
    raise_at_token("unexpected token: " + token_str, token_);
}

void Parser::unexpected_token_in_atomic() {
    if (!unclosed_stack_.empty()) {
        const Unclosed &u = unclosed_stack_.back();
        raise("unterminated " + u.name, u.location);
    }
    unexpected_token();
}

bool Parser::is_var(const std::string &name) {
    if (in_macro_expression_) return true;
    return name == "self" || var_in_scope(name);
}

std::string Parser::temp_arg_name() { return "__arg" + std::to_string(temp_arg_count_++); }

}  // namespace crystal
