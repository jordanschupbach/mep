#pragma once

// Internals shared by the H.264 decoder's translation units
// (h264_decoder.cpp, h264_tables.cpp, h264_cabac.cpp) and its test. Not
// part of the player-facing API -- that is h264_decoder.h.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "h264_decoder.h"

namespace h264::detail {

// --- Tables (h264_tables.cpp; ITU-T H.264 clause numbers alongside) ------------

// CAVLC (9.2): coeff_token by nC class [0..3] (0<=nC<2, 2<=nC<4, 4<=nC<8,
// 8<=nC), indexed TotalCoeff * 4 + TrailingOnes; length 0 = no such code.
extern const uint8_t kCoeffTokenLen[4][4 * 17];
extern const uint8_t kCoeffTokenBits[4][4 * 17];
// coeff_token for chroma DC 4:2:0 (nC == -1), TotalCoeff 0..4.
extern const uint8_t kChromaDcCoeffTokenLen[4 * 5];
extern const uint8_t kChromaDcCoeffTokenBits[4 * 5];
// total_zeros for 4x4 blocks, by TotalCoeff - 1 (Tables 9-7, 9-8).
extern const uint8_t kTotalZerosLen[15][16];
extern const uint8_t kTotalZerosBits[15][16];
// total_zeros for 2x2 chroma DC, by TotalCoeff - 1 (Table 9-9a).
extern const uint8_t kChromaDcTotalZerosLen[3][4];
extern const uint8_t kChromaDcTotalZerosBits[3][4];
// run_before by min(zerosLeft, 7) - 1 (Table 9-10).
extern const uint8_t kRunLen[7][16];
extern const uint8_t kRunBits[7][16];
// coded_block_pattern me(v) mapping for ChromaArrayType 1/2 (Table 9-4):
// [codeNum] -> cbp, Intra_4x4/8x8 and Inter.
extern const uint8_t kCbpIntra[48];
extern const uint8_t kCbpInter[48];
// Deblocking (Tables 8-16, 8-17): alpha', beta' by indexA/B, tC0' by
// indexA and bS - 1.
extern const uint8_t kAlpha[52];
extern const uint8_t kBeta[52];
extern const uint8_t kTc0[52][3];
// QPc from qPi (Table 8-15), qPi 0..51.
extern const uint8_t kChromaQp[52];
// Frame zig-zag scans: scan index -> raster position (8.5.6).
extern const uint8_t kZigzag4x4[16];
extern const uint8_t kZigzag8x8[64];
// normAdjust4x4/8x8's v tables (8.5.9).
extern const uint8_t kNormAdjust4x4[6][3];
extern const uint8_t kNormAdjust8x8[6][6];
// Default scaling lists in zig-zag order (Tables 7-3, 7-4).
extern const uint8_t kDefault4x4Intra[16];
extern const uint8_t kDefault4x4Inter[16];
extern const uint8_t kDefault8x8Intra[64];
extern const uint8_t kDefault8x8Inter[64];

// --- Bitstream ----------------------------------------------------------------

// Strips emulation_prevention_three_byte (00 00 03) from a NAL unit's
// payload (after its header byte) into `out`.
void UnescapeRbsp(const uint8_t *data, size_t len, std::vector<uint8_t> *out);

// MSB-first reader over an RBSP. Reads past the end yield zeros and set
// overrun(), so a corrupt slice can't read out of bounds.
class BitReader {
public:
    BitReader() = default;
    BitReader(const uint8_t *data, size_t len) : data_(data), len_(len) {}
    /** @brief Reads `n` (0..32) bits. */
    uint32_t U(int n) {
        if (n == 0) return 0;
        uint32_t v = Peek(n);
        Skip(n);
        return v;
    }
    /** @brief Reads one bit. */
    uint32_t U1() {
        if (pos_ >= len_ * 8) {
            overrun_ = true;
            ++pos_;
            return 0;
        }
        const uint32_t v = (data_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1u;
        ++pos_;
        return v;
    }
    /** @brief The next `n` (1..32) bits without consuming them. */
    uint32_t Peek(int n) const {
        uint64_t v = 0;
        const size_t byte = pos_ >> 3;
        for (size_t i = 0; i < 5; ++i) v = (v << 8) | (byte + i < len_ ? data_[byte + i] : 0u);
        v <<= 24 + (pos_ & 7);
        return static_cast<uint32_t>(v >> (64 - n));
    }
    /** @brief Consumes `n` bits. */
    void Skip(int n) {
        pos_ += static_cast<size_t>(n);
        if (pos_ > len_ * 8) overrun_ = true;
    }
    /**
     * @brief ue(v), Exp-Golomb, saturated at kMaxUe: no syntax element the decoder uses
     * comes near it, so a corrupt value can't wrap when cast to int.
     */
    uint32_t Ue() {
        int zeros = 0;
        while (U1() == 0) {
            if (++zeros > 31 || overrun_) {
                overrun_ = true;
                return 0;
            }
        }
        const uint32_t v = zeros == 0 ? 0 : ((1u << zeros) - 1u + U(zeros));
        return v > kMaxUe ? kMaxUe : v;
    }
    static constexpr uint32_t kMaxUe = 1u << 24;
    /** @brief se(v). */
    int32_t Se() {
        const uint32_t k = Ue();
        return (k & 1u) ? static_cast<int32_t>((k + 1) / 2) : -static_cast<int32_t>(k / 2);
    }
    /** @brief more_rbsp_data(): anything before the rbsp_stop_one_bit. */
    bool MoreRbspData() const;
    size_t BitPos() const { return pos_; }
    size_t BitsLeft() const { return pos_ >= len_ * 8 ? 0 : len_ * 8 - pos_; }
    bool ByteAligned() const { return (pos_ & 7) == 0; }
    const uint8_t *Data() const { return data_; }
    size_t Size() const { return len_; }
    void SetBitPos(size_t p) { pos_ = p; }
    bool overrun() const { return overrun_; }

private:
    const uint8_t *data_ = nullptr;
    size_t len_ = 0;
    size_t pos_ = 0;
    bool overrun_ = false;
};

// A variable-length code decoded by one table lookup on the next
// `max_len` bits. Entries: value | (length << 8); 0 = invalid code.
class Vlc {
public:
    /** @brief Builds from parallel length/code arrays; entry i decodes to value i. Returns false if two codes collide. */
    bool Build(const uint8_t *lens, const uint8_t *bits, int count);
    /** @brief Decodes one symbol; -1 on an invalid code. */
    int Read(BitReader &br) const {
        const uint16_t e = table_[br.Peek(max_len_)];
        if (e == 0) {
            br.Skip(1);
            return -1;
        }
        br.Skip(e >> 8);
        return e & 0xff;
    }
    int max_len() const { return max_len_; }

private:
    int max_len_ = 1;
    std::vector<uint16_t> table_;
};

// --- Parameter sets (7.3.2.1, 7.3.2.2) ----------------------------------------------

struct ScalingLists {
    uint8_t m4[6][16];  // raster order; Y/Cb/Cr intra, Y/Cb/Cr inter
    uint8_t m8[2][64];  // raster order; Y intra, Y inter
};

struct Sps {
    bool valid = false;
    int profile_idc = 0;
    int constraint_flags = 0;
    int level_idc = 0;
    int chroma_format_idc = 1;
    int bit_depth_luma = 8, bit_depth_chroma = 8;
    bool transform_bypass = false;
    bool scaling_present = false;
    ScalingLists scaling{};
    int log2_max_frame_num = 4;
    int poc_type = 0;
    int log2_max_poc_lsb = 4;
    bool delta_pic_order_always_zero = false;
    int offset_for_non_ref_pic = 0;
    int offset_for_top_to_bottom_field = 0;
    std::vector<int> offset_for_ref_frame;
    int max_num_ref_frames = 0;
    bool gaps_allowed = false;
    int width_mbs = 0, height_mbs = 0;
    bool frame_mbs_only = true;
    bool direct_8x8_inference = false;
    int crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;  // in luma samples
    int matrix = 2;
    bool full_range = false;
    int max_num_reorder = -1;  // -1: not signalled
    int max_dec_frame_buffering = -1;
};

struct Pps {
    bool valid = false;
    int sps_id = 0;
    bool cabac = false;
    bool bottom_field_pic_order_present = false;
    int num_ref_idx_default[2] = {1, 1};
    bool weighted_pred = false;
    int weighted_bipred_idc = 0;
    int pic_init_qp = 26;
    int chroma_qp_offset[2] = {0, 0};
    bool deblocking_control_present = false;
    bool constrained_intra_pred = false;
    bool redundant_pic_cnt_present = false;
    bool transform_8x8_mode = false;
    bool scaling_present = false;
    ScalingLists scaling{};  // effective lists (after the SPS/default fall-backs)
};

// --- Slices ------------------------------------------------------------------------

enum SliceType { kSliceP = 0, kSliceB = 1, kSliceI = 2 };

struct RefPicModification {
    int idc = 3;
    uint32_t value = 0;
};

struct Mmco {
    int op = 0;
    uint32_t a = 0, b = 0;
};

struct SliceHeader {
    int nal_ref_idc = 0;
    bool idr = false;
    int first_mb = 0;
    int type = kSliceI;
    int pps_id = 0;
    int frame_num = 0;
    int idr_pic_id = 0;
    int poc_lsb = 0;
    int delta_poc_bottom = 0;
    int delta_poc[2] = {0, 0};
    bool direct_spatial = false;
    int num_ref_idx[2] = {0, 0};
    std::vector<RefPicModification> mods[2];
    // pred_weight_table: [list][ref] luma weight/offset, chroma [cb/cr] weight/offset.
    int luma_log2_denom = 0, chroma_log2_denom = 0;
    bool has_weights = false;
    int16_t luma_weight[2][32] = {}, luma_offset[2][32] = {};
    int16_t chroma_weight[2][32][2] = {}, chroma_offset[2][32][2] = {};
    bool no_output_of_prior_pics = false;
    bool long_term_reference = false;
    bool adaptive_marking = false;
    std::vector<Mmco> mmcos;
    int cabac_init_idc = 0;
    int qp = 26;
    int disable_deblocking = 0;
    int alpha_offset = 0, beta_offset = 0;  // already doubled (FilterOffsetA/B)
};

// --- Pictures ----------------------------------------------------------------------

// Per-macroblock state kept with a picture: what neighbours (CAVLC nC,
// CABAC contexts, prediction), the deblocking filter and temporal direct
// prediction of later pictures need.
struct MbInfo {
    int slice_num = -1;  // -1: not decoded
    bool intra = false;
    bool i16 = false;
    bool i4 = false;      // Intra_4x4 or Intra_8x8 (has per-block pred modes)
    bool pcm = false;
    bool skip = false;
    bool t8 = false;      // transform_size_8x8_flag
    bool direct16 = false;  // B_Skip / B_Direct_16x16 (CABAC context)
    int8_t qp = 0;        // QPy
    int8_t qpc[2] = {0, 0};
    uint8_t cbp = 0;
    uint8_t chroma_pred = 0;
    uint8_t nz[16] = {};       // luma total_coeff (CAVLC) / coded (CABAC), raster 4x4
    uint8_t nzc[2][4] = {};    // chroma AC, raster 2x2
    uint8_t cbf_dc = 0;        // CABAC coded_block_flag of the DC blocks: bit0 luma DC, bit1 Cb, bit2 Cr
    uint16_t coded = 0;        // luma 4x4 blocks with nonzero coefficients (deblocking)
    int8_t ipred[16] = {};     // Intra4x4/8x8 pred modes, raster 4x4 (-1: none)
    int8_t ref[2][4] = {{-1, -1, -1, -1}, {-1, -1, -1, -1}};
    int ref_uid[2][4] = {{-1, -1, -1, -1}, {-1, -1, -1, -1}};  // the reference pictures themselves
    int ref_poc[2][4] = {};
    bool ref_long[2][4] = {};
    int16_t mv[2][16][2] = {};
    uint8_t mvd[2][16][2] = {};  // |mvd| clipped to 255 (CABAC contexts)
    bool sub_direct[4] = {};
    // Deblocking parameters of its slice.
    int8_t dbf_idc = 0, alpha_off = 0, beta_off = 0;
};

struct Frame {
    int uid = 0;
    int width = 0, height = 0;  // in samples, whole macroblocks
    std::vector<uint8_t> y, u, v;
    std::vector<MbInfo> mbs;
    int frame_num = 0;
    int frame_num_wrap = 0;
    int poc = 0;
    int long_term_frame_idx = 0;
    bool short_ref = false;
    bool long_ref = false;
    bool needs_output = false;
    bool non_existing = false;
    bool mmco5 = false;
    int64_t tag = 0;
    int matrix = 2;
    bool full_range = false;
    int crop[4] = {0, 0, 0, 0};  // left, right, top, bottom
};

using FramePtr = std::shared_ptr<Frame>;

}  // namespace h264::detail
