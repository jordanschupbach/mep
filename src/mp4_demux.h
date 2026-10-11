#pragma once

// An in-house MP4 (ISO base media file) demuxer for the YouTube player
// (mp4_player.h): it reads a file *as a stream* -- front to back, through
// a ByteSource that may be an HTTP download -- and hands out its samples
// one at a time in file order, each with its presentation time. Both
// layouts YouTube serves are covered:
//   - progressive (itag 18): one moov with every track's whole sample
//     table (stts/ctts/stsc/stsz/stco/stss), then one interleaved mdat;
//   - fragmented (the adaptive streams): a moov holding only the tracks'
//     setup (and mvex defaults), a sidx indexing the fragments, then
//     moof+mdat pairs whose trun boxes list the samples.
// A moov placed after the mdat is fetched by seeking the source past it
// and back. Seeking (Reader::Seek) moves the source to the byte where the
// data for a time starts: the video sync sample at or before it (and the
// audio sample at it) for a progressive file, the sidx fragment holding it
// for a fragmented one. Only what the player needs is parsed: the first
// video (avc1/avc3) and audio (mp4a) sample descriptions, edit lists'
// leading offset, and nothing encrypted.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mp4 {

enum class TrackKind { Video, Audio, Other };

struct Sample {
    uint64_t offset = 0;  // absolute file offset
    uint32_t size = 0;
    int64_t dts = 0;  // decode time, track timescale
    int32_t cto = 0;  // composition offset: pts = dts + cto
    uint32_t duration = 0;
    bool sync = false;
};

struct Track {
    uint32_t id = 0;
    TrackKind kind = TrackKind::Other;
    std::string codec;  // the sample entry's four-cc: "avc1", "mp4a", ...
    uint32_t timescale = 0;
    uint64_t duration = 0;  // media duration, track timescale
    // Edit list: the media time shown at presentation time 0 (an
    // encoder's priming, typically), and an empty leading edit's delay.
    int64_t media_time = 0;
    double delay_sec = 0.0;
    int width = 0, height = 0;          // video
    int channels = 0, sample_rate = 0;  // audio
    std::vector<uint8_t> config;        // avcC payload (video) / AudioSpecificConfig (audio)
    std::vector<Sample> samples;        // progressive files: the whole table
    // Fragmented files: the mvex/trex defaults a trun falls back on.
    uint32_t default_duration = 0, default_size = 0, default_flags = 0;

    /** @brief A time in this track's timescale as presentation seconds (edit list applied). */
    double Seconds(int64_t t) const;
};

// One sidx reference: a fragment's place in the file and in time.
struct Segment {
    uint64_t offset = 0;
    uint64_t size = 0;
    double start_sec = 0.0;
    double duration_sec = 0.0;
};

struct Movie {
    std::vector<Track> tracks;
    bool fragmented = false;
    std::vector<Segment> segments;  // fragmented files with a sidx
    double duration_sec = 0.0;

    /** @brief Index of the first video track (avc1/avc3), or -1. */
    int VideoTrack() const;
    /** @brief Index of the first audio track (mp4a), or -1. */
    int AudioTrack() const;
};

/**
 * @brief Parses a moov box's payload (the bytes after its 8- or 16-byte header).
 * @param data The payload.
 * @param len Its length.
 * @param out Filled with the tracks, their setup and (progressive files) sample tables.
 * @param error Why it failed.
 * @return False when the box is malformed or holds no track this demuxer reads.
 */
bool ParseMoov(const uint8_t *data, size_t len, Movie *out, std::string *error);

/**
 * @brief Parses a sidx box's payload into `movie->segments`.
 * @param data The payload.
 * @param len Its length.
 * @param box_end The file offset just past the whole sidx box (its references count from there).
 * @param movie The movie the index belongs to (its tracks give the timescale's meaning).
 * @param error Why it failed.
 */
bool ParseSidx(const uint8_t *data, size_t len, uint64_t box_end, Movie *movie, std::string *error);

// One sample of a fragment: the index of its track in Movie::tracks.
struct FragmentSample {
    int track = -1;
    Sample sample;
};

/**
 * @brief Parses a moof box's payload into its samples, with absolute offsets.
 * @param data The payload.
 * @param len Its length.
 * @param moof_offset The file offset of the moof box's first byte.
 * @param movie The movie (its tracks' ids and trex defaults).
 * @param out The fragment's samples, appended in file order.
 * @param error Why it failed.
 */
bool ParseMoof(const uint8_t *data, size_t len, uint64_t moof_offset, const Movie &movie, std::vector<FragmentSample> *out,
               std::string *error);

// Where the bytes come from: a file, or an HTTP download that SeekTo
// restarts at an offset.
class ByteSource {
public:
    virtual ~ByteSource() = default;
    /** @brief Reads exactly `n` bytes; false at the end, on an error, or when cancelled. */
    virtual bool Read(uint8_t *dst, size_t n) = 0;
    /** @brief Moves to absolute offset `offset`; the next Read starts there. */
    virtual bool SeekTo(uint64_t offset) = 0;
    /** @brief The absolute offset of the next byte Read returns. */
    virtual uint64_t Offset() const = 0;
    /** @brief Skips `n` bytes forward (a short skip reads; a long one seeks). */
    virtual bool Skip(uint64_t n);
    /** @brief Why the source stopped delivering, when it knows better than "the data ended" (e.g. a refused download); "" otherwise. */
    virtual std::string LastError() const { return ""; }
};

// A sample with its bytes, as Reader::Next hands it out.
struct Packet {
    int track = -1;  // index into Movie::tracks
    double pts_sec = 0.0;
    double duration_sec = 0.0;
    bool sync = false;
    std::vector<uint8_t> data;
};

class Reader {
public:
    /**
     * @brief Reads the file's setup from `src` (at offset 0): every box up to the moov (and, for a fragmented file, its sidx).
     * @param src The bytes; it must outlive the Reader.
     * @param error Why it failed.
     */
    bool Open(ByteSource *src, std::string *error);
    const Movie &movie() const { return movie_; }
    /**
     * @brief Moves the source to where the data for presentation time `sec` starts.
     *
     * Packets from there on come out as usual: video from the sync sample at or before `sec` (a decoder needs it),
     * audio a little before `sec` -- the caller drops what precedes the time.
     */
    bool Seek(double sec, std::string *error);
    /**
     * @brief The next sample in file order with its bytes.
     * @return False at the end of the file (error empty) or on a failure (error set).
     */
    bool Next(Packet *out, std::string *error);

private:
    bool ReadBoxHeader(uint64_t *size, uint32_t *type, uint64_t *header_len);
    bool NextProgressive(Packet *out, std::string *error);
    bool NextFragmented(Packet *out, std::string *error);
    bool Emit(int track, const Sample &s, Packet *out, std::string *error);

    ByteSource *src_ = nullptr;
    Movie movie_;
    // Progressive: every sample of the tracks read, in file order.
    struct Entry {
        uint64_t offset;
        int track;
        uint32_t index;
    };
    std::vector<Entry> order_;
    size_t cursor_ = 0;
    // Fragmented: the current fragment's samples, and the file offset
    // where the boxes after the setup start.
    std::vector<FragmentSample> fragment_;
    size_t fragment_cursor_ = 0;
    uint64_t data_start_ = 0;
    uint64_t mdat_end_ = 0;  // end of the mdat being read (fragmented)
};

}  // namespace mp4
