#pragma once

// In-house AAC-LC decoder (ISO/IEC 14496-3 GA, low-complexity profile) for
// the YouTube player's audio track -- the AAC half of what used to be an
// ffmpeg subprocess. Feed it the AudioSpecificConfig from an MP4 `esds`
// box once, then one raw access unit (one MP4 sample, no ADTS header) at a
// time; each yields 1024 interleaved int16 frames.
//
// Covered: SCE/CPE/LFE/DSE/PCE/FIL/END elements, long and short windows
// (all four window sequences, both window shapes), section data,
// scalefactors (including intensity and noise positions), spectral
// Huffman codebooks 1-11 with escapes, pulse data, TNS, M/S and intensity
// stereo, perceptual noise substitution, and the IMDCT/overlap-add
// filterbank. An SBR/PS extension (HE-AAC) is tolerated by decoding the LC
// core only, at the core sample rate. Not covered (Configure or Decode
// refuses them): other object types (Main, SSR, LTP, ER), 960-sample
// frames, coupling channel elements, and more than two output channels.
//
// No dependencies beyond the standard library, so it is unit-testable
// without a device (src/aac_decoder_test.cpp).

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace aac {

class BitReader;

class Decoder {
public:
    Decoder();

    /**
     * @brief Reads an AudioSpecificConfig (the DecoderSpecificInfo inside an MP4 esds box) and prepares to decode.
     * @param asc The config bytes.
     * @param len Their length.
     * @param error Set to why, when the stream is not AAC-LC mono/stereo with 1024-sample frames.
     * @return True when the decoder is ready for Decode.
     */
    bool Configure(const uint8_t *asc, size_t len, std::string *error);
    /** @brief The output sample rate (the LC core's, for an HE-AAC stream); 0 before Configure. */
    int SampleRate() const { return sample_rate_; }
    /** @brief The number of output channels (1 or 2); 0 before Configure. */
    int Channels() const { return channels_; }

    /**
     * @brief Decodes one raw access unit into 1024 interleaved int16 frames appended to `out`.
     * @param data The access unit (one MP4 sample, no ADTS header).
     * @param len Its length in bytes.
     * @param out Receives 1024 * Channels() samples on success; untouched on failure.
     * @param error Set to why on a bitstream error; the caller may skip the unit and carry on.
     * @return True on success.
     */
    bool Decode(const uint8_t *data, size_t len, std::vector<int16_t> *out, std::string *error);

    /** @brief Drops the overlap-add state, for a discontinuity such as a seek. */
    void Reset();

    // One channel's decoded side information and spectrum for the frame
    // being decoded (public only so the implementation's free functions
    // can name it).
    struct IcsInfo {
        int window_sequence = 0;  // 0 long, 1 long start, 2 eight short, 3 long stop
        int window_shape = 0;     // 0 sine, 1 KBD
        int max_sfb = 0;
        int num_windows = 1;
        int num_window_groups = 1;
        std::array<int, 8> window_group_length{};
        int num_swb = 0;
        const uint16_t *swb_offset = nullptr;  // num_swb + 1 entries
    };
    struct Channel {
        IcsInfo ics;
        int global_gain = 0;
        std::array<uint8_t, 8 * 64> band_type{};  // [group * 64 + sfb]
        std::array<int, 8 * 64> sf{};             // scalefactor / intensity position / noise energy
        bool tns_present = false;
        struct TnsFilter {
            int length = 0, order = 0;
            bool direction = false;
            std::array<float, 32> lpc{};  // a[1..order]
        };
        std::array<int, 8> tns_n_filt{};
        std::array<std::array<TnsFilter, 4>, 8> tns{};
        std::array<float, 1024> spec{};  // window-major: [window * 128 + k] for short windows
        // Filterbank state across frames.
        std::array<float, 1024> overlap{};
        int prev_window_shape = 0;
    };

private:
    bool DecodeIcs(BitReader &br, Channel &ch, bool common_window, std::string *error);
    void Filterbank(Channel &ch, float *time_out);

    int sample_rate_ = 0;
    int sample_rate_index_ = -1;
    int channels_ = 0;
    std::array<Channel, 2> ch_{};
    uint32_t noise_seed_ = 0x1f2e3d4cu;
    std::vector<float> scratch_;
};

/**
 * @brief Self-check of the standard tables built into the decoder: every Huffman codebook (spectral 1-11 and the
 * scalefactor book) is prefix-free and complete, and every scalefactor band table ends at its window length.
 * @param error Set to the first problem found.
 * @return True when all tables pass. For the unit test.
 */
bool CheckTables(std::string *error);

}  // namespace aac
