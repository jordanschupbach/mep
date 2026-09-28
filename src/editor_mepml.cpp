// Editor-side half of mepml (src/mepml_doc.h holds the pure half): turning
// a parsed document into Decorations, the heading/image/link registries
// DrawPane reads, folds, and splicing code-block results back into a
// buffer. Everything here is a thin layer over mepml::Parse/Highlight --
// the language's own rules live, and are tested, in mepml_doc.cpp.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "editor.h"
#include "gfx/input.h"
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
    if (!org_conceal_visible_) return 0;  // scaled runs are only drawn while concealing
    if (row < 0 || row >= buf.LineCount()) return 0;
    const size_t hash = std::hash<std::string>{}(buf.lines[static_cast<size_t>(row)]);
    float scale = 1.0f;
    bool folded = false;
    for (const Fold &f : buf.folds)
        if (f.closed && f.start_row == row) folded = true;
    if (folded) {
        // A closed fold draws its summary line, not the row: only a folded
        // mepml header's summary has a large title (its own scale).
        auto sit = buf.mepml_fold_summaries.find(row);
        if (sit == buf.mepml_fold_summaries.end() || sit->second.text_hash != hash) return 0;
        scale = sit->second.title_scale;
    } else {
        if (const int pics = MepmlTableImageBoxes(buf, row, nullptr)) return pics;
        auto it = buf.mepml_row_scale.find(row);
        if (it == buf.mepml_row_scale.end() || hash != it->second.text_hash) return 0;
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
    if (over <= kSlack) return 0;
    return static_cast<int>(std::ceil((over - kSlack) / lh));
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

mepml::Document Editor::MepmlParseCurrent(bool with_imports) const {
    const Buffer &buf = Buf();
    if (!with_imports) return mepml::Parse(buf.lines);
    std::string file = buf.filename;
    std::error_code ec;
    if (!file.empty() && file[0] != '/') file = std::filesystem::absolute(file, ec).string();
    return mepml::ParseWithImports(file, buf.lines, ReadFileLines);
}

void Editor::MepmlScan(int ns, bool own_diagnostics) {
    Buffer &buf = Buf();
    buf.mepml_heading_rows.clear();
    buf.mepml_row_scale.clear();
    buf.mepml_table_images.clear();
    buf.mepml_virtual_rows.clear();
    buf.mepml_fold_summaries.clear();
    buf.mepml_html_rows.clear();
    mepml_table_grids_.erase(CurrentBufferId());
    mepml_block_cards_.erase(CurrentBufferId());
    if (!IsMepmlBuffer()) return;
    ClearOrgImageRows();
    buf.org_link_spans.clear();

    const mepml::Document doc = MepmlParseCurrent(true);
    const int n = buf.LineCount();
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
    int cur_row = 0, cur_col = 0;
    GetCursorForLua(&cur_row, &cur_col);
    const bool conceal = OrgConcealVisible();

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

    const std::vector<mepml::Span> spans = mepml::Highlight(doc);

    // The rows of a caption or alt text the cursor is in show their source,
    // every one of them (the rendered text stands in for all of them).
    std::unordered_set<int> revealed_rows;
    for (const mepml::Block &b : doc.blocks) {
        if (!b.origin.empty()) continue;
        for (const auto &range : {std::make_pair(b.caption_line, b.caption_line_end), std::make_pair(b.alt_line, b.alt_line_end)})
            if (range.first >= 0 && cur_row >= range.first && cur_row <= range.second)
                for (int row = range.first; row <= range.second; ++row) revealed_rows.insert(row);
    }
    MepmlBuildCards(doc);
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
        if (s.line < 0 || s.line >= n) continue;
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
                d.bold = (s.style & (mepml::kCallout | mepml::kCite)) != 0 && !s.replace.empty() &&
                         (s.style & mepml::kCallout);
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

    if (conceal) MepmlTableLayout(doc, spans, table_layout_rows, table_edge_markup, ns);
    MepmlFitCards();

    // Callouts get a coloured bar in the sign column on every line.
    for (const mepml::Block &b : doc.blocks) {
        if (!b.origin.empty() || b.kind != mepml::BlockKind::Callout) continue;
        for (int row = b.line_start; row <= b.line_end; ++row) {
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
        if (dg.line < 0 || dg.line >= n) continue;
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
}

void Editor::RecomputeMepmlFolds() {
    std::vector<Fold> old_folds;
    for (const Fold &f : Buf().folds)
        if (f.provider == "mepml") old_folds.push_back(f);
    ClearFoldsFromProvider("mepml");
    if (!IsMepmlBuffer()) return;
    const mepml::Document doc = MepmlParseCurrent(false);
    const int n = Buf().LineCount();
    auto add = [&](int start, int end) {
        if (end < start) return;
        bool closed = false;
        for (const Fold &of : old_folds) {
            if (of.start_row == start) {
                closed = of.closed;
                break;
            }
        }
        Buf().folds.push_back({start, end, closed, "mepml"});
    };
    // Heading sections: to the line before the next heading of the same or
    // shallower depth, trailing blank lines excluded.
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
        while (end > heads[k]->line_start && Buf().lines[static_cast<size_t>(end)].find_first_not_of(" \t") == std::string::npos)
            --end;
        add(heads[k]->line_start, end);
    }
    for (const HeaderRun &run : HeaderRuns(doc)) add(run.first, run.last);
    for (const mepml::Slide &sl : mepml::Slides(doc, n)) add(sl.line_start, sl.line_end);
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
            add(b.line_start, b.result_line_start - 1);
            add(b.result_line_start, b.result_line_end);
            continue;
        }
        if (b.kind == mepml::BlockKind::Code || b.kind == mepml::BlockKind::Citation ||
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
OrgLatexFragments Editor::MepmlLatexFragments() const {
    OrgLatexFragments out;
    const mepml::Document doc = MepmlParseCurrent(false);
    std::function<void(const mepml::Block &, const std::vector<mepml::Inline> &)> walk =
        [&](const mepml::Block &b, const std::vector<mepml::Inline> &ins) {
            for (const mepml::Inline &x : ins) {
                if (x.kind == mepml::InlineKind::Math) {
                    OrgLatexInlineFragment f;
                    const bool display = x.arg == "display";
                    f.body = (display ? "\\[" : "$") + x.text + (display ? "\\]" : "$");
                    mepml::Block::Pos p = b.OffsetToPos(x.start), q = b.OffsetToPos(x.end);
                    for (int line = p.line; line <= q.line; ++line) {
                        OrgLatexInlinePart part;
                        part.row = line + 1;
                        part.col_start = (line == p.line ? p.col : 0) + 1;
                        part.col_end = (line == q.line ? q.col : static_cast<int>(Buf().lines[static_cast<size_t>(line)].size())) + 1;
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

std::vector<OrgLiteralSpan> Editor::MepmlLiteralSpans() const {
    std::vector<OrgLiteralSpan> out;
    const mepml::Document doc = MepmlParseCurrent(false);
    constexpr std::uint32_t kLiteral = mepml::kCode | mepml::kResult | mepml::kVerbatim | mepml::kMath |
                                       mepml::kMeta | mepml::kDirective | mepml::kCite | mepml::kTableRule |
                                       mepml::kRule;
    // Markup covers a link's `|url]` and a command's `\color{red}{` too,
    // so a URL or a colour name is never checked as a word.
    for (const mepml::Span &s : mepml::Highlight(doc)) {
        if (!s.markup && !(s.style & kLiteral)) continue;
        OrgLiteralSpan sp;
        sp.row = s.line + 1;
        sp.col_start = s.col_start + 1;
        sp.col_end = s.col_end + 1;
        out.push_back(sp);
    }
    return out;
}

void Editor::MepmlTableLayout(const mepml::Document &doc, const std::vector<mepml::Span> &spans,
                              const std::unordered_set<int> &rows,
                              const std::set<std::pair<int, int>> &edge_markup, int ns) {
    Buffer &buf = Buf();
    const int n = buf.LineCount();
    const bool pictures = OrgImagesVisible();
    std::unordered_map<int, std::vector<const mepml::Span *>> by_line;
    for (const mepml::Span &sp : spans)
        if (rows.count(sp.line)) by_line[sp.line].push_back(&sp);

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

    for (const mepml::Block &b : doc.blocks) {
        if (!b.origin.empty() || b.kind != mepml::BlockKind::Table) continue;
        int body_end = b.line_end;
        if (b.caption_line >= 0) body_end = std::min(body_end, b.caption_line - 1);
        if (b.alt_line >= 0) body_end = std::min(body_end, b.alt_line - 1);
        if (b.rows_end >= 0) body_end = std::min(body_end, b.rows_end);

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
                // Width measured from the concealed spans when the row is
                // laid out, raw codepoints for the cursor's own row (drawn
                // as typed), so its cells still count toward the columns.
                auto it = by_line.find(row);
                if (!c.image.empty()) {
                    c.width = 0;
                } else if (it != by_line.end()) {
                    for (const mepml::Span *sp : it->second)
                        if (sp->col_start >= c.cs && sp->col_end <= c.ce) c.width += span_width(*sp, line);
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
                std::string rule = "├";
                for (size_t k = 0; k < widths.size(); ++k) {
                    if (k) rule += "┼";
                    rule += Repeat("─", widths[k] + 2);
                }
                rule += "┤";
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
                // ASCII, like org's own pipes: the grid pass draws the
                // rules and outline over it, and a full-height box glyph
                // would poke out past the outline's rounded corners.
                text += picture_row ? " " : "|";
                if (k < cs.size()) text += " " + std::string(static_cast<size_t>(before[k]), ' ');
                // A short row: draw its missing cells after the last pipe.
                if (k + 1 == nb)
                    for (size_t m = cs.size(); m < widths.size(); ++m)
                        text += " " + std::string(static_cast<size_t>(widths[m]), ' ') + " |";
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
    for (const HeaderRun &run : HeaderRuns(doc)) {
        OrgBlockCard head;
        head.meta_row = head.begin_row = run.first;
        head.end_row = run.last;
        head.kind = "header";
        head.bare = true;
        head.content_cols = widest(run.first, run.last);
        cards.push_back(std::move(head));
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
}  // namespace

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
    run.label = argv ? blk->lang + " program" : command.substr(0, command.find('\n'));
    run.temp_files = temp_files;
    // Its last picture goes to file= (relative to the document), else
    // beside the document under a name its code decides; an unsaved
    // document keeps none.
    if (!doc_file.empty()) {
        std::string ref = StrOption(*blk, "file");
        if (ref.empty()) ref = doc_file.stem().string() + "-gui-" + CodeHash(blk->code) + ".png";
        const std::filesystem::path p(ref);
        run.snapshot_ref = ref;
        run.snapshot_path = (p.is_absolute() ? p : doc_file.parent_path() / p).lexically_normal().string();
    }
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
            lines.push_back("[stopped]");
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
