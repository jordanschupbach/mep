#include "pdf_xref.h"

#include "pdf_filters.h"

#include <algorithm>
#include <cctype>
#include <memory>

namespace pdfxref {

namespace {

bool MatchKeyword(const unsigned char *data, size_t len, size_t pos, const char *keyword) {
    size_t klen = std::char_traits<char>::length(keyword);
    if (pos + klen > len) return false;
    for (size_t i = 0; i < klen; ++i) {
        if (data[pos + i] != static_cast<unsigned char>(keyword[i])) return false;
    }
    return true;
}

// Searches backward from the end of the buffer for the last occurrence
// of `needle` -- used for "startxref" (a file can contain more than one,
// across incremental updates; only the last is authoritative) and, in
// recovery mode, the last "trailer" keyword.
bool FindLast(const unsigned char *data, size_t len, const char *needle, size_t *out_pos) {
    size_t nlen = std::char_traits<char>::length(needle);
    if (nlen > len) return false;
    for (size_t i = len - nlen;; --i) {
        if (MatchKeyword(data, len, i, needle)) {
            *out_pos = i;
            return true;
        }
        if (i == 0) break;
    }
    return false;
}

bool ReadUInt(const unsigned char *data, size_t len, size_t &pos, long long *out) {
    size_t start = pos;
    long long value = 0;
    while (pos < len && std::isdigit(data[pos])) {
        value = value * 10 + (data[pos] - '0');
        ++pos;
    }
    if (pos == start) return false;
    *out = value;
    return true;
}

long long ReadBigEndianField(const unsigned char *data, size_t width) {
    long long value = 0;
    for (size_t i = 0; i < width; ++i) value = (value << 8) | data[i];
    return value;
}

}  // namespace

const Entry *XrefTable::Find(int num) const {
    auto it = entries_.find(num);
    return it == entries_.end() ? nullptr : &it->second;
}

void XrefTable::SetupEncryption(const unsigned char *data, size_t len) {
    is_encrypted_ = false;
    encryption_.reset();

    const pdfobj::Object *encrypt_ref = trailer_.Find("Encrypt");
    if (!encrypt_ref || encrypt_ref->IsNull()) return;
    is_encrypted_ = true;

    // The /Encrypt dict's own object number, if it's an indirect
    // reference (the overwhelmingly common real-world case) -- -1 if
    // embedded directly in the trailer, matching EncryptionState's own
    // "no object number to exclude from decryption" convention (see
    // pdf_crypt.h). Resolved via ResolveObject with encryption_ still
    // null (just reset above), so this lookup itself is never
    // decrypted -- correct either way, since spec 7.6.1 says the
    // Encrypt dict's own strings are never encrypted in the first
    // place.
    pdfobj::Object encrypt_dict = *encrypt_ref;
    int encrypt_obj_num = -1;
    if (encrypt_ref->IsReference()) {
        encrypt_obj_num = encrypt_ref->ref_val.num;
        encrypt_dict = ResolveObject(data, len, *this, encrypt_ref->ref_val.num, encrypt_ref->ref_val.gen);
    }
    if (!encrypt_dict.IsDict()) return;

    // Trailer's own /ID array, first element -- required by Algorithm 2
    // and never itself encrypted (it's a direct trailer entry, never
    // reached through ResolveObject's decryption path at all).
    std::string id0;
    if (const pdfobj::Object *id = trailer_.Find("ID")) {
        if (id->IsArray() && !id->array_val.empty() && id->array_val[0].IsString()) id0 = id->array_val[0].str_val;
    }

    auto state = std::make_unique<pdfcrypt::EncryptionState>();
    if (pdfcrypt::SetupStandardSecurityHandler(encrypt_dict, id0, encrypt_obj_num, state.get())) {
        encryption_ = std::move(state);
    }
    // Else: a real (non-empty) user password is required, or this uses
    // an unsupported /Filter//V/R/CFM combination -- encryption_ stays
    // null, IsEncrypted() stays true; see this method's own doc comment
    // in pdf_xref.h for how a caller is meant to tell those apart from
    // "not encrypted at all".
}

void XrefTable::MergeTrailer(const pdfobj::Object &section_trailer) {
    if (!section_trailer.IsDict()) return;
    for (const auto &kv : section_trailer.dict_val) {
        trailer_.type = pdfobj::Type::Dict;
        trailer_.dict_val.emplace(kv.first, kv.second);  // first-wins: emplace no-ops if key already present
    }
}

bool XrefTable::LoadClassicTable(const unsigned char *data, size_t len, size_t pos, long long *out_prev,
                                  bool *out_has_prev) {
    *out_has_prev = false;
    if (!MatchKeyword(data, len, pos, "xref")) return false;
    pos += 4;

    while (true) {
        pdfobj::SkipWhitespaceAndComments(data, len, pos);
        if (MatchKeyword(data, len, pos, "trailer")) {
            pos += 7;
            break;
        }
        long long start = 0, count = 0;
        size_t save = pos;
        if (!ReadUInt(data, len, pos, &start)) return pos != save;  // no subsections at all: malformed, else tolerate what we got
        pdfobj::SkipWhitespaceAndComments(data, len, pos);
        if (!ReadUInt(data, len, pos, &count)) break;
        for (long long i = 0; i < count; ++i) {
            pdfobj::SkipWhitespaceAndComments(data, len, pos);
            long long offset = 0, gen = 0;
            if (!ReadUInt(data, len, pos, &offset)) break;
            pdfobj::SkipWhitespaceAndComments(data, len, pos);
            if (!ReadUInt(data, len, pos, &gen)) break;
            pdfobj::SkipWhitespaceAndComments(data, len, pos);
            if (pos >= len) break;
            unsigned char kind_char = data[pos];
            ++pos;
            int obj_num = static_cast<int>(start + i);
            Entry entry;
            if (kind_char == 'n') {
                entry.kind = EntryKind::InUse;
                entry.offset = offset;
                entry.gen = static_cast<int>(gen);
            } else {
                entry.kind = EntryKind::Free;
            }
            entries_.emplace(obj_num, entry);
        }
    }

    pdfobj::SkipWhitespaceAndComments(data, len, pos);
    pdfobj::Object trailer_dict;
    if (!pdfobj::ParseObject(data, len, pos, &trailer_dict) || !trailer_dict.IsDict()) return true;  // tolerate a missing/malformed trailer dict
    MergeTrailer(trailer_dict);
    if (const pdfobj::Object *prev = trailer_dict.Find("Prev")) {
        *out_prev = prev->AsInt(0);
        *out_has_prev = true;
    }
    return true;
}

bool XrefTable::LoadXrefStream(const unsigned char *data, size_t len, size_t pos, long long *out_prev,
                                bool *out_has_prev) {
    *out_has_prev = false;
    pdfobj::IndirectObject ind;
    if (!pdfobj::ParseIndirectObject(data, len, pos, &ind) || !ind.value.IsDict() || !ind.has_stream) return false;

    const pdfobj::Object *w = ind.value.Find("W");
    if (!w || !w->IsArray() || w->array_val.size() != 3) return false;
    int widths[3] = {static_cast<int>(w->array_val[0].AsInt()), static_cast<int>(w->array_val[1].AsInt()),
                      static_cast<int>(w->array_val[2].AsInt())};
    int record_width = widths[0] + widths[1] + widths[2];
    if (record_width <= 0) return false;

    std::string raw(reinterpret_cast<const char *>(data) + ind.stream_offset, ind.stream_length);
    std::string decoded;
    if (!pdffilter::FlateDecode(raw, ind.value.Find("DecodeParms"), &decoded)) return false;

    std::vector<std::pair<long long, long long>> index_ranges;
    if (const pdfobj::Object *index = ind.value.Find("Index")) {
        for (size_t i = 0; i + 1 < index->array_val.size(); i += 2) {
            index_ranges.emplace_back(index->array_val[i].AsInt(), index->array_val[i + 1].AsInt());
        }
    }
    if (index_ranges.empty()) {
        long long size = 0;
        if (const pdfobj::Object *sz = ind.value.Find("Size")) size = sz->AsInt();
        index_ranges.emplace_back(0, size);
    }

    const unsigned char *rec = reinterpret_cast<const unsigned char *>(decoded.data());
    size_t rec_pos = 0;
    for (const auto &range : index_ranges) {
        for (long long i = 0; i < range.second; ++i) {
            if (rec_pos + static_cast<size_t>(record_width) > decoded.size()) break;
            int obj_num = static_cast<int>(range.first + i);
            long long type = widths[0] == 0 ? 1 : ReadBigEndianField(rec + rec_pos, static_cast<size_t>(widths[0]));
            long long f2 = widths[1] == 0 ? 0 : ReadBigEndianField(rec + rec_pos + static_cast<size_t>(widths[0]),
                                                                    static_cast<size_t>(widths[1]));
            long long f3 = widths[2] == 0
                               ? 0
                               : ReadBigEndianField(rec + rec_pos + static_cast<size_t>(widths[0] + widths[1]),
                                                     static_cast<size_t>(widths[2]));
            rec_pos += static_cast<size_t>(record_width);

            Entry entry;
            if (type == 0) {
                entry.kind = EntryKind::Free;
            } else if (type == 1) {
                entry.kind = EntryKind::InUse;
                entry.offset = f2;
                entry.gen = static_cast<int>(f3);
            } else if (type == 2) {
                entry.kind = EntryKind::Compressed;
                entry.stream_num = f2;
                entry.index_in_stream = f3;
            } else {
                continue;  // unrecognized type: skip this record, tolerant of malformed content
            }
            entries_.emplace(obj_num, entry);
        }
    }

    MergeTrailer(ind.value);
    if (const pdfobj::Object *prev = ind.value.Find("Prev")) {
        *out_prev = prev->AsInt(0);
        *out_has_prev = true;
    }
    return true;
}

bool XrefTable::LoadSection(const unsigned char *data, size_t len, long long offset, long long *out_prev,
                             bool *out_has_prev) {
    if (offset < 0 || static_cast<size_t>(offset) >= len) return false;
    size_t pos = static_cast<size_t>(offset);
    pdfobj::SkipWhitespaceAndComments(data, len, pos);
    if (MatchKeyword(data, len, pos, "xref")) return LoadClassicTable(data, len, pos, out_prev, out_has_prev);
    return LoadXrefStream(data, len, pos, out_prev, out_has_prev);
}

void XrefTable::Load(const unsigned char *data, size_t len) {
    size_t startxref_pos = 0;
    if (!FindLast(data, len, "startxref", &startxref_pos)) {
        RecoverByScanning(data, len);
        return;
    }
    size_t pos = startxref_pos + std::char_traits<char>::length("startxref");
    pdfobj::SkipWhitespaceAndComments(data, len, pos);
    long long offset = 0;
    if (!ReadUInt(data, len, pos, &offset)) {
        RecoverByScanning(data, len);
        return;
    }

    std::vector<long long> visited;
    bool first = true;
    while (true) {
        if (std::find(visited.begin(), visited.end(), offset) != visited.end()) break;  // cyclic /Prev: stop
        visited.push_back(offset);
        long long prev = 0;
        bool has_prev = false;
        bool ok = LoadSection(data, len, offset, &prev, &has_prev);
        if (!ok) {
            if (first) {
                RecoverByScanning(data, len);
                return;
            }
            break;  // a broken /Prev section past the first: tolerate, keep what's already merged
        }
        first = false;
        if (!has_prev) break;
        offset = prev;
    }

    if (entries_.empty() && trailer_.Find("Root") == nullptr) {
        RecoverByScanning(data, len);
        return;
    }
    SetupEncryption(data, len);
}

void XrefTable::RecoverByScanning(const unsigned char *data, size_t len) {
    entries_.clear();
    trailer_ = pdfobj::Object();

    // Forward scan for "N G obj" (a later occurrence of the same object
    // number overwrites an earlier one -- last revision wins, matching
    // incremental-update semantics, the opposite merge direction from
    // the normal /Prev-chain-newest-first walk above since this scans
    // physical file order instead).
    size_t pos = 0;
    while (pos + 3 <= len) {
        if (!std::isdigit(data[pos]) || (pos > 0 && std::isdigit(data[pos - 1]))) {
            ++pos;
            continue;
        }
        size_t p = pos;
        long long num = 0;
        if (!ReadUInt(data, len, p, &num)) {
            ++pos;
            continue;
        }
        size_t after_num = p;
        pdfobj::SkipWhitespaceAndComments(data, len, p);
        if (p == after_num) {  // "obj" must be whitespace-separated from the object number
            ++pos;
            continue;
        }
        long long gen = 0;
        if (!ReadUInt(data, len, p, &gen)) {
            ++pos;
            continue;
        }
        size_t after_gen = p;
        pdfobj::SkipWhitespaceAndComments(data, len, p);
        if (p == after_gen || !MatchKeyword(data, len, p, "obj")) {
            ++pos;
            continue;
        }
        p += 3;
        Entry entry;
        entry.kind = EntryKind::InUse;
        entry.offset = static_cast<long long>(pos);
        entry.gen = static_cast<int>(gen);
        entries_[static_cast<int>(num)] = entry;  // forward overwrite: last one in the file wins
        pos = p;
    }

    size_t trailer_pos = 0;
    if (FindLast(data, len, "trailer", &trailer_pos)) {
        size_t p = trailer_pos + std::char_traits<char>::length("trailer");
        pdfobj::SkipWhitespaceAndComments(data, len, p);
        pdfobj::Object trailer_dict;
        if (pdfobj::ParseObject(data, len, p, &trailer_dict) && trailer_dict.IsDict()) {
            MergeTrailer(trailer_dict);
        }
    }

    if (trailer_.Find("Root") == nullptr) {
        for (const auto &kv : entries_) {
            if (kv.second.kind != EntryKind::InUse) continue;
            pdfobj::IndirectObject ind;
            if (!pdfobj::ParseIndirectObject(data, len, static_cast<size_t>(kv.second.offset), &ind)) continue;
            if (!ind.value.IsDict()) continue;
            const pdfobj::Object *type = ind.value.Find("Type");
            if (type && type->AsString("") == "Catalog") {
                pdfobj::Object root;
                root.type = pdfobj::Type::Reference;
                root.ref_val.num = kv.first;
                root.ref_val.gen = kv.second.gen;
                trailer_.type = pdfobj::Type::Dict;
                trailer_.dict_val["Root"] = root;
                break;
            }
        }
    }
    if (trailer_.Find("Size") == nullptr && !entries_.empty()) {
        int max_num = entries_.rbegin()->first;
        pdfobj::Object size;
        size.type = pdfobj::Type::Int;
        size.int_val = max_num + 1;
        trailer_.type = pdfobj::Type::Dict;
        trailer_.dict_val["Size"] = size;
    }
    SetupEncryption(data, len);
}

pdfobj::Object ResolveObject(const unsigned char *data, size_t len, const XrefTable &table, int num, int /*gen*/) {
    const Entry *entry = table.Find(num);
    if (!entry) return pdfobj::Object();

    if (entry->kind == EntryKind::InUse) {
        if (entry->offset < 0 || static_cast<size_t>(entry->offset) >= len) return pdfobj::Object();
        pdfobj::IndirectObject ind;
        auto resolver = [&](int rnum, int rgen, long long *out_length) -> bool {
            pdfobj::Object length_obj = ResolveObject(data, len, table, rnum, rgen);
            if (!length_obj.IsNumber()) return false;
            *out_length = length_obj.AsInt();
            return true;
        };
        if (!pdfobj::ParseIndirectObject(data, len, static_cast<size_t>(entry->offset), &ind, resolver)) {
            return pdfobj::Object();
        }
        // PDFIUM_REMOVAL_PLAN.md Phase 12: decrypt every String value
        // nested in the parsed object, using ITS OWN recorded (num, gen)
        // -- ind.num/ind.gen, as actually parsed from this object's own
        // "N G obj" header, rather than the caller-supplied `gen`
        // parameter (which this function already documents as not
        // authoritative -- see its own doc comment in pdf_xref.h). A
        // no-op when the document isn't encrypted (table.Encryption()
        // null) or this is the /Encrypt dict's own object number.
        pdfcrypt::DecryptObjectStrings(table.Encryption(), ind.num, ind.gen, &ind.value);
        return ind.value;
    }

    if (entry->kind == EntryKind::Compressed) {
        // The whole ObjStm is decoded and indexed once (GetDecodedObjStm),
        // then every object it contains is a hash lookup + a single
        // ParseObject -- the pre-cache version re-inflated and re-scanned
        // the entire stream on every one of these calls, which is what
        // made opening a large object-stream PDF (hundreds of page dicts
        // sharing one ObjStm) freeze for ~20s.
        const DecodedObjStm *stm = table.GetDecodedObjStm(data, len, static_cast<int>(entry->stream_num));
        if (!stm) return pdfobj::Object();  // cannot happen (see contract), but keep the resolve total
        auto it = stm->offset.find(num);
        if (it == stm->offset.end()) return pdfobj::Object();
        size_t value_pos = it->second;
        if (value_pos >= stm->body.size()) return pdfobj::Object();
        pdfobj::Object value;
        pdfobj::ParseObject(reinterpret_cast<const unsigned char *>(stm->body.data()), stm->body.size(), value_pos,
                             &value);
        return value;
    }

    return pdfobj::Object();
}

const DecodedObjStm *XrefTable::GetDecodedObjStm(const unsigned char *data, size_t len, int stream_num) const {
    // Fast path: already decoded. The stored shared_ptr keeps the
    // DecodedObjStm alive for the table's lifetime, so returning a raw
    // pointer out from under the lock is safe (entries are never erased).
    {
        std::lock_guard<std::mutex> lk(objstm_cache_->mtx);
        auto it = objstm_cache_->map.find(stream_num);
        if (it != objstm_cache_->map.end()) return it->second.get();
    }

    // Decode OUTSIDE the lock: resolving the ObjStm's own /Length (a rare
    // but legal indirect reference) re-enters ResolveObject, which could
    // reach GetDecodedObjStm again for a different stream -- holding the
    // lock across that would risk deadlock. The cost of two threads
    // occasionally decoding the same stream on a cold miss (one loses the
    // emplace race below) is negligible next to that.
    auto decoded = std::make_shared<DecodedObjStm>();

    const Entry *stream_entry = Find(stream_num);
    if (stream_entry && stream_entry->kind == EntryKind::InUse) {
        pdfobj::IndirectObject stream_ind;
        auto resolver = [&](int rnum, int rgen, long long *out_length) -> bool {
            pdfobj::Object length_obj = ResolveObject(data, len, *this, rnum, rgen);
            if (!length_obj.IsNumber()) return false;
            *out_length = length_obj.AsInt();
            return true;
        };
        if (pdfobj::ParseIndirectObject(data, len, static_cast<size_t>(stream_entry->offset), &stream_ind, resolver) &&
            stream_ind.has_stream && stream_ind.value.IsDict()) {
            const pdfobj::Object &stream_obj = stream_ind.value;
            // The ObjStm's own raw bytes are decrypted as a whole stream
            // (using ITS OWN object number/generation) before FlateDecode;
            // the individual objects extracted from the decompressed body
            // are never separately decrypted -- spec 7.5.7: object-stream
            // contents are already plaintext once the containing stream is
            // decrypted. Cross-reference streams (LoadXrefStream) are the
            // one stream type never encrypted at all, per the same section.
            std::string raw = pdfcrypt::DecryptStreamBytes(
                Encryption(), stream_ind.num, stream_ind.gen,
                std::string(reinterpret_cast<const char *>(data) + stream_ind.stream_offset,
                            stream_ind.stream_length));
            std::string body;
            if (pdffilter::FlateDecode(raw, stream_obj.Find("DecodeParms"), &body)) {
                long long n = 0, first = 0;
                if (const pdfobj::Object *n_obj = stream_obj.Find("N")) n = n_obj->AsInt();
                if (const pdfobj::Object *first_obj = stream_obj.Find("First")) first = first_obj->AsInt();

                // Index every "obj_num obj_offset" pair in the header once,
                // storing each object's ABSOLUTE value position (/First +
                // offset) so a later lookup is O(1).
                const unsigned char *header = reinterpret_cast<const unsigned char *>(body.data());
                size_t header_len = body.size();
                size_t hpos = 0;
                for (long long i = 0; i < n; ++i) {
                    pdfobj::SkipWhitespaceAndComments(header, header_len, hpos);
                    long long obj_num = 0;
                    if (!ReadUInt(header, header_len, hpos, &obj_num)) break;
                    pdfobj::SkipWhitespaceAndComments(header, header_len, hpos);
                    long long obj_offset = 0;
                    if (!ReadUInt(header, header_len, hpos, &obj_offset)) break;
                    long long value_pos = first + obj_offset;
                    if (value_pos >= 0 && static_cast<size_t>(value_pos) < body.size()) {
                        // First occurrence wins if a stream lists a number
                        // twice (matches the old early-break-on-match scan).
                        decoded->offset.emplace(static_cast<int>(obj_num), static_cast<size_t>(value_pos));
                    }
                }
                decoded->body = std::move(body);
            }
        }
    }

    std::lock_guard<std::mutex> lk(objstm_cache_->mtx);
    // Another thread may have decoded the same stream while we worked
    // unlocked; keep whichever landed first, they are equivalent.
    auto [it, inserted] = objstm_cache_->map.emplace(stream_num, std::move(decoded));
    (void)inserted;
    return it->second.get();
}

bool ResolveStream(const unsigned char *data, size_t len, const XrefTable &table, int num, int /*gen*/,
                    pdfobj::Object *out_dict, std::string *out_raw_stream) {
    *out_dict = pdfobj::Object();
    out_raw_stream->clear();
    const Entry *entry = table.Find(num);
    if (!entry || entry->kind != EntryKind::InUse) return false;
    if (entry->offset < 0 || static_cast<size_t>(entry->offset) >= len) return false;

    pdfobj::IndirectObject ind;
    auto resolver = [&](int rnum, int rgen, long long *out_length) -> bool {
        pdfobj::Object length_obj = ResolveObject(data, len, table, rnum, rgen);
        if (!length_obj.IsNumber()) return false;
        *out_length = length_obj.AsInt();
        return true;
    };
    if (!pdfobj::ParseIndirectObject(data, len, static_cast<size_t>(entry->offset), &ind, resolver)) return false;
    pdfcrypt::DecryptObjectStrings(table.Encryption(), ind.num, ind.gen, &ind.value);
    *out_dict = ind.value;
    if (!ind.has_stream) return false;
    *out_raw_stream = pdfcrypt::DecryptStreamBytes(
        table.Encryption(), ind.num, ind.gen,
        std::string(reinterpret_cast<const char *>(data) + ind.stream_offset, ind.stream_length));
    return true;
}

}  // namespace pdfxref
