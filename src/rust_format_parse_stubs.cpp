// rust_format_parse_stubs.cpp -- weak stand-ins for the rustc_parse port's
// entry points (rust_lex / rust_ast / rust_parse), so the Rust formatter links
// into targets that do not (yet) build the parser: there Format() sees no
// parse and returns its input unchanged with an error. Wherever the parser's
// own sources are linked too (MEP_RUST_PARSE_SOURCES), their strong
// definitions replace these.
#include "rust_ast.h"
#include "rust_lex.h"
#include "rust_parse.h"

// NOLINTBEGIN
namespace rust {

__attribute__((weak)) std::unique_ptr<ParseResult> parse_file(std::string, const ParseOptions&) { return nullptr; }
__attribute__((weak)) Fragment parse_fragment(ParseResult&, uint32_t, uint32_t, FragmentKind, bool) { return Fragment(); }
__attribute__((weak)) std::unique_ptr<ParseResult> parse_string_fragment(std::string, FragmentKind, Fragment&,
                                                                         const ParseOptions&) {
    return nullptr;
}
__attribute__((weak)) std::string dump_crate(const ParseResult&) { return {}; }
__attribute__((weak)) std::string dump_expr(const ParseResult&, const Expr*) { return {}; }
__attribute__((weak)) const char* kw_str(Kw) { return ""; }
__attribute__((weak)) Kw kw_lookup(std::string_view) { return Kw::None; }
__attribute__((weak)) bool kw_is_special(Kw) { return false; }
__attribute__((weak)) bool kw_is_used_keyword(Kw, Edition) { return false; }
__attribute__((weak)) bool kw_is_unused_keyword(Kw, Edition) { return false; }
__attribute__((weak)) bool kw_is_reserved(Kw, Edition) { return false; }
__attribute__((weak)) bool kw_is_path_segment_keyword(Kw) { return false; }
__attribute__((weak)) LexResult lex(std::string_view, const LexOptions&) { return LexResult(); }
__attribute__((weak)) bool is_whitespace(uint32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
__attribute__((weak)) bool is_id_start(uint32_t) { return false; }
__attribute__((weak)) bool is_id_continue(uint32_t) { return false; }
__attribute__((weak)) uint32_t decode_utf8(std::string_view s, size_t i, size_t& len) {
    len = 1;
    return i < s.size() ? static_cast<unsigned char>(s[i]) : 0;
}
__attribute__((weak)) std::string token_to_string(const Token&, std::string_view) { return {}; }
__attribute__((weak)) const char* tok_str(Tok) { return ""; }
__attribute__((weak)) bool tok_is_punct(Tok) { return false; }
__attribute__((weak)) LineIndex::LineIndex(std::string_view src) : src_(src) {}
__attribute__((weak)) LineCol LineIndex::at(uint32_t) const { return LineCol(); }
__attribute__((weak)) const char* binop_str(BinOpKind) { return ""; }
__attribute__((weak)) const char* assignop_str(AssignOpKind) { return ""; }
__attribute__((weak)) const char* unop_str(UnOp) { return ""; }
__attribute__((weak)) const char* expr_kind_name(ExprKind) { return ""; }
__attribute__((weak)) const char* pat_kind_name(PatKind) { return ""; }
__attribute__((weak)) const char* ty_kind_name(TyKind) { return ""; }
__attribute__((weak)) const char* item_kind_name(ItemKind) { return ""; }
__attribute__((weak)) bool Param::is_self() const { return false; }
__attribute__((weak)) bool FnDecl::c_variadic() const { return false; }

}  // namespace rust
// NOLINTEND
