#include "mepml_a11y.h"

#include "a11y_html.h"
#include "math_speech.h"

#include <utility>

namespace mepml {

namespace {

using a11y::Node;
using a11y::Role;

struct Builder {
    const Document &doc;
    const Block *block = nullptr;  // the block whose inlines are being walked

    int LineOf(int offset) const {
        if (!block || !block->origin.empty()) return -1;
        return block->OffsetToPos(offset).line;
    }
    static void AddText(std::vector<Node> *out, const std::string &text, int line) {
        if (text.empty()) return;
        if (!out->empty() && out->back().role == Role::Text) {
            out->back().text += text;
            return;
        }
        Node t;
        t.role = Role::Text;
        t.text = text;
        t.line = line;
        out->push_back(std::move(t));
    }
    // (Line breaks inside a block's text are spaces.)
    static std::string Flat(std::string s) {
        for (char &c : s)
            if (c == '\n') c = ' ';
        return s;
    }
    // A formula with no \alttext is read as its TeX said aloud, as the
    // tagged PDF made from the document reads it.
    static std::string Spoken(const std::string &tex) {
        const std::string said = mathspeech::Speak(tex);
        return said.empty() ? Flat(tex) : said;
    }
    void Inlines(const std::vector<Inline> &ins, std::vector<Node> *out) {
        for (const Inline &x : ins) {
            const int line = LineOf(x.start);
            switch (x.kind) {
                case InlineKind::Text:
                case InlineKind::Verbatim: AddText(out, Flat(x.text), line); break;
                case InlineKind::Comment:
                case InlineKind::Raw: break;
                case InlineKind::Math: {
                    Node f;
                    f.role = Role::Formula;
                    f.tag = "math";
                    f.alt = x.alt;
                    f.has_alt = !x.alt.empty();
                    f.line = line;
                    AddText(&f.children, Spoken(x.text), line);
                    out->push_back(std::move(f));
                    break;
                }
                case InlineKind::Link: {
                    Node l;
                    l.role = Role::Link;
                    l.tag = "link";
                    l.line = line;
                    Inlines(x.children, &l.children);
                    if (l.children.empty()) AddText(&l.children, x.arg, line);
                    out->push_back(std::move(l));
                    break;
                }
                case InlineKind::Cite:
                case InlineKind::CiteP: AddText(out, CiteLabel(doc, x.text, x.kind == InlineKind::CiteP), line); break;
                case InlineKind::Footnote: {
                    // (Read where it is called, as part of the line.)
                    Node n;
                    n.role = Role::Group;
                    n.tag = "footnote";
                    n.line = line;
                    AddText(&n.children, " (footnote: ", line);
                    Inlines(x.children, &n.children);
                    AddText(&n.children, ")", line);
                    out->push_back(std::move(n));
                    break;
                }
                default: Inlines(x.children, out); break;
            }
        }
    }
    Node Leaf(Role role, const char *tag, const Block &b) const {
        Node n;
        n.role = role;
        n.tag = tag;
        n.line = b.origin.empty() ? b.line_start : -1;
        return n;
    }
    void Caption(const Block &b, const std::string &label, Node *to) {
        if (b.caption_inlines.empty()) return;
        Node c = Leaf(Role::Caption, "caption", b);
        if (b.origin.empty() && b.caption_line >= 0) c.line = b.caption_line;
        AddText(&c.children, label.empty() ? "" : label + ": ", c.line);
        Inlines(b.caption_inlines, &c.children);
        to->children.push_back(std::move(c));
    }
    // A figure of block `b`: read as the block's \alttext.
    Node Figure(const Block &b, int line) {
        Node f = Leaf(Role::Figure, "image", b);
        if (line >= 0 && b.origin.empty()) f.line = line;
        f.alt = b.alt;
        f.has_alt = b.alt_line >= 0;
        f.decorative = b.alt_line >= 0 && b.alt.empty();
        return f;
    }

    void List(const Block &b, std::vector<Node> *out) {
        // Items nest by indent: a deeper item's list sits in the item before it.
        Node root = Leaf(Role::List, "list", b);
        std::vector<std::pair<int, Node *>> open;  // indent, the list at that depth
        open.push_back({b.items.empty() ? 0 : b.items.front().indent, &root});
        for (const ListItem &it : b.items) {
            while (open.size() > 1 && it.indent < open.back().first) open.pop_back();
            if (it.indent > open.back().first && !open.back().second->children.empty()) {
                Node sub = Leaf(Role::List, "list", b);
                sub.line = b.origin.empty() ? it.line : -1;
                Node &parent_item = open.back().second->children.back();
                parent_item.children.push_back(std::move(sub));
                open.push_back({it.indent, &parent_item.children.back()});
            }
            Node item = Leaf(Role::ListItem, "list-item", b);
            item.line = b.origin.empty() ? it.line : -1;
            Node label;
            label.role = Role::Label;
            label.line = item.line;
            std::string mark = it.ordered ? std::to_string(it.number) + "." : "\xE2\x80\xA2";
            if (it.checkbox >= 0) mark += it.checkbox ? " (done)" : " (to do)";
            AddText(&label.children, mark, item.line);
            item.children.push_back(std::move(label));
            Inlines(it.content, &item.children);
            open.back().second->children.push_back(std::move(item));
            // (Pointers into a list's children stay good: only the last
            // child of the innermost open list is ever added to.)
        }
        out->push_back(std::move(root));
    }

    void Table(const Block &b, const std::string &label, std::vector<Node> *out) {
        Node t = Leaf(Role::Table, "table", b);
        t.summary = b.alt;
        Caption(b, label, &t);
        int line = b.line_start;
        for (size_t r = 0; r < b.rows.size(); ++r) {
            Node row = Leaf(Role::Row, "table-row", b);
            const bool head = static_cast<int>(r) < b.header_rows;
            for (const TableCell &cell : b.rows[r]) {
                Node c = Leaf(head ? Role::HeaderCell : Role::Cell, "table-cell", b);
                if (b.origin.empty()) c.line = LineOf(cell.start);
                if (head) c.scope = "column";
                if (!cell.image.empty()) {
                    // A picture in a cell has no \alttext of its own: the
                    // table's says what the table shows, pictures and all.
                    Node f = Leaf(Role::Figure, "image", b);
                    f.line = c.line;
                    f.has_alt = f.decorative = !b.alt.empty();
                    c.children.push_back(std::move(f));
                } else {
                    Inlines(cell.content, &c.children);
                }
                if (c.line >= 0) line = c.line;
                row.children.push_back(std::move(c));
            }
            if (b.origin.empty()) row.line = line;
            t.children.push_back(std::move(row));
        }
        out->push_back(std::move(t));
    }

    void Build(Node *root) {
        const std::vector<std::string> labels = BlockLabels(doc);
        const std::vector<bool> hidden = ExportHidden(doc);
        // Slides and boxes hold the blocks between their markers.
        std::vector<std::vector<Node>> stack(1);  // children being collected, one level per open section
        std::vector<Node> heads;                  // the section node of each level above the first
        auto close = [&] {
            if (heads.empty()) return;
            Node s = std::move(heads.back());
            heads.pop_back();
            s.children = std::move(stack.back());
            stack.pop_back();
            stack.back().push_back(std::move(s));
        };
        for (size_t bi = 0; bi < doc.blocks.size(); ++bi) {
            const Block &b = doc.blocks[bi];
            if (hidden[bi]) continue;
            block = &b;
            std::vector<Node> *out = &stack.back();
            switch (b.kind) {
                case BlockKind::Heading: {
                    Node h = Leaf(Role::Heading, "heading", b);
                    h.level = b.level;
                    Inlines(b.inlines, &h.children);
                    out->push_back(std::move(h));
                    break;
                }
                case BlockKind::Paragraph:
                case BlockKind::Command: {
                    Node p = Leaf(Role::Paragraph, "paragraph", b);
                    Inlines(b.inlines, &p.children);
                    if (!p.children.empty()) out->push_back(std::move(p));
                    break;
                }
                case BlockKind::MathBlock: {
                    Node f = Leaf(Role::Formula, "math-block", b);
                    f.alt = b.alt;
                    f.has_alt = !b.alt.empty();
                    AddText(&f.children, Spoken(b.code), f.line);
                    Caption(b, labels[bi], &f);
                    out->push_back(std::move(f));
                    break;
                }
                case BlockKind::Image: {
                    Node f = Figure(b, -1);
                    Caption(b, labels[bi], &f);
                    out->push_back(std::move(f));
                    break;
                }
                case BlockKind::Svg: {
                    // A picture: read as its \alttext, like an image.
                    Node f = Figure(b, -1);
                    f.tag = "svg";
                    Caption(b, labels[bi], &f);
                    out->push_back(std::move(f));
                    break;
                }
                case BlockKind::Html: {
                    // What the markup says, as a reader meets it in a page;
                    // its \alttext is a description, read before it.
                    Node h = Leaf(Role::Group, "html", b);
                    if (!b.alt.empty()) h.summary = b.alt;
                    for (Node &c : a11y::FromHtml(b.code).root.children) h.children.push_back(std::move(c));
                    Caption(b, labels[bi], &h);
                    if (!h.children.empty()) out->push_back(std::move(h));
                    break;
                }
                case BlockKind::Code: {
                    bool show_code = true, show_results = true;
                    CodeExports(doc, b, &show_code, &show_results);
                    if (show_code) {
                        Node c = Leaf(Role::Code, "code", b);
                        AddText(&c.children, b.code, c.line);
                        out->push_back(std::move(c));
                    }
                    if (!show_results) break;
                    if (b.result_format.empty() || b.result_format == "terminal") {
                        std::string text;
                        for (const std::string &l : b.result_lines) {
                            std::string img;
                            if (!ResultImagePath(l, &img)) text += l + "\n";
                        }
                        if (!text.empty()) {
                            Node r = Leaf(Role::Code, "results", b);
                            if (b.origin.empty() && b.result_line_start >= 0) r.line = b.result_line_start;
                            AddText(&r.children, text, r.line);
                            out->push_back(std::move(r));
                        }
                    }
                    for (size_t k = 0; k < b.result_images.size(); ++k) {
                        Node f = Figure(b, b.result_images[k].first);
                        if (k + 1 == b.result_images.size()) Caption(b, labels[bi], &f);
                        out->push_back(std::move(f));
                    }
                    break;
                }
                case BlockKind::Table: Table(b, labels[bi], out); break;
                case BlockKind::List: List(b, out); break;
                case BlockKind::Abstract: {
                    Node s = Leaf(Role::Section, "abstract", b);
                    Node h = Leaf(Role::Heading, "abstract-title", b);
                    h.level = 2;
                    AddText(&h.children, "Abstract", h.line);
                    s.children.push_back(std::move(h));
                    for (const std::vector<Inline> &para : AbstractParagraphs(b)) {
                        Node p = Leaf(Role::Paragraph, "paragraph", b);
                        Inlines(para, &p.children);
                        s.children.push_back(std::move(p));
                    }
                    out->push_back(std::move(s));
                    break;
                }
                case BlockKind::SlideBegin: {
                    // (An open slide ends where the next begins.)
                    while (!heads.empty()) close();
                    heads.push_back(Leaf(Role::Section, "slide", b));
                    stack.emplace_back();
                    break;
                }
                case BlockKind::SlideEnd:
                    while (!heads.empty()) close();
                    break;
                case BlockKind::BoxBegin: {
                    Node s = Leaf(Role::Section, "box", b);
                    Node title = Leaf(Role::Paragraph, "box-title", b);
                    AddText(&title.children, BoxLabel(b.keyword) + (b.caption_inlines.empty() ? "" : ": "), title.line);
                    Inlines(b.caption_inlines, &title.children);
                    std::vector<Node> body;
                    body.push_back(std::move(title));
                    if (!b.inlines.empty()) {
                        Node p = Leaf(Role::Paragraph, "paragraph", b);
                        Inlines(b.inlines, &p.children);
                        body.push_back(std::move(p));
                    }
                    if (b.box_closed) {
                        s.children = std::move(body);
                        out->push_back(std::move(s));
                    } else {
                        heads.push_back(std::move(s));
                        stack.push_back(std::move(body));
                    }
                    break;
                }
                case BlockKind::BoxEnd:
                    if (!heads.empty() && heads.back().tag == "box") close();
                    break;
                // (Columns are read in order, left to right: no element of
                // their own.)
                case BlockKind::LayoutBegin:
                case BlockKind::LayoutEnd: break;
                case BlockKind::Bibliography: {
                    if (doc.cite_order.empty()) break;
                    Node s = Leaf(Role::Section, "bibliography", b);
                    Node h = Leaf(Role::Heading, "bibliography-title", b);
                    h.level = 2;
                    AddText(&h.children, "References", h.line);
                    s.children.push_back(std::move(h));
                    Node l = Leaf(Role::List, "list", b);
                    int n = 0;
                    for (const std::string &key : doc.cite_order) {
                        const auto it = doc.citations.find(key);
                        if (it == doc.citations.end()) continue;
                        const BibEntryParts e = BibEntry(it->second);
                        Node item = Leaf(Role::ListItem, "list-item", b);
                        Node label;
                        label.role = Role::Label;
                        AddText(&label.children, "[" + std::to_string(++n) + "]", item.line);
                        item.children.push_back(std::move(label));
                        AddText(&item.children, e.lead + e.title + e.rest, item.line);
                        l.children.push_back(std::move(item));
                    }
                    s.children.push_back(std::move(l));
                    out->push_back(std::move(s));
                    break;
                }
                case BlockKind::Callout:  // (a comment: in no export, so not read)
                case BlockKind::Rule:
                case BlockKind::TableOfContents:
                case BlockKind::Comment:
                case BlockKind::Meta:
                case BlockKind::Import:
                case BlockKind::Citation:
                case BlockKind::Define:
                case BlockKind::Raw: break;
            }
        }
        while (!heads.empty()) close();
        root->children = std::move(stack.back());
    }
};

}  // namespace

a11y::Document Accessibility(const Document &doc) {
    a11y::Document out;
    out.format = "mepml";
    out.title = doc.title;
    out.lang = DocumentLanguage(doc);
    out.root.role = Role::Document;
    Builder b{doc};
    b.Build(&out.root);
    return out;
}

}  // namespace mepml
