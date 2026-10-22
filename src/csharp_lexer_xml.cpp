// mep's C# lexer, part 2: XML documentation comments, cref / name attribute values and unicode escapes.
// A translation of Roslyn's src/Compilers/CSharp/Portable/Parser/Lexer.cs from LexXmlDocComment to the end
// (MIT, (c) .NET Foundation and Contributors; see csharp_syntax.h for the commit). Same method order as the C#.

#include <algorithm>
#include <string>

#include "csharp_parser_internal.h"

namespace csharp {

namespace {

using XmlParseErrorCode = DocumentationCommentParser::XmlParseErrorCode;

int Xml(XmlParseErrorCode code) { return static_cast<int>(code); }

const char16_t *XmlParseErrorCodeName(int code) {
    static const char16_t *const kNames[] = {
        u"XML_RefUndefinedEntity_1", u"XML_InvalidCharEntity",       u"XML_InvalidUnicodeChar",
        u"XML_InvalidWhitespace",    u"XML_MissingEqualsAttribute",  u"XML_StringLiteralNoStartQuote",
        u"XML_StringLiteralNoEndQuote", u"XML_StringLiteralNonAsciiQuote", u"XML_LessThanInAttributeValue",
        u"XML_IncorrectComment",     u"XML_ElementTypeMatch",        u"XML_DuplicateAttribute",
        u"XML_WhitespaceMissing",    u"XML_EndTagNotExpected",       u"XML_CDataEndTagNotAllowed",
        u"XML_ExpectedIdentifier",   u"XML_ExpectedEndOfTag",        u"XML_InvalidToken",
        u"XML_ExpectedEndOfXml",     u"XML_EndTagExpected",
    };
    return code >= 0 && code < static_cast<int>(sizeof(kNames) / sizeof(kNames[0])) ? kNames[code] : u"XML_?";
}

/** @brief A one-character string in the arena (C# `ch.ToString()`). */
Text CharText(char16_t ch) { return CurrentArena()->CopyText(std::u16string(1, ch)); }

// Roslyn's XmlCharType (Microsoft.CodeAnalysis.XmlCharType, XML 1.0 4th edition name characters):
// IsStartNCNameCharXml4e / IsNCNameCharXml4e, as ranges extracted from the .NET 10 SDK's Microsoft.CodeAnalysis.dll
// by calling the two methods for every UTF-16 unit.
struct XmlCharRange {
    char16_t first;
    char16_t last;
};

const XmlCharRange kStartNCNameCharXml4e[] = {
    {0x0041, 0x005A}, {0x005F, 0x005F}, {0x0061, 0x007A}, {0x00C0, 0x00D6}, {0x00D8, 0x00F6}, {0x00F8, 0x0131},
    {0x0134, 0x013E}, {0x0141, 0x0148}, {0x014A, 0x017E}, {0x0180, 0x01C3}, {0x01CD, 0x01F0}, {0x01F4, 0x01F5},
    {0x01FA, 0x0217}, {0x0250, 0x02A8}, {0x02BB, 0x02C1}, {0x0386, 0x0386}, {0x0388, 0x038A}, {0x038C, 0x038C},
    {0x038E, 0x03A1}, {0x03A3, 0x03CE}, {0x03D0, 0x03D6}, {0x03DA, 0x03DA}, {0x03DC, 0x03DC}, {0x03DE, 0x03DE},
    {0x03E0, 0x03E0}, {0x03E2, 0x03F3}, {0x0401, 0x040C}, {0x040E, 0x044F}, {0x0451, 0x045C}, {0x045E, 0x0481},
    {0x0490, 0x04C4}, {0x04C7, 0x04C8}, {0x04CB, 0x04CC}, {0x04D0, 0x04EB}, {0x04EE, 0x04F5}, {0x04F8, 0x04F9},
    {0x0531, 0x0556}, {0x0559, 0x0559}, {0x0561, 0x0586}, {0x05D0, 0x05EA}, {0x05F0, 0x05F2}, {0x0621, 0x063A},
    {0x0641, 0x064A}, {0x0671, 0x06B7}, {0x06BA, 0x06BE}, {0x06C0, 0x06CE}, {0x06D0, 0x06D3}, {0x06D5, 0x06D5},
    {0x06E5, 0x06E6}, {0x0905, 0x0939}, {0x093D, 0x093D}, {0x0958, 0x0961}, {0x0985, 0x098C}, {0x098F, 0x0990},
    {0x0993, 0x09A8}, {0x09AA, 0x09B0}, {0x09B2, 0x09B2}, {0x09B6, 0x09B9}, {0x09DC, 0x09DD}, {0x09DF, 0x09E1},
    {0x09F0, 0x09F1}, {0x0A05, 0x0A0A}, {0x0A0F, 0x0A10}, {0x0A13, 0x0A28}, {0x0A2A, 0x0A30}, {0x0A32, 0x0A33},
    {0x0A35, 0x0A36}, {0x0A38, 0x0A39}, {0x0A59, 0x0A5C}, {0x0A5E, 0x0A5E}, {0x0A72, 0x0A74}, {0x0A85, 0x0A8B},
    {0x0A8D, 0x0A8D}, {0x0A8F, 0x0A91}, {0x0A93, 0x0AA8}, {0x0AAA, 0x0AB0}, {0x0AB2, 0x0AB3}, {0x0AB5, 0x0AB9},
    {0x0ABD, 0x0ABD}, {0x0AE0, 0x0AE0}, {0x0B05, 0x0B0C}, {0x0B0F, 0x0B10}, {0x0B13, 0x0B28}, {0x0B2A, 0x0B30},
    {0x0B32, 0x0B33}, {0x0B36, 0x0B39}, {0x0B3D, 0x0B3D}, {0x0B5C, 0x0B5D}, {0x0B5F, 0x0B61}, {0x0B85, 0x0B8A},
    {0x0B8E, 0x0B90}, {0x0B92, 0x0B95}, {0x0B99, 0x0B9A}, {0x0B9C, 0x0B9C}, {0x0B9E, 0x0B9F}, {0x0BA3, 0x0BA4},
    {0x0BA8, 0x0BAA}, {0x0BAE, 0x0BB5}, {0x0BB7, 0x0BB9}, {0x0C05, 0x0C0C}, {0x0C0E, 0x0C10}, {0x0C12, 0x0C28},
    {0x0C2A, 0x0C33}, {0x0C35, 0x0C39}, {0x0C60, 0x0C61}, {0x0C85, 0x0C8C}, {0x0C8E, 0x0C90}, {0x0C92, 0x0CA8},
    {0x0CAA, 0x0CB3}, {0x0CB5, 0x0CB9}, {0x0CDE, 0x0CDE}, {0x0CE0, 0x0CE1}, {0x0D05, 0x0D0C}, {0x0D0E, 0x0D10},
    {0x0D12, 0x0D28}, {0x0D2A, 0x0D39}, {0x0D60, 0x0D61}, {0x0E01, 0x0E2E}, {0x0E30, 0x0E30}, {0x0E32, 0x0E33},
    {0x0E40, 0x0E45}, {0x0E81, 0x0E82}, {0x0E84, 0x0E84}, {0x0E87, 0x0E88}, {0x0E8A, 0x0E8A}, {0x0E8D, 0x0E8D},
    {0x0E94, 0x0E97}, {0x0E99, 0x0E9F}, {0x0EA1, 0x0EA3}, {0x0EA5, 0x0EA5}, {0x0EA7, 0x0EA7}, {0x0EAA, 0x0EAB},
    {0x0EAD, 0x0EAE}, {0x0EB0, 0x0EB0}, {0x0EB2, 0x0EB3}, {0x0EBD, 0x0EBD}, {0x0EC0, 0x0EC4}, {0x0F40, 0x0F47},
    {0x0F49, 0x0F69}, {0x10A0, 0x10C5}, {0x10D0, 0x10F6}, {0x1100, 0x1100}, {0x1102, 0x1103}, {0x1105, 0x1107},
    {0x1109, 0x1109}, {0x110B, 0x110C}, {0x110E, 0x1112}, {0x113C, 0x113C}, {0x113E, 0x113E}, {0x1140, 0x1140},
    {0x114C, 0x114C}, {0x114E, 0x114E}, {0x1150, 0x1150}, {0x1154, 0x1155}, {0x1159, 0x1159}, {0x115F, 0x1161},
    {0x1163, 0x1163}, {0x1165, 0x1165}, {0x1167, 0x1167}, {0x1169, 0x1169}, {0x116D, 0x116E}, {0x1172, 0x1173},
    {0x1175, 0x1175}, {0x119E, 0x119E}, {0x11A8, 0x11A8}, {0x11AB, 0x11AB}, {0x11AE, 0x11AF}, {0x11B7, 0x11B8},
    {0x11BA, 0x11BA}, {0x11BC, 0x11C2}, {0x11EB, 0x11EB}, {0x11F0, 0x11F0}, {0x11F9, 0x11F9}, {0x1E00, 0x1E9B},
    {0x1EA0, 0x1EF9}, {0x1F00, 0x1F15}, {0x1F18, 0x1F1D}, {0x1F20, 0x1F45}, {0x1F48, 0x1F4D}, {0x1F50, 0x1F57},
    {0x1F59, 0x1F59}, {0x1F5B, 0x1F5B}, {0x1F5D, 0x1F5D}, {0x1F5F, 0x1F7D}, {0x1F80, 0x1FB4}, {0x1FB6, 0x1FBC},
    {0x1FBE, 0x1FBE}, {0x1FC2, 0x1FC4}, {0x1FC6, 0x1FCC}, {0x1FD0, 0x1FD3}, {0x1FD6, 0x1FDB}, {0x1FE0, 0x1FEC},
    {0x1FF2, 0x1FF4}, {0x1FF6, 0x1FFC}, {0x2126, 0x2126}, {0x212A, 0x212B}, {0x212E, 0x212E}, {0x2180, 0x2182},
    {0x3007, 0x3007}, {0x3021, 0x3029}, {0x3041, 0x3094}, {0x30A1, 0x30FA}, {0x3105, 0x312C}, {0x4E00, 0x9FA5},
    {0xAC00, 0xD7A3},
};
const XmlCharRange kNCNameCharXml4e[] = {
    {0x002D, 0x002E}, {0x0030, 0x0039}, {0x0041, 0x005A}, {0x005F, 0x005F}, {0x0061, 0x007A}, {0x00B7, 0x00B7},
    {0x00C0, 0x00D6}, {0x00D8, 0x00F6}, {0x00F8, 0x0131}, {0x0134, 0x013E}, {0x0141, 0x0148}, {0x014A, 0x017E},
    {0x0180, 0x01C3}, {0x01CD, 0x01F0}, {0x01F4, 0x01F5}, {0x01FA, 0x0217}, {0x0250, 0x02A8}, {0x02BB, 0x02C1},
    {0x02D0, 0x02D1}, {0x0300, 0x0345}, {0x0360, 0x0361}, {0x0386, 0x038A}, {0x038C, 0x038C}, {0x038E, 0x03A1},
    {0x03A3, 0x03CE}, {0x03D0, 0x03D6}, {0x03DA, 0x03DA}, {0x03DC, 0x03DC}, {0x03DE, 0x03DE}, {0x03E0, 0x03E0},
    {0x03E2, 0x03F3}, {0x0401, 0x040C}, {0x040E, 0x044F}, {0x0451, 0x045C}, {0x045E, 0x0481}, {0x0483, 0x0486},
    {0x0490, 0x04C4}, {0x04C7, 0x04C8}, {0x04CB, 0x04CC}, {0x04D0, 0x04EB}, {0x04EE, 0x04F5}, {0x04F8, 0x04F9},
    {0x0531, 0x0556}, {0x0559, 0x0559}, {0x0561, 0x0586}, {0x0591, 0x05A1}, {0x05A3, 0x05B9}, {0x05BB, 0x05BD},
    {0x05BF, 0x05BF}, {0x05C1, 0x05C2}, {0x05C4, 0x05C4}, {0x05D0, 0x05EA}, {0x05F0, 0x05F2}, {0x0621, 0x063A},
    {0x0640, 0x0652}, {0x0660, 0x0669}, {0x0670, 0x06B7}, {0x06BA, 0x06BE}, {0x06C0, 0x06CE}, {0x06D0, 0x06D3},
    {0x06D5, 0x06E8}, {0x06EA, 0x06ED}, {0x06F0, 0x06F9}, {0x0901, 0x0903}, {0x0905, 0x0939}, {0x093C, 0x094D},
    {0x0951, 0x0954}, {0x0958, 0x0963}, {0x0966, 0x096F}, {0x0981, 0x0983}, {0x0985, 0x098C}, {0x098F, 0x0990},
    {0x0993, 0x09A8}, {0x09AA, 0x09B0}, {0x09B2, 0x09B2}, {0x09B6, 0x09B9}, {0x09BC, 0x09BC}, {0x09BE, 0x09C4},
    {0x09C7, 0x09C8}, {0x09CB, 0x09CD}, {0x09D7, 0x09D7}, {0x09DC, 0x09DD}, {0x09DF, 0x09E3}, {0x09E6, 0x09F1},
    {0x0A02, 0x0A02}, {0x0A05, 0x0A0A}, {0x0A0F, 0x0A10}, {0x0A13, 0x0A28}, {0x0A2A, 0x0A30}, {0x0A32, 0x0A33},
    {0x0A35, 0x0A36}, {0x0A38, 0x0A39}, {0x0A3C, 0x0A3C}, {0x0A3E, 0x0A42}, {0x0A47, 0x0A48}, {0x0A4B, 0x0A4D},
    {0x0A59, 0x0A5C}, {0x0A5E, 0x0A5E}, {0x0A66, 0x0A74}, {0x0A81, 0x0A83}, {0x0A85, 0x0A8B}, {0x0A8D, 0x0A8D},
    {0x0A8F, 0x0A91}, {0x0A93, 0x0AA8}, {0x0AAA, 0x0AB0}, {0x0AB2, 0x0AB3}, {0x0AB5, 0x0AB9}, {0x0ABC, 0x0AC5},
    {0x0AC7, 0x0AC9}, {0x0ACB, 0x0ACD}, {0x0AE0, 0x0AE0}, {0x0AE6, 0x0AEF}, {0x0B01, 0x0B03}, {0x0B05, 0x0B0C},
    {0x0B0F, 0x0B10}, {0x0B13, 0x0B28}, {0x0B2A, 0x0B30}, {0x0B32, 0x0B33}, {0x0B36, 0x0B39}, {0x0B3C, 0x0B43},
    {0x0B47, 0x0B48}, {0x0B4B, 0x0B4D}, {0x0B56, 0x0B57}, {0x0B5C, 0x0B5D}, {0x0B5F, 0x0B61}, {0x0B66, 0x0B6F},
    {0x0B82, 0x0B83}, {0x0B85, 0x0B8A}, {0x0B8E, 0x0B90}, {0x0B92, 0x0B95}, {0x0B99, 0x0B9A}, {0x0B9C, 0x0B9C},
    {0x0B9E, 0x0B9F}, {0x0BA3, 0x0BA4}, {0x0BA8, 0x0BAA}, {0x0BAE, 0x0BB5}, {0x0BB7, 0x0BB9}, {0x0BBE, 0x0BC2},
    {0x0BC6, 0x0BC8}, {0x0BCA, 0x0BCD}, {0x0BD7, 0x0BD7}, {0x0BE7, 0x0BEF}, {0x0C01, 0x0C03}, {0x0C05, 0x0C0C},
    {0x0C0E, 0x0C10}, {0x0C12, 0x0C28}, {0x0C2A, 0x0C33}, {0x0C35, 0x0C39}, {0x0C3E, 0x0C44}, {0x0C46, 0x0C48},
    {0x0C4A, 0x0C4D}, {0x0C55, 0x0C56}, {0x0C60, 0x0C61}, {0x0C66, 0x0C6F}, {0x0C82, 0x0C83}, {0x0C85, 0x0C8C},
    {0x0C8E, 0x0C90}, {0x0C92, 0x0CA8}, {0x0CAA, 0x0CB3}, {0x0CB5, 0x0CB9}, {0x0CBE, 0x0CC4}, {0x0CC6, 0x0CC8},
    {0x0CCA, 0x0CCD}, {0x0CD5, 0x0CD6}, {0x0CDE, 0x0CDE}, {0x0CE0, 0x0CE1}, {0x0CE6, 0x0CEF}, {0x0D02, 0x0D03},
    {0x0D05, 0x0D0C}, {0x0D0E, 0x0D10}, {0x0D12, 0x0D28}, {0x0D2A, 0x0D39}, {0x0D3E, 0x0D43}, {0x0D46, 0x0D48},
    {0x0D4A, 0x0D4D}, {0x0D57, 0x0D57}, {0x0D60, 0x0D61}, {0x0D66, 0x0D6F}, {0x0E01, 0x0E2E}, {0x0E30, 0x0E3A},
    {0x0E40, 0x0E4E}, {0x0E50, 0x0E59}, {0x0E81, 0x0E82}, {0x0E84, 0x0E84}, {0x0E87, 0x0E88}, {0x0E8A, 0x0E8A},
    {0x0E8D, 0x0E8D}, {0x0E94, 0x0E97}, {0x0E99, 0x0E9F}, {0x0EA1, 0x0EA3}, {0x0EA5, 0x0EA5}, {0x0EA7, 0x0EA7},
    {0x0EAA, 0x0EAB}, {0x0EAD, 0x0EAE}, {0x0EB0, 0x0EB9}, {0x0EBB, 0x0EBD}, {0x0EC0, 0x0EC4}, {0x0EC6, 0x0EC6},
    {0x0EC8, 0x0ECD}, {0x0ED0, 0x0ED9}, {0x0F18, 0x0F19}, {0x0F20, 0x0F29}, {0x0F35, 0x0F35}, {0x0F37, 0x0F37},
    {0x0F39, 0x0F39}, {0x0F3E, 0x0F47}, {0x0F49, 0x0F69}, {0x0F71, 0x0F84}, {0x0F86, 0x0F8B}, {0x0F90, 0x0F95},
    {0x0F97, 0x0F97}, {0x0F99, 0x0FAD}, {0x0FB1, 0x0FB7}, {0x0FB9, 0x0FB9}, {0x10A0, 0x10C5}, {0x10D0, 0x10F6},
    {0x1100, 0x1100}, {0x1102, 0x1103}, {0x1105, 0x1107}, {0x1109, 0x1109}, {0x110B, 0x110C}, {0x110E, 0x1112},
    {0x113C, 0x113C}, {0x113E, 0x113E}, {0x1140, 0x1140}, {0x114C, 0x114C}, {0x114E, 0x114E}, {0x1150, 0x1150},
    {0x1154, 0x1155}, {0x1159, 0x1159}, {0x115F, 0x1161}, {0x1163, 0x1163}, {0x1165, 0x1165}, {0x1167, 0x1167},
    {0x1169, 0x1169}, {0x116D, 0x116E}, {0x1172, 0x1173}, {0x1175, 0x1175}, {0x119E, 0x119E}, {0x11A8, 0x11A8},
    {0x11AB, 0x11AB}, {0x11AE, 0x11AF}, {0x11B7, 0x11B8}, {0x11BA, 0x11BA}, {0x11BC, 0x11C2}, {0x11EB, 0x11EB},
    {0x11F0, 0x11F0}, {0x11F9, 0x11F9}, {0x1E00, 0x1E9B}, {0x1EA0, 0x1EF9}, {0x1F00, 0x1F15}, {0x1F18, 0x1F1D},
    {0x1F20, 0x1F45}, {0x1F48, 0x1F4D}, {0x1F50, 0x1F57}, {0x1F59, 0x1F59}, {0x1F5B, 0x1F5B}, {0x1F5D, 0x1F5D},
    {0x1F5F, 0x1F7D}, {0x1F80, 0x1FB4}, {0x1FB6, 0x1FBC}, {0x1FBE, 0x1FBE}, {0x1FC2, 0x1FC4}, {0x1FC6, 0x1FCC},
    {0x1FD0, 0x1FD3}, {0x1FD6, 0x1FDB}, {0x1FE0, 0x1FEC}, {0x1FF2, 0x1FF4}, {0x1FF6, 0x1FFC}, {0x20D0, 0x20DC},
    {0x20E1, 0x20E1}, {0x2126, 0x2126}, {0x212A, 0x212B}, {0x212E, 0x212E}, {0x2180, 0x2182}, {0x3005, 0x3005},
    {0x3007, 0x3007}, {0x3021, 0x302F}, {0x3031, 0x3035}, {0x3041, 0x3094}, {0x3099, 0x309A}, {0x309D, 0x309E},
    {0x30A1, 0x30FA}, {0x30FC, 0x30FE}, {0x3105, 0x312C}, {0x4E00, 0x9FA5}, {0xAC00, 0xD7A3},
};

template <size_t N>
bool InRanges(const XmlCharRange (&ranges)[N], char16_t ch) {
    const XmlCharRange *end = ranges + N;
    const XmlCharRange *it =
        std::upper_bound(ranges, end, ch, [](char16_t c, const XmlCharRange &r) { return c < r.first; });
    if (it == ranges) return false;
    --it;
    return ch <= it->last;
}

}  // namespace

// AbstractLexer.AddError(XmlParseErrorCode code, params object[] args): Roslyn makes an XmlSyntaxDiagnosticInfo(0, 0,
// code, args), reported as WRN_XMLParseError with the XML message. These are only ever produced while lexing a
// documentation comment, where Create() drops the token's errors (DocumentationMode.Parse), so they never reach a
// tree. They are still recorded (as WRN_XMLParseError, offset 0 width 0, the XmlParseErrorCode's name as the first
// argument) so that HasErrors / GetErrors behave as in Roslyn.
void Lexer::AddXmlError(int xmlParseErrorCode, std::initializer_list<Text> args) {
    SyntaxDiagnosticInfo d;
    d.offset = 0;
    d.width = 0;
    d.code = ErrorCode::WRN_XMLParseError;
    d.args[d.arg_count++] = Text(XmlParseErrorCodeName(xmlParseErrorCode));
    for (Text a : args) {
        if (d.arg_count < 4) d.args[d.arg_count++] = a;
    }
    AddError(d);
}

GreenNode *Lexer::LexXmlDocComment(XmlDocCommentStyle style) {
    LexerMode saveMode = _mode;

    if (!_xmlParser) _xmlParser = std::make_unique<DocumentationCommentParser>(this);
    _xmlParser->ReInitialize(style == XmlDocCommentStyle::SingleLine ? LexerMode::XmlDocCommentStyleSingleLine
                                                                     : LexerMode::XmlDocCommentStyleDelimited);

    bool isTerminated = false;
    GreenNode *docComment = _xmlParser->ParseDocumentationComment(isTerminated);

    // We better have finished with the whole comment. There should be error
    // code in the implementation of ParseXmlDocComment that ensures this.
    CS_ASSERT(LocationIs(XmlDocCommentLocation::End) || TextWindow.PeekChar() == SlidingTextWindow::InvalidCharacter);

    _mode = saveMode;

    if (!isTerminated) {
        // The comment didn't end.  Report an error at the start point.
        // NOTE: report this error even if the DocumentationMode is less than diagnose - the comment
        // would be malformed as a non-doc comment as well.
        AddError(LexemeStartPosition, CurrentLexemeWidth(), ErrorCode::ERR_OpenEndedComment);
    }

    return docComment;
}

/// Lexer entry point for LexMode.XmlDocComment
GreenNode *Lexer::LexXmlToken() {
    TokenInfo xmlTokenInfo{};

    SyntaxListBuilder leading;
    LexXmlDocCommentLeadingTrivia(leading);

    Start();
    ScanXmlToken(xmlTokenInfo);
    std::vector<SyntaxDiagnosticInfo> errors = GetErrors();

    return Create(xmlTokenInfo, leading.ToList(), nullptr, errors);
}

bool Lexer::ScanXmlToken(TokenInfo &info) {
    char16_t ch;

    CS_ASSERT(!LocationIs(XmlDocCommentLocation::Start));
    CS_ASSERT(!LocationIs(XmlDocCommentLocation::Exterior));

    if (LocationIs(XmlDocCommentLocation::End)) {
        info.Kind = SyntaxKind::EndOfDocumentationCommentToken;
        return true;
    }

    ch = TextWindow.PeekChar();
    if (ch == u'&') {
        ScanXmlEntity(info);
        info.Kind = SyntaxKind::XmlEntityLiteralToken;
    } else if (ch == u'<') {
        ScanXmlTagStart(info);
    } else if (ch == u'\r' || ch == u'\n') {
        ScanXmlTextLiteralNewLineToken(info);
    } else if (ch == SlidingTextWindow::InvalidCharacter && TextWindow.IsReallyAtEnd()) {
        info.Kind = SyntaxKind::EndOfDocumentationCommentToken;
    } else {
        // default:
        if (SyntaxFacts::IsNewLine(ch)) {
            ScanXmlTextLiteralNewLineToken(info);  // goto case '\n'
        } else {
            ScanXmlText(info);
            info.Kind = SyntaxKind::XmlTextLiteralToken;
        }
    }

    CS_ASSERT(info.Kind != SyntaxKind::None || info.HasText);
    return info.Kind != SyntaxKind::None;
}

void Lexer::ScanXmlTextLiteralNewLineToken(TokenInfo &info) {
    ScanEndOfLine();
    info.StringValue = info.Text_ = GetNonInternedLexemeText();
    info.HasStringValue = info.HasText = true;
    info.Kind = SyntaxKind::XmlTextLiteralNewLineToken;
    MutateLocation(XmlDocCommentLocation::Exterior);
}

void Lexer::ScanXmlTagStart(TokenInfo &info) {
    CS_ASSERT(TextWindow.PeekChar() == u'<');

    if (TextWindow.PeekChar(1) == u'!') {
        if (TextWindow.PeekChar(2) == u'-' && TextWindow.PeekChar(3) == u'-') {
            TextWindow.AdvanceChar(4);
            info.Kind = SyntaxKind::XmlCommentStartToken;
        } else if (TextWindow.PeekChar(2) == u'[' && TextWindow.PeekChar(3) == u'C' && TextWindow.PeekChar(4) == u'D' &&
                   TextWindow.PeekChar(5) == u'A' && TextWindow.PeekChar(6) == u'T' && TextWindow.PeekChar(7) == u'A' &&
                   TextWindow.PeekChar(8) == u'[') {
            TextWindow.AdvanceChar(9);
            info.Kind = SyntaxKind::XmlCDataStartToken;
        } else {
            // TODO: Take the < by itself, I guess?
            TextWindow.AdvanceChar();
            info.Kind = SyntaxKind::LessThanToken;
        }
    } else if (TextWindow.PeekChar(1) == u'/') {
        TextWindow.AdvanceChar(2);
        info.Kind = SyntaxKind::LessThanSlashToken;
    } else if (TextWindow.PeekChar(1) == u'?') {
        TextWindow.AdvanceChar(2);
        info.Kind = SyntaxKind::XmlProcessingInstructionStartToken;
    } else {
        TextWindow.AdvanceChar();
        info.Kind = SyntaxKind::LessThanToken;
    }
}

void Lexer::ScanXmlEntity(TokenInfo &info) {
    info.StringValue = Text();
    info.HasStringValue = false;

    CS_ASSERT(TextWindow.PeekChar() == u'&');
    TextWindow.AdvanceChar();
    _builder.clear();
    std::optional<int> error;
    std::vector<Text> errorArgs;  // null == empty

    char16_t ch;
    if (IsXmlNameStartChar(ch = TextWindow.PeekChar())) {
        while (IsXmlNameChar(ch = TextWindow.PeekChar())) {
            // Important bit of information here: none of \0, \r, \n, and crucially for
            // delimited comments, * are considered Xml name characters. Also, since
            // entities appear in xml text and attribute text, it's relevant here that
            // none of <, /, >, ', ", =, are Xml name characters. Note that - and ] are
            // irrelevant--entities do not appear in comments or cdata.

            TextWindow.AdvanceChar();
            _builder.push_back(ch);
        }

        if (_builder == u"lt") {
            info.StringValue = u"<";
            info.HasStringValue = true;
        } else if (_builder == u"gt") {
            info.StringValue = u">";
            info.HasStringValue = true;
        } else if (_builder == u"amp") {
            info.StringValue = u"&";
            info.HasStringValue = true;
        } else if (_builder == u"apos") {
            info.StringValue = u"'";
            info.HasStringValue = true;
        } else if (_builder == u"quot") {
            info.StringValue = u"\"";
            info.HasStringValue = true;
        } else {
            error = Xml(XmlParseErrorCode::XML_RefUndefinedEntity_1);
            errorArgs = {Intern(_builder)};
        }
    } else if (ch == u'#') {
        TextWindow.AdvanceChar();
        bool isHex = TextWindow.PeekChar() == u'x';
        uint32_t charValue = 0;

        if (isHex) {
            TextWindow.AdvanceChar();  // x
            while (SyntaxFacts::IsHexDigit(ch = TextWindow.PeekChar())) {
                TextWindow.AdvanceChar();

                // disallow overflow
                if (charValue <= 0x7FFFFFF) {
                    charValue = (charValue << 4) + static_cast<uint32_t>(SyntaxFacts::HexValue(ch));
                }
            }
        } else {
            while (SyntaxFacts::IsDecDigit(ch = TextWindow.PeekChar())) {
                TextWindow.AdvanceChar();

                // disallow overflow
                if (charValue <= 0x7FFFFFF) {
                    charValue = (charValue << 3) + (charValue << 1) + static_cast<uint32_t>(SyntaxFacts::DecValue(ch));
                }
            }
        }

        if (TextWindow.PeekChar() != u';') {
            error = Xml(XmlParseErrorCode::XML_InvalidCharEntity);
        }

        if (MatchesProductionForXmlChar(charValue)) {
            char16_t lowSurrogate;
            char16_t highSurrogate = GetCharsFromUtf32(charValue, lowSurrogate);

            _builder.push_back(highSurrogate);
            if (lowSurrogate != SlidingTextWindow::InvalidCharacter) {
                _builder.push_back(lowSurrogate);
            }

            info.StringValue = Intern(_builder);
            info.HasStringValue = true;
        } else {
            if (!error) {
                error = Xml(XmlParseErrorCode::XML_InvalidUnicodeChar);
            }
        }
    } else {
        if (SyntaxFacts::IsWhitespace(ch) || SyntaxFacts::IsNewLine(ch)) {
            if (!error) {
                error = Xml(XmlParseErrorCode::XML_InvalidWhitespace);
            }
        } else {
            if (!error) {
                error = Xml(XmlParseErrorCode::XML_InvalidToken);
                errorArgs = {CharText(ch)};
            }
        }
    }

    ch = TextWindow.PeekChar();
    if (ch == u';') {
        TextWindow.AdvanceChar();
    } else {
        if (!error) {
            error = Xml(XmlParseErrorCode::XML_InvalidToken);
            errorArgs = {CharText(ch)};
        }
    }

    // If we don't have a value computed from above, then we don't recognize the entity, in which
    // case we will simply use the text.

    info.Text_ = GetInternedLexemeText();
    info.HasText = true;
    if (!info.HasStringValue) {
        info.StringValue = info.Text_;
        info.HasStringValue = true;
    }

    if (error) {
        if (errorArgs.empty()) {
            AddXmlError(*error);
        } else {
            AddXmlError(*error, {errorArgs[0]});
        }
    }
}

bool Lexer::MatchesProductionForXmlChar(uint32_t charValue) {
    // Char ::= #x9 | #xA | #xD | [#x20-#xD7FF] | [#xE000-#xFFFD] | [#x10000-#x10FFFF] /* any Unicode character, excluding the surrogate blocks, FFFE, and FFFF. */

    return charValue == 0x9 || charValue == 0xA || charValue == 0xD || (charValue >= 0x20 && charValue <= 0xD7FF) ||
           (charValue >= 0xE000 && charValue <= 0xFFFD) || (charValue >= 0x10000 && charValue <= 0x10FFFF);
}

void Lexer::ScanXmlText(TokenInfo &info) {
    // Collect "]]>" strings into their own XmlText.
    if (TextWindow.PeekChar() == u']' && TextWindow.PeekChar(1) == u']' && TextWindow.PeekChar(2) == u'>') {
        TextWindow.AdvanceChar(3);
        info.StringValue = info.Text_ = GetNonInternedLexemeText();
        info.HasStringValue = info.HasText = true;
        AddXmlError(Xml(XmlParseErrorCode::XML_CDataEndTagNotAllowed));
        return;
    }

    while (true) {
        char16_t ch = TextWindow.PeekChar();
        bool stop = false;
        if (ch == SlidingTextWindow::InvalidCharacter && TextWindow.IsReallyAtEnd()) {
            stop = true;
        } else if (ch == u'&' || ch == u'<' || ch == u'\r' || ch == u'\n') {
            stop = true;
        } else if (ch == u'*' && StyleIs(XmlDocCommentStyle::Delimited) && TextWindow.PeekChar(1) == u'/') {
            // we're at the end of the comment, but don't lex it yet.
            stop = true;
        } else if (ch == u']' && TextWindow.PeekChar(1) == u']' && TextWindow.PeekChar(2) == u'>') {
            stop = true;
        } else if (SyntaxFacts::IsNewLine(ch)) {
            stop = true;  // default: goto case '\n'
        }

        if (stop) {
            info.StringValue = info.Text_ = GetNonInternedLexemeText();
            info.HasStringValue = info.HasText = true;
            return;
        }

        TextWindow.AdvanceChar();
    }
}

/// Lexer entry point for LexMode.XmlElementTag
GreenNode *Lexer::LexXmlElementTagToken() {
    TokenInfo tagInfo{};

    SyntaxListBuilder leading;
    LexXmlDocCommentLeadingTriviaWithWhitespace(leading);

    Start();
    ScanXmlElementTagToken(tagInfo);
    std::vector<SyntaxDiagnosticInfo> errors = GetErrors();

    // PERF: De-dupe common XML element tags
    if (errors.empty() && tagInfo.ContextualKind == SyntaxKind::None && tagInfo.Kind == SyntaxKind::IdentifierToken) {
        GreenNode *token = DocumentationCommentParser::DocumentationCommentXmlTokens::LookupToken(tagInfo.Text_, leading);
        if (token != nullptr) {
            return token;
        }
    }

    return Create(tagInfo, leading.ToList(), nullptr, errors);
}

bool Lexer::ScanXmlElementTagToken(TokenInfo &info) {
    char16_t ch;

    CS_ASSERT(!LocationIs(XmlDocCommentLocation::Start));
    CS_ASSERT(!LocationIs(XmlDocCommentLocation::Exterior));

    if (LocationIs(XmlDocCommentLocation::End)) {
        info.Kind = SyntaxKind::EndOfDocumentationCommentToken;
        return true;
    }

    ch = TextWindow.PeekChar();
    bool isDefault = false;
    switch (ch) {
        case u'<':
            ScanXmlTagStart(info);
            break;

        case u'>':
            TextWindow.AdvanceChar();
            info.Kind = SyntaxKind::GreaterThanToken;
            break;

        case u'/':
            if (TextWindow.PeekChar(1) == u'>') {
                TextWindow.AdvanceChar(2);
                info.Kind = SyntaxKind::SlashGreaterThanToken;
                break;
            }

            isDefault = true;
            break;

        case u'"':
            TextWindow.AdvanceChar();
            info.Kind = SyntaxKind::DoubleQuoteToken;
            break;

        case u'\'':
            TextWindow.AdvanceChar();
            info.Kind = SyntaxKind::SingleQuoteToken;
            break;

        case u'=':
            TextWindow.AdvanceChar();
            info.Kind = SyntaxKind::EqualsToken;
            break;

        case u':':
            TextWindow.AdvanceChar();
            info.Kind = SyntaxKind::ColonToken;
            break;

        case u'\r':
        case u'\n':
            // Assert?
            break;

        case SlidingTextWindow::InvalidCharacter:
            if (!TextWindow.IsReallyAtEnd()) {
                isDefault = true;
                break;
            }

            info.Kind = SyntaxKind::EndOfDocumentationCommentToken;
            break;

        case u'*':
            if (StyleIs(XmlDocCommentStyle::Delimited) && TextWindow.PeekChar(1) == u'/') {
                // Assert? We should have gotten this in the leading trivia.
                CS_ASSERT(false);  // "Should have picked up leading indentationTrivia, but didn't."
                break;
            }

            isDefault = true;
            break;

        default:
            isDefault = true;
            break;
    }

    if (isDefault) {
        if (IsXmlNameStartChar(ch)) {
            ScanXmlName(info);
            info.StringValue = info.Text_;
            info.HasStringValue = info.HasText;
            info.Kind = SyntaxKind::IdentifierToken;
        } else if (SyntaxFacts::IsWhitespace(ch) || SyntaxFacts::IsNewLine(ch)) {
            // whitespace! needed to do a better job with trivia
            CS_ASSERT(false);  // "Should have picked up leading indentationTrivia, but didn't."
        } else {
            TextWindow.AdvanceChar();
            info.Kind = SyntaxKind::None;
            info.StringValue = info.Text_ = GetNonInternedLexemeText();
            info.HasStringValue = info.HasText = true;
        }
    }

    CS_ASSERT(info.Kind != SyntaxKind::None || info.HasText);
    return info.Kind != SyntaxKind::None;
}

void Lexer::ScanXmlName(TokenInfo &info) {
    int start = TextWindow.Position();

    while (true) {
        char16_t ch = TextWindow.PeekChar();

        // Important bit of information here: none of \0, \r, \n, and crucially for
        // delimited comments, * are considered Xml name characters.
        if (ch != u':' && IsXmlNameChar(ch)) {
            // Although ':' is a name char, we don't include it in ScanXmlName
            // since it is its own token. This enables the parser to add structure
            // to colon-separated names.

            // TODO: Could put a big switch here for common cases
            // if this is a perf bottleneck.
            TextWindow.AdvanceChar();
        } else {
            break;
        }
    }

    info.Text_ = TextWindow.GetText(start, TextWindow.Position() - start);
    info.HasText = true;
}

/// Determines whether this Unicode character can start a XMLName.
bool Lexer::IsXmlNameStartChar(char16_t ch) {
    // TODO: which is the right one?
    return InRanges(kStartNCNameCharXml4e, ch);  // XmlCharType.IsStartNCNameCharXml4e(ch)
}

/// Determines if this Unicode character can be part of an XML Name.
bool Lexer::IsXmlNameChar(char16_t ch) {
    // TODO: which is the right one?
    return InRanges(kNCNameCharXml4e, ch);  // XmlCharType.IsNCNameCharXml4e(ch)
}

// TODO: There is a lot of duplication between attribute text, CDATA text, and comment text.
// It would be nice to factor them together.

/// Lexer entry point for LexMode.XmlAttributeText
GreenNode *Lexer::LexXmlAttributeTextToken() {
    TokenInfo info{};

    SyntaxListBuilder leading;
    LexXmlDocCommentLeadingTrivia(leading);

    Start();
    ScanXmlAttributeTextToken(info);
    std::vector<SyntaxDiagnosticInfo> errors = GetErrors();

    return Create(info, leading.ToList(), nullptr, errors);
}

bool Lexer::ScanXmlAttributeTextToken(TokenInfo &info) {
    CS_ASSERT(!LocationIs(XmlDocCommentLocation::Start));
    CS_ASSERT(!LocationIs(XmlDocCommentLocation::Exterior));

    if (LocationIs(XmlDocCommentLocation::End)) {
        info.Kind = SyntaxKind::EndOfDocumentationCommentToken;
        return true;
    }

    char16_t ch = TextWindow.PeekChar();
    bool isDefault = false;
    switch (ch) {
        case u'"':
            if (ModeIs(LexerMode::XmlAttributeTextDoubleQuote)) {
                TextWindow.AdvanceChar();
                info.Kind = SyntaxKind::DoubleQuoteToken;
                break;
            }

            isDefault = true;
            break;

        case u'\'':
            if (ModeIs(LexerMode::XmlAttributeTextQuote)) {
                TextWindow.AdvanceChar();
                info.Kind = SyntaxKind::SingleQuoteToken;
                break;
            }

            isDefault = true;
            break;

        case u'&':
            ScanXmlEntity(info);
            info.Kind = SyntaxKind::XmlEntityLiteralToken;
            break;

        case u'<':
            TextWindow.AdvanceChar();
            info.Kind = SyntaxKind::LessThanToken;
            break;

        case u'\r':
        case u'\n':
            ScanXmlTextLiteralNewLineToken(info);
            break;

        case SlidingTextWindow::InvalidCharacter:
            if (!TextWindow.IsReallyAtEnd()) {
                isDefault = true;
                break;
            }

            info.Kind = SyntaxKind::EndOfDocumentationCommentToken;
            break;

        default:
            isDefault = true;
            break;
    }

    if (isDefault) {
        if (SyntaxFacts::IsNewLine(ch)) {
            ScanXmlTextLiteralNewLineToken(info);  // goto case '\n'
        } else {
            ScanXmlAttributeText(info);
            info.Kind = SyntaxKind::XmlTextLiteralToken;
        }
    }

    CS_ASSERT(info.Kind != SyntaxKind::None || info.HasText);
    return info.Kind != SyntaxKind::None;
}

void Lexer::ScanXmlAttributeText(TokenInfo &info) {
    while (true) {
        char16_t ch = TextWindow.PeekChar();
        bool stop = false;
        if (ch == u'"' && ModeIs(LexerMode::XmlAttributeTextDoubleQuote)) {
            stop = true;
        } else if (ch == u'\'' && ModeIs(LexerMode::XmlAttributeTextQuote)) {
            stop = true;
        } else if (ch == u'&' || ch == u'<' || ch == u'\r' || ch == u'\n') {
            stop = true;
        } else if (ch == SlidingTextWindow::InvalidCharacter && TextWindow.IsReallyAtEnd()) {
            stop = true;
        } else if (ch == u'*' && StyleIs(XmlDocCommentStyle::Delimited) && TextWindow.PeekChar(1) == u'/') {
            // we're at the end of the comment, but don't lex it yet.
            stop = true;
        } else if (SyntaxFacts::IsNewLine(ch)) {
            stop = true;  // default: goto case '\n'
        }

        if (stop) {
            info.StringValue = info.Text_ = GetNonInternedLexemeText();
            info.HasStringValue = info.HasText = true;
            return;
        }

        TextWindow.AdvanceChar();
    }
}

/// Lexer entry point for LexerMode.XmlCharacter.
GreenNode *Lexer::LexXmlCharacter() {
    TokenInfo info{};

    // TODO: Dev11 allows C# comments and newlines in cref trivia (DevDiv #530523).
    SyntaxListBuilder leading;
    LexXmlDocCommentLeadingTriviaWithWhitespace(leading);

    Start();
    ScanXmlCharacter(info);
    std::vector<SyntaxDiagnosticInfo> errors = GetErrors();

    return Create(info, leading.ToList(), nullptr, errors);
}

/// Scan a single XML character (or entity).  Assumes that leading trivia has already
/// been consumed.
bool Lexer::ScanXmlCharacter(TokenInfo &info) {
    CS_ASSERT(!LocationIs(XmlDocCommentLocation::Start));
    CS_ASSERT(!LocationIs(XmlDocCommentLocation::Exterior));

    if (LocationIs(XmlDocCommentLocation::End)) {
        info.Kind = SyntaxKind::EndOfDocumentationCommentToken;
        return true;
    }

    char16_t ch = TextWindow.PeekChar();
    if (ch == u'&') {
        ScanXmlEntity(info);
        info.Kind = SyntaxKind::XmlEntityLiteralToken;
    } else if (ch == SlidingTextWindow::InvalidCharacter && TextWindow.IsReallyAtEnd()) {
        info.Kind = SyntaxKind::EndOfFileToken;
    } else {
        info.Kind = SyntaxKind::XmlTextLiteralToken;
        char16_t next = TextWindow.NextChar();
        info.Text_ = info.StringValue = CharText(next);
        info.HasText = info.HasStringValue = true;
    }

    return true;
}

/// Lexer entry point for LexerMode.XmlCrefQuote, LexerMode.XmlCrefDoubleQuote,
/// LexerMode.XmlNameQuote, and LexerMode.XmlNameDoubleQuote.
GreenNode *Lexer::LexXmlCrefOrNameToken() {
    TokenInfo info{};

    // TODO: Dev11 allows C# comments and newlines in cref trivia (DevDiv #530523).
    SyntaxListBuilder leading;
    LexXmlDocCommentLeadingTriviaWithWhitespace(leading);

    Start();
    ScanXmlCrefToken(info);
    std::vector<SyntaxDiagnosticInfo> errors = GetErrors();

    return Create(info, leading.ToList(), nullptr, errors);
}

/// Scan a single cref attribute token.  Assumes that leading trivia has already
/// been consumed.
///
/// Within this method, characters that are not XML meta-characters can be seamlessly
/// replaced with the corresponding XML entities.
bool Lexer::ScanXmlCrefToken(TokenInfo &info) {
    CS_ASSERT(!LocationIs(XmlDocCommentLocation::Start));
    CS_ASSERT(!LocationIs(XmlDocCommentLocation::Exterior));

    if (LocationIs(XmlDocCommentLocation::End)) {
        info.Kind = SyntaxKind::EndOfDocumentationCommentToken;
        return true;
    }

    int beforeConsumed = TextWindow.Position();
    char16_t consumedChar = TextWindow.NextChar();
    char16_t consumedSurrogate = SlidingTextWindow::InvalidCharacter;

    // This first switch is for special characters.  If we see the corresponding
    // XML entities, we DO NOT want to take these actions.
    bool isNewLine = false;
    switch (consumedChar) {
        case u'"':
            if (ModeIs(LexerMode::XmlCrefDoubleQuote) || ModeIs(LexerMode::XmlNameDoubleQuote)) {
                info.Kind = SyntaxKind::DoubleQuoteToken;
                return true;
            }

            break;

        case u'\'':
            if (ModeIs(LexerMode::XmlCrefQuote) || ModeIs(LexerMode::XmlNameQuote)) {
                info.Kind = SyntaxKind::SingleQuoteToken;
                return true;
            }

            break;

        case u'<':
            info.Text_ = GetNonInternedLexemeText();
            info.HasText = true;
            AddXmlError(Xml(XmlParseErrorCode::XML_LessThanInAttributeValue), {info.Text_});  // ErrorCode.WRN_XMLParseError
            return true;

        case SlidingTextWindow::InvalidCharacter:
            if (!TextWindow.IsReallyAtEnd()) {
                // goto default: (InvalidCharacter is not a new line)
                break;
            }

            info.Kind = SyntaxKind::EndOfDocumentationCommentToken;
            return true;

        case u'\r':
        case u'\n':
            isNewLine = true;
            break;

        case u'&':
            TextWindow.Reset(beforeConsumed);
            if (!TryScanXmlEntity(consumedChar, consumedSurrogate)) {
                TextWindow.Reset(beforeConsumed);
                ScanXmlEntity(info);
                info.Kind = SyntaxKind::XmlEntityLiteralToken;
                return true;
            }

            // TryScanXmlEntity advances even when it returns false.
            break;

        case u'{':
            consumedChar = u'<';
            break;

        case u'}':
            consumedChar = u'>';
            break;

        default:
            if (SyntaxFacts::IsNewLine(consumedChar)) {
                isNewLine = true;  // goto case '\n'
            }

            break;
    }

    if (isNewLine) {
        TextWindow.Reset(beforeConsumed);
        ScanXmlTextLiteralNewLineToken(info);
    }

    CS_ASSERT(TextWindow.Position() > beforeConsumed);  // "First character or entity has been consumed."

    // NOTE: None of these cases will be matched if the surrogate is non-zero (UTF-16 rules)
    // so we don't need to check for that explicitly.

    // NOTE: there's a lot of overlap between this switch and the one in
    // ScanSyntaxToken, but we probably don't want to share code because
    // ScanSyntaxToken is really hot code and this switch does some extra
    // work.
    switch (consumedChar) {
        //// Single-Character Punctuation/Operators ////
        case u'(':
            info.Kind = SyntaxKind::OpenParenToken;
            break;
        case u')':
            info.Kind = SyntaxKind::CloseParenToken;
            break;
        case u'[':
            info.Kind = SyntaxKind::OpenBracketToken;
            break;
        case u']':
            info.Kind = SyntaxKind::CloseBracketToken;
            break;
        case u',':
            info.Kind = SyntaxKind::CommaToken;
            break;
        case u'.':
            if (AdvanceIfMatches(u'.')) {
                if (TextWindow.PeekChar() == u'.') {
                    // See documentation in ScanSyntaxToken
                    AddCrefError(ErrorCode::ERR_UnexpectedCharacter, {u"."});
                }

                info.Kind = SyntaxKind::DotDotToken;
            } else {
                info.Kind = SyntaxKind::DotToken;
            }
            break;
        case u'?':
            info.Kind = SyntaxKind::QuestionToken;
            break;
        case u'&':
            info.Kind = SyntaxKind::AmpersandToken;
            break;
        case u'*':
            info.Kind = SyntaxKind::AsteriskToken;
            break;
        case u'|':
            info.Kind = SyntaxKind::BarToken;
            break;
        case u'^':
            info.Kind = SyntaxKind::CaretToken;
            break;
        case u'%':
            info.Kind = SyntaxKind::PercentToken;
            break;
        case u'/':
            info.Kind = SyntaxKind::SlashToken;
            break;
        case u'~':
            info.Kind = SyntaxKind::TildeToken;
            break;

        // NOTE: Special case - convert curly brackets into angle brackets.
        case u'{':
            info.Kind = SyntaxKind::LessThanToken;
            break;
        case u'}':
            info.Kind = SyntaxKind::GreaterThanToken;
            break;

        //// Multi-Character Punctuation/Operators ////
        case u':':
            if (AdvanceIfMatches(u':')) info.Kind = SyntaxKind::ColonColonToken;
            else info.Kind = SyntaxKind::ColonToken;
            break;
        case u'=':
            if (AdvanceIfMatches(u'=')) info.Kind = SyntaxKind::EqualsEqualsToken;
            else info.Kind = SyntaxKind::EqualsToken;
            break;
        case u'!':
            if (AdvanceIfMatches(u'=')) info.Kind = SyntaxKind::ExclamationEqualsToken;
            else info.Kind = SyntaxKind::ExclamationToken;
            break;
        case u'>':
            if (AdvanceIfMatches(u'=')) info.Kind = SyntaxKind::GreaterThanEqualsToken;
            // GreaterThanGreaterThanToken/GreaterThanGreaterThanGreaterThanToken is synthesized in the parser since it is ambiguous (with closing nested type parameter lists)
            else info.Kind = SyntaxKind::GreaterThanToken;
            break;
        case u'<':
            if (AdvanceIfMatches(u'=')) info.Kind = SyntaxKind::LessThanEqualsToken;
            else if (AdvanceIfMatches(u'<')) info.Kind = SyntaxKind::LessThanLessThanToken;
            else info.Kind = SyntaxKind::LessThanToken;
            break;
        case u'+':
            if (AdvanceIfMatches(u'+')) info.Kind = SyntaxKind::PlusPlusToken;
            else info.Kind = SyntaxKind::PlusToken;
            break;
        case u'-':
            if (AdvanceIfMatches(u'-')) info.Kind = SyntaxKind::MinusMinusToken;
            else info.Kind = SyntaxKind::MinusToken;
            break;
        default:
            break;
    }

    if (info.Kind != SyntaxKind::None) {
        CS_ASSERT(!info.HasText);         // "Haven't tried to set it yet."
        CS_ASSERT(!info.HasStringValue);  // "Haven't tried to set it yet."

        Text valueText = SyntaxFacts::GetText(info.Kind);
        Text actualText = GetNonInternedLexemeText();
        if (!valueText.empty() && actualText != valueText) {
            info.RequiresTextForXmlEntity = true;
            info.Text_ = actualText;
            info.HasText = true;
            info.StringValue = valueText;
            info.HasStringValue = true;
        }
    } else {
        // If we didn't match any of the above cases, then we either have an
        // identifier or an unexpected character.

        TextWindow.Reset(beforeConsumed);

        if (ScanIdentifier(info) && info.Text_.size() > 0) {
            // ACASEY:  All valid identifier characters should be valid in XML attribute values,
            // but I don't want to add an assert because XML character classification is expensive.
            // check to see if it is an actual keyword
            // NOTE: name attribute values don't respect keywords - everything is an identifier.
            SyntaxKind keywordKind = SyntaxKind::None;
            if (!InXmlNameAttributeValue() && !info.IsVerbatim && !info.HasIdentifierEscapeSequence &&
                _cache.TryGetKeywordKind(info.StringValue, keywordKind)) {
                if (SyntaxFacts::IsContextualKeyword(keywordKind)) {
                    info.Kind = SyntaxKind::IdentifierToken;
                    info.ContextualKind = keywordKind;
                    // Don't need to set any special flags to store the original text of an identifier.
                } else {
                    info.Kind = keywordKind;
                    info.RequiresTextForXmlEntity = info.Text_ != info.StringValue;
                }
            } else {
                info.ContextualKind = info.Kind = SyntaxKind::IdentifierToken;
            }
        } else {
            if (consumedChar == u'@') {
                // Saw '@', but it wasn't followed by an identifier (otherwise ScanIdentifier would have succeeded).
                if (TextWindow.PeekChar() == u'@') {
                    TextWindow.NextChar();
                    info.Text_ = GetInternedLexemeText();
                    info.HasText = true;
                    info.StringValue = u"";  // Can't be null for an identifier.
                    info.HasStringValue = true;
                } else {
                    ScanXmlEntity(info);
                }
                info.Kind = SyntaxKind::IdentifierToken;
                AddError(ErrorCode::ERR_ExpectedVerbatimLiteral);
            } else if (TextWindow.PeekChar() == u'&') {
                ScanXmlEntity(info);
                info.Kind = SyntaxKind::XmlEntityLiteralToken;
                AddCrefError(ErrorCode::ERR_UnexpectedCharacter, {info.Text_});
            } else {
                char16_t bad = TextWindow.NextChar();
                info.Text_ = GetNonInternedLexemeText();
                info.HasText = true;

                // If it's valid in XML, then it was unexpected in cref mode.
                // Otherwise, it's just bad XML.
                if (MatchesProductionForXmlChar(static_cast<uint32_t>(bad))) {
                    AddCrefError(ErrorCode::ERR_UnexpectedCharacter, {info.Text_});
                } else {
                    AddXmlError(Xml(XmlParseErrorCode::XML_InvalidUnicodeChar));
                }
            }
        }
    }

    CS_ASSERT(info.Kind != SyntaxKind::None || info.HasText);
    return info.Kind != SyntaxKind::None;
}

/// Given a character, advance the input if either the character or the
/// corresponding XML entity appears next in the text window.
bool Lexer::AdvanceIfMatches(char16_t ch) {
    char16_t peekCh = TextWindow.PeekChar();
    if ((peekCh == ch) || (peekCh == u'{' && ch == u'<') || (peekCh == u'}' && ch == u'>')) {
        TextWindow.AdvanceChar();
        return true;
    }

    if (peekCh == u'&') {
        int pos = TextWindow.Position();

        char16_t nextChar;
        char16_t nextSurrogate;
        if (TryScanXmlEntity(nextChar, nextSurrogate) && nextChar == ch &&
            nextSurrogate == SlidingTextWindow::InvalidCharacter) {
            return true;
        }

        TextWindow.Reset(pos);
    }

    return false;
}

/// Convenience property for determining whether we are currently lexing the
/// value of a cref or name attribute.
bool Lexer::InXmlCrefOrNameAttributeValue() const {
    switch (_mode & LexerMode::MaskLexMode) {
        case LexerMode::XmlCrefQuote:
        case LexerMode::XmlCrefDoubleQuote:
        case LexerMode::XmlNameQuote:
        case LexerMode::XmlNameDoubleQuote:
            return true;
        default:
            return false;
    }
}

/// Convenience property for determining whether we are currently lexing the
/// value of a name attribute.
bool Lexer::InXmlNameAttributeValue() const {
    switch (_mode & LexerMode::MaskLexMode) {
        case LexerMode::XmlNameQuote:
        case LexerMode::XmlNameDoubleQuote:
            return true;
        default:
            return false;
    }
}

/// Diagnostics that occur within cref attributes need to be
/// wrapped with ErrorCode.WRN_ErrorOverride.
void Lexer::AddCrefError(ErrorCode code, std::initializer_list<Text> args) {
    AddCrefError(std::optional<SyntaxDiagnosticInfo>(MakeError(code, args)));
}

/// Diagnostics that occur within cref attributes need to be
/// wrapped with ErrorCode.WRN_ErrorOverride.
void Lexer::AddCrefError(const std::optional<SyntaxDiagnosticInfo> &info) {
    if (info) {
        // AddError(ErrorCode.WRN_ErrorOverride, info, info.Code): {0} is the wrapped diagnostic (formatted as its
        // message), {1} its error number. (Only ever produced inside documentation comments, where Create() drops
        // the token's errors.)
        Text message = Intern(ToUtf16(FormatDiagnosticMessage(*info)));
        Text number = Intern(ToUtf16(std::to_string(static_cast<int>(info->code))));
        AddError(ErrorCode::WRN_ErrorOverride, {message, number});
    }
}

/// Lexer entry point for LexMode.XmlCDataSectionText
GreenNode *Lexer::LexXmlCDataSectionTextToken() {
    TokenInfo info{};

    SyntaxListBuilder leading;
    LexXmlDocCommentLeadingTrivia(leading);

    Start();
    ScanXmlCDataSectionTextToken(info);
    std::vector<SyntaxDiagnosticInfo> errors = GetErrors();

    return Create(info, leading.ToList(), nullptr, errors);
}

bool Lexer::ScanXmlCDataSectionTextToken(TokenInfo &info) {
    char16_t ch;

    CS_ASSERT(!LocationIs(XmlDocCommentLocation::Start));
    CS_ASSERT(!LocationIs(XmlDocCommentLocation::Exterior));

    if (LocationIs(XmlDocCommentLocation::End)) {
        info.Kind = SyntaxKind::EndOfDocumentationCommentToken;
        return true;
    }

    ch = TextWindow.PeekChar();
    if (ch == u']' && TextWindow.PeekChar(1) == u']' && TextWindow.PeekChar(2) == u'>') {
        TextWindow.AdvanceChar(3);
        info.Kind = SyntaxKind::XmlCDataEndToken;
    } else if (ch == u'\r' || ch == u'\n') {
        ScanXmlTextLiteralNewLineToken(info);
    } else if (ch == SlidingTextWindow::InvalidCharacter && TextWindow.IsReallyAtEnd()) {
        info.Kind = SyntaxKind::EndOfDocumentationCommentToken;
    } else {
        // default:
        if (SyntaxFacts::IsNewLine(ch)) {
            ScanXmlTextLiteralNewLineToken(info);  // goto case '\n'
        } else {
            ScanXmlCDataSectionText(info);
            info.Kind = SyntaxKind::XmlTextLiteralToken;
        }
    }

    return true;
}

void Lexer::ScanXmlCDataSectionText(TokenInfo &info) {
    while (true) {
        char16_t ch = TextWindow.PeekChar();
        bool stop = false;
        if (ch == u']' && TextWindow.PeekChar(1) == u']' && TextWindow.PeekChar(2) == u'>') {
            stop = true;
        } else if (ch == u'\r' || ch == u'\n') {
            stop = true;
        } else if (ch == SlidingTextWindow::InvalidCharacter && TextWindow.IsReallyAtEnd()) {
            stop = true;
        } else if (ch == u'*' && StyleIs(XmlDocCommentStyle::Delimited) && TextWindow.PeekChar(1) == u'/') {
            // we're at the end of the comment, but don't lex it yet.
            stop = true;
        } else if (SyntaxFacts::IsNewLine(ch)) {
            stop = true;  // default: goto case '\n'
        }

        if (stop) {
            info.StringValue = info.Text_ = GetNonInternedLexemeText();
            info.HasStringValue = info.HasText = true;
            return;
        }

        TextWindow.AdvanceChar();
    }
}

/// Lexer entry point for LexMode.XmlCommentText
GreenNode *Lexer::LexXmlCommentTextToken() {
    TokenInfo info{};

    SyntaxListBuilder leading;
    LexXmlDocCommentLeadingTrivia(leading);

    Start();
    ScanXmlCommentTextToken(info);
    std::vector<SyntaxDiagnosticInfo> errors = GetErrors();

    return Create(info, leading.ToList(), nullptr, errors);
}

bool Lexer::ScanXmlCommentTextToken(TokenInfo &info) {
    char16_t ch;

    CS_ASSERT(!LocationIs(XmlDocCommentLocation::Start));
    CS_ASSERT(!LocationIs(XmlDocCommentLocation::Exterior));

    if (LocationIs(XmlDocCommentLocation::End)) {
        info.Kind = SyntaxKind::EndOfDocumentationCommentToken;
        return true;
    }

    ch = TextWindow.PeekChar();
    if (ch == u'-' && TextWindow.PeekChar(1) == u'-') {
        if (TextWindow.PeekChar(2) == u'>') {
            TextWindow.AdvanceChar(3);
            info.Kind = SyntaxKind::XmlCommentEndToken;
        } else {
            TextWindow.AdvanceChar(2);
            info.Kind = SyntaxKind::MinusMinusToken;
        }
    } else if (ch == u'\r' || ch == u'\n') {
        ScanXmlTextLiteralNewLineToken(info);
    } else if (ch == SlidingTextWindow::InvalidCharacter && TextWindow.IsReallyAtEnd()) {
        info.Kind = SyntaxKind::EndOfDocumentationCommentToken;
    } else {
        // default:
        if (SyntaxFacts::IsNewLine(ch)) {
            ScanXmlTextLiteralNewLineToken(info);  // goto case '\n'
        } else {
            ScanXmlCommentText(info);
            info.Kind = SyntaxKind::XmlTextLiteralToken;
        }
    }

    return true;
}

void Lexer::ScanXmlCommentText(TokenInfo &info) {
    while (true) {
        char16_t ch = TextWindow.PeekChar();
        bool stop = false;
        if (ch == u'-' && TextWindow.PeekChar(1) == u'-') {
            stop = true;
        } else if (ch == u'\r' || ch == u'\n') {
            stop = true;
        } else if (ch == SlidingTextWindow::InvalidCharacter && TextWindow.IsReallyAtEnd()) {
            stop = true;
        } else if (ch == u'*' && StyleIs(XmlDocCommentStyle::Delimited) && TextWindow.PeekChar(1) == u'/') {
            // we're at the end of the comment, but don't lex it yet.
            stop = true;
        } else if (SyntaxFacts::IsNewLine(ch)) {
            stop = true;  // default: goto case '\n'
        }

        if (stop) {
            info.StringValue = info.Text_ = GetNonInternedLexemeText();
            info.HasStringValue = info.HasText = true;
            return;
        }

        TextWindow.AdvanceChar();
    }
}

/// Lexer entry point for LexMode.XmlProcessingInstructionText
GreenNode *Lexer::LexXmlProcessingInstructionTextToken() {
    TokenInfo info{};

    SyntaxListBuilder leading;
    LexXmlDocCommentLeadingTrivia(leading);

    Start();
    ScanXmlProcessingInstructionTextToken(info);
    std::vector<SyntaxDiagnosticInfo> errors = GetErrors();

    return Create(info, leading.ToList(), nullptr, errors);
}

// CONSIDER: This could easily be merged with ScanXmlCDataSectionTextToken
bool Lexer::ScanXmlProcessingInstructionTextToken(TokenInfo &info) {
    char16_t ch;

    CS_ASSERT(!LocationIs(XmlDocCommentLocation::Start));
    CS_ASSERT(!LocationIs(XmlDocCommentLocation::Exterior));

    if (LocationIs(XmlDocCommentLocation::End)) {
        info.Kind = SyntaxKind::EndOfDocumentationCommentToken;
        return true;
    }

    ch = TextWindow.PeekChar();
    if (ch == u'?' && TextWindow.PeekChar(1) == u'>') {
        TextWindow.AdvanceChar(2);
        info.Kind = SyntaxKind::XmlProcessingInstructionEndToken;
    } else if (ch == u'\r' || ch == u'\n') {
        ScanXmlTextLiteralNewLineToken(info);
    } else if (ch == SlidingTextWindow::InvalidCharacter && TextWindow.IsReallyAtEnd()) {
        info.Kind = SyntaxKind::EndOfDocumentationCommentToken;
    } else {
        // default:
        if (SyntaxFacts::IsNewLine(ch)) {
            ScanXmlTextLiteralNewLineToken(info);  // goto case '\n'
        } else {
            ScanXmlProcessingInstructionText(info);
            info.Kind = SyntaxKind::XmlTextLiteralToken;
        }
    }

    return true;
}

// CONSIDER: This could easily be merged with ScanXmlCDataSectionText
void Lexer::ScanXmlProcessingInstructionText(TokenInfo &info) {
    while (true) {
        char16_t ch = TextWindow.PeekChar();
        bool stop = false;
        if (ch == u'?' && TextWindow.PeekChar(1) == u'>') {
            stop = true;
        } else if (ch == u'\r' || ch == u'\n') {
            stop = true;
        } else if (ch == SlidingTextWindow::InvalidCharacter && TextWindow.IsReallyAtEnd()) {
            stop = true;
        } else if (ch == u'*' && StyleIs(XmlDocCommentStyle::Delimited) && TextWindow.PeekChar(1) == u'/') {
            // we're at the end of the comment, but don't lex it yet.
            stop = true;
        } else if (SyntaxFacts::IsNewLine(ch)) {
            stop = true;  // default: goto case '\n'
        }

        if (stop) {
            info.StringValue = info.Text_ = GetNonInternedLexemeText();
            info.HasStringValue = info.HasText = true;
            return;
        }

        TextWindow.AdvanceChar();
    }
}

/// Collects XML doc comment exterior trivia, and therefore is a no op unless we are in the Start or Exterior of an
/// XML doc comment.
void Lexer::LexXmlDocCommentLeadingTrivia(SyntaxListBuilder &trivia) {
    int start = TextWindow.Position();
    Start();

    if (LocationIs(XmlDocCommentLocation::Start) && StyleIs(XmlDocCommentStyle::Delimited)) {
        // Read the /** that begins an XML doc comment. Since these are recognized only
        // when the trailing character is not a '*', we wind up in the interior of the
        // doc comment at the end.

        if (TextWindow.PeekChar() == u'/' && TextWindow.PeekChar(1) == u'*' && TextWindow.PeekChar(2) == u'*' &&
            TextWindow.PeekChar(3) != u'*') {
            TextWindow.AdvanceChar(3);
            Text text = GetInternedLexemeText();
            AddTrivia(SyntaxFactory::DocumentationCommentExteriorTrivia(text), trivia);
            MutateLocation(XmlDocCommentLocation::Interior);
            return;
        }
    } else if (LocationIs(XmlDocCommentLocation::Start) || LocationIs(XmlDocCommentLocation::Exterior)) {
        // We're in the exterior of an XML doc comment and need to eat the beginnings of
        // lines, for single line and delimited comments. We chew up white space until
        // a non-whitespace character, and then make the right decision depending on
        // what kind of comment we're in.

        while (true) {
            char16_t ch = TextWindow.PeekChar();
            if (ch == u' ' || ch == u'\t' || ch == u'\v' || ch == u'\f') {
                TextWindow.AdvanceChar();
                continue;
            }

            if (ch == u'/' && StyleIs(XmlDocCommentStyle::SingleLine) && TextWindow.PeekChar(1) == u'/' &&
                TextWindow.PeekChar(2) == u'/' && TextWindow.PeekChar(3) != u'/') {
                TextWindow.AdvanceChar(3);
                Text text = GetInternedLexemeText();
                AddTrivia(SyntaxFactory::DocumentationCommentExteriorTrivia(text), trivia);
                MutateLocation(XmlDocCommentLocation::Interior);
                return;
            }

            if (ch == u'*' && StyleIs(XmlDocCommentStyle::Delimited)) {
                while (TextWindow.PeekChar() == u'*' && TextWindow.PeekChar(1) != u'/') {
                    TextWindow.AdvanceChar();
                }

                Text text = GetInternedLexemeText();
                if (!text.empty()) {
                    AddTrivia(SyntaxFactory::DocumentationCommentExteriorTrivia(text), trivia);
                }

                // This setup ensures that on the final line of a comment, if we have
                // the string "  */", the "*/" part is separated from the whitespace
                // and therefore recognizable as the end of the comment.

                if (TextWindow.PeekChar() == u'*' && TextWindow.PeekChar(1) == u'/') {
                    TextWindow.AdvanceChar(2);
                    AddTrivia(SyntaxFactory::DocumentationCommentExteriorTrivia(u"*/"), trivia);
                    MutateLocation(XmlDocCommentLocation::End);
                } else {
                    MutateLocation(XmlDocCommentLocation::Interior);
                }

                return;
            }

            // default:
            if (SyntaxFacts::IsWhitespace(ch)) {
                TextWindow.AdvanceChar();  // goto case ' '
                continue;
            }

            // so here we have something else. if this is a single-line xml
            // doc comment, that means we're on a line that's no longer a doc
            // comment, so we need to rewind. if we're in a delimited doc comment,
            // then that means we hit pay dirt and we're back into xml text.

            if (StyleIs(XmlDocCommentStyle::SingleLine)) {
                TextWindow.Reset(start);
                MutateLocation(XmlDocCommentLocation::End);
            } else  // XmlDocCommentStyle.Delimited
            {
                CS_ASSERT(StyleIs(XmlDocCommentStyle::Delimited));

                Text text = GetInternedLexemeText();
                if (!text.empty()) AddTrivia(SyntaxFactory::DocumentationCommentExteriorTrivia(text), trivia);
                MutateLocation(XmlDocCommentLocation::Interior);
            }

            return;
        }
    } else if (!LocationIs(XmlDocCommentLocation::End) && StyleIs(XmlDocCommentStyle::Delimited)) {
        if (TextWindow.PeekChar() == u'*' && TextWindow.PeekChar(1) == u'/') {
            TextWindow.AdvanceChar(2);
            Text text = GetInternedLexemeText();
            AddTrivia(SyntaxFactory::DocumentationCommentExteriorTrivia(text), trivia);
            MutateLocation(XmlDocCommentLocation::End);
        }
    }
}

void Lexer::LexXmlDocCommentLeadingTriviaWithWhitespace(SyntaxListBuilder &trivia) {
    while (true) {
        LexXmlDocCommentLeadingTrivia(trivia);

        char16_t ch = TextWindow.PeekChar();
        if (LocationIs(XmlDocCommentLocation::Interior) && (SyntaxFacts::IsWhitespace(ch) || SyntaxFacts::IsNewLine(ch))) {
            LexXmlWhitespaceAndNewLineTrivia(trivia);
        } else {
            break;
        }
    }
}

/// Collects whitespace and new line trivia for XML doc comments. Does not see XML doc comment exterior trivia, and is
/// a no op unless we are in the interior.
void Lexer::LexXmlWhitespaceAndNewLineTrivia(SyntaxListBuilder &trivia) {
    Start();
    if (LocationIs(XmlDocCommentLocation::Interior)) {
        char16_t ch = TextWindow.PeekChar();
        bool isWhitespace = ch == u' ' || ch == u'\t' || ch == u'\v' || ch == u'\f';
        bool isNewLine = ch == u'\r' || ch == u'\n';
        if (!isWhitespace && !isNewLine) {
            if (ch == u'*' && StyleIs(XmlDocCommentStyle::Delimited) && TextWindow.PeekChar(1) == u'/') {
                // we're at the end of the comment, but don't add as trivia here.
                return;
            }

            // default:
            if (SyntaxFacts::IsWhitespace(ch)) {
                isWhitespace = true;  // goto case ' '
            } else if (SyntaxFacts::IsNewLine(ch)) {
                isNewLine = true;  // goto case '\n'
            } else {
                return;
            }
        }

        if (isWhitespace) {
            AddTrivia(ScanWhitespace(), trivia);
        } else {
            GreenNode *endOfLine = ScanEndOfLine();
            CS_ASSERT(endOfLine != nullptr);
            AddTrivia(endOfLine, trivia);
            MutateLocation(XmlDocCommentLocation::Exterior);
            return;
        }
    }
}

bool Lexer::IsUnicodeEscape() {
    if (TextWindow.PeekChar() == u'\\') {
        char16_t ch2 = TextWindow.PeekChar(1);
        if (ch2 == u'U' || ch2 == u'u') {
            return true;
        }
    }

    return false;
}

char16_t Lexer::PeekCharOrUnicodeEscape(char16_t &surrogateCharacter) {
    if (IsUnicodeEscape()) {
        return PeekUnicodeEscape(surrogateCharacter);
    } else {
        surrogateCharacter = SlidingTextWindow::InvalidCharacter;
        return TextWindow.PeekChar();
    }
}

char16_t Lexer::PeekUnicodeEscape(char16_t &surrogateCharacter) {
    int position = TextWindow.Position();

    // if we're peeking, then we don't want to change the position
    std::optional<SyntaxDiagnosticInfo> info;
    char16_t ch = ScanUnicodeEscape(/*peek:*/ true, surrogateCharacter, info);
    CS_ASSERT(!info);  // "Never produce a diagnostic while peeking."
    TextWindow.Reset(position);
    return ch;
}

char16_t Lexer::NextCharOrUnicodeEscape(char16_t &surrogateCharacter, std::optional<SyntaxDiagnosticInfo> &info) {
    char16_t ch = TextWindow.PeekChar();
    CS_ASSERT(ch != SlidingTextWindow::InvalidCharacter);
    if (ch == u'\\') {
        char16_t ch2 = TextWindow.PeekChar(1);
        if (ch2 == u'U' || ch2 == u'u') {
            return ScanUnicodeEscape(/*peek:*/ false, surrogateCharacter, info);
        }
    }

    surrogateCharacter = SlidingTextWindow::InvalidCharacter;
    info.reset();
    TextWindow.AdvanceChar();
    return ch;
}

char16_t Lexer::NextUnicodeEscape(char16_t &surrogateCharacter, std::optional<SyntaxDiagnosticInfo> &info) {
    return ScanUnicodeEscape(/*peek:*/ false, surrogateCharacter, info);
}

char16_t Lexer::ScanUnicodeEscape(bool peek, char16_t &surrogateCharacter, std::optional<SyntaxDiagnosticInfo> &info) {
    surrogateCharacter = SlidingTextWindow::InvalidCharacter;
    info.reset();

    int start = TextWindow.Position();
    char16_t character = TextWindow.PeekChar();
    CS_ASSERT(character == u'\\');
    TextWindow.AdvanceChar();

    character = TextWindow.PeekChar();
    if (character == u'U') {
        uint32_t uintChar = 0;

        TextWindow.AdvanceChar();
        if (!SyntaxFacts::IsHexDigit(TextWindow.PeekChar())) {
            if (!peek) {
                info = CreateIllegalEscapeDiagnostic(start);
            }
        } else {
            for (int i = 0; i < 8; i++) {
                character = TextWindow.PeekChar();
                if (!SyntaxFacts::IsHexDigit(character)) {
                    if (!peek) {
                        info = CreateIllegalEscapeDiagnostic(start);
                    }

                    break;
                }

                uintChar = (uintChar << 4) + static_cast<uint32_t>(SyntaxFacts::HexValue(character));
                TextWindow.AdvanceChar();
            }

            if (uintChar > 0x0010FFFF) {
                if (!peek) {
                    info = CreateIllegalEscapeDiagnostic(start);
                }
            } else {
                character = GetCharsFromUtf32(uintChar, surrogateCharacter);
            }
        }
    } else {
        CS_ASSERT(character == u'u' || character == u'x');

        int intChar = 0;
        TextWindow.AdvanceChar();
        if (!SyntaxFacts::IsHexDigit(TextWindow.PeekChar())) {
            if (!peek) {
                info = CreateIllegalEscapeDiagnostic(start);
            }
        } else {
            for (int i = 0; i < 4; i++) {
                char16_t ch2 = TextWindow.PeekChar();
                if (!SyntaxFacts::IsHexDigit(ch2)) {
                    if (character == u'u') {
                        if (!peek) {
                            info = CreateIllegalEscapeDiagnostic(start);
                        }
                    }

                    break;
                }

                intChar = (intChar << 4) + SyntaxFacts::HexValue(ch2);
                TextWindow.AdvanceChar();
            }

            character = static_cast<char16_t>(intChar);
        }
    }

    return character;
}

/// Given that the next character is an ampersand ('&amp;'), attempt to interpret the
/// following characters as an XML entity.  On success, populate the out parameters
/// with the low and high UTF-16 surrogates for the character represented by the
/// entity.
/// NOTE: Always advances, even on failure.
bool Lexer::TryScanXmlEntity(char16_t &ch, char16_t &surrogate) {
    CS_ASSERT(TextWindow.PeekChar() == u'&');

    ch = u'&';
    TextWindow.AdvanceChar();

    surrogate = SlidingTextWindow::InvalidCharacter;

    switch (TextWindow.PeekChar()) {
        case u'l':
            if (TextWindow.AdvanceIfMatches(u"lt;")) {
                ch = u'<';
                return true;
            }
            break;
        case u'g':
            if (TextWindow.AdvanceIfMatches(u"gt;")) {
                ch = u'>';
                return true;
            }
            break;
        case u'a':
            if (TextWindow.AdvanceIfMatches(u"amp;")) {
                ch = u'&';
                return true;
            } else if (TextWindow.AdvanceIfMatches(u"apos;")) {
                ch = u'\'';
                return true;
            }
            break;
        case u'q':
            if (TextWindow.AdvanceIfMatches(u"quot;")) {
                ch = u'"';
                return true;
            }
            break;
        case u'#': {
            TextWindow.AdvanceChar();  // #

            uint32_t uintChar = 0;

            if (TextWindow.AdvanceIfMatches(u"x")) {
                char16_t digit;
                while (SyntaxFacts::IsHexDigit(digit = TextWindow.PeekChar())) {
                    TextWindow.AdvanceChar();

                    // disallow overflow
                    if (uintChar <= 0x7FFFFFF) {
                        uintChar = (uintChar << 4) + static_cast<uint32_t>(SyntaxFacts::HexValue(digit));
                    } else {
                        return false;
                    }
                }
            } else {
                char16_t digit;
                while (SyntaxFacts::IsDecDigit(digit = TextWindow.PeekChar())) {
                    TextWindow.AdvanceChar();

                    // disallow overflow
                    if (uintChar <= 0x7FFFFFF) {
                        uintChar = (uintChar << 3) + (uintChar << 1) + static_cast<uint32_t>(SyntaxFacts::DecValue(digit));
                    } else {
                        return false;
                    }
                }
            }

            if (TextWindow.AdvanceIfMatches(u";")) {
                ch = GetCharsFromUtf32(uintChar, surrogate);
                return true;
            }

            break;
        }
        default:
            break;
    }

    return false;
}

SyntaxDiagnosticInfo Lexer::CreateIllegalEscapeDiagnostic(int start) const {
    SyntaxDiagnosticInfo d;
    d.offset = start - LexemeStartPosition;
    d.width = TextWindow.Position() - start;
    d.code = ErrorCode::ERR_IllegalEscape;
    return d;
}

char16_t Lexer::GetCharsFromUtf32(uint32_t codepoint, char16_t &lowSurrogate) {
    if (codepoint < 0x00010000u) {
        lowSurrogate = SlidingTextWindow::InvalidCharacter;
        return static_cast<char16_t>(codepoint);
    } else {
        // (unchecked: a codepoint above 0x10FFFF -- possible from TryScanXmlEntity -- truncates as the C# casts do)
        lowSurrogate = static_cast<char16_t>((codepoint - 0x00010000u) % 0x0400u + 0xDC00u);
        return static_cast<char16_t>((codepoint - 0x00010000u) / 0x0400u + 0xD800u);
    }
}

}  // namespace csharp
