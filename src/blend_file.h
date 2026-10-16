#pragma once

// Low-level reader for Blender's .blend file format: file header, file
// blocks ("BHead"s), and the SDNA catalogue every .blend carries
// describing its own struct layouts. Reading fields *by name* through
// SDNA is what makes one reader work across Blender versions: field
// offsets, struct sizes and even which fields exist change from release
// to release, but the names of the parts we need mostly do not.
//
// Supported containers: uncompressed, gzip (Blender < 3.0 "Compress"),
// and zstd (Blender >= 3.0 "Compress", via zstd_decode.h). Supported
// headers: the classic 12-byte "BLENDER_v248" / "BLENDER-V279" form
// (4- or 8-byte pointers, little or big endian) and the 17-byte
// "BLENDER17-01v0500" form Blender 5.0 introduced (always little-endian,
// 8-byte pointers, 64-bit block lengths).
//
// The scene-level interpretation (objects, meshes, materials) lives in
// blend_scene.h; nothing here knows what a mesh is.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace blend {

struct Field {
    std::string type;  // SDNA type name: "float", "Object", "ListBase", ...
    std::string name;  // bare identifier: "loc", "mat", "parentinv"
    std::string decl;  // SDNA declaration: "*mat", "loc[3]", "parentinv[4][4]", "(*func)()"
    size_t offset = 0;
    size_t size = 0;       // total bytes
    size_t elem_size = 0;  // bytes per element (the pointer size for pointers)
    size_t count = 1;      // product of the array dimensions
    int pointer_depth = 0;
    bool is_float = false;   // float or double, not a pointer
    int struct_index = -1;   // for an embedded (non-pointer) struct field
};

struct Struct {
    std::string name;
    size_t size = 0;
    std::vector<Field> fields;
    std::unordered_map<std::string, size_t> index;  // bare name -> fields[]
    bool is_id = false;  // starts with `ID id` (Object, Mesh, Material, ...)

    const Field *Find(std::string_view field) const;
};

struct Block {
    char code[5] = {0, 0, 0, 0, 0};  // "OB\0\0", "ME\0\0", "DATA", "DNA1", ...
    uint32_t sdna = 0;               // struct index of the block's contents
    uint64_t old = 0;                // address the data had when it was written
    size_t offset = 0;               // byte offset of the data in File::bytes()
    size_t len = 0;                  // bytes of data
    size_t nr = 0;                   // number of struct instances
    // Index (into File::blocks()) of the ID block this block belongs to:
    // DATA blocks belong to the nearest preceding non-DATA block, which
    // is the ID (Object, Mesh, ...) whose data they hold. Other blocks
    // are their own group.
    size_t group = 0;

    bool Is(const char *c) const;  // compares the code, e.g. Is("OB")
};

class File {
public:
    // Reads `path`, decompressing gzip/zstd containers, then parses it.
    bool Load(const std::string &path, std::string *error);
    // Same, for bytes already in memory (compressed or not).
    bool Parse(std::string bytes, std::string *error);

    int version() const { return version_; }            // 248, 279, 405, 500, ...
    int format_version() const { return format_; }      // 0 classic header, 1 for "BLENDER17-01"
    size_t pointer_size() const { return ptr_size_; }
    bool big_endian() const { return big_endian_; }
    const std::string &compression() const { return compression_; }  // "", "gzip" or "zstd"

    const std::vector<Block> &blocks() const { return blocks_; }
    const std::vector<Struct> &structs() const { return structs_; }
    const Struct *FindStruct(std::string_view name) const;
    const Struct *StructOf(const Block &b) const;

    // Block whose data contains old address `ptr` (normally starts at it),
    // or nullptr. `*inner` (when given) receives ptr - block.old.
    //
    // Old addresses are only unique per ID: Blender resolves a pointer to
    // DATA among the DATA blocks of the ID being read, and a pointer to
    // another ID among the ID blocks (Blender 5.x writes deterministic
    // per-ID addresses that really do repeat between IDs). So lookups
    // take the group (Block::group) the pointer was read from, and try,
    // in order: that group's blocks, the non-DATA (ID) blocks, then any
    // block at all (older writers used real, globally unique addresses).
    // `want_id` (the pointer is known to point at an ID) tries the ID
    // blocks first.
    const Block *BlockFor(uint64_t ptr, size_t group, size_t *inner = nullptr, bool want_id = false) const;

    const unsigned char *data() const { return reinterpret_cast<const unsigned char *>(bytes_.data()); }
    size_t size() const { return bytes_.size(); }

    // Primitive reads at absolute byte offsets, honoring file endianness.
    // Out-of-range reads return 0.
    uint64_t ReadUnsigned(size_t offset, size_t bytes) const;
    int64_t ReadSigned(size_t offset, size_t bytes) const;
    float ReadFloat(size_t offset) const;
    double ReadDouble(size_t offset) const;
    uint64_t ReadPointer(size_t offset) const { return ReadUnsigned(offset, ptr_size_); }
    // A NUL-terminated string starting at `offset`, at most `max` bytes.
    std::string ReadString(size_t offset, size_t max) const;

private:
    bool ParseHeaderAndBlocks(std::string *error);
    bool ParseSdna(const Block &b, std::string *error);

    std::string bytes_;
    int version_ = 0;
    int format_ = 0;
    size_t ptr_size_ = 8;
    bool big_endian_ = false;
    std::string compression_;
    std::vector<Block> blocks_;
    std::vector<Struct> structs_;
    std::unordered_map<std::string, size_t> struct_index_;
    // old address -> blocks_[], per lookup tier (see BlockFor).
    std::map<std::pair<size_t, uint64_t>, size_t> by_group_address_;
    std::map<uint64_t, size_t> by_id_address_;
    std::map<uint64_t, size_t> by_address_;
};

// A struct instance inside the file: reads fields by name. A
// default-constructed (or failed-lookup) View is invalid and every read
// on it returns the given default, so chains like
// `ob.Deref("data").Int("totvert")` need no null checks in between.
class View {
public:
    View() = default;
    View(const File *f, const Struct *s, size_t offset, size_t group)
        : file_(f), struct_(s), offset_(offset), group_(group) {}

    bool valid() const { return file_ != nullptr && struct_ != nullptr; }
    const Struct *type() const { return struct_; }
    const File *file() const { return file_; }
    size_t offset() const { return offset_; }
    size_t group() const { return group_; }  // Block::group its pointers resolve against
    bool Has(std::string_view field) const;

    // Element `index` of a numeric field (0 for scalars); integer fields
    // read through Float convert, and vice versa.
    int64_t Int(std::string_view field, int64_t def = 0, size_t index = 0) const;
    double Float(std::string_view field, double def = 0.0, size_t index = 0) const;
    // Pointer value (old address) of a pointer field, element `index`.
    uint64_t Ptr(std::string_view field, size_t index = 0) const;
    // A char-array field as a string (up to its first NUL).
    std::string Str(std::string_view field) const;
    // An embedded struct field (e.g. Object.id, Mesh.vdata).
    View Sub(std::string_view field) const;
    // Follows a pointer field to the struct it points at; the struct
    // type comes from the field declaration, falling back to the block's
    // own SDNA type when the field is `void *`.
    View Deref(std::string_view field, size_t index = 0) const;
    // The n-th struct of the same type laid out right after this one
    // (array elements in one block).
    View At(size_t n) const;

private:
    const File *file_ = nullptr;
    const Struct *struct_ = nullptr;
    size_t offset_ = 0;
    size_t group_ = 0;
};

// View of the struct at old address `ptr` (resolved from `group`, see
// File::BlockFor), typed as `type` (or the block's own SDNA struct when
// `type` is empty). The view's group is the target block's group.
View ViewAt(const File &f, uint64_t ptr, size_t group, std::string_view type = {});

// View of the first struct in block `index` (typed by its SDNA index).
View ViewOfBlock(const File &f, size_t index);

}  // namespace blend
