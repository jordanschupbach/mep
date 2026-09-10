// ANIMATION_VIDEO_PLAN.md Phase 3 coverage for mov_container.h:
// WriteMovFile -> OpenMovFile -> ReadMovFrameJpeg round-tripped against
// synthetic JPEG frames from jpeg_codec.h's Encode (already covered on
// its own by jpeg_encoder_test.cpp), confirming frame count/dimensions/
// fps and an exact byte-for-byte match on every frame's stored bytes,
// plus that each stored frame still decodes. Opt-in sample dump (pass a
// directory as argv[1]) so a real .mov can be inspected/played outside
// mep, matching mep-jpeg-codec-test's own shape.

#include "mov_container.h"

#include "jpeg_codec.h"

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

// A small solid-color frame, distinguishable per index by its red
// channel -- enough to confirm frames come back in the right order with
// the right bytes, without needing a full image comparison.
std::string MakeFrame(int width, int height, int index) {
    std::vector<unsigned char> px(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    unsigned char r = static_cast<unsigned char>((index * 37) % 256);
    for (size_t i = 0; i < px.size(); i += 4) {
        px[i + 0] = r;
        px[i + 1] = static_cast<unsigned char>(index * 5);
        px[i + 2] = 128;
        px[i + 3] = 255;
    }
    std::string jpeg = jpeg::Encode(width, height, 4, px.data(), width * 4, 85);
    CHECK(!jpeg.empty());
    return jpeg;
}
}  // namespace

int main(int argc, char **argv) {
    const bool dump_sample = argc > 1;
    const std::string out_path = dump_sample ? (std::string(argv[1]) + "/sample.mov") : "/tmp/mov_container_test_tmp.mov";

    const int width = 64, height = 48;
    const int num_frames = 12;
    const int fps_num = 24, fps_den = 1;
    std::vector<std::string> frames;
    for (int i = 0; i < num_frames; i++) frames.push_back(MakeFrame(width, height, i));

    std::string error;
    CHECK(mov::WriteMovFile(out_path, width, height, fps_num, fps_den, frames, &error));

    mov::MovFile mf;
    CHECK(mov::OpenMovFile(out_path, &mf, &error));
    CHECK(mf.width == width);
    CHECK(mf.height == height);
    CHECK(mf.fps > 23.9 && mf.fps < 24.1);
    CHECK(static_cast<int>(mf.frame_index.size()) == num_frames);

    for (int i = 0; i < num_frames; i++) {
        std::vector<unsigned char> got = mov::ReadMovFrameJpeg(out_path, mf, i);
        const std::string &want = frames[static_cast<size_t>(i)];
        CHECK(got.size() == want.size());
        // Compare as unsigned bytes on both sides -- std::string's `char`
        // is signed on this platform, so comparing it directly against
        // `unsigned char` promotes 0x80-0xFF bytes (which JPEG data is
        // full of, starting with the 0xFF SOI marker at byte 0) to
        // different signed-vs-unsigned int values and spuriously fails
        // std::equal even on byte-identical data.
        CHECK(std::memcmp(got.data(), want.data(), got.size()) == 0);

        int dw = 0, dh = 0;
        std::string decode_error;
        unsigned char *decoded = jpeg::Decode(got.data(), got.size(), &dw, &dh, &decode_error);
        CHECK(decoded != nullptr);
        CHECK(dw == width);
        CHECK(dh == height);
        std::free(decoded);
    }

    // Out-of-range frame index -> empty, not a crash.
    CHECK(mov::ReadMovFrameJpeg(out_path, mf, -1).empty());
    CHECK(mov::ReadMovFrameJpeg(out_path, mf, num_frames).empty());

    // Malformed/missing file -> OpenMovFile fails cleanly.
    mov::MovFile bad;
    CHECK(!mov::OpenMovFile("/nonexistent/path/does_not_exist.mov", &bad, &error));

    if (!dump_sample) std::remove(out_path.c_str());

    std::printf("mov_container_test: all checks passed\n");
    return 0;
}
