// OpenDocument presentation (.odp) reading and writing for pres_doc.h.
//
// Reading resolves ODF's style inheritance: a shape's graphic (or
// presentation) style, its parent chain up to the default graphic style,
// and the paragraph and text styles of its spans. A presentation object
// (presentation:class="title"/"outline"/...) takes what its master page's
// object of the same class defines when it has no geometry of its own. A
// formula object shows as its replacement picture when the file has one.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>

#include "math_markup.h"
#include "office_math.h"
#include "pres_doc.h"
#include "pres_internal.h"
#include "zip_archive.h"

namespace pres {

using namespace detail;

namespace {

// --- Styles ---------------------------------------------------------------------------

// A style's properties, flattened (every *-properties element's attributes
// by local name), and its parent.
struct Style {
    std::string parent;
    std::string family;
    std::map<std::string, std::string> props;
    std::string list_style;  // style:list-style-name (a presentation style's list)
};

// A list style: per level, 0 no label, 1 bullet, 2 number.
using ListStyle = std::map<int, int>;

struct Styles {
    std::map<std::string, Style> named;       // by style:name
    std::map<std::string, Style> defaults;    // by family (style:default-style)
    std::map<std::string, ListStyle> lists;
    std::map<std::string, std::string> fill_images;  // draw:fill-image name -> its picture

    void Add(const xml::xml_node &container) {
        for (const xml::xml_node &c : Elements(container)) {
            if (Is(c, "style") || Is(c, "default-style")) {
                Style s;
                s.parent = Attr(c, "parent-style-name");
                s.family = Attr(c, "family");
                s.list_style = Attr(c, "list-style-name");
                for (const xml::xml_node &p : Elements(c)) {
                    const char *ln = Local(p.name());
                    if (std::strstr(ln, "-properties") == nullptr) continue;
                    CollectAttrs(p, &s.props);
                    // A graphic style's list styles for its text (presentation outline).
                    if (Is(p, "graphic-properties"))
                        if (xml::xml_node ls = Child(p, "list-style")) AddList("#graphic:" + Attr(c, "name"), ls);
                }
                if (Is(c, "default-style")) defaults[s.family] = s;
                else named[Attr(c, "name")] = s;
            } else if (Is(c, "list-style")) {
                AddList(Attr(c, "name"), c);
            } else if (Is(c, "fill-image")) {
                fill_images[Attr(c, "name")] = Attr(c, "href");
            }
        }
    }

    static void CollectAttrs(const xml::xml_node &n, std::map<std::string, std::string> *out);

    void AddList(const std::string &name, const xml::xml_node &ls) {
        ListStyle l;
        for (const xml::xml_node &lvl : Elements(ls)) {
            const int level = std::max(1, std::atoi(Attr(lvl, "level").c_str()));
            if (Is(lvl, "list-level-style-bullet") || Is(lvl, "list-level-style-image")) l[level] = 1;
            else if (Is(lvl, "list-level-style-number")) l[level] = Attr(lvl, "num-format").empty() ? 0 : 2;
        }
        lists[name] = l;
    }

    // `name`'s properties with its ancestors' underneath, then the
    // family's default under all.
    std::map<std::string, std::string> Resolve(const std::string &name, const std::string &family) const {
        std::vector<const Style *> chain;
        std::string cur = name;
        for (int guard = 0; !cur.empty() && guard < 32; ++guard) {
            auto it = named.find(cur);
            if (it == named.end()) break;
            chain.push_back(&it->second);
            cur = it->second.parent;
        }
        std::map<std::string, std::string> out;
        auto d = defaults.find(family.empty() && !chain.empty() ? chain.back()->family : family);
        if (d != defaults.end()) out = d->second.props;
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            for (const auto &kv : (*it)->props) out[kv.first] = kv.second;
        return out;
    }

    std::string ListOf(const std::string &style_name) const {
        std::string cur = style_name;
        for (int guard = 0; !cur.empty() && guard < 32; ++guard) {
            auto it = named.find(cur);
            if (it == named.end()) break;
            if (!it->second.list_style.empty()) return it->second.list_style;
            if (lists.count("#graphic:" + cur)) return "#graphic:" + cur;
            cur = it->second.parent;
        }
        return "";
    }
};

void Styles::CollectAttrs(const xml::xml_node &n, std::map<std::string, std::string> *out) {
    // xml_node offers attributes by name only, so the ones this reader
    // uses are asked for by name.
    static const char *const kNames[] = {
        "fill", "fill-color", "stroke", "stroke-color", "stroke-width", "textarea-vertical-align", "shrink-to-fit",
        "text-align", "font-size", "font-weight", "font-style", "color", "text-underline-style", "text-line-through-style",
        "font-name", "font-family", "background-color", "page-width", "page-height", "auto-grow-height",
        "textarea-horizontal-align", "fill-image-name", "text-position",
    };
    for (const char *name : kNames) {
        const std::string v = Attr(n, name);
        if (!v.empty()) (*out)[name] = v;
    }
}

double FontSizePt(const std::string &v, double base) {
    if (v.empty()) return 0;
    if (v.back() == '%') return base * std::atof(v.c_str()) / 100.0;
    return static_cast<double>(OdfLengthToEmu(v)) / kEmuPerPt;
}

std::string Prop(const std::map<std::string, std::string> &m, const char *k) {
    auto it = m.find(k);
    return it == m.end() ? std::string() : it->second;
}

// --- Reading ------------------------------------------------------------------------

class Reader {
public:
    Reader(const std::string &bytes) : bytes_(bytes) {}

    bool Read(Presentation *out, std::string *error) {
        std::string content, styles_text;
        if (!Get("content.xml", &content) || !ParseXml(content, &content_doc_, &content_root_)) {
            if (error) *error = "not an OpenDocument presentation (no content.xml)";
            return false;
        }
        if (Get("styles.xml", &styles_text) && ParseXml(styles_text, &styles_doc_, &styles_root_)) {
            styles_.Add(Child(styles_root_, "styles"));
            styles_.Add(Child(styles_root_, "automatic-styles"));
        }
        styles_.Add(Child(content_root_, "automatic-styles"));
        // Page size: the first master page's layout.
        xml::xml_node masters = Child(styles_root_, "master-styles");
        for (const xml::xml_node &mp : Children(masters, "master-page")) {
            master_pages_[Attr(mp, "name")] = mp;
            if (out->width == 12192000 || first_master_.empty()) {
                if (first_master_.empty()) first_master_ = Attr(mp, "name");
            }
        }
        for (const xml::xml_node &pl : Children(Child(styles_root_, "automatic-styles"), "page-layout")) {
            xml::xml_node p = Child(pl, "page-layout-properties");
            const long w = OdfLengthToEmu(Attr(p, "page-width")), h = OdfLengthToEmu(Attr(p, "page-height"));
            if (w > 0 && h > 0) layouts_[Attr(pl, "name")] = {w, h};
        }
        if (!first_master_.empty()) {
            auto l = layouts_.find(Attr(master_pages_[first_master_], "page-layout-name"));
            if (l != layouts_.end()) {
                out->width = l->second.first;
                out->height = l->second.second;
            }
        }
        out->slides.clear();
        xml::xml_node pres = Path(content_root_, "body/presentation");
        for (const xml::xml_node &page : Children(pres, "page")) {
            Slide s;
            ReadPage(page, &s);
            out->slides.push_back(std::move(s));
        }
        std::string meta;
        xml::xml_document meta_doc;
        xml::xml_node meta_root;
        if (Get("meta.xml", &meta) && ParseXml(meta, &meta_doc, &meta_root)) {
            xml::xml_node m = Child(meta_root, "meta");
            out->title = AllText(Child(m, "title"));
            out->author = AllText(Child(m, "initial-creator"));
            if (out->author.empty()) out->author = AllText(Child(m, "creator"));
        }
        if (out->slides.empty()) out->slides.push_back(Slide{});
        return true;
    }

private:
    const std::string &bytes_;
    xml::xml_document content_doc_, styles_doc_;
    xml::xml_node content_root_, styles_root_;
    Styles styles_;
    std::map<std::string, xml::xml_node> master_pages_;
    std::map<std::string, std::pair<long, long>> layouts_;
    std::string first_master_;
    Align default_align_ = Align::Left;

    bool Get(const std::string &name, std::string *out) const {
        return zip::Extract(reinterpret_cast<const unsigned char *>(bytes_.data()), bytes_.size(), name.c_str(), *out);
    }

    std::string PageBackground(const std::string &style) const {
        const auto p = styles_.Resolve(style, "drawing-page");
        if (Prop(p, "fill") == "solid") return NormalizeColor(Prop(p, "fill-color"));
        return "";
    }

    void ReadPage(const xml::xml_node &page, Slide *s) {
        const std::string master = Attr(page, "master-page-name");
        s->background = PageBackground(Attr(page, "style-name"));
        if (s->background.empty()) {
            auto m = master_pages_.find(master);
            if (m != master_pages_.end()) s->background = PageBackground(Attr(m->second, "style-name"));
        }
        if (s->background == "FFFFFF") s->background.clear();
        ReadShapes(page, master, s);
        // Speaker notes: the notes page's text.
        if (xml::xml_node notes = Child(page, "notes")) {
            for (const xml::xml_node &f : Children(notes, "frame")) {
                if (Attr(f, "class") != "notes") continue;
                for (const xml::xml_node &p : Elements(Child(f, "text-box"))) {
                    if (!s->notes.empty()) s->notes += "\n";
                    s->notes += AllText(p);
                }
            }
        }
    }

    // A master page's presentation object of `cls`, for its geometry.
    xml::xml_node MasterObject(const std::string &master, const std::string &cls) {
        auto m = master_pages_.find(master);
        if (m == master_pages_.end()) return {};
        for (const xml::xml_node &f : Children(m->second, "frame"))
            if (Attr(f, "class") == cls) return f;
        return {};
    }

    void ReadShapes(const xml::xml_node &container, const std::string &master, Slide *s) {
        for (const xml::xml_node &c : Elements(container)) {
            if (Is(c, "frame")) ReadFrame(c, master, s);
            else if (Is(c, "rect") || Is(c, "ellipse") || Is(c, "custom-shape") || Is(c, "circle")) ReadDrawShape(c, s);
            else if (Is(c, "line") || Is(c, "connector")) ReadLine(c, s);
            else if (Is(c, "g")) ReadShapes(c, master, s);
        }
    }

    bool Geometry(const xml::xml_node &n, Shape *s) const {
        if (!HasAttr(n, "width") || !HasAttr(n, "height")) return false;
        s->x = OdfLengthToEmu(Attr(n, "x"));
        s->y = OdfLengthToEmu(Attr(n, "y"));
        s->w = OdfLengthToEmu(Attr(n, "width"));
        s->h = OdfLengthToEmu(Attr(n, "height"));
        return true;
    }

    // The graphic style of a shape (draw:style-name, else the presentation
    // style), flattened.
    std::map<std::string, std::string> ShapeStyle(const xml::xml_node &n) const {
        std::map<std::string, std::string> out = styles_.Resolve(Attr(n, "style-name"), "graphic");
        // A presentation style underneath the shape's own.
        const std::string pres = Attr(n, "presentation:style-name");
        if (!pres.empty()) {
            std::map<std::string, std::string> p = styles_.Resolve(pres, "presentation");
            for (const auto &kv : out) p[kv.first] = kv.second;
            out = p;
        }
        return out;
    }

    void ApplyGraphic(const std::map<std::string, std::string> &g, Shape *s) const {
        if (Prop(g, "fill") == "solid") s->fill = NormalizeColor(Prop(g, "fill-color"));
        if (Prop(g, "stroke") == "solid" || Prop(g, "stroke") == "dash") {
            s->line = NormalizeColor(Prop(g, "stroke-color"));
            if (s->line.empty()) s->line = "3465A4";
            const std::string w = Prop(g, "stroke-width");
            s->line_pt = w.empty() ? 0.75 : std::max(0.5, static_cast<double>(OdfLengthToEmu(w)) / kEmuPerPt);
        }
        const std::string va = Prop(g, "textarea-vertical-align");
        s->valign = va == "middle" ? VAlign::Middle : va == "bottom" ? VAlign::Bottom : VAlign::Top;
        s->autofit = Prop(g, "shrink-to-fit") == "true";
        const double size = FontSizePt(Prop(g, "font-size"), 18);
        if (size > 0) s->font_pt = size;
        const std::string color = NormalizeColor(Prop(g, "color"));
        if (!color.empty() && color != "000000") s->text_color = color;
    }

    void ReadFrame(const xml::xml_node &f, const std::string &master, Slide *s) {
        Shape sh;
        sh.name = Attr(f, "name");
        const std::string cls = Attr(f, "class");
        if (cls == "page-number" || cls == "footer" || cls == "date-time" || cls == "header" || cls == "notes") return;
        if (!Geometry(f, &sh)) {
            xml::xml_node mo = MasterObject(master, cls);
            if (!mo || !Geometry(mo, &sh)) return;
        }
        sh.is_title = cls == "title";
        const auto g = ShapeStyle(f);
        ApplyGraphic(g, &sh);
        const std::string list = styles_.ListOf(Attr(f, "presentation:style-name"));
        if (xml::xml_node box = Child(f, "text-box")) {
            sh.kind = ShapeKind::Text;
            ReadText(box, sh.font_pt, cls == "outline" ? list : "", cls == "outline", &sh);
            // An empty placeholder is only a prompt.
            if (!cls.empty() && Attr(f, "placeholder") == "true") return;
            if (sh.paras.empty()) sh.paras.push_back(Paragraph{});
            s->shapes.push_back(std::move(sh));
            return;
        }
        if (xml::xml_node table = Child(f, "table")) {
            sh.kind = ShapeKind::Text;
            sh.font_pt = 14;
            sh.line = "BFC5CC";
            sh.line_pt = 0.75;
            for (const xml::xml_node &row : Children(table, "table-row")) {
                std::string line;
                for (const xml::xml_node &cell : Children(row, "table-cell")) line += (line.empty() ? "" : "\t") + AllText(cell);
                Paragraph p;
                TextRun r;
                r.text = line;
                p.runs.push_back(r);
                sh.paras.push_back(p);
            }
            s->shapes.push_back(std::move(sh));
            return;
        }
        // A formula object: its maths, typeset by mep, rather than the
        // replacement picture that may come with it.
        if (xml::xml_node obj = Child(f, "object")) {
            if (ReadFormula(obj, &sh)) {
                s->shapes.push_back(std::move(sh));
                return;
            }
        }
        // A picture, or an object shown by its replacement picture.
        xml::xml_node img;
        for (const xml::xml_node &c : Children(f, "image")) {
            img = c;
            break;
        }
        if (img) {
            std::string href = Attr(img, "href");
            while (href.rfind("./", 0) == 0) href.erase(0, 2);
            std::string bytes;
            if (!href.empty() && Get(href, &bytes)) {
                sh.kind = ShapeKind::Image;
                sh.image = std::make_shared<const std::string>(std::move(bytes));
                sh.image_ext = ExtOf(href);
                sh.alt = AllText(Child(f, "desc"));
                if (sh.alt.empty()) sh.alt = AllText(Child(f, "title"));
                sh.fill.clear();
                sh.line.clear();
                s->shapes.push_back(std::move(sh));
                return;
            }
        }
    }

    // A formula object's maths, as a text shape of maths runs: a TeX
    // annotation (mep's own equations) as written; a paragraph formula
    // (mep's: a table whose rows mix <mtext> words with maths) as a
    // paragraph of text and inline maths again; any other formula's
    // MathML as TeX. Its size in points is what fits the frame.
    bool ReadFormula(const xml::xml_node &obj, Shape *sh) {
        std::string href = Attr(obj, "href");
        while (href.rfind("./", 0) == 0) href.erase(0, 2);
        std::string xml_text;
        xml::xml_document doc;
        xml::xml_node root;
        if (!Get(href + "/content.xml", &xml_text) || !ParseXml(xml_text, &doc, &root) || !Is(root, "math")) return false;
        xml::xml_node body = root;
        std::string tex_note;
        if (xml::xml_node sem = Child(root, "semantics")) {
            for (const xml::xml_node &a : Children(sem, "annotation"))
                if (Attr(a, "encoding").find("tex") != std::string::npos) tex_note = AllText(a);
            for (const xml::xml_node &c : Elements(sem))
                if (!Is(c, "annotation") && !Is(c, "annotation-xml")) {
                    body = c;
                    break;
                }
        }
        sh->kind = ShapeKind::Text;
        sh->fill.clear();
        sh->line.clear();
        sh->paras.clear();
        const double h_pt = static_cast<double>(std::labs(sh->h)) / kEmuPerPt;
        // A paragraph formula.
        if (tex_note.empty() && Is(body, "mtable")) {
            Paragraph para;
            int rows = 0;
            bool words = false;
            // Its height in ems, row by row as mepml's writer measured it (a
            // line of text, or its tallest maths), gives its text size.
            double height_em = 0;
            for (const xml::xml_node &tr : Children(body, "mtr")) {
                ++rows;
                double row_em = 1.11;
                xml::xml_node row = Child(Child(tr, "mtd"), "mrow");
                for (const xml::xml_node &c : Elements(row)) {
                    if (!Is(c, "mtext")) {
                        const std::string tex = officemath::MathmlToTex(c);
                        double wem = 0, hem = 0;
                        TexMathExtent(tex, false, &wem, &hem);
                        row_em = std::max(row_em, hem);
                        para.runs.push_back(MathRun(tex, false));
                        continue;
                    }
                    words = true;
                    TextRun r;
                    r.text = AllText(c);
                    for (size_t k; (k = r.text.find("\xC2\xA0")) != std::string::npos;) r.text.replace(k, 2, " ");
                    const std::string v = Attr(c, "mathvariant");
                    r.bold = v.find("bold") != std::string::npos;
                    r.italic = v.find("italic") != std::string::npos;
                    if (v == "monospace") r.font = "Liberation Mono";
                    r.color = NormalizeColor(Attr(c, "mathcolor"));
                    para.runs.push_back(r);
                }
                height_em += row_em + (rows > 1 ? 0.17 : 0.0);
                // (A row was a line of the one paragraph.)
                if (!para.runs.empty() && !para.runs.back().IsMath() && para.runs.back().text.back() != ' ') para.runs.back().text += " ";
            }
            if (words && !para.runs.empty()) {
                while (!para.runs.back().IsMath() && !para.runs.back().text.empty() && para.runs.back().text.back() == ' ') para.runs.back().text.pop_back();
                sh->paras.push_back(para);
                sh->valign = VAlign::Top;
                sh->font_pt = std::max(6.0, h_pt / std::max(1.0, height_em));
                // A formula has no insets; the text box it becomes does.
                sh->x -= 91440;
                sh->w += 2 * 91440;
                sh->y -= 45720;
                // (and room for a text line's own leading, which Math's lines lack)
                sh->h += 2 * 45720 + sh->h / 4;
                sh->autofit = true;
                return true;
            }
        }
        const std::string tex = tex_note.empty() ? officemath::MathmlToTex(body) : tex_note;
        if (tex.empty()) return false;
        double wem = 0, hem = 0;
        TexMathExtent(tex, true, &wem, &hem);
        sh->font_pt = std::max(6.0, hem > 0 ? h_pt / hem : 18.0);
        sh->valign = VAlign::Middle;
        Paragraph para;
        para.align = Align::Center;
        para.runs.push_back(MathRun(tex, true));
        sh->paras.push_back(para);
        sh->name = sh->name.empty() ? "Equation" : sh->name;
        return true;
    }

    void ReadDrawShape(const xml::xml_node &n, Slide *s) {
        Shape sh;
        sh.name = Attr(n, "name");
        if (!Geometry(n, &sh)) return;
        sh.kind = Is(n, "ellipse") || Is(n, "circle") ? ShapeKind::Ellipse : ShapeKind::Rect;
        if (Is(n, "custom-shape")) {
            const std::string type = Attr(Child(n, "enhanced-geometry"), "type");
            if (type == "ellipse") sh.kind = ShapeKind::Ellipse;
            else if (type == "round-rectangle") sh.kind = ShapeKind::RoundRect;
        }
        if (Is(n, "rect") && HasAttr(n, "corner-radius")) sh.kind = ShapeKind::RoundRect;
        const auto g = ShapeStyle(n);
        // A picture as a shape's fill (how LibreOffice keeps a PowerPoint
        // equation's fallback picture): the picture.
        if (Prop(g, "fill") == "bitmap") {
            auto fi = styles_.fill_images.find(Prop(g, "fill-image-name"));
            std::string href = fi == styles_.fill_images.end() ? std::string() : fi->second, bytes;
            while (href.rfind("./", 0) == 0) href.erase(0, 2);
            if (!href.empty() && Get(href, &bytes)) {
                sh.kind = ShapeKind::Image;
                sh.image = std::make_shared<const std::string>(std::move(bytes));
                sh.image_ext = ExtOf(href);
                s->shapes.push_back(std::move(sh));
                return;
            }
        }
        ApplyGraphic(g, &sh);
        // A shape with neither fill nor outline is a text box; a real
        // shape's text is centred both ways unless its style says otherwise.
        const bool visible = !sh.fill.empty() || !sh.line.empty();
        if (!visible && sh.kind == ShapeKind::Rect) sh.kind = ShapeKind::Text;
        if (Prop(g, "textarea-vertical-align").empty()) sh.valign = visible ? VAlign::Middle : VAlign::Top;
        const std::string ha = Prop(g, "textarea-horizontal-align");
        default_align_ = ha == "center" || (ha.empty() && visible) ? Align::Center : Align::Left;
        ReadText(n, sh.font_pt, "", false, &sh);
        default_align_ = Align::Left;
        if (sh.paras.empty()) {
            Paragraph p;
            p.align = Align::Center;
            sh.paras.push_back(p);
        }
        s->shapes.push_back(std::move(sh));
    }

    void ReadLine(const xml::xml_node &n, Slide *s) {
        Shape sh;
        sh.kind = ShapeKind::Line;
        sh.name = Attr(n, "name");
        const long x1 = OdfLengthToEmu(Attr(n, "x1")), y1 = OdfLengthToEmu(Attr(n, "y1"));
        const long x2 = OdfLengthToEmu(Attr(n, "x2")), y2 = OdfLengthToEmu(Attr(n, "y2"));
        sh.x = x1;
        sh.y = y1;
        sh.w = x2 - x1;
        sh.h = y2 - y1;
        ApplyGraphic(ShapeStyle(n), &sh);
        if (sh.line.empty()) {
            sh.line = "1F2328";
            sh.line_pt = 1;
        }
        sh.fill.clear();
        s->shapes.push_back(std::move(sh));
    }

    // Text inside a text box or a shape: paragraphs, headings and lists.
    void ReadText(const xml::xml_node &box, double base_pt, const std::string &list_style, bool outline, Shape *sh) {
        for (const xml::xml_node &c : Elements(box)) {
            if (Is(c, "p") || Is(c, "h")) ReadPara(c, base_pt, 0, false, false, sh);
            else if (Is(c, "list")) ReadList(c, base_pt, 0, list_style.empty() ? Attr(c, "style-name") : list_style, outline, sh);
        }
    }

    void ReadList(const xml::xml_node &list, double base_pt, int depth, std::string style, bool outline, Shape *sh) {
        if (!Attr(list, "style-name").empty()) style = Attr(list, "style-name");
        bool bullet = true, numbered = false;
        auto ls = styles_.lists.find(style);
        if (ls != styles_.lists.end()) {
            auto lv = ls->second.find(depth + 1);
            if (lv != ls->second.end()) {
                bullet = lv->second == 1;
                numbered = lv->second == 2;
            }
        } else if (!outline && style.empty()) {
            bullet = true;
        }
        for (const xml::xml_node &item : Elements(list)) {
            if (!Is(item, "list-item") && !Is(item, "list-header")) continue;
            for (const xml::xml_node &c : Elements(item)) {
                if (Is(c, "p") || Is(c, "h")) ReadPara(c, base_pt, depth, bullet && !Is(item, "list-header"), numbered, sh);
                else if (Is(c, "list")) ReadList(c, base_pt, depth + 1, style, outline, sh);
            }
        }
    }

    TextRun RunStyle(const std::string &style, double base_pt, const TextRun &inherit) const {
        TextRun r = inherit;
        if (style.empty()) return r;
        const auto t = styles_.Resolve(style, "text");
        const double size = FontSizePt(Prop(t, "font-size"), base_pt);
        if (size > 0 && std::fabs(size - base_pt) > 0.01) r.size_pt = size;
        if (!Prop(t, "font-weight").empty()) r.bold = Prop(t, "font-weight") == "bold" || std::atoi(Prop(t, "font-weight").c_str()) >= 600;
        if (!Prop(t, "font-style").empty()) r.italic = Prop(t, "font-style") == "italic" || Prop(t, "font-style") == "oblique";
        const std::string u = Prop(t, "text-underline-style");
        if (!u.empty()) r.underline = u != "none";
        const std::string st = Prop(t, "text-line-through-style");
        if (!st.empty()) r.strike = st != "none";
        const std::string pos = Prop(t, "text-position");
        if (!pos.empty()) r.baseline = pos.rfind("super", 0) == 0 || (std::atof(pos.c_str()) > 0) ? 1 : pos.rfind("sub", 0) == 0 || std::atof(pos.c_str()) < 0 ? -1 : 0;
        const std::string color = NormalizeColor(Prop(t, "color"));
        if (!color.empty()) r.color = color;
        std::string font = Prop(t, "font-family");
        if (font.empty()) font = Prop(t, "font-name");
        if (!font.empty()) {
            if (font.front() == '\'' && font.size() > 1) font = font.substr(1, font.size() - 2);
            r.font = font;
        }
        return r;
    }

    void ReadPara(const xml::xml_node &p, double base_pt, int depth, bool bullet, bool numbered, Shape *sh) {
        Paragraph para;
        para.level = depth;
        para.bullet = bullet;
        para.numbered = numbered;
        const std::string pstyle = Attr(p, "style-name");
        const auto props = styles_.Resolve(pstyle, "paragraph");
        const std::string align = Prop(props, "text-align");
        para.align = default_align_;
        if (align == "start" || align == "left") para.align = Align::Left;
        else if (align == "center") para.align = Align::Center;
        else if (align == "end" || align == "right") para.align = Align::Right;
        else if (align == "justify") para.align = Align::Justify;
        const TextRun para_run = RunStyle(pstyle, base_pt, TextRun{});
        // Text joins the run before it when their styles match (<text:s/>
        // and friends split what was written as one run).
        auto append = [&](TextRun r) {
            if (r.color == sh->text_color || (sh->text_color.empty() && r.color == "000000")) r.color.clear();
            if (!para.runs.empty() && para.runs.back().bold == r.bold && para.runs.back().italic == r.italic &&
                para.runs.back().underline == r.underline && para.runs.back().strike == r.strike && para.runs.back().color == r.color &&
                para.runs.back().size_pt == r.size_pt && para.runs.back().font == r.font && para.runs.back().link == r.link &&
                para.runs.back().baseline == r.baseline)
                para.runs.back().text += r.text;
            else
                para.runs.push_back(std::move(r));
        };
        std::function<void(const xml::xml_node &, const TextRun &)> walk = [&](const xml::xml_node &n, const TextRun &style) {
            for (xml::xml_node c = n.first_child(); c; c = c.next_sibling()) {
                if (c.type() == xml::node_pcdata || c.type() == xml::node_cdata) {
                    TextRun r = style;
                    r.text = c.value();
                    append(std::move(r));
                    continue;
                }
                if (c.type() != xml::node_element) continue;
                if (Is(c, "span")) walk(c, RunStyle(Attr(c, "style-name"), base_pt, style));
                else if (Is(c, "a")) {
                    TextRun l = style;
                    l.link = Attr(c, "href");
                    walk(c, l);
                } else if (Is(c, "s")) {
                    TextRun r = style;
                    const int n_sp = std::max(1, std::atoi(Attr(c, "c").c_str()));
                    r.text = std::string(static_cast<size_t>(n_sp), ' ');
                    append(std::move(r));
                } else if (Is(c, "tab")) {
                    TextRun r = style;
                    r.text = "\t";
                    append(std::move(r));
                } else if (Is(c, "line-break")) {
                    Paragraph next = para;
                    next.runs.clear();
                    sh->paras.push_back(std::move(para));
                    para = std::move(next);
                } else {
                    walk(c, style);  // fields, bookmarks, ...: their text
                }
            }
        };
        walk(p, para_run);
        sh->paras.push_back(std::move(para));
    }
};

// --- Writing ------------------------------------------------------------------------

const char *kNs =
    "xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
    "xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" "
    "xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" "
    "xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\" "
    "xmlns:draw=\"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0\" "
    "xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\" "
    "xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
    "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
    "xmlns:meta=\"urn:oasis:names:tc:opendocument:xmlns:meta:1.0\" "
    "xmlns:svg=\"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0\" "
    "xmlns:presentation=\"urn:oasis:names:tc:opendocument:xmlns:presentation:1.0\" "
    "office:version=\"1.3\"";

std::string Pt(double pt) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1fpt", pt);
    return b;
}

// Text as ODF: runs of spaces as <text:s/>, tabs as <text:tab/>.
std::string OdfText(const std::string &t) {
    std::string o;
    size_t i = 0;
    while (i < t.size()) {
        const char c = t[i];
        if (c == '\t') {
            o += "<text:tab/>";
            ++i;
        } else if (c == ' ' && (i == 0 || i + 1 == t.size() || t[i + 1] == ' ')) {
            size_t j = i;
            while (j < t.size() && t[j] == ' ') ++j;
            if (i > 0 && j < t.size()) {
                o += ' ';
                ++i;
            }
            if (j > i) o += "<text:s text:c=\"" + Num(static_cast<long>(j - i)) + "\"/>";
            i = j;
        } else {
            o += Esc(std::string(1, c));
            ++i;
        }
    }
    return o;
}

class Writer {
public:
    std::string auto_styles;
    std::vector<std::pair<std::string, std::string>> pictures;
    std::vector<std::pair<std::string, std::string>> formulas;  // ("Object N", its MathML)

    // An equation shape: a formula object (MathML, its TeX kept as an
    // annotation) as large as the equation is at the shape's text size and
    // centred in the shape -- Impress stretches a formula to its frame.
    std::string FormulaXml(const Shape &s, const std::string &name) {
        const TextRun &r = s.paras[0].runs[0];
        const double pt = r.size_pt > 0 ? r.size_pt : s.font_pt;
        double wem = 0, hem = 0;
        TexMathExtent(r.tex, true, &wem, &hem);
        Shape f = s;
        f.w = std::min(std::labs(s.w), std::lround(wem * pt * kEmuPerPt));
        f.h = std::min(std::labs(s.h), std::lround(hem * pt * kEmuPerPt));
        f.x = std::min(s.x, s.x + s.w) + (std::labs(s.w) - f.w) / 2;
        f.y = std::min(s.y, s.y + s.h) + (std::labs(s.h) - f.h) / 2;
        const std::string obj = "Object " + Num(static_cast<long>(formulas.size()) + 1);
        formulas.emplace_back(obj, TexToLibreOfficeMathMl(r.tex, true));
        return "<draw:frame draw:style-name=\"" + GraphicStyle(Shape{}, true) + "\" draw:layer=\"layout\"" + name + " " + Geom(f) +
               "><draw:object xlink:href=\"./" + obj + "\" xlink:type=\"simple\" xlink:show=\"embed\" xlink:actuate=\"onLoad\"/></draw:frame>";
    }

    std::string Style(const std::string &family, const std::string &body) {
        const std::string key = family + "|" + body;
        auto it = styles_.find(key);
        if (it != styles_.end()) return it->second;
        const std::string name = std::string(family == "graphic" ? "gr" : family == "paragraph" ? "P" : family == "text" ? "T" : "dp") +
                                 Num(static_cast<long>(styles_.size()) + 1);
        auto_styles += "<style:style style:name=\"" + name + "\" style:family=\"" + family + "\">" + body + "</style:style>";
        styles_[key] = name;
        return name;
    }

    std::string TextStyle(const TextRun &r, const Shape &s) {
        const double pt = r.size_pt > 0 ? r.size_pt : s.font_pt;
        std::string props = "fo:font-size=\"" + Pt(pt) + "\"";
        props += r.bold ? " fo:font-weight=\"bold\"" : " fo:font-weight=\"normal\"";
        props += r.italic ? " fo:font-style=\"italic\"" : " fo:font-style=\"normal\"";
        if (r.underline) props += " style:text-underline-style=\"solid\" style:text-underline-width=\"auto\" style:text-underline-color=\"font-color\"";
        if (r.strike) props += " style:text-line-through-style=\"solid\"";
        if (r.baseline) props += r.baseline > 0 ? " style:text-position=\"super 58%\"" : " style:text-position=\"sub 58%\"";
        const std::string color = !r.color.empty() ? r.color : s.text_color;
        if (!color.empty()) props += " fo:color=\"#" + color + "\"";
        if (!r.font.empty()) props += " fo:font-family=\"'" + Esc(r.font) + "'\"";
        return Style("text", "<style:text-properties " + props + "/>");
    }

    std::string ParaXml(const Paragraph &p, const Shape &s) {
        const char *align = p.align == Align::Center ? "center" : p.align == Align::Right ? "end" : p.align == Align::Justify ? "justify" : "start";
        std::string o = "<text:p text:style-name=\"" +
                        Style("paragraph", std::string("<style:paragraph-properties fo:text-align=\"") + align +
                                               "\" fo:margin-top=\"0cm\" fo:margin-bottom=\"0cm\"/><style:text-properties fo:font-size=\"" + Pt(s.font_pt) + "\"/>") +
                        "\">";
        for (const TextRun &r : p.runs) {
            // Inline maths: typeset text (an Impress text box cannot hold a
            // formula), in a serif as maths is set.
            if (r.IsMath()) {
                for (const MathTextRun &m : TexToTextRuns(r.tex)) {
                    TextRun t = r;
                    t.tex.clear();
                    t.text = m.text;
                    t.italic = m.italic;
                    t.bold = r.bold || m.bold;
                    t.baseline = m.script;
                    t.font = "Liberation Serif";
                    o += "<text:span text:style-name=\"" + TextStyle(t, s) + "\">" + OdfText(t.text) + "</text:span>";
                }
                continue;
            }
            std::string text = OdfText(r.text);
            if (!r.link.empty()) text = "<text:a xlink:type=\"simple\" xlink:href=\"" + Esc(r.link) + "\">" + text + "</text:a>";
            o += "<text:span text:style-name=\"" + TextStyle(r, s) + "\">" + text + "</text:span>";
        }
        return o + "</text:p>";
    }

    std::string TextXml(const Shape &s) {
        std::string o;
        for (const Paragraph &p : s.paras) {
            if (!p.bullet && !p.numbered) {
                o += ParaXml(p, s);
                continue;
            }
            std::string open, close;
            for (int k = 0; k <= p.level; ++k) {
                open += k == 0 ? "<text:list text:style-name=\"" + std::string(p.numbered ? "LNum" : "LBul") + "\">" : "<text:list>";
                open += "<text:list-item>";
                close = "</text:list-item></text:list>" + close;
            }
            o += open + ParaXml(p, s) + close;
        }
        return o;
    }

    std::string GraphicStyle(const Shape &s, bool text_box) {
        std::string props;
        props += s.fill.empty() ? "draw:fill=\"none\"" : "draw:fill=\"solid\" draw:fill-color=\"#" + s.fill + "\"";
        if (s.line.empty() || s.line_pt <= 0) props += " draw:stroke=\"none\"";
        else {
            char w[32];
            std::snprintf(w, sizeof w, "%.3fcm", s.line_pt * 2.54 / 72.0);
            props += std::string(" draw:stroke=\"solid\" svg:stroke-color=\"#") + s.line + "\" svg:stroke-width=\"" + w + "\"";
        }
        const char *va = s.valign == VAlign::Middle ? "middle" : s.valign == VAlign::Bottom ? "bottom" : "top";
        props += std::string(" draw:textarea-vertical-align=\"") + va + "\" draw:auto-grow-height=\"false\"";
        props += " fo:padding-top=\"0.13cm\" fo:padding-bottom=\"0.13cm\" fo:padding-left=\"0.25cm\" fo:padding-right=\"0.25cm\"";
        if (s.autofit) props += " style:shrink-to-fit=\"true\"";
        if (!text_box) props += " draw:textarea-horizontal-align=\"justify\"";
        // The shape's text defaults, where a reader looks for them first.
        std::string text = "fo:font-size=\"" + Pt(s.font_pt) + "\"";
        if (!s.text_color.empty()) text += " fo:color=\"#" + s.text_color + "\"";
        return Style("graphic", "<style:graphic-properties " + props + "/><style:text-properties " + text + "/>");
    }

    static std::string Geom(const Shape &s) {
        long x = s.x, y = s.y, w = s.w, h = s.h;
        if (w < 0) {
            x += w;
            w = -w;
        }
        if (h < 0) {
            y += h;
            h = -h;
        }
        return "svg:x=\"" + EmuToCm(x) + "\" svg:y=\"" + EmuToCm(y) + "\" svg:width=\"" + EmuToCm(w) + "\" svg:height=\"" + EmuToCm(h) + "\"";
    }

    std::string ShapeXml(const Shape &s) {
        const std::string name = s.name.empty() ? "" : " draw:name=\"" + Esc(s.name) + "\"";
        switch (s.kind) {
            case ShapeKind::Image: {
                if (!s.image) return "";
                const std::string ext = s.image_ext.empty() ? "png" : s.image_ext;
                const std::string file = "Pictures/image" + Num(static_cast<long>(pictures.size()) + 1) + "." + ext;
                pictures.emplace_back(file, *s.image);
                std::string o = "<draw:frame draw:style-name=\"" + GraphicStyle(Shape{}, true) + "\" draw:layer=\"layout\"" + name + " " + Geom(s) +
                                "><draw:image xlink:href=\"" + file + "\" xlink:type=\"simple\" xlink:show=\"embed\" xlink:actuate=\"onLoad\"><text:p/></draw:image>";
                if (!s.alt.empty()) o += "<svg:desc>" + Esc(s.alt) + "</svg:desc>";
                return o + "</draw:frame>";
            }
            case ShapeKind::Line:
                return "<draw:line draw:style-name=\"" + GraphicStyle(s, false) + "\" draw:layer=\"layout\"" + name + " svg:x1=\"" + EmuToCm(s.x) +
                       "\" svg:y1=\"" + EmuToCm(s.y) + "\" svg:x2=\"" + EmuToCm(s.x + s.w) + "\" svg:y2=\"" + EmuToCm(s.y + s.h) + "\"><text:p/></draw:line>";
            case ShapeKind::Text:
                if (IsEquationShape(s)) return FormulaXml(s, name);
                return "<draw:frame draw:style-name=\"" + GraphicStyle(s, true) + "\" draw:layer=\"layout\"" + name +
                       (s.is_title ? " presentation:class=\"title\"" : "") + " " + Geom(s) + "><draw:text-box>" + TextXml(s) + "</draw:text-box></draw:frame>";
            case ShapeKind::Rect:
            case ShapeKind::RoundRect:
                return "<draw:rect draw:style-name=\"" + GraphicStyle(s, false) + "\" draw:layer=\"layout\"" + name +
                       (s.kind == ShapeKind::RoundRect ? " draw:corner-radius=\"" + EmuToCm(std::min(std::labs(s.w), std::labs(s.h)) / 6) + "\"" : "") + " " +
                       Geom(s) + ">" + TextXml(s) + "</draw:rect>";
            case ShapeKind::Ellipse:
                return "<draw:ellipse draw:style-name=\"" + GraphicStyle(s, false) + "\" draw:layer=\"layout\"" + name + " " + Geom(s) + ">" + TextXml(s) +
                       "</draw:ellipse>";
        }
        return "";
    }

    std::string PageXml(const Slide &sl, int n) {
        std::string style;
        if (!sl.background.empty())
            style = Style("drawing-page", "<style:drawing-page-properties presentation:background-visible=\"true\" draw:fill=\"solid\" draw:fill-color=\"#" +
                                              sl.background + "\"/>");
        else
            style = Style("drawing-page", "<style:drawing-page-properties presentation:background-visible=\"true\"/>");
        std::string o = "<draw:page draw:name=\"page" + Num(n) + "\" draw:style-name=\"" + style + "\" draw:master-page-name=\"Default\">";
        for (const Shape &s : sl.shapes) o += ShapeXml(s);
        return o + "</draw:page>";
    }

private:
    std::map<std::string, std::string> styles_;
};

std::string ListStyles() {
    std::string bul = "<text:list-style style:name=\"LBul\">", num = "<text:list-style style:name=\"LNum\">";
    for (int l = 1; l <= 9; ++l) {
        char props[160];
        std::snprintf(props, sizeof props, "<style:list-level-properties text:space-before=\"%.2fcm\" text:min-label-width=\"0.8cm\"/>", (l - 1) * 0.9);
        bul += "<text:list-level-style-bullet text:level=\"" + Num(l) + "\" text:bullet-char=\"" + std::string(l % 2 ? "•" : "–") +
               "\" text:bullet-relative-size=\"100%\">" + props + "<style:text-properties fo:font-family=\"'Liberation Sans'\" fo:font-size=\"100%\"/></text:list-level-style-bullet>";
        num += "<text:list-level-style-number text:level=\"" + Num(l) + "\" style:num-format=\"1\" style:num-suffix=\".\">" + props +
               "<style:text-properties fo:font-size=\"100%\"/></text:list-level-style-number>";
    }
    return bul + "</text:list-style>" + num + "</text:list-style>";
}

}  // namespace

bool LoadOdp(const std::string &bytes, Presentation *out, std::string *error) {
    Presentation p;
    Reader r(bytes);
    if (!r.Read(&p, error)) return false;
    *out = std::move(p);
    return true;
}

std::string SaveOdp(const Presentation &p) {
    Writer w;
    std::string pages;
    for (size_t k = 0; k < p.slides.size(); ++k) pages += w.PageXml(p.slides[k], static_cast<int>(k) + 1);
    const std::string content = std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-content ") + kNs + "><office:automatic-styles>" +
                                w.auto_styles + ListStyles() + "</office:automatic-styles><office:body><office:presentation>" + pages +
                                "</office:presentation></office:body></office:document-content>";
    const std::string styles =
        std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-styles ") + kNs +
        "><office:styles><style:default-style style:family=\"graphic\"><style:text-properties fo:font-family=\"'Liberation Sans'\" "
        "fo:font-size=\"18pt\" fo:color=\"#000000\"/></style:default-style></office:styles><office:automatic-styles>"
        "<style:page-layout style:name=\"PM1\"><style:page-layout-properties fo:margin-top=\"0cm\" fo:margin-bottom=\"0cm\" fo:margin-left=\"0cm\" "
        "fo:margin-right=\"0cm\" fo:page-width=\"" + EmuToCm(p.width) + "\" fo:page-height=\"" + EmuToCm(p.height) +
        "\" style:print-orientation=\"landscape\"/></style:page-layout>"
        "<style:style style:name=\"Mdp1\" style:family=\"drawing-page\"><style:drawing-page-properties draw:background-size=\"border\" "
        "draw:fill=\"solid\" draw:fill-color=\"#ffffff\"/></style:style></office:automatic-styles>"
        "<office:master-styles><draw:layer-set><draw:layer draw:name=\"layout\"/><draw:layer draw:name=\"background\"/>"
        "<draw:layer draw:name=\"backgroundobjects\"/><draw:layer draw:name=\"controls\"/><draw:layer draw:name=\"measurelines\"/></draw:layer-set>"
        "<style:master-page style:name=\"Default\" style:page-layout-name=\"PM1\" draw:style-name=\"Mdp1\"/></office:master-styles></office:document-styles>";
    const std::string meta = std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-meta ") + kNs +
                             "><office:meta><meta:generator>mep</meta:generator><dc:title>" + Esc(p.title) + "</dc:title><meta:initial-creator>" +
                             Esc(p.author) + "</meta:initial-creator></office:meta></office:document-meta>";
    std::string manifest = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.3\">"
                           "<manifest:file-entry manifest:full-path=\"/\" manifest:version=\"1.3\" manifest:media-type=\"application/vnd.oasis.opendocument.presentation\"/>"
                           "<manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>"
                           "<manifest:file-entry manifest:full-path=\"styles.xml\" manifest:media-type=\"text/xml\"/>"
                           "<manifest:file-entry manifest:full-path=\"meta.xml\" manifest:media-type=\"text/xml\"/>";
    for (const auto &pic : w.pictures)
        manifest += "<manifest:file-entry manifest:full-path=\"" + pic.first + "\" manifest:media-type=\"" + MimeForExt(ExtOf(pic.first)) + "\"/>";
    for (const auto &f : w.formulas)
        manifest += "<manifest:file-entry manifest:full-path=\"" + f.first + "/\" manifest:version=\"1.3\" "
                    "manifest:media-type=\"application/vnd.oasis.opendocument.formula\"/>"
                    "<manifest:file-entry manifest:full-path=\"" + f.first + "/content.xml\" manifest:media-type=\"text/xml\"/>";
    manifest += "</manifest:manifest>";
    std::vector<zip::EntryToWrite> e = {
        {"mimetype", "application/vnd.oasis.opendocument.presentation", true},
        {"META-INF/manifest.xml", manifest},
        {"content.xml", content},
        {"styles.xml", styles},
        {"meta.xml", meta},
    };
    for (const auto &pic : w.pictures) e.push_back({pic.first, pic.second, true});
    for (const auto &f : w.formulas) e.push_back({f.first + "/content.xml", "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n" + f.second});
    return zip::BuildArchive(e);
}

}  // namespace pres
