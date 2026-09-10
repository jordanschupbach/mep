module;

#include <algorithm>
#include <string>
#include <vector>

module mep.diff;

namespace mep::diff {

std::vector<DiffHunk> MyersDiffHunks(const std::vector<std::string> &a, const std::vector<std::string> &b) {
    int n = static_cast<int>(a.size()), m = static_cast<int>(b.size());
    int max_d = n + m;
    if (max_d == 0) return {};
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
        trace.push_back(v);
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

    // Walk the recorded traces backward from (n,m) to (0,0), emitting
    // per-line ops, then coalesce contiguous runs into hunks below.
    struct Op {
        char kind;  // '=' / '-' (only in a) / '+' (only in b)
    };
    std::vector<Op> ops;
    int x = n, y = m;
    for (int d = found_d; d > 0; d--) {
        const std::vector<int> &vd = trace[static_cast<size_t>(d)];
        int k = x - y;
        int prev_k = (k == -d || (k != d && vd[static_cast<size_t>(vidx(k - 1))] < vd[static_cast<size_t>(vidx(k + 1))])) ? k + 1 : k - 1;
        int prev_x = vd[static_cast<size_t>(vidx(prev_k))];
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
