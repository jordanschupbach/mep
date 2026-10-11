#pragma once

// The macroblock layer of the H.264 decoder (h264_decoder.h): one slice's
// macroblocks parsed (CAVLC or CABAC), predicted (intra or motion
// compensated) and reconstructed into the current picture, and the
// picture's deblocking filter once all its slices are in.

#include <cstdint>
#include <string>
#include <vector>

#include "h264_internal.h"

namespace h264::detail {

class Cabac;

// Everything one slice's macroblocks need to know about their slice.
struct SliceContext {
    const Sps *sps = nullptr;
    const Pps *pps = nullptr;
    const SliceHeader *sh = nullptr;
    Frame *cur = nullptr;
    int slice_num = 0;
    // RefPicList0/1, num_ref_idx entries each (never null).
    std::vector<Frame *> refs[2];
};

class SliceDecoder {
public:
    SliceDecoder();
    ~SliceDecoder();
    SliceDecoder(const SliceDecoder &) = delete;
    SliceDecoder &operator=(const SliceDecoder &) = delete;

    /**
     * @brief Decodes the macroblocks of one slice into ctx.cur.
     * @param ctx The slice and the picture it belongs to.
     * @param br The slice's RBSP, positioned just after its header.
     * @param error Set when the slice data is damaged; macroblocks decoded before that stay.
     * @return False on damaged slice data.
     */
    bool DecodeSlice(const SliceContext &ctx, BitReader &br, std::string *error);
    /**
     * @brief Runs the deblocking filter (8.7) over a fully decoded picture.
     * @param f The picture.
     * @param pps The picture's PPS (chroma QP offsets).
     */
    void Deblock(Frame &f, const Pps &pps);

private:
    struct Impl;
    Impl *impl_;
};

}  // namespace h264::detail
