#pragma once

// Inputs for mep-bench (plans/PERFORMANCE_PLAN.md): real files from this
// checkout where one exists (src/editor.cpp is a 40k-line C++ file,
// README.org a real Org document), grown or synthesized to a fixed size
// where a benchmark needs a size the repo does not have. Everything is
// deterministic, so two runs of one commit time the same work.

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef MEP_SOURCE_DIR
#define MEP_SOURCE_DIR "."
#endif

namespace mep::bench {

/** @brief An absolute path to `rel` inside the source checkout this binary was built from. */
inline std::string RepoPath(const std::string &rel) { return std::string(MEP_SOURCE_DIR) + "/" + rel; }

inline std::string ReadFileOrEmpty(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

inline std::vector<std::string> SplitLines(const std::string &text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) nl = text.size();
        lines.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    return lines;
}

inline std::string JoinLines(const std::vector<std::string> &lines) {
    std::string s;
    size_t total = 0;
    for (const std::string &l : lines) total += l.size() + 1;
    s.reserve(total);
    for (const std::string &l : lines) {
        s += l;
        s += '\n';
    }
    return s;
}

/** @brief `lines` repeated (whole copies, then a prefix) until exactly `count` lines. */
inline std::vector<std::string> GrowLines(const std::vector<std::string> &lines, size_t count) {
    std::vector<std::string> out;
    out.reserve(count);
    while (out.size() < count && !lines.empty())
        for (size_t i = 0; i < lines.size() && out.size() < count; i++) out.push_back(lines[i]);
    return out;
}

/** @brief A tiny deterministic PRNG (xorshift), so fixtures never depend on the platform's <random>. */
struct Rng {
    uint64_t s = 0x9E3779B97F4A7C15ull;
    uint64_t Next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    size_t Below(size_t n) { return n ? static_cast<size_t>(Next() % n) : 0; }
};

/** @brief Plain English-like prose (dictionary words, sentences, paragraphs), `lines` lines of ~70 columns. */
inline std::vector<std::string> ProseLines(size_t lines) {
    static const char *kWords[] = {"the", "editor", "buffer", "renders", "every", "frame", "while", "typing",
                                   "quickly", "through", "large", "documents", "with", "syntax", "highlighting",
                                   "and", "folds", "search", "results", "appear", "instantly", "under", "cursor",
                                   "performance", "matters", "because", "latency", "is", "felt", "by", "people",
                                   "who", "write", "code", "notes", "papers", "a", "of", "in", "on", "speling",
                                   "mistakes", "occasionaly", "creep", "into", "text"};
    Rng rng;
    std::vector<std::string> out;
    out.reserve(lines);
    for (size_t i = 0; i < lines; i++) {
        if (i % 8 == 7) {
            out.emplace_back();
            continue;
        }
        std::string l;
        while (l.size() < 68) {
            if (!l.empty()) l += ' ';
            l += kWords[rng.Below(sizeof(kWords) / sizeof(kWords[0]))];
        }
        out.push_back(l);
    }
    return out;
}

}  // namespace mep::bench
