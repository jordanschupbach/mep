#ifndef MEP_PROJECT_SEARCH_H
#define MEP_PROJECT_SEARCH_H

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// A builtin stand-in for ripgrep: project-wide file listing and grep,
// in-process, for machines with no `rg` on PATH. Every project search mep
// ships (live grep, find files, the todo scan, the winbar's directory
// picker) is written against `rg`'s argv and output format, and without
// the binary each one used to fail outright -- live grep, with no
// fallback at all, simply never showed a result. mep.job_start routes an
// `rg` argv it can parse here instead (lua_env.cpp's l_job_start), so
// callers keep one code path and get the real rg whenever it is installed.
//
// What it reproduces is the subset those callers (and a typical user
// config) depend on, with rg's own defaults:
//   - the walk skips hidden entries (unless --hidden), never follows
//     directory symlinks, always skips .git, and honors .gitignore (inside
//     a git repository, from the repo root down), .git/info/exclude, the
//     global ~/.config/git/ignore, and .ignore/.rgignore, with gitignore's
//     glob rules (anchoring, trailing-slash dirs, **, negation);
//   - binary files (a NUL in the first 64KB) are skipped by grep;
//   - output is rg's --no-heading shape: `path`, `path:text` or
//     `path:line:text`, with paths spelled relative to how they were given
//     (`.` gives `./src/x`, no path gives `src/x`).
// The pattern is matched with mep's own regex engine (regex.h), which
// covers the ECMAScript/PCRE-ish syntax people actually type into a grep
// prompt; a pattern with no metacharacters takes a plain substring path.
namespace mep_search {

struct Options {
    std::string pattern;
    bool files_only = false;     // --files
    bool hidden = false;         // --hidden / -.
    bool no_ignore = false;      // --no-ignore
    bool ignore_case = false;    // -i
    bool smart_case = false;     // -S
    bool fixed_strings = false;  // -F
    bool line_number = false;    // -n
    bool with_filename = true;   // -H/-I; rg's own default, except for a lone file argument
    bool with_filename_set = false;
    // --glob values: "!x" excludes, "x" whitelists files. A glob with no
    // '/' matches an entry's name at any depth, like rg's.
    std::vector<std::string> globs;
    std::vector<std::string> paths;  // empty: the cwd, printed without a prefix
};

/**
 * @brief Parses a ripgrep argv (argv[0] == "rg") into Options.
 * @param argv The full argv, including "rg" itself.
 * @return The options, or nullopt for any flag outside the supported subset
 * (the caller then spawns the real binary, unchanged).
 */
std::optional<Options> ParseRgArgs(const std::vector<std::string> &argv);

/**
 * @brief Runs a search, emitting one rg-shaped output line at a time.
 * @param opts What to search for and where.
 * @param cwd Directory relative paths (and the default path) resolve against.
 * @param emit Receives each output line.
 * @param cancelled Polled throughout; the search returns early once it is true.
 * @return rg's exit status: 0 something matched, 1 nothing did, 2 error (e.g. a bad pattern).
 */
int Run(const Options &opts, const std::string &cwd, const std::function<void(const std::string &)> &emit,
        const std::atomic<bool> &cancelled);

/**
 * @brief Literals at least one of which every match of `pattern` must
 * contain (one per top-level alternative) -- the substring prefilter Run
 * puts in front of the regex.
 * @param pattern A regex.
 * @return The literals, or empty when the pattern guarantees none.
 */
std::vector<std::string> PrefilterLiterals(const std::string &pattern);

/**
 * @brief Matches a gitignore-style glob against a '/'-separated path
 * (`*` and `?` stop at '/', `**` crosses it, `[...]` classes, `\` escapes).
 * @param glob The glob.
 * @param path The path to test.
 * @return True if the whole path matches.
 */
bool GlobMatch(const std::string &glob, const std::string &path);

}  // namespace mep_search

#endif  // MEP_PROJECT_SEARCH_H
