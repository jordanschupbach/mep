// mep's Crystal formatter: the visitors for calls and their arguments,
// blocks, operators, assignments, type definitions, control expressions,
// case/select, exception handlers, procs and asm -- formatter.cr's `visit`
// methods from Call to AsmOperand, in its order. See
// crystal_format_internal.h.

#include <algorithm>

#include "crystal_format_internal.h"

namespace crfmt {

namespace {

uint64_t Id(const Node *n) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(n)); }

bool In(const std::string &s, std::initializer_list<const char *> names) {
    for (const char *n : names)
        if (s == n) return true;
    return false;
}

Node *Last(const std::vector<Node *> &v) {
    if (v.empty()) throw FormatBug("Index out of bounds");
    return v.back();
}

}  // namespace

// --- calls ---------------------------------------------------------------------------------

bool Formatter::visit_call(Node *node) {
    // This is the case of `...`
    if (node->name == "`") {
        accept(node->args.at(0));
        return false;
    }

    const bool special_call = In(node->name, {"as", "as?", "is_a?", "nil?", "responds_to?"});

    Node *obj = node->obj;

    // Consider the case of `&.as(...)` and similar
    if (obj && obj->is(NodeKind::Nop)) obj = nullptr;

    // Consider the case of `as T`, that is, casting `self` without an explicit `self`
    if (special_call && obj && obj->is(NodeKind::Var) && obj->name == "self" && !tok_->keyword(Keyword::SELF))
        obj = nullptr;

    const int column = column_;
    // The indent for arguments and block belonging to this node.
    int base_indent = indent_;

    // Special case: $1, $2, ...
    if (tok_->type == Tok::GLOBAL_MATCH_DATA_INDEX && In(node->name, {"[]", "[]?"}) && obj &&
        obj->is(NodeKind::Global)) {
        write("$");
        write(tok_->value_s());
        next_token();
        return false;
    }

    if (node->global) write_token(Tok::OP_COLON_COLON);

    if (obj) {
        // This handles unary operators written in prefix notation.
        // The relevant distinction is that the call has a receiver and the
        // current token is not that object but a unary operator.
        if (crystal::IsUnaryOperator(tok_->type) && node->name == crystal::TokName(tok_->type) &&
            !node->has_any_args()) {
            write(tok_->type);
            next_token_skip_space_or_newline();
            accept(obj);
            return false;
        }

        accept(obj);

        bool passed_backslash_newline = tok_->passed_backslash_newline;
        const bool needs_space = tok_->type == Tok::SPACE || !In(node->name, {"*", "/", "**", "//"});

        slash_is_not_regex();
        skip_space();

        // It's something like `foo.bar\n
        //                        .baz`
        if (tok_->type == Tok::NEWLINE || wrote_newline_) {
            base_indent = indent_ + 2;
            indent_at(base_indent, [&] { consume_newlines(); });
            write_indent(base_indent);
        }

        if (tok_->type != Tok::OP_PERIOD) {
            // It's an operator
            if (tok_->type == Tok::OP_LSQUARE) {
                write("[");
                next_token_skip_space();

                std::vector<Node *> &args = node->args;

                Node *last_arg = nullptr;
                if (node->name == "[]=") {
                    last_arg = Last(args);
                    args.pop_back();
                }

                auto [has_newlines, found_comment, unused] =
                    format_args(args, true, node->has_named_args ? &node->named_args : nullptr, nullptr, indent_ + 2);
                (void)unused;
                if (tok_->type == Tok::OP_COMMA || tok_->type == Tok::NEWLINE) {
                    if (has_newlines) {
                        write(",");
                        found_comment = next_token_skip_space_b();
                        if (!found_comment) write_line();
                        write_indent();
                        skip_space_or_newline();
                    } else {
                        next_token_skip_space_or_newline();
                    }
                } else {
                    found_comment = skip_space_or_newline();
                    if (found_comment) write_indent();
                }

                // foo[&.bar]
                if (node->block && tok_->type == Tok::OP_AMP) {
                    const bool has_args = !node->args.empty() || node->has_named_args;
                    if (has_args) write(",");
                    format_block(node->block, has_args);
                }

                write_token(Tok::OP_RSQUARE);

                if (node->name == "[]?") {
                    skip_space();

                    // This might not be present in the case of `x[y] ||= z`
                    if (tok_->type == Tok::OP_QUESTION) {
                        write("?");
                        next_token();
                    }
                }

                if (last_arg) {
                    skip_space_or_newline();

                    write(" =");
                    next_token_skip_space();
                    accept_assign_value_after_equals(last_arg);
                }

                return false;
            } else if (tok_->type == Tok::OP_LSQUARE_RSQUARE) {
                write("[]");
                next_token();

                if (node->name == "[]=") {
                    skip_space_or_newline();
                    write_token(" ", Tok::OP_EQ, " ");
                    skip_space_or_newline();
                    inside_call_or_assign([&] { accept(Last(node->args)); });
                }

                return false;
            }

            if (needs_space && !passed_backslash_newline) write(" ");
            write(node->name);

            // This is the case of a-1 and a+1
            if (tok_->type == Tok::NUMBER) lexer_.set_current_pos(tok_->start + 1);

            slash_is_regex();

            next_token();
            passed_backslash_newline = tok_->passed_backslash_newline;
            const bool found_comment = skip_space();

            if (found_comment || tok_->type == Tok::NEWLINE) {
                int next_indent;
                if (inside_call_or_assign_ == 0) {
                    next_indent = indent_ + 2;
                } else {
                    next_indent = column == 0 ? 2 : column;
                }
                indent_at(next_indent, [&] {
                    skip_space_write_line();
                    skip_space_or_newline();
                });
                write_indent(next_indent, Last(node->args));
            } else {
                if (needs_space && !passed_backslash_newline) write(" ");
                inside_call_or_assign([&] { accept(Last(node->args)); });
            }

            return false;
        }

        lexer_.wants_def_or_macro_name_ = true;
        next_token();
        lexer_.wants_def_or_macro_name_ = false;
        skip_space();

        if (tok_->type == Tok::NEWLINE || wrote_newline_) {
            base_indent = indent_ + 2;
            indent_at(base_indent, [&] { consume_newlines(); });
            write_indent(base_indent);
        }

        write(".");

        skip_space_or_newline();
    }

    // This is for foo &.[bar] and &.[bar]?, or foo.[bar] and foo.[bar]?
    if (In(node->name, {"[]", "[]?"}) && tok_->type == Tok::OP_LSQUARE) {
        write("[");
        next_token_skip_space_or_newline();
        format_call_args(node, false, base_indent);
        skip_space_or_newline();
        write_token(Tok::OP_RSQUARE);
        if (node->name == "[]?") write_token(Tok::OP_QUESTION);
        return false;
    }

    // This is for foo.[bar] = baz
    if (node->name == "[]=" && tok_->type == Tok::OP_LSQUARE) {
        write("[");
        next_token_skip_space_or_newline();
        Node *last_arg = Last(node->args);
        node->args.pop_back();
        format_call_args(node, true, base_indent);
        skip_space_or_newline();
        write_token(Tok::OP_RSQUARE);
        skip_space_or_newline();
        write(" =");
        next_token_skip_space();
        accept_assign_value_after_equals(last_arg);
        return false;
    }

    // This is for foo.[] = bar
    if (node->name == "[]=" && tok_->type == Tok::OP_LSQUARE_RSQUARE) {
        write_token(Tok::OP_LSQUARE_RSQUARE);
        next_token_skip_space_or_newline();
        write(" =");
        next_token_skip_space();
        accept_assign_value_after_equals(Last(node->args));
        return false;
    }

    const bool assignment = crystal::Lexer::is_setter(node->name);

    if (assignment) {
        write(std::string_view(node->name).substr(0, node->name.size() - 1));
    } else {
        write(node->name);
    }
    next_token();

    const bool passed_backslash_newline = tok_->passed_backslash_newline;

    if (assignment) {
        skip_space();

        next_token();
        if (tok_->type == Tok::OP_LPAREN) {
            write("=(");
            slash_is_regex();
            next_token();
            format_call_args(node, true, base_indent);
            skip_space_or_newline();
            write_token(Tok::OP_RPAREN);
        } else {
            write(" =");
            skip_space();
            accept_assign_value_after_equals(Last(node->args));
        }

        return false;
    }

    bool has_parentheses = false;
    bool ends_with_newline = false;
    const bool has_args = node->has_any_args();

    bool has_newlines = false;
    bool found_comment = false;

    // For special calls we want to format `.as (Int32)` into `.as(Int32)`
    // so we remove the space between "as" and "(".
    if (special_call) skip_space();

    // If the call has a single argument which is a parenthesized `Expressions`,
    // we skip whitespace between the method name and the arg. The parenthesized
    // arg is transformed into a call with parenthesis: `foo (a)` becomes `foo(a)`.
    if (node->args.size() == 1 && tok_->type == Tok::SPACE && !node->has_named_args && !node->block_arg &&
        !node->block && node->args[0]->is(NodeKind::Expressions) &&
        node->args[0]->keyword == crystal::ExpressionsKeyword::Paren && node->args[0]->exps.size() == 1) {
        // ...except do not transform `foo ()` into `foo()`, as the former is
        // actually semantically equivalent to `foo(nil)`
        Node *arg = node->args[0]->exps[0];
        if (!arg->is(NodeKind::Nop)) {
            skip_space();
            node->args[0] = arg;
        }
    }

    if (tok_->type == Tok::OP_LPAREN) {
        slash_is_regex();
        next_token();

        // If it's something like `foo.bar()` we rewrite it as `foo.bar`
        // (parentheses are not needed). Also applies for special calls
        // like `nil?` when there might not be a receiver.
        if ((obj || special_call) && !has_args && !node->block_arg && !node->block) {
            skip_space_or_newline();
            check(Tok::OP_RPAREN);
            next_token();
            return false;
        }

        write("(");
        has_parentheses = true;
        std::tie(has_newlines, found_comment, std::ignore) = format_call_args(node, true, base_indent);
        if (!found_comment) found_comment = skip_space();
        if (tok_->type == Tok::NEWLINE) ends_with_newline = true;
        indent_at(base_indent + 2, [&] { skip_space_or_newline(true, ends_with_newline); });
    } else if (has_args || node->block_arg) {
        if (!passed_backslash_newline) write(" ");
        skip_space();
        std::tie(has_newlines, found_comment, std::ignore) = format_call_args(node, false, base_indent);
    }

    if (Node *block = node->block) {
        bool needs_space = !has_parentheses || has_args;
        int block_indent = base_indent;
        skip_space();
        if (has_parentheses) {
            if (tok_->type == Tok::OP_COMMA) {
                next_token();
                const bool wrote_newline = skip_space_at(block_indent, true);
                if (wrote_newline || tok_->type == Tok::NEWLINE) {
                    if (!wrote_newline) {
                        next_token_skip_space_or_newline();
                        write(",");
                        write_line();
                    }
                    needs_space = false;
                    if (tok_->type != Tok::OP_RPAREN) block_indent += 2;  // foo(1, ↵  &.foo) case
                    write_indent(block_indent);
                } else {
                    if (tok_->type != Tok::OP_RPAREN) write(",");  // foo(1, &.foo) case
                }
            }
            if (tok_->type == Tok::OP_RPAREN) {
                if (ends_with_newline) {
                    if (!(found_comment || wrote_newline_)) write_line();
                    write_indent();
                }
                write(")");
                next_token_skip_space_or_newline();

                indent_at(block_indent, [&] { format_block(block, needs_space); });
                return false;
            }
            indent_at(block_indent, [&] { format_block(block, needs_space); });

            skip_space();
            if (tok_->type == Tok::NEWLINE) ends_with_newline = true;
            skip_space_or_newline();

            finish_args(has_parentheses, has_newlines, ends_with_newline, found_comment, base_indent);
            return false;
        }
        indent_at(block_indent, [&] { format_block(block, needs_space); });

        finish_args(has_parentheses, has_newlines, ends_with_newline, found_comment, base_indent);
        return false;
    }

    finish_args(has_parentheses, has_newlines, ends_with_newline, found_comment, base_indent);
    return false;
}

std::tuple<bool, bool, int> Formatter::format_call_args(Node *node, bool has_parentheses, int base_indent) {
    return indent_at(base_indent, [&] {
        return format_args(node->args, has_parentheses, node->has_named_args ? &node->named_args : nullptr,
                           node->block_arg, indent_ + 2);
    });
}

std::tuple<bool, bool, int> Formatter::format_args(const std::vector<Node *> &args, bool has_parentheses,
                                                   const std::vector<Node *> *named_args, Node *block_arg,
                                                   int needed_indent, bool do_consume_newlines) {
    (void)has_parentheses;
    bool has_newlines = false;
    bool found_comment = false;
    inside_call_or_assign_++;

    if (!args.empty()) {
        std::tie(has_newlines, found_comment, needed_indent) =
            format_args_simple(args, needed_indent, do_consume_newlines);
    }

    if (named_args) {
        bool named_args_found_comment;
        std::tie(has_newlines, named_args_found_comment, needed_indent) =
            format_named_args(args, *named_args, needed_indent);
        if (args.empty() && named_args_found_comment) found_comment = true;
    }

    if (block_arg) has_newlines = format_block_arg(block_arg, needed_indent);

    inside_call_or_assign_--;

    return {has_newlines, found_comment, needed_indent};
}

std::tuple<bool, bool, int> Formatter::format_args_simple(const std::vector<Node *> &args, int needed_indent,
                                                          bool do_consume_newlines) {
    bool has_newlines = false;
    bool found_comment = false;
    bool next_needs_indent = false;

    if (tok_->type == Tok::NEWLINE) {
        if (do_consume_newlines) {
            indent_at(needed_indent, [&] { consume_newlines(); });
            skip_space_or_newline();
        } else {
            write_line();
            indent_at(needed_indent, [&] { next_token_skip_space_or_newline(); });
        }
        next_needs_indent = true;
        has_newlines = true;
    }

    skip_space_or_newline();
    for (size_t i = 0; i < args.size(); i++) {
        Node *arg = args[i];
        if (next_needs_indent) {
            write_indent(needed_indent, arg);
        } else {
            indent_node(indent_, arg);
        }
        next_needs_indent = false;
        if (!last_p(i, args)) {
            if (last_is_heredoc_ && tok_->type == Tok::NEWLINE) {
                skip_space_or_newline();
                write_indent();
            } else {
                skip_space();
            }
            slash_is_regex();
            write_token(Tok::OP_COMMA);

            if (tok_->passed_backslash_newline) {
                write_line();
                next_needs_indent = true;
                has_newlines = true;
            } else {
                found_comment = skip_space_at(needed_indent);
                if (found_comment) {
                    write_indent(needed_indent);
                } else if (tok_->type == Tok::NEWLINE) {
                    indent_at(needed_indent, [&] { consume_newlines(); });
                    next_needs_indent = true;
                    has_newlines = true;
                } else {
                    write(" ");
                }
            }
            skip_space_or_newline();
        }
    }

    return {has_newlines, found_comment, needed_indent};
}

std::tuple<bool, bool, int> Formatter::format_named_args(const std::vector<Node *> &args,
                                                         const std::vector<Node *> &named_args, int needed_indent) {
    skip_space_at(needed_indent);

    const int named_args_column = needed_indent;

    if (!args.empty()) {
        write_token(Tok::OP_COMMA);
        const bool found_comment = skip_space_at(needed_indent);
        if (found_comment || tok_->type == Tok::NEWLINE) {
            if (!last_is_heredoc_) write_indent(needed_indent);
        } else {
            write(" ");
        }
    }

    return format_args(named_args, false, nullptr, nullptr, named_args_column, true);
}

bool Formatter::format_block_arg(Node *block_arg, int needed_indent) {
    bool has_newlines = false;
    skip_space_or_newline();
    if (tok_->type == Tok::OP_COMMA) {
        write(",");
        next_token_skip_space();
        if (tok_->type == Tok::NEWLINE) {
            write_line();
            write_indent(needed_indent);
            has_newlines = true;
        } else {
            write(" ");
        }
    }
    skip_space_or_newline();
    write_token(Tok::OP_AMP);
    skip_space_or_newline();
    accept(block_arg);
    return has_newlines;
}

void Formatter::finish_args(bool has_parentheses, bool has_newlines, bool ends_with_newline, bool found_comment,
                            int column) {
    if (!has_parentheses) return;
    if (tok_->type == Tok::OP_COMMA) {
        next_token();
        found_comment = skip_space_at(column + 2, true) || found_comment;
        if (tok_->type == Tok::NEWLINE && has_newlines) {
            write(",");
            write_line();
            skip_space_or_newline_at(column + 2);
            write_indent(column);
            skip_space_or_newline_at(column);
        } else {
            found_comment = skip_space_or_newline_at(column + 2) || found_comment;
            if (has_newlines) {
                if (!found_comment) {
                    write(",");
                    write_line();
                }
                write_indent(column);
            }
        }
    } else if (found_comment) {
        write_indent(column);
    }
    check(Tok::OP_RPAREN);

    if (ends_with_newline) {
        if (!wrote_newline_) write_line();
        write_indent(column);
    }
    write(")");
    next_token();
}

void Formatter::format_parenthesized_args(const std::vector<Node *> &args, const std::vector<Node *> *named_args) {
    write("(");
    next_token_skip_space();
    auto [has_newlines, found_comment, unused] = format_args(args, true, named_args, nullptr, indent_ + 2);
    (void)unused;
    skip_space();
    bool ends_with_newline = false;
    if (tok_->type == Tok::NEWLINE) {
        ends_with_newline = true;
        next_token();
    }
    skip_space_or_newline();
    finish_args(true, has_newlines, ends_with_newline, found_comment, indent_);
}

bool Formatter::visit_named_argument(Node *node) {
    format_named_argument_name(node->name);
    skip_space_or_newline();
    write_token(Tok::OP_COLON, " ");

    slash_is_regex();

    skip_space_or_newline();
    accept(node->value_node);

    return false;
}

// --- blocks --------------------------------------------------------------------------------

void Formatter::format_block(Node *node, bool needs_space) {
    bool needs_comma = false;
    const int old_inside_call_or_assign = inside_call_or_assign_;
    inside_call_or_assign_ = 0;

    bool comma_before_comment = false;

    if (tok_->type == Tok::OP_COMMA) {
        next_token();
        if (tok_->type == Tok::SPACE) next_token();
        if (tok_->type == Tok::COMMENT) {
            write(",");
            needs_comma = false;
            comma_before_comment = true;
            indent_ += 2;
        } else {
            needs_comma = true;
        }
        skip_space_or_newline();
        if (comma_before_comment) indent_ -= 2;
    }

    if (tok_->keyword(Keyword::DO)) {
        if (comma_before_comment) {
            indent_ += 2;
            write_indent();
        } else {
            write(" ");
        }
        write("do");
        next_token();
        skip_space_at(indent_ + 2);
        Node *body = format_block_args(node->exps, node);
        const int old_implicit_exception_handler_indent = implicit_exception_handler_indent_;
        implicit_exception_handler_indent_ = indent_;
        format_nested_with_end(body);
        implicit_exception_handler_indent_ = old_implicit_exception_handler_indent;
        indent_ -= 2;
    } else if (tok_->type == Tok::OP_LCURLY) {
        if (needs_comma) write(",");
        write(" {");
        next_token_skip_space();
        Node *body = format_block_args(node->exps, node);
        if (tok_->type == Tok::OP_SEMICOLON) next_token_skip_space_or_newline();
        if (tok_->type == Tok::NEWLINE) {
            format_nested(body);
            skip_space_or_newline();
            write_indent();
        } else {
            if (!body->is(NodeKind::Nop)) {
                write(" ");
                accept(body);
            }
            skip_space_or_newline();
            write(" ");
        }
        write_token(Tok::OP_RCURLY);
    } else {
        // It's foo &.bar
        if (needs_comma) write(",");
        if (needs_space) write(" ");
        write_token(Tok::OP_AMP);
        skip_space_or_newline();
        write(".");
        lexer_.wants_def_or_macro_name_ = true;
        next_token_skip_space_or_newline();
        lexer_.wants_def_or_macro_name_ = false;

        Node *body = node->body;
        switch (body->kind) {
            case NodeKind::Call:
                clear_object(body);
                indent_node(indent_, body);
                break;
            case NodeKind::IsA:
                if (body->obj->is(NodeKind::Var)) {
                    Node *call = body->nil_check ? new_call(nullptr, "nil?") : new_call(nullptr, "is_a?", {body->right});
                    accept(call);
                } else {
                    clear_object(body);
                    accept(body);
                }
                break;
            case NodeKind::RespondsTo:
                if (body->obj->is(NodeKind::Var)) {
                    Node *sym = make(NodeKind::SymbolLiteral);
                    sym->value = body->name;
                    accept(new_call(nullptr, "responds_to?", {sym}));
                } else {
                    clear_object(body);
                    accept(body);
                }
                break;
            case NodeKind::Cast:
                if (body->obj->is(NodeKind::Var)) {
                    accept(new_call(nullptr, "as", {body->right}));
                } else {
                    clear_object(body);
                    accept(body);
                }
                break;
            case NodeKind::NilableCast:
                if (body->obj->is(NodeKind::Var)) {
                    accept(new_call(nullptr, "as?", {body->right}));
                } else {
                    clear_object(body);
                    accept(body);
                }
                break;
            case NodeKind::ReadInstanceVar:
                if (body->obj->is(NodeKind::Var)) {
                    accept(new_call(nullptr, body->name));
                } else {
                    clear_object(body);
                    accept(body);
                }
                break;
            case NodeKind::Not:
                if (body->exp->is(NodeKind::Var)) {
                    accept(new_call(nullptr, "!"));
                } else {
                    clear_object(body);
                    accept(body);
                }
                break;
            default:
                throw FormatBug(std::string("BUG: unexpected node for &. argument, not ") +
                                crystal::NodeKindName(body->kind));
        }
    }

    inside_call_or_assign_ = old_inside_call_or_assign;
}

void Formatter::clear_object(Node *node) {
    if (!node) return;
    switch (node->kind) {
        case NodeKind::Call:
            if (node->obj && node->obj->is(NodeKind::Var)) {
                node->obj = nullptr;
            } else {
                clear_object(node->obj);
            }
            break;
        case NodeKind::IsA:
        case NodeKind::RespondsTo:
        case NodeKind::Cast:
        case NodeKind::NilableCast:
            if (node->obj && node->obj->is(NodeKind::Var)) {
                node->obj = nop();
            } else {
                clear_object(node->obj);
            }
            break;
        case NodeKind::Not:
            if (node->exp && node->exp->is(NodeKind::Var)) {
                node->exp = nop();
            } else {
                clear_object(node->exp);
            }
            break;
        default:
            break;
    }
}

Node *Formatter::format_block_args(const std::vector<Node *> &args, Node *node) {
    if (args.empty()) return node->body;

    write_token(" ", Tok::OP_BAR);
    skip_space_or_newline();
    for (size_t i = 0; i < args.size(); i++) {
        format_block_arg();

        if (tok_->type == Tok::OP_COMMA) {
            next_token_skip_space_or_newline();
            if (!last_p(i, args)) write(", ");
        }
    }
    skip_space_or_newline();
    write_token(Tok::OP_BAR);
    skip_space();

    return node->body;
}

void Formatter::format_block_arg() {
    if (tok_->type == Tok::OP_STAR) write_token(Tok::OP_STAR);

    if (tok_->type == Tok::OP_LPAREN) {
        write("(");
        next_token_skip_space_or_newline();

        while (true) {
            format_block_arg();
            if (tok_->type == Tok::OP_COMMA) next_token_skip_space_or_newline();

            if (tok_->type == Tok::OP_RPAREN) {
                next_token();
                write(")");
                break;
            }
            write(", ");
        }
    } else if (tok_->type == Tok::IDENT) {
        write(tok_->value_s());
        next_token();
    } else if (tok_->type == Tok::UNDERSCORE) {
        write("_");
        next_token();
    } else {
        throw FormatBug(std::string("BUG: unexpected token ") + crystal::TokName(tok_->type));
    }

    skip_space_or_newline();
}

Node *Formatter::remove_to_skip(Node *node, int to_skip) {
    if (to_skip <= 0) return node->body;
    Node *body = node->body;
    if (body->is(NodeKind::ExceptionHandler) && body->implicit) {
        Node *sub_body = remove_to_skip(body, to_skip);
        body->body = sub_body;
        return body;
    }

    if (body->is(NodeKind::Expressions)) {
        if (static_cast<size_t>(to_skip) > body->exps.size()) throw FormatBug("Index out of bounds");
        body->exps.erase(body->exps.begin(), body->exps.begin() + to_skip);
        switch (body->exps.size()) {
            case 0:
                return nop();
            case 1:
                return body->exps.front();
            default:
                return body;
        }
    }
    return nop();
}

// --- operators and assignments -------------------------------------------------------------

bool Formatter::format_binary(Node *node, Tok token, Tok alternative) {
    const int column = column_;

    accept(node->left);
    skip_space_or_newline();

    // This is the case of `left ||= right`
    if (tok_->type == alternative) {
        write(" ");
        write(alternative);
        write(" ");
        next_token_skip_space();
        Node *right = node->right;
        if (right->is(NodeKind::Assign)) {
            accept_assign_value(right->value_node);
        } else if (right->is(NodeKind::Call)) {
            accept_assign_value(Last(right->args));
        } else {
            throw FormatBug("BUG: expected Assign or Call after op assign");
        }
        return false;
    }

    write_token(" ", token);
    const bool found_comment = skip_space();
    if (found_comment || tok_->type == Tok::NEWLINE) {
        int next_indent;
        if (inside_call_or_assign_ == 0) {
            next_indent = inside_cond_ == 0 ? indent_ + 2 : indent_;
        } else {
            next_indent = column == 0 ? 2 : column;
        }
        indent_at(next_indent, [&] {
            skip_space_write_line();
            skip_space_or_newline();
        });
        write_indent(next_indent, node->right);
        return false;
    }

    skip_space_or_newline();
    write(" ");
    accept(node->right);

    return false;
}

bool Formatter::visit_not(Node *node) {
    if (tok_->type == Tok::OP_BANG) {
        write_token(Tok::OP_BANG);
        skip_space_or_newline();
        accept(node->exp);
    } else {
        // Can be `exp.!`
        accept(node->exp);
        skip_space_or_newline();
        write_token(Tok::OP_PERIOD);
        skip_space_or_newline();
        write_token(Tok::OP_BANG);
    }
    return false;
}

bool Formatter::visit_assign(Node *node) {
    Node *target = node->target;
    if (target->is(NodeKind::Var)) vars.back().insert(target->name);

    accept(target);
    skip_space_or_newline();

    const bool check_align = check_assign_length(node->target);
    slash_is_regex();
    write_token(" ", Tok::OP_EQ);
    skip_space(false, false);
    accept_assign_value_after_equals(node->value_node, check_align);

    return false;
}

bool Formatter::visit_op_assign(Node *node) {
    accept(node->target);
    skip_space_or_newline();

    slash_is_regex();
    write(" ");
    write(node->value);
    write("=");
    next_token_skip_space();
    accept_assign_value_after_equals(node->value_node);

    return false;
}

void Formatter::accept_assign_value_after_equals(Node *value, bool check_align) {
    if (tok_->type == Tok::NEWLINE) {
        next_token_skip_space_or_newline();
        write_line();
        write_indent(indent_ + 2, value);
    } else {
        write(" ");
        accept_assign_value(value, check_align);
    }
}

void Formatter::accept_assign_value(Node *value, bool check_align) {
    const int before_column = column_;
    if (tok_->keyword(Keyword::IF) || tok_->keyword(Keyword::CASE) || value->is(NodeKind::MacroIf)) {
        indent_node(column_, value);
    } else {
        inside_call_or_assign([&] { accept(value); });
    }
    if (check_align) check_assign_align(before_column, value);
}

bool Formatter::check_assign_length(Node *exp) {
    if (assign_length_) {
        const int target_length = assign_length(exp);
        const int gap = *assign_length_ - target_length;
        for (int i = 0; i < gap; i++) write(" ");
        assign_length_.reset();
        return true;
    }
    return false;
}

void Formatter::check_assign_align(int before_column, Node *exp) {
    if (exp->is(NodeKind::NumberLiteral)) assign_infos_.push_back(AlignInfo{0, line_, before_column, column_, column_, true});
}

// --- type definitions ----------------------------------------------------------------------

bool Formatter::visit_visibility_modifier(Node *node) {
    switch (node->modifier) {
        case crystal::Visibility::Private:
            write_keyword(Keyword::PRIVATE, " ");
            break;
        case crystal::Visibility::Protected:
            write_keyword(Keyword::PROTECTED, " ");
            break;
        case crystal::Visibility::Public:
            // no keyword needed
            break;
    }
    accept(node->exp);
    return false;
}

bool Formatter::visit_module_def(Node *node) {
    write_keyword(Keyword::MODULE, " ");

    accept(node->name_node);
    format_type_vars(node);

    format_nested_with_end(node->body);
    return false;
}

bool Formatter::visit_annotation_def(Node *node) {
    write_keyword(Keyword::ANNOTATION, " ");

    accept(node->name_node);

    skip_space_at(indent_ + 2);

    if (tok_->type == Tok::OP_SEMICOLON) {
        skip_semicolon_or_space_or_newline();
        check_end();
        write("; end");
    } else {
        skip_space_or_newline();
        check_end();
        write_line();
        write_indent();
        write("end");
    }

    next_token();
    return false;
}

bool Formatter::visit_class_def(Node *node) {
    if (node->is_abstract) write_keyword(Keyword::ABSTRACT, " ");
    write_keyword(node->is_struct ? Keyword::STRUCT : Keyword::CLASS, " ");

    accept(node->name_node);
    format_type_vars(node);

    if (node->superclass) {
        skip_space_or_newline();
        write_token(" ", Tok::OP_LT, " ");
        skip_space_or_newline();
        accept(node->superclass);
    }

    format_nested_with_end(node->body);
    return false;
}

void Formatter::format_type_vars(Node *node) {
    if (!node->has_names) return;
    const std::vector<std::string> &type_vars = node->names;
    skip_space();
    write_token(Tok::OP_LPAREN);
    skip_space_or_newline();
    for (size_t i = 0; i < type_vars.size(); i++) {
        if (static_cast<int>(i) == node->splat_index) write_token(Tok::OP_STAR);
        write(type_vars[i]);
        next_token_skip_space_or_newline();
        if (tok_->type == Tok::OP_COMMA) {
            if (!last_p(i, type_vars)) write(", ");
            next_token_skip_space_or_newline();
        }
    }
    write_token(Tok::OP_RPAREN);
    skip_space();
}

bool Formatter::visit_c_struct_or_union_def(Node *node) {
    write_keyword(node->is_union ? Keyword::UNION : Keyword::STRUCT, " ");

    write(node->name);
    next_token();

    inside_struct_or_union++;
    format_nested_with_end(node->body);
    inside_struct_or_union--;

    return false;
}

bool Formatter::visit_lib_def(Node *node) {
    inside_lib++;

    write_keyword(Keyword::LIB, " ");

    accept(node->name_node);

    format_nested_with_end(node->body);

    inside_lib--;
    return false;
}

bool Formatter::visit_enum_def(Node *node) {
    write_keyword(Keyword::ENUM, " ");
    accept(node->name_node);

    if (Node *base_type = node->restriction) {
        skip_space();
        write_token(" ", Tok::OP_COLON, " ");
        skip_space_or_newline();
        accept(base_type);
    }

    inside_enum++;
    format_nested_with_end(expressions_from(node->exps));
    inside_enum--;

    return false;
}

bool Formatter::visit_type_declaration(Node *node) {
    accept(node->var);
    skip_space_or_newline();

    // This is for a case like `x, y : Int32`
    // (the original tests `@inside_struct_or_union && ...`, and an Int32 is always truthy)
    if (tok_->type == Tok::OP_COMMA) {
        exp_needs_indent_ = false;
        write(", ");
        next_token();
        return false;
    }

    check(Tok::OP_COLON);
    next_token_skip_space_or_newline();
    write(" : ");
    accept(node->declared_type);
    if (Node *value = node->value_node) {
        skip_space();
        check(Tok::OP_EQ);
        next_token_skip_space_or_newline();
        write(" = ");
        accept(value);
    }
    return false;
}

bool Formatter::visit_uninitialized_var(Node *node) {
    Node *var = node->var;

    if (var->is(NodeKind::Var)) vars.back().insert(var->name);

    accept(var);
    skip_space_or_newline();
    write_token(" ", Tok::OP_EQ, " ");
    skip_space_or_newline();
    write_keyword(Keyword::UNINITIALIZED, " ");
    skip_space_or_newline();
    accept(node->declared_type);
    return false;
}

// --- control expressions -----------------------------------------------------------------

bool Formatter::format_control_expression(Node *node, Keyword keyword) {
    write_keyword(keyword);

    bool has_parentheses = false;
    if (tok_->type == Tok::OP_LPAREN) {
        has_parentheses = true;
        write("(");
        next_token_skip_space_or_newline();
    }

    if (Node *exp = node->exp) {
        if (!has_parentheses) write(" ");
        skip_space();

        // If the number of consecutive `{`s starting a tuple literal is 1 less
        // than the level of tuple nesting in the actual AST node, this means the
        // parser synthesized a TupleLiteral from multiple expressions, e.g.
        //
        //     return {1, 2}, 3, 4
        //     return { {1, 2}, 3, 4 }
        //
        // The tuple depth is 2 in both cases but only 1 leading curly brace is
        // present on the first return.
        if (exp->is(NodeKind::TupleLiteral) && opening_curly_brace_count() < leading_tuple_depth(exp)) {
            format_args(exp->exps, has_parentheses);
            if (has_parentheses) skip_space();
        } else {
            indent_node(indent_, exp);
            skip_space();
        }
    }

    if (has_parentheses) write_token(Tok::OP_RPAREN);

    return false;
}

int Formatter::opening_curly_brace_count() {
    int count = 0;
    lexer_.lookahead(
        [&] {
            while (lexer_.token().type == Tok::OP_LCURLY) {
                count++;
                lexer_.next_token_skip_space_or_newline();
            }
            return false;
        },
        true);
    return count;
}

int Formatter::leading_tuple_depth(Node *exp) {
    int count = 0;
    while (exp && exp->is(NodeKind::TupleLiteral)) {
        count++;
        exp = exp->exps.empty() ? nullptr : exp->exps.front();
    }
    return count;
}

bool Formatter::visit_yield(Node *node) {
    if (Node *scope = node->scope) {
        write_keyword(Keyword::WITH, " ");
        accept(scope);
        skip_space_or_newline();
        write(" ");
    }

    write_keyword(Keyword::YIELD);

    if (tok_->type == Tok::OP_LPAREN) {
        format_parenthesized_args(node->exps);
    } else {
        if (!node->exps.empty()) write(" ");
        skip_space();
        format_args(node->exps, false);
    }

    return false;
}

bool Formatter::visit_case(Node *node) {
    slash_is_regex();
    write_keyword(Keyword::CASE);
    skip_space();

    if (Node *cond = node->cond) {
        write(" ");
        accept(cond);
    }

    skip_space_write_line();

    const bool align_number = std::all_of(node->exps.begin(), node->exps.end(), [](Node *a_when) {
        return a_when->exps.size() == 1 && a_when->exps.front()->is(NodeKind::NumberLiteral);
    });

    for (size_t i = 0; i < node->exps.size(); i++) {
        format_when(node, node->exps[i], last_p(i, node->exps), align_number);
        skip_space_or_newline_at(indent_ + 2);
    }

    skip_space_or_newline();

    if (Node *a_else = node->else_) {
        write_indent();
        write_keyword(Keyword::ELSE);
        bool found_comment = skip_space_at(indent_ + 2);
        if (tok_->type == Tok::NEWLINE || found_comment) {
            if (!found_comment) write_line();
            format_nested(a_else);
            skip_space_or_newline_at(indent_ + 2);
        } else {
            while (tok_->type == Tok::OP_SEMICOLON) next_token_skip_space();

            when_infos_.push_back(AlignInfo{Id(node), line_, column_, column_, column_, false});
            write(" ");
            accept(a_else);
            const bool wrote_newline = wrote_newline_;
            found_comment = skip_space_or_newline();
            if (!(found_comment || wrote_newline)) write_line();
        }
    }

    check_end();
    write_indent();
    write("end");
    next_token();

    return false;
}

bool Formatter::format_when(Node *case_node, Node *node, bool is_last, bool align_number) {
    (void)is_last;
    skip_space_or_newline();

    slash_is_regex();
    write_indent();
    write_keyword(node->exhaustive ? Keyword::IN : Keyword::WHEN, " ");
    const int base_indent = column_;
    const int when_start_line = line_;
    const int when_start_column = column_;
    bool next_needs_indent = false;
    for (size_t i = 0; i < node->exps.size(); i++) {
        if (next_needs_indent) write_indent(base_indent);
        accept(node->exps[i]);
        next_needs_indent = false;
        if (!last_p(i, node->exps)) {
            skip_space_or_newline();
            if (tok_->type == Tok::OP_COMMA) {
                write(",");
                slash_is_regex();
                next_token();
                const bool found_comment = skip_space();
                if (found_comment || tok_->type == Tok::NEWLINE) {
                    if (!found_comment) write_line();
                    skip_space_or_newline();
                    next_needs_indent = true;
                } else {
                    write(" ");
                }
            }
        }
    }
    const int when_column_middle = column_;
    indent2([&] { skip_space(); });
    if (tok_->type == Tok::OP_SEMICOLON || tok_->keyword(Keyword::THEN)) {
        const std::string separator = tok_->to_s();
        slash_is_regex();
        if (tok_->type == Tok::OP_SEMICOLON) {
            skip_semicolon_or_space();
        } else {
            next_token_skip_space();
        }
        if (tok_->type == Tok::NEWLINE) {
            format_nested(node->body, indent_);
        } else {
            if (separator == "then") write(" ");
            write(separator);
            write(" ");
            const int when_column_end = column_;
            accept(node->body);
            const bool wrote_newline = wrote_newline_;
            if (line_ == when_start_line) {
                when_infos_.push_back(AlignInfo{Id(case_node), line_, when_start_column, when_column_middle,
                                                when_column_end, align_number});
            }
            const bool found_comment = skip_space();
            if (!(found_comment || wrote_newline)) write_line();
        }
    } else {
        format_nested(node->body, indent_);
    }

    return false;
}

bool Formatter::visit_select(Node *node) {
    slash_is_regex();
    write_keyword(Keyword::SELECT);
    skip_space_write_line();
    skip_space_or_newline();

    for (Node *a_when : node->exps) {
        bool needs_indent = false;
        write_indent();
        write_keyword(Keyword::WHEN);
        skip_space_or_newline_at(indent_ + 2);
        write(" ");
        accept(a_when->exps.at(0));
        const bool found_comment = skip_space_at(indent_ + 2);
        if (tok_->type == Tok::OP_SEMICOLON || tok_->keyword(Keyword::THEN)) {
            const char *sep = tok_->type == Tok::OP_SEMICOLON ? "; " : " then ";
            next_token();
            skip_space_at(indent_ + 2);
            if (tok_->type == Tok::NEWLINE) {
                write_line();
                skip_space_or_newline_at(indent_ + 2);
                needs_indent = true;
            } else {
                write(sep);
                skip_space_or_newline_at(indent_ + 2);
            }
        } else {
            if (!found_comment) write_line();
            skip_space_or_newline_at(indent_ + 2);
            needs_indent = true;
        }
        if (needs_indent) {
            format_nested(a_when->body);
        } else {
            accept(a_when->body);
            write_line();
        }
        skip_space_or_newline_at(indent_ + 2);
    }

    if (Node *node_else = node->else_) {
        write_indent();
        write_keyword(Keyword::ELSE);
        const bool found_comment = skip_space_at(indent_ + 2);
        if (!found_comment) write_line();
        skip_space_or_newline_at(indent_ + 2);
        format_nested(node_else);
        skip_space_or_newline_at(indent_ + 2);
    }

    write_indent();
    write_keyword(Keyword::END);

    return false;
}

bool Formatter::visit_annotation(Node *node) {
    write_token(Tok::OP_AT_LSQUARE);
    skip_space_or_newline();

    accept(node->name_node);
    skip_space_or_newline();

    if (tok_->type == Tok::OP_LPAREN) {
        const bool has_args = !node->args.empty() || node->has_named_args;
        if (has_args) {
            format_parenthesized_args(node->args, node->has_named_args ? &node->named_args : nullptr);
        } else {
            next_token_skip_space_or_newline();
            check(Tok::OP_RPAREN);
            next_token();
        }
    }

    skip_space_or_newline();
    write_token(Tok::OP_RSQUARE);

    return false;
}

bool Formatter::visit_multi_assign(Node *node) {
    const std::vector<Node *> &targets = node->exps;
    for (size_t i = 0; i < targets.size(); i++) {
        Node *target = targets[i];
        if (target->is(NodeKind::Var)) vars.back().insert(target->name);

        accept(target);
        skip_space_or_newline();
        if (tok_->type == Tok::OP_COMMA) {
            if (!last_p(i, targets)) write(", ");
            next_token_skip_space_or_newline();
        }
    }

    write_token(" ", Tok::OP_EQ);
    skip_space();
    if (tok_->type == Tok::NEWLINE) {
        next_token_skip_space_or_newline();
        write_line();
        if (node->exps2.size() == 1) {
            write_indent(indent_ + 2, node->exps2.front());
            return false;
        }
        write_indent(indent_ + 2);
    } else {
        write(" ");
    }
    format_multi_assign_values(node->exps2);

    return false;
}

void Formatter::format_multi_assign_values(const std::vector<Node *> &values) {
    if (values.size() == 1) {
        accept_assign_value(values.front());
    } else {
        indent_at(column_, [&] {
            for (size_t i = 0; i < values.size(); i++) {
                accept(values[i]);
                if (!last_p(i, values)) {
                    skip_space_or_newline();
                    if (tok_->type == Tok::OP_COMMA) {
                        write(", ");
                        next_token_skip_space_or_newline();
                    }
                }
            }
        });
    }
}

bool Formatter::visit_exception_handler(Node *node) {
    int column = indent_;

    bool implicit_handler = false;
    if (node->implicit) {
        accept(node->body);
        if (!skip_space_or_newline(true)) write_line();
        implicit_handler = true;
        column = implicit_exception_handler_indent_;
    } else if (node->suffix_) {
        bool inline_ = false;

        // This is the case of:
        //
        //     begin exp rescue exp end
        //
        // It's parsed as:
        //
        //     begin (exp rescue exp) end
        //
        // So it's a suffix rescue inside a begin/end, but the Parser
        // returns it as an ExceptionHandler node.
        if (tok_->keyword(Keyword::BEGIN)) {
            inline_ = true;
            write_keyword(Keyword::BEGIN);
            skip_space_or_newline();
            write(" ");
        }

        accept(node->body);
        const bool passed_backslash_newline = tok_->passed_backslash_newline;
        skip_space();
        if (!passed_backslash_newline) write(" ");
        if (tok_->keyword(Keyword::RESCUE)) {
            write_keyword(Keyword::RESCUE);
            write(" ");
            next_token_skip_space_or_newline();
            if (!node->has_exps || node->exps.empty()) throw FormatBug("Nil assertion failed");
            accept(node->exps.front()->body);
        } else if (tok_->keyword(Keyword::ENSURE)) {
            write_keyword(Keyword::ENSURE);
            write(" ");
            next_token_skip_space_or_newline();
            if (!node->ensure_) throw FormatBug("Nil assertion failed");
            accept(node->ensure_);
        } else {
            throw FormatBug("expected 'rescue' or 'ensure'");
        }

        if (inline_) {
            skip_space_or_newline();
            write(" ");
            write_keyword(Keyword::END);
        }

        return false;
    }

    if (!implicit_handler) {
        write_keyword(Keyword::BEGIN);
        format_nested(node->body, column);
    }

    if (node->has_exps) {
        for (Node *node_rescue : node->exps) {
            skip_space_or_newline_at(column + 2, true);
            write_indent(column);
            write_keyword(Keyword::RESCUE);

            if (node_rescue->has_name) {
                skip_space_or_newline();
                write(" ");
                write(node_rescue->name);
                next_token();
            }

            if (node_rescue->has_exps) {
                const std::vector<Node *> &types = node_rescue->exps;
                skip_space_or_newline();
                if (node_rescue->has_name) {
                    write_token(" ", Tok::OP_COLON, " ");
                    skip_space_or_newline();
                } else {
                    write(" ");
                }
                for (size_t j = 0; j < types.size(); j++) {
                    accept(types[j]);
                    if (!last_p(j, types)) {
                        skip_space_or_newline();
                        if (tok_->type == Tok::OP_BAR) {
                            write(" | ");
                            next_token_skip_space_or_newline();
                        }
                    }
                }
            }
            format_nested(node_rescue->body, column);
        }
    }

    if (Node *node_else = node->else_) {
        skip_space_or_newline_at(column + 2, true);
        write_indent(column);
        write_keyword(Keyword::ELSE);
        format_nested(node_else, column);
    }

    if (Node *node_ensure = node->ensure_) {
        skip_space_or_newline_at(column + 2, true);
        write_indent(column);
        write_keyword(Keyword::ENSURE);
        format_nested(node_ensure, column);
    }

    if (!implicit_handler) {
        skip_space_or_newline_at(column + 2, true);
        check_end();
        write_indent(column);
        write("end");
        next_token();
    }

    return false;
}

bool Formatter::format_alias_or_typedef(Node *node, Keyword keyword, Node *value) {
    write_keyword(keyword, " ");

    if (node->is(NodeKind::Alias)) {
        accept(node->name_node);
    } else {
        write(node->name);
        next_token();
    }

    skip_space();
    write_token(" ", Tok::OP_EQ, " ");
    skip_space_or_newline();

    accept(value);

    return false;
}

// --- procs ---------------------------------------------------------------------------------

bool Formatter::visit_proc_pointer(Node *node) {
    write_token(Tok::OP_MINUS_GT);
    skip_space_or_newline();

    if (node->global) {
        write_token(Tok::OP_COLON_COLON);
        skip_space_or_newline();
    }

    if (Node *obj = node->obj) {
        accept(obj);
        skip_space();
        write_token(Tok::OP_PERIOD);
        skip_space_or_newline();
    }

    write(node->name);
    next_token_skip_space();
    if (tok_->type == Tok::OP_EQ) next_token_skip_space();

    if (tok_->type == Tok::OP_LPAREN) {
        if (!node->args.empty()) write("(");
        next_token_skip_space();
        for (size_t i = 0; i < node->args.size(); i++) {
            accept(node->args[i]);
            skip_space_or_newline();
            if (tok_->type == Tok::OP_COMMA) {
                if (!last_p(i, node->args)) write(", ");
                next_token_skip_space_or_newline();
            }
        }
        if (!node->args.empty()) write(")");
        next_token_skip_space();
    }

    return false;
}

bool Formatter::visit_proc_literal(Node *node) {
    write_token(Tok::OP_MINUS_GT);
    skip_space_or_newline();

    Node *a_def = node->def_;

    if (tok_->type == Tok::OP_LPAREN) {
        if (!a_def->args.empty()) write("(");
        next_token_skip_space_or_newline();

        for (size_t i = 0; i < a_def->args.size(); i++) {
            accept(a_def->args[i]);
            skip_space_or_newline();
            if (tok_->type == Tok::OP_COMMA) {
                if (!last_p(i, a_def->args)) write(", ");
                next_token_skip_space_or_newline();
            }
        }

        check(Tok::OP_RPAREN);
        if (!a_def->args.empty()) write(")");
        next_token_skip_space_or_newline();
    }

    if (Node *return_type = a_def->return_type) {
        check(Tok::OP_COLON);
        write(" : ");
        next_token_skip_space_or_newline();
        accept(return_type);
        skip_space_or_newline();
    }

    write(" ");

    bool is_do = false;
    if (tok_->keyword(Keyword::DO)) {
        write_keyword(Keyword::DO);
        is_do = true;
    } else {
        write_token(Tok::OP_LCURLY);
        if (a_def->body->is(NodeKind::Nop)) write(" ");
    }
    skip_space();

    if (tok_->type == Tok::NEWLINE) {
        format_nested(a_def->body);
    } else {
        skip_space_or_newline();
        if (!a_def->body->is(NodeKind::Nop)) {
            write(" ");
            accept(a_def->body);
            write(" ");
        }
    }

    indent2([&] { skip_space_or_newline(); });

    if (is_do) {
        check_end();
        write_indent();
        write("end");
        next_token();
    } else {
        if (wrote_newline_) write_indent();
        write_token(Tok::OP_RCURLY);
    }

    return false;
}

bool Formatter::visit_external_var(Node *node) {
    check(Tok::GLOBAL);
    write(tok_->value_s());
    next_token_skip_space_or_newline();

    if (tok_->type == Tok::OP_EQ) {
        write(" = ");
        next_token_skip_space_or_newline();
        write(tok_->value_s());
        next_token_skip_space_or_newline();
    }

    write_token(" ", Tok::OP_COLON, " ");
    skip_space_or_newline();

    accept(node->restriction);

    return false;
}

bool Formatter::visit_metaclass(Node *node) {
    accept(node->name_node);
    skip_space();

    write_token(Tok::OP_PERIOD);
    skip_space_or_newline();
    write_keyword(Keyword::CLASS);

    return false;
}

// --- asm -----------------------------------------------------------------------------------

bool Formatter::visit_asm(Node *node) {
    write_keyword(Keyword::ASM);
    skip_space_or_newline();

    int column = column_;
    bool has_newlines = false;

    write_token(Tok::OP_LPAREN);
    skip_space();

    if (tok_->type == Tok::NEWLINE) {
        indent_ += 2;
        consume_newlines();
        has_newlines = true;
    }
    skip_space_or_newline();

    Node *string = string_literal(node->value);

    if (has_newlines) {
        write_indent(indent_, string);
    } else {
        indent_node(column_, string);
    }

    skip_space();

    if (tok_->type == Tok::NEWLINE) {
        consume_newlines();
        if (node->has_exps || node->has_exps2) {
            column += 4;
            write_indent(column);
        }
    }

    skip_space_or_newline();

    int expected_parts;
    if (node->volatile_ || node->alignstack || node->intel || node->can_throw) {
        expected_parts = 4;
    } else if (node->has_names) {
        expected_parts = 3;
    } else if (node->has_exps2) {
        expected_parts = 2;
    } else if (node->has_exps) {
        expected_parts = 1;
    } else {
        expected_parts = 0;
    }

    if (expected_parts > 0) write(" ");
    const int colon_column = column_;

    int part_index = 0;
    while (part_index < expected_parts) {
        if (tok_->type == Tok::OP_COLON_COLON) {
            write_token(Tok::OP_COLON_COLON);
            part_index += 2;
        } else if (tok_->type == Tok::OP_COLON) {
            write_token(Tok::OP_COLON);
            part_index += 1;
        } else if (part_index > 0) {
            // The original would go round again on the same part; it never
            // gets here on code the parser accepted.
            throw FormatBug("BUG: expected ':' in asm");
        }
        skip_space();
        consume_newlines();

        if (part_index == 1) {
            if (node->has_exps) visit_asm_parts(node->exps, colon_column, [&](Node *output) { accept(output); });
        } else if (part_index == 2) {
            if (node->has_exps2) visit_asm_parts(node->exps2, colon_column, [&](Node *input) { accept(input); });
        } else if (part_index == 3) {
            if (node->has_names) {
                visit_asm_parts(node->names, colon_column,
                                [&](const std::string &clobber) { accept(string_literal(clobber)); });
            }
        } else if (part_index == 4) {
            std::vector<bool> parts;
            for (bool b : {node->volatile_, node->alignstack, node->intel, node->can_throw})
                if (b) parts.push_back(b);
            visit_asm_parts(parts, colon_column, [&](bool) { accept(string_literal("")); });
        } else {
            break;
        }
    }

    // Mop up any trailing unused : or ::, don't write them since they should be removed
    while (tok_->type == Tok::OP_COLON || tok_->type == Tok::OP_COLON_COLON) next_token_skip_space_or_newline();

    skip_space_or_newline();

    if (has_newlines) {
        indent_ -= 2;

        if (!wrote_newline_) write_line();
        write_indent();
    }

    write_token(Tok::OP_RPAREN);

    return false;
}

bool Formatter::visit_asm_operand(Node *node) {
    accept(string_literal(node->value));

    skip_space_or_newline();
    write_token(Tok::OP_LPAREN);
    skip_space_or_newline();
    accept(node->exp);
    skip_space_or_newline();
    write_token(Tok::OP_RPAREN);

    return false;
}

template <typename T, typename F>
void Formatter::visit_asm_parts(const std::vector<T> &parts, int colon_column, F &&f) {
    if (wrote_newline_) {
        write_indent();
    } else {
        write(" ");
    }

    const int column = column_;

    for (size_t i = 0; i < parts.size(); i++) {
        f(parts[i]);
        skip_space();

        if (tok_->type == Tok::OP_COMMA) {
            if (!last_p(i, parts)) write(",");
            next_token_skip_space();
        }

        if (tok_->type == Tok::NEWLINE) {
            if (last_p(i, parts)) {
                next_token_skip_space_or_newline();
                if (tok_->type == Tok::OP_COLON || tok_->type == Tok::OP_COLON_COLON) {
                    write_line();
                    write_indent(colon_column);
                }
            } else {
                consume_newlines();
                write_indent(last_p(i, parts) ? colon_column : column);
                skip_space_or_newline();
            }
        } else {
            skip_space_or_newline();
            if (last_p(i, parts)) {
                if (tok_->type == Tok::OP_COLON || tok_->type == Tok::OP_COLON_COLON) write(" ");
            } else {
                write(" ");
            }
        }
    }
}

}  // namespace crfmt
