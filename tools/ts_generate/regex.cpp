// A port of the parts of regex-syntax 0.8.5 that tree-sitter's generator
// uses: ast/parse.rs (the parser), hir/translate.rs (as a recursive
// function rather than its visitor stack machine -- the same result),
// hir/mod.rs's smart constructors, hir/interval.rs and unicode.rs.
#include "regex.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <string_view>

#include "unicode_tables.h"

namespace tsgen::regex {

namespace u = tsgen::unicode;

// ---- interval.rs, for chars --------------------------------------------------

namespace {

constexpr uint32_t kMaxChar = 0x10FFFF;

uint32_t Increment(uint32_t c) { return c == 0xD7FF ? 0xE000 : c + 1; }
uint32_t Decrement(uint32_t c) { return c == 0xE000 ? 0xD7FF : c - 1; }

using Range = std::pair<uint32_t, uint32_t>;

Range Create(uint32_t lower, uint32_t upper) { return lower <= upper ? Range{lower, upper} : Range{upper, lower}; }

bool IsContiguous(const Range &a, const Range &b) {
    return std::max(a.first, b.first) <= std::min(a.second, b.second) + 1;  // as_u32 + saturating_add(1)
}

bool IsIntersectionEmpty(const Range &a, const Range &b) {
    return std::max(a.first, b.first) > std::min(a.second, b.second);
}

bool IsSubset(const Range &a, const Range &b) {
    return (b.first <= a.first && a.first <= b.second) && (b.first <= a.second && a.second <= b.second);
}

std::pair<std::optional<Range>, std::optional<Range>> RangeDifference(const Range &self, const Range &other) {
    if (IsSubset(self, other)) return {std::nullopt, std::nullopt};
    if (IsIntersectionEmpty(self, other)) return {self, std::nullopt};
    bool add_lower = other.first > self.first;
    bool add_upper = other.second < self.second;
    std::pair<std::optional<Range>, std::optional<Range>> ret;
    if (add_lower) ret.first = Create(self.first, Decrement(other.first));
    if (add_upper) {
        Range r = Create(Increment(other.second), self.second);
        if (!ret.first) ret.first = r;
        else ret.second = r;
    }
    return ret;
}

bool IsCanonical(const std::vector<Range> &ranges) {
    for (size_t i = 0; i + 1 < ranges.size(); i++) {
        if (ranges[i] >= ranges[i + 1]) return false;
        if (IsContiguous(ranges[i], ranges[i + 1])) return false;
    }
    return true;
}

}  // namespace

void ClassUnicode::Canonicalize() {
    if (IsCanonical(ranges)) return;
    std::sort(ranges.begin(), ranges.end());
    std::vector<Range> out;
    for (const Range &r : ranges) {
        if (!out.empty() && IsContiguous(out.back(), r)) {
            out.back() = Create(std::min(out.back().first, r.first), std::max(out.back().second, r.second));
            continue;
        }
        out.push_back(r);
    }
    ranges = std::move(out);
}

void ClassUnicode::Union(const ClassUnicode &o) {
    if (o.ranges.empty() || ranges == o.ranges) return;
    ranges.insert(ranges.end(), o.ranges.begin(), o.ranges.end());
    Canonicalize();
}

void ClassUnicode::Intersect(const ClassUnicode &o) {
    if (ranges.empty()) return;
    if (o.ranges.empty()) {
        ranges.clear();
        return;
    }
    std::vector<Range> out;
    size_t a = 0, b = 0;
    while (true) {
        uint32_t lower = std::max(ranges[a].first, o.ranges[b].first);
        uint32_t upper = std::min(ranges[a].second, o.ranges[b].second);
        if (lower <= upper) out.push_back(Create(lower, upper));
        if (ranges[a].second < o.ranges[b].second) {
            if (++a == ranges.size()) break;
        } else {
            if (++b == o.ranges.size()) break;
        }
    }
    ranges = std::move(out);
}

void ClassUnicode::Difference(const ClassUnicode &o) {
    if (ranges.empty() || o.ranges.empty()) return;
    std::vector<Range> out;
    size_t a = 0, b = 0;
    while (a < ranges.size() && b < o.ranges.size()) {
        if (o.ranges[b].second < ranges[a].first) {
            b++;
            continue;
        }
        if (ranges[a].second < o.ranges[b].first) {
            out.push_back(ranges[a]);
            a++;
            continue;
        }
        Range range = ranges[a];
        bool skip = false;
        while (b < o.ranges.size() && !IsIntersectionEmpty(range, o.ranges[b])) {
            Range old_range = range;
            auto [r1, r2] = RangeDifference(range, o.ranges[b]);
            if (!r1 && !r2) {
                a++;
                skip = true;
                break;
            }
            if (r1 && r2) {
                out.push_back(*r1);
                range = *r2;
            } else {
                range = r1 ? *r1 : *r2;
            }
            if (o.ranges[b].second > old_range.second) break;
            b++;
        }
        if (skip) continue;
        out.push_back(range);
        a++;
    }
    while (a < ranges.size()) out.push_back(ranges[a++]);
    ranges = std::move(out);
}

void ClassUnicode::SymmetricDifference(const ClassUnicode &o) {
    ClassUnicode intersection = *this;
    intersection.Intersect(o);
    Union(o);
    Difference(intersection);
}

void ClassUnicode::Negate() {
    if (ranges.empty()) {
        ranges.push_back({0, kMaxChar});
        return;
    }
    std::vector<Range> out;
    if (ranges[0].first > 0) out.push_back(Create(0, Decrement(ranges[0].first)));
    for (size_t i = 1; i < ranges.size(); i++)
        out.push_back(Create(Increment(ranges[i - 1].second), Decrement(ranges[i].first)));
    if (ranges.back().second < kMaxChar) out.push_back(Create(Increment(ranges.back().second), kMaxChar));
    ranges = std::move(out);
}

namespace {

const u::Fold *FindFold(uint32_t c) {
    const u::Fold *begin = std::begin(u::kFolds), *end = std::end(u::kFolds);
    const u::Fold *it = std::lower_bound(begin, end, c, [](const u::Fold &f, uint32_t v) { return f.c < v; });
    return it != end && it->c == c ? it : nullptr;
}

bool FoldOverlaps(uint32_t start, uint32_t end) {
    const u::Fold *begin = std::begin(u::kFolds), *e = std::end(u::kFolds);
    const u::Fold *it = std::lower_bound(begin, e, start, [](const u::Fold &f, uint32_t v) { return f.c < v; });
    return it != e && it->c <= end;
}

}  // namespace

void ClassUnicode::CaseFoldSimple() {
    size_t len = ranges.size();
    for (size_t i = 0; i < len; i++) {
        Range r = ranges[i];
        if (!FoldOverlaps(r.first, r.second)) continue;
        for (uint32_t cp = r.first; cp <= r.second; cp++) {
            if (cp >= 0xD800 && cp <= 0xDFFF) continue;
            if (const u::Fold *f = FindFold(cp))
                for (uint32_t k = 0; k < f->count; k++) {
                    uint32_t t = u::kFoldTargets[f->offset + k];
                    ranges.push_back({t, t});
                }
        }
    }
    Canonicalize();
}

namespace {

ClassUnicode TableClass(const u::Table &t) {
    ClassUnicode c;
    for (uint32_t i = 0; i < t.count; i++) c.ranges.push_back({u::kRanges[t.offset + i].lo, u::kRanges[t.offset + i].hi});
    c.Canonicalize();
    return c;
}

template <size_t N>
const u::Table *FindTable(const u::Table (&tables)[N], std::string_view name) {
    for (const u::Table &t : tables)
        if (name == t.name) return &t;
    return nullptr;
}

bool InTable(const u::Table &t, uint32_t c) {
    const u::Range *begin = u::kRanges + t.offset, *end = begin + t.count;
    const u::Range *it = std::lower_bound(begin, end, c, [](const u::Range &r, uint32_t v) { return r.hi < v; });
    return it != end && it->lo <= c;
}

const u::Table &AlphabeticTable() {
    static const u::Table *t = FindTable(u::kPropertyBool, "Alphabetic");
    return *t;
}

bool IsWhitespace(uint32_t c) { return InTable(u::kPerlSpace, c); }

bool IsAlphanumeric(uint32_t c) {
    if (InTable(AlphabeticTable(), c)) return true;
    for (const char *cat : {"Decimal_Number", "Letter_Number", "Other_Number"})
        if (InTable(*FindTable(u::kGeneralCategory, cat), c)) return true;
    return false;
}

}  // namespace

bool IsAlphabetic(uint32_t c) { return InTable(AlphabeticTable(), c); }

// ---- unicode.rs's class queries ---------------------------------------------

namespace {

std::string SymbolicNameNormalize(const std::string &x) {
    std::string slice = x;
    size_t start = 0;
    bool starts_with_is = false;
    if (slice.size() >= 2) {
        std::string first = slice.substr(0, 2);
        starts_with_is = first == "is" || first == "IS" || first == "iS" || first == "Is";
        if (starts_with_is) start = 2;
    }
    std::string out;
    for (size_t i = start; i < slice.size(); i++) {
        unsigned char b = static_cast<unsigned char>(slice[i]);
        if (b == ' ' || b == '_' || b == '-') continue;
        if (b >= 'A' && b <= 'Z') out += static_cast<char>(b + ('a' - 'A'));
        else if (b <= 0x7F) out += static_cast<char>(b);
    }
    if (starts_with_is && out == "c") out = "isc";
    return out;
}

const char *CanonicalPropName(const std::string &normalized) {
    for (const u::Name &n : u::kPropertyNames)
        if (normalized == n.normalized) return n.canonical;
    return nullptr;
}

const u::Values *PropertyValues(std::string_view property) {
    for (const u::Values &v : u::kPropertyValues)
        if (property == v.property) return &v;
    return nullptr;
}

const char *CanonicalValue(const u::Values &vals, const std::string &normalized) {
    for (uint32_t i = 0; i < vals.count; i++)
        if (normalized == vals.names[i].normalized) return vals.names[i].canonical;
    return nullptr;
}

const char *CanonicalGencat(const std::string &normalized) {
    if (normalized == "any") return "Any";
    if (normalized == "assigned") return "Assigned";
    if (normalized == "ascii") return "ASCII";
    return CanonicalValue(*PropertyValues("General_Category"), normalized);
}

const char *CanonicalScript(const std::string &normalized) { return CanonicalValue(*PropertyValues("Script"), normalized); }

bool Gencat(std::string_view name, ClassUnicode &out) {
    if (name == "Decimal_Number") {
        out = TableClass(u::kPerlDecimal);
        return true;
    }
    if (name == "ASCII") {
        out = ClassUnicode{{{0, 0x7F}}};
        return true;
    }
    if (name == "Any") {
        out = ClassUnicode{{{0, kMaxChar}}};
        return true;
    }
    if (name == "Assigned") {
        if (!Gencat("Unassigned", out)) return false;
        out.Negate();
        return true;
    }
    const u::Table *t = FindTable(u::kGeneralCategory, name);
    if (!t) return false;
    out = TableClass(*t);
    return true;
}

bool BoolProperty(std::string_view name, ClassUnicode &out) {
    if (name == "Decimal_Number") {
        out = TableClass(u::kPerlDecimal);
        return true;
    }
    if (name == "White_Space") {
        out = TableClass(u::kPerlSpace);
        return true;
    }
    const u::Table *t = FindTable(u::kPropertyBool, name);
    if (!t) return false;
    out = TableClass(*t);
    return true;
}

template <size_t N>
bool ByName(const u::Table (&tables)[N], std::string_view name, ClassUnicode &out) {
    const u::Table *t = FindTable(tables, name);
    if (!t) return false;
    out = TableClass(*t);
    return true;
}

// unicode::class: OneLetter/Binary(name) when `value` is null, else ByValue.
bool UnicodeClass(const std::string &name, const std::string *value, ClassUnicode &out, std::string &error) {
    if (!value) {
        std::string norm = SymbolicNameNormalize(name);
        if (norm != "cf" && norm != "sc" && norm != "lc") {
            if (const char *canon = CanonicalPropName(norm)) {
                if (BoolProperty(canon, out)) return true;
                error = "Unicode property not found";
                return false;
            }
        }
        if (const char *canon = CanonicalGencat(norm)) {
            if (Gencat(canon, out)) return true;
            error = "Unicode property value not found";
            return false;
        }
        if (const char *canon = CanonicalScript(norm)) {
            if (ByName(u::kScript, canon, out)) return true;
            error = "Unicode property value not found";
            return false;
        }
        error = "Unicode property not found";
        return false;
    }
    std::string prop = SymbolicNameNormalize(name);
    std::string val = SymbolicNameNormalize(*value);
    const char *canon_name = CanonicalPropName(prop);
    if (!canon_name) {
        error = "Unicode property not found";
        return false;
    }
    std::string_view cn = canon_name;
    if (cn == "General_Category") {
        const char *canon = CanonicalGencat(val);
        if (!canon || !Gencat(canon, out)) {
            error = "Unicode property value not found";
            return false;
        }
        return true;
    }
    if (cn == "Script") {
        const char *canon = CanonicalScript(val);
        if (!canon || !ByName(u::kScript, canon, out)) {
            error = "Unicode property value not found";
            return false;
        }
        return true;
    }
    const u::Values *vals = PropertyValues(cn);
    const char *canon_val = vals ? CanonicalValue(*vals, val) : nullptr;
    if (!canon_val) {
        error = "Unicode property value not found";
        return false;
    }
    if (cn == "Age") {
        out = ClassUnicode{};
        bool found = false;
        for (const u::Table &t : u::kAge) {
            ClassUnicode c = TableClass(t);
            out.Union(c);
            if (std::string_view(t.name) == canon_val) {
                found = true;
                break;
            }
        }
        if (!found) error = "Unicode property value not found";
        return found;
    }
    bool ok = false;
    if (cn == "Script_Extensions") ok = ByName(u::kScriptExtension, canon_val, out);
    else if (cn == "Grapheme_Cluster_Break") ok = ByName(u::kGraphemeClusterBreak, canon_val, out);
    else if (cn == "Sentence_Break") ok = ByName(u::kSentenceBreak, canon_val, out);
    else if (cn == "Word_Break") ok = ByName(u::kWordBreak, canon_val, out);
    else {
        error = "Unicode property not found";
        return false;
    }
    if (!ok) error = "Unicode property value not found";
    return ok;
}

}  // namespace

// ---- hir/mod.rs's smart constructors ----------------------------------------

namespace {

size_t Utf8Len(uint32_t c) { return c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4; }

std::string Utf8(uint32_t c) {
    std::string out;
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
    return out;
}

// The first char of `s` if all of `s` is exactly one UTF-8 char.
std::optional<uint32_t> SingleChar(const std::string &s) {
    if (s.empty()) return std::nullopt;
    unsigned char b0 = static_cast<unsigned char>(s[0]);
    size_t n = b0 < 0x80 ? 1 : (b0 >> 5) == 6 ? 2 : (b0 >> 4) == 14 ? 3 : (b0 >> 3) == 30 ? 4 : 0;
    if (n == 0 || s.size() != n) return std::nullopt;
    uint32_t c = n == 1 ? b0 : n == 2 ? (b0 & 0x1F) : n == 3 ? (b0 & 0x0F) : (b0 & 0x07);
    for (size_t i = 1; i < n; i++) c = (c << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
    return c;
}

Hir Empty() {
    Hir h;
    h.minimum_len = 0;
    h.maximum_len = 0;
    return h;
}

Hir Literal(std::string bytes) {
    if (bytes.empty()) return Empty();
    Hir h;
    h.kind = Hir::Kind::Literal;
    h.minimum_len = h.maximum_len = bytes.size();
    h.literal = std::move(bytes);
    return h;
}

Hir Class(ClassUnicode cls) {
    if (cls.ranges.empty()) {
        // Hir::fail(): an empty byte class; tree-sitter turns it into the
        // same empty character set an empty Unicode class would be.
        Hir h;
        h.kind = Hir::Kind::Class;
        return h;
    }
    if (cls.ranges.size() == 1 && cls.ranges[0].first == cls.ranges[0].second) return Literal(Utf8(cls.ranges[0].first));
    Hir h;
    h.kind = Hir::Kind::Class;
    h.minimum_len = Utf8Len(cls.ranges.front().first);
    h.maximum_len = Utf8Len(cls.ranges.back().second);
    h.cls = std::move(cls);
    return h;
}

Hir Look() {
    Hir h;
    h.kind = Hir::Kind::Look;
    h.minimum_len = h.maximum_len = 0;
    return h;
}

Hir Repetition(uint32_t min, std::optional<uint32_t> max, bool greedy, Hir sub) {
    if (sub.maximum_len == std::optional<size_t>(0)) {
        min = std::min<uint32_t>(min, 1);
        max = max ? std::min<uint32_t>(*max, 1) : 1;
    }
    if (min == 0 && max == std::optional<uint32_t>(0)) return Empty();
    if (min == 1 && max == std::optional<uint32_t>(1)) return sub;
    Hir h;
    h.kind = Hir::Kind::Repetition;
    if (sub.minimum_len) h.minimum_len = *sub.minimum_len * min;  // saturating: never large here
    if (max && sub.maximum_len) h.maximum_len = *sub.maximum_len * *max;
    h.min = min;
    h.max = max;
    h.greedy = greedy;
    h.subs.push_back(std::move(sub));
    return h;
}

Hir Capture(uint32_t index, std::optional<std::string> name, Hir sub) {
    Hir h;
    h.kind = Hir::Kind::Capture;
    h.minimum_len = sub.minimum_len;
    h.maximum_len = sub.maximum_len;
    h.capture_index = index;
    h.capture_name = std::move(name);
    h.subs.push_back(std::move(sub));
    return h;
}

void ConcatProps(Hir &h) {
    std::optional<size_t> mn = 0, mx = 0;
    for (const Hir &x : h.subs) {
        if (mn) mn = x.minimum_len ? std::optional<size_t>(*mn + *x.minimum_len) : std::nullopt;
        if (mx) mx = x.maximum_len ? std::optional<size_t>(*mx + *x.maximum_len) : std::nullopt;
    }
    h.minimum_len = mn;
    h.maximum_len = mx;
}

Hir Concat(std::vector<Hir> subs) {
    std::vector<Hir> out;
    std::optional<std::string> prior;
    auto flush = [&] {
        if (prior) out.push_back(Literal(std::move(*prior))), prior.reset();
    };
    auto take = [&](Hir &&sub) {
        if (sub.kind == Hir::Kind::Literal) {
            if (prior) *prior += sub.literal;
            else prior = sub.literal;
        } else {
            flush();
            out.push_back(std::move(sub));
        }
    };
    for (Hir &sub : subs) {
        if (sub.kind == Hir::Kind::Concat) {
            for (Hir &sub2 : sub.subs) take(std::move(sub2));
        } else if (sub.kind != Hir::Kind::Empty) {
            take(std::move(sub));
        }
    }
    flush();
    if (out.empty()) return Empty();
    if (out.size() == 1) return std::move(out[0]);
    Hir h;
    h.kind = Hir::Kind::Concat;
    h.subs = std::move(out);
    ConcatProps(h);
    return h;
}

Hir Alternation(std::vector<Hir> subs);

Hir LiftCommonPrefixOrBuild(std::vector<Hir> hirs) {
    auto build = [](std::vector<Hir> alts) {
        Hir h;
        h.kind = Hir::Kind::Alternation;
        std::optional<size_t> mn, mx;
        bool min_poisoned = false, max_poisoned = false;
        for (const Hir &p : alts) {
            if (!min_poisoned) {
                if (p.minimum_len) {
                    if (!mn || *p.minimum_len < *mn) mn = p.minimum_len;
                } else {
                    mn.reset();
                    min_poisoned = true;
                }
            }
            if (!max_poisoned) {
                if (p.maximum_len) {
                    if (!mx || *p.maximum_len > *mx) mx = p.maximum_len;
                } else {
                    mx.reset();
                    max_poisoned = true;
                }
            }
        }
        h.minimum_len = mn;
        h.maximum_len = mx;
        h.subs = std::move(alts);
        return h;
    };
    if (hirs.size() <= 1 || hirs[0].kind != Hir::Kind::Concat || hirs[0].subs.empty()) return build(std::move(hirs));
    size_t prefix_len = hirs[0].subs.size();
    for (size_t k = 1; k < hirs.size(); k++) {
        if (hirs[k].kind != Hir::Kind::Concat) return build(std::move(hirs));
        size_t common = 0;
        while (common < prefix_len && common < hirs[k].subs.size() && hirs[0].subs[common] == hirs[k].subs[common]) common++;
        prefix_len = common;
        if (prefix_len == 0) return build(std::move(hirs));
    }
    std::vector<Hir> prefix_concat;
    std::vector<Hir> suffix_alts;
    for (Hir &h : hirs) {
        std::vector<Hir> suffix(std::make_move_iterator(h.subs.begin() + static_cast<long>(prefix_len)),
                                std::make_move_iterator(h.subs.end()));
        h.subs.resize(prefix_len);
        suffix_alts.push_back(Concat(std::move(suffix)));
        if (prefix_concat.empty()) prefix_concat = std::move(h.subs);
    }
    prefix_concat.push_back(Alternation(std::move(suffix_alts)));
    return Concat(std::move(prefix_concat));
}

Hir Alternation(std::vector<Hir> subs) {
    std::vector<Hir> out;
    for (Hir &sub : subs) {
        if (sub.kind == Hir::Kind::Alternation) {
            for (Hir &s2 : sub.subs) out.push_back(std::move(s2));
        } else {
            out.push_back(std::move(sub));
        }
    }
    if (out.empty()) return Class(ClassUnicode{});  // Hir::fail()
    if (out.size() == 1) return std::move(out[0]);
    // singleton_chars: every alternative a one-char literal.
    {
        ClassUnicode cls;
        bool all = true;
        for (const Hir &h : out) {
            std::optional<uint32_t> c;
            if (h.kind == Hir::Kind::Literal) c = SingleChar(h.literal);
            if (!c) {
                all = false;
                break;
            }
            cls.ranges.push_back({*c, *c});
        }
        if (all) {
            cls.Canonicalize();
            return Class(std::move(cls));
        }
    }
    // class_chars: every alternative a class.
    {
        ClassUnicode cls;
        bool all = true;
        for (const Hir &h : out) {
            if (h.kind != Hir::Kind::Class) {
                all = false;
                break;
            }
            cls.Union(h.cls);
        }
        if (all) return Class(std::move(cls));
    }
    return LiftCommonPrefixOrBuild(std::move(out));
}

}  // namespace

bool Hir::operator==(const Hir &o) const {
    if (kind != o.kind) return false;
    switch (kind) {
        case Kind::Empty:
        case Kind::Look:
            return true;
        case Kind::Literal:
            return literal == o.literal;
        case Kind::Class:
            return cls == o.cls;
        case Kind::Repetition:
            return min == o.min && max == o.max && greedy == o.greedy && subs == o.subs;
        case Kind::Capture:
            return capture_index == o.capture_index && capture_name == o.capture_name && subs == o.subs;
        case Kind::Concat:
        case Kind::Alternation:
            return subs == o.subs;
    }
    return false;
}

// ---- ast/parse.rs -----------------------------------------------------------

namespace {

struct Flags {
    std::optional<bool> case_insensitive, multi_line, dot_matches_new_line, swap_greed, unicode, crlf;
    void Merge(const Flags &prev) {
        if (!case_insensitive) case_insensitive = prev.case_insensitive;
        if (!multi_line) multi_line = prev.multi_line;
        if (!dot_matches_new_line) dot_matches_new_line = prev.dot_matches_new_line;
        if (!swap_greed) swap_greed = prev.swap_greed;
        if (!unicode) unicode = prev.unicode;
        if (!crlf) crlf = prev.crlf;
    }
};

struct AstFlagItem {
    enum class Kind : uint8_t { Negation, CaseInsensitive, MultiLine, DotMatchesNewLine, SwapGreed, Unicode, CRLF, IgnoreWhitespace } kind;
};

enum class ClassPerlKind : uint8_t { Digit, Space, Word };
enum class AsciiKind : uint8_t { Alnum, Alpha, Ascii, Blank, Cntrl, Digit, Graph, Lower, Print, Punct, Space, Upper, Word, Xdigit };

struct UnicodeQuery {
    bool negated = false;
    std::string name;
    std::optional<std::string> value;  // NamedValue
    bool not_equal = false;            // `!=`: the value's class negated
};

struct ClassSet;

struct ClassSetItem {
    enum class Kind : uint8_t { Empty, Literal, Range, Ascii, Unicode, Perl, Bracketed, Union } kind = Kind::Empty;
    uint32_t c = 0, end = 0;
    AsciiKind ascii{};
    ClassPerlKind perl{};
    bool negated = false;  // Ascii, Perl
    UnicodeQuery unicode;
    std::shared_ptr<ClassSet> bracketed;  // the bracket's set; negated above
    std::vector<ClassSetItem> items;     // Union
};

struct ClassSet {
    enum class Kind : uint8_t { Item, BinaryOp } kind = Kind::Item;
    ClassSetItem item;
    enum class Op : uint8_t { Intersection, Difference, SymmetricDifference } op{};
    std::shared_ptr<ClassSet> lhs, rhs;
};

struct Ast {
    enum class Kind : uint8_t {
        Empty, Flags, Literal, Dot, Assertion, ClassUnicode, ClassPerl, ClassBracketed, Repetition, Group, Alternation, Concat
    } kind = Kind::Empty;
    uint32_t c = 0;                      // Literal
    std::vector<AstFlagItem> flags;      // Flags; a non-capturing group's flags
    UnicodeQuery unicode;                // ClassUnicode
    ClassPerlKind perl{};                // ClassPerl
    bool negated = false;                // ClassPerl, ClassBracketed
    std::shared_ptr<ClassSet> set;       // ClassBracketed
    uint32_t min = 0;                    // Repetition
    std::optional<uint32_t> max;
    bool greedy = true;
    enum class GroupKind : uint8_t { CaptureIndex, CaptureName, NonCapturing } group_kind{};
    uint32_t capture_index = 0;
    std::string capture_name;
    std::vector<Ast> asts;  // Repetition/Group: one; Alternation/Concat: several
};

Ast ConcatIntoAst(std::vector<Ast> asts) {
    if (asts.empty()) return Ast{};
    if (asts.size() == 1) return std::move(asts[0]);
    Ast a;
    a.kind = Ast::Kind::Concat;
    a.asts = std::move(asts);
    return a;
}

Ast AlternationIntoAst(std::vector<Ast> asts) {
    if (asts.empty()) return Ast{};
    if (asts.size() == 1) return std::move(asts[0]);
    Ast a;
    a.kind = Ast::Kind::Alternation;
    a.asts = std::move(asts);
    return a;
}

ClassSetItem UnionIntoItem(std::vector<ClassSetItem> items) {
    if (items.empty()) return ClassSetItem{};
    if (items.size() == 1) return std::move(items[0]);
    ClassSetItem u;
    u.kind = ClassSetItem::Kind::Union;
    u.items = std::move(items);
    return u;
}

bool IsMetaCharacter(uint32_t c) {
    switch (c) {
        case '\\': case '.': case '+': case '*': case '?': case '(': case ')': case '|': case '[': case ']':
        case '{': case '}': case '^': case '$': case '#': case '&': case '-': case '~':
            return true;
        default:
            return false;
    }
}

bool IsEscapeableCharacter(uint32_t c) {
    if (IsMetaCharacter(c)) return true;
    if (c >= 0x80) return false;
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return false;
    if (c == '<' || c == '>') return false;
    return true;
}

bool IsHex(uint32_t c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

std::optional<AsciiKind> AsciiFromName(const std::u32string &name) {
    static const std::pair<const char *, AsciiKind> names[] = {
        {"alnum", AsciiKind::Alnum}, {"alpha", AsciiKind::Alpha}, {"ascii", AsciiKind::Ascii}, {"blank", AsciiKind::Blank},
        {"cntrl", AsciiKind::Cntrl}, {"digit", AsciiKind::Digit}, {"graph", AsciiKind::Graph}, {"lower", AsciiKind::Lower},
        {"print", AsciiKind::Print}, {"punct", AsciiKind::Punct}, {"space", AsciiKind::Space}, {"upper", AsciiKind::Upper},
        {"word", AsciiKind::Word},   {"xdigit", AsciiKind::Xdigit}};
    for (const auto &[n, k] : names) {
        std::string_view sv = n;
        if (sv.size() != name.size()) continue;
        bool eq = true;
        for (size_t i = 0; i < sv.size(); i++)
            if (static_cast<uint32_t>(sv[i]) != name[i]) eq = false;
        if (eq) return k;
    }
    return std::nullopt;
}

struct ParseError {
    std::string message;
};

class Parser {
public:
    explicit Parser(std::u32string p) : pattern_(std::move(p)) {}

    Ast Parse() {
        std::vector<Ast> concat;
        while (true) {
            BumpSpace();
            if (IsEof()) break;
            switch (Char()) {
                case '(':
                    PushGroup(concat);
                    break;
                case ')':
                    PopGroup(concat);
                    break;
                case '|':
                    PushAlternate(concat);
                    break;
                case '[': {
                    Ast a = ParseSetClass();
                    concat.push_back(std::move(a));
                    break;
                }
                case '?':
                    ParseUncountedRepetition(concat, 0, 1);
                    break;
                case '*':
                    ParseUncountedRepetition(concat, 0, std::nullopt);
                    break;
                case '+':
                    ParseUncountedRepetition(concat, 1, std::nullopt);
                    break;
                case '{':
                    ParseCountedRepetition(concat);
                    break;
                default:
                    concat.push_back(PrimitiveIntoAst(ParsePrimitive()));
            }
        }
        return PopGroupEnd(std::move(concat));
    }

private:
    struct GroupState {
        bool is_alternation = false;
        std::vector<Ast> concat;  // Group: the concat before the group
        Ast group;                // Group
        bool ignore_whitespace = false;
        std::vector<Ast> alts;    // Alternation
    };

    struct ClassState {
        bool is_op = false;
        std::vector<ClassSetItem> union_items;  // Open: the parent union
        bool negated = false;                   // Open: the bracket being parsed
        ClassSet::Op op{};                      // Op
        std::shared_ptr<ClassSet> lhs;          // Op
    };

    struct Primitive {
        enum class Kind : uint8_t { Literal, Assertion, Dot, Perl, Unicode } kind = Kind::Literal;
        uint32_t c = 0;
        ClassPerlKind perl{};
        bool negated = false;
        UnicodeQuery unicode;
    };

    std::u32string pattern_;
    size_t pos_ = 0;
    uint32_t capture_index_ = 0;
    bool ignore_whitespace_ = false;
    std::vector<GroupState> stack_group_;
    std::vector<ClassState> stack_class_;
    std::vector<std::u32string> capture_names_;

    [[noreturn]] void Error(const std::string &m) { throw ParseError{m}; }
    bool IsEof() const { return pos_ == pattern_.size(); }
    uint32_t Char() const { return pattern_[pos_]; }
    bool Bump() {
        if (IsEof()) return false;
        pos_++;
        return !IsEof();
    }
    bool BumpIf(const char *prefix) {
        std::string_view p = prefix;
        if (pos_ + p.size() > pattern_.size()) return false;
        for (size_t i = 0; i < p.size(); i++)
            if (pattern_[pos_ + i] != static_cast<uint32_t>(p[i])) return false;
        pos_ += p.size();
        return true;
    }
    bool IsLookaroundPrefix() { return BumpIf("?=") || BumpIf("?!") || BumpIf("?<=") || BumpIf("?<!"); }
    bool BumpAndBumpSpace() {
        if (!Bump()) return false;
        BumpSpace();
        return !IsEof();
    }
    void BumpSpace() {
        if (!ignore_whitespace_) return;
        while (!IsEof()) {
            if (IsWhitespace(Char())) {
                Bump();
            } else if (Char() == '#') {
                Bump();
                while (!IsEof()) {
                    uint32_t c = Char();
                    Bump();
                    if (c == '\n') break;
                }
            } else {
                break;
            }
        }
    }
    std::optional<uint32_t> Peek() const {
        if (IsEof() || pos_ + 1 >= pattern_.size()) return std::nullopt;
        return pattern_[pos_ + 1];
    }
    std::optional<uint32_t> PeekSpace() const {
        if (!ignore_whitespace_) return Peek();
        if (IsEof()) return std::nullopt;
        size_t start = pos_ + 1;
        bool in_comment = false;
        size_t i = start;
        for (; i < pattern_.size(); i++) {
            uint32_t c = pattern_[i];
            if (IsWhitespace(c)) continue;
            if (!in_comment && c == '#') in_comment = true;
            else if (in_comment && c == '\n') in_comment = false;
            else break;
        }
        if (i >= pattern_.size()) return std::nullopt;
        return pattern_[i];
    }

    static Ast PrimitiveIntoAst(Primitive p) {
        Ast a;
        switch (p.kind) {
            case Primitive::Kind::Literal:
                a.kind = Ast::Kind::Literal;
                a.c = p.c;
                break;
            case Primitive::Kind::Assertion:
                a.kind = Ast::Kind::Assertion;
                break;
            case Primitive::Kind::Dot:
                a.kind = Ast::Kind::Dot;
                break;
            case Primitive::Kind::Perl:
                a.kind = Ast::Kind::ClassPerl;
                a.perl = p.perl;
                a.negated = p.negated;
                break;
            case Primitive::Kind::Unicode:
                a.kind = Ast::Kind::ClassUnicode;
                a.unicode = std::move(p.unicode);
                break;
        }
        return a;
    }

    void PushAlternate(std::vector<Ast> &concat) {
        Ast item = ConcatIntoAst(std::move(concat));
        concat.clear();
        if (!stack_group_.empty() && stack_group_.back().is_alternation) {
            stack_group_.back().alts.push_back(std::move(item));
        } else {
            GroupState s;
            s.is_alternation = true;
            s.alts.push_back(std::move(item));
            stack_group_.push_back(std::move(s));
        }
        Bump();
    }

    void PushGroup(std::vector<Ast> &concat) {
        bool is_set_flags = false;
        Ast group = ParseGroup(is_set_flags);
        if (is_set_flags) {
            for (const AstFlagItem &f : group.flags) (void)f;
            std::optional<bool> ignore = FlagState(group.flags, AstFlagItem::Kind::IgnoreWhitespace);
            if (ignore) ignore_whitespace_ = *ignore;
            group.kind = Ast::Kind::Flags;
            concat.push_back(std::move(group));
            return;
        }
        bool old_ignore = ignore_whitespace_;
        bool new_ignore = old_ignore;
        if (group.group_kind == Ast::GroupKind::NonCapturing)
            if (auto v = FlagState(group.flags, AstFlagItem::Kind::IgnoreWhitespace)) new_ignore = *v;
        GroupState s;
        s.concat = std::move(concat);
        s.group = std::move(group);
        s.ignore_whitespace = old_ignore;
        stack_group_.push_back(std::move(s));
        ignore_whitespace_ = new_ignore;
        concat.clear();
    }

    void PopGroup(std::vector<Ast> &group_concat) {
        if (stack_group_.empty()) Error("unopened group");
        GroupState top = std::move(stack_group_.back());
        stack_group_.pop_back();
        std::optional<std::vector<Ast>> alt;
        GroupState group_state;
        if (top.is_alternation) {
            if (stack_group_.empty() || stack_group_.back().is_alternation) Error("unopened group");
            alt = std::move(top.alts);
            group_state = std::move(stack_group_.back());
            stack_group_.pop_back();
        } else {
            group_state = std::move(top);
        }
        ignore_whitespace_ = group_state.ignore_whitespace;
        Bump();
        Ast group = std::move(group_state.group);
        group.kind = Ast::Kind::Group;
        if (alt) {
            alt->push_back(ConcatIntoAst(std::move(group_concat)));
            group.asts = {AlternationIntoAst(std::move(*alt))};
        } else {
            group.asts = {ConcatIntoAst(std::move(group_concat))};
        }
        std::vector<Ast> prior = std::move(group_state.concat);
        prior.push_back(std::move(group));
        group_concat = std::move(prior);
    }

    Ast PopGroupEnd(std::vector<Ast> concat) {
        Ast ast;
        if (stack_group_.empty()) {
            return ConcatIntoAst(std::move(concat));
        }
        GroupState top = std::move(stack_group_.back());
        stack_group_.pop_back();
        if (!top.is_alternation) Error("unclosed group");
        top.alts.push_back(ConcatIntoAst(std::move(concat)));
        ast = AlternationIntoAst(std::move(top.alts));
        if (!stack_group_.empty()) Error("unclosed group");
        return ast;
    }

    static std::optional<bool> FlagState(const std::vector<AstFlagItem> &items, AstFlagItem::Kind flag) {
        bool negated = false;
        for (const AstFlagItem &x : items) {
            if (x.kind == AstFlagItem::Kind::Negation) negated = true;
            else if (x.kind == flag) return !negated;
        }
        return std::nullopt;
    }

    void ParseUncountedRepetition(std::vector<Ast> &concat, uint32_t min, std::optional<uint32_t> max) {
        if (concat.empty()) Error("repetition operator missing expression");
        Ast ast = std::move(concat.back());
        concat.pop_back();
        if (ast.kind == Ast::Kind::Empty || ast.kind == Ast::Kind::Flags) Error("repetition operator missing expression");
        bool greedy = true;
        if (Bump() && Char() == '?') {
            greedy = false;
            Bump();
        }
        Ast rep;
        rep.kind = Ast::Kind::Repetition;
        rep.min = min;
        rep.max = max;
        rep.greedy = greedy;
        rep.asts.push_back(std::move(ast));
        concat.push_back(std::move(rep));
    }

    // parse_decimal: Err(true) when there were no digits.
    std::optional<uint32_t> ParseDecimal(bool &empty) {
        std::string digits;
        while (!IsEof() && IsWhitespace(Char())) Bump();
        while (!IsEof() && Char() >= '0' && Char() <= '9') {
            digits += static_cast<char>(Char());
            BumpAndBumpSpace();
        }
        while (!IsEof() && IsWhitespace(Char())) BumpAndBumpSpace();
        empty = digits.empty();
        if (empty) return std::nullopt;
        if (digits.size() > 10) Error("repetition count is not a valid decimal");
        uint64_t v = std::stoull(digits);
        if (v > UINT32_MAX) Error("repetition count is not a valid decimal");
        return static_cast<uint32_t>(v);
    }

    void ParseCountedRepetition(std::vector<Ast> &concat) {
        if (concat.empty()) Error("repetition operator missing expression");
        Ast ast = std::move(concat.back());
        concat.pop_back();
        if (ast.kind == Ast::Kind::Empty || ast.kind == Ast::Kind::Flags) Error("repetition operator missing expression");
        if (!BumpAndBumpSpace()) Error("unclosed counted repetition");
        bool start_empty = false;
        std::optional<uint32_t> count_start = ParseDecimal(start_empty);
        if (IsEof()) Error("unclosed counted repetition");
        uint32_t min = 0;
        std::optional<uint32_t> max;
        if (Char() == ',') {
            if (!BumpAndBumpSpace()) Error("unclosed counted repetition");
            if (Char() != '}') {
                if (!count_start) Error("repetition quantifier expects a valid decimal");  // empty_min_range is off
                bool end_empty = false;
                std::optional<uint32_t> count_end = ParseDecimal(end_empty);
                if (!count_end) Error("repetition quantifier expects a valid decimal");
                min = *count_start;
                max = *count_end;
            } else {
                if (!count_start) Error("repetition quantifier expects a valid decimal");
                min = *count_start;
            }
        } else {
            if (!count_start) Error("repetition quantifier expects a valid decimal");
            min = *count_start;
            max = *count_start;
        }
        if (IsEof() || Char() != '}') Error("unclosed counted repetition");
        bool greedy = true;
        if (BumpAndBumpSpace() && Char() == '?') {
            greedy = false;
            Bump();
        }
        if (max && min > *max) Error("invalid repetition count range");
        Ast rep;
        rep.kind = Ast::Kind::Repetition;
        rep.min = min;
        rep.max = max;
        rep.greedy = greedy;
        rep.asts.push_back(std::move(ast));
        concat.push_back(std::move(rep));
    }

    Ast ParseGroup(bool &is_set_flags) {
        Bump();
        BumpSpace();
        if (IsLookaroundPrefix()) Error("look-around, including look-ahead and look-behind, is not supported");
        Ast group;
        if (BumpIf("?P<") || BumpIf("?<")) {
            uint32_t index = NextCaptureIndex();
            group.group_kind = Ast::GroupKind::CaptureName;
            group.capture_index = index;
            group.capture_name = ParseCaptureName();
            return group;
        }
        if (BumpIf("?")) {
            if (IsEof()) Error("unclosed group");
            std::vector<AstFlagItem> flags = ParseFlags();
            uint32_t char_end = Char();
            Bump();
            if (char_end == ')') {
                if (flags.empty()) Error("repetition operator missing expression");
                is_set_flags = true;
                group.flags = std::move(flags);
                return group;
            }
            group.group_kind = Ast::GroupKind::NonCapturing;
            group.flags = std::move(flags);
            return group;
        }
        group.group_kind = Ast::GroupKind::CaptureIndex;
        group.capture_index = NextCaptureIndex();
        return group;
    }

    uint32_t NextCaptureIndex() { return ++capture_index_; }

    std::string ParseCaptureName() {
        if (IsEof()) Error("unexpected end of group name");
        size_t start = pos_;
        while (true) {
            if (Char() == '>') break;
            uint32_t c = Char();
            bool first = pos_ == start;
            bool ok = first ? (c == '_' || IsAlphabetic(c))
                            : (c == '_' || c == '.' || c == '[' || c == ']' || IsAlphanumeric(c));
            if (!ok) Error("invalid capture group character");
            if (!Bump()) break;
        }
        size_t end = pos_;
        if (IsEof()) Error("unexpected end of group name");
        Bump();
        std::u32string name = pattern_.substr(start, end - start);
        if (name.empty()) Error("empty capture group name");
        for (const std::u32string &n : capture_names_)
            if (n == name) Error("duplicate capture group name");
        capture_names_.push_back(name);
        std::string out;
        for (uint32_t c : name) out += Utf8(c);
        return out;
    }

    std::vector<AstFlagItem> ParseFlags() {
        std::vector<AstFlagItem> items;
        bool last_was_negation = false;
        while (Char() != ':' && Char() != ')') {
            AstFlagItem item{};
            if (Char() == '-') {
                last_was_negation = true;
                item.kind = AstFlagItem::Kind::Negation;
            } else {
                last_was_negation = false;
                switch (Char()) {
                    case 'i': item.kind = AstFlagItem::Kind::CaseInsensitive; break;
                    case 'm': item.kind = AstFlagItem::Kind::MultiLine; break;
                    case 's': item.kind = AstFlagItem::Kind::DotMatchesNewLine; break;
                    case 'U': item.kind = AstFlagItem::Kind::SwapGreed; break;
                    case 'u': item.kind = AstFlagItem::Kind::Unicode; break;
                    case 'R': item.kind = AstFlagItem::Kind::CRLF; break;
                    case 'x': item.kind = AstFlagItem::Kind::IgnoreWhitespace; break;
                    default: Error("unrecognized flag");
                }
            }
            for (const AstFlagItem &x : items)
                if (x.kind == item.kind) Error(item.kind == AstFlagItem::Kind::Negation ? "repeated negation" : "duplicate flag");
            items.push_back(item);
            if (!Bump()) Error("expected flag but got end of regex");
        }
        if (last_was_negation) Error("flag negation operator missing flag");
        return items;
    }

    Primitive ParsePrimitive() {
        Primitive p;
        switch (Char()) {
            case '\\':
                return ParseEscape();
            case '.':
                p.kind = Primitive::Kind::Dot;
                Bump();
                return p;
            case '^':
            case '$':
                p.kind = Primitive::Kind::Assertion;
                Bump();
                return p;
            default:
                p.kind = Primitive::Kind::Literal;
                p.c = Char();
                Bump();
                return p;
        }
    }

    Primitive ParseEscape() {
        if (!Bump()) Error("incomplete escape sequence, reached end of pattern prematurely");
        uint32_t c = Char();
        Primitive p;
        if (c >= '0' && c <= '9') Error("backreferences are not supported");
        if (c == 'x' || c == 'u' || c == 'U') {
            p.kind = Primitive::Kind::Literal;
            p.c = ParseHex();
            return p;
        }
        if (c == 'p' || c == 'P') {
            p.kind = Primitive::Kind::Unicode;
            p.unicode = ParseUnicodeClass();
            return p;
        }
        if (c == 'd' || c == 's' || c == 'w' || c == 'D' || c == 'S' || c == 'W') {
            Bump();
            p.kind = Primitive::Kind::Perl;
            p.negated = c == 'D' || c == 'S' || c == 'W';
            p.perl = (c == 'd' || c == 'D') ? ClassPerlKind::Digit : (c == 's' || c == 'S') ? ClassPerlKind::Space : ClassPerlKind::Word;
            return p;
        }
        Bump();
        if (IsMetaCharacter(c) || IsEscapeableCharacter(c)) {
            p.kind = Primitive::Kind::Literal;
            p.c = c;
            return p;
        }
        p.kind = Primitive::Kind::Literal;
        switch (c) {
            case 'a': p.c = 0x07; return p;
            case 'f': p.c = 0x0C; return p;
            case 't': p.c = '\t'; return p;
            case 'n': p.c = '\n'; return p;
            case 'r': p.c = '\r'; return p;
            case 'v': p.c = 0x0B; return p;
            case 'A':
            case 'z':
            case 'B':
            case '<':
            case '>':
                p.kind = Primitive::Kind::Assertion;
                return p;
            case 'b':
                p.kind = Primitive::Kind::Assertion;
                if (!IsEof() && Char() == '{') MaybeParseSpecialWordBoundary();
                return p;
            default:
                Error("unrecognized escape sequence");
        }
    }

    void MaybeParseSpecialWordBoundary() {
        auto is_valid_char = [](uint32_t c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-'; };
        size_t start = pos_;
        if (!BumpAndBumpSpace()) Error("unexpected end of special word boundary or repetition");
        if (!is_valid_char(Char())) {
            pos_ = start;
            return;
        }
        std::string scratch;
        while (!IsEof() && is_valid_char(Char())) {
            scratch += static_cast<char>(Char());
            BumpAndBumpSpace();
        }
        if (IsEof() || Char() != '}') Error("special word boundary assertion is unclosed or has an invalid character");
        Bump();
        if (scratch != "start" && scratch != "end" && scratch != "start-half" && scratch != "end-half")
            Error("special word boundary assertion is unrecognized");
    }

    uint32_t ParseHex() {
        uint32_t kind = Char();
        unsigned digits = kind == 'x' ? 2 : kind == 'u' ? 4 : 8;
        if (!BumpAndBumpSpace()) Error("incomplete escape sequence, reached end of pattern prematurely");
        std::string hex;
        if (Char() == '{') {
            while (BumpAndBumpSpace() && Char() != '}') {
                if (!IsHex(Char())) Error("hexadecimal literal is not a Unicode scalar value");
                hex += static_cast<char>(Char());
            }
            if (IsEof()) Error("incomplete escape sequence, reached end of pattern prematurely");
            BumpAndBumpSpace();
            if (hex.empty()) Error("hexadecimal literal empty");
        } else {
            for (unsigned i = 0; i < digits; i++) {
                if (i > 0 && !BumpAndBumpSpace()) Error("incomplete escape sequence, reached end of pattern prematurely");
                if (!IsHex(Char())) Error("hexadecimal literal is not a Unicode scalar value");
                hex += static_cast<char>(Char());
            }
            BumpAndBumpSpace();
        }
        if (hex.size() > 8) Error("hexadecimal literal is not a Unicode scalar value");
        uint64_t v = std::stoull(hex, nullptr, 16);
        if (v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) Error("hexadecimal literal is not a Unicode scalar value");
        return static_cast<uint32_t>(v);
    }

    UnicodeQuery ParseUnicodeClass() {
        UnicodeQuery q;
        q.negated = Char() == 'P';
        if (!BumpAndBumpSpace()) Error("incomplete escape sequence, reached end of pattern prematurely");
        if (Char() == '{') {
            std::u32string scratch;
            while (BumpAndBumpSpace() && Char() != '}') scratch += Char();
            if (IsEof()) Error("incomplete escape sequence, reached end of pattern prematurely");
            Bump();
            std::string name;
            for (uint32_t c : scratch) name += Utf8(c);
            size_t i;
            if ((i = name.find("!=")) != std::string::npos) {
                q.name = name.substr(0, i);
                q.value = name.substr(i + 2);
                q.not_equal = true;
            } else if ((i = name.find(':')) != std::string::npos) {
                q.name = name.substr(0, i);
                q.value = name.substr(i + 1);
            } else if ((i = name.find('=')) != std::string::npos) {
                q.name = name.substr(0, i);
                q.value = name.substr(i + 1);
            } else {
                q.name = name;
            }
        } else {
            uint32_t c = Char();
            if (c == '\\') Error("invalid Unicode character class");
            BumpAndBumpSpace();
            q.name = Utf8(c);
        }
        return q;
    }

    // ---- classes
    Ast ParseSetClass() {
        std::vector<ClassSetItem> union_items;
        while (true) {
            BumpSpace();
            if (IsEof()) Error("unclosed character class");
            uint32_t c = Char();
            if (c == '[') {
                if (!stack_class_.empty()) {
                    if (auto cls = MaybeParseAsciiClass()) {
                        union_items.push_back(std::move(*cls));
                        continue;
                    }
                }
                union_items = PushClassOpen(std::move(union_items));
            } else if (c == ']') {
                std::shared_ptr<ClassSet> set;
                bool negated = false;
                if (PopClass(union_items, set, negated)) {
                    Ast a;
                    a.kind = Ast::Kind::ClassBracketed;
                    a.negated = negated;
                    a.set = std::move(set);
                    return a;
                }
            } else if (c == '&' && Peek() == std::optional<uint32_t>('&')) {
                BumpIf("&&");
                union_items = PushClassOp(ClassSet::Op::Intersection, std::move(union_items));
            } else if (c == '-' && Peek() == std::optional<uint32_t>('-')) {
                BumpIf("--");
                union_items = PushClassOp(ClassSet::Op::Difference, std::move(union_items));
            } else if (c == '~' && Peek() == std::optional<uint32_t>('~')) {
                BumpIf("~~");
                union_items = PushClassOp(ClassSet::Op::SymmetricDifference, std::move(union_items));
            } else {
                union_items.push_back(ParseSetClassRange());
            }
        }
    }

    static ClassSetItem PrimitiveIntoClassSetItem(Primitive p) {
        ClassSetItem item;
        switch (p.kind) {
            case Primitive::Kind::Literal:
                item.kind = ClassSetItem::Kind::Literal;
                item.c = p.c;
                return item;
            case Primitive::Kind::Perl:
                item.kind = ClassSetItem::Kind::Perl;
                item.perl = p.perl;
                item.negated = p.negated;
                return item;
            case Primitive::Kind::Unicode:
                item.kind = ClassSetItem::Kind::Unicode;
                item.unicode = std::move(p.unicode);
                return item;
            default:
                throw ParseError{"invalid escape sequence found in character class"};
        }
    }

    ClassSetItem ParseSetClassRange() {
        Primitive prim1 = ParseSetClassItem();
        BumpSpace();
        if (IsEof()) Error("unclosed character class");
        if (Char() != '-' || PeekSpace() == std::optional<uint32_t>(']') || PeekSpace() == std::optional<uint32_t>('-'))
            return PrimitiveIntoClassSetItem(std::move(prim1));
        if (!BumpAndBumpSpace()) Error("unclosed character class");
        Primitive prim2 = ParseSetClassItem();
        if (prim1.kind != Primitive::Kind::Literal || prim2.kind != Primitive::Kind::Literal)
            Error("invalid range boundary, must be a literal");
        if (prim1.c > prim2.c) Error("invalid character class range, the start must be <= the end");
        ClassSetItem item;
        item.kind = ClassSetItem::Kind::Range;
        item.c = prim1.c;
        item.end = prim2.c;
        return item;
    }

    Primitive ParseSetClassItem() {
        if (Char() == '\\') return ParseEscape();
        Primitive p;
        p.kind = Primitive::Kind::Literal;
        p.c = Char();
        Bump();
        return p;
    }

    std::vector<ClassSetItem> PushClassOpen(std::vector<ClassSetItem> parent_union) {
        // parse_set_class_open
        if (!BumpAndBumpSpace()) Error("unclosed character class");
        bool negated = false;
        if (Char() == '^') {
            if (!BumpAndBumpSpace()) Error("unclosed character class");
            negated = true;
        }
        std::vector<ClassSetItem> nested;
        while (Char() == '-') {
            ClassSetItem lit;
            lit.kind = ClassSetItem::Kind::Literal;
            lit.c = '-';
            nested.push_back(std::move(lit));
            if (!BumpAndBumpSpace()) Error("unclosed character class");
        }
        if (nested.empty() && Char() == ']') {
            ClassSetItem lit;
            lit.kind = ClassSetItem::Kind::Literal;
            lit.c = ']';
            nested.push_back(std::move(lit));
            if (!BumpAndBumpSpace()) Error("unclosed character class");
        }
        ClassState s;
        s.union_items = std::move(parent_union);
        s.negated = negated;
        stack_class_.push_back(std::move(s));
        return nested;
    }

    std::shared_ptr<ClassSet> PopClassOp(std::shared_ptr<ClassSet> rhs) {
        if (stack_class_.empty()) Error("internal: empty class stack");
        if (!stack_class_.back().is_op) return rhs;
        ClassState s = std::move(stack_class_.back());
        stack_class_.pop_back();
        auto set = std::make_shared<ClassSet>();
        set->kind = ClassSet::Kind::BinaryOp;
        set->op = s.op;
        set->lhs = std::move(s.lhs);
        set->rhs = std::move(rhs);
        return set;
    }

    std::vector<ClassSetItem> PushClassOp(ClassSet::Op op, std::vector<ClassSetItem> next_union) {
        auto item = std::make_shared<ClassSet>();
        item->item = UnionIntoItem(std::move(next_union));
        std::shared_ptr<ClassSet> new_lhs = PopClassOp(std::move(item));
        ClassState s;
        s.is_op = true;
        s.op = op;
        s.lhs = std::move(new_lhs);
        stack_class_.push_back(std::move(s));
        return {};
    }

    // pop_class: true with the finished top-level class, else continues
    // the parent union (left in `items`).
    bool PopClass(std::vector<ClassSetItem> &items, std::shared_ptr<ClassSet> &out, bool &negated) {
        auto item = std::make_shared<ClassSet>();
        item->item = UnionIntoItem(std::move(items));
        std::shared_ptr<ClassSet> prevset = PopClassOp(std::move(item));
        if (stack_class_.empty() || stack_class_.back().is_op) Error("internal: unexpected class state");
        ClassState s = std::move(stack_class_.back());
        stack_class_.pop_back();
        Bump();
        if (stack_class_.empty()) {
            out = std::move(prevset);
            negated = s.negated;
            return true;
        }
        ClassSetItem bracketed;
        bracketed.kind = ClassSetItem::Kind::Bracketed;
        bracketed.negated = s.negated;
        bracketed.bracketed = std::move(prevset);
        items = std::move(s.union_items);
        items.push_back(std::move(bracketed));
        return false;
    }

    std::optional<ClassSetItem> MaybeParseAsciiClass() {
        size_t start = pos_;
        bool negated = false;
        if (!Bump() || Char() != ':') {
            pos_ = start;
            return std::nullopt;
        }
        if (!Bump()) {
            pos_ = start;
            return std::nullopt;
        }
        if (Char() == '^') {
            negated = true;
            if (!Bump()) {
                pos_ = start;
                return std::nullopt;
            }
        }
        size_t name_start = pos_;
        while (Char() != ':' && Bump()) {
        }
        if (IsEof()) {
            pos_ = start;
            return std::nullopt;
        }
        std::u32string name = pattern_.substr(name_start, pos_ - name_start);
        if (!BumpIf(":]")) {
            pos_ = start;
            return std::nullopt;
        }
        auto kind = AsciiFromName(name);
        if (!kind) {
            pos_ = start;
            return std::nullopt;
        }
        ClassSetItem item;
        item.kind = ClassSetItem::Kind::Ascii;
        item.ascii = *kind;
        item.negated = negated;
        return item;
    }
};

// ---- hir/translate.rs -------------------------------------------------------

std::vector<Range> AsciiRanges(AsciiKind kind) {
    switch (kind) {
        case AsciiKind::Alnum: return {{'0', '9'}, {'A', 'Z'}, {'a', 'z'}};
        case AsciiKind::Alpha: return {{'A', 'Z'}, {'a', 'z'}};
        case AsciiKind::Ascii: return {{0, 0x7F}};
        case AsciiKind::Blank: return {{'\t', '\t'}, {' ', ' '}};
        case AsciiKind::Cntrl: return {{0, 0x1F}, {0x7F, 0x7F}};
        case AsciiKind::Digit: return {{'0', '9'}};
        case AsciiKind::Graph: return {{'!', '~'}};
        case AsciiKind::Lower: return {{'a', 'z'}};
        case AsciiKind::Print: return {{' ', '~'}};
        case AsciiKind::Punct: return {{'!', '/'}, {':', '@'}, {'[', '`'}, {'{', '~'}};
        case AsciiKind::Space: return {{'\t', '\t'}, {'\n', '\n'}, {0x0B, 0x0B}, {0x0C, 0x0C}, {'\r', '\r'}, {' ', ' '}};
        case AsciiKind::Upper: return {{'A', 'Z'}};
        case AsciiKind::Word: return {{'0', '9'}, {'A', 'Z'}, {'_', '_'}, {'a', 'z'}};
        case AsciiKind::Xdigit: return {{'0', '9'}, {'A', 'F'}, {'a', 'f'}};
    }
    return {};
}

class Translator {
public:
    explicit Translator(Flags flags) : flags_(flags) {}

    Hir Translate(const Ast &ast) {
        switch (ast.kind) {
            case Ast::Kind::Empty:
                return Empty();
            case Ast::Kind::Flags: {
                SetFlags(ast.flags);
                return Empty();
            }
            case Ast::Kind::Literal: {
                if (!Unicode()) throw ParseError{"(?-u) is not supported"};
                if (CaseInsensitive() && FoldOverlaps(ast.c, ast.c)) {
                    ClassUnicode cls{{{ast.c, ast.c}}};
                    cls.CaseFoldSimple();
                    return Class(std::move(cls));
                }
                return Literal(Utf8(ast.c));
            }
            case Ast::Kind::Dot: {
                if (!Unicode()) throw ParseError{"(?-u) is not supported"};
                if (flags_.dot_matches_new_line.value_or(false)) return Class(ClassUnicode{{{0, kMaxChar}}});
                if (flags_.crlf.value_or(false)) return Class(ClassUnicode{{{0, 0x09}, {0x0B, 0x0C}, {0x0E, kMaxChar}}});
                ClassUnicode cls{{{'\n', '\n'}}};
                cls.Negate();
                return Class(std::move(cls));
            }
            case Ast::Kind::Assertion:
                return Look();
            case Ast::Kind::ClassPerl:
                return Class(PerlClass(ast.perl, ast.negated));
            case Ast::Kind::ClassUnicode:
                return Class(UnicodeClassOf(ast.unicode));
            case Ast::Kind::ClassBracketed: {
                ClassUnicode cls = EvalSet(*ast.set);
                FoldAndNegate(cls, ast.negated);
                return Class(std::move(cls));
            }
            case Ast::Kind::Repetition: {
                Hir sub = Translate(ast.asts[0]);
                bool greedy = flags_.swap_greed.value_or(false) ? !ast.greedy : ast.greedy;
                return Repetition(ast.min, ast.max, greedy, std::move(sub));
            }
            case Ast::Kind::Group: {
                Flags old = flags_;
                if (ast.group_kind == Ast::GroupKind::NonCapturing) SetFlags(ast.flags);
                Hir sub = Translate(ast.asts[0]);
                flags_ = old;
                if (ast.group_kind == Ast::GroupKind::NonCapturing) return sub;
                std::optional<std::string> name;
                if (ast.group_kind == Ast::GroupKind::CaptureName) name = ast.capture_name;
                return Capture(ast.capture_index, std::move(name), std::move(sub));
            }
            case Ast::Kind::Concat: {
                std::vector<Hir> exprs;
                for (const Ast &a : ast.asts) {
                    Hir h = Translate(a);
                    if (h.kind != Hir::Kind::Empty) exprs.push_back(std::move(h));
                }
                return Concat(std::move(exprs));
            }
            case Ast::Kind::Alternation: {
                std::vector<Hir> exprs;
                for (const Ast &a : ast.asts) exprs.push_back(Translate(a));
                return Alternation(std::move(exprs));
            }
        }
        return Empty();
    }

private:
    Flags flags_;

    bool CaseInsensitive() const { return flags_.case_insensitive.value_or(false); }
    bool Unicode() const { return flags_.unicode.value_or(true); }

    void SetFlags(const std::vector<AstFlagItem> &items) {
        Flags f;
        bool enable = true;
        for (const AstFlagItem &item : items) {
            switch (item.kind) {
                case AstFlagItem::Kind::Negation: enable = false; break;
                case AstFlagItem::Kind::CaseInsensitive: f.case_insensitive = enable; break;
                case AstFlagItem::Kind::MultiLine: f.multi_line = enable; break;
                case AstFlagItem::Kind::DotMatchesNewLine: f.dot_matches_new_line = enable; break;
                case AstFlagItem::Kind::SwapGreed: f.swap_greed = enable; break;
                case AstFlagItem::Kind::Unicode: f.unicode = enable; break;
                case AstFlagItem::Kind::CRLF: f.crlf = enable; break;
                case AstFlagItem::Kind::IgnoreWhitespace: break;
            }
        }
        f.Merge(flags_);
        flags_ = f;
    }

    void FoldAndNegate(ClassUnicode &cls, bool negated) {
        if (CaseInsensitive()) cls.CaseFoldSimple();
        if (negated) cls.Negate();
    }

    ClassUnicode PerlClass(ClassPerlKind kind, bool negated) {
        if (!Unicode()) throw ParseError{"(?-u) is not supported"};
        ClassUnicode cls;
        switch (kind) {
            case ClassPerlKind::Digit: cls = TableClass(u::kPerlDecimal); break;
            case ClassPerlKind::Space: cls = TableClass(u::kPerlSpace); break;
            case ClassPerlKind::Word: cls = TableClass(u::kPerlWord); break;
        }
        if (negated) cls.Negate();
        return cls;
    }

    ClassUnicode UnicodeClassOf(const UnicodeQuery &q) {
        if (!Unicode()) throw ParseError{"Unicode not allowed here"};
        ClassUnicode cls;
        std::string error;
        if (!UnicodeClass(q.name, q.value ? &*q.value : nullptr, cls, error)) throw ParseError{error};
        // translate.rs reads `negated`, not is_negated(): `\p{gc!=L}` is not negated.
        FoldAndNegate(cls, q.negated);
        return cls;
    }

    ClassUnicode EvalItem(const ClassSetItem &item) {
        switch (item.kind) {
            case ClassSetItem::Kind::Empty:
                return {};
            case ClassSetItem::Kind::Literal:
                return ClassUnicode{{{item.c, item.c}}};
            case ClassSetItem::Kind::Range: {
                ClassUnicode c{{Create(item.c, item.end)}};
                return c;
            }
            case ClassSetItem::Kind::Ascii: {
                ClassUnicode c{AsciiRanges(item.ascii)};
                c.Canonicalize();
                FoldAndNegate(c, item.negated);
                return c;
            }
            case ClassSetItem::Kind::Unicode:
                return UnicodeClassOf(item.unicode);
            case ClassSetItem::Kind::Perl:
                return PerlClass(item.perl, item.negated);
            case ClassSetItem::Kind::Bracketed: {
                ClassUnicode c = EvalSet(*item.bracketed);
                FoldAndNegate(c, item.negated);
                return c;
            }
            case ClassSetItem::Kind::Union: {
                ClassUnicode c;
                for (const ClassSetItem &i : item.items) c.Union(EvalItem(i));
                return c;
            }
        }
        return {};
    }

    ClassUnicode EvalSet(const ClassSet &set) {
        if (set.kind == ClassSet::Kind::Item) return EvalItem(set.item);
        ClassUnicode lhs = EvalSet(*set.lhs);
        ClassUnicode rhs = EvalSet(*set.rhs);
        if (CaseInsensitive()) {
            rhs.CaseFoldSimple();
            lhs.CaseFoldSimple();
        }
        switch (set.op) {
            case ClassSet::Op::Intersection: lhs.Intersect(rhs); break;
            case ClassSet::Op::Difference: lhs.Difference(rhs); break;
            case ClassSet::Op::SymmetricDifference: lhs.SymmetricDifference(rhs); break;
        }
        return lhs;
    }
};

std::u32string DecodeUtf8(const std::string &s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        unsigned char b0 = static_cast<unsigned char>(s[i]);
        size_t n = b0 < 0x80 ? 1 : (b0 >> 5) == 6 ? 2 : (b0 >> 4) == 14 ? 3 : 4;
        uint32_t c = n == 1 ? b0 : n == 2 ? (b0 & 0x1F) : n == 3 ? (b0 & 0x0F) : (b0 & 0x07);
        for (size_t k = 1; k < n && i + k < s.size(); k++) c = (c << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        out += c;
        i += n;
    }
    return out;
}

}  // namespace

bool Parse(const std::string &pattern, bool case_insensitive, Hir &out, std::string &error) {
    try {
        Parser parser(DecodeUtf8(pattern));
        Ast ast = parser.Parse();
        Flags flags;
        if (case_insensitive) flags.case_insensitive = true;
        Translator translator(flags);
        out = translator.Translate(ast);
        return true;
    } catch (const ParseError &e) {
        error = "regex parse error:\n    " + pattern + "\nerror: " + e.message;
        return false;
    }
}

}  // namespace tsgen::regex
