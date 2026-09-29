// The presentation editor's model side: a PresSession (editor.h) per open
// .pptx/.odp, its keyboard handling in PresNormal and PresInsert, and the
// edits the toolbar, the mouse (main.cpp's DrawPresPane) and Lua make.
//
// PresNormal works on objects:
//   j/k, PageDown/PageUp  next / previous slide       gg / G  first / last slide
//   Tab / Shift-Tab       select the next / previous shape; Esc deselects
//   arrows                nudge the selection (Shift: further); with nothing
//                         selected, previous / next slide
//   Enter, i, a           type into the selection     x, Delete  delete it
//   t r o O L             add a text box / rectangle / ellipse / rounded
//                         rectangle / line
//   n N X                 new slide / duplicate slide / delete slide
//   J K                   move the slide later / earlier
//   y p D                 copy / paste / duplicate the selected shape
//   ] [                   bring forward / send backward
//   + -                   larger / smaller text       b  bullets on / off
//   Ctrl-B/I/U            bold / italic / underline   u, Ctrl-Z  undo
//   Ctrl-Y, Ctrl-Shift-Z  redo                        P  present (Esc ends)
//   Ctrl-R                theme colours on / off (in every mode, as in the
//                         PDF and image viewers)
// PresInsert types into the selected shape: arrows, Home/End, Enter (a new
// paragraph), Backspace/Delete, Tab/Shift-Tab (list level), Ctrl-B/I/U on
// the caret's paragraph, Esc back to PresNormal.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "editor.h"
#include "gfx/input.h"
#include "image_doc.h"

namespace {

constexpr size_t kMaxPresUndo = 200;

// The UTF-8 character boundaries around a byte offset.
int PrevUtf8(const std::string &s, int off) {
    if (off <= 0) return 0;
    int i = off - 1;
    while (i > 0 && (static_cast<unsigned char>(s[static_cast<size_t>(i)]) & 0xC0) == 0x80) --i;
    return i;
}
int NextUtf8(const std::string &s, int off) {
    const int n = static_cast<int>(s.size());
    if (off >= n) return n;
    int i = off + 1;
    while (i < n && (static_cast<unsigned char>(s[static_cast<size_t>(i)]) & 0xC0) == 0x80) ++i;
    return i;
}

std::string Utf8(int cp) {
    std::string out;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return out;
}

bool ShiftDown() { return gfx::IsKeyDown(gfx::Key::LeftShift) || gfx::IsKeyDown(gfx::Key::RightShift); }
bool CtrlDown() { return gfx::IsKeyDown(gfx::Key::LeftControl) || gfx::IsKeyDown(gfx::Key::RightControl); }
bool Held(gfx::Key k) { return gfx::IsKeyPressed(k) || gfx::IsKeyPressedRepeat(k); }

}  // namespace

// --- Sessions -----------------------------------------------------------------------

bool Editor::IsPresBuffer(int buffer_id) const { return presdocs_.find(buffer_id) != presdocs_.end(); }

const PresSession *Editor::GetPres(int buffer_id) const {
    auto it = presdocs_.find(buffer_id);
    return it == presdocs_.end() ? nullptr : &it->second;
}

PresSession *Editor::GetPresMutable(int buffer_id) {
    auto it = presdocs_.find(buffer_id);
    return it == presdocs_.end() ? nullptr : &it->second;
}

int Editor::NewPresentationBuffer(const std::string &path) {
    const int buffer_id = CreateEmptyBuffer();
    buffers_[static_cast<size_t>(buffer_id)].filename = path;
    PresSession sess;
    sess.buffer_id = buffer_id;
    sess.doc = pres::NewPresentation();
    presdocs_[buffer_id] = std::move(sess);
    CurPane().buffer_id = buffer_id;
    CurPane().cursor = {0, 0};
    CurPane().scroll_row = 0;
    mode_ = Mode::PresNormal;
    status_message_ = path.empty() ? "new presentation (:w name.pptx or name.odp to save)" : "new presentation " + path;
    return buffer_id;
}

void Editor::OpenPresInPlace(const std::string &path) {
    int buffer_id = -1;
    for (size_t i = 0; i < buffers_.size(); i++) {
        if (!BufferInActiveWorkspace(static_cast<int>(i))) continue;
        if (!buffers_[i].deleted && !buffers_[i].filename.empty() && buffers_[i].filename == path && IsPresBuffer(static_cast<int>(i))) {
            buffer_id = static_cast<int>(i);
            break;
        }
    }
    if (buffer_id < 0) {
        PresSession sess;
        std::ifstream probe(path, std::ios::binary);
        if (!probe) {
            // A deck that does not exist yet: a new one, saved there by :w.
            sess.doc = pres::NewPresentation();
            status_message_ = "\"" + path + "\" [New presentation]";
        } else {
            std::string err;
            if (!pres::Load(path, &sess.doc, &err)) {
                status_message_ = "E: \"" + path + "\": " + err;
                return;
            }
            status_message_ = "\"" + path + "\" " + std::to_string(sess.doc.slides.size()) + " slide" + (sess.doc.slides.size() == 1 ? "" : "s");
        }
        buffer_id = CreateEmptyBuffer();
        buffers_[static_cast<size_t>(buffer_id)].filename = path;
        sess.buffer_id = buffer_id;
        presdocs_[buffer_id] = std::move(sess);
    }
    CurPane().buffer_id = buffer_id;
    CurPane().cursor = {0, 0};
    CurPane().scroll_row = 0;
    pending_g_ = false;
}

pres::Slide *Editor::PresCurrentSlide(PresSession &sess) {
    if (sess.doc.slides.empty()) sess.doc.slides.push_back(pres::Slide{});
    sess.slide = std::clamp(sess.slide, 0, static_cast<int>(sess.doc.slides.size()) - 1);
    return &sess.doc.slides[static_cast<size_t>(sess.slide)];
}

pres::Shape *Editor::PresSelectedShape(PresSession &sess) {
    pres::Slide *s = PresCurrentSlide(sess);
    if (sess.selected < 0 || sess.selected >= static_cast<int>(s->shapes.size())) {
        sess.selected = -1;
        return nullptr;
    }
    return &s->shapes[static_cast<size_t>(sess.selected)];
}

void Editor::PresPushUndo(PresSession &sess) {
    sess.undo_stack.push_back(sess.doc);
    if (sess.undo_stack.size() > kMaxPresUndo) sess.undo_stack.erase(sess.undo_stack.begin());
    sess.redo_stack.clear();
}

void Editor::PresMarkModified(PresSession &sess) {
    sess.modified = true;
    if (sess.buffer_id >= 0 && sess.buffer_id < static_cast<int>(buffers_.size())) buffers_[static_cast<size_t>(sess.buffer_id)].modified = true;
}

void Editor::PresUndo(PresSession &sess) {
    if (sess.undo_stack.empty()) {
        status_message_ = "Already at oldest change";
        return;
    }
    sess.redo_stack.push_back(std::move(sess.doc));
    sess.doc = std::move(sess.undo_stack.back());
    sess.undo_stack.pop_back();
    PresSelectedShape(sess);
    PresMarkModified(sess);
}

void Editor::PresRedo(PresSession &sess) {
    if (sess.redo_stack.empty()) {
        status_message_ = "Already at newest change";
        return;
    }
    sess.undo_stack.push_back(std::move(sess.doc));
    sess.doc = std::move(sess.redo_stack.back());
    sess.redo_stack.pop_back();
    PresSelectedShape(sess);
    PresMarkModified(sess);
}

void Editor::PresGotoSlide(PresSession &sess, int slide) {
    if (mode_ == Mode::PresInsert) mode_ = Mode::PresNormal;
    sess.slide = std::clamp(slide, 0, std::max(0, static_cast<int>(sess.doc.slides.size()) - 1));
    sess.selected = -1;
    sess.drag = PresSession::Drag::None;
}

void Editor::PresSelect(PresSession &sess, int shape) {
    if (mode_ == Mode::PresInsert && shape != sess.selected) mode_ = Mode::PresNormal;
    sess.selected = shape;
    PresSelectedShape(sess);
}

int Editor::PresShapeAt(const PresSession &sess, double x, double y) const {
    if (sess.slide < 0 || sess.slide >= static_cast<int>(sess.doc.slides.size())) return -1;
    const pres::Slide &s = sess.doc.slides[static_cast<size_t>(sess.slide)];
    const double slop = static_cast<double>(sess.doc.width) / 200.0;  // lines are thin
    for (size_t i = s.shapes.size(); i-- > 0;) {
        const pres::Shape &sh = s.shapes[i];
        double x0 = static_cast<double>(std::min(sh.x, sh.x + sh.w)), x1 = static_cast<double>(std::max(sh.x, sh.x + sh.w));
        double y0 = static_cast<double>(std::min(sh.y, sh.y + sh.h)), y1 = static_cast<double>(std::max(sh.y, sh.y + sh.h));
        if (sh.kind == pres::ShapeKind::Line) {
            // Within reach of the segment.
            const double dx = static_cast<double>(sh.w), dy = static_cast<double>(sh.h);
            const double len2 = dx * dx + dy * dy;
            double t = len2 > 0 ? ((x - static_cast<double>(sh.x)) * dx + (y - static_cast<double>(sh.y)) * dy) / len2 : 0;
            t = std::clamp(t, 0.0, 1.0);
            const double px = static_cast<double>(sh.x) + t * dx - x, py = static_cast<double>(sh.y) + t * dy - y;
            if (px * px + py * py <= slop * slop * 4) return static_cast<int>(i);
            continue;
        }
        if (x >= x0 - slop && x <= x1 + slop && y >= y0 - slop && y <= y1 + slop) return static_cast<int>(i);
    }
    return -1;
}

// --- Slides and shapes ----------------------------------------------------------------

void Editor::PresAddSlide(PresSession &sess, bool duplicate) {
    PresPushUndo(sess);
    PresCurrentSlide(sess);
    pres::Slide s = duplicate ? sess.doc.slides[static_cast<size_t>(sess.slide)] : pres::NewSlide(sess.doc);
    sess.doc.slides.insert(sess.doc.slides.begin() + sess.slide + 1, std::move(s));
    PresGotoSlide(sess, sess.slide + 1);
    PresMarkModified(sess);
    status_message_ = "slide " + std::to_string(sess.slide + 1) + " of " + std::to_string(sess.doc.slides.size());
}

void Editor::PresDeleteSlide(PresSession &sess) {
    PresPushUndo(sess);
    PresCurrentSlide(sess);
    sess.doc.slides.erase(sess.doc.slides.begin() + sess.slide);
    if (sess.doc.slides.empty()) sess.doc.slides.push_back(pres::NewSlide(sess.doc));
    PresGotoSlide(sess, std::min(sess.slide, static_cast<int>(sess.doc.slides.size()) - 1));
    PresMarkModified(sess);
}

void Editor::PresMoveSlide(PresSession &sess, int delta) {
    const int to = sess.slide + delta;
    if (to < 0 || to >= static_cast<int>(sess.doc.slides.size())) return;
    PresPushUndo(sess);
    std::swap(sess.doc.slides[static_cast<size_t>(sess.slide)], sess.doc.slides[static_cast<size_t>(to)]);
    PresGotoSlide(sess, to);
    PresMarkModified(sess);
}

void Editor::PresAddShape(PresSession &sess, pres::ShapeKind kind) {
    PresPushUndo(sess);
    pres::Slide *s = PresCurrentSlide(sess);
    pres::Shape sh = kind == pres::ShapeKind::Text ? pres::NewTextBox(sess.doc, "") : pres::NewShape(sess.doc, kind);
    // Step away from a shape just added in the same place.
    for (int guard = 0; guard < 20; ++guard) {
        bool clash = false;
        for (const pres::Shape &o : s->shapes) clash = clash || (o.x == sh.x && o.y == sh.y);
        if (!clash) break;
        sh.x += sess.doc.width / 50;
        sh.y += sess.doc.height / 50;
    }
    s->shapes.push_back(std::move(sh));
    sess.selected = static_cast<int>(s->shapes.size()) - 1;
    PresMarkModified(sess);
    // A new text box is for typing into.
    if (kind == pres::ShapeKind::Text) {
        sess.caret_para = 0;
        sess.caret_off = 0;
        mode_ = Mode::PresInsert;
    }
}

bool Editor::PresInsertImage(PresSession &sess, const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string bytes = ss.str();
    ImageDoc img;
    if (bytes.empty() || !img.LoadFromMemory(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size()) || img.Width() <= 0 ||
        img.Height() <= 0)
        return false;
    PresPushUndo(sess);
    pres::Shape sh;
    sh.kind = pres::ShapeKind::Image;
    sh.name = "Picture";
    // As large as fits in 60% of the slide, its aspect kept.
    const double k = std::min(static_cast<double>(sess.doc.width) * 0.6 / img.Width(), static_cast<double>(sess.doc.height) * 0.6 / img.Height());
    sh.w = std::lround(img.Width() * k);
    sh.h = std::lround(img.Height() * k);
    sh.x = (sess.doc.width - sh.w) / 2;
    sh.y = (sess.doc.height - sh.h) / 2;
    std::string ext = path.substr(path.find_last_of('.') + 1);
    for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    sh.image_ext = ext == "jpg" ? "jpeg" : ext;
    const size_t slash = path.find_last_of('/');
    sh.alt = slash == std::string::npos ? path : path.substr(slash + 1);
    sh.image = std::make_shared<const std::string>(std::move(bytes));
    pres::Slide *s = PresCurrentSlide(sess);
    s->shapes.push_back(std::move(sh));
    sess.selected = static_cast<int>(s->shapes.size()) - 1;
    PresMarkModified(sess);
    return true;
}

void Editor::PresDeleteShape(PresSession &sess) {
    if (!PresSelectedShape(sess)) return;
    PresPushUndo(sess);
    pres::Slide *s = PresCurrentSlide(sess);
    s->shapes.erase(s->shapes.begin() + sess.selected);
    sess.selected = -1;
    if (mode_ == Mode::PresInsert) mode_ = Mode::PresNormal;
    PresMarkModified(sess);
}

void Editor::PresDuplicateShape(PresSession &sess) {
    pres::Shape *sh = PresSelectedShape(sess);
    if (!sh) return;
    PresPushUndo(sess);
    pres::Shape copy = *sh;
    copy.x += sess.doc.width / 50;
    copy.y += sess.doc.height / 50;
    copy.is_title = false;
    pres::Slide *s = PresCurrentSlide(sess);
    s->shapes.push_back(std::move(copy));
    sess.selected = static_cast<int>(s->shapes.size()) - 1;
    PresMarkModified(sess);
}

void Editor::PresCopyShape(PresSession &sess) {
    pres::Shape *sh = PresSelectedShape(sess);
    if (!sh) return;
    sess.clipboard = *sh;
    sess.has_clipboard = true;
    status_message_ = "copied " + (sh->name.empty() ? std::string("shape") : sh->name);
}

void Editor::PresPasteShape(PresSession &sess) {
    if (!sess.has_clipboard) return;
    PresPushUndo(sess);
    pres::Shape copy = sess.clipboard;
    copy.is_title = false;
    pres::Slide *s = PresCurrentSlide(sess);
    for (const pres::Shape &o : s->shapes)
        if (o.x == copy.x && o.y == copy.y) {
            copy.x += sess.doc.width / 50;
            copy.y += sess.doc.height / 50;
        }
    s->shapes.push_back(std::move(copy));
    sess.selected = static_cast<int>(s->shapes.size()) - 1;
    PresMarkModified(sess);
}

void Editor::PresRestack(PresSession &sess, int delta) {
    if (!PresSelectedShape(sess)) return;
    pres::Slide *s = PresCurrentSlide(sess);
    const int to = std::clamp(sess.selected + delta, 0, static_cast<int>(s->shapes.size()) - 1);
    if (to == sess.selected) return;
    PresPushUndo(sess);
    std::swap(s->shapes[static_cast<size_t>(sess.selected)], s->shapes[static_cast<size_t>(to)]);
    sess.selected = to;
    PresMarkModified(sess);
}

// --- Formatting -----------------------------------------------------------------------

namespace {

// The paragraphs formatting applies to: the caret's while typing, else all.
std::vector<pres::Paragraph *> Targets(pres::Shape &sh, bool typing, int para) {
    std::vector<pres::Paragraph *> out;
    if (sh.paras.empty()) sh.paras.push_back(pres::Paragraph{});
    if (typing && para >= 0 && para < static_cast<int>(sh.paras.size())) out.push_back(&sh.paras[static_cast<size_t>(para)]);
    else
        for (pres::Paragraph &p : sh.paras) out.push_back(&p);
    return out;
}

bool &Flag(pres::TextRun &r, char which) {
    switch (which) {
        case 'b': return r.bold;
        case 'i': return r.italic;
        case 'u': return r.underline;
        default: return r.strike;
    }
}

}  // namespace

void Editor::PresToggleStyle(PresSession &sess, char which) {
    pres::Shape *sh = PresSelectedShape(sess);
    if (!sh || !sh->HasText()) return;
    if (mode_ != Mode::PresInsert) PresPushUndo(sess);
    std::vector<pres::Paragraph *> ps = Targets(*sh, mode_ == Mode::PresInsert, sess.caret_para);
    bool all = true, any = false;
    for (pres::Paragraph *p : ps) {
        if (p->runs.empty()) p->runs.push_back(pres::TextRun{});
        for (pres::TextRun &r : p->runs) {
            all = all && Flag(r, which);
            any = true;
        }
    }
    for (pres::Paragraph *p : ps)
        for (pres::TextRun &r : p->runs) Flag(r, which) = !(all && any);
    PresMarkModified(sess);
}

void Editor::PresStepFontSize(PresSession &sess, double factor) {
    pres::Shape *sh = PresSelectedShape(sess);
    if (!sh || !sh->HasText()) return;
    if (mode_ != Mode::PresInsert) PresPushUndo(sess);
    auto step = [&](double pt) { return std::clamp(std::round(pt * factor * 2) / 2, 6.0, 200.0); };
    if (mode_ == Mode::PresInsert) {
        for (pres::Paragraph *p : Targets(*sh, true, sess.caret_para))
            for (pres::TextRun &r : p->runs) r.size_pt = step(r.size_pt > 0 ? r.size_pt : sh->font_pt);
    } else {
        sh->font_pt = step(sh->font_pt);
        for (pres::Paragraph &p : sh->paras)
            for (pres::TextRun &r : p.runs)
                if (r.size_pt > 0) r.size_pt = step(r.size_pt);
    }
    PresMarkModified(sess);
    status_message_ = "text size " + std::to_string(static_cast<int>(std::lround(sh->font_pt))) + "pt";
}

void Editor::PresSetAlign(PresSession &sess, pres::Align align) {
    pres::Shape *sh = PresSelectedShape(sess);
    if (!sh || !sh->HasText()) return;
    if (mode_ != Mode::PresInsert) PresPushUndo(sess);
    for (pres::Paragraph *p : Targets(*sh, mode_ == Mode::PresInsert, sess.caret_para)) p->align = align;
    PresMarkModified(sess);
}

void Editor::PresToggleBullets(PresSession &sess) {
    pres::Shape *sh = PresSelectedShape(sess);
    if (!sh || !sh->HasText()) return;
    if (mode_ != Mode::PresInsert) PresPushUndo(sess);
    std::vector<pres::Paragraph *> ps = Targets(*sh, mode_ == Mode::PresInsert, sess.caret_para);
    bool all = true;
    for (pres::Paragraph *p : ps) all = all && p->bullet;
    for (pres::Paragraph *p : ps) {
        p->bullet = !all;
        p->numbered = false;
    }
    PresMarkModified(sess);
}

void Editor::PresSetColor(PresSession &sess, int which, const std::string &rgb) {
    pres::Shape *sh = PresSelectedShape(sess);
    if (!sh) return;
    if (mode_ != Mode::PresInsert) PresPushUndo(sess);
    if (which == 1) {
        sh->fill = rgb;
        // A text box given a fill becomes a shape (and loses it back).
        if (sh->kind == pres::ShapeKind::Text && !rgb.empty()) sh->kind = pres::ShapeKind::Rect;
    } else if (which == 3) {
        sh->line = rgb;
        if (!rgb.empty() && sh->line_pt <= 0) sh->line_pt = 1.5;
    } else if (mode_ == Mode::PresInsert) {
        for (pres::Paragraph *p : Targets(*sh, true, sess.caret_para))
            for (pres::TextRun &r : p->runs) r.color = rgb;
    } else {
        sh->text_color = rgb;
        for (pres::Paragraph &p : sh->paras)
            for (pres::TextRun &r : p.runs) r.color.clear();
    }
    PresMarkModified(sess);
}

void Editor::PresSetBackground(PresSession &sess, const std::string &rgb) {
    PresPushUndo(sess);
    PresCurrentSlide(sess)->background = rgb;
    PresMarkModified(sess);
}

// --- Text -------------------------------------------------------------------------------

void Editor::PresBeginText(PresSession &sess, int para, int off) {
    pres::Shape *sh = PresSelectedShape(sess);
    if (!sh || !sh->HasText()) return;
    PresPushUndo(sess);
    if (sh->paras.empty()) sh->paras.push_back(pres::Paragraph{});
    if (para < 0 || para >= static_cast<int>(sh->paras.size())) para = static_cast<int>(sh->paras.size()) - 1;
    const int len = static_cast<int>(sh->paras[static_cast<size_t>(para)].PlainText().size());
    sess.caret_para = para;
    sess.caret_off = off < 0 || off > len ? len : off;
    mode_ = Mode::PresInsert;
}

void Editor::PresMath(PresSession &sess, const std::string &tex) {
    if (tex.empty()) {
        status_message_ = "E: :PresMath needs TeX, e.g. :PresMath \\frac{a}{b}";
        return;
    }
    pres::Shape *sh = PresSelectedShape(sess);
    if (sh && (mode_ == Mode::PresInsert || sess.math_from_typing) && sh->HasText()) {
        sess.math_from_typing = false;
        pres::InsertMath(*sh, &sess.caret_para, &sess.caret_off, tex);
        PresMarkModified(sess);
        mode_ = Mode::PresInsert;
        return;
    }
    PresPushUndo(sess);
    sh = PresSelectedShape(sess);
    if (sh && pres::IsEquationShape(*sh)) {
        // The box follows the equation, about the same centre.
        const pres::Shape fresh = pres::NewEquation(sess.doc, tex, sh->paras[0].runs[0].size_pt > 0 ? sh->paras[0].runs[0].size_pt : sh->font_pt);
        const long cx = sh->x + sh->w / 2, cy = sh->y + sh->h / 2;
        sh->w = fresh.w;
        sh->h = fresh.h;
        sh->x = cx - sh->w / 2;
        sh->y = cy - sh->h / 2;
        sh->paras[0].runs[0].tex = tex;
    } else {
        pres::Slide *s = PresCurrentSlide(sess);
        s->shapes.push_back(pres::NewEquation(sess.doc, tex));
        sess.selected = static_cast<int>(s->shapes.size()) - 1;
    }
    PresMarkModified(sess);
}

void Editor::PresEditEquation(PresSession &sess) {
    pres::Shape *sh = PresSelectedShape(sess);
    if (!sh || !pres::IsEquationShape(*sh)) return;
    BeginCommand("PresMath " + sh->paras[0].runs[0].tex);
}

void Editor::PresEndText(PresSession &sess) {
    if (mode_ == Mode::PresInsert && GetPres(CurPane().buffer_id) == &sess) mode_ = Mode::PresNormal;
}

void Editor::PresSetShapeText(PresSession &sess, const std::string &text) {
    pres::Shape *sh = PresSelectedShape(sess);
    if (!sh || !sh->HasText()) return;
    if (mode_ != Mode::PresInsert) PresPushUndo(sess);
    // Each line a paragraph, styled like the shape's first.
    pres::Paragraph proto = sh->paras.empty() ? pres::Paragraph{} : sh->paras[0];
    pres::TextRun run = proto.runs.empty() ? pres::TextRun{} : proto.runs[0];
    proto.runs.clear();
    sh->paras.clear();
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        pres::Paragraph p = proto;
        run.text = line;
        if (!line.empty()) p.runs.push_back(run);
        sh->paras.push_back(p);
    }
    if (sh->paras.empty()) sh->paras.push_back(proto);
    sess.caret_para = 0;
    sess.caret_off = 0;
    PresMarkModified(sess);
}

void Editor::PresInsertText(PresSession &sess, const std::string &text) {
    pres::Shape *sh = PresSelectedShape(sess);
    if (!sh) return;
    pres::InsertText(*sh, &sess.caret_para, &sess.caret_off, text);
    PresMarkModified(sess);
}

void Editor::PresBackspace(PresSession &sess, bool forward) {
    pres::Shape *sh = PresSelectedShape(sess);
    if (sh && pres::DeleteChar(*sh, &sess.caret_para, &sess.caret_off, forward)) PresMarkModified(sess);
}

void Editor::PresSplitParagraph(PresSession &sess) {
    pres::Shape *sh = PresSelectedShape(sess);
    if (!sh) return;
    pres::SplitParagraph(*sh, &sess.caret_para, &sess.caret_off);
    PresMarkModified(sess);
}

bool Editor::PresExport(PresSession &sess, const std::string &path, std::string *error) {
    if (path.empty()) {
        if (error) *error = "export to where? (:PresExport name.pptx or name.odp)";
        return false;
    }
    return pres::Save(sess.doc, path, error);
}

void Editor::PresTogglePresenting(PresSession &sess) {
    sess.presenting = !sess.presenting;
    if (mode_ == Mode::PresInsert) mode_ = Mode::PresNormal;
    sess.selected = -1;
    status_message_ = sess.presenting ? "presenting -- j/k or arrows to move, Esc to stop" : "";
}

void Editor::WheelPres(float dy) {
    PresSession *sess = GetPresMutable(CurPane().buffer_id);
    if (!sess) return;
    wheel_accum_pres_ += dy;
    // One slide a notch.
    while (wheel_accum_pres_ >= 1.0f) {
        wheel_accum_pres_ -= 1.0f;
        PresGotoSlide(*sess, sess->slide - 1);
    }
    while (wheel_accum_pres_ <= -1.0f) {
        wheel_accum_pres_ += 1.0f;
        PresGotoSlide(*sess, sess->slide + 1);
    }
}

// --- Keys -------------------------------------------------------------------------------

void Editor::HandlePresNormalInput() {
    PresSession *sess = GetPresMutable(CurPane().buffer_id);
    if (!sess) {
        mode_ = Mode::Normal;
        return;
    }
    PresSession &s = *sess;
    s.math_from_typing = false;  // (a maths command line abandoned with Esc)
    const int slides = static_cast<int>(s.doc.slides.size());
    const bool ctrl = CtrlDown(), shift = ShiftDown();
    pres::Shape *sel = PresSelectedShape(s);

    if (ctrl && gfx::IsKeyPressed(gfx::Key::R)) s.theme_colors = !s.theme_colors;
    if (s.presenting) {
        if (gfx::IsKeyPressed(gfx::Key::Escape)) PresTogglePresenting(s);
        if (Held(gfx::Key::Right) || Held(gfx::Key::Down) || Held(gfx::Key::PageDown) || Held(gfx::Key::Enter) || Held(gfx::Key::J))
            PresGotoSlide(s, s.slide + 1);
        if (Held(gfx::Key::Left) || Held(gfx::Key::Up) || Held(gfx::Key::PageUp) || Held(gfx::Key::Backspace) || Held(gfx::Key::K))
            PresGotoSlide(s, s.slide - 1);
        while (gfx::GetCharPressed() > 0) {
        }
        return;
    }

    if (ctrl) {
        if (gfx::IsKeyPressed(gfx::Key::Z)) {
            if (shift) PresRedo(s);
            else PresUndo(s);
        }
        if (gfx::IsKeyPressed(gfx::Key::Y)) PresRedo(s);
        if (gfx::IsKeyPressed(gfx::Key::B)) PresToggleStyle(s, 'b');
        if (gfx::IsKeyPressed(gfx::Key::I)) PresToggleStyle(s, 'i');
        if (gfx::IsKeyPressed(gfx::Key::U)) PresToggleStyle(s, 'u');
        if (gfx::IsKeyPressed(gfx::Key::C)) PresCopyShape(s);
        if (gfx::IsKeyPressed(gfx::Key::V)) PresPasteShape(s);
        if (gfx::IsKeyPressed(gfx::Key::D)) PresDuplicateShape(s);
        while (gfx::GetCharPressed() > 0) {
        }
        return;
    }
    if (Held(gfx::Key::PageDown)) PresGotoSlide(s, s.slide + 1);
    if (Held(gfx::Key::PageUp)) PresGotoSlide(s, s.slide - 1);
    if (gfx::IsKeyPressed(gfx::Key::Escape)) PresSelect(s, -1);
    if (gfx::IsKeyPressed(gfx::Key::Tab)) {
        const int n = static_cast<int>(PresCurrentSlide(s)->shapes.size());
        if (n > 0) PresSelect(s, ((s.selected < 0 ? (shift ? 0 : -1) : s.selected) + (shift ? n - 1 : 1)) % n);
    }
    if (sel) {
        // Nudge: a hundredth of the slide, a twentieth with Shift.
        const long step = (shift ? s.doc.width / 20 : s.doc.width / 100);
        long dx = 0, dy = 0;
        if (Held(gfx::Key::Left)) dx -= step;
        if (Held(gfx::Key::Right)) dx += step;
        if (Held(gfx::Key::Up)) dy -= step;
        if (Held(gfx::Key::Down)) dy += step;
        if (dx != 0 || dy != 0) {
            PresPushUndo(s);
            sel = PresSelectedShape(s);
            sel->x += dx;
            sel->y += dy;
            PresMarkModified(s);
        }
        if (Held(gfx::Key::Delete) || Held(gfx::Key::Backspace)) PresDeleteShape(s);
        if (gfx::IsKeyPressed(gfx::Key::Enter)) {
            // An equation is edited as TeX, on the command line.
            if (pres::IsEquationShape(*sel)) PresEditEquation(s);
            else PresBeginText(s);
            // The Enter that started typing must not also split a paragraph
            // on PresInsert's first frame.
            while (gfx::GetKeyPressed() != gfx::Key::None) {
            }
            while (gfx::GetCharPressed() > 0) {
            }
            return;
        }
    } else {
        if (Held(gfx::Key::Left) || Held(gfx::Key::Up)) PresGotoSlide(s, s.slide - 1);
        if (Held(gfx::Key::Right) || Held(gfx::Key::Down)) PresGotoSlide(s, s.slide + 1);
    }

    for (int cp = gfx::GetCharPressed(); cp > 0; cp = gfx::GetCharPressed()) {
        if (cp == ':') {
            EnterCommand();
            return;
        }
        if (cp == static_cast<int>(leader_key_) && !whichkey_bindings_.empty()) {
            TriggerWhichKey();
            return;
        }
        if (cp == 'g') {
            if (pending_g_) {
                pending_g_ = false;
                PresGotoSlide(s, 0);
            } else {
                pending_g_ = true;
            }
            continue;
        }
        pending_g_ = false;
        switch (cp) {
            case 'G': PresGotoSlide(s, slides - 1); break;
            case 'j': PresGotoSlide(s, s.slide + 1); break;
            case 'k': PresGotoSlide(s, s.slide - 1); break;
            case 'J': PresMoveSlide(s, 1); break;
            case 'K': PresMoveSlide(s, -1); break;
            case 'n': PresAddSlide(s, false); break;
            case 'N': PresAddSlide(s, true); break;
            case 'X': PresDeleteSlide(s); break;
            case 't': PresAddShape(s, pres::ShapeKind::Text); return;
            case 'r': PresAddShape(s, pres::ShapeKind::Rect); break;
            case 'o': PresAddShape(s, pres::ShapeKind::Ellipse); break;
            case 'O': PresAddShape(s, pres::ShapeKind::RoundRect); break;
            case 'L': PresAddShape(s, pres::ShapeKind::Line); break;
            case 'x': PresDeleteShape(s); break;
            case 'y': PresCopyShape(s); break;
            case 'p': PresPasteShape(s); break;
            case 'D': PresDuplicateShape(s); break;
            case ']': PresRestack(s, 1); break;
            case '[': PresRestack(s, -1); break;
            case '+':
            case '=': PresStepFontSize(s, 1.1); break;
            case '-': PresStepFontSize(s, 1 / 1.1); break;
            case 'b': PresToggleBullets(s); break;
            case 'u': PresUndo(s); break;
            case 'P': PresTogglePresenting(s); break;
            case 'i':
            case 'a':
                if (PresSelectedShape(s)) {
                    PresBeginText(s);
                    while (gfx::GetKeyPressed() != gfx::Key::None) {
                    }
                    while (gfx::GetCharPressed() > 0) {
                    }
                    return;
                }
                break;
            default: break;
        }
    }
}

void Editor::HandlePresInsertInput() {
    PresSession *sess = GetPresMutable(CurPane().buffer_id);
    if (!sess) {
        mode_ = Mode::Normal;
        return;
    }
    PresSession &s = *sess;
    pres::Shape *sh = PresSelectedShape(s);
    if (!sh || !sh->HasText()) {
        mode_ = Mode::PresNormal;
        return;
    }
    if (sh->paras.empty()) sh->paras.push_back(pres::Paragraph{});
    const bool ctrl = CtrlDown(), shift = ShiftDown();
    bool escape = false, enter = false;
    for (gfx::Key key = gfx::GetKeyPressed(); key != gfx::Key::None; key = gfx::GetKeyPressed()) {
        if (key == gfx::Key::Escape) escape = true;
        else if (key == gfx::Key::Enter || key == gfx::Key::KpEnter) enter = true;
    }
    if (escape) {
        mode_ = Mode::PresNormal;
        while (gfx::GetCharPressed() > 0) {
        }
        return;
    }
    if (ctrl) {
        if (gfx::IsKeyPressed(gfx::Key::B)) PresToggleStyle(s, 'b');
        if (gfx::IsKeyPressed(gfx::Key::I)) PresToggleStyle(s, 'i');
        if (gfx::IsKeyPressed(gfx::Key::U)) PresToggleStyle(s, 'u');
        if (gfx::IsKeyPressed(gfx::Key::R)) s.theme_colors = !s.theme_colors;
        while (gfx::GetCharPressed() > 0) {
        }
        // Ctrl-M: maths at the caret (:PresMath, typing resumes after it).
        if (gfx::IsKeyPressed(gfx::Key::M)) {
            s.math_from_typing = true;
            BeginCommand("PresMath ");
        }
        return;
    }
    if (enter) PresSplitParagraph(s);
    for (int cp = gfx::GetCharPressed(); cp > 0; cp = gfx::GetCharPressed())
        if (cp >= 32) PresInsertText(s, Utf8(cp));
    if (Held(gfx::Key::Backspace)) PresBackspace(s, false);
    if (Held(gfx::Key::Delete)) PresBackspace(s, true);
    if (gfx::IsKeyPressed(gfx::Key::Tab)) {
        pres::Paragraph &p = sh->paras[static_cast<size_t>(std::clamp(s.caret_para, 0, static_cast<int>(sh->paras.size()) - 1))];
        p.level = std::clamp(p.level + (shift ? -1 : 1), 0, 8);
        PresMarkModified(s);
    }
    sh = PresSelectedShape(s);
    if (!sh || sh->paras.empty()) return;
    s.caret_para = std::clamp(s.caret_para, 0, static_cast<int>(sh->paras.size()) - 1);
    std::string text = sh->paras[static_cast<size_t>(s.caret_para)].PlainText();
    s.caret_off = std::clamp(s.caret_off, 0, static_cast<int>(text.size()));
    if (Held(gfx::Key::Left)) {
        if (s.caret_off > 0) s.caret_off = PrevUtf8(text, s.caret_off);
        else if (s.caret_para > 0) {
            s.caret_para--;
            s.caret_off = static_cast<int>(sh->paras[static_cast<size_t>(s.caret_para)].PlainText().size());
        }
    }
    if (Held(gfx::Key::Right)) {
        if (s.caret_off < static_cast<int>(text.size())) s.caret_off = NextUtf8(text, s.caret_off);
        else if (s.caret_para + 1 < static_cast<int>(sh->paras.size())) {
            s.caret_para++;
            s.caret_off = 0;
        }
    }
    auto clamp_off = [&] {
        const std::string t = sh->paras[static_cast<size_t>(s.caret_para)].PlainText();
        s.caret_off = std::min(s.caret_off, static_cast<int>(t.size()));
        while (s.caret_off > 0 && s.caret_off < static_cast<int>(t.size()) && (static_cast<unsigned char>(t[static_cast<size_t>(s.caret_off)]) & 0xC0) == 0x80)
            --s.caret_off;
    };
    if (Held(gfx::Key::Up) && s.caret_para > 0) {
        s.caret_para--;
        clamp_off();
    }
    if (Held(gfx::Key::Down) && s.caret_para + 1 < static_cast<int>(sh->paras.size())) {
        s.caret_para++;
        clamp_off();
    }
    if (gfx::IsKeyPressed(gfx::Key::Home)) s.caret_off = 0;
    if (gfx::IsKeyPressed(gfx::Key::End)) s.caret_off = static_cast<int>(sh->paras[static_cast<size_t>(s.caret_para)].PlainText().size());
}
