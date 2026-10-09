#include "a11y_pdf.h"

#include "pdf_content.h"
#include "pdf_struct.h"
#include "pdf_text.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

namespace a11y {

namespace {

struct Box {
    bool any = false;
    double l = 0, b = 0, r = 0, t = 0;
    void Add(double left, double bottom, double right, double top) {
        if (!any) {
            l = left, b = bottom, r = right, t = top;
            any = true;
            return;
        }
        l = std::min(l, left), b = std::min(b, bottom);
        r = std::max(r, right), t = std::max(t, top);
    }
};

// The glyphs of one stretch of a page as text: a space where the next
// glyph starts a new word or a new line (pdf_text.h's own geometry
// rules), and a word hyphenated over a line end put back together.
struct TextBuilder {
    std::string text;
    bool have_prev = false;
    pdfrender::TextGlyph prev;
    std::string last_actual;

    // Ligature expansion and the broken-word test live in pdf_text.h:
    // a reader copying a passage out of the page (JoinGlyphText) wants
    // exactly the same two things a screen reader does.
    void Add(const pdfrender::TextGlyph &g) {
        // A sequence with /ActualText reads as that text, once.
        const std::string piece = pdftext::ExpandLigatures(g.actual_text.empty() ? g.utf8_text : g.actual_text);
        const bool repeat = !g.actual_text.empty() && have_prev && g.actual_text == last_actual;
        last_actual = g.actual_text;
        if (have_prev) {
            const double height = std::max(g.top - g.bottom, 1e-6), prev_height = std::max(prev.top - prev.bottom, 1e-6);
            const double centre = (g.top + g.bottom) / 2, prev_centre = (prev.top + prev.bottom) / 2;
            const bool new_line = std::fabs(centre - prev_centre) > 0.5 * std::max(height, prev_height);
            const bool gap = !new_line && g.left - prev.right > 0.25 * std::max(height, prev_height);
            if (new_line && pdftext::EndsWithLetterHyphen(text) && !piece.empty() &&
                std::islower(static_cast<unsigned char>(piece[0]))) {
                text.pop_back();
            } else if ((new_line || gap) && !text.empty() && text.back() != ' ') {
                text += ' ';
            }
        }
        if (!repeat) text += piece;
        prev = g;
        have_prev = true;
    }
};

struct PageContent {
    std::map<int, std::string> text;  // by MCID
    std::map<int, Box> box;
};

struct Builder {
    const unsigned char *data;
    size_t len;
    const pdfdoc::PdfDocument &document;
    const pdfstruct::Tree &tree;
    std::map<int, PageContent> pages;

    std::vector<pdfrender::TextGlyph> Glyphs(int page_index, std::vector<pdfrender::MarkedBox> *marked) {
        std::vector<pdfrender::TextGlyph> glyphs;
        const pdfdoc::Page *page = document.GetPage(page_index);
        if (!page) return glyphs;
        const std::string content = pdfrender::GetPageContent(data, len, document.Xref(), *page);
        if (content.empty()) return glyphs;
        pdfrender::Canvas canvas = pdfrender::Canvas::MakeWhite(1, 1);
        pdfrender::ExtractContentStreamText(content, canvas, pdfrender::Mat2D{}, page->resources, data, len, document.Xref(),
                                            &glyphs, marked);
        return glyphs;
    }
    const PageContent &Page(int page_index) {
        auto it = pages.find(page_index);
        if (it != pages.end()) return it->second;
        PageContent pc;
        std::vector<pdfrender::MarkedBox> marked;
        std::map<int, TextBuilder> builders;
        for (const pdfrender::TextGlyph &g : Glyphs(page_index, &marked)) {
            if (g.mcid < 0 || g.artifact) continue;
            builders[g.mcid].Add(g);
            pc.box[g.mcid].Add(g.left, g.bottom, g.right, g.top);
        }
        for (auto &kv : builders) pc.text[kv.first] = std::move(kv.second.text);
        for (const pdfrender::MarkedBox &m : marked) pc.box[m.mcid].Add(m.left, m.bottom, m.right, m.top);
        return pages.emplace(page_index, std::move(pc)).first->second;
    }

    static Role RoleOf(const pdfstruct::Element &e, int *level) {
        const std::string &r = e.role;
        if (e.type == "Title" || r == "Title") return Role::Title;
        if (r == "Document") return Role::Document;
        if (r == "Part" || r == "Art" || r == "Sect" || r == "Index") return Role::Section;
        if (r == "H") return *level = 1, Role::Heading;
        if (r.size() == 2 && r[0] == 'H' && r[1] >= '1' && r[1] <= '6') return *level = r[1] - '0', Role::Heading;
        if (r == "P") return Role::Paragraph;
        if (r == "L" || r == "TOC") return Role::List;
        if (r == "LI" || r == "TOCI") return Role::ListItem;
        if (r == "Lbl") return Role::Label;
        if (r == "Table") return Role::Table;
        if (r == "TR") return Role::Row;
        if (r == "TH") return Role::HeaderCell;
        if (r == "TD") return Role::Cell;
        if (r == "Figure") return Role::Figure;
        if (r == "Formula") return Role::Formula;
        if (r == "Caption") return Role::Caption;
        if (r == "Code") return Role::Code;
        if (r == "BlockQuote" || r == "Quote") return Role::Quote;
        if (r == "Note") return Role::Note;
        if (r == "Link") return Role::Link;
        return Role::Group;
    }

    void Grow(Node *n, int page, const Box &b) {
        if (!b.any) return;
        if (n->page < 0) n->page = page;
        if (n->page != page) return;  // (a node's box is on its first page)
        Box u;
        if (n->has_box) u.Add(n->box[0], n->box[1], n->box[2], n->box[3]);
        u.Add(b.l, b.b, b.r, b.t);
        n->has_box = true;
        n->box[0] = u.l, n->box[1] = u.b, n->box[2] = u.r, n->box[3] = u.t;
    }

    Node Element(int index, int depth) {
        const pdfstruct::Element &e = tree.elements[static_cast<size_t>(index)];
        Node n;
        n.role = RoleOf(e, &n.level);
        n.tag = e.type;
        n.alt = e.alt;
        n.has_alt = e.has_alt;
        n.summary = e.summary;
        n.lang = e.lang;
        if (n.role == Role::HeaderCell) n.scope = e.scope == "Row" ? "row" : e.scope == "Both" ? "both" : e.scope.empty() ? "" : "column";
        for (const pdfstruct::Kid &k : e.kids) {
            if (k.kind == pdfstruct::Kid::Kind::Element) {
                if (depth > 200) continue;
                Node child = Element(k.element, depth + 1);
                if (child.has_box) Grow(&n, child.page, Box{true, child.box[0], child.box[1], child.box[2], child.box[3]});
                else if (n.page < 0) n.page = child.page;
                n.children.push_back(std::move(child));
            } else if (k.kind == pdfstruct::Kid::Kind::Content && k.page >= 0 && k.mcid >= 0) {
                const PageContent &pc = Page(k.page);
                const auto box = pc.box.find(k.mcid);
                if (box != pc.box.end()) Grow(&n, k.page, box->second);
                else if (n.page < 0) n.page = k.page;
                const auto text = pc.text.find(k.mcid);
                if (text == pc.text.end() || text->second.empty()) continue;
                Node t;
                t.role = Role::Text;
                t.text = text->second;
                t.page = k.page;
                n.children.push_back(std::move(t));
            }
        }
        // What the element says its content reads as replaces the content.
        if (!e.actual_text.empty()) {
            Node t;
            t.role = Role::Text;
            t.text = e.actual_text;
            t.page = n.page;
            n.children.clear();
            n.children.push_back(std::move(t));
        }
        return n;
    }

    // No structure: the page's lines, a paragraph per block of them (a
    // gap wider than a line and a half starts a new one).
    void Untagged(Node *root) {
        for (int p = 0; p < document.PageCount(); ++p) {
            TextBuilder para;
            Box box;
            auto flush = [&] {
                if (para.text.empty()) return;
                Node n;
                n.role = Role::Paragraph;
                n.page = p;
                n.has_box = box.any;
                n.box[0] = box.l, n.box[1] = box.b, n.box[2] = box.r, n.box[3] = box.t;
                Node t;
                t.role = Role::Text;
                t.text = std::move(para.text);
                t.page = p;
                n.children.push_back(std::move(t));
                root->children.push_back(std::move(n));
                para = TextBuilder();
                box = Box();
            };
            for (const pdfrender::TextGlyph &g : Glyphs(p, nullptr)) {
                if (g.artifact) continue;
                if (para.have_prev) {
                    const double height = std::max(g.top - g.bottom, 1e-6);
                    const double drop = (para.prev.top + para.prev.bottom) / 2 - (g.top + g.bottom) / 2;
                    if (drop > 1.9 * height || drop < -1.9 * height) flush();
                }
                para.Add(g);
                box.Add(g.left, g.bottom, g.right, g.top);
            }
            flush();
        }
    }
};

}  // namespace

Document FromPdf(const unsigned char *data, size_t len, const pdfdoc::PdfDocument &document) {
    const pdfstruct::Tree tree = pdfstruct::Read(data, len, document.Xref(), document);
    Document doc;
    doc.format = "pdf";
    doc.title = tree.title;
    doc.lang = tree.lang;
    doc.spaced = false;
    doc.root.role = Role::Document;
    Builder b{data, len, document, tree, {}};
    doc.tagged = tree.present && !tree.roots.empty();
    if (!doc.tagged) {
        b.Untagged(&doc.root);
        return doc;
    }
    for (int root : tree.roots) {
        Node n = b.Element(root, 0);
        // (The tree's one Document element is the document itself.)
        if (n.role == Role::Document) {
            if (doc.lang.empty()) doc.lang = n.lang;
            for (Node &c : n.children) doc.root.children.push_back(std::move(c));
        } else {
            doc.root.children.push_back(std::move(n));
        }
    }
    return doc;
}

}  // namespace a11y
