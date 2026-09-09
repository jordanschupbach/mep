#include "zip_archive.h"

#include <cstring>

#include "deflate.h"

namespace zip {

namespace {

void AppendLE16(std::string &s, uint16_t v) {
    s += static_cast<char>(v & 0xff);
    s += static_cast<char>((v >> 8) & 0xff);
}
void AppendLE32(std::string &s, uint32_t v) {
    s += static_cast<char>(v & 0xff);
    s += static_cast<char>((v >> 8) & 0xff);
    s += static_cast<char>((v >> 16) & 0xff);
    s += static_cast<char>((v >> 24) & 0xff);
}
uint16_t ReadLE16(const unsigned char *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t ReadLE32(const unsigned char *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

std::string BuildArchive(const std::vector<EntryToWrite> &entries) {
    std::string out;
    struct CdRecord {
        std::string name;
        uint32_t crc, comp_size, uncomp_size, local_offset;
        uint16_t method;
    };
    std::vector<CdRecord> cd;
    for (const EntryToWrite &e : entries) {
        uint32_t crc = deflate::Crc32(0, reinterpret_cast<const unsigned char *>(e.data.data()), e.data.size());
        std::string comp_data;
        uint16_t method;
        if (e.store || e.data.empty()) {
            comp_data = e.data;
            method = 0;
        } else {
            std::string compressed =
                deflate::DeflateRaw(reinterpret_cast<const unsigned char *>(e.data.data()), e.data.size());
            if (!compressed.empty() && compressed.size() < e.data.size()) {
                comp_data = std::move(compressed);
                method = 8;
            } else {
                comp_data = e.data;  // incompressible/tiny -- store rather than grow
                method = 0;
            }
        }
        uint32_t local_offset = static_cast<uint32_t>(out.size());
        out += "PK\x03\x04";
        AppendLE16(out, 20);   // version needed
        AppendLE16(out, 0);    // general purpose flag -- always 0: no data descriptor, no UTF-8 flag needed (ASCII names only)
        AppendLE16(out, method);
        AppendLE16(out, 0);    // mod time
        AppendLE16(out, 0x21);  // mod date: 1980-01-01, the standard "no real timestamp" zip placeholder
        AppendLE32(out, crc);
        AppendLE32(out, static_cast<uint32_t>(comp_data.size()));
        AppendLE32(out, static_cast<uint32_t>(e.data.size()));
        AppendLE16(out, static_cast<uint16_t>(e.name.size()));
        AppendLE16(out, 0);  // extra field length
        out += e.name;
        out += comp_data;
        cd.push_back({e.name, crc, static_cast<uint32_t>(comp_data.size()), static_cast<uint32_t>(e.data.size()),
                       local_offset, method});
    }
    uint32_t cd_offset = static_cast<uint32_t>(out.size());
    for (const CdRecord &r : cd) {
        out += "PK\x01\x02";
        AppendLE16(out, 20);  // version made by
        AppendLE16(out, 20);  // version needed
        AppendLE16(out, 0);   // general purpose flag
        AppendLE16(out, r.method);
        AppendLE16(out, 0);    // mod time
        AppendLE16(out, 0x21);  // mod date
        AppendLE32(out, r.crc);
        AppendLE32(out, r.comp_size);
        AppendLE32(out, r.uncomp_size);
        AppendLE16(out, static_cast<uint16_t>(r.name.size()));
        AppendLE16(out, 0);  // extra field length
        AppendLE16(out, 0);  // comment length
        AppendLE16(out, 0);  // disk number start
        AppendLE16(out, 0);  // internal file attributes
        AppendLE32(out, 0);  // external file attributes
        AppendLE32(out, r.local_offset);
        out += r.name;
    }
    uint32_t cd_size = static_cast<uint32_t>(out.size()) - cd_offset;
    out += "PK\x05\x06";
    AppendLE16(out, 0);  // this disk number
    AppendLE16(out, 0);  // disk with start of central directory
    AppendLE16(out, static_cast<uint16_t>(cd.size()));  // entries on this disk
    AppendLE16(out, static_cast<uint16_t>(cd.size()));  // total entries
    AppendLE32(out, cd_size);
    AppendLE32(out, cd_offset);
    AppendLE16(out, 0);  // comment length
    return out;
}

// -- Reading ----------------------------------------------------------------

namespace {

struct CentralEntry {
    std::string name;
    uint32_t crc = 0, comp_size = 0, uncomp_size = 0, local_offset = 0;
    uint16_t method = 0;
};

// Locates the end-of-central-directory record by searching backward for
// its "PK\x05\x06" signature -- the standard ZIP-parsing approach, since
// the only fixed anchor is the very end of the file (the EOCD's own
// trailing comment field is variable-length, 0-65535 bytes, so it can't
// just be assumed to start at a fixed offset from EOF).
bool FindEocd(const unsigned char *data, size_t len, size_t *eocd_pos) {
    if (len < 22) return false;
    size_t search_start = len > 65557 ? len - 65557 : 0;
    for (size_t pos = len - 22 + 1; pos-- > search_start;) {
        if (data[pos] == 'P' && data[pos + 1] == 'K' && data[pos + 2] == 0x05 && data[pos + 3] == 0x06) {
            *eocd_pos = pos;
            return true;
        }
        if (pos == search_start) break;
    }
    return false;
}

bool ParseCentralDirectory(const unsigned char *data, size_t len, std::vector<CentralEntry> &entries,
                            std::string &error) {
    size_t eocd_pos = 0;
    if (!FindEocd(data, len, &eocd_pos)) {
        error = "not a valid zip archive (no end-of-central-directory record)";
        return false;
    }
    if (eocd_pos + 22 > len) {
        error = "truncated end-of-central-directory record";
        return false;
    }
    const unsigned char *eocd = data + eocd_pos;
    uint16_t total_entries = ReadLE16(eocd + 10);
    uint32_t cd_size = ReadLE32(eocd + 12);
    uint32_t cd_offset = ReadLE32(eocd + 16);
    if (cd_offset > len || static_cast<uint64_t>(cd_offset) + cd_size > len) {
        error = "central directory out of range";
        return false;
    }

    entries.reserve(total_entries);
    size_t pos = cd_offset;
    for (uint16_t i = 0; i < total_entries; i++) {
        if (pos + 46 > len || std::memcmp(data + pos, "PK\x01\x02", 4) != 0) {
            error = "malformed central directory entry";
            return false;
        }
        const unsigned char *rec = data + pos;
        CentralEntry e;
        e.method = ReadLE16(rec + 10);
        e.crc = ReadLE32(rec + 16);
        e.comp_size = ReadLE32(rec + 20);
        e.uncomp_size = ReadLE32(rec + 24);
        uint16_t name_len = ReadLE16(rec + 28);
        uint16_t extra_len = ReadLE16(rec + 30);
        uint16_t comment_len = ReadLE16(rec + 32);
        e.local_offset = ReadLE32(rec + 42);
        pos += 46;
        if (pos + name_len > len) {
            error = "malformed central directory entry name";
            return false;
        }
        e.name.assign(reinterpret_cast<const char *>(data + pos), name_len);
        pos += static_cast<size_t>(name_len) + extra_len + comment_len;
        entries.push_back(std::move(e));
    }
    return true;
}

// Decompresses one entry given its central-directory record -- the
// local header is only consulted for its name/extra-field lengths (to
// locate where the entry's data actually starts); crc/sizes always come
// from the central directory, which is correct even for entries a
// data-descriptor-writing tool (general-purpose bit 3 set, local
// header's own crc/size fields left zero) produced -- see this file's
// own header comment on why BuildArchive itself never writes that form,
// but real-world Word/LibreOffice-produced archives sometimes do.
bool ExtractEntry(const unsigned char *data, size_t len, const CentralEntry &e, std::string &out) {
    if (e.local_offset + 30 > len || std::memcmp(data + e.local_offset, "PK\x03\x04", 4) != 0) return false;
    const unsigned char *local = data + e.local_offset;
    uint16_t name_len = ReadLE16(local + 26);
    uint16_t extra_len = ReadLE16(local + 28);
    size_t data_start = e.local_offset + 30 + name_len + extra_len;
    if (data_start + e.comp_size > len) return false;

    if (e.method == 0) {
        out.assign(reinterpret_cast<const char *>(data + data_start), e.comp_size);
    } else if (e.method == 8) {
        if (!deflate::InflateRaw(data + data_start, e.comp_size, out)) return false;
    } else {
        return false;  // unsupported method -- see MINIZ_REMOVAL_PLAN.md's Scoping decision 1: DEFLATE/STORED only
    }
    if (out.size() != e.uncomp_size) return false;
    uint32_t actual_crc = deflate::Crc32(0, reinterpret_cast<const unsigned char *>(out.data()), out.size());
    return actual_crc == e.crc;
}

}  // namespace

bool Extract(const unsigned char *zip_bytes, size_t zip_len, const char *entry_name, std::string &out) {
    std::vector<CentralEntry> entries;
    std::string error;
    if (!ParseCentralDirectory(zip_bytes, zip_len, entries, error)) return false;
    for (const CentralEntry &e : entries) {
        if (e.name == entry_name) return ExtractEntry(zip_bytes, zip_len, e, out);
    }
    return false;
}

bool ListAll(const unsigned char *zip_bytes, size_t zip_len, std::vector<EntryToWrite> &out, std::string &error) {
    std::vector<CentralEntry> entries;
    if (!ParseCentralDirectory(zip_bytes, zip_len, entries, error)) return false;
    out.clear();
    out.reserve(entries.size());
    for (const CentralEntry &e : entries) {
        std::string content;
        if (!ExtractEntry(zip_bytes, zip_len, e, content)) {
            error = "failed to extract entry '" + e.name + "'";
            return false;
        }
        out.push_back(EntryToWrite{e.name, std::move(content), e.method == 0});
    }
    return true;
}

}  // namespace zip
