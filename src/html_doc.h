#ifndef MEP_HTML_DOC_H
#define MEP_HTML_DOC_H

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Deliberately raylib-free (same reasoning as office_doc.h/image_doc.h/
// pdf_doc.h): parsing and the DOM/computed-style model are pure CPU-side
// data structures, usable/testable without a GL context -- main.cpp is the
// only place that turns a laid-out node tree into word-wrapped, positioned
// text (see its own comment on why word-wrap specifically isn't baked in
// here, mirroring OfficeDoc's paragraphs-not-lines split).
//
// A hand-rolled, intentionally minimal HTML+CSS renderer -- not a spec-
// compliant browser engine. No flexbox/grid, no floats, no positioning
// (absolute/fixed/relative), no external stylesheets or scripts (<link
// rel=stylesheet>, <script src>), no forms/inputs, no tables, no cascade
// specificity beyond "tag < class/id selector < inline style, later rule
// wins a tie within the same bucket". Rendering is monospace throughout
// (mep has exactly one font atlas, g_font, reloaded at one size at a time
// -- see main.cpp's ApplyFontSize) at varying *sizes* (headings scale up
// the same way MenuFontSize() already draws g_font at a size other than
// g_font_size), not a real proportional face. Remote <img> is out of
// scope entirely (no network fetch for anything but the page itself, and
// that's the caller's job -- see mep.browse_command, kBuiltinTextTools);
// a local-file <img src> is supported. See js_engine.h for the paired
// (also intentionally tiny) JS interpreter this hands its DOM tree to.

enum class DomNodeType { Element, Text };

// Every property CSS could plausibly set, already resolved (inherited from
// the parent, then the UA default for this tag, then <style> block rules,
// then the inline style="" attribute, each layer only overriding what it
// actually specifies) -- ComputeStyles (below) is the only thing that
// produces one of these; nothing else in this file mutates `style` on a
// node after that pass. `display_none` aside, none of this says anything
// about *position* -- that's main.cpp's word-wrap layout pass, driven by
// `block` (own line) vs inline (flows with surrounding text) and the
// heading/list bookkeeping below.
struct ComputedStyle {
    bool display_none = false;
    bool block = true;  // false = inline (flows with surrounding text)
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strikethrough = false;
    bool monospace = false;  // <code>/<pre> -- purely cosmetic here (everything's monospace already), kept for a future proportional-font pass
    bool preserve_whitespace = false;  // <pre> -- layout skips word-wrap/whitespace-collapse for this node's own text
    bool has_color = false;
    unsigned char color_r = 0, color_g = 0, color_b = 0;
    bool has_bg = false;
    unsigned char bg_r = 0, bg_g = 0, bg_b = 0;
    // One edge of a CSS border (border-top/-right/-bottom/-left, or the
    // border shorthand applying the same value to all four) -- style
    // keywords (solid/dashed/...) are parsed but not distinguished, every
    // border always draws as a solid line (matches this file's own "not
    // spec-compliant" scope, html_doc.cpp's ApplyDeclarations). Borders
    // never inherit (real CSS doesn't either), so these default to absent
    // on every node the way ComputeStyles' WalkAndStyle already resets
    // most non-inherited fields.
    struct BorderEdge {
        bool present = false;
        float width_px = 1.0f;
        unsigned char r = 0, g = 0, b = 0;
    };
    BorderEdge border_top, border_right, border_bottom, border_left;
    // Multiplier on the pane's base font size -- 1.0 for body text, >1 for
    // headings (h1 largest), <1 never used in the UA defaults but a
    // <style> rule can still set one. Inherits from the parent (matching
    // real CSS -- a <span> with no font-size of its own renders at its
    // parent's computed size, not the root's) unless this tag has its own
    // distinctive UA default (TagDefaults, html_doc.cpp -- headings only),
    // or a CSS rule/inline style sets it directly; see WalkAndStyle's own
    // comment for the exact condition.
    float font_scale = 1.0f;
    // "max-width: Nem" + a horizontal "auto" margin (the "margin: ... auto"
    // or explicit margin-left/right:auto idiom) -- the standard way a real
    // page centers a readable column narrower than the viewport, most
    // commonly on <body> itself (this file's own kBuiltinOrgExport output
    // does exactly this). `max_width_em` is a multiple of *this element's
    // own* resolved font size (font_scale × the pane's base size), same
    // "resolve against ctx at layout time" convention font_scale itself
    // uses -- only main.cpp's layout pass can turn it into actual pixels.
    // Percentage/other units aren't supported (silently ignored, matching
    // this file's own tolerance convention); neither field inherits (real
    // CSS doesn't either).
    bool has_max_width = false;
    float max_width_em = 0.0f;
    bool margin_h_auto = false;
    // Block-level vertical spacing, in *lines* (not px -- main.cpp's
    // layout pass multiplies by whatever line height it's using for that
    // node's own font_scale), before/after this element's own content.
    int margin_top_lines = 0, margin_bottom_lines = 0;
    // List nesting depth (0 = not inside a list) -- indents <li> content
    // and picks its marker; a bare block element's own indent is always 0
    // (no general text-indent support).
    int list_depth = 0;
    bool is_list_item = false;
    bool ordered_list_item = false;  // marker is "N." vs a bullet
    int list_item_index = 0;         // 1-based position within its <ol>, for ordered markers
};

struct DomNode {
    DomNodeType type = DomNodeType::Element;
    std::string tag;   // lowercase; empty for Text nodes
    std::string text;  // Text node content verbatim (whitespace-collapsed at layout time, not here -- see main.cpp)
    std::unordered_map<std::string, std::string> attrs;
    std::vector<std::unique_ptr<DomNode>> children;
    // Non-owning; valid for the node's whole lifetime since nothing in
    // this file or js_engine.cpp ever reparents/moves a node after parse
    // (js_engine.cpp's textContent setter replaces a node's *children*,
    // never the node itself, so parent pointers into the surrounding tree
    // stay valid across a script mutation).
    DomNode *parent = nullptr;
    ComputedStyle style;

    // "id" and "class" attribute lookups (document.getElementById, a
    // <style> block's #id/.class selectors) are frequent enough relative
    // to the handful of attributes a typical element has that a plain
    // linear attrs.find() is already effectively O(1) in practice --
    // these two just spare every caller from spelling out the map lookup
    // and empty-string fallback themselves.
    const std::string &Id() const;
    const std::string &Class() const;
};

struct HtmlDoc {
    std::unique_ptr<DomNode> root;  // synthetic node wrapping the whole document; never null after a successful parse
    std::string title;              // <title> text, "" if absent -- kept denormalized (not re-walked from the tree) since js_engine.cpp's document.title can rewrite it directly

    // Every <script> element's own text content, in document order, *not*
    // yet executed -- js_engine.cpp runs these once, after the DOM finishes
    // parsing, in one shared global scope (real multi-<script> pages rely
    // on exactly that: a later block seeing an earlier one's globals).
    // External <script src> is out of scope entirely (never populated).
    std::vector<std::string> scripts;
};

// Parses `html` (already-decoded UTF-8 text, not raw bytes -- the caller
// owns any charset sniffing/decoding, none attempted here) into `out`.
// Tolerant like every other Load*FromMemory in this codebase (PdfDoc,
// LoadDocxFromMemory, ...): malformed markup degrades gracefully (an
// unclosed tag auto-closes at the nearest ancestor that matches, or at
// end-of-document; an unrecognized tag is still parsed as a generic
// inline container rather than rejected) instead of failing the whole
// parse. Always succeeds -- there's no ill-formed input this bails out on,
// only ones it does something reasonable-but-imperfect with, so unlike
// LoadDocxFromMemory there's no bool/error out-param.
void ParseHtml(const std::string &html, HtmlDoc &out);

// Walks `doc.root`, resolving every node's `style` per this file's own
// header comment (inherit from parent, then this tag's UA default, then
// any matching <style> block rule, then this node's own inline style=
// attribute). Called once by ParseHtml itself (so a fresh parse is
// immediately ready to lay out) and again by js_engine.cpp after a script
// finishes running, in case it touched anything the *inherited* properties
// depend on (textContent doesn't, since it never changes a node's tag/
// attrs/position -- but re-running this is cheap enough for the doc sizes
// this renderer targets that it's not worth tracking whether that
// happened).
void ComputeStyles(HtmlDoc &doc);

// True if `path`'s extension is .html or .htm (case-insensitive) -- same
// convention as IsDocxPath/IsImagePath/etc. (office_doc.h and friends).
bool IsHtmlPath(const std::string &path);

#endif
