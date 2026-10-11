// CABAC slice data for the H.264 decoder (7.3.4, 7.3.5, 9.3): the
// binarizations of the macroblock-layer syntax elements and the context
// index increments that pick their context variables from the neighbouring
// macroblocks and blocks. The arithmetic engine is h264_cabac.cpp's.

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "h264_cabac.h"
#include "h264_mb.h"

namespace h264::detail {

namespace {

// ctxIdxInc of significant_coeff_flag / last_significant_coeff_flag for
// frame-coded 8x8 blocks (Table 9-43), by coefficient index.
const uint8_t kSig8x8[63] = {
    0, 1, 2, 3, 4,  5,  5,  4,  4, 3, 3,  4,  4,  4,  5,  5,  4,  4,  4,  4,  3,  3,  6, 7, 7, 7, 8,  9,  10, 9,  8,  7,
    7, 6, 11, 12, 13, 11, 6, 7, 8, 9, 14, 10, 9,  8,  6,  11, 12, 13, 11, 6,  9,  14, 10, 9, 11, 12, 13, 11, 14, 10, 12,
};
const uint8_t kLast8x8[63] = {
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8,
};

// ctxBlockCat: 0 Intra16x16 DC, 1 Intra16x16 AC, 2 luma 4x4, 3 chroma DC,
// 4 chroma AC, 5 luma 8x8. Context offsets per category (Table 9-40).
const int kCbfBase[6] = {85, 89, 93, 97, 101, 1012};
const int kSigBase[6] = {105 + 0, 105 + 15, 105 + 29, 105 + 44, 105 + 47, 402};
const int kLastBase[6] = {166 + 0, 166 + 15, 166 + 29, 166 + 44, 166 + 47, 417};
const int kAbsBase[6] = {227 + 0, 227 + 10, 227 + 20, 227 + 30, 227 + 39, 426};

}  // namespace

// Parses CABAC macroblocks for an MbDecoder (a friend, for its
// neighbours and the macroblock being decoded).
class CabacMbParser {
public:
    explicit CabacMbParser(MbDecoder &m) : m_(m), c_(*m.cabac_) {}

    bool SkipFlag() {
        const bool b = m_.ctx_->sh->type == kSliceB;
        int inc = 0;
        if (const MbInfo *a = m_.Neighbor(-1, 0); a && !a->skip) ++inc;
        if (const MbInfo *t = m_.Neighbor(0, -1); t && !t->skip) ++inc;
        return c_.Decision((b ? 24 : 11) + inc) != 0;
    }

    bool Parse(MbData *d);
    bool last_dqp_nonzero = false;

private:
    // The neighbouring block covering luma sample (x, y) relative to the
    // macroblock (x or y may be -1): its macroblock and raster 4x4 index;
    // null when unavailable.
    const MbInfo *Block(int x, int y, int *blk) const {
        const MbInfo *mb = nullptr;
        if (x < 0) mb = m_.Neighbor(-1, 0);
        else if (y < 0) mb = m_.Neighbor(0, -1);
        else mb = m_.cur_;
        *blk = ((y & 15) >> 2) * 4 + ((x & 15) >> 2);
        return mb;
    }
    int IntraMbType(int base, bool intra_slice);
    int ChromaPredMode();
    int RefIdx(int list, int x, int y);
    int Mvd(int list, int comp, int x, int y);
    int Cbp();
    int QpDelta();
    int Cbf(int cat, int c, int bx, int by);
    int ResidualBlock(int cat, int cbf_ctx, int max_coeff, int16_t *out, const uint8_t *scan, int start);
    bool Residual(MbData *d);
    bool Transform8x8Flag() {
        int inc = 0;
        if (const MbInfo *a = m_.Neighbor(-1, 0); a && a->t8) ++inc;
        if (const MbInfo *t = m_.Neighbor(0, -1); t && t->t8) ++inc;
        return c_.Decision(399 + inc) != 0;
    }

    MbDecoder &m_;
    Cabac &c_;
};

// mb_type of an intra macroblock (Table 9-36), prefix context `base`
// (3 in I slices, 17/32 for the suffix in P/B slices).
int CabacMbParser::IntraMbType(int base, bool intra_slice) {
    if (intra_slice) {
        int inc = 0;
        if (const MbInfo *a = m_.Neighbor(-1, 0); a && !a->i4) ++inc;
        if (const MbInfo *t = m_.Neighbor(0, -1); t && !t->i4) ++inc;
        if (c_.Decision(base + inc) == 0) return 0;
        base += 2;
    } else {
        if (c_.Decision(base) == 0) return 0;
    }
    if (c_.Terminate()) return 25;  // I_PCM
    const int s = intra_slice ? 1 : 0;
    int t = 1 + 12 * c_.Decision(base + 1);
    if (c_.Decision(base + 2)) t += 4 + 4 * c_.Decision(base + 2 + s);
    t += 2 * c_.Decision(base + 3 + s);
    t += c_.Decision(base + 3 + 2 * s);
    return t;
}

int CabacMbParser::ChromaPredMode() {
    int inc = 0;
    if (const MbInfo *a = m_.Neighbor(-1, 0); a && a->intra && !a->pcm && a->chroma_pred != 0) ++inc;
    if (const MbInfo *t = m_.Neighbor(0, -1); t && t->intra && !t->pcm && t->chroma_pred != 0) ++inc;
    if (c_.Decision(64 + inc) == 0) return 0;
    if (c_.Decision(67) == 0) return 1;
    return c_.Decision(67) == 0 ? 2 : 3;
}

// ref_idx_lX for the partition whose top-left 4x4 block is at (x, y).
int CabacMbParser::RefIdx(int list, int x, int y) {
    auto cond = [&](int nx, int ny) {
        int blk = 0;
        const MbInfo *n = Block(nx, ny, &blk);
        if (!n || n->intra || n->skip) return 0;
        const int b8 = (blk >> 3) * 2 + ((blk & 3) >> 1);
        if (n->direct16 || n->sub_direct[b8]) return 0;
        return n->ref[list][b8] > 0 ? 1 : 0;
    };
    int ctx = cond(x - 1, y) + 2 * cond(x, y - 1);
    int ref = 0;
    while (c_.Decision(54 + ctx)) {
        ++ref;
        ctx = (ctx >> 2) + 4;
        if (ref >= 32) return -1;
    }
    return ref;
}

// One component of mvd_lX for the (sub-)partition whose top-left 4x4
// block is at (x, y): UEG3, signed, prefix cMax 9.
int CabacMbParser::Mvd(int list, int comp, int x, int y) {
    int sum = 0;
    int blk = 0;
    if (const MbInfo *a = Block(x - 1, y, &blk)) sum += a->mvd[list][blk][comp];
    if (const MbInfo *b = Block(x, y - 1, &blk)) sum += b->mvd[list][blk][comp];
    const int base = comp == 0 ? 40 : 47;
    if (!c_.Decision(base + (sum > 2 ? 1 : 0) + (sum > 32 ? 1 : 0))) return 0;
    int v = 1;
    int ctx = base + 3;
    while (v < 9 && c_.Decision(ctx)) {
        if (v < 4) ++ctx;
        ++v;
    }
    if (v >= 9) {
        int k = 3;
        while (c_.Bypass()) {
            v += 1 << k;
            if (++k > 24) return 0;
        }
        while (k--) v += c_.Bypass() << k;
    }
    return c_.Bypass() ? -v : v;
}

// coded_block_pattern: four luma bits (FL, each with its own context) and
// the chroma part (TU, cMax 2) (9.3.3.1.1.4).
int CabacMbParser::Cbp() {
    const MbInfo *a = m_.Neighbor(-1, 0), *t = m_.Neighbor(0, -1);
    // condTermFlagN is 1 when the neighbouring 8x8 block coded no luma.
    auto outside = [](const MbInfo *n, int b8) {
        if (!n || n->pcm) return 0;
        if (!n->skip && ((n->cbp >> b8) & 1)) return 0;
        return 1;
    };
    int cbp = 0;
    for (int b8 = 0; b8 < 4; ++b8) {
        const int x8 = b8 & 1, y8 = b8 >> 1;
        const int ca = x8 ? !((cbp >> (b8 - 1)) & 1) : outside(a, b8 + 1);
        const int cb = y8 ? !((cbp >> (b8 - 2)) & 1) : outside(t, b8 + 2);
        cbp |= c_.Decision(73 + ca + 2 * cb) << b8;
    }
    auto chroma = [](const MbInfo *n) { return !n ? 0 : (n->pcm ? 2 : (n->skip ? 0 : (n->cbp >> 4))); };
    const int ca = chroma(a), cb = chroma(t);
    if (c_.Decision(77 + (ca > 0) + 2 * (cb > 0)) == 0) return cbp;
    return cbp | ((1 + c_.Decision(77 + 4 + (ca == 2) + 2 * (cb == 2))) << 4);
}

int CabacMbParser::QpDelta() {
    if (!c_.Decision(60 + (last_dqp_nonzero ? 1 : 0))) return 0;
    int k = 1;
    int ctx = 62;
    while (c_.Decision(ctx)) {
        ctx = 63;
        if (++k > 52) return 0;
    }
    return (k & 1) ? (k + 1) / 2 : -(k / 2);
}

// coded_block_flag's ctxIdxInc (9.3.3.1.1.9) for block (bx, by) of
// category `cat` (chroma component `c` for 3 and 4).
int CabacMbParser::Cbf(int cat, int c, int bx, int by) {
    const int unavailable = m_.cur_->intra ? 1 : 0;
    auto cond = [&](bool left) -> int {
        if (cat == 0 || cat == 3) {
            const MbInfo *n = left ? m_.Neighbor(-1, 0) : m_.Neighbor(0, -1);
            if (!n) return unavailable;
            return (n->cbf_dc >> (cat == 0 ? 0 : 1 + c)) & 1;
        }
        if (cat == 4) {
            const MbInfo *n = nullptr;
            int idx = 0;
            if (left) {
                if (bx > 0) {
                    n = m_.cur_;
                    idx = by * 2;
                } else {
                    n = m_.Neighbor(-1, 0);
                    idx = by * 2 + 1;
                }
            } else if (by > 0) {
                n = m_.cur_;
                idx = bx;
            } else {
                n = m_.Neighbor(0, -1);
                idx = 2 + bx;
            }
            if (!n) return unavailable;
            return n->nzc[c][idx] > 0 ? 1 : 0;
        }
        int blk = 0;
        const MbInfo *n = left ? Block(bx * 4 - 1, by * 4, &blk) : Block(bx * 4, by * 4 - 1, &blk);
        if (!n) return unavailable;
        return n->nz[blk] > 0 ? 1 : 0;
    };
    return kCbfBase[cat] + cond(true) + 2 * cond(false);
}

// residual_block_cabac (7.3.5.3.3): returns the number of nonzero
// coefficients (0 when coded_block_flag is 0), levels written through
// `scan` (from index `start`) into `out`. `cbf_ctx` < 0: no
// coded_block_flag (an 8x8 block in 4:2:0).
int CabacMbParser::ResidualBlock(int cat, int cbf_ctx, int max_coeff, int16_t *out, const uint8_t *scan, int start) {
    if (cbf_ctx >= 0 && !c_.Decision(cbf_ctx)) return 0;
    int idx[64];
    int count = 0;
    const int sig = kSigBase[cat], last = kLastBase[cat];
    int i = 0;
    for (; i < max_coeff - 1; ++i) {
        const int inc = cat == 5 ? kSig8x8[i] : (cat == 3 ? std::min(i, 2) : i);
        if (c_.Decision(sig + inc)) {
            idx[count++] = i;
            const int linc = cat == 5 ? kLast8x8[i] : (cat == 3 ? std::min(i, 2) : i);
            if (c_.Decision(last + linc)) break;
        }
    }
    if (i == max_coeff - 1) idx[count++] = max_coeff - 1;
    const int abs_base = kAbsBase[cat];
    int eq1 = 0, gt1 = 0;
    for (int k = count - 1; k >= 0; --k) {
        int abs = 1;
        if (c_.Decision(abs_base + (gt1 != 0 ? 0 : std::min(4, 1 + eq1)))) {
            const int ctx = abs_base + 5 + std::min(4 - (cat == 3 ? 1 : 0), gt1);
            abs = 2;
            while (abs < 15 && c_.Decision(ctx)) ++abs;
            if (abs >= 15) {
                int j = 0;
                while (c_.Bypass()) {
                    if (++j > 23) return -1;
                }
                int v = 1;
                while (j--) v = 2 * v + c_.Bypass();
                abs = v + 14;
            }
        }
        if (abs == 1) ++eq1;
        else ++gt1;
        out[scan[start + idx[k]]] = static_cast<int16_t>(c_.Bypass() ? -abs : abs);
    }
    return count;
}

bool CabacMbParser::Residual(MbData *d) {
    MbInfo &m = *m_.cur_;
    static const uint8_t kRaster[4] = {0, 1, 2, 3};
    static const uint8_t kBlkX[16] = {0, 1, 0, 1, 2, 3, 2, 3, 0, 1, 0, 1, 2, 3, 2, 3};
    static const uint8_t kBlkY[16] = {0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 3, 3, 2, 2, 3, 3};
    if (d->kind == MbData::kI16) {
        std::memset(d->dc_luma, 0, sizeof(d->dc_luma));
        const int n = ResidualBlock(0, Cbf(0, 0, 0, 0), 16, d->dc_luma, kZigzag4x4, 0);
        if (n < 0) return false;
        d->has_dc_luma = n > 0;
        if (n > 0) m.cbf_dc |= 1;
    }
    for (int b = 0; b < 16; ++b) {
        const int b8 = b >> 2;
        if (!(d->cbp & (1 << b8))) continue;
        const int bx = kBlkX[b], by = kBlkY[b], blk = by * 4 + bx;
        if (d->t8) {
            if ((b & 3) != 0) continue;
            std::memset(d->coef8[b8], 0, sizeof(d->coef8[b8]));
            const int n = ResidualBlock(5, -1, 64, d->coef8[b8], kZigzag8x8, 0);
            if (n < 0) return false;
            if (n > 0) d->coded8 |= static_cast<uint8_t>(1 << b8);
            for (int k = 0; k < 4; ++k) m.nz[(by + (k >> 1)) * 4 + bx + (k & 1)] = static_cast<uint8_t>(n);
            continue;
        }
        std::memset(d->coef[blk], 0, sizeof(d->coef[blk]));
        const bool ac = d->kind == MbData::kI16;
        const int n = ResidualBlock(ac ? 1 : 2, Cbf(ac ? 1 : 2, 0, bx, by), ac ? 15 : 16, d->coef[blk], kZigzag4x4, ac ? 1 : 0);
        if (n < 0) return false;
        m.nz[blk] = static_cast<uint8_t>(n);
        if (n > 0) d->coded4 |= static_cast<uint16_t>(1 << blk);
    }
    if (d->cbp & 0x30) {
        for (int c = 0; c < 2; ++c) {
            std::memset(d->dc_chroma[c], 0, sizeof(d->dc_chroma[c]));
            const int n = ResidualBlock(3, Cbf(3, c, 0, 0), 4, d->dc_chroma[c], kRaster, 0);
            if (n < 0) return false;
            d->has_dc_chroma[c] = n > 0;
            if (n > 0) m.cbf_dc = static_cast<uint8_t>(m.cbf_dc | (2 << c));
        }
    }
    if (d->cbp & 0x20) {
        for (int c = 0; c < 2; ++c)
            for (int b = 0; b < 4; ++b) {
                std::memset(d->ac_chroma[c][b], 0, sizeof(d->ac_chroma[c][b]));
                const int n = ResidualBlock(4, Cbf(4, c, b & 1, b >> 1), 15, d->ac_chroma[c][b], kZigzag4x4, 1);
                if (n < 0) return false;
                m.nzc[c][b] = static_cast<uint8_t>(n);
                if (n > 0) d->coded_chroma[c] |= static_cast<uint8_t>(1 << b);
            }
    }
    return !c_.Overrun();
}

bool CabacMbParser::Parse(MbData *d) {
    MbInfo &cur = *m_.cur_;
    const SliceHeader &sh = *m_.ctx_->sh;
    const Pps &pps = *m_.ctx_->pps;
    // mb_type (Tables 9-36, 9-37).
    if (sh.type == kSliceI) {
        SetIntraMbType(IntraMbType(3, true), d);
    } else if (sh.type == kSliceP) {
        if (c_.Decision(14) == 0) {
            int t = 0;
            if (c_.Decision(15) == 0) t = 3 * c_.Decision(16);
            else t = 2 - c_.Decision(17);
            SetInterMbType(kSliceP, t, d);
        } else {
            SetIntraMbType(IntraMbType(17, false), d);
        }
    } else {
        int inc = 0;
        if (const MbInfo *a = m_.Neighbor(-1, 0); a && !a->direct16) ++inc;
        if (const MbInfo *t = m_.Neighbor(0, -1); t && !t->direct16) ++inc;
        int t = 0;
        bool intra = false;
        if (!c_.Decision(27 + inc)) {
            t = 0;
        } else if (!c_.Decision(27 + 3)) {
            t = 1 + c_.Decision(27 + 5);
        } else {
            int bits = c_.Decision(27 + 4) << 3;
            bits |= c_.Decision(27 + 5) << 2;
            bits |= c_.Decision(27 + 5) << 1;
            bits |= c_.Decision(27 + 5);
            if (bits < 8) t = bits + 3;
            else if (bits == 13) intra = true;
            else if (bits == 14) t = 11;
            else if (bits == 15) t = 22;
            else t = ((bits << 1) | c_.Decision(27 + 5)) - 4;
        }
        if (intra) SetIntraMbType(IntraMbType(32, false), d);
        else SetInterMbType(kSliceB, t, d);
    }
    if (d->kind == MbData::kPcm) {
        // After the terminating bin: alignment, then the samples, after
        // which the engine starts afresh (9.3.1.2).
        const size_t bit = c_.BitPosition();
        const size_t byte = (bit + 7) / 8;
        const uint8_t *base = m_.cabac_data_;
        if (byte + 384 > m_.cabac_len_) return false;
        std::memcpy(d->pcm, base + byte, 384);
        m_.cabac_data_ = base + byte + 384;
        m_.cabac_len_ -= byte + 384;
        c_.Restart(m_.cabac_data_, m_.cabac_len_);
        last_dqp_nonzero = false;
        return true;
    }
    if (d->kind == MbData::kI4) {
        if (pps.transform_8x8_mode && Transform8x8Flag()) {
            d->kind = MbData::kI8;
            d->t8 = true;
        }
        const int n = d->kind == MbData::kI8 ? 4 : 16;
        for (int i = 0; i < n; ++i) {
            d->prev_pred_flag[i] = c_.Decision(68) != 0;
            if (!d->prev_pred_flag[i]) {
                int r = c_.Decision(69);
                r |= c_.Decision(69) << 1;
                r |= c_.Decision(69) << 2;
                d->rem_pred_mode[i] = static_cast<int8_t>(r);
            }
        }
    }
    if (!d->inter) {
        d->chroma_mode = ChromaPredMode();
    } else if (d->kind == MbData::kInter) {
        if (d->part == kPart8x8) {
            for (int i = 0; i < 4; ++i) {
                int t = 0;
                if (sh.type == kSliceP) {
                    if (c_.Decision(21)) t = 0;
                    else if (!c_.Decision(22)) t = 1;
                    else t = c_.Decision(23) ? 2 : 3;
                } else if (!c_.Decision(36)) {
                    t = 0;
                } else if (!c_.Decision(37)) {
                    t = 1 + c_.Decision(39);
                } else {
                    t = 3;
                    bool done = false;
                    if (c_.Decision(38)) {
                        if (c_.Decision(39)) {
                            t = 11 + c_.Decision(39);
                            done = true;
                        } else {
                            t += 4;
                        }
                    }
                    if (!done) {
                        t += 2 * c_.Decision(39);
                        t += c_.Decision(39);
                    }
                }
                SetSubMbType(sh.type, i, t, d);
                cur.sub_direct[i] = d->sub_direct[i];
            }
            for (int list = 0; list < 2; ++list)
                for (int i = 0; i < 4; ++i) {
                    d->ref[list][i] = -1;
                    if (d->sub_direct[i] || !(d->sub_pred[i] & (1 << list))) continue;
                    d->ref[list][i] = sh.num_ref_idx[list] > 1 ? RefIdx(list, (i & 1) * 8, (i >> 1) * 8) : 0;
                    if (d->ref[list][i] < 0 || d->ref[list][i] >= sh.num_ref_idx[list]) return false;
                    cur.ref[list][i] = static_cast<int8_t>(d->ref[list][i]);
                }
            for (int list = 0; list < 2; ++list)
                for (int i = 0; i < 4; ++i) {
                    if (d->sub_direct[i] || !(d->sub_pred[i] & (1 << list))) continue;
                    const int shape = d->sub_shape[i];
                    const int sw = (shape == kSub8x8 || shape == kSub8x4) ? 8 : 4;
                    const int shh = (shape == kSub8x8 || shape == kSub4x8) ? 8 : 4;
                    for (int s = 0; s < SubPartCount(shape); ++s) {
                        const int x = (i & 1) * 8 + (sw == 8 ? 0 : (s & 1) * 4);
                        const int y = (i >> 1) * 8 + (shh == 8 ? 0 : (sw == 8 ? s * 4 : (s >> 1) * 4));
                        for (int comp = 0; comp < 2; ++comp) {
                            const int v = Mvd(list, comp, x, y);
                            d->mvd[list][i * 4 + s][comp] = static_cast<int16_t>(v);
                            const uint8_t a = static_cast<uint8_t>(std::min(std::abs(v), 255));
                            for (int by = y >> 2; by < (y + shh) >> 2; ++by)
                                for (int bx = x >> 2; bx < (x + sw) >> 2; ++bx) cur.mvd[list][by * 4 + bx][comp] = a;
                        }
                    }
                }
        } else {
            const int parts = d->part == kPart16x16 ? 1 : 2;
            auto geometry = [&](int p, int *x, int *y, int *w, int *h) {
                *x = 0;
                *y = 0;
                *w = 16;
                *h = 16;
                if (d->part == kPart16x8) {
                    *y = p * 8;
                    *h = 8;
                } else if (d->part == kPart8x16) {
                    *x = p * 8;
                    *w = 8;
                }
            };
            for (int list = 0; list < 2; ++list)
                for (int p = 0; p < parts; ++p) {
                    d->ref[list][p] = -1;
                    if (!(d->part_pred[p] & (1 << list))) continue;
                    int x, y, w, h;
                    geometry(p, &x, &y, &w, &h);
                    d->ref[list][p] = sh.num_ref_idx[list] > 1 ? RefIdx(list, x, y) : 0;
                    if (d->ref[list][p] < 0 || d->ref[list][p] >= sh.num_ref_idx[list]) return false;
                    // The partition's 8x8 blocks (16x16: all; 16x8: a row; 8x16: a column).
                    const int mask = d->part == kPart16x16 ? 0xf : (d->part == kPart16x8 ? (p ? 0xc : 0x3) : (p ? 0xa : 0x5));
                    for (int b8 = 0; b8 < 4; ++b8)
                        if (mask & (1 << b8)) cur.ref[list][b8] = static_cast<int8_t>(d->ref[list][p]);
                }
            for (int list = 0; list < 2; ++list)
                for (int p = 0; p < parts; ++p) {
                    if (!(d->part_pred[p] & (1 << list))) continue;
                    int x, y, w, h;
                    geometry(p, &x, &y, &w, &h);
                    for (int comp = 0; comp < 2; ++comp) {
                        const int v = Mvd(list, comp, x, y);
                        d->mvd[list][p][comp] = static_cast<int16_t>(v);
                        const uint8_t a = static_cast<uint8_t>(std::min(std::abs(v), 255));
                        for (int by = y >> 2; by < (y + h) >> 2; ++by)
                            for (int bx = x >> 2; bx < (x + w) >> 2; ++bx) cur.mvd[list][by * 4 + bx][comp] = a;
                    }
                }
        }
    }
    if (d->kind != MbData::kI16) {
        d->cbp = static_cast<uint8_t>(Cbp());
        if ((d->cbp & 15) && pps.transform_8x8_mode && d->kind != MbData::kI4 && d->kind != MbData::kI8 && m_.Allows8x8Transform(*d))
            d->t8 = Transform8x8Flag();
    }
    // (Decoded before the residual so coded_block_flag contexts can tell intra from inter.)
    cur.intra = !d->inter;
    if ((d->cbp & 0x3f) || d->kind == MbData::kI16) {
        d->qp_delta = QpDelta();
        last_dqp_nonzero = d->qp_delta != 0;
        if (d->qp_delta < -26 || d->qp_delta > 25) return false;
    } else {
        last_dqp_nonzero = false;
    }
    m_.SetQp(d);
    return Residual(d);
}

bool MbDecoder::DecodeSliceCabac(BitReader &br, int addr, std::string *error) {
    const SliceHeader &sh = *ctx_->sh;
    while (!br.ByteAligned()) br.U1();  // cabac_alignment_one_bit
    const size_t byte = br.BitPos() / 8;
    if (byte >= br.Size()) {
        *error = "empty CABAC slice";
        return false;
    }
    if (!cabac_) cabac_ = new Cabac;
    cabac_data_ = br.Data() + byte;
    cabac_len_ = br.Size() - byte;
    cabac_->Start(cabac_data_, cabac_len_, sh.type, sh.cabac_init_idc, sh.qp);
    CabacMbParser parser(*this);
    const int total = width_mbs_ * height_mbs_;
    for (;;) {
        StartMb(addr);
        data_.Reset();
        if (sh.type != kSliceI && parser.SkipFlag()) {
            data_.kind = sh.type == kSliceP ? MbData::kPSkip : MbData::kBSkip;
            data_.inter = true;
            SetQp(&data_);
            parser.last_dqp_nonzero = false;
        } else if (!parser.Parse(&data_)) {
            cur_->slice_num = -1;
            *error = "damaged macroblock";
            return false;
        }
        Reconstruct(data_);
        if (cabac_->Overrun()) {
            *error = "CABAC slice data overrun";
            return false;
        }
        ++addr;
        if (cabac_->Terminate()) break;  // end_of_slice_flag
        if (addr >= total) {
            *error = "slice runs past the end of the picture";
            return false;
        }
    }
    return true;
}

}  // namespace h264::detail
