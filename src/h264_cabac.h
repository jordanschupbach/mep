#pragma once

// CABAC (9.3) for the H.264 decoder: the arithmetic decoding engine and
// its context variables. The macroblock-level binarizations that drive it
// are h264_cabac_mb.cpp's.

#include <cstddef>
#include <cstdint>

namespace h264::detail {

class Cabac {
public:
    /**
     * @brief Initialises the engine on slice data starting at `data` (byte aligned) and the context variables for the slice.
     * @param data First byte of the slice data after cabac_alignment_one_bit.
     * @param len Bytes available.
     * @param slice_type 0 P, 1 B, 2 I (the decoder's SliceType).
     * @param cabac_init_idc The slice header's cabac_init_idc.
     * @param qp SliceQPY.
     */
    void Start(const uint8_t *data, size_t len, int slice_type, int cabac_init_idc, int qp);
    /** @brief Re-initialises only the arithmetic decoding engine (after I_PCM samples), keeping the contexts. */
    void Restart(const uint8_t *data, size_t len);
    /** @brief Bits consumed from `data` so far. */
    size_t BitPosition() const;
    /** @brief DecodeDecision (9.3.3.2.1) with context index `ctx`. */
    int Decision(int ctx);
    /** @brief DecodeBypass (9.3.3.2.3). */
    int Bypass();
    /** @brief DecodeTerminate (9.3.3.2.2). */
    int Terminate();
    /** @brief Whether the engine has read past the end of the slice data. */
    bool Overrun() const { return pos_ > len_ + 2; }

private:
    void Renorm();
    int ReadBit() {
        if (bits_left_ == 0) {
            cur_byte_ = pos_ < len_ ? data_[pos_] : 0;
            ++pos_;
            bits_left_ = 8;
        }
        --bits_left_;
        return (cur_byte_ >> bits_left_) & 1;
    }

    const uint8_t *data_ = nullptr;
    size_t len_ = 0, pos_ = 0;
    int cur_byte_ = 0, bits_left_ = 0;
    uint32_t range_ = 0, offset_ = 0;
    uint8_t state_[1024] = {};  // pStateIdx
    uint8_t mps_[1024] = {};    // valMPS
};

}  // namespace h264::detail
