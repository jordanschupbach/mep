// Port of acorn 8.15.0 (MIT): the RegExp literal validator (regexp.js and
// unicode-property-data.js). Integer values are doubles where acorn's
// numbers can grow past 2^31 (decimal and hex digit runs).

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "js_parse_parser.h"

namespace jsparse {

namespace {

const char kScriptValuesAddedInUnicode[] =
    "Gara Garay Gukh Gurung_Khema Hrkt Katakana_Or_Hiragana Kawi Kirat_Rai Krai Nag_Mundari Nagm Ol_Onal Onao Sunu "
    "Sunuwar Todhri Todr Tulu_Tigalari Tutg Unknown Zzzz";

const char kEcma9BinaryProperties[] =
    "ASCII ASCII_Hex_Digit AHex Alphabetic Alpha Any Assigned Bidi_Control Bidi_C Bidi_Mirrored Bidi_M Case_Ignorable "
    "CI Cased Changes_When_Casefolded CWCF Changes_When_Casemapped CWCM Changes_When_Lowercased CWL "
    "Changes_When_NFKC_Casefolded CWKCF Changes_When_Titlecased CWT Changes_When_Uppercased CWU Dash "
    "Default_Ignorable_Code_Point DI Deprecated Dep Diacritic Dia Emoji Emoji_Component Emoji_Modifier "
    "Emoji_Modifier_Base Emoji_Presentation Extender Ext Grapheme_Base Gr_Base Grapheme_Extend Gr_Ext Hex_Digit Hex "
    "IDS_Binary_Operator IDSB IDS_Trinary_Operator IDST ID_Continue IDC ID_Start IDS Ideographic Ideo Join_Control "
    "Join_C Logical_Order_Exception LOE Lowercase Lower Math Noncharacter_Code_Point NChar Pattern_Syntax Pat_Syn "
    "Pattern_White_Space Pat_WS Quotation_Mark QMark Radical Regional_Indicator RI Sentence_Terminal STerm Soft_Dotted "
    "SD Terminal_Punctuation Term Unified_Ideograph UIdeo Uppercase Upper Variation_Selector VS White_Space space "
    "XID_Continue XIDC XID_Start XIDS";
const char kEcma10BinaryAdded[] = "Extended_Pictographic";
const char kEcma12BinaryAdded[] = "EBase EComp EMod EPres ExtPict";

const char kEcma14BinaryPropertiesOfStrings[] =
    "Basic_Emoji Emoji_Keycap_Sequence RGI_Emoji_Modifier_Sequence RGI_Emoji_Flag_Sequence RGI_Emoji_Tag_Sequence "
    "RGI_Emoji_ZWJ_Sequence RGI_Emoji";

const char kGeneralCategoryValues[] =
    "Cased_Letter LC Close_Punctuation Pe Connector_Punctuation Pc Control Cc cntrl Currency_Symbol Sc "
    "Dash_Punctuation Pd Decimal_Number Nd digit Enclosing_Mark Me Final_Punctuation Pf Format Cf Initial_Punctuation "
    "Pi Letter L Letter_Number Nl Line_Separator Zl Lowercase_Letter Ll Mark M Combining_Mark Math_Symbol Sm "
    "Modifier_Letter Lm Modifier_Symbol Sk Nonspacing_Mark Mn Number N Open_Punctuation Ps Other C Other_Letter Lo "
    "Other_Number No Other_Punctuation Po Other_Symbol So Paragraph_Separator Zp Private_Use Co Punctuation P punct "
    "Separator Z Space_Separator Zs Spacing_Mark Mc Surrogate Cs Symbol S Titlecase_Letter Lt Unassigned Cn "
    "Uppercase_Letter Lu";

const char kEcma9ScriptValues[] =
    "Adlam Adlm Ahom Anatolian_Hieroglyphs Hluw Arabic Arab Armenian Armn Avestan Avst Balinese Bali Bamum Bamu "
    "Bassa_Vah Bass Batak Batk Bengali Beng Bhaiksuki Bhks Bopomofo Bopo Brahmi Brah Braille Brai Buginese Bugi Buhid "
    "Buhd Canadian_Aboriginal Cans Carian Cari Caucasian_Albanian Aghb Chakma Cakm Cham Cham Cherokee Cher Common Zyyy "
    "Coptic Copt Qaac Cuneiform Xsux Cypriot Cprt Cyrillic Cyrl Deseret Dsrt Devanagari Deva Duployan Dupl "
    "Egyptian_Hieroglyphs Egyp Elbasan Elba Ethiopic Ethi Georgian Geor Glagolitic Glag Gothic Goth Grantha Gran Greek "
    "Grek Gujarati Gujr Gurmukhi Guru Han Hani Hangul Hang Hanunoo Hano Hatran Hatr Hebrew Hebr Hiragana Hira "
    "Imperial_Aramaic Armi Inherited Zinh Qaai Inscriptional_Pahlavi Phli Inscriptional_Parthian Prti Javanese Java "
    "Kaithi Kthi Kannada Knda Katakana Kana Kayah_Li Kali Kharoshthi Khar Khmer Khmr Khojki Khoj Khudawadi Sind Lao "
    "Laoo Latin Latn Lepcha Lepc Limbu Limb Linear_A Lina Linear_B Linb Lisu Lisu Lycian Lyci Lydian Lydi Mahajani "
    "Mahj Malayalam Mlym Mandaic Mand Manichaean Mani Marchen Marc Masaram_Gondi Gonm Meetei_Mayek Mtei Mende_Kikakui "
    "Mend Meroitic_Cursive Merc Meroitic_Hieroglyphs Mero Miao Plrd Modi Mongolian Mong Mro Mroo Multani Mult Myanmar "
    "Mymr Nabataean Nbat New_Tai_Lue Talu Newa Newa Nko Nkoo Nushu Nshu Ogham Ogam Ol_Chiki Olck Old_Hungarian Hung "
    "Old_Italic Ital Old_North_Arabian Narb Old_Permic Perm Old_Persian Xpeo Old_South_Arabian Sarb Old_Turkic Orkh "
    "Oriya Orya Osage Osge Osmanya Osma Pahawh_Hmong Hmng Palmyrene Palm Pau_Cin_Hau Pauc Phags_Pa Phag Phoenician "
    "Phnx Psalter_Pahlavi Phlp Rejang Rjng Runic Runr Samaritan Samr Saurashtra Saur Sharada Shrd Shavian Shaw Siddham "
    "Sidd SignWriting Sgnw Sinhala Sinh Sora_Sompeng Sora Soyombo Soyo Sundanese Sund Syloti_Nagri Sylo Syriac Syrc "
    "Tagalog Tglg Tagbanwa Tagb Tai_Le Tale Tai_Tham Lana Tai_Viet Tavt Takri Takr Tamil Taml Tangut Tang Telugu Telu "
    "Thaana Thaa Thai Thai Tibetan Tibt Tifinagh Tfng Tirhuta Tirh Ugaritic Ugar Vai Vaii Warang_Citi Wara Yi Yiii "
    "Zanabazar_Square Zanb";
const char kEcma10ScriptAdded[] =
    "Dogra Dogr Gunjala_Gondi Gong Hanifi_Rohingya Rohg Makasar Maka Medefaidrin Medf Old_Sogdian Sogo Sogdian Sogd";
const char kEcma11ScriptAdded[] = "Elymaic Elym Nandinagari Nand Nyiakeng_Puachue_Hmong Hmnp Wancho Wcho";
const char kEcma12ScriptAdded[] = "Chorasmian Chrs Diak Dives_Akuru Khitan_Small_Script Kits Yezi Yezidi";
const char kEcma13ScriptAdded[] = "Cypro_Minoan Cpmn Old_Uyghur Ougr Tangsa Tnsa Toto Vithkuqi Vith";

using WordSet = std::unordered_set<std::string>;

void AddWords(WordSet& set, std::string_view words) {
    size_t i = 0;
    while (i < words.size()) {
        size_t j = words.find(' ', i);
        if (j == std::string_view::npos) j = words.size();
        if (j > i) set.emplace(words.substr(i, j - i));
        i = j + 1;
    }
}

struct UnicodeData {
    WordSet binary, binaryOfStrings, generalCategory, script;
};

const UnicodeData& DataFor(int ecmaVersion) {
    static std::unique_ptr<UnicodeData> cache[15];
    int v = ecmaVersion >= 14 ? 14 : ecmaVersion;
    if (v < 9) v = 9;
    std::unique_ptr<UnicodeData>& slot = cache[v];
    if (!slot) {
        slot = std::make_unique<UnicodeData>();
        AddWords(slot->binary, kEcma9BinaryProperties);
        if (v >= 10) AddWords(slot->binary, kEcma10BinaryAdded);
        if (v >= 12) AddWords(slot->binary, kEcma12BinaryAdded);
        AddWords(slot->binary, kGeneralCategoryValues);
        if (v >= 14) AddWords(slot->binaryOfStrings, kEcma14BinaryPropertiesOfStrings);
        AddWords(slot->generalCategory, kGeneralCategoryValues);
        AddWords(slot->script, kEcma9ScriptValues);
        if (v >= 10) AddWords(slot->script, kEcma10ScriptAdded);
        if (v >= 11) AddWords(slot->script, kEcma11ScriptAdded);
        if (v >= 12) AddWords(slot->script, kEcma12ScriptAdded);
        if (v >= 13) AddWords(slot->script, kEcma13ScriptAdded);
        if (v >= 14) AddWords(slot->script, kScriptValuesAddedInUnicode);
    }
    return *slot;
}

// acorn's codePointToString, appended to a UTF-16 string.
void AppendCodePoint(std::u16string& s, double code) {
    if (code <= 0xFFFF) {
        s += static_cast<char16_t>(static_cast<int>(code) & 0xFFFF);
        return;
    }
    int c = static_cast<int>(code) - 0x10000;
    s += static_cast<char16_t>((c >> 10) + 0xD800);
    s += static_cast<char16_t>((c & 1023) + 0xDC00);
}

bool IsSyntaxCharacter(int ch) {
    return ch == 0x24 || (ch >= 0x28 && ch <= 0x2B) || ch == 0x2E || ch == 0x3F || (ch >= 0x5B && ch <= 0x5E) ||
           (ch >= 0x7B && ch <= 0x7D);
}
bool IsRegularExpressionModifier(int ch) { return ch == 0x69 || ch == 0x6d || ch == 0x73; }
bool IsRegExpIdentifierStart(int ch) { return IsIdentifierStart(ch, true) || ch == 0x24 || ch == 0x5F; }
bool IsRegExpIdentifierPart(int ch) {
    return IsIdentifierChar(ch, true) || ch == 0x24 || ch == 0x5F || ch == 0x200C || ch == 0x200D;
}
bool IsControlLetter(int ch) { return (ch >= 0x41 && ch <= 0x5A) || (ch >= 0x61 && ch <= 0x7A); }
bool IsDecimalDigit(int ch) { return ch >= 0x30 && ch <= 0x39; }
bool IsHexDigit(int ch) {
    return (ch >= 0x30 && ch <= 0x39) || (ch >= 0x41 && ch <= 0x46) || (ch >= 0x61 && ch <= 0x66);
}
int HexToInt(int ch) {
    if (ch >= 0x41 && ch <= 0x46) return 10 + (ch - 0x41);
    if (ch >= 0x61 && ch <= 0x66) return 10 + (ch - 0x61);
    return ch - 0x30;
}
bool IsOctalDigit(int ch) { return ch >= 0x30 && ch <= 0x37; }
bool IsCharacterClassEscape(int ch) {
    return ch == 0x64 || ch == 0x44 || ch == 0x73 || ch == 0x53 || ch == 0x77 || ch == 0x57;
}
bool IsUnicodePropertyNameCharacter(int ch) { return IsControlLetter(ch) || ch == 0x5F; }
bool IsUnicodePropertyValueCharacter(int ch) { return IsUnicodePropertyNameCharacter(ch) || IsDecimalDigit(ch); }
bool IsClassSetReservedDoublePunctuatorCharacter(int ch) {
    return ch == 0x21 || (ch >= 0x23 && ch <= 0x26) || (ch >= 0x2A && ch <= 0x2C) || ch == 0x2E ||
           (ch >= 0x3A && ch <= 0x40) || ch == 0x5E || ch == 0x60 || ch == 0x7E;
}
bool IsClassSetSyntaxCharacter(int ch) {
    return ch == 0x28 || ch == 0x29 || ch == 0x2D || ch == 0x2F || (ch >= 0x5B && ch <= 0x5D) ||
           (ch >= 0x7B && ch <= 0x7D);
}
bool IsClassSetReservedPunctuator(int ch) {
    return ch == 0x21 || ch == 0x23 || ch == 0x25 || ch == 0x26 || ch == 0x2C || ch == 0x2D ||
           (ch >= 0x3A && ch <= 0x3E) || ch == 0x40 || ch == 0x60 || ch == 0x7E;
}

constexpr int CharSetNone = 0, CharSetOk = 1, CharSetString = 2;

struct BranchID {
    BranchID* parent;
    BranchID* base;
    bool separatedFrom(BranchID* alt) {
        for (BranchID* self = this; self; self = self->parent)
            for (BranchID* other = alt; other; other = other->parent)
                if (self->base == other->base && self != other) return true;
        return false;
    }
};

}  // namespace

struct RegexpParser {
    Parser& p;
    int ecmaVersion;
    const UnicodeData& unicodeProperties;
    std::u16string_view source;
    std::u16string_view flags;
    int start = 0;
    bool switchU = false, switchV = false, switchN = false;
    int pos = 0;
    double lastIntValue = 0;
    std::u16string lastStringValue;
    bool lastAssertionIsQuantifiable = false;
    int numCapturingParens = 0;
    double maxBackReference = 0;
    std::unordered_map<std::u16string, std::vector<BranchID*>> groupNames;
    std::vector<std::u16string> backReferenceNames;
    BranchID* branchID = nullptr;
    std::vector<std::unique_ptr<BranchID>> branchPool;
    int depth = 0;

    RegexpParser(Parser& parser, int ev) : p(parser), ecmaVersion(ev), unicodeProperties(DataFor(ev)) {}

    BranchID* NewBranch(BranchID* parent, BranchID* base) {
        branchPool.push_back(std::make_unique<BranchID>(BranchID{parent, nullptr}));
        BranchID* b = branchPool.back().get();
        b->base = base ? base : b;
        return b;
    }

    void reset(int s, std::u16string_view pattern, std::u16string_view f) {
        bool unicodeSets = f.find(u'v') != std::u16string_view::npos;
        bool unicode = f.find(u'u') != std::u16string_view::npos;
        start = s;
        source = pattern;
        flags = f;
        if (unicodeSets && ecmaVersion >= 15) {
            switchU = switchV = switchN = true;
        } else {
            switchU = unicode && ecmaVersion >= 6;
            switchV = false;
            switchN = unicode && ecmaVersion >= 9;
        }
    }

    struct Abort {};  // a recorded (recoverable) error ends the validation
    [[noreturn]] void raise(const std::string& message) {
        p.raiseRecoverable(start, "Invalid regular expression: /" + ToWtf8(source) + "/: " + message);
        throw Abort{};
    }

    int len() const { return static_cast<int>(source.size()); }
    int cu(int i) const { return source[static_cast<size_t>(i)]; }

    int at(int i, bool forceU = false) const {
        int l = len();
        if (i >= l) return -1;
        int c = cu(i);
        if (!(forceU || switchU) || c <= 0xD7FF || c >= 0xE000 || i + 1 >= l) return c;
        int nx = cu(i + 1);
        return nx >= 0xDC00 && nx <= 0xDFFF ? (c << 10) + nx - 0x35FDC00 : c;
    }
    int nextIndex(int i, bool forceU = false) const {
        int l = len();
        if (i >= l) return l;
        int c = cu(i), nx;
        if (!(forceU || switchU) || c <= 0xD7FF || c >= 0xE000 || i + 1 >= l || (nx = cu(i + 1)) < 0xDC00 ||
            nx > 0xDFFF)
            return i + 1;
        return i + 2;
    }
    int current(bool forceU = false) const { return at(pos, forceU); }
    int lookahead(bool forceU = false) const { return at(nextIndex(pos, forceU), forceU); }
    void advance(bool forceU = false) { pos = nextIndex(pos, forceU); }
    bool eat(int ch, bool forceU = false) {
        if (current(forceU) == ch) {
            advance(forceU);
            return true;
        }
        return false;
    }
    bool eatChars(int a, int b) {
        int q = pos;
        for (int ch : {a, b}) {
            int c = at(q);
            if (c == -1 || c != ch) return false;
            q = nextIndex(q);
        }
        pos = q;
        return true;
    }

    struct Depth {
        RegexpParser& r;
        explicit Depth(RegexpParser& rp) : r(rp) {
            if (++r.depth > 1500) {
                --r.depth;
                r.p.raise(r.start, "Too deeply nested");
            }
        }
        ~Depth() { --r.depth; }
        Depth(const Depth&) = delete;
        Depth& operator=(const Depth&) = delete;
    };

    void validateFlags() {
        std::u16string validFlags = u"gim";
        if (ecmaVersion >= 6) validFlags += u"uy";
        if (ecmaVersion >= 9) validFlags += u"s";
        if (ecmaVersion >= 13) validFlags += u"d";
        if (ecmaVersion >= 15) validFlags += u"v";
        bool u = false, v = false;
        for (size_t i = 0; i < flags.size(); ++i) {
            char16_t flag = flags[i];
            if (validFlags.find(flag) == std::u16string::npos) p.raise(start, "Invalid regular expression flag");
            if (flags.find(flag, i + 1) != std::u16string_view::npos)
                p.raise(start, "Duplicate regular expression flag");
            if (flag == u'u') u = true;
            if (flag == u'v') v = true;
        }
        if (ecmaVersion >= 15 && u && v) p.raise(start, "Invalid regular expression flag");
    }

    void validatePattern() {
        pattern();
        if (!switchN && ecmaVersion >= 9 && !groupNames.empty()) {
            switchN = true;
            pattern();
        }
    }

    void pattern() {
        pos = 0;
        lastIntValue = 0;
        lastStringValue.clear();
        lastAssertionIsQuantifiable = false;
        numCapturingParens = 0;
        maxBackReference = 0;
        groupNames.clear();
        backReferenceNames.clear();
        branchID = nullptr;
        disjunction();
        if (pos != len()) {
            if (eat(0x29)) raise("Unmatched ')'");
            if (eat(0x5D) || eat(0x7D)) raise("Lone quantifier brackets");
        }
        if (maxBackReference > numCapturingParens) raise("Invalid escape");
        for (const auto& name : backReferenceNames)
            if (groupNames.find(name) == groupNames.end()) raise("Invalid named capture referenced");
    }

    void disjunction() {
        Depth d(*this);
        bool trackDisjunction = ecmaVersion >= 16;
        if (trackDisjunction) branchID = NewBranch(branchID, nullptr);
        alternative();
        while (eat(0x7C)) {
            if (trackDisjunction) branchID = NewBranch(branchID->parent, branchID->base);
            alternative();
        }
        if (trackDisjunction) branchID = branchID->parent;
        if (eatQuantifier(true)) raise("Nothing to repeat");
        if (eat(0x7B)) raise("Lone quantifier brackets");
    }

    void alternative() {
        while (pos < len() && eatTerm()) {
        }
    }

    bool eatTerm() {
        if (eatAssertion()) {
            if (lastAssertionIsQuantifiable && eatQuantifier()) {
                if (switchU) raise("Invalid quantifier");
            }
            return true;
        }
        if (switchU ? eatAtom() : eatExtendedAtom()) {
            eatQuantifier();
            return true;
        }
        return false;
    }

    bool eatAssertion() {
        int s = pos;
        lastAssertionIsQuantifiable = false;
        if (eat(0x5E) || eat(0x24)) return true;
        if (eat(0x5C)) {
            if (eat(0x42) || eat(0x62)) return true;
            pos = s;
        }
        if (eat(0x28) && eat(0x3F)) {
            bool lookbehind = false;
            if (ecmaVersion >= 9) lookbehind = eat(0x3C);
            if (eat(0x3D) || eat(0x21)) {
                disjunction();
                if (!eat(0x29)) raise("Unterminated group");
                lastAssertionIsQuantifiable = !lookbehind;
                return true;
            }
        }
        pos = s;
        return false;
    }

    bool eatQuantifier(bool noError = false) {
        if (eatQuantifierPrefix(noError)) {
            eat(0x3F);
            return true;
        }
        return false;
    }
    bool eatQuantifierPrefix(bool noError) {
        return eat(0x2A) || eat(0x2B) || eat(0x3F) || eatBracedQuantifier(noError);
    }
    bool eatBracedQuantifier(bool noError) {
        int s = pos;
        if (eat(0x7B)) {
            double min = 0, max = -1;
            if (eatDecimalDigits()) {
                min = lastIntValue;
                if (eat(0x2C) && eatDecimalDigits()) max = lastIntValue;
                if (eat(0x7D)) {
                    if (max != -1 && max < min && !noError) raise("numbers out of order in {} quantifier");
                    return true;
                }
            }
            if (switchU && !noError) raise("Incomplete quantifier");
            pos = s;
        }
        return false;
    }

    bool eatAtom() {
        return eatPatternCharacters() || eat(0x2E) || eatReverseSolidusAtomEscape() || eatCharacterClass() ||
               eatUncapturingGroup() || eatCapturingGroup();
    }
    bool eatReverseSolidusAtomEscape() {
        int s = pos;
        if (eat(0x5C)) {
            if (eatAtomEscape()) return true;
            pos = s;
        }
        return false;
    }
    bool eatUncapturingGroup() {
        int s = pos;
        if (eat(0x28)) {
            if (eat(0x3F)) {
                if (ecmaVersion >= 16) {
                    std::u16string addModifiers = eatModifiers();
                    bool hasHyphen = eat(0x2D);
                    if (!addModifiers.empty() || hasHyphen) {
                        for (size_t i = 0; i < addModifiers.size(); ++i)
                            if (addModifiers.find(addModifiers[i], i + 1) != std::u16string::npos)
                                raise("Duplicate regular expression modifiers");
                        if (hasHyphen) {
                            std::u16string removeModifiers = eatModifiers();
                            if (addModifiers.empty() && removeModifiers.empty() && current() == 0x3A)
                                raise("Invalid regular expression modifiers");
                            for (size_t i = 0; i < removeModifiers.size(); ++i)
                                if (removeModifiers.find(removeModifiers[i], i + 1) != std::u16string::npos ||
                                    addModifiers.find(removeModifiers[i]) != std::u16string::npos)
                                    raise("Duplicate regular expression modifiers");
                        }
                    }
                }
                if (eat(0x3A)) {
                    disjunction();
                    if (eat(0x29)) return true;
                    raise("Unterminated group");
                }
            }
            pos = s;
        }
        return false;
    }
    bool eatCapturingGroup() {
        if (eat(0x28)) {
            if (ecmaVersion >= 9)
                groupSpecifier();
            else if (current() == 0x3F)
                raise("Invalid group");
            disjunction();
            if (eat(0x29)) {
                numCapturingParens += 1;
                return true;
            }
            raise("Unterminated group");
        }
        return false;
    }
    std::u16string eatModifiers() {
        std::u16string modifiers;
        int ch;
        while ((ch = current()) != -1 && IsRegularExpressionModifier(ch)) {
            AppendCodePoint(modifiers, ch);
            advance();
        }
        return modifiers;
    }

    bool eatExtendedAtom() {
        return eat(0x2E) || eatReverseSolidusAtomEscape() || eatCharacterClass() || eatUncapturingGroup() ||
               eatCapturingGroup() || eatInvalidBracedQuantifier() || eatExtendedPatternCharacter();
    }
    bool eatInvalidBracedQuantifier() {
        if (eatBracedQuantifier(true)) raise("Nothing to repeat");
        return false;
    }
    bool eatSyntaxCharacter() {
        int ch = current();
        if (IsSyntaxCharacter(ch)) {
            lastIntValue = ch;
            advance();
            return true;
        }
        return false;
    }
    bool eatPatternCharacters() {
        int s = pos;
        int ch;
        while ((ch = current()) != -1 && !IsSyntaxCharacter(ch)) advance();
        return pos != s;
    }
    bool eatExtendedPatternCharacter() {
        int ch = current();
        if (ch != -1 && ch != 0x24 && !(ch >= 0x28 && ch <= 0x2B) && ch != 0x2E && ch != 0x3F && ch != 0x5B &&
            ch != 0x5E && ch != 0x7C) {
            advance();
            return true;
        }
        return false;
    }

    void groupSpecifier() {
        if (eat(0x3F)) {
            if (!eatGroupName()) raise("Invalid group");
            bool trackDisjunction = ecmaVersion >= 16;
            auto it = groupNames.find(lastStringValue);
            if (it != groupNames.end()) {
                if (trackDisjunction) {
                    for (BranchID* altID : it->second)
                        if (!altID->separatedFrom(branchID)) raise("Duplicate capture group name");
                } else {
                    raise("Duplicate capture group name");
                }
            }
            if (trackDisjunction)
                groupNames[lastStringValue].push_back(branchID);
            else
                groupNames[lastStringValue];
        }
    }
    bool eatGroupName() {
        lastStringValue.clear();
        if (eat(0x3C)) {
            if (eatRegExpIdentifierName() && eat(0x3E)) return true;
            raise("Invalid capture group name");
        }
        return false;
    }
    bool eatRegExpIdentifierName() {
        lastStringValue.clear();
        if (eatRegExpIdentifierStart()) {
            AppendCodePoint(lastStringValue, lastIntValue);
            while (eatRegExpIdentifierPart()) AppendCodePoint(lastStringValue, lastIntValue);
            return true;
        }
        return false;
    }
    bool eatRegExpIdentifierStart() {
        int s = pos;
        bool forceU = ecmaVersion >= 11;
        int ch = current(forceU);
        advance(forceU);
        if (ch == 0x5C && eatRegExpUnicodeEscapeSequence(forceU)) ch = static_cast<int>(lastIntValue);
        if (IsRegExpIdentifierStart(ch)) {
            lastIntValue = ch;
            return true;
        }
        pos = s;
        return false;
    }
    bool eatRegExpIdentifierPart() {
        int s = pos;
        bool forceU = ecmaVersion >= 11;
        int ch = current(forceU);
        advance(forceU);
        if (ch == 0x5C && eatRegExpUnicodeEscapeSequence(forceU)) ch = static_cast<int>(lastIntValue);
        if (IsRegExpIdentifierPart(ch)) {
            lastIntValue = ch;
            return true;
        }
        pos = s;
        return false;
    }

    bool eatAtomEscape() {
        if (eatBackReference() || eatCharacterClassEscape() || eatCharacterEscape() || (switchN && eatKGroupName()))
            return true;
        if (switchU) {
            if (current() == 0x63) raise("Invalid unicode escape");
            raise("Invalid escape");
        }
        return false;
    }
    bool eatBackReference() {
        int s = pos;
        if (eatDecimalEscape()) {
            double n = lastIntValue;
            if (switchU) {
                if (n > maxBackReference) maxBackReference = n;
                return true;
            }
            if (n <= numCapturingParens) return true;
            pos = s;
        }
        return false;
    }
    bool eatKGroupName() {
        if (eat(0x6B)) {
            if (eatGroupName()) {
                backReferenceNames.push_back(lastStringValue);
                return true;
            }
            raise("Invalid named reference");
        }
        return false;
    }

    bool eatCharacterEscape() {
        return eatControlEscape() || eatCControlLetter() || eatZero() || eatHexEscapeSequence() ||
               eatRegExpUnicodeEscapeSequence(false) || (!switchU && eatLegacyOctalEscapeSequence()) ||
               eatIdentityEscape();
    }
    bool eatCControlLetter() {
        int s = pos;
        if (eat(0x63)) {
            if (eatControlLetter()) return true;
            pos = s;
        }
        return false;
    }
    bool eatZero() {
        if (current() == 0x30 && !IsDecimalDigit(lookahead())) {
            lastIntValue = 0;
            advance();
            return true;
        }
        return false;
    }
    bool eatControlEscape() {
        int ch = current();
        int v = ch == 0x74 ? 0x09 : ch == 0x6E ? 0x0A : ch == 0x76 ? 0x0B : ch == 0x66 ? 0x0C : ch == 0x72 ? 0x0D : -1;
        if (v < 0) return false;
        lastIntValue = v;
        advance();
        return true;
    }
    bool eatControlLetter() {
        int ch = current();
        if (IsControlLetter(ch)) {
            lastIntValue = ch % 0x20;
            advance();
            return true;
        }
        return false;
    }
    bool eatRegExpUnicodeEscapeSequence(bool forceU) {
        int s = pos;
        bool su = forceU || switchU;
        if (eat(0x75)) {
            if (eatFixedHexDigits(4)) {
                double lead = lastIntValue;
                if (su && lead >= 0xD800 && lead <= 0xDBFF) {
                    int leadSurrogateEnd = pos;
                    if (eat(0x5C) && eat(0x75) && eatFixedHexDigits(4)) {
                        double trail = lastIntValue;
                        if (trail >= 0xDC00 && trail <= 0xDFFF) {
                            lastIntValue = (lead - 0xD800) * 0x400 + (trail - 0xDC00) + 0x10000;
                            return true;
                        }
                    }
                    pos = leadSurrogateEnd;
                    lastIntValue = lead;
                }
                return true;
            }
            if (su && eat(0x7B) && eatHexDigits() && eat(0x7D) && lastIntValue >= 0 && lastIntValue <= 0x10FFFF)
                return true;
            if (su) raise("Invalid unicode escape");
            pos = s;
        }
        return false;
    }
    bool eatIdentityEscape() {
        if (switchU) {
            if (eatSyntaxCharacter()) return true;
            if (eat(0x2F)) {
                lastIntValue = 0x2F;
                return true;
            }
            return false;
        }
        int ch = current();
        if (ch != 0x63 && (!switchN || ch != 0x6B)) {
            lastIntValue = ch;
            advance();
            return true;
        }
        return false;
    }
    bool eatDecimalEscape() {
        lastIntValue = 0;
        int ch = current();
        if (ch >= 0x31 && ch <= 0x39) {
            do {
                lastIntValue = 10 * lastIntValue + (ch - 0x30);
                advance();
            } while ((ch = current()) >= 0x30 && ch <= 0x39);
            return true;
        }
        return false;
    }

    int eatCharacterClassEscape() {
        int ch = current();
        if (IsCharacterClassEscape(ch)) {
            lastIntValue = -1;
            advance();
            return CharSetOk;
        }
        bool negate = false;
        if (switchU && ecmaVersion >= 9 && ((negate = ch == 0x50) || ch == 0x70)) {
            lastIntValue = -1;
            advance();
            int result;
            if (eat(0x7B) && (result = eatUnicodePropertyValueExpression()) && eat(0x7D)) {
                if (negate && result == CharSetString) raise("Invalid property name");
                return result;
            }
            raise("Invalid property name");
        }
        return CharSetNone;
    }
    int eatUnicodePropertyValueExpression() {
        int s = pos;
        if (eatUnicodePropertyName() && eat(0x3D)) {
            std::u16string name = lastStringValue;
            if (eatUnicodePropertyValue()) {
                validateUnicodePropertyNameAndValue(ToWtf8(name), ToWtf8(lastStringValue));
                return CharSetOk;
            }
        }
        pos = s;
        if (eatUnicodePropertyValue()) return validateUnicodePropertyNameOrValue(ToWtf8(lastStringValue));
        return CharSetNone;
    }
    void validateUnicodePropertyNameAndValue(const std::string& name, const std::string& value) {
        const WordSet* set = nullptr;
        if (name == "General_Category" || name == "gc")
            set = &unicodeProperties.generalCategory;
        else if (name == "Script" || name == "sc" || name == "Script_Extensions" || name == "scx")
            set = &unicodeProperties.script;
        if (!set) raise("Invalid property name");
        if (!set->count(value)) raise("Invalid property value");
    }
    int validateUnicodePropertyNameOrValue(const std::string& nameOrValue) {
        if (unicodeProperties.binary.count(nameOrValue)) return CharSetOk;
        if (switchV && unicodeProperties.binaryOfStrings.count(nameOrValue)) return CharSetString;
        raise("Invalid property name");
    }
    bool eatUnicodePropertyName() {
        int ch;
        lastStringValue.clear();
        while (IsUnicodePropertyNameCharacter(ch = current())) {
            AppendCodePoint(lastStringValue, ch);
            advance();
        }
        return !lastStringValue.empty();
    }
    bool eatUnicodePropertyValue() {
        int ch;
        lastStringValue.clear();
        while (IsUnicodePropertyValueCharacter(ch = current())) {
            AppendCodePoint(lastStringValue, ch);
            advance();
        }
        return !lastStringValue.empty();
    }

    bool eatCharacterClass() {
        if (eat(0x5B)) {
            bool negate = eat(0x5E);
            int result = classContents();
            if (!eat(0x5D)) raise("Unterminated character class");
            if (negate && result == CharSetString) raise("Negated character class may contain strings");
            return true;
        }
        return false;
    }
    int classContents() {
        if (current() == 0x5D) return CharSetOk;
        if (switchV) return classSetExpression();
        nonEmptyClassRanges();
        return CharSetOk;
    }
    void nonEmptyClassRanges() {
        while (eatClassAtom()) {
            double left = lastIntValue;
            if (eat(0x2D) && eatClassAtom()) {
                double right = lastIntValue;
                if (switchU && (left == -1 || right == -1)) raise("Invalid character class");
                if (left != -1 && right != -1 && left > right) raise("Range out of order in character class");
            }
        }
    }
    bool eatClassAtom() {
        int s = pos;
        if (eat(0x5C)) {
            if (eatClassEscape()) return true;
            if (switchU) {
                int ch1 = current();
                if (ch1 == 0x63 || IsOctalDigit(ch1)) raise("Invalid class escape");
                raise("Invalid escape");
            }
            pos = s;
        }
        int ch = current();
        if (ch != 0x5D) {
            lastIntValue = ch;
            advance();
            return true;
        }
        return false;
    }
    bool eatClassEscape() {
        int s = pos;
        if (eat(0x62)) {
            lastIntValue = 0x08;
            return true;
        }
        if (switchU && eat(0x2D)) {
            lastIntValue = 0x2D;
            return true;
        }
        if (!switchU && eat(0x63)) {
            if (eatClassControlLetter()) return true;
            pos = s;
        }
        return eatCharacterClassEscape() || eatCharacterEscape();
    }

    int classSetExpression() {
        Depth d(*this);
        int result = CharSetOk, subResult;
        if (eatClassSetRange()) {
        } else if ((subResult = eatClassSetOperand())) {
            if (subResult == CharSetString) result = CharSetString;
            int s = pos;
            while (eatChars(0x26, 0x26)) {
                if (current() != 0x26 && (subResult = eatClassSetOperand())) {
                    if (subResult != CharSetString) result = CharSetOk;
                    continue;
                }
                raise("Invalid character in character class");
            }
            if (s != pos) return result;
            while (eatChars(0x2D, 0x2D)) {
                if (eatClassSetOperand()) continue;
                raise("Invalid character in character class");
            }
            if (s != pos) return result;
        } else {
            raise("Invalid character in character class");
        }
        for (;;) {
            if (eatClassSetRange()) continue;
            subResult = eatClassSetOperand();
            if (!subResult) return result;
            if (subResult == CharSetString) result = CharSetString;
        }
    }
    bool eatClassSetRange() {
        int s = pos;
        if (eatClassSetCharacter()) {
            double left = lastIntValue;
            if (eat(0x2D) && eatClassSetCharacter()) {
                double right = lastIntValue;
                if (left != -1 && right != -1 && left > right) raise("Range out of order in character class");
                return true;
            }
            pos = s;
        }
        return false;
    }
    int eatClassSetOperand() {
        if (eatClassSetCharacter()) return CharSetOk;
        int r = eatClassStringDisjunction();
        return r ? r : eatNestedClass();
    }
    int eatNestedClass() {
        int s = pos;
        if (eat(0x5B)) {
            bool negate = eat(0x5E);
            int result = classContents();
            if (eat(0x5D)) {
                if (negate && result == CharSetString) raise("Negated character class may contain strings");
                return result;
            }
            pos = s;
        }
        if (eat(0x5C)) {
            int result1 = eatCharacterClassEscape();
            if (result1) return result1;
            pos = s;
        }
        return CharSetNone;
    }
    int eatClassStringDisjunction() {
        int s = pos;
        if (eatChars(0x5C, 0x71)) {
            if (eat(0x7B)) {
                int result = classStringDisjunctionContents();
                if (eat(0x7D)) return result;
            } else {
                raise("Invalid escape");
            }
            pos = s;
        }
        return CharSetNone;
    }
    int classStringDisjunctionContents() {
        int result = classString();
        while (eat(0x7C))
            if (classString() == CharSetString) result = CharSetString;
        return result;
    }
    int classString() {
        int count = 0;
        while (eatClassSetCharacter()) ++count;
        return count == 1 ? CharSetOk : CharSetString;
    }
    bool eatClassSetCharacter() {
        int s = pos;
        if (eat(0x5C)) {
            if (eatCharacterEscape() || eatClassSetReservedPunctuator()) return true;
            if (eat(0x62)) {
                lastIntValue = 0x08;
                return true;
            }
            pos = s;
            return false;
        }
        int ch = current();
        if (ch < 0 || (ch == lookahead() && IsClassSetReservedDoublePunctuatorCharacter(ch))) return false;
        if (IsClassSetSyntaxCharacter(ch)) return false;
        advance();
        lastIntValue = ch;
        return true;
    }
    bool eatClassSetReservedPunctuator() {
        int ch = current();
        if (IsClassSetReservedPunctuator(ch)) {
            lastIntValue = ch;
            advance();
            return true;
        }
        return false;
    }
    bool eatClassControlLetter() {
        int ch = current();
        if (IsDecimalDigit(ch) || ch == 0x5F) {
            lastIntValue = ch % 0x20;
            advance();
            return true;
        }
        return false;
    }
    bool eatHexEscapeSequence() {
        int s = pos;
        if (eat(0x78)) {
            if (eatFixedHexDigits(2)) return true;
            if (switchU) raise("Invalid escape");
            pos = s;
        }
        return false;
    }
    bool eatDecimalDigits() {
        int s = pos;
        int ch;
        lastIntValue = 0;
        while (IsDecimalDigit(ch = current())) {
            lastIntValue = 10 * lastIntValue + (ch - 0x30);
            advance();
        }
        return pos != s;
    }
    bool eatHexDigits() {
        int s = pos;
        int ch;
        lastIntValue = 0;
        while (IsHexDigit(ch = current())) {
            lastIntValue = 16 * lastIntValue + HexToInt(ch);
            advance();
        }
        return pos != s;
    }
    bool eatLegacyOctalEscapeSequence() {
        if (eatOctalDigit()) {
            double n1 = lastIntValue;
            if (eatOctalDigit()) {
                double n2 = lastIntValue;
                if (n1 <= 3 && eatOctalDigit())
                    lastIntValue = n1 * 64 + n2 * 8 + lastIntValue;
                else
                    lastIntValue = n1 * 8 + n2;
            } else {
                lastIntValue = n1;
            }
            return true;
        }
        return false;
    }
    bool eatOctalDigit() {
        int ch = current();
        if (IsOctalDigit(ch)) {
            lastIntValue = ch - 0x30;
            advance();
            return true;
        }
        lastIntValue = 0;
        return false;
    }
    bool eatFixedHexDigits(int length) {
        int s = pos;
        lastIntValue = 0;
        for (int i = 0; i < length; ++i) {
            int ch = current();
            if (!IsHexDigit(ch)) {
                pos = s;
                return false;
            }
            lastIntValue = 16 * lastIntValue + HexToInt(ch);
            advance();
        }
        return true;
    }
};

void Parser::validateRegExpFlagsAndPattern(int s, std::u16string_view pattern, std::u16string_view flags) {
    RegexpParser state(*this, ecmaVersion);
    state.reset(s, pattern, flags);
    try {
        state.validateFlags();
        state.validatePattern();
    } catch (const RegexpParser::Abort&) {
    }
}

}  // namespace jsparse
