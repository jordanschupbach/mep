// Lexing for mep's C formatter: clang's raw lexer for C (C11/C23 with GNU and
// Microsoft keywords, digraphs, `::`, digit separators), wrapped the way
// clang-format's FormatTokenLexer wraps it: whitespace before each token
// recorded (newlines, original column), comments trimmed of trailing blanks,
// `>>` and `<<` split into two tokens (and `<<` re-merged), `_T("...")`
// merged into one string, the identifiers clang-format treats as macros of a
// known kind typed, and `// clang-format off` regions finalized.

#include "c_format_internal.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_map>

namespace cfmt::detail {

namespace {

bool isIdentStart(unsigned char C) { return std::isalpha(C) || C == '_' || C == '$' || C >= 0x80; }
bool isIdentBody(unsigned char C) { return std::isalnum(C) || C == '_' || C == '$' || C >= 0x80; }
bool isHorizontalWhitespace(char C) { return C == ' ' || C == '\t' || C == '\f' || C == '\v'; }
bool isVerticalWhitespace(char C) { return C == '\n' || C == '\r'; }
bool isWhitespaceChar(char C) { return isHorizontalWhitespace(C) || isVerticalWhitespace(C); }

const std::unordered_map<std::string_view, tok> &keywordTable() {
    static const std::unordered_map<std::string_view, tok> Table = [] {
        std::unordered_map<std::string_view, tok> T;
        // Named keywords.
        const std::pair<const char *, tok> Named[] = {
            {"auto", tok::kw_auto},
            {"break", tok::kw_break},
            {"case", tok::kw_case},
            {"char", tok::kw_char},
            {"const", tok::kw_const},
            {"__const", tok::kw_const},
            {"__const__", tok::kw_const},
            {"continue", tok::kw_continue},
            {"default", tok::kw_default},
            {"do", tok::kw_do},
            {"double", tok::kw_double},
            {"else", tok::kw_else},
            {"enum", tok::kw_enum},
            {"extern", tok::kw_extern},
            {"float", tok::kw_float},
            {"for", tok::kw_for},
            {"goto", tok::kw_goto},
            {"if", tok::kw_if},
            {"int", tok::kw_int},
            {"long", tok::kw_long},
            {"register", tok::kw_register},
            {"return", tok::kw_return},
            {"short", tok::kw_short},
            {"signed", tok::kw_signed},
            {"__signed", tok::kw_signed},
            {"__signed__", tok::kw_signed},
            {"sizeof", tok::kw_sizeof},
            {"static", tok::kw_static},
            {"struct", tok::kw_struct},
            {"switch", tok::kw_switch},
            {"typedef", tok::kw_typedef},
            {"union", tok::kw_union},
            {"unsigned", tok::kw_unsigned},
            {"void", tok::kw_void},
            {"volatile", tok::kw_volatile},
            {"__volatile", tok::kw_volatile},
            {"__volatile__", tok::kw_volatile},
            {"while", tok::kw_while},
            {"_Atomic", tok::kw__Atomic},
            {"_Bool", tok::kw__Bool},
            {"_Generic", tok::kw__Generic},
            {"alignas", tok::kw_alignas},
            {"alignof", tok::kw_alignof},
            {"asm", tok::kw_asm},
            {"__asm", tok::kw_asm},
            {"__asm__", tok::kw_asm},
            {"_asm", tok::kw_asm},
            {"bool", tok::kw_bool},
            {"true", tok::kw_true},
            {"false", tok::kw_false},
            {"restrict", tok::kw_restrict},
            {"__restrict", tok::kw_restrict},
            {"__restrict__", tok::kw_restrict},
            {"inline", tok::kw_inline},
            {"__inline", tok::kw_inline},
            {"__inline__", tok::kw_inline},
            {"_inline", tok::kw_inline},
            {"constexpr", tok::kw_constexpr},
            {"static_assert", tok::kw_static_assert},
            {"typeof", tok::kw_typeof},
            {"__typeof", tok::kw_typeof},
            {"__typeof__", tok::kw_typeof},
            {"char8_t", tok::kw_char8_t},
            {"__wchar_t", tok::kw_wchar_t},
            {"__fp16", tok::kw_half},
            {"__bf16", tok::kw___bf16},
            {"_Float16", tok::kw__Float16},
            {"__float128", tok::kw___float128},
            {"__ibm128", tok::kw___ibm128},
            {"__int64", tok::kw___int64},
            {"_int64", tok::kw___int64},
            {"__int128", tok::kw___int128},
            {"__int8", tok::kw_char},
            {"_int8", tok::kw_char},
            {"__int16", tok::kw_short},
            {"_int16", tok::kw_short},
            {"__int32", tok::kw_int},
            {"_int32", tok::kw_int},
            {"__auto_type", tok::kw___auto_type},
            {"__attribute", tok::kw___attribute},
            {"__attribute__", tok::kw___attribute},
            {"__declspec", tok::kw___declspec},
            {"_declspec", tok::kw___declspec},
            {"__try", tok::kw___try},
            {"__finally", tok::kw___finally},
            {"__cdecl", tok::kw___cdecl},
            {"_cdecl", tok::kw___cdecl},
            {"__stdcall", tok::kw___stdcall},
            {"_stdcall", tok::kw___stdcall},
            {"__fastcall", tok::kw___fastcall},
            {"_fastcall", tok::kw___fastcall},
            {"__thiscall", tok::kw___thiscall},
            {"_thiscall", tok::kw___thiscall},
            {"__regcall", tok::kw___regcall},
            {"__vectorcall", tok::kw___vectorcall},
            {"_vectorcall", tok::kw___vectorcall},
            {"_Nonnull", tok::kw__Nonnull},
            {"_Nullable", tok::kw__Nullable},
            {"_Null_unspecified", tok::kw__Null_unspecified},
            {"__ptr32", tok::kw___ptr32},
            {"__ptr64", tok::kw___ptr64},
            {"__funcref", tok::kw___funcref},
        };
        for (const auto &[Name, Kind] : Named) T.emplace(Name, Kind);
        // Keywords never tested by name: lexed as kw_other.
        const char *Other[] = {
            "_ExtInt", "_BitInt", "_Alignas", "_Alignof", "_Complex", "__complex", "__complex__", "_Imaginary",
            "_Noreturn", "_Static_assert", "_Thread_local", "__func__", "__objc_yes", "__objc_no", "__ptrauth",
            "_Countof", "nullptr", "__nullptr", "thread_local", "typeof_unqual", "__typeof_unqual",
            "__typeof_unqual__", "_Decimal32", "_Decimal64", "_Decimal128", "__alignof", "__alignof__",
            "_alignof", "__builtin_alignof", "__builtin_choose_expr", "__builtin_offsetof", "__builtin_FILE",
            "__builtin_FILE_NAME", "__builtin_FUNCTION", "__builtin_FUNCSIG", "__builtin_LINE",
            "__builtin_COLUMN", "__builtin_types_compatible_p", "__builtin_va_arg", "__extension__", "__imag",
            "__imag__", "__real", "__real__", "__label__", "__thread", "__FUNCTION__", "__PRETTY_FUNCTION__",
            "__FUNCDNAME__", "__FUNCSIG__", "L__FUNCTION__", "L__FUNCSIG__", "__private_extern__",
            "__module_private__", "__pascal", "__forceinline", "__unaligned", "__super", "__sptr", "__uptr",
            "__w64", "__uuidof", "_uuidof", "__leave", "__if_exists", "__if_not_exists", "__single_inheritance",
            "__multiple_inheritance", "__virtual_inheritance", "__interface", "_Nullable_result",
            "__builtin_convertvector", "__builtin_bit_cast", "__builtin_available", "__bridge",
            "__bridge_transfer", "__bridge_retained", "__bridge_retain", "__covariant", "__contravariant",
            "__kindof", "__is_destructible", "__is_nothrow_destructible", "__is_bitwise_cloneable",
        };
        for (const char *Name : Other) T.emplace(Name, tok::kw_other);
        return T;
    }();
    return Table;
}

pp ppKeyword(std::string_view Text) {
    static const std::unordered_map<std::string_view, pp> Table = {
        {"if", pp::if_},         {"ifdef", pp::ifdef},     {"ifndef", pp::ifndef},
        {"elif", pp::elif},      {"elifdef", pp::elifdef}, {"elifndef", pp::elifndef},
        {"else", pp::else_},     {"endif", pp::endif},     {"defined", pp::defined},
        {"include", pp::include}, {"include_next", pp::include_next}, {"import", pp::import},
        {"define", pp::define},  {"undef", pp::undef},     {"line", pp::line},
        {"error", pp::error},    {"warning", pp::warning}, {"pragma", pp::pragma},
        {"ident", pp::other},    {"sccs", pp::other},      {"assert", pp::other},
        {"unassert", pp::other}, {"embed", pp::other},     {"__public_macro", pp::other},
        {"__private_macro", pp::other},
    };
    auto It = Table.find(Text);
    return It == Table.end() ? pp::none : It->second;
}

// The macros LLVM style knows by name (ForEachMacros, IfMacros, ...).
TT knownMacroType(std::string_view Text) {
    static const std::unordered_map<std::string_view, TT> Table = {
        {"foreach", TT::ForEachMacro},
        {"Q_FOREACH", TT::ForEachMacro},
        {"BOOST_FOREACH", TT::ForEachMacro},
        {"KJ_IF_MAYBE", TT::IfMacro},
        {"__capability", TT::AttributeMacro},
        {"Q_UNUSED", TT::StatementMacro},
        {"QT_REQUIRE_VERSION", TT::StatementMacro},
        {"BOOST_PP_STRINGIZE", TT::UntouchableMacroFunc},
        {"CF_SWIFT_NAME", TT::UntouchableMacroFunc},
        {"NS_SWIFT_NAME", TT::UntouchableMacroFunc},
        {"PP_STRINGIZE", TT::UntouchableMacroFunc},
        {"STRINGIZE", TT::UntouchableMacroFunc},
        {"Q_EMIT", TT::StatementAttributeLikeMacro},
    };
    auto It = Table.find(Text);
    return It == Table.end() ? TT::Unknown : It->second;
}

bool isClangFormatOnOff(std::string_view Comment, bool On) {
    if (Comment == (On ? "/* clang-format on */" : "/* clang-format off */")) return true;
    std::string_view Prefix = On ? "// clang-format on" : "// clang-format off";
    if (Comment.substr(0, Prefix.size()) != Prefix) return false;
    std::string_view Rest = Comment.substr(Prefix.size());
    return Rest.empty() || Rest[0] == ':' || Rest[0] == ' ' || Rest[0] == '\t';
}

// The raw lexer: one token at Pos, whitespace (with line splices) returned as
// its own token of kind unknown, as clang's lexer does in keep-whitespace mode.
class RawLexer {
public:
    explicit RawLexer(std::string_view Text) : Text(Text) {}

    struct Raw {
        tok Kind;
        size_t Start;
        size_t Length;
        bool IsWhitespace;
    };

    size_t Pos = 0;

    char at(size_t I) const { return I < Text.size() ? Text[I] : '\0'; }

    // A backslash, optional horizontal whitespace and a newline: the length of
    // that line splice at I, or 0.
    size_t spliceAt(size_t I) const {
        if (at(I) != '\\') return 0;
        size_t J = I + 1;
        while (isHorizontalWhitespace(at(J))) ++J;
        if (at(J) == '\r') return at(J + 1) == '\n' ? J + 2 - I : J + 1 - I;
        if (at(J) == '\n') return J + 1 - I;
        return 0;
    }

    // Character at I with line splices skipped: returns the char and sets Next.
    char peek(size_t I, size_t &Next) const {
        for (;;) {
            size_t S = spliceAt(I);
            if (!S) break;
            I += S;
        }
        Next = I + 1;
        return at(I);
    }

    Raw next() {
        size_t Start = Pos;
        if (Pos >= Text.size()) return {tok::eof, Pos, 0, false};
        // Whitespace run (including line splices and ??/ trigraph splices).
        {
            size_t I = Pos;
            for (;;) {
                if (I < Text.size() && isWhitespaceChar(Text[I])) {
                    ++I;
                } else if (size_t S = spliceAt(I)) {
                    I += S;
                } else if (at(I) == '?' && at(I + 1) == '?' && at(I + 2) == '/' &&
                           (at(I + 3) == '\n' || at(I + 3) == '\r')) {
                    I += 4;
                } else {
                    break;
                }
            }
            if (I > Pos) {
                Pos = I;
                return {tok::unknown, Start, I - Start, true};
            }
        }
        size_t N;
        char C = peek(Pos, N);
        auto finish = [&](tok K, size_t End) {
            Pos = End;
            return Raw{K, Start, End - Start, false};
        };
        auto punct = [&](size_t AfterFirst) -> Raw {
            // Longest-match punctuator starting with C; AfterFirst is the
            // position after C.
            size_t N2, N3;
            char C2 = peek(AfterFirst, N2);
            char C3 = peek(N2, N3);
            switch (C) {
            case '[': return finish(tok::l_square, AfterFirst);
            case ']': return finish(tok::r_square, AfterFirst);
            case '(': return finish(tok::l_paren, AfterFirst);
            case ')': return finish(tok::r_paren, AfterFirst);
            case '{': return finish(tok::l_brace, AfterFirst);
            case '}': return finish(tok::r_brace, AfterFirst);
            case ';': return finish(tok::semi, AfterFirst);
            case ',': return finish(tok::comma, AfterFirst);
            case '?': return finish(tok::question, AfterFirst);
            case '~': return finish(tok::tilde, AfterFirst);
            case '@': return finish(tok::at, AfterFirst);
            case '.':
                if (C2 == '.' && C3 == '.') return finish(tok::ellipsis, N3);
                return finish(tok::period, AfterFirst);
            case '&':
                if (C2 == '&') return finish(tok::ampamp, N2);
                if (C2 == '=') return finish(tok::ampequal, N2);
                return finish(tok::amp, AfterFirst);
            case '*':
                if (C2 == '=') return finish(tok::starequal, N2);
                return finish(tok::star, AfterFirst);
            case '+':
                if (C2 == '+') return finish(tok::plusplus, N2);
                if (C2 == '=') return finish(tok::plusequal, N2);
                return finish(tok::plus, AfterFirst);
            case '-':
                if (C2 == '-') return finish(tok::minusminus, N2);
                if (C2 == '=') return finish(tok::minusequal, N2);
                if (C2 == '>') return finish(tok::arrow, N2);
                return finish(tok::minus, AfterFirst);
            case '!':
                if (C2 == '=') return finish(tok::exclaimequal, N2);
                return finish(tok::exclaim, AfterFirst);
            case '/':
                if (C2 == '=') return finish(tok::slashequal, N2);
                return finish(tok::slash, AfterFirst);
            case '%':
                if (C2 == '=') return finish(tok::percentequal, N2);
                if (C2 == '>') return finish(tok::r_brace, N2);  // digraph
                if (C2 == ':') {
                    size_t N4;
                    if (C3 == '%' && peek(N3, N4) == ':') return finish(tok::hashhash, N4);
                    return finish(tok::hash, N2);
                }
                return finish(tok::percent, AfterFirst);
            case '<':
                if (C2 == '<') {
                    if (C3 == '=') return finish(tok::lesslessequal, N3);
                    return finish(tok::lessless, N2);
                }
                if (C2 == '=') return finish(tok::lessequal, N2);
                if (C2 == ':') return finish(tok::l_square, N2);  // digraph
                if (C2 == '%') return finish(tok::l_brace, N2);   // digraph
                return finish(tok::less, AfterFirst);
            case '>':
                if (C2 == '>') {
                    if (C3 == '=') return finish(tok::greatergreaterequal, N3);
                    return finish(tok::greatergreater, N2);
                }
                if (C2 == '=') return finish(tok::greaterequal, N2);
                return finish(tok::greater, AfterFirst);
            case '^':
                if (C2 == '=') return finish(tok::caretequal, N2);
                return finish(tok::caret, AfterFirst);
            case '|':
                if (C2 == '|') return finish(tok::pipepipe, N2);
                if (C2 == '=') return finish(tok::pipeequal, N2);
                return finish(tok::pipe, AfterFirst);
            case ':':
                if (C2 == ':') return finish(tok::coloncolon, N2);
                if (C2 == '>') return finish(tok::r_square, N2);  // digraph
                return finish(tok::colon, AfterFirst);
            case '=':
                if (C2 == '=') return finish(tok::equalequal, N2);
                return finish(tok::equal, AfterFirst);
            case '#':
                if (C2 == '#') return finish(tok::hashhash, N2);
                if (C2 == '@') return finish(tok::hashat, N2);
                return finish(tok::hash, AfterFirst);
            default:
                return finish(tok::unknown, AfterFirst);
            }
        };

        // Comments.
        if (C == '/') {
            size_t N2;
            char C2 = peek(N, N2);
            if (C2 == '/') {
                // To the end of the line; a line splice continues the comment.
                size_t I = N2;
                for (;;) {
                    if (I >= Text.size()) break;
                    if (size_t S = spliceAt(I)) {
                        I += S;
                        continue;
                    }
                    if (Text[I] == '\n' || Text[I] == '\r') break;
                    ++I;
                }
                return finish(tok::comment, I);
            }
            if (C2 == '*') {
                size_t I = N2;
                bool Closed = false;
                while (I < Text.size()) {
                    size_t J;
                    char D = peek(I, J);
                    if (D == '*') {
                        size_t K;
                        if (peek(J, K) == '/') {
                            I = K;
                            Closed = true;
                            break;
                        }
                    }
                    if (J > Text.size()) {
                        I = Text.size();
                        break;
                    }
                    I = J;
                }
                (void)Closed;
                return finish(tok::comment, std::min(I, Text.size()));
            }
        }

        // Character and string literals, with prefixes.
        auto lexQuoted = [&](size_t QuotePos, char Quote, tok Kind) -> Raw {
            size_t I = QuotePos + 1;
            for (;;) {
                if (I >= Text.size()) return finish(Quote == '"' ? tok::unknown : tok::unknown, I);
                if (size_t S = spliceAt(I)) {
                    I += S;
                    continue;
                }
                char D = Text[I];
                if (D == '\\') {
                    I += 2;
                    continue;
                }
                if (D == '\n' || D == '\r') return finish(tok::unknown, I);
                ++I;
                if (D == Quote) break;
            }
            if (Quote == '\'' && I == QuotePos + 2) return finish(tok::unknown, I);  // ''
            return finish(Kind, I);
        };
        if (C == '"') return lexQuoted(Pos, '"', tok::string_literal);
        if (C == '\'') return lexQuoted(Pos, '\'', tok::char_constant);
        if (C == 'L' || C == 'u' || C == 'U') {
            size_t P = N;
            bool U8 = false;
            if (C == 'u' && at(P) == '8') {
                U8 = true;
                ++P;
            }
            if (at(P) == '"') {
                tok K = C == 'L'  ? tok::wide_string_literal
                        : U8      ? tok::utf8_string_literal
                        : C == 'u' ? tok::utf16_string_literal
                                   : tok::utf32_string_literal;
                return lexQuoted(P, '"', K);
            }
            if (at(P) == '\'') {
                tok K = C == 'L'  ? tok::wide_char_constant
                        : U8      ? tok::utf8_char_constant
                        : C == 'u' ? tok::utf16_char_constant
                                   : tok::utf32_char_constant;
                return lexQuoted(P, '\'', K);
            }
        }

        // Numbers (pp-numbers).
        size_t N2;
        if (std::isdigit(static_cast<unsigned char>(C)) ||
            (C == '.' && std::isdigit(static_cast<unsigned char>(peek(N, N2))))) {
            size_t I = N;
            char Prev = C;
            for (;;) {
                size_t J;
                char D = peek(I, J);
                if (isIdentBody(static_cast<unsigned char>(D)) || D == '.') {
                    Prev = D;
                    I = J;
                    continue;
                }
                if ((D == '+' || D == '-') && (Prev == 'e' || Prev == 'E' || Prev == 'p' || Prev == 'P')) {
                    Prev = D;
                    I = J;
                    continue;
                }
                if (D == '\'') {
                    size_t K;
                    char E = peek(J, K);
                    if (isIdentBody(static_cast<unsigned char>(E))) {
                        Prev = E;
                        I = K;
                        continue;
                    }
                }
                break;
            }
            return finish(tok::numeric_constant, I);
        }

        // Identifiers and keywords.
        if (isIdentStart(static_cast<unsigned char>(C)) ||
            (C == '\\' && (at(N) == 'u' || at(N) == 'U'))) {
            size_t I = N;
            if (C == '\\') I = N + 1;
            for (;;) {
                size_t J;
                char D = peek(I, J);
                if (isIdentBody(static_cast<unsigned char>(D))) {
                    I = J;
                    continue;
                }
                if (D == '\\' && (at(J) == 'u' || at(J) == 'U')) {
                    I = J + 1;
                    continue;
                }
                break;
            }
            return finish(tok::identifier, I);
        }

        return punct(N);
    }

private:
    std::string_view Text;
};

size_t countLeadingWhitespace(std::string_view Text) {
    size_t Cur = 0;
    while (Cur < Text.size()) {
        if (isWhitespaceChar(Text[Cur])) {
            ++Cur;
        } else if (Text[Cur] == '\\') {
            size_t L = Cur + 1;
            while (L < Text.size() && isHorizontalWhitespace(Text[L])) ++L;
            if (L >= Text.size() || !isVerticalWhitespace(Text[L])) break;
            Cur = L + 1;
        } else if (Text[Cur] == '?' && Cur + 3 < Text.size() && Text[Cur + 1] == '?' && Text[Cur + 2] == '/' &&
                   (Text[Cur + 3] == '\n' || Text[Cur + 3] == '\r')) {
            Cur += 4;
        } else {
            break;
        }
    }
    return Cur;
}

class FormatTokenLexer {
public:
    FormatTokenLexer(Source &Src, const Style &S) : Src(Src), S(S), Raw(Src.Text) {}

    void lex() {
        do {
            Src.Tokens.push_back(getNextToken());
            tryMergePreviousTokens();
        } while (Src.Tokens.back()->isNot(tok::eof));
    }

private:
    FormatToken *getStashedToken() {
        FormatToken *Prev = FormatTok;
        FormatToken *T = Src.make();
        T->Kind = Prev->Kind;
        size_t Loc = static_cast<size_t>(Prev->TokenText.data() - Src.Text.data()) + 1;
        T->WhitespaceStart = Loc;
        T->WhitespaceEnd = Loc;
        T->TokenText = Src.Text.substr(Loc, 1);
        T->ColumnWidth = 1;
        T->OriginalColumn = Prev->OriginalColumn + 1;
        T->Finalized = Prev->Finalized;
        FormatTok = T;
        return T;
    }

    FormatToken *getNextToken() {
        if (Stashed) {
            Stashed = false;
            return getStashedToken();
        }
        FormatTok = Src.make();
        RawLexer::Raw R = readRawToken();
        size_t WhitespaceStart = R.Start - TrailingWhitespace;
        FormatTok->IsFirst = IsFirstToken;
        IsFirstToken = false;
        unsigned WhitespaceLength = TrailingWhitespace;
        while (R.Kind != tok::eof && R.IsWhitespace) {
            std::string_view Text = Src.Text.substr(R.Start, R.Length);
            size_t Leading = countLeadingWhitespace(Text);
            (void)Leading;
            bool InEscape = false;
            for (size_t i = 0, e = Text.size(); i != e; ++i) {
                switch (Text[i]) {
                case '\r':
                    if (i + 1 < e && Text[i + 1] == '\n') break;
                    [[fallthrough]];
                case '\n':
                    ++FormatTok->NewlinesBefore;
                    if (!InEscape)
                        FormatTok->HasUnescapedNewline = true;
                    else
                        InEscape = false;
                    FormatTok->LastNewlineOffset = static_cast<unsigned>(WhitespaceLength + i + 1);
                    Column = 0;
                    break;
                case '\f':
                case '\v':
                    Column = 0;
                    break;
                case ' ':
                    ++Column;
                    break;
                case '\t':
                    Column += S.TabWidth - (S.TabWidth ? Column % S.TabWidth : 0);
                    break;
                case '\\':
                case '?':
                case '/':
                    InEscape = true;
                    break;
                default:
                    break;
                }
            }
            WhitespaceLength += Text.size();
            R = readRawToken();
        }
        FormatTok->Kind = R.Kind;
        FormatTok->TokenText = Src.Text.substr(R.Start, R.Length);
        FormatTok->Finalized = FormattingDisabled;
        if (FormatTok->is(tok::unknown) && !FormatTok->TokenText.empty() && FormatTok->TokenText[0] == '"') {
            FormatTok->Kind = tok::string_literal;
            FormatTok->IsUnterminatedLiteral = true;
        }
        if (FormatTok->is(tok::comment) && isClangFormatOnOff(FormatTok->TokenText, /*On=*/true))
            FormattingDisabled = false;
        FormatTok->Finalized = FormattingDisabled;
        if (FormatTok->is(tok::comment) && isClangFormatOnOff(FormatTok->TokenText, /*On=*/false))
            FormattingDisabled = true;

        if (FormatTok->is(tok::unknown)) FormatTok->setType(TT::ImplicitStringLiteral);

        // A `//` comment ending in a backslash continues on the next line; cut
        // it at the backslash when the next line is another `//` comment.
        if (FormatTok->is(tok::comment) && FormatTok->TokenText.substr(0, 2) == "//") {
            std::string_view Text = FormatTok->TokenText;
            for (size_t Pos = Text.find('\\'); Pos++ != std::string_view::npos; Pos = Text.find('\\', Pos)) {
                if (Pos < Text.size() && Text[Pos] == '\n') {
                    std::string_view Rest = Text.substr(Pos + 1);
                    size_t F = Rest.find_first_not_of(" \t\n\v\f\r");
                    if (F != std::string_view::npos && Rest.substr(F, 2) == "//") {
                        FormatTok->TokenText = Text.substr(0, Pos);
                        Raw.Pos = R.Start + Pos;
                        break;
                    }
                }
            }
        }

        FormatTok->WhitespaceStart = WhitespaceStart;
        FormatTok->WhitespaceEnd = WhitespaceStart + WhitespaceLength;
        FormatTok->OriginalColumn = Column;

        TrailingWhitespace = 0;
        if (FormatTok->is(tok::comment)) {
            std::string_view Untrimmed = FormatTok->TokenText;
            size_t End = Untrimmed.find_last_not_of(" \t\v\f");
            FormatTok->TokenText = End == std::string_view::npos ? Untrimmed.substr(0, 0) : Untrimmed.substr(0, End + 1);
            TrailingWhitespace = static_cast<unsigned>(Untrimmed.size() - FormatTok->TokenText.size());
        } else if (FormatTok->is(tok::identifier)) {
            auto &KW = keywordTable();
            auto It = KW.find(FormatTok->TokenText);
            if (It != KW.end()) FormatTok->Kind = It->second;
            FormatTok->HasIdentifierInfo = true;
            FormatTok->PPKind = ppKeyword(FormatTok->TokenText);
        } else if (FormatTok->isOneOf(tok::greatergreater, tok::lessless)) {
            FormatTok->Kind = FormatTok->is(tok::greatergreater) ? tok::greater : tok::less;
            FormatTok->TokenText = FormatTok->TokenText.substr(0, 1);
            ++Column;
            Stashed = true;
        }

        std::string_view Text = FormatTok->TokenText;
        size_t FirstNewlinePos = Text.find('\n');
        if (FirstNewlinePos == std::string_view::npos) {
            FormatTok->ColumnWidth = columnWidthWithTabs(Text, Column, S.TabWidth);
            Column += FormatTok->ColumnWidth;
        } else {
            FormatTok->IsMultiline = true;
            FormatTok->ColumnWidth = columnWidthWithTabs(Text.substr(0, FirstNewlinePos), Column, S.TabWidth);
            FormatTok->LastLineColumnWidth =
                columnWidthWithTabs(Text.substr(Text.find_last_of('\n') + 1), 0, S.TabWidth);
            Column = FormatTok->LastLineColumnWidth;
        }

        // Identifiers LLVM style knows as macros of some kind.
        if (FormatTok->HasIdentifierInfo) {
            TT Known = knownMacroType(FormatTok->TokenText);
            const FormatToken *Last = Src.Tokens.empty() ? nullptr : Src.Tokens.back();
            bool AfterDefine = Last && Last->HasIdentifierInfo && Last->PPKind == pp::define;
            if (Known != TT::Unknown && !AfterDefine) {
                FormatTok->setType(Known);
                if (Known == TT::IfMacro) FormatTok->Kind = tok::kw_if;
            }
        }
        return FormatTok;
    }

    RawLexer::Raw readRawToken() { return Raw.next(); }

    void tryMergePreviousTokens() {
        if (tryMerge_TMacro()) return;
        if (tryMergeLessLess()) return;
        tryMergeForEach();
    }

    bool tryMerge_TMacro() {
        auto &Tokens = Src.Tokens;
        if (Tokens.size() < 4) return false;
        FormatToken *Last = Tokens.back();
        if (Last->isNot(tok::r_paren)) return false;
        FormatToken *String = Tokens[Tokens.size() - 2];
        if (String->isNot(tok::string_literal) || String->IsMultiline) return false;
        if (Tokens[Tokens.size() - 3]->isNot(tok::l_paren)) return false;
        FormatToken *Macro = Tokens[Tokens.size() - 4];
        if (Macro->TokenText != "_T") return false;
        const char *Start = Macro->TokenText.data();
        const char *End = Last->TokenText.data() + Last->TokenText.size();
        String->TokenText = std::string_view(Start, static_cast<size_t>(End - Start));
        String->IsFirst = Macro->IsFirst;
        String->LastNewlineOffset = Macro->LastNewlineOffset;
        String->WhitespaceStart = Macro->WhitespaceStart;
        String->WhitespaceEnd = Macro->WhitespaceEnd;
        String->OriginalColumn = Macro->OriginalColumn;
        String->ColumnWidth = columnWidthWithTabs(String->TokenText, String->OriginalColumn, S.TabWidth);
        String->NewlinesBefore = Macro->NewlinesBefore;
        String->HasUnescapedNewline = Macro->HasUnescapedNewline;
        Tokens.pop_back();
        Tokens.pop_back();
        Tokens.pop_back();
        Tokens.back() = String;
        return true;
    }

    bool tryMergeLessLess() {
        auto &Tokens = Src.Tokens;
        if (Tokens.size() < 3) return false;
        size_t F = Tokens.size() - 3;
        if (Tokens[F]->isNot(tok::less) || Tokens[F + 1]->isNot(tok::less)) return false;
        if (Tokens[F + 1]->hasWhitespaceBefore()) return false;
        FormatToken *X = Tokens.size() > 3 ? Tokens[F - 1] : nullptr;
        if (X && X->is(tok::less)) return false;
        FormatToken *Y = Tokens[F + 2];
        if (Y->is(tok::less)) return false;
        Tokens[F]->Kind = tok::lessless;
        Tokens[F]->TokenText = std::string_view(Tokens[F]->TokenText.data(), 2);
        Tokens[F]->ColumnWidth += 1;
        Tokens.erase(Tokens.end() - 2);
        return true;
    }

    bool tryMergeForEach() {
        auto &Tokens = Src.Tokens;
        if (Tokens.size() < 2) return false;
        FormatToken *For = Tokens[Tokens.size() - 2];
        FormatToken *Each = Tokens[Tokens.size() - 1];
        if (For->isNot(tok::kw_for) || Each->isNot(tok::identifier) || Each->TokenText != "each") return false;
        For->setType(TT::ForEachMacro);
        For->Kind = tok::kw_for;
        For->TokenText = std::string_view(
            For->TokenText.data(), static_cast<size_t>(Each->TokenText.data() + Each->TokenText.size() -
                                                       For->TokenText.data()));
        For->ColumnWidth += Each->ColumnWidth;
        Tokens.pop_back();
        return true;
    }

    Source &Src;
    const Style &S;
    RawLexer Raw;
    FormatToken *FormatTok = nullptr;
    bool IsFirstToken = true;
    bool Stashed = false;
    unsigned Column = 0;
    unsigned TrailingWhitespace = 0;
    bool FormattingDisabled = false;
};

// --- UTF-8 widths -------------------------------------------------------------------

bool decodeUTF8(std::string_view S, size_t &I, uint32_t &CP) {
    unsigned char C = static_cast<unsigned char>(S[I]);
    unsigned Len = C < 0x80 ? 1 : (C >> 5) == 6 ? 2 : (C >> 4) == 14 ? 3 : (C >> 3) == 30 ? 4 : 0;
    if (!Len || I + Len > S.size()) return false;
    CP = Len == 1 ? C : Len == 2 ? (C & 0x1F) : Len == 3 ? (C & 0x0F) : (C & 0x07);
    for (unsigned k = 1; k < Len; ++k) {
        unsigned char D = static_cast<unsigned char>(S[I + k]);
        if ((D & 0xC0) != 0x80) return false;
        CP = (CP << 6) | (D & 0x3F);
    }
    I += Len;
    return true;
}

int codePointWidth(uint32_t CP) {
    if (CP < 0x20 || (CP >= 0x7F && CP < 0xA0)) return -1;
    // Combining marks and zero-width characters.
    if ((CP >= 0x0300 && CP <= 0x036F) || (CP >= 0x0483 && CP <= 0x0489) || (CP >= 0x0591 && CP <= 0x05BD) ||
        (CP >= 0x0610 && CP <= 0x061A) || (CP >= 0x064B && CP <= 0x065F) || (CP >= 0x200B && CP <= 0x200F) ||
        (CP >= 0x20D0 && CP <= 0x20FF) || (CP >= 0xFE00 && CP <= 0xFE0F) || (CP >= 0xFE20 && CP <= 0xFE2F) ||
        CP == 0x00AD)
        return 0;
    if ((CP >= 0x1100 && CP <= 0x115F) || (CP >= 0x2E80 && CP <= 0x303E) || (CP >= 0x3041 && CP <= 0x33FF) ||
        (CP >= 0x3400 && CP <= 0x4DBF) || (CP >= 0x4E00 && CP <= 0x9FFF) || (CP >= 0xA000 && CP <= 0xA4CF) ||
        (CP >= 0xAC00 && CP <= 0xD7A3) || (CP >= 0xF900 && CP <= 0xFAFF) || (CP >= 0xFE30 && CP <= 0xFE4F) ||
        (CP >= 0xFF00 && CP <= 0xFF60) || (CP >= 0xFFE0 && CP <= 0xFFE6) || (CP >= 0x1F300 && CP <= 0x1F64F) ||
        (CP >= 0x1F900 && CP <= 0x1F9FF) || (CP >= 0x20000 && CP <= 0x3FFFD))
        return 2;
    return 1;
}

}  // namespace

unsigned columnWidth(std::string_view Text) {
    size_t I = 0;
    int Width = 0;
    while (I < Text.size()) {
        uint32_t CP;
        if (!decodeUTF8(Text, I, CP)) return static_cast<unsigned>(Text.size());
        int W = codePointWidth(CP);
        if (W < 0) return static_cast<unsigned>(Text.size());
        Width += W;
    }
    return static_cast<unsigned>(Width);
}

unsigned columnWidthWithTabs(std::string_view Text, unsigned StartColumn, unsigned TabWidth) {
    unsigned TotalWidth = 0;
    std::string_view Tail = Text;
    for (;;) {
        size_t TabPos = Tail.find('\t');
        if (TabPos == std::string_view::npos) return TotalWidth + columnWidth(Tail);
        TotalWidth += columnWidth(Tail.substr(0, TabPos));
        if (TabWidth) TotalWidth += TabWidth - (StartColumn + TotalWidth) % TabWidth;
        Tail = Tail.substr(TabPos + 1);
    }
}

unsigned getCodePointNumBytes(char FirstChar) {
    unsigned char C = static_cast<unsigned char>(FirstChar);
    if (C < 0x80) return 1;
    if ((C >> 5) == 6) return 2;
    if ((C >> 4) == 14) return 3;
    if ((C >> 3) == 30) return 4;
    return 1;
}

unsigned getEscapeSequenceLength(std::string_view Text) {
    // encoding::getEscapeSequenceLength.
    if (Text.size() < 2) return static_cast<unsigned>(Text.size());
    switch (Text[1]) {
    case 'u':
        return 6;
    case 'U':
        return 10;
    case 'x': {
        unsigned I = 2;  // Point after '\x'.
        while (I < Text.size() && std::isxdigit(static_cast<unsigned char>(Text[I]))) ++I;
        return I;
    }
    default:
        if (Text[1] >= '0' && Text[1] <= '7') {
            unsigned I = 1;
            while (I < Text.size() && I < 4 && Text[I] >= '0' && Text[I] <= '7') ++I;
            return I;
        }
        return 1 + getCodePointNumBytes(Text[1]);
    }
}

void Lex(Source &Src, const Style &S) {
    FormatTokenLexer L(Src, S);
    L.lex();
}

}  // namespace cfmt::detail
