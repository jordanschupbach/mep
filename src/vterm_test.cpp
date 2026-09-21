// Windowless test for vterm.cpp's Unicode width handling: VTermCharWidth
// classification, zero-width (variation selector/combining mark) drops in
// PutChar, and double-width (CJK/emoji) lead+continuation pair integrity
// across overwrite, wrap, erase, insert/delete, and resize -- what keeps
// mep's :terminal grid column-aligned with the wcwidth arithmetic a child
// process (starship, TUIs) does on its own output.
// CHECK(), never assert(): the Release build strips assert() entirely.
#include "vterm.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

// UTF-8 literals used throughout: pinned as named constants so the test
// bodies read as intent rather than byte soup.
const std::string kSnowflake = "❄";     // U+2744, narrow
const std::string kVs16 = "️";          // U+FE0F variation selector-16, zero-width
const std::string kSauropod = "\U0001F995";  // U+1F995, wide (emoji)
const std::string kHan = "一";           // U+4E00, wide (CJK)
const std::string kAcute = "́";         // U+0301 combining acute, zero-width
}  // namespace

int main() {
    // --- VTermCharWidth classification, including range boundaries.
    {
        CHECK(VTermCharWidth('a') == 1);
        CHECK(VTermCharWidth(0x25B3) == 1);     // white up triangle (starship cmake)
        CHECK(VTermCharWidth(0x2744) == 1);     // snowflake: narrow base (VS16 dropped separately)
        CHECK(VTermCharWidth(0x276F) == 1);     // starship prompt char
        CHECK(VTermCharWidth(0xE0A0) == 1);     // Powerline branch glyph (PUA)
        CHECK(VTermCharWidth(0xFE0F) == 0);     // variation selector-16
        CHECK(VTermCharWidth(0x200D) == 0);     // ZWJ
        CHECK(VTermCharWidth(0x0301) == 0);     // combining acute
        CHECK(VTermCharWidth(0x0300) == 0);     // combining range start boundary
        CHECK(VTermCharWidth(0x036F) == 0);     // combining range end boundary
        CHECK(VTermCharWidth(0x02FF) == 1);     // just below combining range
        CHECK(VTermCharWidth(0x0370) == 1);     // just above combining range
        CHECK(VTermCharWidth(0x4E00) == 2);     // CJK
        CHECK(VTermCharWidth(0x1F995) == 2);    // sauropod emoji
        CHECK(VTermCharWidth(0x26A1) == 2);     // high voltage: emoji-presentation
        CHECK(VTermCharWidth(0xAC00) == 2);     // Hangul syllables start boundary
        CHECK(VTermCharWidth(0xD7A3) == 2);     // Hangul syllables end boundary
        CHECK(VTermCharWidth(0x1FAFF) == 2);    // extended-A pictographs end boundary
        CHECK(VTermCharWidth(0x2028) == 1);     // between zero-width ranges
    }

    // --- Zero-width chars consume no cell: "❄️ x" lands as ❄, ' ', 'x'.
    {
        VTerm t(4, 20);
        t.Feed(kSnowflake + kVs16 + " x");
        CHECK(t.At(0, 0).ch == kSnowflake);
        CHECK(t.At(0, 0).width == 1);
        CHECK(t.At(0, 1).ch == " ");
        CHECK(t.At(0, 2).ch == "x");
        CHECK(t.CursorCol() == 3);
    }

    // --- Combining mark after a letter degrades to the bare base letter.
    {
        VTerm t(4, 20);
        t.Feed("e" + kAcute + "f");
        CHECK(t.At(0, 0).ch == "e");
        CHECK(t.At(0, 1).ch == "f");
        CHECK(t.CursorCol() == 2);
    }

    // --- Wide char writes a lead+continuation pair and advances 2.
    {
        VTerm t(4, 20);
        t.Feed(kSauropod);
        CHECK(t.At(0, 0).ch == kSauropod);
        CHECK(t.At(0, 0).width == 2);
        CHECK(t.At(0, 1).ch.empty());
        CHECK(t.At(0, 1).width == 0);
        CHECK(t.CursorCol() == 2);
        t.Feed("x");
        CHECK(t.At(0, 2).ch == "x");
    }

    // --- Overwriting the lead blanks the orphaned continuation.
    {
        VTerm t(4, 20);
        t.Feed(kHan);
        t.Feed("\x1b[1;1H");  // CUP to (0,0)
        t.Feed("x");
        CHECK(t.At(0, 0).ch == "x");
        CHECK(t.At(0, 0).width == 1);
        CHECK(t.At(0, 1).width == 1);  // continuation reset to a normal blank
        CHECK(t.At(0, 1).ch == " ");
    }

    // --- Overwriting the continuation blanks the orphaned lead.
    {
        VTerm t(4, 20);
        t.Feed(kHan);
        t.Feed("\x1b[1;2H");  // CUP to (0,1) -- the continuation cell
        t.Feed("y");
        CHECK(t.At(0, 0).ch == " ");  // torn lead blanked
        CHECK(t.At(0, 0).width == 1);
        CHECK(t.At(0, 1).ch == "y");
    }

    // --- A wide char in the last column wraps xterm-style: orphan column
    //     blanked, glyph written at the start of the next line.
    {
        VTerm t(4, 10);
        t.Feed("\x1b[1;10H");  // CUP to last column of row 0
        t.Feed(kSauropod);
        CHECK(t.At(0, 9).ch == " ");
        CHECK(t.At(0, 9).width == 1);
        CHECK(t.At(1, 0).ch == kSauropod);
        CHECK(t.At(1, 0).width == 2);
        CHECK(t.At(1, 1).width == 0);
        CHECK(t.CursorRow() == 1);
        CHECK(t.CursorCol() == 2);
    }

    // --- A wide char ending exactly at the last column defers wrap, and
    //     the next narrow char wraps to the following line.
    {
        VTerm t(4, 10);
        t.Feed("\x1b[1;9H");  // CUP to col 8 (0-indexed): pair fills cols 8-9
        t.Feed(kHan);
        CHECK(t.At(0, 8).ch == kHan);
        CHECK(t.At(0, 9).width == 0);
        CHECK(t.CursorRow() == 0);  // pending wrap, not yet taken
        t.Feed("z");
        CHECK(t.CursorRow() == 1);
        CHECK(t.At(1, 0).ch == "z");
    }

    // --- EL erasing from the cursor leaves no orphan when the range edge
    //     splits a pair (cursor on the continuation: lead is outside the
    //     erased range and must be blanked).
    {
        VTerm t(4, 20);
        t.Feed(kHan);
        t.Feed("abc");
        t.Feed("\x1b[1;2H");  // cursor onto the continuation cell
        t.Feed("\x1b[K");     // EL 0: erase cursor..end
        CHECK(t.At(0, 0).ch == " ");  // torn lead blanked
        CHECK(t.At(0, 0).width == 1);
        CHECK(t.At(0, 2).ch == " ");
    }

    // --- ECH across a pair's lead blanks the continuation outside the range.
    {
        VTerm t(4, 20);
        t.Feed("ab" + kHan + "cd");
        t.Feed("\x1b[1;3H");  // cursor on the pair's lead (col 2)
        t.Feed("\x1b[1X");    // ECH 1: blank just the lead
        CHECK(t.At(0, 2).ch == " ");
        CHECK(t.At(0, 3).width == 1);  // continuation reset, no orphan
        CHECK(t.At(0, 4).ch == "c");
    }

    // --- DCH pulling a torn continuation to the cursor blanks it.
    {
        VTerm t(4, 20);
        t.Feed("a" + kHan + "b");
        t.Feed("\x1b[1;2H");  // cursor on the pair's lead (col 1)
        t.Feed("\x1b[1P");    // DCH 1: delete the lead; continuation shifts into its place
        CHECK(t.At(0, 1).width != 0);  // no orphan continuation at the seam
        CHECK(t.At(0, 2).ch == "b" || t.At(0, 1).ch == "b");  // row content shifted left
    }

    // --- ICH shifting a pair's lead into the last column blanks it.
    {
        VTerm t(4, 6);
        t.Feed("\x1b[1;5H");  // CUP to col 4: pair fills cols 4-5
        t.Feed(kHan);
        t.Feed("\x1b[1;1H");
        t.Feed("\x1b[1@");    // ICH 1: lead shifts from col 4 to col 5, continuation falls off
        CHECK(t.At(0, 5).width != 2);  // no lead stranded in the last column
    }

    // --- Resize cutting a pair at the new right edge blanks the lead.
    {
        VTerm t(4, 10);
        t.Feed("\x1b[1;9H");  // pair at cols 8-9
        t.Feed(kHan);
        t.Resize(4, 9);       // new last column is 8 -- the lead, now cut
        CHECK(t.At(0, 8).width != 2);
        CHECK(t.At(0, 8).ch == " ");
    }

    // --- UTF-8 sequence split across two Feed calls still classifies width.
    {
        VTerm t(4, 20);
        std::string bytes = kSauropod;  // 4 bytes
        t.Feed(bytes.substr(0, 2));
        t.Feed(bytes.substr(2));
        CHECK(t.At(0, 0).ch == kSauropod);
        CHECK(t.At(0, 0).width == 2);
        CHECK(t.CursorCol() == 2);
    }

    // --- Zero-width split across Feed calls is still dropped.
    {
        VTerm t(4, 20);
        std::string bytes = kVs16;  // 3 bytes
        t.Feed("a");
        t.Feed(bytes.substr(0, 1));
        t.Feed(bytes.substr(1));
        t.Feed("b");
        CHECK(t.At(0, 1).ch == "b");
        CHECK(t.CursorCol() == 2);
    }

    // --- Mouse-tracking + bracketed-paste mode set/reset.
    {
        VTerm t(10, 40);
        CHECK(t.MouseTracking() == VTermMouseTracking::Off);
        CHECK(!t.MouseSgr());
        CHECK(!t.BracketedPaste());
        t.Feed("\x1b[?1000h");
        CHECK(t.MouseTracking() == VTermMouseTracking::Normal);
        t.Feed("\x1b[?1002h");
        CHECK(t.MouseTracking() == VTermMouseTracking::ButtonEvent);
        t.Feed("\x1b[?1003h");
        CHECK(t.MouseTracking() == VTermMouseTracking::AnyMotion);
        t.Feed("\x1b[?1006h");
        CHECK(t.MouseSgr());
        t.Feed("\x1b[?2004h");
        CHECK(t.BracketedPaste());
        t.Feed("\x1b[?1000l");  // resetting any tracking level -> Off
        CHECK(t.MouseTracking() == VTermMouseTracking::Off);
        t.Feed("\x1b[?1006l");
        CHECK(!t.MouseSgr());
        t.Feed("\x1b[?2004l");
        CHECK(!t.BracketedPaste());
    }

    // --- Combined DECSET list ("?1002;1006h") sets tracking and encoding.
    {
        VTerm t(10, 40);
        t.Feed("\x1b[?1002;1006h");
        CHECK(t.MouseTracking() == VTermMouseTracking::ButtonEvent);
        CHECK(t.MouseSgr());
    }

    // --- RIS (ESC c) clears mouse/paste state.
    {
        VTerm t(10, 40);
        t.Feed("\x1b[?1003h\x1b[?1006h\x1b[?2004h");
        t.Feed("\x1b" "c");
        CHECK(t.MouseTracking() == VTermMouseTracking::Off);
        CHECK(!t.MouseSgr());
        CHECK(!t.BracketedPaste());
    }

    // OSC 10/11 describes this terminal's configured defaults to apps
    // such as Codex, so a light embedding theme does not induce a dark
    // prompt background.
    {
        VTerm t(10, 40);
        t.SetOscDefaultColors(VTermColor{VTermColorKind::Rgb, 0, 12, 34, 56},
                             VTermColor{VTermColorKind::Rgb, 0, 250, 240, 230});
        CHECK(!t.QueriesOscDefaultColors());
        CHECK(t.Feed("\x1b]10;?\x07") == "\x1b]10;rgb:0c0c/2222/3838\x1b\\");
        CHECK(!t.QueriesOscDefaultColors());
        CHECK(t.Feed("\x1b]11;?\x07") == "\x1b]11;rgb:fafa/f0f0/e6e6\x1b\\");
        CHECK(t.QueriesOscDefaultColors());
    }

    std::printf("vterm_test: all checks passed\n");
    return 0;
}
