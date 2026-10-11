#pragma once

// An in-house H.264 / AVC (ITU-T H.264) video decoder for the YouTube
// player: MP4-style access units in, 8-bit 4:2:0 pictures out, in output
// order. No media library behind it -- parsing, entropy decoding,
// prediction, the inverse transforms, the deblocking filter and reference
// picture management are all here: h264_decoder.cpp (NAL units, parameter
// sets, reference pictures, output order), h264_slice.cpp and
// h264_cabac_mb.cpp (CAVLC and CABAC macroblock parsing), h264_mb.cpp
// (prediction and reconstruction), h264_deblock.cpp, h264_cabac.cpp (the
// arithmetic decoder) and h264_tables.cpp.
//
// Supported: progressive (frame_mbs_only) 8-bit 4:2:0 streams of the
// Constrained Baseline, Baseline, Main and High profiles -- CAVLC and
// CABAC, I/P/B slices, I_PCM, multiple slices and reference frames,
// reference list modification and memory management operations, long-term
// references, all three picture order count types, spatial and temporal
// direct prediction, explicit and implicit weighted prediction, the 8x8
// transform and scaling matrices. Output is bit-exact with the reference
// decoders (YouTube's own streams and the JVT conformance suite's progressive
// bitstreams were checked frame by frame). Rejected with an error:
// interlaced coding (field pictures, MBAFF), slice groups (FMO), data
// partitioning, SP/SI slices, bit depths above 8 and chroma formats other
// than 4:2:0. A picture that fails to decode partway is still output, its
// missing macroblocks filled from the previous reference picture, so a
// damaged slice costs a glitch rather than a stall.
//
// One Decoder decodes one stream on one thread at a time.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace h264 {

// One decoded picture, cropped to the display size: tightly packed 8-bit
// planes, `y` width * height, `u` and `v` (width / 2) * (height / 2)
// (rounded up for odd sizes).
struct Picture {
    int width = 0, height = 0;
    std::vector<uint8_t> y, u, v;
    int64_t tag = 0;          // the `tag` given to the Decode call that produced it
    int matrix = 2;           // VUI matrix_coefficients: 1 = BT.709, 5/6 = BT.601, 2 = unspecified
    bool full_range = false;  // VUI video_full_range_flag
};

class Decoder {
public:
    Decoder();
    ~Decoder();
    Decoder(const Decoder &) = delete;
    Decoder &operator=(const Decoder &) = delete;

    /**
     * @brief Takes the stream's parameter sets from an MP4 avcC box payload (AVCDecoderConfigurationRecord).
     * @param avcc The box payload, after its 8-byte box header.
     * @param len Its length.
     * @param error Set to why it was refused.
     * @return False when the record is malformed or its SPS/PPS use something unsupported.
     */
    bool Configure(const uint8_t *avcc, size_t len, std::string *error);
    /**
     * @brief Decodes one access unit in MP4 form (NAL units each preceded by a big-endian length of the size Configure read).
     * @param data The sample's bytes.
     * @param len Their length.
     * @param tag Carried to the Picture this access unit produces (the sample's presentation time, say).
     * @param error Set to why decoding failed; the decoder stays usable.
     * @return False on a decoding error. Pictures already decoded remain available from GetPicture.
     */
    bool Decode(const uint8_t *data, size_t len, int64_t tag, std::string *error);
    /**
     * @brief Decodes an Annex B byte stream (start-code delimited NAL units), one access unit per call.
     * @param data The bytes, which may include SPS/PPS NAL units.
     * @param len Their length.
     * @param tag As for Decode.
     * @param error Set to why decoding failed.
     * @return False on a decoding error.
     */
    bool DecodeAnnexB(const uint8_t *data, size_t len, int64_t tag, std::string *error);
    /**
     * @brief Pops the next decoded picture in output (picture order count) order.
     * @param out Receives the picture.
     * @return False when none is ready yet.
     */
    bool GetPicture(Picture *out);
    /** @brief End of stream: makes every picture still held for reordering available to GetPicture. */
    void Flush();
    /** @brief After a seek: drops every reference and pending picture; decoding resumes at the next IDR picture or recovery point. */
    void Reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief Converts a picture to RGBA (alpha 255), using its matrix (BT.709, else BT.601) and range.
 * @param pic The picture.
 * @param rgba Destination of pic.width * pic.height * 4 bytes.
 */
void ToRgba(const Picture &pic, uint8_t *rgba);

}  // namespace h264
