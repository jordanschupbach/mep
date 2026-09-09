#include "xml_doc.h"

#include <cctype>
#include <cstring>

namespace xml {

struct Attr {
    std::string name;
    std::string value;
};

struct Node {
    xml_node_type type = node_element;
    std::string name;   // element/declaration tag name; unused for pcdata/cdata
    std::string value;  // text content; only meaningful for pcdata/cdata
    std::vector<Attr> attrs;
    std::vector<std::unique_ptr<Node>> children;
    Node *parent = nullptr;

    Node *AppendChild(xml_node_type t) {
        auto child = std::make_unique<Node>();
        child->type = t;
        child->parent = this;
        Node *raw = child.get();
        children.push_back(std::move(child));
        return raw;
    }
};

// -- xml_attribute --------------------------------------------------------

xml_attribute::operator bool() const { return owner_ != nullptr && index_ < owner_->attrs.size(); }

const char *xml_attribute::as_string(const char *def) const {
    if (!static_cast<bool>(*this)) return def;
    return owner_->attrs[index_].value.c_str();
}

int xml_attribute::as_int(int def) const {
    if (!static_cast<bool>(*this)) return def;
    const std::string &v = owner_->attrs[index_].value;
    if (v.empty()) return def;
    try {
        return std::stoi(v);
    } catch (...) {
        return def;
    }
}

void xml_attribute::set_value(const char *v) {
    if (static_cast<bool>(*this)) owner_->attrs[index_].value = v ? v : "";
}

void xml_attribute::set_value(int v) { set_value(std::to_string(v)); }
void xml_attribute::set_value(unsigned int v) { set_value(std::to_string(v)); }

// -- xml_text ---------------------------------------------------------------

const char *xml_text::get() const {
    if (!node_) return "";
    for (const auto &c : node_->children) {
        if (c->type == node_pcdata || c->type == node_cdata) return c->value.c_str();
    }
    return "";
}

void xml_text::set(const char *v) {
    if (!node_) return;
    for (const auto &c : node_->children) {
        if (c->type == node_pcdata || c->type == node_cdata) {
            c->value = v ? v : "";
            return;
        }
    }
    Node *t = node_->AppendChild(node_pcdata);
    t->value = v ? v : "";
}

// -- xml_node -----------------------------------------------------------

const char *xml_node::name() const { return node_ ? node_->name.c_str() : ""; }
const char *xml_node::value() const { return node_ ? node_->value.c_str() : ""; }
xml_node_type xml_node::type() const { return node_ ? node_->type : node_null; }

xml_node xml_node::child(const char *name) const {
    if (!node_) return xml_node();
    for (const auto &c : node_->children) {
        if (c->type == node_element && c->name == name) return xml_node(c.get());
    }
    return xml_node();
}

std::vector<xml_node> xml_node::children() const {
    std::vector<xml_node> out;
    if (!node_) return out;
    out.reserve(node_->children.size());
    for (const auto &c : node_->children) out.emplace_back(c.get());
    return out;
}

std::vector<xml_node> xml_node::children(const char *name) const {
    std::vector<xml_node> out;
    if (!node_) return out;
    for (const auto &c : node_->children) {
        if (c->type == node_element && c->name == name) out.emplace_back(c.get());
    }
    return out;
}

xml_node xml_node::first_child() const {
    if (!node_ || node_->children.empty()) return xml_node();
    return xml_node(node_->children.front().get());
}

xml_node xml_node::next_sibling() const {
    if (!node_ || !node_->parent) return xml_node();
    const std::vector<std::unique_ptr<Node>> &siblings = node_->parent->children;
    for (std::size_t i = 0; i < siblings.size(); i++) {
        if (siblings[i].get() == node_) {
            if (i + 1 < siblings.size()) return xml_node(siblings[i + 1].get());
            return xml_node();
        }
    }
    return xml_node();
}

xml_attribute xml_node::attribute(const char *name) const {
    if (!node_) return xml_attribute();
    for (std::size_t i = 0; i < node_->attrs.size(); i++) {
        if (node_->attrs[i].name == name) return xml_attribute(node_, i);
    }
    return xml_attribute();
}

xml_node xml_node::append_child(const char *name) {
    if (!node_) return xml_node();
    Node *c = node_->AppendChild(node_element);
    c->name = name;
    return xml_node(c);
}

xml_node xml_node::append_child(xml_node_type type) {
    if (!node_) return xml_node();
    return xml_node(node_->AppendChild(type));
}

xml_node xml_node::insert_child_before(const char *name, const xml_node &anchor) {
    if (!node_) return xml_node();
    auto child = std::make_unique<Node>();
    child->type = node_element;
    child->name = name;
    child->parent = node_;
    Node *raw = child.get();
    auto &kids = node_->children;
    auto pos = kids.end();
    for (auto it = kids.begin(); it != kids.end(); ++it) {
        if (it->get() == anchor.node_) {
            pos = it;
            break;
        }
    }
    kids.insert(pos, std::move(child));  // pos == end() (anchor not found) falls back to append, matching a safe default
    return xml_node(raw);
}

void xml_node::set_value(const char *v) {
    if (node_) node_->value = v ? v : "";
}

xml_attribute xml_node::append_attribute(const char *name) {
    if (!node_) return xml_attribute();
    node_->attrs.push_back(Attr{name, ""});
    return xml_attribute(node_, node_->attrs.size() - 1);
}

bool xml_node::remove_child(const xml_node &child) {
    if (!node_ || !child.node_) return false;
    auto &kids = node_->children;
    for (auto it = kids.begin(); it != kids.end(); ++it) {
        if (it->get() == child.node_) {
            kids.erase(it);
            return true;
        }
    }
    return false;
}

// -- xml_document -------------------------------------------------------

xml_document::xml_document() : root_(std::make_unique<Node>()) { root_->type = node_document; }
xml_document::~xml_document() = default;

void xml_document::reset() {
    root_ = std::make_unique<Node>();
    root_->type = node_document;
}

xml_node xml_document::child(const char *name) const { return xml_node(root_.get()).child(name); }
xml_node xml_document::append_child(const char *name) { return xml_node(root_.get()).append_child(name); }
xml_node xml_document::append_child(xml_node_type type) { return xml_node(root_.get()).append_child(type); }

// -- Parsing --------------------------------------------------------------

namespace {

bool IsNameChar(unsigned char c) {
    return std::isalnum(c) || c == ':' || c == '_' || c == '-' || c == '.';
}

// Appends one decoded XML entity/character reference (the text right
// after '&', up to and including the terminating ';') as UTF-8 bytes.
// Unrecognized entities are passed through literally (matching real-
// world lenient parsers' behavior for the rare malformed-but-tolerated
// case, rather than aborting the whole parse).
void AppendEntity(const std::string &src, std::size_t &i, std::string &out) {
    std::size_t semi = src.find(';', i);
    if (semi == std::string::npos) {
        out.push_back('&');
        i++;
        return;
    }
    std::string ent = src.substr(i + 1, semi - i - 1);
    unsigned long cp = 0;
    bool numeric = false;
    if (!ent.empty() && ent[0] == '#') {
        numeric = true;
        if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')) {
            cp = std::strtoul(ent.c_str() + 2, nullptr, 16);
        } else {
            cp = std::strtoul(ent.c_str() + 1, nullptr, 10);
        }
    } else if (ent == "amp") {
        out.push_back('&');
    } else if (ent == "lt") {
        out.push_back('<');
    } else if (ent == "gt") {
        out.push_back('>');
    } else if (ent == "quot") {
        out.push_back('"');
    } else if (ent == "apos") {
        out.push_back('\'');
    } else {
        // Unknown named entity -- pass through verbatim rather than drop
        // or abort (real-world producer error tolerance).
        out.push_back('&');
        out += ent;
        out.push_back(';');
    }
    if (numeric) {
        // Encode the codepoint as UTF-8.
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    i = semi + 1;
}

std::string DecodeEntities(const std::string &raw) {
    std::string out;
    out.reserve(raw.size());
    std::size_t i = 0;
    while (i < raw.size()) {
        if (raw[i] == '&') {
            AppendEntity(raw, i, out);
        } else {
            out.push_back(raw[i]);
            i++;
        }
    }
    return out;
}

class Parser {
public:
    Parser(const char *data, std::size_t size) : s_(data, size) {}

    bool Parse(Node *root) {
        std::size_t i = 0;
        SkipMisc(i);
        ParseChildren(root, i, "");
        return true;  // lenient: never fails outright, matching this module's tolerance-over-strictness scope
    }

private:
    std::string s_;

    // Skips whitespace, the XML declaration, comments, and processing
    // instructions before the first real element -- none of these are
    // represented in the tree on the read side (nothing ever queries a
    // parsed document for its own declaration node; only the write side
    // needs to produce one, via xml_node::append_child(node_declaration)).
    void SkipMisc(std::size_t &i) {
        for (;;) {
            while (i < s_.size() && std::isspace(static_cast<unsigned char>(s_[i]))) i++;
            if (i + 1 < s_.size() && s_[i] == '<' && s_[i + 1] == '?') {
                std::size_t end = s_.find("?>", i);
                i = (end == std::string::npos) ? s_.size() : end + 2;
                continue;
            }
            if (i + 3 < s_.size() && s_.compare(i, 4, "<!--") == 0) {
                std::size_t end = s_.find("-->", i);
                i = (end == std::string::npos) ? s_.size() : end + 3;
                continue;
            }
            break;
        }
    }

    // Parses `parent`'s children starting at byte `i` until a matching
    // "</tag_name>" close (or end of input, tolerated the same way
    // real-world lenient parsers handle a truncated/malformed tail).
    void ParseChildren(Node *parent, std::size_t &i, const std::string &tag_name) {
        std::string text_run;
        auto flush_text = [&] {
            if (text_run.empty()) return;
            Node *t = parent->AppendChild(node_pcdata);
            t->value = DecodeEntities(text_run);
            text_run.clear();
        };
        while (i < s_.size()) {
            if (s_[i] != '<') {
                text_run.push_back(s_[i]);
                i++;
                continue;
            }
            // Comment.
            if (s_.compare(i, 4, "<!--") == 0) {
                flush_text();
                std::size_t end = s_.find("-->", i);
                i = (end == std::string::npos) ? s_.size() : end + 3;
                continue;
            }
            // CDATA.
            if (s_.compare(i, 9, "<![CDATA[") == 0) {
                flush_text();
                std::size_t end = s_.find("]]>", i);
                std::size_t content_start = i + 9;
                std::size_t content_end = (end == std::string::npos) ? s_.size() : end;
                Node *cd = parent->AppendChild(node_cdata);
                cd->value = s_.substr(content_start, content_end - content_start);
                i = (end == std::string::npos) ? s_.size() : end + 3;
                continue;
            }
            // Processing instruction (e.g. a stray <?...?> mid-document).
            if (i + 1 < s_.size() && s_[i + 1] == '?') {
                flush_text();
                std::size_t end = s_.find("?>", i);
                i = (end == std::string::npos) ? s_.size() : end + 2;
                continue;
            }
            // Close tag.
            if (i + 1 < s_.size() && s_[i + 1] == '/') {
                flush_text();
                std::size_t end = s_.find('>', i);
                i = (end == std::string::npos) ? s_.size() : end + 1;
                return;  // caller resumes right after this close tag
            }
            // Open (possibly self-closing) tag.
            flush_text();
            ParseElement(parent, i);
        }
        (void)tag_name;
    }

    void ParseElement(Node *parent, std::size_t &i) {
        i++;  // consume '<'
        std::size_t name_start = i;
        while (i < s_.size() && IsNameChar(static_cast<unsigned char>(s_[i]))) i++;
        std::string name = s_.substr(name_start, i - name_start);
        Node *el = parent->AppendChild(node_element);
        el->name = name;

        for (;;) {
            while (i < s_.size() && std::isspace(static_cast<unsigned char>(s_[i]))) i++;
            if (i >= s_.size()) return;
            if (s_[i] == '/' && i + 1 < s_.size() && s_[i + 1] == '>') {
                i += 2;
                return;  // self-closing: no children to parse
            }
            if (s_[i] == '>') {
                i++;
                break;
            }
            // Attribute: name, '=', quoted value.
            std::size_t attr_name_start = i;
            while (i < s_.size() && IsNameChar(static_cast<unsigned char>(s_[i]))) i++;
            std::string attr_name = s_.substr(attr_name_start, i - attr_name_start);
            if (attr_name.empty()) {
                i++;  // stray/unexpected byte -- skip forward rather than loop forever on malformed input
                continue;
            }
            while (i < s_.size() && std::isspace(static_cast<unsigned char>(s_[i]))) i++;
            std::string attr_value;
            if (i < s_.size() && s_[i] == '=') {
                i++;
                while (i < s_.size() && std::isspace(static_cast<unsigned char>(s_[i]))) i++;
                if (i < s_.size() && (s_[i] == '"' || s_[i] == '\'')) {
                    char quote = s_[i];
                    i++;
                    std::size_t val_start = i;
                    while (i < s_.size() && s_[i] != quote) i++;
                    attr_value = DecodeEntities(s_.substr(val_start, i - val_start));
                    if (i < s_.size()) i++;  // consume closing quote
                }
            }
            el->attrs.push_back(Attr{attr_name, attr_value});
        }
        ParseChildren(el, i, name);
    }
};

}  // namespace

xml_parse_result xml_document::load_buffer(const void *data, std::size_t size, xml_parse_flags, xml_encoding) {
    root_ = std::make_unique<Node>();
    root_->type = node_document;
    if (!data || size == 0) return xml_parse_result(false, "empty document");
    Parser p(static_cast<const char *>(data), size);
    p.Parse(root_.get());
    for (const auto &c : root_->children) {
        if (c->type == node_element) return xml_parse_result(true);
    }
    return xml_parse_result(false, "no element found");
}

// -- Writing --------------------------------------------------------------

namespace {

void WriteEscapedText(std::ostream &os, const std::string &s) {
    for (char c : s) {
        switch (c) {
            case '&':
                os << "&amp;";
                break;
            case '<':
                os << "&lt;";
                break;
            default:
                os << c;
        }
    }
}

void WriteEscapedAttr(std::ostream &os, const std::string &s) {
    for (char c : s) {
        switch (c) {
            case '&':
                os << "&amp;";
                break;
            case '<':
                os << "&lt;";
                break;
            case '"':
                os << "&quot;";
                break;
            default:
                os << c;
        }
    }
}

void WriteNode(std::ostream &os, const Node *n) {
    switch (n->type) {
        case node_declaration: {
            os << "<?" << n->name;
            for (const Attr &a : n->attrs) os << " " << a.name << "=\"" << a.value << "\"";
            os << "?>";
            return;
        }
        case node_pcdata:
            WriteEscapedText(os, n->value);
            return;
        case node_cdata:
            os << "<![CDATA[" << n->value << "]]>";
            return;
        case node_element: {
            os << "<" << n->name;
            for (const Attr &a : n->attrs) {
                os << " " << a.name << "=\"";
                WriteEscapedAttr(os, a.value);
                os << "\"";
            }
            if (n->children.empty()) {
                os << "/>";
                return;
            }
            os << ">";
            for (const auto &c : n->children) WriteNode(os, c.get());
            os << "</" << n->name << ">";
            return;
        }
        case node_document:
        case node_null:
        default:
            for (const auto &c : n->children) WriteNode(os, c.get());
            return;
    }
}

}  // namespace

void xml_document::save(std::ostream &os, const char *, xml_format_flags) const { WriteNode(os, root_.get()); }

}  // namespace xml
