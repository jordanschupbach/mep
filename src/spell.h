#pragma once

// mep's own dependency-free spell checker -- there is no libhunspell/aspell
// linkage and no compiled .spl format anywhere in the build (a deliberate
// "keep dependencies to an absolute minimum" choice, same ethos as the
// in-house fzf-style FuzzyScore in editor.h). Instead this loads a plaintext
// one-word-per-line wordlist bundled as an asset (assets/spell/en_US.words,
// SCOWL-derived) and does its own membership check + edit-distance/phonetic
// suggestion ranking. This is Neovim's own approach minus the binary .spl
// container: a word list plus a hand-rolled suggester.
//
// Checking (IsMisspelled) is case-insensitive set membership, cheap enough to
// run over every visible word each frame for the SpellBad squiggle. Suggestion
// (Suggest) is on-demand only -- it scans the length-bucketed candidate pool
// and ranks by Levenshtein distance, tie-broken by a Soundex phonetic key and
// by wordlist order (a frequency proxy). A user-owned personal dictionary
// (nvim's spellfile model) lives in plaintext files the caller supplies, so a
// word the user "adds" passes immediately and persists across sessions.

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace spell {

class SpellChecker {
   public:
    // Loads the main wordlist (one word per line; blank lines and lines
    // starting with '#' ignored). Replaces any previously loaded dictionary.
    // Returns false if the file could not be opened (checker then treats every
    // word as correct, i.e. spell-checking is effectively off, rather than
    // flagging the whole buffer). The caller resolves the asset path -- e.g.
    // via main.cpp's DashboardAssetPath("spell/en_US.words") -- so this module
    // stays free of any asset-location logic.
    bool LoadDictionary(const std::string &path);

    // Points the checker at the user's personal spellfiles and loads their
    // current contents: `good_path` holds words to treat as correct (nvim `zg`),
    // `wrong_path` holds words to always flag even if in the main dictionary
    // (nvim `zw`). Either path may be empty to disable that half (e.g. in
    // tests); a missing file is fine (treated as empty, created on first add).
    // Remembers the paths so AddGood/AddWrong can append to them.
    void SetPersonalFiles(const std::string &good_path, const std::string &wrong_path);

    // True once a dictionary with at least one word has been loaded. When
    // false, IsMisspelled always returns false so nothing is flagged.
    bool ready() const { return !words_.empty(); }

    // Whether `word` (a single token, no surrounding whitespace) should be
    // flagged as misspelled. Case-insensitive. Tokens with no ASCII letter,
    // tokens containing a digit, and (by default) all-uppercase acronyms are
    // never flagged. A word in the personal "wrong" list is always flagged; a
    // word in the personal "good" list is never flagged.
    bool IsMisspelled(std::string_view word) const;

    // Up to `max` correction candidates for `word`, best first. Ranked by
    // edit distance, then Soundex match, then wordlist order. Each suggestion
    // is re-cased to match `word`'s pattern (Titlecase / ALLCAPS / lower).
    // Returns empty if the checker is not ready or nothing is close enough.
    std::vector<std::string> Suggest(std::string_view word, int max = 9) const;

    // Adds `word` to the personal "good" list: it passes IsMisspelled
    // immediately and is appended to the good spellfile (dirs created as
    // needed). A no-op for the file write if no good path was set.
    void AddGood(std::string_view word);

    // Adds `word` to the personal "wrong" list: it now fails IsMisspelled even
    // if the main dictionary contains it, and is appended to the wrong
    // spellfile. A no-op for the file write if no wrong path was set.
    void AddWrong(std::string_view word);

    // Whether to skip all-uppercase tokens of length >= 2 (acronyms like NASA,
    // JSON). Defaults true, matching vim's default of not flagging them.
    void set_ignore_uppercase(bool v) { ignore_uppercase_ = v; }

   private:
    // Original-case words in file order (order = frequency proxy for ranking).
    std::vector<std::string> words_;
    // Lowercased membership set over words_ + personal good, for O(1) checking.
    std::unordered_set<std::string> lower_set_;
    // Lowercased personal overrides.
    std::unordered_set<std::string> personal_good_;
    std::unordered_set<std::string> personal_wrong_;
    // word length -> indices into words_, for banding the suggestion search.
    std::unordered_map<int, std::vector<int>> by_len_;
    // Lowercased common English word -> frequency rank (0 = most common). The
    // wordlist is only alphabetical, so with no other signal a distance tie
    // between e.g. "the"/"tea"/"tee" for the typo "teh" would resolve
    // alphabetically ("tea"). This built-in high-frequency tier is the primary
    // tie-break after edit distance so the genuinely-likeliest word wins.
    std::unordered_map<std::string, int> common_rank_;

    std::string good_path_;
    std::string wrong_path_;
    bool ignore_uppercase_ = true;

    void IndexWord(const std::string &word);
};

// --- small pure helpers, exposed for unit testing -------------------------

// ASCII-lowercased copy (non-ASCII bytes pass through unchanged).
std::string ToLowerAscii(std::string_view s);

// Levenshtein edit distance, early-exiting once the running minimum exceeds
// `max_dist` (returns max_dist + 1 in that case) so callers can cheaply reject
// far-apart candidates.
int EditDistance(std::string_view a, std::string_view b, int max_dist);

// Classic 4-character Soundex key (e.g. "Robert" -> "R163"). Empty for a
// token with no leading ASCII letter. Used only as a suggestion tie-breaker.
std::string Soundex(std::string_view word);

// Re-cases `suggestion` to follow `pattern`'s shape: ALLCAPS if pattern is all
// uppercase (len >= 2), Titlecase if pattern starts uppercase, else unchanged.
std::string MatchCase(std::string_view suggestion, std::string_view pattern);

// Default personal spellfile paths under $XDG_CONFIG_HOME/mep/spell (falling
// back to ~/.config/mep/spell), mirroring mep's init.lua/parsers convention.
// Returns empty strings if neither env var is usable.
void DefaultPersonalPaths(std::string *good_out, std::string *wrong_out);

}  // namespace spell
