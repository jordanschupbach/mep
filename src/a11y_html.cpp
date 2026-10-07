#include "a11y_html.h"

#include "html_doc.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>  // std::atoi -- previously only reached via the (Linux-only) PCH

namespace a11y {

namespace {

std::string Attr(const DomNode *n, const char *name) {
    const auto it = n->attrs.find(name);
    return it == n->attrs.end() ? "" : it->second;
}

bool IsBlank(const std::string &s) {
    return std::all_of(s.begin(), s.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)); });
}

std::string RawText(const DomNode *n) {
    if (n->type == DomNodeType::Text) return n->text;
    std::string out;
    for (const auto &c : n->children) out += RawText(c.get());
    return out;
}

// Runs of blanks as one space (the ends kept: they separate words from
// what comes before and after).
std::string Collapse(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (out.empty() || out.back() != ' ') out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

struct Builder {
    Document *doc;

    void Children(const DomNode *n, std::vector<Node> *out, bool pre) {
        for (const auto &c : n->children) Walk(c.get(), out, pre);
    }
    Node Make(Role role, const DomNode *n) {
        Node node;
        node.role = role;
        node.tag = n->tag;
        node.lang = Attr(n, "lang");
        return node;
    }
    void Walk(const DomNode *n, std::vector<Node> *out, bool pre) {
        if (n->type == DomNodeType::Text) {
            const std::string text = pre ? n->text : Collapse(n->text);
            if (text.empty()) return;
            if (!out->empty() && out->back().role == Role::Text) {
                out->back().text += text;
                return;
            }
            Node t;
            t.role = Role::Text;
            t.text = text;
            out->push_back(std::move(t));
            return;
        }
        if (n->type != DomNodeType::Element) {
            Children(n, out, pre);
            return;
        }
        const std::string &tag = n->tag;
        if (tag == "script" || tag == "style" || tag == "head" || tag == "template" || tag == "noscript") return;
        if (Attr(n, "aria-hidden") == "true" || n->attrs.count("hidden")) return;
        const std::string role = Attr(n, "role");
        if (tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6') {
            Node h = Make(Role::Heading, n);
            h.level = tag[1] - '0';
            Children(n, &h.children, false);
            out->push_back(std::move(h));
        } else if (role == "heading") {
            Node h = Make(Role::Heading, n);
            h.level = std::clamp(std::atoi(Attr(n, "aria-level").c_str()), 1, 6);
            Children(n, &h.children, false);
            out->push_back(std::move(h));
        } else if (tag == "img" || role == "img") {
            Node f = Make(Role::Figure, n);
            const bool has = n->attrs.count("alt") || n->attrs.count("aria-label");
            f.alt = n->attrs.count("aria-label") ? Attr(n, "aria-label") : Attr(n, "alt");
            f.has_alt = has;
            f.decorative = role == "presentation" || role == "none" || (has && IsBlank(f.alt));
            out->push_back(std::move(f));
        } else if (tag == "math" || role == "math") {
            Node f = Make(Role::Formula, n);
            // The label is the formula's own, or the element's wrapping it.
            for (const DomNode *a = n; a && f.alt.empty(); a = a->parent) {
                if (a != n && Attr(a, "role") != "math") break;
                f.alt = Attr(a, "aria-label");
            }
            f.has_alt = !f.alt.empty();
            const std::string tex = Collapse(RawText(n));
            if (!IsBlank(tex)) {
                Node t;
                t.role = Role::Text;
                t.text = tex;
                f.children.push_back(std::move(t));
            }
            // (A display's caption is written inside its wrapper.)
            for (const auto &c : n->children)
                if (c->type == DomNodeType::Element && c->Class() == "caption") {
                    Node cap = Make(Role::Caption, c.get());
                    Children(c.get(), &cap.children, false);
                    f.children.erase(std::remove_if(f.children.begin(), f.children.end(), [](const Node &x) { return x.role == Role::Text; }),
                                     f.children.end());
                    std::string own;
                    for (const auto &k : n->children)
                        if (k.get() != c.get()) own += RawText(k.get());
                    Node t;
                    t.role = Role::Text;
                    t.text = Collapse(own);
                    f.children.push_back(std::move(t));
                    f.children.push_back(std::move(cap));
                }
            out->push_back(std::move(f));
        } else if (tag == "p") {
            Node p = Make(n->Class() == "caption" ? Role::Caption : Role::Paragraph, n);
            Children(n, &p.children, false);
            out->push_back(std::move(p));
        } else if (tag == "figcaption" || tag == "caption" || (tag == "div" && n->Class() == "caption")) {
            Node c = Make(Role::Caption, n);
            Children(n, &c.children, false);
            out->push_back(std::move(c));
        } else if (tag == "ul" || tag == "ol") {
            Node l = Make(Role::List, n);
            int number = 0;
            for (const auto &c : n->children) {
                if (c->type != DomNodeType::Element || c->tag != "li") continue;
                Node item = Make(Role::ListItem, c.get());
                Node label;
                label.role = Role::Label;
                Node t;
                t.role = Role::Text;
                t.text = tag == "ol" ? std::to_string(++number) + "." : "\xE2\x80\xA2";
                label.children.push_back(std::move(t));
                item.children.push_back(std::move(label));
                Children(c.get(), &item.children, false);
                l.children.push_back(std::move(item));
            }
            out->push_back(std::move(l));
        } else if (tag == "table") {
            Node t = Make(Role::Table, n);
            t.summary = Attr(n, "aria-description");
            if (t.summary.empty()) t.summary = Attr(n, "summary");
            Table(n, &t);
            out->push_back(std::move(t));
        } else if (tag == "blockquote") {
            Node q = Make(Role::Quote, n);
            Children(n, &q.children, false);
            out->push_back(std::move(q));
        } else if (tag == "pre") {
            Node c = Make(Role::Code, n);
            std::string text = RawText(n);
            if (!text.empty() && text.front() == '\n') text.erase(text.begin());
            Node t;
            t.role = Role::Text;
            t.text = text;
            c.children.push_back(std::move(t));
            out->push_back(std::move(c));
        } else if (tag == "a") {
            Node l = Make(Role::Link, n);
            if (n->attrs.count("aria-label")) {
                l.alt = Attr(n, "aria-label");
                l.has_alt = true;
            }
            Children(n, &l.children, false);
            out->push_back(std::move(l));
        } else if (tag == "section" || tag == "article" || tag == "aside" || tag == "nav") {
            Node s = Make(Role::Section, n);
            Children(n, &s.children, pre);
            if (!s.children.empty()) out->push_back(std::move(s));
        } else if (tag == "br") {
            Node t;
            t.role = Role::Text;
            t.text = " ";
            out->push_back(std::move(t));
        } else if (tag == "div" || tag == "figure" || tag == "body" || tag == "html" || tag == "main" || tag == "header" ||
                   tag == "footer" || tag == "li" || tag == "dl" || tag == "dt" || tag == "dd") {
            // A block that is nothing itself: its content, kept apart
            // from the text around it.
            Node g = Make(Role::Group, n);
            Children(n, &g.children, pre);
            if (g.children.empty()) return;
            const bool blocks = std::any_of(g.children.begin(), g.children.end(),
                                            [](const Node &c) { return c.role != Role::Text && c.role != Role::Link; });
            if (blocks) {
                for (Node &c : g.children) out->push_back(std::move(c));
            } else {
                g.role = Role::Paragraph;
                out->push_back(std::move(g));
            }
        } else {
            Children(n, out, pre);  // inline markup: its text is the line's
        }
    }
    void Table(const DomNode *n, Node *t) {
        for (const auto &c : n->children) {
            if (c->type != DomNodeType::Element) continue;
            if (c->tag == "caption") {
                Node cap = Make(Role::Caption, c.get());
                Children(c.get(), &cap.children, false);
                t->children.push_back(std::move(cap));
            } else if (c->tag == "tr") {
                Node row = Make(Role::Row, c.get());
                for (const auto &cell : c->children) {
                    if (cell->type != DomNodeType::Element || (cell->tag != "td" && cell->tag != "th")) continue;
                    Node x = Make(cell->tag == "th" ? Role::HeaderCell : Role::Cell, cell.get());
                    if (cell->tag == "th") x.scope = Attr(cell.get(), "scope") == "row" ? "row" : "column";
                    Children(cell.get(), &x.children, false);
                    row.children.push_back(std::move(x));
                }
                t->children.push_back(std::move(row));
            } else {
                Table(c.get(), t);  // thead / tbody / tfoot
            }
        }
    }
    void Head(const DomNode *n) {
        if (n->type != DomNodeType::Element && n->children.empty()) return;
        if (n->tag == "html" && doc->lang.empty()) doc->lang = Attr(n, "lang");
        if (n->tag == "title" && doc->title.empty()) {
            std::string t = Collapse(RawText(n));
            while (!t.empty() && t.back() == ' ') t.pop_back();
            while (!t.empty() && t.front() == ' ') t.erase(t.begin());
            doc->title = t;
        }
        if (n->tag == "body") return;
        for (const auto &c : n->children) Head(c.get());
    }
};

}  // namespace

Document FromHtmlDom(const DomNode *root) {
    Document doc;
    doc.format = "html";
    doc.root.role = Role::Document;
    if (!root) return doc;
    Builder b{&doc};
    b.Head(root);
    b.Walk(root, &doc.root.children, false);
    return doc;
}

Document FromHtml(const std::string &html) {
    HtmlDoc parsed;
    ParseHtml(html, parsed, /*full_document=*/true, /*compute_styles=*/false);
    return FromHtmlDom(parsed.root.get());
}

}  // namespace a11y
