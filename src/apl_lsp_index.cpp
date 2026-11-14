// The workspace index of mep's APL language server: see apl_lsp_index.h.
#include "apl_lsp_index.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>

namespace {

std::string Lower(std::string s) {
    for (char &c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

bool Stat(const std::string &path, int64_t *mtime, uint64_t *size) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    if (ec) return false;
    const auto n = std::filesystem::file_size(path, ec);
    if (ec) return false;
    *mtime = static_cast<int64_t>(t.time_since_epoch().count());
    *size = static_cast<uint64_t>(n);
    return true;
}

bool ReadAll(const std::string &path, std::string *out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return true;
}

bool SkipDir(const std::string &name) {
    if (name.empty() || name[0] == '.') return true;
    static const char *const kSkip[] = {"node_modules", "CMakeFiles", "_build", "_deps", "__pycache__",
                                        "target", "obj", "venv", "result"};
    for (const char *s : kSkip) {
        if (name == s) return true;
    }
    return name.compare(0, 5, "build") == 0 || name.compare(0, 11, "cmake-build") == 0;
}

}  // namespace

bool AplSourceName(const std::string &name) {
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    const std::string ext = Lower(name.substr(dot + 1));
    static const char *const kExt[] = {"apl", "apls", "aplf", "aplo", "apln", "aplc", "apli", "dyalog"};
    for (const char *e : kExt) {
        if (ext == e) return true;
    }
    return false;
}

std::vector<std::string> AplSplitLines(const std::string &text) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            lines.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty() && cur.back() == '\r') cur.pop_back();
    lines.push_back(cur);
    return lines;
}

void AplWorkspaceIndex::Scan(const std::string &dir, size_t *budget, int depth) {
    std::error_code ec;
    std::vector<std::string> subdirs;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (*budget == 0) return;
        (*budget)--;
        const std::string name = it->path().filename().string();
        std::error_code ec2;
        if (it->is_directory(ec2)) {
            if (!SkipDir(name) && depth < 12) subdirs.push_back(it->path().string());
        } else if (AplSourceName(name)) {
            files_.push_back(it->path().string());
        }
    }
    std::sort(subdirs.begin(), subdirs.end());
    for (const std::string &s : subdirs) Scan(s, budget, depth + 1);
}

void AplWorkspaceIndex::SetRoot(const std::string &root) {
    root_ = root;
    files_.clear();
    size_t budget = 50000;
    if (!root.empty()) Scan(root, &budget, 0);
    if (const char *env = std::getenv("MEP_APL_PATH")) {
        std::string s = env;
        size_t p = 0;
        while (p <= s.size()) {
            size_t e = s.find(':', p);
            if (e == std::string::npos) e = s.size();
            const std::string dir = s.substr(p, e - p);
            p = e + 1;
            if (dir.empty()) continue;
            std::error_code ec;
            if (std::filesystem::is_regular_file(dir, ec)) {
                files_.push_back(dir);
            } else {
                size_t b = 20000;
                Scan(dir, &b, 0);
            }
        }
    }
    std::set<std::string> seen;
    std::vector<std::string> unique;
    for (const std::string &f : files_) {
        if (seen.insert(f).second) unique.push_back(f);
    }
    files_ = unique;
}

void AplWorkspaceIndex::SetOpen(const std::string &path, std::shared_ptr<const AplLspDocument> doc) {
    open_[path] = std::move(doc);
}

void AplWorkspaceIndex::Close(const std::string &path) { open_.erase(path); }

void AplWorkspaceIndex::Refresh(const std::string &path) {
    parsed_.erase(path);
    if (AplSourceName(path) && std::find(files_.begin(), files_.end(), path) == files_.end() && !root_.empty() &&
        path.compare(0, root_.size(), root_) == 0) {
        files_.push_back(path);
    }
}

std::shared_ptr<const AplLspDocument> AplWorkspaceIndex::Load(const std::string &path) {
    int64_t mtime = 0;
    uint64_t size = 0;
    if (!Stat(path, &mtime, &size)) return nullptr;
    auto it = parsed_.find(path);
    if (it != parsed_.end() && it->second.mtime == mtime && it->second.size == size) return it->second.doc;
    std::string text;
    if (size > 64u * 1024u * 1024u || !ReadAll(path, &text)) return nullptr;
    Parsed p;
    p.mtime = mtime;
    p.size = size;
    p.doc = AplLspAnalyze(AplSplitLines(text), path);
    parsed_[path] = p;
    return p.doc;
}

AplLspWorkspace AplWorkspaceIndex::Workspace(const std::string &exclude_path) {
    AplLspWorkspace ws;
    std::set<std::string> done;
    for (const auto &kv : open_) {
        if (kv.first == exclude_path) continue;
        done.insert(kv.first);
        if (kv.second) ws.docs.push_back(kv.second);
    }
    for (const std::string &f : files_) {
        if (f == exclude_path || done.count(f)) continue;
        if (auto d = Load(f)) ws.docs.push_back(d);
    }
    return ws;
}
