#ifndef MEP_MEPML_ELEMENT_H
#define MEP_MEPML_ELEMENT_H

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// mepml's structure as style sheets see it (docs/mepml-spec/structure.md):
// every construct is an element with a name and attributes, some with
// parts (`box::label`), each under its ancestors. mepml_doc.cpp says which
// element everything in a document is; mepml_style.cpp computes a style for
// an element from the sheets; neither knows how anything is drawn.

namespace mepml {

struct Element {
    std::string name;  // "heading", "box", "bold" ...
    std::vector<std::pair<std::string, std::string>> attrs;  // ("level", "2"), ("kind", "definition")
    std::string part;  // "" for the element itself, else "label", "markup" ...

    Element() = default;
    explicit Element(std::string n) : name(std::move(n)) {}
    Element(std::string n, std::string attr, std::string value) : name(std::move(n)) {
        attrs.emplace_back(std::move(attr), std::move(value));
    }
    // The attribute's value, nullptr when the element has none by that name.
    const std::string *Attr(const std::string &attr) const {
        for (const auto &kv : attrs)
            if (kv.first == attr) return &kv.second;
        return nullptr;
    }
    Element &With(std::string attr, std::string value = "") {
        attrs.emplace_back(std::move(attr), std::move(value));
        return *this;
    }
    // This element's `part`.
    Element Part(std::string p) const {
        Element e = *this;
        e.part = std::move(p);
        return e;
    }
};

// Every distinct path from the document down to an element, as a trie: a
// node is an element under its parent node. A document's thousands of
// spans share a few dozen paths, so a style is computed once per node
// rather than once per span.
struct ElementPaths {
    struct Node {
        int parent = -1;  // -1: directly under the root
        Element element;
    };
    std::vector<Node> nodes;

    // The node for `element` under `parent` (-1 for the root), made on first use.
    int Intern(int parent, const Element &element) {
        std::string key = std::to_string(parent);
        key += '\x1f';
        key += element.name;
        key += '\x1f';
        key += element.part;
        for (const auto &kv : element.attrs) {
            key += '\x1f';
            key += kv.first;
            key += '=';
            key += kv.second;
        }
        auto it = index_.find(key);
        if (it != index_.end()) return it->second;
        const int id = static_cast<int>(nodes.size());
        nodes.push_back({parent, element});
        index_.emplace(std::move(key), id);
        return id;
    }
    // The elements from the outermost down to `node`.
    std::vector<const Element *> Path(int node) const {
        std::vector<const Element *> out;
        for (int n = node; n >= 0 && n < static_cast<int>(nodes.size()); n = nodes[static_cast<size_t>(n)].parent)
            out.push_back(&nodes[static_cast<size_t>(n)].element);
        return {out.rbegin(), out.rend()};
    }

  private:
    std::unordered_map<std::string, int> index_;
};

}  // namespace mepml

#endif  // MEP_MEPML_ELEMENT_H
