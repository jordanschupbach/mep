#include "git/odb.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <cstring>
#include <string>
#include <vector>

#include "deflate.h"
#include "git/util.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mep::git {

namespace {
uint32_t Be32(const unsigned char *p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
constexpr size_t kCacheLimit = 64u << 20;
}  // namespace

MappedFile::~MappedFile() {
#if !defined(_WIN32)
    if (mapped_ && data_) ::munmap(const_cast<unsigned char *>(data_), size_);
#endif
}

bool MappedFile::Open(const std::string &path) {
#if !defined(_WIN32)
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return false;
    }
    size_ = static_cast<size_t>(st.st_size);
    if (size_ == 0) {
        ::close(fd);
        data_ = reinterpret_cast<const unsigned char *>("");
        return true;
    }
    void *p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) return false;
    data_ = static_cast<const unsigned char *>(p);
    mapped_ = true;
    return true;
#else
    if (!ReadFile(path, fallback_)) return false;
    data_ = reinterpret_cast<const unsigned char *>(fallback_.data());
    size_ = fallback_.size();
    return true;
#endif
}

// ------------------------------------------------------------ packs

bool Pack::Open(const std::string &idx_path) {
    if (!idx_.Open(idx_path)) return false;
    const unsigned char *d = idx_.data();
    size_t n = idx_.size();
    if (n < 8 + 256 * 4) return false;
    pack_path = idx_path.substr(0, idx_path.size() - 4) + ".pack";
    if (d[0] == 0xff && d[1] == 't' && d[2] == 'O' && d[3] == 'c') {
        if (Be32(d + 4) != 2) return false;
        version_ = 2;
        fanout_ = d + 8;
        count_ = Be32(fanout_ + 255 * 4);
        names_ = fanout_ + 256 * 4;
        size_t need = 8 + 256 * 4 + static_cast<size_t>(count_) * (20 + 4 + 4);
        if (n < need) return false;
        offsets32_ = names_ + static_cast<size_t>(count_) * 24;
        offsets64_ = offsets32_ + static_cast<size_t>(count_) * 4;
    } else {
        version_ = 1;
        fanout_ = d;
        count_ = Be32(fanout_ + 255 * 4);
        if (n < 256 * 4 + static_cast<size_t>(count_) * 24) return false;
        names_ = d + 256 * 4;  // entries of (4-byte offset, 20-byte name)
    }
    return true;
}

Oid Pack::NameAt(uint32_t i) const {
    Oid o;
    const unsigned char *p = version_ == 2 ? names_ + static_cast<size_t>(i) * 20 : names_ + static_cast<size_t>(i) * 24 + 4;
    std::memcpy(o.b.data(), p, 20);
    return o;
}

uint64_t Pack::OffsetAt(uint32_t i) const {
    if (version_ == 1) return Be32(names_ + static_cast<size_t>(i) * 24);
    uint32_t off = Be32(offsets32_ + static_cast<size_t>(i) * 4);
    if (!(off & 0x80000000u)) return off;
    const unsigned char *p = offsets64_ + static_cast<size_t>(off & 0x7fffffffu) * 8;
    return (static_cast<uint64_t>(Be32(p)) << 32) | Be32(p + 4);
}

uint32_t Pack::LowerBound(const uint8_t *prefix, size_t nbytes) const {
    uint32_t lo = prefix[0] ? Be32(fanout_ + (static_cast<size_t>(prefix[0]) - 1) * 4) : 0;
    uint32_t hi = Be32(fanout_ + static_cast<size_t>(prefix[0]) * 4);
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const unsigned char *name =
            version_ == 2 ? names_ + static_cast<size_t>(mid) * 20 : names_ + static_cast<size_t>(mid) * 24 + 4;
        if (std::memcmp(name, prefix, nbytes) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

long Pack::Find(const Oid &oid) const {
    uint32_t i = LowerBound(oid.b.data(), 20);
    if (i < count_ && NameAt(i) == oid) return static_cast<long>(i);
    return -1;
}

bool Pack::EnsurePack() {
    if (pack_) return true;
    if (pack_failed_) return false;
    auto m = std::make_unique<MappedFile>();
    if (!m->Open(pack_path) || m->size() < 12 || std::memcmp(m->data(), "PACK", 4) != 0) {
        pack_failed_ = true;
        return false;
    }
    pack_ = std::move(m);
    return true;
}

namespace {
bool ApplyDelta(const std::string &base, const unsigned char *delta, size_t len, std::string &out) {
    size_t i = 0;
    auto varint = [&](uint64_t &v) {
        v = 0;
        int shift = 0;
        while (i < len) {
            unsigned char c = delta[i++];
            v |= static_cast<uint64_t>(c & 0x7f) << shift;
            shift += 7;
            if (!(c & 0x80)) return true;
        }
        return false;
    };
    uint64_t src_size, dst_size;
    if (!varint(src_size) || !varint(dst_size)) return false;
    if (src_size != base.size()) return false;
    out.clear();
    out.reserve(static_cast<size_t>(dst_size));
    while (i < len) {
        unsigned char op = delta[i++];
        if (op & 0x80) {
            uint64_t off = 0, size = 0;
            for (int b = 0; b < 4; b++)
                if (op & (1 << b)) {
                    if (i >= len) return false;
                    off |= static_cast<uint64_t>(delta[i++]) << (8 * b);
                }
            for (int b = 0; b < 3; b++)
                if (op & (0x10 << b)) {
                    if (i >= len) return false;
                    size |= static_cast<uint64_t>(delta[i++]) << (8 * b);
                }
            if (size == 0) size = 0x10000;
            if (off + size > base.size()) return false;
            out.append(base, static_cast<size_t>(off), static_cast<size_t>(size));
        } else if (op) {
            if (i + op > len) return false;
            out.append(reinterpret_cast<const char *>(delta + i), op);
            i += op;
        } else {
            return false;
        }
    }
    return out.size() == dst_size;
}
}  // namespace

bool Pack::ReadAt(uint64_t offset, ObjType &type, std::string &data, Odb &odb, int depth) {
    if (depth > 10000 || !EnsurePack()) return false;
    auto cit = cache_.find(offset);
    if (cit != cache_.end()) {
        type = cit->second.type;
        data = cit->second.data;
        return true;
    }
    const unsigned char *p = pack_->data();
    size_t n = pack_->size();
    if (offset >= n) return false;
    size_t i = static_cast<size_t>(offset);
    unsigned char c = p[i++];
    int t = (c >> 4) & 7;
    uint64_t size = c & 15;
    int shift = 4;
    while (c & 0x80) {
        if (i >= n) return false;
        c = p[i++];
        size |= static_cast<uint64_t>(c & 0x7f) << shift;
        shift += 7;
    }
    std::string inflated;
    if (t >= 1 && t <= 4) {
        if (!deflate::InflateZlib(p + i, n - i, inflated)) return false;
        if (inflated.size() != size) return false;
        type = static_cast<ObjType>(t);
        data = std::move(inflated);
    } else if (t == 6 || t == 7) {
        ObjType base_type;
        std::string base;
        if (t == 6) {
            if (i >= n) return false;
            c = p[i++];
            uint64_t off = c & 127;
            while (c & 128) {
                if (i >= n) return false;
                off += 1;
                c = p[i++];
                off = (off << 7) + (c & 127);
            }
            if (off > offset) return false;
            if (!ReadAt(offset - off, base_type, base, odb, depth + 1)) return false;
        } else {
            if (i + 20 > n) return false;
            Oid base_oid;
            std::memcpy(base_oid.b.data(), p + i, 20);
            i += 20;
            long pos = Find(base_oid);
            if (pos >= 0) {
                if (!ReadAt(OffsetAt(static_cast<uint32_t>(pos)), base_type, base, odb, depth + 1)) return false;
            } else if (!odb.Read(base_oid, base_type, base)) {
                return false;
            }
        }
        if (!deflate::InflateZlib(p + i, n - i, inflated)) return false;
        if (inflated.size() != size) return false;
        if (!ApplyDelta(base, reinterpret_cast<const unsigned char *>(inflated.data()), inflated.size(), data))
            return false;
        type = base_type;
    } else {
        return false;
    }
    // Cache trees and commits (and small blobs): delta chains revisit them.
    if (type != ObjType::Blob || data.size() < 64 * 1024) {
        if (cache_bytes_ + data.size() > kCacheLimit) {
            cache_.clear();
            cache_bytes_ = 0;
        }
        cache_bytes_ += data.size();
        cache_[offset] = Cached{type, data};
    }
    return true;
}

// ------------------------------------------------------------ odb

Odb::Odb(const std::string &objects_dir) { dirs_.push_back(objects_dir); }

void Odb::Reload() {
    loaded_ = false;
    packs_.clear();
    loose_names_.clear();
    dirs_.resize(1);
}

void Odb::EnsureLoaded() {
    if (loaded_) return;
    loaded_ = true;
    // Alternates (one level is plenty in practice; git allows nesting).
    std::string alt;
    if (ReadFile(dirs_[0] + "/info/alternates", alt)) {
        size_t pos = 0;
        while (pos < alt.size()) {
            size_t nl = alt.find('\n', pos);
            if (nl == std::string::npos) nl = alt.size();
            std::string line = Trim(alt.substr(pos, nl - pos));
            pos = nl + 1;
            if (line.empty() || line[0] == '#') continue;
            dirs_.push_back(AbsolutePath(line, dirs_[0]));
        }
    }
    for (const std::string &dir : dirs_) {
        std::vector<DirEntry> ents;
        ListDir(dir + "/pack", ents);
        std::vector<std::string> names;
        for (const DirEntry &e : ents)
            if (EndsWith(e.name, ".idx")) names.push_back(e.name);
        std::sort(names.begin(), names.end());
        for (const std::string &name : names) {
            auto p = std::make_unique<Pack>();
            if (p->Open(dir + "/pack/" + name) && FileExists(p->pack_path)) packs_.push_back(std::move(p));
        }
    }
}

bool Odb::ReadLoose(const std::string &dir, const Oid &oid, ObjType &type, std::string &data) {
    std::string hex = oid.Hex();
    std::string raw;
    if (!ReadFile(dir + "/" + hex.substr(0, 2) + "/" + hex.substr(2), raw)) return false;
    std::string inflated;
    if (!deflate::InflateZlib(reinterpret_cast<const unsigned char *>(raw.data()), raw.size(), inflated)) return false;
    size_t sp = inflated.find(' ');
    size_t nul = inflated.find('\0');
    if (sp == std::string::npos || nul == std::string::npos || sp > nul) return false;
    type = TypeFromName(std::string_view(inflated).substr(0, sp));
    if (type == ObjType::None) return false;
    data = inflated.substr(nul + 1);
    return true;
}

bool Odb::Read(const Oid &oid, ObjType &type, std::string &data) {
    EnsureLoaded();
    for (auto &p : packs_) {
        long i = p->Find(oid);
        if (i >= 0 && p->ReadAt(p->OffsetAt(static_cast<uint32_t>(i)), type, data, *this)) return true;
    }
    for (const std::string &dir : dirs_)
        if (ReadLoose(dir, oid, type, data)) return true;
    // A pack may have appeared since (a concurrent gc/repack).
    size_t before = packs_.size();
    Reload();
    EnsureLoaded();
    if (packs_.size() != before) {
        for (auto &p : packs_) {
            long i = p->Find(oid);
            if (i >= 0 && p->ReadAt(p->OffsetAt(static_cast<uint32_t>(i)), type, data, *this)) return true;
        }
    }
    return false;
}

bool Odb::Has(const Oid &oid) {
    EnsureLoaded();
    for (auto &p : packs_)
        if (p->Find(oid) >= 0) return true;
    std::string hex = oid.Hex();
    for (const std::string &dir : dirs_)
        if (FileExists(dir + "/" + hex.substr(0, 2) + "/" + hex.substr(2))) return true;
    return false;
}

bool Odb::WriteLoose(ObjType type, std::string_view data, Oid &out, std::string *err) {
    out = HashObject(type, data);
    if (Has(out)) return true;
    std::string hex = out.Hex();
    loose_names_.erase(dirs_[0] + "/" + hex.substr(0, 2));
    std::string dir = dirs_[0] + "/" + hex.substr(0, 2);
    MakeDirs(dir);
    std::string raw = std::string(TypeName(type)) + " " + std::to_string(data.size());
    raw.push_back('\0');
    raw.append(data);
    std::string z = deflate::DeflateZlib(reinterpret_cast<const unsigned char *>(raw.data()), raw.size());
    static std::atomic<unsigned> counter{0};
    std::string tmp = dir + "/tmp_obj_mep_" + std::to_string(counter++) + "_" + hex.substr(2, 8);
    if (!WriteFile(tmp, z)) {
        if (err) *err = "unable to write object " + hex;
        return false;
    }
#if !defined(_WIN32)
    ::chmod(tmp.c_str(), 0444);
    if (::rename(tmp.c_str(), (dir + "/" + hex.substr(2)).c_str()) != 0) {
        ::unlink(tmp.c_str());
        if (!Has(out)) {
            if (err) *err = "unable to write object " + hex;
            return false;
        }
    }
#else
    std::error_code ec;
    std::filesystem::rename(tmp, dir + "/" + hex.substr(2), ec);
#endif
    return true;
}

uint64_t Odb::ApproxCount() {
    EnsureLoaded();
    uint64_t n = 0;
    for (auto &p : packs_) n += p->Count();
    return n;
}

void Odb::FindPrefix(std::string_view hex, std::vector<Oid> &out, size_t limit) {
    EnsureLoaded();
    out.clear();
    if (hex.size() < 2) return;
    uint8_t prefix[20] = {0};
    size_t nbytes = hex.size() / 2;
    for (size_t i = 0; i < nbytes; i++) {
        std::string byte(hex.substr(2 * i, 2));
        prefix[i] = static_cast<uint8_t>(std::strtoul(byte.c_str(), nullptr, 16));
    }
    auto matches = [&](const Oid &o) { return StartsWith(o.Hex(), hex); };
    auto add = [&](const Oid &o) {
        if (std::find(out.begin(), out.end(), o) == out.end()) out.push_back(o);
    };
    std::string byte_prefix(hex.substr(0, nbytes * 2));
    for (auto &p : packs_) {
        for (uint32_t i = p->LowerBound(prefix, nbytes); i < p->Count() && out.size() < limit; i++) {
            Oid o = p->NameAt(i);
            if (!StartsWith(o.Hex(), byte_prefix)) break;
            if (matches(o)) add(o);
        }
    }
    for (const std::string &dir : dirs_) {
        std::vector<DirEntry> ents;
        ListDir(dir + "/" + std::string(hex.substr(0, 2)), ents);
        for (const DirEntry &e : ents) {
            if (e.name.size() != 38) continue;
            Oid o;
            if (Oid::FromHex(std::string(hex.substr(0, 2)) + e.name, o) && matches(o)) add(o);
        }
    }
}

int Odb::MaxCommonPrefix(const Oid &oid) {
    EnsureLoaded();
    std::string hex = oid.Hex();
    int best = 0;
    auto common = [&](const Oid &other) {
        if (other == oid) return;
        std::string h = other.Hex();
        int k = 0;
        while (k < 40 && h[static_cast<size_t>(k)] == hex[static_cast<size_t>(k)]) k++;
        best = std::max(best, k);
    };
    for (auto &p : packs_) {
        uint32_t i = p->LowerBound(oid.b.data(), 20);
        if (i < p->Count()) common(p->NameAt(i));
        if (i + 1 < p->Count()) common(p->NameAt(i + 1));
        if (i > 0) common(p->NameAt(i - 1));
    }
    for (const std::string &dir : dirs_) {
        std::string key = dir + "/" + hex.substr(0, 2);
        auto it = loose_names_.find(key);
        if (it == loose_names_.end()) {
            std::vector<DirEntry> ents;
            ListDir(key, ents);
            std::vector<Oid> names;
            for (const DirEntry &e : ents) {
                Oid o;
                if (e.name.size() == 38 && Oid::FromHex(hex.substr(0, 2) + e.name, o)) names.push_back(o);
            }
            it = loose_names_.emplace(key, std::move(names)).first;
        }
        for (const Oid &o : it->second) common(o);
    }
    return best;
}

}  // namespace mep::git
