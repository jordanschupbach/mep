/**
 * @file zig_parse.cpp
 * @brief Zig's parser (lib/std/zig/Parse.zig, Zig 0.16), ported decision for
 *        decision, plus Ast.parse.
 *
 * Ported from Zig, MIT license, Copyright (c) Zig contributors; the notice is
 * in zig_ast.h. The function names and their order are Parse.zig's. Mapping:
 *
 *  - `error.ParseError` is the C++ exception ParseError; `catch |err| switch`
 *    blocks are try/catch, `errdefer p.unreserveNode(i)` is a catch-and-rethrow.
 *    Out-of-memory is std::bad_alloc, which nothing here catches.
 *  - `?Node.Index` is a NodeIndex where kNone means null (the `orelse return
 *    null` chains read the same); Node.OptionalIndex is OptionalIndex.
 *  - Zig evaluates a struct literal's fields in order; C++ does not promise
 *    that for function arguments, so wherever a node's fields each consume
 *    tokens the values are computed into locals first, in Zig's order.
 *  - Nesting is capped at kMaxNesting (see zig_ast.h): exceeding it throws
 *    NestingLimit, which unwinds the whole parse.
 */
#include "zig_ast.h"

#include <cassert>
#include <cstring>

namespace zig {

namespace {

struct ParseError {};
struct NestingLimit {};

constexpr NodeIndex kNull = kNone;

struct OperInfo {
    std::int8_t prec;
    NodeTag tag;
    bool assoc_none;
};

OperInfo operInfo(TokenTag t) {
    switch (t) {
        case TokenTag::keyword_or: return {10, NodeTag::bool_or, false};
        case TokenTag::keyword_and: return {20, NodeTag::bool_and, false};
        case TokenTag::equal_equal: return {30, NodeTag::equal_equal, true};
        case TokenTag::bang_equal: return {30, NodeTag::bang_equal, true};
        case TokenTag::angle_bracket_left: return {30, NodeTag::less_than, true};
        case TokenTag::angle_bracket_right: return {30, NodeTag::greater_than, true};
        case TokenTag::angle_bracket_left_equal: return {30, NodeTag::less_or_equal, true};
        case TokenTag::angle_bracket_right_equal: return {30, NodeTag::greater_or_equal, true};
        case TokenTag::ampersand: return {40, NodeTag::bit_and, false};
        case TokenTag::caret: return {40, NodeTag::bit_xor, false};
        case TokenTag::pipe: return {40, NodeTag::bit_or, false};
        case TokenTag::keyword_orelse: return {40, NodeTag::orelse_, false};
        case TokenTag::keyword_catch: return {40, NodeTag::catch_, false};
        case TokenTag::angle_bracket_angle_bracket_left: return {50, NodeTag::shl, false};
        case TokenTag::angle_bracket_angle_bracket_left_pipe: return {50, NodeTag::shl_sat, false};
        case TokenTag::angle_bracket_angle_bracket_right: return {50, NodeTag::shr, false};
        case TokenTag::plus: return {60, NodeTag::add, false};
        case TokenTag::minus: return {60, NodeTag::sub, false};
        case TokenTag::plus_plus: return {60, NodeTag::array_cat, false};
        case TokenTag::plus_percent: return {60, NodeTag::add_wrap, false};
        case TokenTag::minus_percent: return {60, NodeTag::sub_wrap, false};
        case TokenTag::plus_pipe: return {60, NodeTag::add_sat, false};
        case TokenTag::minus_pipe: return {60, NodeTag::sub_sat, false};
        case TokenTag::pipe_pipe: return {70, NodeTag::merge_error_sets, false};
        case TokenTag::asterisk: return {70, NodeTag::mul, false};
        case TokenTag::slash: return {70, NodeTag::div, false};
        case TokenTag::percent: return {70, NodeTag::mod, false};
        case TokenTag::asterisk_asterisk: return {70, NodeTag::array_mult, false};
        case TokenTag::asterisk_percent: return {70, NodeTag::mul_wrap, false};
        case TokenTag::asterisk_pipe: return {70, NodeTag::mul_sat, false};
        default: return {-1, NodeTag::root, false};
    }
}

bool isWhitespace(char c) {
    // std.ascii.whitespace: ' ', '\t', '\n', '\r', VT, FF
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x0B || c == 0x0C;
}

std::optional<NodeTag> assignOpNode(TokenTag tok) {
    switch (tok) {
        case TokenTag::asterisk_equal: return NodeTag::assign_mul;
        case TokenTag::slash_equal: return NodeTag::assign_div;
        case TokenTag::percent_equal: return NodeTag::assign_mod;
        case TokenTag::plus_equal: return NodeTag::assign_add;
        case TokenTag::minus_equal: return NodeTag::assign_sub;
        case TokenTag::angle_bracket_angle_bracket_left_equal: return NodeTag::assign_shl;
        case TokenTag::angle_bracket_angle_bracket_left_pipe_equal: return NodeTag::assign_shl_sat;
        case TokenTag::angle_bracket_angle_bracket_right_equal: return NodeTag::assign_shr;
        case TokenTag::ampersand_equal: return NodeTag::assign_bit_and;
        case TokenTag::caret_equal: return NodeTag::assign_bit_xor;
        case TokenTag::pipe_equal: return NodeTag::assign_bit_or;
        case TokenTag::asterisk_percent_equal: return NodeTag::assign_mul_wrap;
        case TokenTag::plus_percent_equal: return NodeTag::assign_add_wrap;
        case TokenTag::minus_percent_equal: return NodeTag::assign_sub_wrap;
        case TokenTag::asterisk_pipe_equal: return NodeTag::assign_mul_sat;
        case TokenTag::plus_pipe_equal: return NodeTag::assign_add_sat;
        case TokenTag::minus_pipe_equal: return NodeTag::assign_sub_sat;
        case TokenTag::equal: return NodeTag::assign;
        default: return std::nullopt;
    }
}

OptionalIndex opt(NodeIndex n) { return OptionalIndex::from(n); }

struct Parse {
    std::string_view source;
    const std::vector<TokenEntry>& tokens;
    TokenIndex tok_i = 0;
    std::vector<Error> errors;
    std::vector<Node> nodes;
    std::vector<std::uint32_t> extra_data;
    std::vector<NodeIndex> scratch;
    std::uint32_t depth = 0;

    explicit Parse(std::string_view src, const std::vector<TokenEntry>& toks) : source(src), tokens(toks) {}

    // RAII guard for the nesting cap.
    struct Nest {
        Parse& p;
        explicit Nest(Parse& parser) : p(parser) {
            if (++p.depth > kMaxNesting) {
                --p.depth;
                throw NestingLimit{};
            }
        }
        ~Nest() { --p.depth; }
        Nest(const Nest&) = delete;
        Nest& operator=(const Nest&) = delete;
    };

    // `defer p.scratch.shrinkRetainingCapacity(scratch_top)`.
    struct ScratchTop {
        Parse& p;
        std::size_t top;
        explicit ScratchTop(Parse& parser) : p(parser), top(parser.scratch.size()) {}
        ~ScratchTop() { p.scratch.resize(top); }
        ScratchTop(const ScratchTop&) = delete;
        ScratchTop& operator=(const ScratchTop&) = delete;
    };

    TokenTag tokenTag(TokenIndex i) const { return tokens[i].tag; }
    ByteOffset tokenStart(TokenIndex i) const { return tokens[i].start; }
    NodeTag nodeTag(NodeIndex n) const { return nodes[n].tag; }
    TokenIndex nodeMainToken(NodeIndex n) const { return nodes[n].main_token; }
    Data nodeData(NodeIndex n) const { return nodes[n].data; }

    // SmallSpan
    struct SmallSpan {
        bool multi;
        OptionalIndex zero_or_one;
        SubRange span;
    };

    struct Members {
        std::size_t len;
        /// opt_node_and_opt_node if len <= 2, else extra_range.
        Data data;
        bool trailing;

        SubRange toSpan(Parse& p) const {
            switch (len) {
                case 0: return p.listToSpan({});
                case 1: {
                    NodeIndex a[1] = {data.lhs};
                    return p.listToSpan(a);
                }
                case 2: {
                    NodeIndex a[2] = {data.lhs, data.rhs};
                    return p.listToSpan(a);
                }
                default: return data.extra_range();
            }
        }
    };

    SubRange listToSpan(std::span<const NodeIndex> list) {
        // Copy first: `list` may alias scratch, never extra_data, but be safe.
        std::vector<NodeIndex> copy(list.begin(), list.end());
        extra_data.insert(extra_data.end(), copy.begin(), copy.end());
        auto end = static_cast<ExtraIndex>(extra_data.size());
        return SubRange{static_cast<ExtraIndex>(end - copy.size()), end};
    }

    NodeIndex addNode(Node elem) {
        auto result = static_cast<NodeIndex>(nodes.size());
        nodes.push_back(elem);
        return result;
    }

    NodeIndex setNode(std::size_t i, Node elem) {
        nodes[i] = elem;
        return static_cast<NodeIndex>(i);
    }

    std::size_t reserveNode(NodeTag tag) {
        nodes.push_back(Node{tag, 0, Data::undef()});
        return nodes.size() - 1;
    }

    void unreserveNode(std::size_t node_index) {
        if (nodes.size() == node_index) {
            nodes.pop_back();
        } else {
            // A zombie node is left in the tree; make it as inoffensive as
            // possible (there is no no-op node).
            nodes[node_index].tag = NodeTag::unreachable_literal;
            nodes[node_index].main_token = tok_i;
        }
    }

    ExtraIndex addExtra(std::initializer_list<std::uint32_t> fields) {
        auto result = static_cast<ExtraIndex>(extra_data.size());
        extra_data.insert(extra_data.end(), fields.begin(), fields.end());
        return result;
    }
    ExtraIndex addExtraSubRange(SubRange r) { return addExtra({r.start, r.end}); }

    // ---- errors -----------------------------------------------------------

    void warnExpected(TokenTag expected_token) {
        Error e{};
        e.tag = ErrorTag::expected_token;
        e.token = tok_i;
        e.expected_tag = expected_token;
        warnMsg(e);
    }

    void warn(ErrorTag error_tag) {
        Error e{};
        e.tag = error_tag;
        e.token = tok_i;
        warnMsg(e);
    }

    void warnMsg(Error msg) {
        switch (msg.tag) {
            case ErrorTag::expected_semi_after_decl:
            case ErrorTag::expected_semi_after_stmt:
            case ErrorTag::expected_comma_after_field:
            case ErrorTag::expected_comma_after_arg:
            case ErrorTag::expected_comma_after_param:
            case ErrorTag::expected_comma_after_initializer:
            case ErrorTag::expected_comma_after_switch_prong:
            case ErrorTag::expected_comma_after_for_operand:
            case ErrorTag::expected_comma_after_capture:
            case ErrorTag::expected_semi_or_else:
            case ErrorTag::expected_semi_or_lbrace:
            case ErrorTag::expected_token:
            case ErrorTag::expected_block:
            case ErrorTag::expected_block_or_assignment:
            case ErrorTag::expected_block_or_expr:
            case ErrorTag::expected_block_or_field:
            case ErrorTag::expected_expr:
            case ErrorTag::expected_expr_or_assignment:
            case ErrorTag::expected_fn:
            case ErrorTag::expected_inlinable:
            case ErrorTag::expected_labelable:
            case ErrorTag::expected_param_list:
            case ErrorTag::expected_prefix_expr:
            case ErrorTag::expected_primary_type_expr:
            case ErrorTag::expected_pub_item:
            case ErrorTag::expected_return_type:
            case ErrorTag::expected_suffix_op:
            case ErrorTag::expected_type_expr:
            case ErrorTag::expected_var_decl:
            case ErrorTag::expected_var_decl_or_fn:
            case ErrorTag::expected_loop_payload:
            case ErrorTag::expected_container:
                if (msg.token != 0 && !tokensOnSameLine(msg.token - 1, msg.token)) {
                    Error copy = msg;
                    copy.token_is_prev = true;
                    copy.token -= 1;
                    errors.push_back(copy);
                    return;
                }
                break;
            default: break;
        }
        errors.push_back(msg);
    }

    [[noreturn]] void fail(ErrorTag tag) {
        Error e{};
        e.tag = tag;
        e.token = tok_i;
        failMsg(e);
    }

    [[noreturn]] void failExpected(TokenTag expected_token) {
        Error e{};
        e.tag = ErrorTag::expected_token;
        e.token = tok_i;
        e.expected_tag = expected_token;
        failMsg(e);
    }

    [[noreturn]] void failMsg(Error msg) {
        warnMsg(msg);
        throw ParseError{};
    }

    // ---- root ---------------------------------------------------------------

    /// Root <- skip ContainerMembers eof
    void parseRoot() {
        // Root node must be index 0.
        nodes.push_back(Node{NodeTag::root, 0, Data::undef()});
        Members root_members = parseContainerMembers();
        SubRange root_decls = root_members.toSpan(*this);
        if (tokenTag(tok_i) != TokenTag::eof) warnExpected(TokenTag::eof);
        nodes[0].data = Data::pair(root_decls.start, root_decls.end);
    }

    /// Parse in ZON mode. Subset of the language.
    void parseZon() {
        // We must use index 0 so that 0 can be used as null elsewhere.
        nodes.push_back(Node{NodeTag::root, 0, Data::undef()});
        NodeIndex node_index;
        try {
            node_index = expectExpr();
        } catch (const ParseError&) {
            assert(!errors.empty());
            return;
        }
        if (tokenTag(tok_i) != TokenTag::eof) warnExpected(TokenTag::eof);
        nodes[0].data.lhs = node_index;
    }

    /// ContainerMembers <- container_doc_comment? ContainerDeclaration* (ContainerField COMMA)* (ContainerField / ContainerDeclaration*)
    Members parseContainerMembers() {
        Nest nest(*this);
        ScratchTop st(*this);
        const std::size_t scratch_top = st.top;

        enum class FieldState { none, seen, end, err };
        FieldState field_state = FieldState::none;
        NodeIndex field_state_end = 0;  // payload of .end

        TokenIndex last_field = 0;

        // Skip container doc comments.
        while (eatToken(TokenTag::container_doc_comment) != kNone) {
        }

        bool trailing = false;
        while (true) {
            std::optional<TokenIndex> doc_comment = eatDocComments();

            TokenTag sw = tokenTag(tok_i);
        sw_again:
            switch (sw) {
                case TokenTag::keyword_test: {
                    if (doc_comment) {
                        Error e{};
                        e.tag = ErrorTag::test_doc_comment;
                        e.token = *doc_comment;
                        warnMsg(e);
                    }
                    NodeIndex maybe_test_decl_node = expectTestDeclRecoverable();
                    if (maybe_test_decl_node != kNull) {
                        if (field_state == FieldState::seen) {
                            field_state = FieldState::end;
                            field_state_end = maybe_test_decl_node;
                        }
                        scratch.push_back(maybe_test_decl_node);
                    }
                    trailing = false;
                    break;
                }
                case TokenTag::keyword_comptime:
                    if (tokenTag(tok_i + 1) == TokenTag::l_brace) {
                        if (doc_comment) {
                            Error e{};
                            e.tag = ErrorTag::comptime_doc_comment;
                            e.token = *doc_comment;
                            warnMsg(e);
                        }
                        TokenIndex comptime_token = nextToken();
                        NodeIndex opt_block;
                        try {
                            opt_block = parseBlock();
                        } catch (const ParseError&) {
                            findNextContainerMember();
                            opt_block = kNull;
                        }
                        if (opt_block != kNull) {
                            NodeIndex comptime_node =
                                addNode(Node{NodeTag::comptime_, comptime_token, Data::pair(opt_block, 0xAAAAAAAAu)});
                            if (field_state == FieldState::seen) {
                                field_state = FieldState::end;
                                field_state_end = comptime_node;
                            }
                            scratch.push_back(comptime_node);
                        }
                        trailing = false;
                        break;
                    } else {
                        const TokenIndex identifier = tok_i;
                        NodeIndex container_field;
                        try {
                            container_field = expectContainerField();
                        } catch (const ParseError&) {
                            last_field = identifier;
                            findNextContainerMember();
                            continue;
                        }
                        switch (field_state) {
                            case FieldState::none: field_state = FieldState::seen; break;
                            case FieldState::err:
                            case FieldState::seen: break;
                            case FieldState::end: {
                                Error e1{};
                                e1.tag = ErrorTag::decl_between_fields;
                                e1.token = nodeMainToken(field_state_end);
                                warnMsg(e1);
                                Error e2{};
                                e2.tag = ErrorTag::previous_field;
                                e2.is_note = true;
                                e2.token = last_field;
                                warnMsg(e2);
                                Error e3{};
                                e3.tag = ErrorTag::next_field;
                                e3.is_note = true;
                                e3.token = identifier;
                                warnMsg(e3);
                                // Continue parsing; error will be reported later.
                                field_state = FieldState::err;
                                break;
                            }
                        }
                        scratch.push_back(container_field);
                        bool brk = false;
                        switch (tokenTag(tok_i)) {
                            case TokenTag::comma:
                                tok_i += 1;
                                trailing = true;
                                last_field = identifier;
                                continue;
                            case TokenTag::r_brace:
                            case TokenTag::eof:
                                trailing = false;
                                brk = true;
                                break;
                            default: break;
                        }
                        last_field = identifier;
                        if (brk) goto loop_end;
                        // There is not allowed to be a decl after a field with no comma.
                        // Report error but recover parser.
                        warn(ErrorTag::expected_comma_after_field);
                        findNextContainerMember();
                        break;
                    }
                case TokenTag::keyword_pub:
                case TokenTag::keyword_const:
                case TokenTag::keyword_var:
                case TokenTag::keyword_threadlocal:
                case TokenTag::keyword_export:
                case TokenTag::keyword_extern:
                case TokenTag::keyword_inline:
                case TokenTag::keyword_noinline:
                case TokenTag::keyword_fn: {
                    const TokenTag t = sw;
                    if (t == TokenTag::keyword_extern) {
                        switch (tokenTag(tok_i + 1)) {
                            case TokenTag::keyword_struct:
                            case TokenTag::keyword_union:
                            case TokenTag::keyword_enum:
                            case TokenTag::keyword_opaque: sw = tokenTag(tok_i + 1); goto sw_again;
                            default: break;
                        }
                    }
                    if (t == TokenTag::keyword_inline) {
                        switch (tokenTag(tok_i + 1)) {
                            case TokenTag::keyword_for:
                            case TokenTag::keyword_while: sw = tokenTag(tok_i + 1); goto sw_again;
                            default: break;
                        }
                    }

                    tok_i += (t == TokenTag::keyword_pub) ? 1u : 0u;
                    NodeIndex opt_top_level_decl = expectTopLevelDeclRecoverable();
                    if (opt_top_level_decl != kNull) {
                        if (field_state == FieldState::seen) {
                            field_state = FieldState::end;
                            field_state_end = opt_top_level_decl;
                        }
                        scratch.push_back(opt_top_level_decl);
                    }
                    trailing = tokenTag(tok_i - 1) == TokenTag::semicolon;
                    break;
                }
                case TokenTag::eof:
                case TokenTag::r_brace:
                    if (doc_comment) {
                        Error e{};
                        e.tag = ErrorTag::unattached_doc_comment;
                        e.token = *doc_comment;
                        warnMsg(e);
                    }
                    goto loop_end;
                default: {
                    bool c_container;
                    try {
                        c_container = parseCStyleContainer();
                    } catch (const ParseError&) {
                        c_container = false;
                    }
                    if (c_container) continue;

                    const TokenIndex identifier = tok_i;
                    NodeIndex container_field;
                    try {
                        container_field = expectContainerField();
                    } catch (const ParseError&) {
                        last_field = identifier;
                        findNextContainerMember();
                        continue;
                    }
                    switch (field_state) {
                        case FieldState::none: field_state = FieldState::seen; break;
                        case FieldState::err:
                        case FieldState::seen: break;
                        case FieldState::end: {
                            Error e1{};
                            e1.tag = ErrorTag::decl_between_fields;
                            e1.token = nodeMainToken(field_state_end);
                            warnMsg(e1);
                            Error e2{};
                            e2.tag = ErrorTag::previous_field;
                            e2.is_note = true;
                            e2.token = last_field;
                            warnMsg(e2);
                            Error e3{};
                            e3.tag = ErrorTag::next_field;
                            e3.is_note = true;
                            e3.token = identifier;
                            warnMsg(e3);
                            // Continue parsing; error will be reported later.
                            field_state = FieldState::err;
                            break;
                        }
                    }
                    scratch.push_back(container_field);
                    bool brk = false;
                    switch (tokenTag(tok_i)) {
                        case TokenTag::comma:
                            tok_i += 1;
                            trailing = true;
                            last_field = identifier;
                            continue;
                        case TokenTag::r_brace:
                        case TokenTag::eof:
                            trailing = false;
                            brk = true;
                            break;
                        default: break;
                    }
                    last_field = identifier;
                    if (brk) goto loop_end;
                    // There is not allowed to be a decl after a field with no comma.
                    // Report error but recover parser.
                    warn(ErrorTag::expected_comma_after_field);
                    if (tokenTag(tok_i) == TokenTag::semicolon && tokenTag(identifier) == TokenTag::identifier) {
                        Error e{};
                        e.tag = ErrorTag::var_const_decl;
                        e.is_note = true;
                        e.token = identifier;
                        warnMsg(e);
                    }
                    findNextContainerMember();
                    continue;
                }
            }
        }
    loop_end:

        std::span<const NodeIndex> items(scratch.data() + scratch_top, scratch.size() - scratch_top);
        if (items.size() <= 2) {
            return Members{items.size(),
                           Data::pair(items.size() >= 1 ? items[0] : kNone, items.size() >= 2 ? items[1] : kNone),
                           trailing};
        }
        SubRange r = listToSpan(items);
        return Members{items.size(), Data::pair(r.start, r.end), trailing};
    }

    /// Attempts to find next container member by searching for certain tokens
    void findNextContainerMember() {
        std::uint32_t level = 0;
        while (true) {
            TokenIndex tok = nextToken();
            switch (tokenTag(tok)) {
                // Any of these can start a new top level declaration.
                case TokenTag::keyword_test:
                case TokenTag::keyword_comptime:
                case TokenTag::keyword_pub:
                case TokenTag::keyword_export:
                case TokenTag::keyword_extern:
                case TokenTag::keyword_inline:
                case TokenTag::keyword_noinline:
                case TokenTag::keyword_threadlocal:
                case TokenTag::keyword_const:
                case TokenTag::keyword_var:
                case TokenTag::keyword_fn:
                    if (level == 0) {
                        tok_i -= 1;
                        return;
                    }
                    break;
                case TokenTag::identifier:
                    if (tokenTag(tok + 1) == TokenTag::comma && level == 0) {
                        tok_i -= 1;
                        return;
                    }
                    break;
                case TokenTag::comma:
                case TokenTag::semicolon:
                    // this decl was likely meant to end here
                    if (level == 0) return;
                    break;
                case TokenTag::l_paren:
                case TokenTag::l_bracket:
                case TokenTag::l_brace: level += 1; break;
                case TokenTag::r_paren:
                case TokenTag::r_bracket:
                    if (level != 0) level -= 1;
                    break;
                case TokenTag::r_brace:
                    if (level == 0) {
                        // end of container, exit
                        tok_i -= 1;
                        return;
                    }
                    level -= 1;
                    break;
                case TokenTag::eof: tok_i -= 1; return;
                default: break;
            }
        }
    }

    /// Attempts to find the next statement by searching for a semicolon
    void findNextStmt() {
        std::uint32_t level = 0;
        while (true) {
            TokenIndex tok = nextToken();
            switch (tokenTag(tok)) {
                case TokenTag::l_brace: level += 1; break;
                case TokenTag::r_brace:
                    if (level == 0) {
                        tok_i -= 1;
                        return;
                    }
                    level -= 1;
                    break;
                case TokenTag::semicolon:
                    if (level == 0) return;
                    break;
                case TokenTag::eof: tok_i -= 1; return;
                default: break;
            }
        }
    }

    /// TestDecl <- KEYWORD_test (STRINGLITERALSINGLE / IDENTIFIER)? Block
    NodeIndex expectTestDecl() {
        TokenIndex test_token = assertToken(TokenTag::keyword_test);
        OptionalTokenIndex name_token;
        switch (tokenTag(tok_i)) {
            case TokenTag::string_literal:
            case TokenTag::identifier: name_token = OptionalIndex::from(nextToken()); break;
            default: break;
        }
        NodeIndex block_node = parseBlock();
        if (block_node == kNull) fail(ErrorTag::expected_block);
        return addNode(Node{NodeTag::test_decl, test_token, Data::pair(name_token.value, block_node)});
    }

    NodeIndex expectTestDeclRecoverable() {
        try {
            return expectTestDecl();
        } catch (const ParseError&) {
            findNextContainerMember();
            return kNull;
        }
    }

    /// Decl
    NodeIndex expectTopLevelDecl() {
        TokenIndex extern_export_inline_token = nextToken();
        bool is_extern = false;
        bool expect_fn = false;
        bool expect_var_or_fn = false;
        switch (tokenTag(extern_export_inline_token)) {
            case TokenTag::keyword_extern:
                eatToken(TokenTag::string_literal);
                is_extern = true;
                expect_var_or_fn = true;
                break;
            case TokenTag::keyword_export: expect_var_or_fn = true; break;
            case TokenTag::keyword_inline:
            case TokenTag::keyword_noinline: expect_fn = true; break;
            default: tok_i -= 1; break;
        }
        NodeIndex opt_fn_proto = parseFnProto();
        if (opt_fn_proto != kNull) {
            NodeIndex fn_proto = opt_fn_proto;
            switch (tokenTag(tok_i)) {
                case TokenTag::semicolon: tok_i += 1; return fn_proto;
                case TokenTag::l_brace: {
                    if (is_extern) {
                        Error e{};
                        e.tag = ErrorTag::extern_fn_body;
                        e.token = extern_export_inline_token;
                        warnMsg(e);
                        return kNull;
                    }
                    std::size_t fn_decl_index = reserveNode(NodeTag::fn_decl);
                    NodeIndex body_block;
                    try {
                        body_block = parseBlock();
                    } catch (...) {
                        unreserveNode(fn_decl_index);
                        throw;
                    }
                    return setNode(fn_decl_index,
                                   Node{NodeTag::fn_decl, nodeMainToken(fn_proto), Data::pair(fn_proto, body_block)});
                }
                default:
                    // Since parseBlock only return error.ParseError on
                    // a missing '}' we can assume this function was
                    // supposed to end here.
                    warn(ErrorTag::expected_semi_or_lbrace);
                    return kNull;
            }
        }
        if (expect_fn) {
            warn(ErrorTag::expected_fn);
            throw ParseError{};
        }

        TokenIndex thread_local_token = eatToken(TokenTag::keyword_threadlocal);
        NodeIndex var_decl = parseGlobalVarDecl();
        if (var_decl != kNull) return var_decl;
        if (thread_local_token != kNone) fail(ErrorTag::expected_var_decl);
        if (expect_var_or_fn) fail(ErrorTag::expected_var_decl_or_fn);
        fail(ErrorTag::expected_pub_item);
    }

    NodeIndex expectTopLevelDeclRecoverable() {
        try {
            return expectTopLevelDecl();
        } catch (const ParseError&) {
            findNextContainerMember();
            return kNull;
        }
    }

    /// FnProto <- KEYWORD_fn IDENTIFIER? LPAREN ParamDeclList RPAREN ByteAlign? AddrSpace? LinkSection? CallConv? EXCLAMATIONMARK? TypeExpr !ExprSuffix
    NodeIndex parseFnProto() {
        TokenIndex fn_token = eatToken(TokenTag::keyword_fn);
        if (fn_token == kNone) return kNull;

        // We want the fn proto node to be before its children in the array.
        std::size_t fn_proto_index = reserveNode(NodeTag::fn_proto);
        try {
            eatToken(TokenTag::identifier);
            SmallSpan params = parseParamDeclList();
            NodeIndex align_expr = parseByteAlign();
            NodeIndex addrspace_expr = parseAddrSpace();
            NodeIndex section_expr = parseLinkSection();
            NodeIndex callconv_expr = parseCallconv();
            eatToken(TokenTag::bang);

            NodeIndex return_type_expr = parseTypeExpr();
            if (return_type_expr == kNull) {
                // most likely the user forgot to specify the return type.
                // Mark return type as invalid and try to continue.
                warn(ErrorTag::expected_return_type);
            }

            if (align_expr == kNull && section_expr == kNull && callconv_expr == kNull && addrspace_expr == kNull) {
                if (!params.multi) {
                    return setNode(fn_proto_index, Node{NodeTag::fn_proto_simple, fn_token,
                                                        Data::pair(params.zero_or_one.value, return_type_expr)});
                }
                ExtraIndex e = addExtraSubRange(params.span);
                return setNode(fn_proto_index, Node{NodeTag::fn_proto_multi, fn_token, Data::pair(e, return_type_expr)});
            }
            if (!params.multi) {
                ExtraIndex e = addExtra({params.zero_or_one.value, align_expr, addrspace_expr, section_expr, callconv_expr});
                return setNode(fn_proto_index, Node{NodeTag::fn_proto_one, fn_token, Data::pair(e, return_type_expr)});
            }
            ExtraIndex e =
                addExtra({params.span.start, params.span.end, align_expr, addrspace_expr, section_expr, callconv_expr});
            return setNode(fn_proto_index, Node{NodeTag::fn_proto, fn_token, Data::pair(e, return_type_expr)});
        } catch (...) {
            unreserveNode(fn_proto_index);
            throw;
        }
    }

    void setVarDeclInitExpr(NodeIndex var_decl, OptionalIndex init_expr) {
        switch (nodeTag(var_decl)) {
            case NodeTag::simple_var_decl:
            case NodeTag::aligned_var_decl:
            case NodeTag::local_var_decl:
            case NodeTag::global_var_decl: nodes[var_decl].data.rhs = init_expr.value; break;
            default: assert(false);
        }
    }

    /// VarDeclProto <- (KEYWORD_const / KEYWORD_var) IDENTIFIER (COLON TypeExpr)? ByteAlign? AddrSpace? LinkSection?
    /// Returns a `*_var_decl` node with its rhs (init expression) initialized to .none.
    NodeIndex parseVarDeclProto() {
        TokenIndex mut_token = eatToken(TokenTag::keyword_const);
        if (mut_token == kNone) mut_token = eatToken(TokenTag::keyword_var);
        if (mut_token == kNone) return kNull;

        expectToken(TokenTag::identifier);
        NodeIndex opt_type_node = eatToken(TokenTag::colon) == kNone ? kNull : expectTypeExpr();
        NodeIndex opt_align_node = parseByteAlign();
        NodeIndex opt_addrspace_node = parseAddrSpace();
        NodeIndex opt_section_node = parseLinkSection();

        if (opt_section_node == kNull && opt_addrspace_node == kNull) {
            if (opt_align_node == kNull)
                return addNode(Node{NodeTag::simple_var_decl, mut_token, Data::pair(opt_type_node, kNone)});
            if (opt_type_node == kNull)
                return addNode(Node{NodeTag::aligned_var_decl, mut_token, Data::pair(opt_align_node, kNone)});
            ExtraIndex e = addExtra({opt_type_node, opt_align_node});
            return addNode(Node{NodeTag::local_var_decl, mut_token, Data::pair(e, kNone)});
        }
        ExtraIndex e = addExtra({opt_type_node, opt_align_node, opt_addrspace_node, opt_section_node});
        return addNode(Node{NodeTag::global_var_decl, mut_token, Data::pair(e, kNone)});
    }

    /// GlobalVarDecl <- VarDeclProto (EQUAL Expr?) SEMICOLON
    NodeIndex parseGlobalVarDecl() {
        NodeIndex var_decl = parseVarDeclProto();
        if (var_decl == kNull) return kNull;

        NodeIndex init_node = kNull;
        switch (tokenTag(tok_i)) {
            case TokenTag::equal_equal:
                warn(ErrorTag::wrong_equal_var_decl);
                tok_i += 1;
                init_node = expectExpr();
                break;
            case TokenTag::equal:
                tok_i += 1;
                init_node = expectExpr();
                break;
            default: break;
        }

        setVarDeclInitExpr(var_decl, opt(init_node));

        expectSemicolon(ErrorTag::expected_semi_after_decl, false);
        return var_decl;
    }

    /// ContainerField <- doc_comment? (KEYWORD_comptime / !KEYWORD_comptime) !KEYWORD_fn (IDENTIFIER COLON / !(IDENTIFIER COLON))? TypeExpr ByteAlign? (EQUAL Expr)?
    NodeIndex expectContainerField() {
        eatToken(TokenTag::keyword_comptime);
        const TokenIndex main_token = tok_i;
        eatTokens({TokenTag::identifier, TokenTag::colon});
        NodeIndex type_expr = expectTypeExpr();
        NodeIndex align_expr = parseByteAlign();
        NodeIndex value_expr = eatToken(TokenTag::equal) == kNone ? kNull : expectExpr();

        if (align_expr == kNull)
            return addNode(Node{NodeTag::container_field_init, main_token, Data::pair(type_expr, value_expr)});
        if (value_expr == kNull)
            return addNode(Node{NodeTag::container_field_align, main_token, Data::pair(type_expr, align_expr)});
        ExtraIndex e = addExtra({align_expr, value_expr});
        return addNode(Node{NodeTag::container_field, main_token, Data::pair(type_expr, e)});
    }

    NodeIndex addUnary(NodeTag tag, TokenIndex main_token, NodeIndex child) {
        return addNode(Node{tag, main_token, Data::pair(child, 0xAAAAAAAAu)});
    }

    /// BlockStatement / Statement / ExprStatement
    NodeIndex expectStatement(bool is_block_level) {
        Nest nest(*this);
        TokenIndex comptime_token = eatToken(TokenTag::keyword_comptime);
        if (comptime_token != kNone) {
            NodeIndex opt_block_expr = parseBlockExpr();
            if (opt_block_expr != kNull) return addUnary(NodeTag::comptime_, comptime_token, opt_block_expr);

            if (is_block_level) return expectVarDeclExprStatement(comptime_token);
            NodeIndex assign = expectAssignExpr();
            expectSemicolon(ErrorTag::expected_semi_after_stmt, true);
            if (nodeTag(assign) != NodeTag::assign_destructure) return addUnary(NodeTag::comptime_, comptime_token, assign);
            return assign;
        }

        switch (tokenTag(tok_i)) {
            case TokenTag::keyword_nosuspend: {
                TokenIndex t = nextToken();
                NodeIndex child = expectBlockExprStatement();
                return addUnary(NodeTag::nosuspend_, t, child);
            }
            case TokenTag::keyword_suspend: {
                TokenIndex token = nextToken();
                NodeIndex block_expr = expectBlockExprStatement();
                return addUnary(NodeTag::suspend_, token, block_expr);
            }
            case TokenTag::keyword_defer:
                if (is_block_level) {
                    TokenIndex t = nextToken();
                    NodeIndex child = expectBlockExprStatement();
                    return addUnary(NodeTag::defer_, t, child);
                }
                break;
            case TokenTag::keyword_errdefer:
                if (is_block_level) {
                    TokenIndex t = nextToken();
                    OptionalTokenIndex payload = parsePayload();
                    NodeIndex child = expectBlockExprStatement();
                    return addNode(Node{NodeTag::errdefer_, t, Data::pair(payload.value, child)});
                }
                break;
            case TokenTag::keyword_if: return expectIfStatement();
            case TokenTag::keyword_enum:
            case TokenTag::keyword_struct:
            case TokenTag::keyword_union: {
                const TokenIndex identifier = tok_i + 1;
                if (parseCStyleContainer()) {
                    // Return something so that `expectStatement` is happy.
                    return addNode(Node{NodeTag::identifier, identifier, Data::undef()});
                }
                break;
            }
            default: break;
        }

        NodeIndex labeled_statement = parseLabeledStatement();
        if (labeled_statement != kNull) return labeled_statement;

        if (is_block_level) return expectVarDeclExprStatement(kNone);
        NodeIndex assign = expectAssignExpr();
        expectSemicolon(ErrorTag::expected_semi_after_stmt, true);
        return assign;
    }

    /// VarDeclExprStatement
    NodeIndex expectVarDeclExprStatement(TokenIndex comptime_token) {
        ScratchTop st(*this);
        const std::size_t scratch_top = st.top;

        while (true) {
            NodeIndex opt_var_decl_proto = parseVarDeclProto();
            if (opt_var_decl_proto != kNull) {
                scratch.push_back(opt_var_decl_proto);
            } else {
                NodeIndex expr = parseExpr();
                if (expr == kNull) {
                    if (scratch.size() == scratch_top) {
                        // We parsed nothing
                        fail(ErrorTag::expected_statement);
                    } else {
                        // We've had at least one LHS, but had a bad comma
                        fail(ErrorTag::expected_expr_or_var_decl);
                    }
                }
                scratch.push_back(expr);
            }
            if (eatToken(TokenTag::comma) == kNone) break;
        }

        const std::size_t lhs_count = scratch.size() - scratch_top;
        assert(lhs_count > 0);

        TokenIndex equal_token = eatToken(TokenTag::equal);
        if (equal_token == kNone) {
            if (lhs_count > 1) {
                // Definitely a destructure, so allow recovering from ==
                TokenIndex tok = eatToken(TokenTag::equal_equal);
                if (tok != kNone) {
                    Error e{};
                    e.tag = ErrorTag::wrong_equal_var_decl;
                    e.token = tok;
                    warnMsg(e);
                    equal_token = tok;
                } else {
                    failExpected(TokenTag::equal);
                }
            } else {
                const NodeIndex lhs = scratch[scratch_top];
                bool handled = false;
                switch (nodeTag(lhs)) {
                    case NodeTag::global_var_decl:
                    case NodeTag::local_var_decl:
                    case NodeTag::simple_var_decl:
                    case NodeTag::aligned_var_decl: {
                        // Definitely a var decl, so allow recovering from ==
                        TokenIndex tok = eatToken(TokenTag::equal_equal);
                        if (tok != kNone) {
                            Error e{};
                            e.tag = ErrorTag::wrong_equal_var_decl;
                            e.token = tok;
                            warnMsg(e);
                            equal_token = tok;
                            handled = true;
                        } else {
                            failExpected(TokenTag::equal);
                        }
                        break;
                    }
                    default: break;
                }
                if (!handled) {
                    NodeIndex expr = finishAssignExpr(lhs);
                    expectSemicolon(ErrorTag::expected_semi_after_stmt, true);
                    if (comptime_token != kNone) return addUnary(NodeTag::comptime_, comptime_token, expr);
                    return expr;
                }
            }
        }

        NodeIndex rhs = expectExpr();
        expectSemicolon(ErrorTag::expected_semi_after_stmt, true);

        if (lhs_count == 1) {
            const NodeIndex lhs = scratch[scratch_top];
            switch (nodeTag(lhs)) {
                case NodeTag::simple_var_decl:
                case NodeTag::aligned_var_decl:
                case NodeTag::local_var_decl:
                case NodeTag::global_var_decl:
                    setVarDeclInitExpr(lhs, opt(rhs));
                    // Don't need to wrap in comptime
                    return lhs;
                default: break;
            }
            NodeIndex expr = addNode(Node{NodeTag::assign, equal_token, Data::pair(lhs, rhs)});
            if (comptime_token != kNone) return addUnary(NodeTag::comptime_, comptime_token, expr);
            return expr;
        }

        // An actual destructure! No need for any `comptime` wrapper here.
        auto extra_start = static_cast<ExtraIndex>(extra_data.size());
        extra_data.push_back(static_cast<std::uint32_t>(lhs_count));
        extra_data.insert(extra_data.end(), scratch.begin() + static_cast<std::ptrdiff_t>(scratch_top), scratch.end());
        return addNode(Node{NodeTag::assign_destructure, equal_token, Data::pair(extra_start, rhs)});
    }

    /// If a parse error occurs, reports an error, but then finds the next statement
    /// and returns that one instead. If a parse error occurs but there is no following
    /// statement, returns null.
    NodeIndex expectStatementRecoverable() {
        while (true) {
            try {
                return expectStatement(true);
            } catch (const ParseError&) {
                findNextStmt();  // Try to skip to the next statement.
                switch (tokenTag(tok_i)) {
                    case TokenTag::r_brace: return kNull;
                    case TokenTag::eof: throw;
                    default: continue;
                }
            }
        }
    }

    /// IfStatement
    NodeIndex expectIfStatement() {
        TokenIndex if_token = assertToken(TokenTag::keyword_if);
        expectToken(TokenTag::l_paren);
        NodeIndex condition = expectExpr();
        expectToken(TokenTag::r_paren);
        parsePtrPayload();

        // TODO propose to change the syntax so that semicolons are always required
        // inside if statements, even if there is an `else`.
        bool else_required = false;
        NodeIndex then_expr = parseBlockExpr();
        if (then_expr == kNull) {
            NodeIndex assign_expr = parseAssignExpr();
            if (assign_expr == kNull) fail(ErrorTag::expected_block_or_assignment);
            if (eatToken(TokenTag::semicolon) != kNone)
                return addNode(Node{NodeTag::if_simple, if_token, Data::pair(condition, assign_expr)});
            else_required = true;
            then_expr = assign_expr;
        }
        if (eatToken(TokenTag::keyword_else) == kNone) {
            if (else_required) warn(ErrorTag::expected_semi_or_else);
            return addNode(Node{NodeTag::if_simple, if_token, Data::pair(condition, then_expr)});
        }
        parsePayload();
        NodeIndex else_expr = expectStatement(false);
        ExtraIndex e = addExtra({then_expr, else_expr});
        return addNode(Node{NodeTag::if_, if_token, Data::pair(condition, e)});
    }

    /// LabeledStatement <- BlockLabel? (Block / LoopStatement / SwitchExpr)
    NodeIndex parseLabeledStatement() {
        TokenIndex opt_label_token = parseBlockLabel();

        NodeIndex block = parseBlock();
        if (block != kNull) return block;
        NodeIndex loop_stmt = parseLoopStatement();
        if (loop_stmt != kNull) return loop_stmt;
        NodeIndex switch_expr = parseSwitchExpr(opt_label_token != kNone);
        if (switch_expr != kNull) return switch_expr;

        if (opt_label_token == kNone) return kNull;
        const TokenIndex label_token = opt_label_token;

        const TokenIndex after_colon = tok_i;
        if (parseTypeExpr() != kNull) {
            NodeIndex a = parseByteAlign();
            NodeIndex b = parseAddrSpace();
            NodeIndex c = parseLinkSection();
            NodeIndex d = eatToken(TokenTag::equal) == kNone ? kNull : expectExpr();
            if (a != kNull || b != kNull || c != kNull || d != kNull) {
                Error e{};
                e.tag = ErrorTag::expected_var_const;
                e.token = label_token;
                failMsg(e);
            }
        }
        Error e{};
        e.tag = ErrorTag::expected_labelable;
        e.token = after_colon;
        failMsg(e);
    }

    /// LoopStatement <- KEYWORD_inline? (ForStatement / WhileStatement)
    NodeIndex parseLoopStatement() {
        TokenIndex inline_token = eatToken(TokenTag::keyword_inline);

        NodeIndex for_statement = parseForStatement();
        if (for_statement != kNull) return for_statement;
        NodeIndex while_statement = parseWhileStatement();
        if (while_statement != kNull) return while_statement;

        if (inline_token == kNone) return kNull;

        // If we've seen "inline", there should have been a "for" or "while"
        fail(ErrorTag::expected_inlinable);
    }

    /// ForStatement
    NodeIndex parseForStatement() {
        TokenIndex for_token = eatToken(TokenTag::keyword_for);
        if (for_token == kNone) return kNull;

        ScratchTop st(*this);
        const std::size_t scratch_top = st.top;
        const std::size_t inputs = forPrefix();

        bool else_required = false;
        bool seen_semicolon = false;
        NodeIndex then_expr = parseBlockExpr();
        if (then_expr == kNull) {
            NodeIndex assign_expr = parseAssignExpr();
            if (assign_expr == kNull) fail(ErrorTag::expected_block_or_assignment);
            if (eatToken(TokenTag::semicolon) != kNone) {
                seen_semicolon = true;
            } else {
                else_required = true;
            }
            then_expr = assign_expr;
        }
        bool has_else = false;
        if (!seen_semicolon && eatToken(TokenTag::keyword_else) != kNone) {
            scratch.push_back(then_expr);
            NodeIndex else_stmt = expectStatement(false);
            scratch.push_back(else_stmt);
            has_else = true;
        } else if (inputs == 1) {
            if (else_required) warn(ErrorTag::expected_semi_or_else);
            return addNode(Node{NodeTag::for_simple, for_token, Data::pair(scratch[scratch_top], then_expr)});
        } else {
            if (else_required) warn(ErrorTag::expected_semi_or_else);
            scratch.push_back(then_expr);
        }
        SubRange r = listToSpan(std::span<const NodeIndex>(scratch.data() + scratch_top, scratch.size() - scratch_top));
        return addNode(Node{NodeTag::for_, for_token,
                            Data::pair(r.start, ForData{static_cast<std::uint32_t>(inputs), has_else}.pack())});
    }

    NodeIndex addWhileSimpleOrCont(TokenIndex while_token, NodeIndex condition, NodeIndex cont_expr,
                                   NodeIndex then_expr) {
        if (cont_expr == kNull)
            return addNode(Node{NodeTag::while_simple, while_token, Data::pair(condition, then_expr)});
        ExtraIndex e = addExtra({cont_expr, then_expr});
        return addNode(Node{NodeTag::while_cont, while_token, Data::pair(condition, e)});
    }

    /// WhileStatement
    NodeIndex parseWhileStatement() {
        TokenIndex while_token = eatToken(TokenTag::keyword_while);
        if (while_token == kNone) return kNull;
        expectToken(TokenTag::l_paren);
        NodeIndex condition = expectExpr();
        expectToken(TokenTag::r_paren);
        parsePtrPayload();
        NodeIndex cont_expr = parseWhileContinueExpr();

        // TODO propose to change the syntax so that semicolons are always required
        // inside while statements, even if there is an `else`.
        bool else_required = false;
        NodeIndex then_expr = parseBlockExpr();
        if (then_expr == kNull) {
            NodeIndex assign_expr = parseAssignExpr();
            if (assign_expr == kNull) fail(ErrorTag::expected_block_or_assignment);
            if (eatToken(TokenTag::semicolon) != kNone)
                return addWhileSimpleOrCont(while_token, condition, cont_expr, assign_expr);
            else_required = true;
            then_expr = assign_expr;
        }
        if (eatToken(TokenTag::keyword_else) == kNone) {
            if (else_required) warn(ErrorTag::expected_semi_or_else);
            return addWhileSimpleOrCont(while_token, condition, cont_expr, then_expr);
        }
        parsePayload();
        NodeIndex else_expr = expectStatement(false);
        ExtraIndex e = addExtra({cont_expr, then_expr, else_expr});
        return addNode(Node{NodeTag::while_, while_token, Data::pair(condition, e)});
    }

    /// BlockExprStatement
    NodeIndex parseBlockExprStatement() {
        NodeIndex block_expr = parseBlockExpr();
        if (block_expr != kNull) return block_expr;
        NodeIndex assign_expr = parseAssignExpr();
        if (assign_expr != kNull) {
            expectSemicolon(ErrorTag::expected_semi_after_stmt, true);
            return assign_expr;
        }
        return kNull;
    }

    NodeIndex expectBlockExprStatement() {
        NodeIndex n = parseBlockExprStatement();
        if (n == kNull) fail(ErrorTag::expected_block_or_expr);
        return n;
    }

    /// BlockExpr <- BlockLabel? Block
    NodeIndex parseBlockExpr() {
        switch (tokenTag(tok_i)) {
            case TokenTag::identifier:
                if (tokenTag(tok_i + 1) == TokenTag::colon && tokenTag(tok_i + 2) == TokenTag::l_brace) {
                    tok_i += 2;
                    return parseBlock();
                }
                return kNull;
            case TokenTag::l_brace: return parseBlock();
            default: return kNull;
        }
    }

    /// AssignExpr <- Expr (AssignOp Expr / (COMMA Expr)+ EQUAL Expr)?
    NodeIndex parseAssignExpr() {
        NodeIndex expr = parseExpr();
        if (expr == kNull) return kNull;
        return finishAssignExpr(expr);
    }

    /// SingleAssignExpr <- Expr (AssignOp Expr)?
    NodeIndex parseSingleAssignExpr() {
        NodeIndex lhs = parseExpr();
        if (lhs == kNull) return kNull;
        auto tag = assignOpNode(tokenTag(tok_i));
        if (!tag) return lhs;
        TokenIndex t = nextToken();
        NodeIndex rhs = expectExpr();
        return addNode(Node{*tag, t, Data::pair(lhs, rhs)});
    }

    NodeIndex finishAssignExpr(NodeIndex lhs) {
        TokenTag tok = tokenTag(tok_i);
        if (tok == TokenTag::comma) return finishAssignDestructureExpr(lhs);
        auto tag = assignOpNode(tok);
        if (!tag) return lhs;
        TokenIndex t = nextToken();
        NodeIndex rhs = expectExpr();
        return addNode(Node{*tag, t, Data::pair(lhs, rhs)});
    }

    NodeIndex finishAssignDestructureExpr(NodeIndex first_lhs) {
        ScratchTop st(*this);
        const std::size_t scratch_top = st.top;

        scratch.push_back(first_lhs);

        while (eatToken(TokenTag::comma) != kNone) {
            NodeIndex expr = expectExpr();
            scratch.push_back(expr);
        }

        TokenIndex equal_token = expectToken(TokenTag::equal);

        NodeIndex rhs = expectExpr();

        const std::size_t lhs_count = scratch.size() - scratch_top;
        assert(lhs_count > 1);

        auto extra_start = static_cast<ExtraIndex>(extra_data.size());
        extra_data.push_back(static_cast<std::uint32_t>(lhs_count));
        extra_data.insert(extra_data.end(), scratch.begin() + static_cast<std::ptrdiff_t>(scratch_top), scratch.end());
        return addNode(Node{NodeTag::assign_destructure, equal_token, Data::pair(extra_start, rhs)});
    }

    NodeIndex expectSingleAssignExpr() {
        NodeIndex n = parseSingleAssignExpr();
        if (n == kNull) fail(ErrorTag::expected_expr_or_assignment);
        return n;
    }

    NodeIndex expectAssignExpr() {
        NodeIndex n = parseAssignExpr();
        if (n == kNull) fail(ErrorTag::expected_expr_or_assignment);
        return n;
    }

    NodeIndex parseExpr() { return parseExprPrecedence(0); }

    NodeIndex expectExpr() {
        NodeIndex n = parseExpr();
        if (n == kNull) fail(ErrorTag::expected_expr);
        return n;
    }

    NodeIndex parseExprPrecedence(std::int32_t min_prec) {
        assert(min_prec >= 0);
        NodeIndex node = parsePrefixExpr();
        if (node == kNull) return kNull;

        std::int8_t banned_prec = -1;

        while (true) {
            const TokenTag tok_tag = tokenTag(tok_i);
            const OperInfo info = operInfo(tok_tag);
            if (info.prec < min_prec) break;
            if (info.prec == banned_prec) fail(ErrorTag::chained_comparison_operators);

            const TokenIndex oper_token = nextToken();
            // Special-case handling for "catch"
            if (tok_tag == TokenTag::keyword_catch) parsePayload();
            NodeIndex rhs = parseExprPrecedence(info.prec + 1);
            if (rhs == kNull) {
                warn(ErrorTag::expected_expr);
                return node;
            }

            {
                const std::size_t tok_len = tokenLexeme(tok_tag).size();
                const char char_before = source[tokenStart(oper_token) - 1];
                const char char_after = source[tokenStart(oper_token) + tok_len];
                if (tok_tag == TokenTag::ampersand && char_after == '&') {
                    // without types we don't know if '&&' was intended as 'bitwise_and address_of', or a c-style logical_and
                    // The best the parser can do is recommend changing it to 'and' or ' & &'
                    Error e{};
                    e.tag = ErrorTag::invalid_ampersand_ampersand;
                    e.token = oper_token;
                    warnMsg(e);
                } else if (isWhitespace(char_before) != isWhitespace(char_after)) {
                    Error e{};
                    e.tag = ErrorTag::mismatched_binary_op_whitespace;
                    e.token = oper_token;
                    warnMsg(e);
                }
            }

            node = addNode(Node{info.tag, oper_token, Data::pair(node, rhs)});

            if (info.assoc_none) banned_prec = info.prec;
        }

        return node;
    }

    /// PrefixExpr <- PrefixOp* PrimaryExpr
    NodeIndex parsePrefixExpr() {
        Nest nest(*this);
        NodeTag tag;
        switch (tokenTag(tok_i)) {
            case TokenTag::bang: tag = NodeTag::bool_not; break;
            case TokenTag::minus: tag = NodeTag::negation; break;
            case TokenTag::tilde: tag = NodeTag::bit_not; break;
            case TokenTag::minus_percent: tag = NodeTag::negation_wrap; break;
            case TokenTag::ampersand: tag = NodeTag::address_of; break;
            case TokenTag::keyword_try: tag = NodeTag::try_; break;
            default: return parsePrimaryExpr();
        }
        TokenIndex t = nextToken();
        NodeIndex child = expectPrefixExpr();
        return addUnary(tag, t, child);
    }

    NodeIndex expectPrefixExpr() {
        NodeIndex n = parsePrefixExpr();
        if (n == kNull) fail(ErrorTag::expected_prefix_expr);
        return n;
    }

    struct PtrModifiers {
        OptionalIndex align_node;
        OptionalIndex addrspace_node;
        OptionalIndex bit_range_start;
        OptionalIndex bit_range_end;
    };

    /// The ptr_type_bit_range / ptr_type / ptr_type_aligned choice shared by
    /// `*` and `**`.
    NodeIndex addSinglePtr(TokenIndex asterisk, const PtrModifiers& mods, NodeIndex elem_type) {
        if (!mods.bit_range_start.isNone()) {
            ExtraIndex e = addExtra({kNone, mods.align_node.value, mods.addrspace_node.value,
                                     mods.bit_range_start.value, mods.bit_range_end.value});
            return addNode(Node{NodeTag::ptr_type_bit_range, asterisk, Data::pair(e, elem_type)});
        }
        if (!mods.addrspace_node.isNone()) {
            ExtraIndex e = addExtra({kNone, mods.align_node.value, mods.addrspace_node.value});
            return addNode(Node{NodeTag::ptr_type, asterisk, Data::pair(e, elem_type)});
        }
        return addNode(Node{NodeTag::ptr_type_aligned, asterisk, Data::pair(mods.align_node.value, elem_type)});
    }

    /// The ptr_type_aligned / ptr_type_sentinel / ptr_type choice shared by
    /// `[*]` and `[]`.
    NodeIndex addMultiPtr(TokenIndex lbracket, NodeIndex sentinel, const PtrModifiers& mods, NodeIndex elem_type) {
        if (sentinel == kNull && mods.addrspace_node.isNone())
            return addNode(Node{NodeTag::ptr_type_aligned, lbracket, Data::pair(mods.align_node.value, elem_type)});
        if (mods.align_node.isNone() && mods.addrspace_node.isNone())
            return addNode(Node{NodeTag::ptr_type_sentinel, lbracket, Data::pair(sentinel, elem_type)});
        ExtraIndex e = addExtra({sentinel, mods.align_node.value, mods.addrspace_node.value});
        return addNode(Node{NodeTag::ptr_type, lbracket, Data::pair(e, elem_type)});
    }

    /// TypeExpr <- PrefixTypeOp* ErrorUnionExpr
    NodeIndex parseTypeExpr() {
        Nest nest(*this);
        switch (tokenTag(tok_i)) {
            case TokenTag::question_mark: {
                TokenIndex t = nextToken();
                NodeIndex child = expectTypeExpr();
                return addUnary(NodeTag::optional_type, t, child);
            }
            case TokenTag::keyword_anyframe:
                if (tokenTag(tok_i + 1) == TokenTag::arrow) {
                    TokenIndex t = nextToken();
                    TokenIndex arrow = nextToken();
                    NodeIndex child = expectTypeExpr();
                    return addNode(Node{NodeTag::anyframe_type, t, Data::pair(arrow, child)});
                }
                return parseErrorUnionExpr();
            case TokenTag::asterisk: {
                TokenIndex asterisk = nextToken();
                PtrModifiers mods = parsePtrModifiers();
                NodeIndex elem_type = expectTypeExpr();
                return addSinglePtr(asterisk, mods, elem_type);
            }
            case TokenTag::asterisk_asterisk: {
                TokenIndex asterisk = nextToken();
                PtrModifiers mods = parsePtrModifiers();
                NodeIndex elem_type = expectTypeExpr();
                NodeIndex inner = addSinglePtr(asterisk, mods, elem_type);
                return addNode(Node{NodeTag::ptr_type_aligned, asterisk, Data::pair(kNone, inner)});
            }
            case TokenTag::l_bracket:
                if (tokenTag(tok_i + 1) == TokenTag::asterisk) {
                    TokenIndex l_bracket = nextToken();
                    nextToken();
                    NodeIndex sentinel = kNull;
                    TokenIndex ident = eatToken(TokenTag::identifier);
                    if (ident != kNone) {
                        std::string_view ident_slice =
                            source.substr(tokenStart(ident), tokenStart(ident + 1) - tokenStart(ident));
                        while (!ident_slice.empty() && isWhitespace(ident_slice.back())) ident_slice.remove_suffix(1);
                        if (ident_slice != "c") tok_i -= 1;
                    } else if (eatToken(TokenTag::colon) != kNone) {
                        sentinel = expectExpr();
                    }
                    expectToken(TokenTag::r_bracket);
                    PtrModifiers mods = parsePtrModifiers();
                    NodeIndex elem_type = expectTypeExpr();
                    if (!mods.bit_range_start.isNone()) {
                        Error e{};
                        e.tag = ErrorTag::invalid_bit_range;
                        e.token = nodeMainToken(mods.bit_range_start.value);
                        warnMsg(e);
                    }
                    return addMultiPtr(l_bracket, sentinel, mods, elem_type);
                } else {
                    TokenIndex lbracket = nextToken();
                    NodeIndex len_expr = parseExpr();
                    NodeIndex sentinel = eatToken(TokenTag::colon) != kNone ? expectExpr() : kNull;
                    expectToken(TokenTag::r_bracket);
                    if (len_expr == kNull) {
                        PtrModifiers mods = parsePtrModifiers();
                        NodeIndex elem_type = expectTypeExpr();
                        if (!mods.bit_range_start.isNone()) {
                            Error e{};
                            e.tag = ErrorTag::invalid_bit_range;
                            e.token = nodeMainToken(mods.bit_range_start.value);
                            warnMsg(e);
                        }
                        return addMultiPtr(lbracket, sentinel, mods, elem_type);
                    }
                    switch (tokenTag(tok_i)) {
                        case TokenTag::keyword_align:
                        case TokenTag::keyword_const:
                        case TokenTag::keyword_volatile:
                        case TokenTag::keyword_allowzero:
                        case TokenTag::keyword_addrspace: fail(ErrorTag::ptr_mod_on_array_child_type);
                        default: break;
                    }
                    NodeIndex elem_type = expectTypeExpr();
                    if (sentinel == kNull)
                        return addNode(Node{NodeTag::array_type, lbracket, Data::pair(len_expr, elem_type)});
                    ExtraIndex e = addExtra({sentinel, elem_type});
                    return addNode(Node{NodeTag::array_type_sentinel, lbracket, Data::pair(len_expr, e)});
                }
            default: return parseErrorUnionExpr();
        }
    }

    NodeIndex expectTypeExpr() {
        NodeIndex n = parseTypeExpr();
        if (n == kNull) fail(ErrorTag::expected_type_expr);
        return n;
    }

    NodeIndex addBreakLike(NodeTag tag) {
        TokenIndex t = nextToken();
        OptionalTokenIndex label = parseBreakLabel();
        NodeIndex rhs = parseExpr();
        return addNode(Node{tag, t, Data::pair(label.value, rhs)});
    }

    NodeIndex addUnaryExpectExpr(NodeTag tag) {
        TokenIndex t = nextToken();
        NodeIndex child = expectExpr();
        return addUnary(tag, t, child);
    }

    enum class Body { expr, type };

    NodeIndex parseBody(Body b) { return b == Body::expr ? expectExpr() : expectTypeExpr(); }

    /// PrimaryExpr
    NodeIndex parsePrimaryExpr() {
        switch (tokenTag(tok_i)) {
            case TokenTag::keyword_asm: return expectAsmExpr();
            case TokenTag::keyword_if: return parseIfExpr();
            case TokenTag::keyword_break: return addBreakLike(NodeTag::break_);
            case TokenTag::keyword_continue: return addBreakLike(NodeTag::continue_);
            case TokenTag::keyword_comptime: return addUnaryExpectExpr(NodeTag::comptime_);
            case TokenTag::keyword_nosuspend: return addUnaryExpectExpr(NodeTag::nosuspend_);
            case TokenTag::keyword_resume: return addUnaryExpectExpr(NodeTag::resume_);
            case TokenTag::keyword_return: {
                TokenIndex t = nextToken();
                NodeIndex child = parseExpr();
                return addUnary(NodeTag::return_, t, child);
            }
            case TokenTag::identifier:
                if (tokenTag(tok_i + 1) == TokenTag::colon) {
                    switch (tokenTag(tok_i + 2)) {
                        case TokenTag::keyword_inline:
                            tok_i += 3;
                            switch (tokenTag(tok_i)) {
                                case TokenTag::keyword_for: return parseFor(Body::expr);
                                case TokenTag::keyword_while: return parseWhileExpr();
                                default: fail(ErrorTag::expected_inlinable);
                            }
                        case TokenTag::keyword_for: tok_i += 2; return parseFor(Body::expr);
                        case TokenTag::keyword_while: tok_i += 2; return parseWhileExpr();
                        default: return parseCurlySuffixExpr();
                    }
                }
                return parseCurlySuffixExpr();
            case TokenTag::keyword_inline:
                tok_i += 1;
                switch (tokenTag(tok_i)) {
                    case TokenTag::keyword_for: return parseFor(Body::expr);
                    case TokenTag::keyword_while: return parseWhileExpr();
                    default: fail(ErrorTag::expected_inlinable);
                }
            case TokenTag::keyword_for: return parseFor(Body::expr);
            case TokenTag::keyword_while: return parseWhileExpr();
            case TokenTag::l_brace: return parseBlock();
            default: return parseCurlySuffixExpr();
        }
    }

    /// IfExpr <- IfPrefix Expr (KEYWORD_else Payload? Expr)? !ExprSuffix
    NodeIndex parseIfExpr() { return parseIf(Body::expr); }

    /// Block <- LBRACE BlockStatement* RBRACE
    NodeIndex parseBlock() {
        TokenIndex lbrace = eatToken(TokenTag::l_brace);
        if (lbrace == kNone) return kNull;
        Nest nest(*this);
        ScratchTop st(*this);
        const std::size_t scratch_top = st.top;
        while (true) {
            if (tokenTag(tok_i) == TokenTag::r_brace) break;
            NodeIndex statement = expectStatementRecoverable();
            if (statement == kNull) break;
            scratch.push_back(statement);
        }
        expectToken(TokenTag::r_brace);
        std::span<const NodeIndex> statements(scratch.data() + scratch_top, scratch.size() - scratch_top);
        const bool semicolon = !statements.empty() && tokenTag(tok_i - 2) == TokenTag::semicolon;
        if (statements.size() <= 2) {
            return addNode(Node{semicolon ? NodeTag::block_two_semicolon : NodeTag::block_two, lbrace,
                                Data::pair(statements.size() >= 1 ? statements[0] : kNone,
                                           statements.size() >= 2 ? statements[1] : kNone)});
        }
        SubRange r = listToSpan(statements);
        return addNode(Node{semicolon ? NodeTag::block_semicolon : NodeTag::block, lbrace, Data::pair(r.start, r.end)});
    }

    /// ForPrefix <- KEYWORD_for LPAREN ForInput (COMMA ForInput)* COMMA? RPAREN ForPayload
    std::size_t forPrefix() {
        const std::size_t start = scratch.size();
        expectToken(TokenTag::l_paren);

        while (true) {
            NodeIndex input = expectExpr();
            TokenIndex ellipsis = eatToken(TokenTag::ellipsis2);
            if (ellipsis != kNone) {
                NodeIndex end = parseExpr();
                input = addNode(Node{NodeTag::for_range, ellipsis, Data::pair(input, end)});
            }

            scratch.push_back(input);
            switch (tokenTag(tok_i)) {
                case TokenTag::comma: tok_i += 1; break;
                case TokenTag::r_paren: tok_i += 1; goto inputs_done;
                case TokenTag::colon:
                case TokenTag::r_brace:
                case TokenTag::r_bracket: failExpected(TokenTag::r_paren);
                // Likely just a missing comma; give error but continue parsing.
                default: warn(ErrorTag::expected_comma_after_for_operand); break;
            }
            if (eatToken(TokenTag::r_paren) != kNone) break;
        }
    inputs_done:
        const std::size_t inputs = scratch.size() - start;

        if (eatToken(TokenTag::pipe) == kNone) {
            warn(ErrorTag::expected_loop_payload);
            return inputs;
        }

        bool warned_excess = false;
        std::uint32_t captures = 0;
        while (true) {
            eatToken(TokenTag::asterisk);
            TokenIndex identifier = expectToken(TokenTag::identifier);
            captures += 1;
            if (captures > inputs && !warned_excess) {
                Error e{};
                e.tag = ErrorTag::extra_for_capture;
                e.token = identifier;
                warnMsg(e);
                warned_excess = true;
            }
            switch (tokenTag(tok_i)) {
                case TokenTag::comma: tok_i += 1; break;
                case TokenTag::pipe: tok_i += 1; goto captures_done;
                // Likely just a missing comma; give error but continue parsing.
                default: warn(ErrorTag::expected_comma_after_capture); break;
            }
            if (eatToken(TokenTag::pipe) != kNone) break;
        }
    captures_done:

        if (captures < inputs) {
            const std::size_t index = scratch.size() - captures;
            const TokenIndex input = nodeMainToken(scratch[index]);
            Error e{};
            e.tag = ErrorTag::for_input_not_captured;
            e.token = input;
            warnMsg(e);
        }
        return inputs;
    }

    /// WhileExpr <- WhilePrefix Expr (KEYWORD_else Payload? Expr)? !ExprSuffix
    NodeIndex parseWhileExpr() { return parseWhileGeneric(Body::expr); }

    /// WhileTypeExpr <- WhilePrefix TypeExpr (KEYWORD_else Payload? TypeExpr)? !ExprSuffix
    NodeIndex parseWhileTypeExpr() { return parseWhileGeneric(Body::type); }

    NodeIndex parseWhileGeneric(Body body) {
        TokenIndex while_token = eatToken(TokenTag::keyword_while);
        if (while_token == kNone) return kNull;
        expectToken(TokenTag::l_paren);
        NodeIndex condition = expectExpr();
        expectToken(TokenTag::r_paren);
        parsePtrPayload();
        NodeIndex cont_expr = parseWhileContinueExpr();

        NodeIndex then_expr = parseBody(body);
        if (eatToken(TokenTag::keyword_else) == kNone)
            return addWhileSimpleOrCont(while_token, condition, cont_expr, then_expr);
        parsePayload();
        NodeIndex else_expr = parseBody(body);
        ExtraIndex e = addExtra({cont_expr, then_expr, else_expr});
        return addNode(Node{NodeTag::while_, while_token, Data::pair(condition, e)});
    }

    /// Parses the `{ ... }` of an InitList after the lbrace. Returns false if
    /// it turned out to be a list of plain expressions (not field inits).
    /// CurlySuffixExpr <- TypeExpr InitList?
    NodeIndex parseCurlySuffixExpr() {
        NodeIndex lhs = parseTypeExpr();
        if (lhs == kNull) return kNull;
        TokenIndex lbrace = eatToken(TokenTag::l_brace);
        if (lbrace == kNone) return lhs;

        // If there are 0 or 1 items, we can use ArrayInitOne/StructInitOne;
        // otherwise we use the full ArrayInit/StructInit.

        ScratchTop st(*this);
        const std::size_t scratch_top = st.top;
        NodeIndex opt_field_init = parseFieldInit();
        if (opt_field_init != kNull) {
            scratch.push_back(opt_field_init);
            while (true) {
                switch (tokenTag(tok_i)) {
                    case TokenTag::comma: tok_i += 1; break;
                    case TokenTag::r_brace: tok_i += 1; goto fields_done;
                    case TokenTag::colon:
                    case TokenTag::r_paren:
                    case TokenTag::r_bracket: failExpected(TokenTag::r_brace);
                    // Likely just a missing comma; give error but continue parsing.
                    default: warn(ErrorTag::expected_comma_after_initializer); break;
                }
                if (eatToken(TokenTag::r_brace) != kNone) break;
                NodeIndex next = expectFieldInit();
                scratch.push_back(next);
            }
        fields_done:
            const bool comma = tokenTag(tok_i - 2) == TokenTag::comma;
            std::span<const NodeIndex> inits(scratch.data() + scratch_top, scratch.size() - scratch_top);
            assert(!inits.empty());
            if (inits.size() <= 1) {
                return addNode(Node{comma ? NodeTag::struct_init_one_comma : NodeTag::struct_init_one, lbrace,
                                    Data::pair(lhs, inits[0])});
            }
            SubRange r = listToSpan(inits);
            ExtraIndex e = addExtraSubRange(r);
            return addNode(Node{comma ? NodeTag::struct_init_comma : NodeTag::struct_init, lbrace, Data::pair(lhs, e)});
        }

        while (true) {
            if (eatToken(TokenTag::r_brace) != kNone) break;
            NodeIndex elem_init = expectExpr();
            scratch.push_back(elem_init);
            switch (tokenTag(tok_i)) {
                case TokenTag::comma: tok_i += 1; break;
                case TokenTag::r_brace: tok_i += 1; goto elems_done;
                case TokenTag::colon:
                case TokenTag::r_paren:
                case TokenTag::r_bracket: failExpected(TokenTag::r_brace);
                // Likely just a missing comma; give error but continue parsing.
                default: warn(ErrorTag::expected_comma_after_initializer); break;
            }
        }
    elems_done:
        const bool comma = tokenTag(tok_i - 2) == TokenTag::comma;
        std::span<const NodeIndex> inits(scratch.data() + scratch_top, scratch.size() - scratch_top);
        switch (inits.size()) {
            case 0: return addNode(Node{NodeTag::struct_init_one, lbrace, Data::pair(lhs, kNone)});
            case 1:
                return addNode(Node{comma ? NodeTag::array_init_one_comma : NodeTag::array_init_one, lbrace,
                                    Data::pair(lhs, inits[0])});
            default: {
                SubRange r = listToSpan(inits);
                ExtraIndex e = addExtraSubRange(r);
                return addNode(Node{comma ? NodeTag::array_init_comma : NodeTag::array_init, lbrace, Data::pair(lhs, e)});
            }
        }
    }

    /// ErrorUnionExpr <- SuffixExpr (EXCLAMATIONMARK TypeExpr)?
    NodeIndex parseErrorUnionExpr() {
        NodeIndex suffix_expr = parseSuffixExpr();
        if (suffix_expr == kNull) return kNull;
        TokenIndex bang = eatToken(TokenTag::bang);
        if (bang == kNone) return suffix_expr;
        NodeIndex rhs = expectTypeExpr();
        return addNode(Node{NodeTag::error_union, bang, Data::pair(suffix_expr, rhs)});
    }

    /// SuffixExpr <- PrimaryTypeExpr (SuffixOp / FnCallArguments)*
    NodeIndex parseSuffixExpr() {
        NodeIndex res = parsePrimaryTypeExpr();
        if (res == kNull) return kNull;
        while (true) {
            NodeIndex opt_suffix_op = parseSuffixOp(res);
            if (opt_suffix_op != kNull) {
                res = opt_suffix_op;
                continue;
            }
            TokenIndex lparen = eatToken(TokenTag::l_paren);
            if (lparen == kNone) return res;
            ScratchTop st(*this);
            const std::size_t scratch_top = st.top;
            while (true) {
                if (eatToken(TokenTag::r_paren) != kNone) break;
                NodeIndex param = expectExpr();
                scratch.push_back(param);
                switch (tokenTag(tok_i)) {
                    case TokenTag::comma: tok_i += 1; break;
                    case TokenTag::r_paren: tok_i += 1; goto args_done;
                    case TokenTag::colon:
                    case TokenTag::r_brace:
                    case TokenTag::r_bracket: failExpected(TokenTag::r_paren);
                    // Likely just a missing comma; give error but continue parsing.
                    default: warn(ErrorTag::expected_comma_after_arg); break;
                }
            }
        args_done:
            const bool comma = tokenTag(tok_i - 2) == TokenTag::comma;
            std::span<const NodeIndex> params(scratch.data() + scratch_top, scratch.size() - scratch_top);
            if (params.size() <= 1) {
                res = addNode(Node{comma ? NodeTag::call_one_comma : NodeTag::call_one, lparen,
                                   Data::pair(res, params.size() >= 1 ? params[0] : kNone)});
            } else {
                SubRange r = listToSpan(params);
                ExtraIndex e = addExtraSubRange(r);
                res = addNode(Node{comma ? NodeTag::call_comma : NodeTag::call, lparen, Data::pair(res, e)});
            }
        }
    }

    NodeIndex addLeaf(NodeTag tag) { return addNode(Node{tag, nextToken(), Data::undef()}); }

    /// PrimaryTypeExpr
    NodeIndex parsePrimaryTypeExpr() {
        switch (tokenTag(tok_i)) {
            case TokenTag::char_literal: return addLeaf(NodeTag::char_literal);
            case TokenTag::number_literal: return addLeaf(NodeTag::number_literal);
            case TokenTag::keyword_unreachable: return addLeaf(NodeTag::unreachable_literal);
            case TokenTag::keyword_anyframe: return addLeaf(NodeTag::anyframe_literal);
            case TokenTag::string_literal: return addLeaf(NodeTag::string_literal);

            case TokenTag::builtin: return parseBuiltinCall();
            case TokenTag::keyword_fn: return parseFnProto();
            case TokenTag::keyword_if: return parseIf(Body::type);
            case TokenTag::keyword_switch: return expectSwitchExpr(false);

            case TokenTag::keyword_extern:
            case TokenTag::keyword_packed: tok_i += 1; return parseContainerDeclAuto();

            case TokenTag::keyword_struct:
            case TokenTag::keyword_opaque:
            case TokenTag::keyword_enum:
            case TokenTag::keyword_union: return parseContainerDeclAuto();

            case TokenTag::keyword_comptime: {
                TokenIndex t = nextToken();
                NodeIndex child = expectTypeExpr();
                return addUnary(NodeTag::comptime_, t, child);
            }
            case TokenTag::multiline_string_literal_line: {
                const TokenIndex first_line = nextToken();
                while (tokenTag(tok_i) == TokenTag::multiline_string_literal_line) tok_i += 1;
                return addNode(Node{NodeTag::multiline_string_literal, first_line, Data::pair(first_line, tok_i - 1)});
            }
            case TokenTag::identifier:
                if (tokenTag(tok_i + 1) == TokenTag::colon) {
                    switch (tokenTag(tok_i + 2)) {
                        case TokenTag::keyword_inline:
                            tok_i += 3;
                            switch (tokenTag(tok_i)) {
                                case TokenTag::keyword_for: return parseFor(Body::type);
                                case TokenTag::keyword_while: return parseWhileTypeExpr();
                                default: fail(ErrorTag::expected_inlinable);
                            }
                        case TokenTag::keyword_for: tok_i += 2; return parseFor(Body::type);
                        case TokenTag::keyword_while: tok_i += 2; return parseWhileTypeExpr();
                        case TokenTag::keyword_switch: tok_i += 2; return expectSwitchExpr(true);
                        case TokenTag::l_brace: tok_i += 2; return parseBlock();
                        default: return addLeaf(NodeTag::identifier);
                    }
                }
                return addLeaf(NodeTag::identifier);
            case TokenTag::keyword_inline:
                tok_i += 1;
                switch (tokenTag(tok_i)) {
                    case TokenTag::keyword_for: return parseFor(Body::type);
                    case TokenTag::keyword_while: return parseWhileTypeExpr();
                    default: fail(ErrorTag::expected_inlinable);
                }
            case TokenTag::keyword_for: return parseFor(Body::type);
            case TokenTag::keyword_while: return parseWhileTypeExpr();
            case TokenTag::period:
                switch (tokenTag(tok_i + 1)) {
                    case TokenTag::identifier:
                        tok_i += 1;
                        return addLeaf(NodeTag::enum_literal);  // main token: the identifier
                    case TokenTag::l_brace: return parseDotInitList();
                    default: return kNull;
                }
            case TokenTag::keyword_error:
                if (tokenTag(tok_i + 1) == TokenTag::l_brace) {
                    const TokenIndex error_token = tok_i;
                    tok_i += 2;
                    while (true) {
                        if (eatToken(TokenTag::r_brace) != kNone) break;
                        eatDocComments();
                        expectToken(TokenTag::identifier);
                        switch (tokenTag(tok_i)) {
                            case TokenTag::comma: tok_i += 1; break;
                            case TokenTag::r_brace: tok_i += 1; goto errset_done;
                            case TokenTag::colon:
                            case TokenTag::r_paren:
                            case TokenTag::r_bracket: failExpected(TokenTag::r_brace);
                            // Likely just a missing comma; give error but continue parsing.
                            default: warn(ErrorTag::expected_comma_after_field); break;
                        }
                    }
                errset_done:
                    return addNode(
                        Node{NodeTag::error_set_decl, error_token, Data::pair(error_token + 1, tok_i - 1)});
                } else {
                    const TokenIndex main_token = nextToken();
                    if (eatToken(TokenTag::period) == kNone) failExpected(TokenTag::period);
                    if (eatToken(TokenTag::identifier) == kNone) failExpected(TokenTag::identifier);
                    return addNode(Node{NodeTag::error_value, main_token, Data::undef()});
                }
            case TokenTag::l_paren: {
                TokenIndex t = nextToken();
                NodeIndex expr = expectExpr();
                TokenIndex rparen = expectToken(TokenTag::r_paren);
                return addNode(Node{NodeTag::grouped_expression, t, Data::pair(expr, rparen)});
            }
            default: return kNull;
        }
    }

    /// `.{ ... }` (the PrimaryTypeExpr `.l_brace` case).
    NodeIndex parseDotInitList() {
        const TokenIndex lbrace = tok_i + 1;
        tok_i = lbrace + 1;

        // If there are 0, 1, or 2 items, we can use ArrayInitDotTwo/StructInitDotTwo;
        // otherwise we use the full ArrayInitDot/StructInitDot.

        ScratchTop st(*this);
        const std::size_t scratch_top = st.top;
        NodeIndex opt_field_init = parseFieldInit();
        if (opt_field_init != kNull) {
            scratch.push_back(opt_field_init);
            while (true) {
                switch (tokenTag(tok_i)) {
                    case TokenTag::comma: tok_i += 1; break;
                    case TokenTag::r_brace: tok_i += 1; goto fields_done;
                    case TokenTag::colon:
                    case TokenTag::r_paren:
                    case TokenTag::r_bracket: failExpected(TokenTag::r_brace);
                    // Likely just a missing comma; give error but continue parsing.
                    default: warn(ErrorTag::expected_comma_after_initializer); break;
                }
                if (eatToken(TokenTag::r_brace) != kNone) break;
                NodeIndex next = expectFieldInit();
                scratch.push_back(next);
            }
        fields_done:
            const bool comma = tokenTag(tok_i - 2) == TokenTag::comma;
            std::span<const NodeIndex> inits(scratch.data() + scratch_top, scratch.size() - scratch_top);
            assert(!inits.empty());
            if (inits.size() <= 2) {
                return addNode(Node{comma ? NodeTag::struct_init_dot_two_comma : NodeTag::struct_init_dot_two, lbrace,
                                    Data::pair(inits.size() >= 1 ? inits[0] : kNone,
                                               inits.size() >= 2 ? inits[1] : kNone)});
            }
            SubRange r = listToSpan(inits);
            return addNode(
                Node{comma ? NodeTag::struct_init_dot_comma : NodeTag::struct_init_dot, lbrace, Data::pair(r.start, r.end)});
        }

        while (true) {
            if (eatToken(TokenTag::r_brace) != kNone) break;
            NodeIndex elem_init = expectExpr();
            scratch.push_back(elem_init);
            switch (tokenTag(tok_i)) {
                case TokenTag::comma: tok_i += 1; break;
                case TokenTag::r_brace: tok_i += 1; goto elems_done;
                case TokenTag::colon:
                case TokenTag::r_paren:
                case TokenTag::r_bracket: failExpected(TokenTag::r_brace);
                // Likely just a missing comma; give error but continue parsing.
                default: warn(ErrorTag::expected_comma_after_initializer); break;
            }
        }
    elems_done:
        const bool comma = tokenTag(tok_i - 2) == TokenTag::comma;
        std::span<const NodeIndex> inits(scratch.data() + scratch_top, scratch.size() - scratch_top);
        if (inits.size() <= 2) {
            NodeTag tag = inits.empty() ? NodeTag::struct_init_dot_two
                          : comma       ? NodeTag::array_init_dot_two_comma
                                        : NodeTag::array_init_dot_two;
            return addNode(Node{tag, lbrace,
                                Data::pair(inits.size() >= 1 ? inits[0] : kNone, inits.size() >= 2 ? inits[1] : kNone)});
        }
        SubRange r = listToSpan(inits);
        return addNode(
            Node{comma ? NodeTag::array_init_dot_comma : NodeTag::array_init_dot, lbrace, Data::pair(r.start, r.end)});
    }

    NodeIndex expectPrimaryTypeExpr() {
        NodeIndex n = parsePrimaryTypeExpr();
        if (n == kNull) fail(ErrorTag::expected_primary_type_expr);
        return n;
    }

    /// SwitchExpr <- KEYWORD_switch LPAREN Expr RPAREN LBRACE SwitchProngList RBRACE
    NodeIndex parseSwitchExpr(bool is_labeled) {
        TokenIndex switch_token = eatToken(TokenTag::keyword_switch);
        if (switch_token == kNone) return kNull;
        return expectSwitchSuffix(is_labeled ? switch_token - 2 : switch_token);
    }

    NodeIndex expectSwitchExpr(bool is_labeled) {
        TokenIndex switch_token = assertToken(TokenTag::keyword_switch);
        return expectSwitchSuffix(is_labeled ? switch_token - 2 : switch_token);
    }

    NodeIndex expectSwitchSuffix(TokenIndex main_token) {
        expectToken(TokenTag::l_paren);
        NodeIndex expr_node = expectExpr();
        expectToken(TokenTag::r_paren);
        expectToken(TokenTag::l_brace);
        SubRange cases = parseSwitchProngList();
        const bool trailing_comma = tokenTag(tok_i - 1) == TokenTag::comma;
        expectToken(TokenTag::r_brace);
        ExtraIndex e = addExtraSubRange(cases);
        return addNode(Node{trailing_comma ? NodeTag::switch_comma : NodeTag::switch_, main_token, Data::pair(expr_node, e)});
    }

    /// AsmExpr <- KEYWORD_asm KEYWORD_volatile? LPAREN Expr AsmOutput? RPAREN
    NodeIndex expectAsmExpr() {
        const TokenIndex asm_token = assertToken(TokenTag::keyword_asm);
        eatToken(TokenTag::keyword_volatile);
        expectToken(TokenTag::l_paren);
        const NodeIndex template_ = expectExpr();

        TokenIndex rparen = eatToken(TokenTag::r_paren);
        if (rparen != kNone) return addNode(Node{NodeTag::asm_simple, asm_token, Data::pair(template_, rparen)});

        expectToken(TokenTag::colon);

        ScratchTop st(*this);
        const std::size_t scratch_top = st.top;

        while (true) {
            NodeIndex output_item = parseAsmOutputItem();
            if (output_item == kNull) break;
            scratch.push_back(output_item);
            switch (tokenTag(tok_i)) {
                case TokenTag::comma: tok_i += 1; break;
                // All possible delimiters.
                case TokenTag::colon:
                case TokenTag::r_paren:
                case TokenTag::r_brace:
                case TokenTag::r_bracket: goto outputs_done;
                // Likely just a missing comma; give error but continue parsing.
                default: warnExpected(TokenTag::comma); break;
            }
        }
    outputs_done:

        OptionalIndex clobbers;
        if (eatToken(TokenTag::colon) != kNone) {
            while (true) {
                NodeIndex input_item = parseAsmInputItem();
                if (input_item == kNull) break;
                scratch.push_back(input_item);
                switch (tokenTag(tok_i)) {
                    case TokenTag::comma: tok_i += 1; break;
                    // All possible delimiters.
                    case TokenTag::colon:
                    case TokenTag::r_paren:
                    case TokenTag::r_brace:
                    case TokenTag::r_bracket: goto inputs_done;
                    // Likely just a missing comma; give error but continue parsing.
                    default: warnExpected(TokenTag::comma); break;
                }
            }
        inputs_done:
            if (eatToken(TokenTag::colon) != kNone) clobbers = opt(expectExpr());
        }

        rparen = expectToken(TokenTag::r_paren);
        SubRange span = listToSpan(std::span<const NodeIndex>(scratch.data() + scratch_top, scratch.size() - scratch_top));
        ExtraIndex e = addExtra({span.start, span.end, clobbers.value, rparen});
        return addNode(Node{NodeTag::asm_, asm_token, Data::pair(template_, e)});
    }

    /// AsmOutputItem <- LBRACKET IDENTIFIER RBRACKET STRINGLITERALSINGLE LPAREN (MINUSRARROW TypeExpr / IDENTIFIER) RPAREN
    NodeIndex parseAsmOutputItem() {
        if (eatToken(TokenTag::l_bracket) == kNone) return kNull;
        const TokenIndex identifier = expectToken(TokenTag::identifier);
        expectToken(TokenTag::r_bracket);
        expectToken(TokenTag::string_literal);
        expectToken(TokenTag::l_paren);
        NodeIndex type_expr = kNone;
        if (eatToken(TokenTag::arrow) != kNone) {
            type_expr = expectTypeExpr();
        } else {
            expectToken(TokenTag::identifier);
        }
        const TokenIndex rparen = expectToken(TokenTag::r_paren);
        return addNode(Node{NodeTag::asm_output, identifier, Data::pair(type_expr, rparen)});
    }

    /// AsmInputItem <- LBRACKET IDENTIFIER RBRACKET STRINGLITERALSINGLE LPAREN Expr RPAREN
    NodeIndex parseAsmInputItem() {
        if (eatToken(TokenTag::l_bracket) == kNone) return kNull;
        const TokenIndex identifier = expectToken(TokenTag::identifier);
        expectToken(TokenTag::r_bracket);
        expectToken(TokenTag::string_literal);
        expectToken(TokenTag::l_paren);
        const NodeIndex expr = expectExpr();
        const TokenIndex rparen = expectToken(TokenTag::r_paren);
        return addNode(Node{NodeTag::asm_input, identifier, Data::pair(expr, rparen)});
    }

    /// BreakLabel <- COLON IDENTIFIER
    OptionalTokenIndex parseBreakLabel() {
        TokenIndex i = eatTokens({TokenTag::colon, TokenTag::identifier});
        return i != kNone ? OptionalIndex::from(i + 1) : OptionalIndex::none();
    }

    /// BlockLabel <- IDENTIFIER COLON
    TokenIndex parseBlockLabel() { return eatTokens({TokenTag::identifier, TokenTag::colon}); }

    /// FieldInit <- DOT IDENTIFIER EQUAL Expr
    NodeIndex parseFieldInit() {
        if (eatTokens({TokenTag::period, TokenTag::identifier, TokenTag::equal}) != kNone) return expectExpr();
        return kNull;
    }

    NodeIndex expectFieldInit() {
        if (eatTokens({TokenTag::period, TokenTag::identifier, TokenTag::equal}) != kNone) return expectExpr();
        fail(ErrorTag::expected_initializer);
    }

    /// WhileContinueExpr <- COLON LPAREN AssignExpr RPAREN
    NodeIndex parseWhileContinueExpr() {
        if (eatToken(TokenTag::colon) == kNone) return kNull;
        expectToken(TokenTag::l_paren);
        NodeIndex node = parseAssignExpr();
        if (node == kNull) fail(ErrorTag::expected_expr_or_assignment);
        expectToken(TokenTag::r_paren);
        return node;
    }

    /// `KEYWORD LPAREN Expr RPAREN`: LinkSection, CallConv, AddrSpace, ByteAlign.
    NodeIndex parseKeywordParenExpr(TokenTag keyword) {
        if (eatToken(keyword) == kNone) return kNull;
        expectToken(TokenTag::l_paren);
        NodeIndex expr_node = expectExpr();
        expectToken(TokenTag::r_paren);
        return expr_node;
    }
    NodeIndex parseLinkSection() { return parseKeywordParenExpr(TokenTag::keyword_linksection); }
    NodeIndex parseCallconv() { return parseKeywordParenExpr(TokenTag::keyword_callconv); }
    NodeIndex parseAddrSpace() { return parseKeywordParenExpr(TokenTag::keyword_addrspace); }
    NodeIndex parseByteAlign() { return parseKeywordParenExpr(TokenTag::keyword_align); }

    /// ParamDecl. May return null and still be followed by more params
    /// (anytype, `...`).
    NodeIndex expectParamDecl() {
        eatDocComments();
        switch (tokenTag(tok_i)) {
            case TokenTag::keyword_noalias:
            case TokenTag::keyword_comptime: tok_i += 1; break;
            case TokenTag::ellipsis3: tok_i += 1; return kNull;
            default: break;
        }
        eatTokens({TokenTag::identifier, TokenTag::colon});
        if (eatToken(TokenTag::keyword_anytype) != kNone) return kNull;
        return expectTypeExpr();
    }

    /// Payload <- PIPE IDENTIFIER PIPE
    OptionalTokenIndex parsePayload() {
        if (eatToken(TokenTag::pipe) == kNone) return OptionalIndex::none();
        const TokenIndex identifier = expectToken(TokenTag::identifier);
        expectToken(TokenTag::pipe);
        return OptionalIndex::from(identifier);
    }

    /// PtrPayload <- PIPE ASTERISK? IDENTIFIER PIPE
    OptionalTokenIndex parsePtrPayload() {
        if (eatToken(TokenTag::pipe) == kNone) return OptionalIndex::none();
        eatToken(TokenTag::asterisk);
        const TokenIndex identifier = expectToken(TokenTag::identifier);
        expectToken(TokenTag::pipe);
        return OptionalIndex::from(identifier);
    }

    /// PtrIndexPayload <- PIPE ASTERISK? IDENTIFIER (COMMA IDENTIFIER)? PIPE
    OptionalTokenIndex parsePtrIndexPayload() {
        if (eatToken(TokenTag::pipe) == kNone) return OptionalIndex::none();
        eatToken(TokenTag::asterisk);
        const TokenIndex identifier = expectToken(TokenTag::identifier);
        if (eatToken(TokenTag::comma) != kNone) expectToken(TokenTag::identifier);
        expectToken(TokenTag::pipe);
        return OptionalIndex::from(identifier);
    }

    /// SwitchProng <- KEYWORD_inline? SwitchCase EQUALRARROW PtrIndexPayload? AssignExpr
    NodeIndex parseSwitchProng() {
        ScratchTop st(*this);
        const std::size_t scratch_top = st.top;

        const bool is_inline = eatToken(TokenTag::keyword_inline) != kNone;

        if (eatToken(TokenTag::keyword_else) == kNone) {
            while (true) {
                NodeIndex item = parseSwitchItem();
                if (item == kNull) break;
                scratch.push_back(item);
                if (eatToken(TokenTag::comma) == kNone) break;
            }
            if (scratch_top == scratch.size()) {
                if (is_inline) tok_i -= 1;
                return kNull;
            }
        }
        const TokenIndex arrow_token = expectToken(TokenTag::equal_angle_bracket_right);
        parsePtrIndexPayload();

        std::span<const NodeIndex> items(scratch.data() + scratch_top, scratch.size() - scratch_top);
        if (items.size() <= 1) {
            const NodeIndex first = items.size() >= 1 ? items[0] : kNone;
            NodeIndex target = expectSingleAssignExpr();
            return addNode(Node{is_inline ? NodeTag::switch_case_inline_one : NodeTag::switch_case_one, arrow_token,
                                Data::pair(first, target)});
        }
        SubRange r = listToSpan(items);
        ExtraIndex e = addExtraSubRange(r);
        NodeIndex target = expectSingleAssignExpr();
        return addNode(
            Node{is_inline ? NodeTag::switch_case_inline : NodeTag::switch_case, arrow_token, Data::pair(e, target)});
    }

    /// SwitchItem <- Expr (DOT3 Expr)?
    NodeIndex parseSwitchItem() {
        NodeIndex expr = parseExpr();
        if (expr == kNull) return kNull;

        TokenIndex token = eatToken(TokenTag::ellipsis3);
        if (token != kNone) {
            NodeIndex rhs = expectExpr();
            return addNode(Node{NodeTag::switch_range, token, Data::pair(expr, rhs)});
        }
        return expr;
    }

    PtrModifiers parsePtrModifiers() {
        PtrModifiers result{};
        bool saw_const = false;
        bool saw_volatile = false;
        bool saw_allowzero = false;
        while (true) {
            switch (tokenTag(tok_i)) {
                case TokenTag::keyword_align:
                    if (!result.align_node.isNone()) warn(ErrorTag::extra_align_qualifier);
                    tok_i += 1;
                    expectToken(TokenTag::l_paren);
                    result.align_node = opt(expectExpr());

                    if (eatToken(TokenTag::colon) != kNone) {
                        result.bit_range_start = opt(expectExpr());
                        expectToken(TokenTag::colon);
                        result.bit_range_end = opt(expectExpr());
                    }

                    expectToken(TokenTag::r_paren);
                    break;
                case TokenTag::keyword_const:
                    if (saw_const) warn(ErrorTag::extra_const_qualifier);
                    tok_i += 1;
                    saw_const = true;
                    break;
                case TokenTag::keyword_volatile:
                    if (saw_volatile) warn(ErrorTag::extra_volatile_qualifier);
                    tok_i += 1;
                    saw_volatile = true;
                    break;
                case TokenTag::keyword_allowzero:
                    if (saw_allowzero) warn(ErrorTag::extra_allowzero_qualifier);
                    tok_i += 1;
                    saw_allowzero = true;
                    break;
                case TokenTag::keyword_addrspace:
                    if (!result.addrspace_node.isNone()) warn(ErrorTag::extra_addrspace_qualifier);
                    result.addrspace_node = opt(parseAddrSpace());
                    break;
                default: return result;
            }
        }
    }

    /// SuffixOp
    NodeIndex parseSuffixOp(NodeIndex lhs) {
        switch (tokenTag(tok_i)) {
            case TokenTag::l_bracket: {
                const TokenIndex lbracket = nextToken();
                const NodeIndex index_expr = expectExpr();

                if (eatToken(TokenTag::ellipsis2) != kNone) {
                    const NodeIndex opt_end_expr = parseExpr();
                    if (eatToken(TokenTag::colon) != kNone) {
                        const NodeIndex sentinel = expectExpr();
                        expectToken(TokenTag::r_bracket);
                        ExtraIndex e = addExtra({index_expr, opt_end_expr, sentinel});
                        return addNode(Node{NodeTag::slice_sentinel, lbracket, Data::pair(lhs, e)});
                    }
                    expectToken(TokenTag::r_bracket);
                    if (opt_end_expr == kNull)
                        return addNode(Node{NodeTag::slice_open, lbracket, Data::pair(lhs, index_expr)});
                    ExtraIndex e = addExtra({index_expr, opt_end_expr});
                    return addNode(Node{NodeTag::slice, lbracket, Data::pair(lhs, e)});
                }
                expectToken(TokenTag::r_bracket);
                return addNode(Node{NodeTag::array_access, lbracket, Data::pair(lhs, index_expr)});
            }
            case TokenTag::period_asterisk: return addUnary(NodeTag::deref, nextToken(), lhs);
            case TokenTag::invalid_periodasterisks:
                warn(ErrorTag::asterisk_after_ptr_deref);
                return addUnary(NodeTag::deref, nextToken(), lhs);
            case TokenTag::period:
                switch (tokenTag(tok_i + 1)) {
                    case TokenTag::identifier: {
                        TokenIndex dot = nextToken();
                        TokenIndex name = nextToken();
                        return addNode(Node{NodeTag::field_access, dot, Data::pair(lhs, name)});
                    }
                    case TokenTag::question_mark: {
                        TokenIndex dot = nextToken();
                        TokenIndex q = nextToken();
                        return addNode(Node{NodeTag::unwrap_optional, dot, Data::pair(lhs, q)});
                    }
                    case TokenTag::l_brace:
                        // this a misplaced `.{`, handle the error somewhere else
                        return kNull;
                    default:
                        tok_i += 1;
                        warn(ErrorTag::expected_suffix_op);
                        return kNull;
                }
            default: return kNull;
        }
    }

    NodeIndex addContainerMembers(NodeTag two, NodeTag two_trailing, NodeTag many, NodeTag many_trailing,
                                  TokenIndex main_token, const Members& members) {
        if (members.len <= 2)
            return addNode(Node{members.trailing ? two_trailing : two, main_token, members.data});
        SubRange span = members.toSpan(*this);
        return addNode(Node{members.trailing ? many_trailing : many, main_token, Data::pair(span.start, span.end)});
    }

    /// ContainerDeclAuto <- ContainerDeclType LBRACE ContainerMembers RBRACE
    NodeIndex parseContainerDeclAuto() {
        const TokenIndex main_token = nextToken();
        NodeIndex arg_expr = kNull;
        switch (tokenTag(main_token)) {
            case TokenTag::keyword_opaque: break;
            case TokenTag::keyword_struct:
            case TokenTag::keyword_enum:
                if (eatToken(TokenTag::l_paren) != kNone) {
                    arg_expr = expectExpr();
                    expectToken(TokenTag::r_paren);
                }
                break;
            case TokenTag::keyword_union:
                if (eatToken(TokenTag::l_paren) != kNone) {
                    if (eatToken(TokenTag::keyword_enum) != kNone) {
                        if (eatToken(TokenTag::l_paren) != kNone) {
                            const NodeIndex enum_tag_expr = expectExpr();
                            expectToken(TokenTag::r_paren);
                            expectToken(TokenTag::r_paren);

                            expectToken(TokenTag::l_brace);
                            Members members = parseContainerMembers();
                            SubRange members_span = members.toSpan(*this);
                            expectToken(TokenTag::r_brace);
                            ExtraIndex e = addExtraSubRange(members_span);
                            return addNode(Node{members.trailing ? NodeTag::tagged_union_enum_tag_trailing
                                                                 : NodeTag::tagged_union_enum_tag,
                                                main_token, Data::pair(enum_tag_expr, e)});
                        }
                        expectToken(TokenTag::r_paren);

                        expectToken(TokenTag::l_brace);
                        Members members = parseContainerMembers();
                        expectToken(TokenTag::r_brace);
                        return addContainerMembers(NodeTag::tagged_union_two, NodeTag::tagged_union_two_trailing,
                                                   NodeTag::tagged_union, NodeTag::tagged_union_trailing, main_token,
                                                   members);
                    }
                    arg_expr = expectExpr();
                    expectToken(TokenTag::r_paren);
                }
                break;
            default: tok_i -= 1; fail(ErrorTag::expected_container);
        }
        expectToken(TokenTag::l_brace);
        Members members = parseContainerMembers();
        expectToken(TokenTag::r_brace);
        if (arg_expr == kNull) {
            return addContainerMembers(NodeTag::container_decl_two, NodeTag::container_decl_two_trailing,
                                       NodeTag::container_decl, NodeTag::container_decl_trailing, main_token, members);
        }
        SubRange span = members.toSpan(*this);
        ExtraIndex e = addExtraSubRange(span);
        return addNode(Node{members.trailing ? NodeTag::container_decl_arg_trailing : NodeTag::container_decl_arg,
                            main_token, Data::pair(arg_expr, e)});
    }

    /// Give a helpful error message for those transitioning from
    /// C's 'struct Foo {};' to Zig's 'const Foo = struct {};'.
    bool parseCStyleContainer() {
        const TokenIndex main_token = tok_i;
        switch (tokenTag(tok_i)) {
            case TokenTag::keyword_enum:
            case TokenTag::keyword_union:
            case TokenTag::keyword_struct: break;
            default: return false;
        }
        const TokenIndex identifier = tok_i + 1;
        if (tokenTag(identifier) != TokenTag::identifier) return false;
        tok_i += 2;

        Error e1{};
        e1.tag = ErrorTag::c_style_container;
        e1.token = identifier;
        e1.expected_tag = tokenTag(main_token);
        warnMsg(e1);
        Error e2{};
        e2.tag = ErrorTag::zig_style_container;
        e2.is_note = true;
        e2.token = identifier;
        e2.expected_tag = tokenTag(main_token);
        warnMsg(e2);

        expectToken(TokenTag::l_brace);
        parseContainerMembers();
        expectToken(TokenTag::r_brace);
        expectSemicolon(ErrorTag::expected_semi_after_decl, true);
        return true;
    }

    /// SwitchProngList <- (SwitchProng COMMA)* SwitchProng?
    SubRange parseSwitchProngList() {
        ScratchTop st(*this);
        const std::size_t scratch_top = st.top;

        while (true) {
            NodeIndex item = parseSwitchProng();
            if (item == kNull) break;

            scratch.push_back(item);

            switch (tokenTag(tok_i)) {
                case TokenTag::comma: tok_i += 1; break;
                // All possible delimiters.
                case TokenTag::colon:
                case TokenTag::r_paren:
                case TokenTag::r_brace:
                case TokenTag::r_bracket: goto done;
                // Likely just a missing comma; give error but continue parsing.
                default: warn(ErrorTag::expected_comma_after_switch_prong); break;
            }
        }
    done:
        return listToSpan(std::span<const NodeIndex>(scratch.data() + scratch_top, scratch.size() - scratch_top));
    }

    /// ParamDeclList <- (ParamDecl COMMA)* (ParamDecl / DOT3 COMMA?)?
    SmallSpan parseParamDeclList() {
        expectToken(TokenTag::l_paren);
        ScratchTop st(*this);
        const std::size_t scratch_top = st.top;
        enum class Varargs { none, seen, nonfinal };
        Varargs varargs = Varargs::none;
        TokenIndex varargs_nonfinal = 0;
        while (true) {
            if (eatToken(TokenTag::r_paren) != kNone) break;
            if (varargs == Varargs::seen) {
                varargs = Varargs::nonfinal;
                varargs_nonfinal = tok_i;
            }
            NodeIndex opt_param = expectParamDecl();
            if (opt_param != kNull) {
                scratch.push_back(opt_param);
            } else if (tokenTag(tok_i - 1) == TokenTag::ellipsis3) {
                if (varargs == Varargs::none) varargs = Varargs::seen;
            }
            switch (tokenTag(tok_i)) {
                case TokenTag::comma: tok_i += 1; break;
                case TokenTag::r_paren: tok_i += 1; goto params_done;
                case TokenTag::colon:
                case TokenTag::r_brace:
                case TokenTag::r_bracket: failExpected(TokenTag::r_paren);
                // Likely just a missing comma; give error but continue parsing.
                default: warn(ErrorTag::expected_comma_after_param); break;
            }
        }
    params_done:
        if (varargs == Varargs::nonfinal) {
            Error e{};
            e.tag = ErrorTag::varargs_nonfinal;
            e.token = varargs_nonfinal;
            warnMsg(e);
        }
        std::span<const NodeIndex> params(scratch.data() + scratch_top, scratch.size() - scratch_top);
        switch (params.size()) {
            case 0: return SmallSpan{false, OptionalIndex::none(), {}};
            case 1: return SmallSpan{false, opt(params[0]), {}};
            default: return SmallSpan{true, OptionalIndex::none(), listToSpan(params)};
        }
    }

    /// FnCallArguments <- LPAREN ExprList RPAREN
    NodeIndex parseBuiltinCall() {
        const TokenIndex builtin_token = assertToken(TokenTag::builtin);
        if (eatToken(TokenTag::l_paren) == kNone) {
            warn(ErrorTag::expected_param_list);
            // Pretend this was an identifier so we can continue parsing.
            return addNode(Node{NodeTag::identifier, builtin_token, Data::undef()});
        }
        ScratchTop st(*this);
        const std::size_t scratch_top = st.top;
        while (true) {
            if (eatToken(TokenTag::r_paren) != kNone) break;
            NodeIndex param = expectExpr();
            scratch.push_back(param);
            switch (tokenTag(tok_i)) {
                case TokenTag::comma: tok_i += 1; break;
                case TokenTag::r_paren: tok_i += 1; goto args_done;
                // Likely just a missing comma; give error but continue parsing.
                default: warn(ErrorTag::expected_comma_after_arg); break;
            }
        }
    args_done:
        const bool comma = tokenTag(tok_i - 2) == TokenTag::comma;
        std::span<const NodeIndex> params(scratch.data() + scratch_top, scratch.size() - scratch_top);
        if (params.size() <= 2) {
            return addNode(Node{comma ? NodeTag::builtin_call_two_comma : NodeTag::builtin_call_two, builtin_token,
                                Data::pair(params.size() >= 1 ? params[0] : kNone,
                                           params.size() >= 2 ? params[1] : kNone)});
        }
        SubRange span = listToSpan(params);
        return addNode(Node{comma ? NodeTag::builtin_call_comma : NodeTag::builtin_call, builtin_token,
                            Data::pair(span.start, span.end)});
    }

    /// IfPrefix <- KEYWORD_if LPAREN Expr RPAREN PtrPayload?
    NodeIndex parseIf(Body body) {
        const TokenIndex if_token = eatToken(TokenTag::keyword_if);
        if (if_token == kNone) return kNull;
        expectToken(TokenTag::l_paren);
        const NodeIndex condition = expectExpr();
        expectToken(TokenTag::r_paren);
        parsePtrPayload();

        const NodeIndex then_expr = parseBody(body);

        if (eatToken(TokenTag::keyword_else) == kNone)
            return addNode(Node{NodeTag::if_simple, if_token, Data::pair(condition, then_expr)});
        parsePayload();
        const NodeIndex else_expr = parseBody(body);

        ExtraIndex e = addExtra({then_expr, else_expr});
        return addNode(Node{NodeTag::if_, if_token, Data::pair(condition, e)});
    }

    /// ForExpr / ForTypeExpr
    NodeIndex parseFor(Body body) {
        const TokenIndex for_token = eatToken(TokenTag::keyword_for);
        if (for_token == kNone) return kNull;

        ScratchTop st(*this);
        const std::size_t scratch_top = st.top;
        const std::size_t inputs = forPrefix();

        const NodeIndex then_expr = parseBody(body);
        bool has_else = false;
        if (eatToken(TokenTag::keyword_else) != kNone) {
            scratch.push_back(then_expr);
            const NodeIndex else_expr = parseBody(body);
            scratch.push_back(else_expr);
            has_else = true;
        } else if (inputs == 1) {
            return addNode(Node{NodeTag::for_simple, for_token, Data::pair(scratch[scratch_top], then_expr)});
        } else {
            scratch.push_back(then_expr);
        }
        SubRange r = listToSpan(std::span<const NodeIndex>(scratch.data() + scratch_top, scratch.size() - scratch_top));
        return addNode(Node{NodeTag::for_, for_token,
                            Data::pair(r.start, ForData{static_cast<std::uint32_t>(inputs), has_else}.pack())});
    }

    /// Skips over doc comment tokens. Returns the first one, if any.
    std::optional<TokenIndex> eatDocComments() {
        TokenIndex tok = eatToken(TokenTag::doc_comment);
        if (tok == kNone) return std::nullopt;
        TokenIndex first_line = tok;
        if (tok > 0 && tokensOnSameLine(tok - 1, tok)) {
            Error e{};
            e.tag = ErrorTag::same_line_doc_comment;
            e.token = tok;
            warnMsg(e);
            first_line = eatToken(TokenTag::doc_comment);
            if (first_line == kNone) return std::nullopt;
        }
        while (eatToken(TokenTag::doc_comment) != kNone) {
        }
        return first_line;
    }

    bool tokensOnSameLine(TokenIndex token1, TokenIndex token2) const {
        std::string_view s = source.substr(tokenStart(token1), tokenStart(token2) - tokenStart(token1));
        return s.find('\n') == std::string_view::npos;
    }

    TokenIndex eatToken(TokenTag tag) { return tokenTag(tok_i) == tag ? nextToken() : kNone; }

    TokenIndex eatTokens(std::initializer_list<TokenTag> tags) {
        TokenIndex i = tok_i;
        for (TokenTag t : tags) {
            if (i >= tokens.size() || tokenTag(i) != t) return kNone;
            ++i;
        }
        const TokenIndex result = tok_i;
        tok_i = i;
        return result;
    }

    TokenIndex assertToken(TokenTag tag) {
        const TokenIndex token = nextToken();
        assert(tokenTag(token) == tag);
        (void)tag;
        return token;
    }

    TokenIndex expectToken(TokenTag tag) {
        if (tokenTag(tok_i) != tag) {
            Error e{};
            e.tag = ErrorTag::expected_token;
            e.token = tok_i;
            e.expected_tag = tag;
            failMsg(e);
        }
        return nextToken();
    }

    void expectSemicolon(ErrorTag error_tag, bool recoverable) {
        if (tokenTag(tok_i) == TokenTag::semicolon) {
            nextToken();
            return;
        }
        warn(error_tag);
        if (!recoverable) throw ParseError{};
    }

    TokenIndex nextToken() {
        const TokenIndex result = tok_i;
        tok_i += 1;
        return result;
    }
};

}  // namespace

Ast parse(std::string_view source_in, Mode mode) {
    Ast tree;
    tree.source.assign(source_in.data(), source_in.size());
    tree.mode = mode;

    // Empirically, the zig std lib has an 8:1 ratio of source bytes to token count.
    tree.tokens.reserve(tree.source.size() / 8 + 1);
    Tokenizer tokenizer = Tokenizer::init(tree.source.c_str(), tree.source.size());
    while (true) {
        const Token token = tokenizer.next();
        tree.tokens.push_back(TokenEntry{token.tag, static_cast<ByteOffset>(token.start)});
        if (token.tag == TokenTag::eof) break;
    }

    Parse parser(tree.source, tree.tokens);
    // Empirically, Zig source code has a 2:1 ratio of tokens to AST nodes.
    parser.nodes.reserve((tree.tokens.size() + 2) / 2);
    try {
        switch (mode) {
            case Mode::zig: parser.parseRoot(); break;
            case Mode::zon: parser.parseZon(); break;
        }
    } catch (const NestingLimit&) {
        // Too deep to parse safely: report it and leave an empty root.
        Error e{};
        e.tag = ErrorTag::nesting_too_deep;
        e.token = parser.tok_i < tree.tokens.size() ? parser.tok_i : static_cast<TokenIndex>(tree.tokens.size() - 1);
        parser.errors.insert(parser.errors.begin(), e);
        parser.nodes.resize(1);
        parser.extra_data.clear();
        if (mode == Mode::zig) {
            parser.nodes[0].data = Data::pair(0, 0);
        } else {
            parser.nodes[0].data = Data::undef();
        }
    } catch (const ParseError&) {
        // parseRoot recovers from every ParseError; parseZon catches its own.
        // A ParseError escaping means expectStatementRecoverable hit eof.
        assert(!parser.errors.empty());
    }

    tree.nodes = std::move(parser.nodes);
    tree.extra_data = std::move(parser.extra_data);
    tree.errors = std::move(parser.errors);
    return tree;
}

}  // namespace zig
