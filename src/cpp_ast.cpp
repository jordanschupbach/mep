// Tokenizer, preprocessor and parser for mep's own C++ language server.
// See cpp_ast.h for the shape of what this produces and for the scope
// this deliberately stops at; cpp_lsp.cpp is the analysis half above it.
//
// The file is in three parts, in translation-phase order:
//   1. TokenizeCpp -- line splices, comments, every literal form, and the
//      punctuator table.
//   2. Preprocessor -- directive parsing, conditional evaluation against
//      the macros this file defines, and macro expansion.
//   3. Parser -- declarations, statements and expressions, with the two
//      documented ambiguity heuristics and error recovery between them.

#include "cpp_ast.h"

#include "cpp_ast_internal.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

// Guards against a pathological document taking the editor's helper
// process down with it: both are generous next to any file a person
// edits, and both fail by truncating rather than by crashing.
constexpr size_t kMaxExpandedTokens = 2000000;
constexpr int kMaxMacroDepth = 40;

// --- Character classification ----------------------------------------

bool IsSpaceChar(char c) { return c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r'; }

bool IsDigitChar(char c) { return c >= '0' && c <= '9'; }

}  // namespace

bool CppPosLess(const CppPos &a, const CppPos &b) { return a.line != b.line ? a.line < b.line : a.col < b.col; }

bool IsCppIdentStart(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || c >= 0x80;
}

bool IsCppIdentChar(unsigned char c) { return IsCppIdentStart(c) || (c >= '0' && c <= '9'); }

namespace {

// --- Keywords ---------------------------------------------------------

// Every C++20 reserved keyword. `final`, `override`, `import` and
// `module` are deliberately absent: they are identifiers with special
// meaning in context, and a file may still use them as names.
const std::unordered_set<std::string> &KeywordSet() {
    static const std::unordered_set<std::string> kSet = {
        "alignas",      "alignof",   "and",       "and_eq",    "asm",          "auto",
        "bitand",       "bitor",     "bool",      "break",     "case",         "catch",
        "char",         "char8_t",   "char16_t",  "char32_t",  "class",        "co_await",
        "co_return",    "co_yield",  "compl",     "concept",   "const",        "consteval",
        "constexpr",    "constinit", "const_cast", "continue", "decltype",     "default",
        "delete",       "do",        "double",    "dynamic_cast", "else",      "enum",
        "explicit",     "export",    "extern",    "false",     "float",        "for",
        "friend",       "goto",      "if",        "inline",    "int",          "long",
        "mutable",      "namespace", "new",       "noexcept",  "not",          "not_eq",
        "nullptr",      "operator",  "or",        "or_eq",     "private",      "protected",
        "public",       "register",  "reinterpret_cast",       "requires",     "return",
        "short",        "signed",    "sizeof",    "static",    "static_assert", "static_cast",
        "struct",       "switch",    "template",  "this",      "thread_local", "throw",
        "true",         "try",       "typedef",   "typeid",    "typename",     "union",
        "unsigned",     "using",     "virtual",   "void",      "volatile",     "wchar_t",
        "while",        "xor",       "xor_eq",
        // GCC/Clang spellings a real file is full of; treating them as
        // keywords is what keeps `__attribute__((...))` and `__asm__`
        // from looking like a declaration of something.
        "__attribute__", "__asm__", "__asm", "__inline", "__inline__", "__restrict", "__restrict__",
        "__extension__", "__typeof__", "__volatile__", "__const", "__signed__", "__builtin_va_arg",
        "__declspec", "__forceinline", "__int64", "__cdecl", "__stdcall", "__attribute",
        "__alignof__", "__label__", "__real__", "__imag__", "__decltype", "__typeof",
    };
    return kSet;
}

// The alternative spellings of operators (C++ [lex.digraph]). Mapped to
// the punctuator they mean while lexing, so nothing downstream has to
// know they exist.
const std::unordered_map<std::string, std::string> &AlternativeTokens() {
    static const std::unordered_map<std::string, std::string> kMap = {
        {"and", "&&"},    {"and_eq", "&="}, {"bitand", "&"},  {"bitor", "|"}, {"compl", "~"},
        {"not", "!"},     {"not_eq", "!="}, {"or", "||"},     {"or_eq", "|="}, {"xor", "^"},
        {"xor_eq", "^="},
    };
    return kMap;
}

const std::unordered_set<std::string> &FundamentalTypeSet() {
    static const std::unordered_set<std::string> kSet = {
        "void",   "bool", "char",     "char8_t", "char16_t", "char32_t", "wchar_t", "short",
        "int",    "long", "signed",   "unsigned", "float",   "double",   "auto",    "__int64",
    };
    return kSet;
}

}  // namespace

bool IsCppKeyword(const std::string &name) { return KeywordSet().count(name) != 0; }

bool IsCppFundamentalType(const std::string &name) { return FundamentalTypeSet().count(name) != 0; }

namespace {

// --- Source map -------------------------------------------------------

// The whole file as one byte stream with every backslash-newline splice
// already removed, plus the physical (line, column) each byte came from.
// Doing this once up front is what lets the tokenizer below be a plain
// index scanner: a spliced string literal, a `//` comment continued onto
// the next line and a macro definition spread over ten lines all behave
// exactly like their unspliced equivalents, and every token still reports
// the position a reader can point at.
struct SourceMap {
    std::string text;
    std::vector<int> line;
    std::vector<int> col;
    int last_line = 0;
    int last_col = 0;
};

SourceMap BuildSourceMap(const std::vector<std::string> &lines) {
    SourceMap m;
    size_t total = 0;
    for (const std::string &l : lines) total += l.size() + 1;
    m.text.reserve(total);
    m.line.reserve(total);
    m.col.reserve(total);
    for (size_t li = 0; li < lines.size(); li++) {
        std::string body = lines[li];
        if (!body.empty() && body.back() == '\r') body.pop_back();
        const bool spliced = !body.empty() && body.back() == '\\' && li + 1 < lines.size();
        const size_t limit = spliced ? body.size() - 1 : body.size();
        for (size_t ci = 0; ci < limit; ci++) {
            m.text += body[ci];
            m.line.push_back(static_cast<int>(li));
            m.col.push_back(static_cast<int>(ci));
        }
        if (!spliced) {
            m.text += '\n';
            m.line.push_back(static_cast<int>(li));
            m.col.push_back(static_cast<int>(limit));
        }
    }
    m.last_line = lines.empty() ? 0 : static_cast<int>(lines.size()) - 1;
    m.last_col = lines.empty() ? 0 : static_cast<int>(lines.back().size());
    return m;
}

CppPos PosAt(const SourceMap &m, size_t i) {
    CppPos p;
    if (i < m.line.size()) {
        p.line = m.line[i];
        p.col = m.col[i];
    } else {
        p.line = m.last_line;
        p.col = m.last_col;
    }
    return p;
}

// --- Punctuators ------------------------------------------------------

// Longest-match first; the tokenizer walks this table in order.
const char *const kPunctuators[] = {
    "<<=", ">>=", "->*", "...", "<=>", "##",  "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
    "++",  "--",  "+=",  "-=",  "*=",  "/=",  "%=", "&=", "|=", "^=", "->", ".*", "::", "#",
    "+",   "-",   "*",   "/",   "%",   "^",   "&",  "|",  "~",  "!",  "=",  "<",  ">",  ",",
    ".",   "?",   ":",   ";",   "(",   ")",   "[",  "]",  "{",  "}",
};

bool IsStringPrefix(const std::string &s) {
    return s == "L" || s == "u" || s == "U" || s == "u8";
}

bool IsRawStringPrefix(const std::string &s) {
    return s == "R" || s == "LR" || s == "uR" || s == "UR" || s == "u8R";
}

}  // namespace

// --- Tokenizer --------------------------------------------------------

std::vector<CppToken> TokenizeCpp(const std::vector<std::string> &lines, std::vector<CppSyntaxError> *out_errors,
                                  std::vector<CppComment> *out_comments) {
    const SourceMap map = BuildSourceMap(lines);
    const std::string &s = map.text;
    std::vector<CppToken> out;
    out.reserve(s.size() / 4 + 8);

    auto error = [&](const CppPos &start, const CppPos &end, const char *code, const std::string &msg) {
        if (out_errors == nullptr) return;
        CppSyntaxError e;
        e.start = start;
        e.end = end;
        e.code = code;
        e.message = msg;
        out_errors->push_back(e);
    };

    size_t i = 0;
    bool at_line_start = true;
    bool space_before = false;
    // The directive line a token belongs to: set when a `#` is seen at the
    // start of a line, cleared at the next newline. The preprocessor reads
    // this rather than re-deriving where each directive ends, which a
    // backslash-spliced `#define` would otherwise make hard.
    int directive_line = -1;

    while (i < s.size()) {
        const char c = s[i];
        if (c == '\n') {
            i++;
            at_line_start = true;
            space_before = true;
            directive_line = -1;
            continue;
        }
        if (IsSpaceChar(c)) {
            i++;
            space_before = true;
            continue;
        }
        // Comments.
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            const size_t start = i;
            while (i < s.size() && s[i] != '\n') i++;
            if (out_comments != nullptr) {
                CppComment cm;
                cm.line = PosAt(map, start).line;
                cm.col = PosAt(map, start).col;
                cm.end_line = PosAt(map, i == 0 ? 0 : i - 1).line;
                cm.end_col = PosAt(map, i == 0 ? 0 : i - 1).col + 1;
                cm.text = s.substr(start, i - start);
                cm.own_line = at_line_start;
                cm.block = false;
                cm.doc = cm.text.rfind("///", 0) == 0 || cm.text.rfind("//!", 0) == 0;
                out_comments->push_back(cm);
            }
            space_before = true;
            continue;
        }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            const size_t start = i;
            i += 2;
            bool closed = false;
            while (i + 1 < s.size()) {
                if (s[i] == '*' && s[i + 1] == '/') {
                    i += 2;
                    closed = true;
                    break;
                }
                i++;
            }
            if (!closed) i = s.size();
            if (out_comments != nullptr) {
                CppComment cm;
                cm.line = PosAt(map, start).line;
                cm.col = PosAt(map, start).col;
                const CppPos endp = PosAt(map, i == 0 ? 0 : i - 1);
                cm.end_line = endp.line;
                cm.end_col = endp.col + 1;
                cm.text = s.substr(start, i - start);
                cm.own_line = at_line_start;
                cm.block = true;
                cm.doc = cm.text.rfind("/**", 0) == 0 || cm.text.rfind("/*!", 0) == 0;
                out_comments->push_back(cm);
            }
            if (!closed) {
                error(PosAt(map, start), PosAt(map, s.size()), "unterminated-comment",
                      "Block comment is never closed");
            }
            at_line_start = false;
            space_before = true;
            continue;
        }

        CppToken tok;
        tok.start = PosAt(map, i);
        tok.bol = at_line_start;
        tok.space_before = space_before;
        tok.directive_line = directive_line;
        const size_t start = i;

        // Identifier, keyword, or a prefixed string/character literal.
        if (IsCppIdentStart(static_cast<unsigned char>(c))) {
            size_t j = i;
            while (j < s.size() && IsCppIdentChar(static_cast<unsigned char>(s[j]))) j++;
            const std::string word = s.substr(i, j - i);
            const bool quote_follows = j < s.size() && (s[j] == '"' || s[j] == '\'');
            if (quote_follows && (IsStringPrefix(word) || IsRawStringPrefix(word))) {
                // Fall through to the literal scanners below with `i`
                // still at the prefix; they re-read it from `word`.
                if (IsRawStringPrefix(word) && s[j] == '"') {
                    // R"delim( ... )delim" -- the one literal whose
                    // terminator is chosen by the file itself, and the one
                    // that may legally contain unescaped newlines.
                    size_t k = j + 1;
                    std::string delim;
                    while (k < s.size() && s[k] != '(' && s[k] != '\n' && delim.size() < 16) {
                        delim += s[k];
                        k++;
                    }
                    bool closed = false;
                    if (k < s.size() && s[k] == '(') {
                        const std::string closer = ")" + delim + "\"";
                        const size_t at = s.find(closer, k + 1);
                        if (at != std::string::npos) {
                            k = at + closer.size();
                            closed = true;
                        } else {
                            k = s.size();
                        }
                    }
                    tok.kind = CppTokKind::String;
                    tok.unterminated = !closed;
                    i = k;
                    // A user-defined literal suffix binds to the literal.
                    while (i < s.size() && IsCppIdentChar(static_cast<unsigned char>(s[i]))) i++;
                    tok.text = s.substr(start, i - start);
                    tok.end = PosAt(map, i);
                    if (!closed) {
                        error(tok.start, tok.end, "unterminated-raw-string",
                              "Raw string literal is never closed");
                    }
                    out.push_back(tok);
                    at_line_start = false;
                    space_before = false;
                    continue;
                }
                i = j;  // a prefixed "..." or '...': scanned below
            } else {
                tok.kind = IsCppKeyword(word) ? CppTokKind::Keyword : CppTokKind::Ident;
                const auto alt = AlternativeTokens().find(word);
                if (alt != AlternativeTokens().end()) {
                    // `and`, `not_eq`, ... are the operators they spell.
                    tok.kind = CppTokKind::Op;
                    tok.text = alt->second;
                    i = j;
                    tok.end = PosAt(map, i);
                    out.push_back(tok);
                    at_line_start = false;
                    space_before = false;
                    continue;
                }
                i = j;
                tok.text = word;
                tok.end = PosAt(map, i);
                out.push_back(tok);
                at_line_start = false;
                space_before = false;
                continue;
            }
        }

        // Preprocessing number: a digit, or a `.` immediately followed by
        // one. The grammar is deliberately loose (`0x1p-3`, `1'000'000`,
        // `1.0e+5f`, `123_km`) because a pp-number is whatever a compiler
        // hands to the literal parser, not a validated value.
        if (IsDigitChar(s[i]) || (s[i] == '.' && i + 1 < s.size() && IsDigitChar(s[i + 1]))) {
            size_t j = i;
            if (s[j] == '.') j++;
            while (j < s.size()) {
                const char d = s[j];
                if (IsCppIdentChar(static_cast<unsigned char>(d)) || d == '.') {
                    const bool exponent = (d == 'e' || d == 'E' || d == 'p' || d == 'P') && j + 1 < s.size() &&
                                          (s[j + 1] == '+' || s[j + 1] == '-');
                    j += exponent ? 2 : 1;
                    continue;
                }
                // A digit separator, not a character literal: `1'000`.
                if (d == '\'' && j + 1 < s.size() && IsCppIdentChar(static_cast<unsigned char>(s[j + 1]))) {
                    j += 2;
                    continue;
                }
                break;
            }
            tok.kind = CppTokKind::Number;
            i = j;
            tok.text = s.substr(start, i - start);
            tok.end = PosAt(map, i);
            out.push_back(tok);
            at_line_start = false;
            space_before = false;
            continue;
        }

        // String and character literals (the prefix, if any, was already
        // stepped over above).
        if (s[i] == '"' || s[i] == '\'') {
            const char quote = s[i];
            size_t j = i + 1;
            bool closed = false;
            while (j < s.size() && s[j] != '\n') {
                if (s[j] == '\\' && j + 1 < s.size() && s[j + 1] != '\n') {
                    j += 2;
                    continue;
                }
                if (s[j] == quote) {
                    j++;
                    closed = true;
                    break;
                }
                j++;
            }
            i = j;
            while (closed && i < s.size() && IsCppIdentChar(static_cast<unsigned char>(s[i]))) i++;
            tok.kind = quote == '"' ? CppTokKind::String : CppTokKind::Char;
            tok.unterminated = !closed;
            tok.text = s.substr(start, i - start);
            tok.end = PosAt(map, i);
            if (!closed) {
                error(tok.start, tok.end, quote == '"' ? "unterminated-string" : "unterminated-char",
                      quote == '"' ? "String literal is never closed" : "Character literal is never closed");
            }
            out.push_back(tok);
            at_line_start = false;
            space_before = false;
            continue;
        }

        // Punctuators.
        bool matched = false;
        for (const char *p : kPunctuators) {
            const size_t n = std::strlen(p);
            if (s.compare(i, n, p) == 0) {
                tok.kind = CppTokKind::Op;
                tok.text = p;
                i += n;
                tok.end = PosAt(map, i);
                if (tok.text == "#" && tok.bol) directive_line = tok.start.line;
                tok.directive_line = directive_line;
                out.push_back(tok);
                matched = true;
                break;
            }
        }
        if (matched) {
            at_line_start = false;
            space_before = false;
            continue;
        }

        // Anything else is not a C++ token at all. Reported once and
        // skipped: a stray backtick from a pasted diff should cost one
        // diagnostic, not the rest of the file.
        i++;
        tok.kind = CppTokKind::Op;
        tok.text = s.substr(start, i - start);
        tok.end = PosAt(map, i);
        error(tok.start, tok.end, "stray-character",
              "Stray '" + tok.text + "' in source: C++ has no token that starts with it");
        at_line_start = false;
        space_before = false;
    }

    CppToken end_tok;
    end_tok.kind = CppTokKind::End;
    end_tok.start = PosAt(map, s.size());
    end_tok.end = end_tok.start;
    end_tok.bol = true;
    out.push_back(end_tok);
    return out;
}

// === Preprocessor =====================================================
//
// Translation phase 4, to the extent a language server can honestly do
// it. The rules this implements, and the one place it deliberately
// guesses:
//   - `#if`/`#ifdef` groups are evaluated against the macros the file
//     itself defines plus a host-shaped predefined set (see
//     PredefinedMacros). An identifier nothing defines is 0, exactly as
//     the standard says -- but the group is marked `certain == false`,
//     because a build system's `-DHAVE_FOO` would have flipped it, and
//     the analysis half must not report an unused anything inside a
//     region it may simply have guessed away.
//   - `__has_include(...)` answers 1 for a header it cannot look up. The
//     file in front of us was written to compile somewhere, so assuming
//     its includes exist is the reading that produces the code its author
//     meant, rather than the fallback branch.
//   - Macros this file defines are expanded, object-like and
//     function-like both, with `#` and `##`. Macros from headers nobody
//     read stay identifiers: see cpp_ast.h on why the declaration parser
//     is written to survive that.

namespace {

// Macros a hosted GCC/Clang on this machine would already have defined.
// Shapes the `#if` decisions toward "what this file compiles to here",
// which is the reading the person editing it is looking at.
struct PredefEntry {
    const char *name;
    const char *value;
};

const PredefEntry kPredefined[] = {
    {"__cplusplus", "202002L"}, {"__STDC_HOSTED__", "1"},   {"__GNUC__", "13"},
    {"__GNUC_MINOR__", "2"},    {"__linux__", "1"},         {"__unix__", "1"},
    {"__unix", "1"},            {"__linux", "1"},           {"__ELF__", "1"},
    {"__x86_64__", "1"},        {"__x86_64", "1"},          {"__amd64__", "1"},
    {"__LP64__", "1"},          {"_LP64", "1"},             {"__CHAR_BIT__", "8"},
    {"__SIZEOF_INT__", "4"},    {"__SIZEOF_LONG__", "8"},   {"__SIZEOF_POINTER__", "8"},
    {"__BYTE_ORDER__", "1234"}, {"__ORDER_LITTLE_ENDIAN__", "1234"},
    {"__ORDER_BIG_ENDIAN__", "4321"},
    {"__STDCPP_DEFAULT_NEW_ALIGNMENT__", "16"},
    {"__EXCEPTIONS", "1"},      {"__GXX_RTTI", "1"},
    // The language feature-test macros a C++20 front end defines. They
    // are what a standard header checks before deciding whether the
    // feature it wraps exists, so leaving them out sends several headers
    // down their "this compiler is too old" branch.
    {"__cpp_concepts", "202002L"},          {"__cpp_impl_coroutine", "201902L"},
    {"__cpp_constexpr", "201907L"},         {"__cpp_consteval", "201811L"},
    {"__cpp_constinit", "201907L"},         {"__cpp_deduction_guides", "201907L"},
    {"__cpp_if_constexpr", "201606L"},      {"__cpp_inline_variables", "201606L"},
    {"__cpp_structured_bindings", "201606L"}, {"__cpp_fold_expressions", "201603L"},
    {"__cpp_variadic_templates", "200704L"}, {"__cpp_rvalue_references", "200610L"},
    {"__cpp_range_based_for", "201603L"},   {"__cpp_alias_templates", "200704L"},
    {"__cpp_lambdas", "200907L"},           {"__cpp_generic_lambdas", "201707L"},
    {"__cpp_init_captures", "201803L"},     {"__cpp_noexcept_function_type", "201510L"},
    {"__cpp_nontype_template_parameter_auto", "201606L"},
    {"__cpp_designated_initializers", "201707L"},
    {"__cpp_char8_t", "201811L"},           {"__cpp_conditional_explicit", "201806L"},
    {"__cpp_aggregate_paren_init", "201902L"},
    {"__cpp_using_enum", "201907L"},        {"__cpp_modules", "201907L"},
    {"__cpp_three_way_comparison", "201907L"},
    {"__cpp_exceptions", "199711L"},        {"__cpp_rtti", "199711L"},
    {"__cpp_threadsafe_static_init", "200806L"},
    // The macros a compiler substitutes rather than defines. Their values
    // are placeholders -- nothing here evaluates them -- but their
    // *existence* matters: a file using `__FILE__` must not be told it
    // made the name up.
    {"__FILE__", "\"file\""},
    {"__LINE__", "1"},
    {"__DATE__", "\"date\""},
    {"__TIME__", "\"time\""},
    {"__COUNTER__", "0"},
    {"__BASE_FILE__", "\"file\""},
    {"__INCLUDE_LEVEL__", "0"},
};

/** @brief Builds a one-token replacement list, for a predefined macro's value. */
std::vector<CppToken> ValueTokens(const std::string &value) {
    std::vector<CppToken> body;
    if (value.empty()) return body;
    CppToken t;
    t.kind = CppTokKind::Ident;
    if (IsDigitChar(value[0])) t.kind = CppTokKind::Number;
    if (value[0] == '"') t.kind = CppTokKind::String;
    t.text = value;
    body.push_back(t);
    return body;
}

using MacroTable = std::unordered_map<std::string, CppMacro>;

MacroTable PredefinedMacros(const std::vector<std::string> &extra) {
    MacroTable table;
    for (const PredefEntry &e : kPredefined) {
        CppMacro m;
        m.name = e.name;
        m.body = ValueTokens(e.value);
        m.predefined = true;
        table[m.name] = m;
    }
    for (const std::string &def : extra) {
        const size_t eq = def.find('=');
        CppMacro m;
        m.name = eq == std::string::npos ? def : def.substr(0, eq);
        m.body = ValueTokens(eq == std::string::npos ? std::string("1") : def.substr(eq + 1));
        m.predefined = true;
        if (!m.name.empty()) table[m.name] = m;
    }
    return table;
}

CppDirectiveKind DirectiveKindOf(const std::string &word) {
    if (word == "include" || word == "include_next" || word == "import") return CppDirectiveKind::Include;
    if (word == "define") return CppDirectiveKind::Define;
    if (word == "undef") return CppDirectiveKind::Undef;
    if (word == "if") return CppDirectiveKind::If;
    if (word == "ifdef") return CppDirectiveKind::Ifdef;
    if (word == "ifndef") return CppDirectiveKind::Ifndef;
    if (word == "elif") return CppDirectiveKind::Elif;
    if (word == "elifdef") return CppDirectiveKind::ElifDef;
    if (word == "elifndef") return CppDirectiveKind::ElifNdef;
    if (word == "else") return CppDirectiveKind::Else;
    if (word == "endif") return CppDirectiveKind::Endif;
    if (word == "pragma") return CppDirectiveKind::Pragma;
    if (word == "error") return CppDirectiveKind::Error;
    if (word == "warning") return CppDirectiveKind::Warning;
    if (word == "line") return CppDirectiveKind::Line;
    return CppDirectiveKind::Unknown;
}

// --- Conditional-expression evaluation --------------------------------

// A value plus whether it was reached without guessing. `certain` is
// false as soon as an identifier nothing in this file defines takes part,
// which is the honest answer for `#ifdef HAVE_ZLIB` in a file that has
// never seen its build system.
struct CondValue {
    long long value = 0;
    bool certain = true;
};

long long ParseIntegerLiteral(const std::string &text) {
    std::string digits;
    for (char c : text) {
        if (c != '\'') digits += c;
    }
    // Strip the suffix letters an integer literal may carry.
    size_t n = digits.size();
    while (n > 0) {
        const char c = digits[n - 1];
        if (c == 'u' || c == 'U' || c == 'l' || c == 'L' || c == 'z' || c == 'Z') {
            n--;
            continue;
        }
        break;
    }
    digits.resize(n);
    if (digits.empty()) return 0;
    if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'b' || digits[1] == 'B')) {
        long long v = 0;
        for (size_t k = 2; k < digits.size(); k++) v = v * 2 + (digits[k] == '1' ? 1 : 0);
        return v;
    }
    return std::strtoll(digits.c_str(), nullptr, 0);
}

long long ParseCharLiteral(const std::string &text) {
    const size_t open = text.find('\'');
    if (open == std::string::npos || open + 1 >= text.size()) return 0;
    size_t k = open + 1;
    if (text[k] == '\\' && k + 1 < text.size()) {
        switch (text[k + 1]) {
            case 'n': return '\n';
            case 't': return '\t';
            case 'r': return '\r';
            case '0': return 0;
            case '\\': return '\\';
            case '\'': return '\'';
            default: return static_cast<unsigned char>(text[k + 1]);
        }
    }
    return static_cast<unsigned char>(text[k]);
}

// A recursive-descent evaluator over already-expanded condition tokens.
// Integer-only, as `#if` is: no floats, no sizeof, no casts, and every
// remaining identifier is 0.
class CondEvaluator {
public:
    CondEvaluator(const std::vector<CppToken> &tokens, bool *certain) : t_(tokens), certain_(certain) {}

    long long Run() {
        const long long v = Conditional(0);
        return v;
    }

private:
    const std::vector<CppToken> &t_;
    bool *certain_;
    size_t i_ = 0;

    bool AtEnd() const { return i_ >= t_.size(); }
    const std::string &Peek() const {
        static const std::string kEmpty;
        return AtEnd() ? kEmpty : t_[i_].text;
    }
    bool Eat(const char *op) {
        if (!AtEnd() && t_[i_].kind == CppTokKind::Op && t_[i_].text == op) {
            i_++;
            return true;
        }
        return false;
    }

    long long Conditional(int depth) {
        const long long cond = Binary(0, depth);
        if (Eat("?")) {
            const long long a = Conditional(depth + 1);
            long long b = 0;
            if (Eat(":")) b = Conditional(depth + 1);
            return cond != 0 ? a : b;
        }
        return cond;
    }

    static int PrecedenceOf(const std::string &op) {
        if (op == "||") return 1;
        if (op == "&&") return 2;
        if (op == "|") return 3;
        if (op == "^") return 4;
        if (op == "&") return 5;
        if (op == "==" || op == "!=") return 6;
        if (op == "<" || op == ">" || op == "<=" || op == ">=") return 7;
        if (op == "<<" || op == ">>") return 8;
        if (op == "+" || op == "-") return 9;
        if (op == "*" || op == "/" || op == "%") return 10;
        return 0;
    }

    long long Binary(int min_prec, int depth) {
        if (depth > 64) return 0;
        long long lhs = Unary(depth);
        while (!AtEnd() && t_[i_].kind == CppTokKind::Op) {
            const std::string op = t_[i_].text;
            const int prec = PrecedenceOf(op);
            if (prec == 0 || prec < min_prec) break;
            i_++;
            const long long rhs = Binary(prec + 1, depth + 1);
            if (op == "||") lhs = (lhs != 0 || rhs != 0) ? 1 : 0;
            else if (op == "&&") lhs = (lhs != 0 && rhs != 0) ? 1 : 0;
            else if (op == "|") lhs = lhs | rhs;
            else if (op == "^") lhs = lhs ^ rhs;
            else if (op == "&") lhs = lhs & rhs;
            else if (op == "==") lhs = lhs == rhs ? 1 : 0;
            else if (op == "!=") lhs = lhs != rhs ? 1 : 0;
            else if (op == "<") lhs = lhs < rhs ? 1 : 0;
            else if (op == ">") lhs = lhs > rhs ? 1 : 0;
            else if (op == "<=") lhs = lhs <= rhs ? 1 : 0;
            else if (op == ">=") lhs = lhs >= rhs ? 1 : 0;
            else if (op == "<<") lhs = rhs >= 0 && rhs < 64 ? lhs << rhs : 0;
            else if (op == ">>") lhs = rhs >= 0 && rhs < 64 ? lhs >> rhs : 0;
            else if (op == "+") lhs = lhs + rhs;
            else if (op == "-") lhs = lhs - rhs;
            else if (op == "*") lhs = lhs * rhs;
            else if (op == "/") lhs = rhs == 0 ? 0 : lhs / rhs;
            else if (op == "%") lhs = rhs == 0 ? 0 : lhs % rhs;
        }
        return lhs;
    }

    long long Unary(int depth) {
        if (depth > 64 || AtEnd()) return 0;
        if (Eat("!")) return Unary(depth + 1) == 0 ? 1 : 0;
        if (Eat("-")) return -Unary(depth + 1);
        if (Eat("+")) return Unary(depth + 1);
        if (Eat("~")) return ~Unary(depth + 1);
        if (Eat("(")) {
            const long long v = Conditional(depth + 1);
            Eat(")");
            return v;
        }
        const CppToken &tok = t_[i_];
        i_++;
        if (tok.kind == CppTokKind::Number) return ParseIntegerLiteral(tok.text);
        if (tok.kind == CppTokKind::Char) return ParseCharLiteral(tok.text);
        if (tok.kind == CppTokKind::Keyword && tok.text == "true") return 1;
        if (tok.kind == CppTokKind::Keyword && tok.text == "false") return 0;
        if (tok.kind == CppTokKind::Ident || tok.kind == CppTokKind::Keyword) {
            // `__has_include(<x>)` and friends: a file that asks was
            // written to compile with them present (see this section's
            // header comment), so answer yes and skip the argument.
            const bool has_query = tok.text.rfind("__has_", 0) == 0;
            if (!AtEnd() && t_[i_].kind == CppTokKind::Op && t_[i_].text == "(") {
                int depth_parens = 0;
                do {
                    if (t_[i_].kind == CppTokKind::Op && t_[i_].text == "(") depth_parens++;
                    if (t_[i_].kind == CppTokKind::Op && t_[i_].text == ")") depth_parens--;
                    i_++;
                } while (!AtEnd() && depth_parens > 0);
                if (has_query) return 1;
                if (certain_ != nullptr) *certain_ = false;
                return 0;
            }
            if (has_query) return 1;
            // An identifier nothing defined: 0 by the standard, and a
            // guess by this server's lights.
            if (certain_ != nullptr) *certain_ = false;
            return 0;
        }
        return 0;
    }
};

}  // namespace

namespace {

// --- Macro expansion --------------------------------------------------

/** @brief Renders an argument's tokens the way `#x` must spell them. */
std::string StringizeTokens(const std::vector<CppToken> &arg) {
    std::string out = "\"";
    for (size_t k = 0; k < arg.size(); k++) {
        if (k > 0 && arg[k].space_before) out += ' ';
        for (char c : arg[k].text) {
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
    }
    out += '"';
    return out;
}

/** @brief Classifies a token produced by `##`, whose two halves may have had different kinds. */
CppTokKind KindOfPasted(const std::string &text) {
    if (text.empty()) return CppTokKind::Op;
    if (IsDigitChar(text[0])) return CppTokKind::Number;
    if (IsCppIdentStart(static_cast<unsigned char>(text[0]))) {
        return IsCppKeyword(text) ? CppTokKind::Keyword : CppTokKind::Ident;
    }
    if (text[0] == '"') return CppTokKind::String;
    if (text[0] == '\'') return CppTokKind::Char;
    return CppTokKind::Op;
}

// The expander. Deliberately a simplification of [cpp.replace]: it has no
// hide sets, only a stack of the macros currently being expanded, so a
// mutually recursive pair stops one expansion later than a conformant
// preprocessor would. Nothing downstream can tell the difference, and the
// depth and token budgets mean even a deliberately explosive macro costs
// a bounded amount of work.
class MacroExpander {
public:
    MacroTable macros;
    size_t budget = kMaxExpandedTokens;
    bool truncated = false;

    /** @brief Expands every macro invocation in `in`, appending the result to `out`. */
    void ExpandAll(const std::vector<CppToken> &in, std::vector<CppToken> &out, std::vector<std::string> &stack,
                   int depth) {
        size_t i = 0;
        while (i < in.size()) {
            const size_t consumed = TryExpand(in, i, out, stack, depth);
            if (consumed == 0) {
                if (budget == 0) {
                    truncated = true;
                    return;
                }
                budget--;
                out.push_back(in[i]);
                i++;
            } else {
                i += consumed;
            }
        }
    }

    /**
     * @brief Expands one invocation starting at `i`, if there is one.
     * @return the number of input tokens consumed, or 0 when `in[i]` is not a macro invocation
     */
    size_t TryExpand(const std::vector<CppToken> &in, size_t i, std::vector<CppToken> &out,
                     std::vector<std::string> &stack, int depth) {
        if (in[i].kind != CppTokKind::Ident) return 0;
        const auto it = macros.find(in[i].text);
        if (it == macros.end()) return 0;
        if (depth > kMaxMacroDepth || budget == 0) {
            truncated = true;
            return 0;
        }
        for (const std::string &active : stack) {
            if (active == in[i].text) return 0;  // no self-reference, per [cpp.rescan]
        }
        const CppMacro &macro = it->second;
        std::vector<std::vector<CppToken>> args;
        size_t consumed = 1;
        if (macro.function_like) {
            size_t j = i + 1;
            if (j >= in.size() || in[j].kind != CppTokKind::Op || in[j].text != "(") return 0;
            j++;
            int nest = 1;
            std::vector<CppToken> current;
            while (j < in.size() && nest > 0) {
                const CppToken &tok = in[j];
                if (tok.kind == CppTokKind::Op) {
                    if (tok.text == "(" || tok.text == "[" || tok.text == "{") nest++;
                    else if (tok.text == ")" || tok.text == "]" || tok.text == "}") nest--;
                    if (nest == 0) break;
                    const bool splitting = tok.text == "," && nest == 1 &&
                                           (!macro.variadic || args.size() + 1 < macro.params.size());
                    if (splitting) {
                        args.push_back(current);
                        current.clear();
                        j++;
                        continue;
                    }
                }
                current.push_back(tok);
                j++;
            }
            if (j >= in.size()) return 0;  // an invocation that never closes: leave it alone
            if (!current.empty() || !args.empty() || !macro.params.empty()) args.push_back(current);
            consumed = j + 1 - i;
        }
        const std::vector<CppToken> substituted = Substitute(macro, args, in[i]);
        std::vector<CppToken> expanded;
        stack.push_back(macro.name);
        ExpandAll(substituted, expanded, stack, depth + 1);
        stack.pop_back();
        // Every token the expansion produced points at the invocation, so
        // a diagnostic on one lands where the reader can see it.
        const CppPos site_start = in[i].start;
        const CppPos site_end = in[i + consumed - 1].end;
        for (CppToken &tok : expanded) {
            tok.from_macro = true;
            tok.start = site_start;
            tok.end = site_end;
            tok.bol = false;
            tok.directive_line = -1;
        }
        if (!expanded.empty() && in[i].space_before) expanded.front().space_before = true;
        out.insert(out.end(), expanded.begin(), expanded.end());
        return consumed;
    }

private:
    /** @brief Replaces a macro's parameters with its arguments, honouring `#`, `##` and `__VA_OPT__`. */
    std::vector<CppToken> Substitute(const CppMacro &macro, const std::vector<std::vector<CppToken>> &args,
                                     const CppToken &site) {
        // A token with empty text standing in for an omitted argument, so
        // `a ## b` still knows it had two halves ([cpp.concat]'s
        // placemarker).
        CppToken placemarker;
        placemarker.kind = CppTokKind::Op;
        placemarker.start = site.start;
        placemarker.end = site.end;

        auto arg_index = [&](const std::string &name) -> int {
            for (size_t k = 0; k < macro.params.size(); k++) {
                if (macro.params[k] == name) return static_cast<int>(k);
            }
            if (macro.variadic && name == "__VA_ARGS__" && !macro.params.empty()) {
                return static_cast<int>(macro.params.size()) - 1;
            }
            return -1;
        };
        auto arg_at = [&](int index) -> const std::vector<CppToken> & {
            static const std::vector<CppToken> kEmpty;
            if (index < 0 || static_cast<size_t>(index) >= args.size()) return kEmpty;
            return args[static_cast<size_t>(index)];
        };
        const bool have_variadic_args =
            macro.variadic && !macro.params.empty() && !arg_at(static_cast<int>(macro.params.size()) - 1).empty();

        std::vector<CppToken> seq;
        for (size_t k = 0; k < macro.body.size(); k++) {
            const CppToken &tok = macro.body[k];
            // `__VA_OPT__(...)`: the contents survive only when the
            // variadic argument was non-empty.
            if (tok.kind == CppTokKind::Ident && tok.text == "__VA_OPT__" && k + 1 < macro.body.size() &&
                macro.body[k + 1].text == "(") {
                size_t j = k + 2;
                int nest = 1;
                std::vector<CppToken> inner;
                while (j < macro.body.size() && nest > 0) {
                    if (macro.body[j].text == "(") nest++;
                    if (macro.body[j].text == ")") {
                        nest--;
                        if (nest == 0) break;
                    }
                    inner.push_back(macro.body[j]);
                    j++;
                }
                if (have_variadic_args) {
                    for (const CppToken &inner_tok : inner) {
                        const int index = arg_index(inner_tok.text);
                        if (index >= 0) {
                            const std::vector<CppToken> &a = arg_at(index);
                            seq.insert(seq.end(), a.begin(), a.end());
                        } else {
                            seq.push_back(inner_tok);
                        }
                    }
                }
                k = j;
                continue;
            }
            // `#param`: the argument's spelling, not its expansion.
            if (tok.kind == CppTokKind::Op && tok.text == "#" && k + 1 < macro.body.size()) {
                const int index = arg_index(macro.body[k + 1].text);
                if (index >= 0) {
                    CppToken str = tok;
                    str.kind = CppTokKind::String;
                    str.text = StringizeTokens(arg_at(index));
                    seq.push_back(str);
                    k++;
                    continue;
                }
            }
            const int index = arg_index(tok.text);
            if (index >= 0) {
                const bool pasted = (k + 1 < macro.body.size() && macro.body[k + 1].text == "##") ||
                                    (k > 0 && macro.body[k - 1].text == "##");
                std::vector<CppToken> value = arg_at(index);
                if (!pasted && !value.empty()) {
                    // An argument is macro-expanded before substitution
                    // ([cpp.subst]) -- except next to `##`.
                    std::vector<CppToken> expanded_arg;
                    std::vector<std::string> stack;
                    ExpandAll(value, expanded_arg, stack, kMaxMacroDepth - 4);
                    value = expanded_arg;
                }
                if (value.empty()) {
                    seq.push_back(placemarker);
                } else {
                    seq.insert(seq.end(), value.begin(), value.end());
                }
                continue;
            }
            seq.push_back(tok);
        }

        // The paste pass. `,##__VA_ARGS__` with no variadic argument
        // drops the comma too -- the GNU extension every C codebase of a
        // certain age relies on.
        std::vector<CppToken> out;
        for (size_t k = 0; k < seq.size(); k++) {
            if (seq[k].kind == CppTokKind::Op && seq[k].text == "##" && !out.empty() && k + 1 < seq.size()) {
                CppToken lhs = out.back();
                out.pop_back();
                const CppToken &rhs = seq[k + 1];
                k++;
                if (lhs.text.empty()) {
                    if (!rhs.text.empty()) out.push_back(rhs);
                    continue;
                }
                if (rhs.text.empty()) {
                    if (lhs.text == "," && macro.variadic) continue;
                    out.push_back(lhs);
                    continue;
                }
                CppToken merged = lhs;
                merged.text = lhs.text + rhs.text;
                merged.kind = KindOfPasted(merged.text);
                out.push_back(merged);
                continue;
            }
            if (seq[k].text.empty() && seq[k].kind == CppTokKind::Op) continue;  // a leftover placemarker
            out.push_back(seq[k]);
        }
        return out;
    }
};

// --- Directive parsing ------------------------------------------------

/** @brief Reads one `#...` line out of the raw token stream. */
CppDirective ReadDirective(const std::vector<CppToken> &raw, size_t start, size_t *out_next) {
    CppDirective d;
    const int line = raw[start].start.line;
    d.start = raw[start].start;
    d.line = line;
    d.end = raw[start].end;
    d.end_line = line;
    size_t i = start + 1;
    const int directive_line = raw[start].directive_line;
    auto in_directive = [&](size_t k) {
        return k < raw.size() && raw[k].kind != CppTokKind::End && raw[k].directive_line == directive_line;
    };
    if (in_directive(i) && (raw[i].kind == CppTokKind::Ident || raw[i].kind == CppTokKind::Keyword)) {
        d.name = raw[i].text;
        d.kind = DirectiveKindOf(d.name);
        i++;
    } else if (!in_directive(i)) {
        d.kind = CppDirectiveKind::None;
    }
    const size_t body_start = i;
    while (in_directive(i)) {
        d.end = raw[i].end;
        d.end_line = raw[i].end.line;
        i++;
    }
    d.body.assign(raw.begin() + static_cast<long>(body_start), raw.begin() + static_cast<long>(i));
    *out_next = i;

    if (d.kind == CppDirectiveKind::Include) {
        // `#include <a/b.h>` is several tokens by the time it gets here
        // (`<`, `a`, `/`, `b`, `.`, `h`, `>`), so the header name is
        // rebuilt from their spelling rather than read off one token.
        if (!d.body.empty() && d.body.front().kind == CppTokKind::String &&
            d.body.front().text.size() >= 2 && d.body.front().text.front() == '"') {
            d.header = d.body.front().text.substr(1, d.body.front().text.size() - 2);
            d.angled = false;
        } else if (!d.body.empty() && d.body.front().text == "<") {
            d.angled = true;
            for (size_t k = 1; k < d.body.size(); k++) {
                if (d.body[k].text == ">") break;
                if (k > 1 && d.body[k].space_before) d.header += ' ';
                d.header += d.body[k].text;
            }
        }
    } else if (d.kind == CppDirectiveKind::Define && !d.body.empty()) {
        d.name = d.body.front().text;
        size_t k = 1;
        if (k < d.body.size() && d.body[k].kind == CppTokKind::Op && d.body[k].text == "(" &&
            !d.body[k].space_before) {
            d.function_like = true;
            k++;
            while (k < d.body.size() && d.body[k].text != ")") {
                if (d.body[k].text == "...") {
                    d.variadic = true;
                    d.params.push_back("__VA_ARGS__");
                } else if (d.body[k].kind == CppTokKind::Ident) {
                    // A named variadic parameter, GNU style: `args...`.
                    if (k + 1 < d.body.size() && d.body[k + 1].text == "...") {
                        d.variadic = true;
                        d.params.push_back(d.body[k].text);
                        k++;
                    } else {
                        d.params.push_back(d.body[k].text);
                    }
                }
                k++;
            }
            if (k < d.body.size()) k++;  // the `)`
        }
        d.body.erase(d.body.begin(), d.body.begin() + static_cast<long>(std::min(k, d.body.size())));
    } else if (d.kind == CppDirectiveKind::Undef || d.kind == CppDirectiveKind::Ifdef ||
               d.kind == CppDirectiveKind::Ifndef || d.kind == CppDirectiveKind::ElifDef ||
               d.kind == CppDirectiveKind::ElifNdef) {
        if (!d.body.empty()) d.name = d.body.front().text;
    } else if (d.kind == CppDirectiveKind::Pragma && !d.body.empty()) {
        d.name = d.body.front().text;
    }
    return d;
}

// One level of `#if` nesting while the preprocessor walks the file.
struct CondFrame {
    bool parent_active = true;
    bool taken = false;   // some group in this chain was already active
    bool active = false;  // this group is the active one
    bool certain = true;
    size_t if_index = 0;   // index into the directive list, for #endif to close
    int group_line = 0;    // the line the current group opened on
};

}  // namespace

void CppPreprocess(const std::vector<CppToken> &raw, const std::vector<std::string> &lines,
                   const CppParseOptions &opts, std::vector<CppSyntaxError> *out_errors,
                   CppPreprocessOutput *out) {
    auto error = [&](const CppPos &start, const CppPos &end, const char *code, const std::string &msg) {
        if (out_errors == nullptr) return;
        CppSyntaxError e;
        e.start = start;
        e.end = end;
        e.code = code;
        e.message = msg;
        out_errors->push_back(e);
    };

    MacroExpander expander;
    expander.macros = PredefinedMacros(opts.defines);
    out->line_active.assign(lines.size(), true);
    auto deactivate = [&](int from_line, int to_line) {
        for (int l = from_line; l < to_line && l >= 0; l++) {
            if (static_cast<size_t>(l) < out->line_active.size()) out->line_active[static_cast<size_t>(l)] = false;
        }
    };

    std::vector<CondFrame> stack;
    // The tokens that survive conditional compilation, plus the points at
    // which the macro table changes: expansion runs as a second pass, so
    // a `#define` in the middle of the file has to be replayed at exactly
    // the right token.
    std::vector<CppToken> kept;
    std::vector<std::pair<size_t, size_t>> events;  // (index into `kept`, index into directives)

    auto currently_active = [&]() {
        for (const CondFrame &f : stack) {
            if (!f.active) return false;
        }
        return true;
    };
    auto currently_certain = [&]() {
        for (const CondFrame &f : stack) {
            if (!f.certain) return false;
        }
        return true;
    };

    size_t i = 0;
    while (i < raw.size() && raw[i].kind != CppTokKind::End) {
        if (raw[i].directive_line < 0 || raw[i].text != "#" || !raw[i].bol) {
            if (currently_active()) kept.push_back(raw[i]);
            i++;
            continue;
        }
        size_t next = i;
        CppDirective d = ReadDirective(raw, i, &next);
        i = next;
        d.depth = static_cast<int>(stack.size());
        const bool parent_active = currently_active();
        d.active = parent_active;
        d.certain = currently_certain();

        switch (d.kind) {
            case CppDirectiveKind::If:
            case CppDirectiveKind::Ifdef:
            case CppDirectiveKind::Ifndef: {
                CondFrame frame;
                frame.parent_active = parent_active;
                frame.if_index = out->directives.size();
                frame.group_line = d.line;
                bool certain = true;
                bool value = false;
                if (d.kind == CppDirectiveKind::If) {
                    std::vector<CppToken> condition;
                    // `defined X` / `defined(X)` is answered before
                    // expansion, or the macro would eat its own argument.
                    for (size_t k = 0; k < d.body.size(); k++) {
                        if (d.body[k].text == "defined") {
                            size_t j = k + 1;
                            bool paren = false;
                            if (j < d.body.size() && d.body[j].text == "(") {
                                paren = true;
                                j++;
                            }
                            std::string name;
                            if (j < d.body.size()) name = d.body[j].text;
                            if (paren) {
                                while (j < d.body.size() && d.body[j].text != ")") j++;
                            }
                            CppToken lit = d.body[k];
                            lit.kind = CppTokKind::Number;
                            const auto found = expander.macros.find(name);
                            lit.text = found != expander.macros.end() ? "1" : "0";
                            if (found == expander.macros.end() || found->second.predefined) {
                                // Whether a macro a build system would
                                // have supplied is defined is exactly
                                // what this server cannot know.
                                if (found == expander.macros.end()) certain = false;
                            }
                            condition.push_back(lit);
                            k = j;
                            continue;
                        }
                        condition.push_back(d.body[k]);
                    }
                    std::vector<CppToken> expanded;
                    std::vector<std::string> expand_stack;
                    expander.ExpandAll(condition, expanded, expand_stack, 0);
                    CondEvaluator eval(expanded, &certain);
                    value = eval.Run() != 0;
                } else {
                    const bool defined = expander.macros.count(d.name) != 0;
                    value = d.kind == CppDirectiveKind::Ifdef ? defined : !defined;
                    if (!defined) certain = false;
                }
                frame.certain = d.certain && certain;
                frame.active = parent_active && value;
                frame.taken = frame.active;
                d.active = frame.active;
                d.certain = frame.certain;
                stack.push_back(frame);
                break;
            }
            case CppDirectiveKind::Elif:
            case CppDirectiveKind::ElifDef:
            case CppDirectiveKind::ElifNdef:
            case CppDirectiveKind::Else: {
                if (stack.empty()) {
                    error(d.start, d.end, "pp-stray-conditional",
                          "#" + d.name + " without a matching #if");
                    break;
                }
                CondFrame &frame = stack.back();
                if (!frame.active) deactivate(frame.group_line + 1, d.line);
                bool value = !frame.taken;
                if (d.kind == CppDirectiveKind::Elif) {
                    bool certain = frame.certain;
                    std::vector<CppToken> condition;
                    for (size_t k = 0; k < d.body.size(); k++) {
                        if (d.body[k].text == "defined") {
                            size_t j = k + 1;
                            bool paren = false;
                            if (j < d.body.size() && d.body[j].text == "(") {
                                paren = true;
                                j++;
                            }
                            std::string name;
                            if (j < d.body.size()) name = d.body[j].text;
                            if (paren) {
                                while (j < d.body.size() && d.body[j].text != ")") j++;
                            }
                            CppToken lit = d.body[k];
                            lit.kind = CppTokKind::Number;
                            const bool defined = expander.macros.count(name) != 0;
                            lit.text = defined ? "1" : "0";
                            if (!defined) certain = false;
                            condition.push_back(lit);
                            k = j;
                            continue;
                        }
                        condition.push_back(d.body[k]);
                    }
                    std::vector<CppToken> expanded;
                    std::vector<std::string> expand_stack;
                    expander.ExpandAll(condition, expanded, expand_stack, 0);
                    CondEvaluator eval(expanded, &certain);
                    value = !frame.taken && eval.Run() != 0;
                    frame.certain = certain;
                } else if (d.kind != CppDirectiveKind::Else) {
                    const bool defined = expander.macros.count(d.name) != 0;
                    value = !frame.taken && (d.kind == CppDirectiveKind::ElifDef ? defined : !defined);
                    if (!defined) frame.certain = false;
                }
                frame.active = frame.parent_active && value;
                frame.taken = frame.taken || frame.active;
                frame.group_line = d.line;
                d.active = frame.active;
                d.certain = frame.certain;
                break;
            }
            case CppDirectiveKind::Endif: {
                if (stack.empty()) {
                    error(d.start, d.end, "pp-stray-conditional", "#endif without a matching #if");
                    break;
                }
                CondFrame &frame = stack.back();
                if (!frame.active) deactivate(frame.group_line + 1, d.line);
                if (frame.if_index < out->directives.size()) {
                    out->directives[frame.if_index].end_directive_line = d.line;
                }
                stack.pop_back();
                break;
            }
            case CppDirectiveKind::Define: {
                if (!d.active || d.name.empty()) break;
                CppMacro macro;
                macro.name = d.name;
                macro.function_like = d.function_like;
                macro.variadic = d.variadic;
                macro.params = d.params;
                macro.body = d.body;
                macro.line = d.line;
                macro.pos = d.start;
                if (!d.body.empty()) macro.pos = d.body.front().start;
                const auto existing = expander.macros.find(macro.name);
                if (existing != expander.macros.end() && !existing->second.predefined) {
                    std::string old_text;
                    std::string new_text;
                    for (const CppToken &tok : existing->second.body) old_text += tok.text + " ";
                    for (const CppToken &tok : macro.body) new_text += tok.text + " ";
                    if (old_text != new_text || existing->second.params != macro.params) {
                        error(d.start, d.end, "pp-macro-redefined",
                              "Macro '" + macro.name + "' is redefined with a different body (first defined on line " +
                                  std::to_string(existing->second.line + 1) + ")");
                    }
                }
                // The table is updated here as well as replayed in the
                // expansion pass below, because `#if` conditions between
                // here and there are evaluated against it.
                expander.macros[macro.name] = macro;
                events.emplace_back(kept.size(), out->directives.size());
                break;
            }
            case CppDirectiveKind::Undef:
                if (d.active && !d.name.empty()) {
                    expander.macros.erase(d.name);
                    events.emplace_back(kept.size(), out->directives.size());
                }
                break;
            case CppDirectiveKind::Include: {
                if (!d.active) break;
                CppInclude inc;
                inc.line = d.line;
                inc.col = d.start.col;
                inc.header = d.header;
                inc.angled = d.angled;
                inc.path_col = d.body.empty() ? d.start.col : d.body.front().start.col;
                inc.path_end = d.end.col;
                if (opts.resolve_includes && !d.angled && !d.header.empty()) {
                    std::vector<std::string> roots;
                    if (!opts.doc_dir.empty()) roots.push_back(opts.doc_dir);
                    for (const std::string &dir : opts.include_dirs) roots.push_back(dir);
                    // A quoted include is usually written relative to the
                    // project's include root rather than to the file
                    // (`#include "gfx/types.h"` from src/gfx/foo.cpp), and
                    // this server has no -I flags to tell it where that
                    // is. Walking a few directories up finds it, and a
                    // wrong guess can only resolve to a file that really
                    // is there.
                    std::filesystem::path up(opts.doc_dir);
                    for (int level = 0; level < 3 && up.has_parent_path(); level++) {
                        up = up.parent_path();
                        if (up.empty() || up == up.root_path()) break;
                        roots.push_back(up.string());
                    }
                    for (const std::string &root : roots) {
                        std::error_code ec;
                        const std::filesystem::path candidate = std::filesystem::path(root) / d.header;
                        if (std::filesystem::is_regular_file(candidate, ec)) {
                            inc.resolved = candidate.lexically_normal().string();
                            break;
                        }
                    }
                }
                out->includes.push_back(inc);
                break;
            }
            case CppDirectiveKind::Error:
                // Only when the branch that reached it was decided from
                // macros this file actually defines. `#error "requires
                // -fcoroutines"` sits behind a macro a header we never
                // read would have set, and reporting it would mean
                // shouting at every standard header a person opens.
                if (d.active && d.certain) {
                    std::string message;
                    for (const CppToken &tok : d.body) {
                        if (!message.empty() && tok.space_before) message += ' ';
                        message += tok.text;
                    }
                    if (message.size() > 2 && message.front() == '"' && message.back() == '"') {
                        message = message.substr(1, message.size() - 2);
                    }
                    error(d.start, d.end, "pp-error", message.empty() ? "#error" : message);
                }
                break;
            case CppDirectiveKind::Warning:
                if (d.active && d.certain) {
                    std::string message;
                    for (const CppToken &tok : d.body) {
                        if (!message.empty() && tok.space_before) message += ' ';
                        message += tok.text;
                    }
                    error(d.start, d.end, "pp-warning", message.empty() ? "#warning" : message);
                }
                break;
            case CppDirectiveKind::Unknown:
                if (d.active) {
                    error(d.start, d.end, "pp-unknown-directive",
                          "Unknown preprocessor directive '#" + d.name + "'");
                }
                break;
            default:
                break;
        }
        out->directives.push_back(d);
    }

    for (const CondFrame &frame : stack) {
        if (frame.if_index < out->directives.size()) {
            const CppDirective &open = out->directives[frame.if_index];
            error(open.start, open.end, "pp-unterminated-conditional",
                  "#" + open.name + " is never closed by an #endif");
        }
        if (!frame.active) deactivate(frame.group_line + 1, static_cast<int>(lines.size()));
        out->truncated = true;
    }

    // Second pass: expand macros over the tokens that survived, replaying
    // each `#define`/`#undef` at the point it took effect.
    expander.macros = PredefinedMacros(opts.defines);
    size_t event = 0;
    std::vector<std::string> expand_stack;
    size_t k = 0;
    while (k <= kept.size()) {
        while (event < events.size() && events[event].first == k) {
            const CppDirective &d = out->directives[events[event].second];
            if (d.kind == CppDirectiveKind::Undef) {
                expander.macros.erase(d.name);
            } else {
                CppMacro macro;
                macro.name = d.name;
                macro.function_like = d.function_like;
                macro.variadic = d.variadic;
                macro.params = d.params;
                macro.body = d.body;
                macro.line = d.line;
                macro.pos = d.start;
                expander.macros[macro.name] = macro;
            }
            event++;
        }
        if (k == kept.size()) break;
        const size_t consumed = expander.TryExpand(kept, k, out->tokens, expand_stack, 0);
        if (consumed == 0) {
            out->tokens.push_back(kept[k]);
            k++;
        } else {
            k += consumed;
        }
    }
    if (expander.truncated) out->truncated = true;
    for (const auto &entry : expander.macros) {
        if (!entry.second.predefined) out->macros[entry.first] = entry.second;
    }

    CppToken end_tok;
    end_tok.kind = CppTokKind::End;
    if (!raw.empty()) {
        end_tok.start = raw.back().start;
        end_tok.end = raw.back().end;
    }
    out->tokens.push_back(end_tok);
}
