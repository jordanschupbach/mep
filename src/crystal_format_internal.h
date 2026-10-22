#pragma once

// The insides of crystal_format.h: the Formatter class, a port of the
// Crystal compiler's Crystal::Formatter (src/compiler/crystal/tools/
// formatter.cr, Crystal 1.19), method for method and with its names, so a
// change to Crystal's formatter maps onto these files:
//
//   crystal_format.cpp         Format(), the proof-reading, and the
//                              formatter's plumbing: the lexer-sync helpers
//                              (next_token, skip_space, write_comment...),
//                              writing, indentation, and `finish` with its
//                              post-passes (fix_heredocs, align_infos,
//                              align_comments, format_doc_comments)
//   crystal_format_visit.cpp   the visitors for literals, expressions,
//                              definitions and macros
//   crystal_format_call.cpp    the visitors for calls, blocks, operators,
//                              assignments and control flow
//
// The original is a Crystal Visitor: `accept` dispatches on the node's
// class. Here `visit` switches on Node::kind. Crystal's overloads that
// differ only by argument type get distinct names (skip_space_at for
// `skip_space(indent : Int32, ...)`, indent_node for `indent(node)`...).
// Where the original builds a throwaway node (`Call.new("as", to)` to
// print a cast like a call) this one builds it in the formatter's own
// arena. A `raise "BUG: ..."` of the original throws FormatBug; a syntax
// error from the lexer is a crystal::SyntaxError, as there.

#include <cstdint>
#include <deque>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "crystal_ast.h"
#include "crystal_parser.h"
#include "crystal_syntax.h"

namespace crfmt {

using crystal::Keyword;
using crystal::Node;
using crystal::NodeKind;
using crystal::Tok;
using crystal::Token;

/** @brief The original's `raise "..."`: the formatter met something it cannot handle. */
struct FormatBug : std::runtime_error {
    using std::runtime_error::runtime_error;
};

using VarScopes = std::vector<std::unordered_set<std::string>>;

class Formatter {
public:
    /** @brief Formatter.format: parses `source` and formats it (throws SyntaxError / FormatBug). */
    static std::string FormatSource(std::string_view source);

    explicit Formatter(std::string_view source);
    Formatter(const Formatter &) = delete;
    Formatter &operator=(const Formatter &) = delete;

    void accept(Node *node);
    std::string finish();
    bool skip_space_or_newline(bool last = false, bool at_least_one = false, bool next_comes_end = false);
    void write_indent() { write_indent(indent_); }

    std::set<int> no_rstrip_lines;
    VarScopes vars;
    int inside_lib = 0;
    int inside_enum = 0;
    int inside_struct_or_union = 0;
    int indent_ = 0;
    int subformat_nesting = 0;

private:
    struct AlignInfo {
        uint64_t id;
        int line;
        int start_column;
        int middle_column;
        int end_column;
        bool number;
        int size() const { return end_column - start_column; }
    };
    struct CommentInfo {
        int start_line;
        int end_line;
        bool needs_newline = true;
        bool needs_format;
    };
    struct HeredocFix {
        int start_line;
        int end_line;
        int difference;
    };
    struct HeredocInfo {
        Node *node;
        Token token;
        int line;
        int column;
        int indent;
        int string_continuation;
    };
    // A literal's element: a node, a hash entry or a named tuple entry.
    using HashEntry = std::pair<Node *, Node *>;
    using NamedEntry = std::pair<std::string, Node *>;

    std::string source_;  // the lexer reads this copy
    crystal::Lexer lexer_;
    crystal::Ast synth_;  // nodes the formatter makes up (Call.new(...) and the like)
    std::deque<HeredocInfo> heredoc_infos_;
    std::vector<int> comment_columns_;  // kNoColumn: nil
    static constexpr int kNoColumn = -1000000000;
    int line_ = 0;
    int column_ = 0;
    Token *tok_;
    std::string output_;
    std::string line_output_;
    bool wrote_newline_ = false;
    bool wrote_double_newlines_ = false;
    bool wrote_comment_ = false;
    crystal::MacroState macro_state_;
    int inside_macro_ = 0;
    int inside_cond_ = 0;
    int implicit_exception_handler_indent_ = 0;
    std::string last_write_;
    bool exp_needs_indent_ = true;
    int inside_def_ = 0;
    int paren_count_ = 0;
    std::vector<AlignInfo> when_infos_;
    std::vector<AlignInfo> hash_infos_;
    std::vector<AlignInfo> assign_infos_;
    std::vector<CommentInfo> doc_comments_;
    std::optional<CommentInfo> current_doc_comment_;
    std::unordered_set<const Node *> hash_in_same_line_;
    bool shebang_ = false;
    std::vector<HeredocFix> heredoc_fixes_;
    std::optional<int> assign_length_;
    Node *current_hash_ = nullptr;
    bool last_is_heredoc_ = false;
    bool last_arg_is_skip_ = false;
    int string_continuation_ = 0;
    int inside_call_or_assign_ = 0;
    bool passed_backslash_newline_ = false;
    bool consuming_heredocs_ = false;

    // --- made-up nodes --------------------------------------------------------------
    Node *make(NodeKind k) { return synth_.make(k); }
    Node *string_literal(const std::string &value);
    Node *new_call(Node *obj, const std::string &name, std::vector<Node *> args = {});
    Node *nop() { return make(NodeKind::Nop); }
    Node *expressions_from(const std::vector<Node *> &exps);

    // --- visitors (crystal_format_visit.cpp / crystal_format_call.cpp) ----------------
    bool visit(Node *node);
    bool visit_expressions(Node *node);
    bool assign_p(Node *exp);
    int assign_length(Node *exp);
    std::pair<Node *, int> find_assign_chunk(const std::vector<Node *> &expressions, Node *last, size_t i);
    bool needs_two_lines_p(Node *node, Node *next_node);
    bool abstract_def_p(Node *node);
    bool needs_two_lines_p(Node *node);
    bool visit_string_literal(Node *node);
    void write_sanitized_string_body(bool escape);
    bool visit_string_interpolation(Node *node, Token *token, int line, int column, int old_indent,
                                    int old_string_continuation, bool wrote_token = false);
    void consume_heredocs();
    bool visit_regex_literal(Node *node);
    void format_regex_modifiers();
    bool space_slash_newline_p();
    bool space_newline_p();
    bool visit_array_literal(Node *node);
    template <typename T>
    void format_literal_elements(const std::vector<T> &elements, Tok prefix, Tok suffix);
    void accept_element(Node *node) { accept(node); }
    void accept_element(const HashEntry &entry) { format_hash_entry(current_hash_, entry); }
    void accept_element(const NamedEntry &entry) { format_named_tuple_entry(current_hash_, entry); }
    static bool element_is_nop(Node *node) { return node->is(NodeKind::Nop); }
    static bool element_is_nop(const HashEntry &) { return false; }
    static bool element_is_nop(const NamedEntry &) { return false; }
    void indent_element(int indent, Node *node) { indent_node(indent, node); }
    template <typename E>
    void indent_element(int indent, const E &entry);
    template <typename E>
    void write_indent_element(int indent, const E &element);
    bool visit_hash_literal(Node *node);
    void format_hash_entry(Node *hash, const HashEntry &entry);
    bool visit_named_tuple_literal(Node *node);
    void format_named_tuple_entry(Node *hash, const NamedEntry &entry);
    void format_named_argument_name(const std::string &name);
    void finish_list(Tok suffix, bool has_newlines, bool found_comment, bool found_first_newline,
                     bool write_space_at_end);
    bool check_hash_info(Node *hash, const Node *key, int start_line, int start_column, int middle_column);
    bool visit_range_literal(Node *node);
    bool check_open_paren();
    void check_close_paren();
    bool visit_path(Node *node);
    bool visit_generic(Node *node);
    bool visit_generic_inner(Node *node);
    bool visit_union(Node *node);
    bool visit_if_or_unless(Node *node, Keyword keyword);
    void format_if_at_cond(Node *node, Keyword keyword, bool check_end = true);
    void format_elsif(Node *node_else, Keyword keyword);
    bool format_while_or_until(Node *node, Keyword keyword);
    void format_nested(Node *node, int indent, bool write_end_line = true, bool write_indent = true);
    void format_nested(Node *node) { format_nested(node, indent_); }
    bool format_nested_with_end(Node *node, int column, bool write_end_line = true);
    bool format_nested_with_end(Node *node) { return format_nested_with_end(node, indent_); }
    void format_end(int column);
    bool visit_def(Node *node);
    int format_def_args(Node *node);
    int format_def_args(const std::vector<Node *> &args, Node *block_arg, int splat_index, bool variadic,
                        Node *double_splat, bool yields);
    void format_parameter_annotations(Node *node);
    template <typename F>
    bool format_def_arg(bool wrote_newline, bool has_more, bool write_trailing_comma, F &&body);
    bool at_skip_p();
    bool visit_fun_def(Node *node);
    bool visit_macro(Node *node);
    void format_macro_literal_only(Node *node, const std::string &source, int macro_node_line);
    bool format_macro_body(Node *node);
    bool format_macro_end();
    bool visit_macro_literal(Node *node);
    bool visit_macro_verbatim(Node *node);
    bool visit_macro_expression(Node *node);
    bool visit_macro_if(Node *node);
    bool format_macro_if_epilogue(Node *node, crystal::MacroState macro_state, bool check_end = true);
    bool visit_macro_for(Node *node);
    bool visit_macro_var(Node *node);
    void write_macro_slashes();
    void format_macro_contents(Node *node, int macro_node_line);
    std::optional<std::string> macro_literal_contents(Node *node);
    bool only_macro_literal_p(Node *node);
    void extract_macro_literal_contents(Node *node, std::string &io);
    std::string subformat(const std::string &source, std::set<int> *no_rstrip_lines_out);
    bool visit_arg(Node *node);
    void reset_macro_state();
    bool visit_splat(Node *node, Tok token);
    bool visit_proc_notation(Node *node);
    bool visit_read_instance_var(Node *node);

    bool visit_call(Node *node);
    std::tuple<bool, bool, int> format_call_args(Node *node, bool has_parentheses, int base_indent);
    std::tuple<bool, bool, int> format_args(const std::vector<Node *> &args, bool has_parentheses,
                                            const std::vector<Node *> *named_args, Node *block_arg,
                                            int needed_indent, bool do_consume_newlines = false);
    std::tuple<bool, bool, int> format_args(const std::vector<Node *> &args, bool has_parentheses) {
        return format_args(args, has_parentheses, nullptr, nullptr, indent_ + 2);
    }
    std::tuple<bool, bool, int> format_args_simple(const std::vector<Node *> &args, int needed_indent,
                                                   bool do_consume_newlines);
    std::tuple<bool, bool, int> format_named_args(const std::vector<Node *> &args,
                                                  const std::vector<Node *> &named_args, int needed_indent);
    bool format_block_arg(Node *block_arg, int needed_indent);
    void finish_args(bool has_parentheses, bool has_newlines, bool ends_with_newline, bool found_comment, int column);
    void format_parenthesized_args(const std::vector<Node *> &args, const std::vector<Node *> *named_args = nullptr);
    bool visit_named_argument(Node *node);
    void format_block(Node *node, bool needs_space);
    void clear_object(Node *node);
    Node *format_block_args(const std::vector<Node *> &args, Node *node);
    void format_block_arg();
    Node *remove_to_skip(Node *node, int to_skip);
    bool format_binary(Node *node, Tok token, Tok alternative);
    bool visit_not(Node *node);
    bool visit_assign(Node *node);
    bool visit_op_assign(Node *node);
    void accept_assign_value_after_equals(Node *value, bool check_align = false);
    void accept_assign_value(Node *value, bool check_align = false);
    bool check_assign_length(Node *exp);
    void check_assign_align(int before_column, Node *exp);
    bool visit_visibility_modifier(Node *node);
    bool visit_module_def(Node *node);
    bool visit_annotation_def(Node *node);
    bool visit_class_def(Node *node);
    void format_type_vars(Node *node);
    bool visit_c_struct_or_union_def(Node *node);
    bool visit_lib_def(Node *node);
    bool visit_enum_def(Node *node);
    bool visit_type_declaration(Node *node);
    bool visit_uninitialized_var(Node *node);
    bool format_control_expression(Node *node, Keyword keyword);
    int opening_curly_brace_count();
    int leading_tuple_depth(Node *exp);
    bool visit_yield(Node *node);
    bool visit_case(Node *node);
    bool format_when(Node *case_node, Node *node, bool is_last, bool align_number);
    bool visit_select(Node *node);
    bool visit_annotation(Node *node);
    bool visit_multi_assign(Node *node);
    void format_multi_assign_values(const std::vector<Node *> &values);
    bool visit_exception_handler(Node *node);
    bool format_alias_or_typedef(Node *node, Keyword keyword, Node *value);
    bool visit_proc_pointer(Node *node);
    bool visit_proc_literal(Node *node);
    bool visit_external_var(Node *node);
    bool visit_metaclass(Node *node);
    bool visit_asm(Node *node);
    bool visit_asm_operand(Node *node);
    template <typename T, typename F>
    void visit_asm_parts(const std::vector<T> &parts, int colon_column, F &&f);

    // --- plumbing (crystal_format.cpp) -------------------------------------------------
    Token &next_token();
    Token &next_string_token();
    Token &next_string_array_token();
    void next_macro_token();
    void next_token_skip_space() {
        next_token();
        skip_space();
    }
    void next_token_skip_space_or_newline() {
        next_token();
        skip_space_or_newline();
    }
    // next_token_skip_space's value in the original is skip_space's.
    bool next_token_skip_space_b() {
        next_token();
        return skip_space();
    }
    bool skip_space(bool write_comma = false, bool consume_newline = true);
    bool skip_space_at(int indent, bool write_comma = false);
    bool skip_space_or_newline_at(int indent, bool last = false, bool at_least_one = false,
                                  bool next_comes_end = false);
    void slash_is_regex() { lexer_.slash_is_regex_on(); }
    void slash_is_not_regex() { lexer_.slash_is_not_regex(); }
    bool skip_space_write_line();
    void skip_semicolon();
    bool skip_semicolon_or_space();
    void skip_semicolon_or_space_or_newline();
    void jump_semicolon();
    void write_comment(bool needs_indent = true, bool consume_newline = true, bool next_comes_end = false);
    void consume_newlines(bool next_comes_end = false);
    template <typename F>
    auto indent2(F &&f) -> decltype(f());  // indent(&): two more
    void indent_node(Node *node);
    template <typename F>
    auto indent_at(int indent, F &&f) -> decltype(f());  // indent(indent : Int, &)
    void indent_node(int indent, Node *node);
    void no_indent(Node *node);
    template <typename F>
    void no_indent_do(F &&f);
    void write_indent(int indent);
    void write_indent(int indent, Node *node);
    void write(std::string_view s);
    void write(Keyword k) { write(std::string_view(crystal::KeywordName(k))); }
    void write(Tok t) { write(std::string_view(crystal::TokName(t))); }
    void write_line();
    void increment_line();
    void decrement_line();
    void increment_lines(int count);
    void fix_heredocs(std::vector<std::string> &lines);
    static int leading_space_count(const std::string &line, int max_count);
    void align_infos(std::vector<std::string> &lines, const std::vector<AlignInfo> &align_infos);
    std::pair<const AlignInfo *, int> find_last_info(const std::vector<AlignInfo> &align_infos, const AlignInfo *base,
                                                     size_t i);
    void align_info(std::vector<std::string> &lines, const AlignInfo &info, int max_size);
    void align_comments(std::vector<std::string> &lines);
    int find_max_column(size_t base, int max);
    static std::string align_comment(const std::string &line, int comment_column, int max_column);
    void format_doc_comments(std::vector<std::string> &lines);
    void write_keyword(Keyword keyword);
    void write_keyword(const char *before, Keyword keyword);
    void write_keyword(Keyword keyword, const char *after, bool skip_space_or_newline = true);
    void write_keyword(const char *before, Keyword keyword, const char *after);
    void write_token(Tok type);
    void write_token(const char *before, Tok type);
    void write_token(Tok type, const char *after);
    void write_token(const char *before, Tok type, const char *after);
    void check_keyword(Keyword k);
    void check_keyword(Keyword a, Keyword b);
    void check(Tok type);
    void check(Tok a, Tok b);
    [[noreturn]] void unexpected(const std::string &expecting);
    void check_end();
    template <typename C>
    static bool last_p(size_t index, const C &collection) {
        return index + 1 == collection.size();
    }
    template <typename F>
    void inside_macro(F &&f);
    template <typename F>
    void outside_macro(F &&f);
    bool inside_macro_p() const { return inside_macro_ != 0; }
    bool check_macro_whitespace();
    template <typename F>
    void inside_cond(F &&f);
    template <typename F>
    void inside_call_or_assign(F &&f);
};

// --- templates ----------------------------------------------------------------------

template <typename F>
auto Formatter::indent2(F &&f) -> decltype(f()) {
    indent_ += 2;
    if constexpr (std::is_void_v<decltype(f())>) {
        f();
        indent_ -= 2;
    } else {
        auto value = f();
        indent_ -= 2;
        return value;
    }
}

template <typename F>
auto Formatter::indent_at(int indent, F &&f) -> decltype(f()) {
    const int old_indent = indent_;
    indent_ = indent;
    if constexpr (std::is_void_v<decltype(f())>) {
        f();
        passed_backslash_newline_ = false;
        indent_ = old_indent;
    } else {
        auto value = f();
        passed_backslash_newline_ = false;
        indent_ = old_indent;
        return value;
    }
}

template <typename F>
void Formatter::no_indent_do(F &&f) {
    const int old_indent = indent_;
    indent_ = 0;
    f();
    indent_ = old_indent;
}

template <typename F>
void Formatter::inside_macro(F &&f) {
    inside_macro_++;
    f();
    inside_macro_--;
}

template <typename F>
void Formatter::outside_macro(F &&f) {
    const int old_inside_macro = inside_macro_;
    inside_macro_ = 0;
    f();
    inside_macro_ = old_inside_macro;
}

template <typename F>
void Formatter::inside_cond(F &&f) {
    inside_cond_++;
    f();
    inside_cond_--;
}

template <typename F>
void Formatter::inside_call_or_assign(F &&f) {
    inside_call_or_assign_++;
    f();
    inside_call_or_assign_--;
}

// --- text helpers (crystal_format.cpp) -------------------------------------------------

/** @brief Char#ascii_whitespace?. */
inline bool AsciiWhitespace(char32_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}
/** @brief String#strip / #rstrip / #lstrip (Unicode whitespace, as Crystal's). */
std::string Strip(std::string_view s);
std::string Rstrip(std::string_view s);
/** @brief The characters of `s` from character `from` (String#[from..]); FormatBug past the end. */
std::string CharsFrom(std::string_view s, size_t from);
/** @brief The first `n` characters (String#[0...n], clamped). */
std::string CharsTo(std::string_view s, size_t n);
/** @brief String#[i]? as a code point, 0 when out of range. */
char32_t CharAt(std::string_view s, size_t i);

}  // namespace crfmt
