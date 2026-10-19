// mep-bench: the leaf libraries every interactive path leans on -- the
// in-house regex engine (/, :s, project search), inflate/deflate (PNG,
// zip-based office files, git objects), the PNG and JPEG codecs (every
// inline image), JSON (every LSP and agent message) and the spell checker
// (run over every visible word each frame).

#include "bench_util.h"
#include "bench/fixtures.h"

#include "deflate.h"
#include "jpeg_codec.h"
#include "json.h"
#include "png_codec.h"
#include "regex.h"
#include "spell.h"

#include <cstdlib>
#include <string>
#include <vector>

using mep::bench::DoNotOptimize;
using mep::bench::Measure;

namespace {

const std::string &SourceText() {
    // src/editor.cpp: ~1.9 MB of real code, the haystack for every regex scenario.
    static const std::string text = mep::bench::ReadFileOrEmpty(mep::bench::RepoPath("src/editor.cpp"));
    return text;
}

int CountMatches(const mep_regex::Regex &re, const std::string &text) {
    int count = 0;
    int from = 0;
    while (from <= static_cast<int>(text.size())) {
        mep_regex::Match m = re.Search(text, from);
        if (!m.ok()) break;
        count++;
        from = m.end > m.start ? m.end : m.start + 1;
    }
    return count;
}

// Line-at-a-time is how / and :s actually call it (one Search per buffer line).
int CountMatchesPerLine(const mep_regex::Regex &re, const std::vector<std::string> &lines) {
    int count = 0;
    for (const std::string &l : lines)
        if (re.Search(l).ok()) count++;
    return count;
}

}  // namespace

MEP_BENCH("core/regex") {
    const std::string &text = SourceText();
    const std::vector<std::string> lines = mep::bench::SplitLines(text);
    const long long kb = static_cast<long long>(text.size() / 1024);
    struct Case {
        const char *name;
        const char *pattern;
        bool ignore_case = false;  // :set ignorecase (the engine has no inline (?i))
    };
    const Case cases[] = {
        {"literal", "PushUndo"},
        {"literal_rare", "ShiftDecorationsForLineEdit"},
        {"word_boundary", "\\bstatus_message_\\b"},
        {"class_star", "[A-Z][a-z]+[A-Z][A-Za-z]*\\("},
        {"alternation", "TODO|FIXME|XXX|HACK"},
        {"dot_star_anchor", "^\\s*//.*plan"},
        {"case_insensitive", "buffer", true},
    };
    for (const Case &c : cases) {
        mep_regex::Regex re(c.pattern, c.ignore_case);
        if (!re.ok()) {
            std::fprintf(stderr, "core/regex: pattern %s did not compile: %s\n", c.pattern, re.error().c_str());
            continue;
        }
        Measure(std::string("whole_text_") + c.name, kb, [&] { DoNotOptimize(CountMatches(re, text)); });
        Measure(std::string("per_line_") + c.name, static_cast<long long>(lines.size()),
                [&] { DoNotOptimize(CountMatchesPerLine(re, lines)); });
    }
    Measure("compile_x1000", 1000, [] {
        for (int i = 0; i < 1000; i++) {
            mep_regex::Regex re("\\b(foo|bar)[0-9]+\\s*=\\s*\"[^\"]*\"");
            DoNotOptimize(re.ok());
        }
    });
}

MEP_BENCH("core/deflate") {
    const std::string &text = SourceText();
    const auto *bytes = reinterpret_cast<const unsigned char *>(text.data());
    const long long kb = static_cast<long long>(text.size() / 1024);
    const std::string packed = deflate::DeflateZlib(bytes, text.size());
    Measure("inflate_source_1.9mb", kb, [&] {
        std::string out;
        deflate::InflateZlib(reinterpret_cast<const unsigned char *>(packed.data()), packed.size(), out);
        DoNotOptimize(out.size());
    });
    Measure("deflate_source_1.9mb", kb, [&] { DoNotOptimize(deflate::DeflateZlib(bytes, text.size()).size()); });
    Measure("crc32_1.9mb", kb, [&] { DoNotOptimize(deflate::Crc32(0, bytes, text.size())); });
}

namespace {

// A photo-like 1920x1080 RGB image: smooth gradients plus a little noise,
// so neither codec gets a degenerate (all-flat or all-noise) input.
std::vector<unsigned char> SyntheticImage(int w, int h) {
    std::vector<unsigned char> px(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
    mep::bench::Rng rng;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 3;
            const int noise = static_cast<int>(rng.Below(9)) - 4;
            px[i] = static_cast<unsigned char>(std::clamp((x * 255) / w + noise, 0, 255));
            px[i + 1] = static_cast<unsigned char>(std::clamp((y * 255) / h + noise, 0, 255));
            px[i + 2] = static_cast<unsigned char>(std::clamp(((x ^ y) & 0xff) / 2 + 64 + noise, 0, 255));
        }
    }
    return px;
}

}  // namespace

MEP_BENCH("core/image") {
    const int w = 1920;
    const int h = 1080;
    const std::vector<unsigned char> px = SyntheticImage(w, h);
    const long long mpix = 2;  // ~2.07 megapixels; n reads as "images" x 2
    const std::string png_bytes = png::Encode(w, h, 3, px.data(), w * 3);
    const std::string jpg_bytes = jpeg::Encode(w, h, 3, px.data(), w * 3, 85);
    Measure("png_decode_1080p", mpix, [&] {
        int ow = 0;
        int oh = 0;
        std::string err;
        unsigned char *out = png::Decode(reinterpret_cast<const unsigned char *>(png_bytes.data()), png_bytes.size(),
                                         &ow, &oh, &err);
        DoNotOptimize(out);
        std::free(out);
    });
    Measure("png_encode_1080p", mpix, [&] { DoNotOptimize(png::Encode(w, h, 3, px.data(), w * 3).size()); });
    Measure("jpeg_decode_1080p", mpix, [&] {
        int ow = 0;
        int oh = 0;
        std::string err;
        unsigned char *out = jpeg::Decode(reinterpret_cast<const unsigned char *>(jpg_bytes.data()), jpg_bytes.size(),
                                          &ow, &oh, &err);
        DoNotOptimize(out);
        std::free(out);
    });
    Measure("jpeg_encode_1080p_q85", mpix, [&] { DoNotOptimize(jpeg::Encode(w, h, 3, px.data(), w * 3, 85).size()); });
}

MEP_BENCH("core/json") {
    // An LSP-shaped payload: a completion list of 5000 items.
    Json items = Json::Array();
    for (int i = 0; i < 5000; i++) {
        Json item = Json::Object();
        item["label"] = "completion_item_" + std::to_string(i);
        item["kind"] = i % 25;
        item["detail"] = "std::vector<std::string> (const std::string &text, int flags)";
        item["sortText"] = std::to_string(100000 + i);
        Json edit = Json::Object();
        Json range = Json::Object();
        Json start = Json::Object();
        start["line"] = i;
        start["character"] = 4;
        range["start"] = start;
        range["end"] = start;
        edit["range"] = range;
        edit["newText"] = "completion_item_" + std::to_string(i) + "()";
        item["textEdit"] = edit;
        items.push_back(item);
    }
    Json root = Json::Object();
    root["jsonrpc"] = "2.0";
    root["id"] = 7;
    Json result = Json::Object();
    result["isIncomplete"] = false;
    result["items"] = items;
    root["result"] = result;
    const std::string text = root.dump();
    const long long kb = static_cast<long long>(text.size() / 1024);
    Measure("parse_completion_5000", kb, [&] {
        Json out;
        Json::Parse(text, &out);
        DoNotOptimize(out.size());
    });
    Measure("dump_completion_5000", kb, [&] { DoNotOptimize(root.dump().size()); });
}

MEP_BENCH("core/spell") {
    spell::SpellChecker checker;
    const std::string dict = mep::bench::RepoPath("assets/spell/en_US.words");
    if (!checker.LoadDictionary(dict)) {
        std::fprintf(stderr, "core/spell: no dictionary at %s, skipped\n", dict.c_str());
        return;
    }
    Measure("load_dictionary", 1, [&] {
        spell::SpellChecker c;
        c.LoadDictionary(dict);
        DoNotOptimize(c.ready());
    });
    // One screenful of prose (60 rows), checked word by word the way the
    // per-frame SpellBad pass does.
    std::vector<std::string> words;
    for (const std::string &l : mep::bench::ProseLines(60)) {
        size_t i = 0;
        while (i < l.size()) {
            size_t j = l.find(' ', i);
            if (j == std::string::npos) j = l.size();
            if (j > i) words.push_back(l.substr(i, j - i));
            i = j + 1;
        }
    }
    Measure("check_screenful", static_cast<long long>(words.size()), [&] {
        int bad = 0;
        for (const std::string &w : words) bad += checker.IsMisspelled(w) ? 1 : 0;
        DoNotOptimize(bad);
    });
    Measure("suggest_x20", 20, [&] {
        for (int i = 0; i < 20; i++) DoNotOptimize(checker.Suggest(i % 2 ? "occasionaly" : "speling").size());
    });
}
