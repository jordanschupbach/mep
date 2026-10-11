// The H.264 deblocking filter (8.7) over a decoded picture: per macroblock
// in raster order, vertical edges then horizontal ones, luma then chroma,
// each edge segment filtered with the boundary strength of the 4x4 blocks
// on either side of it.

#include <cstdlib>

#include "h264_mb.h"

namespace h264::detail {

namespace {

inline int Clip3(int lo, int hi, int v) { return v < lo ? lo : (v > hi ? hi : v); }
inline uint8_t Clip1(int v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); }

inline bool MvFar(const int16_t *a, const int16_t *b) { return std::abs(a[0] - b[0]) >= 4 || std::abs(a[1] - b[1]) >= 4; }

// bS 1 or 0 from the motion of the 4x4 blocks on either side (8.7.2.1):
// different reference pictures, a different number of motion vectors, or
// motion vectors a whole sample or more apart.
int MotionBs(const MbInfo &p, int pb, const MbInfo &q, int qb) {
    const int p8 = ((pb >> 3) << 1) | ((pb & 3) >> 1), q8 = ((qb >> 3) << 1) | ((qb & 3) >> 1);
    const int pr0 = p.ref[0][p8] >= 0 ? p.ref_uid[0][p8] : -1, pr1 = p.ref[1][p8] >= 0 ? p.ref_uid[1][p8] : -1;
    const int qr0 = q.ref[0][q8] >= 0 ? q.ref_uid[0][q8] : -1, qr1 = q.ref[1][q8] >= 0 ? q.ref_uid[1][q8] : -1;
    const int pn = (pr0 >= 0) + (pr1 >= 0), qn = (qr0 >= 0) + (qr1 >= 0);
    if (pn != qn) return 1;
    if (pn == 1) {
        const int pl = pr0 >= 0 ? 0 : 1, ql = qr0 >= 0 ? 0 : 1;
        const int prf = pl == 0 ? pr0 : pr1, qrf = ql == 0 ? qr0 : qr1;
        if (prf != qrf) return 1;
        return MvFar(p.mv[pl][pb], q.mv[ql][qb]) ? 1 : 0;
    }
    if (pn == 0) return 0;  // (cannot happen for inter blocks)
    if (!((pr0 == qr0 && pr1 == qr1) || (pr0 == qr1 && pr1 == qr0))) return 1;
    const int16_t *p0 = p.mv[0][pb], *p1 = p.mv[1][pb], *q0 = q.mv[0][qb], *q1 = q.mv[1][qb];
    if (pr0 != pr1) {
        if (pr0 == qr0) return (MvFar(p0, q0) || MvFar(p1, q1)) ? 1 : 0;
        return (MvFar(p0, q1) || MvFar(p1, q0)) ? 1 : 0;
    }
    // Both motion vectors of each block use the same picture: either pairing may match.
    return ((MvFar(p0, q0) || MvFar(p1, q1)) && (MvFar(p0, q1) || MvFar(p1, q0))) ? 1 : 0;
}

int Bs(const MbInfo &p, int pb, const MbInfo &q, int qb, bool mb_edge) {
    if (p.intra || q.intra) return mb_edge ? 4 : 3;
    if (((q.coded >> qb) & 1) || ((p.coded >> pb) & 1)) return 2;
    return MotionBs(p, pb, q, qb);
}

// Filters `n` sample lines across one luma edge. `q` points at q0 of the
// first line; `across` steps from p0 to q0, `along` to the next line.
void FilterLuma(uint8_t *q, int across, int along, int n, int bs, int alpha, int beta, int tc0) {
    for (int i = 0; i < n; ++i, q += along) {
        const int p0 = q[-across], p1 = q[-2 * across], p2 = q[-3 * across];
        const int q0 = q[0], q1 = q[across], q2 = q[2 * across];
        if (std::abs(p0 - q0) >= alpha || std::abs(p1 - p0) >= beta || std::abs(q1 - q0) >= beta) continue;
        const int ap = std::abs(p2 - p0), aq = std::abs(q2 - q0);
        if (bs < 4) {
            const int tc = tc0 + (ap < beta) + (aq < beta);
            const int delta = Clip3(-tc, tc, (((q0 - p0) << 2) + (p1 - q1) + 4) >> 3);
            q[-across] = Clip1(p0 + delta);
            q[0] = Clip1(q0 - delta);
            if (ap < beta) q[-2 * across] = static_cast<uint8_t>(p1 + Clip3(-tc0, tc0, (p2 + ((p0 + q0 + 1) >> 1) - (p1 << 1)) >> 1));
            if (aq < beta) q[across] = static_cast<uint8_t>(q1 + Clip3(-tc0, tc0, (q2 + ((p0 + q0 + 1) >> 1) - (q1 << 1)) >> 1));
        } else {
            const int p3 = q[-4 * across], q3 = q[3 * across];
            const bool strong = std::abs(p0 - q0) < ((alpha >> 2) + 2);
            if (ap < beta && strong) {
                q[-across] = static_cast<uint8_t>((p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3);
                q[-2 * across] = static_cast<uint8_t>((p2 + p1 + p0 + q0 + 2) >> 2);
                q[-3 * across] = static_cast<uint8_t>((2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3);
            } else {
                q[-across] = static_cast<uint8_t>((2 * p1 + p0 + q1 + 2) >> 2);
            }
            if (aq < beta && strong) {
                q[0] = static_cast<uint8_t>((p1 + 2 * p0 + 2 * q0 + 2 * q1 + q2 + 4) >> 3);
                q[across] = static_cast<uint8_t>((p0 + q0 + q1 + q2 + 2) >> 2);
                q[2 * across] = static_cast<uint8_t>((2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3);
            } else {
                q[0] = static_cast<uint8_t>((2 * q1 + q0 + p1 + 2) >> 2);
            }
        }
    }
}

void FilterChroma(uint8_t *q, int across, int along, int n, int bs, int alpha, int beta, int tc0) {
    for (int i = 0; i < n; ++i, q += along) {
        const int p0 = q[-across], p1 = q[-2 * across];
        const int q0 = q[0], q1 = q[across];
        if (std::abs(p0 - q0) >= alpha || std::abs(p1 - p0) >= beta || std::abs(q1 - q0) >= beta) continue;
        if (bs < 4) {
            const int tc = tc0 + 1;
            const int delta = Clip3(-tc, tc, (((q0 - p0) << 2) + (p1 - q1) + 4) >> 3);
            q[-across] = Clip1(p0 + delta);
            q[0] = Clip1(q0 - delta);
        } else {
            q[-across] = static_cast<uint8_t>((2 * p1 + p0 + q1 + 2) >> 2);
            q[0] = static_cast<uint8_t>((2 * q1 + q0 + p1 + 2) >> 2);
        }
    }
}

void DeblockMb(Frame &f, int mbx, int mby) {
    const int wmbs = f.width / 16;
    const MbInfo &q = f.mbs[static_cast<size_t>(mby * wmbs + mbx)];
    if (q.slice_num < 0 || q.dbf_idc == 1) return;
    const MbInfo *left = mbx > 0 ? &f.mbs[static_cast<size_t>(mby * wmbs + mbx - 1)] : nullptr;
    const MbInfo *top = mby > 0 ? &f.mbs[static_cast<size_t>((mby - 1) * wmbs + mbx)] : nullptr;
    if (left && (left->slice_num < 0 || (q.dbf_idc == 2 && left->slice_num != q.slice_num))) left = nullptr;
    if (top && (top->slice_num < 0 || (q.dbf_idc == 2 && top->slice_num != q.slice_num))) top = nullptr;
    const int W = f.width, cw = W / 2;
    for (int dir = 0; dir < 2; ++dir) {
        const MbInfo *outside = dir == 0 ? left : top;
        for (int e = 0; e < 4; ++e) {
            if (e == 0 && !outside) continue;
            const MbInfo &p = e == 0 ? *outside : q;
            int bs[4];
            bool any = false;
            for (int k = 0; k < 4; ++k) {
                // Vertical edges (dir 0) run down column e; horizontal ones along row e.
                const int qb = dir == 0 ? k * 4 + e : e * 4 + k;
                const int pb = dir == 0 ? (e == 0 ? k * 4 + 3 : k * 4 + e - 1) : (e == 0 ? 12 + k : (e - 1) * 4 + k);
                bs[k] = Bs(p, pb, q, qb, e == 0);
                any |= bs[k] != 0;
            }
            if (!any) continue;
            const bool luma_edge = !(q.t8 && (e & 1));
            if (luma_edge) {
                const int qp_av = (p.qp + q.qp + 1) >> 1;
                const int index_a = Clip3(0, 51, qp_av + q.alpha_off), index_b = Clip3(0, 51, qp_av + q.beta_off);
                const int alpha = kAlpha[index_a], beta = kBeta[index_b];
                for (int k = 0; k < 4; ++k) {
                    if (bs[k] == 0) continue;
                    uint8_t *qs = dir == 0 ? &f.y[static_cast<size_t>((mby * 16 + k * 4) * W + mbx * 16 + e * 4)]
                                           : &f.y[static_cast<size_t>((mby * 16 + e * 4) * W + mbx * 16 + k * 4)];
                    FilterLuma(qs, dir == 0 ? 1 : W, dir == 0 ? W : 1, 4, bs[k], alpha, beta, bs[k] < 4 ? kTc0[index_a][bs[k] - 1] : 0);
                }
            }
            if (e & 1) continue;  // 4:2:0 chroma has edges at chroma 0 and 4 only
            for (int c = 0; c < 2; ++c) {
                const int qp_av = (p.qpc[c] + q.qpc[c] + 1) >> 1;
                const int index_a = Clip3(0, 51, qp_av + q.alpha_off), index_b = Clip3(0, 51, qp_av + q.beta_off);
                const int alpha = kAlpha[index_a], beta = kBeta[index_b];
                uint8_t *plane = c == 0 ? f.u.data() : f.v.data();
                for (int k = 0; k < 4; ++k) {
                    if (bs[k] == 0) continue;
                    uint8_t *qs = dir == 0 ? &plane[static_cast<size_t>((mby * 8 + k * 2) * cw + mbx * 8 + e * 2)]
                                           : &plane[static_cast<size_t>((mby * 8 + e * 2) * cw + mbx * 8 + k * 2)];
                    FilterChroma(qs, dir == 0 ? 1 : cw, dir == 0 ? cw : 1, 2, bs[k], alpha, beta, bs[k] < 4 ? kTc0[index_a][bs[k] - 1] : 0);
                }
            }
        }
    }
}

}  // namespace

void DeblockPicture(Frame &f, const Pps &pps) {
    (void)pps;
    const int wmbs = f.width / 16, hmbs = f.height / 16;
    for (int y = 0; y < hmbs; ++y)
        for (int x = 0; x < wmbs; ++x) DeblockMb(f, x, y);
}

}  // namespace h264::detail
