#pragma once

// Internal: revision walking, pretty formats, decorations and the
// `--graph` renderer (a port of git's graph.c state machine).

#include <cstdint>
#include <string>
#include <vector>

#include "git/git.h"

namespace mep::git {

struct LogOptions {
    std::vector<std::string> revs;  // start points; empty = HEAD
    bool all = false;
    long max_count = -1;
    bool graph = false;
    bool color = false;
    bool topo = false;
    bool first_parent = false;
    enum class Format { User, Oneline, Medium, Short, Full } format = Format::Medium;
    std::string user_format;  // for Format::User
    bool tformat = true;      // terminator (vs separator) semantics
    enum class Date { Default, Short, Relative, Iso, IsoStrict, Unix, Raw } date = Date::Default;
    int decorate = -1;  // -1 auto (off: not a tty), 0 no, 1 short
    std::string reflog_ref;  // non-empty: walk this ref's reflog (-g)
    bool reflog_walk = false;
    bool patch = false, stat = false;
    bool no_walk = false;  // git show
    bool abbrev_commit = false;
    bool combined_merges = false;  // git show: merges get a --cc diff (log -p shows none)
};

// Runs `git log` / `git show` style output into `out`. Returns 0, or 128
// with `err` set (bad revision etc.).
int RunLog(Repo &repo, const LogOptions &opts, std::string &out, std::string &err);

// Formats one commit with a user format string (shared by for-each-ref's
// %(subject) etc. where convenient).
std::string RelativeDate(int64_t t);

}  // namespace mep::git
