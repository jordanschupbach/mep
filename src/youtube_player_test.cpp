// Coverage for youtube_player.h's pure helpers: yt-dlp JSON parsing
// (flat-playlist search rows, resolved stream info in both the muxed
// single-format and requested_formats shapes), URL canonicalisation,
// ffmpeg/yt-dlp argv construction, and the FrameAssembler's behaviour
// across arbitrary chunk boundaries. No network, no display.
#include "youtube_player.h"

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

bool Has(const std::vector<std::string> &argv, const std::string &needle) {
    for (const std::string &a : argv)
        if (a == needle) return true;
    return false;
}

int IndexOf(const std::vector<std::string> &argv, const std::string &needle) {
    for (size_t i = 0; i < argv.size(); i++)
        if (argv[i] == needle) return static_cast<int>(i);
    return -1;
}

void TestSearchLine() {
    yt::SearchResult r;
    CHECK(!yt::ParseSearchLine("", &r));
    CHECK(!yt::ParseSearchLine("WARNING: something", &r));
    CHECK(!yt::ParseSearchLine("{\"title\": \"no id\"}", &r));
    const char *line =
        "{\"_type\": \"url\", \"id\": \"aqz-KE-bpKQ\", \"url\": \"https://www.youtube.com/watch?v=aqz-KE-bpKQ\", "
        "\"title\": \"Big Buck Bunny\", \"duration\": 635.0, \"channel\": \"Blender\", \"view_count\": 1234567, "
        "\"live_status\": \"not_live\"}\n";
    CHECK(yt::ParseSearchLine(line, &r));
    CHECK(r.id == "aqz-KE-bpKQ");
    CHECK(r.title == "Big Buck Bunny");
    CHECK(r.channel == "Blender");
    CHECK(r.duration_sec == 635.0);
    CHECK(r.view_count == 1234567.0);
    CHECK(!r.is_live);
    CHECK(r.url == "https://www.youtube.com/watch?v=aqz-KE-bpKQ");

    // Live rows: null duration, is_live status, uploader instead of channel.
    const char *live = "{\"id\": \"liveid12345\", \"title\": \"radio\", \"duration\": null, \"uploader\": \"Lofi\", "
                       "\"live_status\": \"is_live\"}";
    CHECK(yt::ParseSearchLine(live, &r));
    CHECK(r.is_live);
    CHECK(r.duration_sec == 0.0);
    CHECK(r.channel == "Lofi");
    CHECK(r.url == "https://www.youtube.com/watch?v=liveid12345");
}

void TestStreamInfoMuxed() {
    const char *json =
        "{\"id\": \"aqz-KE-bpKQ\", \"title\": \"Big Buck Bunny\", \"duration\": 635, \"channel\": \"Blender\", "
        "\"format_id\": \"18\", \"url\": \"https://rr5.googlevideo.com/videoplayback?itag=18\", "
        "\"vcodec\": \"avc1.42001E\", \"acodec\": \"mp4a.40.2\", \"width\": 640, \"height\": 360, \"fps\": 30, "
        "\"http_headers\": {\"User-Agent\": \"Mozilla/5.0 test\", \"Accept\": \"*/*\"}}";
    yt::StreamInfo info;
    std::string err;
    CHECK(yt::ParseStreamInfo(json, &info, &err));
    CHECK(info.muxed);
    CHECK(info.has_video && info.has_audio);
    CHECK(info.video_url == info.audio_url);
    CHECK(info.width == 640 && info.height == 360);
    CHECK(info.fps == 30.0);
    CHECK(info.duration_sec == 635.0);
    CHECK(info.video_headers == "User-Agent: Mozilla/5.0 test\r\nAccept: */*\r\n");
    CHECK(info.audio_headers == info.video_headers);
}

void TestStreamInfoSplit() {
    const char *json =
        "{\"id\": \"x\", \"title\": \"Split\", \"duration\": 10.5, \"requested_formats\": ["
        "{\"format_id\": \"135\", \"url\": \"https://v.example/v\", \"vcodec\": \"avc1\", \"acodec\": \"none\", "
        "\"width\": 854, \"height\": 480, \"fps\": 24, \"http_headers\": {\"User-Agent\": \"ua\"}},"
        "{\"format_id\": \"140\", \"url\": \"https://a.example/a\", \"vcodec\": \"none\", \"acodec\": \"mp4a\", "
        "\"http_headers\": {\"User-Agent\": \"ua2\"}}]}";
    yt::StreamInfo info;
    std::string err;
    CHECK(yt::ParseStreamInfo(json, &info, &err));
    CHECK(!info.muxed);
    CHECK(info.video_url == "https://v.example/v");
    CHECK(info.audio_url == "https://a.example/a");
    CHECK(info.audio_headers == "User-Agent: ua2\r\n");
    CHECK(info.width == 854 && info.height == 480 && info.fps == 24.0);

    CHECK(!yt::ParseStreamInfo("not json", &info, &err));
    CHECK(!err.empty());
    CHECK(!yt::ParseStreamInfo("{\"id\": \"nothing\"}", &info, &err));
}

void TestCanonicalUrl() {
    CHECK(yt::CanonicalVideoUrl("aqz-KE-bpKQ") == "https://www.youtube.com/watch?v=aqz-KE-bpKQ");
    CHECK(yt::CanonicalVideoUrl("  aqz-KE-bpKQ\n") == "https://www.youtube.com/watch?v=aqz-KE-bpKQ");
    CHECK(yt::CanonicalVideoUrl("https://youtu.be/aqz-KE-bpKQ") == "https://youtu.be/aqz-KE-bpKQ");
    CHECK(yt::CanonicalVideoUrl("https://www.youtube.com/watch?v=aqz-KE-bpKQ&t=10") ==
          "https://www.youtube.com/watch?v=aqz-KE-bpKQ&t=10");
    CHECK(yt::CanonicalVideoUrl("not an id!!") == "not an id!!");
}

void TestArgv() {
    std::vector<std::string> s = yt::SearchArgv("lofi beats", 15);
    CHECK(s.front() == "yt-dlp");
    CHECK(s.back() == "ytsearch15:lofi beats");
    CHECK(Has(s, "--flat-playlist") && Has(s, "-j"));
    CHECK(yt::SearchArgv("x", 500).back() == "ytsearch50:x");

    std::vector<std::string> r = yt::ResolveArgv("https://www.youtube.com/watch?v=abc", "android", 480);
    CHECK(r.front() == "yt-dlp");
    CHECK(r.back() == "https://www.youtube.com/watch?v=abc");
    int ea = IndexOf(r, "--extractor-args");
    CHECK(ea >= 0 && r[static_cast<size_t>(ea) + 1] == "youtube:player_client=android");
    int f = IndexOf(r, "-f");
    CHECK(f >= 0 && r[static_cast<size_t>(f) + 1].find("height<=480") != std::string::npos);
    CHECK(!Has(yt::ResolveArgv("u", "", 360), "--extractor-args"));

    yt::StreamInfo info;
    info.video_url = "https://v";
    info.audio_url = "https://a";
    info.video_headers = "User-Agent: ua\r\n";
    info.audio_headers = "User-Agent: ua\r\n";
    std::vector<std::string> v = yt::VideoDecodeArgv(info, 12.5, 640, 360, 30.0);
    CHECK(v.front() == "ffmpeg");
    CHECK(v.back() == "pipe:1");
    int ss = IndexOf(v, "-ss");
    CHECK(ss >= 0 && v[static_cast<size_t>(ss) + 1] == "12.500");
    int hdr = IndexOf(v, "-headers");
    CHECK(hdr >= 0 && v[static_cast<size_t>(hdr) + 1] == "User-Agent: ua\r\n");
    int i = IndexOf(v, "-i");
    CHECK(i >= 0 && v[static_cast<size_t>(i) + 1] == "https://v");
    CHECK(Has(v, "rgba") && Has(v, "rawvideo") && Has(v, "scale=640:360") && Has(v, "-an"));
    // -ss must precede -i (input seeking: a byte-range request, not a decode-and-discard).
    CHECK(ss < i);
    CHECK(!Has(yt::VideoDecodeArgv(info, 0.0, 640, 360, 30.0), "-ss"));

    std::vector<std::string> a = yt::AudioDecodeArgv(info, 0.0, 2, 48000);
    CHECK(Has(a, "s16le") && Has(a, "-vn") && Has(a, "48000") && Has(a, "-ac"));
    int ai = IndexOf(a, "-i");
    CHECK(ai >= 0 && a[static_cast<size_t>(ai) + 1] == "https://a");
}

void TestFrameAssembler() {
    yt::FrameAssembler fa(4);
    CHECK(fa.Ready() == 0);
    const char bytes[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    CHECK(fa.Push(bytes, 3) == 0);  // partial
    CHECK(fa.Push(bytes + 3, 3) == 1);  // completes frame 1, starts frame 2
    CHECK(fa.Push(bytes + 6, 4) == 2);  // completes frame 2, 2 bytes of frame 3
    std::vector<uint8_t> f;
    CHECK(fa.Pop(&f));
    CHECK(f.size() == 4 && f[0] == 1 && f[3] == 4);
    CHECK(fa.Pop(&f));
    CHECK(f[0] == 5 && f[3] == 8);
    CHECK(!fa.Pop(&f));
    CHECK(fa.Push(bytes + 8, 2) == 1);  // 9,10 + the queued 9,10 tail -> frame 3
    CHECK(fa.Pop(&f));
    CHECK(f[0] == 9 && f[1] == 10 && f[2] == 9 && f[3] == 10);
    // One huge chunk holding many frames.
    std::vector<char> big(4 * 100, 'x');
    CHECK(fa.Push(big.data(), big.size()) == 100);
    fa.DropAll();
    CHECK(fa.Ready() == 0);
    fa.Reset(0);
    CHECK(fa.Push(big.data(), big.size()) == 0);  // no frame size: nothing assembled
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
    TestSearchLine();
    TestStreamInfoMuxed();
    TestStreamInfoSplit();
    TestCanonicalUrl();
    TestArgv();
    TestFrameAssembler();
    TestFormatting();
    std::printf("youtube_player_test: all checks passed\n");
    return 0;
}
