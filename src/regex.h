#ifndef MEP_REGEX_H
#define MEP_REGEX_H

#include <memory>
#include <string>
#include <utility>
#include <vector>

// A small, dependency-free regex engine (NVIM_PARITY_PLAN.md's own
// "explicit stretch, deliberately deferred everywhere" item -- both
// parity docs kept search/substitute/header-arg matching on plain
// substrings specifically to avoid this). Implements a standard-ish
// (ECMAScript/PCRE-flavored, not Vim's own idiosyncratic magic/\v/\zs
// system) subset directly over UTF-8 byte strings:
//
//   literals, .  (any one UTF-8 codepoint, not byte)
//   * + ? {n} {n,} {n,m}         (greedy; append ? for non-greedy: *? +? ??)
//   [...] [^...] with a-z ranges and \d \w \s \D \W \S shorthands inside
//   \d \w \s \D \W \S \b \B      outside classes too
//   ^ $                          anchor to the start/end of the *whole
//                                subject string* passed to Search/Match --
//                                callers that want per-line ^/$ (mep's own
//                                usual case, one line at a time) already
//                                get that for free by construction.
//   ( ... )  (?: ... )           capturing / non-capturing groups
//   |                            alternation
//   \. \* \( \\ ...              backslash-escape any special char literal
//
// Deliberately NOT implemented: backreferences (\1 inside the pattern
// itself, as opposed to a replacement string), lookaround ((?=...) etc),
// Unicode property classes (\p{...}), POSIX classes ([:alpha:]). None of
// mep's own planned consumers (search, :s, org/markdown/babel header-arg
// matching) need them; add only if a real one turns out to.
//
// Matching is a plain recursive-descent backtracker (continuation-passing:
// Matcher::Match(node, pos, continuation)), not a Thompson-NFA/Pike VM --
// simpler to implement correctly, at the cost of pathological patterns
// (deeply nested ambiguous quantifiers) being able to blow up
// exponentially, same well-known trade-off PCRE/Perl/Vim's own regex
// engines all made too. Not a concern in practice for the short,
// hand-typed patterns an editor's search/substitute actually sees.
namespace mep_regex {

struct Match {
    int start = -1;  // byte offset into the subject, -1 if no match
    int end = -1;    // byte offset, exclusive
    // groups[0] is always the whole match (== {start, end}); groups[i]
    // for i>=1 is capture group i, {-1,-1} if that group didn't
    // participate (e.g. the untaken side of an alternation).
    std::vector<std::pair<int, int>> groups;

    /**
     * @brief Reports whether this Match represents a successful match.
     * @return True if start is a valid (non-negative) byte offset, false if no match was found.
     */
    bool ok() const { return start >= 0; }
};

class Regex {
public:
    // Compiles `pattern`. Check ok() before calling Search/Match --
    // querying a failed compile just returns "no match" rather than
    // asserting, but error() has a human-readable reason worth surfacing
    // to whatever UI is showing the pattern (e.g. :s's own status line).
    /**
     * @brief Compiles `pattern` into this Regex, optionally case-insensitively.
     * @param pattern The regex pattern text to compile.
     * @param ignore_case When true, matching folds ASCII letter case.
     */
    explicit Regex(const std::string &pattern, bool ignore_case = false);
    /**
     * @brief Destroys the Regex, releasing its compiled AST.
     */
    ~Regex();
    /**
     * @brief Move-constructs a Regex, transferring ownership of the compiled pattern from `other`.
     * @param other The Regex to move from; left in a valid but unspecified state.
     */
    Regex(Regex &&) noexcept;
    /**
     * @brief Move-assigns this Regex, transferring ownership of the compiled pattern from `other`.
     * @param other The Regex to move from; left in a valid but unspecified state.
     * @return Reference to this Regex.
     */
    Regex &operator=(Regex &&) noexcept;
    /**
     * @brief Deleted -- Regex is move-only; it owns its compiled AST via unique_ptr and is not copyable.
     * @param other Unused; copy construction is not supported.
     */
    Regex(const Regex &) = delete;
    /**
     * @brief Deleted -- Regex is move-only; it owns its compiled AST via unique_ptr and is not copyable.
     * @param other Unused; copy assignment is not supported.
     * @return Not applicable; this overload is deleted.
     */
    Regex &operator=(const Regex &) = delete;

    /**
     * @brief Reports whether the pattern compiled successfully.
     * @return True if compilation succeeded and Search/Match may be used, false otherwise.
     */
    bool ok() const { return ok_; }
    /**
     * @brief Returns the human-readable compile error, if any.
     * @return The error message describing why compilation failed, or an empty string if it succeeded.
     */
    const std::string &error() const { return error_; }

    // First match starting at or after byte offset `from`.
    /**
     * @brief Finds the first match of the compiled pattern at or after a given byte offset.
     * @param text The subject string to search.
     * @param from The byte offset to start searching from.
     * @return A Match describing the first match found; ok() is false if there is no match or the pattern failed to compile.
     */
    Match Search(const std::string &text, int from = 0) const;
    // Whole-string match (anchors Search's result to start==0 &&
    // end==text.size() -- NOT the same as wrapping the pattern in ^...$,
    // since ^/$ inside the pattern already anchor to the subject's own
    // start/end per the class comment above; this just additionally
    // requires the match to *span* the whole string).
    /**
     * @brief Checks whether the pattern matches the entire subject string.
     * @param text The subject string to test.
     * @return True if a match spans exactly the whole string, false otherwise.
     */
    bool FullMatch(const std::string &text) const;
    /**
     * @brief Checks whether the pattern matches anywhere within the subject string.
     * @param text The subject string to search.
     * @return True if any match is found, false otherwise.
     */
    bool PartialMatch(const std::string &text) const { return Search(text).ok(); }

    // `repl` may reference capture groups as \1-\9 (Perl-style) or Vim's
    // own \0/& for the whole match -- both accepted, since mep's own :s
    // command has historically been documented against Vim's \0 convention
    // (see kBuiltinSubstitute in main.cpp) and callers shouldn't have to
    // care which style a given user typed.
    /**
     * @brief Replaces the first match of the pattern in `text` with an expanded replacement string.
     * @param text The subject string to search and replace within.
     * @param repl The replacement template; may reference capture groups via \1-\9 or Vim-style \0/&.
     * @return A new string with the first match replaced, or `text` unchanged if there was no match.
     */
    std::string ReplaceFirst(const std::string &text, const std::string &repl) const;
    /**
     * @brief Replaces every non-overlapping match of the pattern in `text` with an expanded replacement string.
     * @param text The subject string to search and replace within.
     * @param repl The replacement template; may reference capture groups via \1-\9 or Vim-style \0/&.
     * @return A new string with all matches replaced.
     */
    std::string ReplaceAll(const std::string &text, const std::string &repl) const;

    // Expands \1-\9 / \0 / & in `repl` against one already-found `m` (from
    // this Regex's own Search() over `text`) -- the piece ReplaceFirst/
    // ReplaceAll build on, exposed directly for callers that need to drive
    // their own find/replace loop instead (e.g. Editor::ExSubstitute's
    // per-line, count-tracking :s loop, which -- unlike ReplaceAll -- needs
    // to know how many replacements happened and respect a per-line
    // first-match-only vs every-match `g` flag).
    /**
     * @brief Expands a replacement template's capture-group references (\1-\9, \0, &) against one already-found match.
     * @param text The original subject string the match `m` was found in.
     * @param m The match whose captured groups supply the replacement text.
     * @param repl The replacement template to expand.
     * @return The expanded replacement string.
     */
    static std::string ExpandReplacement(const std::string &text, const Match &m, const std::string &repl);

    // Implementation detail (AST node), public only so the free-standing
    // Parser/Matcher classes in regex.cpp's anonymous namespace can name
    // it -- not part of the intended public API, don't build against it
    // from outside regex.cpp.
    struct Node;

private:
    std::unique_ptr<Node> root_;
    int group_count_ = 0;
    bool ignore_case_ = false;
    bool ok_ = false;
    std::string error_;
};

}  // namespace mep_regex

#endif  // MEP_REGEX_H
