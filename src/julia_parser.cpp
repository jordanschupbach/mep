// The Julia recursive-descent parser for julia_parse.h.
//
// Ported from JuliaSyntax.jl 0.4.10 src/parser.jl (MIT "Expat" licence,
// Copyright (c) 2021 Julia Computing and contributors). Functions keep their
// JuliaSyntax names and order so the two can be read side by side; the
// `# a ==> (tree)` examples of parser.jl are not repeated here.
//
// Diagnostics from 1.0.2: where Julia 1.12's parser (Base.JuliaSyntax 1.0.2)
// reports something different and the difference is only in what is
// reported, not in the tree, the 1.0.2 diagnostic is emitted on top of the
// 0.4.10 tree. Each such place is marked "1.0.2:" below.
//
// Not reproduced, because 1.0.2's tree differs there and its later
// diagnostics follow its own tree (only in damaged code; the 3-mutant corpus
// run of tools/julia_parse_check.py finds nothing else):
//  * `true` / `false` are numbers in 1.0.2 (K"Bool"), so `=true` or
//    `"a""b"true` is not juxtaposed and its recovery moves on;
//  * 1.0.2 stops a bracket list right after a generator and resyncs at the
//    next closing token without counting brackets (`(x for x in a f())`):
//    the first "Expected `)` or `,`" is emitted here, the follow-on
//    "extra tokens" / "premature end of input" etc. are not;
//  * 1.0.2 puts a `do` block inside its call, so a term right after `end`
//    with no space between (`f() do x end"a"`) is juxtaposed there, while
//    0.4.10's `do` node is a block form and ends the expression.
//
// Depth: DepthGuard counts nesting in the recursive functions; past
// kMaxNestingDepth the rest of the input becomes one error node (see
// julia_parse.h).

#include "julia_parse_internal.h"

#include <string>

namespace jlparse {

namespace {

struct ParseState {
    ParseStream *stream;
    // Disable range colon for parsing ternary conditional operator
    bool range_colon_enabled = true;
    // In space-sensitive mode "x -y" is 2 expressions, not a subtraction
    bool space_sensitive = false;
    // Seeing `for` stops parsing macro arguments and makes a generator
    bool for_generator = false;
    // Treat 'end' like a normal symbol instead of a reserved word
    bool end_symbol = false;
    // Treat newline like ordinary whitespace instead of as a potential separator
    bool whitespace_newline = false;
    // Enable parsing `where` with high precedence
    bool where_enabled = true;
};

ParseState normal_context(ParseState ps) {
    ps.range_colon_enabled = true;
    ps.space_sensitive = false;
    ps.where_enabled = true;
    ps.for_generator = false;
    ps.end_symbol = false;
    ps.whitespace_newline = false;
    return ps;
}

ParseState with_space_sensitive(ParseState ps) {
    ps.space_sensitive = true;
    ps.whitespace_newline = false;
    return ps;
}

//------------------------------------------------------------------------------
// Convenient wrappers for ParseStream

constexpr int NL_DEFAULT = -1;  // skip_newlines=nothing: use ps.whitespace_newline

bool skip_nl(const ParseState &ps, int skip_newlines) {
    return skip_newlines == NL_DEFAULT ? ps.whitespace_newline : skip_newlines != 0;
}

Kind peek(const ParseState &ps, size_t n = 1, int skip_newlines = NL_DEFAULT) {
    return ps.stream->peek(n, skip_nl(ps, skip_newlines));
}

SyntaxToken peek_token(const ParseState &ps, size_t n = 1, int skip_newlines = NL_DEFAULT) {
    return ps.stream->peek_token(n, skip_nl(ps, skip_newlines));
}

FullToken peek_full_token(const ParseState &ps, size_t n = 1, int skip_newlines = NL_DEFAULT) {
    return ps.stream->peek_full_token(n, skip_nl(ps, skip_newlines));
}

PeekBehind peek_behind(const ParseState &ps, bool skip_trivia = true, bool skip_parens = true) {
    return ps.stream->peek_behind(skip_trivia, skip_parens);
}

PeekBehind peek_behind(const ParseState &ps, ParseStreamPosition pos) { return ps.stream->peek_behind(pos); }

ParseStreamPosition peek_behind_pos(const ParseState &ps) { return ps.stream->peek_behind_pos(); }

ParseStreamPosition bump(const ParseState &ps, RawFlags flags = EMPTY_FLAGS, int skip_newlines = NL_DEFAULT,
                         const char *error = nullptr, Kind remap_kind = K("None")) {
    return ps.stream->bump(flags, skip_nl(ps, skip_newlines), error, remap_kind);
}

ParseStreamPosition bump_remap(const ParseState &ps, Kind remap_kind, RawFlags flags = EMPTY_FLAGS) {
    return ps.stream->bump(flags, skip_nl(ps, NL_DEFAULT), nullptr, remap_kind);
}

ParseStreamPosition bump_trivia(const ParseState &ps, RawFlags flags = EMPTY_FLAGS, bool skip_newlines = true,
                                const char *error = nullptr) {
    return ps.stream->bump_trivia(flags, skip_newlines, error);
}

ParseStreamPosition bump_invisible(const ParseState &ps, Kind kind, RawFlags flags = EMPTY_FLAGS,
                                   const char *error = nullptr) {
    if (error) {
        std::string e = error;
        return ps.stream->bump_invisible(kind, flags, &e);
    }
    return ps.stream->bump_invisible(kind, flags, nullptr);
}

ParseStreamPosition bump_invisible_s(const ParseState &ps, Kind kind, RawFlags flags, const std::string &error) {
    return ps.stream->bump_invisible(kind, flags, &error);
}

ParseStreamPosition bump_glue(const ParseState &ps, Kind kind, RawFlags flags) {
    return ps.stream->bump_glue(kind, flags);
}

ParseStreamPosition bump_split(const ParseState &ps, std::initializer_list<SplitSpec> spec) {
    return ps.stream->bump_split(spec);
}

void reset_node_kind(const ParseState &ps, ParseStreamPosition pos, Kind kind) {
    ps.stream->reset_node(pos, &kind, nullptr);
}
void reset_node_flags(const ParseState &ps, ParseStreamPosition pos, RawFlags flags) {
    ps.stream->reset_node(pos, nullptr, &flags);
}
void reset_node(const ParseState &ps, ParseStreamPosition pos, Kind kind, RawFlags flags) {
    ps.stream->reset_node(pos, &kind, &flags);
}

bool steal_token_bytes(const ParseState &ps, ParseStreamPosition pos, uint32_t numbytes) {
    return ps.stream->steal_token_bytes(pos, numbytes);
}

ParseStreamPosition position(const ParseState &ps) { return ps.stream->position(); }

ParseStreamPosition emit(const ParseState &ps, ParseStreamPosition mark, Kind kind, RawFlags flags = EMPTY_FLAGS) {
    return ps.stream->emit(mark, kind, flags, nullptr);
}

ParseStreamPosition emit_err(const ParseState &ps, ParseStreamPosition mark, Kind kind, RawFlags flags,
                             const std::string &error) {
    return ps.stream->emit(mark, kind, flags, &error);
}

// emit_diagnostic(ps; error=...) / (ps; whitespace=true, warning=...)
void diag_here(const ParseState &ps, const std::string &msg, bool whitespace = false, Level level = Level::Error) {
    ps.stream->emit_diagnostic_here(whitespace, level, msg);
}
// emit_diagnostic(ps, mark; error/warning=...)
void diag_mark(const ParseState &ps, ParseStreamPosition mark, const std::string &msg, Level level = Level::Error) {
    ps.stream->emit_diagnostic_mark(mark, level, msg);
}
// emit_diagnostic(ps, mark, end_mark; error=...)
void diag_marks(const ParseState &ps, ParseStreamPosition mark, ParseStreamPosition end_mark, const std::string &msg) {
    ps.stream->emit_diagnostic_marks(mark, end_mark, Level::Error, msg);
}

// The diagnostic `emit(ps, mark, K"error", error=msg)` would make, without
// the node: for 1.0.2 errors whose nodes 0.4.10 doesn't have.
void diag_emit_range(const ParseState &ps, ParseStreamPosition mark, const std::string &msg) {
    ParseStream &s = *ps.stream;
    s.emit_diagnostic_range(s.token_first_byte(mark.token_index + 1), s.token_last_byte(s.tokens.size()),
                            Level::Error, msg);
}

ParseStreamPosition first_child_position(const ParseState &ps, ParseStreamPosition pos) {
    return ps.stream->first_child_position(pos);
}

std::string untokenize(Kind k) { return KindName(k); }

//------------------------------------------------------------------------------
// Depth safety

struct DepthGuard {
    ParseStream *s;
    explicit DepthGuard(const ParseState &ps) : s(ps.stream) { s->depth += 1; }
    ~DepthGuard() { s->depth -= 1; }
    DepthGuard(const DepthGuard &) = delete;
    DepthGuard &operator=(const DepthGuard &) = delete;
};

bool depth_exceeded(const ParseState &ps) { return ps.stream->depth > kMaxNestingDepth; }

// Past the nesting cap: everything left becomes one error node. Only called
// at the start of functions which make no assumption about the next token.
void check_depth(const ParseState &ps) {
    ParseStream &s = *ps.stream;
    if (s.depth_exceeded || s.depth <= kMaxNestingDepth) return;
    ParseStreamPosition mark = s.position();
    while (s.peek(1, true) != K("EndMarker")) s.bump(EMPTY_FLAGS, true);
    std::string e = "expression nested too deeply";
    s.emit(mark, K("error"), EMPTY_FLAGS, &e);
    s.depth_exceeded = true;
}

//------------------------------------------------------------------------------
// Parsing-specific predicates on tokens/kinds

bool is_plain_equals(const SyntaxToken &t) { return t.kind == K("=") && !is_decorated(t.flags); }

bool is_closing_token(const ParseState &ps, Kind k) {
    return k == K("else") || k == K("elseif") || k == K("catch") || k == K("finally") || k == K(",") ||
           k == K(")") || k == K("]") || k == K("}") || k == K(";") || k == K("EndMarker") ||
           (k == K("end") && !ps.end_symbol);
}

bool is_block_continuation_keyword(const ParseState &ps, Kind k) {
    return jlparse::is_block_continuation_keyword(k) && !(ps.end_symbol && k == K("end"));
}

bool is_closer_or_newline(const ParseState &ps, Kind k) { return is_closing_token(ps, k) || k == K("NewlineWs"); }

bool is_initial_reserved_word(const ParseState &ps, Kind k) {
    bool is_iresword = k == K("begin") || k == K("while") || k == K("if") || k == K("for") || k == K("try") ||
                       k == K("return") || k == K("break") || k == K("continue") || k == K("function") ||
                       k == K("macro") || k == K("quote") || k == K("let") || k == K("local") ||
                       k == K("global") || k == K("const") || k == K("do") || k == K("struct") ||
                       k == K("module") || k == K("baremodule") || k == K("using") || k == K("import") ||
                       k == K("export");
    return is_iresword && !(k == K("begin") && ps.end_symbol);
}

bool is_reserved_word(Kind k) { return is_keyword(k) && !is_contextual_keyword(k); }

bool peek_initial_reserved_words(const ParseState &ps) {
    Kind k = peek(ps);
    if (is_initial_reserved_word(ps, k)) {
        return true;
    } else if (is_contextual_keyword(k)) {
        Kind k2 = peek(ps, 2, 0);
        return (k == K("mutable") && k2 == K("struct")) || (k == K("primitive") && k2 == K("type")) ||
               (k == K("abstract") && k2 == K("type"));
    }
    return false;
}

bool is_block_form(Kind k) {
    return k == K("block") || k == K("quote") || k == K("if") || k == K("for") || k == K("while") ||
           k == K("let") || k == K("function") || k == K("macro") || k == K("abstract") || k == K("primitive") ||
           k == K("struct") || k == K("try") || k == K("module");
}

bool is_syntactic_operator(Kind k) {
    return k == K("&&") || k == K("||") || k == K(".") || k == K("...") || k == K("->") ||
           (is_prec_assignment(k) && k != K("~"));
}

bool is_syntactic_unary_op(Kind k) { return k == K("$") || k == K("&") || k == K("::"); }

bool is_type_operator(const SyntaxToken &t) {
    return (t.kind == K("<:") || t.kind == K(">:")) && !is_dotted(t.flags);
}

bool is_unary_op(const SyntaxToken &t) {
    Kind k = t.kind;
    return !is_suffixed(t.flags) &&
           (((k == K("<:") || k == K(">:")) && !is_dotted(t.flags)) || k == K("+") || k == K("-") ||
            k == K("!") || k == K("~") || k == K("¬") || k == K("√") || k == K("∛") || k == K("∜") ||
            k == K("⋆") || k == K("±") || k == K("∓"));
}

bool is_both_unary_and_binary(const SyntaxToken &t) {
    Kind k = t.kind;
    return !is_suffixed(t.flags) &&
           (k == K("+") || k == K("-") || k == K("⋆") || k == K("±") || k == K("∓") ||
            ((k == K("$") || k == K("&") || k == K("~")) && !is_dotted(t.flags)));
}

bool is_string_macro_suffix(Kind k) {
    return k == K("Identifier") || is_keyword(k) || is_word_operator(k) || is_number(k);
}

bool is_valid_identifier(Kind k) { return !(is_syntactic_operator(k) || k == K("?") || k == K(".'")); }

// The last expression is a plain `...` command literal: a macrocall in 0.4.10
// (`core_@cmd` + cmdstring), a bare cmdstring in 1.0.2, so 1.0.2 does not
// take it as a macro-call function signature. For 1.0.2's diagnostics.
bool is_cmd_literal(const ParseState &ps) {
    ParseStreamPosition p = peek_behind_pos(ps);
    if (p.range_index == 0 || ps.stream->peek_behind(p).kind != K("macrocall")) return false;
    return ps.stream->peek_behind(first_child_position(ps, p)).kind == K("core_@cmd");
}

// The expression is a call after stripping `where` and `::`
bool was_eventually_call(const ParseState &ps) {
    ParseStream &stream = *ps.stream;
    ParseStreamPosition p = peek_behind_pos(ps);
    while (true) {
        PeekBehind b = stream.peek_behind(p);
        if (b.kind == K("call")) {
            return true;
        } else if (b.kind == K("where") || b.kind == K("parens") ||
                   (b.kind == K("::") && has_flags(b.flags, INFIX_FLAG))) {
            p = first_child_position(ps, p);
        } else {
            return false;
        }
    }
}

ParseStreamPosition bump_dotsplit(const ParseState &ps, RawFlags flags = EMPTY_FLAGS, bool emit_dot_node = false,
                                  Kind remap_kind = K("None")) {
    SyntaxToken t = peek_token(ps);
    ParseStreamPosition pos;
    if (is_dotted(t.flags)) {
        bump_trivia(ps);
        ParseStreamPosition mark = position(ps);
        Kind k = remap_kind != K("None") ? remap_kind : t.kind;
        pos = bump_split(ps, {{1, K("."), TRIVIA_FLAG}, {0, k, flags}});
        if (emit_dot_node) pos = emit(ps, mark, K("."));
    } else {
        if (remap_kind != K("None"))
            pos = bump_remap(ps, remap_kind);
        else
            pos = bump(ps);
    }
    return pos;
}

//------------------------------------------------------------------------------
// Parser Utils

// Bump an expected closing token. If not found, discard unexpected tokens
// until we find it or another closing token.
void bump_closing_token(const ParseState &ps, Kind closing_kind, const char *alternative_closer_hint = nullptr) {
    if (peek(ps) == closing_kind) {
        bump_trivia(ps);
        bump(ps, TRIVIA_FLAG);
        return;
    }
    std::string errmsg = "Expected `" + untokenize(closing_kind) + "`";
    if (alternative_closer_hint) errmsg += alternative_closer_hint;
    // We didn't find the closing token. Read ahead in the stream
    ParseStreamPosition mark = position(ps);
    diag_marks(ps, mark, mark, errmsg);
    while (true) {
        Kind k = peek(ps);
        if (is_closing_token(ps, k) && !(k == K(",") || k == K(";"))) break;
        bump(ps);
    }
    // mark as trivia => ignore in AST.
    emit(ps, mark, K("error"), TRIVIA_FLAG);
    if (peek(ps) == closing_kind) bump(ps, TRIVIA_FLAG);
}

// Read tokens until we find an expected closing token.
template <class F>
void recover(F is_closer, const ParseState &ps, RawFlags flags, ParseStreamPosition mark, const std::string &error) {
    while (true) {
        Kind k = peek(ps);
        if (k == K("EndMarker")) {
            bump_invisible(ps, K("error"), TRIVIA_FLAG, "premature end of input");
            break;
        } else if (is_closer(ps, k)) {
            break;
        }
        bump(ps);
    }
    emit_err(ps, mark, K("error"), flags, error);
}

// Emit an error if the version is less than `min_ver` (never with version 1.12).
void min_supported_version(int major, int minor, const ParseState &ps, ParseStreamPosition mark,
                           const std::string &message) {
    ParseStream &s = *ps.stream;
    if (s.version_major < major || (s.version_major == major && s.version_minor < minor)) {
        std::string msg = message + " not supported in Julia version " + std::to_string(s.version_major) + "." +
                          std::to_string(s.version_minor) + " < " + std::to_string(major) + "." +
                          std::to_string(minor);
        emit_err(ps, mark, K("error"), EMPTY_FLAGS, msg);
    }
}

// flisp: disallow-space
void bump_disallowed_space(const ParseState &ps) {
    if (peek_token(ps).preceding_whitespace)
        bump_trivia(ps, TRIVIA_FLAG, false, "whitespace is not allowed here");
}

void bump_semicolon_trivia(const ParseState &ps) {
    while (true) {
        Kind k = peek(ps);
        if (!(k == K(";") || k == K("NewlineWs"))) break;
        bump(ps, TRIVIA_FLAG);
    }
}

//------------------------------------------------------------------------------
// Forward declarations (parser.jl relies on Julia's late binding)

void parse_toplevel(ParseState ps);
void parse_block(ParseState ps);
template <class F> void parse_block_down(ParseState ps, F down);
template <class F> bool parse_block_inner(ParseState ps, F down);
void parse_stmts(ParseState ps);
void parse_public(ParseState ps);
void parse_docstring(ParseState ps);
void parse_eq(ParseState ps);
void parse_eq_star(ParseState ps);
template <class F> void parse_assignment(ParseState ps, F down);
template <class F> void parse_assignment_with_initial_ex(ParseState ps, ParseStreamPosition mark, F down);
int parse_comma(ParseState ps, bool do_emit = true);
void parse_pair(ParseState ps);
void parse_cond(ParseState ps);
void parse_arrow(ParseState ps);
void parse_or(ParseState ps);
void parse_and(ParseState ps);
void parse_comparison(ParseState ps, bool subtype_comparison = false);
void parse_pipe_lt(ParseState ps);
void parse_pipe_gt(ParseState ps);
void parse_range(ParseState ps);
void parse_invalid_ops(ParseState ps);
void parse_expr(ParseState ps);
void parse_term(ParseState ps);
void parse_rational(ParseState ps);
void parse_shift(ParseState ps);
void parse_unary_subtype(ParseState ps);
void parse_where_chain(ParseState ps0, ParseStreamPosition mark);
template <class F> void parse_where(ParseState ps, F down);
void parse_juxtapose(ParseState ps);
void parse_unary(ParseState ps);
void parse_factor(ParseState ps);
void parse_factor_with_initial_ex(ParseState ps, ParseStreamPosition mark);
void parse_factor_after(ParseState ps);
void parse_decl_with_initial_ex(ParseState ps, ParseStreamPosition mark);
void parse_call(ParseState ps);
void parse_unary_prefix(ParseState ps);
void parse_call_chain(ParseState ps, ParseStreamPosition mark, bool is_macrocall = false);
void parse_subtype_spec(ParseState ps);
void parse_struct_field(ParseState ps);
void parse_resword(ParseState ps);
void parse_if_elseif(ParseState ps, bool is_elseif = false, bool is_elseif_whitespace_err = false);
bool parse_global_local_const_vars(ParseState ps);
bool parse_function_signature(ParseState ps, bool is_function);
void parse_try(ParseState ps);
void parse_catch(ParseState ps);
void parse_do(ParseState ps, ParseStreamPosition mark);
void fix_macro_name_kind(ParseState ps, ParseStreamPosition macro_name_position, Kind name_kind = K("None"));
void parse_macro_name(ParseState ps);
void parse_atsym(ParseState ps, bool allow_quotes = true, bool allow_quotes_102 = true);
void parse_imports(ParseState ps);
bool parse_import(ParseState ps, Kind word, bool has_import_prefix);
void parse_import_path(ParseState ps);
template <class F> int parse_comma_separated(ParseState ps, F down);
void parse_iteration_spec(ParseState ps);
void parse_iteration_specs(ParseState ps);
int parse_space_separated_exprs(ParseState ps);
void parse_call_arglist(ParseState ps, Kind closer);
struct CatResult {
    Kind kind;
    RawFlags flags;
};
CatResult parse_vect(ParseState ps, Kind closer);
void parse_generator(ParseState ps, ParseStreamPosition mark);
CatResult parse_comprehension(ParseState ps, ParseStreamPosition mark, Kind closer);
CatResult parse_array(ParseState ps, ParseStreamPosition mark, Kind closer, bool end_is_symbol);
CatResult parse_cat(ParseState ps, Kind closer, bool end_is_symbol);
void parse_paren(ParseState ps, bool check_identifiers = true);
void parse_string(ParseState ps, bool raw);
void emit_braces(ParseState ps, ParseStreamPosition mark, Kind ckind, RawFlags cflags);
void parse_atom(ParseState ps, bool check_identifiers = true);

// The options returned by parse_brackets' `after_parse` callback: a union of
// the fields the various callers use.
struct BracketOpts {
    bool needs_parameters = false;
    bool is_paren_call = false;
    bool is_block = false;
    bool is_tuple = false;
    bool is_anon_func = false;
    bool parsed_call = false;
    bool needs_parse_call = false;
    bool simple_interp = false;
    bool maybe_grouping_parens = false;  // 1.0.2, for its diagnostics
};
template <class F>
BracketOpts parse_brackets(F after_parse, ParseState ps, Kind closing_kind, bool generator_is_last = true);

//------------------------------------------------------------------------------
// Parser

// parse left-to-right binary operator
template <class D, class P> void parse_LtoR(ParseState ps, D down, P is_op) {
    ParseStreamPosition mark = position(ps);
    down(ps);
    while (is_op(peek(ps))) {
        SyntaxToken t = peek_token(ps);
        bump_dotsplit(ps);
        down(ps);
        emit(ps, mark, is_dotted(t.flags) ? K("dotcall") : K("call"), INFIX_FLAG);
    }
}

// parse right-to-left binary operator
template <class D, class P, class S> void parse_RtoL(ParseState ps, D down, P is_op, S self) {
    DepthGuard guard(ps);
    check_depth(ps);
    ParseStreamPosition mark = position(ps);
    down(ps);
    SyntaxToken t = peek_token(ps);
    if (is_op(t.kind)) {
        bump_dotsplit(ps);
        self(ps);
        emit(ps, mark, is_dotted(t.flags) ? K("dotcall") : K("call"), INFIX_FLAG);
    }
}

// parse block-like structures
template <class D, class Del, class Clo> bool parse_Nary(ParseState ps, D down, Del is_delim, Clo is_closing) {
    bump_trivia(ps);
    Kind k = peek(ps);
    if (is_closing(k)) return true;
    int n_delims = 0;
    if (is_delim(k)) {
        // allow leading delimiters
    } else {
        down(ps);
    }
    while (is_delim(peek(ps))) {
        bump(ps, TRIVIA_FLAG);
        n_delims += 1;
        k = peek(ps);
        if (k == K("EndMarker") || is_closing(k)) {
            break;
        } else if (is_delim(k)) {
            // ignore empty delimited sections
            continue;
        }
        down(ps);
    }
    return n_delims != 0;
}

void parse_toplevel(ParseState ps) {
    ParseStreamPosition mark = position(ps);
    while (true) {
        if (peek(ps, 1, 1) == K("EndMarker")) {
            bump_trivia(ps);
            break;
        } else {
            parse_stmts(ps);
        }
    }
    emit(ps, mark, K("toplevel"));
}

template <class F> void parse_block_down(ParseState ps, F down) {
    ParseStreamPosition mark = position(ps);
    parse_block_inner(ps, down);
    emit(ps, mark, K("block"));
}

void parse_block(ParseState ps) { parse_block_down(ps, parse_eq); }

template <class F> bool parse_block_inner(ParseState ps, F down) {
    return parse_Nary(
        ps, down, [](Kind k) { return k == K("NewlineWs") || k == K(";"); },
        [](Kind k) {
            return k == K("end") || k == K("else") || k == K("elseif") || k == K("catch") || k == K("finally");
        });
}

void parse_stmts(ParseState ps) {
    ParseStreamPosition mark = position(ps);
    bool do_emit = parse_Nary(
        ps, parse_public, [](Kind k) { return k == K(";"); }, [](Kind k) { return k == K("NewlineWs"); });
    // check for unparsed junk after an expression
    ParseStreamPosition junk_mark = position(ps);
    while (true) {
        Kind k = peek(ps);
        if (k == K("EndMarker") || k == K("NewlineWs")) break;
        bump(ps);
    }
    if (junk_mark != position(ps))
        emit_err(ps, junk_mark, K("error"), TRIVIA_FLAG, "extra tokens after end of expression");
    if (do_emit) emit(ps, mark, K("toplevel"), TOPLEVEL_SEMICOLONS_FLAG);
}

void parse_public(ParseState ps) {
    if ((ps.stream->version_major > 1 || ps.stream->version_minor >= 11) && peek(ps) == K("public")) {
        Kind k2 = peek(ps, 2);
        if (k2 == K("(") || k2 == K("=") || k2 == K("[")) {
            diag_here(ps, "using public as an identifier is deprecated", false, Level::Warning);
        } else {
            parse_resword(ps);
            return;
        }
    }
    parse_docstring(ps);
}

template <class F> void parse_docstring_down(ParseState ps, F down) {
    ParseStreamPosition mark = position(ps);
    down(ps);
    if (peek_behind(ps).kind == K("string")) {
        bool is_doc = true;
        Kind k = peek(ps);
        if (is_closing_token(ps, k)) {
            is_doc = false;
        } else if (k == K("NewlineWs")) {
            Kind k2 = peek(ps, 2);
            if (is_closing_token(ps, k2) || k2 == K("NewlineWs")) {
                is_doc = false;
            } else {
                bump(ps, TRIVIA_FLAG);  // NewlineWs
            }
        }
        if (is_doc) {
            down(ps);
            emit(ps, mark, K("doc"));
        }
    }
}

void parse_docstring(ParseState ps) { parse_docstring_down(ps, parse_eq); }

void parse_eq(ParseState ps) { parse_assignment(ps, [](ParseState p) { parse_comma(p); }); }

void parse_eq_star(ParseState ps) {
    Kind k = peek(ps);
    Kind k2 = peek(ps, 2);
    if ((is_literal(k) || k == K("Identifier")) &&
        (k2 == K(",") || k2 == K(")") || k2 == K("}") || k2 == K("]"))) {
        bump(ps);
    } else {
        parse_assignment(ps, parse_pair);
    }
}

template <class F> void parse_assignment(ParseState ps, F down) {
    DepthGuard guard(ps);
    check_depth(ps);
    ParseStreamPosition mark = position(ps);
    down(ps);
    parse_assignment_with_initial_ex(ps, mark, down);
}

template <class F> void parse_assignment_with_initial_ex(ParseState ps, ParseStreamPosition mark, F down) {
    SyntaxToken t = peek_token(ps);
    Kind k = t.kind;
    if (!is_prec_assignment(k)) return;
    if (k == K("~")) {
        if (ps.space_sensitive && t.preceding_whitespace && !peek_token(ps, 2).preceding_whitespace) {
            // Unary ~ in space sensitive context is not assignment precedence
            return;
        }
        bump_dotsplit(ps);
        bump_trivia(ps);
        parse_assignment(ps, down);
        emit(ps, mark, is_dotted(t.flags) ? K("dotcall") : K("call"), INFIX_FLAG);
    } else {
        bump(ps, TRIVIA_FLAG);
        bump_trivia(ps);
        parse_assignment(ps, down);
        emit(ps, mark, k, t.flags);
    }
}

int parse_comma(ParseState ps, bool do_emit) {
    ParseStreamPosition mark = position(ps);
    int n_commas = 0;
    parse_pair(ps);
    while (true) {
        if (peek(ps) != K(",")) {
            if (do_emit && n_commas >= 1) emit(ps, mark, K("tuple"));
            return n_commas;
        }
        bump(ps, TRIVIA_FLAG);
        n_commas += 1;
        if (is_plain_equals(peek_token(ps))) {
            // Allow trailing comma before `=`
            continue;
        }
        parse_pair(ps);
    }
}

void parse_pair(ParseState ps) {
    parse_RtoL(ps, parse_cond, [](Kind k) { return is_prec_pair(k); }, parse_pair);
}

void parse_cond(ParseState ps) {
    DepthGuard guard(ps);
    check_depth(ps);
    ParseStreamPosition mark = position(ps);
    parse_arrow(ps);
    SyntaxToken t = peek_token(ps);
    if (t.kind != K("?")) return;
    if (!t.preceding_whitespace)
        bump_invisible(ps, K("error"), TRIVIA_FLAG, "space required before `?` operator");
    bump(ps, TRIVIA_FLAG);  // ?
    t = peek_token(ps);
    if (!t.preceding_whitespace)
        bump_invisible(ps, K("error"), TRIVIA_FLAG, "space required after `?` operator");
    {
        ParseState ps2 = ps;
        ps2.range_colon_enabled = false;
        parse_eq_star(ps2);
    }
    t = peek_token(ps);
    if (!t.preceding_whitespace)
        bump_invisible(ps, K("error"), TRIVIA_FLAG, "space required before `:` in `?` expression");
    if (t.kind == K(":")) {
        bump(ps, TRIVIA_FLAG);
    } else {
        bump_invisible(ps, K("error"), TRIVIA_FLAG, "`:` expected in `?` expression");
    }
    t = peek_token(ps, 1, 1);
    if (!t.preceding_whitespace)
        bump_invisible(ps, K("error"), TRIVIA_FLAG, "space required after `:` in `?` expression");
    if (is_block_continuation_keyword(ps, t.kind)) {
        bump_invisible_s(ps, K("error"), TRIVIA_FLAG, "unexpected `" + std::string(KindName(t.kind)) + "`");
        emit(ps, mark, K("if"));
        return;
    }
    parse_eq_star(ps);
    emit(ps, mark, K("?"));
}

void parse_arrow(ParseState ps) {
    DepthGuard guard(ps);
    check_depth(ps);
    ParseStreamPosition mark = position(ps);
    parse_or(ps);
    SyntaxToken t = peek_token(ps);
    Kind k = t.kind;
    if (is_prec_arrow(k)) {
        if (t.kind == K("-->") && !is_decorated(t.flags)) {
            bump(ps, TRIVIA_FLAG);
            parse_arrow(ps);
            emit(ps, mark, k, t.flags);
        } else {
            bump_dotsplit(ps);
            parse_arrow(ps);
            emit(ps, mark, is_dotted(t.flags) ? K("dotcall") : K("call"), INFIX_FLAG);
        }
    }
}

template <class D, class P, class S> void parse_lazy_cond(ParseState ps, D down, P is_op, S self) {
    DepthGuard guard(ps);
    check_depth(ps);
    ParseStreamPosition mark = position(ps);
    down(ps);
    SyntaxToken t = peek_token(ps);
    Kind k = t.kind;
    if (is_op(k)) {
        bump(ps, TRIVIA_FLAG);
        self(ps);
        emit(ps, mark, k, t.flags);
        if (is_dotted(t.flags)) min_supported_version(1, 7, ps, mark, "dotted operators `.||` and `.&&`");
    }
}

void parse_or(ParseState ps) {
    parse_lazy_cond(ps, parse_and, [](Kind k) { return is_prec_lazy_or(k); }, parse_or);
}

void parse_and(ParseState ps) {
    parse_lazy_cond(ps, [](ParseState p) { parse_comparison(p); }, [](Kind k) { return is_prec_lazy_and(k); },
                    parse_and);
}

void parse_comparison(ParseState ps, bool subtype_comparison) {
    ParseStreamPosition mark = position(ps);
    if (subtype_comparison && is_reserved_word(peek(ps))) {
        // Recovery
        std::string name = untokenize(peek(ps));
        bump(ps);
        emit_err(ps, mark, K("error"), EMPTY_FLAGS, "Invalid type name `" + name + "`");
    } else {
        parse_pipe_lt(ps);
    }
    int n_comparisons = 0;
    ParseStreamPosition op_pos = NO_POSITION;
    bool op_dotted = false;
    SyntaxToken initial_tok = peek_token(ps);
    while (true) {
        SyntaxToken t = peek_token(ps);
        if (!is_prec_comparison(t.kind)) break;
        n_comparisons += 1;
        op_dotted = is_dotted(t.flags);
        op_pos = bump_dotsplit(ps, EMPTY_FLAGS, true);
        parse_pipe_lt(ps);
    }
    if (n_comparisons == 1) {
        if (is_type_operator(initial_tok)) {
            reset_node_flags(ps, op_pos, TRIVIA_FLAG);
            emit(ps, mark, initial_tok.kind);
        } else {
            if (op_dotted) reset_node(ps, op_pos, K("TOMBSTONE"), TRIVIA_FLAG);
            emit(ps, mark, is_dotted(initial_tok.flags) ? K("dotcall") : K("call"), INFIX_FLAG);
        }
    } else if (n_comparisons > 1) {
        emit(ps, mark, K("comparison"));
    }
}

void parse_pipe_lt(ParseState ps) {
    parse_RtoL(ps, parse_pipe_gt, [](Kind k) { return is_prec_pipe_lt(k); }, parse_pipe_lt);
}

void parse_pipe_gt(ParseState ps) {
    parse_LtoR(ps, parse_range, [](Kind k) { return is_prec_pipe_gt(k); });
}

void parse_range(ParseState ps) {
    ParseStreamPosition mark = position(ps);
    parse_invalid_ops(ps);
    SyntaxToken initial_tok = peek_token(ps);
    Kind initial_kind = initial_tok.kind;
    if (initial_kind != K(":") && is_prec_colon(initial_kind)) {
        bump_dotsplit(ps);
        parse_invalid_ops(ps);
        emit(ps, mark, is_dotted(initial_tok.flags) ? K("dotcall") : K("call"), INFIX_FLAG);
    } else if (initial_kind == K(":") && ps.range_colon_enabled) {
        int n_colons = 0;
        while (peek(ps) == K(":")) {
            if (ps.space_sensitive && peek_token(ps).preceding_whitespace &&
                !peek_token(ps, 2).preceding_whitespace) {
                // Tricky cases in space sensitive mode
                break;
            }
            SyntaxToken t2 = peek_token(ps, 2);
            if ((t2.kind == K("<") || t2.kind == K(">")) && !t2.preceding_whitespace) {
                // Error heuristic: `:>` or `:<`
                bump_trivia(ps, EMPTY_FLAGS, false);
                ParseStreamPosition emark = position(ps);
                bump(ps);  // K":"
                std::string ks = untokenize(peek(ps));
                bump(ps);  // K"<" or K">"
                emit_err(ps, emark, K("error"), EMPTY_FLAGS,
                         "Invalid `:" + ks + "` found, maybe replace with `" + ks + ":`");
                parse_invalid_ops(ps);
                emit(ps, mark, K("call"), INFIX_FLAG);
                break;
            }
            n_colons += 1;
            bump(ps, n_colons == 1 ? EMPTY_FLAGS : TRIVIA_FLAG);
            bool had_newline = peek(ps) == K("NewlineWs");
            SyntaxToken t = peek_token(ps);
            if (is_closing_token(ps, t.kind)) {
                bump_invisible(ps, K("error"), EMPTY_FLAGS, "missing last argument in range expression");
                emit(ps, mark, K("call"), INFIX_FLAG);
                diag_here(ps, "found unexpected closing token");
                return;
            }
            if (had_newline) {
                // Error message for people coming from python
                diag_here(ps, "line break after `:` in range expression", true);
                bump_invisible(ps, K("error"));
                emit(ps, mark, K("call"), INFIX_FLAG);
                return;
            }
            parse_invalid_ops(ps);
            if (n_colons == 2) {
                emit(ps, mark, K("call"), INFIX_FLAG);
                n_colons = 0;
            }
        }
        if (n_colons > 0) emit(ps, mark, K("call"), INFIX_FLAG);
    }

    if (peek(ps) == K("...")) {
        bump(ps, TRIVIA_FLAG);
        emit(ps, mark, K("..."));
    }
}

void parse_invalid_ops(ParseState ps) {
    ParseStreamPosition mark = position(ps);
    parse_expr(ps);
    while (true) {
        SyntaxToken t = peek_token(ps);
        if (!(t.kind == K("ErrorInvalidOperator") || t.kind == K("Error**"))) break;
        bump_trivia(ps);
        bump_dotsplit(ps);
        parse_expr(ps);
        emit(ps, mark, is_dotted(t.flags) ? K("dotcall") : K("call"), INFIX_FLAG);
    }
}

// parse left to right chains of a given binary operator
template <class D> void parse_chain(ParseState ps, D down, Kind op_kind) {
    while (true) {
        SyntaxToken t = peek_token(ps);
        if (!(t.kind == op_kind && !is_decorated(t.flags))) break;
        if (ps.space_sensitive && t.preceding_whitespace && is_both_unary_and_binary(t) &&
            !peek_token(ps, 2).preceding_whitespace) {
            break;
        }
        bump(ps, TRIVIA_FLAG);
        down(ps);
    }
}

// Parse left to right, combining any of `chain_ops` into one call
template <class D, class P, class Ch> void parse_with_chains(ParseState ps, D down, P is_op, Ch is_chain_op) {
    ParseStreamPosition mark = position(ps);
    down(ps);
    while (true) {
        SyntaxToken t = peek_token(ps);
        if (!is_op(t.kind)) break;
        if (ps.space_sensitive && t.preceding_whitespace && is_both_unary_and_binary(t) &&
            !peek_token(ps, 2).preceding_whitespace) {
            // The following is two elements of a hcat
            break;
        }
        bump_dotsplit(ps);
        down(ps);
        if (is_chain_op(t.kind) && !is_decorated(t.flags)) parse_chain(ps, down, t.kind);
        emit(ps, mark, is_dotted(t.flags) ? K("dotcall") : K("call"), INFIX_FLAG);
    }
}

void parse_expr(ParseState ps) {
    parse_with_chains(ps, parse_term, [](Kind k) { return is_prec_plus(k); },
                      [](Kind k) { return k == K("+") || k == K("++"); });
}

void parse_term(ParseState ps) {
    parse_with_chains(ps, parse_rational, [](Kind k) { return is_prec_times(k); },
                      [](Kind k) { return k == K("*"); });
}

void parse_rational(ParseState ps) {
    parse_LtoR(ps, parse_shift, [](Kind k) { return is_prec_rational(k); });
}

void parse_shift(ParseState ps) {
    parse_LtoR(ps, parse_unary_subtype, [](Kind k) { return is_prec_bitshift(k); });
}

// parse `<: A where B` as `<: (A where B)` (issue #21545)
void parse_unary_subtype(ParseState ps) {
    DepthGuard guard(ps);
    check_depth(ps);
    SyntaxToken t = peek_token(ps);
    if (is_type_operator(t)) {
        Kind k2 = peek(ps, 2);
        if (is_closing_token(ps, k2) || k2 == K("NewlineWs") || k2 == K("=")) {
            // return operator by itself
            bump(ps);
        } else if (k2 == K("{") || k2 == K("(")) {
            // parse <:{T}(x::T) or <:(x::T) like other unary operators
            parse_where(ps, parse_juxtapose);
        } else {
            ParseStreamPosition mark = position(ps);
            bump(ps, TRIVIA_FLAG);
            parse_unary_subtype(ps);
            emit(ps, mark, t.kind, PREFIX_OP_FLAG);
        }
    } else {
        parse_where(ps, parse_juxtapose);
    }
}

void parse_where_chain(ParseState ps0, ParseStreamPosition mark) {
    ParseState ps = ps0;
    ps.where_enabled = false;
    while (peek(ps) == K("where")) {
        bump(ps, TRIVIA_FLAG);  // where
        bump_trivia(ps);
        Kind k = peek(ps);
        if (k == K("{")) {
            ParseStreamPosition m = position(ps);
            bump(ps, TRIVIA_FLAG);
            CatResult c = parse_cat(ps, K("}"), ps.end_symbol);
            emit_braces(ps, m, c.kind, c.flags);
            emit(ps, mark, K("where"));
        } else {
            parse_comparison(ps);
            emit(ps, mark, K("where"));
        }
    }
}

template <class F> void parse_where(ParseState ps, F down) {
    ParseStreamPosition mark = position(ps);
    down(ps);
    if (ps.where_enabled && peek(ps) == K("where")) parse_where_chain(ps, mark);
}

// Juxtaposition.
void parse_juxtapose(ParseState ps) {
    ParseStreamPosition mark = position(ps);
    parse_unary(ps);
    int n_terms = 1;
    while (true) {
        SyntaxToken t = peek_token(ps);
        Kind k = t.kind;
        Kind prev_k = peek_behind(ps).kind;
        bool is_juxtapose = false;
        if (!t.preceding_whitespace &&
            (is_number(prev_k) ||
             (!is_number(k) && k != K("@") &&
              !(is_block_form(prev_k) || is_syntactic_unary_op(prev_k) || is_initial_reserved_word(ps, prev_k)))) &&
            (!is_operator(k) || is_radical_op(k)) && !is_closing_token(ps, k)) {
            if (prev_k == K("string") || is_string_delim(t.kind)) {
                bump_invisible(ps, K("error"), TRIVIA_FLAG, "cannot juxtapose string literal");
                is_juxtapose = true;
            } else if (!is_initial_reserved_word(ps, k)) {
                is_juxtapose = true;
            }
        }
        if (!is_juxtapose) break;
        if (is_radical_op(t.kind))
            parse_unary(ps);
        else
            parse_factor(ps);
        n_terms += 1;
    }
    if (n_terms > 1) emit(ps, mark, K("juxtapose"));
}

// Parse numeric literal prefixes, calls to unary operators and prefix
// calls involving arbitrary operators with bracketed arglists.
void parse_unary(ParseState ps) {
    DepthGuard guard(ps);
    check_depth(ps);
    ParseStreamPosition mark = position(ps);
    bump_trivia(ps);
    SyntaxToken op_t = peek_token(ps);
    Kind op_k = op_t.kind;
    if (!is_operator(op_k) || is_word_operator(op_k) || (op_k == K(":") || op_k == K("'") || op_k == K(".'")) ||
        (is_syntactic_unary_op(op_k) && !is_dotted(op_t.flags)) || is_syntactic_operator(op_k)) {
        // `op_t` is not an initial operator
        parse_factor(ps);
        return;
    }
    SyntaxToken t2 = peek_token(ps, 2);
    Kind k2 = t2.kind;
    if ((op_k == K("-") || op_k == K("+")) && !is_decorated(op_t.flags)) {
        if (!t2.preceding_whitespace &&
            (k2 == K("Integer") || k2 == K("Float") || k2 == K("Float32") ||
             (op_k == K("+") && (k2 == K("BinInt") || k2 == K("HexInt") || k2 == K("OctInt"))))) {
            Kind k3 = peek(ps, 3);
            if (is_prec_power(k3) || k3 == K("[") || k3 == K("{")) {
                bump(ps);
                parse_factor(ps);
                emit(ps, mark, K("call"), PREFIX_OP_FLAG);
            } else {
                // signed numeric literal
                bump_glue(ps, t2.kind, EMPTY_FLAGS);
                parse_factor_with_initial_ex(ps, mark);
            }
            return;
        }
    }
    if (is_closing_token(ps, k2) || k2 == K("NewlineWs") || k2 == K("=")) {
        // Standalone operators parsed as `op` or `(. op)`
        parse_atom(ps);
    } else if (k2 == K("{") || (!is_unary_op(op_t) && k2 == K("("))) {
        // Call with type parameters or non-unary prefix call
        parse_factor(ps);
    } else if (k2 == K("(")) {
        ParseStreamPosition op_pos = bump_dotsplit(ps, EMPTY_FLAGS, true);

        bool space_before_paren = t2.preceding_whitespace;
        ParseStreamPosition ws_mark{}, ws_error_pos{}, ws_mark_end{};
        if (space_before_paren) {
            // Setup possible whitespace error between operator and (
            ws_mark = position(ps);
            bump_trivia(ps);
            ws_error_pos = emit(ps, ws_mark, K("TOMBSTONE"));
            ws_mark_end = position(ps);
        }

        ParseStreamPosition mark_before_paren = position(ps);
        bump(ps, TRIVIA_FLAG);  // (
        bool initial_semi = peek(ps, 1, 1) == K(";");
        BracketOpts opts = parse_brackets(
            [&](bool had_commas, bool had_splat, int num_semis, int num_subexprs) {
                bool is_paren_call = had_commas || had_splat || (initial_semi && num_subexprs > 0) ||
                                     (initial_semi && num_semis == 1) || (num_semis == 0 && num_subexprs == 0);
                BracketOpts o;
                o.needs_parameters = is_paren_call;
                o.is_paren_call = is_paren_call;
                o.is_block = !is_paren_call && num_semis > 0;
                return o;
            },
            ps, K(")"));

        if (opts.is_paren_call) {
            if (space_before_paren) {
                // Whitespace not allowed before prefix function call bracket
                reset_node_kind(ps, ws_error_pos, K("error"));
                diag_marks(ps, ws_mark, ws_mark_end,
                           "whitespace not allowed between prefix function call and argument list");
            }
            if (is_type_operator(op_t)) {
                emit(ps, mark, op_k);
                reset_node_flags(ps, op_pos, TRIVIA_FLAG);
            } else {
                emit(ps, mark, K("call"));
            }
            parse_call_chain(ps, mark);
            parse_factor_with_initial_ex(ps, mark);
        } else {
            // Unary function calls with brackets as grouping, not an arglist
            if (opts.is_block)
                emit(ps, mark_before_paren, K("block"), PARENS_FLAG);
            else
                emit(ps, mark_before_paren, K("parens"));
            parse_call_chain(ps, mark_before_paren);
            parse_factor_with_initial_ex(ps, mark_before_paren);
            if (is_type_operator(op_t)) {
                emit(ps, mark, op_k, PREFIX_OP_FLAG);
                reset_node_flags(ps, op_pos, TRIVIA_FLAG);
            } else {
                if (is_dotted(op_t.flags)) {
                    emit(ps, mark, K("dotcall"), PREFIX_OP_FLAG);
                    reset_node_kind(ps, op_pos, K("TOMBSTONE"));
                } else {
                    emit(ps, mark, K("call"), PREFIX_OP_FLAG);
                }
            }
        }
    } else {
        ParseStreamPosition op_pos;
        if (is_unary_op(op_t)) {
            // Normal unary calls
            op_pos = bump_dotsplit(ps, EMPTY_FLAGS);
        } else {
            bump_dotsplit(ps, EMPTY_FLAGS, true);
            // 1.0.2 bumps this operator remapped to Identifier, so an invalid
            // operator token (`--x`, `**x`) gets no diagnostic of its own.
            if (op_t.kind == K("ErrorInvalidOperator") || op_t.kind == K("Error**"))
                ps.stream->remapped_102.push_back(ps.stream->tokens.size());
            op_pos = emit_err(ps, mark, K("error"), EMPTY_FLAGS, "not a unary operator");
        }
        parse_unary(ps);
        if (is_type_operator(op_t)) {
            reset_node_flags(ps, op_pos, TRIVIA_FLAG);
            emit(ps, mark, op_k, PREFIX_OP_FLAG);
        } else {
            emit(ps, mark, is_dotted(op_t.flags) ? K("dotcall") : K("call"), PREFIX_OP_FLAG);
        }
    }
}

// handle ^ and .^
void parse_factor(ParseState ps) {
    ParseStreamPosition mark = position(ps);
    parse_call(ps);
    parse_factor_with_initial_ex(ps, mark);
}

void parse_factor_with_initial_ex(ParseState ps, ParseStreamPosition mark) {
    parse_decl_with_initial_ex(ps, mark);
    SyntaxToken t = peek_token(ps);
    if (is_prec_power(t.kind)) {
        bump_dotsplit(ps);
        parse_factor_after(ps);
        emit(ps, mark, is_dotted(t.flags) ? K("dotcall") : K("call"), INFIX_FLAG);
    }
}

void parse_factor_after(ParseState ps) {
    parse_RtoL(ps, parse_juxtapose, [](Kind k) { return is_prec_power(k); }, parse_factor_after);
}

// Parse type declarations and lambda syntax
void parse_decl_with_initial_ex(ParseState ps, ParseStreamPosition mark) {
    while (peek(ps) == K("::")) {
        bump(ps, TRIVIA_FLAG);
        parse_where(ps, parse_call);
        emit(ps, mark, K("::"), INFIX_FLAG);
    }
    if (peek(ps) == K("->")) {
        bump(ps, TRIVIA_FLAG);
        // -> is unusual: it binds tightly on the left and loosely on the right.
        parse_eq_star(ps);
        emit(ps, mark, K("->"));
    }
}

// parse function call, indexing, dot, and transpose expressions
void parse_call(ParseState ps) {
    if (peek_initial_reserved_words(ps)) {
        parse_resword(ps);
    } else {
        ParseStreamPosition mark = position(ps);
        parse_unary_prefix(ps);
        parse_call_chain(ps, mark);
    }
}

// parse syntactic unary operators
void parse_unary_prefix(ParseState ps) {
    DepthGuard guard(ps);
    check_depth(ps);
    ParseStreamPosition mark = position(ps);
    SyntaxToken t = peek_token(ps);
    Kind k = t.kind;
    if (is_syntactic_unary_op(k) && !is_dotted(t.flags)) {
        Kind k2 = peek(ps, 2);
        if ((k == K("&") || k == K("$")) && (is_closing_token(ps, k2) || k2 == K("NewlineWs"))) {
            bump(ps);
        } else {
            bump(ps, TRIVIA_FLAG);
            if (k == K("&") || k == K("::")) {
                parse_where(ps, parse_call);
            } else {
                parse_unary_prefix(ps);
            }
            // Only need PREFIX_OP_FLAG for ::
            RawFlags f = k == K("::") ? PREFIX_OP_FLAG : EMPTY_FLAGS;
            emit(ps, mark, k, f);
        }
    } else {
        parse_atom(ps);
    }
}

// Parses a chain of suffixes at function call precedence, leftmost binding
// tightest: calls, indexing, field access, adjoint, string macros.
void parse_call_chain(ParseState ps, ParseStreamPosition mark, bool is_macrocall) {
    if (is_number(peek_behind(ps).kind) && peek(ps) == K("(")) {
        // juxtaposition with numbers is multiply, not call
        return;
    }
    // source range of the @-prefixed part of a macro
    bool have_macro_atname_range = false;
    ParseStreamPosition macro_atname_range[2] = {};
    bool maybe_strmac = true;
    // We record the last component of chains of dot-separated identifiers so we
    // know which identifier was the macro name.
    ParseStreamPosition macro_name_position = position(ps);
    while (true) {
        bool maybe_strmac_1 = false;
        SyntaxToken t = peek_token(ps);
        Kind k = t.kind;
        if (!is_macrocall && ps.space_sensitive && t.preceding_whitespace &&
            (k == K("(") || k == K("[") || k == K("{") || k == K("\"") || k == K("\"\"\"") || k == K("`") ||
             k == K("```"))) {
            break;
        } else if (is_macrocall && (t.preceding_whitespace || !(k == K("(") || k == K("[") || k == K("{") ||
                                                                  k == K("'") || k == K(".")))) {
            // Macro calls with space-separated arguments
            fix_macro_name_kind(ps, macro_name_position);
            {
                ParseState ps2 = with_space_sensitive(ps);
                int n_args = parse_space_separated_exprs(ps2);
                bool is_doc_macro = peek_behind(ps2, macro_name_position).orig_kind == K("doc");
                if (is_doc_macro && n_args == 1) {
                    // Parse extended @doc args on next line
                    Kind k2 = peek(ps2, 2);
                    if (peek(ps2) == K("NewlineWs") && !is_closing_token(ps2, k2) && k2 != K("NewlineWs")) {
                        bump(ps2);  // newline
                        parse_eq(ps2);
                    }
                }
                emit(ps2, mark, K("macrocall"));
            }
            break;
        } else if (k == K("(")) {
            bump_disallowed_space(ps);
            bump(ps, TRIVIA_FLAG);
            parse_call_arglist(ps, K(")"));
            emit(ps, mark, is_macrocall ? K("macrocall") : K("call"), is_macrocall ? PARENS_FLAG : EMPTY_FLAGS);
            if (peek(ps) == K("do")) parse_do(ps, mark);
            if (is_macrocall) {
                fix_macro_name_kind(ps, macro_name_position);
                is_macrocall = false;
                have_macro_atname_range = false;
            }
        } else if (k == K("[")) {
            ParseStreamPosition m = position(ps);
            bump_disallowed_space(ps);
            bump(ps, TRIVIA_FLAG);
            ParseState ps2 = ps;
            ps2.end_symbol = true;
            CatResult c = parse_cat(ps2, K("]"), ps.end_symbol);
            if (is_macrocall) {
                fix_macro_name_kind(ps, macro_name_position);
                emit(ps, m, c.kind, c.flags);
                if (c.kind == K("ncat")) min_supported_version(1, 7, ps, m, "multidimensional array syntax");
                emit(ps, mark, K("macrocall"));
                is_macrocall = false;
                have_macro_atname_range = false;
            } else {
                Kind outk = c.kind == K("vect")            ? K("ref")
                            : c.kind == K("hcat")          ? K("typed_hcat")
                            : c.kind == K("vcat")          ? K("typed_vcat")
                            : c.kind == K("comprehension") ? K("typed_comprehension")
                            : c.kind == K("ncat")          ? K("typed_ncat")
                                                           : K("error");
                emit(ps, mark, outk, c.flags);
                if (c.kind == K("ncat")) min_supported_version(1, 7, ps, mark, "multidimensional array syntax");
            }
        } else if (k == K(".")) {
            bump_disallowed_space(ps);
            ParseStreamPosition emark = position(ps);
            if (have_macro_atname_range) {
                // Allow `@` in macrocall only in first and last position
                diag_marks(ps, macro_atname_range[0], macro_atname_range[1],
                           "`@` must appear on first or last macro name component");
                bump(ps, TRIVIA_FLAG, NL_DEFAULT, "Unexpected `.` after macro name");
            } else {
                bump(ps, TRIVIA_FLAG);
            }
            k = peek(ps);
            if (k == K("(")) {
                if (is_macrocall) {
                    bump_invisible(ps, K("error"), TRIVIA_FLAG);
                    diag_mark(ps, mark, "dot call syntax not supported for macros");
                }
                bump_disallowed_space(ps);
                bump(ps, TRIVIA_FLAG);
                parse_call_arglist(ps, K(")"));
                emit(ps, mark, K("dotcall"));
            } else if (k == K(":")) {
                ParseStreamPosition m = position(ps);
                bump(ps, TRIVIA_FLAG);
                bump_disallowed_space(ps);
                parse_atom(ps, false);
                emit(ps, m, K("quote"), COLON_QUOTE);
                emit(ps, mark, K("."));
            } else if (k == K("$")) {
                ParseStreamPosition m = position(ps);
                bump(ps, TRIVIA_FLAG);
                parse_atom(ps);
                emit(ps, m, K("$"));
                macro_name_position = position(ps);
                emit(ps, m, K("inert"));
                emit(ps, mark, K("."));
            } else if (k == K("@")) {
                // A macro call after some prefix A has been consumed
                ParseStreamPosition m = position(ps);
                if (is_macrocall) {
                    bump(ps, TRIVIA_FLAG, NL_DEFAULT, "repeated `@` in macro module path");
                } else {
                    bump(ps, TRIVIA_FLAG);
                    is_macrocall = true;
                }
                parse_macro_name(ps);
                macro_name_position = position(ps);
                have_macro_atname_range = true;
                macro_atname_range[0] = m;
                macro_atname_range[1] = position(ps);
                emit(ps, m, K("quote"));
                emit(ps, mark, K("."));
            } else if (k == K("'")) {
                bump(ps);
                emit_err(ps, emark, K("error"), TRIVIA_FLAG, "the .' operator for transpose is discontinued");
            } else {
                // Field/property syntax
                ParseStreamPosition m = position(ps);
                parse_atom(ps, false);
                macro_name_position = position(ps);
                maybe_strmac_1 = true;
                emit(ps, m, K("quote"));
                emit(ps, mark, K("."));
            }
        } else if (k == K("'") && !t.preceding_whitespace) {
            bump(ps);
            emit(ps, mark, K("call"), POSTFIX_OP_FLAG);
        } else if (k == K("{")) {
            // Type parameter curlies and macro calls
            ParseStreamPosition m = position(ps);
            bump_disallowed_space(ps);
            bump(ps, TRIVIA_FLAG);
            parse_call_arglist(ps, K("}"));
            if (is_macrocall) {
                fix_macro_name_kind(ps, macro_name_position);
                emit(ps, m, K("braces"));
                emit(ps, mark, K("macrocall"));
                min_supported_version(1, 6, ps, mark, "macro call without space before `{}`");
                is_macrocall = false;
                have_macro_atname_range = false;
            } else {
                emit(ps, mark, K("curly"));
            }
        } else if ((k == K("\"") || k == K("\"\"\"") || k == K("`") || k == K("```")) && !t.preceding_whitespace &&
                   maybe_strmac &&
                   [&] {
                       Kind origk = peek_behind(ps, macro_name_position).orig_kind;
                       return origk == K("Identifier") || is_contextual_keyword(origk) || is_word_operator(origk);
                   }()) {
            // Custom string and command literals
            Kind outk = is_string_delim(k) ? K("StringMacroName") : K("CmdMacroName");
            fix_macro_name_kind(ps, macro_name_position, outk);
            parse_string(ps, true);
            t = peek_token(ps);
            k = t.kind;
            if (!t.preceding_whitespace && is_string_macro_suffix(k)) {
                // Macro suffixes can include keywords and numbers
                Kind suffix_kind = (k == K("Identifier") || is_keyword(k) || is_word_operator(k)) ? K("String") : k;
                bump_remap(ps, suffix_kind);
            }
            emit(ps, mark, K("macrocall"));
        } else {
            break;
        }
        maybe_strmac = maybe_strmac_1;
    }
}

// Parse the `A<:B` part of type definitions like `struct A<:B end`
void parse_subtype_spec(ParseState ps) { parse_comparison(ps, true); }

void parse_struct_field(ParseState ps) {
    ParseStreamPosition mark = position(ps);
    bool const_field = peek(ps) == K("const");
    if (const_field) bump(ps, TRIVIA_FLAG);
    parse_eq(ps);
    if (const_field) {
        emit(ps, mark, K("const"));
        min_supported_version(1, 8, ps, mark, "`const` struct field");
    }
}

// parse expressions or blocks introduced by syntactic reserved words.
void parse_resword(ParseState ps) {
    DepthGuard guard(ps);
    ps = normal_context(ps);
    bump_trivia(ps);
    ParseStreamPosition mark = position(ps);
    Kind word = peek(ps);
    if (word == K("begin") || word == K("quote")) {
        bump(ps, TRIVIA_FLAG);
        parse_block_inner(ps, parse_docstring);
        bump_closing_token(ps, K("end"));
        emit(ps, mark, K("block"));
        if (word == K("quote")) emit(ps, mark, K("quote"));
    } else if (word == K("while")) {
        bump(ps, TRIVIA_FLAG);
        parse_cond(ps);
        parse_block(ps);
        bump_closing_token(ps, K("end"));
        emit(ps, mark, K("while"));
    } else if (word == K("for")) {
        bump(ps, TRIVIA_FLAG);
        parse_iteration_specs(ps);
        parse_block(ps);
        bump_closing_token(ps, K("end"));
        emit(ps, mark, K("for"));
    } else if (word == K("let")) {
        bump(ps, TRIVIA_FLAG);
        ParseStreamPosition m = position(ps);
        Kind k0 = peek(ps);
        if (k0 == K("NewlineWs") || k0 == K(";")) {
        } else {
            parse_comma_separated(ps, parse_eq_star);
        }
        emit(ps, m, K("block"));
        Kind k = peek(ps);
        if (k == K("NewlineWs") || k == K(";")) {
            bump(ps, TRIVIA_FLAG);
        } else if (k == K("end")) {
            // pass
        } else {
            recover(is_closer_or_newline, ps, TRIVIA_FLAG, position(ps),
                    "let variables should end in `;` or newline");
        }
        parse_block(ps);
        bump_closing_token(ps, K("end"));
        emit(ps, mark, K("let"));
    } else if (word == K("if")) {
        parse_if_elseif(ps);
    } else if (word == K("global") || word == K("local")) {
        bump(ps, TRIVIA_FLAG);
        bool have_const = false;
        ParseStreamPosition const_mark{};
        if (peek(ps) == K("const")) {
            have_const = true;
            const_mark = position(ps);
            bump(ps, TRIVIA_FLAG);
        }
        bool had_assignment = parse_global_local_const_vars(ps);
        if (have_const) {
            emit(ps, const_mark, K("const"));
            if (!had_assignment) emit_err(ps, mark, K("error"), EMPTY_FLAGS, "expected assignment after `const`");
        }
        emit(ps, mark, word);
    } else if (word == K("const")) {
        bump(ps, TRIVIA_FLAG);
        bool have_scope = false;
        ParseStreamPosition scope_mark{};
        Kind scope_k = peek(ps);
        if (scope_k == K("local") || scope_k == K("global")) {
            have_scope = true;
            scope_mark = position(ps);
            bump(ps, TRIVIA_FLAG);
        }
        bool had_assignment = parse_global_local_const_vars(ps);
        if (have_scope) emit(ps, scope_mark, scope_k);
        emit(ps, mark, K("const"));
        if (!had_assignment) emit_err(ps, mark, K("error"), EMPTY_FLAGS, "expected assignment after `const`");
    } else if (word == K("function") || word == K("macro")) {
        bump(ps, TRIVIA_FLAG);
        bump_trivia(ps);
        bool has_body = parse_function_signature(ps, word == K("function"));
        if (has_body) {
            parse_block(ps);
            bump_closing_token(ps, K("end"));
            emit(ps, mark, word);
        } else {
            // Function/macro definition with no methods
            bump(ps, TRIVIA_FLAG, 1);
            emit(ps, mark, word);
        }
    } else if (word == K("abstract")) {
        bump(ps, TRIVIA_FLAG);
        bump(ps, TRIVIA_FLAG);  // type
        parse_subtype_spec(ps);
        bump_semicolon_trivia(ps);
        bump_closing_token(ps, K("end"));
        emit(ps, mark, K("abstract"));
    } else if (word == K("struct") || word == K("mutable")) {
        bool is_mut = word == K("mutable");
        if (is_mut) bump(ps, TRIVIA_FLAG);
        bump(ps, TRIVIA_FLAG);  // struct
        parse_subtype_spec(ps);
        parse_block_down(ps, parse_struct_field);
        bump_closing_token(ps, K("end"));
        emit(ps, mark, K("struct"), is_mut ? MUTABLE_FLAG : EMPTY_FLAGS);
    } else if (word == K("primitive")) {
        bump(ps, TRIVIA_FLAG);
        bump(ps, TRIVIA_FLAG);  // type
        {
            ParseState ps2 = with_space_sensitive(ps);
            parse_subtype_spec(ps2);
            parse_cond(ps2);
        }
        bump_semicolon_trivia(ps);
        bump_closing_token(ps, K("end"));
        emit(ps, mark, K("primitive"));
    } else if (word == K("try")) {
        parse_try(ps);
    } else if (word == K("return")) {
        bump(ps, TRIVIA_FLAG);
        Kind k = peek(ps);
        if (k == K("NewlineWs") || is_closing_token(ps, k)) {
        } else {
            parse_eq(ps);
        }
        emit(ps, mark, K("return"));
    } else if (word == K("break") || word == K("continue")) {
        bump(ps, TRIVIA_FLAG);
        emit(ps, mark, word);
        Kind k = peek(ps);
        if (!((k == K("NewlineWs") || k == K(";") || k == K(")") || k == K(":") || k == K("EndMarker")) ||
              (k == K("end") && !ps.end_symbol))) {
            recover(is_closer_or_newline, ps, TRIVIA_FLAG, position(ps),
                    "unexpected token after " + untokenize(word));
        }
    } else if (word == K("module") || word == K("baremodule")) {
        bump(ps, TRIVIA_FLAG);
        if (is_reserved_word(peek(ps))) {
            bump(ps, EMPTY_FLAGS, NL_DEFAULT, "Invalid module name");
        } else {
            parse_unary_prefix(ps);
        }
        parse_block_down(ps, parse_public);
        bump_closing_token(ps, K("end"));
        emit(ps, mark, K("module"), word == K("baremodule") ? BARE_MODULE_FLAG : EMPTY_FLAGS);
    } else if (word == K("export") || word == K("public")) {
        bump(ps, TRIVIA_FLAG);
        parse_comma_separated(ps, [](ParseState p) { parse_atsym(p, false, false); });
        emit(ps, mark, word);
    } else if (word == K("import") || word == K("using")) {
        parse_imports(ps);
    } else if (word == K("do")) {
        bump(ps, TRIVIA_FLAG, NL_DEFAULT, "invalid `do` syntax");
    } else {
        // internal_error("unhandled reserved word"): only reachable after the
        // depth cap consumed the input.
        bump(ps);
    }
}

// Parse if-elseif-else-end expressions
void parse_if_elseif(ParseState ps, bool is_elseif, bool is_elseif_whitespace_err) {
    DepthGuard guard(ps);
    ParseStreamPosition mark = position(ps);
    Kind word = peek(ps);
    if (is_elseif_whitespace_err) {
        word = K("elseif");
    } else {
        bump(ps, TRIVIA_FLAG);
    }
    Kind k0 = peek(ps);
    if (k0 == K("NewlineWs") || k0 == K("end")) {
        std::string e = "missing condition in `" + untokenize(word) + "`";
        bump_trivia(ps, EMPTY_FLAGS, true, e.c_str());
    } else {
        parse_cond(ps);
    }
    parse_block(ps);
    bump_trivia(ps);
    Kind k = peek(ps);
    if (k == K("elseif")) {
        if (depth_exceeded(ps))
            check_depth(ps);
        else
            parse_if_elseif(ps, true);
    } else if (k == K("else")) {
        ParseStreamPosition emark = position(ps);
        bump(ps, TRIVIA_FLAG);
        if (peek(ps) == K("if")) {
            // Recovery: User wrote `else if` by mistake ?
            bump(ps, TRIVIA_FLAG);
            emit_err(ps, emark, K("error"), TRIVIA_FLAG, "use `elseif` instead of `else if`");
            if (depth_exceeded(ps))
                check_depth(ps);
            else
                parse_if_elseif(ps, true, true);
        } else {
            parse_block(ps);
        }
    }
    if (!is_elseif) bump_closing_token(ps, K("end"));
    emit(ps, mark, word);
}

// Like parse_assignment, but specialized so that we can omit the tuple when
// there's commas but no assignment.
bool parse_global_local_const_vars(ParseState ps) {
    ParseStreamPosition mark = position(ps);
    int n_commas = parse_comma(ps, false);
    SyntaxToken t = peek_token(ps);
    if (is_prec_assignment(t.kind)) {
        if (n_commas >= 1) emit(ps, mark, K("tuple"));
        parse_assignment_with_initial_ex(ps, mark, [](ParseState p) { parse_comma(p); });
    }
    return t.kind == K("=") && !is_dotted(t.flags);
}

// Parse function and macro definitions
bool parse_function_signature(ParseState ps, bool is_function) {
    bool is_anon_func = false;
    bool parsed_call = false;
    bool needs_parse_call = true;

    ParseStreamPosition mark = position(ps);
    if (!is_function) {
        // Parse macro name
        parse_unary_prefix(ps);
        Kind kb = peek_behind(ps).orig_kind;
        if (is_initial_reserved_word(ps, kb)) {
            emit_err(ps, mark, K("error"), EMPTY_FLAGS, "invalid macro name");
        }
    } else {
        if (peek(ps) != K("(")) {
            parse_unary_prefix(ps);
        } else {
            bump(ps, TRIVIA_FLAG);
            bool is_empty_tuple = peek(ps, 1, 1) == K(")");
            BracketOpts opts = parse_brackets(
                [&](bool had_commas, bool had_splat, int num_semis, int num_subexprs) {
                    bool _parsed_call = was_eventually_call(ps);
                    Kind pk2 = peek(ps, 2);
                    bool _needs_parse_call = pk2 == K("(") || pk2 == K(".");
                    bool _is_anon_func = !_needs_parse_call && !_parsed_call;
                    BracketOpts o;
                    o.needs_parameters = _is_anon_func;
                    o.is_anon_func = _is_anon_func;
                    o.parsed_call = _parsed_call;
                    o.needs_parse_call = _needs_parse_call;
                    o.maybe_grouping_parens = !had_commas && !had_splat && num_semis == 0 && num_subexprs == 1;
                    return o;
                },
                ps, K(")"));
            is_anon_func = opts.is_anon_func;
            parsed_call = opts.parsed_call;
            needs_parse_call = opts.needs_parse_call;
            if (is_anon_func) {
                Kind pbk = peek_behind(ps).kind;
                bool ambiguous_parens = opts.maybe_grouping_parens &&
                                       ((pbk == K("macrocall") && !is_cmd_literal(ps)) || pbk == K("$"));
                emit(ps, mark, K("tuple"), PARENS_FLAG);
                if (ambiguous_parens) {
                    // 1.0.2: `(@f(x))` is ambiguous between an anonymous
                    // function and a macro-call signature. 1.0.2 wraps the
                    // tuple in an error node; only its diagnostic here.
                    diag_emit_range(ps, mark,
                                    "Ambiguous signature. Add a trailing comma if this is a 1-argument anonymous "
                                    "function; remove parentheses if this is a macro call acting as function "
                                    "signature.");
                }
            } else if (is_empty_tuple) {
                emit(ps, mark, K("tuple"), PARENS_FLAG);
            } else {
                emit(ps, mark, K("parens"), PARENS_FLAG);
            }
        }
        if (!is_anon_func) {
            Kind kb = peek_behind(ps).orig_kind;
            if (is_reserved_word(kb)) emit_err(ps, mark, K("error"), EMPTY_FLAGS, "invalid function name");
        }
    }
    if (peek(ps, 1, 1) == K("end") && !is_anon_func && !parsed_call) {
        // 1.0.2 runs parse_call_chain first and then accepts only an
        // identifier, `var"..."` or `$x` before `end`; anything else is an
        // "Invalid signature" error (only its diagnostic here).
        if (needs_parse_call) {
            // 1.0.2 makes a bare (undotted) operator atom an Identifier, so
            // `function + end` is fine there, `function .+ end` and
            // `function : end` are not.
            PeekBehind pb = peek_behind(ps);
            Kind sk = pb.kind;
            bool op_name = pb.is_leaf && is_operator(sk) && !is_dotted(pb.flags) && sk != K(":");
            if (!(op_name || sk == K("Identifier") || sk == K("var") || sk == K("$") || sk == K("call") ||
                  (sk == K("macrocall") && !is_cmd_literal(ps))))
                diag_emit_range(ps, mark,
                                std::string("Invalid signature in ") + (is_function ? "function" : "macro") +
                                    " definition");
        }
        return false;
    }
    if (needs_parse_call) {
        // Parse function argument list
        parse_call_chain(ps, mark);
        Kind sk = peek_behind(ps).kind;
        if ((sk == K("macrocall") && !is_cmd_literal(ps)) || sk == K("do")) {
            // 1.0.2 accepts a macro call as the signature (Julia >= 1.12),
            // and puts a `do` block inside its call (so the signature is a
            // `call` there): the 0.4.10 error node without its diagnostic.
            emit(ps, mark, K("error"));
        } else if (sk != K("call")) {
            emit_err(ps, mark, K("error"), EMPTY_FLAGS,
                     std::string("Invalid signature in ") + (is_function ? "function" : "macro") + " definition");
        }
    }
    if (is_function && peek(ps) == K("::")) {
        // Function return type
        bump(ps, TRIVIA_FLAG);
        parse_call(ps);
        emit(ps, mark, K("::"), INFIX_FLAG);
    }
    if (peek(ps) == K("where")) parse_where_chain(ps, mark);
    return true;
}

// Parse a try block
void parse_try(ParseState ps) {
    ParseStreamPosition mark = position(ps);
    bump(ps, TRIVIA_FLAG);
    parse_block(ps);
    bool has_catch = false;
    bool has_finally = false;
    bump_trivia(ps);
    if (peek(ps) == K("catch")) {
        has_catch = true;
        parse_catch(ps);
    }
    bump_trivia(ps);
    if (peek(ps) == K("else")) {
        ParseStreamPosition else_mark = position(ps);
        bump(ps, TRIVIA_FLAG);
        parse_block(ps);
        if (!has_catch) emit_err(ps, else_mark, K("error"), EMPTY_FLAGS, "Expected `catch` before `else`");
        min_supported_version(1, 8, ps, else_mark, "`else` after `catch`");
        emit(ps, else_mark, K("else"));
    }
    bump_trivia(ps);
    if (peek(ps) == K("finally")) {
        ParseStreamPosition finally_mark = position(ps);
        has_finally = true;
        bump(ps, TRIVIA_FLAG);
        parse_block(ps);
        emit(ps, finally_mark, K("finally"));
    }
    // Wart: the flisp parser allows finally before catch
    bump_trivia(ps);
    if (!has_catch && peek(ps) == K("catch")) {
        ParseStreamPosition m = position(ps);
        parse_catch(ps);
        diag_mark(ps, m, "`catch` after `finally` will execute out of order", Level::Warning);
    }
    bool missing_recovery = !has_catch && !has_finally;
    if (missing_recovery) bump_invisible(ps, K("error"), TRIVIA_FLAG);
    bump_closing_token(ps, K("end"));
    emit(ps, mark, K("try"));
    if (missing_recovery) diag_mark(ps, mark, "try without catch or finally");
}

void parse_catch(ParseState ps) {
    ParseStreamPosition mark = position(ps);
    bump(ps, TRIVIA_FLAG);
    Kind k = peek(ps);
    if (k == K("NewlineWs") || k == K(";") || is_closing_token(ps, k)) {
        bump_invisible(ps, K("false"));
    } else {
        ParseStreamPosition m = position(ps);
        parse_eq_star(ps);
        Kind pk = peek_behind(ps).kind;
        if (!(pk == K("Identifier") || pk == K("var") || pk == K("$")))
            emit_err(ps, m, K("error"), EMPTY_FLAGS, "a variable name is expected after `catch`");
    }
    parse_block(ps);
    emit(ps, mark, K("catch"));
}

void parse_do(ParseState ps, ParseStreamPosition mark) {
    bump(ps, TRIVIA_FLAG);  // do
    ps = normal_context(ps);
    ParseStreamPosition m = position(ps);
    Kind k = peek(ps);
    if (k == K("NewlineWs") || k == K(";")) {
        bump(ps, TRIVIA_FLAG);
    } else {
        parse_comma_separated(ps, parse_range);
    }
    emit(ps, m, K("tuple"));
    parse_block(ps);
    bump_closing_token(ps, K("end"));
    emit(ps, mark, K("do"));
}

bool _is_valid_macro_name(const PeekBehind &peektok) {
    return !is_error(peektok.kind) && (peektok.is_leaf || peektok.kind == K("var"));
}

void fix_macro_name_kind(ParseState ps, ParseStreamPosition macro_name_position, Kind name_kind) {
    Kind k = peek_behind(ps, macro_name_position).kind;
    if (k == K("var")) {
        macro_name_position = first_child_position(ps, macro_name_position);
        k = peek_behind(ps, macro_name_position).kind;
    } else if (k == K("parens")) {
        macro_name_position = first_child_position(ps, macro_name_position);
        if (macro_name_position == NO_POSITION) return;
        k = peek_behind(ps, macro_name_position).kind;
    } else if (k == K("error")) {
        // Error already reported in parse_macro_name
        return;
    }
    if (name_kind == K("None")) {
        name_kind = _is_valid_macro_name(peek_behind(ps, macro_name_position)) ? K("MacroName") : K("error");
        if (name_kind == K("error")) diag_marks(ps, macro_name_position, macro_name_position, "invalid macro name");
    }
    reset_node_kind(ps, macro_name_position, name_kind);
}

void parse_macro_name(ParseState ps) {
    bump_disallowed_space(ps);
    ParseStreamPosition mark = position(ps);
    parse_atom(ps, false);
    PeekBehind b = peek_behind(ps, true, false);
    if (b.kind == K("parens")) {
        diag_mark(ps, mark, "parenthesizing macro names is unnecessary", Level::Warning);
    } else if (!_is_valid_macro_name(b)) {
        emit_err(ps, mark, K("error"), EMPTY_FLAGS, "invalid macro name");
    }
}

// Parse an identifier, interpolation or @-prefixed symbol.
// `allow_quotes_102` is 1.0.2's `allow_quotes`, which differs in import
// paths; it only changes the diagnostics.
void parse_atsym(ParseState ps, bool allow_quotes, bool allow_quotes_102) {
    bump_trivia(ps);
    if (peek(ps) == K("@")) {
        bump(ps, TRIVIA_FLAG);
        parse_macro_name(ps);
        fix_macro_name_kind(ps, position(ps));
    } else {
        ParseStreamPosition mark = position(ps);
        if (allow_quotes_102 && peek(ps) == K(":") && !is_closing_token(ps, peek(ps, 2)))
            diag_here(ps, "quoting with `:` is not required here", false, Level::Warning);
        parse_unary_prefix(ps);
        auto find = [&](bool aq, bool *warn_parens) {
            ParseStreamPosition pos = position(ps);
            *warn_parens = false;
            if (peek_behind(ps, pos).kind == K("parens")) {
                pos = first_child_position(ps, pos);
                *warn_parens = true;
            }
            if (aq && peek_behind(ps, pos).kind == K("quote")) {
                pos = first_child_position(ps, pos);
                if (peek_behind(ps, pos).kind == K("parens")) {
                    pos = first_child_position(ps, pos);
                    *warn_parens = true;
                }
            }
            return pos;
        };
        auto is_ok = [](const PeekBehind &b) {
            return (b.is_leaf && (b.kind == K("Identifier") || is_operator(b.kind))) ||
                   (!b.is_leaf && (b.kind == K("$") || b.kind == K("var")));
        };
        bool warn_parens = false, warn_parens_102 = false;
        ParseStreamPosition pos = find(allow_quotes, &warn_parens);
        ParseStreamPosition pos_102 = find(allow_quotes_102, &warn_parens_102);
        PeekBehind b = peek_behind(ps, pos);
        PeekBehind b_102 = peek_behind(ps, pos_102);
        // 1.0.2: no warning when the parenthesized thing is `..`
        if (warn_parens_102 && b_102.orig_kind != K(".."))
            diag_mark(ps, mark, "parentheses are not required here", Level::Warning);
        if (!is_ok(b)) {
            emit_err(ps, mark, K("error"), EMPTY_FLAGS, "expected identifier");
        } else if (!is_ok(b_102)) {
            // 1.0.2 makes an error node here; only its diagnostic.
            diag_emit_range(ps, mark, "expected identifier");
        }
    }
}

// Parse import and using syntax
void parse_imports(ParseState ps) {
    ParseStreamPosition mark = position(ps);
    Kind word = peek(ps);
    bump(ps, TRIVIA_FLAG);
    ParseStreamPosition emark = position(ps);
    bool initial_as = parse_import(ps, word, false);
    SyntaxToken t = peek_token(ps);
    Kind k = t.kind;
    bool has_import_prefix = false;
    bool has_comma = false;
    if (k == K(":") && !t.preceding_whitespace) {
        bump(ps, TRIVIA_FLAG);
        has_import_prefix = true;
        if (initial_as) emit_err(ps, emark, K("error"), EMPTY_FLAGS, "`as` before `:` in import/using");
    } else if (k == K(",")) {
        bump(ps, TRIVIA_FLAG);
        has_comma = true;
    }
    if (has_import_prefix || has_comma) {
        bool has_import_prefix_ = has_import_prefix;
        auto down = [word, has_import_prefix_](ParseState ps1) { parse_import(ps1, word, has_import_prefix_); };
        parse_comma_separated(ps, down);
        if (peek(ps) == K(":")) {
            // Error recovery
            ParseStreamPosition emark2 = position(ps);
            bump(ps, TRIVIA_FLAG);
            parse_comma_separated(ps, down);
            emit_err(ps, emark2, K("error"), TRIVIA_FLAG,
                     "`:` can only be used when importing a single module. Split imports into multiple lines");
        }
    }
    if (has_import_prefix) emit(ps, mark, K(":"));
    emit(ps, mark, word);
}

// Parse individual module path and renaming with `as`
bool parse_import(ParseState ps, Kind word, bool has_import_prefix) {
    ParseStreamPosition mark = position(ps);
    parse_import_path(ps);
    if (peek(ps) == K("as")) {
        bump(ps, TRIVIA_FLAG);
        parse_atsym(ps, true, false);  // 1.0.2: parse_atsym(ps, false)
        emit(ps, mark, K("as"));
        if (word == K("using") && !has_import_prefix)
            emit_err(ps, mark, K("error"), EMPTY_FLAGS,
                     "`using` with `as` renaming requires a `:` and context module");
        min_supported_version(1, 6, ps, mark, "`import ... as`");
        return true;
    }
    return false;
}

void parse_import_path(ParseState ps) {
    ParseStreamPosition mark = position(ps);
    bump_trivia(ps);
    // The tokenizer produces conjoined dotted tokens .. and ...
    bool first_dot = true;
    while (true) {
        SyntaxToken t = peek_token(ps);
        Kind k = t.kind;
        if (!first_dot && t.preceding_whitespace)
            diag_here(ps, "space between dots in import path", true, Level::Warning);
        if (k == K(".")) {
            bump(ps);
        } else if (k == K("..")) {
            bump_split(ps, {{1, K("."), EMPTY_FLAGS}, {1, K("."), EMPTY_FLAGS}});
        } else if (k == K("...")) {
            bump_split(ps, {{1, K("."), EMPTY_FLAGS}, {1, K("."), EMPTY_FLAGS}, {1, K("."), EMPTY_FLAGS}});
        } else {
            break;
        }
        first_dot = false;
    }
    if (is_dotted(peek_token(ps).flags)) {
        bump_trivia(ps);
        bump_split(ps, {{1, K("."), EMPTY_FLAGS}, {1, peek(ps), EMPTY_FLAGS}});
    } else {
        parse_atsym(ps, true, false);  // 1.0.2: parse_atsym(ps, false)
    }
    while (true) {
        SyntaxToken t = peek_token(ps);
        Kind k = t.kind;
        if (k == K(".")) {
            bump_disallowed_space(ps);
            bump(ps, TRIVIA_FLAG);
            parse_atsym(ps);
        } else if (is_dotted(t.flags)) {
            if (t.preceding_whitespace)
                diag_here(ps, "space between dots in import path", true, Level::Warning);
            bump_trivia(ps);
            ParseStreamPosition m = position(ps);
            bump_split(ps, {{1, K("."), TRIVIA_FLAG}, {1, k, EMPTY_FLAGS}});
            if (is_syntactic_operator(k)) {
                // 1.0.2: an error node (only the diagnostic here)
                diag_emit_range(ps, m, "syntactic operators not allowed in import");
            }
        } else if (k == K("...")) {
            bump_split(ps, {{1, K("."), TRIVIA_FLAG}, {2, K(".."), EMPTY_FLAGS}});
        } else {
            break;
        }
    }
    emit(ps, mark, K("importpath"));
}

// parse comma-separated assignments, like "i=1:n,j=1:m,..."
template <class F> int parse_comma_separated(ParseState ps, F down) {
    int n_subexprs = 0;
    while (true) {
        down(ps);
        n_subexprs += 1;
        if (peek(ps) == K(",")) {
            bump(ps, TRIVIA_FLAG);
        } else {
            break;
        }
    }
    return n_subexprs;
}

// allows newline before =/in/∈ in generator expressions (issue #37393)
Kind peek_skip_newline_in_gen(const ParseState &ps, size_t n = 1) {
    Kind k = peek(ps, n);
    if (ps.for_generator && k == K("NewlineWs")) k = peek(ps, n + 1);
    return k;
}

bool is_iter_eq(Kind k) { return k == K("=") || k == K("in") || k == K("∈"); }

// parse comma-separated "assignment" but allowing `in` and `∈`
void parse_iteration_spec(ParseState ps) {
    ParseStreamPosition mark = position(ps);
    // Handle `outer` contextual keyword
    parse_pipe_lt(with_space_sensitive(ps));
    if (peek_behind(ps).orig_kind == K("outer")) {
        if (is_iter_eq(peek_skip_newline_in_gen(ps))) {
            // Not outer keyword
        } else {
            reset_node(ps, position(ps), K("outer"), TRIVIA_FLAG);
            parse_pipe_lt(ps);
            emit(ps, mark, K("outer"));
        }
    }
    if (is_iter_eq(peek_skip_newline_in_gen(ps))) {
        bump(ps, TRIVIA_FLAG);
        parse_pipe_lt(ps);
    } else {
        // Recovery heuristic
        recover([](const ParseState &p, Kind k) { return k == K(",") || k == K("NewlineWs") || is_closing_token(p, k); },
                ps, EMPTY_FLAGS, position(ps), "invalid iteration spec: expected one of `=` `in` or `∈`");
    }
    emit(ps, mark, K("="));
}

void parse_iteration_specs(ParseState ps) {
    ParseStreamPosition mark = position(ps);
    int n_iters = parse_comma_separated(ps, parse_iteration_spec);
    if (n_iters > 1) emit(ps, mark, K("cartesian_iterator"));
}

int parse_space_separated_exprs(ParseState ps) {
    ps = with_space_sensitive(ps);
    int n_sep = 0;
    while (true) {
        Kind k = peek(ps);
        if (is_closing_token(ps, k) || k == K("NewlineWs") || (ps.for_generator && k == K("for"))) break;
        parse_eq(ps);
        n_sep += 1;
    }
    return n_sep;
}

// like parse-arglist, but with `for` parsed as a generator
void parse_call_arglist(ParseState ps, Kind closer) {
    ps.for_generator = true;
    parse_brackets(
        [](bool, bool, int, int) {
            BracketOpts o;
            o.needs_parameters = true;
            return o;
        },
        ps, closer, false);
}

// Parse the suffix of comma-separated array expressions such as [x, suffix].
CatResult parse_vect(ParseState ps, Kind closer) {
    parse_brackets(
        [](bool, bool, int, int) {
            BracketOpts o;
            o.needs_parameters = true;
            return o;
        },
        ps, closer);
    return {K("vect"), EMPTY_FLAGS};
}

// Parse generators
void parse_generator(ParseState ps, ParseStreamPosition mark) {
    while (true) {
        SyntaxToken t = peek_token(ps);
        if (t.kind != K("for")) break;
        if (!t.preceding_whitespace)
            bump_invisible(ps, K("error"), TRIVIA_FLAG, "Expected space before `for` in generator");
        bump(ps, TRIVIA_FLAG);
        ParseStreamPosition iter_mark = position(ps);
        parse_iteration_specs(ps);
        if (peek(ps) == K("if")) {
            bump(ps, TRIVIA_FLAG);
            parse_cond(ps);
            emit(ps, iter_mark, K("filter"));
        }
    }
    emit(ps, mark, K("generator"));
}

CatResult parse_comprehension(ParseState ps, ParseStreamPosition mark, Kind closer) {
    ps.whitespace_newline = true;
    ps.space_sensitive = false;
    ps.end_symbol = false;
    parse_generator(ps, mark);
    bump_closing_token(ps, closer);
    return {K("comprehension"), EMPTY_FLAGS};
}

enum class ArrayOrder { unknown, row_major, column_major };
constexpr int kTypeminInt = -2147483647 - 1;

struct DimBp {
    int dim;
    int bp;
};

DimBp parse_array_separator(ParseState ps, ArrayOrder &array_order) {
    const char *sep_mismatch_err =
        "cannot mix space and ;; separators in an array expression, except to wrap a line";
    ParseStreamPosition mark = position(ps);
    SyntaxToken t = peek_token(ps, 1, 1);
    if (t.kind == K(";")) {
        // Newlines before semicolons are not significant
        bump_trivia(ps);
        int n_semis = 1;
        while (true) {
            bump(ps, TRIVIA_FLAG);
            t = peek_token(ps);
            if (t.kind != K(";")) break;
            if (t.preceding_whitespace) bump_disallowed_space(ps);
            n_semis += 1;
        }
        bool had_newline = peek(ps) == K("NewlineWs");
        // Newlines after semicolons are not significant
        bump_trivia(ps);
        if (n_semis == 2) {
            if (array_order == ArrayOrder::row_major) {
                if (had_newline) {
                    return {2, 0};
                } else {
                    emit_err(ps, mark, K("error"), TRIVIA_FLAG, sep_mismatch_err);
                }
            } else {
                array_order = ArrayOrder::column_major;
            }
        }
        return {n_semis, -n_semis};
    }
    t = peek_token(ps);
    Kind k = t.kind;
    if (k == K("NewlineWs")) {
        bump_trivia(ps);
        if (peek(ps) == K("]")) {
            return {kTypeminInt, kTypeminInt};
        } else {
            return {1, -1};
        }
    } else if (k == K(",")) {
        bump(ps, TRIVIA_FLAG, NL_DEFAULT, "unexpected comma in array expression");
        return {1, -1};
    } else {
        if (t.preceding_whitespace && !is_closing_token(ps, k)) {
            if (array_order == ArrayOrder::column_major) {
                bump_trivia(ps, TRIVIA_FLAG, true, sep_mismatch_err);
            } else {
                array_order = ArrayOrder::row_major;
            }
            return {2, 0};
        } else {
            return {kTypeminInt, kTypeminInt};
        }
    }
}

DimBp parse_array_inner(ParseState ps, int binding_power, ArrayOrder &array_order) {
    ParseStreamPosition mark = NO_POSITION;
    int dim = -1;
    int bp = binding_power;
    while (true) {
        if (bp < binding_power) return {dim, bp};
        // Allow trailing separators
        if (is_closing_token(ps, peek(ps))) return {kTypeminInt, kTypeminInt};
        DimBp next;
        if (bp == binding_power) {
            // Parse one expression
            mark = position(ps);
            parse_eq_star(ps);
            next = parse_array_separator(ps, array_order);
        } else {
            // Recurse to parse a separator with greater binding power.
            next = parse_array_inner(ps, bp, array_order);
            if (bp == 0)
                emit(ps, mark, K("row"));
            else
                emit(ps, mark, K("nrow"), set_numeric_flags(dim));
        }
        dim = next.dim;
        bp = next.bp;
    }
}

CatResult parse_array(ParseState ps, ParseStreamPosition mark, Kind closer, bool end_is_symbol) {
    ps.end_symbol = end_is_symbol;
    ArrayOrder array_order = ArrayOrder::unknown;
    DimBp s = parse_array_separator(ps, array_order);
    int dim = s.dim, binding_power = s.bp;
    if (binding_power == kTypeminInt) {
        bump_closing_token(ps, closer);
        return {K("hcat"), EMPTY_FLAGS};
    }
    while (true) {
        DimBp n = parse_array_inner(ps, binding_power, array_order);
        if (n.bp == kTypeminInt) break;
        if (binding_power == 0)
            emit(ps, mark, K("row"));
        else
            emit(ps, mark, K("nrow"), set_numeric_flags(dim));
        dim = n.dim;
        binding_power = n.bp;
    }
    bump_closing_token(ps, closer);
    return binding_power == -1  ? CatResult{K("vcat"), EMPTY_FLAGS}
           : binding_power == 0 ? CatResult{K("hcat"), EMPTY_FLAGS}
                                : CatResult{K("ncat"), set_numeric_flags(dim)};
}

// Parse array concatenation/construction/indexing syntax inside of `[]` or `{}`.
CatResult parse_cat(ParseState ps, Kind closer, bool end_is_symbol) {
    ps.range_colon_enabled = true;
    ps.space_sensitive = true;
    ps.where_enabled = true;
    ps.whitespace_newline = false;
    ps.for_generator = true;
    Kind k = peek(ps, 1, 1);
    ParseStreamPosition mark = position(ps);
    if (k == closer) {
        return parse_vect(ps, closer);
    } else if (k == K(";")) {
        bump_trivia(ps);
        ArrayOrder order = ArrayOrder::unknown;
        DimBp s = parse_array_separator(ps, order);
        min_supported_version(1, 8, ps, mark, "empty multidimensional array syntax");
        bump_closing_token(ps, closer);
        return {K("ncat"), set_numeric_flags(s.dim)};
    }
    parse_eq_star(ps);
    k = peek(ps, 1, 1);
    if (k == K(",") || (is_closing_token(ps, k) && k != K(";"))) {
        if (k == K(",")) bump(ps, TRIVIA_FLAG, 1);
        return parse_vect(ps, closer);
    } else if (k == K("for")) {
        return parse_comprehension(ps, mark, closer);
    } else {
        return parse_array(ps, mark, closer, end_is_symbol);
    }
}

// Parse un-prefixed parenthesized syntax.
void parse_paren(ParseState ps, bool check_identifiers) {
    ps.range_colon_enabled = true;
    ps.space_sensitive = false;
    ps.where_enabled = true;
    ps.whitespace_newline = true;
    ParseStreamPosition mark = position(ps);
    bump(ps, TRIVIA_FLAG);  // K"("
    Kind k = peek(ps);
    if (k == K(")")) {
        bump(ps, TRIVIA_FLAG);
        emit(ps, mark, K("tuple"), PARENS_FLAG);
    } else if (is_syntactic_operator(k)) {
        // allow :(=) etc in unchecked contexts, eg quotes
        parse_atom(ps, check_identifiers);
        bump_closing_token(ps, K(")"));
        emit(ps, mark, K("parens"));
    } else if (!check_identifiers && k == K("::") && peek(ps, 2, 1) == K(")")) {
        // allow :(::) as a special case
        bump(ps);
        bump(ps, TRIVIA_FLAG, 1);
        emit(ps, mark, K("parens"));
    } else {
        bool initial_semi = peek(ps) == K(";");
        BracketOpts opts = parse_brackets(
            [&](bool had_commas, bool had_splat, int num_semis, int num_subexprs) {
                bool is_tuple = had_commas || (had_splat && num_semis >= 1) ||
                                (initial_semi && (num_semis == 1 || num_subexprs > 0));
                BracketOpts o;
                o.needs_parameters = is_tuple;
                o.is_tuple = is_tuple;
                o.is_block = num_semis > 0;
                return o;
            },
            ps, K(")"));
        if (opts.is_tuple) {
            emit(ps, mark, K("tuple"), PARENS_FLAG);
        } else if (opts.is_block) {
            emit(ps, mark, K("block"), PARENS_FLAG);
        } else {
            emit(ps, mark, K("parens"));
        }
    }
}

// Handle bracketed syntax inside any of () [] or {} where there's a mixture
// of commas and semicolon delimiters.
//
// `generator_is_last` is 1.0.2's argument (false for call argument lists).
// 1.0.2 stops the list right after a generator (or, with it false, after a
// generator not followed by `,` `;` or the closer) and reports the missing
// closer there, while 0.4.10 carries on parsing elements. The tree stays
// 0.4.10's; only the diagnostics inside the brackets are swapped for 1.0.2's
// single "Expected" at that point.
template <class F>
BracketOpts parse_brackets(F after_parse, ParseState ps, Kind closing_kind, bool generator_is_last) {
    ps.range_colon_enabled = true;
    ps.space_sensitive = false;
    ps.where_enabled = true;
    ps.whitespace_newline = true;
    std::vector<ParseStreamPosition> params_positions;
    int num_subexprs = 0;
    int num_semis = 0;
    bool had_commas = false;
    bool had_splat = false;
    bool have_param_start = false;
    ParseStreamPosition param_start{};
    bool have_gen_stop = false;  // 1.0.2 would have stopped at gen_stop
    ParseStreamPosition gen_stop{};
    size_t gen_ndiag = 0;
    while (true) {
        Kind k = peek(ps);
        if (k == closing_kind) {
            break;
        } else if (k == K(";")) {
            // Start of parameters list
            if (have_param_start) params_positions.push_back(emit(ps, param_start, K("TOMBSTONE")));
            num_semis += 1;
            have_param_start = true;
            param_start = position(ps);
            bump(ps, TRIVIA_FLAG);
            bump_trivia(ps);
        } else if (is_closing_token(ps, k)) {
            // Error; handled below in bump_closing_token
            break;
        } else {
            ParseStreamPosition mark = position(ps);
            parse_eq_star(ps);
            num_subexprs += 1;
            if (num_subexprs == 1) had_splat = peek_behind(ps).kind == K("...");
            SyntaxToken t = peek_token(ps, 1, 1);
            k = t.kind;
            if (k == K(",")) {
                had_commas = true;
                bump(ps, TRIVIA_FLAG);
            } else if (k == K(";") || k == closing_kind) {
                // Handled above
                continue;
            } else if (k == K("for")) {
                // Generator syntax
                parse_generator(ps, mark);
                Kind kn = peek(ps, 1, 1);
                bool stops = generator_is_last ? kn != closing_kind
                                               : !(kn == K(",") || kn == K(";") || kn == closing_kind);
                if (stops && !have_gen_stop) {
                    have_gen_stop = true;
                    gen_stop = position(ps);
                    gen_ndiag = ps.stream->diagnostics.size();
                }
            } else {
                // Error - recovery done when consuming closing_kind
                break;
            }
        }
    }
    if (have_param_start && position(ps) != param_start)
        params_positions.push_back(emit(ps, param_start, K("TOMBSTONE")));
    BracketOpts opts = after_parse(had_commas, had_splat, num_semis, num_subexprs);
    // Emit nested parameter nodes if necessary
    if (opts.needs_parameters) {
        for (ParseStreamPosition pos : params_positions) reset_node_kind(ps, pos, K("parameters"));
    }
    bump_closing_token(ps, closing_kind, " or `,`");
    if (have_gen_stop && !ps.stream->depth_exceeded) {
        ps.stream->diagnostics.resize(gen_ndiag);
        std::string errmsg = "Expected `" + untokenize(closing_kind) + "` or `,`";
        diag_marks(ps, gen_stop, gen_stop, errmsg);
    }
    return opts;
}

bool _is_indentation(unsigned char b) { return b == ' ' || b == '\t'; }

// Parse a string, embedded interpolations and deindent triple quoted strings
// by marking indentation characters as whitespace trivia.
void parse_string(ParseState ps, bool raw) {
    ParseStreamPosition mark = position(ps);
    Kind delim_k = peek(ps);
    bool triplestr = delim_k == K("\"\"\"") || delim_k == K("```");
    Kind string_chunk_kind = (delim_k == K("\"") || delim_k == K("\"\"\"")) ? K("String") : K("CmdString");
    uint32_t indent_ref_i = 0;
    int64_t indent_ref_len = INT64_MAX;
    std::vector<ParseStreamPosition> indent_chunks;
    std::string_view txtbuf = ps.stream->text;
    auto txt = [&](uint32_t i) { return static_cast<unsigned char>(txtbuf[i - 1]); };
    RawFlags chunk_flags = raw ? RAW_STRING_FLAG : EMPTY_FLAGS;
    bump(ps, TRIVIA_FLAG);
    bool first_chunk = true;
    int n_nontrivia_chunks = 0;
    bool prev_chunk_newline = false;
    while (true) {
        FullToken t = peek_full_token(ps);
        Kind k = t.kind;
        if (k == K("$")) {
            if (raw) break;  // tokenization error, see parse_atom
            if (prev_chunk_newline) {
                indent_ref_i = t.first_byte;
                indent_ref_len = 0;
            }
            bump(ps, TRIVIA_FLAG);
            k = peek(ps);
            if (k == K("(")) {
                ParseStreamPosition m = position(ps);
                bump(ps, TRIVIA_FLAG);
                BracketOpts opts = parse_brackets(
                    [](bool had_commas, bool, int num_semis, int num_subexprs) {
                        BracketOpts o;
                        o.needs_parameters = false;
                        o.simple_interp = !had_commas && num_semis == 0 && num_subexprs == 1;
                        return o;
                    },
                    ps, K(")"));
                if (!opts.simple_interp || peek_behind(ps, true, false).kind == K("generator"))
                    emit_err(ps, m, K("error"), EMPTY_FLAGS, "invalid interpolation syntax");
                emit(ps, m, K("parens"));
            } else if (k == K("var")) {
                // var identifiers disabled in strings
                bump_remap(ps, K("Identifier"));
            } else if (k == K("Identifier") || is_keyword(k) || is_word_operator(k)) {
                parse_atom(ps);
            } else {
                bump_invisible(ps, K("error"), EMPTY_FLAGS,
                               "identifier or parenthesized expression expected after $ in string");
            }
            first_chunk = false;
            n_nontrivia_chunks += 1;
            prev_chunk_newline = false;
        } else if (k == string_chunk_kind) {
            bool single_newline = false;
            if (triplestr && first_chunk && t.span() <= 2) {
                uint32_t s = t.span();
                unsigned char b = txt(t.last_byte);
                single_newline = (s == 1 && (b == '\n' || b == '\r')) || (s == 2 && (txt(t.first_byte) == '\r' && b == '\n'));
            }
            if (single_newline) {
                // First line of triple string is a newline only: mark as trivia.
                bump(ps, TRIVIA_FLAG);
                first_chunk = false;
                prev_chunk_newline = true;
            } else {
                if (triplestr) {
                    unsigned char b0 = txt(t.first_byte);
                    if (prev_chunk_newline && b0 != '\n' && b0 != '\r') {
                        // Compute length of longest common prefix of mixed
                        // spaces and tabs, in bytes
                        if (indent_ref_i == 0) {
                            uint32_t i = t.first_byte - 1;
                            while (i < t.last_byte && _is_indentation(txt(i + 1))) i += 1;
                            indent_ref_i = t.first_byte;
                            indent_ref_len = static_cast<int64_t>(i) - t.first_byte + 1;
                        } else {
                            int64_t j = 0;
                            while (j < t.span() && j < indent_ref_len) {
                                if (txt(static_cast<uint32_t>(j) + t.first_byte) !=
                                    txt(static_cast<uint32_t>(j) + indent_ref_i))
                                    break;
                                j += 1;
                            }
                            indent_ref_len = std::min(indent_ref_len, j);
                        }
                        // Prepare a place for indentiation trivia, if necessary
                        indent_chunks.push_back(bump_invisible(ps, K("TOMBSTONE")));
                    }
                    unsigned char b = txt(t.last_byte);
                    prev_chunk_newline = b == '\n' || b == '\r';
                }
                bump(ps, chunk_flags);
                first_chunk = false;
                n_nontrivia_chunks += 1;
            }
        } else if (k == K("ErrorInvalidInterpolationTerminator") || k == K("ErrorBidiFormatting") ||
                   k == K("ErrorInvalidUTF8")) {
            // Treat these errors as string chunks
            bump(ps);
            n_nontrivia_chunks += 1;
        } else {
            break;
        }
    }
    bool had_end_delim = peek(ps) == delim_k;
    if (triplestr && prev_chunk_newline && had_end_delim) {
        // Newline at end of string
        indent_ref_len = 0;
    }
    if (triplestr && indent_ref_len > 0) {
        for (ParseStreamPosition pos : indent_chunks) {
            reset_node(ps, pos, K("Whitespace"), TRIVIA_FLAG);
            bool rhs_empty = steal_token_bytes(ps, pos, static_cast<uint32_t>(indent_ref_len));
            if (rhs_empty) {
                // Empty chunks after dedent are removed
                n_nontrivia_chunks -= 1;
            }
        }
    }
    if (had_end_delim) {
        if (n_nontrivia_chunks == 0) {
            // Empty strings, or empty after triple quoted processing
            bump_invisible(ps, string_chunk_kind, chunk_flags);
        }
        bump(ps, TRIVIA_FLAG);
    } else {
        // Missing delimiter recovery
        bump_invisible(ps, K("error"), TRIVIA_FLAG, "unterminated string literal");
    }
    Kind string_kind = (delim_k == K("\"") || delim_k == K("\"\"\"")) ? K("string") : K("cmdstring");
    RawFlags str_flags =
        static_cast<RawFlags>((triplestr ? TRIPLE_STRING_FLAG : EMPTY_FLAGS) | (raw ? RAW_STRING_FLAG : EMPTY_FLAGS));
    emit(ps, mark, string_kind, str_flags);
}

void emit_braces(ParseState ps, ParseStreamPosition mark, Kind ckind, RawFlags cflags) {
    if (ckind == K("hcat")) {
        emit(ps, mark, K("row"), cflags);
    } else if (ckind == K("ncat")) {
        emit(ps, mark, K("nrow"), cflags);
    }
    if (ckind == K("ncat")) min_supported_version(1, 7, ps, mark, "multidimensional array syntax");
    Kind outk = (ckind == K("vect") || ckind == K("comprehension")) ? K("braces") : K("bracescat");
    emit(ps, mark, outk);
}

// parse numbers, identifiers, parenthesized expressions, lists, vectors, etc.
void parse_atom(ParseState ps, bool check_identifiers) {
    DepthGuard guard(ps);
    check_depth(ps);
    bump_trivia(ps);
    ParseStreamPosition mark = position(ps);
    Kind leading_kind = peek(ps);
    if (is_error(leading_kind)) {
        // Errors for bad tokens are emitted in validate_tokens()
        bump(ps);
    } else if (leading_kind == K("'")) {
        // char literal
        bump(ps, TRIVIA_FLAG);
        Kind k = peek(ps);
        if (k == K("'")) {
            bump_invisible(ps, K("error"), EMPTY_FLAGS, "empty character literal");
            bump(ps, TRIVIA_FLAG);
        } else if (k == K("EndMarker")) {
            bump_invisible(ps, K("error"), EMPTY_FLAGS, "unterminated character literal");
        } else {
            if (k == K("Char")) {
                bump(ps);
            } else if (is_error(k)) {
                bump(ps);
            } else {
                // FIXME in JuliaSyntax: a tokenization error, remap the kind
                bump_remap(ps, K("Char"));
            }
            if (peek(ps) == K("'")) {
                bump(ps, TRIVIA_FLAG);
            } else {
                bump_invisible(ps, K("error"), TRIVIA_FLAG, "unterminated character literal");
            }
        }
        emit(ps, mark, K("char"));
    } else if (leading_kind == K("Char")) {
        // FIXME in JuliaSyntax: a tokenization error
        bump_remap(ps, K("Identifier"));
    } else if (leading_kind == K(":")) {
        // symbol/expression quote
        SyntaxToken t = peek_token(ps, 2);
        Kind k = t.kind;
        if (is_closing_token(ps, k) && (!is_keyword(k) || t.preceding_whitespace)) {
            // : is a literal colon in some circumstances
            bump(ps);  // K":"
            return;
        }
        bump(ps, TRIVIA_FLAG);  // K":"
        if (t.preceding_whitespace)
            bump_trivia(ps, TRIVIA_FLAG, true, "whitespace not allowed after `:` used for quoting");
        // Being inside quote makes keywords into identifiers at the first
        // level of nesting
        ParseState ps2 = ps;
        ps2.end_symbol = false;
        parse_atom(ps2, false);
        emit(ps, mark, K("quote"), COLON_QUOTE);
    } else if (check_identifiers && leading_kind == K("=") && is_plain_equals(peek_token(ps))) {
        bump(ps, EMPTY_FLAGS, NL_DEFAULT, "unexpected `=`");
    } else if (leading_kind == K("Identifier")) {
        bump(ps);
    } else if (is_word_operator(leading_kind)) {
        bump_remap(ps, K("Identifier"));
    } else if (is_operator(leading_kind)) {
        bump_dotsplit(ps, EMPTY_FLAGS, true);
        if (check_identifiers && !is_valid_identifier(leading_kind))
            emit_err(ps, mark, K("error"), EMPTY_FLAGS, "invalid identifier");
    } else if (is_keyword(leading_kind)) {
        SyntaxToken t2{};
        if (leading_kind == K("var") && (t2 = peek_token(ps, 2), t2.kind == K("\"") && !t2.preceding_whitespace)) {
            bump(ps, TRIVIA_FLAG);
            bump(ps, TRIVIA_FLAG);
            if (peek(ps) == K("String")) {
                bump(ps, RAW_STRING_FLAG, NL_DEFAULT, nullptr, K("Identifier"));
            } else {
                bump_invisible(ps, K("Identifier"), RAW_STRING_FLAG);
            }
            if (peek(ps) == K("\"")) {
                bump(ps, TRIVIA_FLAG);
            } else {
                bump_invisible(ps, K("error"), TRIVIA_FLAG, "unterminated `var\"\"` identifier");
            }
            SyntaxToken t = peek_token(ps);
            Kind k = t.kind;
            if (t.preceding_whitespace || is_operator(k) || k == K("(") || k == K(")") || k == K("[") ||
                k == K("]") || k == K("{") || k == K("}") || k == K(",") || k == K(";") || k == K("@") ||
                k == K("EndMarker")) {
                // var"x"+  ==>  x
            } else if (is_string_macro_suffix(k)) {
                bump(ps, TRIVIA_FLAG, NL_DEFAULT, "suffix not allowed after `var\"...\"` syntax");
            } else if (k == K("`") || k == K("\"") || k == K("\"\"\"") || k == K("```")) {
                bump_invisible(ps, K("error"), TRIVIA_FLAG,
                               "`var\"...\"` syntax not supported as string macro name");
            }
            emit(ps, mark, K("var"));
        } else if (check_identifiers && is_closing_token(ps, leading_kind)) {
            bump(ps, EMPTY_FLAGS, NL_DEFAULT, "invalid identifier");
        } else {
            // Remap keywords to identifiers.
            bump_remap(ps, K("Identifier"));
        }
    } else if (leading_kind == K("(")) {  // parens or tuple
        parse_paren(ps, check_identifiers);
    } else if (leading_kind == K("[")) {  // cat expression
        bump(ps, TRIVIA_FLAG);
        CatResult c = parse_cat(ps, K("]"), ps.end_symbol);
        emit(ps, mark, c.kind, c.flags);
        if (c.kind == K("ncat")) min_supported_version(1, 7, ps, mark, "multidimensional array syntax");
    } else if (leading_kind == K("{")) {  // cat expression
        bump(ps, TRIVIA_FLAG);
        CatResult c = parse_cat(ps, K("}"), ps.end_symbol);
        emit_braces(ps, mark, c.kind, c.flags);
    } else if (leading_kind == K("@")) {  // macro call
        bump(ps, TRIVIA_FLAG);
        parse_macro_name(ps);
        parse_call_chain(ps, mark, true);
    } else if (is_string_delim(leading_kind)) {
        parse_string(ps, false);
    } else if (leading_kind == K("`") || leading_kind == K("```")) {
        bump_invisible(ps, K("core_@cmd"));
        parse_string(ps, true);
        emit(ps, mark, K("macrocall"));
    } else if (is_literal(leading_kind)) {
        bump(ps);
    } else if (is_closing_token(ps, leading_kind)) {
        // Leave closing token in place for other productions to recover with
        std::string msg = leading_kind == K("EndMarker") ? std::string("premature end of input")
                                                         : "unexpected `" + untokenize(leading_kind) + "`";
        diag_here(ps, msg);
        bump_invisible(ps, K("error"));
    } else {
        bump(ps, EMPTY_FLAGS, NL_DEFAULT, "invalid syntax atom");
    }
}

}  // namespace

void ParseToplevel(ParseStream &stream) {
    ParseState ps{&stream};
    parse_toplevel(ps);
}

}  // namespace jlparse
