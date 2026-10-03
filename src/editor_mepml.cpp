// Editor-side half of mepml (src/mepml_doc.h holds the pure half): turning
// a parsed document into Decorations, the heading/image/link registries
// DrawPane reads, folds, and splicing code-block results back into a
// buffer. Everything here is a thin layer over mepml::Parse/Highlight --
// the language's own rules live, and are tested, in mepml_doc.cpp.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "editor.h"
#include "gfx/input.h"
#include "gfx/platform.h"
#include "lua_env.h"
#include "job.h"
#include "mepml_doc.h"
#include "png_codec.h"

std::string LspFiletype(const std::string &fname);
// editor.cpp's mtime-cached image header sniff (native pixel size, 0/0 if unreadable).
void ImagePixelSizeCached(const std::string &path, int *width, int *height);

namespace {

bool ReadFileLines(const std::string &path, std::vector<std::string> *out) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out->push_back(line);
    }
    return true;
}

// Byte offset -> display column (codepoints), for the has_fg_color path,
// which alone among decoration kinds takes columns rather than bytes.
int ByteToColumn(const std::string &line, int byte) {
    int col = 0;
    const int n = std::min(byte, static_cast<int>(line.size()));
    for (int i = 0; i < n; ++i) {
        if ((static_cast<unsigned char>(line[static_cast<size_t>(i)]) & 0xC0) != 0x80) ++col;
    }
    return col;
}

const char *CalloutColor(const std::string &kw) {
    if (kw == "WARNING" || kw == "CAUTION") return "Yellow";
    if (kw == "ERROR" || kw == "DANGER") return "Red";
    if (kw == "TIP" || kw == "HINT" || kw == "SUCCESS") return "Green";
    if (kw == "TODO" || kw == "FIXME" || kw == "IMPORTANT") return "Purple";
    if (kw == "QUESTION" || kw == "EXAMPLE") return "Cyan";
    return "Blue";  // NOTE, INFO
}

const char *HeadingColor(int level) {
    return level == 1 ? "OrgHeadlineLevel1" : level == 2 ? "OrgHeadlineLevel2" : "OrgHeadlineLevel3";
}

std::string Lowered(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// A document header's title: its size relative to body text, unfolded
// and folded alike.
constexpr float kHeaderTitleScale = 2.2f;

// A document header: a run of `//?` lines on consecutive rows (Title,
// Option, Author, MACRO, ...). Each run is rendered as one card and folds
// as one block; `//?` lines right above a code fence belong to the block
// (its options) and are not Meta blocks, so they never start a run.
struct HeaderRun {
    int first = 0, last = 0;
    std::vector<const mepml::Block *> entries;
};
std::vector<HeaderRun> HeaderRuns(const mepml::Document &doc) {
    std::vector<HeaderRun> runs;
    for (const mepml::Block &b : doc.blocks) {
        if (!b.origin.empty() || b.kind != mepml::BlockKind::Meta) continue;
        if (!runs.empty() && runs.back().last + 1 == b.line_start) {
            runs.back().last = b.line_end;
        } else {
            runs.push_back({b.line_start, b.line_end, {}});
        }
        runs.back().entries.push_back(&b);
    }
    return runs;
}

// Where a `//? Key: value` line's key and value sit (byte offsets).
struct MetaLineCols {
    int value_start = 0;  // first byte of the value (the line's end when it has none)
    bool ok = false;
};
MetaLineCols MetaCols(const std::string &line) {
    MetaLineCols c;
    const size_t a = line.find_first_not_of(" \t");
    if (a == std::string::npos || line.compare(a, 3, "//?") != 0) return c;
    const size_t colon = line.find(':', a + 3);
    size_t v = colon == std::string::npos ? line.size() : colon + 1;
    while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) ++v;
    c.value_start = static_cast<int>(v);
    c.ok = true;
    return c;
}

// Which blocks run in a terminal, and with what: `exec` (the body is a
// command line), or a shell block with results=terminal (its body is the
// script) -- both handed to that shell with -c.
bool TerminalShellFor(const mepml::Block &b, std::string *shell) {
    if (b.kind != mepml::BlockKind::Code) return false;
    const std::string lang = Lowered(b.lang);
    if (lang == "exec" || lang == "executable") {
        *shell = "/bin/sh";
        return true;
    }
    bool terminal = false;
    for (const mepml::Option &o : b.options)
        terminal = terminal || ((Lowered(o.name) == "results" || Lowered(o.name) == "output") && Lowered(o.value.s) == "terminal");
    if (!terminal) return false;
    if (lang == "sh" || lang == "shell") *shell = "/bin/sh";
    else if (lang == "bash" || lang == "zsh" || lang == "fish" || lang == "dash") *shell = lang;
    else return false;
    return true;
}

int IntOption(const mepml::Block &b, const char *name, int fallback) {
    for (const mepml::Option &o : b.options)
        if (Lowered(o.name) == name && o.value.kind == mepml::ValueKind::Int) return static_cast<int>(o.value.i);
    return fallback;
}

// The code block a run belongs to: the one with its command whose fence
// is nearest where the run last saw it (edits above move it).
const mepml::Block *BlockForRun(const mepml::Document &doc, const std::string &code, int fence_row) {
    const mepml::Block *best = nullptr;
    int best_d = 1 << 30;
    for (const mepml::Block &b : doc.blocks) {
        if (!b.origin.empty() || b.kind != mepml::BlockKind::Code || b.code != code) continue;
        const int d = std::abs(b.code_line_start - 1 - fence_row);
        if (d < best_d) {
            best_d = d;
            best = &b;
        }
    }
    return best;
}

int Codepoints(const std::string &s) {
    int n = 0;
    for (char c : s)
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    return n;
}

// The size a span is drawn at relative to body text: >big< and <small>
// are fixed steps, \fs{pt} is relative to a 12pt body. Clamped so a typo
// can't make a row fill the pane.
float SpanScale(const mepml::Span &s) {
    float k = 1.0f;
    if (s.style & mepml::kBig) k *= 1.3f;
    if (s.style & mepml::kSmall) k *= 0.8f;
    if (s.font_size > 0.0f) k *= s.font_size / 12.0f;
    if (s.style & (mepml::kSuper | mepml::kSub)) k *= 0.7f;
    return std::clamp(k, 0.5f, 3.0f);
}

// \f{family} -> one of the faces mep embeds (Decoration::virt_family).
std::string SpanFamily(const std::string &font) {
    if (font.empty()) return "";
    std::string f;
    for (char c : font) f += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char *mono : {"mono", "courier", "consol", "code", "menlo", "fixed"})
        if (f.find(mono) != std::string::npos) return "mono";
    if (f.find("sans") != std::string::npos) return "sans";
    for (const char *serif : {"serif", "times", "georgia", "garamond", "palatino", "cambria", "book", "roman"})
        if (f.find(serif) != std::string::npos) return "serif";
    return "sans";  // Helvetica, Arial, Verdana, ...
}

// Columns DrawPane reserves for a run drawn at `scale` (Decoration::virt_scale).
// The largest a scaled run is drawn inside a table cell (see MepmlScan).
constexpr float kTableMaxScale = 1.25f;

int StyledCols(int codepoints, float scale) {
    return std::max(1, static_cast<int>(std::ceil(static_cast<float>(codepoints) * scale - 1e-3f)));
}

// What a span occupies once concealed -- the same arithmetic DrawPane's
// collapse does: hidden markup is 0, a replacement its own codepoints, a
// scaled run StyledCols(). `max_scale` caps runs the way table cells do.
int ConcealedWidth(const mepml::Span &sp, const std::string &line, float max_scale) {
    const int cp = Codepoints(line.substr(static_cast<size_t>(sp.col_start),
                                          static_cast<size_t>(sp.col_end - sp.col_start)));
    if (sp.markup) return Codepoints(sp.replace);
    const float k = std::min(SpanScale(sp), max_scale);
    if ((k != 1.0f || !SpanFamily(sp.font).empty()) && !(sp.style & (mepml::kMath | mepml::kComment)))
        return StyledCols(cp, k);
    return cp;
}

std::string Repeat(const char *glyph, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += glyph;
    return out;
}

}  // namespace

bool Editor::IsMepmlBuffer() const { return LspFiletype(Buf().filename) == "mepml"; }

int Editor::HeadingLevelForRow(const Buffer &buf, int row) {
    if (row < 0 || row >= buf.LineCount()) return 0;
    const std::string &line = buf.lines[static_cast<size_t>(row)];
    const std::string ft = LspFiletype(buf.filename);
    if (ft == "org") return OrgHeadlineLevel(line);
    if (ft == "mepml") {
        auto it = buf.mepml_heading_rows.find(row);
        if (it == buf.mepml_heading_rows.end()) return 0;
        return mepml::LineHeadingLevel(line) == it->second ? it->second : 0;
    }
    return 0;
}

int Editor::HeadingExtraSlotsForLevel(int level) {
    if (level <= 0) return 0;
    constexpr int kCount = static_cast<int>(sizeof(kOrgHeadingStyles) / sizeof(kOrgHeadingStyles[0]));
    return kOrgHeadingStyles[std::min(level, kCount) - 1].slots - 1;
}

int Editor::HeadingHideLenForRow(const Buffer &buf, int row) {
    if (row < 0 || row >= buf.LineCount()) return 0;
    const std::string &line = buf.lines[static_cast<size_t>(row)];
    const std::string ft = LspFiletype(buf.filename);
    if (ft == "org") return OrgHeadlineStarHideLen(line);
    if (ft == "mepml" && HeadingLevelForRow(buf, row) > 0) return mepml::LineHeadingMarkupLen(line);
    return 0;
}

int Editor::HeadingIndentColsForRow(const Buffer &buf, int row) {
    if (row < 0 || row >= buf.LineCount()) return 0;
    const std::string &line = buf.lines[static_cast<size_t>(row)];
    const std::string ft = LspFiletype(buf.filename);
    if (ft == "org") return OrgHeadlineStarIndentCols(line);
    if (ft == "mepml") return std::max(0, HeadingLevelForRow(buf, row) - 1);
    return 0;
}

int Editor::RowTopPadSlots(const Buffer &buf, int row) const {
    if (row < 0 || row >= buf.LineCount()) return 0;
    bool folded = false;
    for (const Fold &f : buf.folds)
        if (f.closed && f.start_row == row) folded = true;
    // A closed fold draws its summary line, not the row's maths.
    const int math = folded ? 0 : InlineMathPadFor(buf, row).top;
    if (!org_conceal_visible_) return math;  // scaled runs are only drawn while concealing
    const size_t hash = std::hash<std::string>{}(buf.lines[static_cast<size_t>(row)]);
    float scale = 1.0f;
    if (folded) {
        // A closed fold draws its summary line, not the row: only a folded
        // mepml header's summary has a large title (its own scale).
        auto sit = buf.mepml_fold_summaries.find(row);
        if (sit == buf.mepml_fold_summaries.end() || sit->second.text_hash != hash) return 0;
        scale = sit->second.title_scale;
    } else {
        if (const int pics = MepmlTableImageBoxes(buf, row, nullptr)) return std::max(pics, math);
        auto it = buf.mepml_row_scale.find(row);
        if (it == buf.mepml_row_scale.end() || hash != it->second.text_hash) return math;
        if (org_images_visible_ && buf.org_image_rows.count(row)) return 0;
        scale = it->second.scale;
    }
    // How far the tallest run rises above the body text's own box
    // (DrawStyledRun bottom-aligns on a ~0.78em ascent).
    const double lh = render_line_height_;
    const double fs = lh - 6.0;
    const double over = 0.78 * (static_cast<double>(scale) - 1.0) * fs;
    // A glyph's box starts above its ink, and every line has 6px of
    // leading: a >big< (1.3x) run's few pixels of overshoot fit in that.
    constexpr double kSlack = 6.0;
    if (over <= kSlack) return math;
    return std::max(math, static_cast<int>(std::ceil((over - kSlack) / lh)));
}

float Editor::InlineMathTopOffset(const Buffer::OrgLatexInlineSpan &span) const {
    const double lh = render_line_height_;
    if (span.baseline < 0.0f) return static_cast<float>((lh - static_cast<double>(span.height)) / 2.0);
    // The prose's baseline: g_font is baked so its ascent-to-descent span
    // is the font size (stbtt_ScaleForPixelHeight), and JetBrains Mono's
    // ascent is 1020 of those 1320 units; the line's 6px of leading sit
    // under the text (LineHeight).
    constexpr double kTextAscentEm = 1020.0 / 1320.0;
    const double text_baseline = kTextAscentEm * (lh - 6.0);
    return static_cast<float>(text_baseline - static_cast<double>(span.baseline));
}

Editor::InlineMathPad Editor::InlineMathPadFor(const Buffer &buf, int row) const {
    InlineMathPad pad;
    if (!org_latex_visible_ || render_line_height_ <= 0.0) return pad;
    auto it = buf.org_latex_inline.find(row);
    if (it == buf.org_latex_inline.end()) return pad;
    if (org_images_visible_ && buf.org_image_rows.count(row)) return pad;  // drawn as its picture
    const double lh = render_line_height_;
    double over_top = 0.0, over_bottom = 0.0;
    for (const Buffer::OrgLatexInlineSpan &sp : it->second) {
        if (sp.path.empty() || sp.height <= 0) continue;
        const double top = static_cast<double>(InlineMathTopOffset(sp));
        over_top = std::max(over_top, -top);
        over_bottom = std::max(over_bottom, top + static_cast<double>(sp.height) - lh);
    }
    // A superscript or a small inline fraction pokes a few pixels past
    // the line, into the leading and the neighbours' empty ascender and
    // descender space: no reason to push whole lines apart for that.
    const double slack = 0.25 * lh;
    auto slots = [&](double over) { return over > slack ? static_cast<int>(std::ceil((over - slack) / lh)) : 0; };
    pad.top = slots(over_top);
    pad.bottom = slots(over_bottom);
    return pad;
}

int Editor::RowMathExtraSlots(const Buffer &buf, int row, int sublines, int wrap_cols, int cursor_row) const {
    const InlineMathPad pad = InlineMathPadFor(buf, row);
    if (pad.top + pad.bottom == 0) return 0;
    // Soft-wrap counts a row's lines by its raw length (WrapLenForRow),
    // and inline maths' TeX is far longer than its render, so the last of
    // them are often left empty. They draw nothing, so only the lines the
    // row can still fill once its maths collapses get maths room: never
    // fewer than the draw loop fills (the other concealed markup only
    // shortens the row further), so nothing is drawn past what is counted.
    int lines = std::max(1, sublines);
    if (wrap_cols > 0 && lines > 1 && !buf.mepml_table_row_cols.count(row) && render_char_width_ > 0.0) {
        int len = WrapLenForRow(buf, row);
        for (const Buffer::OrgLatexInlineSpan &sp : buf.org_latex_inline.at(row)) {
            if (sp.col_end <= sp.col_start || OrgLatexInlineRevealed(sp, row, cursor_row)) continue;
            int drawn = 0;  // a fragment's continuation rows collapse to nothing
            if (!sp.path.empty()) {
                if (sp.width <= 0) continue;  // unreadable: its source stays
                drawn = std::max(1, static_cast<int>(std::ceil(static_cast<double>(sp.width) / render_char_width_ - 0.05)));
            }
            len -= std::max(0, (sp.col_end - sp.col_start) - drawn);
        }
        lines = std::clamp((std::max(1, len) + wrap_cols - 1) / wrap_cols, 1, lines);
    }
    return (lines - 1) * (pad.top + pad.bottom) + pad.bottom;
}

int Editor::RowLinePitchSlots(const Buffer &buf, int row) const {
    const InlineMathPad pad = InlineMathPadFor(buf, row);
    return 1 + pad.top + pad.bottom;
}

int Editor::MepmlTableImageBoxes(const Buffer &buf, int row, std::vector<MepmlCellImageBox> *out) const {
    if (out) out->clear();
    if (!org_conceal_visible_ || !org_images_visible_) return 0;
    auto it = buf.mepml_table_images.find(row);
    if (it == buf.mepml_table_images.end() || row < 0 || row >= buf.LineCount()) return 0;
    if (std::hash<std::string>{}(buf.lines[static_cast<size_t>(row)]) != it->second.text_hash) return 0;
    const float cw = std::max(1.0f, static_cast<float>(render_char_width_));
    const float lh = std::max(1.0f, static_cast<float>(render_line_height_));
    // A picture fills its cell's width, but never grows past its own
    // pixels, nor taller than kMaxSlots lines (a portrait shrinks instead).
    constexpr int kMaxSlots = 12, kUnknownSlots = 6;
    constexpr float kPad = 4.0f;  // px above and below the tallest picture
    std::vector<MepmlCellImageBox> boxes;
    float tallest = 0.0f;
    for (const Buffer::MepmlTableImage &img : it->second.cells) {
        MepmlCellImageBox b;
        b.image = &img;
        const float box_w = std::max(1.0f, static_cast<float>(img.cols) * cw);
        if (img.width > 0 && img.height > 0) {
            float k = std::min({1.0f, box_w / static_cast<float>(img.width),
                                static_cast<float>(kMaxSlots) * lh / static_cast<float>(img.height)});
            b.w = static_cast<float>(img.width) * k;
            b.h = static_cast<float>(img.height) * k;
        } else {
            b.w = box_w;
            b.h = static_cast<float>(kUnknownSlots) * lh;
        }
        b.x = static_cast<float>(img.col) * cw + (box_w - b.w) * 0.5f;
        tallest = std::max(tallest, b.h);
        boxes.push_back(b);
    }
    if (boxes.empty()) return 0;
    const int slots = static_cast<int>(std::ceil((tallest + 2.0f * kPad) / lh - 1e-3f));
    // Every picture centred in the headroom, so a row of mixed shapes
    // shares one middle line.
    const float room = static_cast<float>(slots) * lh;
    for (MepmlCellImageBox &b : boxes) b.y = (room - b.h) * 0.5f;
    if (out) *out = std::move(boxes);
    return slots;
}

const Buffer::MepmlVirtualBlock *Editor::MepmlVirtualBlockForRow(const Buffer &buf, int row, int cursor_row) const {
    // Drawn even under the cursor, like an inline image: the row is one
    // word of source, and a block that vanished whenever the caret passed
    // through it could never be scrolled through or clicked. The raw
    // directive is a concealment toggle (<leader>km) away.
    (void)cursor_row;
    if (!org_conceal_visible_) return nullptr;
    auto it = buf.mepml_virtual_rows.find(row);
    if (it == buf.mepml_virtual_rows.end() || row < 0 || row >= buf.LineCount()) return nullptr;
    if (std::hash<std::string>{}(buf.lines[static_cast<size_t>(row)]) != it->second.text_hash) return nullptr;
    for (const Fold &f : buf.folds)
        if (f.closed && f.start_row <= row && f.end_row >= row) return nullptr;
    return &it->second;
}

const Buffer::MepmlFoldSummary *Editor::MepmlFoldSummaryForRow(const Buffer &buf, int row, int cursor_row) const {
    if (!org_conceal_visible_ || row < 0 || row >= buf.LineCount()) return nullptr;
    auto it = buf.mepml_fold_summaries.find(row);
    if (it == buf.mepml_fold_summaries.end()) return nullptr;
    if (row == cursor_row && !it->second.keep_under_cursor) return nullptr;
    if (std::hash<std::string>{}(buf.lines[static_cast<size_t>(row)]) != it->second.text_hash) return nullptr;
    return &it->second;
}

bool Editor::MepmlToggleRaw() {
    Buffer &buf = Buf();
    buf.mepml_raw = !buf.mepml_raw;
    return buf.mepml_raw;
}

bool Editor::MepmlRaw(int buffer_id) const {
    if (buffer_id < 0 || buffer_id >= static_cast<int>(buffers_.size())) return false;
    return buffers_[static_cast<size_t>(buffer_id)].mepml_raw;
}

int Editor::MepmlToggleHeaderFolds() {
    if (!IsMepmlBuffer()) return -1;
    RecomputeMepmlFolds();
    std::vector<Fold *> heads;
    for (const HeaderRun &run : HeaderRuns(MepmlParseCurrent(false)))
        for (Fold &f : Buf().folds)
            if (f.provider == "mepml" && f.start_row == run.first && f.end_row == run.last) heads.push_back(&f);
    if (heads.empty()) return -1;
    bool any_open = false;
    for (const Fold *f : heads) any_open = any_open || !f->closed;
    for (Fold *f : heads) f->closed = any_open;
    return any_open ? 1 : 0;
}

std::string Editor::MepmlCurrentFile() const {
    std::string file = Buf().filename;
    std::error_code ec;
    if (!file.empty() && file[0] != '/') file = std::filesystem::absolute(file, ec).string();
    return file;
}

namespace {
long long FileMtime(const std::string &path) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    return ec ? -1 : static_cast<long long>(t.time_since_epoch().count());
}
}  // namespace

Editor::MepmlParseCache &Editor::MepmlCacheEntry(bool with_imports) const {
    const Buffer &buf = Buf();
    const int id = CurrentBufferId();
    MepmlParseCache &plain = mepml_parse_cache_[0][id];
    if (!(plain.valid && plain.lines == buf.lines)) {
        plain.doc = mepml::Parse(buf.lines);
        plain.lines = buf.lines;
        plain.valid = true;
        plain.spans_valid = false;
        plain.spans.clear();
        plain.has_imports = false;
        plain.generation = ++mepml_parse_generation_;
        for (const mepml::Block &b : plain.doc.blocks) {
            if (b.kind == mepml::BlockKind::Import) plain.has_imports = true;
            if (b.kind == mepml::BlockKind::Meta && b.keyword.size() == 6) {
                std::string k = b.keyword;
                for (char &ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (k == "import") plain.has_imports = true;
            }
            if (plain.has_imports) break;
        }
    }
    if (!with_imports || !plain.has_imports) return plain;

    std::string file = buf.filename;
    std::error_code ec;
    if (!file.empty() && file[0] != '/') file = std::filesystem::absolute(file, ec).string();
    MepmlParseCache &c = mepml_parse_cache_[1][id];
    if (c.valid && c.file == file && c.lines == buf.lines) {
        bool fresh = true;
        for (const auto &r : c.reads) {
            if (FileMtime(r.first) != r.second) {
                fresh = false;
                break;
            }
        }
        if (fresh) return c;
    }
    c.reads.clear();
    c.doc = mepml::ParseWithImports(file, buf.lines, [&c](const std::string &path, std::vector<std::string> *out) {
        c.reads.emplace_back(path, FileMtime(path));
        return ReadFileLines(path, out);
    });
    c.file = file;
    c.lines = buf.lines;
    c.valid = true;
    c.spans_valid = false;
    c.spans.clear();
    c.generation = ++mepml_parse_generation_;
    return c;
}

const mepml::Document &Editor::MepmlParseCurrent(bool with_imports) const { return MepmlCacheEntry(with_imports).doc; }

const std::vector<mepml::Span> &Editor::MepmlSpansCurrent(bool with_imports) const {
    MepmlParseCache &c = MepmlCacheEntry(with_imports);
    if (!c.spans_valid) {
        c.spans = mepml::Highlight(c.doc);
        c.spans_valid = true;
    }
    return c.spans;
}

namespace {
// Buffers shorter than this parse in place when asked (a few hundred
// microseconds); only above it is the parse worth a thread and a frame's delay.
constexpr int kMepmlAsyncParseMinLines = 2000;
}  // namespace

bool Editor::MepmlParseReady() {
    if (!IsMepmlBuffer()) return true;
    const Buffer &buf = Buf();
    if (buf.LineCount() < kMepmlAsyncParseMinLines) return true;
    const int id = CurrentBufferId();
    MepmlParseCache &plain = mepml_parse_cache_[0][id];
    auto fresh = [&] {
        if (!(plain.valid && plain.lines == buf.lines)) return false;
        if (!plain.has_imports) return true;
        const MepmlParseCache &imp = mepml_parse_cache_[1][id];
        return imp.valid && imp.lines == buf.lines;
    };
    if (fresh()) return true;

    MepmlAsyncParse &a = mepml_async_[id];
    if (a.running && a.result.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        MepmlAsyncResult r = a.result.get();
        a.running = false;
        if (r.lines == buf.lines) {
            plain.doc = std::move(r.plain);
            plain.spans = std::move(r.plain_spans);
            plain.spans_valid = true;
            plain.lines = r.lines;
            plain.valid = true;
            plain.has_imports = r.has_imports;
            plain.generation = ++mepml_parse_generation_;
            if (r.has_imports) {
                MepmlParseCache &imp = mepml_parse_cache_[1][id];
                imp.doc = std::move(r.imports);
                imp.spans = std::move(r.imports_spans);
                imp.spans_valid = true;
                imp.lines = std::move(r.lines);
                imp.file = r.file;
                imp.reads = std::move(r.reads);
                imp.valid = true;
                imp.generation = ++mepml_parse_generation_;
            }
            return true;
        }
    }
    if (!a.running) {
        std::string file = buf.filename;
        std::error_code ec;
        if (!file.empty() && file[0] != '/') file = std::filesystem::absolute(file, ec).string();
        // Parse, Highlight and ParseWithImports keep no shared mutable
        // state (mepml_doc.cpp: function-local const tables only), so a
        // snapshot of the lines is all the worker needs.
        a.result = std::async(std::launch::async, [lines = buf.lines, file]() mutable {
            MepmlAsyncResult r;
            r.lines = std::move(lines);
            r.file = file;
            r.plain = mepml::Parse(r.lines);
            for (const mepml::Block &b : r.plain.blocks) {
                std::string k = b.keyword;
                for (char &ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (b.kind == mepml::BlockKind::Import || (b.kind == mepml::BlockKind::Meta && k == "import")) {
                    r.has_imports = true;
                    break;
                }
            }
            r.plain_spans = mepml::Highlight(r.plain);
            if (r.has_imports) {
                r.imports = mepml::ParseWithImports(file, r.lines, [&r](const std::string &path, std::vector<std::string> *out) {
                    r.reads.emplace_back(path, FileMtime(path));
                    return ReadFileLines(path, out);
                });
                r.imports_spans = mepml::Highlight(r.imports);
            }
            return r;
        });
        a.running = true;
    }
    return false;
}

void Editor::MepmlScan(int ns, bool own_diagnostics) {
    Buffer &buf = Buf();
    // A document just opened: its folds are built now, rather than lazily
    // by the first fold command, so the code its exports leave out starts
    // out folded (Buffer::mepml_folds_seeded).
    if (!buf.mepml_folds_seeded && !(present_.active && CurrentBufferId() == present_.view_buffer))
        RecomputeMepmlFolds();
    int cur_row = 0, cur_col = 0;
    GetCursorForLua(&cur_row, &cur_col);
    const bool conceal = OrgConcealVisible();

    // The rows of a caption or alt text the cursor is in show their source,
    // every one of them (the rendered text stands in for all of them).
    // So do all the rows of a formula spanning lines that the cursor is in:
    // its render is withdrawn for the whole fragment (OrgLatexRenderForRow,
    // OrgLatexInlineRevealed), and a `\(` or `$$` left concealed on another
    // of its rows read as a missing delimiter.
    auto revealed_for = [](const mepml::Document &d, int row) {
        std::unordered_set<int> out;
        auto reveal = [&](int first, int last) {
            if (first >= 0 && row >= first && row <= last)
                for (int r = first; r <= last; ++r) out.insert(r);
        };
        std::function<void(const mepml::Block &, const std::vector<mepml::Inline> &)> reveal_math =
            [&](const mepml::Block &b, const std::vector<mepml::Inline> &ins) {
                for (const mepml::Inline &x : ins) {
                    if (x.kind == mepml::InlineKind::Math) reveal(b.OffsetToPos(x.start).line, b.OffsetToPos(x.end).line);
                    else reveal_math(b, x.children);
                }
            };
        for (const mepml::Block &b : d.blocks) {
            if (!b.origin.empty()) continue;
            reveal(b.caption_line, b.caption_line_end);
            reveal(b.alt_line, b.alt_line_end);
            if (b.kind == mepml::BlockKind::MathBlock) {
                int end = b.line_end;
                if (b.caption_line >= 0) end = std::min(end, b.caption_line - 1);
                if (b.alt_line >= 0) end = std::min(end, b.alt_line - 1);
                reveal(b.line_start, end);
            }
            reveal_math(b, b.inlines);
            for (const mepml::ListItem &it : b.items) reveal_math(b, it.content);
        }
        return out;
    };

    // Patch or full. A scan that differs from the last full one only in
    // the cursor's row (same parse, same toggles and widths, nothing else
    // having touched the namespace) re-emits just the rows that depend on
    // the cursor: the old and new cursor rows and whatever each reveals
    // as a unit. Everything the rest of this function builds -- the
    // registries, cards, table grids, header layout -- depends on the
    // document alone, except where a table or a document header holds
    // the cursor (both lay out around the cursor's row), which takes the
    // full scan. So does a buffer with a running terminal or program
    // window, whose results rows the full scan follows.
    MepmlScanState &state = mepml_scan_state_[CurrentBufferId()];
    const MepmlParseCache *entry = IsMepmlBuffer() ? &MepmlCacheEntry(true) : nullptr;
    // Widths the scan lays out to: captions and HTML results (TextWidth,
    // the pane's columns) and the slide rule (TextColsForBuffer).
    const int text_width = TextWidth();
    const int scan_pane_cols = CurPane().text_cols, scan_buffer_cols = TextColsForBuffer(CurrentBufferId());
    const bool images = OrgImagesVisible();
    std::unordered_set<int> patch_rows, patch_tables;
    const bool same_inputs = entry && state.valid && state.doc == entry && state.generation == entry->generation &&
                             state.ns == ns && state.own_diagnostics == own_diagnostics && state.conceal == conceal &&
                             state.images == images && state.text_width == text_width &&
                             state.pane_cols == scan_pane_cols && state.buffer_cols == scan_buffer_cols &&
                             state.table_math_gen == buf.mepml_table_math_gen &&
                             buf.decorations.count(ns) && buf.decorations[ns].size() == state.deco_count &&
                             !buf.mepml_raw;
    bool patch = same_inputs && state.cur_row != cur_row;
    bool runs_here = false;
    for (const auto &kv : mepml_terms_)
        if (kv.second.buffer_id == CurrentBufferId()) runs_here = true;
    for (const auto &kv : mepml_guis_)
        if (kv.second.buffer_id == CurrentBufferId() ||
            (present_.active && CurrentBufferId() == present_.view_buffer && kv.second.buffer_id == present_.source_buffer))
            runs_here = true;  // (a presented slide shows the document's windows)
    // Nothing it depends on has changed -- which is often: the edit hooks
    // fire on the change epoch, which most keys bump, motions included.
    if (same_inputs && state.cur_row == cur_row && !runs_here) return;
    if (runs_here) patch = false;
    if (patch) {
        const mepml::Document &d = entry->doc;
        auto touches = [&](int first, int last) {
            return (state.cur_row >= first && state.cur_row <= last) || (cur_row >= first && cur_row <= last);
        };
        for (const HeaderRun &run : HeaderRuns(d))
            if (touches(run.first, run.last)) patch = false;
        if (patch) {
            patch_rows = revealed_for(d, state.cur_row);
            for (int r : revealed_for(d, cur_row)) patch_rows.insert(r);
            patch_rows.insert(state.cur_row);
            patch_rows.insert(cur_row);
            // A table lays out around the cursor's row (its every other
            // row is aligned to a grid; the cursor's shows its raw text), so
            // a table the cursor left or entered is re-laid out whole.
            for (const mepml::Block &b : d.blocks) {
                if (!b.origin.empty() || b.kind != mepml::BlockKind::Table || !touches(b.line_start, b.line_end)) continue;
                patch_tables.insert(b.line_start);
                for (int r = b.line_start; r <= b.line_end; ++r) patch_rows.insert(r);
            }
            std::vector<Decoration> &v = buf.decorations[ns];
            v.erase(std::remove_if(v.begin(), v.end(), [&](const Decoration &x) { return patch_rows.count(x.row) > 0; }),
                    v.end());
        }
    }
    // Only these rows are emitted in a patch; every row in a full scan.
    auto in_scope = [&](int row) { return !patch || patch_rows.count(row) > 0; };

    if (!patch) {
        ClearNamespace(ns);
        buf.mepml_heading_rows.clear();
        buf.mepml_row_scale.clear();
        buf.mepml_table_images.clear();
        buf.mepml_table_row_cols.clear();
        buf.mepml_virtual_rows.clear();
        buf.mepml_fold_summaries.clear();
        buf.mepml_html_rows.clear();
        mepml_table_grids_.erase(CurrentBufferId());
        mepml_block_cards_.erase(CurrentBufferId());
        state.valid = false;
        if (!IsMepmlBuffer()) return;
        ClearOrgImageRows();
        buf.org_link_spans.clear();
        // Raw (Buffer::mepml_raw): everything above cleared, nothing put
        // back -- the grammar's colours are all that is left.
        if (buf.mepml_raw) return;
    }

    const mepml::Document &doc = MepmlParseCurrent(true);
    const int n = buf.LineCount();
    if (!patch) {
    // Terminals running in this buffer's blocks: follow each to its block
    // (MepmlTermRun::fence_row), and give its results rows its screen.
    for (auto &kv : mepml_terms_) {
        MepmlTermRun &run = kv.second;
        if (run.buffer_id != CurrentBufferId()) continue;
        const mepml::Block *b = BlockForRun(doc, run.code, run.fence_row);
        if (!b) continue;
        run.fence_row = b->code_line_start - 1;
        run.results_end = b->result_line_end;
        if (b->result_line_start >= 0 && b->result_line_end > b->result_line_start + 1) {
            Buffer::OrgLatexRender r;
            r.term_run = kv.first;
            r.end_row = b->result_line_end - 1;
            r.slots = run.rows;
            buf.mepml_html_rows[b->result_line_start + 1] = std::move(r);
        }
    }
    // Programs with a window of their own: their results rows are where it goes.
    for (auto &kv : mepml_guis_) {
        MepmlGuiRun &run = kv.second;
        if (run.buffer_id != CurrentBufferId()) continue;
        const mepml::Block *b = BlockForRun(doc, run.code, run.fence_row);
        if (!b) continue;
        run.fence_row = b->code_line_start - 1;
        run.results_end = b->result_line_end;
        if (b->result_line_start >= 0 && b->result_line_end > b->result_line_start + 1) {
            Buffer::OrgLatexRender r;
            r.term_run = kv.first;  // (ids are shared with terminal runs)
            r.end_row = b->result_line_end - 1;
            r.slots = run.rows;
            buf.mepml_html_rows[b->result_line_start + 1] = std::move(r);
        }
    }

    // A presented slide (MepmlPresentStart): a program running in the
    // document's block shows its window under the slide's output fence for
    // that block (mepml::PresentationPage::Shown::live_fence).
    if (present_.active && CurrentBufferId() == present_.view_buffer && !present_.pages.empty()) {
        const mepml::PresentationPage &page = present_.pages[static_cast<size_t>(present_.page)];
        for (auto &kv : mepml_guis_) {
            if (kv.second.buffer_id != present_.source_buffer) continue;
            const int fence = MepmlPresentSourceFence(kv.first);
            for (const mepml::PresentationPage::Shown &sh : page.blocks) {
                if (sh.source_fence != fence || sh.live_fence < 0) continue;
                const int view_fence = sh.live_fence + 1;  // the view's first row is the cursor's blank
                for (const mepml::Block &b : doc.blocks) {
                    if (b.kind != mepml::BlockKind::Code || b.code_line_start - 1 != view_fence) continue;
                    if (b.code_line_end < b.code_line_start) continue;
                    Buffer::OrgLatexRender r;
                    r.term_run = kv.first;
                    r.end_row = b.code_line_end;
                    r.slots = kv.second.rows;
                    buf.mepml_html_rows[b.code_line_start] = std::move(r);
                }
            }
        }
    }

    // Registries: headings, images, clickable links.
    std::function<void(const mepml::Block &, const std::vector<mepml::Inline> &)> links =
        [&](const mepml::Block &b, const std::vector<mepml::Inline> &ins) {
            for (const mepml::Inline &x : ins) {
                if (x.kind == mepml::InlineKind::Link) {
                    mepml::Block::Pos p = b.OffsetToPos(x.start), q = b.OffsetToPos(x.end);
                    if (p.line == q.line) {
                        Buffer::OrgLinkSpan sp;
                        sp.col_start = p.col;
                        sp.col_end = q.col;
                        sp.target = x.arg;
                        sp.display = b.text.substr(static_cast<size_t>(x.inner_start),
                                                   static_cast<size_t>(x.inner_end - x.inner_start));
                        sp.bracketed = true;
                        buf.org_link_spans[p.line].push_back(sp);
                    }
                }
                links(b, x.children);
            }
        };
    // Captions read "Figure N: ..." / "Table N: ..." (numbered exactly as
    // the HTML export numbers them, mepml::BlockLabels).
    const std::vector<std::string> block_labels = mepml::BlockLabels(doc);
    for (const mepml::Block &b : doc.blocks) {
        if (!b.origin.empty()) continue;
        if (b.kind == mepml::BlockKind::Heading) buf.mepml_heading_rows[b.line_start] = b.level;
        // A caption and an alt text draw as their text wrapped to the text
        // width in place of their source rows (one line or several) --
        // centred under a figure (an \image, or a code block that drew one),
        // which is drawn centred itself; the alt text small. Their raw rows come back
        // while the cursor is in them (OrgLatexRenderForRow).
        if (conceal && (b.caption_line >= 0 || b.alt_line >= 0)) {
            const bool centred = b.kind == mepml::BlockKind::Image ||
                                 (b.kind == mepml::BlockKind::Code && !b.result_images.empty());
            int width = TextWidth();
            if (CurPane().text_cols > 8) width = std::min(width, CurPane().text_cols - 1);
            auto place = [&](int first, int last, std::vector<mepml::RenderedLine> lines, float scale) {
                if (lines.empty() || first < 0 || last < first || last >= n) return;
                Buffer::OrgLatexRender r;
                r.styled = std::move(lines);
                r.styled_scale = scale;
                r.end_row = last;
                r.slots = static_cast<int>(r.styled.size());
                buf.mepml_html_rows[first] = std::move(r);
            };
            if (b.caption_line >= 0) {
                const size_t idx = static_cast<size_t>(&b - doc.blocks.data());
                place(b.caption_line, b.caption_line_end,
                      mepml::RenderCaption(doc, b, idx < block_labels.size() ? block_labels[idx] : std::string(), width, centred),
                      1.0f);
            }
            if (b.alt_line >= 0 && !b.alt.empty()) {
                constexpr float kAltScale = 0.85f;
                place(b.alt_line, b.alt_line_end,
                      mepml::RenderAltText(b.alt, static_cast<int>(static_cast<float>(width) / kAltScale), centred), kAltScale);
            }
        }
        // An imported file's path (`\import(path)` or a header `//? Import:
        // path`) is a link to it: <leader>kl or a click opens the file.
        if ((b.kind == mepml::BlockKind::Import || (b.kind == mepml::BlockKind::Meta && Lowered(b.keyword) == "import")) &&
            b.line_start >= 0 && b.line_start < buf.LineCount()) {
            const std::string &line = buf.lines[static_cast<size_t>(b.line_start)];
            const std::string target = [&] {
                const size_t a = b.value.find_first_not_of(" \t"), z = b.value.find_last_not_of(" \t");
                return a == std::string::npos ? std::string() : b.value.substr(a, z - a + 1);
            }();
            const size_t from = b.kind == mepml::BlockKind::Import ? line.find_first_of("{(") : line.find(':');
            const size_t at = target.empty() || from == std::string::npos ? std::string::npos : line.find(target, from);
            if (at != std::string::npos) {
                Buffer::OrgLinkSpan sp;
                sp.col_start = static_cast<int>(at);
                sp.col_end = static_cast<int>(at + target.size());
                sp.target = target;
                sp.display = target;
                buf.org_link_spans[b.line_start].push_back(sp);
            }
        }
        if (b.kind == mepml::BlockKind::TableOfContents || b.kind == mepml::BlockKind::Bibliography) {
            Buffer::MepmlVirtualBlock vb;
            const int width = std::max(20, TextWidth());
            vb.lines = b.kind == mepml::BlockKind::TableOfContents ? mepml::RenderToc(doc, width)
                                                                   : mepml::RenderBibliography(doc, width);
            vb.text_hash = std::hash<std::string>{}(buf.lines[static_cast<size_t>(b.line_start)]);
            // Folded (RecomputeMepmlFolds), it reads as its title and a count.
            Buffer::MepmlFoldSummary sum;
            sum.title = vb.lines.empty() ? std::string() : vb.lines[0].text;
            sum.title_hl = "OrgHeadlineLevel2";
            sum.keep_under_cursor = true;
            sum.text_hash = vb.text_hash;
            size_t count = doc.cite_order.size();
            const char *noun = count == 1 ? "reference" : "references";
            if (b.kind == mepml::BlockKind::TableOfContents) {
                count = 0;
                for (const mepml::Block &h : doc.blocks)
                    if (h.kind == mepml::BlockKind::Heading && h.origin.empty()) ++count;
                noun = count == 1 ? "heading" : "headings";
            }
            sum.detail = "  \u2022 " + std::to_string(count) + " " + noun;
            buf.mepml_fold_summaries[b.line_start] = std::move(sum);
            buf.mepml_virtual_rows[b.line_start] = std::move(vb);
        }
        if (b.kind == mepml::BlockKind::Image && !b.value.empty()) {
            std::string resolved = OrgResolvePath(b.value);
            std::error_code ec;
            if (std::filesystem::exists(resolved, ec)) SetOrgImageRow(b.line_start, resolved);
        }
        // HTML a code block produced (`results=html`): its result lines
        // draw as the rendered markup, at the height it lays out to.
        if (b.kind == mepml::BlockKind::Code && b.result_format == "html" && html_measure_ &&
            b.result_line_end > b.result_line_start + 1) {
            Buffer::OrgLatexRender r;
            for (size_t k = 0; k < b.result_lines.size(); ++k) r.html += (k ? "\n" : "") + b.result_lines[k];
            const std::string file = MepmlCurrentFile();
            r.base_dir = file.empty() ? std::string(".") : std::filesystem::path(file).parent_path().string();
            r.end_row = b.result_line_end - 1;
            // The text width, or the pane's when that is narrower (the
            // card is inset a little from both edges).
            r.html_cols = TextWidth();
            if (CurPane().text_cols > 8) r.html_cols = std::min(r.html_cols, CurPane().text_cols - 3);
            r.slots = std::max(1, html_measure_(r.html, r.base_dir, r.html_cols));
            buf.mepml_html_rows[b.result_line_start + 1] = std::move(r);
        }
        // Figures a code block produced (`// \image(...)` result lines) are
        // drawn on their own rows inside the results card.
        for (const auto &img : b.result_images) {
            std::string resolved = OrgResolvePath(img.second);
            std::error_code ec;
            if (std::filesystem::exists(resolved, ec)) SetOrgImageRow(img.first, resolved);
        }
        links(b, b.inlines);
        links(b, b.caption_inlines);
        for (const mepml::ListItem &it : b.items) links(b, it.content);
        for (const auto &row : b.rows)
            for (const mepml::TableCell &c : row) links(b, c.content);
    }

    }  // !patch: the registries

    auto add = [&](Decoration d) {
        if (d.row < 0 || d.row >= n) return;
        const int len = static_cast<int>(buf.lines[static_cast<size_t>(d.row)].size());
        d.col_start = std::clamp(d.col_start, 0, len);
        d.col_end = std::clamp(d.col_end, d.col_start, len);
        if (d.col_end <= d.col_start && !d.whole_line) return;
        // Above the grammar's own captures (the 'syntax' namespace, kBuiltinSyntax)
        // wherever both colour the same span: this scan knows the context.
        if (d.priority == 0) d.priority = 2;
        AddDecoration(ns, std::move(d));
    };

    const std::vector<mepml::Span> &spans = MepmlSpansCurrent(true);

    const std::unordered_set<int> revealed_rows = revealed_for(doc, cur_row);
    if (!patch) MepmlBuildCards(doc);
    // Table rows laid out by MepmlTableLayout below (every row of a table
    // but the cursor's, while concealing): their pipes are drawn there,
    // so the per-span pass leaves them alone.
    std::unordered_set<int> citation_rows;
    for (const mepml::Block &b : doc.blocks)
        if (b.origin.empty() && b.kind == mepml::BlockKind::Citation)
            for (int row = b.line_start; row <= b.line_end; ++row) citation_rows.insert(row);
    // Slide titles (their first heading), for the "Slide N" rule.
    std::map<int, std::string> slide_titles;
    for (const mepml::Slide &sl : mepml::Slides(doc, n)) slide_titles[sl.number] = sl.title;
    std::unordered_set<int> table_layout_rows, table_rows;
    for (const mepml::Block &b : doc.blocks) {
        if (!b.origin.empty() || b.kind != mepml::BlockKind::Table) continue;
        for (int row = b.line_start; row <= (b.rows_end >= 0 ? b.rows_end : b.line_end); ++row) {
            if ((b.caption_line >= 0 && row >= b.caption_line && row <= b.caption_line_end) ||
                (b.alt_line >= 0 && row >= b.alt_line && row <= b.alt_line_end))
                continue;
            table_rows.insert(row);
            if (conceal && row != cur_row) table_layout_rows.insert(row);
        }
    }
    // Concealed markup right at the open edge of a GFM row -- `~setosa~ |
    // 1.46` has no leading pipe to draw it on -- is left to
    // MepmlTableLayout, which draws the missing pipe in the markup's place.
    std::set<std::pair<int, int>> table_edge_markup;  // (row, col_start)
    for (const mepml::Span &s : spans) {
        if (!in_scope(s.line)) continue;
        if (!s.markup || (s.style & (mepml::kMath | mepml::kDirective)) || !table_layout_rows.count(s.line)) continue;
        const std::string &line = buf.lines[static_cast<size_t>(s.line)];
        const std::vector<std::pair<int, int>> cells = mepml::TableCells(line);
        if (cells.empty()) continue;
        int first = cells.front().first, last = cells.back().second;
        const bool lead = first > 0 && line[static_cast<size_t>(first - 1)] == '|';
        const bool trail = last < static_cast<int>(line.size()) && line[static_cast<size_t>(last)] == '|';
        while (first < last && (line[static_cast<size_t>(first)] == ' ' || line[static_cast<size_t>(first)] == '\t')) ++first;
        while (last > first && (line[static_cast<size_t>(last - 1)] == ' ' || line[static_cast<size_t>(last - 1)] == '\t')) --last;
        if ((!lead && s.col_start == first) || (!trail && s.col_end == last)) table_edge_markup.emplace(s.line, s.col_start);
    }

    for (const mepml::Span &s : spans) {
        if (s.line < 0 || s.line >= n || !in_scope(s.line)) continue;
        const std::string &line = buf.lines[static_cast<size_t>(s.line)];
        const bool on_cursor = s.line == cur_row || revealed_rows.count(s.line) > 0;
        const bool heading_row = buf.mepml_heading_rows.count(s.line) > 0;
        const bool hide = conceal && !on_cursor && !heading_row;
        Decoration base;
        base.row = s.line;
        base.col_start = s.col_start;
        base.col_end = s.col_end;

        // Colour first: the most specific construct wins.
        std::string hl;
        if (s.style & mepml::kHeading) hl = HeadingColor(s.heading_level);
        if (s.style & mepml::kComment) hl = "Comment";
        if (s.style & mepml::kMeta) hl = "Purple";
        if (s.style & mepml::kDirective) hl = "Cyan";
        if (s.style & mepml::kCode) hl = (s.style & mepml::kDirective) ? "Comment" : "";
        if (s.style & mepml::kResult) hl = (s.style & mepml::kComment) ? "Comment" : "";
        if (s.style & mepml::kTableRule) hl = "Comment";
        if (s.style & (mepml::kSuper | mepml::kSub)) hl = "Purple";
        if (s.style & mepml::kMono) hl = "Cyan";
        if (s.style & mepml::kInsert) hl = "Green";
        if (s.style & mepml::kDelete) hl = "Red";
        if (s.style & mepml::kVerbatim) hl = "Green";
        if (s.style & mepml::kMath) hl = "Purple";
        if (s.style & mepml::kFootnote) hl = "Comment";
        if (s.style & mepml::kLink) hl = "Blue";
        if (s.style & mepml::kCite) hl = (s.style & mepml::kError) ? "Red" : "Blue";
        if (s.style & mepml::kCallout) hl = CalloutColor(s.callout);
        if (s.style & mepml::kRule) hl = "Comment";
        if (s.style & mepml::kListMarker) hl = "Yellow";
        // A box's label and markup in its kind's colour; its title keeps
        // the text's own, in bold.
        if (s.style & mepml::kBox) {
            const mepml::BoxKind *kind = mepml::FindBoxKind(s.target);
            hl = !s.markup ? "" : kind ? kind->hl : "Cyan";
        }

        // Inside a table the grid's band is one line tall, so a run is
        // capped at what fits a line rather than given headroom.
        const float scale = table_rows.count(s.line) ? std::min(SpanScale(s), kTableMaxScale) : SpanScale(s);
        const std::string family = SpanFamily(s.font);
        const bool styled = !s.markup && (scale != 1.0f || !family.empty()) && !heading_row &&
                            !(s.style & (mepml::kCode | mepml::kResult | mepml::kMath | mepml::kComment));
        if (styled && scale > 1.0f && !table_rows.count(s.line)) {
            Buffer::MepmlRowScale &rs = buf.mepml_row_scale[s.line];
            rs.scale = std::max(rs.scale, scale);
            rs.text_hash = std::hash<std::string>{}(line);
        }
        // Raw (cursor row, concealment off): size can't be shown on the
        // column grid, so it is hinted with colour instead.
        if (!(styled && hide)) {
            if (s.style & mepml::kSmall) hl = "Comment";
            if (s.style & mepml::kBig) hl = "Yellow";
        }
        if ((s.style & mepml::kTableRule) && table_layout_rows.count(s.line)) continue;
        if (table_edge_markup.count({s.line, s.col_start})) continue;
        // A picture cell's `\image(...)`: MepmlTableLayout hides it and the
        // picture is drawn above the row.
        if ((s.style & mepml::kTable) && (s.style & mepml::kDirective) && !s.target.empty() &&
            table_layout_rows.count(s.line) && OrgImagesVisible())
            continue;
        // Rows whose inner structure the tree-sitter grammar colours better
        // than one flat colour could -- //? key: value lines, code fences'
        // language and options, citation fields -- keep its captures
        // (mep.syntax_highlight) rather than being painted over here.
        if ((s.style & mepml::kMeta) || ((s.style & mepml::kCode) && (s.style & mepml::kDirective)) ||
            citation_rows.count(s.line))
            continue;

        if (s.markup) {
            if (hide && !(s.style & mepml::kHeading)) {
                Decoration d = base;
                d.virt_overlay = true;
                d.priority = 10;
                if ((s.style & mepml::kRule) && s.replace.empty()) {
                    d.virt_text = Repeat("─", s.col_end - s.col_start);
                } else if (s.style & mepml::kTableRule) {
                    // `|` -> `│`, and the |---| separator row drawn as a rule
                    // -- same width, so the columns never move.
                    std::string raw = line.substr(static_cast<size_t>(s.col_start),
                                                  static_cast<size_t>(s.col_end - s.col_start));
                    for (char c : raw) d.virt_text += c == '|' ? "│" : (c == '-' || c == ':' || c == '=') ? "─" : std::string(1, c);
                } else {
                    d.virt_text = s.replace;
                    d.conceal = s.replace.empty();
                }
                d.virt_text_hl = hl.empty() ? "Comment" : hl;
                d.bold = ((s.style & (mepml::kCallout | mepml::kCite)) != 0 && !s.replace.empty() &&
                          (s.style & mepml::kCallout)) ||
                         ((s.style & mepml::kBox) && !s.replace.empty());
                // A slide's opener and closer: rules across the text,
                // the opener's labelled "Slide N: title". A trailing
                // comment keeps its place, so then the rule stops short.
                if (s.style & mepml::kSlide) {
                    const bool alone = line.find_first_not_of(" \t", static_cast<size_t>(s.col_end)) == std::string::npos;
                    // (Never wider than the pane, where it would wrap.)
                    const int pane_cols = TextColsForBuffer(CurrentBufferId());
                    const int width = (pane_cols > 0 ? std::min(TextWidth(), pane_cols - 1) : TextWidth()) - s.col_start;
                    std::string label;
                    if (!s.replace.empty()) {
                        label = "── " + s.replace;
                        const int number = std::atoi(s.replace.c_str() + 6);  // "Slide N"
                        auto t = slide_titles.find(number);
                        if (t != slide_titles.end() && !t->second.empty()) label += ": " + t->second;
                        label += " ";
                    }
                    d.virt_text = label + (alone ? Repeat("─", std::max(3, width - Codepoints(label))) : std::string());
                    d.conceal = false;
                    d.bold = !label.empty();
                    d.virt_text_hl = label.empty() ? "Comment" : "Cyan";
                    add(d);
                    continue;
                }
                // An abstract's label: bold, and centred when it has its
                // line to itself (the way a paper sets it).
                if ((s.style & mepml::kAbstract) && !s.replace.empty()) {
                    d.bold = true;
                    d.virt_text_hl = HeadingColor(2);
                    if (s.replace == "Abstract" && line.find_first_not_of(" \t", static_cast<size_t>(s.col_end)) == std::string::npos)
                        d.virt_text = std::string(static_cast<size_t>(std::max(0, (TextWidth() - Codepoints(s.replace)) / 2)), ' ') + s.replace;
                }
                // A proof's tombstone, where its `)` was: at the right edge.
                if ((s.style & mepml::kBox) && s.replace == "\u220E") {
                    const int pane_cols = TextColsForBuffer(CurrentBufferId());
                    const int width = (pane_cols > 0 ? std::min(TextWidth(), pane_cols - 1) : TextWidth()) - s.col_start;
                    d.virt_text = std::string(static_cast<size_t>(std::max(0, width - 2)), ' ') + s.replace;
                    d.bold = false;
                }
                add(d);
            } else {
                Decoration d = base;
                d.hl_group = (s.style & (mepml::kCallout | mepml::kHeading | mepml::kListMarker | mepml::kCite |
                                         mepml::kDirective | mepml::kMeta))
                                 ? hl
                                 : "Comment";
                if (d.hl_group.empty()) d.hl_group = "Comment";
                add(d);
            }
            continue;
        }

        // Scaled / other-face text: the run is replaced by itself drawn at
        // its real size and face (Decoration::virt_scale/virt_family),
        // carrying every style it has, so nothing else is drawn over it.
        if (styled && hide) {
            Decoration d = base;
            d.virt_overlay = true;
            d.virt_text = line.substr(static_cast<size_t>(s.col_start), static_cast<size_t>(s.col_end - s.col_start));
            d.virt_scale = scale;
            d.virt_family = family;
            d.virt_raise = (s.style & mepml::kSuper) ? 0.38f : (s.style & mepml::kSub) ? -0.2f : 0.0f;
            d.priority = 10;
            d.bold = (s.style & mepml::kBold) != 0;
            d.italic = (s.style & mepml::kItalic) != 0;
            d.underline = (s.style & (mepml::kUnderline | mepml::kInsert | mepml::kLink)) != 0;
            d.strikethrough = (s.style & (mepml::kStrike | mepml::kDelete)) != 0;
            std::uint32_t crgb = 0;
            if (!s.color.empty() && mepml::ParseColor(s.color, &crgb)) {
                d.has_fg_color = true;
                d.fg_color = ThemeColor{static_cast<unsigned char>((crgb >> 16) & 0xff),
                                        static_cast<unsigned char>((crgb >> 8) & 0xff),
                                        static_cast<unsigned char>(crgb & 0xff), 255};
            }
            d.virt_text_hl = hl.empty() ? "Normal" : hl;
            add(d);
            if (s.style & mepml::kHighlight) {
                Decoration h = base;
                h.hl_group = "Yellow";
                h.bg_fill = true;
                h.priority = -5;
                add(h);
            }
            continue;
        }

        // Content. A literal \color{} beats the theme colour.
        std::uint32_t rgb = 0;
        if (!s.color.empty() && mepml::ParseColor(s.color, &rgb)) {
            Decoration d = base;
            d.col_start = ByteToColumn(line, s.col_start);
            d.col_end = ByteToColumn(line, s.col_end);
            d.has_fg_color = true;
            d.fg_color = ThemeColor{static_cast<unsigned char>((rgb >> 16) & 0xff),
                                    static_cast<unsigned char>((rgb >> 8) & 0xff),
                                    static_cast<unsigned char>(rgb & 0xff), 255};
            add(d);
        } else if (!hl.empty()) {
            Decoration d = base;
            d.hl_group = hl;
            add(d);
        }
        if (s.style & mepml::kHighlight) {
            Decoration d = base;
            d.hl_group = "Yellow";
            d.bg_fill = true;
            d.priority = -5;  // under every other style on the span
            add(d);
        }
        const bool bold = (s.style & mepml::kBold) || ((s.style & mepml::kTable) && (s.style & mepml::kBold));
        const std::string style_hl = hl.empty() ? "Normal" : hl;
        if (bold) {
            Decoration d = base;
            d.bold = true;
            d.hl_group = style_hl;
            add(d);
        }
        if (s.style & mepml::kItalic) {
            Decoration d = base;
            d.italic = true;
            d.hl_group = style_hl;
            add(d);
        }
        if (s.style & (mepml::kUnderline | mepml::kInsert | mepml::kLink)) {
            Decoration d = base;
            d.underline = true;
            d.hl_group = style_hl;
            add(d);
        }
        if (s.style & (mepml::kStrike | mepml::kDelete)) {
            Decoration d = base;
            d.strikethrough = true;
            d.hl_group = (s.style & mepml::kDelete) ? "Red" : "Comment";
            add(d);
        }
    }

    if (!patch) {
        if (conceal) MepmlTableLayout(doc, spans, table_layout_rows, table_edge_markup, ns);
        MepmlFitCards();
    } else if (!patch_tables.empty() && conceal) {
        // Replace just these tables' grids and picture rows.
        std::vector<OrgTableGrid> &grids = mepml_table_grids_[CurrentBufferId()];
        grids.erase(std::remove_if(grids.begin(), grids.end(),
                                   [&](const OrgTableGrid &g) { return patch_tables.count(g.start_row) > 0; }),
                    grids.end());
        for (const mepml::Block &b : doc.blocks)
            if (b.origin.empty() && b.kind == mepml::BlockKind::Table && patch_tables.count(b.line_start))
                for (int r = b.line_start; r <= b.line_end; ++r) buf.mepml_table_images.erase(r);
        MepmlTableLayout(doc, spans, table_layout_rows, table_edge_markup, ns, &patch_tables);
        MepmlFitCards();
    }

    // Callouts get a coloured bar in the sign column on every line.
    for (const mepml::Block &b : doc.blocks) {
        if (!b.origin.empty() || b.kind != mepml::BlockKind::Callout) continue;
        for (int row = b.line_start; row <= b.line_end; ++row) {
            if (!in_scope(row)) continue;
            Decoration d;
            d.row = row;
            d.whole_line = true;
            d.sign_shape = "bar";
            d.sign_hl = CalloutColor(b.keyword);
            add(d);
        }
    }

    // Document headers (runs of `//?` lines): the title large and bold,
    // a subtitle under it, every other key as a muted label in a column of
    // its own with its value beside it -- an Option's name = value
    // coloured by type. The card behind them comes from MepmlBuildCards;
    // the cursor's row shows its raw line, like any other markup.
    for (const HeaderRun &run : HeaderRuns(doc)) {
        if (patch) break;  // never holds the cursor in a patch; its fold summaries are the full scan's
        int label_w = 0;
        for (const mepml::Block *e : run.entries) {
            const std::string k = Lowered(e->keyword);
            if (k != "title" && k != "subtitle") label_w = std::max(label_w, Codepoints(e->keyword));
        }
        std::string title;
        int options = 0;
        std::vector<std::string> others;
        for (const mepml::Block *e : run.entries) {
            const std::string k = Lowered(e->keyword);
            if (k == "title") title = e->value;
            else if (k == "option") ++options;
            else others.push_back(k == "subtitle" || k == "author" || k == "date" || k == "import" ? e->value : k);
        }
        // Folded, the header reads as its title and a muted tally.
        if (run.last > run.first) {
            Buffer::MepmlFoldSummary sum;
            sum.title = title.empty() ? run.entries[0]->keyword + ": " + run.entries[0]->value : title;
            sum.title_scale = title.empty() ? 1.0f : kHeaderTitleScale;
            if (options > 0) others.insert(others.begin(), std::to_string(options) + (options == 1 ? " option" : " options"));
            for (const std::string &o : others)
                if (!o.empty()) sum.detail += "  \u2022 " + o;
            sum.text_hash = std::hash<std::string>{}(buf.lines[static_cast<size_t>(run.first)]);
            buf.mepml_fold_summaries[run.first] = std::move(sum);
        }
        if (!conceal) continue;
        for (const mepml::Block *e : run.entries) {
            const int row = e->line_start;
            if (row < 0 || row >= n) continue;
            const std::string &line = buf.lines[static_cast<size_t>(row)];
            const MetaLineCols mc = MetaCols(line);
            if (!mc.ok) continue;
            const std::string k = Lowered(e->keyword);
            const int len = static_cast<int>(line.size());
            const std::string value = line.substr(static_cast<size_t>(mc.value_start));
            if (k == "title" || k == "subtitle") {
                const float scale = k == "title" ? kHeaderTitleScale : 1.15f;
                // The padding a scaled row needs above it, whether or not
                // the cursor is on it (so moving onto it doesn't jump).
                if (scale > 1.0f && !value.empty()) {
                    Buffer::MepmlRowScale &rs = buf.mepml_row_scale[row];
                    rs.scale = std::max(rs.scale, scale);
                    rs.text_hash = std::hash<std::string>{}(line);
                }
                if (row == cur_row) continue;
                Decoration pre;
                pre.row = row;
                pre.col_start = 0;
                pre.col_end = mc.value_start;
                pre.virt_overlay = true;
                pre.conceal = true;
                pre.priority = 10;
                add(pre);
                if (value.empty()) continue;
                Decoration v;
                v.row = row;
                v.col_start = mc.value_start;
                v.col_end = len;
                v.virt_overlay = true;
                v.virt_text = value;
                v.virt_scale = scale;
                v.bold = k == "title";
                v.italic = k == "subtitle";
                v.virt_text_hl = k == "title" ? HeadingColor(1) : "Comment";
                v.priority = 10;
                add(v);
                continue;
            }
            if (row == cur_row) continue;
            // `//? Key: ` becomes the key, padded so every value in the
            // header starts in the same column.
            Decoration label;
            label.row = row;
            label.col_start = 0;
            label.col_end = mc.value_start;
            label.virt_overlay = true;
            label.virt_text = e->keyword + std::string(static_cast<size_t>(label_w - Codepoints(e->keyword) + 2), ' ');
            label.virt_text_hl = "Comment";
            label.priority = 10;
            add(label);
            if (value.empty()) continue;
            auto piece = [&](int from, int to, const std::string &text, const char *hl, bool bold) {
                Decoration d;
                d.row = row;
                d.col_start = from;
                d.col_end = to;
                d.virt_overlay = true;
                d.virt_text = text;
                d.virt_text_hl = hl;
                d.bold = bold;
                d.priority = 10;
                add(d);
            };
            const size_t eq = value.find('=');
            if (k == "option" && eq != std::string::npos && eq > 0) {
                std::string name = value.substr(0, eq);
                while (!name.empty() && name.back() == ' ') name.pop_back();
                size_t vs = eq + 1;
                while (vs < value.size() && value[vs] == ' ') ++vs;
                const std::string raw = value.substr(vs);
                const mepml::Value parsed = mepml::Value::Parse(raw);
                const int at = mc.value_start;
                piece(at, at + static_cast<int>(eq), name, "Cyan", true);
                piece(at + static_cast<int>(eq), at + static_cast<int>(vs), " = ", "Comment", false);
                if (!raw.empty())
                    piece(at + static_cast<int>(vs), len, raw, parsed.kind == mepml::ValueKind::String ? "Green" : "Purple", false);
            } else {
                piece(mc.value_start, len, value, k == "import" ? "Blue" : "Normal", false);
            }
        }
    }

    // Diagnostics: an underline on the offending columns; the message
    // itself only on the cursor's row, where it is being asked about.
    for (const mepml::Diagnostic &dg : doc.diagnostics) {
        if (!own_diagnostics) break;  // the language server's, a superset, are drawn instead
        if (dg.line < 0 || dg.line >= n || !in_scope(dg.line)) continue;
        const char *group = dg.severity == mepml::Diagnostic::Error ? "Error" : "Warn";
        Decoration u;
        u.row = dg.line;
        u.col_start = dg.col_start;
        u.col_end = std::max(dg.col_end, dg.col_start + 1);
        u.underline = true;
        u.hl_group = group;
        add(u);
        if (dg.line == cur_row) {
            Decoration m;
            m.row = dg.line;
            m.col_start = m.col_end = 0;
            m.virt_text = "  " + dg.message;
            m.virt_text_hl = group;
            m.virt_text_eol = true;
            AddDecoration(ns, std::move(m));
        }
    }

    state.valid = true;
    state.doc = &MepmlCacheEntry(true);
    state.generation = MepmlCacheEntry(true).generation;
    state.ns = ns;
    state.own_diagnostics = own_diagnostics;
    state.conceal = conceal;
    state.images = images;
    state.text_width = text_width;
    state.pane_cols = scan_pane_cols;
    state.buffer_cols = scan_buffer_cols;
    state.cur_row = cur_row;
    state.table_math_gen = buf.mepml_table_math_gen;
    state.deco_count = buf.decorations.count(ns) ? buf.decorations[ns].size() : 0;
}

void Editor::RecomputeMepmlFolds() {
    std::vector<Fold> old_folds;
    for (const Fold &f : Buf().folds)
        if (f.provider == "mepml") old_folds.push_back(f);
    ClearFoldsFromProvider("mepml");
    if (!IsMepmlBuffer()) return;
    const mepml::Document &doc = MepmlParseCurrent(false);
    const int n = Buf().LineCount();
    // The first build on a buffer starts the code the exports leave out
    // folded (Buffer::mepml_folds_seeded) -- not the presentation's own
    // page buffer, nor with folding off, nor around the cursor.
    const bool seed = !Buf().mepml_folds_seeded && Buf().fold_enabled &&
                      !(present_.active && CurrentBufferId() == present_.view_buffer);
    Buf().mepml_folds_seeded = true;
    const int cursor_row = CurPane().buffer_id == CurrentBufferId() ? CurPane().cursor.row : -1;
    auto add = [&](int start, int end, bool seed_closed = false) {
        if (end < start) return;
        bool closed = seed && seed_closed && !(cursor_row >= start && cursor_row <= end);
        for (const Fold &of : old_folds) {
            if (of.start_row == start) {
                closed = of.closed;
                break;
            }
        }
        Buf().folds.push_back({start, end, closed, "mepml"});
    };
    // Whether a code block's code is left out of the exports.
    auto code_hidden = [&](const mepml::Block &b) {
        bool code = true, results = true;
        mepml::CodeExports(doc, b, &code, &results);
        return !code;
    };
    // Heading sections: to the line before the next heading of the same or
    // shallower depth, trailing blank lines excluded -- and never past the
    // end of the slide the heading is on (its closer is the slide's).
    const std::vector<mepml::Slide> slides = mepml::Slides(doc, n);
    std::vector<const mepml::Block *> heads;
    for (const mepml::Block &b : doc.blocks)
        if (b.kind == mepml::BlockKind::Heading) heads.push_back(&b);
    for (size_t k = 0; k < heads.size(); ++k) {
        int end = n - 1;
        for (size_t j = k + 1; j < heads.size(); ++j) {
            if (heads[j]->level <= heads[k]->level) {
                end = heads[j]->line_start - 1;
                break;
            }
        }
        for (const mepml::Slide &sl : slides) {
            if (heads[k]->line_start <= sl.line_start || heads[k]->line_start > sl.line_end) continue;
            end = std::min(end, sl.closed ? sl.line_end - 1 : sl.line_end);
        }
        while (end > heads[k]->line_start && Buf().lines[static_cast<size_t>(end)].find_first_not_of(" \t") == std::string::npos)
            --end;
        add(heads[k]->line_start, end);
    }
    for (const HeaderRun &run : HeaderRuns(doc)) add(run.first, run.last);
    for (const mepml::Slide &sl : slides) add(sl.line_start, sl.line_end);
    // Boxes (\definition ...): from the opening line to the closing `)`.
    {
        std::vector<const mepml::Block *> open;
        for (const mepml::Block &b : doc.blocks) {
            if (!b.origin.empty()) continue;
            if (b.kind == mepml::BlockKind::BoxBegin && b.box_closed) add(b.line_start, b.line_end);
            else if (b.kind == mepml::BlockKind::BoxBegin) open.push_back(&b);
            else if (b.kind == mepml::BlockKind::BoxEnd && !open.empty()) {
                add(open.back()->line_start, b.line_end);
                open.pop_back();
            }
        }
    }
    for (const mepml::Block &b : doc.blocks) {
        // \toc/\bibliography: one row of source drawn as a block of
        // generated lines -- folding it (a one-row fold, hiding no rows)
        // collapses the rendering to its title (MepmlScan's summary).
        if (b.kind == mepml::BlockKind::TableOfContents || b.kind == mepml::BlockKind::Bibliography) {
            add(b.line_start, b.line_start);
            continue;
        }
        // A code block and its results fold separately: the code (its
        // option lines and fences), and the result_begin..result_end region.
        if (b.kind == mepml::BlockKind::Code && b.result_line_start >= 0) {
            add(b.line_start, b.result_line_start - 1, code_hidden(b));
            add(b.result_line_start, b.result_line_end);
            continue;
        }
        if (b.kind == mepml::BlockKind::Code) {
            add(b.line_start, b.line_end, code_hidden(b));
            continue;
        }
        if (b.kind == mepml::BlockKind::Citation ||
            b.kind == mepml::BlockKind::MathBlock || b.kind == mepml::BlockKind::Comment ||
            b.kind == mepml::BlockKind::Table || b.kind == mepml::BlockKind::List ||
            b.kind == mepml::BlockKind::Abstract)
            add(b.line_start, b.line_end);
    }
}

bool Editor::MepmlSpliceResults(int buffer_id, int fence_row, const std::string &code, const std::string &output) {
    if (buffer_id < 0 || buffer_id >= static_cast<int>(buffers_.size())) return false;
    const Buffer &buf = buffers_[static_cast<size_t>(buffer_id)];
    const mepml::Document doc = mepml::Parse(buf.lines);
    for (const mepml::Block &b : doc.blocks) {
        if (b.kind != mepml::BlockKind::Code || b.code_line_start - 1 != fence_row) continue;
        if (b.code != code) return false;  // edited while it ran
        int first = 0, last = 0;
        mepml::ResultsReplaceRange(b, &first, &last);
        // results=html: the output is HTML, kept as such and rendered.
        std::vector<std::string> lines = mepml::FormatResults(output, mepml::ResultFormatFor(b));
        if (first == last && first > static_cast<int>(buf.lines.size())) first = last = static_cast<int>(buf.lines.size());
        ReplaceLinesAt(buffer_id, first, last, lines);
        return true;
    }
    return false;
}

// The LaTeX preview's fragments (kBuiltinOrgLatex renders them with
// tectonic), taken from the parse rather than OrgLatexScan's line
// heuristics: a `$` inside a code block or a results region is never
// math here, and a trailing \alttext() is hidden along with the formula
// it describes.
namespace {
// MepmlLatexFragments for any mepml text (the presentation's pages too).
OrgLatexFragments MepmlLatexFragmentsOf(const mepml::Document &doc, const std::vector<std::string> &lines) {
    OrgLatexFragments out;
    // Whether nothing but blanks shares the fragment's first line before it
    // or its last line after it.
    auto blank = [](const std::string &t) { return t.find_first_not_of(" \t") == std::string::npos; };
    auto MepmlOwnsRows = [&](mepml::Block::Pos p, mepml::Block::Pos q) {
        if (p.line < 0 || q.line >= static_cast<int>(lines.size())) return false;
        const std::string &first = lines[static_cast<size_t>(p.line)], &last = lines[static_cast<size_t>(q.line)];
        return blank(first.substr(0, std::min(first.size(), static_cast<size_t>(p.col)))) &&
               blank(last.substr(std::min(last.size(), static_cast<size_t>(q.col))));
    };
    std::function<void(const mepml::Block &, const std::vector<mepml::Inline> &)> walk =
        [&](const mepml::Block &b, const std::vector<mepml::Inline> &ins) {
            for (const mepml::Inline &x : ins) {
                if (x.kind == mepml::InlineKind::Math) {
                    OrgLatexInlineFragment f;
                    const bool display = x.arg == "display";
                    f.body = (display ? "\\[" : "$") + x.text + (display ? "\\]" : "$");
                    mepml::Block::Pos p = b.OffsetToPos(x.start), q = b.OffsetToPos(x.end);
                    // A fragment that spans lines and has them to itself (a
                    // `\(` alone on its line, the maths, then `\)`) is a
                    // display in all but name: rendered inline it was squeezed
                    // to one text row, with the rows under it left blank.
                    // Drawn as a block instead, like a `$$` one.
                    if (p.line != q.line && MepmlOwnsRows(p, q)) {
                        OrgLatexBlockFragment blk;
                        blk.start_row = p.line + 1;
                        blk.end_row = q.line + 1;
                        blk.body = std::move(f.body);
                        out.blocks.push_back(std::move(blk));
                        continue;
                    }
                    for (int line = p.line; line <= q.line; ++line) {
                        OrgLatexInlinePart part;
                        part.row = line + 1;
                        part.col_start = (line == p.line ? p.col : 0) + 1;
                        part.col_end = (line == q.line ? q.col : static_cast<int>(lines[static_cast<size_t>(line)].size())) + 1;
                        f.parts.push_back(part);
                    }
                    out.inlines.push_back(std::move(f));
                    continue;
                }
                walk(b, x.children);
            }
        };
    for (const mepml::Block &b : doc.blocks) {
        if (b.kind == mepml::BlockKind::MathBlock) {
            int end = b.line_end;
            if (b.caption_line >= 0) end = std::min(end, b.caption_line - 1);
            if (b.alt_line >= 0) end = std::min(end, b.alt_line - 1);
            OrgLatexBlockFragment f;
            f.start_row = b.line_start + 1;
            f.end_row = end + 1;
            f.body = "\\[" + b.code + "\\]";
            out.blocks.push_back(std::move(f));
            continue;
        }
        walk(b, b.inlines);
        walk(b, b.caption_inlines);
        for (const mepml::ListItem &it : b.items) walk(b, it.content);
        for (const auto &row : b.rows)
            for (const mepml::TableCell &c : row) walk(b, c.content);
    }
    return out;
}
}  // namespace

OrgLatexFragments Editor::MepmlLatexFragments() const {
    if (Buf().mepml_raw) return {};  // a raw document's maths stays source
    return MepmlLatexFragmentsOf(MepmlParseCurrent(false), Buf().lines);
}

std::vector<OrgLiteralSpan> Editor::MepmlLiteralSpans() const {
    std::vector<OrgLiteralSpan> out;
    constexpr std::uint32_t kLiteral = mepml::kCode | mepml::kResult | mepml::kVerbatim | mepml::kMath |
                                       mepml::kMeta | mepml::kDirective | mepml::kCite | mepml::kTableRule |
                                       mepml::kRule;
    // Markup covers a link's `|url]` and a command's `\color{red}{` too,
    // so a URL or a colour name is never checked as a word.
    for (const mepml::Span &s : MepmlSpansCurrent(false)) {
        if (!s.markup && !(s.style & kLiteral)) continue;
        OrgLiteralSpan sp;
        sp.row = s.line + 1;
        sp.col_start = s.col_start + 1;
        sp.col_end = s.col_end + 1;
        out.push_back(sp);
    }
    return out;
}

bool Editor::MepmlTablesStale() {
    if (!IsMepmlBuffer()) return false;
    auto it = mepml_scan_state_.find(CurrentBufferId());
    return it != mepml_scan_state_.end() && it->second.valid && it->second.table_math_gen != Buf().mepml_table_math_gen;
}

void Editor::MepmlTableLayout(const mepml::Document &doc, const std::vector<mepml::Span> &spans,
                              const std::unordered_set<int> &rows,
                              const std::set<std::pair<int, int>> &edge_markup, int ns,
                              const std::unordered_set<int> *only_tables) {
    Buffer &buf = Buf();
    const int n = buf.LineCount();
    const bool pictures = OrgImagesVisible();
    // Every table row's spans -- the cursor's too: its columns are
    // measured concealed like the rest, so the grid keeps its size when
    // the cursor steps onto a row and only that row shows its source.
    std::unordered_set<int> table_rows;
    for (const mepml::Block &b : doc.blocks)
        if (b.origin.empty() && b.kind == mepml::BlockKind::Table && (!only_tables || only_tables->count(b.line_start)))
            for (int row = b.line_start; row <= b.line_end; ++row) table_rows.insert(row);
    std::unordered_map<int, std::vector<const mepml::Span *>> by_line;
    for (const mepml::Span &sp : spans)
        if (table_rows.count(sp.line)) by_line[sp.line].push_back(&sp);

    // What a span occupies once concealed -- the same arithmetic DrawPane's
    // collapse does: hidden markup is 0, a replacement its own codepoints,
    // a scaled run StyledCols().
    auto span_width = [&](const mepml::Span &sp, const std::string &line) {
        return ConcealedWidth(sp, line, kTableMaxScale);
    };
    // A row's cells, split exactly as the parser splits them, and which of
    // its outer pipes it has: a GitHub-flavoured Markdown row may leave
    // either out (`a | b`), and the layout then draws the missing one.
    struct RowShape {
        std::vector<std::pair<int, int>> cells;  // [begin, end) byte ranges
        bool lead = false, trail = false;
        int first = 0;  // byte column of the leading pipe, or of the first cell
    };
    auto shape_of = [](const std::string &line) {
        RowShape r;
        r.cells = mepml::TableCells(line);
        if (r.cells.empty()) return r;
        const int b0 = r.cells.front().first, e1 = r.cells.back().second;
        r.lead = b0 > 0 && line[static_cast<size_t>(b0 - 1)] == '|';
        r.trail = e1 < static_cast<int>(line.size()) && line[static_cast<size_t>(e1)] == '|';
        r.first = r.lead ? b0 - 1 : b0;
        return r;
    };
    auto add = [&](Decoration d) {
        if (d.row < 0 || d.row >= n || d.col_end <= d.col_start) return;
        AddDecoration(ns, std::move(d));
    };
    // Inline maths is drawn as its render, at the render's own width --
    // not the TeX's -- so a cell holding some is as wide as that draws
    // (DrawPane's collapse), else every pipe after it lands off its rule.
    // The columns each fragment was measured at are recorded: a render
    // that lands later at another width lays the table out again.
    struct MathRun {
        int col_start, col_end, cols;
    };
    // Measured as if the cursor were elsewhere (its row's maths at their
    // render's width too), so the grid doesn't shift as it moves.
    auto math_runs = [&](int row) {
        std::vector<MathRun> out;
        if (!OrgLatexVisible()) return out;
        auto it = buf.org_latex_inline.find(row);
        if (it == buf.org_latex_inline.end()) return out;
        for (const Buffer::OrgLatexInlineSpan &sp : it->second) {
            if (sp.col_end <= sp.col_start) continue;
            // A fragment's continuation row draws nothing (its render is
            // on the row it starts on).
            const int cols = sp.path.empty() ? 0 : LatexInlineDrawCols(sp.path);
            if (cols >= 0) out.push_back({sp.col_start, sp.col_end, cols});
        }
        return out;
    };

    for (const mepml::Block &b : doc.blocks) {
        if (!b.origin.empty() || b.kind != mepml::BlockKind::Table) continue;
        // MepmlScan's patch lays out only the tables the cursor left or entered.
        if (only_tables && !only_tables->count(b.line_start)) continue;
        int body_end = b.line_end;
        if (b.caption_line >= 0) body_end = std::min(body_end, b.caption_line - 1);
        if (b.alt_line >= 0) body_end = std::min(body_end, b.alt_line - 1);
        if (b.rows_end >= 0) body_end = std::min(body_end, b.rows_end);
        // This table's record of its maths' widths starts afresh.
        for (auto it = buf.mepml_table_math_cols.lower_bound({b.line_start, -1});
             it != buf.mepml_table_math_cols.end() && it->first.first <= body_end;)
            it = buf.mepml_table_math_cols.erase(it);

        struct Cell {
            int ws_start, cs, ce, ws_end;  // segment [ws_start, ws_end), content [cs, ce)
            int width;
            std::string image;  // a picture cell's resolved path (`\image(...)`)
        };
        std::map<int, std::vector<Cell>> cells;
        std::map<int, RowShape> shapes;
        std::vector<int> widths;
        for (int row = b.line_start; row <= body_end && row < n; ++row) {
            const std::string &line = buf.lines[static_cast<size_t>(row)];
            const RowShape &shape = shapes[row] = shape_of(line);
            if (row == b.separator_line || shape.cells.empty()) continue;
            for (size_t k = 0; k < shape.cells.size(); ++k) {
                const int ws = shape.cells[k].first, we = shape.cells[k].second;
                Cell c{ws, ws, we, we, 0, {}};
                while (c.cs < c.ce && (line[static_cast<size_t>(c.cs)] == ' ' || line[static_cast<size_t>(c.cs)] == '\t')) ++c.cs;
                while (c.ce > c.cs && (line[static_cast<size_t>(c.ce - 1)] == ' ' || line[static_cast<size_t>(c.ce - 1)] == '\t')) --c.ce;
                // A picture cell has no text of its own on the grid: its
                // column is sized for the picture below, on every row
                // (the cursor's too, so the grid never shifts under it).
                std::string img;
                if (pictures &&
                    mepml::ResultImagePath(line.substr(static_cast<size_t>(c.cs), static_cast<size_t>(c.ce - c.cs)), &img)) {
                    std::string resolved = OrgResolvePath(img);
                    std::error_code ec;
                    if (std::filesystem::exists(resolved, ec)) c.image = std::move(resolved);
                }
                // Width measured from the concealed spans -- on the
                // cursor's row too, which draws as typed and may overrun
                // its cell rather than widen the grid under it.
                auto it = by_line.find(row);
                if (!c.image.empty()) {
                    c.width = 0;
                } else if (it != by_line.end()) {
                    const std::vector<MathRun> maths = math_runs(row);
                    auto in_math = [&](const mepml::Span &sp) {
                        for (const MathRun &m : maths)
                            if (sp.col_start >= m.col_start && sp.col_end <= m.col_end) return true;
                        return false;
                    };
                    for (const mepml::Span *sp : it->second)
                        if (sp->col_start >= c.cs && sp->col_end <= c.ce && !in_math(*sp)) c.width += span_width(*sp, line);
                    for (const MathRun &m : maths)
                        if (m.col_start >= c.cs && m.col_end <= c.ce) c.width += m.cols;
                    // Every fragment in the row, rendered or not yet.
                    for (const mepml::Span *sp : it->second) {
                        if (!(sp->style & mepml::kMath) || sp->markup) continue;
                        int used = -1;
                        int start = sp->col_start;
                        for (const MathRun &m : maths)
                            if (sp->col_start >= m.col_start && sp->col_end <= m.col_end) used = m.cols, start = m.col_start;
                        if (used < 0) {
                            // Unrendered: keyed by where its fragment (with
                            // its opening `$` or `\(`) starts.
                            start = sp->col_start;
                            while (start > 0 && (line[static_cast<size_t>(start - 1)] == '$' || line[static_cast<size_t>(start - 1)] == '(' ||
                                                 line[static_cast<size_t>(start - 1)] == '\\'))
                                --start;
                        }
                        buf.mepml_table_math_cols[{row, start}] = used;
                    }
                } else {
                    c.width = Codepoints(line.substr(static_cast<size_t>(c.cs), static_cast<size_t>(c.ce - c.cs)));
                }
                if (widths.size() <= k) widths.resize(k + 1, 0);
                widths[k] = std::max(widths[k], c.width);
                cells[row].push_back(c);
            }
        }
        if (widths.empty()) continue;
        // Picture columns share what the text width leaves once the text
        // columns and the pipes have theirs.
        {
            std::vector<bool> picture_col(widths.size(), false);
            for (const auto &kv : cells)
                for (size_t k = 0; k < kv.second.size(); ++k)
                    if (!kv.second[k].image.empty()) picture_col[k] = true;
            const int npic = static_cast<int>(std::count(picture_col.begin(), picture_col.end(), true));
            if (npic > 0) {
                int avail = TextWidth();
                if (CurPane().text_cols > 8) avail = std::min(avail, CurPane().text_cols - 2);
                const RowShape &s0 = shapes[b.line_start];
                int used = 1 + 3 * static_cast<int>(widths.size());
                if (!s0.cells.empty()) used += Codepoints(buf.lines[static_cast<size_t>(b.line_start)].substr(0, static_cast<size_t>(s0.first)));
                for (size_t k = 0; k < widths.size(); ++k)
                    if (!picture_col[k]) used += widths[k];
                constexpr int kMinPictureCols = 8;
                // ...but no wider than half the text: a lone picture
                // column is a thumbnail beside its row, not a figure.
                const int share = std::max(kMinPictureCols, std::min((avail - used) / npic, avail / 2));
                for (size_t k = 0; k < widths.size(); ++k)
                    if (picture_col[k]) widths[k] = std::max(widths[k], share);
            }
        }
        // The grid DrawPane's org table pass draws (outline, header wash,
        // zebra stripes, rules), in the display columns the layout below
        // puts every pipe at.
        {
            OrgTableGrid g;
            g.start_row = b.line_start;
            g.end_row = std::min(body_end, n - 1);
            const RowShape &s0 = shapes[b.line_start];
            g.indent = s0.cells.empty() ? 0 : Codepoints(buf.lines[static_cast<size_t>(b.line_start)].substr(0, static_cast<size_t>(s0.first)));
            int col = g.indent;
            g.rule_cols.push_back(col);
            for (int w : widths) {
                col += w + 3;
                g.rule_cols.push_back(col);
            }
            g.width = col - g.indent + 1;
            if (b.separator_line >= 0) {
                g.sep_rows.push_back(b.separator_line);
                g.header_end_row = b.separator_line - 1;
            }
            for (int row = g.start_row; row <= g.end_row; ++row)
                if (!rows.count(row)) g.raw_rows.push_back(row);
            mepml_table_grids_[CurrentBufferId()].push_back(g);
            // Soft-wrap measures the laid-out rows by the grid they draw
            // across (Editor::WrapLenForRow); the cursor's raw row by its text.
            for (int row = g.start_row; row <= g.end_row; ++row) {
                if (rows.count(row)) buf.mepml_table_row_cols[row] = g.indent + g.width;
                else buf.mepml_table_row_cols.erase(row);
            }
            // Each picture in its cell's content box (after the `| `).
            for (const auto &kv : cells) {
                Buffer::MepmlTableImageRow pics;
                int at = g.indent;
                for (size_t k = 0; k < kv.second.size() && k < widths.size(); ++k) {
                    const Cell &c = kv.second[k];
                    if (!c.image.empty()) {
                        Buffer::MepmlTableImage img;
                        img.path = c.image;
                        ImagePixelSizeCached(img.path, &img.width, &img.height);
                        img.col = at + 2;
                        img.cols = widths[k];
                        pics.cells.push_back(std::move(img));
                    }
                    at += widths[k] + 3;
                }
                if (pics.cells.empty()) continue;
                pics.text_hash = std::hash<std::string>{}(buf.lines[static_cast<size_t>(kv.first)]);
                buf.mepml_table_images[kv.first] = std::move(pics);
            }
        }
        auto align_of = [&](size_t k) {
            return k < b.aligns.size() ? b.aligns[k] : mepml::Align::Default;
        };

        for (int row = b.line_start; row <= body_end && row < n; ++row) {
            if (!rows.count(row)) continue;
            const std::string &line = buf.lines[static_cast<size_t>(row)];
            const RowShape &shape = shapes[row];
            if (shape.cells.empty()) continue;
            if (row == b.separator_line) {
                // Blank: the grid pass draws the rule across the table.
                std::string rule = " ";
                for (size_t k = 0; k < widths.size(); ++k) rule += std::string(static_cast<size_t>(widths[k]) + 3, ' ');
                Decoration d;
                d.row = row;
                d.col_start = shape.first;
                d.col_end = static_cast<int>(line.size());
                d.virt_overlay = true;
                d.virt_text = rule;
                d.virt_text_hl = "Comment";
                d.priority = 10;
                add(d);
                continue;
            }
            const std::vector<Cell> &cs = cells[row];
            // On a row of pictures the pipes would stand alone in empty
            // space beside the grid's own rules, so only the rules draw.
            const bool picture_row = std::any_of(cs.begin(), cs.end(), [](const Cell &c) { return !c.image.empty(); });
            // Padding each cell needs before / after its content.
            std::vector<int> before(widths.size(), 0), after(widths.size(), 0);
            for (size_t k = 0; k < widths.size(); ++k) {
                const int pad = widths[k] - (k < cs.size() ? cs[k].width : 0);
                switch (align_of(k)) {
                    case mepml::Align::Right: before[k] = pad; break;
                    case mepml::Align::Center: before[k] = pad / 2; after[k] = pad - pad / 2; break;
                    default: after[k] = pad; break;
                }
            }
            // One pipe before each cell and one after the last: `k` is the
            // boundary before cell k (k == cells: after the last one).
            const size_t nb = shape.cells.size() + 1;
            // Is [at, at+len) inside some span the scan conceals or draws
            // its own way (markup, maths, a directive)? A drawn pipe must
            // not take that span's place.
            auto claimed = [&](int at, int len) {
                auto it = by_line.find(row);
                if (it == by_line.end()) return false;
                for (const mepml::Span *sp : it->second)
                    if ((sp->markup || (sp->style & (mepml::kMath | mepml::kDirective))) && sp->col_start < at + len &&
                        sp->col_end > at)
                        return true;
                return false;
            };
            // Bytes in the UTF-8 codepoint starting at `at`.
            auto cp_len = [&](int at) {
                const unsigned char c = static_cast<unsigned char>(line[static_cast<size_t>(at)]);
                const int len = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
                return std::min(len, static_cast<int>(line.size()) - at);
            };
            for (size_t k = 0; k < nb; ++k) {
                std::string text;
                if (k > 0 && k - 1 < widths.size()) text += std::string(static_cast<size_t>(after[k - 1]), ' ') + " ";
                // Blank: the grid pass draws the column rules and the
                // outline at exactly these columns, as unbroken lines; a
                // drawn `|` beside them only read as a stray glyph.
                (void)picture_row;
                text += " ";
                if (k < cs.size()) text += " " + std::string(static_cast<size_t>(before[k]), ' ');
                // A short row: draw its missing cells after the last pipe.
                if (k + 1 == nb)
                    for (size_t m = cs.size(); m < widths.size(); ++m)
                        text += " " + std::string(static_cast<size_t>(widths[m]), ' ') + "  ";
                Decoration d;
                d.row = row;
                d.virt_overlay = true;
                d.virt_text_hl = "Comment";
                d.priority = 10;
                const bool real = (k > 0 && k + 1 < nb) || (k == 0 && shape.lead) || (k + 1 == nb && shape.trail);
                if (real) {
                    d.col_start = k == 0 ? shape.first : shape.cells[k - 1].second;
                    d.col_end = d.col_start + 1;
                    d.virt_text = text;
                    add(d);
                    continue;
                }
                // A GFM row without this outer pipe: it is drawn on the
                // cell's first (or last) character, which the overlay then
                // repeats in the text's own colour -- unless that character
                // belongs to something drawn its own way, when the row
                // keeps its raw edge there.
                const Cell *c = nullptr;
                if (k == 0 && !cs.empty()) c = &cs.front();
                if (k + 1 == nb && k > 0 && k - 1 < cs.size()) c = &cs[k - 1];
                if (!c || c->ce <= c->cs || !c->image.empty()) continue;
                // Markup on that edge (the render pass left it to us): the
                // pipe takes its place, beside whatever it is replaced by.
                const mepml::Span *edge = nullptr;
                if (auto it = by_line.find(row); it != by_line.end())
                    for (const mepml::Span *sp : it->second)
                        if (edge_markup.count({row, sp->col_start}) && (k == 0 ? sp->col_start == c->cs : sp->col_end == c->ce))
                            edge = sp;
                if (edge) {
                    d.col_start = edge->col_start;
                    d.col_end = edge->col_end;
                    d.virt_text = k == 0 ? text + edge->replace : edge->replace + text;
                    add(d);
                    continue;
                }
                const int at = k == 0 ? c->cs : c->ce - 1;
                int start = at;
                while (k != 0 && start > c->cs && (static_cast<unsigned char>(line[static_cast<size_t>(start)]) & 0xc0) == 0x80) --start;
                const int len = cp_len(start);
                if (claimed(start, len)) continue;
                const std::string ch = line.substr(static_cast<size_t>(start), static_cast<size_t>(len));
                d.col_start = start;
                d.col_end = start + len;
                d.virt_text = k == 0 ? text + ch : ch + text;
                d.virt_text_hl = "";
                add(d);
            }
            // Past a missing trailing pipe, the row's own trailing spaces.
            if (!shape.trail && shape.cells.back().second < static_cast<int>(line.size())) {
                Decoration d;
                d.row = row;
                d.col_start = shape.cells.back().second;
                d.col_end = static_cast<int>(line.size());
                d.virt_overlay = true;
                d.conceal = true;
                d.priority = 10;
                add(d);
            }
            // The source's own spacing around each cell is replaced by the
            // padding above, so it is hidden -- as is a picture cell's
            // `\image(...)`, the picture being drawn above the row.
            for (const Cell &c : cs) {
                if (!c.image.empty()) {
                    Decoration d;
                    d.row = row;
                    d.col_start = c.cs;
                    d.col_end = c.ce;
                    d.virt_overlay = true;
                    d.conceal = true;
                    d.priority = 10;
                    add(d);
                }
                for (auto range : {std::make_pair(c.ws_start, c.cs), std::make_pair(c.ce, c.ws_end)}) {
                    if (range.second <= range.first) continue;
                    Decoration d;
                    d.row = row;
                    d.col_start = range.first;
                    d.col_end = range.second;
                    d.virt_overlay = true;
                    d.conceal = true;
                    d.priority = 10;
                    add(d);
                }
            }
        }
    }
}

void Editor::MepmlBuildCards(const mepml::Document &doc) {
    std::vector<OrgBlockCard> &cards = mepml_block_cards_[CurrentBufferId()];
    cards.clear();
    const Buffer &buf = Buf();
    const int n = buf.LineCount();
    auto widest = [&](int from, int to) {
        int cols = 0;
        for (int r = std::max(0, from); r <= to && r < n; ++r)
            cols = std::max(cols, Codepoints(buf.lines[static_cast<size_t>(r)]));
        return cols;
    };
    auto trimmed = [&](int r) {
        if (r < 0 || r >= n) return std::string();
        const std::string &l = buf.lines[static_cast<size_t>(r)];
        const size_t a = l.find_first_not_of(" \t");
        const size_t z = l.find_last_not_of(" \t");
        return a == std::string::npos ? std::string() : l.substr(a, z - a + 1);
    };
    // A slide: a card round everything on it, its `\slide(` line the
    // title bar ("Slide N" and the slide's title), its closing `)` the
    // floor. Pushed first, so its wash and outline go down under the cards
    // of the blocks it holds (DrawPane insets those inside it). A slide
    // still being typed (no closer yet) keeps its rules instead: it would
    // otherwise swallow every slide after it.
    for (const mepml::Slide &sl : mepml::Slides(doc, n)) {
        if (!sl.closed) continue;
        OrgBlockCard card;
        card.meta_row = card.begin_row = sl.line_start;
        card.end_row = sl.line_end;
        card.kind = "slide";
        card.chip = "Slide " + std::to_string(sl.number);
        card.title = sl.title;
        card.content_cols = widest(sl.line_start, sl.line_end);
        card.fold_row = sl.line_start;
        cards.push_back(std::move(card));
    }
    for (const HeaderRun &run : HeaderRuns(doc)) {
        OrgBlockCard head;
        head.meta_row = head.begin_row = run.first;
        head.end_row = run.last;
        head.kind = "header";
        head.bare = true;
        head.content_cols = widest(run.first, run.last);
        cards.push_back(std::move(head));
    }
    // A box (\definition ...): a plain card in its kind's colour from its
    // opening line to its closing `)` -- the same line for one closed
    // where its text ends.
    {
        std::set<int> math_rows;
        for (const mepml::Block &b : doc.blocks)
            if (b.origin.empty() && b.kind == mepml::BlockKind::MathBlock)
                for (int r = b.line_start; r <= b.line_end; ++r) math_rows.insert(r);
        std::vector<size_t> open;
        for (size_t i = 0; i < doc.blocks.size(); ++i) {
            const mepml::Block &b = doc.blocks[i];
            if (!b.origin.empty()) continue;
            int first = -1, last = -1;
            const mepml::Block *begin = nullptr;
            if (b.kind == mepml::BlockKind::BoxBegin && b.box_closed) {
                begin = &b;
                first = b.line_start;
                last = b.line_end;
            } else if (b.kind == mepml::BlockKind::BoxBegin) {
                open.push_back(i);
                continue;
            } else if (b.kind == mepml::BlockKind::BoxEnd && !open.empty()) {
                begin = &doc.blocks[open.back()];
                open.pop_back();
                first = begin->line_start;
                last = b.line_end;
            } else {
                continue;
            }
            const mepml::BoxKind *kind = mepml::FindBoxKind(begin->keyword);
            OrgBlockCard card;
            card.meta_row = card.begin_row = first;
            card.end_row = last;
            card.kind = "box";
            card.bare = true;
            card.tint = kind ? kind->hl : "Cyan";
            // Folded, it collapses to a bar naming it.
            card.chip = kind ? kind->label : begin->keyword;
            card.title = mepml::InlinePlainText(begin->caption_inlines);
            if (last > first) card.fold_row = first;
            // As wide as its text as drawn -- markup concealed, maths about as
            // wide as it typesets -- not as its source: display maths' TeX
            // and inline markup are far wider than what is on screen.
            for (int r = first; r <= last; ++r)
                if (!math_rows.count(r))
                    card.content_cols = std::max(card.content_cols, mepml::ProseColumns(buf.lines[static_cast<size_t>(r)]));
            cards.push_back(std::move(card));
        }
    }
    // An abstract: a plain card of its own behind its prose.
    for (const mepml::Block &b : doc.blocks) {
        if (!b.origin.empty() || b.kind != mepml::BlockKind::Abstract) continue;
        OrgBlockCard card;
        card.meta_row = card.begin_row = b.line_start;
        card.end_row = b.line_end;
        card.kind = "abstract";
        card.bare = true;
        card.content_cols = widest(b.line_start, b.line_end);
        cards.push_back(std::move(card));
    }
    for (const mepml::Block &b : doc.blocks) {
        if (!b.origin.empty() || b.kind != mepml::BlockKind::Code) continue;
        // The code itself: its `//?` option lines and ``` header become the
        // title bar (language chip, one chip per option, play button).
        OrgBlockCard code;
        // The card starts at the fence: `//?` option lines above it stay
        // ordinary metadata lines rather than a tall, empty header band.
        code.begin_row = code.meta_row = b.code_line_start - 1;
        const int close = b.code_line_end + 1;
        code.end_row = trimmed(close) == "```" ? close : -1;
        code.kind = "src";
        code.is_src = true;
        code.lang = b.lang;
        // A figure's caption belongs under the figure; any other block's
        // titles its card.
        code.title = b.result_images.empty() ? b.caption : std::string();
        for (const mepml::Option &o : b.options)
            if (o.line == code.begin_row) code.options.push_back({o.name, o.value.s, o.line});
        code.content_cols = widest(code.meta_row, code.end_row >= 0 ? code.end_row : n - 1);
        int run_id = -1;
        for (const auto &kv : mepml_terms_)
            if (kv.second.buffer_id == CurrentBufferId() && kv.second.fence_row == code.begin_row && kv.second.code == b.code)
                run_id = kv.first;
        for (const auto &kv : mepml_guis_)
            if (kv.second.buffer_id == CurrentBufferId() && kv.second.fence_row == code.begin_row && kv.second.code == b.code)
                run_id = kv.first;
        code.term_run = run_id;
        code.fold_row = b.line_start;  // RecomputeMepmlFolds folds the option lines too
        cards.push_back(std::move(code));
        // Its results: a card of their own, the `// result_begin:` and
        // `// result_end` markers standing in as its header and floor.
        if (b.result_line_start >= 0) {
            OrgBlockCard out;
            out.meta_row = out.begin_row = b.result_line_start;
            out.end_row = b.result_line_end > b.result_line_start ? b.result_line_end : -1;
            out.kind = b.result_format == "html"       ? "html"
                       : b.result_format == "terminal" ? "terminal"
                       : b.result_format == "gui"      ? "gui"
                                                       : "output";
            out.is_src = false;
            out.term_run = run_id;
            if (b.result_line_end > b.result_line_start) out.fold_row = b.result_line_start;
            // Rendered HTML is laid out to the text width; its source's
            // long lines must not widen the card.
            // Nor may a Markdown result's raw rows (they are laid out too).
            out.content_cols = b.result_format == "html" || b.result_format == "markdown" || run_id >= 0
                                   ? 0
                                   : widest(out.begin_row, b.result_line_end);
            cards.push_back(std::move(out));
        }
    }
}

void Editor::MepmlFitCards() {
    auto cit = mepml_block_cards_.find(CurrentBufferId());
    if (cit == mepml_block_cards_.end()) return;
    std::vector<OrgBlockCard> &cards = cit->second;
    const Buffer &buf = Buf();
    const int n = buf.LineCount();
    static const std::vector<OrgTableGrid> kNoGrids;
    auto git = mepml_table_grids_.find(CurrentBufferId());
    const std::vector<OrgTableGrid> &grids = git == mepml_table_grids_.end() ? kNoGrids : git->second;
    // A row as drawn: a laid-out table row spans its whole grid (its
    // padded cells can run wider than the markup), anything else its text.
    auto row_cols = [&](int r) {
        for (const OrgTableGrid &g : grids) {
            if (r < g.start_row || r > g.end_row) continue;
            if (std::find(g.raw_rows.begin(), g.raw_rows.end(), r) != g.raw_rows.end()) break;
            return std::max(g.indent + g.width, Codepoints(buf.lines[static_cast<size_t>(r)]));
        }
        return Codepoints(buf.lines[static_cast<size_t>(r)]);
    };
    for (size_t i = 0; i < cards.size(); ++i) {
        OrgBlockCard &code = cards[i];
        if (!code.is_src || code.bare) continue;
        // MepmlBuildCards pushes a block's results card right after its code.
        if (i + 1 >= cards.size() || cards[i + 1].is_src || cards[i + 1].bare) continue;
        OrgBlockCard &out = cards[i + 1];
        // Plain output and Markdown results (tables, prose) are drawn row by
        // row; HTML, a terminal and a program's window are laid out to fit.
        if (out.kind == "output" && out.term_run < 0) {
            const int last = out.end_row >= 0 ? out.end_row : n - 1;
            for (int r = std::max(0, out.begin_row); r <= last && r < n; ++r)
                out.content_cols = std::max(out.content_cols, row_cols(r));
        }
        // A block and its results line up on one right edge.
        code.content_cols = out.content_cols = std::max(code.content_cols, out.content_cols);
    }
    // A slide (or a box) is at least as wide as the widest card it holds.
    for (OrgBlockCard &slide : cards) {
        if (slide.kind != "slide" && slide.kind != "box") continue;
        for (const OrgBlockCard &c : cards)
            if (&c != &slide && c.meta_row > slide.begin_row && c.end_row >= 0 && c.end_row < slide.end_row)
                slide.content_cols = std::max(slide.content_cols, c.content_cols);
    }
}

// ---------------------------------------------------------------------------
// Terminals inside mepml results

int Editor::MepmlTerminalStart(int buffer_id, int fence_row, const std::vector<std::string> *argv,
                               const std::vector<std::string> &temp_files) {
    // The prepared program's files go once it has run, or now if it never does.
    auto drop_temps = [&temp_files] {
        for (const std::string &f : temp_files) {
            std::error_code ec;
            std::filesystem::remove(f, ec);
        }
    };
    if (buffer_id < 0 || buffer_id >= static_cast<int>(buffers_.size())) {
        drop_temps();
        return -1;
    }
    const Buffer &buf = buffers_[static_cast<size_t>(buffer_id)];
    const mepml::Document doc = mepml::Parse(buf.lines);
    const mepml::Block *blk = nullptr;
    for (const mepml::Block &b : doc.blocks)
        if (b.kind == mepml::BlockKind::Code && b.code_line_start - 1 == fence_row) blk = &b;
    std::string shell;
    if (!blk || (argv ? argv->empty() : !TerminalShellFor(*blk, &shell))) {
        status_message_ = "Not a block that runs in a terminal (```exec, results=exec, or a shell block with results=terminal)";
        drop_temps();
        return -1;
    }
    std::string command = blk->code;
    while (!command.empty() && (command.back() == '\n' || command.back() == ' ')) command.pop_back();
    if (!argv && command.find_first_not_of(" \t\n") == std::string::npos) {
        status_message_ = "The block has nothing to run";
        return -1;
    }
    for (const auto &kv : mepml_terms_) {
        if (kv.second.buffer_id == buffer_id && kv.second.fence_row == fence_row && !kv.second.sess.exited) {
            status_message_ = "Already running (C-c C-k stops it)";
            drop_temps();
            return -1;
        }
    }
    // The terminal's size: rows=/cols= options, else 16 rows by the text
    // width (or the pane's, when that is narrower).
    int cols = TextWidth() - 2;
    if (CurPane().text_cols > 8) cols = std::min(cols, CurPane().text_cols - 4);
    const int rows = std::clamp(IntOption(*blk, "rows", 16), 3, 100);
    cols = std::clamp(IntOption(*blk, "cols", cols), 20, 400);

    const int id = next_mepml_term_++;
    MepmlTermRun &run = mepml_terms_[id];
    run.buffer_id = buffer_id;
    run.code = blk->code;
    run.fence_row = fence_row;
    run.rows = rows;
    run.cols = cols;
    run.sess.buffer_id = buffer_id;
    run.sess.vterm = std::make_unique<VTerm>(rows, cols);
    run.sess.last_rows = rows;
    run.sess.last_cols = cols;
    ThemeColor fg, bg;
    ResolveHighlight("Normal", &fg);
    ResolveHighlight("NormalBg", &bg);
    run.sess.vterm->SetOscDefaultColors(VTermColor{VTermColorKind::Rgb, 0, fg.r, fg.g, fg.b},
                                        VTermColor{VTermColorKind::Rgb, 0, bg.r, bg.g, bg.b});
#if defined(__EMSCRIPTEN__)
    mepml_terms_.erase(id);
    drop_temps();
    status_message_ = "Running a program in a terminal needs the desktop build";
    return -1;
#else
    VTerm *vt = run.sess.vterm.get();
    JobManager::Callbacks cb;
    // Output goes straight into the screen (the block may be scrolled out
    // of view, but it is small, and a TUI that blocks on a full pty would
    // look hung the moment it came back).
    cb.on_stdout_raw = [this, id, vt](const std::string &chunk) {
        const std::string reply = vt->Feed(chunk);
        if (reply.empty()) return;
        auto it = mepml_terms_.find(id);
        if (it != mepml_terms_.end()) TerminalWrite(it->second.sess, reply);
    };
    cb.on_exit = [this, id](int code) {
        auto it = mepml_terms_.find(id);
        if (it == mepml_terms_.end()) return;
        it->second.sess.exited = true;
        it->second.sess.exit_code = code;
    };
    std::vector<std::pair<std::string, std::string>> env = {{"TERM", "xterm-256color"}, {"COLORTERM", "truecolor"},
                                                            {"LINES", std::to_string(rows)}, {"COLUMNS", std::to_string(cols)}};
    std::string cwd = ActiveRoot();
    if (!buf.filename.empty()) {
        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::absolute(buf.filename, ec).parent_path();
        if (!ec && !dir.empty()) cwd = dir.string();
    }
    std::vector<std::string> spawn_argv = argv ? *argv : std::vector<std::string>{shell, "-c", command};
    const std::string program = spawn_argv.front();
    run.sess.job_id = JobManager::Instance().Spawn(std::move(spawn_argv), cwd, std::move(cb), /*use_pty=*/true, std::move(env));
    if (run.sess.job_id == 0) {
        mepml_terms_.erase(id);
        status_message_ = "Could not start " + program;
        drop_temps();
        return -1;
    }
    run.temp_files = temp_files;
    JobManager::Instance().ResizePty(run.sess.job_id, cols, rows);
#endif
    // Its results: a placeholder the terminal is drawn over while it runs.
    int first = 0, last = 0;
    mepml::ResultsReplaceRange(*blk, &first, &last);
    if (first == last && first > static_cast<int>(buf.lines.size())) first = last = static_cast<int>(buf.lines.size());
    // (A results=exec block's body is a program, not a command: named by
    // its language instead.)
    const std::string label = argv ? blk->lang + " program" : command.substr(0, command.find('\n'));
    ReplaceLinesAt(buffer_id, first, last, {"// result_begin: terminal", "// [running] " + label, "// result_end"});
    // Its extent until the next MepmlScan: the stop key works on any of it.
    auto started = mepml_terms_.find(id);
    if (started != mepml_terms_.end()) started->second.results_end = first + 2;
    status_message_ = "Running " + label + " -- Enter to type into it, C-c C-k to stop it";
    return id;
}

bool Editor::MepmlTerminalStop(int run_id) {
    auto gui = mepml_guis_.find(run_id);
    if (gui != mepml_guis_.end()) {
        mep::gui_embed::EmbeddedApp &app = *gui->second.app;
        app.Stop();
        status_message_ = app.Stops() == 1   ? "Asked it to close (again to stop it)"
                          : app.Stops() == 2 ? "Stopping... (again to kill it)"
                                             : "Killed";
        return true;
    }
    auto it = mepml_terms_.find(run_id);
    if (it == mepml_terms_.end()) return false;
    MepmlTermRun &run = it->second;
    if (run.sess.exited || run.sess.job_id <= 0) return true;
    // Politely first (SIGTERM to its process group); a program that
    // ignores that is killed outright the second time.
    if (run.stops++ == 0) {
        JobManager::Instance().Kill(run.sess.job_id);
        status_message_ = "Stopping... (again to kill it)";
    } else {
        JobManager::Instance().KillHard(run.sess.job_id);
        status_message_ = "Killed";
    }
    return true;
}

int Editor::MepmlTerminalRunAt(int row) const {
    for (const auto &kv : mepml_terms_) {
        const MepmlTermRun &run = kv.second;
        if (run.buffer_id != CurrentBufferId() || run.sess.exited) continue;
        if (row >= run.fence_row && row <= std::max(run.fence_row + 1, run.results_end)) return kv.first;
    }
    for (const auto &kv : mepml_guis_) {
        const MepmlGuiRun &run = kv.second;
        if (run.buffer_id != CurrentBufferId()) continue;
        if (row >= run.fence_row && row <= std::max(run.fence_row + 1, run.results_end)) return kv.first;
    }
    return -1;
}

bool Editor::MepmlTerminalFocus(int run_id) {
    auto gui = mepml_guis_.find(run_id);
    if (gui != mepml_guis_.end()) {
        mep::gui_embed::EmbeddedApp &app = *gui->second.app;
        // (Still true when there is nothing to focus yet: the key or click
        // was meant for the program, not for the document.)
        if (app.GetState() != mep::gui_embed::EmbeddedApp::State::Shown || !app.Focus(true)) {
            status_message_ = "Its window is not showing yet";
            return true;
        }
        mepml_gui_focus_ = run_id;
        const std::string name = app.Title().empty() ? gui->second.label : app.Title();
        status_message_ = "Typing into " + name + " -- Ctrl-\\ (or a click outside it) returns to the document";
        return true;
    }
    auto it = mepml_terms_.find(run_id);
    if (it == mepml_terms_.end() || it->second.sess.exited) return false;
    mepml_term_focus_ = run_id;
    terminal_pending_ctrl_bs_ = false;
    mode_ = Mode::Terminal;
    status_message_ = "Typing into the program -- Ctrl-\\ Ctrl-N returns to the document";
    return true;
}

const TerminalSession *Editor::MepmlTerminalSession(int run_id) const {
    auto it = mepml_terms_.find(run_id);
    return it == mepml_terms_.end() ? nullptr : &it->second.sess;
}

namespace {
// A terminal's screen as lines of text, leading and trailing blank lines dropped.
std::vector<std::string> ScreenText(const VTerm &vt) {
    std::vector<std::string> screen;
    for (int r = 0; r < vt.Rows(); ++r) {
        std::string line;
        for (int c = 0; c < vt.Cols(); ++c) {
            const VTermCell &cell = vt.At(r, c);
            if (cell.width == 0) continue;
            line += cell.ch.empty() ? std::string(" ") : cell.ch;
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        screen.push_back(line);
    }
    while (!screen.empty() && screen.back().empty()) screen.pop_back();
    while (!screen.empty() && screen.front().empty()) screen.erase(screen.begin());
    return screen;
}
// A results=exec run's source and binary, once nothing will run them again.
void RemoveTempFiles(const std::vector<std::string> &files) {
    for (const std::string &f : files) {
        std::error_code ec;
        std::filesystem::remove(f, ec);
    }
}
}  // namespace

void Editor::MepmlTerminalsTick() {
    for (auto it = mepml_terms_.begin(); it != mepml_terms_.end();) {
        MepmlTermRun &run = it->second;
        if (run.buffer_id < 0 || run.buffer_id >= static_cast<int>(buffers_.size())) {
            if (!run.sess.exited && run.sess.job_id > 0) JobManager::Instance().KillHard(run.sess.job_id);
            RemoveTempFiles(run.temp_files);
            it = mepml_terms_.erase(it);
            continue;
        }
        if (!run.sess.exited) {
            if (run.sess.vterm->AltScreenActive()) run.alt_snapshot = ScreenText(*run.sess.vterm);
            ++it;
            continue;
        }
        // Finished: its last screen becomes the block's results, as text --
        // a full-screen program's last frame when it left nothing behind.
        std::vector<std::string> screen = ScreenText(*run.sess.vterm);
        if (screen.empty()) screen = run.alt_snapshot;
        if (run.stops > 0) screen.push_back("[stopped]");
        else if (run.sess.exit_code != 0) screen.push_back("[exit status " + std::to_string(run.sess.exit_code) + "]");
        std::string text;
        for (size_t k = 0; k < screen.size(); ++k) text += (k ? "\n" : "") + screen[k];
        const Buffer &buf = buffers_[static_cast<size_t>(run.buffer_id)];
        const mepml::Document doc = mepml::Parse(buf.lines);
        if (const mepml::Block *b = BlockForRun(doc, run.code, run.fence_row)) {
            int first = 0, last = 0;
            mepml::ResultsReplaceRange(*b, &first, &last);
            ReplaceLinesAt(run.buffer_id, first, last, mepml::FormatResults(text, "terminal"));
        }
        if (mepml_term_focus_ == it->first) {
            mepml_term_focus_ = -1;
            if (mode_ == Mode::Terminal) mode_ = Mode::Normal;
        }
        RemoveTempFiles(run.temp_files);
        it = mepml_terms_.erase(it);
    }
    MepmlGuisTick();
}

// ---------------------------------------------------------------------------
// GUI programs inside mepml results

// main.cpp's inline-image cache: a re-run's picture must be reloaded.
void InvalidateOrgInlineImageTexture(const std::string &path);

namespace {
std::string StrOption(const mepml::Block &b, const char *name) {
    for (const mepml::Option &o : b.options)
        if (Lowered(o.name) == name) return o.value.s;
    return "";
}

// A short, stable name for a block's picture: its code, hashed (FNV-1a).
std::string CodeHash(const std::string &code) {
    uint32_t h = 2166136261u;
    for (char c : code) h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x", h);
    return buf;
}

// Where a program window's picture is kept: file= (relative to the
// document), else beside the document under a name its code decides.
// `*ref` is how the results name it, `*path` where it is (absolute).
void GuiSnapshotPaths(const mepml::Block &b, const std::filesystem::path &doc_file, std::string *ref, std::string *path) {
    *ref = StrOption(b, "file");
    if (ref->empty()) *ref = doc_file.stem().string() + "-gui-" + CodeHash(b.code) + ".png";
    const std::filesystem::path p(*ref);
    *path = (p.is_absolute() ? p : doc_file.parent_path() / p).lexically_normal().string();
}
}  // namespace

mepml::Document Editor::MepmlParseForExport(const std::vector<std::string> &tags) const {
    const std::string file = MepmlCurrentFile();
    if (file.empty()) return mepml::ParseForExport(file, Buf().lines, ReadFileLines, tags);
    // A program still running in its window (a web app, a GUI) has only
    // "[running]" for results: an export shows a picture of it instead --
    // taken now while it runs, else the last one kept on disk.
    std::vector<std::string> lines = Buf().lines;
    const mepml::Document doc = mepml::Parse(lines);
    for (auto b = doc.blocks.rbegin(); b != doc.blocks.rend(); ++b) {
        if (!b->origin.empty() || b->kind != mepml::BlockKind::Code || b->result_format != "gui") continue;
        std::string ref, path;
        GuiSnapshotPaths(*b, file, &ref, &path);
        for (const auto &kv : mepml_guis_) {
            const MepmlGuiRun &run = kv.second;
            if (run.buffer_id != CurrentBufferId() || !run.app || BlockForRun(doc, run.code, run.fence_row) != &*b) continue;
            const mep::gui_embed::Snapshot &snap = run.app->LastSnapshot();
            if (snap.Empty() || run.snapshot_path.empty()) break;
            const std::string png = png::Encode(snap.width, snap.height, 4, snap.rgba.data(), snap.width * 4);
            std::ofstream f(run.snapshot_path, std::ios::binary);
            if (!png.empty() && f.write(png.data(), static_cast<std::streamsize>(png.size()))) {
                ref = run.snapshot_ref;
                path = run.snapshot_path;
            }
            break;
        }
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) continue;
        int first = 0, last = 0;
        mepml::ResultsReplaceRange(*b, &first, &last);
        const std::vector<std::string> res = mepml::FormatResults("\\image(" + ref + ")");
        lines.erase(lines.begin() + first, lines.begin() + std::min(last, static_cast<int>(lines.size())));
        lines.insert(lines.begin() + first, res.begin(), res.end());
    }
    return mepml::ParseForExport(file, lines, ReadFileLines, tags);
}

int Editor::MepmlGuiStart(int buffer_id, int fence_row, const std::vector<std::string> *argv,
                          const std::vector<std::string> &temp_files) {
    if (buffer_id < 0 || buffer_id >= static_cast<int>(buffers_.size())) {
        RemoveTempFiles(temp_files);
        return -1;
    }
    const Buffer &buf = buffers_[static_cast<size_t>(buffer_id)];
    const mepml::Document doc = mepml::Parse(buf.lines);
    const mepml::Block *blk = nullptr;
    for (const mepml::Block &b : doc.blocks)
        if (b.kind == mepml::BlockKind::Code && b.code_line_start - 1 == fence_row) blk = &b;
    const std::string lang = blk ? Lowered(blk->lang) : std::string();
    if (!blk || (argv ? argv->empty() : lang != "exec-gui" && lang != "gui")) {
        status_message_ = "Not a block whose program has a window (```exec-gui, or results=exec-gui)";
        RemoveTempFiles(temp_files);
        return -1;
    }
    std::string command = blk->code;
    while (!command.empty() && (command.back() == '\n' || command.back() == ' ')) command.pop_back();
    if (!argv && command.find_first_not_of(" \t\n") == std::string::npos) {
        status_message_ = "The block has nothing to run";
        return -1;
    }
    for (const auto &kv : mepml_guis_) {
        if (kv.second.buffer_id == buffer_id && kv.second.fence_row == fence_row) {
            status_message_ = "Already running (C-c C-k stops it)";
            RemoveTempFiles(temp_files);
            return -1;
        }
    }
    if (!gui_backend_) gui_backend_ = mep::gui_embed::CreateBackend(native_window_handle_);

    std::string cwd = ActiveRoot();
    std::filesystem::path doc_file;
    if (!buf.filename.empty()) {
        std::error_code ec;
        doc_file = std::filesystem::absolute(buf.filename, ec);
        if (!ec && !doc_file.parent_path().empty()) cwd = doc_file.parent_path().string();
    }
    auto app = std::make_unique<mep::gui_embed::EmbeddedApp>(*gui_backend_);
    const std::vector<std::string> cmd = argv ? *argv : std::vector<std::string>{"/bin/sh", "-c", command};
    std::string error;
    if (!app->Start(cmd, cwd, &error)) {
        status_message_ = error;
        RemoveTempFiles(temp_files);
        return -1;
    }
    const int id = next_mepml_term_++;
    MepmlGuiRun &run = mepml_guis_[id];
    run.app = std::move(app);
    run.buffer_id = buffer_id;
    run.code = blk->code;
    run.fence_row = fence_row;
    run.rows = std::clamp(IntOption(*blk, "rows", 20), 3, 200);
    // Its width: cols=, else the text width (or the pane's, when that is
    // narrower) -- the same as a terminal run, so it fits its card.
    int cols = TextWidth() - 2;
    if (CurPane().text_cols > 8) cols = std::min(cols, CurPane().text_cols - 4);
    run.cols = std::clamp(IntOption(*blk, "cols", cols), 10, 400);
    const std::string res = Lowered(StrOption(*blk, "results"));
    run.label = !argv                              ? command.substr(0, command.find('\n'))
                : (res == "web" || res == "app") ? (lang == "html" ? std::string("web page") : blk->lang + " web app")
                                                 : blk->lang + " program";
    run.web = res == "web" || res == "app";
    // A web page is part of what it shows: hovering and dragging reach it
    // without a click to hand it the keyboard first.
    if (run.web) run.app->SetPointerThrough(true);
    run.temp_files = temp_files;
    // Its last picture goes to file= (relative to the document), else
    // beside the document under a name its code decides; an unsaved
    // document keeps none.
    if (!doc_file.empty()) GuiSnapshotPaths(*blk, doc_file, &run.snapshot_ref, &run.snapshot_path);
    int first = 0, last = 0;
    mepml::ResultsReplaceRange(*blk, &first, &last);
    if (first == last && first > static_cast<int>(buf.lines.size())) first = last = static_cast<int>(buf.lines.size());
    ReplaceLinesAt(buffer_id, first, last, {"// result_begin: gui", "// [running] " + run.label, "// result_end"});
    run.results_end = first + 2;
    status_message_ = "Running " + run.label + " -- its window shows in the results; Enter or a click types into it, C-c C-k stops it";
    return id;
}

bool Editor::MepmlGuiViewOf(int run_id, MepmlGuiView *out) const {
    auto it = mepml_guis_.find(run_id);
    if (it == mepml_guis_.end()) return false;
    const MepmlGuiRun &run = it->second;
    using State = mep::gui_embed::EmbeddedApp::State;
    out->cols = run.cols;
    out->shown = run.app->GetState() == State::Shown;
    out->focused = mepml_gui_focus_ == run_id;
    switch (run.app->GetState()) {
        case State::Starting: out->status = "starting " + run.label + " ..."; break;
        case State::NoWindow:
            out->status = run.app->LastSnapshot().Empty() ? run.label + " has not opened a window (C-c C-k stops it)"
                                                          : "its window is closed -- C-c C-k stops " + run.label;
            break;
        case State::Shown: out->status.clear(); break;
        case State::Exited: out->status = run.label + " has ended"; break;
    }
    return true;
}

void Editor::MepmlGuiPlace(int run_id, const mep::gui_embed::Rect &full, const mep::gui_embed::Rect &clip) {
    auto it = mepml_guis_.find(run_id);
    if (it == mepml_guis_.end()) return;
    it->second.placed = full;
    it->second.app->Place(full, clip);
}

void Editor::MepmlGuisEndFrame() {
    // A web page lets the pointer straight through, so mep keeps the
    // keyboard on a focus proxy while one is on screen (else the keys would
    // go to the page whenever the mouse rests over it).
    bool web_shown = false;
    for (const auto &kv : mepml_guis_)
        if (kv.second.web && kv.second.app->GetState() == mep::gui_embed::EmbeddedApp::State::Shown) web_shown = true;
    gfx::SetKeyboardFocusProxy(web_shown);
    for (auto &kv : mepml_guis_) {
        kv.second.app->EndFrame();
        if (mepml_gui_focus_ == kv.first && kv.second.app->TakeFocusLost()) {
            mepml_gui_focus_ = -1;
            status_message_ = "Back in the document";
        }
    }
    if (gui_backend_) gui_backend_->Flush();
}

void Editor::MepmlGuisTick() {
    if (mepml_guis_.empty() || !gui_backend_) return;
    using State = mep::gui_embed::EmbeddedApp::State;
    gui_backend_->Pump();
    // A window that names no process is only taken as a program's own
    // while exactly one program is waiting for its window.
    int waiting = 0;
    for (const auto &kv : mepml_guis_) waiting += kv.second.app->GetState() == State::Starting ? 1 : 0;
    for (auto it = mepml_guis_.begin(); it != mepml_guis_.end();) {
        MepmlGuiRun &run = it->second;
        if (run.buffer_id < 0 || run.buffer_id >= static_cast<int>(buffers_.size())) {
            if (mepml_gui_focus_ == it->first) mepml_gui_focus_ = -1;
            run.app.reset();
            RemoveTempFiles(run.temp_files);
            it = mepml_guis_.erase(it);
            continue;
        }
        run.app->Tick(now_, waiting == 1);
        if (run.app->TakeFocusGained()) {
            // A web page clicked into: it has the keyboard now.
            mepml_gui_focus_ = it->first;
            status_message_ = "The page has the keyboard: Ctrl-\\ or a click outside it comes back";
        }
        if (mepml_gui_focus_ == it->first) {
            bool lost = run.app->TakeFocusLost() || !run.app->Focused();
            // mep only sees a click while the program has the keyboard
            // when it lands outside the program's window: that click
            // takes the keyboard back.
            if (!lost && (gfx::IsMouseButtonPressed(gfx::MouseButton::Left) || gfx::IsMouseButtonPressed(gfx::MouseButton::Right) ||
                          gfx::IsMouseButtonPressed(gfx::MouseButton::Middle))) {
                const gfx::Vector2 m = gfx::GetMousePosition();
                const mep::gui_embed::Rect &r = run.placed;
                if (m.x < static_cast<float>(r.x) || m.y < static_cast<float>(r.y) || m.x >= static_cast<float>(r.x + r.w) ||
                    m.y >= static_cast<float>(r.y + r.h)) {
                    run.app->Focus(false);
                    lost = true;
                }
            }
            if (lost) {
                mepml_gui_focus_ = -1;
                status_message_ = "Back in the document";
            }
        }
        if (run.app->GetState() != State::Exited) {
            ++it;
            continue;
        }
        // Ended: its last picture, what it printed, and how it ended
        // become the block's results.
        std::vector<std::string> lines;
        const mep::gui_embed::Snapshot &snap = run.app->LastSnapshot();
        if (!snap.Empty() && !run.snapshot_path.empty()) {
            const std::string png = png::Encode(snap.width, snap.height, 4, snap.rgba.data(), snap.width * 4);
            std::ofstream f(run.snapshot_path, std::ios::binary);
            if (!png.empty() && f.write(png.data(), static_cast<std::streamsize>(png.size()))) {
                f.close();
                InvalidateOrgInlineImageTexture(run.snapshot_path);
                lines.push_back("\\image(" + run.snapshot_ref + ")");
            }
        }
        const std::vector<std::string> &out = run.app->Stdout();
        for (size_t k = out.size() > 40 ? out.size() - 40 : 0; k < out.size(); ++k) lines.push_back(out[k]);
        if (run.app->Stops() > 0) {
            // A web page or app runs until it is stopped: its picture says it all.
            if (!run.web || lines.empty()) lines.push_back("[stopped]");
        } else if (run.app->ExitCode() != 0) {
            const std::vector<std::string> &err = run.app->Stderr();
            for (size_t k = err.size() > 10 ? err.size() - 10 : 0; k < err.size(); ++k) lines.push_back(err[k]);
            lines.push_back("[exit status " + std::to_string(run.app->ExitCode()) + "]");
        }
        if (lines.empty()) lines.push_back("[closed]");
        std::string text;
        for (size_t k = 0; k < lines.size(); ++k) text += (k ? "\n" : "") + lines[k];
        const Buffer &buf = buffers_[static_cast<size_t>(run.buffer_id)];
        const mepml::Document doc = mepml::Parse(buf.lines);
        if (const mepml::Block *b = BlockForRun(doc, run.code, run.fence_row)) {
            int first = 0, last = 0;
            mepml::ResultsReplaceRange(*b, &first, &last);
            // Plain results: the \image line draws as a picture, like a figure's.
            ReplaceLinesAt(run.buffer_id, first, last, mepml::FormatResults(text));
        }
        if (mepml_gui_focus_ == it->first) mepml_gui_focus_ = -1;
        run.app.reset();
        RemoveTempFiles(run.temp_files);
        it = mepml_guis_.erase(it);
    }
}

// --- The presentation view ---------------------------------------------------

// main.cpp owns the text font; full-screen mode sets it large.
float GetFontSizePx();
void SetFontSizePx(float px);

namespace {
// The view's first row is left blank: the cursor waits there while
// presenting, where it reveals nothing, and it is the slide's top margin.
constexpr int kPresentParkRow = 0;
// Full-screen text is sized so that this many rows and columns fill the
// screen -- a slide written for a 16:9 Beamer frame fits, and reads across
// a room.
constexpr float kPresentRows = 22.0f;
constexpr float kPresentCols = 64.0f;
constexpr float kPresentFontStep = 1.1f;
// The longest a slide stays blank waiting for its maths (one that fails
// to compile never arrives).
constexpr double kPresentRevealWait = 5.0;
}  // namespace

bool Editor::MepmlPresentStart(bool fullscreen, std::string *error) {
    if (present_.active) {
        MepmlPresentSetFullscreen(fullscreen);
        return true;
    }
    if (!IsMepmlBuffer()) {
        if (error) *error = "Not a mepml document";
        return false;
    }
    const Buffer &src = Buf();
    const int wrap = std::max(0, CurPane().text_cols - 1);
    std::vector<mepml::PresentationPage> pages =
        mepml::PresentationPages(MepmlCurrentFile(), src.lines, ReadFileLines, wrap);
    if (pages.empty()) {
        if (error) *error = "No slides: a presentation's slides are \\slide( ... ) blocks";
        return false;
    }
    MepmlPresentState st;
    st.active = true;
    st.source_buffer = CurrentBufferId();
    st.pane_id = ActivePaneId();
    st.source_lines = src.lines;
    st.wrap_cols = wrap;
    st.pages = std::move(pages);
    st.source_cursor = CurPane().cursor;
    st.source_scroll = CurPane().scroll_row;
    st.saved_zoomed_pane = zoomed_pane_id_;
    st.saved_zen = zen_mode_;
    // Start at the slide the cursor is on (the title page before the first).
    const int row = CurPane().cursor.row;
    for (size_t i = 0; i < st.pages.size(); ++i)
        if (st.pages[i].source_line >= 0 && st.pages[i].source_line <= row) st.page = static_cast<int>(i);

    // The view: a buffer beside the source (relative pictures resolve the
    // same), hidden by its dot, never written.
    const std::filesystem::path path(MepmlCurrentFile());
    const std::string view_name =
        (path.parent_path() / ("." + path.stem().string() + ".present.mepml")).string();
    int view = -1;
    for (size_t i = 0; i < buffers_.size(); ++i)
        if (buffers_[i].mepml_present_view && buffers_[i].filename == view_name) view = static_cast<int>(i);
    if (view < 0) view = CreateEmptyBuffer();
    Buffer &vb = buffers_[static_cast<size_t>(view)];
    vb.filename = view_name;
    vb.mepml_present_view = true;
    vb.deleted = false;
    vb.hide_line_numbers = true;
    vb.workspace_id = src.workspace_id;
    st.view_buffer = view;
    present_ = std::move(st);

    CurPane().buffer_id = view;
    zoomed_pane_id_ = present_.pane_id;
    present_.warm_pending = true;
    MepmlPresentShowPage();
    if (fullscreen) MepmlPresentApplyFullscreen(true);
    SyncModeToActivePaneBuffer();
    mode_ = Mode::Normal;
    MepmlPresentAutoStart();
    status_message_ = "Presenting: h/l (or C-n/C-p, arrows) change slide, n for normal mode, C-c C-c runs code, f " +
                      std::string(fullscreen ? "to leave full screen" : "for full screen") + ", q to stop";
    return true;
}

void Editor::MepmlPresentShowPage(bool keep_cursor) {
    if (!present_.active || present_.pages.empty()) return;
    present_.page = std::clamp(present_.page, 0, static_cast<int>(present_.pages.size()) - 1);
    std::vector<std::string> lines = {""};
    for (const std::string &l : present_.pages[static_cast<size_t>(present_.page)].lines) lines.push_back(l);
    present_.view_lines = lines;
    Buffer &vb = buffers_[static_cast<size_t>(present_.view_buffer)];
    vb.lines = std::move(lines);
    vb.modified = false;
    vb.undo_stack.clear();
    vb.redo_stack.clear();
    vb.folds.clear();
    // Tells the buffer-change hooks (the mepml render, highlighting).
    change_epoch_++;
    // What has to be rendered before the slide may show.
    present_.need_rows.clear();
    present_.need_inline.clear();
    const OrgLatexFragments frags = MepmlLatexFragmentsOf(mepml::Parse(vb.lines), vb.lines);
    for (const OrgLatexBlockFragment &b : frags.blocks) present_.need_rows.push_back(b.start_row - 1);
    for (const OrgLatexInlineFragment &f : frags.inlines)
        if (!f.parts.empty()) present_.need_inline.emplace_back(f.parts[0].row - 1, f.parts[0].col_start - 1);
    if (ActivePaneId() == present_.pane_id && CurPane().buffer_id == present_.view_buffer) {
        if (present_.caret && keep_cursor) {
            ClampCursor();  // the same slide again (its source changed): the cursor stays put
        } else {
            CurPane().cursor = present_.caret ? CursorPos{std::min(kPresentParkRow + 1, vb.LineCount() - 1), 0}
                                              : CursorPos{kPresentParkRow, 0};
            CurPane().scroll_row = 0;
        }
        // Render now rather than from next frame's hooks: formulas already
        // in the cache are then on the slide in the very frame it appears.
        if (lua_) lua_->DoString("if mep.mepml_render then mep.mepml_render() end "
                                 "if mep.org_latex_scan then mep.org_latex_scan() end");
    }
    MepmlPresentUpdateReveal();
}

bool Editor::MepmlPresentMathReady() const {
    if (!OrgLatexVisible()) return true;
    const Buffer &vb = buffers_[static_cast<size_t>(present_.view_buffer)];
    for (int row : present_.need_rows)
        if (!vb.org_latex_rows.count(row)) return false;
    for (const auto &need : present_.need_inline) {
        auto it = vb.org_latex_inline.find(need.first);
        if (it == vb.org_latex_inline.end()) return false;
        bool found = false;
        for (const Buffer::OrgLatexInlineSpan &sp : it->second) found = found || (sp.col_start == need.second && !sp.path.empty());
        if (!found) return false;
    }
    return true;
}

void Editor::MepmlPresentUpdateReveal() {
    if (!present_.active) return;
    const double now = gfx::GetTime();
    bool ready = MepmlPresentMathReady();
    // Full screen also waits for the size the slide fits at (known at once
    // for a slide shown before).
    if (ready && present_.fullscreen && present_.autofit)
        ready = present_.fit_checked;
    if (present_.timed_out_page == present_.page) ready = true;
    if (ready) {
        present_.revealed = true;
        return;
    }
    if (present_.revealed) {
        present_.revealed = false;
        present_.reveal_deadline = now + kPresentRevealWait;
    } else if (now >= present_.reveal_deadline) {
        present_.timed_out_page = present_.page;
        present_.revealed = true;
    }
}

void Editor::MepmlPresentGoto(int page) {
    if (!present_.active) return;
    page = std::clamp(page, 0, static_cast<int>(present_.pages.size()) - 1);
    if (page == present_.page) return;
    present_.page = page;
    // Every slide starts the way the presentation does: no cursor.
    if (present_.caret) {
        present_.caret = false;
        if (mode_ != Mode::Command) mode_ = Mode::Normal;
    }
    present_.fit_checked = present_.settled.count(page) > 0;
    present_.timed_out_page = -1;
    // Each slide starts from the screen's text size (the last may have
    // needed less, MepmlPresentTick).
    if (present_.fullscreen && present_.autofit && present_.fit_px > 0.0f) {
        auto known = present_.page_px.find(page);
        const float px = known != present_.page_px.end() ? known->second : present_.fit_px;
        if (GetFontSizePx() != px) SetFontSizePx(px);
    }
    MepmlPresentShowPage();
    MepmlPresentAutoStart();
}

void Editor::MepmlPresentApplyFullscreen(bool on) {
    present_.fullscreen = on;
    present_.page_px.clear();
    present_.settled.clear();
    present_.fit_checked = false;
    present_.warm_pending = true;
    zen_mode_ = on ? true : present_.saved_zen;
    gfx::SetWindowFullscreen(on);
    if (on) {
        // The text is sized by MepmlPresentTick, from the screen once the
        // window has become full screen (a frame or two from now).
        if (present_.saved_font_px <= 0.0f) present_.saved_font_px = GetFontSizePx();
        present_.autofit = true;
        present_.fit_w = present_.fit_h = -1;
    } else if (present_.saved_font_px > 0.0f) {
        SetFontSizePx(present_.saved_font_px);
        present_.saved_font_px = 0.0f;
    }
}

void Editor::MepmlPresentSetFullscreen(bool on) {
    if (!present_.active || on == present_.fullscreen) return;
    MepmlPresentApplyFullscreen(on);
}

void Editor::MepmlPresentSetCaret(bool on) {
    if (!present_.active) return;
    present_.caret = on;
    if (ActivePaneId() == present_.pane_id && CurPane().buffer_id == present_.view_buffer) {
        const Buffer &vb = buffers_[static_cast<size_t>(present_.view_buffer)];
        CurPane().cursor = on ? CursorPos{std::min(kPresentParkRow + 1, vb.LineCount() - 1), 0}
                              : CursorPos{kPresentParkRow, 0};
        if (!on) CurPane().scroll_row = 0;
    }
    mode_ = Mode::Normal;
    status_message_ = on ? "Normal mode on the slide: move and yank as usual, C-c C-c runs the block under the cursor, P to present"
                         : "";
}

void Editor::MepmlPresentRebuild() {
    if (!present_.active) return;
    const Buffer &src = buffers_[static_cast<size_t>(present_.source_buffer)];
    std::string file = src.filename;
    std::error_code ec;
    if (!file.empty() && file[0] != '/') file = std::filesystem::absolute(file, ec).string();
    std::vector<mepml::PresentationPage> pages =
        mepml::PresentationPages(file, src.lines, ReadFileLines, present_.wrap_cols);
    present_.source_lines = src.lines;
    if (pages.empty()) return;  // every slide gone mid-edit: keep what is shown
    // Stay on the same slide: by its number, else by position.
    const mepml::PresentationPage &was = present_.pages[static_cast<size_t>(present_.page)];
    int page = std::min(present_.page, static_cast<int>(pages.size()) - 1);
    for (size_t i = 0; i < pages.size(); ++i)
        if (pages[i].number == was.number) page = static_cast<int>(i);
    const bool same = page == present_.page;
    present_.pages = std::move(pages);
    present_.page = page;
    MepmlPresentShowPage(same);
}

void Editor::MepmlPresentStop() {
    if (!present_.active) return;
    // The web pages and apps it started itself stop with it (each leaves
    // its picture as its block's results).
    for (int fence : present_.auto_started) {
        const int run = MepmlPresentRunningAt(fence);
        if (run >= 0) MepmlTerminalStop(run);
    }
    if (present_.fullscreen) gfx::SetWindowFullscreen(false);
    if (present_.saved_font_px > 0.0f) SetFontSizePx(present_.saved_font_px);
    zen_mode_ = present_.saved_zen;
    zoomed_pane_id_ = present_.saved_zoomed_pane;
    // Back to the source, at the slide last shown.
    const int page_line = present_.pages.empty() ? -1 : present_.pages[static_cast<size_t>(present_.page)].source_line;
    const MepmlPresentState st = std::move(present_);
    present_ = MepmlPresentState();
    if (ActivePaneId() != st.pane_id) FocusPaneById(st.pane_id);
    if (st.source_buffer >= 0 && st.source_buffer < static_cast<int>(buffers_.size()) &&
        CurPane().buffer_id == st.view_buffer) {
        CurPane().buffer_id = st.source_buffer;
        if (page_line >= 0) {
            CurPane().cursor = {page_line, 0};
            CurPane().scroll_row = page_line;
        } else {
            CurPane().cursor = st.source_cursor;
            CurPane().scroll_row = st.source_scroll;
        }
        ClampCursor();
    }
    if (st.view_buffer >= 0 && st.view_buffer < static_cast<int>(buffers_.size())) {
        Buffer &vb = buffers_[static_cast<size_t>(st.view_buffer)];
        vb.lines = {""};
        vb.deleted = true;
    }
    SyncModeToActivePaneBuffer();
    mode_ = Mode::Normal;
    status_message_.clear();
}

void Editor::MepmlPresentTick() {
    if (!present_.active) return;
    const bool here = ActivePaneId() == present_.pane_id;
    // The pane went on to show something else (:e, :b): the presentation
    // is over, without taking the pane back.
    if (here && CurPane().buffer_id != present_.view_buffer) {
        const int shown = CurPane().buffer_id;
        const CursorPos cur = CurPane().cursor;
        const int scroll = CurPane().scroll_row;
        MepmlPresentStop();
        CurPane().buffer_id = shown;
        CurPane().cursor = cur;
        CurPane().scroll_row = scroll;
        ClampCursor();
        SyncModeToActivePaneBuffer();
        return;
    }
    if (present_.source_buffer >= static_cast<int>(buffers_.size()) ||
        buffers_[static_cast<size_t>(present_.source_buffer)].deleted) {
        MepmlPresentStop();
        return;
    }
    Buffer &vb = buffers_[static_cast<size_t>(present_.view_buffer)];
    if (vb.lines != present_.view_lines) {
        // An edit in caret mode: the view is the deck's, not a draft.
        vb.lines = present_.view_lines;
        vb.modified = false;
        change_epoch_++;
        if (mode_ == Mode::Insert) mode_ = Mode::Normal;
        if (here) ClampCursor();
        status_message_ = "The slides are read-only here: edit the source (q leaves the presentation)";
    }
    // Without caret mode nothing moves the cursor off its blank row (a
    // click, the wheel, a drag): it would reveal the source under it.
    if (here && !present_.caret && CurPane().buffer_id == present_.view_buffer &&
        (CurPane().cursor.row != kPresentParkRow || CurPane().cursor.col != 0 || CurPane().scroll_row != 0)) {
        CurPane().cursor = {kPresentParkRow, 0};
        CurPane().scroll_row = 0;
        if (mode_ == Mode::Visual || mode_ == Mode::VisualLine || mode_ == Mode::VisualBlock) mode_ = Mode::Normal;
    }
    // Full screen: the text sized to the screen, again whenever it changes
    // size, until +/- says otherwise.
    bool sized = false;
    if (present_.fullscreen && present_.autofit) {
        const int w = gfx::GetScreenWidth(), h = gfx::GetScreenHeight();
        if ((w != present_.fit_w || h != present_.fit_h) && w > 0 && h > 0) {
            present_.fit_w = w;
            present_.fit_h = h;
            present_.fit_px = std::min(static_cast<float>(h) / kPresentRows - 6.0f,
                                       static_cast<float>(w) / (kPresentCols * 0.6f));
            SetFontSizePx(present_.fit_px);
            present_.page_px.clear();
            present_.settled.clear();
            present_.fit_checked = false;
            present_.warm_pending = true;
            sized = true;
        }
    }
    // Prose is filled to the pane's width: again when that changes (the
    // text size, full screen, the window).
    if (here && CurPane().text_cols > 1 && CurPane().text_cols - 1 != present_.wrap_cols) {
        present_.wrap_cols = CurPane().text_cols - 1;
        MepmlPresentRebuild();
        sized = true;
    }
    // A slide taller than the screen at that size gets smaller text, a
    // step a frame (once the last step's layout has been drawn), down to
    // half. Only ever smaller: a formula whose picture arrives late still
    // gets its room.
    if (GetFontSizePx() != present_.seen_px) {
        present_.seen_px = GetFontSizePx();
        sized = true;  // the pane has not been drawn at this size yet
    }
    if (!sized && here && present_.fullscreen && present_.autofit && present_.fit_px > 0.0f &&
        CurPane().buffer_id == present_.view_buffer && MepmlPresentMathReady()) {
        const Pane &pane = CurPane();
        int slots = 0;
        // Walked the way the draw loop walks: a formula's (or an html
        // result's) further source rows are drawn by its first one.
        for (int r = 0; r < vb.LineCount(); ++r) {
            int first = 0;
            if (RenderContaining(pane, vb, r, &first)) continue;
            slots += PaneRowSlots(pane, vb, r, pane.text_cols);
        }
        if (slots > pane.visible_lines && GetFontSizePx() > present_.fit_px * 0.5f) {
            SetFontSizePx(GetFontSizePx() / 1.08f);
            present_.page_px[present_.page] = GetFontSizePx();
        } else if (!present_.fit_checked) {
            present_.fit_checked = true;
            present_.page_px[present_.page] = GetFontSizePx();
            present_.settled.insert(present_.page);
        }
    }
    MepmlPresentUpdateReveal();
    // Follow the source (a block that finished running, an edit in
    // another pane) a couple of times a second.
    const double now = gfx::GetTime();
    if (now >= present_.next_source_check) {
        present_.next_source_check = now + 0.5;
        if (buffers_[static_cast<size_t>(present_.source_buffer)].lines != present_.source_lines) {
            MepmlPresentRebuild();
            present_.warm_pending = true;
        }
    }
}

bool Editor::HandleMepmlPresentInput() {
    if (!present_.active || ActivePaneId() != present_.pane_id || CurPane().buffer_id != present_.view_buffer)
        return false;
    auto held = [](gfx::Key k) { return gfx::IsKeyPressed(k) || gfx::IsKeyPressedRepeat(k); };
    const bool ctrl = gfx::IsKeyDown(gfx::Key::LeftControl) || gfx::IsKeyDown(gfx::Key::RightControl);
    const bool alt = gfx::IsKeyDown(gfx::Key::LeftAlt) || gfx::IsKeyDown(gfx::Key::RightAlt);
    if (present_.caret) {
        // (Ctrl-n / Ctrl-p come off the key queue in Normal mode, which
        // reads every Ctrl combination from it: HandleNormalInput.)
        // The presentation's own keys still work with a cursor (they are
        // the presentation's, not vim's, while presenting): f, + = - 0, r
        // and q. Read off the key state -- the typed-character queue
        // belongs to Normal mode, which would then see nothing.
        if (!ctrl && !alt) {
            const bool shift = gfx::IsKeyDown(gfx::Key::LeftShift) || gfx::IsKeyDown(gfx::Key::RightShift);
            int key = 0;
            if (gfx::IsKeyPressed(gfx::Key::F) && !shift) key = 'f';
            else if (held(gfx::Key::Equal)) key = shift ? '+' : '=';
            else if (held(gfx::Key::Minus) && !shift) key = '-';
            else if (gfx::IsKeyPressed(gfx::Key::Zero) && !shift) key = '0';
            else if (gfx::IsKeyPressed(gfx::Key::R) && !shift) key = 'r';
            else if (gfx::IsKeyPressed(gfx::Key::Q) && !shift) key = 'q';
            if (key != 0) {
                while (gfx::GetCharPressed() > 0) {
                }
                MepmlPresentKey(key);
                return true;
            }
        }
        // Normal mode's own keys: C-c C-c runs the block under the cursor
        // (TryRunOrgBabelAtCursor), and P goes back to presenting (its
        // typed character is Normal mode's, HandleNormalInput).
        return false;
    }
    // Ctrl combinations come off the key queue, as Normal mode reads them
    // (a key-down poll misses them on a slow frame): Ctrl-n / Ctrl-p step
    // through the slides, and C-c C-c runs every code block the slide shows.
    if (ctrl && !alt) {
        int ctrl_step = 0;
        bool ours = false;
        for (gfx::Key key = gfx::GetKeyPressed(); key != gfx::Key::None; key = gfx::GetKeyPressed()) {
            if (key == gfx::Key::N) ctrl_step = 1, ours = true;
            else if (key == gfx::Key::P) ctrl_step = -1, ours = true;
            else if (key == gfx::Key::C) {
                ours = true;
                if (pending_ctrl_c_ && (now_ - pending_ctrl_c_time_) < kCtrlCChordTimeoutSec) {
                    pending_ctrl_c_ = false;
                    MepmlPresentRunBlocks(-1);
                } else {
                    pending_ctrl_c_ = true;
                    pending_ctrl_c_time_ = now_;
                }
            }
        }
        if (held(gfx::Key::N) && !ours) ctrl_step = 1, ours = true;  // held down: repeat
        if (held(gfx::Key::P) && !ours) ctrl_step = -1, ours = true;
        if (ctrl_step != 0) MepmlPresentGoto(present_.page + ctrl_step);
        while (gfx::GetCharPressed() > 0) {
        }
        if (ours) return true;
    }
    if (ctrl || alt) return false;  // window/pane chords still work
    int step = 0;
    if (held(gfx::Key::Right) || held(gfx::Key::Down) || held(gfx::Key::PageDown) || held(gfx::Key::Enter) ||
        held(gfx::Key::KpEnter))
        step = 1;
    if (held(gfx::Key::Left) || held(gfx::Key::Up) || held(gfx::Key::PageUp) || held(gfx::Key::Backspace)) step = -1;
    if (gfx::IsKeyPressed(gfx::Key::Home)) MepmlPresentGoto(0);
    if (gfx::IsKeyPressed(gfx::Key::End)) MepmlPresentGoto(MepmlPresentPageCount() - 1);
    if (gfx::IsKeyPressed(gfx::Key::Escape)) {
        MepmlPresentStop();
        return true;
    }
    int ch = 0;
    while ((ch = gfx::GetCharPressed()) > 0) {
        switch (ch) {
            case ' ': case 'l': step = 1; break;
            case 'h': step = -1; break;
            // n: normal mode -- the cursor on the slide, at its first
            // character, and vim's keys (P comes back).
            case 'n':
                MepmlPresentSetCaret(true);
                while (gfx::GetCharPressed() > 0) {
                }
                return true;
            case 'g': MepmlPresentGoto(0); break;
            case 'G': MepmlPresentGoto(MepmlPresentPageCount() - 1); break;
            case 'f': case 'r': case '+': case '=': case '-': case '0': MepmlPresentKey(ch); break;
            case 'q':
                MepmlPresentKey(ch);
                while (gfx::GetCharPressed() > 0) {
                }
                return true;
            case ':':
                EnterCommand();
                while (gfx::GetCharPressed() > 0) {
                }
                return true;
            default: break;
        }
        if (!present_.active) return true;
    }
    if (step != 0) MepmlPresentGoto(present_.page + step);
    return true;
}

void Editor::MepmlPresentKey(int ch) {
    switch (ch) {
        case 'f': MepmlPresentSetFullscreen(!present_.fullscreen); break;
        case 'r': MepmlPresentRebuild(); break;
        case 'q': MepmlPresentStop(); break;
        case '+': case '=': case '-': case '0':
            if (present_.saved_font_px <= 0.0f) present_.saved_font_px = GetFontSizePx();
            present_.autofit = false;
            present_.page_px.clear();
            present_.settled.clear();
            present_.warm_pending = true;
            if (ch == '0') {
                // Back to the size before the presentation, or in full
                // screen to the size that fits it.
                present_.autofit = present_.fullscreen;
                present_.fit_w = present_.fit_h = -1;
                if (!present_.fullscreen) SetFontSizePx(present_.saved_font_px);
            } else {
                SetFontSizePx(ch == '-' ? GetFontSizePx() / kPresentFontStep : GetFontSizePx() * kPresentFontStep);
            }
            break;
        default: break;
    }
}

int Editor::MepmlPresentSourceFence(int run_id) const {
    int code_fence = -1;
    std::string code;
    int buffer = -1;
    if (auto g = mepml_guis_.find(run_id); g != mepml_guis_.end()) {
        code = g->second.code;
        code_fence = g->second.fence_row;
        buffer = g->second.buffer_id;
    } else if (auto t = mepml_terms_.find(run_id); t != mepml_terms_.end()) {
        code = t->second.code;
        code_fence = t->second.fence_row;
        buffer = t->second.buffer_id;
    }
    if (buffer < 0 || buffer >= static_cast<int>(buffers_.size())) return -1;
    // Where its block is now (the document may have changed above it).
    const mepml::Document doc = mepml::Parse(buffers_[static_cast<size_t>(buffer)].lines);
    const mepml::Block *blk = BlockForRun(doc, code, code_fence);
    return blk ? blk->code_line_start - 1 : -1;
}

int Editor::MepmlPresentRunningAt(int fence) const {
    if (!present_.active) return -1;
    for (const auto &kv : mepml_guis_)
        if (kv.second.buffer_id == present_.source_buffer && MepmlPresentSourceFence(kv.first) == fence) return kv.first;
    for (const auto &kv : mepml_terms_)
        if (kv.second.buffer_id == present_.source_buffer && MepmlPresentSourceFence(kv.first) == fence) return kv.first;
    return -1;
}

void Editor::MepmlPresentStartFences(const std::vector<int> &fences) {
    if (fences.empty() || !lua_ || !present_.active) return;
    // mep_mepml_run_block reads the block and the file from the current
    // buffer and remembers which buffer to write back to, so the source is
    // current just while each run starts.
    const int view = CurPane().buffer_id;
    const CursorPos cursor = CurPane().cursor;
    const int scroll = CurPane().scroll_row;
    CurPane().buffer_id = present_.source_buffer;
    for (int fence : fences) lua_->DoString("mep.mepml_run_block_at(" + std::to_string(fence + 1) + ")");
    CurPane().buffer_id = view;
    CurPane().cursor = cursor;
    CurPane().scroll_row = scroll;
}

void Editor::MepmlPresentAutoStart() {
    if (!present_.active || present_.pages.empty()) return;
    // A web page or app on the slide is the slide's: it runs (live, under
    // the mouse) as soon as the slide is shown, rather than as its last
    // picture. It keeps running while other slides show; the
    // presentation's end stops it.
    std::vector<int> start;
    for (const mepml::PresentationPage::Shown &sh : present_.pages[static_cast<size_t>(present_.page)].blocks) {
        if (!sh.live || sh.source_fence < 0 || MepmlPresentRunningAt(sh.source_fence) >= 0) continue;
        start.push_back(sh.source_fence);
        present_.auto_started.insert(sh.source_fence);
    }
    MepmlPresentStartFences(start);
}

bool Editor::MepmlPresentRunBlocks(int view_row) {
    if (!present_.active || present_.pages.empty()) return false;
    const mepml::PresentationPage &page = present_.pages[static_cast<size_t>(present_.page)];
    // What can run from the slide: a block whose code it shows, or a live
    // one (a web page, an app, a window) whose output it shows. The view's
    // first row is the cursor's blank, so page line k is view row k + 1.
    std::vector<int> fences;
    for (const mepml::PresentationPage::Shown &sh : page.blocks) {
        if (!sh.code && !sh.live) continue;
        if (view_row >= 0 && (view_row < sh.first + 1 || view_row > sh.last + 1)) continue;
        fences.push_back(sh.source_fence);
    }
    if (fences.empty()) {
        status_message_ = view_row < 0 ? "Nothing on this slide to run" : "Not on a block the slide can run";
        return false;
    }
    if (!lua_) return false;
    // A program already running from one of them (a web app, a window): this
    // stops it instead, so C-c C-c starts and stops a live block.
    int stopped = 0;
    std::vector<int> start;
    for (int fence : fences) {
        const int running = MepmlPresentRunningAt(fence);
        if (running >= 0) {
            MepmlTerminalStop(running);
            present_.auto_started.erase(fence);
            ++stopped;
        } else {
            start.push_back(fence);
        }
    }
    // Run in the document: its block has the options the slide leaves out,
    // and its results region is where they go (the slide follows).
    // mep_mepml_run_block reads the block and the file from the current
    // buffer and remembers which buffer to write back to, so the source is
    // current just while each run starts.
    MepmlPresentStartFences(start);
    std::string msg;
    if (!start.empty()) msg = "Running " + std::to_string(start.size()) + (start.size() == 1 ? " block" : " blocks");
    if (stopped > 0) msg += std::string(msg.empty() ? "Stopping " : ", stopping ") + std::to_string(stopped);
    status_message_ = msg;
    return true;
}

std::vector<std::pair<std::string, float>> Editor::MepmlPresentTakeWarm() {
    std::vector<std::pair<std::string, float>> out;
    if (!present_.active || !present_.warm_pending || present_.pages.empty()) return out;
    // Full screen renders at the size the screen gets, known once the
    // window has become full screen; ask again then.
    const bool fitted = present_.fullscreen && present_.autofit;
    if (fitted && present_.fit_px <= 0.0f) return out;
    present_.warm_pending = false;
    const int n = static_cast<int>(present_.pages.size());
    std::set<std::pair<std::string, float>> seen;
    // The slide shown, the ones after it in order, then those before.
    for (int k = 0; k < n; ++k) {
        const int page = (present_.page + k) % n;
        float px = GetFontSizePx();
        if (fitted) {
            auto known = present_.page_px.find(page);
            px = known != present_.page_px.end() ? known->second : present_.fit_px;
        }
        const std::vector<std::string> &lines = present_.pages[static_cast<size_t>(page)].lines;
        const OrgLatexFragments frags = MepmlLatexFragmentsOf(mepml::Parse(lines), lines);
        auto add = [&](const std::string &body) {
            if (seen.insert({body, px}).second) out.emplace_back(body, px);
        };
        for (const OrgLatexBlockFragment &b : frags.blocks) add(b.body);
        for (const OrgLatexInlineFragment &f : frags.inlines) add(f.body);
    }
    return out;
}
