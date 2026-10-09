#include "doc_export.h"
#include "html_doc.h"
#include "image_doc.h"
#include "math_speech.h"
#include "mepml_doc.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xml_doc.h"
#include "zip_archive.h"

import mep.path_util;

namespace {

// --- shared DOM-walk helpers -----------------------------------------------

// Same resolution rule as main.cpp's own ResolveHtmlImagePath (the
// in-pane browser's <img> loader) -- kept as a separate copy rather than
// a shared header function since that one lives in main.cpp (raylib-
// linked) and this file deliberately isn't.
/**
 * @brief Resolves an <img src="..."> value to a local filesystem path usable to read the image, or "" if it isn't a readable local reference.
 * @param src The raw src attribute value (may be a remote URL, a file: URI, an absolute path, or a path relative to `base_dir`).
 * @param base_dir Base directory a relative `src` is resolved against.
 * @return The resolved local path, or an empty string for http(s) URLs.
 */
std::string ResolveLocalPath(const std::string &src, const std::string &base_dir) {
    if (src.empty()) return "";
    if (src.compare(0, 7, "http://") == 0 || src.compare(0, 8, "https://") == 0) return "";
    std::string s = src;
    if (s.compare(0, 5, "file:") == 0) s = s.substr(5);
    if (!s.empty() && s[0] == '/') return s;
    if (base_dir.empty()) return s;
    return base_dir + "/" + s;
}

/**
 * @brief Reads the entire contents of a binary file into `out`.
 * @param path Filesystem path of the file to read.
 * @param out Set to the file's raw bytes on success.
 * @return True if the file was opened and read; false if it could not be opened.
 */
bool ReadFileBytes(const std::string &path, std::string &out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

/**
 * @brief Recursively concatenates all Text-node content under `node`, ignoring element structure.
 * @param node DOM node whose descendant text is collected.
 * @return The concatenated raw text.
 */
std::string CollectRawText(const DomNode *node) {
    std::string out;
    for (auto &c : node->children) {
        if (c->type == DomNodeType::Text) out += c->text;
        else out += CollectRawText(c.get());
    }
    return out;
}

/**
 * @brief Checks whether `tag` is an HTML heading tag ("h1".."h6") and, if so, extracts its level.
 * @param tag Lowercase tag name to test.
 * @param level Set to the heading level (1-6) when this returns true; left unchanged otherwise.
 * @return True if `tag` is a heading tag.
 */
bool IsHeadingTag(const std::string &tag, int &level) {
    if (tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6') {
        level = tag[1] - '0';
        return true;
    }
    return false;
}

/**
 * @brief Checks whether a <math> node is a display-mode (block) equation rather than inline.
 * @param node The <math> DOM node to inspect.
 * @return True if the node's "display" attribute is "1".
 */
bool IsMathDisplay(const DomNode *node) {
    auto it = node->attrs.find("display");
    return it != node->attrs.end() && it->second == "1";
}

// table/tr/td/th may have thead/tbody/tfoot wrappers in between (both
// backends' own table walkers need the flat row list either way) --
// collected once here rather than duplicated per backend.
/**
 * @brief Recursively collects every <tr> descendant of `node` into a flat list, looking through any thead/tbody/tfoot wrappers.
 * @param node Table (or table-section) DOM node to search.
 * @param rows Appended with a pointer to each <tr> found, in document order.
 */
void CollectTableRows(const DomNode *node, std::vector<const DomNode *> &rows) {
    for (auto &c : node->children) {
        if (c->tag == "tr") rows.push_back(c.get());
        else if (c->type == DomNodeType::Element)
            CollectTableRows(c.get(), rows);
    }
}

/**
 * @brief Computes the widest row (by td/th count) across a table's rows.
 * @param rows The table's flat row list (as produced by CollectTableRows).
 * @return The maximum number of td/th cells found in any single row.
 */
size_t TableMaxCols(const std::vector<const DomNode *> &rows) {
    size_t max_cols = 0;
    for (const DomNode *r : rows) {
        size_t n = 0;
        for (const auto &c : r->children) {
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
/**
 * @brief Unwraps a full <html>/<body>-wrapped document down to its <body>; a no-op for a bare fragment.
 * @param root The parsed document's root DOM node.
 * @return The <body> node when a full document was passed; otherwise `root` unchanged (including when `root` is null).
 */
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

/**
 * @brief Escapes prose text for safe inclusion in LaTeX source, converting every LaTeX-special character to its literal-printing command.
 * @param s Raw (unescaped) text.
 * @return The LaTeX-escaped text.
 */
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
/**
 * @brief Escapes text for a fancyvrb Verbatim block using commandchars, escaping only backslash and the two brace characters that Verbatim's commandchars mode reserves.
 * @param s Raw (unescaped) code/verbatim text.
 * @return The Verbatim-safe escaped text.
 */
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

/**
 * @brief Finds the first direct child element of `node` with the given tag name.
 * @param node Parent DOM node whose direct children are searched.
 * @param tag Tag name to match.
 * @return Pointer to the first matching child, or nullptr if none is found.
 */
const DomNode *FindChildTag(const DomNode *node, const std::string &tag) {
    for (auto &c : node->children) {
        if (c->type == DomNodeType::Element && c->tag == tag) return c.get();
    }
    return nullptr;
}

// `text` as a PDF text string for a \special: UTF-16BE in hex behind a
// byte-order mark, so nothing in it can be read as TeX or as PDF syntax.
std::string PdfHexText(const std::string &text) {
    static const char *kHex = "0123456789ABCDEF";
    std::string out = "<FEFF";
    auto unit = [&](unsigned u) {
        for (int shift = 12; shift >= 0; shift -= 4) out += kHex[(u >> shift) & 0xF];
    };
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        unsigned cp = c;
        size_t n = 1;
        if (c >= 0xF0) cp = c & 0x07u, n = 4;
        else if (c >= 0xE0) cp = c & 0x0Fu, n = 3;
        else if (c >= 0xC0) cp = c & 0x1Fu, n = 2;
        if (i + n > text.size()) break;
        for (size_t k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3Fu);
        i += n;
        if (cp == '\n' || cp == '\r' || cp == '\t') cp = ' ';
        if (cp >= 0x10000) {
            cp -= 0x10000;
            unit(0xD800 + (cp >> 10));
            unit(0xDC00 + (cp & 0x3FF));
        } else {
            unit(cp);
        }
    }
    return out + ">";
}

// Tagged PDF (ISO 32000-1 §14.7-14.8): what makes the PDF readable by a
// screen reader. The LaTeX this file writes says what every piece of the
// page is -- a heading, a paragraph, a table cell, a figure and the text
// that stands for it, a formula and how it is read -- through three
// macros the preamble defines over xdvipdfmx's specials (kLatexTagging):
//
//   \mepS{id}{parent}{Type}{entries}  a structure element (`entries` its
//                                     /Alt and the like), child of `parent`
//   \mepM{id}{Type} ... \mepE{}       content of element `id` -- a
//                                     marked-content sequence; TeX may
//                                     break it over a page, where it is
//                                     closed and opened again
//   \mepMb{id}{Type} ... \mepEb{}     the same inside a box TeX never
//                                     breaks (a table cell, a display)
//
// The ids are this walker's own, counted from 1 (0 is the Document), so
// the macros need no state of their own about what is open. Compiled by
// anything but XeTeX (tectonic's engine) they do nothing.
struct LatexTagger {
    bool on = false;
    int next_id = 1;
    struct Open {
        int id;
        std::string type;
        bool implied;  // a paragraph nothing in the HTML asked for: loose text's
    };
    std::vector<Open> open;  // innermost last; the Document itself is not on it
    bool mc = false;         // content of open.back() is being written
    bool mc_boxed = false;
    int boxed = 0;           // inside a box TeX never breaks over a page

    // Whether an element of this type holds text itself (the rest hold
    // only other elements: text straight inside one gets a paragraph).
    static bool HoldsText(const std::string &t) {
        static const char *kTypes[] = {"P", "H1", "H2", "H3", "H4", "H5", "H6", "LBody", "TD", "TH", "Caption",
                                       "Lbl", "Link", "Formula", "Code", "Title", "Note", "Span"};
        return std::any_of(std::begin(kTypes), std::end(kTypes), [&](const char *k) { return t == k; });
    }
    void CloseMc(std::string &out) {
        if (!mc) return;
        out += mc_boxed ? "\\mepEb{}" : "\\mepE{}";
        mc = false;
    }
    // Opens an element under the innermost open one; End(out, depth) with
    // what this returns closes it (and any implied paragraph inside it).
    size_t Begin(std::string &out, const std::string &type, const std::string &entries = "", bool implied = false) {
        if (!on) return 0;
        CloseMc(out);
        const size_t depth = open.size();
        const int id = next_id++;
        out += "\\mepS{" + std::to_string(id) + "}{" + std::to_string(open.empty() ? 0 : open.back().id) + "}{" + type + "}{" +
               entries + "}";
        open.push_back({id, type, implied});
        return depth;
    }
    // Before a block of the page: the text before it is over.
    void Block(std::string &out) {
        if (!on) return;
        CloseMc(out);
        while (!open.empty() && open.back().implied) open.pop_back();
    }
    size_t BeginBlock(std::string &out, const std::string &type, const std::string &entries = "") {
        Block(out);
        return Begin(out, type, entries);
    }
    // An element inside a line of text (a link, a formula).
    size_t BeginInline(std::string &out, const std::string &type, const std::string &entries = "") {
        if (!on) return 0;
        // (An implied paragraph stays open after the element: the text
        // that follows it on the line is the paragraph's too.)
        if (open.empty() || !HoldsText(open.back().type)) Begin(out, "P", "", true);
        return Begin(out, type, entries);
    }
    void End(std::string &out, size_t depth) {
        if (!on) return;
        CloseMc(out);
        if (open.size() > depth) open.resize(depth);
    }
    int Id() const { return open.empty() ? 0 : open.back().id; }
    // Before text: it is content of the innermost element.
    void Text(std::string &out) {
        if (!on || mc) return;
        if (open.empty() || !HoldsText(open.back().type)) Begin(out, "P", "", true);
        mc_boxed = boxed > 0;
        out += std::string(mc_boxed ? "\\mepMb{" : "\\mepM{") + std::to_string(open.back().id) + "}{" + open.back().type + "}";
        mc = true;
    }
    // Content of the innermost element set in a box of its own (a
    // picture, a display): Boxed(out) ... CloseMc(out).
    void Boxed(std::string &out) {
        if (!on || open.empty()) return;
        CloseMc(out);
        mc_boxed = true;
        out += "\\mepMb{" + std::to_string(open.back().id) + "}{" + open.back().type + "}";
        mc = true;
    }
    // Something drawn that says nothing (a rule, a decorative picture):
    // set while no content is open, it is an artifact of the page.
    std::string Artifact(const std::string &tex) const { return on ? "\\mepA{" + tex + "}" : tex; }
};

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
/**
 * @brief Renders an org-code-block <div> (with its highlighted <span class="tok-x"> children) as a bordered/headered tcolorbox around a fancyvrb Verbatim, appending the result to `out`.
 * @param div_node The <div class="org-code-block"> node to render.
 * @param out String the rendered LaTeX is appended to.
 */
void RenderOrgCodeBlockLatex(const DomNode *div_node, LatexTagger &tg, std::string &out) {
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
            if (tok.empty()) {
                body += text;
            } else {
                body += "\\mepc{";
                body += tok;
                body += "}{";
                body += text;
                body += "}";
            }
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
    out += "\n";
    const size_t el = tg.BeginBlock(out, "Code");
    const std::string lines = tg.on ? "\\mepV{" + std::to_string(tg.Id()) + "}" : "";
    tg.End(out, el);
    out += "\\begin{mepcodebox}{" + LatexEscape(lang.empty() ? "text" : lang) + "}\n\\color{mepCodeFg}\n" + lines +
           "\\begin{Verbatim}[commandchars=\\\\\\{\\}]\n" + body + "\n\\end{Verbatim}\n\\end{mepcodebox}\n";
}

struct LatexCtx {
    std::string base_dir;
    // Inside a Beamer frame: no sectioning (headings are bold lines),
    // tabulars rather than longtables, pictures sized to the frame,
    // callouts as blocks, and a figure's caption under it.
    bool beamer = false;
    // How many mepml columns (\columns) the walk is inside: a picture there
    // is set in its column, not floated.
    int columns = 0;
    LatexTagger tag;
};

bool IsBlank(const std::string &s) {
    return std::all_of(s.begin(), s.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)); });
}

// The text that stands for a formula: the aria-label of the element
// mepml::ToHtml (or any page) wraps it in.
std::string MathLabel(const DomNode *node) {
    int hops = 0;
    for (const DomNode *n = node; n && hops < 3; n = n->parent, ++hops) {
        const auto role = n->attrs.find("role");
        const auto label = n->attrs.find("aria-label");
        if (label != n->attrs.end() && (n == node || (role != n->attrs.end() && role->second == "math"))) return label->second;
    }
    return "";
}

std::string AttrOr(const DomNode *node, const char *name, const std::string &fallback = "") {
    const auto it = node->attrs.find(name);
    return it == node->attrs.end() ? fallback : it->second;
}

// A page's own language: <html lang="...">.
std::string HtmlLang(const DomNode *root) {
    if (!root) return "";
    for (auto &c : root->children)
        if (c->type == DomNodeType::Element && c->tag == "html") return AttrOr(c.get(), "lang");
    return "";
}

std::string TrimBlanks(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

/**
 * @brief Recursively walks one DOM node and its descendants, appending the equivalent LaTeX markup to `out`.
 * @param node DOM node (element or text) to render.
 * @param ctx Shared LaTeX rendering context (currently just the image base directory).
 * @param out String the rendered LaTeX is appended to.
 */
void WalkLatexNode(const DomNode *node, LatexCtx &ctx, std::string &out) {
    LatexTagger &tg = ctx.tag;
    if (node->type == DomNodeType::Text) {
        // (Blanks between blocks are not text of any element.)
        if (!IsBlank(node->text)) tg.Text(out);
        out += LatexEscape(node->text);
        return;
    }
    const std::string &tag = node->tag;
    if (tag == "script" || tag == "style" || tag == "head" || tag == "title") return;
    // An element that is a block of the page ends the line of text before it.
    {
        static const char *kBlocks[] = {"p",  "div", "section", "figure", "figcaption", "table", "ul",      "ol",  "li",
                                        "blockquote", "pre", "hr", "h1", "h2", "h3", "h4", "h5", "h6", "nav", "caption",
                                        "article", "header", "footer", "aside", "dl", "dt", "dd"};
        if (std::any_of(std::begin(kBlocks), std::end(kBlocks), [&](const char *b) { return tag == b; })) tg.Block(out);
    }
    // A mepml \raw(tex, ...) (mepml::ToHtml wraps a \raw that is not for
    // HTML in a mep-raw element): its text is LaTeX, written as it is.
    if ((tag == "span" || tag == "div") && node->Class() == "mep-raw") {
        const auto it = node->attrs.find("data-raw");
        const std::string raw = it == node->attrs.end() ? CollectRawText(node) : it->second;
        if (tag == "span" && !IsBlank(raw)) tg.Text(out);
        out += tag == "div" ? "\n" + raw + "\n" : raw;
        return;
    }

    int level;
    if (ctx.beamer && IsHeadingTag(tag, level)) {
        out += "\n\\par";
        const size_t el = tg.BeginBlock(out, "H" + std::to_string(std::min(level + 1, 6)));
        tg.Text(out);
        out += level <= 2 ? "{\\large\\bfseries " : "{\\bfseries ";
        for (auto &c : node->children) WalkLatexNode(c.get(), ctx, out);
        out += "}";
        tg.End(out, el);
        out += "\\par\\smallskip\n";
        return;
    }
    if (ctx.beamer) {
        const std::string cls = node->Class();
        // A code block's language label says nothing on a slide.
        if (tag == "figcaption" && cls == "lang") return;
        if (tag == "figcaption" || ((tag == "div" || tag == "p") && cls == "caption")) {
            out += "\n\\par{\\centering\\footnotesize\\color{mepCodeMuted}";
            const size_t el = tg.BeginBlock(out, "Caption");
            for (auto &c : node->children) WalkLatexNode(c.get(), ctx, out);
            tg.End(out, el);
            out += "\\par}\n";
            return;
        }
        // mepml's callouts (`// NOTE: ...`): a titled block.
        if (tag == "div" && cls.compare(0, 8, "callout ") == 0) {
            // (Its title is set in a box of the block's own.)
            std::string open_el, title, text;
            const size_t el = tg.BeginBlock(open_el, "Note");
            for (auto &c : node->children) {
                if (c->type != DomNodeType::Element || c->Class() != "callout-title") continue;
                ++tg.boxed;
                WalkLatexNode(c.get(), ctx, title);
                tg.CloseMc(title);
                --tg.boxed;
            }
            for (auto &c : node->children)
                if (c->type != DomNodeType::Element || c->Class() != "callout-title") WalkLatexNode(c.get(), ctx, text);
            tg.End(text, el);
            out += "\n" + open_el + "\\begin{block}{" + title + "}\n" + text + "\n\\end{block}\n";
            return;
        }
        if (tag == "pre") {
            std::string raw = CollectRawText(node);
            if (!raw.empty() && raw.front() == '\n') raw.erase(raw.begin());
            while (!raw.empty() && raw.back() == '\n') raw.pop_back();
            const size_t el = tg.BeginBlock(out, "Code");
            if (tg.on) out += "\\mepV{" + std::to_string(tg.Id()) + "}";
            tg.End(out, el);
            out += "\n\\begin{Verbatim}[fontsize=\\footnotesize,frame=single,rulecolor=\\color{mepCodeBorder}" +
                   std::string(cls == "results" ? ",formatcom=\\color{mepCodeMuted}" : "") +
                   ",commandchars=\\\\\\{\\}]\n" + LatexEscapeVerbatim(raw) + "\n\\end{Verbatim}\n";
            return;
        }
    }
    if (IsHeadingTag(tag, level)) {
        // article class only goes 5 deep (section..subparagraph) --
        // clamp h5/h6 both to subparagraph rather than erroring.
        static const char *kCmds[] = {"section", "section",       "subsection",    "subsubsection",
                                       "paragraph", "subparagraph", "subparagraph"};
        // Tagged, the heading's content starts at its number (\mepH leaves
        // it for the number or the title to open, whichever is set
        // first), and the contents line and the bookmark take the title
        // as plain text.
        const size_t el = tg.BeginBlock(out, "H" + std::to_string(std::clamp(level, 1, 6)));
        std::string title;
        if (tg.on) {
            out += "\\mepH{" + std::to_string(tg.Id()) + "}{" + tg.open.back().type + "}";
            title = "\\mepHs{}";
            tg.mc = true;
            tg.mc_boxed = false;
        }
        for (auto &c : node->children) WalkLatexNode(c.get(), ctx, title);
        tg.End(title, el);
        out += "\n\\" + std::string(kCmds[std::min(level, 6)]) +
               (tg.on ? "[{" + LatexEscape(TrimBlanks(CollectRawText(node))) + "}]" : "") + "{" + title + "}\n";
        return;
    }
    if (tag == "math") {
        std::string latex = CollectRawText(node);
        // A formula is read as its \alttext; one without, as its TeX said
        // aloud ("theta hat 1 equals a over b": math_speech.h).
        const std::string label = MathLabel(node);
        std::string spoken = label.empty() ? mathspeech::Speak(latex) : label;
        if (spoken.empty()) spoken = TrimBlanks(latex);
        const std::string entries = "/Alt " + PdfHexText(spoken);
        if (IsMathDisplay(node)) {
            const size_t el = tg.Begin(out, "Formula", entries);
            out += "\n\\[";
            tg.Boxed(out);
            out += latex;
            tg.End(out, el);
            out += "\\]\n";
        } else {
            const size_t el = tg.BeginInline(out, "Formula", entries);
            tg.Text(out);
            out += "$" + latex + "$";
            tg.End(out, el);
        }
        return;
    }
    // A mepml @abstract (mepml::ToHtml): LaTeX's own abstract, whose
    // environment prints the "Abstract" heading itself.
    if ((tag == "section" || tag == "div") && node->Class() == "abstract") {
        out += "\n";
        const size_t el = tg.BeginBlock(out, "Sect");
        if (tg.on) {
            // (The environment sets its heading itself.)
            const size_t head = tg.Begin(out, "H2");
            const std::string id = std::to_string(tg.Id());
            tg.End(out, head);
            out += "\\renewcommand{\\abstractname}{\\mepM{" + id + "}{H2}Abstract\\mepE{}}";
        }
        out += "\\begin{abstract}\n";
        for (auto &c : node->children)
            if (c->Class() != "abstract-title") WalkLatexNode(c.get(), ctx, out);
        tg.End(out, el);
        out += "\\end{abstract}\n";
        return;
    }
    // mepml's tabs (\tabs / \tab, mepml::ToHtml): paper shows one tab no
    // better than all of them, so each tab's content is set in turn under
    // its title in bold; the radio buttons are the page's alone.
    if (tag == "div" && node->Class() == "mtabs") {
        out += "\n";
        for (auto &c : node->children) {
            if (c->type != DomNodeType::Element || c->tag == "input") continue;
            if (c->Class() == "mtab-label") {
                // (One line of the document, tagged as the box titles are.)
                std::string head, title, title_end;
                const size_t line = tg.Begin(head, "P");
                tg.Text(head);
                for (auto &k : c->children) WalkLatexNode(k.get(), ctx, title);
                tg.End(title_end, line);
                out += "\n\\par\\medskip\\noindent" + head + "\\textbf{" + title + "}" + title_end + "\\par\\nopagebreak\\smallskip\n";
            } else if (c->Class() == "mtab") {
                for (auto &k : c->children) WalkLatexNode(k.get(), ctx, out);
            } else {
                WalkLatexNode(c.get(), ctx, out);
            }
        }
        out += "\\par\\medskip\n";
        return;
    }
    // mepml's columns (\columns / \column, mepml::ToHtml): Beamer's own on a
    // slide, minipages side by side in an article. A column is as wide as
    // it says (data-width, a percentage); the rest share what is left.
    if (tag == "div" && node->Class() == "mcols") {
        std::vector<const DomNode *> cols;
        std::vector<double> widths;
        double named = 0;
        int unnamed = 0;
        for (auto &c : node->children) {
            if (c->type != DomNodeType::Element || c->Class() != "mcol") {
                // (Anything written between the columns comes before them.)
                WalkLatexNode(c.get(), ctx, out);
                continue;
            }
            const double w = c->attrs.count("data-width") ? std::atof(c->attrs.at("data-width").c_str()) : 0.0;
            cols.push_back(c.get());
            widths.push_back(w);
            named += w;
            unnamed += w > 0 ? 0 : 1;
        }
        if (cols.empty()) return;
        const double share = unnamed > 0 ? std::max(5.0, (100.0 - named) / unnamed) : 0.0;
        double total = 0;
        for (double &w : widths) {
            if (w <= 0) w = share;
            total += w;
        }
        // (A gap of 4% of the line between neighbours.)
        const double usable = 1.0 - 0.04 * static_cast<double>(cols.size() - 1);
        out += ctx.beamer ? "\n\\begin{columns}[T,onlytextwidth]\n" : "\n\\par\\medskip\\noindent\n";
        ++ctx.columns;
        for (size_t i = 0; i < cols.size(); ++i) {
            char frac[32];
            std::snprintf(frac, sizeof(frac), "%.3f", widths[i] / total * usable);
            out += ctx.beamer ? "\\begin{column}{" + std::string(frac) + "\\textwidth}\n"
                              : "\\begin{minipage}[t]{" + std::string(frac) + "\\linewidth}\n";
            for (auto &c : cols[i]->children) WalkLatexNode(c.get(), ctx, out);
            out += ctx.beamer ? "\n\\end{column}\n" : i + 1 < cols.size() ? "\n\\end{minipage}\\hfill\n" : "\n\\end{minipage}\\par\\medskip\n";
        }
        --ctx.columns;
        if (ctx.beamer) out += "\\end{columns}\n";
        return;
    }
    // A mepml box (\definition and its kin, mepml::ToHtml): a tcolorbox in
    // the kind's colours, its label in small capitals before the title.
    if (tag == "div" && node->attrs.count("data-kind") && node->Class().rfind("mbox", 0) == 0) {
        const std::string kind = node->attrs.at("data-kind");
        std::string colour = mepml::FindBoxKind(kind) ? "mepbox" + kind : "mepboxdefinition";
        // Colours of the document's own (its style sheets': mepml::ToHtml
        // writes them on the box): defined here, where the box is.
        auto hex6 = [](const std::string &v) {
            return v.size() == 6 && std::all_of(v.begin(), v.end(), [](char ch) { return std::isxdigit(static_cast<unsigned char>(ch)); });
        };
        if (node->attrs.count("data-color") && node->attrs.count("data-tint") && hex6(node->attrs.at("data-color")) &&
            hex6(node->attrs.at("data-tint"))) {
            std::string c = node->attrs.at("data-color"), t = node->attrs.at("data-tint");
            for (char &ch : c) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            for (char &ch : t) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            colour = "mepboxc" + c + "t" + t;
            out += "\n\\definecolor{" + colour + "}{HTML}{" + c + "}\\definecolor{" + colour + "tint}{HTML}{" + t + "}";
        }
        // What closes it: a proof's tombstone, or the sheets' own mark.
        // (It says nothing a reader needs: an artifact of the page.)
        std::string end_mark = kind == "proof" ? "\\hfill" + tg.Artifact("\\ensuremath{\\blacksquare}") + "\n" : "";
        if (node->attrs.count("data-end")) {
            const std::string &e = node->attrs.at("data-end");
            // (The two squares are maths symbols; anything else is text.)
            end_mark = e.empty()                  ? std::string()
                       : e == "\xE2\x88\x8E" ? "\\hfill" + tg.Artifact("\\ensuremath{\\blacksquare}") + "\n"
                       : e == "\xE2\x96\xA1" ? "\\hfill" + tg.Artifact("\\ensuremath{\\square}") + "\n"
                                              : "\\hfill " + tg.Artifact(LatexEscape(e)) + "\n";
        }
        // The box is a section of the document; its label and title are
        // one line of it, its first.
        std::string open_el, head, label, name, name_end, body;
        const size_t el = tg.BeginBlock(open_el, "Sect");
        for (auto &c : node->children) {
            if (c->type == DomNodeType::Element && c->Class() == "mbox-title") {
                const size_t line = tg.Begin(head, "P");
                tg.Text(head);
                for (auto &t : c->children) {
                    if (t->type != DomNodeType::Element) continue;
                    if (t->Class() == "mbox-label") label = LatexEscape(CollectRawText(t.get()));
                    else if (t->Class() == "mbox-name")
                        for (auto &k : t->children) WalkLatexNode(k.get(), ctx, name);
                }
                tg.End(name_end, line);
            } else {
                WalkLatexNode(c.get(), ctx, body);
            }
        }
        tg.End(body, el);
        out += "\n" + open_el + "\\begin{mepbox}{" + colour + "}\n" + head + "{\\sffamily\\bfseries\\footnotesize\\color{" + colour +
               "}\\MakeUppercase{" + label + "}}" + (name.empty() ? "" : "\\enspace\\textbf{" + name + "}") + name_end +
               "\\par\\smallskip\n" + body + end_mark + "\\end{mepbox}\n";
        return;
    }
    if (tag == "div" && node->Class() == "org-code-block") {
        RenderOrgCodeBlockLatex(node, tg, out);
        return;
    }
    if (tag == "p") {
        out += "\n";
        const size_t el = tg.BeginBlock(out, node->Class() == "caption" ? "Caption" : "P");
        for (auto &c : node->children) WalkLatexNode(c.get(), ctx, out);
        tg.End(out, el);
        out += "\n\n";
        return;
    }
    if (tag == "figcaption" || tag == "caption" || (tag == "div" && node->Class() == "caption")) {
        const size_t el = tg.BeginBlock(out, "Caption");
        for (auto &c : node->children) WalkLatexNode(c.get(), ctx, out);
        tg.End(out, el);
        return;
    }
    if (tag == "br") {
        out += " \\\\\n";
        return;
    }
    // A task list's box (mepml's `- [ ]` / `- [x]`).
    if (tag == "input" && node->attrs.count("type") && node->attrs.at("type") == "checkbox") {
        tg.Text(out);
        out += node->attrs.count("checked") ? "$\\boxtimes$ " : "$\\square$ ";
        return;
    }
    if (tag == "hr") {
        out += "\n\\par\\noindent" + tg.Artifact("\\rule{\\linewidth}{0.4pt}") + "\\par\n";
        return;
    }
    if (tag == "b" || tag == "strong") {
        tg.Text(out);
        out += "\\textbf{";
        for (auto &c : node->children) WalkLatexNode(c.get(), ctx, out);
        out += "}";
        return;
    }
    if (tag == "i" || tag == "em") {
        tg.Text(out);
        out += "\\textit{";
        for (auto &c : node->children) WalkLatexNode(c.get(), ctx, out);
        out += "}";
        return;
    }
    if (tag == "u") {
        tg.Text(out);
        out += "\\underline{";
        for (auto &c : node->children) WalkLatexNode(c.get(), ctx, out);
        out += "}";
        return;
    }
    if (tag == "s" || tag == "strike" || tag == "del") {
        tg.Text(out);
        out += "\\sout{";
        for (auto &c : node->children) WalkLatexNode(c.get(), ctx, out);
        out += "}";
        return;
    }
    if (tag == "code" || tag == "tt") {
        tg.Text(out);
        out += "\\texttt{" + LatexEscape(CollectRawText(node)) + "}";
        return;
    }
    // What a mepml style sheet changes for a run of text (mepml::ToHtml
    // marks it when the export is LaTeX's): its colour, weight and slant.
    if (tag == "span" && node->Class() == "mep-style") {
        tg.Text(out);
        std::string open = "{";
        auto attr = [&](const char *name) { return node->attrs.count(name) ? node->attrs.at(name) : std::string(); };
        std::string c = attr("data-color");
        if (c.size() == 6 && std::all_of(c.begin(), c.end(), [](char ch) { return std::isxdigit(static_cast<unsigned char>(ch)); })) {
            for (char &ch : c) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            open += "\\color[HTML]{" + c + "}";
        }
        if (attr("data-bold") == "1") open += "\\bfseries";
        else if (attr("data-bold") == "0") open += "\\mdseries";
        if (attr("data-italic") == "1") open += "\\itshape";
        else if (attr("data-italic") == "0") open += "\\upshape";
        out += open + (open.size() > 1 ? " " : "");
        for (auto &c2 : node->children) WalkLatexNode(c2.get(), ctx, out);
        out += "}";
        return;
    }
    if (tag == "span" && node->Class().compare(0, 3, "hl-") == 0) {
        // <span class="hl-<color>">: kBuiltinOrgNotes' {{{hl(color,text)}}}
        // highlight macro, expanded by the HTML export pass (main.cpp,
        // mep.org_export_marks.html.hl). Colored + bold, matching the
        // .hl-* CSS rules and the in-editor render. Unknown color names
        // fall through to the generic-container recursion below (an
        // undefined LaTeX color would abort the whole compile).
        std::string color = node->Class().substr(3);
        if (color == "red" || color == "orange" || color == "yellow" || color == "green" ||
            color == "cyan" || color == "blue" || color == "purple") {
            tg.Text(out);
            out += "\\textcolor{mephl" + color + "}{\\textbf{";
            for (auto &c : node->children) WalkLatexNode(c.get(), ctx, out);
            out += "}}";
            return;
        }
    }
    if (tag == "pre") {
        // <pre class="mermaid"> (kBuiltinOrgExport's mermaid diagram
        // form): there is no LaTeX-side mermaid renderer -- mermaid is
        // JS, and rasterizing would need the mmdc CLI, which the devshell
        // doesn't carry -- so the diagram source ships as the same titled
        // code box org code blocks use instead of silently disappearing.
        if (node->Class() == "mermaid") {
            std::string src = LatexEscapeVerbatim(CollectRawText(node));
            if (!src.empty() && src.front() == '\n') src.erase(src.begin());
            out += "\n";
            const size_t el = tg.BeginBlock(out, "Code");
            const std::string lines = tg.on ? "\\mepV{" + std::to_string(tg.Id()) + "}" : "";
            tg.End(out, el);
            out += "\\begin{mepcodebox}{mermaid}\n\\color{mepCodeFg}\n" + lines + "\\begin{Verbatim}[commandchars=\\\\\\{\\}]\n" +
                   src + "\n\\end{Verbatim}\n\\end{mepcodebox}\n";
            return;
        }
        std::string raw = CollectRawText(node);
        if (!raw.empty() && raw.front() == '\n') raw.erase(raw.begin());
        if (tg.on) {
            // (fancyvrb's Verbatim sets a line at a time, so each can be
            // marked as the code's; LaTeX's own verbatim cannot.)
            out += "\n";
            const size_t el = tg.BeginBlock(out, "Code");
            out += "\\mepV{" + std::to_string(tg.Id()) + "}";
            tg.End(out, el);
            out += "\\begin{Verbatim}\n" + raw + "\n\\end{Verbatim}\n";
            return;
        }
        out += "\n\\begin{verbatim}\n" + raw + "\n\\end{verbatim}\n";
        return;
    }
    if (tag == "blockquote") {
        out += "\n";
        const size_t el = tg.BeginBlock(out, "BlockQuote");
        out += "\\begin{quote}\n";
        for (auto &c : node->children) WalkLatexNode(c.get(), ctx, out);
        tg.End(out, el);
        out += "\n\\end{quote}\n";
        return;
    }
    if (tag == "ul" || tag == "ol") {
        out += "\n";
        const size_t el = tg.BeginBlock(
            out, "L", tag == "ul" ? "/A << /O /List /ListNumbering /Disc >>" : "/A << /O /List /ListNumbering /Decimal >>");
        // (\mepLfix: the labels this list sets are its items' /Lbl.)
        out += std::string(tag == "ul" ? "\\begin{itemize}" : "\\begin{enumerate}") + (tg.on ? "\\mepLfix{}" : "") + "\n";
        for (auto &c : node->children) WalkLatexNode(c.get(), ctx, out);
        tg.End(out, el);
        out += tag == "ul" ? "\\end{itemize}\n" : "\\end{enumerate}\n";
        return;
    }
    if (tag == "li") {
        const size_t el = tg.BeginBlock(out, "LI");
        if (tg.on) {
            const size_t lbl = tg.Begin(out, "Lbl");
            out += "\\mepLbl{" + std::to_string(tg.Id()) + "}";
            tg.End(out, lbl);
        }
        out += "\\item ";
        tg.Begin(out, "LBody");
        for (auto &c : node->children) WalkLatexNode(c.get(), ctx, out);
        tg.End(out, el);
        out += "\n";
        return;
    }
    if (tag == "a") {
        auto it = node->attrs.find("href");
        // A link into the page itself has nowhere to go on paper: its text
        // stays, and a footnote's way back (to "#fnref...") goes.
        if (it != node->attrs.end() && !it->second.empty() && it->second[0] == '#') {
            if (it->second.rfind("#fnref", 0) == 0) return;
            for (auto &c : node->children) WalkLatexNode(c.get(), ctx, out);
            return;
        }
        const size_t el = tg.BeginInline(out, "Link");
        std::string inner;
        for (auto &c : node->children) WalkLatexNode(c.get(), ctx, inner);
        tg.End(inner, el);
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
        // A picture is a Figure read as its alt text; one whose alt text is
        // empty (alt="", mepml's `\alttext()`) only decorates the page, and
        // is no part of what is read.
        const bool decorative = node->attrs.count("alt") && IsBlank(node->attrs.at("alt"));
        auto picture = [&](const std::string &options) {
            const std::string tex = "\\includegraphics[" + options + "]{" + resolved + "}";
            if (decorative) return tg.Artifact(tex);
            std::string o;
            const std::string alt = AttrOr(node, "alt");
            const size_t el = tg.Begin(o, "Figure", alt.empty() ? "" : "/Alt " + PdfHexText(alt));
            tg.Boxed(o);
            o += tex;
            tg.End(o, el);
            return o;
        };
        tg.CloseMc(out);
        if (!resolved.empty() && probe && tg.boxed > 0) {
            // (In a table's cell: no float, no display.)
            out += picture("width=0.2\\linewidth,keepaspectratio");
        } else if (!resolved.empty() && probe && ctx.beamer) {
            out += "\n\\begin{center}" + picture("width=\\linewidth,height=0.62\\textheight,keepaspectratio") + "\\end{center}\n";
        } else if (!resolved.empty() && probe && ctx.columns > 0) {
            // (No float in a column's minipage.)
            out += "\n\\begin{center}" + picture("width=\\linewidth,keepaspectratio") + "\\end{center}\n";
        } else if (!resolved.empty() && probe) {
            // (Tagged, the figure stays where it is written -- [H] --: one
            // that floated would be read out of place.)
            out += std::string("\n\\begin{figure}[") + (tg.on ? "H" : "h") + "]\n\\centering\n" + picture("width=0.9\\linewidth") +
                   "\n\\end{figure}\n";
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
        // What the table shows, for a reader who cannot see it (mepml's
        // \alttext under a table): its /Summary.
        std::string summary = AttrOr(node, "aria-description");
        if (summary.empty()) summary = AttrOr(node, "summary");
        out += "\n";
        const size_t table_el =
            tg.BeginBlock(out, "Table", summary.empty() ? "" : "/A << /O /Table /Summary " + PdfHexText(summary) + " >>");
        // A table's <caption> goes above it.
        if (const DomNode *cap = FindChildTag(node, "caption")) {
            out += "\\par{\\centering\\footnotesize\\color{mepCodeMuted}";
            const size_t el = tg.Begin(out, "Caption");
            for (auto &c : cap->children) WalkLatexNode(c.get(), ctx, out);
            tg.End(out, el);
            out += "\\par}\n";
        }
        out += ctx.beamer ? "\\begin{center}\\small\\begin{tabular}{|" : "\\begin{longtable}{|";
        for (size_t i = 0; i < max_cols; i++) out += "l|";
        out += "}\n\\hline\n";
        ++tg.boxed;
        for (const DomNode *r : rows) {
            // A header cell heads its column in a row of them, its row otherwise.
            const bool header_row = std::all_of(r->children.begin(), r->children.end(), [](const auto &c) { return c->tag != "td"; });
            const size_t row_el = tg.Begin(out, "TR");
            bool first = true;
            for (const auto &c : r->children) {
                if (c->tag != "td" && c->tag != "th") continue;
                if (!first) out += " & ";
                first = false;
                const bool th = c->tag == "th";
                const size_t cell_el = tg.Begin(
                    out, th ? "TH" : "TD", th ? std::string("/A << /O /Table /Scope /") + (header_row ? "Column" : "Row") + " >>" : "");
                if (th) {
                    if (!IsBlank(CollectRawText(c.get()))) tg.Text(out);
                    out += "\\textbf{";
                }
                for (auto &gc : c->children) WalkLatexNode(gc.get(), ctx, out);
                if (th) out += "}";
                tg.End(out, cell_el);
            }
            tg.End(out, row_el);
            out += " \\\\\n\\hline\n";
        }
        --tg.boxed;
        tg.End(out, table_el);
        out += ctx.beamer ? "\\end{tabular}\\end{center}\n" : "\\end{longtable}\n";
        return;
    }
    // Unrecognized container (div/span/body/#document/...) -- recurse
    // with no wrapper, matching ParseHtml's own "unknown tag renders as
    // a generic container" tolerance.
    for (auto &c : node->children) WalkLatexNode(c.get(), ctx, out);
}

}  // namespace

namespace {
// The tagging macros (see LatexTagger), over xdvipdfmx's pdf: specials.
//
// A structure element is two objects, itself and the array of its kids
// (@mepseN, @mepkN). \mepS only queues things, so no special ever lands
// between two paragraphs, where it would hide the space LaTeX means to put
// there: the objects' definitions are written at the start of the next
// page shipped out -- before anything on a page can name them, in
// whatever order the page's boxes were set (Beamer sets a frame's title
// after its body, and puts it above) -- and the element is added to its
// parent's kids just before the next piece of content, so the kids are in
// the order they are on the page.
//
// A marked-content sequence may not run over a page, but a paragraph may:
// each \mepM leaves a mark saying what is open (\mepE one saying nothing
// is), and \@makecol -- where LaTeX makes a page's column -- closes what
// its last mark leaves open and opens it again at the top of the next. A
// breakable tcolorbox splits its own text, so its parts do the same from
// \splitbotmarks (the `meptagged` style).
//
// Everything on a page is either content of some element or an artifact
// (a table's rules, a box's frame, a page number): each page opens an
// /Artifact sequence at its start and closes it at its end, and a piece of
// real content closes it before itself and opens it again after, so the
// two never nest and nothing is left unmarked.
//
// MCIDs must count from 0 on every page, and TeX does not know the page
// a piece of text lands on until it is shipped out: every sequence writes
// its page to the .aux as it is shipped, the next run numbers from that,
// and from that too comes the parent tree (which element each MCID of
// each page belongs to), written on the last page.
const char *kLatexTagging = R"tex(\makeatletter
\newif\ifmep@tag \mep@tagfalse
\ifdefined\XeTeXversion \mep@tagtrue \fi
\newcount\mep@mc
\newcount\mep@p
\def\mep@pend{}
\def\mep@defs{}
\def\mep@open{}
\def\mep@tcbopen{}
\def\mep@dash{-}
\def\mep@lastpg{0}
\def\mep@hpend{}
\def\mep@lblid{}
\def\mep@mcpage#1#2{%
  \expandafter\ifx\csname mep@n@#2\endcsname\relax
    \expandafter\gdef\csname mep@n@#2\endcsname{0}\expandafter\gdef\csname mep@pg@#2\endcsname{}\fi
  \expandafter\xdef\csname mep@l@#1\endcsname{\csname mep@n@#2\endcsname}%
  \expandafter\xdef\csname mep@n@#2\endcsname{\the\numexpr\csname mep@n@#2\endcsname+1\relax}%
  \expandafter\g@addto@macro\csname mep@pg@#2\endcsname{\mep@pt{#1}}%
  \ifnum#2>\mep@lastpg\relax\xdef\mep@lastpg{#2}\fi}
\AtBeginDocument{\global\let\mep@mcpage\@gobbletwo}
\ifmep@tag
\newmarks\mep@marks
\def\mepS#1#2#3#4{\xdef\mep@defs{\mep@defs
  \special{pdf:obj @mepk#1 []}%
  \special{pdf:obj @mepse#1 << /Type /StructElem /S /#3 /P @mepse#2 /K @mepk#1 #4 >>}}%
  \xdef\mep@pend{\mep@pend\special{pdf:put @mepk#2 @mepse#1}}}
\def\mep@bdc#1#2{%
  \mep@pend\gdef\mep@pend{}%
  \global\advance\mep@mc\@ne
  \expandafter\xdef\csname mep@o@\the\mep@mc\endcsname{#1}%
  \edef\mep@id{\ifcsname mep@l@\the\mep@mc\endcsname\csname mep@l@\the\mep@mc\endcsname\else\the\mep@mc\fi}%
  \special{pdf:code EMC /#2 <</MCID \mep@id>> BDC}%
  \special{pdf:put @mepk#1 << /Type /MCR /Pg @thispage /MCID \mep@id >>}%
  \if@filesw\edef\mep@w{\write\@auxout{\string\mep@mcpage{\the\mep@mc}{\noexpand\the\ReadonlyShipoutCounter}}}\mep@w\fi}
\def\mep@emc{\special{pdf:code EMC /Artifact BMC}}
\def\mepM#1#2{\leavevmode\mep@bdc{#1}{#2}\marks\mep@marks{{#1}{#2}}}
\def\mepMb#1#2{\mep@bdc{#1}{#2}}
\def\mepE{\mep@emc\marks\mep@marks{-}}
\def\mepEb{\mep@emc}
\def\mepA#1{#1}
\def\mepH#1#2{\gdef\mep@hpend{\mepM{#1}{#2}}}
\DeclareRobustCommand\mepHs{\mep@hpend\gdef\mep@hpend{}}
\def\@seccntformat#1{\mepHs\csname the#1\endcsname\quad}
\def\mepV#1{\def\FancyVerbFormatLine##1{\mepMb{#1}{Code}##1\mepEb}}
\def\mepLbl#1{\gdef\mep@lblid{#1}}
\def\mep@lbl#1{\ifx\mep@lblid\@empty#1\else\mepMb{\mep@lblid}{Lbl}\global\let\mep@lblid\@empty#1\mepEb\fi}
\def\mepLfix{\let\mep@oml\makelabel\def\makelabel##1{\mep@oml{\mep@lbl{##1}}}}
\def\mep@colopen{\ifx\mep@open\@empty\else\expandafter\mep@bdc\mep@open\fi}
\def\mep@colclose{\edef\mep@bot{\botmarks\mep@marks}%
  \ifx\mep@bot\mep@dash\global\let\mep@open\@empty\else\global\let\mep@open\mep@bot\fi
  \ifx\mep@open\@empty\else\mep@emc\fi}
\g@addto@macro\@makecol{\setbox\@outputbox\vbox to\@colht{\mep@colopen\unvbox\@outputbox\mep@colclose}}
\def\mep@tcbfinish{\edef\mep@bot{\splitbotmarks\mep@marks}%
  \ifx\mep@bot\@empty\else\ifx\mep@bot\mep@dash\global\let\mep@tcbopen\@empty\else\global\let\mep@tcbopen\mep@bot\fi\fi
  \ifx\mep@tcbopen\@empty\else\mep@emc\fi}
\def\mep@tcbstart{\ifx\mep@tcbopen\@empty\else\expandafter\mep@bdc\mep@tcbopen\fi}
\def\mep@tcblast{\global\let\mep@tcbopen\@empty}
\tcbset{meptagged/.style={extras first and middle={finish={\mep@tcbfinish}},
  extras middle and last={overlay={\mep@tcbstart}},extras last={finish={\mep@tcblast}}}}
\AddToHook{shipout/background}{\mep@defs\gdef\mep@defs{}%
  \special{pdf:put @thispage << /StructParents \the\numexpr\ReadonlyShipoutCounter-1\relax\space /Tabs /S >>}%
  \special{pdf:code /Artifact BMC}}
\AddToHook{shipout/foreground}{\special{pdf:code EMC}}
\def\mep@pt#1{ \ifcsname mep@o@#1\endcsname @mepse\csname mep@o@#1\endcsname\else null\fi}
\def\mep@parents{%
  \mep@p=\z@ \def\mep@nums{}%
  \loop\ifnum\mep@p<\mep@lastpg\relax \advance\mep@p\@ne
    \special{pdf:obj @meppt\the\mep@p\space [\ifcsname mep@pg@\the\mep@p\endcsname\csname mep@pg@\the\mep@p\endcsname\fi]}%
    \edef\mep@nums{\mep@nums\space\the\numexpr\mep@p-1\relax\space @meppt\the\mep@p}%
  \repeat
  \special{pdf:put @meproot << /ParentTree << /Nums [\mep@nums] >> /ParentTreeNextKey \mep@lastpg >>}}
\AddToHook{shipout/lastpage}{\mep@defs\gdef\mep@defs{}\mep@pend\gdef\mep@pend{}\mep@parents}
\def\mepDoc#1#2{\xdef\mep@defs{\special{pdf:obj @mepk0 []}%
    \special{pdf:obj @meproot << /Type /StructTreeRoot /RoleMap << /Title /P >> >>}%
    \special{pdf:obj @mepse0 << /Type /StructElem /S /Document /P @meproot /K @mepk0 >>}%
    \special{pdf:put @meproot << /K @mepse0 >>}%
    \special{pdf:put @catalog << /StructTreeRoot @meproot /MarkInfo << /Marked true >> #1 >>}#2}}
\else
\def\mepS#1#2#3#4{}\def\mepM#1#2{}\def\mepMb#1#2{}\def\mepE{}\def\mepEb{}\def\mepA#1{#1}
\def\mepH#1#2{}\def\mepHs{}\def\mepV#1{}\def\mepLbl#1{}\def\mepLfix{}\def\mepDoc#1#2{}
\fi
\makeatother
)tex";

// An article's page number, set where no content is open (so it is an
// artifact like the rest of the page's furniture) rather than in the
// middle of a paragraph that runs over the page.
const char *kLatexTaggingArticle = R"tex(\makeatletter
\ifmep@tag
\def\ps@plain{\let\@mkboth\@gobbletwo\let\@oddhead\@empty\let\@evenhead\@empty
  \def\@oddfoot{\mepA{\reset@font\hfil\thepage\hfil}}\let\@evenfoot\@oddfoot}
\pagestyle{plain}
\fi
\makeatother
)tex";

// \mepDoc for a document in `lang` called `title`: its language and title
// in the catalog and the document information, where a reader looks.
std::string LatexTaggingDocument(const std::string &lang, const std::string &title) {
    std::string tag;
    for (char c : lang)
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-') tag += c;
    std::string catalog, info;
    if (!tag.empty()) catalog += "/Lang (" + tag + ")";
    if (!title.empty()) {
        catalog += " /ViewerPreferences << /DisplayDocTitle true >>";
        info = "\\special{pdf:docinfo << /Title " + PdfHexText(title) + " >>}";
    }
    // The same as XMP metadata, where newer readers (and PDF/UA) look.
    auto xml = [](const std::string &text) {
        std::string o;
        for (char c : text) {
            if (c == '&') o += "&amp;";
            else if (c == '<') o += "&lt;";
            else if (c == '>') o += "&gt;";
            else o += c;
        }
        return o;
    };
    std::string xmp = "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
                      "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
                      "<rdf:Description rdf:about=\"\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\">";
    if (!title.empty()) xmp += "<dc:title><rdf:Alt><rdf:li xml:lang=\"x-default\">" + xml(title) + "</rdf:li></rdf:Alt></dc:title>";
    if (!tag.empty()) xmp += "<dc:language><rdf:Bag><rdf:li>" + tag + "</rdf:li></rdf:Bag></dc:language>";
    xmp += "</rdf:Description></rdf:RDF></x:xmpmeta>\n<?xpacket end=\"w\"?>";
    static const char *kHex = "0123456789ABCDEF";
    std::string hex;
    for (char c : xmp) {
        hex += kHex[(static_cast<unsigned char>(c) >> 4) & 0xF];
        hex += kHex[static_cast<unsigned char>(c) & 0xF];
    }
    info += "\\special{pdf:stream @mepxmp <" + hex + "> << /Type /Metadata /Subtype /XML >>}"
            "\\special{pdf:put @catalog << /Metadata @mepxmp >>}";
    return "\\mepDoc{" + catalog + "}{" + info + "}\n";
}

// The packages and definitions WalkLatexNode's output needs. Beamer
// brings hyperref itself and sizes its own pages; longtable is article-only.
void LatexPreamble(std::ostringstream &out, bool beamer) {
    if (beamer)
        // (usepdftitle=false: the PDF's own title is the plain one \mepDoc
        // writes, not the tagged \title.)
        out << "\\documentclass[11pt,aspectratio=169,usepdftitle=false]{beamer}\n"
            << "\\setbeamertemplate{navigation symbols}{}\n"
            << "\\setbeamertemplate{footline}[frame number]\n";
    else
        out << "\\documentclass[11pt]{article}\n";
    out << "\\usepackage[utf8]{inputenc}\n"
        << "\\usepackage[T1]{fontenc}\n"
        << "\\usepackage{graphicx}\n"
        << "\\usepackage[normalem]{ulem}\n"
        << "\\usepackage{amsmath}\n"
        << "\\usepackage{amssymb}\n"
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
        // mephl<color>: the {{{hl(color,text)}}} highlight macro's seven
        // colors (WalkLatexNode's span.hl-* branch) -- same hexes as the
        // meptok* palette / the HTML export's .hl-* CSS rules, so editor,
        // HTML, and PDF renders of a highlight all agree.
        << "\\definecolor{mephlred}{HTML}{CF222E}\n"
        << "\\definecolor{mephlorange}{HTML}{953800}\n"
        << "\\definecolor{mephlyellow}{HTML}{9A6700}\n"
        << "\\definecolor{mephlgreen}{HTML}{1A7F37}\n"
        << "\\definecolor{mephlcyan}{HTML}{0B7285}\n"
        << "\\definecolor{mephlblue}{HTML}{0550AE}\n"
        << "\\definecolor{mephlpurple}{HTML}{8250DF}\n"
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
    // mepml's boxes (\definition, \theorem ...): each kind's accent and
    // the paper behind it, from mepml::BoxKinds like the HTML's.
    auto hex = [](const char *c) {
        std::string h = c[0] == '#' ? c + 1 : c;
        for (char &ch : h) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        return h;
    };
    for (const mepml::BoxKind &k : mepml::BoxKinds())
        out << "\\definecolor{mepbox" << k.name << "}{HTML}{" << hex(k.color) << "}\n"
            << "\\definecolor{mepbox" << k.name << "tint}{HTML}{" << hex(k.tint) << "}\n";
    // (On a slide, room is short: smaller text and tighter spacing.)
    out << kLatexTagging;
    out << "\\newtcolorbox{mepbox}[1]{enhanced, breakable, meptagged, frame hidden, boxrule=0pt, arc=2pt, colback=#1tint, "
           "borderline west={3pt}{0pt}{#1}, "
        << (beamer ? "fontupper=\\small, left=6pt, right=6pt, top=2pt, bottom=3pt, before skip=4pt, after skip=4pt}\n"
                   : "left=8pt, right=8pt, top=5pt, bottom=6pt, before skip=8pt, after skip=8pt}\n");
    if (!beamer) out << "\\usepackage{longtable}\n\\usepackage{float}\n\\usepackage[margin=1in]{geometry}\n\\usepackage{hyperref}\n";
    if (!beamer) out << kLatexTaggingArticle;
}

}  // namespace

std::string ExportHtmlToLatex(const std::string &html, const std::string &title, const std::string &author,
                               const std::string &base_dir, const std::string &lang) {
    HtmlDoc doc;
    // The LaTeX walker reads tags, attributes and text, never a computed style.
    ParseHtml(html, doc, /*full_document=*/true, /*compute_styles=*/false);
    const DomNode *root = ContentRoot(doc.root.get());

    LatexCtx ctx;
    ctx.base_dir = base_dir;
    ctx.tag.on = true;
    // (\maketitle sets these first, whatever their numbers.)
    const int title_id = ctx.tag.next_id++, author_id = ctx.tag.next_id++;
    std::string body;
    if (root) {
        for (auto &c : root->children) WalkLatexNode(c.get(), ctx, body);
    }
    ctx.tag.Block(body);

    std::ostringstream out;
    LatexPreamble(out, false);
    out << LatexTaggingDocument(lang.empty() ? HtmlLang(doc.root.get()) : lang, title);
    auto tagged = [](int id, const char *type, const char *m, const char *e, const std::string &text) {
        const std::string n = std::to_string(id);
        return "\\mepS{" + n + "}{0}{" + type + "}{}\\" + m + "{" + n + "}{" + type + "}" + text + "\\" + e + "{}";
    };
    if (!title.empty()) out << "\\title{" << tagged(title_id, "Title", "mepM", "mepE", LatexEscape(title)) << "}\n";
    // (The author is set in a tabular: a box.)
    if (!author.empty()) out << "\\author{" << tagged(author_id, "P", "mepMb", "mepEb", LatexEscape(author)) << "}\n";
    out << "\\date{}\n\\begin{document}\n";
    if (!title.empty()) out << "\\maketitle\n";
    out << body << "\n\\end{document}\n";
    return out.str();
}

std::string ExportHtmlSlidesToBeamer(const std::vector<BeamerFrame> &frames, const std::string &title,
                                     const std::string &subtitle, const std::string &author, const std::string &date,
                                     const std::string &base_dir, const std::string &lang) {
    LatexCtx ctx;
    ctx.base_dir = base_dir;
    ctx.beamer = true;
    ctx.tag.on = true;
    // A fragment's own content, walked as the article backend walks a page.
    auto walk = [&](const std::string &html) {
        HtmlDoc doc;
        ParseHtml(html, doc, /*full_document=*/true, /*compute_styles=*/false);
        std::string out;
        if (const DomNode *root = ContentRoot(doc.root.get()))
            for (auto &c : root->children) WalkLatexNode(c.get(), ctx, out);
        return out;
    };
    LatexTagger &tg = ctx.tag;
    std::ostringstream out;
    LatexPreamble(out, true);
    out << LatexTaggingDocument(lang, title);
    // (Beamer's hyperref writes the PDF's title last: it is told the plain one.)
    if (!title.empty()) out << "\\hypersetup{pdftitle={" << LatexEscape(title) << "}}\n";
    // The title slide's lines, each set in a box of the title page: what
    // is read is the text itself, and the short form ([...]) is what
    // Beamer puts in the PDF's own title and author.
    std::string cover;
    const size_t cover_el = tg.BeginBlock(cover, "Sect");
    auto line = [&](const char *type, const std::string &text) {
        const std::string plain = LatexEscape(text);
        if (!tg.on) return "{" + plain + "}";
        std::string o;
        const size_t el = tg.Begin(o, type);
        tg.Boxed(o);
        o += plain;
        tg.End(o, el);
        return "[{" + plain + "}]{" + o + "}";
    };
    if (!title.empty()) out << "\\title" << line("Title", title) << "\n";
    if (!subtitle.empty()) out << "\\subtitle" << line("P", subtitle) << "\n";
    if (!author.empty()) out << "\\author" << line("P", author) << "\n";
    out << "\\date" << (date.empty() ? "{}" : line("P", date)) << "\n\\begin{document}\n";
    tg.End(cover, cover_el);
    if (!title.empty() || !subtitle.empty() || !author.empty()) out << cover << "\\begin{frame}\n\\titlepage\n\\end{frame}\n\n";
    for (const BeamerFrame &f : frames) {
        // fragile: a frame may hold verbatim code. The title is walked
        // like the body, so its inline markup and maths survive.
        // Each frame is a section of the deck, its title the heading
        // (set in the frame's own title box).
        std::string frame_el, t;
        const size_t el = tg.BeginBlock(frame_el, "Sect");
        const bool titled = !IsBlank(f.title_html);
        const size_t head = titled ? tg.Begin(t, "H1") : 0;
        ++tg.boxed;
        t += walk(f.title_html);
        --tg.boxed;
        if (titled) tg.End(t, head);
        while (!t.empty() && (t.back() == '\n' || t.back() == ' ')) t.pop_back();
        while (!t.empty() && (t.front() == '\n' || t.front() == ' ')) t.erase(t.begin());
        std::string body = walk(f.body_html);
        tg.End(body, el);
        out << frame_el << "\\begin{frame}[fragile]";
        if (!t.empty()) out << "{" << t << "}";
        out << "\n" << body << "\n\\end{frame}\n\n";
    }
    out << "\\end{document}\n";
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
/**
 * @brief Appends `text` to `parent` as one or more pcdata nodes, converting tabs to <text:tab/> and runs of 2+ spaces to ODF's <text:s> repeated-whitespace convention.
 * @param parent XML node the resulting pcdata/tab/space nodes are appended to.
 * @param text Raw text to append.
 */
void AppendTextRun(xml::xml_node parent, const std::string &text) {
    size_t i = 0, n = text.size();
    size_t seg_start = 0;
    /**
     * @brief Appends the pending pcdata segment [seg_start, end) to `parent` (a no-op if the segment is empty).
     * @param end Exclusive end offset into `text` of the segment to flush.
     */
    auto flush_pcdata = [&](size_t end) {
        if (end > seg_start) parent.append_child(xml::node_pcdata).set_value(text.substr(seg_start, end - seg_start).c_str());
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
                parent.append_child(xml::node_pcdata).set_value(" ");
                xml::xml_node s = parent.append_child("text:s");
                s.append_attribute("text:c").set_value(static_cast<unsigned int>(count - 1));
                seg_start = i;
            }
            continue;
        }
        i++;
    }
    flush_pcdata(n);
}

void AppendOdtImageFrame(const DomNode *node, OdtCtx &ctx, xml::xml_node container);

/**
 * @brief Recursively renders an inline-context DOM node (text, formatting, links, math, images) into ODT text runs appended to `container`.
 * @param node DOM node (element or text) to render.
 * @param ctx Shared ODT rendering context (base dir, image/table counters, collected images).
 * @param container XML node (an open <text:p>/<text:span>/<text:a>) the resulting run(s) are appended to.
 */
void WalkOdtInline(const DomNode *node, OdtCtx &ctx, xml::xml_node container) {
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
        xml::xml_node span = container.append_child("text:span");
        span.append_attribute("text:style-name").set_value("MepSrc");
        AppendTextRun(span, IsMathDisplay(node) ? ("[" + latex + "]") : ("$" + latex + "$"));
        return;
    }
    if (tag == "code" || tag == "tt") {
        xml::xml_node span = container.append_child("text:span");
        span.append_attribute("text:style-name").set_value("MepSrc");
        AppendTextRun(span, CollectRawText(node));
        return;
    }
    if (tag == "a") {
        auto it = node->attrs.find("href");
        xml::xml_node link = container.append_child("text:a");
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
        xml::xml_node span = container.append_child("text:span");
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

void WalkOdtBlock(const DomNode *node, OdtCtx &ctx, xml::xml_node text_body);

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
/**
 * @brief Decodes an <img>'s source file (to get its intrinsic aspect ratio) and appends a <draw:frame>/<draw:image> referencing it, registering the image's bytes in `ctx` for later zip packaging; falls back to a text placeholder if the file can't be read/decoded.
 * @param node The <img> DOM node to render.
 * @param ctx Shared ODT rendering context; the decoded image is appended to `ctx.images` and `ctx.image_counter` is advanced.
 * @param container XML node the resulting <draw:frame> (or placeholder text) is appended to.
 */
void AppendOdtImageFrame(const DomNode *node, OdtCtx &ctx, xml::xml_node container) {
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

    xml::xml_node frame = container.append_child("draw:frame");
    frame.append_attribute("draw:name").set_value(("MepImage" + std::to_string(ctx.image_counter)).c_str());
    frame.append_attribute("text:anchor-type").set_value("as-char");
    frame.append_attribute("svg:width").set_value(width_buf);
    frame.append_attribute("svg:height").set_value(height_buf);
    xml::xml_node img_el = frame.append_child("draw:image");
    img_el.append_attribute("xlink:href").set_value(zip_name.c_str());
    img_el.append_attribute("xlink:type").set_value("simple");
    img_el.append_attribute("xlink:show").set_value("embed");
    img_el.append_attribute("xlink:actuate").set_value("onLoad");
}

/**
 * @brief Renders an HTML <table> as a <table:table> (with one <table:table-column> per max-width column and one <table:table-row>/<table:table-cell> per source row/cell), appended to `text_body`.
 * @param node The <table> DOM node to render.
 * @param ctx Shared ODT rendering context; `ctx.table_counter` is advanced to name the table uniquely.
 * @param text_body XML node (the document's <office:text>) the resulting <table:table> is appended to.
 */
void AppendOdtTable(const DomNode *node, OdtCtx &ctx, xml::xml_node text_body) {
    std::vector<const DomNode *> rows;
    CollectTableRows(node, rows);
    size_t max_cols = TableMaxCols(rows);
    if (max_cols == 0) return;
    xml::xml_node table = text_body.append_child("table:table");
    table.append_attribute("table:name").set_value(("MepTable" + std::to_string(ctx.table_counter++)).c_str());
    for (size_t i = 0; i < max_cols; i++) table.append_child("table:table-column");
    for (const DomNode *r : rows) {
        xml::xml_node trow = table.append_child("table:table-row");
        for (const auto &c : r->children) {
            if (c->tag != "td" && c->tag != "th") continue;
            xml::xml_node cell = trow.append_child("table:table-cell");
            cell.append_attribute("office:value-type").set_value("string");
            xml::xml_node p = cell.append_child("text:p");
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
                           xml::xml_node text_body);

/**
 * @brief Recursively renders a block-context DOM node (heading, paragraph, list, table, blockquote, pre, image, or generic container) into ODT body content appended to `text_body`.
 * @param node DOM node (element or, as a tolerant fallback, text) to render.
 * @param ctx Shared ODT rendering context (base dir, image/table counters, collected images).
 * @param text_body XML node (the document's <office:text>, or an ancestor list item) the resulting content is appended to.
 */
void WalkOdtBlock(const DomNode *node, OdtCtx &ctx, xml::xml_node text_body) {
    if (node->type == DomNodeType::Text) {
        // Reached only when the CALLER didn't already route this
        // through WalkOdtBlockChildren (i.e. every real entry point
        // below) -- kept as a tolerant fallback rather than an assert,
        // same "degrade gracefully" spirit as the rest of this walker.
        bool blank = node->text.find_first_not_of(" \t\r\n") == std::string::npos;
        if (!blank) {
            xml::xml_node p = text_body.append_child("text:p");
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
        xml::xml_node h = text_body.append_child("text:h");
        h.append_attribute("text:outline-level").set_value(level);
        h.append_attribute("text:style-name").set_value(("MepHeading" + std::to_string(level)).c_str());
        for (auto &c : node->children) WalkOdtInline(c.get(), ctx, h);
        return;
    }
    if (tag == "p") {
        xml::xml_node p = text_body.append_child("text:p");
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
            xml::xml_node p = text_body.append_child("text:p");
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
                xml::xml_node p = text_body.append_child("text:p");
                p.append_attribute("text:style-name").set_value("MepQuote");
                for (auto &gc : c->children) WalkOdtInline(gc.get(), ctx, p);
            } else {
                WalkOdtBlock(c.get(), ctx, text_body);
            }
        }
        return;
    }
    if (tag == "ul" || tag == "ol") {
        xml::xml_node list = text_body.append_child("text:list");
        list.append_attribute("text:style-name").set_value(tag == "ul" ? "MepBulletList" : "MepNumberList");
        for (const auto &c : node->children) {
            if (c->tag != "li") continue;
            xml::xml_node item = list.append_child("text:list-item");
            xml::xml_node p = item.append_child("text:p");
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

/**
 * @brief Walks a block container's children as a sequence, dispatching each block-level sibling through WalkOdtBlock and coalescing runs of consecutive inline siblings (text and inline elements) into single <text:p> paragraphs.
 * @param children The child node list of a block container to walk.
 * @param ctx Shared ODT rendering context (base dir, image/table counters, collected images).
 * @param text_body XML node the resulting paragraphs/blocks are appended to.
 */
void WalkOdtBlockChildren(const std::vector<std::unique_ptr<DomNode>> &children, OdtCtx &ctx,
                           xml::xml_node text_body) {
    xml::xml_node open_p;  // empty/null until a run of inline content opens one
    for (auto &c : children) {
        bool is_block = c->type == DomNodeType::Element && c->style.block;
        if (is_block) {
            open_p = xml::xml_node();
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
/**
 * @brief Appends every named <style:style>/<text:list-style> content.xml's WalkOdt* functions reference (bold/italic/underline/strike/mono text styles, heading/preformatted/quote paragraph styles, an hr style, a table-heading style, and bullet/number list styles) to `auto_styles`.
 * @param auto_styles XML node (the document's <office:automatic-styles>) the style definitions are appended to.
 */
void AppendOdtAutomaticStyles(xml::xml_node auto_styles) {
    /**
     * @brief Appends one text:family <style:style> (bold/italic/underline/strike/mono flags) named `name` to `auto_styles`.
     * @param name Style name later referenced via text:style-name.
     * @param bold Whether to set fo:font-weight to bold.
     * @param italic Whether to set fo:font-style to italic.
     * @param underline Whether to set a solid text-underline-style.
     * @param strike Whether to set a solid text-line-through-style.
     * @param mono Whether to set the font to "mep Mono".
     */
    auto text_style = [&](const char *name, bool bold, bool italic, bool underline, bool strike, bool mono) {
        xml::xml_node s = auto_styles.append_child("style:style");
        s.append_attribute("style:name").set_value(name);
        s.append_attribute("style:family").set_value("text");
        xml::xml_node tp = s.append_child("style:text-properties");
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

    /**
     * @brief Appends one paragraph-family <style:style> (font size/weight/style/name plus top/bottom margins) named `name` to `auto_styles`.
     * @param name Style name later referenced via text:style-name.
     * @param font_pt Font size in points.
     * @param bold Whether to set fo:font-weight to bold.
     * @param margin_top Top margin, in inches.
     * @param margin_bottom Bottom margin, in inches.
     * @param mono Whether to set the font to "mep Mono".
     * @param italic Whether to set fo:font-style to italic.
     */
    auto para_style = [&](const std::string &name, double font_pt, bool bold, double margin_top,
                           double margin_bottom, bool mono, bool italic) {
        xml::xml_node s = auto_styles.append_child("style:style");
        s.append_attribute("style:name").set_value(name.c_str());
        s.append_attribute("style:family").set_value("paragraph");
        xml::xml_node pp = s.append_child("style:paragraph-properties");
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.3fin", margin_top);
        pp.append_attribute("fo:margin-top").set_value(buf);
        std::snprintf(buf, sizeof(buf), "%.3fin", margin_bottom);
        pp.append_attribute("fo:margin-bottom").set_value(buf);
        xml::xml_node tp = s.append_child("style:text-properties");
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
    xml::xml_node hr = auto_styles.append_child("style:style");
    hr.append_attribute("style:name").set_value("MepHr");
    hr.append_attribute("style:family").set_value("paragraph");
    xml::xml_node hr_pp = hr.append_child("style:paragraph-properties");
    hr_pp.append_attribute("style:border-line-width-bottom").set_value("0.0008in 0.0008in 0.0008in");
    hr_pp.append_attribute("fo:border-bottom").set_value("0.5pt solid #000000");
    hr_pp.append_attribute("fo:padding").set_value("0in");
    hr_pp.append_attribute("fo:margin-top").set_value("0.1in");
    hr_pp.append_attribute("fo:margin-bottom").set_value("0.1in");

    xml::xml_node th = auto_styles.append_child("style:style");
    th.append_attribute("style:name").set_value("MepTableHeading");
    th.append_attribute("style:family").set_value("paragraph");
    th.append_child("style:text-properties").append_attribute("fo:font-weight").set_value("bold");

    // List styles -- one level only (a nested <ul>/<ol> reuses the same
    // level-1 marker rather than a distinct per-depth one; a real, if
    // visually flat, documented v1 simplification).
    /**
     * @brief Appends one single-level bullet <text:list-style> named `name`, using `bullet_char` as the marker, to `auto_styles`.
     * @param name Style name later referenced via text:style-name on a <text:list>.
     * @param bullet_char UTF-8 bytes of the bullet glyph.
     */
    auto list_bullet_style = [&](const char *name, const char *bullet_char) {
        xml::xml_node ls = auto_styles.append_child("text:list-style");
        ls.append_attribute("style:name").set_value(name);
        xml::xml_node lvl = ls.append_child("text:list-level-style-bullet");
        lvl.append_attribute("text:level").set_value("1");
        lvl.append_attribute("text:bullet-char").set_value(bullet_char);
        xml::xml_node lp = lvl.append_child("style:list-level-properties");
        lp.append_attribute("text:list-level-position-and-space-mode").set_value("label-alignment");
        xml::xml_node la = lp.append_child("style:list-level-label-alignment");
        la.append_attribute("text:label-followed-by").set_value("listtab");
        la.append_attribute("text:list-tab-stop-position").set_value("0.5in");
        la.append_attribute("fo:text-indent").set_value("-0.25in");
        la.append_attribute("fo:margin-left").set_value("0.5in");
    };
    list_bullet_style("MepBulletList", "\xe2\x80\xa2");  // U+2022 BULLET, UTF-8 bytes (not a \u escape)

    xml::xml_node ls = auto_styles.append_child("text:list-style");
    ls.append_attribute("style:name").set_value("MepNumberList");
    xml::xml_node lvl = ls.append_child("text:list-level-style-number");
    lvl.append_attribute("text:level").set_value("1");
    lvl.append_attribute("style:num-format").set_value("1");
    lvl.append_attribute("style:num-suffix").set_value(".");
    lvl.append_attribute("text:display-levels").set_value("1");
    xml::xml_node lp = lvl.append_child("style:list-level-properties");
    lp.append_attribute("text:list-level-position-and-space-mode").set_value("label-alignment");
    xml::xml_node la = lp.append_child("style:list-level-label-alignment");
    la.append_attribute("text:label-followed-by").set_value("listtab");
    la.append_attribute("text:list-tab-stop-position").set_value("0.5in");
    la.append_attribute("fo:text-indent").set_value("-0.25in");
    la.append_attribute("fo:margin-left").set_value("0.5in");
}

/**
 * @brief Builds a minimal-but-valid styles.xml (default page layout only; per-document styles live in content.xml's own automatic styles).
 * @return The serialized styles.xml document.
 */
std::string BuildOdtStylesXml() {
    // A minimal-but-valid styles.xml -- content.xml's own automatic
    // styles (above) are fully self-contained, so this file only needs
    // to exist and be well-formed; it carries the document's default
    // page layout (letter-ish, 1in margins) via office:master-styles.
    xml::xml_document doc;
    xml::xml_node decl = doc.append_child(xml::node_declaration);
    decl.append_attribute("version").set_value("1.0");
    decl.append_attribute("encoding").set_value("UTF-8");
    xml::xml_node root = doc.append_child("office:document-styles");
    root.append_attribute("xmlns:office").set_value("urn:oasis:names:tc:opendocument:xmlns:office:1.0");
    root.append_attribute("xmlns:style").set_value("urn:oasis:names:tc:opendocument:xmlns:style:1.0");
    root.append_attribute("xmlns:fo").set_value("urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0");
    root.append_attribute("office:version").set_value("1.2");
    xml::xml_node styles = root.append_child("office:styles");
    xml::xml_node standard = styles.append_child("style:style");
    standard.append_attribute("style:name").set_value("Standard");
    standard.append_attribute("style:family").set_value("paragraph");
    standard.append_attribute("style:class").set_value("text");

    xml::xml_node master_styles = root.append_child("office:master-styles");
    xml::xml_node master = master_styles.append_child("style:master-page");
    master.append_attribute("style:name").set_value("Standard");
    master.append_attribute("style:page-layout-name").set_value("MepPageLayout");

    xml::xml_node auto_styles = root.append_child("office:automatic-styles");
    xml::xml_node layout = auto_styles.append_child("style:page-layout");
    layout.append_attribute("style:name").set_value("MepPageLayout");
    xml::xml_node lp = layout.append_child("style:page-layout-properties");
    lp.append_attribute("fo:margin-top").set_value("1in");
    lp.append_attribute("fo:margin-bottom").set_value("1in");
    lp.append_attribute("fo:margin-left").set_value("1in");
    lp.append_attribute("fo:margin-right").set_value("1in");

    std::ostringstream ss;
    doc.save(ss, "", xml::format_raw);
    return ss.str();
}

/**
 * @brief Builds the ODT package's meta.xml, recording title/author (when non-empty) and a fixed generator string.
 * @param title Document title; omitted from the output when empty.
 * @param author Document author; omitted from the output when empty.
 * @return The serialized meta.xml document.
 */
std::string BuildOdtMetaXml(const std::string &title, const std::string &author) {
    xml::xml_document doc;
    xml::xml_node decl = doc.append_child(xml::node_declaration);
    decl.append_attribute("version").set_value("1.0");
    decl.append_attribute("encoding").set_value("UTF-8");
    xml::xml_node root = doc.append_child("office:document-meta");
    root.append_attribute("xmlns:office").set_value("urn:oasis:names:tc:opendocument:xmlns:office:1.0");
    root.append_attribute("xmlns:dc").set_value("http://purl.org/dc/elements/1.1/");
    root.append_attribute("xmlns:meta").set_value("urn:oasis:names:tc:opendocument:xmlns:meta:1.0");
    root.append_attribute("office:version").set_value("1.2");
    xml::xml_node meta = root.append_child("office:meta");
    if (!title.empty()) meta.append_child("dc:title").append_child(xml::node_pcdata).set_value(title.c_str());
    if (!author.empty()) meta.append_child("dc:creator").append_child(xml::node_pcdata).set_value(author.c_str());
    meta.append_child("meta:generator").append_child(xml::node_pcdata).set_value("mep");
    std::ostringstream ss;
    doc.save(ss, "", xml::format_raw);
    return ss.str();
}

/**
 * @brief Builds the ODT package's META-INF/manifest.xml, listing the fixed content/styles/meta entries plus one entry per embedded image.
 * @param images Images collected during content rendering, each contributing one manifest entry.
 * @return The serialized manifest.xml document.
 */
std::string BuildOdtManifestXml(const std::vector<OdtImage> &images) {
    xml::xml_document doc;
    xml::xml_node decl = doc.append_child(xml::node_declaration);
    decl.append_attribute("version").set_value("1.0");
    decl.append_attribute("encoding").set_value("UTF-8");
    xml::xml_node root = doc.append_child("manifest:manifest");
    root.append_attribute("xmlns:manifest").set_value("urn:oasis:names:tc:opendocument:xmlns:manifest:1.0");
    root.append_attribute("manifest:version").set_value("1.2");
    /**
     * @brief Appends one <manifest:file-entry> for a zip archive path to `root`.
     * @param path The archive-relative full path of the entry.
     * @param media_type The entry's MIME media type.
     */
    auto entry = [&](const char *path, const char *media_type) {
        xml::xml_node e = root.append_child("manifest:file-entry");
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
    doc.save(ss, "", xml::format_raw);
    return ss.str();
}

}  // namespace

bool ExportHtmlToOdt(const std::string &html, const std::string &out_path, const std::string &title,
                     const std::string &author, const std::string &base_dir, std::string &error) {
    HtmlDoc doc;
    ParseHtml(html, doc);
    const DomNode *root = ContentRoot(doc.root.get());

    OdtCtx ctx;
    ctx.base_dir = base_dir;

    xml::xml_document content_doc;
    xml::xml_node decl = content_doc.append_child(xml::node_declaration);
    decl.append_attribute("version").set_value("1.0");
    decl.append_attribute("encoding").set_value("UTF-8");
    xml::xml_node content_root = content_doc.append_child("office:document-content");
    content_root.append_attribute("xmlns:office").set_value("urn:oasis:names:tc:opendocument:xmlns:office:1.0");
    content_root.append_attribute("xmlns:text").set_value("urn:oasis:names:tc:opendocument:xmlns:text:1.0");
    content_root.append_attribute("xmlns:table").set_value("urn:oasis:names:tc:opendocument:xmlns:table:1.0");
    content_root.append_attribute("xmlns:style").set_value("urn:oasis:names:tc:opendocument:xmlns:style:1.0");
    content_root.append_attribute("xmlns:fo").set_value("urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0");
    content_root.append_attribute("xmlns:draw").set_value("urn:oasis:names:tc:opendocument:xmlns:drawing:1.0");
    content_root.append_attribute("xmlns:svg").set_value("urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0");
    content_root.append_attribute("xmlns:xlink").set_value("http://www.w3.org/1999/xlink");
    content_root.append_attribute("office:version").set_value("1.2");

    xml::xml_node auto_styles = content_root.append_child("office:automatic-styles");
    AppendOdtAutomaticStyles(auto_styles);

    xml::xml_node body_el = content_root.append_child("office:body");
    xml::xml_node text_body = body_el.append_child("office:text");
    if (root) {
        WalkOdtBlockChildren(root->children, ctx, text_body);
    }

    std::ostringstream content_ss;
    content_doc.save(content_ss, "", xml::format_raw);
    std::string content_xml = content_ss.str();
    std::string styles_xml = BuildOdtStylesXml();
    std::string meta_xml = BuildOdtMetaXml(title, author);
    std::string manifest_xml = BuildOdtManifestXml(ctx.images);
    // "mimetype" must be the first entry and stored uncompressed -- real
    // ODF readers/validators rely on this for fast format sniffing
    // without inflating anything (see zip_archive.h's own header for the
    // real-world round-trip failure that shaped its writer's exact
    // local-header format).
    std::vector<zip::EntryToWrite> zip_entries = {
        {"mimetype", "application/vnd.oasis.opendocument.text", true},
        {"META-INF/manifest.xml", manifest_xml, false},
        {"content.xml", content_xml, false},
        {"styles.xml", styles_xml, false},
        {"meta.xml", meta_xml, false},
    };
    for (const OdtImage &img : ctx.images) zip_entries.push_back({img.zip_name, img.bytes, true});
    std::string archive = zip::BuildArchive(zip_entries);

    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
        error = "failed to write " + out_path;
        return false;
    }
    out.write(archive.data(), static_cast<std::streamsize>(archive.size()));
    return true;
}
