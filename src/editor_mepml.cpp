// Editor-side half of mepml (src/mepml_doc.h holds the pure half): turning
// a parsed document into Decorations, the heading/image/link registries
// DrawPane reads, folds, and splicing code-block results back into a
// buffer. Everything here is a thin layer over mepml::Parse/Highlight --
// the language's own rules live, and are tested, in mepml_doc.cpp.
#include <chrono>
#include <cstring>
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
#include "a11y_file.h"
#include "a11y_html.h"
#include "mepml_a11y.h"
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

std::string Lowered(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

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

// Columns DrawPane reserves for a run drawn at `scale` (Decoration::virt_scale).
// The largest a scaled run is drawn inside a table cell (see MepmlScan).
constexpr float kTableMaxScale = 1.25f;

int StyledCols(int codepoints, float scale) {
    return std::max(1, static_cast<int>(std::ceil(static_cast<float>(codepoints) * scale - 1e-3f)));
}

// What a span occupies once concealed -- the same arithmetic DrawPane's
// collapse does: hidden markup is 0, a replacement its own codepoints, a
// scaled run StyledCols(). `max_scale` caps runs the way table cells do.
// `st` is the span's computed style (its size and face are the sheets').
int ConcealedWidth(const mepml::Span &sp, const std::string &line, float max_scale, const mepml::style::Computed &st) {
    const int cp = Codepoints(line.substr(static_cast<size_t>(sp.col_start),
                                          static_cast<size_t>(sp.col_end - sp.col_start)));
    if (sp.markup) return Codepoints(sp.replace);
    const float k = std::min(std::clamp(st.font_size, 0.5f, 3.0f), max_scale);
    if ((k != 1.0f || !st.font_family.empty()) && !(sp.style & (mepml::kMath | mepml::kComment)))
        return StyledCols(cp, k);
    return cp;
}

// A sheet's colour as a highlight group's name: the theme group it names,
// or the literal colour as "#rrggbb" (Editor::ResolveHighlight reads both).
std::string StyleHl(const mepml::style::Color &c) {
    if (c.kind == mepml::style::Color::Theme) return c.group;
    if (c.kind != mepml::style::Color::Rgb) return std::string();
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%06x", static_cast<unsigned>(c.rgb & 0xffffffu));
    return buf;
}

// A sheet's colour as a card's (OrgCardColor): set, even when it is `none`.
OrgCardColor CardColorOf(const mepml::style::Color &c) {
    OrgCardColor out;
    out.set = true;
    out.hl = StyleHl(c);
    out.alpha = c.alpha;
    return out;
}

std::string Repeat(const char *glyph, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += glyph;
    return out;
}

}  // namespace

std::string DocStyleHl(const mepml::style::Color &c) { return StyleHl(c); }

OrgCardLook Editor::DocCardLook(const mepml::Element &element, const std::string &media) const {
    // (The same reading of a block's computed style as MepmlBuildCards'
    // look_of, from the default and user sheets alone.)
    const mepml::style::Computed &own = DocSheetStyle({element}, media);
    mepml::Element active = element;
    active.With(":active");
    const mepml::style::Computed &on = DocSheetStyle({active}, media);
    const mepml::style::Computed &band = DocSheetStyle({element, element.Part("header")}, media);
    const mepml::style::Computed &chip = DocSheetStyle({element, element.Part("label")}, media);
    OrgCardLook look;
    look.wash = CardColorOf(own.background);
    look.border = CardColorOf(own.border_color);
    look.border_active = CardColorOf(on.border_color);
    look.stripe = CardColorOf(own.border_left_color);
    look.band = CardColorOf(band.background);
    look.chip = CardColorOf(chip.background.kind != mepml::style::Color::None ? chip.background
                            : chip.has_color                                  ? chip.color
                                                                              : mepml::style::Color());
    if (chip.background.kind != mepml::style::Color::None && chip.has_color) look.chip_text = CardColorOf(chip.color);
    const mepml::style::Computed &title = DocSheetStyle({element, element.Part("title")}, media);
    if (title.has_color) look.title = CardColorOf(title.color);
    const mepml::style::Computed &option = DocSheetStyle({element, element.Part("option")}, media);
    if (option.has_color) look.option = CardColorOf(option.color);
    const mepml::style::Computed &button = DocSheetStyle({element, element.Part("button")}, media);
    if (button.has_color && button.color != own.color) look.button = CardColorOf(button.color);
    if (band.has_color && band.color != own.color) look.header_text = CardColorOf(band.color);
    return look;
}

void Editor::DocTableLook(OrgTableGrid *grid, const std::string &media) const {
    const mepml::Element table("table");
    const mepml::style::Computed &ts = DocSheetStyle({table}, media);
    grid->look.wash = CardColorOf(ts.background);
    grid->look.border = CardColorOf(ts.border_color);
    mepml::Element active = table;
    active.With(":active");
    grid->look.border_active = CardColorOf(DocSheetStyle({active}, media).border_color);
    const mepml::style::Computed &rs = DocSheetStyle({table, table.Part("rule")}, media);
    if (rs.has_color) grid->rule = CardColorOf(rs.color);
}

bool Editor::IsMepmlBuffer() const { return LspFiletype(Buf().filename) == "mepml"; }

bool IsMarkdownFiletype(const std::string &ft) { return ft == "md" || ft == "markdown"; }

int MdHeadingLevel(const std::string &line) {
    // An ATX heading: up to three spaces of indent, one to six `#`, then a
    // space (or nothing: `##` alone is an empty heading in CommonMark, but
    // mep draws only titled ones large).
    size_t i = 0;
    while (i < line.size() && i < 3 && line[i] == ' ') ++i;
    int level = 0;
    while (i < line.size() && line[i] == '#' && level < 7) {
        ++level;
        ++i;
    }
    if (level == 0 || level > 6) return 0;
    if (i >= line.size() || (line[i] != ' ' && line[i] != '\t')) return 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    return i < line.size() ? level : 0;
}

int MdHeadingMarkupLen(const std::string &line) {
    // The `#`s and the blanks after them, from the line's start: what the
    // rendered heading hides (like an org headline's stars).
    const int level = MdHeadingLevel(line);
    if (level <= 0) return 0;
    size_t i = 0;
    while (i < line.size() && line[i] == ' ') ++i;
    i += static_cast<size_t>(level);
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    return static_cast<int>(i);
}

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
    // Markdown: an ATX heading outside a fenced block (Editor::MdConceal
    // records which rows are headings, so a `# comment` in a code fence
    // is not one).
    if (IsMarkdownFiletype(ft)) {
        auto it = buf.mepml_heading_rows.find(row);
        if (it == buf.mepml_heading_rows.end()) return 0;
        return MdHeadingLevel(line) == it->second ? it->second : 0;
    }
    return 0;
}

int Editor::HeadingExtraSlotsForLevel(int level) {
    if (level <= 0) return 0;
    constexpr int kCount = static_cast<int>(sizeof(kOrgHeadingStyles) / sizeof(kOrgHeadingStyles[0]));
    return kOrgHeadingStyles[std::min(level, kCount) - 1].slots - 1;
}

OrgHeadingStyle Editor::HeadingStyleForRow(const Buffer &buf, int row) {
    const int level = HeadingLevelForRow(buf, row);
    if (level <= 0) return OrgHeadingStyle{1.0f, 1};
    constexpr int kCount = static_cast<int>(sizeof(kOrgHeadingStyles) / sizeof(kOrgHeadingStyles[0]));
    OrgHeadingStyle style = kOrgHeadingStyles[std::min(level, kCount) - 1];
    // A mepml heading's size is its style's; a larger one takes a second
    // slot (and a third ...) once it no longer fits the line's own.
    auto it = buf.mepml_heading_scale.find(row);
    if (it != buf.mepml_heading_scale.end()) {
        style.scale = it->second;
        style.slots = std::max(1, static_cast<int>(std::ceil(static_cast<double>(it->second) - 0.2)));
    }
    return style;
}

int Editor::HeadingExtraSlotsForRow(const Buffer &buf, int row) { return HeadingStyleForRow(buf, row).slots - 1; }

int Editor::HeadingHideLenForRow(const Buffer &buf, int row) {
    if (row < 0 || row >= buf.LineCount()) return 0;
    const std::string &line = buf.lines[static_cast<size_t>(row)];
    const std::string ft = LspFiletype(buf.filename);
    if (ft == "org") return OrgHeadlineStarHideLen(line);
    if (ft == "mepml" && HeadingLevelForRow(buf, row) > 0) return mepml::LineHeadingMarkupLen(line);
    if (IsMarkdownFiletype(ft) && HeadingLevelForRow(buf, row) > 0) return MdHeadingMarkupLen(line);
    return 0;
}

int Editor::HeadingIndentColsForRow(const Buffer &buf, int row) {
    if (row < 0 || row >= buf.LineCount()) return 0;
    const std::string &line = buf.lines[static_cast<size_t>(row)];
    const std::string ft = LspFiletype(buf.filename);
    if (ft == "org") return OrgHeadlineStarIndentCols(line);
    if (ft == "mepml" || IsMarkdownFiletype(ft)) return std::max(0, HeadingLevelForRow(buf, row) - 1);
    return 0;
}

const mepml::style::Computed &Editor::DocSheetStyle(const std::vector<mepml::Element> &chain, const std::string &media) const {
    std::string signature;
    std::vector<std::shared_ptr<const mepml::style::Sheet>> sheets = MepmlSheets(&signature);
    if (signature != doc_sheet_styles_signature_) {
        doc_sheet_styles_.clear();
        doc_sheet_styles_signature_ = signature;
    }
    std::string key = media + "|";
    for (const mepml::Element &e : chain) {
        key += e.name + ":" + e.part;
        for (const auto &kv : e.attrs) key += "[" + kv.first + "=" + kv.second + "]";
        key += ">";
    }
    auto it = doc_sheet_styles_.find(key);
    if (it != doc_sheet_styles_.end()) return it->second;
    mepml::style::Cascade cascade;
    cascade.sheets = std::move(sheets);
    cascade.media = {"editor", "screen"};
    // (`media` is one tag, or several joined by commas: "md,source".)
    for (size_t at = 0; at < media.size();) {
        const size_t comma = media.find(',', at);
        const std::string tag = media.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        if (!tag.empty()) cascade.media.push_back(tag);
        if (comma == std::string::npos) break;
        at = comma + 1;
    }
    const mepml::Element root("document");
    std::vector<const mepml::Element *> path = {&root};
    mepml::style::Computed st = cascade.Compute(path, mepml::style::Computed());
    for (const mepml::Element &e : chain) {
        path.push_back(&e);
        st = cascade.Compute(path, st);
    }
    return doc_sheet_styles_.emplace(std::move(key), std::move(st)).first->second;
}

void Editor::MepmlColumnsPlace(const Pane &pane, const Buffer &buf, int wrap_cols) const {
    buf.mepml_col_place.clear();
    buf.mepml_col_pad.clear();
    // (As written -- raw, plain, or with concealment off -- the source is
    // shown line under line.)
    if (buf.mepml_columns.empty() || buf.mepml_raw || PaneOrgPlain(pane) || !org_conceal_visible_) return;
    constexpr int kGap = 2, kNarrowest = 12;
    const int total = pane.text_cols - 1;
    const int line_count = buf.LineCount();
    auto starts = [&](int row, const char *what) {
        const std::string &l = buf.lines[static_cast<size_t>(row)];
        const size_t at = l.find_first_not_of(" \t");
        return at != std::string::npos && l.compare(at, std::strlen(what), what) == 0;
    };
    for (const Buffer::MepmlColumnsBlock &blk : buf.mepml_columns) {
        // The block as it was scanned (an edit since may have moved it).
        if (blk.start_row < 0 || blk.end_row >= line_count || !starts(blk.start_row, "\\columns(") || !starts(blk.end_row, ")"))
            continue;
        // The cursor in it: shown as written, one column after another.
        if (pane.cursor.row >= blk.start_row && pane.cursor.row <= blk.end_row) continue;
        // (A closed fold is one line where it is: fine inside one column,
        // not across the lines the layout hangs on.)
        bool sound = true;
        for (const Fold &f : buf.folds) {
            if (!f.closed || f.start_row > blk.end_row || f.end_row < blk.start_row) continue;
            bool inside = false;
            for (const Buffer::MepmlColumnsBlock::Column &c : blk.columns)
                inside = inside || (f.start_row > c.first_row && f.end_row < c.last_row);
            sound = sound && inside;
        }
        std::vector<int> percents;
        int next = blk.start_row + 1;
        for (const Buffer::MepmlColumnsBlock::Column &c : blk.columns) {
            if (c.first_row != next || c.last_row < c.first_row || c.last_row >= blk.end_row || !starts(c.first_row, "\\column(") ||
                !starts(c.last_row, ")"))
                sound = false;
            next = c.last_row + 1;
            percents.push_back(c.percent);
        }
        if (!sound || next != blk.end_row) continue;
        const std::vector<int> widths = mepml::ColumnCols(percents, total, kGap);
        if (*std::min_element(widths.begin(), widths.end()) < kNarrowest) continue;
        int x = 0;
        for (size_t k = 0; k < blk.columns.size(); ++k) {
            for (int r = blk.columns[k].first_row; r <= blk.columns[k].last_row; ++r)
                buf.mepml_col_place[r] = Buffer::MepmlColumnPlace{x, widths[k], blk.start_row, blk.end_row, -1};
            x += widths[k] + kGap;
        }
        for (int r : blk.prose_rows) {
            const auto pl = buf.mepml_col_place.find(r);
            if (pl != buf.mepml_col_place.end()) pl->second.drawn_cols = mepml::ProseColumns(buf.lines[static_cast<size_t>(r)]);
        }
        // Each column's height, walked the way every slot walker walks.
        // (All of them before any move is set: a move is part of what a
        // row measures.)
        std::vector<int> heights;
        for (const Buffer::MepmlColumnsBlock::Column &c : blk.columns) {
            int h = 0;
            for (int r = c.first_row; r <= c.last_row; r = PaneNextDrawnRow(pane, buf, r)) h += PaneRowSlots(pane, buf, r, wrap_cols);
            heights.push_back(h);
        }
        for (size_t k = 0; k + 1 < heights.size(); ++k) buf.mepml_col_pad[blk.columns[k + 1].first_row] = -heights[k];
        const int tallest = *std::max_element(heights.begin(), heights.end());
        if (tallest != heights.back()) buf.mepml_col_pad[blk.end_row] = tallest - heights.back();
        // (The closing line is part of the row of columns, at full width:
        // a view whose top is there still draws the columns above it.)
        buf.mepml_col_place[blk.end_row] = Buffer::MepmlColumnPlace{0, total, blk.start_row, blk.end_row, -1};
    }
}

int Editor::RowTopPadSlots(const Buffer &buf, int row) const {
    if (buf.mepml_col_pad.empty()) return RowHeadroomSlots(buf, row);
    const auto pad = buf.mepml_col_pad.find(row);
    return RowHeadroomSlots(buf, row) + (pad == buf.mepml_col_pad.end() ? 0 : pad->second);
}

int Editor::RowHeadroomSlots(const Buffer &buf, int row) const {
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
        // A table row drawn as its wrapped layout has its formulas' room
        // in the layout's own lines: only its pictures are above it.
        if (TableWrapRowFor(buf, row)) return MepmlTableImageBoxes(buf, row, nullptr);
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
    // (`sublines` are the lines the row draws on: WrapLenForRow measures
    // it with its maths collapsed.)
    (void)wrap_cols;
    (void)cursor_row;
    const int lines = std::max(1, sublines);
    return (lines - 1) * (pad.top + pad.bottom) + pad.bottom;
}

int Editor::WrapLenForRow(const Buffer &buf, int row, int cursor_row) const {
    if (buf.mepml_single_line_rows.count(row) != 0) return 1;
    if (!buf.mepml_col_place.empty()) {
        const auto pl = buf.mepml_col_place.find(row);
        if (pl != buf.mepml_col_place.end() && pl->second.drawn_cols >= 0) return pl->second.drawn_cols;
    }
    const auto tbl = buf.mepml_table_row_cols.find(row);
    if (tbl != buf.mepml_table_row_cols.end()) return tbl->second;
    const std::string &line = buf.lines[static_cast<size_t>(row)];
    const int raw = static_cast<int>(line.size());
    if (row == cursor_row && !buf.mepml_view) return raw;
    // DrawPane's collapse (its `conceal_runs`), from the same registries.
    std::vector<std::array<int, 3>> runs;
    if (org_conceal_visible_ || buf.mepml_view) {
        const auto it = buf.mepml_row_conceal.find(row);
        if (it != buf.mepml_row_conceal.end()) runs = it->second;
    }
    const auto maths = org_latex_visible_ ? buf.org_latex_inline.find(row) : buf.org_latex_inline.end();
    if (maths != buf.org_latex_inline.end() && render_char_width_ > 0.0) {
        // An org table row hands a cell's slack back before its `|`: the
        // row is as long as it was.
        const size_t first_glyph = line.find_first_not_of(" \t");
        if (first_glyph != std::string::npos && line[first_glyph] == '|' && LspFiletype(buf.filename) != "mepml") return raw;
        std::ptrdiff_t marked = static_cast<std::ptrdiff_t>(runs.size());  // the markup's runs: the first `marked`
        for (const Buffer::OrgLatexInlineSpan &sp : maths->second) {
            if (sp.col_end <= sp.col_start || OrgLatexInlineRevealed(buf, sp, row, cursor_row)) continue;
            int drawn = 0;  // a fragment's continuation rows collapse to nothing
            if (!sp.path.empty()) {
                if (sp.width <= 0) continue;  // unreadable: its source stays
                drawn = std::max(1, static_cast<int>(std::ceil(static_cast<double>(sp.width) / render_char_width_ - 0.05)));
            }
            // A formula owns its whole source span: the markup concealed
            // inside it (its delimiters, an \alttext()) goes with it.
            const auto inside = std::remove_if(runs.begin(), runs.begin() + marked, [&](const std::array<int, 3> &r) {
                return r[0] < sp.col_end && r[1] > sp.col_start;
            });
            const std::ptrdiff_t kept = inside - runs.begin();
            runs.erase(inside, runs.begin() + marked);
            marked = kept;
            runs.push_back({sp.col_start, sp.col_end, drawn});
        }
    }
    // (A headline is drawn at its own size, with nothing collapsed.)
    if (runs.empty() || HeadingLevelForRow(buf, row) > 0) return raw;
    std::sort(runs.begin(), runs.end());
    int len = raw, end = 0;
    for (const std::array<int, 3> &r : runs) {
        if (r[0] < end || r[1] > raw) continue;  // only the first of an overlapping pair counts
        end = r[1];
        len -= (r[1] - r[0]) - r[2];
    }
    return std::max(1, len);
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
    if (row == cursor_row && !it->second.keep_under_cursor && !buf.mepml_view) return nullptr;
    if (std::hash<std::string>{}(buf.lines[static_cast<size_t>(row)]) != it->second.text_hash) return nullptr;
    return &it->second;
}

bool Editor::MepmlToggleRaw() {
    Buffer &buf = Buf();
    buf.mepml_raw = !buf.mepml_raw;
    return buf.mepml_raw;
}

bool Editor::MepmlToggleView() {
    Buffer &buf = Buf();
    buf.mepml_view = !buf.mepml_view;
    // A view is of the rendered document: raw text (<leader>kr) would show
    // none of it, so entering the view leaves raw.
    if (buf.mepml_view) buf.mepml_raw = false;
    return buf.mepml_view;
}

bool Editor::MepmlView(int buffer_id) const {
    if (buffer_id < 0 || buffer_id >= static_cast<int>(buffers_.size())) return false;
    return buffers_[static_cast<size_t>(buffer_id)].mepml_view;
}

bool Editor::MepmlRaw(int buffer_id) const {
    if (buffer_id < 0 || buffer_id >= static_cast<int>(buffers_.size())) return false;
    return buffers_[static_cast<size_t>(buffer_id)].mepml_raw;
}

bool Editor::MepmlIsPresentation(int buffer_id) const {
    if (buffer_id < 0 || buffer_id >= static_cast<int>(buffers_.size())) return false;
    return buffers_[static_cast<size_t>(buffer_id)].mepml_presentation;
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
        c.paths = mepml::ElementPaths();
        // A presented page is a piece of its document: inside its slide,
        // and the title page's lines the header's title, subtitle, author.
        mepml::HighlightContext context;
        const bool page = MepmlPresentContext(&context);
        c.spans = mepml::Highlight(c.doc, &c.paths, &c.block_nodes, page ? &context : nullptr);
        c.spans_valid = true;
        c.styles_generation = 0;
    }
    return c.spans;
}

bool Editor::MepmlPresentContext(mepml::HighlightContext *out) const {
    if (!present_.active || CurrentBufferId() != present_.view_buffer || present_.pages.empty()) return false;
    const mepml::PresentationPage &page = present_.pages[static_cast<size_t>(std::clamp(present_.page, 0, static_cast<int>(present_.pages.size()) - 1))];
    out->container = mepml::Element("slide", "number", std::to_string(page.number));
    out->title_page = page.number == 0;
    if (out->title_page) out->container.With("title");
    return true;
}

const std::vector<int> &Editor::MepmlBlockNodesCurrent(bool with_imports) const {
    MepmlSpansCurrent(with_imports);
    return MepmlCacheEntry(with_imports).block_nodes;
}

std::shared_ptr<const mepml::style::Sheet> Editor::MepmlSheetFile(const std::string &path, std::string *signature) const {
    const long long mtime = FileMtime(path);
    *signature += path + "@" + std::to_string(mtime) + ";";
    MepmlSheetFileEntry &e = mepml_sheet_files_[path];
    if (e.sheet && e.mtime == mtime) return e.sheet;
    e.mtime = mtime;
    std::vector<std::string> lines;
    // (Not `mtime < 0`: file times count from an epoch in the future.)
    if (!ReadFileLines(path, &lines)) {
        e.sheet = nullptr;
        return nullptr;
    }
    std::string text;
    for (const std::string &l : lines) text += l + "\n";
    e.sheet = std::make_shared<mepml::style::Sheet>(mepml::style::Parse(text, path));
    return e.sheet;
}

std::vector<std::shared_ptr<const mepml::style::Sheet>> Editor::MepmlSheets(std::string *signature) const {
    // The built-in sheet is mep's default look (assets/mepml/default.mepss).
    static const std::shared_ptr<const mepml::style::Sheet> builtin(&mepml::style::DefaultSheet(), [](const mepml::style::Sheet *) {});
    std::vector<std::shared_ptr<const mepml::style::Sheet>> out = {builtin};
    std::string sig;
    // The user's own, for every document.
    std::string config;
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) config = xdg;
    else if (const char *home = std::getenv("HOME"); home && *home) config = std::string(home) + "/.config";
    if (!config.empty())
        if (auto user = MepmlSheetFile(config + "/mep/mepml.mepss", &sig)) out.push_back(std::move(user));
    // Then the ones the document names (`//? Style:`), its imports' first.
    // A presented slide is styled by the document it is a page of.
    // Last, the sheets written in the document itself (`\raw(style, ...)`).
    auto inline_sheet = [&](const std::string &text) {
        const std::string key = "inline:" + std::to_string(std::hash<std::string>{}(text));
        sig += key + ";";
        MepmlSheetFileEntry &e = mepml_sheet_files_[key];
        if (!e.sheet) e.sheet = std::make_shared<mepml::style::Sheet>(mepml::style::Parse(text, "document"));
        out.push_back(e.sheet);
    };
    if (present_.active && CurrentBufferId() == present_.view_buffer) {
        for (const std::string &path : present_.sheet_paths)
            if (auto sheet = MepmlSheetFile(path, &sig)) out.push_back(std::move(sheet));
        for (const std::string &text : present_.inline_sheets) inline_sheet(text);
    } else if (IsMepmlBuffer()) {
        const std::string file = MepmlCurrentFile();
        const mepml::Document &doc = MepmlParseCurrent(true);
        for (const mepml::StyleRef &ref : doc.styles)
            if (auto sheet = MepmlSheetFile(mepml::ResolvePath(ref.base.empty() ? file : ref.base, ref.path), &sig))
                out.push_back(std::move(sheet));
        for (const std::string &text : mepml::InlineStyleSheets(doc)) inline_sheet(text);
    }
    if (signature) *signature = sig;
    return out;
}

bool Editor::MepmlStylesStale() {
    if (!IsMepmlBuffer()) return false;
    auto it = mepml_scan_state_.find(CurrentBufferId());
    if (it == mepml_scan_state_.end() || !it->second.valid) return false;
    // A sheet saved since the last scan: looked for a few times a second,
    // not every frame.
    const auto now = std::chrono::steady_clock::now();
    if (now - mepml_sheets_checked_ < std::chrono::milliseconds(300)) return false;
    mepml_sheets_checked_ = now;
    if (!MepmlParseReady()) return false;  // (never a parse on this thread just to look)
    std::string sig;
    MepmlSheets(&sig);
    return sig != it->second.sheet_signature;
}

const Editor::MepmlNodeStyles &Editor::MepmlStylesCurrent(bool with_imports) const {
    MepmlSpansCurrent(with_imports);
    MepmlParseCache &c = MepmlCacheEntry(with_imports);
    std::string signature;
    std::vector<std::shared_ptr<const mepml::style::Sheet>> sheets = MepmlSheets(&signature);
    if (c.styles_generation == mepml_sheet_generation_ && c.styles_signature == signature) return c.styles;
    mepml::style::Cascade cascade;
    cascade.sheets = std::move(sheets);
    const bool presenting = present_.active && CurrentBufferId() == present_.view_buffer;
    cascade.media = presenting ? std::vector<std::string>{"present", "slides", "screen"} : std::vector<std::string>{"editor", "screen"};
    c.styles.rendered = cascade.ComputeAll(c.paths);
    cascade.media.push_back("source");
    c.styles.source = cascade.ComputeAll(c.paths);
    // The background behind a node's text: its own, else the nearest
    // inline ancestor's (a block's background is its box's, not its text's).
    c.styles.inline_bg.assign(c.paths.nodes.size(), mepml::style::Color());
    for (size_t n = 0; n < c.paths.nodes.size(); ++n) {
        const mepml::ElementPaths::Node &node = c.paths.nodes[n];
        if (mepml::IsBlockElement(node.element.name) && node.element.part.empty()) continue;
        if (c.styles.rendered[n].background.kind != mepml::style::Color::None) c.styles.inline_bg[n] = c.styles.rendered[n].background;
        else if (node.parent >= 0) c.styles.inline_bg[n] = c.styles.inline_bg[static_cast<size_t>(node.parent)];
    }
    c.styles_generation = mepml_sheet_generation_;
    c.styles_signature = signature;
    c.part_styles.clear();
    // The characters the sheets' generated text uses, for the renderer to
    // bake glyphs for (it has ASCII and a fixed set otherwise).
    for (const std::vector<mepml::style::Computed> *set : {&c.styles.rendered, &c.styles.source}) {
        for (const mepml::style::Computed &st : *set) {
            if (!st.has_content) continue;
            const std::string &t = st.content;
            for (size_t i = 0; i < t.size();) {
                const unsigned char b = static_cast<unsigned char>(t[i]);
                const size_t len = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
                if (len > 1 && i + len <= t.size()) {
                    int cp = b & (0xFF >> (len + 1));
                    for (size_t k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(t[i + k]) & 0x3F);
                    if (mepml_glyphs_.insert(cp).second) ++mepml_glyph_generation_;
                }
                i += len;
            }
        }
    }
    return c.styles;
}

const mepml::style::Computed &Editor::MepmlChainStyle(int node, const std::vector<mepml::Element> &chain) const {
    const MepmlNodeStyles &styles = MepmlStylesCurrent(true);
    MepmlParseCache &c = MepmlCacheEntry(true);
    static const mepml::style::Computed kNone;
    if (node < 0 || static_cast<size_t>(node) >= c.paths.nodes.size()) return kNone;
    if (chain.empty()) return styles.rendered[static_cast<size_t>(node)];
    std::string key;
    for (const mepml::Element &e : chain) {
        key += e.name + ":" + e.part;
        for (const auto &kv : e.attrs) key += "[" + kv.first + "=" + kv.second + "]";
        key += ">";
    }
    auto it = c.part_styles.find({node, key});
    if (it != c.part_styles.end()) return it->second;
    mepml::style::Cascade cascade;
    cascade.sheets = MepmlSheets();
    const bool presenting = present_.active && CurrentBufferId() == present_.view_buffer;
    cascade.media = presenting ? std::vector<std::string>{"present", "slides", "screen"} : std::vector<std::string>{"editor", "screen"};
    std::vector<const mepml::Element *> path = c.paths.Path(node);
    mepml::style::Computed st = styles.rendered[static_cast<size_t>(node)];
    for (const mepml::Element &e : chain) {
        path.push_back(&e);
        st = cascade.Compute(path, st);
    }
    return c.part_styles.emplace(std::make_pair(node, key), std::move(st)).first->second;
}

mepml::style::Computed Editor::MepmlStateStyle(int node, const char *state) const {
    const MepmlNodeStyles &styles = MepmlStylesCurrent(true);
    const MepmlParseCache &c = MepmlCacheEntry(true);
    if (node < 0 || static_cast<size_t>(node) >= c.paths.nodes.size()) return mepml::style::Computed();
    mepml::style::Cascade cascade;
    cascade.sheets = MepmlSheets();
    const bool presenting = present_.active && CurrentBufferId() == present_.view_buffer;
    cascade.media = presenting ? std::vector<std::string>{"present", "slides", "screen"} : std::vector<std::string>{"editor", "screen"};
    std::vector<const mepml::Element *> path = c.paths.Path(node);
    mepml::Element in_state = c.paths.nodes[static_cast<size_t>(node)].element;
    in_state.With(state);
    path.back() = &in_state;
    const int parent = c.paths.nodes[static_cast<size_t>(node)].parent;
    static const mepml::style::Computed kRoot;
    return cascade.Compute(path, parent >= 0 ? styles.rendered[static_cast<size_t>(parent)] : kRoot);
}

const mepml::style::Computed &Editor::MepmlPartStyle(int node, const std::string &part) const {
    const MepmlParseCache &c = MepmlCacheEntry(true);
    static const mepml::style::Computed kNone;
    if (node < 0 || static_cast<size_t>(node) >= c.paths.nodes.size()) return kNone;
    return MepmlChainStyle(node, {c.paths.nodes[static_cast<size_t>(node)].element.Part(part)});
}

void Editor::MepmlStyleRendered(std::vector<mepml::RenderedLine> *lines, int node, const mepml::Element *under, bool titled) const {
    const MepmlParseCache &c = MepmlCacheEntry(true);
    if (node < 0 || static_cast<size_t>(node) >= c.paths.nodes.size()) return;
    const mepml::Element &own = c.paths.nodes[static_cast<size_t>(node)].element;
    for (size_t li = 0; li < lines->size(); ++li) {
        for (mepml::RenderedSpan &sp : (*lines)[li].spans) {
            // What the span is, from what its flags say of it.
            std::vector<mepml::Element> chain;
            if (under) chain.push_back(*under);
            if (titled && li == 0) {
                chain.push_back(own.Part("title"));
            } else if (sp.style & mepml::kHeading) {
                chain.push_back(mepml::Element("heading", "level", std::to_string(sp.heading_level)));
            } else if (under && (sp.style & mepml::kDirective)) {
                chain.push_back(under->Part("label"));  // "Figure 1:"
            } else {
                if (sp.style & mepml::kComment) chain.push_back(mepml::Element("comment"));
                if (sp.style & mepml::kBold) chain.push_back(mepml::Element("bold"));
                if (sp.style & mepml::kItalic) chain.push_back(mepml::Element("italic"));
                if (sp.style & mepml::kMono) chain.push_back(mepml::Element("mono"));
                if (sp.style & mepml::kVerbatim) chain.push_back(mepml::Element("verbatim"));
                if (sp.style & mepml::kMath) chain.push_back(mepml::Element("math"));
                if (sp.style & mepml::kLink) chain.push_back(mepml::Element("link"));
                if (sp.style & mepml::kCite) chain.push_back(mepml::Element("cite"));
            }
            const mepml::style::Computed &st = MepmlChainStyle(node, chain);
            sp.hl = st.has_color ? StyleHl(st.color) : std::string("Normal");
            sp.style = (sp.style & ~(mepml::kBold | mepml::kItalic)) | (st.bold ? mepml::kBold : 0u) | (st.italic ? mepml::kItalic : 0u);
        }
    }
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
            plain.paths = std::move(r.plain_paths);
            plain.block_nodes = std::move(r.plain_block_nodes);
            plain.styles_generation = 0;
            plain.spans_valid = true;
            plain.lines = r.lines;
            plain.valid = true;
            plain.has_imports = r.has_imports;
            plain.generation = ++mepml_parse_generation_;
            if (r.has_imports) {
                MepmlParseCache &imp = mepml_parse_cache_[1][id];
                imp.doc = std::move(r.imports);
                imp.spans = std::move(r.imports_spans);
                imp.paths = std::move(r.imports_paths);
                imp.block_nodes = std::move(r.imports_block_nodes);
                imp.styles_generation = 0;
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
            r.plain_spans = mepml::Highlight(r.plain, &r.plain_paths, &r.plain_block_nodes);
            if (r.has_imports) {
                r.imports = mepml::ParseWithImports(file, r.lines, [&r](const std::string &path, std::vector<std::string> *out) {
                    r.reads.emplace_back(path, FileMtime(path));
                    return ReadFileLines(path, out);
                });
                r.imports_spans = mepml::Highlight(r.imports, &r.imports_paths, &r.imports_block_nodes);
            }
            return r;
        });
        a.running = true;
    }
    return false;
}

namespace {
// A set of tabs (`\tabs(` ... `)`) as the editor shows it: closed, and
// holding nothing but its `\tab(Title,` ... `)`s (comments aside).
struct MepmlTabSet {
    struct Tab {
        int open_row = 0, close_row = 0;
        std::string title;
    };
    int open_row = 0, close_row = 0;
    std::vector<Tab> tabs;
};
// The document's sets of tabs, in the order they open: a set's place in it
// is its key in Buffer::mepml_tab_active.
std::vector<MepmlTabSet> MepmlTabSets(const mepml::Document &doc) {
    std::vector<MepmlTabSet> out;
    struct Open {
        bool tabs = false, tab = false, sound = true;
        MepmlTabSet set;
    };
    std::vector<Open> open;  // the boxes, columns and tabs open, innermost last
    for (const mepml::Block &b : doc.blocks) {
        if (!b.origin.empty()) continue;
        if (b.kind == mepml::BlockKind::SlideBegin || b.kind == mepml::BlockKind::SlideEnd) {
            open.clear();  // (a set left open ends with its slide: not shown as tabs)
            continue;
        }
        const bool opens = (b.kind == mepml::BlockKind::BoxBegin && !b.box_closed) || b.kind == mepml::BlockKind::LayoutBegin;
        const bool closes = b.kind == mepml::BlockKind::BoxEnd || b.kind == mepml::BlockKind::LayoutEnd;
        if (closes) {
            if (open.empty()) continue;
            Open done = std::move(open.back());
            open.pop_back();
            if (done.tab && !open.empty() && open.back().tabs && !open.back().set.tabs.empty())
                open.back().set.tabs.back().close_row = b.line_end;
            if (done.tabs && done.sound && !done.set.tabs.empty()) {
                done.set.close_row = b.line_end;
                out.push_back(std::move(done.set));
            }
            continue;
        }
        // Directly inside a set: a tab, or a comment; anything else and the
        // set is shown as written.
        const bool in_set = !open.empty() && open.back().tabs;
        const bool is_tab = b.kind == mepml::BlockKind::LayoutBegin && b.keyword == "tab";
        if (in_set) {
            if (is_tab) {
                MepmlTabSet::Tab t;
                t.open_row = t.close_row = b.line_start;
                t.title = b.caption.empty() ? "Tab " + std::to_string(open.back().set.tabs.size() + 1) : b.caption;
                open.back().set.tabs.push_back(std::move(t));
            } else if (b.kind != mepml::BlockKind::Comment && b.kind != mepml::BlockKind::Callout) {
                open.back().sound = false;
            }
        }
        if (opens) {
            Open o;
            o.tabs = b.kind == mepml::BlockKind::LayoutBegin && b.keyword == "tabs";
            o.tab = is_tab && in_set;
            o.set.open_row = b.line_start;
            open.push_back(std::move(o));
        }
    }
    std::sort(out.begin(), out.end(), [](const MepmlTabSet &a, const MepmlTabSet &b) { return a.open_row < b.open_row; });
    return out;
}
size_t MepmlTabsSig(const std::vector<MepmlTabSet> &sets) {
    size_t h = sets.size();
    auto mix = [&h](size_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); };
    for (const MepmlTabSet &set : sets) {
        mix(static_cast<size_t>(set.open_row));
        mix(static_cast<size_t>(set.close_row));
        for (const MepmlTabSet::Tab &t : set.tabs) {
            mix(static_cast<size_t>(t.open_row));
            mix(static_cast<size_t>(t.close_row));
            mix(std::hash<std::string>{}(t.title));
        }
    }
    return h;
}
}  // namespace

void Editor::MepmlScan(int ns, bool own_diagnostics) {
    Buffer &buf = Buf();
    // A document just opened: its folds are built now, rather than lazily
    // by the first fold command, so the code its exports leave out starts
    // out folded (Buffer::mepml_folds_seeded).
    if (!buf.mepml_folds_seeded && !(present_.active && CurrentBufferId() == present_.view_buffer))
        RecomputeMepmlFolds();
    // Tabs written, removed or retitled since their folds were built: built
    // again, so a set shows as tabs as soon as it is closed.
    else if (IsMepmlBuffer() && !(present_.active && CurrentBufferId() == present_.view_buffer) &&
             MepmlTabsSig(MepmlTabSets(MepmlParseCurrent(false))) != buf.mepml_tabs_sig)
        RecomputeMepmlTabFolds();
    int cur_row = 0, cur_col = 0;
    GetCursorForLua(&cur_row, &cur_col);
    // View mode (Buffer::mepml_view): no row is the cursor's, so nothing
    // below puts source back for it -- the scan is of the document alone.
    if (buf.mepml_view) cur_row = -1;
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
    std::string sheet_signature;
    if (entry) MepmlSheets(&sheet_signature);
    std::unordered_set<int> patch_rows, patch_tables;
    const bool same_inputs = entry && state.valid && state.doc == entry && state.generation == entry->generation &&
                             state.ns == ns && state.own_diagnostics == own_diagnostics && state.conceal == conceal &&
                             state.images == images && state.text_width == text_width &&
                             state.pane_cols == scan_pane_cols && state.buffer_cols == scan_buffer_cols &&
                             state.table_math_gen == buf.mepml_table_math_gen &&
                             state.sheet_signature == sheet_signature && state.view == buf.mepml_view &&
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
        buf.mepml_heading_scale.clear();
        buf.mepml_heading_look.clear();
        buf.mepml_row_align.clear();
        buf.mepml_page_bg = OrgCardColor();
        buf.mepml_row_scale.clear();
        buf.mepml_table_images.clear();
        buf.mepml_table_row_cols.clear();
        if (IsMepmlBuffer()) buf.org_table_wrap_rows.clear();  // (its tables laid out wrapped: MepmlTableLayout)
        buf.mepml_virtual_rows.clear();
        buf.mepml_fold_summaries.clear();
        buf.mepml_html_rows.clear();
        buf.mepml_single_line_rows.clear();
        buf.mepml_alt_notes.clear();
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
    buf.mepml_presentation = mepml::IsPresentation(doc);
    // Rows of columns (\columns): each one's lines and its columns', for
    // MepmlColumnsPlace. One inside another's column is not set side by
    // side itself, and neither is one holding anything but columns.
    if (!patch) {
        buf.mepml_columns.clear();
        buf.mepml_col_place.clear();
        buf.mepml_col_pad.clear();
        Buffer::MepmlColumnsBlock cur;
        int depth = 0;  // the containers open inside (and counting) the row: 0 outside one
        bool sound = true;
        for (const mepml::Block &b : doc.blocks) {
            if (!b.origin.empty()) continue;
            const bool opens = (b.kind == mepml::BlockKind::BoxBegin && !b.box_closed) || b.kind == mepml::BlockKind::LayoutBegin;
            const bool closes = b.kind == mepml::BlockKind::BoxEnd || b.kind == mepml::BlockKind::LayoutEnd;
            if (b.kind == mepml::BlockKind::SlideBegin || b.kind == mepml::BlockKind::SlideEnd) {
                depth = 0;  // (a row left open ends with its slide: not laid out)
                continue;
            }
            if (depth == 0) {
                if (b.kind == mepml::BlockKind::LayoutBegin && b.keyword == "columns") {
                    cur = Buffer::MepmlColumnsBlock();
                    cur.start_row = b.line_start;
                    depth = 1;
                    sound = true;
                }
                continue;
            }
            if (depth == 1 && !closes) {
                if (b.kind == mepml::BlockKind::LayoutBegin && b.keyword == "column") {
                    Buffer::MepmlColumnsBlock::Column c;
                    c.first_row = b.line_start;
                    c.percent = mepml::ColumnPercent(b);
                    cur.columns.push_back(c);
                } else {
                    sound = false;
                }
            }
            if (depth >= 2 && (b.kind == mepml::BlockKind::Paragraph || b.kind == mepml::BlockKind::List))
                for (int r = b.line_start; r <= b.line_end; ++r) cur.prose_rows.push_back(r);
            if (opens) ++depth;
            if (closes) {
                --depth;
                if (depth == 1 && !cur.columns.empty()) cur.columns.back().last_row = b.line_end;
                if (depth == 0) {
                    cur.end_row = b.line_end;
                    if (sound && cur.columns.size() >= 2) buf.mepml_columns.push_back(cur);
                }
            }
        }
    }
    // The text columns a row has where it is in such a column (a caption
    // is wrapped and centred in its column's width), else `cols`.
    auto column_cols = [&](int row, int cols) {
        for (const Buffer::MepmlColumnsBlock &blk : buf.mepml_columns) {
            if (row <= blk.start_row || row >= blk.end_row) continue;
            std::vector<int> percents;
            for (const Buffer::MepmlColumnsBlock::Column &c : blk.columns) percents.push_back(c.percent);
            const std::vector<int> widths = mepml::ColumnCols(percents, std::max(1, CurPane().text_cols - 1));
            for (size_t k = 0; k < blk.columns.size(); ++k)
                if (row >= blk.columns[k].first_row && row <= blk.columns[k].last_row) return std::min(cols, widths[k]);
        }
        return cols;
    };
    const int n = buf.LineCount();
    if (!patch) {
    // A presented page's paper: its slide's `background`.
    {
        mepml::HighlightContext page;
        if (MepmlPresentContext(&page)) {
            MepmlSpansCurrent(true);  // (the tree the chain hangs from)
            const mepml::style::Color &bg = MepmlChainStyle(0, {page.container}).background;
            if (bg.kind != mepml::style::Color::None) {
                buf.mepml_page_bg.set = true;
                buf.mepml_page_bg.hl = StyleHl(bg);
                buf.mepml_page_bg.alpha = bg.alpha;
            }
        }
    }
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
        if (b.kind == mepml::BlockKind::Heading) {
            buf.mepml_heading_rows[b.line_start] = b.level;
            // Its size is its element's font-size.
            const size_t bi = static_cast<size_t>(&b - doc.blocks.data());
            const std::vector<int> &nodes = MepmlBlockNodesCurrent(true);
            const MepmlNodeStyles &heading_styles = MepmlStylesCurrent(true);
            const int node = bi < nodes.size() ? nodes[bi] : -1;
            if (node >= 0 && static_cast<size_t>(node) < heading_styles.rendered.size()) {
                const mepml::style::Computed &hs = heading_styles.rendered[static_cast<size_t>(node)];
                buf.mepml_heading_scale[b.line_start] = std::clamp(hs.font_size, 0.5f, 3.0f);
                Buffer::MepmlHeadingLook look;
                look.bold = hs.bold;
                look.italic = hs.italic;
                look.align = hs.text_align == mepml::style::TextAlign::Center ? 1 : hs.text_align == mepml::style::TextAlign::Right ? 2 : 0;
                look.underline = hs.underline;
                look.strike = hs.strike;
                if (hs.has_decoration_color) look.line_hl = StyleHl(hs.decoration_color);
                else if (hs.has_color) look.line_hl = StyleHl(hs.color);
                if (look.bold || look.italic || look.align || look.underline || look.strike) buf.mepml_heading_look[b.line_start] = look;
            }
        }
        // Running text is set the way its element's text-align says.
        if (b.kind == mepml::BlockKind::Paragraph || b.kind == mepml::BlockKind::Abstract || b.kind == mepml::BlockKind::Callout ||
            b.kind == mepml::BlockKind::List) {
            const size_t bi = static_cast<size_t>(&b - doc.blocks.data());
            const std::vector<int> &nodes = MepmlBlockNodesCurrent(true);
            const MepmlNodeStyles &text_styles = MepmlStylesCurrent(true);
            const int node = bi < nodes.size() ? nodes[bi] : -1;
            if (node >= 0 && static_cast<size_t>(node) < text_styles.rendered.size()) {
                const mepml::style::TextAlign ta = text_styles.rendered[static_cast<size_t>(node)].text_align;
                const int align = ta == mepml::style::TextAlign::Center ? 1 : ta == mepml::style::TextAlign::Right ? 2 : 0;
                for (int r = b.line_start; align != 0 && r <= b.line_end && r < buf.LineCount(); ++r) {
                    // (Not a line that only opens or closes the block: `\abstract(`, `)`.)
                    const std::string &text = buf.lines[static_cast<size_t>(r)];
                    const size_t last = text.find_last_not_of(" \t");
                    if (last == std::string::npos || text[last] == '(' || text.find_first_not_of(" \t") == last) continue;
                    buf.mepml_row_align[r] = align;
                }
            }
        }
        // A caption and an alt text draw as their text wrapped to the text
        // width in place of their source rows (one line or several) --
        // centred under a figure (an \image, or a code block that drew one),
        // which is drawn centred itself; the alt text small. Their raw rows come back
        // while the cursor is in them (OrgLatexRenderForRow).
        if (conceal && (b.caption_line >= 0 || b.alt_line >= 0)) {
            const bool centred = mepml::IsFigure(b) ||
                                 (b.kind == mepml::BlockKind::Code && !b.result_images.empty());
            int width = TextWidth();
            if (CurPane().text_cols > 8) width = std::min(width, CurPane().text_cols - 1);
            width = column_cols(b.caption_line >= 0 ? b.caption_line : b.alt_line, width);
            auto place = [&](int first, int last, std::vector<mepml::RenderedLine> lines, float scale) {
                if (lines.empty() || first < 0 || last < first || last >= n) return;
                Buffer::OrgLatexRender r;
                r.styled = std::move(lines);
                r.styled_scale = scale;
                r.end_row = last;
                r.slots = static_cast<int>(r.styled.size());
                buf.mepml_html_rows[first] = std::move(r);
            };
            // Their look is their elements' (`caption`, its `::label`,
            // `alt-text`), as the sheets compute it under this block.
            const size_t idx = static_cast<size_t>(&b - doc.blocks.data());
            const std::vector<int> &owner_nodes = MepmlBlockNodesCurrent(true);
            const int owner = idx < owner_nodes.size() ? owner_nodes[idx] : -1;
            if (b.caption_line >= 0) {
                std::string of = "code";
                if (mepml::IsFigure(b) || !b.result_images.empty()) of = "figure";
                else if (b.kind == mepml::BlockKind::Table) of = "table";
                else if (b.kind == mepml::BlockKind::MathBlock) of = "math";
                const mepml::Element caption("caption", "of", of);
                // "Figure 3": the number is the document's, the word round
                // it the sheets' (`caption::label { content: "Fig. %n" }`).
                std::string label = idx < block_labels.size() ? block_labels[idx] : std::string();
                if (!label.empty() && owner >= 0) {
                    const mepml::style::Computed &ls = MepmlChainStyle(owner, {caption, caption.Part("label")});
                    const size_t digits = label.find_last_not_of("0123456789");
                    if (ls.has_content && digits != std::string::npos && digits + 1 < label.size())
                        label = mepml::style::ExpandContent(ls.content, label.substr(digits + 1), of, "");
                }
                std::vector<mepml::RenderedLine> lines = mepml::RenderCaption(doc, b, label, width, centred);
                MepmlStyleRendered(&lines, owner, &caption, false);
                place(b.caption_line, b.caption_line_end, std::move(lines), 1.0f);
            }
            // An alt text is for a reader who cannot see the page, so it
            // takes no room on it: its rows collapse to nothing, and while
            // the cursor is on the block it describes it shows in a popup
            // by the block (Buffer::mepml_alt_notes, drawn by DrawPane).
            // On its own rows the cursor finds its source, like a
            // caption's. An empty one (`\alttext()`: decoration) collapses
            // the same way.
            if (b.alt_line >= 0 && b.alt_line_end >= b.alt_line && b.alt_line_end < n) {
                Buffer::OrgLatexRender gone;
                gone.slots = 0;
                gone.end_row = b.alt_line_end;
                buf.mepml_html_rows[b.alt_line] = std::move(gone);
                buf.mepml_alt_notes.push_back({b.line_start, b.line_end, b.alt_line, b.alt_line_end, b.alt});
                // Its source, under the cursor, stays on one line too.
                for (int row = b.alt_line; row <= b.alt_line_end; ++row) buf.mepml_single_line_rows.insert(row);
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
            {
                const size_t vi = static_cast<size_t>(&b - doc.blocks.data());
                const std::vector<int> &vnodes = MepmlBlockNodesCurrent(true);
                const int vnode = vi < vnodes.size() ? vnodes[vi] : -1;
                MepmlStyleRendered(&vb.lines, vnode, nullptr, true);
                // The card behind the list: the element's background and outline.
                const MepmlNodeStyles &all = MepmlStylesCurrent(true);
                if (vnode >= 0 && static_cast<size_t>(vnode) < all.rendered.size()) {
                    vb.wash = CardColorOf(all.rendered[static_cast<size_t>(vnode)].background);
                    vb.border = CardColorOf(all.rendered[static_cast<size_t>(vnode)].border_color);
                }
            }
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
        // SVG and HTML written in the document: its lines draw as the
        // rendered markup, at the height it lays out to -- its source back
        // while the cursor is in it (OrgLatexRenderForRow).
        if ((b.kind == mepml::BlockKind::Svg || b.kind == mepml::BlockKind::Html) && html_measure_ &&
            b.code_line_end >= b.code_line_start && b.code_line_end < n) {
            Buffer::OrgLatexRender r;
            // (An SVG is a figure: centred, as an image is, over its caption.)
            r.html = b.kind == mepml::BlockKind::Svg ? "<div style=\"text-align:center\">" + b.code + "</div>" : b.code;
            const std::string file = MepmlCurrentFile();
            r.base_dir = file.empty() ? std::string(".") : std::filesystem::path(file).parent_path().string();
            r.end_row = b.code_line_end;
            r.html_cols = column_cols(b.code_line_start, TextWidth());
            if (CurPane().text_cols > 8) r.html_cols = std::min(r.html_cols, CurPane().text_cols - 3);
            r.slots = std::max(1, html_measure_(r.html, r.base_dir, r.html_cols));
            buf.mepml_html_rows[b.code_line_start] = std::move(r);
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
    // Inline formulas' alt texts (`\(x\)\alttext(...)`), each over its own
    // source.
    if (!patch && conceal) {
        std::function<void(const mepml::Block &, const std::vector<mepml::Inline> &)> walk =
            [&](const mepml::Block &b, const std::vector<mepml::Inline> &ins) {
                for (const mepml::Inline &x : ins) {
                    if (x.kind == mepml::InlineKind::Math && !x.alt.empty()) {
                        const mepml::Block::Pos p = b.OffsetToPos(x.start), q = b.OffsetToPos(x.end);
                        if (p.line >= 0 && q.line >= p.line && q.line < n)
                            buf.mepml_alt_notes.push_back({p.line, q.line, -1, -1, x.alt, p.col, q.col});
                    }
                    walk(b, x.children);
                }
            };
        for (const mepml::Block &b : doc.blocks) {
            if (!b.origin.empty()) continue;
            walk(b, b.inlines);
            walk(b, b.caption_inlines);
            for (const mepml::ListItem &it : b.items) walk(b, it.content);
            for (const auto &cells : b.rows)
                for (const mepml::TableCell &c : cells) walk(b, c.content);
        }
    }
    // A presented slide's alt texts: their lines are not on the page
    // (PresentationPage::alts), so each is hung on the block that ends
    // where what it describes does.
    if (!patch && conceal && present_.active && CurrentBufferId() == present_.view_buffer && !present_.pages.empty()) {
        const mepml::PresentationPage &page =
            present_.pages[static_cast<size_t>(std::clamp(present_.page, 0, static_cast<int>(present_.pages.size()) - 1))];
        for (const auto &alt : page.alts) {
            const int row = alt.first + 1;  // (the view's lines start with a blank one)
            for (const mepml::Block &b : doc.blocks) {
                if (!b.origin.empty() || row < b.line_start || row > b.line_end) continue;
                buf.mepml_alt_notes.push_back({b.line_start, b.line_end, -1, -1, alt.second});
                break;
            }
        }
    }
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

    // How each span looks is the style the sheets compute for its element
    // (docs/mepml-spec/rendering.md): nothing below decides a colour, a
    // weight, a size or a generated word of its own.
    const MepmlNodeStyles &styles = MepmlStylesCurrent(true);
    const mepml::ElementPaths &paths = MepmlCacheEntry(true).paths;
    static const mepml::style::Computed kNoStyle;
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

        // A line shown as its source (the cursor's, or every line with
        // concealment off) is styled as source: its markup is `::markup`.
        const bool source = !conceal || on_cursor;
        const int node = source ? s.source_path : s.path;
        const bool has_node = node >= 0 && static_cast<size_t>(node) < styles.rendered.size();
        const mepml::style::Computed &st =
            !has_node ? kNoStyle : source ? styles.source[static_cast<size_t>(node)] : styles.rendered[static_cast<size_t>(node)];
        std::string hl = st.has_color ? StyleHl(st.color) : std::string();
        // The author's own \color(): drawn as a literal colour, as it was
        // before sheets (unless something inside it has a colour of its own).
        std::uint32_t literal = 0;
        const bool literal_color = !s.color.empty() && mepml::ParseColor(s.color, &literal) && st.has_color &&
                                   st.color.kind == mepml::style::Color::Rgb && st.color.rgb == literal;

        // Inside a table the grid's band is one line tall, so a run is
        // capped at what fits a line rather than given headroom.
        const float full_scale = std::clamp(st.font_size, 0.5f, 3.0f);
        const float scale = table_rows.count(s.line) ? std::min(full_scale, kTableMaxScale) : full_scale;
        const std::string &family = st.font_family;
        const bool styled = !s.markup && (scale != 1.0f || !family.empty()) && !heading_row &&
                            !(s.style & (mepml::kCode | mepml::kResult | mepml::kMath | mepml::kComment));
        if (styled && scale > 1.0f && !table_rows.count(s.line)) {
            Buffer::MepmlRowScale &rs = buf.mepml_row_scale[s.line];
            rs.scale = std::max(rs.scale, scale);
            rs.text_hash = std::hash<std::string>{}(line);
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
                // What stands in for the markup: the sheet's `content` for
                // this part where it gives one, else what the document
                // itself supplies (a citation's label, a footnote's number).
                std::string replace = s.replace;
                if (st.has_content) {
                    const std::string text = mepml::style::ExpandContent(st.content, std::to_string(s.number),
                                                                         (s.style & mepml::kCallout) ? s.callout : s.target, "");
                    if (s.style & mepml::kListMarker) {
                        // (The item's indent, the marker, a space.)
                        const size_t indent = s.replace.find_first_not_of(' ');
                        replace = std::string(indent == std::string::npos ? 0 : indent, ' ') + text + " ";
                    } else if (s.style & mepml::kBox) {
                        if (paths.nodes[static_cast<size_t>(node)].element.part == "label") {
                            // The opener keeps what joins it to the title or the
                            // text (": " / ". "); the full stop after a title is
                            // the label's too, and stays as it is.
                            const std::string own = mepml::BoxLabel(s.target);
                            replace = s.replace.compare(0, own.size(), own) == 0 ? text + s.replace.substr(own.size()) : s.replace;
                        } else {
                            replace = (s.block_end ? "" : " ") + text;
                        }
                    } else if (s.style & mepml::kAbstract) {
                        const bool run_in = s.replace.size() >= 2 && s.replace.compare(s.replace.size() - 2, 2, ". ") == 0;
                        replace = text + (run_in ? ". " : "");
                    } else if (!(s.style & mepml::kRule)) {
                        replace = text;
                    }
                }
                if ((s.style & mepml::kRule) && s.replace.empty()) {
                    // Across the text width (never wider than the pane), as
                    // a rule on paper runs across the page.
                    const int pane_cols = TextColsForBuffer(CurrentBufferId());
                    const int rule_width = std::max(s.col_end - s.col_start, (pane_cols > 0 ? std::min(TextWidth(), pane_cols - 1) : TextWidth()) - s.col_start);
                    d.virt_text = Repeat(st.has_content && !st.content.empty() ? st.content.c_str() : "─", rule_width);
                } else if (s.style & mepml::kTableRule) {
                    // `|` -> `│`, and the |---| separator row drawn as a rule
                    // -- same width, so the columns never move.
                    std::string raw = line.substr(static_cast<size_t>(s.col_start),
                                                  static_cast<size_t>(s.col_end - s.col_start));
                    for (char c : raw) d.virt_text += c == '|' ? "│" : (c == '-' || c == ':' || c == '=') ? "─" : std::string(1, c);
                } else {
                    d.virt_text = replace;
                    d.conceal = replace.empty();
                }
                d.virt_text_hl = hl.empty() ? "Comment" : hl;
                d.bold = st.bold && !replace.empty();
                d.italic = st.italic && !replace.empty();
                // A slide's opener and closer: rules across the text,
                // the opener's labelled "Slide N: title". A trailing
                // comment keeps its place, so then the rule stops short.
                if (s.style & mepml::kSlide) {
                    const bool alone = line.find_first_not_of(" \t", static_cast<size_t>(s.col_end)) == std::string::npos;
                    // (Never wider than the pane, where it would wrap.)
                    const int pane_cols = TextColsForBuffer(CurrentBufferId());
                    const int width = (pane_cols > 0 ? std::min(TextWidth(), pane_cols - 1) : TextWidth()) - s.col_start;
                    std::string label;
                    if (!replace.empty()) {
                        label = "── " + replace;
                        auto t = slide_titles.find(s.number);
                        if (t != slide_titles.end() && !t->second.empty()) label += ": " + t->second;
                        label += " ";
                    }
                    d.virt_text = label + (alone ? Repeat("─", std::max(3, width - Codepoints(label))) : std::string());
                    d.conceal = false;
                    add(d);
                    continue;
                }
                // An abstract's label: centred (text-align) when it has its
                // line to itself, the way a paper sets it.
                if ((s.style & mepml::kAbstract) && !replace.empty() && st.text_align == mepml::style::TextAlign::Center &&
                    replace.find(". ") == std::string::npos &&
                    line.find_first_not_of(" \t", static_cast<size_t>(s.col_end)) == std::string::npos)
                    d.virt_text = std::string(static_cast<size_t>(std::max(0, (TextWidth() - Codepoints(replace)) / 2)), ' ') + replace;
                // A box's end mark (a proof's tombstone), where its `)` was
                // on a line of its own: at the right edge.
                if ((s.style & mepml::kBox) && s.block_end && !replace.empty()) {
                    const int pane_cols = TextColsForBuffer(CurrentBufferId());
                    const int width = (pane_cols > 0 ? std::min(TextWidth(), pane_cols - 1) : TextWidth()) - s.col_start;
                    d.virt_text = std::string(static_cast<size_t>(std::max(0, width - 1 - Codepoints(replace))), ' ') + replace;
                }
                add(d);
            } else {
                Decoration d = base;
                d.hl_group = hl.empty() ? "Comment" : hl;
                d.bold = st.bold;
                d.italic = st.italic;
                add(d);
            }
            continue;
        }

        // Scaled / other-face text: the run is replaced by itself drawn at
        // its real size and face (Decoration::virt_scale/virt_family),
        // carrying every style it has, so nothing else is drawn over it.
        const mepml::style::Color &bg = has_node ? styles.inline_bg[static_cast<size_t>(node)] : kNoStyle.background;
        const std::string deco_hl = st.has_decoration_color ? StyleHl(st.decoration_color) : std::string();
        if (styled && hide) {
            Decoration d = base;
            d.virt_overlay = true;
            d.virt_text = line.substr(static_cast<size_t>(s.col_start), static_cast<size_t>(s.col_end - s.col_start));
            d.virt_scale = scale;
            d.virt_family = family;
            d.virt_raise = st.vertical_align == mepml::style::VerticalAlign::Super ? 0.38f
                           : st.vertical_align == mepml::style::VerticalAlign::Sub ? -0.2f
                                                                                   : 0.0f;
            d.priority = 10;
            d.bold = st.bold;
            d.italic = st.italic;
            d.underline = st.underline;
            d.strikethrough = st.strike;
            if (literal_color) {
                d.has_fg_color = true;
                d.fg_color = ThemeColor{static_cast<unsigned char>((literal >> 16) & 0xff),
                                        static_cast<unsigned char>((literal >> 8) & 0xff),
                                        static_cast<unsigned char>(literal & 0xff), 255};
            }
            d.virt_text_hl = hl.empty() || literal_color ? "Normal" : hl;
            add(d);
            if (bg.kind != mepml::style::Color::None) {
                Decoration h = base;
                h.hl_group = StyleHl(bg);
                h.bg_fill = true;
                h.priority = -5;
                add(h);
            }
            continue;
        }

        // Content.
        if (literal_color) {
            Decoration d = base;
            d.col_start = ByteToColumn(line, s.col_start);
            d.col_end = ByteToColumn(line, s.col_end);
            d.has_fg_color = true;
            d.fg_color = ThemeColor{static_cast<unsigned char>((literal >> 16) & 0xff),
                                    static_cast<unsigned char>((literal >> 8) & 0xff),
                                    static_cast<unsigned char>(literal & 0xff), 255};
            add(d);
            hl.clear();
        } else if (!hl.empty()) {
            Decoration d = base;
            d.hl_group = hl;
            add(d);
        }
        if (bg.kind != mepml::style::Color::None) {
            Decoration d = base;
            d.hl_group = StyleHl(bg);
            d.bg_fill = true;
            d.priority = -5;  // under every other style on the span
            add(d);
        }
        const std::string style_hl = hl.empty() ? "Normal" : hl;
        if (st.bold) {
            Decoration d = base;
            d.bold = true;
            d.hl_group = style_hl;
            add(d);
        }
        if (st.italic) {
            Decoration d = base;
            d.italic = true;
            d.hl_group = style_hl;
            add(d);
        }
        if (st.underline) {
            Decoration d = base;
            d.underline = true;
            d.hl_group = style_hl;
            add(d);
        }
        if (st.strike) {
            Decoration d = base;
            d.strikethrough = true;
            d.hl_group = deco_hl.empty() ? style_hl : deco_hl;
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
                for (int r = b.line_start; r <= b.line_end; ++r) {
                    buf.mepml_table_images.erase(r);
                    buf.org_table_wrap_rows.erase(r);
                }
        MepmlTableLayout(doc, spans, table_layout_rows, table_edge_markup, ns, &patch_tables);
        MepmlFitCards();
    }

    // Callouts get a coloured bar in the sign column on every line.
    const std::vector<int> &block_nodes = MepmlBlockNodesCurrent(true);
    for (size_t bi = 0; bi < doc.blocks.size(); ++bi) {
        const mepml::Block &b = doc.blocks[bi];
        if (!b.origin.empty() || b.kind != mepml::BlockKind::Callout) continue;
        // The bar is the callout's own colour.
        const int node = bi < block_nodes.size() ? block_nodes[bi] : -1;
        std::string bar = "Blue";
        if (node >= 0 && static_cast<size_t>(node) < styles.rendered.size() && styles.rendered[static_cast<size_t>(node)].has_color)
            bar = StyleHl(styles.rendered[static_cast<size_t>(node)].color);
        for (int row = b.line_start; row <= b.line_end; ++row) {
            if (!in_scope(row)) continue;
            Decoration d;
            d.row = row;
            d.whole_line = true;
            d.sign_shape = "bar";
            d.sign_hl = bar;
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
        // Each line's look is its `meta` element's style: the title's and
        // subtitle's size, weight and colour, the `::key` label, an
        // option's `::name` and `::value`, any other line's `::value`.
        auto meta_node = [&](const mepml::Block *e) {
            const size_t bi = static_cast<size_t>(e - doc.blocks.data());
            return bi < block_nodes.size() ? block_nodes[bi] : -1;
        };
        auto meta_style = [&](const mepml::Block *e) -> const mepml::style::Computed & {
            const int node = meta_node(e);
            return node >= 0 && static_cast<size_t>(node) < styles.rendered.size() ? styles.rendered[static_cast<size_t>(node)] : kNoStyle;
        };
        auto hl_of = [](const mepml::style::Computed &c, const char *fallback) {
            return c.has_color ? StyleHl(c.color) : std::string(fallback);
        };
        float title_scale = 1.0f;
        for (const mepml::Block *e : run.entries) {
            const std::string k = Lowered(e->keyword);
            if (k == "title") title_scale = std::clamp(meta_style(e).font_size, 0.5f, 3.0f);
            if (k == "title") title = e->value;
            else if (k == "option") ++options;
            else others.push_back(k == "subtitle" || k == "author" || k == "date" || k == "import" ? e->value : k);
        }
        // Folded, the header reads as its title and a muted tally.
        if (run.last > run.first) {
            Buffer::MepmlFoldSummary sum;
            sum.title = title.empty() ? run.entries[0]->keyword + ": " + run.entries[0]->value : title;
            sum.title_scale = title.empty() ? 1.0f : title_scale;
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
                const mepml::style::Computed &ms = meta_style(e);
                const float scale = std::clamp(ms.font_size, 0.5f, 3.0f);
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
                v.bold = ms.bold;
                v.italic = ms.italic;
                v.virt_text_hl = hl_of(ms, "Normal");
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
            const mepml::style::Computed &key_style = MepmlPartStyle(meta_node(e), "key");
            label.virt_text_hl = hl_of(key_style, "Comment");
            label.bold = key_style.bold;
            label.italic = key_style.italic;
            label.priority = 10;
            add(label);
            if (value.empty()) continue;
            auto piece = [&](int from, int to, const std::string &text, const mepml::style::Computed &ps, const char *fallback) {
                Decoration d;
                d.row = row;
                d.col_start = from;
                d.col_end = to;
                d.virt_overlay = true;
                d.virt_text = text;
                d.virt_text_hl = hl_of(ps, fallback);
                d.bold = ps.bold;
                d.italic = ps.italic;
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
                const int at = mc.value_start;
                piece(at, at + static_cast<int>(eq), name, MepmlPartStyle(meta_node(e), "name"), "Normal");
                piece(at + static_cast<int>(eq), at + static_cast<int>(vs), " = ", MepmlPartStyle(meta_node(e), "markup"), "Comment");
                if (!raw.empty()) piece(at + static_cast<int>(vs), len, raw, MepmlPartStyle(meta_node(e), "value"), "Normal");
            } else {
                piece(mc.value_start, len, value, MepmlPartStyle(meta_node(e), "value"), "Normal");
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

    // What each row conceals (Buffer::mepml_row_conceal), as DrawPane will
    // collapse it: the overlays that stand a span down to its replacement.
    {
        if (!patch) buf.mepml_row_conceal.clear();
        for (int r : patch_rows) buf.mepml_row_conceal.erase(r);
        std::unordered_set<int> unmeasured;
        const auto found = buf.decorations.find(ns);
        if (found != buf.decorations.end()) {
            for (const Decoration &d : found->second) {
                if (!in_scope(d.row)) continue;
                if (d.whole_line || !d.virt_overlay || (d.virt_text.empty() && !d.conceal) || d.virt_text_eol) continue;
                if (d.col_end <= d.col_start) continue;
                if (!d.virt_family.empty()) {
                    unmeasured.insert(d.row);
                    continue;
                }
                int cp = 0;
                for (char c : d.virt_text) cp += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
                if (d.virt_scale != 1.0f) cp = std::max(1, static_cast<int>(std::ceil(static_cast<float>(cp) * d.virt_scale - 1e-3f)));
                buf.mepml_row_conceal[d.row].push_back({d.col_start, d.col_end, cp});
            }
        }
        for (int r : unmeasured) buf.mepml_row_conceal.erase(r);
        for (auto &kv : buf.mepml_row_conceal)
            if (in_scope(kv.first)) std::sort(kv.second.begin(), kv.second.end());
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
    state.view = buf.mepml_view;
    state.table_math_gen = buf.mepml_table_math_gen;
    state.sheet_signature = sheet_signature;
    state.deco_count = buf.decorations.count(ns) ? buf.decorations[ns].size() : 0;
}


void Editor::RecomputeMepmlTabFolds() {
    Buffer &buf = Buf();
    std::vector<Fold> old_folds;
    for (const Fold &f : buf.folds)
        if (f.provider == "mepml-tabs") old_folds.push_back(f);
    ClearFoldsFromProvider("mepml-tabs");
    buf.mepml_tab_rows.clear();
    buf.mepml_tabs_sig = 0;
    if (!IsMepmlBuffer()) return;
    const std::vector<MepmlTabSet> sets = MepmlTabSets(MepmlParseCurrent(false));
    buf.mepml_tabs_sig = MepmlTabsSig(sets);
    const int cursor_row = CurPane().buffer_id == CurrentBufferId() ? CurPane().cursor.row : -1;
    // Closed, unless the user has opened it (zo) since it was built.
    auto add = [&](int start, int end) {
        bool closed = buf.fold_enabled;
        for (const Fold &of : old_folds)
            if (of.start_row == start && of.end_row == end) closed = of.closed;
        buf.folds.push_back({start, end, closed, "mepml-tabs"});
    };
    for (size_t si = 0; si < sets.size(); ++si) {
        const MepmlTabSet &set = sets[si];
        const int n = static_cast<int>(set.tabs.size());
        int &active = buf.mepml_tab_active[static_cast<int>(si)];
        // The cursor in one of the tabs (one just written, say) shows that
        // one: what is being edited is never folded away under it.
        for (int k = 0; k < n; ++k)
            if (cursor_row >= set.tabs[static_cast<size_t>(k)].open_row && cursor_row <= set.tabs[static_cast<size_t>(k)].close_row)
                active = k;
        active = std::clamp(active, 0, n - 1);
        const MepmlTabSet::Tab &shown = set.tabs[static_cast<size_t>(active)];
        // The set's opening line down to the shown tab's own, read as the
        // strip; its closing line down to the set's, as the rule under it.
        add(set.open_row, shown.open_row);
        add(shown.close_row, set.close_row);
        Buffer::MepmlTabRow row;
        for (const MepmlTabSet::Tab &t : set.tabs) row.titles.push_back(t.title);
        row.active = active;
        row.set = static_cast<int>(si);
        buf.mepml_tab_rows[set.open_row] = row;
        row.footer = true;
        buf.mepml_tab_rows[shown.close_row] = std::move(row);
    }
    NormalizeFoldList(buf.folds, buf.LineCount());
}

bool Editor::MepmlSelectTab(int set, int tab) {
    if (!IsMepmlBuffer()) return false;
    const std::vector<MepmlTabSet> sets = MepmlTabSets(MepmlParseCurrent(false));
    if (set < 0 || set >= static_cast<int>(sets.size())) return false;
    const MepmlTabSet &s = sets[static_cast<size_t>(set)];
    const int n = static_cast<int>(s.tabs.size());
    Buf().mepml_tab_active[set] = ((tab % n) + n) % n;
    // The cursor in the set goes to its strip: not left in a tab now
    // folded away, nor pulling the set back to that tab.
    if (CurPane().buffer_id == CurrentBufferId() && CurPane().cursor.row >= s.open_row && CurPane().cursor.row <= s.close_row)
        SetCursorForLua(s.open_row, 0);
    RecomputeMepmlTabFolds();
    return true;
}

std::string Editor::MepmlCycleTab(int delta) {
    if (!IsMepmlBuffer()) return "";
    const std::vector<MepmlTabSet> sets = MepmlTabSets(MepmlParseCurrent(false));
    const int row = CurPane().cursor.row;
    // The innermost set around the cursor, else the first below it.
    int pick = -1;
    for (int k = 0; k < static_cast<int>(sets.size()); ++k)
        if (row >= sets[static_cast<size_t>(k)].open_row && row <= sets[static_cast<size_t>(k)].close_row) pick = k;
    for (int k = 0; pick < 0 && k < static_cast<int>(sets.size()); ++k)
        if (sets[static_cast<size_t>(k)].open_row > row) pick = k;
    if (pick < 0) return "";
    // (Which tab it shows now: the folds' own record, the cursor's tab
    // having been made the shown one when they were built.)
    RecomputeMepmlTabFolds();
    const int now = Buf().mepml_tab_active[pick];
    if (!MepmlSelectTab(pick, now + delta)) return "";
    const MepmlTabSet &s = sets[static_cast<size_t>(pick)];
    return s.tabs[static_cast<size_t>(Buf().mepml_tab_active[pick])].title;
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
    const std::vector<MepmlTabSet> tab_sets = MepmlTabSets(doc);
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
        // ... nor past the end of the tab it is in (a fold crossing the
        // tabs' own would be merged with them).
        for (const MepmlTabSet &set : tab_sets)
            for (const MepmlTabSet::Tab &t : set.tabs)
                if (heads[k]->line_start > t.open_row && heads[k]->line_start < t.close_row) end = std::min(end, t.close_row - 1);
        while (end > heads[k]->line_start && Buf().lines[static_cast<size_t>(end)].find_first_not_of(" \t") == std::string::npos)
            --end;
        add(heads[k]->line_start, end);
    }
    for (const HeaderRun &run : HeaderRuns(doc)) add(run.first, run.last);
    for (const mepml::Slide &sl : slides) add(sl.line_start, sl.line_end);
    // Boxes (\definition ...) and columns: from the opening line to the
    // closing `)`.
    {
        std::vector<const mepml::Block *> open;
        for (const mepml::Block &b : doc.blocks) {
            if (!b.origin.empty()) continue;
            if (b.kind == mepml::BlockKind::BoxBegin && b.box_closed) add(b.line_start, b.line_end);
            else if (b.kind == mepml::BlockKind::BoxBegin || b.kind == mepml::BlockKind::LayoutBegin) open.push_back(&b);
            else if ((b.kind == mepml::BlockKind::BoxEnd || b.kind == mepml::BlockKind::LayoutEnd) && !open.empty()) {
                // (Tabs fold their own way: RecomputeMepmlTabFolds.)
                if (open.back()->kind != mepml::BlockKind::LayoutBegin || (open.back()->keyword != "tabs" && open.back()->keyword != "tab"))
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
            b.kind == mepml::BlockKind::Abstract || b.kind == mepml::BlockKind::Svg || b.kind == mepml::BlockKind::Html)
            add(b.line_start, b.line_end);
    }
    RecomputeMepmlTabFolds();
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
        // results=html (or svg, or output that is an SVG image): the output
        // is HTML, kept as such and rendered.
        std::vector<std::string> lines = mepml::FormatResults(output, mepml::ResultFormatFor(b, output));
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
    if (MepmlStylesStale()) return true;
    auto it = mepml_scan_state_.find(CurrentBufferId());
    if (it == mepml_scan_state_.end() || !it->second.valid) return false;
    if (it->second.table_math_gen != Buf().mepml_table_math_gen) return true;
    // The pane has other columns than the tables were laid out for: one
    // wrapped to the old width, or one that no longer fits the new.
    const int cols = CurPane().text_cols;
    if (cols == it->second.pane_cols || !org_table_wrap_visible_) return false;
    for (const auto &kv : Buf().org_table_wrap_rows)
        if (kv.second.hashed) return true;
    auto grids = mepml_table_grids_.find(CurrentBufferId());
    if (grids != mepml_table_grids_.end())
        for (const OrgTableGrid &g : grids->second)
            if (g.indent + g.width > cols) return true;
    return false;
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
    const MepmlNodeStyles &node_styles = MepmlStylesCurrent(true);
    static const mepml::style::Computed kPlain;
    auto span_width = [&](const mepml::Span &sp, const std::string &line) {
        const bool known = sp.path >= 0 && static_cast<size_t>(sp.path) < node_styles.rendered.size();
        return ConcealedWidth(sp, line, kTableMaxScale, known ? node_styles.rendered[static_cast<size_t>(sp.path)] : kPlain);
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
        const Buffer::OrgLatexInlineSpan *span;
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
            if (cols >= 0) out.push_back({sp.col_start, sp.col_end, cols, &sp});
        }
        return out;
    };
    // What the scan drew in place of each concealed markup on a table row
    // (its overlays' text, by (row, col_start)), for a table laid out
    // wrapped below -- gathered once, and only when there is one.
    std::map<std::pair<int, int>, std::string> shown_markup;
    bool shown_markup_built = false;
    auto shown_for = [&](const mepml::Span &sp) -> const std::string * {
        if (!shown_markup_built) {
            shown_markup_built = true;
            auto ds = buf.decorations.find(ns);
            if (ds != buf.decorations.end())
                for (const Decoration &d : ds->second)
                    if (d.virt_overlay && d.col_end > d.col_start && table_rows.count(d.row))
                        shown_markup[{d.row, d.col_start}] = d.virt_text;
        }
        auto it = shown_markup.find({sp.line, sp.col_start});
        return it == shown_markup.end() ? nullptr : &it->second;
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
        std::vector<int> unbreakable;  // each column's widest formula: no narrower than that
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
                    for (const MathRun &m : maths) {
                        if (m.col_start < c.cs || m.col_end > c.ce) continue;
                        c.width += m.cols;
                        if (unbreakable.size() <= k) unbreakable.resize(k + 1, 0);
                        unbreakable[k] = std::max(unbreakable[k], m.cols);
                    }
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
        // A table wider than the pane has columns: left to soft-wrap, a row
        // broke wherever the pane ended and went on from the left edge,
        // through the first column. Its columns are re-budgeted to fit
        // instead (the widest give way first, PlanOrgTableWrap's split) and
        // every cell wraps inside its own -- on the cursor's row too, which
        // shows its source in the same columns.
        const int table_indent = [&] {
            const RowShape &s0 = shapes[b.line_start];
            return s0.cells.empty() ? 0 : Codepoints(buf.lines[static_cast<size_t>(b.line_start)].substr(0, static_cast<size_t>(s0.first)));
        }();
        bool wrapped = false;
        if (org_table_wrap_visible_ && wrap_ && !buf.no_wrap) {
            // (A column short of the pane's, so the outline's right edge is in view.)
            int budget = CurPane().text_cols - 1;
            // (In a column set beside others, that column's.)
            for (const Buffer::MepmlColumnsBlock &blk : buf.mepml_columns) {
                if (b.line_start <= blk.start_row || b.line_start >= blk.end_row) continue;
                std::vector<int> percents;
                for (const Buffer::MepmlColumnsBlock::Column &c : blk.columns) percents.push_back(c.percent);
                const std::vector<int> cols = mepml::ColumnCols(percents, std::max(1, CurPane().text_cols - 1));
                for (size_t k = 0; k < blk.columns.size() && k < cols.size(); ++k)
                    if (b.line_start >= blk.columns[k].first_row && b.line_start <= blk.columns[k].last_row)
                        budget = std::min(budget, cols[k]);
            }
            int total = table_indent + 1;
            for (int w : widths) total += w + 3;
            if (budget >= 8 && total > budget) {
                // The columns' widths in the budget, those with a `floor`
                // held at it and the rest sharing what they leave.
                auto plan_cols = [&](const std::vector<int> &floor) {
                    std::vector<int> out = widths;
                    OrgTableCells natural;
                    std::vector<size_t> free_cols;
                    int rest = budget;
                    for (size_t k = 0; k < widths.size(); ++k) {
                        if (floor[k] > 0) {
                            out[k] = floor[k];
                            rest -= floor[k] + 3;
                        } else {
                            natural.cells.emplace_back(static_cast<size_t>(std::max(0, widths[k])), 'x');
                            free_cols.push_back(k);
                        }
                    }
                    if (!free_cols.empty()) {
                        const OrgTableWrapPlan plan = PlanOrgTableWrap({natural}, rest, table_indent);
                        for (size_t i = 0; i < free_cols.size() && i < plan.col_widths.size(); ++i)
                            out[free_cols[i]] = std::min(widths[free_cols[i]], plan.col_widths[i]);
                    }
                    return out;
                };
                auto fits = [&](const std::vector<int> &cols) {
                    int sum = table_indent + 1;
                    for (int w : cols) sum += w + 3;
                    return sum <= budget;
                };
                std::vector<int> floor(widths.size(), 0);
                const std::vector<int> plain = plan_cols(floor);
                // A formula is not broken across lines: its column is no
                // narrower than it, while the others can make up for that
                // (where they cannot, it is drawn smaller, to its column).
                std::vector<int> planned = plain;
                for (size_t pass = 0; pass < widths.size(); ++pass) {
                    bool grew = false;
                    for (size_t k = 0; k < widths.size() && k < unbreakable.size(); ++k) {
                        if (floor[k] > 0 || unbreakable[k] <= planned[k]) continue;
                        floor[k] = std::min(unbreakable[k], widths[k]);
                        grew = true;
                    }
                    if (!grew) break;
                    planned = plan_cols(floor);
                }
                if (!fits(planned)) planned = plain;
                if (fits(planned) && planned != widths) {
                    wrapped = true;
                    widths = planned;
                }
            }
        }
        // (A row of the wrapped table that is drawn from that layout.)
        auto wraps = [&](int row) {
            if (!wrapped || row == b.separator_line) return false;
            auto it = shapes.find(row);
            return it != shapes.end() && !it->second.cells.empty();
        };
        // The grid DrawPane's org table pass draws (outline, header wash,
        // zebra stripes, rules), in the display columns the layout below
        // puts every pipe at.
        {
            OrgTableGrid g;
            g.start_row = b.line_start;
            g.end_row = std::min(body_end, n - 1);
            g.indent = table_indent;
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
                if (!rows.count(row) && !wraps(row)) g.raw_rows.push_back(row);
            // Its colours are the table element's: `background` the hue of
            // its header and stripes, `border-color` (and `:active`'s) its
            // outline, `table::rule`'s `color` its grid lines.
            {
                const size_t bi = static_cast<size_t>(&b - doc.blocks.data());
                const std::vector<int> &nodes = MepmlBlockNodesCurrent(true);
                const int node = bi < nodes.size() ? nodes[bi] : -1;
                const MepmlNodeStyles &all = MepmlStylesCurrent(true);
                if (node >= 0 && static_cast<size_t>(node) < all.rendered.size()) {
                    const mepml::style::Computed &ts = all.rendered[static_cast<size_t>(node)];
                    g.look.wash = CardColorOf(ts.background);
                    g.look.border = CardColorOf(ts.border_color);
                    g.look.border_active = CardColorOf(MepmlStateStyle(node, ":active").border_color);
                    const mepml::style::Computed &rs = MepmlPartStyle(node, "rule");
                    if (rs.has_color) g.rule = CardColorOf(rs.color);
                }
            }
            mepml_table_grids_[CurrentBufferId()].push_back(g);
            // Soft-wrap measures the laid-out rows by the grid they draw
            // across (Editor::WrapLenForRow); the cursor's raw row by its text.
            for (int row = g.start_row; row <= g.end_row; ++row) {
                if (rows.count(row) && !wraps(row)) buf.mepml_table_row_cols[row] = g.indent + g.width;
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

        // One row of a table laid out wrapped: each cell's text as it is
        // drawn (`concealed`: markup stood down to what the scan shows in
        // its place, a formula to its render; otherwise the source as
        // typed), wrapped to its column, the cells side by side. The row's
        // decorations are not redone for it: OrgTableWrapRun says which
        // bytes of the row each drawn run is, and DrawPane colours the
        // layout from them.
        auto wrap_row = [&](int row, bool concealed) {
            const std::string &line = buf.lines[static_cast<size_t>(row)];
            const RowShape &shape = shapes[row];
            const std::vector<Cell> &cs = cells[row];
            const size_t ncols = widths.size();
            // Stands in a cell's text for one column of a formula's render
            // while it is wrapped (not a space: the columns stay together),
            // and is drawn blank.
            constexpr char kMathBlank = '\x01';
            struct Piece {
                int sa, sb;  // bytes of the row
                int da, db;  // bytes of the cell's drawn text
                bool verbatim;
                const Buffer::OrgLatexInlineSpan *math;
            };
            // A word as the wrap placed it: from byte `d` of the drawn
            // text, at byte `out` of its line.
            struct Chunk {
                int d, len, line, out;
            };
            struct CellLayout {
                std::vector<Piece> pieces;
                std::vector<std::string> lines;
                std::vector<Chunk> chunks;
            };
            std::vector<CellLayout> lay(ncols);
            const std::vector<MathRun> maths = concealed ? math_runs(row) : std::vector<MathRun>();
            size_t height = 1;
            for (size_t k = 0; k < ncols && k < cs.size(); ++k) {
                const Cell &c = cs[k];
                CellLayout &cell = lay[k];
                std::string drawn;
                auto put = [&](int sa, int sb, const std::string &text, bool verbatim, const Buffer::OrgLatexInlineSpan *math) {
                    if (sb <= sa) return;
                    cell.pieces.push_back({sa, sb, static_cast<int>(drawn.size()), static_cast<int>(drawn.size() + text.size()), verbatim, math});
                    drawn += text;
                };
                // (A picture cell's `\image(...)` is hidden: its picture is drawn above the row.)
                if (!(concealed && !c.image.empty())) {
                    struct Item {
                        int a, b;
                        std::string text;
                        const Buffer::OrgLatexInlineSpan *math;
                    };
                    std::vector<Item> items;
                    if (concealed) {
                        // A formula: the columns its render is drawn over,
                        // kept together as one word.
                        for (const MathRun &m : maths) {
                            if (m.col_start < c.cs || m.col_end > c.ce) continue;
                            std::string blank;
                            blank.assign(static_cast<size_t>(std::clamp(m.cols, 0, std::max(1, widths[k]))), kMathBlank);
                            items.push_back({m.col_start, m.col_end, blank, m.span});
                        }
                        if (auto it = by_line.find(row); it != by_line.end()) {
                            for (const mepml::Span *sp : it->second) {
                                if (!sp->markup || sp->col_start < c.cs || sp->col_end > c.ce) continue;
                                bool in_math = false;
                                for (const MathRun &m : maths)
                                    if (sp->col_start >= m.col_start && sp->col_end <= m.col_end) in_math = true;
                                if (in_math) continue;
                                if (const std::string *shown = shown_for(*sp)) items.push_back({sp->col_start, sp->col_end, *shown, nullptr});
                                else if (edge_markup.count({row, sp->col_start})) items.push_back({sp->col_start, sp->col_end, sp->replace, nullptr});
                            }
                        }
                        std::stable_sort(items.begin(), items.end(), [](const Item &x, const Item &y) { return x.a < y.a; });
                    }
                    int pos = c.cs;
                    for (const Item &it : items) {
                        if (it.a < pos) continue;
                        put(pos, it.a, line.substr(static_cast<size_t>(pos), static_cast<size_t>(it.a - pos)), true, nullptr);
                        put(it.a, it.b, it.text, false, it.math);
                        pos = it.b;
                    }
                    put(pos, c.ce, line.substr(static_cast<size_t>(pos), static_cast<size_t>(std::max(0, c.ce - pos))), true, nullptr);
                }
                cell.lines = OrgTableWrapCell(drawn, widths[k]);
                // Where each word went: the wrap drops the spaces it breaks
                // on and keeps the words in order.
                size_t p = 0;
                for (size_t li = 0; li < cell.lines.size(); ++li) {
                    const std::string &t = cell.lines[li];
                    for (size_t q = 0; q < t.size();) {
                        if (t[q] == ' ') {
                            ++q;
                            continue;
                        }
                        size_t r = t.find(' ', q);
                        if (r == std::string::npos) r = t.size();
                        while (p < drawn.size() && (drawn[p] == ' ' || drawn[p] == '\t')) ++p;
                        cell.chunks.push_back({static_cast<int>(p), static_cast<int>(r - q), static_cast<int>(li), static_cast<int>(q)});
                        p += r - q;
                        q = r;
                    }
                }
                for (std::string &t : cell.lines) std::replace(t.begin(), t.end(), kMathBlank, ' ');
                height = std::max(height, cell.lines.size());
            }
            // A formula taller than a line is drawn at its size: the line
            // it is on gets empty lines above and below for it, the room
            // Editor::InlineMathPadFor makes around an unwrapped row.
            std::vector<int> pad_top(height, 0), pad_bottom(height, 0);
            const double lh = render_line_height_;
            if (lh > 0.0) {
                for (const CellLayout &cell : lay) {
                    for (const Piece &pc : cell.pieces) {
                        if (!pc.math || pc.math->path.empty() || pc.math->height <= 0) continue;
                        // (Drawn to its columns: smaller, where they are fewer than it is wide.)
                        Buffer::OrgLatexInlineSpan as = *pc.math;
                        const double room = static_cast<double>(pc.db - pc.da) * render_char_width_;
                        if (as.width > 0 && room > 0.0 && room < static_cast<double>(as.width)) {
                            const double k = room / static_cast<double>(as.width);
                            as.height = static_cast<int>(std::ceil(static_cast<double>(as.height) * k));
                            if (as.baseline >= 0.0f) as.baseline = static_cast<float>(static_cast<double>(as.baseline) * k);
                        }
                        for (const Chunk &ch : cell.chunks) {
                            if (ch.d + ch.len <= pc.da || ch.d >= pc.db) continue;
                            const double top = static_cast<double>(InlineMathTopOffset(as));
                            const double slack = 0.25 * lh;
                            auto slots = [&](double over) { return over > slack ? static_cast<int>(std::ceil((over - slack) / lh)) : 0; };
                            const size_t l = static_cast<size_t>(ch.line);
                            pad_top[l] = std::max(pad_top[l], slots(-top));
                            pad_bottom[l] = std::max(pad_bottom[l], slots(top + static_cast<double>(as.height) - lh));
                            break;
                        }
                    }
                }
            }
            std::vector<int> at(height, 0);  // the drawn line each of the row's lines is
            int total = 0;
            for (size_t l = 0; l < height; ++l) {
                total += pad_top[l];
                at[l] = total;
                total += 1 + pad_bottom[l];
            }
            Buffer::OrgTableWrapRow entry;
            entry.lines.resize(static_cast<size_t>(total));
            entry.indent = table_indent;
            entry.width = 1;
            std::vector<int> rule_cols{table_indent};
            for (int w : widths) {
                entry.width += w + 3;
                rule_cols.push_back(rule_cols.back() + w + 3);
            }
            entry.hashed = true;
            entry.text_hash = std::hash<std::string>{}(line);
            // (Blank where the pipes are: the grid pass draws the rules.)
            std::vector<std::vector<int>> cell_at(ncols, std::vector<int>(height, 0));  // the byte each cell's text starts at
            for (size_t l = 0; l < height; ++l) {
                std::string text(static_cast<size_t>(table_indent), ' ');
                for (size_t k = 0; k < ncols; ++k) {
                    text += "  ";
                    static const std::string kEmpty;
                    const std::string &cell_text = l < lay[k].lines.size() ? lay[k].lines[l] : kEmpty;
                    const int pad = std::max(0, widths[k] - Codepoints(cell_text));
                    const mepml::Align al = align_of(k);
                    const int before = al == mepml::Align::Right ? pad : al == mepml::Align::Center ? pad / 2 : 0;
                    text.append(static_cast<size_t>(before), ' ');
                    cell_at[k][l] = static_cast<int>(text.size());
                    text += cell_text;
                    text.append(static_cast<size_t>(pad - before + 1), ' ');
                }
                entry.lines[static_cast<size_t>(at[l])].text = std::move(text);
            }
            std::vector<std::pair<size_t, size_t>> cell_runs(ncols, {0, 0});  // each cell's runs, [first, last)
            for (size_t k = 0; k < ncols; ++k) {
                cell_runs[k].first = entry.runs.size();
                for (const Piece &pc : lay[k].pieces) {
                    bool first_of_piece = true;
                    for (const Chunk &ch : lay[k].chunks) {
                        const int lo = std::max(pc.da, ch.d), hi = std::min(pc.db, ch.d + ch.len);
                        if (hi <= lo) continue;
                        OrgTableWrapRun r;
                        r.line = at[static_cast<size_t>(ch.line)];
                        r.col_start = cell_at[k][static_cast<size_t>(ch.line)] + ch.out + (lo - ch.d);
                        r.col_end = r.col_start + (hi - lo);
                        r.verbatim = pc.verbatim;
                        r.src_start = pc.verbatim ? pc.sa + (lo - pc.da) : pc.sa;
                        r.src_end = pc.verbatim ? pc.sa + (hi - pc.da) : pc.sb;
                        if (pc.math) {
                            r.math = pc.math->path;
                            r.math_height = pc.math->height;
                            r.math_baseline = pc.math->baseline;
                        }
                        // Words of one piece side by side are one run, the
                        // space between them with it.
                        if (!first_of_piece && !entry.runs.empty()) {
                            OrgTableWrapRun &prev = entry.runs.back();
                            if (prev.line == r.line && prev.col_end + 1 == r.col_start && (!pc.verbatim || prev.src_end + 1 == r.src_start)) {
                                prev.col_end = r.col_end;
                                prev.src_end = r.src_end;
                                continue;
                            }
                        }
                        first_of_piece = false;
                        entry.runs.push_back(std::move(r));
                    }
                }
                cell_runs[k].second = entry.runs.size();
            }
            // Where each byte of the row is drawn: in its run; a byte that
            // is not drawn (padding, concealed markup, a space the wrap
            // broke on) just past what is before it; a pipe on its rule.
            auto shown_at = [&](int ln, int byte) {
                const std::string &t = entry.lines[static_cast<size_t>(ln)].text;
                return OrgTableWrapPos{ln, Codepoints(t.substr(0, std::min(t.size(), static_cast<size_t>(std::max(0, byte)))))};
            };
            entry.caret.assign(line.size() + 1, OrgTableWrapPos{at[0], table_indent});
            for (int bb = 0; bb < shape.first && bb < static_cast<int>(line.size()); ++bb)
                entry.caret[static_cast<size_t>(bb)] = OrgTableWrapPos{at[0], std::min(table_indent, Codepoints(line.substr(0, static_cast<size_t>(bb))))};
            OrgTableWrapPos here{at[0], table_indent};
            for (size_t k = 0; k < cs.size() && k < ncols; ++k) {
                const Cell &c = cs[k];
                if (c.ws_start > 0 && line[static_cast<size_t>(c.ws_start - 1)] == '|')
                    entry.caret[static_cast<size_t>(c.ws_start - 1)] = OrgTableWrapPos{at[0], rule_cols[k]};
                here = OrgTableWrapPos{at[0], rule_cols[k] + 2};
                size_t ri = cell_runs[k].first;
                const size_t rend = cell_runs[k].second;
                if (ri < rend) here = shown_at(entry.runs[ri].line, entry.runs[ri].col_start);
                auto pass = [&](int upto) {
                    while (ri < rend && entry.runs[ri].src_end <= upto) {
                        here = shown_at(entry.runs[ri].line, entry.runs[ri].col_end);
                        ++ri;
                    }
                };
                for (int bb = c.ws_start; bb < c.ws_end && bb < static_cast<int>(line.size()); ++bb) {
                    pass(bb);
                    if (ri < rend && entry.runs[ri].src_start <= bb) {
                        const OrgTableWrapRun &r = entry.runs[ri];
                        entry.caret[static_cast<size_t>(bb)] = shown_at(r.line, r.verbatim ? r.col_start + (bb - r.src_start) : r.col_start);
                    } else {
                        entry.caret[static_cast<size_t>(bb)] = here;
                    }
                }
                pass(static_cast<int>(line.size()) + 1);
            }
            // The closing pipe, and past the end of the row.
            const int last = shape.cells.back().second;
            if (shape.trail) {
                entry.caret[static_cast<size_t>(last)] = OrgTableWrapPos{at[0], rule_cols.back()};
                for (size_t bb = static_cast<size_t>(last) + 1; bb <= line.size(); ++bb)
                    entry.caret[bb] = OrgTableWrapPos{at[0], rule_cols.back() + 1};
            } else {
                for (size_t bb = static_cast<size_t>(last); bb <= line.size(); ++bb) entry.caret[bb] = here;
            }
            buf.org_table_wrap_rows[row] = std::move(entry);
        };

        for (int row = b.line_start; row <= body_end && row < n; ++row) {
            if (wraps(row)) {
                wrap_row(row, rows.count(row) > 0);
                continue;
            }
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
    // Each card's colours are its element's computed style (the sheets'
    // `background`, `border-color`, `border-left-color`; its `::header`
    // and `::label` parts for the title band and the kind chip; `:active`
    // for the outline while the cursor is in it).
    const MepmlNodeStyles &styles = MepmlStylesCurrent(true);
    const mepml::ElementPaths &paths = MepmlCacheEntry(true).paths;
    const std::vector<int> &block_nodes = MepmlBlockNodesCurrent(true);
    mepml::style::Cascade cascade;
    cascade.sheets = MepmlSheets();
    cascade.media = present_.active && CurrentBufferId() == present_.view_buffer
                        ? std::vector<std::string>{"present", "slides", "screen"}
                        : std::vector<std::string>{"editor", "screen"};
    auto card_color = [](const mepml::style::Color &c) {
        OrgCardColor out;
        out.set = true;
        out.hl = StyleHl(c);
        out.alpha = c.alpha;
        return out;
    };
    // (One per distinct element: a document's code blocks share a handful.)
    std::map<std::string, OrgCardLook> looks;
    // A presented slide's output fences (the view's first row is the
    // cursor's blank): what is in them is drawn with no card round it.
    std::set<int> plain_fences;
    if (present_.active && CurrentBufferId() == present_.view_buffer && !present_.pages.empty()) {
        const mepml::PresentationPage &page =
            present_.pages[static_cast<size_t>(std::clamp(present_.page, 0, static_cast<int>(present_.pages.size()) - 1))];
        for (int line : page.outputs) plain_fences.insert(line + 1);
    }
    // The look of `element` under the node `parent` of the tree (-1: the
    // document's root).
    auto look_of = [&](int parent, const mepml::Element &element) {
        std::string key = std::to_string(parent) + "|" + element.name;
        for (const auto &kv : element.attrs) key += "|" + kv.first + "=" + kv.second;
        auto it = looks.find(key);
        if (it != looks.end()) return it->second;
        std::vector<const mepml::Element *> path = paths.Path(parent);
        static const mepml::style::Computed kRoot;
        const mepml::style::Computed &from =
            parent >= 0 && static_cast<size_t>(parent) < styles.rendered.size() ? styles.rendered[static_cast<size_t>(parent)] : kRoot;
        path.push_back(&element);
        const mepml::style::Computed own = cascade.Compute(path, from);
        mepml::Element active = element;
        active.With(":active");
        path.back() = &active;
        const mepml::style::Computed on = cascade.Compute(path, from);
        path.back() = &element;
        const mepml::Element header = element.Part("header"), label = element.Part("label");
        path.push_back(&header);
        const mepml::style::Computed band = cascade.Compute(path, own);
        path.back() = &label;
        const mepml::style::Computed chip = cascade.Compute(path, own);
        OrgCardLook look;
        look.wash = card_color(own.background);
        look.border = card_color(own.border_color);
        look.border_active = card_color(on.border_color);
        look.stripe = card_color(own.border_left_color);
        look.band = card_color(band.background);
        // The chip is the label's background, or its text colour as a block.
        look.chip = card_color(chip.background.kind != mepml::style::Color::None ? chip.background
                               : chip.has_color                                  ? chip.color
                                                                                 : mepml::style::Color());
        // (With a background of its own, the label's colour is its words'.)
        if (chip.background.kind != mepml::style::Color::None && chip.has_color) look.chip_text = card_color(chip.color);
        const mepml::Element title = element.Part("title"), option = element.Part("option");
        path.back() = &title;
        const mepml::style::Computed title_style = cascade.Compute(path, own);
        if (title_style.has_color) look.title = card_color(title_style.color);
        path.back() = &option;
        const mepml::style::Computed option_style = cascade.Compute(path, own);
        if (option_style.has_color) look.option = card_color(option_style.color);
        const mepml::Element button = element.Part("button");
        path.back() = &button;
        const mepml::style::Computed button_style = cascade.Compute(path, own);
        // (Only a colour of the part's own: one inherited from the block is its text's.)
        if (button_style.has_color && button_style.color != own.color) look.button = card_color(button_style.color);
        if (band.has_color && band.color != own.color) look.header_text = card_color(band.color);
        looks.emplace(std::move(key), look);
        return look;
    };
    // The look of the block `b` (its own node of the tree).
    auto look_of_block = [&](const mepml::Block &b) {
        const size_t bi = static_cast<size_t>(&b - doc.blocks.data());
        const int node = bi < block_nodes.size() ? block_nodes[bi] : -1;
        if (node < 0 || static_cast<size_t>(node) >= paths.nodes.size()) return OrgCardLook();
        return look_of(paths.nodes[static_cast<size_t>(node)].parent, paths.nodes[static_cast<size_t>(node)].element);
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
        // (Its chip is the slide's `::label`: "Slide %n" unless a sheet says.)
        card.chip = "Slide " + std::to_string(sl.number);
        if (sl.first_block < doc.blocks.size() && sl.first_block < block_nodes.size()) {
            const mepml::style::Computed &label = MepmlPartStyle(block_nodes[sl.first_block], "label");
            if (label.has_content) card.chip = mepml::style::ExpandContent(label.content, std::to_string(sl.number), "", sl.title);
        }
        card.title = sl.title;
        card.content_cols = widest(sl.line_start, sl.line_end);
        card.fold_row = sl.line_start;
        if (sl.first_block < doc.blocks.size()) card.look = look_of_block(doc.blocks[sl.first_block]);
        cards.push_back(std::move(card));
    }
    for (const HeaderRun &run : HeaderRuns(doc)) {
        OrgBlockCard head;
        head.meta_row = head.begin_row = run.first;
        head.end_row = run.last;
        head.kind = "header";
        head.bare = true;
        // (The header is the element its `//?` lines are in.)
        head.look = look_of(paths.nodes.empty() ? -1 : 0, mepml::Element("header"));
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
            OrgBlockCard card;
            card.meta_row = card.begin_row = first;
            card.end_row = last;
            card.kind = "box";
            card.bare = true;
            card.look = look_of_block(*begin);
            // Folded, it collapses to a bar naming it.
            card.chip = mepml::BoxLabel(begin->keyword);
            {
                const size_t bi = static_cast<size_t>(begin - doc.blocks.data());
                const int node = bi < block_nodes.size() ? block_nodes[bi] : -1;
                const mepml::style::Computed &label = MepmlPartStyle(node, "label");
                if (label.has_content) card.chip = mepml::style::ExpandContent(label.content, "", begin->keyword, "");
                // (A box with an end mark keeps the row of its `)` for it.)
                card.end_mark = MepmlPartStyle(node, "end").has_content && !MepmlPartStyle(node, "end").content.empty();
            }
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
        card.look = look_of_block(b);
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
        code.plain = plain_fences.count(code.begin_row) != 0;
        code.look = look_of_block(b);
        // (Read before the push below can move the cards.)
        const size_t code_bi = static_cast<size_t>(&b - doc.blocks.data());
        const int code_node = code_bi < block_nodes.size() ? block_nodes[code_bi] : -1;
        const int results_parent = code_node >= 0 && static_cast<size_t>(code_node) < paths.nodes.size()
                                       ? paths.nodes[static_cast<size_t>(code_node)].parent
                                       : -1;
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
            out.look = look_of(results_parent, mepml::Element("results", "format", b.result_format.empty() ? "text" : b.result_format));
            out.term_run = run_id;
            // (A presented slide shows a block's results as they are.)
            out.plain = present_.active && CurrentBufferId() == present_.view_buffer;
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

// The height of the PNG at `path`, 0 when there is no readable one: its
// IHDR, which the signature puts at a fixed offset -- the picture itself
// does not have to be decoded to compare how much of a window two of them
// hold.
int PngHeightOf(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    unsigned char head[24];
    if (!f.read(reinterpret_cast<char *>(head), sizeof head)) return 0;
    static const unsigned char kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (std::memcmp(head, kSig, sizeof kSig) != 0 || std::memcmp(head + 12, "IHDR", 4) != 0) return 0;
    const unsigned long h = (static_cast<unsigned long>(head[20]) << 24) | (static_cast<unsigned long>(head[21]) << 16) |
                            (static_cast<unsigned long>(head[22]) << 8) | static_cast<unsigned long>(head[23]);
    return h > 0x7fffffffUL ? 0 : static_cast<int>(h);
}

// Whether the picture already at `path` shows more of the window than
// `snap` does, so saving `snap` over it would lose the better one. Only a
// clipped snapshot -- a piece of a window the pane's edge cut off -- can
// lose this way; a whole one is always what the document should carry.
bool KeepPictureOnDisk(const mep::gui_embed::Snapshot &snap, const std::string &path) {
    return snap.clipped && PngHeightOf(path) > snap.height;
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
            // Only a piece of the window ever showed (it sits past the
            // pane's edge): a picture already on disk that holds more of
            // it beats the sliver this would save.
            if (KeepPictureOnDisk(snap, run.snapshot_path)) {
                ref = run.snapshot_ref;
                path = run.snapshot_path;
                break;
            }
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
            // A web page (or, on macOS, any program) clicked into: it has the keyboard now.
            mepml_gui_focus_ = it->first;
            status_message_ = "The program has the keyboard: Ctrl-\\ or a click outside its window comes back";
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
            // Only a piece of the window ever showed: keep the picture
            // already on disk when it holds more of the window than this.
            if (KeepPictureOnDisk(snap, run.snapshot_path)) {
                lines.push_back("\\image(" + run.snapshot_ref + ")");
            } else {
                const std::string png = png::Encode(snap.width, snap.height, 4, snap.rgba.data(), snap.width * 4);
                std::ofstream f(run.snapshot_path, std::ios::binary);
                if (!png.empty() && f.write(png.data(), static_cast<std::streamsize>(png.size()))) {
                    f.close();
                    InvalidateOrgInlineImageTexture(run.snapshot_path);
                    lines.push_back("\\image(" + run.snapshot_ref + ")");
                }
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
    for (const mepml::StyleRef &ref : MepmlParseCurrent(true).styles)
        st.sheet_paths.push_back(mepml::ResolvePath(ref.base.empty() ? MepmlCurrentFile() : ref.base, ref.path));
    st.inline_sheets = mepml::InlineStyleSheets(MepmlParseCurrent(true));
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
    status_message_ = "Presenting: h/l (or C-n/C-p, arrows) change slide, +/- text size, j/k scroll, n for normal mode, C-c C-c runs code, f " +
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
            // The same slide again keeps its place (MepmlPresentScroll);
            // another starts at its top.
            if (present_.caret || !keep_cursor) SetPaneScrollTop(CurPane(), 0, 0);
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
        if (!on) SetPaneScrollTop(CurPane(), 0, 0);
    }
    mode_ = Mode::Normal;
    status_message_ = on ? "Normal mode on the slide: move and yank as usual, C-c C-c runs the block under the cursor, P to present"
                         : "";
}

bool Editor::MepmlPresentScroll(int slots) {
    if (!present_.active || present_.caret || ActivePaneId() != present_.pane_id ||
        CurPane().buffer_id != present_.view_buffer)
        return false;
    Pane &pane = CurPane();
    MepmlPresentPlaceScroll(pane, buffers_[static_cast<size_t>(present_.view_buffer)], pane.wrap_cols, slots);
    return true;
}

void Editor::MepmlPresentPlaceScroll(Pane &pane, const Buffer &buf, int wrap_cols, int move) {
    // Where each drawn row sits on the slide, in visual lines from its
    // top, counted the way the draw loop walks (a formula's further source
    // rows are drawn by its first one). Not simply one row after another:
    // a \column( row steps back up to its block's top (a negative
    // RowTopPadSlots), so rows side by side share the same lines.
    struct Placed {
        int row, top, slots;
    };
    std::vector<Placed> rows;
    int at = 0, total = 0, top = -1;
    for (int r = 0; r < buf.LineCount(); r = PaneNextDrawnRow(pane, buf, r)) {
        const int slots = PaneRowSlots(pane, buf, r, wrap_cols);
        if (r <= pane.scroll_row) top = at + (r == pane.scroll_row ? pane.scroll_sub : slots);
        if (slots > 0) rows.push_back({r, at, slots});
        at += slots;
        total = std::max(total, at);
    }
    // The slide's last line goes no higher than the pane's.
    const int visible = std::max(1, pane.visible_lines);
    const int want = std::clamp(top + move, 0, std::max(0, total - visible));
    // The first row over that line is the view's top one (in a set of
    // columns: the leftmost that reaches so far down).
    for (const Placed &p : rows) {
        if (want < p.top || want >= p.top + p.slots) continue;
        if (pane.scroll_row != p.row || pane.scroll_sub != want - p.top) SetPaneScrollTop(pane, p.row, want - p.top);
        break;
    }
    present_.scroll_top = want;
    present_.scroll_visible = visible;
    present_.scroll_total = total;
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
    present_.sheet_paths.clear();
    const mepml::Document written = mepml::ParseWithImports(file, src.lines, ReadFileLines);
    for (const mepml::StyleRef &ref : written.styles)
        present_.sheet_paths.push_back(mepml::ResolvePath(ref.base.empty() ? file : ref.base, ref.path));
    present_.inline_sheets = mepml::InlineStyleSheets(written);
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
    // click, a drag): it would reveal the source under it. (The view is
    // moved by MepmlPresentScroll alone.)
    if (here && !present_.caret && CurPane().buffer_id == present_.view_buffer &&
        (CurPane().cursor.row != kPresentParkRow || CurPane().cursor.col != 0)) {
        CurPane().cursor = {kPresentParkRow, 0};
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
    // through the slides, Ctrl-d / Ctrl-u move half a screen down / up a
    // slide taller than it, and C-c C-c runs every code block the slide shows.
    if (ctrl && !alt) {
        int ctrl_step = 0;
        bool ours = false;
        const int half = std::max(1, CurPane().visible_lines / 2);
        for (gfx::Key key = gfx::GetKeyPressed(); key != gfx::Key::None; key = gfx::GetKeyPressed()) {
            if (key == gfx::Key::N) ctrl_step = 1, ours = true;
            else if (key == gfx::Key::P) ctrl_step = -1, ours = true;
            else if (key == gfx::Key::D) MepmlPresentScroll(half), ours = true;
            else if (key == gfx::Key::U) MepmlPresentScroll(-half), ours = true;
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
            // j / k: down / up a slide taller than the screen (larger text).
            case 'j': MepmlPresentScroll(1); break;
            case 'k': MepmlPresentScroll(-1); break;
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
    // A web page, app or program on the slide is the slide's: it runs
    // (live, under the mouse) as soon as the slide is shown, rather than as
    // its last picture or output. It keeps running while other slides
    // show; the presentation's end stops it.
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

bool Editor::Accessibility(int buffer_id, a11y::Document *out, std::string *error) const {
    auto fail = [&](const std::string &why) {
        if (error) *error = why;
        return false;
    };
    if (buffer_id < 0 || buffer_id >= static_cast<int>(buffers_.size())) return fail("no such buffer");
    if (const PdfBufferState *pdf = PdfBufferFor(buffer_id)) {
        if (!pdf->doc) return fail("the PDF is not loaded");
        *out = pdf->doc->Accessibility();
        return true;
    }
    if (const HtmlSession *html = GetHtml(buffer_id)) {
        *out = a11y::FromHtmlDom(html->doc.root.get());
        return true;
    }
    const Buffer &buf = buffers_[static_cast<size_t>(buffer_id)];
    const std::string &file = buf.filename;
    const size_t dot = file.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : file.substr(dot + 1);
    for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == "mepml") {
        *out = mepml::Accessibility(mepml::ParseWithImports(file, buf.lines, ReadFileLines));
        return true;
    }
    if (file.empty() || GetTerminal(buffer_id)) return fail("nothing here to read: open a document (mepml, PDF, HTML, docx, odt, Markdown, Org)");
    return a11y::FromFile(file, out, error);
}
