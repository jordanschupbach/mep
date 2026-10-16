// .blend container, header, file-block and SDNA parsing. See blend_file.h.

#include "blend_file.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

#include "deflate.h"
#include "zstd_decode.h"

namespace blend {

namespace {

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

int ParseDigits(const std::string &s, size_t pos, size_t n) {
    int v = 0;
    for (size_t i = 0; i < n; i++) {
        if (pos + i >= s.size() || !IsDigit(s[pos + i])) return -1;
        v = v * 10 + (s[pos + i] - '0');
    }
    return v;
}

// gzip (RFC 1952), possibly several members back to back.
bool Gunzip(const std::string &in, std::string *out, std::string *error) {
    const unsigned char *p = reinterpret_cast<const unsigned char *>(in.data());
    size_t len = in.size();
    size_t pos = 0;
    out->clear();
    while (pos + 18 <= len && p[pos] == 0x1f && p[pos + 1] == 0x8b) {
        if (p[pos + 2] != 8) {
            if (error) *error = "gzip: unsupported compression method";
            return false;
        }
        unsigned flags = p[pos + 3];
        size_t q = pos + 10;
        if (flags & 4) {  // FEXTRA
            if (q + 2 > len) break;
            size_t xlen = static_cast<size_t>(p[q]) | (static_cast<size_t>(p[q + 1]) << 8);
            q += 2 + xlen;
        }
        if (flags & 8) {  // FNAME
            while (q < len && p[q] != 0) q++;
            q++;
        }
        if (flags & 16) {  // FCOMMENT
            while (q < len && p[q] != 0) q++;
            q++;
        }
        if (flags & 2) q += 2;  // FHCRC
        if (q >= len) break;
        std::string member;
        size_t used = 0;
        if (!deflate::InflateRaw(p + q, len - q, member, &used)) {
            if (error) *error = "gzip: corrupt deflate stream";
            return false;
        }
        out->append(member);
        pos = q + used + 8;  // CRC32 + ISIZE
    }
    if (out->empty()) {
        if (error) *error = "gzip: no data";
        return false;
    }
    return true;
}

}  // namespace

const Field *Struct::Find(std::string_view field) const {
    auto it = index.find(std::string(field));
    return it == index.end() ? nullptr : &fields[it->second];
}

bool Block::Is(const char *c) const {
    size_t n = std::strlen(c);
    for (size_t i = 0; i < 4; i++) {
        char want = i < n ? c[i] : '\0';
        if (code[i] != want) return false;
    }
    return true;
}

bool File::Load(const std::string &path, std::string *error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return Parse(std::move(bytes), error);
}

bool File::Parse(std::string bytes, std::string *error) {
    compression_.clear();
    const unsigned char *p = reinterpret_cast<const unsigned char *>(bytes.data());
    if (bytes.size() >= 2 && p[0] == 0x1f && p[1] == 0x8b) {
        std::string raw;
        if (!Gunzip(bytes, &raw, error)) return false;
        bytes_ = std::move(raw);
        compression_ = "gzip";
    } else if (zstd::LooksLikeZstd(p, bytes.size())) {
        std::string raw;
        if (!zstd::Decompress(p, bytes.size(), raw, error)) return false;
        bytes_ = std::move(raw);
        compression_ = "zstd";
    } else {
        bytes_ = std::move(bytes);
    }
    return ParseHeaderAndBlocks(error);
}

bool File::ParseHeaderAndBlocks(std::string *error) {
    auto fail = [&](const std::string &msg) {
        if (error) *error = msg;
        return false;
    };
    if (bytes_.size() < 12 || bytes_.compare(0, 7, "BLENDER") != 0) return fail("not a .blend file (bad header)");
    size_t header_size;
    if (IsDigit(bytes_[7])) {
        // "BLENDER17-01v0500": header size, '-', file-format version,
        // endianness, 4-digit Blender version.
        int hs = ParseDigits(bytes_, 7, 2);
        if (hs < 17 || bytes_.size() < static_cast<size_t>(hs) || bytes_[9] != '-') return fail("bad .blend header");
        header_size = static_cast<size_t>(hs);
        format_ = ParseDigits(bytes_, 10, 2);
        if (format_ != 1) return fail("unsupported .blend file-format version " + std::to_string(format_));
        char endian = bytes_[12];
        if (endian != 'v' && endian != 'V') return fail("bad .blend header");
        big_endian_ = endian == 'V';
        version_ = ParseDigits(bytes_, 13, 4);
        ptr_size_ = 8;
    } else {
        header_size = 12;
        format_ = 0;
        if (bytes_[7] == '_') {
            ptr_size_ = 4;
        } else if (bytes_[7] == '-') {
            ptr_size_ = 8;
        } else {
            return fail("bad .blend header");
        }
        if (bytes_[8] != 'v' && bytes_[8] != 'V') return fail("bad .blend header");
        big_endian_ = bytes_[8] == 'V';
        version_ = ParseDigits(bytes_, 9, 3);
    }
    if (version_ < 0) return fail("bad .blend version");

    size_t pos = header_size;
    const Block *dna = nullptr;
    blocks_.clear();
    by_group_address_.clear();
    by_id_address_.clear();
    by_address_.clear();
    for (;;) {
        Block b;
        size_t bhead_size = format_ == 1 ? 32 : (ptr_size_ == 8 ? 24 : 20);
        if (pos + bhead_size > bytes_.size()) {
            // Some writers omit ENDB; accept a clean end of blocks.
            if (pos == bytes_.size()) break;
            return fail("truncated .blend block header");
        }
        std::memcpy(b.code, bytes_.data() + pos, 4);
        if (format_ == 1) {
            b.sdna = static_cast<uint32_t>(ReadUnsigned(pos + 4, 4));
            b.old = ReadUnsigned(pos + 8, 8);
            int64_t len = ReadSigned(pos + 16, 8);
            int64_t nr = ReadSigned(pos + 24, 8);
            if (len < 0 || nr < 0) return fail("corrupt .blend block header");
            b.len = static_cast<size_t>(len);
            b.nr = static_cast<size_t>(nr);
        } else {
            int64_t len = ReadSigned(pos + 4, 4);
            b.old = ReadPointer(pos + 8);
            b.sdna = static_cast<uint32_t>(ReadUnsigned(pos + 8 + ptr_size_, 4));
            int64_t nr = ReadSigned(pos + 12 + ptr_size_, 4);
            if (len < 0 || nr < 0) return fail("corrupt .blend block header");
            b.len = static_cast<size_t>(len);
            b.nr = static_cast<size_t>(nr);
        }
        pos += bhead_size;
        b.offset = pos;
        if (b.Is("ENDB")) break;
        if (b.len > bytes_.size() - pos) return fail("truncated .blend block");
        pos += b.len;
        blocks_.push_back(b);
    }
    size_t owner = 0;
    for (size_t i = 0; i < blocks_.size(); i++) {
        Block &b = blocks_[i];
        if (b.Is("DNA1")) dna = &b;
        bool is_data = b.Is("DATA");
        if (!is_data) owner = i;
        b.group = owner;
        if (b.old == 0) continue;
        by_group_address_.emplace(std::make_pair(owner, b.old), i);
        if (!is_data) by_id_address_.emplace(b.old, i);
        by_address_.emplace(b.old, i);
    }
    if (!dna) return fail(".blend file has no DNA1 (SDNA) block");
    return ParseSdna(*dna, error);
}

bool File::ParseSdna(const Block &b, std::string *error) {
    auto fail = [&](const std::string &msg) {
        if (error) *error = "SDNA: " + msg;
        return false;
    };
    size_t base = b.offset;
    size_t end = b.offset + b.len;
    size_t pos = base;
    auto expect = [&](const char *tag) {
        if (pos + 4 > end || bytes_.compare(pos, 4, tag) != 0) return false;
        pos += 4;
        return true;
    };
    auto align4 = [&]() { pos = base + ((pos - base + 3) & ~size_t{3}); };
    auto read_strings = [&](std::vector<std::string> *out) {
        if (pos + 4 > end) return false;
        int64_t n = ReadSigned(pos, 4);
        pos += 4;
        if (n < 0 || static_cast<uint64_t>(n) > b.len) return false;
        out->reserve(static_cast<size_t>(n));
        for (int64_t i = 0; i < n; i++) {
            size_t e = pos;
            while (e < end && bytes_[e] != '\0') e++;
            if (e >= end) return false;
            out->emplace_back(bytes_.data() + pos, e - pos);
            pos = e + 1;
        }
        align4();
        return true;
    };

    std::vector<std::string> names, types;
    if (!expect("SDNA") || !expect("NAME") || !read_strings(&names)) return fail("bad NAME section");
    if (!expect("TYPE") || !read_strings(&types)) return fail("bad TYPE section");
    if (!expect("TLEN")) return fail("bad TLEN section");
    std::vector<size_t> tlen(types.size());
    if (pos + 2 * types.size() > end) return fail("bad TLEN section");
    for (size_t i = 0; i < types.size(); i++) tlen[i] = static_cast<size_t>(ReadUnsigned(pos + 2 * i, 2));
    pos += 2 * types.size();
    align4();
    if (!expect("STRC") || pos + 4 > end) return fail("bad STRC section");
    int64_t nstructs = ReadSigned(pos, 4);
    pos += 4;
    if (nstructs < 0) return fail("bad STRC section");

    // Struct names first, so field types can be resolved to struct indices.
    struct RawStruct {
        size_t type;
        std::vector<std::pair<size_t, size_t>> fields;
    };
    std::vector<RawStruct> raw;
    raw.reserve(static_cast<size_t>(nstructs));
    for (int64_t s = 0; s < nstructs; s++) {
        if (pos + 4 > end) return fail("truncated STRC section");
        RawStruct rs;
        rs.type = static_cast<size_t>(ReadUnsigned(pos, 2));
        size_t nf = static_cast<size_t>(ReadUnsigned(pos + 2, 2));
        pos += 4;
        if (rs.type >= types.size() || pos + 4 * nf > end) return fail("corrupt STRC entry");
        for (size_t f = 0; f < nf; f++) {
            size_t ft = static_cast<size_t>(ReadUnsigned(pos, 2));
            size_t fn = static_cast<size_t>(ReadUnsigned(pos + 2, 2));
            pos += 4;
            if (ft >= types.size() || fn >= names.size()) return fail("corrupt STRC field");
            rs.fields.emplace_back(ft, fn);
        }
        raw.push_back(std::move(rs));
    }
    structs_.clear();
    struct_index_.clear();
    structs_.resize(raw.size());
    std::unordered_map<size_t, size_t> type_to_struct;
    for (size_t s = 0; s < raw.size(); s++) {
        structs_[s].name = types[raw[s].type];
        structs_[s].size = tlen[raw[s].type];
        struct_index_.emplace(structs_[s].name, s);
        type_to_struct.emplace(raw[s].type, s);
    }
    for (size_t s = 0; s < raw.size(); s++) {
        Struct &st = structs_[s];
        size_t offset = 0;
        for (auto [ft, fn] : raw[s].fields) {
            Field f;
            f.type = types[ft];
            f.decl = names[fn];
            const std::string &d = f.decl;
            bool func = d.size() > 1 && d[0] == '(' && d[1] == '*';
            std::string bare;
            size_t count = 1;
            for (size_t i = 0; i < d.size(); i++) {
                char c = d[i];
                if (c == '*') {
                    f.pointer_depth++;
                } else if (c == '[') {
                    size_t close = d.find(']', i);
                    if (close == std::string::npos) break;
                    size_t n = 0;
                    for (size_t k = i + 1; k < close; k++) {
                        if (IsDigit(d[k])) n = n * 10 + static_cast<size_t>(d[k] - '0');
                    }
                    count *= n;
                    i = close;
                } else if (c == '(' || c == ')') {
                    if (func && c == ')' && !bare.empty()) {
                        // "(*func)()": the name ends at the first ')'.
                        break;
                    }
                } else {
                    bare += c;
                }
            }
            if (func && f.pointer_depth == 0) f.pointer_depth = 1;
            f.name = bare;
            f.count = count;
            if (f.pointer_depth > 0) {
                f.elem_size = ptr_size_;
            } else {
                f.elem_size = tlen[ft];
                f.is_float = f.type == "float" || f.type == "double";
                auto it = type_to_struct.find(ft);
                if (it != type_to_struct.end()) f.struct_index = static_cast<int>(it->second);
            }
            f.size = f.elem_size * f.count;
            f.offset = offset;
            offset += f.size;
            if (st.fields.empty()) st.is_id = f.type == "ID" && f.name == "id" && f.pointer_depth == 0;
            st.index.emplace(f.name, st.fields.size());
            st.fields.push_back(std::move(f));
        }
    }
    return true;
}

const Struct *File::FindStruct(std::string_view name) const {
    auto it = struct_index_.find(std::string(name));
    return it == struct_index_.end() ? nullptr : &structs_[it->second];
}

const Struct *File::StructOf(const Block &b) const { return b.sdna < structs_.size() ? &structs_[b.sdna] : nullptr; }

const Block *File::BlockFor(uint64_t ptr, size_t group, size_t *inner, bool want_id) const {
    if (ptr == 0) return nullptr;
    auto contains = [&](size_t index) -> const Block * {
        const Block &b = blocks_[index];
        if (ptr < b.old) return nullptr;
        uint64_t delta = ptr - b.old;
        if (delta != 0 && delta >= b.len) return nullptr;
        if (inner) *inner = static_cast<size_t>(delta);
        return &b;
    };
    auto in = [&](const std::map<uint64_t, size_t> &map) -> const Block * {
        auto jt = map.upper_bound(ptr);
        if (jt == map.begin()) return nullptr;
        --jt;
        return contains(jt->second);
    };
    if (want_id) {
        if (const Block *b = in(by_id_address_)) return b;
    }
    auto it = by_group_address_.upper_bound(std::make_pair(group, ptr));
    if (it != by_group_address_.begin()) {
        --it;
        if (it->first.first == group) {
            if (const Block *b = contains(it->second)) return b;
        }
    }
    for (const std::map<uint64_t, size_t> *map : {&by_id_address_, &by_address_}) {
        auto jt = map->upper_bound(ptr);
        if (jt == map->begin()) continue;
        --jt;
        if (const Block *b = contains(jt->second)) return b;
    }
    return nullptr;
}

uint64_t File::ReadUnsigned(size_t offset, size_t bytes) const {
    if (bytes == 0 || bytes > 8 || offset > bytes_.size() || bytes > bytes_.size() - offset) return 0;
    const unsigned char *p = data() + offset;
    uint64_t v = 0;
    if (big_endian_) {
        for (size_t i = 0; i < bytes; i++) v = (v << 8) | p[i];
    } else {
        for (size_t i = bytes; i-- > 0;) v = (v << 8) | p[i];
    }
    return v;
}

int64_t File::ReadSigned(size_t offset, size_t bytes) const {
    uint64_t v = ReadUnsigned(offset, bytes);
    if (bytes > 0 && bytes < 8) {
        uint64_t sign = uint64_t{1} << (bytes * 8 - 1);
        if (v & sign) v |= ~((sign << 1) - 1);
    }
    return static_cast<int64_t>(v);
}

float File::ReadFloat(size_t offset) const {
    uint32_t u = static_cast<uint32_t>(ReadUnsigned(offset, 4));
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

double File::ReadDouble(size_t offset) const {
    uint64_t u = ReadUnsigned(offset, 8);
    double d;
    std::memcpy(&d, &u, 8);
    return d;
}

std::string File::ReadString(size_t offset, size_t max) const {
    if (offset >= bytes_.size()) return {};
    size_t n = std::min(max, bytes_.size() - offset);
    const char *p = bytes_.data() + offset;
    size_t len = 0;
    while (len < n && p[len] != '\0') len++;
    return std::string(p, len);
}

bool View::Has(std::string_view field) const { return valid() && struct_->Find(field) != nullptr; }

int64_t View::Int(std::string_view field, int64_t def, size_t index) const {
    if (!valid()) return def;
    const Field *f = struct_->Find(field);
    if (!f || index >= f->count || f->pointer_depth > 0) return def;
    size_t at = offset_ + f->offset + index * f->elem_size;
    if (f->is_float) {
        return static_cast<int64_t>(f->elem_size == 8 ? file_->ReadDouble(at) : static_cast<double>(file_->ReadFloat(at)));
    }
    if (f->elem_size == 0 || f->elem_size > 8) return def;
    bool is_unsigned = f->type == "char" || f->type == "uchar" || f->type == "ushort" || f->type == "uint" ||
                       f->type == "uint8_t" || f->type == "uint16_t" || f->type == "uint32_t" || f->type == "uint64_t" ||
                       f->type == "ulong";
    if (is_unsigned) return static_cast<int64_t>(file_->ReadUnsigned(at, f->elem_size));
    return file_->ReadSigned(at, f->elem_size);
}

double View::Float(std::string_view field, double def, size_t index) const {
    if (!valid()) return def;
    const Field *f = struct_->Find(field);
    if (!f || index >= f->count || f->pointer_depth > 0) return def;
    if (!f->is_float) return static_cast<double>(Int(field, static_cast<int64_t>(def), index));
    size_t at = offset_ + f->offset + index * f->elem_size;
    return f->elem_size == 8 ? file_->ReadDouble(at) : static_cast<double>(file_->ReadFloat(at));
}

uint64_t View::Ptr(std::string_view field, size_t index) const {
    if (!valid()) return 0;
    const Field *f = struct_->Find(field);
    if (!f || f->pointer_depth == 0 || index >= f->count) return 0;
    return file_->ReadPointer(offset_ + f->offset + index * f->elem_size);
}

std::string View::Str(std::string_view field) const {
    if (!valid()) return {};
    const Field *f = struct_->Find(field);
    if (!f || f->pointer_depth > 0) return {};
    return file_->ReadString(offset_ + f->offset, f->size);
}

View View::Sub(std::string_view field) const {
    if (!valid()) return {};
    const Field *f = struct_->Find(field);
    if (!f || f->pointer_depth > 0 || f->struct_index < 0) return {};
    return View(file_, &file_->structs()[static_cast<size_t>(f->struct_index)], offset_ + f->offset, group_);
}

View View::Deref(std::string_view field, size_t index) const {
    if (!valid()) return {};
    const Field *f = struct_->Find(field);
    if (!f || f->pointer_depth == 0) return {};
    uint64_t ptr = Ptr(field, index);
    if (f->pointer_depth == 1 && file_->FindStruct(f->type)) return ViewAt(*file_, ptr, group_, f->type);
    return ViewAt(*file_, ptr, group_);
}

View View::At(size_t n) const {
    if (!valid()) return {};
    size_t at = offset_ + n * struct_->size;
    if (at + struct_->size > file_->size()) return {};
    return View(file_, struct_, at, group_);
}

View ViewAt(const File &f, uint64_t ptr, size_t group, std::string_view type) {
    size_t inner = 0;
    const Struct *want = type.empty() ? nullptr : f.FindStruct(type);
    if (!type.empty() && !want) return {};
    const Block *b = f.BlockFor(ptr, group, &inner, want && want->is_id);
    if (!b) return {};
    const Struct *s = want ? want : f.StructOf(*b);
    if (!s) return {};
    if (inner + s->size > b->len) return {};
    return View(&f, s, b->offset + inner, b->group);
}

View ViewOfBlock(const File &f, size_t index) {
    if (index >= f.blocks().size()) return {};
    const Block &b = f.blocks()[index];
    const Struct *s = f.StructOf(b);
    if (!s || s->size > b.len) return {};
    return View(&f, s, b.offset, b.group);
}

}  // namespace blend
