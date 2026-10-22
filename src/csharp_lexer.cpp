// mep's C# lexer, part 1: a translation of Roslyn's Lexer.cs from the top through LexDirectiveTrivia (everything
// before LexXmlDocComment), plus AbstractLexer's MakeError overloads and LexerCache's keyword lookup. The XML
// doc-comment / cref lexing, unicode escapes and string literals are csharp_lexer_xml.cpp and
// csharp_lexer_string.cpp. See csharp_parser_internal.h for the conventions and csharp_syntax.h for the license.

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

#include "csharp_parser_internal.h"

namespace csharp {

namespace {

bool IsAsciiLetter(char16_t c) { return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z'); }
bool IsAsciiDigit(char16_t c) { return c >= u'0' && c <= u'9'; }
bool IsHighSurrogate(char16_t c) { return c >= 0xD800 && c <= 0xDBFF; }
bool IsLowSurrogate(char16_t c) { return c >= 0xDC00 && c <= 0xDFFF; }

/** @brief The builder's ASCII text (numeric literals only ever hold ASCII). */
std::string ToAscii(Text text) {
    std::string s;
    s.reserve(text.size());
    for (char16_t c : text) s.push_back(c < 128 ? static_cast<char>(c) : '?');
    return s;
}

std::u16string FromAscii(const std::string &s) { return std::u16string(s.begin(), s.end()); }

/** @brief UInt64.TryParse(text, NumberStyles.None / AllowHexSpecifier, Invariant) for digit-only text. */
bool TryParseUInt64(Text text, bool isHex, uint64_t &result) {
    result = 0;
    if (text.empty()) return false;
    uint64_t value = 0;
    for (char16_t c : text) {
        if (isHex) {
            if (!SyntaxFacts::IsHexDigit(c)) return false;
            if ((value >> 60) != 0) return false;  // overflow
            value = (value << 4) | static_cast<uint64_t>(SyntaxFacts::HexValue(c));
        } else {
            if (!IsAsciiDigit(c)) return false;
            uint64_t d = static_cast<uint64_t>(c - u'0');
            if (value > (std::numeric_limits<uint64_t>::max() - d) / 10) return false;  // overflow
            value = value * 10 + d;
        }
    }
    result = value;
    return true;
}

/**
 * @brief For a decimal floating-point string that std::from_chars reported out of range: whether its magnitude
 *        is huge (overflow) rather than tiny (underflow to zero).
 */
bool IsHugeMagnitude(const std::string &s) {
    int64_t pointExp = 0;  // decimal exponent of the first significant digit (roughly)
    bool seenNonZero = false, inFraction = false;
    size_t i = 0;
    for (; i < s.size() && s[i] != 'e' && s[i] != 'E'; i++) {
        char c = s[i];
        if (c == '.') {
            inFraction = true;
            continue;
        }
        if (!seenNonZero) {
            if (c != '0') {
                seenNonZero = true;
                if (!inFraction) pointExp++;
            } else if (inFraction) {
                pointExp--;
            }
        } else if (!inFraction) {
            pointExp++;
        }
    }
    int64_t exp = 0;
    bool neg = false;
    if (i < s.size()) {
        i++;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
        for (; i < s.size(); i++) {
            if (exp < 100000000000LL) exp = exp * 10 + (s[i] - '0');
        }
    }
    return pointExp + (neg ? -exp : exp) > 0;
}

/**
 * @brief .NET's double.ToString() / float.ToString() (shortest round-trip digits, "G" layout: scientific when
 *        the decimal point position is beyond max(digits, 15 or 7) or below -3, "E+XX" exponent).
 */
std::u16string FormatRealNet(double value, bool isSingle) {
    if (std::isinf(value)) return value > 0 ? u"Infinity" : u"-Infinity";
    if (std::isnan(value)) return u"NaN";
    if (value == 0) return std::signbit(value) ? u"-0" : u"0";
    char buf[64];
    std::to_chars_result r = isSingle ? std::to_chars(buf, buf + sizeof buf, static_cast<float>(value),
                                                      std::chars_format::scientific)
                                      : std::to_chars(buf, buf + sizeof buf, value, std::chars_format::scientific);
    std::string sci(buf, r.ptr);
    bool negative = false;
    size_t i = 0;
    if (sci[0] == '-') {
        negative = true;
        i = 1;
    }
    std::string digits;
    for (; i < sci.size() && sci[i] != 'e'; i++) {
        if (sci[i] != '.') digits.push_back(sci[i]);
    }
    int exp = std::atoi(sci.c_str() + i + 1);
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    int count = static_cast<int>(digits.size());
    int digPos = exp + 1;
    int nMaxDigits = std::max(count, isSingle ? 7 : 15);
    std::string out = negative ? "-" : "";
    if (digPos > nMaxDigits || digPos < -3) {
        out += digits[0];
        if (count > 1) {
            out += '.';
            out += digits.substr(1);
        }
        out += 'E';
        out += exp < 0 ? '-' : '+';
        std::string e = std::to_string(exp < 0 ? -exp : exp);
        if (e.size() < 2) e = "0" + e;
        out += e;
    } else if (digPos <= 0) {
        out += "0.";
        out.append(static_cast<size_t>(-digPos), '0');
        out += digits;
    } else if (digPos >= count) {
        out += digits;
        out.append(static_cast<size_t>(digPos - count), '0');
    } else {
        out += digits.substr(0, static_cast<size_t>(digPos));
        out += '.';
        out += digits.substr(static_cast<size_t>(digPos));
    }
    return FromAscii(out);
}

/**
 * @brief .NET's decimal.TryParse(text, AllowDecimalPoint | AllowExponent, Invariant) for the lexer's
 *        well-formed text (Number.TryParseNumber into a 31-byte decimal NumberBuffer, then
 *        Number.TryNumberToDecimal). On success sets the value's 96-bit integer, scale, and returns true.
 */
bool TryParseDecimalNet(const std::string &s, uint32_t &hiOut, uint64_t &loOut, int &scaleOut) {
    constexpr int maxDigCount = 30;  // DecimalNumberBufferLength (29 + 1 + 1) - 1
    unsigned char digits[maxDigCount + 1] = {};
    int digCount = 0, digEnd = 0, scale = 0;
    bool nonZero = false, inDecimal = false, hasNonZeroTail = false;
    size_t i = 0;
    for (; i < s.size(); i++) {
        char ch = s[i];
        if (ch == '.') {
            inDecimal = true;
            continue;
        }
        if (ch == 'e' || ch == 'E') break;
        if (ch != '0' || nonZero) {
            if (digCount < maxDigCount) {
                digits[digCount] = static_cast<unsigned char>(ch);
                digEnd = digCount + 1;  // NumberBufferKind.Decimal: trailing zeros are kept
            } else if (ch != '0') {
                hasNonZeroTail = true;
            }
            if (!inDecimal) scale++;
            digCount++;
            nonZero = true;
        } else if (inDecimal) {
            scale--;
        }
    }
    if (i < s.size()) {
        i++;
        bool negExp = false;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) negExp = s[i++] == '-';
        if (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            int exp = 0;
            for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; i++) {
                if (exp >= 100000000) {
                    exp = std::numeric_limits<int>::max();
                    scale = 0;
                    break;
                }
                exp = exp * 10 + (s[i] - '0');
            }
            if (negExp) exp = -exp;
            scale += exp;
        }
    }
    digits[digEnd] = 0;

    // Number.TryNumberToDecimal.
    const unsigned char *p = digits;
    int e = scale;
    uint32_t c = *p;
    if (c == 0) {
        hiOut = 0;
        loOut = 0;
        scaleOut = std::clamp(-e, 0, 28);
        return true;
    }
    if (e > 29) return false;  // DecimalPrecision

    uint64_t low64 = 0;
    while (e > -28) {
        e--;
        low64 *= 10;
        low64 += c - '0';
        c = *++p;
        if (low64 >= std::numeric_limits<uint64_t>::max() / 10) break;
        if (c == 0) {
            while (e > 0) {
                e--;
                low64 *= 10;
                if (low64 >= std::numeric_limits<uint64_t>::max() / 10) break;
            }
            break;
        }
    }

    uint32_t high = 0;
    while ((e > 0 || (c != 0 && e > -28)) &&
           (high < std::numeric_limits<uint32_t>::max() / 10 ||
            (high == std::numeric_limits<uint32_t>::max() / 10 &&
             (low64 < 0x9999999999999999ULL || (low64 == 0x9999999999999999ULL && c <= '5'))))) {
        // multiply by 10
        uint64_t tmpLow = static_cast<uint64_t>(static_cast<uint32_t>(low64)) * 10ULL;
        uint64_t tmp64 = static_cast<uint64_t>(static_cast<uint32_t>(low64 >> 32)) * 10ULL + (tmpLow >> 32);
        low64 = static_cast<uint32_t>(tmpLow) + (tmp64 << 32);
        high = static_cast<uint32_t>(tmp64 >> 32) + high * 10;
        if (c != 0) {
            c -= '0';
            low64 += c;
            if (low64 < c) high++;
            c = *++p;
        }
        e--;
    }

    if (c >= '5') {
        bool round = true;
        if (c == '5' && (low64 & 1) == 0) {
            c = *++p;
            bool hasZeroTail = !hasNonZeroTail;
            while (c != 0 && hasZeroTail) {
                hasZeroTail &= (c == '0');
                c = *++p;
            }
            if (hasZeroTail) round = false;  // goto NoRounding
        }
        if (round) {
            if (++low64 == 0 && ++high == 0) {
                low64 = 0x999999999999999AULL;
                high = std::numeric_limits<uint32_t>::max() / 10;
                e++;
            }
        }
    }
    // NoRounding:
    if (e > 0) return false;
    if (e <= -29) {
        hiOut = 0;
        loOut = 0;
        scaleOut = 28;  // DecimalPrecision - 1
    } else {
        hiOut = high;
        loOut = low64;
        scaleOut = -e;
    }
    return true;
}

/** @brief decimal.ToString() of a 96-bit integer and scale. */
std::u16string FormatDecimalNet(uint32_t hi, uint64_t lo, int scale) {
    uint32_t limbs[3] = {hi, static_cast<uint32_t>(lo >> 32), static_cast<uint32_t>(lo)};
    std::string digits;
    do {
        uint64_t rem = 0;
        for (uint32_t &limb : limbs) {
            uint64_t cur = (rem << 32) | limb;
            limb = static_cast<uint32_t>(cur / 10);
            rem = cur % 10;
        }
        digits.push_back(static_cast<char>('0' + static_cast<int>(rem)));
    } while (limbs[0] != 0 || limbs[1] != 0 || limbs[2] != 0);
    while (static_cast<int>(digits.size()) <= scale) digits.push_back('0');
    std::string out(digits.rbegin(), digits.rend());
    if (scale > 0) out.insert(out.size() - static_cast<size_t>(scale), ".");
    return FromAscii(out);
}

double DecimalToDouble(uint32_t hi, uint64_t lo, int scale) {
    double v = static_cast<double>(hi) * 18446744073709551616.0 + static_cast<double>(lo);
    return v / std::pow(10.0, scale);
}

std::u16string Hex(uint32_t value, int width) {
    static const char16_t kDigits[] = u"0123456789abcdef";
    std::u16string s;
    for (int i = width - 1; i >= 0; i--) s.push_back(kDigits[(value >> (i * 4)) & 0xF]);
    return s;
}

// UnicodeCategory values (System.Globalization numbering).
constexpr int kLineSeparator = 12, kParagraphSeparator = 13, kControl = 14, kSurrogate = 16,
              kOtherNotAssigned = 29;

bool NeedsEscaping(int category) {
    return category == kControl || category == kOtherNotAssigned || category == kParagraphSeparator ||
           category == kLineSeparator || category == kSurrogate;
}

/**
 * @brief Whether a supplementary code point is unassigned (OtherNotAssigned) -- the one NeedsEscaping category a
 *        surrogate pair can have. Approximate: the BMP-only category table has no supplementary data, so this
 *        treats the planes Unicode leaves empty (and the plane-14 gaps) as unassigned.
 */
bool SupplementaryIsUnassigned(uint32_t cp) {
    if ((cp & 0xFFFE) == 0xFFFE) return true;  // noncharacters
    if (cp >= 0x323B0 && cp < 0xE0000) return true;
    if (cp >= 0xE0000 && cp < 0xF0000) {
        return !(cp == 0xE0001 || (cp >= 0xE0020 && cp <= 0xE007F) || (cp >= 0xE0100 && cp <= 0xE01EF));
    }
    return false;
}

/** @brief ObjectDisplay.FormatLiteral(text, ObjectDisplayOptions.EscapeNonPrintableCharacters). */
std::u16string FormatLiteralEscapeNonPrintable(Text str) {
    std::u16string sb;
    for (size_t i = 0; i < str.size(); i++) {
        char16_t c = str[i];
        if (GetUnicodeCategory(c) == kSurrogate) {
            if (IsHighSurrogate(c) && i + 1 < str.size() && IsLowSurrogate(str[i + 1])) {
                uint32_t unicode = 0x10000u + ((static_cast<uint32_t>(c) - 0xD800u) << 10) +
                                   (static_cast<uint32_t>(str[i + 1]) - 0xDC00u);
                if (SupplementaryIsUnassigned(unicode)) {
                    sb += u"\\U" + Hex(unicode, 8);
                } else {
                    sb.push_back(c);
                    sb.push_back(str[i + 1]);
                }
                i++;
            } else {
                // an unpaired surrogate
                sb += u"\\u" + Hex(c, 4);
            }
            continue;
        }
        const char16_t *replaceWith = nullptr;
        switch (c) {
        case u'\\': replaceWith = u"\\\\"; break;
        case u'\0': replaceWith = u"\\0"; break;
        case u'\a': replaceWith = u"\\a"; break;
        case u'\b': replaceWith = u"\\b"; break;
        case u'\f': replaceWith = u"\\f"; break;
        case u'\n': replaceWith = u"\\n"; break;
        case u'\r': replaceWith = u"\\r"; break;
        case u'\t': replaceWith = u"\\t"; break;
        case u'\v': replaceWith = u"\\v"; break;
        default: break;
        }
        if (replaceWith != nullptr) {
            sb += replaceWith;
        } else if (NeedsEscaping(GetUnicodeCategory(c))) {
            sb += u"\\u" + Hex(c, 4);
        } else {
            sb.push_back(c);
        }
    }
    return sb;
}

LiteralValue *NewValue(LiteralValue::Type type) {
    LiteralValue *v = CurrentArena()->NewArray<LiteralValue>(1);
    v->type = type;
    return v;
}

Text UnsignedText(uint64_t v) { return CurrentArena()->CopyText(FromAscii(std::to_string(v))); }
Text SignedText(int64_t v) { return CurrentArena()->CopyText(FromAscii(std::to_string(v))); }

}  // namespace

// --- LexerCache.cs ----------------------------------------------------------------------

bool Lexer::LexerCache::TryGetKeywordKind(Text key, SyntaxKind &kind) {
    if (static_cast<int>(key.size()) > MaxKeywordLength) {
        kind = SyntaxKind::None;
        return false;
    }
    // The keyword-kind map's value factory.
    kind = SyntaxFacts::GetKeywordKind(key);
    if (kind == SyntaxKind::None) kind = SyntaxFacts::GetContextualKeywordKind(key);
    return kind != SyntaxKind::None;
}

// --- AbstractLexer.cs ---------------------------------------------------------------------

SyntaxDiagnosticInfo Lexer::MakeError(int position, int width, ErrorCode code) const {
    int offset = GetLexemeOffsetFromPosition(position);
    SyntaxDiagnosticInfo d;
    d.offset = offset;
    d.width = width;
    d.code = code;
    return d;
}

SyntaxDiagnosticInfo Lexer::MakeError(int position, int width, ErrorCode code, std::initializer_list<Text> args) const {
    SyntaxDiagnosticInfo d = MakeError(position, width, code);
    for (Text a : args) {
        if (d.arg_count < 4) d.args[d.arg_count++] = a;
    }
    return d;
}

SyntaxDiagnosticInfo Lexer::MakeError(ErrorCode code) {
    SyntaxDiagnosticInfo d;
    d.code = code;
    return d;
}

SyntaxDiagnosticInfo Lexer::MakeError(ErrorCode code, std::initializer_list<Text> args) {
    SyntaxDiagnosticInfo d = MakeError(code);
    for (Text a : args) {
        if (d.arg_count < 4) d.args[d.arg_count++] = a;
    }
    return d;
}

// --- Lexer.cs -------------------------------------------------------------------------------

Lexer::Lexer(Text text, bool allowPreprocessorDirectives, bool interpolationFollowedByColon) {
    TextWindow.text = text;
    TextWindow.position = 0;
    _allowPreprocessorDirectives = allowPreprocessorDirectives;
    _interpolationFollowedByColon = interpolationFollowedByColon;
    // Obtain pooled items
    _leadingTriviaCache = SyntaxListBuilder::Create();
    _trailingTriviaCache = SyntaxListBuilder::Create();
}

Lexer::~Lexer() = default;

bool Lexer::InDocumentationComment() const {
    switch (ModeOf(_mode)) {
    case LexerMode::XmlDocComment:
    case LexerMode::XmlElementTag:
    case LexerMode::XmlAttributeTextQuote:
    case LexerMode::XmlAttributeTextDoubleQuote:
    case LexerMode::XmlCrefQuote:
    case LexerMode::XmlCrefDoubleQuote:
    case LexerMode::XmlNameQuote:
    case LexerMode::XmlNameDoubleQuote:
    case LexerMode::XmlCDataSectionText:
    case LexerMode::XmlCommentText:
    case LexerMode::XmlProcessingInstructionText:
    case LexerMode::XmlCharacter:
        return true;
    default:
        return false;
    }
}

GreenNode *Lexer::LexRef(LexerMode &mode) {
    GreenNode *result = Lex(mode);
    mode = _mode;
    return result;
}

GreenNode *Lexer::Lex(LexerMode mode) {
    _mode = mode;
    switch (_mode) {
    case LexerMode::Syntax:
    case LexerMode::DebuggerSyntax:
        // QuickScanSyntaxToken is an optimization that yields the same tokens: not ported.
        return LexSyntaxToken();
    case LexerMode::Directive:
        return LexDirectiveToken();
    default:
        break;
    }

    switch (ModeOf(_mode)) {
    case LexerMode::XmlDocComment:
        return LexXmlToken();
    case LexerMode::XmlElementTag:
        return LexXmlElementTagToken();
    case LexerMode::XmlAttributeTextQuote:
    case LexerMode::XmlAttributeTextDoubleQuote:
        return LexXmlAttributeTextToken();
    case LexerMode::XmlCDataSectionText:
        return LexXmlCDataSectionTextToken();
    case LexerMode::XmlCommentText:
        return LexXmlCommentTextToken();
    case LexerMode::XmlProcessingInstructionText:
        return LexXmlProcessingInstructionTextToken();
    case LexerMode::XmlCrefQuote:
    case LexerMode::XmlCrefDoubleQuote:
        return LexXmlCrefOrNameToken();
    case LexerMode::XmlNameQuote:
    case LexerMode::XmlNameDoubleQuote:
        // Same lexing as a cref attribute, just treat the identifiers a little differently.
        return LexXmlCrefOrNameToken();
    case LexerMode::XmlCharacter:
        return LexXmlCharacter();
    default:
        // ExceptionUtilities.UnexpectedValue: unreachable for the modes the parsers use.
        CS_ASSERT(false);
        return LexSyntaxToken();
    }
}

GreenNode *Lexer::LexSyntaxToken() {
    _leadingTriviaCache.Clear();
    LexSyntaxTrivia(/*isFollowingToken:*/ TextWindow.Position() > 0, /*isTrailing:*/ false, _leadingTriviaCache);
    SyntaxListBuilder leading = _leadingTriviaCache;

    TokenInfo tokenInfo{};

    Start();
    ScanSyntaxToken(tokenInfo);
    std::vector<SyntaxDiagnosticInfo> errors = GetErrors();

    _trailingTriviaCache.Clear();
    LexSyntaxTrivia(/*isFollowingToken:*/ true, /*isTrailing:*/ true, _trailingTriviaCache);
    SyntaxListBuilder trailing = _trailingTriviaCache;

    GreenNode *leadingNode = leading.ToListNode();
    GreenNode *trailingNode = trailing.ToListNode();
    return Create(tokenInfo, leadingNode, trailingNode, errors);
}

GreenNode *Lexer::LexSyntaxLeadingTrivia() {
    _leadingTriviaCache.Clear();
    LexSyntaxTrivia(/*isFollowingToken:*/ TextWindow.Position() > 0, /*isTrailing:*/ false, _leadingTriviaCache);
    return _leadingTriviaCache.ToListNode();
}

GreenNode *Lexer::LexSyntaxTrailingTrivia() {
    _trailingTriviaCache.Clear();
    LexSyntaxTrivia(/*isFollowingToken:*/ true, /*isTrailing:*/ true, _trailingTriviaCache);
    return _trailingTriviaCache.ToListNode();
}

// `leading` / `trailing` are the builders' list nodes (leading?.ToListNode()): callers pass a builder (it converts)
// or nullptr.
GreenNode *Lexer::Create(const TokenInfo &info, GreenNode *leadingNode, GreenNode *trailingNode,
                         const std::vector<SyntaxDiagnosticInfo> &errors) {
    CS_ASSERT(info.Kind != SyntaxKind::IdentifierToken || info.HasStringValue);

    GreenNode *token;
    if (info.RequiresTextForXmlEntity) {
        token = SyntaxFactory::Token(leadingNode, info.Kind, info.Text_, info.StringValue, trailingNode);
    } else {
        switch (info.Kind) {
        case SyntaxKind::IdentifierToken:
            token = SyntaxFactory::Identifier(info.ContextualKind, leadingNode, info.Text_, info.StringValue,
                                              trailingNode);
            break;
        case SyntaxKind::NumericLiteralToken: {
            // SyntaxFactory.Literal(leading, text, <value>, trailing): ValueText is the value's ToString().
            LiteralValue *value = nullptr;
            Text valueText;
            switch (info.ValueKind) {
            case SpecialType::System_Int32:
                value = NewValue(LiteralValue::Int32);
                value->bits = static_cast<uint64_t>(static_cast<int64_t>(info.IntValue));
                valueText = SignedText(info.IntValue);
                break;
            case SpecialType::System_UInt32:
                value = NewValue(LiteralValue::UInt32);
                value->bits = info.UintValue;
                valueText = UnsignedText(info.UintValue);
                break;
            case SpecialType::System_Int64:
                value = NewValue(LiteralValue::Int64);
                value->bits = static_cast<uint64_t>(info.LongValue);
                valueText = SignedText(info.LongValue);
                break;
            case SpecialType::System_UInt64:
                value = NewValue(LiteralValue::UInt64);
                value->bits = info.UlongValue;
                valueText = UnsignedText(info.UlongValue);
                break;
            case SpecialType::System_Single:
                value = NewValue(LiteralValue::Single);
                value->real = static_cast<double>(info.FloatValue);
                valueText = CurrentArena()->CopyText(FormatRealNet(static_cast<double>(info.FloatValue), true));
                break;
            case SpecialType::System_Double:
                value = NewValue(LiteralValue::Double);
                value->real = info.DoubleValue;
                valueText = CurrentArena()->CopyText(FormatRealNet(info.DoubleValue, false));
                break;
            case SpecialType::System_Decimal:
                value = NewValue(LiteralValue::Decimal);
                value->real = info.DecimalValue;
                valueText = _decimalValueText;
                break;
            default:
                // ExceptionUtilities.UnexpectedValue(info.ValueKind)
                CS_ASSERT(false);
                valueText = info.Text_;
                break;
            }
            token = SyntaxFactory::TokenWithValue(SyntaxKind::NumericLiteralToken, leadingNode, info.Text_, valueText,
                                                  value, trailingNode);
            break;
        }
        case SyntaxKind::InterpolatedStringToken:
            // we do not record a separate "value" for an interpolated string token, as it must be rescanned during parsing.
            token = SyntaxFactory::Literal(leadingNode, info.Text_, info.Kind, info.Text_, trailingNode);
            break;
        case SyntaxKind::StringLiteralToken:
        case SyntaxKind::Utf8StringLiteralToken:
        case SyntaxKind::SingleLineRawStringLiteralToken:
        case SyntaxKind::Utf8SingleLineRawStringLiteralToken:
        case SyntaxKind::MultiLineRawStringLiteralToken:
        case SyntaxKind::Utf8MultiLineRawStringLiteralToken:
            token = SyntaxFactory::Literal(leadingNode, info.Text_, info.Kind, info.StringValue, trailingNode);
            break;
        case SyntaxKind::CharacterLiteralToken: {
            LiteralValue *value = NewValue(LiteralValue::Char);
            value->bits = info.CharValue;
            Text valueText = CurrentArena()->CopyText(std::u16string(1, info.CharValue));
            token = SyntaxFactory::TokenWithValue(SyntaxKind::CharacterLiteralToken, leadingNode, info.Text_,
                                                  valueText, value, trailingNode);
            break;
        }
        case SyntaxKind::XmlTextLiteralNewLineToken:
            token = SyntaxFactory::XmlTextNewLine(leadingNode, info.Text_, info.StringValue, trailingNode);
            break;
        case SyntaxKind::XmlTextLiteralToken:
            token = SyntaxFactory::XmlTextLiteral(leadingNode, info.Text_, info.StringValue, trailingNode);
            break;
        case SyntaxKind::XmlEntityLiteralToken:
            token = SyntaxFactory::XmlEntity(leadingNode, info.Text_, info.StringValue, trailingNode);
            break;
        case SyntaxKind::EndOfDocumentationCommentToken:
        case SyntaxKind::EndOfFileToken:
            token = SyntaxFactory::Token(leadingNode, info.Kind, trailingNode);
            break;
        case SyntaxKind::RazorContentToken:
            token = SyntaxFactory::Token(leadingNode, info.Kind, info.Text_, trailingNode);
            break;
        case SyntaxKind::None:
            token = SyntaxFactory::BadToken(leadingNode, info.Text_, trailingNode);
            break;
        default:
            CS_ASSERT(SyntaxFacts::IsPunctuationOrKeyword(info.Kind));
            token = SyntaxFactory::Token(leadingNode, info.Kind, trailingNode);
            break;
        }
    }

    // DocumentationMode.Parse < Diagnose: errors are dropped inside documentation comments.
    if (!errors.empty() && !InDocumentationComment()) {
        token = WithDiagnosticsGreen(token, errors.data(), static_cast<int>(errors.size()));
    }

    return token;
}

void Lexer::ScanSyntaxToken(TokenInfo &info) {
    // Initialize for new token scan
    info.Kind = SyntaxKind::None;
    info.ContextualKind = SyntaxKind::None;
    info.Text_ = Text();
    info.HasText = false;

    char16_t character;
    bool isEscaped = false;
    int startingPosition = TextWindow.Position();

    // Start scanning the token
    character = TextWindow.PeekChar();
    switch (character) {
    case u'\"':
    case u'\'':
        ScanStringLiteral(info, /*inDirective:*/ false);
        break;

    case u'/':
        TextWindow.AdvanceChar();
        info.Kind = TextWindow.TryAdvance(u'=') ? SyntaxKind::SlashEqualsToken : SyntaxKind::SlashToken;
        break;

    case u'.':
        if (IsAsciiDigit(TextWindow.PeekChar(1))) {
            int atDotPosition = TextWindow.Position();
            if (atDotPosition >= 1 && atDotPosition == LexemeStartPosition) {
                // We have something like: .0
                //
                // This could be a fp number *except* the case where we have `..0` in that case, we want two
                // dots followed by an integer (which will be treated as a range expression).
                //
                // Move back one space to see what's before this dot and adjust accordingly.
                if (TextWindow.PreviousChar() == u'.') {
                    // We have two dots in a row.  Treat the second dot as a dot, not the start of a number literal.
                    TextWindow.AdvanceChar();
                    info.Kind = SyntaxKind::DotToken;
                    break;
                }

                // Fall through naturally and scan the number out as a floating point number.
            }
        }

        if (!ScanNumericLiteral(info)) {
            TextWindow.AdvanceChar();
            info.Kind = SyntaxKind::DotToken;
        }

        break;

    case u',':
        TextWindow.AdvanceChar();
        info.Kind = SyntaxKind::CommaToken;
        break;

    case u':':
        TextWindow.AdvanceChar();
        info.Kind = TextWindow.TryAdvance(u':') ? SyntaxKind::ColonColonToken : SyntaxKind::ColonToken;
        break;

    case u';':
        TextWindow.AdvanceChar();
        info.Kind = SyntaxKind::SemicolonToken;
        break;

    case u'~':
        TextWindow.AdvanceChar();
        info.Kind = SyntaxKind::TildeToken;
        break;

    case u'!':
        TextWindow.AdvanceChar();
        info.Kind = TextWindow.TryAdvance(u'=') ? SyntaxKind::ExclamationEqualsToken : SyntaxKind::ExclamationToken;
        break;

    case u'=':
        TextWindow.AdvanceChar();
        info.Kind = TextWindow.TryAdvance(u'=')   ? SyntaxKind::EqualsEqualsToken
                    : TextWindow.TryAdvance(u'>') ? SyntaxKind::EqualsGreaterThanToken
                                                  : SyntaxKind::EqualsToken;
        break;

    case u'*':
        TextWindow.AdvanceChar();
        info.Kind = TextWindow.TryAdvance(u'=') ? SyntaxKind::AsteriskEqualsToken : SyntaxKind::AsteriskToken;
        break;

    case u'(':
        TextWindow.AdvanceChar();
        info.Kind = SyntaxKind::OpenParenToken;
        break;

    case u')':
        TextWindow.AdvanceChar();
        info.Kind = SyntaxKind::CloseParenToken;
        break;

    case u'{':
        TextWindow.AdvanceChar();
        info.Kind = SyntaxKind::OpenBraceToken;
        break;

    case u'}':
        TextWindow.AdvanceChar();
        info.Kind = SyntaxKind::CloseBraceToken;
        break;

    case u'[':
        TextWindow.AdvanceChar();
        info.Kind = SyntaxKind::OpenBracketToken;
        break;

    case u']':
        TextWindow.AdvanceChar();
        info.Kind = SyntaxKind::CloseBracketToken;
        break;

    case u'?':
        TextWindow.AdvanceChar();
        info.Kind = TextWindow.TryAdvance(u'?') ? TextWindow.TryAdvance(u'=') ? SyntaxKind::QuestionQuestionEqualsToken
                                                                             : SyntaxKind::QuestionQuestionToken
                                                : SyntaxKind::QuestionToken;
        break;

    case u'+':
        TextWindow.AdvanceChar();
        info.Kind = TextWindow.TryAdvance(u'=')   ? SyntaxKind::PlusEqualsToken
                    : TextWindow.TryAdvance(u'+') ? SyntaxKind::PlusPlusToken
                                                  : SyntaxKind::PlusToken;
        break;

    case u'-':
        TextWindow.AdvanceChar();
        info.Kind = TextWindow.TryAdvance(u'=')   ? SyntaxKind::MinusEqualsToken
                    : TextWindow.TryAdvance(u'-') ? SyntaxKind::MinusMinusToken
                    : TextWindow.TryAdvance(u'>') ? SyntaxKind::MinusGreaterThanToken
                                                  : SyntaxKind::MinusToken;
        break;

    case u'%':
        TextWindow.AdvanceChar();
        info.Kind = TextWindow.TryAdvance(u'=') ? SyntaxKind::PercentEqualsToken : SyntaxKind::PercentToken;
        break;

    case u'&':
        TextWindow.AdvanceChar();
        info.Kind = TextWindow.TryAdvance(u'=')   ? SyntaxKind::AmpersandEqualsToken
                    : TextWindow.TryAdvance(u'&') ? SyntaxKind::AmpersandAmpersandToken
                                                  : SyntaxKind::AmpersandToken;
        break;

    case u'^':
        TextWindow.AdvanceChar();
        info.Kind = TextWindow.TryAdvance(u'=') ? SyntaxKind::CaretEqualsToken : SyntaxKind::CaretToken;
        break;

    case u'|':
        TextWindow.AdvanceChar();
        info.Kind = TextWindow.TryAdvance(u'=')   ? SyntaxKind::BarEqualsToken
                    : TextWindow.TryAdvance(u'|') ? SyntaxKind::BarBarToken
                                                  : SyntaxKind::BarToken;
        break;

    case u'<':
        TextWindow.AdvanceChar();
        info.Kind = TextWindow.TryAdvance(u'=') ? SyntaxKind::LessThanEqualsToken
                    : TextWindow.TryAdvance(u'<')
                        ? TextWindow.TryAdvance(u'=') ? SyntaxKind::LessThanLessThanEqualsToken
                                                      : SyntaxKind::LessThanLessThanToken
                        : SyntaxKind::LessThanToken;
        break;

    case u'>':
        TextWindow.AdvanceChar();
        info.Kind = TextWindow.TryAdvance(u'=') ? SyntaxKind::GreaterThanEqualsToken : SyntaxKind::GreaterThanToken;
        break;

    case u'@':
        if (!TryScanAtStringToken(info) && !ScanIdentifierOrKeyword(info)) {
            CS_ASSERT(TextWindow.PeekChar() == u'@');

            if (TextWindow.PeekChar(1) == u':') {
                // Razor HTML transition. For best consumption by razor, we want to simply pretend it's a token and
                // consume all the way to the end of the line.
                info.Kind = SyntaxKind::RazorContentToken;
                AddError(TextWindow.Position() + 1, /*width:*/ 1, ErrorCode::ERR_ExpectedVerbatimLiteral);

                ScanToEndOfLine();
                info.Text_ = GetNonInternedLexemeText();
                info.HasText = true;
                break;
            }

            ConsumeAtSignSequence();
            info.Text_ = GetInternedLexemeText();
            info.HasText = true;
            AddError(ErrorCode::ERR_ExpectedVerbatimLiteral);
        }
        break;

    case u'$':
        if (TryScanInterpolatedString(info)) {
            break;
        }

        if (ModeIs(LexerMode::DebuggerSyntax)) {
            goto case_identifier;
        }

        goto case_default;

    case u'\\': {
        // Could be unicode escape. Try that.
        isEscaped = true;
        char16_t surrogateIgnored;
        character = PeekCharOrUnicodeEscape(surrogateIgnored);
        if (SyntaxFacts::IsIdentifierStartCharacter(character)) {
            goto case_identifier;
        }

        goto case_default;
    }

    case SlidingTextWindow::InvalidCharacter:
        if (!TextWindow.IsReallyAtEnd()) {
            goto case_default;
        }

        if (_directives.HasUnfinishedIf()) {
            AddError(ErrorCode::ERR_EndifDirectiveExpected);
        }

        if (_directives.HasUnfinishedRegion()) {
            AddError(ErrorCode::ERR_EndRegionDirectiveExpected);
        }

        info.Kind = SyntaxKind::EndOfFileToken;
        break;

    default:
    case_default:
        // The C# switch's `case '_': case (>= 'a' and <= 'z') or (>= 'A' and <= 'Z'):` and `case >= '0' and <= '9':`
        // (only reached directly, never by a goto: those come with '$', '\\' (isEscaped) or U+FFFF).
        if (character == u'_' || IsAsciiLetter(character)) {
            goto case_identifier;
        }
        if (!isEscaped && IsAsciiDigit(character)) {
            ScanNumericLiteral(info);
            break;
        }

        if (SyntaxFacts::IsIdentifierStartCharacter(character)) {
            goto case_identifier;
        }

        if (isEscaped) {
            char16_t surrogateIgnored;
            std::optional<SyntaxDiagnosticInfo> error;
            NextCharOrUnicodeEscape(surrogateIgnored, error);
            AddError(error);
        } else {
            TextWindow.AdvanceChar();

            // If we ran into the start of a surrogate pair, then see if we have the whole pair.  If so,
            // skip the pair in its entirety so we can produce a good error message that treats the two as a
            // single entity to tell the user about.
            if (IsHighSurrogate(character) && IsLowSurrogate(TextWindow.PeekChar())) TextWindow.AdvanceChar();
        }

        // If we get too many characters that we cannot make sense of, treat the entire rest of the file as
        // a single invalid character, so we can bail out of parsing early without producing an unbounded
        // number of errors.
        if (_badTokenCount++ <= 200) {
            info.Text_ = GetInternedLexemeText();
        } else {
            int end = static_cast<int>(TextWindow.Text_().size());
            info.Text_ = TextWindow.GetText(startingPosition, end - startingPosition);
            TextWindow.Reset(end);
        }
        info.HasText = true;

        // if the original text wasn't already escaped, then escape it in the error message so that it's
        // clear what the issue is.  i.e. if the users source had the literal six characters in order like
        // so:  `\` `u` `D` `C` `E` `7`, then there's no need to escape that again when reporting the error.
        // However, if the user's code has the actual System.Char \uDCE7 char in it, then we want to print
        // that out in escaped form so they have an actual clue about what the character value is that we
        // have a problem with.
        {
            Text messageText = isEscaped ? info.Text_ : Intern(FormatLiteralEscapeNonPrintable(info.Text_));
            AddError(ErrorCode::ERR_UnexpectedCharacter, {messageText});
        }
        break;
    }
    return;

case_identifier:
    // All the 'common' identifier characters are represented directly in these switch cases for optimal
    // perf.  Calling IsIdentifierChar() functions is relatively expensive.
    ScanIdentifierOrKeyword(info);
}

bool Lexer::TryScanAtStringToken(TokenInfo &info) {
    CS_ASSERT(TextWindow.PeekChar() == u'@');

    int index = 0;
    while (TextWindow.PeekChar(index) == u'@') {
        index++;
    }

    if (TextWindow.PeekChar(index) == u'"') {
        // @"
        ScanVerbatimStringLiteral(info);
        return true;
    } else if (TextWindow.PeekChar(index) == u'$') {
        // @$"
        ScanInterpolatedStringLiteral(info);
        return true;
    }

    return false;
}

bool Lexer::TryScanInterpolatedString(TokenInfo &info) {
    CS_ASSERT(TextWindow.PeekChar() == u'$');

    char16_t next = TextWindow.PeekChar(1);
    if (next == u'$' || next == u'@' || next == u'"') {
        // $$ - definitely starts a raw interpolated string.
        // $@ - definitely starts an interpolated string.
        //
        // This will also match for $@@.  This is an error case when the user thinks they can mix verbatim and raw
        // interpolations together.  This will be properly handled in ScanInterpolatedStringLiteral
        ScanInterpolatedStringLiteral(info);
        return true;
    }

    return false;
}

// CheckFeatureAvailability(MessageID): every feature is available (LanguageVersion.Preview), so it never reports;
// its calls are dropped below.

bool Lexer::ScanInteger() {
    int start = TextWindow.Position();
    while (IsAsciiDigit(TextWindow.PeekChar())) {
        TextWindow.AdvanceChar();
    }

    return start < TextWindow.Position();
}

// Allows underscores in integers, except at beginning for decimal and end
void Lexer::ScanNumericLiteralSingleInteger(bool &underscoreInWrongPlace, bool &usedUnderscore,
                                            bool &firstCharWasUnderscore, bool isHex, bool isBinary) {
    if (TextWindow.PeekChar() == u'_') {
        if (isHex || isBinary) {
            firstCharWasUnderscore = true;
        } else {
            underscoreInWrongPlace = true;
        }
    }

    bool lastCharWasUnderscore = false;
    while (true) {
        char16_t ch = TextWindow.PeekChar();
        if (ch == u'_') {
            usedUnderscore = true;
            lastCharWasUnderscore = true;
        } else if (!(isHex      ? SyntaxFacts::IsHexDigit(ch)
                     : isBinary ? SyntaxFacts::IsBinaryDigit(ch)
                                : SyntaxFacts::IsDecDigit(ch))) {
            break;
        } else {
            _builder.push_back(ch);
            lastCharWasUnderscore = false;
        }
        TextWindow.AdvanceChar();
    }

    if (lastCharWasUnderscore) {
        underscoreInWrongPlace = true;
    }
}

bool Lexer::ScanNumericLiteral(TokenInfo &info) {
    int start = TextWindow.Position();
    char16_t ch;
    bool isHex = false;
    bool isBinary = false;
    bool hasDecimal = false;
    bool hasExponent = false;
    info.Text_ = Text();
    info.HasText = false;
    info.ValueKind = SpecialType::None;
    _builder.clear();
    bool hasUSuffix = false;
    bool hasLSuffix = false;
    bool underscoreInWrongPlace = false;
    bool usedUnderscore = false;
    bool firstCharWasUnderscore = false;

    ch = TextWindow.PeekChar();
    if (ch == u'0') {
        ch = TextWindow.PeekChar(1);
        if (ch == u'x' || ch == u'X') {
            TextWindow.AdvanceChar(2);
            isHex = true;
        } else if (ch == u'b' || ch == u'B') {
            // CheckFeatureAvailability(MessageID.IDS_FeatureBinaryLiteral): always available.
            TextWindow.AdvanceChar(2);
            isBinary = true;
        }
    }

    if (isHex || isBinary) {
        // It's OK if it has no digits after the '0x' -- we'll catch it in ScanNumericLiteral
        // and give a proper error then.
        ScanNumericLiteralSingleInteger(underscoreInWrongPlace, usedUnderscore, firstCharWasUnderscore, isHex,
                                        isBinary);

        if (TextWindow.PeekChar() == u'L' || TextWindow.PeekChar() == u'l') {
            TextWindow.AdvanceChar();
            hasLSuffix = true;
            if (TextWindow.PeekChar() == u'u' || TextWindow.PeekChar() == u'U') {
                TextWindow.AdvanceChar();
                hasUSuffix = true;
            }
        } else if (TextWindow.PeekChar() == u'u' || TextWindow.PeekChar() == u'U') {
            TextWindow.AdvanceChar();
            hasUSuffix = true;
            if (TextWindow.PeekChar() == u'L' || TextWindow.PeekChar() == u'l') {
                TextWindow.AdvanceChar();
                hasLSuffix = true;
            }
        }
    } else {
        ScanNumericLiteralSingleInteger(underscoreInWrongPlace, usedUnderscore, firstCharWasUnderscore,
                                        /*isHex:*/ false, /*isBinary:*/ false);

        if (ModeIs(LexerMode::DebuggerSyntax) && TextWindow.PeekChar() == u'#') {
            // Previously, in DebuggerSyntax mode, "123#" was a valid identifier.
            TextWindow.AdvanceChar();
            info.StringValue = info.Text_ = GetInternedLexemeText();
            info.HasStringValue = info.HasText = true;
            info.Kind = SyntaxKind::IdentifierToken;
            AddError(MakeError(ErrorCode::ERR_LegacyObjectIdSyntax));
            return true;
        }

        if ((ch = TextWindow.PeekChar()) == u'.') {
            char16_t ch2 = TextWindow.PeekChar(1);
            if (ch2 >= u'0' && ch2 <= u'9') {
                hasDecimal = true;
                _builder.push_back(ch);
                TextWindow.AdvanceChar();

                ScanNumericLiteralSingleInteger(underscoreInWrongPlace, usedUnderscore, firstCharWasUnderscore,
                                                /*isHex:*/ false, /*isBinary:*/ false);
            } else if (_builder.empty()) {
                // we only have the dot so far.. (no preceding number or following number)
                TextWindow.Reset(start);
                return false;
            }
        }

        if ((ch = TextWindow.PeekChar()) == u'E' || ch == u'e') {
            _builder.push_back(ch);
            TextWindow.AdvanceChar();
            hasExponent = true;
            if ((ch = TextWindow.PeekChar()) == u'-' || ch == u'+') {
                _builder.push_back(ch);
                TextWindow.AdvanceChar();
            }

            if (!(((ch = TextWindow.PeekChar()) >= u'0' && ch <= u'9') || ch == u'_')) {
                // use this for now (CS0595), cant use CS0594 as we dont know 'type'
                AddError(MakeError(ErrorCode::ERR_InvalidReal));
                // add dummy exponent, so parser does not blow up
                _builder.push_back(u'0');
            } else {
                ScanNumericLiteralSingleInteger(underscoreInWrongPlace, usedUnderscore, firstCharWasUnderscore,
                                                /*isHex:*/ false, /*isBinary:*/ false);
            }
        }

        ch = TextWindow.PeekChar();
        if (hasExponent || hasDecimal) {
            if (ch == u'f' || ch == u'F') {
                TextWindow.AdvanceChar();
                info.ValueKind = SpecialType::System_Single;
            } else if (ch == u'D' || ch == u'd') {
                TextWindow.AdvanceChar();
                info.ValueKind = SpecialType::System_Double;
            } else if (ch == u'm' || ch == u'M') {
                TextWindow.AdvanceChar();
                info.ValueKind = SpecialType::System_Decimal;
            } else {
                info.ValueKind = SpecialType::System_Double;
            }
        } else if (ch == u'f' || ch == u'F') {
            TextWindow.AdvanceChar();
            info.ValueKind = SpecialType::System_Single;
        } else if (ch == u'D' || ch == u'd') {
            TextWindow.AdvanceChar();
            info.ValueKind = SpecialType::System_Double;
        } else if (ch == u'm' || ch == u'M') {
            TextWindow.AdvanceChar();
            info.ValueKind = SpecialType::System_Decimal;
        } else if (ch == u'L' || ch == u'l') {
            TextWindow.AdvanceChar();
            hasLSuffix = true;
            if (TextWindow.PeekChar() == u'u' || TextWindow.PeekChar() == u'U') {
                TextWindow.AdvanceChar();
                hasUSuffix = true;
            }
        } else if (ch == u'u' || ch == u'U') {
            hasUSuffix = true;
            TextWindow.AdvanceChar();
            if (TextWindow.PeekChar() == u'L' || TextWindow.PeekChar() == u'l') {
                TextWindow.AdvanceChar();
                hasLSuffix = true;
            }
        }
    }

    if (underscoreInWrongPlace) {
        int width = TextWindow.Position() - start;
        AddError(MakeError(start, width, ErrorCode::ERR_InvalidNumber));
    }
    // else if (firstCharWasUnderscore) / else if (usedUnderscore): CheckFeatureAvailability -- always available.

    info.Kind = SyntaxKind::NumericLiteralToken;
    info.Text_ = GetInternedLexemeText();
    info.HasText = true;
    std::u16string valueText = _builder;  // TextWindow.Intern(_builder)
    uint64_t val;
    switch (info.ValueKind) {
    case SpecialType::System_Single:
        info.FloatValue = GetValueSingle(valueText);
        break;
    case SpecialType::System_Double:
        info.DoubleValue = GetValueDouble(valueText);
        break;
    case SpecialType::System_Decimal: {
        int end = TextWindow.Position();
        info.DecimalValue = GetValueDecimal(valueText, start, end);
        break;
    }
    default:
        if (valueText.empty()) {
            if (!underscoreInWrongPlace) {
                AddError(MakeError(ErrorCode::ERR_InvalidNumber));
            }
            val = 0;  // safe default
        } else {
            val = GetValueUInt64(valueText, isHex, isBinary);
        }

        // 2.4.4.2 Integer literals
        // ...
        // The type of an integer literal is determined as follows:

        // * If the literal has no suffix, it has the first of these types in which its value can be represented: int, uint, long, ulong.
        if (!hasUSuffix && !hasLSuffix) {
            if (val <= static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
                info.ValueKind = SpecialType::System_Int32;
                info.IntValue = static_cast<int32_t>(val);
            } else if (val <= std::numeric_limits<uint32_t>::max()) {
                info.ValueKind = SpecialType::System_UInt32;
                info.UintValue = static_cast<uint32_t>(val);
            } else if (val <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
                info.ValueKind = SpecialType::System_Int64;
                info.LongValue = static_cast<int64_t>(val);
            } else {
                info.ValueKind = SpecialType::System_UInt64;
                info.UlongValue = val;
            }
        } else if (hasUSuffix && !hasLSuffix) {
            // * If the literal is suffixed by U or u, it has the first of these types in which its value can be represented: uint, ulong.
            if (val <= std::numeric_limits<uint32_t>::max()) {
                info.ValueKind = SpecialType::System_UInt32;
                info.UintValue = static_cast<uint32_t>(val);
            } else {
                info.ValueKind = SpecialType::System_UInt64;
                info.UlongValue = val;
            }
        }

        // * If the literal is suffixed by L or l, it has the first of these types in which its value can be represented: long, ulong.
        else if (!hasUSuffix && hasLSuffix) {
            if (val <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
                info.ValueKind = SpecialType::System_Int64;
                info.LongValue = static_cast<int64_t>(val);
            } else {
                info.ValueKind = SpecialType::System_UInt64;
                info.UlongValue = val;
            }
        }

        // * If the literal is suffixed by UL, Ul, uL, ul, LU, Lu, lU, or lu, it is of type ulong.
        else {
            CS_ASSERT(hasUSuffix && hasLSuffix);
            info.ValueKind = SpecialType::System_UInt64;
            info.UlongValue = val;
        }

        break;
    }

    return true;
}

// TODO: Change to Int64.TryParse when it supports NumberStyles.AllowBinarySpecifier (inline this method into GetValueUInt32/64)
bool Lexer::TryParseBinaryUInt64(const std::u16string &text, uint64_t &value) {
    value = 0;
    for (char16_t c : text) {
        // if uppermost bit is set, then the next bitshift will overflow
        if ((value & 0x8000000000000000ULL) != 0) {
            return false;
        }
        // We shouldn't ever get a string that's nonbinary (see ScanNumericLiteral),
        // so don't explicitly check for it (there's a debug assert in SyntaxFacts)
        uint64_t bit = static_cast<uint64_t>(SyntaxFacts::BinaryValue(c));
        value = (value << 1) | bit;
    }
    return true;
}

// used in directives
int32_t Lexer::GetValueInt32(Text text, bool isHex) {
    // Int32.TryParse(text, isHex ? AllowHexSpecifier : None, Invariant): digits only here.
    int32_t result = 0;
    uint64_t wide = 0;
    bool ok = TryParseUInt64(text, isHex, wide);
    if (ok) {
        if (isHex) {
            ok = wide <= std::numeric_limits<uint32_t>::max();
            if (ok) result = static_cast<int32_t>(static_cast<uint32_t>(wide));
        } else {
            ok = wide <= static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
            if (ok) result = static_cast<int32_t>(wide);
        }
    }
    if (!ok) {
        // we've already lexed the literal, so the error must be from overflow
        result = 0;
        AddError(MakeError(ErrorCode::ERR_IntOverflow));
    }

    return result;
}

// used for all non-directive integer literals (cast to desired type afterward)
uint64_t Lexer::GetValueUInt64(Text text, bool isHex, bool isBinary) {
    uint64_t result;
    if (isBinary) {
        if (!TryParseBinaryUInt64(std::u16string(text), result)) {
            AddError(MakeError(ErrorCode::ERR_IntOverflow));
        }
    } else if (!TryParseUInt64(text, isHex, result)) {
        // we've already lexed the literal, so the error must be from overflow
        AddError(MakeError(ErrorCode::ERR_IntOverflow));
    }

    return result;
}

// RealParser.TryParseDouble / TryParseFloat: correctly rounded (IEEE round-to-nearest-even); fail only on
// overflow (the result is then infinity). std::from_chars is locale-independent and correctly rounded too.
double Lexer::GetValueDouble(const std::u16string &text) {
    std::string s = ToAscii(text);
    double result = 0;
    std::from_chars_result r = std::from_chars(s.data(), s.data() + s.size(), result, std::chars_format::general);
    bool overflow = false;
    if (r.ec == std::errc::result_out_of_range) {
        if (IsHugeMagnitude(s)) {
            overflow = true;
            result = std::numeric_limits<double>::infinity();
        } else {
            result = 0;
        }
    }
    if (overflow) {
        // we've already lexed the literal, so the error must be from overflow
        AddError(MakeError(ErrorCode::ERR_FloatOverflow, {u"double"}));
    }

    return result;
}

float Lexer::GetValueSingle(const std::u16string &text) {
    std::string s = ToAscii(text);
    float result = 0;
    std::from_chars_result r = std::from_chars(s.data(), s.data() + s.size(), result, std::chars_format::general);
    bool overflow = false;
    if (r.ec == std::errc::result_out_of_range) {
        if (IsHugeMagnitude(s)) {
            overflow = true;
            result = std::numeric_limits<float>::infinity();
        } else {
            result = 0;
        }
    }
    if (overflow) {
        // we've already lexed the literal, so the error must be from overflow
        AddError(MakeError(ErrorCode::ERR_FloatOverflow, {u"float"}));
    }

    return result;
}

double Lexer::GetValueDecimal(const std::u16string &text, int start, int end) {
    // Use decimal.TryParse to parse value (see Lexer.cs for how it differs from Dev11).
    uint32_t hi = 0;
    uint64_t lo = 0;
    int scale = 0;
    if (!TryParseDecimalNet(ToAscii(text), hi, lo, scale)) {
        // we've already lexed the literal, so the error must be from overflow
        AddError(MakeError(start, end - start, ErrorCode::ERR_FloatOverflow, {u"decimal"}));
        hi = 0;
        lo = 0;
        scale = 0;
    }

    _decimalValueText = Intern(FormatDecimalNet(hi, lo, scale));
    return DecimalToDouble(hi, lo, scale);
}

void Lexer::ResetIdentBuffer() {
    _identLen = 0;
    _identBuffer.clear();
}

void Lexer::AddIdentChar(char16_t ch) {
    if (_identLen >= static_cast<int>(_identBuffer.size())) {
        GrowIdentBuffer();
    }

    _identBuffer[static_cast<size_t>(_identLen++)] = ch;
}

void Lexer::GrowIdentBuffer() { _identBuffer.resize(_identBuffer.empty() ? 32 : _identBuffer.size() * 2); }

bool Lexer::ScanIdentifier(TokenInfo &info) {
    return ScanIdentifier_FastPath(info) ||
           (InXmlCrefOrNameAttributeValue() ? ScanIdentifier_CrefSlowPath(info) : ScanIdentifier_SlowPath(info));
}

// Implements a faster identifier lexer for the common case in the language where:
//
//   a) identifiers are not verbatim
//   b) identifiers don't contain unicode characters
//   c) identifiers don't contain unicode escapes
//
// (Roslyn's window span ends where its character window does; here it is the rest of the text. Either way,
// running off its end falls back to the slow path, which scans the same identifier.)
//
// Note: it is critical that this method must only be called from a
// code path that checked for IsIdentifierStartChar or '@' first.
bool Lexer::ScanIdentifier_FastPath(TokenInfo &info) {
    if ((_mode & LexerMode::MaskLexMode) == LexerMode::DebuggerSyntax) {
        // Debugger syntax is wonky.  Can't use the fast path for it.
        return false;
    }

    Text textWindowCharSpan = TextWindow.text.substr(static_cast<size_t>(TextWindow.Position()));
    size_t currentIndex = 0;

    while (true) {
        // If we do not not have any more contiguous characters within the char span that we can look at,
        // then fall back to slow path
        if (currentIndex == textWindowCharSpan.size()) return false;

        char16_t c = textWindowCharSpan[currentIndex];
        switch (c) {
        case u'&':
            // If we're in a cref, this could be the start of an
            // xml entity that belongs in the identifier.
            if (InXmlCrefOrNameAttributeValue()) {
                // Fall back on the slow path.
                return false;
            }

            // Otherwise, end the identifier.
            [[fallthrough]];
        case u'\0':
        case u' ':
        case u'\r':
        case u'\n':
        case u'\t':
        case u'!':
        case u'%':
        case u'(':
        case u')':
        case u'*':
        case u'+':
        case u',':
        case u'-':
        case u'.':
        case u'/':
        case u':':
        case u';':
        case u'<':
        case u'=':
        case u'>':
        case u'?':
        case u'[':
        case u']':
        case u'^':
        case u'{':
        case u'|':
        case u'}':
        case u'~':
        case u'"':
        case u'\'': {
            // All of the following characters are not valid in an
            // identifier.  If we see any of them, then we know we're
            // done.
            int length = static_cast<int>(currentIndex);
            TextWindow.AdvanceChar(length);
            info.Text_ = info.StringValue = textWindowCharSpan.substr(0, currentIndex);
            info.HasText = info.HasStringValue = true;
            info.IsVerbatim = false;
            return true;
        }
        default:
            if (IsAsciiDigit(c)) {
                if (currentIndex == 0) {
                    return false;
                }
                currentIndex++;
                continue;
            }
            if (IsAsciiLetter(c) || c == u'_') {
                // All of these characters are valid inside an identifier.
                // consume it and keep processing.
                currentIndex++;
                continue;
            }

            // case '@':  verbatim identifiers are handled in the slow path
            // case '\\': unicode escapes are handled in the slow path
            // Any other character is something we cannot handle.  i.e.
            // unicode chars or an escape.  Just break out and move to
            // the slow path.
            return false;
        }
    }
}

bool Lexer::ScanIdentifier_SlowPath(TokenInfo &info) {
    int start = TextWindow.Position();
    ResetIdentBuffer();

    while (TextWindow.PeekChar() == u'@') {
        TextWindow.AdvanceChar();
    }

    int atCount = TextWindow.Position() - start;
    info.IsVerbatim = atCount > 0;

    bool isObjectAddress = false;
    int width;
    while (true) {
        char16_t surrogateCharacter = SlidingTextWindow::InvalidCharacter;
        bool isEscaped = false;
        char16_t ch = TextWindow.PeekChar();
    top:
        switch (ch) {
        case u'\\':
            if (!isEscaped && IsUnicodeEscape()) {
                // ^^^^^^^ otherwise \u1234 looks just like ሴ! (i.e. escape within escape)
                info.HasIdentifierEscapeSequence = true;
                isEscaped = true;
                ch = PeekUnicodeEscape(surrogateCharacter);
                goto top;
            }

            goto case_default;
        case u'$':
            if (!ModeIs(LexerMode::DebuggerSyntax) || _identLen > 0) {
                goto LoopExit;
            }

            break;
        case SlidingTextWindow::InvalidCharacter:
            if (!TextWindow.IsReallyAtEnd()) {
                goto case_default;
            }

            goto LoopExit;
        case u'0': {
            if (_identLen == 0) {
                // Debugger syntax allows @0x[hexdigit]+ for object address identifiers.
                char16_t next = TextWindow.PeekChar(1);
                if (info.IsVerbatim && ModeIs(LexerMode::DebuggerSyntax) && (next == u'x' || next == u'X')) {
                    isObjectAddress = true;
                } else {
                    goto LoopExit;
                }
            }

            // Again, these are the 'common' identifier characters...
            break;
        }
        case u' ':
        case u'\t':
        case u'.':
        case u';':
        case u'(':
        case u')':
        case u',':
            // ...and these are the 'common' stop characters.
            goto LoopExit;
        case u'<':
            if (_identLen == 0 && ModeIs(LexerMode::DebuggerSyntax) && TextWindow.PeekChar(1) == u'>') {
                // In DebuggerSyntax mode, identifiers are allowed to begin with <>.
                TextWindow.AdvanceChar(2);
                AddIdentChar(u'<');
                AddIdentChar(u'>');
                continue;
            }

            goto LoopExit;
        default:
        case_default: {
            if (ch == u'_' || IsAsciiLetter(ch)) {
                // Again, these are the 'common' identifier characters...
                break;
            }
            if (ch >= u'1' && ch <= u'9') {
                if (_identLen == 0) {
                    goto LoopExit;
                }

                // Again, these are the 'common' identifier characters...
                break;
            }

            // This is the 'expensive' call
            if (_identLen == 0 && ch > 127 && SyntaxFacts::IsIdentifierStartCharacter(ch)) {
                break;
            } else if (_identLen > 0 && ch > 127 && SyntaxFacts::IsIdentifierPartCharacter(ch)) {
                //// BUG 424819 : Handle identifier chars > 0xFFFF via surrogate pairs
                if (SyntaxFacts::IsFormattingChar(ch)) {
                    if (isEscaped) {
                        std::optional<SyntaxDiagnosticInfo> error;
                        NextCharOrUnicodeEscape(surrogateCharacter, error);
                        AddError(error);
                    } else {
                        TextWindow.AdvanceChar();
                    }

                    continue;  // Ignore formatting characters
                }

                break;
            } else {
                // Not a valid identifier character, so bail.
                goto LoopExit;
            }
        }
        }

        if (isEscaped) {
            std::optional<SyntaxDiagnosticInfo> error;
            NextCharOrUnicodeEscape(surrogateCharacter, error);
            AddError(error);
        } else {
            TextWindow.AdvanceChar();
        }

        AddIdentChar(ch);
        if (surrogateCharacter != SlidingTextWindow::InvalidCharacter) {
            AddIdentChar(surrogateCharacter);
        }
    }

LoopExit:
    width = CurrentLexemeWidth();  // exact size of input characters
    if (_identLen > 0) {
        info.Text_ = GetInternedLexemeText();
        info.HasText = true;

        // id buffer is identical to width in input
        if (_identLen == width) {
            info.StringValue = info.Text_;
        } else {
            info.StringValue = Intern(_identBuffer.substr(0, static_cast<size_t>(_identLen)));
        }
        info.HasStringValue = true;

        if (isObjectAddress) {
            // @0x[hexdigit]+
            const int objectAddressOffset = 2;
            std::u16string valueText = _identBuffer.substr(static_cast<size_t>(objectAddressOffset),
                                                           static_cast<size_t>(_identLen - objectAddressOffset));
            // Verify valid hex value.
            bool allHex = true;
            for (char16_t c : valueText) allHex = allHex && SyntaxFacts::IsHexDigit(c);
            if (valueText.empty() || !allHex) {
                goto Fail;
            }
            // Parse hex value to check for overflow.
            GetValueUInt64(valueText, /*isHex:*/ true, /*isBinary:*/ false);
        }

        if (atCount >= 2) {
            AddError(start, atCount, ErrorCode::ERR_IllegalAtSequence);
        }

        return true;
    }

Fail:
    info.Text_ = Text();
    info.HasText = false;
    info.StringValue = Text();
    info.HasStringValue = false;
    TextWindow.Reset(start);
    return false;
}

/// This method is essentially the same as ScanIdentifier_SlowPath,
/// except that it can handle XML entities.  Since ScanIdentifier
/// is hot code and since this method does extra work, it seem
/// worthwhile to separate it from the common case.
bool Lexer::ScanIdentifier_CrefSlowPath(TokenInfo &info) {
    CS_ASSERT(InXmlCrefOrNameAttributeValue());

    int start = TextWindow.Position();
    ResetIdentBuffer();

    if (AdvanceIfMatches(u'@')) {
        // In xml name attribute values, the '@' is part of the value text of the identifier
        // (to match dev11).
        if (InXmlNameAttributeValue()) {
            AddIdentChar(u'@');
        } else {
            info.IsVerbatim = true;
        }
    }

    while (true) {
        int beforeConsumed = TextWindow.Position();
        char16_t consumedChar;
        char16_t consumedSurrogate;

        if (TextWindow.PeekChar() == u'&') {
            if (!TryScanXmlEntity(consumedChar, consumedSurrogate)) {
                // If it's not a valid entity, then it's not part of the identifier.
                TextWindow.Reset(beforeConsumed);
                goto LoopExit;
            }
        } else {
            consumedChar = TextWindow.NextChar();
            consumedSurrogate = SlidingTextWindow::InvalidCharacter;
        }

        // NOTE: If the surrogate is non-zero, then consumedChar won't match
        // any of the cases below (UTF-16 guarantees that members of surrogate
        // pairs aren't separately valid).

        {
            bool isEscaped = false;
        top:
            switch (consumedChar) {
            case u'\\':
                // NOTE: For completeness, we should allow xml entities in unicode escape
                // sequences (DevDiv #16321).  Since it is not currently a priority, we will
                // try to make the interim behavior sensible: we will only attempt to scan
                // a unicode escape if NONE of the characters are XML entities (including
                // the backslash, which we have already consumed).
                // When we're ready to implement this behavior, we can drop the position
                // check and use AdvanceIfMatches instead of PeekChar.
                if (!isEscaped && (TextWindow.Position() == beforeConsumed + 1) &&
                    (TextWindow.PeekChar() == u'u' || TextWindow.PeekChar() == u'U')) {
                    CS_ASSERT(consumedSurrogate == SlidingTextWindow::InvalidCharacter);

                    info.HasIdentifierEscapeSequence = true;

                    TextWindow.Reset(beforeConsumed);
                    // ^^^^^^^ otherwise \u1234 looks just like ሴ! (i.e. escape within escape)
                    isEscaped = true;
                    std::optional<SyntaxDiagnosticInfo> error;
                    consumedChar = NextUnicodeEscape(consumedSurrogate, error);
                    AddCrefError(error);
                    goto top;
                }

                goto case_default;

            case u' ':
            case u'$':
            case u'\t':
            case u'.':
            case u';':
            case u'(':
            case u')':
            case u',':
            case u'<':
                // ...and these are the 'common' stop characters.
                TextWindow.Reset(beforeConsumed);
                goto LoopExit;
            case SlidingTextWindow::InvalidCharacter:
                if (!TextWindow.IsReallyAtEnd()) {
                    goto case_default;
                }

                TextWindow.Reset(beforeConsumed);
                goto LoopExit;
            default:
            case_default: {
                if (consumedChar == u'_' || IsAsciiLetter(consumedChar)) {
                    // Again, these are the 'common' identifier characters...
                    break;
                }
                if (IsAsciiDigit(consumedChar)) {
                    if (_identLen == 0) {
                        TextWindow.Reset(beforeConsumed);
                        goto LoopExit;
                    }

                    // Again, these are the 'common' identifier characters...
                    break;
                }

                // This is the 'expensive' call
                if (_identLen == 0 && consumedChar > 127 && SyntaxFacts::IsIdentifierStartCharacter(consumedChar)) {
                    break;
                } else if (_identLen > 0 && consumedChar > 127 &&
                           SyntaxFacts::IsIdentifierPartCharacter(consumedChar)) {
                    //// BUG 424819 : Handle identifier chars > 0xFFFF via surrogate pairs
                    if (SyntaxFacts::IsFormattingChar(consumedChar)) {
                        continue;  // Ignore formatting characters
                    }

                    break;
                } else {
                    // Not a valid identifier character, so bail.
                    TextWindow.Reset(beforeConsumed);
                    goto LoopExit;
                }
            }
            }
        }

        AddIdentChar(consumedChar);
        if (consumedSurrogate != SlidingTextWindow::InvalidCharacter) {
            AddIdentChar(consumedSurrogate);
        }
    }

LoopExit:
    if (_identLen > 0) {
        // NOTE: If we don't intern the string value, then we won't get a hit
        // in the keyword dictionary!  (It searches for a key using identity.)
        // The text does not have to be interned (and probably shouldn't be
        // if it contains entities (else-case).

        int width = CurrentLexemeWidth();  // exact size of input characters

        // id buffer is identical to width in input
        if (_identLen == width) {
            info.StringValue = GetInternedLexemeText();
            info.Text_ = info.StringValue;
        } else {
            info.StringValue = Intern(_identBuffer.substr(0, static_cast<size_t>(_identLen)));
            info.Text_ = GetNonInternedLexemeText();
        }
        info.HasStringValue = info.HasText = true;

        return true;
    } else {
        info.Text_ = Text();
        info.HasText = false;
        info.StringValue = Text();
        info.HasStringValue = false;
        TextWindow.Reset(start);
        return false;
    }
}

bool Lexer::ScanIdentifierOrKeyword(TokenInfo &info) {
    info.ContextualKind = SyntaxKind::None;

    if (ScanIdentifier(info)) {
        CS_ASSERT(info.HasText);

        // check to see if it is an actual keyword
        if (!info.IsVerbatim && !info.HasIdentifierEscapeSequence) {
            if (ModeIs(LexerMode::Directive)) {
                SyntaxKind keywordKind = SyntaxFacts::GetPreprocessorKeywordKind(info.Text_);
                if (SyntaxFacts::IsPreprocessorContextualKeyword(keywordKind)) {
                    // Let the parser decide which instances are actually keywords.
                    info.Kind = SyntaxKind::IdentifierToken;
                    info.ContextualKind = keywordKind;
                } else {
                    info.Kind = keywordKind;
                }
            } else {
                if (!_cache.TryGetKeywordKind(info.Text_, info.Kind)) {
                    info.ContextualKind = info.Kind = SyntaxKind::IdentifierToken;
                } else if (SyntaxFacts::IsContextualKeyword(info.Kind)) {
                    info.ContextualKind = info.Kind;
                    info.Kind = SyntaxKind::IdentifierToken;
                }
            }

            if (info.Kind == SyntaxKind::None) {
                info.Kind = SyntaxKind::IdentifierToken;
            }
        } else {
            info.ContextualKind = info.Kind = SyntaxKind::IdentifierToken;
        }

        return true;
    } else {
        info.Kind = SyntaxKind::None;
        return false;
    }
}

void Lexer::LexSyntaxTrivia(bool isFollowingToken, bool isTrailing, SyntaxListBuilder &triviaList) {
    bool onlyWhitespaceOnLine = !isTrailing;

    auto lexSingleLineComment = [this](SyntaxListBuilder &list) {
        ScanToEndOfLine();
        Text text = GetNonInternedLexemeText();
        AddTrivia(SyntaxFactory::Comment(text), list);
    };

    auto lexMultiLineComment = [this](SyntaxListBuilder &list, char16_t delimiter) {
        bool isTerminated;
        ScanMultiLineComment(isTerminated, delimiter);
        if (!isTerminated) {
            // The comment didn't end.  Report an error at the start point.
            AddError(ErrorCode::ERR_OpenEndedComment);
        }

        Text text = GetNonInternedLexemeText();
        AddTrivia(SyntaxFactory::Comment(text), list);
    };

    while (true) {
        Start();
        char16_t ch = TextWindow.PeekChar();
        if (ch == u' ') {
            AddTrivia(ScanWhitespace(), triviaList);
            continue;
        } else if (ch > 127) {
            if (SyntaxFacts::IsWhitespace(ch)) {
                ch = u' ';
            } else if (SyntaxFacts::IsNewLine(ch)) {
                ch = u'\n';
            }
        }

        switch (ch) {
        case u' ':
        case u'\t':  // Horizontal tab
        case u'\v':  // Vertical Tab
        case u'\f':  // Form-feed
        case u'\x1A':
            AddTrivia(ScanWhitespace(), triviaList);
            break;
        case u'/':
            if ((ch = TextWindow.PeekChar(1)) == u'/') {
                if (!SuppressDocumentationCommentParse() && TextWindow.PeekChar(2) == u'/' &&
                    TextWindow.PeekChar(3) != u'/') {
                    // Doc comments should never be in trailing trivia.
                    // Stop processing so that it will be leading trivia on the next token.
                    if (isTrailing) {
                        return;
                    }

                    GreenNode *docComment = LexXmlDocComment(XmlDocCommentStyle::SingleLine);
                    AddTrivia(docComment, triviaList);
                    break;
                }

                // normal single line comment
                lexSingleLineComment(triviaList);
                onlyWhitespaceOnLine = false;
                break;
            } else if (ch == u'*') {
                if (!SuppressDocumentationCommentParse() && TextWindow.PeekChar(2) == u'*' &&
                    TextWindow.PeekChar(3) != u'*' && TextWindow.PeekChar(3) != u'/') {
                    // Doc comments should never be in trailing trivia.
                    // Stop processing so that it will be leading trivia on the next token.
                    if (isTrailing) {
                        return;
                    }

                    GreenNode *docComment = LexXmlDocComment(XmlDocCommentStyle::Delimited);
                    AddTrivia(docComment, triviaList);
                    break;
                }

                lexMultiLineComment(triviaList, /*delimiter:*/ u'/');
                onlyWhitespaceOnLine = false;
                break;
            }

            // not trivia
            return;
        case u'@':
            if ((ch = TextWindow.PeekChar(1)) == u'*') {
                // Razor comment. We pretend that it's a multi-line comment for error recovery, but it's an error case.
                AddError(TextWindow.Position(), /*width:*/ 1, ErrorCode::ERR_UnexpectedCharacter, {u"@"});
                lexMultiLineComment(triviaList, /*delimiter:*/ u'@');
                onlyWhitespaceOnLine = false;
                break;
            } else {
                return;
            }
        case u'\r':
        case u'\n': {
            GreenNode *endOfLine = ScanEndOfLine();
            CS_ASSERT(endOfLine != nullptr);
            AddTrivia(endOfLine, triviaList);
            if (isTrailing) {
                return;
            }

            onlyWhitespaceOnLine = true;
            break;
        }
        case u'#':
            if (_allowPreprocessorDirectives) {
                if (isTrailing || !onlyWhitespaceOnLine) {
                    // Directives cannot be in trailing trivia.
                    // We parse the directive (ignoring its effects like disabled text or defined symbols).
                    // We extract the text corresponding to the parsed directive
                    // and add it as a BadToken in SkippedTokensTrivia.

                    int savePosition = TextWindow.Position();

                    // All the `false` arguments affect only error reporting and the resulting node, both of which we discard.
                    // However, passing `false` can skip some unnecessary checks.
                    ParseDirective(/*isActive:*/ false, /*endIsActive:*/ false, /*isFollowingToken:*/ false);

                    Text text = TextWindow.GetText(savePosition, TextWindow.Position() - savePosition);

                    SyntaxDiagnosticInfo error;
                    error.offset = 0;
                    error.width = 1;
                    error.code = ErrorCode::ERR_BadDirectivePlacement;

                    GreenNode *token = WithDiagnosticsGreen(SyntaxFactory::BadToken(nullptr, text, nullptr), &error, 1);

                    AddTrivia(SyntaxFactory::SkippedTokensTrivia(token), triviaList);
                } else {
                    LexDirectiveAndExcludedTrivia(isFollowingToken, triviaList);
                }

                onlyWhitespaceOnLine = true;
                break;
            } else {
                return;
            }

        // Note: we specifically do not look for the >>>>>>> pattern as the start of
        // a conflict marker trivia.  That's because *technically* (albeit unlikely)
        // >>>>>>> could be the end of a very generic construct.  So, instead, we only
        // recognize >>>>>>> as we are scanning the trivia after a ======= marker
        // (which can never be part of legal code).
        // case '>':
        case u'|':
        case u'=':
        case u'<':
            if (!isTrailing) {
                if (IsConflictMarkerTrivia()) {
                    LexConflictMarkerTrivia(triviaList);
                    break;
                }
            }

            return;

        default:
            return;
        }
    }
}

// All conflict markers consist of the same character repeated seven times.  If it is
// a <<<<<<< or >>>>>>> marker then it is also followed by a space.
bool Lexer::IsConflictMarkerTrivia() {
    int position = TextWindow.Position();
    Text text = TextWindow.Text_();
    int length = static_cast<int>(text.size());
    if (position == 0 || SyntaxFacts::IsNewLine(text[static_cast<size_t>(position - 1)])) {
        char16_t firstCh = text[static_cast<size_t>(position)];
        CS_ASSERT(firstCh == u'<' || firstCh == u'|' || firstCh == u'=' || firstCh == u'>');

        if ((position + s_conflictMarkerLength) <= length) {
            for (int i = 0, n = s_conflictMarkerLength; i < n; i++) {
                if (text[static_cast<size_t>(position + i)] != firstCh) {
                    return false;
                }
            }

            if (firstCh == u'|' || firstCh == u'=') {
                return true;
            }

            return (position + s_conflictMarkerLength) < length &&
                   text[static_cast<size_t>(position + s_conflictMarkerLength)] == u' ';
        }
    }

    return false;
}

void Lexer::LexConflictMarkerTrivia(SyntaxListBuilder &triviaList) {
    Start();

    AddError(TextWindow.Position(), s_conflictMarkerLength, ErrorCode::ERR_Merge_conflict_marker_encountered);

    char16_t startCh = TextWindow.PeekChar();

    // First create a trivia from the start of this merge conflict marker to the
    // end of line/file (whichever comes first).
    LexConflictMarkerHeader(triviaList);

    // Now add the newlines as the next trivia.
    LexConflictMarkerEndOfLine(triviaList);

    // Now, if it was an ||||||| or ======= marker, then also created a DisabledText trivia for
    // the contents of the file after it, up until the next >>>>>>> marker we see.
    if (startCh == u'|' || startCh == u'=') {
        LexConflictMarkerDisabledText(startCh == u'=', triviaList);
    }
}

SyntaxListBuilder Lexer::LexConflictMarkerDisabledText(bool atSecondMiddleMarker, SyntaxListBuilder &triviaList) {
    // Consume everything from the end of the current mid-conflict marker to the start of the next
    // end-conflict marker
    Start();

    bool hitNextMarker = false;
    while (true) {
        char16_t ch = TextWindow.PeekChar();
        if (ch == SlidingTextWindow::InvalidCharacter) {
            break;
        }

        if (!atSecondMiddleMarker && ch == u'=' && IsConflictMarkerTrivia()) {
            hitNextMarker = true;
            break;
        }

        // If we hit the end-conflict marker, then lex it out at this point.
        if (ch == u'>' && IsConflictMarkerTrivia()) {
            hitNextMarker = true;
            break;
        }

        TextWindow.AdvanceChar();
    }

    if (CurrentLexemeWidth() > 0) {
        AddTrivia(SyntaxFactory::DisabledText(GetNonInternedLexemeText()), triviaList);
    }

    if (hitNextMarker) {
        LexConflictMarkerTrivia(triviaList);
    }

    return triviaList;
}

void Lexer::LexConflictMarkerEndOfLine(SyntaxListBuilder &triviaList) {
    Start();
    while (SyntaxFacts::IsNewLine(TextWindow.PeekChar())) {
        TextWindow.AdvanceChar();
    }

    if (CurrentLexemeWidth() > 0) {
        AddTrivia(SyntaxFactory::EndOfLine(GetNonInternedLexemeText()), triviaList);
    }
}

void Lexer::LexConflictMarkerHeader(SyntaxListBuilder &triviaList) {
    while (true) {
        char16_t ch = TextWindow.PeekChar();
        if (ch == SlidingTextWindow::InvalidCharacter || SyntaxFacts::IsNewLine(ch)) {
            break;
        }

        TextWindow.AdvanceChar();
    }

    AddTrivia(SyntaxFactory::ConflictMarker(GetNonInternedLexemeText()), triviaList);
}

void Lexer::AddTrivia(GreenNode *trivia, SyntaxListBuilder &list) {
    if (HasErrors()) {
        trivia = WithDiagnosticsGreen(trivia, _errors.data(), static_cast<int>(_errors.size()));
    }

    if (list.IsNull()) {
        list = SyntaxListBuilder::Create();
    }

    list.Add(trivia);
}

void Lexer::ScanMultiLineComment(bool &isTerminated, char16_t delimiter) {
    CS_ASSERT(delimiter == u'/' || delimiter == u'@');
    CS_ASSERT(TextWindow.PeekChar() == delimiter && TextWindow.PeekChar(1) == u'*');
    TextWindow.AdvanceChar(2);

    char16_t ch;
    while (true) {
        if ((ch = TextWindow.PeekChar()) == SlidingTextWindow::InvalidCharacter && TextWindow.IsReallyAtEnd()) {
            isTerminated = false;
            break;
        } else if (ch == u'*' && TextWindow.PeekChar(1) == delimiter) {
            TextWindow.AdvanceChar(2);
            isTerminated = true;
            break;
        } else {
            TextWindow.AdvanceChar();
        }
    }
}

void Lexer::ScanToEndOfLine() {
    char16_t ch;
    while (!SyntaxFacts::IsNewLine(ch = TextWindow.PeekChar()) &&
           (ch != SlidingTextWindow::InvalidCharacter || !TextWindow.IsReallyAtEnd())) {
        TextWindow.AdvanceChar();
    }
}

/// Scans a new-line sequence (either a single new-line character or a CR-LF combo).
GreenNode *Lexer::ScanEndOfLine() {
    char16_t ch;
    switch (ch = TextWindow.PeekChar()) {
    case u'\r':
        TextWindow.AdvanceChar();
        return TextWindow.TryAdvance(u'\n') ? SyntaxFactory::EndOfLine(u"\r\n")   // CarriageReturnLineFeed
                                             : SyntaxFactory::EndOfLine(u"\r");  // CarriageReturn
    case u'\n':
        TextWindow.AdvanceChar();
        return SyntaxFactory::EndOfLine(u"\n");  // LineFeed
    default:
        if (SyntaxFacts::IsNewLine(ch)) {
            int pos = TextWindow.Position();
            TextWindow.AdvanceChar();
            return SyntaxFactory::EndOfLine(TextWindow.GetText(pos, 1));  // ch.ToString()
        }

        return nullptr;
    }
}

/// Scans all of the whitespace (not new-lines) into a trivia node until it runs out.
GreenNode *Lexer::ScanWhitespace() {
    CS_ASSERT(SyntaxFacts::IsWhitespace(TextWindow.PeekChar()));

    bool onlySpaces = true;

    while (true) {
        char16_t ch = TextWindow.PeekChar();
        if (ch == u' ') {
            TextWindow.AdvanceChar();
            continue;
        }
        if (ch == u'\t' || ch == u'\v' || ch == u'\f' || ch == u'\x1A' ||
            (ch != u'\r' && ch != u'\n' && ch > 127 && SyntaxFacts::IsWhitespace(ch))) {
            onlySpaces = false;
            TextWindow.AdvanceChar();
            continue;
        }
        break;
    }

    CS_ASSERT(CurrentLexemeWidth() > 0);

    if (CurrentLexemeWidth() == 1 && onlySpaces) {
        return SyntaxFactory::Space;
    } else {
        // (LexerCache.LookupWhitespaceTrivia only shares identical trivia.)
        return SyntaxFactory::Whitespace(GetInternedLexemeText());
    }
}

void Lexer::LexDirectiveAndExcludedTrivia(bool isFollowingToken, SyntaxListBuilder &triviaList) {
    GreenNode *directive = LexSingleDirective(true, true, isFollowingToken, triviaList);

    // also lex excluded stuff
    bool isBranching = KindDerivesFrom(directive->kind, "BranchingDirectiveTriviaSyntax");
    if (isBranching && !BranchTaken(directive)) {
        LexExcludedDirectivesAndTrivia(true, triviaList);
    }
}

void Lexer::LexExcludedDirectivesAndTrivia(bool endIsActive, SyntaxListBuilder &triviaList) {
    while (true) {
        bool hasFollowingDirective;
        GreenNode *text = LexDisabledText(hasFollowingDirective);
        if (text != nullptr) {
            AddTrivia(text, triviaList);
        }

        if (!hasFollowingDirective) {
            break;
        }

        GreenNode *directive = LexSingleDirective(false, endIsActive, false, triviaList);
        bool isBranching = KindDerivesFrom(directive->kind, "BranchingDirectiveTriviaSyntax");
        if (directive->kind == SyntaxKind::EndIfDirectiveTrivia || (isBranching && BranchTaken(directive))) {
            break;
        } else if (directive->kind == SyntaxKind::IfDirectiveTrivia) {
            LexExcludedDirectivesAndTrivia(false, triviaList);
        }
    }
}

GreenNode *Lexer::LexSingleDirective(bool isActive, bool endIsActive, bool isFollowingToken,
                                     SyntaxListBuilder &triviaList) {
    if (SyntaxFacts::IsWhitespace(TextWindow.PeekChar())) {
        Start();
        AddTrivia(ScanWhitespace(), triviaList);
    }

    GreenNode *directive = ParseDirective(isActive, endIsActive, isFollowingToken);

    AddTrivia(directive, triviaList);
    _directives = ApplyDirectives(directive, _directives);
    return directive;
}

GreenNode *Lexer::ParseDirective(bool isActive, bool endIsActive, bool isFollowingToken) {
    LexerMode saveMode = _mode;

    if (!_directiveParser) _directiveParser = std::make_unique<DirectiveParser>(this);
    _directiveParser->ReInitialize(_directives);

    GreenNode *directive = _directiveParser->ParseDirective(isActive, endIsActive, isFollowingToken);

    _mode = saveMode;
    return directive;
}

// consume text up to the next directive
GreenNode *Lexer::LexDisabledText(bool &followedByDirective) {
    Start();

    int lastLineStart = TextWindow.Position();
    int lines = 0;  // (counted but unused, as in Lexer.cs)
    bool allWhitespace = true;

    while (true) {
        char16_t ch = TextWindow.PeekChar();
        if (ch == SlidingTextWindow::InvalidCharacter && TextWindow.IsReallyAtEnd()) {
            followedByDirective = false;
            return CurrentLexemeWidth() > 0 ? SyntaxFactory::DisabledText(GetNonInternedLexemeText()) : nullptr;
        }
        if (ch == u'#' && _allowPreprocessorDirectives) {
            followedByDirective = true;
            if (!(lastLineStart < TextWindow.Position() && !allWhitespace)) {
                TextWindow.Reset(lastLineStart);  // reset so directive parser can consume the starting whitespace on this line
                return CurrentLexemeWidth() > 0 ? SyntaxFactory::DisabledText(GetNonInternedLexemeText()) : nullptr;
            }
            // goto default ('#' is neither a newline nor whitespace)
        }
        if (ch == u'\r' || ch == u'\n' || SyntaxFacts::IsNewLine(ch)) {
            ScanEndOfLine();
            lastLineStart = TextWindow.Position();
            allWhitespace = true;
            lines++;
            (void)lines;
            continue;
        }
        // default:
        allWhitespace = allWhitespace && SyntaxFacts::IsWhitespace(ch);
        TextWindow.AdvanceChar();
    }
}

GreenNode *Lexer::LexDirectiveToken() {
    Start();
    TokenInfo info{};
    ScanDirectiveToken(info);
    std::vector<SyntaxDiagnosticInfo> errors = GetErrors();

    SyntaxListBuilder directiveTriviaCache = _directiveTriviaCache;
    if (!directiveTriviaCache.IsNull()) directiveTriviaCache.Clear();
    _directiveTriviaCache = SyntaxListBuilder();

    LexDirectiveTrailingTrivia(info.Kind == SyntaxKind::EndOfDirectiveToken, directiveTriviaCache);

    GreenNode *token = Create(info, nullptr, directiveTriviaCache.ToList(), errors);
    _directiveTriviaCache = directiveTriviaCache;

    return token;
}

std::optional<Text> Lexer::LexOptionalPreprocessingMessage() {
    int start = TextWindow.Position();
    bool any = false;

    // Skip the rest of the line until we hit a EOL or EOF.  This follows the PP_Message portion of the specification.
    while (true) {
        char16_t ch = TextWindow.PeekChar();
        if (SyntaxFacts::IsNewLine(ch)) {
            // don't consume EOL characters here
            break;
        } else if (ch == SlidingTextWindow::InvalidCharacter && TextWindow.IsReallyAtEnd()) {
            // don't consume EOF characters here
            break;
        }

        any = true;  // builder.Append(ch)
        TextWindow.AdvanceChar();
    }

    if (!any) return std::nullopt;
    return TextWindow.GetText(start, TextWindow.Position() - start);
}

GreenNode *Lexer::LexEndOfDirectiveAfterOptionalPreprocessingMessage(GreenNode *leading) {
    // now try to consume the EOL if there.
    SyntaxListBuilder directiveTriviaCache = _directiveTriviaCache;
    if (!directiveTriviaCache.IsNull()) directiveTriviaCache.Clear();
    _directiveTriviaCache = SyntaxListBuilder();

    LexDirectiveTrailingTrivia(/*includeEndOfLine:*/ true, directiveTriviaCache);
    GreenNode *trailing = directiveTriviaCache.ToList();
    _directiveTriviaCache = directiveTriviaCache;

    GreenNode *endOfDirective = SyntaxFactory::Token(leading, SyntaxKind::EndOfDirectiveToken, trailing);

    return endOfDirective;
}

GreenNode *Lexer::LexEndOfDirectiveWithOptionalPreprocessingMessage() {
    std::optional<Text> message = LexOptionalPreprocessingMessage();
    GreenNode *leading = message ? SyntaxFactory::PreprocessingMessage(*message) : nullptr;

    return LexEndOfDirectiveAfterOptionalPreprocessingMessage(leading);
}

GreenNode *Lexer::LexEndOfDirectiveWithOptionalContent(GreenNode *&content) {
    std::optional<Text> message = LexOptionalPreprocessingMessage();
    // SyntaxToken.StringLiteral(message): a StringLiteralToken whose text and value are the message.
    content = message ? SyntaxFactory::Literal(nullptr, *message, SyntaxKind::StringLiteralToken, *message, nullptr)
                      : nullptr;
    return LexEndOfDirectiveAfterOptionalPreprocessingMessage(nullptr);
}

bool Lexer::ScanDirectiveToken(TokenInfo &info) {
    char16_t character;
    char16_t surrogateCharacter;
    bool isEscaped = false;

    switch (character = TextWindow.PeekChar()) {
    case SlidingTextWindow::InvalidCharacter:
        if (!TextWindow.IsReallyAtEnd()) {
            goto case_default;
        }
        // don't consume end characters here
        info.Kind = SyntaxKind::EndOfDirectiveToken;
        break;

    case u'\r':
    case u'\n':
    case_newline:
        // don't consume end characters here
        info.Kind = SyntaxKind::EndOfDirectiveToken;
        break;

    case u'#':
        TextWindow.AdvanceChar();
        info.Kind = SyntaxKind::HashToken;
        break;

    case u'(':
        TextWindow.AdvanceChar();
        info.Kind = SyntaxKind::OpenParenToken;
        break;

    case u')':
        TextWindow.AdvanceChar();
        info.Kind = SyntaxKind::CloseParenToken;
        break;

    case u',':
        TextWindow.AdvanceChar();
        info.Kind = SyntaxKind::CommaToken;
        break;

    case u'-':
        TextWindow.AdvanceChar();
        info.Kind = SyntaxKind::MinusToken;
        break;

    case u':':
        TextWindow.AdvanceChar();
        info.Kind = SyntaxKind::ColonToken;
        break;

    case u'!':
        TextWindow.AdvanceChar();
        if (TextWindow.PeekChar() == u'=') {
            TextWindow.AdvanceChar();
            info.Kind = SyntaxKind::ExclamationEqualsToken;
        } else {
            info.Kind = SyntaxKind::ExclamationToken;
        }

        break;

    case u'=':
        TextWindow.AdvanceChar();
        if (TextWindow.PeekChar() == u'=') {
            TextWindow.AdvanceChar();
            info.Kind = SyntaxKind::EqualsEqualsToken;
        } else {
            info.Kind = SyntaxKind::EqualsToken;
        }

        break;

    case u'&':
        if (TextWindow.PeekChar(1) == u'&') {
            TextWindow.AdvanceChar(2);
            info.Kind = SyntaxKind::AmpersandAmpersandToken;
            break;
        }

        goto case_default;

    case u'|':
        if (TextWindow.PeekChar(1) == u'|') {
            TextWindow.AdvanceChar(2);
            info.Kind = SyntaxKind::BarBarToken;
            break;
        }

        goto case_default;

    case u'0':
    case u'1':
    case u'2':
    case u'3':
    case u'4':
    case u'5':
    case u'6':
    case u'7':
    case u'8':
    case u'9':
        ScanInteger();
        info.Kind = SyntaxKind::NumericLiteralToken;
        info.Text_ = GetInternedLexemeText();
        info.HasText = true;
        info.ValueKind = SpecialType::System_Int32;
        info.IntValue = GetValueInt32(info.Text_, false);
        break;

    case u'\"':
        ScanStringLiteral(info, /*inDirective:*/ true);
        break;

    case u'\\': {
        // Could be unicode escape. Try that.
        character = PeekCharOrUnicodeEscape(surrogateCharacter);
        isEscaped = true;
        if (SyntaxFacts::IsIdentifierStartCharacter(character)) {
            ScanIdentifierOrKeyword(info);
            break;
        }

        goto case_default;
    }

    default:
    case_default:
        if (!isEscaped && SyntaxFacts::IsNewLine(character)) {
            goto case_newline;
        }

        if (SyntaxFacts::IsIdentifierStartCharacter(character)) {
            ScanIdentifierOrKeyword(info);
        } else {
            // unknown single character
            if (isEscaped) {
                std::optional<SyntaxDiagnosticInfo> error;
                NextCharOrUnicodeEscape(surrogateCharacter, error);
                AddError(error);
            } else {
                TextWindow.AdvanceChar();
            }

            info.Kind = SyntaxKind::None;
            info.Text_ = GetInternedLexemeText();
            info.HasText = true;
        }

        break;
    }

    CS_ASSERT(info.Kind != SyntaxKind::None || info.HasText);
    return info.Kind != SyntaxKind::None;
}

void Lexer::LexDirectiveTrailingTrivia(bool includeEndOfLine, SyntaxListBuilder &trivia) {
    GreenNode *tr;
    while (true) {
        int pos = TextWindow.Position();
        tr = LexDirectiveTrivia();
        if (tr == nullptr) {
            break;
        } else if (tr->kind == SyntaxKind::EndOfLineTrivia) {
            if (includeEndOfLine) {
                AddTrivia(tr, trivia);
            } else {
                // don't consume end of line...
                TextWindow.Reset(pos);
            }

            break;
        } else {
            AddTrivia(tr, trivia);
        }
    }
}

GreenNode *Lexer::LexDirectiveTrivia() {
    GreenNode *trivia = nullptr;

    Start();
    char16_t ch = TextWindow.PeekChar();
    switch (ch) {
    case u'/':
        if (TextWindow.PeekChar(1) == u'/') {
            // normal single line comment
            ScanToEndOfLine();
            Text text = GetNonInternedLexemeText();
            trivia = SyntaxFactory::Comment(text);
        }

        break;
    case u'\r':
    case u'\n':
    case_newline:
        trivia = ScanEndOfLine();
        break;
    case u' ':
    case u'\t':  // Horizontal tab
    case u'\v':  // Vertical Tab
    case u'\f':  // Form-feed
    case_space:
        trivia = ScanWhitespace();
        break;

    default:
        if (SyntaxFacts::IsWhitespace(ch)) {
            goto case_space;
        }

        if (SyntaxFacts::IsNewLine(ch)) {
            goto case_newline;
        }

        break;
    }

    return trivia;
}

}  // namespace csharp
