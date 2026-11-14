// Finding the source of USEd modules for mep's Fortran language server. See
// fortran_lsp_modules.h.
#include "fortran_lsp_modules.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "fortran_parse.h"

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

std::string DirOf(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    return slash == 0 ? "/" : path.substr(0, slash);
}

std::string Join(const std::string &dir, const std::string &name) {
    if (dir.empty()) return name;
    return dir.back() == '/' ? dir + name : dir + "/" + name;
}

// Build output and tool directories the workspace index does not descend into.
bool SkipDir(const std::string &name) {
    if (name.empty() || name[0] == '.') return true;
    static const char *const kSkip[] = {"node_modules", "CMakeFiles", "_build", "_deps", "__pycache__",
                                        "target", "bin", "obj", "venv"};
    for (const char *s : kSkip) {
        if (name == s) return true;
    }
    return name.compare(0, 5, "build") == 0 || name.compare(0, 11, "cmake-build") == 0;
}

// -I dirs (`-Idir` or `-I dir`) of a compiler flags string.
void IncludeDirs(const char *flags, std::vector<std::string> *out) {
    if (!flags) return;
    std::istringstream in(flags);
    std::string tok;
    bool next = false;
    while (in >> tok) {
        if (next) {
            out->push_back(tok);
            next = false;
        } else if (tok == "-I") {
            next = true;
        } else if (tok.compare(0, 2, "-I") == 0) {
            out->push_back(tok.substr(2));
        }
    }
}

class IndexSource : public FortranLspModuleSource {
public:
    IndexSource(FortranModuleIndex *index, std::string from) : index_(index), from_(std::move(from)) {}
    std::shared_ptr<const FortranLspDocument> Module(const std::string &name) override {
        return index_->Find(name, from_);
    }
    std::vector<std::string> ModuleNames() override { return index_->Names(from_); }

private:
    FortranModuleIndex *index_;
    std::string from_;
};

}  // namespace

bool FortranSourceName(const std::string &name) {
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    const std::string ext = Lower(name.substr(dot + 1));
    static const char *const kExt[] = {"f90", "f95", "f03", "f08", "f18", "f", "for", "ftn", "fpp", "f77"};
    for (const char *e : kExt) {
        if (ext == e) return true;
    }
    return false;
}

void FortranModuleIndex::SetRoot(const std::string &root) {
    root_ = root;
    workspace_.clear();
    workspace_paths_.clear();
    workspace_files_ = 0;
    if (root.empty()) return;
    size_t budget = 20000;
    std::vector<std::string> stack = {root};
    std::vector<std::string> files;
    while (!stack.empty() && budget > 0) {
        const std::string dir = stack.back();
        stack.pop_back();
        std::error_code ec;
        std::vector<std::string> subdirs;
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            std::error_code fec;
            const bool is_dir = it->is_directory(fec);
            if (fec) continue;
            if (is_dir) {
                if (!SkipDir(name)) subdirs.push_back(Join(dir, name));
                continue;
            }
            if (!FortranSourceName(name) || budget == 0) continue;
            const uintmax_t size = it->file_size(fec);
            if (fec || size > 64u * 1024 * 1024) continue;
            budget--;
            files.push_back(Join(dir, name));
        }
        std::sort(subdirs.rbegin(), subdirs.rend());
        for (const std::string &s : subdirs) stack.push_back(s);
    }
    std::sort(files.begin(), files.end());
    for (const std::string &f : files) {
        workspace_paths_.insert(f);
        workspace_files_++;
        for (const std::string &m : ModulesOfFile(f)) workspace_.emplace(m, f);  // the first file wins
    }
}

void FortranModuleIndex::SetOpen(const std::string &path, std::shared_ptr<const FortranLspDocument> doc) {
    open_[path] = std::move(doc);
}

void FortranModuleIndex::Close(const std::string &path) { open_.erase(path); }

void FortranModuleIndex::Refresh(const std::string &path) {
    const bool in_root = !root_.empty() && path.compare(0, root_.size(), root_) == 0;
    if (!in_root && !workspace_paths_.count(path)) return;
    for (auto it = workspace_.begin(); it != workspace_.end();) {
        it = it->second == path ? workspace_.erase(it) : std::next(it);
    }
    workspace_paths_.insert(path);
    for (const std::string &m : ModulesOfFile(path)) workspace_.emplace(m, path);
}

const std::vector<std::string> &FortranModuleIndex::ModulesOfFile(const std::string &path) {
    static const std::vector<std::string> kNone;
    int64_t mtime = 0;
    uint64_t size = 0;
    if (!Stat(path, &mtime, &size)) return kNone;
    FileModules &fm = file_modules_[path];
    if (fm.mtime == mtime && fm.size == size && (mtime != 0 || size != 0)) return fm.names;
    std::string text;
    fm.names.clear();
    if (ReadAll(path, &text)) fm.names = FortranLspScanModuleNames(text);
    fm.mtime = mtime;
    fm.size = size;
    return fm.names;
}

bool FortranModuleIndex::Defines(const std::shared_ptr<const FortranLspDocument> &doc, const std::string &module) const {
    if (!doc) return false;
    const std::vector<std::string> mods = FortranLspModulesDefined(*doc);
    return std::find(mods.begin(), mods.end(), module) != mods.end();
}

std::shared_ptr<const FortranLspDocument> FortranModuleIndex::Load(const std::string &path) {
    auto open = open_.find(path);
    if (open != open_.end()) return open->second;
    int64_t mtime = 0;
    uint64_t size = 0;
    if (!Stat(path, &mtime, &size)) return nullptr;
    auto it = parsed_.find(path);
    if (it != parsed_.end() && it->second.mtime == mtime && it->second.size == size) return it->second.doc;
    if (loading_.count(path)) return nullptr;  // a USE cycle
    std::string text;
    if (!ReadAll(path, &text)) return nullptr;
    loading_.insert(path);
    std::shared_ptr<const FortranLspDocument> doc = Analyze(fortran::SplitLines(fortran::DecodeSource(text)), path);
    loading_.erase(path);
    parsed_[path] = Parsed{mtime, size, doc};
    return doc;
}

std::vector<std::string> FortranModuleIndex::SearchDirs(const std::string &from_path) const {
    std::vector<std::string> dirs;
    if (const char *p = std::getenv("MEP_FORTRAN_PATH")) {
        std::string s = p;
        size_t start = 0;
        for (size_t i = 0; i <= s.size(); i++) {
            if (i == s.size() || s[i] == ':') {
                if (i > start) dirs.push_back(s.substr(start, i - start));
                start = i + 1;
            }
        }
    }
    std::vector<std::string> inc;
    IncludeDirs(std::getenv("FFLAGS"), &inc);
    IncludeDirs(std::getenv("FCFLAGS"), &inc);
    for (std::string &d : inc) {
        if (!d.empty() && d[0] != '/' && !root_.empty()) d = Join(root_, d);  // relative to the workspace
        dirs.push_back(d);
    }
    if (!from_path.empty()) dirs.push_back(DirOf(from_path));
    std::vector<std::string> out;
    for (const std::string &d : dirs) {
        if (std::find(out.begin(), out.end(), d) == out.end()) out.push_back(d);
    }
    return out;
}

std::shared_ptr<const FortranLspDocument> FortranModuleIndex::Find(const std::string &module_name,
                                                                   const std::string &from_path) {
    const std::string module = Lower(module_name);
    for (const auto &o : open_) {
        if (o.first != from_path && Defines(o.second, module)) return o.second;
    }
    auto ws = workspace_.find(module);
    if (ws != workspace_.end() && ws->second != from_path) {
        auto doc = Load(ws->second);
        if (Defines(doc, module)) return doc;
    }
    for (const std::string &dir : SearchDirs(from_path)) {
        std::error_code ec;
        std::vector<std::string> files;
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (FortranSourceName(name)) files.push_back(Join(dir, name));
        }
        std::sort(files.begin(), files.end());
        for (const std::string &f : files) {
            if (f == from_path) continue;
            const std::vector<std::string> &names = ModulesOfFile(f);
            if (std::find(names.begin(), names.end(), module) == names.end()) continue;
            auto doc = Load(f);
            if (Defines(doc, module)) return doc;
        }
    }
    return nullptr;
}

std::vector<std::string> FortranModuleIndex::Names(const std::string &from_path) {
    std::set<std::string> names;
    for (const auto &w : workspace_) names.insert(w.first);
    for (const auto &o : open_) {
        if (o.second) {
            for (const std::string &m : FortranLspModulesDefined(*o.second)) names.insert(m);
        }
    }
    for (const std::string &dir : SearchDirs(from_path)) {
        std::error_code ec;
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (!FortranSourceName(name)) continue;
            for (const std::string &m : ModulesOfFile(Join(dir, name))) names.insert(m);
        }
    }
    return std::vector<std::string>(names.begin(), names.end());
}

bool FortranModuleIndex::Stale(const FortranLspDocument &doc) {
    const std::string &path = FortranLspPath(doc);
    for (const auto &imp : FortranLspImports(doc)) {
        auto now = Find(imp.first, path);
        if (now != imp.second || (now && Stale(*now))) return true;
    }
    return false;
}

std::shared_ptr<const FortranLspDocument> FortranModuleIndex::Analyze(const std::vector<std::string> &lines,
                                                                      const std::string &path) {
    IndexSource source(this, path);
    return FortranLspAnalyze(lines, path, &source);
}
