// The index (dircache): v2/v3/v4 read and write, stat comparison, worktree
// hashing, and building trees from it.

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <system_error>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "git/git.h"
#include "git/sha1.h"
#include "git/util.h"

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mep::git {

namespace {
uint32_t Be32(const unsigned char *p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
uint16_t Be16(const unsigned char *p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
void Put32(std::string &s, uint32_t v) {
    s.push_back(static_cast<char>(v >> 24));
    s.push_back(static_cast<char>(v >> 16));
    s.push_back(static_cast<char>(v >> 8));
    s.push_back(static_cast<char>(v));
}
void Put16(std::string &s, uint16_t v) {
    s.push_back(static_cast<char>(v >> 8));
    s.push_back(static_cast<char>(v));
}

int CompareEntry(std::string_view a_path, int a_stage, std::string_view b_path, int b_stage) {
    int c = a_path.compare(b_path);
    if (c) return c;
    return a_stage - b_stage;
}
}  // namespace

bool Index::Load(const std::string &path, std::string *err) {
    entries.clear();
    version = 2;
    loaded_from_file = false;
    std::string data;
    if (!ReadFile(path, data)) return true;  // no index yet
    FileStat st;
    if (LStat(path, st)) {
        file_mtime_s = st.mtime_s;
        file_mtime_ns = st.mtime_ns;
    }
    const unsigned char *d = reinterpret_cast<const unsigned char *>(data.data());
    size_t n = data.size();
    if (n < 12 + 20 || std::memcmp(d, "DIRC", 4) != 0) {
        if (err) *err = "index file corrupt";
        return false;
    }
    version = Be32(d + 4);
    if (version < 2 || version > 4) {
        if (err) *err = "index file has unsupported version " + std::to_string(version);
        return false;
    }
    {
        Sha1 h;
        h.Update(d, n - 20);
        auto sum = h.Final();
        bool zero = true;
        for (size_t i = 0; i < 20; i++)
            if (d[n - 20 + i]) zero = false;
        if (!zero && std::memcmp(sum.data(), d + n - 20, 20) != 0) {
            if (err) *err = "index file corrupt (bad checksum)";
            return false;
        }
    }
    uint32_t count = Be32(d + 8);
    size_t pos = 12;
    const size_t end = n - 20;
    std::string prev;
    entries.reserve(count);
    for (uint32_t k = 0; k < count; k++) {
        if (pos + 62 > end) {
            if (err) *err = "index file corrupt (truncated)";
            return false;
        }
        IndexEntry e;
        const unsigned char *p = d + pos;
        e.ctime_s = Be32(p);
        e.ctime_ns = Be32(p + 4);
        e.mtime_s = Be32(p + 8);
        e.mtime_ns = Be32(p + 12);
        e.dev = Be32(p + 16);
        e.ino = Be32(p + 20);
        e.mode = Be32(p + 24);
        e.uid = Be32(p + 28);
        e.gid = Be32(p + 32);
        e.size = Be32(p + 36);
        std::memcpy(e.oid.b.data(), p + 40, 20);
        e.flags = Be16(p + 60);
        size_t hdr = 62;
        if (e.flags & 0x4000) {
            if (version < 3) {
                if (err) *err = "index file corrupt (extended flag in v2)";
                return false;
            }
            e.flags_ext = Be16(p + 62);
            hdr = 64;
        }
        size_t name_start = pos + hdr;
        if (version == 4) {
            size_t i = name_start;
            unsigned char c = d[i++];
            uint64_t strip = c & 127;
            while (c & 128) {
                strip += 1;
                c = d[i++];
                strip = (strip << 7) + (c & 127);
            }
            const void *nul = std::memchr(d + i, 0, end - i);
            if (!nul || strip > prev.size()) {
                if (err) *err = "index file corrupt (name)";
                return false;
            }
            size_t len = static_cast<size_t>(static_cast<const unsigned char *>(nul) - (d + i));
            e.path = prev.substr(0, prev.size() - static_cast<size_t>(strip)) +
                     std::string(reinterpret_cast<const char *>(d + i), len);
            pos = i + len + 1;
        } else {
            const void *nul = std::memchr(d + name_start, 0, end - name_start);
            if (!nul) {
                if (err) *err = "index file corrupt (name)";
                return false;
            }
            size_t len = static_cast<size_t>(static_cast<const unsigned char *>(nul) - (d + name_start));
            e.path.assign(reinterpret_cast<const char *>(d + name_start), len);
            size_t entry_len = (hdr + len + 8) & ~static_cast<size_t>(7);
            pos += entry_len;
        }
        prev = e.path;
        entries.push_back(std::move(e));
    }
    // Extensions: optional ones (uppercase signature) are dropped on write;
    // mandatory ones (split index "link", sparse "sdir") we can't honour.
    while (pos + 8 <= end) {
        const unsigned char *sig = d + pos;
        uint32_t sz = Be32(d + pos + 4);
        if (sig[0] < 'A' || sig[0] > 'Z') {
            if (err) *err = "index uses unsupported extension '" + std::string(reinterpret_cast<const char *>(sig), 4) + "'";
            return false;
        }
        pos += 8 + sz;
    }
    loaded_from_file = true;
    return true;
}

bool Index::Save(const std::string &path, std::string *err) {
    // Racy-git is handled on the read side, as git does: an entry whose
    // mtime is not older than the index file itself is content-checked
    // (Status), never trusted on stat data alone.
    bool need_v3 = false;
    for (const IndexEntry &e : entries)
        if (e.flags_ext) need_v3 = true;
    uint32_t ver = version;
    if (ver < 3 && need_v3) ver = 3;
    if (ver == 3 && !need_v3) ver = 2;
    std::string out = "DIRC";
    Put32(out, ver);
    Put32(out, static_cast<uint32_t>(entries.size()));
    std::string prev;
    for (const IndexEntry &e : entries) {
        size_t start = out.size();
        Put32(out, e.ctime_s);
        Put32(out, e.ctime_ns);
        Put32(out, e.mtime_s);
        Put32(out, e.mtime_ns);
        Put32(out, e.dev);
        Put32(out, e.ino);
        Put32(out, e.mode);
        Put32(out, e.uid);
        Put32(out, e.gid);
        Put32(out, e.size);
        out.append(reinterpret_cast<const char *>(e.oid.b.data()), 20);
        uint16_t flags = static_cast<uint16_t>(e.flags & 0xb000);  // assume-valid, stage
        if (e.flags_ext && ver >= 3) flags |= 0x4000;
        flags |= static_cast<uint16_t>(std::min<size_t>(e.path.size(), 0xfff));
        Put16(out, flags);
        if (flags & 0x4000) Put16(out, e.flags_ext);
        if (ver == 4) {
            size_t common = 0;
            while (common < prev.size() && common < e.path.size() && prev[common] == e.path[common]) common++;
            uint64_t strip = prev.size() - common;
            // git's encode_varint
            unsigned char varint[16];
            int pos = 15;
            varint[pos] = static_cast<unsigned char>(strip & 127);
            while (strip >>= 7) varint[--pos] = static_cast<unsigned char>(128 | (--strip & 127));
            out.append(reinterpret_cast<const char *>(varint + pos), static_cast<size_t>(16 - pos));
            out += e.path.substr(common);
            out.push_back('\0');
        } else {
            out += e.path;
            size_t len = out.size() - start;
            size_t padded = (len + 8) & ~static_cast<size_t>(7);
            out.append(padded - len, '\0');
        }
        prev = e.path;
    }
    Sha1 h;
    h.Update(out);
    auto sum = h.Final();
    out.append(reinterpret_cast<const char *>(sum.data()), 20);
    if (!WriteFileAtomic(path, out, err)) return false;
    version = ver;
    FileStat st;
    if (LStat(path, st)) {
        file_mtime_s = st.mtime_s;
        file_mtime_ns = st.mtime_ns;
    }
    return true;
}

size_t Index::LowerBound(std::string_view path) const {
    size_t lo = 0, hi = entries.size();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (CompareEntry(entries[mid].path, entries[mid].Stage(), path, 0) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

long Index::Find(std::string_view path, int stage) const {
    size_t i = LowerBound(path);
    for (; i < entries.size() && entries[i].path == path; i++)
        if (entries[i].Stage() == stage) return static_cast<long>(i);
    return -1;
}

void Index::Put(IndexEntry e) {
    e.SetStage(0);
    size_t i = LowerBound(e.path);
    size_t j = i;
    while (j < entries.size() && entries[j].path == e.path) j++;
    if (j > i) entries.erase(entries.begin() + static_cast<long>(i), entries.begin() + static_cast<long>(j));
    entries.insert(entries.begin() + static_cast<long>(i), std::move(e));
}

bool Index::Remove(std::string_view path) {
    size_t i = LowerBound(path);
    size_t j = i;
    while (j < entries.size() && entries[j].path == path) j++;
    if (j == i) return false;
    entries.erase(entries.begin() + static_cast<long>(i), entries.begin() + static_cast<long>(j));
    return true;
}

bool Index::HasConflicts() const {
    for (const IndexEntry &e : entries)
        if (e.Stage()) return true;
    return false;
}

// ------------------------------------------------------------ stat

bool LStat(const std::string &path, FileStat &st) {
    st = FileStat{};
#if !defined(_WIN32)
    struct stat s;
    if (::lstat(path.c_str(), &s) != 0) return false;
    st.exists = true;
    st.is_dir = S_ISDIR(s.st_mode);
    st.is_reg = S_ISREG(s.st_mode);
    st.is_link = S_ISLNK(s.st_mode);
    st.exec = (s.st_mode & S_IXUSR) != 0;
#if defined(__APPLE__)
    st.ctime_s = static_cast<uint32_t>(s.st_ctimespec.tv_sec);
    st.ctime_ns = static_cast<uint32_t>(s.st_ctimespec.tv_nsec);
    st.mtime_s = static_cast<uint32_t>(s.st_mtimespec.tv_sec);
    st.mtime_ns = static_cast<uint32_t>(s.st_mtimespec.tv_nsec);
#else
    st.ctime_s = static_cast<uint32_t>(s.st_ctim.tv_sec);
    st.ctime_ns = static_cast<uint32_t>(s.st_ctim.tv_nsec);
    st.mtime_s = static_cast<uint32_t>(s.st_mtim.tv_sec);
    st.mtime_ns = static_cast<uint32_t>(s.st_mtim.tv_nsec);
#endif
    st.dev = static_cast<uint32_t>(s.st_dev);
    st.ino = static_cast<uint32_t>(s.st_ino);
    st.uid = static_cast<uint32_t>(s.st_uid);
    st.gid = static_cast<uint32_t>(s.st_gid);
    st.full_size = static_cast<uint64_t>(s.st_size);
    st.size = static_cast<uint32_t>(s.st_size);
    return true;
#else
    std::error_code ec;
    auto status = std::filesystem::symlink_status(path, ec);
    if (ec || !std::filesystem::exists(status)) return false;
    st.exists = true;
    st.is_dir = std::filesystem::is_directory(status);
    st.is_reg = std::filesystem::is_regular_file(status);
    st.is_link = std::filesystem::is_symlink(status);
    if (st.is_reg) {
        st.full_size = std::filesystem::file_size(path, ec);
        st.size = static_cast<uint32_t>(st.full_size);
        auto t = std::filesystem::last_write_time(path, ec);
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
        st.mtime_s = static_cast<uint32_t>(ns / 1000000000);
        st.mtime_ns = static_cast<uint32_t>(ns % 1000000000);
    }
    return true;
#endif
}

uint32_t ModeFromStat(const FileStat &st, const Config &cfg, uint32_t existing_mode) {
    if (st.is_link) return 0120000;
    if (st.is_dir) return 0160000;
    bool trust_exec = cfg.GetBool("core.filemode", true);
    if (!trust_exec) {
        if ((existing_mode & 0170000) == 0100000) return existing_mode;
        return 0100644;
    }
    return st.exec ? 0100755 : 0100644;
}

void FillStat(IndexEntry &e, const FileStat &st) {
    e.ctime_s = st.ctime_s;
    e.ctime_ns = st.ctime_ns;
    e.mtime_s = st.mtime_s;
    e.mtime_ns = st.mtime_ns;
    e.dev = st.dev;
    e.ino = st.ino;
    e.uid = st.uid;
    e.gid = st.gid;
    e.size = st.size;
}

bool StatMatches(const IndexEntry &e, const FileStat &st, const Config &cfg) {
    uint32_t type = e.mode & 0170000;
    if (type == 0100000 && !st.is_reg) return false;
    if (type == 0120000 && !st.is_link) return false;
    if (type == 0100000 && cfg.GetBool("core.filemode", true)) {
        bool exec = (e.mode & 0111) != 0;
        if (exec != st.exec) return false;
    }
    if (e.mtime_s != st.mtime_s) return false;
    if (e.mtime_ns && e.mtime_ns != st.mtime_ns) return false;
    std::string check = cfg.Get("core.checkstat").value_or("default");
    if (cfg.GetBool("core.trustctime", true) && check != "minimal") {
        if (e.ctime_s != st.ctime_s) return false;
        if (e.ctime_ns && e.ctime_ns != st.ctime_ns) return false;
    }
    if (check != "minimal") {
        if (e.ino != st.ino || e.uid != st.uid || e.gid != st.gid) return false;
    }
    if (e.size != st.size) return false;
    return true;
}

bool ReadWorktreeFile(const std::string &abs, const FileStat &st, std::string &out) {
#if !defined(_WIN32)
    if (st.is_link) {
        std::string buf(4096, '\0');
        ssize_t n = ::readlink(abs.c_str(), buf.data(), buf.size());
        if (n < 0) return false;
        buf.resize(static_cast<size_t>(n));
        out = std::move(buf);
        return true;
    }
#endif
    (void)st;
    return ReadFile(abs, out);
}

bool HashWorktreeFile(const std::string &abs, const FileStat &st, Oid &out, std::string *content) {
    std::string data;
    if (!ReadWorktreeFile(abs, st, data)) return false;
    out = HashObject(ObjType::Blob, data);
    if (content) *content = std::move(data);
    return true;
}

// ------------------------------------------------------------ trees

namespace {
bool BuildTree(Repo &repo, const std::vector<const IndexEntry *> &ents, size_t lo, size_t hi, size_t prefix_len,
               Oid &out, std::string *err) {
    std::vector<TreeEntry> tree;
    size_t i = lo;
    while (i < hi) {
        const IndexEntry *e = ents[i];
        std::string_view rest = std::string_view(e->path).substr(prefix_len);
        size_t slash = rest.find('/');
        if (slash == std::string_view::npos) {
            tree.push_back(TreeEntry{e->mode, std::string(rest), e->oid});
            i++;
            continue;
        }
        std::string dir(rest.substr(0, slash));
        size_t j = i;
        std::string dir_prefix = e->path.substr(0, prefix_len + slash + 1);
        while (j < hi && StartsWith(ents[j]->path, dir_prefix)) j++;
        Oid sub;
        if (!BuildTree(repo, ents, i, j, dir_prefix.size(), sub, err)) return false;
        tree.push_back(TreeEntry{040000, dir, sub});
        i = j;
    }
    return repo.Write(ObjType::Tree, SerializeTree(std::move(tree)), out, err);
}
}  // namespace

bool WriteTreeFromIndex(Repo &repo, const Index &idx, Oid &out, std::string *err) {
    std::vector<const IndexEntry *> ents;
    for (const IndexEntry &e : idx.entries) {
        if (e.Stage() != 0) {
            if (err) *err = "error: '" + e.path + "' is unmerged";
            return false;
        }
        if (e.IntentToAdd()) continue;
        ents.push_back(&e);
    }
    return BuildTree(repo, ents, 0, ents.size(), 0, out, err);
}

bool StageContent(Repo &repo, const std::string &rel, const std::string &content, std::string *err) {
    Index idx;
    if (!idx.Load(repo.IndexFile(), err)) return false;
    long i = idx.Find(rel);
    IndexEntry e;
    if (i >= 0) e = idx.entries[static_cast<size_t>(i)];
    else {
        e.path = rel;
        e.mode = 0100644;
    }
    Oid blob;
    if (!repo.Write(ObjType::Blob, content, blob, err)) return false;
    e.oid = blob;
    // No stat data: the worktree file differs from this content, so the
    // next status must hash it rather than trust a stat match.
    e.ctime_s = e.ctime_ns = e.mtime_s = e.mtime_ns = e.dev = e.ino = e.uid = e.gid = 0;
    e.size = static_cast<uint32_t>(content.size());
    idx.Put(e);
    return idx.Save(repo.IndexFile(), err);
}

}  // namespace mep::git
