#pragma once

// The parsing side of the Tests sidebar (kBuiltinActivityBar's
// mep.activity_test_panel, main.cpp): what tests a CMake build directory
// registers and what happened when ctest ran them, plus the one line
// protocol every other test provider's driver speaks (assets/test_drivers).
//
//   ParseCtestList   -- `ctest --show-only=json-v1` -> one CtestTest per test.
//   ParseCtestLine   -- one line of a live `ctest` run -> "test N started" /
//                       "test N finished with status S", for updating the
//                       sidebar's per-test marks while the run is going.
//   ParseCtestOutput -- the whole run's lines -> every test's result, with
//                       the lines ctest printed under it (its own output,
//                       under --output-on-failure) attached.
//
// Pure string functions with no editor, buffer or Lua dependency (same ethos
// as indent.h), so they're unit-tested on their own in test_runner_test.cpp
// against verbatim ctest and driver output.

#include <string>
#include <vector>

namespace meptest {

struct CtestTest {
    std::string name;
    // The test's argv. ctest leaves it out entirely when the executable
    // doesn't exist yet (an EXCLUDE_FROM_ALL target nobody has built), so
    // this is empty for those.
    std::vector<std::string> command;
    std::string working_dir;  // WORKING_DIRECTORY property, "" if unset
    std::vector<std::string> labels;
};

// Parses `ctest --show-only=json-v1` output. Returns false (and leaves *out
// empty) when `json` isn't a ctestInfo document -- e.g. ctest printed an error
// instead because the directory isn't a configured build tree.
bool ParseCtestList(const std::string &json, std::vector<CtestTest> *out);

// "passed", "failed", "notrun" (ctest's own "Not Run": typically a missing
// executable), "skipped" or "timeout".
struct CtestResult {
    std::string name;
    std::string status;
    std::string detail;  // ctest's status text verbatim, e.g. "Subprocess aborted***Exception:"
    double seconds = 0.0;
    std::vector<std::string> output;  // lines ctest printed under this test's result line
};

struct CtestLine {
    enum class Kind { Other, Start, Result };
    Kind kind = Kind::Other;
    CtestResult result;  // name always set for Start/Result; the rest only for Result
};

// Classifies one line of ctest's console output: "    Start 3: foo" is a
// Start, "3/9 Test #3: foo ......   Passed    0.01 sec" is a Result.
CtestLine ParseCtestLine(const std::string &line);

// Every Result line in a run, in order, each with the lines that followed it
// up to the next Start/Result line or ctest's closing summary.
std::vector<CtestResult> ParseCtestOutput(const std::vector<std::string> &lines);

// An anchored ctest -R regex matching exactly `name` (regex metacharacters
// escaped), so running "foo" doesn't also run "foo-bar".
std::string CtestExactNameRegex(const std::string &name);

// The CMake target to build before running `test`: the basename of its
// executable when ctest knows it, else the test's own name (what an unbuilt
// target's test is conventionally called -- ctest can't say otherwise, see
// CtestTest::command).
std::string CtestBuildTarget(const CtestTest &test);

// --- mep's test driver protocol --------------------------------------------
//
// The pytest/unittest, testthat, Jest, Vitest and node:test drivers in
// assets/test_drivers all report through these lines, so one parser serves
// every provider but ctest:
//
//   @@mep-test runner NAME                  which runner the driver picked
//   @@mep-test case NAME                    a test exists (listing)
//   @@mep-test start NAME                   it began
//   @@mep-test result STATUS SECONDS NAME   it ended: passed/failed/skipped
//
// A marker may follow other text on its line (a runner's unterminated
// progress dots); that text is ignored. NAME runs to the end of the line
// and may contain spaces.

struct ProtocolLine {
    enum class Kind { Other, Runner, Case, Start, Result };
    Kind kind = Kind::Other;
    // name for Case/Start/Result; status and seconds only for Result;
    // detail is the status as a word ("Failed"), for the sidebar.
    CtestResult result;
    std::string runner;  // for Runner
};

ProtocolLine ParseProtocolLine(const std::string &line);

// Every Case name in a listing, in order, each once.
std::vector<std::string> ParseProtocolCases(const std::vector<std::string> &lines);

// Every Result in a run, in order. A result's output is the lines between
// the marker before it (of any test) and itself: drivers print what a test
// produced right before its result line, which keeps that right even when a
// runner works on several files at once and their start lines interleave.
std::vector<CtestResult> ParseProtocolOutput(const std::vector<std::string> &lines);

}  // namespace meptest
