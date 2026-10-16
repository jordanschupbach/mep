#include "tex/tex_resolver.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace meptex {

namespace fs = std::filesystem;

namespace {

bool IsFile(const std::string &p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

bool EndsWith(const std::string &s, const std::string &suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Ranks a path under a tree's tex/ for the order TeX Live's TEXINPUTS for
// LaTeX searches: tex/latex, tex/generic, tex/plain, then everything else.
int TexRank(const std::string &rel) {
    if (rel.rfind("tex/latex/", 0) == 0) return 0;
    if (rel.rfind("tex/generic/", 0) == 0) return 1;
    if (rel.rfind("tex/plain/", 0) == 0) return 2;
    return 3;
}

}  // namespace

void Resolver::add_dir(const std::string &dir) { dirs_.push_back(dir); }

void Resolver::add_tree(const std::string &root) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return;
    struct Hit {
        int rank;
        std::string rel, path;
    };
    std::unordered_map<std::string, std::vector<Hit>> tex, tfm, fmt, other;
    auto add = [&](const std::string &rel, const std::string &base, const std::string &path) {
        if (rel.rfind("tex/", 0) == 0) {
            tex[base].push_back({TexRank(rel), rel, path});
        } else if (rel.rfind("fonts/tfm/", 0) == 0) {
            tfm[base].push_back({0, rel, path});
        } else if (EndsWith(base, ".fmt")) {
            fmt[base].push_back({0, rel, path});
        } else if (rel.rfind("fonts/", 0) == 0) {
            other[base].push_back({0, rel, path});
        }
    };
    // The tree's ls-R (kpathsea's file database) lists every file, so the
    // tree need not be walked: `./dir:` lines, then that directory's names.
    std::ifstream ls(root + "/ls-R");
    std::string line;
    if (ls && std::getline(ls, line) && line.rfind("% ls-R", 0) == 0) {
        std::string dir;
        bool skip = true;
        while (std::getline(ls, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (line.back() == ':') {
                dir = line.substr(0, line.size() - 1);
                if (dir.rfind("./", 0) == 0) dir = dir.substr(2);
                else if (dir == ".") dir.clear();
                else if (dir.rfind(root, 0) == 0) dir = dir.substr(std::min(dir.size(), root.size() + 1));
                // only the subtrees the engine reads from
                skip = !(dir.rfind("tex/", 0) == 0 || dir.rfind("fonts/", 0) == 0 || dir.rfind("web2c", 0) == 0);
                continue;
            }
            if (skip || line[0] == '%') continue;
            const std::string rel = dir.empty() ? line : dir + "/" + line;
            if (line.find('.') == std::string::npos) continue;  // a directory (TeX never looks up a name without one)
            add(rel, line, root + "/" + rel);
        }
    } else {
        for (fs::recursive_directory_iterator it(root, fs::directory_options::follow_directory_symlink, ec), end;
             it != end; it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            add(it->path().lexically_relative(root).generic_string(), it->path().filename().string(),
                it->path().string());
        }
    }
    auto merge = [](std::unordered_map<std::string, std::vector<Hit>> &from,
                    std::unordered_map<std::string, std::vector<std::string>> &into) {
        for (auto &[base, hits] : from) {
            std::sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) {
                return a.rank != b.rank ? a.rank < b.rank : a.rel < b.rel;
            });
            std::vector<std::string> &dst = into[base];
            for (const Hit &h : hits) dst.push_back(h.path);
        }
    };
    merge(tex, tex_);
    merge(tfm, tfm_);
    merge(fmt, fmt_);
    merge(other, other_);
}

std::string Resolver::find(const std::string &name, int kind) const {
    if (name.empty()) return "";
    std::vector<std::string> candidates;
    if (kind == kTexFile && !EndsWith(name, ".tex")) candidates.push_back(name + ".tex");
    candidates.push_back(name);
    const auto &table = kind == kTexFile ? tex_ : kind == kTfmFile ? tfm_ : kind == kFmtFile ? fmt_ : other_;
    for (const std::string &c : candidates) {
        if (c.find('/') != std::string::npos) {
            // A path: as given, relative to the working directory.
            if (IsFile(c)) return c;
            continue;
        }
        for (const std::string &d : dirs_) {
            const std::string p = (d.empty() || d == ".") ? c : d + "/" + c;
            if (IsFile(p)) return (d.empty() || d == ".") ? "./" + c : p;
        }
        auto it = table.find(c);
        if (it != table.end() && !it->second.empty()) return it->second.front();
        if (kind == kOtherFile) {
            auto jt = tex_.find(c);
            if (jt != tex_.end() && !jt->second.empty()) return jt->second.front();
        }
    }
    return "";
}

}  // namespace meptex
