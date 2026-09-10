module;

#include <string>
#include <vector>

export module mep.diff;

// Myers diff (NVIM_PARITY_PLAN.md Part IV Phase 17): the equivalent of
// Neovim's built-in vim.diff(), needed so git-gutter hunks don't have to
// shell `git diff` per keystroke. A small, dependency-free module (not
// editor.h/editor.cpp, where this originated) specifically so it can be
// shared by things that don't want editor.cpp's own large dependency
// footprint -- CRDT_PERFORMANCE_PLAN.md Phase 3 reuses it from
// collab_session.cpp for a multi-hunk local-edit diff, which needs to
// stay linkable without pulling in the whole Editor (mep-collab-session-
// test, in particular, builds collab_session.cpp with no editor.cpp at
// all). editor.h/editor.cpp still declare/use DiffHunk/MyersDiffHunks by
// name via `using` aliases into this namespace, so every existing call
// site (Editor::GitGutterRefresh/GitStageHunk, mep.diff_lines) is
// unchanged.
//
// BUILD_PERFORMANCE_PLAN.md Round 2 Phase D: this project's first real
// C++20 module (a small, macro-free, standard-library-only leaf was
// deliberately chosen as the pilot -- see that phase's own notes on why
// most of the codebase, which leans heavily on macros and deep,
// established #include chains, isn't a realistic modules-conversion
// candidate without much larger, riskier surgery). `module;` above opens
// a global module fragment purely to house the *legacy* #includes this
// module's own interface needs (<string>/<vector>) -- the standard's own
// mechanism for a module that still depends on non-modular headers.

export namespace mep::diff {

struct DiffHunk {
    int old_start, old_count, new_start, new_count;
};

// Classic O(ND) Myers diff (Myers 1986), operating on opaque line indices
// via equality only -- returns the *edit script* as a sequence of (line
// present only in `a`) / (line present only in `b`) markers, coalesced
// into contiguous hunks. old_start/new_start are 1-indexed (gitsigns
// convention: a pure insertion is reported at the line after which it
// was inserted, 0 if at the very top).
/**
 * @brief Computes the Myers O(ND) diff between two line sequences, coalesced into contiguous hunks.
 * @param a The "old" line sequence.
 * @param b The "new" line sequence.
 * @return The edit script as a sequence of DiffHunk ranges.
 */
std::vector<DiffHunk> MyersDiffHunks(const std::vector<std::string> &a, const std::vector<std::string> &b);

}  // namespace mep::diff
