/**
 * @file zig_ast.h
 * @brief Zig's own syntax tree (std.zig.Ast), ported to C++: the shared front
 *        end of mep-zig-lsp and mep's Zig formatter.
 *
 * A decision-for-decision port of Zig 0.16's lib/std/zig/tokenizer.zig,
 * Parse.zig and Ast.zig, so the tree, the token list, the extra data and the
 * parse errors are the ones `zig fmt` / `zig ast-check` build -- node for node,
 * array for array, message for message. Names follow Zig's (camelCase
 * methods, the same node and token tags, the same `full.*` views) so a reader
 * can diff this against the Zig files and a port of Ast/Render.zig reads
 * naturally on top of it. Zig names that are C++ keywords (`.@"if"`,
 * `.@"catch"`, ...) get a trailing underscore: `NodeTag::if_`, `NodeTag::catch_`.
 *
 * Deviation from Zig: the parser is recursive and runs inside an editor, so
 * nesting is capped (kMaxNesting); past it the parse stops with a
 * `nesting_too_deep` error and an empty root, instead of growing the stack
 * until it overflows. Zig has no such limit. Nothing in a real corpus comes
 * near it.
 *
 * Files: zig_tokenizer.cpp (tokenizer.zig), zig_parse.cpp (Parse.zig),
 * zig_ast.cpp (Ast.zig).
 *
 * Ported from Zig (https://ziglang.org), lib/std/zig/, under the MIT license:
 *
 * The MIT License (Expat)
 *
 * Copyright (c) Zig contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zig {

/// How deep the recursive parser may nest before it gives up (see the file
/// comment). Counted per parsePrefixExpr/parseTypeExpr/parseBlock/
/// parseContainerMembers activation.
constexpr std::uint32_t kMaxNesting = 1000;

// ---- tokens (tokenizer.zig) ------------------------------------------------

#define ZIG_TOKEN_TAGS(X)                                                                                         \
    X(invalid) X(invalid_periodasterisks) X(identifier) X(string_literal) X(multiline_string_literal_line)       \
    X(char_literal) X(eof) X(builtin) X(bang) X(pipe) X(pipe_pipe) X(pipe_equal) X(equal) X(equal_equal)         \
    X(equal_angle_bracket_right) X(bang_equal) X(l_paren) X(r_paren) X(semicolon) X(percent) X(percent_equal)    \
    X(l_brace) X(r_brace) X(l_bracket) X(r_bracket) X(period) X(period_asterisk) X(ellipsis2) X(ellipsis3)       \
    X(caret) X(caret_equal) X(plus) X(plus_plus) X(plus_equal) X(plus_percent) X(plus_percent_equal)             \
    X(plus_pipe) X(plus_pipe_equal) X(minus) X(minus_equal) X(minus_percent) X(minus_percent_equal)              \
    X(minus_pipe) X(minus_pipe_equal) X(asterisk) X(asterisk_equal) X(asterisk_asterisk) X(asterisk_percent)     \
    X(asterisk_percent_equal) X(asterisk_pipe) X(asterisk_pipe_equal) X(arrow) X(colon) X(slash)                 \
    X(slash_equal) X(comma) X(ampersand) X(ampersand_equal) X(question_mark) X(angle_bracket_left)               \
    X(angle_bracket_left_equal) X(angle_bracket_angle_bracket_left) X(angle_bracket_angle_bracket_left_equal)    \
    X(angle_bracket_angle_bracket_left_pipe) X(angle_bracket_angle_bracket_left_pipe_equal)                      \
    X(angle_bracket_right) X(angle_bracket_right_equal) X(angle_bracket_angle_bracket_right)                     \
    X(angle_bracket_angle_bracket_right_equal) X(tilde) X(number_literal) X(doc_comment)                         \
    X(container_doc_comment) X(keyword_addrspace) X(keyword_align) X(keyword_allowzero) X(keyword_and)           \
    X(keyword_anyframe) X(keyword_anytype) X(keyword_asm) X(keyword_break) X(keyword_callconv)                   \
    X(keyword_catch) X(keyword_comptime) X(keyword_const) X(keyword_continue) X(keyword_defer)                   \
    X(keyword_else) X(keyword_enum) X(keyword_errdefer) X(keyword_error) X(keyword_export) X(keyword_extern)     \
    X(keyword_fn) X(keyword_for) X(keyword_if) X(keyword_inline) X(keyword_noalias) X(keyword_noinline)          \
    X(keyword_nosuspend) X(keyword_opaque) X(keyword_or) X(keyword_orelse) X(keyword_packed) X(keyword_pub)      \
    X(keyword_resume) X(keyword_return) X(keyword_linksection) X(keyword_struct) X(keyword_suspend)              \
    X(keyword_switch) X(keyword_test) X(keyword_threadlocal) X(keyword_try) X(keyword_union)                     \
    X(keyword_unreachable) X(keyword_var) X(keyword_volatile) X(keyword_while)

enum class TokenTag : std::uint8_t {
#define ZIG_X(name) name,
    ZIG_TOKEN_TAGS(ZIG_X)
#undef ZIG_X
};

/// Token.Tag.lexeme(): the fixed spelling, or empty for tags without one.
std::string_view tokenLexeme(TokenTag tag);
/// Token.Tag.symbol(): the lexeme, or a description ("an identifier", "EOF").
std::string_view tokenSymbol(TokenTag tag);
/// The tag's Zig name ("keyword_fn"), for dumps.
std::string_view tokenTagName(TokenTag tag);
/// Token.getKeyword(): the keyword tag for `bytes`, if it is one.
std::optional<TokenTag> getKeyword(std::string_view bytes);

/// A token as the tokenizer returns it (Token: tag + loc).
struct Token {
    TokenTag tag;
    std::size_t start;
    std::size_t end;
};

/// std.zig.Tokenizer. `buffer` must be NUL-terminated at buffer[len] (a
/// std::string's data() is), as Zig's `[:0]const u8` is.
struct Tokenizer {
    const char* buffer;
    std::size_t len;
    std::size_t index;

    /// Skips a UTF-8 BOM, as Tokenizer.init does.
    static Tokenizer init(const char* buffer, std::size_t len);
    /// After this returns invalid, it resets on the next newline. An eof
    /// token is always returned at the end.
    Token next();
};

// ---- nodes (Ast.zig) ---------------------------------------------------------

using ByteOffset = std::uint32_t;
using TokenIndex = std::uint32_t;
using NodeIndex = std::uint32_t;
using ExtraIndex = std::uint32_t;
constexpr std::uint32_t kNone = 0xFFFFFFFFu;

/// Node.OptionalIndex / OptionalTokenIndex: an index, or none (maxInt(u32)).
struct OptionalIndex {
    std::uint32_t value = kNone;
    static constexpr OptionalIndex none() { return OptionalIndex{}; }
    static constexpr OptionalIndex from(std::uint32_t v) { return OptionalIndex{v}; }
    /// fromOptional(?Index): kNone stays none.
    static constexpr OptionalIndex fromOptional(std::uint32_t v) { return OptionalIndex{v}; }
    constexpr bool isNone() const { return value == kNone; }
    constexpr explicit operator bool() const { return value != kNone; }
    /// unwrap().? -- only when known to be present.
    constexpr std::uint32_t unwrap() const { return value; }
    constexpr bool operator==(const OptionalIndex& o) const { return value == o.value; }
};
using OptionalNodeIndex = OptionalIndex;
using OptionalTokenIndex = OptionalIndex;

#define ZIG_NODE_TAGS(X)                                                                                          \
    X(root, "root") X(test_decl, "test_decl") X(global_var_decl, "global_var_decl")                                \
    X(local_var_decl, "local_var_decl") X(simple_var_decl, "simple_var_decl")                                      \
    X(aligned_var_decl, "aligned_var_decl") X(errdefer_, "errdefer") X(defer_, "defer") X(catch_, "catch")         \
    X(field_access, "field_access") X(unwrap_optional, "unwrap_optional") X(equal_equal, "equal_equal")            \
    X(bang_equal, "bang_equal") X(less_than, "less_than") X(greater_than, "greater_than")                          \
    X(less_or_equal, "less_or_equal") X(greater_or_equal, "greater_or_equal") X(assign_mul, "assign_mul")          \
    X(assign_div, "assign_div") X(assign_mod, "assign_mod") X(assign_add, "assign_add")                            \
    X(assign_sub, "assign_sub") X(assign_shl, "assign_shl") X(assign_shl_sat, "assign_shl_sat")                    \
    X(assign_shr, "assign_shr") X(assign_bit_and, "assign_bit_and") X(assign_bit_xor, "assign_bit_xor")            \
    X(assign_bit_or, "assign_bit_or") X(assign_mul_wrap, "assign_mul_wrap") X(assign_add_wrap, "assign_add_wrap")  \
    X(assign_sub_wrap, "assign_sub_wrap") X(assign_mul_sat, "assign_mul_sat") X(assign_add_sat, "assign_add_sat")  \
    X(assign_sub_sat, "assign_sub_sat") X(assign, "assign") X(assign_destructure, "assign_destructure")            \
    X(merge_error_sets, "merge_error_sets") X(mul, "mul") X(div, "div") X(mod, "mod")                              \
    X(array_mult, "array_mult") X(mul_wrap, "mul_wrap") X(mul_sat, "mul_sat") X(add, "add") X(sub, "sub")          \
    X(array_cat, "array_cat") X(add_wrap, "add_wrap") X(sub_wrap, "sub_wrap") X(add_sat, "add_sat")                \
    X(sub_sat, "sub_sat") X(shl, "shl") X(shl_sat, "shl_sat") X(shr, "shr") X(bit_and, "bit_and")                  \
    X(bit_xor, "bit_xor") X(bit_or, "bit_or") X(orelse_, "orelse") X(bool_and, "bool_and")                         \
    X(bool_or, "bool_or") X(bool_not, "bool_not") X(negation, "negation") X(bit_not, "bit_not")                    \
    X(negation_wrap, "negation_wrap") X(address_of, "address_of") X(try_, "try")                                   \
    X(optional_type, "optional_type") X(array_type, "array_type") X(array_type_sentinel, "array_type_sentinel")    \
    X(ptr_type_aligned, "ptr_type_aligned") X(ptr_type_sentinel, "ptr_type_sentinel") X(ptr_type, "ptr_type")      \
    X(ptr_type_bit_range, "ptr_type_bit_range") X(slice_open, "slice_open") X(slice, "slice")                      \
    X(slice_sentinel, "slice_sentinel") X(deref, "deref") X(array_access, "array_access")                          \
    X(array_init_one, "array_init_one") X(array_init_one_comma, "array_init_one_comma")                            \
    X(array_init_dot_two, "array_init_dot_two") X(array_init_dot_two_comma, "array_init_dot_two_comma")            \
    X(array_init_dot, "array_init_dot") X(array_init_dot_comma, "array_init_dot_comma")                            \
    X(array_init, "array_init") X(array_init_comma, "array_init_comma") X(struct_init_one, "struct_init_one")      \
    X(struct_init_one_comma, "struct_init_one_comma") X(struct_init_dot_two, "struct_init_dot_two")                \
    X(struct_init_dot_two_comma, "struct_init_dot_two_comma") X(struct_init_dot, "struct_init_dot")                \
    X(struct_init_dot_comma, "struct_init_dot_comma") X(struct_init, "struct_init")                                \
    X(struct_init_comma, "struct_init_comma") X(call_one, "call_one") X(call_one_comma, "call_one_comma")          \
    X(call, "call") X(call_comma, "call_comma") X(switch_, "switch") X(switch_comma, "switch_comma")               \
    X(switch_case_one, "switch_case_one") X(switch_case_inline_one, "switch_case_inline_one")                      \
    X(switch_case, "switch_case") X(switch_case_inline, "switch_case_inline") X(switch_range, "switch_range")       \
    X(while_simple, "while_simple") X(while_cont, "while_cont") X(while_, "while") X(for_simple, "for_simple")     \
    X(for_, "for") X(for_range, "for_range") X(if_simple, "if_simple") X(if_, "if") X(suspend_, "suspend")         \
    X(resume_, "resume") X(continue_, "continue") X(break_, "break") X(return_, "return")                          \
    X(fn_proto_simple, "fn_proto_simple") X(fn_proto_multi, "fn_proto_multi") X(fn_proto_one, "fn_proto_one")      \
    X(fn_proto, "fn_proto") X(fn_decl, "fn_decl") X(anyframe_type, "anyframe_type")                                \
    X(anyframe_literal, "anyframe_literal") X(char_literal, "char_literal") X(number_literal, "number_literal")     \
    X(unreachable_literal, "unreachable_literal") X(identifier, "identifier") X(enum_literal, "enum_literal")       \
    X(string_literal, "string_literal") X(multiline_string_literal, "multiline_string_literal")                    \
    X(grouped_expression, "grouped_expression") X(builtin_call_two, "builtin_call_two")                            \
    X(builtin_call_two_comma, "builtin_call_two_comma") X(builtin_call, "builtin_call")                            \
    X(builtin_call_comma, "builtin_call_comma") X(error_set_decl, "error_set_decl")                                \
    X(container_decl, "container_decl") X(container_decl_trailing, "container_decl_trailing")                      \
    X(container_decl_two, "container_decl_two") X(container_decl_two_trailing, "container_decl_two_trailing")      \
    X(container_decl_arg, "container_decl_arg") X(container_decl_arg_trailing, "container_decl_arg_trailing")      \
    X(tagged_union, "tagged_union") X(tagged_union_trailing, "tagged_union_trailing")                              \
    X(tagged_union_two, "tagged_union_two") X(tagged_union_two_trailing, "tagged_union_two_trailing")              \
    X(tagged_union_enum_tag, "tagged_union_enum_tag")                                                              \
    X(tagged_union_enum_tag_trailing, "tagged_union_enum_tag_trailing")                                            \
    X(container_field_init, "container_field_init") X(container_field_align, "container_field_align")              \
    X(container_field, "container_field") X(comptime_, "comptime") X(nosuspend_, "nosuspend")                      \
    X(block_two, "block_two") X(block_two_semicolon, "block_two_semicolon") X(block, "block")                      \
    X(block_semicolon, "block_semicolon") X(asm_simple, "asm_simple") X(asm_, "asm") X(asm_output, "asm_output")   \
    X(asm_input, "asm_input") X(error_value, "error_value") X(error_union, "error_union")

enum class NodeTag : std::uint8_t {
#define ZIG_X(name, str) name,
    ZIG_NODE_TAGS(ZIG_X)
#undef ZIG_X
};

/// The tag's Zig name ("if", "fn_decl"), for dumps.
std::string_view nodeTagName(NodeTag tag);

/// Node.For, packed into a u32: inputs in the low 31 bits, has_else on top.
struct ForData {
    std::uint32_t inputs;
    bool has_else;
    static ForData unpack(std::uint32_t v) { return ForData{v & 0x7FFFFFFFu, (v >> 31) != 0}; }
    std::uint32_t pack() const { return (inputs & 0x7FFFFFFFu) | (has_else ? 0x80000000u : 0u); }
};

/// Node.SubRange: [start, end) into extra_data.
struct SubRange {
    ExtraIndex start;
    ExtraIndex end;
};

/// Node.Data: an untagged pair of u32s; the node's tag says which view
/// applies. Each accessor mirrors one of Zig's union fields.
struct Data {
    std::uint32_t lhs = 0xAAAAAAAAu;
    std::uint32_t rhs = 0xAAAAAAAAu;

    static Data undef() { return Data{}; }
    static Data pair(std::uint32_t a, std::uint32_t b) { return Data{a, b}; }

    NodeIndex node() const { return lhs; }
    OptionalNodeIndex opt_node() const { return OptionalIndex::from(lhs); }
    TokenIndex token() const { return lhs; }
    std::pair<NodeIndex, NodeIndex> node_and_node() const { return {lhs, rhs}; }
    std::pair<OptionalNodeIndex, OptionalNodeIndex> opt_node_and_opt_node() const {
        return {OptionalIndex::from(lhs), OptionalIndex::from(rhs)};
    }
    std::pair<NodeIndex, OptionalNodeIndex> node_and_opt_node() const { return {lhs, OptionalIndex::from(rhs)}; }
    std::pair<OptionalNodeIndex, NodeIndex> opt_node_and_node() const { return {OptionalIndex::from(lhs), rhs}; }
    std::pair<NodeIndex, ExtraIndex> node_and_extra() const { return {lhs, rhs}; }
    std::pair<ExtraIndex, NodeIndex> extra_and_node() const { return {lhs, rhs}; }
    std::pair<ExtraIndex, OptionalNodeIndex> extra_and_opt_node() const { return {lhs, OptionalIndex::from(rhs)}; }
    std::pair<NodeIndex, TokenIndex> node_and_token() const { return {lhs, rhs}; }
    std::pair<TokenIndex, NodeIndex> token_and_node() const { return {lhs, rhs}; }
    std::pair<TokenIndex, TokenIndex> token_and_token() const { return {lhs, rhs}; }
    std::pair<OptionalNodeIndex, TokenIndex> opt_node_and_token() const { return {OptionalIndex::from(lhs), rhs}; }
    std::pair<OptionalTokenIndex, NodeIndex> opt_token_and_node() const { return {OptionalIndex::from(lhs), rhs}; }
    std::pair<OptionalTokenIndex, OptionalNodeIndex> opt_token_and_opt_node() const {
        return {OptionalIndex::from(lhs), OptionalIndex::from(rhs)};
    }
    std::pair<OptionalTokenIndex, OptionalTokenIndex> opt_token_and_opt_token() const {
        return {OptionalIndex::from(lhs), OptionalIndex::from(rhs)};
    }
    std::pair<ExtraIndex, ForData> for_() const { return {lhs, ForData::unpack(rhs)}; }
    SubRange extra_range() const { return SubRange{lhs, rhs}; }
};

struct Node {
    NodeTag tag;
    TokenIndex main_token;
    Data data;
};

// The extra_data records (Node.LocalVarDecl ...), field order as in Zig.
namespace extra {
struct LocalVarDecl { NodeIndex type_node; NodeIndex align_node; };
struct ArrayTypeSentinel { NodeIndex sentinel; NodeIndex elem_type; };
struct PtrType { OptionalNodeIndex sentinel; OptionalNodeIndex align_node; OptionalNodeIndex addrspace_node; };
struct PtrTypeBitRange {
    OptionalNodeIndex sentinel; NodeIndex align_node; OptionalNodeIndex addrspace_node;
    NodeIndex bit_range_start; NodeIndex bit_range_end;
};
struct If { NodeIndex then_expr; NodeIndex else_expr; };
struct ContainerField { NodeIndex align_expr; NodeIndex value_expr; };
struct GlobalVarDecl {
    OptionalNodeIndex type_node; OptionalNodeIndex align_node;
    OptionalNodeIndex addrspace_node; OptionalNodeIndex section_node;
};
struct Slice { NodeIndex start; NodeIndex end; };
struct SliceSentinel { NodeIndex start; OptionalNodeIndex end; NodeIndex sentinel; };
struct While { OptionalNodeIndex cont_expr; NodeIndex then_expr; NodeIndex else_expr; };
struct WhileCont { NodeIndex cont_expr; NodeIndex then_expr; };
struct FnProtoOne {
    OptionalNodeIndex param; OptionalNodeIndex align_expr; OptionalNodeIndex addrspace_expr;
    OptionalNodeIndex section_expr; OptionalNodeIndex callconv_expr;
};
struct FnProto {
    ExtraIndex params_start; ExtraIndex params_end; OptionalNodeIndex align_expr;
    OptionalNodeIndex addrspace_expr; OptionalNodeIndex section_expr; OptionalNodeIndex callconv_expr;
};
struct Asm { ExtraIndex items_start; ExtraIndex items_end; OptionalNodeIndex clobbers; TokenIndex rparen; };
}  // namespace extra

// ---- errors ------------------------------------------------------------------

#define ZIG_ERROR_TAGS(X)                                                                                         \
    X(asterisk_after_ptr_deref) X(chained_comparison_operators) X(decl_between_fields) X(expected_block)         \
    X(expected_block_or_assignment) X(expected_block_or_expr) X(expected_block_or_field)                         \
    X(expected_container_members) X(expected_expr) X(expected_expr_or_assignment) X(expected_expr_or_var_decl)   \
    X(expected_fn) X(expected_inlinable) X(expected_labelable) X(expected_param_list) X(expected_prefix_expr)    \
    X(expected_primary_type_expr) X(expected_pub_item) X(expected_return_type) X(expected_semi_or_else)         \
    X(expected_semi_or_lbrace) X(expected_statement) X(expected_suffix_op) X(expected_type_expr)                \
    X(expected_var_decl) X(expected_var_decl_or_fn) X(expected_loop_payload) X(expected_container)              \
    X(extern_fn_body) X(extra_addrspace_qualifier) X(extra_align_qualifier) X(extra_allowzero_qualifier)        \
    X(extra_const_qualifier) X(extra_volatile_qualifier) X(ptr_mod_on_array_child_type) X(invalid_bit_range)    \
    X(same_line_doc_comment) X(unattached_doc_comment) X(test_doc_comment) X(comptime_doc_comment)              \
    X(varargs_nonfinal) X(expected_semi_after_decl) X(expected_semi_after_stmt) X(expected_comma_after_field)   \
    X(expected_comma_after_arg) X(expected_comma_after_param) X(expected_comma_after_initializer)               \
    X(expected_comma_after_switch_prong) X(expected_comma_after_for_operand) X(expected_comma_after_capture)    \
    X(expected_initializer) X(mismatched_binary_op_whitespace) X(invalid_ampersand_ampersand)                   \
    X(c_style_container) X(expected_var_const) X(wrong_equal_var_decl) X(var_const_decl) X(extra_for_capture)  \
    X(for_input_not_captured) X(zig_style_container) X(previous_field) X(next_field) X(expected_token)          \
    X(invalid_byte) X(nesting_too_deep)

enum class ErrorTag : std::uint8_t {
#define ZIG_X(name) name,
    ZIG_ERROR_TAGS(ZIG_X)
#undef ZIG_X
};

/// Ast.Error. `expected_tag` is meaningful for expected_token,
/// c_style_container and zig_style_container; `offset` for invalid_byte.
struct Error {
    ErrorTag tag;
    bool is_note = false;
    /// True if `token` points to the token before the token causing an issue.
    bool token_is_prev = false;
    TokenIndex token = 0;
    TokenTag expected_tag = TokenTag::invalid;
    std::size_t offset = 0;
};

enum class Mode { zig, zon };

struct Ast;

/// Ast.full: fully assembled views over the compact node encodings.
namespace full {

struct VarDecl {
    std::optional<TokenIndex> visib_token;
    std::optional<TokenIndex> extern_export_token;
    std::optional<TokenIndex> lib_name;
    std::optional<TokenIndex> threadlocal_token;
    std::optional<TokenIndex> comptime_token;
    struct Components {
        TokenIndex mut_token;
        OptionalNodeIndex type_node;
        OptionalNodeIndex align_node;
        OptionalNodeIndex addrspace_node;
        OptionalNodeIndex section_node;
        OptionalNodeIndex init_node;
    } ast;
    TokenIndex firstToken() const;
};

struct AssignDestructure {
    std::optional<TokenIndex> comptime_token;
    struct Components {
        std::span<const NodeIndex> variables;
        TokenIndex equal_token;
        NodeIndex value_expr;
    } ast;
};

struct If {
    /// The first token after the `|`: an identifier or a `*`.
    std::optional<TokenIndex> payload_token;
    /// The identifier after the `|`.
    std::optional<TokenIndex> error_token;
    /// Populated only if else_expr != none.
    TokenIndex else_token = 0;
    struct Components {
        TokenIndex if_token;
        NodeIndex cond_expr;
        NodeIndex then_expr;
        OptionalNodeIndex else_expr;
    } ast;
};

struct While {
    struct Components {
        TokenIndex while_token;
        NodeIndex cond_expr;
        OptionalNodeIndex cont_expr;
        NodeIndex then_expr;
        OptionalNodeIndex else_expr;
    } ast;
    std::optional<TokenIndex> inline_token;
    std::optional<TokenIndex> label_token;
    std::optional<TokenIndex> payload_token;
    std::optional<TokenIndex> error_token;
    /// Populated only if else_expr != none.
    TokenIndex else_token = 0;
};

struct For {
    struct Components {
        TokenIndex for_token;
        std::span<const NodeIndex> inputs;
        NodeIndex then_expr;
        OptionalNodeIndex else_expr;
    } ast;
    std::optional<TokenIndex> inline_token;
    std::optional<TokenIndex> label_token;
    TokenIndex payload_token = 0;
    /// Populated only if else_expr != none.
    std::optional<TokenIndex> else_token;
};

struct ContainerField {
    std::optional<TokenIndex> comptime_token;
    struct Components {
        TokenIndex main_token;
        /// Can only be none after convertToNonTupleLike.
        OptionalNodeIndex type_expr;
        OptionalNodeIndex align_expr;
        OptionalNodeIndex value_expr;
        bool tuple_like;
    } ast;
    TokenIndex firstToken() const { return comptime_token ? *comptime_token : ast.main_token; }
    void convertToNonTupleLike(const Ast& tree);
};

struct FnProto {
    std::optional<TokenIndex> visib_token;
    std::optional<TokenIndex> extern_export_inline_token;
    std::optional<TokenIndex> lib_name;
    std::optional<TokenIndex> name_token;
    TokenIndex lparen = 0;
    struct Components {
        NodeIndex proto_node;
        TokenIndex fn_token;
        OptionalNodeIndex return_type;
        std::span<const NodeIndex> params;
        OptionalNodeIndex align_expr;
        OptionalNodeIndex addrspace_expr;
        OptionalNodeIndex section_expr;
        OptionalNodeIndex callconv_expr;
    } ast;

    struct Param {
        std::optional<TokenIndex> first_doc_comment;
        std::optional<TokenIndex> name_token;
        std::optional<TokenIndex> comptime_noalias;
        std::optional<TokenIndex> anytype_ellipsis3;
        std::optional<NodeIndex> type_expr;
    };

    TokenIndex firstToken() const {
        return visib_token ? *visib_token : extern_export_inline_token ? *extern_export_inline_token : ast.fn_token;
    }

    /// Abstracts over anytype and `...` not being in `params`.
    struct Iterator {
        const Ast* tree;
        const FnProto* fn_proto;
        std::size_t param_i;
        TokenIndex tok_i;
        bool tok_flag;
        std::optional<Param> next();
    };
    Iterator iterate(const Ast& tree) const;
};

struct StructInit {
    struct Components {
        TokenIndex lbrace;
        std::span<const NodeIndex> fields;
        OptionalNodeIndex type_expr;
    } ast;
};

struct ArrayInit {
    struct Components {
        TokenIndex lbrace;
        std::span<const NodeIndex> elements;
        OptionalNodeIndex type_expr;
    } ast;
};

struct ArrayType {
    struct Components {
        TokenIndex lbracket;
        NodeIndex elem_count;
        OptionalNodeIndex sentinel;
        NodeIndex elem_type;
    } ast;
};

/// std.builtin.Type.Pointer.Size.
enum class PtrSize { one, many, slice, c };

struct PtrType {
    PtrSize size;
    std::optional<TokenIndex> allowzero_token;
    std::optional<TokenIndex> const_token;
    std::optional<TokenIndex> volatile_token;
    struct Components {
        TokenIndex main_token;
        OptionalNodeIndex align_node;
        OptionalNodeIndex addrspace_node;
        OptionalNodeIndex sentinel;
        OptionalNodeIndex bit_range_start;
        OptionalNodeIndex bit_range_end;
        NodeIndex child_type;
    } ast;
};

struct Slice {
    struct Components {
        NodeIndex sliced;
        TokenIndex lbracket;
        NodeIndex start;
        OptionalNodeIndex end;
        OptionalNodeIndex sentinel;
    } ast;
};

struct ContainerDecl {
    std::optional<TokenIndex> layout_token;
    struct Components {
        TokenIndex main_token;
        /// Populated when main_token is keyword_union.
        std::optional<TokenIndex> enum_token;
        std::span<const NodeIndex> members;
        OptionalNodeIndex arg;
    } ast;
};

struct Switch {
    struct Components {
        TokenIndex switch_token;
        NodeIndex condition;
        std::span<const NodeIndex> cases;
    } ast;
    std::optional<TokenIndex> label_token;
};

struct SwitchCase {
    std::optional<TokenIndex> inline_token;
    /// The first token after the `|`: an identifier or a `*`.
    std::optional<TokenIndex> payload_token;
    struct Components {
        /// Empty for an else case.
        std::span<const NodeIndex> values;
        TokenIndex arrow_token;
        NodeIndex target_expr;
    } ast;
};

struct Asm {
    struct Components {
        TokenIndex asm_token;
        NodeIndex template_;
        std::span<const NodeIndex> items;
        OptionalNodeIndex clobbers;
        TokenIndex rparen;
    } ast;
    std::optional<TokenIndex> volatile_token;
    std::span<const NodeIndex> outputs;
    std::span<const NodeIndex> inputs;
};

struct Call {
    struct Components {
        TokenIndex lparen;
        NodeIndex fn_expr;
        std::span<const NodeIndex> params;
    } ast;
};

}  // namespace full

/// Ast.Location (0-based line and column).
struct Location {
    std::size_t line;
    std::size_t column;
    std::size_t line_start;
    std::size_t line_end;
};

/// Ast.Span.
struct Span {
    std::uint32_t start;
    std::uint32_t end;
    std::uint32_t main;
};

struct TokenEntry {
    TokenTag tag;
    ByteOffset start;
};

/// std.zig.Ast. Owns a copy of its source (NUL-terminated, as Zig's
/// `[:0]const u8`), so the tree outlives the caller's buffer.
struct Ast {
    std::string source;
    std::vector<TokenEntry> tokens;
    std::vector<Node> nodes;
    std::vector<std::uint32_t> extra_data;
    Mode mode = Mode::zig;
    std::vector<Error> errors;

    using NodeBuffer1 = std::array<NodeIndex, 1>;
    using NodeBuffer2 = std::array<NodeIndex, 2>;

    TokenTag tokenTag(TokenIndex i) const { return tokens[i].tag; }
    ByteOffset tokenStart(TokenIndex i) const { return tokens[i].start; }
    NodeTag nodeTag(NodeIndex n) const { return nodes[n].tag; }
    TokenIndex nodeMainToken(NodeIndex n) const { return nodes[n].main_token; }
    Data nodeData(NodeIndex n) const { return nodes[n].data; }
    bool isTokenPrecededByTags(TokenIndex ti, std::initializer_list<TokenTag> expected) const;

    /// Extra offset for the column of an error pointing after its token.
    std::uint32_t errorOffset(const Error& parse_error) const;
    Location tokenLocation(ByteOffset start_offset, TokenIndex token_index) const;
    std::string_view tokenSlice(TokenIndex token_index) const;

    std::span<const NodeIndex> extraDataSlice(SubRange range) const;
    std::span<const NodeIndex> extraDataSliceWithLen(ExtraIndex start, std::uint32_t len) const;
    SubRange extraSubRange(ExtraIndex i) const { return SubRange{extra_data[i], extra_data[i + 1]}; }
    extra::LocalVarDecl extraLocalVarDecl(ExtraIndex i) const;
    extra::ArrayTypeSentinel extraArrayTypeSentinel(ExtraIndex i) const;
    extra::PtrType extraPtrType(ExtraIndex i) const;
    extra::PtrTypeBitRange extraPtrTypeBitRange(ExtraIndex i) const;
    extra::If extraIf(ExtraIndex i) const;
    extra::ContainerField extraContainerField(ExtraIndex i) const;
    extra::GlobalVarDecl extraGlobalVarDecl(ExtraIndex i) const;
    extra::Slice extraSlice(ExtraIndex i) const;
    extra::SliceSentinel extraSliceSentinel(ExtraIndex i) const;
    extra::While extraWhile(ExtraIndex i) const;
    extra::WhileCont extraWhileCont(ExtraIndex i) const;
    extra::FnProtoOne extraFnProtoOne(ExtraIndex i) const;
    extra::FnProto extraFnProto(ExtraIndex i) const;
    extra::Asm extraAsm(ExtraIndex i) const;

    std::span<const NodeIndex> rootDecls() const;
    /// Ast.renderError: the message text, identical to Zig's.
    std::string renderError(const Error& parse_error) const;
    TokenIndex firstToken(NodeIndex node) const;
    TokenIndex lastToken(NodeIndex node) const;
    bool tokensOnSameLine(TokenIndex token1, TokenIndex token2) const;
    std::string_view getNodeSource(NodeIndex node) const;

    full::VarDecl globalVarDecl(NodeIndex node) const;
    full::VarDecl localVarDecl(NodeIndex node) const;
    full::VarDecl simpleVarDecl(NodeIndex node) const;
    full::VarDecl alignedVarDecl(NodeIndex node) const;
    full::AssignDestructure assignDestructure(NodeIndex node) const;
    full::If ifSimple(NodeIndex node) const;
    full::If ifFull(NodeIndex node) const;
    full::ContainerField containerField(NodeIndex node) const;
    full::ContainerField containerFieldInit(NodeIndex node) const;
    full::ContainerField containerFieldAlign(NodeIndex node) const;
    full::FnProto fnProtoSimple(NodeBuffer1& buffer, NodeIndex node) const;
    full::FnProto fnProtoMulti(NodeIndex node) const;
    full::FnProto fnProtoOne(NodeBuffer1& buffer, NodeIndex node) const;
    full::FnProto fnProto(NodeIndex node) const;
    full::StructInit structInitOne(NodeIndex* buffer, NodeIndex node) const;
    full::StructInit structInitDotTwo(NodeBuffer2& buffer, NodeIndex node) const;
    full::StructInit structInitDot(NodeIndex node) const;
    full::StructInit structInit(NodeIndex node) const;
    full::ArrayInit arrayInitOne(NodeIndex* buffer, NodeIndex node) const;
    full::ArrayInit arrayInitDotTwo(NodeBuffer2& buffer, NodeIndex node) const;
    full::ArrayInit arrayInitDot(NodeIndex node) const;
    full::ArrayInit arrayInit(NodeIndex node) const;
    full::ArrayType arrayType(NodeIndex node) const;
    full::ArrayType arrayTypeSentinel(NodeIndex node) const;
    full::PtrType ptrTypeAligned(NodeIndex node) const;
    full::PtrType ptrTypeSentinel(NodeIndex node) const;
    full::PtrType ptrType(NodeIndex node) const;
    full::PtrType ptrTypeBitRange(NodeIndex node) const;
    full::Slice sliceOpen(NodeIndex node) const;
    full::Slice slice(NodeIndex node) const;
    full::Slice sliceSentinel(NodeIndex node) const;
    full::ContainerDecl containerDeclTwo(NodeBuffer2& buffer, NodeIndex node) const;
    full::ContainerDecl containerDecl(NodeIndex node) const;
    full::ContainerDecl containerDeclArg(NodeIndex node) const;
    full::ContainerDecl containerDeclRoot() const;
    full::ContainerDecl taggedUnionTwo(NodeBuffer2& buffer, NodeIndex node) const;
    full::ContainerDecl taggedUnion(NodeIndex node) const;
    full::ContainerDecl taggedUnionEnumTag(NodeIndex node) const;
    full::Switch switchFull(NodeIndex node) const;
    full::SwitchCase switchCaseOne(NodeIndex node) const;
    full::SwitchCase switchCase(NodeIndex node) const;
    full::Asm asmSimple(NodeIndex node) const;
    full::Asm asmFull(NodeIndex node) const;
    full::While whileSimple(NodeIndex node) const;
    full::While whileCont(NodeIndex node) const;
    full::While whileFull(NodeIndex node) const;
    full::For forSimple(NodeIndex node) const;
    full::For forFull(NodeIndex node) const;
    full::Call callOne(NodeBuffer1& buffer, NodeIndex node) const;
    full::Call callFull(NodeIndex node) const;

    std::optional<full::VarDecl> fullVarDecl(NodeIndex node) const;
    std::optional<full::If> fullIf(NodeIndex node) const;
    std::optional<full::While> fullWhile(NodeIndex node) const;
    std::optional<full::For> fullFor(NodeIndex node) const;
    std::optional<full::ContainerField> fullContainerField(NodeIndex node) const;
    std::optional<full::FnProto> fullFnProto(NodeBuffer1& buffer, NodeIndex node) const;
    std::optional<full::StructInit> fullStructInit(NodeBuffer2& buffer, NodeIndex node) const;
    std::optional<full::ArrayInit> fullArrayInit(NodeBuffer2& buffer, NodeIndex node) const;
    std::optional<full::ArrayType> fullArrayType(NodeIndex node) const;
    std::optional<full::PtrType> fullPtrType(NodeIndex node) const;
    std::optional<full::Slice> fullSlice(NodeIndex node) const;
    std::optional<full::ContainerDecl> fullContainerDecl(NodeBuffer2& buffer, NodeIndex node) const;
    std::optional<full::Switch> fullSwitch(NodeIndex node) const;
    std::optional<full::SwitchCase> fullSwitchCase(NodeIndex node) const;
    std::optional<full::Asm> fullAsm(NodeIndex node) const;
    std::optional<full::Call> fullCall(NodeBuffer1& buffer, NodeIndex node) const;
    std::optional<std::span<const NodeIndex>> builtinCallParams(NodeBuffer2& buffer, NodeIndex node) const;
    std::optional<std::span<const NodeIndex>> blockStatements(NodeBuffer2& buffer, NodeIndex node) const;

    Span nodeToSpan(NodeIndex node) const;
    Span tokenToSpan(TokenIndex token) const;
    Span tokensToSpan(TokenIndex start, TokenIndex end, TokenIndex main) const;

  private:
    full::VarDecl fullVarDeclComponents(const full::VarDecl::Components& info) const;
    full::AssignDestructure fullAssignDestructureComponents(const full::AssignDestructure::Components& info) const;
    full::If fullIfComponents(const full::If::Components& info) const;
    full::ContainerField fullContainerFieldComponents(const full::ContainerField::Components& info) const;
    full::FnProto fullFnProtoComponents(const full::FnProto::Components& info) const;
    full::PtrType fullPtrTypeComponents(const full::PtrType::Components& info) const;
    full::ContainerDecl fullContainerDeclComponents(const full::ContainerDecl::Components& info) const;
    full::Switch fullSwitchComponents(const full::Switch::Components& info) const;
    full::SwitchCase fullSwitchCaseComponents(const full::SwitchCase::Components& info, NodeIndex node) const;
    full::Asm fullAsmComponents(const full::Asm::Components& info) const;
    full::While fullWhileComponents(const full::While::Components& info) const;
    full::For fullForComponents(const full::For::Components& info) const;
};

/// Ast.parse: tokenize and parse `source` (copied into the result).
Ast parse(std::string_view source, Mode mode = Mode::zig);

/// One error as `zig fmt` / `zig ast-check` print it (their AstGen
/// lowerAstErrors): the error's byte offset, 0-based line/column, the
/// message, and whether it is a note attached to the previous error.
struct RenderedError {
    std::size_t byte_offset;
    std::size_t line;
    std::size_t column;
    std::string message;
    bool is_note;
};

/// The tree's parse errors as Zig reports them: errors[0] gets the
/// "contains invalid byte" treatment when it sits on an invalid literal or
/// comment token, every other error is placed at its token (after it when
/// token_is_prev), notes follow the error they belong to.
std::vector<RenderedError> renderErrors(const Ast& tree);

}  // namespace zig
