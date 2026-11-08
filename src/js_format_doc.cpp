// Prettier 3.6.2's document builders, utilities and printer, ported from
// src/document/{builders,utils,printer}.js and src/utils/get-string-width.js.
// See js_format_doc.h.

#include "js_format_doc.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace jsfmt {

// ---------------------------------------------------------------- arena

DocArena::DocArena() {
    line = New(DocKind::Line);
    softline = New(DocKind::Line);
    softline->soft = true;
    hardline_nobreak = New(DocKind::Line);
    hardline_nobreak->hard = true;
    literalline_nobreak = New(DocKind::Line);
    literalline_nobreak->hard = true;
    literalline_nobreak->literal = true;
    break_parent = New(DocKind::BreakParent);
    line_suffix_boundary = New(DocKind::LineSuffixBoundary);
    trim = New(DocKind::Trim);
    hardline = Cat({hardline_nobreak, break_parent});
    literalline = Cat({literalline_nobreak, break_parent});
}

Doc* DocArena::New(DocKind k) {
    if (docs_.size() >= max_docs_) throw BudgetExceeded();
    docs_.emplace_back();
    Doc* d = &docs_.back();
    d->kind = k;
    return d;
}

Doc** DocArena::AllocParts(size_t n) {
    if (n == 0) return nullptr;
    if (n > chunk_left_) {
        const size_t size = std::max<size_t>(n, 4096);
        chunks_.emplace_back(new Doc*[size]);
        chunk_pos_ = chunks_.back().get();
        chunk_left_ = size;
    }
    Doc** p = chunk_pos_;
    chunk_pos_ += n;
    chunk_left_ -= n;
    return p;
}

Doc* DocArena::Str(std::string_view s) {
    if (s.empty()) return nullptr;
    Doc* d = New(DocKind::String);
    d->str = s;
    return d;
}

Doc* DocArena::Own(std::string s) {
    if (s.empty()) return nullptr;
    strings_.push_back(std::move(s));
    return Str(strings_.back());
}

Doc* DocArena::Cat(std::initializer_list<Doc*> parts) {
    Doc* d = New(DocKind::Array);
    d->n = static_cast<uint32_t>(parts.size());
    d->parts = AllocParts(parts.size());
    std::copy(parts.begin(), parts.end(), d->parts);
    return d;
}

Doc* DocArena::Cat(const Parts& parts) {
    Doc* d = New(DocKind::Array);
    d->n = static_cast<uint32_t>(parts.size());
    d->parts = AllocParts(parts.size());
    std::copy(parts.begin(), parts.end(), d->parts);
    return d;
}

Doc* DocArena::Indent(Doc* contents) {
    Doc* d = New(DocKind::Indent);
    d->contents = contents;
    return d;
}

Doc* DocArena::Align(int n, Doc* contents) {
    Doc* d = New(DocKind::Align);
    d->align_kind = AlignKind::Width;
    d->align_n = n;
    d->contents = contents;
    return d;
}

Doc* DocArena::AlignText(std::string_view s, Doc* contents) {
    Doc* d = New(DocKind::Align);
    d->align_kind = AlignKind::Text;
    d->str = s;
    d->contents = contents;
    return d;
}

Doc* DocArena::DedentToRoot(Doc* contents) {
    Doc* d = New(DocKind::Align);
    d->align_kind = AlignKind::DedentToRoot;
    d->contents = contents;
    return d;
}

Doc* DocArena::MarkAsRoot(Doc* contents) {
    Doc* d = New(DocKind::Align);
    d->align_kind = AlignKind::Root;
    d->contents = contents;
    return d;
}

Doc* DocArena::Dedent(Doc* contents) {
    Doc* d = New(DocKind::Align);
    d->align_kind = AlignKind::Dedent;
    d->contents = contents;
    return d;
}

Doc* DocArena::Group(Doc* contents, bool should_break, GroupId id) {
    Doc* d = New(DocKind::Group);
    d->contents = contents;
    d->brk = should_break ? 1 : 0;
    d->id = id;
    return d;
}

Doc* DocArena::ConditionalGroup(const Parts& states, bool should_break) {
    Doc* d = New(DocKind::Group);
    d->contents = states.empty() ? nullptr : states[0];
    d->brk = should_break ? 1 : 0;
    d->expanded = true;
    d->n = static_cast<uint32_t>(states.size());
    d->parts = AllocParts(states.size());
    std::copy(states.begin(), states.end(), d->parts);
    return d;
}

Doc* DocArena::Fill(const Parts& parts) {
    Doc* d = New(DocKind::Fill);
    d->n = static_cast<uint32_t>(parts.size());
    d->parts = AllocParts(parts.size());
    std::copy(parts.begin(), parts.end(), d->parts);
    return d;
}

Doc* DocArena::IfBreak(Doc* break_contents, Doc* flat_contents, GroupId group_id) {
    Doc* d = New(DocKind::IfBreak);
    d->contents = break_contents;
    d->flat = flat_contents;
    d->id = group_id;
    return d;
}

Doc* DocArena::IndentIfBreak(Doc* contents, GroupId group_id, bool negate) {
    Doc* d = New(DocKind::IndentIfBreak);
    d->contents = contents;
    d->id = group_id;
    d->negate = negate;
    return d;
}

Doc* DocArena::LineSuffix(Doc* contents) {
    Doc* d = New(DocKind::LineSuffix);
    d->contents = contents;
    return d;
}

Doc* DocArena::Label(uint32_t label, Doc* contents) {
    if (!label) return contents;
    Doc* d = New(DocKind::Label);
    d->label = label;
    d->contents = contents;
    return d;
}

Parts DocArena::JoinParts(Doc* sep, const Parts& docs) {
    Parts parts;
    parts.reserve(docs.size() * 2);
    for (size_t i = 0; i < docs.size(); i++) {
        if (i != 0) parts.push_back(sep);
        parts.push_back(docs[i]);
    }
    return parts;
}

Doc* DocArena::Join(Doc* sep, const Parts& docs) { return Cat(JoinParts(sep, docs)); }

Doc* DocArena::AddAlignmentToDoc(Doc* doc, int size, int tab_width) {
    Doc* aligned = doc;
    if (size > 0) {
        for (int i = 0; i < size / tab_width; ++i) aligned = Indent(aligned);
        aligned = Align(size % tab_width, aligned);
        aligned = DedentToRoot(aligned);
    }
    return aligned;
}

// ---------------------------------------------------------------- utils

namespace {

// traverseDoc's child order (pushed reversed so they pop in order).
template <class OnEnter>
void Traverse(Doc* root, OnEnter&& on_enter) {
    std::vector<Doc*> stack{root};
    while (!stack.empty()) {
        Doc* doc = stack.back();
        stack.pop_back();
        if (!doc) continue;
        if (!on_enter(doc)) continue;
        switch (doc->kind) {
            case DocKind::Array:
            case DocKind::Fill:
                for (uint32_t i = doc->n; i-- > 0;) stack.push_back(doc->parts[i]);
                break;
            case DocKind::IfBreak:
                stack.push_back(doc->flat);
                stack.push_back(doc->contents);
                break;
            case DocKind::Group:
            case DocKind::Align:
            case DocKind::Indent:
            case DocKind::IndentIfBreak:
            case DocKind::Label:
            case DocKind::LineSuffix:
                stack.push_back(doc->contents);
                break;
            default:
                break;
        }
    }
}

}  // namespace

bool DocArena::WillBreak(Doc* doc) {
    bool found = false;
    Traverse(doc, [&](Doc* d) {
        if (found) return false;
        if ((d->kind == DocKind::Group && d->brk) || (d->kind == DocKind::Line && d->hard) ||
            d->kind == DocKind::BreakParent) {
            found = true;
            return false;
        }
        return true;
    });
    return found;
}

bool DocArena::CanBreak(Doc* doc) {
    bool found = false;
    Traverse(doc, [&](Doc* d) {
        if (found) return false;
        if (d->kind == DocKind::Line) {
            found = true;
            return false;
        }
        return true;
    });
    return found;
}

bool DocArena::IsEmptyDoc(Doc* doc) {
    bool empty = true;
    Traverse(doc, [&](Doc* d) {
        if (!empty) return false;
        switch (d->kind) {
            case DocKind::String:
            case DocKind::Trim:
            case DocKind::LineSuffixBoundary:
            case DocKind::Line:
            case DocKind::BreakParent:
                empty = false;
                return false;
            default:
                return true;
        }
    });
    return empty;
}

void DocArena::PropagateBreaks(Doc* root) {
    std::unordered_set<Doc*> visited;
    std::vector<Doc*> groups;
    auto break_parent_group = [&]() {
        if (!groups.empty()) {
            Doc* parent = groups.back();
            if (!parent->expanded && !parent->brk) parent->brk = 2;
        }
    };
    // (doc, exit?) pairs; exit entries are only pushed for groups (the only
    // kind propagateBreaks' onExit looks at).
    std::vector<std::pair<Doc*, bool>> stack{{root, false}};
    while (!stack.empty()) {
        auto [doc, is_exit] = stack.back();
        stack.pop_back();
        if (!doc) continue;
        if (is_exit) {
            Doc* g = groups.back();
            groups.pop_back();
            if (g->brk) break_parent_group();
            continue;
        }
        if (doc->kind == DocKind::BreakParent) break_parent_group();
        if (doc->kind == DocKind::Group) {
            stack.push_back({doc, true});
            groups.push_back(doc);
            if (!visited.insert(doc).second) continue;
        }
        switch (doc->kind) {
            case DocKind::Array:
            case DocKind::Fill:
                for (uint32_t i = doc->n; i-- > 0;) stack.push_back({doc->parts[i], false});
                break;
            case DocKind::IfBreak:
                stack.push_back({doc->flat, false});
                stack.push_back({doc->contents, false});
                break;
            case DocKind::Group:
                if (doc->expanded) {
                    for (uint32_t i = doc->n; i-- > 0;) stack.push_back({doc->parts[i], false});
                } else {
                    stack.push_back({doc->contents, false});
                }
                break;
            case DocKind::Align:
            case DocKind::Indent:
            case DocKind::IndentIfBreak:
            case DocKind::Label:
            case DocKind::LineSuffix:
                stack.push_back({doc->contents, false});
                break;
            default:
                break;
        }
    }
}

template <class Fn>
Doc* DocArena::MapDoc(Doc* root, Fn&& fn) {
    if (!root || root->kind == DocKind::String) return fn(root);
    std::unordered_map<Doc*, Doc*> mapped;
    auto rec = [&](auto&& self, Doc* doc) -> Doc* {
        if (!doc) return fn(doc);
        auto it = mapped.find(doc);
        if (it != mapped.end()) return it->second;
        Doc* result = nullptr;
        switch (doc->kind) {
            case DocKind::Array:
            case DocKind::Fill: {
                Parts parts(doc->n);
                for (uint32_t i = 0; i < doc->n; i++) parts[i] = self(self, doc->parts[i]);
                Doc* copy = doc->kind == DocKind::Array ? Cat(parts) : Fill(parts);
                result = fn(copy);
                break;
            }
            case DocKind::IfBreak: {
                Doc* b = self(self, doc->contents);
                Doc* f = self(self, doc->flat);
                Doc* copy = New(DocKind::IfBreak);
                *copy = *doc;
                copy->contents = b;
                copy->flat = f;
                result = fn(copy);
                break;
            }
            case DocKind::Group: {
                Doc* copy = New(DocKind::Group);
                *copy = *doc;
                if (doc->expanded) {
                    copy->parts = AllocParts(doc->n);
                    for (uint32_t i = 0; i < doc->n; i++) copy->parts[i] = self(self, doc->parts[i]);
                    copy->contents = doc->n ? copy->parts[0] : nullptr;
                } else {
                    copy->contents = self(self, doc->contents);
                }
                result = fn(copy);
                break;
            }
            case DocKind::Align:
            case DocKind::Indent:
            case DocKind::IndentIfBreak:
            case DocKind::Label:
            case DocKind::LineSuffix: {
                Doc* c = self(self, doc->contents);
                Doc* copy = New(doc->kind);
                *copy = *doc;
                copy->contents = c;
                if (doc->kind == DocKind::IndentIfBreak) copy->flat = nullptr;  // cached indent()
                result = fn(copy);
                break;
            }
            default:
                result = fn(doc);
                break;
        }
        mapped[doc] = result;
        return result;
    };
    return rec(rec, root);
}

Doc* DocArena::RemoveLines(Doc* doc) {
    return MapDoc(doc, [&](Doc* d) -> Doc* {
        if (!d) return d;
        if (d->kind == DocKind::Line && !d->hard) return d->soft ? nullptr : Str(" ");
        if (d->kind == DocKind::IfBreak) return d->flat;
        return d;
    });
}

Doc* DocArena::CleanDoc(Doc* doc) {
    return MapDoc(doc, [&](Doc* d) -> Doc* {
        if (!d) return d;
        switch (d->kind) {
            case DocKind::Fill: {
                bool all_empty = true;
                for (uint32_t i = 0; i < d->n; i++) {
                    if (d->parts[i]) all_empty = false;
                }
                if (all_empty) return nullptr;
                break;
            }
            case DocKind::Group:
                if (!d->contents && !d->id && !d->brk && !d->expanded) return nullptr;
                if (d->contents && d->contents->kind == DocKind::Group && d->contents->id == d->id &&
                    d->contents->brk == d->brk && !d->expanded && !d->contents->expanded) {
                    return d->contents;
                }
                break;
            case DocKind::Align:
            case DocKind::Indent:
            case DocKind::IndentIfBreak:
            case DocKind::LineSuffix:
                if (!d->contents) return nullptr;
                break;
            case DocKind::IfBreak:
                if (!d->flat && !d->contents) return nullptr;
                break;
            case DocKind::Array: {
                Parts parts;
                for (uint32_t i = 0; i < d->n; i++) {
                    Doc* part = d->parts[i];
                    if (!part) continue;
                    Doc* first = part;
                    Doc** rest = nullptr;
                    uint32_t nrest = 0;
                    if (part->kind == DocKind::Array) {
                        if (part->n == 0) continue;  // [currentPart] = [] -> undefined, skipped below
                        first = part->parts[0];
                        rest = part->parts + 1;
                        nrest = part->n - 1;
                    }
                    if (first && first->kind == DocKind::String && !parts.empty() && parts.back() &&
                        parts.back()->kind == DocKind::String) {
                        parts.back() = Own(std::string(parts.back()->str) + std::string(first->str));
                    } else {
                        parts.push_back(first);
                    }
                    for (uint32_t k = 0; k < nrest; k++) parts.push_back(rest[k]);
                }
                if (parts.empty()) return nullptr;
                if (parts.size() == 1) return parts[0];
                return Cat(parts);
            }
            default:
                break;
        }
        return d;
    });
}

Parts DocArena::StripTrailingHardlineFromParts(Doc** src, uint32_t count) {
    Parts parts(src, src + count);
    while (parts.size() >= 2 && parts[parts.size() - 2] && parts[parts.size() - 2]->kind == DocKind::Line &&
           parts.back() && parts.back()->kind == DocKind::BreakParent) {
        parts.resize(parts.size() - 2);
    }
    if (!parts.empty()) parts.back() = StripTrailingHardlineFromDoc(parts.back());
    return parts;
}

Doc* DocArena::StripTrailingHardlineFromDoc(Doc* doc) {
    if (!doc) return doc;
    switch (doc->kind) {
        case DocKind::Indent:
        case DocKind::IndentIfBreak:
        case DocKind::Group:
        case DocKind::LineSuffix:
        case DocKind::Label: {
            Doc* copy = New(doc->kind);
            *copy = *doc;
            copy->contents = StripTrailingHardlineFromDoc(doc->contents);
            if (doc->kind == DocKind::IndentIfBreak) copy->flat = nullptr;
            return copy;
        }
        case DocKind::IfBreak: {
            Doc* copy = New(doc->kind);
            *copy = *doc;
            copy->contents = StripTrailingHardlineFromDoc(doc->contents);
            copy->flat = StripTrailingHardlineFromDoc(doc->flat);
            return copy;
        }
        case DocKind::Fill:
            return Fill(StripTrailingHardlineFromParts(doc->parts, doc->n));
        case DocKind::Array:
            return Cat(StripTrailingHardlineFromParts(doc->parts, doc->n));
        case DocKind::String: {
            std::string_view s = doc->str;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.remove_suffix(1);
            return s.size() == doc->str.size() ? doc : Str(s);
        }
        default:
            return doc;
    }
}

Doc* DocArena::StripTrailingHardline(Doc* doc) { return StripTrailingHardlineFromDoc(CleanDoc(doc)); }

Doc* DocArena::ReplaceEndOfLine(std::string_view text) {
    Parts pieces;
    size_t start = 0;
    while (true) {
        const size_t nl = text.find('\n', start);
        if (nl == std::string_view::npos) {
            pieces.push_back(Str(text.substr(start)));
            break;
        }
        pieces.push_back(Str(text.substr(start, nl - start)));
        start = nl + 1;
    }
    return Join(literalline, pieces);
}

Doc* DocArena::ReplaceEndOfLineDoc(Doc* doc) {
    return MapDoc(doc, [&](Doc* d) -> Doc* {
        if (!d) return ReplaceEndOfLine("");
        if (d->kind == DocKind::String) return ReplaceEndOfLine(d->str);
        return d;
    });
}

// ---------------------------------------------------------------- string width

namespace {

struct Range {
    uint32_t lo, hi;
};

// get-east-asian-width's isWide || isFullWidth, as code point ranges
// (generated from the copy bundled in prettier 3.6.2's doc.mjs).
const Range kWide[] = {
    {0x1100,0x115f}, {0x231a,0x231b}, {0x2329,0x232a}, {0x23e9,0x23ec}, {0x23f0,0x23f0}, {0x23f3,0x23f3},
    {0x25fd,0x25fe}, {0x2614,0x2615}, {0x2630,0x2637}, {0x2648,0x2653}, {0x267f,0x267f}, {0x268a,0x268f},
    {0x2693,0x2693}, {0x26a1,0x26a1}, {0x26aa,0x26ab}, {0x26bd,0x26be}, {0x26c4,0x26c5}, {0x26ce,0x26ce},
    {0x26d4,0x26d4}, {0x26ea,0x26ea}, {0x26f2,0x26f3}, {0x26f5,0x26f5}, {0x26fa,0x26fa}, {0x26fd,0x26fd},
    {0x2705,0x2705}, {0x270a,0x270b}, {0x2728,0x2728}, {0x274c,0x274c}, {0x274e,0x274e}, {0x2753,0x2755},
    {0x2757,0x2757}, {0x2795,0x2797}, {0x27b0,0x27b0}, {0x27bf,0x27bf}, {0x2b1b,0x2b1c}, {0x2b50,0x2b50},
    {0x2b55,0x2b55}, {0x2e80,0x2e99}, {0x2e9b,0x2ef3}, {0x2f00,0x2fd5}, {0x2ff0,0x303e}, {0x3041,0x3096},
    {0x3099,0x30ff}, {0x3105,0x312f}, {0x3131,0x318e}, {0x3190,0x31e5}, {0x31ef,0x321e}, {0x3220,0x3247},
    {0x3250,0xa48c}, {0xa490,0xa4c6}, {0xa960,0xa97c}, {0xac00,0xd7a3}, {0xf900,0xfaff}, {0xfe10,0xfe19},
    {0xfe30,0xfe52}, {0xfe54,0xfe66}, {0xfe68,0xfe6b}, {0xff01,0xff60}, {0xffe0,0xffe6}, {0x16fe0,0x16fe4},
    {0x16ff0,0x16ff1}, {0x17000,0x187f7}, {0x18800,0x18cd5}, {0x18cff,0x18d08}, {0x1aff0,0x1aff3}, {0x1aff5,0x1affb},
    {0x1affd,0x1affe}, {0x1b000,0x1b122}, {0x1b132,0x1b132}, {0x1b150,0x1b152}, {0x1b155,0x1b155}, {0x1b164,0x1b167},
    {0x1b170,0x1b2fb}, {0x1d300,0x1d356}, {0x1d360,0x1d376}, {0x1f004,0x1f004}, {0x1f0cf,0x1f0cf}, {0x1f18e,0x1f18e},
    {0x1f191,0x1f19a}, {0x1f200,0x1f202}, {0x1f210,0x1f23b}, {0x1f240,0x1f248}, {0x1f250,0x1f251}, {0x1f260,0x1f265},
    {0x1f300,0x1f320}, {0x1f32d,0x1f335}, {0x1f337,0x1f37c}, {0x1f37e,0x1f393}, {0x1f3a0,0x1f3ca}, {0x1f3cf,0x1f3d3},
    {0x1f3e0,0x1f3f0}, {0x1f3f4,0x1f3f4}, {0x1f3f8,0x1f43e}, {0x1f440,0x1f440}, {0x1f442,0x1f4fc}, {0x1f4ff,0x1f53d},
    {0x1f54b,0x1f54e}, {0x1f550,0x1f567}, {0x1f57a,0x1f57a}, {0x1f595,0x1f596}, {0x1f5a4,0x1f5a4}, {0x1f5fb,0x1f64f},
    {0x1f680,0x1f6c5}, {0x1f6cc,0x1f6cc}, {0x1f6d0,0x1f6d2}, {0x1f6d5,0x1f6d7}, {0x1f6dc,0x1f6df}, {0x1f6eb,0x1f6ec},
    {0x1f6f4,0x1f6fc}, {0x1f7e0,0x1f7eb}, {0x1f7f0,0x1f7f0}, {0x1f90c,0x1f93a}, {0x1f93c,0x1f945}, {0x1f947,0x1f9ff},
    {0x1fa70,0x1fa7c}, {0x1fa80,0x1fa89}, {0x1fa8f,0x1fac6}, {0x1face,0x1fadc}, {0x1fadf,0x1fae9}, {0x1faf0,0x1faf8},
    {0x20000,0x2fffd}, {0x30000,0x3fffd},
};

bool IsWide(uint32_t cp) {
    size_t lo = 0, hi = sizeof(kWide) / sizeof(kWide[0]);
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (cp < kWide[mid].lo) {
            hi = mid;
        } else if (cp > kWide[mid].hi) {
            lo = mid + 1;
        } else {
            return true;
        }
    }
    return false;
}

uint32_t DecodeUtf8(std::string_view s, size_t& i) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    auto cont = [&](size_t k) -> uint32_t {
        return i + k < s.size() ? (static_cast<unsigned char>(s[i + k]) & 0x3Fu) : 0u;
    };
    if (b0 < 0x80) {
        i += 1;
        return b0;
    }
    if ((b0 & 0xE0) == 0xC0) {
        const uint32_t cp = ((b0 & 0x1Fu) << 6) | cont(1);
        i += 2;
        return cp;
    }
    if ((b0 & 0xF0) == 0xE0) {
        const uint32_t cp = ((b0 & 0x0Fu) << 12) | (cont(1) << 6) | cont(2);
        i += 3;
        return cp;
    }
    const uint32_t cp = ((b0 & 0x07u) << 18) | (cont(1) << 12) | (cont(2) << 6) | cont(3);
    i += 4;
    return cp;
}

bool IsEmojiBase(uint32_t cp) {
    return (cp >= 0x1F000 && cp <= 0x1FAFF) || (cp >= 0x2600 && cp <= 0x27BF) || (cp >= 0x2B00 && cp <= 0x2BFF);
}

}  // namespace

int StringWidth(std::string_view s) {
    bool ascii = true;
    for (char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u > 0x7F) {
            ascii = false;
            break;
        }
    }
    if (ascii) return static_cast<int>(s.size());
    int width = 0;
    size_t i = 0;
    bool in_emoji = false;  // emoji-regex: a ZWJ sequence or modified emoji counts as one 2-wide unit
    while (i < s.size()) {
        const uint32_t cp = DecodeUtf8(s, i);
        if (cp <= 0x1F || (cp >= 0x7F && cp <= 0x9F)) continue;
        if (cp >= 0x300 && cp <= 0x36F) continue;
        if (in_emoji && (cp == 0x200D || cp == 0xFE0F || (cp >= 0x1F3FB && cp <= 0x1F3FF))) {
            if (cp == 0x200D && i < s.size()) {
                size_t j = i;
                const uint32_t next = DecodeUtf8(s, j);
                if (IsEmojiBase(next)) i = j;
            }
            continue;
        }
        if (IsEmojiBase(cp) && (IsWide(cp) || (i < s.size() && static_cast<unsigned char>(s[i]) == 0xEF))) {
            // a wide pictograph, or a text-default one followed by U+FE0F
            width += 2;
            in_emoji = true;
            continue;
        }
        in_emoji = false;
        width += IsWide(cp) ? 2 : 1;
    }
    return width;
}

size_t Utf16Length(std::string_view s) {
    size_t n = 0;
    for (char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if ((u & 0xC0) != 0x80) n += (u >= 0xF0) ? 2 : 1;
    }
    return n;
}

// ---------------------------------------------------------------- printer

namespace {

enum Mode : uint8_t { kModeNone = 0, kModeBreak = 1, kModeFlat = 2 };

struct IndPart {
    uint8_t type;  // 0 indent, 1 stringAlign, 2 numberAlign
    int n;
    std::string_view s;
};

struct Ind {
    std::string value;
    int length = 0;
    std::vector<IndPart> queue;
    const Ind* root = nullptr;
    mutable const Ind* indent_child = nullptr;
};

struct Cmd {
    const Ind* ind;
    Mode mode;
    Doc* doc;
    uint32_t fill_off;
};

class LinePrinter {
public:
    LinePrinter(DocArena& arena, const PrintOptions& opts) : arena_(arena), opts_(opts) {
        inds_.emplace_back();
        root_ind_ = &inds_.back();
    }

    std::string Print(Doc* doc);

private:
    const Ind* MakeIndent(const Ind* ind) {
        if (!ind->indent_child) ind->indent_child = GenerateInd(ind, IndPart{0, 0, {}}, false);
        return ind->indent_child;
    }

    const Ind* MakeAlign(const Ind* ind, const Doc* doc) {
        switch (doc->align_kind) {
            case AlignKind::DedentToRoot:
                return ind->root ? ind->root : root_ind_;
            case AlignKind::Dedent:
                return GenerateInd(ind, IndPart{0, 0, {}}, true);
            case AlignKind::Root: {
                inds_.push_back(*ind);
                Ind* copy = &inds_.back();
                copy->root = ind;
                copy->indent_child = nullptr;
                return copy;
            }
            case AlignKind::Text:
                if (doc->str.empty()) return ind;
                return GenerateInd(ind, IndPart{1, 0, doc->str}, false);
            case AlignKind::Width:
                if (doc->align_n == 0) return ind;
                if (doc->align_n < 0) return GenerateInd(ind, IndPart{0, 0, {}}, true);
                return GenerateInd(ind, IndPart{2, doc->align_n, {}}, false);
        }
        return ind;
    }

    const Ind* GenerateInd(const Ind* ind, IndPart part, bool dedent) {
        Ind out;
        out.root = ind->root;
        out.queue = ind->queue;
        if (dedent) {
            if (!out.queue.empty()) out.queue.pop_back();
        } else {
            out.queue.push_back(part);
        }
        int last_tabs = 0, last_spaces = 0;
        auto add_tabs = [&](int count) {
            out.value.append(static_cast<size_t>(count), '\t');
            out.length += opts_.tab_width * count;
        };
        auto add_spaces = [&](int count) {
            out.value.append(static_cast<size_t>(count), ' ');
            out.length += count;
        };
        auto reset_last = [&] { last_tabs = last_spaces = 0; };
        auto flush_spaces = [&] {
            if (last_spaces > 0) add_spaces(last_spaces);
            reset_last();
        };
        auto flush_tabs = [&] {
            if (last_tabs > 0) add_tabs(last_tabs);
            reset_last();
        };
        auto flush = [&] {
            if (opts_.use_tabs) {
                flush_tabs();
            } else {
                flush_spaces();
            }
        };
        for (const IndPart& p : out.queue) {
            switch (p.type) {
                case 0:
                    flush();
                    if (opts_.use_tabs) {
                        add_tabs(1);
                    } else {
                        add_spaces(opts_.tab_width);
                    }
                    break;
                case 1:
                    flush();
                    out.value += p.s;
                    out.length += static_cast<int>(p.s.size());
                    break;
                default:
                    last_tabs += 1;
                    last_spaces += p.n;
                    break;
            }
        }
        flush_spaces();
        inds_.push_back(std::move(out));
        return &inds_.back();
    }

    static int Trim(std::string& out) {
        size_t k = out.size();
        while (k > 0 && (out[k - 1] == ' ' || out[k - 1] == '\t')) k--;
        const int count = static_cast<int>(out.size() - k);
        out.resize(k);
        return count;
    }

    void Step() {
        if (opts_.work && ++*opts_.work > opts_.work_limit) throw BudgetExceeded();
    }

    Doc* IndentOf(Doc* iib) {
        // indent(doc.contents) for an indentIfBreak, created once.
        if (!iib->flat) iib->flat = arena_.Indent(iib->contents);
        return iib->flat;
    }

    bool Fits(const Cmd& next, const std::vector<Cmd>& rest, int64_t width, bool has_line_suffix,
              bool must_be_flat);

    DocArena& arena_;
    const PrintOptions& opts_;
    std::deque<Ind> inds_;
    const Ind* root_ind_;
    std::vector<uint8_t> group_mode_;  // indexed by GroupId

    Mode GroupModeOf(GroupId id) const { return id < group_mode_.size() ? Mode(group_mode_[id]) : kModeNone; }
};

bool LinePrinter::Fits(const Cmd& next, const std::vector<Cmd>& rest, int64_t width, bool has_line_suffix,
                       bool must_be_flat) {
    if (opts_.width < 0) return true;
    size_t rest_idx = rest.size();
    std::vector<Cmd> cmds{next};
    std::string out;
    while (width >= 0) {
        if (cmds.empty()) {
            if (rest_idx == 0) return true;
            cmds.push_back(rest[--rest_idx]);
            continue;
        }
        Step();
        const Cmd cmd = cmds.back();
        cmds.pop_back();
        Doc* doc = cmd.doc;
        if (!doc) continue;
        switch (doc->kind) {
            case DocKind::String:
                out += doc->str;
                width -= StringWidth(doc->str);
                break;
            case DocKind::Array:
            case DocKind::Fill: {
                const uint32_t end = doc->kind == DocKind::Fill ? cmd.fill_off : 0;
                for (uint32_t i = doc->n; i-- > end;) cmds.push_back(Cmd{cmd.ind, cmd.mode, doc->parts[i], 0});
                break;
            }
            case DocKind::Indent:
            case DocKind::Align:
            case DocKind::IndentIfBreak:
            case DocKind::Label:
                cmds.push_back(Cmd{cmd.ind, cmd.mode, doc->contents, 0});
                break;
            case DocKind::Trim:
                width += Trim(out);
                break;
            case DocKind::Group: {
                if (must_be_flat && doc->brk) return false;
                const Mode group_mode = doc->brk ? kModeBreak : cmd.mode;
                Doc* contents =
                    doc->expanded && group_mode == kModeBreak ? (doc->n ? doc->parts[doc->n - 1] : nullptr) : doc->contents;
                cmds.push_back(Cmd{cmd.ind, group_mode, contents, 0});
                break;
            }
            case DocKind::IfBreak: {
                Mode group_mode = cmd.mode;
                if (doc->id) {
                    group_mode = GroupModeOf(doc->id);
                    if (group_mode == kModeNone) group_mode = kModeFlat;
                }
                Doc* contents = group_mode == kModeBreak ? doc->contents : doc->flat;
                if (contents) cmds.push_back(Cmd{cmd.ind, cmd.mode, contents, 0});
                break;
            }
            case DocKind::Line:
                if (cmd.mode == kModeBreak || doc->hard) return true;
                if (!doc->soft) {
                    out += ' ';
                    width--;
                }
                break;
            case DocKind::LineSuffix:
                has_line_suffix = true;
                break;
            case DocKind::LineSuffixBoundary:
                if (has_line_suffix) return false;
                break;
            case DocKind::BreakParent:
                break;
        }
    }
    return false;
}

std::string LinePrinter::Print(Doc* root) {
    const int64_t width = opts_.width;
    int64_t pos = 0;
    std::vector<Cmd> cmds{Cmd{root_ind_, kModeBreak, root, 0}};
    std::string out;
    bool should_remeasure = false;
    std::vector<Cmd> line_suffix;

    arena_.PropagateBreaks(root);

    auto set_group_mode = [&](GroupId id, Mode m) {
        if (id >= group_mode_.size()) group_mode_.resize(id + 1, kModeNone);
        group_mode_[id] = m;
    };

    while (!cmds.empty()) {
        Step();
        const Cmd cmd = cmds.back();
        cmds.pop_back();
        Doc* doc = cmd.doc;
        const Ind* ind = cmd.ind;
        const Mode mode = cmd.mode;
        if (doc) {
            switch (doc->kind) {
                case DocKind::String:
                    out += doc->str;
                    if (!cmds.empty()) pos += StringWidth(doc->str);
                    break;
                case DocKind::Array:
                    for (uint32_t i = doc->n; i-- > 0;) cmds.push_back(Cmd{ind, mode, doc->parts[i], 0});
                    break;
                case DocKind::Indent:
                    cmds.push_back(Cmd{MakeIndent(ind), mode, doc->contents, 0});
                    break;
                case DocKind::Align:
                    cmds.push_back(Cmd{MakeAlign(ind, doc), mode, doc->contents, 0});
                    break;
                case DocKind::Trim:
                    pos -= Trim(out);
                    break;
                case DocKind::Group: {
                    bool handled = false;
                    if (mode == kModeFlat && !should_remeasure) {
                        cmds.push_back(Cmd{ind, doc->brk ? kModeBreak : kModeFlat, doc->contents, 0});
                        handled = true;
                    }
                    if (!handled) {
                        should_remeasure = false;
                        const Cmd next{ind, kModeFlat, doc->contents, 0};
                        const int64_t rem = width - pos;
                        const bool has_line_suffix = !line_suffix.empty();
                        if (!doc->brk && Fits(next, cmds, rem, has_line_suffix, false)) {
                            cmds.push_back(next);
                        } else if (doc->expanded) {
                            Doc* most_expanded = doc->n ? doc->parts[doc->n - 1] : nullptr;
                            if (doc->brk) {
                                cmds.push_back(Cmd{ind, kModeBreak, most_expanded, 0});
                            } else {
                                for (uint32_t i = 1; i < doc->n + 1; i++) {
                                    if (i >= doc->n) {
                                        cmds.push_back(Cmd{ind, kModeBreak, most_expanded, 0});
                                        break;
                                    }
                                    const Cmd state{ind, kModeFlat, doc->parts[i], 0};
                                    if (Fits(state, cmds, rem, has_line_suffix, false)) {
                                        cmds.push_back(state);
                                        break;
                                    }
                                }
                            }
                        } else {
                            cmds.push_back(Cmd{ind, kModeBreak, doc->contents, 0});
                        }
                    }
                    if (doc->id) set_group_mode(doc->id, cmds.back().mode);
                    break;
                }
                case DocKind::Fill: {
                    const int64_t rem = width - pos;
                    const uint32_t offset = cmd.fill_off;
                    if (doc->n <= offset) break;
                    const uint32_t length = doc->n - offset;
                    Doc* content = doc->parts[offset];
                    const Cmd content_flat{ind, kModeFlat, content, 0};
                    const Cmd content_break{ind, kModeBreak, content, 0};
                    static const std::vector<Cmd> kNone;
                    const bool content_fits = Fits(content_flat, kNone, rem, !line_suffix.empty(), true);
                    if (length == 1) {
                        cmds.push_back(content_fits ? content_flat : content_break);
                        break;
                    }
                    Doc* whitespace = doc->parts[offset + 1];
                    const Cmd ws_flat{ind, kModeFlat, whitespace, 0};
                    const Cmd ws_break{ind, kModeBreak, whitespace, 0};
                    if (length == 2) {
                        if (content_fits) {
                            cmds.push_back(ws_flat);
                            cmds.push_back(content_flat);
                        } else {
                            cmds.push_back(ws_break);
                            cmds.push_back(content_break);
                        }
                        break;
                    }
                    Doc* second = doc->parts[offset + 2];
                    const Cmd remaining{ind, mode, doc, offset + 2};
                    Doc* pair = arena_.Cat({content, whitespace, second});
                    const Cmd pair_flat{ind, kModeFlat, pair, 0};
                    const bool pair_fits = Fits(pair_flat, kNone, rem, !line_suffix.empty(), true);
                    cmds.push_back(remaining);
                    if (pair_fits) {
                        cmds.push_back(ws_flat);
                        cmds.push_back(content_flat);
                    } else if (content_fits) {
                        cmds.push_back(ws_break);
                        cmds.push_back(content_flat);
                    } else {
                        cmds.push_back(ws_break);
                        cmds.push_back(content_break);
                    }
                    break;
                }
                case DocKind::IfBreak:
                case DocKind::IndentIfBreak: {
                    const Mode group_mode = doc->id ? GroupModeOf(doc->id) : mode;
                    if (group_mode == kModeBreak) {
                        Doc* bc = doc->kind == DocKind::IfBreak ? doc->contents
                                  : doc->negate                 ? doc->contents
                                                                : IndentOf(doc);
                        if (bc) cmds.push_back(Cmd{ind, mode, bc, 0});
                    }
                    if (group_mode == kModeFlat) {
                        Doc* fc = doc->kind == DocKind::IfBreak ? doc->flat
                                  : doc->negate                 ? IndentOf(doc)
                                                                : doc->contents;
                        if (fc) cmds.push_back(Cmd{ind, mode, fc, 0});
                    }
                    break;
                }
                case DocKind::LineSuffix:
                    line_suffix.push_back(Cmd{ind, mode, doc->contents, 0});
                    break;
                case DocKind::LineSuffixBoundary:
                    if (!line_suffix.empty()) cmds.push_back(Cmd{ind, mode, arena_.hardline_nobreak, 0});
                    break;
                case DocKind::Line: {
                    bool do_break = mode == kModeBreak;
                    if (mode == kModeFlat) {
                        if (!doc->hard) {
                            if (!doc->soft) {
                                out += ' ';
                                pos += 1;
                            }
                        } else {
                            should_remeasure = true;
                            do_break = true;
                        }
                    }
                    if (do_break) {
                        if (!line_suffix.empty()) {
                            cmds.push_back(Cmd{ind, mode, doc, 0});
                            for (size_t k = line_suffix.size(); k-- > 0;) cmds.push_back(line_suffix[k]);
                            line_suffix.clear();
                            break;
                        }
                        if (doc->literal) {
                            if (ind->root) {
                                out += '\n';
                                out += ind->root->value;
                                pos = ind->root->length;
                            } else {
                                out += '\n';
                                pos = 0;
                            }
                        } else {
                            pos -= Trim(out);
                            out += '\n';
                            out += ind->value;
                            pos = ind->length;
                        }
                    }
                    break;
                }
                case DocKind::Label:
                    cmds.push_back(Cmd{ind, mode, doc->contents, 0});
                    break;
                case DocKind::BreakParent:
                    break;
            }
        }
        if (cmds.empty() && !line_suffix.empty()) {
            for (size_t k = line_suffix.size(); k-- > 0;) cmds.push_back(line_suffix[k]);
            line_suffix.clear();
        }
    }
    return out;
}

}  // namespace

std::string PrintDocToString(DocArena& arena, Doc* doc, const PrintOptions& opts) {
    LinePrinter p(arena, opts);
    return p.Print(doc);
}

}  // namespace jsfmt
