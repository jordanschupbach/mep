#pragma once

// mep's own in-house minimal XML DOM parser + writer -- see
// PUGIXML_REMOVAL_PLAN.md for the full writeup. Replaces the vendored
// `pugixml` dependency for src/office_doc.cpp/office_odt.cpp/
// doc_export.cpp's DOCX/ODT XML-part reading and writing.
//
// Deliberately shaped like pugixml's own lightweight-handle API (`xml::
// xml_node`/`xml_attribute`/`xml_document`, `node`/`attribute`/`children`/
// `append_child`/`append_attribute`/`set_value`/`save` with the same call
// shapes) rather than a from-scratch redesign -- every one of this
// module's three consumers keeps its existing reading/writing logic
// almost verbatim, needing only a namespace/type-name retarget, not a
// rewrite. See that plan's Scoping decisions for what's deliberately
// NOT supported: XPath, namespace resolution (prefixed names like
// "w:pPr" are opaque strings, matching every real call site), DTD/
// external-entity resolution, non-UTF-8 encodings, streaming parse.

#include <cstddef>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace xml {

enum xml_node_type {
    node_null,
    node_document,
    node_element,
    node_pcdata,
    node_cdata,
    node_declaration,
};

// Accepted (for load_buffer's pugixml-shaped call signature) and
// ignored: this parser has exactly one behavior -- a lenient, UTF-8-only
// parse tolerant of self-closing tags, mixed content, the 5 standard
// entities plus numeric character references, and CDATA sections --
// see PUGIXML_REMOVAL_PLAN.md's Scoping decisions for why a flags/
// encoding system isn't needed.
enum xml_parse_flags { parse_default };
enum xml_encoding { encoding_utf8 };
enum xml_format_flags { format_raw };

struct Node;  // internal tree node, defined in xml_doc.cpp

// Lightweight, copyable handle onto one attribute (mirrors pugixml's own
// xml_attribute: doesn't own the data, "empty"/null-safe like xml_node).
class xml_attribute {
public:
    xml_attribute() = default;
    xml_attribute(Node *owner, std::size_t index) : owner_(owner), index_(index) {}
    explicit operator bool() const;
    bool operator!() const { return !static_cast<bool>(*this); }
    const char *as_string(const char *def = "") const;
    int as_int(int def = 0) const;
    void set_value(const char *v);
    void set_value(const std::string &v) { set_value(v.c_str()); }
    void set_value(int v);
    void set_value(unsigned int v);

private:
    Node *owner_ = nullptr;
    std::size_t index_ = static_cast<std::size_t>(-1);
};

// Wraps an element node's first pcdata/cdata child, if any -- matches
// pugixml's xml_node::text() (NOT the node's own value(), which is only
// meaningful when the node itself IS a text node; see CollectOdtInline's
// own child.value() use for that distinction in office_odt.cpp).
class xml_text {
public:
    explicit xml_text(Node *n) : node_(n) {}
    const char *get() const;
    // Sets (creating if absent) this node's first pcdata child's value --
    // matches pugixml's xml_text::set(), used by every writer call site
    // that builds a <w:t>/<text:p>-style leaf via `.text().set(...)`
    // instead of the more verbose `.append_child(node_pcdata).set_value(...)`.
    void set(const char *v);
    void set(const std::string &v) { set(v.c_str()); }

private:
    Node *node_ = nullptr;
};

class xml_node {
public:
    xml_node() = default;
    explicit xml_node(Node *n) : node_(n) {}
    explicit operator bool() const { return node_ != nullptr; }
    bool operator!() const { return node_ == nullptr; }
    bool empty() const { return node_ == nullptr; }
    bool operator==(const xml_node &o) const { return node_ == o.node_; }

    const char *name() const;
    const char *value() const;
    xml_node_type type() const;

    xml_node child(const char *name) const;
    std::vector<xml_node> children() const;
    std::vector<xml_node> children(const char *name) const;
    xml_node first_child() const;
    xml_node next_sibling() const;

    xml_attribute attribute(const char *name) const;
    xml_text text() const { return xml_text(node_); }

    xml_node append_child(const char *name);
    xml_node append_child(xml_node_type type);
    xml_node insert_child_before(const char *name, const xml_node &anchor);
    xml_attribute append_attribute(const char *name);
    bool remove_child(const xml_node &child);
    void set_value(const char *v);

private:
    Node *node_ = nullptr;
};

// Matches pugixml's own xml_parse_result: bool-convertible (this
// module's parser is deliberately lenient/tolerant of real-world XML
// quirks -- see this file's own top comment -- so it only ever reports
// failure for genuinely empty/unparseable input, not minor spec
// deviations), with a description() for the same "malformed X: ..."
// user-facing error strings pugixml's own callers already build.
class xml_parse_result {
public:
    explicit xml_parse_result(bool ok, const char *desc = "") : ok_(ok), description_(desc) {}
    explicit operator bool() const { return ok_; }
    bool operator!() const { return !ok_; }
    const char *description() const { return description_.c_str(); }

private:
    bool ok_;
    std::string description_;
};

class xml_document {
public:
    xml_document();
    ~xml_document();
    xml_document(const xml_document &) = delete;
    xml_document &operator=(const xml_document &) = delete;

    xml_parse_result load_buffer(const void *data, std::size_t size, xml_parse_flags flags = parse_default,
                                  xml_encoding encoding = encoding_utf8);

    xml_node child(const char *name) const;
    xml_node append_child(const char *name);
    xml_node append_child(xml_node_type type);
    void save(std::ostream &os, const char *indent = "", xml_format_flags flags = format_raw) const;
    // Discards all content, back to a freshly-constructed empty document
    // -- matches pugixml's xml_document::reset(), used when a loaded
    // document turned out to have no usable root and needs rebuilding
    // from scratch (see office_doc.cpp's word/_rels/document.xml.rels
    // handling).
    void reset();

private:
    std::unique_ptr<Node> root_;
};

}  // namespace xml
