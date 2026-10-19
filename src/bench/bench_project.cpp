// mep-bench: whole-project operations -- the in-process ripgrep
// (project_search.cpp, behind :Rg and the search panel) and the in-house
// git (status for the file tree and gutter, log for the git panel), both
// run against this checkout itself.

#include "bench_util.h"
#include "bench/fixtures.h"

#include "git/git.h"
#include "project_search.h"

#include <atomic>
#include <string>
#include <vector>

using mep::bench::DoNotOptimize;
using mep::bench::Measure;

namespace {

int Search(const std::string &pattern, bool fixed, bool files_only = false) {
    mep_search::Options o;
    o.pattern = pattern;
    o.fixed_strings = fixed;
    o.files_only = files_only;
    o.line_number = true;
    o.paths = {"src", "tools", "docs", "plans"};
    std::atomic<bool> cancelled{false};
    int lines = 0;
    mep_search::Run(o, MEP_SOURCE_DIR, [&](const std::string &) { lines++; }, cancelled);
    return lines;
}

}  // namespace

MEP_BENCH("project/search") {
    Measure("literal_common", 1, [] { DoNotOptimize(Search("PushUndo", true)); });
    Measure("literal_rare", 1, [] { DoNotOptimize(Search("ShiftDecorationsForLineEdit", true)); });
    Measure("regex_word", 1, [] { DoNotOptimize(Search("\\bkMaxUndo\\b", false)); });
    Measure("regex_no_literal", 1, [] { DoNotOptimize(Search("[A-Z]{3}_[A-Z]{3}[0-9]", false)); });
    Measure("files_list", 1, [] { DoNotOptimize(Search("", false, true)); });
}

MEP_BENCH("project/git") {
    std::string err;
    Measure("discover", 1, [] {
        std::string e;
        DoNotOptimize(mep::git::Repo::Discover(MEP_SOURCE_DIR, &e).get());
    });
    auto repo = mep::git::Repo::Discover(MEP_SOURCE_DIR, &err);
    if (!repo) {
        std::fprintf(stderr, "project/git: no repository (%s), skipped\n", err.c_str());
        return;
    }
    Measure("status", 1, [&] {
        mep::git::StatusOptions opts;
        opts.ahead_behind = false;
        mep::git::StatusResult out;
        std::string e;
        mep::git::Status(*repo, opts, out, &e);
    });
    Measure("status_fresh_repo", 1, [] {
        std::string e;
        auto r = mep::git::Repo::Discover(MEP_SOURCE_DIR, &e);
        mep::git::StatusOptions opts;
        mep::git::StatusResult out;
        mep::git::Status(*r, opts, out, &e);
    });
    Measure("branch_info", 1, [&] {
        mep::git::BranchInfo info;
        DoNotOptimize(mep::git::GetBranchInfo(*repo, info, true));
    });
}
