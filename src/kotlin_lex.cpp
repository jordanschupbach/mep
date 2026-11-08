// mep's Kotlin lexer: Kotlin.flex (the JFlex spec behind kotlinc's
// _JetLexer) ported rule for rule, with JFlex's longest-match / first-rule
// semantics written out by hand. See kotlin_parse.h.
//
// Ported from the Kotlin compiler (JetBrains, Apache License 2.0),
// Kotlin.flex, KtTokens.java, version 2.4.10.
#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

#include "kotlin_parse.h"

namespace kt {

namespace {

// JFlex's [:letter:] (Character.isLetter: Lu Ll Lt Lm Lo) and [:digit:] (Nd) above ASCII.
// Generated from the Unicode Character Database (Python unicodedata 16.0).
const uint32_t kLetterRanges[][2] = {
    {0xAA, 0xAA}, {0xB5, 0xB5}, {0xBA, 0xBA}, {0xC0, 0xD6}, {0xD8, 0xF6}, {0xF8, 0x2C1},
    {0x2C6, 0x2D1}, {0x2E0, 0x2E4}, {0x2EC, 0x2EC}, {0x2EE, 0x2EE}, {0x370, 0x374}, {0x376, 0x377},
    {0x37A, 0x37D}, {0x37F, 0x37F}, {0x386, 0x386}, {0x388, 0x38A}, {0x38C, 0x38C}, {0x38E, 0x3A1},
    {0x3A3, 0x3F5}, {0x3F7, 0x481}, {0x48A, 0x52F}, {0x531, 0x556}, {0x559, 0x559}, {0x560, 0x588},
    {0x5D0, 0x5EA}, {0x5EF, 0x5F2}, {0x620, 0x64A}, {0x66E, 0x66F}, {0x671, 0x6D3}, {0x6D5, 0x6D5},
    {0x6E5, 0x6E6}, {0x6EE, 0x6EF}, {0x6FA, 0x6FC}, {0x6FF, 0x6FF}, {0x710, 0x710}, {0x712, 0x72F},
    {0x74D, 0x7A5}, {0x7B1, 0x7B1}, {0x7CA, 0x7EA}, {0x7F4, 0x7F5}, {0x7FA, 0x7FA}, {0x800, 0x815},
    {0x81A, 0x81A}, {0x824, 0x824}, {0x828, 0x828}, {0x840, 0x858}, {0x860, 0x86A}, {0x870, 0x887},
    {0x889, 0x88E}, {0x8A0, 0x8C9}, {0x904, 0x939}, {0x93D, 0x93D}, {0x950, 0x950}, {0x958, 0x961},
    {0x971, 0x980}, {0x985, 0x98C}, {0x98F, 0x990}, {0x993, 0x9A8}, {0x9AA, 0x9B0}, {0x9B2, 0x9B2},
    {0x9B6, 0x9B9}, {0x9BD, 0x9BD}, {0x9CE, 0x9CE}, {0x9DC, 0x9DD}, {0x9DF, 0x9E1}, {0x9F0, 0x9F1},
    {0x9FC, 0x9FC}, {0xA05, 0xA0A}, {0xA0F, 0xA10}, {0xA13, 0xA28}, {0xA2A, 0xA30}, {0xA32, 0xA33},
    {0xA35, 0xA36}, {0xA38, 0xA39}, {0xA59, 0xA5C}, {0xA5E, 0xA5E}, {0xA72, 0xA74}, {0xA85, 0xA8D},
    {0xA8F, 0xA91}, {0xA93, 0xAA8}, {0xAAA, 0xAB0}, {0xAB2, 0xAB3}, {0xAB5, 0xAB9}, {0xABD, 0xABD},
    {0xAD0, 0xAD0}, {0xAE0, 0xAE1}, {0xAF9, 0xAF9}, {0xB05, 0xB0C}, {0xB0F, 0xB10}, {0xB13, 0xB28},
    {0xB2A, 0xB30}, {0xB32, 0xB33}, {0xB35, 0xB39}, {0xB3D, 0xB3D}, {0xB5C, 0xB5D}, {0xB5F, 0xB61},
    {0xB71, 0xB71}, {0xB83, 0xB83}, {0xB85, 0xB8A}, {0xB8E, 0xB90}, {0xB92, 0xB95}, {0xB99, 0xB9A},
    {0xB9C, 0xB9C}, {0xB9E, 0xB9F}, {0xBA3, 0xBA4}, {0xBA8, 0xBAA}, {0xBAE, 0xBB9}, {0xBD0, 0xBD0},
    {0xC05, 0xC0C}, {0xC0E, 0xC10}, {0xC12, 0xC28}, {0xC2A, 0xC39}, {0xC3D, 0xC3D}, {0xC58, 0xC5A},
    {0xC5D, 0xC5D}, {0xC60, 0xC61}, {0xC80, 0xC80}, {0xC85, 0xC8C}, {0xC8E, 0xC90}, {0xC92, 0xCA8},
    {0xCAA, 0xCB3}, {0xCB5, 0xCB9}, {0xCBD, 0xCBD}, {0xCDD, 0xCDE}, {0xCE0, 0xCE1}, {0xCF1, 0xCF2},
    {0xD04, 0xD0C}, {0xD0E, 0xD10}, {0xD12, 0xD3A}, {0xD3D, 0xD3D}, {0xD4E, 0xD4E}, {0xD54, 0xD56},
    {0xD5F, 0xD61}, {0xD7A, 0xD7F}, {0xD85, 0xD96}, {0xD9A, 0xDB1}, {0xDB3, 0xDBB}, {0xDBD, 0xDBD},
    {0xDC0, 0xDC6}, {0xE01, 0xE30}, {0xE32, 0xE33}, {0xE40, 0xE46}, {0xE81, 0xE82}, {0xE84, 0xE84},
    {0xE86, 0xE8A}, {0xE8C, 0xEA3}, {0xEA5, 0xEA5}, {0xEA7, 0xEB0}, {0xEB2, 0xEB3}, {0xEBD, 0xEBD},
    {0xEC0, 0xEC4}, {0xEC6, 0xEC6}, {0xEDC, 0xEDF}, {0xF00, 0xF00}, {0xF40, 0xF47}, {0xF49, 0xF6C},
    {0xF88, 0xF8C}, {0x1000, 0x102A}, {0x103F, 0x103F}, {0x1050, 0x1055}, {0x105A, 0x105D},
    {0x1061, 0x1061}, {0x1065, 0x1066}, {0x106E, 0x1070}, {0x1075, 0x1081}, {0x108E, 0x108E},
    {0x10A0, 0x10C5}, {0x10C7, 0x10C7}, {0x10CD, 0x10CD}, {0x10D0, 0x10FA}, {0x10FC, 0x1248},
    {0x124A, 0x124D}, {0x1250, 0x1256}, {0x1258, 0x1258}, {0x125A, 0x125D}, {0x1260, 0x1288},
    {0x128A, 0x128D}, {0x1290, 0x12B0}, {0x12B2, 0x12B5}, {0x12B8, 0x12BE}, {0x12C0, 0x12C0},
    {0x12C2, 0x12C5}, {0x12C8, 0x12D6}, {0x12D8, 0x1310}, {0x1312, 0x1315}, {0x1318, 0x135A},
    {0x1380, 0x138F}, {0x13A0, 0x13F5}, {0x13F8, 0x13FD}, {0x1401, 0x166C}, {0x166F, 0x167F},
    {0x1681, 0x169A}, {0x16A0, 0x16EA}, {0x16F1, 0x16F8}, {0x1700, 0x1711}, {0x171F, 0x1731},
    {0x1740, 0x1751}, {0x1760, 0x176C}, {0x176E, 0x1770}, {0x1780, 0x17B3}, {0x17D7, 0x17D7},
    {0x17DC, 0x17DC}, {0x1820, 0x1878}, {0x1880, 0x1884}, {0x1887, 0x18A8}, {0x18AA, 0x18AA},
    {0x18B0, 0x18F5}, {0x1900, 0x191E}, {0x1950, 0x196D}, {0x1970, 0x1974}, {0x1980, 0x19AB},
    {0x19B0, 0x19C9}, {0x1A00, 0x1A16}, {0x1A20, 0x1A54}, {0x1AA7, 0x1AA7}, {0x1B05, 0x1B33},
    {0x1B45, 0x1B4C}, {0x1B83, 0x1BA0}, {0x1BAE, 0x1BAF}, {0x1BBA, 0x1BE5}, {0x1C00, 0x1C23},
    {0x1C4D, 0x1C4F}, {0x1C5A, 0x1C7D}, {0x1C80, 0x1C8A}, {0x1C90, 0x1CBA}, {0x1CBD, 0x1CBF},
    {0x1CE9, 0x1CEC}, {0x1CEE, 0x1CF3}, {0x1CF5, 0x1CF6}, {0x1CFA, 0x1CFA}, {0x1D00, 0x1DBF},
    {0x1E00, 0x1F15}, {0x1F18, 0x1F1D}, {0x1F20, 0x1F45}, {0x1F48, 0x1F4D}, {0x1F50, 0x1F57},
    {0x1F59, 0x1F59}, {0x1F5B, 0x1F5B}, {0x1F5D, 0x1F5D}, {0x1F5F, 0x1F7D}, {0x1F80, 0x1FB4},
    {0x1FB6, 0x1FBC}, {0x1FBE, 0x1FBE}, {0x1FC2, 0x1FC4}, {0x1FC6, 0x1FCC}, {0x1FD0, 0x1FD3},
    {0x1FD6, 0x1FDB}, {0x1FE0, 0x1FEC}, {0x1FF2, 0x1FF4}, {0x1FF6, 0x1FFC}, {0x2071, 0x2071},
    {0x207F, 0x207F}, {0x2090, 0x209C}, {0x2102, 0x2102}, {0x2107, 0x2107}, {0x210A, 0x2113},
    {0x2115, 0x2115}, {0x2119, 0x211D}, {0x2124, 0x2124}, {0x2126, 0x2126}, {0x2128, 0x2128},
    {0x212A, 0x212D}, {0x212F, 0x2139}, {0x213C, 0x213F}, {0x2145, 0x2149}, {0x214E, 0x214E},
    {0x2183, 0x2184}, {0x2C00, 0x2CE4}, {0x2CEB, 0x2CEE}, {0x2CF2, 0x2CF3}, {0x2D00, 0x2D25},
    {0x2D27, 0x2D27}, {0x2D2D, 0x2D2D}, {0x2D30, 0x2D67}, {0x2D6F, 0x2D6F}, {0x2D80, 0x2D96},
    {0x2DA0, 0x2DA6}, {0x2DA8, 0x2DAE}, {0x2DB0, 0x2DB6}, {0x2DB8, 0x2DBE}, {0x2DC0, 0x2DC6},
    {0x2DC8, 0x2DCE}, {0x2DD0, 0x2DD6}, {0x2DD8, 0x2DDE}, {0x2E2F, 0x2E2F}, {0x3005, 0x3006},
    {0x3031, 0x3035}, {0x303B, 0x303C}, {0x3041, 0x3096}, {0x309D, 0x309F}, {0x30A1, 0x30FA},
    {0x30FC, 0x30FF}, {0x3105, 0x312F}, {0x3131, 0x318E}, {0x31A0, 0x31BF}, {0x31F0, 0x31FF},
    {0x3400, 0x4DBF}, {0x4E00, 0xA48C}, {0xA4D0, 0xA4FD}, {0xA500, 0xA60C}, {0xA610, 0xA61F},
    {0xA62A, 0xA62B}, {0xA640, 0xA66E}, {0xA67F, 0xA69D}, {0xA6A0, 0xA6E5}, {0xA717, 0xA71F},
    {0xA722, 0xA788}, {0xA78B, 0xA7CD}, {0xA7D0, 0xA7D1}, {0xA7D3, 0xA7D3}, {0xA7D5, 0xA7DC},
    {0xA7F2, 0xA801}, {0xA803, 0xA805}, {0xA807, 0xA80A}, {0xA80C, 0xA822}, {0xA840, 0xA873},
    {0xA882, 0xA8B3}, {0xA8F2, 0xA8F7}, {0xA8FB, 0xA8FB}, {0xA8FD, 0xA8FE}, {0xA90A, 0xA925},
    {0xA930, 0xA946}, {0xA960, 0xA97C}, {0xA984, 0xA9B2}, {0xA9CF, 0xA9CF}, {0xA9E0, 0xA9E4},
    {0xA9E6, 0xA9EF}, {0xA9FA, 0xA9FE}, {0xAA00, 0xAA28}, {0xAA40, 0xAA42}, {0xAA44, 0xAA4B},
    {0xAA60, 0xAA76}, {0xAA7A, 0xAA7A}, {0xAA7E, 0xAAAF}, {0xAAB1, 0xAAB1}, {0xAAB5, 0xAAB6},
    {0xAAB9, 0xAABD}, {0xAAC0, 0xAAC0}, {0xAAC2, 0xAAC2}, {0xAADB, 0xAADD}, {0xAAE0, 0xAAEA},
    {0xAAF2, 0xAAF4}, {0xAB01, 0xAB06}, {0xAB09, 0xAB0E}, {0xAB11, 0xAB16}, {0xAB20, 0xAB26},
    {0xAB28, 0xAB2E}, {0xAB30, 0xAB5A}, {0xAB5C, 0xAB69}, {0xAB70, 0xABE2}, {0xAC00, 0xD7A3},
    {0xD7B0, 0xD7C6}, {0xD7CB, 0xD7FB}, {0xF900, 0xFA6D}, {0xFA70, 0xFAD9}, {0xFB00, 0xFB06},
    {0xFB13, 0xFB17}, {0xFB1D, 0xFB1D}, {0xFB1F, 0xFB28}, {0xFB2A, 0xFB36}, {0xFB38, 0xFB3C},
    {0xFB3E, 0xFB3E}, {0xFB40, 0xFB41}, {0xFB43, 0xFB44}, {0xFB46, 0xFBB1}, {0xFBD3, 0xFD3D},
    {0xFD50, 0xFD8F}, {0xFD92, 0xFDC7}, {0xFDF0, 0xFDFB}, {0xFE70, 0xFE74}, {0xFE76, 0xFEFC},
    {0xFF21, 0xFF3A}, {0xFF41, 0xFF5A}, {0xFF66, 0xFFBE}, {0xFFC2, 0xFFC7}, {0xFFCA, 0xFFCF},
    {0xFFD2, 0xFFD7}, {0xFFDA, 0xFFDC}, {0x10000, 0x1000B}, {0x1000D, 0x10026}, {0x10028, 0x1003A},
    {0x1003C, 0x1003D}, {0x1003F, 0x1004D}, {0x10050, 0x1005D}, {0x10080, 0x100FA},
    {0x10280, 0x1029C}, {0x102A0, 0x102D0}, {0x10300, 0x1031F}, {0x1032D, 0x10340},
    {0x10342, 0x10349}, {0x10350, 0x10375}, {0x10380, 0x1039D}, {0x103A0, 0x103C3},
    {0x103C8, 0x103CF}, {0x10400, 0x1049D}, {0x104B0, 0x104D3}, {0x104D8, 0x104FB},
    {0x10500, 0x10527}, {0x10530, 0x10563}, {0x10570, 0x1057A}, {0x1057C, 0x1058A},
    {0x1058C, 0x10592}, {0x10594, 0x10595}, {0x10597, 0x105A1}, {0x105A3, 0x105B1},
    {0x105B3, 0x105B9}, {0x105BB, 0x105BC}, {0x105C0, 0x105F3}, {0x10600, 0x10736},
    {0x10740, 0x10755}, {0x10760, 0x10767}, {0x10780, 0x10785}, {0x10787, 0x107B0},
    {0x107B2, 0x107BA}, {0x10800, 0x10805}, {0x10808, 0x10808}, {0x1080A, 0x10835},
    {0x10837, 0x10838}, {0x1083C, 0x1083C}, {0x1083F, 0x10855}, {0x10860, 0x10876},
    {0x10880, 0x1089E}, {0x108E0, 0x108F2}, {0x108F4, 0x108F5}, {0x10900, 0x10915},
    {0x10920, 0x10939}, {0x10980, 0x109B7}, {0x109BE, 0x109BF}, {0x10A00, 0x10A00},
    {0x10A10, 0x10A13}, {0x10A15, 0x10A17}, {0x10A19, 0x10A35}, {0x10A60, 0x10A7C},
    {0x10A80, 0x10A9C}, {0x10AC0, 0x10AC7}, {0x10AC9, 0x10AE4}, {0x10B00, 0x10B35},
    {0x10B40, 0x10B55}, {0x10B60, 0x10B72}, {0x10B80, 0x10B91}, {0x10C00, 0x10C48},
    {0x10C80, 0x10CB2}, {0x10CC0, 0x10CF2}, {0x10D00, 0x10D23}, {0x10D4A, 0x10D65},
    {0x10D6F, 0x10D85}, {0x10E80, 0x10EA9}, {0x10EB0, 0x10EB1}, {0x10EC2, 0x10EC4},
    {0x10F00, 0x10F1C}, {0x10F27, 0x10F27}, {0x10F30, 0x10F45}, {0x10F70, 0x10F81},
    {0x10FB0, 0x10FC4}, {0x10FE0, 0x10FF6}, {0x11003, 0x11037}, {0x11071, 0x11072},
    {0x11075, 0x11075}, {0x11083, 0x110AF}, {0x110D0, 0x110E8}, {0x11103, 0x11126},
    {0x11144, 0x11144}, {0x11147, 0x11147}, {0x11150, 0x11172}, {0x11176, 0x11176},
    {0x11183, 0x111B2}, {0x111C1, 0x111C4}, {0x111DA, 0x111DA}, {0x111DC, 0x111DC},
    {0x11200, 0x11211}, {0x11213, 0x1122B}, {0x1123F, 0x11240}, {0x11280, 0x11286},
    {0x11288, 0x11288}, {0x1128A, 0x1128D}, {0x1128F, 0x1129D}, {0x1129F, 0x112A8},
    {0x112B0, 0x112DE}, {0x11305, 0x1130C}, {0x1130F, 0x11310}, {0x11313, 0x11328},
    {0x1132A, 0x11330}, {0x11332, 0x11333}, {0x11335, 0x11339}, {0x1133D, 0x1133D},
    {0x11350, 0x11350}, {0x1135D, 0x11361}, {0x11380, 0x11389}, {0x1138B, 0x1138B},
    {0x1138E, 0x1138E}, {0x11390, 0x113B5}, {0x113B7, 0x113B7}, {0x113D1, 0x113D1},
    {0x113D3, 0x113D3}, {0x11400, 0x11434}, {0x11447, 0x1144A}, {0x1145F, 0x11461},
    {0x11480, 0x114AF}, {0x114C4, 0x114C5}, {0x114C7, 0x114C7}, {0x11580, 0x115AE},
    {0x115D8, 0x115DB}, {0x11600, 0x1162F}, {0x11644, 0x11644}, {0x11680, 0x116AA},
    {0x116B8, 0x116B8}, {0x11700, 0x1171A}, {0x11740, 0x11746}, {0x11800, 0x1182B},
    {0x118A0, 0x118DF}, {0x118FF, 0x11906}, {0x11909, 0x11909}, {0x1190C, 0x11913},
    {0x11915, 0x11916}, {0x11918, 0x1192F}, {0x1193F, 0x1193F}, {0x11941, 0x11941},
    {0x119A0, 0x119A7}, {0x119AA, 0x119D0}, {0x119E1, 0x119E1}, {0x119E3, 0x119E3},
    {0x11A00, 0x11A00}, {0x11A0B, 0x11A32}, {0x11A3A, 0x11A3A}, {0x11A50, 0x11A50},
    {0x11A5C, 0x11A89}, {0x11A9D, 0x11A9D}, {0x11AB0, 0x11AF8}, {0x11BC0, 0x11BE0},
    {0x11C00, 0x11C08}, {0x11C0A, 0x11C2E}, {0x11C40, 0x11C40}, {0x11C72, 0x11C8F},
    {0x11D00, 0x11D06}, {0x11D08, 0x11D09}, {0x11D0B, 0x11D30}, {0x11D46, 0x11D46},
    {0x11D60, 0x11D65}, {0x11D67, 0x11D68}, {0x11D6A, 0x11D89}, {0x11D98, 0x11D98},
    {0x11EE0, 0x11EF2}, {0x11F02, 0x11F02}, {0x11F04, 0x11F10}, {0x11F12, 0x11F33},
    {0x11FB0, 0x11FB0}, {0x12000, 0x12399}, {0x12480, 0x12543}, {0x12F90, 0x12FF0},
    {0x13000, 0x1342F}, {0x13441, 0x13446}, {0x13460, 0x143FA}, {0x14400, 0x14646},
    {0x16100, 0x1611D}, {0x16800, 0x16A38}, {0x16A40, 0x16A5E}, {0x16A70, 0x16ABE},
    {0x16AD0, 0x16AED}, {0x16B00, 0x16B2F}, {0x16B40, 0x16B43}, {0x16B63, 0x16B77},
    {0x16B7D, 0x16B8F}, {0x16D40, 0x16D6C}, {0x16E40, 0x16E7F}, {0x16F00, 0x16F4A},
    {0x16F50, 0x16F50}, {0x16F93, 0x16F9F}, {0x16FE0, 0x16FE1}, {0x16FE3, 0x16FE3},
    {0x17000, 0x187F7}, {0x18800, 0x18CD5}, {0x18CFF, 0x18D08}, {0x1AFF0, 0x1AFF3},
    {0x1AFF5, 0x1AFFB}, {0x1AFFD, 0x1AFFE}, {0x1B000, 0x1B122}, {0x1B132, 0x1B132},
    {0x1B150, 0x1B152}, {0x1B155, 0x1B155}, {0x1B164, 0x1B167}, {0x1B170, 0x1B2FB},
    {0x1BC00, 0x1BC6A}, {0x1BC70, 0x1BC7C}, {0x1BC80, 0x1BC88}, {0x1BC90, 0x1BC99},
    {0x1D400, 0x1D454}, {0x1D456, 0x1D49C}, {0x1D49E, 0x1D49F}, {0x1D4A2, 0x1D4A2},
    {0x1D4A5, 0x1D4A6}, {0x1D4A9, 0x1D4AC}, {0x1D4AE, 0x1D4B9}, {0x1D4BB, 0x1D4BB},
    {0x1D4BD, 0x1D4C3}, {0x1D4C5, 0x1D505}, {0x1D507, 0x1D50A}, {0x1D50D, 0x1D514},
    {0x1D516, 0x1D51C}, {0x1D51E, 0x1D539}, {0x1D53B, 0x1D53E}, {0x1D540, 0x1D544},
    {0x1D546, 0x1D546}, {0x1D54A, 0x1D550}, {0x1D552, 0x1D6A5}, {0x1D6A8, 0x1D6C0},
    {0x1D6C2, 0x1D6DA}, {0x1D6DC, 0x1D6FA}, {0x1D6FC, 0x1D714}, {0x1D716, 0x1D734},
    {0x1D736, 0x1D74E}, {0x1D750, 0x1D76E}, {0x1D770, 0x1D788}, {0x1D78A, 0x1D7A8},
    {0x1D7AA, 0x1D7C2}, {0x1D7C4, 0x1D7CB}, {0x1DF00, 0x1DF1E}, {0x1DF25, 0x1DF2A},
    {0x1E030, 0x1E06D}, {0x1E100, 0x1E12C}, {0x1E137, 0x1E13D}, {0x1E14E, 0x1E14E},
    {0x1E290, 0x1E2AD}, {0x1E2C0, 0x1E2EB}, {0x1E4D0, 0x1E4EB}, {0x1E5D0, 0x1E5ED},
    {0x1E5F0, 0x1E5F0}, {0x1E7E0, 0x1E7E6}, {0x1E7E8, 0x1E7EB}, {0x1E7ED, 0x1E7EE},
    {0x1E7F0, 0x1E7FE}, {0x1E800, 0x1E8C4}, {0x1E900, 0x1E943}, {0x1E94B, 0x1E94B},
    {0x1EE00, 0x1EE03}, {0x1EE05, 0x1EE1F}, {0x1EE21, 0x1EE22}, {0x1EE24, 0x1EE24},
    {0x1EE27, 0x1EE27}, {0x1EE29, 0x1EE32}, {0x1EE34, 0x1EE37}, {0x1EE39, 0x1EE39},
    {0x1EE3B, 0x1EE3B}, {0x1EE42, 0x1EE42}, {0x1EE47, 0x1EE47}, {0x1EE49, 0x1EE49},
    {0x1EE4B, 0x1EE4B}, {0x1EE4D, 0x1EE4F}, {0x1EE51, 0x1EE52}, {0x1EE54, 0x1EE54},
    {0x1EE57, 0x1EE57}, {0x1EE59, 0x1EE59}, {0x1EE5B, 0x1EE5B}, {0x1EE5D, 0x1EE5D},
    {0x1EE5F, 0x1EE5F}, {0x1EE61, 0x1EE62}, {0x1EE64, 0x1EE64}, {0x1EE67, 0x1EE6A},
    {0x1EE6C, 0x1EE72}, {0x1EE74, 0x1EE77}, {0x1EE79, 0x1EE7C}, {0x1EE7E, 0x1EE7E},
    {0x1EE80, 0x1EE89}, {0x1EE8B, 0x1EE9B}, {0x1EEA1, 0x1EEA3}, {0x1EEA5, 0x1EEA9},
    {0x1EEAB, 0x1EEBB}, {0x20000, 0x2A6DF}, {0x2A700, 0x2B739}, {0x2B740, 0x2B81D},
    {0x2B820, 0x2CEA1}, {0x2CEB0, 0x2EBE0}, {0x2EBF0, 0x2EE5D}, {0x2F800, 0x2FA1D},
    {0x30000, 0x3134A}, {0x31350, 0x323AF},
};
const uint32_t kDigitRanges[][2] = {
    {0x660, 0x669}, {0x6F0, 0x6F9}, {0x7C0, 0x7C9}, {0x966, 0x96F}, {0x9E6, 0x9EF}, {0xA66, 0xA6F},
    {0xAE6, 0xAEF}, {0xB66, 0xB6F}, {0xBE6, 0xBEF}, {0xC66, 0xC6F}, {0xCE6, 0xCEF}, {0xD66, 0xD6F},
    {0xDE6, 0xDEF}, {0xE50, 0xE59}, {0xED0, 0xED9}, {0xF20, 0xF29}, {0x1040, 0x1049},
    {0x1090, 0x1099}, {0x17E0, 0x17E9}, {0x1810, 0x1819}, {0x1946, 0x194F}, {0x19D0, 0x19D9},
    {0x1A80, 0x1A89}, {0x1A90, 0x1A99}, {0x1B50, 0x1B59}, {0x1BB0, 0x1BB9}, {0x1C40, 0x1C49},
    {0x1C50, 0x1C59}, {0xA620, 0xA629}, {0xA8D0, 0xA8D9}, {0xA900, 0xA909}, {0xA9D0, 0xA9D9},
    {0xA9F0, 0xA9F9}, {0xAA50, 0xAA59}, {0xABF0, 0xABF9}, {0xFF10, 0xFF19}, {0x104A0, 0x104A9},
    {0x10D30, 0x10D39}, {0x10D40, 0x10D49}, {0x11066, 0x1106F}, {0x110F0, 0x110F9},
    {0x11136, 0x1113F}, {0x111D0, 0x111D9}, {0x112F0, 0x112F9}, {0x11450, 0x11459},
    {0x114D0, 0x114D9}, {0x11650, 0x11659}, {0x116C0, 0x116C9}, {0x116D0, 0x116E3},
    {0x11730, 0x11739}, {0x118E0, 0x118E9}, {0x11950, 0x11959}, {0x11BF0, 0x11BF9},
    {0x11C50, 0x11C59}, {0x11D50, 0x11D59}, {0x11DA0, 0x11DA9}, {0x11F50, 0x11F59},
    {0x16130, 0x16139}, {0x16A60, 0x16A69}, {0x16AC0, 0x16AC9}, {0x16B50, 0x16B59},
    {0x16D70, 0x16D79}, {0x1CCF0, 0x1CCF9}, {0x1D7CE, 0x1D7FF}, {0x1E140, 0x1E149},
    {0x1E2F0, 0x1E2F9}, {0x1E4F0, 0x1E4F9}, {0x1E5F1, 0x1E5FA}, {0x1E950, 0x1E959},
    {0x1FBF0, 0x1FBF9},
};

bool InRanges(const uint32_t (*r)[2], size_t n, uint32_t cp) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (cp < r[mid][0]) hi = mid;
        else if (cp > r[mid][1]) lo = mid + 1;
        else return true;
    }
    return false;
}

bool IsLetterCp(uint32_t cp) {
    if (cp < 0x80) return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || cp == '_';
    return InRanges(kLetterRanges, sizeof kLetterRanges / sizeof kLetterRanges[0], cp);
}

bool IsIdentPartCp(uint32_t cp) {
    if (cp < 0x80) return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || cp == '_';
    return InRanges(kLetterRanges, sizeof kLetterRanges / sizeof kLetterRanges[0], cp) ||
           InRanges(kDigitRanges, sizeof kDigitRanges / sizeof kDigitRanges[0], cp);
}

enum LexState { YYINITIAL, STRING_PREFIX, STRING, RAW_STRING, SHORT_TEMPLATE_ENTRY, BLOCK_COMMENT_ST,
                DOC_COMMENT_ST, LONG_TEMPLATE_ENTRY, UNMATCHED_BACKTICK };

struct SavedState {
    LexState state;
    int lbrace;
    int required;
};

bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsDigitOrUnderscore(char c) { return IsDigit(c) || c == '_'; }
bool IsHexOrUnderscore(char c) {
    return IsDigitOrUnderscore(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
bool IsHex(char c) { return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

struct KeywordEntry {
    std::string_view text;
    KtType type;
};

// Kotlin.flex's hard keywords (the soft ones lex as IDENTIFIER).
const KeywordEntry kKeywords[] = {
    {"typealias", KtType::TYPE_ALIAS_KEYWORD}, {"interface", KtType::INTERFACE_KEYWORD},
    {"continue", KtType::CONTINUE_KEYWORD},    {"package", KtType::PACKAGE_KEYWORD},
    {"return", KtType::RETURN_KEYWORD},        {"object", KtType::OBJECT_KEYWORD},
    {"while", KtType::WHILE_KEYWORD},          {"break", KtType::BREAK_KEYWORD},
    {"class", KtType::CLASS_KEYWORD},          {"throw", KtType::THROW_KEYWORD},
    {"false", KtType::FALSE_KEYWORD},          {"super", KtType::SUPER_KEYWORD},
    {"typeof", KtType::TYPEOF_KEYWORD},        {"when", KtType::WHEN_KEYWORD},
    {"true", KtType::TRUE_KEYWORD},            {"this", KtType::THIS_KEYWORD},
    {"null", KtType::NULL_KEYWORD},            {"else", KtType::ELSE_KEYWORD},
    {"try", KtType::TRY_KEYWORD},              {"val", KtType::VAL_KEYWORD},
    {"var", KtType::VAR_KEYWORD},              {"fun", KtType::FUN_KEYWORD},
    {"for", KtType::FOR_KEYWORD},              {"is", KtType::IS_KEYWORD},
    {"in", KtType::IN_KEYWORD},                {"if", KtType::IF_KEYWORD},
    {"do", KtType::DO_KEYWORD},                {"as", KtType::AS_KEYWORD},
};

class Lexer {
public:
    explicit Lexer(std::string_view text) : t_(text) {}

    std::vector<KtLexeme> Run() {
        std::vector<KtLexeme> out;
        out.reserve(t_.size() / 3 + 16);
        while (true) {
            size_t start = 0;
            KtType type = Next(&start);
            if (type == KtType::NONE) break;
            out.push_back({type, static_cast<uint32_t>(start)});
        }
        out.push_back({KtType::NONE, static_cast<uint32_t>(t_.size())});
        return out;
    }

private:
    std::string_view t_;
    size_t pos_ = 0;
    LexState state_ = YYINITIAL;
    std::vector<SavedState> stack_;
    int lbrace_ = 0;
    int required_ = -1;
    int comment_depth_ = 0;
    size_t comment_start_ = 0;

    char At(size_t i) const { return i < t_.size() ? t_[i] : '\0'; }
    bool Has(size_t i) const { return i < t_.size(); }

    // Decodes one code point at i; *len gets its byte length (1 for a stray byte).
    uint32_t Cp(size_t i, size_t *len) const {
        unsigned char c = static_cast<unsigned char>(t_[i]);
        if (c < 0x80) {
            *len = 1;
            return c;
        }
        size_t n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (n == 1 || i + n > t_.size()) {
            *len = 1;
            return 0xFFFD;
        }
        uint32_t cp = n == 2 ? (c & 0x1Fu) : n == 3 ? (c & 0x0Fu) : (c & 0x07u);
        for (size_t k = 1; k < n; k++) {
            unsigned char cc = static_cast<unsigned char>(t_[i + k]);
            if ((cc & 0xC0) != 0x80) {
                *len = 1;
                return 0xFFFD;
            }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        *len = n;
        return cp;
    }

    size_t CpLen(size_t i) const {
        size_t len = 1;
        Cp(i, &len);
        return len;
    }

    void PushState(LexState s) {
        stack_.push_back({state_, lbrace_, required_});
        lbrace_ = 0;
        required_ = -1;
        state_ = s;
    }

    void PushInterpolationPrefix(int prefix) {
        stack_.push_back({state_, lbrace_, required_});
        lbrace_ = 0;
        required_ = prefix;
        state_ = STRING_PREFIX;
    }

    void PopState() {
        if (stack_.empty()) {  // cannot happen with Kotlin.flex's rules; stay safe
            state_ = YYINITIAL;
            lbrace_ = 0;
            required_ = -1;
            return;
        }
        SavedState s = stack_.back();
        stack_.pop_back();
        lbrace_ = s.lbrace;
        required_ = s.required;
        state_ = s.state;
    }

    // Length of {PLAIN_IDENTIFIER} at i (0 if none).
    size_t PlainIdentLen(size_t i) const {
        if (!Has(i)) return 0;
        size_t len = 0;
        uint32_t cp = Cp(i, &len);
        if (!IsLetterCp(cp)) return 0;
        size_t j = i + len;
        while (Has(j)) {
            cp = Cp(j, &len);
            if (!IsIdentPartCp(cp)) break;
            j += len;
        }
        return j - i;
    }

    // Length of {ESCAPED_IDENTIFIER} = `[^`\n]+` at i (0 if none).
    size_t EscapedIdentLen(size_t i) const {
        if (At(i) != '`') return 0;
        size_t j = i + 1;
        while (Has(j) && t_[j] != '`' && t_[j] != '\n') j++;
        if (j == i + 1 || At(j) != '`') return 0;
        return j + 1 - i;
    }

    size_t IdentLen(size_t i) const {
        size_t n = PlainIdentLen(i);
        return n ? n : EscapedIdentLen(i);
    }

    // {INTEGER_LITERAL} at i (0 if none).
    size_t IntegerLen(size_t i) const {
        if (!IsDigit(At(i))) return 0;
        auto suffix = [&](size_t j) {
            if (At(j) == 'u' || At(j) == 'U') j++;
            if (At(j) == 'l' || At(j) == 'L') j++;
            return j;
        };
        size_t j = i + 1;
        while (IsDigitOrUnderscore(At(j))) j++;
        size_t best = suffix(j) - i;
        if (At(i) == '0' && (At(i + 1) == 'x' || At(i + 1) == 'X')) {
            size_t k = i + 2;
            while (IsHexOrUnderscore(At(k))) k++;
            best = std::max(best, suffix(k) - i);
        }
        if (At(i) == '0' && (At(i + 1) == 'b' || At(i + 1) == 'B')) {
            size_t k = i + 2;
            while (IsDigitOrUnderscore(At(k))) k++;
            best = std::max(best, suffix(k) - i);
        }
        return best;
    }

    // {DOUBLE_LITERAL} at i (0 if none).
    size_t DoubleLen(size_t i) const {
        size_t best = 0;
        size_t r = 0;  // {DIGITS} run at i
        if (IsDigit(At(i))) {
            r = 1;
            while (IsDigitOrUnderscore(At(i + r))) r++;
        }
        auto exponent = [&](size_t j) -> size_t {  // end of {EXPONENT_PART} at j, or j if none
            if (At(j) != 'e' && At(j) != 'E') return j;
            j++;
            if (At(j) == '+' || At(j) == '-') j++;
            while (IsDigitOrUnderscore(At(j))) j++;
            return j;
        };
        // {DIGITS}? "." {DIGITS} {EXPONENT_PART}? [Ff]?
        size_t d = i + r;
        if (At(d) == '.' && IsDigit(At(d + 1))) {
            size_t j = d + 2;
            while (IsDigitOrUnderscore(At(j))) j++;
            j = exponent(j);
            if (At(j) == 'f' || At(j) == 'F') j++;
            best = std::max(best, j - i);
        }
        // {DIGITS} ({EXPONENT_PART} [Ff]? | [Ff])
        if (r) {
            size_t j = i + r;
            size_t e = exponent(j);
            if (e != j) {
                if (At(e) == 'f' || At(e) == 'F') e++;
                best = std::max(best, e - i);
            } else if (At(j) == 'f' || At(j) == 'F') {
                best = std::max(best, j + 1 - i);
            }
        }
        return best;
    }

    // ESCAPE_SEQUENCE = \\(u{HEX}{4}|[^\n]) at i (0 if none).
    size_t EscapeLen(size_t i) const {
        if (At(i) != '\\' || !Has(i + 1) || t_[i + 1] == '\n') return 0;
        if (t_[i + 1] == 'u' && IsHex(At(i + 2)) && IsHex(At(i + 3)) && IsHex(At(i + 4)) && IsHex(At(i + 5)))
            return 6;
        return 1 + CpLen(i + 1);
    }

    // The dollar rules shared by STRING and RAW_STRING ({SHORT_TEMPLATE_ENTRY},
    // {LONELY_DOLLAR}, {LONG_TEMPLATE_ENTRY_START}), at a '$'.
    KtType Dollar(size_t start) {
        size_t k = 0;
        while (At(start + k) == '$') k++;
        int prefix = static_cast<int>(k);
        size_t ident = IdentLen(start + k);
        if (ident) {  // {SHORT_TEMPLATE_ENTRY}
            if (prefix == required_) {
                PushState(SHORT_TEMPLATE_ENTRY);
                pos_ = start + k;
                return KtType::SHORT_TEMPLATE_ENTRY_START;
            }
            if (prefix < required_) {
                pos_ = start + k;
                return KtType::REGULAR_STRING_PART;
            }
            pos_ = start + k - static_cast<size_t>(required_);
            return KtType::REGULAR_STRING_PART;
        }
        if (At(start + k) == '{') {  // {LONG_TEMPLATE_ENTRY_START}
            if (prefix == required_) {
                PushState(LONG_TEMPLATE_ENTRY);
                pos_ = start + k + 1;
                return KtType::LONG_TEMPLATE_ENTRY_START;
            }
            if (prefix < required_) {
                pos_ = start + k;
                return KtType::REGULAR_STRING_PART;
            }
            pos_ = start + k - static_cast<size_t>(required_);
            return KtType::REGULAR_STRING_PART;
        }
        pos_ = start + k;  // {LONELY_DOLLAR}
        return KtType::REGULAR_STRING_PART;
    }

    // One token; *start gets its start. NONE at the end of input.
    KtType Next(size_t *start) {
        while (true) {
            size_t s = pos_;
            *start = s;
            switch (state_) {
            case BLOCK_COMMENT_ST:
            case DOC_COMMENT_ST: {
                // "/*" nests, "*/" closes, [\s\S] continues; <<EOF>> closes.
                while (true) {
                    if (!Has(pos_)) {
                        KtType t = state_ == DOC_COMMENT_ST ? KtType::DOC_COMMENT : KtType::BLOCK_COMMENT;
                        PopState();
                        *start = comment_start_;
                        if (comment_start_ == pos_) return KtType::NONE;
                        return t;
                    }
                    if (t_[pos_] == '/' && At(pos_ + 1) == '*') {
                        comment_depth_++;
                        pos_ += 2;
                    } else if (t_[pos_] == '*' && At(pos_ + 1) == '/') {
                        pos_ += 2;
                        if (comment_depth_ > 0) {
                            comment_depth_--;
                        } else {
                            KtType t = state_ == DOC_COMMENT_ST ? KtType::DOC_COMMENT : KtType::BLOCK_COMMENT;
                            PopState();
                            *start = comment_start_;
                            return t;
                        }
                    } else {
                        pos_ += CpLen(pos_);
                    }
                }
            }
            case STRING_PREFIX:
                if (!Has(s)) return KtType::NONE;
                if (t_[s] == '"' && At(s + 1) == '"' && At(s + 2) == '"') {
                    state_ = RAW_STRING;
                    pos_ = s + 3;
                    return KtType::OPEN_QUOTE;
                }
                if (t_[s] == '"') {
                    state_ = STRING;
                    pos_ = s + 1;
                    return KtType::OPEN_QUOTE;
                }
                pos_ = s + CpLen(s);  // unreachable: entered only at a quote
                return KtType::BAD_CHARACTER;
            case STRING: {
                if (!Has(s)) return KtType::NONE;
                char c = t_[s];
                if (c == '\n') {
                    PopState();
                    return KtType::DANGLING_NEWLINE;  // zero-width: the \n is lexed again
                }
                if (c == '"') {
                    PopState();
                    pos_ = s + 1;
                    return KtType::CLOSING_QUOTE;
                }
                if (c == '\\') {
                    size_t e = EscapeLen(s);
                    if (e) {
                        pos_ = s + e;
                        return KtType::ESCAPE_SEQUENCE;
                    }
                    pos_ = s + 1;
                    return KtType::BAD_CHARACTER;
                }
                if (c == '$') return Dollar(s);
                size_t j = s;
                while (Has(j) && t_[j] != '\\' && t_[j] != '"' && t_[j] != '\n' && t_[j] != '$') j++;
                pos_ = j;
                return KtType::REGULAR_STRING_PART;
            }
            case RAW_STRING: {
                if (!Has(s)) return KtType::NONE;
                char c = t_[s];
                if (c == '\n' || c == '\\') {
                    pos_ = s + 1;
                    return KtType::REGULAR_STRING_PART;
                }
                if (c == '"') {
                    size_t q = 0;
                    while (At(s + q) == '"') q++;
                    if (q >= 3) {
                        if (q == 3) {
                            PopState();
                            pos_ = s + 3;
                            return KtType::CLOSING_QUOTE;
                        }
                        pos_ = s + q - 3;
                        return KtType::REGULAR_STRING_PART;
                    }
                    pos_ = s + 1;
                    return KtType::REGULAR_STRING_PART;
                }
                if (c == '$') return Dollar(s);
                size_t j = s;
                while (Has(j) && t_[j] != '\\' && t_[j] != '"' && t_[j] != '\n' && t_[j] != '$') j++;
                pos_ = j;
                return KtType::REGULAR_STRING_PART;
            }
            case SHORT_TEMPLATE_ENTRY: {
                if (!Has(s)) return KtType::NONE;
                size_t n = IdentLen(s);
                if (n) {
                    pos_ = s + n;
                    PopState();
                    return t_.substr(s, n) == "this" ? KtType::THIS_KEYWORD : KtType::IDENTIFIER;
                }
                pos_ = s + (t_[s] == '\n' ? 1 : CpLen(s));
                return KtType::BAD_CHARACTER;
            }
            case YYINITIAL:
            case LONG_TEMPLATE_ENTRY:
            case UNMATCHED_BACKTICK: {
                if (!Has(s)) return KtType::NONE;
                bool ok = true;
                KtType t = Initial(s, &ok);
                if (ok) return t;
                continue;  // an action without a return: keep scanning from pos_
            }
            }
        }
    }

    KtType Op(size_t s, size_t len, KtType t) {
        pos_ = s + len;
        return t;
    }

    // The rules of the inclusive states. *returned is false for an action that does not return.
    KtType Initial(size_t s, bool *returned) {
        char c = t_[s];
        char c1 = At(s + 1), c2 = At(s + 2);
        // {INTERPOLATION}? \"
        if (c == '"' || c == '$') {
            size_t k = 0;
            while (At(s + k) == '$') k++;
            if (At(s + k) == '"') {
                PushInterpolationPrefix(std::max(static_cast<int>(k), 1));
                pos_ = s + k;
                if (k != 0) return KtType::INTERPOLATION_PREFIX;
                *returned = false;
                return KtType::NONE;
            }
            if (c == '$') {
                size_t n = IdentLen(s + 1);
                if (n) return Op(s, 1 + n, KtType::FIELD_IDENTIFIER);
                return Op(s, 1, KtType::BAD_CHARACTER);
            }
        }
        if (state_ == LONG_TEMPLATE_ENTRY) {
            if (c == '{') {
                lbrace_++;
                return Op(s, 1, KtType::LBRACE);
            }
            if (c == '}') {
                if (lbrace_ == 0) {
                    PopState();
                    return Op(s, 1, KtType::LONG_TEMPLATE_ENTRY_END);
                }
                lbrace_--;
                return Op(s, 1, KtType::RBRACE);
            }
        }
        if (c == '/') {
            if (c1 == '*' && c2 == '*' && At(s + 3) == '/') return Op(s, 4, KtType::BLOCK_COMMENT);
            if (c1 == '*') {
                bool doc = c2 == '*';
                PushState(doc ? DOC_COMMENT_ST : BLOCK_COMMENT_ST);
                comment_depth_ = 0;
                comment_start_ = s;
                pos_ = s + (doc ? 3 : 2);
                *returned = false;
                return KtType::NONE;
            }
            if (c1 == '/') {
                size_t j = s;
                while (Has(j) && t_[j] != '\n') j++;
                return Op(s, j - s, KtType::EOL_COMMENT);
            }
            if (c1 == '=') return Op(s, 2, KtType::DIVEQ);
            return Op(s, 1, KtType::DIV);
        }
        if (c == ' ' || c == '\n' || c == '\t' || c == '\f') {
            size_t j = s;
            while (Has(j) && (t_[j] == ' ' || t_[j] == '\n' || t_[j] == '\t' || t_[j] == '\f')) j++;
            return Op(s, j - s, KtType::WHITE_SPACE);
        }
        if (c == '#') {
            if (c1 == '!') {
                if (s == 0) {
                    size_t j = s;
                    while (Has(j) && t_[j] != '\n') j++;
                    return Op(s, j - s, KtType::SHEBANG_COMMENT);
                }
            }
            return Op(s, 1, KtType::HASH);
        }
        if (IsDigit(c) || (c == '.' && IsDigit(c1))) {
            size_t il = IntegerLen(s);
            size_t dl = DoubleLen(s);
            size_t idd = (il && At(s + il) == '.' && At(s + il + 1) == '.') ? il + 2 : 0;
            if (idd > dl && idd >= il) return Op(s, il, KtType::INTEGER_LITERAL);  // {INTEGER_LITERAL}\.\.
            if (il >= dl && il > 0) return Op(s, il, KtType::INTEGER_LITERAL);
            if (dl) return Op(s, dl, KtType::FLOAT_LITERAL);
        }
        if (c == '\'') {
            // "'"([^\\\'\n]|{ESCAPE_SEQUENCE})*("'"|\\)?
            size_t j = s + 1;
            while (Has(j)) {
                char d = t_[j];
                if (d == '\n') break;
                if (d == '\'') {
                    j++;
                    break;
                }
                if (d == '\\') {
                    size_t e = EscapeLen(j);
                    if (e) {
                        j += e;
                        continue;
                    }
                    j++;  // the closing \\ alternative
                    break;
                }
                j += CpLen(j);
            }
            return Op(s, j - s, KtType::CHARACTER_LITERAL);
        }
        {
            size_t n = PlainIdentLen(s);
            if (n) {
                std::string_view w = t_.substr(s, n);
                if (w == "as" && At(s + 2) == '?') return Op(s, 3, KtType::AS_SAFE);
                for (const KeywordEntry &k : kKeywords)
                    if (k.text == w) return Op(s, n, k.type);
                return Op(s, n, KtType::IDENTIFIER);
            }
        }
        if (c == '`') {
            size_t n = EscapedIdentLen(s);
            if (n) return Op(s, n, KtType::IDENTIFIER);
            PushState(UNMATCHED_BACKTICK);
            return Op(s, 1, KtType::BAD_CHARACTER);
        }
        switch (c) {
        case '!':
            if ((c1 == 'i' && (c2 == 'n' || c2 == 's')) && Has(s + 3)) {
                size_t len = 0;
                uint32_t cp = Cp(s + 3, &len);
                if (IsIdentPartCp(cp)) return Op(s, 1, KtType::EXCL);  // \!in{IDENTIFIER_PART}
            }
            if (c1 == '=' && c2 == '=') return Op(s, 3, KtType::EXCLEQEQEQ);
            if (c1 == 'i' && c2 == 'n') return Op(s, 3, KtType::NOT_IN);
            if (c1 == 'i' && c2 == 's') return Op(s, 3, KtType::NOT_IS);
            if (c1 == '=') return Op(s, 2, KtType::EXCLEQ);
            return Op(s, 1, KtType::EXCL);
        case '.':
            if (c1 == '.' && c2 == '.') return Op(s, 3, KtType::RESERVED);
            if (c1 == '.' && c2 == '<') return Op(s, 3, KtType::RANGE_UNTIL);
            if (c1 == '.') return Op(s, 2, KtType::RANGE);
            return Op(s, 1, KtType::DOT);
        case '=':
            if (c1 == '=' && c2 == '=') return Op(s, 3, KtType::EQEQEQ);
            if (c1 == '=') return Op(s, 2, KtType::EQEQ);
            if (c1 == '>') return Op(s, 2, KtType::DOUBLE_ARROW);
            return Op(s, 1, KtType::EQ);
        case '+':
            if (c1 == '+') return Op(s, 2, KtType::PLUSPLUS);
            if (c1 == '=') return Op(s, 2, KtType::PLUSEQ);
            return Op(s, 1, KtType::PLUS);
        case '-':
            if (c1 == '-') return Op(s, 2, KtType::MINUSMINUS);
            if (c1 == '=') return Op(s, 2, KtType::MINUSEQ);
            if (c1 == '>') return Op(s, 2, KtType::ARROW);
            return Op(s, 1, KtType::MINUS);
        case '<':
            if (c1 == '=') return Op(s, 2, KtType::LTEQ);
            return Op(s, 1, KtType::LT);
        case '>':
            if (c1 == '=') return Op(s, 2, KtType::GTEQ);
            return Op(s, 1, KtType::GT);
        case '&':
            if (c1 == '&') return Op(s, 2, KtType::ANDAND);
            return Op(s, 1, KtType::AND);
        case '|':
            if (c1 == '|') return Op(s, 2, KtType::OROR);
            break;
        case '*':
            if (c1 == '=') return Op(s, 2, KtType::MULTEQ);
            return Op(s, 1, KtType::MUL);
        case '%':
            if (c1 == '=') return Op(s, 2, KtType::PERCEQ);
            return Op(s, 1, KtType::PERC);
        case ':':
            if (c1 == ':') return Op(s, 2, KtType::COLONCOLON);
            return Op(s, 1, KtType::COLON);
        case ';':
            if (c1 == ';') return Op(s, 2, KtType::DOUBLE_SEMICOLON);
            return Op(s, 1, KtType::SEMICOLON);
        case '[': return Op(s, 1, KtType::LBRACKET);
        case ']': return Op(s, 1, KtType::RBRACKET);
        case '{': return Op(s, 1, KtType::LBRACE);
        case '}': return Op(s, 1, KtType::RBRACE);
        case '(': return Op(s, 1, KtType::LPAR);
        case ')': return Op(s, 1, KtType::RPAR);
        case '?': return Op(s, 1, KtType::QUEST);
        case ',': return Op(s, 1, KtType::COMMA);
        case '@': return Op(s, 1, KtType::AT);
        default: break;
        }
        return Op(s, CpLen(s), KtType::BAD_CHARACTER);  // [\s\S]
    }
};

}  // namespace

std::vector<KtLexeme> KtLex(std::string_view text) { return Lexer(text).Run(); }

}  // namespace kt
