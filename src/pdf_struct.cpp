#include "pdf_struct.h"

#include <functional>
#include <map>
#include <set>

namespace pdfstruct {

namespace {

struct Reader {
    const unsigned char *data;
    size_t len;
    const pdfxref::XrefTable &table;
    const pdfdoc::PdfDocument &document;
    Tree *tree;
    pdfobj::Object role_map;
    std::set<int> seen;  // object numbers of the elements read: a tree that loops is cut
    std::map<int, int> element_of;  // object number -> index into tree->elements
    size_t budget = 2000000;  // elements + kids: a bound on what a hostile file can make us build

    pdfobj::Object Deref(const pdfobj::Object &o) const {
        pdfobj::Object cur = o;
        for (int hops = 0; cur.IsReference() && hops < 16; ++hops)
            cur = pdfxref::ResolveObject(data, len, table, cur.ref_val.num, cur.ref_val.gen);
        return cur;
    }
    std::string Text(const pdfobj::Object &dict, const char *key, bool *has = nullptr) const {
        const pdfobj::Object *v = dict.Find(key);
        if (!v) return "";
        const pdfobj::Object o = Deref(*v);
        if (!o.IsString()) return "";
        if (has) *has = true;
        return pdfobj::TextStringToUtf8(o.str_val);
    }
    int PageOf(const pdfobj::Object *ref) const {
        if (!ref || !ref->IsReference()) return -1;
        return document.PageIndexForObjectNum(ref->ref_val.num);
    }
    std::string Role(const std::string &type) const {
        std::string role = type;
        // (A map may chain: a -> b -> a standard type.)
        for (int hops = 0; hops < 8 && role_map.IsDict(); ++hops) {
            const pdfobj::Object *to = role_map.Find(role);
            if (!to) break;
            const std::string next = Deref(*to).AsString("");
            if (next.empty() || next == role) break;
            role = next;
        }
        return role;
    }
    // /A: one attribute object or an array of them (revision numbers
    // between); each a dict (or a stream's) with an /O owner.
    void Attributes(const pdfobj::Object &a, Element *e) const {
        const pdfobj::Object o = Deref(a);
        if (o.IsArray()) {
            for (const pdfobj::Object &item : o.array_val) Attributes(item, e);
            return;
        }
        if (!o.IsDict()) return;
        if (const pdfobj::Object *v = o.Find("Summary")) {
            const pdfobj::Object s = Deref(*v);
            if (s.IsString()) e->summary = pdfobj::TextStringToUtf8(s.str_val);
        }
        if (const pdfobj::Object *v = o.Find("Scope")) e->scope = Deref(*v).AsString("");
        if (const pdfobj::Object *v = o.Find("ListNumbering")) e->list_numbering = Deref(*v).AsString("");
    }

    // One entry of a /K: an element, a marked-content reference, an
    // object reference, or a bare MCID on the element's own page.
    void AddKid(const pdfobj::Object &k, int parent, int depth) {
        if (budget == 0) return;
        --budget;
        if (k.IsNumber()) {
            Kid kid;
            kid.kind = Kid::Kind::Content;
            kid.mcid = static_cast<int>(k.AsDouble());
            kid.page = parent >= 0 ? tree->elements[static_cast<size_t>(parent)].page : -1;
            if (parent >= 0) tree->elements[static_cast<size_t>(parent)].kids.push_back(kid);
            return;
        }
        const int obj_num = k.IsReference() ? k.ref_val.num : -1;
        const pdfobj::Object o = Deref(k);
        if (!o.IsDict()) return;
        const std::string type = o.Find("Type") ? Deref(*o.Find("Type")).AsString("") : "";
        if (type == "MCR" || type == "OBJR") {
            Kid kid;
            kid.kind = type == "MCR" ? Kid::Kind::Content : Kid::Kind::Object;
            kid.page = PageOf(o.Find("Pg"));
            if (kid.page < 0 && parent >= 0) kid.page = tree->elements[static_cast<size_t>(parent)].page;
            if (const pdfobj::Object *id = o.Find("MCID")) kid.mcid = static_cast<int>(Deref(*id).AsDouble(-1));
            if (parent >= 0) tree->elements[static_cast<size_t>(parent)].kids.push_back(kid);
            return;
        }
        // A structure element (its /Type is optional).
        if (!o.Find("S") || depth > 200) return;
        if (obj_num >= 0 && !seen.insert(obj_num).second) return;
        Element e;
        e.type = Deref(*o.Find("S")).AsString("");
        e.role = Role(e.type);
        e.alt = Text(o, "Alt", &e.has_alt);
        e.actual_text = Text(o, "ActualText");
        e.title = Text(o, "T");
        e.lang = Text(o, "Lang");
        e.expansion = Text(o, "E");
        e.id = Text(o, "ID");
        e.page = PageOf(o.Find("Pg"));
        if (e.page < 0 && parent >= 0) e.page = tree->elements[static_cast<size_t>(parent)].page;
        e.parent = parent;
        if (const pdfobj::Object *a = o.Find("A")) Attributes(*a, &e);
        const int index = static_cast<int>(tree->elements.size());
        tree->elements.push_back(std::move(e));
        if (obj_num >= 0) element_of[obj_num] = index;
        if (parent >= 0) {
            Kid kid;
            kid.element = index;
            tree->elements[static_cast<size_t>(parent)].kids.push_back(kid);
        } else {
            tree->roots.push_back(index);
        }
        if (const pdfobj::Object *kids = o.Find("K")) AddKids(*kids, index, depth + 1);
    }
    void AddKids(const pdfobj::Object &k, int parent, int depth) {
        // (An array is looked through here: its entries may be references
        // to elements, which AddKid must see as references.)
        const pdfobj::Object o = k.IsReference() ? Deref(k) : k;
        if (o.IsArray()) {
            for (const pdfobj::Object &item : o.array_val) AddKid(item, parent, depth);
        } else {
            AddKid(k, parent, depth);
        }
    }
};

}  // namespace

Tree Read(const unsigned char *data, size_t len, const pdfxref::XrefTable &table, const pdfdoc::PdfDocument &document) {
    Tree tree;
    Reader r{data, len, table, document, &tree, {}, {}, {}};
    if (const pdfobj::Object *info_ref = table.Trailer().Find("Info")) {
        const pdfobj::Object info = r.Deref(*info_ref);
        if (info.IsDict()) tree.title = r.Text(info, "Title");
    }
    const pdfobj::Object *root_ref = table.Trailer().Find("Root");
    if (!root_ref) return tree;
    const pdfobj::Object catalog = r.Deref(*root_ref);
    if (!catalog.IsDict()) return tree;
    tree.lang = r.Text(catalog, "Lang");
    if (const pdfobj::Object *mi = catalog.Find("MarkInfo")) {
        const pdfobj::Object info = r.Deref(*mi);
        if (const pdfobj::Object *m = info.IsDict() ? info.Find("Marked") : nullptr) {
            const pdfobj::Object v = r.Deref(*m);
            tree.marked = v.type == pdfobj::Type::Bool && v.bool_val;
        }
    }
    if (const pdfobj::Object *vp = catalog.Find("ViewerPreferences")) {
        const pdfobj::Object prefs = r.Deref(*vp);
        if (const pdfobj::Object *d = prefs.IsDict() ? prefs.Find("DisplayDocTitle") : nullptr) {
            const pdfobj::Object v = r.Deref(*d);
            tree.display_title = v.type == pdfobj::Type::Bool && v.bool_val;
        }
    }
    const pdfobj::Object *str_ref = catalog.Find("StructTreeRoot");
    if (!str_ref) return tree;
    const pdfobj::Object root = r.Deref(*str_ref);
    if (!root.IsDict()) return tree;
    tree.present = true;
    tree.parent_tree = root.Find("ParentTree") != nullptr;
    if (const pdfobj::Object *rm = root.Find("RoleMap")) r.role_map = r.Deref(*rm);
    if (const pdfobj::Object *kids = root.Find("K")) r.AddKids(*kids, -1, 0);
    // The parent tree: a number tree (spec 7.9.7) from each page's
    // /StructParents to the array of its marked content's elements.
    if (const pdfobj::Object *pt = root.Find("ParentTree")) {
        std::map<int, pdfobj::Object> entries;
        int budget = 100000;
        std::function<void(const pdfobj::Object &, int)> walk = [&](const pdfobj::Object &node_ref, int depth) {
            const pdfobj::Object node = r.Deref(node_ref);
            if (!node.IsDict() || depth > 32 || --budget < 0) return;
            if (const pdfobj::Object *nums = node.Find("Nums")) {
                const pdfobj::Object arr = r.Deref(*nums);
                if (arr.IsArray())
                    for (size_t i = 0; i + 1 < arr.array_val.size(); i += 2)
                        if (arr.array_val[i].IsNumber()) entries[static_cast<int>(arr.array_val[i].AsDouble())] = arr.array_val[i + 1];
            }
            if (const pdfobj::Object *kids = node.Find("Kids")) {
                const pdfobj::Object arr = r.Deref(*kids);
                if (arr.IsArray())
                    for (const pdfobj::Object &k : arr.array_val) walk(k, depth + 1);
            }
        };
        walk(*pt, 0);
        tree.page_parents.resize(static_cast<size_t>(document.PageCount()));
        for (int p = 0; p < document.PageCount(); ++p) {
            const pdfdoc::Page *page = document.GetPage(p);
            const pdfobj::Object *key = page ? page->dict.Find("StructParents") : nullptr;
            if (!key || !r.Deref(*key).IsNumber()) continue;
            const auto it = entries.find(static_cast<int>(r.Deref(*key).AsDouble()));
            if (it == entries.end()) continue;
            const pdfobj::Object arr = r.Deref(it->second);
            if (!arr.IsArray()) continue;
            for (const pdfobj::Object &e : arr.array_val) {
                const auto found = e.IsReference() ? r.element_of.find(e.ref_val.num) : r.element_of.end();
                tree.page_parents[static_cast<size_t>(p)].push_back(found == r.element_of.end() ? -1 : found->second);
            }
        }
    }
    return tree;
}

bool Tree::ParentsConsistent() const {
    for (size_t e = 0; e < elements.size(); ++e)
        for (const Kid &k : elements[e].kids) {
            if (k.kind != Kid::Kind::Content) continue;
            if (k.page < 0 || k.page >= static_cast<int>(page_parents.size()) || k.mcid < 0) return false;
            const std::vector<int> &page = page_parents[static_cast<size_t>(k.page)];
            if (k.mcid >= static_cast<int>(page.size()) || page[static_cast<size_t>(k.mcid)] != static_cast<int>(e)) return false;
        }
    return true;
}

}  // namespace pdfstruct
