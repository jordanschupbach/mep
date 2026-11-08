/**
 * @file zig_ast.cpp
 * @brief std.zig.Ast's queries (lib/std/zig/Ast.zig, Zig 0.16): token
 *        locations, error messages, first/last token, the full.* views.
 *
 * Ported from Zig, MIT license, Copyright (c) Zig contributors; the notice is
 * in zig_ast.h. Function names and order follow Ast.zig. `renderErrors` at
 * the end ports the error lowering `zig fmt` / `zig ast-check` do in
 * AstGen.lowerAstErrors.
 */
#include "zig_ast.h"

#include <cassert>
#include <cstring>

namespace zig {

namespace {

const std::string_view kNodeTagNames[] = {
#define ZIG_X(name, str) str,
    ZIG_NODE_TAGS(ZIG_X)
#undef ZIG_X
};

/// loadOptionalNodesIntoBuffer: the leading present entries of `items`.
template <std::size_t N>
std::span<const NodeIndex> loadOptionalNodesIntoBuffer(NodeIndex* buffer, const std::array<std::uint32_t, N>& items) {
    for (std::size_t i = 0; i < N; ++i) {
        if (items[i] == kNone) return std::span<const NodeIndex>(buffer, i);
        buffer[i] = items[i];
    }
    return std::span<const NodeIndex>(buffer, N);
}

OptionalIndex opt(NodeIndex n) { return OptionalIndex::from(n); }

}  // namespace

std::string_view nodeTagName(NodeTag tag) { return kNodeTagNames[static_cast<std::size_t>(tag)]; }

bool Ast::isTokenPrecededByTags(TokenIndex ti, std::initializer_list<TokenTag> expected) const {
    if (expected.size() > ti) return false;
    TokenIndex i = ti - static_cast<TokenIndex>(expected.size());
    for (TokenTag t : expected) {
        if (tokens[i].tag != t) return false;
        ++i;
    }
    return true;
}

std::uint32_t Ast::errorOffset(const Error& parse_error) const {
    return parse_error.token_is_prev ? static_cast<std::uint32_t>(tokenSlice(parse_error.token).size()) : 0;
}

Location Ast::tokenLocation(ByteOffset start_offset, TokenIndex token_index) const {
    Location loc{0, 0, start_offset, source.size()};
    const std::size_t token_start = tokenStart(token_index);

    // Scan to by line until we go past the token start
    while (true) {
        std::size_t i = source.find('\n', loc.line_start);
        if (i == std::string::npos) break;
        if (i >= token_start) break;  // Went past
        loc.line += 1;
        loc.line_start = i + 1;
    }

    const std::size_t offset = loc.line_start;
    for (std::size_t i = 0; offset + i < source.size(); ++i) {
        const char c = source[offset + i];
        if (i + offset == token_start) {
            loc.line_end = i + offset;
            while (loc.line_end < source.size() && source[loc.line_end] != '\n') loc.line_end += 1;
            return loc;
        }
        if (c == '\n') {
            loc.line += 1;
            loc.column = 0;
            loc.line_start = i + 1;
        } else {
            loc.column += 1;
        }
    }
    return loc;
}

std::string_view Ast::tokenSlice(TokenIndex token_index) const {
    const TokenTag token_tag = tokenTag(token_index);

    // Many tokens can be determined entirely by their tag.
    std::string_view lexeme = tokenLexeme(token_tag);
    if (!lexeme.empty()) return lexeme;

    // For some tokens, re-tokenization is needed to find the end.
    Tokenizer tokenizer{source.c_str(), source.size(), tokenStart(token_index)};
    const Token token = tokenizer.next();
    assert(token.tag == token_tag);
    return std::string_view(source).substr(token.start, token.end - token.start);
}

std::span<const NodeIndex> Ast::extraDataSlice(SubRange range) const {
    return std::span<const NodeIndex>(extra_data.data() + range.start, range.end - range.start);
}

std::span<const NodeIndex> Ast::extraDataSliceWithLen(ExtraIndex start, std::uint32_t len) const {
    return std::span<const NodeIndex>(extra_data.data() + start, len);
}

extra::LocalVarDecl Ast::extraLocalVarDecl(ExtraIndex i) const { return {extra_data[i], extra_data[i + 1]}; }
extra::ArrayTypeSentinel Ast::extraArrayTypeSentinel(ExtraIndex i) const { return {extra_data[i], extra_data[i + 1]}; }
extra::PtrType Ast::extraPtrType(ExtraIndex i) const {
    return {opt(extra_data[i]), opt(extra_data[i + 1]), opt(extra_data[i + 2])};
}
extra::PtrTypeBitRange Ast::extraPtrTypeBitRange(ExtraIndex i) const {
    return {opt(extra_data[i]), extra_data[i + 1], opt(extra_data[i + 2]), extra_data[i + 3], extra_data[i + 4]};
}
extra::If Ast::extraIf(ExtraIndex i) const { return {extra_data[i], extra_data[i + 1]}; }
extra::ContainerField Ast::extraContainerField(ExtraIndex i) const { return {extra_data[i], extra_data[i + 1]}; }
extra::GlobalVarDecl Ast::extraGlobalVarDecl(ExtraIndex i) const {
    return {opt(extra_data[i]), opt(extra_data[i + 1]), opt(extra_data[i + 2]), opt(extra_data[i + 3])};
}
extra::Slice Ast::extraSlice(ExtraIndex i) const { return {extra_data[i], extra_data[i + 1]}; }
extra::SliceSentinel Ast::extraSliceSentinel(ExtraIndex i) const {
    return {extra_data[i], opt(extra_data[i + 1]), extra_data[i + 2]};
}
extra::While Ast::extraWhile(ExtraIndex i) const { return {opt(extra_data[i]), extra_data[i + 1], extra_data[i + 2]}; }
extra::WhileCont Ast::extraWhileCont(ExtraIndex i) const { return {extra_data[i], extra_data[i + 1]}; }
extra::FnProtoOne Ast::extraFnProtoOne(ExtraIndex i) const {
    return {opt(extra_data[i]), opt(extra_data[i + 1]), opt(extra_data[i + 2]), opt(extra_data[i + 3]),
            opt(extra_data[i + 4])};
}
extra::FnProto Ast::extraFnProto(ExtraIndex i) const {
    return {extra_data[i],          extra_data[i + 1],      opt(extra_data[i + 2]),
            opt(extra_data[i + 3]), opt(extra_data[i + 4]), opt(extra_data[i + 5])};
}
extra::Asm Ast::extraAsm(ExtraIndex i) const {
    return {extra_data[i], extra_data[i + 1], opt(extra_data[i + 2]), extra_data[i + 3]};
}

std::span<const NodeIndex> Ast::rootDecls() const {
    switch (mode) {
        case Mode::zig: return extraDataSlice(nodeData(0).extra_range());
        // Ensure that the returned slice points into the existing memory of the Ast
        case Mode::zon: return std::span<const NodeIndex>(&nodes[0].data.lhs, 1);
    }
    return {};
}

std::string Ast::renderError(const Error& parse_error) const {
    switch (parse_error.tag) {
        case ErrorTag::asterisk_after_ptr_deref: return "'.*' cannot be followed by '*'; are you missing a space?";
        case ErrorTag::chained_comparison_operators: return "comparison operators cannot be chained";
        case ErrorTag::decl_between_fields: return "declarations are not allowed between container fields";
        case ErrorTag::expected_block:
            return std::string("expected block, found '") + std::string(tokenSymbol(tokenTag(parse_error.token + (parse_error.token_is_prev ? 1u : 0u)))) + "'";
        case ErrorTag::expected_block_or_assignment:
            return std::string("expected block or assignment, found '") + std::string(tokenSymbol(tokenTag(parse_error.token + (parse_error.token_is_prev ? 1u : 0u)))) + "'";
        case ErrorTag::expected_block_or_expr:
            return std::string("expected block or expression, found '") + std::string(tokenSymbol(tokenTag(parse_error.token + (parse_error.token_is_prev ? 1u : 0u)))) + "'";
        case ErrorTag::expected_block_or_field:
            return std::string("expected block or field, found '") + std::string(tokenSymbol(tokenTag(parse_error.token + (parse_error.token_is_prev ? 1u : 0u)))) + "'";
        case ErrorTag::expected_container_members:
            return std::string("expected test, comptime, var decl, or container field, found '") + std::string(tokenSymbol(tokenTag(parse_error.token))) + "'";
        case ErrorTag::expected_expr:
            return std::string("expected expression, found '") + std::string(tokenSymbol(tokenTag(parse_error.token + (parse_error.token_is_prev ? 1u : 0u)))) + "'";
        case ErrorTag::expected_expr_or_assignment:
            return std::string("expected expression or assignment, found '") + std::string(tokenSymbol(tokenTag(parse_error.token + (parse_error.token_is_prev ? 1u : 0u)))) + "'";
        case ErrorTag::expected_expr_or_var_decl:
            return std::string("expected expression or var decl, found '") + std::string(tokenSymbol(tokenTag(parse_error.token + (parse_error.token_is_prev ? 1u : 0u)))) + "'";
        case ErrorTag::expected_fn:
            return std::string("expected function, found '") + std::string(tokenSymbol(tokenTag(parse_error.token + (parse_error.token_is_prev ? 1u : 0u)))) + "'";
        case ErrorTag::expected_inlinable:
            return std::string("expected 'while' or 'for', found '") + std::string(tokenSymbol(tokenTag(parse_error.token + (parse_error.token_is_prev ? 1u : 0u)))) + "'";
        case ErrorTag::expected_labelable:
            return std::string("expected 'while', 'for', 'inline', or '{', found '") + std::string(tokenSymbol(tokenTag(parse_error.token + (parse_error.token_is_prev ? 1u : 0u)))) + "'";
        case ErrorTag::expected_param_list:
            return std::string("expected parameter list, found '") + std::string(tokenSymbol(tokenTag(parse_error.token + (parse_error.token_is_prev ? 1u : 0u)))) + "'";
        case ErrorTag::expected_prefix_expr:
            return std::string("expected prefix expression, found '") + std::string(tokenSymbol(tokenTag(parse_error.token + (parse_error.token_is_prev ? 1u : 0u)))) + "'";
        case ErrorTag::expected_primary_type_expr:
            return std::string("expected primary type expression, found '") + std::string(tokenSymbol(tokenTag(parse_error.token + (parse_error.token_is_prev ? 1u : 0u)))) + "'";
        case ErrorTag::expected_pub_item: return "expected function or variable declaration after pub";
        case ErrorTag::expected_return_type:
            return std::string("expected return type expression, found '") + std::string(tokenSymbol(tokenTag(parse_error.token + (parse_error.token_is_prev ? 1u : 0u)))) + "'";
        case ErrorTag::expected_semi_or_else: return "expected ';' or 'else' after statement";
        case ErrorTag::expected_semi_or_lbrace: return "expected ';' or block after function prototype";
        case ErrorTag::expected_statement:
            return std::string("expected statement, found '") + std::string(tokenSymbol(tokenTag(parse_error.token))) + "'";
        case ErrorTag::expected_suffix_op:
            return std::string("expected pointer dereference, optional unwrap, or field access, found '") + std::string(tokenSymbol(tokenTag(parse_error.token + (parse_error.token_is_prev ? 1u : 0u)))) + "'";
        case ErrorTag::expected_type_expr:
            return std::string("expected type expression, found '") + std::string(tokenSymbol(tokenTag(parse_error.token + (parse_error.token_is_prev ? 1u : 0u)))) + "'";
        case ErrorTag::expected_var_decl:
            return std::string("expected variable declaration, found '") + std::string(tokenSymbol(tokenTag(parse_error.token + (parse_error.token_is_prev ? 1u : 0u)))) + "'";
        case ErrorTag::expected_var_decl_or_fn:
            return std::string("expected variable declaration or function, found '") + std::string(tokenSymbol(tokenTag(parse_error.token + (parse_error.token_is_prev ? 1u : 0u)))) + "'";
        case ErrorTag::expected_loop_payload:
            return std::string("expected loop payload, found '") + std::string(tokenSymbol(tokenTag(parse_error.token + (parse_error.token_is_prev ? 1u : 0u)))) + "'";
        case ErrorTag::expected_container:
            return std::string("expected a struct, enum or union, found '") + std::string(tokenSymbol(tokenTag(parse_error.token + (parse_error.token_is_prev ? 1u : 0u)))) + "'";
        case ErrorTag::extern_fn_body: return "extern functions have no body";
        case ErrorTag::extra_addrspace_qualifier: return "extra addrspace qualifier";
        case ErrorTag::extra_align_qualifier: return "extra align qualifier";
        case ErrorTag::extra_allowzero_qualifier: return "extra allowzero qualifier";
        case ErrorTag::extra_const_qualifier: return "extra const qualifier";
        case ErrorTag::extra_volatile_qualifier: return "extra volatile qualifier";
        case ErrorTag::ptr_mod_on_array_child_type:
            return std::string("pointer modifier '") + std::string(tokenSymbol(tokenTag(parse_error.token))) + "' not allowed on array child type";
        case ErrorTag::invalid_bit_range: return "bit range only allowed on single item pointers";
        case ErrorTag::same_line_doc_comment: return "same line documentation comment";
        case ErrorTag::unattached_doc_comment: return "unattached documentation comment";
        case ErrorTag::test_doc_comment: return "documentation comments cannot be attached to tests";
        case ErrorTag::comptime_doc_comment: return "documentation comments cannot be attached to comptime blocks";
        case ErrorTag::varargs_nonfinal: return "function prototype has parameter after varargs";
        case ErrorTag::expected_semi_after_decl: return "expected ';' after declaration";
        case ErrorTag::expected_semi_after_stmt: return "expected ';' after statement";
        case ErrorTag::expected_comma_after_field: return "expected ',' after field";
        case ErrorTag::expected_comma_after_arg: return "expected ',' after argument";
        case ErrorTag::expected_comma_after_param: return "expected ',' after parameter";
        case ErrorTag::expected_comma_after_initializer: return "expected ',' after initializer";
        case ErrorTag::expected_comma_after_switch_prong: return "expected ',' after switch prong";
        case ErrorTag::expected_comma_after_for_operand: return "expected ',' after for operand";
        case ErrorTag::expected_comma_after_capture: return "expected ',' after for capture";
        case ErrorTag::expected_initializer: return "expected field initializer";
        case ErrorTag::mismatched_binary_op_whitespace:
            return std::string("binary operator '") + std::string(tokenLexeme(tokenTag(parse_error.token))) + "' has whitespace on one side, but not the other";
        case ErrorTag::invalid_ampersand_ampersand: return "ambiguous use of '&&'; use 'and' for logical AND, or change whitespace to ' & &' for bitwise AND";
        case ErrorTag::c_style_container:
            return std::string("'") + std::string(tokenSymbol(parse_error.expected_tag)) + " " + std::string(tokenSlice(parse_error.token)) + "' is invalid";
        case ErrorTag::zig_style_container:
            return std::string("to declare a container do 'const ") + std::string(tokenSlice(parse_error.token)) + " = " + std::string(tokenSymbol(parse_error.expected_tag)) + "'";
        case ErrorTag::previous_field: return "field before declarations here";
        case ErrorTag::next_field: return "field after declarations here";
        case ErrorTag::expected_var_const: return "expected 'var' or 'const' before variable declaration";
        case ErrorTag::wrong_equal_var_decl: return "variable initialized with '==' instead of '='";
        case ErrorTag::var_const_decl: return "use 'var' or 'const' to declare variable";
        case ErrorTag::extra_for_capture: return "extra capture in for loop";
        case ErrorTag::for_input_not_captured: return "for input is not captured";
        case ErrorTag::invalid_byte: {
            std::string_view tok_slice = std::string_view(source).substr(tokenStart(parse_error.token));
            std::string_view kind;
            switch (tok_slice[0]) {
                case '\'': kind = "character literal"; break;
                case '"':
                case '\\': kind = "string literal"; break;
                case '/': kind = "comment"; break;
                default: assert(false);
            }
            // std.zig.fmtChar (charEscape) of one byte.
            const auto c = static_cast<unsigned char>(
                parse_error.offset < tok_slice.size() ? tok_slice[parse_error.offset] : '\0');
            std::string esc;
            switch (c) {
                case '\n': esc = "\\n"; break;
                case '\r': esc = "\\r"; break;
                case '\t': esc = "\\t"; break;
                case '\\': esc = "\\\\"; break;
                case '\'': esc = "\\'"; break;
                default:
                    if (c == '"' || c == ' ' || c == '!' || (c >= '#' && c <= '&') || (c >= '(' && c <= '[') ||
                        (c >= ']' && c <= '~')) {
                        esc = std::string(1, static_cast<char>(c));
                    } else {
                        static const char kHex[] = "0123456789abcdef";
                        esc = "\\x";
                        esc += kHex[c >> 4];
                        esc += kHex[c & 0xF];
                    }
                    break;
            }
            return std::string(kind) + " contains invalid byte: '" + esc + "'";
        }
        case ErrorTag::expected_token: {
            const TokenTag found_tag = tokenTag(parse_error.token + (parse_error.token_is_prev ? 1u : 0u));
            const std::string_view expected_symbol = tokenSymbol(parse_error.expected_tag);
            if (found_tag == TokenTag::invalid)
                return std::string("expected '") + std::string(expected_symbol) + "', found invalid bytes";
            return std::string("expected '") + std::string(expected_symbol) + "', found '" +
                   std::string(tokenSymbol(found_tag)) + "'";
        }
        case ErrorTag::nesting_too_deep:
            // Not a Zig error: see kMaxNesting.
            return "nesting too deep: more than " + std::to_string(kMaxNesting) + " levels";
    }
    return {};
}

TokenIndex Ast::firstToken(NodeIndex node) const {
    std::uint32_t end_offset = 0;
    NodeIndex n = node;
    while (true) {
        switch (nodeTag(n)) {
            case NodeTag::root: return 0;

            case NodeTag::test_decl:
            case NodeTag::errdefer_:
            case NodeTag::defer_:
            case NodeTag::bool_not:
            case NodeTag::negation:
            case NodeTag::bit_not:
            case NodeTag::negation_wrap:
            case NodeTag::address_of:
            case NodeTag::try_:
            case NodeTag::optional_type:
            case NodeTag::switch_:
            case NodeTag::switch_comma:
            case NodeTag::if_simple:
            case NodeTag::if_:
            case NodeTag::suspend_:
            case NodeTag::resume_:
            case NodeTag::continue_:
            case NodeTag::break_:
            case NodeTag::return_:
            case NodeTag::anyframe_type:
            case NodeTag::identifier:
            case NodeTag::anyframe_literal:
            case NodeTag::char_literal:
            case NodeTag::number_literal:
            case NodeTag::unreachable_literal:
            case NodeTag::string_literal:
            case NodeTag::multiline_string_literal:
            case NodeTag::grouped_expression:
            case NodeTag::builtin_call_two:
            case NodeTag::builtin_call_two_comma:
            case NodeTag::builtin_call:
            case NodeTag::builtin_call_comma:
            case NodeTag::error_set_decl:
            case NodeTag::comptime_:
            case NodeTag::nosuspend_:
            case NodeTag::asm_simple:
            case NodeTag::asm_:
            case NodeTag::array_type:
            case NodeTag::array_type_sentinel:
            case NodeTag::error_value: return nodeMainToken(n) - end_offset;

            case NodeTag::array_init_dot:
            case NodeTag::array_init_dot_comma:
            case NodeTag::array_init_dot_two:
            case NodeTag::array_init_dot_two_comma:
            case NodeTag::struct_init_dot:
            case NodeTag::struct_init_dot_comma:
            case NodeTag::struct_init_dot_two:
            case NodeTag::struct_init_dot_two_comma:
            case NodeTag::enum_literal: return nodeMainToken(n) - 1 - end_offset;

            case NodeTag::catch_:
            case NodeTag::equal_equal:
            case NodeTag::bang_equal:
            case NodeTag::less_than:
            case NodeTag::greater_than:
            case NodeTag::less_or_equal:
            case NodeTag::greater_or_equal:
            case NodeTag::assign_mul:
            case NodeTag::assign_div:
            case NodeTag::assign_mod:
            case NodeTag::assign_add:
            case NodeTag::assign_sub:
            case NodeTag::assign_shl:
            case NodeTag::assign_shl_sat:
            case NodeTag::assign_shr:
            case NodeTag::assign_bit_and:
            case NodeTag::assign_bit_xor:
            case NodeTag::assign_bit_or:
            case NodeTag::assign_mul_wrap:
            case NodeTag::assign_add_wrap:
            case NodeTag::assign_sub_wrap:
            case NodeTag::assign_mul_sat:
            case NodeTag::assign_add_sat:
            case NodeTag::assign_sub_sat:
            case NodeTag::assign:
            case NodeTag::merge_error_sets:
            case NodeTag::mul:
            case NodeTag::div:
            case NodeTag::mod:
            case NodeTag::array_mult:
            case NodeTag::mul_wrap:
            case NodeTag::mul_sat:
            case NodeTag::add:
            case NodeTag::sub:
            case NodeTag::array_cat:
            case NodeTag::add_wrap:
            case NodeTag::sub_wrap:
            case NodeTag::add_sat:
            case NodeTag::sub_sat:
            case NodeTag::shl:
            case NodeTag::shl_sat:
            case NodeTag::shr:
            case NodeTag::bit_and:
            case NodeTag::bit_xor:
            case NodeTag::bit_or:
            case NodeTag::orelse_:
            case NodeTag::bool_and:
            case NodeTag::bool_or:
            case NodeTag::slice_open:
            case NodeTag::array_access:
            case NodeTag::array_init_one:
            case NodeTag::array_init_one_comma:
            case NodeTag::switch_range:
            case NodeTag::error_union: n = nodeData(n).node_and_node().first; break;

            case NodeTag::for_range:
            case NodeTag::call_one:
            case NodeTag::call_one_comma:
            case NodeTag::struct_init_one:
            case NodeTag::struct_init_one_comma: n = nodeData(n).node_and_opt_node().first; break;

            case NodeTag::field_access:
            case NodeTag::unwrap_optional: n = nodeData(n).node_and_token().first; break;

            case NodeTag::slice:
            case NodeTag::slice_sentinel:
            case NodeTag::array_init:
            case NodeTag::array_init_comma:
            case NodeTag::struct_init:
            case NodeTag::struct_init_comma:
            case NodeTag::call:
            case NodeTag::call_comma: n = nodeData(n).node_and_extra().first; break;

            case NodeTag::deref: n = nodeData(n).node(); break;

            case NodeTag::assign_destructure: n = assignDestructure(n).ast.variables[0]; break;

            case NodeTag::fn_decl:
            case NodeTag::fn_proto_simple:
            case NodeTag::fn_proto_multi:
            case NodeTag::fn_proto_one:
            case NodeTag::fn_proto: {
                TokenIndex i = nodeMainToken(n);  // fn token
                while (i > 0) {
                    i -= 1;
                    switch (tokenTag(i)) {
                        case TokenTag::keyword_extern:
                        case TokenTag::keyword_export:
                        case TokenTag::keyword_pub:
                        case TokenTag::keyword_inline:
                        case TokenTag::keyword_noinline:
                        case TokenTag::string_literal: continue;
                        default: return i + 1 - end_offset;
                    }
                }
                return i - end_offset;
            }

            case NodeTag::container_field_init:
            case NodeTag::container_field_align:
            case NodeTag::container_field: {
                const TokenIndex name_token = nodeMainToken(n);
                const bool has_comptime_token = isTokenPrecededByTags(name_token, {TokenTag::keyword_comptime});
                end_offset += has_comptime_token ? 1u : 0u;
                return name_token - end_offset;
            }

            case NodeTag::global_var_decl:
            case NodeTag::local_var_decl:
            case NodeTag::simple_var_decl:
            case NodeTag::aligned_var_decl: {
                TokenIndex i = nodeMainToken(n);  // mut token
                while (i > 0) {
                    i -= 1;
                    switch (tokenTag(i)) {
                        case TokenTag::keyword_extern:
                        case TokenTag::keyword_export:
                        case TokenTag::keyword_comptime:
                        case TokenTag::keyword_pub:
                        case TokenTag::keyword_threadlocal:
                        case TokenTag::string_literal: continue;
                        default: return i + 1 - end_offset;
                    }
                }
                return i - end_offset;
            }

            case NodeTag::block:
            case NodeTag::block_semicolon:
            case NodeTag::block_two:
            case NodeTag::block_two_semicolon: {
                // Look for a label.
                const TokenIndex lbrace = nodeMainToken(n);
                if (isTokenPrecededByTags(lbrace, {TokenTag::identifier, TokenTag::colon})) end_offset += 2;
                return lbrace - end_offset;
            }

            case NodeTag::container_decl:
            case NodeTag::container_decl_trailing:
            case NodeTag::container_decl_two:
            case NodeTag::container_decl_two_trailing:
            case NodeTag::container_decl_arg:
            case NodeTag::container_decl_arg_trailing:
            case NodeTag::tagged_union:
            case NodeTag::tagged_union_trailing:
            case NodeTag::tagged_union_two:
            case NodeTag::tagged_union_two_trailing:
            case NodeTag::tagged_union_enum_tag:
            case NodeTag::tagged_union_enum_tag_trailing: {
                const TokenIndex main_token = nodeMainToken(n);
                switch (tokenTag(main_token == 0 ? 0 : main_token - 1)) {
                    case TokenTag::keyword_packed:
                    case TokenTag::keyword_extern: end_offset += 1; break;
                    default: break;
                }
                return main_token - end_offset;
            }

            case NodeTag::ptr_type_aligned:
            case NodeTag::ptr_type_sentinel:
            case NodeTag::ptr_type:
            case NodeTag::ptr_type_bit_range: return nodeMainToken(n) - end_offset;

            case NodeTag::switch_case_one:
            case NodeTag::switch_case_inline_one:
            case NodeTag::switch_case:
            case NodeTag::switch_case_inline: {
                const full::SwitchCase full_switch = *fullSwitchCase(n);
                if (full_switch.inline_token) return *full_switch.inline_token;
                if (full_switch.ast.values.empty()) return full_switch.ast.arrow_token - 1 - end_offset;  // else token
                n = full_switch.ast.values[0];
                break;
            }

            case NodeTag::asm_output:
            case NodeTag::asm_input:
                assert(tokenTag(nodeMainToken(n) - 1) == TokenTag::l_bracket);
                return nodeMainToken(n) - 1 - end_offset;

            case NodeTag::while_simple:
            case NodeTag::while_cont:
            case NodeTag::while_:
            case NodeTag::for_simple:
            case NodeTag::for_: {
                // Look for a label and inline.
                const TokenIndex main_token = nodeMainToken(n);
                TokenIndex result = main_token;
                if (isTokenPrecededByTags(result, {TokenTag::keyword_inline})) result = result - 1;
                if (isTokenPrecededByTags(result, {TokenTag::identifier, TokenTag::colon})) result = result - 2;
                return result - end_offset;
            }
        }
    }
}

TokenIndex Ast::lastToken(NodeIndex node) const {
    NodeIndex n = node;
    std::uint32_t end_offset = 0;
    while (true) {
        switch (nodeTag(n)) {
            case NodeTag::root: return static_cast<TokenIndex>(tokens.size() - 1);

            case NodeTag::bool_not:
            case NodeTag::negation:
            case NodeTag::bit_not:
            case NodeTag::negation_wrap:
            case NodeTag::address_of:
            case NodeTag::try_:
            case NodeTag::optional_type:
            case NodeTag::suspend_:
            case NodeTag::resume_:
            case NodeTag::nosuspend_:
            case NodeTag::comptime_: n = nodeData(n).node(); break;

            case NodeTag::catch_:
            case NodeTag::equal_equal:
            case NodeTag::bang_equal:
            case NodeTag::less_than:
            case NodeTag::greater_than:
            case NodeTag::less_or_equal:
            case NodeTag::greater_or_equal:
            case NodeTag::assign_mul:
            case NodeTag::assign_div:
            case NodeTag::assign_mod:
            case NodeTag::assign_add:
            case NodeTag::assign_sub:
            case NodeTag::assign_shl:
            case NodeTag::assign_shl_sat:
            case NodeTag::assign_shr:
            case NodeTag::assign_bit_and:
            case NodeTag::assign_bit_xor:
            case NodeTag::assign_bit_or:
            case NodeTag::assign_mul_wrap:
            case NodeTag::assign_add_wrap:
            case NodeTag::assign_sub_wrap:
            case NodeTag::assign_mul_sat:
            case NodeTag::assign_add_sat:
            case NodeTag::assign_sub_sat:
            case NodeTag::assign:
            case NodeTag::merge_error_sets:
            case NodeTag::mul:
            case NodeTag::div:
            case NodeTag::mod:
            case NodeTag::array_mult:
            case NodeTag::mul_wrap:
            case NodeTag::mul_sat:
            case NodeTag::add:
            case NodeTag::sub:
            case NodeTag::array_cat:
            case NodeTag::add_wrap:
            case NodeTag::sub_wrap:
            case NodeTag::add_sat:
            case NodeTag::sub_sat:
            case NodeTag::shl:
            case NodeTag::shl_sat:
            case NodeTag::shr:
            case NodeTag::bit_and:
            case NodeTag::bit_xor:
            case NodeTag::bit_or:
            case NodeTag::orelse_:
            case NodeTag::bool_and:
            case NodeTag::bool_or:
            case NodeTag::error_union:
            case NodeTag::if_simple:
            case NodeTag::while_simple:
            case NodeTag::for_simple:
            case NodeTag::fn_decl:
            case NodeTag::array_type:
            case NodeTag::switch_range: n = nodeData(n).node_and_node().second; break;

            case NodeTag::test_decl:
            case NodeTag::errdefer_: n = nodeData(n).opt_token_and_node().second; break;
            case NodeTag::defer_: n = nodeData(n).node(); break;
            case NodeTag::anyframe_type: n = nodeData(n).token_and_node().second; break;

            case NodeTag::switch_case_one:
            case NodeTag::switch_case_inline_one:
            case NodeTag::ptr_type_aligned:
            case NodeTag::ptr_type_sentinel: n = nodeData(n).opt_node_and_node().second; break;

            case NodeTag::assign_destructure:
            case NodeTag::ptr_type:
            case NodeTag::ptr_type_bit_range:
            case NodeTag::switch_case:
            case NodeTag::switch_case_inline: n = nodeData(n).extra_and_node().second; break;

            case NodeTag::fn_proto_simple: n = nodeData(n).opt_node_and_opt_node().second.unwrap(); break;
            case NodeTag::fn_proto_multi:
            case NodeTag::fn_proto_one:
            case NodeTag::fn_proto: n = nodeData(n).extra_and_opt_node().second.unwrap(); break;

            case NodeTag::for_range: {
                const OptionalIndex rhs = nodeData(n).node_and_opt_node().second;
                if (rhs.isNone()) return nodeMainToken(n) + end_offset;
                n = rhs.value;
                break;
            }

            case NodeTag::field_access:
            case NodeTag::unwrap_optional:
            case NodeTag::asm_simple: return nodeData(n).node_and_token().second + end_offset;
            case NodeTag::grouped_expression:
            case NodeTag::asm_input: return nodeData(n).node_and_token().second + end_offset;
            case NodeTag::multiline_string_literal:
            case NodeTag::error_set_decl: return nodeData(n).token_and_token().second + end_offset;
            case NodeTag::asm_output: return nodeData(n).opt_node_and_token().second + end_offset;
            case NodeTag::error_value: return nodeMainToken(n) + 2 + end_offset;

            case NodeTag::anyframe_literal:
            case NodeTag::char_literal:
            case NodeTag::number_literal:
            case NodeTag::unreachable_literal:
            case NodeTag::identifier:
            case NodeTag::deref:
            case NodeTag::enum_literal:
            case NodeTag::string_literal: return nodeMainToken(n) + end_offset;

            case NodeTag::return_: {
                const OptionalIndex child = nodeData(n).opt_node();
                if (child.isNone()) return nodeMainToken(n) + end_offset;
                n = child.value;
                break;
            }

            case NodeTag::call: {
                const SubRange params = extraSubRange(nodeData(n).node_and_extra().second);
                assert(params.start != params.end);
                end_offset += 1;  // for the rparen
                n = extra_data[params.end - 1];  // last parameter
                break;
            }
            case NodeTag::tagged_union_enum_tag: {
                const auto [arg, extra_index] = nodeData(n).node_and_extra();
                const SubRange members = extraSubRange(extra_index);
                if (members.start == members.end) {
                    end_offset += 4;  // for the rparen + rparen + lbrace + rbrace
                    n = arg;
                } else {
                    end_offset += 1;  // for the rbrace
                    n = extra_data[members.end - 1];  // last parameter
                }
                break;
            }
            case NodeTag::call_comma:
            case NodeTag::tagged_union_enum_tag_trailing: {
                const SubRange params = extraSubRange(nodeData(n).node_and_extra().second);
                assert(params.start != params.end);
                end_offset += 2;  // for the comma/semicolon + rparen/rbrace
                n = extra_data[params.end - 1];  // last parameter
                break;
            }
            case NodeTag::switch_: {
                const auto [condition, extra_index] = nodeData(n).node_and_extra();
                const SubRange cases = extraSubRange(extra_index);
                if (cases.start == cases.end) {
                    end_offset += 3;  // rparen, lbrace, rbrace
                    n = condition;
                } else {
                    end_offset += 1;  // for the rbrace
                    n = extra_data[cases.end - 1];  // last case
                }
                break;
            }
            case NodeTag::container_decl_arg: {
                const auto [arg, extra_index] = nodeData(n).node_and_extra();
                const SubRange members = extraSubRange(extra_index);
                if (members.end == members.start) {
                    end_offset += 3;  // for the rparen + lbrace + rbrace
                    n = arg;
                } else {
                    end_offset += 1;  // for the rbrace
                    n = extra_data[members.end - 1];  // last parameter
                }
                break;
            }
            case NodeTag::asm_: {
                const extra::Asm e = extraAsm(nodeData(n).node_and_extra().second);
                return e.rparen + end_offset;
            }
            case NodeTag::array_init:
            case NodeTag::struct_init: {
                const SubRange elements = extraSubRange(nodeData(n).node_and_extra().second);
                assert(elements.start != elements.end);
                end_offset += 1;  // for the rbrace
                n = extra_data[elements.end - 1];  // last element
                break;
            }
            case NodeTag::array_init_comma:
            case NodeTag::struct_init_comma:
            case NodeTag::container_decl_arg_trailing:
            case NodeTag::switch_comma: {
                const SubRange members = extraSubRange(nodeData(n).node_and_extra().second);
                assert(members.start != members.end);
                end_offset += 2;  // for the comma + rbrace
                n = extra_data[members.end - 1];  // last parameter
                break;
            }
            case NodeTag::array_init_dot:
            case NodeTag::struct_init_dot:
            case NodeTag::block:
            case NodeTag::container_decl:
            case NodeTag::tagged_union:
            case NodeTag::builtin_call: {
                const SubRange range = nodeData(n).extra_range();
                assert(range.start != range.end);
                end_offset += 1;  // for the rbrace
                n = extra_data[range.end - 1];  // last statement
                break;
            }
            case NodeTag::array_init_dot_comma:
            case NodeTag::struct_init_dot_comma:
            case NodeTag::block_semicolon:
            case NodeTag::container_decl_trailing:
            case NodeTag::tagged_union_trailing:
            case NodeTag::builtin_call_comma: {
                const SubRange range = nodeData(n).extra_range();
                assert(range.start != range.end);
                end_offset += 2;  // for the comma/semicolon + rbrace/rparen
                n = extra_data[range.end - 1];  // last member
                break;
            }
            case NodeTag::call_one: {
                const OptionalIndex first_param = nodeData(n).node_and_opt_node().second;
                end_offset += 1;  // for the rparen
                if (first_param.isNone()) return nodeMainToken(n) + end_offset;
                n = first_param.value;
                break;
            }

            case NodeTag::array_init_dot_two:
            case NodeTag::block_two:
            case NodeTag::builtin_call_two:
            case NodeTag::struct_init_dot_two:
            case NodeTag::container_decl_two:
            case NodeTag::tagged_union_two: {
                const auto [opt_lhs, opt_rhs] = nodeData(n).opt_node_and_opt_node();
                if (!opt_rhs.isNone()) {
                    end_offset += 1;  // for the rparen/rbrace
                    n = opt_rhs.value;
                } else if (!opt_lhs.isNone()) {
                    end_offset += 1;  // for the rparen/rbrace
                    n = opt_lhs.value;
                } else {
                    switch (nodeTag(n)) {
                        case NodeTag::array_init_dot_two:
                        case NodeTag::block_two:
                        case NodeTag::struct_init_dot_two: end_offset += 1; break;  // rbrace
                        case NodeTag::builtin_call_two: end_offset += 2; break;  // lparen/lbrace + rparen/rbrace
                        case NodeTag::container_decl_two: {
                            std::uint32_t i = 2;  // lbrace + rbrace
                            while (tokenTag(nodeMainToken(n) + i) == TokenTag::container_doc_comment) i += 1;
                            end_offset += i;
                            break;
                        }
                        case NodeTag::tagged_union_two: {
                            std::uint32_t i = 5;  // (enum) {}
                            while (tokenTag(nodeMainToken(n) + i) == TokenTag::container_doc_comment) i += 1;
                            end_offset += i;
                            break;
                        }
                        default: assert(false);
                    }
                    return nodeMainToken(n) + end_offset;
                }
                break;
            }
            case NodeTag::array_init_dot_two_comma:
            case NodeTag::builtin_call_two_comma:
            case NodeTag::block_two_semicolon:
            case NodeTag::struct_init_dot_two_comma:
            case NodeTag::container_decl_two_trailing:
            case NodeTag::tagged_union_two_trailing: {
                const auto [opt_lhs, opt_rhs] = nodeData(n).opt_node_and_opt_node();
                end_offset += 2;  // for the comma/semicolon + rbrace/rparen
                if (!opt_rhs.isNone()) {
                    n = opt_rhs.value;
                } else {
                    assert(!opt_lhs.isNone());
                    n = opt_lhs.value;
                }
                break;
            }
            case NodeTag::simple_var_decl: {
                const auto [type_node, init_node] = nodeData(n).opt_node_and_opt_node();
                if (!init_node.isNone()) {
                    n = init_node.value;
                } else if (!type_node.isNone()) {
                    n = type_node.value;
                } else {
                    end_offset += 1;  // from mut token to name
                    return nodeMainToken(n) + end_offset;
                }
                break;
            }
            case NodeTag::aligned_var_decl: {
                const auto [align_node, init_node] = nodeData(n).node_and_opt_node();
                if (!init_node.isNone()) {
                    n = init_node.value;
                } else {
                    end_offset += 1;  // for the rparen
                    n = align_node;
                }
                break;
            }
            case NodeTag::global_var_decl: {
                const auto [extra_index, init_node] = nodeData(n).extra_and_opt_node();
                if (!init_node.isNone()) {
                    n = init_node.value;
                } else {
                    const extra::GlobalVarDecl e = extraGlobalVarDecl(extra_index);
                    if (!e.section_node.isNone()) {
                        end_offset += 1;  // for the rparen
                        n = e.section_node.value;
                    } else if (!e.addrspace_node.isNone()) {
                        end_offset += 1;  // for the rparen
                        n = e.addrspace_node.value;
                    } else if (!e.align_node.isNone()) {
                        end_offset += 1;  // for the rparen
                        n = e.align_node.value;
                    } else if (!e.type_node.isNone()) {
                        n = e.type_node.value;
                    } else {
                        end_offset += 1;  // from mut token to name
                        return nodeMainToken(n) + end_offset;
                    }
                }
                break;
            }
            case NodeTag::local_var_decl: {
                const auto [extra_index, init_node] = nodeData(n).extra_and_opt_node();
                if (!init_node.isNone()) {
                    n = init_node.value;
                } else {
                    const extra::LocalVarDecl e = extraLocalVarDecl(extra_index);
                    end_offset += 1;  // for the rparen
                    n = e.align_node;
                }
                break;
            }
            case NodeTag::container_field_init: {
                const auto [type_expr, value_expr] = nodeData(n).node_and_opt_node();
                n = value_expr.isNone() ? type_expr : value_expr.value;
                break;
            }

            case NodeTag::array_access:
            case NodeTag::array_init_one:
            case NodeTag::container_field_align:
                end_offset += 1;  // for the rbracket/rbrace/rparen
                n = nodeData(n).node_and_node().second;
                break;
            case NodeTag::container_field: n = extraContainerField(nodeData(n).node_and_extra().second).value_expr; break;

            case NodeTag::struct_init_one: {
                const OptionalIndex first_field = nodeData(n).node_and_opt_node().second;
                end_offset += 1;  // rbrace
                if (first_field.isNone()) return nodeMainToken(n) + end_offset;
                n = first_field.value;
                break;
            }
            case NodeTag::slice_open:
                end_offset += 2;  // ellipsis2 + rbracket, or comma + rparen
                n = nodeData(n).node_and_node().second;
                break;
            case NodeTag::array_init_one_comma:
                end_offset += 2;  // comma + rbrace
                n = nodeData(n).node_and_node().second;
                break;
            case NodeTag::call_one_comma:
            case NodeTag::struct_init_one_comma:
                end_offset += 2;  // ellipsis2 + rbracket, or comma + rparen
                n = nodeData(n).node_and_opt_node().second.unwrap();
                break;
            case NodeTag::slice:
                end_offset += 1;  // rbracket
                n = extraSlice(nodeData(n).node_and_extra().second).end;
                break;
            case NodeTag::slice_sentinel:
                end_offset += 1;  // rbracket
                n = extraSliceSentinel(nodeData(n).node_and_extra().second).sentinel;
                break;

            case NodeTag::continue_:
            case NodeTag::break_: {
                const auto [opt_label, opt_rhs] = nodeData(n).opt_token_and_opt_node();
                if (!opt_rhs.isNone()) {
                    n = opt_rhs.value;
                } else if (!opt_label.isNone()) {
                    return opt_label.value + end_offset;
                } else {
                    return nodeMainToken(n) + end_offset;
                }
                break;
            }
            case NodeTag::while_cont: n = extraWhileCont(nodeData(n).node_and_extra().second).then_expr; break;
            case NodeTag::while_: n = extraWhile(nodeData(n).node_and_extra().second).else_expr; break;
            case NodeTag::if_: n = extraIf(nodeData(n).node_and_extra().second).else_expr; break;
            case NodeTag::for_: {
                const auto [extra_index, f] = nodeData(n).for_();
                const std::uint32_t index = extra_index + f.inputs + (f.has_else ? 1u : 0u);
                n = extra_data[index];
                break;
            }
            case NodeTag::array_type_sentinel:
                n = extraArrayTypeSentinel(nodeData(n).node_and_extra().second).elem_type;
                break;
        }
    }
}

bool Ast::tokensOnSameLine(TokenIndex token1, TokenIndex token2) const {
    std::string_view s = std::string_view(source).substr(tokenStart(token1), tokenStart(token2) - tokenStart(token1));
    return s.find('\n') == std::string_view::npos;
}

std::string_view Ast::getNodeSource(NodeIndex node) const {
    const TokenIndex first_token = firstToken(node);
    const TokenIndex last_token = lastToken(node);
    const std::size_t start = tokenStart(first_token);
    const std::size_t end = tokenStart(last_token) + tokenSlice(last_token).size();
    return std::string_view(source).substr(start, end - start);
}

full::VarDecl Ast::globalVarDecl(NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::global_var_decl);
    const auto [extra_index, init_node] = nodeData(node).extra_and_opt_node();
    const extra::GlobalVarDecl e = extraGlobalVarDecl(extra_index);
    return fullVarDeclComponents(
        {nodeMainToken(node), e.type_node, e.align_node, e.addrspace_node, e.section_node, init_node});
}

full::VarDecl Ast::localVarDecl(NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::local_var_decl);
    const auto [extra_index, init_node] = nodeData(node).extra_and_opt_node();
    const extra::LocalVarDecl e = extraLocalVarDecl(extra_index);
    return fullVarDeclComponents({nodeMainToken(node), opt(e.type_node), opt(e.align_node), OptionalIndex::none(),
                                  OptionalIndex::none(), init_node});
}

full::VarDecl Ast::simpleVarDecl(NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::simple_var_decl);
    const auto [type_node, init_node] = nodeData(node).opt_node_and_opt_node();
    return fullVarDeclComponents({nodeMainToken(node), type_node, OptionalIndex::none(), OptionalIndex::none(),
                                  OptionalIndex::none(), init_node});
}

full::VarDecl Ast::alignedVarDecl(NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::aligned_var_decl);
    const auto [align_node, init_node] = nodeData(node).node_and_opt_node();
    return fullVarDeclComponents({nodeMainToken(node), OptionalIndex::none(), opt(align_node), OptionalIndex::none(),
                                  OptionalIndex::none(), init_node});
}

full::AssignDestructure Ast::assignDestructure(NodeIndex node) const {
    const auto [extra_index, value_expr] = nodeData(node).extra_and_node();
    const std::uint32_t variable_count = extra_data[extra_index];
    return fullAssignDestructureComponents(
        {extraDataSliceWithLen(extra_index + 1, variable_count), nodeMainToken(node), value_expr});
}

full::If Ast::ifSimple(NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::if_simple);
    const auto [cond_expr, then_expr] = nodeData(node).node_and_node();
    return fullIfComponents({nodeMainToken(node), cond_expr, then_expr, OptionalIndex::none()});
}

full::If Ast::ifFull(NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::if_);
    const auto [cond_expr, extra_index] = nodeData(node).node_and_extra();
    const extra::If e = extraIf(extra_index);
    return fullIfComponents({nodeMainToken(node), cond_expr, e.then_expr, opt(e.else_expr)});
}

namespace {
bool fieldTupleLike(const Ast& tree, TokenIndex main_token) {
    return tree.tokenTag(main_token) != TokenTag::identifier || tree.tokenTag(main_token + 1) != TokenTag::colon;
}
}  // namespace

full::ContainerField Ast::containerField(NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::container_field);
    const auto [type_expr, extra_index] = nodeData(node).node_and_extra();
    const extra::ContainerField e = extraContainerField(extra_index);
    const TokenIndex main_token = nodeMainToken(node);
    return fullContainerFieldComponents(
        {main_token, opt(type_expr), opt(e.align_expr), opt(e.value_expr), fieldTupleLike(*this, main_token)});
}

full::ContainerField Ast::containerFieldInit(NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::container_field_init);
    const auto [type_expr, value_expr] = nodeData(node).node_and_opt_node();
    const TokenIndex main_token = nodeMainToken(node);
    return fullContainerFieldComponents(
        {main_token, opt(type_expr), OptionalIndex::none(), value_expr, fieldTupleLike(*this, main_token)});
}

full::ContainerField Ast::containerFieldAlign(NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::container_field_align);
    const auto [type_expr, align_expr] = nodeData(node).node_and_node();
    const TokenIndex main_token = nodeMainToken(node);
    return fullContainerFieldComponents(
        {main_token, opt(type_expr), opt(align_expr), OptionalIndex::none(), fieldTupleLike(*this, main_token)});
}

full::FnProto Ast::fnProtoSimple(NodeBuffer1& buffer, NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::fn_proto_simple);
    const auto [first_param, return_type] = nodeData(node).opt_node_and_opt_node();
    const auto params = loadOptionalNodesIntoBuffer<1>(buffer.data(), {first_param.value});
    return fullFnProtoComponents({node, nodeMainToken(node), return_type, params, OptionalIndex::none(),
                                  OptionalIndex::none(), OptionalIndex::none(), OptionalIndex::none()});
}

full::FnProto Ast::fnProtoMulti(NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::fn_proto_multi);
    const auto [extra_index, return_type] = nodeData(node).extra_and_opt_node();
    const auto params = extraDataSlice(extraSubRange(extra_index));
    return fullFnProtoComponents({node, nodeMainToken(node), return_type, params, OptionalIndex::none(),
                                  OptionalIndex::none(), OptionalIndex::none(), OptionalIndex::none()});
}

full::FnProto Ast::fnProtoOne(NodeBuffer1& buffer, NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::fn_proto_one);
    const auto [extra_index, return_type] = nodeData(node).extra_and_opt_node();
    const extra::FnProtoOne e = extraFnProtoOne(extra_index);
    const auto params = loadOptionalNodesIntoBuffer<1>(buffer.data(), {e.param.value});
    return fullFnProtoComponents({node, nodeMainToken(node), return_type, params, e.align_expr, e.addrspace_expr,
                                  e.section_expr, e.callconv_expr});
}

full::FnProto Ast::fnProto(NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::fn_proto);
    const auto [extra_index, return_type] = nodeData(node).extra_and_opt_node();
    const extra::FnProto e = extraFnProto(extra_index);
    const auto params = extraDataSlice(SubRange{e.params_start, e.params_end});
    return fullFnProtoComponents({node, nodeMainToken(node), return_type, params, e.align_expr, e.addrspace_expr,
                                  e.section_expr, e.callconv_expr});
}

full::StructInit Ast::structInitOne(NodeIndex* buffer, NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::struct_init_one || nodeTag(node) == NodeTag::struct_init_one_comma);
    const auto [type_expr, first_field] = nodeData(node).node_and_opt_node();
    const auto fields = loadOptionalNodesIntoBuffer<1>(buffer, {first_field.value});
    return full::StructInit{{nodeMainToken(node), fields, opt(type_expr)}};
}

full::StructInit Ast::structInitDotTwo(NodeBuffer2& buffer, NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::struct_init_dot_two || nodeTag(node) == NodeTag::struct_init_dot_two_comma);
    const Data d = nodeData(node);
    const auto fields = loadOptionalNodesIntoBuffer<2>(buffer.data(), {d.lhs, d.rhs});
    return full::StructInit{{nodeMainToken(node), fields, OptionalIndex::none()}};
}

full::StructInit Ast::structInitDot(NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::struct_init_dot || nodeTag(node) == NodeTag::struct_init_dot_comma);
    const auto fields = extraDataSlice(nodeData(node).extra_range());
    return full::StructInit{{nodeMainToken(node), fields, OptionalIndex::none()}};
}

full::StructInit Ast::structInit(NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::struct_init || nodeTag(node) == NodeTag::struct_init_comma);
    const auto [type_expr, extra_index] = nodeData(node).node_and_extra();
    const auto fields = extraDataSlice(extraSubRange(extra_index));
    return full::StructInit{{nodeMainToken(node), fields, opt(type_expr)}};
}

full::ArrayInit Ast::arrayInitOne(NodeIndex* buffer, NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::array_init_one || nodeTag(node) == NodeTag::array_init_one_comma);
    const auto [type_expr, first] = nodeData(node).node_and_node();
    buffer[0] = first;
    return full::ArrayInit{{nodeMainToken(node), std::span<const NodeIndex>(buffer, 1), opt(type_expr)}};
}

full::ArrayInit Ast::arrayInitDotTwo(NodeBuffer2& buffer, NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::array_init_dot_two || nodeTag(node) == NodeTag::array_init_dot_two_comma);
    const Data d = nodeData(node);
    const auto elements = loadOptionalNodesIntoBuffer<2>(buffer.data(), {d.lhs, d.rhs});
    return full::ArrayInit{{nodeMainToken(node), elements, OptionalIndex::none()}};
}

full::ArrayInit Ast::arrayInitDot(NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::array_init_dot || nodeTag(node) == NodeTag::array_init_dot_comma);
    const auto elements = extraDataSlice(nodeData(node).extra_range());
    return full::ArrayInit{{nodeMainToken(node), elements, OptionalIndex::none()}};
}

full::ArrayInit Ast::arrayInit(NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::array_init || nodeTag(node) == NodeTag::array_init_comma);
    const auto [type_expr, extra_index] = nodeData(node).node_and_extra();
    const auto elements = extraDataSlice(extraSubRange(extra_index));
    return full::ArrayInit{{nodeMainToken(node), elements, opt(type_expr)}};
}

full::ArrayType Ast::arrayType(NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::array_type);
    const auto [elem_count, elem_type] = nodeData(node).node_and_node();
    return full::ArrayType{{nodeMainToken(node), elem_count, OptionalIndex::none(), elem_type}};
}

full::ArrayType Ast::arrayTypeSentinel(NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::array_type_sentinel);
    const auto [elem_count, extra_index] = nodeData(node).node_and_extra();
    const extra::ArrayTypeSentinel e = extraArrayTypeSentinel(extra_index);
    return full::ArrayType{{nodeMainToken(node), elem_count, opt(e.sentinel), e.elem_type}};
}

full::PtrType Ast::ptrTypeAligned(NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::ptr_type_aligned);
    const auto [align_node, child_type] = nodeData(node).opt_node_and_node();
    return fullPtrTypeComponents({nodeMainToken(node), align_node, OptionalIndex::none(), OptionalIndex::none(),
                                  OptionalIndex::none(), OptionalIndex::none(), child_type});
}

full::PtrType Ast::ptrTypeSentinel(NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::ptr_type_sentinel);
    const auto [sentinel, child_type] = nodeData(node).opt_node_and_node();
    return fullPtrTypeComponents({nodeMainToken(node), OptionalIndex::none(), OptionalIndex::none(), sentinel,
                                  OptionalIndex::none(), OptionalIndex::none(), child_type});
}

full::PtrType Ast::ptrType(NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::ptr_type);
    const auto [extra_index, child_type] = nodeData(node).extra_and_node();
    const extra::PtrType e = extraPtrType(extra_index);
    return fullPtrTypeComponents({nodeMainToken(node), e.align_node, e.addrspace_node, e.sentinel,
                                  OptionalIndex::none(), OptionalIndex::none(), child_type});
}

full::PtrType Ast::ptrTypeBitRange(NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::ptr_type_bit_range);
    const auto [extra_index, child_type] = nodeData(node).extra_and_node();
    const extra::PtrTypeBitRange e = extraPtrTypeBitRange(extra_index);
    return fullPtrTypeComponents({nodeMainToken(node), opt(e.align_node), e.addrspace_node, e.sentinel,
                                  opt(e.bit_range_start), opt(e.bit_range_end), child_type});
}

full::Slice Ast::sliceOpen(NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::slice_open);
    const auto [sliced, start] = nodeData(node).node_and_node();
    return full::Slice{{sliced, nodeMainToken(node), start, OptionalIndex::none(), OptionalIndex::none()}};
}

full::Slice Ast::slice(NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::slice);
    const auto [sliced, extra_index] = nodeData(node).node_and_extra();
    const extra::Slice e = extraSlice(extra_index);
    return full::Slice{{sliced, nodeMainToken(node), e.start, opt(e.end), OptionalIndex::none()}};
}

full::Slice Ast::sliceSentinel(NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::slice_sentinel);
    const auto [sliced, extra_index] = nodeData(node).node_and_extra();
    const extra::SliceSentinel e = extraSliceSentinel(extra_index);
    return full::Slice{{sliced, nodeMainToken(node), e.start, e.end, opt(e.sentinel)}};
}

full::ContainerDecl Ast::containerDeclTwo(NodeBuffer2& buffer, NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::container_decl_two || nodeTag(node) == NodeTag::container_decl_two_trailing);
    const Data d = nodeData(node);
    const auto members = loadOptionalNodesIntoBuffer<2>(buffer.data(), {d.lhs, d.rhs});
    return fullContainerDeclComponents({nodeMainToken(node), std::nullopt, members, OptionalIndex::none()});
}

full::ContainerDecl Ast::containerDecl(NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::container_decl || nodeTag(node) == NodeTag::container_decl_trailing);
    const auto members = extraDataSlice(nodeData(node).extra_range());
    return fullContainerDeclComponents({nodeMainToken(node), std::nullopt, members, OptionalIndex::none()});
}

full::ContainerDecl Ast::containerDeclArg(NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::container_decl_arg || nodeTag(node) == NodeTag::container_decl_arg_trailing);
    const auto [arg, extra_index] = nodeData(node).node_and_extra();
    const auto members = extraDataSlice(extraSubRange(extra_index));
    return fullContainerDeclComponents({nodeMainToken(node), std::nullopt, members, opt(arg)});
}

full::ContainerDecl Ast::containerDeclRoot() const {
    return full::ContainerDecl{std::nullopt, {0, std::nullopt, rootDecls(), OptionalIndex::none()}};
}

full::ContainerDecl Ast::taggedUnionTwo(NodeBuffer2& buffer, NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::tagged_union_two || nodeTag(node) == NodeTag::tagged_union_two_trailing);
    const Data d = nodeData(node);
    const auto members = loadOptionalNodesIntoBuffer<2>(buffer.data(), {d.lhs, d.rhs});
    const TokenIndex main_token = nodeMainToken(node);
    return fullContainerDeclComponents({main_token, main_token + 2, members, OptionalIndex::none()});
}

full::ContainerDecl Ast::taggedUnion(NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::tagged_union || nodeTag(node) == NodeTag::tagged_union_trailing);
    const auto members = extraDataSlice(nodeData(node).extra_range());
    const TokenIndex main_token = nodeMainToken(node);
    return fullContainerDeclComponents({main_token, main_token + 2, members, OptionalIndex::none()});
}

full::ContainerDecl Ast::taggedUnionEnumTag(NodeIndex node) const {
    assert(nodeTag(node) == NodeTag::tagged_union_enum_tag ||
           nodeTag(node) == NodeTag::tagged_union_enum_tag_trailing);
    const auto [arg, extra_index] = nodeData(node).node_and_extra();
    const auto members = extraDataSlice(extraSubRange(extra_index));
    const TokenIndex main_token = nodeMainToken(node);
    return fullContainerDeclComponents({main_token, main_token + 2, members, opt(arg)});
}

full::Switch Ast::switchFull(NodeIndex node) const {
    const TokenIndex main_token = nodeMainToken(node);
    TokenIndex switch_token = main_token;
    std::optional<TokenIndex> label_token;
    switch (tokenTag(main_token)) {
        case TokenTag::identifier:
            switch_token = main_token + 2;
            label_token = main_token;
            break;
        case TokenTag::keyword_switch: break;
        default: assert(false);
    }
    const auto [condition, extra_index] = nodeData(node).node_and_extra();
    const auto cases = extraDataSlice(extraSubRange(extra_index));
    return full::Switch{{switch_token, condition, cases}, label_token};
}

full::SwitchCase Ast::switchCaseOne(NodeIndex node) const {
    const auto [first_value, target_expr] = nodeData(node).opt_node_and_node();
    // Ensure that the returned slice points into the existing memory of the Ast
    const std::span<const NodeIndex> values =
        first_value.isNone() ? std::span<const NodeIndex>() : std::span<const NodeIndex>(&nodes[node].data.lhs, 1);
    return fullSwitchCaseComponents({values, nodeMainToken(node), target_expr}, node);
}

full::SwitchCase Ast::switchCase(NodeIndex node) const {
    const auto [extra_index, target_expr] = nodeData(node).extra_and_node();
    const auto values = extraDataSlice(extraSubRange(extra_index));
    return fullSwitchCaseComponents({values, nodeMainToken(node), target_expr}, node);
}

full::Asm Ast::asmSimple(NodeIndex node) const {
    const auto [template_, rparen] = nodeData(node).node_and_token();
    return fullAsmComponents({nodeMainToken(node), template_, {}, OptionalIndex::none(), rparen});
}

full::Asm Ast::asmFull(NodeIndex node) const {
    const auto [template_, extra_index] = nodeData(node).node_and_extra();
    const extra::Asm e = extraAsm(extra_index);
    const auto items = extraDataSlice(SubRange{e.items_start, e.items_end});
    return fullAsmComponents({nodeMainToken(node), template_, items, e.clobbers, e.rparen});
}

full::While Ast::whileSimple(NodeIndex node) const {
    const auto [cond_expr, then_expr] = nodeData(node).node_and_node();
    return fullWhileComponents(
        {nodeMainToken(node), cond_expr, OptionalIndex::none(), then_expr, OptionalIndex::none()});
}

full::While Ast::whileCont(NodeIndex node) const {
    const auto [cond_expr, extra_index] = nodeData(node).node_and_extra();
    const extra::WhileCont e = extraWhileCont(extra_index);
    return fullWhileComponents({nodeMainToken(node), cond_expr, opt(e.cont_expr), e.then_expr, OptionalIndex::none()});
}

full::While Ast::whileFull(NodeIndex node) const {
    const auto [cond_expr, extra_index] = nodeData(node).node_and_extra();
    const extra::While e = extraWhile(extra_index);
    return fullWhileComponents({nodeMainToken(node), cond_expr, e.cont_expr, e.then_expr, opt(e.else_expr)});
}

full::For Ast::forSimple(NodeIndex node) const {
    const Data& data = nodes[node].data;
    return fullForComponents(
        {nodeMainToken(node), std::span<const NodeIndex>(&data.lhs, 1), data.rhs, OptionalIndex::none()});
}

full::For Ast::forFull(NodeIndex node) const {
    const auto [extra_index, f] = nodeData(node).for_();
    const auto inputs = extraDataSliceWithLen(extra_index, f.inputs);
    const NodeIndex then_expr = extra_data[extra_index + f.inputs];
    const OptionalIndex else_expr = f.has_else ? opt(extra_data[extra_index + f.inputs + 1]) : OptionalIndex::none();
    return fullForComponents({nodeMainToken(node), inputs, then_expr, else_expr});
}

full::Call Ast::callOne(NodeBuffer1& buffer, NodeIndex node) const {
    const auto [fn_expr, first_param] = nodeData(node).node_and_opt_node();
    const auto params = loadOptionalNodesIntoBuffer<1>(buffer.data(), {first_param.value});
    return full::Call{{nodeMainToken(node), fn_expr, params}};
}

full::Call Ast::callFull(NodeIndex node) const {
    const auto [fn_expr, extra_index] = nodeData(node).node_and_extra();
    const auto params = extraDataSlice(extraSubRange(extra_index));
    return full::Call{{nodeMainToken(node), fn_expr, params}};
}

full::VarDecl Ast::fullVarDeclComponents(const full::VarDecl::Components& info) const {
    full::VarDecl result{};
    result.ast = info;
    TokenIndex i = info.mut_token;
    while (i > 0) {
        i -= 1;
        bool stop = false;
        switch (tokenTag(i)) {
            case TokenTag::keyword_extern:
            case TokenTag::keyword_export: result.extern_export_token = i; break;
            case TokenTag::keyword_comptime: result.comptime_token = i; break;
            case TokenTag::keyword_pub: result.visib_token = i; break;
            case TokenTag::keyword_threadlocal: result.threadlocal_token = i; break;
            case TokenTag::string_literal: result.lib_name = i; break;
            default: stop = true; break;
        }
        if (stop) break;
    }
    return result;
}

full::AssignDestructure Ast::fullAssignDestructureComponents(const full::AssignDestructure::Components& info) const {
    full::AssignDestructure result{std::nullopt, info};
    const TokenIndex first_variable_token = firstToken(info.variables[0]);
    TokenIndex maybe_comptime_token;
    switch (nodeTag(info.variables[0])) {
        case NodeTag::global_var_decl:
        case NodeTag::local_var_decl:
        case NodeTag::aligned_var_decl:
        case NodeTag::simple_var_decl: maybe_comptime_token = first_variable_token; break;
        default: maybe_comptime_token = first_variable_token - 1; break;
    }
    if (tokenTag(maybe_comptime_token) == TokenTag::keyword_comptime) result.comptime_token = maybe_comptime_token;
    return result;
}

full::If Ast::fullIfComponents(const full::If::Components& info) const {
    full::If result{};
    result.ast = info;
    // if (cond_expr) |x|
    //              ^ ^
    const TokenIndex payload_pipe = lastToken(info.cond_expr) + 2;
    if (tokenTag(payload_pipe) == TokenTag::pipe) result.payload_token = payload_pipe + 1;
    if (!info.else_expr.isNone()) {
        // then_expr else |x|
        //           ^    ^
        result.else_token = lastToken(info.then_expr) + 1;
        if (tokenTag(result.else_token + 1) == TokenTag::pipe) result.error_token = result.else_token + 2;
    }
    return result;
}

full::ContainerField Ast::fullContainerFieldComponents(const full::ContainerField::Components& info) const {
    full::ContainerField result{std::nullopt, info};
    if (isTokenPrecededByTags(info.main_token, {TokenTag::keyword_comptime})) {
        // comptime type = init,
        // ^        ^
        // comptime name: type = init,
        // ^        ^
        result.comptime_token = info.main_token - 1;
    }
    return result;
}

full::FnProto Ast::fullFnProtoComponents(const full::FnProto::Components& info) const {
    full::FnProto result{};
    result.ast = info;
    TokenIndex i = info.fn_token;
    while (i > 0) {
        i -= 1;
        bool stop = false;
        switch (tokenTag(i)) {
            case TokenTag::keyword_extern:
            case TokenTag::keyword_export:
            case TokenTag::keyword_inline:
            case TokenTag::keyword_noinline: result.extern_export_inline_token = i; break;
            case TokenTag::keyword_pub: result.visib_token = i; break;
            case TokenTag::string_literal: result.lib_name = i; break;
            default: stop = true; break;
        }
        if (stop) break;
    }
    const TokenIndex after_fn_token = info.fn_token + 1;
    if (tokenTag(after_fn_token) == TokenTag::identifier) {
        result.name_token = after_fn_token;
        result.lparen = after_fn_token + 1;
    } else {
        result.lparen = after_fn_token;
    }
    assert(tokenTag(result.lparen) == TokenTag::l_paren);
    return result;
}

full::PtrType Ast::fullPtrTypeComponents(const full::PtrType::Components& info) const {
    full::PtrSize size = full::PtrSize::one;
    switch (tokenTag(info.main_token)) {
        case TokenTag::asterisk:
        case TokenTag::asterisk_asterisk: size = full::PtrSize::one; break;
        case TokenTag::l_bracket:
            if (tokenTag(info.main_token + 1) == TokenTag::asterisk) {
                size = tokenTag(info.main_token + 2) == TokenTag::identifier ? full::PtrSize::c : full::PtrSize::many;
            } else {
                size = full::PtrSize::slice;
            }
            break;
        default: assert(false);
    }
    full::PtrType result{size, std::nullopt, std::nullopt, std::nullopt, info};
    // We need to be careful that we don't iterate over any sub-expressions
    // here while looking for modifiers as that could result in false
    // positives. Therefore, start after a sentinel if there is one and
    // skip over any align node and bit range nodes.
    TokenIndex i;
    if (!info.sentinel.isNone()) {
        i = lastToken(info.sentinel.value) + 1;
    } else {
        switch (size) {
            case full::PtrSize::one: i = info.main_token; break;
            case full::PtrSize::slice: i = info.main_token + 1; break;
            case full::PtrSize::many: i = info.main_token + 2; break;
            case full::PtrSize::c: i = info.main_token + 3; break;
        }
    }
    i += 1;
    const TokenIndex end = firstToken(info.child_type);
    for (; i < end; i += 1) {
        switch (tokenTag(i)) {
            case TokenTag::keyword_allowzero: result.allowzero_token = i; break;
            case TokenTag::keyword_const: result.const_token = i; break;
            case TokenTag::keyword_volatile: result.volatile_token = i; break;
            case TokenTag::keyword_align:
                if (!info.bit_range_end.isNone()) {
                    assert(!info.bit_range_start.isNone());
                    i = lastToken(info.bit_range_end.value) + 1;
                } else if (!info.align_node.isNone()) {
                    i = lastToken(info.align_node.value) + 1;
                } else {
                    assert(false);
                }
                break;
            case TokenTag::keyword_addrspace: i = lastToken(info.addrspace_node.unwrap()) + 1; break;
            default: assert(false);
        }
    }
    return result;
}

full::ContainerDecl Ast::fullContainerDeclComponents(const full::ContainerDecl::Components& info) const {
    full::ContainerDecl result{std::nullopt, info};
    if (info.main_token == 0) return result;  // .root
    const TokenIndex previous_token = info.main_token - 1;
    switch (tokenTag(previous_token)) {
        case TokenTag::keyword_extern:
        case TokenTag::keyword_packed: result.layout_token = previous_token; break;
        default: break;
    }
    return result;
}

full::Switch Ast::fullSwitchComponents(const full::Switch::Components& info) const {
    const TokenIndex tok_i = info.switch_token == 0 ? 0 : info.switch_token - 1;
    full::Switch result{info, std::nullopt};
    if (tokenTag(tok_i) == TokenTag::colon && tokenTag(tok_i == 0 ? 0 : tok_i - 1) == TokenTag::identifier)
        result.label_token = tok_i - 1;
    return result;
}

full::SwitchCase Ast::fullSwitchCaseComponents(const full::SwitchCase::Components& info, NodeIndex node) const {
    full::SwitchCase result{std::nullopt, std::nullopt, info};
    if (tokenTag(info.arrow_token + 1) == TokenTag::pipe) result.payload_token = info.arrow_token + 2;
    switch (nodeTag(node)) {
        case NodeTag::switch_case_inline:
        case NodeTag::switch_case_inline_one:
            result.inline_token =
                result.ast.values.empty() ? info.arrow_token - 2 : firstToken(result.ast.values[0]) - 1;
            break;
        default: result.inline_token = std::nullopt; break;
    }
    return result;
}

full::Asm Ast::fullAsmComponents(const full::Asm::Components& info) const {
    full::Asm result{info, std::nullopt, {}, {}};
    if (tokenTag(info.asm_token + 1) == TokenTag::keyword_volatile) result.volatile_token = info.asm_token + 1;
    std::size_t outputs_end = info.items.size();
    for (std::size_t i = 0; i < info.items.size(); ++i) {
        if (nodeTag(info.items[i]) != NodeTag::asm_output) {
            outputs_end = i;
            break;
        }
    }
    result.outputs = info.items.subspan(0, outputs_end);
    result.inputs = info.items.subspan(outputs_end);
    return result;
}

full::While Ast::fullWhileComponents(const full::While::Components& info) const {
    full::While result{};
    result.ast = info;
    TokenIndex tok_i = info.while_token;
    if (isTokenPrecededByTags(tok_i, {TokenTag::keyword_inline})) {
        result.inline_token = tok_i - 1;
        tok_i = tok_i - 1;
    }
    if (isTokenPrecededByTags(tok_i, {TokenTag::identifier, TokenTag::colon})) result.label_token = tok_i - 2;
    const TokenIndex last_cond_token = lastToken(info.cond_expr);
    if (tokenTag(last_cond_token + 2) == TokenTag::pipe) result.payload_token = last_cond_token + 3;
    if (!info.else_expr.isNone()) {
        // then_expr else |x|
        //           ^    ^
        result.else_token = lastToken(info.then_expr) + 1;
        if (tokenTag(result.else_token + 1) == TokenTag::pipe) result.error_token = result.else_token + 2;
    }
    return result;
}

full::For Ast::fullForComponents(const full::For::Components& info) const {
    full::For result{};
    result.ast = info;
    TokenIndex tok_i = info.for_token;
    if (isTokenPrecededByTags(tok_i, {TokenTag::keyword_inline})) {
        result.inline_token = tok_i - 1;
        tok_i = tok_i - 1;
    }
    if (isTokenPrecededByTags(tok_i, {TokenTag::identifier, TokenTag::colon})) result.label_token = tok_i - 2;
    const TokenIndex last_cond_token = lastToken(info.inputs[info.inputs.size() - 1]);
    result.payload_token = last_cond_token + 3u + (tokenTag(last_cond_token + 1) == TokenTag::comma ? 1u : 0u);
    if (!info.else_expr.isNone()) result.else_token = lastToken(info.then_expr) + 1;
    return result;
}

std::optional<full::VarDecl> Ast::fullVarDecl(NodeIndex node) const {
    switch (nodeTag(node)) {
        case NodeTag::global_var_decl: return globalVarDecl(node);
        case NodeTag::local_var_decl: return localVarDecl(node);
        case NodeTag::aligned_var_decl: return alignedVarDecl(node);
        case NodeTag::simple_var_decl: return simpleVarDecl(node);
        default: return std::nullopt;
    }
}

std::optional<full::If> Ast::fullIf(NodeIndex node) const {
    switch (nodeTag(node)) {
        case NodeTag::if_simple: return ifSimple(node);
        case NodeTag::if_: return ifFull(node);
        default: return std::nullopt;
    }
}

std::optional<full::While> Ast::fullWhile(NodeIndex node) const {
    switch (nodeTag(node)) {
        case NodeTag::while_simple: return whileSimple(node);
        case NodeTag::while_cont: return whileCont(node);
        case NodeTag::while_: return whileFull(node);
        default: return std::nullopt;
    }
}

std::optional<full::For> Ast::fullFor(NodeIndex node) const {
    switch (nodeTag(node)) {
        case NodeTag::for_simple: return forSimple(node);
        case NodeTag::for_: return forFull(node);
        default: return std::nullopt;
    }
}

std::optional<full::ContainerField> Ast::fullContainerField(NodeIndex node) const {
    switch (nodeTag(node)) {
        case NodeTag::container_field_init: return containerFieldInit(node);
        case NodeTag::container_field_align: return containerFieldAlign(node);
        case NodeTag::container_field: return containerField(node);
        default: return std::nullopt;
    }
}

std::optional<full::FnProto> Ast::fullFnProto(NodeBuffer1& buffer, NodeIndex node) const {
    switch (nodeTag(node)) {
        case NodeTag::fn_proto: return fnProto(node);
        case NodeTag::fn_proto_multi: return fnProtoMulti(node);
        case NodeTag::fn_proto_one: return fnProtoOne(buffer, node);
        case NodeTag::fn_proto_simple: return fnProtoSimple(buffer, node);
        case NodeTag::fn_decl: return fullFnProto(buffer, nodeData(node).node_and_node().first);
        default: return std::nullopt;
    }
}

std::optional<full::StructInit> Ast::fullStructInit(NodeBuffer2& buffer, NodeIndex node) const {
    switch (nodeTag(node)) {
        case NodeTag::struct_init_one:
        case NodeTag::struct_init_one_comma: return structInitOne(buffer.data(), node);
        case NodeTag::struct_init_dot_two:
        case NodeTag::struct_init_dot_two_comma: return structInitDotTwo(buffer, node);
        case NodeTag::struct_init_dot:
        case NodeTag::struct_init_dot_comma: return structInitDot(node);
        case NodeTag::struct_init:
        case NodeTag::struct_init_comma: return structInit(node);
        default: return std::nullopt;
    }
}

std::optional<full::ArrayInit> Ast::fullArrayInit(NodeBuffer2& buffer, NodeIndex node) const {
    switch (nodeTag(node)) {
        case NodeTag::array_init_one:
        case NodeTag::array_init_one_comma: return arrayInitOne(buffer.data(), node);
        case NodeTag::array_init_dot_two:
        case NodeTag::array_init_dot_two_comma: return arrayInitDotTwo(buffer, node);
        case NodeTag::array_init_dot:
        case NodeTag::array_init_dot_comma: return arrayInitDot(node);
        case NodeTag::array_init:
        case NodeTag::array_init_comma: return arrayInit(node);
        default: return std::nullopt;
    }
}

std::optional<full::ArrayType> Ast::fullArrayType(NodeIndex node) const {
    switch (nodeTag(node)) {
        case NodeTag::array_type: return arrayType(node);
        case NodeTag::array_type_sentinel: return arrayTypeSentinel(node);
        default: return std::nullopt;
    }
}

std::optional<full::PtrType> Ast::fullPtrType(NodeIndex node) const {
    switch (nodeTag(node)) {
        case NodeTag::ptr_type_aligned: return ptrTypeAligned(node);
        case NodeTag::ptr_type_sentinel: return ptrTypeSentinel(node);
        case NodeTag::ptr_type: return ptrType(node);
        case NodeTag::ptr_type_bit_range: return ptrTypeBitRange(node);
        default: return std::nullopt;
    }
}

std::optional<full::Slice> Ast::fullSlice(NodeIndex node) const {
    switch (nodeTag(node)) {
        case NodeTag::slice_open: return sliceOpen(node);
        case NodeTag::slice: return slice(node);
        case NodeTag::slice_sentinel: return sliceSentinel(node);
        default: return std::nullopt;
    }
}

std::optional<full::ContainerDecl> Ast::fullContainerDecl(NodeBuffer2& buffer, NodeIndex node) const {
    switch (nodeTag(node)) {
        case NodeTag::root: return containerDeclRoot();
        case NodeTag::container_decl:
        case NodeTag::container_decl_trailing: return containerDecl(node);
        case NodeTag::container_decl_arg:
        case NodeTag::container_decl_arg_trailing: return containerDeclArg(node);
        case NodeTag::container_decl_two:
        case NodeTag::container_decl_two_trailing: return containerDeclTwo(buffer, node);
        case NodeTag::tagged_union:
        case NodeTag::tagged_union_trailing: return taggedUnion(node);
        case NodeTag::tagged_union_enum_tag:
        case NodeTag::tagged_union_enum_tag_trailing: return taggedUnionEnumTag(node);
        case NodeTag::tagged_union_two:
        case NodeTag::tagged_union_two_trailing: return taggedUnionTwo(buffer, node);
        default: return std::nullopt;
    }
}

std::optional<full::Switch> Ast::fullSwitch(NodeIndex node) const {
    switch (nodeTag(node)) {
        case NodeTag::switch_:
        case NodeTag::switch_comma: return switchFull(node);
        default: return std::nullopt;
    }
}

std::optional<full::SwitchCase> Ast::fullSwitchCase(NodeIndex node) const {
    switch (nodeTag(node)) {
        case NodeTag::switch_case_one:
        case NodeTag::switch_case_inline_one: return switchCaseOne(node);
        case NodeTag::switch_case:
        case NodeTag::switch_case_inline: return switchCase(node);
        default: return std::nullopt;
    }
}

std::optional<full::Asm> Ast::fullAsm(NodeIndex node) const {
    switch (nodeTag(node)) {
        case NodeTag::asm_simple: return asmSimple(node);
        case NodeTag::asm_: return asmFull(node);
        default: return std::nullopt;
    }
}

std::optional<full::Call> Ast::fullCall(NodeBuffer1& buffer, NodeIndex node) const {
    switch (nodeTag(node)) {
        case NodeTag::call:
        case NodeTag::call_comma: return callFull(node);
        case NodeTag::call_one:
        case NodeTag::call_one_comma: return callOne(buffer, node);
        default: return std::nullopt;
    }
}

std::optional<std::span<const NodeIndex>> Ast::builtinCallParams(NodeBuffer2& buffer, NodeIndex node) const {
    switch (nodeTag(node)) {
        case NodeTag::builtin_call_two:
        case NodeTag::builtin_call_two_comma: {
            const Data d = nodeData(node);
            return loadOptionalNodesIntoBuffer<2>(buffer.data(), {d.lhs, d.rhs});
        }
        case NodeTag::builtin_call:
        case NodeTag::builtin_call_comma: return extraDataSlice(nodeData(node).extra_range());
        default: return std::nullopt;
    }
}

std::optional<std::span<const NodeIndex>> Ast::blockStatements(NodeBuffer2& buffer, NodeIndex node) const {
    switch (nodeTag(node)) {
        case NodeTag::block_two:
        case NodeTag::block_two_semicolon: {
            const Data d = nodeData(node);
            return loadOptionalNodesIntoBuffer<2>(buffer.data(), {d.lhs, d.rhs});
        }
        case NodeTag::block:
        case NodeTag::block_semicolon: return extraDataSlice(nodeData(node).extra_range());
        default: return std::nullopt;
    }
}

// ---- full.* methods ------------------------------------------------------------

TokenIndex full::VarDecl::firstToken() const {
    if (visib_token) return *visib_token;
    if (extern_export_token) return *extern_export_token;
    if (threadlocal_token) return *threadlocal_token;
    if (comptime_token) return *comptime_token;
    return ast.mut_token;
}

void full::ContainerField::convertToNonTupleLike(const Ast& tree) {
    if (!ast.tuple_like) return;
    if (tree.nodeTag(ast.type_expr.unwrap()) != NodeTag::identifier) return;
    ast.type_expr = OptionalIndex::none();
    ast.tuple_like = false;
}

std::optional<full::FnProto::Param> full::FnProto::Iterator::next() {
    const Ast& t = *tree;
    while (true) {
        std::optional<TokenIndex> first_doc_comment;
        std::optional<TokenIndex> comptime_noalias;
        std::optional<TokenIndex> param_name_token;
        if (!tok_flag) {
            if (param_i >= fn_proto->ast.params.size()) return std::nullopt;
            const NodeIndex param_type = fn_proto->ast.params[param_i];
            TokenIndex ti = t.firstToken(param_type) - 1;
            for (;; ti -= 1) {
                bool stop = false;
                switch (t.tokenTag(ti)) {
                    case TokenTag::colon: continue;
                    case TokenTag::identifier: param_name_token = ti; break;
                    case TokenTag::doc_comment: first_doc_comment = ti; break;
                    case TokenTag::keyword_comptime:
                    case TokenTag::keyword_noalias: comptime_noalias = ti; break;
                    default: stop = true; break;
                }
                if (stop) break;
            }
            param_i += 1;
            tok_i = t.lastToken(param_type) + 1;
            // Look for anytype and ... params afterwards.
            if (t.tokenTag(tok_i) == TokenTag::comma) tok_i += 1;
            tok_flag = true;
            return Param{first_doc_comment, param_name_token, comptime_noalias, std::nullopt, param_type};
        }
        if (t.tokenTag(tok_i) == TokenTag::comma) tok_i += 1;
        if (t.tokenTag(tok_i) == TokenTag::r_paren) return std::nullopt;
        if (t.tokenTag(tok_i) == TokenTag::doc_comment) {
            first_doc_comment = tok_i;
            while (t.tokenTag(tok_i) == TokenTag::doc_comment) tok_i += 1;
        }
        switch (t.tokenTag(tok_i)) {
            case TokenTag::ellipsis3:
                tok_flag = false;  // Next iteration should return null.
                return Param{first_doc_comment, std::nullopt, std::nullopt, tok_i, std::nullopt};
            case TokenTag::keyword_noalias:
            case TokenTag::keyword_comptime:
                comptime_noalias = tok_i;
                tok_i += 1;
                break;
            default: break;
        }
        if (t.tokenTag(tok_i) == TokenTag::identifier && t.tokenTag(tok_i + 1) == TokenTag::colon) {
            param_name_token = tok_i;
            tok_i += 2;
        }
        if (t.tokenTag(tok_i) == TokenTag::keyword_anytype) {
            tok_i += 1;
            return Param{first_doc_comment, param_name_token, comptime_noalias, tok_i - 1, std::nullopt};
        }
        tok_flag = false;
    }
}

full::FnProto::Iterator full::FnProto::iterate(const Ast& tree) const {
    return Iterator{&tree, this, 0, lparen + 1, true};
}

// ---- spans ---------------------------------------------------------------------

Span Ast::nodeToSpan(NodeIndex node) const {
    return tokensToSpan(firstToken(node), lastToken(node), nodeMainToken(node));
}

Span Ast::tokenToSpan(TokenIndex token) const { return tokensToSpan(token, token, token); }

Span Ast::tokensToSpan(TokenIndex start, TokenIndex end, TokenIndex main) const {
    TokenIndex start_tok = start;
    TokenIndex end_tok = end;
    if (tokensOnSameLine(start, end)) {
        // do nothing
    } else if (tokensOnSameLine(start, main)) {
        end_tok = main;
    } else if (tokensOnSameLine(main, end)) {
        start_tok = main;
    } else {
        start_tok = main;
        end_tok = main;
    }
    const std::uint32_t start_off = tokenStart(start_tok);
    const std::uint32_t end_off = tokenStart(end_tok) + static_cast<std::uint32_t>(tokenSlice(end_tok).size());
    return Span{start_off, end_off, tokenStart(main)};
}

// ---- AstGen.lowerAstErrors -------------------------------------------------------

namespace {

/// std.zig.findLineColumn: 0-based line and byte column of `offset`.
void findLineColumn(std::string_view source, std::size_t offset, std::size_t& line, std::size_t& column) {
    line = 0;
    column = 0;
    std::size_t line_start = 0;
    for (std::size_t i = 0; i < offset && i < source.size(); ++i) {
        if (source[i] == '\n') {
            line += 1;
            line_start = i + 1;
        }
    }
    column = offset - line_start;
}

RenderedError makeRendered(const Ast& tree, std::size_t byte_offset, std::string message, bool is_note) {
    RenderedError r{byte_offset, 0, 0, std::move(message), is_note};
    findLineColumn(tree.source, byte_offset, r.line, r.column);
    return r;
}

}  // namespace

std::vector<RenderedError> renderErrors(const Ast& tree) {
    std::vector<RenderedError> out;
    if (tree.errors.empty()) return out;

    const Error& parse_err = tree.errors[0];
    const TokenIndex tok = parse_err.token + (parse_err.token_is_prev ? 1u : 0u);
    const std::size_t tok_start = tree.tokenStart(tok);
    const char start_char = tree.source[tok_start];

    if (parse_err.tag != ErrorTag::nesting_too_deep && tree.tokenTag(tok) == TokenTag::invalid &&
        (start_char == '"' || start_char == '\'' || start_char == '/' ||
         std::string_view(tree.source).substr(tok_start).starts_with("\\\\"))) {
        const std::size_t tok_len = tree.tokenSlice(tok).size();
        const std::size_t tok_end = tok_start + tok_len;
        std::size_t idx = tok_start;
        for (; idx < tok_end; ++idx) {
            const auto c = static_cast<unsigned char>(tree.source[idx]);
            if (c <= 0x09 || (c >= 0x0b && c <= 0x1f) || c == 0x7f) break;
        }
        const std::size_t bad_off = idx - tok_start;
        Error ast_err{};
        ast_err.tag = ErrorTag::invalid_byte;
        ast_err.token = tok;
        ast_err.offset = bad_off;
        out.push_back(makeRendered(tree, tok_start + bad_off, tree.renderError(ast_err), false));
        return out;
    }

    // The first error and the notes that follow it; later errors are dropped,
    // as lowerAstErrors does.
    std::vector<RenderedError> notes;
    for (std::size_t i = 1; i < tree.errors.size(); ++i) {
        const Error& err = tree.errors[i];
        if (!err.is_note) break;
        notes.push_back(makeRendered(tree, tree.tokenStart(err.token), tree.renderError(err), true));
    }
    const std::size_t extra_offset = tree.errorOffset(parse_err);
    out.push_back(
        makeRendered(tree, tree.tokenStart(parse_err.token) + extra_offset, tree.renderError(parse_err), false));
    out.insert(out.end(), notes.begin(), notes.end());
    return out;
}

}  // namespace zig
