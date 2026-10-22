// The C# language server's type model: type names, the BCL table's
// indexes, the universe of BCL and source types, and the types of
// expressions as far as a reader can see them. See csharp_lsp_analysis.h.
#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>

#include "csharp_lsp_analysis.h"

namespace cslsp {

// --- Type names --------------------------------------------------------------------------

namespace {

struct TypeParser {
    const std::string &s;
    size_t i = 0;

    void Ws() {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
    }
    bool Eat(char c) {
        Ws();
        if (i < s.size() && s[i] == c) {
            i++;
            return true;
        }
        return false;
    }
    std::string Ident() {
        Ws();
        size_t start = i;
        if (i < s.size() && s[i] == '@') i++;
        while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_' ||
                                static_cast<unsigned char>(s[i]) >= 0x80)) {
            i++;
        }
        std::string id = s.substr(start, i - start);
        if (!id.empty() && id[0] == '@') id = id.substr(1);
        return id;
    }

    TypeName Parse(int depth = 0) {
        TypeName t;
        if (depth > 32) return t;
        Ws();
        // Modifiers that may precede a type in a signature.
        for (;;) {
            size_t save = i;
            std::string w = Ident();
            if (w == "ref" || w == "out" || w == "in" || w == "params" || w == "this" || w == "scoped" ||
                w == "readonly") {
                continue;
            }
            i = save;
            break;
        }
        Ws();
        if (i < s.size() && s[i] == '(') {
            i++;
            for (;;) {
                TypeName e = Parse(depth + 1);
                Ws();
                std::string name;
                size_t save = i;
                std::string maybe = Ident();
                if (!maybe.empty()) name = maybe;
                else i = save;
                t.tuple.push_back(e);
                t.tuple_names.push_back(name);
                if (Eat(',')) continue;
                Eat(')');
                break;
            }
        } else {
            std::string name;
            for (;;) {
                std::string id = Ident();
                if (id.empty()) break;
                if (id == "global" && i + 1 < s.size() && s[i] == ':' && s[i + 1] == ':') {
                    i += 2;
                    continue;
                }
                std::vector<TypeName> args;
                if (Eat('<')) {
                    if (Eat('>')) {
                        args.push_back(TypeName());  // unbound: List<>
                    } else {
                        for (;;) {
                            args.push_back(Parse(depth + 1));
                            if (Eat(',')) continue;
                            Eat('>');
                            break;
                        }
                    }
                }
                if (!name.empty()) name += '.';
                name += id;
                if (!args.empty()) name += "`" + std::to_string(args.size());
                for (TypeName &a : args) t.args.push_back(std::move(a));
                Ws();
                if (i < s.size() && s[i] == '.') {
                    i++;
                    continue;
                }
                if (i + 1 < s.size() && s[i] == ':' && s[i + 1] == ':') {
                    i += 2;
                    name.clear();  // alias::Name
                    continue;
                }
                break;
            }
            t.name = name;
        }
        for (;;) {
            Ws();
            if (i < s.size() && s[i] == '?') {
                i++;
                t.nullable = true;
            } else if (i < s.size() && s[i] == '*') {
                i++;
                t.pointer++;
            } else if (i < s.size() && s[i] == '[') {
                size_t save = i;
                i++;
                int rank = 1;
                Ws();
                while (i < s.size() && s[i] == ',') {
                    rank++;
                    i++;
                    Ws();
                }
                if (i < s.size() && s[i] == ']') {
                    i++;
                    t.ranks.push_back(rank);
                } else {
                    i = save;
                    break;
                }
            } else {
                break;
            }
        }
        return t;
    }
};

const std::map<std::string, std::string> &KeywordTypes() {
    static const std::map<std::string, std::string> m = {
        {"bool", "System.Boolean"}, {"byte", "System.Byte"},     {"sbyte", "System.SByte"},
        {"char", "System.Char"},    {"decimal", "System.Decimal"}, {"double", "System.Double"},
        {"float", "System.Single"}, {"int", "System.Int32"},     {"uint", "System.UInt32"},
        {"long", "System.Int64"},   {"ulong", "System.UInt64"},  {"short", "System.Int16"},
        {"ushort", "System.UInt16"}, {"object", "System.Object"}, {"string", "System.String"},
        {"nint", "System.IntPtr"},  {"nuint", "System.UIntPtr"}, {"void", "System.Void"},
        {"dynamic", "System.Object"},
    };
    return m;
}

}  // namespace

TypeName ParseTypeName(const std::string &text) {
    TypeParser p{text};
    return p.Parse();
}

std::string KeywordTypeFullName(const std::string &name) {
    auto it = KeywordTypes().find(name);
    return it == KeywordTypes().end() ? name : it->second;
}

std::string FullNameKeyword(const std::string &name) {
    for (const auto &kv : KeywordTypes()) {
        if (kv.second == name && kv.first != "dynamic" && kv.first != "nint" && kv.first != "nuint") return kv.first;
    }
    return name;
}

std::string ShowType(const TypeName &t) {
    std::string out;
    if (!t.tuple.empty()) {
        out = "(";
        for (size_t i = 0; i < t.tuple.size(); i++) {
            if (i) out += ", ";
            out += ShowType(t.tuple[i]);
            if (i < t.tuple_names.size() && !t.tuple_names[i].empty()) out += " " + t.tuple_names[i];
        }
        out += ")";
    } else {
        // Segments with arities take their arguments in order; namespaces dropped.
        std::string name = FullNameKeyword(t.name);
        std::vector<std::string> segs;
        size_t start = 0;
        for (size_t i = 0; i <= name.size(); i++) {
            if (i == name.size() || name[i] == '.') {
                segs.push_back(name.substr(start, i - start));
                start = i + 1;
            }
        }
        // Drop leading namespace segments: keep from the first segment that is a type (heuristic: the first
        // generic or the last).
        size_t argi = 0;
        std::vector<std::string> shown;
        for (const std::string &seg : segs) {
            size_t tick = seg.find('`');
            std::string base = tick == std::string::npos ? seg : seg.substr(0, tick);
            int arity = tick == std::string::npos ? 0 : std::atoi(seg.c_str() + tick + 1);
            std::string part = base;
            if (arity > 0) {
                part += "<";
                for (int a = 0; a < arity; a++) {
                    if (a) part += ", ";
                    if (argi < t.args.size()) part += ShowType(t.args[argi++]);
                }
                part += ">";
            }
            shown.push_back(part);
        }
        // Namespace segments start lowercase rarely; keep the last segment plus any generic owners.
        size_t first = shown.empty() ? 0 : shown.size() - 1;
        for (size_t i = 0; i + 1 < segs.size(); i++) {
            if (segs[i].find('`') != std::string::npos) {
                first = i;
                break;
            }
        }
        for (size_t i = first; i < shown.size(); i++) {
            if (i > first) out += ".";
            out += shown[i];
        }
    }
    if (t.nullable) out += "?";
    for (int i = 0; i < t.pointer; i++) out += "*";
    for (int r : t.ranks) out += "[" + std::string(static_cast<size_t>(r - 1), ',') + "]";
    return out;
}

TypeName Substitute(const TypeName &t, const std::map<std::string, TypeName> &subst) {
    if (subst.empty()) return t;
    if (t.args.empty() && t.tuple.empty()) {
        auto it = subst.find(t.name);
        if (it != subst.end() && !it->second.Empty()) {
            TypeName r = it->second;
            for (int rank : t.ranks) r.ranks.push_back(rank);
            r.nullable = r.nullable || t.nullable;
            r.pointer += t.pointer;
            return r;
        }
        return t;
    }
    TypeName r = t;
    for (TypeName &a : r.args) a = Substitute(a, subst);
    for (TypeName &e : r.tuple) e = Substitute(e, subst);
    return r;
}

std::string MetadataKey(const TypeName &t) { return KeywordTypeFullName(t.name); }

std::string BclNamespace(const CsBclType &t) {
    TypeName n = ParseTypeName(t.name);
    // The namespace is the leading run of segments before the first type segment; types start uppercase and
    // namespaces in the table are known.
    const BclIndex &b = Bcl();
    std::string best;
    for (const std::string &ns : b.namespaces) {
        if (n.name.size() > ns.size() && n.name.compare(0, ns.size(), ns) == 0 && n.name[ns.size()] == '.' &&
            ns.size() > best.size()) {
            best = ns;
        }
    }
    return best;
}

// --- The BCL table's indexes -------------------------------------------------------------------

namespace {

std::string SimpleKey(const std::string &name) {
    size_t dot = name.rfind('.');
    return dot == std::string::npos ? name : name.substr(dot + 1);
}

}  // namespace

const BclIndex &Bcl() {
    static const BclIndex index = [] {
        BclIndex b;
        b.types = CsBclTypes(&b.type_count);
        b.members = CsBclMembers(&b.member_count);
        std::set<std::string> nss;
        for (size_t i = 0; i < b.type_count; i++) {
            TypeName n = ParseTypeName(b.types[i].name);
            b.by_full[n.name] = i;
            b.by_simple.emplace(SimpleKey(n.name), i);
        }
        // Namespaces: the prefixes of type names that are not themselves types.
        for (size_t i = 0; i < b.type_count; i++) {
            TypeName n = ParseTypeName(b.types[i].name);
            std::string prefix;
            size_t start = 0;
            for (size_t k = 0; k < n.name.size(); k++) {
                if (n.name[k] == '.') {
                    prefix = n.name.substr(0, k);
                    if (b.by_full.count(prefix)) break;
                    nss.insert(prefix);
                    start = k + 1;
                }
            }
            (void)start;
        }
        b.namespaces.assign(nss.begin(), nss.end());
        size_t i = 0;
        while (i < b.member_count) {
            size_t j = i;
            while (j < b.member_count && std::strcmp(b.members[j].owner, b.members[i].owner) == 0) j++;
            b.member_range[b.members[i].owner] = {i, j};
            for (size_t k = i; k < j; k++) {
                if (b.members[k].flags & kCsBclExtension) b.extensions.emplace(b.members[k].name, k);
            }
            i = j;
        }
        return b;
    }();
    return index;
}

// --- ResolvedType --------------------------------------------------------------------------------

std::string ResolvedType::FullName() const {
    if (bcl) return ParseTypeName(bcl->name).name;
    if (source) return source->FullName();
    return name.name;
}

std::string ResolvedType::Kind() const {
    if (bcl) return bcl->kind;
    if (source) return source->kind;
    return "";
}

// --- Universe ------------------------------------------------------------------------------------

namespace {

/** @brief The workspace's declarations as source tables. */
std::shared_ptr<const SourceTables> ProjectTables(const CsProject &project) {
    if (project.cache && project.cache_version == project.version) {
        return std::static_pointer_cast<const SourceTables>(project.cache);
    }
    SourceIndex idx;
    for (const CsProjectDef &d : project.defs) {
        bool isType = d.kind == "class" || d.kind == "struct" || d.kind == "interface" || d.kind == "enum" ||
                      d.kind == "record" || d.kind == "record struct" || d.kind == "delegate" ||
                      d.kind == "static class";
        if (isType) {
            SourceType t;
            t.ns = d.ns;
            t.owner = d.owner;
            t.name = d.name;
            t.kind = d.kind;
            std::string b = d.bases;
            size_t start = 0;
            int depth = 0;
            for (size_t i = 0; i <= b.size(); i++) {
                if (i < b.size() && b[i] == '<') depth++;
                if (i < b.size() && b[i] == '>') depth--;
                if (i == b.size() || (b[i] == ',' && depth == 0)) {
                    std::string part = b.substr(start, i - start);
                    part.erase(0, part.find_first_not_of(' '));
                    if (!part.empty()) t.bases.push_back(part);
                    start = i + 1;
                }
            }
            // Type parameters from the signature: "class Foo<T, U>".
            size_t open = d.signature.find(d.name + "<");
            if (open != std::string::npos) {
                size_t i = open + d.name.size() + 1;
                std::string cur;
                int dep = 0;
                auto push = [&](std::string tp) {
                    for (const char *v : {"in", "out"}) {
                        size_t n = std::strlen(v);
                        if (tp.size() > n && tp.compare(0, n, v) == 0 && std::isupper(static_cast<unsigned char>(tp[n]))) {
                            tp = tp.substr(n);
                        }
                    }
                    t.type_params.push_back(tp);
                };
                for (; i < d.signature.size(); i++) {
                    char c = d.signature[i];
                    if (c == '<') dep++;
                    if (c == '>' && dep-- == 0) break;
                    if (c == ',' && dep == 0) {
                        push(cur);
                        cur.clear();
                        continue;
                    }
                    if (!std::isspace(static_cast<unsigned char>(c))) cur += c;
                }
                if (!cur.empty()) push(cur);
            }
            t.doc = d.doc;
            t.signature = d.signature;
            t.path = d.path;
            t.range = d.range;
            idx.types.push_back(t);
        } else {
            SourceMember m;
            m.owner_full = d.owner;  // a member's owner is its type's full key
            m.name = d.name;
            m.kind = d.kind;
            m.type = d.type;
            m.params = d.params;
            m.signature = d.signature;
            m.doc = d.doc;
            m.is_static = d.is_static;
            m.is_extension = d.is_extension;
            m.path = d.path;
            m.range = d.range;
            idx.members.push_back(m);
        }
    }
    auto tables = std::make_shared<SourceTables>();
    tables->Load(idx);
    project.cache = tables;
    project.cache_version = project.version;
    return tables;
}

}  // namespace

Universe::Universe(const SourceIndex *current, const CsProject *project) {
    if (current) {
        auto tables = std::make_shared<SourceTables>();
        tables->Load(*current);
        layers_.push_back(tables);
    }
    if (project) layers_.push_back(ProjectTables(*project));
}

void SourceTables::Load(const SourceIndex &idx) {
    for (const SourceType &t : idx.types) {
        by_name.emplace(t.type_params.empty() ? t.name : t.name + "`" + std::to_string(t.type_params.size()),
                        types.size());
        types.push_back(t);
        std::string n;
        for (size_t i = 0; i <= t.ns.size(); i++) {
            if (i == t.ns.size() || t.ns[i] == '.') {
                n = t.ns.substr(0, i);
                if (!n.empty() && std::find(namespaces.begin(), namespaces.end(), n) == namespaces.end()) {
                    namespaces.push_back(n);
                }
            }
        }
    }
    for (const SourceMember &m : idx.members) {
        by_owner.emplace(m.owner_full, members.size());
        members.push_back(m);
    }
}

namespace {

ResolvedType FromBcl(size_t index, const TypeName &ref) {
    const BclIndex &b = Bcl();
    ResolvedType r;
    r.bcl = &b.types[index];
    r.name = ref;
    TypeName decl = ParseTypeName(r.bcl->name);
    for (size_t i = 0; i < decl.args.size() && i < ref.args.size(); i++) {
        if (!ref.args[i].Empty()) r.subst[decl.args[i].name] = ref.args[i];
    }
    return r;
}

ResolvedType FromSource(const SourceType &t, const TypeName &ref) {
    ResolvedType r;
    r.source = &t;
    r.name = ref;
    for (size_t i = 0; i < t.type_params.size() && i < ref.args.size(); i++) r.subst[t.type_params[i]] = ref.args[i];
    return r;
}

}  // namespace

ResolvedType Universe::Resolve(const TypeName &t0, const std::vector<std::string> &usings, const std::string &ns,
                               const std::vector<std::string> &enclosing) const {
    ResolvedType none;
    none.name = t0;
    if (t0.IsArray()) {
        // Arrays: System.Array's members (and IEnumerable<T> for LINQ through the element type).
        TypeName arr;
        arr.name = "System.Array";
        const BclIndex &b = Bcl();
        auto it = b.by_full.find("System.Array");
        if (it == b.by_full.end()) return none;
        ResolvedType r = FromBcl(it->second, arr);
        r.name = t0;
        TypeName elem = t0;
        elem.ranks.erase(elem.ranks.begin());
        r.subst["T"] = elem;  // for the IEnumerable<T> the array implements
        return r;
    }
    if (!t0.tuple.empty()) {
        TypeName vt;
        vt.name = "System.ValueTuple`" + std::to_string(t0.tuple.size());
        vt.args = t0.tuple;
        ResolvedType r = Resolve(vt, usings, ns, enclosing);
        r.name = t0;
        return r;
    }
    TypeName t = t0;
    if (t.nullable) {
        // int? is Nullable<int>; a reference type's ? changes nothing here.
        t.nullable = false;
        std::string full = KeywordTypeFullName(t.name);
        static const std::set<std::string> valueTypes = {
            "System.Boolean", "System.Byte", "System.SByte", "System.Char", "System.Decimal", "System.Double",
            "System.Single", "System.Int32", "System.UInt32", "System.Int64", "System.UInt64", "System.Int16",
            "System.UInt16", "System.IntPtr", "System.UIntPtr"};
        if (valueTypes.count(full)) {
            TypeName nt;
            nt.name = "System.Nullable`1";
            nt.args.push_back(t);
            ResolvedType r = Resolve(nt, usings, ns, enclosing);
            r.name = t0;
            return r;
        }
    }
    std::string name = KeywordTypeFullName(t.name);
    const BclIndex &b = Bcl();
    // Source types: nested in an enclosing type, in the namespace chain, then imported.
    std::string simple = SimpleKey(name);
    bool qualified = name.find('.') != std::string::npos;
    auto sourceMatch = [&](const SourceType &st) -> int {
        // Higher is better; -1 no match.
        std::string full = st.FullName();
        if (qualified) {
            if (full == name) return 100;
            for (const std::string &u : usings) {
                if (full == u + "." + name) return 90;
            }
            if (!ns.empty() && full == ns + "." + name) return 95;
            return -1;
        }
        // Nested in an enclosing type?
        std::string encl;
        for (size_t i = 0; i < enclosing.size(); i++) {
            encl += (encl.empty() ? "" : ".") + enclosing[i];
        }
        if (!st.owner.empty()) {
            if (!encl.empty() && (encl == st.owner || encl.rfind(st.owner + ".", 0) == 0)) return 80;
            return -1;
        }
        // In the current namespace or a parent.
        std::string n2 = ns;
        for (;;) {
            if (st.ns == n2) return 70 + static_cast<int>(n2.size() > 0);
            size_t dot = n2.rfind('.');
            if (n2.empty()) break;
            n2 = dot == std::string::npos ? "" : n2.substr(0, dot);
        }
        for (const std::string &u : usings) {
            if (st.ns == u) return 60;
        }
        return -1;
    };
    int best = -1;
    const SourceType *bestType = nullptr;
    for (const auto &layer : layers_) {
        auto range = layer->by_name.equal_range(simple);
        for (auto it = range.first; it != range.second; ++it) {
            int score = sourceMatch(layer->types[it->second]);
            if (score > best) {
                best = score;
                bestType = &layer->types[it->second];
            }
        }
    }
    if (bestType) return FromSource(*bestType, t);
    // BCL.
    auto exact = b.by_full.find(name);
    if (exact != b.by_full.end()) return FromBcl(exact->second, t);
    auto br = b.by_simple.equal_range(simple);
    for (auto it = br.first; it != br.second; ++it) {
        std::string full = ParseTypeName(b.types[it->second].name).name;
        if (qualified) {
            for (const std::string &u : usings) {
                if (full == u + "." + name) return FromBcl(it->second, t);
            }
            continue;
        }
        std::string nsOf = full.substr(0, full.size() - simple.size() - (full.size() > simple.size() ? 1 : 0));
        for (const std::string &u : usings) {
            if (nsOf == u) return FromBcl(it->second, t);
        }
        if (!ns.empty() && (nsOf == ns || ns.rfind(nsOf + ".", 0) == 0)) return FromBcl(it->second, t);
    }
    return none;
}

std::vector<ResolvedType> Universe::Bases(const ResolvedType &t) const {
    std::vector<ResolvedType> out;
    if (t.bcl) {
        const BclIndex &b = Bcl();
        auto add = [&](const std::string &text) {
            if (text.empty()) return;
            TypeName n = Substitute(ParseTypeName(text), t.subst);
            auto it = b.by_full.find(n.name);
            if (it != b.by_full.end()) out.push_back(FromBcl(it->second, n));
        };
        std::string kind = t.bcl->kind;
        if (kind != "enum") add(t.bcl->base);
        else add("System.Enum");
        if (kind == "interface") add("System.Object");
        std::string ifs = t.bcl->interfaces;
        size_t start = 0;
        for (size_t i = 0; i <= ifs.size(); i++) {
            if (i == ifs.size() || ifs[i] == ';') {
                add(ifs.substr(start, i - start));
                start = i + 1;
            }
        }
        if (t.name.IsArray()) {
            // T[] implements IList<T>, IReadOnlyList<T>.
            add("System.Collections.Generic.IList<T>");
            add("System.Collections.Generic.IReadOnlyList<T>");
        }
    } else if (t.source) {
        std::vector<std::string> enclosing;
        if (!t.source->owner.empty()) enclosing.push_back(t.source->owner);
        enclosing.push_back(t.source->name);
        bool hasClassBase = false;
        for (const std::string &bs : t.source->bases) {
            TypeName n = Substitute(ParseTypeName(bs), t.subst);
            ResolvedType r = Resolve(n, {"System", "System.Collections.Generic"}, t.source->ns, enclosing);
            if (r.Known()) {
                if (r.Kind() != "interface") hasClassBase = true;
                out.push_back(r);
            }
        }
        const std::string &k = t.source->kind;
        TypeName fallback;
        if (k == "enum") fallback.name = "System.Enum";
        else if (k == "struct" || k == "record struct") fallback.name = "System.ValueType";
        else if (k == "delegate") fallback.name = "System.Delegate";
        else fallback.name = "System.Object";
        if (!hasClassBase && k != "interface") {
            const BclIndex &b = Bcl();
            auto it = b.by_full.find(fallback.name);
            if (it != b.by_full.end()) out.push_back(FromBcl(it->second, fallback));
        }
    }
    return out;
}

namespace {

std::string BclKindName(uint8_t kind) {
    switch (kind) {
    case kCsBclMethod:
        return "method";
    case kCsBclConstructor:
        return "constructor";
    case kCsBclProperty:
        return "property";
    case kCsBclField:
        return "field";
    case kCsBclEvent:
        return "event";
    case kCsBclEnumMember:
        return "enum member";
    case kCsBclIndexer:
        return "indexer";
    default:
        return "method";
    }
}

MemberInfo FromBclMember(const CsBclMember &m, const ResolvedType &owner) {
    MemberInfo mi;
    mi.bcl = &m;
    mi.name = m.name;
    mi.kind = BclKindName(m.kind);
    if (m.kind == kCsBclField && (m.flags & kCsBclConst)) mi.kind = "const";
    mi.type = Substitute(ParseTypeName(m.type), owner.subst);
    mi.signature = m.signature;
    mi.params = m.params;
    mi.doc = m.doc;
    mi.owner = ShowType(ParseTypeName(m.owner));
    mi.is_static = (m.flags & kCsBclStatic) != 0;
    mi.is_extension = (m.flags & kCsBclExtension) != 0;
    return mi;
}

MemberInfo FromSourceMember(const SourceMember &m, const ResolvedType &owner) {
    MemberInfo mi;
    mi.source = &m;
    mi.name = m.name;
    mi.kind = m.kind;
    mi.type = Substitute(ParseTypeName(m.type), owner.subst);
    mi.signature = m.signature;
    mi.params = m.params;
    mi.doc = m.doc;
    mi.owner = owner.source ? owner.source->name : "";
    mi.is_static = m.is_static;
    mi.is_extension = m.is_extension;
    return mi;
}

}  // namespace

std::vector<MemberInfo> Universe::Members(const ResolvedType &t, const std::string &name, bool include_inherited) const {
    std::vector<MemberInfo> out;
    std::set<std::string> seenTypes;
    std::set<std::string> hidden;  // names already found on a derived type (fields, properties) -- not overloads
    std::vector<ResolvedType> queue{t};
    size_t qi = 0;
    while (qi < queue.size() && qi < 64) {
        ResolvedType cur = queue[qi++];
        std::string key = cur.FullName();
        if (!seenTypes.insert(key).second) continue;
        std::set<std::string> namesHere;
        if (cur.bcl) {
            const BclIndex &b = Bcl();
            auto it = b.member_range.find(cur.bcl->name);
            if (it != b.member_range.end()) {
                for (size_t i = it->second.first; i < it->second.second; i++) {
                    const CsBclMember &m = b.members[i];
                    if (!name.empty() && name != m.name) continue;
                    if (qi > 1 && m.kind == kCsBclConstructor) continue;
                    if (m.flags & kCsBclExtension) continue;  // offered through Extensions()
                    if (qi > 1 && m.kind != kCsBclMethod && hidden.count(m.name)) continue;
                    out.push_back(FromBclMember(m, cur));
                    namesHere.insert(m.name);
                }
            }
        } else if (cur.source) {
            // The type's members from the layer that declares it (a type in the current file is also in the
            // workspace's index, with stale positions: the first layer that has members wins).
            for (const auto &layer : layers_) {
                auto range = layer->by_owner.equal_range(cur.source->FullName());
                if (range.first == range.second) continue;
                for (auto it = range.first; it != range.second; ++it) {
                    const SourceMember &m = layer->members[it->second];
                    if (!name.empty() && name != m.name) continue;
                    if (qi > 1 && m.kind == "constructor") continue;
                    if (qi > 1 && m.kind != "method" && hidden.count(m.name)) continue;
                    out.push_back(FromSourceMember(m, cur));
                    namesHere.insert(m.name);
                }
                break;
            }
        }
        hidden.insert(namesHere.begin(), namesHere.end());
        if (!include_inherited) break;
        for (ResolvedType &b : Bases(cur)) queue.push_back(b);
    }
    return out;
}

namespace {

/** @brief A generic method's type parameter names, from its signature ("... Select<TSource, TResult>(..."). */
std::vector<std::string> MethodTypeParams(const std::string &signature, const std::string &name) {
    std::vector<std::string> out;
    size_t p = signature.find(name + "<");
    if (p == std::string::npos) return out;
    size_t i = p + name.size() + 1;
    std::string cur;
    int depth = 0;
    for (; i < signature.size(); i++) {
        char c = signature[i];
        if (c == '<') depth++;
        if (c == '>') {
            if (depth == 0) break;
            depth--;
        }
        if (c == ',' && depth == 0) {
            out.push_back(cur);
            cur.clear();
            continue;
        }
        if (!std::isspace(static_cast<unsigned char>(c))) cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    for (std::string &tp : out) {
        for (const char *v : {"in", "out"}) {
            size_t n = std::strlen(v);
            if (tp.size() > n && tp.compare(0, n, v) == 0 && std::isupper(static_cast<unsigned char>(tp[n]))) tp = tp.substr(n);
        }
    }
    return out;
}

/** @brief Whether a receiver type `t` (with its bases) matches an extension's `this` parameter type; binds
 *         the extension's type parameters into *subst. */
bool MatchesThis(const Universe &u, const ResolvedType &t, const TypeName &thisType,
                 const std::vector<std::string> &typeParams, std::map<std::string, TypeName> *subst) {
    std::string want = KeywordTypeFullName(thisType.name);
    bool isTypeParam = std::find(typeParams.begin(), typeParams.end(), thisType.name) != typeParams.end();
    if (isTypeParam && thisType.args.empty()) {
        // `this T source` matches anything; `this T[] source` any array (T its element type).
        if (thisType.ranks.empty()) {
            (*subst)[want] = t.name;
            return true;
        }
        if (!t.name.IsArray()) return false;
        TypeName elem = t.name;
        elem.ranks.erase(elem.ranks.begin());
        (*subst)[want] = elem;
        return true;
    }
    if (thisType.IsArray()) {
        // `this int[] a`: the receiver must be an array (its element type is not checked).
        return t.name.IsArray();
    }
    std::vector<ResolvedType> queue{t};
    std::set<std::string> seen;
    for (size_t i = 0; i < queue.size() && i < 64; i++) {
        const ResolvedType &cur = queue[i];
        std::string full = cur.FullName();
        if (!seen.insert(full).second) continue;
        size_t dot = full.rfind('.');
        std::string simple = dot == std::string::npos ? full : full.substr(dot + 1);
        if (full == want || (want.find('.') == std::string::npos && simple == want)) {
            TypeName decl = cur.bcl ? ParseTypeName(cur.bcl->name) : TypeName();
            // Bind the extension's type arguments from the matched type's arguments.
            for (size_t k = 0; k < thisType.args.size(); k++) {
                TypeName actual;
                if (cur.bcl && k < decl.args.size()) {
                    auto f = cur.subst.find(decl.args[k].name);
                    if (f != cur.subst.end()) actual = f->second;
                } else if (cur.source && k < cur.source->type_params.size()) {
                    auto f = cur.subst.find(cur.source->type_params[k]);
                    if (f != cur.subst.end()) actual = f->second;
                }
                if (!actual.Empty()) (*subst)[thisType.args[k].name] = actual;
            }
            return true;
        }
        for (ResolvedType &b : u.Bases(cur)) queue.push_back(b);
    }
    return false;
}

}  // namespace

std::vector<MemberInfo> Universe::Extensions(const ResolvedType &t, const std::string &name,
                                             const std::vector<std::string> &usings) const {
    std::vector<MemberInfo> out;
    if (!t.Known() || t.is_static_ref) return out;
    const BclIndex &b = Bcl();
    std::set<std::string> imported(usings.begin(), usings.end());
    auto consider = [&](const CsBclMember &m) {
        // The owner's namespace must be imported.
        TypeName owner = ParseTypeName(m.owner);
        size_t dot = owner.name.rfind('.');
        std::string ns = dot == std::string::npos ? "" : owner.name.substr(0, dot);
        if (!imported.count(ns)) return;
        // "(this IEnumerable<TSource> source, ...)": the first parameter's type.
        std::string params = m.params;
        // The receiver is the first parameter (the table's parameter lists leave out its `this`).
        size_t thisPos = params.find("this ");
        size_t start = thisPos == std::string::npos ? 1 : thisPos + 5;
        int depth = 0;
        size_t end = start;
        for (; end < params.size(); end++) {
            char c = params[end];
            if (c == '<' || c == '(' || c == '[') depth++;
            if (c == '>' || c == ')' || c == ']') {
                if (depth == 0) break;
                depth--;
            }
            if (c == ',' && depth == 0) break;
        }
        std::string first = params.substr(start, end - start);
        size_t sp = first.rfind(' ');
        if (sp != std::string::npos) first = first.substr(0, sp);
        std::map<std::string, TypeName> subst;
        if (!MatchesThis(*this, t, ParseTypeName(first), MethodTypeParams(m.signature, m.name), &subst)) return;
        ResolvedType ownerT;
        ownerT.subst = subst;
        MemberInfo mi = FromBclMember(m, ownerT);
        out.push_back(mi);
    };
    if (!name.empty()) {
        auto range = b.extensions.equal_range(name);
        for (auto it = range.first; it != range.second; ++it) consider(b.members[it->second]);
    } else {
        for (const auto &kv : b.extensions) consider(b.members[kv.second]);
    }
    // Source extension methods (any imported namespace or the current one).
    for (const auto &layer : layers_)
    for (const SourceMember &m : layer->members) {
        if (!m.is_extension) continue;
        if (!name.empty() && m.name != name) continue;
        std::string params = m.params;
        size_t thisPos = params.find("this ");
        if (thisPos == std::string::npos) continue;
        std::string rest = params.substr(thisPos + 5);
        size_t end = rest.find_first_of(",)");
        std::string first = rest.substr(0, end);
        size_t sp = first.rfind(' ');
        if (sp != std::string::npos) first = first.substr(0, sp);
        std::map<std::string, TypeName> subst;
        if (!MatchesThis(*this, t, ParseTypeName(first), MethodTypeParams(m.signature, m.name), &subst)) continue;
        ResolvedType ownerT;
        ownerT.subst = subst;
        out.push_back(FromSourceMember(m, ownerT));
    }
    return out;
}

std::vector<ResolvedType> Universe::VisibleTypes(const std::vector<std::string> &usings, const std::string &ns) const {
    std::vector<ResolvedType> out;
    std::set<std::string> nss(usings.begin(), usings.end());
    std::string n2 = ns;
    for (;;) {
        nss.insert(n2);
        if (n2.empty()) break;
        size_t dot = n2.rfind('.');
        n2 = dot == std::string::npos ? "" : n2.substr(0, dot);
    }
    for (const std::string &n : nss) {
        for (ResolvedType &t : TypesIn(n)) out.push_back(t);
    }
    return out;
}

std::vector<ResolvedType> Universe::TypesIn(const std::string &ns) const {
    std::vector<ResolvedType> out;
    const BclIndex &b = Bcl();
    for (size_t i = 0; i < b.type_count; i++) {
        TypeName n = ParseTypeName(b.types[i].name);
        if (n.name.size() <= ns.size() + 1 || (!ns.empty() && (n.name.compare(0, ns.size(), ns) != 0 || n.name[ns.size()] != '.'))) {
            continue;
        }
        std::string rest = ns.empty() ? n.name : n.name.substr(ns.size() + 1);
        if (rest.find('.') != std::string::npos) continue;  // nested type or deeper namespace
        TypeName ref;
        ref.name = n.name;
        out.push_back(FromBcl(i, ref));
    }
    std::set<std::string> seen;
    for (const auto &layer : layers_)
    for (const SourceType &t : layer->types) {
        if (t.ns == ns && t.owner.empty() && seen.insert(t.FullName()).second) {
            TypeName ref;
            ref.name = t.name;
            out.push_back(FromSource(t, ref));
        }
    }
    return out;
}

bool Universe::IsNamespace(const std::string &ns) const {
    const BclIndex &b = Bcl();
    if (std::binary_search(b.namespaces.begin(), b.namespaces.end(), ns)) return true;
    for (const auto &layer : layers_) {
        if (std::find(layer->namespaces.begin(), layer->namespaces.end(), ns) != layer->namespaces.end()) return true;
    }
    return false;
}

std::vector<std::string> Universe::ChildNamespaces(const std::string &ns) const {
    std::set<std::string> out;
    auto consider = [&](const std::string &full) {
        if (!ns.empty() && (full.size() <= ns.size() || full.compare(0, ns.size(), ns) != 0 || full[ns.size()] != '.')) {
            return;
        }
        std::string rest = ns.empty() ? full : full.substr(ns.size() + 1);
        size_t dot = rest.find('.');
        out.insert(dot == std::string::npos ? rest : rest.substr(0, dot));
    };
    for (const std::string &n : Bcl().namespaces) consider(n);
    for (const auto &layer : layers_) {
        for (const std::string &n : layer->namespaces) consider(n);
    }
    return std::vector<std::string>(out.begin(), out.end());
}

// --- Model: the types of expressions ---------------------------------------------------------------

namespace {

ResolvedType Unknown() { return ResolvedType(); }

}  // namespace

ResolvedType Model::ResolveTypeText(const std::string &text, const SyntaxNode *context) const {
    Context ctx = ContextAt(doc, context);
    TypeName t = ParseTypeName(text);
    return universe.Resolve(t, ctx.usings, ctx.ns, ctx.enclosing);
}

ResolvedType Model::ResolveTypeNode(const SyntaxNode *type) const {
    if (!type) return Unknown();
    std::string text = doc.Text(type);
    if (text == "var") return Unknown();
    return ResolveTypeText(text, type);
}

ResolvedType Model::TypeOfSymbol(int symbol, int depth) const {
    if (symbol < 0 || depth > 12) return Unknown();
    const LocalSymbol &s = binding.symbols[static_cast<size_t>(symbol)];
    if (s.type && doc.Text(s.type) != "var") return ResolveTypeNode(s.type);
    if (s.kind == SymKind::ForEachVariable && s.init) {
        // foreach (var x in xs): the element type of xs.
        ResolvedType coll = TypeOf(s.init, depth + 1);
        if (coll.name.IsArray()) {
            TypeName e = coll.name;
            e.ranks.erase(e.ranks.begin());
            return ResolveTypeText(ShowType(e), s.init);
        }
        if (coll.Known()) {
            // Look for IEnumerable<T> among its bases.
            std::vector<ResolvedType> queue{coll};
            for (size_t i = 0; i < queue.size() && i < 64; i++) {
                if (queue[i].FullName() == "System.Collections.Generic.IEnumerable`1") {
                    auto f = queue[i].subst.find("T");
                    if (f != queue[i].subst.end()) {
                        Context ctx = ContextAt(doc, s.init);
                        return universe.Resolve(f->second, ctx.usings, ctx.ns, ctx.enclosing);
                    }
                }
                for (ResolvedType &b : universe.Bases(queue[i])) queue.push_back(b);
            }
        }
        return Unknown();
    }
    if (s.init && (s.kind == SymKind::Local || s.kind == SymKind::UsingVariable)) return TypeOf(s.init, depth + 1);
    if (s.kind == SymKind::CatchVariable) return ResolveTypeText("System.Exception", s.decl);
    return Unknown();
}

std::vector<MemberInfo> Model::EnclosingMembers(const SyntaxNode *node, const std::string &name) const {
    std::vector<MemberInfo> out;
    Context ctx = ContextAt(doc, node);
    // Innermost type first, then outer types.
    std::vector<std::string> encl = ctx.enclosing;
    while (!encl.empty()) {
        TypeName tn;
        tn.name = encl.back();
        std::vector<std::string> outer(encl.begin(), encl.end() - 1);
        ResolvedType t = universe.Resolve(tn, ctx.usings, ctx.ns, outer);
        if (t.Known()) {
            for (MemberInfo &m : universe.Members(t, name)) out.push_back(m);
            if (!out.empty()) return out;
        }
        encl.pop_back();
    }
    // Top-level statements' local functions.
    if (ctx.enclosing.empty()) {
        for (const auto &layer : universe.Layers())
        for (const SourceMember &m : layer->members) {
            if (m.owner_full == "Program" && (name.empty() || m.name == name)) {
                ResolvedType none;
                MemberInfo mi;
                mi.source = &m;
                mi.name = m.name;
                mi.kind = m.kind;
                mi.type = ParseTypeName(m.type);
                mi.signature = m.signature;
                mi.params = m.params;
                mi.doc = m.doc;
                out.push_back(mi);
            }
        }
    }
    return out;
}

std::vector<MemberInfo> Model::MembersOf(const ResolvedType &receiver, const std::string &name,
                                         const SyntaxNode *context) const {
    std::vector<MemberInfo> out;
    if (!receiver.Known()) return out;
    // `this.` and `base.` see protected members; other receivers do not.
    bool inside = false;
    if (context) {
        const SyntaxNode *left = context->Field(Field::Expression);
        inside = left && (left->Kind() == SyntaxKind::ThisExpression || left->Kind() == SyntaxKind::BaseExpression);
    }
    for (MemberInfo &m : universe.Members(receiver, name)) {
        if (!inside && m.bcl && (m.bcl->flags & kCsBclProtected)) continue;
        if (receiver.is_static_ref && !m.is_static && m.kind != "const" && m.kind != "enum member") continue;
        if (!receiver.is_static_ref && (m.is_static || m.kind == "const") && m.kind != "enum member") continue;
        if (m.kind == "constructor") continue;
        out.push_back(m);
    }
    if (!receiver.is_static_ref) {
        Context ctx = ContextAt(doc, context);
        for (MemberInfo &m : universe.Extensions(receiver, name, ctx.usings)) out.push_back(m);
    }
    return out;
}

ResolvedType Model::TypeOf(const SyntaxNode *e, int depth) const {
    if (!e || depth > 16) return Unknown();
    auto bcl = [&](const char *full) { return ResolveTypeText(full, e); };
    switch (e->Kind()) {
    case SyntaxKind::ParenthesizedExpression:
    case SyntaxKind::CheckedExpression:
    case SyntaxKind::UncheckedExpression:
    case SyntaxKind::SuppressNullableWarningExpression:
        return TypeOf(e->Field(Field::Expression) ? e->Field(Field::Expression) : e->Field(Field::Operand), depth + 1);
    case SyntaxKind::StringLiteralExpression:
    case SyntaxKind::InterpolatedStringExpression:
    case SyntaxKind::Utf8StringLiteralExpression:
        return bcl("System.String");
    case SyntaxKind::CharacterLiteralExpression:
        return bcl("System.Char");
    case SyntaxKind::TrueLiteralExpression:
    case SyntaxKind::FalseLiteralExpression:
    case SyntaxKind::LogicalNotExpression:
    case SyntaxKind::EqualsExpression:
    case SyntaxKind::NotEqualsExpression:
    case SyntaxKind::LessThanExpression:
    case SyntaxKind::LessThanOrEqualExpression:
    case SyntaxKind::GreaterThanExpression:
    case SyntaxKind::GreaterThanOrEqualExpression:
    case SyntaxKind::LogicalAndExpression:
    case SyntaxKind::LogicalOrExpression:
    case SyntaxKind::IsExpression:
    case SyntaxKind::IsPatternExpression:
        return bcl("System.Boolean");
    case SyntaxKind::NumericLiteralExpression: {
        const SyntaxNode *tok = e->Field(Field::Token);
        std::string text = tok ? Doc::TokenValue(tok) : "";
        std::string lower;
        for (char c : text) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        bool hex = lower.rfind("0x", 0) == 0 || lower.rfind("0b", 0) == 0;
        if (!hex && (lower.back() == 'f')) return bcl("System.Single");
        if (!hex && (lower.back() == 'd' || lower.find('.') != std::string::npos ||
                     (lower.find('e') != std::string::npos))) {
            return bcl(lower.back() == 'm' ? "System.Decimal" : "System.Double");
        }
        if (lower.back() == 'm') return bcl("System.Decimal");
        if (lower.size() >= 2 && (lower.substr(lower.size() - 2) == "ul" || lower.substr(lower.size() - 2) == "lu")) {
            return bcl("System.UInt64");
        }
        if (lower.back() == 'l') return bcl("System.Int64");
        if (lower.back() == 'u') return bcl("System.UInt32");
        return bcl("System.Int32");
    }
    case SyntaxKind::ThisExpression:
    case SyntaxKind::BaseExpression: {
        Context ctx = ContextAt(doc, e);
        if (ctx.enclosing.empty()) return Unknown();
        TypeName tn;
        tn.name = ctx.enclosing.back();
        std::vector<std::string> outer(ctx.enclosing.begin(), ctx.enclosing.end() - 1);
        ResolvedType t = universe.Resolve(tn, ctx.usings, ctx.ns, outer);
        if (e->Kind() == SyntaxKind::BaseExpression && t.Known()) {
            for (ResolvedType &b : universe.Bases(t)) {
                if (b.Kind() != "interface") return b;
            }
        }
        return t;
    }
    case SyntaxKind::ObjectCreationExpression:
    case SyntaxKind::ArrayCreationExpression:
    case SyntaxKind::StackAllocArrayCreationExpression:
        return ResolveTypeNode(e->Field(Field::Type));
    case SyntaxKind::ImplicitObjectCreationExpression: {
        // new(): the declared type of what it initializes.
        const SyntaxNode *p = e->parent;
        if (p && p->Kind() == SyntaxKind::EqualsValueClause && p->parent &&
            p->parent->Kind() == SyntaxKind::VariableDeclarator && p->parent->parent) {
            return ResolveTypeNode(p->parent->parent->Field(Field::Type));
        }
        return Unknown();
    }
    case SyntaxKind::CastExpression:
    case SyntaxKind::DefaultExpression:
        return ResolveTypeNode(e->Field(Field::Type));
    case SyntaxKind::AsExpression:
        return ResolveTypeNode(e->Field(Field::Right));
    case SyntaxKind::TypeOfExpression:
        return bcl("System.Type");
    case SyntaxKind::AwaitExpression: {
        ResolvedType t = TypeOf(e->Field(Field::Expression), depth + 1);
        std::string full = t.FullName();
        if (full == "System.Threading.Tasks.Task`1" || full == "System.Threading.Tasks.ValueTask`1") {
            auto f = t.subst.find("TResult");
            if (f != t.subst.end()) {
                Context ctx = ContextAt(doc, e);
                return universe.Resolve(f->second, ctx.usings, ctx.ns, ctx.enclosing);
            }
        }
        return Unknown();
    }
    case SyntaxKind::ConditionalExpression: {
        ResolvedType t = TypeOf(e->Field(Field::WhenTrue), depth + 1);
        return t.Known() ? t : TypeOf(e->Field(Field::WhenFalse), depth + 1);
    }
    case SyntaxKind::CoalesceExpression:
        return TypeOf(e->Field(Field::Left), depth + 1);
    case SyntaxKind::AddExpression: {
        ResolvedType l = TypeOf(e->Field(Field::Left), depth + 1);
        if (l.FullName() == "System.String") return l;
        ResolvedType r = TypeOf(e->Field(Field::Right), depth + 1);
        if (r.FullName() == "System.String") return r;
        return l;
    }
    case SyntaxKind::SubtractExpression:
    case SyntaxKind::MultiplyExpression:
    case SyntaxKind::DivideExpression:
    case SyntaxKind::ModuloExpression:
        return TypeOf(e->Field(Field::Left), depth + 1);
    case SyntaxKind::SimpleAssignmentExpression:
        return TypeOf(e->Field(Field::Left), depth + 1);
    case SyntaxKind::IdentifierName:
    case SyntaxKind::GenericName: {
        const SyntaxNode *tok = e->Field(Field::Identifier);
        int sym = tok ? SymbolAt(binding, tok) : -1;
        if (sym >= 0) return TypeOfSymbol(sym, depth + 1);
        std::string name = Doc::TokenValue(tok);
        if (name.empty()) return Unknown();
        // A member of the enclosing type?
        std::vector<MemberInfo> ms = EnclosingMembers(e, name);
        for (const MemberInfo &m : ms) {
            if (m.kind == "method" || m.kind == "constructor") continue;
            Context ctx = ContextAt(doc, e);
            ResolvedType t = universe.Resolve(m.type, ctx.usings, ctx.ns, ctx.enclosing);
            if (t.Known()) return t;
        }
        // A type name (static access)?
        ResolvedType t = ResolveTypeText(doc.Text(e), e);
        if (t.Known()) {
            t.is_static_ref = true;
            return t;
        }
        // A namespace?
        if (universe.IsNamespace(name)) {
            ResolvedType n;
            n.ns = name;
            return n;
        }
        return Unknown();
    }
    case SyntaxKind::PredefinedType: {
        ResolvedType t = ResolveTypeText(doc.Text(e), e);
        t.is_static_ref = true;
        return t;
    }
    case SyntaxKind::SimpleMemberAccessExpression:
    case SyntaxKind::QualifiedName: {
        const SyntaxNode *left = e->Field(Field::Expression) ? e->Field(Field::Expression) : e->Field(Field::Left);
        const SyntaxNode *right = e->Field(Field::Name) ? e->Field(Field::Name) : e->Field(Field::Right);
        ResolvedType l = TypeOf(left, depth + 1);
        std::string name = right ? Doc::TokenValue(right->Field(Field::Identifier)) : "";
        if (!l.ns.empty()) {
            std::string full = l.ns + "." + name;
            ResolvedType t = ResolveTypeText(full, e);
            if (t.Known()) {
                t.is_static_ref = true;
                return t;
            }
            if (universe.IsNamespace(full)) {
                ResolvedType n;
                n.ns = full;
                return n;
            }
            return Unknown();
        }
        if (!l.Known()) return Unknown();
        for (const MemberInfo &m : MembersOf(l, name, e)) {
            if (m.kind == "method") continue;
            Context ctx = ContextAt(doc, e);
            ResolvedType t = universe.Resolve(m.type, ctx.usings, ctx.ns, ctx.enclosing);
            if (t.Known()) return t;
        }
        // A nested type (Outer.Inner).
        if (l.is_static_ref) {
            ResolvedType t = ResolveTypeText(doc.Text(e), e);
            if (t.Known()) {
                t.is_static_ref = true;
                return t;
            }
        }
        return Unknown();
    }
    case SyntaxKind::InvocationExpression: {
        const SyntaxNode *target = e->Field(Field::Expression);
        const SyntaxNode *args = e->Field(Field::ArgumentList);
        size_t argc = 0;
        if (args) {
            for (const SyntaxNode *a : args->FieldItems(Field::Arguments)) {
                if (!a->IsToken()) argc++;
            }
        }
        std::vector<MemberInfo> candidates;
        if (target && target->Kind() == SyntaxKind::SimpleMemberAccessExpression) {
            ResolvedType l = TypeOf(target->Field(Field::Expression), depth + 1);
            const SyntaxNode *nm = target->Field(Field::Name);
            std::string name = nm ? Doc::TokenValue(nm->Field(Field::Identifier)) : "";
            candidates = MembersOf(l, name, e);
        } else if (target && (target->Kind() == SyntaxKind::IdentifierName || target->Kind() == SyntaxKind::GenericName)) {
            const SyntaxNode *tok = target->Field(Field::Identifier);
            int sym = tok ? SymbolAt(binding, tok) : -1;
            if (sym >= 0) {
                const LocalSymbol &s = binding.symbols[static_cast<size_t>(sym)];
                if (s.kind == SymKind::LocalFunction && s.type) return ResolveTypeNode(s.type);
                return Unknown();
            }
            candidates = EnclosingMembers(e, Doc::TokenValue(tok));
        }
        // Prefer a method whose parameter count fits.
        const MemberInfo *best = nullptr;
        for (const MemberInfo &m : candidates) {
            if (m.kind != "method") continue;
            size_t commas = 0;
            int depthP = 0;
            bool any = false;
            for (char c : m.params) {
                if (c == '<' || c == '(' || c == '[') depthP++;
                else if (c == '>' || c == ')' || c == ']') depthP--;
                else if (c == ',' && depthP == 1) commas++;
                else if (!std::isspace(static_cast<unsigned char>(c)) && depthP == 1) any = true;
            }
            size_t count = any ? commas + 1 : 0;
            if (m.is_extension && count > 0) count--;
            if (!best || count == argc) best = &m;
            if (count == argc) break;
        }
        if (!best) return Unknown();
        Context ctx = ContextAt(doc, e);
        TypeName rt = best->type;
        // An extension's unbound type parameters stay unknown.
        return universe.Resolve(rt, ctx.usings, ctx.ns, ctx.enclosing);
    }
    case SyntaxKind::ElementAccessExpression: {
        ResolvedType l = TypeOf(e->Field(Field::Expression), depth + 1);
        if (l.name.IsArray()) {
            TypeName el = l.name;
            el.ranks.erase(el.ranks.begin());
            Context ctx = ContextAt(doc, e);
            return universe.Resolve(el, ctx.usings, ctx.ns, ctx.enclosing);
        }
        for (const MemberInfo &m : universe.Members(l, "")) {
            if (m.kind == "indexer") {
                Context ctx = ContextAt(doc, e);
                return universe.Resolve(m.type, ctx.usings, ctx.ns, ctx.enclosing);
            }
        }
        return Unknown();
    }
    default:
        return Unknown();
    }
}

}  // namespace cslsp
