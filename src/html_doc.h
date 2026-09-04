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

// A compact, renderer-independent display list for a canvas element.  The
// JavaScript binding records commands here; main.cpp replays them into the
// page's raylib render pass.  Keeping it on the DOM node means a canvas has
// browser-like persistent pixels across layout frames instead of disappearing
// whenever the preview is redrawn.
struct CanvasGradientStop {
    float offset = 0.0f;
    unsigned char r = 0, g = 0, b = 0, a = 255;
};

// A CanvasGradient's geometry in canvas coordinates. Commands copy the
// gradient active at record time, so a later addColorStop() can't retro-
// actively recolor pixels already "painted" -- the same snapshot semantics
// a real bitmap canvas has.
struct CanvasGradient {
    bool present = false;
    bool radial = false;
    float x0 = 0, y0 = 0, r0 = 0, x1 = 0, y1 = 0, r1 = 0;
    std::vector<CanvasGradientStop> stops;
};

struct CanvasCommand {
    enum class Kind { FillRect, StrokeRect, ClearRect, StrokePath, FillPath, FillText, ImageData };
    Kind kind = Kind::FillRect;
    float x = 0, y = 0, w = 0, h = 0;
    unsigned char r = 0, g = 0, b = 0, a = 255;  // flat paint (globalAlpha already applied)
    CanvasGradient gradient;                     // when present, overrides r/g/b (see CanvasGradientColorAt)
    float line_width = 1.0f;
    float font_size = 0.0f;  // FillText: canvas-space font size, 0 = renderer default
    // Path geometry, already transformed by the CTM current at record time.
    std::vector<float> points;
    // FillPath: index triples into `points`, wound for raylib's DrawTriangle
    // (see SvgShape::triangles in svg_doc.h for the convention).
    std::vector<unsigned> triangles;
    std::string text;
    std::vector<unsigned char> pixels;
};

// Evaluates `gradient` at a canvas-space point: linear gradients project
// onto the axis, radial ones use the distance from the inner circle's
// center normalized by the outer radius (concentric approximation). Stops
// are interpolated in straight RGBA. Returns false if there are no stops.
bool CanvasGradientColorAt(const CanvasGradient &gradient, float x, float y,
                           unsigned char &r, unsigned char &g, unsigned char &b, unsigned char &a);

// A CSS length kept unresolved until layout, where the containing block and
// effective font size are available. `auto_value` is meaningful for margins;
// other properties simply treat it as their normal automatic value.
struct CssLength {
    enum class Unit { Px, Percent, Em, Rem };
    bool set = false;
    bool auto_value = false;
    float value = 0.0f;
    Unit unit = Unit::Px;
};

struct CssEdges {
    CssLength top, right, bottom, left;
};

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
    // CSS box-model lengths. They intentionally retain their units here:
    // percentages need the containing block width and em needs this node's
    // resolved font size, neither of which the DOM/style pass knows.
    CssEdges margin, padding;
    CssLength width, height, min_width, min_height, max_width, max_height;
    bool border_box = false;  // false = content-box (the CSS default)
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
    // A host-owned shadow root.  It is intentionally separate from light DOM
    // children so DOM APIs retain both trees while layout renders the shadow
    // subtree when present.
    std::unique_ptr<DomNode> shadow_root;
    // Non-owning; valid for the node's whole lifetime since nothing in
    // this file or js_engine.cpp ever reparents/moves a node after parse
    // (js_engine.cpp's textContent setter replaces a node's *children*,
    // never the node itself, so parent pointers into the surrounding tree
    // stay valid across a script mutation).
    DomNode *parent = nullptr;
    ComputedStyle style;

    // Form/details state is DOM-owned rather than reconstructed by layout.
    // This makes the initial HTML attributes observable now and gives the
    // future input/event layer stable state to mutate.
    std::string form_value;
    bool form_checked = false;
    bool form_disabled = false;
    bool details_open = false;
    bool media_paused = true;
    bool media_muted = false;
    double media_current_time = 0.0;
    double media_volume = 1.0;
    // Filled by LoadHtmlMedia from the resolved local source: duration in
    // seconds and HTMLMediaElement.readyState (0 = HAVE_NOTHING, 4 =
    // HAVE_ENOUGH_DATA). `media_error` is non-empty when the source could
    // not be decoded (unsupported container/codec, missing file).
    double media_duration = 0.0;
    bool media_ended = false;
    int media_ready_state = 0;
    std::string media_error;
    std::string media_source_path;  // resolved local path, "" for remote/none

    // Canvas's coordinate space is separate from its CSS layout size.  These
    // are initialized from width/height attributes (or the HTML defaults)
    // and the command list is populated by CanvasRenderingContext2D.
    int canvas_width = 300;
    int canvas_height = 150;
    std::vector<CanvasCommand> canvas_commands;

    // "id" and "class" attribute lookups (document.getElementById, a
    // <style> block's #id/.class selectors) are frequent enough relative
    // to the handful of attributes a typical element has that a plain
    // linear attrs.find() is already effectively O(1) in practice --
    // these two just spare every caller from spelling out the map lookup
    // and empty-string fallback themselves.
    /**
     * @brief Returns this node's "id" attribute value, or an empty string if it has none.
     * @return The "id" attribute value, or a shared empty string if absent.
     */
    const std::string &Id() const;
    /**
     * @brief Returns this node's "class" attribute value, or an empty string if it has none.
     * @return The "class" attribute value, or a shared empty string if absent.
     */
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
    // Nodes made through document.createElement/createTextNode remain here
    // until insertion. This gives detached DOM wrappers stable ownership.
    std::vector<std::unique_ptr<DomNode>> detached_nodes;
};

// Renderer-independent accessibility projection.  The editor can later map
// this to its platform accessibility bridge without reinterpreting HTML or
// ARIA at every call site.
struct AccessibleNode {
    std::string role;
    std::string name;
    std::string description;
    bool disabled = false;
    bool checked = false;
    std::vector<AccessibleNode> children;
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
/**
 * @brief Parses HTML markup into a DOM tree, extracts math spans, and computes styles, always succeeding (tolerant of malformed markup).
 * @param html Already-decoded UTF-8 HTML text to parse.
 * @param out Destination document; its root/title/scripts are reset and repopulated.
 */
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
/**
 * @brief Resolves the computed `style` of every node in `doc.root` by cascading inheritance, tag defaults, `<style>` block rules, and inline `style=` attributes.
 * @param doc Document whose tree gets its `style` fields (re)computed in place.
 */
void ComputeStyles(HtmlDoc &doc);

// Selector helpers shared by the DOM binding. They use the same parser and
// matcher as the CSS cascade, preventing querySelector from drifting away
// from what a stylesheet actually matches.
DomNode *QuerySelector(DomNode *root, const std::string &selector);
std::vector<DomNode *> QuerySelectorAll(DomNode *root, const std::string &selector);

// Produces the document's semantic accessibility tree, respecting explicit
// ARIA roles/labels and native HTML control semantics.
AccessibleNode BuildAccessibilityTree(const HtmlDoc &doc);

// Resolves every <audio>/<video> element's local source (its own `src` or
// the first <source src>) against `base_dir`, decodes what the in-tree
// media pipeline supports (PCM16 RIFF/WAVE via wav_doc.cpp today) to learn
// its duration, and records readyState/error on the node. Remote sources
// are left unloaded (Part IV networking is where fetching would land).
// Called once per parse, before scripts run, so `duration` is observable.
void LoadHtmlMedia(HtmlDoc &doc, const std::string &base_dir);

// Advances every playing media element's clock by `seconds`; clamps at the
// duration, honoring `loop`, and flips paused/ended at the end. The editor
// calls this once per frame for the visible page.
void AdvanceHtmlMediaClock(HtmlDoc &doc, double seconds);

// True if `path`'s extension is .html or .htm (case-insensitive) -- same
// convention as IsDocxPath/IsImagePath/etc. (office_doc.h and friends).
/**
 * @brief Checks whether a path's extension is .html or .htm, case-insensitively.
 * @param path File path to check.
 * @return True if `path` ends in .html or .htm (any case).
 */
bool IsHtmlPath(const std::string &path);

#endif
