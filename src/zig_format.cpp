/**
 * @file zig_format.cpp
 * @brief mep's Zig formatter: a port of `zig fmt` (see zig_format.h).
 *
 * A decision-for-decision port of Zig 0.16's lib/std/zig/Ast/Render.zig over
 * zig_ast.h's port of std.zig.Ast. Function names and order follow the Zig
 * file so the two can be read side by side. Render.zig's `Fixups` (used by
 * `zig reduce`, never by `zig fmt`) are left out: `zig fmt` renders with
 * empty fixups, so every fixup branch is dead there.
 *
 * Writer.WriteFailed is a C++ exception (WriteFailed), thrown only by the
 * "no newline" writer behind rendersMultiline, exactly where Zig's
 * drainNoNewline fails. Recursion follows the tree, whose depth the parser
 * caps (zig::kMaxNesting).
 *
 * Ported from Zig (https://ziglang.org), lib/std/zig/Ast/Render.zig and the
 * helpers it uses (std.zig.fmtString, string_literal.parseEscapeSequence,
 * primitives.isPrimitive, isValidId), under the MIT license:
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
#include "zig_format.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "zig_ast.h"

namespace zigfmt {

namespace {

using zig::Ast;
using zig::NodeIndex;
using zig::NodeTag;
using zig::OptionalIndex;
using zig::TokenIndex;
using zig::TokenTag;
namespace full = zig::full;

constexpr std::size_t indent_delta = 4;
constexpr std::size_t asm_indent_delta = 2;

/// Writer.Error.WriteFailed.
struct WriteFailed {};

/// The std.Io.Writer the stream writes to: a growing string, or (for
/// rendersMultiline) a sink that fails on the first newline.
struct Writer {
    std::string buf;
    bool no_newline = false;

    void writeAll(std::string_view bytes) {
        if (no_newline) {
            if (bytes.find('\n') != std::string_view::npos) throw WriteFailed{};
            return;
        }
        buf.append(bytes);
    }
    void writeByte(char byte) {
        if (no_newline) {
            if (byte == '\n') throw WriteFailed{};
            return;
        }
        buf.push_back(byte);
    }
    void splatByteAll(char byte, std::size_t n) {
        if (no_newline) {
            if (byte == '\n' && n != 0) throw WriteFailed{};
            return;
        }
        buf.append(n, byte);
    }
};

enum class Space {
    /// Output the token lexeme only.
    none,
    /// Output the token lexeme followed by a single space.
    space,
    /// Output the token lexeme followed by a newline.
    newline,
    /// If the next token is a comma, render it as well. If not, insert one.
    /// In either case, a newline will be inserted afterwards.
    comma,
    /// Additionally consume the next token if it is a comma.
    /// In either case, a space will be inserted afterwards.
    comma_space,
    /// Additionally consume the next token if it is a semicolon.
    /// In either case, a newline will be inserted afterwards.
    semicolon,
    /// If the next token is not a multiline string literal, this acts as .space,
    /// otherwise this acts as .none.
    maybe_space,
    /// Additionally consume the next token if it is a comma.
    /// In either case, a space will be inserted afterwards
    /// if the following token is not a multiline string literal.
    comma_maybe_space,
    /// Skip rendering whitespace and comments. If this is used, the caller
    /// *must* handle whitespace and comments manually.
    skip,
};

enum class IndentType { normal, after_equals, binop, field_access };

/// AutoIndentingStream: inserts indentation of written data by keeping track
/// of the current indentation level (see Render.zig for the full story).
struct AutoIndentingStream {
    struct StackElem {
        IndentType indent_type;
        bool realized;
    };
    struct SpaceElem {
        Space space;
        std::size_t indent_count;
    };

    Writer *underlying_writer;
    /// Offset into the source at which formatting has been disabled with a
    /// `zig fmt: off` comment.
    std::optional<std::size_t> disabled_offset;
    std::size_t indent_count = 0;
    std::size_t indent_delta_;
    std::vector<StackElem> indent_stack;
    std::vector<SpaceElem> space_stack;
    std::optional<std::size_t> space_mode;
    std::size_t disable_indent_committing = 0;
    bool current_line_empty = true;
    /// the most recently applied indent
    std::size_t applied_indent = 0;

    AutoIndentingStream(Writer *w, std::size_t starting_indent_delta)
        : underlying_writer(w), indent_delta_(starting_indent_delta) {}

    void writeAll(std::string_view bytes) {
        if (bytes.empty()) return;
        applyIndent();
        if (!disabled_offset) underlying_writer->writeAll(bytes);
        if (bytes.back() == '\n') resetLine();
    }

    /// print("{s}\n", ...): the data ends with the format's newline.
    void printLine(std::string_view bytes) {
        applyIndent();
        if (!disabled_offset) {
            underlying_writer->writeAll(bytes);
            underlying_writer->writeByte('\n');
        }
        resetLine();
    }

    /// print("{f}", fmtString(...)): never ends with a newline.
    void printEscaped(std::string_view bytes) {
        applyIndent();
        if (!disabled_offset) underlying_writer->writeAll(bytes);
    }

    void writeByte(char byte) {
        applyIndent();
        if (!disabled_offset) underlying_writer->writeByte(byte);
    }

    void splatByteAll(char byte, std::size_t n) {
        applyIndent();
        if (!disabled_offset) underlying_writer->splatByteAll(byte, n);
    }

    // Change the indent delta without changing the final indentation level
    void setIndentDelta(std::size_t new_indent_delta) {
        if (indent_delta_ == new_indent_delta) {
            return;
        } else if (indent_delta_ > new_indent_delta) {
            indent_count = indent_count * (indent_delta_ / new_indent_delta);
        } else {
            indent_count = indent_count / (new_indent_delta / indent_delta_);
        }
        indent_delta_ = new_indent_delta;
    }

    void insertNewline() {
        if (!disabled_offset) underlying_writer->writeByte('\n');
        resetLine();
    }

    /// Insert a newline unless the current line is blank
    void maybeInsertNewline() {
        if (!current_line_empty) insertNewline();
    }

    /// Checks to see if the most recent indentation exceeds the currently pushed indents
    bool isLineOverIndented() const {
        if (current_line_empty) return false;
        return applied_indent > currentIndent();
    }

    void resetLine() {
        current_line_empty = true;

        if (disable_indent_committing > 0) return;

        if (!indent_stack.empty()) {
            // By default, we realize the most recent indentation scope.
            std::size_t to_realize = indent_stack.size() - 1;

            if (indent_stack.size() >= 2 && indent_stack[to_realize - 1].indent_type == IndentType::after_equals &&
                indent_stack[to_realize - 1].realized && indent_stack[to_realize].indent_type == IndentType::binop) {
                // If we are in a .binop scope and our direct parent is .after_equals, don't indent.
                return;
            }

            if (indent_stack[to_realize].indent_type == IndentType::field_access) {
                // Only realize the top-most field_access in a chain.
                while (to_realize > 0 && indent_stack[to_realize - 1].indent_type == IndentType::field_access)
                    to_realize -= 1;
            }

            if (indent_stack[to_realize].realized) return;
            indent_stack[to_realize].realized = true;
            indent_count += 1;
        }
    }

    /// Disables indentation level changes during the next newlines until re-enabled.
    void disableIndentCommitting() { disable_indent_committing += 1; }
    void enableIndentCommitting() { disable_indent_committing -= 1; }

    void pushSpace(Space space) { space_stack.push_back(SpaceElem{space, indent_count}); }
    void popSpace() { space_stack.pop_back(); }

    /// Sets current indentation level to be the same as that of the last pushSpace.
    void enableSpaceMode(Space space) {
        if (space_stack.empty()) return;
        const SpaceElem curr = space_stack.back();
        if (curr.space != space) return;
        space_mode = curr.indent_count;
    }

    void disableSpaceMode() { space_mode.reset(); }

    std::size_t lastSpaceModeIndent() const {
        if (space_stack.empty()) return 0;
        return space_stack.back().indent_count * indent_delta_;
    }

    /// Push default indentation. Doesn't actually write any indentation.
    void pushIndent(IndentType indent_type) { indent_stack.push_back(StackElem{indent_type, false}); }

    /// Forces an indentation level to be realized.
    void forcePushIndent(IndentType indent_type) {
        indent_stack.push_back(StackElem{indent_type, true});
        indent_count += 1;
    }

    void popIndent() {
        const bool realized = indent_stack.back().realized;
        indent_stack.pop_back();
        if (realized) indent_count -= 1;
    }

    /// Forces the last pushed indent to be realized
    void forceLastIndent() {
        StackElem &top = indent_stack.back();
        if (!top.realized) {
            top.realized = true;
            indent_count += 1;
        }
    }

    bool indentStackEmpty() const { return indent_stack.empty(); }

    /// Writes ' ' bytes if the current line is empty
    void applyIndent() {
        const std::size_t current_indent = currentIndent();
        if (current_line_empty) {
            if (current_indent > 0 && !disabled_offset) underlying_writer->splatByteAll(' ', current_indent);
            applied_indent = current_indent;
        }
        current_line_empty = false;
    }

    std::size_t currentIndent() const {
        const std::size_t count = space_mode ? *space_mode : indent_count;
        return count * indent_delta_;
    }
};

struct Render {
    AutoIndentingStream *ais;
    const Ast *tree;
};

enum class Container { enum_, tuple, other };

enum class QuoteBehavior { preserve_when_shadowing, eagerly_unquote, eagerly_unquote_except_underscore };

// ---- small helpers from std ---------------------------------------------------

bool isAsciiWhitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\x0b' || c == '\x0c';
}

std::string_view trimEnd(std::string_view s) {
    while (!s.empty() && isAsciiWhitespace(s.back())) s.remove_suffix(1);
    return s;
}

std::string_view trimStart(std::string_view s) {
    while (!s.empty() && isAsciiWhitespace(s.front())) s.remove_prefix(1);
    return s;
}

bool containsAtLeastTwoNewlines(std::string_view s) {
    return std::count(s.begin(), s.end(), '\n') >= 2;
}

/// std.zig.stringEscape (the `{f}` of fmtString).
void stringEscape(std::string_view bytes, std::string &out) {
    static const char kHex[] = "0123456789abcdef";
    for (const char ch : bytes) {
        const auto byte = static_cast<unsigned char>(ch);
        switch (byte) {
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\'': out += '\''; break;
            default:
                if (byte == ' ' || byte == '!' || (byte >= '#' && byte <= '&') || (byte >= '(' && byte <= '[') ||
                    (byte >= ']' && byte <= '~')) {
                    out += ch;
                } else {
                    out += "\\x";
                    out += kHex[byte >> 4];
                    out += kHex[byte & 0xF];
                }
        }
    }
}

/// string_literal.parseEscapeSequence: the codepoint, or nullopt on failure.
/// `offset` moves exactly as Zig's does (past the escape on success; by two,
/// or not at all for a trailing backslash, on failure).
std::optional<std::uint32_t> parseEscapeSequence(std::string_view slice, std::size_t &offset) {
    if (slice.size() == offset + 1) return std::nullopt;
    offset += 2;
    const auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    switch (slice[offset - 1]) {
        case 'n': return '\n';
        case 'r': return '\r';
        case '\\': return '\\';
        case 't': return '\t';
        case '\'': return '\'';
        case '"': return '"';
        case 'x': {
            std::uint32_t value = 0;
            std::size_t i = offset;
            for (; i < offset + 2; i++) {
                if (i == slice.size()) return std::nullopt;
                const int h = hexval(slice[i]);
                if (h < 0) return std::nullopt;
                value = value * 16 + static_cast<std::uint32_t>(h);
            }
            offset = i;
            return value;
        }
        case 'u': {
            std::size_t i = offset;
            if (i >= slice.size() || slice[i] != '{') return std::nullopt;
            i += 1;
            if (i >= slice.size()) return std::nullopt;
            if (slice[i] == '}') return std::nullopt;
            std::uint32_t value = 0;
            bool closed = false;
            for (; i < slice.size(); i++) {
                const char c = slice[i];
                if (c == '}') {
                    i += 1;
                    closed = true;
                    break;
                }
                const int h = hexval(c);
                if (h < 0) return std::nullopt;
                value = value * 16 + static_cast<std::uint32_t>(h);
                if (value > 0x10ffff) return std::nullopt;
            }
            if (!closed) return std::nullopt;
            offset = i;
            return value;
        }
        default: return std::nullopt;
    }
}

/// std.zig.primitives.isPrimitive.
bool isPrimitive(std::string_view name) {
    static constexpr std::string_view kNames[] = {
        "anyerror", "anyframe", "anyopaque", "bool",     "c_int",   "c_long",         "c_longdouble",
        "c_longlong", "c_char", "c_short",   "c_uint",   "c_ulong", "c_ulonglong",    "c_ushort",
        "comptime_float", "comptime_int", "f128", "f16", "f32", "f64", "f80", "false", "isize", "noreturn",
        "null", "true", "type", "undefined", "usize", "void"};
    for (const std::string_view n : kNames)
        if (n == name) return true;
    if (name.size() < 2) return false;
    if (name[0] != 'i' && name[0] != 'u') return false;
    for (const char c : name.substr(1))
        if (c < '0' || c > '9') return false;
    return true;
}

/// std.zig.isValidId.
bool isValidId(std::string_view bytes) {
    if (bytes.empty()) return false;
    for (std::size_t i = 0; i < bytes.size(); i++) {
        const char c = bytes[i];
        if (c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) continue;
        if (c >= '0' && c <= '9') {
            if (i == 0) return false;
            continue;
        }
        return false;
    }
    return !zig::getKeyword(bytes).has_value();
}

/// The longest primitive or keyword name ("comptime_float").
constexpr std::size_t longest_keyword_or_primitive_len = 14;

// ---- forward declarations -----------------------------------------------------

void renderMembers(Render &r, std::span<const NodeIndex> members);
void renderMember(Render &r, Container container, NodeIndex decl, Space space);
void renderExpressions(Render &r, std::span<const NodeIndex> expressions, Space space);
void renderExpression(Render &r, NodeIndex node, Space space);
bool rendersMultiline(const Render &r, NodeIndex node);
void renderArrayType(Render &r, const full::ArrayType &array_type, Space space);
void renderPtrType(Render &r, const full::PtrType &ptr_type, Space space);
void renderSlice(Render &r, NodeIndex slice_node, const full::Slice &slice, Space space);
void renderAsmOutput(Render &r, NodeIndex asm_output, Space space);
void renderAsmInput(Render &r, NodeIndex asm_input, Space space);
void renderVarDecl(Render &r, const full::VarDecl &var_decl, bool ignore_comptime_token, Space space);
void renderIf(Render &r, const full::If &if_node, Space space);
void renderWhile(Render &r, const full::While &while_node, Space space);
void renderThenElse(Render &r, TokenIndex last_prefix_token, NodeIndex then_expr, std::optional<TokenIndex> else_token,
                    std::optional<TokenIndex> maybe_error_token, OptionalIndex opt_else_expr, Space space);
void renderFor(Render &r, const full::For &for_node, Space space);
void renderContainerField(Render &r, Container container, const full::ContainerField &field_param, Space space);
void renderBuiltinCall(Render &r, TokenIndex builtin_token, std::span<const NodeIndex> params, Space space);
void renderFnProto(Render &r, const full::FnProto &fn_proto, Space space);
void renderSwitchCase(Render &r, const full::SwitchCase &switch_case, Space space);
void renderBlock(Render &r, NodeIndex block_node, std::span<const NodeIndex> statements, Space space);
void finishRenderBlock(Render &r, NodeIndex block_node, std::span<const NodeIndex> statements, Space space);
void renderStructInit(Render &r, NodeIndex struct_node, const full::StructInit &struct_init, Space space);
void renderArrayInit(Render &r, const full::ArrayInit &array_init, Space space);
void renderContainerDecl(Render &r, NodeIndex container_decl_node, const full::ContainerDecl &container_decl,
                         Space space);
void renderAsm(Render &r, const full::Asm &asm_node, Space space);
void renderCall(Render &r, const full::Call &call, Space space);
void renderParamList(Render &r, TokenIndex lparen, std::span<const NodeIndex> params, Space space);
void renderExpressionComma(Render &r, NodeIndex node, Space space);
void renderTokenComma(Render &r, TokenIndex token, Space space);
void renderIdentifierComma(Render &r, TokenIndex token, Space space, QuoteBehavior quote);
void renderToken(Render &r, TokenIndex token_index, Space space);
void renderTokenOverrideSpaceMode(Render &r, TokenIndex token_index, Space space, Space override_space);
void renderSpace(Render &r, TokenIndex token_index, std::size_t lexeme_len, Space space);
void renderIdentifier(Render &r, TokenIndex token_index, Space space, QuoteBehavior quote);
void renderQuotedIdentifier(Render &r, TokenIndex token_index, Space space, bool unquote);
void renderIdentifierContents(AutoIndentingStream &ais, std::string_view bytes);
bool hasComment(const Ast &tree, TokenIndex start_token, TokenIndex end_token);
bool hasMultilineString(const Ast &tree, TokenIndex start_token, TokenIndex end_token);
bool hasDocComment(const Ast &tree, TokenIndex start_token, TokenIndex end_token);
bool renderComments(Render &r, std::size_t start, std::size_t end);
void renderExtraNewline(Render &r, NodeIndex node);
void renderExtraNewlineToken(Render &r, TokenIndex token_index);
void renderDocComments(Render &r, TokenIndex end_token);
void renderContainerDocComments(Render &r, TokenIndex start_token);
std::string_view tokenSliceForRender(const Ast &tree, TokenIndex token_index);
bool hasTrailingComment(const Ast &tree, TokenIndex t);
bool anythingBetween(const Ast &tree, TokenIndex start_token, TokenIndex end_token);
void writeFixingWhitespace(Writer &w, std::string_view slice);
bool nodeIsBlock(NodeTag tag);
bool nodeIsIfForWhileSwitch(NodeTag tag);
bool nodeCausesSliceOpSpace(NodeTag tag);

// ---- the renderer ---------------------------------------------------------------

void renderTree(Writer &w, const Ast &tree) {
    AutoIndentingStream auto_indenting_stream(&w, indent_delta);
    Render r{&auto_indenting_stream, &tree};

    // Render all the line comments at the beginning of the file.
    const std::size_t comment_end_loc = tree.tokenStart(0);
    (void)renderComments(r, 0, comment_end_loc);

    if (tree.tokenTag(0) == TokenTag::container_doc_comment) {
        renderContainerDocComments(r, 0);
    }

    switch (tree.mode) {
        case zig::Mode::zig: renderMembers(r, tree.rootDecls()); break;
        case zig::Mode::zon: renderExpression(r, tree.rootDecls()[0], Space::newline); break;
    }

    if (auto_indenting_stream.disabled_offset) {
        writeFixingWhitespace(*auto_indenting_stream.underlying_writer,
                              std::string_view(tree.source).substr(*auto_indenting_stream.disabled_offset));
    }
}

/// Render all members in the given slice, keeping empty lines where appropriate
void renderMembers(Render &r, std::span<const NodeIndex> members) {
    const Ast &tree = *r.tree;
    if (members.empty()) return;
    Container container = Container::tuple;
    for (const NodeIndex member : members) {
        if (auto field = tree.fullContainerField(member)) {
            if (!field->ast.tuple_like) {
                container = Container::other;
                break;
            }
        }
    }
    renderMember(r, container, members[0], Space::newline);
    for (const NodeIndex member : members.subspan(1)) {
        renderExtraNewline(r, member);
        renderMember(r, container, member, Space::newline);
    }
}

/// Go back from a fn token to the first token a declaration renders
/// (`pub extern "c" inline fn`).
TokenIndex firstFnPrefixToken(const Ast &tree, TokenIndex fn_token) {
    TokenIndex i = fn_token;
    while (i > 0) {
        i -= 1;
        switch (tree.tokenTag(i)) {
            case TokenTag::keyword_extern:
            case TokenTag::keyword_export:
            case TokenTag::keyword_pub:
            case TokenTag::string_literal:
            case TokenTag::keyword_inline:
            case TokenTag::keyword_noinline: continue;
            default: i += 1; return i;
        }
    }
    return i;
}

void renderMember(Render &r, Container container, NodeIndex decl, Space space) {
    const Ast &tree = *r.tree;
    AutoIndentingStream &ais = *r.ais;
    renderDocComments(r, tree.firstToken(decl));
    switch (tree.nodeTag(decl)) {
        case NodeTag::fn_decl: {
            // Some examples:
            // pub extern "foo" fn ...
            // export fn ...
            const auto [fn_proto, body_node] = tree.nodeData(decl).node_and_node();
            const TokenIndex fn_token = tree.nodeMainToken(fn_proto);
            // Go back to the first token we should render here.
            for (TokenIndex i = firstFnPrefixToken(tree, fn_token); i < fn_token; i++) {
                renderToken(r, i, Space::space);
            }
            switch (tree.nodeTag(fn_proto)) {
                case NodeTag::fn_proto_one:
                case NodeTag::fn_proto: {
                    Ast::NodeBuffer1 buf{};
                    const OptionalIndex opt_callconv_expr = tree.nodeTag(fn_proto) == NodeTag::fn_proto_one
                                                                ? tree.fnProtoOne(buf, fn_proto).ast.callconv_expr
                                                                : tree.fnProto(fn_proto).ast.callconv_expr;

                    // Keep in sync with logic in `renderFnProto`. PROMOTE_CALLCONV_INLINE
                    if (opt_callconv_expr) {
                        const NodeIndex callconv_expr = opt_callconv_expr.unwrap();
                        if (tree.nodeTag(callconv_expr) == NodeTag::enum_literal) {
                            if (tree.tokenSlice(tree.nodeMainToken(callconv_expr)) == "@\"inline\"") {
                                ais.underlying_writer->writeAll("inline ");
                            }
                        }
                    }
                    break;
                }
                default: break;
            }
            renderExpression(r, fn_proto, Space::space);
            return renderExpression(r, body_node, space);
        }
        case NodeTag::fn_proto_simple:
        case NodeTag::fn_proto_multi:
        case NodeTag::fn_proto_one:
        case NodeTag::fn_proto: {
            // Extern function prototypes are parsed as these tags.
            // Go back to the first token we should render here.
            const TokenIndex fn_token = tree.nodeMainToken(decl);
            for (TokenIndex i = firstFnPrefixToken(tree, fn_token); i < fn_token; i++) {
                renderToken(r, i, Space::space);
            }
            renderExpression(r, decl, Space::none);
            return renderToken(r, tree.lastToken(decl) + 1, space);  // semicolon
        }

        case NodeTag::global_var_decl:
        case NodeTag::local_var_decl:
        case NodeTag::simple_var_decl:
        case NodeTag::aligned_var_decl:
            ais.pushSpace(Space::semicolon);
            renderVarDecl(r, *tree.fullVarDecl(decl), false, Space::semicolon);
            ais.popSpace();
            return;

        case NodeTag::test_decl: {
            const TokenIndex test_token = tree.nodeMainToken(decl);
            const auto [opt_name_token, block_node] = tree.nodeData(decl).opt_token_and_node();
            renderToken(r, test_token, Space::space);
            if (opt_name_token) {
                const TokenIndex name_token = opt_name_token.unwrap();
                switch (tree.tokenTag(name_token)) {
                    case TokenTag::string_literal: renderToken(r, name_token, Space::space); break;
                    case TokenTag::identifier:
                        renderIdentifier(r, name_token, Space::space, QuoteBehavior::preserve_when_shadowing);
                        break;
                    default: break;
                }
            }
            return renderExpression(r, block_node, space);
        }

        case NodeTag::container_field_init:
        case NodeTag::container_field_align:
        case NodeTag::container_field:
            return renderContainerField(r, container, *tree.fullContainerField(decl), space);

        case NodeTag::comptime_: return renderExpression(r, decl, space);

        default: return;
    }
}

/// Render all expressions in the slice, keeping empty lines where appropriate
void renderExpressions(Render &r, std::span<const NodeIndex> expressions, Space space) {
    if (expressions.empty()) return;
    renderExpression(r, expressions[0], space);
    for (const NodeIndex expression : expressions.subspan(1)) {
        renderExtraNewline(r, expression);
        renderExpression(r, expression, space);
    }
}

enum CastKind : std::uint8_t { ptrCast, alignCast, addrSpaceCast, constCast, volatileCast, kCastKinds };

std::optional<CastKind> castKind(std::string_view name) {
    if (name == "ptrCast") return ptrCast;
    if (name == "alignCast") return alignCast;
    if (name == "addrSpaceCast") return addrSpaceCast;
    if (name == "constCast") return constCast;
    if (name == "volatileCast") return volatileCast;
    return std::nullopt;
}

/// The builtin token to render for a cast builtin: nested cast builtins are
/// rendered in a canonical order (Render.zig's `canonicalize` block).
TokenIndex canonicalBuiltinToken(const Ast &tree, TokenIndex builtin_token, std::size_t params_len) {
    if (params_len != 1) return builtin_token;
    const auto kind = castKind(tree.tokenSlice(builtin_token).substr(1));
    if (!kind) return builtin_token;

    std::array<std::optional<TokenIndex>, kCastKinds> cast_map{};
    cast_map[*kind] = builtin_token;

    std::size_t casts_before = 0;
    TokenIndex prev_builtin_token = builtin_token;
    while (prev_builtin_token >= 2) {
        prev_builtin_token -= 2;
        if (tree.tokenTag(prev_builtin_token) != TokenTag::builtin) break;
        const auto prev_kind = castKind(tree.tokenSlice(prev_builtin_token).substr(1));
        if (!prev_kind) break;
        if (cast_map[*prev_kind]) return builtin_token;
        // This must be checked after so that cast builtins as arguments to other
        // builtins containing comments are reordered.
        if (hasComment(tree, prev_builtin_token, prev_builtin_token + 2)) return builtin_token;
        cast_map[*prev_kind] = prev_builtin_token;
        casts_before += 1;
    }

    TokenIndex next_builtin_token = builtin_token + 2;
    while (true) {
        if (hasComment(tree, next_builtin_token - 2, next_builtin_token)) return builtin_token;
        if (tree.tokenTag(next_builtin_token) != TokenTag::builtin) break;
        const auto next_kind = castKind(tree.tokenSlice(next_builtin_token).substr(1));
        if (!next_kind) break;
        if (cast_map[*next_kind]) return builtin_token;
        cast_map[*next_kind] = next_builtin_token;
        next_builtin_token += 2;
    }

    std::size_t skip = casts_before;
    for (const auto &entry : cast_map) {
        if (!entry) continue;
        if (skip == 0) return *entry;
        skip -= 1;
    }
    return builtin_token;
}

void renderExpression(Render &r, NodeIndex node, Space space) {
    const Ast &tree = *r.tree;
    AutoIndentingStream &ais = *r.ais;
    switch (tree.nodeTag(node)) {
        case NodeTag::identifier: {
            const TokenIndex token_index = tree.nodeMainToken(node);
            return renderIdentifier(r, token_index, space, QuoteBehavior::preserve_when_shadowing);
        }

        case NodeTag::number_literal:
        case NodeTag::char_literal:
        case NodeTag::unreachable_literal:
        case NodeTag::anyframe_literal:
        case NodeTag::string_literal: return renderToken(r, tree.nodeMainToken(node), space);

        case NodeTag::multiline_string_literal: {
            ais.maybeInsertNewline();

            const auto [first_tok, last_tok] = tree.nodeData(node).token_and_token();
            for (TokenIndex i = first_tok; i < last_tok; i++) {
                renderToken(r, i, Space::newline);
            }
            if (space != Space::skip) {
                renderToken(r, last_tok, Space::newline);
            } else {
                renderToken(r, last_tok, Space::skip);
                ais.insertNewline();  // A newline is part of the token, so it still needs rendered here.
            }

            const TokenIndex next_token = last_tok + 1;
            const TokenTag next_token_tag = tree.tokenTag(next_token);

            // dedent the next thing that comes after a multiline string literal
            if (next_token_tag != TokenTag::colon && !ais.indentStackEmpty() &&
                ais.lastSpaceModeIndent() < ais.currentIndent()) {
                auto &indent_top = ais.indent_stack.back();
                if (indent_top.realized) {
                    indent_top.realized = false;
                    ais.indent_count -= 1;
                }
            }

            switch (space) {
                case Space::none:
                case Space::space:
                case Space::newline:
                case Space::maybe_space:
                case Space::skip: break;
                case Space::semicolon:
                    if (next_token_tag == TokenTag::semicolon)
                        renderTokenOverrideSpaceMode(r, next_token, Space::newline, Space::semicolon);
                    break;
                case Space::comma:
                    if (next_token_tag == TokenTag::comma)
                        renderTokenOverrideSpaceMode(r, next_token, Space::newline, Space::comma);
                    break;
                case Space::comma_space:
                    if (next_token_tag == TokenTag::comma) renderToken(r, next_token, Space::space);
                    break;
                case Space::comma_maybe_space:
                    if (next_token_tag == TokenTag::comma) renderToken(r, next_token, Space::maybe_space);
                    break;
            }
            return;
        }

        case NodeTag::error_value: {
            const TokenIndex main_token = tree.nodeMainToken(node);
            renderToken(r, main_token, Space::none);
            renderToken(r, main_token + 1, Space::none);
            return renderIdentifier(r, main_token + 2, space, QuoteBehavior::eagerly_unquote);
        }

        case NodeTag::block_two:
        case NodeTag::block_two_semicolon:
        case NodeTag::block:
        case NodeTag::block_semicolon: {
            Ast::NodeBuffer2 buf{};
            const auto statements = *tree.blockStatements(buf, node);
            return renderBlock(r, node, statements, space);
        }

        case NodeTag::errdefer_: {
            const TokenIndex defer_token = tree.nodeMainToken(node);
            const auto [maybe_payload_token, expr] = tree.nodeData(node).opt_token_and_node();

            renderToken(r, defer_token, Space::maybe_space);
            if (maybe_payload_token) {
                const TokenIndex payload_token = maybe_payload_token.unwrap();
                renderToken(r, payload_token - 1, Space::none);  // |
                renderIdentifier(r, payload_token, Space::none, QuoteBehavior::preserve_when_shadowing);
                renderToken(r, payload_token + 1, Space::maybe_space);  // |
            }
            return renderExpression(r, expr, space);
        }

        case NodeTag::defer_:
        case NodeTag::comptime_:
        case NodeTag::nosuspend_:
        case NodeTag::suspend_: {
            const TokenIndex main_token = tree.nodeMainToken(node);
            const NodeIndex item = tree.nodeData(node).node();
            renderToken(r, main_token, Space::maybe_space);
            return renderExpression(r, item, space);
        }

        case NodeTag::catch_: {
            const TokenIndex main_token = tree.nodeMainToken(node);
            const auto [lhs, rhs] = tree.nodeData(node).node_and_node();
            const TokenIndex fallback_first = tree.firstToken(rhs);

            const bool seperate_line = !tree.tokensOnSameLine(main_token, fallback_first) ||
                                       tree.tokenTag(fallback_first) == TokenTag::multiline_string_literal_line;
            const Space after_op_space = seperate_line ? Space::newline : Space::space;

            renderExpression(r, lhs, Space::space);  // target

            ais.pushIndent(IndentType::normal);
            if (tree.tokenTag(fallback_first - 1) == TokenTag::pipe) {
                renderToken(r, main_token, Space::space);     // catch keyword
                renderToken(r, main_token + 1, Space::none);  // pipe
                renderIdentifier(r, main_token + 2, Space::none, QuoteBehavior::preserve_when_shadowing);
                renderToken(r, main_token + 3, after_op_space);  // pipe
            } else {
                renderToken(r, main_token, after_op_space);  // catch keyword
            }
            renderExpression(r, rhs, space);  // fallback
            ais.popIndent();
            return;
        }

        case NodeTag::field_access: {
            const auto [lhs, name_token] = tree.nodeData(node).node_and_token();
            const TokenIndex dot_token = name_token - 1;

            ais.pushIndent(IndentType::field_access);

            const TokenIndex lhs_last_token = tree.lastToken(lhs);
            const bool same_line = tree.tokensOnSameLine(lhs_last_token, name_token);
            // Keeping a space after the number ensures it will not turn into a decimal number
            // (e.g. "0xF .A").
            const Space number_space =
                (tree.tokenTag(lhs_last_token) == TokenTag::number_literal && same_line) ? Space::space : Space::none;
            renderExpression(r, lhs, number_space);

            // Allow a line break between the lhs and the dot if the lhs and rhs
            // are on different lines.
            if (!same_line && !hasComment(tree, lhs_last_token, dot_token)) ais.insertNewline();

            renderToken(r, dot_token, Space::none);

            renderIdentifier(r, name_token, space, QuoteBehavior::eagerly_unquote);  // field
            ais.popIndent();
            return;
        }

        case NodeTag::error_union:
        case NodeTag::switch_range: {
            const auto [lhs, rhs] = tree.nodeData(node).node_and_node();
            renderExpression(r, lhs, Space::none);
            renderToken(r, tree.nodeMainToken(node), Space::none);
            return renderExpression(r, rhs, space);
        }
        case NodeTag::for_range: {
            const auto [start, opt_end] = tree.nodeData(node).node_and_opt_node();
            renderExpression(r, start, Space::none);
            if (opt_end) {
                renderToken(r, tree.nodeMainToken(node), Space::none);
                return renderExpression(r, opt_end.unwrap(), space);
            } else {
                return renderToken(r, tree.nodeMainToken(node), space);
            }
        }

        case NodeTag::assign:
        case NodeTag::assign_bit_and:
        case NodeTag::assign_bit_or:
        case NodeTag::assign_shl:
        case NodeTag::assign_shl_sat:
        case NodeTag::assign_shr:
        case NodeTag::assign_bit_xor:
        case NodeTag::assign_div:
        case NodeTag::assign_sub:
        case NodeTag::assign_sub_wrap:
        case NodeTag::assign_sub_sat:
        case NodeTag::assign_mod:
        case NodeTag::assign_add:
        case NodeTag::assign_add_wrap:
        case NodeTag::assign_add_sat:
        case NodeTag::assign_mul:
        case NodeTag::assign_mul_wrap:
        case NodeTag::assign_mul_sat: {
            const auto [lhs, rhs] = tree.nodeData(node).node_and_node();
            renderExpression(r, lhs, Space::space);
            const TokenIndex op_token = tree.nodeMainToken(node);
            ais.pushIndent(IndentType::after_equals);
            const bool rhs_seperate_line = !tree.tokensOnSameLine(op_token, op_token + 1) ||
                                           tree.tokenTag(op_token + 1) == TokenTag::multiline_string_literal_line;
            renderToken(r, op_token, rhs_seperate_line ? Space::newline : Space::space);
            renderExpression(r, rhs, space);
            ais.popIndent();
            return;
        }

        case NodeTag::add:
        case NodeTag::add_wrap:
        case NodeTag::add_sat:
        case NodeTag::array_cat:
        case NodeTag::array_mult:
        case NodeTag::bang_equal:
        case NodeTag::bit_and:
        case NodeTag::bit_or:
        case NodeTag::shl:
        case NodeTag::shl_sat:
        case NodeTag::shr:
        case NodeTag::bit_xor:
        case NodeTag::bool_and:
        case NodeTag::bool_or:
        case NodeTag::div:
        case NodeTag::equal_equal:
        case NodeTag::greater_or_equal:
        case NodeTag::greater_than:
        case NodeTag::less_or_equal:
        case NodeTag::less_than:
        case NodeTag::merge_error_sets:
        case NodeTag::mod:
        case NodeTag::mul:
        case NodeTag::mul_wrap:
        case NodeTag::mul_sat:
        case NodeTag::sub:
        case NodeTag::sub_wrap:
        case NodeTag::sub_sat:
        case NodeTag::orelse_: {
            const auto [lhs, rhs] = tree.nodeData(node).node_and_node();
            renderExpression(r, lhs, Space::space);
            const TokenIndex op_token = tree.nodeMainToken(node);
            ais.pushIndent(IndentType::binop);
            const bool rhs_seperate_line = !tree.tokensOnSameLine(op_token, op_token + 1) ||
                                           tree.tokenTag(op_token + 1) == TokenTag::multiline_string_literal_line;
            renderToken(r, op_token, rhs_seperate_line ? Space::newline : Space::space);
            renderExpression(r, rhs, space);
            ais.popIndent();
            return;
        }

        case NodeTag::assign_destructure: {
            const full::AssignDestructure full_ = tree.assignDestructure(node);
            if (full_.comptime_token) renderToken(r, *full_.comptime_token, Space::maybe_space);

            for (std::size_t i = 0; i < full_.ast.variables.size(); i++) {
                const NodeIndex variable_node = full_.ast.variables[i];
                const Space variable_space =
                    i == full_.ast.variables.size() - 1 ? Space::maybe_space : Space::comma_maybe_space;
                switch (tree.nodeTag(variable_node)) {
                    case NodeTag::global_var_decl:
                    case NodeTag::local_var_decl:
                    case NodeTag::simple_var_decl:
                    case NodeTag::aligned_var_decl:
                        renderVarDecl(r, *tree.fullVarDecl(variable_node), true, variable_space);
                        break;
                    default: renderExpression(r, variable_node, variable_space); break;
                }
            }
            ais.pushIndent(IndentType::after_equals);
            const bool expr_seperate_line =
                !tree.tokensOnSameLine(full_.ast.equal_token, full_.ast.equal_token + 1) ||
                tree.tokenTag(full_.ast.equal_token + 1) == TokenTag::multiline_string_literal_line;
            renderToken(r, full_.ast.equal_token, expr_seperate_line ? Space::newline : Space::space);
            renderExpression(r, full_.ast.value_expr, space);
            ais.popIndent();
            return;
        }

        case NodeTag::bit_not:
        case NodeTag::bool_not:
        case NodeTag::negation:
        case NodeTag::negation_wrap:
        case NodeTag::optional_type:
        case NodeTag::address_of:
            renderToken(r, tree.nodeMainToken(node), Space::none);
            return renderExpression(r, tree.nodeData(node).node(), space);

        case NodeTag::try_:
        case NodeTag::resume_:
            renderToken(r, tree.nodeMainToken(node), Space::maybe_space);
            return renderExpression(r, tree.nodeData(node).node(), space);

        case NodeTag::array_type:
        case NodeTag::array_type_sentinel: return renderArrayType(r, *tree.fullArrayType(node), space);

        case NodeTag::ptr_type_aligned:
        case NodeTag::ptr_type_sentinel:
        case NodeTag::ptr_type:
        case NodeTag::ptr_type_bit_range: return renderPtrType(r, *tree.fullPtrType(node), space);

        case NodeTag::array_init_one:
        case NodeTag::array_init_one_comma:
        case NodeTag::array_init_dot_two:
        case NodeTag::array_init_dot_two_comma:
        case NodeTag::array_init_dot:
        case NodeTag::array_init_dot_comma:
        case NodeTag::array_init:
        case NodeTag::array_init_comma: {
            Ast::NodeBuffer2 elements{};
            return renderArrayInit(r, *tree.fullArrayInit(elements, node), space);
        }

        case NodeTag::struct_init_one:
        case NodeTag::struct_init_one_comma:
        case NodeTag::struct_init_dot_two:
        case NodeTag::struct_init_dot_two_comma:
        case NodeTag::struct_init_dot:
        case NodeTag::struct_init_dot_comma:
        case NodeTag::struct_init:
        case NodeTag::struct_init_comma: {
            Ast::NodeBuffer2 buf{};
            return renderStructInit(r, node, *tree.fullStructInit(buf, node), space);
        }

        case NodeTag::call_one:
        case NodeTag::call_one_comma:
        case NodeTag::call:
        case NodeTag::call_comma: {
            Ast::NodeBuffer1 buf{};
            return renderCall(r, *tree.fullCall(buf, node), space);
        }

        case NodeTag::array_access: {
            const auto [lhs, rhs] = tree.nodeData(node).node_and_node();
            const TokenIndex lbracket = tree.firstToken(rhs) - 1;
            const TokenIndex rbracket = tree.lastToken(rhs) + 1;
            renderExpression(r, lhs, Space::none);
            // One line check must come after rendering lhs since it can influence
            // isLineOverIndented
            const bool one_line = tree.tokensOnSameLine(lbracket, rbracket) && !rendersMultiline(r, rhs);
            const Space inner_space = one_line ? Space::none : Space::newline;
            ais.pushIndent(IndentType::normal);
            renderToken(r, lbracket, inner_space);  // [
            renderExpression(r, rhs, inner_space);
            ais.popIndent();
            return renderToken(r, rbracket, space);  // ]
        }

        case NodeTag::slice_open:
        case NodeTag::slice:
        case NodeTag::slice_sentinel: return renderSlice(r, node, *tree.fullSlice(node), space);

        case NodeTag::deref:
            renderExpression(r, tree.nodeData(node).node(), Space::none);
            return renderToken(r, tree.nodeMainToken(node), space);

        case NodeTag::unwrap_optional: {
            const auto [lhs, question_mark] = tree.nodeData(node).node_and_token();
            const TokenIndex dot_token = question_mark - 1;
            renderExpression(r, lhs, Space::none);
            renderToken(r, dot_token, Space::none);
            return renderToken(r, question_mark, space);
        }

        case NodeTag::break_:
        case NodeTag::continue_: {
            const TokenIndex main_token = tree.nodeMainToken(node);
            const auto [opt_label_token, opt_target] = tree.nodeData(node).opt_token_and_opt_node();

            const Space before_target_space = opt_target ? Space::maybe_space : space;
            const Space before_label_space = opt_label_token ? Space::space : before_target_space;

            renderToken(r, main_token, before_label_space);
            if (opt_label_token) {
                const TokenIndex label_token = opt_label_token.unwrap();
                renderToken(r, label_token - 1, Space::none);  // :
                renderIdentifier(r, label_token, before_target_space, QuoteBehavior::eagerly_unquote);
            }
            if (opt_target) renderExpression(r, opt_target.unwrap(), space);
            return;
        }

        case NodeTag::return_: {
            const OptionalIndex expr = tree.nodeData(node).opt_node();
            if (expr) {
                renderToken(r, tree.nodeMainToken(node), Space::maybe_space);
                renderExpression(r, expr.unwrap(), space);
            } else {
                renderToken(r, tree.nodeMainToken(node), space);
            }
            return;
        }

        case NodeTag::grouped_expression: {
            const auto [expr, rparen] = tree.nodeData(node).node_and_token();
            ais.pushIndent(IndentType::normal);
            renderToken(r, tree.nodeMainToken(node), Space::none);  // lparen
            renderExpression(r, expr, Space::none);
            ais.popIndent();
            return renderToken(r, rparen, space);
        }

        case NodeTag::container_decl:
        case NodeTag::container_decl_trailing:
        case NodeTag::container_decl_arg:
        case NodeTag::container_decl_arg_trailing:
        case NodeTag::container_decl_two:
        case NodeTag::container_decl_two_trailing:
        case NodeTag::tagged_union:
        case NodeTag::tagged_union_trailing:
        case NodeTag::tagged_union_enum_tag:
        case NodeTag::tagged_union_enum_tag_trailing:
        case NodeTag::tagged_union_two:
        case NodeTag::tagged_union_two_trailing: {
            Ast::NodeBuffer2 buf{};
            return renderContainerDecl(r, node, *tree.fullContainerDecl(buf, node), space);
        }

        case NodeTag::error_set_decl: {
            const TokenIndex error_token = tree.nodeMainToken(node);
            const auto [lbrace, rbrace] = tree.nodeData(node).token_and_token();

            renderToken(r, error_token, Space::none);

            if (lbrace + 1 == rbrace) {
                // There is nothing between the braces so render condensed: `error{}`
                renderToken(r, lbrace, Space::none);
                return renderToken(r, rbrace, space);
            } else if (lbrace + 2 == rbrace && tree.tokenTag(lbrace + 1) == TokenTag::identifier) {
                // There is exactly one member and no trailing comma or
                // comments, so render without surrounding spaces: `error{Foo}`
                renderToken(r, lbrace, Space::none);
                renderIdentifier(r, lbrace + 1, Space::none, QuoteBehavior::eagerly_unquote);
                return renderToken(r, rbrace, space);
            } else if (!(tree.tokenTag(rbrace - 1) != TokenTag::comma && !hasDocComment(tree, lbrace + 1, rbrace) &&
                         !hasComment(tree, lbrace, rbrace))) {
                // Render each member on a new line.
                ais.pushIndent(IndentType::normal);
                renderToken(r, lbrace, Space::newline);
                for (TokenIndex i = lbrace + 1; i < rbrace; i++) {
                    const TokenTag tag = tree.tokenTag(i);
                    if (tag == TokenTag::comma) continue;
                    if (i > lbrace + 1) renderExtraNewlineToken(r, i);
                    switch (tag) {
                        case TokenTag::doc_comment: renderToken(r, i, Space::newline); break;
                        case TokenTag::identifier:
                            ais.pushSpace(Space::comma);
                            renderIdentifier(r, i, Space::comma, QuoteBehavior::eagerly_unquote);
                            ais.popSpace();
                            break;
                        default: break;
                    }
                }
                ais.popIndent();
                return renderToken(r, rbrace, space);
            } else {
                // Render each member on one line.
                renderToken(r, lbrace, Space::space);
                for (TokenIndex i = lbrace + 1; i < rbrace; i++) {
                    if (tree.tokenTag(i) == TokenTag::identifier)
                        renderIdentifier(r, i, Space::comma_space, QuoteBehavior::eagerly_unquote);
                }
                return renderToken(r, rbrace, space);
            }
        }

        case NodeTag::builtin_call_two:
        case NodeTag::builtin_call_two_comma:
        case NodeTag::builtin_call:
        case NodeTag::builtin_call_comma: {
            Ast::NodeBuffer2 buf{};
            const auto params = *tree.builtinCallParams(buf, node);
            const TokenIndex builtin_token = canonicalBuiltinToken(tree, tree.nodeMainToken(node), params.size());
            return renderBuiltinCall(r, builtin_token, params, space);
        }

        case NodeTag::fn_proto_simple:
        case NodeTag::fn_proto_multi:
        case NodeTag::fn_proto_one:
        case NodeTag::fn_proto: {
            Ast::NodeBuffer1 buf{};
            return renderFnProto(r, *tree.fullFnProto(buf, node), space);
        }

        case NodeTag::anyframe_type: {
            const TokenIndex main_token = tree.nodeMainToken(node);
            renderToken(r, main_token, Space::none);      // anyframe
            renderToken(r, main_token + 1, Space::none);  // ->
            return renderExpression(r, tree.nodeData(node).token_and_node().second, space);
        }

        case NodeTag::switch_:
        case NodeTag::switch_comma: {
            const full::Switch full_ = tree.switchFull(node);

            if (full_.label_token) {
                renderIdentifier(r, *full_.label_token, Space::none, QuoteBehavior::eagerly_unquote);  // label
                renderToken(r, *full_.label_token + 1, Space::space);                                 // :
            }

            const TokenIndex rparen = tree.lastToken(full_.ast.condition) + 1;

            renderToken(r, full_.ast.switch_token, Space::space);     // switch
            renderToken(r, full_.ast.switch_token + 1, Space::none);  // (
            renderExpression(r, full_.ast.condition, Space::none);    // condition expression
            renderToken(r, rparen, Space::space);                     // )

            ais.pushIndent(IndentType::normal);
            if (full_.ast.cases.empty()) {
                renderToken(r, rparen + 1, Space::none);  // {
            } else {
                renderToken(r, rparen + 1, Space::newline);  // {
                ais.pushSpace(Space::comma);
                renderExpressions(r, full_.ast.cases, Space::comma);
                ais.popSpace();
            }
            ais.popIndent();
            return renderToken(r, tree.lastToken(node), space);  // }
        }

        case NodeTag::switch_case_one:
        case NodeTag::switch_case_inline_one:
        case NodeTag::switch_case:
        case NodeTag::switch_case_inline: return renderSwitchCase(r, *tree.fullSwitchCase(node), space);

        case NodeTag::while_simple:
        case NodeTag::while_cont:
        case NodeTag::while_: return renderWhile(r, *tree.fullWhile(node), space);

        case NodeTag::for_simple:
        case NodeTag::for_: return renderFor(r, *tree.fullFor(node), space);

        case NodeTag::if_simple:
        case NodeTag::if_: return renderIf(r, *tree.fullIf(node), space);

        case NodeTag::asm_simple:
        case NodeTag::asm_: return renderAsm(r, *tree.fullAsm(node), space);

        case NodeTag::enum_literal:
            renderToken(r, tree.nodeMainToken(node) - 1, Space::none);  // .
            return renderIdentifier(r, tree.nodeMainToken(node), space, QuoteBehavior::eagerly_unquote);  // name

        default: return;  // unreachable in Zig
    }
}

bool rendersMultiline(const Render &r, NodeIndex node) {
    if (r.ais->disabled_offset) return true;
    Writer no_nl_w;
    no_nl_w.no_newline = true;
    AutoIndentingStream sub_ais(&no_nl_w, r.ais->indent_delta_);
    // The following are needed to make sure isLineOverIndented is correct
    sub_ais.indent_count = r.ais->indent_count;
    sub_ais.applied_indent = r.ais->applied_indent;
    sub_ais.current_line_empty = r.ais->current_line_empty;

    Render sub_r{&sub_ais, r.tree};
    try {
        renderExpression(sub_r, node, Space::none);
    } catch (const WriteFailed &) {
        return true;
    }
    if (sub_ais.disabled_offset) return true;
    return false;
}

void renderArrayType(Render &r, const full::ArrayType &array_type, Space space) {
    const Ast &tree = *r.tree;
    AutoIndentingStream &ais = *r.ais;
    const TokenIndex rbracket = tree.firstToken(array_type.ast.elem_type) - 1;
    bool one_line = tree.tokensOnSameLine(array_type.ast.lbracket, rbracket) &&
                    !rendersMultiline(r, array_type.ast.elem_count);
    if (one_line && array_type.ast.sentinel) one_line = !rendersMultiline(r, array_type.ast.sentinel.unwrap());
    const Space inner_space = one_line ? Space::none : Space::newline;
    ais.pushIndent(IndentType::normal);
    renderToken(r, array_type.ast.lbracket, inner_space);  // lbracket
    renderExpression(r, array_type.ast.elem_count, inner_space);
    if (array_type.ast.sentinel) {
        const NodeIndex sentinel = array_type.ast.sentinel.unwrap();
        renderToken(r, tree.firstToken(sentinel) - 1, inner_space);  // colon
        renderExpression(r, sentinel, inner_space);
    }
    ais.popIndent();
    renderToken(r, rbracket, Space::none);  // rbracket
    return renderExpression(r, array_type.ast.elem_type, space);
}

void renderPtrType(Render &r, const full::PtrType &ptr_type, Space space) {
    const Ast &tree = *r.tree;
    const TokenIndex main_token = ptr_type.ast.main_token;

    switch (ptr_type.size) {
        case full::PtrSize::one:
            // Since ** tokens exist and the same token is shared by two
            // nested pointer types, we check to see if we are the parent
            // in such a relationship. If so, skip rendering anything for
            // this pointer type and rely on the child to render our asterisk
            // as well when it renders the ** token.
            if (tree.tokenTag(main_token) == TokenTag::asterisk_asterisk &&
                main_token == tree.nodeMainToken(ptr_type.ast.child_type)) {
                return renderExpression(r, ptr_type.ast.child_type, space);
            }
            renderToken(r, main_token, Space::none);  // asterisk
            break;
        case full::PtrSize::many:
            if (ptr_type.ast.sentinel) {
                const NodeIndex sentinel = ptr_type.ast.sentinel.unwrap();
                renderToken(r, main_token, Space::none);      // lbracket
                renderToken(r, main_token + 1, Space::none);  // asterisk
                renderToken(r, main_token + 2, Space::none);  // colon
                renderExpression(r, sentinel, Space::none);
                renderToken(r, tree.lastToken(sentinel) + 1, Space::none);  // rbracket
            } else {
                renderToken(r, main_token, Space::none);      // lbracket
                renderToken(r, main_token + 1, Space::none);  // asterisk
                renderToken(r, main_token + 2, Space::none);  // rbracket
            }
            break;
        case full::PtrSize::c:
            renderToken(r, main_token, Space::none);      // lbracket
            renderToken(r, main_token + 1, Space::none);  // asterisk
            renderToken(r, main_token + 2, Space::none);  // c
            renderToken(r, main_token + 3, Space::none);  // rbracket
            break;
        case full::PtrSize::slice:
            if (ptr_type.ast.sentinel) {
                const NodeIndex sentinel = ptr_type.ast.sentinel.unwrap();
                renderToken(r, main_token, Space::none);      // lbracket
                renderToken(r, main_token + 1, Space::none);  // colon
                renderExpression(r, sentinel, Space::none);
                renderToken(r, tree.lastToken(sentinel) + 1, Space::none);  // rbracket
            } else {
                renderToken(r, main_token, Space::none);      // lbracket
                renderToken(r, main_token + 1, Space::none);  // rbracket
            }
            break;
    }

    // .maybe_space cannot be used at the end of each qualifier since they may be reordered
    enum class Qual { volatile_, const_, addrspace_, align_, allowzero_, none };
    const Qual final_qual = ptr_type.volatile_token          ? Qual::volatile_
                            : ptr_type.const_token           ? Qual::const_
                            : ptr_type.ast.addrspace_node    ? Qual::addrspace_
                            : ptr_type.ast.align_node        ? Qual::align_
                            : ptr_type.allowzero_token       ? Qual::allowzero_
                                                             : Qual::none;
    const Space final_qual_space =
        tree.tokenTag(tree.firstToken(ptr_type.ast.child_type)) != TokenTag::multiline_string_literal_line
            ? Space::space
            : Space::none;

    if (ptr_type.allowzero_token) {
        const Space this_space = final_qual == Qual::allowzero_ ? final_qual_space : Space::space;
        renderToken(r, *ptr_type.allowzero_token, this_space);
    }

    if (ptr_type.ast.align_node) {
        const NodeIndex align_node = ptr_type.ast.align_node.unwrap();
        const Space this_space = final_qual == Qual::align_ ? final_qual_space : Space::space;
        const TokenIndex align_first = tree.firstToken(align_node);
        renderToken(r, align_first - 2, Space::none);  // align
        renderToken(r, align_first - 1, Space::none);  // lparen
        renderExpression(r, align_node, Space::none);
        if (ptr_type.ast.bit_range_start) {
            const NodeIndex bit_range_start = ptr_type.ast.bit_range_start.unwrap();
            const NodeIndex bit_range_end = ptr_type.ast.bit_range_end.unwrap();
            renderToken(r, tree.firstToken(bit_range_start) - 1, Space::none);  // colon
            renderExpression(r, bit_range_start, Space::none);
            renderToken(r, tree.firstToken(bit_range_end) - 1, Space::none);  // colon
            renderExpression(r, bit_range_end, Space::none);
            renderToken(r, tree.lastToken(bit_range_end) + 1, this_space);  // rparen
        } else {
            renderToken(r, tree.lastToken(align_node) + 1, this_space);  // rparen
        }
    }

    if (ptr_type.ast.addrspace_node) {
        const NodeIndex addrspace_node = ptr_type.ast.addrspace_node.unwrap();
        const Space this_space = final_qual == Qual::addrspace_ ? final_qual_space : Space::space;
        const TokenIndex addrspace_first = tree.firstToken(addrspace_node);
        renderToken(r, addrspace_first - 2, Space::none);  // addrspace
        renderToken(r, addrspace_first - 1, Space::none);  // lparen
        renderExpression(r, addrspace_node, Space::none);
        renderToken(r, tree.lastToken(addrspace_node) + 1, this_space);  // rparen
    }

    if (ptr_type.const_token) {
        const Space this_space = final_qual == Qual::const_ ? final_qual_space : Space::space;
        renderToken(r, *ptr_type.const_token, this_space);
    }

    if (ptr_type.volatile_token) {
        renderToken(r, *ptr_type.volatile_token, final_qual_space);
    }

    return renderExpression(r, ptr_type.ast.child_type, space);
}

void renderSlice(Render &r, NodeIndex slice_node, const full::Slice &slice, Space space) {
    const Ast &tree = *r.tree;
    const bool space_around_dots = nodeCausesSliceOpSpace(tree.nodeTag(slice.ast.start)) ||
                                   (slice.ast.end && nodeCausesSliceOpSpace(tree.nodeTag(slice.ast.end.unwrap())));
    const Space after_start_space = space_around_dots ? Space::space : Space::none;
    const Space before_sentinel_space = slice.ast.sentinel ? Space::space : Space::none;
    const Space after_dots_space =
        slice.ast.end ? (space_around_dots ? Space::maybe_space : Space::none) : before_sentinel_space;

    renderExpression(r, slice.ast.sliced, Space::none);
    renderToken(r, slice.ast.lbracket, Space::none);  // lbracket

    const TokenIndex start_last = tree.lastToken(slice.ast.start);
    renderExpression(r, slice.ast.start, after_start_space);
    renderToken(r, start_last + 1, after_dots_space);  // ellipsis2 ("..")

    if (slice.ast.end) renderExpression(r, slice.ast.end.unwrap(), before_sentinel_space);

    if (slice.ast.sentinel) {
        const NodeIndex sentinel = slice.ast.sentinel.unwrap();
        renderToken(r, tree.firstToken(sentinel) - 1, Space::none);  // colon
        renderExpression(r, sentinel, Space::none);
    }

    return renderToken(r, tree.lastToken(slice_node), space);  // rbracket
}

void renderAsmOutput(Render &r, NodeIndex asm_output, Space space) {
    const Ast &tree = *r.tree;
    const TokenIndex symbolic_name = tree.nodeMainToken(asm_output);

    renderToken(r, symbolic_name - 1, Space::none);                                         // lbracket
    renderIdentifier(r, symbolic_name, Space::none, QuoteBehavior::eagerly_unquote);         // ident
    renderToken(r, symbolic_name + 1, Space::space);                                        // rbracket
    renderToken(r, symbolic_name + 2, Space::space);                                        // "constraint"
    renderToken(r, symbolic_name + 3, Space::none);                                         // lparen

    if (tree.tokenTag(symbolic_name + 4) == TokenTag::arrow) {
        const auto [type_expr, rparen] = tree.nodeData(asm_output).opt_node_and_token();
        renderToken(r, symbolic_name + 4, Space::maybe_space);  // ->
        renderExpression(r, type_expr.unwrap(), Space::none);
        return renderToken(r, rparen, space);
    } else {
        renderIdentifier(r, symbolic_name + 4, Space::none, QuoteBehavior::eagerly_unquote);  // ident
        return renderToken(r, symbolic_name + 5, space);                                     // rparen
    }
}

void renderAsmInput(Render &r, NodeIndex asm_input, Space space) {
    const Ast &tree = *r.tree;
    const TokenIndex symbolic_name = tree.nodeMainToken(asm_input);
    const auto [expr, rparen] = tree.nodeData(asm_input).node_and_token();

    renderToken(r, symbolic_name - 1, Space::none);                                  // lbracket
    renderIdentifier(r, symbolic_name, Space::none, QuoteBehavior::eagerly_unquote);  // ident
    renderToken(r, symbolic_name + 1, Space::space);                                 // rbracket
    renderToken(r, symbolic_name + 2, Space::space);                                 // "constraint"
    renderToken(r, symbolic_name + 3, Space::none);                                  // lparen
    renderExpression(r, expr, Space::none);
    return renderToken(r, rparen, space);
}

void renderVarDecl(Render &r, const full::VarDecl &var_decl, bool ignore_comptime_token, Space space) {
    const Ast &tree = *r.tree;
    AutoIndentingStream &ais = *r.ais;

    if (var_decl.visib_token) renderToken(r, *var_decl.visib_token, Space::space);  // pub

    if (var_decl.extern_export_token) {
        renderToken(r, *var_decl.extern_export_token, Space::space);  // extern
        if (var_decl.lib_name) renderToken(r, *var_decl.lib_name, Space::space);  // "lib"
    }

    if (var_decl.threadlocal_token) renderToken(r, *var_decl.threadlocal_token, Space::space);  // threadlocal

    if (!ignore_comptime_token) {
        if (var_decl.comptime_token) renderToken(r, *var_decl.comptime_token, Space::space);  // comptime
    }

    renderToken(r, var_decl.ast.mut_token, Space::space);  // var

    enum class Last { value, linksection_, addrspace_, align_, type, identifier };
    const Last last_component = var_decl.ast.init_node        ? Last::value
                                : var_decl.ast.section_node   ? Last::linksection_
                                : var_decl.ast.addrspace_node ? Last::addrspace_
                                : var_decl.ast.align_node     ? Last::align_
                                : var_decl.ast.type_node      ? Last::type
                                                              : Last::identifier;

    if (last_component == Last::identifier) {
        return renderIdentifier(r, var_decl.ast.mut_token + 1, space, QuoteBehavior::preserve_when_shadowing);
    }
    const Space after_ident_space = var_decl.ast.type_node ? Space::none : Space::space;
    renderIdentifier(r, var_decl.ast.mut_token + 1, after_ident_space, QuoteBehavior::preserve_when_shadowing);

    if (var_decl.ast.type_node) {
        const NodeIndex type_node = var_decl.ast.type_node.unwrap();
        renderToken(r, var_decl.ast.mut_token + 2, Space::maybe_space);  // :
        if (last_component == Last::type) return renderExpression(r, type_node, space);
        renderExpression(r, type_node, Space::space);
    }

    if (var_decl.ast.align_node) {
        const NodeIndex align_node = var_decl.ast.align_node.unwrap();
        const TokenIndex lparen = tree.firstToken(align_node) - 1;
        const TokenIndex align_kw = lparen - 1;
        const TokenIndex rparen = tree.lastToken(align_node) + 1;
        renderToken(r, align_kw, Space::none);  // align
        renderToken(r, lparen, Space::none);    // (
        renderExpression(r, align_node, Space::none);
        if (last_component == Last::align_) return renderToken(r, rparen, space);  // )
        renderToken(r, rparen, Space::space);                                       // )
    }

    if (var_decl.ast.addrspace_node) {
        const NodeIndex addrspace_node = var_decl.ast.addrspace_node.unwrap();
        const TokenIndex lparen = tree.firstToken(addrspace_node) - 1;
        const TokenIndex addrspace_kw = lparen - 1;
        const TokenIndex rparen = tree.lastToken(addrspace_node) + 1;
        renderToken(r, addrspace_kw, Space::none);  // addrspace
        renderToken(r, lparen, Space::none);        // (
        renderExpression(r, addrspace_node, Space::none);
        if (last_component == Last::addrspace_) return renderToken(r, rparen, space);  // )
        renderToken(r, rparen, Space::space);                                           // )
    }

    if (var_decl.ast.section_node) {
        const NodeIndex section_node = var_decl.ast.section_node.unwrap();
        const TokenIndex lparen = tree.firstToken(section_node) - 1;
        const TokenIndex section_kw = lparen - 1;
        const TokenIndex rparen = tree.lastToken(section_node) + 1;
        renderToken(r, section_kw, Space::none);  // linksection
        renderToken(r, lparen, Space::none);      // (
        renderExpression(r, section_node, Space::none);
        if (last_component == Last::linksection_) return renderToken(r, rparen, space);  // )
        renderToken(r, rparen, Space::space);                                             // )
    }

    const NodeIndex init_node = var_decl.ast.init_node.unwrap();

    const TokenIndex eq_token = tree.firstToken(init_node) - 1;
    const bool rhs_seperate_line = !tree.tokensOnSameLine(eq_token, eq_token + 1) ||
                                   tree.tokenTag(eq_token + 1) == TokenTag::multiline_string_literal_line;
    const Space eq_space = rhs_seperate_line ? Space::newline : Space::space;
    ais.pushIndent(IndentType::after_equals);
    renderToken(r, eq_token, eq_space);    // =
    renderExpression(r, init_node, space);  // ;
    ais.popIndent();
}

void renderIf(Render &r, const full::If &if_node, Space space) {
    full::While w{};
    w.ast.while_token = if_node.ast.if_token;
    w.ast.cond_expr = if_node.ast.cond_expr;
    w.ast.cont_expr = OptionalIndex::none();
    w.ast.then_expr = if_node.ast.then_expr;
    w.ast.else_expr = if_node.ast.else_expr;
    w.inline_token = std::nullopt;
    w.label_token = std::nullopt;
    w.payload_token = if_node.payload_token;
    w.else_token = if_node.else_token;
    w.error_token = if_node.error_token;
    return renderWhile(r, w, space);
}

/// Note that this function is additionally used to render if expressions, with
/// respective values set to null.
void renderWhile(Render &r, const full::While &while_node, Space space) {
    const Ast &tree = *r.tree;

    if (while_node.label_token) {
        const TokenIndex label = *while_node.label_token;
        renderIdentifier(r, label, Space::none, QuoteBehavior::eagerly_unquote);  // label
        renderToken(r, label + 1, Space::space);                                  // :
    }

    if (while_node.inline_token) renderToken(r, *while_node.inline_token, Space::space);  // inline

    renderToken(r, while_node.ast.while_token, Space::space);     // if/for/while
    renderToken(r, while_node.ast.while_token + 1, Space::none);  // lparen
    renderExpression(r, while_node.ast.cond_expr, Space::none);   // condition

    TokenIndex last_prefix_token = tree.lastToken(while_node.ast.cond_expr) + 1;  // rparen

    if (while_node.payload_token) {
        const TokenIndex payload_token = *while_node.payload_token;
        renderToken(r, last_prefix_token, Space::space);
        renderToken(r, payload_token - 1, Space::none);  // |
        TokenIndex ident = payload_token;
        if (tree.tokenTag(payload_token) == TokenTag::asterisk) {
            renderToken(r, payload_token, Space::none);  // *
            ident = payload_token + 1;
        }
        renderIdentifier(r, ident, Space::none, QuoteBehavior::preserve_when_shadowing);  // identifier
        TokenIndex pipe;
        if (tree.tokenTag(ident + 1) == TokenTag::comma) {
            renderToken(r, ident + 1, Space::space);                                              // ,
            renderIdentifier(r, ident + 2, Space::none, QuoteBehavior::preserve_when_shadowing);  // index
            pipe = ident + 3;
        } else {
            pipe = ident + 1;
        }
        last_prefix_token = pipe;
    }

    if (while_node.ast.cont_expr) {
        const NodeIndex cont_expr = while_node.ast.cont_expr.unwrap();
        renderToken(r, last_prefix_token, Space::space);
        const TokenIndex lparen = tree.firstToken(cont_expr) - 1;
        renderToken(r, lparen - 1, Space::space);  // :
        renderToken(r, lparen, Space::none);       // lparen
        renderExpression(r, cont_expr, Space::none);
        last_prefix_token = tree.lastToken(cont_expr) + 1;  // rparen
    }

    renderThenElse(r, last_prefix_token, while_node.ast.then_expr, while_node.else_token, while_node.error_token,
                   while_node.ast.else_expr, space);
}

void renderThenElse(Render &r, TokenIndex last_prefix_token, NodeIndex then_expr, std::optional<TokenIndex> else_token,
                    std::optional<TokenIndex> maybe_error_token, OptionalIndex opt_else_expr, Space space) {
    const Ast &tree = *r.tree;
    AutoIndentingStream &ais = *r.ais;
    const bool then_expr_is_block = nodeIsBlock(tree.nodeTag(then_expr));
    const TokenIndex then_expr_first_token = tree.firstToken(then_expr);
    const bool indent_then_expr =
        !then_expr_is_block && (!tree.tokensOnSameLine(last_prefix_token, then_expr_first_token) ||
                                tree.tokenTag(then_expr_first_token) == TokenTag::multiline_string_literal_line);

    if (indent_then_expr) ais.pushIndent(IndentType::normal);

    if (then_expr_is_block && ais.isLineOverIndented()) {
        ais.disableIndentCommitting();
        renderToken(r, last_prefix_token, Space::newline);
        ais.enableIndentCommitting();
    } else if (indent_then_expr) {
        renderToken(r, last_prefix_token, Space::newline);
    } else {
        renderToken(r, last_prefix_token, Space::space);
    }

    if (opt_else_expr) {
        const NodeIndex else_expr = opt_else_expr.unwrap();
        if (indent_then_expr) {
            renderExpression(r, then_expr, Space::newline);
        } else {
            renderExpression(r, then_expr, Space::space);
        }

        if (indent_then_expr) ais.popIndent();

        TokenIndex last_else_token = *else_token;

        if (maybe_error_token) {
            const TokenIndex error_token = *maybe_error_token;
            renderToken(r, last_else_token, Space::space);  // else
            renderToken(r, error_token - 1, Space::none);   // |
            renderIdentifier(r, error_token, Space::none, QuoteBehavior::preserve_when_shadowing);  // identifier
            last_else_token = error_token + 1;                                                     // |
        }

        const bool indent_else_expr =
            (indent_then_expr && !nodeIsBlock(tree.nodeTag(else_expr)) &&
             !nodeIsIfForWhileSwitch(tree.nodeTag(else_expr))) ||
            tree.tokenTag(tree.firstToken(else_expr)) == TokenTag::multiline_string_literal_line;
        if (indent_else_expr) {
            ais.pushIndent(IndentType::normal);
            renderToken(r, last_else_token, Space::newline);
            renderExpression(r, else_expr, space);
            ais.popIndent();
        } else {
            renderToken(r, last_else_token, Space::space);
            renderExpression(r, else_expr, space);
        }
    } else {
        renderExpression(r, then_expr, space);
        if (indent_then_expr) ais.popIndent();
    }
}

void renderFor(Render &r, const full::For &for_node, Space space) {
    const Ast &tree = *r.tree;
    AutoIndentingStream &ais = *r.ais;

    if (for_node.label_token) {
        const TokenIndex label = *for_node.label_token;
        renderIdentifier(r, label, Space::none, QuoteBehavior::eagerly_unquote);  // label
        renderToken(r, label + 1, Space::space);                                  // :
    }

    if (for_node.inline_token) renderToken(r, *for_node.inline_token, Space::space);  // inline

    renderToken(r, for_node.ast.for_token, Space::space);  // if/for/while

    const TokenIndex lparen = for_node.ast.for_token + 1;
    renderParamList(r, lparen, for_node.ast.inputs, Space::space);

    TokenIndex cur = for_node.payload_token;
    TokenIndex pipe = cur;
    while (tree.tokenTag(pipe) != TokenTag::pipe) pipe++;
    const bool capture_trailing_comma = tree.tokenTag(pipe - 1) == TokenTag::comma;

    if (capture_trailing_comma) ais.pushIndent(IndentType::normal);
    renderToken(r, cur - 1, capture_trailing_comma ? Space::newline : Space::none);  // |
    while (true) {
        if (tree.tokenTag(cur) == TokenTag::asterisk) {
            renderToken(r, cur, Space::none);  // *
            cur += 1;
        }
        renderIdentifier(r, cur, Space::none, QuoteBehavior::preserve_when_shadowing);  // identifier
        cur += 1;
        if (tree.tokenTag(cur) == TokenTag::comma) {
            renderToken(r, cur, capture_trailing_comma ? Space::newline : Space::space);  // ,
            cur += 1;
        }
        if (tree.tokenTag(cur) == TokenTag::pipe) break;
    }
    if (capture_trailing_comma) ais.popIndent();

    renderThenElse(r, cur, for_node.ast.then_expr, for_node.else_token, std::nullopt, for_node.ast.else_expr, space);
}

void renderContainerField(Render &r, Container container, const full::ContainerField &field_param, Space space) {
    const Ast &tree = *r.tree;
    AutoIndentingStream &ais = *r.ais;
    full::ContainerField field = field_param;
    if (container != Container::tuple) field.convertToNonTupleLike(tree);
    const QuoteBehavior quote =
        container == Container::enum_ ? QuoteBehavior::eagerly_unquote_except_underscore : QuoteBehavior::eagerly_unquote;

    if (field.comptime_token) renderToken(r, *field.comptime_token, Space::maybe_space);  // comptime

    enum class Last { value, align_, type, identifier };
    const Last last_component = field.ast.value_expr   ? Last::value
                                : field.ast.align_expr ? Last::align_
                                : field.ast.type_expr  ? Last::type
                                                       : Last::identifier;

    if (!field.ast.tuple_like) {
        if (last_component == Last::identifier) {
            return renderIdentifierComma(r, field.ast.main_token, space, quote);  // name
        }
        const Space this_space = field.ast.type_expr ? Space::none : Space::space;
        renderIdentifier(r, field.ast.main_token, this_space, quote);  // name
    }

    if (field.ast.type_expr) {
        const NodeIndex type_expr = field.ast.type_expr.unwrap();
        if (!field.ast.tuple_like) renderToken(r, field.ast.main_token + 1, Space::maybe_space);  // :

        if (last_component == Last::type) return renderExpressionComma(r, type_expr, space);  // type
        renderExpression(r, type_expr, Space::space);                                          // type
    }

    if (field.ast.align_expr) {
        const NodeIndex align_expr = field.ast.align_expr.unwrap();
        const TokenIndex align_token = tree.firstToken(align_expr) - 2;
        renderToken(r, align_token, Space::none);      // align
        renderToken(r, align_token + 1, Space::none);  // (
        renderExpression(r, align_expr, Space::none);  // alignment
        const TokenIndex rparen = tree.lastToken(align_expr) + 1;
        if (last_component == Last::align_) return renderTokenComma(r, rparen, space);  // )
        renderToken(r, rparen, Space::space);
    }

    if (field.ast.value_expr) {
        const NodeIndex value_expr = field.ast.value_expr.unwrap();
        const TokenIndex eq_token = tree.firstToken(value_expr) - 1;
        const bool seperate_line = !tree.tokensOnSameLine(eq_token, eq_token + 1) ||
                                   tree.tokenTag(eq_token + 1) == TokenTag::multiline_string_literal_line;
        const Space eq_space = seperate_line ? Space::newline : Space::space;

        ais.pushIndent(IndentType::after_equals);
        renderToken(r, eq_token, eq_space);  // =
        if (eq_space == Space::space) {
            ais.popIndent();
            return renderExpressionComma(r, value_expr, space);  // value
        }

        const TokenIndex maybe_comma = tree.lastToken(value_expr) + 1;
        if (tree.tokenTag(maybe_comma) == TokenTag::comma) {
            renderExpression(r, value_expr, Space::none);  // value
            ais.popIndent();
            renderToken(r, maybe_comma, Space::newline);
        } else {
            renderExpression(r, value_expr, space);  // value
            ais.popIndent();
        }
    }
}

void renderBuiltinCall(Render &r, TokenIndex builtin_token, std::span<const NodeIndex> params, Space space) {
    renderToken(r, builtin_token, Space::none);  // @name
    return renderParamList(r, builtin_token + 1, params, space);
}

TokenIndex fnProtoRparen(const Ast &tree, const full::FnProto &fn_proto, TokenIndex maybe_bang) {
    // These may appear in any order, so we have to check the token_starts array
    // to find out which is first.
    TokenIndex rparen = tree.tokenTag(maybe_bang) == TokenTag::bang ? maybe_bang - 1 : maybe_bang;
    auto smallest_start = tree.tokenStart(maybe_bang);
    for (const OptionalIndex opt : {fn_proto.ast.align_expr, fn_proto.ast.addrspace_expr, fn_proto.ast.section_expr,
                                    fn_proto.ast.callconv_expr}) {
        if (!opt) continue;
        const TokenIndex tok = tree.firstToken(opt.unwrap()) - 3;
        const auto start = tree.tokenStart(tok);
        if (start < smallest_start) {
            rparen = tok;
            smallest_start = start;
        }
    }
    return rparen;
}

bool isOneLineFnProto(const Ast &tree, const full::FnProto &fn_proto, TokenIndex lparen, TokenIndex rparen) {
    const bool trailing_comma = tree.tokenTag(rparen - 1) == TokenTag::comma;
    if (trailing_comma || hasComment(tree, lparen, rparen)) return false;

    // Check that there are no doc comments
    TokenIndex after_last_param = lparen + 1;
    for (const NodeIndex expr : fn_proto.ast.params) {
        // Looking before each param is insufficient since anytype is not included in `params`
        if (hasDocComment(tree, after_last_param, tree.firstToken(expr))) return false;
        after_last_param = tree.lastToken(expr) + 1;
    }
    return !hasDocComment(tree, after_last_param, rparen);
}

void renderFnProto(Render &r, const full::FnProto &fn_proto, Space space) {
    const Ast &tree = *r.tree;
    AutoIndentingStream &ais = *r.ais;

    const TokenIndex after_fn_token = fn_proto.ast.fn_token + 1;
    TokenIndex lparen;
    if (tree.tokenTag(after_fn_token) == TokenTag::identifier) {
        renderToken(r, fn_proto.ast.fn_token, Space::space);                                       // fn
        renderIdentifier(r, after_fn_token, Space::none, QuoteBehavior::preserve_when_shadowing);  // name
        lparen = after_fn_token + 1;
    } else {
        renderToken(r, fn_proto.ast.fn_token, Space::space);  // fn
        lparen = fn_proto.ast.fn_token + 1;
    }

    const NodeIndex return_type = fn_proto.ast.return_type.unwrap();
    const TokenIndex maybe_bang = tree.firstToken(return_type) - 1;
    const TokenIndex rparen = fnProtoRparen(tree, fn_proto, maybe_bang);

    // The params list is a sparse set that does *not* include anytype or ... parameters.

    if (isOneLineFnProto(tree, fn_proto, lparen, rparen)) {
        // Render all on one line, no trailing comma.
        renderToken(r, lparen, Space::none);  // (

        std::size_t param_i = 0;
        TokenIndex last_param_token = lparen;
        while (true) {
            last_param_token += 1;
            const TokenTag tag = tree.tokenTag(last_param_token);
            if (tag == TokenTag::ellipsis3) {
                renderToken(r, last_param_token, Space::none);  // ...
                break;
            } else if (tag == TokenTag::keyword_noalias || tag == TokenTag::keyword_comptime) {
                renderToken(r, last_param_token, Space::maybe_space);
                last_param_token += 1;
            } else if (tag == TokenTag::keyword_anytype) {
                renderToken(r, last_param_token, Space::none);  // anytype
                continue;
            } else if (tag == TokenTag::r_paren) {
                break;
            } else if (tag == TokenTag::comma) {
                renderToken(r, last_param_token, Space::maybe_space);  // ,
                continue;
            }
            if (tree.tokenTag(last_param_token) == TokenTag::identifier &&
                tree.tokenTag(last_param_token + 1) == TokenTag::colon) {
                renderIdentifier(r, last_param_token, Space::none, QuoteBehavior::preserve_when_shadowing);  // name
                last_param_token = last_param_token + 1;
                renderToken(r, last_param_token, Space::maybe_space);  // :
                last_param_token += 1;
            }
            if (tree.tokenTag(last_param_token) == TokenTag::keyword_anytype) {
                renderToken(r, last_param_token, Space::none);  // anytype
                continue;
            }
            const NodeIndex param = fn_proto.ast.params[param_i];
            param_i += 1;
            renderExpression(r, param, Space::none);
            last_param_token = tree.lastToken(param);
        }
    } else {
        // One param per line.
        ais.pushIndent(IndentType::normal);
        renderToken(r, lparen, Space::newline);  // (

        std::size_t param_i = 0;
        TokenIndex last_param_token = lparen;
        while (true) {
            last_param_token += 1;
            const TokenTag tag = tree.tokenTag(last_param_token);
            if (tag == TokenTag::doc_comment) {
                renderToken(r, last_param_token, Space::newline);
                continue;
            } else if (tag == TokenTag::ellipsis3) {
                renderToken(r, last_param_token, Space::comma);  // ...
                break;
            } else if (tag == TokenTag::keyword_noalias || tag == TokenTag::keyword_comptime) {
                renderToken(r, last_param_token, Space::maybe_space);
                last_param_token += 1;
            } else if (tag == TokenTag::keyword_anytype) {
                renderToken(r, last_param_token, Space::comma);  // anytype
                if (tree.tokenTag(last_param_token + 1) == TokenTag::comma) last_param_token += 1;
                continue;
            } else if (tag == TokenTag::r_paren) {
                break;
            }
            if (tree.tokenTag(last_param_token) == TokenTag::identifier &&
                tree.tokenTag(last_param_token + 1) == TokenTag::colon) {
                renderIdentifier(r, last_param_token, Space::none, QuoteBehavior::preserve_when_shadowing);  // name
                last_param_token += 1;
                renderToken(r, last_param_token, Space::maybe_space);  // :
                last_param_token += 1;
            }
            if (tree.tokenTag(last_param_token) == TokenTag::keyword_anytype) {
                renderToken(r, last_param_token, Space::comma);  // anytype
                if (tree.tokenTag(last_param_token + 1) == TokenTag::comma) last_param_token += 1;
                continue;
            }
            const NodeIndex param = fn_proto.ast.params[param_i];
            param_i += 1;
            ais.pushSpace(Space::comma);
            renderExpression(r, param, Space::comma);
            ais.popSpace();
            last_param_token = tree.lastToken(param);
            if (tree.tokenTag(last_param_token + 1) == TokenTag::comma) last_param_token += 1;
        }
        ais.popIndent();
    }

    renderToken(r, rparen, Space::maybe_space);  // )

    if (fn_proto.ast.align_expr) {
        const NodeIndex align_expr = fn_proto.ast.align_expr.unwrap();
        const TokenIndex align_lparen = tree.firstToken(align_expr) - 1;
        const TokenIndex align_rparen = tree.lastToken(align_expr) + 1;
        renderToken(r, align_lparen - 1, Space::none);  // align
        renderToken(r, align_lparen, Space::none);      // (
        renderExpression(r, align_expr, Space::none);
        renderToken(r, align_rparen, Space::maybe_space);  // )
    }

    if (fn_proto.ast.addrspace_expr) {
        const NodeIndex addrspace_expr = fn_proto.ast.addrspace_expr.unwrap();
        const TokenIndex align_lparen = tree.firstToken(addrspace_expr) - 1;
        const TokenIndex align_rparen = tree.lastToken(addrspace_expr) + 1;
        renderToken(r, align_lparen - 1, Space::none);  // addrspace
        renderToken(r, align_lparen, Space::none);      // (
        renderExpression(r, addrspace_expr, Space::none);
        renderToken(r, align_rparen, Space::maybe_space);  // )
    }

    if (fn_proto.ast.section_expr) {
        const NodeIndex section_expr = fn_proto.ast.section_expr.unwrap();
        const TokenIndex section_lparen = tree.firstToken(section_expr) - 1;
        const TokenIndex section_rparen = tree.lastToken(section_expr) + 1;
        renderToken(r, section_lparen - 1, Space::none);  // section
        renderToken(r, section_lparen, Space::none);      // (
        renderExpression(r, section_expr, Space::none);
        renderToken(r, section_rparen, Space::maybe_space);  // )
    }

    if (fn_proto.ast.callconv_expr) {
        const NodeIndex callconv_expr = fn_proto.ast.callconv_expr.unwrap();
        // Keep in sync with logic in `renderMember`. PROMOTE_CALLCONV_INLINE
        const bool is_callconv_inline = tree.tokenSlice(tree.nodeMainToken(callconv_expr)) == "@\"inline\"";
        const bool is_declaration = fn_proto.name_token.has_value();
        if (!(is_declaration && is_callconv_inline)) {
            const TokenIndex callconv_lparen = tree.firstToken(callconv_expr) - 1;
            const TokenIndex callconv_rparen = tree.lastToken(callconv_expr) + 1;
            renderToken(r, callconv_lparen - 1, Space::none);  // callconv
            renderToken(r, callconv_lparen, Space::none);      // (
            renderExpression(r, callconv_expr, Space::none);
            renderToken(r, callconv_rparen, Space::maybe_space);  // )
        }
    }

    if (tree.tokenTag(maybe_bang) == TokenTag::bang) renderToken(r, maybe_bang, Space::none);  // !
    return renderExpression(r, return_type, space);
}

void renderSwitchCase(Render &r, const full::SwitchCase &switch_case, Space space) {
    AutoIndentingStream &ais = *r.ais;
    const Ast &tree = *r.tree;
    const bool trailing_comma = tree.tokenTag(switch_case.ast.arrow_token - 1) == TokenTag::comma;
    const bool has_comment_before_arrow =
        !switch_case.ast.values.empty() &&
        hasComment(tree, tree.firstToken(switch_case.ast.values[0]), switch_case.ast.arrow_token);

    // render inline keyword
    if (switch_case.inline_token) renderToken(r, *switch_case.inline_token, Space::maybe_space);

    // Render everything before the arrow
    if (switch_case.ast.values.empty()) {
        renderToken(r, switch_case.ast.arrow_token - 1, Space::space);  // else keyword
    } else if (trailing_comma || has_comment_before_arrow) {
        // Render each value on a new line
        ais.pushSpace(Space::comma);
        renderExpressions(r, switch_case.ast.values, Space::comma);
        ais.popSpace();
    } else {
        // Render on one line
        for (const NodeIndex value_expr : switch_case.ast.values) {
            renderExpression(r, value_expr, Space::comma_maybe_space);
        }
    }

    renderToken(r, switch_case.ast.arrow_token, Space::maybe_space);  // =>

    if (switch_case.payload_token) {
        const TokenIndex payload_token = *switch_case.payload_token;
        renderToken(r, payload_token - 1, Space::none);  // pipe
        TokenIndex ident = payload_token;
        if (tree.tokenTag(ident) == TokenTag::asterisk) {
            renderToken(r, payload_token, Space::none);  // asterisk
            ident += 1;
        }
        renderIdentifier(r, ident, Space::none, QuoteBehavior::preserve_when_shadowing);  // identifier
        if (tree.tokenTag(ident + 1) == TokenTag::comma) {
            ident += 2;
            renderToken(r, ident - 1, Space::space);                                          // ,
            renderIdentifier(r, ident, Space::none, QuoteBehavior::preserve_when_shadowing);  // identifier
        }
        renderToken(r, ident + 1, Space::maybe_space);  // pipe
    }

    return renderExpression(r, switch_case.ast.target_expr, space);
}

void renderBlock(Render &r, NodeIndex block_node, std::span<const NodeIndex> statements, Space space) {
    const Ast &tree = *r.tree;
    AutoIndentingStream &ais = *r.ais;
    const TokenIndex lbrace = tree.nodeMainToken(block_node);

    if (tree.isTokenPrecededByTags(lbrace, {TokenTag::identifier, TokenTag::colon})) {
        renderIdentifier(r, lbrace - 2, Space::none, QuoteBehavior::eagerly_unquote);  // identifier
        renderToken(r, lbrace - 1, Space::space);                                      // :
    }
    ais.pushIndent(IndentType::normal);
    if (statements.empty()) {
        renderToken(r, lbrace, Space::none);
        ais.popIndent();
        renderToken(r, tree.lastToken(block_node), space);  // rbrace
        return;
    }
    renderToken(r, lbrace, Space::newline);
    return finishRenderBlock(r, block_node, statements, space);
}

void finishRenderBlock(Render &r, NodeIndex block_node, std::span<const NodeIndex> statements, Space space) {
    const Ast &tree = *r.tree;
    AutoIndentingStream &ais = *r.ais;
    for (std::size_t i = 0; i < statements.size(); i++) {
        const NodeIndex stmt = statements[i];
        if (i != 0) renderExtraNewline(r, stmt);
        ais.pushSpace(Space::semicolon);
        switch (tree.nodeTag(stmt)) {
            case NodeTag::global_var_decl:
            case NodeTag::local_var_decl:
            case NodeTag::simple_var_decl:
            case NodeTag::aligned_var_decl:
                renderVarDecl(r, *tree.fullVarDecl(stmt), false, Space::semicolon);
                break;
            default: renderExpression(r, stmt, Space::semicolon); break;
        }
        ais.popSpace();
    }
    ais.popIndent();

    renderToken(r, tree.lastToken(block_node), space);  // rbrace
}

void renderStructInit(Render &r, NodeIndex struct_node, const full::StructInit &struct_init, Space space) {
    const Ast &tree = *r.tree;
    AutoIndentingStream &ais = *r.ais;

    if (struct_init.ast.type_expr) {
        renderExpression(r, struct_init.ast.type_expr.unwrap(), Space::none);  // T
    } else {
        renderToken(r, struct_init.ast.lbrace - 1, Space::none);  // .
    }

    if (struct_init.ast.fields.empty()) {
        ais.pushIndent(IndentType::normal);
        renderToken(r, struct_init.ast.lbrace, Space::none);  // lbrace
        ais.popIndent();
        return renderToken(r, struct_init.ast.lbrace + 1, space);  // rbrace
    }

    const TokenIndex rbrace = tree.lastToken(struct_node);
    const bool trailing_comma = tree.tokenTag(rbrace - 1) == TokenTag::comma;
    if (trailing_comma || hasComment(tree, struct_init.ast.lbrace, rbrace)) {
        // Render one field init per line.
        ais.pushIndent(IndentType::normal);
        renderToken(r, struct_init.ast.lbrace, Space::newline);

        for (std::size_t i = 0; i < struct_init.ast.fields.size(); i++) {
            const NodeIndex field_init = struct_init.ast.fields[i];
            const TokenIndex init_token = tree.firstToken(field_init);
            if (i != 0) renderExtraNewlineToken(r, init_token - 3);
            renderToken(r, init_token - 3, Space::none);                                         // .
            renderIdentifier(r, init_token - 2, Space::space, QuoteBehavior::eagerly_unquote);  // name
            renderToken(r, init_token - 1, Space::maybe_space);                                  // =

            ais.pushSpace(Space::comma);
            renderExpression(r, field_init, Space::comma);
            ais.popSpace();
        }

        ais.popIndent();
    } else {
        // Render all on one line, no trailing comma.
        renderToken(r, struct_init.ast.lbrace, Space::space);

        for (const NodeIndex field_init : struct_init.ast.fields) {
            const TokenIndex init_token = tree.firstToken(field_init);
            renderToken(r, init_token - 3, Space::none);                                         // .
            renderIdentifier(r, init_token - 2, Space::space, QuoteBehavior::eagerly_unquote);  // name
            renderToken(r, init_token - 1, Space::maybe_space);                                  // =
            renderExpression(r, field_init, Space::comma_space);
        }
    }

    return renderToken(r, rbrace, space);
}

constexpr std::size_t kNonprint = static_cast<std::size_t>(-1);

void renderArrayInit(Render &r, const full::ArrayInit &array_init, Space space) {
    const Ast &tree = *r.tree;
    AutoIndentingStream &ais = *r.ais;

    if (array_init.ast.type_expr) {
        renderExpression(r, array_init.ast.type_expr.unwrap(), Space::none);  // T
    } else {
        renderToken(r, array_init.ast.lbrace - 1, Space::none);  // .
    }

    if (array_init.ast.elements.empty()) {
        ais.pushIndent(IndentType::normal);
        renderToken(r, array_init.ast.lbrace, Space::none);  // lbrace
        ais.popIndent();
        return renderToken(r, array_init.ast.lbrace + 1, space);  // rbrace
    }

    const NodeIndex last_elem = array_init.ast.elements.back();
    const TokenIndex last_elem_token = tree.lastToken(last_elem);
    const bool trailing_comma = tree.tokenTag(last_elem_token + 1) == TokenTag::comma;
    const TokenIndex rbrace = trailing_comma ? last_elem_token + 2 : last_elem_token + 1;

    if (array_init.ast.elements.size() == 1) {
        const NodeIndex only_elem = array_init.ast.elements[0];
        const TokenIndex first_token = tree.firstToken(only_elem);
        if (tree.tokenTag(first_token) != TokenTag::multiline_string_literal_line &&
            !anythingBetween(tree, last_elem_token, rbrace)) {
            renderToken(r, array_init.ast.lbrace, Space::none);
            renderExpression(r, only_elem, Space::none);
            return renderToken(r, rbrace, space);
        }
    }

    const bool contains_comment = hasComment(tree, array_init.ast.lbrace, rbrace);
    const bool contains_multiline_string = hasMultilineString(tree, array_init.ast.lbrace, rbrace);

    if (!trailing_comma && !contains_comment && !contains_multiline_string) {
        // Render all on one line, no trailing comma.
        if (array_init.ast.elements.size() == 1) {
            // If there is only one element, we don't use spaces
            renderToken(r, array_init.ast.lbrace, Space::none);
            renderExpression(r, array_init.ast.elements[0], Space::none);
        } else {
            renderToken(r, array_init.ast.lbrace, Space::space);
            for (const NodeIndex elem : array_init.ast.elements) renderExpression(r, elem, Space::comma_space);
        }
        return renderToken(r, last_elem_token + 1, space);  // rbrace
    }

    ais.pushIndent(IndentType::normal);
    renderToken(r, array_init.ast.lbrace, Space::newline);
    ais.pushSpace(Space::comma);

    const std::size_t n = array_init.ast.elements.size();
    std::vector<std::size_t> expr_widths(n);
    {
        Writer buf;
        AutoIndentingStream sub_ais(&buf, indent_delta);
        sub_ais.disabled_offset = ais.disabled_offset;
        Render sub_r{&sub_ais, r.tree};
        for (std::size_t k = 0; k < n; k++) {
            const NodeIndex e = array_init.ast.elements[k];
            const bool begin_disabled = sub_ais.disabled_offset.has_value();
            // `.skip` space so trailing commments aren't included
            renderExpressionComma(sub_r, e, Space::skip);
            if (!begin_disabled && !sub_ais.disabled_offset) {
                const std::string &w = buf.buf;
                std::size_t width = w.size() - (!w.empty() && w.back() == ',' ? 1 : 0);
                for (const char c : w) {
                    const auto uc = static_cast<unsigned char>(c);
                    if (uc < 0x20 || uc > 0x7e) {
                        width = kNonprint;
                        break;
                    }
                }
                expr_widths[k] = width;
            } else {
                expr_widths[k] = kNonprint;
            }

            // Write trailing comments since they may enable/disable zig fmt
            buf.buf.clear();
            TokenIndex after_expr = tree.lastToken(e);
            if (tree.tokenTag(after_expr + 1) == TokenTag::comma) after_expr += 1;
            renderSpace(sub_r, after_expr, tokenSliceForRender(tree, after_expr).size(), Space::none);

            buf.buf.clear();
            // The following are needed to make sure isLineOverIndented is not influenced by
            // the previous element.
            sub_ais.indent_count = 0;
            sub_ais.applied_indent = 0;
        }
    }

    std::span<const NodeIndex> remaining_exprs = array_init.ast.elements;
    std::span<const std::size_t> remaining_widths(expr_widths);
    while (!remaining_exprs.empty()) {
        std::size_t row_size = 1;
        bool broke = false;
        for (std::size_t j = 0; j < remaining_exprs.size(); j++) {
            const std::size_t len = j + 1;
            const NodeIndex e = remaining_exprs[j];
            if (remaining_widths[j] == kNonprint) {
                broke = true;
                break;
            }
            row_size = len;

            TokenIndex after_expr = tree.lastToken(e);
            if (tree.tokenTag(after_expr + 1) == TokenTag::comma) after_expr += 1;
            if (!tree.tokensOnSameLine(after_expr, after_expr + 1)) {
                broke = true;
                break;
            }
        }
        if (!broke) {
            // All the expressions are on the same line.
            // However, if there is a trailing comma, we put them each on their own line.
            if (tree.tokenTag(rbrace - 1) == TokenTag::comma) row_size = 1;
        }

        // Determine the size of this section
        std::size_t section_end = remaining_exprs.size();
        {
            std::size_t line_start = row_size;  // Start after the first row to ignore comments on it
            for (std::size_t i = line_start; i < remaining_exprs.size(); i++) {
                const NodeIndex e = remaining_exprs[i];
                const TokenIndex expr_first = tree.firstToken(e);
                // Any nonprint character terminates the line because they are always put on their
                // own line, so they will not end up on the same line as the trailing comment.
                // (Zig indexes the whole expr_widths here, not the remaining ones.)
                if (expr_widths[i - 1] == kNonprint || !tree.tokensOnSameLine(expr_first - 1, expr_first)) {
                    line_start = i;
                }

                TokenIndex after_expr = tree.lastToken(e);
                if (tree.tokenTag(after_expr + 1) == TokenTag::comma) after_expr += 1;
                if (hasTrailingComment(tree, after_expr)) {
                    section_end = line_start;
                    break;
                }
            }
        }
        const auto section_exprs = remaining_exprs.subspan(0, section_end);
        const auto section_widths = remaining_widths.subspan(0, section_end);
        remaining_exprs = remaining_exprs.subspan(section_end);
        remaining_widths = remaining_widths.subspan(section_end);

        // Determine the width of each column
        std::vector<std::size_t> col_widths(row_size, 0);

        std::size_t col = 0;
        for (const std::size_t w : section_widths) {
            if (w == kNonprint) {
                col = 0;
                continue;
            }
            col_widths[col] = std::max(col_widths[col], w);
            col += 1;
            if (col == row_size) col = 0;
        }

        // Render each expression
        col = 0;
        for (std::size_t i = 0; i < section_exprs.size(); i++) {
            const NodeIndex e = section_exprs[i];
            const std::size_t w = section_widths[i];
            if (i + 1 == section_end || col + 1 == row_size || w == kNonprint || section_widths[i + 1] == kNonprint) {
                renderExpression(r, e, Space::comma);
                col = 0;
                if (i + 1 != section_end) renderExtraNewline(r, section_exprs[i + 1]);
            } else {
                renderExpression(r, e, Space::comma_space);
                ais.splatByteAll(' ', col_widths[col] - w);
                col += 1;
            }
        }
    }

    ais.popSpace();
    ais.popIndent();
    return renderToken(r, rbrace, space);  // rbrace
}

bool isOneLineContainerDecl(const Ast &tree, const full::ContainerDecl &container_decl, TokenIndex lbrace,
                            TokenIndex rbrace) {
    // We print all the members in one-line unless one of the following conditions are true:

    // 1. The container has comments or multiline strings.
    if (hasComment(tree, lbrace, rbrace) || hasMultilineString(tree, lbrace, rbrace)) return false;

    // 2. The container has a container comment.
    if (tree.tokenTag(lbrace + 1) == TokenTag::container_doc_comment) return false;

    // 3. A member of the container has a doc comment.
    if (hasDocComment(tree, lbrace + 1, rbrace)) return false;

    // 4. The container has non-field members.
    for (const NodeIndex member : container_decl.ast.members) {
        if (!tree.fullContainerField(member)) return false;
    }

    return true;
}

void renderContainerDecl(Render &r, NodeIndex container_decl_node, const full::ContainerDecl &container_decl,
                         Space space) {
    const Ast &tree = *r.tree;
    AutoIndentingStream &ais = *r.ais;

    if (container_decl.layout_token) renderToken(r, *container_decl.layout_token, Space::space);

    Container container = Container::other;
    switch (tree.tokenTag(container_decl.ast.main_token)) {
        case TokenTag::keyword_enum: container = Container::enum_; break;
        case TokenTag::keyword_struct:
            container = Container::tuple;
            for (const NodeIndex member : container_decl.ast.members) {
                if (auto field = tree.fullContainerField(member)) {
                    if (!field->ast.tuple_like) {
                        container = Container::other;
                        break;
                    }
                }
            }
            break;
        default: container = Container::other; break;
    }

    TokenIndex lbrace;
    if (container_decl.ast.enum_token) {
        const TokenIndex enum_token = *container_decl.ast.enum_token;
        renderToken(r, container_decl.ast.main_token, Space::none);  // union
        renderToken(r, enum_token - 1, Space::none);                 // lparen
        renderToken(r, enum_token, Space::none);                     // enum
        if (container_decl.ast.arg) {
            const NodeIndex arg = container_decl.ast.arg.unwrap();
            renderToken(r, enum_token + 1, Space::none);  // lparen
            renderExpression(r, arg, Space::none);
            const TokenIndex rparen = tree.lastToken(arg) + 1;
            renderToken(r, rparen, Space::none);       // rparen
            renderToken(r, rparen + 1, Space::space);  // rparen
            lbrace = rparen + 2;
        } else {
            renderToken(r, enum_token + 1, Space::space);  // rparen
            lbrace = enum_token + 2;
        }
    } else if (container_decl.ast.arg) {
        const NodeIndex arg = container_decl.ast.arg.unwrap();
        renderToken(r, container_decl.ast.main_token, Space::none);      // union
        renderToken(r, container_decl.ast.main_token + 1, Space::none);  // lparen
        renderExpression(r, arg, Space::none);
        const TokenIndex rparen = tree.lastToken(arg) + 1;
        renderToken(r, rparen, Space::space);  // rparen
        lbrace = rparen + 1;
    } else {
        renderToken(r, container_decl.ast.main_token, Space::space);  // union
        lbrace = container_decl.ast.main_token + 1;
    }

    const TokenIndex rbrace = tree.lastToken(container_decl_node);

    if (container_decl.ast.members.empty()) {
        ais.pushIndent(IndentType::normal);
        if (tree.tokenTag(lbrace + 1) == TokenTag::container_doc_comment) {
            renderToken(r, lbrace, Space::newline);  // lbrace
            renderContainerDocComments(r, lbrace + 1);
        } else {
            renderToken(r, lbrace, Space::none);  // lbrace
        }
        ais.popIndent();
        return renderToken(r, rbrace, space);  // rbrace
    }

    const bool src_has_trailing_comma = tree.tokenTag(rbrace - 1) == TokenTag::comma;
    if (!src_has_trailing_comma && isOneLineContainerDecl(tree, container_decl, lbrace, rbrace)) {
        // Print all the declarations on the same line.
        renderToken(r, lbrace, Space::space);  // lbrace
        for (const NodeIndex member : container_decl.ast.members) renderMember(r, container, member, Space::space);
        return renderToken(r, rbrace, space);  // rbrace
    }

    // One member per line.
    ais.pushIndent(IndentType::normal);
    renderToken(r, lbrace, Space::newline);  // lbrace
    if (tree.tokenTag(lbrace + 1) == TokenTag::container_doc_comment) renderContainerDocComments(r, lbrace + 1);
    for (std::size_t i = 0; i < container_decl.ast.members.size(); i++) {
        const NodeIndex member = container_decl.ast.members[i];
        if (i != 0) renderExtraNewline(r, member);
        switch (tree.nodeTag(member)) {
            // For container fields, ensure a trailing comma is added if necessary.
            case NodeTag::container_field_init:
            case NodeTag::container_field_align:
            case NodeTag::container_field:
                ais.pushSpace(Space::comma);
                renderMember(r, container, member, Space::comma);
                ais.popSpace();
                break;
            default: renderMember(r, container, member, Space::newline); break;
        }
    }
    ais.popIndent();

    return renderToken(r, rbrace, space);  // rbrace
}

void renderAsm(Render &r, const full::Asm &asm_node, Space space) {
    const Ast &tree = *r.tree;
    AutoIndentingStream &ais = *r.ais;

    renderToken(r, asm_node.ast.asm_token, Space::space);  // asm

    if (asm_node.volatile_token) {
        renderToken(r, *asm_node.volatile_token, Space::space);     // volatile
        renderToken(r, *asm_node.volatile_token + 1, Space::none);  // lparen
    } else {
        renderToken(r, asm_node.ast.asm_token + 1, Space::none);  // lparen
    }

    std::array<std::optional<TokenIndex>, 3> render_colons{};
    {
        std::array<TokenIndex, 3> colons{};
        unsigned render = 0;

        const TokenIndex rparen = asm_node.ast.rparen;
        do {
            colons[0] = tree.lastToken(asm_node.ast.template_) + 1;
            if (colons[0] == rparen) break;

            if (!asm_node.outputs.empty()) {
                colons[1] = tree.lastToken(asm_node.outputs.back()) + 1;
                if (tree.tokenTag(colons[1]) == TokenTag::comma) colons[1] += 1;
                render = 1;
            } else {
                colons[1] = colons[0] + 1;
                if (hasComment(tree, colons[0], colons[1])) render = 1;
            }
            if (colons[1] == rparen) break;

            // Next colon is not checked for here since it cannot present without clobbers
            if (!asm_node.inputs.empty()) {
                render = 2;
            } else {
                const TokenIndex colon_or_rparen = colons[1] + 1;
                if (hasComment(tree, colons[1], colon_or_rparen)) render = 2;
            }

            if (asm_node.ast.clobbers) {
                colons[2] = tree.firstToken(asm_node.ast.clobbers.unwrap()) - 1;
                render = 3;
            }
        } while (false);

        for (unsigned i = 0; i < render; i++) render_colons[i] = colons[i];
    }

    ais.forcePushIndent(IndentType::normal);

    if (asm_node.ast.items.empty()) {
        if (asm_node.ast.clobbers) {
            const NodeIndex clobbers = asm_node.ast.clobbers.unwrap();
            // asm ("foo" ::: clobbers)
            renderExpression(r, asm_node.ast.template_, Space::space);
            // Render the three colons.
            const TokenIndex first_clobber = tree.firstToken(clobbers);
            renderToken(r, first_clobber - 3, Space::none);
            renderToken(r, first_clobber - 2, Space::none);
            renderToken(r, first_clobber - 1, Space::maybe_space);
            renderExpression(r, clobbers, Space::none);
            ais.popIndent();
            return renderToken(r, asm_node.ast.rparen, space);  // rparen
        }

        if (!render_colons[0]) {
            // asm ("foo")
            renderExpression(r, asm_node.ast.template_, Space::none);
            ais.popIndent();
            return renderToken(r, asm_node.ast.rparen, space);  // rparen
        }
    }

    renderExpression(r, asm_node.ast.template_, Space::newline);
    ais.forceLastIndent();  // Might have been dedented by a multiline string literal

    const std::size_t prev_indent_delta = ais.indent_delta_;  // May be part of another asm expression
    // so indent_delta can't be unconditionally used
    ais.setIndentDelta(asm_indent_delta);

    do {
        if (render_colons[0]) {
            const TokenIndex colon1 = *render_colons[0];
            if (!asm_node.outputs.empty()) {
                renderToken(r, colon1, Space::space);
                ais.forcePushIndent(IndentType::normal);

                const std::size_t final_ = asm_node.outputs.size() - 1;
                for (std::size_t i = 0; i < final_; i++) {
                    renderAsmOutput(r, asm_node.outputs[i], Space::none);
                    const TokenIndex next_start = tree.firstToken(asm_node.outputs[i + 1]);
                    renderToken(r, next_start - 1, Space::newline);  // ,
                    renderExtraNewlineToken(r, next_start);
                }

                ais.pushSpace(Space::comma);
                renderAsmOutput(r, asm_node.outputs[final_], Space::comma);
                ais.popSpace();
                ais.popIndent();
            } else {
                renderToken(r, colon1, Space::newline);
            }
        }

        if (render_colons[1]) {
            const TokenIndex colon2 = *render_colons[1];
            if (!asm_node.inputs.empty()) {
                renderToken(r, colon2, Space::space);
                ais.forcePushIndent(IndentType::normal);

                const std::size_t final_ = asm_node.inputs.size() - 1;
                for (std::size_t i = 0; i < final_; i++) {
                    renderAsmInput(r, asm_node.inputs[i], Space::none);
                    const TokenIndex next_start = tree.firstToken(asm_node.inputs[i + 1]);
                    renderToken(r, next_start - 1, Space::newline);  // ,
                    renderExtraNewlineToken(r, next_start);
                }

                ais.pushSpace(Space::comma);
                renderAsmInput(r, asm_node.inputs[final_], Space::comma);
                ais.popSpace();
                ais.popIndent();
            } else {
                renderToken(r, colon2, Space::newline);
            }
        } else {
            break;
        }

        if (render_colons[2]) {
            const TokenIndex colon3 = *render_colons[2];
            const NodeIndex clobbers = asm_node.ast.clobbers.unwrap();
            renderToken(r, colon3, Space::maybe_space);
            renderExpression(r, clobbers, Space::none);
            ais.forceLastIndent();  // Might have been dedented by a multiline string literal
        }
    } while (false);

    ais.setIndentDelta(prev_indent_delta);
    ais.popIndent();
    return renderToken(r, asm_node.ast.rparen, space);  // rparen
}

void renderCall(Render &r, const full::Call &call, Space space) {
    renderExpression(r, call.ast.fn_expr, Space::none);
    return renderParamList(r, call.ast.lparen, call.ast.params, space);
}

void renderParamList(Render &r, TokenIndex lparen, std::span<const NodeIndex> params, Space space) {
    const Ast &tree = *r.tree;
    AutoIndentingStream &ais = *r.ais;

    if (params.empty()) {
        ais.pushIndent(IndentType::normal);
        renderToken(r, lparen, Space::none);  // (
        ais.popIndent();
        return renderToken(r, lparen + 1, space);  // )
    }

    const NodeIndex last_param = params.back();
    const TokenIndex after_last_param_tok = tree.lastToken(last_param) + 1;
    if (tree.tokenTag(after_last_param_tok) == TokenTag::comma) {
        ais.pushIndent(IndentType::normal);
        renderToken(r, lparen, Space::newline);  // (
        for (std::size_t i = 0; i < params.size(); i++) {
            const NodeIndex param_node = params[i];
            if (i + 1 < params.size()) {
                renderExpression(r, param_node, Space::none);
                const TokenIndex comma = tree.lastToken(param_node) + 1;
                renderToken(r, comma, Space::newline);  // ,
                renderExtraNewline(r, params[i + 1]);
            } else {
                ais.pushSpace(Space::comma);
                renderExpression(r, param_node, Space::comma);
                ais.popSpace();
            }
        }
        ais.popIndent();
        return renderToken(r, after_last_param_tok + 1, space);  // )
    }

    ais.pushIndent(IndentType::normal);
    renderToken(r, lparen, Space::none);  // (
    for (std::size_t i = 0; i < params.size(); i++) {
        const NodeIndex param_node = params[i];
        renderExpression(r, param_node, Space::none);
        if (i + 1 < params.size()) {
            const TokenIndex comma = tree.lastToken(param_node) + 1;
            renderToken(r, comma, Space::maybe_space);
        }
    }
    ais.popIndent();
    return renderToken(r, after_last_param_tok, space);  // )
}

/// Render an expression, and the comma that follows it, if it is present in the source.
/// If a comma is present, and `space` is `Space.comma`, render only a single comma.
void renderExpressionComma(Render &r, NodeIndex node, Space space) {
    const Ast &tree = *r.tree;
    const TokenIndex maybe_comma = tree.lastToken(node) + 1;
    if (tree.tokenTag(maybe_comma) == TokenTag::comma && space != Space::comma) {
        renderExpression(r, node, Space::none);
        return renderToken(r, maybe_comma, space);
    } else {
        return renderExpression(r, node, space);
    }
}

/// Render a token, and the comma that follows it, if it is present in the source.
void renderTokenComma(Render &r, TokenIndex token, Space space) {
    const Ast &tree = *r.tree;
    const TokenIndex maybe_comma = token + 1;
    if (tree.tokenTag(maybe_comma) == TokenTag::comma && space != Space::comma) {
        renderToken(r, token, Space::none);
        return renderToken(r, maybe_comma, space);
    } else {
        return renderToken(r, token, space);
    }
}

/// Render an identifier, and the comma that follows it, if it is present in the source.
void renderIdentifierComma(Render &r, TokenIndex token, Space space, QuoteBehavior quote) {
    const Ast &tree = *r.tree;
    const TokenIndex maybe_comma = token + 1;
    if (tree.tokenTag(maybe_comma) == TokenTag::comma && space != Space::comma) {
        renderIdentifier(r, token, Space::none, quote);
        return renderToken(r, maybe_comma, space);
    } else {
        return renderIdentifier(r, token, space, quote);
    }
}

void renderToken(Render &r, TokenIndex token_index, Space space) {
    const std::string_view lexeme = tokenSliceForRender(*r.tree, token_index);
    r.ais->writeAll(lexeme);
    renderSpace(r, token_index, lexeme.size(), space);
}

void renderTokenOverrideSpaceMode(Render &r, TokenIndex token_index, Space space, Space override_space) {
    const std::string_view lexeme = tokenSliceForRender(*r.tree, token_index);
    r.ais->writeAll(lexeme);
    r.ais->enableSpaceMode(override_space);
    renderSpace(r, token_index, lexeme.size(), space);
    r.ais->disableSpaceMode();
}

void renderSpace(Render &r, TokenIndex token_index, std::size_t lexeme_len, Space space) {
    const Ast &tree = *r.tree;
    AutoIndentingStream &ais = *r.ais;

    const TokenTag next_token_tag = tree.tokenTag(token_index + 1);

    if (space == Space::skip) return;

    if (space == Space::comma && next_token_tag != TokenTag::comma) ais.writeByte(',');
    if (space == Space::semicolon || space == Space::comma) ais.enableSpaceMode(space);
    struct DisableSpaceMode {
        AutoIndentingStream &a;
        ~DisableSpaceMode() { a.disableSpaceMode(); }
    } defer_disable{ais};
    const bool comment = renderComments(r, tree.tokenStart(token_index) + lexeme_len, tree.tokenStart(token_index + 1));
    switch (space) {
        case Space::none: break;
        case Space::space:
            if (!comment) ais.writeByte(' ');
            break;
        case Space::newline:
            if (!comment) ais.insertNewline();
            break;

        case Space::comma:
            if (next_token_tag == TokenTag::comma) {
                renderToken(r, token_index + 1, Space::newline);
            } else if (!comment) {
                ais.insertNewline();
            }
            break;

        case Space::comma_space:
            if (next_token_tag == TokenTag::comma) {
                renderToken(r, token_index + 1, Space::space);
            } else if (!comment) {
                ais.writeByte(' ');
            }
            break;

        case Space::semicolon:
            if (next_token_tag == TokenTag::semicolon) {
                renderToken(r, token_index + 1, Space::newline);
            } else if (!comment) {
                ais.insertNewline();
            }
            break;

        case Space::maybe_space:
            if (!comment && next_token_tag != TokenTag::multiline_string_literal_line) ais.writeByte(' ');
            break;

        case Space::comma_maybe_space:
            if (next_token_tag == TokenTag::comma) {
                renderToken(r, token_index + 1, Space::maybe_space);
            } else if (!comment) {
                ais.writeByte(' ');
            }
            break;

        case Space::skip: break;
    }
}

void renderIdentifier(Render &r, TokenIndex token_index, Space space, QuoteBehavior quote) {
    const Ast &tree = *r.tree;
    const std::string_view lexeme = tokenSliceForRender(tree, token_index);

    if (lexeme.empty() || lexeme[0] != '@') return renderToken(r, token_index, space);

    const std::string_view contents = lexeme.substr(2, lexeme.size() - 3);  // inside the @"" quotation

    // Empty name can't be unquoted.
    if (contents.empty()) return renderQuotedIdentifier(r, token_index, space, false);

    // Special case for _.
    if (contents == "_") {
        switch (quote) {
            case QuoteBehavior::eagerly_unquote: return renderQuotedIdentifier(r, token_index, space, true);
            case QuoteBehavior::eagerly_unquote_except_underscore:
            case QuoteBehavior::preserve_when_shadowing: return renderQuotedIdentifier(r, token_index, space, false);
        }
    }

    // Scan the entire name for characters that would (after un-escaping) be illegal in a symbol,
    // i.e. contents don't match: [A-Za-z_][A-Za-z0-9_]*
    const auto is_alpha_ = [](std::uint32_t c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
    };
    std::size_t contents_i = 0;
    while (contents_i < contents.size()) {
        const char c = contents[contents_i];
        if (c >= '0' && c <= '9') {
            if (contents_i == 0) return renderQuotedIdentifier(r, token_index, space, false);
        } else if (is_alpha_(static_cast<unsigned char>(c))) {
        } else if (c == '\\') {
            std::size_t esc_offset = contents_i;
            const auto res = parseEscapeSequence(contents, esc_offset);
            if (!res) return renderQuotedIdentifier(r, token_index, space, false);
            const std::uint32_t ch = *res;
            if (ch >= '0' && ch <= '9') {
                if (contents_i == 0) return renderQuotedIdentifier(r, token_index, space, false);
            } else if (!is_alpha_(ch)) {
                return renderQuotedIdentifier(r, token_index, space, false);
            }
            contents_i = esc_offset;
            continue;
        } else {
            return renderQuotedIdentifier(r, token_index, space, false);
        }
        contents_i += 1;
    }

    // Read enough of the name (while un-escaping) to determine if it's a keyword or primitive.
    // If it's too long to fit in this buffer, we know it's neither and quoting is unnecessary.
    // If we read the whole thing, we have to do further checks.
    std::array<char, longest_keyword_or_primitive_len> buf{};

    contents_i = 0;
    std::size_t buf_i = 0;
    while (contents_i < contents.size() && buf_i < longest_keyword_or_primitive_len) {
        if (contents[contents_i] == '\\') {
            const auto res = parseEscapeSequence(contents, contents_i);
            buf[buf_i] = static_cast<char>(res.value_or(0));
            buf_i += 1;
        } else {
            buf[buf_i] = contents[contents_i];
            contents_i += 1;
            buf_i += 1;
        }
    }

    // We read the whole thing, so it could be a keyword or primitive.
    if (contents_i == contents.size()) {
        const std::string_view name(buf.data(), buf_i);
        if (!isValidId(name)) return renderQuotedIdentifier(r, token_index, space, false);
        if (isPrimitive(name)) {
            switch (quote) {
                case QuoteBehavior::eagerly_unquote:
                case QuoteBehavior::eagerly_unquote_except_underscore:
                    return renderQuotedIdentifier(r, token_index, space, true);
                case QuoteBehavior::preserve_when_shadowing:
                    return renderQuotedIdentifier(r, token_index, space, false);
            }
        }
    }

    renderQuotedIdentifier(r, token_index, space, true);
}

// Renders a @"" quoted identifier, normalizing escapes.
// Unnecessary escapes are un-escaped, and \u escapes are normalized to \x when they fit.
// If unquote is true, the @"" is removed and the result is a bare symbol whose validity is asserted.
void renderQuotedIdentifier(Render &r, TokenIndex token_index, Space space, bool unquote) {
    const Ast &tree = *r.tree;
    AutoIndentingStream &ais = *r.ais;
    const std::string_view lexeme = tokenSliceForRender(tree, token_index);

    if (!unquote) ais.writeAll("@\"");
    const std::string_view contents = lexeme.substr(2, lexeme.size() - 3);
    renderIdentifierContents(ais, contents);
    if (!unquote) ais.writeByte('"');

    renderSpace(r, token_index, lexeme.size(), space);
}

void renderIdentifierContents(AutoIndentingStream &ais, std::string_view bytes) {
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        const auto byte = static_cast<unsigned char>(bytes[pos]);
        if (byte == '\\') {
            const std::size_t old_pos = pos;
            const auto res = parseEscapeSequence(bytes, pos);
            const std::string_view escape_sequence = bytes.substr(old_pos, pos - old_pos);
            if (res) {
                if (*res <= 0x7f) {
                    std::string out;
                    const char one = static_cast<char>(*res);
                    stringEscape(std::string_view(&one, 1), out);
                    ais.printEscaped(out);
                } else {
                    ais.writeAll(escape_sequence);
                }
            } else {
                // Escape the stray backslash
                // This also avoids cases like "\x3\x39" becoming "\x39"
                ais.writeByte('\\');
                ais.writeAll(escape_sequence);
                if (pos == old_pos) pos += 1;  // (a trailing backslash: cannot occur in a valid token)
            }
        } else if (byte <= 0x7f) {
            std::string out;
            stringEscape(bytes.substr(pos, 1), out);
            ais.printEscaped(out);
            pos += 1;
        } else {
            ais.writeByte(static_cast<char>(byte));
            pos += 1;
        }
    }
}

/// Returns true if there exists a line comment between any of the tokens from
/// `start_token` to `end_token`.
bool hasComment(const Ast &tree, TokenIndex start_token, TokenIndex end_token) {
    const std::string_view source(tree.source);
    for (TokenIndex token = start_token; token < end_token; token++) {
        const std::size_t start = tree.tokenStart(token) + tree.tokenSlice(token).size();
        const std::size_t end = tree.tokenStart(token + 1);
        if (start < end && source.substr(start, end - start).find('/') != std::string_view::npos) return true;
    }
    return false;
}

/// Returns true if there exists a multiline string literal between the start
/// of token `start_token` and the start of token `end_token`.
bool hasMultilineString(const Ast &tree, TokenIndex start_token, TokenIndex end_token) {
    for (TokenIndex t = start_token; t < end_token; t++)
        if (tree.tokenTag(t) == TokenTag::multiline_string_literal_line) return true;
    return false;
}

/// Returns true if there exists a doc comment between the start of token
/// `start_token` and the start of token `end_token`.
bool hasDocComment(const Ast &tree, TokenIndex start_token, TokenIndex end_token) {
    for (TokenIndex t = start_token; t < end_token; t++)
        if (tree.tokenTag(t) == TokenTag::doc_comment) return true;
    return false;
}

/// Assumes that start is the first byte past the previous token and
/// that end is the last byte before the next token.
bool renderComments(Render &r, std::size_t start, std::size_t end) {
    const Ast &tree = *r.tree;
    AutoIndentingStream &ais = *r.ais;
    const std::string_view source(tree.source);

    std::size_t index = start;
    while (true) {
        if (index > end) break;
        const std::size_t offset = source.substr(index, end - index).find("//");
        if (offset == std::string_view::npos) break;
        const std::size_t comment_start = index + offset;

        // If there is no newline, the comment ends with EOF
        const std::size_t newline_index = source.substr(comment_start, end - comment_start).find('\n');
        const std::optional<std::size_t> newline =
            newline_index == std::string_view::npos ? std::nullopt : std::optional<std::size_t>(comment_start + newline_index);

        const std::string_view untrimmed_comment =
            source.substr(comment_start, (newline ? *newline : source.size()) - comment_start);
        const std::string_view trimmed_comment = trimEnd(untrimmed_comment);

        // Don't leave any whitespace at the start of the file
        if (index != 0) {
            const std::string_view before = source.substr(index, comment_start - index);
            if (index == start && containsAtLeastTwoNewlines(before)) {
                // Leave up to one empty line before the first comment
                ais.insertNewline();
                ais.insertNewline();
            } else if (before.find('\n') != std::string_view::npos) {
                // Respect the newline directly before the comment.
                // Note: This allows an empty line between comments
                ais.insertNewline();
            } else if (index == start) {
                // Otherwise if the first comment is on the same line as
                // the token before it, prefix it with a single space.
                ais.writeByte(' ');
            }
        }

        index = 1 + (newline ? *newline : end - 1);

        const std::string_view comment_content = trimStart(trimmed_comment.substr(2));
        if (ais.disabled_offset && comment_content == "zig fmt: on") {
            // Write the source for which formatting was disabled directly
            // to the underlying writer, fixing up invalid whitespace.
            const std::string_view disabled_source =
                source.substr(*ais.disabled_offset, comment_start - *ais.disabled_offset);
            writeFixingWhitespace(*ais.underlying_writer, disabled_source);
            // Write with the canonical single space.
            ais.underlying_writer->writeAll("// zig fmt: on\n");
            ais.disabled_offset.reset();
            ais.resetLine();
        } else if (!ais.disabled_offset && comment_content == "zig fmt: off") {
            // Write with the canonical single space.
            ais.writeAll("// zig fmt: off\n");
            ais.disabled_offset = index;
        } else {
            // Write the comment minus trailing whitespace.
            ais.printLine(trimmed_comment);
        }
    }

    if (index != start && containsAtLeastTwoNewlines(source.substr(index - 1, end - (index - 1)))) {
        // Don't leave any whitespace at the end of the file
        if (end != source.size()) ais.insertNewline();
    }

    return index != start;
}

void renderExtraNewline(Render &r, NodeIndex node) { return renderExtraNewlineToken(r, r.tree->firstToken(node)); }

/// Check if there is an empty line immediately before the given token. If so, render it.
void renderExtraNewlineToken(Render &r, TokenIndex token_index) {
    const Ast &tree = *r.tree;
    AutoIndentingStream &ais = *r.ais;
    const std::string_view source(tree.source);
    const std::size_t token_start = tree.tokenStart(token_index);
    if (token_start == 0) return;
    const std::size_t prev_token_end =
        token_index == 0 ? 0 : tree.tokenStart(token_index - 1) + tokenSliceForRender(tree, token_index - 1).size();

    // If there is a immediately preceding comment or doc_comment,
    // skip it because required extra newline has already been rendered.
    if (prev_token_end <= token_start &&
        source.substr(prev_token_end, token_start - prev_token_end).find("//") != std::string_view::npos)
        return;
    if (tree.isTokenPrecededByTags(token_index, {TokenTag::doc_comment})) return;

    // Iterate backwards to the end of the previous token, stopping if a
    // non-whitespace character is encountered or two newlines have been found.
    std::size_t i = token_start - 1;
    unsigned newlines = 0;
    while (isAsciiWhitespace(source[i])) {
        if (source[i] == '\n') newlines += 1;
        if (newlines == 2) return ais.insertNewline();
        if (i == prev_token_end) break;
        i -= 1;
    }
}

/// end_token is the token one past the last doc comment token. This function
/// searches backwards from there.
void renderDocComments(Render &r, TokenIndex end_token) {
    const Ast &tree = *r.tree;
    // Search backwards for the first doc comment.
    if (end_token == 0) return;
    TokenIndex tok = end_token - 1;
    while (true) {
        if (tree.tokenTag(tok) != TokenTag::doc_comment) {
            tok += 1;
            break;
        }
        if (tok == 0) break;
        tok -= 1;
    }
    const TokenIndex first_tok = tok;
    if (first_tok == end_token) return;

    if (first_tok != 0) {
        const TokenTag prev_token_tag = tree.tokenTag(first_tok - 1);
        if (prev_token_tag != TokenTag::l_brace) renderExtraNewlineToken(r, first_tok);
    }

    while (tree.tokenTag(tok) == TokenTag::doc_comment) {
        renderToken(r, tok, Space::newline);
        tok += 1;
    }
}

/// start_token is first container doc comment token.
void renderContainerDocComments(Render &r, TokenIndex start_token) {
    const Ast &tree = *r.tree;
    TokenIndex tok = start_token;
    while (tree.tokenTag(tok) == TokenTag::container_doc_comment) {
        renderToken(r, tok, Space::newline);
        tok += 1;
    }
    // Render extra newline if there is one between final container doc comment and
    // the next token. If the next token is a doc comment, that code path
    // will have its own logic to insert a newline.
    if (tree.tokenTag(tok) != TokenTag::doc_comment) renderExtraNewlineToken(r, tok);
}

std::string_view tokenSliceForRender(const Ast &tree, TokenIndex token_index) {
    std::string_view ret = tree.tokenSlice(token_index);
    switch (tree.tokenTag(token_index)) {
        case TokenTag::container_doc_comment:
        case TokenTag::doc_comment: ret = trimEnd(ret); break;
        default: break;
    }
    return ret;
}

bool hasTrailingComment(const Ast &tree, TokenIndex t) {
    const std::string_view source(tree.source);
    const std::size_t start = tree.tokenStart(t) + tree.tokenSlice(t).size();
    const std::size_t end = tree.tokenStart(t + 1);
    for (std::size_t i = start; i < end; i++) {
        if (source[i] == '\n') return false;
        if (source[i] == '/') return true;
    }
    return false;
}

/// Returns `true` if and only if there are any tokens or line comments between
/// start_token and end_token.
bool anythingBetween(const Ast &tree, TokenIndex start_token, TokenIndex end_token) {
    if (start_token + 1 != end_token) return true;
    return hasComment(tree, start_token, end_token);
}

void writeFixingWhitespace(Writer &w, std::string_view slice) {
    for (const char byte : slice) {
        switch (byte) {
            case '\t': w.splatByteAll(' ', indent_delta); break;
            case '\r': break;
            default: w.writeByte(byte); break;
        }
    }
}

bool nodeIsBlock(NodeTag tag) {
    switch (tag) {
        case NodeTag::block:
        case NodeTag::block_semicolon:
        case NodeTag::block_two:
        case NodeTag::block_two_semicolon: return true;
        default: return false;
    }
}

bool nodeIsIfForWhileSwitch(NodeTag tag) {
    switch (tag) {
        case NodeTag::if_:
        case NodeTag::if_simple:
        case NodeTag::for_:
        case NodeTag::for_simple:
        case NodeTag::while_:
        case NodeTag::while_simple:
        case NodeTag::while_cont:
        case NodeTag::switch_:
        case NodeTag::switch_comma: return true;
        default: return false;
    }
}

bool nodeCausesSliceOpSpace(NodeTag tag) {
    switch (tag) {
        case NodeTag::catch_:
        case NodeTag::add:
        case NodeTag::add_wrap:
        case NodeTag::array_cat:
        case NodeTag::array_mult:
        case NodeTag::assign:
        case NodeTag::assign_bit_and:
        case NodeTag::assign_bit_or:
        case NodeTag::assign_shl:
        case NodeTag::assign_shr:
        case NodeTag::assign_bit_xor:
        case NodeTag::assign_div:
        case NodeTag::assign_sub:
        case NodeTag::assign_sub_wrap:
        case NodeTag::assign_mod:
        case NodeTag::assign_add:
        case NodeTag::assign_add_wrap:
        case NodeTag::assign_mul:
        case NodeTag::assign_mul_wrap:
        case NodeTag::bang_equal:
        case NodeTag::bit_and:
        case NodeTag::bit_or:
        case NodeTag::shl:
        case NodeTag::shr:
        case NodeTag::bit_xor:
        case NodeTag::bool_and:
        case NodeTag::bool_or:
        case NodeTag::div:
        case NodeTag::equal_equal:
        case NodeTag::error_union:
        case NodeTag::greater_or_equal:
        case NodeTag::greater_than:
        case NodeTag::less_or_equal:
        case NodeTag::less_than:
        case NodeTag::merge_error_sets:
        case NodeTag::mod:
        case NodeTag::mul:
        case NodeTag::mul_wrap:
        case NodeTag::sub:
        case NodeTag::sub_wrap:
        case NodeTag::orelse_: return true;
        default: return false;
    }
}

// ---- the self-check -----------------------------------------------------------
//
// Not part of Render.zig. The formatted text is parsed again, and its tokens
// and comments are compared with the input's after undoing, on both sides,
// exactly the rewrites `zig fmt` makes:
//   - a trailing comma (before `) ] } | => :`) may be added: dropped;
//   - an identifier is compared by its decoded name (`@"foo"` == `foo`);
//   - doc comments and comments lose trailing whitespace; a `zig fmt:
//     off/on` comment gets one space; a disabled region's tabs become four
//     spaces and its CRs vanish;
//   - nested cast builtins (@ptrCast, @alignCast, ...) are reordered: their
//     names compare equal to one another;
//   - `callconv(.@"inline")` on a declaration becomes a leading `inline`:
//     both spellings are dropped.

struct CanonToken {
    TokenTag tag;
    std::string text;
    bool operator==(const CanonToken &o) const { return tag == o.tag && text == o.text; }
};

std::string decodeIdentifier(std::string_view lexeme) {
    if (lexeme.size() < 3 || lexeme[0] != '@') return std::string(lexeme);
    const std::string_view contents = lexeme.substr(2, lexeme.size() - 3);
    std::string out;
    std::size_t i = 0;
    while (i < contents.size()) {
        if (contents[i] == '\\') {
            const std::size_t before = i;
            const auto cp = parseEscapeSequence(contents, i);
            if (!cp) return "@\"" + std::string(contents) + "\"";  // undecodable: compare verbatim
            if (i == before) return std::string(lexeme);
            const std::uint32_t c = *cp;
            if (c < 0x80) {
                out += static_cast<char>(c);
            } else if (c < 0x800) {
                out += static_cast<char>(0xC0 | (c >> 6));
                out += static_cast<char>(0x80 | (c & 0x3F));
            } else if (c < 0x10000) {
                out += static_cast<char>(0xE0 | (c >> 12));
                out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (c & 0x3F));
            } else {
                out += static_cast<char>(0xF0 | (c >> 18));
                out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (c & 0x3F));
            }
        } else {
            out += contents[i];
            i += 1;
        }
    }
    return out;
}

std::string canonComment(std::string_view comment) {
    std::string out;
    for (const char c : comment) {
        if (c == '\r') continue;
        if (c == '\t') {
            out += "    ";
        } else {
            out += c;
        }
    }
    const std::string_view trimmed = trimEnd(out);
    const std::string_view content = trimStart(trimmed.substr(2));
    if (content == "zig fmt: on" || content == "zig fmt: off") return "// " + std::string(content);
    return std::string(trimmed);
}

bool isClosing(TokenTag t) {
    return t == TokenTag::r_paren || t == TokenTag::r_bracket || t == TokenTag::r_brace || t == TokenTag::pipe ||
           t == TokenTag::equal_angle_bracket_right || t == TokenTag::colon;
}

/// The canonical token+comment stream of a parsed tree.
std::vector<CanonToken> canonicalStream(const Ast &tree) {
    const std::string_view source(tree.source);
    std::vector<CanonToken> raw;
    raw.reserve(tree.tokens.size() * 2);
    const auto add_comments = [&](std::size_t start, std::size_t end) {
        std::size_t i = start;
        while (i < end) {
            const std::size_t at = source.substr(i, end - i).find("//");
            if (at == std::string_view::npos) break;
            const std::size_t cs = i + at;
            std::size_t ce = source.substr(cs, end - cs).find('\n');
            ce = ce == std::string_view::npos ? end : cs + ce;
            raw.push_back(CanonToken{TokenTag::invalid, canonComment(source.substr(cs, ce - cs))});
            i = ce;
        }
    };
    add_comments(0, tree.tokens.empty() ? source.size() : tree.tokenStart(0));
    for (TokenIndex t = 0; t < tree.tokens.size(); t++) {
        const TokenTag tag = tree.tokenTag(t);
        if (tag == TokenTag::eof) break;
        const std::string_view slice = tree.tokenSlice(t);
        std::string text;
        switch (tag) {
            case TokenTag::identifier: text = decodeIdentifier(slice); break;
            case TokenTag::doc_comment:
            case TokenTag::container_doc_comment: text = std::string(trimEnd(slice)); break;
            case TokenTag::builtin:
                text = castKind(slice.substr(1)) ? std::string("@<cast>") : std::string(slice);
                break;
            default: text = std::string(slice); break;
        }
        raw.push_back(CanonToken{tag, std::move(text)});
        add_comments(tree.tokenStart(t) + slice.size(), tree.tokenStart(t + 1));
    }

    // Drop trailing commas and the two spellings of an inline declaration.
    std::vector<CanonToken> out;
    out.reserve(raw.size());
    const auto next_real = [&](std::size_t k) -> TokenTag {
        for (std::size_t j = k + 1; j < raw.size(); j++)
            if (raw[j].tag != TokenTag::invalid) return raw[j].tag;
        return TokenTag::eof;
    };
    for (std::size_t k = 0; k < raw.size(); k++) {
        const CanonToken &tok = raw[k];
        if (tok.tag == TokenTag::comma && isClosing(next_real(k))) continue;
        if (tok.tag == TokenTag::keyword_inline && next_real(k) == TokenTag::keyword_fn) continue;
        if (tok.tag == TokenTag::keyword_callconv && k + 4 < raw.size() && raw[k + 1].tag == TokenTag::l_paren &&
            raw[k + 2].tag == TokenTag::period && raw[k + 3].tag == TokenTag::identifier &&
            raw[k + 3].text == "inline" && raw[k + 4].tag == TokenTag::r_paren) {
            k += 4;
            continue;
        }
        out.push_back(tok);
    }
    return out;
}

/// The first mismatch between two canonical streams, as a 1-based line in
/// the input, or 0 when they agree.
bool sameStreams(const std::vector<CanonToken> &a, const std::vector<CanonToken> &b) { return a == b; }

}  // namespace

Result Format(std::string_view src, const Options &opts) {
    Result result;
    result.text = std::string(src);

    // readSourceFileToEndAlloc: these encodings are refused (UTF-16 LE is
    // converted by zig fmt; mep leaves it alone).
    if (src.starts_with("\xff\xfe") || src.starts_with("\xfe\xff")) {
        result.error = "unsupported encoding (UTF-16/UTF-32 byte order mark)";
        return result;
    }

    zig::Mode mode = opts.mode == Mode::zon ? zig::Mode::zon : zig::Mode::zig;
    Ast tree = zig::parse(src, mode);
    if (!tree.errors.empty() && opts.mode == Mode::auto_) {
        Ast zon = zig::parse(src, zig::Mode::zon);
        if (zon.errors.empty()) {
            tree = std::move(zon);
            mode = zig::Mode::zon;
        }
    }
    if (!tree.errors.empty()) {
        const auto errors = zig::renderErrors(tree);
        if (!errors.empty()) {
            result.error = errors[0].message;
            result.error_line = static_cast<int>(errors[0].line) + 1;
        } else {
            result.error = "syntax error";
        }
        return result;
    }

    Writer w;
    try {
        renderTree(w, tree);
    } catch (const WriteFailed &) {
        result.error = "internal error: write failed";
        return result;
    }

    // The self-check: the output must parse, to the same tokens and comments.
    const Ast again = zig::parse(w.buf, mode);
    if (!again.errors.empty()) {
        const auto errors = zig::renderErrors(again);
        result.error = "formatter self-check failed: the output does not parse";
        if (!errors.empty()) result.error += " (" + errors[0].message + ")";
        return result;
    }
    if (!sameStreams(canonicalStream(tree), canonicalStream(again))) {
        result.error = "formatter self-check failed: the output's tokens differ from the input's";
        return result;
    }

    result.ok = true;
    result.text = std::move(w.buf);
    return result;
}

}  // namespace zigfmt
