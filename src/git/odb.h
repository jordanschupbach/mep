#pragma once

// The object database: loose objects and packfiles (.idx v1/v2, OFS/REF
// deltas), plus alternates. Internal to mep::git (Repo owns one).

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "git/git.h"

namespace mep::git {

class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile();
    MappedFile(const MappedFile &) = delete;
    MappedFile &operator=(const MappedFile &) = delete;
    bool Open(const std::string &path);
    const unsigned char *data() const { return data_; }
    size_t size() const { return size_; }

private:
    const unsigned char *data_ = nullptr;
    size_t size_ = 0;
    bool mapped_ = false;
    std::string fallback_;
};

class Pack {
public:
    bool Open(const std::string &idx_path);
    uint32_t Count() const { return count_; }
    // Position of `oid` in the index, or -1.
    long Find(const Oid &oid) const;
    // First index position whose name is >= the (partial) prefix bytes.
    uint32_t LowerBound(const uint8_t *prefix, size_t nbytes) const;
    Oid NameAt(uint32_t i) const;
    uint64_t OffsetAt(uint32_t i) const;
    bool ReadAt(uint64_t offset, ObjType &type, std::string &data, class Odb &odb, int depth = 0);

    std::string pack_path;

private:
    bool EnsurePack();
    MappedFile idx_;
    std::unique_ptr<MappedFile> pack_;
    bool pack_failed_ = false;
    int version_ = 2;
    uint32_t count_ = 0;
    const unsigned char *fanout_ = nullptr;
    const unsigned char *names_ = nullptr;
    const unsigned char *offsets32_ = nullptr;
    const unsigned char *offsets64_ = nullptr;
    // Delta-base cache: offset -> (type, data).
    struct Cached {
        ObjType type;
        std::string data;
    };
    std::unordered_map<uint64_t, Cached> cache_;
    size_t cache_bytes_ = 0;
};

class Odb {
public:
    explicit Odb(const std::string &objects_dir);
    bool Read(const Oid &oid, ObjType &type, std::string &data);
    bool Has(const Oid &oid);
    bool WriteLoose(ObjType type, std::string_view data, Oid &out, std::string *err);
    uint64_t ApproxCount();
    // All object ids starting with the hex prefix (stops after `limit`).
    void FindPrefix(std::string_view hex, std::vector<Oid> &out, size_t limit = 2);
    // Longest common hex-prefix length between `oid` and any *other* object.
    int MaxCommonPrefix(const Oid &oid);
    void Reload();

private:
    void EnsureLoaded();
    bool ReadLoose(const std::string &dir, const Oid &oid, ObjType &type, std::string &data);
    std::vector<std::string> dirs_;  // objects dir + alternates
    std::vector<std::unique_ptr<Pack>> packs_;
    std::unordered_map<std::string, std::vector<Oid>> loose_names_;  // "<dir>/xx" -> ids (abbrev checks)
    bool loaded_ = false;
};

}  // namespace mep::git
