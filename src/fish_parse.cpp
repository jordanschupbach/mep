// fish-script reader: text, unescaping, tokenizer and tree. See fish_parse.h.
#include "fish_parse.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>

namespace fishparse {

// --- Text ---------------------------------------------------------------------------------

namespace {

constexpr char32_t kEncodeDirectBase = 0xF600;  // an undecodable byte b reads as kEncodeDirectBase + b

void DecodeUtf8(std::string_view s, std::u32string *out, std::vector<size_t> *bytes) {
    size_t i = 0;
    while (i < s.size()) {
        const auto b0 = static_cast<unsigned char>(s[i]);
        size_t n = 0;
        char32_t cp = 0;
        if (b0 < 0x80) {
            n = 1;
            cp = b0;
        } else if ((b0 & 0xE0) == 0xC0) {
            n = 2;
            cp = b0 & 0x1Fu;
        } else if ((b0 & 0xF0) == 0xE0) {
            n = 3;
            cp = b0 & 0x0Fu;
        } else if ((b0 & 0xF8) == 0xF0) {
            n = 4;
            cp = b0 & 0x07u;
        }
        bool ok = n > 0 && i + n <= s.size();
        for (size_t k = 1; ok && k < n; k++) {
            const auto b = static_cast<unsigned char>(s[i + k]);
            if ((b & 0xC0) != 0x80) ok = false;
            cp = (cp << 6) | (b & 0x3Fu);
        }
        if (ok) {
            if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
                (cp >= 0xD800 && cp <= 0xDFFF))
                ok = false;
        }
        if (!ok) {
            n = 1;
            cp = kEncodeDirectBase + b0;
        }
        if (bytes) bytes->push_back(i);
        out->push_back(cp);
        i += n;
    }
    if (bytes) bytes->push_back(s.size());
}

}  // namespace

Source Source::FromUtf8(std::string_view utf8) {
    Source s;
    DecodeUtf8(utf8, &s.text, &s.byte_of);
    return s;
}

std::string ToUtf8(std::u32string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char32_t c : s) {
        if (c >= kEncodeDirectBase && c < kEncodeDirectBase + 0x100 && c >= kEncodeDirectBase + 0x80) {
            out.push_back(static_cast<char>(c - kEncodeDirectBase));
        } else if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

bool IsWhitespace(char32_t c) {
    switch (c) {
    case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x20: case 0x85: case 0xA0: case 0x1680:
    case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000:
        return true;
    default:
        return c >= 0x2000 && c <= 0x200A;
    }
}

namespace {

bool IsReserved(char32_t c) { return (c >= 0xFDD0 && c < 0xFDF0) || (c >= kEncodeDirectBase && c < kEncodeDirectBase + 0x100); }

bool IsPua(char32_t c) {
    return (c >= 0xE000 && c <= 0xF8FF) || (c >= 0xF0000 && c <= 0xFFFFD) || (c >= 0x100000 && c <= 0x10FFFD);
}

// Unicode letters and digits: the Alphabetic and Numeric properties (what
// fish's character tests use), as ranges above ASCII. Generated with Rust's
// char::is_alphanumeric (Unicode 16).
struct CpRange {
    char32_t lo, hi;
};
const CpRange kAlnumRanges[] = {
    {0xAA,0xAA},{0xB2,0xB3},{0xB5,0xB5},{0xB9,0xBA},{0xBC,0xBE},{0xC0,0xD6},
    {0xD8,0xF6},{0xF8,0x2C1},{0x2C6,0x2D1},{0x2E0,0x2E4},{0x2EC,0x2EC},{0x2EE,0x2EE},
    {0x345,0x345},{0x363,0x374},{0x376,0x377},{0x37A,0x37D},{0x37F,0x37F},{0x386,0x386},
    {0x388,0x38A},{0x38C,0x38C},{0x38E,0x3A1},{0x3A3,0x3F5},{0x3F7,0x481},{0x48A,0x52F},
    {0x531,0x556},{0x559,0x559},{0x560,0x588},{0x5B0,0x5BD},{0x5BF,0x5BF},{0x5C1,0x5C2},
    {0x5C4,0x5C5},{0x5C7,0x5C7},{0x5D0,0x5EA},{0x5EF,0x5F2},{0x610,0x61A},{0x620,0x657},
    {0x659,0x669},{0x66E,0x6D3},{0x6D5,0x6DC},{0x6E1,0x6E8},{0x6ED,0x6FC},{0x6FF,0x6FF},
    {0x710,0x73F},{0x74D,0x7B1},{0x7C0,0x7EA},{0x7F4,0x7F5},{0x7FA,0x7FA},{0x800,0x817},
    {0x81A,0x82C},{0x840,0x858},{0x860,0x86A},{0x870,0x887},{0x889,0x88F},{0x897,0x897},
    {0x8A0,0x8C9},{0x8D4,0x8DF},{0x8E3,0x8E9},{0x8F0,0x93B},{0x93D,0x94C},{0x94E,0x950},
    {0x955,0x963},{0x966,0x96F},{0x971,0x983},{0x985,0x98C},{0x98F,0x990},{0x993,0x9A8},
    {0x9AA,0x9B0},{0x9B2,0x9B2},{0x9B6,0x9B9},{0x9BD,0x9C4},{0x9C7,0x9C8},{0x9CB,0x9CC},
    {0x9CE,0x9CE},{0x9D7,0x9D7},{0x9DC,0x9DD},{0x9DF,0x9E3},{0x9E6,0x9F1},{0x9F4,0x9F9},
    {0x9FC,0x9FC},{0xA01,0xA03},{0xA05,0xA0A},{0xA0F,0xA10},{0xA13,0xA28},{0xA2A,0xA30},
    {0xA32,0xA33},{0xA35,0xA36},{0xA38,0xA39},{0xA3E,0xA42},{0xA47,0xA48},{0xA4B,0xA4C},
    {0xA51,0xA51},{0xA59,0xA5C},{0xA5E,0xA5E},{0xA66,0xA75},{0xA81,0xA83},{0xA85,0xA8D},
    {0xA8F,0xA91},{0xA93,0xAA8},{0xAAA,0xAB0},{0xAB2,0xAB3},{0xAB5,0xAB9},{0xABD,0xAC5},
    {0xAC7,0xAC9},{0xACB,0xACC},{0xAD0,0xAD0},{0xAE0,0xAE3},{0xAE6,0xAEF},{0xAF9,0xAFC},
    {0xB01,0xB03},{0xB05,0xB0C},{0xB0F,0xB10},{0xB13,0xB28},{0xB2A,0xB30},{0xB32,0xB33},
    {0xB35,0xB39},{0xB3D,0xB44},{0xB47,0xB48},{0xB4B,0xB4C},{0xB56,0xB57},{0xB5C,0xB5D},
    {0xB5F,0xB63},{0xB66,0xB6F},{0xB71,0xB77},{0xB82,0xB83},{0xB85,0xB8A},{0xB8E,0xB90},
    {0xB92,0xB95},{0xB99,0xB9A},{0xB9C,0xB9C},{0xB9E,0xB9F},{0xBA3,0xBA4},{0xBA8,0xBAA},
    {0xBAE,0xBB9},{0xBBE,0xBC2},{0xBC6,0xBC8},{0xBCA,0xBCC},{0xBD0,0xBD0},{0xBD7,0xBD7},
    {0xBE6,0xBF2},{0xC00,0xC0C},{0xC0E,0xC10},{0xC12,0xC28},{0xC2A,0xC39},{0xC3D,0xC44},
    {0xC46,0xC48},{0xC4A,0xC4C},{0xC55,0xC56},{0xC58,0xC5A},{0xC5C,0xC5D},{0xC60,0xC63},
    {0xC66,0xC6F},{0xC78,0xC7E},{0xC80,0xC83},{0xC85,0xC8C},{0xC8E,0xC90},{0xC92,0xCA8},
    {0xCAA,0xCB3},{0xCB5,0xCB9},{0xCBD,0xCC4},{0xCC6,0xCC8},{0xCCA,0xCCC},{0xCD5,0xCD6},
    {0xCDC,0xCDE},{0xCE0,0xCE3},{0xCE6,0xCEF},{0xCF1,0xCF3},{0xD00,0xD0C},{0xD0E,0xD10},
    {0xD12,0xD3A},{0xD3D,0xD44},{0xD46,0xD48},{0xD4A,0xD4C},{0xD4E,0xD4E},{0xD54,0xD63},
    {0xD66,0xD78},{0xD7A,0xD7F},{0xD81,0xD83},{0xD85,0xD96},{0xD9A,0xDB1},{0xDB3,0xDBB},
    {0xDBD,0xDBD},{0xDC0,0xDC6},{0xDCF,0xDD4},{0xDD6,0xDD6},{0xDD8,0xDDF},{0xDE6,0xDEF},
    {0xDF2,0xDF3},{0xE01,0xE3A},{0xE40,0xE46},{0xE4D,0xE4D},{0xE50,0xE59},{0xE81,0xE82},
    {0xE84,0xE84},{0xE86,0xE8A},{0xE8C,0xEA3},{0xEA5,0xEA5},{0xEA7,0xEB9},{0xEBB,0xEBD},
    {0xEC0,0xEC4},{0xEC6,0xEC6},{0xECD,0xECD},{0xED0,0xED9},{0xEDC,0xEDF},{0xF00,0xF00},
    {0xF20,0xF33},{0xF40,0xF47},{0xF49,0xF6C},{0xF71,0xF83},{0xF88,0xF97},{0xF99,0xFBC},
    {0x1000,0x1036},{0x1038,0x1038},{0x103B,0x1049},{0x1050,0x109D},{0x10A0,0x10C5},{0x10C7,0x10C7},
    {0x10CD,0x10CD},{0x10D0,0x10FA},{0x10FC,0x1248},{0x124A,0x124D},{0x1250,0x1256},{0x1258,0x1258},
    {0x125A,0x125D},{0x1260,0x1288},{0x128A,0x128D},{0x1290,0x12B0},{0x12B2,0x12B5},{0x12B8,0x12BE},
    {0x12C0,0x12C0},{0x12C2,0x12C5},{0x12C8,0x12D6},{0x12D8,0x1310},{0x1312,0x1315},{0x1318,0x135A},
    {0x1369,0x137C},{0x1380,0x138F},{0x13A0,0x13F5},{0x13F8,0x13FD},{0x1401,0x166C},{0x166F,0x167F},
    {0x1681,0x169A},{0x16A0,0x16EA},{0x16EE,0x16F8},{0x1700,0x1713},{0x171F,0x1733},{0x1740,0x1753},
    {0x1760,0x176C},{0x176E,0x1770},{0x1772,0x1773},{0x1780,0x17B3},{0x17B6,0x17C8},{0x17D7,0x17D7},
    {0x17DC,0x17DC},{0x17E0,0x17E9},{0x17F0,0x17F9},{0x1810,0x1819},{0x1820,0x1878},{0x1880,0x18AA},
    {0x18B0,0x18F5},{0x1900,0x191E},{0x1920,0x192B},{0x1930,0x1938},{0x1946,0x196D},{0x1970,0x1974},
    {0x1980,0x19AB},{0x19B0,0x19C9},{0x19D0,0x19DA},{0x1A00,0x1A1B},{0x1A20,0x1A5E},{0x1A61,0x1A74},
    {0x1A80,0x1A89},{0x1A90,0x1A99},{0x1AA7,0x1AA7},{0x1ABF,0x1AC0},{0x1ACC,0x1ACE},{0x1B00,0x1B33},
    {0x1B35,0x1B43},{0x1B45,0x1B4C},{0x1B50,0x1B59},{0x1B80,0x1BA9},{0x1BAC,0x1BE5},{0x1BE7,0x1BF1},
    {0x1C00,0x1C36},{0x1C40,0x1C49},{0x1C4D,0x1C7D},{0x1C80,0x1C8A},{0x1C90,0x1CBA},{0x1CBD,0x1CBF},
    {0x1CE9,0x1CEC},{0x1CEE,0x1CF3},{0x1CF5,0x1CF6},{0x1CFA,0x1CFA},{0x1D00,0x1DBF},{0x1DD3,0x1DF4},
    {0x1E00,0x1F15},{0x1F18,0x1F1D},{0x1F20,0x1F45},{0x1F48,0x1F4D},{0x1F50,0x1F57},{0x1F59,0x1F59},
    {0x1F5B,0x1F5B},{0x1F5D,0x1F5D},{0x1F5F,0x1F7D},{0x1F80,0x1FB4},{0x1FB6,0x1FBC},{0x1FBE,0x1FBE},
    {0x1FC2,0x1FC4},{0x1FC6,0x1FCC},{0x1FD0,0x1FD3},{0x1FD6,0x1FDB},{0x1FE0,0x1FEC},{0x1FF2,0x1FF4},
    {0x1FF6,0x1FFC},{0x2070,0x2071},{0x2074,0x2079},{0x207F,0x2089},{0x2090,0x209C},{0x2102,0x2102},
    {0x2107,0x2107},{0x210A,0x2113},{0x2115,0x2115},{0x2119,0x211D},{0x2124,0x2124},{0x2126,0x2126},
    {0x2128,0x2128},{0x212A,0x212D},{0x212F,0x2139},{0x213C,0x213F},{0x2145,0x2149},{0x214E,0x214E},
    {0x2150,0x2189},{0x2460,0x249B},{0x24B6,0x24FF},{0x2776,0x2793},{0x2C00,0x2CE4},{0x2CEB,0x2CEE},
    {0x2CF2,0x2CF3},{0x2CFD,0x2CFD},{0x2D00,0x2D25},{0x2D27,0x2D27},{0x2D2D,0x2D2D},{0x2D30,0x2D67},
    {0x2D6F,0x2D6F},{0x2D80,0x2D96},{0x2DA0,0x2DA6},{0x2DA8,0x2DAE},{0x2DB0,0x2DB6},{0x2DB8,0x2DBE},
    {0x2DC0,0x2DC6},{0x2DC8,0x2DCE},{0x2DD0,0x2DD6},{0x2DD8,0x2DDE},{0x2DE0,0x2DFF},{0x2E2F,0x2E2F},
    {0x3005,0x3007},{0x3021,0x3029},{0x3031,0x3035},{0x3038,0x303C},{0x3041,0x3096},{0x309D,0x309F},
    {0x30A1,0x30FA},{0x30FC,0x30FF},{0x3105,0x312F},{0x3131,0x318E},{0x3192,0x3195},{0x31A0,0x31BF},
    {0x31F0,0x31FF},{0x3220,0x3229},{0x3248,0x324F},{0x3251,0x325F},{0x3280,0x3289},{0x32B1,0x32BF},
    {0x3400,0x4DBF},{0x4E00,0xA48C},{0xA4D0,0xA4FD},{0xA500,0xA60C},{0xA610,0xA62B},{0xA640,0xA66E},
    {0xA674,0xA67B},{0xA67F,0xA6EF},{0xA717,0xA71F},{0xA722,0xA788},{0xA78B,0xA7DC},{0xA7F1,0xA805},
    {0xA807,0xA827},{0xA830,0xA835},{0xA840,0xA873},{0xA880,0xA8C3},{0xA8C5,0xA8C5},{0xA8D0,0xA8D9},
    {0xA8F2,0xA8F7},{0xA8FB,0xA8FB},{0xA8FD,0xA92A},{0xA930,0xA952},{0xA960,0xA97C},{0xA980,0xA9B2},
    {0xA9B4,0xA9BF},{0xA9CF,0xA9D9},{0xA9E0,0xA9FE},{0xAA00,0xAA36},{0xAA40,0xAA4D},{0xAA50,0xAA59},
    {0xAA60,0xAA76},{0xAA7A,0xAABE},{0xAAC0,0xAAC0},{0xAAC2,0xAAC2},{0xAADB,0xAADD},{0xAAE0,0xAAEF},
    {0xAAF2,0xAAF5},{0xAB01,0xAB06},{0xAB09,0xAB0E},{0xAB11,0xAB16},{0xAB20,0xAB26},{0xAB28,0xAB2E},
    {0xAB30,0xAB5A},{0xAB5C,0xAB69},{0xAB70,0xABEA},{0xABF0,0xABF9},{0xAC00,0xD7A3},{0xD7B0,0xD7C6},
    {0xD7CB,0xD7FB},{0xF900,0xFA6D},{0xFA70,0xFAD9},{0xFB00,0xFB06},{0xFB13,0xFB17},{0xFB1D,0xFB28},
    {0xFB2A,0xFB36},{0xFB38,0xFB3C},{0xFB3E,0xFB3E},{0xFB40,0xFB41},{0xFB43,0xFB44},{0xFB46,0xFBB1},
    {0xFBD3,0xFD3D},{0xFD50,0xFD8F},{0xFD92,0xFDC7},{0xFDF0,0xFDFB},{0xFE70,0xFE74},{0xFE76,0xFEFC},
    {0xFF10,0xFF19},{0xFF21,0xFF3A},{0xFF41,0xFF5A},{0xFF66,0xFFBE},{0xFFC2,0xFFC7},{0xFFCA,0xFFCF},
    {0xFFD2,0xFFD7},{0xFFDA,0xFFDC},{0x10000,0x1000B},{0x1000D,0x10026},{0x10028,0x1003A},{0x1003C,0x1003D},
    {0x1003F,0x1004D},{0x10050,0x1005D},{0x10080,0x100FA},{0x10107,0x10133},{0x10140,0x10178},{0x1018A,0x1018B},
    {0x10280,0x1029C},{0x102A0,0x102D0},{0x102E1,0x102FB},{0x10300,0x10323},{0x1032D,0x1034A},{0x10350,0x1037A},
    {0x10380,0x1039D},{0x103A0,0x103C3},{0x103C8,0x103CF},{0x103D1,0x103D5},{0x10400,0x1049D},{0x104A0,0x104A9},
    {0x104B0,0x104D3},{0x104D8,0x104FB},{0x10500,0x10527},{0x10530,0x10563},{0x10570,0x1057A},{0x1057C,0x1058A},
    {0x1058C,0x10592},{0x10594,0x10595},{0x10597,0x105A1},{0x105A3,0x105B1},{0x105B3,0x105B9},{0x105BB,0x105BC},
    {0x105C0,0x105F3},{0x10600,0x10736},{0x10740,0x10755},{0x10760,0x10767},{0x10780,0x10785},{0x10787,0x107B0},
    {0x107B2,0x107BA},{0x10800,0x10805},{0x10808,0x10808},{0x1080A,0x10835},{0x10837,0x10838},{0x1083C,0x1083C},
    {0x1083F,0x10855},{0x10858,0x10876},{0x10879,0x1089E},{0x108A7,0x108AF},{0x108E0,0x108F2},{0x108F4,0x108F5},
    {0x108FB,0x1091B},{0x10920,0x10939},{0x10940,0x10959},{0x10980,0x109B7},{0x109BC,0x109CF},{0x109D2,0x10A03},
    {0x10A05,0x10A06},{0x10A0C,0x10A13},{0x10A15,0x10A17},{0x10A19,0x10A35},{0x10A40,0x10A48},{0x10A60,0x10A7E},
    {0x10A80,0x10A9F},{0x10AC0,0x10AC7},{0x10AC9,0x10AE4},{0x10AEB,0x10AEF},{0x10B00,0x10B35},{0x10B40,0x10B55},
    {0x10B58,0x10B72},{0x10B78,0x10B91},{0x10BA9,0x10BAF},{0x10C00,0x10C48},{0x10C80,0x10CB2},{0x10CC0,0x10CF2},
    {0x10CFA,0x10D27},{0x10D30,0x10D39},{0x10D40,0x10D65},{0x10D69,0x10D69},{0x10D6F,0x10D85},{0x10E60,0x10E7E},
    {0x10E80,0x10EA9},{0x10EAB,0x10EAC},{0x10EB0,0x10EB1},{0x10EC2,0x10EC7},{0x10EFA,0x10EFC},{0x10F00,0x10F27},
    {0x10F30,0x10F45},{0x10F51,0x10F54},{0x10F70,0x10F81},{0x10FB0,0x10FCB},{0x10FE0,0x10FF6},{0x11000,0x11045},
    {0x11052,0x1106F},{0x11071,0x11075},{0x11080,0x110B8},{0x110C2,0x110C2},{0x110D0,0x110E8},{0x110F0,0x110F9},
    {0x11100,0x11132},{0x11136,0x1113F},{0x11144,0x11147},{0x11150,0x11172},{0x11176,0x11176},{0x11180,0x111BF},
    {0x111C1,0x111C4},{0x111CE,0x111DA},{0x111DC,0x111DC},{0x111E1,0x111F4},{0x11200,0x11211},{0x11213,0x11234},
    {0x11237,0x11237},{0x1123E,0x11241},{0x11280,0x11286},{0x11288,0x11288},{0x1128A,0x1128D},{0x1128F,0x1129D},
    {0x1129F,0x112A8},{0x112B0,0x112E8},{0x112F0,0x112F9},{0x11300,0x11303},{0x11305,0x1130C},{0x1130F,0x11310},
    {0x11313,0x11328},{0x1132A,0x11330},{0x11332,0x11333},{0x11335,0x11339},{0x1133D,0x11344},{0x11347,0x11348},
    {0x1134B,0x1134C},{0x11350,0x11350},{0x11357,0x11357},{0x1135D,0x11363},{0x11380,0x11389},{0x1138B,0x1138B},
    {0x1138E,0x1138E},{0x11390,0x113B5},{0x113B7,0x113C0},{0x113C2,0x113C2},{0x113C5,0x113C5},{0x113C7,0x113CA},
    {0x113CC,0x113CD},{0x113D1,0x113D1},{0x113D3,0x113D3},{0x11400,0x11441},{0x11443,0x11445},{0x11447,0x1144A},
    {0x11450,0x11459},{0x1145F,0x11461},{0x11480,0x114C1},{0x114C4,0x114C5},{0x114C7,0x114C7},{0x114D0,0x114D9},
    {0x11580,0x115B5},{0x115B8,0x115BE},{0x115D8,0x115DD},{0x11600,0x1163E},{0x11640,0x11640},{0x11644,0x11644},
    {0x11650,0x11659},{0x11680,0x116B5},{0x116B8,0x116B8},{0x116C0,0x116C9},{0x116D0,0x116E3},{0x11700,0x1171A},
    {0x1171D,0x1172A},{0x11730,0x1173B},{0x11740,0x11746},{0x11800,0x11838},{0x118A0,0x118F2},{0x118FF,0x11906},
    {0x11909,0x11909},{0x1190C,0x11913},{0x11915,0x11916},{0x11918,0x11935},{0x11937,0x11938},{0x1193B,0x1193C},
    {0x1193F,0x11942},{0x11950,0x11959},{0x119A0,0x119A7},{0x119AA,0x119D7},{0x119DA,0x119DF},{0x119E1,0x119E1},
    {0x119E3,0x119E4},{0x11A00,0x11A32},{0x11A35,0x11A3E},{0x11A50,0x11A97},{0x11A9D,0x11A9D},{0x11AB0,0x11AF8},
    {0x11B60,0x11B67},{0x11BC0,0x11BE0},{0x11BF0,0x11BF9},{0x11C00,0x11C08},{0x11C0A,0x11C36},{0x11C38,0x11C3E},
    {0x11C40,0x11C40},{0x11C50,0x11C6C},{0x11C72,0x11C8F},{0x11C92,0x11CA7},{0x11CA9,0x11CB6},{0x11D00,0x11D06},
    {0x11D08,0x11D09},{0x11D0B,0x11D36},{0x11D3A,0x11D3A},{0x11D3C,0x11D3D},{0x11D3F,0x11D41},{0x11D43,0x11D43},
    {0x11D46,0x11D47},{0x11D50,0x11D59},{0x11D60,0x11D65},{0x11D67,0x11D68},{0x11D6A,0x11D8E},{0x11D90,0x11D91},
    {0x11D93,0x11D96},{0x11D98,0x11D98},{0x11DA0,0x11DA9},{0x11DB0,0x11DDB},{0x11DE0,0x11DE9},{0x11EE0,0x11EF6},
    {0x11F00,0x11F10},{0x11F12,0x11F3A},{0x11F3E,0x11F40},{0x11F50,0x11F59},{0x11FB0,0x11FB0},{0x11FC0,0x11FD4},
    {0x12000,0x12399},{0x12400,0x1246E},{0x12480,0x12543},{0x12F90,0x12FF0},{0x13000,0x1342F},{0x13441,0x13446},
    {0x13460,0x143FA},{0x14400,0x14646},{0x16100,0x1612E},{0x16130,0x16139},{0x16800,0x16A38},{0x16A40,0x16A5E},
    {0x16A60,0x16A69},{0x16A70,0x16ABE},{0x16AC0,0x16AC9},{0x16AD0,0x16AED},{0x16B00,0x16B2F},{0x16B40,0x16B43},
    {0x16B50,0x16B59},{0x16B5B,0x16B61},{0x16B63,0x16B77},{0x16B7D,0x16B8F},{0x16D40,0x16D6C},{0x16D70,0x16D79},
    {0x16E40,0x16E96},{0x16EA0,0x16EB8},{0x16EBB,0x16ED3},{0x16F00,0x16F4A},{0x16F4F,0x16F87},{0x16F8F,0x16F9F},
    {0x16FE0,0x16FE1},{0x16FE3,0x16FE3},{0x16FF0,0x16FF6},{0x17000,0x18CD5},{0x18CFF,0x18D1E},{0x18D80,0x18DF2},
    {0x1AFF0,0x1AFF3},{0x1AFF5,0x1AFFB},{0x1AFFD,0x1AFFE},{0x1B000,0x1B122},{0x1B132,0x1B132},{0x1B150,0x1B152},
    {0x1B155,0x1B155},{0x1B164,0x1B167},{0x1B170,0x1B2FB},{0x1BC00,0x1BC6A},{0x1BC70,0x1BC7C},{0x1BC80,0x1BC88},
    {0x1BC90,0x1BC99},{0x1BC9E,0x1BC9E},{0x1CCF0,0x1CCF9},{0x1D2C0,0x1D2D3},{0x1D2E0,0x1D2F3},{0x1D360,0x1D378},
    {0x1D400,0x1D454},{0x1D456,0x1D49C},{0x1D49E,0x1D49F},{0x1D4A2,0x1D4A2},{0x1D4A5,0x1D4A6},{0x1D4A9,0x1D4AC},
    {0x1D4AE,0x1D4B9},{0x1D4BB,0x1D4BB},{0x1D4BD,0x1D4C3},{0x1D4C5,0x1D505},{0x1D507,0x1D50A},{0x1D50D,0x1D514},
    {0x1D516,0x1D51C},{0x1D51E,0x1D539},{0x1D53B,0x1D53E},{0x1D540,0x1D544},{0x1D546,0x1D546},{0x1D54A,0x1D550},
    {0x1D552,0x1D6A5},{0x1D6A8,0x1D6C0},{0x1D6C2,0x1D6DA},{0x1D6DC,0x1D6FA},{0x1D6FC,0x1D714},{0x1D716,0x1D734},
    {0x1D736,0x1D74E},{0x1D750,0x1D76E},{0x1D770,0x1D788},{0x1D78A,0x1D7A8},{0x1D7AA,0x1D7C2},{0x1D7C4,0x1D7CB},
    {0x1D7CE,0x1D7FF},{0x1DF00,0x1DF1E},{0x1DF25,0x1DF2A},{0x1E000,0x1E006},{0x1E008,0x1E018},{0x1E01B,0x1E021},
    {0x1E023,0x1E024},{0x1E026,0x1E02A},{0x1E030,0x1E06D},{0x1E08F,0x1E08F},{0x1E100,0x1E12C},{0x1E137,0x1E13D},
    {0x1E140,0x1E149},{0x1E14E,0x1E14E},{0x1E290,0x1E2AD},{0x1E2C0,0x1E2EB},{0x1E2F0,0x1E2F9},{0x1E4D0,0x1E4EB},
    {0x1E4F0,0x1E4F9},{0x1E5D0,0x1E5ED},{0x1E5F0,0x1E5FA},{0x1E6C0,0x1E6DE},{0x1E6E0,0x1E6F5},{0x1E6FE,0x1E6FF},
    {0x1E7E0,0x1E7E6},{0x1E7E8,0x1E7EB},{0x1E7ED,0x1E7EE},{0x1E7F0,0x1E7FE},{0x1E800,0x1E8C4},{0x1E8C7,0x1E8CF},
    {0x1E900,0x1E943},{0x1E947,0x1E947},{0x1E94B,0x1E94B},{0x1E950,0x1E959},{0x1EC71,0x1ECAB},{0x1ECAD,0x1ECAF},
    {0x1ECB1,0x1ECB4},{0x1ED01,0x1ED2D},{0x1ED2F,0x1ED3D},{0x1EE00,0x1EE03},{0x1EE05,0x1EE1F},{0x1EE21,0x1EE22},
    {0x1EE24,0x1EE24},{0x1EE27,0x1EE27},{0x1EE29,0x1EE32},{0x1EE34,0x1EE37},{0x1EE39,0x1EE39},{0x1EE3B,0x1EE3B},
    {0x1EE42,0x1EE42},{0x1EE47,0x1EE47},{0x1EE49,0x1EE49},{0x1EE4B,0x1EE4B},{0x1EE4D,0x1EE4F},{0x1EE51,0x1EE52},
    {0x1EE54,0x1EE54},{0x1EE57,0x1EE57},{0x1EE59,0x1EE59},{0x1EE5B,0x1EE5B},{0x1EE5D,0x1EE5D},{0x1EE5F,0x1EE5F},
    {0x1EE61,0x1EE62},{0x1EE64,0x1EE64},{0x1EE67,0x1EE6A},{0x1EE6C,0x1EE72},{0x1EE74,0x1EE77},{0x1EE79,0x1EE7C},
    {0x1EE7E,0x1EE7E},{0x1EE80,0x1EE89},{0x1EE8B,0x1EE9B},{0x1EEA1,0x1EEA3},{0x1EEA5,0x1EEA9},{0x1EEAB,0x1EEBB},
    {0x1F100,0x1F10C},{0x1F130,0x1F149},{0x1F150,0x1F169},{0x1F170,0x1F189},{0x1FBF0,0x1FBF9},{0x20000,0x2A6DF},
    {0x2A700,0x2B81D},{0x2B820,0x2CEAD},{0x2CEB0,0x2EBE0},{0x2EBF0,0x2EE5D},{0x2F800,0x2FA1D},{0x30000,0x3134A},
    {0x31350,0x33479},
};

bool UnicodeAlnum(char32_t c) {
    if (c < 0x80) return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    const auto *it = std::upper_bound(std::begin(kAlnumRanges), std::end(kAlnumRanges), c,
                                      [](char32_t v, const CpRange &r) { return v < r.lo; });
    return it != std::begin(kAlnumRanges) && c <= (it - 1)->hi;
}

}  // namespace

bool ValidVarNameChar(char32_t c) { return c == '_' || (!IsReserved(c) && !IsPua(c) && UnicodeAlnum(c)); }

bool ValidVarName(std::u32string_view s) {
    if (s.empty()) return false;
    for (char32_t c : s)
        if (!ValidVarNameChar(c)) return false;
    return true;
}

// --- Unescaping -------------------------------------------------------------------------

namespace {

int DigitIn(char32_t c, int base) {
    int d = -1;
    if (c >= '0' && c <= '9') d = static_cast<int>(c - '0');
    else if (c >= 'a' && c <= 'z') d = static_cast<int>(c - 'a') + 10;
    else if (c >= 'A' && c <= 'Z') d = static_cast<int>(c - 'A') + 10;
    return d >= 0 && d < base ? d : -1;
}

// Reads a backslash escape outside quotes; returns the characters consumed, 0 on error.
size_t ReadUnquotedEscape(std::u32string_view in, std::u32string *result, bool allow_incomplete, bool special) {
    auto at = [&](size_t i) { return i < in.size() ? in[i] : U'\0'; };
    bool have_char = false;
    char32_t result_char = 0;
    bool errored = false;
    size_t in_pos = 1;
    std::string byte_buff;
    for (;;) {
        const char32_t c = at(in_pos);
        in_pos++;
        if (c == '\0') {
            in_pos--;
            if (!allow_incomplete) errored = true;
        } else if ((c >= '0' && c <= '7') || c == 'u' || c == 'U' || c == 'x' || c == 'X') {
            uint64_t res = 0;
            int chars = 2, base = 16;
            bool byte_literal = false;
            uint64_t max_val = 0x7F;
            if (c == 'u') {
                chars = 4;
                max_val = 0xFFFF;
            } else if (c == 'U') {
                chars = 8;
                max_val = 0x10FFFF;
            } else if (c == 'x' || c == 'X') {
                byte_literal = true;
                max_val = 0xFF;
            } else {
                base = 8;
                chars = 3;
                in_pos--;
            }
            for (int i = 0; i < chars; i++) {
                const int d = DigitIn(at(in_pos), base);
                if (d < 0) {
                    if (i == 0) errored = true;
                    break;
                }
                res = res * static_cast<uint64_t>(base) + static_cast<uint64_t>(d);
                in_pos++;
            }
            if (!errored && res <= max_val) {
                if (byte_literal) {
                    byte_buff.push_back(static_cast<char>(res));
                    have_char = false;
                    if (at(in_pos) == '\\' && (at(in_pos + 1) == 'x' || at(in_pos + 1) == 'X')) {
                        in_pos++;
                        continue;
                    }
                } else {
                    char32_t ch = static_cast<char32_t>(res);
                    if (ch >= 0xD800 && ch <= 0xDFFF) ch = 0xFFFD;
                    have_char = true;
                    result_char = ch;
                }
            } else {
                errored = true;
            }
        } else if (c == 'a') {
            have_char = true, result_char = 0x07;
        } else if (c == 'b') {
            have_char = true, result_char = 0x08;
        } else if (c == 'c') {
            const char32_t s = at(in_pos);
            in_pos++;
            if (s >= 'a' && s <= 'a' + 32) {
                have_char = true, result_char = s - 'a' + 1;
            } else if (s >= 'A' && s <= 'A' + 32) {
                have_char = true, result_char = s - 'A' + 1;
            } else {
                errored = true;
            }
        } else if (c == 'e') {
            have_char = true, result_char = 0x1B;
        } else if (c == 'f') {
            have_char = true, result_char = 0x0C;
        } else if (c == 'n') {
            have_char = true, result_char = '\n';
        } else if (c == 'r') {
            have_char = true, result_char = '\r';
        } else if (c == 't') {
            have_char = true, result_char = '\t';
        } else if (c == 'v') {
            have_char = true, result_char = 0x0B;
        } else if (c == '\n') {
            have_char = false;
        } else {
            if (special) result->push_back(kInternalSeparator);
            have_char = true, result_char = c;
        }
        if (errored) return 0;
        if (!byte_buff.empty()) DecodeUtf8(byte_buff, result, nullptr);
        break;
    }
    if (have_char) {
        if (IsReserved(result_char)) return 0;
        result->push_back(result_char);
    }
    return in_pos;
}

}  // namespace

bool Unescape(std::u32string_view input, bool special, bool allow_incomplete, std::u32string *out,
              bool no_backslashes) {
    std::u32string &result = *out;
    result.clear();
    auto at = [&](size_t i) { return i < input.size() ? input[i] : U'\0'; };
    std::vector<size_t> braces, vars_or_seps;
    long brace_count = 0;
    size_t potential_word_start = static_cast<size_t>(-1);
    bool errored = false;
    enum { Unquoted, Single, Double } mode = Unquoted;
    static const std::u32string kSelf = U"%self";
    size_t pos = 0;
    while (pos < input.size() && !errored) {
        const char32_t c = input[pos];
        bool append = true;
        char32_t ch = c;
        if (mode == Unquoted) {
            if (c == '\\' && !no_backslashes) {
                const size_t n = ReadUnquotedEscape(input.substr(pos), &result, allow_incomplete, special);
                if (n) pos += n - 1;
                else errored = true;
                append = false;
            } else if (c == '~' && special && (pos == 0 || pos == potential_word_start)) {
                ch = kHomeDirectory;
            } else if (c == '%' && special && pos == 0 && input == kSelf) {
                ch = kProcessExpandSelf;
                pos += kSelf.size() - 1;
            } else if (c == '*' && special) {
                if (!result.empty() && result.back() == kAnyString) {
                    result.pop_back();
                    ch = kAnyStringRecursive;
                } else {
                    ch = kAnyString;
                }
            } else if (c == '$' && special) {
                const bool is_cmdsub = pos + 1 < input.size() && input[pos + 1] == '(';
                if (!is_cmdsub) {
                    ch = kVariableExpand;
                    vars_or_seps.push_back(pos);
                }
            } else if (c == '{' && special) {
                brace_count++;
                ch = kBraceBegin;
                braces.push_back(result.size());
                potential_word_start = pos + 1;
            } else if (c == '}' && special) {
                brace_count--;
                ch = kBraceEnd;
                if (!braces.empty()) {
                    const size_t brace = braces.back();
                    braces.pop_back();
                    if (vars_or_seps.empty() || vars_or_seps.back() < brace) {
                        result[brace] = '{';
                        for (size_t i = brace + 1; i < result.size(); i++)
                            if (result[i] == kBraceSpace) result[i] = ' ';
                        ch = '}';
                    }
                    while (!vars_or_seps.empty() && vars_or_seps.back() > brace) vars_or_seps.pop_back();
                }
            } else if (c == ',' && special && brace_count > 0) {
                ch = kBraceSep;
                vars_or_seps.push_back(pos);
                potential_word_start = pos + 1;
            } else if (c == ' ' && special && brace_count > 0) {
                ch = kBraceSpace;
            } else if (c == '\'') {
                mode = Single;
                append = special;
                ch = kInternalSeparator;
            } else if (c == '"') {
                mode = Double;
                append = special;
                ch = kInternalSeparator;
            }
        } else if (mode == Single) {
            if (c == '\\') {
                const char32_t n = at(pos + 1);
                if (n == '\\' || n == '\'') {
                    ch = n;
                    pos++;
                } else if (n == '\0') {
                    if (!allow_incomplete) {
                        errored = true;
                    } else {
                        pos++;
                        ch = '\\';
                    }
                }
            } else if (c == '\'') {
                append = special;
                ch = kInternalSeparator;
                mode = Unquoted;
            }
        } else {
            if (c == '"') {
                mode = Unquoted;
                append = special;
                ch = kInternalSeparator;
            } else if (c == '\\') {
                const char32_t n = at(pos + 1);
                if (n == '\0') {
                    if (!allow_incomplete) errored = true;
                    else ch = '\0';
                } else if (n == '\\' || n == '$' || n == '"') {
                    ch = n;
                    pos++;
                } else if (n == '\n') {
                    append = false;
                    pos++;
                }
            } else if (c == '$' && special) {
                ch = kVariableExpandSingle;
                vars_or_seps.push_back(pos);
            }
        }
        if (append && !errored) result.push_back(ch);
        pos++;
    }
    return !errored;
}

// --- Tokenizer --------------------------------------------------------------------------

const char *TokErrMessage(TokErr e) {
    switch (e) {
    case TokErr::None: return "";
    case TokErr::UnterminatedQuote: return "Unexpected end of string, quotes are not balanced";
    case TokErr::UnterminatedSubshell: return "Unexpected end of string, expecting ')'";
    case TokErr::UnterminatedSlice: return "Unexpected end of string, square brackets do not match";
    case TokErr::UnterminatedEscape: return "Unexpected end of string, incomplete escape sequence";
    case TokErr::InvalidRedirect: return "Invalid input/output redirection";
    case TokErr::InvalidPipe: return "Cannot use stdin (fd 0) as pipe output";
    case TokErr::ClosingUnopenedSubshell: return "Unexpected ')' for unopened parenthesis";
    case TokErr::IllegalSlice: return "Unexpected '[' at this location";
    case TokErr::ClosingUnopenedBrace: return "Unexpected '}' for unopened brace";
    case TokErr::UnterminatedBrace: return "Unexpected end of string, incomplete parameter expansion";
    case TokErr::ExpectedPcloseFoundBclose: return "Unexpected '}' found, expecting ')'";
    case TokErr::ExpectedBcloseFoundPclose: return "Unexpected ')' found, expecting '}'";
    }
    return "";
}

namespace {

bool SpaceNotNl(char32_t c) { return c != '\n' && (c == ' ' || c == '\t' || c == '\r' || IsWhitespace(c)); }

size_t CommentEnd(std::u32string_view s, size_t pos) {
    for (;;) {
        pos++;
        if (pos >= s.size() || s[pos] == '\n') return pos;
    }
}

bool IsTokenDelimiter(char32_t c, char32_t next) { return c == '(' || !TokIsStringChar(c, next); }

struct PipeOrRedir {
    long fd = -1;
    bool is_pipe = false;
    size_t consumed = 0;
};

// The redirection or pipe at the start of @p s; false if there is none.
bool ParsePipeOrRedir(std::u32string_view s, PipeOrRedir *out) {
    auto at = [&](size_t i) { return i < s.size() ? s[i] : U'\0'; };
    size_t cur = 0;
    while (cur < s.size() && s[cur] >= '0' && s[cur] <= '9') cur++;
    const bool has_fd = cur > 0;
    long fd_val = -1;
    if (has_fd) {
        fd_val = 0;
        for (size_t i = 0; i < cur; i++) {
            fd_val = fd_val * 10 + static_cast<long>(s[i] - '0');
            if (fd_val > 2147483647L) {
                fd_val = -1;
                break;
            }
        }
    }
    auto take = [&](char32_t c) {
        if (at(cur) != c) return false;
        cur++;
        return true;
    };
    PipeOrRedir r;
    const char32_t c = at(cur);
    if (c == '|') {
        if (has_fd) return false;
        cur++;
        r.fd = 1;
        r.is_pipe = true;
        take('&');
    } else if (c == '>') {
        cur++;
        take('>');
        if (take('|')) {
            r.is_pipe = true;
            r.fd = has_fd ? fd_val : 1;
        } else if (take('&')) {
            r.fd = has_fd ? fd_val : 1;
        } else {
            r.fd = has_fd ? fd_val : 1;
            take('?');
        }
    } else if (c == '<') {
        cur++;
        if (!take('&')) take('?');
        r.fd = has_fd ? fd_val : 0;
    } else if (c == '&') {
        cur++;
        if (take('|')) {
            r.fd = 1;
            r.is_pipe = true;
        } else if (take('>')) {
            r.fd = 1;
            take('>');
            take('?');
        } else {
            return false;
        }
    } else {
        return false;
    }
    r.consumed = cur;
    *out = r;
    return true;
}

bool IsKeywordChar(char32_t c) {
    return (c < 0x80 && std::isalnum(static_cast<int>(c))) || c == '\'' || c == '"' || c == '\\' || c == '\n' ||
           c == '!';
}

// fish's keyword view of a token: unquoted if it only holds keyword characters.
std::u32string UnescapeKeyword(std::u32string_view t) {
    bool needs = false;
    for (char32_t c : t) {
        if (!IsKeywordChar(c)) return {};
        needs = needs || c == '"' || c == '\'' || c == '\\';
    }
    if (!needs) return std::u32string(t);
    std::u32string out;
    if (!Unescape(t, false, false, &out)) return {};
    return out;
}

Kw KeywordFromText(std::u32string_view s) {
    static const std::pair<const char32_t *, Kw> kTable[] = {
        {U"!", Kw::Exclam}, {U"and", Kw::And}, {U"begin", Kw::Begin}, {U"builtin", Kw::Builtin},
        {U"case", Kw::Case}, {U"command", Kw::Command}, {U"else", Kw::Else}, {U"end", Kw::End},
        {U"exec", Kw::Exec}, {U"for", Kw::For}, {U"function", Kw::Function}, {U"if", Kw::If},
        {U"in", Kw::In}, {U"not", Kw::Not}, {U"or", Kw::Or}, {U"switch", Kw::Switch},
        {U"time", Kw::Time}, {U"while", Kw::While},
    };
    for (const auto &e : kTable)
        if (s == e.first) return e.second;
    return Kw::None;
}

bool IsSubcommandWord(std::u32string_view s) {
    static const char32_t *const kSub[] = {U"!", U"and", U"begin", U"builtin", U"command", U"else",
                                           U"exec", U"if", U"not", U"or", U"time", U"while"};
    for (const char32_t *w : kSub)
        if (s == w) return true;
    return false;
}

}  // namespace

bool TokIsStringChar(char32_t c, char32_t next) {
    switch (c) {
    case '\0': case ' ': case '\n': case '|': case '\t': case ';': case '\r': case '<': case '>':
        return false;
    case '&':
        return next != '\0' && TokIsStringChar(next, '\0');
    default:
        return true;
    }
}

size_t QuoteEnd(std::u32string_view s, size_t pos, char32_t quote) {
    for (;;) {
        pos++;
        if (pos >= s.size()) return std::u32string_view::npos;
        const char32_t c = s[pos];
        if (c == '\\') {
            pos++;
        } else if (c == quote || (quote == '"' && c == '$' && pos + 1 < s.size() && s[pos + 1] == '(')) {
            return pos;
        }
    }
}

size_t VariableAssignmentEquals(std::u32string_view s) {
    bool found = false;
    for (size_t i = 0; i < s.size(); i++) {
        const char32_t c = s[i];
        if (!found) {
            if (!ValidVarNameChar(c)) return std::u32string_view::npos;
            found = true;
        } else {
            if (c == '=') return i;
            if (!ValidVarNameChar(c)) return std::u32string_view::npos;
        }
    }
    return std::u32string_view::npos;
}

Tokenizer::Tokenizer(std::u32string_view src, unsigned flags, std::function<void(size_t)> on_quote)
    : src_(src),
      brace_parser_(!(flags & kTokArgumentList)),
      accept_unfinished_(flags & kTokAcceptUnfinished),
      show_comments_(flags & kTokShowComments),
      show_blank_lines_(flags & kTokShowBlankLines),
      continue_after_error_(flags & kTokContinueAfterError),
      on_quote_(std::move(on_quote)) {}

Tok Tokenizer::Error(TokErr e, size_t token_start, size_t error_loc, long token_length, size_t error_len) {
    if (token_length >= 0 && continue_after_error_) {
        cursor_ = error_loc + static_cast<size_t>(token_length);
    } else {
        has_next_ = false;
    }
    Tok t;
    t.type = TokType::Error;
    t.offset = token_start;
    t.length = token_length >= 0 ? static_cast<size_t>(token_length) : cursor_ - token_start;
    t.error_offset = error_loc - token_start;
    t.error_length = error_len;
    t.error = e;
    return t;
}

bool Tokenizer::Next(Tok *out) {
    if (!has_next_) return false;
    for (;;) {
        if (At(cursor_) == '\\' && At(cursor_ + 1) == '\n') {
            cursor_ += 2;
            continue_line_after_comment_ = true;
        } else if (cursor_ < src_.size() && SpaceNotNl(src_[cursor_])) {
            cursor_++;
        } else {
            break;
        }
    }
    while (At(cursor_) == '#' && cursor_ < src_.size()) {
        const size_t cs = cursor_;
        cursor_ = CommentEnd(src_, cursor_);
        const size_t clen = cursor_ - cs;
        if (At(cursor_) == '\n' && cursor_ < src_.size() && continue_line_after_comment_) cursor_++;
        if (show_comments_) {
            Tok t;
            t.type = TokType::Comment;
            t.offset = cs;
            t.length = clen;
            *out = t;
            return true;
        }
        while (cursor_ < src_.size() && SpaceNotNl(src_[cursor_])) cursor_++;
    }
    continue_line_after_comment_ = false;
    const size_t start = cursor_;
    const char32_t c = At(cursor_);
    const char32_t next = At(cursor_ + 1);
    bool at_cmd = false;
    Tok t;
    t.offset = start;
    if (cursor_ >= src_.size() || c == '\0') {
        has_next_ = false;
        return false;
    }
    if (c == '\r' || c == '\n' || c == ';') {
        t.type = TokType::End;
        t.length = 1;
        cursor_++;
        at_cmd = true;
        if (!show_blank_lines_) {
            while (cursor_ < src_.size()) {
                const char32_t d = src_[cursor_];
                if (d != '\n' && d != '\r' && d != ' ' && d != '\t') break;
                cursor_++;
            }
        }
    } else if (c == '{' && brace_parser_ && at_command_position_) {
        unclosed_braces_++;
        t.type = TokType::LeftBrace;
        t.length = 1;
        cursor_++;
        at_cmd = true;
    } else if (c == '}') {
        if (!brace_parser_ || unclosed_braces_ == 0) {
            *out = Error(TokErr::ClosingUnopenedBrace, cursor_, cursor_, 1, 1);
            return true;
        }
        unclosed_braces_--;
        t.type = TokType::RightBrace;
        t.length = 1;
        cursor_++;
    } else if (c == '&' && next == '&') {
        t.type = TokType::AndAnd;
        t.length = 2;
        cursor_ += 2;
        at_cmd = true;
    } else if (c == '&' && (next == '>' || next == '|')) {
        PipeOrRedir r;
        ParsePipeOrRedir(src_.substr(cursor_), &r);
        t.type = r.is_pipe ? TokType::Pipe : TokType::Redirect;
        t.length = r.consumed;
        cursor_ += r.consumed;
        at_cmd = next == '|';
    } else if (c == '&') {
        t.type = TokType::Background;
        t.length = 1;
        cursor_++;
        at_cmd = true;
    } else if (c == '|' && next == '|') {
        t.type = TokType::OrOr;
        t.length = 2;
        cursor_ += 2;
        at_cmd = true;
    } else if (c == '|') {
        PipeOrRedir r;
        ParsePipeOrRedir(src_.substr(cursor_), &r);
        t.type = r.is_pipe ? TokType::Pipe : TokType::Redirect;
        t.length = r.consumed;
        cursor_ += r.consumed;
        at_cmd = true;
    } else if (c == '>' || c == '<') {
        PipeOrRedir r;
        if (ParsePipeOrRedir(src_.substr(cursor_), &r)) {
            if (r.fd < 0) {
                t = Error(TokErr::InvalidRedirect, cursor_, cursor_, static_cast<long>(r.consumed), r.consumed);
            } else {
                t.type = r.is_pipe ? TokType::Pipe : TokType::Redirect;
                t.length = r.consumed;
                cursor_ += r.consumed;
            }
        } else {
            t = Error(TokErr::InvalidRedirect, cursor_, cursor_, -1, 0);
        }
    } else {
        PipeOrRedir r;
        const bool redir = c >= '0' && c <= '9' && ParsePipeOrRedir(src_.substr(cursor_), &r);
        if (redir) {
            if (r.is_pipe && r.fd == 0) {
                t = Error(TokErr::InvalidPipe, cursor_, cursor_, static_cast<long>(r.consumed), r.consumed);
            } else {
                t.type = r.is_pipe ? TokType::Pipe : TokType::Redirect;
                t.length = r.consumed;
                cursor_ += r.consumed;
                at_cmd = r.is_pipe;
            }
        } else {
            t = ReadString();
            if (brace_parser_ && at_command_position_) {
                const std::u32string_view text = src_.substr(t.offset, t.length);
                at_cmd = IsSubcommandWord(UnescapeKeyword(text)) ||
                         VariableAssignmentEquals(text) != std::u32string_view::npos;
            }
        }
    }
    if (brace_parser_) at_command_position_ = at_cmd;
    *out = t;
    return true;
}

bool Tokenizer::OpeningQuote(std::vector<size_t> &quoted_cmdsubs, size_t paren_depth, char32_t quote,
                             size_t *err) {
    if (on_quote_) on_quote_(cursor_);
    const size_t end = QuoteEnd(src_, cursor_, quote);
    if (end != std::u32string_view::npos) {
        size_t one_past = end + 1;
        if (src_[end] == '$') {
            one_past = end;
            quoted_cmdsubs.push_back(paren_depth);
        }
        cursor_ = end;
        if (on_quote_) on_quote_(one_past);
        return true;
    }
    *err = cursor_;
    cursor_ = src_.size();
    return false;
}

Tok Tokenizer::ReadString() {
    enum : unsigned { kSubshell = 1, kArray = 2, kCurly = 4, kEscape = 8 };
    unsigned mode = 0;
    std::vector<size_t> paren_offsets, brace_offsets, quoted_cmdsubs;
    std::vector<char32_t> expecting;
    size_t slice_offset = 0;
    const size_t buff_start = cursor_;
    bool is_token_begin = true;
    while (cursor_ != src_.size()) {
        const char32_t c = src_[cursor_];
        if (mode & kEscape) {
            mode &= ~static_cast<unsigned>(kEscape);
        } else if (c < 0x80 && std::isalpha(static_cast<int>(c))) {
        } else if (c == '\\') {
            mode |= kEscape;
        } else if (c == '#' && is_token_begin) {
            cursor_ = CommentEnd(src_, cursor_) - 1;
        } else if (c == '(') {
            paren_offsets.push_back(cursor_);
            expecting.push_back(')');
            mode |= kSubshell;
        } else if (c == '{') {
            brace_offsets.push_back(cursor_);
            expecting.push_back('}');
            mode |= kCurly;
        } else if (c == ')') {
            if (!expecting.empty() && expecting.back() == '}')
                return Error(TokErr::ExpectedBcloseFoundPclose, cursor_, cursor_, 1, 1);
            if (paren_offsets.empty()) return Error(TokErr::ClosingUnopenedSubshell, cursor_, cursor_, 1, 1);
            paren_offsets.pop_back();
            if (paren_offsets.empty()) mode &= ~static_cast<unsigned>(kSubshell);
            expecting.pop_back();
            if (!quoted_cmdsubs.empty() && quoted_cmdsubs.back() == paren_offsets.size()) {
                quoted_cmdsubs.pop_back();
                size_t err = 0;
                if (!OpeningQuote(quoted_cmdsubs, paren_offsets.size(), '"', &err)) {
                    if (!accept_unfinished_) return Error(TokErr::UnterminatedQuote, buff_start, err, -1, 0);
                    break;
                }
            }
        } else if (c == '}') {
            if (!expecting.empty() && expecting.back() == ')')
                return Error(TokErr::ExpectedPcloseFoundBclose, cursor_, cursor_, 1, 1);
            if (brace_offsets.empty()) break;
            brace_offsets.pop_back();
            if (brace_offsets.empty()) mode &= ~static_cast<unsigned>(kCurly);
            expecting.pop_back();
        } else if (c == '[') {
            if (cursor_ != buff_start) {
                mode |= kArray;
                slice_offset = cursor_;
            }
        } else if (c == ']' && (mode & kArray)) {
            mode &= ~static_cast<unsigned>(kArray);
        } else if (c == '\'' || c == '"') {
            size_t err = 0;
            if (!OpeningQuote(quoted_cmdsubs, paren_offsets.size(), c, &err)) {
                if (!accept_unfinished_) return Error(TokErr::UnterminatedQuote, buff_start, err, -1, 1);
                break;
            }
        } else if (mode == 0 && !TokIsStringChar(c, At(cursor_ + 1))) {
            break;
        }
        is_token_begin = IsTokenDelimiter(c, At(cursor_ + 1));
        cursor_++;
    }
    if (!accept_unfinished_ && mode != 0) {
        if (mode & kEscape) return Error(TokErr::UnterminatedEscape, buff_start, cursor_ - 1, -1, 1);
        if (mode & kArray) return Error(TokErr::UnterminatedSlice, buff_start, slice_offset, -1, 1);
        if (mode & kSubshell) return Error(TokErr::UnterminatedSubshell, buff_start, paren_offsets.back(), -1, 1);
        return Error(TokErr::UnterminatedBrace, buff_start, brace_offsets.back(), -1, 1);
    }
    Tok t;
    t.type = TokType::String;
    t.offset = buff_start;
    t.length = cursor_ - buff_start;
    return t;
}

int LocateCmdsub(std::u32string_view input, size_t *cursor, bool allow_incomplete, bool *is_quoted,
                 bool *has_dollar, Cmdsub *out) {
    if (*cursor >= input.size()) return 0;
    const size_t npos = std::u32string_view::npos;
    bool escaped = false, is_token_begin = true, syntax_error = false;
    long paren_count = 0;
    std::vector<long> quoted_cmdsubs;
    size_t pos = *cursor;
    size_t last_dollar = npos, paren_begin = npos, paren_end = npos;
    // Returns the quote's end or npos; `virt` starts one before pos (the ")" of "$(...)").
    auto opening_quote = [&](size_t p, char32_t q, bool virt) -> size_t {
        if (virt) p = p > 0 ? p - 1 : 0;
        const size_t qe = QuoteEnd(input, p, q);
        if (qe == npos) return npos;
        if (input[qe] == '$') {
            last_dollar = qe;
            quoted_cmdsubs.push_back(paren_count);
        }
        if (paren_count == 0 && is_quoted) *is_quoted = input[qe] == '$';
        return qe;
    };
    if (is_quoted && *is_quoted && !input.empty()) {
        const size_t qe = opening_quote(pos, '"', true);
        pos = qe == npos ? input.size() : qe + 1;
    }
    while (pos < input.size()) {
        const char32_t c = input[pos];
        if (!escaped) {
            if (c == '\'' || c == '"') {
                const size_t qe = opening_quote(pos, c, false);
                if (qe == npos) break;
                pos = qe;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '#' && is_token_begin) {
                pos = CommentEnd(input, pos) - 1;
            } else if (c == '$') {
                last_dollar = pos;
            } else if (c == '(') {
                if (paren_count == 0 && paren_begin == npos) {
                    paren_begin = pos;
                    if (has_dollar) *has_dollar = pos > 0 && last_dollar == pos - 1;
                }
                paren_count++;
            } else if (c == ')') {
                paren_count--;
                if (paren_count == 0) {
                    paren_end = pos;
                    break;
                }
                if (paren_count < 0) {
                    syntax_error = true;
                    break;
                }
                if (!quoted_cmdsubs.empty() && quoted_cmdsubs.back() == paren_count) {
                    quoted_cmdsubs.pop_back();
                    const size_t qe = opening_quote(pos, '"', true);
                    if (qe == npos) break;
                    pos = qe;
                }
            }
            is_token_begin = IsTokenDelimiter(c, pos + 1 < input.size() ? input[pos + 1] : U'\0');
        } else {
            escaped = false;
            is_token_begin = false;
        }
        pos++;
    }
    syntax_error = syntax_error || paren_count < 0 || (paren_count > 0 && !allow_incomplete);
    if (syntax_error) return -1;
    if (paren_begin == npos) return 0;
    out->start = paren_begin;
    out->end = paren_count != 0 ? input.size() : paren_end + 1;
    out->num_closing = paren_count == 0 ? 1 : 0;
    *cursor = out->end;
    return 1;
}

// --- Tree -------------------------------------------------------------------------------

const char *KwName(Kw k) {
    switch (k) {
    case Kw::And: return "and";
    case Kw::Begin: return "begin";
    case Kw::Builtin: return "builtin";
    case Kw::Case: return "case";
    case Kw::Command: return "command";
    case Kw::Else: return "else";
    case Kw::End: return "end";
    case Kw::Exclam: return "!";
    case Kw::Exec: return "exec";
    case Kw::For: return "for";
    case Kw::Function: return "function";
    case Kw::If: return "if";
    case Kw::In: return "in";
    case Kw::Not: return "not";
    case Kw::Or: return "or";
    case Kw::Switch: return "switch";
    case Kw::Time: return "time";
    case Kw::While: return "while";
    case Kw::None: break;
    }
    return "unknown_keyword";
}

int Ast::Field(int n, Role r) const {
    for (int k : nodes[static_cast<size_t>(n)].kids)
        if (nodes[static_cast<size_t>(k)].role == r) return k;
    return -1;
}

namespace {

void CombineRange(const Ast &ast, int n, Range *total, bool *unsourced) {
    const Node &node = ast[n];
    if (node.IsLeaf()) {
        if (!node.sourced) {
            *unsourced = true;
        } else if (node.len == 0) {
        } else if (total->len == 0) {
            *total = Range{node.start, node.len};
        } else {
            const size_t s = std::min(total->start, node.start);
            const size_t e = std::max(total->End(), node.start + node.len);
            *total = Range{s, e - s};
        }
        return;
    }
    for (int k : node.kids) CombineRange(ast, k, total, unsourced);
}

}  // namespace

bool Ast::RangeOf(int n, Range *out) const {
    Range total;
    bool unsourced = false;
    CombineRange(*this, n, &total, &unsourced);
    *out = unsourced ? Range{} : total;
    return !unsourced;
}

namespace {

struct PToken {
    PTok type = PTok::Invalid;
    Kw kw = Kw::None;
    bool dash = false, help = false, newline = false, may_assign = false;
    TokErr err = TokErr::None;
    size_t start = kNoLocation, len = 0;
    bool IsDashString() const { return type == PTok::String && dash; }
};

std::string TokTypeDescription(PTok t, Kw kw) {
    if (kw != Kw::None) return std::string("keyword: '") + KwName(kw) + "'";
    switch (t) {
    case PTok::String: return "a string";
    case PTok::Pipe: return "a pipe";
    case PTok::Redirection: return "a redirection";
    case PTok::Background: return "a '&'";
    case PTok::LeftBrace: return "a '{'";
    case PTok::RightBrace: return "a '}'";
    case PTok::AndAnd: return "'&&'";
    case PTok::OrOr: return "'||'";
    case PTok::End: return "end of the statement";
    case PTok::Terminate: return "end of the input";
    case PTok::TokenizerError: return "an incomplete token";
    case PTok::Invalid: break;
    }
    return "a ParseTokenType::invalid";
}

std::string KeywordsDescription(std::initializer_list<Kw> kws) {
    if (kws.size() == 1) return std::string("keyword '") + KwName(*kws.begin()) + "'";
    std::string res = "keywords ";
    bool first = true;
    for (Kw k : kws) {
        if (!first) res += " or ";
        first = false;
        res += std::string("'") + KwName(k) + "'";
    }
    return res;
}

std::string TokensDescription(std::initializer_list<PTok> ts) {
    std::string res;
    for (PTok t : ts) {
        if (!res.empty()) res += " or ";
        res += TokTypeDescription(t, Kw::None);
    }
    return res;
}

bool ListChompsNewlines(Kind k) {
    return k == Kind::JobList || k == Kind::CaseItemList || k == Kind::AndorJobList || k == Kind::ElseifClauseList;
}
bool ListChompsSemis(Kind k) { return k == Kind::JobList || k == Kind::CaseItemList || k == Kind::AndorJobList; }

constexpr int kMaxDepth = 2000;  // deeper than this reads as an error rather than overflowing the stack

class Populator {
public:
    Populator(Ast &ast, std::u32string_view src, const ParseFlags &f)
        : ast_(ast), src_(src), flags_(f), tok_(src, TokFlagsFor(f)) {}

    void Run() {
        const int top = New(Kind::JobList, Role::None, -1);
        PopulateList(top, true);
        ChompExtras(Kind::JobList);
        ast_.any_error = any_error_;
    }

private:
    static unsigned TokFlagsFor(const ParseFlags &f) {
        unsigned fl = 0;
        if (f.include_comments) fl |= kTokShowComments;
        if (f.accept_incomplete_tokens) fl |= kTokAcceptUnfinished;
        if (f.continue_after_error) fl |= kTokContinueAfterError;
        return fl;
    }

    // --- token stream ---
    PToken AdvanceOne() {
        Tok t;
        if (!tok_.Next(&t)) {
            PToken p;
            p.type = PTok::Terminate;
            return p;
        }
        PToken p;
        switch (t.type) {
        case TokType::String: p.type = PTok::String; break;
        case TokType::Pipe: p.type = PTok::Pipe; break;
        case TokType::Redirect: p.type = PTok::Redirection; break;
        case TokType::Background: p.type = PTok::Background; break;
        case TokType::End: p.type = PTok::End; break;
        case TokType::LeftBrace: p.type = PTok::LeftBrace; break;
        case TokType::RightBrace: p.type = PTok::RightBrace; break;
        case TokType::AndAnd: p.type = PTok::AndAnd; break;
        case TokType::OrOr: p.type = PTok::OrOr; break;
        case TokType::Error: p.type = PTok::TokenizerError; break;
        case TokType::Comment: p.type = PTok::Invalid; break;
        }
        const std::u32string_view text = src_.substr(t.offset, t.length);
        if (t.type == TokType::Comment) {
            ast_.comments.push_back(Range{t.offset, t.length});
            p.start = t.offset;
            p.len = t.length;
            return p;  // type Invalid marks a comment
        }
        if (t.type == TokType::String) p.kw = KeywordFromText(UnescapeKeyword(text));
        p.dash = !text.empty() && text[0] == '-';
        p.help = text == U"-h" || text == U"--help";
        p.newline = p.type == PTok::End && text == U"\n";
        p.may_assign = VariableAssignmentEquals(text) != std::u32string_view::npos;
        p.err = t.error;
        p.start = t.offset;
        p.len = t.length;
        if (t.error != TokErr::None && t.error_offset < p.len) {
            p.start += t.error_offset;
            p.len = t.error_length;
        }
        return p;
    }
    PToken NextFromTok() {
        for (;;) {
            PToken p = AdvanceOne();
            if (p.type == PTok::Invalid) continue;  // a comment, recorded
            return p;
        }
    }
    const PToken &Peek(size_t idx) {
        while (idx >= la_count_) {
            la_[(la_start_ + la_count_) % 2] = NextFromTok();
            la_count_++;
        }
        return la_[(la_start_ + idx) % 2];
    }
    PToken Pop() {
        if (la_count_ == 0) return NextFromTok();
        PToken r = la_[la_start_];
        la_start_ = (la_start_ + 1) % 2;
        la_count_--;
        return r;
    }

    // --- errors ---
    void Error(size_t start, size_t len, std::string text) {
        any_error_ = true;
        if (unwinding_) return;
        unwinding_ = true;
        if (start != kNoLocation) ast_.errors.push_back(Range{start, len});
        ast_.parse_errors.push_back(ParseError{start, len, std::move(text)});
    }
    void ErrorAt(const PToken &t, std::string text) { Error(t.start, t.len, std::move(text)); }

    bool UnsourceLeaves() {
        if (unwinding_) return true;
        return flags_.leave_unterminated && Peek(0).type == PTok::Terminate;
    }

    // --- nodes ---
    int New(Kind k, Role r, int parent) {
        Node n;
        n.kind = k;
        n.role = r;
        n.parent = parent;
        ast_.nodes.push_back(std::move(n));
        const int id = static_cast<int>(ast_.nodes.size()) - 1;
        if (parent >= 0) ast_.nodes[static_cast<size_t>(parent)].kids.push_back(id);
        return id;
    }
    Node &N(int i) { return ast_.nodes[static_cast<size_t>(i)]; }

    PToken ConsumeAny() { return Pop(); }

    // consume_token_type: always consumes; mismatches are errors.
    bool ConsumeType(PTok type, size_t *start, size_t *len) {
        PToken t = ConsumeAny();
        if (t.type != type) {
            ErrorAt(t, "Expected " + TokTypeDescription(type, Kw::None) + ", but found " +
                           TokTypeDescription(t.type, t.kw));
            *start = 0;
            *len = 0;
            return false;
        }
        *start = t.start;
        *len = t.len;
        return true;
    }

    void ConsumeExcess() {
        PToken t = ConsumeAny();
        switch (t.type) {
        case PTok::String:
            if (t.kw == Kw::Case) ErrorAt(t, "'case' builtin not inside of switch block");
            else if (t.kw == Kw::End) ErrorAt(t, "'end' outside of a block");
            else if (t.kw == Kw::Else) ErrorAt(t, "'else' builtin not inside of if block");
            else ErrorAt(t, "Expected a command");  // unreachable in fish
            break;
        case PTok::Redirection:
            if (Peek(0).type == PTok::String) {
                PToken next = Pop();
                const size_t s = std::min(next.start, t.start);
                const size_t e = std::max(next.start + next.len, t.start + t.len);
                Error(s, e - s, "Expected a string, but found a redirection");
                break;
            }
            ErrorAt(t, "Expected a string, but found " + TokTypeDescription(t.type, t.kw));
            break;
        case PTok::Pipe: case PTok::RightBrace: case PTok::Background: case PTok::AndAnd: case PTok::OrOr:
            ErrorAt(t, "Expected a string, but found " + TokTypeDescription(t.type, t.kw));
            break;
        case PTok::TokenizerError:
            ErrorAt(t, TokErrMessage(t.err));
            break;
        default:
            ErrorAt(t, "Unexpected token");
            break;
        }
    }

    void ChompExtras(Kind k) {
        const bool semis = ListChompsSemis(k), nls = ListChompsNewlines(k);
        for (;;) {
            const PToken &p = Peek(0);
            if (nls && p.type == PTok::End && p.newline) {
                Pop();
            } else if (semis && p.type == PTok::End && !p.newline) {
                PToken t = Pop();
                if (flags_.show_extra_semis) ast_.semis.push_back(Range{t.start, t.len});
            } else {
                break;
            }
        }
    }

    // --- leaves ---
    void VisitToken(int parent, Role role, std::initializer_list<PTok> allowed) {
        const int n = New(Kind::Token, role, parent);
        if (UnsourceLeaves()) return;
        const PToken &p = Peek(0);
        if (std::find(allowed.begin(), allowed.end(), p.type) == allowed.end()) {
            if (flags_.leave_unterminated &&
                (p.err == TokErr::UnterminatedQuote || p.err == TokErr::UnterminatedSubshell))
                return;
            ErrorAt(p, "Expected " + TokensDescription(allowed) + ", but found " + TokTypeDescription(p.type, p.kw));
            return;
        }
        PToken t = ConsumeAny();
        N(n).tok = t.type;
        N(n).sourced = true;
        N(n).start = t.start;
        N(n).len = t.len;
    }

    // Returns true on a "break": a missing `end` (the caller reports it).
    bool VisitKeyword(int parent, Role role, std::initializer_list<Kw> allowed) {
        const int n = New(Kind::Keyword, role, parent);
        N(n).kw_allowed = *allowed.begin();
        if (UnsourceLeaves()) return false;
        const PToken &p = Peek(0);
        if (std::find(allowed.begin(), allowed.end(), p.kw) == allowed.end()) {
            if (flags_.leave_unterminated &&
                (p.err == TokErr::UnterminatedQuote || p.err == TokErr::UnterminatedSubshell))
                return false;
            if (allowed.size() == 1 && *allowed.begin() == Kw::End) {
                break_tok_ = p;
                return true;
            }
            ErrorAt(p, "Expected " + KeywordsDescription(allowed) + ", but found " + TokTypeDescription(p.type, p.kw));
            return false;
        }
        PToken t = ConsumeAny();
        N(n).kw = t.kw;
        N(n).sourced = true;
        N(n).start = t.start;
        N(n).len = t.len;
        return false;
    }

    void VisitStringLeaf(int n) {
        if (UnsourceLeaves()) return;
        size_t s = 0, l = 0;
        ConsumeType(PTok::String, &s, &l);
        N(n).sourced = true;
        N(n).start = s;
        N(n).len = l;
    }

    void VisitArgument(int parent, Role role) { VisitStringLeaf(New(Kind::Argument, role, parent)); }

    void VisitMaybeNewlines(int parent) {
        const int n = New(Kind::MaybeNewlines, Role::Newlines, parent);
        if (UnsourceLeaves()) return;
        size_t start = 0, len = 0;
        while (Peek(0).newline) {
            size_t s = 0, l = 0;
            ConsumeType(PTok::End, &s, &l);
            if (len == 0) start = s, len = l;
            else len = s + l - start;
        }
        N(n).sourced = true;
        N(n).start = start;
        N(n).len = len;
    }

    // --- can_be_parsed ---
    bool CanParse(Kind k) {
        const PToken &p0 = Peek(0);
        switch (k) {
        case Kind::JobConjunction:
            return p0.type == PTok::LeftBrace ||
                   (p0.type == PTok::String && p0.kw != Kw::Case && p0.kw != Kw::End && p0.kw != Kw::Else);
        case Kind::VariableAssignment: {
            if (!p0.may_assign) return false;
            const PTok t1 = Peek(1).type;
            if (t1 == PTok::String || t1 == PTok::LeftBrace) return true;
            if (t1 == PTok::Terminate) return flags_.leave_unterminated;
            return false;
        }
        case Kind::JobContinuation: return p0.type == PTok::Pipe;
        case Kind::JobConjunctionContinuation: return p0.type == PTok::AndAnd || p0.type == PTok::OrOr;
        case Kind::ArgumentOrRedirection: return p0.type == PTok::String || p0.type == PTok::Redirection;
        case Kind::Argument: return p0.type == PTok::String;
        case Kind::ElseifClause: return p0.kw == Kw::Else && Peek(1).kw == Kw::If;
        case Kind::ElseClause: return p0.kw == Kw::Else;
        case Kind::CaseItem: return p0.kw == Kw::Case;
        case Kind::AndorJob: {
            if (p0.kw != Kw::And && p0.kw != Kw::Or) return false;
            const PToken &p1 = Peek(1);
            return (p1.type == PTok::String || p1.type == PTok::LeftBrace) && !p1.help;
        }
        default: return false;
        }
    }
    bool CanParseDecorator() {
        const Kw k = Peek(0).kw;
        if (k != Kw::And && k != Kw::Or) return false;
        return !Peek(1).help;
    }
    bool CanParseStatementDecorator() {
        const Kw k = Peek(0).kw;
        if (k != Kw::Command && k != Kw::Builtin && k != Kw::Exec) return false;
        const PToken &p1 = Peek(1);
        return p1.type == PTok::String && !p1.IsDashString();
    }
    bool CanParseTime() { return Peek(0).kw == Kw::Time && !Peek(1).IsDashString(); }

    // --- lists ---
    static Kind ContentsOf(Kind list) {
        switch (list) {
        case Kind::JobList: return Kind::JobConjunction;
        case Kind::VariableAssignmentList: return Kind::VariableAssignment;
        case Kind::ArgumentOrRedirectionList: return Kind::ArgumentOrRedirection;
        case Kind::ElseifClauseList: return Kind::ElseifClause;
        case Kind::JobContinuationList: return Kind::JobContinuation;
        case Kind::AndorJobList: return Kind::AndorJob;
        case Kind::JobConjunctionContinuationList: return Kind::JobConjunctionContinuation;
        case Kind::CaseItemList: return Kind::CaseItem;
        case Kind::ArgumentList: return Kind::Argument;
        default: return Kind::Argument;
        }
    }

    void PopulateList(int list, bool exhaust) {
        if (unwinding_) return;
        const Kind lk = N(list).kind;
        const Kind ck = ContentsOf(lk);
        for (;;) {
            if (unwinding_) {
                if (!(lk == Kind::JobList && flags_.continue_after_error)) break;
                for (;;) {
                    const PTok t = Peek(0).type;
                    if (t == PTok::String || t == PTok::Terminate || t == PTok::End) break;
                    PToken tk = Pop();
                    ast_.errors.push_back(Range{tk.start, tk.len});
                }
                unwinding_ = false;
            }
            ChompExtras(lk);
            if (CanParse(ck)) {
                VisitContents(ck, list);
            } else if (exhaust && Peek(0).type != PTok::Terminate) {
                ConsumeExcess();
            } else {
                break;
            }
        }
    }

    void VisitContents(Kind ck, int list) {
        switch (ck) {
        case Kind::JobConjunction: VisitJobConjunction(list, Role::Item); break;
        case Kind::VariableAssignment: {
            const int n = New(Kind::VariableAssignment, Role::Item, list);
            VisitStringLeaf(n);
            break;
        }
        case Kind::ArgumentOrRedirection: {
            const int n = New(Kind::ArgumentOrRedirection, Role::Item, list);
            if (Peek(0).type == PTok::String) {
                VisitArgument(n, Role::Embedded);
            } else {
                const int r = New(Kind::Redirection, Role::Embedded, n);
                VisitToken(r, Role::Oper, {PTok::Redirection});
                VisitToken(r, Role::Target, {PTok::String});
            }
            break;
        }
        case Kind::ElseifClause: {
            const int n = New(Kind::ElseifClause, Role::Item, list);
            VisitKeyword(n, Role::Kw, {Kw::Else});
            VisitIfClause(n);
            break;
        }
        case Kind::JobContinuation: VisitJobContinuation(list); break;
        case Kind::AndorJob: {
            const int n = New(Kind::AndorJob, Role::Item, list);
            VisitJobConjunction(n, Role::Job);
            break;
        }
        case Kind::JobConjunctionContinuation: {
            const int n = New(Kind::JobConjunctionContinuation, Role::Item, list);
            VisitToken(n, Role::Conjunction, {PTok::AndAnd, PTok::OrOr});
            VisitMaybeNewlines(n);
            VisitJobPipeline(n, Role::Job);
            break;
        }
        case Kind::CaseItem: {
            const int n = New(Kind::CaseItem, Role::Item, list);
            VisitKeyword(n, Role::Kw, {Kw::Case});
            PopulateList(New(Kind::ArgumentList, Role::Arguments, n), false);
            VisitToken(n, Role::SemiNl, {PTok::End});
            VisitJobList(n, Role::Body);
            break;
        }
        case Kind::Argument: VisitArgument(list, Role::Item); break;
        default: break;
        }
    }

    void VisitJobList(int parent, Role role) { PopulateList(New(Kind::JobList, role, parent), false); }

    // --- branches ---
    void VisitJobConjunction(int parent, Role role) {
        const int n = New(Kind::JobConjunction, role, parent);
        if (++depth_ > kMaxDepth) TooDeep();
        if (CanParseDecorator()) VisitKeyword(n, Role::Decorator, {Kw::And, Kw::Or});
        VisitJobPipeline(n, Role::Job);
        PopulateList(New(Kind::JobConjunctionContinuationList, Role::Continuations, n), false);
        if (Peek(0).type == PTok::End) VisitToken(n, Role::SemiNl, {PTok::End});
        depth_--;
    }

    void VisitJobPipeline(int parent, Role role) {
        const int n = New(Kind::JobPipeline, role, parent);
        if (CanParseTime()) VisitKeyword(n, Role::Time, {Kw::Time});
        PopulateList(New(Kind::VariableAssignmentList, Role::Variables, n), false);
        VisitStatement(n, Role::Statement);
        PopulateList(New(Kind::JobContinuationList, Role::Continuation, n), false);
        if (Peek(0).type == PTok::Background) VisitToken(n, Role::Bg, {PTok::Background});
    }

    void VisitJobContinuation(int list) {
        const Kw kw = Peek(1).kw;
        if (kw == Kw::And || kw == Kw::Or)
            ErrorAt(Peek(1), std::string("The '") + KwName(kw) + "' command can not be used in a pipeline");
        const int n = New(Kind::JobContinuation, Role::Item, list);
        VisitToken(n, Role::Pipe, {PTok::Pipe});
        VisitMaybeNewlines(n);
        PopulateList(New(Kind::VariableAssignmentList, Role::Variables, n), false);
        VisitStatement(n, Role::Statement);
    }

    void NewDecorated(int st) {
        const int n = New(Kind::DecoratedStatement, Role::Embedded, st);
        if (CanParseStatementDecorator()) VisitKeyword(n, Role::OptDecoration, {Kw::Command, Kw::Builtin, Kw::Exec});
        VisitToken(n, Role::Command, {PTok::String});
        PopulateList(New(Kind::ArgumentOrRedirectionList, Role::ArgsOrRedirs, n), false);
        if (!unwinding_ && Peek(0).type == PTok::LeftBrace)
            ErrorAt(Peek(0), "Expected end of the statement, but found " + TokTypeDescription(PTok::LeftBrace, Kw::None));
    }

    void VisitStatement(int parent, Role role) {
        const int st = New(Kind::Statement, role, parent);
        if (++depth_ > kMaxDepth) TooDeep();
        Statement(st);
        depth_--;
    }

    void Statement(int st) {
        const PToken p0 = Peek(0);
        if (p0.type == PTok::Terminate && flags_.leave_unterminated) {
            NewDecorated(st);
            return;
        }
        if (p0.type == PTok::LeftBrace) {
            const int n = New(Kind::BraceStatement, Role::Embedded, st);
            VisitToken(n, Role::LeftBrace, {PTok::LeftBrace});
            VisitJobList(n, Role::Jobs);
            VisitToken(n, Role::RightBrace, {PTok::RightBrace});
            PopulateList(New(Kind::ArgumentOrRedirectionList, Role::ArgsOrRedirs, n), false);
            return;
        }
        if (p0.type != PTok::String) {
            ErrorAt(p0, "Expected a command, but found " + TokTypeDescription(p0.type, p0.kw));
            NewDecorated(st);
            return;
        }
        if (p0.may_assign) {
            PToken t = ConsumeAny();
            const std::u32string_view text = src_.substr(t.start, t.len);
            const size_t eq = VariableAssignmentEquals(text);
            ErrorAt(t, "Unsupported use of '='. In fish, please use 'set " + ToUtf8(text.substr(0, eq)) + " " +
                           ToUtf8(text.substr(eq + 1)) + "'.");
            NewDecorated(st);
            return;
        }
        const PToken p1 = Peek(1);
        const bool help_only = p0.kw == Kw::Begin || p0.kw == Kw::Function || p0.kw == Kw::If ||
                               p0.kw == Kw::Switch || p0.kw == Kw::While;
        if (help_only ? p1.help : p1.IsDashString()) {
            NewDecorated(st);
            return;
        }
        if (p0.kw != Kw::Begin && p0.kw != Kw::End && p1.type == PTok::Terminate) {
            NewDecorated(st);
            return;
        }
        switch (p0.kw) {
        case Kw::Not: case Kw::Exclam: {
            const int n = New(Kind::NotStatement, Role::Embedded, st);
            VisitKeyword(n, Role::Kw, {Kw::Not, Kw::Exclam});
            if (CanParseTime()) VisitKeyword(n, Role::Time, {Kw::Time});
            PopulateList(New(Kind::VariableAssignmentList, Role::Variables, n), false);
            VisitStatement(n, Role::Contents);
            return;
        }
        case Kw::For: case Kw::While: case Kw::Function: case Kw::Begin: {
            const int n = New(Kind::BlockStatement, Role::Embedded, st);
            const int hw = New(Kind::BlockStatementHeader, Role::Header, n);
            if (p0.kw == Kw::For) {
                const int h = New(Kind::ForHeader, Role::Embedded, hw);
                VisitKeyword(h, Role::Kw, {Kw::For});
                VisitToken(h, Role::VarName, {PTok::String});
                VisitKeyword(h, Role::KwIn, {Kw::In});
                PopulateList(New(Kind::ArgumentList, Role::Args, h), false);
                VisitToken(h, Role::SemiNl, {PTok::End});
            } else if (p0.kw == Kw::While) {
                const int h = New(Kind::WhileHeader, Role::Embedded, hw);
                VisitKeyword(h, Role::Kw, {Kw::While});
                VisitJobConjunction(h, Role::Condition);
                PopulateList(New(Kind::AndorJobList, Role::AndorTail, h), false);
            } else if (p0.kw == Kw::Function) {
                const int h = New(Kind::FunctionHeader, Role::Embedded, hw);
                VisitKeyword(h, Role::Kw, {Kw::Function});
                VisitArgument(h, Role::FirstArg);
                PopulateList(New(Kind::ArgumentList, Role::Args, h), false);
                VisitToken(h, Role::SemiNl, {PTok::End});
            } else {
                const int h = New(Kind::BeginHeader, Role::Embedded, hw);
                VisitKeyword(h, Role::Kw, {Kw::Begin});
                if (Peek(0).type == PTok::End) VisitToken(h, Role::SemiNl, {PTok::End});
            }
            VisitJobList(n, Role::Jobs);
            if (VisitKeyword(n, Role::End, {Kw::End})) {
                MissingEnd(n);
                return;
            }
            PopulateList(New(Kind::ArgumentOrRedirectionList, Role::ArgsOrRedirs, n), false);
            return;
        }
        case Kw::If: {
            const int n = New(Kind::IfStatement, Role::Embedded, st);
            VisitIfClause(n);
            PopulateList(New(Kind::ElseifClauseList, Role::ElseifClauses, n), false);
            if (CanParse(Kind::ElseClause)) {
                const int e = New(Kind::ElseClause, Role::ElseClause, n);
                VisitKeyword(e, Role::Kw, {Kw::Else});
                if (Peek(0).type == PTok::End) VisitToken(e, Role::SemiNl, {PTok::End});
                VisitJobList(e, Role::Body);
            }
            if (VisitKeyword(n, Role::End, {Kw::End})) {
                MissingEnd(n);
                return;
            }
            PopulateList(New(Kind::ArgumentOrRedirectionList, Role::ArgsOrRedirs, n), false);
            return;
        }
        case Kw::Switch: {
            const int n = New(Kind::SwitchStatement, Role::Embedded, st);
            VisitKeyword(n, Role::Kw, {Kw::Switch});
            VisitArgument(n, Role::Argument);
            VisitToken(n, Role::SemiNl, {PTok::End});
            PopulateList(New(Kind::CaseItemList, Role::Cases, n), false);
            if (VisitKeyword(n, Role::End, {Kw::End})) {
                MissingEnd(n);
                return;
            }
            PopulateList(New(Kind::ArgumentOrRedirectionList, Role::ArgsOrRedirs, n), false);
            return;
        }
        case Kw::End:
            ErrorAt(p0, "Expected a command, but found " + TokTypeDescription(p0.type, p0.kw));
            NewDecorated(st);
            return;
        default:
            NewDecorated(st);
            return;
        }
    }

    void VisitIfClause(int parent) {
        const int n = New(Kind::IfClause, Role::IfClause, parent);
        VisitKeyword(n, Role::Kw, {Kw::If});
        VisitJobConjunction(n, Role::Condition);
        PopulateList(New(Kind::AndorJobList, Role::AndorTail, n), false);
        VisitJobList(n, Role::Body);
    }

    // did_visit_fields_of after a missing `end`.
    void MissingEnd(int n) {
        // The unvisited fields still exist in fish's tree, unsourced.
        New(Kind::ArgumentOrRedirectionList, Role::ArgsOrRedirs, n);
        if (unwinding_) return;
        const PToken tok = break_tok_;
        if (tok.type == PTok::TokenizerError && tok.err == TokErr::ClosingUnopenedBrace)
            ErrorAt(tok, TokErrMessage(tok.err));
        int kw = -1;
        const char *what = "";
        const Node &node = N(n);
        if (node.kind == Kind::BlockStatement) {
            const int h = ast_[ast_.Field(n, Role::Header)].kids[0];
            kw = ast_.Field(h, Role::Kw);
            switch (N(h).kind) {
            case Kind::ForHeader: what = "for loop"; break;
            case Kind::WhileHeader: what = "while loop"; break;
            case Kind::FunctionHeader: what = "function definition"; break;
            default: what = "begin"; break;
            }
        } else if (node.kind == Kind::IfStatement) {
            kw = ast_.Field(ast_.Field(n, Role::IfClause), Role::Kw);
            what = "if statement";
        } else {
            kw = ast_.Field(n, Role::Kw);
            what = "switch statement";
        }
        const PToken &next = Peek(0);
        if (next.type == PTok::String && (next.kw == Kw::Case || next.kw == Kw::Else || next.kw == Kw::End))
            ConsumeExcess();
        const Node &k = N(kw);
        Error(k.start, k.len, std::string("Missing end to balance this ") + what);
    }

    [[noreturn]] void TooDeep() { throw DepthExceeded{}; }

public:
    struct DepthExceeded {};

private:
    Ast &ast_;
    std::u32string_view src_;
    ParseFlags flags_;
    Tokenizer tok_;
    std::array<PToken, 2> la_{};
    size_t la_start_ = 0, la_count_ = 0;
    bool unwinding_ = false, any_error_ = false;
    PToken break_tok_;
    int depth_ = 0;
};

}  // namespace

Ast Parse(std::u32string_view src, const ParseFlags &flags) {
    Ast ast;
    try {
        Populator p(ast, src, flags);
        p.Run();
    } catch (const Populator::DepthExceeded &) {
        Ast bad;
        bad.nodes.push_back(Node{});
        bad.any_error = true;
        bad.parse_errors.push_back(ParseError{0, 0, "The nesting is too deep to read"});
        return bad;
    }
    return ast;
}

bool IsBuiltin(std::u32string_view name) {
    static const char32_t *const kBuiltins[] = {
        U"!", U".", U":", U"[", U"_", U"abbr", U"and", U"argparse", U"begin", U"bg", U"bind", U"block",
        U"break", U"breakpoint", U"builtin", U"case", U"cd", U"command", U"commandline", U"complete",
        U"contains", U"continue", U"count", U"disown", U"echo", U"else", U"emit", U"end", U"eval", U"exec",
        U"exit", U"false", U"fg", U"fish_indent", U"fish_key_reader", U"for", U"function", U"functions",
        U"history", U"if", U"jobs", U"math", U"not", U"or", U"path", U"printf", U"pwd", U"random", U"read",
        U"realpath", U"return", U"set", U"set_color", U"source", U"status", U"string", U"switch", U"test",
        U"time", U"true", U"type", U"ulimit", U"wait", U"while",
    };
    for (const char32_t *b : kBuiltins)
        if (name == b) return true;
    return false;
}

}  // namespace fishparse
