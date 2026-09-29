// PowerPoint (.pptx) reading and writing for pres_doc.h.
//
// Reading resolves what a PowerPoint slide inherits, the way PowerPoint
// draws it: a placeholder without its own position takes its layout's (or
// failing that its master's); text takes its size, bullets and alignment
// from the placeholder chain and then from the master's text styles; a
// theme colour (<a:schemeClr>) comes from the theme with its tints and
// shades applied. Groups are flattened (their child coordinate space
// mapped onto the slide); an <mc:AlternateContent> is read from its
// Fallback (what every reader can show); a table becomes a text box with a
// line per row. What has no place in pres_doc.h's model is left out.

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

MathPictureFn &MathPicture() {
    static MathPictureFn fn;
    return fn;
}

// --- Reading ------------------------------------------------------------------------

struct Zip {
    const std::string &bytes;
    bool Get(const std::string &name, std::string *out) const {
        return zip::Extract(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size(), name.c_str(), *out);
    }
};

// A part's relationships: id -> (type, resolved target).
struct Rels {
    std::map<std::string, std::pair<std::string, std::string>> by_id;
    std::string Target(const std::string &id) const {
        auto it = by_id.find(id);
        return it == by_id.end() ? std::string() : it->second.second;
    }
    std::string OfType(const std::string &type_suffix) const {
        for (const auto &kv : by_id) {
            const std::string &t = kv.second.first;
            if (t.size() >= type_suffix.size() && t.compare(t.size() - type_suffix.size(), type_suffix.size(), type_suffix) == 0)
                return kv.second.second;
        }
        return "";
    }
};

Rels ReadRels(const Zip &z, const std::string &part) {
    Rels r;
    const size_t slash = part.rfind('/');
    const std::string rels_part =
        (slash == std::string::npos ? std::string() : part.substr(0, slash + 1)) + "_rels/" + part.substr(slash + 1) + ".rels";
    std::string text;
    if (!z.Get(rels_part, &text)) return r;
    xml::xml_document doc;
    xml::xml_node root;
    if (!ParseXml(text, &doc, &root)) return r;
    for (const xml::xml_node &rel : Children(root, "Relationship")) {
        const std::string mode = Attr(rel, "TargetMode");
        const std::string target = Attr(rel, "Target");
        r.by_id[Attr(rel, "Id")] = {Attr(rel, "Type"), mode == "External" ? target : ResolvePart(part, target)};
    }
    return r;
}

// One level of text defaults (<a:lvlNpPr>, a placeholder's lstStyle, the
// master's txStyles).
struct LevelStyle {
    double size_pt = 0;
    int bullet = -1;  // -1 unknown, 0 none, 1 bullet, 2 numbered
    int align = -1;   // Align, -1 unknown
    std::string color;
    bool bold = false, has_bold = false;
};
using LevelStyles = std::map<int, LevelStyle>;  // level 1..9

// What a placeholder contributes: its position and its text defaults.
struct Placeholder {
    bool has_xfrm = false;
    long x = 0, y = 0, w = 0, h = 0;
    LevelStyles levels;
    int anchor = -1;  // VAlign, -1 unknown
    std::string fill;
};

struct PlaceholderSet {
    std::map<std::string, Placeholder> by_type;  // "title", "body", "subTitle", ...
    std::map<std::string, Placeholder> by_idx;
    const Placeholder *Find(const std::string &type, const std::string &idx) const {
        if (!idx.empty()) {
            auto it = by_idx.find(idx);
            if (it != by_idx.end()) return &it->second;
        }
        auto it = by_type.find(type);
        if (it != by_type.end()) return &it->second;
        return nullptr;
    }
};

struct Theme {
    std::map<std::string, std::string> colors;  // dk1, lt1, accent1, ...
    std::string minor_font, major_font;
};

class Reader {
public:
    Reader(const Zip &z) : zip_(z) {}

    bool Read(Presentation *out, std::string *error) {
        std::string pres_text;
        xml::xml_document pres_doc;
        xml::xml_node pres;
        if (!zip_.Get("ppt/presentation.xml", &pres_text) || !ParseXml(pres_text, &pres_doc, &pres)) {
            if (error) *error = "not a PowerPoint file (no ppt/presentation.xml)";
            return false;
        }
        if (xml::xml_node sz = Child(pres, "sldSz")) {
            out->width = std::atol(Attr(sz, "cx").c_str());
            out->height = std::atol(Attr(sz, "cy").c_str());
        }
        if (out->width <= 0 || out->height <= 0) {
            out->width = 12192000;
            out->height = 6858000;
        }
        const Rels rels = ReadRels(zip_, "ppt/presentation.xml");
        ReadTheme(rels.OfType("/theme"));
        out->slides.clear();
        for (const xml::xml_node &id : Children(Child(pres, "sldIdLst"), "sldId")) {
            // (r:id, not the slide's own numeric id="...")
            std::string rid = id.attribute("r:id").as_string();
            if (rid.empty())
                for (const char *q : {"relationships:id", "R:id"}) rid = id.attribute(q).as_string(), (void)q;
            const std::string part = rels.Target(rid);
            if (part.empty()) continue;
            Slide s;
            if (ReadSlide(part, &s)) out->slides.push_back(std::move(s));
        }
        std::string core;
        xml::xml_document core_doc;
        xml::xml_node core_root;
        if (zip_.Get("docProps/core.xml", &core) && ParseXml(core, &core_doc, &core_root)) {
            out->title = AllText(Child(core_root, "title"));
            out->author = AllText(Child(core_root, "creator"));
        }
        if (out->slides.empty()) out->slides.push_back(Slide{});
        return true;
    }

private:
    const Zip &zip_;
    Theme theme_;
    std::map<std::string, std::string> color_map_ = {{"tx1", "dk1"}, {"bg1", "lt1"}, {"tx2", "dk2"}, {"bg2", "lt2"}};
    // The master's text styles.
    LevelStyles title_style_, body_style_, other_style_;

    void ReadTheme(const std::string &part) {
        std::string text;
        xml::xml_document doc;
        xml::xml_node root;
        if (part.empty() || !zip_.Get(part, &text) || !ParseXml(text, &doc, &root)) return;
        xml::xml_node elems = Child(root, "themeElements");
        for (const xml::xml_node &c : Elements(Child(elems, "clrScheme"))) {
            std::string v;
            if (xml::xml_node srgb = Child(c, "srgbClr")) v = Attr(srgb, "val");
            else if (xml::xml_node sys = Child(c, "sysClr")) v = Attr(sys, "lastClr");
            theme_.colors[Local(c.name())] = NormalizeColor(v);
        }
        xml::xml_node fonts = Child(elems, "fontScheme");
        theme_.minor_font = Attr(Path(fonts, "minorFont/latin"), "typeface");
        theme_.major_font = Attr(Path(fonts, "majorFont/latin"), "typeface");
    }

    std::string Font(const std::string &typeface) const {
        if (typeface == "+mn-lt") return theme_.minor_font;
        if (typeface == "+mj-lt") return theme_.major_font;
        return typeface;
    }

    // The colour a colour element (<a:srgbClr>, <a:schemeClr>, ...) holds,
    // with its modifiers.
    std::string ColorOf(const xml::xml_node &c) const {
        std::string rgb;
        if (Is(c, "srgbClr")) rgb = NormalizeColor(Attr(c, "val"));
        else if (Is(c, "sysClr")) rgb = NormalizeColor(Attr(c, "lastClr"));
        else if (Is(c, "prstClr")) rgb = Attr(c, "val") == "white" ? "FFFFFF" : "000000";
        else if (Is(c, "schemeClr")) {
            std::string name = Attr(c, "val");
            auto m = color_map_.find(name);
            if (m != color_map_.end()) name = m->second;
            auto t = theme_.colors.find(name);
            rgb = t == theme_.colors.end() ? std::string() : t->second;
        } else {
            return "";
        }
        int lum_mod = 100000, lum_off = 0, shade = 0, tint = 0;
        for (const xml::xml_node &mod : Elements(c)) {
            const int v = std::atoi(Attr(mod, "val").c_str());
            if (Is(mod, "lumMod")) lum_mod = v;
            else if (Is(mod, "lumOff")) lum_off = v;
            else if (Is(mod, "shade")) shade = v;
            else if (Is(mod, "tint")) tint = v;
        }
        return ApplyShade(ApplyLum(rgb, lum_mod, lum_off), shade, tint);
    }

    // The colour inside a fill-like element (<a:solidFill>, a gradient's first stop).
    std::string FillColor(const xml::xml_node &fill) const {
        if (Is(fill, "solidFill")) {
            for (const xml::xml_node &c : Elements(fill)) return ColorOf(c);
        }
        if (Is(fill, "gradFill")) {
            xml::xml_node gs = Path(fill, "gsLst/gs");
            for (const xml::xml_node &c : Elements(gs)) return ColorOf(c);
        }
        return "";
    }

    static int AlignOf(const std::string &algn) {
        if (algn == "ctr") return static_cast<int>(Align::Center);
        if (algn == "r") return static_cast<int>(Align::Right);
        if (algn == "just" || algn == "dist") return static_cast<int>(Align::Justify);
        if (algn == "l") return static_cast<int>(Align::Left);
        return -1;
    }

    LevelStyles ReadLevels(const xml::xml_node &lst) const {
        LevelStyles out;
        if (!lst) return out;
        for (int lvl = 1; lvl <= 9; ++lvl) {
            const std::string name = "lvl" + std::to_string(lvl) + "pPr";
            xml::xml_node p = Child(lst, name.c_str());
            if (!p) continue;
            LevelStyle s;
            s.align = AlignOf(Attr(p, "algn"));
            if (Child(p, "buNone")) s.bullet = 0;
            else if (Child(p, "buAutoNum")) s.bullet = 2;
            else if (Child(p, "buChar") || Child(p, "buBlip")) s.bullet = 1;
            if (xml::xml_node d = Child(p, "defRPr")) {
                const std::string sz = Attr(d, "sz");
                if (!sz.empty()) s.size_pt = std::atof(sz.c_str()) / 100.0;
                for (const xml::xml_node &f : Elements(d))
                    if (Is(f, "solidFill")) s.color = FillColor(f);
                if (HasAttr(d, "b")) {
                    s.has_bold = true;
                    s.bold = Attr(d, "b") == "1" || Attr(d, "b") == "true";
                }
            }
            out[lvl] = s;
        }
        return out;
    }

    static void Overlay(LevelStyles *base, const LevelStyles &top) {
        for (const auto &kv : top) {
            LevelStyle &b = (*base)[kv.first];
            const LevelStyle &t = kv.second;
            if (t.size_pt > 0) b.size_pt = t.size_pt;
            if (t.bullet >= 0) b.bullet = t.bullet;
            if (t.align >= 0) b.align = t.align;
            if (!t.color.empty()) b.color = t.color;
            if (t.has_bold) {
                b.has_bold = true;
                b.bold = t.bold;
            }
        }
    }

    static bool ReadXfrm(const xml::xml_node &xfrm, long *x, long *y, long *w, long *h) {
        xml::xml_node off = Child(xfrm, "off"), ext = Child(xfrm, "ext");
        if (!off || !ext) return false;
        *x = std::atol(Attr(off, "x").c_str());
        *y = std::atol(Attr(off, "y").c_str());
        *w = std::atol(Attr(ext, "cx").c_str());
        *h = std::atol(Attr(ext, "cy").c_str());
        return true;
    }

    static void PhKey(const xml::xml_node &sp, std::string *type, std::string *idx, bool *is_ph) {
        xml::xml_node nv;
        for (const xml::xml_node &c : Elements(sp))
            if (std::strncmp(Local(c.name()), "nv", 2) == 0) nv = c;
        xml::xml_node ph = Path(nv, "nvPr/ph");
        *is_ph = static_cast<bool>(ph);
        *type = ph ? Attr(ph, "type") : "";
        if (ph && type->empty()) *type = "body";
        *idx = ph ? Attr(ph, "idx") : "";
    }

    // A layout's or master's placeholders.
    PlaceholderSet ReadPlaceholders(const xml::xml_node &root) const {
        PlaceholderSet set;
        for (const xml::xml_node &sp : Children(Path(root, "cSld/spTree"), "sp")) {
            std::string type, idx;
            bool is_ph = false;
            PhKey(sp, &type, &idx, &is_ph);
            if (!is_ph) continue;
            Placeholder p;
            p.has_xfrm = ReadXfrm(Path(sp, "spPr/xfrm"), &p.x, &p.y, &p.w, &p.h);
            p.levels = ReadLevels(Path(sp, "txBody/lstStyle"));
            const std::string anchor = Attr(Path(sp, "txBody/bodyPr"), "anchor");
            p.anchor = anchor == "ctr" ? 1 : anchor == "b" ? 2 : anchor == "t" ? 0 : -1;
            for (const xml::xml_node &f : Elements(Child(sp, "spPr")))
                if (Is(f, "solidFill")) p.fill = FillColor(f);
            if (!idx.empty()) set.by_idx[idx] = p;
            if (set.by_type.find(type) == set.by_type.end()) set.by_type[type] = p;
        }
        return set;
    }

    std::string BackgroundOf(const xml::xml_node &root) const {
        xml::xml_node bg = Path(root, "cSld/bg");
        if (!bg) return "";
        if (xml::xml_node pr = Child(bg, "bgPr")) {
            for (const xml::xml_node &f : Elements(pr))
                if (Is(f, "solidFill") || Is(f, "gradFill")) return FillColor(f);
        }
        if (xml::xml_node ref = Child(bg, "bgRef")) {
            for (const xml::xml_node &c : Elements(ref)) return ColorOf(c);
        }
        return "";
    }

    struct Context {
        PlaceholderSet layout, master;
        std::string part;
        Rels rels;
    };

    bool ReadSlide(const std::string &part, Slide *out) {
        std::string text;
        xml::xml_document doc;
        xml::xml_node root;
        if (!zip_.Get(part, &text) || !ParseXml(text, &doc, &root)) return false;
        Context ctx;
        ctx.part = part;
        ctx.rels = ReadRels(zip_, part);
        // Layout, then master.
        std::string layout_bg, master_bg;
        const std::string layout_part = ctx.rels.OfType("/slideLayout");
        std::string layout_text, master_text;
        xml::xml_document layout_doc, master_doc;
        xml::xml_node layout_root, master_root;
        if (!layout_part.empty() && zip_.Get(layout_part, &layout_text) && ParseXml(layout_text, &layout_doc, &layout_root)) {
            ctx.layout = ReadPlaceholders(layout_root);
            layout_bg = BackgroundOf(layout_root);
            const std::string master_part = ReadRels(zip_, layout_part).OfType("/slideMaster");
            if (!master_part.empty() && zip_.Get(master_part, &master_text) && ParseXml(master_text, &master_doc, &master_root)) {
                ctx.master = ReadPlaceholders(master_root);
                master_bg = BackgroundOf(master_root);
                if (xml::xml_node cm = Child(master_root, "clrMap")) {
                    for (const char *k : {"tx1", "tx2", "bg1", "bg2"}) {
                        const std::string v = Attr(cm, k);
                        if (!v.empty()) color_map_[k] = v;
                    }
                }
                xml::xml_node tx = Child(master_root, "txStyles");
                title_style_ = ReadLevels(Child(tx, "titleStyle"));
                body_style_ = ReadLevels(Child(tx, "bodyStyle"));
                other_style_ = ReadLevels(Child(tx, "otherStyle"));
            }
        }
        out->background = BackgroundOf(root);
        if (out->background.empty()) out->background = layout_bg.empty() ? master_bg : layout_bg;
        if (out->background == "FFFFFF") out->background.clear();
        ReadTree(Path(root, "cSld/spTree"), ctx, Identity(), out);
        // Speaker notes: the notes slide's body placeholder.
        const std::string notes_part = ctx.rels.OfType("/notesSlide");
        std::string notes_text;
        xml::xml_document notes_doc;
        xml::xml_node notes_root;
        if (!notes_part.empty() && zip_.Get(notes_part, &notes_text) && ParseXml(notes_text, &notes_doc, &notes_root)) {
            for (const xml::xml_node &sp : Children(Path(notes_root, "cSld/spTree"), "sp")) {
                std::string type, idx;
                bool is_ph = false;
                PhKey(sp, &type, &idx, &is_ph);
                if (type != "body") continue;
                for (const xml::xml_node &p : Children(Child(sp, "txBody"), "p")) {
                    if (!out->notes.empty()) out->notes += "\n";
                    out->notes += AllText(p);
                }
            }
        }
        return true;
    }

    static bool HasOfficeMath(const xml::xml_node &n) {
        if (!n) return false;
        for (const xml::xml_node &c : Elements(n)) {
            if (std::strcmp(c.name(), "a14:m") == 0 || HasOfficeMath(c)) return true;
        }
        return false;
    }

    // A group's child coordinate space mapped onto its parent's.
    struct Transform {
        double sx = 1, sy = 1, dx = 0, dy = 0;
    };
    static Transform Identity() { return Transform{}; }

    void ReadTree(const xml::xml_node &tree, const Context &ctx, const Transform &t, Slide *out) {
        for (const xml::xml_node &c : Elements(tree)) {
            if (Is(c, "sp")) ReadSp(c, ctx, t, out);
            else if (Is(c, "pic")) ReadPic(c, ctx, t, out);
            else if (Is(c, "cxnSp")) ReadConnector(c, t, out);
            else if (Is(c, "graphicFrame")) ReadGraphicFrame(c, t, out);
            else if (Is(c, "grpSp")) {
                xml::xml_node xfrm = Path(c, "grpSpPr/xfrm");
                long x, y, w, h;
                Transform inner = t;
                if (ReadXfrm(xfrm, &x, &y, &w, &h)) {
                    xml::xml_node choff = Child(xfrm, "chOff"), chext = Child(xfrm, "chExt");
                    const double cx = std::atof(Attr(choff, "x").c_str()), cy = std::atof(Attr(choff, "y").c_str());
                    const double cw = std::atof(Attr(chext, "cx").c_str()), ch = std::atof(Attr(chext, "cy").c_str());
                    const double sx = cw > 0 ? static_cast<double>(w) / cw : 1, sy = ch > 0 ? static_cast<double>(h) / ch : 1;
                    // child -> group: (v - ch_off) * s + off; then the parent's own.
                    inner.sx = t.sx * sx;
                    inner.sy = t.sy * sy;
                    inner.dx = t.sx * (static_cast<double>(x) - cx * sx) + t.dx;
                    inner.dy = t.sy * (static_cast<double>(y) - cy * sy) + t.dy;
                }
                ReadTree(c, ctx, inner, out);
            } else if (Is(c, "AlternateContent")) {
                // An Office equation (a14:m) from the Choice -- it is the
                // maths itself, where the Fallback is only its picture or
                // text; anything else from what every reader can show.
                xml::xml_node choice = Child(c, "Choice"), fb = Child(c, "Fallback");
                ReadTree(HasOfficeMath(choice) || !fb ? choice : fb, ctx, t, out);
            }
        }
    }

    static void Place(const Transform &t, long x, long y, long w, long h, Shape *s) {
        s->x = std::lround(static_cast<double>(x) * t.sx + t.dx);
        s->y = std::lround(static_cast<double>(y) * t.sy + t.dy);
        s->w = std::lround(static_cast<double>(w) * t.sx);
        s->h = std::lround(static_cast<double>(h) * t.sy);
    }

    void ReadSpPr(const xml::xml_node &sppr, const xml::xml_node &style, Shape *s) const {
        bool fill_set = false, line_set = false;
        for (const xml::xml_node &f : Elements(sppr)) {
            if (Is(f, "solidFill") || Is(f, "gradFill")) {
                s->fill = FillColor(f);
                fill_set = true;
            } else if (Is(f, "noFill")) {
                s->fill.clear();
                fill_set = true;
            } else if (Is(f, "ln")) {
                const std::string w = Attr(f, "w");
                if (!w.empty()) s->line_pt = std::atof(w.c_str()) / kEmuPerPt;
                for (const xml::xml_node &lf : Elements(f)) {
                    if (Is(lf, "solidFill")) {
                        s->line = FillColor(lf);
                        line_set = true;
                    } else if (Is(lf, "noFill")) {
                        s->line.clear();
                        line_set = true;
                    }
                }
            }
        }
        // A shape drawn from the theme's styles (<p:style>): its fill and
        // outline references.
        if (style) {
            xml::xml_node fr = Child(style, "fillRef"), lr = Child(style, "lnRef");
            if (!fill_set && fr && std::atoi(Attr(fr, "idx").c_str()) > 0)
                for (const xml::xml_node &c : Elements(fr)) s->fill = ColorOf(c);
            if (!line_set && lr && std::atoi(Attr(lr, "idx").c_str()) > 0) {
                for (const xml::xml_node &c : Elements(lr)) s->line = ColorOf(c);
                if (s->line_pt <= 0) s->line_pt = 1;
            }
            if (xml::xml_node fontref = Child(style, "fontRef"))
                for (const xml::xml_node &c : Elements(fontref)) s->text_color = ColorOf(c);
        }
        if (s->line.empty()) s->line_pt = 0;
    }

    void ReadSp(const xml::xml_node &sp, const Context &ctx, const Transform &t, Slide *out) {
        Shape s;
        s.name = Attr(Path(sp, "nvSpPr/cNvPr"), "name");
        std::string type, idx;
        bool is_ph = false;
        PhKey(sp, &type, &idx, &is_ph);
        const bool title = type == "title" || type == "ctrTitle";
        s.is_title = title;
        // Footers, dates and slide numbers are the program's to fill in.
        if (type == "dt" || type == "ftr" || type == "sldNum" || type == "hdr") return;
        const Placeholder *lay = is_ph ? ctx.layout.Find(type, idx) : nullptr;
        const Placeholder *mas = is_ph ? ctx.master.Find(type == "ctrTitle" ? "title" : type == "subTitle" ? "body" : type, idx) : nullptr;
        if (is_ph && !mas) mas = ctx.master.Find(title ? "title" : "body", "");
        xml::xml_node sppr = Child(sp, "spPr");
        long x, y, w, h;
        if (ReadXfrm(Child(sppr, "xfrm"), &x, &y, &w, &h)) Place(t, x, y, w, h, &s);
        else if (lay && lay->has_xfrm) Place(t, lay->x, lay->y, lay->w, lay->h, &s);
        else if (mas && mas->has_xfrm) Place(t, mas->x, mas->y, mas->w, mas->h, &s);
        else if (!is_ph) return;
        // A shape filled with a picture (an equation's fallback, a picture
        // placeholder): the picture.
        if (xml::xml_node blip = Path(sppr, "blipFill/blip")) {
            std::string bytes;
            const std::string part = ctx.rels.Target(Attr(blip, "embed"));
            if (!part.empty() && zip_.Get(part, &bytes) && s.w != 0) {
                s.kind = ShapeKind::Image;
                s.image = std::make_shared<const std::string>(std::move(bytes));
                s.image_ext = ExtOf(part);
                out->shapes.push_back(std::move(s));
                return;
            }
        }
        const std::string prst = Attr(Child(sppr, "prstGeom"), "prst");
        if (prst == "ellipse") s.kind = ShapeKind::Ellipse;
        else if (prst == "roundRect") s.kind = ShapeKind::RoundRect;
        else if (prst == "line" || prst == "straightConnector1") s.kind = ShapeKind::Line;
        else if (Attr(Path(sp, "nvSpPr/cNvSpPr"), "txBox") == "1" || is_ph) s.kind = ShapeKind::Text;
        else s.kind = ShapeKind::Rect;
        if (lay && !lay->fill.empty()) s.fill = lay->fill;
        ReadSpPr(sppr, Child(sp, "style"), &s);
        if (s.kind == ShapeKind::Rect && s.fill.empty() && s.line.empty()) s.kind = ShapeKind::Text;
        // Text, with its inherited defaults: master text style, then the
        // master's placeholder, the layout's, and the shape's own lstStyle.
        LevelStyles levels = title ? title_style_ : is_ph && type != "body" && type != "subTitle" && type != "obj" ? other_style_ : body_style_;
        if (!is_ph) {
            levels = other_style_;
            for (auto &kv : levels) kv.second.bullet = 0;
        }
        if (type == "subTitle" || type == "ctrTitle")
            for (auto &kv : levels) kv.second.bullet = 0;
        if (mas) Overlay(&levels, mas->levels);
        if (lay) Overlay(&levels, lay->levels);
        xml::xml_node body = Child(sp, "txBody");
        Overlay(&levels, ReadLevels(Child(body, "lstStyle")));
        xml::xml_node bodypr = Child(body, "bodyPr");
        const std::string anchor = Attr(bodypr, "anchor");
        int va = anchor == "ctr" ? 1 : anchor == "b" ? 2 : anchor == "t" ? 0 : -1;
        if (va < 0 && lay) va = lay->anchor;
        if (va < 0 && mas) va = mas->anchor;
        s.valign = va == 1 ? VAlign::Middle : va == 2 ? VAlign::Bottom : VAlign::Top;
        if (va < 0 && (s.kind == ShapeKind::Rect || s.kind == ShapeKind::Ellipse || s.kind == ShapeKind::RoundRect)) s.valign = VAlign::Middle;
        s.autofit = static_cast<bool>(Child(bodypr, "normAutofit"));
        double font_scale = 1.0;
        if (xml::xml_node na = Child(bodypr, "normAutofit")) {
            const std::string fs = Attr(na, "fontScale");
            if (!fs.empty()) font_scale = std::atof(fs.c_str()) / 100000.0;
        }
        s.font_pt = levels.count(1) && levels[1].size_pt > 0 ? levels[1].size_pt : 18;
        if (levels.count(1) && !levels[1].color.empty()) s.text_color = levels[1].color;
        for (const xml::xml_node &p : Children(body, "p")) ReadParagraph(p, levels, ctx, font_scale, &s);
        // An empty placeholder is only a prompt ("Click to add title").
        if (is_ph && s.PlainText().find_first_not_of(" \n\t") == std::string::npos && s.fill.empty()) return;
        if (s.paras.empty() && s.HasText()) s.paras.push_back(Paragraph{});
        if (s.font_pt * font_scale > 0) s.font_pt *= font_scale;
        out->shapes.push_back(std::move(s));
    }

    void ReadParagraph(const xml::xml_node &p, LevelStyles &levels, const Context &ctx, double font_scale, Shape *s) const {
        Paragraph para;
        xml::xml_node ppr = Child(p, "pPr");
        para.level = std::max(0, std::atoi(Attr(ppr, "lvl").c_str()));
        const LevelStyle &ls = levels[para.level + 1];
        int align = AlignOf(Attr(ppr, "algn"));
        if (align < 0) align = ls.align;
        para.align = align < 0 ? Align::Left : static_cast<Align>(align);
        int bullet = ls.bullet;
        if (Child(ppr, "buNone")) bullet = 0;
        else if (Child(ppr, "buAutoNum")) bullet = 2;
        else if (Child(ppr, "buChar")) bullet = 1;
        para.bullet = bullet == 1;
        para.numbered = bullet == 2;
        auto run_from = [&](const xml::xml_node &rpr, const std::string &text) {
            TextRun r;
            r.text = text;
            r.bold = ls.has_bold && ls.bold;
            const std::string sz = Attr(rpr, "sz");
            if (!sz.empty()) r.size_pt = std::atof(sz.c_str()) / 100.0 * font_scale;
            else if (ls.size_pt > 0 && std::fabs(ls.size_pt - s->font_pt) > 0.01) r.size_pt = ls.size_pt * font_scale;
            if (HasAttr(rpr, "b")) r.bold = Attr(rpr, "b") == "1" || Attr(rpr, "b") == "true";
            r.italic = Attr(rpr, "i") == "1" || Attr(rpr, "i") == "true";
            const std::string u = Attr(rpr, "u");
            r.underline = !u.empty() && u != "none";
            const std::string st = Attr(rpr, "strike");
            r.strike = !st.empty() && st != "noStrike";
            const int base = std::atoi(Attr(rpr, "baseline").c_str());
            r.baseline = base > 0 ? 1 : base < 0 ? -1 : 0;
            for (const xml::xml_node &c : Elements(rpr)) {
                if (Is(c, "solidFill")) r.color = FillColor(c);
                else if (Is(c, "latin")) r.font = Font(Attr(c, "typeface"));
                else if (Is(c, "hlinkClick")) r.link = ctx.rels.Target(Attr(c, "id"));
            }
            if (r.color.empty() && !ls.color.empty() && ls.color != s->text_color) r.color = ls.color;
            // What only restates the shape's defaults is left to them.
            if (r.color == s->text_color) r.color.clear();
            if (std::fabs(r.size_pt - s->font_pt * font_scale) < 0.01) r.size_pt = 0;
            return r;
        };
        for (const xml::xml_node &c : Elements(p)) {
            if (Is(c, "r") || Is(c, "fld")) {
                TextRun r = run_from(Child(c, "rPr"), AllText(Child(c, "t")));
                if (!r.text.empty()) para.runs.push_back(r);
            } else if (Is(c, "br")) {
                // A line break inside a paragraph: a paragraph of its own here.
                Paragraph next = para;
                next.runs.clear();
                s->paras.push_back(std::move(para));
                para = std::move(next);
            } else if (std::strcmp(c.name(), "a14:m") == 0) {
                // An Office equation: a paragraph of display maths
                // (<m:oMathPara>) or inline maths (<m:oMath>).
                TextRun style = run_from(xml::xml_node{}, "");
                // Its size: the equation's own runs', else the text's before it.
                std::function<xml::xml_node(const xml::xml_node &)> find_rpr = [&](const xml::xml_node &n) -> xml::xml_node {
                    for (const xml::xml_node &k : Elements(n)) {
                        if (std::strcmp(k.name(), "a:rPr") == 0 && HasAttr(k, "sz")) return k;
                        if (xml::xml_node f = find_rpr(k)) return f;
                    }
                    return {};
                };
                if (xml::xml_node rpr = find_rpr(c)) {
                    style.size_pt = std::atof(Attr(rpr, "sz").c_str()) / 100.0 * font_scale;
                    if (std::fabs(style.size_pt - s->font_pt * font_scale) < 0.01) style.size_pt = 0;
                } else {
                    for (size_t k = para.runs.size(); k-- > 0;)
                        if (!para.runs[k].IsMath()) {
                            style.size_pt = para.runs[k].size_pt;
                            break;
                        }
                }
                for (const xml::xml_node &m : Elements(c)) {
                    const bool para_math = Is(m, "oMathPara");
                    std::vector<xml::xml_node> maths = para_math ? Children(m, "oMath") : std::vector<xml::xml_node>{m};
                    for (const xml::xml_node &om : maths) {
                        const std::string tex = officemath::OmmlToTex(om);
                        if (tex.empty()) continue;
                        TextRun r = MathRun(tex, para_math);
                        r.size_pt = style.size_pt;
                        r.color = style.color;
                        para.runs.push_back(r);
                    }
                }
            } else if (Is(c, "AlternateContent")) {
                // Inline maths (a14:m) and friends: the fallback's runs.
                xml::xml_node fb = Child(c, "Fallback");
                for (const xml::xml_node &r : Children(fb, "r")) {
                    TextRun tr = run_from(Child(r, "rPr"), AllText(Child(r, "t")));
                    if (!tr.text.empty()) para.runs.push_back(tr);
                }
            }
        }
        s->paras.push_back(std::move(para));
    }

    void ReadPic(const xml::xml_node &pic, const Context &ctx, const Transform &t, Slide *out) const {
        Shape s;
        s.kind = ShapeKind::Image;
        xml::xml_node cnv = Path(pic, "nvPicPr/cNvPr");
        s.name = Attr(cnv, "name");
        s.alt = Attr(cnv, "descr");
        long x, y, w, h;
        if (!ReadXfrm(Path(pic, "spPr/xfrm"), &x, &y, &w, &h)) return;
        Place(t, x, y, w, h, &s);
        const std::string part = ctx.rels.Target(Attr(Path(pic, "blipFill/blip"), "embed"));
        std::string bytes;
        if (part.empty() || !zip_.Get(part, &bytes)) return;
        s.image = std::make_shared<const std::string>(std::move(bytes));
        s.image_ext = ExtOf(part);
        out->shapes.push_back(std::move(s));
    }

    void ReadConnector(const xml::xml_node &cxn, const Transform &t, Slide *out) const {
        Shape s;
        s.kind = ShapeKind::Line;
        s.name = Attr(Path(cxn, "nvCxnSpPr/cNvPr"), "name");
        long x, y, w, h;
        xml::xml_node xfrm = Path(cxn, "spPr/xfrm");
        if (!ReadXfrm(xfrm, &x, &y, &w, &h)) return;
        Place(t, x, y, w, h, &s);
        // A flipped line runs from the other corner; the model's line runs
        // from (x, y) to (x + w, y + h), so a flip becomes a negative extent.
        if (Attr(xfrm, "flipH") == "1") {
            s.x += s.w;
            s.w = -s.w;
        }
        if (Attr(xfrm, "flipV") == "1") {
            s.y += s.h;
            s.h = -s.h;
        }
        s.line = "1F2328";
        s.line_pt = 1;
        ReadSpPr(Child(cxn, "spPr"), Child(cxn, "style"), &s);
        if (s.line.empty()) return;
        out->shapes.push_back(std::move(s));
    }

    void ReadGraphicFrame(const xml::xml_node &gf, const Transform &t, Slide *out) const {
        xml::xml_node tbl = Path(gf, "graphic/graphicData/tbl");
        if (!tbl) return;
        Shape s;
        s.kind = ShapeKind::Text;
        s.name = Attr(Path(gf, "nvGraphicFramePr/cNvPr"), "name");
        long x, y, w, h;
        if (!ReadXfrm(Child(gf, "xfrm"), &x, &y, &w, &h)) return;
        Place(t, x, y, w, h, &s);
        s.font_pt = 14;
        s.line = "BFC5CC";
        s.line_pt = 0.75;
        // Rows as lines of tab-separated cells, the header row bold.
        bool first = true;
        for (const xml::xml_node &tr : Children(tbl, "tr")) {
            Paragraph p;
            std::string text;
            for (const xml::xml_node &tc : Children(tr, "tc")) {
                std::string cell;
                for (const xml::xml_node &para : Children(Child(tc, "txBody"), "p")) {
                    if (!cell.empty()) cell += " ";
                    cell += AllText(para);
                }
                text += (text.empty() ? "" : "\t") + cell;
            }
            TextRun r;
            r.text = text;
            r.bold = first && Attr(Child(tbl, "tblPr"), "firstRow") == "1";
            p.runs.push_back(r);
            s.paras.push_back(p);
            first = false;
        }
        out->shapes.push_back(std::move(s));
    }
};

// --- Writing ------------------------------------------------------------------------

const char *kHead = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n";
const char *kNs =
    "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
    "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
    "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"";
const std::string kRel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
const char *kMathNs =
    "xmlns:mc=\"http://schemas.openxmlformats.org/markup-compatibility/2006\" "
    "xmlns:a14=\"http://schemas.microsoft.com/office/drawing/2010/main\" "
    "xmlns:m=\"http://schemas.openxmlformats.org/officeDocument/2006/math\"";
const char *kGroup =
    "<p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>"
    "<p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/><a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr>";

class Writer {
public:
    std::vector<std::pair<std::string, std::string>> rels;   // (type, target), rId2 on
    std::vector<std::pair<std::string, std::string>> media;  // (zip name, bytes)
    int *media_counter = nullptr;
    int next_id = 2;
    // Writing a shape's Choice (Office equations) rather than its Fallback.
    bool math_mode = false;

    std::string Rel(const std::string &type, const std::string &target) {
        rels.emplace_back(type, target);
        return "rId" + Num(static_cast<long>(rels.size()) + 1);
    }

    static std::string Xfrm(const Shape &s, const char *ns) {
        long x = s.x, y = s.y, w = s.w, h = s.h;
        std::string flip;
        if (w < 0) {
            x += w;
            w = -w;
            flip += " flipH=\"1\"";
        }
        if (h < 0) {
            y += h;
            h = -h;
            flip += " flipV=\"1\"";
        }
        return std::string("<") + ns + ":xfrm" + flip + "><a:off x=\"" + Num(x) + "\" y=\"" + Num(y) + "\"/><a:ext cx=\"" + Num(w) + "\" cy=\"" +
               Num(h) + "\"/></" + ns + ":xfrm>";
    }

    static std::string Fill(const std::string &c) {
        return c.empty() ? "<a:noFill/>" : "<a:solidFill><a:srgbClr val=\"" + c + "\"/></a:solidFill>";
    }
    static std::string Line(const Shape &s) {
        if (s.line.empty() || s.line_pt <= 0) return "<a:ln><a:noFill/></a:ln>";
        return "<a:ln w=\"" + Num(std::lround(s.line_pt * kEmuPerPt)) + "\">" + Fill(s.line) + "</a:ln>";
    }

    std::string Run(const TextRun &r, const Shape &s) {
        const double pt = r.size_pt > 0 ? r.size_pt : s.font_pt;
        std::string a = " lang=\"en-US\" sz=\"" + Num(std::lround(pt * 100)) + "\"";
        a += r.bold ? " b=\"1\"" : " b=\"0\"";
        a += r.italic ? " i=\"1\"" : " i=\"0\"";
        if (r.underline) a += " u=\"sng\"";
        if (r.strike) a += " strike=\"sngStrike\"";
        if (r.baseline) a += r.baseline > 0 ? " baseline=\"30000\"" : " baseline=\"-25000\"";
        const std::string color = !r.color.empty() ? r.color : s.text_color;
        std::string inner = color.empty() ? std::string() : Fill(color);
        const std::string font = r.font.empty() ? std::string("+mn-lt") : r.font;
        inner += "<a:latin typeface=\"" + Esc(font) + "\"/><a:cs typeface=\"" + Esc(font) + "\"/>";
        if (!r.link.empty()) inner += "<a:hlinkClick r:id=\"" + Rel("hyperlink", r.link) + "\"/>";
        return "<a:r><a:rPr" + a + ">" + inner + "</a:rPr><a:t>" + Esc(r.text) + "</a:t></a:r>";
    }

    std::string Para(const Paragraph &p, const Shape &s) {
        std::string ppr;
        switch (p.align) {
            case Align::Left: ppr = " algn=\"l\""; break;
            case Align::Center: ppr = " algn=\"ctr\""; break;
            case Align::Right: ppr = " algn=\"r\""; break;
            case Align::Justify: ppr = " algn=\"just\""; break;
        }
        const long indent = 342900;
        if (p.level > 0) ppr += " lvl=\"" + Num(p.level) + "\"";
        if (p.bullet || p.numbered) ppr += " marL=\"" + Num(indent * (p.level + 1)) + "\" indent=\"-" + Num(indent) + "\"";
        else if (p.level > 0) ppr += " marL=\"" + Num(indent * p.level) + "\" indent=\"0\"";
        std::string inner;
        if (p.bullet) inner = "<a:buFont typeface=\"Arial\"/><a:buChar char=\"" + std::string(p.level % 2 ? "–" : "•") + "\"/>";
        else if (p.numbered) inner = "<a:buAutoNum type=\"arabicPeriod\"/>";
        else inner = "<a:buNone/>";
        std::string o = "<a:p><a:pPr" + ppr + ">" + inner + "</a:pPr>";
        for (const TextRun &r : p.runs) {
            if (r.IsMath()) {
                o += MathXml(r, s);
                continue;
            }
            if (!r.text.empty()) o += Run(r, s);
        }
        return o + "<a:endParaRPr lang=\"en-US\" sz=\"" + Num(std::lround(s.font_pt * 100)) + "\"/></a:p>";
    }

    // Maths: an Office equation in the Choice, and the text it reads as
    // (typeset runs, math_markup.h) in the Fallback.
    std::string MathXml(const TextRun &r, const Shape &s) {
        const double pt = r.size_pt > 0 ? r.size_pt : s.font_pt;
        if (math_mode) {
            const std::string rpr = "<a:rPr lang=\"en-US\" sz=\"" + Num(std::lround(pt * 100)) + "\"><a:latin typeface=\"Cambria Math\"/><a:cs typeface=\"Cambria Math\"/></a:rPr>";
            const std::string omml = TexToOmml(r.tex, rpr);
            if (r.display) return "<a14:m><m:oMathPara><m:oMathParaPr><m:jc m:val=\"centerGroup\"/></m:oMathParaPr>" + omml + "</m:oMathPara></a14:m>";
            return "<a14:m>" + omml + "</a14:m>";
        }
        std::string o;
        for (const MathTextRun &m : TexToTextRuns(r.tex)) {
            TextRun t = r;
            t.tex.clear();
            t.text = m.text;
            t.italic = m.italic;
            t.bold = r.bold || m.bold;
            t.baseline = m.script;
            t.font = "Times New Roman";
            o += Run(t, s);
        }
        return o;
    }

    static bool HasMath(const Shape &s) {
        for (const Paragraph &p : s.paras)
            for (const TextRun &r : p.runs)
                if (r.IsMath()) return true;
        return false;
    }

    std::string TextBody(const Shape &s, const char *tag) {
        const char *anchor = s.valign == VAlign::Middle ? "ctr" : s.valign == VAlign::Bottom ? "b" : "t";
        // The shape's own text defaults, where a reader looks for them first.
        std::string lst;
        for (int lvl = 1; lvl <= 9; ++lvl)
            lst += "<a:lvl" + Num(lvl) + "pPr><a:defRPr sz=\"" + Num(std::lround(s.font_pt * 100)) + "\">" + (s.text_color.empty() ? "" : Fill(s.text_color)) +
                   "</a:defRPr></a:lvl" + Num(lvl) + "pPr>";
        std::string o = std::string("<") + tag + "><a:bodyPr wrap=\"square\" lIns=\"91440\" tIns=\"45720\" rIns=\"91440\" bIns=\"45720\" anchor=\"" +
                        anchor + "\">" + (s.autofit ? "<a:normAutofit/>" : "<a:noAutofit/>") + "</a:bodyPr><a:lstStyle>" + lst + "</a:lstStyle>";
        if (s.paras.empty()) o += "<a:p><a:endParaRPr lang=\"en-US\"/></a:p>";
        for (const Paragraph &p : s.paras) o += Para(p, s);
        return o + "</" + tag + ">";
    }

    // A shape with maths is written twice (markup compatibility): with
    // Office equations for the readers that know them, and with its maths
    // as text for the rest.
    std::string ShapeXml(const Shape &s) {
        if (!s.HasText() || !HasMath(s)) return ShapeXmlOnce(s);
        const int id = next_id;
        math_mode = true;
        const std::string with = ShapeXmlOnce(s);
        math_mode = false;
        next_id = id;
        // An equation's fallback is a picture of it when there is a
        // renderer, centred in the shape.
        std::string fallback;
        std::string png;
        double w_pt = 0, h_pt = 0;
        if (IsEquationShape(s) && MathPicture()) {
            const TextRun &r = s.paras[0].runs[0];
            if (MathPicture()(r.tex, r.size_pt > 0 ? r.size_pt : s.font_pt, &png, &w_pt, &h_pt) && !png.empty() && w_pt > 0) {
                Shape pic;
                pic.kind = ShapeKind::Image;
                pic.name = s.name;
                pic.alt = r.tex;
                pic.image = std::make_shared<const std::string>(std::move(png));
                pic.image_ext = "png";
                pic.w = std::min(std::labs(s.w), std::lround(w_pt * kEmuPerPt));
                pic.h = std::lround(h_pt * kEmuPerPt * static_cast<double>(pic.w) / (w_pt * kEmuPerPt));
                pic.x = std::min(s.x, s.x + s.w) + (std::labs(s.w) - pic.w) / 2;
                pic.y = std::min(s.y, s.y + s.h) + std::max(0L, (std::labs(s.h) - pic.h) / 2);
                fallback = ShapeXmlOnce(pic);
            }
        }
        if (fallback.empty()) fallback = ShapeXmlOnce(s);
        return "<mc:AlternateContent><mc:Choice Requires=\"a14\">" + with + "</mc:Choice><mc:Fallback>" + fallback +
               "</mc:Fallback></mc:AlternateContent>";
    }

    std::string ShapeXmlOnce(const Shape &s) {
        const int id = next_id++;
        const std::string name = s.name.empty() ? "Shape " + Num(id) : s.name;
        if (s.kind == ShapeKind::Image) {
            if (!s.image) return "";
            const std::string ext = s.image_ext.empty() ? "png" : s.image_ext;
            const std::string file = "image" + Num(++*media_counter) + "." + ext;
            media.emplace_back("ppt/media/" + file, *s.image);
            const std::string rid = Rel("image", "../media/" + file);
            return "<p:pic><p:nvPicPr><p:cNvPr id=\"" + Num(id) + "\" name=\"" + Esc(name) + "\" descr=\"" + Esc(s.alt) +
                   "\"/><p:cNvPicPr><a:picLocks noChangeAspect=\"1\"/></p:cNvPicPr><p:nvPr/></p:nvPicPr><p:blipFill><a:blip r:embed=\"" + rid +
                   "\"/><a:stretch><a:fillRect/></a:stretch></p:blipFill><p:spPr>" + Xfrm(s, "a") +
                   "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr></p:pic>";
        }
        if (s.kind == ShapeKind::Line) {
            return "<p:cxnSp><p:nvCxnSpPr><p:cNvPr id=\"" + Num(id) + "\" name=\"" + Esc(name) + "\"/><p:cNvCxnSpPr/><p:nvPr/></p:nvCxnSpPr><p:spPr>" +
                   Xfrm(s, "a") + "<a:prstGeom prst=\"line\"><a:avLst/></a:prstGeom>" + Line(s) + "</p:spPr></p:cxnSp>";
        }
        const char *prst = s.kind == ShapeKind::Ellipse ? "ellipse" : s.kind == ShapeKind::RoundRect ? "roundRect" : "rect";
        std::string nv = "<p:nvSpPr><p:cNvPr id=\"" + Num(id) + "\" name=\"" + Esc(name) + "\"/><p:cNvSpPr" +
                         (s.kind == ShapeKind::Text ? " txBox=\"1\"" : "") + "/><p:nvPr>" + (s.is_title ? "<p:ph type=\"title\"/>" : "") +
                         "</p:nvPr></p:nvSpPr>";
        return "<p:sp>" + nv + "<p:spPr>" + Xfrm(s, "a") + "<a:prstGeom prst=\"" + prst + "\"><a:avLst/></a:prstGeom>" + Fill(s.fill) + Line(s) +
               "</p:spPr>" + TextBody(s, "p:txBody") + "</p:sp>";
    }

    std::string SlideXml(const Slide &sl) {
        std::string shapes;
        for (const Shape &s : sl.shapes) shapes += ShapeXml(s);
        std::string bg;
        if (!sl.background.empty())
            bg = "<p:bg><p:bgPr>" + Fill(sl.background) + "<a:effectLst/></p:bgPr></p:bg>";
        return std::string(kHead) + "<p:sld " + kNs + " " + kMathNs + "><p:cSld>" + bg + "<p:spTree>" + kGroup + shapes +
               "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sld>";
    }

    std::string RelsXml() const {
        std::string o = std::string(kHead) +
                        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                        "<Relationship Id=\"rId1\" Type=\"" + kRel + "slideLayout\" Target=\"../slideLayouts/slideLayout1.xml\"/>";
        for (size_t k = 0; k < rels.size(); ++k)
            o += "<Relationship Id=\"rId" + Num(static_cast<long>(k) + 2) + "\" Type=\"" + kRel + rels[k].first + "\" Target=\"" + Esc(rels[k].second) +
                 "\"" + (rels[k].first == "hyperlink" ? " TargetMode=\"External\"" : "") + "/>";
        return o + "</Relationships>";
    }
};

std::string Theme() {
    auto clr = [](const char *name, const char *rgb) { return std::string("<a:") + name + "><a:srgbClr val=\"" + rgb + "\"/></a:" + name + ">"; };
    std::string fills, lines, effects;
    for (int k = 0; k < 3; ++k) {
        fills += "<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>";
        lines += "<a:ln w=\"6350\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:ln>";
        effects += "<a:effectStyle><a:effectLst/></a:effectStyle>";
    }
    const std::string font = "<a:latin typeface=\"Arial\"/><a:ea typeface=\"\"/><a:cs typeface=\"\"/>";
    return std::string(kHead) +
           "<a:theme xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" name=\"mep\"><a:themeElements><a:clrScheme name=\"mep\">" +
           clr("dk1", "000000") + clr("lt1", "FFFFFF") + clr("dk2", "1F2328") + clr("lt2", "F6F8FA") + clr("accent1", "6B8AFD") +
           clr("accent2", "E5A50A") + clr("accent3", "2F7D32") + clr("accent4", "C678DD") + clr("accent5", "0B7285") + clr("accent6", "C62828") +
           clr("hlink", "0B5CAD") + clr("folHlink", "8250DF") + "</a:clrScheme><a:fontScheme name=\"mep\"><a:majorFont>" + font +
           "</a:majorFont><a:minorFont>" + font + "</a:minorFont></a:fontScheme><a:fmtScheme name=\"mep\"><a:fillStyleLst>" + fills +
           "</a:fillStyleLst><a:lnStyleLst>" + lines + "</a:lnStyleLst><a:effectStyleLst>" + effects + "</a:effectStyleLst><a:bgFillStyleLst>" +
           fills + "</a:bgFillStyleLst></a:fmtScheme></a:themeElements></a:theme>";
}

}  // namespace

void SetMathPictureRenderer(MathPictureFn fn) { MathPicture() = std::move(fn); }

bool LoadPptx(const std::string &bytes, Presentation *out, std::string *error) {
    Zip z{bytes};
    Presentation p;
    Reader r(z);
    if (!r.Read(&p, error)) return false;
    *out = std::move(p);
    return true;
}

std::string SavePptx(const Presentation &p) {
    std::vector<zip::EntryToWrite> slides;
    int media = 0;
    std::string ids, pres_rels, overrides, exts;
    std::map<std::string, bool> seen_ext;
    for (size_t k = 0; k < p.slides.size(); ++k) {
        Writer w;
        w.media_counter = &media;
        const std::string n = Num(static_cast<long>(k) + 1);
        slides.push_back({"ppt/slides/slide" + n + ".xml", w.SlideXml(p.slides[k])});
        slides.push_back({"ppt/slides/_rels/slide" + n + ".xml.rels", w.RelsXml()});
        for (auto &m : w.media) {
            const std::string ext = ExtOf(m.first);
            if (!seen_ext[ext]) {
                seen_ext[ext] = true;
                exts += "<Default Extension=\"" + ext + "\" ContentType=\"" + MimeForExt(ext) + "\"/>";
            }
            slides.push_back({m.first, m.second, true});
        }
        ids += "<p:sldId id=\"" + Num(256 + static_cast<long>(k)) + "\" r:id=\"rId" + Num(static_cast<long>(k) + 10) + "\"/>";
        pres_rels += "<Relationship Id=\"rId" + Num(static_cast<long>(k) + 10) + "\" Type=\"" + kRel + "slide\" Target=\"slides/slide" + n + ".xml\"/>";
        overrides += "<Override PartName=\"/ppt/slides/slide" + n + ".xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slide+xml\"/>";
    }
    const std::string ct = "application/vnd.openxmlformats-officedocument.presentationml.";
    const std::string rels_head = std::string(kHead) + "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
    std::vector<zip::EntryToWrite> e = {
        {"[Content_Types].xml",
         std::string(kHead) + "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
             "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
             "<Default Extension=\"xml\" ContentType=\"application/xml\"/>" + exts +
             "<Override PartName=\"/ppt/presentation.xml\" ContentType=\"" + ct + "presentation.main+xml\"/>"
             "<Override PartName=\"/ppt/slideMasters/slideMaster1.xml\" ContentType=\"" + ct + "slideMaster+xml\"/>"
             "<Override PartName=\"/ppt/slideLayouts/slideLayout1.xml\" ContentType=\"" + ct + "slideLayout+xml\"/>"
             "<Override PartName=\"/ppt/presProps.xml\" ContentType=\"" + ct + "presProps+xml\"/>"
             "<Override PartName=\"/ppt/viewProps.xml\" ContentType=\"" + ct + "viewProps+xml\"/>"
             "<Override PartName=\"/ppt/tableStyles.xml\" ContentType=\"" + ct + "tableStyles+xml\"/>"
             "<Override PartName=\"/ppt/theme/theme1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.theme+xml\"/>"
             "<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/>"
             "<Override PartName=\"/docProps/app.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.extended-properties+xml\"/>" +
             overrides + "</Types>"},
        {"_rels/.rels", rels_head + "<Relationship Id=\"rId1\" Type=\"" + kRel + "officeDocument\" Target=\"ppt/presentation.xml\"/>"
                            "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" Target=\"docProps/core.xml\"/>"
                            "<Relationship Id=\"rId3\" Type=\"" + kRel + "extended-properties\" Target=\"docProps/app.xml\"/></Relationships>"},
        {"docProps/core.xml", std::string(kHead) +
                                  "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
                                  "xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>" + Esc(p.title) + "</dc:title><dc:creator>" + Esc(p.author) +
                                  "</dc:creator></cp:coreProperties>"},
        {"docProps/app.xml", std::string(kHead) +
                                 "<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/extended-properties\"><Application>mep</Application><Slides>" +
                                 Num(static_cast<long>(p.slides.size())) + "</Slides></Properties>"},
        {"ppt/presentation.xml", std::string(kHead) + "<p:presentation " + kNs + " saveSubsetFonts=\"1\"><p:sldMasterIdLst><p:sldMasterId id=\"2147483648\" r:id=\"rId1\"/></p:sldMasterIdLst><p:sldIdLst>" +
                                     ids + "</p:sldIdLst><p:sldSz cx=\"" + Num(p.width) + "\" cy=\"" + Num(p.height) +
                                     "\"/><p:notesSz cx=\"6858000\" cy=\"9144000\"/></p:presentation>"},
        {"ppt/_rels/presentation.xml.rels",
         rels_head + "<Relationship Id=\"rId1\" Type=\"" + kRel + "slideMaster\" Target=\"slideMasters/slideMaster1.xml\"/>"
             "<Relationship Id=\"rId2\" Type=\"" + kRel + "theme\" Target=\"theme/theme1.xml\"/>"
             "<Relationship Id=\"rId3\" Type=\"" + kRel + "presProps\" Target=\"presProps.xml\"/>"
             "<Relationship Id=\"rId4\" Type=\"" + kRel + "viewProps\" Target=\"viewProps.xml\"/>"
             "<Relationship Id=\"rId5\" Type=\"" + kRel + "tableStyles\" Target=\"tableStyles.xml\"/>" + pres_rels + "</Relationships>"},
        {"ppt/presProps.xml", std::string(kHead) + "<p:presentationPr " + kNs + "/>"},
        {"ppt/viewProps.xml", std::string(kHead) + "<p:viewPr " + kNs + "/>"},
        {"ppt/tableStyles.xml", std::string(kHead) + "<a:tblStyleLst xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" def=\"{5C22544A-7EE6-4342-B048-85BDC9FD1C3A}\"/>"},
        {"ppt/slideMasters/slideMaster1.xml",
         std::string(kHead) + "<p:sldMaster " + kNs + "><p:cSld><p:bg><p:bgPr><a:solidFill><a:srgbClr val=\"FFFFFF\"/></a:solidFill><a:effectLst/></p:bgPr></p:bg><p:spTree>" +
             kGroup + "</p:spTree></p:cSld><p:clrMap bg1=\"lt1\" tx1=\"dk1\" bg2=\"lt2\" tx2=\"dk2\" accent1=\"accent1\" accent2=\"accent2\" "
                      "accent3=\"accent3\" accent4=\"accent4\" accent5=\"accent5\" accent6=\"accent6\" hlink=\"hlink\" folHlink=\"folHlink\"/>"
                      "<p:sldLayoutIdLst><p:sldLayoutId id=\"2147483649\" r:id=\"rId1\"/></p:sldLayoutIdLst>"
                      "<p:txStyles><p:titleStyle><a:lvl1pPr><a:defRPr sz=\"3600\"/></a:lvl1pPr></p:titleStyle>"
                      "<p:bodyStyle><a:lvl1pPr><a:defRPr sz=\"2400\"/></a:lvl1pPr></p:bodyStyle>"
                      "<p:otherStyle><a:lvl1pPr><a:defRPr sz=\"1800\"/></a:lvl1pPr></p:otherStyle></p:txStyles></p:sldMaster>"},
        {"ppt/slideMasters/_rels/slideMaster1.xml.rels",
         rels_head + "<Relationship Id=\"rId1\" Type=\"" + kRel + "slideLayout\" Target=\"../slideLayouts/slideLayout1.xml\"/>"
             "<Relationship Id=\"rId2\" Type=\"" + kRel + "theme\" Target=\"../theme/theme1.xml\"/></Relationships>"},
        {"ppt/slideLayouts/slideLayout1.xml", std::string(kHead) + "<p:sldLayout " + kNs + " type=\"blank\" preserve=\"1\"><p:cSld name=\"Blank\"><p:spTree>" + kGroup +
                                                  "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sldLayout>"},
        {"ppt/slideLayouts/_rels/slideLayout1.xml.rels",
         rels_head + "<Relationship Id=\"rId1\" Type=\"" + kRel + "slideMaster\" Target=\"../slideMasters/slideMaster1.xml\"/></Relationships>"},
        {"ppt/theme/theme1.xml", Theme()},
    };
    e.insert(e.end(), slides.begin(), slides.end());
    return zip::BuildArchive(e);
}

}  // namespace pres
