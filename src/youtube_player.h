#pragma once

// Editor-free helpers for the in-pane YouTube player (Mode::Youtube,
// YoutubeSession in editor.h, DrawYoutubePane in main.cpp).
//
// The player never links a media library: `yt-dlp` resolves a watch page
// to direct stream URLs (plus the HTTP headers those URLs are bound to)
// and runs searches, and `ffmpeg` decodes the streams into raw RGBA
// frames / PCM16 samples over a pipe (JobManager raw-stdout jobs). This
// header holds the pure pieces of that pipeline -- yt-dlp JSON parsing,
// command-line construction and the byte-stream-to-frame assembler -- so
// they can be unit-tested (src/youtube_player_test.cpp) without an
// Editor, a display or a network.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace yt {

// One row of a `yt-dlp "ytsearchN:query" --flat-playlist -j` result
// (one JSON object per line).
struct SearchResult {
    std::string id;
    std::string url;
    std::string title;
    std::string channel;
    double duration_sec = 0.0;  // 0 when unknown (live streams, playlists)
    double view_count = 0.0;    // 0 when unknown
    bool is_live = false;
};

// Parses one line of flat-playlist search output. Returns false for lines
// that aren't a video entry (blank, warnings, malformed JSON, no id).
bool ParseSearchLine(const std::string &json_line, SearchResult *out);

// Everything the decoder needs for one video, from `yt-dlp -j URL`.
// `video_headers`/`audio_headers` are already in ffmpeg's `-headers`
// form ("Name: value\r\n" concatenated), because googlevideo stream URLs
// are bound to the User-Agent (and friends) yt-dlp requested them with
// -- fetching one with different headers is a 403.
struct StreamInfo {
    std::string id;
    std::string title;
    std::string channel;
    double duration_sec = 0.0;
    double fps = 30.0;
    int width = 0, height = 0;
    std::string video_url, video_headers;
    std::string audio_url, audio_headers;
    bool has_video = false;
    bool has_audio = false;
    // True when one muxed URL carries both tracks (e.g. itag 18): the
    // audio ffmpeg job then reads the same URL as the video one.
    bool muxed = false;
};

bool ParseStreamInfo(const std::string &json, StreamInfo *out, std::string *error);

// Accepts a watch URL, a youtu.be short link, a bare 11-char video id or
// any other URL yt-dlp might understand; returns the canonical form to
// hand to yt-dlp (watch URL for ids, the input untouched otherwise).
std::string CanonicalVideoUrl(const std::string &url_or_id);

// Command lines. `player_client` is yt-dlp's `youtube:player_client`
// extractor arg; "android" is the default because its URLs are fetchable
// by a plain ffmpeg GET (the default web clients hand out PO-token-bound
// URLs that 403 outside yt-dlp itself).
std::vector<std::string> SearchArgv(const std::string &query, int max_results);
std::vector<std::string> ResolveArgv(const std::string &url, const std::string &player_client, int max_height);
// ffmpeg decoding `info`'s video track from `start_sec` into raw RGBA
// frames of exactly `width`x`height` at constant `fps` on stdout.
std::vector<std::string> VideoDecodeArgv(const StreamInfo &info, double start_sec, int width, int height, double fps);
// ffmpeg decoding `info`'s audio track from `start_sec` into interleaved
// PCM16 (`channels` x `rate`) on stdout.
std::vector<std::string> AudioDecodeArgv(const StreamInfo &info, double start_sec, int channels, int rate);

// Reassembles fixed-size frames out of arbitrarily-chunked pipe reads.
class FrameAssembler {
public:
    explicit FrameAssembler(size_t frame_bytes = 0) : frame_bytes_(frame_bytes) {}
    void Reset(size_t frame_bytes);
    size_t FrameBytes() const { return frame_bytes_; }
    // Appends a chunk; returns how many complete frames are now queued.
    size_t Push(const char *data, size_t len);
    size_t Ready() const { return frames_.size(); }
    // Pops the oldest complete frame (false when none is ready).
    bool Pop(std::vector<uint8_t> *out);
    // Drops every queued frame but keeps the partial tail.
    void DropAll() { frames_.clear(); }

private:
    size_t frame_bytes_ = 0;
    std::vector<uint8_t> partial_;
    std::deque<std::vector<uint8_t>> frames_;
};

// "m:ss" / "h:mm:ss" for the transport readout and result rows.
std::string FormatDuration(double seconds);
// "1.2M views" style.
std::string FormatViewCount(double views);
// Thumbnail URL for a video id (YouTube's stable mqdefault JPEG).
std::string ThumbnailUrl(const std::string &id);

}  // namespace yt
