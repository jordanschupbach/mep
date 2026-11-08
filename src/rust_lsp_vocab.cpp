// Lookups over the generated Rust standard-library tables
// (rust_lsp_vocab_data.cpp): entries sorted by path, name, kind; aliases by
// alias; impls by type, trait, header; prelude, keywords and attributes by
// name.

#include "rust_lsp_vocab.h"

#include <algorithm>

extern const RustVocabEntry kRustVocab[];
extern const size_t kRustVocabCount;
extern const RustVocabAlias kRustVocabAliases[];
extern const size_t kRustVocabAliasCount;
extern const RustVocabImpl kRustVocabImpls[];
extern const size_t kRustVocabImplCount;
extern const RustVocabPreludeName kRustVocabPrelude[];
extern const size_t kRustVocabPreludeCount;
extern const RustVocabSupertrait kRustVocabSupertraits[];
extern const size_t kRustVocabSupertraitCount;
extern const RustVocabEntry kRustVocabKeywords[];
extern const size_t kRustVocabKeywordCount;
extern const RustVocabEntry kRustVocabAttributes[];
extern const size_t kRustVocabAttributeCount;

namespace {

const RustVocabEntry *EntriesEnd() { return kRustVocab + kRustVocabCount; }

const RustVocabEntry *LowerBound(std::string_view path, std::string_view name) {
    return std::lower_bound(kRustVocab, EntriesEnd(), 0, [&](const RustVocabEntry &e, int) {
        const int c = std::string_view(e.path).compare(path);
        return c != 0 ? c < 0 : std::string_view(e.name) < name;
    });
}

// Entries with exactly this path and name.
std::pair<const RustVocabEntry *, const RustVocabEntry *> EqualRange(std::string_view path,
                                                                     std::string_view name) {
    const RustVocabEntry *lo = LowerBound(path, name);
    const RustVocabEntry *hi = lo;
    while (hi != EntriesEnd() && hi->path == path && hi->name == name) ++hi;
    return {lo, hi};
}

bool IsMacroish(RustVocabKind k) {
    return k == RustVocabKind::Macro || k == RustVocabKind::Derive || k == RustVocabKind::Attribute;
}

const RustVocabEntry *PreferNonMacro(std::pair<const RustVocabEntry *, const RustVocabEntry *> r) {
    for (const RustVocabEntry *it = r.first; it != r.second; ++it)
        if (!IsMacroish(it->kind)) return it;
    return r.first != r.second ? r.first : nullptr;
}

const RustVocabAlias *FindAlias(std::string_view alias) {
    const RustVocabAlias *end = kRustVocabAliases + kRustVocabAliasCount;
    const RustVocabAlias *it = std::lower_bound(kRustVocabAliases, end, alias, [](const RustVocabAlias &a,
                                                                                 std::string_view s) {
        return std::string_view(a.alias) < s;
    });
    if (it != end && it->alias == alias) return it;
    return nullptr;
}

std::string_view StripPrefix(std::string_view path) {
    if (path.substr(0, 2) == "::") path.remove_prefix(2);
    for (std::string_view p : {"std::primitive::", "core::primitive::"})
        if (path.substr(0, p.size()) == p) return path.substr(p.size());
    return path;
}

bool HasItem(std::string_view path) {
    auto r = EqualRange(path, "");
    return r.first != r.second;
}

bool IsOwnerKind(RustVocabKind k) {
    return k == RustVocabKind::Struct || k == RustVocabKind::Enum || k == RustVocabKind::Union ||
           k == RustVocabKind::Trait || k == RustVocabKind::Primitive;
}

// path -> (canonical owner-or-item path, member name or "")
bool Resolve(std::string_view path, std::string &out_path, std::string &out_member, int depth = 0) {
    path = StripPrefix(path);
    if (path.empty() || depth > 8) return false;
    if (HasItem(path)) {
        out_path.assign(path);
        out_member.clear();
        return true;
    }
    if (const RustVocabAlias *a = FindAlias(path)) {
        out_path = a->canonical;
        out_member = a->member;
        return true;
    }
    const size_t sep = path.rfind("::");
    if (sep == std::string_view::npos) return false;
    std::string owner, owner_member;
    if (!Resolve(path.substr(0, sep), owner, owner_member, depth + 1) || !owner_member.empty()) return false;
    const std::string_view last = path.substr(sep + 2);
    const RustVocabEntry *oe = PreferNonMacro(EqualRange(owner, ""));
    if (oe == nullptr) return false;
    if (oe->kind == RustVocabKind::Module) {
        // A module re-exported under another name: core::option -> std::option.
        std::string joined = owner + "::" + std::string(last);
        if (joined.size() == path.size() && joined == path) return false;
        return Resolve(joined, out_path, out_member, depth + 1);
    }
    if (IsOwnerKind(oe->kind) || oe->kind == RustVocabKind::TypeAlias) {
        out_path = owner;
        out_member.assign(last);
        return true;
    }
    return false;
}

std::string_view LastSegment(std::string_view p) {
    const size_t sep = p.rfind("::");
    return sep == std::string_view::npos ? p : p.substr(sep + 2);
}

const RustVocabEntry *FindInTable(const RustVocabEntry *table, size_t count, std::string_view name) {
    const RustVocabEntry *end = table + count;
    const RustVocabEntry *it = std::lower_bound(
        table, end, name, [](const RustVocabEntry &e, std::string_view s) { return std::string_view(e.name) < s; });
    if (it != end && it->name == name) return it;
    return nullptr;
}

std::vector<const RustVocabEntry *> AllOf(const RustVocabEntry *table, size_t count) {
    std::vector<const RustVocabEntry *> out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) out.push_back(&table[i]);
    return out;
}

unsigned char EditionBit(int edition) {
    switch (edition) {
        case 2015: return kRustEdition2015;
        case 2018: return kRustEdition2018;
        case 2021: return kRustEdition2021;
        case 2024: return kRustEdition2024;
        default: return edition > 2024 ? kRustEdition2024 : kRustEdition2015;
    }
}

}  // namespace

std::string RustVocabCanonicalPath(std::string_view path) {
    std::string p, m;
    if (!Resolve(path, p, m)) return {};
    return m.empty() ? p : p + "::" + m;
}

const RustVocabEntry *RustVocabFind(std::string_view path) {
    std::string p, m;
    if (!Resolve(path, p, m)) return nullptr;
    if (!m.empty()) return RustVocabFindMember(p, m);
    return PreferNonMacro(EqualRange(p, ""));
}

std::vector<const RustVocabEntry *> RustVocabFindAll(std::string_view path) {
    std::vector<const RustVocabEntry *> out;
    std::string p, m;
    if (!Resolve(path, p, m)) return out;
    auto r = EqualRange(p, m);
    for (const RustVocabEntry *it = r.first; it != r.second; ++it) out.push_back(it);
    return out;
}

const RustVocabEntry *RustVocabFindMember(std::string_view owner_path, std::string_view name) {
    if (name.empty()) return nullptr;
    std::string p, m;
    if (!Resolve(owner_path, p, m) || !m.empty()) return nullptr;
    auto r = EqualRange(p, name);
    return r.first != r.second ? r.first : nullptr;
}

std::vector<RustVocabModuleMember> RustVocabModuleMembers(std::string_view module_path) {
    std::vector<RustVocabModuleMember> out;
    std::string mod, m;
    if (!Resolve(module_path, mod, m) || !m.empty()) return out;
    const RustVocabEntry *me = PreferNonMacro(EqualRange(mod, ""));
    if (me == nullptr || me->kind != RustVocabKind::Module) return out;
    const std::string prefix = mod + "::";
    // Own items: entries whose path is prefix + one segment.
    for (const RustVocabEntry *it = LowerBound(prefix, ""); it != EntriesEnd(); ++it) {
        const std::string_view path = it->path;
        if (path.substr(0, prefix.size()) != prefix) break;
        if (*it->name != '\0') continue;
        const std::string_view rest = path.substr(prefix.size());
        if (rest.find("::") != std::string_view::npos) continue;
        out.push_back({rest, it});
    }
    // Re-exports: aliases directly under the module.
    const RustVocabAlias *aend = kRustVocabAliases + kRustVocabAliasCount;
    const RustVocabAlias *a = std::lower_bound(kRustVocabAliases, aend, std::string_view(prefix),
                                               [](const RustVocabAlias &x, std::string_view s) {
                                                   return std::string_view(x.alias) < s;
                                               });
    for (; a != aend; ++a) {
        const std::string_view alias = a->alias;
        if (alias.substr(0, prefix.size()) != prefix) break;
        const std::string_view rest = alias.substr(prefix.size());
        if (rest.find("::") != std::string_view::npos) continue;
        auto r = EqualRange(a->canonical, a->member);
        for (const RustVocabEntry *it = r.first; it != r.second; ++it) out.push_back({rest, it});
    }
    std::sort(out.begin(), out.end(), [](const RustVocabModuleMember &x, const RustVocabModuleMember &y) {
        return x.name != y.name ? x.name < y.name : x.entry < y.entry;
    });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const RustVocabModuleMember &x, const RustVocabModuleMember &y) {
                              return x.name == y.name && x.entry == y.entry;
                          }),
              out.end());
    return out;
}

std::vector<const RustVocabEntry *> RustVocabOwnMembers(std::string_view owner_path) {
    std::vector<const RustVocabEntry *> out;
    std::string p, m;
    if (!Resolve(owner_path, p, m) || !m.empty()) return out;
    const RustVocabEntry *it = LowerBound(p, "");
    for (; it != EntriesEnd() && it->path == p; ++it)
        if (*it->name != '\0') out.push_back(it);
    return out;
}

std::vector<const RustVocabImpl *> RustVocabImpls(std::string_view type_path) {
    std::vector<const RustVocabImpl *> out;
    std::string p, m;
    if (!Resolve(type_path, p, m) || !m.empty()) return out;
    const RustVocabImpl *end = kRustVocabImpls + kRustVocabImplCount;
    const RustVocabImpl *it = std::lower_bound(kRustVocabImpls, end, std::string_view(p),
                                               [](const RustVocabImpl &r, std::string_view s) {
                                                   return std::string_view(r.type) < s;
                                               });
    for (; it != end && it->type == p; ++it) out.push_back(it);
    return out;
}

std::vector<const RustVocabImpl *> RustVocabImplementors(std::string_view trait_path) {
    std::vector<const RustVocabImpl *> out;
    std::string p, m;
    if (!Resolve(trait_path, p, m) || !m.empty()) return out;
    for (size_t i = 0; i < kRustVocabImplCount; ++i)
        if (kRustVocabImpls[i].trait == p) out.push_back(&kRustVocabImpls[i]);
    return out;
}

bool RustVocabImplements(std::string_view type_path, std::string_view trait_path) {
    const std::string t = RustVocabCanonicalPath(trait_path);
    if (t.empty()) return false;
    for (const RustVocabImpl *r : RustVocabImpls(type_path))
        if (r->trait == t && !r->negative) return true;
    return false;
}

std::string_view RustVocabDerefTarget(std::string_view type_path) {
    for (const RustVocabImpl *r : RustVocabImpls(type_path))
        if (std::string_view(r->trait) == "std::ops::Deref" && *r->deref_target != '\0') return r->deref_target;
    return {};
}

std::vector<std::string_view> RustVocabSupertraits(std::string_view trait_path) {
    std::vector<std::string_view> out;
    const std::string p = RustVocabCanonicalPath(trait_path);
    for (size_t i = 0; i < kRustVocabSupertraitCount; ++i)
        if (kRustVocabSupertraits[i].trait == p) out.emplace_back(kRustVocabSupertraits[i].supertrait);
    return out;
}

std::vector<RustVocabMemberRef> RustVocabTypeMembers(std::string_view type_path, bool include_blanket) {
    std::vector<RustVocabMemberRef> out;
    for (const RustVocabEntry *e : RustVocabOwnMembers(type_path)) out.push_back({e, nullptr});
    std::vector<std::string_view> seen;
    for (const RustVocabImpl *r : RustVocabImpls(type_path)) {
        if (r->negative || (r->blanket && !include_blanket)) continue;
        const std::string_view trait = r->trait;
        if (std::find(seen.begin(), seen.end(), trait) != seen.end()) continue;
        seen.push_back(trait);
        const RustVocabEntry *it = LowerBound(trait, "");
        for (; it != EntriesEnd() && it->path == trait; ++it)
            if (*it->name != '\0') out.push_back({it, r});
    }
    return out;
}

std::vector<const RustVocabPreludeName *> RustVocabPrelude(int edition) {
    std::vector<const RustVocabPreludeName *> out;
    const unsigned char bit = EditionBit(edition);
    for (size_t i = 0; i < kRustVocabPreludeCount; ++i)
        if (kRustVocabPrelude[i].editions & bit) out.push_back(&kRustVocabPrelude[i]);
    return out;
}

std::vector<const RustVocabPreludeName *> RustVocabPreludeFind(std::string_view name, int edition) {
    std::vector<const RustVocabPreludeName *> out;
    const unsigned char bit = EditionBit(edition);
    const RustVocabPreludeName *end = kRustVocabPrelude + kRustVocabPreludeCount;
    const RustVocabPreludeName *it =
        std::lower_bound(kRustVocabPrelude, end, name, [](const RustVocabPreludeName &p, std::string_view s) {
            return std::string_view(p.name) < s;
        });
    for (; it != end && it->name == name; ++it)
        if (it->editions & bit) out.push_back(it);
    return out;
}

const RustVocabEntry *RustVocabKeyword(std::string_view name) {
    return FindInTable(kRustVocabKeywords, kRustVocabKeywordCount, name);
}

std::vector<const RustVocabEntry *> RustVocabKeywords() { return AllOf(kRustVocabKeywords, kRustVocabKeywordCount); }

const RustVocabEntry *RustVocabAttribute(std::string_view name) {
    return FindInTable(kRustVocabAttributes, kRustVocabAttributeCount, name);
}

std::vector<const RustVocabEntry *> RustVocabAttributes() {
    return AllOf(kRustVocabAttributes, kRustVocabAttributeCount);
}

const RustVocabEntry *RustVocabMacro(std::string_view name) {
    if (name.size() > 1 && name.back() == '!') name.remove_suffix(1);
    const std::string std_path = "std::" + std::string(name);
    auto r = EqualRange(std_path, "");
    for (const RustVocabEntry *it = r.first; it != r.second; ++it)
        if (it->kind == RustVocabKind::Macro) return it;
    for (size_t i = 0; i < kRustVocabCount; ++i)
        if (kRustVocab[i].kind == RustVocabKind::Macro && LastSegment(kRustVocab[i].path) == name)
            return &kRustVocab[i];
    return nullptr;
}

std::vector<const RustVocabEntry *> RustVocabMacros() {
    std::vector<const RustVocabEntry *> out;
    for (size_t i = 0; i < kRustVocabCount; ++i)
        if (kRustVocab[i].kind == RustVocabKind::Macro) out.push_back(&kRustVocab[i]);
    return out;
}

const char *RustVocabKindName(RustVocabKind kind) {
    switch (kind) {
        case RustVocabKind::Module: return "module";
        case RustVocabKind::Struct: return "struct";
        case RustVocabKind::Enum: return "enum";
        case RustVocabKind::Variant: return "variant";
        case RustVocabKind::Union: return "union";
        case RustVocabKind::Trait: return "trait";
        case RustVocabKind::Fn: return "function";
        case RustVocabKind::Method: return "method";
        case RustVocabKind::AssocFn: return "associated function";
        case RustVocabKind::AssocConst: return "associated constant";
        case RustVocabKind::AssocType: return "associated type";
        case RustVocabKind::Const: return "constant";
        case RustVocabKind::Static: return "static";
        case RustVocabKind::TypeAlias: return "type alias";
        case RustVocabKind::Macro: return "macro";
        case RustVocabKind::Derive: return "derive macro";
        case RustVocabKind::Primitive: return "primitive";
        case RustVocabKind::Keyword: return "keyword";
        case RustVocabKind::Attribute: return "attribute";
        case RustVocabKind::Field: return "field";
    }
    return "item";
}

size_t RustVocabSize() { return kRustVocabCount; }

const RustVocabEntry &RustVocabAt(size_t i) { return kRustVocab[i]; }
