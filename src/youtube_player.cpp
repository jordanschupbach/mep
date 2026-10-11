#include "youtube_player.h"

#include "json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace yt {

namespace {

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

// InnerTube's text objects: {"simpleText": "..."} or {"runs": [{"text": ...}, ...]}.
std::string Text(const Json &t) {
    if (t.get("simpleText").is_string()) return t.get("simpleText").as_string();
    std::string out;
    for (const Json &run : t.get("runs").items()) out += run.get("text").as_string();
    return out;
}

// "3:45" / "1:02:03" -> seconds; 0 when it isn't one.
double ClockSeconds(const std::string &s) {
    double total = 0.0;
    double part = 0.0;
    bool any = false;
    for (char c : s) {
        if (c >= '0' && c <= '9') {
            part = part * 10.0 + (c - '0');
            any = true;
        } else if (c == ':') {
            total = (total + part) * 60.0;
            part = 0.0;
        } else {
            return 0.0;
        }
    }
    return any ? total + part : 0.0;
}

// The digits of "14,141,464 views" as a number; 0 when there are none.
double DigitsValue(const std::string &s) {
    double v = 0.0;
    bool any = false;
    for (char c : s) {
        if (c >= '0' && c <= '9') {
            v = v * 10.0 + (c - '0');
            any = true;
        } else if (c != ',' && c != '.' && c != ' ') {
            if (any) break;
        }
    }
    return v;
}

// mimeType 'video/mp4; codecs="avc1.42001E, mp4a.40.2"' -> its codecs value.
std::string Codecs(const std::string &mime) {
    const size_t at = mime.find("codecs=\"");
    if (at == std::string::npos) return "";
    const size_t from = at + 8;
    const size_t to = mime.find('"', from);
    return mime.substr(from, to == std::string::npos ? std::string::npos : to - from);
}

bool StartsWith(const std::string &s, const char *prefix) { return s.rfind(prefix, 0) == 0; }

// Every renderer object under `j` named `key`, depth first.
void Collect(const Json &j, const char *key, std::vector<const Json *> *out) {
    if (j.is_object()) {
        for (const auto &kv : j.fields()) {
            if (kv.first == key && kv.second.is_object())
                out->push_back(&kv.second);
            else
                Collect(kv.second, key, out);
        }
    } else if (j.is_array()) {
        for (const Json &item : j.items()) Collect(item, key, out);
    }
}

}  // namespace

const std::vector<Client> &PlayerClients() {
    static const std::vector<Client> clients = {
        {"ANDROID", "20.10.38", "com.google.android.youtube/20.10.38 (Linux; U; Android 11) gzip", true},
        {"ANDROID_VR", "1.62.27",
         "com.google.android.apps.youtube.vr.oculus/1.62.27 (Linux; U; Android 12L; eureka-user Build/SQ3A.220605.009.A1) gzip"},
    };
    return clients;
}

const Client &SearchClient() {
    static const Client client = {"WEB", "2.20250101.00.00",
                                  "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) "
                                  "Chrome/131.0.0.0 Safari/537.36"};
    return client;
}

std::string VideoId(const std::string &url_or_id) {
    const std::string s = Trim(url_or_id);
    if (IsVideoId(s)) return s;
    auto id_after = [&](const char *marker) -> std::string {
        const size_t at = s.find(marker);
        if (at == std::string::npos) return "";
        const std::string id = s.substr(at + std::strlen(marker), 11);
        return IsVideoId(id) ? id : "";
    };
    for (const char *marker : {"?v=", "&v=", "youtu.be/", "/shorts/", "/embed/", "/live/", "/v/"}) {
        std::string id = id_after(marker);
        if (!id.empty()) return id;
    }
    return "";
}

std::string CanonicalVideoUrl(const std::string &url_or_id) {
    std::string s = Trim(url_or_id);
    if (IsVideoId(s)) return "https://www.youtube.com/watch?v=" + s;
    return s;
}

std::string PlayerRequestBody(const Client &client, const std::string &video_id) {
    Json c = Json::Object();
    c["clientName"] = client.name;
    c["clientVersion"] = client.version;
    c["hl"] = "en";
    c["gl"] = "US";
    if (client.name == "ANDROID_VR") {
        c["deviceMake"] = "Oculus";
        c["deviceModel"] = "Quest 3";
        c["androidSdkVersion"] = 32;
        c["osName"] = "Android";
        c["osVersion"] = "12L";
    } else if (client.name == "ANDROID") {
        c["androidSdkVersion"] = 30;
        c["osName"] = "Android";
        c["osVersion"] = "11";
    }
    Json context = Json::Object();
    context["client"] = std::move(c);
    Json body = Json::Object();
    body["context"] = std::move(context);
    body["videoId"] = video_id;
    body["contentCheckOk"] = true;
    body["racyCheckOk"] = true;
    return body.dump();
}

std::string SearchRequestBody(const std::string &query) {
    Json c = Json::Object();
    c["clientName"] = SearchClient().name;
    c["clientVersion"] = SearchClient().version;
    c["hl"] = "en";
    c["gl"] = "US";
    Json context = Json::Object();
    context["client"] = std::move(c);
    Json body = Json::Object();
    body["context"] = std::move(context);
    body["query"] = query;
    body["params"] = "EgIQAQ%3D%3D";  // the "Videos" filter: no channels or playlists
    return body.dump();
}

bool ParsePlayerResponse(const std::string &json, const Client &client, int max_height, StreamInfo *out, std::string *error) {
    auto fail = [&](const std::string &msg) {
        if (error) *error = msg;
        return false;
    };
    if (out == nullptr) return fail("no output");
    Json j;
    if (!Json::Parse(Trim(json), &j) || !j.is_object()) return fail("YouTube sent no usable answer");
    const Json &status = j.get("playabilityStatus");
    if (status.get("status").as_string() != "OK") {
        std::string reason = status.get("reason").as_string();
        if (reason.empty() && status.get("messages").size() > 0) reason = status.get("messages").items()[0].as_string();
        if (reason.empty()) reason = "this video can't be played (" + status.get("status").as_string("no status") + ")";
        return fail(reason);
    }
    StreamInfo info;
    const Json &details = j.get("videoDetails");
    info.id = details.get("videoId").as_string();
    info.title = details.get("title").as_string();
    info.channel = details.get("author").as_string();
    info.duration_sec = std::atof(details.get("lengthSeconds").as_string("0").c_str());
    if (details.get("isLive").as_bool()) return fail("live streams aren't supported yet");

    const Json &streaming = j.get("streamingData");
    // The muxed 360p mp4: one download carries both tracks.
    for (const Json &f : streaming.get("formats").items()) {
        if (f.get("itag").as_int() != 18 || !f.get("url").is_string()) continue;
        info.video_url = info.audio_url = f.get("url").as_string();
        info.video_user_agent = info.audio_user_agent = client.user_agent;
        info.video_codec = Codecs(f.get("mimeType").as_string());
        info.width = f.get("width").as_int(640);
        info.height = f.get("height").as_int(360);
        if (f.get("fps").as_double() > 0.0) info.fps = f.get("fps").as_double();
        info.has_video = info.has_audio = info.muxed = true;
        *out = std::move(info);
        return true;
    }
    if (client.muxed_only) return fail("no muxed 360p format for " + client.name);
    // Otherwise a video-only H.264 mp4 and an AAC-LC audio mp4.
    const Json *video = nullptr;
    const Json *audio = nullptr;
    const int cap = max_height > 0 ? max_height : 480;
    for (const Json &f : streaming.get("adaptiveFormats").items()) {
        if (!f.get("url").is_string()) continue;  // a ciphered URL: not for these clients
        const std::string mime = f.get("mimeType").as_string();
        const std::string codecs = Codecs(mime);
        if (StartsWith(mime, "video/mp4") && StartsWith(codecs, "avc1")) {
            const int h = f.get("height").as_int();
            if (h <= 0 || h > cap) continue;
            if (!video || h > video->get("height").as_int()) video = &f;
        } else if (StartsWith(mime, "audio/mp4") && codecs == "mp4a.40.2") {
            if (!audio || f.get("itag").as_int() == 140) audio = &f;
        }
    }
    if (!video && !audio) return fail("no format this player can decode (it plays H.264 + AAC)");
    if (video) {
        info.video_url = video->get("url").as_string();
        info.video_user_agent = client.user_agent;
        info.video_codec = Codecs(video->get("mimeType").as_string());
        info.width = video->get("width").as_int();
        info.height = video->get("height").as_int();
        if (video->get("fps").as_double() > 0.0) info.fps = video->get("fps").as_double();
        info.has_video = true;
    }
    if (audio) {
        info.audio_url = audio->get("url").as_string();
        info.audio_user_agent = client.user_agent;
        info.has_audio = true;
    }
    if (info.fps <= 0.0 || info.fps > 120.0) info.fps = 30.0;
    *out = std::move(info);
    return true;
}

std::vector<SearchResult> ParseSearchResponse(const std::string &json) {
    std::vector<SearchResult> out;
    Json j;
    if (!Json::Parse(Trim(json), &j)) return out;
    std::vector<const Json *> rows;
    Collect(j, "videoRenderer", &rows);
    for (const Json *row : rows) {
        SearchResult r;
        r.id = row->get("videoId").as_string();
        if (!IsVideoId(r.id)) continue;
        r.url = "https://www.youtube.com/watch?v=" + r.id;
        r.title = Text(row->get("title"));
        if (r.title.empty()) r.title = r.id;
        r.channel = Text(row->get("ownerText"));
        if (r.channel.empty()) r.channel = Text(row->get("longBylineText"));
        r.duration_sec = ClockSeconds(Text(row->get("lengthText")));
        const std::string views = Text(row->get("viewCountText"));
        r.view_count = DigitsValue(views);
        r.is_live = !row->get("lengthText").is_object() && views.find("watching") != std::string::npos;
        out.push_back(std::move(r));
    }
    return out;
}

std::vector<std::string> InnertubeArgv(const char *endpoint, const Client &client, const std::string &body) {
    return {"curl", "-s", "--max-time", "20", "-X", "POST", "-H", "Content-Type: application/json", "-A", client.user_agent,
            "--data-binary", body, endpoint};
}

std::vector<std::string> FetchArgv(const std::string &url) { return {"curl", "-sL", "--max-time", "15", url}; }

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
