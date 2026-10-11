// text_quality.h: telling a page whose glyphs map to arbitrary letters
// from a page of real text.
//
// The threshold here decides whether asking a model about a page quotes
// its words or sends the page as a picture, so both directions of
// mistake matter: calling real text garbage sends an image nobody asked
// for and loses the exact wording, and calling garbage real text sends
// a prompt quoting "D D C D DD D".
//
// The real-world numbers these assertions encode, measured with
// `mep-pdf-probe quality`: The Art of Computer Programming vol. 4A (all
// Type 3 bitmap fonts, no /ToUnicode) scores 3-5 distinct characters per
// page and 457 of its 901 pages trip the test; across about 2,500 pages
// of real books and every generated fixture, nothing is flagged and no
// page scores below 33.

#include "text_quality.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int g_checks = 0;

// stderr, not stdout: stdout is fully buffered when the test runner
// pipes it and abort() does not flush, so a printf'd failure would print
// nothing at all. Same reasoning as the other tests here.
void Check(bool condition, const char *expression, int line) {
    ++g_checks;
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

std::string Repeat(const std::string &unit, int times) {
    std::string out;
    for (int i = 0; i < times; ++i) out += unit;
    return out;
}

// The shape of a real TAOCP page: a few letters and spaces, nothing else.
void TestMismappedGlyphsAreDegenerate() {
    const std::string garbage = Repeat("D D C D DD D D B D D D\n", 20);
    const textq::Quality q = textq::Measure(garbage);
    CHECK(q.chars >= textq::Quality::kMinChars);
    CHECK(q.distinct <= 5);  // B, C, D and nothing else
    CHECK(q.digits == 0);
    CHECK(q.Degenerate());
}

// Ordinary prose must never be flagged, however long or short.
void TestProseIsNotDegenerate() {
    const std::string prose = Repeat(
        "The quick brown fox jumps over the lazy dog, and the examiner "
        "notes that this sentence uses a wide spread of letters. ", 6);
    const textq::Quality q = textq::Measure(prose);
    CHECK(q.chars >= textq::Quality::kMinChars);
    CHECK(q.distinct > 20);
    CHECK(!q.Degenerate());
}

// A page of numbers -- a results table, a log -- uses few distinct
// characters by nature, and its text is perfectly good. This is what
// the digit guard is for.
void TestNumericTableIsNotDegenerate() {
    const std::string table = Repeat("12.5 -3.7 408.1 0.02 99.9 7.0 -1.3 256.4\n", 12);
    const textq::Quality q = textq::Measure(table);
    CHECK(q.chars >= textq::Quality::kMinChars);
    CHECK(q.distinct <= textq::Quality::kMaxDistinct + 2);  // digits, '.', '-'
    CHECK(q.digits * 4 >= q.chars);                          // mostly digits
    CHECK(!q.Degenerate());
}

// Short text is never judged: a two-word selection, or a heading, can
// legitimately use very few distinct characters.
void TestShortTextIsNeverDegenerate() {
    CHECK(!textq::Measure("SEE ALSO").Degenerate());
    CHECK(!textq::Measure("AAAA").Degenerate());
    CHECK(!textq::Measure("").Degenerate());
    CHECK(textq::Measure("").chars == 0);
    // ...right up to the length floor, so the boundary is where it says.
    const std::string just_under = Repeat("D", static_cast<int>(textq::Quality::kMinChars) - 1);
    CHECK(!textq::Measure(just_under).Degenerate());
    const std::string just_over = Repeat("D", static_cast<int>(textq::Quality::kMinChars));
    CHECK(textq::Measure(just_over).Degenerate());
}

// A non-Latin script must never be mistaken for garbage, however small
// its repeated vocabulary. Any non-ASCII byte exempts the text, which is
// what makes this hold: five Greek letters repeated score only six
// distinct BYTES, so counting those alone called this garbage until the
// exemption was added.
void TestNonLatinScriptIsNotDegenerate() {
    // Greek and CJK, repeated past the length floor and using a
    // deliberately narrow vocabulary -- the hard case, since diversity
    // alone would not save them.
    const std::string greek = Repeat("αβγδε ", 40);
    const textq::Quality gq = textq::Measure(greek);
    CHECK(gq.chars >= textq::Quality::kMinChars);
    CHECK(!gq.Degenerate());
    CHECK(gq.nonascii > 0);
    const std::string cjk = Repeat("漢字のテキスト ", 30);
    const textq::Quality cq = textq::Measure(cjk);
    CHECK(cq.chars >= textq::Quality::kMinChars);
    CHECK(!cq.Degenerate());
    // Even a single typographic character in otherwise-ASCII garbage is
    // enough, which is the conservative direction: better to quote text
    // that might be poor than to replace good text with a picture.
    const textq::Quality mixed = textq::Measure(Repeat("D D C D DD D\n", 20) + "\xe2\x80\x94");
    CHECK(mixed.nonascii > 0);
    CHECK(!mixed.Degenerate());
}

// Whitespace is not content: it must not count toward the length that
// makes a page judgeable, or a mostly-blank page would qualify.
void TestWhitespaceIsNotCounted() {
    const textq::Quality q = textq::Measure(Repeat(" \t\n", 500) + "hello");
    CHECK(q.chars == 5);
    CHECK(!q.Degenerate());
}

// The margin the threshold relies on: real pages measured at 33 distinct
// characters and up (over ~2,500 pages), garbage at 5 and below.
// Asserted so that moving the constant toward either observation is a
// deliberate act rather than a tweak.
void TestThresholdSitsInTheMeasuredGap() {
    CHECK(textq::Quality::kMaxDistinct > 5);
    CHECK(textq::Quality::kMaxDistinct < 33);
}

}  // namespace

int main() {
    TestMismappedGlyphsAreDegenerate();
    TestProseIsNotDegenerate();
    TestNumericTableIsNotDegenerate();
    TestShortTextIsNeverDegenerate();
    TestNonLatinScriptIsNotDegenerate();
    TestWhitespaceIsNotCounted();
    TestThresholdSitsInTheMeasuredGap();
    std::printf("text_quality_test: all %d checks passed\n", g_checks);
    return 0;
}
