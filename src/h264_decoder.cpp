// The H.264 decoder's top level (h264_decoder.h): NAL units, parameter
// sets, slice headers, picture order counts, reference picture management
// and output order, plus the RGBA conversion. Macroblocks -- their parsing,
// prediction, residual and the deblocking filter -- are h264_slice.cpp's;
// the CABAC engine is h264_cabac.cpp's.

#include "h264_decoder.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "h264_internal.h"
#include "h264_slice.h"

namespace h264 {
namespace detail {

bool BitReader::MoreRbspData() const {
    if (pos_ >= len_ * 8) return false;
    size_t last = len_;
    while (last > 0 && data_[last - 1] == 0) --last;  // cabac_zero_words / trailing zeros
    if (last == 0) return false;
    const unsigned b = data_[last - 1];
    int tz = 0;
    while (((b >> tz) & 1u) == 0) ++tz;
    const size_t stop_bit = (last - 1) * 8 + static_cast<size_t>(7 - tz);
    return pos_ < stop_bit;
}

void UnescapeRbsp(const uint8_t *data, size_t len, std::vector<uint8_t> *out) {
    out->clear();
    out->reserve(len);
    int zeros = 0;
    for (size_t i = 0; i < len; ++i) {
        const uint8_t b = data[i];
        if (zeros >= 2 && b == 3) {
            zeros = 0;
            continue;
        }
        out->push_back(b);
        zeros = b == 0 ? zeros + 1 : 0;
    }
}

bool Vlc::Build(const uint8_t *lens, const uint8_t *bits, int count) {
    max_len_ = 1;
    for (int i = 0; i < count; ++i) max_len_ = std::max<int>(max_len_, lens[i]);
    table_.assign(size_t{1} << max_len_, 0);
    for (int i = 0; i < count; ++i) {
        const int len = lens[i];
        if (len == 0) continue;
        const size_t first = static_cast<size_t>(bits[i]) << (max_len_ - len);
        const size_t n = size_t{1} << (max_len_ - len);
        for (size_t k = 0; k < n; ++k) {
            if (table_[first + k] != 0) return false;  // not prefix-free
            table_[first + k] = static_cast<uint16_t>(i | (len << 8));
        }
    }
    return true;
}

namespace {

// --- Parameter sets ------------------------------------------------------------------

constexpr int kMaxSps = 32;
constexpr int kMaxPps = 256;

// scaling_list() (7.3.2.1.1.1) into raster order. `use_default` reports
// useDefaultScalingMatrixFlag.
void ReadScalingList(BitReader &br, uint8_t *raster, int size, bool *use_default) {
    const uint8_t *zz = size == 16 ? kZigzag4x4 : kZigzag8x8;
    int last = 8, next = 8;
    *use_default = false;
    for (int j = 0; j < size; ++j) {
        if (next != 0) {
            const int delta = br.Se();
            next = (last + delta + 256) % 256;
            if (j == 0 && next == 0) {
                *use_default = true;
                return;
            }
        }
        const int v = next == 0 ? last : next;
        raster[zz[j]] = static_cast<uint8_t>(v);
        last = v;
    }
}

void DefaultList(uint8_t *raster, int size, bool intra) {
    const uint8_t *zz = size == 16 ? kZigzag4x4 : kZigzag8x8;
    const uint8_t *def = size == 16 ? (intra ? kDefault4x4Intra : kDefault4x4Inter) : (intra ? kDefault8x8Intra : kDefault8x8Inter);
    for (int j = 0; j < size; ++j) raster[zz[j]] = def[j];
}

// The scaling lists of an SPS or PPS with scaling_matrix_present set, with
// fall-back rule A (SPS: `base` null) or B (PPS: `base` the SPS's lists).
void ReadScalingLists(BitReader &br, int count, const ScalingLists *base, ScalingLists *out) {
    for (int i = 0; i < count; ++i) {
        const bool present = br.U1() != 0;
        const bool is4 = i < 6;
        uint8_t *dst = is4 ? out->m4[i] : out->m8[i - 6];
        const int size = is4 ? 16 : 64;
        const bool intra = is4 ? i < 3 : i == 6;
        bool use_default = false;
        if (present) {
            ReadScalingList(br, dst, size, &use_default);
            if (use_default) DefaultList(dst, size, intra);
            continue;
        }
        // Fall-back: the first list of a kind comes from the default
        // (rule A) or the SPS (rule B); the others repeat the one before.
        if (i == 0 || i == 3 || i >= 6) {
            if (base) {
                std::memcpy(dst, is4 ? base->m4[i] : base->m8[i - 6], static_cast<size_t>(size));
            } else {
                DefaultList(dst, size, intra);
            }
        } else {
            std::memcpy(dst, out->m4[i - 1], 16);
        }
    }
}

void FlatLists(ScalingLists *s) {
    std::memset(s->m4, 16, sizeof(s->m4));
    std::memset(s->m8, 16, sizeof(s->m8));
}

void SkipHrd(BitReader &br) {
    const uint32_t cpb_cnt = br.Ue() + 1;
    br.U(4);
    br.U(4);
    for (uint32_t i = 0; i < cpb_cnt && i < 32; ++i) {
        br.Ue();
        br.Ue();
        br.U1();
    }
    br.U(5);
    br.U(5);
    br.U(5);
    br.U(5);
}

void ParseVui(BitReader &br, Sps *sps) {
    if (br.U1()) {  // aspect_ratio_info_present_flag
        if (br.U(8) == 255) {
            br.U(16);
            br.U(16);
        }
    }
    if (br.U1()) br.U1();  // overscan
    if (br.U1()) {         // video_signal_type_present_flag
        br.U(3);
        sps->full_range = br.U1() != 0;
        if (br.U1()) {
            br.U(8);
            br.U(8);
            sps->matrix = static_cast<int>(br.U(8));
        }
    }
    if (br.U1()) {  // chroma_loc_info_present_flag
        br.Ue();
        br.Ue();
    }
    if (br.U1()) {  // timing_info_present_flag
        br.U(32);
        br.U(32);
        br.U1();
    }
    const bool nal_hrd = br.U1() != 0;
    if (nal_hrd) SkipHrd(br);
    const bool vcl_hrd = br.U1() != 0;
    if (vcl_hrd) SkipHrd(br);
    if (nal_hrd || vcl_hrd) br.U1();  // low_delay_hrd_flag
    br.U1();                          // pic_struct_present_flag
    if (br.U1()) {                    // bitstream_restriction_flag
        br.U1();
        br.Ue();
        br.Ue();
        br.Ue();
        br.Ue();
        sps->max_num_reorder = static_cast<int>(br.Ue());
        sps->max_dec_frame_buffering = static_cast<int>(br.Ue());
        if (br.overrun()) sps->max_num_reorder = sps->max_dec_frame_buffering = -1;
    }
}

bool ParseSps(BitReader &br, Sps *sps, int *id, std::string *error) {
    *sps = Sps{};
    sps->profile_idc = static_cast<int>(br.U(8));
    sps->constraint_flags = static_cast<int>(br.U(8));
    sps->level_idc = static_cast<int>(br.U(8));
    *id = static_cast<int>(br.Ue());
    if (*id >= kMaxSps) {
        *error = "bad seq_parameter_set_id";
        return false;
    }
    FlatLists(&sps->scaling);
    const int p = sps->profile_idc;
    if (p == 100 || p == 110 || p == 122 || p == 244 || p == 44 || p == 83 || p == 86 || p == 118 || p == 128 || p == 138 ||
        p == 139 || p == 134 || p == 135) {
        sps->chroma_format_idc = static_cast<int>(br.Ue());
        if (sps->chroma_format_idc == 3) br.U1();  // separate_colour_plane_flag
        sps->bit_depth_luma = static_cast<int>(br.Ue()) + 8;
        sps->bit_depth_chroma = static_cast<int>(br.Ue()) + 8;
        sps->transform_bypass = br.U1() != 0;
        sps->scaling_present = br.U1() != 0;
        if (sps->scaling_present) ReadScalingLists(br, sps->chroma_format_idc == 3 ? 12 : 8, nullptr, &sps->scaling);
    }
    sps->log2_max_frame_num = static_cast<int>(br.Ue()) + 4;
    sps->poc_type = static_cast<int>(br.Ue());
    if (sps->poc_type == 0) {
        sps->log2_max_poc_lsb = static_cast<int>(br.Ue()) + 4;
    } else if (sps->poc_type == 1) {
        sps->delta_pic_order_always_zero = br.U1() != 0;
        sps->offset_for_non_ref_pic = br.Se();
        sps->offset_for_top_to_bottom_field = br.Se();
        const uint32_t n = br.Ue();
        if (n > 255) {
            *error = "bad num_ref_frames_in_pic_order_cnt_cycle";
            return false;
        }
        for (uint32_t i = 0; i < n; ++i) sps->offset_for_ref_frame.push_back(br.Se());
    }
    sps->max_num_ref_frames = static_cast<int>(br.Ue());
    sps->gaps_allowed = br.U1() != 0;
    sps->width_mbs = static_cast<int>(br.Ue()) + 1;
    sps->height_mbs = static_cast<int>(br.Ue()) + 1;
    sps->frame_mbs_only = br.U1() != 0;
    if (!sps->frame_mbs_only) br.U1();  // mb_adaptive_frame_field_flag
    sps->direct_8x8_inference = br.U1() != 0;
    if (br.U1()) {  // frame_cropping_flag (4:2:0: units of 2 samples)
        sps->crop_left = static_cast<int>(br.Ue()) * 2;
        sps->crop_right = static_cast<int>(br.Ue()) * 2;
        sps->crop_top = static_cast<int>(br.Ue()) * 2;
        sps->crop_bottom = static_cast<int>(br.Ue()) * 2;
    }
    if (br.U1()) ParseVui(br, sps);
    if (br.overrun()) {
        *error = "truncated sequence parameter set";
        return false;
    }
    if (!sps->frame_mbs_only) {
        *error = "interlaced video (field coding) is not supported";
        return false;
    }
    if (sps->chroma_format_idc != 1) {
        *error = "only 4:2:0 video is supported";
        return false;
    }
    if (sps->bit_depth_luma != 8 || sps->bit_depth_chroma != 8) {
        *error = "only 8-bit video is supported";
        return false;
    }
    if (sps->width_mbs > 512 || sps->height_mbs > 512 || sps->width_mbs * sps->height_mbs > 139264 || sps->max_num_ref_frames > 16 ||
        sps->log2_max_frame_num > 16 || sps->log2_max_poc_lsb > 16 || sps->poc_type > 2) {
        *error = "unsupported sequence parameter set values";
        return false;
    }
    const int w = sps->width_mbs * 16, h = sps->height_mbs * 16;
    if (sps->crop_left + sps->crop_right >= w || sps->crop_top + sps->crop_bottom >= h)
        sps->crop_left = sps->crop_right = sps->crop_top = sps->crop_bottom = 0;
    sps->valid = true;
    return true;
}

// The parts of a PPS that need its SPS to be read (the scaling lists and
// their fall-backs) are kept raw here and resolved when a slice activates it.
struct PpsRaw {
    Pps pps;
    std::vector<uint8_t> rbsp;     // for re-reading the tail against the SPS
    size_t tail_bit = 0;           // bit position of transform_8x8_mode_flag, when present
    bool has_tail = false;
};

bool ParsePpsHead(BitReader &br, PpsRaw *raw, int *id, std::string *error) {
    Pps &pps = raw->pps;
    pps = Pps{};
    *id = static_cast<int>(br.Ue());
    pps.sps_id = static_cast<int>(br.Ue());
    if (*id >= kMaxPps || pps.sps_id >= kMaxSps) {
        *error = "bad picture parameter set id";
        return false;
    }
    pps.cabac = br.U1() != 0;
    pps.bottom_field_pic_order_present = br.U1() != 0;
    if (br.Ue() != 0) {
        *error = "flexible macroblock ordering (slice groups) is not supported";
        return false;
    }
    pps.num_ref_idx_default[0] = static_cast<int>(br.Ue()) + 1;
    pps.num_ref_idx_default[1] = static_cast<int>(br.Ue()) + 1;
    pps.weighted_pred = br.U1() != 0;
    pps.weighted_bipred_idc = static_cast<int>(br.U(2));
    pps.pic_init_qp = 26 + br.Se();
    br.Se();  // pic_init_qs_minus26
    pps.chroma_qp_offset[0] = br.Se();
    pps.deblocking_control_present = br.U1() != 0;
    pps.constrained_intra_pred = br.U1() != 0;
    pps.redundant_pic_cnt_present = br.U1() != 0;
    pps.chroma_qp_offset[1] = pps.chroma_qp_offset[0];
    raw->has_tail = br.MoreRbspData();
    raw->tail_bit = br.BitPos();
    if (br.overrun() || pps.num_ref_idx_default[0] > 32 || pps.num_ref_idx_default[1] > 32 || pps.pic_init_qp < 0 ||
        pps.pic_init_qp > 51 || std::abs(pps.chroma_qp_offset[0]) > 12) {
        *error = "bad picture parameter set";
        return false;
    }
    pps.valid = true;
    return true;
}

// Resolves a PPS against its SPS: the High-profile tail and the effective
// scaling lists.
Pps ResolvePps(const PpsRaw &raw, const Sps &sps) {
    Pps pps = raw.pps;
    pps.scaling = sps.scaling;  // no pic lists: the sequence's (Flat when it has none)
    if (raw.has_tail) {
        BitReader br(raw.rbsp.data(), raw.rbsp.size());
        br.SetBitPos(raw.tail_bit);
        pps.transform_8x8_mode = br.U1() != 0;
        pps.scaling_present = br.U1() != 0;
        if (pps.scaling_present) {
            // Rule B falls back to the SPS's lists when the SPS has them,
            // else rule A (the defaults).
            ScalingLists lists{};
            FlatLists(&lists);
            ReadScalingLists(br, 6 + (pps.transform_8x8_mode ? 2 : 0), sps.scaling_present ? &sps.scaling : nullptr, &lists);
            if (!pps.transform_8x8_mode) std::memcpy(lists.m8, sps.scaling.m8, sizeof(lists.m8));
            pps.scaling = lists;
        }
        pps.chroma_qp_offset[1] = br.Se();
        if (br.overrun()) pps.chroma_qp_offset[1] = pps.chroma_qp_offset[0];
    }
    return pps;
}

}  // namespace

// --- The decoder state ---------------------------------------------------------------

struct State {
    Sps sps[kMaxSps];
    PpsRaw pps[kMaxPps];
    int nal_length_size = 4;

    // The active sequence and the pictures it decodes into.
    int active_sps = -1;
    Sps cur_sps;
    std::vector<FramePtr> dpb;        // reference frames and frames waiting for output
    std::vector<FramePtr> pool;       // spare frames of the active size
    std::vector<FramePtr> ready;      // output order, waiting for GetPicture
    int next_uid = 1;
    int dpb_capacity = 16;     // frames (references and pictures awaiting output) the DPB holds
    int reorder_depth = 16;    // pictures that may wait for output ahead of a later-decoded one

    // Picture order count / frame_num state carried between pictures.
    int prev_poc_msb = 0, prev_poc_lsb = 0;
    int prev_frame_num_offset = 0;
    int prev_frame_num = 0;
    int prev_ref_frame_num = 0;
    int cur_poc_top = 0, cur_poc_bottom = 0;
    int max_long_term_frame_idx = -1;  // -1: "no long-term frame indices"
    bool waiting_for_keyframe = true;
    // Decoding resumed at a non-IDR I picture (after Reset): pictures that
    // would be shown before it predict from the GOP that was skipped, so
    // they are decoded but not output, until the next IDR picture.
    bool recovering = false;
    int recovery_poc = 0;

    // The picture being decoded.
    FramePtr cur;
    SliceHeader first_header;
    int slices_in_picture = 0;
    bool picture_error = false;
    std::vector<uint8_t> rbsp;
    SliceDecoder slice_decoder;

    bool DecodeNal(const uint8_t *nal, size_t len, int64_t tag, std::string *error);
    bool ParseSliceHeader(BitReader &br, int nal_type, int nal_ref_idc, SliceHeader *sh, std::string *error);
    bool StartPicture(const SliceHeader &sh, int64_t tag, std::string *error);
    void FinishPicture();
    void ConcealMissing();
    bool ActivateSps(int id, std::string *error);
    FramePtr NewFrame();
    void ComputePoc(const SliceHeader &sh, Frame *f);
    void FillFrameNumGap(const SliceHeader &sh);
    void MarkReferences(const SliceHeader &sh);
    void SlidingWindow();
    bool BuildRefLists(const SliceHeader &sh, std::vector<Frame *> lists[2], std::string *error);
    void Output(bool flush_all);
    void DropAll();
    int MaxFrameNum() const { return 1 << cur_sps.log2_max_frame_num; }
};

namespace {

// MaxDpbFrames from the level (Table A-1's MaxDpbMbs) for the frame size.
int MaxDpbFrames(const Sps &sps) {
    int max_dpb_mbs = 0;
    switch (sps.level_idc) {
        case 9: case 10: max_dpb_mbs = 396; break;
        case 11: max_dpb_mbs = (sps.constraint_flags & 0x10) && sps.profile_idc != 100 ? 396 : 900; break;
        case 12: case 13: case 20: max_dpb_mbs = 2376; break;
        case 21: max_dpb_mbs = 4752; break;
        case 22: case 30: max_dpb_mbs = 8100; break;
        case 31: max_dpb_mbs = 18000; break;
        case 32: max_dpb_mbs = 20480; break;
        case 40: case 41: max_dpb_mbs = 32768; break;
        case 42: max_dpb_mbs = 34816; break;
        case 50: max_dpb_mbs = 110400; break;
        case 51: case 52: max_dpb_mbs = 184320; break;
        default: max_dpb_mbs = 696320; break;
    }
    const int frames = max_dpb_mbs / (sps.width_mbs * sps.height_mbs);
    return std::clamp(frames, 1, 16);
}

}  // namespace

bool State::ActivateSps(int id, std::string *error) {
    const Sps &s = sps[id];
    if (!s.valid) {
        *error = "slice refers to a missing sequence parameter set";
        return false;
    }
    const bool resize = active_sps < 0 || s.width_mbs != cur_sps.width_mbs || s.height_mbs != cur_sps.height_mbs;
    active_sps = id;
    cur_sps = s;
    if (resize) {
        Output(true);
        dpb.clear();
        pool.clear();
    }
    // C.4.5.3's bumping: a picture is output when the DPB has no room for
    // the next one, or -- when the VUI bounds the reordering -- as soon as
    // more pictures wait than any later one can precede. (Even a Baseline
    // stream may order its pictures non-monotonically.)
    dpb_capacity = std::clamp(s.max_dec_frame_buffering >= 0 ? std::max(s.max_dec_frame_buffering, s.max_num_ref_frames) : MaxDpbFrames(s), 1, 16);
    reorder_depth = std::clamp(s.max_num_reorder >= 0 ? s.max_num_reorder : dpb_capacity, 0, 16);
    return true;
}

FramePtr State::NewFrame() {
    FramePtr f;
    // A pool frame nobody else holds any more (not in the DPB, not waiting
    // in `ready`) is reused as it is; its planes are overwritten.
    for (size_t i = 0; i < pool.size(); ++i) {
        if (pool[i].use_count() == 1) {
            f = pool[i];
            break;
        }
    }
    if (!f) {
        f = std::make_shared<Frame>();
        f->width = cur_sps.width_mbs * 16;
        f->height = cur_sps.height_mbs * 16;
        const size_t luma = static_cast<size_t>(f->width) * static_cast<size_t>(f->height);
        f->y.assign(luma, 0);
        f->u.assign(luma / 4, 128);
        f->v.assign(luma / 4, 128);
        f->mbs.resize(static_cast<size_t>(cur_sps.width_mbs * cur_sps.height_mbs));
        pool.push_back(f);
    }
    f->uid = next_uid++;
    f->short_ref = f->long_ref = f->needs_output = f->non_existing = f->mmco5 = false;
    f->long_term_frame_idx = 0;
    f->poc = 0;
    f->matrix = cur_sps.matrix;
    f->full_range = cur_sps.full_range;
    f->crop[0] = cur_sps.crop_left;
    f->crop[1] = cur_sps.crop_right;
    f->crop[2] = cur_sps.crop_top;
    f->crop[3] = cur_sps.crop_bottom;
    for (MbInfo &m : f->mbs) m.slice_num = -1;
    return f;
}

bool State::ParseSliceHeader(BitReader &br, int nal_type, int nal_ref_idc, SliceHeader *sh, std::string *error) {
    sh->nal_ref_idc = nal_ref_idc;
    sh->idr = nal_type == 5;
    sh->first_mb = static_cast<int>(br.Ue());
    const uint32_t raw_type = br.Ue();
    if (raw_type > 9) {
        *error = "bad slice_type";
        return false;
    }
    const int t = static_cast<int>(raw_type % 5);
    if (t == 3 || t == 4) {
        *error = "SP/SI slices are not supported";
        return false;
    }
    sh->type = t == 0 ? kSliceP : (t == 1 ? kSliceB : kSliceI);
    sh->pps_id = static_cast<int>(br.Ue());
    if (sh->pps_id >= kMaxPps || !pps[sh->pps_id].pps.valid) {
        *error = "slice refers to a missing picture parameter set";
        return false;
    }
    const Pps &p = pps[sh->pps_id].pps;
    const Sps &s = sps[p.sps_id];
    if (!s.valid) {
        *error = "slice refers to a missing sequence parameter set";
        return false;
    }
    sh->frame_num = static_cast<int>(br.U(s.log2_max_frame_num));
    if (sh->idr) sh->idr_pic_id = static_cast<int>(br.Ue());
    if (s.poc_type == 0) {
        sh->poc_lsb = static_cast<int>(br.U(s.log2_max_poc_lsb));
        if (p.bottom_field_pic_order_present) sh->delta_poc_bottom = br.Se();
    } else if (s.poc_type == 1 && !s.delta_pic_order_always_zero) {
        sh->delta_poc[0] = br.Se();
        if (p.bottom_field_pic_order_present) sh->delta_poc[1] = br.Se();
    }
    if (p.redundant_pic_cnt_present && br.Ue() != 0) {
        *error = "redundant";  // a redundant coded picture: skipped by the caller
        return false;
    }
    if (sh->type == kSliceB) sh->direct_spatial = br.U1() != 0;
    sh->num_ref_idx[0] = p.num_ref_idx_default[0];
    sh->num_ref_idx[1] = p.num_ref_idx_default[1];
    if (sh->type != kSliceI) {
        if (br.U1()) {
            sh->num_ref_idx[0] = static_cast<int>(br.Ue()) + 1;
            if (sh->type == kSliceB) sh->num_ref_idx[1] = static_cast<int>(br.Ue()) + 1;
        }
        if (sh->num_ref_idx[0] > 32 || sh->num_ref_idx[1] > 32) {
            *error = "bad num_ref_idx_active";
            return false;
        }
    }
    if (sh->type != kSliceB) sh->num_ref_idx[1] = 0;
    if (sh->type == kSliceI) sh->num_ref_idx[0] = 0;
    for (int list = 0; list < 2; ++list) {
        sh->mods[list].clear();
        if ((list == 0 && sh->type != kSliceI) || (list == 1 && sh->type == kSliceB)) {
            if (br.U1()) {
                for (int n = 0; n < 100; ++n) {
                    RefPicModification m;
                    m.idc = static_cast<int>(br.Ue());
                    if (m.idc == 3) break;
                    if (m.idc > 2 || br.overrun()) {
                        *error = "bad ref_pic_list_modification";
                        return false;
                    }
                    m.value = br.Ue();
                    sh->mods[list].push_back(m);
                }
            }
        }
    }
    sh->has_weights = (p.weighted_pred && sh->type == kSliceP) || (p.weighted_bipred_idc == 1 && sh->type == kSliceB);
    if (sh->has_weights) {
        sh->luma_log2_denom = static_cast<int>(br.Ue());
        sh->chroma_log2_denom = static_cast<int>(br.Ue());
        if (sh->luma_log2_denom > 7 || sh->chroma_log2_denom > 7) {
            *error = "bad pred_weight_table";
            return false;
        }
        for (int list = 0; list < (sh->type == kSliceB ? 2 : 1); ++list) {
            for (int i = 0; i < sh->num_ref_idx[list]; ++i) {
                sh->luma_weight[list][i] = static_cast<int16_t>(1 << sh->luma_log2_denom);
                sh->luma_offset[list][i] = 0;
                if (br.U1()) {
                    sh->luma_weight[list][i] = static_cast<int16_t>(br.Se());
                    sh->luma_offset[list][i] = static_cast<int16_t>(br.Se());
                }
                for (int c = 0; c < 2; ++c) {
                    sh->chroma_weight[list][i][c] = static_cast<int16_t>(1 << sh->chroma_log2_denom);
                    sh->chroma_offset[list][i][c] = 0;
                }
                if (br.U1()) {
                    for (int c = 0; c < 2; ++c) {
                        sh->chroma_weight[list][i][c] = static_cast<int16_t>(br.Se());
                        sh->chroma_offset[list][i][c] = static_cast<int16_t>(br.Se());
                    }
                }
            }
        }
    }
    sh->mmcos.clear();
    sh->adaptive_marking = false;
    if (nal_ref_idc != 0) {
        if (sh->idr) {
            sh->no_output_of_prior_pics = br.U1() != 0;
            sh->long_term_reference = br.U1() != 0;
        } else {
            sh->adaptive_marking = br.U1() != 0;
            if (sh->adaptive_marking) {
                for (int n = 0; n < 100; ++n) {
                    Mmco m;
                    m.op = static_cast<int>(br.Ue());
                    if (m.op == 0) break;
                    if (m.op > 6 || br.overrun()) {
                        *error = "bad memory_management_control_operation";
                        return false;
                    }
                    if (m.op == 1 || m.op == 3) m.a = br.Ue();
                    if (m.op == 2) m.a = br.Ue();
                    if (m.op == 3 || m.op == 6) m.b = br.Ue();
                    if (m.op == 4) m.a = br.Ue();
                    sh->mmcos.push_back(m);
                }
            }
        }
    }
    sh->cabac_init_idc = 0;
    if (p.cabac && sh->type != kSliceI) {
        sh->cabac_init_idc = static_cast<int>(br.Ue());
        if (sh->cabac_init_idc > 2) {
            *error = "bad cabac_init_idc";
            return false;
        }
    }
    sh->qp = p.pic_init_qp + br.Se();
    if (sh->qp < 0 || sh->qp > 51) {
        *error = "bad slice_qp_delta";
        return false;
    }
    sh->disable_deblocking = 0;
    sh->alpha_offset = sh->beta_offset = 0;
    if (p.deblocking_control_present) {
        sh->disable_deblocking = static_cast<int>(br.Ue());
        if (sh->disable_deblocking > 2) {
            *error = "bad disable_deblocking_filter_idc";
            return false;
        }
        if (sh->disable_deblocking != 1) {
            sh->alpha_offset = br.Se() * 2;
            sh->beta_offset = br.Se() * 2;
            if (std::abs(sh->alpha_offset) > 12 || std::abs(sh->beta_offset) > 12) {
                *error = "bad deblocking filter offsets";
                return false;
            }
        }
    }
    if (br.overrun()) {
        *error = "truncated slice header";
        return false;
    }
    return true;
}

// 8.2.1: TopFieldOrderCnt / BottomFieldOrderCnt; a frame's POC is the smaller.
void State::ComputePoc(const SliceHeader &sh, Frame *f) {
    const Sps &s = cur_sps;
    const int max_frame_num = MaxFrameNum();
    int top = 0, bottom = 0;
    if (s.poc_type == 0) {
        // (After a picture with mmco5, FinishPicture has already set
        // prev_poc_msb/lsb to 0 and its adjusted top POC.)
        if (sh.idr) prev_poc_msb = prev_poc_lsb = 0;
        const int max_lsb = 1 << s.log2_max_poc_lsb;
        int msb = prev_poc_msb;
        if (sh.poc_lsb < prev_poc_lsb && prev_poc_lsb - sh.poc_lsb >= max_lsb / 2) {
            msb = prev_poc_msb + max_lsb;
        } else if (sh.poc_lsb > prev_poc_lsb && sh.poc_lsb - prev_poc_lsb > max_lsb / 2) {
            msb = prev_poc_msb - max_lsb;
        }
        top = msb + sh.poc_lsb;
        bottom = top + sh.delta_poc_bottom;
        if (sh.nal_ref_idc != 0) {
            prev_poc_msb = msb;
            prev_poc_lsb = sh.poc_lsb;
        }
    } else {
        int frame_num_offset = 0;
        if (!sh.idr) {
            frame_num_offset = prev_frame_num > sh.frame_num ? prev_frame_num_offset + max_frame_num : prev_frame_num_offset;
        }
        if (s.poc_type == 1) {
            const int n = static_cast<int>(s.offset_for_ref_frame.size());
            int abs_frame_num = n != 0 ? frame_num_offset + sh.frame_num : 0;
            if (sh.nal_ref_idc == 0 && abs_frame_num > 0) --abs_frame_num;
            int expected = 0;
            if (abs_frame_num > 0) {
                int delta_per_cycle = 0;
                for (int v : s.offset_for_ref_frame) delta_per_cycle += v;
                const int cycle = (abs_frame_num - 1) / n;
                const int in_cycle = (abs_frame_num - 1) % n;
                expected = cycle * delta_per_cycle;
                for (int i = 0; i <= in_cycle; ++i) expected += s.offset_for_ref_frame[static_cast<size_t>(i)];
            }
            if (sh.nal_ref_idc == 0) expected += s.offset_for_non_ref_pic;
            top = expected + sh.delta_poc[0];
            bottom = top + s.offset_for_top_to_bottom_field + sh.delta_poc[1];
        } else {
            int temp = 0;
            if (!sh.idr) temp = sh.nal_ref_idc == 0 ? 2 * (frame_num_offset + sh.frame_num) - 1 : 2 * (frame_num_offset + sh.frame_num);
            top = bottom = temp;
        }
        prev_frame_num_offset = frame_num_offset;
    }
    f->poc = std::min(top, bottom);
    cur_poc_top = top;
    cur_poc_bottom = bottom;
}

// 8.2.5.2: frames for the frame_num values a stream skipped -- "non-existing"
// short-term references so the sliding window and later PicNums come out as
// the encoder meant. Their pictures copy the latest reference (they are
// never output, and only a damaged stream would predict from them).
void State::FillFrameNumGap(const SliceHeader &sh) {
    const int max_frame_num = MaxFrameNum();
    Frame *latest = nullptr;
    for (const FramePtr &f : dpb)
        if ((f->short_ref || f->long_ref) && (!latest || f->uid > latest->uid)) latest = f.get();
    int n = (prev_ref_frame_num + 1) % max_frame_num;
    // Only the last max_num_ref_frames of a long gap can still be
    // referenced; the frames before them would slide straight out.
    const int gap = (sh.frame_num - n + max_frame_num) % max_frame_num;
    const int keep = std::max(cur_sps.max_num_ref_frames, 1);
    if (gap > keep) {
        n = (n + gap - keep) % max_frame_num;
        prev_frame_num_offset += (prev_frame_num > n) ? max_frame_num : 0;
        prev_frame_num = n;
    }
    for (int guard = 0; n != sh.frame_num && guard < keep; ++guard) {
        FramePtr f = NewFrame();
        if (latest) {
            f->y = latest->y;
            f->u = latest->u;
            f->v = latest->v;
            f->mbs = latest->mbs;
        }
        f->frame_num = n;
        f->non_existing = true;
        if (prev_frame_num > n) prev_frame_num_offset += max_frame_num;
        f->poc = latest ? latest->poc : 0;
        for (const FramePtr &g : dpb) g->frame_num_wrap = g->frame_num > n ? g->frame_num - max_frame_num : g->frame_num;
        SlidingWindow();
        f->short_ref = true;
        f->frame_num_wrap = n;
        dpb.push_back(f);
        prev_ref_frame_num = prev_frame_num = n;
        n = (n + 1) % max_frame_num;
    }
}

void State::SlidingWindow() {
    const int max_refs = std::max(cur_sps.max_num_ref_frames, 1);
    for (;;) {
        int count = 0;
        Frame *oldest = nullptr;
        for (const FramePtr &f : dpb) {
            if (f.get() == cur.get()) continue;
            if (f->short_ref || f->long_ref) ++count;
            if (f->short_ref && (!oldest || f->frame_num_wrap < oldest->frame_num_wrap)) oldest = f.get();
        }
        if (count < max_refs || !oldest) return;
        oldest->short_ref = false;
    }
}

// 8.2.5: marks the current picture and applies its marking operations.
void State::MarkReferences(const SliceHeader &sh) {
    const int max_frame_num = MaxFrameNum();
    for (const FramePtr &f : dpb) f->frame_num_wrap = f->frame_num > cur->frame_num ? f->frame_num - max_frame_num : f->frame_num;
    if (sh.idr) {
        for (const FramePtr &f : dpb) f->short_ref = f->long_ref = false;
        if (sh.long_term_reference) {
            cur->long_ref = true;
            cur->long_term_frame_idx = 0;
            max_long_term_frame_idx = 0;
        } else {
            cur->short_ref = true;
            max_long_term_frame_idx = -1;
        }
        return;
    }
    if (sh.adaptive_marking) {
        const int curr_pic_num = cur->frame_num;
        auto short_by_pic_num = [&](int pic_num) -> Frame * {
            for (const FramePtr &f : dpb)
                if (f.get() != cur.get() && f->short_ref && f->frame_num_wrap == pic_num) return f.get();
            return nullptr;
        };
        for (const Mmco &m : sh.mmcos) {
            switch (m.op) {
                case 1:
                    if (Frame *f = short_by_pic_num(curr_pic_num - static_cast<int>(m.a + 1))) f->short_ref = false;
                    break;
                case 2:
                    for (const FramePtr &f : dpb)
                        if (f->long_ref && f->long_term_frame_idx == static_cast<int>(m.a)) f->long_ref = false;
                    break;
                case 3: {
                    Frame *target = short_by_pic_num(curr_pic_num - static_cast<int>(m.a + 1));
                    for (const FramePtr &f : dpb)
                        if (f.get() != target && f->long_ref && f->long_term_frame_idx == static_cast<int>(m.b)) f->long_ref = false;
                    if (target) {
                        target->short_ref = false;
                        target->long_ref = true;
                        target->long_term_frame_idx = static_cast<int>(m.b);
                    }
                    break;
                }
                case 4:
                    max_long_term_frame_idx = static_cast<int>(m.a) - 1;
                    for (const FramePtr &f : dpb)
                        if (f->long_ref && f->long_term_frame_idx > max_long_term_frame_idx) f->long_ref = false;
                    break;
                case 5:
                    for (const FramePtr &f : dpb)
                        if (f.get() != cur.get()) f->short_ref = f->long_ref = false;
                    max_long_term_frame_idx = -1;
                    cur->mmco5 = true;
                    break;
                case 6:
                    for (const FramePtr &f : dpb)
                        if (f.get() != cur.get() && f->long_ref && f->long_term_frame_idx == static_cast<int>(m.b)) f->long_ref = false;
                    cur->long_ref = true;
                    cur->long_term_frame_idx = static_cast<int>(m.b);
                    break;
                default: break;
            }
        }
    } else {
        SlidingWindow();
    }
    if (!cur->long_ref) cur->short_ref = true;
    // A stream that keeps more references than it declared loses its oldest.
    SlidingWindow();
}

bool State::BuildRefLists(const SliceHeader &sh, std::vector<Frame *> lists[2], std::string *error) {
    const int max_frame_num = MaxFrameNum();
    std::vector<Frame *> shorts, longs;
    for (const FramePtr &f : dpb) {
        if (f.get() == cur.get()) continue;
        if (f->short_ref) {
            f->frame_num_wrap = f->frame_num > cur->frame_num ? f->frame_num - max_frame_num : f->frame_num;
            shorts.push_back(f.get());
        } else if (f->long_ref) {
            longs.push_back(f.get());
        }
    }
    std::sort(longs.begin(), longs.end(), [](const Frame *a, const Frame *b) { return a->long_term_frame_idx < b->long_term_frame_idx; });
    lists[0].clear();
    lists[1].clear();
    if (sh.type == kSliceP) {
        std::sort(shorts.begin(), shorts.end(), [](const Frame *a, const Frame *b) { return a->frame_num_wrap > b->frame_num_wrap; });
        lists[0] = shorts;
        lists[0].insert(lists[0].end(), longs.begin(), longs.end());
    } else if (sh.type == kSliceB) {
        std::vector<Frame *> before, after;
        for (Frame *f : shorts) (f->poc < cur->poc ? before : after).push_back(f);
        std::sort(before.begin(), before.end(), [](const Frame *a, const Frame *b) { return a->poc > b->poc; });
        std::sort(after.begin(), after.end(), [](const Frame *a, const Frame *b) { return a->poc < b->poc; });
        lists[0] = before;
        lists[0].insert(lists[0].end(), after.begin(), after.end());
        lists[0].insert(lists[0].end(), longs.begin(), longs.end());
        lists[1] = after;
        lists[1].insert(lists[1].end(), before.begin(), before.end());
        lists[1].insert(lists[1].end(), longs.begin(), longs.end());
        if (lists[1].size() > 1 && lists[1] == lists[0]) std::swap(lists[1][0], lists[1][1]);
    }
    for (int x = 0; x < 2; ++x) {
        const int n = sh.num_ref_idx[x];
        std::vector<Frame *> &list = lists[x];
        list.resize(static_cast<size_t>(n), nullptr);
        // 8.2.4.3: modifications.
        int pred = cur->frame_num;
        int ref_idx = 0;
        for (const RefPicModification &m : sh.mods[x]) {
            if (ref_idx >= n) break;
            Frame *pic = nullptr;
            bool is_long = false;
            int pic_num = 0;
            if (m.idc == 0 || m.idc == 1) {
                const int abs_diff = static_cast<int>(m.value) + 1;
                int no_wrap = 0;
                if (m.idc == 0) {
                    no_wrap = pred - abs_diff;
                    if (no_wrap < 0) no_wrap += max_frame_num;
                } else {
                    no_wrap = pred + abs_diff;
                    if (no_wrap >= max_frame_num) no_wrap -= max_frame_num;
                }
                pred = no_wrap;
                pic_num = no_wrap > cur->frame_num ? no_wrap - max_frame_num : no_wrap;
                for (Frame *f : shorts)
                    if (f->frame_num_wrap == pic_num) pic = f;
            } else {
                is_long = true;
                pic_num = static_cast<int>(m.value);
                for (Frame *f : longs)
                    if (f->long_term_frame_idx == pic_num) pic = f;
            }
            if (!pic) {
                *error = "ref_pic_list_modification names a missing picture";
                continue;
            }
            list.insert(list.begin() + ref_idx, pic);
            ++ref_idx;
            int n_idx = ref_idx;
            for (int c = ref_idx; c < static_cast<int>(list.size()); ++c) {
                Frame *f = list[static_cast<size_t>(c)];
                const bool same = f && (is_long ? (f->long_ref && f->long_term_frame_idx == pic_num)
                                                : (f->short_ref && f->frame_num_wrap == pic_num));
                if (!same) list[static_cast<size_t>(n_idx++)] = f;
            }
            list.resize(static_cast<size_t>(n));
        }
        // Damaged or truncated lists still decode (badly) rather than crash.
        Frame *fallback = nullptr;
        for (Frame *f : list)
            if (f && !fallback) fallback = f;
        if (!fallback && !shorts.empty()) fallback = shorts.front();
        if (!fallback && !longs.empty()) fallback = longs.front();
        if (!fallback) fallback = cur.get();
        for (Frame *&f : list)
            if (!f) f = fallback;
    }
    return true;
}

// Moves pictures to `ready` in output order: all of them (flush_all), or
// while the DPB is over capacity or more than reorder_depth wait.
void State::Output(bool flush_all) {
    for (;;) {
        Frame *best = nullptr;
        int waiting = 0, held = 0;
        for (const FramePtr &f : dpb) {
            if (f->needs_output || f->short_ref || f->long_ref) ++held;
            if (!f->needs_output) continue;
            ++waiting;
            if (!best || f->poc < best->poc) best = f.get();
        }
        if (!best || (!flush_all && held <= dpb_capacity && waiting <= reorder_depth)) break;
        best->needs_output = false;
        for (const FramePtr &f : dpb)
            if (f.get() == best) ready.push_back(f);
    }
    dpb.erase(std::remove_if(dpb.begin(), dpb.end(), [](const FramePtr &f) { return !f->needs_output && !f->short_ref && !f->long_ref; }),
              dpb.end());
}

void State::DropAll() {
    cur.reset();
    dpb.clear();
    ready.clear();
    prev_poc_msb = prev_poc_lsb = 0;
    prev_frame_num_offset = prev_frame_num = prev_ref_frame_num = 0;
    max_long_term_frame_idx = -1;
    waiting_for_keyframe = true;
    recovering = false;
}

bool State::StartPicture(const SliceHeader &sh, int64_t tag, std::string *error) {
    if (!ActivateSps(pps[sh.pps_id].pps.sps_id, error)) return false;
    if (sh.idr) {
        // Everything before an IDR picture comes out before it (or is
        // dropped, when the stream says so).
        if (sh.no_output_of_prior_pics) {
            for (const FramePtr &f : dpb) f->needs_output = false;
        }
        for (const FramePtr &f : dpb) f->short_ref = f->long_ref = false;
        Output(true);
    } else if (sh.frame_num != prev_ref_frame_num && sh.frame_num != (prev_ref_frame_num + 1) % MaxFrameNum()) {
        FillFrameNumGap(sh);
    }
    cur = NewFrame();
    cur->frame_num = sh.frame_num;
    cur->tag = tag;
    ComputePoc(sh, cur.get());
    first_header = sh;
    slices_in_picture = 0;
    picture_error = false;
    return true;
}

// Macroblocks no slice reached (lost or damaged slice data) are filled
// from the latest reference picture, so a glitch shows the scene a moment
// ago rather than whatever an old pooled frame held.
void State::ConcealMissing() {
    Frame *src = nullptr;
    for (const FramePtr &f : dpb)
        if (f.get() != cur.get() && (f->short_ref || f->long_ref) && (!src || f->uid > src->uid)) src = f.get();
    const int wmbs = cur->width / 16, cw = cur->width / 2;
    for (size_t i = 0; i < cur->mbs.size(); ++i) {
        MbInfo &m = cur->mbs[i];
        if (m.slice_num >= 0) continue;
        m = MbInfo{};
        m.intra = true;  // (no motion for later pictures' direct prediction to borrow)
        const int mbx = static_cast<int>(i) % wmbs, mby = static_cast<int>(i) / wmbs;
        for (int y = 0; y < 16; ++y) {
            uint8_t *d = &cur->y[static_cast<size_t>((mby * 16 + y) * cur->width + mbx * 16)];
            if (src) std::memcpy(d, &src->y[static_cast<size_t>((mby * 16 + y) * cur->width + mbx * 16)], 16);
            else std::memset(d, 16, 16);
        }
        for (int y = 0; y < 8; ++y) {
            const size_t o = static_cast<size_t>((mby * 8 + y) * cw + mbx * 8);
            if (src) {
                std::memcpy(&cur->u[o], &src->u[o], 8);
                std::memcpy(&cur->v[o], &src->v[o], 8);
            } else {
                std::memset(&cur->u[o], 128, 8);
                std::memset(&cur->v[o], 128, 8);
            }
        }
    }
}

void State::FinishPicture() {
    if (!cur) return;
    ConcealMissing();
    slice_decoder.Deblock(*cur, ResolvePps(pps[first_header.pps_id], cur_sps));
    const SliceHeader &sh = first_header;
    if (sh.nal_ref_idc != 0) MarkReferences(sh);
    if (cur->mmco5) {
        // 8.2.1: the picture after an mmco5 counts from zero again, and
        // everything before it is output first.
        const int temp = std::min(cur_poc_top, cur_poc_bottom);
        cur->poc = 0;
        cur->frame_num = 0;
        prev_poc_msb = 0;
        prev_poc_lsb = cur_poc_top - temp;
        prev_frame_num_offset = 0;
        prev_frame_num = prev_ref_frame_num = 0;
        Output(true);
    } else {
        prev_frame_num = sh.frame_num;
        if (sh.nal_ref_idc != 0) prev_ref_frame_num = sh.frame_num;
    }
    cur->needs_output = !(recovering && cur->poc < recovery_poc);
    if (cur->mmco5) recovering = false;
    dpb.push_back(cur);
    cur.reset();
    Output(false);
}

bool State::DecodeNal(const uint8_t *nal, size_t len, int64_t tag, std::string *error) {
    if (len < 1) return true;
    const int nal_type = nal[0] & 0x1f;
    const int nal_ref_idc = (nal[0] >> 5) & 3;
    switch (nal_type) {
        case 7: {
            UnescapeRbsp(nal + 1, len - 1, &rbsp);
            BitReader br(rbsp.data(), rbsp.size());
            Sps s;
            int id = 0;
            if (!ParseSps(br, &s, &id, error)) return false;
            sps[id] = s;
            return true;
        }
        case 8: {
            PpsRaw raw;
            UnescapeRbsp(nal + 1, len - 1, &raw.rbsp);
            BitReader br(raw.rbsp.data(), raw.rbsp.size());
            int id = 0;
            if (!ParsePpsHead(br, &raw, &id, error)) return false;
            pps[id] = std::move(raw);
            return true;
        }
        case 1:
        case 5: {
            UnescapeRbsp(nal + 1, len - 1, &rbsp);
            BitReader br(rbsp.data(), rbsp.size());
            SliceHeader sh;
            std::string err;
            if (!ParseSliceHeader(br, nal_type, nal_ref_idc, &sh, &err)) {
                if (err == "redundant") return true;
                *error = err;
                return false;
            }
            if (cur && sh.first_mb == 0) FinishPicture();
            if (!cur) {
                bool recovery_point = false;
                if (waiting_for_keyframe) {
                    if (!sh.idr && sh.type != kSliceI) return true;  // until a picture that needs no references
                    waiting_for_keyframe = false;
                    recovery_point = !sh.idr;
                }
                if (!StartPicture(sh, tag, error)) return false;
                if (sh.idr) recovering = false;
                if (recovery_point) {
                    recovering = true;
                    recovery_poc = cur->poc;
                }
            }
            const Pps p = ResolvePps(pps[sh.pps_id], cur_sps);
            SliceContext ctx;
            ctx.sps = &cur_sps;
            ctx.pps = &p;
            ctx.sh = &sh;
            ctx.cur = cur.get();
            ctx.slice_num = slices_in_picture++;
            if (!BuildRefLists(sh, ctx.refs, error)) return false;
            if (!slice_decoder.DecodeSlice(ctx, br, error)) {
                picture_error = true;
                return false;
            }
            return true;
        }
        case 2:
        case 3:
        case 4: *error = "data partitioning is not supported"; return false;
        default: return true;  // SEI, delimiters, filler, end of sequence/stream
    }
}

}  // namespace detail

struct Decoder::Impl : detail::State {};

Decoder::Decoder() : impl_(std::make_unique<Impl>()) {}
Decoder::~Decoder() = default;

bool Decoder::Configure(const uint8_t *avcc, size_t len, std::string *error) {
    if (len < 7 || avcc[0] != 1) {
        *error = "not an AVCDecoderConfigurationRecord";
        return false;
    }
    impl_->nal_length_size = (avcc[4] & 3) + 1;
    size_t pos = 5;
    for (int kind = 0; kind < 2; ++kind) {
        if (pos >= len) break;
        const int count = kind == 0 ? (avcc[pos] & 0x1f) : avcc[pos];
        ++pos;
        for (int i = 0; i < count; ++i) {
            if (pos + 2 > len) {
                *error = "truncated avcC";
                return false;
            }
            const size_t n = (size_t{avcc[pos]} << 8) | avcc[pos + 1];
            pos += 2;
            if (pos + n > len) {
                *error = "truncated avcC";
                return false;
            }
            if (!impl_->DecodeNal(avcc + pos, n, 0, error)) return false;
            pos += n;
        }
    }
    return true;
}

bool Decoder::Decode(const uint8_t *data, size_t len, int64_t tag, std::string *error) {
    const size_t ls = static_cast<size_t>(impl_->nal_length_size);
    size_t pos = 0;
    bool ok = true;
    std::string first_error;
    while (pos + ls <= len) {
        size_t n = 0;
        for (size_t i = 0; i < ls; ++i) n = (n << 8) | data[pos + i];
        pos += ls;
        if (n > len - pos) {
            first_error = "truncated NAL unit";
            ok = false;
            break;
        }
        std::string err;
        if (!impl_->DecodeNal(data + pos, n, tag, &err)) {
            if (ok) first_error = err;
            ok = false;
        }
        pos += n;
    }
    impl_->FinishPicture();
    if (!ok && error) *error = first_error;
    return ok;
}

bool Decoder::DecodeAnnexB(const uint8_t *data, size_t len, int64_t tag, std::string *error) {
    bool ok = true;
    std::string first_error;
    size_t i = 0;
    auto find_start = [&](size_t from) -> size_t {
        for (size_t k = from; k + 3 <= len; ++k)
            if (data[k] == 0 && data[k + 1] == 0 && data[k + 2] == 1) return k;
        return len;
    };
    i = find_start(0);
    while (i < len) {
        const size_t begin = i + 3;
        size_t next = find_start(begin);
        size_t end = next;
        while (end > begin && data[end - 1] == 0) --end;  // trailing_zero_8bits / the next start code's leading zero
        std::string err;
        if (!impl_->DecodeNal(data + begin, end - begin, tag, &err)) {
            if (ok) first_error = err;
            ok = false;
        }
        i = next;
    }
    impl_->FinishPicture();
    if (!ok && error) *error = first_error;
    return ok;
}

bool Decoder::GetPicture(Picture *out) {
    if (impl_->ready.empty()) return false;
    detail::FramePtr f = impl_->ready.front();
    impl_->ready.erase(impl_->ready.begin());
    const int w = f->width - f->crop[0] - f->crop[1];
    const int h = f->height - f->crop[2] - f->crop[3];
    out->width = w;
    out->height = h;
    out->tag = f->tag;
    out->matrix = f->matrix;
    out->full_range = f->full_range;
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    out->y.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
    out->u.resize(static_cast<size_t>(cw) * static_cast<size_t>(ch));
    out->v.resize(out->u.size());
    for (int r = 0; r < h; ++r)
        std::memcpy(&out->y[static_cast<size_t>(r * w)], &f->y[static_cast<size_t>((r + f->crop[2]) * f->width + f->crop[0])],
                    static_cast<size_t>(w));
    const int fcw = f->width / 2;
    for (int r = 0; r < ch; ++r) {
        const size_t src = static_cast<size_t>((r + f->crop[2] / 2) * fcw + f->crop[0] / 2);
        std::memcpy(&out->u[static_cast<size_t>(r * cw)], &f->u[src], static_cast<size_t>(cw));
        std::memcpy(&out->v[static_cast<size_t>(r * cw)], &f->v[src], static_cast<size_t>(cw));
    }
    return true;
}

void Decoder::Flush() {
    impl_->FinishPicture();
    impl_->Output(true);
}

void Decoder::Reset() { impl_->DropAll(); }

void ToRgba(const Picture &pic, uint8_t *rgba) {
    // Fixed-point (x 2^16) Y'CbCr -> R'G'B' for the picture's matrix and
    // range. BT.709 when the stream says so, else BT.601 -- what an
    // unspecified SD stream (most of YouTube's itag 18) is encoded with.
    const bool bt709 = pic.matrix == 1;
    const double kr = bt709 ? 0.2126 : 0.299, kb = bt709 ? 0.0722 : 0.114;
    const double kg = 1.0 - kr - kb;
    const double ys = pic.full_range ? 1.0 : 255.0 / 219.0;
    const double cs = pic.full_range ? 1.0 : 255.0 / 224.0;
    const int y_mul = static_cast<int>(ys * 65536.0 + 0.5);
    const int y_off = pic.full_range ? 0 : 16;
    const int cr_r = static_cast<int>(2.0 * (1.0 - kr) * cs * 65536.0 + 0.5);
    const int cb_b = static_cast<int>(2.0 * (1.0 - kb) * cs * 65536.0 + 0.5);
    const int cb_g = static_cast<int>(2.0 * (1.0 - kb) * kb / kg * cs * 65536.0 + 0.5);
    const int cr_g = static_cast<int>(2.0 * (1.0 - kr) * kr / kg * cs * 65536.0 + 0.5);
    const int w = pic.width, h = pic.height, cw = (w + 1) / 2;
    auto clamp = [](int v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); };
    for (int r = 0; r < h; ++r) {
        const uint8_t *yr = &pic.y[static_cast<size_t>(r * w)];
        const uint8_t *ur = &pic.u[static_cast<size_t>((r / 2) * cw)];
        const uint8_t *vr = &pic.v[static_cast<size_t>((r / 2) * cw)];
        uint8_t *o = rgba + static_cast<size_t>(r) * static_cast<size_t>(w) * 4u;
        for (int x = 0; x < w; ++x) {
            const int yy = (yr[x] - y_off) * y_mul + 32768;
            const int cb = ur[x >> 1] - 128, cr = vr[x >> 1] - 128;
            o[0] = clamp((yy + cr_r * cr) >> 16);
            o[1] = clamp((yy - cb_g * cb - cr_g * cr) >> 16);
            o[2] = clamp((yy + cb_b * cb) >> 16);
            o[3] = 255;
            o += 4;
        }
    }
}

}  // namespace h264
