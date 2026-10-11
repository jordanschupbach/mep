#pragma once

// Editor-free helpers for the in-pane YouTube player (Mode::Youtube,
// YoutubeSession in editor.h, DrawYoutubePane in main.cpp).
//
// The player needs no media tool of its own: YouTube's InnerTube API (the
// JSON endpoints its own apps call) resolves a video to direct stream URLs
// and runs searches, `curl` carries the HTTPS (the same way http_client.h
// fetches every https:// page), and mp4_player.h demuxes and decodes the
// streams in-process (mp4_demux.h, h264_decoder.h, aac_decoder.h). This
// header holds the pure pieces of that: request bodies, response parsing,
// format choice and the curl command lines -- unit-tested in
// src/youtube_player_test.cpp without an Editor, a display or a network.

#include <string>
#include <vector>

namespace yt {

// One video row of a search.
struct SearchResult {
    std::string id;
    std::string url;
    std::string title;
    std::string channel;
    double duration_sec = 0.0;  // 0 when unknown (live streams, playlists)
    double view_count = 0.0;    // 0 when unknown
    bool is_live = false;
};

// Everything the decoder needs for one video. `video_user_agent` /
// `audio_user_agent` are what the stream URLs must be fetched with:
// googlevideo URLs are bound to the client that asked for them.
struct StreamInfo {
    std::string id;
    std::string title;
    std::string channel;
    double duration_sec = 0.0;
    double fps = 30.0;
    int width = 0, height = 0;
    std::string video_url, video_user_agent;
    std::string audio_url, audio_user_agent;
    std::string video_codec;  // the format's codecs= value, e.g. "avc1.42001E"
    bool has_video = false;
    bool has_audio = false;
    // True when one muxed URL carries both tracks (itag 18): the player
    // then reads both from the one download.
    bool muxed = false;
};

// An InnerTube client identity: which app the request claims to be. The
// stream URLs a client is handed only work when fetched with its
// `user_agent`.
struct Client {
    std::string name;     // context.client.clientName, e.g. "ANDROID_VR"
    std::string version;  // context.client.clientVersion
    std::string user_agent;
    // Only its muxed itag 18 downloads in full: its adaptive URLs want a PO
    // token, without which googlevideo refuses everything past the first
    // few hundred KB.
    bool muxed_only = false;
};

// The clients a resolve tries, in order. None needs a signature or a PO
// token for what it is used for: ANDROID for the muxed itag 18 (the one
// YouTube's bot check passes most often), then ANDROID_VR (itag 18 or the
// adaptive pair, when its bot check passes). IOS is not among them: its
// URLs all want a PO token now, and it no longer lists HLS for ordinary
// videos.
const std::vector<Client> &PlayerClients();
// The client searches go through (WEB: its results carry durations and
// view counts).
const Client &SearchClient();

constexpr const char *kPlayerEndpoint = "https://www.youtube.com/youtubei/v1/player?prettyPrint=false";
constexpr const char *kSearchEndpoint = "https://www.youtube.com/youtubei/v1/search?prettyPrint=false";

// The video id in a watch URL (watch?v=, youtu.be/, /shorts/, /embed/,
// /live/), or `url_or_id` itself when it already is one; "" when there is
// none.
std::string VideoId(const std::string &url_or_id);
// A watch URL for a bare 11-char video id; anything else untouched.
std::string CanonicalVideoUrl(const std::string &url_or_id);

// Request bodies (JSON) for the player and search endpoints.
std::string PlayerRequestBody(const Client &client, const std::string &video_id);
std::string SearchRequestBody(const std::string &query);

// Reads a player response into `out`, choosing what to play: the muxed
// itag 18 when it is listed, else (unless `client.muxed_only`) the best
// H.264 video no taller than `max_height` plus the AAC audio (itag 140, or
// any mp4a). `client` is the one that asked (its user agent goes with the
// URLs). False with
// `error` set when the video is unplayable (the response's own reason, e.g.
// "Sign in to confirm you're not a bot") or offers nothing this player
// decodes.
bool ParsePlayerResponse(const std::string &json, const Client &client, int max_height, StreamInfo *out, std::string *error);
// The video rows of a search response, in order (channels, playlists and
// shelves are skipped).
std::vector<SearchResult> ParseSearchResponse(const std::string &json);

// curl command lines: a POST of `body` to an InnerTube endpoint as
// `client`, and a plain GET (thumbnails).
std::vector<std::string> InnertubeArgv(const char *endpoint, const Client &client, const std::string &body);
std::vector<std::string> FetchArgv(const std::string &url);

// "m:ss" / "h:mm:ss" for the transport readout and result rows.
std::string FormatDuration(double seconds);
// "1.2M views" style.
std::string FormatViewCount(double views);
// Thumbnail URL for a video id (YouTube's stable mqdefault JPEG).
std::string ThumbnailUrl(const std::string &id);

}  // namespace yt
