// mepml -> Markdown, Org, plain text, RTF, DOCX, ODT and LaTeX. See
// mepml_convert.h for how the formats divide between this file (writers
// that walk the parsed mepml::Document) and mep's HTML-based exporters.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "doc_export.h"
#include "mepml_convert.h"
#include "zip_archive.h"

namespace mepml {

// --- formats --------------------------------------------------------------

namespace {
// A paragraph of nothing but \raw text: its text is the export's own
// markup (a whole <w:p>, say), so it gets no paragraph around it.
bool OnlyRaw(const std::vector<Inline> &ins) {
    bool any = false;
    for (const Inline &x : ins) {
        if (x.kind == InlineKind::Raw) any = true;
        else if (x.kind != InlineKind::Text || x.text.find_first_not_of(" \t\n") != std::string::npos) return false;
    }
    return any;
}
std::string LowerStr(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

Format FormatFromName(const std::string &name_in) {
    const std::string n = LowerStr(name_in);
    if (n == "mepml") return Format::Mepml;
    if (n == "html" || n == "htm") return Format::Html;
    if (n == "md" || n == "markdown") return Format::Markdown;
    if (n == "org") return Format::Org;
    if (n == "rtf") return Format::Rtf;
    if (n == "docx" || n == "word") return Format::Docx;
    if (n == "odt") return Format::Odt;
    if (n == "tex" || n == "latex") return Format::Latex;
    if (n == "pdf") return Format::Pdf;
    if (n == "txt" || n == "text") return Format::Text;
    if (n == "pptx" || n == "powerpoint") return Format::Pptx;
    if (n == "odp" || n == "impress") return Format::Odp;
    return Format::Unknown;
}

Format FormatFromPath(const std::string &path) {
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return Format::Unknown;
    return FormatFromName(path.substr(dot + 1));
}

std::vector<std::string> ExportTags(Format f, const Document &doc, bool beamer) {
    const bool deck = beamer || IsPresentation(doc);
    switch (f) {
        case Format::Html: return deck && !beamer ? std::vector<std::string>{"html", "slides"} : std::vector<std::string>{"html"};
        case Format::Latex:
            return deck ? std::vector<std::string>{"beamer", "tex", "latex", "slides"} : std::vector<std::string>{"tex", "latex"};
        case Format::Pdf:
            return deck ? std::vector<std::string>{"pdf", "beamer", "tex", "latex", "slides"}
                        : std::vector<std::string>{"pdf", "tex", "latex"};
        case Format::Markdown: return {"md", "markdown"};
        case Format::Org: return {"org"};
        case Format::Rtf: return {"rtf"};
        case Format::Docx: return {"docx", "word", "office"};
        case Format::Odt: return {"odt", "office"};
        case Format::Text: return {"txt", "text"};
        case Format::Pptx: return {"pptx", "powerpoint", "office", "slides"};
        case Format::Odp: return {"odp", "impress", "office", "slides"};
        case Format::Mepml:
        case Format::Unknown: break;
    }
    return {};
}

std::string FormatExtension(Format f) {
    switch (f) {
        case Format::Mepml: return "mepml";
        case Format::Html: return "html";
        case Format::Markdown: return "md";
        case Format::Org: return "org";
        case Format::Rtf: return "rtf";
        case Format::Docx: return "docx";
        case Format::Odt: return "odt";
        case Format::Latex: return "tex";
        case Format::Pdf: return "pdf";
        case Format::Text: return "txt";
        case Format::Pptx: return "pptx";
        case Format::Odp: return "odp";
        case Format::Unknown: break;
    }
    return "";
}

bool CanExport(Format f) { return f != Format::Unknown && f != Format::Mepml; }
bool CanImport(Format f) {
    return f == Format::Html || f == Format::Markdown || f == Format::Org || f == Format::Rtf || f == Format::Docx ||
           f == Format::Odt || f == Format::Text;
}

namespace {

// --- shared helpers ---------------------------------------------------------

std::string Join(const std::vector<std::string> &v, const std::string &sep) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) out += (i ? sep : "") + v[i];
    return out;
}

std::string TrimStr(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// An html result as readable text, for the formats that cannot hold HTML
// (plain text, RTF, Word, OpenDocument): the markup's words, one line per
// block element, table cells two spaces apart; scripts and styles dropped.
std::vector<std::string> HtmlResultText(const std::string &html) {
    std::string text;
    const std::string lower = LowerStr(html);
    size_t i = 0;
    while (i < html.size()) {
        if (html[i] != '<') {
            if (html[i] == '&') {
                static const std::pair<const char *, const char *> kEnt[] = {
                    {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&apos;", "'"}, {"&nbsp;", " "}};
                bool done = false;
                for (const auto &e : kEnt) {
                    if (lower.compare(i, std::strlen(e.first), e.first) == 0) {
                        text += e.second;
                        i += std::strlen(e.first);
                        done = true;
                        break;
                    }
                }
                if (done) continue;
            }
            text += (html[i] == '\n' || html[i] == '\t' || html[i] == '\r') ? ' ' : html[i];
            ++i;
            continue;
        }
        const size_t gt = html.find('>', i);
        if (gt == std::string::npos) break;
        std::string tag = lower.substr(i + 1, gt - i - 1);
        const bool closing = !tag.empty() && tag[0] == '/';
        if (closing) tag = tag.substr(1);
        tag = tag.substr(0, tag.find_first_of(" \t\n/"));
        i = gt + 1;
        // Whatever a script, style or head holds is not text.
        if (!closing && (tag == "script" || tag == "style" || tag == "head" || tag == "title")) {
            const size_t end = lower.find("</" + tag, i);
            i = end == std::string::npos ? html.size() : lower.find('>', end) + 1;
            continue;
        }
        static const char *kBlock[] = {"p", "div", "br", "tr", "li", "ul", "ol", "table", "h1", "h2", "h3", "h4", "h5", "h6",
                                       "pre", "hr", "section", "article", "header", "footer", "blockquote", "figure", "figcaption",
                                       "thead", "tbody", "dl", "dt", "dd", "caption"};
        bool block = false;
        for (const char *b : kBlock) block = block || tag == b;
        if (block) text += '\n';
        else if (closing && (tag == "td" || tag == "th")) text += "  ";
    }
    std::vector<std::string> out;
    std::istringstream ss(text);
    std::string l;
    while (std::getline(ss, l)) {
        std::string t;
        for (char c : l) {
            if (c == ' ' && !t.empty() && t.back() == ' ' && t.size() > 1 && t[t.size() - 2] == ' ') continue;  // keep a cell gap, fold the rest
            t += c;
        }
        t = TrimStr(t);
        if (!t.empty()) out.push_back(t);
    }
    return out;
}

// A code block's results as lines of text: an html result as its text.
// Markdown results have none of their own: they are the blocks after the
// code block, exported as any other.
std::vector<std::string> ResultTextLines(const Block &b) {
    if (b.result_format == "markdown") return {};
    return b.result_format == "html" ? HtmlResultText(Join(b.result_lines, "\n")) : b.result_lines;
}

// Soft line breaks inside a paragraph (the source's own wrapping) as
// spaces, for formats where a newline would mean something else.
std::string Unwrap(const std::string &s) {
    std::string o = s;
    for (char &c : o)
        if (c == '\n') c = ' ';
    return o;
}

bool ReadBinary(const std::string &path, std::string *out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    *out = ss.str();
    return true;
}

std::string ResolveRel(const std::string &base_dir, const std::string &p) {
    if (p.empty() || p[0] == '/' || base_dir.empty()) return p;
    std::string q = p;
    while (q.rfind("./", 0) == 0) q = q.substr(2);
    return base_dir + "/" + q;
}

// Pixel size from a PNG or JPEG header; false for anything else.
bool ImageSize(const std::string &b, int *w, int *h, std::string *ext) {
    auto u8 = [&](size_t i) { return i < b.size() ? static_cast<unsigned>(static_cast<unsigned char>(b[i])) : 0u; };
    if (b.size() > 24 && b.compare(1, 3, "PNG") == 0) {
        *w = static_cast<int>((u8(16) << 24) | (u8(17) << 16) | (u8(18) << 8) | u8(19));
        *h = static_cast<int>((u8(20) << 24) | (u8(21) << 16) | (u8(22) << 8) | u8(23));
        *ext = "png";
        return true;
    }
    if (b.size() > 4 && u8(0) == 0xFF && u8(1) == 0xD8) {
        size_t i = 2;
        while (i + 9 < b.size()) {
            if (u8(i) != 0xFF) return false;
            const unsigned marker = u8(i + 1);
            const size_t len = (u8(i + 2) << 8) | u8(i + 3);
            if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
                *h = static_cast<int>((u8(i + 5) << 8) | u8(i + 6));
                *w = static_cast<int>((u8(i + 7) << 8) | u8(i + 8));
                *ext = "jpeg";
                return true;
            }
            i += 2 + len;
        }
    }
    return false;
}

// ===========================================================================
// Markdown
// ===========================================================================

// The document's \citation entries as mepml source, for formats whose
// importer can find them again in a comment (Markdown, Org).
std::string CitationsMepml(const Document &doc) {
    std::string c;
    for (const auto &kv : doc.citations) {
        std::string fields;
        for (size_t f = 0; f < kv.second.field_order.size(); ++f) {
            const std::string &name = kv.second.field_order[f];
            std::string v = kv.second.fields.at(name);
            // Nothing in a value may end the comment it travels in.
            for (size_t at = v.find("--"); at != std::string::npos; at = v.find("--", at + 2)) v.replace(at, 2, "-\\-");
            for (size_t at = v.find("#+end"); at != std::string::npos; at = v.find("#+end", at + 2)) v.replace(at, 1, "\\#");
            fields += "\n  " + name + " = {" + v + "}" + (f + 1 < kv.second.field_order.size() ? "," : "");
        }
        // `\citation(key, fields)`, unless a parenthesis in a value would
        // end the group early; the older `@citation{key}{fields}` then.
        int depth = 0;
        for (char ch : fields) depth += ch == '(' ? 1 : ch == ')' ? -1 : 0;
        const bool paren = depth == 0 && fields.find(')') == std::string::npos;
        c += (c.empty() ? "" : "\n") + (paren ? "\\citation(" + kv.first + "," + fields + "\n)"
                                             : "@citation{" + kv.first + "}{" + fields + "\n}");
    }
    return c;
}

// A figure whose \alttext is empty: it only decorates the page.
bool Decorative(const Block &b) { return b.alt_line >= 0 && b.alt.empty(); }

struct MdWriter {
    const Document &doc;
    std::vector<std::pair<int, std::string>> footnotes;

    static std::string Esc(const std::string &s) {
        std::string o;
        for (char c : s) {
            if (std::strchr("\\`*_[]<>#|@$^~", c)) o += '\\';
            o += c;
        }
        return o;
    }
    static std::string Ticks(const std::string &code) {
        size_t run = 0, best = 0;
        for (char c : code) {
            run = c == '`' ? run + 1 : 0;
            best = std::max(best, run);
        }
        const std::string fence(best + 1, '`');
        const bool pad = !code.empty() && (code.front() == '`' || code.back() == '`');
        return fence + (pad ? " " : "") + code + (pad ? " " : "") + fence;
    }

    std::string Inl(const std::vector<Inline> &ins) {
        std::string o;
        for (const Inline &x : ins) o += Node(x);
        return o;
    }
    std::string Node(const Inline &x) {
        const std::string in = Inl(x.children);
        switch (x.kind) {
            case InlineKind::Text: return Esc(x.text);
            case InlineKind::Bold: return "**" + in + "**";
            case InlineKind::Italic: return "*" + in + "*";
            case InlineKind::Underline: return "<u>" + in + "</u>";
            case InlineKind::Superscript: return "<sup>" + in + "</sup>";
            case InlineKind::Subscript: return "<sub>" + in + "</sub>";
            case InlineKind::Small: return "<small>" + in + "</small>";
            case InlineKind::Big: return "<big>" + in + "</big>";
            case InlineKind::Mono: return "<code>" + in + "</code>";
            case InlineKind::Highlight: return "<mark>" + in + "</mark>";
            case InlineKind::Strike: return "~~" + in + "~~";
            case InlineKind::Insert: return "<ins>" + in + "</ins>";
            case InlineKind::Delete: return "<del>" + in + "</del>";
            case InlineKind::Verbatim: return Ticks(x.text);
            case InlineKind::Link: return "[" + in + "](" + x.arg + ")";
            case InlineKind::Font: return "<span style=\"font-family:" + x.arg + "\">" + in + "</span>";
            case InlineKind::FontSize: return "<span style=\"font-size:" + x.arg + "pt\">" + in + "</span>";
            case InlineKind::Color: return "<span style=\"color:" + x.arg + "\">" + in + "</span>";
            case InlineKind::Class: return IsBoxKindName(x.arg) ? "<span class=\"mep-" + x.arg + "\">" + in + "</span>" : in;
            case InlineKind::Footnote:
                footnotes.emplace_back(x.number, Unwrap(in));
                return "[^" + std::to_string(x.number) + "]";
            case InlineKind::Cite: return "@" + x.text;
            case InlineKind::CiteP: return "[@" + x.text + "]";
            case InlineKind::Math: return x.arg == "display" ? "$$" + x.text + "$$" : "$" + x.text + "$";
            case InlineKind::Comment: return "";
            case InlineKind::Raw: return x.text;  // for this export: as it is
            case InlineKind::Command: return Esc("\\" + x.arg + "(" + x.text + ")");  // no \define: as written
        }
        return "";
    }

    std::string Write() {
        std::vector<std::string> blocks;
        // Front matter: the title and every other //? key.
        {
            std::string fm;
            for (const auto &kv : doc.meta) {
                // An export has its imports' content inlined already.
                if (kv.first.empty() || LowerStr(kv.first) == "import") continue;
                std::string key = LowerStr(kv.first);
                std::string v = kv.second;
                if (key == "option") {
                    const size_t eq = v.find('=');
                    if (eq == std::string::npos) continue;
                    key = TrimStr(v.substr(0, eq));
                    v = Value::Parse(v.substr(eq + 1)).s;
                }
                std::string q;
                for (char c : v) q += (c == '"' ? std::string("\\\"") : std::string(1, c));
                fm += key + ": \"" + q + "\"\n";
            }
            if (!fm.empty()) blocks.push_back("---\n" + fm + "---");
        }
        const std::vector<std::string> labels = BlockLabels(doc);
        const std::vector<bool> export_hidden = ExportHidden(doc);
        for (size_t bi = 0; bi < doc.blocks.size(); ++bi) {
            const Block &b = doc.blocks[bi];
            if (export_hidden[bi]) continue;
            bool show_code = true, show_results = true;
            if (b.kind == BlockKind::Code) CodeExports(doc, b, &show_code, &show_results);
            const std::string caption = b.caption_inlines.empty() ? "" : Inl(b.caption_inlines);
            auto captioned = [&](const std::string &body) {
                if (caption.empty()) return body;
                return body + "\n\n*" + (labels[bi].empty() ? "" : labels[bi] + ": ") + caption + "*";
            };
            switch (b.kind) {
                case BlockKind::Paragraph: blocks.push_back(Inl(b.inlines)); break;
                case BlockKind::Heading: blocks.push_back(std::string(static_cast<size_t>(b.level), '#') + " " + Unwrap(Inl(b.inlines))); break;
                case BlockKind::MathBlock: blocks.push_back("$$\n" + b.code + "\n$$"); break;
                case BlockKind::Code: {
                    std::string opts;
                    for (const Option &o : b.options) {
                        if (!opts.empty()) opts += ", ";
                        opts += o.name + "=\"" + o.value.s + "\"";
                    }
                    std::string fence = "```";
                    while (b.code.find(fence) != std::string::npos) fence += "`";
                    std::string out = show_code ? fence + b.lang + (opts.empty() ? "" : " {" + opts + "}") + "\n" + b.code + "\n" + fence : "";
                    std::vector<std::string> text;
                    // (Markdown results are the blocks that follow.)
                    const std::vector<std::string> none;
                    for (const std::string &l : b.result_format == "markdown" || !show_results ? none : b.result_lines) {
                        std::string img;
                        if (!b.result_format.empty() || !ResultImagePath(l, &img)) text.push_back(l);
                    }
                    // HTML the block produced: Markdown's own raw HTML, between
                    // markers mep's importer reads it back from.
                    auto para = [&out](const std::string &more) { out += (out.empty() ? "" : "\n\n") + more; };
                    if (b.result_line_start >= 0 && !text.empty() && b.result_format == "html")
                        para("<!-- mepml:results html -->\n" + Join(text, "\n") + "\n<!-- /mepml:results -->");
                    else if (b.result_line_start >= 0 && !text.empty())
                        para("```output\n" + Join(text, "\n") + "\n```");
                    std::string figs;
                    if (show_results)
                        for (const auto &im : b.result_images) figs += (figs.empty() ? "" : "\n") + ("![" + b.alt + "](" + im.second + ")");
                    if (!figs.empty()) para(captioned(figs));
                    else if (!caption.empty() && !out.empty()) para("*" + caption + "*");
                    if (!out.empty()) blocks.push_back(out);
                    break;
                }
                case BlockKind::Image: blocks.push_back(captioned("![" + b.alt + "](" + b.value + ")")); break;
                case BlockKind::Table: {
                    if (b.rows.empty()) break;
                    size_t cols = 0;
                    for (const auto &r : b.rows) cols = std::max(cols, r.size());
                    auto row = [&](const std::vector<TableCell> &cells) {
                        std::string o = "|";
                        for (size_t c = 0; c < cols; ++c) {
                            std::string t = c < cells.size() ? Unwrap(Inl(cells[c].content)) : "";
                            o += " " + t + " |";
                        }
                        return o;
                    };
                    std::vector<std::string> lines;
                    lines.push_back(row(b.rows[0]));
                    std::string sep = "|";
                    for (size_t c = 0; c < cols; ++c) {
                        const Align a = c < b.aligns.size() ? b.aligns[c] : Align::Default;
                        sep += a == Align::Left ? " :--- |" : a == Align::Right ? " ---: |" : a == Align::Center ? " :---: |" : " --- |";
                    }
                    lines.push_back(sep);
                    for (size_t r = 1; r < b.rows.size(); ++r) lines.push_back(row(b.rows[r]));
                    blocks.push_back(captioned(Join(lines, "\n")));
                    break;
                }
                case BlockKind::List: {
                    std::vector<std::string> lines;
                    for (const ListItem &it : b.items) {
                        std::string m = it.ordered ? std::to_string(it.number) + ". " : "- ";
                        if (it.checkbox >= 0) m += it.checkbox ? "[x] " : "[ ] ";
                        lines.push_back(std::string(static_cast<size_t>(it.indent), ' ') + m + Unwrap(Inl(it.content)));
                    }
                    blocks.push_back(Join(lines, "\n"));
                    break;
                }
                case BlockKind::Rule: blocks.push_back("---"); break;
                case BlockKind::Bibliography: {
                    // The markers are invisible in rendered Markdown; mep's
                    // importer turns them back into \bibliography / \toc.
                    std::vector<std::string> lines{"<!-- mepml:bibliography -->", "## References", ""};
                    int n = 0;
                    for (const std::string &key : doc.cite_order) {
                        const BibEntryParts e = BibEntry(doc.citations.at(key));
                        lines.push_back(std::to_string(++n) + ". " + Esc(e.lead) + (e.title.empty() ? "" : "*" + Esc(e.title) + "*") + Esc(e.rest));
                    }
                    blocks.push_back(Join(lines, "\n"));
                    break;
                }
                case BlockKind::TableOfContents: {
                    std::vector<std::string> lines{"<!-- mepml:toc -->", "## Contents", ""};
                    for (const Block &h : doc.blocks) {
                        if (h.kind != BlockKind::Heading) continue;
                        const std::string t = InlinePlainText(h.inlines);
                        std::string slug;
                        for (char c : LowerStr(t)) {
                            if (std::isalnum(static_cast<unsigned char>(c))) slug += c;
                            else if (c == ' ' || c == '-') slug += '-';
                        }
                        lines.push_back(std::string(static_cast<size_t>(2 * (h.level - 1)), ' ') + "- [" + Esc(t) + "](#" + slug + ")");
                    }
                    blocks.push_back(Join(lines, "\n"));
                    break;
                }
                case BlockKind::Abstract: {
                    // Marked like \toc, so mep's importer gets \abstract back.
                    std::vector<std::string> parts{"<!-- mepml:abstract -->\n**Abstract**"};
                    for (const std::vector<Inline> &para : AbstractParagraphs(b)) parts.push_back(Inl(para));
                    parts.push_back("<!-- /mepml:abstract -->");
                    blocks.push_back(Join(parts, "\n\n"));
                    break;
                }
                case BlockKind::BoxBegin: {
                    // Markdown has no box: a bold heading, its content, and
                    // markers (holding the title) mep's importer reads back.
                    const std::string title = b.caption_inlines.empty() ? "" : Unwrap(Inl(b.caption_inlines));
                    blocks.push_back("<!-- mepml:box " + b.keyword + (title.empty() ? "" : " " + title) + " -->\n**" +
                                     ExportBoxLook(doc, b.keyword).label + (title.empty() ? "" : ": " + title) + "**");
                    if (!b.inlines.empty()) blocks.push_back(Inl(b.inlines));
                    if (b.box_closed) blocks.push_back("<!-- /mepml:box -->");
                    break;
                }
                case BlockKind::BoxEnd: blocks.push_back("<!-- /mepml:box -->"); break;
                case BlockKind::Callout:  // (a comment: the author's own note)
                case BlockKind::Comment:
                case BlockKind::Meta:
                case BlockKind::Import:
                case BlockKind::Citation:
                // (A slide's content exports as ordinary blocks here; the
                // slide formats -- beamer, pptx, odp -- are their own.)
                case BlockKind::SlideBegin:
                case BlockKind::SlideEnd: break;
                case BlockKind::Define: break;
                case BlockKind::Raw: blocks.push_back(b.code); break;
                case BlockKind::Command: blocks.push_back(Esc(b.text)); break;
            }
        }
        // The bibliography's entries, as mepml, in a comment.
        if (!doc.citations.empty()) blocks.push_back("<!-- mepml\n" + CitationsMepml(doc) + "\n-->");
        std::string out = Join(blocks, "\n\n");
        if (!footnotes.empty()) {
            out += "\n";
            for (const auto &fn : footnotes) out += "\n[^" + std::to_string(fn.first) + "]: " + fn.second;
        }
        return out + "\n";
    }
};

// ===========================================================================
// Org
// ===========================================================================

// A paragraph keeps its source line breaks, but a line that would start an
// Org element (a footnote definition, a headline, a list item, a keyword,
// a table row, a drawer) joins the line before it.
std::string SafeLines(const std::string &text) {
    std::string out;
    std::istringstream ss(text);
    std::string l;
    bool first = true;
    while (std::getline(ss, l)) {
        const std::string t = TrimStr(l);
        size_t d = 0;
        while (d < t.size() && std::isdigit(static_cast<unsigned char>(t[d]))) ++d;
        size_t stars = 0;
        while (stars < t.size() && t[stars] == '*') ++stars;
        const bool starts_element = t.rfind("[fn:", 0) == 0 || (stars > 0 && stars < t.size() && t[stars] == ' ') || t.rfind("#", 0) == 0 ||
                                    t.rfind("- ", 0) == 0 || t.rfind("+ ", 0) == 0 || t.rfind("|", 0) == 0 || t.rfind(":", 0) == 0 ||
                                    (d > 0 && d < t.size() && (t[d] == '.' || t[d] == ')'));
        if (first) out = l;
        else out += (starts_element ? " " : "\n") + (starts_element ? t : l);
        first = false;
    }
    return out;
}

struct OrgWriter {
    const Document &doc;
    std::vector<std::pair<int, std::string>> footnotes;

    std::string Inl(const std::vector<Inline> &ins) {
        // Adjacent text (an escaped marker is a node of its own) is escaped
        // as one, so Text() sees what is around each marker.
        std::string o, text;
        for (const Inline &x : ins) {
            if (x.kind == InlineKind::Text) {
                text += x.text;
                continue;
            }
            o += Text(text) + Node(x);
            text.clear();
        }
        return o + Text(text);
    }
    // Org has no escape character; the documented way to keep a marker
    // literal is a zero-width space in front of it, where it could open.
    static std::string Text(const std::string &t) {
        std::string o;
        for (size_t i = 0; i < t.size(); ++i) {
            const char c = t[i];
            const bool could_open = std::strchr("*/_=~+", c) && (i == 0 || std::strchr(" \t\n-({'\"", t[i - 1])) && i + 1 < t.size() &&
                                    t[i + 1] != ' ' && t[i + 1] != '\n';
            if (could_open) o += "\u200b";
            o += c;
        }
        return o;
    }
    std::string Node(const Inline &x) {
        const std::string in = Inl(x.children);
        switch (x.kind) {
            case InlineKind::Text: return Text(x.text);
            case InlineKind::Bold: return "*" + in + "*";
            case InlineKind::Italic: return "/" + in + "/";
            case InlineKind::Underline: return "_" + in + "_";
            case InlineKind::Superscript: return "^{" + in + "}";
            case InlineKind::Subscript: return "_{" + in + "}";
            case InlineKind::Strike:
            case InlineKind::Delete: return "+" + in + "+";
            case InlineKind::Mono: return "=" + InlinePlainText(x.children) + "=";
            case InlineKind::Verbatim: return "~" + x.text + "~";
            case InlineKind::Link: return "[[" + x.arg + "][" + in + "]]";
            case InlineKind::Footnote:
                footnotes.emplace_back(x.number, Unwrap(in));
                return "[fn:" + std::to_string(x.number) + "]";
            case InlineKind::Cite: return "[cite/t:@" + x.text + "]";
            case InlineKind::CiteP: return "[cite:@" + x.text + "]";
            case InlineKind::Math: return x.arg == "display" ? "\\[" + x.text + "\\]" : "\\(" + x.text + "\\)";
            case InlineKind::Comment: return "";
            case InlineKind::Raw: return x.text;
            case InlineKind::Command: return Text("\\" + x.arg + "(" + x.text + ")");
            // No org equivalent: the text, unstyled.
            case InlineKind::Small:
            case InlineKind::Big:
            case InlineKind::Highlight:
            case InlineKind::Insert:
            case InlineKind::Font:
            case InlineKind::FontSize:
            case InlineKind::Color: return in;
            case InlineKind::Class: return in;
        }
        return in;
    }

    std::string Write() {
        std::vector<std::string> blocks;
        std::vector<std::string> head;
        for (const auto &kv : doc.meta) {
            if (kv.first.empty() || LowerStr(kv.first) == "import") continue;  // inlined already
            std::string key = kv.first;
            for (char &c : key) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            head.push_back("#+" + key + ": " + kv.second);
        }
        if (!head.empty()) blocks.push_back(Join(head, "\n"));
        const std::vector<std::string> labels = BlockLabels(doc);
        const std::vector<bool> export_hidden = ExportHidden(doc);
        for (size_t bi = 0; bi < doc.blocks.size(); ++bi) {
            const Block &b = doc.blocks[bi];
            if (export_hidden[bi]) continue;
            bool show_code = true, show_results = true;
            if (b.kind == BlockKind::Code) CodeExports(doc, b, &show_code, &show_results);
            const std::string cap = b.caption_inlines.empty() ? "" : "#+CAPTION: " + Unwrap(Inl(b.caption_inlines)) + "\n";
            switch (b.kind) {
                case BlockKind::Paragraph: blocks.push_back(SafeLines(Inl(b.inlines))); break;
                case BlockKind::Heading: blocks.push_back(std::string(static_cast<size_t>(b.level), '*') + " " + Unwrap(Inl(b.inlines))); break;
                // (A callout -- `// NOTE: ...` -- is a comment like any other:
                // the author's, not the reader's.)
                case BlockKind::Callout:
                case BlockKind::Comment: {
                    std::vector<std::string> lines;
                    std::istringstream ss(b.text);
                    std::string l;
                    while (std::getline(ss, l)) {
                        const size_t p = l.find("//");
                        lines.push_back("#" + (p == std::string::npos ? l : " " + TrimStr(l.substr(p + 2))));
                    }
                    blocks.push_back(Join(lines, "\n"));
                    break;
                }
                case BlockKind::MathBlock: blocks.push_back(cap + "\\[\n" + b.code + "\n\\]"); break;
                case BlockKind::Code: {
                    std::string args;
                    for (const Option &o : b.options) args += " :" + o.name + " " + o.value.s;
                    std::string body;
                    std::istringstream ss(b.code);
                    std::string l;
                    // Org reads a body line starting with `*` or `#+` as
                    // structure unless it is escaped with a comma.
                    while (std::getline(ss, l)) {
                        const std::string t = TrimStr(l);
                        if (!t.empty() && (t[0] == '*' || t.rfind("#+", 0) == 0 || t.rfind(",*", 0) == 0)) l = "," + l;
                        body += l + "\n";
                    }
                    std::string out = show_code ? "#+begin_src " + (b.lang.empty() ? std::string("text") : b.lang) + args + "\n" + body + "#+end_src" : "";
                    if (!show_results) {
                    } else if (b.result_line_start >= 0 && b.result_format == "html") {
                        // org-babel's own form for `:results html`.
                        out += std::string(out.empty() ? "" : "\n\n") + "#+RESULTS:\n#+begin_export html\n" + Join(b.result_lines, "\n") + "\n#+end_export";
                    } else if (b.result_line_start >= 0 && b.result_format != "markdown") {
                        out += std::string(out.empty() ? "" : "\n\n") + (b.result_images.empty() ? std::string() : cap) + "#+RESULTS:";
                        for (const std::string &rl : b.result_lines) {
                            std::string img;
                            out += "\n" + (ResultImagePath(rl, &img) ? "[[file:" + img + "]]" : ": " + rl);
                        }
                    }
                    if (!out.empty()) blocks.push_back(out);
                    break;
                }
                // (#+ATTR_HTML's :alt is what Org's own HTML export reads a picture as.)
                case BlockKind::Image:
                    blocks.push_back(cap + (b.alt_line >= 0 ? "#+ATTR_HTML: :alt " + b.alt + "\n" : "") + "[[file:" + b.value + "]]");
                    break;
                case BlockKind::Table: {
                    std::vector<std::string> lines;
                    for (size_t r = 0; r < b.rows.size(); ++r) {
                        std::string o = "|";
                        for (const TableCell &c : b.rows[r]) o += " " + Unwrap(Inl(c.content)) + " |";
                        lines.push_back(o);
                        if (static_cast<int>(r) + 1 == b.header_rows) {
                            std::string sep = "|";
                            for (size_t c = 0; c < b.rows[r].size(); ++c) sep += std::string(c ? "+" : "") + "---";
                            lines.push_back(sep + "|");
                        }
                    }
                    blocks.push_back(cap + Join(lines, "\n"));
                    break;
                }
                case BlockKind::List: {
                    std::vector<std::string> lines;
                    for (const ListItem &it : b.items) {
                        std::string m = it.ordered ? std::to_string(it.number) + ". " : "- ";
                        if (it.checkbox >= 0) m += it.checkbox ? "[X] " : "[ ] ";
                        lines.push_back(std::string(static_cast<size_t>(it.indent), ' ') + m + Unwrap(Inl(it.content)));
                    }
                    blocks.push_back(Join(lines, "\n"));
                    break;
                }
                case BlockKind::Rule: blocks.push_back("-----"); break;
                case BlockKind::Bibliography: {
                    std::vector<std::string> lines{"# mepml:bibliography", "* References"};
                    for (const std::string &key : doc.cite_order) {
                        const BibEntryParts e = BibEntry(doc.citations.at(key));
                        lines.push_back("1. " + e.lead + (e.title.empty() ? "" : "/" + e.title + "/") + e.rest);
                    }
                    blocks.push_back(Join(lines, "\n"));
                    break;
                }
                case BlockKind::TableOfContents: blocks.push_back("#+TOC: headlines 3"); break;
                case BlockKind::Abstract: {
                    // Org's own convention (ox-latex makes it LaTeX's abstract).
                    std::vector<std::string> paras;
                    for (const std::vector<Inline> &para : AbstractParagraphs(b)) paras.push_back(SafeLines(Inl(para)));
                    blocks.push_back("#+begin_abstract\n" + Join(paras, "\n\n") + "\n#+end_abstract");
                    break;
                }
                // Org's special blocks: #+begin_definition Title ... #+end_definition.
                case BlockKind::BoxBegin: {
                    const std::string title = b.caption_inlines.empty() ? "" : SafeLines(Inl(b.caption_inlines));
                    // (A kind of the document's own, `\boxed(axiom, ...)`, is
                    // `box_axiom`: told from Org's own blocks on the way back.)
                    const std::string name = FindBoxKind(b.keyword) ? b.keyword : "box_" + b.keyword;
                    std::string o = "#+begin_" + name + (title.empty() ? "" : " " + title);
                    if (!b.inlines.empty()) o += "\n" + SafeLines(Inl(b.inlines));
                    if (b.box_closed) o += "\n#+end_" + name;
                    blocks.push_back(o);
                    break;
                }
                case BlockKind::BoxEnd: blocks.push_back("#+end_" + (FindBoxKind(b.keyword) ? b.keyword : "box_" + b.keyword)); break;
                case BlockKind::Meta:
                case BlockKind::Import:
                case BlockKind::Citation:
                // (A slide's content exports as ordinary blocks here; the
                // slide formats -- beamer, pptx, odp -- are their own.)
                case BlockKind::SlideBegin:
                case BlockKind::SlideEnd: break;
                case BlockKind::Define: break;
                case BlockKind::Raw: blocks.push_back(b.code); break;
                case BlockKind::Command: blocks.push_back(SafeLines(Text(b.text))); break;
            }
        }
        if (!doc.citations.empty()) blocks.push_back("#+begin_comment\nmepml\n" + CitationsMepml(doc) + "\n#+end_comment");
        std::string out = Join(blocks, "\n\n");
        if (!footnotes.empty()) {
            out += "\n\n* Footnotes\n";
            for (const auto &fn : footnotes) out += "\n[fn:" + std::to_string(fn.first) + "] " + fn.second;
        }
        return out + "\n";
    }
};

// ===========================================================================
// Plain text
// ===========================================================================

struct TextWriter {
    const Document &doc;
    std::vector<std::pair<int, std::string>> footnotes;

    std::string Inl(const std::vector<Inline> &ins) {
        std::string o;
        for (const Inline &x : ins) {
            if (x.kind == InlineKind::Text || x.kind == InlineKind::Verbatim || x.kind == InlineKind::Raw) o += x.text;
            else if (x.kind == InlineKind::Math) o += x.text;
            else if (x.kind == InlineKind::Command) o += "\\" + x.arg + "(" + x.text + ")";  // no \define: as written
            else if (x.kind == InlineKind::Footnote) {
                footnotes.emplace_back(x.number, Unwrap(Inl(x.children)));
                o += "[" + std::to_string(x.number) + "]";
            } else if (x.kind == InlineKind::Cite || x.kind == InlineKind::CiteP) {
                o += CiteLabel(doc, x.text, x.kind == InlineKind::CiteP);
            } else if (x.kind == InlineKind::Link) {
                const std::string t = Inl(x.children);
                o += t == x.arg ? t : t + " <" + x.arg + ">";
            } else if (x.kind != InlineKind::Comment) {
                o += Inl(x.children);
            }
        }
        return o;
    }

    std::string Write() {
        std::vector<std::string> blocks;
        if (!doc.title.empty()) blocks.push_back(doc.title + "\n" + std::string(doc.title.size(), '='));
        const std::vector<std::string> labels = BlockLabels(doc);
        const std::vector<bool> export_hidden = ExportHidden(doc);
        for (size_t bi = 0; bi < doc.blocks.size(); ++bi) {
            const Block &b = doc.blocks[bi];
            if (export_hidden[bi]) continue;
            bool show_code = true, show_results = true;
            if (b.kind == BlockKind::Code) CodeExports(doc, b, &show_code, &show_results);
            const std::string cap = b.caption_inlines.empty() ? "" : "\n" + (labels[bi].empty() ? "" : labels[bi] + ": ") + Inl(b.caption_inlines);
            switch (b.kind) {
                case BlockKind::Paragraph: blocks.push_back(Inl(b.inlines)); break;
                case BlockKind::Heading: {
                    const std::string t = Unwrap(Inl(b.inlines));
                    blocks.push_back(t + "\n" + std::string(t.size(), b.level == 1 ? '=' : '-'));
                    break;
                }
                case BlockKind::MathBlock: blocks.push_back("    " + b.code + cap); break;
                case BlockKind::Code: {
                    std::string out;
                    std::istringstream ss(b.code);
                    std::string l;
                    while (show_code && std::getline(ss, l)) out += "    " + l + "\n";
                    // Without the code, the output is set in as it would be printed.
                    const std::string lead = show_code ? "    > " : "    ";
                    for (const std::string &rl : show_results ? ResultTextLines(b) : std::vector<std::string>()) {
                        std::string img;
                        out += lead + (b.result_format.empty() && ResultImagePath(rl, &img) ? "[figure: " + img + "]" : rl) + "\n";
                    }
                    if (!out.empty()) out.pop_back();
                    if (!out.empty()) blocks.push_back(out + cap);
                    break;
                }
                case BlockKind::Image: blocks.push_back("[figure: " + b.value + "]" + cap); break;
                case BlockKind::Table: {
                    std::vector<std::vector<std::string>> cells;
                    std::vector<size_t> w;
                    for (const auto &r : b.rows) {
                        cells.emplace_back();
                        for (size_t c = 0; c < r.size(); ++c) {
                            cells.back().push_back(Unwrap(Inl(r[c].content)));
                            if (w.size() <= c) w.resize(c + 1, 0);
                            w[c] = std::max(w[c], cells.back().back().size());
                        }
                    }
                    std::vector<std::string> lines;
                    for (size_t r = 0; r < cells.size(); ++r) {
                        std::string o;
                        for (size_t c = 0; c < cells[r].size(); ++c)
                            o += (c ? "  " : "") + cells[r][c] + std::string(w[c] - cells[r][c].size(), ' ');
                        lines.push_back(TrimStr(o));
                        if (static_cast<int>(r) + 1 == b.header_rows) {
                            std::string sep;
                            for (size_t c = 0; c < w.size(); ++c) sep += (c ? "  " : "") + std::string(w[c], '-');
                            lines.push_back(sep);
                        }
                    }
                    blocks.push_back(Join(lines, "\n") + cap);
                    break;
                }
                case BlockKind::List: {
                    std::vector<std::string> lines;
                    for (const ListItem &it : b.items) {
                        std::string m = it.ordered ? std::to_string(it.number) + ". " : "* ";
                        if (it.checkbox >= 0) m += it.checkbox ? "[x] " : "[ ] ";
                        lines.push_back(std::string(static_cast<size_t>(it.indent), ' ') + m + Unwrap(Inl(it.content)));
                    }
                    blocks.push_back(Join(lines, "\n"));
                    break;
                }
                case BlockKind::Rule: blocks.push_back(std::string(40, '-')); break;
                case BlockKind::Bibliography:
                case BlockKind::TableOfContents: {
                    std::vector<std::string> lines;
                    for (const RenderedLine &l : b.kind == BlockKind::Bibliography ? RenderBibliography(doc, 78) : RenderToc(doc, 78))
                        lines.push_back(l.text);
                    blocks.push_back(Join(lines, "\n"));
                    break;
                }
                case BlockKind::Abstract: {
                    std::vector<std::string> parts{"Abstract"};
                    for (const std::vector<Inline> &para : AbstractParagraphs(b)) parts.push_back(Inl(para));
                    blocks.push_back(Join(parts, "\n\n"));
                    break;
                }
                case BlockKind::BoxBegin:
                    blocks.push_back(BoxHeading(doc, b) + (b.inlines.empty() ? "" : ". " + Inl(b.inlines)));
                    break;
                case BlockKind::BoxEnd: break;
                case BlockKind::Callout:  // (a comment: the author's own note)
                case BlockKind::Comment:
                case BlockKind::Meta:
                case BlockKind::Import:
                case BlockKind::Citation:
                // (A slide's content exports as ordinary blocks here; the
                // slide formats -- beamer, pptx, odp -- are their own.)
                case BlockKind::SlideBegin:
                case BlockKind::SlideEnd: break;
                case BlockKind::Define: break;
                case BlockKind::Raw: blocks.push_back(b.code); break;
                case BlockKind::Command: blocks.push_back(b.text); break;
            }
        }
        std::string out = Join(blocks, "\n\n");
        if (!footnotes.empty()) {
            out += "\n\n" + std::string(20, '-');
            for (const auto &fn : footnotes) out += "\n[" + std::to_string(fn.first) + "] " + fn.second;
        }
        return out + "\n";
    }
};

// What mepml knows that an office package has no element for -- //?
// metadata, each code block's language and options, the bibliography's
// fields -- rides along as custom document properties (docProps/custom.xml,
// <meta:user-defined>), which mep's importer reads back and every office
// suite keeps as ordinary custom properties. Code blocks are numbered in
// document order and marked in the body by a bookmark "mepml_code_N".
// Values are single-line: newline, tab and backslash are written \n, \t, \\
// (XML readers are free to fold whitespace in element text).
std::string PropEsc(const std::string &v) {
    std::string o;
    for (char c : v) o += c == '\n' ? std::string("\\n") : c == '\t' ? std::string("\\t") : c == '\\' ? std::string("\\\\") : std::string(1, c);
    return o;
}

std::vector<std::pair<std::string, std::string>> OfficeProps(const Document &doc) {
    std::vector<std::pair<std::string, std::string>> p;
    int n = 0;
    for (const auto &kv : doc.meta)  // (an import's content is inlined in the package already)
        if (LowerStr(kv.first) != "title" && LowerStr(kv.first) != "import")
            p.push_back({"mepml.meta." + std::to_string(++n), kv.first + ": " + kv.second});
    n = 0;
    const std::vector<bool> export_hidden = ExportHidden(doc);
    for (size_t bi = 0; bi < doc.blocks.size(); ++bi) {
        const Block &b = doc.blocks[bi];
        if (b.kind != BlockKind::Code || export_hidden[bi]) continue;
        bool show_code = true, show_results = true;
        CodeExports(doc, b, &show_code, &show_results);
        if (!show_code) continue;  // (its bookmark is not in the body either)
        std::string v = b.lang;
        for (const Option &o : b.options) v += "\n" + o.name + "\t" + o.value.s;
        p.push_back({"mepml.code." + std::to_string(++n), v});
        // An html result is shown as its text in the package; its markup
        // rides along so an import gets the HTML itself back.
        if (b.result_format == "html") p.push_back({"mepml.result." + std::to_string(n), Join(b.result_lines, "\n")});
    }
    for (const auto &kv : doc.citations) {
        std::string v;
        for (const std::string &f : kv.second.field_order) v += f + "\t" + kv.second.fields.at(f) + "\n";
        p.push_back({"mepml.cite." + kv.first, v});
    }
    for (auto &kv : p) kv.second = PropEsc(kv.second);
    return p;
}

// ===========================================================================
// RTF
// ===========================================================================

// What the document's style sheets change of a run, kept apart from the
// run's own formatting: Word and Writer carry it as a character style
// named for the change ("MepSheet-cAA0000-b1"), so the text looks as the
// sheet says and reads back as it was written -- a reader ignores a
// style's look, where bold set on the run itself would come back as *bold*.
struct SheetLook {
    std::string color;  // RRGGBB
    int bold = -1, italic = -1;
    // "ins" / "del": inserted or deleted text whose own colour (what a
    // reader knows it by) gave way to the sheet's -- the style's name says
    // what it was.
    std::string tag;
    bool any() const { return !color.empty() || bold >= 0 || italic >= 0; }
    // +inserted+ and !deleted! text is written in a colour of its own; a
    // sheet's colour for it replaces that one.
    template <typename Fmt>
    void TakeFor(const Inline &x, const ExportTextStyler::Change &ch, Fmt *f) {
        Take(ch);
        if (ch.color.empty() || (x.kind != InlineKind::Insert && x.kind != InlineKind::Delete)) return;
        f->color.clear();
        tag = x.kind == InlineKind::Insert ? "ins" : "del";
    }
    void Take(const ExportTextStyler::Change &ch) {
        if (!ch.color.empty()) color = ch.color;
        if (ch.bold >= 0) bold = ch.bold;
        if (ch.italic >= 0) italic = ch.italic;
    }
    // `sep` is '-' in Word's style ids and '_' in ODF's names.
    std::string Id(char sep) const {
        std::string id = "MepSheet";
        if (!tag.empty()) id += std::string(1, sep) + tag;
        if (!color.empty()) id += std::string(1, sep) + "c" + color;
        if (bold >= 0) id += std::string(1, sep) + (bold ? "b1" : "b0");
        if (italic >= 0) id += std::string(1, sep) + (italic ? "i1" : "i0");
        return id;
    }
};

struct RtfWriter {
    const Document &doc;
    std::string base_dir;
    std::vector<std::string> classes{};  // \class names met, in order: \cs40, \cs41 ...
    std::vector<std::string> fonts{"Times New Roman", "Helvetica", "Courier New"};
    std::vector<std::uint32_t> colors{0x000000, 0x1a5fb4, 0x2e7d32, 0xc62828, 0xfff3a3, 0x6b6b6b};
    enum { kBlack = 1, kBlue = 2, kGreen = 3, kRed = 4, kYellow = 5, kGray = 6 };

    int FontIndex(const std::string &name) {
        for (size_t i = 0; i < fonts.size(); ++i)
            if (LowerStr(fonts[i]) == LowerStr(name)) return static_cast<int>(i);
        fonts.push_back(name);
        return static_cast<int>(fonts.size() - 1);
    }
    int ColorIndex(const std::string &name) {
        std::uint32_t rgb = 0;
        if (!ParseColor(name, &rgb)) return kBlack;
        for (size_t i = 0; i < colors.size(); ++i)
            if (colors[i] == rgb) return static_cast<int>(i) + 1;
        colors.push_back(rgb);
        return static_cast<int>(colors.size());
    }

    // Text with RTF's three specials escaped and non-ASCII as \uN?.
    static std::string Esc(const std::string &s) {
        std::string o;
        for (size_t i = 0; i < s.size();) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (c < 0x80) {
                if (c == '\\' || c == '{' || c == '}') o += '\\';
                o += c == '\n' ? ' ' : static_cast<char>(c);
                ++i;
                continue;
            }
            int n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
            std::uint32_t cp = c & (n == 4 ? 0x07u : n == 3 ? 0x0Fu : 0x1Fu);
            for (int k = 1; k < n && i + static_cast<size_t>(k) < s.size(); ++k)
                cp = (cp << 6) | (static_cast<unsigned char>(s[i + static_cast<size_t>(k)]) & 0x3Fu);
            i += static_cast<size_t>(n);
            if (cp > 0xFFFF) cp = '?';
            o += "\\u" + std::to_string(cp > 0x7FFF ? static_cast<int>(cp) - 0x10000 : static_cast<int>(cp)) + "?";
        }
        return o;
    }

    // (No style sheet's look on a run here: RTF keeps a look on the run
    // itself, where it would read back as the author's own *bold*. Word and
    // Writer carry it in a character style instead -- SheetLook.)
    std::string Inl(const std::vector<Inline> &ins) {
        std::string o;
        for (const Inline &x : ins) o += Node(x);
        return o;
    }
    std::string Node(const Inline &x) {
        const std::string in = Inl(x.children);
        switch (x.kind) {
            case InlineKind::Text: return Esc(x.text);
            case InlineKind::Bold: return "{\\b " + in + "}";
            case InlineKind::Italic: return "{\\i " + in + "}";
            case InlineKind::Underline: return "{\\ul " + in + "}";
            case InlineKind::Superscript: return "{\\super " + in + "}";
            case InlineKind::Subscript: return "{\\sub " + in + "}";
            case InlineKind::Small: return "{\\fs18 " + in + "}";
            case InlineKind::Big: return "{\\fs30 " + in + "}";
            case InlineKind::Mono: return "{\\f2 " + in + "}";
            case InlineKind::Highlight: return "{\\highlight" + std::to_string(kYellow) + " " + in + "}";
            case InlineKind::Strike: return "{\\strike " + in + "}";
            case InlineKind::Insert: return "{\\ul\\cf" + std::to_string(kGreen) + " " + in + "}";
            case InlineKind::Delete: return "{\\strike\\cf" + std::to_string(kRed) + " " + in + "}";
            case InlineKind::Verbatim: return "{\\cs30\\f2 " + Esc(x.text) + "}";
            case InlineKind::Link:
                return "{\\field{\\*\\fldinst{HYPERLINK \"" + Esc(x.arg) + "\"}}{\\fldrslt{\\ul\\cf" + std::to_string(kBlue) + " " + in + "}}}";
            case InlineKind::Font: return "{\\f" + std::to_string(FontIndex(x.arg)) + " " + in + "}";
            case InlineKind::FontSize: {
                const int half = static_cast<int>(std::atof(x.arg.c_str()) * 2.0);
                return "{\\fs" + std::to_string(half > 0 ? half : 24) + " " + in + "}";
            }
            case InlineKind::Color: return "{\\cf" + std::to_string(ColorIndex(x.arg)) + " " + in + "}";
            // A character style named for the class (the stylesheet lists
            // them). Only the name: RTF keeps a style's look on the run
            // itself, where it would read back as the text's own.
            case InlineKind::Class: {
                if (!IsBoxKindName(x.arg)) return in;
                auto it = std::find(classes.begin(), classes.end(), x.arg);
                if (it == classes.end()) it = classes.insert(classes.end(), x.arg);
                return "{\\cs" + std::to_string(40 + (it - classes.begin())) + " " + in + "}";
            }
            case InlineKind::Footnote:
                return "{\\super\\chftn}{\\footnote\\pard\\plain\\fs20 {\\super\\chftn} " + in + "}";
            case InlineKind::Cite:
            case InlineKind::CiteP:
                // A link to the entry's bookmark; the tip says which kind.
                return "{\\field{\\*\\fldinst{HYPERLINK \\\\l \"ref_" + Esc(x.text) + "\" \\\\o \"" +
                       std::string(x.kind == InlineKind::CiteP ? "\\\\citep" : "\\\\cite") + "\"}}{\\fldrslt{" +
                       Esc(CiteLabel(doc, x.text, x.kind == InlineKind::CiteP)) + "}}}";
            case InlineKind::Math: return "{\\cs31\\i " + Esc(x.text) + "}";
            case InlineKind::Comment: return "";
            case InlineKind::Raw: return x.text;
            case InlineKind::Command: return Esc("\\" + x.arg + "(" + x.text + ")");
        }
        return in;
    }

    static std::string Para(const std::string &props, const std::string &body) {
        return "{\\pard\\plain\\sa160\\fs22" + props + " " + body + "\\par}\n";
    }

    int code_n = 0;
    std::vector<bool> lists{};  // \lsN (N = index + 1) -> numbered?
    static std::string Bookmark(const std::string &name) { return "{\\*\\bkmkstart " + name + "}{\\*\\bkmkend " + name + "}"; }

    std::string Picture(const std::string &path, const std::string &alt = "", bool decorative = false) {
        (void)decorative;  // (RTF has no way to say so)
        std::string bytes, ext;
        int w = 0, h = 0;
        if (!ReadBinary(ResolveRel(base_dir, path), &bytes) || !ImageSize(bytes, &w, &h, &ext) || w <= 0 || h <= 0) {
            // Not readable: linked, the way Word links a picture.
            std::string p;
            for (char c : path) p += c == '\\' ? std::string("\\\\\\\\") : std::string(1, c);
            return Para("\\qc", "{\\field{\\*\\fldinst{INCLUDEPICTURE \"" + Esc(p) + "\" \\\\d}}{\\fldrslt{" + Esc(alt) + "}}}");
        }
        // At most 6 inches wide (8640 twips); 15 twips per pixel at 96 dpi.
        double scale = std::min(1.0, 8640.0 / (w * 15.0));
        std::string hex;
        static const char *digits = "0123456789abcdef";
        for (size_t i = 0; i < bytes.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(bytes[i]);
            hex += digits[c >> 4];
            hex += digits[c & 15];
            if (i % 64 == 63) hex += '\n';
        }
        const std::string prop = alt.empty() ? "" : "{\\*\\picprop{\\sp{\\sn wzDescription}{\\sv " + Esc(alt) + "}}}";
        return Para("\\qc", "{\\pict" + prop + "\\" + std::string(ext == "png" ? "pngblip" : "jpegblip") + "\\picw" + std::to_string(w) +
                                 "\\pich" + std::to_string(h) + "\\picwgoal" + std::to_string(static_cast<int>(w * 15 * scale)) +
                                 "\\pichgoal" + std::to_string(static_cast<int>(h * 15 * scale)) + "\n" + hex + "}");
    }

    std::string Caption(const std::string &label, const std::vector<Inline> &cap) {
        if (cap.empty()) return "";
        return Para("\\s10\\qc\\fs20\\i", (label.empty() ? "" : "{\\b " + Esc(label) + ": }") + Inl(cap));
    }

    std::string Write() {
        std::string body;
        if (!doc.title.empty()) body += Para("\\s7\\qc\\sa320\\b\\fs40", Esc(doc.title));
        const std::vector<std::string> labels = BlockLabels(doc);
        static const int kHeadSize[] = {36, 32, 28, 26, 24, 24};
        const std::vector<bool> export_hidden = ExportHidden(doc);
        for (size_t bi = 0; bi < doc.blocks.size(); ++bi) {
            const Block &b = doc.blocks[bi];
            if (export_hidden[bi]) continue;
            bool show_code = true, show_results = true;
            if (b.kind == BlockKind::Code) CodeExports(doc, b, &show_code, &show_results);
            switch (b.kind) {
                case BlockKind::Paragraph: body += OnlyRaw(b.inlines) ? Inl(b.inlines) : Para("", Inl(b.inlines)); break;
                case BlockKind::Heading: {
                    const int lvl = std::min(6, std::max(1, b.level));
                    body += Para("\\s" + std::to_string(lvl) + "\\sb240\\keepn\\b\\outlinelevel" + std::to_string(lvl - 1) + "\\fs" +
                                     std::to_string(kHeadSize[lvl - 1]),
                                 Inl(b.inlines));
                    break;
                }
                case BlockKind::MathBlock:
                    body += Para("\\s18\\qc", "{\\cs31\\i " + Esc(TrimStr(b.code)) + "}") + Caption(labels[bi], b.caption_inlines);
                    break;
                case BlockKind::Code: {
                    // A paragraph per line, the first marked with the block's
                    // number (its header is in \userprops); results in gray.
                    std::istringstream ss(b.code);
                    std::string l;
                    const size_t before = body.size();
                    const std::string style = "\\s8\\li360\\sa0\\f2\\fs20";
                    if (show_code) {
                        std::string mark = Bookmark("mepml_code_" + std::to_string(++code_n));
                        while (std::getline(ss, l)) {
                            body += Para(style, mark + Esc(l));
                            mark.clear();
                        }
                        if (!mark.empty()) body += Para(style, mark);
                    }
                    const std::string gray = "\\cf" + std::to_string(ColorIndex("#555555"));
                    if (show_results) {
                        for (const std::string &rl : ResultTextLines(b)) {
                            std::string img;
                            if (!b.result_format.empty() || !ResultImagePath(rl, &img)) body += Para(style + gray, Esc(rl));
                        }
                        for (const auto &im : b.result_images) body += Picture(im.second, b.alt, Decorative(b));
                    }
                    if (body.size() > before) body += Caption(labels[bi], b.caption_inlines);
                    break;
                }
                case BlockKind::Image: body += Picture(b.value, b.alt, Decorative(b)) + Caption(labels[bi], b.caption_inlines); break;
                case BlockKind::Table: {
                    size_t cols = 0;
                    for (const auto &r : b.rows) cols = std::max(cols, r.size());
                    if (cols == 0) break;
                    const int cw = static_cast<int>(8640 / cols);
                    for (size_t r = 0; r < b.rows.size(); ++r) {
                        body += "\\trowd\\trgaph108";
                        if (static_cast<int>(r) < b.header_rows) body += "\\trhdr";
                        for (size_t c = 0; c < cols; ++c)
                            body += "\\clbrdrt\\brdrs\\clbrdrl\\brdrs\\clbrdrb\\brdrs\\clbrdrr\\brdrs\\cellx" + std::to_string(cw * static_cast<int>(c + 1));
                        body += "\n";
                        for (size_t c = 0; c < cols; ++c) {
                            const std::string t = c < b.rows[r].size() ? Inl(b.rows[r][c].content) : "";
                            const Align a = c < b.aligns.size() ? b.aligns[c] : Align::Default;
                            body += "\\pard\\plain\\intbl\\fs22" +
                                    std::string(a == Align::Center ? "\\qc" : a == Align::Right ? "\\qr" : a == Align::Left ? "\\ql" : "") +
                                    (static_cast<int>(r) < b.header_rows ? "\\b " : " ") + t + "\\cell\n";
                        }
                        body += "\\row\n";
                    }
                    body += Caption(labels[bi], b.caption_inlines);
                    break;
                }
                case BlockKind::List: {
                    // Items numbered per level and kind, as they display; one
                    // list definition per list and kind, so numbering restarts.
                    std::map<std::pair<int, bool>, int> count;
                    int ls[2] = {0, 0};
                    for (const ListItem &it : b.items) {
                        int &id = ls[it.ordered ? 1 : 0];
                        if (id == 0) {
                            lists.push_back(it.ordered);
                            id = static_cast<int>(lists.size());
                        }
                        const std::string m = it.ordered ? std::to_string(++count[{it.indent, true}]) + "." : "\\bullet";
                        const std::string box = it.checkbox < 0 ? "" : it.checkbox ? "\\u9746? " : "\\u9744? ";
                        const int lvl = std::min(8, it.indent / 2);
                        body += Para("\\s20\\ls" + std::to_string(id) + "\\ilvl" + std::to_string(lvl) + "\\fi-360\\li" + std::to_string(360 * (lvl + 2)) + "\\sa60",
                                     "{\\listtext " + m + "\\tab}" + box + Inl(it.content));
                    }
                    break;
                }
                case BlockKind::Rule: body += "{\\pard\\brdrb\\brdrs\\brdrw10\\brsp20 \\par}\n"; break;
                case BlockKind::Bibliography:
                case BlockKind::TableOfContents: {
                    const bool bib = b.kind == BlockKind::Bibliography;
                    body += bib ? Para("\\s1\\sb240\\keepn\\b\\outlinelevel0\\fs36", Bookmark("mepml_bibliography") + "References")
                                : Para("\\s19\\sb240\\keepn\\b\\fs28", "Contents");
                    if (bib) {
                        int n = 0;
                        for (const std::string &key : doc.cite_order) {
                            const BibEntryParts e = BibEntry(doc.citations.at(key));
                            body += Para("\\s17\\fi-360\\li360\\sa60", Bookmark("ref_" + key) + "[" + std::to_string(++n) + "]\\tab " + Esc(e.lead) +
                                                                     (e.title.empty() ? "" : "{\\i " + Esc(e.title) + "}") + Esc(e.rest));
                        }
                    } else {
                        for (const Block &h : doc.blocks)
                            if (h.kind == BlockKind::Heading)
                                body += Para("\\s" + std::to_string(10 + std::min(6, std::max(1, h.level))) + "\\li" + std::to_string(360 * h.level) + "\\sa40",
                                             Esc(InlinePlainText(h.inlines)));
                    }
                    break;
                }
                case BlockKind::Abstract:
                    body += Para("\\s22\\qc\\sb240\\keepn\\b\\fs22", "Abstract");
                    for (const std::vector<Inline> &para : AbstractParagraphs(b)) body += Para("\\s21\\li720\\ri720\\fs20", Inl(para));
                    break;
                case BlockKind::BoxBegin: {
                    // A heading paragraph ruled down its left in the box's
                    // colour, on its paper (paragraph formatting only: the
                    // words read back as they were written).
                    const BoxKind *k = FindBoxKind(b.keyword);
                    // (Its label and colours are the style sheets' where they say.)
                    const BoxLook look = ExportBoxLook(doc, b.keyword);
                    const std::string rule = std::to_string(ColorIndex(k ? look.color : "#2c7fb8"));
                    const std::string tint = std::to_string(ColorIndex(k ? look.tint : "#eef5fb"));
                    std::string heading = Esc(look.label);
                    if (!b.caption_inlines.empty()) heading += ": " + Inl(b.caption_inlines);
                    body += Para("\\s23\\keepn\\sb200\\li120\\brdrl\\brdrs\\brdrw40\\brsp100\\brdrcf" + rule + "\\cbpat" + tint, heading);
                    if (!b.inlines.empty()) body += Para("", Inl(b.inlines));
                    break;
                }
                case BlockKind::BoxEnd: break;
                case BlockKind::Callout:  // (a comment: the author's own note)
                case BlockKind::Comment:
                case BlockKind::Meta:
                case BlockKind::Import:
                case BlockKind::Citation:
                // (A slide's content exports as ordinary blocks here; the
                // slide formats -- beamer, pptx, odp -- are their own.)
                case BlockKind::SlideBegin:
                case BlockKind::SlideEnd: break;
                case BlockKind::Define: break;
                case BlockKind::Raw: body += b.code + "\n"; break;
                case BlockKind::Command: body += Para("", Esc(b.text)); break;
            }
        }
        std::string out = "{\\rtf1\\ansi\\ansicpg1252\\deff0\\uc1\n{\\fonttbl";
        for (size_t i = 0; i < fonts.size(); ++i) {
            const char *fam = i == 0 ? "froman" : i == 2 ? "fmodern" : "fswiss";
            out += "{\\f" + std::to_string(i) + "\\" + fam + " " + fonts[i] + ";}";
        }
        out += "}\n{\\colortbl;";
        for (std::uint32_t c : colors)
            out += "\\red" + std::to_string((c >> 16) & 255) + "\\green" + std::to_string((c >> 8) & 255) + "\\blue" + std::to_string(c & 255) + ";";
        out += "}\n";
        // Named styles, so other programs (and mep's reader) know each
        // paragraph's role.
        out += "{\\stylesheet{\\s0\\fs22 Normal;}";
        for (int l = 1; l <= 6; ++l) out += "{\\s" + std::to_string(l) + "\\outlinelevel" + std::to_string(l - 1) + "\\sbasedon0\\snext0 heading " + std::to_string(l) + ";}";
        out += "{\\s7\\sbasedon0 Title;}{\\s8\\sbasedon0 Source Code;}{\\s9\\sbasedon0 Quote;}{\\s10\\sbasedon0 caption;}";
        for (int l = 1; l <= 6; ++l) out += "{\\s" + std::to_string(10 + l) + "\\sbasedon0 toc " + std::to_string(l) + ";}";
        out += "{\\s17\\sbasedon0 Bibliography;}{\\s18\\sbasedon0 Math Display;}{\\s19\\sbasedon0 TOC Heading;}{\\s20\\sbasedon0 List Paragraph;}"
               "{\\s21\\sbasedon0 Abstract;}{\\s22\\sbasedon0 Abstract Title;}{\\s23\\sbasedon0 Box Title;}"
               "{\\*\\cs30 Verbatim Char;}{\\*\\cs31 Math;}";  // (bare: some readers apply a character style's look to paragraphs)
        for (size_t i = 0; i < classes.size(); ++i) out += "{\\*\\cs" + std::to_string(40 + i) + " mep class " + classes[i] + ";}";
        out += "}\n";
        if (!lists.empty()) {
            std::string table = "{\\*\\listtable", overrides = "{\\*\\listoverridetable";
            for (size_t i = 0; i < lists.size(); ++i) {
                const std::string id = std::to_string(i + 1);
                table += "{\\list\\listtemplateid" + id + "\\listhybrid";
                for (int l = 0; l < 9; ++l) {
                    const std::string ind = "\\fi-360\\li" + std::to_string(360 * (l + 2));
                    if (lists[i]) {
                        char lvl[8];
                        std::snprintf(lvl, sizeof lvl, "%02x", l);
                        table += std::string("{\\listlevel\\levelnfc0\\levelnfcn0\\leveljc0\\levelstartat1\\levelfollow0{\\leveltext\\'02\\'") + lvl +
                                 ".;}{\\levelnumbers\\'01;}" + ind + "}";
                    } else {
                        table += "{\\listlevel\\levelnfc23\\levelnfcn23\\leveljc0\\levelstartat1\\levelfollow0{\\leveltext\\'01" +
                                 std::string(l % 2 ? "\\u9702 ?" : "\\u8226 ?") + ";}{\\levelnumbers;}" + ind + "}";
                    }
                }
                table += "\\listid" + id + "}";
                overrides += "{\\listoverride\\listid" + id + "\\listoverridecount0\\ls" + id + "}";
            }
            out += table + "}\n" + overrides + "}\n";
        }
        if (!doc.title.empty()) out += "{\\info{\\title " + Esc(doc.title) + "}}\n";
        const auto props = OfficeProps(doc);
        if (!props.empty()) {
            out += "{\\*\\userprops";
            for (const auto &kv : props) out += "{\\propname " + Esc(kv.first) + "}\\proptype30{\\staticval " + Esc(kv.second) + "}\n";
            out += "}\n";
        }
        return out + body + "}\n";
    }
};

// ===========================================================================
// DOCX
// ===========================================================================

std::string XmlEsc(const std::string &s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\n': o += ' '; break;
            default: o += c;
        }
    }
    return o;
}

struct DocxRunFmt {
    bool b = false, i = false, u = false, strike = false, sup = false, sub = false, mono = false, mark = false;
    std::string color;  // RRGGBB
    std::string font;
    int half_points = 0;
    std::string style;  // rStyle
    bool keep_spaces = false;  // code: indentation is meaning, not layout
    SheetLook sheet;  // what the document's style sheets change of the run
};

struct DocxWriter {
    const Document &doc;
    std::string base_dir;
    std::vector<std::string> rels;  // extra relationship XML
    std::vector<zip::EntryToWrite> media;
    std::vector<std::string> footnotes;  // <w:footnote> XML
    std::vector<int> list_nums;          // numId -> abstractNum (0 bullet, 1 decimal)
    int next_rel = 10;
    int next_pic = 1;
    int code_n = 0, mark_n = 0;
    ExportTextStyler styler{doc};
    std::map<std::string, SheetLook> sheet_styles{};  // the "MepSheet-..." styles the body uses

    // A bookmark around nothing: a named point in the text.
    std::string Bookmark(const std::string &name) {
        const std::string id = std::to_string(++mark_n);
        return "<w:bookmarkStart w:id=\"" + id + "\" w:name=\"" + XmlEsc(name) + "\"/><w:bookmarkEnd w:id=\"" + id + "\"/>";
    }
    // TeX as an Office Math run: shown as-is by Word, read back as maths.
    static std::string OMath(const std::string &tex) {
        return "<m:oMath><m:r><m:t xml:space=\"preserve\">" + XmlEsc(tex) + "</m:t></m:r></m:oMath>";
    }

    std::string Run(const std::string &text, const DocxRunFmt &f) {
        if (text.empty()) return "";
        std::string pr;
        if (!f.style.empty()) {
            pr += "<w:rStyle w:val=\"" + f.style + "\"/>";
        } else if (f.sheet.any()) {
            // (A run has one character style: a class's or a link's comes first.)
            sheet_styles[f.sheet.Id('-')] = f.sheet;
            pr += "<w:rStyle w:val=\"" + f.sheet.Id('-') + "\"/>";
        }
        if (f.mono) pr += "<w:rFonts w:ascii=\"Courier New\" w:hAnsi=\"Courier New\" w:cs=\"Courier New\"/>";
        else if (!f.font.empty()) pr += "<w:rFonts w:ascii=\"" + XmlEsc(f.font) + "\" w:hAnsi=\"" + XmlEsc(f.font) + "\"/>";
        if (f.b) pr += "<w:b/>";
        if (f.i) pr += "<w:i/>";
        if (f.strike) pr += "<w:strike/>";
        if (!f.color.empty()) pr += "<w:color w:val=\"" + f.color + "\"/>";
        if (f.half_points > 0) pr += "<w:sz w:val=\"" + std::to_string(f.half_points) + "\"/>";
        if (f.mark) pr += "<w:highlight w:val=\"yellow\"/>";
        if (f.u) pr += "<w:u w:val=\"single\"/>";
        if (f.sup) pr += "<w:vertAlign w:val=\"superscript\"/>";
        if (f.sub) pr += "<w:vertAlign w:val=\"subscript\"/>";
        // A source line break is a space, and (as everywhere but code) runs
        // of spaces are one.
        std::string t;
        for (char c : text) {
            if (c == '\n' || c == '\r') c = ' ';
            if (c == ' ' && !t.empty() && t.back() == ' ' && !f.mono && !f.keep_spaces) continue;
            t += c;
        }
        return "<w:r>" + (pr.empty() ? "" : "<w:rPr>" + pr + "</w:rPr>") + "<w:t xml:space=\"preserve\">" + XmlEsc(t) + "</w:t></w:r>";
    }

    std::string Inl(const std::vector<Inline> &ins, const DocxRunFmt &f) {
        std::string o;
        for (const Inline &x : ins) o += Node(x, f);
        return o;
    }
    // What the document's style sheets change of a run (ExportTextStyler):
    // its colour, weight and slant.
    static void ApplySheet(const ExportTextStyler::Change &ch, DocxRunFmt *f) { f->sheet.Take(ch); }
    struct SheetScope {
        ExportTextStyler &styler;
        bool entered;
        ~SheetScope() {
            if (entered) styler.Leave();
        }
    };
    std::string Node(const Inline &x, DocxRunFmt f) {
        const bool styled = styler.active() && x.kind != InlineKind::Text;
        const ExportTextStyler::Change change = styled ? styler.Enter(x) : ExportTextStyler::Change();
        const SheetScope scope{styler, styled};
        switch (x.kind) {
            case InlineKind::Text: return Run(x.text, f);
            case InlineKind::Bold: f.b = true; break;
            case InlineKind::Italic: f.i = true; break;
            case InlineKind::Underline: f.u = true; break;
            case InlineKind::Superscript: f.sup = true; break;
            case InlineKind::Subscript: f.sub = true; break;
            case InlineKind::Small: f.half_points = 18; break;
            case InlineKind::Big: f.half_points = 30; break;
            case InlineKind::Mono: f.mono = true; break;
            case InlineKind::Highlight: f.mark = true; break;
            case InlineKind::Strike: f.strike = true; break;
            case InlineKind::Insert: f.u = true; f.color = "2E7D32"; break;
            case InlineKind::Delete: f.strike = true; f.color = "C62828"; break;
            case InlineKind::Verbatim:
                f.mono = true;
                f.style = "VerbatimChar";
                return Run(x.text, f);
            case InlineKind::Font: f.font = x.arg; break;
            case InlineKind::FontSize: f.half_points = static_cast<int>(std::atof(x.arg.c_str()) * 2.0); break;
            // A character style named for the class (Styles() defines one
            // per class, in the look the document's sheets give it).
            case InlineKind::Class:
                if (IsBoxKindName(x.arg)) f.style = "MepClass-" + x.arg;
                break;
            case InlineKind::Color: {
                std::uint32_t rgb = 0;
                if (ParseColor(x.arg, &rgb)) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "%06X", rgb);
                    f.color = buf;
                }
                break;
            }
            case InlineKind::Link: {
                const std::string id = "rId" + std::to_string(next_rel++);
                rels.push_back("<Relationship Id=\"" + id +
                               "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink\" Target=\"" +
                               XmlEsc(x.arg) + "\" TargetMode=\"External\"/>");
                f.style = "Hyperlink";
                return "<w:hyperlink r:id=\"" + id + "\">" + Inl(x.children, f) + "</w:hyperlink>";
            }
            case InlineKind::Footnote: {
                const int id = static_cast<int>(footnotes.size()) + 1;
                DocxRunFmt plain;
                footnotes.push_back("<w:footnote w:id=\"" + std::to_string(id) +
                                    "\"><w:p><w:pPr><w:pStyle w:val=\"FootnoteText\"/></w:pPr><w:r><w:rPr><w:rStyle w:val=\"FootnoteReference\"/></w:rPr><w:footnoteRef/></w:r>" +
                                    Run(" ", plain) + Inl(x.children, plain) + "</w:p></w:footnote>");
                return "<w:r><w:rPr><w:rStyle w:val=\"FootnoteReference\"/></w:rPr><w:footnoteReference w:id=\"" +
                       std::to_string(id) + "\"/></w:r>";
            }
            case InlineKind::Cite:
            case InlineKind::CiteP:
                // An internal link to the entry's bookmark; the tooltip says
                // which kind of citation it is.
                return "<w:hyperlink w:anchor=\"ref_" + XmlEsc(x.text) + "\" w:tooltip=\"" +
                       std::string(x.kind == InlineKind::CiteP ? "\\citep" : "\\cite") + "\">" +
                       Run(CiteLabel(doc, x.text, x.kind == InlineKind::CiteP), f) + "</w:hyperlink>";
            case InlineKind::Math: return OMath(x.text);
            case InlineKind::Comment: return "";
            case InlineKind::Raw: return x.text;  // WordprocessingML runs, as written
            case InlineKind::Command: return Run("\\" + x.arg + "(" + x.text + ")", f);
        }
        f.sheet.TakeFor(x, change, &f);  // (after the kind's own look: a sheet's rule for it wins)
        return Inl(x.children, f);
    }

    static std::string P(const std::string &ppr, const std::string &runs) {
        return "<w:p>" + (ppr.empty() ? "" : "<w:pPr>" + ppr + "</w:pPr>") + runs + "</w:p>";
    }
    static std::string Style(const std::string &s) { return "<w:pStyle w:val=\"" + s + "\"/>"; }

    std::string Picture(const std::string &path, const std::string &alt = "", bool decorative = false) {
        std::string bytes, ext;
        int w = 0, h = 0;
        // A picture that cannot be read is linked, not embedded: the
        // document still has the figure, and Word shows it if the file
        // turns up.
        const bool linked = !ReadBinary(ResolveRel(base_dir, path), &bytes) || !ImageSize(bytes, &w, &h, &ext) || w <= 0 || h <= 0;
        if (linked) {
            w = 384;
            h = 288;
        }
        const int n = next_pic++;
        const std::string name = linked ? path : "image" + std::to_string(n) + "." + (ext == "png" ? "png" : "jpeg");
        if (!linked) media.push_back({"word/media/" + name, bytes, true});
        const std::string id = "rId" + std::to_string(next_rel++);
        rels.push_back("<Relationship Id=\"" + id + "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"" +
                       (linked ? XmlEsc(path) + "\" TargetMode=\"External" : "media/" + name) + "\"/>");
        // 9525 EMU per pixel at 96 dpi, at most 6 inches wide.
        double cx = w * 9525.0, cy = h * 9525.0;
        const double max_cx = 6.0 * 914400.0;
        if (cx > max_cx) {
            cy *= max_cx / cx;
            cx = max_cx;
        }
        const std::string ext_xml = "cx=\"" + std::to_string(static_cast<long long>(cx)) + "\" cy=\"" + std::to_string(static_cast<long long>(cy)) + "\"";
        const std::string pid = std::to_string(n);
        return P("<w:jc w:val=\"center\"/>",
                 "<w:r><w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\"><wp:extent " + ext_xml +
                     "/><wp:docPr id=\"" + pid + "\" name=\"Picture " + pid + "\" descr=\"" + XmlEsc(alt) + "\"" +
                     // (Word's "mark as decorative": a picture a screen reader passes over.)
                     (decorative ? "><a:extLst xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\">"
                                   "<a:ext uri=\"{C183D7F6-B498-43B3-948B-1728B52AA6E4}\">"
                                   "<adec:decorative xmlns:adec=\"http://schemas.microsoft.com/office/drawing/2017/decorative\" val=\"1\"/>"
                                   "</a:ext></a:extLst></wp:docPr>"
                                 : "/>") +
                     "<a:graphic xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"><pic:pic xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"><pic:nvPicPr><pic:cNvPr id=\"" +
                     pid + "\" name=\"" + XmlEsc(name) + "\"/><pic:cNvPicPr/></pic:nvPicPr><pic:blipFill><a:blip " + (linked ? "r:link" : "r:embed") + "=\"" + id +
                     "\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill><pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext " + ext_xml +
                     "/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr></pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing></w:r>");
    }

    std::string Caption(const std::string &label, const std::vector<Inline> &cap) {
        if (cap.empty()) return "";
        DocxRunFmt f, bold;
        bold.b = true;
        return P(Style("Caption"), (label.empty() ? "" : Run(label + ": ", bold)) + Inl(cap, f));
    }

    std::string Body() {
        std::string body;
        DocxRunFmt none;
        if (!doc.title.empty()) body += P(Style("Title"), Run(doc.title, none));
        const std::vector<std::string> labels = BlockLabels(doc);
        const std::vector<bool> export_hidden = ExportHidden(doc);
        for (size_t bi = 0; bi < doc.blocks.size(); ++bi) {
            const Block &b = doc.blocks[bi];
            if (export_hidden[bi]) continue;
            // (The block's own text: what the document's sheets change of it.)
            none = DocxRunFmt();
            ApplySheet(styler.ForBlock(b), &none);
            bool show_code = true, show_results = true;
            if (b.kind == BlockKind::Code) CodeExports(doc, b, &show_code, &show_results);
            switch (b.kind) {
                case BlockKind::Paragraph: body += OnlyRaw(b.inlines) ? Inl(b.inlines, none) : P("", Inl(b.inlines, none)); break;
                case BlockKind::Heading:
                    body += P(Style("Heading" + std::to_string(std::min(6, std::max(1, b.level)))), Inl(b.inlines, none));
                    break;
                case BlockKind::MathBlock:
                    body += P("<w:jc w:val=\"center\"/>", "<m:oMathPara>" + OMath(TrimStr(b.code)) + "</m:oMathPara>") +
                            Caption(labels[bi], b.caption_inlines);
                    break;
                case BlockKind::Code: {
                    std::istringstream ss(b.code);
                    std::string l;
                    const size_t before = body.size();
                    DocxRunFmt code_fmt;
                    code_fmt.keep_spaces = true;
                    // (A blank line is an empty paragraph -- Word shows it as
                    // one -- not a space, which a reader would keep.)
                    if (show_code) {
                        std::string mark = Bookmark("mepml_code_" + std::to_string(++code_n));
                        while (std::getline(ss, l)) {
                            body += P(Style("SourceCode"), mark + (l.empty() ? std::string() : Run(l, code_fmt)));
                            mark.clear();
                        }
                        if (!mark.empty()) body += P(Style("SourceCode"), mark + Run(" ", none));
                    }
                    DocxRunFmt out;
                    out.color = "555555";
                    out.keep_spaces = true;
                    if (show_results) {
                        for (const std::string &rl : ResultTextLines(b)) {
                            std::string img;
                            if (!b.result_format.empty() || !ResultImagePath(rl, &img)) body += P(Style("SourceCode"), rl.empty() ? std::string() : Run(rl, out));
                        }
                        for (const auto &im : b.result_images) body += Picture(im.second, b.alt, Decorative(b));
                    }
                    if (body.size() > before) body += Caption(labels[bi], b.caption_inlines);
                    break;
                }
                case BlockKind::Image: body += Picture(b.value, b.alt, Decorative(b)) + Caption(labels[bi], b.caption_inlines); break;
                case BlockKind::Table: {
                    size_t cols = 0;
                    for (const auto &r : b.rows) cols = std::max(cols, r.size());
                    if (cols == 0) break;
                    if (!b.caption_inlines.empty()) body += Caption(labels[bi], b.caption_inlines);
                    // (Its \alttext is Word's table description: what a
                    // screen reader says the table shows.)
                    body += "<w:tbl><w:tblPr><w:tblStyle w:val=\"TableGrid\"/><w:tblW w:w=\"0\" w:type=\"auto\"/>" +
                            (b.alt.empty() ? std::string() : "<w:tblDescription w:val=\"" + XmlEsc(b.alt) + "\"/>") + "</w:tblPr><w:tblGrid>";
                    for (size_t c = 0; c < cols; ++c) body += "<w:gridCol w:w=\"" + std::to_string(9000 / cols) + "\"/>";
                    body += "</w:tblGrid>";
                    for (size_t r = 0; r < b.rows.size(); ++r) {
                        const bool head = static_cast<int>(r) < b.header_rows;
                        body += std::string("<w:tr>") + (head ? "<w:trPr><w:tblHeader/></w:trPr>" : "");
                        DocxRunFmt f;
                        f.b = head;
                        for (size_t c = 0; c < cols; ++c) {
                            const Align a = c < b.aligns.size() ? b.aligns[c] : Align::Default;
                            const std::string jc = a == Align::Center  ? "<w:jc w:val=\"center\"/>"
                                                   : a == Align::Right ? "<w:jc w:val=\"right\"/>"
                                                   : a == Align::Left  ? "<w:jc w:val=\"left\"/>"
                                                                       : "";
                            body += "<w:tc>" + P(jc, c < b.rows[r].size() ? Inl(b.rows[r][c].content, f) : "") + "</w:tc>";
                        }
                        body += "</w:tr>";
                    }
                    body += "</w:tbl>";
                    break;
                }
                case BlockKind::List: {
                    // One numbering instance per list (so each numbered list
                    // starts at its own first number).
                    // (and kind: bullets and numbers mixed in one list get one of each).
                    int num_ids[2] = {0, 0};
                    for (const ListItem &it : b.items) {
                        const int kind = it.ordered ? 1 : 0;
                        if (num_ids[kind] == 0) {
                            list_nums.push_back(kind);
                            num_ids[kind] = static_cast<int>(list_nums.size());
                        }
                        const int num_id = num_ids[kind];
                        const int lvl = std::min(8, it.indent / 2);
                        std::string lead;
                        if (it.checkbox >= 0) lead = it.checkbox ? "\u2612 " : "\u2610 ";
                        body += P(Style("ListParagraph") + "<w:numPr><w:ilvl w:val=\"" + std::to_string(lvl) + "\"/><w:numId w:val=\"" +
                                      std::to_string(num_id) + "\"/></w:numPr>",
                                  Run(lead, none) + Inl(it.content, none));
                    }
                    break;
                }
                case BlockKind::Rule:
                    body += P("<w:pBdr><w:bottom w:val=\"single\" w:sz=\"6\" w:space=\"1\" w:color=\"auto\"/></w:pBdr>", "");
                    break;
                case BlockKind::Bibliography: {
                    body += P(Style("Heading1"), Bookmark("mepml_bibliography") + Run("References", none));
                    int n = 0;
                    DocxRunFmt it;
                    it.i = true;
                    for (const std::string &key : doc.cite_order) {
                        const BibEntryParts e = BibEntry(doc.citations.at(key));
                        body += P(Style("Bibliography"), Bookmark("ref_" + key) + Run("[" + std::to_string(++n) + "]\t" + e.lead, none) +
                                                             Run(e.title, it) + Run(e.rest, none));
                    }
                    break;
                }
                case BlockKind::TableOfContents: {
                    body += P(Style("TOCHeading"), Run("Contents", none));
                    for (const Block &h : doc.blocks)
                        if (h.kind == BlockKind::Heading)
                            body += P(Style("TOC" + std::to_string(std::min(6, std::max(1, h.level)))), Run(InlinePlainText(h.inlines), none));
                    break;
                }
                case BlockKind::Abstract:
                    // pandoc's style names, so its reader (and mep's) knows the part.
                    body += P(Style("AbstractTitle"), Run("Abstract", none));
                    for (const std::vector<Inline> &para : AbstractParagraphs(b)) body += P(Style("Abstract"), Inl(para, none));
                    break;
                case BlockKind::BoxBegin: {
                    // A heading paragraph in the box's own style (Styles():
                    // bold, its colour, a rule down the left on its paper).
                    const BoxKind *k = FindBoxKind(b.keyword);
                    const std::string name = k ? k->name : "definition";
                    std::string heading = Run(ExportBoxLook(doc, b.keyword).label + (b.caption_inlines.empty() ? "" : ": "), none);
                    if (!b.caption_inlines.empty()) heading += Inl(b.caption_inlines, none);
                    body += P(Style("Box" + std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])))) + name.substr(1)), heading);
                    if (!b.inlines.empty()) body += P("", Inl(b.inlines, none));
                    break;
                }
                case BlockKind::BoxEnd: break;
                case BlockKind::Callout:  // (a comment: the author's own note)
                case BlockKind::Comment:
                case BlockKind::Meta:
                case BlockKind::Import:
                case BlockKind::Citation:
                // (A slide's content exports as ordinary blocks here; the
                // slide formats -- beamer, pptx, odp -- are their own.)
                case BlockKind::SlideBegin:
                case BlockKind::SlideEnd: break;
                case BlockKind::Define: break;
                case BlockKind::Raw: body += b.code; break;
                case BlockKind::Command: body += P("", Run(b.text, none)); break;
            }
        }
        return body;
    }

    static std::string Styles(const Document &doc, const std::map<std::string, SheetLook> &sheet_styles) {
        std::string s =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
            "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
            "<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii=\"Calibri\" w:hAnsi=\"Calibri\"/><w:sz w:val=\"22\"/>" +
            // (The document's language: a screen reader's voice, the spelling checker's dictionary.)
            (DocumentLanguage(doc).empty() ? std::string() : "<w:lang w:val=\"" + DocumentLanguage(doc) + "\"/>") +
            "</w:rPr></w:rPrDefault>"
            "<w:pPrDefault><w:pPr><w:spacing w:after=\"160\" w:line=\"264\" w:lineRule=\"auto\"/></w:pPr></w:pPrDefault></w:docDefaults>"
            "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/></w:style>"
            "<w:style w:type=\"paragraph\" w:styleId=\"Title\"><w:name w:val=\"Title\"/><w:basedOn w:val=\"Normal\"/><w:pPr><w:jc w:val=\"center\"/><w:spacing w:after=\"320\"/></w:pPr><w:rPr><w:b/><w:sz w:val=\"48\"/></w:rPr></w:style>";
        static const int kSize[] = {36, 30, 26, 24, 22, 22};
        for (int i = 1; i <= 6; ++i) {
            s += "<w:style w:type=\"paragraph\" w:styleId=\"Heading" + std::to_string(i) + "\"><w:name w:val=\"heading " + std::to_string(i) +
                 "\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:qFormat/><w:pPr><w:keepNext/><w:spacing w:before=\"240\" w:after=\"80\"/><w:outlineLvl w:val=\"" +
                 std::to_string(i - 1) + "\"/></w:pPr><w:rPr><w:b/><w:sz w:val=\"" + std::to_string(kSize[i - 1]) + "\"/></w:rPr></w:style>";
        }
        s += "<w:style w:type=\"paragraph\" w:styleId=\"SourceCode\"><w:name w:val=\"Source Code\"/><w:basedOn w:val=\"Normal\"/><w:pPr><w:spacing w:after=\"0\"/><w:ind w:left=\"360\"/><w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"F2F2F2\"/></w:pPr><w:rPr><w:rFonts w:ascii=\"Courier New\" w:hAnsi=\"Courier New\"/><w:sz w:val=\"19\"/></w:rPr></w:style>"
             "<w:style w:type=\"paragraph\" w:styleId=\"Caption\"><w:name w:val=\"caption\"/><w:basedOn w:val=\"Normal\"/><w:pPr><w:jc w:val=\"center\"/></w:pPr><w:rPr><w:i/><w:sz w:val=\"20\"/></w:rPr></w:style>"
             "<w:style w:type=\"paragraph\" w:styleId=\"Quote\"><w:name w:val=\"Quote\"/><w:basedOn w:val=\"Normal\"/><w:pPr><w:pBdr><w:left w:val=\"single\" w:sz=\"18\" w:space=\"8\" w:color=\"61AFEF\"/></w:pBdr><w:ind w:left=\"360\"/></w:pPr></w:style>"
             "<w:style w:type=\"paragraph\" w:styleId=\"ListParagraph\"><w:name w:val=\"List Paragraph\"/><w:basedOn w:val=\"Normal\"/><w:pPr><w:spacing w:after=\"40\"/></w:pPr></w:style>"
             "<w:style w:type=\"paragraph\" w:styleId=\"TOCHeading\"><w:name w:val=\"TOC Heading\"/><w:basedOn w:val=\"Heading1\"/></w:style>"
             "<w:style w:type=\"paragraph\" w:styleId=\"Bibliography\"><w:name w:val=\"Bibliography\"/><w:basedOn w:val=\"Normal\"/><w:pPr><w:ind w:left=\"567\" w:hanging=\"567\"/></w:pPr></w:style>"
             "<w:style w:type=\"paragraph\" w:styleId=\"AbstractTitle\"><w:name w:val=\"Abstract Title\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Abstract\"/><w:pPr><w:keepNext/><w:jc w:val=\"center\"/><w:spacing w:before=\"240\" w:after=\"80\"/></w:pPr><w:rPr><w:b/><w:sz w:val=\"22\"/></w:rPr></w:style>"
             "<w:style w:type=\"paragraph\" w:styleId=\"Abstract\"><w:name w:val=\"Abstract\"/><w:basedOn w:val=\"Normal\"/><w:pPr><w:ind w:left=\"720\" w:right=\"720\"/></w:pPr><w:rPr><w:sz w:val=\"20\"/></w:rPr></w:style>";
        // mepml's boxes: a heading per kind ("Definition Box"), bold in its
        // colour, ruled down the left on its paper.
        // What the document's style sheets change of the text: a character
        // style for each change the body uses.
        for (const auto &kv : sheet_styles) {
            const SheetLook &look = kv.second;
            s += "<w:style w:type=\"character\" w:customStyle=\"1\" w:styleId=\"" + kv.first + "\"><w:name w:val=\"" + kv.first + "\"/><w:rPr>" +
                 (look.bold == 1 ? "<w:b/>" : look.bold == 0 ? "<w:b w:val=\"0\"/>" : "") +
                 (look.italic == 1 ? "<w:i/>" : look.italic == 0 ? "<w:i w:val=\"0\"/>" : "") +
                 (look.color.empty() ? "" : "<w:color w:val=\"" + look.color + "\"/>") + "</w:rPr></w:style>";
        }
        // \class(name, ...): a character style each.
        for (const std::string &name : ClassNames(doc)) {
            const ClassLook look = ExportClassLook(doc, name);
            s += "<w:style w:type=\"character\" w:customStyle=\"1\" w:styleId=\"MepClass-" + name + "\"><w:name w:val=\"mep class " + name + "\"/><w:rPr>" +
                 (look.bold ? "<w:b/>" : "") + (look.italic ? "<w:i/>" : "") +
                 (look.color.empty() ? "" : "<w:color w:val=\"" + look.color.substr(1) + "\"/>") + "</w:rPr></w:style>";
        }
        for (const BoxKind &k : BoxKinds()) {
            const BoxLook look = ExportBoxLook(doc, k.name);  // (the style sheets' colours, where they give any)
            std::string id = k.name, colour = look.color.substr(1), tint = look.tint.substr(1);
            id[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(id[0])));
            s += "<w:style w:type=\"paragraph\" w:styleId=\"Box" + id + "\"><w:name w:val=\"" + k.label + " Box\"/><w:basedOn w:val=\"Normal\"/>"
                 "<w:pPr><w:keepNext/><w:pBdr><w:left w:val=\"single\" w:sz=\"24\" w:space=\"6\" w:color=\"" + colour + "\"/></w:pBdr>"
                 "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"" + tint + "\"/><w:spacing w:before=\"200\" w:after=\"80\"/><w:ind w:left=\"120\"/></w:pPr>"
                 "<w:rPr><w:b/><w:color w:val=\"" + colour + "\"/></w:rPr></w:style>";
        }
        for (int i = 1; i <= 6; ++i)
            s += "<w:style w:type=\"paragraph\" w:styleId=\"TOC" + std::to_string(i) + "\"><w:name w:val=\"toc " + std::to_string(i) +
                 "\"/><w:basedOn w:val=\"Normal\"/><w:pPr><w:spacing w:after=\"60\"/><w:ind w:left=\"" + std::to_string(240 * (i - 1)) + "\"/></w:pPr></w:style>";
        s +=
             "<w:style w:type=\"paragraph\" w:styleId=\"FootnoteText\"><w:name w:val=\"footnote text\"/><w:basedOn w:val=\"Normal\"/><w:rPr><w:sz w:val=\"18\"/></w:rPr></w:style>"
             "<w:style w:type=\"character\" w:styleId=\"FootnoteReference\"><w:name w:val=\"footnote reference\"/><w:rPr><w:vertAlign w:val=\"superscript\"/></w:rPr></w:style>"
             "<w:style w:type=\"character\" w:styleId=\"VerbatimChar\"><w:name w:val=\"Verbatim Char\"/><w:rPr><w:rFonts w:ascii=\"Courier New\" w:hAnsi=\"Courier New\"/></w:rPr></w:style>"
             "<w:style w:type=\"character\" w:styleId=\"Hyperlink\"><w:name w:val=\"Hyperlink\"/><w:rPr><w:color w:val=\"1A5FB4\"/><w:u w:val=\"single\"/></w:rPr></w:style>"
             "<w:style w:type=\"table\" w:styleId=\"TableGrid\"><w:name w:val=\"Table Grid\"/><w:tblPr><w:tblBorders>"
             "<w:top w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/><w:left w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/>"
             "<w:bottom w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/><w:right w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/>"
             "<w:insideH w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/><w:insideV w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/>"
             "</w:tblBorders></w:tblPr></w:style></w:styles>";
        return s;
    }

    std::string Numbering() const {
        std::string s =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
            "<w:numbering xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">";
        for (int a = 0; a < 2; ++a) {
            s += "<w:abstractNum w:abstractNumId=\"" + std::to_string(a) + "\"><w:multiLevelType w:val=\"hybridMultilevel\"/>";
            for (int l = 0; l < 9; ++l) {
                s += "<w:lvl w:ilvl=\"" + std::to_string(l) + "\"><w:start w:val=\"1\"/><w:numFmt w:val=\"" +
                     std::string(a == 0 ? "bullet" : "decimal") + "\"/><w:lvlText w:val=\"" +
                     (a == 0 ? std::string(l % 2 ? "\u25e6" : "\u2022") : "%" + std::to_string(l + 1) + ".") +
                     "\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"" + std::to_string(720 * (l + 1)) +
                     "\" w:hanging=\"360\"/></w:pPr></w:lvl>";
            }
            s += "</w:abstractNum>";
        }
        for (size_t i = 0; i < list_nums.size(); ++i)
            s += "<w:num w:numId=\"" + std::to_string(i + 1) + "\"><w:abstractNumId w:val=\"" + std::to_string(list_nums[i]) +
                 "\"/><w:lvlOverride w:ilvl=\"0\"><w:startOverride w:val=\"1\"/></w:lvlOverride></w:num>";
        return s + "</w:numbering>";
    }
};


// ===========================================================================
// ODT
// ===========================================================================

struct OdtFmt {
    bool b = false, i = false, u = false, strike = false, sup = false, sub = false, mono = false, mark = false;
    std::string color, font;
    double pt = 0.0;
    SheetLook sheet;  // what the document's style sheets change of the run (not part of Key)
    std::string Key() const {
        return std::string(b ? "b" : "") + (i ? "i" : "") + (u ? "u" : "") + (strike ? "s" : "") + (sup ? "^" : "") +
               (sub ? "_" : "") + (mono ? "m" : "") + (mark ? "h" : "") + "|" + color + "|" + font + "|" + std::to_string(pt);
    }
};

struct OdtWriter {
    const Document &doc;
    std::string base_dir;
    std::map<std::string, std::string> text_styles;  // OdtFmt::Key -> T<n>
    std::string auto_styles;
    std::vector<zip::EntryToWrite> pictures;
    int note_n = 0, pic_n = 0, code_n = 0;
    ExportTextStyler styler{doc};
    std::map<std::string, SheetLook> sheet_styles{};  // the "MepSheet_..." styles the body uses

    std::string StyleFor(const OdtFmt &f) {
        const std::string key = f.Key();
        auto it = text_styles.find(key);
        if (it != text_styles.end()) return it->second;
        const std::string name = "T" + std::to_string(text_styles.size() + 1);
        std::string props;
        if (f.b) props += " fo:font-weight=\"bold\"";
        if (f.i) props += " fo:font-style=\"italic\"";
        if (f.u) props += " style:text-underline-style=\"solid\" style:text-underline-width=\"auto\" style:text-underline-color=\"font-color\"";
        if (f.strike) props += " style:text-line-through-style=\"solid\"";
        if (f.sup) props += " style:text-position=\"super 58%\"";
        if (f.sub) props += " style:text-position=\"sub 58%\"";
        if (f.mono) props += " style:font-name=\"Liberation Mono\" fo:font-family=\"'Liberation Mono'\"";
        else if (!f.font.empty()) props += " fo:font-family=\"" + XmlEsc(f.font) + "\"";
        if (f.mark) props += " fo:background-color=\"#fff3a3\"";
        if (!f.color.empty()) props += " fo:color=\"#" + f.color + "\"";
        if (f.pt > 0) props += " fo:font-size=\"" + std::to_string(static_cast<int>(f.pt)) + "pt\"";
        auto_styles += "<style:style style:name=\"" + name + "\" style:family=\"text\"><style:text-properties" + props + "/></style:style>";
        text_styles[key] = name;
        return name;
    }

    std::string Run(const std::string &text, const OdtFmt &f) {
        if (text.empty()) return "";
        std::string t;
        // Runs of spaces and tabs need ODF's own elements.
        for (size_t k = 0; k < text.size(); ++k) {
            const char c = text[k];
            if (c == '\t') t += "<text:tab/>";
            else if (c == '\n' || c == '\r') t += ' ';  // ODF text is whitespace-collapsed anyway
            else if (c == ' ' && (k == 0 || (k + 1 < text.size() && text[k + 1] == ' '))) {
                // ODF drops leading spaces and folds runs of them: those
                // are written as <text:s/>.
                size_t n = 0;
                while (k < text.size() && text[k] == ' ') {
                    ++n;
                    ++k;
                }
                --k;
                t += "<text:s text:c=\"" + std::to_string(n) + "\"/>";
            } else t += XmlEsc(std::string(1, c));
        }
        if (f.Key() != OdtFmt().Key()) t = "<text:span text:style-name=\"" + StyleFor(f) + "\">" + t + "</text:span>";
        // The sheets' look round the run's own, as a named style.
        if (f.sheet.any()) {
            sheet_styles[f.sheet.Id('_')] = f.sheet;
            t = "<text:span text:style-name=\"" + f.sheet.Id('_') + "\">" + t + "</text:span>";
        }
        return t;
    }

    std::string Inl(const std::vector<Inline> &ins, const OdtFmt &f) {
        std::string o;
        for (const Inline &x : ins) o += Node(x, f);
        return o;
    }
    // What the document's style sheets change of a run (ExportTextStyler):
    // its colour, weight and slant.
    static void ApplySheet(const ExportTextStyler::Change &ch, OdtFmt *f) { f->sheet.Take(ch); }
    struct SheetScope {
        ExportTextStyler &styler;
        bool entered;
        ~SheetScope() {
            if (entered) styler.Leave();
        }
    };
    std::string Node(const Inline &x, OdtFmt f) {
        const bool styled = styler.active() && x.kind != InlineKind::Text;
        const ExportTextStyler::Change change = styled ? styler.Enter(x) : ExportTextStyler::Change();
        const SheetScope scope{styler, styled};
        switch (x.kind) {
            case InlineKind::Text: return Run(x.text, f);
            case InlineKind::Bold: f.b = true; break;
            case InlineKind::Italic: f.i = true; break;
            case InlineKind::Underline: f.u = true; break;
            case InlineKind::Superscript: f.sup = true; break;
            case InlineKind::Subscript: f.sub = true; break;
            case InlineKind::Small: f.pt = 9; break;
            case InlineKind::Big: f.pt = 15; break;
            case InlineKind::Mono: f.mono = true; break;
            case InlineKind::Highlight: f.mark = true; break;
            case InlineKind::Strike: f.strike = true; break;
            case InlineKind::Insert: f.u = true; f.color = "2e7d32"; break;
            case InlineKind::Delete: f.strike = true; f.color = "c62828"; break;
            case InlineKind::Verbatim: return "<text:span text:style-name=\"Source_20_Text\">" + Run(x.text, OdtFmt()) + "</text:span>";
            case InlineKind::Font: f.font = x.arg; break;
            case InlineKind::FontSize: f.pt = std::atof(x.arg.c_str()); break;
            // A text style named for the class (OdtStyles defines one per
            // class, in the look the document's sheets give it).
            case InlineKind::Class:
                if (!IsBoxKindName(x.arg)) break;
                return "<text:span text:style-name=\"MepClass_" + x.arg + "\">" + Inl(x.children, f) + "</text:span>";
            case InlineKind::Color: {
                std::uint32_t rgb = 0;
                if (ParseColor(x.arg, &rgb)) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "%06x", rgb);
                    f.color = buf;
                }
                break;
            }
            case InlineKind::Link:
                return "<text:a xlink:type=\"simple\" xlink:href=\"" + XmlEsc(x.arg) + "\">" + Inl(x.children, f) + "</text:a>";
            case InlineKind::Footnote: {
                const std::string n = std::to_string(++note_n);
                return "<text:note text:id=\"ftn" + n + "\" text:note-class=\"footnote\"><text:note-citation>" + n +
                       "</text:note-citation><text:note-body><text:p text:style-name=\"Footnote\">" + Inl(x.children, OdtFmt()) +
                       "</text:p></text:note-body></text:note>";
            }
            case InlineKind::Cite:
            case InlineKind::CiteP:
                // A link to the entry's bookmark; its title says which kind.
                return "<text:a xlink:type=\"simple\" xlink:href=\"#ref_" + XmlEsc(x.text) + "\" office:title=\"" +
                       std::string(x.kind == InlineKind::CiteP ? "\\citep" : "\\cite") + "\">" +
                       Run(CiteLabel(doc, x.text, x.kind == InlineKind::CiteP), f) + "</text:a>";
            case InlineKind::Math: return "<text:span text:style-name=\"Math\">" + Run(x.text, OdtFmt()) + "</text:span>";
            case InlineKind::Comment: return "";
            case InlineKind::Raw: return x.text;  // ODF inline XML, as written
            case InlineKind::Command: return Run("\\" + x.arg + "(" + x.text + ")", f);
        }
        f.sheet.TakeFor(x, change, &f);  // (after the kind's own look: a sheet's rule for it wins)
        return Inl(x.children, f);
    }

    static std::string P(const std::string &style, const std::string &body) {
        return "<text:p" + (style.empty() ? std::string() : " text:style-name=\"" + style + "\"") + ">" + body + "</text:p>";
    }

    std::string Picture(const std::string &path, const std::string &alt = "", bool decorative = false) {
        std::string bytes, ext;
        int w = 0, h = 0;
        // A picture that cannot be read is linked (ODF paths are relative
        // to the package, hence ../), not embedded.
        const bool linked = !ReadBinary(ResolveRel(base_dir, path), &bytes) || !ImageSize(bytes, &w, &h, &ext) || w <= 0 || h <= 0;
        if (linked) {
            w = 384;
            h = 288;
        }
        const bool relative = !path.empty() && path[0] != '/' && path.find("://") == std::string::npos;
        const std::string name =
            linked ? (relative ? "../" + path : path) : "Pictures/image" + std::to_string(pic_n + 1) + "." + (ext == "png" ? "png" : "jpg");
        ++pic_n;
        if (!linked) pictures.push_back({name, bytes, true});
        // 96 dpi, at most 16 cm wide.
        double wcm = w / 96.0 * 2.54, hcm = h / 96.0 * 2.54;
        if (wcm > 16.0) {
            hcm *= 16.0 / wcm;
            wcm = 16.0;
        }
        char size[96];
        std::snprintf(size, sizeof size, "svg:width=\"%.2fcm\" svg:height=\"%.2fcm\"", wcm, hcm);
        return P("Figure", "<draw:frame draw:name=\"image" + std::to_string(pic_n) + "\" text:anchor-type=\"as-char\" " + size +
                               (decorative ? " loext:decorative=\"true\"" : "") + "><draw:image xlink:href=\"" + XmlEsc(name) +
                               "\" xlink:type=\"simple\" xlink:show=\"embed\" xlink:actuate=\"onLoad\"/>" +
                               (alt.empty() ? "" : "<svg:desc>" + XmlEsc(alt) + "</svg:desc>") + "</draw:frame>");
    }
    std::string Caption(const std::string &label, const std::vector<Inline> &cap) {
        if (cap.empty()) return "";
        OdtFmt b;
        b.b = true;
        return P("Caption", (label.empty() ? "" : Run(label + ": ", b)) + Inl(cap, OdtFmt()));
    }

    std::string Body() {
        std::string body;
        OdtFmt none;
        if (!doc.title.empty()) body += P("Title", Run(doc.title, none));
        const std::vector<std::string> labels = BlockLabels(doc);
        const std::vector<bool> export_hidden = ExportHidden(doc);
        for (size_t bi = 0; bi < doc.blocks.size(); ++bi) {
            const Block &b = doc.blocks[bi];
            if (export_hidden[bi]) continue;
            // (The block's own text: what the document's sheets change of it.)
            none = OdtFmt();
            ApplySheet(styler.ForBlock(b), &none);
            bool show_code = true, show_results = true;
            if (b.kind == BlockKind::Code) CodeExports(doc, b, &show_code, &show_results);
            switch (b.kind) {
                case BlockKind::Paragraph: body += OnlyRaw(b.inlines) ? Inl(b.inlines, none) : P("", Inl(b.inlines, none)); break;
                case BlockKind::Heading: {
                    const int lvl = std::min(6, std::max(1, b.level));
                    body += "<text:h text:style-name=\"Heading_20_" + std::to_string(lvl) + "\" text:outline-level=\"" +
                            std::to_string(lvl) + "\">" + Inl(b.inlines, none) + "</text:h>";
                    break;
                }
                case BlockKind::MathBlock:
                    body += P("Math_20_Display", "<text:span text:style-name=\"Math\">" + Run(TrimStr(b.code), none) + "</text:span>") +
                            Caption(labels[bi], b.caption_inlines);
                    break;
                case BlockKind::Code: {
                    std::istringstream ss(b.code);
                    std::string l;
                    const size_t before = body.size();
                    if (show_code) {
                        std::string mark = "<text:bookmark text:name=\"mepml_code_" + std::to_string(++code_n) + "\"/>";
                        while (std::getline(ss, l)) {
                            body += P("Preformatted_20_Text", mark + Run(l, none));
                            mark.clear();
                        }
                        if (!mark.empty()) body += P("Preformatted_20_Text", mark);
                    }
                    OdtFmt out;
                    out.color = "555555";
                    if (show_results) {
                        for (const std::string &rl : ResultTextLines(b)) {
                            std::string img;
                            if (!b.result_format.empty() || !ResultImagePath(rl, &img)) body += P("Preformatted_20_Text", Run(rl, out));
                        }
                        for (const auto &im : b.result_images) body += Picture(im.second, b.alt, Decorative(b));
                    }
                    if (body.size() > before) body += Caption(labels[bi], b.caption_inlines);
                    break;
                }
                case BlockKind::Image: body += Picture(b.value, b.alt, Decorative(b)) + Caption(labels[bi], b.caption_inlines); break;
                case BlockKind::Table: {
                    size_t cols = 0;
                    for (const auto &r : b.rows) cols = std::max(cols, r.size());
                    if (cols == 0) break;
                    body += "<table:table table:name=\"Table" + std::to_string(bi) + "\" table:style-name=\"Grid\">";
                    if (!b.alt.empty()) body += "<table:desc>" + XmlEsc(b.alt) + "</table:desc>";
                    body += "<table:table-column table:number-columns-repeated=\"" + std::to_string(cols) + "\"/>";
                    for (size_t r = 0; r < b.rows.size(); ++r) {
                        const bool head = static_cast<int>(r) < b.header_rows;
                        if (head && r == 0) body += "<table:table-header-rows>";
                        body += "<table:table-row>";
                        OdtFmt f;
                        f.b = head;
                        for (size_t c = 0; c < cols; ++c) {
                            const Align a = c < b.aligns.size() ? b.aligns[c] : Align::Default;
                            const std::string ps = a == Align::Center  ? "TableCenter"
                                                  : a == Align::Right ? "TableRight"
                                                  : a == Align::Left  ? "TableLeft"
                                                                      : "TableContents";
                            body += "<table:table-cell table:style-name=\"Cell\" office:value-type=\"string\">" +
                                    P(ps, c < b.rows[r].size() ? Inl(b.rows[r][c].content, f) : "") + "</table:table-cell>";
                        }
                        body += "</table:table-row>";
                        if (head && static_cast<int>(r) + 1 == b.header_rows) body += "</table:table-header-rows>";
                    }
                    body += "</table:table>" + Caption(labels[bi], b.caption_inlines);
                    break;
                }
                case BlockKind::List: {
                    // Nested <text:list>s by indent.
                    const bool ordered = !b.items.empty() && b.items[0].ordered;
                    std::vector<int> depth;
                    auto open = [](bool numbered) { return "<text:list text:style-name=\"" + std::string(numbered ? "LNum" : "LBullet") + "\">"; };
                    body += open(ordered);
                    bool top_ordered = ordered;
                    depth.push_back(b.items.empty() ? 0 : b.items[0].indent);
                    bool open_item = false;
                    for (const ListItem &it : b.items) {
                        while (depth.size() > 1 && it.indent < depth.back()) {
                            body += "</text:list-item></text:list>";
                            depth.pop_back();
                        }
                        // Bullets turning into numbers (or back) at the top: a new list.
                        if (depth.size() == 1 && open_item && it.indent <= depth.back() && it.ordered != top_ordered) {
                            body += "</text:list-item></text:list>" + open(it.ordered);
                            top_ordered = it.ordered;
                            open_item = false;
                        }
                        if (it.indent > depth.back() && open_item) {
                            body += open(it.ordered);
                            depth.push_back(it.indent);
                            open_item = false;
                        } else if (open_item) {
                            body += "</text:list-item>";
                        }
                        std::string lead;
                        if (it.checkbox >= 0) lead = it.checkbox ? "☒ " : "☐ ";
                        body += "<text:list-item>" + P("ListText", Run(lead, none) + Inl(it.content, none));
                        open_item = true;
                    }
                    for (size_t d = 0; d < depth.size(); ++d) body += (open_item || d ? "</text:list-item>" : "") + std::string("</text:list>");
                    break;
                }
                case BlockKind::Rule: body += P("Rule", ""); break;
                case BlockKind::Bibliography: {
                    body += "<text:h text:style-name=\"Heading_20_1\" text:outline-level=\"1\"><text:bookmark text:name=\"mepml_bibliography\"/>References</text:h>";
                    int n = 0;
                    OdtFmt it;
                    it.i = true;
                    for (const std::string &key : doc.cite_order) {
                        const BibEntryParts e = BibEntry(doc.citations.at(key));
                        body += P("Bibliography", "<text:bookmark text:name=\"ref_" + XmlEsc(key) + "\"/>" + Run("[" + std::to_string(++n) + "] " + e.lead, none) + Run(e.title, it) + Run(e.rest, none));
                    }
                    break;
                }
                case BlockKind::TableOfContents: {
                    body += "<text:h text:style-name=\"Contents_20_Heading\" text:outline-level=\"1\">Contents</text:h>";
                    for (const Block &h : doc.blocks)
                        if (h.kind == BlockKind::Heading)
                            body += P("Contents_20_" + std::to_string(std::min(6, h.level)), Run(InlinePlainText(h.inlines), none));
                    break;
                }
                case BlockKind::Abstract:
                    body += P("Abstract_20_Title", Run("Abstract", none));
                    for (const std::vector<Inline> &para : AbstractParagraphs(b)) body += P("Abstract", Inl(para, none));
                    break;
                case BlockKind::BoxBegin: {
                    // A heading paragraph in the box's own style (see the
                    // styles: bold, its colour, ruled down the left).
                    const BoxKind *k = FindBoxKind(b.keyword);
                    std::string heading = Run(ExportBoxLook(doc, b.keyword).label + (b.caption_inlines.empty() ? "" : ": "), none);
                    if (!b.caption_inlines.empty()) heading += Inl(b.caption_inlines, none);
                    body += P("Box_20_" + std::string(k ? k->label : "Definition"), heading);
                    if (!b.inlines.empty()) body += P("", Inl(b.inlines, none));
                    break;
                }
                case BlockKind::BoxEnd: break;
                case BlockKind::Callout:  // (a comment: the author's own note)
                case BlockKind::Comment:
                case BlockKind::Meta:
                case BlockKind::Import:
                case BlockKind::Citation:
                // (A slide's content exports as ordinary blocks here; the
                // slide formats -- beamer, pptx, odp -- are their own.)
                case BlockKind::SlideBegin:
                case BlockKind::SlideEnd: break;
                case BlockKind::Define: break;
                case BlockKind::Raw: body += b.code; break;
                case BlockKind::Command: body += P("", Run(b.text, none)); break;
            }
        }
        return body;
    }
};

const char *kOdtNs =
    "xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" "
    "xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\" "
    "xmlns:draw=\"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0\" xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\" "
    "xmlns:xlink=\"http://www.w3.org/1999/xlink\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
    "xmlns:meta=\"urn:oasis:names:tc:opendocument:xmlns:meta:1.0\" xmlns:svg=\"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0\" "
    "xmlns:loext=\"urn:org:documentfoundation:names:experimental:office:xmlns:loext:1.0\" "
    "office:version=\"1.3\"";

std::string OdtStyles(const Document &doc, const std::map<std::string, SheetLook> &sheet_styles) {
    std::string s = std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-styles ") + kOdtNs +
                    "><office:font-face-decls><style:font-face style:name=\"Liberation Mono\" svg:font-family=\"'Liberation Mono'\" style:font-pitch=\"fixed\"/></office:font-face-decls><office:styles>"
                    "<style:default-style style:family=\"paragraph\"><style:paragraph-properties fo:margin-bottom=\"0.2cm\"/><style:text-properties fo:font-size=\"11pt\"/></style:default-style>"
                    "<style:style style:name=\"Standard\" style:family=\"paragraph\" style:class=\"text\"/>"
                    "<style:style style:name=\"Title\" style:family=\"paragraph\" style:parent-style-name=\"Standard\"><style:paragraph-properties fo:text-align=\"center\" fo:margin-bottom=\"0.5cm\"/><style:text-properties fo:font-size=\"24pt\" fo:font-weight=\"bold\"/></style:style>";
    static const int kPt[] = {18, 15, 13, 12, 11, 11};
    for (int i = 1; i <= 6; ++i)
        s += "<style:style style:name=\"Heading_20_" + std::to_string(i) + "\" style:display-name=\"Heading " + std::to_string(i) +
             "\" style:family=\"paragraph\" style:parent-style-name=\"Standard\" style:default-outline-level=\"" + std::to_string(i) +
             "\" style:class=\"text\"><style:paragraph-properties fo:margin-top=\"0.4cm\" fo:margin-bottom=\"0.2cm\" fo:keep-with-next=\"always\"/><style:text-properties fo:font-size=\"" +
             std::to_string(kPt[i - 1]) + "pt\" fo:font-weight=\"bold\"/></style:style>";
    s += "<style:style style:name=\"Source_20_Text\" style:display-name=\"Source Text\" style:family=\"text\"><style:text-properties style:font-name=\"Liberation Mono\" fo:font-family=\"'Liberation Mono'\"/></style:style>";
    s += "<style:style style:name=\"Math\" style:family=\"text\"><style:text-properties fo:font-style=\"italic\"/></style:style>"
         "<style:style style:name=\"Math_20_Display\" style:display-name=\"Math Display\" style:family=\"paragraph\" style:parent-style-name=\"Standard\"><style:paragraph-properties fo:text-align=\"center\"/></style:style>";
    s += "<style:style style:name=\"Contents_20_Heading\" style:display-name=\"Contents Heading\" style:family=\"paragraph\" style:parent-style-name=\"Heading_20_1\"/>";
    for (int i = 1; i <= 6; ++i)
        s += "<style:style style:name=\"Contents_20_" + std::to_string(i) + "\" style:display-name=\"Contents " + std::to_string(i) +
             "\" style:family=\"paragraph\" style:parent-style-name=\"Standard\"><style:paragraph-properties fo:margin-left=\"" +
             std::to_string(i - 1) + "cm\" fo:margin-bottom=\"0.05cm\"/></style:style>";
    s += "<style:style style:name=\"Preformatted_20_Text\" style:display-name=\"Preformatted Text\" style:family=\"paragraph\" style:parent-style-name=\"Standard\"><style:paragraph-properties fo:margin-bottom=\"0cm\" fo:margin-left=\"0.6cm\" fo:background-color=\"#f2f2f2\"/><style:text-properties style:font-name=\"Liberation Mono\" fo:font-family=\"'Liberation Mono'\" fo:font-size=\"9.5pt\"/></style:style>"
         "<style:style style:name=\"Quotations\" style:family=\"paragraph\" style:parent-style-name=\"Standard\"><style:paragraph-properties fo:margin-left=\"0.6cm\" fo:border-left=\"0.1cm solid #61afef\" fo:padding-left=\"0.3cm\"/></style:style>"
         "<style:style style:name=\"Caption\" style:family=\"paragraph\" style:parent-style-name=\"Standard\"><style:paragraph-properties fo:text-align=\"center\"/><style:text-properties fo:font-style=\"italic\" fo:font-size=\"10pt\"/></style:style>"
         "<style:style style:name=\"Figure\" style:family=\"paragraph\" style:parent-style-name=\"Standard\"><style:paragraph-properties fo:text-align=\"center\"/></style:style>"
         "<style:style style:name=\"ListText\" style:family=\"paragraph\" style:parent-style-name=\"Standard\"><style:paragraph-properties fo:margin-bottom=\"0.05cm\"/></style:style>"
         "<style:style style:name=\"TableContents\" style:display-name=\"Table Contents\" style:family=\"paragraph\" style:parent-style-name=\"Standard\"/>"
         "<style:style style:name=\"TableLeft\" style:family=\"paragraph\" style:parent-style-name=\"TableContents\"><style:paragraph-properties fo:text-align=\"start\"/></style:style>"
         "<style:style style:name=\"TableCenter\" style:family=\"paragraph\" style:parent-style-name=\"TableContents\"><style:paragraph-properties fo:text-align=\"center\"/></style:style>"
         "<style:style style:name=\"TableRight\" style:family=\"paragraph\" style:parent-style-name=\"TableContents\"><style:paragraph-properties fo:text-align=\"end\"/></style:style>"
         "<style:style style:name=\"Footnote\" style:family=\"paragraph\" style:parent-style-name=\"Standard\"><style:text-properties fo:font-size=\"9pt\"/></style:style>"
         "<style:style style:name=\"Bibliography\" style:family=\"paragraph\" style:parent-style-name=\"Standard\"><style:paragraph-properties fo:margin-left=\"0.8cm\" fo:text-indent=\"-0.8cm\"/></style:style>"
         "<style:style style:name=\"Rule\" style:family=\"paragraph\" style:parent-style-name=\"Standard\"><style:paragraph-properties fo:border-bottom=\"0.05pt solid #000000\"/></style:style>"
         "<style:style style:name=\"Abstract_20_Title\" style:display-name=\"Abstract Title\" style:family=\"paragraph\" style:parent-style-name=\"Standard\"><style:paragraph-properties fo:text-align=\"center\" fo:margin-top=\"0.4cm\" fo:keep-with-next=\"always\"/><style:text-properties fo:font-weight=\"bold\"/></style:style>"
         "<style:style style:name=\"Abstract\" style:family=\"paragraph\" style:parent-style-name=\"Standard\"><style:paragraph-properties fo:margin-left=\"1.25cm\" fo:margin-right=\"1.25cm\"/><style:text-properties fo:font-size=\"10pt\"/></style:style>";
    // mepml's boxes: a heading per kind, bold in its colour, ruled down
    // the left on its paper.
    // What the document's style sheets change of the text: a text style
    // for each change the body uses.
    for (const auto &kv : sheet_styles) {
        const SheetLook &look = kv.second;
        s += "<style:style style:name=\"" + kv.first + "\" style:family=\"text\"><style:text-properties" +
             (look.bold >= 0 ? std::string(" fo:font-weight=\"") + (look.bold ? "bold" : "normal") + "\"" : "") +
             (look.italic >= 0 ? std::string(" fo:font-style=\"") + (look.italic ? "italic" : "normal") + "\"" : "") +
             (look.color.empty() ? "" : " fo:color=\"#" + look.color + "\"") + "/></style:style>";
    }
    // \class(name, ...): a text style each.
    for (const std::string &name : ClassNames(doc)) {
        const ClassLook look = ExportClassLook(doc, name);
        s += "<style:style style:name=\"MepClass_" + name + "\" style:display-name=\"mep class " + name + "\" style:family=\"text\"><style:text-properties" +
             (look.bold ? " fo:font-weight=\"bold\"" : "") + (look.italic ? " fo:font-style=\"italic\"" : "") +
             (look.color.empty() ? "" : " fo:color=\"" + look.color + "\"") + "/></style:style>";
    }
    for (const BoxKind &k : BoxKinds()) {
        // (The style keeps the kind's own name -- the importer reads boxes
        // back by it -- whatever a sheet calls the kind; its colours are
        // the sheets' where they give any.)
        const BoxLook look = ExportBoxLook(doc, k.name);
        s += "<style:style style:name=\"Box_20_" + std::string(k.label) + "\" style:display-name=\"" + k.label +
             " Box\" style:family=\"paragraph\" style:parent-style-name=\"Standard\"><style:paragraph-properties fo:margin-top=\"0.35cm\" "
             "fo:keep-with-next=\"always\" fo:border-left=\"0.1cm solid " + look.color + "\" fo:padding-left=\"0.25cm\" fo:background-color=\"" + look.tint +
             "\"/><style:text-properties fo:font-weight=\"bold\" fo:color=\"" + look.color + "\"/></style:style>";
    }
    s += "<text:list-style style:name=\"LBullet\">";
    for (int l = 1; l <= 10; ++l)
        s += "<text:list-level-style-bullet text:level=\"" + std::to_string(l) + "\" text:bullet-char=\"" + (l % 2 ? "•" : "◦") +
             "\"><style:list-level-properties text:list-level-position-and-space-mode=\"label-alignment\"><style:list-level-label-alignment text:label-followed-by=\"listtab\" fo:text-indent=\"-0.5cm\" fo:margin-left=\"" +
             std::to_string(l * 0.8).substr(0, 4) + "cm\"/></style:list-level-properties></text:list-level-style-bullet>";
    s += "</text:list-style><text:list-style style:name=\"LNum\">";
    for (int l = 1; l <= 10; ++l)
        s += "<text:list-level-style-number text:level=\"" + std::to_string(l) +
             "\" style:num-suffix=\".\" style:num-format=\"1\"><style:list-level-properties text:list-level-position-and-space-mode=\"label-alignment\"><style:list-level-label-alignment text:label-followed-by=\"listtab\" fo:text-indent=\"-0.5cm\" fo:margin-left=\"" +
             std::to_string(l * 0.8).substr(0, 4) + "cm\"/></style:list-level-properties></text:list-level-style-number>";
    s += "</text:list-style></office:styles></office:document-styles>";
    return s;
}
}  // namespace

std::string ToMarkdown(const Document &doc) { return MdWriter{doc, {}}.Write(); }
std::string ToOrg(const Document &doc) { return OrgWriter{doc, {}}.Write(); }
std::string ToPlainText(const Document &doc) { return TextWriter{doc, {}}.Write(); }
std::string ToRtf(const Document &doc, const std::string &base_dir) {
    RtfWriter w{doc, base_dir};
    return w.Write();
}

namespace {
std::string BeamerDeck(const Document &doc, const std::string &base_dir) {
    HtmlOptions opts;
    opts.standalone = false;
    std::vector<BeamerFrame> frames;
    for (const SlideHtml &f : SlideFragments(doc, opts)) frames.push_back({f.title, f.body});
    return ExportHtmlSlidesToBeamer(frames, doc.title, MetaValue(doc, "subtitle"), MetaValue(doc, "author"), MetaValue(doc, "date"), base_dir,
                                    DocumentLanguage(doc));
}
}  // namespace

std::string ToBeamer(const Document &doc, const std::string &base_dir, std::string *error) {
    if (Slides(doc, 0).empty()) {
        if (error) *error = "no \\slide in the document: a Beamer deck is made of its \\slide blocks";
        return "";
    }
    return BeamerDeck(doc, base_dir);
}

std::string ToLatex(const Document &doc, const std::string &base_dir) {
    if (IsPresentation(doc)) return BeamerDeck(doc, base_dir);
    HtmlOptions opts;
    opts.standalone = false;
    return ExportHtmlToLatex(ToHtml(doc, opts), doc.title, MetaValue(doc, "author"), base_dir, DocumentLanguage(doc));
}

std::string ToHtmlFor(const Document &doc) { return IsPresentation(doc) ? ToSlidesHtml(doc) : ToHtml(doc); }

bool WriteOdt(const Document &doc, const std::string &path, const std::string &base_dir, std::string *error) {
    OdtWriter w{doc, base_dir, {}, {}, {}, 0, 0};
    const std::string body = w.Body();
    const std::string content = std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-content ") + kOdtNs +
                                "><office:automatic-styles>"
                                "<style:style style:name=\"Grid\" style:family=\"table\"><style:table-properties table:border-model=\"collapsing\"/></style:style>"
                                "<style:style style:name=\"Cell\" style:family=\"table-cell\"><style:table-cell-properties fo:padding=\"0.1cm\" fo:border=\"0.5pt solid #000000\"/></style:style>" +
                                w.auto_styles + "</office:automatic-styles><office:body><office:text>" + body +
                                "</office:text></office:body></office:document-content>";
    std::string author;
    for (const auto &kv : doc.meta)
        if (LowerStr(kv.first) == "author") author = kv.second;
    const std::string meta = std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-meta ") + kOdtNs +
                             "><office:meta><meta:generator>mep</meta:generator><dc:title>" + XmlEsc(doc.title) + "</dc:title>" +
                             (DocumentLanguage(doc).empty() ? "" : "<dc:language>" + DocumentLanguage(doc) + "</dc:language>") +
                             (author.empty() ? "" : "<dc:creator>" + XmlEsc(author) + "</dc:creator>") + [&] {
                                 std::string u;
                                 for (const auto &kv : OfficeProps(doc))
                                     u += "<meta:user-defined meta:name=\"" + XmlEsc(kv.first) + "\">" + XmlEsc(kv.second) + "</meta:user-defined>";
                                 return u;
                             }() + "</office:meta></office:document-meta>";
    std::string manifest =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.3\">"
        "<manifest:file-entry manifest:full-path=\"/\" manifest:media-type=\"application/vnd.oasis.opendocument.text\" manifest:version=\"1.3\"/>"
        "<manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>"
        "<manifest:file-entry manifest:full-path=\"styles.xml\" manifest:media-type=\"text/xml\"/>"
        "<manifest:file-entry manifest:full-path=\"meta.xml\" manifest:media-type=\"text/xml\"/>";
    for (const auto &p : w.pictures)
        manifest += "<manifest:file-entry manifest:full-path=\"" + p.name + "\" manifest:media-type=\"" +
                    (p.name.size() > 4 && p.name.compare(p.name.size() - 4, 4, ".png") == 0 ? "image/png" : "image/jpeg") + "\"/>";
    manifest += "</manifest:manifest>";
    // The mimetype entry first and stored, as ODF requires.
    std::vector<zip::EntryToWrite> entries = {{"mimetype", "application/vnd.oasis.opendocument.text", true},
                                              {"META-INF/manifest.xml", manifest, false},
                                              {"content.xml", content, false},
                                              {"styles.xml", OdtStyles(doc, w.sheet_styles), false},
                                              {"meta.xml", meta, false}};
    for (auto &p : w.pictures) entries.push_back(p);
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    const std::string zip = zip::BuildArchive(entries);
    f.write(zip.data(), static_cast<std::streamsize>(zip.size()));
    return static_cast<bool>(f);
}

bool WriteDocx(const Document &doc, const std::string &path, const std::string &base_dir, std::string *error) {
    DocxWriter w{doc, base_dir, {}, {}, {}, {}, 10, 1};
    const std::string body = w.Body();
    const std::string ns =
        "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
        "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\" "
        "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
        "xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\" "
        "xmlns:m=\"http://schemas.openxmlformats.org/officeDocument/2006/math\"";
    const std::string document = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:document " + ns + "><w:body>" + body +
                                 "<w:sectPr><w:pgSz w:w=\"11906\" w:h=\"16838\"/><w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\" w:header=\"708\" w:footer=\"708\" w:gutter=\"0\"/></w:sectPr></w:body></w:document>";
    std::string footnotes = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:footnotes " + ns +
                            "><w:footnote w:type=\"separator\" w:id=\"-1\"><w:p><w:r><w:separator/></w:r></w:p></w:footnote>"
                            "<w:footnote w:type=\"continuationSeparator\" w:id=\"0\"><w:p><w:r><w:continuationSeparator/></w:r></w:p></w:footnote>";
    for (const std::string &f : w.footnotes) footnotes += f;
    footnotes += "</w:footnotes>";
    std::string doc_rels =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering\" Target=\"numbering.xml\"/>"
        "<Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/footnotes\" Target=\"footnotes.xml\"/>"
        "<Relationship Id=\"rId4\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/settings\" Target=\"settings.xml\"/>";
    for (const std::string &r : w.rels) doc_rels += r;
    doc_rels += "</Relationships>";
    std::string core = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>" +
                       XmlEsc(doc.title) + "</dc:title><dc:creator>mep</dc:creator></cp:coreProperties>";
    std::string custom =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/custom-properties\" "
        "xmlns:vt=\"http://schemas.openxmlformats.org/officeDocument/2006/docPropsVTypes\">";
    int pid = 2;
    for (const auto &kv : OfficeProps(doc))
        custom += "<property fmtid=\"{D5CDD505-2E9C-101B-9397-08002B2CF9AE}\" pid=\"" + std::to_string(pid++) + "\" name=\"" + XmlEsc(kv.first) +
                  "\"><vt:lpwstr>" + XmlEsc(kv.second) + "</vt:lpwstr></property>";
    custom += "</Properties>";
    std::vector<zip::EntryToWrite> entries = {
        {"[Content_Types].xml",
         "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
         "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
         "<Default Extension=\"xml\" ContentType=\"application/xml\"/><Default Extension=\"png\" ContentType=\"image/png\"/>"
         "<Default Extension=\"jpeg\" ContentType=\"image/jpeg\"/>"
         "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
         "<Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
         "<Override PartName=\"/word/numbering.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml\"/>"
         "<Override PartName=\"/word/footnotes.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.footnotes+xml\"/>"
         "<Override PartName=\"/word/settings.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.settings+xml\"/>"
         "<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/>"
         "<Override PartName=\"/docProps/custom.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.custom-properties+xml\"/>"
         "</Types>",
         false},
        {"_rels/.rels",
         "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
         "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
         "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" Target=\"docProps/core.xml\"/>"
         "<Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/custom-properties\" Target=\"docProps/custom.xml\"/>"
         "</Relationships>",
         false},
        {"docProps/core.xml", core, false},
        {"docProps/custom.xml", custom, false},
        {"word/document.xml", document, false},
        {"word/_rels/document.xml.rels", doc_rels, false},
        {"word/styles.xml", DocxWriter::Styles(doc, w.sheet_styles), false},
        {"word/numbering.xml", w.Numbering(), false},
        {"word/footnotes.xml", footnotes, false},
        {"word/settings.xml",
         "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:settings xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
         "<w:footnotePr><w:footnote w:id=\"-1\"/><w:footnote w:id=\"0\"/></w:footnotePr></w:settings>",
         false},
    };
    for (auto &m : w.media) entries.push_back(m);
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    const std::string zip = zip::BuildArchive(entries);
    f.write(zip.data(), static_cast<std::streamsize>(zip.size()));
    return static_cast<bool>(f);
}

bool ExportFile(const Document &doc, const std::string &path, const std::string &base_dir, std::string *error) {
    const Format f = FormatFromPath(path);
    std::string text;
    switch (f) {
        case Format::Docx: return WriteDocx(doc, path, base_dir, error);
        case Format::Odt: return WriteOdt(doc, path, base_dir, error);
        case Format::Pptx: return WritePptx(doc, path, base_dir, error);
        case Format::Odp: return WriteOdp(doc, path, base_dir, error);
        case Format::Html: text = ToHtmlFor(doc); break;
        case Format::Markdown: text = ToMarkdown(doc); break;
        case Format::Org: text = ToOrg(doc); break;
        case Format::Rtf: text = ToRtf(doc, base_dir); break;
        case Format::Latex: text = ToLatex(doc, base_dir); break;
        case Format::Text: text = ToPlainText(doc); break;
        default:
            if (error) *error = "cannot export to " + path + " (PDF goes through LaTeX; see mep-mepml)";
            return false;
    }
    std::ofstream o(path, std::ios::binary);
    if (!o) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    o << text;
    return static_cast<bool>(o);
}

}  // namespace mepml
