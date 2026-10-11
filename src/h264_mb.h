#pragma once

// The H.264 decoder's per-macroblock machinery, shared by its parsing
// (h264_slice.cpp: CAVLC; h264_cabac_mb.cpp: CABAC) and reconstruction
// (h264_mb.cpp: prediction and residual; h264_deblock.cpp) units.

#include <cstdint>
#include <string>

#include "h264_internal.h"
#include "h264_slice.h"

namespace h264::detail {

enum PartShape { kPart16x16, kPart16x8, kPart8x16, kPart8x8 };
enum SubShape { kSub8x8, kSub8x4, kSub4x8, kSub4x4 };

inline int SubPartCount(int shape) { return shape == kSub8x8 ? 1 : (shape == kSub4x4 ? 4 : 2); }

// One macroblock's syntax, as either entropy decoder leaves it for the
// reconstruction.
struct MbData {
    enum Kind { kI4, kI8, kI16, kPcm, kInter, kPSkip, kBSkip, kBDirect16 };
    Kind kind = kI4;
    bool inter = false;
    bool t8 = false;
    int i16_mode = 0;
    int chroma_mode = 0;
    uint8_t cbp = 0;  // bits 0..3 luma 8x8, bits 4..5 chroma (0, 1 = DC, 2 = DC + AC)
    int qp_delta = 0;
    // Intra_4x4 / 8x8 prediction mode syntax, by luma4x4BlkIdx / luma8x8BlkIdx.
    bool prev_pred_flag[16] = {};
    int8_t rem_pred_mode[16] = {};
    // Inter partitions.
    int part = kPart16x16;
    uint8_t part_pred[2] = {0, 0};
    bool ref0_only = false;  // P_8x8ref0
    int sub_shape[4] = {};
    uint8_t sub_pred[4] = {};
    bool sub_direct[4] = {};
    int ref[2][4] = {{-1, -1, -1, -1}, {-1, -1, -1, -1}};  // by partition (16x16/16x8/8x16) or 8x8 block
    int16_t mvd[2][16][2] = {};                              // by partition, or 8x8 block * 4 + sub-partition
    // Residual: transform coefficient levels at raster positions, not yet
    // scaled. Only blocks flagged in coded4 / coded8 / coded_chroma (and
    // the DC flags) hold meaningful data.
    int16_t coef[16][16];  // luma 4x4 blocks, raster block index
    int16_t coef8[4][64];  // luma 8x8 blocks
    int16_t dc_luma[16];   // Intra_16x16 DC, raster over the 4x4 blocks
    int16_t dc_chroma[2][4];
    int16_t ac_chroma[2][4][16];
    uint16_t coded4 = 0;
    uint8_t coded8 = 0;
    uint8_t coded_chroma[2] = {0, 0};
    bool has_dc_luma = false;
    bool has_dc_chroma[2] = {false, false};
    uint8_t pcm[384];

    /** @brief Clears the syntax for the next macroblock (not the coefficient arrays, which are cleared per coded block). */
    void Reset() {
        kind = kI4;
        inter = t8 = false;
        i16_mode = chroma_mode = 0;
        cbp = 0;
        qp_delta = 0;
        part = kPart16x16;
        part_pred[0] = part_pred[1] = 0;
        ref0_only = false;
        for (int i = 0; i < 4; ++i) {
            sub_shape[i] = 0;
            sub_pred[i] = 0;
            sub_direct[i] = false;
            ref[0][i] = ref[1][i] = -1;
        }
        for (auto &l : mvd)
            for (auto &v : l) v[0] = v[1] = 0;
        coded4 = 0;
        coded8 = 0;
        coded_chroma[0] = coded_chroma[1] = 0;
        has_dc_luma = false;
        has_dc_chroma[0] = has_dc_chroma[1] = false;
    }
};

void SetIntraMbType(int t, MbData *d);
bool SetInterMbType(int slice_type, int t, MbData *d);
bool SetSubMbType(int slice_type, int i, int t, MbData *d);
bool CavlcTablesOk();

class Cabac;

// Decodes the macroblocks of a slice into its picture.
class MbDecoder {
public:
    MbDecoder();
    ~MbDecoder();
    MbDecoder(const MbDecoder &) = delete;
    MbDecoder &operator=(const MbDecoder &) = delete;

    bool DecodeSlice(const SliceContext &ctx, BitReader &br, std::string *error);

private:
    friend class CabacMbParser;
    // Neighbouring macroblock at (dx, dy) macroblocks away, when it is
    // available (inside the picture and the current slice).
    const MbInfo *Neighbor(int dx, int dy) const {
        const int x = mbx_ + dx, y = mby_ + dy;
        if (x < 0 || y < 0 || x >= width_mbs_) return nullptr;
        const MbInfo *m = &mbs_[static_cast<size_t>(y * width_mbs_ + x)];
        return m->slice_num == slice_num_ ? m : nullptr;
    }
    // ... and available for intra prediction (constrained_intra_pred).
    const MbInfo *IntraNeighbor(int dx, int dy) const {
        const MbInfo *m = Neighbor(dx, dy);
        return m && (m->intra || !ctx_->pps->constrained_intra_pred) ? m : nullptr;
    }

    // CABAC slice data (h264_cabac_mb.cpp).
    bool DecodeSliceCabac(BitReader &br, int addr, std::string *error);
    // CAVLC (h264_slice.cpp).
    bool ParseCavlc(BitReader &br, MbData *d);
    bool ReadResidualCavlcMb(BitReader &br, MbData *d);
    bool ReadRefIdx(BitReader &br, int list, int *out) const;
    int LumaNc(int bx, int by) const;
    int ChromaNc(int c, int bx, int by) const;

    // Reconstruction (h264_mb.cpp).
    void StartMb(int addr);
    void SetQp(MbData *d);
    bool Allows8x8Transform(const MbData &d) const;
    void Reconstruct(MbData &d);
    void IntraPred4x4Modes(MbData &d);
    void IntraPred8x8Modes(MbData &d);
    void PredictIntra4x4(int bx, int by, int mode);
    void PredictIntra8x8(int x8, int y8, int mode);
    void PredictIntra16x16(int mode);
    void PredictIntraChroma(int mode);
    void AddLumaResidual(const MbData &d);
    void AddLuma4x4(const MbData &d, int blk);
    void AddLuma8x8(const MbData &d, int b8);
    void AddChromaResidual(const MbData &d);
    // Motion vectors (h264_mb.cpp).
    struct Nb {
        bool avail = false;
        int ref = -1;
        int mv[2] = {0, 0};
    };
    Nb NeighborMotion(int list, int x, int y) const;
    void PredictMv(int list, int ref, int x, int y, int w, int h, int shape, int part, int mvp[2]) const;
    void PSkipMv(int mv[2]) const;
    void SetMotion(int list, int x, int y, int w, int h, int ref, const int mv[2]);
    void DirectSpatial(int b8_first, int b8_last);
    void DirectTemporal(int b8_first, int b8_last);
    void Direct(int b8_first, int b8_last) {
        if (ctx_->sh->direct_spatial) {
            DirectSpatial(b8_first, b8_last);
        } else {
            DirectTemporal(b8_first, b8_last);
        }
    }
    void DeriveMotion(MbData &d);
    void MotionCompensate(int x, int y, int w, int h);
    void ComputeImplicitWeights();

    const SliceContext *ctx_ = nullptr;
    Frame *frame_ = nullptr;
    MbInfo *mbs_ = nullptr;
    MbInfo *cur_ = nullptr;
    int width_mbs_ = 0, height_mbs_ = 0;
    int mbx_ = 0, mby_ = 0;
    int slice_num_ = 0;
    int prev_qp_ = 0;
    uint16_t decoded4_ = 0;  // this macroblock's 4x4 blocks whose motion is set (8.4.1.3 availability)
    bool spatial_cache_valid_ = false;
    bool spatial_zero_ = false;
    int spatial_ref_[2] = {-1, -1};
    int spatial_mv_[2][2] = {};
    int implicit_w_[32][32] = {};
    // Dequantisation tables for the slice's QP-independent part: LevelScale
    // (8.5.9) by list and qP % 6.
    int level4_[6][6][16] = {};
    int level8_[2][6][64] = {};
    Cabac *cabac_ = nullptr;
    const uint8_t *cabac_data_ = nullptr;  // the engine's current input (moves past I_PCM samples)
    size_t cabac_len_ = 0;
    MbData data_;
};

// Deblocking filter over a decoded picture (h264_deblock.cpp).
void DeblockPicture(Frame &f, const Pps &pps);

}  // namespace h264::detail
