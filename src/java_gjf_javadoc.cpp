// Copyright 2016 Google Inc., Apache License 2.0, ported from google-java-format 1.36.1
// (core/src/main/java/com/google/googlejavaformat/java/javadoc/).
//
// GJF's javadoc formatter (JavadocFormatter, JavadocLexer, JavadocWriter, Token,
// NestingStack, CharStream), ported nearly 1:1 so a diff against GJF can be
// localised to the Java method it came from. Names follow the Java ones.
//
// Only the classic `/** ... */` path is ported. GJF also has a Markdown `///`
// path (MarkdownPositions.java), but the reference GJF runs on JDK 21, where
// JavaInputAstVisitor never records markdown javadoc positions (it checks
// Runtime.version().feature() >= 23), so formatJavadoc is only ever called on
// `/** ... */` comments.
//
// Java semantics kept exactly: lengths and columns are UTF-16 code units
// (String.length()); every java.util.regex Pattern is a hand matcher with
// Java's meaning (`\s` = [ \t\n\x0B\f\r], `\w` = [a-zA-Z_0-9], `.` excludes
// line terminators unless DOTALL, CASE_INSENSITIVE is ASCII-only, and `\b`
// on JDK 21 is ASCII-word based but still treats a non-spacing mark (Mn) that
// follows a letter/digit as a word character).

#include "java_gjf_javadoc.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace javafmt::gjf {
namespace {

constexpr std::size_t npos = std::string::npos;
constexpr int kMaxLineLength = 100;  // JavadocFormatter.MAX_LINE_LENGTH

// Thrown where JavadocLexer throws LexException; formatJavadoc returns its input.
struct LexException {};
// Thrown where GJF itself would throw something else (an IllegalArgumentException,
// AssertionError, NoSuchElementException...). GJF would fail the whole file; we
// return the input unchanged.
struct InternalError {};

// ---------------------------------------------------------------------------
// Text helpers (Java String semantics over UTF-8 bytes)

/** Java String.length(): UTF-16 code units. A 4-byte UTF-8 sequence is 2 units. */
int Utf16Length(std::string_view s) {
    int n = 0;
    for (char ch : s) {
        auto b = static_cast<unsigned char>(ch);
        if ((b & 0xC0U) != 0x80U) n += (b >= 0xF0U) ? 2 : 1;
    }
    return n;
}

/** Decodes the code point at byte `i` (lenient: a bad sequence yields its first byte). */
std::uint32_t CodePointAt(std::string_view s, std::size_t i, std::size_t *len_out = nullptr) {
    auto b0 = static_cast<unsigned char>(s[i]);
    std::size_t len = 1;
    std::uint32_t cp = b0;
    if (b0 >= 0xF0U && b0 < 0xF8U) {
        len = 4;
        cp = b0 & 0x07U;
    } else if (b0 >= 0xE0U) {
        len = 3;
        cp = b0 & 0x0FU;
    } else if (b0 >= 0xC0U) {
        len = 2;
        cp = b0 & 0x1FU;
    }
    if (len > 1) {
        if (i + len > s.size()) {
            len = 1;
            cp = b0;
        } else {
            for (std::size_t k = 1; k < len; ++k) {
                auto b = static_cast<unsigned char>(s[i + k]);
                if ((b & 0xC0U) != 0x80U) {
                    len = 1;
                    cp = b0;
                    break;
                }
                cp = (cp << 6) | (b & 0x3FU);
            }
        }
    }
    if (len_out != nullptr) *len_out = len;
    return cp;
}

bool IsAsciiWord(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/** Java regex `\s` without UNICODE_CHARACTER_CLASS. */
bool IsJavaSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\x0B' || c == '\f' || c == '\r';
}

char AsciiLower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

struct CpRange {
    std::uint32_t lo, hi;
};

// Character.getType(c) == NON_SPACING_MARK on JDK 21 (Unicode 15.0), generated
// from the reference JVM.
constexpr CpRange kNonSpacingMarks[] = {
    {0x300,0x36F},{0x483,0x487},{0x591,0x5BD},{0x5BF,0x5BF},{0x5C1,0x5C2},{0x5C4,0x5C5},
    {0x5C7,0x5C7},{0x610,0x61A},{0x64B,0x65F},{0x670,0x670},{0x6D6,0x6DC},{0x6DF,0x6E4},
    {0x6E7,0x6E8},{0x6EA,0x6ED},{0x711,0x711},{0x730,0x74A},{0x7A6,0x7B0},{0x7EB,0x7F3},
    {0x7FD,0x7FD},{0x816,0x819},{0x81B,0x823},{0x825,0x827},{0x829,0x82D},{0x859,0x85B},
    {0x898,0x89F},{0x8CA,0x8E1},{0x8E3,0x902},{0x93A,0x93A},{0x93C,0x93C},{0x941,0x948},
    {0x94D,0x94D},{0x951,0x957},{0x962,0x963},{0x981,0x981},{0x9BC,0x9BC},{0x9C1,0x9C4},
    {0x9CD,0x9CD},{0x9E2,0x9E3},{0x9FE,0x9FE},{0xA01,0xA02},{0xA3C,0xA3C},{0xA41,0xA42},
    {0xA47,0xA48},{0xA4B,0xA4D},{0xA51,0xA51},{0xA70,0xA71},{0xA75,0xA75},{0xA81,0xA82},
    {0xABC,0xABC},{0xAC1,0xAC5},{0xAC7,0xAC8},{0xACD,0xACD},{0xAE2,0xAE3},{0xAFA,0xAFF},
    {0xB01,0xB01},{0xB3C,0xB3C},{0xB3F,0xB3F},{0xB41,0xB44},{0xB4D,0xB4D},{0xB55,0xB56},
    {0xB62,0xB63},{0xB82,0xB82},{0xBC0,0xBC0},{0xBCD,0xBCD},{0xC00,0xC00},{0xC04,0xC04},
    {0xC3C,0xC3C},{0xC3E,0xC40},{0xC46,0xC48},{0xC4A,0xC4D},{0xC55,0xC56},{0xC62,0xC63},
    {0xC81,0xC81},{0xCBC,0xCBC},{0xCBF,0xCBF},{0xCC6,0xCC6},{0xCCC,0xCCD},{0xCE2,0xCE3},
    {0xD00,0xD01},{0xD3B,0xD3C},{0xD41,0xD44},{0xD4D,0xD4D},{0xD62,0xD63},{0xD81,0xD81},
    {0xDCA,0xDCA},{0xDD2,0xDD4},{0xDD6,0xDD6},{0xE31,0xE31},{0xE34,0xE3A},{0xE47,0xE4E},
    {0xEB1,0xEB1},{0xEB4,0xEBC},{0xEC8,0xECE},{0xF18,0xF19},{0xF35,0xF35},{0xF37,0xF37},
    {0xF39,0xF39},{0xF71,0xF7E},{0xF80,0xF84},{0xF86,0xF87},{0xF8D,0xF97},{0xF99,0xFBC},
    {0xFC6,0xFC6},{0x102D,0x1030},{0x1032,0x1037},{0x1039,0x103A},{0x103D,0x103E},{0x1058,0x1059},
    {0x105E,0x1060},{0x1071,0x1074},{0x1082,0x1082},{0x1085,0x1086},{0x108D,0x108D},{0x109D,0x109D},
    {0x135D,0x135F},{0x1712,0x1714},{0x1732,0x1733},{0x1752,0x1753},{0x1772,0x1773},{0x17B4,0x17B5},
    {0x17B7,0x17BD},{0x17C6,0x17C6},{0x17C9,0x17D3},{0x17DD,0x17DD},{0x180B,0x180D},{0x180F,0x180F},
    {0x1885,0x1886},{0x18A9,0x18A9},{0x1920,0x1922},{0x1927,0x1928},{0x1932,0x1932},{0x1939,0x193B},
    {0x1A17,0x1A18},{0x1A1B,0x1A1B},{0x1A56,0x1A56},{0x1A58,0x1A5E},{0x1A60,0x1A60},{0x1A62,0x1A62},
    {0x1A65,0x1A6C},{0x1A73,0x1A7C},{0x1A7F,0x1A7F},{0x1AB0,0x1ABD},{0x1ABF,0x1ACE},{0x1B00,0x1B03},
    {0x1B34,0x1B34},{0x1B36,0x1B3A},{0x1B3C,0x1B3C},{0x1B42,0x1B42},{0x1B6B,0x1B73},{0x1B80,0x1B81},
    {0x1BA2,0x1BA5},{0x1BA8,0x1BA9},{0x1BAB,0x1BAD},{0x1BE6,0x1BE6},{0x1BE8,0x1BE9},{0x1BED,0x1BED},
    {0x1BEF,0x1BF1},{0x1C2C,0x1C33},{0x1C36,0x1C37},{0x1CD0,0x1CD2},{0x1CD4,0x1CE0},{0x1CE2,0x1CE8},
    {0x1CED,0x1CED},{0x1CF4,0x1CF4},{0x1CF8,0x1CF9},{0x1DC0,0x1DFF},{0x20D0,0x20DC},{0x20E1,0x20E1},
    {0x20E5,0x20F0},{0x2CEF,0x2CF1},{0x2D7F,0x2D7F},{0x2DE0,0x2DFF},{0x302A,0x302D},{0x3099,0x309A},
    {0xA66F,0xA66F},{0xA674,0xA67D},{0xA69E,0xA69F},{0xA6F0,0xA6F1},{0xA802,0xA802},{0xA806,0xA806},
    {0xA80B,0xA80B},{0xA825,0xA826},{0xA82C,0xA82C},{0xA8C4,0xA8C5},{0xA8E0,0xA8F1},{0xA8FF,0xA8FF},
    {0xA926,0xA92D},{0xA947,0xA951},{0xA980,0xA982},{0xA9B3,0xA9B3},{0xA9B6,0xA9B9},{0xA9BC,0xA9BD},
    {0xA9E5,0xA9E5},{0xAA29,0xAA2E},{0xAA31,0xAA32},{0xAA35,0xAA36},{0xAA43,0xAA43},{0xAA4C,0xAA4C},
    {0xAA7C,0xAA7C},{0xAAB0,0xAAB0},{0xAAB2,0xAAB4},{0xAAB7,0xAAB8},{0xAABE,0xAABF},{0xAAC1,0xAAC1},
    {0xAAEC,0xAAED},{0xAAF6,0xAAF6},{0xABE5,0xABE5},{0xABE8,0xABE8},{0xABED,0xABED},{0xFB1E,0xFB1E},
    {0xFE00,0xFE0F},{0xFE20,0xFE2F},{0x101FD,0x101FD},{0x102E0,0x102E0},{0x10376,0x1037A},{0x10A01,0x10A03},
    {0x10A05,0x10A06},{0x10A0C,0x10A0F},{0x10A38,0x10A3A},{0x10A3F,0x10A3F},{0x10AE5,0x10AE6},{0x10D24,0x10D27},
    {0x10EAB,0x10EAC},{0x10EFD,0x10EFF},{0x10F46,0x10F50},{0x10F82,0x10F85},{0x11001,0x11001},{0x11038,0x11046},
    {0x11070,0x11070},{0x11073,0x11074},{0x1107F,0x11081},{0x110B3,0x110B6},{0x110B9,0x110BA},{0x110C2,0x110C2},
    {0x11100,0x11102},{0x11127,0x1112B},{0x1112D,0x11134},{0x11173,0x11173},{0x11180,0x11181},{0x111B6,0x111BE},
    {0x111C9,0x111CC},{0x111CF,0x111CF},{0x1122F,0x11231},{0x11234,0x11234},{0x11236,0x11237},{0x1123E,0x1123E},
    {0x11241,0x11241},{0x112DF,0x112DF},{0x112E3,0x112EA},{0x11300,0x11301},{0x1133B,0x1133C},{0x11340,0x11340},
    {0x11366,0x1136C},{0x11370,0x11374},{0x11438,0x1143F},{0x11442,0x11444},{0x11446,0x11446},{0x1145E,0x1145E},
    {0x114B3,0x114B8},{0x114BA,0x114BA},{0x114BF,0x114C0},{0x114C2,0x114C3},{0x115B2,0x115B5},{0x115BC,0x115BD},
    {0x115BF,0x115C0},{0x115DC,0x115DD},{0x11633,0x1163A},{0x1163D,0x1163D},{0x1163F,0x11640},{0x116AB,0x116AB},
    {0x116AD,0x116AD},{0x116B0,0x116B5},{0x116B7,0x116B7},{0x1171D,0x1171F},{0x11722,0x11725},{0x11727,0x1172B},
    {0x1182F,0x11837},{0x11839,0x1183A},{0x1193B,0x1193C},{0x1193E,0x1193E},{0x11943,0x11943},{0x119D4,0x119D7},
    {0x119DA,0x119DB},{0x119E0,0x119E0},{0x11A01,0x11A0A},{0x11A33,0x11A38},{0x11A3B,0x11A3E},{0x11A47,0x11A47},
    {0x11A51,0x11A56},{0x11A59,0x11A5B},{0x11A8A,0x11A96},{0x11A98,0x11A99},{0x11C30,0x11C36},{0x11C38,0x11C3D},
    {0x11C3F,0x11C3F},{0x11C92,0x11CA7},{0x11CAA,0x11CB0},{0x11CB2,0x11CB3},{0x11CB5,0x11CB6},{0x11D31,0x11D36},
    {0x11D3A,0x11D3A},{0x11D3C,0x11D3D},{0x11D3F,0x11D45},{0x11D47,0x11D47},{0x11D90,0x11D91},{0x11D95,0x11D95},
    {0x11D97,0x11D97},{0x11EF3,0x11EF4},{0x11F00,0x11F01},{0x11F36,0x11F3A},{0x11F40,0x11F40},{0x11F42,0x11F42},
    {0x13440,0x13440},{0x13447,0x13455},{0x16AF0,0x16AF4},{0x16B30,0x16B36},{0x16F4F,0x16F4F},{0x16F8F,0x16F92},
    {0x16FE4,0x16FE4},{0x1BC9D,0x1BC9E},{0x1CF00,0x1CF2D},{0x1CF30,0x1CF46},{0x1D167,0x1D169},{0x1D17B,0x1D182},
    {0x1D185,0x1D18B},{0x1D1AA,0x1D1AD},{0x1D242,0x1D244},{0x1DA00,0x1DA36},{0x1DA3B,0x1DA6C},{0x1DA75,0x1DA75},
    {0x1DA84,0x1DA84},{0x1DA9B,0x1DA9F},{0x1DAA1,0x1DAAF},{0x1E000,0x1E006},{0x1E008,0x1E018},{0x1E01B,0x1E021},
    {0x1E023,0x1E024},{0x1E026,0x1E02A},{0x1E08F,0x1E08F},{0x1E130,0x1E136},{0x1E2AE,0x1E2AE},{0x1E2EC,0x1E2EF},
    {0x1E4EC,0x1E4EF},{0x1E8D0,0x1E8D6},{0x1E944,0x1E94A},{0xE0100,0xE01EF},
};

bool IsNonSpacingMark(std::uint32_t cp) {
    std::size_t lo = 0;
    std::size_t hi = std::size(kNonSpacingMarks);
    while (lo < hi) {
        std::size_t mid = (lo + hi) / 2;
        if (cp < kNonSpacingMarks[mid].lo) {
            hi = mid;
        } else if (cp > kNonSpacingMarks[mid].hi) {
            lo = mid + 1;
        } else {
            return true;
        }
    }
    return false;
}

/**
 * Java `\b` at byte `i` where the character before `i` is known to be an ASCII
 * letter or digit (true for every `\b` in these patterns). On JDK 21 the right
 * side is a word character if it is ASCII [A-Za-z0-9_], or a non-spacing mark
 * with a letter/digit base before it (always so here). Region end = no char.
 */
bool WordBoundaryAfterWordChar(std::string_view s, std::size_t i) {
    if (i >= s.size()) return true;
    if (IsAsciiWord(s[i])) return false;
    return !IsNonSpacingMark(CodePointAt(s, i));
}

bool StartsWithAt(std::string_view s, std::size_t pos, std::string_view prefix) {
    return pos <= s.size() && s.size() - pos >= prefix.size() && s.compare(pos, prefix.size(), prefix) == 0;
}

/** Java String.trim(): strips chars <= U+0020 from both ends. */
std::string JavaTrim(std::string_view s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && static_cast<unsigned char>(s[b]) <= 0x20U) ++b;
    while (e > b && static_cast<unsigned char>(s[e - 1]) <= 0x20U) --e;
    return std::string(s.substr(b, e - b));
}

// ---------------------------------------------------------------------------
// Token.java

enum class Kind {
    BeginJavadoc,
    EndJavadoc,
    FooterJavadocTagStart,
    SnippetBegin,
    SnippetEnd,
    ListOpenTag,
    ListCloseTag,
    ListItemOpenTag,
    ListItemCloseTag,
    HeaderOpenTag,
    HeaderCloseTag,
    ParagraphOpenTag,
    ParagraphCloseTag,
    BlockQuoteOpenTag,
    BlockQuoteCloseTag,
    PreOpenTag,
    PreCloseTag,
    CodeOpenTag,
    CodeCloseTag,
    TableOpenTag,
    TableCloseTag,
    MoeBeginStripComment,
    MoeEndStripComment,
    HtmlComment,
    BrTag,
    Whitespace,
    ForcedNewline,
    OptionalLineBreak,
    Literal,
};

struct Token {
    Kind kind;
    std::string value;

    int length() const { return Utf16Length(value); }
};

bool IsStartOfLineToken(Kind k) {
    return k == Kind::ListItemOpenTag || k == Kind::HeaderOpenTag || k == Kind::ParagraphOpenTag;
}

// ---------------------------------------------------------------------------
// NestingStack.java

enum class NestingContext {
    HTML_PRE_CONTEXT,
    HTML_CODE_CONTEXT,
    TABLE,
    SNIPPET_CONTEXT,
    BRACE_CONTEXT,
    INLINE_TAG_CONTEXT,
};

using ContextSet = std::initializer_list<NestingContext>;

bool SetHas(ContextSet set, NestingContext c) { return std::find(set.begin(), set.end(), c) != set.end(); }

class NestingStack {
   public:
    void push(NestingContext v) { stack_.push_back(v); }

    std::optional<NestingContext> popIfIn(ContextSet values) {
        if (stack_.empty() || !SetHas(values, stack_.back())) return std::nullopt;
        NestingContext v = stack_.back();
        stack_.pop_back();
        return v;
    }

    void popUntil(NestingContext value) {
        if (!contains(value)) return;
        NestingContext popped;
        do {
            popped = stack_.back();
            stack_.pop_back();
        } while (popped != value);
    }

    bool contains(NestingContext v) const { return std::find(stack_.begin(), stack_.end(), v) != stack_.end(); }

    bool containsAny(ContextSet values) const {
        for (NestingContext c : stack_) {
            if (SetHas(values, c)) return true;
        }
        return false;
    }

    bool isEmpty() const { return stack_.empty(); }

   private:
    std::vector<NestingContext> stack_;
};

class IntNestingStack {  // NestingStack.Int
   public:
    int total() const { return total_; }
    void push(int value) {
        stack_.push_back(value);
        total_ += value;
    }
    void push() { push(1); }
    void popIfNotEmpty() {
        if (!stack_.empty()) {
            total_ -= stack_.back();
            stack_.pop_back();
        }
    }
    bool isEmpty() const { return stack_.empty(); }
    void reset() {
        stack_.clear();
        total_ = 0;
    }

   private:
    std::vector<int> stack_;
    int total_ = 0;
};

// ---------------------------------------------------------------------------
// The lexer's Patterns. Each returns the match end for lookingAt() at `p`, or npos.

/** CLASSIC_NEWLINE_PATTERN = "[ \t]*\n[ \t]*[*]?[ \t]?" */
std::size_t MatchClassicNewline(std::string_view s, std::size_t p) {
    std::size_t i = p;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i >= s.size() || s[i] != '\n') return npos;
    ++i;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i < s.size() && s[i] == '*') ++i;
    if (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return i;
}

/** FOOTER_TAG_PATTERN = "@(param\\s+<\\w+>|[a-z]\\w*)" */
std::size_t MatchFooterTag(std::string_view s, std::size_t p) {
    if (p >= s.size() || s[p] != '@') return npos;
    std::size_t i = p + 1;
    if (StartsWithAt(s, i, "param")) {
        std::size_t j = i + 5;
        std::size_t k = j;
        while (k < s.size() && IsJavaSpace(s[k])) ++k;
        if (k > j && k < s.size() && s[k] == '<') {
            std::size_t w = k + 1;
            std::size_t x = w;
            while (x < s.size() && IsAsciiWord(s[x])) ++x;
            if (x > w && x < s.size() && s[x] == '>') return x + 1;
        }
    }
    if (i < s.size() && s[i] >= 'a' && s[i] <= 'z') {
        ++i;
        while (i < s.size() && IsAsciiWord(s[i])) ++i;
        return i;
    }
    return npos;
}

/** "<!--\\s*MOE:" + which + "\\s*-->" */
std::size_t MatchMoeStripComment(std::string_view s, std::size_t p, std::string_view which) {
    if (!StartsWithAt(s, p, "<!--")) return npos;
    std::size_t i = p + 4;
    while (i < s.size() && IsJavaSpace(s[i])) ++i;
    if (!StartsWithAt(s, i, "MOE:")) return npos;
    i += 4;
    if (!StartsWithAt(s, i, which)) return npos;
    i += which.size();
    while (i < s.size() && IsJavaSpace(s[i])) ++i;
    if (!StartsWithAt(s, i, "-->")) return npos;
    return i + 3;
}

/** HTML_COMMENT_PATTERN = "<!--.*?-->" with DOTALL */
std::size_t MatchHtmlComment(std::string_view s, std::size_t p) {
    if (!StartsWithAt(s, p, "<!--")) return npos;
    std::size_t e = s.find("-->", p + 4);
    return e == npos ? npos : e + 3;
}

/** SNIPPET_TAG_OPEN_PATTERN = "[{]@snippet\\b" */
std::size_t MatchSnippetOpen(std::string_view s, std::size_t p) {
    if (!StartsWithAt(s, p, "{@snippet")) return npos;
    std::size_t e = p + 9;
    return WordBoundaryAfterWordChar(s, e) ? e : npos;
}

/** INLINE_TAG_OPEN_PATTERN = "[{]@\\w*" */
std::size_t MatchInlineTagOpen(std::string_view s, std::size_t p) {
    if (!StartsWithAt(s, p, "{@")) return npos;
    std::size_t i = p + 2;
    while (i < s.size() && IsAsciiWord(s[i])) ++i;
    return i;
}

/** CLASSIC_LITERAL_PATTERN = ".[^ \t\n@<{}*]*" with DOTALL */
std::size_t MatchClassicLiteral(std::string_view s, std::size_t p) {
    if (p >= s.size()) return npos;
    std::size_t len = 1;
    CodePointAt(s, p, &len);
    std::size_t i = p + len;
    while (i < s.size()) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '@' || c == '<' || c == '{' || c == '}' || c == '*') break;
        ++i;
    }
    return i;
}

/**
 * openTagPattern / closeTagPattern: "<(?:names)\\b[^>]*>" (or "</...") with
 * CASE_INSENSITIVE. `names` are the lower-case alternatives; a '#' in a name
 * stands for [1-6] (for "h[1-6]"). `next_gt` caches the next '>' position.
 */
class TagMatcher {
   public:
    explicit TagMatcher(std::string_view s) : s_(s) {}

    std::size_t Match(std::size_t p, bool close, std::initializer_list<std::string_view> names) {
        std::size_t i = p;
        if (i >= s_.size() || s_[i] != '<') return npos;
        ++i;
        if (close) {
            if (i >= s_.size() || s_[i] != '/') return npos;
            ++i;
        }
        for (std::string_view name : names) {
            if (!NameAt(i, name)) continue;
            std::size_t e = i + name.size();
            if (!WordBoundaryAfterWordChar(s_, e)) continue;
            std::size_t gt = NextGt(e);
            return gt == npos ? npos : gt + 1;
        }
        return npos;
    }

   private:
    bool NameAt(std::size_t i, std::string_view name) const {
        if (s_.size() - i < name.size()) return false;
        for (std::size_t k = 0; k < name.size(); ++k) {
            char c = AsciiLower(s_[i + k]);
            if (name[k] == '#') {
                if (c < '1' || c > '6') return false;
            } else if (c != name[k]) {
                return false;
            }
        }
        return true;
    }

    std::size_t NextGt(std::size_t from) {
        if (cache_valid_ && from >= cache_from_ && (cache_gt_ == npos || from <= cache_gt_)) return cache_gt_;
        cache_from_ = from;
        cache_gt_ = s_.find('>', from);
        cache_valid_ = true;
        return cache_gt_;
    }

    std::string_view s_;
    bool cache_valid_ = false;
    std::size_t cache_from_ = 0;
    std::size_t cache_gt_ = npos;
};

// ---------------------------------------------------------------------------
// CharStream.java

class CharStream {
   public:
    explicit CharStream(std::string_view input) : input_(input) {}

    bool tryConsume(std::string_view expected) {
        if (!StartsWithAt(input_, position_, expected)) return false;
        token_end_ = position_ + expected.size();
        return true;
    }

    /** tryConsumeRegex: `end` is a matcher's result at position(). */
    bool tryConsumeEnd(std::size_t end) {
        if (end == npos) return false;
        token_end_ = end;
        return true;
    }

    std::string readAndResetRecorded() {
        if (token_end_ == npos || token_end_ < position_) throw InternalError{};
        std::string result(input_.substr(position_, token_end_ - position_));
        position_ = token_end_;
        token_end_ = npos;
        return result;
    }

    bool isExhausted() const { return position_ == input_.size(); }
    std::size_t position() const { return position_; }
    std::string_view text() const { return input_; }

   private:
    std::string_view input_;
    std::size_t position_ = 0;
    std::size_t token_end_ = npos;
};

// ---------------------------------------------------------------------------
// JavadocLexer.java

constexpr ContextSet kTagContexts = {NestingContext::SNIPPET_CONTEXT, NestingContext::INLINE_TAG_CONTEXT};
constexpr ContextSet kBraceContexts = {NestingContext::SNIPPET_CONTEXT, NestingContext::INLINE_TAG_CONTEXT,
                                       NestingContext::BRACE_CONTEXT};
constexpr ContextSet kPreserveFormattingContexts = {NestingContext::HTML_PRE_CONTEXT, NestingContext::TABLE,
                                                    NestingContext::HTML_CODE_CONTEXT,
                                                    NestingContext::SNIPPET_CONTEXT};

bool HasMultipleNewlines(std::string_view s) { return std::count(s.begin(), s.end(), '\n') > 1; }

/** NON_UNIX_LINE_ENDING = "\r\n?" replaced by "\n". */
std::string NormalizeLineEndings(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '\r') {
            out += '\n';
            if (i + 1 < input.size() && input[i + 1] == '\n') ++i;
        } else {
            out += input[i];
        }
    }
    return out;
}

std::string StripJavadocBeginAndEnd(const std::string &input) {
    if (!StartsWithAt(input, 0, "/**")) throw InternalError{};
    if (!(input.size() >= 2 && input.compare(input.size() - 2, 2, "*/") == 0 && Utf16Length(input) > 4)) {
        throw InternalError{};
    }
    return input.substr(3, input.size() - 5);
}

class JavadocLexer {
   public:
    explicit JavadocLexer(std::string_view input) : input_(input), tags_(input) {}

    std::vector<Token> generateTokens() {
        std::vector<Token> tokens;
        tokens.push_back({Kind::BeginJavadoc, "/**"});
        while (!input_.isExhausted()) {
            Kind kind = consumeToken();
            tokens.push_back({kind, input_.readAndResetRecorded()});
        }
        checkMatchingTags();
        tokens.push_back({Kind::EndJavadoc, "*/"});

        std::vector<Token> result = joinAdjacentLiteralsAndAdjacentWhitespace(tokens);
        result = inferParagraphTags(result);
        result = optionalizeSpacesAfterLinks(result);
        result = deindentPreCodeBlocks(result);
        return result;
    }

   private:
    std::size_t pos() const { return input_.position(); }
    std::string_view text() const { return input_.text(); }

    bool tryOpenTag(std::initializer_list<std::string_view> names) {
        return input_.tryConsumeEnd(tags_.Match(pos(), false, names));
    }
    bool tryCloseTag(std::initializer_list<std::string_view> names) {
        return input_.tryConsumeEnd(tags_.Match(pos(), true, names));
    }
    /**
     * MatchClassicNewline, but remembering where the current run of spaces/tabs
     * ends so a long run without a newline is not rescanned at every space.
     */
    std::size_t matchClassicNewline() {
        std::size_t p = pos();
        std::string_view s = text();
        if (p < s.size() && (s[p] == ' ' || s[p] == '\t')) {
            if (!(p >= run_start_ && p < run_end_)) {
                run_start_ = p;
                run_end_ = p;
                while (run_end_ < s.size() && (s[run_end_] == ' ' || s[run_end_] == '\t')) ++run_end_;
            }
            if (run_end_ >= s.size() || s[run_end_] != '\n') return npos;
        }
        return MatchClassicNewline(s, p);
    }

    bool tryLiteral() { return input_.tryConsumeEnd(MatchClassicLiteral(text(), pos())); }

    Kind consumeToken() {
        bool preserve = preserveExistingFormatting();

        if (input_.tryConsumeEnd(matchClassicNewline())) {
            something_since_newline_ = false;
            return preserve ? Kind::ForcedNewline : Kind::Whitespace;
        } else if (input_.tryConsume(" ") || input_.tryConsume("\t")) {
            return preserve ? Kind::Literal : Kind::Whitespace;
        }

        if (!something_since_newline_ && input_.tryConsumeEnd(MatchFooterTag(text(), pos()))) {
            checkMatchingTags();
            something_since_newline_ = true;
            return Kind::FooterJavadocTagStart;
        }
        something_since_newline_ = true;

        if (input_.tryConsumeEnd(MatchSnippetOpen(text(), pos()))) {
            if (context_stack_.containsAny(kBraceContexts)) {
                context_stack_.push(NestingContext::BRACE_CONTEXT);
                return Kind::Literal;
            }
            context_stack_.push(NestingContext::SNIPPET_CONTEXT);
            return Kind::SnippetBegin;
        } else if (input_.tryConsumeEnd(MatchInlineTagOpen(text(), pos()))) {
            context_stack_.push(NestingContext::INLINE_TAG_CONTEXT);
            return Kind::Literal;
        } else if (input_.tryConsume("{")) {
            if (context_stack_.containsAny(kBraceContexts)) context_stack_.push(NestingContext::BRACE_CONTEXT);
            return Kind::Literal;
        } else if (input_.tryConsume("}")) {
            auto popped = context_stack_.popIfIn(kBraceContexts);
            if (popped == NestingContext::SNIPPET_CONTEXT) return Kind::SnippetEnd;
            return Kind::Literal;
        }

        // Inside an inline tag, don't do any HTML interpretation.
        if (context_stack_.containsAny(kTagContexts)) {
            if (!tryLiteral()) throw InternalError{};
            return Kind::Literal;
        }

        if (tryOpenTag({"pre"})) {
            context_stack_.push(NestingContext::HTML_PRE_CONTEXT);
            return preserve ? Kind::Literal : Kind::PreOpenTag;
        } else if (tryCloseTag({"pre"})) {
            context_stack_.popUntil(NestingContext::HTML_PRE_CONTEXT);
            return preserveExistingFormatting() ? Kind::Literal : Kind::PreCloseTag;
        }

        if (tryOpenTag({"code"})) {
            context_stack_.push(NestingContext::HTML_CODE_CONTEXT);
            return preserve ? Kind::Literal : Kind::CodeOpenTag;
        } else if (tryCloseTag({"code"})) {
            context_stack_.popUntil(NestingContext::HTML_CODE_CONTEXT);
            return preserveExistingFormatting() ? Kind::Literal : Kind::CodeCloseTag;
        }

        if (tryOpenTag({"table"})) {
            context_stack_.push(NestingContext::TABLE);
            return preserve ? Kind::Literal : Kind::TableOpenTag;
        } else if (tryCloseTag({"table"})) {
            context_stack_.popUntil(NestingContext::TABLE);
            return preserveExistingFormatting() ? Kind::Literal : Kind::TableCloseTag;
        }

        if (preserve) {
            if (!tryLiteral()) throw InternalError{};
            return Kind::Literal;
        }

        if (tryOpenTag({"p"})) return Kind::ParagraphOpenTag;
        if (tryCloseTag({"p"})) return Kind::ParagraphCloseTag;
        if (tryOpenTag({"ul", "ol", "dl"})) return Kind::ListOpenTag;
        if (tryCloseTag({"ul", "ol", "dl"})) return Kind::ListCloseTag;
        if (tryOpenTag({"li", "dt", "dd"})) return Kind::ListItemOpenTag;
        if (tryCloseTag({"li", "dt", "dd"})) return Kind::ListItemCloseTag;
        if (tryOpenTag({"blockquote"})) return Kind::BlockQuoteOpenTag;
        if (tryCloseTag({"blockquote"})) return Kind::BlockQuoteCloseTag;
        if (tryOpenTag({"h#"})) return Kind::HeaderOpenTag;
        if (tryCloseTag({"h#"})) return Kind::HeaderCloseTag;
        if (tryOpenTag({"br"})) return Kind::BrTag;
        if (input_.tryConsumeEnd(MatchMoeStripComment(text(), pos(), "begin_intracomment_strip"))) {
            return Kind::MoeBeginStripComment;
        }
        if (input_.tryConsumeEnd(MatchMoeStripComment(text(), pos(), "end_intracomment_strip"))) {
            return Kind::MoeEndStripComment;
        }
        if (input_.tryConsumeEnd(MatchHtmlComment(text(), pos()))) return Kind::HtmlComment;
        if (tryLiteral()) return Kind::Literal;
        throw InternalError{};
    }

    bool preserveExistingFormatting() const { return context_stack_.containsAny(kPreserveFormattingContexts); }

    void checkMatchingTags() const {
        if (!context_stack_.isEmpty()) throw LexException{};
    }

    static std::string consumeAdjacentWhitespace(const std::vector<Token> &in, std::size_t &i) {
        std::string seen;
        while (i < in.size() && in[i].kind == Kind::Whitespace) seen += in[i++].value;
        return seen;
    }

    static std::vector<Token> joinAdjacentLiteralsAndAdjacentWhitespace(const std::vector<Token> &in) {
        std::vector<Token> out;
        std::string accumulated;
        std::size_t i = 0;
        while (i < in.size()) {
            if (in[i].kind == Kind::Literal) {
                accumulated += in[i++].value;
                continue;
            }
            if (accumulated.empty()) {
                if (in[i].kind == Kind::Whitespace) {
                    out.push_back({Kind::Whitespace, consumeAdjacentWhitespace(in, i)});
                } else {
                    out.push_back(in[i++]);
                }
                continue;
            }
            std::string seen = consumeAdjacentWhitespace(in, i);
            if (i >= in.size()) throw InternalError{};  // PeekingIterator.peek() past the end
            if (in[i].kind == Kind::Literal && StartsWithAt(in[i].value, 0, "@")) {
                accumulated += ' ';
                accumulated += in[i++].value;
                continue;
            }
            out.push_back({Kind::Literal, accumulated});
            accumulated.clear();
            if (!seen.empty()) out.push_back({Kind::Whitespace, seen});
        }
        return out;
    }

    static std::vector<Token> inferParagraphTags(const std::vector<Token> &in) {
        std::vector<Token> out;
        std::size_t i = 0;
        while (i < in.size()) {
            if (in[i].kind == Kind::Literal) {
                out.push_back(in[i++]);
                if (i >= in.size()) throw InternalError{};
                if (in[i].kind == Kind::Whitespace && HasMultipleNewlines(in[i].value)) {
                    out.push_back(in[i++]);
                    if (i >= in.size()) throw InternalError{};
                    if (in[i].kind == Kind::Literal) out.push_back({Kind::ParagraphOpenTag, "<p>"});
                }
            } else {
                out.push_back(in[i++]);
            }
        }
        return out;
    }

    /** value().matches("href=[^>]*>") */
    static bool IsHrefLiteral(std::string_view v) {
        return v.size() >= 6 && StartsWithAt(v, 0, "href=") && v.find('>') == v.size() - 1;
    }

    static std::vector<Token> optionalizeSpacesAfterLinks(const std::vector<Token> &in) {
        std::vector<Token> out;
        std::size_t i = 0;
        while (i < in.size()) {
            if (in[i].kind == Kind::Literal && IsHrefLiteral(in[i].value)) {
                out.push_back(in[i++]);
                if (i >= in.size()) throw InternalError{};
                if (in[i].kind == Kind::Whitespace) out.push_back({Kind::OptionalLineBreak, in[i++].value});
            } else {
                out.push_back(in[i++]);
            }
        }
        return out;
    }

    /** value().matches("[ \t]*[{]@code") */
    static bool IsPreCodeStart(std::string_view v) {
        std::size_t i = 0;
        while (i < v.size() && (v[i] == ' ' || v[i] == '\t')) ++i;
        return v.substr(i) == "{@code";
    }

    static std::vector<Token> deindentPreCodeBlocks(const std::vector<Token> &in) {
        std::vector<Token> out;
        std::size_t i = 0;
        while (i < in.size()) {
            if (in[i].kind != Kind::PreOpenTag) {
                out.push_back(in[i++]);
                continue;
            }
            out.push_back(in[i++]);
            std::vector<Token> initial_newlines;
            while (i < in.size() && in[i].kind == Kind::ForcedNewline) initial_newlines.push_back(in[i++]);
            if (i >= in.size()) throw InternalError{};
            if (!(in[i].kind == Kind::Literal && IsPreCodeStart(in[i].value))) {
                out.insert(out.end(), initial_newlines.begin(), initial_newlines.end());
                out.push_back(in[i++]);
                continue;
            }
            deindentPreCodeBlock(out, in, i);
        }
        return out;
    }

    static void deindentPreCodeBlock(std::vector<Token> &out, const std::vector<Token> &in, std::size_t &i) {
        std::deque<Token> saved;
        out.push_back({Kind::Literal, JavaTrim(in[i++].value)});
        while (i < in.size() && in[i].kind != Kind::PreCloseTag) saved.push_back(in[i++]);
        while (!saved.empty() && saved.front().kind == Kind::ForcedNewline) saved.pop_front();
        while (!saved.empty() && saved.back().kind == Kind::ForcedNewline) saved.pop_back();
        if (saved.empty()) return;

        // move the trailing `}` to its own line
        Token last = saved.back();
        bool trailing_brace = false;
        if (last.kind == Kind::Literal && !last.value.empty() && last.value.back() == '}') {
            saved.pop_back();
            if (last.length() > 1) {
                saved.push_back({Kind::Literal, last.value.substr(0, last.value.size() - 1)});
                saved.push_back({Kind::ForcedNewline, ""});  // ForcedNewline(null)
            }
            trailing_brace = true;
        }

        // CharMatcher.isNot(' ').indexIn: the prefix before it is all ASCII spaces,
        // so the byte index equals the UTF-16 index.
        int trim = -1;
        for (const Token &t : saved) {
            if (t.kind != Kind::Literal) continue;
            std::size_t idx = t.value.find_first_not_of(' ');
            if (idx != npos) {
                int k = static_cast<int>(idx);
                if (trim == -1 || k < trim) trim = k;
            }
        }

        out.push_back({Kind::ForcedNewline, "\n"});
        for (const Token &t : saved) {
            if (t.kind == Kind::Literal) {
                bool cut = trim > 0 && t.length() > trim;
                out.push_back({Kind::Literal, cut ? t.value.substr(static_cast<std::size_t>(trim)) : t.value});
            } else {
                out.push_back(t);
            }
        }
        if (trailing_brace) {
            out.push_back({Kind::Literal, "}"});
        } else {
            out.push_back({Kind::ForcedNewline, "\n"});
        }
    }

    CharStream input_;
    TagMatcher tags_;
    NestingStack context_stack_;
    bool something_since_newline_ = false;
    std::size_t run_start_ = 0;
    std::size_t run_end_ = 0;
};

std::vector<Token> Lex(std::string_view raw) {
    std::string input = StripJavadocBeginAndEnd(NormalizeLineEndings(raw));
    return JavadocLexer(input).generateTokens();
}

// ---------------------------------------------------------------------------
// JavadocWriter.java (classic Javadoc only)

enum class RequestedWhitespace { NONE, WHITESPACE, NEWLINE, BLANK_LINE };
enum class AutoIndent { AUTO_INDENT, NO_AUTO_INDENT };

class JavadocWriter {
   public:
    explicit JavadocWriter(int block_indent) : block_indent_(block_indent) {}

    void requestWhitespace() { requestWhitespace(RequestedWhitespace::WHITESPACE); }

    void requestMoeBeginStripComment(const Token &token) { requested_moe_begin_strip_comment_ = token; }

    void writeBeginJavadoc() {
        output_ += "/**";
        writeNewline();
    }

    void writeEndJavadoc() {
        output_ += "\n";
        appendSpaces(block_indent_ + 1);
        output_ += "*/";
    }

    void writeFooterJavadocTagStart(const Token &token) {
        continuing_list_item_of_innermost_list_ = false;
        continuing_list_item_stack_.reset();
        continuing_list_stack_.reset();
        post_write_modified_continuing_list_stack_.reset();
        if (!continuing_footer_tag_) {
            requestBlankLine();
        } else {
            continuing_footer_tag_ = false;
            requestNewline();
        }
        writeToken(token);
        continuing_footer_tag_ = true;
    }

    void writeSnippetBegin(const Token &token) {
        requestBlankLine();
        writeToken(token);
    }

    void writeSnippetEnd(const Token &token) {
        writeToken(token);
        requestBlankLine();
    }

    void writeListOpen(const Token &token) {
        requestBlankLine();
        writeToken(token);
        continuing_list_item_of_innermost_list_ = false;
        continuing_list_stack_.push(token.value.empty() ? 0 : 2);
        post_write_modified_continuing_list_stack_.push();
        requestNewline();
    }

    void writeListClose(const Token &token) {
        requestNewline();
        continuing_list_item_stack_.popIfNotEmpty();
        continuing_list_stack_.popIfNotEmpty();
        writeToken(token);
        post_write_modified_continuing_list_stack_.popIfNotEmpty();
        requestBlankLine();
    }

    void writeListItemOpen(const Token &token) {
        requestNewline();
        if (continuing_list_item_of_innermost_list_) {
            continuing_list_item_of_innermost_list_ = false;
            continuing_list_item_stack_.popIfNotEmpty();
        }
        writeToken(token);
        continuing_list_item_of_innermost_list_ = true;
        continuing_list_item_stack_.push(token.length());
    }

    void writeHeaderOpen(const Token &token) {
        requestBlankLine();
        writeToken(token);
    }

    void writeHeaderClose(const Token &token) {
        writeToken(token);
        requestBlankLine();
    }

    void writeParagraphOpen(const Token &token) {
        if (!wrote_anything_significant_) return;
        requestBlankLine();
        writeToken(token);
    }

    void writeBlockQuoteOpen(const Token &token) {
        requestBlankLine();
        writeToken(token);
        requestNewline();
    }

    void writeBlockQuoteClose(const Token &token) {
        requestNewline();
        writeToken(token);
        requestBlankLine();
    }

    void writePreOpen(const Token &token) {
        requestBlankLine();
        writeToken(token);
    }

    void writePreClose(const Token &token) {
        writeToken(token);
        requestBlankLine();
    }

    void writeCodeOpen(const Token &token) { writeToken(token); }
    void writeCodeClose(const Token &token) { writeToken(token); }

    void writeTableOpen(const Token &token) {
        requestBlankLine();
        writeToken(token);
    }

    void writeTableClose(const Token &token) {
        writeToken(token);
        requestBlankLine();
    }

    void writeMoeEndStripComment(const Token &token) {
        writeLineBreakNoAutoIndent();
        appendSpaces(indent_for_moe_end_strip_comment_);
        writeToken(token);
        requestNewline();
    }

    void writeHtmlComment(const Token &token) {
        requestNewline();
        writeToken(token);
        requestNewline();
    }

    void writeBr(const Token &token) {
        writeToken(token);
        requestNewline();
    }

    void writeLineBreakNoAutoIndent() { writeNewline(AutoIndent::NO_AUTO_INDENT); }

    void writeLiteral(const Token &token) { writeToken(token); }

    std::string &str() { return output_; }

   private:
    void requestWhitespace(RequestedWhitespace w) { requested_whitespace_ = std::max(w, requested_whitespace_); }
    void requestBlankLine() { requestWhitespace(RequestedWhitespace::BLANK_LINE); }
    void requestNewline() { requestWhitespace(RequestedWhitespace::NEWLINE); }

    void flushWhitespace() {
        if (requested_moe_begin_strip_comment_) requestNewline();
        if (!wrote_anything_significant_) requested_whitespace_ = RequestedWhitespace::NONE;
        if (requested_whitespace_ == RequestedWhitespace::BLANK_LINE &&
            (!post_write_modified_continuing_list_stack_.isEmpty() || continuing_footer_tag_)) {
            requested_whitespace_ = RequestedWhitespace::NEWLINE;
        }
        if (requested_whitespace_ == RequestedWhitespace::BLANK_LINE) {
            writeBlankLine();
            requested_whitespace_ = RequestedWhitespace::NONE;
        } else if (requested_whitespace_ == RequestedWhitespace::NEWLINE) {
            writeNewline();
            requested_whitespace_ = RequestedWhitespace::NONE;
        }
    }

    void writeToken(const Token &token) {
        if (token.value.empty()) return;
        flushWhitespace();
        bool need_whitespace = requested_whitespace_ == RequestedWhitespace::WHITESPACE;
        int len = token.length();
        if (!at_start_of_line_ && len + (need_whitespace ? 1 : 0) > remaining_on_line_) writeNewline();
        if (!at_start_of_line_ && need_whitespace) {
            output_ += ' ';
            remaining_on_line_--;
        }
        if (requested_moe_begin_strip_comment_) {
            output_ += requested_moe_begin_strip_comment_->value;
            requested_moe_begin_strip_comment_.reset();
            indent_for_moe_end_strip_comment_ = innerIndent();
            wrote_anything_significant_ = true;
            requestNewline();
            writeToken(token);  // recurses once: the request was just cleared
            return;
        }
        output_ += token.value;
        if (!IsStartOfLineToken(token.kind)) at_start_of_line_ = false;
        remaining_on_line_ -= len;
        requested_whitespace_ = RequestedWhitespace::NONE;
        wrote_anything_significant_ = true;
    }

    void writeNewlineStart() {
        output_ += '\n';
        appendSpaces(block_indent_ + 1);
        output_ += '*';
    }

    void writeBlankLine() {
        writeNewlineStart();
        writeNewline();
    }

    void writeNewline(AutoIndent auto_indent = AutoIndent::AUTO_INDENT) {
        writeNewlineStart();
        appendSpaces(1);
        remaining_on_line_ = kMaxLineLength - block_indent_ - 3;
        if (auto_indent == AutoIndent::AUTO_INDENT) {
            appendSpaces(innerIndent());
            remaining_on_line_ -= innerIndent();
        }
        at_start_of_line_ = true;
    }

    int innerIndent() const {
        int inner = continuing_list_item_stack_.total() + continuing_list_stack_.total();
        if (continuing_footer_tag_) inner += 4;
        return inner;
    }

    void appendSpaces(int count) {
        if (count > 0) output_.append(static_cast<std::size_t>(count), ' ');
    }

    int block_indent_;
    std::string output_;
    bool continuing_list_item_of_innermost_list_ = false;
    bool continuing_footer_tag_ = false;
    IntNestingStack continuing_list_item_stack_;
    IntNestingStack continuing_list_stack_;
    IntNestingStack post_write_modified_continuing_list_stack_;
    int remaining_on_line_ = 0;
    bool at_start_of_line_ = false;
    RequestedWhitespace requested_whitespace_ = RequestedWhitespace::NONE;
    std::optional<Token> requested_moe_begin_strip_comment_;
    int indent_for_moe_end_strip_comment_ = 0;
    bool wrote_anything_significant_ = false;
};

// ---------------------------------------------------------------------------
// JavadocFormatter.java

/** SIMPLE_TAG_PATTERN = "^<\\w+\\s*\/?\\s*>" (CASE_INSENSITIVE), matches(). */
bool IsSimpleTag(std::string_view v) {
    std::size_t i = 0;
    if (v.empty() || v[0] != '<') return false;
    ++i;
    std::size_t w = i;
    while (i < v.size() && IsAsciiWord(v[i])) ++i;
    if (i == w) return false;
    while (i < v.size() && IsJavaSpace(v[i])) ++i;
    if (i < v.size() && v[i] == '/') ++i;
    while (i < v.size() && IsJavaSpace(v[i])) ++i;
    return i + 1 == v.size() && v[i] == '>';
}

Token Standardize(const Token &token, std::string_view standard) {
    return IsSimpleTag(token.value) ? Token{token.kind, std::string(standard)} : token;
}

std::string Render(const std::vector<Token> &input, int block_indent) {
    JavadocWriter output(block_indent);
    for (const Token &token : input) {
        switch (token.kind) {
            case Kind::BeginJavadoc:
                output.writeBeginJavadoc();
                break;
            case Kind::EndJavadoc:
                output.writeEndJavadoc();
                return std::move(output.str());
            case Kind::FooterJavadocTagStart:
                output.writeFooterJavadocTagStart(token);
                break;
            case Kind::SnippetBegin:
                output.writeSnippetBegin(token);
                break;
            case Kind::SnippetEnd:
                output.writeSnippetEnd(token);
                break;
            case Kind::ListOpenTag:
                output.writeListOpen(token);
                break;
            case Kind::ListCloseTag:
                output.writeListClose(token);
                break;
            case Kind::ListItemOpenTag:
                output.writeListItemOpen(token);
                break;
            case Kind::HeaderOpenTag:
                output.writeHeaderOpen(token);
                break;
            case Kind::HeaderCloseTag:
                output.writeHeaderClose(token);
                break;
            case Kind::ParagraphOpenTag:
                output.writeParagraphOpen(Standardize(token, "<p>"));
                break;
            case Kind::BlockQuoteOpenTag:
                output.writeBlockQuoteOpen(token);
                break;
            case Kind::BlockQuoteCloseTag:
                output.writeBlockQuoteClose(token);
                break;
            case Kind::PreOpenTag:
                output.writePreOpen(token);
                break;
            case Kind::PreCloseTag:
                output.writePreClose(token);
                break;
            case Kind::CodeOpenTag:
                output.writeCodeOpen(token);
                break;
            case Kind::CodeCloseTag:
                output.writeCodeClose(token);
                break;
            case Kind::TableOpenTag:
                output.writeTableOpen(token);
                break;
            case Kind::TableCloseTag:
                output.writeTableClose(token);
                break;
            case Kind::MoeBeginStripComment:
                output.requestMoeBeginStripComment(token);
                break;
            case Kind::MoeEndStripComment:
                output.writeMoeEndStripComment(token);
                break;
            case Kind::HtmlComment:
                output.writeHtmlComment(token);
                break;
            case Kind::BrTag:
                output.writeBr(Standardize(token, "<br>"));
                break;
            case Kind::Whitespace:
                output.requestWhitespace();
                break;
            case Kind::ForcedNewline:
                output.writeLineBreakNoAutoIndent();
                break;
            case Kind::Literal:
                output.writeLiteral(token);
                break;
            case Kind::ListItemCloseTag:
            case Kind::OptionalLineBreak:
            case Kind::ParagraphCloseTag:
                break;
        }
    }
    throw InternalError{};  // AssertionError: no EndJavadoc
}

/** `.` without DOTALL: false for \n \r U+0085 U+2028 U+2029. */
bool HasLineTerminator(std::string_view s) {
    if (s.find_first_of("\n\r") != npos) return true;
    return s.find("\xC2\x85") != npos || s.find("\xE2\x80\xA8") != npos || s.find("\xE2\x80\xA9") != npos;
}

bool OneLineJavadoc(std::string_view line, int block_indent) {
    int one_liner_content_length = kMaxLineLength - 7 - block_indent;  // "/**  */".length()
    if (Utf16Length(line) > one_liner_content_length) return false;
    if (StartsWithAt(line, 0, "@") && line != "@hide") return false;
    return true;
}

/** ONE_CONTENT_LINE_PATTERN = " *\/[*][*]\n *[*] (.*)\n *[*]/", matches(). */
std::string MakeSingleLineIfPossible(int block_indent, std::string input) {
    std::string_view s = input;
    std::size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    if (!StartsWithAt(s, i, "/**\n")) return input;
    i += 4;
    while (i < s.size() && s[i] == ' ') ++i;
    if (!StartsWithAt(s, i, "* ")) return input;
    i += 2;
    std::size_t nl = s.find('\n', i);
    if (nl == npos) return input;
    std::string_view line = s.substr(i, nl - i);
    if (HasLineTerminator(line)) return input;
    std::size_t j = nl + 1;
    while (j < s.size() && s[j] == ' ') ++j;
    if (s.substr(j) != "*/") return input;
    if (line.empty()) return "/** */";
    if (OneLineJavadoc(line, block_indent)) return "/** " + std::string(line) + " */";
    return input;
}

}  // namespace

std::string FormatJavadoc(std::string_view input, int block_indent) {
    try {
        std::vector<Token> tokens;
        try {
            tokens = Lex(input);
        } catch (const LexException &) {
            return std::string(input);
        }
        return MakeSingleLineIfPossible(block_indent, Render(tokens, block_indent));
    } catch (const InternalError &) {
        return std::string(input);
    }
}

}  // namespace javafmt::gjf
