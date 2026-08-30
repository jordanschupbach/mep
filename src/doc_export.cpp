#include "doc_export.h"
#include "html_doc.h"
#include "image_doc.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <vector>

#include "miniz.h"
#include "pugixml.hpp"

namespace {

// --- shared DOM-walk helpers -----------------------------------------------

std::string LowerExt(const std::string &path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot + 1);
    for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

// Same resolution rule as main.cpp's own ResolveHtmlImagePath (the
// in-pane browser's <img> loader) -- kept as a separate copy rather than
// a shared header function since that one lives in main.cpp (raylib-
// linked) and this file deliberately isn't.
std::string ResolveLocalPath(const std::string &src, const std::string &base_dir) {
    if (src.empty()) return "";
    if (src.compare(0, 7, "http://") == 0 || src.compare(0, 8, "https://") == 0) return "";
    std::string s = src;
    if (s.compare(0, 5, "file:") == 0) s = s.substr(5);
    if (!s.empty() && s[0] == '/') return s;
    if (base_dir.empty()) return s;
    return base_dir + "/" + s;
}

bool ReadFileBytes(const std::string &path, std::string &out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

std::string CollectRawText(const DomNode *node) {
    std::string out;
    for (auto &c : node->children) {
        if (c->type == DomNodeType::Text) out += c->text;
        else out += CollectRawText(c.get());
    }
    return out;
}

bool IsHeadingTag(const std::string &tag, int &level) {
    if (tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6') {
        level = tag[1] - '0';
        return true;
    }
    return false;
}

bool IsMathDisplay(const DomNode *node) {
    auto it = node->attrs.find("display");
    return it != node->attrs.end() && it->second == "1";
}

// table/tr/td/th may have thead/tbody/tfoot wrappers in between (both
// backends' own table walkers need the flat row list either way) --
// collected once here rather than duplicated per backend.
void CollectTableRows(const DomNode *node, std::vector<const DomNode *> &rows) {
    for (auto &c : node->children) {
        if (c->tag == "tr") rows.push_back(c.get());
        else if (c->type == DomNodeType::Element)
            CollectTableRows(c.get(), rows);
    }
}

size_t TableMaxCols(const std::vector<const DomNode *> &rows) {
    size_t max_cols = 0;
    for (const DomNode *r : rows) {
        size_t n = 0;
        for (auto &c : r->children) {
            if (c->tag == "td" || c->tag == "th") n++;
        }
        max_cols = std::max(max_cols, n);
    }
    return max_cols;
}

// Unwraps a full <html>[<head>...]<body>...</body></html> document down
// to its <body> (whose own children are the real content root); a bare
// fragment -- what kBuiltinOrgExport's mep.org_export('html') actually
// hands both backends below -- has neither, so this is a no-op for the
// expected input shape. Purely defensive: nothing here relies on it, but
// it costs little and avoids silently walking a <head>'s own <style>/
// <meta> children if a full document is ever passed in by mistake.
const DomNode *ContentRoot(const DomNode *root) {
    if (!root) return root;
    const DomNode *cur = root;
    for (auto &c : cur->children) {
        if (c->type == DomNodeType::Element && c->tag == "html") {
            cur = c.get();
            break;
        }
    }
    for (auto &c : cur->children) {
        if (c->type == DomNodeType::Element && c->tag == "body") return c.get();
    }
    return cur;
}

// ============================================================================
// LaTeX backend
// ============================================================================

std::string LatexEscape(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\textbackslash{}"; break;
            case '{': out += "\\{"; break;
            case '}': out += "\\}"; break;
            case '$': out += "\\$"; break;
            case '&': out += "\\&"; break;
            case '#': out += "\\#"; break;
            case '^': out += "\\textasciicircum{}"; break;
            case '_': out += "\\_"; break;
            case '~': out += "\\textasciitilde{}"; break;
            case '%': out += "\\%"; break;
            default: out += c;
        }
    }
    return out;
}

// Escapes only the three characters fancyvrb's Verbatim environment can't
// already render literally once commandchars=\\\{\} is active (see
// RenderOrgCodeBlockLatex below): a real backslash, since commandchars
// picked '\' as its own escape-introducer; and '{'/'}', since commandchars
// picked those as its command-argument delimiters. Every other character
// -- including LaTeX's usual specials ('$', '&', '#', '^', '_', '~', '%')
// -- Verbatim already renders as a literal glyph with no escaping needed,
// unlike LatexEscape's prose context above. Mirrors Pygments' own LaTeX
// formatter, which escapes exactly this same trio (as \PYZbs{}/\PYZob{}/
// \PYZcb{}) for exactly this reason.
std::string LatexEscapeVerbatim(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\mepbs{}"; break;
            case '{': out += "\\mepob{}"; break;
            case '}': out += "\\mepcb{}"; break;
            default: out += c;
        }
    }
    return out;
}

const DomNode *FindChildTag(const DomNode *node, const std::string &tag) {
    for (auto &c : node->children) {
        if (c->type == DomNodeType::Element && c->tag == tag) return c.get();
    }
    return nullptr;
}

// Renders a mep_org_html_code_block-shaped <div class="org-code-block">
// (main.cpp's kBuiltinOrgExport) as a bordered/headered tcolorbox
// (mepcodebox, defined in this file's own LaTeX preamble below) around a
// fancyvrb Verbatim -- the LaTeX counterpart of that HTML div's own
// border/header/highlighting CSS, sharing its Treesitter-computed
// highlighting exactly rather than re-deriving it (no minted/Pygments
// dependency, which would need -shell-escape support tectonic may not
// offer, plus a Python install this codebase otherwise never requires).
// <span class="tok-x"> children (x one of mep.ts_capture_hl's own
// highlight-group names, lowercased) become \mepc{x}{...}, sharing that
// exact suffix with the meptok<x> colors this file's own preamble defines
// -- so this function needs no separate capture->color table of its own,
// just the class name already baked into the HTML by the same export pass.
void RenderOrgCodeBlockLatex(const DomNode *div_node, std::string &out) {
    const DomNode *pre = FindChildTag(div_node, "pre");
    const DomNode *code = pre ? FindChildTag(pre, "code") : nullptr;
    if (!code) return;  // malformed/hand-written input -- degrade to nothing rather than guess
    // data-lang, not a "language-X" class (mep_org_html_code_block, main.cpp
    // -- see its own comment on why: a per-block-varying class value can
    // never be targeted by a single CSS rule in mep's own in-pane HTML
    // viewer, which is why that generator moved it to a plain attribute).
    std::string lang;
    auto lang_it = code->attrs.find("data-lang");
    if (lang_it != code->attrs.end()) lang = lang_it->second;
    std::string body;
    for (auto &c : code->children) {
        if (c->type == DomNodeType::Text) {
            body += LatexEscapeVerbatim(c->text);
        } else if (c->type == DomNodeType::Element && c->tag == "span") {
            const std::string &cls = c->Class();
            std::string tok = cls.compare(0, 4, "tok-") == 0 ? cls.substr(4) : "";
            std::string text = LatexEscapeVerbatim(CollectRawText(c.get()));
            body += tok.empty() ? text : ("\\mepc{" + tok + "}{" + text + "}");
        }
    }
    // A leading newline right after <code ...> (mep_org_html_code_block
    // always starts the body on its own line) would otherwise become a
    // blank first line inside the Verbatim block.
    if (!body.empty() && body.front() == '\n') body.erase(body.begin());
    // \color{mepCodeFg} sets the Verbatim's default (unhighlighted) text
    // color to match the box's dark background -- tcolorbox's own text
    // color inside the box is otherwise whatever ambient color surrounds
    // it (normally black, invisible against mepCodeBg).
    out += "\n\\begin{mepcodebox}{" + LatexEscape(lang.empty() ? "text" : lang) +
           "}\n\\color{mepCodeFg}\n\\begin{Verbatim}[commandchars=\\\\\\{\\}]\n" + body +
           "\n\\end{Verbatim}\n\\end{mepcodebox}\n";
}

struct LatexCtx {
    std::string base_dir;
};

void WalkLatexNode(const DomNode *node, LatexCtx &ctx, std::string &out) {
    if (node->type == DomNodeType::Text) {
        out += LatexEscape(node->text);
        return;
    }
    const std::string &tag = node->tag;
    if (tag == "script" || tag == "style" || tag == "head" || tag == "title") return;

    int level;
    if (IsHeadingTag(tag, level)) {
        // article class only goes 5 deep (section..subparagraph) --
        // clamp h5/h6 both to subparagraph rather than erroring.
        static const char *kCmds[] = {"section", "section",       "subsection",    "subsubsection",
                                       "paragraph", "subparagraph", "subparagraph"};
        out += "\n\\" + std::string(kCmds[std::min(level, 6)]) + "{";
        for (auto &c : node->children) WalkLatexNode(c.get(), ctx, out);
        out += "}\n";
        return;
    }
    if (tag == "math") {
        std::string latex = CollectRawText(node);
        out += IsMathDisplay(node) ? ("\n\\[" + latex + "\\]\n") : ("$" + latex + "$");
        return;
    }
    if (tag == "div" && node->Class() == "org-code-block") {
        RenderOrgCodeBlockLatex(node, out);
        return;
    }
    if (tag == "p") {
        out += "\n";
        for (auto &c : node->children) WalkLatexNode(c.get(), ctx, out);
        out += "\n\n";
        return;
    }
    if (tag == "br") {
        out += " \\\\\n";
        return;
    }
    if (tag == "hr") {
        out += "\n\\par\\noindent\\rule{\\linewidth}{0.4pt}\\par\n";
        return;
    }
    if (tag == "b" || tag == "strong") {
        out += "\\textbf{";
        for (auto &c : node->children) WalkLatexNode(c.get(), ctx, out);
        out += "}";
        return;
    }
    if (tag == "i" || tag == "em") {
        out += "\\textit{";
        for (auto &c : node->children) WalkLatexNode(c.get(), ctx, out);
        out += "}";
        return;
    }
    if (tag == "u") {
        out += "\\underline{";
        for (auto &c : node->children) WalkLatexNode(c.get(), ctx, out);
        out += "}";
        return;
    }
    if (tag == "s" || tag == "strike" || tag == "del") {
        out += "\\sout{";
        for (auto &c : node->children) WalkLatexNode(c.get(), ctx, out);
        out += "}";
        return;
    }
    if (tag == "code" || tag == "tt") {
        out += "\\texttt{" + LatexEscape(CollectRawText(node)) + "}";
        return;
    }
    if (tag == "pre") {
        std::string raw = CollectRawText(node);
        if (!raw.empty() && raw.front() == '\n') raw.erase(raw.begin());
        out += "\n\\begin{verbatim}\n" + raw + "\n\\end{verbatim}\n";
        return;
    }
    if (tag == "blockquote") {
        out += "\n\\begin{quote}\n";
        for (auto &c : node->children) WalkLatexNode(c.get(), ctx, out);
        out += "\n\\end{quote}\n";
        return;
    }
    if (tag == "ul" || tag == "ol") {
        out += tag == "ul" ? "\n\\begin{itemize}\n" : "\n\\begin{enumerate}\n";
        for (auto &c : node->children) WalkLatexNode(c.get(), ctx, out);
        out += tag == "ul" ? "\\end{itemize}\n" : "\\end{enumerate}\n";
        return;
    }
    if (tag == "li") {
        out += "\\item ";
        for (auto &c : node->children) WalkLatexNode(c.get(), ctx, out);
        out += "\n";
        return;
    }
    if (tag == "a") {
        auto it = node->attrs.find("href");
        std::string inner;
        for (auto &c : node->children) WalkLatexNode(c.get(), ctx, inner);
        if (it == node->attrs.end() || it->second.empty()) {
            out += inner;
        } else {
            // \href's URL argument is *not* run through prose escaping
            // (a literal '#'/'%' there is normal in a real URL and
            // \href handles it as a special, mostly-verbatim argument);
            // only '{'/'}' would actually break parsing, and neither is
            // valid in a bare URL, so it's passed through as-is.
            out += "\\href{" + it->second + "}{" + inner + "}";
        }
        return;
    }
    if (tag == "img") {
        auto src_it = node->attrs.find("src");
        std::string resolved = src_it != node->attrs.end() ? ResolveLocalPath(src_it->second, ctx.base_dir) : "";
        std::ifstream probe(resolved, std::ios::binary);
        if (!resolved.empty() && probe) {
            out += "\n\\begin{figure}[h]\n\\centering\n\\includegraphics[width=0.9\\linewidth]{" + resolved +
                   "}\n\\end{figure}\n";
        } else {
            out += "\n% [image not found: " + LatexEscape(src_it != node->attrs.end() ? src_it->second : "?") + "]\n";
        }
        return;
    }
    if (tag == "table") {
        std::vector<const DomNode *> rows;
        CollectTableRows(node, rows);
        size_t max_cols = TableMaxCols(rows);
        if (max_cols == 0) return;
        out += "\n\\begin{longtable}{|";
        for (size_t i = 0; i < max_cols; i++) out += "l|";
        out += "}\n\\hline\n";
        for (const DomNode *r : rows) {
            bool first = true;
            for (auto &c : r->children) {
                if (c->tag != "td" && c->tag != "th") continue;
                if (!first) out += " & ";
                first = false;
                if (c->tag == "th") out += "\\textbf{";
                for (auto &gc : c->children) WalkLatexNode(gc.get(), ctx, out);
                if (c->tag == "th") out += "}";
            }
            out += " \\\\\n\\hline\n";
        }
        out += "\\end{longtable}\n";
        return;
    }
    // Unrecognized container (div/span/body/#document/...) -- recurse
    // with no wrapper, matching ParseHtml's own "unknown tag renders as
    // a generic container" tolerance.
    for (auto &c : node->children) WalkLatexNode(c.get(), ctx, out);
}

}  // namespace

std::string ExportHtmlToLatex(const std::string &html, const std::string &title, const std::string &author,
                               const std::string &base_dir) {
    HtmlDoc doc;
    ParseHtml(html, doc);
    const DomNode *root = ContentRoot(doc.root.get());

    LatexCtx ctx;
    ctx.base_dir = base_dir;
    std::string body;
    if (root) {
        for (auto &c : root->children) WalkLatexNode(c.get(), ctx, body);
    }

    std::ostringstream out;
    out << "\\documentclass[11pt]{article}\n"
        << "\\usepackage[utf8]{inputenc}\n"
        << "\\usepackage[T1]{fontenc}\n"
        << "\\usepackage{graphicx}\n"
        << "\\usepackage{longtable}\n"
        << "\\usepackage[normalem]{ulem}\n"
        << "\\usepackage{amsmath}\n"
        << "\\usepackage{amssymb}\n"
        << "\\usepackage[margin=1in]{geometry}\n"
        << "\\usepackage{hyperref}\n"
        // Code-block styling (RenderOrgCodeBlockLatex, above): xcolor for
        // \definecolor/\textcolor, fancyvrb for a Verbatim that can mix in
        // real LaTeX commands (commandchars) instead of rendering 100%
        // literally, tcolorbox (skins+breakable libraries, loaded as
        // package options) for the bordered/headered box itself --
        // breakable so a long code block can split across a page instead
        // of overflowing it. Same light-on-white palette as
        // mep_org_html_wrap_document's own CSS (main.cpp) -- white body
        // (mepCodeBg matches the page itself, no separate page-color
        // definition needed), a light gray header band, and a colored left
        // accent bar -- so HTML and PDF export look like the same theme;
        // meptok<name> shares its exact (lowercase) suffix with the
        // tok-<name> CSS class names the HTML side of this same export
        // pass emits -- both lowercase for the same reason:
        // mep.org_html_highlight_line lowercases the highlight-group name
        // before putting it in a class, since mep's own in-pane HTML
        // viewer (html_doc.cpp) lowercases every CSS class selector it
        // parses, so a mixed-case class would never match its own
        // stylesheet rule there. LaTeX color names are case-sensitive same
        // as CSS class names, so RenderOrgCodeBlockLatex reading that same
        // lowercase suffix back out of the class attribute needs these
        // defined lowercase too, not just the CSS.
        << "\\usepackage{xcolor}\n"
        << "\\usepackage{fancyvrb}\n"
        << "\\usepackage[skins,breakable]{tcolorbox}\n"
        << "\\definecolor{meptokcomment}{HTML}{6B7280}\n"
        << "\\definecolor{meptokgreen}{HTML}{1A7F37}\n"
        << "\\definecolor{meptokcyan}{HTML}{0B7285}\n"
        << "\\definecolor{meptokpurple}{HTML}{8250DF}\n"
        << "\\definecolor{meptokblue}{HTML}{0550AE}\n"
        << "\\definecolor{meptokorange}{HTML}{953800}\n"
        << "\\definecolor{meptokred}{HTML}{CF222E}\n"
        << "\\definecolor{meptokyellow}{HTML}{9A6700}\n"
        << "\\definecolor{mepCodeBg}{HTML}{FFFFFF}\n"
        << "\\definecolor{mepCodeFg}{HTML}{24292E}\n"
        << "\\definecolor{mepCodeMuted}{HTML}{57606A}\n"
        << "\\definecolor{mepCodeAccent}{HTML}{6B8AFD}\n"
        << "\\definecolor{mepCodeBorder}{HTML}{D0D7DE}\n"
        << "\\definecolor{mepCodeHeaderBg}{HTML}{F6F8FA}\n"
        // \mepbs/\mepob/\mepcb print a literal backslash/brace pair from
        // inside the Verbatim's commandchars escape (LatexEscapeVerbatim
        // routes every literal '\'/'{'/'}' in a code block's own source
        // through these); \mepc wraps one Treesitter-captured span in its
        // highlight-group color.
        << "\\newcommand{\\mepbs}{\\textbackslash}\n"
        << "\\newcommand{\\mepob}{\\{}\n"
        << "\\newcommand{\\mepcb}{\\}}\n"
        << "\\newcommand{\\mepc}[2]{\\textcolor{meptok#1}{#2}}\n"
        // boxrule (a thin, all-around mepCodeBorder frame) plus the
        // thicker borderline west accent on top of it, mirroring the HTML
        // side's own border-top/right/bottom (subtle gray) + border-left
        // (accent) split exactly.
        << "\\newtcolorbox{mepcodebox}[1]{enhanced, breakable, boxrule=0.75pt, arc=2pt, "
           "colback=mepCodeBg, colframe=mepCodeBorder, borderline west={3pt}{0pt}{mepCodeAccent}, "
           "fonttitle=\\ttfamily\\small, coltitle=mepCodeMuted, colbacktitle=mepCodeHeaderBg, "
           "title=#1, left=8pt, right=8pt, top=6pt, bottom=6pt}\n";
    if (!title.empty()) out << "\\title{" << LatexEscape(title) << "}\n";
    if (!author.empty()) out << "\\author{" << LatexEscape(author) << "}\n";
    out << "\\date{}\n\\begin{document}\n";
    if (!title.empty()) out << "\\maketitle\n";
    out << body << "\n\\end{document}\n";
    return out.str();
}

// ============================================================================
// ODT backend
// ============================================================================

namespace {

struct OdtImage {
    std::string zip_name;
    std::string bytes;
};

struct OdtCtx {
    std::string base_dir;
    int image_counter = 0;
    int table_counter = 0;
    std::vector<OdtImage> images;
};

// Splits `text` into pcdata chunks around '\t' (-> <text:tab/>) and runs
// of 2+ spaces (-> one literal space + <text:s text:c="N-1"/>, ODF's own
// convention for preserving repeated whitespace an XML/HTML-style
// collapse would otherwise eat) and appends the result to `parent`.
void AppendTextRun(pugi::xml_node parent, const std::string &text) {
    size_t i = 0, n = text.size();
    size_t seg_start = 0;
    auto flush_pcdata = [&](size_t end) {
        if (end > seg_start) parent.append_child(pugi::node_pcdata).set_value(text.substr(seg_start, end - seg_start).c_str());
    };
    while (i < n) {
        if (text[i] == '\t') {
            flush_pcdata(i);
            parent.append_child("text:tab");
            i++;
            seg_start = i;
            continue;
        }
        if (text[i] == ' ') {
            size_t run_start = i;
            while (i < n && text[i] == ' ') i++;
            size_t count = i - run_start;
            if (count >= 2) {
                flush_pcdata(run_start);
                parent.append_child(pugi::node_pcdata).set_value(" ");
                pugi::xml_node s = parent.append_child("text:s");
                s.append_attribute("text:c").set_value(static_cast<unsigned int>(count - 1));
                seg_start = i;
            }
            continue;
        }
        i++;
    }
    flush_pcdata(n);
}

void AppendOdtImageFrame(const DomNode *node, OdtCtx &ctx, pugi::xml_node container);

void WalkOdtInline(const DomNode *node, OdtCtx &ctx, pugi::xml_node container) {
    if (node->type == DomNodeType::Text) {
        AppendTextRun(container, node->text);
        return;
    }
    const std::string &tag = node->tag;
    if (tag == "script" || tag == "style" || tag == "head" || tag == "title") return;
    if (tag == "br") {
        container.append_child("text:line-break");
        return;
    }
    if (tag == "math") {
        // No real OpenDocument Formula/MathML support (v1 scope cut) --
        // shown as its own raw LaTeX source in monospace text instead of
        // silently dropping the equation.
        std::string latex = CollectRawText(node);
        pugi::xml_node span = container.append_child("text:span");
        span.append_attribute("text:style-name").set_value("MepSrc");
        AppendTextRun(span, IsMathDisplay(node) ? ("[" + latex + "]") : ("$" + latex + "$"));
        return;
    }
    if (tag == "code" || tag == "tt") {
        pugi::xml_node span = container.append_child("text:span");
        span.append_attribute("text:style-name").set_value("MepSrc");
        AppendTextRun(span, CollectRawText(node));
        return;
    }
    if (tag == "a") {
        auto it = node->attrs.find("href");
        pugi::xml_node link = container.append_child("text:a");
        link.append_attribute("xlink:type").set_value("simple");
        link.append_attribute("xlink:href").set_value(it != node->attrs.end() ? it->second.c_str() : "");
        for (auto &c : node->children) WalkOdtInline(c.get(), ctx, link);
        return;
    }
    const char *style = nullptr;
    if (tag == "b" || tag == "strong") style = "MepBold";
    else if (tag == "i" || tag == "em") style = "MepItalic";
    else if (tag == "u") style = "MepUnderline";
    else if (tag == "s" || tag == "strike" || tag == "del") style = "MepStrike";
    if (style) {
        pugi::xml_node span = container.append_child("text:span");
        span.append_attribute("text:style-name").set_value(style);
        for (auto &c : node->children) WalkOdtInline(c.get(), ctx, span);
        return;
    }
    if (tag == "img") {
        // <img> is inline per html_doc.cpp's own TagDefaults (s.block =
        // false), so it's WalkOdtInline, not WalkOdtBlock, that
        // ordinarily receives it -- straight into `container`, whatever
        // that is (an open <text:p> from WalkOdtBlockChildren's own
        // coalescing, or a <text:span>/<text:a> another inline tag
        // already opened). See AppendOdtImageFrame's own header for why
        // this never needs to open its own paragraph.
        AppendOdtImageFrame(node, ctx, container);
        return;
    }
    // Unrecognized inline container (span/whatever) -- recurse unwrapped.
    for (auto &c : node->children) WalkOdtInline(c.get(), ctx, container);
}

void WalkOdtBlock(const DomNode *node, OdtCtx &ctx, pugi::xml_node text_body);

// Appends one <draw:frame><draw:image .../></draw:frame> (ODF images are
// always text:anchor-type="as-char", i.e. inline-flowing within
// whatever paragraph/span run contains them -- never a paragraph in
// their own right) directly to `container`, decoding the source file via
// ImageDoc (already linked, raylib-free -- same decoder main.cpp uses
// for texture upload) purely to get its intrinsic pixel size, so the
// frame gets a real aspect ratio instead of a fixed square. Print width
// is capped at 15cm; an image whose file can't be read/decoded falls
// back to a text placeholder appended to `container` directly, same
// tolerance the LaTeX backend's own <img> handling has.
void AppendOdtImageFrame(const DomNode *node, OdtCtx &ctx, pugi::xml_node container) {
    auto src_it = node->attrs.find("src");
    std::string resolved = src_it != node->attrs.end() ? ResolveLocalPath(src_it->second, ctx.base_dir) : "";
    std::string bytes;
    bool ok = !resolved.empty() && ReadFileBytes(resolved, bytes);
    ImageDoc decoded;
    if (ok) ok = decoded.LoadFromMemory(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size());
    if (!ok) {
        AppendTextRun(container, "[image not found: " + (src_it != node->attrs.end() ? src_it->second : std::string("?")) + "]");
        return;
    }
    std::string ext = LowerExt(resolved);
    if (ext.empty()) ext = "png";
    std::string zip_name = "Pictures/img" + std::to_string(ctx.image_counter++) + "." + ext;
    ctx.images.push_back({zip_name, bytes});

    double width_cm = 15.0;
    double height_cm = decoded.Width() > 0 ? width_cm * static_cast<double>(decoded.Height()) / decoded.Width() : width_cm;
    char width_buf[32], height_buf[32];
    std::snprintf(width_buf, sizeof(width_buf), "%.2fcm", width_cm);
    std::snprintf(height_buf, sizeof(height_buf), "%.2fcm", height_cm);

    pugi::xml_node frame = container.append_child("draw:frame");
    frame.append_attribute("draw:name").set_value(("MepImage" + std::to_string(ctx.image_counter)).c_str());
    frame.append_attribute("text:anchor-type").set_value("as-char");
    frame.append_attribute("svg:width").set_value(width_buf);
    frame.append_attribute("svg:height").set_value(height_buf);
    pugi::xml_node img_el = frame.append_child("draw:image");
    img_el.append_attribute("xlink:href").set_value(zip_name.c_str());
    img_el.append_attribute("xlink:type").set_value("simple");
    img_el.append_attribute("xlink:show").set_value("embed");
    img_el.append_attribute("xlink:actuate").set_value("onLoad");
}

void AppendOdtTable(const DomNode *node, OdtCtx &ctx, pugi::xml_node text_body) {
    std::vector<const DomNode *> rows;
    CollectTableRows(node, rows);
    size_t max_cols = TableMaxCols(rows);
    if (max_cols == 0) return;
    pugi::xml_node table = text_body.append_child("table:table");
    table.append_attribute("table:name").set_value(("MepTable" + std::to_string(ctx.table_counter++)).c_str());
    for (size_t i = 0; i < max_cols; i++) table.append_child("table:table-column");
    for (const DomNode *r : rows) {
        pugi::xml_node trow = table.append_child("table:table-row");
        for (auto &c : r->children) {
            if (c->tag != "td" && c->tag != "th") continue;
            pugi::xml_node cell = trow.append_child("table:table-cell");
            cell.append_attribute("office:value-type").set_value("string");
            pugi::xml_node p = cell.append_child("text:p");
            if (c->tag == "th") p.append_attribute("text:style-name").set_value("MepTableHeading");
            for (auto &gc : c->children) WalkOdtInline(gc.get(), ctx, p);
        }
    }
}

// mep.org_export('html') (kBuiltinOrgExport, main.cpp) emits an ordinary
// prose line as bare text/inline markup with NO <p> wrapper at all --
// real browsers tolerate this fine (whitespace-collapsed text nodes
// flow together visually regardless), but naively dispatching each such
// sibling through WalkOdtBlock one at a time would give every fragment
// its own separate <text:p> (and, worse, silently drop an inline
// element's own <b>/<i>/etc. styling, since WalkOdtBlock's own "stray
// text" handling doesn't know about a surrounding inline tag the way
// WalkOdtInline does). This walks a block container's children as a
// sequence instead, using each child's own already-computed
// ComputedStyle::block (html_doc.cpp's ComputeStyles -- the same
// block-vs-inline signal main.cpp's own HtmlLayoutBlock dispatches on
// for pane rendering) to group a run of consecutive inline
// siblings -- Text nodes and inline elements alike -- into ONE
// <text:p>, only closing it when a genuinely block-level sibling (a
// <p>/<h1>/<ul>/<table>/...) is reached. A run of pure whitespace
// between two block siblings (the literal '\n' text nodes
// table.concat(out, '\n') leaves between output lines) is dropped
// rather than opening an empty paragraph for it.
void WalkOdtBlockChildren(const std::vector<std::unique_ptr<DomNode>> &children, OdtCtx &ctx,
                           pugi::xml_node text_body);

void WalkOdtBlock(const DomNode *node, OdtCtx &ctx, pugi::xml_node text_body) {
    if (node->type == DomNodeType::Text) {
        // Reached only when the CALLER didn't already route this
        // through WalkOdtBlockChildren (i.e. every real entry point
        // below) -- kept as a tolerant fallback rather than an assert,
        // same "degrade gracefully" spirit as the rest of this walker.
        bool blank = node->text.find_first_not_of(" \t\r\n") == std::string::npos;
        if (!blank) {
            pugi::xml_node p = text_body.append_child("text:p");
            AppendTextRun(p, node->text);
        }
        return;
    }
    const std::string &tag = node->tag;
    if (tag == "script" || tag == "style" || tag == "head" || tag == "title") return;
    // mep_org_html_code_block's (main.cpp) own language-label/Copy-button
    // header has no ODT equivalent (no WalkLatexNode-style rich box here
    // either) -- skipped rather than falling through to the generic
    // container case below, which would otherwise leak "python Copy" as
    // stray paragraph text right before that code block's own <pre>
    // (still handled normally, just below, via the tag == "pre" case: a
    // plain preformatted block, unhighlighted, same as before this class
    // existed).
    if (tag == "div" && node->Class() == "org-code-header") return;

    int level;
    if (IsHeadingTag(tag, level)) {
        pugi::xml_node h = text_body.append_child("text:h");
        h.append_attribute("text:outline-level").set_value(level);
        h.append_attribute("text:style-name").set_value(("MepHeading" + std::to_string(level)).c_str());
        for (auto &c : node->children) WalkOdtInline(c.get(), ctx, h);
        return;
    }
    if (tag == "p") {
        pugi::xml_node p = text_body.append_child("text:p");
        for (auto &c : node->children) WalkOdtInline(c.get(), ctx, p);
        return;
    }
    if (tag == "hr") {
        text_body.append_child("text:p").append_attribute("text:style-name").set_value("MepHr");
        return;
    }
    if (tag == "pre") {
        std::string raw = CollectRawText(node);
        if (!raw.empty() && raw.front() == '\n') raw.erase(raw.begin());
        size_t start = 0, n = raw.size();
        while (start <= n) {
            size_t nl = raw.find('\n', start);
            std::string line = raw.substr(start, (nl == std::string::npos ? n : nl) - start);
            pugi::xml_node p = text_body.append_child("text:p");
            p.append_attribute("text:style-name").set_value("MepPreformatted");
            AppendTextRun(p, line);
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
        return;
    }
    if (tag == "blockquote") {
        // A <p> child gets the MepQuote paragraph style; anything else
        // (not currently produced by mep.org_export('html'), which has
        // no #+BEGIN_QUOTE handling yet -- this only matters for
        // hand-written HTML input) falls back through WalkOdtBlock one
        // child at a time, same tolerance-not-perfection as everywhere
        // else in this file.
        for (auto &c : node->children) {
            if (c->type == DomNodeType::Element && c->tag == "p") {
                pugi::xml_node p = text_body.append_child("text:p");
                p.append_attribute("text:style-name").set_value("MepQuote");
                for (auto &gc : c->children) WalkOdtInline(gc.get(), ctx, p);
            } else {
                WalkOdtBlock(c.get(), ctx, text_body);
            }
        }
        return;
    }
    if (tag == "ul" || tag == "ol") {
        pugi::xml_node list = text_body.append_child("text:list");
        list.append_attribute("text:style-name").set_value(tag == "ul" ? "MepBulletList" : "MepNumberList");
        for (auto &c : node->children) {
            if (c->tag != "li") continue;
            pugi::xml_node item = list.append_child("text:list-item");
            pugi::xml_node p = item.append_child("text:p");
            for (auto &gc : c->children) {
                if (gc->type == DomNodeType::Element && (gc->tag == "ul" || gc->tag == "ol")) {
                    WalkOdtBlock(gc.get(), ctx, item);  // nested list, sibling of the <text:p> above
                } else {
                    WalkOdtInline(gc.get(), ctx, p);
                }
            }
        }
        return;
    }
    if (tag == "table") {
        AppendOdtTable(node, ctx, text_body);
        return;
    }
    if (tag == "img") {
        // Reached only if this <img>'s own computed style was overridden
        // to block-level (unusual -- TagDefaults' own default is
        // inline, the normal case WalkOdtInline's own "img" branch
        // handles) -- give it a paragraph of its own to flow within,
        // same as any other block-dispatched content here.
        AppendOdtImageFrame(node, ctx, text_body.append_child("text:p"));
        return;
    }
    // Unrecognized container (div/span/body/#document/...) -- walk its
    // children as a sequence (WalkOdtBlockChildren, above) rather than
    // recursing into WalkOdtBlock one at a time, so any bare inline
    // content living directly inside it (not itself wrapped in a <p> --
    // exactly the shape mep.org_export('html') produces for an ordinary
    // prose line) still gets coalesced into one real paragraph instead
    // of one-<text:p>-per-fragment.
    WalkOdtBlockChildren(node->children, ctx, text_body);
}

void WalkOdtBlockChildren(const std::vector<std::unique_ptr<DomNode>> &children, OdtCtx &ctx,
                           pugi::xml_node text_body) {
    pugi::xml_node open_p;  // empty/null until a run of inline content opens one
    for (auto &c : children) {
        bool is_block = c->type == DomNodeType::Element && c->style.block;
        if (is_block) {
            open_p = pugi::xml_node();
            WalkOdtBlock(c.get(), ctx, text_body);
            continue;
        }
        bool blank_text = c->type == DomNodeType::Text && c->text.find_first_not_of(" \t\r\n") == std::string::npos;
        if (!open_p) {
            if (blank_text) continue;  // whitespace between block siblings -- drop, don't open an empty paragraph
            open_p = text_body.append_child("text:p");
        }
        WalkOdtInline(c.get(), ctx, open_p);
    }
}

// Every named style content.xml's WalkOdt* functions reference, defined
// as <office:automatic-style>s the same self-contained way
// office_odt.cpp's own SaveOdtToMemory already does (see its
// GetOrCreateTextStyle/GetOrCreateParaStyle) -- no dependency on
// styles.xml resolving a matching named style.
void AppendOdtAutomaticStyles(pugi::xml_node auto_styles) {
    auto text_style = [&](const char *name, bool bold, bool italic, bool underline, bool strike, bool mono) {
        pugi::xml_node s = auto_styles.append_child("style:style");
        s.append_attribute("style:name").set_value(name);
        s.append_attribute("style:family").set_value("text");
        pugi::xml_node tp = s.append_child("style:text-properties");
        if (bold) tp.append_attribute("fo:font-weight").set_value("bold");
        if (italic) tp.append_attribute("fo:font-style").set_value("italic");
        if (underline) tp.append_attribute("style:text-underline-style").set_value("solid");
        if (strike) tp.append_attribute("style:text-line-through-style").set_value("solid");
        if (mono) tp.append_attribute("style:font-name").set_value("mep Mono");
    };
    text_style("MepBold", true, false, false, false, false);
    text_style("MepItalic", false, true, false, false, false);
    text_style("MepUnderline", false, false, true, false, false);
    text_style("MepStrike", false, false, false, true, false);
    text_style("MepSrc", false, false, false, false, true);

    auto para_style = [&](const std::string &name, double font_pt, bool bold, double margin_top,
                           double margin_bottom, bool mono, bool italic) {
        pugi::xml_node s = auto_styles.append_child("style:style");
        s.append_attribute("style:name").set_value(name.c_str());
        s.append_attribute("style:family").set_value("paragraph");
        pugi::xml_node pp = s.append_child("style:paragraph-properties");
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.3fin", margin_top);
        pp.append_attribute("fo:margin-top").set_value(buf);
        std::snprintf(buf, sizeof(buf), "%.3fin", margin_bottom);
        pp.append_attribute("fo:margin-bottom").set_value(buf);
        pugi::xml_node tp = s.append_child("style:text-properties");
        char fbuf[16];
        std::snprintf(fbuf, sizeof(fbuf), "%.0fpt", font_pt);
        tp.append_attribute("fo:font-size").set_value(fbuf);
        if (bold) tp.append_attribute("fo:font-weight").set_value("bold");
        if (italic) tp.append_attribute("fo:font-style").set_value("italic");
        if (mono) tp.append_attribute("style:font-name").set_value("mep Mono");
    };
    static const double kHeadingSizes[6] = {24, 20, 16, 13, 12, 11};
    for (int i = 1; i <= 6; i++) {
        para_style("MepHeading" + std::to_string(i), kHeadingSizes[i - 1], true, 0.15, 0.08, false, false);
    }
    para_style("MepPreformatted", 10, false, 0.05, 0.05, true, false);
    para_style("MepQuote", 11, false, 0.1, 0.1, false, true);

    // Horizontal rule: a paragraph with a bottom border and no text --
    // ODF has no dedicated <hr> equivalent, this is the conventional way
    // real ODF writers represent one.
    pugi::xml_node hr = auto_styles.append_child("style:style");
    hr.append_attribute("style:name").set_value("MepHr");
    hr.append_attribute("style:family").set_value("paragraph");
    pugi::xml_node hr_pp = hr.append_child("style:paragraph-properties");
    hr_pp.append_attribute("style:border-line-width-bottom").set_value("0.0008in 0.0008in 0.0008in");
    hr_pp.append_attribute("fo:border-bottom").set_value("0.5pt solid #000000");
    hr_pp.append_attribute("fo:padding").set_value("0in");
    hr_pp.append_attribute("fo:margin-top").set_value("0.1in");
    hr_pp.append_attribute("fo:margin-bottom").set_value("0.1in");

    pugi::xml_node th = auto_styles.append_child("style:style");
    th.append_attribute("style:name").set_value("MepTableHeading");
    th.append_attribute("style:family").set_value("paragraph");
    th.append_child("style:text-properties").append_attribute("fo:font-weight").set_value("bold");

    // List styles -- one level only (a nested <ul>/<ol> reuses the same
    // level-1 marker rather than a distinct per-depth one; a real, if
    // visually flat, documented v1 simplification).
    auto list_bullet_style = [&](const char *name, const char *bullet_char) {
        pugi::xml_node ls = auto_styles.append_child("text:list-style");
        ls.append_attribute("style:name").set_value(name);
        pugi::xml_node lvl = ls.append_child("text:list-level-style-bullet");
        lvl.append_attribute("text:level").set_value("1");
        lvl.append_attribute("text:bullet-char").set_value(bullet_char);
        pugi::xml_node lp = lvl.append_child("style:list-level-properties");
        lp.append_attribute("text:list-level-position-and-space-mode").set_value("label-alignment");
        pugi::xml_node la = lp.append_child("style:list-level-label-alignment");
        la.append_attribute("text:label-followed-by").set_value("listtab");
        la.append_attribute("text:list-tab-stop-position").set_value("0.5in");
        la.append_attribute("fo:text-indent").set_value("-0.25in");
        la.append_attribute("fo:margin-left").set_value("0.5in");
    };
    list_bullet_style("MepBulletList", "\xe2\x80\xa2");  // U+2022 BULLET, UTF-8 bytes (not a \u escape)

    pugi::xml_node ls = auto_styles.append_child("text:list-style");
    ls.append_attribute("style:name").set_value("MepNumberList");
    pugi::xml_node lvl = ls.append_child("text:list-level-style-number");
    lvl.append_attribute("text:level").set_value("1");
    lvl.append_attribute("style:num-format").set_value("1");
    lvl.append_attribute("style:num-suffix").set_value(".");
    lvl.append_attribute("text:display-levels").set_value("1");
    pugi::xml_node lp = lvl.append_child("style:list-level-properties");
    lp.append_attribute("text:list-level-position-and-space-mode").set_value("label-alignment");
    pugi::xml_node la = lp.append_child("style:list-level-label-alignment");
    la.append_attribute("text:label-followed-by").set_value("listtab");
    la.append_attribute("text:list-tab-stop-position").set_value("0.5in");
    la.append_attribute("fo:text-indent").set_value("-0.25in");
    la.append_attribute("fo:margin-left").set_value("0.5in");
}

std::string BuildOdtStylesXml() {
    // A minimal-but-valid styles.xml -- content.xml's own automatic
    // styles (above) are fully self-contained, so this file only needs
    // to exist and be well-formed; it carries the document's default
    // page layout (letter-ish, 1in margins) via office:master-styles.
    pugi::xml_document doc;
    pugi::xml_node decl = doc.append_child(pugi::node_declaration);
    decl.append_attribute("version").set_value("1.0");
    decl.append_attribute("encoding").set_value("UTF-8");
    pugi::xml_node root = doc.append_child("office:document-styles");
    root.append_attribute("xmlns:office").set_value("urn:oasis:names:tc:opendocument:xmlns:office:1.0");
    root.append_attribute("xmlns:style").set_value("urn:oasis:names:tc:opendocument:xmlns:style:1.0");
    root.append_attribute("xmlns:fo").set_value("urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0");
    root.append_attribute("office:version").set_value("1.2");
    pugi::xml_node styles = root.append_child("office:styles");
    pugi::xml_node standard = styles.append_child("style:style");
    standard.append_attribute("style:name").set_value("Standard");
    standard.append_attribute("style:family").set_value("paragraph");
    standard.append_attribute("style:class").set_value("text");

    pugi::xml_node master_styles = root.append_child("office:master-styles");
    pugi::xml_node master = master_styles.append_child("style:master-page");
    master.append_attribute("style:name").set_value("Standard");
    master.append_attribute("style:page-layout-name").set_value("MepPageLayout");

    pugi::xml_node auto_styles = root.append_child("office:automatic-styles");
    pugi::xml_node layout = auto_styles.append_child("style:page-layout");
    layout.append_attribute("style:name").set_value("MepPageLayout");
    pugi::xml_node lp = layout.append_child("style:page-layout-properties");
    lp.append_attribute("fo:margin-top").set_value("1in");
    lp.append_attribute("fo:margin-bottom").set_value("1in");
    lp.append_attribute("fo:margin-left").set_value("1in");
    lp.append_attribute("fo:margin-right").set_value("1in");

    std::ostringstream ss;
    doc.save(ss, "", pugi::format_raw);
    return ss.str();
}

std::string BuildOdtMetaXml(const std::string &title, const std::string &author) {
    pugi::xml_document doc;
    pugi::xml_node decl = doc.append_child(pugi::node_declaration);
    decl.append_attribute("version").set_value("1.0");
    decl.append_attribute("encoding").set_value("UTF-8");
    pugi::xml_node root = doc.append_child("office:document-meta");
    root.append_attribute("xmlns:office").set_value("urn:oasis:names:tc:opendocument:xmlns:office:1.0");
    root.append_attribute("xmlns:dc").set_value("http://purl.org/dc/elements/1.1/");
    root.append_attribute("xmlns:meta").set_value("urn:oasis:names:tc:opendocument:xmlns:meta:1.0");
    root.append_attribute("office:version").set_value("1.2");
    pugi::xml_node meta = root.append_child("office:meta");
    if (!title.empty()) meta.append_child("dc:title").append_child(pugi::node_pcdata).set_value(title.c_str());
    if (!author.empty()) meta.append_child("dc:creator").append_child(pugi::node_pcdata).set_value(author.c_str());
    meta.append_child("meta:generator").append_child(pugi::node_pcdata).set_value("mep");
    std::ostringstream ss;
    doc.save(ss, "", pugi::format_raw);
    return ss.str();
}

std::string BuildOdtManifestXml(const std::vector<OdtImage> &images) {
    pugi::xml_document doc;
    pugi::xml_node decl = doc.append_child(pugi::node_declaration);
    decl.append_attribute("version").set_value("1.0");
    decl.append_attribute("encoding").set_value("UTF-8");
    pugi::xml_node root = doc.append_child("manifest:manifest");
    root.append_attribute("xmlns:manifest").set_value("urn:oasis:names:tc:opendocument:xmlns:manifest:1.0");
    root.append_attribute("manifest:version").set_value("1.2");
    auto entry = [&](const char *path, const char *media_type) {
        pugi::xml_node e = root.append_child("manifest:file-entry");
        e.append_attribute("manifest:full-path").set_value(path);
        e.append_attribute("manifest:media-type").set_value(media_type);
    };
    entry("/", "application/vnd.oasis.opendocument.text");
    entry("content.xml", "text/xml");
    entry("styles.xml", "text/xml");
    entry("meta.xml", "text/xml");
    for (const OdtImage &img : images) {
        std::string ext = LowerExt(img.zip_name);
        std::string media = ext == "jpg" || ext == "jpeg" ? "image/jpeg" : "image/" + ext;
        entry(img.zip_name.c_str(), media.c_str());
    }
    std::ostringstream ss;
    doc.save(ss, "", pugi::format_raw);
    return ss.str();
}

struct ZipEntryToWrite {
    std::string name;
    std::string data;  // uncompressed
    bool store;         // true = STORED, false = DEFLATE
};

void AppendLE16(std::string &s, uint16_t v) {
    s += static_cast<char>(v & 0xff);
    s += static_cast<char>((v >> 8) & 0xff);
}
void AppendLE32(std::string &s, uint32_t v) {
    s += static_cast<char>(v & 0xff);
    s += static_cast<char>((v >> 8) & 0xff);
    s += static_cast<char>((v >> 16) & 0xff);
    s += static_cast<char>((v >> 24) & 0xff);
}

// A from-scratch, minimal ZIP writer -- used instead of miniz's own
// mz_zip_writer_* API specifically because that API's mz_zip_writer_
// add_mem_ex_v2 (miniz.c) unconditionally sets general-purpose bit 3
// (MZ_ZIP_LDH_BIT_FLAG_HAS_LOCATOR -- sizes/CRC deferred to a trailing
// data descriptor after the entry's data) for any real in-memory buffer
// add, with no public flag able to suppress it -- MZ_ZIP_FLAG_WRITE_
// HEADER_SET_SIZE turned out to affect a different, lower-level
// callback-based write path, not this one (confirmed by reading
// miniz.c's own add_mem_ex_v2 body after the flag alone didn't change
// the output). That's valid per the zip spec and fine for miniz's own
// reader/most general-purpose tools, but LibreOffice's own strict ODF
// package loader rejects a "mimetype" entry (first entry, used for fast
// format sniffing) written that way -- confirmed two ways during
// development: (1) a real `libreoffice --headless --convert-to pdf`
// round-trip failed with "BrokenPackageRequest" against the miniz-
// writer-produced archive, and (2) rebuilding the exact same entries
// with Python's stdlib zipfile module (which writes real sizes/CRC
// directly in the local header, bit 3 clear) round-tripped through
// LibreOffice successfully. This writer always writes real sizes/CRC
// directly (bit 3 always clear, no data descriptor ever emitted) --
// still using miniz's own mz_crc32 and tdefl_compress_mem_to_heap
// (called with -15 window bits + MZ_DEFAULT_STRATEGY, the exact same
// tdefl_create_comp_flags_from_zip_params args miniz's own zip writer
// uses internally, see its mz_zip_writer_add_mem_ex_v2) for the actual
// CRC/deflate work rather than reimplementing either.
std::string BuildZipArchive(const std::vector<ZipEntryToWrite> &entries) {
    std::string out;
    struct CdRecord {
        std::string name;
        uint32_t crc, comp_size, uncomp_size, local_offset;
        uint16_t method;
    };
    std::vector<CdRecord> cd;
    for (const ZipEntryToWrite &e : entries) {
        uint32_t crc =
            static_cast<uint32_t>(mz_crc32(0, reinterpret_cast<const unsigned char *>(e.data.data()), e.data.size()));
        std::string comp_data;
        uint16_t method;
        if (e.store || e.data.empty()) {
            comp_data = e.data;
            method = 0;
        } else {
            size_t out_len = 0;
            mz_uint flags = tdefl_create_comp_flags_from_zip_params(MZ_DEFAULT_LEVEL, -15, MZ_DEFAULT_STRATEGY);
            void *compressed =
                tdefl_compress_mem_to_heap(e.data.data(), e.data.size(), &out_len, static_cast<int>(flags));
            if (compressed && out_len < e.data.size()) {
                comp_data.assign(static_cast<const char *>(compressed), out_len);
                method = 8;
            } else {
                comp_data = e.data;  // incompressible/tiny -- store rather than grow
                method = 0;
            }
            if (compressed) mz_free(compressed);
        }
        uint32_t local_offset = static_cast<uint32_t>(out.size());
        out += "PK\x03\x04";
        AppendLE16(out, 20);      // version needed
        AppendLE16(out, 0);       // general purpose flag -- always 0: no data descriptor, no UTF-8 flag needed (ASCII names only)
        AppendLE16(out, method);
        AppendLE16(out, 0);       // mod time
        AppendLE16(out, 0x21);    // mod date: 1980-01-01, the standard "no real timestamp" zip placeholder
        AppendLE32(out, crc);
        AppendLE32(out, static_cast<uint32_t>(comp_data.size()));
        AppendLE32(out, static_cast<uint32_t>(e.data.size()));
        AppendLE16(out, static_cast<uint16_t>(e.name.size()));
        AppendLE16(out, 0);  // extra field length
        out += e.name;
        out += comp_data;
        cd.push_back({e.name, crc, static_cast<uint32_t>(comp_data.size()), static_cast<uint32_t>(e.data.size()),
                       local_offset, method});
    }
    uint32_t cd_offset = static_cast<uint32_t>(out.size());
    for (const CdRecord &r : cd) {
        out += "PK\x01\x02";
        AppendLE16(out, 20);  // version made by
        AppendLE16(out, 20);  // version needed
        AppendLE16(out, 0);   // general purpose flag
        AppendLE16(out, r.method);
        AppendLE16(out, 0);    // mod time
        AppendLE16(out, 0x21);  // mod date
        AppendLE32(out, r.crc);
        AppendLE32(out, r.comp_size);
        AppendLE32(out, r.uncomp_size);
        AppendLE16(out, static_cast<uint16_t>(r.name.size()));
        AppendLE16(out, 0);  // extra field length
        AppendLE16(out, 0);  // comment length
        AppendLE16(out, 0);  // disk number start
        AppendLE16(out, 0);  // internal file attributes
        AppendLE32(out, 0);  // external file attributes
        AppendLE32(out, r.local_offset);
        out += r.name;
    }
    uint32_t cd_size = static_cast<uint32_t>(out.size()) - cd_offset;
    out += "PK\x05\x06";
    AppendLE16(out, 0);  // this disk number
    AppendLE16(out, 0);  // disk with start of central directory
    AppendLE16(out, static_cast<uint16_t>(cd.size()));  // entries on this disk
    AppendLE16(out, static_cast<uint16_t>(cd.size()));  // total entries
    AppendLE32(out, cd_size);
    AppendLE32(out, cd_offset);
    AppendLE16(out, 0);  // comment length
    return out;
}

}  // namespace

bool ExportHtmlToOdt(const std::string &html, const std::string &out_path, const std::string &title,
                     const std::string &author, const std::string &base_dir, std::string &error) {
    HtmlDoc doc;
    ParseHtml(html, doc);
    const DomNode *root = ContentRoot(doc.root.get());

    OdtCtx ctx;
    ctx.base_dir = base_dir;

    pugi::xml_document content_doc;
    pugi::xml_node decl = content_doc.append_child(pugi::node_declaration);
    decl.append_attribute("version").set_value("1.0");
    decl.append_attribute("encoding").set_value("UTF-8");
    pugi::xml_node content_root = content_doc.append_child("office:document-content");
    content_root.append_attribute("xmlns:office").set_value("urn:oasis:names:tc:opendocument:xmlns:office:1.0");
    content_root.append_attribute("xmlns:text").set_value("urn:oasis:names:tc:opendocument:xmlns:text:1.0");
    content_root.append_attribute("xmlns:table").set_value("urn:oasis:names:tc:opendocument:xmlns:table:1.0");
    content_root.append_attribute("xmlns:style").set_value("urn:oasis:names:tc:opendocument:xmlns:style:1.0");
    content_root.append_attribute("xmlns:fo").set_value("urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0");
    content_root.append_attribute("xmlns:draw").set_value("urn:oasis:names:tc:opendocument:xmlns:drawing:1.0");
    content_root.append_attribute("xmlns:svg").set_value("urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0");
    content_root.append_attribute("xmlns:xlink").set_value("http://www.w3.org/1999/xlink");
    content_root.append_attribute("office:version").set_value("1.2");

    pugi::xml_node auto_styles = content_root.append_child("office:automatic-styles");
    AppendOdtAutomaticStyles(auto_styles);

    pugi::xml_node body_el = content_root.append_child("office:body");
    pugi::xml_node text_body = body_el.append_child("office:text");
    if (root) {
        WalkOdtBlockChildren(root->children, ctx, text_body);
    }

    std::ostringstream content_ss;
    content_doc.save(content_ss, "", pugi::format_raw);
    std::string content_xml = content_ss.str();
    std::string styles_xml = BuildOdtStylesXml();
    std::string meta_xml = BuildOdtMetaXml(title, author);
    std::string manifest_xml = BuildOdtManifestXml(ctx.images);
    // "mimetype" must be the first entry and stored uncompressed -- real
    // ODF readers/validators rely on this for fast format sniffing
    // without inflating anything (see BuildZipArchive's own header for
    // why this whole archive is hand-written rather than built via
    // miniz's own zip-writer API).
    std::vector<ZipEntryToWrite> zip_entries = {
        {"mimetype", "application/vnd.oasis.opendocument.text", true},
        {"META-INF/manifest.xml", manifest_xml, false},
        {"content.xml", content_xml, false},
        {"styles.xml", styles_xml, false},
        {"meta.xml", meta_xml, false},
    };
    for (const OdtImage &img : ctx.images) zip_entries.push_back({img.zip_name, img.bytes, true});
    std::string archive = BuildZipArchive(zip_entries);

    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
        error = "failed to write " + out_path;
        return false;
    }
    out.write(archive.data(), static_cast<std::streamsize>(archive.size()));
    return true;
}
