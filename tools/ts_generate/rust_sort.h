// Rust's slice::sort_unstable (ipnsort, library/core/src/slice/sort in
// rustc 1.91.1, the compiler tree-sitter 0.25.10 was built with), ported so
// that elements comparing equal end up in the same order. tree-sitter
// sorts parse states by size with it, and many states tie on size: their
// final numbering is whatever order this algorithm leaves them in.
//
// The algorithm depends on the element type's size and whether it is
// Copy (its small-sort and partition choices); the caller says which Rust
// type it is sorting.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace tsgen {

struct RustSortType {
    size_t size;   // size_of::<T>()
    bool is_copy;  // every type tree-sitter sorts is Freeze
};

namespace rust_sort_detail {

template <class T, class Less>
struct Sorter {
    std::vector<T> &v;
    Less &is_less;
    RustSortType type;

    enum class Small { Fallback, General, Network };
    Small SmallKind() const {
        constexpr size_t kMaxStack = 4096;
        if (type.is_copy && type.size <= 8 && type.size * 32 <= kMaxStack) return Small::Network;
        if (type.size * 48 <= kMaxStack) return Small::General;
        return Small::Fallback;
    }
    size_t SmallThreshold() const { return SmallKind() == Small::Fallback ? 16 : 32; }

    // insert_tail(begin, tail) on v[base..]
    void InsertTail(size_t begin, size_t tail) {
        if (!is_less(v[tail], v[tail - 1])) return;
        T tmp = std::move(v[tail]);
        size_t j = tail;
        while (true) {
            v[j] = std::move(v[j - 1]);
            j--;
            if (j == begin) break;
            if (!is_less(tmp, v[j - 1])) break;
        }
        v[j] = std::move(tmp);
    }

    void InsertionSortShiftLeft(size_t base, size_t len, size_t offset) {
        for (size_t tail = base + offset; tail < base + len; tail++) InsertTail(base, tail);
    }

    void SwapIfLess(size_t base, size_t a, size_t b) {
        if (is_less(v[base + b], v[base + a])) std::swap(v[base + a], v[base + b]);
    }

    void Sort9(size_t base) {
        static const int net[][2] = {{0, 3}, {1, 7}, {2, 5}, {4, 8}, {0, 7}, {2, 4}, {3, 8}, {5, 6}, {0, 2},
                                     {1, 3}, {4, 5}, {7, 8}, {1, 4}, {3, 6}, {5, 7}, {0, 1}, {2, 4}, {3, 5},
                                     {6, 8}, {2, 3}, {4, 5}, {6, 7}, {1, 2}, {3, 4}, {5, 6}};
        for (const auto &p : net) SwapIfLess(base, static_cast<size_t>(p[0]), static_cast<size_t>(p[1]));
    }

    void Sort13(size_t base) {
        static const int net[][2] = {{0, 12}, {1, 10}, {2, 9},  {3, 7},  {5, 11}, {6, 8},  {1, 6},  {2, 3},  {4, 11},
                                     {7, 9},  {8, 10}, {0, 4},  {1, 2},  {3, 6},  {7, 8},  {9, 10}, {11, 12}, {4, 6},
                                     {5, 9},  {8, 11}, {10, 12}, {0, 5}, {3, 8},  {4, 7},  {6, 11}, {9, 10}, {0, 1},
                                     {2, 5},  {6, 9},  {7, 8},  {10, 11}, {1, 3}, {2, 4},  {5, 6},  {9, 10}, {1, 2},
                                     {3, 4},  {5, 7},  {6, 8},  {2, 3},  {4, 5},  {6, 7},  {8, 9},  {3, 4},  {5, 6}};
        for (const auto &p : net) SwapIfLess(base, static_cast<size_t>(p[0]), static_cast<size_t>(p[1]));
    }

    // bidirectional_merge: src[0..len] (two sorted halves) -> dst[0..len]
    void BidirectionalMerge(const std::vector<T> &src, std::vector<T> &dst) {
        size_t len = src.size();
        size_t half = len / 2;
        long left = 0, right = static_cast<long>(half), d = 0;
        long left_rev = static_cast<long>(half) - 1, right_rev = static_cast<long>(len) - 1, d_rev = static_cast<long>(len) - 1;
        for (size_t k = 0; k < half; k++) {
            bool is_l = !is_less(src[static_cast<size_t>(right)], src[static_cast<size_t>(left)]);
            dst[static_cast<size_t>(d++)] = is_l ? src[static_cast<size_t>(left)] : src[static_cast<size_t>(right)];
            right += !is_l;
            left += is_l;
            bool is_l2 = !is_less(src[static_cast<size_t>(right_rev)], src[static_cast<size_t>(left_rev)]);
            dst[static_cast<size_t>(d_rev--)] = is_l2 ? src[static_cast<size_t>(right_rev)] : src[static_cast<size_t>(left_rev)];
            right_rev -= is_l2;
            left_rev -= !is_l2;
        }
        long left_end = left_rev + 1;
        if (len % 2 != 0) {
            bool left_nonempty = left < left_end;
            dst[static_cast<size_t>(d)] = left_nonempty ? src[static_cast<size_t>(left)] : src[static_cast<size_t>(right)];
        }
    }

    void SmallSortNetwork(size_t base, size_t len) {
        if (len < 2) return;
        size_t half = len / 2;
        bool no_merge = len < 18;
        size_t region_base = base, region_len = no_merge ? len : half;
        while (true) {
            size_t presorted = 1;
            if (region_len >= 13) {
                Sort13(region_base);
                presorted = 13;
            } else if (region_len >= 9) {
                Sort9(region_base);
                presorted = 9;
            }
            InsertionSortShiftLeft(region_base, region_len, presorted);
            if (no_merge) return;
            if (region_base != base) break;
            region_base = base + half;
            region_len = len - half;
        }
        std::vector<T> src(v.begin() + static_cast<long>(base), v.begin() + static_cast<long>(base + len));
        std::vector<T> dst(len);
        BidirectionalMerge(src, dst);
        for (size_t i = 0; i < len; i++) v[base + i] = std::move(dst[i]);
    }

    // sort4_stable: v-like `src` at `s` -> dst at `d`
    void Sort4Stable(const std::vector<T> &src, size_t s, std::vector<T> &dst, size_t d) {
        bool c1 = is_less(src[s + 1], src[s]);
        bool c2 = is_less(src[s + 3], src[s + 2]);
        size_t a = s + c1, b = s + !c1, c = s + 2 + c2, dd = s + 2 + !c2;
        bool c3 = is_less(src[c], src[a]);
        bool c4 = is_less(src[dd], src[b]);
        size_t min = c3 ? c : a;
        size_t max = c4 ? b : dd;
        size_t unknown_left = c3 ? a : (c4 ? c : b);
        size_t unknown_right = c4 ? dd : (c3 ? b : c);
        bool c5 = is_less(src[unknown_right], src[unknown_left]);
        size_t lo = c5 ? unknown_right : unknown_left;
        size_t hi = c5 ? unknown_left : unknown_right;
        dst[d] = src[min];
        dst[d + 1] = src[lo];
        dst[d + 2] = src[hi];
        dst[d + 3] = src[max];
    }

    void SmallSortGeneral(size_t base, size_t len) {
        if (len < 2) return;
        std::vector<T> vv(v.begin() + static_cast<long>(base), v.begin() + static_cast<long>(base + len));
        std::vector<T> scratch(len + 16);
        size_t half = len / 2;
        size_t presorted;
        if (type.size <= 16 && len >= 16) {
            for (size_t off : {size_t(0), half}) {
                std::vector<T> tmp(8);
                Sort4Stable(vv, off, tmp, 0);
                Sort4Stable(vv, off + 4, tmp, 4);
                std::vector<T> merged(8);
                BidirectionalMerge(tmp, merged);
                for (size_t i = 0; i < 8; i++) scratch[off + i] = merged[i];
            }
            presorted = 8;
        } else if (len >= 8) {
            Sort4Stable(vv, 0, scratch, 0);
            Sort4Stable(vv, half, scratch, half);
            presorted = 4;
        } else {
            scratch[0] = vv[0];
            scratch[half] = vv[half];
            presorted = 1;
        }
        for (size_t off : {size_t(0), half}) {
            size_t desired = off == 0 ? half : len - half;
            for (size_t i = presorted; i < desired; i++) {
                scratch[off + i] = vv[off + i];
                // insert_tail(dst, dst + i) within scratch[off..]
                size_t tail = off + i;
                if (!is_less(scratch[tail], scratch[tail - 1])) continue;
                T tmp = scratch[tail];
                size_t j = tail;
                while (true) {
                    scratch[j] = scratch[j - 1];
                    j--;
                    if (j == off) break;
                    if (!is_less(tmp, scratch[j - 1])) break;
                }
                scratch[j] = tmp;
            }
        }
        scratch.resize(len);
        std::vector<T> out(len);
        BidirectionalMerge(scratch, out);
        for (size_t i = 0; i < len; i++) v[base + i] = std::move(out[i]);
    }

    void SmallSort(size_t base, size_t len) {
        switch (SmallKind()) {
            case Small::Network:
                SmallSortNetwork(base, len);
                break;
            case Small::General:
                SmallSortGeneral(base, len);
                break;
            case Small::Fallback:
                if (len >= 2) InsertionSortShiftLeft(base, len, 1);
                break;
        }
    }

    void SiftDown(size_t base, size_t len, size_t node) {
        while (true) {
            size_t child = 2 * node + 1;
            if (child >= len) break;
            if (child + 1 < len) child += is_less(v[base + child], v[base + child + 1]);
            if (!is_less(v[base + node], v[base + child])) break;
            std::swap(v[base + node], v[base + child]);
            node = child;
        }
    }

    void Heapsort(size_t base, size_t len) {
        for (size_t i = len + len / 2; i-- > 0;) {
            size_t sift_idx;
            if (i >= len) {
                sift_idx = i - len;
            } else {
                std::swap(v[base], v[base + i]);
                sift_idx = 0;
            }
            SiftDown(base, std::min(i, len), sift_idx);
        }
    }

    size_t Median3(size_t a, size_t b, size_t c) {
        bool x = is_less(v[a], v[b]);
        bool y = is_less(v[a], v[c]);
        if (x == y) {
            bool z = is_less(v[b], v[c]);
            return (z ^ x) ? c : b;
        }
        return a;
    }

    size_t Median3Rec(size_t a, size_t b, size_t c, size_t n) {
        if (n * 8 >= 64) {
            size_t n8 = n / 8;
            a = Median3Rec(a, a + n8 * 4, a + n8 * 7, n8);
            b = Median3Rec(b, b + n8 * 4, b + n8 * 7, n8);
            c = Median3Rec(c, c + n8 * 4, c + n8 * 7, n8);
        }
        return Median3(a, b, c);
    }

    size_t ChoosePivot(size_t base, size_t len) {
        size_t len_div_8 = len / 8;
        size_t a = base, b = base + len_div_8 * 4, c = base + len_div_8 * 7;
        if (len < 64) return Median3(a, b, c) - base;
        return Median3Rec(a, b, c, len_div_8) - base;
    }

    // The partitions see v[base+1 .. base+len] and the pivot at v[base].
    template <class L>
    size_t PartitionLomuto(size_t base, size_t len, L &less) {
        if (len == 0) return 0;
        const T &pivot = v[base - 1];
        T gap_value = v[base];
        size_t num_lt = 0, gap_pos = base;
        size_t right = base + 1;
        bool right_is_tmp = false;
        auto body = [&] {
            const T &r = right_is_tmp ? gap_value : v[right];
            bool right_is_lt = less(r, pivot);
            size_t left = base + num_lt;
            v[gap_pos] = v[left];
            v[left] = right_is_tmp ? gap_value : v[right];
            gap_pos = right;
            num_lt += right_is_lt;
            right++;
        };
        size_t unroll = type.size <= 16 ? 2 : 1;
        size_t unroll_end = base + len - (unroll - 1);
        while (right < unroll_end) {
            body();
            if (unroll == 2) body();
        }
        size_t end = base + len;
        while (true) {
            bool is_done = right == end;
            if (is_done) right_is_tmp = true;
            body();
            if (is_done) break;
        }
        return num_lt;
    }

    template <class L>
    size_t PartitionHoare(size_t base, size_t len, L &less) {
        if (len == 0) return 0;
        const T pivot = v[base - 1];
        std::optional<T> gap_value;
        size_t gap_pos = 0;
        size_t left = base, right = base + len;
        while (true) {
            while (left < right && less(v[left], pivot)) left++;
            while (true) {
                right--;
                if (left >= right || less(v[right], pivot)) break;
            }
            if (left >= right) break;
            if (!gap_value) {
                gap_value = v[left];
            } else {
                v[gap_pos] = v[left];
            }
            gap_pos = right;
            v[left] = v[right];
            left++;
        }
        if (gap_value) v[gap_pos] = *gap_value;
        return left - base;
    }

    template <class L>
    size_t Partition(size_t base, size_t len, size_t pivot, L &less) {
        if (len == 0) return 0;
        std::swap(v[base], v[base + pivot]);
        size_t num_lt = type.size <= 96 ? PartitionLomuto(base + 1, len - 1, less) : PartitionHoare(base + 1, len - 1, less);
        std::swap(v[base], v[base + num_lt]);
        return num_lt;
    }

    void Quicksort(size_t base, size_t len, std::optional<size_t> ancestor_pivot, uint32_t limit) {
        while (true) {
            if (len <= SmallThreshold()) {
                SmallSort(base, len);
                return;
            }
            if (limit == 0) {
                Heapsort(base, len);
                return;
            }
            limit--;
            size_t pivot_pos = ChoosePivot(base, len);
            if (ancestor_pivot && !is_less(v[*ancestor_pivot], v[base + pivot_pos])) {
                auto le = [&](const T &a, const T &b) { return !is_less(b, a); };
                size_t num_lt = Partition(base, len, pivot_pos, le);
                base += num_lt + 1;
                len -= num_lt + 1;
                ancestor_pivot.reset();
                continue;
            }
            size_t num_lt = Partition(base, len, pivot_pos, is_less);
            Quicksort(base, num_lt, ancestor_pivot, limit);
            ancestor_pivot = base + num_lt;
            base += num_lt + 1;
            len -= num_lt + 1;
        }
    }

    void Sort() {
        size_t len = v.size();
        if (len < 2) return;
        if (len <= 20) {
            InsertionSortShiftLeft(0, len, 1);
            return;
        }
        // find_existing_run
        size_t run_len = 2;
        bool descending = is_less(v[1], v[0]);
        if (descending) {
            while (run_len < len && is_less(v[run_len], v[run_len - 1])) run_len++;
        } else {
            while (run_len < len && !is_less(v[run_len], v[run_len - 1])) run_len++;
        }
        if (run_len == len) {
            if (descending) std::reverse(v.begin(), v.end());
            return;
        }
        uint32_t log = 0;
        for (size_t n = len | 1; n > 1; n >>= 1) log++;
        Quicksort(0, len, std::nullopt, 2 * log);
    }
};

}  // namespace rust_sort_detail

template <class T, class Less>
void RustSortUnstable(std::vector<T> &v, RustSortType type, Less is_less) {
    rust_sort_detail::Sorter<T, Less> sorter{v, is_less, type};
    sorter.Sort();
}

}  // namespace tsgen
