// Tree/index/worktree pairing, rename detection (git's diffcore-delta
// similarity), an xdiff-style line diff (Myers + git's group compaction
// with the indent heuristic), and unified-patch / --stat output.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <limits>
#include <unordered_map>
#include <vector>

#include "git/git.h"
#include "git/util.h"

namespace mep::git {

// ------------------------------------------------------------ pairing

std::vector<DiffPair> DiffMaps(const std::map<std::string, TreeEntry> &a, const std::map<std::string, TreeEntry> &b) {
    std::vector<DiffPair> out;
    auto ia = a.begin();
    auto ib = b.begin();
    while (ia != a.end() || ib != b.end()) {
        int c;
        if (ia == a.end()) c = 1;
        else if (ib == b.end()) c = -1;
        else c = ia->first.compare(ib->first);
        DiffPair p;
        if (c < 0) {
            p.status = 'D';
            p.a = DiffSide{ia->first, ia->second.mode, ia->second.oid, "", false, ""};
            p.b.path = ia->first;
            out.push_back(std::move(p));
            ++ia;
        } else if (c > 0) {
            p.status = 'A';
            p.a.path = ib->first;
            p.b = DiffSide{ib->first, ib->second.mode, ib->second.oid, "", false, ""};
            out.push_back(std::move(p));
            ++ib;
        } else {
            if (ia->second.oid != ib->second.oid || ia->second.mode != ib->second.mode) {
                bool type_change = (ia->second.mode & 0170000) != (ib->second.mode & 0170000);
                p.status = type_change ? 'T' : 'M';
                p.a = DiffSide{ia->first, ia->second.mode, ia->second.oid, "", false, ""};
                p.b = DiffSide{ib->first, ib->second.mode, ib->second.oid, "", false, ""};
                out.push_back(std::move(p));
            }
            ++ia;
            ++ib;
        }
    }
    return out;
}

bool DiffTrees(Repo &repo, const Oid &a, const Oid &b, const std::vector<std::string> &pathspec,
               std::vector<DiffPair> &out) {
    std::map<std::string, TreeEntry> ma, mb;
    if (!a.IsNull() && !repo.FlattenTree(a, ma)) return false;
    if (!b.IsNull() && !repo.FlattenTree(b, mb)) return false;
    if (!pathspec.empty()) {
        for (auto it = ma.begin(); it != ma.end();) it = PathspecMatch(pathspec, it->first) ? std::next(it) : ma.erase(it);
        for (auto it = mb.begin(); it != mb.end();) it = PathspecMatch(pathspec, it->first) ? std::next(it) : mb.erase(it);
    }
    out = DiffMaps(ma, mb);
    return true;
}

bool LoadSide(Repo &repo, DiffSide &s) {
    if (s.loaded) return true;
    s.loaded = true;
    if (!s.mode) {
        s.data.clear();
        return true;
    }
    if ((s.mode & 0170000) == 0160000) {
        s.data = "Subproject commit " + s.oid.Hex() + (s.dirty ? "-dirty" : "") + "\n";
        return true;
    }
    if (!s.abs.empty()) {
        FileStat st;
        if (!LStat(s.abs, st)) return false;
        if (!ReadWorktreeFile(s.abs, st, s.data)) return false;
        if (s.oid.IsNull()) s.oid = HashObject(ObjType::Blob, s.data);
        return true;
    }
    return repo.ReadBlob(s.oid, s.data);
}

// ------------------------------------------------------------ renames

namespace {
constexpr int kMaxScore = 60000;
constexpr unsigned kHashBase = 107927;

bool IsBinaryData(std::string_view s) {
    size_t n = std::min<size_t>(s.size(), 8000);
    return std::memchr(s.data(), 0, n) != nullptr;
}

// git's diffcore-delta hash_chars: (hash -> byte count) of each span.
std::unordered_map<unsigned, unsigned> SpanHash(std::string_view s) {
    std::unordered_map<unsigned, unsigned> h;
    bool is_text = !IsBinaryData(s);
    unsigned accum1 = 0, accum2 = 0;
    unsigned n = 0;
    size_t sz = s.size();
    const unsigned char *buf = reinterpret_cast<const unsigned char *>(s.data());
    while (sz) {
        unsigned c = *buf++;
        unsigned old_1 = accum1;
        sz--;
        if (is_text && c == '\r' && sz && *buf == '\n') continue;
        accum1 = (accum1 << 7) ^ (accum2 >> 25);
        accum2 = (accum2 << 7) ^ (old_1 >> 25);
        accum1 += c;
        if (++n < 64 && c != '\n') continue;
        unsigned hashval = (accum1 + accum2 * 0x61) % kHashBase;
        h[hashval] += n;
        n = 0;
        accum1 = accum2 = 0;
    }
    if (n > 0) {
        unsigned hashval = (accum1 + accum2 * 0x61) % kHashBase;
        h[hashval] += n;
    }
    return h;
}

int Similarity(const std::string &src, const std::string &dst, int min_score) {
    size_t max_size = std::max(src.size(), dst.size());
    size_t base_size = std::min(src.size(), dst.size());
    size_t delta_size = max_size - base_size;
    if (static_cast<uint64_t>(max_size) * static_cast<uint64_t>(kMaxScore - min_score) <
        static_cast<uint64_t>(delta_size) * kMaxScore)
        return 0;
    if (dst.empty()) return 0;
    auto hs = SpanHash(src);
    auto hd = SpanHash(dst);
    uint64_t copied = 0;
    for (const auto &[k, sc] : hs) {
        auto it = hd.find(k);
        if (it == hd.end()) continue;
        copied += std::min(sc, it->second);
    }
    return static_cast<int>(copied * kMaxScore / max_size);
}
}  // namespace

void DetectRenames(Repo &repo, std::vector<DiffPair> &pairs, int min_score_percent) {
    std::vector<size_t> srcs, dsts;
    for (size_t i = 0; i < pairs.size(); i++) {
        if (pairs[i].status == 'D') srcs.push_back(i);
        else if (pairs[i].status == 'A') dsts.push_back(i);
    }
    if (srcs.empty() || dsts.empty()) return;
    std::vector<long> dst_src(pairs.size(), -1);
    std::vector<int> dst_score(pairs.size(), 0);
    std::vector<bool> src_used(pairs.size(), false);
    // Exact renames first (same blob id), preferring a matching basename.
    for (size_t d : dsts) {
        long best = -1;
        for (size_t s : srcs) {
            if (src_used[s] || pairs[s].a.oid != pairs[d].b.oid) continue;
            if ((pairs[s].a.mode & 0170000) != (pairs[d].b.mode & 0170000)) continue;
            if (best < 0 || (BaseName(pairs[s].a.path) == BaseName(pairs[d].b.path) &&
                             BaseName(pairs[static_cast<size_t>(best)].a.path) != BaseName(pairs[d].b.path)))
                best = static_cast<long>(s);
        }
        if (best >= 0) {
            dst_src[d] = best;
            dst_score[d] = kMaxScore;
            src_used[static_cast<size_t>(best)] = true;
        }
    }
    // Inexact.
    int min_score = min_score_percent * kMaxScore / 100;
    struct Cand {
        int score;
        size_t d, s;
    };
    std::vector<Cand> cands;
    size_t nsrc = 0, ndst = 0;
    for (size_t s : srcs)
        if (!src_used[s]) nsrc++;
    for (size_t d : dsts)
        if (dst_src[d] < 0) ndst++;
    int64_t limit = repo.Cfg().GetInt("diff.renamelimit", 1000);
    if (nsrc && ndst && static_cast<int64_t>(nsrc * ndst) <= limit * limit) {
        for (size_t d : dsts) {
            if (dst_src[d] >= 0) continue;
            if ((pairs[d].b.mode & 0170000) != 0100000) continue;
            if (!LoadSide(repo, pairs[d].b)) continue;
            for (size_t s : srcs) {
                if (src_used[s]) continue;
                if ((pairs[s].a.mode & 0170000) != 0100000) continue;
                if (!LoadSide(repo, pairs[s].a)) continue;
                int sc = Similarity(pairs[s].a.data, pairs[d].b.data, min_score);
                if (sc >= min_score) cands.push_back({sc, d, s});
            }
        }
        std::stable_sort(cands.begin(), cands.end(), [](const Cand &x, const Cand &y) { return x.score > y.score; });
        for (const Cand &c : cands) {
            if (dst_src[c.d] >= 0 || src_used[c.s]) continue;
            dst_src[c.d] = static_cast<long>(c.s);
            dst_score[c.d] = c.score;
            src_used[c.s] = true;
        }
    }
    std::vector<DiffPair> out;
    for (size_t i = 0; i < pairs.size(); i++) {
        if (pairs[i].status == 'D' && src_used[i]) continue;
        if (pairs[i].status == 'A' && dst_src[i] >= 0) {
            DiffPair p;
            p.status = 'R';
            p.a = pairs[static_cast<size_t>(dst_src[i])].a;
            p.b = pairs[i].b;
            p.score = dst_score[i] * 100 / kMaxScore;
            out.push_back(std::move(p));
            continue;
        }
        out.push_back(std::move(pairs[i]));
    }
    pairs = std::move(out);
}

// ------------------------------------------------------------ line diff

std::vector<std::string_view> SplitLines(std::string_view s) {
    std::vector<std::string_view> out;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t nl = s.find('\n', pos);
        size_t end = nl == std::string_view::npos ? s.size() : nl + 1;
        out.push_back(s.substr(pos, end - pos));
        pos = end;
    }
    return out;
}

bool IsBinary(std::string_view s) { return IsBinaryData(s); }

namespace {
// --- A port of git's xdiff (xprepare.c + xdiffi.c): record classification,
// trimming of common ends, discarding of lines with no match on the other
// side (and of "multimatch" lines amid them), then Myers' divide and
// conquer with xdiff's cost heuristics. Matching xdiff here, rather than
// any minimal diff, is what makes hunks line up with git's own output.
constexpr long kXdlMaxEqLimit = 1024;
constexpr long kXdlSimscanWindow = 100;
constexpr long kXdlKpdisRun = 4;
constexpr long kXdlMaxCostMin = 256;
constexpr long kXdlHeurMinCost = 256;
constexpr long kXdlSnakeCnt = 20;
constexpr long kXdlKHeur = 4;
constexpr long kXdlLineMax = std::numeric_limits<long>::max();

long BogoSqrt(long n) {
    long i;
    for (i = 1; n > 0; n >>= 2) i <<= 1;
    return i;
}

struct XdlSide {
    const std::vector<int> *cls;  // class id per record
    long nrec;
    long dstart = 0, dend = -1;
    std::vector<long> rindex;  // reduced -> record index
    std::vector<long> ha;      // reduced class ids
    long nreff = 0;
    std::vector<char> *rchg;  // index i+1 is record i
    char &Chg(long i) { return (*rchg)[static_cast<size_t>(i + 1)]; }
};

bool CleanMMatch(const std::vector<char> &dis, long i, long s, long e) {
    if (i - s > kXdlSimscanWindow) s = i - kXdlSimscanWindow;
    if (e - i > kXdlSimscanWindow) e = i + kXdlSimscanWindow;
    long r, rdis0, rpdis0, rdis1, rpdis1;
    for (r = 1, rdis0 = 0, rpdis0 = 1; (i - r) >= s; r++) {
        char d = dis[static_cast<size_t>(i - r)];
        if (!d) rdis0++;
        else if (d == 2) rpdis0++;
        else break;
    }
    if (rdis0 == 0) return false;
    for (r = 1, rdis1 = 0, rpdis1 = 1; (i + r) <= e; r++) {
        char d = dis[static_cast<size_t>(i + r)];
        if (!d) rdis1++;
        else if (d == 2) rpdis1++;
        else break;
    }
    if (rdis1 == 0) return false;
    rdis1 += rdis0;
    rpdis1 += rpdis0;
    return rpdis1 * kXdlKpdisRun < (rpdis1 + rdis1);
}

struct XdlEnv {
    long mxcost, snake_cnt, heur_min;
};
struct XdlSplit {
    long i1 = 0, i2 = 0;
    bool min_lo = false, min_hi = false;
};

void XdlSplitBox(const long *ha1, long off1, long lim1, const long *ha2, long off2, long lim2, long *kvdf, long *kvdb,
                 bool need_min, XdlSplit &spl, const XdlEnv &xenv) {
    long dmin = off1 - lim2, dmax = lim1 - off2;
    long fmid = off1 - off2, bmid = lim1 - lim2;
    long odd = (fmid - bmid) & 1;
    long fmin = fmid, fmax = fmid;
    long bmin = bmid, bmax = bmid;
    long ec, d, i1, i2, prev1, best, dd, v, k;
    kvdf[fmid] = off1;
    kvdb[bmid] = lim1;
    for (ec = 1;; ec++) {
        bool got_snake = false;
        if (fmin > dmin) kvdf[--fmin - 1] = -1;
        else ++fmin;
        if (fmax < dmax) kvdf[++fmax + 1] = -1;
        else --fmax;
        for (d = fmax; d >= fmin; d -= 2) {
            if (kvdf[d - 1] >= kvdf[d + 1]) i1 = kvdf[d - 1] + 1;
            else i1 = kvdf[d + 1];
            prev1 = i1;
            i2 = i1 - d;
            for (; i1 < lim1 && i2 < lim2 && ha1[i1] == ha2[i2]; i1++, i2++) {
            }
            if (i1 - prev1 > xenv.snake_cnt) got_snake = true;
            kvdf[d] = i1;
            if (odd && bmin <= d && d <= bmax && kvdb[d] <= i1) {
                spl.i1 = i1;
                spl.i2 = i2;
                spl.min_lo = spl.min_hi = true;
                return;
            }
        }
        if (bmin > dmin) kvdb[--bmin - 1] = kXdlLineMax;
        else ++bmin;
        if (bmax < dmax) kvdb[++bmax + 1] = kXdlLineMax;
        else --bmax;
        for (d = bmax; d >= bmin; d -= 2) {
            if (kvdb[d - 1] < kvdb[d + 1]) i1 = kvdb[d - 1];
            else i1 = kvdb[d + 1] - 1;
            prev1 = i1;
            i2 = i1 - d;
            for (; i1 > off1 && i2 > off2 && ha1[i1 - 1] == ha2[i2 - 1]; i1--, i2--) {
            }
            if (prev1 - i1 > xenv.snake_cnt) got_snake = true;
            kvdb[d] = i1;
            if (!odd && fmin <= d && d <= fmax && i1 <= kvdf[d]) {
                spl.i1 = i1;
                spl.i2 = i2;
                spl.min_lo = spl.min_hi = true;
                return;
            }
        }
        if (need_min) continue;
        if (got_snake && ec > xenv.heur_min) {
            for (best = 0, d = fmax; d >= fmin; d -= 2) {
                dd = d > fmid ? d - fmid : fmid - d;
                i1 = kvdf[d];
                i2 = i1 - d;
                v = (i1 - off1) + (i2 - off2) - dd;
                if (v > kXdlKHeur * ec && v > best && off1 + xenv.snake_cnt <= i1 && i1 < lim1 &&
                    off2 + xenv.snake_cnt <= i2 && i2 < lim2) {
                    for (k = 1; ha1[i1 - k] == ha2[i2 - k]; k++)
                        if (k == xenv.snake_cnt) {
                            best = v;
                            spl.i1 = i1;
                            spl.i2 = i2;
                            break;
                        }
                }
            }
            if (best > 0) {
                spl.min_lo = true;
                spl.min_hi = false;
                return;
            }
            for (best = 0, d = bmax; d >= bmin; d -= 2) {
                dd = d > bmid ? d - bmid : bmid - d;
                i1 = kvdb[d];
                i2 = i1 - d;
                v = (lim1 - i1) + (lim2 - i2) - dd;
                if (v > kXdlKHeur * ec && v > best && off1 < i1 && i1 <= lim1 - xenv.snake_cnt && off2 < i2 &&
                    i2 <= lim2 - xenv.snake_cnt) {
                    for (k = 0; ha1[i1 + k] == ha2[i2 + k]; k++)
                        if (k == xenv.snake_cnt - 1) {
                            best = v;
                            spl.i1 = i1;
                            spl.i2 = i2;
                            break;
                        }
                }
            }
            if (best > 0) {
                spl.min_lo = false;
                spl.min_hi = true;
                return;
            }
        }
        if (ec >= xenv.mxcost) {
            long fbest = -1, fbest1 = -1;
            for (d = fmax; d >= fmin; d -= 2) {
                i1 = std::min(kvdf[d], lim1);
                i2 = i1 - d;
                if (lim2 < i2) {
                    i1 = lim2 + d;
                    i2 = lim2;
                }
                if (fbest < i1 + i2) {
                    fbest = i1 + i2;
                    fbest1 = i1;
                }
            }
            long bbest = kXdlLineMax, bbest1 = kXdlLineMax;
            for (d = bmax; d >= bmin; d -= 2) {
                i1 = std::max(off1, kvdb[d]);
                i2 = i1 - d;
                if (i2 < off2) {
                    i1 = off2 + d;
                    i2 = off2;
                }
                if (i1 + i2 < bbest) {
                    bbest = i1 + i2;
                    bbest1 = i1;
                }
            }
            if ((lim1 + lim2) - bbest < fbest - (off1 + off2)) {
                spl.i1 = fbest1;
                spl.i2 = fbest - fbest1;
                spl.min_lo = true;
                spl.min_hi = false;
            } else {
                spl.i1 = bbest1;
                spl.i2 = bbest - bbest1;
                spl.min_lo = false;
                spl.min_hi = true;
            }
            return;
        }
    }
}

void XdlRecsCmp(XdlSide &d1, long off1, long lim1, XdlSide &d2, long off2, long lim2, long *kvdf, long *kvdb,
                bool need_min, const XdlEnv &xenv) {
    const long *ha1 = d1.ha.data(), *ha2 = d2.ha.data();
    for (; off1 < lim1 && off2 < lim2 && ha1[off1] == ha2[off2]; off1++, off2++) {
    }
    for (; off1 < lim1 && off2 < lim2 && ha1[lim1 - 1] == ha2[lim2 - 1]; lim1--, lim2--) {
    }
    if (off1 == lim1) {
        for (; off2 < lim2; off2++) d2.Chg(d2.rindex[static_cast<size_t>(off2)]) = 1;
    } else if (off2 == lim2) {
        for (; off1 < lim1; off1++) d1.Chg(d1.rindex[static_cast<size_t>(off1)]) = 1;
    } else {
        XdlSplit spl;
        XdlSplitBox(ha1, off1, lim1, ha2, off2, lim2, kvdf, kvdb, need_min, spl, xenv);
        XdlRecsCmp(d1, off1, spl.i1, d2, off2, spl.i2, kvdf, kvdb, spl.min_lo, xenv);
        XdlRecsCmp(d1, spl.i1, lim1, d2, spl.i2, lim2, kvdf, kvdb, spl.min_hi, xenv);
    }
}

// Fills rchg1/rchg2 (size n+2, offset by one) the way xdl_do_diff does.
void XdlDoDiff(const std::vector<int> &c1, const std::vector<int> &c2, int nclasses, std::vector<char> &rchg1,
               std::vector<char> &rchg2) {
    XdlSide x1, x2;
    x1.cls = &c1;
    x2.cls = &c2;
    x1.nrec = static_cast<long>(c1.size());
    x2.nrec = static_cast<long>(c2.size());
    x1.rchg = &rchg1;
    x2.rchg = &rchg2;
    // Occurrence counts per class on each side.
    std::vector<long> len1(static_cast<size_t>(nclasses), 0), len2(static_cast<size_t>(nclasses), 0);
    for (int c : c1) len1[static_cast<size_t>(c)]++;
    for (int c : c2) len2[static_cast<size_t>(c)]++;
    // xdl_trim_ends
    long i, lim = std::min(x1.nrec, x2.nrec);
    for (i = 0; i < lim; i++)
        if (c1[static_cast<size_t>(i)] != c2[static_cast<size_t>(i)]) break;
    x1.dstart = x2.dstart = i;
    long j;
    for (lim -= i, j = 0; j < lim; j++)
        if (c1[static_cast<size_t>(x1.nrec - 1 - j)] != c2[static_cast<size_t>(x2.nrec - 1 - j)]) break;
    x1.dend = x1.nrec - j - 1;
    x2.dend = x2.nrec - j - 1;
    // xdl_cleanup_records
    std::vector<char> dis1(static_cast<size_t>(x1.nrec) + 1, 0), dis2(static_cast<size_t>(x2.nrec) + 1, 0);
    long mlim = BogoSqrt(x1.nrec);
    if (mlim > kXdlMaxEqLimit) mlim = kXdlMaxEqLimit;
    for (i = x1.dstart; i <= x1.dend; i++) {
        long nm = len2[static_cast<size_t>(c1[static_cast<size_t>(i)])];
        dis1[static_cast<size_t>(i)] = nm == 0 ? 0 : (nm >= mlim ? 2 : 1);
    }
    mlim = BogoSqrt(x2.nrec);
    if (mlim > kXdlMaxEqLimit) mlim = kXdlMaxEqLimit;
    for (i = x2.dstart; i <= x2.dend; i++) {
        long nm = len1[static_cast<size_t>(c2[static_cast<size_t>(i)])];
        dis2[static_cast<size_t>(i)] = nm == 0 ? 0 : (nm >= mlim ? 2 : 1);
    }
    for (i = x1.dstart; i <= x1.dend; i++) {
        char d = dis1[static_cast<size_t>(i)];
        if (d == 1 || (d == 2 && !CleanMMatch(dis1, i, x1.dstart, x1.dend))) {
            x1.rindex.push_back(i);
            x1.ha.push_back(c1[static_cast<size_t>(i)]);
        } else {
            x1.Chg(i) = 1;
        }
    }
    for (i = x2.dstart; i <= x2.dend; i++) {
        char d = dis2[static_cast<size_t>(i)];
        if (d == 1 || (d == 2 && !CleanMMatch(dis2, i, x2.dstart, x2.dend))) {
            x2.rindex.push_back(i);
            x2.ha.push_back(c2[static_cast<size_t>(i)]);
        } else {
            x2.Chg(i) = 1;
        }
    }
    x1.nreff = static_cast<long>(x1.rindex.size());
    x2.nreff = static_cast<long>(x2.rindex.size());
    long ndiags = x1.nreff + x2.nreff + 3;
    std::vector<long> kvd(static_cast<size_t>(2 * ndiags + 2), 0);
    long *kvdf = kvd.data() + x2.nreff + 1;
    long *kvdb = kvd.data() + ndiags + x2.nreff + 1;
    XdlEnv xenv;
    xenv.mxcost = BogoSqrt(ndiags);
    if (xenv.mxcost < kXdlMaxCostMin) xenv.mxcost = kXdlMaxCostMin;
    xenv.snake_cnt = kXdlSnakeCnt;
    xenv.heur_min = kXdlHeurMinCost;
    XdlRecsCmp(x1, 0, x1.nreff, x2, 0, x2.nreff, kvdf, kvdb, false, xenv);
}

// --- git's xdl_change_compact, with the indent heuristic.
struct XFile {
    const std::vector<std::string_view> *recs;
    const std::vector<int> *ids;
    std::vector<char> rchg;  // size nrec + 2, index i+1 is record i
    long nrec;
    char &Chg(long i) { return rchg[static_cast<size_t>(i + 1)]; }
    bool Match(long i, long j) const { return (*ids)[static_cast<size_t>(i)] == (*ids)[static_cast<size_t>(j)]; }
};
struct Group {
    long start, end;
};
void GroupInit(XFile &f, Group &g) {
    g.start = g.end = 0;
    while (f.Chg(g.end)) g.end++;
}
bool GroupNext(XFile &f, Group &g) {
    if (g.end == f.nrec) return false;
    g.start = g.end + 1;
    for (g.end = g.start; f.Chg(g.end); g.end++) {
    }
    return true;
}
bool GroupPrevious(XFile &f, Group &g) {
    if (g.start == 0) return false;
    g.end = g.start - 1;
    for (g.start = g.end; f.Chg(g.start - 1); g.start--) {
    }
    return true;
}
bool GroupSlideDown(XFile &f, Group &g) {
    if (g.end < f.nrec && f.Match(g.start, g.end)) {
        f.Chg(g.start++) = 0;
        f.Chg(g.end++) = 1;
        while (f.Chg(g.end)) g.end++;
        return true;
    }
    return false;
}
bool GroupSlideUp(XFile &f, Group &g) {
    if (g.start > 0 && f.Match(g.start - 1, g.end - 1)) {
        f.Chg(--g.start) = 1;
        f.Chg(--g.end) = 0;
        while (f.Chg(g.start - 1)) g.start--;
        return true;
    }
    return false;
}

constexpr int kMaxIndent = 200, kMaxBlanks = 20;
int GetIndent(std::string_view rec) {
    int ret = 0;
    for (char ch : rec) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isspace(c)) return ret;
        if (c == ' ') ret += 1;
        else if (c == '\t') ret += 8 - ret % 8;
        if (ret >= kMaxIndent) return kMaxIndent;
    }
    return -1;
}
struct SplitMeasurement {
    int end_of_file, indent, pre_blank, pre_indent, post_blank, post_indent;
};
void MeasureSplit(const XFile &f, long split, SplitMeasurement &m) {
    if (split >= f.nrec) {
        m.end_of_file = 1;
        m.indent = -1;
    } else {
        m.end_of_file = 0;
        m.indent = GetIndent((*f.recs)[static_cast<size_t>(split)]);
    }
    m.pre_blank = 0;
    m.pre_indent = -1;
    for (long i = split - 1; i >= 0; i--) {
        m.pre_indent = GetIndent((*f.recs)[static_cast<size_t>(i)]);
        if (m.pre_indent != -1) break;
        m.pre_blank += 1;
        if (m.pre_blank == kMaxBlanks) {
            m.pre_indent = 0;
            break;
        }
    }
    m.post_blank = 0;
    m.post_indent = -1;
    for (long i = split + 1; i < f.nrec; i++) {
        m.post_indent = GetIndent((*f.recs)[static_cast<size_t>(i)]);
        if (m.post_indent != -1) break;
        m.post_blank += 1;
        if (m.post_blank == kMaxBlanks) {
            m.post_indent = 0;
            break;
        }
    }
}
struct SplitScore {
    int effective_indent, penalty;
};
void ScoreAddSplit(const SplitMeasurement &m, SplitScore &s) {
    if (m.pre_indent == -1 && m.pre_blank == 0) s.penalty += 1;  // START_OF_FILE_PENALTY
    if (m.end_of_file) s.penalty += 21;                          // END_OF_FILE_PENALTY
    int post_blank = (m.indent == -1) ? 1 + m.post_blank : 0;
    int total_blank = m.pre_blank + post_blank;
    s.penalty += -30 * total_blank;  // TOTAL_BLANK_WEIGHT
    s.penalty += 6 * post_blank;     // POST_BLANK_WEIGHT
    int indent = m.indent != -1 ? m.indent : m.post_indent;
    bool any_blanks = total_blank != 0;
    s.effective_indent += indent;
    if (indent == -1 || m.pre_indent == -1) {
    } else if (indent > m.pre_indent) {
        s.penalty += any_blanks ? 10 : -4;
    } else if (indent == m.pre_indent) {
    } else if (m.post_indent != -1 && m.post_indent > indent) {
        s.penalty += any_blanks ? 17 : 24;
    } else {
        s.penalty += any_blanks ? 17 : 23;
    }
}
int ScoreCmp(const SplitScore &s1, const SplitScore &s2) {
    int cmp = (s1.effective_indent > s2.effective_indent) - (s1.effective_indent < s2.effective_indent);
    return 60 * cmp + (s1.penalty - s2.penalty);
}

void ChangeCompact(XFile &f, XFile &fo) {
    Group g, go;
    GroupInit(f, g);
    GroupInit(fo, go);
    for (;;) {
        if (g.end != g.start) {
            long groupsize, earliest_end, end_matching_other;
            do {
                groupsize = g.end - g.start;
                end_matching_other = -1;
                while (GroupSlideUp(f, g)) GroupPrevious(fo, go);
                earliest_end = g.end;
                if (go.end > go.start) end_matching_other = g.end;
                for (;;) {
                    if (!GroupSlideDown(f, g)) break;
                    GroupNext(fo, go);
                    if (go.end > go.start) end_matching_other = g.end;
                }
            } while (groupsize != g.end - g.start);
            if (g.end == earliest_end) {
            } else if (end_matching_other != -1) {
                while (go.end == go.start) {
                    GroupSlideUp(f, g);
                    GroupPrevious(fo, go);
                }
            } else {
                long shift = earliest_end;
                if (g.end - groupsize - 1 > shift) shift = g.end - groupsize - 1;
                if (g.end - 100 > shift) shift = g.end - 100;
                long best_shift = -1;
                SplitScore best{0, 0};
                for (; shift <= g.end; shift++) {
                    SplitMeasurement m;
                    SplitScore score{0, 0};
                    MeasureSplit(f, shift, m);
                    ScoreAddSplit(m, score);
                    MeasureSplit(f, shift - groupsize, m);
                    ScoreAddSplit(m, score);
                    if (best_shift == -1 || ScoreCmp(score, best) <= 0) {
                        best = score;
                        best_shift = shift;
                    }
                }
                while (g.end > best_shift) {
                    GroupSlideUp(f, g);
                    GroupPrevious(fo, go);
                }
            }
        }
        if (!GroupNext(f, g)) break;
        GroupNext(fo, go);
    }
}
}  // namespace

std::vector<LineHunk> DiffLines(const std::vector<std::string_view> &a, const std::vector<std::string_view> &b) {
    std::unordered_map<std::string_view, int> ids;
    std::vector<int> ia(a.size()), ib(b.size());
    for (size_t i = 0; i < a.size(); i++) ia[i] = ids.emplace(a[i], static_cast<int>(ids.size())).first->second;
    for (size_t i = 0; i < b.size(); i++) ib[i] = ids.emplace(b[i], static_cast<int>(ids.size())).first->second;
    XFile fa{&a, &ia, std::vector<char>(a.size() + 2, 0), static_cast<long>(a.size())};
    XFile fb{&b, &ib, std::vector<char>(b.size() + 2, 0), static_cast<long>(b.size())};
    XdlDoDiff(ia, ib, static_cast<int>(ids.size()), fa.rchg, fb.rchg);
    ChangeCompact(fa, fb);
    ChangeCompact(fb, fa);
    // Build the script: walk both in step.
    std::vector<LineHunk> out;
    long i1 = 0, i2 = 0;
    long n1 = fa.nrec, n2 = fb.nrec;
    while (i1 < n1 || i2 < n2) {
        if (i1 < n1 && i2 < n2 && !fa.Chg(i1) && !fb.Chg(i2)) {
            i1++;
            i2++;
            continue;
        }
        long s1 = i1, s2 = i2;
        while (i1 < n1 && fa.Chg(i1)) i1++;
        while (i2 < n2 && fb.Chg(i2)) i2++;
        if (i1 == s1 && i2 == s2) {
            // Should not happen; avoid looping forever.
            if (i1 < n1) i1++;
            if (i2 < n2) i2++;
            continue;
        }
        out.push_back(LineHunk{s1, i1 - s1, s2, i2 - s2});
    }
    return out;
}

// ------------------------------------------------------------ output

namespace {
std::string AbbrevOid(Repo &repo, const Oid &o) {
    if (o.IsNull()) return std::string(static_cast<size_t>(repo.DefaultAbbrev()), '0');
    return repo.Abbrev(o);
}
std::string Mode6(uint32_t mode) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%06o", mode);
    return buf;
}

long FuncLine(const std::vector<std::string_view> &a, long start, long limit, std::string &func) {
    long step = start > limit ? -1 : 1;
    for (long l = start; l != limit && l >= 0 && l < static_cast<long>(a.size()); l += step) {
        std::string_view rec = a[static_cast<size_t>(l)];
        if (!rec.empty() && (std::isalpha(static_cast<unsigned char>(rec[0])) || rec[0] == '_' || rec[0] == '$')) {
            size_t len = std::min<size_t>(rec.size(), 80);
            while (len > 0 && std::isspace(static_cast<unsigned char>(rec[len - 1]))) len--;
            func = std::string(rec.substr(0, len));
            return l;
        }
    }
    return -1;
}

void EmitLine(std::string &out, char sign, std::string_view line) {
    out += sign;
    if (!line.empty() && line.back() == '\n') {
        out.append(line);
    } else {
        out.append(line);
        out += "\n\\ No newline at end of file\n";
    }
}

void EmitHunks(std::string &out, const std::vector<std::string_view> &a, const std::vector<std::string_view> &b,
               const std::vector<LineHunk> &hunks, int ctx) {
    std::string func;
    long funcprev = -1;
    size_t i = 0;
    while (i < hunks.size()) {
        size_t j = i;
        while (j + 1 < hunks.size() && hunks[j + 1].a0 - (hunks[j].a0 + hunks[j].a_len) <= 2 * ctx) j++;
        long s1 = std::max(hunks[i].a0 - ctx, 0L);
        long s2 = std::max(hunks[i].b0 - ctx, 0L);
        long e1 = std::min(hunks[j].a0 + hunks[j].a_len + ctx, static_cast<long>(a.size()));
        long e2 = std::min(hunks[j].b0 + hunks[j].b_len + ctx, static_cast<long>(b.size()));
        // Context can't extend past what the other side has in common.
        long lead = hunks[i].a0 - s1;
        long lead2 = hunks[i].b0 - s2;
        long l = std::min(lead, lead2);
        s1 = hunks[i].a0 - l;
        s2 = hunks[i].b0 - l;
        long trail = e1 - (hunks[j].a0 + hunks[j].a_len);
        long trail2 = e2 - (hunks[j].b0 + hunks[j].b_len);
        long t = std::min(trail, trail2);
        e1 = hunks[j].a0 + hunks[j].a_len + t;
        e2 = hunks[j].b0 + hunks[j].b_len + t;
        FuncLine(a, s1 - 1, funcprev, func);
        funcprev = s1 - 1;
        long c1 = e1 - s1, c2 = e2 - s2;
        out += "@@ -" + std::to_string(c1 ? s1 + 1 : s1);
        if (c1 != 1) out += "," + std::to_string(c1);
        out += " +" + std::to_string(c2 ? s2 + 1 : s2);
        if (c2 != 1) out += "," + std::to_string(c2);
        out += " @@";
        if (!func.empty()) out += " " + func;
        out += "\n";
        long pa = s1;
        for (size_t k = i; k <= j; k++) {
            const LineHunk &h = hunks[k];
            while (pa < h.a0) {
                EmitLine(out, ' ', a[static_cast<size_t>(pa)]);
                pa++;
            }
            for (long x = h.a0; x < h.a0 + h.a_len; x++) EmitLine(out, '-', a[static_cast<size_t>(x)]);
            for (long y = h.b0; y < h.b0 + h.b_len; y++) EmitLine(out, '+', b[static_cast<size_t>(y)]);
            pa = h.a0 + h.a_len;
        }
        while (pa < e1) {
            EmitLine(out, ' ', a[static_cast<size_t>(pa)]);
            pa++;
        }
        i = j + 1;
    }
}

std::string PathA(const DiffPair &p) { return p.a.path; }
std::string PathB(const DiffPair &p) { return p.b.path.empty() ? p.a.path : p.b.path; }

void SplitTypeChanges(std::vector<DiffPair> &pairs) {
    std::vector<DiffPair> out;
    for (DiffPair &p : pairs) {
        if (p.status != 'T') {
            out.push_back(std::move(p));
            continue;
        }
        DiffPair d = p, a = p;
        d.status = 'D';
        d.b.mode = 0;
        d.b.oid = Oid{};
        d.b.abs.clear();
        d.b.loaded = false;
        d.b.data.clear();
        a.status = 'A';
        a.a.mode = 0;
        a.a.oid = Oid{};
        a.a.loaded = false;
        a.a.data.clear();
        out.push_back(std::move(d));
        out.push_back(std::move(a));
    }
    pairs = std::move(out);
}
}  // namespace

std::string FormatPatch(Repo &repo, std::vector<DiffPair> &pairs, const DiffOptions &opts) {
    SplitTypeChanges(pairs);
    std::string out;
    for (DiffPair &p : pairs) {
        std::string na = PathA(p), nb = PathB(p);
        out += "diff --git " + QuotePath("a/" + na) + " " + QuotePath("b/" + nb) + "\n";
        bool is_new = p.a.mode == 0, is_del = p.b.mode == 0;
        if (is_new) out += "new file mode " + Mode6(p.b.mode) + "\n";
        else if (is_del) out += "deleted file mode " + Mode6(p.a.mode) + "\n";
        else if (p.a.mode != p.b.mode) out += "old mode " + Mode6(p.a.mode) + "\nnew mode " + Mode6(p.b.mode) + "\n";
        if (p.status == 'R') {
            out += "similarity index " + std::to_string(p.score) + "%\n";
            out += "rename from " + QuotePath(na) + "\nrename to " + QuotePath(nb) + "\n";
        }
        if (!LoadSide(repo, p.a) || !LoadSide(repo, p.b)) continue;
        if (p.b.mode && p.b.oid.IsNull()) p.b.oid = HashObject(ObjType::Blob, p.b.data);
        if (p.a.oid != p.b.oid) {
            out += "index " + AbbrevOid(repo, p.a.oid) + ".." + AbbrevOid(repo, p.b.oid);
            if (!is_new && !is_del && p.a.mode == p.b.mode) out += " " + Mode6(p.a.mode);
            out += "\n";
        } else if (!p.b.dirty) {
            continue;  // pure rename / mode change: no content to show

        }
        std::string la = is_new ? "/dev/null" : QuotePath("a/" + na);
        std::string lb = is_del ? "/dev/null" : QuotePath("b/" + nb);
        bool binary = ((p.a.mode & 0170000) != 0160000 && IsBinaryData(p.a.data)) ||
                      ((p.b.mode & 0170000) != 0160000 && IsBinaryData(p.b.data));
        if (binary) {
            out += "Binary files " + (is_new ? std::string("/dev/null") : "a/" + na) + " and " +
                   (is_del ? std::string("/dev/null") : "b/" + nb) + " differ\n";
            continue;
        }
        std::vector<std::string_view> a = SplitLines(p.a.data), b = SplitLines(p.b.data);
        std::vector<LineHunk> hunks = DiffLines(a, b);
        // git prints the ---/+++ pair along with the first hunk: none at
        // all for, e.g., a new empty file.
        if (hunks.empty()) continue;
        out += "--- " + la + "\n+++ " + lb + "\n";
        EmitHunks(out, a, b, hunks, opts.context);
    }
    return out;
}

namespace {
std::string PprintRename(const std::string &a, const std::string &b) {
    if (QuotePath(a) != a || QuotePath(b) != b) return QuotePath(a) + " => " + QuotePath(b);
    long len_a = static_cast<long>(a.size()), len_b = static_cast<long>(b.size());
    long pfx = 0;
    {
        long i = 0;
        while (i < len_a && i < len_b && a[static_cast<size_t>(i)] == b[static_cast<size_t>(i)]) {
            if (a[static_cast<size_t>(i)] == '/') pfx = i + 1;
            i++;
        }
    }
    long sfx = 0;
    long adjust = pfx ? 1 : 0;
    long oa = len_a, ob = len_b;  // positions (old_name = a + oa)
    // git compares *old_name == *new_name where old_name starts at the terminating NUL.
    while (pfx - adjust <= oa && pfx - adjust <= ob) {
        char ca = oa < len_a ? a[static_cast<size_t>(oa)] : '\0';
        char cb = ob < len_b ? b[static_cast<size_t>(ob)] : '\0';
        if (ca != cb) break;
        if (ca == '/') sfx = len_a - oa;
        oa--;
        ob--;
        if (oa < 0 || ob < 0) break;
    }
    long amid = std::max(0L, len_a - pfx - sfx);
    long bmid = std::max(0L, len_b - pfx - sfx);
    std::string out;
    if (pfx + sfx) {
        out += a.substr(0, static_cast<size_t>(pfx));
        out += '{';
    }
    out += a.substr(static_cast<size_t>(pfx), static_cast<size_t>(amid));
    out += " => ";
    out += b.substr(static_cast<size_t>(pfx), static_cast<size_t>(bmid));
    if (pfx + sfx) {
        out += '}';
        out += a.substr(static_cast<size_t>(len_a - sfx));
    }
    return out;
}
int DecimalWidth(uint64_t n) {
    int w = 1;
    while (n >= 10) {
        n /= 10;
        w++;
    }
    return w;
}
long ScaleLinear(long it, long width, long max_change) {
    if (!it) return 0;
    return 1 + (it * (width - 1) / max_change);
}
}  // namespace

std::string FormatStat(Repo &repo, std::vector<DiffPair> &pairs, const DiffOptions &opts) {
    struct File {
        std::string name;
        long added = 0, deleted = 0;
        bool binary = false;
        bool interesting = false;
    };
    std::vector<File> files;
    for (DiffPair &p : pairs) {
        File f;
        f.name = p.status == 'R' ? PprintRename(p.a.path, p.b.path) : QuotePath(PathB(p));
        if (!LoadSide(repo, p.a) || !LoadSide(repo, p.b)) continue;
        bool sub = (p.a.mode & 0170000) == 0160000 || (p.b.mode & 0170000) == 0160000;
        f.binary = !sub && (IsBinaryData(p.a.data) || IsBinaryData(p.b.data));
        f.interesting = p.status != 'M' || p.a.mode != p.b.mode;
        if (f.binary) {
            f.added = static_cast<long>(p.b.data.size());
            f.deleted = static_cast<long>(p.a.data.size());
            if (p.a.data == p.b.data) f.added = f.deleted = 0;
        } else {
            std::vector<std::string_view> a = SplitLines(p.a.data), b = SplitLines(p.b.data);
            for (const LineHunk &h : DiffLines(a, b)) {
                f.added += h.b_len;
                f.deleted += h.a_len;
            }
        }
        files.push_back(std::move(f));
    }
    long max_change = 0, max_len = 0;
    int number_width = 0, bin_width = 0;
    for (const File &f : files) {
        long change = f.added + f.deleted;
        if (!f.interesting && change == 0 && !f.binary) continue;
        max_len = std::max(max_len, static_cast<long>(f.name.size()));
        if (f.binary) {
            int w = 14 + DecimalWidth(static_cast<uint64_t>(f.added)) + DecimalWidth(static_cast<uint64_t>(f.deleted));
            bin_width = std::max(bin_width, w);
            number_width = 3;
            continue;
        }
        max_change = std::max(max_change, change);
    }
    long width = opts.stat_width;
    number_width = std::max(DecimalWidth(static_cast<uint64_t>(max_change)), number_width);
    if (width < 16 + 6 + number_width) width = 16 + 6 + number_width;
    long graph_width = max_change + 4 > bin_width ? max_change : bin_width - 4;
    long name_width = max_len;
    if (name_width + number_width + 6 + graph_width > width) {
        if (graph_width > width * 3 / 8 - number_width - 6) {
            graph_width = width * 3 / 8 - number_width - 6;
            if (graph_width < 6) graph_width = 6;
        }
        if (name_width > width - number_width - 6 - graph_width) name_width = width - number_width - 6 - graph_width;
        else graph_width = width - number_width - 6 - name_width;
    }
    std::string out;
    long total_files = 0, adds = 0, dels = 0;
    for (const File &f : files) {
        long added = f.added, deleted = f.deleted;
        if (!f.interesting && added + deleted == 0 && !f.binary) continue;
        total_files++;
        std::string name = f.name;
        std::string prefix;
        long len = name_width;
        if (name_width < static_cast<long>(name.size())) {
            prefix = "...";
            len -= 3;
            if (len < 0) len = 0;
            name = name.substr(name.size() - static_cast<size_t>(len));
            size_t slash = name.find('/');
            if (slash != std::string::npos) name = name.substr(slash);
        }
        long padding = len - static_cast<long>(name.size());
        if (padding < 0) padding = 0;
        auto pad = [](long n) { return std::string(static_cast<size_t>(n), ' '); };
        auto rjust = [](const std::string &s, int w) {
            return s.size() >= static_cast<size_t>(w) ? s : std::string(static_cast<size_t>(w) - s.size(), ' ') + s;
        };
        if (f.binary) {
            out += " " + prefix + name + pad(padding) + " | " + rjust("Bin", number_width);
            if (!added && !deleted) {
                out += "\n";
                continue;
            }
            out += " " + std::to_string(deleted) + " -> " + std::to_string(added) + " bytes\n";
            continue;
        }
        adds += added;
        dels += deleted;
        long add = added, del = deleted;
        if (graph_width <= max_change) {
            long total = ScaleLinear(add + del, graph_width, max_change);
            if (total < 2 && add && del) total = 2;
            if (add < del) {
                add = ScaleLinear(add, graph_width, max_change);
                del = total - add;
            } else {
                del = ScaleLinear(del, graph_width, max_change);
                add = total - del;
            }
        }
        out += " " + prefix + name + pad(padding) + " | " + rjust(std::to_string(added + deleted), number_width) +
               (added + deleted ? " " : "");
        out += std::string(static_cast<size_t>(add), '+') + std::string(static_cast<size_t>(del), '-') + "\n";
    }
    if (total_files == 0) {
        out += " 0 files changed\n";
        return out;
    }
    out += " " + std::to_string(total_files) + (total_files == 1 ? " file changed" : " files changed");
    if (adds || dels == 0) out += ", " + std::to_string(adds) + (adds == 1 ? " insertion(+)" : " insertions(+)");
    if (dels || adds == 0) out += ", " + std::to_string(dels) + (dels == 1 ? " deletion(-)" : " deletions(-)");
    out += "\n";
    return out;
}

// ------------------------------------------------------------ combined diff

namespace {
// A port of git's combine-diff.c (dense mode, the `--cc` that `git show`
// uses for merges): per result line, which parents it is new to; per
// position, the lines lost from which parents (coalesced across parents by
// LCS); then git's hunk selection and its `@@@` output.
struct LostLine {
    std::string text;  // without the '\n'
    unsigned long parent_map;
};
struct SLine {
    std::string_view text;  // the result line, without '\n'
    std::vector<LostLine> lost;
    std::vector<LostLine> plost;  // pending for the parent being processed
    unsigned long flag = 0;
    std::vector<unsigned long> p_lno;
};
constexpr unsigned long kCombinedContext = 3;

void CoalesceLines(std::vector<LostLine> &base, std::vector<LostLine> &newv, int parent) {
    if (newv.empty()) return;
    if (base.empty()) {
        base = std::move(newv);
        newv.clear();
        return;
    }
    enum Dir : char { kMatch, kBase, kNew };
    size_t lb = base.size(), ln = newv.size();
    std::vector<std::vector<int>> lcs(lb + 1, std::vector<int>(ln + 1, 0));
    std::vector<std::vector<char>> dir(lb + 1, std::vector<char>(ln + 1, kBase));
    for (size_t j = 1; j <= ln; j++) dir[0][j] = kNew;
    for (size_t i = 1; i <= lb; i++) {
        for (size_t j = 1; j <= ln; j++) {
            if (base[i - 1].text == newv[j - 1].text) {
                lcs[i][j] = lcs[i - 1][j - 1] + 1;
                dir[i][j] = kMatch;
            } else if (lcs[i][j - 1] >= lcs[i - 1][j]) {
                lcs[i][j] = lcs[i][j - 1];
                dir[i][j] = kNew;
            } else {
                lcs[i][j] = lcs[i - 1][j];
                dir[i][j] = kBase;
            }
        }
    }
    size_t i = lb, j = ln;
    while (i != 0 || j != 0) {
        if (dir[i][j] == kMatch) {
            base[i - 1].parent_map |= 1UL << parent;
            i--;
            j--;
        } else if (dir[i][j] == kNew) {
            base.insert(base.begin() + static_cast<long>(i), newv[j - 1]);
            j--;
        } else {
            i--;
        }
    }
    newv.clear();
}

unsigned long FindNext(const std::vector<SLine> &sl, unsigned long mark, unsigned long i, unsigned long cnt,
                       bool uninteresting) {
    while (i <= cnt) {
        bool marked = (sl[i].flag & mark) != 0;
        if (uninteresting ? !marked : marked) return i;
        i++;
    }
    return i;
}

unsigned long AdjustHunkTail(const std::vector<SLine> &sl, unsigned long all_mask, unsigned long hunk_begin,
                             unsigned long i) {
    if (hunk_begin + 1 <= i && !(sl[i - 1].flag & all_mask)) i--;
    return i;
}

bool GiveContext(std::vector<SLine> &sl, unsigned long cnt, int num_parent) {
    unsigned long all_mask = (1UL << num_parent) - 1;
    unsigned long mark = 1UL << num_parent;
    unsigned long no_pre_delete = 2UL << num_parent;
    unsigned long i = FindNext(sl, mark, 0, cnt, false);
    if (cnt < i) return false;
    while (i <= cnt) {
        unsigned long j = kCombinedContext < i ? i - kCombinedContext : 0;
        while (j < i) {
            if (!(sl[j].flag & mark)) sl[j].flag |= no_pre_delete;
            sl[j++].flag |= mark;
        }
        for (;;) {
            j = FindNext(sl, mark, i, cnt, true);
            if (cnt < j) return true;
            unsigned long k = FindNext(sl, mark, j, cnt, false);
            j = AdjustHunkTail(sl, all_mask, i, j);
            if (k < j + kCombinedContext) {
                while (j < k) sl[j++].flag |= mark;
                i = k;
                continue;
            }
            i = k;
            k = (j + kCombinedContext < cnt + 1) ? j + kCombinedContext : cnt + 1;
            while (j < k) sl[j++].flag |= mark;
            break;
        }
    }
    return true;
}

bool MakeHunks(std::vector<SLine> &sl, unsigned long cnt, int num_parent) {
    unsigned long all_mask = (1UL << num_parent) - 1;
    unsigned long mark = 1UL << num_parent;
    for (unsigned long i = 0; i <= cnt; i++) {
        if ((sl[i].flag & all_mask) || !sl[i].lost.empty()) sl[i].flag |= mark;
        else sl[i].flag &= ~mark;
    }
    unsigned long i = 0;
    while (i <= cnt) {
        while (i <= cnt && !(sl[i].flag & mark)) i++;
        if (cnt < i) break;
        unsigned long hunk_begin = i, j;
        for (j = i + 1; j <= cnt; j++) {
            if (!(sl[j].flag & mark)) {
                unsigned long la = AdjustHunkTail(sl, all_mask, hunk_begin, j);
                la = (la + kCombinedContext < cnt + 1) ? la + kCombinedContext : cnt + 1;
                bool contin = false;
                while (la && j <= --la) {
                    if (sl[la].flag & mark) {
                        contin = true;
                        break;
                    }
                }
                if (!contin) break;
                j = la;
            }
        }
        unsigned long hunk_end = j;
        unsigned long same_diff = 0;
        bool has_interesting = false;
        for (j = i; j < hunk_end && !has_interesting; j++) {
            unsigned long this_diff = sl[j].flag & all_mask;
            if (this_diff) {
                if (!same_diff) same_diff = this_diff;
                else if (same_diff != this_diff) {
                    has_interesting = true;
                    break;
                }
            }
            for (const LostLine &ll : sl[j].lost) {
                if (has_interesting) break;
                this_diff = ll.parent_map;
                if (!same_diff) same_diff = this_diff;
                else if (same_diff != this_diff) has_interesting = true;
            }
        }
        if (!has_interesting && same_diff != all_mask)
            for (j = hunk_begin; j < hunk_end; j++) sl[j].flag &= ~mark;
        i = hunk_end;
    }
    return GiveContext(sl, cnt, num_parent);
}

void DumpSLine(std::string &out, const std::vector<SLine> &sl, unsigned long cnt, int num_parent) {
    unsigned long mark = 1UL << num_parent;
    unsigned long no_pre_delete = 2UL << num_parent;
    unsigned long lno = 0;
    auto show_line = [&](std::string_view line) {
        out.append(line);
        out += '\n';
    };
    for (;;) {
        std::string_view hunk_comment;
        bool have_comment = false;
        while (lno <= cnt && !(sl[lno].flag & mark)) {
            std::string_view t = sl[lno].text;
            if (lno < cnt && !t.empty() &&
                (std::isalpha(static_cast<unsigned char>(t[0])) || t[0] == '_' || t[0] == '$')) {
                hunk_comment = t;
                have_comment = true;
            }
            lno++;
        }
        if (cnt < lno) break;
        unsigned long hunk_end;
        for (hunk_end = lno + 1; hunk_end <= cnt; hunk_end++)
            if (!(sl[hunk_end].flag & mark)) break;
        unsigned long rlines = hunk_end - lno;
        if (cnt < hunk_end) rlines--;
        out += std::string(static_cast<size_t>(num_parent) + 1, '@');
        for (int n = 0; n < num_parent; n++) {
            unsigned long l0 = sl[lno].p_lno[static_cast<size_t>(n)];
            unsigned long l1 = sl[hunk_end].p_lno[static_cast<size_t>(n)];
            out += " -" + std::to_string(l0) + "," + std::to_string(l1 - l0);
        }
        out += " +" + std::to_string(lno + 1) + "," + std::to_string(rlines) + " ";
        out += std::string(static_cast<size_t>(num_parent) + 1, '@');
        if (have_comment) {
            size_t comment_end = 0;
            for (size_t k = 0; k < 40 && k < hunk_comment.size(); k++) {
                unsigned char ch = static_cast<unsigned char>(hunk_comment[k]);
                if (!ch || ch == '\n') break;
                if (!std::isspace(ch)) comment_end = k;
            }
            if (comment_end) out += " ";
            out.append(hunk_comment.substr(0, comment_end));
        }
        out += "\n";
        while (lno < hunk_end) {
            const SLine &s = sl[lno++];
            if (!(s.flag & no_pre_delete)) {
                for (const LostLine &ll : s.lost) {
                    for (int n = 0; n < num_parent; n++) out += (ll.parent_map & (1UL << n)) ? '-' : ' ';
                    show_line(ll.text);
                }
            }
            if (cnt < lno) break;
            unsigned long p_mask = 1;
            for (int n = 0; n < num_parent; n++) {
                out += (p_mask & s.flag) ? '+' : ' ';
                p_mask <<= 1;
            }
            show_line(s.text);
        }
    }
}

std::string_view StripNl(std::string_view l) {
    if (!l.empty() && l.back() == '\n') l.remove_suffix(1);
    return l;
}
}  // namespace

std::string FormatCombined(Repo &repo, const Commit &merge, int &num_paths) {
    num_paths = 0;
    std::string out;
    int num_parent = static_cast<int>(merge.parents.size());
    if (num_parent < 2) return out;
    std::map<std::string, TreeEntry> result;
    repo.FlattenTree(merge.tree, result);
    std::vector<std::map<std::string, TreeEntry>> parents(static_cast<size_t>(num_parent));
    for (int i = 0; i < num_parent; i++) {
        Commit pc;
        if (repo.ReadCommit(merge.parents[static_cast<size_t>(i)], pc)) repo.FlattenTree(pc.tree, parents[static_cast<size_t>(i)]);
    }
    // Paths that differ from every parent.
    std::vector<std::string> paths;
    {
        std::vector<DiffPair> first = DiffMaps(parents[0], result);
        for (const DiffPair &p : first) {
            const std::string &path = p.b.mode ? p.b.path : p.a.path;
            bool all = true;
            for (int i = 1; i < num_parent && all; i++) {
                auto pi = parents[static_cast<size_t>(i)].find(path);
                auto ri = result.find(path);
                bool pe = pi != parents[static_cast<size_t>(i)].end(), re = ri != result.end();
                if (pe == re && (!pe || (pi->second.oid == ri->second.oid && pi->second.mode == ri->second.mode)))
                    all = false;
            }
            if (all) paths.push_back(path);
        }
    }
    num_paths = static_cast<int>(paths.size());
    for (const std::string &path : paths) {
        auto ri = result.find(path);
        uint32_t rmode = ri == result.end() ? 0 : ri->second.mode;
        Oid roid = ri == result.end() ? Oid{} : ri->second.oid;
        std::vector<uint32_t> pmode(static_cast<size_t>(num_parent), 0);
        std::vector<Oid> poid(static_cast<size_t>(num_parent));
        for (int i = 0; i < num_parent; i++) {
            auto pi = parents[static_cast<size_t>(i)].find(path);
            if (pi != parents[static_cast<size_t>(i)].end()) {
                pmode[static_cast<size_t>(i)] = pi->second.mode;
                poid[static_cast<size_t>(i)] = pi->second.oid;
            }
        }
        bool mode_differs = false;
        for (uint32_t m : pmode)
            if (m != rmode) mode_differs = true;
        bool result_deleted = rmode == 0;
        std::string rdata;
        if (!result_deleted) {
            if ((rmode & 0170000) == 0160000) rdata = "Subproject commit " + roid.Hex() + "\n";
            else repo.ReadBlob(roid, rdata);
        }
        std::vector<std::string> pdata(static_cast<size_t>(num_parent));
        bool binary = IsBinaryData(rdata);
        for (int i = 0; i < num_parent; i++) {
            size_t ui = static_cast<size_t>(i);
            if (!pmode[ui]) continue;
            if ((pmode[ui] & 0170000) == 0160000) pdata[ui] = "Subproject commit " + poid[ui].Hex() + "\n";
            else repo.ReadBlob(poid[ui], pdata[ui]);
            if (IsBinaryData(pdata[ui])) binary = true;
        }
        auto header = [&](bool file_header) {
            out += "diff --cc " + QuotePath(path) + "\n";
            out += "index ";
            for (int i = 0; i < num_parent; i++) {
                const Oid &o = poid[static_cast<size_t>(i)];
                if (i) out += ",";
                out += o.IsNull() ? std::string(static_cast<size_t>(repo.DefaultAbbrev()), '0') : repo.Abbrev(o);
            }
            out += ".." + (roid.IsNull() ? std::string(static_cast<size_t>(repo.DefaultAbbrev()), '0') : repo.Abbrev(roid)) + "\n";
            bool added = false, deleted = false;
            if (mode_differs) {
                deleted = rmode == 0;
                added = !deleted;
                for (int i = 0; added && i < num_parent; i++)
                    if (pmode[static_cast<size_t>(i)] != 0) added = false;
                char buf[32];
                if (added) {
                    std::snprintf(buf, sizeof buf, "%06o", rmode);
                    out += std::string("new file mode ") + buf;
                } else {
                    if (deleted) out += "deleted file ";
                    out += "mode ";
                    for (int i = 0; i < num_parent; i++) {
                        std::snprintf(buf, sizeof buf, "%s%06o", i ? "," : "", pmode[static_cast<size_t>(i)]);
                        out += buf;
                    }
                    if (rmode) {
                        std::snprintf(buf, sizeof buf, "..%06o", rmode);
                        out += buf;
                    }
                }
                out += "\n";
            }
            if (!file_header) return;
            out += added ? "--- /dev/null\n" : "--- " + QuotePath("a/" + path) + "\n";
            out += deleted ? "+++ /dev/null\n" : "+++ " + QuotePath("b/" + path) + "\n";
        };
        if (binary) {
            header(false);
            out += "Binary files differ\n";
            continue;
        }
        std::vector<std::string_view> rlines = SplitLines(rdata);
        unsigned long cnt = static_cast<unsigned long>(rlines.size());
        std::vector<SLine> sl(cnt + 2);
        for (unsigned long k = 0; k < cnt; k++) sl[k].text = StripNl(rlines[k]);
        for (SLine &s : sl) s.p_lno.assign(static_cast<size_t>(num_parent), 0);
        for (int n = 0; n < num_parent; n++) {
            size_t un = static_cast<size_t>(n);
            int same_as = -1;
            for (int j = 0; j < n; j++)
                if (poid[static_cast<size_t>(j)] == poid[un] && pmode[static_cast<size_t>(j)] == pmode[un]) {
                    same_as = j;
                    break;
                }
            unsigned long nmask = 1UL << n;
            if (same_as >= 0) {
                unsigned long jmask = 1UL << same_as;
                for (unsigned long lno = 0; lno <= cnt + 1; lno++) {
                    SLine &s = sl[lno];
                    s.p_lno[un] = s.p_lno[static_cast<size_t>(same_as)];
                    for (LostLine &ll : s.lost)
                        if (ll.parent_map & jmask) ll.parent_map |= nmask;
                    if (s.flag & jmask) s.flag |= nmask;
                }
                continue;
            }
            if (!result_deleted) {
                std::vector<std::string_view> plines = SplitLines(pdata[un]);
                for (const LineHunk &h : DiffLines(plines, rlines)) {
                    unsigned long nb = static_cast<unsigned long>(h.b_len ? h.b0 + 1 : h.b0);
                    unsigned long nn = static_cast<unsigned long>(h.b_len);
                    SLine *bucket = nn == 0 ? &sl[nb] : &sl[nb - 1];
                    for (long x = h.a0; x < h.a0 + h.a_len; x++)
                        bucket->plost.push_back({std::string(StripNl(plines[static_cast<size_t>(x)])), nmask});
                    for (long y = 0; y < h.b_len; y++) sl[static_cast<size_t>(h.b0 + y)].flag |= nmask;
                }
            }
            unsigned long p_lno = 1;
            unsigned long lno;
            for (lno = 0; lno <= cnt; lno++) {
                SLine &s = sl[lno];
                s.p_lno[un] = p_lno;
                if (!s.plost.empty()) CoalesceLines(s.lost, s.plost, n);
                for (const LostLine &ll : s.lost)
                    if (ll.parent_map & nmask) p_lno++;
                if (lno < cnt && !(s.flag & nmask)) p_lno++;
            }
            sl[lno].p_lno[un] = p_lno;
        }
        bool show_hunks = MakeHunks(sl, cnt, num_parent);
        if (show_hunks || mode_differs) {
            header(true);
            if (!result_deleted) DumpSLine(out, sl, cnt, num_parent);
        }
    }
    return out;
}

}  // namespace mep::git
