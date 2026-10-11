// The macroblock layer of the H.264 decoder (h264_slice.h): slice data
// parsing (CAVLC here; CABAC binarizations in h264_cabac_mb.cpp), intra and
// inter prediction, the inverse transforms, and the deblocking filter.
// Clause numbers refer to ITU-T H.264.

#include "h264_slice.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "h264_cabac.h"
#include "h264_mb.h"

namespace h264::detail {

namespace {

// --- CAVLC code tables ---------------------------------------------------------------

struct CavlcVlcs {
    Vlc coeff_token[4];
    Vlc chroma_dc_token;
    Vlc total_zeros[15];
    Vlc chroma_dc_total_zeros[3];
    Vlc run[7];
    bool ok = true;
};

const CavlcVlcs &Vlcs() {
    static const CavlcVlcs v = [] {
        CavlcVlcs t;
        for (int i = 0; i < 4; ++i) t.ok &= t.coeff_token[i].Build(kCoeffTokenLen[i], kCoeffTokenBits[i], 4 * 17);
        t.ok &= t.chroma_dc_token.Build(kChromaDcCoeffTokenLen, kChromaDcCoeffTokenBits, 4 * 5);
        for (int i = 0; i < 15; ++i) t.ok &= t.total_zeros[i].Build(kTotalZerosLen[i], kTotalZerosBits[i], 16 - i);
        for (int i = 0; i < 3; ++i) t.ok &= t.chroma_dc_total_zeros[i].Build(kChromaDcTotalZerosLen[i], kChromaDcTotalZerosBits[i], 4 - i);
        for (int i = 0; i < 7; ++i) t.ok &= t.run[i].Build(kRunLen[i], kRunBits[i], i < 6 ? i + 2 : 15);
        return t;
    }();
    return v;
}

}  // namespace

bool CavlcTablesOk() { return Vlcs().ok; }

// --- Macroblock types (Tables 7-11, 7-13, 7-14, 7-17, 7-18) -------------------------

namespace {

struct InterType {
    int part;
    uint8_t pred[2];  // per partition: bit 0 list 0, bit 1 list 1
};

const InterType kPTypes[5] = {
    {kPart16x16, {1, 0}}, {kPart16x8, {1, 1}}, {kPart8x16, {1, 1}}, {kPart8x8, {0, 0}}, {kPart8x8, {0, 0}},
};
// B mb_type 0..22 (0 = B_Direct_16x16, 22 = B_8x8).
const InterType kBTypes[23] = {
    {kPart16x16, {0, 0}}, {kPart16x16, {1, 0}}, {kPart16x16, {2, 0}}, {kPart16x16, {3, 0}}, {kPart16x8, {1, 1}},
    {kPart8x16, {1, 1}},  {kPart16x8, {2, 2}},  {kPart8x16, {2, 2}},  {kPart16x8, {1, 2}},  {kPart8x16, {1, 2}},
    {kPart16x8, {2, 1}},  {kPart8x16, {2, 1}},  {kPart16x8, {1, 3}},  {kPart8x16, {1, 3}},  {kPart16x8, {2, 3}},
    {kPart8x16, {2, 3}},  {kPart16x8, {3, 1}},  {kPart8x16, {3, 1}},  {kPart16x8, {3, 2}},  {kPart8x16, {3, 2}},
    {kPart16x8, {3, 3}},  {kPart8x16, {3, 3}},  {kPart8x8, {0, 0}},
};

struct SubType {
    int shape;  // kSub8x8 .. kSub4x4
    uint8_t pred;
};
const SubType kPSubTypes[4] = {{kSub8x8, 1}, {kSub8x4, 1}, {kSub4x8, 1}, {kSub4x4, 1}};
// B sub_mb_type 0..12 (0 = B_Direct_8x8: shape is decided by direct_8x8_inference).
const SubType kBSubTypes[13] = {
    {kSub8x8, 0}, {kSub8x8, 1}, {kSub8x8, 2}, {kSub8x8, 3}, {kSub8x4, 1}, {kSub4x8, 1}, {kSub8x4, 2},
    {kSub4x8, 2}, {kSub8x4, 3}, {kSub4x8, 3}, {kSub4x4, 1}, {kSub4x4, 2}, {kSub4x4, 3},
};

}  // namespace

void SetIntraMbType(int t, MbData *d) {
    // I mb_type: 0 I_NxN, 1..24 I_16x16, 25 I_PCM.
    d->inter = false;
    if (t == 0) {
        d->kind = MbData::kI4;  // (kI8 once transform_size_8x8_flag is read)
    } else if (t == 25) {
        d->kind = MbData::kPcm;
    } else {
        d->kind = MbData::kI16;
        d->i16_mode = (t - 1) % 4;
        d->cbp = static_cast<uint8_t>((((t - 1) / 4) % 3) << 4 | ((t >= 13) ? 15 : 0));
    }
}

bool SetInterMbType(int slice_type, int t, MbData *d) {
    d->inter = true;
    d->kind = MbData::kInter;
    if (slice_type == kSliceP) {
        if (t > 4) return false;
        d->part = kPTypes[t].part;
        d->part_pred[0] = kPTypes[t].pred[0];
        d->part_pred[1] = kPTypes[t].pred[1];
        d->ref0_only = t == 4;
        return true;
    }
    if (t > 22) return false;
    if (t == 0) {
        d->kind = MbData::kBDirect16;
        d->part = kPart16x16;
        return true;
    }
    d->part = kBTypes[t].part;
    d->part_pred[0] = kBTypes[t].pred[0];
    d->part_pred[1] = kBTypes[t].pred[1];
    return true;
}

bool SetSubMbType(int slice_type, int i, int t, MbData *d) {
    if (slice_type == kSliceP) {
        if (t > 3) return false;
        d->sub_shape[i] = kPSubTypes[t].shape;
        d->sub_pred[i] = kPSubTypes[t].pred;
        d->sub_direct[i] = false;
        return true;
    }
    if (t > 12) return false;
    d->sub_shape[i] = kBSubTypes[t].shape;
    d->sub_pred[i] = kBSubTypes[t].pred;
    d->sub_direct[i] = t == 0;
    return true;
}

// --- CAVLC macroblock parsing (7.3.5, 9.2) -------------------------------------------

namespace {

// residual_block_cavlc: up to `max_coeff` levels at scan positions
// start..start+max_coeff-1, written through `scan` into `block`. `nc` is
// the coeff_token table selector (-1: chroma DC). Returns TotalCoeff, or
// -1 on a damaged block.
int ReadResidualCavlc(BitReader &br, int nc, int16_t *block, const uint8_t *scan, int start, int max_coeff) {
    const CavlcVlcs &v = Vlcs();
    int token = 0;
    if (nc < 0) {
        token = v.chroma_dc_token.Read(br);
    } else {
        token = v.coeff_token[nc < 2 ? 0 : (nc < 4 ? 1 : (nc < 8 ? 2 : 3))].Read(br);
    }
    if (token < 0) return -1;
    const int total = token >> 2, trailing = token & 3;
    if (total == 0) return 0;
    if (total > max_coeff) return -1;
    int levels[16];
    int suffix_length = (total > 10 && trailing < 3) ? 1 : 0;
    for (int i = 0; i < total; ++i) {
        if (i < trailing) {
            levels[i] = br.U1() ? -1 : 1;
            continue;
        }
        int prefix = 0;
        while (br.U1() == 0) {
            if (++prefix > 31 || br.overrun()) return -1;
        }
        int level_code = std::min(15, prefix) << suffix_length;
        if (suffix_length > 0 || prefix >= 14) {
            int size = suffix_length;
            if (prefix == 14 && suffix_length == 0) size = 4;
            if (prefix >= 15) size = prefix - 3;
            if (size > 0) level_code += static_cast<int>(br.U(size));
        }
        if (prefix >= 15 && suffix_length == 0) level_code += 15;
        if (prefix >= 16) level_code += (1 << (prefix - 3)) - 4096;
        if (i == trailing && trailing < 3) level_code += 2;
        levels[i] = (level_code % 2 == 0) ? (level_code + 2) >> 1 : (-level_code - 1) >> 1;
        if (suffix_length == 0) suffix_length = 1;
        if (std::abs(levels[i]) > (3 << (suffix_length - 1)) && suffix_length < 6) ++suffix_length;
    }
    int zeros_left = 0;
    if (total < max_coeff) {
        zeros_left = nc < 0 ? v.chroma_dc_total_zeros[total - 1].Read(br) : v.total_zeros[total - 1].Read(br);
        if (zeros_left < 0) return -1;
    }
    if (zeros_left + total > max_coeff) return -1;
    int pos = total + zeros_left - 1;  // scan index (relative to start) of levels[0]
    for (int i = 0; i < total; ++i) {
        block[scan[start + pos]] = static_cast<int16_t>(levels[i]);
        if (i == total - 1) break;
        int run = 0;
        if (zeros_left > 0) {
            run = v.run[std::min(zeros_left, 7) - 1].Read(br);
            if (run < 0 || run > zeros_left) return -1;
        }
        zeros_left -= run;
        pos -= run + 1;
    }
    return total;
}

}  // namespace

int MbDecoder::LumaNc(int bx, int by) const {
    int n = 0, count = 0;
    if (bx > 0) {
        n += cur_->nz[by * 4 + bx - 1];
        ++count;
    } else if (const MbInfo *a = Neighbor(-1, 0)) {
        n += a->nz[by * 4 + 3];
        ++count;
    }
    if (by > 0) {
        n += cur_->nz[(by - 1) * 4 + bx];
        ++count;
    } else if (const MbInfo *b = Neighbor(0, -1)) {
        n += b->nz[12 + bx];
        ++count;
    }
    return count == 2 ? (n + 1) >> 1 : n;
}

int MbDecoder::ChromaNc(int c, int bx, int by) const {
    int n = 0, count = 0;
    if (bx > 0) {
        n += cur_->nzc[c][by * 2];
        ++count;
    } else if (const MbInfo *a = Neighbor(-1, 0)) {
        n += a->nzc[c][by * 2 + 1];
        ++count;
    }
    if (by > 0) {
        n += cur_->nzc[c][bx];
        ++count;
    } else if (const MbInfo *b = Neighbor(0, -1)) {
        n += b->nzc[c][2 + bx];
        ++count;
    }
    return count == 2 ? (n + 1) >> 1 : n;
}

bool MbDecoder::ReadRefIdx(BitReader &br, int list, int *out) const {
    const int n = ctx_->sh->num_ref_idx[list];
    if (n <= 1) {
        *out = 0;
        return true;
    }
    const uint32_t v = n == 2 ? (br.U1() ^ 1u) : br.Ue();
    if (v >= static_cast<uint32_t>(n)) return false;
    *out = static_cast<int>(v);
    return true;
}

bool MbDecoder::ParseCavlc(BitReader &br, MbData *d) {
    const SliceHeader &sh = *ctx_->sh;
    uint32_t mb_type = br.Ue();
    if (sh.type == kSliceI) {
        if (mb_type > 25) return false;
        SetIntraMbType(static_cast<int>(mb_type), d);
    } else if (sh.type == kSliceP) {
        if (mb_type < 5) {
            SetInterMbType(kSliceP, static_cast<int>(mb_type), d);
        } else {
            if (mb_type - 5 > 25) return false;
            SetIntraMbType(static_cast<int>(mb_type - 5), d);
        }
    } else {
        if (mb_type < 23) {
            SetInterMbType(kSliceB, static_cast<int>(mb_type), d);
        } else {
            if (mb_type - 23 > 25) return false;
            SetIntraMbType(static_cast<int>(mb_type - 23), d);
        }
    }
    if (d->kind == MbData::kPcm) {
        while (!br.ByteAligned()) br.U1();  // pcm_alignment_zero_bit
        for (int i = 0; i < 384; ++i) d->pcm[i] = static_cast<uint8_t>(br.U(8));
        return !br.overrun();
    }
    const Pps &pps = *ctx_->pps;
    if (d->kind == MbData::kI4) {
        if (pps.transform_8x8_mode && br.U1()) {
            d->kind = MbData::kI8;
            d->t8 = true;
        }
        const int n = d->kind == MbData::kI8 ? 4 : 16;
        for (int i = 0; i < n; ++i) {
            d->prev_pred_flag[i] = br.U1() != 0;
            d->rem_pred_mode[i] = d->prev_pred_flag[i] ? 0 : static_cast<int8_t>(br.U(3));
        }
    }
    if (!d->inter) {
        d->chroma_mode = static_cast<int>(br.Ue());
        if (d->chroma_mode > 3) return false;
    } else if (d->kind == MbData::kInter) {
        if (d->part == kPart8x8) {
            for (int i = 0; i < 4; ++i)
                if (!SetSubMbType(sh.type, i, static_cast<int>(br.Ue()), d)) return false;
            for (int list = 0; list < 2; ++list)
                for (int i = 0; i < 4; ++i) {
                    d->ref[list][i] = -1;
                    if (d->sub_direct[i] || !(d->sub_pred[i] & (1 << list))) continue;
                    if (d->ref0_only) {
                        d->ref[list][i] = 0;
                    } else if (!ReadRefIdx(br, list, &d->ref[list][i])) {
                        return false;
                    }
                }
            for (int list = 0; list < 2; ++list)
                for (int i = 0; i < 4; ++i) {
                    if (d->sub_direct[i] || !(d->sub_pred[i] & (1 << list))) continue;
                    const int parts = SubPartCount(d->sub_shape[i]);
                    for (int s = 0; s < parts; ++s) {
                        d->mvd[list][i * 4 + s][0] = static_cast<int16_t>(br.Se());
                        d->mvd[list][i * 4 + s][1] = static_cast<int16_t>(br.Se());
                    }
                }
        } else {
            const int parts = d->part == kPart16x16 ? 1 : 2;
            for (int list = 0; list < 2; ++list)
                for (int p = 0; p < parts; ++p) {
                    d->ref[list][p] = -1;
                    if (d->part_pred[p] & (1 << list))
                        if (!ReadRefIdx(br, list, &d->ref[list][p])) return false;
                }
            for (int list = 0; list < 2; ++list)
                for (int p = 0; p < parts; ++p) {
                    if (!(d->part_pred[p] & (1 << list))) continue;
                    d->mvd[list][p][0] = static_cast<int16_t>(br.Se());
                    d->mvd[list][p][1] = static_cast<int16_t>(br.Se());
                }
        }
    }
    if (d->kind != MbData::kI16) {
        const uint32_t code = br.Ue();
        if (code > 47) return false;
        d->cbp = d->inter || d->kind == MbData::kBDirect16 ? kCbpInter[code] : kCbpIntra[code];
        if ((d->cbp & 15) && pps.transform_8x8_mode && d->kind != MbData::kI4 && d->kind != MbData::kI8 && Allows8x8Transform(*d))
            d->t8 = br.U1() != 0;
    }
    if ((d->cbp & 0x3f) || d->kind == MbData::kI16) {
        d->qp_delta = br.Se();
        if (d->qp_delta < -26 || d->qp_delta > 25) return false;
    }
    SetQp(d);
    return ReadResidualCavlcMb(br, d);
}

bool MbDecoder::ReadResidualCavlcMb(BitReader &br, MbData *d) {
    MbInfo &m = *cur_;
    if (d->kind == MbData::kI16) {
        std::memset(d->dc_luma, 0, sizeof(d->dc_luma));
        const int n = ReadResidualCavlc(br, LumaNc(0, 0), d->dc_luma, kZigzag4x4, 0, 16);
        if (n < 0) return false;
        d->has_dc_luma = n > 0;
    }
    for (int b8 = 0; b8 < 4; ++b8) {
        if (!(d->cbp & (1 << b8))) continue;
        for (int i = 0; i < 4; ++i) {
            const int bx = (b8 & 1) * 2 + (i & 1), by = (b8 >> 1) * 2 + (i >> 1);
            const int blk = by * 4 + bx;
            int n = 0;
            if (d->t8) {
                // CAVLC carries an 8x8 block as four interleaved 4x4 ones.
                if (i == 0) std::memset(d->coef8[b8], 0, sizeof(d->coef8[b8]));
                uint8_t scan[16];
                for (int k = 0; k < 16; ++k) scan[k] = kZigzag8x8[4 * k + i];
                n = ReadResidualCavlc(br, LumaNc(bx, by), d->coef8[b8], scan, 0, 16);
                if (n > 0) d->coded8 |= static_cast<uint8_t>(1 << b8);
            } else {
                std::memset(d->coef[blk], 0, sizeof(d->coef[blk]));
                if (d->kind == MbData::kI16) {
                    n = ReadResidualCavlc(br, LumaNc(bx, by), d->coef[blk], kZigzag4x4, 1, 15);
                } else {
                    n = ReadResidualCavlc(br, LumaNc(bx, by), d->coef[blk], kZigzag4x4, 0, 16);
                }
                if (n > 0) d->coded4 |= static_cast<uint16_t>(1 << blk);
            }
            if (n < 0) return false;
            m.nz[blk] = static_cast<uint8_t>(n);
        }
    }
    if (d->cbp & 0x30) {
        for (int c = 0; c < 2; ++c) {
            std::memset(d->dc_chroma[c], 0, sizeof(d->dc_chroma[c]));
            static const uint8_t kRaster[4] = {0, 1, 2, 3};
            const int n = ReadResidualCavlc(br, -1, d->dc_chroma[c], kRaster, 0, 4);
            if (n < 0) return false;
            d->has_dc_chroma[c] = n > 0;
        }
    }
    if (d->cbp & 0x20) {
        for (int c = 0; c < 2; ++c)
            for (int b = 0; b < 4; ++b) {
                std::memset(d->ac_chroma[c][b], 0, sizeof(d->ac_chroma[c][b]));
                const int n = ReadResidualCavlc(br, ChromaNc(c, b & 1, b >> 1), d->ac_chroma[c][b], kZigzag4x4, 1, 15);
                if (n < 0) return false;
                m.nzc[c][b] = static_cast<uint8_t>(n);
                if (n > 0) d->coded_chroma[c] |= static_cast<uint8_t>(1 << b);
            }
    }
    return !br.overrun();
}

// --- The slice ----------------------------------------------------------------------

struct SliceDecoder::Impl {
    MbDecoder mb;
};

SliceDecoder::SliceDecoder() : impl_(new Impl) {}
SliceDecoder::~SliceDecoder() { delete impl_; }

bool SliceDecoder::DecodeSlice(const SliceContext &ctx, BitReader &br, std::string *error) {
    if (!Vlcs().ok) {
        *error = "internal: CAVLC tables";
        return false;
    }
    return impl_->mb.DecodeSlice(ctx, br, error);
}

void SliceDecoder::Deblock(Frame &f, const Pps &pps) { DeblockPicture(f, pps); }

}  // namespace h264::detail
