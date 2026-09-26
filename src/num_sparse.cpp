#include "num_sparse.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <cmath>
#include <numeric>
#include <queue>

namespace num {
namespace {

// Index helper: every container here is indexed by int (DOF numbers,
// matrix rows) but stored in std::vector, so the cast happens in one
// place rather than at several hundred call sites.
inline std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

}  // namespace

// ---------------------------------------------------------------------
// SparseMatrix
// ---------------------------------------------------------------------

SparseMatrix SparseMatrix::FromTriplets(int rows, int cols, const std::vector<Triplet> &triplets) {
    SparseMatrix m;
    if (rows <= 0 || cols <= 0) return m;
    m.rows_ = rows;
    m.cols_ = cols;

    // Counting sort into rows, then sort each row by column and sum
    // duplicates in one pass. Avoids building an intermediate map, which
    // for an assembly of millions of triplets dominates everything else.
    std::vector<int> counts(Idx(rows) + 1, 0);
    for (const Triplet &t : triplets) {
        if (t.row < 0 || t.row >= rows || t.col < 0 || t.col >= cols) continue;
        ++counts[Idx(t.row) + 1];
    }
    for (int i = 0; i < rows; ++i) counts[Idx(i) + 1] += counts[Idx(i)];
    const int total = counts[Idx(rows)];

    std::vector<int> scratch_col(Idx(total), 0);
    std::vector<double> scratch_val(Idx(total), 0.0);
    std::vector<int> cursor = counts;
    for (const Triplet &t : triplets) {
        if (t.row < 0 || t.row >= rows || t.col < 0 || t.col >= cols) continue;
        const int p = cursor[Idx(t.row)]++;
        scratch_col[Idx(p)] = t.col;
        scratch_val[Idx(p)] = t.value;
    }

    m.row_start_.assign(Idx(rows) + 1, 0);
    m.col_index_.reserve(Idx(total));
    m.values_.reserve(Idx(total));
    std::vector<int> order;
    for (int r = 0; r < rows; ++r) {
        const int begin = counts[Idx(r)];
        const int end = counts[Idx(r) + 1];
        order.resize(Idx(end - begin));
        std::iota(order.begin(), order.end(), begin);
        std::sort(order.begin(), order.end(),
                  [&scratch_col](int a, int b) { return scratch_col[Idx(a)] < scratch_col[Idx(b)]; });
        int i = 0;
        const int count = end - begin;
        while (i < count) {
            const int col = scratch_col[Idx(order[Idx(i)])];
            double sum = 0.0;
            while (i < count && scratch_col[Idx(order[Idx(i)])] == col) {
                sum += scratch_val[Idx(order[Idx(i)])];
                ++i;
            }
            // Exact zeros are dropped: they cost memory and factorization
            // time forever after, and an assembled coefficient that
            // cancels to exactly zero carries no structural meaning.
            if (sum != 0.0) {
                m.col_index_.push_back(col);
                m.values_.push_back(sum);
            }
        }
        m.row_start_[Idx(r) + 1] = static_cast<int>(m.col_index_.size());
    }
    return m;
}

SparseMatrix SparseMatrix::Identity(int n) {
    std::vector<Triplet> t;
    t.reserve(Idx(n));
    for (int i = 0; i < n; ++i) t.push_back(Triplet{i, i, 1.0});
    return FromTriplets(n, n, t);
}

double SparseMatrix::At(int row, int col) const {
    if (row < 0 || row >= rows_ || col < 0 || col >= cols_) return 0.0;
    const int begin = row_start_[Idx(row)];
    const int end = row_start_[Idx(row) + 1];
    const auto first = col_index_.begin() + begin;
    const auto last = col_index_.begin() + end;
    const auto found = std::lower_bound(first, last, col);
    if (found == last || *found != col) return 0.0;
    return values_[Idx(static_cast<int>(found - col_index_.begin()))];
}

void SparseMatrix::Multiply(const std::vector<double> &x, std::vector<double> *y) const {
    y->assign(Idx(rows_), 0.0);
    if (static_cast<int>(x.size()) != cols_) return;
    for (int r = 0; r < rows_; ++r) {
        double sum = 0.0;
        for (int p = row_start_[Idx(r)]; p < row_start_[Idx(r) + 1]; ++p) {
            sum += values_[Idx(p)] * x[Idx(col_index_[Idx(p)])];
        }
        (*y)[Idx(r)] = sum;
    }
}

void SparseMatrix::MultiplyTransposed(const std::vector<double> &x, std::vector<double> *y) const {
    y->assign(Idx(cols_), 0.0);
    if (static_cast<int>(x.size()) != rows_) return;
    for (int r = 0; r < rows_; ++r) {
        const double xr = x[Idx(r)];
        if (xr == 0.0) continue;
        for (int p = row_start_[Idx(r)]; p < row_start_[Idx(r) + 1]; ++p) {
            (*y)[Idx(col_index_[Idx(p)])] += values_[Idx(p)] * xr;
        }
    }
}

SparseMatrix SparseMatrix::Transposed() const {
    std::vector<Triplet> t;
    t.reserve(values_.size());
    for (int r = 0; r < rows_; ++r) {
        for (int p = row_start_[Idx(r)]; p < row_start_[Idx(r) + 1]; ++p) {
            t.push_back(Triplet{col_index_[Idx(p)], r, values_[Idx(p)]});
        }
    }
    return FromTriplets(cols_, rows_, t);
}

SparseMatrix SparseMatrix::TransposedTimesSelf() const {
    // Row r of A contributes the outer product of that row with itself to
    // A^T*A. Accumulating row by row keeps the working set to one row's
    // pattern, rather than materializing A^T first.
    std::vector<Triplet> t;
    for (int r = 0; r < rows_; ++r) {
        const int begin = row_start_[Idx(r)];
        const int end = row_start_[Idx(r) + 1];
        for (int p = begin; p < end; ++p) {
            for (int q = begin; q < end; ++q) {
                t.push_back(Triplet{col_index_[Idx(p)], col_index_[Idx(q)], values_[Idx(p)] * values_[Idx(q)]});
            }
        }
    }
    return FromTriplets(cols_, cols_, t);
}

bool SparseMatrix::IsSymmetric(double tol) const {
    if (rows_ != cols_) return false;
    for (int r = 0; r < rows_; ++r) {
        for (int p = row_start_[Idx(r)]; p < row_start_[Idx(r) + 1]; ++p) {
            const int c = col_index_[Idx(p)];
            if (std::fabs(values_[Idx(p)] - At(c, r)) > tol) return false;
        }
    }
    return true;
}

SparseMatrix SparseMatrix::Permuted(const std::vector<int> &perm) const {
    const std::vector<int> inv = InvertPermutation(perm);
    std::vector<Triplet> t;
    t.reserve(values_.size());
    for (int r = 0; r < rows_; ++r) {
        for (int p = row_start_[Idx(r)]; p < row_start_[Idx(r) + 1]; ++p) {
            t.push_back(Triplet{inv[Idx(r)], inv[Idx(col_index_[Idx(p)])], values_[Idx(p)]});
        }
    }
    return FromTriplets(rows_, cols_, t);
}

// ---------------------------------------------------------------------
// Orderings
// ---------------------------------------------------------------------

std::vector<int> InvertPermutation(const std::vector<int> &perm) {
    std::vector<int> inv(perm.size(), 0);
    for (std::size_t i = 0; i < perm.size(); ++i) inv[Idx(perm[i])] = static_cast<int>(i);
    return inv;
}

namespace {

// Adjacency (the matrix graph), excluding self-loops. Both orderings work
// from this rather than from the matrix directly.
std::vector<std::vector<int>> BuildAdjacency(const SparseMatrix &a) {
    const int n = a.Rows();
    std::vector<std::vector<int>> adj(Idx(n));
    const std::vector<int> &rs = a.RowStart();
    const std::vector<int> &ci = a.ColIndex();
    for (int r = 0; r < n; ++r) {
        for (int p = rs[Idx(r)]; p < rs[Idx(r) + 1]; ++p) {
            const int c = ci[Idx(p)];
            if (c != r) adj[Idx(r)].push_back(c);
        }
    }
    return adj;
}

// A pseudo-peripheral node: one at (nearly) maximum eccentricity, which
// is where a Cuthill-McKee sweep should start to produce the narrowest
// band. Found by George and Liu's iteration -- sweep from any node, take
// a minimum-degree node in the last level, repeat while the eccentricity
// keeps rising. Bounded so a pathological graph cannot loop.
int PseudoPeripheralNode(const std::vector<std::vector<int>> &adj, int start, std::vector<int> *level_scratch) {
    const int n = static_cast<int>(adj.size());
    int current = start;
    int best_eccentricity = -1;
    for (int sweep = 0; sweep < 16; ++sweep) {
        std::vector<int> &level = *level_scratch;
        level.assign(Idx(n), -1);
        std::vector<int> queue;
        queue.push_back(current);
        level[Idx(current)] = 0;
        std::size_t head = 0;
        int max_level = 0;
        while (head < queue.size()) {
            const int u = queue[head++];
            for (int v : adj[Idx(u)]) {
                if (level[Idx(v)] < 0) {
                    level[Idx(v)] = level[Idx(u)] + 1;
                    max_level = std::max(max_level, level[Idx(v)]);
                    queue.push_back(v);
                }
            }
        }
        if (max_level <= best_eccentricity) break;
        best_eccentricity = max_level;
        // Restart from the least-connected node of the deepest level.
        int next = current;
        std::size_t best_degree = static_cast<std::size_t>(-1);
        for (int v = 0; v < n; ++v) {
            if (level[Idx(v)] == max_level && adj[Idx(v)].size() < best_degree) {
                best_degree = adj[Idx(v)].size();
                next = v;
            }
        }
        if (next == current) break;
        current = next;
    }
    return current;
}

std::vector<int> CuthillMcKee(const SparseMatrix &a) {
    const int n = a.Rows();
    const std::vector<std::vector<int>> adj = BuildAdjacency(a);
    std::vector<int> visited(Idx(n), 0);
    std::vector<int> order;
    order.reserve(Idx(n));
    std::vector<int> level_scratch;

    // Every connected component gets its own sweep -- a mesh of several
    // disconnected bodies (an assembly, Part E.6) is the normal case, not
    // an error, and a single BFS would silently order only one of them.
    for (int seed = 0; seed < n; ++seed) {
        if (visited[Idx(seed)]) continue;
        const int start = PseudoPeripheralNode(adj, seed, &level_scratch);
        if (visited[Idx(start)]) continue;
        std::vector<int> queue;
        queue.push_back(start);
        visited[Idx(start)] = 1;
        std::size_t head = 0;
        while (head < queue.size()) {
            const int u = queue[head++];
            order.push_back(u);
            // Neighbours entered in increasing degree: the ordering that
            // makes Cuthill-McKee produce a narrow band rather than a
            // merely breadth-first one.
            std::vector<int> neighbours;
            for (int v : adj[Idx(u)]) {
                if (!visited[Idx(v)]) neighbours.push_back(v);
            }
            std::sort(neighbours.begin(), neighbours.end(),
                      [&adj](int x, int y) { return adj[Idx(x)].size() < adj[Idx(y)].size(); });
            for (int v : neighbours) {
                visited[Idx(v)] = 1;
                queue.push_back(v);
            }
        }
    }
    // The reversal is what turns Cuthill-McKee into *Reverse*
    // Cuthill-McKee: same bandwidth, but markedly less fill under
    // factorization, which is the property that matters here.
    std::reverse(order.begin(), order.end());
    return order;
}


// --- Approximate minimum degree -------------------------------------------
//
// THE POINT OF THE QUOTIENT GRAPH. Minimum degree above works by forming
// the clique that elimination creates, explicitly: eliminate a node, and
// every pair of its neighbours gains an edge. That is exactly right and
// it is why the routine is quadratic -- a node of degree d costs d^2
// edge insertions, and in three dimensions d grows as elimination
// proceeds, so the graph the algorithm is walking becomes far denser than
// the matrix it came from. Past a few tens of thousands of unknowns the
// ordering costs more than the factorization it exists to speed up, which
// is why it used to give up and fall back to Cuthill-McKee.
//
// AMD never forms the clique. An eliminated node becomes an *element*,
// and a live variable records which elements it touches rather than which
// variables they imply; the clique is represented by the element, in
// space proportional to its size rather than its size squared. Three
// further things make it fast enough to leave on:
//
//   * APPROXIMATE DEGREES. The exact external degree of a variable needs
//     the union of the elements around it, which is as expensive as the
//     clique was. The bound |L_p \ i| + sum over the other elements of
//     |L_e \ L_p| is computable in one pass over the element lists, is
//     never smaller than the truth, and is exact whenever a variable
//     touches at most two elements -- which most of them do.
//   * SUPERVARIABLES. Variables with identical adjacency are
//     indistinguishable and can be eliminated together. On a structural
//     mesh this is not a small saving: the three degrees of freedom of a
//     node have exactly the same pattern, so the graph collapses
//     threefold before anything else happens.
//   * MASS ELIMINATION AND ELEMENT ABSORPTION. A variable whose whole
//     remaining neighbourhood is the element just formed costs nothing to
//     eliminate next, so it goes now; an element whose variable list is
//     contained in the new one carries no information and is dropped.
//
// The result is a different ordering from exact minimum degree, not a
// worse one: the approximation is of the *degree*, which is a heuristic
// choice of pivot, not of the factorization.
std::vector<int> ApproximateMinimumDegree(const SparseMatrix &a) {
    const int n = a.Rows();
    const std::vector<std::vector<int>> initial = BuildAdjacency(a);

    std::vector<std::vector<int>> var_adj(Idx(n));
    std::vector<std::vector<int>> elem_adj(Idx(n));
    std::vector<std::vector<int>> elem_vars(Idx(n));
    std::vector<std::vector<int>> members(Idx(n));
    std::vector<int> degree(Idx(n), 0);
    std::vector<int> weight(Idx(n), 1);
    std::vector<int> elem_size(Idx(n), 0);
    std::vector<char> alive(Idx(n), 1);
    std::vector<char> is_element(Idx(n), 0);

    for (int i = 0; i < n; ++i) {
        std::vector<int> &list = var_adj[Idx(i)];
        list = initial[Idx(i)];
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());
        list.erase(std::remove(list.begin(), list.end(), i), list.end());
        degree[Idx(i)] = static_cast<int>(list.size());
        members[Idx(i)].push_back(i);
    }

    // Degree buckets with lazy deletion: a variable's degree only ever
    // changes upward here, so a stale entry is recognised by its degree no
    // longer matching the bucket it sits in.
    std::vector<std::vector<int>> bucket(Idx(n + 1));
    for (int i = 0; i < n; ++i) bucket[Idx(degree[Idx(i)])].push_back(i);
    int min_degree = 0;

    std::vector<int> order;
    order.reserve(Idx(n));
    std::vector<int> mark(Idx(n), -1);
    std::vector<int> element_work(Idx(n), 0);
    std::vector<char> element_seen(Idx(n), 0);
    int eliminated = 0;
    int stamp = 0;

    while (eliminated < n) {
        int p = -1;
        while (min_degree <= n) {
            std::vector<int> &at = bucket[Idx(min_degree)];
            while (!at.empty()) {
                const int candidate = at.back();
                at.pop_back();
                if (!alive[Idx(candidate)] || is_element[Idx(candidate)]) continue;
                if (degree[Idx(candidate)] != min_degree) continue;
                p = candidate;
                break;
            }
            if (p >= 0) break;
            ++min_degree;
        }
        if (p < 0) break;

        // L_p: every live variable reachable from p, through its own
        // edges and through the elements it touches.
        ++stamp;
        std::vector<int> lp;
        mark[Idx(p)] = stamp;
        for (const int v : var_adj[Idx(p)]) {
            if (!alive[Idx(v)] || is_element[Idx(v)] || mark[Idx(v)] == stamp) continue;
            mark[Idx(v)] = stamp;
            lp.push_back(v);
        }
        for (const int e : elem_adj[Idx(p)]) {
            if (!is_element[Idx(e)] || !alive[Idx(e)]) continue;
            for (const int v : elem_vars[Idx(e)]) {
                if (!alive[Idx(v)] || is_element[Idx(v)] || mark[Idx(v)] == stamp) continue;
                mark[Idx(v)] = stamp;
                lp.push_back(v);
            }
        }
        // The elements p touched are absorbed into p: everything they
        // said is now said by L_p.
        for (const int e : elem_adj[Idx(p)]) alive[Idx(e)] = 0;

        order.push_back(p);
        eliminated += weight[Idx(p)];
        alive[Idx(p)] = 1;
        is_element[Idx(p)] = 1;
        var_adj[Idx(p)].clear();
        elem_adj[Idx(p)].clear();
        elem_vars[Idx(p)] = lp;
        int lp_weight = 0;
        for (const int v : lp) lp_weight += weight[Idx(v)];
        elem_size[Idx(p)] = lp_weight;

        // Rewire each variable of L_p onto the new element, dropping the
        // edges the element now covers -- which is what keeps the
        // variable lists shrinking instead of growing.
        for (const int i : lp) {
            std::vector<int> &vars = var_adj[Idx(i)];
            std::vector<int> kept;
            kept.reserve(vars.size());
            for (const int v : vars) {
                if (!alive[Idx(v)] || is_element[Idx(v)] || v == p) continue;
                if (mark[Idx(v)] == stamp) continue;  // now covered by element p
                kept.push_back(v);
            }
            vars.swap(kept);
            std::vector<int> &elems = elem_adj[Idx(i)];
            std::vector<int> kept_elems;
            kept_elems.reserve(elems.size() + 1);
            for (const int e : elems) {
                if (!is_element[Idx(e)] || !alive[Idx(e)]) continue;
                kept_elems.push_back(e);
            }
            kept_elems.push_back(p);
            elems.swap(kept_elems);
        }

        // The approximate degree, in two passes over the element lists.
        // w[e] counts what is left of element e once L_p is taken out of
        // it; an element that has nothing left is contained in L_p and is
        // absorbed.
        for (const int i : lp) {
            for (const int e : elem_adj[Idx(i)]) {
                if (e == p) continue;
                if (!element_seen[Idx(e)]) {
                    element_seen[Idx(e)] = 1;
                    element_work[Idx(e)] = elem_size[Idx(e)];
                }
                element_work[Idx(e)] -= weight[Idx(i)];
            }
        }
        for (const int i : lp) {
            long long approximate = static_cast<long long>(lp_weight) - weight[Idx(i)];
            for (const int e : elem_adj[Idx(i)]) {
                if (e == p) continue;
                approximate += std::max(0, element_work[Idx(e)]);
            }
            for (const int v : var_adj[Idx(i)]) approximate += weight[Idx(v)];
            const long long bounded =
                std::min<long long>(approximate, static_cast<long long>(n) - eliminated);
            degree[Idx(i)] = static_cast<int>(std::max<long long>(0, bounded));
        }
        for (const int i : lp) {
            for (const int e : elem_adj[Idx(i)]) {
                if (e == p || !element_seen[Idx(e)]) continue;
                if (element_work[Idx(e)] <= 0) alive[Idx(e)] = 0;
                element_seen[Idx(e)] = 0;
            }
        }
        // Absorbed elements leave stale entries behind; they are filtered
        // when next walked rather than hunted down now.

        // SUPERVARIABLES. Two variables of L_p with the same neighbours
        // are indistinguishable for the rest of the elimination, so one
        // absorbs the other. Hashed first, compared only on collision.
        std::map<unsigned long long, std::vector<int>> by_hash;
        for (const int i : lp) {
            if (!alive[Idx(i)] || is_element[Idx(i)]) continue;
            unsigned long long hash = 1469598103934665603ull;
            for (const int v : var_adj[Idx(i)]) hash = (hash ^ static_cast<unsigned>(v)) * 1099511628211ull;
            for (const int e : elem_adj[Idx(i)]) hash = (hash ^ static_cast<unsigned>(e + n)) * 1099511628211ull;
            by_hash[hash].push_back(i);
        }
        for (auto &entry : by_hash) {
            std::vector<int> &group = entry.second;
            for (std::size_t x = 0; x < group.size(); ++x) {
                const int i = group[x];
                if (!alive[Idx(i)] || is_element[Idx(i)]) continue;
                for (std::size_t y = x + 1; y < group.size(); ++y) {
                    const int j = group[y];
                    if (!alive[Idx(j)] || is_element[Idx(j)]) continue;
                    if (var_adj[Idx(i)] != var_adj[Idx(j)]) continue;
                    if (elem_adj[Idx(i)] != elem_adj[Idx(j)]) continue;
                    weight[Idx(i)] += weight[Idx(j)];
                    for (const int member : members[Idx(j)]) members[Idx(i)].push_back(member);
                    members[Idx(j)].clear();
                    alive[Idx(j)] = 0;
                    eliminated += 0;  // j is absorbed, not eliminated
                    degree[Idx(i)] = std::max(0, degree[Idx(i)] - weight[Idx(j)]);
                }
            }
        }
        // A variable absorbed into another is gone from every list it was
        // in; the filters above drop it on the next walk.
        for (const int i : lp) {
            if (!alive[Idx(i)] || is_element[Idx(i)]) continue;
            bucket[Idx(degree[Idx(i)])].push_back(i);
            min_degree = std::min(min_degree, degree[Idx(i)]);
        }
        // MASS ELIMINATION: a variable with nothing outside L_p left to
        // connect to costs nothing to eliminate now.
        for (const int i : lp) {
            if (!alive[Idx(i)] || is_element[Idx(i)] || degree[Idx(i)] != 0) continue;
            order.push_back(i);
            eliminated += weight[Idx(i)];
            alive[Idx(i)] = 0;
            is_element[Idx(i)] = 0;
        }
    }

    // Any variable never picked -- one with no edges at all -- goes at the
    // end; the ordering has to be a permutation whatever the graph was.
    std::vector<char> placed(Idx(n), 0);
    std::vector<int> out;
    out.reserve(Idx(n));
    for (const int super : order) {
        for (const int member : members[Idx(super)]) {
            if (placed[Idx(member)]) continue;
            placed[Idx(member)] = 1;
            out.push_back(member);
        }
    }
    for (int i = 0; i < n; ++i) {
        for (const int member : members[Idx(i)]) {
            if (placed[Idx(member)]) continue;
            placed[Idx(member)] = 1;
            out.push_back(member);
        }
    }
    return out;
}

std::vector<int> MinimumDegree(const SparseMatrix &a) {
    const int n = a.Rows();
    const std::vector<std::vector<int>> initial = BuildAdjacency(a);
    // Sorted-unique neighbour sets, updated as elimination fills them in.
    std::vector<std::vector<int>> adj(Idx(n));
    for (int i = 0; i < n; ++i) {
        adj[Idx(i)] = initial[Idx(i)];
        std::sort(adj[Idx(i)].begin(), adj[Idx(i)].end());
        adj[Idx(i)].erase(std::unique(adj[Idx(i)].begin(), adj[Idx(i)].end()), adj[Idx(i)].end());
    }
    std::vector<int> eliminated(Idx(n), 0);
    std::vector<int> order;
    order.reserve(Idx(n));

    for (int step = 0; step < n; ++step) {
        // Linear scan for the minimum degree. A priority queue with lazy
        // deletion would cut this, but the quadratic term that actually
        // matters at scale is the clique fill below, and the real fix for
        // both is AMD's quotient graph (see the header).
        int pick = -1;
        std::size_t best = static_cast<std::size_t>(-1);
        for (int i = 0; i < n; ++i) {
            if (eliminated[Idx(i)]) continue;
            if (adj[Idx(i)].size() < best) {
                best = adj[Idx(i)].size();
                pick = i;
            }
        }
        if (pick < 0) break;
        order.push_back(pick);
        eliminated[Idx(pick)] = 1;

        // The eliminated node's remaining neighbours become a clique:
        // eliminating it creates an edge between every pair of them,
        // which is precisely the fill a factorization would produce.
        std::vector<int> nbrs;
        for (int v : adj[Idx(pick)]) {
            if (!eliminated[Idx(v)]) nbrs.push_back(v);
        }
        for (std::size_t i = 0; i < nbrs.size(); ++i) {
            std::vector<int> &list = adj[Idx(nbrs[i])];
            // Drop the eliminated node from each neighbour's list.
            list.erase(std::remove(list.begin(), list.end(), pick), list.end());
            for (std::size_t j = 0; j < nbrs.size(); ++j) {
                if (i == j) continue;
                const int other = nbrs[j];
                const auto pos = std::lower_bound(list.begin(), list.end(), other);
                if (pos == list.end() || *pos != other) list.insert(pos, other);
            }
        }
        adj[Idx(pick)].clear();
    }
    return order;
}

}  // namespace

std::vector<int> ComputeOrdering(const SparseMatrix &a, Ordering ordering) {
    const int n = a.Rows();
    if (n <= 0) return {};
    switch (ordering) {
        case Ordering::Natural: {
            std::vector<int> perm(Idx(n));
            std::iota(perm.begin(), perm.end(), 0);
            return perm;
        }
        case Ordering::ReverseCuthillMcKee:
            return CuthillMcKee(a);
        case Ordering::MinimumDegree:
            return MinimumDegree(a);
        case Ordering::ApproximateMinimumDegree:
            return ApproximateMinimumDegree(a);
    }
    std::vector<int> perm(Idx(n));
    std::iota(perm.begin(), perm.end(), 0);
    return perm;
}

// ---------------------------------------------------------------------
// SparseLDLT
// ---------------------------------------------------------------------

bool SparseLDLT::Factorize(const SparseMatrix &a, Ordering ordering, double singular_tolerance) {
    factorized_ = false;
    error_.clear();
    if (a.Rows() != a.Cols() || a.Rows() == 0) {
        error_ = "matrix is not square, or is empty";
        return false;
    }
    n_ = a.Rows();
    perm_ = ComputeOrdering(a, ordering);
    if (static_cast<int>(perm_.size()) != n_) {
        error_ = "ordering did not cover every row (disconnected or malformed graph)";
        return false;
    }
    inv_perm_ = InvertPermutation(perm_);
    const SparseMatrix pa = a.Permuted(perm_);

    // Reference scale for the singularity test below. The largest
    // diagonal entry, not a norm: it is what a pivot is directly
    // comparable against, and it costs one pass.
    double diagonal_scale = 0.0;
    for (int i = 0; i < n_; ++i) diagonal_scale = std::max(diagonal_scale, std::fabs(a.At(i, i)));
    if (diagonal_scale == 0.0) diagonal_scale = 1.0;
    const double pivot_floor = singular_tolerance * diagonal_scale;

    // Davis's LDL, in two passes over the lower triangle of the permuted
    // matrix. The symbolic pass builds the elimination tree and counts
    // each column of L; the numeric pass fills it in.
    //
    // The elimination tree is the structure that makes this work at all:
    // parent_[i] is the row of the first off-diagonal nonzero in column i
    // of L, and the nonzero pattern of row k of L is exactly the set of
    // nodes on the tree paths from k's own nonzeros up to the root. That
    // is what lets the numeric pass touch only the entries that will be
    // nonzero, instead of scanning the whole triangle.
    const std::vector<int> &rs = pa.RowStart();
    const std::vector<int> &ci = pa.ColIndex();
    const std::vector<double> &vals = pa.Values();

    parent_.assign(Idx(n_), -1);
    std::vector<int> flag(Idx(n_), -1);
    std::vector<int> column_count(Idx(n_), 0);
    for (int k = 0; k < n_; ++k) {
        parent_[Idx(k)] = -1;
        flag[Idx(k)] = k;
        for (int p = rs[Idx(k)]; p < rs[Idx(k) + 1]; ++p) {
            int i = ci[Idx(p)];
            if (i >= k) continue;  // lower triangle only
            for (; flag[Idx(i)] != k; i = parent_[Idx(i)]) {
                if (parent_[Idx(i)] == -1) parent_[Idx(i)] = k;
                ++column_count[Idx(i)];
                flag[Idx(i)] = k;
            }
        }
    }

    l_start_.assign(Idx(n_) + 1, 0);
    for (int k = 0; k < n_; ++k) l_start_[Idx(k) + 1] = l_start_[Idx(k)] + column_count[Idx(k)];
    const int nnz = l_start_[Idx(n_)];
    l_index_.assign(Idx(nnz), 0);
    l_values_.assign(Idx(nnz), 0.0);
    d_.assign(Idx(n_), 0.0);

    std::vector<double> y(Idx(n_), 0.0);
    std::vector<int> pattern(Idx(n_), 0);
    std::vector<int> fill(Idx(n_), 0);  // entries written so far per column
    std::fill(flag.begin(), flag.end(), -1);

    for (int k = 0; k < n_; ++k) {
        y[Idx(k)] = 0.0;
        int top = n_;
        flag[Idx(k)] = k;
        fill[Idx(k)] = 0;
        for (int p = rs[Idx(k)]; p < rs[Idx(k) + 1]; ++p) {
            const int col = ci[Idx(p)];
            if (col > k) continue;
            y[Idx(col)] += vals[Idx(p)];
            int len = 0;
            for (int i = col; flag[Idx(i)] != k; i = parent_[Idx(i)]) {
                pattern[Idx(len++)] = i;
                flag[Idx(i)] = k;
            }
            // Reverse onto the top of the stack, so `pattern[top..n)` ends
            // up in increasing order -- which the elimination below needs,
            // since column i must be fully applied before any column that
            // depends on it.
            while (len > 0) pattern[Idx(--top)] = pattern[Idx(--len)];
        }
        d_[Idx(k)] = y[Idx(k)];
        y[Idx(k)] = 0.0;
        for (; top < n_; ++top) {
            const int i = pattern[Idx(top)];
            const double yi = y[Idx(i)];
            y[Idx(i)] = 0.0;
            for (int p = l_start_[Idx(i)]; p < l_start_[Idx(i)] + fill[Idx(i)]; ++p) {
                y[Idx(l_index_[Idx(p)])] -= l_values_[Idx(p)] * yi;
            }
            const double l_ki = yi / d_[Idx(i)];
            d_[Idx(k)] -= l_ki * yi;
            const int slot = l_start_[Idx(i)] + fill[Idx(i)];
            l_index_[Idx(slot)] = k;
            l_values_[Idx(slot)] = l_ki;
            ++fill[Idx(i)];
        }
        if (std::fabs(d_[Idx(k)]) <= pivot_floor) {
            error_ = "matrix is singular or near-singular: pivot " + std::to_string(d_[Idx(k)]) + " at row " +
                     std::to_string(perm_[Idx(k)]) + " is below " + std::to_string(pivot_floor) +
                     " (" + std::to_string(singular_tolerance) + " of the largest diagonal, " +
                     std::to_string(diagonal_scale) +
                     "); an unrestrained rigid-body mode is by far the usual cause";
            return false;
        }
    }
    factorized_ = true;
    return true;
}

bool SparseLDLT::Solve(const std::vector<double> &b, std::vector<double> *x) const {
    if (!factorized_ || static_cast<int>(b.size()) != n_) return false;

    // A spilled factor is read a column at a time. The two triangular
    // solves walk the columns in order, forwards then backwards, so this
    // is sequential reading in both directions and not random access to a
    // file, which is why spilling is worth doing at all.
    std::FILE *file = nullptr;
    long long value_base = 0;
    std::vector<int> index_buffer;
    std::vector<double> value_buffer;
    if (!spill_path_.empty()) {
        file = std::fopen(spill_path_.c_str(), "rb");
        if (file == nullptr) return false;
        const long long count = l_start_.empty() ? 0 : l_start_.back();
        value_base = count * static_cast<long long>(sizeof(int));
    }
    auto column = [&](int k, const int **index, const double **values, int *count) {
        const int begin = l_start_[Idx(k)];
        *count = l_start_[Idx(k) + 1] - begin;
        if (file == nullptr) {
            *index = l_index_.data() + begin;
            *values = l_values_.data() + begin;
            return true;
        }
        index_buffer.resize(Idx(*count));
        value_buffer.resize(Idx(*count));
        if (*count == 0) {
            *index = index_buffer.data();
            *values = value_buffer.data();
            return true;
        }
        if (std::fseek(file, static_cast<long>(static_cast<long long>(begin) *
                                              static_cast<long long>(sizeof(int))),
                       SEEK_SET) != 0) {
            return false;
        }
        if (std::fread(index_buffer.data(), sizeof(int), Idx(*count), file) != Idx(*count)) {
            return false;
        }
        if (std::fseek(file,
                       static_cast<long>(value_base + static_cast<long long>(begin) *
                                                          static_cast<long long>(sizeof(double))),
                       SEEK_SET) != 0) {
            return false;
        }
        if (std::fread(value_buffer.data(), sizeof(double), Idx(*count), file) != Idx(*count)) {
            return false;
        }
        *index = index_buffer.data();
        *values = value_buffer.data();
        return true;
    };

    std::vector<double> z(Idx(n_), 0.0);
    // Permute into the factorization's ordering.
    for (int i = 0; i < n_; ++i) z[Idx(i)] = b[Idx(perm_[Idx(i)])];
    // Forward solve L*z = Pb (L has an implicit unit diagonal).
    for (int k = 0; k < n_; ++k) {
        const int *index = nullptr;
        const double *values = nullptr;
        int count = 0;
        if (!column(k, &index, &values, &count)) {
            if (file != nullptr) std::fclose(file);
            return false;
        }
        const double zk = z[Idx(k)];
        for (int p = 0; p < count; ++p) z[Idx(index[p])] -= values[p] * zk;
    }
    // Diagonal.
    for (int k = 0; k < n_; ++k) z[Idx(k)] /= d_[Idx(k)];
    // Backward solve L^T*z = z.
    for (int k = n_; k-- > 0;) {
        const int *index = nullptr;
        const double *values = nullptr;
        int count = 0;
        if (!column(k, &index, &values, &count)) {
            if (file != nullptr) std::fclose(file);
            return false;
        }
        double sum = z[Idx(k)];
        for (int p = 0; p < count; ++p) sum -= values[p] * z[Idx(index[p])];
        z[Idx(k)] = sum;
    }
    if (file != nullptr) std::fclose(file);
    x->assign(Idx(n_), 0.0);
    for (int i = 0; i < n_; ++i) (*x)[Idx(perm_[Idx(i)])] = z[Idx(i)];
    return true;
}

long long SparseLDLT::BytesInMemory() const {
    return static_cast<long long>(l_index_.size()) * static_cast<long long>(sizeof(int)) +
           static_cast<long long>(l_values_.size()) * static_cast<long long>(sizeof(double));
}

bool SparseLDLT::SpillTo(const std::string &path, std::string *error) {
    error->clear();
    if (!factorized_) {
        *error = "there is no factor to spill";
        return false;
    }
    if (!spill_path_.empty()) {
        *error = "this factor is already spilled to " + spill_path_;
        return false;
    }
    // Indices in one block and values in the next, so that a column's
    // entries are contiguous within each and reading one costs two seeks
    // rather than one per entry.
    std::FILE *file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        *error = "cannot write the factor to " + path;
        return false;
    }
    bool ok = true;
    if (!l_index_.empty()) {
        ok = std::fwrite(l_index_.data(), sizeof(int), l_index_.size(), file) == l_index_.size();
    }
    if (ok && !l_values_.empty()) {
        ok = std::fwrite(l_values_.data(), sizeof(double), l_values_.size(), file) ==
             l_values_.size();
    }
    if (std::fclose(file) != 0) ok = false;
    if (!ok) {
        *error = "the factor could not be written to " + path + " in full";
        std::remove(path.c_str());
        return false;
    }
    spill_path_ = path;
    // Freed rather than merely cleared: a vector that has been cleared
    // still holds its allocation, which is the whole thing this is for.
    std::vector<int>().swap(l_index_);
    std::vector<double>().swap(l_values_);
    return true;
}

bool SparseLDLT::Reload(std::string *error) {
    error->clear();
    if (spill_path_.empty()) return true;
    const int count = l_start_.empty() ? 0 : l_start_.back();
    std::FILE *file = std::fopen(spill_path_.c_str(), "rb");
    if (file == nullptr) {
        *error = "the spilled factor " + spill_path_ + " cannot be read";
        return false;
    }
    std::vector<int> index(Idx(count));
    std::vector<double> values(Idx(count));
    bool ok = count == 0 ||
              (std::fread(index.data(), sizeof(int), Idx(count), file) == Idx(count) &&
               std::fread(values.data(), sizeof(double), Idx(count), file) == Idx(count));
    std::fclose(file);
    if (!ok) {
        *error = "the spilled factor " + spill_path_ + " is shorter than it should be";
        return false;
    }
    l_index_.swap(index);
    l_values_.swap(values);
    spill_path_.clear();
    return true;
}

int SparseLDLT::NegativeEigenvalueCount() const {
    int count = 0;
    for (double v : d_) {
        if (v < 0.0) ++count;
    }
    return count;
}

// ---------------------------------------------------------------------
// Iterative solvers
// ---------------------------------------------------------------------

double Dot(const std::vector<double> &a, const std::vector<double> &b) {
    double sum = 0.0;
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) sum += a[i] * b[i];
    return sum;
}

double Norm2(const std::vector<double> &v) { return std::sqrt(Dot(v, v)); }

bool IncompleteCholesky(const SparseMatrix &a, std::vector<int> *l_start, std::vector<int> *l_index,
                        std::vector<double> *l_values) {
    const int n = a.Rows();
    const std::vector<int> &rs = a.RowStart();
    const std::vector<int> &ci = a.ColIndex();
    const std::vector<double> &vals = a.Values();

    // Copy A's lower triangle, row by row -- IC(0) keeps exactly this
    // pattern and allows no fill, which is what makes it cheap and also
    // what makes it able to break down.
    l_start->assign(Idx(n) + 1, 0);
    l_index->clear();
    l_values->clear();
    for (int r = 0; r < n; ++r) {
        for (int p = rs[Idx(r)]; p < rs[Idx(r) + 1]; ++p) {
            if (ci[Idx(p)] <= r) {
                l_index->push_back(ci[Idx(p)]);
                l_values->push_back(vals[Idx(p)]);
            }
        }
        (*l_start)[Idx(r) + 1] = static_cast<int>(l_index->size());
    }

    for (int r = 0; r < n; ++r) {
        for (int p = (*l_start)[Idx(r)]; p < (*l_start)[Idx(r) + 1]; ++p) {
            const int c = (*l_index)[Idx(p)];
            double sum = (*l_values)[Idx(p)];
            // sum -= dot of row r and row c over their common columns
            // below c. Both rows are sorted, so this is a merge.
            int pr = (*l_start)[Idx(r)];
            int pc = (*l_start)[Idx(c)];
            while (pr < p && pc < (*l_start)[Idx(c) + 1]) {
                const int kr = (*l_index)[Idx(pr)];
                const int kc = (*l_index)[Idx(pc)];
                if (kc >= c) break;
                if (kr < kc) {
                    ++pr;
                } else if (kc < kr) {
                    ++pc;
                } else {
                    sum -= (*l_values)[Idx(pr)] * (*l_values)[Idx(pc)];
                    ++pr;
                    ++pc;
                }
            }
            if (c == r) {
                if (sum <= 0.0) return false;  // breakdown; caller falls back
                (*l_values)[Idx(p)] = std::sqrt(sum);
            } else {
                // Diagonal of row c is its last entry, by construction.
                const double diag = (*l_values)[Idx((*l_start)[Idx(c) + 1] - 1)];
                if (diag == 0.0) return false;
                (*l_values)[Idx(p)] = sum / diag;
            }
        }
    }
    return true;
}


// ---------------------------------------------------------------------
// Smoothed-aggregation algebraic multigrid (Part H.5)
// ---------------------------------------------------------------------

SparseMatrix Multiply(const SparseMatrix &a, const SparseMatrix &b) {
    const int rows = a.Rows();
    const int cols = b.Cols();
    std::vector<Triplet> triplets;
    // One row of the product at a time, accumulated in a dense scratch
    // row with a list of which entries were touched -- the standard
    // sparse-times-sparse inner loop, and the reason it is linear in the
    // work done rather than in the width of the result.
    std::vector<double> accumulator(Idx(cols), 0.0);
    std::vector<int> touched;
    std::vector<char> in_use(Idx(cols), 0);
    for (int i = 0; i < rows; ++i) {
        touched.clear();
        for (int pa = a.RowStart()[Idx(i)]; pa < a.RowStart()[Idx(i) + 1]; ++pa) {
            const int k = a.ColIndex()[Idx(pa)];
            const double av = a.Values()[Idx(pa)];
            if (av == 0.0) continue;
            for (int pb = b.RowStart()[Idx(k)]; pb < b.RowStart()[Idx(k) + 1]; ++pb) {
                const int j = b.ColIndex()[Idx(pb)];
                if (!in_use[Idx(j)]) {
                    in_use[Idx(j)] = 1;
                    accumulator[Idx(j)] = 0.0;
                    touched.push_back(j);
                }
                accumulator[Idx(j)] += av * b.Values()[Idx(pb)];
            }
        }
        std::sort(touched.begin(), touched.end());
        for (const int j : touched) {
            if (accumulator[Idx(j)] != 0.0) triplets.push_back(Triplet{i, j, accumulator[Idx(j)]});
            in_use[Idx(j)] = 0;
        }
    }
    return SparseMatrix::FromTriplets(rows, cols, triplets);
}

namespace {

// Greedy aggregation along strong connections. Returns one aggregate
// index per row, and the number of aggregates.
//
// Two passes, and the second one matters more than it looks. The first
// grows an aggregate around any node none of whose strong neighbours is
// taken yet, which covers most of the graph in well-shaped clumps. The
// second hands every node the first pass left over to whichever
// neighbouring aggregate it is strongly connected to. Without it, leftover
// nodes become aggregates of one, and an aggregate of one is a coarse
// unknown that represents nothing -- the hierarchy stops coarsening and
// the cycle cost goes up without the convergence improving.
int Aggregate(const SparseMatrix &a, double threshold, std::vector<int> *out) {
    const int n = a.Rows();
    out->assign(Idx(n), -1);
    // STRENGTH IS MEASURED AGAINST THE ROW'S OWN LARGEST OFF-DIAGONAL,
    // not against sqrt(a_ii * a_jj). The latter is the textbook form and
    // it works on the finest level, where the operator is the one the
    // discretisation produced; it fails on the coarse levels, where the
    // Galerkin product has spread each row over many more columns and
    // scaled the whole thing differently. Measured that way the coarse
    // rows had almost no strong connections at all, so almost every node
    // became an aggregate of one and the hierarchy stopped coarsening:
    // 4096 unknowns went to 514 and then to 396, which costs a level and
    // buys nothing. Against the row's own maximum the test is free of
    // both scalings and behaves the same at every level.
    std::vector<double> strongest_in_row(Idx(n), 0.0);
    for (int i = 0; i < n; ++i) {
        for (int p = a.RowStart()[Idx(i)]; p < a.RowStart()[Idx(i) + 1]; ++p) {
            if (a.ColIndex()[Idx(p)] == i) continue;
            strongest_in_row[Idx(i)] =
                std::max(strongest_in_row[Idx(i)], std::fabs(a.Values()[Idx(p)]));
        }
    }

    auto strong = [&](int i, int j, double value) {
        if (i == j) return false;
        if (!(strongest_in_row[Idx(i)] > 0.0)) return false;
        return std::fabs(value) >= threshold * strongest_in_row[Idx(i)];
    };

    int aggregates = 0;
    for (int i = 0; i < n; ++i) {
        if ((*out)[Idx(i)] >= 0) continue;
        bool clear = true;
        for (int p = a.RowStart()[Idx(i)]; p < a.RowStart()[Idx(i) + 1] && clear; ++p) {
            const int j = a.ColIndex()[Idx(p)];
            if (!strong(i, j, a.Values()[Idx(p)])) continue;
            if ((*out)[Idx(j)] >= 0) clear = false;
        }
        if (!clear) continue;
        const int label = aggregates++;
        (*out)[Idx(i)] = label;
        for (int p = a.RowStart()[Idx(i)]; p < a.RowStart()[Idx(i) + 1]; ++p) {
            const int j = a.ColIndex()[Idx(p)];
            if (!strong(i, j, a.Values()[Idx(p)])) continue;
            if ((*out)[Idx(j)] < 0) (*out)[Idx(j)] = label;
        }
    }
    for (int i = 0; i < n; ++i) {
        if ((*out)[Idx(i)] >= 0) continue;
        int best = -1;
        double strongest = 0.0;
        for (int p = a.RowStart()[Idx(i)]; p < a.RowStart()[Idx(i) + 1]; ++p) {
            const int j = a.ColIndex()[Idx(p)];
            if ((*out)[Idx(j)] < 0 || !strong(i, j, a.Values()[Idx(p)])) continue;
            const double value = std::fabs(a.Values()[Idx(p)]);
            if (value <= strongest) continue;
            strongest = value;
            best = (*out)[Idx(j)];
        }
        if (best < 0) {
            // No *strong* neighbour in an aggregate. A weak one is still
            // better than standing alone: a singleton aggregate is a
            // coarse unknown that represents one fine unknown, which adds
            // a row to every level below it and improves nothing.
            for (int p = a.RowStart()[Idx(i)]; p < a.RowStart()[Idx(i) + 1]; ++p) {
                const int j = a.ColIndex()[Idx(p)];
                if (j == i || (*out)[Idx(j)] < 0) continue;
                const double value = std::fabs(a.Values()[Idx(p)]);
                if (value <= strongest) continue;
                strongest = value;
                best = (*out)[Idx(j)];
            }
        }
        // Genuinely isolated -- no neighbour in any aggregate -- and there
        // is nothing to do but give it one of its own.
        (*out)[Idx(i)] = best >= 0 ? best : aggregates++;
    }
    return aggregates;
}

// The node graph of a matrix whose unknowns come in blocks: one vertex
// per node, with the weight between two of them the largest entry of the
// block that couples them. Aggregating this rather than the unknown graph
// is what keeps a node's unknowns together.
SparseMatrix BlockNorms(const SparseMatrix &a, int block) {
    const int nodes = a.Rows() / block;
    std::vector<Triplet> triplets;
    std::vector<double> row(Idx(nodes), 0.0);
    std::vector<int> touched;
    std::vector<char> in_use(Idx(nodes), 0);
    for (int node = 0; node < nodes; ++node) {
        touched.clear();
        for (int sub = 0; sub < block; ++sub) {
            const int i = node * block + sub;
            for (int p = a.RowStart()[Idx(i)]; p < a.RowStart()[Idx(i) + 1]; ++p) {
                const int other = a.ColIndex()[Idx(p)] / block;
                if (!in_use[Idx(other)]) {
                    in_use[Idx(other)] = 1;
                    row[Idx(other)] = 0.0;
                    touched.push_back(other);
                }
                row[Idx(other)] = std::max(row[Idx(other)], std::fabs(a.Values()[Idx(p)]));
            }
        }
        std::sort(touched.begin(), touched.end());
        for (const int other : touched) {
            if (row[Idx(other)] != 0.0) triplets.push_back(Triplet{node, other, row[Idx(other)]});
            in_use[Idx(other)] = 0;
        }
    }
    return SparseMatrix::FromTriplets(nodes, nodes, triplets);
}

// An estimate of the largest eigenvalue of D^-1 A, by a few steps of the
// power method. Used to damp the smoother: Jacobi converges for a damping
// below 2/rho, and 4/(3 rho) is the value that damps the top two thirds
// of the spectrum, which is what a smoother is for.
double SpectralRadiusEstimate(const SparseMatrix &a, const std::vector<double> &inverse_diagonal) {
    const int n = a.Rows();
    std::vector<double> x(Idx(n), 0.0);
    for (int i = 0; i < n; ++i) {
        // A deterministic start that is not the constant vector, which
        // for a Neumann-like operator is very nearly an eigenvector of
        // the *smallest* eigenvalue and would make the power method crawl.
        x[Idx(i)] = 1.0 + 0.5 * std::sin(static_cast<double>(i) * 0.7);
    }
    double value = 0.0;
    std::vector<double> y;
    for (int step = 0; step < 12; ++step) {
        a.Multiply(x, &y);
        for (int i = 0; i < n; ++i) y[Idx(i)] *= inverse_diagonal[Idx(i)];
        double norm = 0.0;
        for (const double entry : y) norm += entry * entry;
        norm = std::sqrt(norm);
        if (!(norm > 0.0)) return 1.0;
        value = norm;
        for (int i = 0; i < n; ++i) x[Idx(i)] = y[Idx(i)] / norm;
    }
    // Rounded up: an underestimate makes the smoother diverge, an
    // overestimate only makes it slower.
    return value > 0.0 ? value * 1.05 : 1.0;
}

}  // namespace

int AlgebraicMultigridPreconditioner::RowsAtLevel(int level) const {
    if (level < 0 || level >= Levels()) return 0;
    return levels_[Idx(level)].a.Rows();
}

int AlgebraicMultigridPreconditioner::NonZerosAtLevel(int level) const {
    if (level < 0 || level >= Levels()) return 0;
    return levels_[Idx(level)].a.NonZeros();
}

double AlgebraicMultigridPreconditioner::OperatorComplexity() const {
    if (levels_.empty() || levels_.front().a.NonZeros() == 0) return 0.0;
    long long total = 0;
    for (const Level &level : levels_) total += level.a.NonZeros();
    return static_cast<double>(total) / static_cast<double>(levels_.front().a.NonZeros());
}

bool AlgebraicMultigridPreconditioner::Build(const SparseMatrix &a, const MultigridOptions &options,
                                             std::string *error) {
    levels_.clear();
    error->clear();
    if (a.Rows() != a.Cols() || a.Rows() == 0) {
        *error = "the matrix is not square, or is empty";
        return false;
    }
    sweeps_ = std::max(1, options.smoothing_sweeps);

    // The near-null space on the finest level. One constant vector unless
    // the caller knows better, and it must: see the header.
    std::vector<std::vector<double>> candidates = options.near_null_space;
    if (candidates.empty()) candidates.push_back(std::vector<double>(Idx(a.Rows()), 1.0));
    for (const std::vector<double> &candidate : candidates) {
        if (static_cast<int>(candidate.size()) == a.Rows()) continue;
        *error = "a near-null-space vector does not have one entry per unknown";
        return false;
    }

    SparseMatrix current = a;
    int block = std::max(1, options.block_size);
    for (int level = 0; level < std::max(1, options.max_levels); ++level) {
        Level built;
        built.a = current;
        const int n = current.Rows();
        built.inverse_diagonal.assign(Idx(n), 1.0);
        for (int i = 0; i < n; ++i) {
            const double d = current.At(i, i);
            built.inverse_diagonal[Idx(i)] = (d != 0.0) ? 1.0 / d : 1.0;
        }
        const double radius = SpectralRadiusEstimate(current, built.inverse_diagonal);
        built.damping = options.damping > 0.0 ? options.damping : 1.0 / radius;

        if (n <= std::max(1, options.coarse_limit) || level + 1 >= std::max(1, options.max_levels)) {
            levels_.push_back(built);
            break;
        }

        std::vector<int> aggregate;
        int coarse = 0;
        if (block > 1 && n % block == 0 && n / block > 1) {
            const SparseMatrix nodes = BlockNorms(current, block);
            std::vector<int> per_node;
            coarse = Aggregate(nodes, options.strength_threshold, &per_node);
            aggregate.assign(Idx(n), 0);
            for (int i = 0; i < n; ++i) aggregate[Idx(i)] = per_node[Idx(i / block)];
        } else {
            coarse = Aggregate(current, options.strength_threshold, &aggregate);
        }
        // No coarsening means the hierarchy has gone as far as it can;
        // another level of the same size would cost work and buy nothing.
        if (coarse >= n) {
            levels_.push_back(built);
            break;
        }

        // The tentative prolongator: one column per aggregate per
        // near-null-space vector, with each aggregate's columns
        // orthonormalised so that P^T P is the identity. Without that
        // normalisation the coarse operator is scaled arbitrarily by
        // aggregate size and the smoother's damping stops meaning
        // anything on the coarse level.
        //
        // COLUMNS THAT COME OUT EMPTY ARE DROPPED, NOT KEPT. An aggregate
        // of three rows cannot carry six independent candidates, so
        // Gram-Schmidt annihilates the later ones -- and a zero column of
        // P is a zero row and column of P^T A P, which is a singular
        // coarse operator. The hierarchy then fails to build and the
        // solver quietly falls back to Jacobi, which is a multigrid method
        // that is not one. Numbering the columns as they are filled rather
        // than by aggregate is all it takes.
        const int per_aggregate = static_cast<int>(candidates.size());
        std::vector<std::vector<int>> members(Idx(coarse));
        for (int i = 0; i < n; ++i) members[Idx(aggregate[Idx(i)])].push_back(i);
        std::vector<Triplet> tentative;
        std::vector<std::vector<double>> coarse_candidates(Idx(per_aggregate));
        int coarse_columns = 0;
        for (int group = 0; group < coarse; ++group) {
            std::vector<std::vector<double>> local(Idx(per_aggregate));
            for (int c = 0; c < per_aggregate; ++c) {
                local[Idx(c)].resize(members[Idx(group)].size());
                for (std::size_t r = 0; r < members[Idx(group)].size(); ++r) {
                    local[Idx(c)][r] = candidates[Idx(c)][Idx(members[Idx(group)][r])];
                }
                for (int earlier = 0; earlier < c; ++earlier) {
                    if (local[Idx(earlier)].empty()) continue;
                    double dot = 0.0;
                    for (std::size_t r = 0; r < local[Idx(c)].size(); ++r) {
                        dot += local[Idx(c)][r] * local[Idx(earlier)][r];
                    }
                    for (std::size_t r = 0; r < local[Idx(c)].size(); ++r) {
                        local[Idx(c)][r] -= dot * local[Idx(earlier)][r];
                    }
                }
                double norm = 0.0;
                for (const double entry : local[Idx(c)]) norm += entry * entry;
                norm = std::sqrt(norm);
                // Relative to the candidate's own size, so that a genuinely
                // dependent direction is dropped and a merely small one is
                // not.
                double original = 0.0;
                for (std::size_t r = 0; r < members[Idx(group)].size(); ++r) {
                    const double entry = candidates[Idx(c)][Idx(members[Idx(group)][r])];
                    original += entry * entry;
                }
                original = std::sqrt(original);
                if (!(norm > std::max(1e-300, original * 1e-8))) {
                    local[Idx(c)].clear();
                    continue;
                }
                const int column = coarse_columns++;
                for (std::size_t r = 0; r < local[Idx(c)].size(); ++r) {
                    local[Idx(c)][r] /= norm;
                    tentative.push_back(
                        Triplet{members[Idx(group)][r], column, local[Idx(c)][r]});
                }
                for (int which = 0; which < per_aggregate; ++which) {
                    coarse_candidates[Idx(which)].resize(Idx(coarse_columns), 0.0);
                }
                // The coarse representation of this candidate: the norm
                // that was divided out, so that P applied to it reproduces
                // the fine one and the near-null space survives the level.
                coarse_candidates[Idx(c)][Idx(column)] = norm;
            }
        }
        // AND THE COARSE LEVEL HAS TO BE SMALLER. With several
        // near-null-space vectors a coarse level carries that many columns
        // per aggregate, so it can be *larger* than the level above it --
        // six rigid-body modes over aggregates of four rows is a hierarchy
        // that grows instead of coarsening, level after level, until a
        // vector allocation fails. The guard has to be on the column count
        // and not on the aggregate count.
        if (coarse_columns >= n * 3 / 4) {
            levels_.push_back(built);
            break;
        }
        for (std::vector<double> &candidate : coarse_candidates) {
            candidate.resize(Idx(coarse_columns), 0.0);
        }
        const SparseMatrix tentative_p =
            SparseMatrix::FromTriplets(n, coarse_columns, tentative);

        // SMOOTHED aggregation: P = (I - damping * D^-1 A) * P0. The
        // tentative prolongator is piecewise constant, so it interpolates
        // with a jump at every aggregate boundary; one pass of the
        // smoother spreads it out, and that is the whole difference
        // between this and plain aggregation, which does not converge
        // independently of the mesh.
        SparseMatrix smoothed_p;
        {
            const SparseMatrix ap = Multiply(current, tentative_p);
            std::vector<Triplet> triplets;
            for (int i = 0; i < n; ++i) {
                const double scale = built.damping * built.inverse_diagonal[Idx(i)];
                for (int p = tentative_p.RowStart()[Idx(i)]; p < tentative_p.RowStart()[Idx(i) + 1];
                     ++p) {
                    triplets.push_back(Triplet{i, tentative_p.ColIndex()[Idx(p)],
                                               tentative_p.Values()[Idx(p)]});
                }
                for (int p = ap.RowStart()[Idx(i)]; p < ap.RowStart()[Idx(i) + 1]; ++p) {
                    triplets.push_back(
                        Triplet{i, ap.ColIndex()[Idx(p)], -scale * ap.Values()[Idx(p)]});
                }
            }
            smoothed_p = SparseMatrix::FromTriplets(n, coarse_columns, triplets);
        }

        built.p = smoothed_p;
        levels_.push_back(built);
        // The Galerkin coarse operator. Symmetric because A is and P is
        // the same on both sides, which is what keeps the whole cycle
        // usable as a conjugate-gradient preconditioner.
        current = Multiply(smoothed_p.Transposed(), Multiply(current, smoothed_p));
        candidates = coarse_candidates;
        // The next level's unknowns come in blocks of however many
        // candidates every aggregate managed to keep. When they did not
        // all keep the same number there is no block structure to speak
        // of and the unknown graph is aggregated instead.
        block = coarse_columns == coarse * per_aggregate ? per_aggregate : 1;
    }

    // The coarsest level is factored, not smoothed: a V-cycle that only
    // ever smooths never removes the error it coarsened for.
    if (!coarse_.Factorize(levels_.back().a, Ordering::ApproximateMinimumDegree, 1e-15)) {
        // A coarse level that is singular is the usual outcome of a
        // matrix with a genuine null space -- an unconstrained model --
        // and the honest response is to say so here rather than to return
        // a preconditioner that produces infinities.
        *error = "the coarsest level of the hierarchy is singular (" + coarse_.Error() + ")";
        return false;
    }
    return true;
}

void AlgebraicMultigridPreconditioner::Apply(const std::vector<double> &r,
                                             std::vector<double> *z) const {
    z->assign(r.size(), 0.0);
    if (levels_.empty()) {
        *z = r;
        return;
    }
    // Iterative, not recursive, so that the stack depth does not depend
    // on how deep the hierarchy went.
    const int deepest = Levels() - 1;
    std::vector<std::vector<double>> residual(Idx(Levels()));
    std::vector<std::vector<double>> correction(Idx(Levels()));
    residual[0] = r;

    for (int level = 0; level < deepest; ++level) {
        const Level &at = levels_[Idx(level)];
        const int n = at.a.Rows();
        correction[Idx(level)].assign(Idx(n), 0.0);
        // Pre-smooth: damped Jacobi, which is symmetric, which is what
        // lets the whole cycle be used with conjugate gradients. A
        // Gauss-Seidel sweep would smooth better and is not symmetric
        // unless it is paired with its own reverse.
        std::vector<double> work;
        for (int sweep = 0; sweep < sweeps_; ++sweep) {
            at.a.Multiply(correction[Idx(level)], &work);
            for (int i = 0; i < n; ++i) {
                correction[Idx(level)][Idx(i)] += at.damping * at.inverse_diagonal[Idx(i)] *
                                                 (residual[Idx(level)][Idx(i)] - work[Idx(i)]);
            }
        }
        at.a.Multiply(correction[Idx(level)], &work);
        std::vector<double> fine(Idx(n), 0.0);
        for (int i = 0; i < n; ++i) fine[Idx(i)] = residual[Idx(level)][Idx(i)] - work[Idx(i)];
        at.p.MultiplyTransposed(fine, &residual[Idx(level + 1)]);
    }

    std::vector<double> coarse_solution;
    if (!coarse_.Solve(residual[Idx(deepest)], &coarse_solution)) {
        coarse_solution.assign(residual[Idx(deepest)].size(), 0.0);
    }
    correction[Idx(deepest)] = coarse_solution;

    for (int level = deepest; level-- > 0;) {
        const Level &at = levels_[Idx(level)];
        const int n = at.a.Rows();
        std::vector<double> prolonged;
        at.p.Multiply(correction[Idx(level + 1)], &prolonged);
        for (int i = 0; i < n; ++i) correction[Idx(level)][Idx(i)] += prolonged[Idx(i)];
        // Post-smooth, the same number of sweeps as before, which is what
        // makes the operator symmetric rather than merely nearly so.
        std::vector<double> work;
        for (int sweep = 0; sweep < sweeps_; ++sweep) {
            at.a.Multiply(correction[Idx(level)], &work);
            for (int i = 0; i < n; ++i) {
                correction[Idx(level)][Idx(i)] += at.damping * at.inverse_diagonal[Idx(i)] *
                                                 (residual[Idx(level)][Idx(i)] - work[Idx(i)]);
            }
        }
    }
    *z = correction[0];
}

namespace {

// Applies a preconditioner built once by MakePreconditioner.
class PreconditionerApply {
public:
    void SetMultigridOptions(const MultigridOptions &options) { options_ = options; }

    bool Build(const SparseMatrix &a, Preconditioner kind) {
        kind_ = kind;
        const int n = a.Rows();
        if (kind_ == Preconditioner::None) return true;
        if (kind_ == Preconditioner::Jacobi) {
            inv_diag_.assign(Idx(n), 1.0);
            for (int i = 0; i < n; ++i) {
                const double d = a.At(i, i);
                // A zero diagonal makes Jacobi undefined; falling back to
                // 1 for that row leaves the row unscaled rather than
                // producing infinities, and CG still converges.
                inv_diag_[Idx(i)] = (d != 0.0) ? 1.0 / d : 1.0;
            }
            return true;
        }
        if (kind_ == Preconditioner::AlgebraicMultigrid) {
            std::string error;
            if (multigrid_.Build(a, options_, &error)) {
                n_ = n;
                return true;
            }
            degraded_ = true;
            kind_ = Preconditioner::Jacobi;
            return Build(a, Preconditioner::Jacobi);
        }
        if (!IncompleteCholesky(a, &l_start_, &l_index_, &l_values_)) {
            // Breakdown is expected often enough that it is not an error:
            // fall back to Jacobi and say so through Degraded().
            degraded_ = true;
            kind_ = Preconditioner::Jacobi;
            return Build(a, Preconditioner::Jacobi);
        }
        n_ = n;
        return true;
    }

    bool Degraded() const { return degraded_; }

    void Apply(const std::vector<double> &r, std::vector<double> *z) const {
        if (kind_ == Preconditioner::None) {
            *z = r;
            return;
        }
        if (kind_ == Preconditioner::Jacobi) {
            z->assign(r.size(), 0.0);
            for (std::size_t i = 0; i < r.size(); ++i) (*z)[i] = r[i] * inv_diag_[i];
            return;
        }
        if (kind_ == Preconditioner::AlgebraicMultigrid) {
            multigrid_.Apply(r, z);
            return;
        }
        // Solve L*L^T*z = r with the incomplete factor.
        *z = r;
        for (int i = 0; i < n_; ++i) {
            double sum = (*z)[Idx(i)];
            for (int p = l_start_[Idx(i)]; p < l_start_[Idx(i) + 1] - 1; ++p) {
                sum -= l_values_[Idx(p)] * (*z)[Idx(l_index_[Idx(p)])];
            }
            (*z)[Idx(i)] = sum / l_values_[Idx(l_start_[Idx(i) + 1] - 1)];
        }
        for (int i = n_; i-- > 0;) {
            (*z)[Idx(i)] /= l_values_[Idx(l_start_[Idx(i) + 1] - 1)];
            const double zi = (*z)[Idx(i)];
            for (int p = l_start_[Idx(i)]; p < l_start_[Idx(i) + 1] - 1; ++p) {
                (*z)[Idx(l_index_[Idx(p)])] -= l_values_[Idx(p)] * zi;
            }
        }
    }

private:
    Preconditioner kind_ = Preconditioner::None;
    bool degraded_ = false;
    int n_ = 0;
    MultigridOptions options_;
    AlgebraicMultigridPreconditioner multigrid_;
    std::vector<double> inv_diag_;
    std::vector<int> l_start_;
    std::vector<int> l_index_;
    std::vector<double> l_values_;
};

}  // namespace

IterativeResult ConjugateGradient(const SparseMatrix &a, const std::vector<double> &b, std::vector<double> *x,
                                  const IterativeOptions &opt) {
    IterativeResult result;
    const int n = a.Rows();
    if (a.Cols() != n || static_cast<int>(b.size()) != n) {
        result.message = "dimension mismatch";
        return result;
    }
    if (x->size() != Idx(n)) x->assign(Idx(n), 0.0);

    PreconditionerApply precond;
    precond.SetMultigridOptions(opt.multigrid);
    precond.Build(a, opt.preconditioner);
    if (precond.Degraded()) {
        result.message = opt.preconditioner == Preconditioner::AlgebraicMultigrid
                             ? "the multigrid hierarchy could not be built; fell back to Jacobi"
                             : "incomplete Cholesky broke down; fell back to Jacobi";
    }

    const double b_norm = Norm2(b);
    if (b_norm == 0.0) {
        // A zero right-hand side has the zero solution, and the relative
        // residual test would divide by zero.
        x->assign(Idx(n), 0.0);
        result.converged = true;
        return result;
    }

    std::vector<double> ax;
    a.Multiply(*x, &ax);
    std::vector<double> r(Idx(n), 0.0);
    for (int i = 0; i < n; ++i) r[Idx(i)] = b[Idx(i)] - ax[Idx(i)];

    std::vector<double> z;
    precond.Apply(r, &z);
    std::vector<double> p = z;
    double rz = Dot(r, z);
    std::vector<double> ap;

    for (int it = 0; it < opt.max_iterations; ++it) {
        result.iterations = it + 1;
        a.Multiply(p, &ap);
        const double p_ap = Dot(p, ap);
        if (p_ap <= 0.0) {
            // A non-positive curvature direction means A is not positive
            // definite, which CG cannot handle. Saying so beats returning
            // a diverging iterate.
            result.message = "matrix is not positive definite (non-positive curvature in CG)";
            result.relative_residual = Norm2(r) / b_norm;
            return result;
        }
        const double alpha = rz / p_ap;
        for (int i = 0; i < n; ++i) {
            (*x)[Idx(i)] += alpha * p[Idx(i)];
            r[Idx(i)] -= alpha * ap[Idx(i)];
        }
        const double residual = Norm2(r) / b_norm;
        result.relative_residual = residual;
        if (residual <= opt.tolerance) {
            result.converged = true;
            return result;
        }
        precond.Apply(r, &z);
        const double rz_next = Dot(r, z);
        const double beta = rz_next / rz;
        rz = rz_next;
        for (int i = 0; i < n; ++i) p[Idx(i)] = z[Idx(i)] + beta * p[Idx(i)];
    }
    if (result.message.empty()) result.message = "reached the iteration limit without converging";
    return result;
}

IterativeResult Gmres(const SparseMatrix &a, const std::vector<double> &b, std::vector<double> *x,
                      const IterativeOptions &opt) {
    IterativeResult result;
    const int n = a.Rows();
    if (a.Cols() != n || static_cast<int>(b.size()) != n) {
        result.message = "dimension mismatch";
        return result;
    }
    if (x->size() != Idx(n)) x->assign(Idx(n), 0.0);
    const int m = std::max(1, opt.restart);

    PreconditionerApply precond;
    // Left preconditioning with IC(0) assumes symmetry, which GMRES's
    // whole reason for existing says we do not have. Jacobi is the only
    // one of the three that is meaningful for a general matrix, so an
    // IncompleteCholesky request is quietly treated as Jacobi here.
    precond.Build(a, opt.preconditioner == Preconditioner::None ? Preconditioner::None : Preconditioner::Jacobi);

    const double b_norm = Norm2(b);
    if (b_norm == 0.0) {
        x->assign(Idx(n), 0.0);
        result.converged = true;
        return result;
    }

    std::vector<std::vector<double>> basis;
    std::vector<double> hessenberg;  // (m+1) x m, column major
    std::vector<double> cosines(Idx(m), 0.0);
    std::vector<double> sines(Idx(m), 0.0);
    std::vector<double> g(Idx(m) + 1, 0.0);

    int total_iterations = 0;
    while (total_iterations < opt.max_iterations) {
        std::vector<double> ax;
        a.Multiply(*x, &ax);
        std::vector<double> r(Idx(n), 0.0);
        for (int i = 0; i < n; ++i) r[Idx(i)] = b[Idx(i)] - ax[Idx(i)];
        std::vector<double> z;
        precond.Apply(r, &z);
        double beta = Norm2(z);
        if (beta == 0.0) {
            result.converged = true;
            result.relative_residual = 0.0;
            return result;
        }
        basis.assign(1, z);
        for (int i = 0; i < n; ++i) basis[0][Idx(i)] /= beta;
        hessenberg.assign(Idx((m + 1) * m), 0.0);
        std::fill(g.begin(), g.end(), 0.0);
        g[0] = beta;

        int k = 0;
        for (; k < m && total_iterations < opt.max_iterations; ++k, ++total_iterations) {
            result.iterations = total_iterations + 1;
            std::vector<double> w_raw;
            a.Multiply(basis[Idx(k)], &w_raw);
            std::vector<double> w;
            precond.Apply(w_raw, &w);
            // Modified Gram-Schmidt: numerically far better behaved than
            // the classical form, which loses orthogonality exactly when
            // the Krylov basis becomes interesting.
            for (int j = 0; j <= k; ++j) {
                const double h = Dot(w, basis[Idx(j)]);
                hessenberg[Idx(j + k * (m + 1))] = h;
                for (int i = 0; i < n; ++i) w[Idx(i)] -= h * basis[Idx(j)][Idx(i)];
            }
            const double h_next = Norm2(w);
            hessenberg[Idx(k + 1 + k * (m + 1))] = h_next;

            // Apply the previous Givens rotations to the new column.
            for (int j = 0; j < k; ++j) {
                const double h1 = hessenberg[Idx(j + k * (m + 1))];
                const double h2 = hessenberg[Idx(j + 1 + k * (m + 1))];
                hessenberg[Idx(j + k * (m + 1))] = cosines[Idx(j)] * h1 + sines[Idx(j)] * h2;
                hessenberg[Idx(j + 1 + k * (m + 1))] = -sines[Idx(j)] * h1 + cosines[Idx(j)] * h2;
            }
            // Build and apply the new rotation, which zeroes the
            // subdiagonal and updates the residual estimate in g.
            const double hk = hessenberg[Idx(k + k * (m + 1))];
            const double denominator = std::sqrt(hk * hk + h_next * h_next);
            if (denominator == 0.0) {
                ++k;
                break;
            }
            cosines[Idx(k)] = hk / denominator;
            sines[Idx(k)] = h_next / denominator;
            hessenberg[Idx(k + k * (m + 1))] = denominator;
            hessenberg[Idx(k + 1 + k * (m + 1))] = 0.0;
            g[Idx(k + 1)] = -sines[Idx(k)] * g[Idx(k)];
            g[Idx(k)] = cosines[Idx(k)] * g[Idx(k)];

            result.relative_residual = std::fabs(g[Idx(k + 1)]) / b_norm;
            if (h_next == 0.0 || result.relative_residual <= opt.tolerance) {
                ++k;
                break;
            }
            basis.push_back(w);
            for (int i = 0; i < n; ++i) basis.back()[Idx(i)] /= h_next;
        }

        // Back-substitute the small triangular system and update x.
        std::vector<double> y(Idx(k), 0.0);
        for (int i = k; i-- > 0;) {
            double sum = g[Idx(i)];
            for (int j = i + 1; j < k; ++j) sum -= hessenberg[Idx(i + j * (m + 1))] * y[Idx(j)];
            const double diag = hessenberg[Idx(i + i * (m + 1))];
            y[Idx(i)] = (diag != 0.0) ? sum / diag : 0.0;
        }
        for (int j = 0; j < k; ++j) {
            for (int i = 0; i < n; ++i) (*x)[Idx(i)] += y[Idx(j)] * basis[Idx(j)][Idx(i)];
        }

        // Recompute the true residual rather than trusting the estimate:
        // with restarting and preconditioning the two can drift apart,
        // and reporting convergence that did not happen is the worst
        // possible failure mode for a solver.
        a.Multiply(*x, &ax);
        double true_residual = 0.0;
        for (int i = 0; i < n; ++i) {
            const double diff = b[Idx(i)] - ax[Idx(i)];
            true_residual += diff * diff;
        }
        result.relative_residual = std::sqrt(true_residual) / b_norm;
        if (result.relative_residual <= opt.tolerance) {
            result.converged = true;
            return result;
        }
    }
    result.message = "reached the iteration limit without converging";
    return result;
}

}  // namespace num
