// Coverage for test_runner.h/.cpp: the ctest parsing behind the Tests
// sidebar. Every fixture below is verbatim ctest 4.x output (paths
// shortened) from a scratch project with one passing, one failing, one
// aborting and one never-built test, so a change in what ctest prints shows
// up here rather than as a silently empty sidebar.

#include "test_runner.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                        \
        }                                                                        \
    } while (0)

void ExpectEq(const std::string &got, const std::string &want, const char *what) {
    if (got != want) {
        std::fprintf(stderr, "%s: got \"%s\", want \"%s\"\n", what, got.c_str(), want.c_str());
        g_failures++;
    }
}

const char *kListJson = R"({
  "backtraceGraph" : { "commands" : [ "add_test" ], "files" : [ "/p/CMakeLists.txt" ], "nodes" : [ { "file" : 0 } ] },
  "kind" : "ctestInfo",
  "tests" :
  [
    {
      "backtrace" : 1,
      "command" : [ "/p/b/ok" ],
      "name" : "ok",
      "properties" :
      [
        { "name" : "LABELS", "value" : [ "fast", "unit" ] },
        { "name" : "WORKING_DIRECTORY", "value" : "/p/b" }
      ]
    },
    {
      "backtrace" : 3,
      "command" : [ "/p/b/bad", "--flag", "x y" ],
      "name" : "bad",
      "properties" : [ { "name" : "WORKING_DIRECTORY", "value" : "/p/b" } ]
    },
    {
      "backtrace" : 7,
      "name" : "unbuilt",
      "properties" : [ { "name" : "WORKING_DIRECTORY", "value" : "/p/b" } ]
    },
    {
      "backtrace" : 9,
      "command" : [ "/p/b/ok" ],
      "name" : "with space",
      "properties" : [ { "name" : "WORKING_DIRECTORY", "value" : "/p/b" } ]
    }
  ],
  "version" : { "major" : 1, "minor" : 0 }
})";

void TestParseList() {
    std::vector<meptest::CtestTest> tests;
    CHECK(meptest::ParseCtestList(kListJson, &tests));
    CHECK(tests.size() == 4);
    if (tests.size() != 4) return;
    ExpectEq(tests[0].name, "ok", "tests[0].name");
    CHECK(tests[0].command.size() == 1);
    ExpectEq(tests[0].working_dir, "/p/b", "tests[0].working_dir");
    CHECK(tests[0].labels.size() == 2);
    CHECK(tests[1].command.size() == 3);
    if (tests[1].command.size() == 3) ExpectEq(tests[1].command[2], "x y", "tests[1].command[2]");
    // An unbuilt EXCLUDE_FROM_ALL target: ctest omits "command" altogether.
    ExpectEq(tests[2].name, "unbuilt", "tests[2].name");
    CHECK(tests[2].command.empty());
    ExpectEq(tests[3].name, "with space", "tests[3].name");
}

void TestParseListRejectsNonCtest() {
    std::vector<meptest::CtestTest> tests;
    CHECK(!meptest::ParseCtestList("", &tests));
    // What ctest prints for a directory that isn't a build tree.
    CHECK(!meptest::ParseCtestList("Test project /tmp\nNo tests were found!!!\n", &tests));
    CHECK(!meptest::ParseCtestList(R"({"kind":"somethingElse","tests":[{"name":"x"}]})", &tests));
    CHECK(tests.empty());
    // A configured tree with no tests is valid, just empty.
    CHECK(meptest::ParseCtestList(R"({"kind":"ctestInfo","tests":[]})", &tests));
    CHECK(tests.empty());
}

void ExpectResult(const std::string &line, const std::string &name, const std::string &status,
                  const std::string &detail, double seconds) {
    meptest::CtestLine l = meptest::ParseCtestLine(line);
    if (l.kind != meptest::CtestLine::Kind::Result) {
        std::fprintf(stderr, "ParseCtestLine(\"%s\") is not a Result\n", line.c_str());
        g_failures++;
        return;
    }
    ExpectEq(l.result.name, name, ("name of: " + line).c_str());
    ExpectEq(l.result.status, status, ("status of: " + line).c_str());
    ExpectEq(l.result.detail, detail, ("detail of: " + line).c_str());
    CHECK(std::fabs(l.result.seconds - seconds) < 1e-9);
}

void TestParseLine() {
    ExpectResult("1/5 Test #1: ok ...............................   Passed    0.35 sec", "ok", "passed", "Passed", 0.35);
    ExpectResult("2/5 Test #2: bad ..............................***Failed    0.20 sec", "bad", "failed", "Failed", 0.20);
    ExpectResult("3/5 Test #3: seg ..............................Subprocess aborted***Exception:   0.20 sec", "seg",
                 "failed", "Subprocess aborted***Exception:", 0.20);
    ExpectResult("4/5 Test #4: unbuilt ..........................***Not Run   0.00 sec", "unbuilt", "notrun",
                 "Not Run", 0.0);
    ExpectResult("5/5 Test #5: with space .......................   Passed    0.00 sec", "with space", "passed",
                 "Passed", 0.0);
    ExpectResult(" 10/120 Test  #10: mep-pdf-xref-test ........***Timeout  1500.03 sec", "mep-pdf-xref-test", "timeout",
                 "Timeout", 1500.03);
    ExpectResult("7/9 Test #7: maybe ............................***Skipped   0.01 sec", "maybe", "skipped", "Skipped",
                 0.01);
    // A name long enough that ctest has room for a single leader dot.
    ExpectResult("1/1 Test #1: a-very-long-test-name-that-fills-the-column .   Passed    0.01 sec",
                 "a-very-long-test-name-that-fills-the-column", "passed", "Passed", 0.01);

    meptest::CtestLine start = meptest::ParseCtestLine("    Start 3: seg");
    CHECK(start.kind == meptest::CtestLine::Kind::Start);
    ExpectEq(start.result.name, "seg", "start name");
    start = meptest::ParseCtestLine("      Start  12: with space");
    CHECK(start.kind == meptest::CtestLine::Kind::Start);
    ExpectEq(start.result.name, "with space", "start name with space");

    for (const char *other : {"", "Test project /p/b", "boom fail", "40% tests passed, 3 tests failed out of 5",
                              "\t  2 - bad (Failed)", "Total Test time (real) =   0.76 sec"}) {
        if (meptest::ParseCtestLine(other).kind != meptest::CtestLine::Kind::Other) {
            std::fprintf(stderr, "ParseCtestLine(\"%s\") should be Other\n", other);
            g_failures++;
        }
    }
}

void TestParseOutput() {
    const std::vector<std::string> lines = {
        "Test project /p/b",
        "    Start 1: ok",
        "1/4 Test #1: ok ...............................   Passed    0.35 sec",
        "    Start 2: bad",
        "2/4 Test #2: bad ..............................***Failed    0.20 sec",
        "boom fail",
        "second line",
        "",
        "    Start 3: unbuilt",
        "Could not find executable /p/b/unbuilt",
        "Looked in the following places:",
        "/p/b/unbuilt",
        "Unable to find executable: /p/b/unbuilt",
        "3/4 Test #3: unbuilt ..........................***Not Run   0.00 sec",
        "    Start 4: seg",
        "4/4 Test #4: seg ..............................Subprocess aborted***Exception:   0.20 sec",
        "",
        "25% tests passed, 3 tests failed out of 4",
        "",
        "The following tests FAILED:",
        "\t  2 - bad (Failed)",
        "Errors while running CTest",
    };
    std::vector<meptest::CtestResult> r = meptest::ParseCtestOutput(lines);
    CHECK(r.size() == 4);
    if (r.size() != 4) return;
    ExpectEq(r[0].status, "passed", "r[0].status");
    CHECK(r[0].output.empty());
    ExpectEq(r[1].status, "failed", "r[1].status");
    CHECK(r[1].output.size() == 2);
    if (r[1].output.size() == 2) {
        ExpectEq(r[1].output[0], "boom fail", "r[1].output[0]");
        ExpectEq(r[1].output[1], "second line", "r[1].output[1]");
    }
    ExpectEq(r[2].status, "notrun", "r[2].status");
    CHECK(r[2].output.size() == 4);
    if (!r[2].output.empty()) ExpectEq(r[2].output[0], "Could not find executable /p/b/unbuilt", "r[2].output[0]");
    ExpectEq(r[3].name, "seg", "r[3].name");
    // Nothing from the closing summary leaks into the last test's output.
    CHECK(r[3].output.empty());
}

void TestExactNameRegex() {
    ExpectEq(meptest::CtestExactNameRegex("mep-job-test"), "^mep-job-test$", "plain");
    ExpectEq(meptest::CtestExactNameRegex("a.b+c(d)"), "^a\\.b\\+c\\(d\\)$", "metachars");
    ExpectEq(meptest::CtestExactNameRegex("with space"), "^with space$", "space");
}

void TestBuildTarget() {
    meptest::CtestTest t;
    t.name = "mep-job-test";
    ExpectEq(meptest::CtestBuildTarget(t), "mep-job-test", "no command -> name");
    t.command = {"/p/build/native/mep-job-test"};
    ExpectEq(meptest::CtestBuildTarget(t), "mep-job-test", "basename");
    t.name = "alias";
    t.command = {"C:\\p\\build\\tool.exe", "--x"};
    ExpectEq(meptest::CtestBuildTarget(t), "tool", "windows exe");
}

// The driver protocol (assets/test_drivers). Fixtures are what the drivers
// printed against scratch projects: pytest 9 (with a module that fails to
// import), Jest 30 running two files at once, testthat 3.
void TestProtocolLine() {
    using K = meptest::ProtocolLine::Kind;
    meptest::ProtocolLine p = meptest::ParseProtocolLine("@@mep-test runner pytest");
    CHECK(p.kind == K::Runner);
    ExpectEq(p.runner, "pytest", "runner");

    p = meptest::ParseProtocolLine("@@mep-test case tests/test_math.py::TestThing::test_param[1]");
    CHECK(p.kind == K::Case);
    ExpectEq(p.result.name, "tests/test_math.py::TestThing::test_param[1]", "case name");

    p = meptest::ParseProtocolLine("@@mep-test start test-add.R");
    CHECK(p.kind == K::Start);
    ExpectEq(p.result.name, "test-add.R", "start name");

    p = meptest::ParseProtocolLine("@@mep-test result failed 0.109 __tests__/bad.js");
    CHECK(p.kind == K::Result);
    ExpectEq(p.result.name, "__tests__/bad.js", "result name");
    ExpectEq(p.result.status, "failed", "result status");
    ExpectEq(p.result.detail, "Failed", "result detail");
    CHECK(std::fabs(p.result.seconds - 0.109) < 1e-9);

    // A name with spaces runs to the end of the line; a CR is dropped.
    p = meptest::ParseProtocolLine("@@mep-test result skipped 0 my dir/test one.R\r");
    CHECK(p.kind == K::Result);
    ExpectEq(p.result.name, "my dir/test one.R", "name with spaces");
    ExpectEq(p.result.status, "skipped", "skipped");

    // An unknown status is a failure rather than a silent pass.
    p = meptest::ParseProtocolLine("@@mep-test result error 1.5 x");
    ExpectEq(p.result.status, "failed", "unknown status");

    // After a runner's unterminated progress characters.
    p = meptest::ParseProtocolLine("..F@@mep-test result passed 0.001 a");
    CHECK(p.kind == K::Result);
    ExpectEq(p.result.name, "a", "after progress dots");

    CHECK(meptest::ParseProtocolLine("@@mep-test result passed x name").kind == K::Other);
    CHECK(meptest::ParseProtocolLine("@@mep-test result passed 0.1").kind == K::Other);
    CHECK(meptest::ParseProtocolLine("@@mep-test start").kind == K::Other);
    CHECK(meptest::ParseProtocolLine("@@mep-test bogus x").kind == K::Other);
    CHECK(meptest::ParseProtocolLine("plain output").kind == K::Other);
}

void TestProtocolCases() {
    const std::vector<std::string> lines = {
        "",
        "@@mep-test runner pytest",
        "@@mep-test case tests/test_broken.py",
        "@@mep-test case tests/test_math.py::test_add",
        "tests/test_math.py::test_add",
        "@@mep-test case tests/test_math.py::test_add",
        "@@mep-test case tests/test_math.py::test_fail",
    };
    std::vector<std::string> cases = meptest::ParseProtocolCases(lines);
    CHECK(cases.size() == 3);
    if (cases.size() == 3) {
        ExpectEq(cases[0], "tests/test_broken.py", "case 0");
        ExpectEq(cases[1], "tests/test_math.py::test_add", "case 1 (once)");
        ExpectEq(cases[2], "tests/test_math.py::test_fail", "case 2");
    }
}

void TestProtocolOutput() {
    // Jest: both files start before either ends; bad.js's message comes
    // after sum.test.js's result and so is bad.js's alone.
    const std::vector<std::string> jest = {
        "",
        "@@mep-test start __tests__/bad.js",
        "",
        "@@mep-test start sum.test.js",
        "",
        "@@mep-test result passed 0.102 sum.test.js",
        "  \xe2\x97\x8f fails",
        "",
        "    expect(received).toBe(expected) // Object.is equality",
        "",
        "@@mep-test result failed 0.109 __tests__/bad.js",
    };
    std::vector<meptest::CtestResult> r = meptest::ParseProtocolOutput(jest);
    CHECK(r.size() == 2);
    if (r.size() == 2) {
        ExpectEq(r[0].name, "sum.test.js", "jest first");
        CHECK(r[0].output.empty());
        ExpectEq(r[1].name, "__tests__/bad.js", "jest second");
        ExpectEq(r[1].status, "failed", "jest second status");
        CHECK(r[1].output.size() == 3);
        if (r[1].output.size() == 3) {
            ExpectEq(r[1].output[0], "  \xe2\x97\x8f fails", "output first line kept");
            ExpectEq(r[1].output[2], "    expect(received).toBe(expected) // Object.is equality", "output last line");
        }
    }

    // testthat: one file at a time, its summary between start and result.
    const std::vector<std::string> r_out = {
        "@@mep-test start test-err.R",
        "err: 1",
        "\xe2\x95\x90\xe2\x95\x90 Failed \xe2\x95\x90\xe2\x95\x90",
        "Error in `eval(code, test_env)`: boom",
        "@@mep-test result failed 0.012 test-err.R",
        "@@mep-test start test-ok.R",
        "ok: .",
        "@@mep-test result passed 0.006 test-ok.R",
        "trailing noise",
    };
    r = meptest::ParseProtocolOutput(r_out);
    CHECK(r.size() == 2);
    if (r.size() == 2) {
        CHECK(r[0].output.size() == 3);
        ExpectEq(r[1].name, "test-ok.R", "testthat second");
        CHECK(r[1].output.size() == 1);
    }
}

}  // namespace

int main() {
    TestParseList();
    TestParseListRejectsNonCtest();
    TestParseLine();
    TestParseOutput();
    TestExactNameRegex();
    TestBuildTarget();
    TestProtocolLine();
    TestProtocolCases();
    TestProtocolOutput();
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("mep-test-runner-test: all passed\n");
    return 0;
}
