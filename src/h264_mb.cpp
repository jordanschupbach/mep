// Macroblock reconstruction for the H.264 decoder (h264_mb.h): the slice
// data loop, intra prediction (8.3), motion vector prediction and the
// direct modes (8.4.1), motion compensation with weighted prediction
// (8.4.2), and scaling plus the inverse transforms (8.5).

#include "h264_mb.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "h264_cabac.h"

namespace h264::detail {

namespace {

inline int Clip3(int lo, int hi, int v) { return v < lo ? lo : (v > hi ? hi : v); }
inline uint8_t Clip1(int v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); }
inline int Median(int a, int b, int c) { return std::max(std::min(a, b), std::min(std::max(a, b), c)); }
inline int MinPositive(int a, int b) { return (a >= 0 && b >= 0) ? std::min(a, b) : std::max(a, b); }

// luma4x4BlkIdx (z-order) <-> raster position in the macroblock's 4x4 grid.
const uint8_t kBlkX[16] = {0, 1, 0, 1, 2, 3, 2, 3, 0, 1, 0, 1, 2, 3, 2, 3};
const uint8_t kBlkY[16] = {0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 3, 3, 2, 2, 3, 3};
const uint8_t kRasterToBlk[16] = {0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15};

// 8.5.12.2: the 4x4 inverse transform of `c` (raster), added to `dst`.
void Idct4Add(const int *c, uint8_t *dst, int stride) {
    int t[16];
    for (int i = 0; i < 4; ++i) {
        const int *r = c + i * 4;
        const int e = r[0] + r[2], f = r[0] - r[2];
        const int g = (r[1] >> 1) - r[3], h = r[1] + (r[3] >> 1);
        t[i * 4 + 0] = e + h;
        t[i * 4 + 1] = f + g;
        t[i * 4 + 2] = f - g;
        t[i * 4 + 3] = e - h;
    }
    for (int j = 0; j < 4; ++j) {
        const int e = t[j] + t[8 + j], f = t[j] - t[8 + j];
        const int g = (t[4 + j] >> 1) - t[12 + j], h = t[4 + j] + (t[12 + j] >> 1);
        const int out[4] = {e + h, f + g, f - g, e - h};
        for (int i = 0; i < 4; ++i) {
            uint8_t &p = dst[i * stride + j];
            p = Clip1(p + ((out[i] + 32) >> 6));
        }
    }
}

// 8.5.13.2: the 8x8 inverse transform of `c` (raster), added to `dst`.
void Idct8Add(const int *c, uint8_t *dst, int stride) {
    int t[64];
    auto one = [](const int *d, int step, int *o, int ostep) {
        const int a0 = d[0] + d[4 * step], a4 = d[0] - d[4 * step];
        const int a2 = (d[2 * step] >> 1) - d[6 * step], a6 = d[2 * step] + (d[6 * step] >> 1);
        const int b0 = a0 + a6, b2 = a4 + a2, b4 = a4 - a2, b6 = a0 - a6;
        const int d1 = d[step], d3 = d[3 * step], d5 = d[5 * step], d7 = d[7 * step];
        const int a1 = -d3 + d5 - d7 - (d7 >> 1);
        const int a3 = d1 + d7 - d3 - (d3 >> 1);
        const int a5 = -d1 + d7 + d5 + (d5 >> 1);
        const int a7 = d3 + d5 + d1 + (d1 >> 1);
        const int b1 = a1 + (a7 >> 2), b7 = a7 - (a1 >> 2);
        const int b3 = a3 + (a5 >> 2), b5 = (a3 >> 2) - a5;
        o[0] = b0 + b7;
        o[ostep] = b2 + b5;
        o[2 * ostep] = b4 + b3;
        o[3 * ostep] = b6 + b1;
        o[4 * ostep] = b6 - b1;
        o[5 * ostep] = b4 - b3;
        o[6 * ostep] = b2 - b5;
        o[7 * ostep] = b0 - b7;
    };
    for (int i = 0; i < 8; ++i) one(c + i * 8, 1, t + i * 8, 1);
    int u[64];
    for (int j = 0; j < 8; ++j) one(t + j, 8, u + j, 8);
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 8; ++j) {
            uint8_t &p = dst[i * stride + j];
            p = Clip1(p + ((u[i * 8 + j] + 32) >> 6));
        }
}

// Coefficients are kept within what a conforming stream can produce, so a
// corrupt one can't overflow the transforms.
inline int ClampCoef(int64_t v) { return static_cast<int>(v < -(1 << 22) ? -(1 << 22) : (v > (1 << 22) ? (1 << 22) : v)); }

// Scales one 4x4 coefficient (8.5.12.1, not a DC handled separately).
inline int Scale4(int c, int level_scale, int qp) {
    const int64_t v = int64_t{c} * level_scale;
    if (qp >= 24) return ClampCoef(v * (int64_t{1} << (qp / 6 - 4)));
    return ClampCoef((v + (1 << (3 - qp / 6))) >> (4 - qp / 6));
}
inline int Scale8(int c, int level_scale, int qp) {
    const int64_t v = int64_t{c} * level_scale;
    if (qp >= 36) return ClampCoef(v * (int64_t{1} << (qp / 6 - 6)));
    return ClampCoef((v + (1 << (5 - qp / 6))) >> (6 - qp / 6));
}

}  // namespace

MbDecoder::MbDecoder() = default;
MbDecoder::~MbDecoder() { delete cabac_; }

// --- The slice data loop (7.3.4) --------------------------------------------------

bool MbDecoder::DecodeSlice(const SliceContext &ctx, BitReader &br, std::string *error) {
    ctx_ = &ctx;
    frame_ = ctx.cur;
    mbs_ = frame_->mbs.data();
    width_mbs_ = ctx.sps->width_mbs;
    height_mbs_ = ctx.sps->height_mbs;
    slice_num_ = ctx.slice_num;
    const SliceHeader &sh = *ctx.sh;
    prev_qp_ = sh.qp;
    // LevelScale4x4/8x8 = weightScale * normAdjust (8.5.9), from the
    // effective scaling lists.
    for (int list = 0; list < 6; ++list)
        for (int m = 0; m < 6; ++m)
            for (int p = 0; p < 16; ++p) {
                const int i = p >> 2, j = p & 3;
                const int v = (i % 2 == 0 && j % 2 == 0) ? kNormAdjust4x4[m][0] : ((i % 2 == 1 && j % 2 == 1) ? kNormAdjust4x4[m][1] : kNormAdjust4x4[m][2]);
                level4_[list][m][p] = ctx.pps->scaling.m4[list][p] * v;
            }
    for (int list = 0; list < 2; ++list)
        for (int m = 0; m < 6; ++m)
            for (int p = 0; p < 64; ++p) {
                const int i = p >> 3, j = p & 7;
                int k = 5;
                if (i % 4 == 0 && j % 4 == 0) k = 0;
                else if (i % 2 == 1 && j % 2 == 1) k = 1;
                else if (i % 4 == 2 && j % 4 == 2) k = 2;
                else if ((i % 4 == 0 && j % 2 == 1) || (i % 2 == 1 && j % 4 == 0)) k = 3;
                else if ((i % 4 == 0 && j % 4 == 2) || (i % 4 == 2 && j % 4 == 0)) k = 4;
                level8_[list][m][p] = ctx.pps->scaling.m8[list][p] * kNormAdjust8x8[m][k];
            }
    if (sh.type == kSliceB && ctx.pps->weighted_bipred_idc == 2) ComputeImplicitWeights();

    const int total = width_mbs_ * height_mbs_;
    int addr = sh.first_mb;
    if (addr >= total) {
        *error = "first_mb_in_slice outside the picture";
        return false;
    }
    if (ctx.pps->cabac) return DecodeSliceCabac(br, addr, error);

    bool more = true;
    while (more) {
        if (sh.type != kSliceI) {
            const uint32_t run = br.Ue();
            if (run > static_cast<uint32_t>(total - addr) || br.overrun()) {
                *error = "bad mb_skip_run";
                return false;
            }
            for (uint32_t k = 0; k < run; ++k) {
                StartMb(addr++);
                data_.Reset();
                data_.kind = sh.type == kSliceP ? MbData::kPSkip : MbData::kBSkip;
                data_.inter = true;
                SetQp(&data_);
                Reconstruct(data_);
            }
            if (run > 0) {
                more = br.MoreRbspData();
                if (!more) break;
            }
        }
        if (addr >= total) {
            *error = "slice runs past the end of the picture";
            return false;
        }
        StartMb(addr);
        data_.Reset();
        if (!ParseCavlc(br, &data_)) {
            cur_->slice_num = -1;
            *error = "damaged macroblock";
            return false;
        }
        Reconstruct(data_);
        ++addr;
        more = br.MoreRbspData();
        if (more && addr >= total) {
            *error = "slice runs past the end of the picture";
            return false;
        }
    }
    return true;
}

void MbDecoder::StartMb(int addr) {
    mbx_ = addr % width_mbs_;
    mby_ = addr / width_mbs_;
    cur_ = &mbs_[addr];
    MbInfo &m = *cur_;
    m.slice_num = slice_num_;
    m.intra = m.i16 = m.i4 = m.pcm = m.skip = m.t8 = m.direct16 = false;
    m.cbp = 0;
    m.chroma_pred = 0;
    m.cbf_dc = 0;
    m.coded = 0;
    std::memset(m.nz, 0, sizeof(m.nz));
    std::memset(m.nzc, 0, sizeof(m.nzc));
    std::memset(m.ipred, -1, sizeof(m.ipred));
    for (int l = 0; l < 2; ++l)
        for (int i = 0; i < 4; ++i) {
            m.ref[l][i] = -1;
            m.ref_uid[l][i] = -1;
            m.ref_poc[l][i] = 0;
            m.ref_long[l][i] = false;
        }
    std::memset(m.mv, 0, sizeof(m.mv));
    std::memset(m.mvd, 0, sizeof(m.mvd));
    for (bool &b : m.sub_direct) b = false;
    m.dbf_idc = static_cast<int8_t>(ctx_->sh->disable_deblocking);
    m.alpha_off = static_cast<int8_t>(ctx_->sh->alpha_offset);
    m.beta_off = static_cast<int8_t>(ctx_->sh->beta_offset);
    decoded4_ = 0;
    spatial_cache_valid_ = false;
}

void MbDecoder::SetQp(MbData *d) {
    int qp = prev_qp_;
    if (d->qp_delta != 0) qp = (prev_qp_ + d->qp_delta + 52) % 52;
    prev_qp_ = qp;
    cur_->qp = static_cast<int8_t>(qp);
    for (int c = 0; c < 2; ++c) cur_->qpc[c] = static_cast<int8_t>(kChromaQp[Clip3(0, 51, qp + ctx_->pps->chroma_qp_offset[c])]);
}

bool MbDecoder::Allows8x8Transform(const MbData &d) const {
    if (d.kind == MbData::kBDirect16) return ctx_->sps->direct_8x8_inference;
    if (d.kind != MbData::kInter) return false;
    if (d.part != kPart8x8) return true;
    for (int i = 0; i < 4; ++i) {
        if (d.sub_direct[i]) {
            if (!ctx_->sps->direct_8x8_inference) return false;
        } else if (d.sub_shape[i] != kSub8x8) {
            return false;
        }
    }
    return true;
}

// --- Reconstruction --------------------------------------------------------------------

void MbDecoder::Reconstruct(MbData &d) {
    MbInfo &m = *cur_;
    const int W = frame_->width;
    if (d.kind == MbData::kPcm) {
        m.intra = m.pcm = true;
        m.qp = 0;
        for (int c = 0; c < 2; ++c) m.qpc[c] = static_cast<int8_t>(kChromaQp[Clip3(0, 51, ctx_->pps->chroma_qp_offset[c])]);
        m.cbp = 0x2f;
        std::memset(m.nz, 16, sizeof(m.nz));
        std::memset(m.nzc, 16, sizeof(m.nzc));
        m.cbf_dc = 7;
        m.coded = 0xffff;
        for (int y = 0; y < 16; ++y)
            std::memcpy(&frame_->y[static_cast<size_t>((mby_ * 16 + y) * W + mbx_ * 16)], &d.pcm[y * 16], 16);
        const int cw = W / 2;
        for (int c = 0; c < 2; ++c) {
            std::vector<uint8_t> &plane = c == 0 ? frame_->u : frame_->v;
            for (int y = 0; y < 8; ++y)
                std::memcpy(&plane[static_cast<size_t>((mby_ * 8 + y) * cw + mbx_ * 8)], &d.pcm[256 + c * 64 + y * 8], 8);
        }
        return;
    }
    m.cbp = d.cbp;
    m.t8 = d.t8;
    if (!d.inter) {
        m.intra = true;
        m.i16 = d.kind == MbData::kI16;
        m.i4 = d.kind == MbData::kI4 || d.kind == MbData::kI8;
        m.chroma_pred = static_cast<uint8_t>(d.chroma_mode);
        if (d.kind == MbData::kI4) {
            IntraPred4x4Modes(d);
            for (int b = 0; b < 16; ++b) {
                const int blk = kBlkY[b] * 4 + kBlkX[b];
                PredictIntra4x4(kBlkX[b], kBlkY[b], m.ipred[blk]);
                if (d.coded4 & (1 << blk)) AddLuma4x4(d, blk);
            }
        } else if (d.kind == MbData::kI8) {
            IntraPred8x8Modes(d);
            for (int b8 = 0; b8 < 4; ++b8) {
                PredictIntra8x8(b8 & 1, b8 >> 1, m.ipred[(b8 >> 1) * 8 + (b8 & 1) * 2]);
                if (d.coded8 & (1 << b8)) AddLuma8x8(d, b8);
            }
        } else {
            PredictIntra16x16(d.i16_mode);
            AddLumaResidual(d);
        }
        PredictIntraChroma(d.chroma_mode);
    } else {
        m.skip = d.kind == MbData::kPSkip || d.kind == MbData::kBSkip;
        m.direct16 = d.kind == MbData::kBSkip || d.kind == MbData::kBDirect16;
        DeriveMotion(d);
        AddLumaResidual(d);
    }
    AddChromaResidual(d);
    uint16_t coded = d.coded4;
    for (int b8 = 0; b8 < 4; ++b8)
        if (d.coded8 & (1 << b8)) coded |= static_cast<uint16_t>(0x33 << ((b8 >> 1) * 8 + (b8 & 1) * 2));
    m.coded = coded;
}

void MbDecoder::AddLuma4x4(const MbData &d, int blk) {
    const int qp = cur_->qp;
    const int list = d.inter ? 3 : 0;
    const int *ls = level4_[list][qp % 6];
    int c[16];
    for (int p = 0; p < 16; ++p) c[p] = d.coef[blk][p] ? Scale4(d.coef[blk][p], ls[p], qp) : 0;
    const int W = frame_->width;
    Idct4Add(c, &frame_->y[static_cast<size_t>((mby_ * 16 + (blk >> 2) * 4) * W + mbx_ * 16 + (blk & 3) * 4)], W);
}

void MbDecoder::AddLuma8x8(const MbData &d, int b8) {
    const int qp = cur_->qp;
    const int *ls = level8_[d.inter ? 1 : 0][qp % 6];
    int c[64];
    for (int p = 0; p < 64; ++p) c[p] = d.coef8[b8][p] ? Scale8(d.coef8[b8][p], ls[p], qp) : 0;
    const int W = frame_->width;
    Idct8Add(c, &frame_->y[static_cast<size_t>((mby_ * 16 + (b8 >> 1) * 8) * W + mbx_ * 16 + (b8 & 1) * 8)], W);
}

void MbDecoder::AddLumaResidual(const MbData &d) {
    if (d.kind == MbData::kI16) {
        // 8.5.10: the DC coefficients' Hadamard transform and scaling.
        const int qp = cur_->qp;
        int dc[16] = {};
        if (d.has_dc_luma) {
            int t[16];
            const int16_t *c = d.dc_luma;
            for (int i = 0; i < 4; ++i) {
                const int a = c[i * 4 + 0], b = c[i * 4 + 1], e = c[i * 4 + 2], f = c[i * 4 + 3];
                t[i * 4 + 0] = a + b + e + f;
                t[i * 4 + 1] = a + b - e - f;
                t[i * 4 + 2] = a - b - e + f;
                t[i * 4 + 3] = a - b + e - f;
            }
            const int ls = level4_[0][qp % 6][0];
            for (int j = 0; j < 4; ++j) {
                const int a = t[j], b = t[4 + j], e = t[8 + j], f = t[12 + j];
                const int col[4] = {a + b + e + f, a + b - e - f, a - b - e + f, a - b + e - f};
                for (int i = 0; i < 4; ++i) {
                    const int64_t v = int64_t{col[i]} * ls;
                    dc[i * 4 + j] = ClampCoef(qp >= 36 ? v * (int64_t{1} << (qp / 6 - 6)) : (v + (1 << (5 - qp / 6))) >> (6 - qp / 6));
                }
            }
        }
        const int *ls = level4_[0][qp % 6];
        const int W = frame_->width;
        for (int blk = 0; blk < 16; ++blk) {
            const bool ac = (d.coded4 >> blk) & 1;
            if (!ac && dc[blk] == 0) continue;
            int c[16];
            c[0] = dc[blk];
            for (int p = 1; p < 16; ++p) c[p] = (ac && d.coef[blk][p]) ? Scale4(d.coef[blk][p], ls[p], qp) : 0;
            Idct4Add(c, &frame_->y[static_cast<size_t>((mby_ * 16 + (blk >> 2) * 4) * W + mbx_ * 16 + (blk & 3) * 4)], W);
        }
        return;
    }
    if (d.t8) {
        for (int b8 = 0; b8 < 4; ++b8)
            if (d.coded8 & (1 << b8)) AddLuma8x8(d, b8);
        return;
    }
    if (d.coded4 == 0) return;
    for (int blk = 0; blk < 16; ++blk)
        if (d.coded4 & (1 << blk)) AddLuma4x4(d, blk);
}

void MbDecoder::AddChromaResidual(const MbData &d) {
    if ((d.cbp & 0x30) == 0) return;
    const int cw = frame_->width / 2;
    for (int c = 0; c < 2; ++c) {
        const int qp = cur_->qpc[c];
        const int list = (d.inter ? 3 : 0) + 1 + c;
        const int *ls = level4_[list][qp % 6];
        int dc[4] = {0, 0, 0, 0};
        if (d.has_dc_chroma[c]) {
            // 8.5.11.1-2: 2x2 transform of the DC coefficients, then scaling.
            int cc[4];
            for (int i = 0; i < 4; ++i) cc[i] = d.dc_chroma[c][i];
            const int f[4] = {cc[0] + cc[1] + cc[2] + cc[3], cc[0] - cc[1] + cc[2] - cc[3], cc[0] + cc[1] - cc[2] - cc[3],
                              cc[0] - cc[1] - cc[2] + cc[3]};
            for (int i = 0; i < 4; ++i) dc[i] = ClampCoef((int64_t{f[i]} * ls[0] * (int64_t{1} << (qp / 6))) >> 5);
        }
        std::vector<uint8_t> &plane = c == 0 ? frame_->u : frame_->v;
        for (int b = 0; b < 4; ++b) {
            const bool ac = (d.coded_chroma[c] >> b) & 1;
            if (!ac && dc[b] == 0) continue;
            int co[16];
            co[0] = dc[b];
            for (int p = 1; p < 16; ++p) co[p] = (ac && d.ac_chroma[c][b][p]) ? Scale4(d.ac_chroma[c][b][p], ls[p], qp) : 0;
            Idct4Add(co, &plane[static_cast<size_t>((mby_ * 8 + (b >> 1) * 4) * cw + mbx_ * 8 + (b & 1) * 4)], cw);
        }
    }
}

// --- Intra prediction (8.3) ------------------------------------------------------------

// Intra4x4PredMode (8.3.1.1).
void MbDecoder::IntraPred4x4Modes(MbData &d) {
    MbInfo &m = *cur_;
    const bool constrained = ctx_->pps->constrained_intra_pred;
    for (int b = 0; b < 16; ++b) {
        const int bx = kBlkX[b], by = kBlkY[b];
        int mode_a = -1, mode_b = -1;
        bool dc_pred = false;
        // A: the block to the left; B: the block above.
        if (bx > 0) {
            mode_a = m.ipred[by * 4 + bx - 1];
        } else {
            const MbInfo *a = Neighbor(-1, 0);
            if (!a || (constrained && !a->intra)) dc_pred = true;
            else mode_a = a->i4 ? a->ipred[by * 4 + 3] : 2;
        }
        if (by > 0) {
            mode_b = m.ipred[(by - 1) * 4 + bx];
        } else {
            const MbInfo *bb = Neighbor(0, -1);
            if (!bb || (constrained && !bb->intra)) dc_pred = true;
            else mode_b = bb->i4 ? bb->ipred[12 + bx] : 2;
        }
        const int pred = dc_pred ? 2 : std::min(mode_a, mode_b);
        const int mode = d.prev_pred_flag[b] ? pred : (d.rem_pred_mode[b] < pred ? d.rem_pred_mode[b] : d.rem_pred_mode[b] + 1);
        m.ipred[by * 4 + bx] = static_cast<int8_t>(mode);
    }
}

// Intra8x8PredMode (8.3.2.1).
void MbDecoder::IntraPred8x8Modes(MbData &d) {
    MbInfo &m = *cur_;
    const bool constrained = ctx_->pps->constrained_intra_pred;
    for (int b8 = 0; b8 < 4; ++b8) {
        const int x8 = b8 & 1, y8 = b8 >> 1;
        int mode_a = -1, mode_b = -1;
        bool dc_pred = false;
        if (x8 > 0) {
            mode_a = m.ipred[y8 * 8];
        } else {
            const MbInfo *a = Neighbor(-1, 0);
            if (!a || (constrained && !a->intra)) {
                dc_pred = true;
            } else if (!a->i4) {
                mode_a = 2;
            } else if (a->t8) {
                mode_a = a->ipred[y8 * 8 + 2];
            } else {
                mode_a = a->ipred[y8 * 8 + 3];  // its 8x8 block's 4x4 block 1 (top right)
            }
        }
        if (y8 > 0) {
            mode_b = m.ipred[x8 * 2];
        } else {
            const MbInfo *bb = Neighbor(0, -1);
            if (!bb || (constrained && !bb->intra)) {
                dc_pred = true;
            } else if (!bb->i4) {
                mode_b = 2;
            } else if (bb->t8) {
                mode_b = bb->ipred[8 + x8 * 2];
            } else {
                mode_b = bb->ipred[12 + x8 * 2];  // its 8x8 block's 4x4 block 2 (bottom left)
            }
        }
        const int pred = dc_pred ? 2 : std::min(mode_a, mode_b);
        const int mode = d.prev_pred_flag[b8] ? pred : (d.rem_pred_mode[b8] < pred ? d.rem_pred_mode[b8] : d.rem_pred_mode[b8] + 1);
        for (int k = 0; k < 4; ++k) m.ipred[(y8 * 2 + (k >> 1)) * 4 + x8 * 2 + (k & 1)] = static_cast<int8_t>(mode);
    }
}

void MbDecoder::PredictIntra4x4(int bx, int by, int mode) {
    const int W = frame_->width;
    uint8_t *dst = &frame_->y[static_cast<size_t>((mby_ * 16 + by * 4) * W + mbx_ * 16 + bx * 4)];
    const bool left = bx > 0 || IntraNeighbor(-1, 0);
    const bool top = by > 0 || IntraNeighbor(0, -1);
    bool topleft = false;
    if (bx > 0 && by > 0) topleft = true;
    else if (bx > 0) topleft = IntraNeighbor(0, -1) != nullptr;
    else if (by > 0) topleft = IntraNeighbor(-1, 0) != nullptr;
    else topleft = IntraNeighbor(-1, -1) != nullptr;
    bool topright = false;
    if (by == 0) {
        topright = bx < 3 ? IntraNeighbor(0, -1) != nullptr : IntraNeighbor(1, -1) != nullptr;
    } else if (bx < 3) {
        topright = kRasterToBlk[(by - 1) * 4 + bx + 1] < kRasterToBlk[by * 4 + bx];
    }
    int T[8] = {}, L[4] = {}, Q = 0;
    if (top) {
        for (int i = 0; i < 4; ++i) T[i] = dst[-W + i];
        for (int i = 4; i < 8; ++i) T[i] = topright ? dst[-W + i] : T[3];
    }
    if (left)
        for (int i = 0; i < 4; ++i) L[i] = dst[i * W - 1];
    if (topleft) Q = dst[-W - 1];
    auto t = [&](int i) { return i < 0 ? Q : T[i]; };
    auto l = [&](int i) { return i < 0 ? Q : L[i]; };
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            int v = 128;
            switch (mode) {
                case 0: v = T[x]; break;
                case 1: v = L[y]; break;
                case 2:
                    if (left && top) v = (T[0] + T[1] + T[2] + T[3] + L[0] + L[1] + L[2] + L[3] + 4) >> 3;
                    else if (left) v = (L[0] + L[1] + L[2] + L[3] + 2) >> 2;
                    else if (top) v = (T[0] + T[1] + T[2] + T[3] + 2) >> 2;
                    break;
                case 3:
                    v = (x == 3 && y == 3) ? (T[6] + 3 * T[7] + 2) >> 2 : (T[x + y] + 2 * T[x + y + 1] + T[x + y + 2] + 2) >> 2;
                    break;
                case 4:
                    if (x > y) v = (t(x - y - 2) + 2 * t(x - y - 1) + t(x - y) + 2) >> 2;
                    else if (x < y) v = (l(y - x - 2) + 2 * l(y - x - 1) + l(y - x) + 2) >> 2;
                    else v = (T[0] + 2 * Q + L[0] + 2) >> 2;
                    break;
                case 5: {
                    const int z = 2 * x - y;
                    if (z >= 0 && (z & 1) == 0) v = (t(x - (y >> 1) - 1) + t(x - (y >> 1)) + 1) >> 1;
                    else if (z >= 0) v = (t(x - (y >> 1) - 2) + 2 * t(x - (y >> 1) - 1) + t(x - (y >> 1)) + 2) >> 2;
                    else if (z == -1) v = (L[0] + 2 * Q + T[0] + 2) >> 2;
                    else v = (l(y - 1) + 2 * l(y - 2) + l(y - 3) + 2) >> 2;
                    break;
                }
                case 6: {
                    const int z = 2 * y - x;
                    if (z >= 0 && (z & 1) == 0) v = (l(y - (x >> 1) - 1) + l(y - (x >> 1)) + 1) >> 1;
                    else if (z >= 0) v = (l(y - (x >> 1) - 2) + 2 * l(y - (x >> 1) - 1) + l(y - (x >> 1)) + 2) >> 2;
                    else if (z == -1) v = (L[0] + 2 * Q + T[0] + 2) >> 2;
                    else v = (t(x - 1) + 2 * t(x - 2) + t(x - 3) + 2) >> 2;
                    break;
                }
                case 7:
                    if ((y & 1) == 0) v = (T[x + (y >> 1)] + T[x + (y >> 1) + 1] + 1) >> 1;
                    else v = (T[x + (y >> 1)] + 2 * T[x + (y >> 1) + 1] + T[x + (y >> 1) + 2] + 2) >> 2;
                    break;
                case 8: {
                    const int z = x + 2 * y;
                    if (z > 5) v = L[3];
                    else if (z == 5) v = (L[2] + 3 * L[3] + 2) >> 2;
                    else if ((z & 1) == 0) v = (L[y + (x >> 1)] + L[y + (x >> 1) + 1] + 1) >> 1;
                    else v = (L[y + (x >> 1)] + 2 * L[y + (x >> 1) + 1] + L[y + (x >> 1) + 2] + 2) >> 2;
                    break;
                }
                default: break;
            }
            dst[y * W + x] = static_cast<uint8_t>(v);
        }
    }
}

void MbDecoder::PredictIntra8x8(int x8, int y8, int mode) {
    const int W = frame_->width;
    uint8_t *dst = &frame_->y[static_cast<size_t>((mby_ * 16 + y8 * 8) * W + mbx_ * 16 + x8 * 8)];
    const bool left = x8 > 0 || IntraNeighbor(-1, 0);
    const bool top = y8 > 0 || IntraNeighbor(0, -1);
    bool topleft = false;
    if (x8 > 0 && y8 > 0) topleft = true;
    else if (x8 > 0) topleft = IntraNeighbor(0, -1) != nullptr;
    else if (y8 > 0) topleft = IntraNeighbor(-1, 0) != nullptr;
    else topleft = IntraNeighbor(-1, -1) != nullptr;
    bool topright = false;
    if (y8 == 0) topright = x8 == 0 ? IntraNeighbor(0, -1) != nullptr : IntraNeighbor(1, -1) != nullptr;
    else topright = x8 == 0;
    // Unfiltered references p[-1..15, -1], p[-1, 0..7].
    int pt[16] = {}, pl[8] = {}, pq = 0;
    if (top) {
        for (int i = 0; i < 8; ++i) pt[i] = dst[-W + i];
        for (int i = 8; i < 16; ++i) pt[i] = topright ? dst[-W + i] : pt[7];
    }
    if (left)
        for (int i = 0; i < 8; ++i) pl[i] = dst[i * W - 1];
    if (topleft) pq = dst[-W - 1];
    // 8.3.2.2.1: reference sample filtering.
    int T[16] = {}, L[8] = {}, Q = 0;
    if (top) {
        T[0] = topleft ? (pq + 2 * pt[0] + pt[1] + 2) >> 2 : (3 * pt[0] + pt[1] + 2) >> 2;
        for (int i = 1; i < 15; ++i) T[i] = (pt[i - 1] + 2 * pt[i] + pt[i + 1] + 2) >> 2;
        T[15] = (pt[14] + 3 * pt[15] + 2) >> 2;
    }
    if (topleft) {
        if (top && left) Q = (pt[0] + 2 * pq + pl[0] + 2) >> 2;
        else if (top) Q = (3 * pq + pt[0] + 2) >> 2;
        else if (left) Q = (3 * pq + pl[0] + 2) >> 2;
        else Q = pq;
    }
    if (left) {
        L[0] = topleft ? (pq + 2 * pl[0] + pl[1] + 2) >> 2 : (3 * pl[0] + pl[1] + 2) >> 2;
        for (int i = 1; i < 7; ++i) L[i] = (pl[i - 1] + 2 * pl[i] + pl[i + 1] + 2) >> 2;
        L[7] = (pl[6] + 3 * pl[7] + 2) >> 2;
    }
    auto t = [&](int i) { return i < 0 ? Q : T[i]; };
    auto l = [&](int i) { return i < 0 ? Q : L[i]; };
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            int v = 128;
            switch (mode) {
                case 0: v = T[x]; break;
                case 1: v = L[y]; break;
                case 2: {
                    int st = 0, sl = 0;
                    for (int i = 0; i < 8; ++i) {
                        st += T[i];
                        sl += L[i];
                    }
                    if (top && left) v = (st + sl + 8) >> 4;
                    else if (left) v = (sl + 4) >> 3;
                    else if (top) v = (st + 4) >> 3;
                    break;
                }
                case 3:
                    v = (x == 7 && y == 7) ? (T[14] + 3 * T[15] + 2) >> 2 : (T[x + y] + 2 * T[x + y + 1] + T[x + y + 2] + 2) >> 2;
                    break;
                case 4:
                    if (x > y) v = (t(x - y - 2) + 2 * t(x - y - 1) + t(x - y) + 2) >> 2;
                    else if (x < y) v = (l(y - x - 2) + 2 * l(y - x - 1) + l(y - x) + 2) >> 2;
                    else v = (T[0] + 2 * Q + L[0] + 2) >> 2;
                    break;
                case 5: {
                    const int z = 2 * x - y;
                    if (z >= 0 && (z & 1) == 0) v = (t(x - (y >> 1) - 1) + t(x - (y >> 1)) + 1) >> 1;
                    else if (z >= 0) v = (t(x - (y >> 1) - 2) + 2 * t(x - (y >> 1) - 1) + t(x - (y >> 1)) + 2) >> 2;
                    else if (z == -1) v = (L[0] + 2 * Q + T[0] + 2) >> 2;
                    else v = (l(y - 2 * x - 1) + 2 * l(y - 2 * x - 2) + l(y - 2 * x - 3) + 2) >> 2;
                    break;
                }
                case 6: {
                    const int z = 2 * y - x;
                    if (z >= 0 && (z & 1) == 0) v = (l(y - (x >> 1) - 1) + l(y - (x >> 1)) + 1) >> 1;
                    else if (z >= 0) v = (l(y - (x >> 1) - 2) + 2 * l(y - (x >> 1) - 1) + l(y - (x >> 1)) + 2) >> 2;
                    else if (z == -1) v = (L[0] + 2 * Q + T[0] + 2) >> 2;
                    else v = (t(x - 2 * y - 1) + 2 * t(x - 2 * y - 2) + t(x - 2 * y - 3) + 2) >> 2;
                    break;
                }
                case 7:
                    if ((y & 1) == 0) v = (T[x + (y >> 1)] + T[x + (y >> 1) + 1] + 1) >> 1;
                    else v = (T[x + (y >> 1)] + 2 * T[x + (y >> 1) + 1] + T[x + (y >> 1) + 2] + 2) >> 2;
                    break;
                case 8: {
                    const int z = x + 2 * y;
                    if (z > 13) v = L[7];
                    else if (z == 13) v = (L[6] + 3 * L[7] + 2) >> 2;
                    else if ((z & 1) == 0) v = (L[y + (x >> 1)] + L[y + (x >> 1) + 1] + 1) >> 1;
                    else v = (L[y + (x >> 1)] + 2 * L[y + (x >> 1) + 1] + L[y + (x >> 1) + 2] + 2) >> 2;
                    break;
                }
                default: break;
            }
            dst[y * W + x] = static_cast<uint8_t>(v);
        }
    }
}

void MbDecoder::PredictIntra16x16(int mode) {
    const int W = frame_->width;
    uint8_t *dst = &frame_->y[static_cast<size_t>(mby_ * 16 * W + mbx_ * 16)];
    const bool left = IntraNeighbor(-1, 0) != nullptr;
    const bool top = IntraNeighbor(0, -1) != nullptr;
    const bool topleft = IntraNeighbor(-1, -1) != nullptr;
    int T[16] = {}, L[16] = {}, Q = 0;
    if (top)
        for (int i = 0; i < 16; ++i) T[i] = dst[-W + i];
    if (left)
        for (int i = 0; i < 16; ++i) L[i] = dst[i * W - 1];
    if (topleft) Q = dst[-W - 1];
    if (mode == 3) {
        int H = 0, V = 0;
        for (int i = 0; i < 8; ++i) {
            H += (i + 1) * (T[8 + i] - (i == 7 ? Q : T[6 - i]));
            V += (i + 1) * (L[8 + i] - (i == 7 ? Q : L[6 - i]));
        }
        const int a = 16 * (L[15] + T[15]);
        const int b = (5 * H + 32) >> 6, c = (5 * V + 32) >> 6;
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x) dst[y * W + x] = Clip1((a + b * (x - 7) + c * (y - 7) + 16) >> 5);
        return;
    }
    int dc = 128;
    if (mode == 2) {
        int st = 0, sl = 0;
        for (int i = 0; i < 16; ++i) {
            st += T[i];
            sl += L[i];
        }
        if (top && left) dc = (st + sl + 16) >> 5;
        else if (left) dc = (sl + 8) >> 4;
        else if (top) dc = (st + 8) >> 4;
    }
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) dst[y * W + x] = static_cast<uint8_t>(mode == 0 ? T[x] : (mode == 1 ? L[y] : dc));
}

void MbDecoder::PredictIntraChroma(int mode) {
    const int cw = frame_->width / 2;
    const bool left = IntraNeighbor(-1, 0) != nullptr;
    const bool top = IntraNeighbor(0, -1) != nullptr;
    const bool topleft = IntraNeighbor(-1, -1) != nullptr;
    for (int c = 0; c < 2; ++c) {
        std::vector<uint8_t> &plane = c == 0 ? frame_->u : frame_->v;
        uint8_t *dst = &plane[static_cast<size_t>(mby_ * 8 * cw + mbx_ * 8)];
        int T[8] = {}, L[8] = {}, Q = 0;
        if (top)
            for (int i = 0; i < 8; ++i) T[i] = dst[-cw + i];
        if (left)
            for (int i = 0; i < 8; ++i) L[i] = dst[i * cw - 1];
        if (topleft) Q = dst[-cw - 1];
        if (mode == 0) {
            // 8.3.4.1-3: DC per 4x4 chroma block.
            for (int b = 0; b < 4; ++b) {
                const int xo = (b & 1) * 4, yo = (b >> 1) * 4;
                int st = 0, sl = 0;
                for (int i = 0; i < 4; ++i) {
                    st += T[xo + i];
                    sl += L[yo + i];
                }
                int v = 128;
                if ((xo == 0 && yo == 0) || (xo > 0 && yo > 0)) {
                    if (top && left) v = (st + sl + 4) >> 3;
                    else if (left) v = (sl + 2) >> 2;
                    else if (top) v = (st + 2) >> 2;
                } else if (xo > 0) {
                    if (top) v = (st + 2) >> 2;
                    else if (left) v = (sl + 2) >> 2;
                } else {
                    if (left) v = (sl + 2) >> 2;
                    else if (top) v = (st + 2) >> 2;
                }
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 4; ++x) dst[(yo + y) * cw + xo + x] = static_cast<uint8_t>(v);
            }
        } else if (mode == 1) {
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) dst[y * cw + x] = static_cast<uint8_t>(L[y]);
        } else if (mode == 2) {
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) dst[y * cw + x] = static_cast<uint8_t>(T[x]);
        } else {
            int H = 0, V = 0;
            for (int i = 0; i < 4; ++i) {
                H += (i + 1) * (T[4 + i] - (i == 3 ? Q : T[2 - i]));
                V += (i + 1) * (L[4 + i] - (i == 3 ? Q : L[2 - i]));
            }
            const int a = 16 * (L[7] + T[7]);
            const int b = (34 * H + 32) >> 6, cc = (34 * V + 32) >> 6;
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) dst[y * cw + x] = Clip1((a + b * (x - 3) + cc * (y - 3) + 16) >> 5);
        }
    }
}

// --- Motion vectors (8.4.1) -------------------------------------------------------------

MbDecoder::Nb MbDecoder::NeighborMotion(int list, int x, int y) const {
    Nb n;
    const MbInfo *m = nullptr;
    int bx = 0, by = 0;
    if (y < 0) {
        if (x < 0) m = Neighbor(-1, -1);
        else if (x < 16) m = Neighbor(0, -1);
        else m = Neighbor(1, -1);
        by = 3;
        bx = (x & 15) >> 2;
    } else if (x < 0) {
        m = Neighbor(-1, 0);
        bx = 3;
        by = y >> 2;
    } else if (x >= 16) {
        return n;
    } else {
        bx = x >> 2;
        by = y >> 2;
        if (!(decoded4_ & (1 << (by * 4 + bx)))) return n;
        m = cur_;
    }
    if (!m) return n;
    n.avail = true;
    if (m->intra) return n;
    const int ref = m->ref[list][(by >> 1) * 2 + (bx >> 1)];
    if (ref < 0) return n;
    n.ref = ref;
    n.mv[0] = m->mv[list][by * 4 + bx][0];
    n.mv[1] = m->mv[list][by * 4 + bx][1];
    return n;
}

// 8.4.1.3: the motion vector predictor of a partition at (x, y), w x h.
// `shape`: 1 for a 16x8 partition, 2 for 8x16, 0 otherwise.
void MbDecoder::PredictMv(int list, int ref, int x, int y, int w, int h, int shape, int part, int mvp[2]) const {
    (void)h;
    Nb a = NeighborMotion(list, x - 1, y);
    Nb b = NeighborMotion(list, x, y - 1);
    Nb c = NeighborMotion(list, x + w, y - 1);
    if (!c.avail) c = NeighborMotion(list, x - 1, y - 1);
    if (shape == 1) {
        if (part == 0 && b.ref == ref) {
            mvp[0] = b.mv[0];
            mvp[1] = b.mv[1];
            return;
        }
        if (part == 1 && a.ref == ref) {
            mvp[0] = a.mv[0];
            mvp[1] = a.mv[1];
            return;
        }
    } else if (shape == 2) {
        if (part == 0 && a.ref == ref) {
            mvp[0] = a.mv[0];
            mvp[1] = a.mv[1];
            return;
        }
        if (part == 1 && c.ref == ref) {
            mvp[0] = c.mv[0];
            mvp[1] = c.mv[1];
            return;
        }
    }
    if (!b.avail && !c.avail && a.avail) {
        b = a;
        c = a;
    }
    const int matches = (a.ref == ref) + (b.ref == ref) + (c.ref == ref);
    if (matches == 1) {
        const Nb &n = a.ref == ref ? a : (b.ref == ref ? b : c);
        mvp[0] = n.mv[0];
        mvp[1] = n.mv[1];
        return;
    }
    mvp[0] = Median(a.mv[0], b.mv[0], c.mv[0]);
    mvp[1] = Median(a.mv[1], b.mv[1], c.mv[1]);
}

// 8.4.1.1: P_Skip.
void MbDecoder::PSkipMv(int mv[2]) const {
    mv[0] = mv[1] = 0;
    const Nb a = NeighborMotion(0, -1, 0);
    const Nb b = NeighborMotion(0, 0, -1);
    if (!a.avail || !b.avail) return;
    if ((a.ref == 0 && a.mv[0] == 0 && a.mv[1] == 0) || (b.ref == 0 && b.mv[0] == 0 && b.mv[1] == 0)) return;
    PredictMv(0, 0, 0, 0, 16, 16, 0, 0, mv);
}

void MbDecoder::SetMotion(int list, int x, int y, int w, int h, int ref, const int mv[2]) {
    MbInfo &m = *cur_;
    for (int by = y >> 2; by < (y + h) >> 2; ++by)
        for (int bx = x >> 2; bx < (x + w) >> 2; ++bx) {
            m.mv[list][by * 4 + bx][0] = static_cast<int16_t>(ref >= 0 ? mv[0] : 0);
            m.mv[list][by * 4 + bx][1] = static_cast<int16_t>(ref >= 0 ? mv[1] : 0);
        }
    for (int b8y = y >> 3; b8y < (y + h + 7) >> 3; ++b8y)
        for (int b8x = x >> 3; b8x < (x + w + 7) >> 3; ++b8x) {
            const int b8 = b8y * 2 + b8x;
            m.ref[list][b8] = static_cast<int8_t>(ref);
            if (ref >= 0) {
                const Frame *f = ctx_->refs[list][static_cast<size_t>(ref)];
                m.ref_uid[list][b8] = f->uid;
                m.ref_poc[list][b8] = f->poc;
                m.ref_long[list][b8] = f->long_ref;
            } else {
                m.ref_uid[list][b8] = -1;
            }
        }
}

namespace {
void MarkDecoded(uint16_t *mask, int x, int y, int w, int h) {
    for (int by = y >> 2; by < (y + h) >> 2; ++by)
        for (int bx = x >> 2; bx < (x + w) >> 2; ++bx) *mask = static_cast<uint16_t>(*mask | (1 << (by * 4 + bx)));
}
}  // namespace

// 8.4.1.2.2: spatial direct prediction for the 8x8 blocks b8_first..b8_last.
void MbDecoder::DirectSpatial(int b8_first, int b8_last) {
    if (!spatial_cache_valid_) {
        // Reference indices and predictors come from the neighbours of the
        // whole macroblock, so they are worked out once per macroblock.
        const uint16_t saved = decoded4_;
        decoded4_ = 0;
        for (int list = 0; list < 2; ++list) {
            const Nb a = NeighborMotion(list, -1, 0);
            const Nb b = NeighborMotion(list, 0, -1);
            Nb c = NeighborMotion(list, 16, -1);
            if (!c.avail) c = NeighborMotion(list, -1, -1);
            spatial_ref_[list] = MinPositive(a.ref, MinPositive(b.ref, c.ref));
        }
        spatial_zero_ = spatial_ref_[0] < 0 && spatial_ref_[1] < 0;
        if (spatial_zero_) spatial_ref_[0] = spatial_ref_[1] = 0;
        for (int list = 0; list < 2; ++list) {
            spatial_mv_[list][0] = spatial_mv_[list][1] = 0;
            if (spatial_ref_[list] >= 0 && !spatial_zero_) PredictMv(list, spatial_ref_[list], 0, 0, 16, 16, 0, 0, spatial_mv_[list]);
        }
        decoded4_ = saved;
        spatial_cache_valid_ = true;
    }
    const Frame *col = ctx_->refs[1][0];
    const MbInfo &cm = col->mbs[static_cast<size_t>(mby_ * width_mbs_ + mbx_)];
    const bool col_short = !col->long_ref;
    const bool inference = ctx_->sps->direct_8x8_inference;
    for (int b8 = b8_first; b8 <= b8_last; ++b8) {
        const int x8 = (b8 & 1) * 8, y8 = (b8 >> 1) * 8;
        for (int k = 0; k < (inference ? 1 : 4); ++k) {
            const int x = inference ? x8 : x8 + (k & 1) * 4, y = inference ? y8 : y8 + (k >> 1) * 4;
            const int size = inference ? 8 : 4;
            // The colocated 4x4 block: the 8x8 block's corner under direct_8x8_inference.
            const int cbx = inference ? ((b8 & 1) ? 3 : 0) : x >> 2, cby = inference ? ((b8 >> 1) ? 3 : 0) : y >> 2;
            bool col_zero = false;
            if (col_short && !cm.intra) {
                const int cb8 = (cby >> 1) * 2 + (cbx >> 1);
                const int cl = cm.ref[0][cb8] >= 0 ? 0 : 1;
                const int16_t *mv = cm.mv[cl][cby * 4 + cbx];
                col_zero = cm.ref[cl][cb8] == 0 && std::abs(mv[0]) <= 1 && std::abs(mv[1]) <= 1;
            }
            for (int list = 0; list < 2; ++list) {
                int mv[2] = {spatial_mv_[list][0], spatial_mv_[list][1]};
                if (spatial_zero_ || spatial_ref_[list] < 0 || (spatial_ref_[list] == 0 && col_zero)) mv[0] = mv[1] = 0;
                SetMotion(list, x, y, size, size, spatial_ref_[list], mv);
            }
        }
        MarkDecoded(&decoded4_, x8, y8, 8, 8);
    }
}

// 8.4.1.2.3: temporal direct prediction.
void MbDecoder::DirectTemporal(int b8_first, int b8_last) {
    const Frame *col = ctx_->refs[1][0];
    const MbInfo &cm = col->mbs[static_cast<size_t>(mby_ * width_mbs_ + mbx_)];
    const bool inference = ctx_->sps->direct_8x8_inference;
    const int cur_poc = frame_->poc;
    for (int b8 = b8_first; b8 <= b8_last; ++b8) {
        const int x8 = (b8 & 1) * 8, y8 = (b8 >> 1) * 8;
        for (int k = 0; k < (inference ? 1 : 4); ++k) {
            const int x = inference ? x8 : x8 + (k & 1) * 4, y = inference ? y8 : y8 + (k >> 1) * 4;
            const int size = inference ? 8 : 4;
            const int cbx = inference ? ((b8 & 1) ? 3 : 0) : x >> 2, cby = inference ? ((b8 >> 1) ? 3 : 0) : y >> 2;
            int mv_col[2] = {0, 0};
            int ref0 = 0;
            if (!cm.intra) {
                const int cb8 = (cby >> 1) * 2 + (cbx >> 1);
                const int cl = cm.ref[0][cb8] >= 0 ? 0 : 1;
                mv_col[0] = cm.mv[cl][cby * 4 + cbx][0];
                mv_col[1] = cm.mv[cl][cby * 4 + cbx][1];
                // The lowest list 0 index of the picture the colocated block used.
                const int uid = cm.ref_uid[cl][cb8];
                for (size_t i = 0; i < ctx_->refs[0].size(); ++i)
                    if (ctx_->refs[0][i]->uid == uid) {
                        ref0 = static_cast<int>(i);
                        break;
                    }
            }
            const Frame *pic0 = ctx_->refs[0][static_cast<size_t>(ref0)];
            const Frame *pic1 = ctx_->refs[1][0];
            int mv0[2], mv1[2];
            const int td_raw = pic1->poc - pic0->poc;
            if (pic0->long_ref || td_raw == 0) {
                mv0[0] = mv_col[0];
                mv0[1] = mv_col[1];
                mv1[0] = mv1[1] = 0;
            } else {
                const int tb = Clip3(-128, 127, cur_poc - pic0->poc);
                const int td = Clip3(-128, 127, td_raw);
                const int tx = (16384 + std::abs(td / 2)) / td;
                const int dsf = Clip3(-1024, 1023, (tb * tx + 32) >> 6);
                for (int i = 0; i < 2; ++i) {
                    mv0[i] = (dsf * mv_col[i] + 128) >> 8;
                    mv1[i] = mv0[i] - mv_col[i];
                }
            }
            SetMotion(0, x, y, size, size, ref0, mv0);
            SetMotion(1, x, y, size, size, 0, mv1);
        }
        MarkDecoded(&decoded4_, x8, y8, 8, 8);
    }
}

void MbDecoder::DeriveMotion(MbData &d) {
    MbInfo &m = *cur_;
    const bool inference = ctx_->sps->direct_8x8_inference;
    auto compensate_direct = [&](int b8) {
        const int x8 = (b8 & 1) * 8, y8 = (b8 >> 1) * 8;
        if (inference) {
            MotionCompensate(x8, y8, 8, 8);
        } else {
            for (int k = 0; k < 4; ++k) MotionCompensate(x8 + (k & 1) * 4, y8 + (k >> 1) * 4, 4, 4);
        }
    };
    if (d.kind == MbData::kPSkip) {
        int mv[2];
        PSkipMv(mv);
        SetMotion(0, 0, 0, 16, 16, 0, mv);
        decoded4_ = 0xffff;
        MotionCompensate(0, 0, 16, 16);
        return;
    }
    if (d.kind == MbData::kBSkip || d.kind == MbData::kBDirect16) {
        Direct(0, 3);
        for (int b8 = 0; b8 < 4; ++b8) {
            m.sub_direct[b8] = true;
            compensate_direct(b8);
        }
        return;
    }
    if (d.part != kPart8x8) {
        const int parts = d.part == kPart16x16 ? 1 : 2;
        for (int p = 0; p < parts; ++p) {
            int x = 0, y = 0, w = 16, h = 16;
            if (d.part == kPart16x8) {
                y = p * 8;
                h = 8;
            } else if (d.part == kPart8x16) {
                x = p * 8;
                w = 8;
            }
            const int shape = d.part == kPart16x8 ? 1 : (d.part == kPart8x16 ? 2 : 0);
            for (int list = 0; list < 2; ++list) {
                if (!(d.part_pred[p] & (1 << list))) continue;
                int mv[2];
                PredictMv(list, d.ref[list][p], x, y, w, h, shape, p, mv);
                mv[0] += d.mvd[list][p][0];
                mv[1] += d.mvd[list][p][1];
                SetMotion(list, x, y, w, h, d.ref[list][p], mv);
                for (int by = y >> 2; by < (y + h) >> 2; ++by)
                    for (int bx = x >> 2; bx < (x + w) >> 2; ++bx) {
                        m.mvd[list][by * 4 + bx][0] = static_cast<uint8_t>(std::min(std::abs(static_cast<int>(d.mvd[list][p][0])), 255));
                        m.mvd[list][by * 4 + bx][1] = static_cast<uint8_t>(std::min(std::abs(static_cast<int>(d.mvd[list][p][1])), 255));
                    }
            }
            MarkDecoded(&decoded4_, x, y, w, h);
            MotionCompensate(x, y, w, h);
        }
        return;
    }
    for (int b8 = 0; b8 < 4; ++b8) {
        const int x8 = (b8 & 1) * 8, y8 = (b8 >> 1) * 8;
        if (d.sub_direct[b8]) {
            m.sub_direct[b8] = true;
            Direct(b8, b8);
            compensate_direct(b8);
            continue;
        }
        const int shape = d.sub_shape[b8];
        const int parts = SubPartCount(shape);
        const int sw = (shape == kSub8x8 || shape == kSub8x4) ? 8 : 4;
        const int sh = (shape == kSub8x8 || shape == kSub4x8) ? 8 : 4;
        for (int s = 0; s < parts; ++s) {
            const int x = x8 + (sw == 8 ? 0 : (s & 1) * 4);
            const int y = y8 + (sh == 8 ? 0 : (sw == 8 ? s * 4 : (s >> 1) * 4));
            for (int list = 0; list < 2; ++list) {
                if (!(d.sub_pred[b8] & (1 << list))) continue;
                int mv[2];
                PredictMv(list, d.ref[list][b8], x, y, sw, sh, 0, 0, mv);
                mv[0] += d.mvd[list][b8 * 4 + s][0];
                mv[1] += d.mvd[list][b8 * 4 + s][1];
                SetMotion(list, x, y, sw, sh, d.ref[list][b8], mv);
                for (int by = y >> 2; by < (y + sh) >> 2; ++by)
                    for (int bx = x >> 2; bx < (x + sw) >> 2; ++bx) {
                        m.mvd[list][by * 4 + bx][0] = static_cast<uint8_t>(std::min(std::abs(static_cast<int>(d.mvd[list][b8 * 4 + s][0])), 255));
                        m.mvd[list][by * 4 + bx][1] = static_cast<uint8_t>(std::min(std::abs(static_cast<int>(d.mvd[list][b8 * 4 + s][1])), 255));
                    }
            }
            // A list this sub-macroblock doesn't use still needs its
            // reference index cleared for the 8x8 block.
            for (int list = 0; list < 2; ++list)
                if (!(d.sub_pred[b8] & (1 << list)) && s == 0) {
                    const int zero[2] = {0, 0};
                    SetMotion(list, x8, y8, 8, 8, -1, zero);
                }
            MarkDecoded(&decoded4_, x, y, sw, sh);
            MotionCompensate(x, y, sw, sh);
        }
    }
}

// --- Motion compensation (8.4.2) ---------------------------------------------------------

namespace {

// 8.4.2.2.1: luma sample interpolation for a w x h block whose integer
// position is (ix, iy) and fractional position (fx, fy) in quarter samples.
void LumaInterp(const Frame &ref, int ix, int iy, int fx, int fy, int w, int h, uint8_t *dst) {
    const int W = ref.width, H = ref.height;
    // The source window: 2 samples left/above, 3 right/below.
    uint8_t edge[(16 + 5) * (16 + 5)];
    const uint8_t *src = nullptr;
    int ss = 0;
    if (ix - 2 >= 0 && iy - 2 >= 0 && ix + w + 3 <= W && iy + h + 3 <= H) {
        src = &ref.y[static_cast<size_t>(iy * W + ix)];
        ss = W;
    } else {
        ss = w + 5;
        for (int y = 0; y < h + 5; ++y) {
            const int sy = Clip3(0, H - 1, iy - 2 + y);
            for (int x = 0; x < w + 5; ++x) edge[y * ss + x] = ref.y[static_cast<size_t>(sy * W + Clip3(0, W - 1, ix - 2 + x))];
        }
        src = edge + 2 * ss + 2;
    }
    auto G = [&](int x, int y) -> int { return src[y * ss + x]; };
    auto tap = [](int a, int b, int c, int d, int e, int f) { return a - 5 * b + 20 * c + 20 * d - 5 * e + f; };
    if (fx == 0 && fy == 0) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) dst[y * 16 + x] = static_cast<uint8_t>(G(x, y));
        return;
    }
    // b: horizontal half samples, rows 0..h (row h is 's' for the row
    // below); hh: vertical half samples, columns 0..w (column w is 'm');
    // j: the centre half samples.
    uint8_t b[17][16], hh[16][17], j[16][16];
    const bool need_s = fy == 3 && fx != 0;
    const bool need_m = fx == 3 && fy != 0;
    // Which intermediates the position uses: b for a b c e f g (and s),
    // h for d h n e i p (and m), j for f i j k q.
    const bool use_b = fx != 0 && fy != 2;
    const bool use_h = fy != 0 && fx != 2;
    const bool use_j = (fx == 2 && fy != 0) || (fy == 2 && fx != 0);
    if (use_b || need_s) {
        const int rows = need_s ? h + 1 : h;
        for (int y = 0; y < rows; ++y)
            for (int x = 0; x < w; ++x)
                b[y][x] = Clip1((tap(G(x - 2, y), G(x - 1, y), G(x, y), G(x + 1, y), G(x + 2, y), G(x + 3, y)) + 16) >> 5);
    }
    if (use_h || need_m) {
        const int cols = need_m ? w + 1 : w;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < cols; ++x)
                hh[y][x] = Clip1((tap(G(x, y - 2), G(x, y - 1), G(x, y), G(x, y + 1), G(x, y + 2), G(x, y + 3)) + 16) >> 5);
    }
    if (use_j) {
        int b1[21][16];
        for (int y = -2; y < h + 3; ++y)
            for (int x = 0; x < w; ++x) b1[y + 2][x] = tap(G(x - 2, y), G(x - 1, y), G(x, y), G(x + 1, y), G(x + 2, y), G(x + 3, y));
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                j[y][x] = Clip1((tap(b1[y][x], b1[y + 1][x], b1[y + 2][x], b1[y + 3][x], b1[y + 4][x], b1[y + 5][x]) + 512) >> 10);
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int v = 0;
            switch (fy * 4 + fx) {
                case 1: v = (G(x, y) + b[y][x] + 1) >> 1; break;              // a
                case 2: v = b[y][x]; break;                                   // b
                case 3: v = (G(x + 1, y) + b[y][x] + 1) >> 1; break;          // c
                case 4: v = (G(x, y) + hh[y][x] + 1) >> 1; break;             // d
                case 5: v = (b[y][x] + hh[y][x] + 1) >> 1; break;             // e
                case 6: v = (b[y][x] + j[y][x] + 1) >> 1; break;              // f
                case 7: v = (b[y][x] + hh[y][x + 1] + 1) >> 1; break;         // g
                case 8: v = hh[y][x]; break;                                  // h
                case 9: v = (hh[y][x] + j[y][x] + 1) >> 1; break;             // i
                case 10: v = j[y][x]; break;                                  // j
                case 11: v = (j[y][x] + hh[y][x + 1] + 1) >> 1; break;        // k
                case 12: v = (G(x, y + 1) + hh[y][x] + 1) >> 1; break;        // n
                case 13: v = (hh[y][x] + b[y + 1][x] + 1) >> 1; break;        // p
                case 14: v = (j[y][x] + b[y + 1][x] + 1) >> 1; break;         // q
                case 15: v = (hh[y][x + 1] + b[y + 1][x] + 1) >> 1; break;    // r
                default: break;
            }
            dst[y * 16 + x] = static_cast<uint8_t>(v);
        }
    }
}

// 8.4.2.2.2: chroma sample interpolation (eighth samples).
void ChromaInterp(const std::vector<uint8_t> &plane, int cw, int ch, int ix, int iy, int fx, int fy, int w, int h, uint8_t *dst) {
    const bool inside = ix >= 0 && iy >= 0 && ix + w + 1 <= cw && iy + h + 1 <= ch;
    auto at = [&](int x, int y) -> int {
        if (inside) return plane[static_cast<size_t>(y * cw + x)];
        return plane[static_cast<size_t>(Clip3(0, ch - 1, y) * cw + Clip3(0, cw - 1, x))];
    };
    const int wa = (8 - fx) * (8 - fy), wb = fx * (8 - fy), wc = (8 - fx) * fy, wd = fx * fy;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int X = ix + x, Y = iy + y;
            dst[y * 8 + x] = static_cast<uint8_t>((wa * at(X, Y) + wb * at(X + 1, Y) + wc * at(X, Y + 1) + wd * at(X + 1, Y + 1) + 32) >> 6);
        }
}

}  // namespace

void MbDecoder::ComputeImplicitWeights() {
    const int cur_poc = frame_->poc;
    for (size_t i = 0; i < ctx_->refs[0].size() && i < 32; ++i)
        for (size_t k = 0; k < ctx_->refs[1].size() && k < 32; ++k) {
            const Frame *p0 = ctx_->refs[0][i];
            const Frame *p1 = ctx_->refs[1][k];
            int w1 = 32;
            const int td_raw = p1->poc - p0->poc;
            if (td_raw != 0 && !p0->long_ref && !p1->long_ref) {
                const int tb = Clip3(-128, 127, cur_poc - p0->poc);
                const int td = Clip3(-128, 127, td_raw);
                const int tx = (16384 + std::abs(td / 2)) / td;
                const int dsf = Clip3(-1024, 1023, (tb * tx + 32) >> 6);
                if ((dsf >> 2) >= -64 && (dsf >> 2) <= 128) w1 = dsf >> 2;
            }
            implicit_w_[i][k] = w1;
        }
}

void MbDecoder::MotionCompensate(int x, int y, int w, int h) {
    const MbInfo &m = *cur_;
    const SliceHeader &sh = *ctx_->sh;
    const int b8 = (y >> 3) * 2 + (x >> 3), blk = (y >> 2) * 4 + (x >> 2);
    const int refs[2] = {m.ref[0][b8], m.ref[1][b8]};
    uint8_t pl[2][256], pcb[2][64], pcr[2][64];
    const int X = mbx_ * 16 + x, Y = mby_ * 16 + y;
    const int cw = frame_->width / 2, ch = frame_->height / 2;
    for (int list = 0; list < 2; ++list) {
        if (refs[list] < 0) continue;
        const Frame &rf = *ctx_->refs[list][static_cast<size_t>(refs[list])];
        const int mvx = m.mv[list][blk][0], mvy = m.mv[list][blk][1];
        LumaInterp(rf, X + (mvx >> 2), Y + (mvy >> 2), mvx & 3, mvy & 3, w, h, pl[list]);
        const int cx = X / 2 + (mvx >> 3), cy = Y / 2 + (mvy >> 3);
        ChromaInterp(rf.u, cw, ch, cx, cy, mvx & 7, mvy & 7, w / 2, h / 2, pcb[list]);
        ChromaInterp(rf.v, cw, ch, cx, cy, mvx & 7, mvy & 7, w / 2, h / 2, pcr[list]);
    }
    const int W = frame_->width;
    uint8_t *dy = &frame_->y[static_cast<size_t>(Y * W + X)];
    uint8_t *du = &frame_->u[static_cast<size_t>((Y / 2) * cw + X / 2)];
    uint8_t *dv = &frame_->v[static_cast<size_t>((Y / 2) * cw + X / 2)];
    const bool implicit = sh.type == kSliceB && ctx_->pps->weighted_bipred_idc == 2;
    const bool explicit_w = sh.has_weights;
    // One plane: combines the per-list predictions into dst.
    auto combine = [&](const uint8_t *p0, const uint8_t *p1, int pstride, uint8_t *dst, int dstride, int bw, int bh, int plane) {
        const bool bi = refs[0] >= 0 && refs[1] >= 0;
        const int single = refs[0] >= 0 ? 0 : 1;
        if (bi && implicit) {
            const int w1 = implicit_w_[refs[0]][refs[1]], w0 = 64 - w1;
            for (int yy = 0; yy < bh; ++yy)
                for (int xx = 0; xx < bw; ++xx)
                    dst[yy * dstride + xx] = Clip1((p0[yy * pstride + xx] * w0 + p1[yy * pstride + xx] * w1 + 32) >> 6);
            return;
        }
        if (explicit_w) {
            const int log_wd = plane == 0 ? sh.luma_log2_denom : sh.chroma_log2_denom;
            auto weight = [&](int list, int &wgt, int &off) {
                const int r = refs[list];
                if (plane == 0) {
                    wgt = sh.luma_weight[list][r];
                    off = sh.luma_offset[list][r];
                } else {
                    wgt = sh.chroma_weight[list][r][plane - 1];
                    off = sh.chroma_offset[list][r][plane - 1];
                }
            };
            if (bi) {
                int w0 = 0, o0 = 0, w1 = 0, o1 = 0;
                weight(0, w0, o0);
                weight(1, w1, o1);
                for (int yy = 0; yy < bh; ++yy)
                    for (int xx = 0; xx < bw; ++xx)
                        dst[yy * dstride + xx] = Clip1(((p0[yy * pstride + xx] * w0 + p1[yy * pstride + xx] * w1 + (1 << log_wd)) >> (log_wd + 1)) +
                                                       ((o0 + o1 + 1) >> 1));
            } else {
                int wgt = 0, off = 0;
                weight(single, wgt, off);
                const uint8_t *p = single == 0 ? p0 : p1;
                for (int yy = 0; yy < bh; ++yy)
                    for (int xx = 0; xx < bw; ++xx) {
                        const int s = p[yy * pstride + xx];
                        dst[yy * dstride + xx] = log_wd >= 1 ? Clip1(((s * wgt + (1 << (log_wd - 1))) >> log_wd) + off) : Clip1(s * wgt + off);
                    }
            }
            return;
        }
        if (bi) {
            for (int yy = 0; yy < bh; ++yy)
                for (int xx = 0; xx < bw; ++xx)
                    dst[yy * dstride + xx] = static_cast<uint8_t>((p0[yy * pstride + xx] + p1[yy * pstride + xx] + 1) >> 1);
        } else {
            const uint8_t *p = single == 0 ? p0 : p1;
            for (int yy = 0; yy < bh; ++yy) std::memcpy(dst + yy * dstride, p + yy * pstride, static_cast<size_t>(bw));
        }
    };
    if (refs[0] < 0 && refs[1] < 0) return;  // (a damaged stream)
    combine(pl[0], pl[1], 16, dy, W, w, h, 0);
    combine(pcb[0], pcb[1], 8, du, cw, w / 2, h / 2, 1);
    combine(pcr[0], pcr[1], 8, dv, cw, w / 2, h / 2, 2);
}

}  // namespace h264::detail
