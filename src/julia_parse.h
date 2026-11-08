#ifndef MEP_JULIA_PARSE_H
#define MEP_JULIA_PARSE_H

// A Julia parser for mep's Julia language server and formatter.
//
// Ported from JuliaSyntax.jl 0.4.10 (MIT "Expat" licence,
// Copyright (c) 2021 Julia Computing and contributors) -- the version Runic.jl
// 1.5 is built on: the tokenizer (tokenize.jl), the kinds (kinds.jl), the
// ParseStream (parse_stream.jl), the recursive-descent parser (parser.jl) and
// the parts of literal_parsing.jl / green_tree.jl that diagnostics and trees
// need. The port goes decision for decision, keeping JuliaSyntax's function
// names, so julia_parser.cpp can be read side by side with parser.jl.
//
// What it promises (tools/julia_parse_check.py measures both):
//  * the green tree -- every node's kind, flags, byte span and children,
//    whitespace / comment trivia included -- is JuliaSyntax 0.4.10's
//    `build_tree(GreenNode, parse!(ParseStream(text); rule=:all))`;
//  * the diagnostics are those of the parser in Julia 1.12 (Base.JuliaSyntax
//    1.0.2, what `Meta.parseall` reports) where 1.0.2 differs from 0.4.10
//    only in its messages; see julia_parser.cpp "Diagnostics from 1.0.2".
//
// Byte positions: tree nodes carry a 0-based `offset` (index into the text)
// and a `span`; diagnostics keep JuliaSyntax's convention, 1-based and
// inclusive (`first_byte`, `last_byte`; an empty range has last = first - 1).
//
// Depth: parsing recurses. Nesting deeper than kMaxNestingDepth (counted in
// the recursive parse functions, roughly 10 per bracket level) stops the
// parse: the rest of the input becomes one `error` node with the diagnostic
// "expression nested too deeply", and later diagnostics are dropped. At the
// cap the parser uses about 1-2 MB of stack (-O2), so it is safe on any
// thread with the usual 8 MB stack.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace jlparse {

//------------------------------------------------------------------------------
// Kinds (kinds.jl)

/**
 * @brief The kind of a token or tree node: an index into kKindNames.
 *
 * Spell kinds the way JuliaSyntax does with `K"..."`: `K("call")`, `K("+=")`,
 * `K("Identifier")`, `K("BEGIN_OPS")` -- K() is consteval, so an unknown name
 * is a compile error and `case K("if"):` works in a switch.
 */
enum class Kind : uint16_t {};

struct KindMarker {
    std::string_view name;
    uint16_t value;
};

// clang-format off
inline constexpr std::string_view kKindNames[] = {
    "None", "EndMarker", "Comment", "Whitespace", "NewlineWs", "Identifier", "@", ",", ";",
    "ErrorEofMultiComment", "ErrorInvalidNumericConstant", "ErrorHexFloatMustContainP",
    "ErrorAmbiguousNumericConstant", "ErrorAmbiguousNumericDotMultiply",
    "ErrorInvalidInterpolationTerminator", "ErrorNumericOverflow", "ErrorInvalidEscapeSequence",
    "ErrorOverLongCharacter", "ErrorInvalidUTF8", "ErrorInvisibleChar", "ErrorUnknownCharacter",
    "ErrorBidiFormatting", "error", "baremodule", "begin", "break", "const", "continue", "do",
    "export", "for", "function", "global", "if", "import", "let", "local", "macro", "module",
    "quote", "return", "struct", "try", "using", "while", "catch", "finally", "else", "elseif",
    "end", "abstract", "as", "doc", "mutable", "outer", "primitive", "public", "type", "var",
    "Integer", "BinInt", "HexInt", "OctInt", "Float", "Float32", "String", "Char", "CmdString",
    "true", "false", "[", "]", "{", "}", "(", ")", "\"", "\"\"\"", "`", "```",
    "ErrorInvalidOperator", "Error**", "...", "=", "+=", "-=", "*=", "/=", "//=", "|=", "^=", "÷=",
    "%=", "<<=", ">>=", ">>>=", "\\=", "&=", ":=", "~", "$=", "⊻=", "≔", "⩴", "≕", "=>", "?", "-->",
    "<--", "<-->", "←", "→", "↔", "↚", "↛", "↞", "↠", "↢", "↣", "↤", "↦", "↮", "⇎", "⇍", "⇏", "⇐",
    "⇒", "⇔", "⇴", "⇶", "⇷", "⇸", "⇹", "⇺", "⇻", "⇼", "⇽", "⇾", "⇿", "⟵", "⟶", "⟷", "⟹", "⟺", "⟻",
    "⟼", "⟽", "⟾", "⟿", "⤀", "⤁", "⤂", "⤃", "⤄", "⤅", "⤆", "⤇", "⤌", "⤍", "⤎", "⤏", "⤐", "⤑", "⤔",
    "⤕", "⤖", "⤗", "⤘", "⤝", "⤞", "⤟", "⤠", "⥄", "⥅", "⥆", "⥇", "⥈", "⥊", "⥋", "⥎", "⥐", "⥒", "⥓",
    "⥖", "⥗", "⥚", "⥛", "⥞", "⥟", "⥢", "⥤", "⥦", "⥧", "⥨", "⥩", "⥪", "⥫", "⥬", "⥭", "⥰", "⧴", "⬱",
    "⬰", "⬲", "⬳", "⬴", "⬵", "⬶", "⬷", "⬸", "⬹", "⬺", "⬻", "⬼", "⬽", "⬾", "⬿", "⭀", "⭁", "⭂", "⭃",
    "⥷", "⭄", "⥺", "⭇", "⭈", "⭉", "⭊", "⭋", "⭌", "￩", "￫", "⇜", "⇝", "↜", "↝", "↩", "↪", "↫", "↬",
    "↼", "↽", "⇀", "⇁", "⇄", "⇆", "⇇", "⇉", "⇋", "⇌", "⇚", "⇛", "⇠", "⇢", "↷", "↶", "↺", "↻", "||",
    "&&", "<:", ">:", ">", "<", ">=", "≥", "<=", "≤", "==", "===", "≡", "!=", "≠", "!==", "≢", "∈",
    "in", "isa", "∉", "∋", "∌", "⊆", "⊈", "⊂", "⊄", "⊊", "∝", "∊", "∍", "∥", "∦", "∷", "∺", "∻",
    "∽", "∾", "≁", "≃", "≂", "≄", "≅", "≆", "≇", "≈", "≉", "≊", "≋", "≌", "≍", "≎", "≐", "≑", "≒",
    "≓", "≖", "≗", "≘", "≙", "≚", "≛", "≜", "≝", "≞", "≟", "≣", "≦", "≧", "≨", "≩", "≪", "≫", "≬",
    "≭", "≮", "≯", "≰", "≱", "≲", "≳", "≴", "≵", "≶", "≷", "≸", "≹", "≺", "≻", "≼", "≽", "≾", "≿",
    "⊀", "⊁", "⊃", "⊅", "⊇", "⊉", "⊋", "⊏", "⊐", "⊑", "⊒", "⊜", "⊩", "⊬", "⊮", "⊰", "⊱", "⊲", "⊳",
    "⊴", "⊵", "⊶", "⊷", "⋍", "⋐", "⋑", "⋕", "⋖", "⋗", "⋘", "⋙", "⋚", "⋛", "⋜", "⋝", "⋞", "⋟", "⋠",
    "⋡", "⋢", "⋣", "⋤", "⋥", "⋦", "⋧", "⋨", "⋩", "⋪", "⋫", "⋬", "⋭", "⋲", "⋳", "⋴", "⋵", "⋶", "⋷",
    "⋸", "⋹", "⋺", "⋻", "⋼", "⋽", "⋾", "⋿", "⟈", "⟉", "⟒", "⦷", "⧀", "⧁", "⧡", "⧣", "⧤", "⧥", "⩦",
    "⩧", "⩪", "⩫", "⩬", "⩭", "⩮", "⩯", "⩰", "⩱", "⩲", "⩳", "⩵", "⩶", "⩷", "⩸", "⩹", "⩺", "⩻", "⩼",
    "⩽", "⩾", "⩿", "⪀", "⪁", "⪂", "⪃", "⪄", "⪅", "⪆", "⪇", "⪈", "⪉", "⪊", "⪋", "⪌", "⪍", "⪎", "⪏",
    "⪐", "⪑", "⪒", "⪓", "⪔", "⪕", "⪖", "⪗", "⪘", "⪙", "⪚", "⪛", "⪜", "⪝", "⪞", "⪟", "⪠", "⪡", "⪢",
    "⪣", "⪤", "⪥", "⪦", "⪧", "⪨", "⪩", "⪪", "⪫", "⪬", "⪭", "⪮", "⪯", "⪰", "⪱", "⪲", "⪳", "⪴", "⪵",
    "⪶", "⪷", "⪸", "⪹", "⪺", "⪻", "⪼", "⪽", "⪾", "⪿", "⫀", "⫁", "⫂", "⫃", "⫄", "⫅", "⫆", "⫇", "⫈",
    "⫉", "⫊", "⫋", "⫌", "⫍", "⫎", "⫏", "⫐", "⫑", "⫒", "⫓", "⫔", "⫕", "⫖", "⫗", "⫘", "⫙", "⫷", "⫸",
    "⫹", "⫺", "⊢", "⊣", "⟂", "⫪", "⫫", "<|", "|>", ":", "..", "…", "⁝", "⋮", "⋱", "⋰", "⋯", "$",
    "+", "-", "++", "⊕", "⊖", "⊞", "⊟", "|", "∪", "∨", "⊔", "±", "∓", "∔", "∸", "≏", "⊎", "⊻", "⊽",
    "⋎", "⋓", "⟇", "⧺", "⧻", "⨈", "⨢", "⨣", "⨤", "⨥", "⨦", "⨧", "⨨", "⨩", "⨪", "⨫", "⨬", "⨭", "⨮",
    "⨹", "⨺", "⩁", "⩂", "⩅", "⩊", "⩌", "⩏", "⩐", "⩒", "⩔", "⩖", "⩗", "⩛", "⩝", "⩡", "⩢", "⩣", "¦",
    "*", "/", "÷", "%", "⋅", "∘", "×", "\\", "&", "∩", "∧", "⊗", "⊘", "⊙", "⊚", "⊛", "⊠", "⊡", "⊓",
    "∗", "∙", "∤", "⅋", "≀", "⊼", "⋄", "⋆", "⋇", "⋉", "⋊", "⋋", "⋌", "⋏", "⋒", "⟑", "⦸", "⦼", "⦾",
    "⦿", "⧶", "⧷", "⨇", "⨰", "⨱", "⨲", "⨳", "⨴", "⨵", "⨶", "⨷", "⨸", "⨻", "⨼", "⨽", "⩀", "⩃", "⩄",
    "⩋", "⩍", "⩎", "⩑", "⩓", "⩕", "⩘", "⩚", "⩜", "⩞", "⩟", "⩠", "⫛", "⊍", "▷", "⨝", "⟕", "⟖", "⟗",
    "⌿", "⨟", "//", "<<", ">>", ">>>", "^", "↑", "↓", "⇵", "⟰", "⟱", "⤈", "⤉", "⤊", "⤋", "⤒", "⤓",
    "⥉", "⥌", "⥍", "⥏", "⥑", "⥔", "⥕", "⥘", "⥙", "⥜", "⥝", "⥠", "⥡", "⥣", "⥥", "⥮", "⥯", "￪", "￬",
    "::", "where", ".", "!", "'", ".'", "->", "¬", "√", "∛", "∜", "TOMBSTONE", "MacroName",
    "StringMacroName", "CmdMacroName", "core_@cmd", "core_@int128_str", "core_@uint128_str",
    "core_@big_str", "block", "call", "dotcall", "comparison", "curly", "inert", "juxtapose",
    "string", "cmdstring", "char", "macrocall", "parameters", "toplevel", "tuple", "ref", "vect",
    "parens", "importpath", "braces", "bracescat", "hcat", "vcat", "ncat", "typed_hcat",
    "typed_vcat", "typed_ncat", "row", "nrow", "generator", "filter", "cartesian_iterator",
    "comprehension", "typed_comprehension", "wrapper",
};
inline constexpr KindMarker kKindMarkers[] = {
    {"BEGIN_ERRORS", 9},
    {"END_ERRORS", 22},
    {"BEGIN_KEYWORDS", 23},
    {"BEGIN_BLOCK_CONTINUATION_KEYWORDS", 45},
    {"END_BLOCK_CONTINUATION_KEYWORDS", 49},
    {"BEGIN_CONTEXTUAL_KEYWORDS", 50},
    {"END_CONTEXTUAL_KEYWORDS", 58},
    {"END_KEYWORDS", 58},
    {"BEGIN_LITERAL", 59},
    {"END_LITERAL", 69},
    {"BEGIN_DELIMITERS", 70},
    {"END_DELIMITERS", 79},
    {"BEGIN_OPS", 80},
    {"BEGIN_ASSIGNMENTS", 83},
    {"END_ASSIGNMENTS", 104},
    {"BEGIN_PAIRARROW", 105},
    {"END_PAIRARROW", 105},
    {"BEGIN_CONDITIONAL", 106},
    {"END_CONDITIONAL", 106},
    {"BEGIN_ARROW", 107},
    {"END_ARROW", 257},
    {"BEGIN_LAZYOR", 258},
    {"END_LAZYOR", 258},
    {"BEGIN_LAZYAND", 259},
    {"END_LAZYAND", 259},
    {"BEGIN_COMPARISON", 260},
    {"END_COMPARISON", 547},
    {"BEGIN_PIPE", 548},
    {"END_PIPE", 549},
    {"BEGIN_COLON", 550},
    {"END_COLON", 557},
    {"BEGIN_PLUS", 558},
    {"END_PLUS", 615},
    {"BEGIN_TIMES", 616},
    {"END_TIMES", 693},
    {"BEGIN_RATIONAL", 694},
    {"END_RATIONAL", 694},
    {"BEGIN_BITSHIFTS", 695},
    {"END_BITSHIFTS", 697},
    {"BEGIN_POWER", 698},
    {"END_POWER", 728},
    {"BEGIN_DECL", 729},
    {"END_DECL", 729},
    {"BEGIN_WHERE", 730},
    {"END_WHERE", 730},
    {"BEGIN_DOT", 731},
    {"END_DOT", 731},
    {"BEGIN_UNICODE_OPS", 736},
    {"END_UNICODE_OPS", 739},
    {"END_OPS", 739},
    {"BEGIN_PARSER_TOKENS", 740},
    {"BEGIN_MACRO_NAMES", 741},
    {"END_MACRO_NAMES", 747},
    {"END_PARSER_TOKENS", 747},
    {"BEGIN_SYNTAX_KINDS", 748},
    {"END_SYNTAX_KINDS", 781},
};
// clang-format on

inline constexpr size_t kNumKinds = sizeof(kKindNames) / sizeof(kKindNames[0]);

namespace detail {
void UnknownKindName();  // never defined: reaching it in K() is a compile error
}  // namespace detail

/** @brief `K"name"`: the kind called @p name (or a BEGIN_/END_ range marker). */
consteval Kind K(std::string_view name) {
    for (size_t i = 0; i < kNumKinds; ++i)
        if (kKindNames[i] == name) return static_cast<Kind>(i);
    for (const KindMarker &m : kKindMarkers)
        if (m.name == name) return static_cast<Kind>(m.value);
    detail::UnknownKindName();
    return Kind{};
}

/** @brief JuliaSyntax's spelling of @p k (`"Identifier"`, `"call"`, `"+="` ...). */
inline const char *KindName(Kind k) {
    size_t i = static_cast<size_t>(k);
    return i < kNumKinds ? kKindNames[i].data() : "?";
}
/** @brief The kind spelled @p name (also BEGIN_/END_ markers); K("None") if unknown. */
Kind KindFromName(std::string_view name);

inline constexpr uint16_t KindIndex(Kind k) { return static_cast<uint16_t>(k); }
inline constexpr bool KindIn(Kind k, Kind lo, Kind hi) {
    return KindIndex(lo) <= KindIndex(k) && KindIndex(k) <= KindIndex(hi);
}

// Kind predicates (kinds.jl)
inline constexpr bool is_contextual_keyword(Kind k) {
    return KindIn(k, K("BEGIN_CONTEXTUAL_KEYWORDS"), K("END_CONTEXTUAL_KEYWORDS"));
}
inline constexpr bool is_error(Kind k) {
    return KindIn(k, K("BEGIN_ERRORS"), K("END_ERRORS")) || k == K("ErrorInvalidOperator") || k == K("Error**");
}
inline constexpr bool is_keyword(Kind k) { return KindIn(k, K("BEGIN_KEYWORDS"), K("END_KEYWORDS")); }
inline constexpr bool is_block_continuation_keyword(Kind k) {
    return KindIn(k, K("BEGIN_BLOCK_CONTINUATION_KEYWORDS"), K("END_BLOCK_CONTINUATION_KEYWORDS"));
}
inline constexpr bool is_literal(Kind k) { return KindIn(k, K("BEGIN_LITERAL"), K("END_LITERAL")); }
inline constexpr bool is_operator(Kind k) { return KindIn(k, K("BEGIN_OPS"), K("END_OPS")); }
inline constexpr bool is_word_operator(Kind k) { return k == K("in") || k == K("isa") || k == K("where"); }

inline constexpr bool is_prec_assignment(Kind k) { return KindIn(k, K("BEGIN_ASSIGNMENTS"), K("END_ASSIGNMENTS")); }
inline constexpr bool is_prec_pair(Kind k) { return KindIn(k, K("BEGIN_PAIRARROW"), K("END_PAIRARROW")); }
inline constexpr bool is_prec_conditional(Kind k) { return KindIn(k, K("BEGIN_CONDITIONAL"), K("END_CONDITIONAL")); }
inline constexpr bool is_prec_arrow(Kind k) { return KindIn(k, K("BEGIN_ARROW"), K("END_ARROW")); }
inline constexpr bool is_prec_lazy_or(Kind k) { return KindIn(k, K("BEGIN_LAZYOR"), K("END_LAZYOR")); }
inline constexpr bool is_prec_lazy_and(Kind k) { return KindIn(k, K("BEGIN_LAZYAND"), K("END_LAZYAND")); }
inline constexpr bool is_prec_comparison(Kind k) { return KindIn(k, K("BEGIN_COMPARISON"), K("END_COMPARISON")); }
inline constexpr bool is_prec_pipe(Kind k) { return KindIn(k, K("BEGIN_PIPE"), K("END_PIPE")); }
inline constexpr bool is_prec_colon(Kind k) { return KindIn(k, K("BEGIN_COLON"), K("END_COLON")); }
inline constexpr bool is_prec_plus(Kind k) { return KindIn(k, K("BEGIN_PLUS"), K("END_PLUS")); }
inline constexpr bool is_prec_bitshift(Kind k) { return KindIn(k, K("BEGIN_BITSHIFTS"), K("END_BITSHIFTS")); }
inline constexpr bool is_prec_times(Kind k) { return KindIn(k, K("BEGIN_TIMES"), K("END_TIMES")); }
inline constexpr bool is_prec_rational(Kind k) { return KindIn(k, K("BEGIN_RATIONAL"), K("END_RATIONAL")); }
inline constexpr bool is_prec_power(Kind k) { return KindIn(k, K("BEGIN_POWER"), K("END_POWER")); }
inline constexpr bool is_prec_decl(Kind k) { return KindIn(k, K("BEGIN_DECL"), K("END_DECL")); }
inline constexpr bool is_prec_where(Kind k) { return KindIn(k, K("BEGIN_WHERE"), K("END_WHERE")); }
inline constexpr bool is_prec_dot(Kind k) { return KindIn(k, K("BEGIN_DOT"), K("END_DOT")); }
inline constexpr bool is_prec_unicode_ops(Kind k) { return KindIn(k, K("BEGIN_UNICODE_OPS"), K("END_UNICODE_OPS")); }
inline constexpr bool is_prec_pipe_lt(Kind k) { return k == K("<|"); }
inline constexpr bool is_prec_pipe_gt(Kind k) { return k == K("|>"); }
inline constexpr bool is_syntax_kind(Kind k) { return KindIn(k, K("BEGIN_SYNTAX_KINDS"), K("END_SYNTAX_KINDS")); }
inline constexpr bool is_macro_name(Kind k) { return KindIn(k, K("BEGIN_MACRO_NAMES"), K("END_MACRO_NAMES")); }
inline constexpr bool is_number(Kind k) {
    return k == K("Integer") || k == K("BinInt") || k == K("HexInt") || k == K("OctInt") || k == K("Float") ||
           k == K("Float32");
}
inline constexpr bool is_string_delim(Kind k) { return k == K("\"") || k == K("\"\"\""); }
inline constexpr bool is_radical_op(Kind k) { return k == K("√") || k == K("∛") || k == K("∜"); }
/** @brief Whitespace or comment kind. */
inline constexpr bool is_whitespace(Kind k) {
    return k == K("Whitespace") || k == K("NewlineWs") || k == K("Comment");
}

//------------------------------------------------------------------------------
// Flags (parse_stream.jl RawFlags)

using RawFlags = uint16_t;
inline constexpr RawFlags EMPTY_FLAGS = 0;
/// Set for tokens or ranges which are syntax trivia after parsing.
inline constexpr RawFlags TRIVIA_FLAG = 1u << 0;
/// Operator is dotted (`.+`).
inline constexpr RawFlags DOTOP_FLAG = 1u << 1;
/// Operator has a suffix (`+₁`).
inline constexpr RawFlags SUFFIXED_FLAG = 1u << 2;
/// Call-type field (bits 3-4) of K"call", K"dotcall" and syntactic operator heads.
inline constexpr RawFlags PREFIX_CALL_FLAG = 0u << 3;
inline constexpr RawFlags INFIX_FLAG = 1u << 3;
inline constexpr RawFlags PREFIX_OP_FLAG = 2u << 3;
inline constexpr RawFlags POSTFIX_OP_FLAG = 3u << 3;
// The following are head-specific and overlap.
/// K"string" / K"cmdstring" was triple-delimited.
inline constexpr RawFlags TRIPLE_STRING_FLAG = 1u << 5;
/// K"string", K"cmdstring" or K"Identifier" needs raw string unescaping.
inline constexpr RawFlags RAW_STRING_FLAG = 1u << 6;
/// K"tuple", K"block" or K"macrocall" delimited by parentheses.
inline constexpr RawFlags PARENS_FLAG = 1u << 5;
/// K"quote" in the short form `:x`.
inline constexpr RawFlags COLON_QUOTE = 1u << 5;
/// K"toplevel" made from `a; b` at top level.
inline constexpr RawFlags TOPLEVEL_SEMICOLONS_FLAG = 1u << 5;
/// K"struct" which is mutable.
inline constexpr RawFlags MUTABLE_FLAG = 1u << 5;
/// K"module" from `baremodule`.
inline constexpr RawFlags BARE_MODULE_FLAG = 1u << 5;
/// The dimension of K"nrow" / K"ncat" (bits 8-15).
inline constexpr RawFlags NUMERIC_FLAGS = 0xff00;

inline constexpr bool has_flags(RawFlags f, RawFlags test) { return (f & test) != 0; }
inline constexpr RawFlags call_type_flags(RawFlags f) { return static_cast<RawFlags>(f & 0b11000); }
inline constexpr int numeric_flags(RawFlags f) { return (f >> 8) & 0xff; }
inline constexpr RawFlags set_numeric_flags(int n) { return static_cast<RawFlags>((n << 8) & NUMERIC_FLAGS); }

/** @brief A kind plus flags, JuliaSyntax's `SyntaxHead`. */
struct SyntaxHead {
    Kind kind{};
    RawFlags flags = 0;
};

inline constexpr bool is_trivia(RawFlags f) { return has_flags(f, TRIVIA_FLAG); }
inline constexpr bool is_prefix_call(RawFlags f) { return call_type_flags(f) == PREFIX_CALL_FLAG; }
inline constexpr bool is_infix_op_call(RawFlags f) { return call_type_flags(f) == INFIX_FLAG; }
inline constexpr bool is_prefix_op_call(RawFlags f) { return call_type_flags(f) == PREFIX_OP_FLAG; }
inline constexpr bool is_postfix_op_call(RawFlags f) { return call_type_flags(f) == POSTFIX_OP_FLAG; }
inline constexpr bool is_dotted(RawFlags f) { return has_flags(f, DOTOP_FLAG); }
inline constexpr bool is_suffixed(RawFlags f) { return has_flags(f, SUFFIXED_FLAG); }
inline constexpr bool is_decorated(RawFlags f) { return is_dotted(f) || is_suffixed(f); }

/** @brief JuliaSyntax's `untokenize(head)` summary, e.g. "call-i", ".+", "string-s-r". */
std::string HeadString(SyntaxHead h, bool include_flag_suffixes = true);

//------------------------------------------------------------------------------
// Green tree (green_tree.jl)

/**
 * @brief One node of the green tree.
 *
 * Leaves are tokens (`leaf` true); interior nodes may have no children (an
 * empty `(block)`), so test `leaf`, not the child count.
 */
struct Node {
    Kind kind{};
    RawFlags flags = 0;
    uint32_t offset = 0;  ///< 0-based byte offset of the node's first byte
    uint32_t span = 0;    ///< length in bytes
    uint32_t end = 0;     ///< index one past the node's last descendant in Tree::nodes
    bool leaf = true;

    SyntaxHead head() const { return {kind, flags}; }
};

/**
 * @brief A green tree as a flat preorder array; nodes[0] is the root (K"toplevel"
 * for Parse(), K"wrapper" if the stream had several roots).
 *
 * Walk the children of node i: `for (uint32_t c = i + 1; c < nodes[i].end; c = nodes[c].end)`,
 * or use ForEachChild / Children.
 */
struct Tree {
    std::vector<Node> nodes;

    bool empty() const { return nodes.empty(); }
    const Node &operator[](uint32_t i) const { return nodes[i]; }
    uint32_t NumChildren(uint32_t i) const;
    std::vector<uint32_t> Children(uint32_t i) const;
    template <class F> void ForEachChild(uint32_t i, F &&f) const {
        for (uint32_t c = i + 1; c < nodes[i].end; c = nodes[c].end) f(c);
    }
};

//------------------------------------------------------------------------------
// Diagnostics (diagnostics.jl)

enum class Level : uint8_t { Error, Warning };

struct Diagnostic {
    int64_t first_byte = 0;  ///< 1-based, inclusive (JuliaSyntax convention)
    int64_t last_byte = 0;   ///< 1-based, inclusive; first_byte - 1 for an empty range
    Level level = Level::Error;
    std::string message;
};

/** @brief JuliaSyntax `source_location`: 1-based line and character column of 1-based @p byte. */
struct LineCol {
    int line = 1;
    int column = 1;
};

/** @brief Line starts of a text, for repeated SourceLocation lookups. */
class SourceIndex {
public:
    explicit SourceIndex(std::string_view text);
    /** @brief Line and column (characters, 1-based) of 1-based byte index @p byte. */
    LineCol Location(int64_t byte) const;

private:
    std::string_view text_;
    std::vector<int64_t> line_starts_;  // 1-based byte index of each line start (+ an end entry)
};

//------------------------------------------------------------------------------
// Parsing (parser_api.jl)

/** @brief Nesting limit (see the top of this file). */
inline constexpr int kMaxNestingDepth = 4000;

struct ParseResult {
    Tree tree;
    std::vector<Diagnostic> diagnostics;  ///< sorted by first_byte, like stream.diagnostics
    bool depth_limited = false;           ///< kMaxNestingDepth was hit
    bool any_error() const;
};

/**
 * @brief `parse!(ParseStream(text); rule=:all)` then `build_tree(GreenNode, ...)`.
 *
 * Never throws for any input bytes (invalid UTF-8 included).
 */
ParseResult Parse(std::string_view text);

/** @brief A raw token from the tokenizer (Tokenize.RawToken); bytes are 0-based inclusive. */
struct RawToken {
    Kind kind{};
    int64_t startbyte = 0;
    int64_t endbyte = -1;
    bool dotop = false;
    bool suffix = false;
};

/** @brief Tokenize.tokenize(text): every raw token, ending with K"EndMarker". */
std::vector<RawToken> Lex(std::string_view text);

}  // namespace jlparse

#endif  // MEP_JULIA_PARSE_H
