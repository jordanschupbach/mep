#pragma once

// The Crystal parser: a port of the Crystal compiler's
// src/compiler/crystal/syntax/parser.cr (Crystal 1.19). As in the
// compiler, Parser is a Lexer subclass that drives the lexer token by
// token, and it stops at the first syntax error, which it reports exactly
// as the compiler would (message, line and column).
//
// Method for method in the original's order and with its names, so a
// change to Crystal's parser maps onto this file. The two places that
// cannot be the same: `Regex.error?` (the compiler compiles every regex
// literal with PCRE2 to report "invalid regex"; this parser does not
// check regex syntax) and the `#<loc:...>` pragmas of macro expansions.
//
// Crystal is Apache-2.0 licensed; this is a derived work (see
// crystal_syntax.h).

#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "crystal_ast.h"
#include "crystal_syntax.h"

namespace crystal {

/** @brief The result of a parse: the tree, or the first syntax error. */
struct ParseResult {
    Node *root = nullptr;  // null on error
    bool ok = false;
    std::string error;
    int error_line = 0;
    int error_column = 0;
    std::vector<std::string> warnings;
};

/**
 * @brief Parses Crystal source as the compiler does a file (wants_doc on: doc comments are attached).
 * @param src the source text
 * @param ast owns the nodes of the result
 * @param filename what `__FILE__` and `__DIR__` expand to
 */
ParseResult Parse(std::string_view src, Ast &ast, const std::string &filename = "");

class Parser : public Lexer {
public:
    Parser(std::string_view src, Ast &ast);

    void set_wants_doc(bool w) {
        wants_doc_ = w;
        doc_enabled = w;
    }
    std::string filename;

    Node *parse();
    Token &next_token() override;

    /** @brief Parser::ParseMode: parse(mode) reads a lib, struct/union or enum body (the formatter's macro code). */
    enum class ParseMode : uint8_t { Normal, Lib, LibStructOrUnion, Enum };
    Node *parse(ParseMode mode);
    /** @brief The variable scopes to start from (`Parser.new(str, var_scopes: ...)`). */
    void set_var_scopes(std::vector<std::unordered_set<std::string>> scopes) { var_scopes_ = std::move(scopes); }

private:
    struct Unclosed {
        std::string name;
        Location location;
    };
    struct CallArgs {
        std::vector<Node *> args;
        bool has_args = false;
        Node *block = nullptr;
        Node *block_arg = nullptr;
        std::vector<Node *> named_args;
        bool has_named_args = false;
        bool stopped_on_do_after_space = false;
        Location end_location;
        bool has_parentheses = false;
    };
    struct ArgExtras {
        Node *block_arg = nullptr;
        bool default_value = false;
        bool splat = false;
        bool double_splat = false;
    };
    struct Piece {
        bool is_string = true;
        std::string str;
        Node *node = nullptr;
        int line_number = 0;
    };

    Ast &ast_;
    std::vector<std::unordered_set<std::string>> var_scopes_;
    std::vector<Unclosed> unclosed_stack_;
    bool calls_super_ = false, calls_initialize_ = false, calls_previous_def_ = false, uses_block_arg_ = false;
    bool is_macro_def_ = false, assigns_special_var_ = false;
    int def_nest_ = 0, fun_nest_ = 0, type_nest_ = 0;
    bool is_constant_assignment_ = false;
    std::vector<Location> call_args_start_locations_;
    int temp_arg_count_ = 0;
    bool in_macro_expression_ = false;
    int stop_on_yield_ = 0;
    bool inside_c_struct_ = false;
    bool wants_doc_ = false;
    int no_type_declaration_ = 0;
    bool consuming_heredocs_ = false;
    bool inside_interpolation_ = false;
    bool stop_on_do_ = false;
    std::vector<std::string> assigned_vars_;
    bool has_visibility_ = false;
    Visibility visibility_ = Visibility::Public;
    bool has_block_arg_name_ = false;
    std::string block_arg_name_;
    int block_arity_ = -1;

    // --- node helpers ---------------------------------------------------------------
    Node *make(NodeKind k) { return ast_.make(k); }
    static Node *at(Node *n, Location l) {
        n->location = l;
        return n;
    }
    static Node *at(Node *n, const Node *o) {
        n->location = Loc(o);
        n->end_location = EndLoc(o);
        return n;
    }
    static Node *at_end(Node *n, Location l) {
        n->end_location = l;
        return n;
    }
    static Node *at_end(Node *n, const Node *o) {
        n->end_location = EndLoc(o);
        return n;
    }
    Node *nop() { return make(NodeKind::Nop); }
    Node *expressions_from(std::vector<Node *> v);
    Node *new_expressions(std::vector<Node *> v);
    Node *var_node(const std::string &name);
    Node *call_node(Node *obj, const std::string &name, std::vector<Node *> args = {}, Node *block = nullptr,
                    Node *block_arg = nullptr, const std::vector<Node *> *named_args = nullptr, bool global = false);
    Node *path_global(const std::string &name);
    Node *string_literal(const std::string &s);
    Node *number_literal(long long v);
    Node *unary(NodeKind k, Node *exp);
    Node *concat_expressions(Node *x, Node *y);

    // --- token helpers ----------------------------------------------------------------
    bool tok_kw(Keyword k) const { return token_.type == Tok::IDENT && token_.vkind == TokValue::Keyword && token_.kw == k; }
    bool value_is(Keyword k) const { return token_.vkind == TokValue::Keyword && token_.kw == k; }
    bool value_is_str(const char *s) const { return token_.vkind == TokValue::String && token_.str == s; }
    std::string value_s() const { return token_.value_s(); }

    // --- the parser (parser.cr order) -------------------------------------------------
    Node *parse_expressions();
    Node *parse_expressions_internal();
    Node *parse_multi_assign();
    bool multi_assign_target(Node *exp);
    bool multi_assign_middle(Node *exp);
    Node *multi_assign_left_hand(Node *exp);
    Node *parse_expression();
    Node *parse_expression_suffix(Node *atomic, Location location);
    Node *parse_op_assign_no_control(bool allow_ops = true, bool allow_suffix = true);
    Node *parse_op_assign(bool allow_ops = true, bool allow_suffix = true);
    Node *parse_question_colon();
    Node *parse_range();
    Node *new_range(Node *exp, Location location, bool exclusive);
    Node *parse_or();
    Node *parse_and();
    Node *parse_equality();
    Node *parse_cmp();
    Node *parse_logical_or();
    Node *parse_logical_and();
    Node *parse_shift();
    Node *parse_add_or_sub();
    Node *parse_mul_or_div();
    Node *parse_pow();
    Node *parse_prefix();
    Node *parse_atomic_with_method();
    Node *parse_atomic_method_suffix(Node *atomic, Location location);
    Node *parse_atomic_method_suffix_special(Node *call, Location location);
    Node *parse_single_arg();
    Node *parse_is_a(Node *atomic);
    Node *parse_as(Node *atomic, NodeKind klass = NodeKind::Cast);
    Node *parse_responds_to(Node *atomic);
    std::string parse_responds_to_name();
    Node *parse_nil_q(Node *atomic);
    Node *parse_negation_suffix(Node *atomic);
    Node *parse_read_instance_var(Node *atomic);
    Node *parse_atomic();
    Node *parse_atomic_without_location();
    template <typename F>
    Node *check_type_declaration(F &&f);
    Node *parse_type_declaration(Node *var);
    bool next_comes_colon_space();
    Node *new_node_check_type_declaration(NodeKind klass);
    Node *parse_generic_or_custom_literal();
    Node *parse_custom_literal(Node *type);
    template <typename F>
    Node *check_not_inside_def(const std::string &message, F &&f);
    bool inside_def() const { return def_nest_ > 0; }
    bool inside_fun() const { return fun_nest_ > 0; }
    Node *parse_annotation();
    Node *parse_begin();
    std::pair<Node *, Location> parse_exception_handler(Node *exp, bool implicit = false,
                                                        Location begin_location = Location());
    Node *parse_rescue();
    std::vector<Node *> parse_rescue_types();
    Node *parse_while_or_until(NodeKind klass);
    bool call_block_arg_follows();
    CallArgs parse_call_block_arg(std::vector<Node *> args, bool check_paren, const std::vector<Node *> *named_args = nullptr);
    Node *parse_call_block_arg_after_dot(Node *obj);
    Node *parse_class_def(bool is_abstract = false, bool is_struct = false, const Token *doc_token = nullptr);
    void parse_type_vars(bool *has, std::vector<std::string> *type_vars, int *splat_index);
    Node *parse_module_def();
    Node *parse_annotation_def();
    Node *parse_parenthesized_expression();
    Node *parse_fun_literal();
    void check_not_pipe_before_proc_literal_body();
    Node *parse_fun_literal_param();
    Node *parse_fun_pointer();
    Node *parse_delimiter(bool want_skip_space = true);
    std::vector<Node *> combine_interpolation_pieces(std::vector<Piece> &pieces, const DelimiterState &ds);
    std::string combine_pieces(std::vector<Piece> &pieces, const DelimiterState &ds);
    void consume_delimiter(std::vector<Piece> &pieces, DelimiterState &ds, bool &has_interpolation, unsigned &options,
                           Location &end_location);
    unsigned consume_regex_options();
    void consume_heredocs();
    void consume_heredoc(const DelimiterState &ds, Node *node);
    bool needs_heredoc_indent_removed(const DelimiterState &ds) const {
        return ds.is_heredoc() && ds.heredoc_indent >= 0;
    }
    std::vector<Node *> remove_heredoc_indent(std::vector<Piece> &pieces, int indent);
    std::string remove_heredoc_from_line(const std::string &line, int indent, int line_number);
    std::string parse_string_without_interpolation(const std::string &context, bool want_skip_space = true);
    Node *parse_string_literal_without_interpolation(const std::string &context, bool want_skip_space = true);
    Node *parse_string_or_symbol_array(NodeKind klass, const char *elements_type);
    Node *parse_empty_array_literal();
    Node *parse_array_literal();
    Node *parse_hash_or_tuple_literal(bool allow_of = true);
    Node *parse_hash_literal(Node *first_key, Location location, bool allow_of);
    bool named_tuple_start();
    bool string_literal_start() const {
        return token_.type == Tok::DELIMITER_START && token_.delimiter_state.kind == DelimiterKind::STRING;
    }
    Node *parse_tuple(Node *first_exp, Location location);
    Node *new_hash_literal(std::vector<std::pair<Node *, Node *>> entries, int line, int column, Location end_location,
                           bool allow_of = true);
    Node *parse_named_tuple(Location location, const std::string &first_key);
    Node *parse_require();
    Node *parse_case();
    void check_valid_exhaustive_expression(Node *exp);
    void add_when_exp(std::vector<Node *> &when_exps, Node *exp);
    bool when_exp_constant(Node *exp);
    bool when_expression_end();
    Node *parse_when_expression(Node *cond, bool single, bool exhaustive);
    Node *parse_select();
    bool valid_select_when(Node *node);
    Node *parse_include_or_extend(NodeKind klass);
    Node *parse_def(bool is_abstract = false, const Token *doc_token = nullptr);
    void prepare_parse_def();
    Node *parse_macro();
    std::pair<Node *, Location> parse_macro_body(Location start_location, MacroState macro_state = MacroState());
    std::vector<Node *> parse_macro_var_exps();
    bool check_macro_skip_whitespace();
    Node *parse_percent_macro_expression();
    Node *parse_macro_expression();
    void check_macro_expression_end();
    Node *parse_percent_macro_control();
    Node *parse_macro_control(Location start_location, MacroState macro_state = MacroState());
    Node *parse_macro_if(Location start_location, MacroState macro_state, bool check_end = true, bool is_unless = false);
    Node *parse_expression_inside_macro();
    Node *parse_def_helper(bool is_abstract);
    void check_valid_def_name();
    void check_valid_def_op_name();
    std::vector<std::string> parse_def_free_vars();
    void compute_block_arg_yields(Node *block_arg);
    ArgExtras parse_param(std::vector<Node *> &params, std::vector<Node *> *extra_assigns, bool parentheses,
                          bool found_default_value, bool found_splat, Node *found_double_splat, bool allow_restrictions);
    Node *parse_def_block_param(std::vector<Node *> *extra_assigns, bool has_annotations, std::vector<Node *> annotations);
    struct ParamName {
        std::string param_name;
        bool has_external_name = false;
        std::string external_name;
        bool found_space = false;
        bool uses_param = false;
    };
    ParamName parse_param_name(Location location, std::vector<Node *> *extra_assigns, bool allow_external_name);
    static bool invalid_internal_name_kw(Keyword k);
    static bool invalid_internal_name_str(const std::string &s);
    void check_valid_param_name(const Token &token);
    Node *parse_if(bool check_end = true);
    Node *parse_if_after_condition(Node *cond, Location location, bool check_end);
    Node *parse_unless();
    Node *parse_unless_after_condition(Node *cond, Location location);
    Node *set_visibility(Node *node);
    Node *parse_var_or_call(bool global = false, bool force_call = false, std::optional<Location> location = std::nullopt);
    bool next_comes_plus_or_minus();
    Node *parse_block(Node *block, bool stop_on_do = false);
    Node *parse_curly_block(Node *block);
    template <typename F>
    Node *parse_block2(F &&f);
    void parse_block_params(Location location, std::vector<Node *> &block_params, int &splat_index,
                            std::map<int, Node *> &unpacks, bool &has_unpacks);
    struct BlockParam {
        Node *var = nullptr;
        bool found_splat = false;
        bool has_unpack = false;
        std::vector<Node *> unpack_expressions;
    };
    BlockParam parse_block_param(bool found_splat, std::vector<std::string> &all_names);
    void push_block_vars(Node *node);
    std::optional<CallArgs> parse_call_args(bool stop_on_do_after_space = false, bool allow_curly = false,
                                            bool control = false);
    std::optional<CallArgs> parse_call_args_space_consumed(bool check_plus_and_minus = true, bool allow_curly = false,
                                                           bool control = false, Tok end_token = Tok::OP_RPAREN,
                                                           bool allow_beginless_range = false);
    CallArgs parse_call_args_named_args(Location location, std::vector<Node *> args, const std::string *first_name,
                                        bool allow_newline);
    std::vector<Node *> parse_named_args(Location location, const std::string *first_name = nullptr,
                                         bool allow_newline = false);
    Node *parse_call_arg(bool found_double_splat = false);
    Node *parse_out();
    Node *parse_generic_or_global_call();
    Node *parse_bare_proc_type();
    Node *parse_union_type();
    Node *parse_atomic_type_with_suffix();
    Node *parse_atomic_type();
    std::vector<Node *> parse_union_types(Tok end_token, bool allow_splats = false);
    Node *parse_generic(bool expression = false);
    Node *parse_generic(bool global, Location location, bool expression);
    Node *parse_path();
    Node *parse_path(bool global, Location location);
    Node *parse_type_args(Node *name);
    std::vector<Node *> parse_named_type_args(Tok end_token);
    template <typename F>
    Node *parse_type_splat(F &&f);
    Node *parse_type_arg();
    Node *parse_type_suffix(Node *type);
    Node *parse_proc_type_output(const std::vector<Node *> *input_types, Location location);
    Node *make_nilable_type(Node *type);
    Node *make_nilable_expression(Node *type);
    Node *make_pointer_type(Node *type);
    Node *make_static_array_type(Node *type, Node *size);
    Node *make_tuple_type(std::vector<Node *> types);
    Node *make_named_tuple_type(std::vector<Node *> named_args);
    bool type_start(bool consume_newlines);
    bool type_start_here();
    bool type_path_start();
    bool delimiter_or_type_suffix();
    Node *parse_typeof();
    Node *parse_visibility_modifier(Visibility modifier);
    Node *parse_asm();
    std::vector<Node *> parse_asm_operands();
    Node *parse_asm_operand();
    std::vector<std::string> parse_asm_clobbers();
    void parse_asm_options(bool &volatile_, bool &alignstack, bool &intel, bool &can_throw);
    Node *parse_yield_with_scope();
    Node *parse_yield(Node *scope = nullptr, std::optional<Location> location = std::nullopt);
    Node *parse_control_expression(NodeKind klass);
    Node *parse_lib();
    Node *parse_lib_body();
    std::vector<Node *> parse_lib_body_expressions();
    Node *parse_lib_body_exp();
    Node *parse_lib_body_exp_without_location();
    Node *parse_fun_def(bool top_level, bool require_body = false);
    Node *parse_alias();
    Node *parse_pointerof();
    Node *parse_sizeof(NodeKind klass);
    Node *parse_offsetof();
    Node *parse_type_def();
    Node *parse_c_struct_or_union(bool is_union);
    std::vector<Node *> parse_c_struct_or_union_body_expressions();
    void parse_c_struct_or_union_fields(std::vector<Node *> &exps);
    Node *parse_enum_def();
    std::vector<Node *> parse_enum_body_expressions();
    Node *node_and_next_token(Node *node);
    bool end_token();
    bool can_be_assigned(Node *node);
    std::string consume_def_or_macro_name();
    std::pair<bool, Location> consume_def_equals_sign_skip_space();
    std::pair<bool, Location> consume_def_equals_sign();
    template <typename F>
    auto with_isolated_var_scope(bool create_scope, F &&f) -> decltype(f());
    template <typename F>
    auto with_lexical_var_scope(F &&f) -> decltype(f());
    template <typename F>
    auto preserve_stop_on_do(bool new_value, F &&f) -> decltype(f());
    template <typename F>
    auto push_visibility(F &&f) -> decltype(f());
    template <typename F>
    auto open(const std::string &symbol, Location location, F &&f) -> decltype(f());
    void push_var(Node *var);
    void push_var_name(const std::string &name) { var_scopes_.back().insert(name); }
    bool var_in_scope(const std::string &name) const { return var_scopes_.back().count(name) > 0; }
    void check_void_value(Node *exp, Location location);
    void check_void_expression_keyword();
    void check(std::initializer_list<Tok> types);
    void check(Tok type);
    void check_ident(Keyword k);
    std::string check_ident();
    std::string check_const();
    [[noreturn]] void unexpected_token(const std::string &msg = "", const Token *token = nullptr);
    [[noreturn]] void unexpected_token_in_atomic();
    bool is_var(const std::string &name);
    std::string temp_arg_name();
    void warn(Location l, const std::string &msg) {
        warnings.push_back(std::to_string(l.line) + ":" + std::to_string(l.column) + ": " + msg);
    }
    std::string doc_or_empty(bool &has) const {
        has = token_.has_doc;
        return token_.has_doc ? token_.doc : std::string();
    }
};

}  // namespace crystal
