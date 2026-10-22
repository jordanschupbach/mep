// mep's Crystal formatter: the visitors for literals, expressions, types,
// conditionals and loops, definitions and macros -- formatter.cr's
// `visit` methods from Expressions to ProcNotation, in its order. See
// crystal_format_internal.h.

#include <algorithm>

#include "crystal_format_internal.h"

namespace crfmt {

using crystal::DelimiterKind;

namespace {

uint64_t Id(const Node *n) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(n)); }

bool StartsWith(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

// Lexer.escape_forbidden_characters: the Unicode bidi controls as \uXXXX escapes.
std::string EscapeForbiddenCharacters(const std::string &s) {
    static const char32_t kBidi[] = {0x202A, 0x202B, 0x202C, 0x202D, 0x202E, 0x2066, 0x2067, 0x2068, 0x2069};
    auto is_bidi = [](std::string_view rest, char32_t *which) {
        if (rest.size() < 3) return false;
        for (char32_t c : kBidi) {
            const std::string enc = crystal::Utf8Encode(c);
            if (rest.substr(0, 3) == enc) {
                *which = c;
                return true;
            }
        }
        return false;
    };
    bool any = false;
    char32_t which;
    for (size_t i = 0; i < s.size(); i++)
        if (is_bidi(std::string_view(s).substr(i), &which)) any = true;
    if (!any) return s;
    std::string out;
    for (size_t i = 0; i < s.size();) {
        if (is_bidi(std::string_view(s).substr(i), &which)) {
            static const char kHex[] = "0123456789ABCDEF";
            out += "\\u";
            for (int shift = 12; shift >= 0; shift -= 4) out += kHex[(which >> shift) & 0xF];
            i += 3;
        } else {
            out += s[i++];
        }
    }
    return out;
}

}  // namespace

void Formatter::accept(Node *node) {
    if (!node) throw FormatBug("BUG: nil node");
    visit(node);
    // end_visit_any
    if (!node->is(NodeKind::StringInterpolation)) last_is_heredoc_ = false;
}

bool Formatter::visit(Node *node) {
    switch (node->kind) {
        case NodeKind::Nop:
            return false;
        case NodeKind::Expressions:
            return visit_expressions(node);
        case NodeKind::NilLiteral:
            write_keyword(Keyword::NIL);
            return false;
        case NodeKind::BoolLiteral:
            check_keyword(Keyword::FALSE, Keyword::TRUE);
            write(node->bool_value ? "true" : "false");
            next_token();
            return false;
        case NodeKind::CharLiteral:
            check(Tok::CHAR);
            write(tok_->raw);
            next_token();
            return false;
        case NodeKind::SymbolLiteral:
            check(Tok::SYMBOL);
            write(tok_->raw);
            next_token();
            return false;
        case NodeKind::NumberLiteral:
            if (tok_->type == Tok::MAGIC_LINE) {
                write(tok_->type);
                next_token();
                return false;
            }
            check(Tok::NUMBER);
            write(tok_->raw);
            next_token();
            return false;
        case NodeKind::StringLiteral:
            return visit_string_literal(node);
        case NodeKind::StringInterpolation:
            if (tok_->delimiter_state.kind == DelimiterKind::HEREDOC) {
                // For heredoc, only write the start: on a newline will print it
                heredoc_infos_.push_back(HeredocInfo{node, *tok_, line_, column_, indent_, string_continuation_});
                lexer_.heredocs.push_back(crystal::Lexer::HeredocEntry{tok_->delimiter_state, &heredoc_infos_.back()});
                write(tok_->raw);
                next_token();
                return false;
            }
            check(Tok::DELIMITER_START);
            return visit_string_interpolation(node, tok_, line_, column_, indent_, string_continuation_);
        case NodeKind::RegexLiteral:
            return visit_regex_literal(node);
        case NodeKind::ArrayLiteral:
            return visit_array_literal(node);
        case NodeKind::TupleLiteral:
            format_literal_elements(node->exps, Tok::OP_LCURLY, Tok::OP_RCURLY);
            return false;
        case NodeKind::HashLiteral:
            return visit_hash_literal(node);
        case NodeKind::NamedTupleLiteral:
            return visit_named_tuple_literal(node);
        case NodeKind::RangeLiteral:
            return visit_range_literal(node);
        case NodeKind::Path:
            return visit_path(node);
        case NodeKind::Generic:
            return visit_generic(node);
        case NodeKind::Union:
            return visit_union(node);
        case NodeKind::If:
            if (node->ternary) {
                accept(node->cond);
                skip_space_or_newline();
                write_token(" ", Tok::OP_QUESTION, " ");
                skip_space_or_newline();
                accept(node->then_);
                skip_space_or_newline();
                write_token(" ", Tok::OP_COLON, " ");
                skip_space_or_newline();
                accept(node->else_);
                return false;
            }
            return visit_if_or_unless(node, Keyword::IF);
        case NodeKind::Unless:
            return visit_if_or_unless(node, Keyword::UNLESS);
        case NodeKind::While:
            return format_while_or_until(node, Keyword::WHILE);
        case NodeKind::Until:
            return format_while_or_until(node, Keyword::UNTIL);
        case NodeKind::Def:
            return visit_def(node);
        case NodeKind::FunDef:
            return visit_fun_def(node);
        case NodeKind::Macro:
            return visit_macro(node);
        case NodeKind::MacroLiteral:
            return visit_macro_literal(node);
        case NodeKind::MacroVerbatim:
            return visit_macro_verbatim(node);
        case NodeKind::MacroExpression:
            return visit_macro_expression(node);
        case NodeKind::MacroIf:
            return visit_macro_if(node);
        case NodeKind::MacroFor:
            return visit_macro_for(node);
        case NodeKind::MacroVar:
            return visit_macro_var(node);
        case NodeKind::Arg:
            return visit_arg(node);
        case NodeKind::Splat:
            return visit_splat(node, Tok::OP_STAR);
        case NodeKind::DoubleSplat:
            return visit_splat(node, Tok::OP_STAR_STAR);
        case NodeKind::ProcNotation:
            return visit_proc_notation(node);
        case NodeKind::Self:
            check_open_paren();
            write_keyword(Keyword::SELF);
            check_close_paren();
            return false;
        case NodeKind::Var:
        case NodeKind::InstanceVar:
        case NodeKind::ClassVar:
        case NodeKind::Global:
            write(node->name);
            next_token();
            return false;
        case NodeKind::ReadInstanceVar:
            return visit_read_instance_var(node);
        case NodeKind::Call:
            return visit_call(node);
        case NodeKind::NamedArgument:
            return visit_named_argument(node);
        case NodeKind::IsA:
            if (node->nil_check) return visit(new_call(node->obj, "nil?"));
            return visit(new_call(node->obj, "is_a?", {node->right}));
        case NodeKind::RespondsTo: {
            Node *sym = make(NodeKind::SymbolLiteral);
            sym->value = node->name;
            return visit(new_call(node->obj, "responds_to?", {sym}));
        }
        case NodeKind::Or:
            return format_binary(node, Tok::OP_BAR_BAR, Tok::OP_BAR_BAR_EQ);
        case NodeKind::And:
            return format_binary(node, Tok::OP_AMP_AMP, Tok::OP_AMP_AMP_EQ);
        case NodeKind::Not:
            return visit_not(node);
        case NodeKind::Assign:
            return visit_assign(node);
        case NodeKind::OpAssign:
            return visit_op_assign(node);
        case NodeKind::Require:
            write_keyword(Keyword::REQUIRE, " ");
            accept(string_literal(node->value));
            return false;
        case NodeKind::VisibilityModifier:
            return visit_visibility_modifier(node);
        case NodeKind::MagicConstant:
            check(node->magic);
            write(node->magic);
            next_token();
            return false;
        case NodeKind::ModuleDef:
            return visit_module_def(node);
        case NodeKind::AnnotationDef:
            return visit_annotation_def(node);
        case NodeKind::ClassDef:
            return visit_class_def(node);
        case NodeKind::CStructOrUnionDef:
            return visit_c_struct_or_union_def(node);
        case NodeKind::Include:
            write_keyword(Keyword::INCLUDE, " ");
            accept(node->name_node);
            return false;
        case NodeKind::Extend:
            write_keyword(Keyword::EXTEND, " ");
            accept(node->name_node);
            return false;
        case NodeKind::LibDef:
            return visit_lib_def(node);
        case NodeKind::EnumDef:
            return visit_enum_def(node);
        case NodeKind::TypeDeclaration:
            return visit_type_declaration(node);
        case NodeKind::UninitializedVar:
            return visit_uninitialized_var(node);
        case NodeKind::Return:
            return format_control_expression(node, Keyword::RETURN);
        case NodeKind::Break:
            return format_control_expression(node, Keyword::BREAK);
        case NodeKind::Next:
            return format_control_expression(node, Keyword::NEXT);
        case NodeKind::Yield:
            return visit_yield(node);
        case NodeKind::Case:
            return visit_case(node);
        case NodeKind::ImplicitObj:
            return false;
        case NodeKind::Select:
            return visit_select(node);
        case NodeKind::Annotation:
            return visit_annotation(node);
        case NodeKind::Cast:
            return visit(new_call(node->obj, "as", {node->right}));
        case NodeKind::NilableCast:
            return visit(new_call(node->obj, "as?", {node->right}));
        case NodeKind::TypeOf:
            return visit(new_call(nullptr, "typeof", node->exps));
        case NodeKind::SizeOf:
            return visit(new_call(nullptr, "sizeof", {node->exp}));
        case NodeKind::InstanceSizeOf:
            return visit(new_call(nullptr, "instance_sizeof", {node->exp}));
        case NodeKind::AlignOf:
            return visit(new_call(nullptr, "alignof", {node->exp}));
        case NodeKind::InstanceAlignOf:
            return visit(new_call(nullptr, "instance_alignof", {node->exp}));
        case NodeKind::OffsetOf:
            return visit(new_call(nullptr, "offsetof", {node->left, node->right}));
        case NodeKind::PointerOf:
            return visit(new_call(nullptr, "pointerof", {node->exp}));
        case NodeKind::Underscore:
            check(Tok::UNDERSCORE);
            write("_");
            next_token();
            return false;
        case NodeKind::MultiAssign:
            return visit_multi_assign(node);
        case NodeKind::ExceptionHandler:
            return visit_exception_handler(node);
        case NodeKind::Alias:
            return format_alias_or_typedef(node, Keyword::ALIAS, node->value_node);
        case NodeKind::TypeDef:
            return format_alias_or_typedef(node, Keyword::TYPE, node->restriction);
        case NodeKind::ProcPointer:
            return visit_proc_pointer(node);
        case NodeKind::ProcLiteral:
            return visit_proc_literal(node);
        case NodeKind::ExternalVar:
            return visit_external_var(node);
        case NodeKind::Out:
            write_keyword(Keyword::OUT, " ");
            accept(node->exp);
            return false;
        case NodeKind::Metaclass:
            return visit_metaclass(node);
        case NodeKind::Block:   // Handled in format_block
        case NodeKind::When:    // Handled in format_when
        case NodeKind::Rescue:  // Handled in visit(node : ExceptionHandler)
            return false;
        case NodeKind::Asm:
            return visit_asm(node);
        case NodeKind::AsmOperand:
            return visit_asm_operand(node);
    }
    throw FormatBug(std::string("BUG: unexpected node: ") + crystal::NodeKindName(node->kind));
}

// --- Expressions -----------------------------------------------------------------------

bool Formatter::visit_expressions(Node *node) {
    const std::vector<Node *> &exps = node->exps;
    if (exps.size() == 1 && tok_->type == Tok::OP_LPAREN) {
        // If it's (...) with a single expression, we treat it
        // like a single expression, indenting it if needed
        write("(");
        next_token_skip_space();
        if (tok_->type == Tok::NEWLINE) {
            next_token_skip_space_or_newline();
            write_line();
            write_indent(indent_ + 2, exps.front());
            skip_space_write_line();
            skip_space_or_newline();
            write_indent();
            write_token(Tok::OP_RPAREN);
            return false;
        }
        skip_space_or_newline();
        accept(exps.front());
        skip_space();
        if (tok_->type == Tok::NEWLINE) {
            skip_space_or_newline();
            write_line();
            write_indent(indent_ + 2);
        }
        skip_space_or_newline();
        write(")");
        next_token();
        return false;
    }

    const int old_indent = indent_;
    bool next_needs_indent = false;

    bool has_newline = false;
    bool has_paren = false;
    bool has_begin = false;

    const bool empty_expressions = exps.size() == 1 && exps[0]->is(NodeKind::Nop);

    if (node->keyword == crystal::ExpressionsKeyword::Paren && tok_->type == Tok::OP_LPAREN) {
        write("(");
        next_needs_indent = false;
        has_paren = true;
        const bool wrote_newline = next_token_skip_space_b();
        if (tok_->type == Tok::NEWLINE || wrote_newline) {
            indent_ += 2;
            if (!wrote_newline) write_line();
            next_token_skip_space_or_newline();
            next_needs_indent = true;
            has_newline = true;
        }
    } else if (node->keyword == crystal::ExpressionsKeyword::Begin && tok_->keyword(Keyword::BEGIN)) {
        write("begin");
        indent_ += 2;
        write_line();
        next_token();
        // Corner case: an empty `begin ... end`.
        // In this case, we should not skip space because it will do in the below loop.
        if (!empty_expressions) {
            skip_space_or_newline();
            if (tok_->type == Tok::OP_SEMICOLON) next_token_skip_space_or_newline();
        }
        has_begin = true;
        next_needs_indent = true;
        has_newline = true;
    }

    Node *last_aligned_assign = nullptr;
    bool last_found_comment = false;
    std::optional<int> max_length;
    skip_space();

    for (size_t i = 0; i < exps.size(); i++) {
        Node *exp = exps[i];
        const bool is_assign = assign_p(exp);
        if (is_assign && !last_aligned_assign) {
            auto chunk = find_assign_chunk(exps, exp, i + 1);
            last_aligned_assign = chunk.first;
            max_length = chunk.second;
        } else if (!is_assign) {
            max_length.reset();
        }

        bool needs_two_lines;
        if (last_p(i, exps)) {
            needs_two_lines = false;
        } else {
            needs_two_lines = needs_two_lines_p(exp, exps[i + 1]);
        }

        assign_length_ = max_length;
        if (next_needs_indent) {
            write_indent(indent_, exp);
        } else {
            indent_node(indent_, exp);
        }

        bool found_comment = skip_space();

        if (tok_->type == Tok::OP_SEMICOLON) {
            if (needs_two_lines) {
                skip_semicolon_or_space_or_newline();
            } else {
                found_comment = skip_semicolon_or_space();
                if (tok_->type == Tok::NEWLINE) {
                    write_line();
                    next_token_skip_space();
                    next_needs_indent = true;
                } else {
                    if (!(last_p(i, exps) || found_comment)) write("; ");
                    skip_space_or_newline();
                    next_needs_indent = found_comment;
                }
            }
        } else {
            next_needs_indent = true;
        }

        if (!exp_needs_indent_) {
            next_needs_indent = false;
            exp_needs_indent_ = true;
        }

        if (last_p(i, exps)) {
            last_found_comment = skip_space_or_newline(true, false, true);
        } else if (needs_two_lines) {
            if (!found_comment) {
                if (wrote_newline_) {
                    if (!wrote_double_newlines_) write_line();
                } else if (!wrote_double_newlines_) {
                    write_line();
                    write_line();
                }
                wrote_double_newlines_ = true;
                found_comment = skip_space_or_newline();
                if (!(found_comment || wrote_double_newlines_)) write_line();
            }
        } else {
            consume_newlines();
        }

        if (last_aligned_assign == exp) last_aligned_assign = nullptr;
    }

    indent_ = old_indent;

    if (has_newline && !last_found_comment && (!wrote_newline_ || empty_expressions)) {
        // Right after the last expression but we didn't insert a newline yet
        // so we are still missing a newline following by an indent and the "end" keyword
        write_line();
        write_indent();
    } else if (has_newline && last_found_comment && wrote_newline_) {
        // Right after the last expression and we did insert a newline (because of a comment)
        // so we are still missing an indent and the "end" keyword
        write_indent();
    }

    if (has_paren) write_token(Tok::OP_RPAREN);

    if (has_begin) {
        check_end();
        next_token();
        write("end");
    }

    return false;
}

bool Formatter::assign_p(Node *exp) {
    switch (exp->kind) {
        case NodeKind::Assign:
            return exp->target->is(NodeKind::Path);
        case NodeKind::VisibilityModifier:
            return assign_p(exp->exp);
        case NodeKind::Arg:
            return true;
        default:
            return false;
    }
}

int Formatter::assign_length(Node *exp) {
    switch (exp->kind) {
        case NodeKind::Assign:
            return assign_length(exp->target);
        case NodeKind::Arg:
            return static_cast<int>(crystal::Utf8Length(exp->name));
        case NodeKind::VisibilityModifier:
            return assign_length(exp->exp);
        case NodeKind::Path:
            if (exp->names.empty()) throw FormatBug("Index out of bounds");
            return static_cast<int>(crystal::Utf8Length(exp->names.front()));
        default:
            return 0;
    }
}

std::pair<Node *, int> Formatter::find_assign_chunk(const std::vector<Node *> &expressions, Node *last, size_t i) {
    int max_length = assign_length(last);

    while (i < expressions.size()) {
        Node *exp = expressions[i];
        const crystal::Location exp_location = crystal::Loc(exp);
        if (!exp_location.known()) break;

        const crystal::Location last_location = crystal::Loc(last);
        if (!last_location.known()) break;
        if (last_location.line + 1 != exp_location.line) break;

        last = exp;
        const int exp_length = assign_length(exp);
        if (exp_length > max_length) max_length = exp_length;

        i++;
    }

    return {last, max_length};
}

bool Formatter::needs_two_lines_p(Node *node, Node *next_node) {
    if (node->is(NodeKind::Annotation) || node->is(NodeKind::MacroIf)) return false;
    if (abstract_def_p(node) && abstract_def_p(next_node)) return false;
    return needs_two_lines_p(node) || needs_two_lines_p(next_node);
}

bool Formatter::abstract_def_p(Node *node) {
    switch (node->kind) {
        case NodeKind::Def:
            return node->is_abstract;
        case NodeKind::VisibilityModifier:
            return abstract_def_p(node->exp);
        default:
            return false;
    }
}

bool Formatter::needs_two_lines_p(Node *node) {
    switch (node->kind) {
        case NodeKind::Def:
        case NodeKind::ClassDef:
        case NodeKind::ModuleDef:
        case NodeKind::LibDef:
        case NodeKind::CStructOrUnionDef:
        case NodeKind::Macro:
            return true;
        case NodeKind::VisibilityModifier:
            return needs_two_lines_p(node->exp);
        default:
            return false;
    }
}

// --- strings -------------------------------------------------------------------------------

bool Formatter::visit_string_literal(Node *node) {
    const int column = column_;

    if (tok_->type == Tok::MAGIC_FILE || tok_->type == Tok::MAGIC_DIR) {
        write(tok_->type);
        next_token();
        return false;
    }

    check(Tok::DELIMITER_START);
    const bool is_regex = tok_->delimiter_state.kind == DelimiterKind::REGEX;

    write(tok_->raw);
    next_string_token();
    const int start_line = line_;

    while (true) {
        if (tok_->type == Tok::STRING) {
            write_sanitized_string_body(tok_->delimiter_state.allow_escapes && !is_regex);
            next_string_token();
        } else if (tok_->type == Tok::INTERPOLATION_START) {
            // This is the case of #{__DIR__}
            write("#{");
            next_token_skip_space_or_newline();
            indent_node(column_, node);
            skip_space_or_newline();
            check(Tok::OP_RCURLY);
            write("}");
            next_string_token();
        } else if (tok_->type == Tok::DELIMITER_END) {
            break;
        } else {
            throw FormatBug(std::string("Bug: unexpected token: ") + crystal::TokName(tok_->type));
        }
    }

    // A multi-line literal's line ends are part of its value. Crystal's
    // formatter strips them anyway (changing the string); mep keeps them.
    for (int l = start_line; l < line_; l++) no_rstrip_lines.insert(l);
    write(tok_->raw);
    if (is_regex) format_regex_modifiers();

    if (space_slash_newline_p()) {
        const int old_indent = indent_;
        if (string_continuation_ == 0) indent_ = column;
        string_continuation_++;
        write(" \\");
        write_line();
        write_indent();
        next_token_skip_space_or_newline();
        visit(node);
        indent_ = old_indent;
        string_continuation_--;
    } else {
        next_token();
    }

    return false;
}

void Formatter::write_sanitized_string_body(bool escape) {
    std::string body = tok_->invalid_escape ? tok_->value_s() : tok_->raw;
    if (escape) body = EscapeForbiddenCharacters(body);
    write(body);
}

bool Formatter::visit_string_interpolation(Node *node, Token *token, int line, int column, int old_indent,
                                           int old_string_continuation, bool wrote_token) {
    (void)line;
    tok_ = token;

    const bool is_regex = token->delimiter_state.kind == DelimiterKind::REGEX;
    const int indent_difference = token->column_number - (column + 1);

    if (!wrote_token) write(token->raw);
    next_string_token();

    crystal::DelimiterState delimiter_state = token->delimiter_state;
    const bool is_heredoc = token->delimiter_state.kind == DelimiterKind::HEREDOC;
    last_is_heredoc_ = is_heredoc;

    const int heredoc_line = line_;

    // To detect the first content of interpolation of string literal correctly,
    // we should consume the first string token if this token contains only removed indentation of heredoc.
    if (is_heredoc && tok_->type == Tok::STRING) {
        const std::string &raw = tok_->raw;
        const bool token_is_indent =
            static_cast<int>(raw.size()) == node->heredoc_indent &&
            std::all_of(raw.begin(), raw.end(), [](char c) { return AsciiWhitespace(static_cast<unsigned char>(c)); });
        if (token_is_indent) {
            write(tok_->raw);
            next_string_token();
        }
    }

    for (Node *exp : node->exps) {
        if (tok_->type == Tok::DELIMITER_END) {
            // Heredoc cannot contain string continuation,
            // so we are done.
            if (is_heredoc) break;

            // This is for " ... " \
            //     " ... "
            if (string_continuation_ == 0) indent_ = column;
            string_continuation_++;

            write(tok_->raw);
            write(" \\");
            write_line();
            next_token_skip_space_or_newline();
            check(Tok::DELIMITER_START);
            write_indent();
            write(tok_->raw);
            next_string_token();
        }

        if (exp->is(NodeKind::StringLiteral)) {
            // It might be #{__DIR__}, for example
            if (tok_->type == Tok::INTERPOLATION_START) {
                write("#{");
                delimiter_state = tok_->delimiter_state;
                next_token_skip_space_or_newline();
                indent_node(column_, exp);
                skip_space_or_newline();
                check(Tok::OP_RCURLY);
                write("}");
                tok_->delimiter_state = delimiter_state;
                next_string_token();
            } else {
                while (true) {
                    check(Tok::STRING);
                    write_sanitized_string_body(delimiter_state.allow_escapes && !is_regex);
                    next_string_token();

                    // On heredoc, pieces of contents are combined due to removing indentation.
                    // Thus, we should consume continuous string tokens at once.
                    if (!is_heredoc || tok_->type != Tok::STRING) break;
                }
            }
        } else {
            check(Tok::INTERPOLATION_START);
            write("#{");
            delimiter_state = tok_->delimiter_state;

            bool wrote_comment = next_token_skip_space_b();
            const bool has_newline = wrote_comment || tok_->type == Tok::NEWLINE;
            skip_space_or_newline();

            if (has_newline) {
                if (!wrote_comment) write_line();
                write_indent(column_ + 2);
                indent_node(column_ + 2, exp);
                wrote_comment = skip_space_or_newline();
                if (!wrote_comment) write_line();
            } else {
                indent_node(column_, exp);
            }

            skip_space_or_newline();
            check(Tok::OP_RCURLY);
            write("}");
            tok_->delimiter_state = delimiter_state;
            next_string_token();
        }
    }

    const int heredoc_end = line_;

    check(Tok::DELIMITER_END);
    write(tok_->raw);

    if (is_heredoc) {
        if (indent_difference > 0) heredoc_fixes_.push_back(HeredocFix{heredoc_line, line_, indent_difference});
        for (int l = heredoc_line; l < heredoc_end; l++) no_rstrip_lines.insert(l);
        write_line();
    } else {
        // A multi-line literal's line ends are part of its value. Crystal's
        // formatter strips them anyway (changing the string); mep keeps them.
        for (int l = heredoc_line; l < heredoc_end; l++) no_rstrip_lines.insert(l);
    }

    if (is_regex) format_regex_modifiers();
    next_token();

    string_continuation_ = old_string_continuation;
    if (!is_heredoc) indent_ = old_indent;

    return false;
}

void Formatter::consume_heredocs() {
    consuming_heredocs_ = true;
    std::reverse(lexer_.heredocs.begin(), lexer_.heredocs.end());
    while (!lexer_.heredocs.empty()) {
        const crystal::Lexer::HeredocEntry heredoc = lexer_.heredocs.back();
        lexer_.heredocs.pop_back();
        auto *info = static_cast<HeredocInfo *>(heredoc.item);
        visit_string_interpolation(info->node, &info->token, info->line, info->column, info->indent,
                                   info->string_continuation, true);
    }
    consuming_heredocs_ = false;
}

bool Formatter::visit_regex_literal(Node *node) {
    // Go back and tokenize again if slash is recognized as divide operator.
    // In this case, slash is tokenized on comment skipping (#4626).
    // However this slash must be a start delimiter of regex because
    // this is parsed as regex literal once before.
    if (tok_->type == Tok::OP_SLASH) {
        lexer_.previous_char();
        lexer_.column_number -= 1;
        slash_is_regex();
        next_token();
    }

    accept(node->value_node);
    return false;
}

void Formatter::format_regex_modifiers() {
    while (true) {
        const char32_t c = lexer_.current_char();
        if (c == 'i' || c == 'm' || c == 'x') {
            write(crystal::Utf8Encode(c));
            lexer_.next_char();
        } else {
            break;
        }
    }
}

bool Formatter::space_slash_newline_p() {
    const int pos = lexer_.current_pos(), line = lexer_.line_number, col = lexer_.column_number;
    while (true) {
        const char32_t c = lexer_.current_char();
        if (c == ' ' || c == '\t') {
            lexer_.next_char();
        } else if (c == '\\') {
            lexer_.set_current_pos(pos);
            return true;
        } else {
            break;
        }
    }
    lexer_.set_current_pos(pos);
    lexer_.line_number = line;
    lexer_.column_number = col;
    return false;
}

bool Formatter::space_newline_p() {
    const int pos = lexer_.current_pos(), line = lexer_.line_number, col = lexer_.column_number;
    while (true) {
        const char32_t c = lexer_.current_char();
        if (c == ' ' || c == '\t') {
            lexer_.next_char();
        } else if (c == '\n') {
            lexer_.set_current_pos(pos);
            return true;
        } else {
            break;
        }
    }
    lexer_.set_current_pos(pos);
    lexer_.line_number = line;
    lexer_.column_number = col;
    return false;
}

// --- collections ---------------------------------------------------------------------------

bool Formatter::visit_array_literal(Node *node) {
    if (tok_->type == Tok::OP_LSQUARE) {
        format_literal_elements(node->exps, Tok::OP_LSQUARE, Tok::OP_RSQUARE);
    } else if (tok_->type == Tok::OP_LSQUARE_RSQUARE) {
        write("[]");
        next_token();
    } else if (tok_->type == Tok::STRING_ARRAY_START || tok_->type == Tok::SYMBOL_ARRAY_START) {
        bool first = true;
        write(tok_->raw);
        size_t count = 0;
        while (true) {
            const bool has_space_newline = space_newline_p();
            if (has_space_newline) {
                write_line();
                if (count == node->exps.size()) {
                    write_indent();
                } else {
                    write_indent(indent_ + 2);
                }
            }
            next_string_array_token();
            if (tok_->type == Tok::STRING) {
                if (!(first || has_space_newline)) write(" ");
                write(tok_->raw);
                first = false;
            } else if (tok_->type == Tok::STRING_ARRAY_END) {
                write(tok_->raw);
                next_token();
                break;
            } else {
                throw FormatBug(std::string("Bug: unexpected token ") + crystal::TokName(tok_->type));
            }
            count++;
        }
        return false;
    } else {
        Node *name = node->name_node;
        if (!name) throw FormatBug("Nil assertion failed");
        accept(name);
        skip_space();
        format_literal_elements(node->exps, Tok::OP_LCURLY, Tok::OP_RCURLY);
    }

    if (node->of) {
        write_keyword(" ", Keyword::OF, " ");
        accept(node->of);
    }

    return false;
}

template <typename E>
void Formatter::indent_element(int indent, const E &entry) {
    indent_at(indent, [&] { accept_element(entry); });
}

template <typename E>
void Formatter::write_indent_element(int indent, const E &element) {
    if (!element_is_nop(element)) write_indent(indent);
    indent_element(indent, element);
}

template <typename T>
void Formatter::format_literal_elements(const std::vector<T> &elements, Tok prefix, Tok suffix) {
    slash_is_regex();
    write_token(prefix);
    bool has_newlines = false;
    bool wrote_newline = false;
    bool write_space_at_end = false;
    bool next_needs_indent = false;
    bool found_comment = false;
    bool found_first_newline = false;
    int offset;
    int start_column;

    found_comment = skip_space();
    if (found_comment || tok_->type == Tok::NEWLINE) {
        // add one level of indentation for contents if a newline is present
        offset = indent_ + 2;
        start_column = indent_ + 2;

        if (elements.empty()) {
            skip_space_or_newline_at(offset, true, true);
            write_token(suffix);
            return;
        }

        indent_at(offset, [&] { consume_newlines(); });
        skip_space_or_newline();
        wrote_newline = true;
        next_needs_indent = true;
        has_newlines = true;
        found_first_newline = true;
    } else {
        // indent contents at the same column as starting token if no newline
        offset = indent_;
        start_column = column_;
    }

    for (size_t i = 0; i < elements.size(); i++) {
        const T &element = elements[i];
        // This is to prevent writing `{{` and `{%`
        if (prefix == Tok::OP_LCURLY && i == 0 && !wrote_newline &&
            (tok_->type == Tok::OP_LCURLY || tok_->type == Tok::OP_LCURLY_LCURLY ||
             tok_->type == Tok::OP_LCURLY_PERCENT || tok_->type == Tok::OP_PERCENT || StartsWith(tok_->raw, "%"))) {
            write(" ");
            write_space_at_end = true;
        }

        if (next_needs_indent) {
            write_indent_element(offset, element);
        } else {
            indent_element(offset, element);
        }
        const bool has_heredoc_in_line = !lexer_.heredocs.empty();

        const bool last = last_p(i, elements);

        found_comment = skip_space_at(offset, (last || has_heredoc_in_line) && has_newlines);

        bool wrote_comma = false;
        if (tok_->type == Tok::OP_COMMA) {
            if (!found_comment && (!last || has_heredoc_in_line)) {
                write(",");
                wrote_comma = true;
            }

            slash_is_regex();
            next_token();
            found_comment = skip_space_at(offset, last && has_newlines);
            if (tok_->type == Tok::NEWLINE) {
                if (last && !found_comment && !wrote_comma) {
                    write(",");
                    found_comment = true;
                }
                indent_at(offset, [&] { consume_newlines(); });
                skip_space_or_newline();
                next_needs_indent = true;
                has_newlines = true;
                offset = start_column;
            } else {
                if (!last && !found_comment) {
                    write(" ");
                    next_needs_indent = false;
                } else if (found_comment) {
                    next_needs_indent = true;
                    offset = start_column;
                }
            }
        }
    }

    finish_list(suffix, has_newlines, found_comment, found_first_newline, write_space_at_end);
}

bool Formatter::visit_hash_literal(Node *node) {
    if (node->name_node) {
        accept(node->name_node);
        skip_space();
    }

    Node *old_hash = current_hash_;
    current_hash_ = node;
    format_literal_elements(node->entries, Tok::OP_LCURLY, Tok::OP_RCURLY);
    current_hash_ = old_hash;

    if (node->has_of) {
        write_keyword(" ", Keyword::OF, " ");
        format_hash_entry(nullptr, HashEntry{node->of_key, node->of_value});
    }

    if (hash_in_same_line_.count(node)) {
        const uint64_t id = Id(node);
        hash_infos_.erase(std::remove_if(hash_infos_.begin(), hash_infos_.end(),
                                         [&](const AlignInfo &info) { return info.id == id; }),
                          hash_infos_.end());
    }

    return false;
}

void Formatter::format_hash_entry(Node *hash, const HashEntry &entry) {
    const int start_line = line_;
    const int start_column = column_;
    bool found_in_same_line = false;

    accept(entry.first);
    skip_space_or_newline();
    int middle_column = column_;
    if (tok_->type == Tok::OP_COLON && entry.first->is(NodeKind::StringLiteral)) {
        write(": ");
        slash_is_regex();
        next_token();
        middle_column = column_;
        found_in_same_line = found_in_same_line ||
                             check_hash_info(hash, entry.first, start_line, start_column, middle_column);
    } else {
        slash_is_regex();
        write_token(" ", Tok::OP_EQ_GT, " ");
        found_in_same_line = found_in_same_line ||
                             check_hash_info(hash, entry.first, start_line, start_column, middle_column);
    }

    skip_space_or_newline();
    accept(entry.second);

    if (hash && found_in_same_line) hash_in_same_line_.insert(hash);
}

bool Formatter::visit_named_tuple_literal(Node *node) {
    Node *old_hash = current_hash_;
    current_hash_ = node;
    format_literal_elements(node->named_entries, Tok::OP_LCURLY, Tok::OP_RCURLY);
    current_hash_ = old_hash;

    if (hash_in_same_line_.count(node)) {
        const uint64_t id = Id(node);
        hash_infos_.erase(std::remove_if(hash_infos_.begin(), hash_infos_.end(),
                                         [&](const AlignInfo &info) { return info.id == id; }),
                          hash_infos_.end());
    }

    return false;
}

void Formatter::format_named_tuple_entry(Node *hash, const NamedEntry &entry) {
    const int start_line = line_;
    const int start_column = column_;
    bool found_in_same_line = false;
    format_named_argument_name(entry.first);
    slash_is_regex();
    write_token(Tok::OP_COLON, " ");
    const int middle_column = column_;
    found_in_same_line =
        found_in_same_line || check_hash_info(hash, nullptr, start_line, start_column, middle_column);
    skip_space_or_newline();
    accept(entry.second);

    if (found_in_same_line) hash_in_same_line_.insert(hash);
}

void Formatter::format_named_argument_name(const std::string &name) {
    if (tok_->type == Tok::DELIMITER_START) {
        accept(string_literal(name));
    } else {
        write(tok_->to_s());
        lexer_.wants_symbol = false;
        next_token();
        lexer_.wants_symbol = true;
    }
}

void Formatter::finish_list(Tok suffix, bool has_newlines, bool found_comment, bool found_first_newline,
                            bool write_space_at_end) {
    if (tok_->type == suffix && !found_first_newline) {
        if (wrote_newline_) {
            write_indent();
        } else if (write_space_at_end) {
            write(" ");
        }
    } else {
        if (!found_comment) found_comment = skip_space_or_newline();
        check(suffix);

        if (wrote_newline_) {
            write_indent();
        } else if (has_newlines) {
            if (!found_comment) {
                write(",");
                write_line();
            }
            write_indent();
        } else if (write_space_at_end) {
            write(" ");
        }
        skip_space_or_newline();
    }

    write_token(suffix);
}

bool Formatter::check_hash_info(Node *hash, const Node *key, int start_line, int start_column, int middle_column) {
    const int end_column = column_;
    bool found_in_same_line = false;
    if (line_ == start_line) {
        if (!hash_infos_.empty() && hash_infos_.back().line == line_) {
            found_in_same_line = true;
        } else if (hash) {
            const bool number = key && key->is(NodeKind::NumberLiteral);
            hash_infos_.push_back(AlignInfo{Id(hash), line_, start_column, middle_column, end_column, number});
        }
    }
    return found_in_same_line;
}

bool Formatter::visit_range_literal(Node *node) {
    accept(node->left);
    skip_space();
    write_token(node->exclusive ? Tok::OP_PERIOD_PERIOD_PERIOD : Tok::OP_PERIOD_PERIOD);
    skip_space();
    accept(node->right);
    return false;
}

// --- types ---------------------------------------------------------------------------------

bool Formatter::check_open_paren() {
    if (tok_->type == Tok::OP_LPAREN) {
        while (tok_->type == Tok::OP_LPAREN) {
            write("(");
            next_token_skip_space();
            paren_count_++;
        }
        return true;
    }
    return false;
}

void Formatter::check_close_paren() {
    while (tok_->type == Tok::OP_RPAREN && paren_count_ > 0) {
        paren_count_--;
        write_token(Tok::OP_RPAREN);
    }
}

bool Formatter::visit_path(Node *node) {
    check_open_paren();

    // Sometimes the :: is not present because the parser generates ::Nil, for example
    if (node->global && tok_->type == Tok::OP_COLON_COLON) {
        write("::");
        next_token_skip_space_or_newline();
    }

    for (size_t i = 0; i < node->names.size(); i++) {
        skip_space_or_newline();
        check(Tok::CONST);
        write(tok_->value_s());
        next_token();
        if (!last_p(i, node->names)) skip_space();
        if (tok_->type == Tok::OP_COLON_COLON) {
            write("::");
            next_token();
        }
    }

    check_close_paren();
    return false;
}

bool Formatter::visit_generic(Node *node) {
    const bool r = visit_generic_inner(node);
    check_close_paren();  // the original's `ensure`
    return r;
}

bool Formatter::visit_generic_inner(Node *node) {
    check_open_paren();

    Node *name = node->name_node;
    if (name->global && tok_->type == Tok::OP_COLON_COLON) {
        write("::");
        next_token_skip_space_or_newline();
    }

    if (node->suffix == crystal::GenericSuffix::Question) {
        accept(node->exps.at(0));
        skip_space();
        write_token(Tok::OP_QUESTION);
        return false;
    }

    // Check if it's T* instead of Pointer(T)
    if (node->suffix == crystal::GenericSuffix::Asterisk) {
        // Count nesting asterisk
        int asterisks = 1;
        while (true) {
            if (node->exps.empty()) throw FormatBug("Index out of bounds");
            Node *type_var = node->exps.front();
            if (!(type_var->is(NodeKind::Generic) && type_var->suffix == crystal::GenericSuffix::Asterisk)) break;
            node = type_var;
            asterisks++;
        }

        if (node->exps.empty()) throw FormatBug("Index out of bounds");
        accept(node->exps.front());
        skip_space_or_newline();

        while (asterisks > 0) {
            skip_space();
            if (tok_->type == Tok::OP_STAR_STAR && asterisks >= 2) {
                write("**");
                next_token();
                asterisks -= 2;
            } else {
                write_token(Tok::OP_STAR);
                asterisks -= 1;
            }
        }

        return false;
    }

    // Check if it's T[N] instead of StaticArray(T, N)
    if (node->suffix == crystal::GenericSuffix::Bracket) {
        accept(node->exps.at(0));
        skip_space_or_newline();
        write_token(Tok::OP_LSQUARE);
        skip_space_or_newline();
        accept(node->exps.at(1));
        skip_space_or_newline();
        write_token(Tok::OP_RSQUARE);
        return false;
    }

    // NOTE: not `name.single_name?` as this is only for the expanded tuple literals
    std::string first_name;
    if (name->global && name->names.size() == 1) first_name = name->names.front();

    // Check if it's {A, B} instead of Tuple(A, B)
    if (first_name == "Tuple" && !(tok_->vkind != crystal::TokValue::None && tok_->value_s() == "Tuple")) {
        write_token(Tok::OP_LCURLY);
        const bool found_comment = skip_space_or_newline();
        bool write_space_at_end = false;
        for (size_t i = 0; i < node->exps.size(); i++) {
            Node *type_var = node->exps[i];
            // This is to prevent writing `{{` and `{%`
            if (i == 0 && !found_comment &&
                (tok_->type == Tok::OP_LCURLY || tok_->type == Tok::OP_LCURLY_LCURLY ||
                 tok_->type == Tok::OP_LCURLY_PERCENT || tok_->type == Tok::OP_PERCENT || StartsWith(tok_->raw, "%"))) {
                write(" ");
                write_space_at_end = true;
            }
            accept(type_var);
            skip_space_or_newline();
            if (tok_->type == Tok::OP_COMMA) {
                if (!last_p(i, node->exps)) write(", ");
                next_token_skip_space_or_newline();
            }
            // Write space at end when write space for preventing writing `{{` and `{%` at first.
            if (last_p(i, node->exps) && write_space_at_end) write(" ");
        }
        write_token(Tok::OP_RCURLY);
        return false;
    }

    // Check if it's {x: A, y: B} instead of NamedTuple(x: A, y: B)
    if (first_name == "NamedTuple" && !(tok_->vkind != crystal::TokValue::None && tok_->value_s() == "NamedTuple")) {
        write_token(Tok::OP_LCURLY);
        skip_space_or_newline();
        if (!node->has_named_args) throw FormatBug("Nil assertion failed");
        const std::vector<Node *> &named_args = node->named_args;
        for (size_t i = 0; i < named_args.size(); i++) {
            accept(named_args[i]);
            skip_space_or_newline();
            if (tok_->type == Tok::OP_COMMA) {
                if (!last_p(i, named_args)) write(", ");
                next_token_skip_space_or_newline();
            }
        }
        write_token(Tok::OP_RCURLY);
        return false;
    }

    accept(name);
    skip_space_or_newline();

    // Given that generic type arguments are always inside parentheses
    // we can start counting them from 0 inside them.
    const int old_paren_count = paren_count_;
    paren_count_ = 0;

    if (node->has_named_args) {
        write_token(Tok::OP_LPAREN);
        skip_space();
        const bool has_newlines = std::get<0>(format_named_args({}, node->named_args, indent_ + 2));
        // `format_named_args` doesn't skip trailing comma
        if (paren_count_ == 0 && tok_->type == Tok::OP_COMMA) {
            next_token_skip_space_or_newline();
            if (has_newlines) {
                write(",");
                write_line();
                write_indent();
            }
        }
        if (paren_count_ == 0) skip_space_or_newline();
        write_token(Tok::OP_RPAREN);
    } else {
        format_literal_elements(node->exps, Tok::OP_LPAREN, Tok::OP_RPAREN);
    }

    // Restore the old parentheses count
    paren_count_ = old_paren_count;

    return false;
}

namespace {

// ASTNode#to_s for the one comparison the formatter makes (`== "::Nil"`): a Path's text.
std::string PathToS(const Node *n) {
    if (!n->is(NodeKind::Path)) return std::string("<") + crystal::NodeKindName(n->kind) + ">";
    std::string s = n->global ? "::" : "";
    for (size_t i = 0; i < n->names.size(); i++) {
        if (i) s += "::";
        s += n->names[i];
    }
    return s;
}

}  // namespace

bool Formatter::visit_union(Node *node) {
    check_open_paren();

    if (tok_->type == Tok::IDENT && tok_->value_s() == "self?" && node->exps.size() == 2 &&
        node->exps[0]->is(NodeKind::Self) && PathToS(node->exps[1]) == "::Nil") {
        write("self?");
        next_token();
        check_close_paren();
        return false;
    }

    const int column = column_;
    for (size_t i = 0; i < node->exps.size(); i++) {
        if (tok_->type == Tok::OP_QUESTION) {
            // This can happen if it's a nilable type written like T?
            write("?");
            next_token();
            break;
        }

        accept(node->exps[i]);

        if (last_p(i, node->exps)) {
            skip_space();
        } else {
            skip_space_or_newline();
        }

        while (true) {
            if (tok_->type == Tok::OP_BAR) {
                write(" | ");
                next_token_skip_space();
                if (tok_->type == Tok::NEWLINE) {
                    write_line();
                    write_indent(column);
                    next_token_skip_space_or_newline();
                }
            } else if (tok_->type == Tok::OP_RPAREN) {
                if (paren_count_ > 0) {
                    paren_count_--;
                    write(")");
                    next_token_skip_space();
                } else {
                    break;
                }
            } else {
                break;
            }
        }
    }

    check_close_paren();
    return false;
}

// --- conditionals and loops -------------------------------------------------------------

bool Formatter::visit_if_or_unless(Node *node, Keyword keyword) {
    if (!tok_->keyword(keyword) && node->else_->is(NodeKind::Nop)) {
        // Suffix if/unless
        accept(node->then_);
        write_keyword(" ", keyword, " ");
        inside_cond([&] { indent_node(column_, node->cond); });
        return false;
    }

    write_keyword(keyword, " ");
    format_if_at_cond(node, keyword);
    return false;
}

void Formatter::format_if_at_cond(Node *node, Keyword keyword, bool check_end_) {
    inside_cond([&] { indent_node(column_, node->cond); });

    skip_space_at(indent_ + 2);
    skip_semicolon();
    format_nested(node->then_);
    skip_space_or_newline_at(indent_ + 2, true);
    jump_semicolon();

    Node *node_else = node->else_;

    if (tok_->keyword(Keyword::ELSE)) {
        write_indent();
        write("else");
        next_token();
        skip_space_at(indent_ + 2);
        skip_semicolon();
        format_nested(node->else_);
    } else if (node_else->is(NodeKind::If) && tok_->keyword(Keyword::ELSIF)) {
        format_elsif(node_else, keyword);
    }

    if (check_end_) format_end(indent_);
}

void Formatter::format_elsif(Node *node_else, Keyword keyword) {
    write_indent();
    write("elsif ");
    next_token_skip_space_or_newline();
    format_if_at_cond(node_else, keyword, false);
}

bool Formatter::format_while_or_until(Node *node, Keyword keyword) {
    write_keyword(keyword, " ");
    inside_cond([&] { indent_node(column_, node->cond); });

    format_nested_with_end(node->body);
    return false;
}

void Formatter::format_nested(Node *node, int indent, bool write_end_line, bool write_indent_) {
    slash_is_regex();
    if (node->is(NodeKind::Nop)) {
        skip_space_write_line();
    } else if (write_indent_) {
        indent_at(indent + 2, [&] {
            skip_space_write_line();
            skip_space_or_newline();
            write_indent(indent + 2, node);
            if (write_end_line) skip_space_write_line();
        });
    } else {
        skip_space_write_line();
        skip_space_or_newline();
        accept(node);
        if (write_end_line) skip_space_write_line();
    }
}

bool Formatter::format_nested_with_end(Node *node, int column, bool write_end_line) {
    skip_space_at(column + 2);

    if (tok_->type == Tok::OP_SEMICOLON) {
        if (node->is(NodeKind::Nop)) {
            skip_semicolon_or_space_or_newline();
            check_end();
            write("; end");
            next_token();
            return false;
        }
        next_token_skip_space();
    }

    format_nested(node, column, write_end_line);
    format_end(column);
    return true;
}

void Formatter::format_end(int column) {
    skip_space_or_newline_at(column + 2, true);
    check_end();
    write_indent(column);
    write("end");
    next_token();
}

// --- definitions ---------------------------------------------------------------------------

bool Formatter::visit_def(Node *node) {
    implicit_exception_handler_indent_ = indent_;
    inside_def_++;
    vars.emplace_back();

    if (node->is_abstract) write_keyword(Keyword::ABSTRACT, " ");

    write_keyword(Keyword::DEF, " ", false);

    if (node->receiver) {
        skip_space_or_newline();
        accept(node->receiver);
        skip_space_or_newline();
        write_token(Tok::OP_PERIOD);
    }

    lexer_.wants_def_or_macro_name_ = true;
    skip_space_or_newline();
    lexer_.wants_def_or_macro_name_ = false;

    write(node->name);

    indent2([&] {
        next_token();

        // this formats `def foo # ...` to `def foo(&) # ...` for yielding
        // methods before consuming the comment line
        if (node->block_arity >= 0 && node->args.empty() && !node->block_arg && !node->double_splat) write("(&)");

        skip_space(false, false);
        if (tok_->type == Tok::OP_EQ) next_token_skip_space();
    });

    const int to_skip = format_def_args(node);

    if (node->return_type) {
        skip_space();
        write_token(" ", Tok::OP_COLON, " ");
        skip_space_or_newline();
        accept(node->return_type);
    }

    if (node->has_names) {
        const std::vector<std::string> &free_vars = node->names;
        skip_space_or_newline();
        write(" forall ");
        next_token();
        const size_t last_index = free_vars.size() - 1;
        for (size_t i = 0; i < free_vars.size(); i++) {
            skip_space_or_newline();
            check(Tok::CONST);
            write(free_vars[i]);
            next_token();
            if (last_index != i) skip_space_or_newline();
            if (tok_->type == Tok::OP_COMMA) {
                write(", ");
                next_token_skip_space_or_newline();
            }
        }
    }

    Node *body = remove_to_skip(node, to_skip);

    if (!node->is_abstract) format_nested_with_end(body);

    vars.pop_back();
    inside_def_--;

    return false;
}

int Formatter::format_def_args(Node *node) {
    const bool yields = node->is(NodeKind::Def) && node->block_arity >= 0;
    return format_def_args(node->args, node->block_arg, node->splat_index, false, node->double_splat, yields);
}

int Formatter::format_def_args(const std::vector<Node *> &args, Node *block_arg, int splat_index, bool variadic,
                               Node *double_splat, bool yields) {
    // If there are no args, remove extra "()"
    if (args.empty() && !block_arg && !double_splat && !variadic) {
        if (tok_->type == Tok::OP_LPAREN) {
            next_token_skip_space_or_newline();
            check(Tok::OP_RPAREN);
            next_token();
        }
        return 0;
    }

    // Count instance variable arguments. See `at_skip?`.
    int to_skip = 0;

    bool wrote_newline = false;
    bool found_first_newline = false;

    const int old_indent = indent_;
    indent_ = column_ + 1;

    write_token(Tok::OP_LPAREN);
    skip_space();

    // When "(" follows newline, it turns on two spaces indentation mode.
    if (tok_->type == Tok::NEWLINE) {
        indent_ = old_indent + 2;
        found_first_newline = true;
        wrote_newline = true;

        write_line();
        next_token_skip_space_or_newline();
    }

    for (size_t i = 0; i < args.size(); i++) {
        Node *arg = args[i];
        const bool has_more = !last_p(i, args) || double_splat || block_arg || yields || variadic;
        wrote_newline = format_def_arg(wrote_newline, has_more, found_first_newline && !has_more, [&] {
            if (static_cast<int>(i) == splat_index) {
                write_token(Tok::OP_STAR);
                skip_space_or_newline();
                if (arg->external_name.empty()) return;  // skip empty splat argument.
            }

            format_parameter_annotations(arg);

            accept(arg);
            if (last_arg_is_skip_) to_skip++;
        });
    }

    if (double_splat) {
        wrote_newline = format_def_arg(wrote_newline, block_arg || yields, found_first_newline, [&] {
            write_token(Tok::OP_STAR_STAR);
            skip_space_or_newline();

            if (at_skip_p()) to_skip++;
            accept(double_splat);
        });
    }

    if (block_arg) {
        wrote_newline = format_def_arg(wrote_newline, false, false, [&] {
            format_parameter_annotations(block_arg);
            write_token(Tok::OP_AMP);
            skip_space_or_newline();

            if (at_skip_p()) to_skip++;
            accept(block_arg);
        });
    } else if (yields) {
        wrote_newline = format_def_arg(wrote_newline, false, false, [&] {
            write("&");
            skip_space_or_newline();
        });
    }

    if (variadic) {
        wrote_newline = format_def_arg(wrote_newline, false, false, [&] { write_token(Tok::OP_PERIOD_PERIOD_PERIOD); });
    }

    if (found_first_newline && !wrote_newline) {
        write_line();
        wrote_newline = true;
    }
    if (wrote_newline) write_indent(found_first_newline ? old_indent : indent_);
    write_token(Tok::OP_RPAREN);

    indent_ = old_indent;

    return to_skip;
}

void Formatter::format_parameter_annotations(Node *node) {
    if (!node->has_annotations) return;

    for (Node *ann : node->annotations) {
        accept(ann);

        skip_space();

        if (tok_->type == Tok::NEWLINE) {
            write_line();
            write_indent();
        } else {
            write(" ");
        }

        skip_space_or_newline();
    }

    if (tok_->type == Tok::NEWLINE) {
        skip_space_or_newline();
        write_line();
        write_indent();
    }
}

template <typename F>
bool Formatter::format_def_arg(bool wrote_newline, bool has_more, bool write_trailing_comma, F &&body) {
    if (wrote_newline) write_indent();

    body();

    // Write "," before skipping spaces to prevent inserting comment between argument and comma.
    if (has_more || (wrote_newline && tok_->type == Tok::OP_COMMA) || write_trailing_comma) write(",");

    bool just_wrote_newline = skip_space();
    if (tok_->type == Tok::NEWLINE) {
        if (has_more) {
            consume_newlines();
            just_wrote_newline = true;
        } else {
            // `last: true` is needed to write newline and comment only if comment is found.
            just_wrote_newline = skip_space_or_newline(true);
        }
    }

    if (tok_->type == Tok::OP_COMMA) {
        const bool found_comment = next_token_skip_space_b();
        if (found_comment) {
            just_wrote_newline = true;
        } else if (tok_->type == Tok::NEWLINE) {
            if (has_more && !just_wrote_newline) {
                consume_newlines();
                just_wrote_newline = true;
            } else {
                just_wrote_newline = skip_space_or_newline(true) || just_wrote_newline;
            }
        } else {
            if (has_more && !just_wrote_newline) write(" ");
        }
    } else if (tok_->type == Tok::OP_RPAREN && has_more && !just_wrote_newline) {
        // if we found a `)` and there are still more parameters to write, it
        // must have been a missing `&` for a def that yields
        write(" ");
    }

    return just_wrote_newline;
}

bool Formatter::at_skip_p() { return tok_->type == Tok::INSTANCE_VAR || tok_->type == Tok::CLASS_VAR; }

bool Formatter::visit_fun_def(Node *node) {
    write_keyword(Keyword::FUN, " ");

    check(Tok::IDENT, Tok::CONST);
    write(node->name);
    next_token_skip_space();

    if (tok_->type == Tok::OP_EQ) {
        write(" = ");
        next_token_skip_space();

        if (tok_->type == Tok::NEWLINE) {
            write_line();
            next_token_skip_space_or_newline();
            write_indent(indent_ + 2);
        }

        if (tok_->type == Tok::DELIMITER_START) {
            indent_node(column_, string_literal(node->value));
        } else {
            write(node->value);
            next_token_skip_space();
        }
    }

    format_def_args(node->args, nullptr, -1, node->varargs, nullptr, false);

    if (node->return_type) {
        skip_space();
        write_token(" ", Tok::OP_COLON, " ");
        skip_space_or_newline();
        accept(node->return_type);
    }

    if (node->body) format_nested_with_end(node->body);

    return false;
}

// --- macros --------------------------------------------------------------------------------

bool Formatter::visit_macro(Node *node) {
    reset_macro_state();

    const int macro_node_line = line_;

    write_keyword(Keyword::MACRO, " ");

    write(node->name);
    next_token();

    if (tok_->type == Tok::OP_EQ && !node->name.empty() && node->name.back() == '=') next_token();

    skip_space(false, false);

    format_def_args(node);

    if (std::optional<std::string> macro_literal_source = macro_literal_contents(node->body)) {
        format_macro_literal_only(node, *macro_literal_source, macro_node_line);
    } else {
        format_macro_body(node);
    }

    return false;
}

void Formatter::format_macro_literal_only(Node *node, const std::string &source, int macro_node_line) {
    try {
        // Only format the macro contents if it's valid Crystal code
        {
            crystal::Ast ast;
            crystal::Parser parser(source, ast);
            parser.parse();
        }

        std::set<int> sub_no_rstrip_lines;
        const std::string value = subformat(source, &sub_no_rstrip_lines);

        // The formatted contents might have heredocs for which we must preserve
        // trailing spaces, so here we copy those from the formatter we used
        // to format the contents to this formatter (we add one because we insert
        // a newline before the contents).
        for (int line : sub_no_rstrip_lines) no_rstrip_lines.insert(macro_node_line + line + 1);

        write_line();
        write(value);

        next_macro_token();

        while (tok_->type != Tok::MACRO_END) next_macro_token();

        skip_space_or_newline();
        check(Tok::MACRO_END);
        write_indent();
        write("end");
        next_token();
    } catch (const crystal::SyntaxError &) {
        format_macro_body(node);
    }
}

bool Formatter::format_macro_body(Node *node) {
    if (tok_->keyword(Keyword::END)) return format_macro_end();

    next_macro_token();

    if (tok_->type == Tok::MACRO_END) return format_macro_end();

    Node *body = node->body;
    if (body->is(NodeKind::Expressions) && body->exps.empty()) {
        while (tok_->type != Tok::MACRO_END) next_macro_token();
        return format_macro_end();
    }

    inside_macro([&] { no_indent_do([&] { format_nested(body, indent_, false, false); }); });

    skip_space_or_newline();
    check(Tok::MACRO_END);
    write("end");
    next_token();
    return false;
}

bool Formatter::format_macro_end() {
    write_line();
    write_indent();
    write("end");
    next_token();
    return false;
}

bool Formatter::visit_macro_literal(Node *node) {
    (void)node;
    int line = line_;
    for (char c : tok_->raw) {
        if (c == '\n') {
            line--;
            no_rstrip_lines.insert(line);
        }
    }

    std::string raw = tok_->raw;

    // If the macro literal has a backlash, but we are subformatting
    // a macro, we need to escape it (if it got here unescaped it was escaped to
    // begin with.)
    if (subformat_nesting > 0) {
        const std::string slashes(static_cast<size_t>(subformat_nesting + 1), '\\');
        std::string escaped;
        for (char c : raw) {
            if (c == '\\') {
                escaped += slashes;
            } else {
                escaped += c;
            }
        }
        raw = escaped;
    }

    write(raw);
    next_macro_token();
    return false;
}

bool Formatter::visit_macro_verbatim(Node *node) {
    reset_macro_state();

    // `{% verbatim %}`
    if (inside_macro_p()) {
        check(Tok::MACRO_CONTROL_START);
    } else {
        check(Tok::OP_LCURLY_PERCENT);
    }
    write("{%");
    next_token_skip_space_or_newline();
    check_keyword(Keyword::VERBATIM);
    write(" verbatim");
    next_token_skip_space();
    check_keyword(Keyword::DO);
    write(" do");
    next_token_skip_space();
    check(Tok::OP_PERCENT_RCURLY);
    write(" %}");

    macro_state_.control_nest++;
    check_macro_whitespace();
    next_macro_token();
    inside_macro([&] { no_indent(node->exp); });
    macro_state_.control_nest--;

    // `{% end %}`
    check(Tok::MACRO_CONTROL_START);
    write("{%");
    next_token_skip_space_or_newline();
    check_keyword(Keyword::END);
    write(" end");
    next_token_skip_space();
    check(Tok::OP_PERCENT_RCURLY);
    write(" %}");

    if (inside_macro_p()) {
        check_macro_whitespace();
        next_macro_token();
    } else {
        next_token();
    }

    return false;
}

bool Formatter::visit_macro_expression(Node *node) {
    reset_macro_state();

    const int old_column = column_;

    if (node->output) {
        if (inside_macro_p()) {
            check(Tok::MACRO_EXPRESSION_START);
        } else {
            check(Tok::OP_LCURLY_LCURLY);
        }
        write_macro_slashes();
        write("{{");
    } else {
        if (!(tok_->type == Tok::MACRO_CONTROL_START || tok_->type == Tok::OP_LCURLY_PERCENT))
            check(Tok::MACRO_CONTROL_START);
        write_macro_slashes();
        write("{%");
    }
    const crystal::MacroState macro_state = macro_state_;
    next_token();

    const bool has_space = tok_->type == Tok::SPACE;
    skip_space();
    const bool has_newline = tok_->type == Tok::NEWLINE;

    const int old_indent = indent_;
    indent_ = column_;

    if (has_newline) {
        write_line();
        write_indent();
        skip_space_or_newline();
        if (line_output_.empty()) {
            write_indent(indent_, node->exp);
        } else {
            indent_node(indent_, node->exp);
        }
    } else {
        skip_space_or_newline();
        if (has_space || !node->output) write(" ");
        indent_node(column_, node->exp);
    }

    indent_ = old_indent;

    skip_space_or_newline();
    macro_state_ = macro_state;

    if (node->output) {
        if (wrote_newline_) {
            write_indent(old_column);
        } else if (has_space && !has_newline) {
            write(" ");
        } else if (has_newline) {
            write_line();
            write_indent(old_column);
        }
        check(Tok::OP_RCURLY);
        next_token();
        check(Tok::OP_RCURLY);
        write("}}");
    } else {
        check(Tok::OP_PERCENT_RCURLY);
        if (wrote_newline_) {
            write_indent(old_column);
        } else if (has_newline) {
            write_line();
            write_indent(old_column);
        } else {
            write(" ");
        }
        write("%}");
    }

    if (inside_macro_p()) {
        check_macro_whitespace();
        next_macro_token();
    } else {
        next_token();
    }

    return false;
}

bool Formatter::visit_macro_if(Node *node) {
    reset_macro_state();

    if (inside_macro_p()) {
        check(Tok::MACRO_CONTROL_START);
    } else {
        check(Tok::OP_LCURLY_PERCENT);
    }

    write_macro_slashes();
    write("{% ");

    next_token_skip_space_or_newline();

    if (tok_->keyword(Keyword::BEGIN)) {
        // This is rewritten to `if true`
        write("begin");
        next_token();
    } else if (tok_->keyword(Keyword::UNLESS)) {
        // This is rewritten to `if !...`
        std::swap(node->then_, node->else_);
        write("unless ");
        next_token_skip_space_or_newline();

        outside_macro([&] { indent_node(column_, node->cond); });
    } else {
        write_keyword(Keyword::IF, " ");
        outside_macro([&] { indent_node(column_, node->cond); });
    }

    return format_macro_if_epilogue(node, macro_state_);
}

bool Formatter::format_macro_if_epilogue(Node *node, crystal::MacroState macro_state, bool check_end_) {
    skip_space_or_newline();
    check(Tok::OP_PERCENT_RCURLY);
    write(" %}");

    macro_state_ = macro_state;
    macro_state_.control_nest++;
    check_macro_whitespace();

    int macro_node_line = line_;
    next_macro_token();
    format_macro_contents(node->then_, macro_node_line);

    if (!node->else_->is(NodeKind::Nop)) {
        check(Tok::MACRO_CONTROL_START);
        next_token_skip_space_or_newline();

        if (tok_->keyword(Keyword::ELSIF)) {
            Node *sub_if = node->else_;
            if (!sub_if->is(NodeKind::MacroIf)) throw FormatBug("Cast failed: expected MacroIf");
            next_token_skip_space_or_newline();
            write_macro_slashes();
            write("{% elsif ");
            outside_macro([&] { indent_node(column_, sub_if->cond); });
            format_macro_if_epilogue(sub_if, macro_state, false);
        } else {
            check_keyword(Keyword::ELSE);
            next_token_skip_space_or_newline();
            check(Tok::OP_PERCENT_RCURLY);

            write_macro_slashes();
            write("{% else %}");

            macro_state_ = macro_state;
            macro_state_.control_nest++;
            check_macro_whitespace();

            macro_node_line = line_;
            next_macro_token();
            format_macro_contents(node->else_, macro_node_line);
        }
    }

    macro_state_ = macro_state;
    if (check_end_) {
        check(Tok::MACRO_CONTROL_START);
        next_token_skip_space_or_newline();

        check_end();
        next_token_skip_space_or_newline();
        check(Tok::OP_PERCENT_RCURLY);

        write_macro_slashes();
        write("{% end %}");

        if (inside_macro_p()) {
            check_macro_whitespace();
            next_macro_token();
        } else {
            next_token();
        }
    }

    return false;
}

bool Formatter::visit_macro_for(Node *node) {
    reset_macro_state();
    const crystal::MacroState old_macro_state = macro_state_;

    if (inside_macro_p()) {
        check(Tok::MACRO_CONTROL_START);
    } else {
        check(Tok::OP_LCURLY_PERCENT);
    }

    write_macro_slashes();
    write("{% ");

    next_token_skip_space_or_newline();

    write_keyword(Keyword::FOR, " ");

    outside_macro([&] {
        for (size_t i = 0; i < node->exps.size(); i++) {
            no_indent(node->exps[i]);
            if (!last_p(i, node->exps)) {
                skip_space_or_newline();
                if (tok_->type == Tok::OP_COMMA) {
                    write(", ");
                    next_token_skip_space_or_newline();
                }
            }
        }
    });

    write_keyword(" ", Keyword::IN, " ");

    outside_macro([&] { indent_node(column_, node->exp); });
    skip_space_or_newline();

    check(Tok::OP_PERCENT_RCURLY);
    write(" %}");

    macro_state_.control_nest++;
    check_macro_whitespace();

    const int macro_node_line = line_;
    next_macro_token();
    format_macro_contents(node->body, macro_node_line);

    macro_state_ = old_macro_state;

    check(Tok::MACRO_CONTROL_START);
    next_token_skip_space_or_newline();

    check_end();
    next_token_skip_space_or_newline();
    check(Tok::OP_PERCENT_RCURLY);

    write_macro_slashes();
    write("{% end %}");

    if (inside_macro_p()) {
        check_macro_whitespace();
        next_macro_token();
    } else {
        next_token();
    }

    return false;
}

bool Formatter::visit_macro_var(Node *node) {
    check(Tok::MACRO_VAR);
    write("%");
    write(node->name);

    if (node->has_exps) {
        next_token();
        write_token(Tok::OP_LCURLY);
        skip_space_or_newline();
        for (size_t i = 0; i < node->exps.size(); i++) {
            indent_node(column_, node->exps[i]);
            skip_space_or_newline();
            if (tok_->type == Tok::OP_COMMA) {
                if (!last_p(i, node->exps)) write(", ");
                next_token_skip_space_or_newline();
            }
        }
        check(Tok::OP_RCURLY);
        write("}");
    }

    next_macro_token();
    return false;
}

void Formatter::write_macro_slashes() {
    for (int i = 0; i < subformat_nesting; i++) write("\\");
}

void Formatter::format_macro_contents(Node *node, int macro_node_line) {
    // If macro contents don't have interpolations nor newlines, and we
    // are at the top-level (not already inside a macro) then the content
    // must be a valid Crystal expression and we can format it.
    std::optional<std::string> value;
    if (!inside_macro_p() && (value = macro_literal_contents(node)) && value->find('\n') != std::string::npos) {
        // Format the value and append 2 more spaces of indentation
        std::set<int> sub_no_rstrip_lines;
        std::string formatted;
        try {
            formatted = subformat(*value, &sub_no_rstrip_lines);
        } catch (const crystal::SyntaxError &ex) {
            throw crystal::SyntaxError(ex.what(), ex.line + macro_node_line, ex.column, ex.size);
        }

        // The formatted contents might have heredocs for which we must preserve
        // trailing spaces, so here we copy those from the formatter we used
        // to format the contents to this formatter (we add one because we insert
        // a newline before the contents).
        for (int line : sub_no_rstrip_lines) no_rstrip_lines.insert(macro_node_line + line + 1);

        write_line();
        write(formatted);
        // No need to append a newline because the formatter value
        // will already have it.
        write_indent();

        int line_count = 0;
        for (size_t start = 0; start < formatted.size();) {
            const size_t nl = formatted.find('\n', start);
            line_count++;
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
        increment_lines(macro_node_line + line_count + 1 - line_);

        const int line = line_;

        // We have to potentially skip multiple macro literal tokens
        while (tok_->type == Tok::MACRO_LITERAL) next_macro_token();

        // Skipping the macro literal tokens might have altered `@line`:
        // restore it to what it was before the macro tokens (we are
        // already accounting for the lines in a different way).
        if (line_ != line) increment_lines(line - line_);
    } else {
        inside_macro([&] { no_indent(node); });
    }
}

std::optional<std::string> Formatter::macro_literal_contents(Node *node) {
    if (!only_macro_literal_p(node)) return std::nullopt;
    std::string io;
    extract_macro_literal_contents(node, io);
    return io;
}

bool Formatter::only_macro_literal_p(Node *node) {
    switch (node->kind) {
        case NodeKind::MacroLiteral:
            return true;
        case NodeKind::Expressions:
            return std::all_of(node->exps.begin(), node->exps.end(), [&](Node *exp) { return only_macro_literal_p(exp); });
        default:
            return false;
    }
}

void Formatter::extract_macro_literal_contents(Node *node, std::string &io) {
    if (node->is(NodeKind::MacroLiteral)) {
        io += node->value;
    } else {
        for (Node *exp : node->exps) extract_macro_literal_contents(exp, io);
    }
}

std::string Formatter::subformat(const std::string &source, std::set<int> *no_rstrip_lines_out) {
    using Mode = crystal::Parser::ParseMode;
    Mode mode;
    if (inside_struct_or_union > 0) {
        mode = Mode::LibStructOrUnion;
    } else if (inside_enum < 0) {
        mode = Mode::Enum;
    } else if (inside_lib > 0) {
        mode = Mode::Lib;
    } else {
        mode = Mode::Normal;
    }

    crystal::Ast ast;
    crystal::Parser parser(source, ast);
    parser.set_var_scopes(vars);
    Node *nodes = parser.parse(mode);

    Formatter formatter(source);
    formatter.inside_lib = inside_lib;
    formatter.inside_enum = inside_enum;
    formatter.inside_struct_or_union = inside_struct_or_union;
    formatter.indent_ = indent_ + 2;
    formatter.skip_space_or_newline();
    formatter.write_indent();
    formatter.subformat_nesting = subformat_nesting + 1;
    formatter.accept(nodes);
    std::string result = formatter.finish();
    *no_rstrip_lines_out = formatter.no_rstrip_lines;
    return result;
}

// --- parameters and type notation ----------------------------------------------------------

bool Formatter::visit_arg(Node *node) {
    last_arg_is_skip_ = false;

    Node *restriction = node->restriction;
    Node *default_value = node->default_value;

    if (inside_lib > 0) {
        // This is the case of `fun foo(Char)`
        if (tok_->type != Tok::IDENT && restriction) {
            accept(restriction);
            return false;
        }
    }

    if (node->name.empty()) {
        skip_space_or_newline();
    } else {
        vars.back().insert(node->name);

        const bool at_skip = at_skip_p();

        if (!at_skip && node->external_name != node->name) {
            if (node->external_name.empty()) {
                write("_");
            } else if (tok_->type == Tok::DELIMITER_START) {
                accept(string_literal(node->external_name));
            } else {
                write(tok_->value_s());
            }
            write(" ");
            next_token_skip_space_or_newline();
        }

        last_arg_is_skip_ = at_skip_p();

        write(tok_->value_s());
        next_token();
    }

    if (restriction) {
        skip_space_or_newline();
        write_token(" ", Tok::OP_COLON, " ");
        skip_space_or_newline();
        accept(restriction);
    }

    if (default_value) {
        // The default value might be a Proc with args, so
        // we need to remember this and restore it later
        const bool old_last_arg_is_skip = last_arg_is_skip_;

        skip_space_or_newline();

        const bool check_align = check_assign_length(node);
        write_token(" ", Tok::OP_EQ, " ");
        const int before_column = column_;
        skip_space_or_newline();
        accept(default_value);
        if (check_align) check_assign_align(before_column, default_value);

        last_arg_is_skip_ = old_last_arg_is_skip;
    }

    // This is the case of an enum member
    if (tok_->type == Tok::OP_SEMICOLON) {
        next_token();
        lexer_.skip_space();
        if (tok_->type == Tok::COMMENT) {
            write_comment();
            exp_needs_indent_ = true;
        } else {
            if (tok_->type == Tok::CONST) write(";");
            write(" ");
            exp_needs_indent_ = tok_->type == Tok::NEWLINE;
        }
    }

    return false;
}

void Formatter::reset_macro_state() {
    if (!inside_macro_p()) macro_state_ = crystal::MacroState{};
}

bool Formatter::visit_splat(Node *node, Tok token) {
    write_token(token);
    skip_space_or_newline();
    accept(node->exp);
    return false;
}

bool Formatter::visit_proc_notation(Node *node) {
    check_open_paren();

    const int paren_count = paren_count_;

    if (node->has_exps) {
        const std::vector<Node *> &inputs = node->exps;
        // Check if it's ((X, Y) -> Z)
        //                ^    ^
        int sub_paren_count = paren_count_;
        if (check_open_paren()) sub_paren_count = paren_count_;

        for (size_t i = 0; i < inputs.size(); i++) {
            accept(inputs[i]);
            if (paren_count_ == sub_paren_count) {
                skip_space_or_newline();
                if (tok_->type == Tok::OP_COMMA) {
                    if (!last_p(i, inputs)) write(", ");
                    next_token_skip_space_or_newline();
                }
            }
        }

        if (sub_paren_count != paren_count) check_close_paren();
    }

    if (paren_count == paren_count_) skip_space_or_newline();
    check_close_paren();
    skip_space();

    if (node->has_exps) write(" ");
    write_token(Tok::OP_MINUS_GT);

    if (node->right) {
        write(" ");
        skip_space_or_newline();
        accept(node->right);
    } else {
        skip_space();
    }

    check_close_paren();

    return false;
}

bool Formatter::visit_read_instance_var(Node *node) {
    accept(node->obj);

    skip_space_or_newline();
    write_token(Tok::OP_PERIOD);
    skip_space_or_newline();
    write(node->name);
    next_token();

    return false;
}

}  // namespace crfmt
