// mep's COBOL formatter, driven directly.
//
// `mep-cobol-format-test` runs the checks below;
// `mep-cobol-format-test --format [--indent N] [--no-align] FILE...` prints
// each file formatted ('-' reads stdin; a refusal goes to stderr, exit 1).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <string>

#include "cobol_format.h"

namespace {
int g_checks = 0;

void Check(bool ok, const char *expr, int line) {
    g_checks++;
    if (ok) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expr, __FILE__, line);
    std::abort();
}
#define CHECK(c) Check((c), #c, __LINE__)

int FormatFiles(int argc, char **argv) {
    cobolfmt::Options opts;
    int status = 0;
    for (int k = 2; k < argc; k++) {
        const std::string arg = argv[k];
        if (arg == "--indent" && k + 1 < argc) {
            opts.indent = std::atoi(argv[++k]);
            continue;
        }
        if (arg == "--no-align") {
            opts.align_pic = false;
            continue;
        }
        std::stringstream ss;
        if (arg == "-") {
            ss << std::cin.rdbuf();
        } else {
            std::ifstream in(arg, std::ios::binary);
            ss << in.rdbuf();
        }
        const cobolfmt::Result r = cobolfmt::Format(ss.str(), opts);
        if (!r.ok) {
            std::fprintf(stderr, "%s:%d: %s\n", arg.c_str(), r.error_line, r.error.c_str());
            status = 1;
        }
        std::cout << r.text;
    }
    return status;
}

// Lines joined with newlines (and one at the end).
std::string Lines(std::initializer_list<std::string> lines) {
    std::string s;
    for (const std::string &l : lines) s += l + "\n";
    return s;
}

// Fixed format: code in Area A (column 8), or at Area B (column 12) plus `depth` indents.
std::string A(const std::string &code) { return "       " + code; }
std::string B(int depth, const std::string &code) { return std::string(static_cast<size_t>(11 + depth * 4), ' ') + code; }
// Free format: code `depth` indents in.
std::string F(int depth, const std::string &code) { return std::string(static_cast<size_t>(depth * 4), ' ') + code; }
// `head` padded with spaces to 0-based column `col`, then `tail`.
std::string At(const std::string &head, int col, const std::string &tail) {
    std::string s = head;
    if (static_cast<int>(s.size()) < col) s += std::string(static_cast<size_t>(col) - s.size(), ' ');
    return s + tail;
}

std::string Fmt(const std::string &src, const cobolfmt::Options &opts = {}) {
    const cobolfmt::Result r = cobolfmt::Format(src, opts);
    if (!r.ok) std::fprintf(stderr, "refused: %s (line %d)\n", r.error.c_str(), r.error_line);
    CHECK(r.ok);
    // Idempotent: formatting the result changes nothing.
    const cobolfmt::Result again = cobolfmt::Format(r.text, opts);
    CHECK(again.ok);
    if (again.text != r.text) std::fprintf(stderr, "not idempotent:\n%s---\n%s", r.text.c_str(), again.text.c_str());
    CHECK(again.text == r.text);
    return r.text;
}

void ExpectFmt(const std::string &src, const std::string &want, int line, const cobolfmt::Options &opts = {}) {
    const std::string got = Fmt(src, opts);
    if (got != want) {
        std::fprintf(stderr, "line %d: got:\n%s---\nwant:\n%s---\n", line, got.c_str(), want.c_str());
    }
    Check(got == want, "formatted text", line);
}
#define EXPECT_FMT(src, want, ...) ExpectFmt((src), (want), __LINE__ __VA_OPT__(, ) __VA_ARGS__)

void TestDivisions() {
    EXPECT_FMT(Lines({"         IDENTIFICATION DIVISION.", "          PROGRAM-ID. HELLO.",
                      "        PROCEDURE DIVISION.", "        MAIN-PARA.", "               DISPLAY \"HI\".",
                      "         STOP RUN.", "         END PROGRAM HELLO."}),
               Lines({A("IDENTIFICATION DIVISION."), A("PROGRAM-ID. HELLO."), A("PROCEDURE DIVISION."),
                      A("MAIN-PARA."), B(0, "DISPLAY \"HI\"."), B(0, "STOP RUN."), A("END PROGRAM HELLO.")}));
    // Already formatted: unchanged.
    const std::string done = Lines({A("IDENTIFICATION DIVISION."), A("PROGRAM-ID. X."), A("ENVIRONMENT DIVISION."),
                                    A("CONFIGURATION SECTION."), A("SOURCE-COMPUTER. GNUCOBOL."),
                                    A("INPUT-OUTPUT SECTION."), A("FILE-CONTROL."),
                                    B(0, "SELECT IN-FILE ASSIGN TO \"in.dat\""),
                                    B(1, "ORGANIZATION IS LINE SEQUENTIAL."), A("DATA DIVISION."),
                                    A("WORKING-STORAGE SECTION."), A("PROCEDURE DIVISION."), B(0, "GOBACK.")});
    EXPECT_FMT(done, done);
    // Environment entries move to Area B; their continuation lines keep their offset.
    EXPECT_FMT(Lines({A("ENVIRONMENT DIVISION."), A("INPUT-OUTPUT SECTION."), A(" FILE-CONTROL."),
                      A("SELECT F ASSIGN TO \"f\""), A("  ORGANIZATION LINE SEQUENTIAL.")}),
               Lines({A("ENVIRONMENT DIVISION."), A("INPUT-OUTPUT SECTION."), A("FILE-CONTROL."),
                      B(0, "SELECT F ASSIGN TO \"f\""), B(0, "  ORGANIZATION LINE SEQUENTIAL.")}));
    // A division header split over two lines.
    EXPECT_FMT(Lines({A("   DATA"), A("    DIVISION."), A("WORKING-STORAGE SECTION."), A("  01 X PIC 9."),
                      A("   PROCEDURE"), A("     DIVISION."), A("DISPLAY X.")}),
               Lines({A("DATA"), A(" DIVISION."), A("WORKING-STORAGE SECTION."), A("01 X PIC 9."), A("PROCEDURE"),
                      B(0, "DIVISION."), B(0, "DISPLAY X.")}),
               [] {
                   cobolfmt::Options o;
                   o.align_pic = false;
                   return o;
               }());
    // A division header in Area B inside the procedure division (no END PROGRAM): left where it is.
    EXPECT_FMT(Lines({A("PROCEDURE DIVISION."), A("STOP RUN."), B(0, "IDENTIFICATION DIVISION."), A("PROGRAM-ID. Y.")}),
               Lines({A("PROCEDURE DIVISION."), B(0, "STOP RUN."), B(0, "IDENTIFICATION DIVISION."), A("PROGRAM-ID. Y.")}));
    // Sections and paragraphs in the procedure division.
    EXPECT_FMT(Lines({A("PROCEDURE DIVISION."), B(0, "MAIN SECTION."), B(0, "P1."), A("MOVE 1 TO X."),
                      B(2, "0100-START."), A("EXIT.")}),
               Lines({A("PROCEDURE DIVISION."), A("MAIN SECTION."), A("P1."), B(0, "MOVE 1 TO X."),
                      A("0100-START."), B(0, "EXIT.")}));
}

void TestIf() {
    EXPECT_FMT(Lines({A("PROCEDURE DIVISION."), A("IF A > 1"), A("DISPLAY \"A\""), A("IF B > 2"),
                      A("DISPLAY \"B\""), A("ELSE"), A("DISPLAY \"C\""), A("END-IF"), A("ELSE"),
                      A("MOVE 0 TO A"), A("END-IF"), A("DISPLAY \"D\".")}),
               Lines({A("PROCEDURE DIVISION."), B(0, "IF A > 1"), B(1, "DISPLAY \"A\""), B(1, "IF B > 2"),
                      B(2, "DISPLAY \"B\""), B(1, "ELSE"), B(2, "DISPLAY \"C\""), B(1, "END-IF"), B(0, "ELSE"),
                      B(1, "MOVE 0 TO A"), B(0, "END-IF"), B(0, "DISPLAY \"D\".")}));
    // A period ends the IF (sentence style).
    EXPECT_FMT(Lines({A("PROCEDURE DIVISION."), A("IF A = 1"), A("MOVE 2 TO B."), A("MOVE 3 TO C.")}),
               Lines({A("PROCEDURE DIVISION."), B(0, "IF A = 1"), B(1, "MOVE 2 TO B."), B(0, "MOVE 3 TO C.")}));
    // ELSE IF chains stay at one depth.
    EXPECT_FMT(Lines({A("PROCEDURE DIVISION."), A("IF A = 1"), A("MOVE 1 TO B"), A("ELSE IF A = 2"),
                      A("MOVE 2 TO B"), A("ELSE"), A("MOVE 3 TO B"), A("END-IF"), A("END-IF.")}),
               Lines({A("PROCEDURE DIVISION."), B(0, "IF A = 1"), B(1, "MOVE 1 TO B"), B(0, "ELSE IF A = 2"),
                      B(1, "MOVE 2 TO B"), B(0, "ELSE"), B(1, "MOVE 3 TO B"), B(0, "END-IF"), B(0, "END-IF.")}));
    // A whole IF on one line opens nothing.
    EXPECT_FMT(Lines({A("PROCEDURE DIVISION."), A("IF A = 1 MOVE 1 TO B END-IF"), A("DISPLAY B.")}),
               Lines({A("PROCEDURE DIVISION."), B(0, "IF A = 1 MOVE 1 TO B END-IF"), B(0, "DISPLAY B.")}));
    // A condition continued on the next line.
    EXPECT_FMT(Lines({A("PROCEDURE DIVISION."), A("IF A = 1"), A("  AND B = 2"), A("DISPLAY A"), A("END-IF.")}),
               Lines({A("PROCEDURE DIVISION."), B(0, "IF A = 1"), B(1, "AND B = 2"), B(1, "DISPLAY A"),
                      B(0, "END-IF.")}));
}

void TestEvaluate() {
    EXPECT_FMT(Lines({A("PROCEDURE DIVISION."), A("EVALUATE TRUE"), A("WHEN A = 1"), A("DISPLAY \"ONE\""),
                      A("WHEN A = 2"), A("WHEN A = 3"), A("DISPLAY \"TWO\""), A("IF B = 1"),
                      A("DISPLAY \"B\""), A("END-IF"), A("WHEN OTHER"), A("CONTINUE"), A("END-EVALUATE"),
                      A("GOBACK.")}),
               Lines({A("PROCEDURE DIVISION."), B(0, "EVALUATE TRUE"), B(1, "WHEN A = 1"), B(2, "DISPLAY \"ONE\""),
                      B(1, "WHEN A = 2"), B(1, "WHEN A = 3"), B(2, "DISPLAY \"TWO\""), B(2, "IF B = 1"),
                      B(3, "DISPLAY \"B\""), B(2, "END-IF"), B(1, "WHEN OTHER"), B(2, "CONTINUE"),
                      B(0, "END-EVALUATE"), B(0, "GOBACK.")}));
    // Nested EVALUATE inside IF.
    EXPECT_FMT(Lines({A("PROCEDURE DIVISION."), A("IF X"), A("EVALUATE Y"), A("WHEN 1 DISPLAY 1"),
                      A("END-EVALUATE"), A("END-IF.")}),
               Lines({A("PROCEDURE DIVISION."), B(0, "IF X"), B(1, "EVALUATE Y"), B(2, "WHEN 1 DISPLAY 1"),
                      B(1, "END-EVALUATE"), B(0, "END-IF.")}));
}

void TestPerform() {
    EXPECT_FMT(Lines({A("PROCEDURE DIVISION."), A("PERFORM VARYING I FROM 1 BY 1 UNTIL I > 10"),
                      A("DISPLAY I"), A("END-PERFORM"), A("PERFORM 5 TIMES"), A("ADD 1 TO N"), A("END-PERFORM"),
                      A("PERFORM UNTIL EOF"), A("PERFORM READ-ONE"), A("END-PERFORM"), A("PERFORM"),
                      A("CONTINUE"), A("END-PERFORM"), A("PERFORM P1 THRU P2"), A("STOP RUN.")}),
               Lines({A("PROCEDURE DIVISION."), B(0, "PERFORM VARYING I FROM 1 BY 1 UNTIL I > 10"),
                      B(1, "DISPLAY I"), B(0, "END-PERFORM"), B(0, "PERFORM 5 TIMES"), B(1, "ADD 1 TO N"),
                      B(0, "END-PERFORM"), B(0, "PERFORM UNTIL EOF"), B(1, "PERFORM READ-ONE"),
                      B(0, "END-PERFORM"), B(0, "PERFORM"), B(1, "CONTINUE"), B(0, "END-PERFORM"),
                      B(0, "PERFORM P1 THRU P2"), B(0, "STOP RUN.")}));
    // PERFORM P UNTIL X: a procedure call (not inline) -- its UNTIL is on the same line, the word after PERFORM is a name.
    EXPECT_FMT(Lines({A("PROCEDURE DIVISION."), A("PERFORM P1 UNTIL X > 1"), A("DISPLAY X.")}),
               Lines({A("PROCEDURE DIVISION."), B(0, "PERFORM P1 UNTIL X > 1"), B(0, "DISPLAY X.")}));
    // EXIT PERFORM opens nothing.
    EXPECT_FMT(Lines({A("PROCEDURE DIVISION."), A("PERFORM FOREVER"), A("IF X"), A("EXIT PERFORM"), A("END-IF"),
                      A("END-PERFORM.")}),
               Lines({A("PROCEDURE DIVISION."), B(0, "PERFORM FOREVER"), B(1, "IF X"), B(2, "EXIT PERFORM"),
                      B(1, "END-IF"), B(0, "END-PERFORM.")}));
}

void TestPhrases() {
    EXPECT_FMT(Lines({A("PROCEDURE DIVISION."), A("READ IN-FILE"), A("AT END"), A("SET EOF TO TRUE"),
                      A("NOT AT END"), A("ADD 1 TO N"), A("END-READ"), A("COMPUTE X = Y / Z"),
                      A("ON SIZE ERROR"), A("MOVE 0 TO X"), A("END-COMPUTE"), A("WRITE R"), A("INVALID KEY"),
                      A("DISPLAY \"BAD\""), A("NOT INVALID KEY"), A("CONTINUE"), A("END-WRITE"),
                      A("CALL \"SUB\" USING X"), A("ON EXCEPTION"), A("DISPLAY \"NO SUB\""), A("END-CALL.")}),
               Lines({A("PROCEDURE DIVISION."), B(0, "READ IN-FILE"), B(1, "AT END"), B(2, "SET EOF TO TRUE"),
                      B(1, "NOT AT END"), B(2, "ADD 1 TO N"), B(0, "END-READ"), B(0, "COMPUTE X = Y / Z"),
                      B(1, "ON SIZE ERROR"), B(2, "MOVE 0 TO X"), B(0, "END-COMPUTE"), B(0, "WRITE R"),
                      B(1, "INVALID KEY"), B(2, "DISPLAY \"BAD\""), B(1, "NOT INVALID KEY"), B(2, "CONTINUE"),
                      B(0, "END-WRITE"), B(0, "CALL \"SUB\" USING X"), B(1, "ON EXCEPTION"),
                      B(2, "DISPLAY \"NO SUB\""), B(0, "END-CALL.")}));
    // Phrase on the statement's own line; then a plain statement after END-READ.
    EXPECT_FMT(Lines({A("PROCEDURE DIVISION."), A("READ F AT END"), A("MOVE 1 TO E"), A("END-READ"),
                      A("DISPLAY E.")}),
               Lines({A("PROCEDURE DIVISION."), B(0, "READ F AT END"), B(2, "MOVE 1 TO E"), B(0, "END-READ"),
                      B(0, "DISPLAY E.")}));
    // A statement with an END- form but no phrase closes when the next verb comes.
    EXPECT_FMT(Lines({A("PROCEDURE DIVISION."), A("IF X"), A("DISPLAY \"A\""), A("DISPLAY \"B\""), A("END-IF.")}),
               Lines({A("PROCEDURE DIVISION."), B(0, "IF X"), B(1, "DISPLAY \"A\""), B(1, "DISPLAY \"B\""),
                      B(0, "END-IF.")}));
    // SEARCH ... AT END ... WHEN.
    EXPECT_FMT(Lines({A("PROCEDURE DIVISION."), A("SEARCH T"), A("AT END"), A("DISPLAY \"NONE\""),
                      A("WHEN T-KEY (IX) = K"), A("DISPLAY \"FOUND\""), A("END-SEARCH.")}),
               Lines({A("PROCEDURE DIVISION."), B(0, "SEARCH T"), B(1, "AT END"), B(2, "DISPLAY \"NONE\""),
                      B(1, "WHEN T-KEY (IX) = K"), B(2, "DISPLAY \"FOUND\""), B(0, "END-SEARCH.")}));
    // MOVE continued over lines, in the procedure division at least one indent in.
    EXPECT_FMT(Lines({A("PROCEDURE DIVISION."), A("MOVE A"), A("TO B.")}),
               Lines({A("PROCEDURE DIVISION."), B(0, "MOVE A"), B(1, "TO B.")}));
    // FUNCTION's name is no verb.
    EXPECT_FMT(Lines({A("PROCEDURE DIVISION."), A("COMPUTE X ="), A("FUNCTION RANDOM"), A("DISPLAY X.")}),
               Lines({A("PROCEDURE DIVISION."), B(0, "COMPUTE X ="), B(1, "FUNCTION RANDOM"), B(0, "DISPLAY X.")}));
}

void TestData() {
    cobolfmt::Options plain;
    plain.align_pic = false;
    EXPECT_FMT(Lines({A("DATA DIVISION."), A("WORKING-STORAGE SECTION."), B(0, "01 WS-REC."), A("05 WS-A PIC X."),
                      A("05 WS-B."), A("10 WS-C PIC 9."), A("88 C-ON VALUE 1."), A("10 WS-D PIC 9."),
                      A("05 WS-E PIC X."), A("77 WS-K PIC 9."), A("78 MAX-N VALUE 10.")}),
               Lines({A("DATA DIVISION."), A("WORKING-STORAGE SECTION."), A("01 WS-REC."), B(0, "05 WS-A PIC X."),
                      B(0, "05 WS-B."), B(1, "10 WS-C PIC 9."), B(2, "88 C-ON VALUE 1."), B(1, "10 WS-D PIC 9."),
                      B(0, "05 WS-E PIC X."), A("77 WS-K PIC 9."), A("78 MAX-N VALUE 10.")}),
               plain);
    // Level numbers need not be in steps: depth follows the hierarchy.
    EXPECT_FMT(Lines({A("DATA DIVISION."), A("WORKING-STORAGE SECTION."), A("01 R."), A("03 A."), A("07 B PIC X."),
                      A("03 C PIC X."), A("01 S PIC X.")}),
               Lines({A("DATA DIVISION."), A("WORKING-STORAGE SECTION."), A("01 R."), B(0, "03 A."),
                      B(1, "07 B PIC X."), B(0, "03 C PIC X."), A("01 S PIC X.")}),
               plain);
    // FD in Area A, its clauses continued; an 88 under an 01.
    EXPECT_FMT(Lines({A("DATA DIVISION."), A("FILE SECTION."), B(0, "FD IN-FILE"), B(0, "    RECORD CONTAINS 80."),
                      B(0, "01 IN-REC PIC X(80)."), A("88 IN-EMPTY VALUE SPACES.")}),
               Lines({A("DATA DIVISION."), A("FILE SECTION."), A("FD IN-FILE"), A("    RECORD CONTAINS 80."),
                      A("01 IN-REC PIC X(80)."), B(0, "88 IN-EMPTY VALUE SPACES.")}),
               plain);
    // An entry continued over lines: the rest keeps its offset.
    EXPECT_FMT(Lines({A("DATA DIVISION."), A("WORKING-STORAGE SECTION."), A("01 R."), A("05 X PIC X(10)"),
                      A("   VALUE \"ABC\".")}),
               Lines({A("DATA DIVISION."), A("WORKING-STORAGE SECTION."), A("01 R."), B(0, "05 X PIC X(10)"),
                      B(0, "   VALUE \"ABC\".")}),
               plain);
    // PICTURE alignment at column 40 (0-based 39).
    EXPECT_FMT(Lines({A("DATA DIVISION."), A("WORKING-STORAGE SECTION."), A("01 R."), A("05 WS-A PIC X."),
                      A("05 WS-LONGER-NAME   PICTURE 9(4) VALUE 0."), A("05 FILLER PIC X(3) VALUE \"A  B\"."),
                      A("01 WS-NO-PIC VALUE 1 PIC 9.")}),
               Lines({A("DATA DIVISION."), A("WORKING-STORAGE SECTION."), A("01 R."), At(B(0, "05 WS-A"), 39, "PIC X."),
                      At(B(0, "05 WS-LONGER-NAME"), 39, "PICTURE 9(4) VALUE 0."),
                      At(B(0, "05 FILLER"), 39, "PIC X(3) VALUE \"A  B\"."), A("01 WS-NO-PIC VALUE 1 PIC 9.")}));
    // A name reaching past column 40: the original gap is kept.
    {
        const std::string name = "05 A-VERY-LONG-DATA-NAME-THAT-GOES-ON-AND-ON";
        EXPECT_FMT(Lines({A("DATA DIVISION."), A("WORKING-STORAGE SECTION."), A("01 R."), A(name + "  PIC X.")}),
                   Lines({A("DATA DIVISION."), A("WORKING-STORAGE SECTION."), A("01 R."), B(0, name + "  PIC X.")}));
    }
    // Alignment that would push past column 72: the gap is kept instead.
    {
        const std::string pic = "PIC X(10) VALUE \"0123456789012345678901\".";
        const std::string src = Lines({A("DATA DIVISION."), A("WORKING-STORAGE SECTION."), A("01 R."), A("05 A " + pic)});
        EXPECT_FMT(src, Lines({A("DATA DIVISION."), A("WORKING-STORAGE SECTION."), A("01 R."), B(0, "05 A " + pic)}));
    }
    // Invalid level number: left alone.
    EXPECT_FMT(Lines({A("DATA DIVISION."), A("WORKING-STORAGE SECTION."), A("  99 X PIC 9.")}),
               Lines({A("DATA DIVISION."), A("WORKING-STORAGE SECTION."), A("  99 X PIC 9.")}), plain);
    // Several entries on one line; the level stack sees them all.
    EXPECT_FMT(Lines({A("DATA DIVISION."), A("WORKING-STORAGE SECTION."), A("01 R. 05 A PIC X."), A("05 B PIC X.")}),
               Lines({A("DATA DIVISION."), A("WORKING-STORAGE SECTION."), A("01 R. 05 A PIC X."), B(0, "05 B PIC X.")}),
               plain);
    // A copybook of fields (no division header): depth by the hierarchy.
    EXPECT_FMT(Lines({"       01 CUST-REC.", "       05 CUST-ID PIC 9(5).", "       05 CUST-NAME PIC X(20)."}),
               Lines({A("01 CUST-REC."), B(0, "05 CUST-ID PIC 9(5)."), B(0, "05 CUST-NAME PIC X(20).")}), plain);
    EXPECT_FMT(Lines({"           05 CUST-ID PIC 9(5).", "         05 CUST-NAME PIC X(20)."}),
               Lines({B(0, "05 CUST-ID PIC 9(5)."), B(0, "05 CUST-NAME PIC X(20).")}), plain);
}

void TestUntouched() {
    // Comment lines, sequence numbers and identification areas stay as they are.
    const std::string seq = "000100";
    EXPECT_FMT(Lines({seq + " PROCEDURE DIVISION.", "000200*  A COMMENT   ", "000300 IF X", "000400 DISPLAY X",
                      At("000500 END-IF.", 72, "PROG0001")}),
               Lines({seq + " PROCEDURE DIVISION.", "000200*  A COMMENT   ", "000300     IF X",
                      "000400         DISPLAY X", At("000500     END-IF.", 72, "PROG0001")}));
    // A line that would pass column 72 is left as it was.
    {
        const std::string longcode = "DISPLAY \"01234567890123456789012345678901234567890123456789012\"";
        const std::string src = Lines({A("PROCEDURE DIVISION."), A("IF Y"), A(longcode), A("END-IF.")});
        EXPECT_FMT(src, Lines({A("PROCEDURE DIVISION."), B(0, "IF Y"), A(longcode), B(0, "END-IF.")}));
    }
    // A line too long to move stays, and so do its continuation lines (relative to it).
    {
        const std::string l1 = A("   05 DM-EXPR-VARIABLE-NAME-LENGTH USAGE BINARY-LONG SIGNED VALUE");
        const std::string l2 = A("       -1.");
        const std::string src = Lines({A("DATA DIVISION."), A("WORKING-STORAGE SECTION."), A("01 R."), l1, l2});
        EXPECT_FMT(src, src);
    }
    // A word running into the identification area: unchanged.
    {
        const std::string l = At(A("    DISPLAY \"X\" ABC"), 70, "DEFGHIJ");
        EXPECT_FMT(Lines({A("PROCEDURE DIVISION."), l}), Lines({A("PROCEDURE DIVISION."), l}));
    }
    // Continued literals: the continued line and the continuation stay put.
    {
        const std::string l1 = At(A("  DISPLAY \"ABCDEFGHIJ"), 72, "");
        const std::string l2 = "      -    \"KLMNOP\".";
        const std::string src = Lines({A("PROCEDURE DIVISION."), A("  MOVE 1 TO X"), l1, l2, A("  DISPLAY Y.")});
        EXPECT_FMT(src, Lines({A("PROCEDURE DIVISION."), B(0, "MOVE 1 TO X"), l1, l2, B(0, "DISPLAY Y.")}));
    }
    // COPY and REPLACE lines stay; the statements around them are indented.
    EXPECT_FMT(Lines({A("DATA DIVISION."), A("WORKING-STORAGE SECTION."), A("    COPY CUSTREC."),
                      A("  REPLACE ==:P:== BY ==WS==."), A("01 :P:-X PIC 9."), A("PROCEDURE DIVISION."),
                      A("IF X"), A("  COPY STMTS."), A("END-IF.")}),
               Lines({A("DATA DIVISION."), A("WORKING-STORAGE SECTION."), A("    COPY CUSTREC."),
                      A("  REPLACE ==:P:== BY ==WS==."), A("01 :P:-X PIC 9."), A("PROCEDURE DIVISION."),
                      B(0, "IF X"), A("  COPY STMTS."), B(0, "END-IF.")}),
               [] {
                   cobolfmt::Options o;
                   o.align_pic = false;
                   return o;
               }());
    // Debugging lines and directives.
    EXPECT_FMT(Lines({A("PROCEDURE DIVISION."), "      D  DISPLAY \"DBG\".", "       >>IF X DEFINED",
                      A("DISPLAY 1."), "       >>END-IF"}),
               Lines({A("PROCEDURE DIVISION."), "      D  DISPLAY \"DBG\".", "       >>IF X DEFINED",
                      B(0, "DISPLAY 1."), "       >>END-IF"}));
    // Comment entries (AUTHOR. ...) are left alone.
    EXPECT_FMT(Lines({A("IDENTIFICATION DIVISION."), A("PROGRAM-ID. X."), A("AUTHOR."), A("    SOMEBODY."),
                      A("      ELSE.")}),
               Lines({A("IDENTIFICATION DIVISION."), A("PROGRAM-ID. X."), A("AUTHOR."), A("    SOMEBODY."),
                      A("      ELSE.")}));
    // EXEC SQL blocks.
    EXPECT_FMT(Lines({A("PROCEDURE DIVISION."), A("EXEC SQL"), A("  SELECT 1"), A("END-EXEC."), A("DISPLAY 1.")}),
               Lines({A("PROCEDURE DIVISION."), A("EXEC SQL"), A("  SELECT 1"), A("END-EXEC."), B(0, "DISPLAY 1.")}));
    // A tab before the code: left alone.
    EXPECT_FMT(Lines({A("PROCEDURE DIVISION."), "\tDISPLAY 1."}), Lines({A("PROCEDURE DIVISION."), "\tDISPLAY 1."}));
    // Case is kept, whitespace inside literals too, `*>` comments ride along.
    EXPECT_FMT(Lines({A("procedure division."), A("if x  *> check"), A("display \"a   b\""), A("end-if.")}),
               Lines({A("procedure division."), B(0, "if x  *> check"), B(1, "display \"a   b\""), B(0, "end-if.")}));
    // Blank lines and trailing whitespace on unchanged lines stay; CRLF and no final newline are kept.
    EXPECT_FMT(A("PROCEDURE DIVISION.") + "\r\n\r\n" + A("DISPLAY 1.") + "\r\n" + A("STOP RUN."),
               A("PROCEDURE DIVISION.") + "\r\n\r\n" + B(0, "DISPLAY 1.") + "\r\n" + B(0, "STOP RUN."));
    EXPECT_FMT("", "");
}

void TestFree() {
    EXPECT_FMT(Lines({">>SOURCE FORMAT FREE", "  IDENTIFICATION DIVISION.", " PROGRAM-ID. F.",
                      "      DATA DIVISION.", "WORKING-STORAGE SECTION.", "  01 R.", "05 A PIC X.",
                      "*> a comment", "  PROCEDURE DIVISION.", " MAIN.", "IF A = \"Y\"", "DISPLAY \"YES\"",
                      "ELSE", "DISPLAY \"NO\" *> trailing", "END-IF", "STOP RUN."}),
               Lines({">>SOURCE FORMAT FREE", "IDENTIFICATION DIVISION.", "PROGRAM-ID. F.", "DATA DIVISION.",
                      "WORKING-STORAGE SECTION.", "01 R.", At(F(1, "05 A"), 32, "PIC X."), "*> a comment",
                      "PROCEDURE DIVISION.", "MAIN.", F(1, "IF A = \"Y\""), F(2, "DISPLAY \"YES\""), F(1, "ELSE"),
                      F(2, "DISPLAY \"NO\" *> trailing"), F(1, "END-IF"), F(1, "STOP RUN.")}));
    // Indent width.
    cobolfmt::Options two;
    two.indent = 2;
    two.align_pic = false;
    EXPECT_FMT(Lines({">>SOURCE FREE", "PROCEDURE DIVISION.", "IF X", "DISPLAY X", "END-IF."}),
               Lines({">>SOURCE FREE", "PROCEDURE DIVISION.", "  IF X", "    DISPLAY X", "  END-IF."}), two);
    // Free format without a directive (guessed from code in the sequence area).
    EXPECT_FMT(Lines({"IDENTIFICATION DIVISION.", "PROGRAM-ID. G.", "PROCEDURE DIVISION.", "DISPLAY \"G\".",
                      "GOBACK."}),
               Lines({"IDENTIFICATION DIVISION.", "PROGRAM-ID. G.", "PROCEDURE DIVISION.", F(1, "DISPLAY \"G\"."),
                      F(1, "GOBACK.")}));
    // Switching format mid-file.
    EXPECT_FMT(Lines({A("PROCEDURE DIVISION."), A("DISPLAY 1"), "       >>SOURCE FORMAT FREE", "DISPLAY 2",
                      ">>SOURCE FORMAT FIXED", A("STOP RUN.")}),
               Lines({A("PROCEDURE DIVISION."), B(0, "DISPLAY 1"), "       >>SOURCE FORMAT FREE", F(1, "DISPLAY 2"),
                      ">>SOURCE FORMAT FIXED", B(0, "STOP RUN.")}));
    // Free-format continued literal (`-` continuation): kept.
    {
        const std::string src = Lines({">>SOURCE FREE", "PROCEDURE DIVISION.", "DISPLAY \"ABC", "-\"DEF\".",
                                       "STOP RUN."});
        EXPECT_FMT(src, Lines({">>SOURCE FREE", "PROCEDURE DIVISION.", "DISPLAY \"ABC", "-\"DEF\".",
                               F(1, "STOP RUN.")}));
    }
}

void TestRefusals() {
    // Guessed free but with fixed sequence numbers: unclear, so refused.
    const std::string src = Lines({"000100 PROCEDURE DIVISION.", "DISPLAY-IT. DISPLAY 1."});
    const cobolfmt::Result r = cobolfmt::Format(src);
    if (r.ok) {
        // Not refused only when the format is decided as fixed; then nothing may move into the sequence area.
        CHECK(r.text.compare(0, 6, "000100") == 0);
    } else {
        CHECK(r.text == src);
        CHECK(r.error_line >= 1);
    }
    // Garbage in, no crash, idempotent.
    for (const char *junk : {"\"", "==", "  END-IF END-IF END-PERFORM.", "IF", "      -", "01", "PROCEDURE DIVISION.\nELSE\nWHEN\n.",
                             "       01 X PIC X VALUE \"", "\t\t\n\r\n", "*>\n>>"}) {
        const cobolfmt::Result a = cobolfmt::Format(junk);
        if (a.ok) {
            const cobolfmt::Result b = cobolfmt::Format(a.text);
            CHECK(b.ok && b.text == a.text);
        } else {
            CHECK(a.text == junk);
        }
    }
}

// The datamunge-style program from the docs: a full round trip through every construct.
void TestProgram() {
    const std::string src = Lines({
        "      *> Example", "       IDENTIFICATION DIVISION.", "       PROGRAM-ID. KMDEMO.",
        "       DATA DIVISION.", "       WORKING-STORAGE SECTION.", "       01 WS-K PIC 9(4) VALUE 0.",
        "       01 WS-REC.", "         05 WS-A PIC X(10).", "           88 WS-A-EMPTY VALUE SPACES.",
        "       PROCEDURE DIVISION.", "       MAIN-PARA.", "           PERFORM VARYING WS-K FROM 1 BY 1",
        "               UNTIL WS-K > 3", "             IF WS-K = 2", "           DISPLAY \"TWO\"",
        "             END-IF", "           END-PERFORM", "           STOP RUN.", "       CLEANUP.",
        "           EXIT.", "       END PROGRAM KMDEMO."});
    const std::string want = Lines({
        "      *> Example", A("IDENTIFICATION DIVISION."), A("PROGRAM-ID. KMDEMO."), A("DATA DIVISION."),
        A("WORKING-STORAGE SECTION."), At(A("01 WS-K"), 39, "PIC 9(4) VALUE 0."), A("01 WS-REC."),
        At(B(0, "05 WS-A"), 39, "PIC X(10)."), B(1, "88 WS-A-EMPTY VALUE SPACES."), A("PROCEDURE DIVISION."),
        A("MAIN-PARA."), B(0, "PERFORM VARYING WS-K FROM 1 BY 1"), B(0, "    UNTIL WS-K > 3"), B(1, "IF WS-K = 2"),
        B(2, "DISPLAY \"TWO\""), B(1, "END-IF"), B(0, "END-PERFORM"), B(0, "STOP RUN."), A("CLEANUP."),
        B(0, "EXIT."), A("END PROGRAM KMDEMO.")});
    EXPECT_FMT(src, want);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc > 1 && std::strcmp(argv[1], "--format") == 0) return FormatFiles(argc, argv);
    TestDivisions();
    TestIf();
    TestEvaluate();
    TestPerform();
    TestPhrases();
    TestData();
    TestUntouched();
    TestFree();
    TestRefusals();
    TestProgram();
    std::printf("mep-cobol-format-test: %d checks passed\n", g_checks);
    return 0;
}
