#include "pdf_encodings.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace pdfenc {

namespace {

// clang-format off

// StandardEncoding (PDF spec Annex D.2) -- the old Adobe/PostScript
// default. Sparse above 126: gaps are genuinely undefined in this
// encoding, not an omission.
const char *const kStandard[256] = {
    /*0x00*/ nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
    /*0x08*/ nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
    /*0x10*/ nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
    /*0x18*/ nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
    /*0x20*/ "space","exclam","quotedbl","numbersign","dollar","percent","ampersand","quoteright",
    /*0x28*/ "parenleft","parenright","asterisk","plus","comma","hyphen","period","slash",
    /*0x30*/ "zero","one","two","three","four","five","six","seven",
    /*0x38*/ "eight","nine","colon","semicolon","less","equal","greater","question",
    /*0x40*/ "at","A","B","C","D","E","F","G",
    /*0x48*/ "H","I","J","K","L","M","N","O",
    /*0x50*/ "P","Q","R","S","T","U","V","W",
    /*0x58*/ "X","Y","Z","bracketleft","backslash","bracketright","asciicircum","underscore",
    /*0x60*/ "quoteleft","a","b","c","d","e","f","g",
    /*0x68*/ "h","i","j","k","l","m","n","o",
    /*0x70*/ "p","q","r","s","t","u","v","w",
    /*0x78*/ "x","y","z","braceleft","bar","braceright","asciitilde",nullptr,
    /*0x80*/ nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
    /*0x88*/ nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
    /*0x90*/ nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
    /*0x98*/ nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
    /*0xA0*/ nullptr,"exclamdown","cent","sterling","fraction","yen","florin","section",
    /*0xA8*/ "currency","quotesingle","quotedblleft","guillemotleft","guilsinglleft","guilsinglright","fi","fl",
    /*0xB0*/ nullptr,"endash","dagger","daggerdbl","periodcentered",nullptr,"paragraph","bullet",
    /*0xB8*/ "quotesinglbase","quotedblbase","quotedblright","guillemotright","ellipsis","perthousand",nullptr,"questiondown",
    /*0xC0*/ nullptr,"grave","acute","circumflex","tilde","macron","breve","dotaccent",
    /*0xC8*/ "dieresis",nullptr,"ring","cedilla",nullptr,"hungarumlaut","ogonek","caron",
    /*0xD0*/ "emdash",nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
    /*0xD8*/ nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
    /*0xE0*/ nullptr,"AE",nullptr,"ordfeminine",nullptr,nullptr,nullptr,nullptr,
    /*0xE8*/ "Lslash","Oslash","OE","ordmasculine",nullptr,nullptr,nullptr,nullptr,
    /*0xF0*/ nullptr,"ae",nullptr,nullptr,nullptr,"dotlessi",nullptr,nullptr,
    /*0xF8*/ "lslash","oslash","oe","germandbls",nullptr,nullptr,nullptr,nullptr,
};

// WinAnsiEncoding (PDF spec Annex D.2) -- matches Windows code page 1252
// for the printable range; code 0xA0 is explicitly specified as
// "space" (not nbsp) per the PDF spec table itself, a well-known quirk.
const char *const kWinAnsi[256] = {
    /*0x00*/ nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
    /*0x08*/ nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
    /*0x10*/ nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
    /*0x18*/ nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
    /*0x20*/ "space","exclam","quotedbl","numbersign","dollar","percent","ampersand","quotesingle",
    /*0x28*/ "parenleft","parenright","asterisk","plus","comma","hyphen","period","slash",
    /*0x30*/ "zero","one","two","three","four","five","six","seven",
    /*0x38*/ "eight","nine","colon","semicolon","less","equal","greater","question",
    /*0x40*/ "at","A","B","C","D","E","F","G",
    /*0x48*/ "H","I","J","K","L","M","N","O",
    /*0x50*/ "P","Q","R","S","T","U","V","W",
    /*0x58*/ "X","Y","Z","bracketleft","backslash","bracketright","asciicircum","underscore",
    /*0x60*/ "grave","a","b","c","d","e","f","g",
    /*0x68*/ "h","i","j","k","l","m","n","o",
    /*0x70*/ "p","q","r","s","t","u","v","w",
    /*0x78*/ "x","y","z","braceleft","bar","braceright","asciitilde",nullptr,
    /*0x80*/ "Euro",nullptr,"quotesinglbase","florin","quotedblbase","ellipsis","dagger","daggerdbl",
    /*0x88*/ "circumflex","perthousand","Scaron","guilsinglleft","OE",nullptr,"Zcaron",nullptr,
    /*0x90*/ nullptr,"quoteleft","quoteright","quotedblleft","quotedblright","bullet","endash","emdash",
    /*0x98*/ "tilde","trademark","scaron","guilsinglright","oe",nullptr,"zcaron","Ydieresis",
    /*0xA0*/ "space","exclamdown","cent","sterling","currency","yen","brokenbar","section",
    /*0xA8*/ "dieresis","copyright","ordfeminine","guillemotleft","logicalnot","hyphen","registered","macron",
    /*0xB0*/ "degree","plusminus","twosuperior","threesuperior","acute","mu","paragraph","periodcentered",
    /*0xB8*/ "cedilla","onesuperior","ordmasculine","guillemotright","onequarter","onehalf","threequarters","questiondown",
    /*0xC0*/ "Agrave","Aacute","Acircumflex","Atilde","Adieresis","Aring","AE","Ccedilla",
    /*0xC8*/ "Egrave","Eacute","Ecircumflex","Edieresis","Igrave","Iacute","Icircumflex","Idieresis",
    /*0xD0*/ "Eth","Ntilde","Ograve","Oacute","Ocircumflex","Otilde","Odieresis","multiply",
    /*0xD8*/ "Oslash","Ugrave","Uacute","Ucircumflex","Udieresis","Yacute","Thorn","germandbls",
    /*0xE0*/ "agrave","aacute","acircumflex","atilde","adieresis","aring","ae","ccedilla",
    /*0xE8*/ "egrave","eacute","ecircumflex","edieresis","igrave","iacute","icircumflex","idieresis",
    /*0xF0*/ "eth","ntilde","ograve","oacute","ocircumflex","otilde","odieresis","divide",
    /*0xF8*/ "oslash","ugrave","uacute","ucircumflex","udieresis","yacute","thorn","ydieresis",
};

// MacRomanEncoding (PDF spec Annex D.2) -- matches classic Mac OS Roman.
const char *const kMacRoman[256] = {
    /*0x00*/ nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
    /*0x08*/ nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
    /*0x10*/ nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
    /*0x18*/ nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
    /*0x20*/ "space","exclam","quotedbl","numbersign","dollar","percent","ampersand","quotesingle",
    /*0x28*/ "parenleft","parenright","asterisk","plus","comma","hyphen","period","slash",
    /*0x30*/ "zero","one","two","three","four","five","six","seven",
    /*0x38*/ "eight","nine","colon","semicolon","less","equal","greater","question",
    /*0x40*/ "at","A","B","C","D","E","F","G",
    /*0x48*/ "H","I","J","K","L","M","N","O",
    /*0x50*/ "P","Q","R","S","T","U","V","W",
    /*0x58*/ "X","Y","Z","bracketleft","backslash","bracketright","asciicircum","underscore",
    /*0x60*/ "grave","a","b","c","d","e","f","g",
    /*0x68*/ "h","i","j","k","l","m","n","o",
    /*0x70*/ "p","q","r","s","t","u","v","w",
    /*0x78*/ "x","y","z","braceleft","bar","braceright","asciitilde",nullptr,
    /*0x80*/ "Adieresis","Aring","Ccedilla","Eacute","Ntilde","Odieresis","Udieresis","aacute",
    /*0x88*/ "agrave","acircumflex","adieresis","atilde","aring","ccedilla","eacute","egrave",
    /*0x90*/ "ecircumflex","edieresis","iacute","igrave","icircumflex","idieresis","ntilde","oacute",
    /*0x98*/ "ograve","ocircumflex","odieresis","otilde","uacute","ugrave","ucircumflex","udieresis",
    /*0xA0*/ "dagger","degree","cent","sterling","section","bullet","paragraph","germandbls",
    /*0xA8*/ "registered","copyright","trademark","acute","dieresis","notequal","AE","Oslash",
    /*0xB0*/ "infinity","plusminus","lessequal","greaterequal","yen","mu","partialdiff","summation",
    /*0xB8*/ "product","pi","integral","ordfeminine","ordmasculine","Omega","ae","oslash",
    /*0xC0*/ "questiondown","exclamdown","logicalnot","radical","florin","approxequal","Delta","guillemotleft",
    /*0xC8*/ "guillemotright","ellipsis","space","Agrave","Atilde","Otilde","OE","oe",
    /*0xD0*/ "endash","emdash","quotedblleft","quotedblright","quoteleft","quoteright","divide","lozenge",
    /*0xD8*/ "ydieresis","Ydieresis","fraction","Euro","guilsinglleft","guilsinglright","fi","fl",
    /*0xE0*/ "daggerdbl","periodcentered","quotesinglbase","quotedblbase","perthousand","Acircumflex","Ecircumflex","Aacute",
    /*0xE8*/ "Edieresis","Egrave","Iacute","Icircumflex","Idieresis","Igrave","Oacute","Ocircumflex",
    /*0xF0*/ nullptr,"Ograve","Uacute","Ucircumflex","Ugrave","dotlessi","circumflex","tilde",
    /*0xF8*/ "macron","breve","dotaccent","ring","cedilla","hungarumlaut","ogonek","caron",
};

// clang-format on

// Adobe-Glyph-List subset: every name the 3 tables above can produce,
// mapped to its Unicode codepoint -- built once, on first use.
const std::unordered_map<std::string, int> &GlyphList() {
    static const std::unordered_map<std::string, int> table = {
        {"space", 0x0020}, {"exclam", 0x0021}, {"quotedbl", 0x0022}, {"numbersign", 0x0023},
        {"dollar", 0x0024}, {"percent", 0x0025}, {"ampersand", 0x0026}, {"quoteright", 0x2019},
        {"quotesingle", 0x0027}, {"parenleft", 0x0028}, {"parenright", 0x0029}, {"asterisk", 0x002A},
        {"plus", 0x002B}, {"comma", 0x002C}, {"hyphen", 0x002D}, {"period", 0x002E}, {"slash", 0x002F},
        {"zero", 0x0030}, {"one", 0x0031}, {"two", 0x0032}, {"three", 0x0033}, {"four", 0x0034},
        {"five", 0x0035}, {"six", 0x0036}, {"seven", 0x0037}, {"eight", 0x0038}, {"nine", 0x0039},
        {"colon", 0x003A}, {"semicolon", 0x003B}, {"less", 0x003C}, {"equal", 0x003D}, {"greater", 0x003E},
        {"question", 0x003F}, {"at", 0x0040}, {"A", 0x0041}, {"B", 0x0042}, {"C", 0x0043}, {"D", 0x0044},
        {"E", 0x0045}, {"F", 0x0046}, {"G", 0x0047}, {"H", 0x0048}, {"I", 0x0049}, {"J", 0x004A},
        {"K", 0x004B}, {"L", 0x004C}, {"M", 0x004D}, {"N", 0x004E}, {"O", 0x004F}, {"P", 0x0050},
        {"Q", 0x0051}, {"R", 0x0052}, {"S", 0x0053}, {"T", 0x0054}, {"U", 0x0055}, {"V", 0x0056},
        {"W", 0x0057}, {"X", 0x0058}, {"Y", 0x0059}, {"Z", 0x005A}, {"bracketleft", 0x005B},
        {"backslash", 0x005C}, {"bracketright", 0x005D}, {"asciicircum", 0x005E}, {"underscore", 0x005F},
        {"quoteleft", 0x2018}, {"grave", 0x0060}, {"a", 0x0061}, {"b", 0x0062}, {"c", 0x0063},
        {"d", 0x0064}, {"e", 0x0065}, {"f", 0x0066}, {"g", 0x0067}, {"h", 0x0068}, {"i", 0x0069},
        {"j", 0x006A}, {"k", 0x006B}, {"l", 0x006C}, {"m", 0x006D}, {"n", 0x006E}, {"o", 0x006F},
        {"p", 0x0070}, {"q", 0x0071}, {"r", 0x0072}, {"s", 0x0073}, {"t", 0x0074}, {"u", 0x0075},
        {"v", 0x0076}, {"w", 0x0077}, {"x", 0x0078}, {"y", 0x0079}, {"z", 0x007A}, {"braceleft", 0x007B},
        {"bar", 0x007C}, {"braceright", 0x007D}, {"asciitilde", 0x007E},
        {"exclamdown", 0x00A1}, {"cent", 0x00A2}, {"sterling", 0x00A3}, {"fraction", 0x2044},
        {"yen", 0x00A5}, {"florin", 0x0192}, {"section", 0x00A7}, {"currency", 0x00A4},
        {"quotedblleft", 0x201C}, {"guillemotleft", 0x00AB}, {"guilsinglleft", 0x2039},
        {"guilsinglright", 0x203A}, {"fi", 0xFB01}, {"fl", 0xFB02}, {"endash", 0x2013},
        {"dagger", 0x2020}, {"daggerdbl", 0x2021}, {"periodcentered", 0x00B7}, {"paragraph", 0x00B6},
        {"bullet", 0x2022}, {"quotesinglbase", 0x201A}, {"quotedblbase", 0x201E}, {"quotedblright", 0x201D},
        {"guillemotright", 0x00BB}, {"ellipsis", 0x2026}, {"perthousand", 0x2030}, {"questiondown", 0x00BF},
        {"acute", 0x00B4}, {"circumflex", 0x02C6}, {"tilde", 0x02DC}, {"macron", 0x00AF},
        {"breve", 0x02D8}, {"dotaccent", 0x02D9}, {"dieresis", 0x00A8}, {"ring", 0x02DA},
        {"cedilla", 0x00B8}, {"hungarumlaut", 0x02DD}, {"ogonek", 0x02DB}, {"caron", 0x02C7},
        {"emdash", 0x2014}, {"AE", 0x00C6}, {"ordfeminine", 0x00AA}, {"Lslash", 0x0141},
        {"Oslash", 0x00D8}, {"OE", 0x0152}, {"ordmasculine", 0x00BA}, {"ae", 0x00E6},
        {"dotlessi", 0x0131}, {"lslash", 0x0142}, {"oslash", 0x00F8}, {"oe", 0x0153},
        {"germandbls", 0x00DF}, {"Euro", 0x20AC}, {"Scaron", 0x0160}, {"Zcaron", 0x017D},
        {"trademark", 0x2122}, {"scaron", 0x0161}, {"zcaron", 0x017E}, {"Ydieresis", 0x0178},
        {"brokenbar", 0x00A6}, {"copyright", 0x00A9}, {"logicalnot", 0x00AC}, {"registered", 0x00AE},
        {"degree", 0x00B0}, {"plusminus", 0x00B1}, {"twosuperior", 0x00B2}, {"threesuperior", 0x00B3},
        {"mu", 0x00B5}, {"onesuperior", 0x00B9}, {"onequarter", 0x00BC}, {"onehalf", 0x00BD},
        {"threequarters", 0x00BE}, {"Agrave", 0x00C0}, {"Aacute", 0x00C1}, {"Acircumflex", 0x00C2},
        {"Atilde", 0x00C3}, {"Adieresis", 0x00C4}, {"Aring", 0x00C5}, {"Ccedilla", 0x00C7},
        {"Egrave", 0x00C8}, {"Eacute", 0x00C9}, {"Ecircumflex", 0x00CA}, {"Edieresis", 0x00CB},
        {"Igrave", 0x00CC}, {"Iacute", 0x00CD}, {"Icircumflex", 0x00CE}, {"Idieresis", 0x00CF},
        {"Eth", 0x00D0}, {"Ntilde", 0x00D1}, {"Ograve", 0x00D2}, {"Oacute", 0x00D3},
        {"Ocircumflex", 0x00D4}, {"Otilde", 0x00D5}, {"Odieresis", 0x00D6}, {"multiply", 0x00D7},
        {"Ugrave", 0x00D9}, {"Uacute", 0x00DA}, {"Ucircumflex", 0x00DB}, {"Udieresis", 0x00DC},
        {"Yacute", 0x00DD}, {"Thorn", 0x00DE}, {"agrave", 0x00E0}, {"aacute", 0x00E1},
        {"acircumflex", 0x00E2}, {"atilde", 0x00E3}, {"adieresis", 0x00E4}, {"aring", 0x00E5},
        {"ccedilla", 0x00E7}, {"egrave", 0x00E8}, {"eacute", 0x00E9}, {"ecircumflex", 0x00EA},
        {"edieresis", 0x00EB}, {"igrave", 0x00EC}, {"iacute", 0x00ED}, {"icircumflex", 0x00EE},
        {"idieresis", 0x00EF}, {"eth", 0x00F0}, {"ntilde", 0x00F1}, {"ograve", 0x00F2},
        {"oacute", 0x00F3}, {"ocircumflex", 0x00F4}, {"otilde", 0x00F5}, {"odieresis", 0x00F6},
        {"divide", 0x00F7}, {"ugrave", 0x00F9}, {"uacute", 0x00FA}, {"ucircumflex", 0x00FB},
        {"udieresis", 0x00FC}, {"yacute", 0x00FD}, {"thorn", 0x00FE}, {"ydieresis", 0x00FF},
        // MacRoman-only symbol glyphs not already covered above.
        {"notequal", 0x2260}, {"infinity", 0x221E}, {"lessequal", 0x2264}, {"greaterequal", 0x2265},
        {"partialdiff", 0x2202}, {"summation", 0x2211}, {"product", 0x220F}, {"pi", 0x03C0},
        {"integral", 0x222B}, {"Omega", 0x03A9}, {"radical", 0x221A}, {"approxequal", 0x2248},
        {"Delta", 0x2206}, {"lozenge", 0x25CA},
        // Common ligatures seen in real /Differences arrays beyond the
        // base tables (e.g. Type1 fonts with a ligature-rich charset).
        {"ff", 0xFB00}, {"ffi", 0xFB03}, {"ffl", 0xFB04},
    };
    return table;
}

int ParseHexEscape(const std::string &name, size_t prefix_len) {
    if (name.size() <= prefix_len) return -1;
    size_t hex_len = name.size() - prefix_len;
    if (hex_len < 4 || hex_len > 6) return -1;
    for (size_t i = prefix_len; i < name.size(); ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(name[i]))) return -1;
    }
    return static_cast<int>(std::strtol(name.c_str() + prefix_len, nullptr, 16));
}

// clang-format off
const char *const kCffStandardStrings[] = {
    ".notdef","space","exclam","quotedbl","numbersign","dollar","percent","ampersand","quoteright",
    "parenleft","parenright","asterisk","plus","comma","hyphen","period","slash","zero","one","two",
    "three","four","five","six","seven","eight","nine","colon","semicolon","less","equal","greater",
    "question","at","A","B","C","D","E","F","G","H","I","J","K","L","M","N","O","P","Q","R","S","T",
    "U","V","W","X","Y","Z","bracketleft","backslash","bracketright","asciicircum","underscore",
    "quoteleft","a","b","c","d","e","f","g","h","i","j","k","l","m","n","o","p","q","r","s","t","u",
    "v","w","x","y","z","braceleft","bar","braceright","asciitilde","exclamdown","cent","sterling",
    "fraction","yen","florin","section","currency","quotesingle","quotedblleft","guillemotleft",
    "guilsinglleft","guilsinglright","fi","fl","endash","dagger","daggerdbl","periodcentered",
    "paragraph","bullet","quotesinglbase","quotedblbase","quotedblright","guillemotright","ellipsis",
    "perthousand","questiondown","grave","acute","circumflex","tilde","macron","breve","dotaccent",
    "dieresis","ring","cedilla","hungarumlaut","ogonek","caron","emdash","AE","ordfeminine","Lslash",
    "Oslash","OE","ordmasculine","ae","dotlessi","lslash","oslash","oe","germandbls","onesuperior",
    "logicalnot","mu","trademark","Eth","onehalf","plusminus","Thorn","onequarter","divide",
    "brokenbar","degree","thorn","threequarters","twosuperior","registered","minus","eth","multiply",
    "threesuperior","copyright","Aacute","Acircumflex","Adieresis","Agrave","Aring","Atilde",
    "Ccedilla","Eacute","Ecircumflex","Edieresis","Egrave","Iacute","Icircumflex","Idieresis",
    "Igrave","Ntilde","Oacute","Ocircumflex","Odieresis","Ograve","Otilde","Scaron","Uacute",
    "Ucircumflex","Udieresis","Ugrave","Yacute","Ydieresis","Zcaron","aacute","acircumflex",
    "adieresis","agrave","aring","atilde","ccedilla","eacute","ecircumflex","edieresis","egrave",
    "iacute","icircumflex","idieresis","igrave","ntilde","oacute","ocircumflex","odieresis","ograve",
    "otilde","scaron","uacute","ucircumflex","udieresis","ugrave","yacute","ydieresis","zcaron",
    "exclamsmall","Hungarumlautsmall","dollaroldstyle","dollarsuperior","ampersandsmall",
    "Acutesmall","parenleftsuperior","parenrightsuperior","twodotenleader","onedotenleader",
    "zerooldstyle","oneoldstyle","twooldstyle","threeoldstyle","fouroldstyle","fiveoldstyle",
    "sixoldstyle","sevenoldstyle","eightoldstyle","nineoldstyle","commasuperior",
    "threequartersemdash","periodsuperior","questionsmall","asuperior","bsuperior","centsuperior",
    "dsuperior","esuperior","isuperior","lsuperior","msuperior","nsuperior","osuperior","rsuperior",
    "ssuperior","tsuperior","ff","fi","fl","ffi","ffl","parenleftinferior","parenrightinferior",
    "Circumflexsmall","hyphensuperior","Gravesmall","Asmall","Bsmall","Csmall","Dsmall","Esmall",
    "Fsmall","Gsmall","Hsmall","Ismall","Jsmall","Ksmall","Lsmall","Msmall","Nsmall","Osmall",
    "Psmall","Qsmall","Rsmall","Ssmall","Tsmall","Usmall","Vsmall","Wsmall","Xsmall","Ysmall",
    "Zsmall","colonmonetary","onefitted","rupiah","Tildesmall","exclamdownsmall","centoldstyle",
    "Lslashsmall","Scaronsmall","Zcaronsmall","Dieresissmall","Brevesmall","Caronsmall",
    "Dotaccentsmall","Macronsmall","figuredash","hypheninferior","Ogoneksmall","Ringsmall",
    "Cedillasmall","questiondownsmall","oneeighth","threeeighths","fiveeighths","seveneighths",
    "onethird","twothirds","zerosuperior","foursuperior","fivesuperior","sixsuperior",
    "sevensuperior","eightsuperior","ninesuperior","zeroinferior","oneinferior","twoinferior",
    "threeinferior","fourinferior","fiveinferior","sixinferior","seveninferior","eightinferior",
    "nineinferior","centinferior","dollarinferior","periodinferior","commainferior","Agravesmall",
    "Aacutesmall","Acircumflexsmall","Atildesmall","Adieresissmall","Aringsmall","AEsmall",
    "Ccedillasmall","Egravesmall","Eacutesmall","Ecircumflexsmall","Edieresissmall","Igravesmall",
    "Iacutesmall","Icircumflexsmall","Idieresissmall","Ethsmall","Ntildesmall","Ogravesmall",
    "Oacutesmall","Ocircumflexsmall","Otildesmall","Odieresissmall","OEsmall","Oslashsmall",
    "Ugravesmall","Uacutesmall","Ucircumflexsmall","Udieresissmall","Yacutesmall","Thornsmall",
    "Ydieresissmall","001.000","001.001","001.002","001.003","Black","Bold","Book","Light",
    "Medium","Regular","Roman","Semibold",
};
// clang-format on

}  // namespace

const char *EncodingName(Base base, int code) {
    if (code < 0 || code > 255) return nullptr;
    switch (base) {
        case Base::kStandard:
            return kStandard[code];
        case Base::kWinAnsi:
            return kWinAnsi[code];
        case Base::kMacRoman:
            return kMacRoman[code];
    }
    return nullptr;
}

int GlyphNameToUnicode(const std::string &name) {
    const auto &table = GlyphList();
    auto it = table.find(name);
    if (it != table.end()) return it->second;
    if (name.size() > 3 && name.compare(0, 3, "uni") == 0) {
        int v = ParseHexEscape(name, 3);
        if (v >= 0) return v;
    }
    if (name.size() > 1 && name[0] == 'u') {
        int v = ParseHexEscape(name, 1);
        if (v >= 0) return v;
    }
    return -1;
}

const char *CffStandardString(int sid) {
    if (sid < 0 || sid >= CffStandardStringCount()) return nullptr;
    return kCffStandardStrings[sid];
}

int CffStandardStringCount() { return static_cast<int>(sizeof(kCffStandardStrings) / sizeof(kCffStandardStrings[0])); }

}  // namespace pdfenc
