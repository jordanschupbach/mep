#include "youtube_player.h"

#include "json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace yt {

namespace {

std::string HeadersForFfmpeg(const Json &http_headers) {
    std::string out;
    if (!http_headers.is_object()) return out;
    for (const auto &kv : http_headers.fields()) {
        if (!kv.second.is_string()) continue;
        out += kv.first;
        out += ": ";
        out += kv.second.as_string();
        out += "\r\n";
    }
    return out;
}

bool IsVideoId(const std::string &s) {
    if (s.size() != 11) return false;
    for (char c : s) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

std::string Trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return s.substr(a, b - a);
}

}  // namespace

bool ParseSearchLine(const std::string &json_line, SearchResult *out) {
    if (out == nullptr) return false;
    std::string line = Trim(json_line);
    if (line.empty() || line[0] != '{') return false;
    Json j;
    if (!Json::Parse(line, &j) || !j.is_object()) return false;
    const std::string id = j.get("id").as_string();
    if (id.empty()) return false;
    SearchResult r;
    r.id = id;
    r.url = j.get("url").as_string();
    if (r.url.empty()) r.url = j.get("webpage_url").as_string();
    if (r.url.empty()) r.url = "https://www.youtube.com/watch?v=" + id;
    r.title = j.get("title").as_string();
    if (r.title.empty()) r.title = id;
    r.channel = j.get("channel").as_string();
    if (r.channel.empty()) r.channel = j.get("uploader").as_string();
    r.duration_sec = j.get("duration").as_double(0.0);
    r.view_count = j.get("view_count").as_double(0.0);
    const Json &live = j.get("live_status");
    r.is_live = live.is_string() && live.as_string() == "is_live";
    *out = std::move(r);
    return true;
}

bool ParseStreamInfo(const std::string &json, StreamInfo *out, std::string *error) {
    auto fail = [&](const std::string &msg) {
        if (error) *error = msg;
        return false;
    };
    if (out == nullptr) return fail("no output");
    Json j;
    if (!Json::Parse(Trim(json), &j) || !j.is_object()) return fail("yt-dlp produced no JSON");
    StreamInfo info;
    info.id = j.get("id").as_string();
    info.title = j.get("title").as_string();
    info.channel = j.get("channel").as_string();
    if (info.channel.empty()) info.channel = j.get("uploader").as_string();
    info.duration_sec = j.get("duration").as_double(0.0);

    auto take_video = [&](const Json &f) {
        info.video_url = f.get("url").as_string();
        info.video_headers = HeadersForFfmpeg(f.get("http_headers"));
        info.width = f.get("width").as_int(0);
        info.height = f.get("height").as_int(0);
        double fps = f.get("fps").as_double(0.0);
        if (fps > 0.0) info.fps = fps;
        info.has_video = !info.video_url.empty();
    };
    auto take_audio = [&](const Json &f) {
        info.audio_url = f.get("url").as_string();
        info.audio_headers = HeadersForFfmpeg(f.get("http_headers"));
        info.has_audio = !info.audio_url.empty();
    };
    auto codec_present = [](const Json &f, const char *key) {
        const Json &c = f.get(key);
        return c.is_string() && !c.as_string().empty() && c.as_string() != "none";
    };

    const Json &reqs = j.get("requested_formats");
    if (reqs.is_array() && reqs.size() > 0) {
        for (const Json &f : reqs.items()) {
            bool v = codec_present(f, "vcodec");
            bool a = codec_present(f, "acodec");
            if (v && !info.has_video) take_video(f);
            if (a && !info.has_audio) take_audio(f);
            if (v && a && !info.muxed) info.muxed = (reqs.size() == 1);
        }
    } else if (!j.get("url").as_string().empty()) {
        // A single pre-muxed format (itag 18 and friends): the top-level
        // object carries the format fields itself.
        bool v = codec_present(j, "vcodec") || j.get("width").as_int(0) > 0;
        bool a = codec_present(j, "acodec");
        if (v) take_video(j);
        if (a || !v) take_audio(j);
        info.muxed = info.has_video && info.has_audio;
    }
    if (!info.has_video && !info.has_audio) return fail("no playable format in yt-dlp output");
    if (info.has_video && (info.width <= 0 || info.height <= 0)) {
        info.width = 640;
        info.height = 360;
    }
    if (info.fps <= 0.0 || info.fps > 120.0) info.fps = 30.0;
    *out = std::move(info);
    return true;
}

std::string CanonicalVideoUrl(const std::string &url_or_id) {
    std::string s = Trim(url_or_id);
    if (IsVideoId(s)) return "https://www.youtube.com/watch?v=" + s;
    return s;
}

std::vector<std::string> SearchArgv(const std::string &query, int max_results) {
    int n = std::clamp(max_results, 1, 50);
    return {"yt-dlp", "--no-update", "--no-warnings", "-q", "--flat-playlist", "-j",
            "ytsearch" + std::to_string(n) + ":" + query};
}

std::vector<std::string> ResolveArgv(const std::string &url, const std::string &player_client, int max_height) {
    int h = max_height > 0 ? max_height : 480;
    std::string hs = std::to_string(h);
    // Prefer one muxed mp4 (single fetch, h264 decodes cheaply), then a
    // separate video+audio pair, then whatever is best.
    std::string fmt = "b[height<=" + hs + "][ext=mp4]/b[height<=" + hs + "]/bv*[height<=" + hs + "][vcodec^=avc1]+ba/bv*[height<=" +
                      hs + "]+ba/b";
    std::vector<std::string> argv = {"yt-dlp", "--no-update", "--no-warnings", "-q", "--no-playlist", "-j", "-f", fmt};
    if (!player_client.empty()) {
        argv.push_back("--extractor-args");
        argv.push_back("youtube:player_client=" + player_client);
    }
    argv.push_back(url);
    return argv;
}

namespace {

std::string SecondsArg(double s) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.3f", std::max(0.0, s));
    return buf;
}

std::vector<std::string> CommonInputArgs(const std::string &headers, double start_sec, const std::string &url) {
    std::vector<std::string> argv = {"ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin",
                                     "-reconnect", "1", "-reconnect_streamed", "1", "-reconnect_delay_max", "5"};
    if (!headers.empty()) {
        argv.push_back("-headers");
        argv.push_back(headers);
    }
    if (start_sec > 0.0) {
        argv.push_back("-ss");
        argv.push_back(SecondsArg(start_sec));
    }
    argv.push_back("-i");
    argv.push_back(url);
    return argv;
}

}  // namespace

std::vector<std::string> VideoDecodeArgv(const StreamInfo &info, double start_sec, int width, int height, double fps) {
    std::vector<std::string> argv = CommonInputArgs(info.video_headers, start_sec, info.video_url);
    char fps_buf[32];
    std::snprintf(fps_buf, sizeof(fps_buf), "%.3f", fps > 0.0 ? fps : 30.0);
    std::vector<std::string> tail = {"-an", "-sn", "-dn", "-map", "0:v:0",
                                     "-vf", "scale=" + std::to_string(width) + ":" + std::to_string(height),
                                     "-fps_mode", "cfr", "-r", fps_buf, "-pix_fmt", "rgba", "-f", "rawvideo", "pipe:1"};
    argv.insert(argv.end(), tail.begin(), tail.end());
    return argv;
}

std::vector<std::string> AudioDecodeArgv(const StreamInfo &info, double start_sec, int channels, int rate) {
    std::vector<std::string> argv = CommonInputArgs(info.audio_headers, start_sec, info.audio_url);
    std::vector<std::string> tail = {"-vn", "-sn", "-dn", "-map", "0:a:0", "-ac", std::to_string(channels),
                                     "-ar", std::to_string(rate), "-f", "s16le", "pipe:1"};
    argv.insert(argv.end(), tail.begin(), tail.end());
    return argv;
}

void FrameAssembler::Reset(size_t frame_bytes) {
    frame_bytes_ = frame_bytes;
    partial_.clear();
    frames_.clear();
}

size_t FrameAssembler::Push(const char *data, size_t len) {
    if (frame_bytes_ == 0 || data == nullptr) return frames_.size();
    size_t off = 0;
    while (off < len) {
        size_t need = frame_bytes_ - partial_.size();
        size_t take = std::min(need, len - off);
        partial_.insert(partial_.end(), reinterpret_cast<const uint8_t *>(data + off),
                        reinterpret_cast<const uint8_t *>(data + off + take));
        off += take;
        if (partial_.size() == frame_bytes_) {
            frames_.push_back(std::move(partial_));
            partial_ = std::vector<uint8_t>();
            partial_.reserve(frame_bytes_);
        }
    }
    return frames_.size();
}

bool FrameAssembler::Pop(std::vector<uint8_t> *out) {
    if (frames_.empty()) return false;
    if (out) *out = std::move(frames_.front());
    frames_.pop_front();
    return true;
}

std::string FormatDuration(double seconds) {
    if (!(seconds > 0.0)) return "--:--";
    long long total = static_cast<long long>(std::llround(seconds));
    long long h = total / 3600, m = (total % 3600) / 60, s = total % 60;
    char buf[32];
    if (h > 0)
        std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", h, m, s);
    else
        std::snprintf(buf, sizeof(buf), "%lld:%02lld", m, s);
    return buf;
}

std::string FormatViewCount(double views) {
    if (!(views > 0.0)) return "";
    char buf[32];
    if (views >= 1e9)
        std::snprintf(buf, sizeof(buf), "%.1fB views", views / 1e9);
    else if (views >= 1e6)
        std::snprintf(buf, sizeof(buf), "%.1fM views", views / 1e6);
    else if (views >= 1e3)
        std::snprintf(buf, sizeof(buf), "%.0fK views", views / 1e3);
    else
        std::snprintf(buf, sizeof(buf), "%.0f views", views);
    return buf;
}

std::string ThumbnailUrl(const std::string &id) { return "https://i.ytimg.com/vi/" + id + "/mqdefault.jpg"; }

}  // namespace yt
