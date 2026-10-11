// Coverage for youtube_player.h's pure helpers: InnerTube request bodies,
// player responses (the muxed itag 18, the adaptive H.264 + AAC pair, an
// unplayable video, a live one, ciphered formats), search responses,
// video-id extraction, the curl command lines and the readout formatting.
// The JSON here is trimmed from real responses. No network, no display.
#include "youtube_player.h"

#include "json.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

int IndexOf(const std::vector<std::string> &argv, const std::string &needle) {
    for (size_t i = 0; i < argv.size(); i++)
        if (argv[i] == needle) return static_cast<int>(i);
    return -1;
}

const yt::Client &Android() { return yt::PlayerClients()[0]; }
const yt::Client &Vr() { return yt::PlayerClients()[1]; }

void TestClients() {
    CHECK(yt::PlayerClients().size() >= 2);
    CHECK(Android().name == "ANDROID" && Android().muxed_only && !Android().user_agent.empty());
    CHECK(Vr().name == "ANDROID_VR" && !Vr().muxed_only && !Vr().user_agent.empty());
    CHECK(yt::SearchClient().name == "WEB");
}

void TestVideoId() {
    CHECK(yt::VideoId("dQw4w9WgXcQ") == "dQw4w9WgXcQ");
    CHECK(yt::VideoId(" dQw4w9WgXcQ ") == "dQw4w9WgXcQ");
    CHECK(yt::VideoId("https://www.youtube.com/watch?v=dQw4w9WgXcQ&t=42s") == "dQw4w9WgXcQ");
    CHECK(yt::VideoId("https://www.youtube.com/watch?feature=share&v=dQw4w9WgXcQ") == "dQw4w9WgXcQ");
    CHECK(yt::VideoId("https://youtu.be/dQw4w9WgXcQ?si=abc") == "dQw4w9WgXcQ");
    CHECK(yt::VideoId("https://www.youtube.com/shorts/dQw4w9WgXcQ") == "dQw4w9WgXcQ");
    CHECK(yt::VideoId("https://www.youtube.com/embed/dQw4w9WgXcQ") == "dQw4w9WgXcQ");
    CHECK(yt::VideoId("https://example.com/").empty());
    CHECK(yt::VideoId("not an id").empty());
    CHECK(yt::CanonicalVideoUrl("dQw4w9WgXcQ") == "https://www.youtube.com/watch?v=dQw4w9WgXcQ");
    CHECK(yt::CanonicalVideoUrl("https://youtu.be/x") == "https://youtu.be/x");
}

void TestRequestBodies() {
    Json body;
    CHECK(Json::Parse(yt::PlayerRequestBody(Vr(), "dQw4w9WgXcQ"), &body));
    CHECK(body.get("videoId").as_string() == "dQw4w9WgXcQ");
    CHECK(body.get("context").get("client").get("clientName").as_string() == "ANDROID_VR");
    CHECK(body.get("context").get("client").get("androidSdkVersion").as_int() == 32);
    CHECK(body.get("context").get("client").get("deviceMake").as_string() == "Oculus");
    CHECK(Json::Parse(yt::PlayerRequestBody(Android(), "dQw4w9WgXcQ"), &body));
    CHECK(body.get("context").get("client").get("clientName").as_string() == "ANDROID");
    CHECK(body.get("context").get("client").get("androidSdkVersion").as_int() == 30);
    CHECK(Json::Parse(yt::SearchRequestBody("lofi \"hip\" hop"), &body));
    CHECK(body.get("query").as_string() == "lofi \"hip\" hop");
    CHECK(body.get("context").get("client").get("clientName").as_string() == "WEB");
}

const char *kMuxed = R"({"playabilityStatus":{"status":"OK"},
 "streamingData":{"formats":[{"itag":18,"url":"https://rr1.googlevideo.com/videoplayback?itag=18",
   "mimeType":"video/mp4; codecs=\"avc1.42001E, mp4a.40.2\"","width":640,"height":360,"fps":25}],
  "adaptiveFormats":[{"itag":137,"url":"https://x/137","mimeType":"video/mp4; codecs=\"avc1.640028\"","width":1920,"height":1080}]},
 "videoDetails":{"videoId":"dQw4w9WgXcQ","title":"Never Gonna Give You Up","author":"Rick Astley","lengthSeconds":"213","isLive":false}})";

void TestPlayerMuxed() {
    yt::StreamInfo info;
    std::string err;
    CHECK(yt::ParsePlayerResponse(kMuxed, Vr(), 480, &info, &err));
    CHECK(info.muxed && info.has_video && info.has_audio);
    CHECK(info.video_url == "https://rr1.googlevideo.com/videoplayback?itag=18" && info.audio_url == info.video_url);
    CHECK(info.video_user_agent == Vr().user_agent);
    CHECK(info.width == 640 && info.height == 360 && info.fps == 25.0);
    CHECK(info.video_codec == "avc1.42001E, mp4a.40.2");
    CHECK(info.title == "Never Gonna Give You Up" && info.channel == "Rick Astley");
    CHECK(info.duration_sec == 213.0 && info.id == "dQw4w9WgXcQ");
    // A muxed-only client takes itag 18 just the same.
    CHECK(yt::ParsePlayerResponse(kMuxed, Android(), 480, &info, &err) && info.muxed);
    CHECK(info.video_user_agent == Android().user_agent);
}

const char *kAdaptive = R"({"playabilityStatus":{"status":"OK"},
 "streamingData":{"adaptiveFormats":[
   {"itag":313,"url":"https://x/313","mimeType":"video/webm; codecs=\"vp09.00.50.08\"","width":3840,"height":2160},
   {"itag":137,"url":"https://x/137","mimeType":"video/mp4; codecs=\"avc1.640028\"","width":1920,"height":1080,"fps":25},
   {"itag":135,"url":"https://x/135","mimeType":"video/mp4; codecs=\"avc1.4D401E\"","width":854,"height":480,"fps":25},
   {"itag":134,"url":"https://x/134","mimeType":"video/mp4; codecs=\"avc1.4D401E\"","width":640,"height":360,"fps":25},
   {"itag":398,"url":"https://x/398","mimeType":"video/mp4; codecs=\"av01.0.05M.08\"","width":1280,"height":720},
   {"itag":139,"url":"https://x/139","mimeType":"audio/mp4; codecs=\"mp4a.40.5\""},
   {"itag":140,"url":"https://x/140","mimeType":"audio/mp4; codecs=\"mp4a.40.2\""},
   {"itag":251,"url":"https://x/251","mimeType":"audio/webm; codecs=\"opus\""}]},
 "videoDetails":{"videoId":"abcdefghijk","title":"t","author":"a","lengthSeconds":"60"}})";

void TestPlayerAdaptive() {
    yt::StreamInfo info;
    std::string err;
    CHECK(yt::ParsePlayerResponse(kAdaptive, Vr(), 480, &info, &err));
    CHECK(!info.muxed && info.has_video && info.has_audio);
    CHECK(info.video_url == "https://x/135" && info.height == 480);  // the tallest H.264 within the cap
    CHECK(info.audio_url == "https://x/140");                         // AAC-LC, not HE-AAC or Opus
    CHECK(info.video_user_agent == Vr().user_agent && info.audio_user_agent == Vr().user_agent);
    CHECK(info.video_codec == "avc1.4D401E");
    CHECK(yt::ParsePlayerResponse(kAdaptive, Vr(), 360, &info, &err) && info.video_url == "https://x/134");
    // A muxed-only client's adaptive URLs are PO-token gated: refused, so the resolve moves on.
    CHECK(!yt::ParsePlayerResponse(kAdaptive, Android(), 480, &info, &err));
    CHECK(err.find("muxed") != std::string::npos);
}

void TestPlayerRefusals() {
    yt::StreamInfo info;
    std::string err;
    CHECK(!yt::ParsePlayerResponse(R"({"playabilityStatus":{"status":"LOGIN_REQUIRED","reason":"Sign in to confirm you're not a bot"}})",
                                   Vr(), 480, &info, &err));
    CHECK(err == "Sign in to confirm you're not a bot");
    CHECK(!yt::ParsePlayerResponse(R"({"playabilityStatus":{"status":"ERROR"}})", Vr(), 480, &info, &err));
    CHECK(err.find("ERROR") != std::string::npos);
    CHECK(!yt::ParsePlayerResponse("not json", Vr(), 480, &info, &err) && !err.empty());
    CHECK(!yt::ParsePlayerResponse(R"({"playabilityStatus":{"status":"OK"},"videoDetails":{"isLive":true}})", Vr(), 480, &info, &err));
    CHECK(err.find("live") != std::string::npos);
    // Only ciphered or undecodable formats.
    CHECK(!yt::ParsePlayerResponse(R"({"playabilityStatus":{"status":"OK"},"streamingData":{"adaptiveFormats":[
        {"itag":137,"signatureCipher":"s=x","mimeType":"video/mp4; codecs=\"avc1.640028\"","height":1080},
        {"itag":251,"url":"https://x","mimeType":"audio/webm; codecs=\"opus\""}]}})",
                                   Vr(), 1080, &info, &err));
    CHECK(err.find("decode") != std::string::npos);
}

const char *kSearch = R"({"contents":{"twoColumnSearchResultsRenderer":{"primaryContents":{"sectionListRenderer":{"contents":[
 {"itemSectionRenderer":{"contents":[
  {"videoRenderer":{"videoId":"n61ULEU7CO0","title":{"runs":[{"text":"Best of lofi"},{"text":" 2021"}]},
    "ownerText":{"runs":[{"text":"Lofi Girl"}]},"lengthText":{"simpleText":"6:10:58"},"viewCountText":{"simpleText":"58,000,859 views"}}},
  {"channelRenderer":{"channelId":"UCx"}},
  {"videoRenderer":{"videoId":"rFZHOHl-L8A","title":{"simpleText":"lofi radio"},"longBylineText":{"runs":[{"text":"Lofi Girl"}]},
    "viewCountText":{"runs":[{"text":"31K"},{"text":" watching"}]}}},
  {"videoRenderer":{"videoId":"short"}}]}}]}}}}})";

void TestSearch() {
    const std::vector<yt::SearchResult> rows = yt::ParseSearchResponse(kSearch);
    CHECK(rows.size() == 2);  // the channel and the malformed id are skipped
    CHECK(rows[0].id == "n61ULEU7CO0" && rows[0].title == "Best of lofi 2021" && rows[0].channel == "Lofi Girl");
    CHECK(rows[0].duration_sec == 6 * 3600 + 10 * 60 + 58 && rows[0].view_count == 58000859.0 && !rows[0].is_live);
    CHECK(rows[0].url == "https://www.youtube.com/watch?v=n61ULEU7CO0");
    CHECK(rows[1].channel == "Lofi Girl" && rows[1].duration_sec == 0.0 && rows[1].is_live);
    CHECK(yt::ParseSearchResponse("").empty());
    CHECK(yt::ParseSearchResponse("{}").empty());
}

void TestArgv() {
    const std::vector<std::string> a = yt::InnertubeArgv(yt::kPlayerEndpoint, Vr(), "{\"x\":1}");
    CHECK(a.front() == "curl" && a.back() == yt::kPlayerEndpoint);
    const int ua = IndexOf(a, "-A");
    CHECK(ua >= 0 && a[static_cast<size_t>(ua) + 1] == Vr().user_agent);
    const int data = IndexOf(a, "--data-binary");
    CHECK(data >= 0 && a[static_cast<size_t>(data) + 1] == "{\"x\":1}");
    CHECK(IndexOf(a, "Content-Type: application/json") >= 0);
    const std::vector<std::string> f = yt::FetchArgv("https://i.ytimg.com/vi/x/mqdefault.jpg");
    CHECK(f.front() == "curl" && f.back() == "https://i.ytimg.com/vi/x/mqdefault.jpg");
}

void TestFormatting() {
    CHECK(yt::FormatDuration(0.0) == "--:--");
    CHECK(yt::FormatDuration(5.0) == "0:05");
    CHECK(yt::FormatDuration(635.0) == "10:35");
    CHECK(yt::FormatDuration(3725.0) == "1:02:05");
    CHECK(yt::FormatViewCount(0.0).empty());
    CHECK(yt::FormatViewCount(950.0) == "950 views");
    CHECK(yt::FormatViewCount(12600.0) == "13K views");
    CHECK(yt::FormatViewCount(1234567.0) == "1.2M views");
    CHECK(yt::FormatViewCount(2.5e9) == "2.5B views");
    CHECK(yt::ThumbnailUrl("abc") == "https://i.ytimg.com/vi/abc/mqdefault.jpg");
}

}  // namespace

int main() {
    TestClients();
    TestVideoId();
    TestRequestBodies();
    TestPlayerMuxed();
    TestPlayerAdaptive();
    TestPlayerRefusals();
    TestSearch();
    TestArgv();
    TestFormatting();
    std::printf("youtube_player_test: all checks passed\n");
    return 0;
}
