module;

#include <algorithm>
#include <string>
#include <vector>

module mep.diff;

namespace mep::diff {

namespace {

// What MyersDiffHunks reports when the edit distance runs past
// kMaxEditDistance: the whole of `a` replaced by the whole of `b`.
// Identical in shape to what the search itself produces for an input
// with no shared lines at all (both starts are 1, the first line of
// each side), so a caller can't tell the bound was hit except by the
// hunk being large.
std::vector<DiffHunk> WholeFileHunk(int n, int m) {
    if (n == 0 && m == 0) return {};
    return {DiffHunk{1, n, 1, m}};
}

}  // namespace

std::vector<DiffHunk> MyersDiffHunks(const std::vector<std::string> &a, const std::vector<std::string> &b) {
    int n = static_cast<int>(a.size()), m = static_cast<int>(b.size());
    if (n + m == 0) return {};
    // The search runs to at most kMaxEditDistance (see text_diff.cppm for
    // why it has to stop somewhere), so both the per-step V array and the
    // trace are sized against that bound rather than against the input.
    int max_d = std::min(n + m, kMaxEditDistance);
    // trace[d] stores the V array (x-coordinates of furthest-reaching D-paths
    // for each diagonal k) at step d, needed to walk the path back afterward.
    std::vector<std::vector<int>> trace;
    std::vector<int> v(static_cast<size_t>(2 * max_d + 1), 0);
    /**
     * @brief Converts a diagonal index k (which may be negative) into a non-negative offset into the v array.
     * @param k The diagonal index.
     * @return The corresponding index into the v array.
     */
    auto vidx = [max_d](int k) { return k + max_d; };
    int found_d = -1;
    for (int d = 0; d <= max_d; d++) {
        // Only diagonals -d..d are live at step d, so the trace keeps just
        // those rather than a full-width copy -- the difference between
        // O(D * (N + M)) and O(D^2) memory, and on a big input with a big
        // D that is the difference between gigabytes and megabytes.
        trace.emplace_back(v.begin() + vidx(-d), v.begin() + vidx(d) + 1);
        for (int k = -d; k <= d; k += 2) {
            int x;
            if (k == -d || (k != d && v[static_cast<size_t>(vidx(k - 1))] < v[static_cast<size_t>(vidx(k + 1))])) {
                x = v[static_cast<size_t>(vidx(k + 1))];
            } else {
                x = v[static_cast<size_t>(vidx(k - 1))] + 1;
            }
            int y = x - k;
            while (x < n && y < m && a[static_cast<size_t>(x)] == b[static_cast<size_t>(y)]) {
                x++;
                y++;
            }
            v[static_cast<size_t>(vidx(k))] = x;
            if (x >= n && y >= m) {
                found_d = d;
                break;
            }
        }
        if (found_d >= 0) break;
    }

    // The two sides are too far apart to describe usefully line by line
    // (or at all, within the bound) -- say so as one hunk rather than
    // spending the memory to spell it out. See text_diff.cppm.
    if (found_d < 0) return WholeFileHunk(n, m);

    // Walk the recorded traces backward from (n,m) to (0,0), emitting
    // per-line ops, then coalesce contiguous runs into hunks below.
    // trace[d] holds only diagonals -d..d (see where it is filled), so a
    // diagonal k sits at k + d within it rather than at vidx(k).
    struct Op {
        char kind;  // '=' / '-' (only in a) / '+' (only in b)
    };
    std::vector<Op> ops;
    int x = n, y = m;
    for (int d = found_d; d > 0; d--) {
        const std::vector<int> &vd = trace[static_cast<size_t>(d)];
        auto at = [&vd, d](int kk) { return vd[static_cast<size_t>(kk + d)]; };
        int k = x - y;
        int prev_k = (k == -d || (k != d && at(k - 1) < at(k + 1))) ? k + 1 : k - 1;
        int prev_x = at(prev_k);
        int prev_y = prev_x - prev_k;
        while (x > prev_x && y > prev_y) {
            ops.push_back({'='});
            x--;
            y--;
        }
        if (x == prev_x) {
            ops.push_back({'+'});
            y--;
        } else {
            ops.push_back({'-'});
            x--;
        }
    }
    while (x > 0 && y > 0) {
        ops.push_back({'='});
        x--;
        y--;
    }
    std::reverse(ops.begin(), ops.end());

    std::vector<DiffHunk> hunks;
    size_t i = 0;
    int a_pos = 0, b_pos = 0;  // 0-indexed count of `a`/`b` lines consumed so far
    while (i < ops.size()) {
        if (ops[i].kind == '=') {
            a_pos++;
            b_pos++;
            i++;
            continue;
        }
        // A pure insertion has no `a` anchor of its own -- gitsigns
        // convention: report it at the line *after* which it was inserted
        // (0 if at the very top), i.e. `a_pos` (0-indexed) before the hunk.
        int old_start = a_pos, new_start = b_pos;
        int old_count = 0, new_count = 0;
        while (i < ops.size() && ops[i].kind != '=') {
            if (ops[i].kind == '-') {
                old_count++;
                a_pos++;
            } else {
                new_count++;
                b_pos++;
            }
            i++;
        }
        hunks.push_back({old_start + 1, old_count, new_start + 1, new_count});
    }
    return hunks;
}

}  // namespace mep::diff
